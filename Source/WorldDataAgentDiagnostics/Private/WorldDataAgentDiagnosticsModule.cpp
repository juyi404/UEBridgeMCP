#include "IWorldDataAgentDiagnosticsModule.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 MaxDiagnosticEntries = 500;

	FString LevelToString(const EWorldDataAgentLogLevel Level)
	{
		switch (Level)
		{
		case EWorldDataAgentLogLevel::Debug: return TEXT("debug");
		case EWorldDataAgentLogLevel::Warning: return TEXT("warning");
		case EWorldDataAgentLogLevel::Error: return TEXT("error");
		default: return TEXT("info");
		}
	}

	void RedactDelimitedValue(FString& Text, const FString& Key)
	{
		int32 SearchFrom = 0;
		FString Lower = Text.ToLower();
		const FString LowerKey = Key.ToLower();
		while (true)
		{
			const int32 KeyIndex = Lower.Find(LowerKey, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (KeyIndex == INDEX_NONE) return;
			int32 ValueStart = KeyIndex + Key.Len();
			while (ValueStart < Text.Len() && (FChar::IsWhitespace(Text[ValueStart]) || Text[ValueStart] == TEXT(':') || Text[ValueStart] == TEXT('='))) ++ValueStart;
			const TCHAR Quote = ValueStart < Text.Len() && (Text[ValueStart] == TEXT('"') || Text[ValueStart] == TEXT('\'')) ? Text[ValueStart++] : 0;
			int32 ValueEnd = ValueStart;
			while (ValueEnd < Text.Len() && (Quote ? Text[ValueEnd] != Quote : !FChar::IsWhitespace(Text[ValueEnd]) && Text[ValueEnd] != TEXT(',') && Text[ValueEnd] != TEXT('}'))) ++ValueEnd;
			if (ValueEnd > ValueStart)
			{
				Text = Text.Left(ValueStart) + TEXT("[REDACTED]") + Text.Mid(ValueEnd);
				Lower = Text.ToLower();
				SearchFrom = ValueStart + 10;
			}
			else
			{
				SearchFrom = ValueStart + 1;
			}
		}
	}

	FString Redact(FString Text)
	{
		for (const TCHAR* Key : { TEXT("authorization"), TEXT("bearer"), TEXT("access_token"), TEXT("api_key"), TEXT("token"), TEXT("secret") })
		{
			RedactDelimitedValue(Text, Key);
		}
		return Text.Left(8192);
	}

	class FWorldDataAgentDiagnostics final : public IWorldDataAgentDiagnostics
	{
	public:
		virtual void Record(const FWorldDataAgentDiagnosticEntry& Entry) override
		{
			FWorldDataAgentDiagnosticEntry Sanitized = Entry;
			Sanitized.TimestampUtc = Sanitized.TimestampUtc == FDateTime() ? FDateTime::UtcNow() : Sanitized.TimestampUtc;
			Sanitized.Component = Redact(Sanitized.Component);
			Sanitized.Code = Redact(Sanitized.Code);
			Sanitized.Message = Redact(Sanitized.Message);
			FScopeLock Lock(&Mutex);
			Entries.Add(MoveTemp(Sanitized));
			if (Entries.Num() > MaxDiagnosticEntries)
			{
				Entries.RemoveAt(0, Entries.Num() - MaxDiagnosticEntries, EAllowShrinking::No);
			}
		}

		virtual TArray<FWorldDataAgentDiagnosticEntry> Snapshot() const override
		{
			FScopeLock Lock(&Mutex);
			return Entries;
		}

		virtual bool ExportRedacted(FString& OutAbsolutePath, FString& OutError) const override
		{
			OutAbsolutePath.Empty();
			OutError.Empty();
			const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("Diagnostics")));
			if (!IFileManager::Get().MakeDirectory(*Directory, true))
			{
				OutError = TEXT("Could not create the diagnostics directory.");
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> JsonEntries;
			for (const FWorldDataAgentDiagnosticEntry& Entry : Snapshot())
			{
				TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("timestampUtc"), Entry.TimestampUtc.ToIso8601());
				Object->SetStringField(TEXT("level"), LevelToString(Entry.Level));
				Object->SetStringField(TEXT("component"), Entry.Component);
				Object->SetStringField(TEXT("code"), Entry.Code);
				Object->SetStringField(TEXT("message"), Entry.Message);
				JsonEntries.Add(MakeShared<FJsonValueObject>(Object));
			}
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetNumberField(TEXT("formatVersion"), 1);
			Root->SetArrayField(TEXT("entries"), JsonEntries);
			FString Json;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			if (!FJsonSerializer::Serialize(Root, Writer))
			{
				OutError = TEXT("Could not serialize diagnostics.");
				return false;
			}
			OutAbsolutePath = FPaths::Combine(Directory, FString::Printf(TEXT("agent-diagnostics-%s.json"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d-%H%M%S"))));
			if (!FFileHelper::SaveStringToFile(Json, *OutAbsolutePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = TEXT("Could not write diagnostics.");
				OutAbsolutePath.Empty();
				return false;
			}
			return true;
		}

		virtual void Clear() override
		{
			FScopeLock Lock(&Mutex);
			Entries.Empty();
		}

	private:
		mutable FCriticalSection Mutex;
		TArray<FWorldDataAgentDiagnosticEntry> Entries;
	};
}

class FWorldDataAgentDiagnosticsModule final : public IWorldDataAgentDiagnosticsModule
{
public:
	virtual IWorldDataAgentDiagnostics& GetDiagnostics() override { return Diagnostics; }

private:
	FWorldDataAgentDiagnostics Diagnostics;
};

IMPLEMENT_MODULE(FWorldDataAgentDiagnosticsModule, WorldDataAgentDiagnostics)
