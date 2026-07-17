#include "WorldDataMCPResponseContext.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "Misc/Crc.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "WorldDataMCPCommon.h"

namespace WorldDataMCP::ResponseContext
{
	namespace
	{
		constexpr int32 GDefaultResponseMaxBytes = 64 * 1024;
		constexpr int32 GMinimumResponseMaxBytes = 4 * 1024;
		constexpr int32 GMaximumResponseMaxBytes = 256 * 1024;
		constexpr int32 GDefaultResponsePageSize = 50;
		constexpr int32 GMaximumResponsePageSize = 500;
		constexpr int32 GMaximumResponseSnapshots = 32;
		constexpr int32 GMaximumSnapshotBytes = 2 * 1024 * 1024;
		constexpr int32 GSnapshotLifetimeMinutes = 10;
		constexpr TCHAR GResponseMetaField[] = TEXT("responseMeta");

		struct FResponseSnapshot
		{
			FString ToolName;
			FString ResultJson;
			FDateTime ExpiresAtUtc;
		};

		FCriticalSection GResponseSnapshotMutex;
		TMap<FString, FResponseSnapshot> GResponseSnapshots;

		TSharedPtr<FJsonObject> ParseObject(const FString& JsonText)
		{
			TSharedPtr<FJsonObject> Parsed;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			return FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() ? Parsed : nullptr;
		}

		FString SerializeArray(const TArray<TSharedPtr<FJsonValue>>& Values)
		{
			FString Out;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Values, Writer);
			return Out;
		}

		int32 GetUtf8ByteCount(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			return Utf8.Length();
		}

		FString MakeRevision(const FString& ResultJson)
		{
			const FTCHARToUTF8 Utf8(*ResultJson);
			const uint32 Crc = FCrc::MemCrc32(Utf8.Get(), Utf8.Length());
			return FString::Printf(TEXT("crc32-%08x-%d"), Crc, Utf8.Length());
		}

