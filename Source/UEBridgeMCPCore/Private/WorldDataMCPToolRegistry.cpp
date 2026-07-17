#include "WorldDataMCPToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeRWLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace WorldDataMCP
{
	namespace
	{
		FToolMetadata ToMetadata(const FToolDefinition& Definition)
		{
			FToolMetadata Metadata;
			Metadata.Name = Definition.Name;
			Metadata.ProviderName = Definition.ProviderName;
			Metadata.Risk = Definition.Risk;
			Metadata.RequiredCapabilities = Definition.RequiredCapabilities;
			Metadata.bRequiresInteractiveApproval = Definition.bRequiresInteractiveApproval;
			Metadata.bAudited = Definition.bAudited;
			Metadata.RevisionPolicy = Definition.RevisionPolicy;
			return Metadata;
		}

		void AddRegistryAnnotations(const FToolDefinition& Definition, const TSharedRef<FJsonObject>& Tool)
		{
			const TSharedPtr<FJsonObject>* ExistingAnnotations = nullptr;
			TSharedPtr<FJsonObject> Annotations;
			if (Tool->TryGetObjectField(TEXT("annotations"), ExistingAnnotations) && ExistingAnnotations && ExistingAnnotations->IsValid())
			{
				Annotations = *ExistingAnnotations;
			}
			else
			{
				Annotations = MakeShared<FJsonObject>();
				Tool->SetObjectField(TEXT("annotations"), Annotations.ToSharedRef());
			}

			Annotations->SetStringField(TEXT("worldDataRisk"), GetToolRiskName(Definition.Risk));
			Annotations->SetBoolField(TEXT("worldDataInteractiveApproval"), Definition.bRequiresInteractiveApproval);
			Annotations->SetBoolField(TEXT("worldDataAudited"), Definition.bAudited);
			Annotations->SetStringField(TEXT("worldDataRevisionPolicy"), GetToolRevisionPolicyName(Definition.RevisionPolicy));

			TArray<TSharedPtr<FJsonValue>> Capabilities;
			for (const FString& Capability : Definition.RequiredCapabilities)
			{
				Capabilities.Add(MakeShared<FJsonValueString>(Capability));
			}
			Annotations->SetArrayField(TEXT("worldDataRequiredCapabilities"), Capabilities);
		}
	}

	FString GetToolRiskName(const EToolRisk Risk)
	{
		switch (Risk)
		{
		case EToolRisk::ReadOnly: return TEXT("read_only");
		case EToolRisk::WorkspaceChange: return TEXT("workspace_change");
		case EToolRisk::Destructive: return TEXT("destructive");
		case EToolRisk::ArbitraryCode: return TEXT("arbitrary_code");
		default: return TEXT("unknown");
		}
	}

	FString GetToolRevisionPolicyName(const EToolRevisionPolicy Policy)
	{
		switch (Policy)
		{
		case EToolRevisionPolicy::None: return TEXT("none");
		case EToolRevisionPolicy::RequireFreshContext: return TEXT("require_fresh_context");
		default: return TEXT("unknown");
		}
	}

	FToolRegistry& FToolRegistry::Get()
	{
		static FToolRegistry Registry;
		return Registry;
	}

	bool FToolRegistry::RegisterTool(FToolDefinition Definition)
	{
		if (Definition.Name.IsEmpty()
			|| Definition.ProviderName.IsEmpty()
			|| Definition.DefinitionJson.IsEmpty()
			|| !static_cast<bool>(Definition.Handler))
		{
			return false;
		}

		TSharedPtr<FJsonObject> DefinitionObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Definition.DefinitionJson);
		FString DeclaredName;
		if (!FJsonSerializer::Deserialize(Reader, DefinitionObject)
			|| !DefinitionObject.IsValid()
			|| !DefinitionObject->TryGetStringField(TEXT("name"), DeclaredName)
			|| DeclaredName != Definition.Name)
		{
			return false;
		}

		FWriteScopeLock WriteLock(Lock);
		Tools.Add(Definition.Name, MoveTemp(Definition));
		return true;
	}

	void FToolRegistry::UnregisterProvider(const FString& ProviderName)
	{
		FWriteScopeLock WriteLock(Lock);
		for (auto It = Tools.CreateIterator(); It; ++It)
		{
			if (It->Value.ProviderName == ProviderName)
			{
				It.RemoveCurrent();
			}
		}
	}

	bool FToolRegistry::Dispatch(const FString& Name, const TSharedPtr<FJsonObject>& Arguments, FString& OutResult) const
	{
		FToolHandler Handler;
		{
			FReadScopeLock ReadLock(Lock);
			const FToolDefinition* Definition = Tools.Find(Name);
			if (!Definition || !static_cast<bool>(Definition->Handler))
			{
				return false;
			}
			Handler = Definition->Handler;
		}

		// Provider code may register/unregister tools, make a resource call, or
		// schedule editor work. Never execute it while the registry lock is held.
		OutResult = Handler(Arguments);
		return true;
	}

	bool FToolRegistry::FindToolMetadata(const FString& Name, FToolMetadata& OutMetadata) const
	{
		FReadScopeLock ReadLock(Lock);
		const FToolDefinition* Definition = Tools.Find(Name);
		if (!Definition)
		{
			return false;
		}
		OutMetadata = ToMetadata(*Definition);
		return true;
	}

	TArray<FToolMetadata> FToolRegistry::GetRegisteredToolMetadata() const
	{
		TArray<FToolMetadata> Result;
		{
			FReadScopeLock ReadLock(Lock);
			Result.Reserve(Tools.Num());
			for (const auto& Pair : Tools)
			{
				Result.Add(ToMetadata(Pair.Value));
			}
		}
		Result.Sort([](const FToolMetadata& Left, const FToolMetadata& Right)
		{
			return Left.Name < Right.Name;
		});
		return Result;
	}

	FString FToolRegistry::GetRegisteredDefinitionsJson() const
	{
		TArray<FToolDefinition> Definitions;
		{
			FReadScopeLock ReadLock(Lock);
			Definitions.Reserve(Tools.Num());
			for (const auto& Pair : Tools)
			{
				Definitions.Add(Pair.Value);
			}
		}
		Definitions.Sort([](const FToolDefinition& Left, const FToolDefinition& Right)
		{
			return Left.Name < Right.Name;
		});

		TArray<TSharedPtr<FJsonValue>> CombinedDefinitions;
		for (const FToolDefinition& Definition : Definitions)
		{
			TSharedPtr<FJsonObject> ParsedDefinition;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Definition.DefinitionJson);
			if (FJsonSerializer::Deserialize(Reader, ParsedDefinition) && ParsedDefinition.IsValid())
			{
				AddRegistryAnnotations(Definition, ParsedDefinition.ToSharedRef());
				CombinedDefinitions.Add(MakeShared<FJsonValueObject>(ParsedDefinition));
			}
		}

		FString Result;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
		FJsonSerializer::Serialize(CombinedDefinitions, Writer);
		return Result;
	}

	void FToolRegistry::Reset()
	{
		FWriteScopeLock WriteLock(Lock);
		Tools.Reset();
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
		struct FResourceSummary
		{
			FString Uri;
			FString Name;
			FString Description;
		};
		TArray<FResourceSummary> Summaries;
		{
			FReadScopeLock ReadLock(Lock);
			Summaries.Reserve(Resources.Num());
			for (const auto& Pair : Resources)
			{
				Summaries.Add(FResourceSummary{ Pair.Key, Pair.Value.Name, Pair.Value.Description });
			}
		}
		Summaries.Sort([](const FResourceSummary& Left, const FResourceSummary& Right)
		{
			return Left.Uri < Right.Uri;
		});

		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FResourceSummary& Resource : Summaries)
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("uri"), Resource.Uri);
			Object->SetStringField(TEXT("name"), Resource.Name);
			Object->SetStringField(TEXT("description"), Resource.Description);
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
		FResourceHandler Handler;
		{
			FReadScopeLock ReadLock(Lock);
			const FResource* Resource = Resources.Find(Uri);
			if (!Resource || !static_cast<bool>(Resource->Handler))
			{
				return false;
			}
			Handler = Resource->Handler;
		}

		// Resources can read editor state and must not run inside the registry
		// lock for the same reason as tool handlers.
		OutResult = Handler();
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

	void FContextRegistry::ClearRevisionProvider()
	{
		FWriteScopeLock WriteLock(Lock);
		WorldRevision = nullptr;
		TargetRevision = nullptr;
	}

	FString FContextRegistry::CaptureWorldRevision() const
	{
		FWorldRevisionHandler Handler;
		{
			FReadScopeLock ReadLock(Lock);
			Handler = WorldRevision;
		}
		return Handler ? Handler() : TEXT("unavailable");
	}

	FString FContextRegistry::CaptureTargetRevision(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments) const
	{
		FTargetRevisionHandler Handler;
		{
			FReadScopeLock ReadLock(Lock);
			Handler = TargetRevision;
		}
		return Handler ? Handler(ToolName, Arguments) : FString();
	}
}
