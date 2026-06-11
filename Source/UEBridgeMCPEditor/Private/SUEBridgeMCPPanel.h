#pragma once

#include "WorldDataCodexACPClient.h"
#include "WorldDataMCPServer.h"
#include "UEBridgeMCPStyle.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "UEBridgeMCPEditor"

class SUEBridgeMCPPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEBridgeMCPPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		LastAction = LOCTEXT("InitialLastAction", "新会话已就绪。");
		LoadSettings();
		ServerPortText = FString::FromInt(FWorldDataMCPServer::IsRunning()
			? FWorldDataMCPServer::GetPort()
			: FWorldDataMCPServer::LoadConfiguredPort());
		RefreshCliDetections();
		ConfigureLightTextBoxStyle();
		ConfigureComposerButtonStyle();
		AcpClient = MakeShared<FWorldDataCodexACPClient>();
		AcpClient->SetPermissionMode(CurrentMode);
		AcpClient->OnText.BindSP(this, &SUEBridgeMCPPanel::HandleAcpText);
		AcpClient->OnStatus.BindSP(this, &SUEBridgeMCPPanel::HandleAcpStatus);
		AcpClient->OnError.BindSP(this, &SUEBridgeMCPPanel::HandleAcpError);
		AcpClient->OnPermission.BindSP(this, &SUEBridgeMCPPanel::HandleAcpPermission);

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

		ResetConversationView();
	}

	~SUEBridgeMCPPanel()
	{
		if (AcpClient.IsValid())
		{
			AcpClient->Stop();
		}
	}

