#include "SUEBridgeMCPPanel.h"

#include "WorldDataMCPServer.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Images/SThrobber.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UEBridgeMCPEditor"

void SUEBridgeMCPPanel::Construct(const FArguments& InArgs)
{
	bStartNewConversationOnConstruct = InArgs._StartNewConversation;
	LastAction = LOCTEXT("InitialLastAction", "新会话已就绪。");
	LoadSettings();
	ServerPortText = FString::FromInt(FWorldDataMCPServer::IsRunning()
		? FWorldDataMCPServer::GetPort()
		: FWorldDataMCPServer::LoadConfiguredPort());
	RefreshCliDetections();
	ConfigureLightTextBoxStyle();
	ConfigureComposerButtonStyle();
	CurrentModel = GetCurrentAgentModel();
	// ACP clients are created lazily per conversation (EnsureClientForConversation) so each
	// conversation runs in its own adapter process + ACP session and can run concurrently.

	ChildSlot
	[
		SNew(SBorder)
		.Padding(0.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
		[
			BuildUEBridgeStyleLayout()
		]
	];

	LoadConversations();
	if (bStartNewConversationOnConstruct)
	{
		ResetConversationView();
		SetLastAction(LOCTEXT("NewConversationWindowReadyAction", "新对话窗口已准备好。"));
	}
	else if (Conversations.Num() > 0)
	{
		const int32 InitialConversationIndex = Conversations.IsValidIndex(ActiveConversationIndex) ? ActiveConversationIndex : 0;
		ActiveConversationIndex = INDEX_NONE;
		LoadConversation(InitialConversationIndex);
		SetLastAction(LOCTEXT("ConversationsRestoredAction", "已恢复对话记忆。"));
	}
	else
	{
		ResetConversationView();
	}
}

SUEBridgeMCPPanel::~SUEBridgeMCPPanel()
{
	SaveActiveConversation();
	StopAllClients();
}

int64 SUEBridgeMCPPanel::GetActiveConversationId() const
{
	return Conversations.IsValidIndex(ActiveConversationIndex) ? Conversations[ActiveConversationIndex].Id : 0;
}

int64 SUEBridgeMCPPanel::EnsureConversationId(int32 ConversationIndex)
{
	if (!Conversations.IsValidIndex(ConversationIndex))
	{
		return 0;
	}
	FConversation& Conversation = Conversations[ConversationIndex];
	if (Conversation.Id == 0)
	{
		Conversation.Id = NextConversationId++;
	}
	return Conversation.Id;
}

int32 SUEBridgeMCPPanel::FindConversationIndexById(int64 ConvId) const
{
	if (ConvId == 0)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Conversations.Num(); ++Index)
	{
		if (Conversations[Index].Id == ConvId)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

SUEBridgeMCPPanel::FConversationRuntime& SUEBridgeMCPPanel::GetRuntime(int64 ConvId)
{
	return ConversationRuntimes.FindOrAdd(ConvId);
}

const SUEBridgeMCPPanel::FConversationRuntime* SUEBridgeMCPPanel::FindRuntime(int64 ConvId) const
{
	return ConvId != 0 ? ConversationRuntimes.Find(ConvId) : nullptr;
}

const SUEBridgeMCPPanel::FConversationRuntime* SUEBridgeMCPPanel::FindActiveRuntime() const
{
	return FindRuntime(GetActiveConversationId());
}

FWorldDataCodexACPClient* SUEBridgeMCPPanel::EnsureClientForConversation(int64 ConvId)
{
	if (ConvId == 0)
	{
		return nullptr;
	}

	FConversationRuntime& Runtime = GetRuntime(ConvId);
	if (!Runtime.Client.IsValid())
	{
		Runtime.Client = MakeShared<FWorldDataCodexACPClient>();
		Runtime.Client->SetPermissionMode(CurrentMode);
		Runtime.Client->SetAgent(CurrentAcpAgent);
		Runtime.Client->SetModel(CurrentModel);
		// Bind the owning ConvId as a delegate payload so callbacks route to this
		// conversation even while the user is looking at a different one.
		Runtime.Client->OnText.BindSP(this, &SUEBridgeMCPPanel::HandleAcpText, ConvId);
		Runtime.Client->OnThought.BindSP(this, &SUEBridgeMCPPanel::HandleAcpThought, ConvId);
		Runtime.Client->OnStatus.BindSP(this, &SUEBridgeMCPPanel::HandleAcpStatus, ConvId);
		Runtime.Client->OnError.BindSP(this, &SUEBridgeMCPPanel::HandleAcpError, ConvId);
		Runtime.Client->OnPermission.BindSP(this, &SUEBridgeMCPPanel::HandleAcpPermission, ConvId);
		Runtime.Client->OnTurnComplete.BindSP(this, &SUEBridgeMCPPanel::HandleAcpTurnComplete, ConvId);
	}
	return Runtime.Client.Get();
}

bool SUEBridgeMCPPanel::IsConversationBusy(int64 ConvId) const
{
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	if (!Runtime)
	{
		return false;
	}
	if (Runtime->bHasPendingPermission)
	{
		return true;
	}
	return Runtime->Client.IsValid() && Runtime->Client->IsProcessing();
}

bool SUEBridgeMCPPanel::ActiveHasPendingPermission() const
{
	const FConversationRuntime* Runtime = FindActiveRuntime();
	return Runtime && Runtime->bHasPendingPermission;
}

bool SUEBridgeMCPPanel::ActiveFocusAwaitingConfirmation() const
{
	const FConversationRuntime* Runtime = FindActiveRuntime();
	return Runtime && Runtime->bFocusPlanAwaitingConfirmation;
}

void SUEBridgeMCPPanel::StopAllClients()
{
	for (TPair<int64, FConversationRuntime>& Pair : ConversationRuntimes)
	{
		if (Pair.Value.Client.IsValid())
		{
			Pair.Value.Client->Stop();
		}
	}
}

void SUEBridgeMCPPanel::ApplyAgentSettingsToAllClients()
{
	for (TPair<int64, FConversationRuntime>& Pair : ConversationRuntimes)
	{
		if (Pair.Value.Client.IsValid())
		{
			Pair.Value.Client->SetAgent(CurrentAcpAgent);
			Pair.Value.Client->SetModel(CurrentModel);
			Pair.Value.Client->SetPermissionMode(CurrentMode);
		}
	}
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildUEBridgeStyleLayout()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			[
				SNew(SBox)
				.HeightOverride(4.0f)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildUEBridgeTopBar()
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				BuildUEBridgeSidebar()
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				BuildUEBridgeMainArea()
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildUEBridgeTopBar()
{
	return SNew(SBorder)
		.Padding(0.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
		[
			SNew(SBox)
			.HeightOverride(38.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SBox)
					.WidthOverride(288.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(12.0f, 0.0f, 6.0f, 0.0f)
						[
							SNew(SBorder)
							.Padding(5.0f, 2.0f)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.18f)); })
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BrandMark", "WD"))
								.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
								.Font(FAppStyle::GetFontStyle("NormalFontBold"))
							]
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("BrandName", "worlddata.ai"))
							.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 10.0f, 0.0f)
						[
							BuildIconTextButton(LOCTEXT("NewChatTopButton", "+"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnNewConversationClicked), LOCTEXT("NewChatTopTooltip", "新建会话"))
						]
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 6.0f, 0.0f)
					[
						SNew(SBorder)
						.Padding(8.0f, 2.0f)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.22f)); })
						[
							SNew(STextBlock)
							.Text_Lambda([this] { return GetAgentLabel(CurrentAcpAgent); })
							.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ConnectionDot", "●"))
						.ColorAndOpacity_Lambda([] { return UEBridgeMCP::GetStatusColor(); })
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNullWidget::NullWidget
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						BuildToolbarButton(LOCTEXT("CopyUrlTopButton", "复制连接"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnCopyUrlClicked))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						BuildToolbarButton(LOCTEXT("RefreshTopButton", "刷新"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnRefreshClicked), TAttribute<bool>::CreateLambda([] { return FWorldDataMCPServer::IsRunning(); }))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						BuildToolbarButton(LOCTEXT("SettingsTopButton", "设置"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSettingsClicked))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 12.0f, 0.0f)
					[
						BuildToolbarButton(LOCTEXT("NewChatButton", "新对话"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnNewConversationClicked))
					]
				]
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildUEBridgeSidebar()
{
	return SNew(SBorder)
		.Padding(0.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
		[
			SNew(SBox)
			.WidthOverride(288.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SAssignNew(SidebarListScrollBox, SScrollBox)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBorder)
					.Padding(10.0f, 8.0f)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							BuildBadge(LOCTEXT("UserAvatar", "J"))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						.Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("UserName", "Ju Yu"))
							.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("UserMore", ">"))
							.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
						]
					]
				]
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildUEBridgeMainArea()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.Padding(18.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SBorder)
					.Padding(0.0f)
					.BorderImage(FAppStyle::GetBrush("NoBorder"))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SBox)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.Visibility_Lambda([this] { return (!bShowDetail && !bShowSettings) ? EVisibility::Visible : EVisibility::Collapsed; })
							[
								SNew(STextBlock)
								.Text(LOCTEXT("EmptyChatPrompt", "今天我能帮您做什么？"))
								.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
							]
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SBox)
							.Visibility_Lambda([this] { return (bShowDetail && !bShowSettings) ? EVisibility::Visible : EVisibility::Collapsed; })
							[
								BuildConversationDetail()
							]
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							SNew(SBox)
							.Visibility_Lambda([this] { return bShowSettings ? EVisibility::Visible : EVisibility::Collapsed; })
							[
								BuildSettingsPanel()
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this] { return bShowSettings ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						BuildComposer()
					]
				]
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildBadge(const FText& Text) const
{
	return SNew(SBorder)
		.Padding(8.0f, 2.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.22f)); })
		[
			SNew(STextBlock)
			.Text(Text)
			.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildToolbarButton(const FText& Label, FOnClicked OnClicked, TAttribute<bool> bIsEnabled) const
{
	return SNew(SButton)
		.HAlign(HAlign_Center)
		.ButtonStyle(&ComposerButtonStyle)
		.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
		.ForegroundColor(FSlateColor(GetPanelTextColor()))
		.Text(Label)
		.IsEnabled(bIsEnabled)
		.OnClicked(OnClicked);
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildIconTextButton(const FText& Label, FOnClicked OnClicked, const FText& Tooltip) const
{
	return SNew(SButton)
		.HAlign(HAlign_Center)
		.ButtonStyle(&ComposerButtonStyle)
		.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
		.ForegroundColor(FSlateColor(GetPanelTextColor()))
		.ContentPadding(FMargin(10.0f, 3.0f))
		.Text(Label)
		.ToolTipText(Tooltip)
		.OnClicked(OnClicked);
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildModeCombo()
{
	return SNew(SComboButton)
		.ToolTipText(LOCTEXT("ModeComboTooltip", "选择 Codex 执行模式"))
		.ComboButtonStyle(&ComposerComboButtonStyle)
		.ButtonStyle(&ComposerButtonStyle)
		.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
		.ForegroundColor(FSlateColor(GetPanelTextColor()))
		.ContentPadding(FMargin(8.0f, 3.0f))
		.HasDownArrow(false)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this] { return GetModeLabel(CurrentMode); })
				.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModeComboChevron", "v"))
				.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
			]
		]
		.MenuContent()
		[
			BuildModeMenu()
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildModeMenu()
{
	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
		[
			SNew(SBorder)
			.Padding(FMargin(8.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f, 2.0f, 4.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ModeMenuTitle", "模式"))
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildModeMenuItem(EWorldDataCodexPermissionMode::Default, LOCTEXT("ModeDefaultLabel", "默认"), LOCTEXT("ModeDefaultDesc", "请求前确认，Shell 禁用"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					BuildModeMenuItem(EWorldDataCodexPermissionMode::Plan, LOCTEXT("ModePlanLabel", "计划模式"), LOCTEXT("ModePlanDesc", "只输出计划，不调用工具"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					BuildModeMenuItem(EWorldDataCodexPermissionMode::Focus, LOCTEXT("ModeFocusLabel", "专注模式"), LOCTEXT("ModeFocusDesc", "多轮推演并输出落地计划，确认后再执行"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					BuildModeMenuItem(EWorldDataCodexPermissionMode::Bypass, LOCTEXT("ModeBypassLabel", "绕过模式"), LOCTEXT("ModeBypassDesc", "自动允许 MCP，Shell 禁用"))
				]
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildModeMenuItem(EWorldDataCodexPermissionMode Mode, const FText& Label, const FText& Description)
{
	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this, Mode]
		{
			return CurrentMode == Mode
				? FSlateColor(GetAccentBorderColor())
				: FSlateColor(UEBridgeMCP::Palette::Background());
		})
		.OnMouseButtonDown_Lambda([this, Mode](const FGeometry&, const FPointerEvent&)
		{
			if (CurrentMode != Mode)
			{
				ResetFocusWorkflow(GetActiveConversationId());
			}
			CurrentMode = Mode;
			// Mode is (re)applied to each conversation's client at send time; also push it to
			// any live clients so idle sessions reflect the new mode immediately.
			for (TPair<int64, FConversationRuntime>& Pair : ConversationRuntimes)
			{
				if (Pair.Value.Client.IsValid())
				{
					Pair.Value.Client->SetPermissionMode(CurrentMode);
				}
			}
			SaveSettings();
			SetLastAction(FText::Format(LOCTEXT("ModeChangedAction", "已切换到：{0}"), GetModeLabel(CurrentMode)));
			FSlateApplication::Get().DismissAllMenus();
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, Mode]
			{
				return CurrentMode == Mode
					? FSlateColor(GetAccentFillColor(0.18f))
					: FSlateColor(UEBridgeMCP::Palette::Background());
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Description)
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
				]
			]
		];
}

FText SUEBridgeMCPPanel::GetModeLabel(EWorldDataCodexPermissionMode Mode) const
{
	switch (Mode)
	{
	case EWorldDataCodexPermissionMode::Plan:
		return LOCTEXT("ModePlanShort", "计划模式");
	case EWorldDataCodexPermissionMode::Focus:
		return LOCTEXT("ModeFocusShort", "专注模式");
	case EWorldDataCodexPermissionMode::Bypass:
		return LOCTEXT("ModeBypassShort", "绕过模式");
	case EWorldDataCodexPermissionMode::Default:
	default:
		return LOCTEXT("ModeDefaultShort", "默认");
	}
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildAgentCombo()
{
	return SNew(SComboButton)
		.ToolTipText(LOCTEXT("AgentComboTooltip", "选择面板对话使用的本地 Agent（ACP 适配器）"))
		.ComboButtonStyle(&ComposerComboButtonStyle)
		.ButtonStyle(&ComposerButtonStyle)
		.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
		.ForegroundColor(FSlateColor(GetPanelTextColor()))
		.ContentPadding(FMargin(8.0f, 3.0f))
		.HasDownArrow(false)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this] { return GetAgentLabel(CurrentAcpAgent); })
				.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("AgentComboChevron", "v"))
				.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
			]
		]
		.MenuContent()
		[
			BuildAgentMenu()
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildAgentMenu()
{
	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
		[
			SNew(SBorder)
			.Padding(FMargin(8.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f, 2.0f, 4.0f, 8.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AgentMenuTitle", "Agent"))
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildAgentMenuItem(EWorldDataAcpAgent::Codex, LOCTEXT("AgentCodexLabel", "Codex"), LOCTEXT("AgentCodexDesc", "通过 codex-acp 适配器接入"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					BuildAgentMenuItem(EWorldDataAcpAgent::Cursor, LOCTEXT("AgentCursorLabel", "Cursor"), LOCTEXT("AgentCursorDesc", "通过 Cursor CLI 的 agent acp 接入"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					BuildAgentMenuItem(EWorldDataAcpAgent::ClaudeCode, LOCTEXT("AgentClaudeLabel", "Claude Code"), LOCTEXT("AgentClaudeDesc", "通过 claude-agent-acp 适配器接入"))
				]
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildAgentMenuItem(EWorldDataAcpAgent Agent, const FText& Label, const FText& Description)
{
	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this, Agent]
		{
			return CurrentAcpAgent == Agent
				? FSlateColor(GetAccentBorderColor())
				: FSlateColor(UEBridgeMCP::Palette::Background());
		})
		.OnMouseButtonDown_Lambda([this, Agent](const FGeometry&, const FPointerEvent&)
		{
			CurrentAcpAgent = Agent;
			NormalizeAgentModels(); // refreshes CurrentModel for the newly selected agent
			// Switching agent/model tears down every live session so the next send in each
			// conversation relaunches on the newly selected adapter/model.
			ApplyAgentSettingsToAllClients();
			SaveSettings();

			// Surface adapter availability immediately: selecting an agent whose ACP adapter
			// is missing would otherwise only fail on the next send.
			const FString AdapterPath = FWorldDataCodexACPClient::FindAdapterBinaryForAgent(CurrentAcpAgent);
			if (AdapterPath.IsEmpty())
			{
				const FString AdapterName = FWorldDataCodexACPClient::GetAdapterBaseName(CurrentAcpAgent);
				FString Hint;
				if (CurrentAcpAgent == EWorldDataAcpAgent::ClaudeCode)
				{
					Hint = FString::Printf(TEXT(" 适配器获取：%s"), *UEBridgeMCP::GetClaudeAcpAdapterUrl());
				}
				else if (CurrentAcpAgent == EWorldDataAcpAgent::Cursor)
				{
					Hint = TEXT(" 请安装 Cursor CLI，并确保 cursor-agent 或 agent 在 PATH 中。");
				}
				SetLastAction(FText::Format(
					LOCTEXT("AgentChangedNoAdapter", "已切换 Agent：{0}，但未找到 {1} 适配器。请安装并加入 PATH、放入插件 Binaries，或设置环境变量。{2}"),
					GetAgentLabel(CurrentAcpAgent), FText::FromString(AdapterName), FText::FromString(Hint)));
			}
			else
			{
				SetLastAction(FText::Format(
					LOCTEXT("AgentChangedOk", "已切换 Agent：{0}（适配器：{1}）"),
					GetAgentLabel(CurrentAcpAgent), FText::FromString(AdapterPath)));
			}
			FSlateApplication::Get().DismissAllMenus();
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, Agent]
			{
				return CurrentAcpAgent == Agent
					? FSlateColor(GetAccentFillColor(0.18f))
					: FSlateColor(UEBridgeMCP::Palette::Background());
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Description)
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
				]
			]
		];
}

FText SUEBridgeMCPPanel::GetAgentLabel(EWorldDataAcpAgent Agent) const
{
	switch (Agent)
	{
	case EWorldDataAcpAgent::Cursor:
		return LOCTEXT("AgentCursorShort", "Cursor");
	case EWorldDataAcpAgent::ClaudeCode:
		return LOCTEXT("AgentClaudeShort", "Claude Code");
	case EWorldDataAcpAgent::Codex:
	default:
		return LOCTEXT("AgentCodexShort", "Codex");
	}
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildModelCombo()
{
	return SNew(SComboButton)
		.ToolTipText_Lambda([this]
		{
			if (CurrentAcpAgent == EWorldDataAcpAgent::ClaudeCode)
			{
				return LOCTEXT("ModelComboTooltipClaude", "选择 Claude Code 使用的模型");
			}
			if (CurrentAcpAgent == EWorldDataAcpAgent::Cursor)
			{
				return LOCTEXT("ModelComboTooltipCursor", "选择 Cursor Agent 使用的模型");
			}
			return LOCTEXT("ModelComboTooltipCodex", "选择 Codex 使用的模型");
		})
		.ComboButtonStyle(&ComposerComboButtonStyle)
		.ButtonStyle(&ComposerButtonStyle)
		.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
		.ForegroundColor(FSlateColor(GetPanelTextColor()))
		.ContentPadding(FMargin(8.0f, 3.0f))
		.HasDownArrow(false)
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this] { return GetModelLabel(GetCurrentAgentModel()); })
				.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelComboChevron", "v"))
				.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
			]
		]
		.OnGetMenuContent(FOnGetContent::CreateSP(this, &SUEBridgeMCPPanel::BuildModelMenu));
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildModelMenu()
{
	TSharedRef<SVerticalBox> Items = SNew(SVerticalBox);
	Items->AddSlot()
	.AutoHeight()
	.Padding(4.0f, 2.0f, 4.0f, 8.0f)
	[
		SNew(STextBlock)
		.Text(GetModelMenuTitle())
		.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
	];

	bool bFirstModel = true;
	auto AddModel = [this, &Items, &bFirstModel](const FString& ModelId, const FText& Label, const FText& Description)
	{
		Items->AddSlot()
		.AutoHeight()
		.Padding(0.0f, bFirstModel ? 0.0f : 2.0f, 0.0f, 0.0f)
		[
			BuildModelMenuItem(ModelId, Label, Description)
		];
		bFirstModel = false;
	};

	switch (CurrentAcpAgent)
	{
	case EWorldDataAcpAgent::Cursor:
		AddModel(FString(), LOCTEXT("ModelDefaultLabelCursor", "Cursor 默认"), LOCTEXT("ModelDefaultDescCursor", "不传 --model，使用 Cursor Agent 自身默认"));
		AddModel(TEXT("gpt-5"), LOCTEXT("ModelCursorGpt5Label", "GPT-5"), LOCTEXT("ModelCursorGpt5Desc", "Cursor Agent 的 GPT-5 模型"));
		AddModel(TEXT("sonnet-4"), LOCTEXT("ModelCursorSonnetLabel", "Sonnet 4"), LOCTEXT("ModelCursorSonnetDesc", "Cursor Agent 的 Sonnet 4 模型"));
		AddModel(TEXT("sonnet-4-thinking"), LOCTEXT("ModelCursorSonnetThinkingLabel", "Sonnet 4 Thinking"), LOCTEXT("ModelCursorSonnetThinkingDesc", "更偏深度推理的 Sonnet 4"));
		break;
	case EWorldDataAcpAgent::ClaudeCode:
		AddModel(FString(), LOCTEXT("ModelDefaultLabelClaude", "Claude 默认"), LOCTEXT("ModelDefaultDescClaude", "不设置 ANTHROPIC_MODEL，使用 Claude Code 自身默认"));
		AddModel(TEXT("claude-opus-4-8"), LOCTEXT("ModelOpusLabel", "Opus 4.8"), LOCTEXT("ModelOpusDesc", "最强推理，适合复杂 UE 任务"));
		AddModel(TEXT("claude-sonnet-4-6"), LOCTEXT("ModelSonnetLabel", "Sonnet 4.6"), LOCTEXT("ModelSonnetDesc", "平衡速度与质量"));
		AddModel(TEXT("claude-haiku-4-5-20251001"), LOCTEXT("ModelHaikuLabel", "Haiku 4.5"), LOCTEXT("ModelHaikuDesc", "最快最省，适合简单操作"));
		break;
	case EWorldDataAcpAgent::Codex:
	default:
		AddModel(FString(), LOCTEXT("ModelDefaultLabelCodex", "Codex 默认"), LOCTEXT("ModelDefaultDescCodex", "不传 --model，使用 Codex 自身默认"));
		AddModel(TEXT("gpt-5.5"), LOCTEXT("ModelCodexGpt55Label", "GPT-5.5"), LOCTEXT("ModelCodexGpt55Desc", "最强编码与复杂任务模型"));
		AddModel(TEXT("gpt-5.4"), LOCTEXT("ModelCodexGpt54Label", "GPT-5.4"), LOCTEXT("ModelCodexGpt54Desc", "适合日常编码与编辑任务"));
		AddModel(TEXT("gpt-5.4-mini"), LOCTEXT("ModelCodexGpt54MiniLabel", "GPT-5.4-Mini"), LOCTEXT("ModelCodexGpt54MiniDesc", "更快、更省的轻量任务模型"));
		AddModel(TEXT("gpt-5.3-codex-spark"), LOCTEXT("ModelCodexSparkLabel", "GPT-5.3-Codex-Spark"), LOCTEXT("ModelCodexSparkDesc", "超快编码模型"));
		break;
	}

	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
		[
			SNew(SBorder)
			.Padding(FMargin(8.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			[
				Items
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildModelMenuItem(const FString& ModelId, const FText& Label, const FText& Description)
{
	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this, ModelId]
		{
			return GetCurrentAgentModel() == ModelId
				? FSlateColor(GetAccentBorderColor())
				: FSlateColor(UEBridgeMCP::Palette::Background());
		})
		.OnMouseButtonDown_Lambda([this, ModelId, Label](const FGeometry&, const FPointerEvent&)
		{
			SetCurrentAgentModel(ModelId);
			// Changing the model tears down every live session so the next send relaunches on it.
			ApplyAgentSettingsToAllClients();
			SaveSettings();
			SetLastAction(FText::Format(LOCTEXT("ModelChangedOk", "已选择模型：{0}（下次发送时生效）"), Label));
			FSlateApplication::Get().DismissAllMenus();
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, ModelId]
			{
				return GetCurrentAgentModel() == ModelId
					? FSlateColor(GetAccentFillColor(0.18f))
					: FSlateColor(UEBridgeMCP::Palette::Background());
			})
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Description)
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
				]
			]
		];
}

FText SUEBridgeMCPPanel::GetModelLabel(const FString& ModelId) const
{
	if (ModelId.IsEmpty()) { return LOCTEXT("ModelDefaultShort", "默认模型"); }
	if (ModelId == TEXT("gpt-5.5")) { return LOCTEXT("ModelCodexGpt55Short", "GPT-5.5"); }
	if (ModelId == TEXT("gpt-5.4")) { return LOCTEXT("ModelCodexGpt54Short", "GPT-5.4"); }
	if (ModelId == TEXT("gpt-5.4-mini")) { return LOCTEXT("ModelCodexGpt54MiniShort", "GPT-5.4-Mini"); }
	if (ModelId == TEXT("gpt-5.3-codex-spark")) { return LOCTEXT("ModelCodexSparkShort", "Spark"); }
	if (ModelId == TEXT("gpt-5")) { return LOCTEXT("ModelCursorGpt5Short", "GPT-5"); }
	if (ModelId == TEXT("sonnet-4")) { return LOCTEXT("ModelCursorSonnetShort", "Sonnet 4"); }
	if (ModelId == TEXT("sonnet-4-thinking")) { return LOCTEXT("ModelCursorSonnetThinkingShort", "Sonnet 4 Thinking"); }
	if (ModelId == TEXT("claude-opus-4-8")) { return LOCTEXT("ModelOpusShort", "Opus 4.8"); }
	if (ModelId == TEXT("claude-sonnet-4-6")) { return LOCTEXT("ModelSonnetShort", "Sonnet 4.6"); }
	if (ModelId.StartsWith(TEXT("claude-haiku-4-5"))) { return LOCTEXT("ModelHaikuShort", "Haiku 4.5"); }
	return FText::FromString(ModelId);
}

FText SUEBridgeMCPPanel::GetModelMenuTitle() const
{
	switch (CurrentAcpAgent)
	{
	case EWorldDataAcpAgent::Cursor:
		return LOCTEXT("ModelMenuTitleCursor", "模型 (Cursor)");
	case EWorldDataAcpAgent::ClaudeCode:
		return LOCTEXT("ModelMenuTitleClaude", "模型 (Claude Code)");
	case EWorldDataAcpAgent::Codex:
	default:
		return LOCTEXT("ModelMenuTitleCodex", "模型 (Codex)");
	}
}

bool SUEBridgeMCPPanel::IsModelValidForAgent(EWorldDataAcpAgent Agent, const FString& ModelId) const
{
	if (ModelId.IsEmpty())
	{
		return true;
	}

	switch (Agent)
	{
	case EWorldDataAcpAgent::Cursor:
		return ModelId == TEXT("gpt-5")
			|| ModelId == TEXT("sonnet-4")
			|| ModelId == TEXT("sonnet-4-thinking");
	case EWorldDataAcpAgent::ClaudeCode:
		return ModelId == TEXT("claude-opus-4-8")
			|| ModelId == TEXT("claude-sonnet-4-6")
			|| ModelId.StartsWith(TEXT("claude-haiku-4-5"));
	case EWorldDataAcpAgent::Codex:
	default:
		return ModelId == TEXT("gpt-5.5")
			|| ModelId == TEXT("gpt-5.4")
			|| ModelId == TEXT("gpt-5.4-mini")
			|| ModelId == TEXT("gpt-5.3-codex-spark");
	}
}

FString SUEBridgeMCPPanel::SanitizeModelForAgent(EWorldDataAcpAgent Agent, const FString& ModelId) const
{
	return IsModelValidForAgent(Agent, ModelId) ? ModelId : FString();
}

void SUEBridgeMCPPanel::NormalizeAgentModels()
{
	CurrentCodexModel = SanitizeModelForAgent(EWorldDataAcpAgent::Codex, CurrentCodexModel);
	CurrentCursorModel = SanitizeModelForAgent(EWorldDataAcpAgent::Cursor, CurrentCursorModel);
	CurrentClaudeModel = SanitizeModelForAgent(EWorldDataAcpAgent::ClaudeCode, CurrentClaudeModel);
	CurrentModel = GetCurrentAgentModel();
}

FString SUEBridgeMCPPanel::GetCurrentAgentModel() const
{
	switch (CurrentAcpAgent)
	{
	case EWorldDataAcpAgent::Cursor:
		return SanitizeModelForAgent(EWorldDataAcpAgent::Cursor, CurrentCursorModel);
	case EWorldDataAcpAgent::ClaudeCode:
		return SanitizeModelForAgent(EWorldDataAcpAgent::ClaudeCode, CurrentClaudeModel);
	case EWorldDataAcpAgent::Codex:
	default:
		return SanitizeModelForAgent(EWorldDataAcpAgent::Codex, CurrentCodexModel);
	}
}

void SUEBridgeMCPPanel::SetCurrentAgentModel(const FString& ModelId)
{
	const FString SanitizedModelId = SanitizeModelForAgent(CurrentAcpAgent, ModelId);
	switch (CurrentAcpAgent)
	{
	case EWorldDataAcpAgent::Cursor:
		CurrentCursorModel = SanitizedModelId;
		break;
	case EWorldDataAcpAgent::ClaudeCode:
		CurrentClaudeModel = SanitizedModelId;
		break;
	case EWorldDataAcpAgent::Codex:
	default:
		CurrentCodexModel = SanitizedModelId;
		break;
	}
	CurrentModel = SanitizedModelId;
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildDateLabel(const FText& Label) const
{
	return SNew(STextBlock)
		.Text(Label)
		.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); });
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildConversationItem(const FText& Title, TAttribute<FText> Age, bool bActive, FOnClicked OnClicked, TAttribute<bool> IsBusy) const
{
	const bool bClickable = OnClicked.IsBound();
	return SNew(SBorder)
		.Padding(0.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.Cursor(bClickable ? EMouseCursor::Hand : EMouseCursor::Default)
		.BorderBackgroundColor_Lambda([this, bActive] { return FSlateColor(bActive ? GetAccentFillColor(0.22f) : FLinearColor::Transparent); })
		.OnMouseButtonDown_Lambda([OnClicked](const FGeometry&, const FPointerEvent&)
		{
			return OnClicked.IsBound() ? OnClicked.Execute() : FReply::Unhandled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(3.0f)
				[
					SNew(SBorder)
					.Padding(0.0f)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this, bActive] { return FSlateColor(bActive ? GetEffectiveAccentColor() : FLinearColor::Transparent); })
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(8.0f, 7.0f, 8.0f, 7.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Title)
					.ColorAndOpacity_Lambda([this, bActive] { return FSlateColor(bActive ? GetEffectiveAccentColor() : GetReadableAccentTextColor()); })
					.Font(bActive ? FAppStyle::GetFontStyle("NormalFontBold") : FAppStyle::GetFontStyle("NormalFont"))
					.AutoWrapText(false)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					// Live "this conversation is running / awaiting approval" marker. Lets the
					// user see background turns without opening each conversation.
					SNew(SBox)
					.WidthOverride(12.0f)
					.HeightOverride(12.0f)
					.VAlign(VAlign_Center)
					.Visibility_Lambda([IsBusy] { return IsBusy.Get() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						SNew(SCircularThrobber)
						.NumPieces(6)
						.Period(0.8f)
						.Radius(5.0f)
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetEffectiveAccentColor()); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Age)
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
				]
			]
	];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildRunningIndicator() const
{
	return SNew(SBorder)
		.Visibility_Lambda([this] { return IsAgentWorking() ? EVisibility::Visible : EVisibility::Collapsed; })
		.Padding(FMargin(10.0f, 7.0f))
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.10f)); })
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.WidthOverride(22.0f)
				.HeightOverride(22.0f)
				[
					SNew(SCircularThrobber)
					.NumPieces(8)
					.Period(0.8f)
					.Radius(6.0f)
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetEffectiveAccentColor()); })
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this] { return GetRunningIndicatorText(); })
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
				.AutoWrapText(true)
			]
		];
}

