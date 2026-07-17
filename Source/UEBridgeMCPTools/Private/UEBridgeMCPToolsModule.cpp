#include "UEBridgeMCPExtractedTools.h"
#include "WorldDataMCPContextProvider.h"
#include "WorldDataMCPToolRegistry.h"
#include "WorldDataMCPTools.h"

#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"

namespace
{
	using FRegistry = WorldDataMCP::FToolRegistry;

	template <typename THandler>
	void Register(const TCHAR* Name, THandler Handler)
	{
		FRegistry::Get().RegisterTool(Name, Handler);
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

		Register(TEXT("list_level_actors"), Tools::ListLevelActors);
		Register(TEXT("get_selected_actors"), Tools::GetSelectedActors);
		Register(TEXT("get_actor_details"), Tools::GetActorDetails);
		Register(TEXT("find_assets"), Tools::FindAssets);
		Register(TEXT("get_content_summary"), Tools::GetContentSummary);
		Register(TEXT("read_asset"), Tools::ReadAsset);
		Register(TEXT("select_actor"), Tools::SelectActor);
		Register(TEXT("spawn_actor"), Tools::SpawnActor);
		Register(TEXT("transform_actor"), Tools::TransformActor);
		Register(TEXT("delete_actor"), Tools::DeleteActor);
		Register(TEXT("attach_actor"), Tools::AttachActor);
		Register(TEXT("set_actor_property"), Tools::SetActorProperty);
		Register(TEXT("save_current_level"), Tools::SaveCurrentLevel);
		Register(TEXT("create_asset"), Tools::CreateAsset);
		Register(TEXT("create_blueprint_asset"), Tools::CreateBlueprintAsset);
		Register(TEXT("modify_material_instance"), Tools::ModifyMaterialInstance);
		Register(TEXT("create_pcg_graph_from_recipe"), Tools::CreatePcgGraphFromRecipe);
		Register(TEXT("__resource_bootstrap"), [](const TSharedPtr<FJsonObject>&) { return Tools::GetBootstrapContextJson(); });

		Register(TEXT("read_log"), ExtractedTools::ReadLog);
		Register(TEXT("execute_python"), ExtractedTools::ExecutePython);
		Register(TEXT("search_assets"), ExtractedTools::SearchAssets);
		Register(TEXT("find_static_meshes"), ExtractedTools::FindStaticMeshes);
		Register(TEXT("get_level_actors"), ExtractedTools::GetLevelActors);
		Register(TEXT("get_project_info"), ExtractedTools::GetProjectInfo);
		Register(TEXT("list_project_modules"), ExtractedTools::ListProjectModules);
		Register(TEXT("get_build_configuration"), ExtractedTools::GetBuildConfiguration);
		Register(TEXT("read_file"), ExtractedTools::ReadFile);
		Register(TEXT("write_file"), ExtractedTools::WriteFile);
		Register(TEXT("delete_file"), ExtractedTools::DeleteFile);
		Register(TEXT("rename_file"), ExtractedTools::RenameFile);
		Register(TEXT("play_in_editor"), ExtractedTools::PlayInEditor);
		Register(TEXT("stop_pie"), ExtractedTools::StopPIE);
		Register(TEXT("pcg_recipe_library_status"), ExtractedTools::PcgRecipeLibraryStatus);
		Register(TEXT("search_pcg_recipes"), ExtractedTools::SearchPcgRecipes);
		Register(TEXT("read_pcg_recipe"), ExtractedTools::ReadPcgRecipe);
		Register(TEXT("read_pcg_scene_binding"), ExtractedTools::ReadPcgSceneBinding);

		FRegistry::Get().RegisterDefinitionSet(Tools::GetToolDefinitionsJson());
		FRegistry::Get().RegisterDefinitionSet(ExtractedTools::GetToolDefinitionsJson());
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
		// Core owns the registry lifetime. It is cleared after all tool modules unload.
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPToolsModule, UEBridgeMCPTools)