private:
	enum class EConversationMessageRole : uint8
	{
		User,
		Assistant,
		System,
		Tool,
		Error
	};

	enum class ECliTool : uint8
	{
		Codex,
		Cursor
	};

	struct FConversationMessage
	{
		EConversationMessageRole Role = EConversationMessageRole::Assistant;
		FString Text;
		bool bStreaming = false;
	};

	struct FConversation
	{
		FText Title;
		FDateTime CreatedAt = FDateTime::Now();
		bool bHasCustomTitle = false;
		TArray<FConversationMessage> Messages;
		FString Transcript;
		int32 ActiveAssistantMessageIndex = INDEX_NONE;
	};

	TSharedRef<SWidget> BuildUEBridgeStyleLayout()
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

	TSharedRef<SWidget> BuildUEBridgeTopBar()
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
							BuildBadge(LOCTEXT("CodexBadge", "Codex"))
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

	TSharedRef<SWidget> BuildUEBridgeSidebar()
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

	TSharedRef<SWidget> BuildUEBridgeMainArea()
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
						BuildComposer()
					]
				]
			];
	}
	TSharedRef<SWidget> BuildBadge(const FText& Text) const
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

	TSharedRef<SWidget> BuildToolbarButton(const FText& Label, FOnClicked OnClicked, TAttribute<bool> bIsEnabled = TAttribute<bool>(true)) const
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

	TSharedRef<SWidget> BuildIconTextButton(const FText& Label, FOnClicked OnClicked, const FText& Tooltip = FText::GetEmpty()) const
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

	TSharedRef<SWidget> BuildModeCombo()
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

	TSharedRef<SWidget> BuildModeMenu()
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
						BuildModeMenuItem(EWorldDataCodexPermissionMode::Bypass, LOCTEXT("ModeBypassLabel", "绕过模式"), LOCTEXT("ModeBypassDesc", "自动允许 MCP，Shell 禁用"))
					]
				]
			];
	}

	TSharedRef<SWidget> BuildModeMenuItem(EWorldDataCodexPermissionMode Mode, const FText& Label, const FText& Description)
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
				CurrentMode = Mode;
				if (AcpClient.IsValid())
				{
					AcpClient->SetPermissionMode(CurrentMode);
				}
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

	FText GetModeLabel(EWorldDataCodexPermissionMode Mode) const
	{
		switch (Mode)
		{
		case EWorldDataCodexPermissionMode::Plan:
			return LOCTEXT("ModePlanShort", "计划模式");
		case EWorldDataCodexPermissionMode::Bypass:
			return LOCTEXT("ModeBypassShort", "绕过模式");
		case EWorldDataCodexPermissionMode::Default:
		default:
			return LOCTEXT("ModeDefaultShort", "默认");
		}
	}

	TSharedRef<SWidget> BuildDateLabel(const FText& Label) const
	{
		return SNew(STextBlock)
			.Text(Label)
			.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); });
	}

	TSharedRef<SWidget> BuildConversationItem(const FText& Title, TAttribute<FText> Age, bool bActive, FOnClicked OnClicked = FOnClicked()) const
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
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(Age)
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
					]
				]
			];
	}

	TSharedRef<SWidget> BuildConversationDetail()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SAssignNew(DetailTitleText, STextBlock)
					.Text(LOCTEXT("InitialDetailTitle", "项目信息"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					BuildToolbarButton(LOCTEXT("CopyCurrentButton", "复制当前内容"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnCopyCurrentClicked))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this] { return bHasPendingPermission ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					BuildPermissionRequestCard()
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(ContentSwitcher, SWidgetSwitcher)
				.WidgetIndex_Lambda([this] { return bDetailIsConversation ? 0 : 1; })
				+ SWidgetSwitcher::Slot()
				[
					BuildConversationMessagesView()
				]
				+ SWidgetSwitcher::Slot()
				[
					SAssignNew(DetailTextBox, SMultiLineEditableTextBox)
					.IsReadOnly(true)
					.AutoWrapText(false)
					.Style(&LightTextBoxStyle)
					.BackgroundColor(FSlateColor(GetPanelBackgroundColor()))
					.ForegroundColor(FSlateColor(GetPanelTextColor()))
					.ReadOnlyForegroundColor(FSlateColor(GetPanelTextColor()))
					.Font(FAppStyle::GetFontStyle("MonospacedText"))
				]
			];
	}

	TSharedRef<SWidget> BuildConversationMessagesView()
	{
		return SNew(SBorder)
			.Padding(FMargin(0.0f, 4.0f))
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			[
				SAssignNew(ConversationScrollBox, SScrollBox)
			];
	}

	TSharedRef<SWidget> BuildConversationMessageWidget(const FConversationMessage& Message) const
	{
		const bool bUser = Message.Role == EConversationMessageRole::User;
		const EHorizontalAlignment BubbleAlign = bUser ? HAlign_Right : HAlign_Left;
		const FString DisplayText = Message.Text.IsEmpty() && Message.bStreaming
			? FString(TEXT("正在思考..."))
			: Message.Text;

		return SNew(SBox)
			.HAlign(BubbleAlign)
			[
				SNew(SBox)
				.MaxDesiredWidth(860.0f)
				[
					SNew(SBorder)
					.Padding(1.0f)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FSlateColor(GetConversationMessageBorderColor(Message.Role)))
					[
						SNew(SBorder)
						.Padding(FMargin(12.0f, 9.0f))
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FSlateColor(GetConversationMessageBackgroundColor(Message.Role)))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot()
							.AutoHeight()
							[
								SNew(STextBlock)
								.Text(GetConversationRoleLabel(Message.Role))
								.ColorAndOpacity(FSlateColor(GetConversationMessageMutedColor(Message.Role)))
								.Font(FAppStyle::GetFontStyle("NormalFontBold"))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 4.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text(FText::FromString(DisplayText))
								.ColorAndOpacity(FSlateColor(GetConversationMessageTextColor(Message.Role)))
								.AutoWrapText(true)
							]
						]
					]
				]
			];
	}

	FText GetConversationRoleLabel(EConversationMessageRole Role) const
	{
		switch (Role)
		{
		case EConversationMessageRole::User:
			return LOCTEXT("ConversationUserRole", "你");
		case EConversationMessageRole::System:
			return LOCTEXT("ConversationSystemRole", "系统");
		case EConversationMessageRole::Tool:
			return LOCTEXT("ConversationToolRole", "工具");
		case EConversationMessageRole::Error:
			return LOCTEXT("ConversationErrorRole", "错误");
		case EConversationMessageRole::Assistant:
		default:
			return LOCTEXT("ConversationAssistantRole", "Codex");
		}
	}

	FLinearColor GetConversationMessageBackgroundColor(EConversationMessageRole Role) const
	{
		switch (Role)
		{
		case EConversationMessageRole::User:
			return GetAccentFillColor(0.16f);
		case EConversationMessageRole::System:
			return UEBridgeMCP::Palette::Surface();
		case EConversationMessageRole::Tool:
			return UEBridgeMCP::Palette::SurfaceRaised();
		case EConversationMessageRole::Error:
			return FLinearColor(1.0f, 0.94f, 0.94f, 1.0f);
		case EConversationMessageRole::Assistant:
		default:
			return UEBridgeMCP::Palette::Background();
		}
	}

	FLinearColor GetConversationMessageBorderColor(EConversationMessageRole Role) const
	{
		switch (Role)
		{
		case EConversationMessageRole::User:
			return GetAccentFillColor(0.32f);
		case EConversationMessageRole::Error:
			return FLinearColor(0.96f, 0.68f, 0.68f, 1.0f);
		case EConversationMessageRole::Tool:
			return UEBridgeMCP::Palette::BorderStrong();
		case EConversationMessageRole::System:
		case EConversationMessageRole::Assistant:
		default:
			return UEBridgeMCP::Palette::Border();
		}
	}

	FLinearColor GetConversationMessageTextColor(EConversationMessageRole Role) const
	{
		return Role == EConversationMessageRole::Error
			? UEBridgeMCP::Palette::Danger()
			: GetPanelTextColor();
	}

	FLinearColor GetConversationMessageMutedColor(EConversationMessageRole Role) const
	{
		switch (Role)
		{
		case EConversationMessageRole::Error:
			return UEBridgeMCP::Palette::Danger();
		case EConversationMessageRole::Tool:
			return UEBridgeMCP::Palette::Primary();
		default:
			return GetPanelMutedTextColor();
		}
	}

	TSharedRef<SWidget> BuildPermissionRequestCard()
	{
		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentBorderColor()); })
			[
				SNew(SBorder)
				.Padding(FMargin(12.0f, 10.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentSurfaceColor()); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("PermissionCardTitle", "需要权限"))
							.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 3.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]
							{
								const FString DisplayTitle = PendingPermissionTitle.IsEmpty()
									? FString(TEXT("Codex 请求执行一个 MCP 工具。"))
									: PendingPermissionTitle;
								return FText::FromString(DisplayTitle);
							})
							.ColorAndOpacity(FSlateColor(UEBridgeMCP::Palette::TextSoft()))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 2.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]
							{
								const FString DisplayTool = PendingPermissionToolName.IsEmpty()
									? FString(TEXT("默认模式会等待你确认后继续。"))
									: FString::Printf(TEXT("工具：%s"), *PendingPermissionToolName);
								return FText::FromString(DisplayTool);
							})
							.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 6.0f, 0.0f)
					[
						BuildPermissionActionButton(
							LOCTEXT("PermissionAllowButton", "允许"),
							true,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnAllowPermissionClicked))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						BuildPermissionActionButton(
							LOCTEXT("PermissionDenyButton", "拒绝"),
							false,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnDenyPermissionClicked))
					]
				]
			];
	}

	TSharedRef<SWidget> BuildPermissionActionButton(const FText& Label, bool bPrimary, FOnClicked OnClicked) const
	{
		return SNew(SButton)
			.ButtonStyle(&ComposerButtonStyle)
			.ButtonColorAndOpacity_Lambda([this, bPrimary]
			{
				return FSlateColor(bPrimary ? GetAccentButtonColor() : GetAccentControlColor());
			})
			.ForegroundColor_Lambda([this, bPrimary]
			{
				return FSlateColor(bPrimary ? GetAccentButtonTextColor() : GetPanelTextColor());
			})
			.ContentPadding(FMargin(12.0f, 4.0f))
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.ToolTipText(Label)
			.OnClicked(OnClicked)
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity_Lambda([this, bPrimary]
				{
					return FSlateColor(bPrimary ? GetAccentButtonTextColor() : GetPanelTextColor());
				})
			];
	}

	TSharedRef<SWidget> BuildSettingsPanel()
	{
		return SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				BuildSettingsContent()
			];
	}

	TSharedRef<SWidget> BuildSettingsContent()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 14.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SettingsTitle", "设置"))
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SettingsSubtitle", "调整当前 WorldData MCP 面板的本地偏好。"))
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					BuildToolbarButton(LOCTEXT("SettingsBackButton", "返回对话"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSettingsBackClicked))
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 12.0f)
			[
				BuildMcpServerPanel()
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.Padding(12.0f)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentSurfaceColor()); })
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("SettingsColorLabel", "Color"))
							.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
							.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 4.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("SettingsColorDescription", "默认白色，用于面板点缀色偏好。"))
							.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 10.0f, 0.0f, 0.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
							.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.18f)); })
							[
								SNew(SBox)
								.HeightOverride(18.0f)
								.WidthOverride(280.0f)
							]
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 10.0f, 0.0f, 0.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								BuildColorPresetButton(FLinearColor::White, LOCTEXT("PresetWhiteTooltip", "白色"))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								BuildColorPresetButton(FLinearColor(0.18f, 0.48f, 1.0f, 1.0f), LOCTEXT("PresetBlueTooltip", "蓝色"))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								BuildColorPresetButton(FLinearColor(0.10f, 0.72f, 0.36f, 1.0f), LOCTEXT("PresetGreenTooltip", "绿色"))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								BuildColorPresetButton(FLinearColor(1.0f, 0.74f, 0.14f, 1.0f), LOCTEXT("PresetYellowTooltip", "黄色"))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.Padding(0.0f, 0.0f, 6.0f, 0.0f)
							[
								BuildColorPresetButton(FLinearColor(0.95f, 0.20f, 0.24f, 1.0f), LOCTEXT("PresetRedTooltip", "红色"))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							[
								BuildColorPresetButton(FLinearColor(0.62f, 0.36f, 1.0f, 1.0f), LOCTEXT("PresetPurpleTooltip", "紫色"))
							]
						]
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(SColorBlock)
						.Color_Lambda([this] { return SettingsColor; })
						.Size(FVector2D(42.0f, 22.0f))
						.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Ignore)
						.OnMouseButtonDown(this, &SUEBridgeMCPPanel::OnSettingsColorBlockClicked)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 10.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text_Lambda([this] { return GetSettingsColorText(); })
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						BuildToolbarButton(LOCTEXT("PickSettingsColorButton", "选择颜色"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnPickSettingsColorClicked))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						BuildToolbarButton(LOCTEXT("ResetSettingsColorButton", "重置"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnResetSettingsColorClicked))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				BuildCliSettingsPanel()
			];
	}

	TSharedRef<SWidget> BuildMcpServerPanel()
	{
		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentSurfaceColor()); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpServerTitle", "MCP 服务器"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpServerSubtitle", "Unreal Engine 连接状态。"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					BuildServerStatusCard()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					BuildServerClientsBanner()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					BuildServerPortCard()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					BuildRegisteredToolsCard()
				]
			];
	}

	TSharedRef<SWidget> BuildServerStatusCard()
	{
		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.10f)); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpStatusLabel", "状态"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(0.0f, 0.0f, 8.0f, 0.0f)
					[
						SNew(SColorBlock)
						.Color_Lambda([this]
						{
							return FWorldDataMCPServer::IsRunning()
								? FLinearColor(0.10f, 0.72f, 0.36f, 1.0f)
								: FLinearColor(0.60f, 0.60f, 0.62f, 1.0f);
						})
						.Size(FVector2D(9.0f, 9.0f))
						.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Ignore)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this] { return GetServerStatusText(); })
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.ButtonStyle(&ComposerButtonStyle)
						.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
						.ForegroundColor(FSlateColor(GetPanelTextColor()))
						.Text_Lambda([this] { return GetServerToggleText(); })
						.OnClicked(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnToggleServerClicked))
					]
				]
			];
	}

	TSharedRef<SWidget> BuildServerClientsBanner()
	{
		return SNew(SBorder)
			.Padding(FMargin(12.0f, 8.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.14f)); })
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("McpClientsBanner", "支持 Claude Code、Cursor、Windsurf 及任意 MCP 客户端。"))
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
			];
	}

	TSharedRef<SWidget> BuildServerPortCard()
	{
		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.10f)); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpPortLabel", "端口"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpPortDescription", "MCP 服务器端口号。"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(SEditableTextBox)
						.Style(&LightTextBoxStyle)
						.IsEnabled_Lambda([this] { return !FWorldDataMCPServer::IsRunning(); })
						.Text_Lambda([this] { return FText::FromString(ServerPortText); })
						.HintText(LOCTEXT("McpPortHint", "例如 7275"))
						.OnTextChanged_Lambda([this](const FText& Text) { ServerPortText = Text.ToString(); })
						.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type) { ServerPortText = Text.ToString(); })
						.SelectAllTextWhenFocused(true)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						BuildToolbarButton(
							LOCTEXT("McpPortApplyButton", "应用"),
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnApplyPortClicked),
							TAttribute<bool>::CreateLambda([this] { return !FWorldDataMCPServer::IsRunning(); }))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 7.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpPortNeedsStopHint", "请先停止服务器再更改端口。"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
				]
			];
	}

	TSharedRef<SWidget> BuildRegisteredToolsCard()
	{
		const TArray<FString>& ToolNames = GetRegisteredToolNames();

		TSharedRef<SWrapBox> Chips = SNew(SWrapBox)
			.UseAllottedSize(true);
		for (const FString& ToolName : ToolNames)
		{
			Chips->AddSlot()
				.Padding(0.0f, 0.0f, 6.0f, 6.0f)
				[
					BuildToolChip(ToolName)
				];
		}

		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.10f)); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::Format(LOCTEXT("McpToolsLabel", "已注册工具 ({0})"), FText::AsNumber(ToolNames.Num())))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("McpToolsSubtitle", "通过 MCP 服务器可用的 Unreal Engine 工具。"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 10.0f, 0.0f, 0.0f)
				[
					Chips
				]
			];
	}

	TSharedRef<SWidget> BuildToolChip(const FString& ToolName)
	{
		return SNew(SBorder)
			.Padding(FMargin(8.0f, 4.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.18f)); })
			[
				SNew(STextBlock)
				.Text(FText::FromString(ToolName))
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
			];
	}

	FText GetServerStatusText() const
	{
		if (FWorldDataMCPServer::IsRunning())
		{
			return FText::Format(
				LOCTEXT("McpStatusRunning", "运行中 — port {0}"),
				FText::FromString(FString::FromInt(FWorldDataMCPServer::GetPort())));
		}
		return LOCTEXT("McpStatusStopped", "已停止");
	}

	FText GetServerToggleText() const
	{
		return FWorldDataMCPServer::IsRunning()
			? LOCTEXT("McpToggleStop", "停止")
			: LOCTEXT("McpToggleStart", "启动");
	}

	int32 ParseServerPort() const
	{
		const int32 Port = FCString::Atoi(*ServerPortText);
		return (Port >= 1024 && Port <= 65535) ? Port : FWorldDataMCPServer::LoadConfiguredPort();
	}

	const TArray<FString>& GetRegisteredToolNames()
	{
		if (CachedToolNames.Num() == 0)
		{
			const FString Json = FWorldDataMCPServer::GetToolDefinitionsJson();
			TArray<TSharedPtr<FJsonValue>> Tools;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			if (FJsonSerializer::Deserialize(Reader, Tools))
			{
				for (const TSharedPtr<FJsonValue>& Value : Tools)
				{
					const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
					FString Name;
					if (Object.IsValid() && Object->TryGetStringField(TEXT("name"), Name))
					{
						CachedToolNames.Add(Name);
					}
				}
			}
		}
		return CachedToolNames;
	}

	FReply OnToggleServerClicked()
	{
		if (FWorldDataMCPServer::IsRunning())
		{
			FWorldDataMCPServer::Stop();
			SetLastAction(LOCTEXT("McpStoppedAction", "已停止 MCP 服务器。"));
			return FReply::Handled();
		}

		FWorldDataMCPServer::Start(ParseServerPort());
		if (FWorldDataMCPServer::IsRunning())
		{
			FWorldDataMCPServer::RefreshConnectionFiles();
			ServerPortText = FString::FromInt(FWorldDataMCPServer::GetPort());
			SetLastAction(LOCTEXT("McpStartedAction", "已启动 MCP 服务器。"));
		}
		else
		{
			SetLastAction(LOCTEXT("McpStartFailedAction", "MCP 服务器启动失败。"));
		}
		return FReply::Handled();
	}

	FReply OnApplyPortClicked()
	{
		if (FWorldDataMCPServer::IsRunning())
		{
			SetLastAction(LOCTEXT("McpPortNeedsStopAction", "请先停止服务器再更改端口。"));
			return FReply::Handled();
		}

		FWorldDataMCPServer::Start(ParseServerPort());
		if (FWorldDataMCPServer::IsRunning())
		{
			FWorldDataMCPServer::RefreshConnectionFiles();
			ServerPortText = FString::FromInt(FWorldDataMCPServer::GetPort());
			SetLastAction(FText::Format(
				LOCTEXT("McpPortAppliedAction", "已在端口 {0} 启动。"),
				FText::FromString(FString::FromInt(FWorldDataMCPServer::GetPort()))));
		}
		else
		{
			SetLastAction(LOCTEXT("McpPortApplyFailedAction", "应用端口失败，服务器未能启动。"));
		}
		return FReply::Handled();
	}

	TSharedRef<SWidget> BuildCliSettingsPanel()
	{
		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentSurfaceColor()); })
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CliSettingsTitle", "CLI 支持"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 12.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CliSettingsSubtitle", "配置本机 Codex CLI 与 Cursor Agent CLI。未填写时会尝试从 PATH 自动发现。"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildCliSettingsRow(ECliTool::Codex)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 12.0f)
				[
					SNew(SSeparator)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildCliSettingsRow(ECliTool::Cursor)
				]
			];
	}

	TSharedRef<SWidget> BuildCliSettingsRow(ECliTool Tool)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(GetCliTitle(Tool))
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Text(GetCliDescription(Tool))
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					BuildCliStatusBadge(Tool)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SEditableTextBox)
					.Style(&LightTextBoxStyle)
					.Text_Lambda([this, Tool] { return FText::FromString(GetCliConfiguredPath(Tool)); })
					.HintText_Lambda([this, Tool]
					{
						return FText::FromString(FString::Printf(TEXT("留空时自动使用 PATH 中的 %s"), *GetCliCommandName(Tool)));
					})
					.OnTextCommitted_Lambda([this, Tool](const FText& Text, ETextCommit::Type)
					{
						SetCliConfiguredPath(Tool, Text.ToString());
					})
					.SelectAllTextWhenFocused(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					BuildToolbarButton(LOCTEXT("DetectCliButton", "自动检测"), FOnClicked::CreateLambda([this, Tool] { return OnDetectCliClicked(Tool); }))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					BuildToolbarButton(LOCTEXT("ClearCliButton", "清空"), FOnClicked::CreateLambda([this, Tool] { return OnClearCliClicked(Tool); }))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this, Tool] { return IsCliAvailable(Tool) ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						BuildToolbarButton(LOCTEXT("DownloadCliButton", "下载"), FOnClicked::CreateLambda([this, Tool] { return OnDownloadCliClicked(Tool); }))
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 7.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda([this, Tool] { return GetCliPathSummary(Tool); })
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
			];
	}

	TSharedRef<SWidget> BuildCliStatusBadge(ECliTool Tool) const
	{
		return SNew(SBorder)
			.Padding(FMargin(8.0f, 2.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, Tool]
			{
				return FSlateColor(UEBridgeMCP::Palette::Blend(
					GetPanelBackgroundColor(),
					IsCliAvailable(Tool) ? UEBridgeMCP::Palette::Success() : UEBridgeMCP::Palette::Warning(),
					0.14f));
			})
			[
				SNew(STextBlock)
				.Text_Lambda([this, Tool]
				{
					return IsCliAvailable(Tool) ? LOCTEXT("CliFoundStatus", "已找到") : LOCTEXT("CliMissingStatus", "未找到");
				})
				.ColorAndOpacity_Lambda([this, Tool]
				{
					return FSlateColor(IsCliAvailable(Tool) ? UEBridgeMCP::Palette::Success() : UEBridgeMCP::Palette::Warning());
				})
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			];
	}

	TSharedRef<SWidget> BuildComposer()
	{
		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentBorderColor()); })
			[
				SNew(SBorder)
				.Padding(FMargin(12.0f, 8.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.MinDesiredHeight(42.0f)
						[
							SAssignNew(ComposerTextBox, SMultiLineEditableTextBox)
							.Style(&LightTextBoxStyle)
							.HintText(LOCTEXT("ComposerHint", "输入消息...（Ctrl+V 粘贴图片，@ 附加文件）"))
							.AutoWrapText(true)
							.BackgroundColor(FSlateColor(GetPanelBackgroundColor()))
							.ForegroundColor(FSlateColor(GetPanelTextColor()))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 12.0f, 0.0f)
						[
							BuildIconTextButton(LOCTEXT("AttachButton", "+"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnOpenProjectFolderClicked), LOCTEXT("AttachTooltip", "打开项目目录"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 16.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ModelText", "GPT-5.5"))
							.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 16.0f, 0.0f)
						[
							BuildModeCombo()
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ExtensionText", "扩展"))
							.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
						]
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						[
							SNullWidget::NullWidget
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(SButton)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							.ContentPadding(FMargin(9.0f, 3.0f))
							.ButtonStyle(&ComposerButtonStyle)
							.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonColor()); })
							.ForegroundColor_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
							.ToolTipText(LOCTEXT("SendTooltip", "发送到 Codex ACP"))
							.OnClicked(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSendClicked))
							[
								SNew(STextBlock)
								.Text(LOCTEXT("SendButtonArrow", ">"))
								.ColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
							]
						]
					]
				]
			];
	}

	void ConfigureLightTextBoxStyle()
	{
		LightTextBoxStyle = FAppStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
		if (const FSlateBrush* WhiteBrush = FAppStyle::GetBrush("WhiteBrush"))
		{
			FSlateBrush NormalBrush = *WhiteBrush;
			NormalBrush.TintColor = FSlateColor(GetPanelBackgroundColor());

			FSlateBrush HoveredBrush = *WhiteBrush;
			HoveredBrush.TintColor = FSlateColor(GetPanelSurfaceColor());

			FSlateBrush FocusedBrush = *WhiteBrush;
			FocusedBrush.TintColor = FSlateColor(GetPanelBackgroundColor());

			LightTextBoxStyle
				.SetBackgroundImageNormal(NormalBrush)
				.SetBackgroundImageHovered(HoveredBrush)
				.SetBackgroundImageFocused(FocusedBrush)
				.SetBackgroundImageReadOnly(NormalBrush);
		}

		FTextBlockStyle TextStyle = LightTextBoxStyle.TextStyle;
		TextStyle.SetColorAndOpacity(FSlateColor(GetPanelTextColor()));
		LightTextBoxStyle
			.SetTextStyle(TextStyle)
			.SetForegroundColor(FSlateColor(GetPanelTextColor()))
			.SetFocusedForegroundColor(FSlateColor(GetPanelTextColor()))
			.SetReadOnlyForegroundColor(FSlateColor(GetPanelTextColor()))
			.SetBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			.SetPadding(FMargin(8.0f, 6.0f));
	}

	void ConfigureComposerButtonStyle()
	{
		ComposerButtonStyle = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
		if (const FSlateBrush* WhiteBrush = FAppStyle::GetBrush("WhiteBrush"))
		{
			FSlateBrush NormalBrush = *WhiteBrush;
			NormalBrush.TintColor = FSlateColor(UEBridgeMCP::Palette::Background());

			FSlateBrush HoveredBrush = *WhiteBrush;
			HoveredBrush.TintColor = FSlateColor(UEBridgeMCP::Palette::ControlHover());

			FSlateBrush PressedBrush = *WhiteBrush;
			PressedBrush.TintColor = FSlateColor(UEBridgeMCP::Palette::ControlPressed());

			FSlateBrush DisabledBrush = *WhiteBrush;
			DisabledBrush.TintColor = FSlateColor(UEBridgeMCP::Palette::Surface());

			ComposerButtonStyle
				.SetNormal(NormalBrush)
				.SetHovered(HoveredBrush)
				.SetPressed(PressedBrush)
				.SetDisabled(DisabledBrush);
		}

		const FSlateColor TextColor(UEBridgeMCP::Palette::Text());
		ComposerButtonStyle
			.SetNormalForeground(TextColor)
			.SetHoveredForeground(TextColor)
			.SetPressedForeground(TextColor)
			.SetDisabledForeground(FSlateColor(UEBridgeMCP::Palette::TextDisabled()))
			.SetNormalPadding(FMargin(0.0f))
			.SetPressedPadding(FMargin(0.0f));

		ComposerComboButtonStyle = FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>("ComboButton");
		ComposerComboButtonStyle
			.SetButtonStyle(ComposerButtonStyle)
			.SetContentPadding(FMargin(0.0f))
			.SetMenuBorderPadding(FMargin(0.0f));
		if (const FSlateBrush* WhiteBrush = FAppStyle::GetBrush("WhiteBrush"))
		{
			FSlateBrush MenuBorderBrush = *WhiteBrush;
			MenuBorderBrush.TintColor = FSlateColor(UEBridgeMCP::Palette::Border());
			ComposerComboButtonStyle.SetMenuBorderBrush(MenuBorderBrush);
		}
	}

	FLinearColor GetAccentFillColor(float Alpha) const
	{
		return UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), GetEffectiveAccentColor(), Alpha);
	}

	FLinearColor GetAccentSurfaceColor() const
	{
		return GetAccentFillColor(0.08f);
	}

	FLinearColor GetAccentControlColor() const
	{
		return GetAccentFillColor(0.12f);
	}

	FLinearColor GetAccentBorderColor() const
	{
		return UEBridgeMCP::Palette::Blend(GetPanelBorderColor(), GetEffectiveAccentColor(), 0.40f);
	}

	FLinearColor GetAccentButtonColor() const
	{
		return GetEffectiveAccentColor();
	}

	FLinearColor GetAccentButtonTextColor() const
	{
		const FLinearColor ButtonColor = GetAccentButtonColor();
		const float Luma = ButtonColor.R * 0.2126f + ButtonColor.G * 0.7152f + ButtonColor.B * 0.0722f;
		return Luma > 0.60f ? GetPanelTextColor() : UEBridgeMCP::Palette::OnPrimary();
	}

	FLinearColor GetReadableAccentTextColor() const
	{
		return GetPanelTextColor();
	}

	FLinearColor GetPanelSubduedTextColor() const
	{
		return GetPanelMutedTextColor();
	}

	FLinearColor GetEffectiveAccentColor() const
	{
		return ResolveAccentColor(SettingsColor);
	}

	static FLinearColor ResolveAccentColor(const FLinearColor& Color)
	{
		const float MaxChannel = FMath::Max3(Color.R, Color.G, Color.B);
		const float MinChannel = FMath::Min3(Color.R, Color.G, Color.B);
		const float Luma = Color.R * 0.2126f + Color.G * 0.7152f + Color.B * 0.0722f;
		const bool bLooksLikeWhite = Luma > 0.92f && (MaxChannel - MinChannel) < 0.06f;
		return bLooksLikeWhite ? UEBridgeMCP::Palette::Primary() : Color;
	}

	static FLinearColor GetPanelBackgroundColor()
	{
		return UEBridgeMCP::Palette::Background();
	}

	static FLinearColor GetPanelSurfaceColor()
	{
		return UEBridgeMCP::Palette::Surface();
	}

	static FLinearColor GetPanelBorderColor()
	{
		return UEBridgeMCP::Palette::Border();
	}

	static FLinearColor GetPanelTextColor()
	{
		return UEBridgeMCP::Palette::Text();
	}

	static FLinearColor GetPanelMutedTextColor()
	{
		return UEBridgeMCP::Palette::TextMuted();
	}

	bool IsSettingsColorSelected(const FLinearColor& Color) const
	{
		return FMath::IsNearlyEqual(SettingsColor.R, Color.R, 0.01f)
			&& FMath::IsNearlyEqual(SettingsColor.G, Color.G, 0.01f)
			&& FMath::IsNearlyEqual(SettingsColor.B, Color.B, 0.01f);
	}

	TSharedRef<SWidget> BuildColorPresetButton(const FLinearColor& Color, const FText& Tooltip)
	{
		return SNew(SButton)
			.ContentPadding(FMargin(2.0f))
			.ToolTipText(Tooltip)
			.ButtonColorAndOpacity_Lambda([this, Color]
			{
				const FLinearColor ResolvedAccent = ResolveAccentColor(Color);
				return IsSettingsColorSelected(Color)
					? FSlateColor(UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), ResolvedAccent, 0.30f))
					: FSlateColor(UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), ResolvedAccent, 0.08f));
			})
			.OnClicked_Lambda([this, Color]
			{
				ApplySettingsColor(Color);
				return FReply::Handled();
			})
			[
				SNew(SColorBlock)
				.Color(Color)
				.Size(FVector2D(26.0f, 18.0f))
				.AlphaDisplayMode(EColorBlockAlphaDisplayMode::Ignore)
				.OnMouseButtonDown_Lambda([this, Color](const FGeometry&, const FPointerEvent&)
				{
					ApplySettingsColor(Color);
					return FReply::Handled();
				})
			];
	}

	FText GetCliTitle(ECliTool Tool) const
	{
		return Tool == ECliTool::Codex
			? LOCTEXT("CodexCliTitle", "Codex CLI")
			: LOCTEXT("CursorCliTitle", "Cursor CLI");
	}

	FText GetCliDescription(ECliTool Tool) const
	{
		return Tool == ECliTool::Codex
			? LOCTEXT("CodexCliDescription", "配置 codex 命令路径；当前面板对话仍通过 codex-acp 适配器接入。")
			: LOCTEXT("CursorCliDescription", "配置 Cursor Agent CLI（cursor-agent），并同步写入 .cursor/mcp.json 供 Cursor 读取 MCP。");
	}

	FString GetCliCommandName(ECliTool Tool) const
	{
		return Tool == ECliTool::Codex ? TEXT("codex") : TEXT("cursor-agent");
	}

	FString GetCliConfiguredPath(ECliTool Tool) const
	{
		return Tool == ECliTool::Codex ? CodexCliPath : CursorCliPath;
	}

	FString GetCliDetectedPath(ECliTool Tool) const
	{
		return Tool == ECliTool::Codex ? DetectedCodexCliPath : DetectedCursorCliPath;
	}

	FString GetCliEffectivePath(ECliTool Tool) const
	{
		const FString Configured = UEBridgeMCP::StripOuterQuotes(GetCliConfiguredPath(Tool));
		if (!Configured.IsEmpty())
		{
			return UEBridgeMCP::PathExists(Configured) ? Configured : FString();
		}
		return GetCliDetectedPath(Tool);
	}

	bool IsCliAvailable(ECliTool Tool) const
	{
		return !GetCliEffectivePath(Tool).IsEmpty();
	}

	FText GetCliPathSummary(ECliTool Tool) const
	{
		const FString EffectivePath = GetCliEffectivePath(Tool);
		if (!EffectivePath.IsEmpty())
		{
			return FText::FromString(FString::Printf(TEXT("当前有效路径：%s"), *EffectivePath));
		}

		const FString Configured = UEBridgeMCP::StripOuterQuotes(GetCliConfiguredPath(Tool));
		if (!Configured.IsEmpty())
		{
			return FText::FromString(FString::Printf(TEXT("已填写路径但无法访问：%s"), *Configured));
		}
		return FText::FromString(FString::Printf(TEXT("未检测到 %s。可以自动检测、手动填写路径，或打开下载页。"), *GetCliCommandName(Tool)));
	}

	void RefreshCliDetections()
	{
		DetectedCodexCliPath = UEBridgeMCP::ResolveCommandOnPath(TEXT("codex"));
		DetectedCursorCliPath = UEBridgeMCP::ResolveCommandOnPath(TEXT("cursor-agent"));
	}

	void SetCliConfiguredPath(ECliTool Tool, const FString& NewPath)
	{
		FString Normalized = UEBridgeMCP::StripOuterQuotes(NewPath);
		if (!Normalized.IsEmpty() && !UEBridgeMCP::PathExists(Normalized) && !Normalized.Contains(TEXT("\\")) && !Normalized.Contains(TEXT("/")))
		{
			const FString Resolved = UEBridgeMCP::ResolveCommandOnPath(Normalized);
			if (!Resolved.IsEmpty())
			{
				Normalized = Resolved;
			}
		}
		FPaths::MakePlatformFilename(Normalized);

		if (Tool == ECliTool::Codex)
		{
			CodexCliPath = Normalized;
		}
		else
		{
			CursorCliPath = Normalized;
		}

		SaveSettings();
		SetLastAction(FText::FromString(FString::Printf(TEXT("%s 路径已保存。"), *GetCliTitle(Tool).ToString())));
	}

	FReply OnDetectCliClicked(ECliTool Tool)
	{
		RefreshCliDetections();
		const FString DetectedPath = GetCliDetectedPath(Tool);
		if (DetectedPath.IsEmpty())
		{
			SetLastAction(FText::FromString(FString::Printf(TEXT("未在 PATH 中找到 %s。"), *GetCliCommandName(Tool))));
			return FReply::Handled();
		}

		SetCliConfiguredPath(Tool, DetectedPath);
		SetLastAction(FText::FromString(FString::Printf(TEXT("已检测到 %s：%s"), *GetCliCommandName(Tool), *DetectedPath)));
		return FReply::Handled();
	}

	FReply OnClearCliClicked(ECliTool Tool)
	{
		if (Tool == ECliTool::Codex)
		{
			CodexCliPath.Empty();
		}
		else
		{
			CursorCliPath.Empty();
		}

		RefreshCliDetections();
		SaveSettings();
		SetLastAction(FText::FromString(FString::Printf(TEXT("%s 路径已清空，将重新使用 PATH 自动检测。"), *GetCliTitle(Tool).ToString())));
		return FReply::Handled();
	}

	FReply OnDownloadCliClicked(ECliTool Tool)
	{
		const FString Url = Tool == ECliTool::Codex ? UEBridgeMCP::GetCodexCliInstallUrl() : UEBridgeMCP::GetCursorCliInstallUrl();
		FString Error;
		if (UEBridgeMCP::OpenExternalUrl(Url, Error))
		{
			SetLastAction(FText::FromString(FString::Printf(TEXT("已打开 %s 下载/安装页面。"), *GetCliTitle(Tool).ToString())));
		}
		else
		{
			SetLastAction(FText::FromString(FString::Printf(TEXT("打开下载页面失败：%s"), *Error)));
		}
		return FReply::Handled();
	}

	void LoadSettings()
	{
		SettingsColor = FLinearColor::White;
		CodexCliPath.Empty();
		CursorCliPath.Empty();

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *UEBridgeMCP::GetSettingsFilePath()))
		{
			SaveSettings();
			return;
		}

		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			SaveSettings();
			return;
		}

		const TSharedPtr<FJsonObject>* ColorObjectPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("color"), ColorObjectPtr) && ColorObjectPtr && ColorObjectPtr->IsValid())
		{
			const TSharedPtr<FJsonObject> ColorObject = *ColorObjectPtr;
			auto ReadComponent = [ColorObject](const FString& Field, float DefaultValue)
			{
				double Value = DefaultValue;
				return ColorObject->TryGetNumberField(Field, Value) ? static_cast<float>(Value) : DefaultValue;
			};

			SettingsColor = FLinearColor(
				FMath::Clamp(ReadComponent(TEXT("r"), 1.0f), 0.0f, 1.0f),
				FMath::Clamp(ReadComponent(TEXT("g"), 1.0f), 0.0f, 1.0f),
				FMath::Clamp(ReadComponent(TEXT("b"), 1.0f), 0.0f, 1.0f),
				1.0f);
		}

		const TSharedPtr<FJsonObject>* CliObjectPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("cli"), CliObjectPtr) && CliObjectPtr && CliObjectPtr->IsValid())
		{
			(*CliObjectPtr)->TryGetStringField(TEXT("codexPath"), CodexCliPath);
			(*CliObjectPtr)->TryGetStringField(TEXT("cursorPath"), CursorCliPath);
			CodexCliPath = UEBridgeMCP::StripOuterQuotes(CodexCliPath);
			CursorCliPath = UEBridgeMCP::StripOuterQuotes(CursorCliPath);
		}

		SaveSettings();
	}

	void ApplySettingsColor(const FLinearColor& NewColor)
	{
		SettingsColor = FLinearColor(
			FMath::Clamp(NewColor.R, 0.0f, 1.0f),
			FMath::Clamp(NewColor.G, 0.0f, 1.0f),
			FMath::Clamp(NewColor.B, 0.0f, 1.0f),
			1.0f);
		SaveSettings();
		SetLastAction(FText::Format(LOCTEXT("SettingsColorAppliedAction", "Color 已应用：{0}"), GetSettingsColorText()));
	}

	void SaveSettings() const
	{
		const FString SettingsPath = UEBridgeMCP::GetSettingsFilePath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsPath), true);

		TSharedPtr<FJsonObject> ColorObject = MakeShared<FJsonObject>();
		ColorObject->SetNumberField(TEXT("r"), SettingsColor.R);
		ColorObject->SetNumberField(TEXT("g"), SettingsColor.G);
		ColorObject->SetNumberField(TEXT("b"), SettingsColor.B);

		TSharedPtr<FJsonObject> CliObject = MakeShared<FJsonObject>();
		CliObject->SetStringField(TEXT("codexPath"), CodexCliPath);
		CliObject->SetStringField(TEXT("cursorPath"), CursorCliPath);

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("color"), ColorObject);
		Root->SetObjectField(TEXT("cli"), CliObject);

		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			FFileHelper::SaveStringToFile(Out, *SettingsPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	}

	FText GetSettingsColorText() const
	{
		const FColor Color = SettingsColor.ToFColor(true);
		return FText::FromString(FString::Printf(TEXT("#%02X%02X%02X"), Color.R, Color.G, Color.B));
	}

	void OpenSettingsColorPicker()
	{
		FColorPickerArgs Args;
		Args.InitialColor = SettingsColor;
		Args.ParentWidget = AsShared();
		Args.bIsModal = true;
		Args.bUseAlpha = false;
		Args.bClampValue = true;
		Args.bOnlyRefreshOnMouseUp = false;
		Args.OnColorCommitted = FOnLinearColorValueChanged::CreateSP(this, &SUEBridgeMCPPanel::HandleSettingsColorChanged);
		if (!OpenColorPicker(Args))
		{
			SetLastAction(LOCTEXT("SettingsColorPickerFailedAction", "颜色选择器打开失败。"));
		}
	}

	void HandleSettingsColorChanged(FLinearColor NewColor)
	{
		ApplySettingsColor(NewColor);
	}

	FReply OnSettingsColorBlockClicked(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
	{
		OpenSettingsColorPicker();
		return FReply::Handled();
	}

	FReply OnPickSettingsColorClicked()
	{
		OpenSettingsColorPicker();
		return FReply::Handled();
	}

	FReply OnResetSettingsColorClicked()
	{
		ApplySettingsColor(FLinearColor::White);
		return FReply::Handled();
	}

	FReply OnSettingsBackClicked()
	{
		bShowSettings = false;
		SetLastAction(LOCTEXT("SettingsBackAction", "已返回对话。"));
		return FReply::Handled();
	}

	void SetDetail(const FText& Title, const FString& Text)
	{
		const FString PrettyText = UEBridgeMCP::PrettyJson(Text);
		bShowSettings = false;
		bShowDetail = true;
		bDetailIsConversation = false;
		CurrentDetailText = PrettyText;
		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(Title);
		}
		if (DetailTextBox.IsValid())
		{
			DetailTextBox->SetText(FText::FromString(PrettyText));
		}
	}

	void SetLastAction(const FText& Text)
	{
		LastAction = Text;
	}

	void RebuildConversationMessages()
	{
		if (!ConversationScrollBox.IsValid())
		{
			return;
		}

		ConversationScrollBox->ClearChildren();
		for (const FConversationMessage& Message : ConversationMessages)
		{
			ConversationScrollBox->AddSlot()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 10.0f))
				[
					BuildConversationMessageWidget(Message)
				];
		}
		ConversationScrollBox->ScrollToEnd();
	}

	int32 AddConversationMessage(EConversationMessageRole Role, const FString& Text, bool bStreaming = false)
	{
		FConversationMessage Message;
		Message.Role = Role;
		Message.Text = Text;
		Message.bStreaming = bStreaming;
		const int32 Index = ConversationMessages.Add(MoveTemp(Message));
		RebuildConversationMessages();
		return Index;
	}

	static FString TrimConversationEventText(const FString& Text)
	{
		FString Trimmed = Text;
		Trimmed.TrimStartAndEndInline();
		if (Trimmed.StartsWith(TEXT("[")))
		{
			int32 CloseIndex = INDEX_NONE;
			if (Trimmed.FindChar(TCHAR(']'), CloseIndex))
			{
				const FString EventTag = Trimmed.Mid(1, CloseIndex - 1);
				FString Remainder = Trimmed.Mid(CloseIndex + 1);
				Remainder.TrimStartAndEndInline();
				return Remainder.IsEmpty() ? EventTag : Remainder;
			}
		}
		return Trimmed;
	}

	bool TryExtractConversationEvent(const FString& Text, EConversationMessageRole& OutRole, FString& OutText) const
	{
		FString Trimmed = Text;
		Trimmed.TrimStartAndEndInline();
		if (!Trimmed.StartsWith(TEXT("[")))
		{
			return false;
		}

		int32 CloseIndex = INDEX_NONE;
		if (!Trimmed.FindChar(TCHAR(']'), CloseIndex))
		{
			return false;
		}

		const FString EventTag = Trimmed.Mid(1, CloseIndex - 1);
		if (EventTag.Contains(TEXT("错误")))
		{
			OutRole = EConversationMessageRole::Error;
		}
		else if (EventTag.Contains(TEXT("工具")))
		{
			OutRole = EConversationMessageRole::Tool;
		}
		else if (EventTag.Contains(TEXT("系统")))
		{
			OutRole = EConversationMessageRole::System;
		}
		else
		{
			return false;
		}

		OutText = TrimConversationEventText(Trimmed);
		return true;
	}

	void StartConversationTurn(const FString& UserMessage)
	{
		if (ActiveAssistantMessageIndex != INDEX_NONE && ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
		{
			ConversationMessages[ActiveAssistantMessageIndex].bStreaming = false;
		}
		ActiveAssistantMessageIndex = INDEX_NONE;

		AddConversationMessage(EConversationMessageRole::User, UserMessage);

		if (Conversations.IsValidIndex(ActiveConversationIndex) && !Conversations[ActiveConversationIndex].bHasCustomTitle)
		{
			Conversations[ActiveConversationIndex].Title = MakeConversationTitle(UserMessage);
			Conversations[ActiveConversationIndex].bHasCustomTitle = true;
			RebuildSidebar();
		}

		if (!ConversationTranscript.IsEmpty())
		{
			ConversationTranscript += TEXT("\n\n");
		}
		ConversationTranscript += FString::Printf(TEXT("你：%s"), *UserMessage);
		RefreshConversationText();
	}

	void AppendAssistantText(const FString& Text)
	{
		if (ActiveAssistantMessageIndex == INDEX_NONE || !ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
		{
			if (!ConversationTranscript.IsEmpty())
			{
				ConversationTranscript += TEXT("\n\nCodex：");
			}
			ActiveAssistantMessageIndex = AddConversationMessage(EConversationMessageRole::Assistant, FString(), true);
		}

		ConversationMessages[ActiveAssistantMessageIndex].Text += Text;
		ConversationMessages[ActiveAssistantMessageIndex].bStreaming = true;
		ConversationTranscript += Text;
		RefreshConversationText();
	}

	void AppendConversationEvent(EConversationMessageRole Role, const FString& Text)
	{
		if (ActiveAssistantMessageIndex != INDEX_NONE && ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
		{
			ConversationMessages[ActiveAssistantMessageIndex].bStreaming = false;
			ActiveAssistantMessageIndex = INDEX_NONE;
		}

		AddConversationMessage(Role, Text);
		const FString RoleLabel = GetConversationRoleLabel(Role).ToString();
		ConversationTranscript += FString::Printf(TEXT("\n\n[%s] %s\n"),
			*RoleLabel,
			*Text);
		RefreshConversationText();
	}

	void AppendConversationText(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return;
		}

		EConversationMessageRole EventRole = EConversationMessageRole::System;
		FString EventText;
		if (TryExtractConversationEvent(Text, EventRole, EventText))
		{
			AppendConversationEvent(EventRole, EventText);
			return;
		}

		AppendAssistantText(Text);
	}

	void RefreshConversationText()
	{
		bShowSettings = false;
		bShowDetail = true;
		bDetailIsConversation = true;
		CurrentDetailText = ConversationTranscript;
		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(LOCTEXT("CodexConversationTitle", "Codex 会话"));
		}
		RebuildConversationMessages();
	}

	void HandleAcpText(const FString& Text)
	{
		AppendConversationText(Text);
	}

	void HandleAcpPermission(const FWorldDataAcpPermissionRequest& Request)
	{
		bShowSettings = false;
		bShowDetail = true;
		bHasPendingPermission = true;
		PendingPermissionId = Request.RequestId;
		PendingPermissionTitle = Request.Title;
		PendingPermissionToolName = !Request.ToolName.IsEmpty() ? Request.ToolName : Request.ToolCallId;
		PendingAllowOptionId = Request.AllowOptionId.IsEmpty() ? TEXT("allow") : Request.AllowOptionId;
		PendingDenyOptionId = Request.DenyOptionId.IsEmpty() ? TEXT("deny") : Request.DenyOptionId;

		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(LOCTEXT("CodexConversationTitle", "Codex 会话"));
		}

		SetLastAction(FText::Format(
			LOCTEXT("PermissionWaitingAction", "等待权限确认：{0}"),
			FText::FromString(PendingPermissionTitle)));
	}

	void HandleAcpStatus(const FString& Text)
	{
		if (Text.Contains(TEXT("回复完成")) && ActiveAssistantMessageIndex != INDEX_NONE && ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
		{
			ConversationMessages[ActiveAssistantMessageIndex].bStreaming = false;
			ActiveAssistantMessageIndex = INDEX_NONE;
			RebuildConversationMessages();
		}
		SetLastAction(FText::FromString(Text));
	}

	void HandleAcpError(const FString& Text)
	{
		ClearPendingPermission();
		AppendConversationText(FString::Printf(TEXT("\n\n[错误] %s\n"), *Text));
		SetLastAction(FText::FromString(Text));
	}

	FReply OnAllowPermissionClicked()
	{
		return ResolvePendingPermission(true);
	}

	FReply OnDenyPermissionClicked()
	{
		return ResolvePendingPermission(false);
	}

	FReply ResolvePendingPermission(bool bAllow)
	{
		if (!bHasPendingPermission)
		{
			return FReply::Handled();
		}

		const FString SelectedOptionId = bAllow ? PendingAllowOptionId : PendingDenyOptionId;
		if (AcpClient.IsValid())
		{
			AcpClient->RespondToPermission(PendingPermissionId, SelectedOptionId);
		}

		ClearPendingPermission();
		SetLastAction(bAllow
			? LOCTEXT("PermissionAllowedAction", "已允许权限请求。")
			: LOCTEXT("PermissionDeniedAction", "已拒绝权限请求。"));
		return FReply::Handled();
	}

	void ClearPendingPermission()
	{
		bHasPendingPermission = false;
		PendingPermissionId = 0;
		PendingPermissionTitle.Empty();
		PendingPermissionToolName.Empty();
		PendingAllowOptionId.Empty();
		PendingDenyOptionId.Empty();
	}

	void ResetConversationView()
	{
		SaveActiveConversation();
		if (AcpClient.IsValid())
		{
			AcpClient->Stop();
		}

		FConversation NewConversation;
		NewConversation.Title = LOCTEXT("NewConversationTitle", "新对话");
		NewConversation.CreatedAt = FDateTime::Now();
		Conversations.Insert(NewConversation, 0);
		ActiveConversationIndex = 0;

		bShowSettings = false;
		bShowDetail = false;
		bDetailIsConversation = false;
		ClearPendingPermission();
		CurrentDetailText.Empty();
		ConversationTranscript.Empty();
		ConversationMessages.Empty();
		ActiveAssistantMessageIndex = INDEX_NONE;
		RebuildConversationMessages();
		RebuildSidebar();
		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(LOCTEXT("NewConversationTitle", "新对话"));
		}
		if (DetailTextBox.IsValid())
		{
			DetailTextBox->SetText(FText::GetEmpty());
		}
		if (ComposerTextBox.IsValid())
		{
			ComposerTextBox->SetText(FText::GetEmpty());
		}
		SetLastAction(LOCTEXT("NewConversationAction", "新对话已准备好。"));
	}

	void SaveActiveConversation()
	{
		if (Conversations.IsValidIndex(ActiveConversationIndex))
		{
			FConversation& Conversation = Conversations[ActiveConversationIndex];
			Conversation.Messages = ConversationMessages;
			Conversation.Transcript = ConversationTranscript;
			Conversation.ActiveAssistantMessageIndex = ActiveAssistantMessageIndex;
		}
	}

	void LoadConversation(int32 Index)
	{
		if (!Conversations.IsValidIndex(Index))
		{
			return;
		}

		SaveActiveConversation();
		ActiveConversationIndex = Index;

		const FConversation& Conversation = Conversations[Index];
		ConversationMessages = Conversation.Messages;
		ConversationTranscript = Conversation.Transcript;
		ActiveAssistantMessageIndex = Conversation.ActiveAssistantMessageIndex;
		CurrentDetailText = Conversation.Transcript;

		bShowSettings = false;
		bShowDetail = ConversationMessages.Num() > 0;
		bDetailIsConversation = true;
		ClearPendingPermission();

		RebuildConversationMessages();
		RebuildSidebar();
		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(ConversationMessages.Num() > 0
				? LOCTEXT("CodexConversationTitle", "Codex 会话")
				: LOCTEXT("NewConversationTitle", "新对话"));
		}
		if (ComposerTextBox.IsValid())
		{
			ComposerTextBox->SetText(FText::GetEmpty());
		}
		SetLastAction(FText::Format(LOCTEXT("SwitchedConversationAction", "已切换到对话：{0}"), GetConversationTitle(Index)));
	}

	FReply OnSelectConversation(int32 Index)
	{
		if (Index != ActiveConversationIndex)
		{
			LoadConversation(Index);
		}
		return FReply::Handled();
	}

	FText GetConversationTitle(int32 Index) const
	{
		return Conversations.IsValidIndex(Index)
			? Conversations[Index].Title
			: LOCTEXT("NewConversationTitle", "新对话");
	}

	FText MakeConversationTitle(const FString& Message) const
	{
		FString Title = Message;
		Title.TrimStartAndEndInline();
		int32 NewlineIndex = INDEX_NONE;
		if (Title.FindChar(TCHAR('\n'), NewlineIndex))
		{
			Title.LeftInline(NewlineIndex);
			Title.TrimEndInline();
		}
		const int32 MaxLength = 18;
		if (Title.Len() > MaxLength)
		{
			Title = Title.Left(MaxLength) + TEXT("…");
		}
		return Title.IsEmpty() ? LOCTEXT("NewConversationTitle", "新对话") : FText::FromString(Title);
	}

	FText GetConversationAgeText(FDateTime CreatedAt) const
	{
		const int32 Seconds = FMath::Max(0, static_cast<int32>((FDateTime::Now() - CreatedAt).GetTotalSeconds()));
		if (Seconds < 5)
		{
			return LOCTEXT("AgeJustNow", "刚刚");
		}
		if (Seconds < 60)
		{
			return FText::FromString(FString::Printf(TEXT("%ds"), Seconds));
		}
		const int32 Minutes = Seconds / 60;
		if (Minutes < 60)
		{
			return FText::FromString(FString::Printf(TEXT("%dm"), Minutes));
		}
		const int32 Hours = Minutes / 60;
		if (Hours < 24)
		{
			return FText::FromString(FString::Printf(TEXT("%dh"), Hours));
		}
		return FText::FromString(FString::Printf(TEXT("%dd"), Hours / 24));
	}

	TSharedRef<SWidget> BuildConversationEntry(int32 Index)
	{
		const FConversation& Conversation = Conversations[Index];
		const FDateTime CreatedAt = Conversation.CreatedAt;
		return BuildConversationItem(
			Conversation.Title,
			TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SUEBridgeMCPPanel::GetConversationAgeText, CreatedAt)),
			Index == ActiveConversationIndex,
			FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSelectConversation, Index));
	}

	void RebuildSidebar()
	{
		if (!SidebarListScrollBox.IsValid())
		{
			return;
		}

		SidebarListScrollBox->ClearChildren();

		const FDateTime Now = FDateTime::Now();
		const FDateTime TodayStart(Now.GetYear(), Now.GetMonth(), Now.GetDay());

		int32 TodayCount = 0;
		int32 RecentCount = 0;
		for (int32 Index = 0; Index < Conversations.Num(); ++Index)
		{
			const bool bIsToday = Conversations[Index].CreatedAt >= TodayStart;
			if (bIsToday)
			{
				if (TodayCount == 0)
				{
					SidebarListScrollBox->AddSlot()
						.Padding(8.0f, 10.0f, 8.0f, 0.0f)
						[
							BuildDateLabel(LOCTEXT("TodayLabel", "今天"))
						];
				}

				SidebarListScrollBox->AddSlot()
					.Padding(6.0f, TodayCount == 0 ? 6.0f : 2.0f, 6.0f, 0.0f)
					[
						BuildConversationEntry(Index)
					];
				++TodayCount;
			}
			else
			{
				if (RecentCount == 0)
				{
					SidebarListScrollBox->AddSlot()
						.Padding(8.0f, 14.0f, 8.0f, 0.0f)
						[
							BuildDateLabel(LOCTEXT("RecentLabel", "最近"))
						];
				}

				SidebarListScrollBox->AddSlot()
					.Padding(6.0f, RecentCount == 0 ? 6.0f : 2.0f, 6.0f, 0.0f)
					[
						BuildConversationEntry(Index)
					];
				++RecentCount;
			}
		}
	}

	void ShowProjectInfo()
	{
		SetDetail(LOCTEXT("ProjectInfoTitle", "项目信息"), FWorldDataMCPServer::GetProjectInfoJson());
	}

	FReply OnNewConversationClicked()
	{
		ResetConversationView();
		return FReply::Handled();
	}

	FReply OnSettingsClicked()
	{
		bShowSettings = true;
		SetLastAction(LOCTEXT("SettingsOpenedAction", "已打开设置。"));
		return FReply::Handled();
	}

	FReply OnSendClicked()
	{
		const FString Message = ComposerTextBox.IsValid() ? ComposerTextBox->GetText().ToString().TrimStartAndEnd() : FString();
		if (Message.IsEmpty())
		{
			SetLastAction(LOCTEXT("EmptyMessageAction", "请输入消息后再发送。"));
			return FReply::Handled();
		}

		if (AcpClient.IsValid() && AcpClient->IsProcessing())
		{
			SetLastAction(LOCTEXT("CodexBusyAction", "Codex 正在处理上一条消息，请稍后再发送。"));
			return FReply::Handled();
		}

		StartConversationTurn(Message);

		if (ComposerTextBox.IsValid())
		{
			ComposerTextBox->SetText(FText::GetEmpty());
		}

		if (!AcpClient.IsValid())
		{
			HandleAcpError(TEXT("Codex ACP 客户端未初始化。"));
			return FReply::Handled();
		}

		SetLastAction(LOCTEXT("SendingToCodexAction", "正在发送到 Codex ACP..."));
		AcpClient->SetPermissionMode(CurrentMode);
		AcpClient->SendPrompt(Message);
		return FReply::Handled();
	}

	FReply OnStartClicked()
	{
		if (!FWorldDataMCPServer::IsRunning())
		{
			FWorldDataMCPServer::Start(FWorldDataMCPServer::LoadConfiguredPort());
		}
		if (FWorldDataMCPServer::IsRunning())
		{
			FWorldDataMCPServer::RefreshConnectionFiles();
		}
		SetLastAction(FWorldDataMCPServer::IsRunning() ? LOCTEXT("StartedAction", "已启动并刷新连接文件。") : LOCTEXT("StartFailedAction", "启动失败。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnStopClicked()
	{
		FWorldDataMCPServer::Stop();
		SetLastAction(LOCTEXT("StoppedAction", "已停止。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnRefreshClicked()
	{
		FWorldDataMCPServer::RefreshConnectionFiles();
		SetLastAction(LOCTEXT("RefreshedAction", "连接文件已刷新。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnStatusClicked()
	{
		SetLastAction(LOCTEXT("ViewedStatusAction", "已查看状态。"));
		SetDetail(LOCTEXT("StatusTitle", "服务状态"), FWorldDataMCPServer::GetStatusJson());
		return FReply::Handled();
	}

	FReply OnProjectInfoClicked()
	{
		SetLastAction(LOCTEXT("ViewedProjectInfoAction", "已查看项目信息。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnBootstrapClicked()
	{
		SetLastAction(LOCTEXT("ViewedBootstrapAction", "已查看启动上下文。"));
		SetDetail(LOCTEXT("BootstrapTitle", "启动上下文"), FWorldDataMCPServer::ReadResource(TEXT("worlddata://context/bootstrap")));
		return FReply::Handled();
	}

	FReply OnPolicyClicked()
	{
		SetLastAction(LOCTEXT("ViewedPolicyAction", "已查看 Codex 策略快照。"));
		SetDetail(LOCTEXT("PolicyTitle", "Codex 策略快照"), FWorldDataMCPServer::ReadResource(TEXT("worlddata://codex/policy-snapshot")));
		return FReply::Handled();
	}

	FReply OnToolsClicked()
	{
		SetLastAction(LOCTEXT("ViewedToolsAction", "已查看工具列表。"));
		SetDetail(LOCTEXT("ToolsTitle", "工具列表"), FWorldDataMCPServer::GetToolDefinitionsJson());
		return FReply::Handled();
	}

	FReply OnResourcesClicked()
	{
		SetLastAction(LOCTEXT("ViewedResourcesAction", "已查看资源列表。"));
		SetDetail(LOCTEXT("ResourcesTitle", "资源列表"), FWorldDataMCPServer::GetResourceListJson());
		return FReply::Handled();
	}

	FReply OnCopyUrlClicked()
	{
		if (FWorldDataMCPServer::IsRunning())
		{
			UEBridgeMCP::CopyToClipboard(FWorldDataMCPServer::GetMcpUrl());
			SetLastAction(LOCTEXT("CopiedUrlAction", "已复制 MCP 地址。"));
			UEBridgeMCP::Notify(LOCTEXT("CopiedUrlNotification", "MCP 地址已复制。"));
		}
		return FReply::Handled();
	}

	FReply OnCopyConfigClicked()
	{
		if (FWorldDataMCPServer::IsRunning())
		{
			UEBridgeMCP::CopyToClipboard(UEBridgeMCP::BuildClientConfigSnippet());
			SetLastAction(LOCTEXT("CopiedConfigAction", "已复制 MCP 配置。"));
			UEBridgeMCP::Notify(LOCTEXT("CopiedConfigNotification", "MCP 配置已复制。"));
		}
		return FReply::Handled();
	}

	FReply OnCopyCurrentClicked()
	{
		if (!CurrentDetailText.IsEmpty())
		{
			UEBridgeMCP::CopyToClipboard(CurrentDetailText);
			SetLastAction(LOCTEXT("CopiedViewAction", "已复制当前内容。"));
			UEBridgeMCP::Notify(LOCTEXT("CopiedViewNotification", "当前面板内容已复制。"));
		}
		return FReply::Handled();
	}

	FReply OnOpenProjectFolderClicked()
	{
		UEBridgeMCP::ExploreFileParent(FWorldDataMCPServer::GetClientConfigFilePath());
		SetLastAction(LOCTEXT("OpenedProjectFolderAction", "已打开项目目录。"));
		return FReply::Handled();
	}

	FReply OnOpenSavedFolderClicked()
	{
		UEBridgeMCP::ExploreFileParent(FWorldDataMCPServer::GetConnectionFilePath());
		SetLastAction(LOCTEXT("OpenedSavedFolderAction", "已打开 Saved 目录。"));
		return FReply::Handled();
	}

	FText LastAction;
	FString CurrentDetailText;
	FString ConversationTranscript;
	TArray<FConversationMessage> ConversationMessages;
	FLinearColor SettingsColor = FLinearColor::White;
	FString CodexCliPath;
	FString CursorCliPath;
	FString DetectedCodexCliPath;
	FString DetectedCursorCliPath;
	FEditableTextBoxStyle LightTextBoxStyle;
	FButtonStyle ComposerButtonStyle;
	FComboButtonStyle ComposerComboButtonStyle;
	EWorldDataCodexPermissionMode CurrentMode = EWorldDataCodexPermissionMode::Default;
	TSharedPtr<FWorldDataCodexACPClient> AcpClient;
	bool bShowDetail = false;
	bool bShowSettings = false;
	bool bDetailIsConversation = false;
	bool bHasPendingPermission = false;
	int32 ActiveAssistantMessageIndex = INDEX_NONE;
	int32 PendingPermissionId = 0;
	FString PendingPermissionTitle;
	FString PendingPermissionToolName;
	FString PendingAllowOptionId;
	FString PendingDenyOptionId;
	TSharedPtr<SWidgetSwitcher> ContentSwitcher;
	TSharedPtr<SScrollBox> ConversationScrollBox;
	TSharedPtr<SScrollBox> SidebarListScrollBox;
	TArray<FConversation> Conversations;
	int32 ActiveConversationIndex = INDEX_NONE;
	FString ServerPortText;
	TArray<FString> CachedToolNames;
	TSharedPtr<STextBlock> DetailTitleText;
	TSharedPtr<SMultiLineEditableTextBox> DetailTextBox;
	TSharedPtr<SMultiLineEditableTextBox> ComposerTextBox;
};

#undef LOCTEXT_NAMESPACE