		bool IsValidTopLevelField(const FString& Field)
		{
			if (Field.IsEmpty() || Field.Len() > 128)
			{
				return false;
			}
			for (const TCHAR Character : Field)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
				{
					return false;
				}
			}
			return true;
		}

		bool ParseCursor(const FString& Cursor, FString& OutSnapshotId, int32& OutOffset)
		{
			int32 SeparatorIndex = INDEX_NONE;
			if (!Cursor.FindChar(TEXT(':'), SeparatorIndex) || SeparatorIndex <= 0 || SeparatorIndex >= Cursor.Len() - 1)
			{
				return false;
			}

			const FString OffsetText = Cursor.Mid(SeparatorIndex + 1);
			for (const TCHAR Character : OffsetText)
			{
				if (!FChar::IsDigit(Character))
				{
					return false;
				}
			}

			OutSnapshotId = Cursor.Left(SeparatorIndex);
			OutOffset = FCString::Atoi(*OffsetText);
			return OutOffset >= 0;
		}

		void PruneExpiredSnapshots_Unsafe()
		{
			const FDateTime Now = FDateTime::UtcNow();
			for (auto It = GResponseSnapshots.CreateIterator(); It; ++It)
			{
				if (It.Value().ExpiresAtUtc <= Now)
				{
					It.RemoveCurrent();
				}
			}
		}

		bool StoreSnapshot(const FString& ToolName, const FString& ResultJson, FString& OutSnapshotId)
		{
			if (GetUtf8ByteCount(ResultJson) > GMaximumSnapshotBytes)
			{
				return false;
			}

			FScopeLock Lock(&GResponseSnapshotMutex);
			PruneExpiredSnapshots_Unsafe();
			while (GResponseSnapshots.Num() >= GMaximumResponseSnapshots)
			{
				FString OldestId;
				FDateTime OldestExpiry = FDateTime::MaxValue();
				for (const TPair<FString, FResponseSnapshot>& Pair : GResponseSnapshots)
				{
					if (Pair.Value.ExpiresAtUtc < OldestExpiry)
					{
						OldestExpiry = Pair.Value.ExpiresAtUtc;
						OldestId = Pair.Key;
					}
				}
				if (OldestId.IsEmpty())
				{
					break;
				}
				GResponseSnapshots.Remove(OldestId);
			}

			OutSnapshotId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
			FResponseSnapshot& Snapshot = GResponseSnapshots.Add(OutSnapshotId);
			Snapshot.ToolName = ToolName;
			Snapshot.ResultJson = ResultJson;
			Snapshot.ExpiresAtUtc = FDateTime::UtcNow() + FTimespan::FromMinutes(GSnapshotLifetimeMinutes);
			return true;
		}

		void ApplyFieldProjection(const TSharedRef<FJsonObject>& Object, const TArray<FString>& Fields)
		{
			if (Fields.IsEmpty())
			{
				return;
			}

			TSharedRef<FJsonObject> Projected = MakeShared<FJsonObject>();
			auto CopyField = [&Object, &Projected](const FString& FieldName)
			{
				const TSharedPtr<FJsonValue> Value = Object->TryGetField(FieldName);
				if (Value.IsValid())
				{
					Projected->SetField(FieldName, Value);
				}
			};

			// Preserve the existing success/error contract even when callers choose a
			// narrow projection. The metadata is attached after the projection.
			CopyField(TEXT("success"));
			CopyField(TEXT("error"));
			for (const FString& Field : Fields)
			{
				CopyField(Field);
			}
			Object->Values = MoveTemp(Projected->Values);
		}

		FString FindPrimaryArrayField(const TSharedRef<FJsonObject>& Object)
		{
			static const TCHAR* const PreferredFields[] = {
				TEXT("actors"), TEXT("assets"), TEXT("components"), TEXT("results"),
				TEXT("rows"), TEXT("files"), TEXT("plugins"), TEXT("items"), TEXT("entries")
			};

			for (const TCHAR* Field : PreferredFields)
			{
				const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
				if (Object->TryGetArrayField(Field, Array) && Array && Array->Num() > 0)
				{
					return Field;
				}
			}

			FString LargestField;
			int32 LargestCount = 0;
			for (const auto& Pair : Object->Values)
			{
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Array)
				{
					continue;
				}
				const FString FieldName(*Pair.Key);
				const int32 Count = Pair.Value->AsArray().Num();
				if (Count > LargestCount)
				{
					LargestCount = Count;
					LargestField = FieldName;
				}
			}
			return LargestField;
		}

		TSharedRef<FJsonObject> MakeResponseMeta(
			const FString& Revision,
			const FResponseControl& Control,
			bool bNotModified,
			bool bTruncated,
			bool bSourceTruncated,
			bool bCursorUnavailable,
			const FString& NextCursor,
			const FString& ArrayField,
			int32 Offset,
			int32 Returned,
			int32 Total)
		{
			TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
			Meta->SetStringField(TEXT("schemaVersion"), TEXT("1.0"));
			Meta->SetStringField(TEXT("revision"), Revision);
			Meta->SetBoolField(TEXT("notModified"), bNotModified);
			Meta->SetBoolField(TEXT("truncated"), bTruncated);
			Meta->SetBoolField(TEXT("sourceTruncated"), bSourceTruncated);
			Meta->SetNumberField(TEXT("maxBytes"), Control.MaxBytes);
			Meta->SetNumberField(TEXT("byteCount"), 0);
			Meta->SetNumberField(TEXT("estimatedTokens"), 0);
			if (!NextCursor.IsEmpty())
			{
				Meta->SetStringField(TEXT("nextCursor"), NextCursor);
			}
			if (bCursorUnavailable)
			{
				Meta->SetBoolField(TEXT("cursorUnavailable"), true);
			}
			if (!ArrayField.IsEmpty())
			{
				TSharedRef<FJsonObject> Page = MakeShared<FJsonObject>();
				Page->SetStringField(TEXT("field"), ArrayField);
				Page->SetNumberField(TEXT("offset"), Offset);
				Page->SetNumberField(TEXT("returned"), Returned);
				Page->SetNumberField(TEXT("total"), Total);
				Meta->SetObjectField(TEXT("page"), Page);
			}
			if (!Control.Fields.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> FieldValues;
				for (const FString& Field : Control.Fields)
				{
					FieldValues.Add(MakeShared<FJsonValueString>(Field));
				}
				Meta->SetArrayField(TEXT("fields"), FieldValues);
			}
			return Meta;
		}

		FString SerializeWithMeta(const TSharedRef<FJsonObject>& Object, const TSharedRef<FJsonObject>& Meta)
		{
			Object->SetObjectField(GResponseMetaField, Meta);
			FString Serialized;
			for (int32 Attempt = 0; Attempt < 3; ++Attempt)
			{
				Serialized = JsonObjectToString(Object);
				const int32 ByteCount = GetUtf8ByteCount(Serialized);
				Meta->SetNumberField(TEXT("byteCount"), ByteCount);
				Meta->SetNumberField(TEXT("estimatedTokens"), FMath::DivideAndRoundUp(ByteCount, 4));
			}
			return Serialized;
		}

		FString MakeOversizeError(
			const TSharedRef<FJsonObject>& FullResult,
			const FString& Revision,
			const FResponseControl& Control,
			const FString& ArrayField,
			int32 Offset,
			int32 Total,
			bool bSourceTruncated)
		{
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetBoolField(TEXT("success"), false);
			Error->SetStringField(TEXT("error"), FString::Printf(
				TEXT("Tool result exceeds responseControl.maxBytes (%d bytes). Request fewer top-level fields, a smaller pageSize, or a narrower tool query."),
				Control.MaxBytes));

			TArray<TSharedPtr<FJsonValue>> AvailableFields;
			for (const auto& Pair : FullResult->Values)
			{
				const FString FieldName(*Pair.Key);
				if (FieldName != GResponseMetaField)
				{
					AvailableFields.Add(MakeShared<FJsonValueString>(FieldName));
				}
			}
			Error->SetArrayField(TEXT("availableFields"), AvailableFields);

			TSharedRef<FJsonObject> Meta = MakeResponseMeta(
				Revision, Control, false, true, bSourceTruncated, false, TEXT(""), ArrayField, Offset, 0, Total);
			Meta->SetBoolField(TEXT("requiresNarrowerQuery"), true);
			return SerializeWithMeta(Error, Meta);
		}
	}

	bool ExtractResponseControl(const TSharedPtr<FJsonObject>& Arguments, FResponseControl& OutControl, FString& OutError)
	{
		OutControl = FResponseControl();
		OutControl.MaxBytes = GDefaultResponseMaxBytes;
		OutControl.PageSize = GDefaultResponsePageSize;
		OutError.Reset();
		if (!Arguments.IsValid())
		{
			return true;
		}
		if (!Arguments->HasField(TEXT("responseControl")))
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* ControlObject = nullptr;
		if (!Arguments->TryGetObjectField(TEXT("responseControl"), ControlObject) || !ControlObject || !ControlObject->IsValid())
		{
			OutError = TEXT("responseControl must be an object when provided.");
			return false;
		}

		const TSharedPtr<FJsonObject> ControlJson = *ControlObject;
		double Number = 0.0;
		if (ControlJson->TryGetNumberField(TEXT("maxBytes"), Number))
		{
			OutControl.MaxBytes = FMath::Clamp(static_cast<int32>(Number), GMinimumResponseMaxBytes, GMaximumResponseMaxBytes);
		}
		if (ControlJson->TryGetNumberField(TEXT("pageSize"), Number))
		{
			OutControl.PageSize = FMath::Clamp(static_cast<int32>(Number), 1, GMaximumResponsePageSize);
		}
		ControlJson->TryGetStringField(TEXT("cursor"), OutControl.Cursor);
		ControlJson->TryGetStringField(TEXT("ifRevision"), OutControl.IfRevision);

		const TArray<TSharedPtr<FJsonValue>>* FieldValues = nullptr;
		if (ControlJson->TryGetArrayField(TEXT("fields"), FieldValues) && FieldValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *FieldValues)
			{
				if (!Value.IsValid() || Value->Type != EJson::String)
				{
					continue;
				}
				const FString Field = Value->AsString();
				if (!IsValidTopLevelField(Field))
				{
					OutError = TEXT("responseControl.fields may only contain top-level alphanumeric, '_' or '-' field names.");
					return false;
				}
				OutControl.Fields.AddUnique(Field);
				if (OutControl.Fields.Num() > 32)
				{
					OutError = TEXT("responseControl.fields supports at most 32 top-level fields.");
					return false;
				}
			}
		}

		Arguments->RemoveField(TEXT("responseControl"));
		return true;
	}

	bool TryResolveCursor(const FString& ToolName, const FString& Cursor, FString& OutResultJson, FString& OutError)
	{
		OutResultJson.Reset();
		OutError.Reset();
		FString SnapshotId;
		int32 IgnoredOffset = 0;
		if (!ParseCursor(Cursor, SnapshotId, IgnoredOffset))
		{
			OutError = TEXT("Invalid responseControl.cursor. Start a new tool call to obtain a fresh cursor.");
			return false;
		}

		FScopeLock Lock(&GResponseSnapshotMutex);
		PruneExpiredSnapshots_Unsafe();
		const FResponseSnapshot* Snapshot = GResponseSnapshots.Find(SnapshotId);
		if (!Snapshot || Snapshot->ToolName != ToolName)
		{
			OutError = TEXT("The response cursor is unknown, expired, or belongs to a different tool. Start the query again.");
			return false;
		}
		OutResultJson = Snapshot->ResultJson;
		return true;
	}

	FString AddResponseControlToToolDefinitions(const FString& DefinitionsJson)
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DefinitionsJson);
		if (!FJsonSerializer::Deserialize(Reader, Tools))
		{
			return DefinitionsJson;
		}

		for (const TSharedPtr<FJsonValue>& ToolValue : Tools)
		{
			const TSharedPtr<FJsonObject> Tool = ToolValue.IsValid() ? ToolValue->AsObject() : nullptr;
			if (!Tool.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* InputSchemaPtr = nullptr;
			if (!Tool->TryGetObjectField(TEXT("inputSchema"), InputSchemaPtr) || !InputSchemaPtr || !InputSchemaPtr->IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject> InputSchema = *InputSchemaPtr;
			const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
			TSharedPtr<FJsonObject> Properties;
			if (InputSchema->TryGetObjectField(TEXT("properties"), PropertiesPtr) && PropertiesPtr && PropertiesPtr->IsValid())
			{
				Properties = *PropertiesPtr;
			}
			else
			{
				Properties = MakeShared<FJsonObject>();
				InputSchema->SetObjectField(TEXT("properties"), Properties.ToSharedRef());
			}

			TSharedRef<FJsonObject> ControlSchema = MakeShared<FJsonObject>();
			ControlSchema->SetStringField(TEXT("type"), TEXT("object"));
			ControlSchema->SetStringField(TEXT("description"), TEXT("Uniform output controls; see server instructions."));
			TSharedRef<FJsonObject> ControlProperties = MakeShared<FJsonObject>();
			auto AddNumber = [&ControlProperties](const TCHAR* Name, int32 Minimum, int32 Maximum)
			{
				TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("type"), TEXT("integer"));
				Field->SetNumberField(TEXT("minimum"), Minimum);
				Field->SetNumberField(TEXT("maximum"), Maximum);
				ControlProperties->SetObjectField(Name, Field);
			};
			AddNumber(TEXT("maxBytes"), GMinimumResponseMaxBytes, GMaximumResponseMaxBytes);
			AddNumber(TEXT("pageSize"), 1, GMaximumResponsePageSize);
			for (const TCHAR* FieldName : { TEXT("cursor"), TEXT("ifRevision") })
			{
				TSharedRef<FJsonObject> Field = MakeShared<FJsonObject>();
				Field->SetStringField(TEXT("type"), TEXT("string"));
				ControlProperties->SetObjectField(FieldName, Field);
			}
			TSharedRef<FJsonObject> FieldsSchema = MakeShared<FJsonObject>();
			FieldsSchema->SetStringField(TEXT("type"), TEXT("array"));
			TSharedRef<FJsonObject> FieldItem = MakeShared<FJsonObject>();
			FieldItem->SetStringField(TEXT("type"), TEXT("string"));
			FieldsSchema->SetObjectField(TEXT("items"), FieldItem);
			FieldsSchema->SetNumberField(TEXT("maxItems"), 32);
			ControlProperties->SetObjectField(TEXT("fields"), FieldsSchema);
			ControlSchema->SetObjectField(TEXT("properties"), ControlProperties);
			Properties->SetObjectField(TEXT("responseControl"), ControlSchema);
		}

		return SerializeArray(Tools);
	}

	FString ShapeToolResult(const FString& ToolName, const FString& ResultJson, const FResponseControl& Control, bool bReadOnlyTool)
	{
		TSharedPtr<FJsonObject> FullResult = ParseObject(ResultJson);
		if (!FullResult.IsValid())
		{
			return ResultJson;
		}

		const FString Revision = MakeRevision(ResultJson);
		bool bSuccess = true;
		FullResult->TryGetBoolField(TEXT("success"), bSuccess);
		if (bReadOnlyTool && Control.Cursor.IsEmpty() && bSuccess && !Control.IfRevision.IsEmpty() && Control.IfRevision == Revision)
		{
			TSharedRef<FJsonObject> NotModified = MakeShared<FJsonObject>();
			NotModified->SetBoolField(TEXT("success"), true);
			TSharedRef<FJsonObject> Meta = MakeResponseMeta(Revision, Control, true, false, false, false, TEXT(""), TEXT(""), 0, 0, 0);
			return SerializeWithMeta(NotModified, Meta);
		}

		FString SnapshotId;
		int32 Offset = 0;
		if (!Control.Cursor.IsEmpty())
		{
			if (!ParseCursor(Control.Cursor, SnapshotId, Offset))
			{
				return ErrorJson(TEXT("Invalid responseControl.cursor. Start a new tool call to obtain a fresh cursor."));
			}
		}

		TSharedPtr<FJsonObject> ProjectedProbe = ParseObject(ResultJson);
		if (!ProjectedProbe.IsValid())
		{
			return ResultJson;
		}
		ApplyFieldProjection(ProjectedProbe.ToSharedRef(), Control.Fields);
		const FString ArrayField = FindPrimaryArrayField(ProjectedProbe.ToSharedRef());
		const TArray<TSharedPtr<FJsonValue>>* FullArray = nullptr;
		if (!ArrayField.IsEmpty())
		{
			ProjectedProbe->TryGetArrayField(ArrayField, FullArray);
		}
		const int32 Total = FullArray ? FullArray->Num() : 0;
		if (Offset > Total)
		{
			return ErrorJson(TEXT("The responseControl.cursor offset is outside the cached result. Start the query again."));
		}

		bool bSourceTruncated = false;
		FullResult->TryGetBoolField(TEXT("truncated"), bSourceTruncated);
		if (Control.Cursor.IsEmpty() && !ArrayField.IsEmpty())
		{
			StoreSnapshot(ToolName, ResultJson, SnapshotId);
		}

		int32 Returned = ArrayField.IsEmpty() ? 0 : FMath::Min(Control.PageSize, Total - Offset);
		for (;;)
		{
			TSharedPtr<FJsonObject> Working = ParseObject(ResultJson);
			if (!Working.IsValid())
			{
				return ResultJson;
			}
			ApplyFieldProjection(Working.ToSharedRef(), Control.Fields);

			bool bHasMorePages = false;
			if (!ArrayField.IsEmpty())
			{
				const TArray<TSharedPtr<FJsonValue>>* SourceArray = nullptr;
				Working->TryGetArrayField(ArrayField, SourceArray);
				TArray<TSharedPtr<FJsonValue>> Page;
				if (SourceArray)
				{
					const int32 Available = SourceArray->Num() - Offset;
					for (int32 Index = 0; Index < FMath::Min(Returned, Available); ++Index)
					{
						Page.Add((*SourceArray)[Offset + Index]);
					}
				}
				Working->SetArrayField(ArrayField, Page);
				bHasMorePages = Offset + Returned < Total;
				if (Working->HasField(TEXT("count")))
				{
					Working->SetNumberField(TEXT("count"), Returned);
				}
				if (Working->HasField(TEXT("truncated")) || bHasMorePages)
				{
					Working->SetBoolField(TEXT("truncated"), bSourceTruncated || bHasMorePages);
				}
			}

			const FString NextCursor = bHasMorePages && !SnapshotId.IsEmpty()
				? FString::Printf(TEXT("%s:%d"), *SnapshotId, Offset + Returned)
				: TEXT("");
			const bool bResponseTruncated = bSourceTruncated || bHasMorePages;
			TSharedRef<FJsonObject> Meta = MakeResponseMeta(
				Revision, Control, false, bResponseTruncated, bSourceTruncated,
				bHasMorePages && SnapshotId.IsEmpty(), NextCursor, ArrayField, Offset, Returned, Total);
			const FString Shaped = SerializeWithMeta(Working.ToSharedRef(), Meta);
			if (GetUtf8ByteCount(Shaped) <= Control.MaxBytes)
			{
				return Shaped;
			}

			if (!ArrayField.IsEmpty() && Returned > 0)
			{
				--Returned;
				continue;
			}

			return MakeOversizeError(FullResult.ToSharedRef(), Revision, Control, ArrayField, Offset, Total, bSourceTruncated);
		}
	}

	void ResetSnapshots()
	{
		FScopeLock Lock(&GResponseSnapshotMutex);
		GResponseSnapshots.Reset();
	}
}
