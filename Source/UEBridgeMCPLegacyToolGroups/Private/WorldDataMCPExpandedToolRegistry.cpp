#include "WorldDataMCPToolRegistry.h"

#include "WorldDataMCPAIAnimTools.h"
#include "WorldDataMCPAnimTools.h"
#include "WorldDataMCPAuthoringTools.h"
#include "WorldDataMCPBlueprintTools.h"
#include "WorldDataMCPEditorTools.h"
#include "WorldDataMCPFoliageTools.h"
#include "WorldDataMCPGameplayTools.h"
#include "WorldDataMCPGasTools.h"
#include "WorldDataMCPGraphTools.h"
#include "WorldDataMCPKnowledgeTools.h"
#include "WorldDataMCPMatLandTools.h"
#include "WorldDataMCPMaterialInstanceTools.h"
#include "WorldDataMCPNiagaraTools.h"
#include "WorldDataMCPPcgKnowledgeTools.h"
#include "WorldDataMCPPCGTools.h"
#include "WorldDataMCPSceneTools.h"
#include "WorldDataMCPSequencerTools.h"
#include "WorldDataMCPSpatialTools.h"
#include "WorldDataMCPStateTreeTools.h"
#include "WorldDataMCPUIDataTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorldDataMCPLegacyToolGroups, Log, All);

namespace WorldDataMCP::LegacyToolGroups
{
	namespace
	{
		using FDefinitionProvider = FString(*)();
		using FGroupDispatcher = bool(*)(const FString&, const TSharedPtr<FJsonObject>&, FString&);

		struct FToolGroup
		{
			const TCHAR* Name;
			FDefinitionProvider GetDefinitions;
			FGroupDispatcher Dispatch;
		};

		const TCHAR* ProviderName = TEXT("UEBridgeMCPLegacyToolGroups");

		const FToolGroup ToolGroups[] =
		{
			{ TEXT("AI and animation"), &AIAnimTools::GetToolDefinitionsJson, &AIAnimTools::Dispatch },
			{ TEXT("Animation"), &AnimTools::GetToolDefinitionsJson, &AnimTools::Dispatch },
			{ TEXT("Authoring"), &AuthoringTools::GetToolDefinitionsJson, &AuthoringTools::Dispatch },
			{ TEXT("Blueprint"), &BlueprintTools::GetToolDefinitionsJson, &BlueprintTools::Dispatch },
			{ TEXT("Editor"), &EditorTools::GetToolDefinitionsJson, &EditorTools::Dispatch },
			{ TEXT("Foliage"), &FoliageTools::GetToolDefinitionsJson, &FoliageTools::Dispatch },
			{ TEXT("Gameplay"), &GameplayTools::GetToolDefinitionsJson, &GameplayTools::Dispatch },
			{ TEXT("GAS"), &GasTools::GetToolDefinitionsJson, &GasTools::Dispatch },
			{ TEXT("Graph"), &GraphTools::GetToolDefinitionsJson, &GraphTools::Dispatch },
			{ TEXT("Knowledge"), &KnowledgeTools::GetToolDefinitionsJson, &KnowledgeTools::Dispatch },
			{ TEXT("Material and landscape"), &MatLandTools::GetToolDefinitionsJson, &MatLandTools::Dispatch },
			{ TEXT("Material instance"), &MaterialInstanceTools::GetToolDefinitionsJson, &MaterialInstanceTools::Dispatch },
			{ TEXT("Niagara"), &NiagaraTools::GetToolDefinitionsJson, &NiagaraTools::Dispatch },
			{ TEXT("PCG knowledge"), &PcgKnowledgeTools::GetToolDefinitionsJson, &PcgKnowledgeTools::Dispatch },
			{ TEXT("PCG authoring"), &PCGTools::GetToolDefinitionsJson, &PCGTools::Dispatch },
			{ TEXT("Scene"), &SceneTools::GetToolDefinitionsJson, &SceneTools::Dispatch },
			{ TEXT("Sequencer"), &SequencerTools::GetToolDefinitionsJson, &SequencerTools::Dispatch },
			{ TEXT("Spatial"), &SpatialTools::GetToolDefinitionsJson, &SpatialTools::Dispatch },
			{ TEXT("StateTree"), &StateTreeTools::GetToolDefinitionsJson, &StateTreeTools::Dispatch },
			{ TEXT("UI and data"), &UIDataTools::GetToolDefinitionsJson, &UIDataTools::Dispatch }
		};

