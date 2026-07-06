// Copyright uuuuzz 2024-2026. All Rights Reserved.

#include "UI/McpToolbarExtension.h"
#include "Subsystem/McpEditorSubsystem.h"
#include "UEBridgeMCPEditor.h"
#include "ToolMenus.h"
#include "Framework/Commands/UIAction.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/SlateTypes.h"
#include "Styling/AppStyle.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "UEBridgeMCP"

namespace
{
	const FName UEBridgeMCPMenuOwnerName(TEXT("UEBridgeMCP"));
	const FName UEBridgeMCPSectionName(TEXT("UEBridgeMCP"));
}

bool FMcpToolbarExtension::bIsInitialized = false;
FDelegateHandle FMcpToolbarExtension::StartupCallbackHandle;

void FMcpToolbarExtension::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	// Register when ToolMenus is ready
	StartupCallbackHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateStatic(&FMcpToolbarExtension::RegisterStatusBarExtension));

	bIsInitialized = true;
}

void FMcpToolbarExtension::Shutdown()
{
	if (!bIsInitialized)
	{
		return;
	}

	UToolMenus* ToolMenus = UToolMenus::Get();
	if (ToolMenus)
	{
		if (StartupCallbackHandle.IsValid())
		{
			ToolMenus->UnRegisterStartupCallback(StartupCallbackHandle);
			StartupCallbackHandle.Reset();
		}

		ToolMenus->UnregisterOwner(UEBridgeMCPMenuOwnerName);
	}

	bIsInitialized = false;
}

void FMcpToolbarExtension::RegisterStatusBarExtension()
{
	UToolMenus* ToolMenus = UToolMenus::Get();
	if (!ToolMenus)
	{
		UE_LOG(LogUEBridgeMCPEditor, Warning, TEXT("MCP Status bar: UToolMenus not available"));
		return;
	}

	UE_LOG(LogUEBridgeMCPEditor, Log, TEXT("MCP Status bar: Registering extension..."));

	ToolMenus->UnregisterOwner(UEBridgeMCPMenuOwnerName);
	FToolMenuOwnerScoped OwnerScoped(UEBridgeMCPMenuOwnerName);

	const FSlateIcon RestartIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh");
	const FUIAction RestartAction(FExecuteAction::CreateStatic(&FMcpToolbarExtension::RestartServer));

	// Primary: Status bar. Keep this for users who expect the bottom-window indicator.
	UToolMenu* StatusBar = ToolMenus->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar"));
	if (StatusBar)
	{
		FToolMenuSection& Section = StatusBar->FindOrAddSection(UEBridgeMCPSectionName, FText::GetEmpty(), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));

		Section.AddEntry(FToolMenuEntry::InitWidget(
			"McpStatus",
			CreateStatusWidget(),
			FText::GetEmpty(),
			true  // bNoIndent
		));

		UE_LOG(LogUEBridgeMCPEditor, Log, TEXT("MCP Status bar: Registered on LevelEditor.StatusBar.ToolBar"));
	}

	// Fallback: Main toolbar. UE 5.7+ layouts can hide the status bar entry depending on window chrome.
	UToolMenu* MainToolbar = ToolMenus->ExtendMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));
	if (MainToolbar)
	{
		FToolMenuSection& Section = MainToolbar->FindOrAddSection(UEBridgeMCPSectionName, LOCTEXT("UEBridgeMCPToolbarSection", "UEBridgeMCP"));

		Section.AddEntry(FToolMenuEntry::InitWidget(
			"McpStatusToolbar",
			CreateStatusWidget(),
			FText::GetEmpty(),
			true  // bNoIndent
		));

		FToolMenuEntry ToolbarEntry = FToolMenuEntry::InitToolBarButton(
			"UEBridgeMCPRestartToolbar",
			RestartAction,
			LOCTEXT("UEBridgeMCPToolbarLabel", "UEBridgeMCP"),
			LOCTEXT("UEBridgeMCPToolbarTooltip", "Restart the UEBridgeMCP server"),
			RestartIcon);
		ToolbarEntry.StyleNameOverride = "CalloutToolbar";
		Section.AddEntry(ToolbarEntry);

		UE_LOG(LogUEBridgeMCPEditor, Log, TEXT("MCP Status bar: Also registered on LevelEditor.LevelEditorToolBar.User (fallback toolbar)"));
	}

	// Always-visible command path from the main menu.
	UToolMenu* ToolsMenu = ToolMenus->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	if (ToolsMenu)
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection(UEBridgeMCPSectionName, LOCTEXT("UEBridgeMCPToolsSection", "UEBridgeMCP"));
		Section.AddMenuEntry(
			"UEBridgeMCPRestartServer",
			LOCTEXT("UEBridgeMCPRestartLabel", "UEBridgeMCP: Restart MCP Server"),
			LOCTEXT("UEBridgeMCPRestartTooltip", "Restart the local UEBridgeMCP server used by MCP clients."),
			RestartIcon,
			RestartAction);

		UE_LOG(LogUEBridgeMCPEditor, Log, TEXT("MCP Status bar: Registered on LevelEditor.MainMenu.Tools"));
	}

	ToolMenus->RefreshAllWidgets();
}

