#include "UEBridgeMCPExtractedTools.h"
#include "WorldDataMCPContextProvider.h"
#include "WorldDataMCPToolRegistry.h"
#include "WorldDataMCPTools.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AssertionMacros.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using FRegistry = WorldDataMCP::FToolRegistry;
	using FToolHandler = WorldDataMCP::FToolHandler;
	using EToolRisk = WorldDataMCP::EToolRisk;

	struct FToolBinding
	{
		const TCHAR* Name;
		FToolHandler Handler;
		EToolRisk Risk;
		TArray<FString> RequiredCapabilities;
	};

	FToolBinding Bind(const TCHAR* Name, FToolHandler Handler, EToolRisk Risk, TArray<FString> RequiredCapabilities = {})
	{
		return FToolBinding{ Name, MoveTemp(Handler), Risk, MoveTemp(RequiredCapabilities) };
	}

	void RegisterCatalog(const FString& ProviderName, const FString& CatalogJson, const TArray<FToolBinding>& Bindings)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CatalogJson);
		ensureMsgf(FJsonSerializer::Deserialize(Reader, Values), TEXT("UEBridgeMCP tool catalog is not valid JSON."));

		TMap<FString, FString> DefinitionsByName;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (!Object.IsValid() || !Object->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				ensureMsgf(false, TEXT("UEBridgeMCP tool catalog contains a definition without a name."));
				continue;
			}

			FString DefinitionJson;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DefinitionJson);
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
			DefinitionsByName.Add(Name, MoveTemp(DefinitionJson));
		}

		for (const FToolBinding& Binding : Bindings)
		{
			FString* DefinitionJson = DefinitionsByName.Find(Binding.Name);
			ensureMsgf(DefinitionJson != nullptr, TEXT("Tool binding '%s' has no JSON schema in its provider catalog."), Binding.Name);
			if (!DefinitionJson)
			{
				continue;
			}

			WorldDataMCP::FToolDefinition Definition;
			Definition.Name = Binding.Name;
			Definition.ProviderName = ProviderName;
			Definition.DefinitionJson = *DefinitionJson;
			Definition.Handler = Binding.Handler;
			Definition.Risk = Binding.Risk;
			Definition.RequiredCapabilities = Binding.RequiredCapabilities;
			Definition.bRequiresInteractiveApproval = Binding.Risk != EToolRisk::ReadOnly;
			Definition.bAudited = true;
			Definition.RevisionPolicy = Binding.Risk == EToolRisk::ReadOnly
				? WorldDataMCP::EToolRevisionPolicy::None
				: WorldDataMCP::EToolRevisionPolicy::RequireFreshContext;
			ensureMsgf(FRegistry::Get().RegisterTool(MoveTemp(Definition)), TEXT("Failed to register UEBridgeMCP tool '%s'."), Binding.Name);
			DefinitionsByName.Remove(Binding.Name);
		}

		for (const auto& Unbound : DefinitionsByName)
		{
			ensureMsgf(false, TEXT("Tool schema '%s' has no executable binding. Register it in the owning provider or move it to Core."), *Unbound.Key);
		}
	}
}

class FUEBridgeMCPToolsModule final : public IModuleInterface, public WorldDataMCP::IWorldDataMCPToolProvider, public WorldDataMCP::IWorldDataMCPResourceProvider
{
public:
	virtual void StartupModule() override
	{
		WorldDataMCP::Tools::RegisterContextProvider();
		RegisterTools();
		RegisterResources();
	}

	virtual FString GetProviderName() const override
	{
		return TEXT("UEBridgeMCPTools");
	}

