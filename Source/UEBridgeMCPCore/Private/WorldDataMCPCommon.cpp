#include "WorldDataMCPCommon.h"

#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace WorldDataMCP
{
	FString JsonObjectToString(const TSharedRef<FJsonObject>& Json, bool bPretty)
	{
		FString Out;
		if (bPretty)
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Json, Writer);
		}
		else
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Json, Writer);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonText)
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		return FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() ? Parsed : nullptr;
	}

	FString ErrorJson(const FString& Message)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Message);
		return JsonObjectToString(Result);
	}

	FString SuccessJson(const TSharedRef<FJsonObject>& Result)
	{
		Result->SetBoolField(TEXT("success"), true);
		return JsonObjectToString(Result);
	}

	FString GetProjectName()
	{
		const FString ProjectFile = FPaths::GetProjectFilePath();
		if (!ProjectFile.IsEmpty())
		{
			return FPaths::GetBaseFilename(ProjectFile);
		}

		FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		FPaths::NormalizeDirectoryName(ProjectDir);
		const FString Name = FPaths::GetCleanFilename(ProjectDir);
		return Name.IsEmpty() ? TEXT("UnrealProject") : Name;
	}
}