TSharedRef<SWidget> FMcpToolbarExtension::CreateStatusWidget()
{
	return SNew(SButton)
		.ButtonStyle(&FAppStyle::Get().GetWidgetStyle<FButtonStyle>("SimpleButton"))
		.OnClicked_Static(&FMcpToolbarExtension::OnStatusButtonClicked)
		.ToolTipText_Static(&FMcpToolbarExtension::GetStatusTooltip)
		.ContentPadding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(10.0f)
				.HeightOverride(10.0f)
				[
					SNew(SImage)
					.Image_Static(&FMcpToolbarExtension::GetStatusBrush)
					.ColorAndOpacity_Static(&FMcpToolbarExtension::GetStatusColor)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("McpLabel", "UEBridgeMCP"))
				.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("SmallText"))
			]
		];
}

const FSlateBrush* FMcpToolbarExtension::GetStatusBrush()
{
	return FAppStyle::Get().GetBrush("Icons.FilledCircle");
}

FSlateColor FMcpToolbarExtension::GetStatusColor()
{
	UMcpEditorSubsystem* Subsystem = GetSubsystem();
	if (!Subsystem)
	{
		return FSlateColor(FLinearColor::Gray);
	}

	FMcpServer* Server = Subsystem->GetServer();
	if (!Server)
	{
		return FSlateColor(FLinearColor::Gray);
	}

	switch (Server->GetStatus())
	{
	case EMcpServerStatus::Running:
		return FSlateColor(FLinearColor::Green);
	case EMcpServerStatus::Overloaded:
		return FSlateColor(FLinearColor(1.0f, 0.5f, 0.0f)); // Orange
	case EMcpServerStatus::Error:
		return FSlateColor(FLinearColor::Yellow);
	case EMcpServerStatus::Stopped:
	default:
		return FSlateColor(FLinearColor::Red);
	}
}

FText FMcpToolbarExtension::GetStatusTooltip()
{
	UMcpEditorSubsystem* Subsystem = GetSubsystem();
	if (!Subsystem)
	{
		return LOCTEXT("McpNotAvailableTooltip", "MCP Server: Not Available");
	}

	FString Version = TEXT("Unknown");
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEBridgeMCP")))
	{
		Version = Plugin->GetDescriptor().VersionName;
	}

	FMcpServer* Server = Subsystem->GetServer();
	FString Status;
	FString Extra;

	if (Server)
	{
		switch (Server->GetStatus())
		{
		case EMcpServerStatus::Running:
			Status = FString::Printf(TEXT("Running on port %d"), Subsystem->GetActualPort());
			if (Server->GetPendingRequestCount() > 0)
			{
				Extra = FString::Printf(TEXT("\nPending requests: %d"), Server->GetPendingRequestCount());
			}
			break;
		case EMcpServerStatus::Overloaded:
			Status = FString::Printf(TEXT("OVERLOADED on port %d"), Subsystem->GetActualPort());
			Extra = FString::Printf(TEXT("\nPending requests: %d (max: %d)"),
				Server->GetPendingRequestCount(), 10);
			break;
		case EMcpServerStatus::Error:
			Status = TEXT("ERROR");
			if (!Server->GetLastError().IsEmpty())
			{
				Extra = FString::Printf(TEXT("\nLast error: %s"), *Server->GetLastError());
			}
			break;
		case EMcpServerStatus::Stopped:
		default:
			{
				int32 ConfiguredPort = Subsystem->GetSettings() ? Subsystem->GetSettings()->ServerPort : 8080;
				Status = FString::Printf(TEXT("Stopped (port %d may be in use)"), ConfiguredPort);
			}
			break;
		}
	}
	else
	{
		Status = TEXT("Not initialized");
	}

	return FText::Format(
		LOCTEXT("McpTooltipFormat", "UEBridgeMCP v{0}\nStatus: {1}{2}\n\nClick to restart server"),
		FText::FromString(Version),
		FText::FromString(Status),
		FText::FromString(Extra));
}

void FMcpToolbarExtension::RestartServer()
{
	UMcpEditorSubsystem* Subsystem = GetSubsystem();
	if (Subsystem)
	{
		Subsystem->RestartServer();
	}
}

FReply FMcpToolbarExtension::OnStatusButtonClicked()
{
	RestartServer();

	return FReply::Handled();
}

UMcpEditorSubsystem* FMcpToolbarExtension::GetSubsystem()
{
	if (GEditor)
	{
		return GEditor->GetEditorSubsystem<UMcpEditorSubsystem>();
	}
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