bool SUEBridgeMCPPanel::IsAgentWorking() const
{
	const FConversationRuntime* Runtime = FindActiveRuntime();
	const bool bClientProcessing = Runtime && Runtime->Client.IsValid() && Runtime->Client->IsProcessing();
	const bool bAssistantStreaming = ActiveAssistantMessageIndex != INDEX_NONE
		&& ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex)
		&& ConversationMessages[ActiveAssistantMessageIndex].bStreaming;
	const bool bThoughtStreaming = ActiveThoughtMessageIndex != INDEX_NONE
		&& ConversationMessages.IsValidIndex(ActiveThoughtMessageIndex)
		&& ConversationMessages[ActiveThoughtMessageIndex].bStreaming;

	return bDetailIsConversation
		&& !ActiveHasPendingPermission()
		&& (bClientProcessing || bAssistantStreaming || bThoughtStreaming);
}

FText SUEBridgeMCPPanel::GetRunningIndicatorText() const
{
	if (ActiveHasPendingPermission())
	{
		return LOCTEXT("AgentPendingPermissionIndicator", "等待权限确认。");
	}

	const FConversationRuntime* Runtime = FindActiveRuntime();
	if (Runtime && Runtime->Client.IsValid() && Runtime->Client->IsProcessing())
	{
		return LOCTEXT("AgentRunningIndicator", "仍在运行中，Claude Code 可能正在读取项目或调用 UEBridgeMCP 工具...");
	}

	return LOCTEXT("AgentStreamingIndicator", "正在生成回复...");
}


#undef LOCTEXT_NAMESPACE