	virtual void RegisterTools() override
	{
		using namespace WorldDataMCP;

		RegisterCatalog(GetProviderName(), Tools::GetToolDefinitionsJson(), {
			Bind(TEXT("list_level_actors"), Tools::ListLevelActors, EToolRisk::ReadOnly),
			Bind(TEXT("get_selected_actors"), Tools::GetSelectedActors, EToolRisk::ReadOnly),
			Bind(TEXT("get_actor_details"), Tools::GetActorDetails, EToolRisk::ReadOnly),
			Bind(TEXT("find_assets"), Tools::FindAssets, EToolRisk::ReadOnly),
			Bind(TEXT("get_content_summary"), Tools::GetContentSummary, EToolRisk::ReadOnly),
			Bind(TEXT("read_asset"), Tools::ReadAsset, EToolRisk::ReadOnly),
			Bind(TEXT("select_actor"), Tools::SelectActor, EToolRisk::ReadOnly),
			Bind(TEXT("spawn_actor"), Tools::SpawnActor, EToolRisk::WorkspaceChange),
			Bind(TEXT("transform_actor"), Tools::TransformActor, EToolRisk::WorkspaceChange),
			Bind(TEXT("delete_actor"), Tools::DeleteActor, EToolRisk::Destructive),
			Bind(TEXT("attach_actor"), Tools::AttachActor, EToolRisk::WorkspaceChange),
			Bind(TEXT("set_actor_property"), Tools::SetActorProperty, EToolRisk::WorkspaceChange, { TEXT("reflected_property_writes") }),
			Bind(TEXT("save_current_level"), Tools::SaveCurrentLevel, EToolRisk::Destructive),
			Bind(TEXT("create_asset"), Tools::CreateAsset, EToolRisk::Destructive),
			Bind(TEXT("create_blueprint_asset"), Tools::CreateBlueprintAsset, EToolRisk::Destructive),
			Bind(TEXT("modify_material_instance"), Tools::ModifyMaterialInstance, EToolRisk::Destructive),
			Bind(TEXT("create_pcg_graph_from_recipe"), Tools::CreatePcgGraphFromRecipe, EToolRisk::Destructive)
		});

		RegisterCatalog(GetProviderName(), ExtractedTools::GetToolDefinitionsJson(), {
			Bind(TEXT("read_log"), ExtractedTools::ReadLog, EToolRisk::ReadOnly),
			Bind(TEXT("execute_python"), ExtractedTools::ExecutePython, EToolRisk::ArbitraryCode, { TEXT("unsafe_python") }),
			Bind(TEXT("search_assets"), ExtractedTools::SearchAssets, EToolRisk::ReadOnly),
			Bind(TEXT("find_static_meshes"), ExtractedTools::FindStaticMeshes, EToolRisk::ReadOnly),
			Bind(TEXT("get_level_actors"), ExtractedTools::GetLevelActors, EToolRisk::ReadOnly),
			Bind(TEXT("get_project_info"), ExtractedTools::GetProjectInfo, EToolRisk::ReadOnly),
			Bind(TEXT("list_project_modules"), ExtractedTools::ListProjectModules, EToolRisk::ReadOnly),
			Bind(TEXT("get_build_configuration"), ExtractedTools::GetBuildConfiguration, EToolRisk::ReadOnly),
			Bind(TEXT("read_file"), ExtractedTools::ReadFile, EToolRisk::ReadOnly, { TEXT("project_file_tools") }),
			Bind(TEXT("write_file"), ExtractedTools::WriteFile, EToolRisk::Destructive, { TEXT("project_file_tools") }),
			Bind(TEXT("delete_file"), ExtractedTools::DeleteFile, EToolRisk::Destructive, { TEXT("project_file_tools") }),
			Bind(TEXT("rename_file"), ExtractedTools::RenameFile, EToolRisk::Destructive, { TEXT("project_file_tools") }),
			Bind(TEXT("play_in_editor"), ExtractedTools::PlayInEditor, EToolRisk::WorkspaceChange),
			Bind(TEXT("stop_pie"), ExtractedTools::StopPIE, EToolRisk::WorkspaceChange),
			Bind(TEXT("pcg_recipe_library_status"), ExtractedTools::PcgRecipeLibraryStatus, EToolRisk::ReadOnly),
			Bind(TEXT("search_pcg_recipes"), ExtractedTools::SearchPcgRecipes, EToolRisk::ReadOnly),
			Bind(TEXT("read_pcg_recipe"), ExtractedTools::ReadPcgRecipe, EToolRisk::ReadOnly),
			Bind(TEXT("read_pcg_scene_binding"), ExtractedTools::ReadPcgSceneBinding, EToolRisk::ReadOnly)
		});
	}

	virtual void RegisterResources() override
	{
		using namespace WorldDataMCP;
		auto& Resources = FResourceRegistry::Get();
		Resources.RegisterResource(TEXT("worlddata://context/bootstrap"), TEXT("Bootstrap Context"), TEXT("Recommended first-read order and compact editor state."), []
		{
			return Tools::GetBootstrapContextJson();
		});
		Resources.RegisterResource(TEXT("worlddata://level/actors"), TEXT("Level Actors"), TEXT("Current editor-world actors, labels, classes, and transforms."), []
		{
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetNumberField(TEXT("maxResults"), 300);
			return Tools::ListLevelActors(Arguments);
		});
		Resources.RegisterResource(TEXT("worlddata://editor/selection"), TEXT("Editor Selection"), TEXT("Actors currently selected in the editor."), []
		{
			return Tools::GetSelectedActors(MakeShared<FJsonObject>());
		});
		Resources.RegisterResource(TEXT("worlddata://content/assets"), TEXT("Content Assets"), TEXT("Compact asset registry survey under /Game."), []
		{
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetStringField(TEXT("path"), TEXT("/Game"));
			Arguments->SetNumberField(TEXT("maxResults"), 300);
			return Tools::FindAssets(Arguments);
		});
		Resources.RegisterResource(TEXT("worlddata://content/summary"), TEXT("Content Summary"), TEXT("Asset counts by class under /Game."), []
		{
			TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetStringField(TEXT("path"), TEXT("/Game"));
			return Tools::GetContentSummary(Arguments);
		});
	}

	virtual void ShutdownModule() override
	{
		FRegistry::Get().UnregisterProvider(GetProviderName());
		WorldDataMCP::FContextRegistry::Get().ClearRevisionProvider();
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPToolsModule, UEBridgeMCPTools)