		FString SerializeDefinition(const TSharedRef<FJsonObject>& Definition)
		{
			FString Json;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
			FJsonSerializer::Serialize(Definition, Writer);
			return Json;
		}

		EToolRisk GetRisk(const TSharedRef<FJsonObject>& Definition)
		{
			const TSharedPtr<FJsonObject>* Annotations = nullptr;
			if (Definition->TryGetObjectField(TEXT("annotations"), Annotations) && Annotations && Annotations->IsValid())
			{
				bool bReadOnly = false;
				if ((*Annotations)->TryGetBoolField(TEXT("readOnlyHint"), bReadOnly) && bReadOnly)
				{
					return EToolRisk::ReadOnly;
				}

				bool bDestructive = false;
				if ((*Annotations)->TryGetBoolField(TEXT("destructiveHint"), bDestructive) && bDestructive)
				{
					return EToolRisk::Destructive;
				}
			}

			// Every unclassified legacy mutation is conservatively treated as a
			// workspace change. The Core approval path remains mandatory.
			return EToolRisk::WorkspaceChange;
		}

		void RegisterToolGroup(const FToolGroup& Group)
		{
			TArray<TSharedPtr<FJsonValue>> Definitions;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Group.GetDefinitions());
			if (!FJsonSerializer::Deserialize(Reader, Definitions))
			{
				UE_LOG(LogWorldDataMCPLegacyToolGroups, Error, TEXT("Unable to parse the %s MCP tool catalog."), Group.Name);
				return;
			}

			for (const TSharedPtr<FJsonValue>& Value : Definitions)
			{
				const TSharedPtr<FJsonObject> Definition = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!Definition.IsValid())
				{
					continue;
				}

				FString ToolName;
				if (!Definition->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
				{
					UE_LOG(LogWorldDataMCPLegacyToolGroups, Warning, TEXT("Skipping unnamed %s tool definition."), Group.Name);
					continue;
				}

				FToolMetadata Existing;
				if (FToolRegistry::Get().FindToolMetadata(ToolName, Existing))
				{
					// The lightweight module contains security-hardened replacements
					// for a small set of the old helpers. Never replace one.
					UE_LOG(LogWorldDataMCPLegacyToolGroups, Verbose, TEXT("Keeping existing MCP tool '%s' from provider '%s'."), *ToolName, *Existing.ProviderName);
					continue;
				}

				FToolDefinition Registered;
				Registered.Name = ToolName;
				Registered.ProviderName = ProviderName;
				Registered.DefinitionJson = SerializeDefinition(Definition.ToSharedRef());
				Registered.Risk = GetRisk(Definition.ToSharedRef());
				Registered.bRequiresInteractiveApproval = Registered.Risk != EToolRisk::ReadOnly;
				Registered.bAudited = true;
				Registered.RevisionPolicy = Registered.Risk == EToolRisk::ReadOnly
					? EToolRevisionPolicy::None
					: EToolRevisionPolicy::RequireFreshContext;
				Registered.Handler = [Dispatcher = Group.Dispatch, Name = ToolName](const TSharedPtr<FJsonObject>& Arguments)
				{
					FString Result;
					if (!Dispatcher(Name, Arguments, Result))
					{
						return ErrorJson(FString::Printf(TEXT("Legacy MCP tool '%s' did not handle the request."), *Name));
					}
					return Result;
				};

				if (!FToolRegistry::Get().RegisterTool(MoveTemp(Registered)))
				{
					UE_LOG(LogWorldDataMCPLegacyToolGroups, Error, TEXT("Failed to register legacy MCP tool '%s'."), *ToolName);
				}
			}
		}
	}

	void RegisterToolGroups()
	{
		for (const FToolGroup& Group : ToolGroups)
		{
			RegisterToolGroup(Group);
		}
	}

	void UnregisterToolGroups()
	{
		FToolRegistry::Get().UnregisterProvider(ProviderName);
	}
}
