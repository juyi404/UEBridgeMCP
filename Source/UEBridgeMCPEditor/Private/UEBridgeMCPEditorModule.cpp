#include "SUEBridgeMCPPanel.h"
#include "WorldDataMCPServer.h"

// Defined in WorldDataMCPToolModuleBootstrap.cpp (this module): registers every built-in
// tool group with the core registry. Forward-declared here rather than exposed via a Core
// header because it lives in the editor module, not UEBridgeMCPCore.
namespace WorldDataMCP { void RegisterBuiltinMCPToolModules(); }

#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
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

class FUEBridgeMCPEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Populate the tool registry before the server starts so tools/list and the
		// trusted-tool catalog see the full set.
		WorldDataMCP::RegisterBuiltinMCPToolModules();

		FWorldDataMCPServer::Start(FWorldDataMCPServer::LoadConfiguredPort());

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			UEBridgeMCP::PanelTabName,
			FOnSpawnTab::CreateRaw(this, &FUEBridgeMCPEditorModule::SpawnPanelTab))
			.SetDisplayName(LOCTEXT("PanelTabDisplayName", "UEBridgeMCP 对话面板"))
			.SetTooltipText(LOCTEXT("PanelTabTooltip", "打开 UEBridgeMCP 对话面板。"))
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
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(UEBridgeMCP::PanelTabName);
			for (const FName& DynamicTabName : DynamicPanelTabNames)
			{
				FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DynamicTabName);
			}
			DynamicPanelTabNames.Empty();
		}

		FWorldDataMCPServer::Stop();
	}

private:
	TSharedRef<SDockTab> SpawnPanelTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SUEBridgeMCPPanel)
			];
	}

	TSharedRef<SDockTab> SpawnConversationWindowTab(const FSpawnTabArgs& Args, FName DynamicTabName)
	{
		TSharedRef<SDockTab> Tab = SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("ConversationWindowTabLabel", "UEBridgeMCP 对话"))
			[
				SNew(SUEBridgeMCPPanel)
				.StartNewConversation(true)
			];

		Tab->SetOnTabClosed(SDockTab::FOnTabClosedCallback::CreateRaw(
			this,
			&FUEBridgeMCPEditorModule::OnConversationWindowClosed,
			DynamicTabName));

		return Tab;
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window"))
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntry(
				FName(TEXT("UEBridgeMCP_OpenPanel")),
				LOCTEXT("OpenPanelMenuLabel", "UEBridgeMCP 对话面板"),
				LOCTEXT("OpenPanelMenuTooltip", "打开 UEBridgeMCP 对话面板。"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"),
				FUIAction(FExecuteAction::CreateRaw(this, &FUEBridgeMCPEditorModule::OpenPanel)));
		}
	}

	void OpenPanel()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(UEBridgeMCP::PanelTabName);
	}

	void OpenConversationWindow()
	{
		const FName DynamicTabName(*FString::Printf(
			TEXT("UEBridgeMCPPanel_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));

		DynamicPanelTabNames.Add(DynamicTabName);
		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
			DynamicTabName,
			FOnSpawnTab::CreateRaw(this, &FUEBridgeMCPEditorModule::SpawnConversationWindowTab, DynamicTabName))
			.SetDisplayName(LOCTEXT("ConversationWindowDisplayName", "UEBridgeMCP 对话"))
			.SetTooltipText(LOCTEXT("ConversationWindowTooltip", "独立的 UEBridgeMCP 对话窗口。"))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

		FGlobalTabmanager::Get()->TryInvokeTab(DynamicTabName);
	}

	void OnConversationWindowClosed(TSharedRef<SDockTab> ClosedTab, FName DynamicTabName)
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(DynamicTabName);
		DynamicPanelTabNames.Remove(DynamicTabName);
	}

	TSet<FName> DynamicPanelTabNames;
};

IMPLEMENT_MODULE(FUEBridgeMCPEditorModule, UEBridgeMCPEditor)

#undef LOCTEXT_NAMESPACE
