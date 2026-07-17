#include "IWorldDataAgentBootstrapModule.h"
#include "IWorldDataAgentUIModule.h"
#include "UEBridgeMCPCoreModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "ToolMenu.h"
#include "ToolMenuEntry.h"
#include "ToolMenuSection.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "UEBridgeMCPEditor"

namespace
{
	const FName PanelTabName(TEXT("WorldDataAgentPanel"));
}

class FUEBridgeMCPEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Load Core first, then let Tools register every handler before the first
		// HTTP request can observe tools/list.
		FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("UEBridgeMCPTools"));
		IWorldDataMCPService& McpService = IUEBridgeMCPCoreModule::Get().GetService();
		McpService.StartConfigured();

		FWorldDataAgentConnectionOptions AgentOptions;
		AgentOptions.ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		AgentOptions.RuntimeManifestPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("Runtime"), TEXT("runtime-manifest.json")));
		AgentOptions.McpUrl = McpService.IsRunning() ? McpService.GetMcpUrl() : FString();
		IWorldDataAgentSubsystem& AgentSubsystem = IWorldDataAgentBootstrapModule::Get().GetSubsystem();
		FString SecretError;
		AgentSubsystem.GetSecurity().StoreEphemeralSecret(
			TEXT("worlddata_mcp"),
			McpService.GetAccessToken(),
			AgentOptions.McpSecretHandle,
			SecretError);
		AgentSubsystem.Initialize(AgentOptions);

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			PanelTabName,
			FOnSpawnTab::CreateRaw(this, &FUEBridgeMCPEditorModule::SpawnPanelTab))
			.SetDisplayName(LOCTEXT("PanelTabDisplayName", "WorldData MCP 控制台"))
			.SetTooltipText(LOCTEXT("PanelTabTooltip", "打开 WorldData MCP 控制台。"))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

		UToolMenus::Get()->RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUEBridgeMCPEditorModule::RegisterMenus));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PanelTabName);
		}

		IWorldDataAgentBootstrapModule::Get().GetSubsystem().Shutdown();
		IUEBridgeMCPCoreModule::Get().GetService().Stop();
	}

private:
	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				IWorldDataAgentUIModule::Get().CreatePanel()
			];
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window"))
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntry(
				FName(TEXT("UEBridgeMCP_OpenPanel")),
				LOCTEXT("OpenPanelMenuLabel", "打开 WorldData MCP 控制台"),
				LOCTEXT("OpenPanelMenuTooltip", "以可停靠标签页打开或聚焦 WorldData MCP 控制台。"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
				FUIAction(FExecuteAction::CreateRaw(this, &FUEBridgeMCPEditorModule::OpenPanel)));
		}
	}

	void OpenPanel()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(PanelTabName);
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPEditorModule, UEBridgeMCPEditor)

#undef LOCTEXT_NAMESPACE
