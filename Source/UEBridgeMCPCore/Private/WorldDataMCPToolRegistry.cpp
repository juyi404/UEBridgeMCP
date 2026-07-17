#include "WorldDataMCPToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeRWLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace WorldDataMCP
{
	FToolRegistry& FToolRegistry::Get()
	{
		static FToolRegistry Registry;
		return Registry;
	}

	void FToolRegistry::RegisterTool(const FString& Name, FToolHandler Handler)
	{
		FWriteScopeLock WriteLock(Lock);
		Handlers.Add(Name, MoveTemp(Handler));
	}

	void FToolRegistry::RegisterDefinitionSet(const FString& DefinitionsJson)
	{
		FWriteScopeLock WriteLock(Lock);
		DefinitionSets.Add(DefinitionsJson);
	}

	bool FToolRegistry::Dispatch(const FString& Name, const TSharedPtr<FJsonObject>& Arguments, FString& OutResult) const
	{
		FReadScopeLock ReadLock(Lock);
		const FToolHandler* Handler = Handlers.Find(Name);
		if (!Handler || !static_cast<bool>(*Handler))
		{
			return false;
		}

		OutResult = (*Handler)(Arguments);
		return true;
	}

	FString FToolRegistry::GetRegisteredDefinitionsJson() const
	{
		FReadScopeLock ReadLock(Lock);
		FString Result;
		for (const FString& Definitions : DefinitionSets)
		{
			if (!Result.IsEmpty())
			{
				Result += TEXT("\n");
			}
			Result += Definitions;
		}
		return Result;
	}

	void FToolRegistry::Reset()
	{
		FWriteScopeLock WriteLock(Lock);
		Handlers.Reset();
		DefinitionSets.Reset();
	}

	FResourceRegistry& FResourceRegistry::Get()
	{
		static FResourceRegistry Registry;
		return Registry;
	}

	void FResourceRegistry::RegisterResource(const FString& Uri, const FString& Name, const FString& Description, FResourceHandler Handler)
	{
		FWriteScopeLock WriteLock(Lock);
		Resources.Add(Uri, FResource{ Name, Description, MoveTemp(Handler) });
	}

	FString FResourceRegistry::GetResourceListJson(const FString& RecommendedFirstRead) const
	{
		TArray<FString> Uris;
		{
			FReadScopeLock ReadLock(Lock);
			Resources.GetKeys(Uris);
		}
		Uris.Sort();

		TArray<TSharedPtr<FJsonValue>> Values;
		FReadScopeLock ReadLock(Lock);
		for (const FString& Uri : Uris)
		{
			const FResource* Resource = Resources.Find(Uri);
			if (!Resource)
			{
				continue;
			}
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("uri"), Uri);
			Object->SetStringField(TEXT("name"), Resource->Name);
			Object->SetStringField(TEXT("description"), Resource->Description);
			Object->SetStringField(TEXT("mimeType"), TEXT("application/json"));
			Values.Add(MakeShared<FJsonValueObject>(Object));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("recommendedFirstRead"), RecommendedFirstRead);
		Result->SetArrayField(TEXT("resources"), Values);
		FString Json;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Result, Writer);
		return Json;
	}

	bool FResourceRegistry::Read(const FString& Uri, FString& OutResult) const
	{
		FReadScopeLock ReadLock(Lock);
		const FResource* Resource = Resources.Find(Uri);
		if (!Resource || !static_cast<bool>(Resource->Handler))
		{
			return false;
		}
		OutResult = Resource->Handler();
		return true;
	}

	FContextRegistry& FContextRegistry::Get()
	{
		static FContextRegistry Registry;
		return Registry;
	}

	void FContextRegistry::RegisterRevisionProvider(FWorldRevisionHandler InWorldRevision, FTargetRevisionHandler InTargetRevision)
	{
		FWriteScopeLock WriteLock(Lock);
		WorldRevision = MoveTemp(InWorldRevision);
		TargetRevision = MoveTemp(InTargetRevision);
	}

	FString FContextRegistry::CaptureWorldRevision() const
	{
		FReadScopeLock ReadLock(Lock);
		return WorldRevision ? WorldRevision() : TEXT("unavailable");
	}

	FString FContextRegistry::CaptureTargetRevision(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments) const
	{
		FReadScopeLock ReadLock(Lock);
		return TargetRevision ? TargetRevision(ToolName, Arguments) : FString();
	}
}
