#include "SUEBridgeMCPPanel.h"
#include "SUEBridgeMCPApprovalView.h"
#include "SUEBridgeMCPWebPanel.h"

#include "WorldDataAgentBackend.h"
#include "UEBridgeMCPAgentController.h"
#include "UEBridgeMCPApprovalController.h"
#include "UEBridgeMCPCoreModule.h"
#include "UEBridgeMCPPanelViewModel.h"
#include "WorldDataAgentBackendFactory.h"
#include "WorldDataCodexACPClient.h"
#include "WorldDataCodexAcpBootstrap.h"
#include "UEBridgeMCPStyle.h"

#include "Async/Async.h"
#include "Framework/Application/SlateApplication.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/MessageDialog.h"
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
		ViewModel = MakeUnique<FUEBridgeMCPPanelViewModel>();
		ViewModel->Initialize(LOCTEXT("InitialLastAction", "新会话已就绪。"));
		LoadSettings();
		RefreshCodexAcpSetupStatus();
		ConfigureLightTextBoxStyle();
		ConfigureComposerButtonStyle();
		AgentController = MakeUnique<FUEBridgeMCPAgentController>();
		ApprovalController = MakeUnique<FUEBridgeMCPApprovalController>();
		StartConfiguredAgentBackend();

		ChildSlot
		[
			SNew(SBorder)
			.Padding(18.0f)
			.BorderImage(&HtmlShellBrush)
			.BorderBackgroundColor(FSlateColor(UEBridgeMCP::Palette::SurfaceRaised()))
			[
				SNew(SBorder)
				.Padding(0.0f)
				.BorderImage(&HtmlShellBrush)
				.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					BuildHtmlParityLayout()
				]
			]
		];

		ResetConversationView();
	}

	~SUEBridgeMCPPanel()
	{
		if (AgentController.IsValid())
		{
			AgentController->Stop();
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
		FString TaskId;
		FString ThreadId;
		bool bHasCustomTitle = false;
		TArray<FConversationMessage> Messages;
		FString Transcript;
		int32 ActiveAssistantMessageIndex = INDEX_NONE;
	};

	FSlateFontInfo GetHtmlParityFont(const int32 Size, const bool bBold = false) const
	{
		FSlateFontInfo Font = FAppStyle::GetFontStyle(bBold ? "NormalFontBold" : "NormalFont");
		Font.Size = Size;
		return Font;
	}

	// Slate parity layer for Tools/ue-bridge-preview/index.html. The underlying
	// server, ACP and conversation logic remains shared with the legacy layout.
	TSharedRef<SWidget> BuildHtmlParityLayout()
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				BuildHtmlParitySidebar()
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox)
				.WidthOverride(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					BuildHtmlParityTopBar()
				]
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					BuildHtmlParityMainArea()
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityTopBar()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.Padding(FMargin(24.0f, 12.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					SNew(SBox)
					.MinDesiredHeight(56.0f)
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
								.Text(LOCTEXT("HtmlParityEyebrow", "WORLDDATA / CODEX"))
								.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
								.Font(GetHtmlParityFont(12, true))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 1.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HtmlParityConversationTitle", "Codex 会话"))
								.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
								.Font(GetHtmlParityFont(18, true))
							]
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 2.0f, 0.0f, 0.0f)
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("HtmlParityConnectionDot", "●"))
									.ColorAndOpacity_Lambda([] { return UEBridgeMCP::GetStatusColor(); })
								]
								+ SHorizontalBox::Slot()
								.AutoWidth()
								.VAlign(VAlign_Center)
								.Padding(4.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text_Lambda([]
									{
										return FText::FromString(FString::Printf(TEXT("MCP 服务%s :%d"),
											GetWorldDataMCPService().IsRunning() ? TEXT("已连接") : TEXT("未运行"),
											GetWorldDataMCPService().IsRunning() ? GetWorldDataMCPService().GetPort() : GetWorldDataMCPService().LoadConfiguredPort()));
									})
									.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
									.Font(GetHtmlParityFont(12))
								]
							]
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(6.0f, 0.0f)
						[
							BuildToolbarButton(LOCTEXT("HtmlParityCopyUrl", "复制连接地址"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnCopyUrlClicked))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							BuildToolbarButton(LOCTEXT("HtmlParitySetupCli", "一键配置 CLI"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSetupCliClicked))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							BuildToolbarButton(LOCTEXT("HtmlParityRefresh", "刷新"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnRefreshClicked), TAttribute<bool>::CreateLambda([] { return GetWorldDataMCPService().IsRunning(); }))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							BuildToolbarButton(LOCTEXT("HtmlParitySettings", "设置"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSettingsClicked))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							BuildHtmlParityStatusPill()
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.HeightOverride(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityStatusPill() const
	{
		return SNew(SBorder)
			.Padding(FMargin(9.0f, 4.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this]
			{
				const FLinearColor StatusColor = AgentBackend.IsValid() && AgentBackend->IsProcessing()
					? UEBridgeMCP::Palette::Warning()
					: (HasRetryableError() ? UEBridgeMCP::Palette::Danger() : UEBridgeMCP::Palette::Success());
				return FSlateColor(UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), StatusColor, 0.10f));
			})
			[
				SNew(STextBlock)
				.Text_Lambda([this]
				{
					if (AgentBackend.IsValid() && AgentBackend->IsProcessing())
					{
						return LOCTEXT("HtmlParityProcessingState", "● 处理中");
					}
					return HasRetryableError() ? LOCTEXT("HtmlParityRetryState", "● 需要重试") : LOCTEXT("HtmlParityReadyState", "● 已就绪");
				})
				.ColorAndOpacity_Lambda([this]
				{
					if (AgentBackend.IsValid() && AgentBackend->IsProcessing())
					{
						return FSlateColor(UEBridgeMCP::Palette::Warning());
					}
					return FSlateColor(HasRetryableError() ? UEBridgeMCP::Palette::Danger() : UEBridgeMCP::Palette::Success());
				})
				.Font(GetHtmlParityFont(12, true))
			];
	}

	TSharedRef<SWidget> BuildHtmlParitySidebarAction(const FText& Icon, const FText& Label, FOnClicked OnClicked, TAttribute<bool> bActive) const
	{
		return SNew(SButton)
			.ButtonStyle(&ComposerButtonStyle)
			.ButtonColorAndOpacity_Lambda([this, bActive]
			{
				return FSlateColor(bActive.Get() ? GetAccentFillColor(0.14f) : GetPanelBackgroundColor());
			})
			.ForegroundColor_Lambda([this, bActive]
			{
				return FSlateColor(bActive.Get() ? GetEffectiveAccentColor() : GetPanelTextColor());
			})
			.ContentPadding(FMargin(10.0f, 8.0f))
			.HAlign(HAlign_Left)
			.OnClicked(OnClicked)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Icon)
					.ColorAndOpacity_Lambda([this, bActive] { return FSlateColor(bActive.Get() ? GetEffectiveAccentColor() : GetPanelSubduedTextColor()); })
					.Font(GetHtmlParityFont(14))
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(9.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(Label)
					.ColorAndOpacity_Lambda([this, bActive] { return FSlateColor(bActive.Get() ? GetEffectiveAccentColor() : GetPanelTextColor()); })
					.Font(GetHtmlParityFont(14))
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParitySidebar()
	{
		return SNew(SBorder)
			.Padding(0.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
			[
				SNew(SBox)
				.WidthOverride(272.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBorder)
						.Padding(FMargin(18.0f, 18.0f, 18.0f, 16.0f))
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(SBorder)
								.Padding(FMargin(7.0f, 5.0f))
								.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
								.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentButtonColor()); })
								[
									SNew(STextBlock)
									.Text(LOCTEXT("HtmlParityBrandMark", "WD"))
									.ColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
									.Font(GetHtmlParityFont(13, true))
								]
							]
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							.VAlign(VAlign_Center)
							.Padding(9.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HtmlParityBrandName", "Unlimited.AI"))
								.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
								.Font(GetHtmlParityFont(15, true))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(5.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("HtmlParityBrandChevron", "⌄"))
								.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SBox)
						.HeightOverride(1.0f)
						[
							SNew(SBorder).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(9.0f, 9.0f, 9.0f, 10.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
						[
							BuildHtmlParitySidebarAction(LOCTEXT("HtmlParityNewIcon", "+"), LOCTEXT("HtmlParityNewChat", "新建会话"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnNewConversationClicked), TAttribute<bool>(false))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
						[
							BuildHtmlParitySidebarAction(LOCTEXT("HtmlParitySettingsIcon", "*"), LOCTEXT("HtmlParitySettingsAction", "设置"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSettingsClicked), TAttribute<bool>::CreateLambda([this] { return bShowSettings; }))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							BuildHtmlParitySidebarAction(LOCTEXT("HtmlParityChatIcon", "o"), LOCTEXT("HtmlParityChatAction", "聊天"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnShowChatClicked), TAttribute<bool>::CreateLambda([this] { return !bShowSettings; }))
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(17.0f, 5.0f, 17.0f, 6.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(STextBlock).Text(LOCTEXT("HtmlParityHistoryTitle", "聊天记录")).ColorAndOpacity(FSlateColor(GetPanelSubduedTextColor())).Font(GetHtmlParityFont(13, true))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(STextBlock).Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("%d 个会话"), Conversations.Num())); }).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(12))
						]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					.Padding(7.0f, 0.0f, 7.0f, 10.0f)
					[
						SAssignNew(SidebarListScrollBox, SScrollBox)
					]
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityMainArea()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SBorder)
				.Padding(FMargin(24.0f, 18.0f, 24.0f, 14.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					SNew(SWidgetSwitcher)
					.WidgetIndex_Lambda([this]
					{
						if (bShowSettings)
						{
							return 2;
						}
						return bShowDetail ? 1 : 0;
					})
					+ SWidgetSwitcher::Slot()
					[
						BuildHtmlParityEmptyChat()
					]
					+ SWidgetSwitcher::Slot()
					[
						BuildHtmlParityConversationContent()
					]
					+ SWidgetSwitcher::Slot()
					[
						BuildSettingsPanel()
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(24.0f, 0.0f, 24.0f, 16.0f)
			[
				BuildHtmlParityComposer()
			];
	}

	TSharedRef<SWidget> BuildHtmlParityEmptyChat()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildHtmlParityPreviewNotice()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 16.0f)
			[
				SNew(SBorder)
					.Padding(1.0f)
					.BorderImage(&HtmlLargeCardBrush)
					.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentBorderColor()); })
					[
						SNew(SBorder)
						.Padding(24.0f)
						.BorderImage(&HtmlLargeCardBrush)
						.BorderBackgroundColor_Lambda([this] { return FSlateColor(UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), GetEffectiveAccentColor(), 0.025f)); })
						[
							SNew(SBox)
							.HAlign(HAlign_Center)
							.VAlign(VAlign_Center)
							[
								SNew(SVerticalBox)
								+ SVerticalBox::Slot()
								.AutoHeight()
								.HAlign(HAlign_Center)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("HtmlParityEmptyPrompt", "今天我能帮您做什么？"))
									.ColorAndOpacity(FSlateColor(GetPanelSubduedTextColor()))
									.Font(GetHtmlParityFont(16))
								]
								+ SVerticalBox::Slot()
								.AutoHeight()
								.HAlign(HAlign_Center)
								.Padding(0.0f, 8.0f, 0.0f, 0.0f)
								[
									SNew(STextBlock)
									.Text(LOCTEXT("HtmlParityEmptyHelp", "发送消息后，Codex 会话、工具事件与权限确认会显示在这里。"))
									.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
									.Font(GetHtmlParityFont(13))
								]
							]
						]
					]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityPreviewNotice()
	{
		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(&HtmlCardBrush)
			.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
			[
				SNew(SBorder)
				.Padding(FMargin(11.0f, 9.0f))
				.BorderImage(&HtmlCardBrush)
				.BorderBackgroundColor(FSlateColor(GetPanelSurfaceColor()))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBorder).Padding(7.0f, 4.0f).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.12f)); })
						[
							SNew(STextBlock).Text(LOCTEXT("HtmlParityNoticeMark", "C")).ColorAndOpacity_Lambda([this] { return FSlateColor(GetEffectiveAccentColor()); }).Font(GetHtmlParityFont(13, true))
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock).Text(LOCTEXT("HtmlParityNoticeTitle", "预览环境")).ColorAndOpacity(FSlateColor(GetPanelTextColor())).Font(GetHtmlParityFont(13, true))
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock).Text(LOCTEXT("HtmlParityNoticeBody", "此处展示本地交互原型；连接 UE 后，服务状态、工具结果与会话内容会实时同步。"))
							.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(12))
						]
					]
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityConversationContent()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.Visibility_Lambda([this] { return bDetailIsConversation ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					BuildHtmlParityPreviewNotice()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this] { return HasPendingMcpApproval() ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					BuildMcpApprovalCard()
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SNew(SBox)
				.Visibility_Lambda([this] { return bHasPendingPermission ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					BuildPermissionRequestCard()
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				SAssignNew(ContentSwitcher, SWidgetSwitcher)
				.WidgetIndex_Lambda([this] { return bDetailIsConversation ? 0 : 1; })
				+ SWidgetSwitcher::Slot()
				[
					BuildConversationMessagesView()
				]
				+ SWidgetSwitcher::Slot()
				[
					BuildHtmlParityDetailView()
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityDetailView()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SAssignNew(DetailTitleText, STextBlock).Text(LOCTEXT("HtmlParityDetailTitle", "服务详情")).ColorAndOpacity(FSlateColor(GetPanelTextColor())).Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					BuildToolbarButton(LOCTEXT("HtmlParityCopyCurrent", "复制当前内容"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnCopyCurrentClicked))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(DetailTextBox, SMultiLineEditableTextBox)
				.IsReadOnly(true).AutoWrapText(false).Style(&LightTextBoxStyle)
				.BackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				.ForegroundColor(FSlateColor(GetPanelTextColor()))
				.ReadOnlyForegroundColor(FSlateColor(GetPanelTextColor()))
				.Font(FAppStyle::GetFontStyle("MonospacedText"))
			];
	}

	TSharedRef<SWidget> BuildHtmlParityContextChip(const FText& Label, const FText& Value, const FText& Tooltip, const FText& Detail = FText::GetEmpty()) const
	{
		return SNew(SBorder)
			.Padding(FMargin(8.0f, 4.0f))
			.BorderImage(&HtmlControlBrush)
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.06f)); })
			.ToolTipText(Tooltip)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(11, true))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(Value).ColorAndOpacity(FSlateColor(GetPanelTextColor())).Font(GetHtmlParityFont(12))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility(Detail.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					[
						SNew(SBorder).Padding(FMargin(4.0f, 1.0f)).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
						[
							SNew(STextBlock).Text(Detail).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(11))
						]
					]
				]
			];
	}

	TSharedRef<SWidget> BuildHtmlParityComposer()
	{
		// The parent slot provides the horizontal inset; the composer should remain
		// compact so the conversation retains the visual focus.
		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(&HtmlLargeCardBrush)
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentBorderColor()); })
			[
				SNew(SBorder)
				.Padding(FMargin(10.0f, 6.0f))
				.BorderImage(&HtmlLargeCardBrush)
				.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SBox).HeightOverride(52.0f)
							[
								SAssignNew(ComposerTextBox, SMultiLineEditableTextBox)
								.Style(&LightTextBoxStyle)
								.HintText(LOCTEXT("HtmlParityComposerHint", "输入消息…（Enter 发送，Shift + Enter 换行）"))
								.AutoWrapText(true)
								.BackgroundColor(FSlateColor(GetPanelBackgroundColor()))
								.ForegroundColor(FSlateColor(GetPanelTextColor()))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 5.0f, 0.0f, 0.0f)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
							[
								BuildIconTextButton(LOCTEXT("HtmlParityOpenProject", "+"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnOpenProjectFolderClicked), LOCTEXT("HtmlParityOpenProjectTooltip", "打开项目目录（附件发送尚未实现）"))
							]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
			[
				BuildBackendCombo()
			]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 7.0f, 0.0f)
							[
								BuildModeCombo()
							]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				BuildModelCombo()
			]
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[
								SNullWidget::NullWidget
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(7.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SBox).Visibility_Lambda([this] { return HasRetryableError() ? EVisibility::Visible : EVisibility::Collapsed; })
								[
									BuildToolbarButton(LOCTEXT("HtmlParityRetry", "重试"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnRetryConversationClicked))
								]
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
							[
								SNew(SButton)
								.HAlign(HAlign_Center).VAlign(VAlign_Center)
								.ContentPadding(FMargin(12.0f, 4.0f))
								.ButtonStyle(&ComposerButtonStyle)
								.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonColor()); })
								.ForegroundColor_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
								.OnClicked(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSendClicked))
								[
							SNew(STextBlock).Text(LOCTEXT("HtmlParitySend", "发送")).ColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); }).Font(GetHtmlParityFont(12, true))
								]
							]
						]
				]
			];
	}

	bool HasRetryableError() const
	{
		return (!AgentBackend.IsValid() || !AgentBackend->IsProcessing())
			&& ConversationMessages.ContainsByPredicate([](const FConversationMessage& Message)
			{
				return Message.Role == EConversationMessageRole::Error;
			});
	}

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
						.WidthOverride(252.0f)
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
							BuildToolbarButton(LOCTEXT("SetupCliTopButton", "一键配置 CLI"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSetupCliClicked))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							BuildToolbarButton(LOCTEXT("RefreshTopButton", "刷新"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnRefreshClicked), TAttribute<bool>::CreateLambda([] { return GetWorldDataMCPService().IsRunning(); }))
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
				.WidthOverride(252.0f)
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
			.Padding(9.0f, 4.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.22f)); })
			[
				SNew(STextBlock)
				.Text(Text)
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
				.Font(GetHtmlParityFont(12, true))
			];
	}

	TSharedRef<SWidget> BuildToolbarButton(const FText& Label, FOnClicked OnClicked, TAttribute<bool> bIsEnabled = TAttribute<bool>(true)) const
	{
		return SNew(SButton)
			.HAlign(HAlign_Center)
			.ButtonStyle(&ComposerButtonStyle)
			.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
			.ForegroundColor(FSlateColor(GetPanelTextColor()))
			.ContentPadding(FMargin(10.0f, 5.0f))
			.IsEnabled(bIsEnabled)
			.OnClicked(OnClicked)
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
				.Font(GetHtmlParityFont(12))
			];
	}

	TSharedRef<SWidget> BuildIconTextButton(const FText& Label, FOnClicked OnClicked, const FText& Tooltip = FText::GetEmpty()) const
	{
		return SNew(SButton)
			.HAlign(HAlign_Center)
			.ButtonStyle(&ComposerButtonStyle)
			.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
			.ForegroundColor(FSlateColor(GetPanelTextColor()))
			.ContentPadding(FMargin(10.0f, 5.0f))
			.ToolTipText(Tooltip)
			.OnClicked(OnClicked)
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
				.Font(GetHtmlParityFont(13))
			];
	}

	void StartConfiguredAgentBackend()
	{
		if (!AgentController.IsValid())
		{
			return;
		}

		AgentBackend = AgentController->Start(
			CurrentMode,
			FWorldDataAcpTextDelegate::CreateSP(this, &SUEBridgeMCPPanel::HandleAcpText),
			FWorldDataAcpStatusDelegate::CreateSP(this, &SUEBridgeMCPPanel::HandleAcpStatus),
			FWorldDataAcpErrorDelegate::CreateSP(this, &SUEBridgeMCPPanel::HandleAcpError),
			FWorldDataAcpPermissionDelegate::CreateSP(this, &SUEBridgeMCPPanel::HandleAcpPermission));
	}

	bool RestartAgentBackend(const FText& SuccessMessage)
	{
		if (AgentBackend.IsValid() && AgentBackend->IsProcessing())
		{
			SetLastAction(LOCTEXT("BackendChangeBusy", "Finish or stop the current response before changing Backend or model."));
			return false;
		}

		bHasPendingPermission = false;
		PendingPermissionId = 0;
		StartConfiguredAgentBackend();
		SetLastAction(SuccessMessage);
		return true;
	}

	FText GetBackendChoiceLabel() const
	{
		const FString Selected = FWorldDataAgentBackendFactory::GetConfiguredBackendId();
		if (Selected == TEXT("auto"))
		{
			return FText::FromString(AgentBackend.IsValid()
				? FString::Printf(TEXT("Auto (%s)"), *AgentBackend->GetDisplayName())
				: TEXT("Auto"));
		}
		if (Selected == TEXT("codex_app_server"))
		{
			return LOCTEXT("BackendCodexAppServer", "Codex app-server");
		}
		if (Selected == TEXT("acp_codex"))
		{
			return LOCTEXT("BackendCodexAcp", "Codex ACP");
		}
		if (Selected == TEXT("acp_cursor"))
		{
			return LOCTEXT("BackendCursorAcp", "Cursor ACP");
		}
		if (Selected == TEXT("acp_claude"))
		{
			return LOCTEXT("BackendClaudeAcp", "Claude Code ACP");
		}
		return FText::FromString(Selected);
	}

	TSharedRef<SWidget> BuildBackendCombo()
	{
		return SNew(SComboButton)
			.ToolTipText(LOCTEXT("BackendComboTooltip", "Choose the embedded conversation backend. HTTP MCP remains a separate UE automation service."))
			.ComboButtonStyle(&ComposerComboButtonStyle)
			.ButtonStyle(&ComposerButtonStyle)
			.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
			.ForegroundColor(FSlateColor(GetPanelTextColor()))
			.ContentPadding(FMargin(8.0f, 3.0f))
			.HasDownArrow(false)
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("BackendComboLabel", "Backend"))
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
					.Font(GetHtmlParityFont(11, true))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this] { return GetBackendChoiceLabel(); })
					.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
					.Font(GetHtmlParityFont(12))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("BackendComboChevron", "v")).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(11))
				]
			]
			.MenuContent()
			[
				BuildBackendMenu()
			];
	}

	TSharedRef<SWidget> BuildBackendMenu()
	{
		TSharedRef<SVerticalBox> Items = SNew(SVerticalBox);
		for (const FWorldDataAgentBackendOption& Option : FWorldDataAgentBackendFactory::GetBackendOptions())
		{
			Items->AddSlot().AutoHeight().Padding(0.0f, 2.0f)
			[
				BuildBackendMenuItem(Option)
			];
		}

		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(&HtmlLargeCardBrush)
			.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
			[
				SNew(SBorder)
				.Padding(FMargin(8.0f, 7.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 2.0f, 4.0f, 8.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("BackendMenuTitle", "Conversation Backend")).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						Items
					]
				]
			];
	}

	TSharedRef<SWidget> BuildBackendMenuItem(const FWorldDataAgentBackendOption& Option)
	{
		const FString OptionId = Option.Id;
		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, OptionId]
			{
				return FWorldDataAgentBackendFactory::GetConfiguredBackendId() == OptionId
					? FSlateColor(GetAccentBorderColor()) : FSlateColor(UEBridgeMCP::Palette::Background());
			})
			.OnMouseButtonDown_Lambda([this, OptionId](const FGeometry&, const FPointerEvent&)
			{
				if (AgentBackend.IsValid() && AgentBackend->IsProcessing())
				{
					SetLastAction(LOCTEXT("BackendChangeBusyMenu", "Finish or stop the current response before changing Backend."));
					FSlateApplication::Get().DismissAllMenus();
					return FReply::Handled();
				}

				FString Error;
				if (!FWorldDataAgentBackendFactory::SetConfiguredBackendId(OptionId, Error))
				{
					SetLastAction(FText::FromString(Error));
				}
				else
				{
					RestartAgentBackend(FText::Format(LOCTEXT("BackendChangedAction", "Conversation Backend changed to {0}. A new backend session will be created on the next message."), GetBackendChoiceLabel()));
				}
				FSlateApplication::Get().DismissAllMenus();
				return FReply::Handled();
			})
			[
				SNew(SBorder)
				.Padding(FMargin(10.0f, 7.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this, OptionId]
				{
					return FWorldDataAgentBackendFactory::GetConfiguredBackendId() == OptionId
						? FSlateColor(GetAccentFillColor(0.18f)) : FSlateColor(UEBridgeMCP::Palette::Background());
				})
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(FText::FromString(Option.DisplayName + (Option.bConfigured ? TEXT("") : TEXT(" (not configured)")))).ColorAndOpacity(FSlateColor(GetPanelTextColor())).Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock).Text(FText::FromString(Option.Description)).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).AutoWrapText(true).WrapTextAt(420.0f)
					]
				]
			];
	}

	bool CanSelectModel() const
	{
		return AgentBackend.IsValid() && AgentBackend->GetCapabilities().bSupportsModelSelection;
	}

	FText GetModelChoiceLabel() const
	{
		if (!CanSelectModel())
		{
			return LOCTEXT("AdapterManagedModel", "Adapter-managed");
		}
		const FString Model = FWorldDataAgentBackendFactory::GetCodexAppServerModel();
		return Model.IsEmpty() ? LOCTEXT("CodexDefaultModel", "Codex default") : FText::FromString(Model);
	}

	TSharedRef<SWidget> BuildModelCombo()
	{
		return SNew(SComboButton)
			.ToolTipText_Lambda([this]
			{
				return CanSelectModel()
					? LOCTEXT("ModelComboTooltip", "Choose the model used for the next Codex app-server thread.")
					: LOCTEXT("AdapterModelTooltip", "This ACP backend controls its own model. The plugin does not send an unverified model-selection request to an adapter.");
			})
			.ComboButtonStyle(&ComposerComboButtonStyle)
			.ButtonStyle(&ComposerButtonStyle)
			.ButtonColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentControlColor()); })
			.ForegroundColor(FSlateColor(GetPanelTextColor()))
			.ContentPadding(FMargin(8.0f, 3.0f))
			.HasDownArrow(false)
			.IsEnabled_Lambda([this] { return CanSelectModel(); })
			.ButtonContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Text(LOCTEXT("ModelComboLabel", "Model")).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(11, true))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text_Lambda([this] { return GetModelChoiceLabel(); }).ColorAndOpacity(FSlateColor(GetPanelTextColor())).Font(GetHtmlParityFont(12))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock).Text(LOCTEXT("ModelComboChevron", "v")).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).Font(GetHtmlParityFont(11))
				]
			]
			.MenuContent()
			[
				BuildModelMenu()
			];
	}

	TSharedRef<SWidget> BuildModelMenu()
	{
		return SNew(SBorder)
			.Padding(1.0f)
			.BorderImage(&HtmlLargeCardBrush)
			.BorderBackgroundColor(FSlateColor(GetPanelBorderColor()))
			[
				SNew(SBorder).Padding(FMargin(10.0f, 8.0f)).BorderImage(FAppStyle::GetBrush("WhiteBrush")).BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).Text(LOCTEXT("ModelMenuTitle", "Codex app-server model")).ColorAndOpacity(FSlateColor(GetPanelTextColor())).Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 7.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("ModelMenuDescription", "Enter an exact model ID supported by the installed Codex app-server. It applies after the backend restarts and creates a new thread.")).ColorAndOpacity(FSlateColor(GetPanelMutedTextColor())).AutoWrapText(true).WrapTextAt(400.0f)
					]
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(ModelTextBox, SEditableTextBox).Text(FText::FromString(FWorldDataAgentBackendFactory::GetCodexAppServerModel())).HintText(LOCTEXT("ModelIdHint", "Codex default"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							BuildToolbarButton(LOCTEXT("ApplyModel", "Apply model"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnApplyModelClicked))
						]
						+ SHorizontalBox::Slot().AutoWidth()
						[
							BuildToolbarButton(LOCTEXT("UseDefaultModel", "Use Codex default"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnUseDefaultModelClicked))
						]
					]
				]
			];
	}

	FReply OnApplyModelClicked()
	{
		if (!CanSelectModel())
		{
			SetLastAction(LOCTEXT("ModelUnavailable", "The selected backend does not expose a verified model-selection capability."));
			return FReply::Handled();
		}
		const FString Model = ModelTextBox.IsValid() ? ModelTextBox->GetText().ToString().TrimStartAndEnd() : FString();
		FWorldDataAgentBackendFactory::SetCodexAppServerModel(Model);
		RestartAgentBackend(Model.IsEmpty()
			? LOCTEXT("ModelResetAction", "Codex default model selected. A new backend thread will be created on the next message.")
			: FText::Format(LOCTEXT("ModelChangedAction", "Codex model changed to {0}. A new backend thread will be created on the next message."), FText::FromString(Model)));
		FSlateApplication::Get().DismissAllMenus();
		return FReply::Handled();
	}

	FReply OnUseDefaultModelClicked()
	{
		if (ModelTextBox.IsValid())
		{
			ModelTextBox->SetText(FText::GetEmpty());
		}
		return OnApplyModelClicked();
	}

	TSharedRef<SWidget> BuildModeCombo()
	{
		return SNew(SComboButton)
			.ToolTipText(LOCTEXT("ModeComboTooltip", "Choose the host execution mode for the selected conversation backend."))
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
					.Text(LOCTEXT("ModeComboLabel", "模式"))
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
					.Font(GetHtmlParityFont(11, true))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text_Lambda([this] { return GetModeLabel(CurrentMode); })
					.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
					.Font(GetHtmlParityFont(12))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ModeComboChevron", "v"))
					.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
					.Font(GetHtmlParityFont(11))
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
			.BorderImage(&HtmlLargeCardBrush)
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
				if (AgentBackend.IsValid())
				{
					AgentBackend->SetPermissionMode(CurrentMode);
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
		return SNew(SButton)
			.ButtonStyle(&ComposerButtonStyle)
			.ContentPadding(FMargin(0.0f))
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			.Cursor(bClickable ? EMouseCursor::Hand : EMouseCursor::Default)
			.IsEnabled(bClickable)
			.OnClicked(OnClicked)
			[
				SNew(SBorder)
				.Padding(0.0f)
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this, bActive] { return FSlateColor(bActive ? GetAccentFillColor(0.22f) : FLinearColor::Transparent); })
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

	TSharedRef<SWidget> BuildConversationMessageWidget(const int32 MessageIndex) const
	{
		if (!ConversationMessages.IsValidIndex(MessageIndex))
		{
			return SNullWidget::NullWidget;
		}

		const FConversationMessage& Message = ConversationMessages[MessageIndex];
		const bool bUser = Message.Role == EConversationMessageRole::User;
		const EHorizontalAlignment BubbleAlign = bUser ? HAlign_Right : HAlign_Left;
		const FString DisplayText = Message.Text.IsEmpty() && Message.bStreaming
			? FString(TEXT("正在思考..."))
			: Message.Text;

		return SNew(SBox)
			.HAlign(BubbleAlign)
			[
				SNew(SBox)
				.MaxDesiredWidth(Message.Role == EConversationMessageRole::Error ? 460.0f : 860.0f)
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
								.Text_Lambda([this, MessageIndex, DisplayText]()
								{
									if (!ConversationMessages.IsValidIndex(MessageIndex))
									{
										return FText::FromString(DisplayText);
									}

									const FConversationMessage& CurrentMessage = ConversationMessages[MessageIndex];
									return CurrentMessage.Text.IsEmpty() && CurrentMessage.bStreaming
										? LOCTEXT("ConversationThinking", "正在思考...")
										: FText::FromString(CurrentMessage.Text);
								})
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
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(UEBridgeMCP::Palette::Blend(GetPanelBorderColor(), UEBridgeMCP::Palette::Warning(), 0.55f)); })
			[
				SNew(SBorder)
				.Padding(FMargin(12.0f, 10.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this] { return FSlateColor(UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), UEBridgeMCP::Palette::Warning(), 0.09f)); })
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
							LOCTEXT("PermissionDenyButton", "拒绝"),
							false,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnDenyPermissionClicked))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						BuildPermissionActionButton(
							LOCTEXT("PermissionAllowButton", "允许"),
							true,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnAllowPermissionClicked))
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
				BuildCodexAcpSetupPanel()
			]
			;
	}

	TSharedRef<SWidget> BuildCodexAcpSetupPanel()
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
					.Text(LOCTEXT("CodexAcpSetupTitle", "内嵌 Codex ACP"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
					.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f, 0.0f, 10.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CodexAcpSetupDescription", "检测已固定的原生适配器；若缺失，下载项目本地的 Codex ACP、计算 SHA-256 并立即配置。"))
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
					.AutoWrapText(true)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text_Lambda([this] { return FText::FromString(CodexAcpSetupStatus); })
						.ColorAndOpacity_Lambda([this]
						{
							return FSlateColor(bCodexAcpConfigured ? UEBridgeMCP::Palette::Success() : UEBridgeMCP::Palette::Warning());
						})
						.AutoWrapText(true)
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(12.0f, 0.0f, 0.0f, 0.0f)
					[
						BuildToolbarButton(
							LOCTEXT("CodexAcpSetupButton", "检测并自动配置"),
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnConfigureCodexAcpClicked),
							TAttribute<bool>::CreateLambda([this]
							{
								return !bCodexAcpSetupInProgress && (!AgentBackend.IsValid() || !AgentBackend->IsProcessing());
							}))
					]
				]
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
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				BuildSecurityCard()
			]
			];
	}

	bool GetNextMcpApproval(FWorldDataMCPApprovalSummary& OutApproval) const
	{
		const TArray<FWorldDataMCPApprovalSummary> Approvals = ApprovalController.IsValid()
			? ApprovalController->GetPendingApprovals()
			: TArray<FWorldDataMCPApprovalSummary>();
		if (Approvals.IsEmpty())
		{
			return false;
		}
		OutApproval = Approvals[0];
		return true;
	}

	bool HasPendingMcpApproval() const
	{
		FWorldDataMCPApprovalSummary Approval;
		return GetNextMcpApproval(Approval);
	}

	TSharedRef<SWidget> BuildMcpApprovalCard()
	{
		return SNew(SUEBridgeMCPApprovalView)
			.Summary_Lambda([this]
			{
				FWorldDataMCPApprovalSummary Approval;
				if (!GetNextMcpApproval(Approval)) return FText::GetEmpty();
				return FText::FromString(FString::Printf(TEXT("Tool: %s  |  Risk: %s  |  Expires UTC: %s"), *Approval.ToolName, *Approval.Risk, *Approval.ExpiresAtUtc.ToIso8601()));
			})
			.Details_Lambda([this]
			{
				FWorldDataMCPApprovalSummary Approval;
				if (!GetNextMcpApproval(Approval)) return FText::GetEmpty();
				const FString Status = Approval.bReadyForDecision ? TEXT("Ready for decision") : TEXT("Capturing target revision");
				return FText::FromString(FString::Printf(TEXT("%s\nChange hash: %s\nTarget revision: %s\n%s"), *Approval.TargetSummary, *Approval.ChangeSummaryHash.Left(16), *Approval.TargetRevision.Left(16), *Status));
			})
			.CanApprove_Lambda([this]
			{
				FWorldDataMCPApprovalSummary Approval;
				return GetNextMcpApproval(Approval) && Approval.bReadyForDecision;
			})
			.OnDeny(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnResolveMcpApprovalClicked, false))
			.OnApprove(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnResolveMcpApprovalClicked, true));
	}
	FReply OnResolveMcpApprovalClicked(bool bApprove)
	{
		FWorldDataMCPApprovalSummary Approval;
		if (!GetNextMcpApproval(Approval))
		{
			return FReply::Handled();
		}

		FString Error;
		const bool bResolved = ApprovalController.IsValid() && ApprovalController->Resolve(Approval.ApprovalId, bApprove, Error);
		SetLastAction(bResolved
			? (bApprove ? LOCTEXT("McpApprovalGranted", "MCP change approved; the background job has resumed.") : LOCTEXT("McpApprovalDenied", "MCP change denied."))
			: FText::FromString(Error));
		return FReply::Handled();
	}

	TSharedRef<SWidget> BuildSecurityCard()
	{
		return SNew(SBorder)
			.Padding(12.0f)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.10f)); })
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
						.Text(LOCTEXT("SecurityTitle", "Security"))
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetReadableAccentTextColor()); })
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text_Lambda([]
						{
							return GetWorldDataMCPService().IsUnsafePythonEnabled()
								? LOCTEXT("UnsafePythonEnabled", "Unsafe Python is enabled for this project. Copying the capability token is an explicit, per-editor-session approval.")
								: LOCTEXT("UnsafePythonDisabled", "Unsafe Python is disabled. Enable it explicitly with [UEBridgeMCP.Security] bEnableUnsafePython=true in the project configuration.");
						})
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelSubduedTextColor()); })
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(12.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 6.0f, 0.0f)
					[
						BuildToolbarButton(
							LOCTEXT("RotateMcpToken", "Rotate MCP Token"),
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnRotateAccessTokenClicked),
							TAttribute<bool>::CreateLambda([] { return GetWorldDataMCPService().IsRunning(); }))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						BuildToolbarButton(
							LOCTEXT("CopyUnsafePythonToken", "Copy Python Token"),
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnCopyUnsafePythonTokenClicked),
							TAttribute<bool>::CreateLambda([] { return GetWorldDataMCPService().IsUnsafePythonEnabled() && GetWorldDataMCPService().IsRunning(); }))
					]
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
							return GetWorldDataMCPService().IsRunning()
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
						.IsEnabled_Lambda([this] { return !GetWorldDataMCPService().IsRunning(); })
						.Text_Lambda([this] { return FText::FromString(ViewModel->ServerPortText); })
						.HintText(LOCTEXT("McpPortHint", "例如 7275"))
						.OnTextChanged_Lambda([this](const FText& Text) { ViewModel->ServerPortText = Text.ToString(); })
						.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type) { ViewModel->ServerPortText = Text.ToString(); })
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
							TAttribute<bool>::CreateLambda([this] { return !GetWorldDataMCPService().IsRunning(); }))
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
		if (GetWorldDataMCPService().IsRunning())
		{
			return FText::Format(
				LOCTEXT("McpStatusRunning", "运行中 — port {0}"),
				FText::FromString(FString::FromInt(GetWorldDataMCPService().GetPort())));
		}
		return LOCTEXT("McpStatusStopped", "已停止");
	}

	FText GetServerToggleText() const
	{
		return GetWorldDataMCPService().IsRunning()
			? LOCTEXT("McpToggleStop", "停止")
			: LOCTEXT("McpToggleStart", "启动");
	}

	int32 ParseServerPort() const
	{
		const int32 Port = FCString::Atoi(*ViewModel->ServerPortText);
		return (Port >= 1024 && Port <= 65535) ? Port : GetWorldDataMCPService().LoadConfiguredPort();
	}

	const TArray<FString>& GetRegisteredToolNames()
	{
		if (CachedToolNames.Num() == 0)
		{
			const FString Json = GetWorldDataMCPService().GetToolDefinitionsJson();
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
		if (GetWorldDataMCPService().IsRunning())
		{
			const FText Confirmation = LOCTEXT("StopMcpServerConfirmation", "Stopping the MCP server affects every console window and external MCP client. Continue?");
			if (FMessageDialog::Open(EAppMsgType::YesNo, Confirmation) != EAppReturnType::Yes)
			{
				return FReply::Handled();
			}
			GetWorldDataMCPService().Stop();
			SetLastAction(LOCTEXT("McpStoppedAction", "已停止 MCP 服务器。"));
			return FReply::Handled();
		}

		GetWorldDataMCPService().Start(ParseServerPort());
		if (GetWorldDataMCPService().IsRunning())
		{
			GetWorldDataMCPService().RefreshConnectionFiles();
			ViewModel->ServerPortText = FString::FromInt(GetWorldDataMCPService().GetPort());
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
		if (GetWorldDataMCPService().IsRunning())
		{
			SetLastAction(LOCTEXT("McpPortNeedsStopAction", "请先停止服务器再更改端口。"));
			return FReply::Handled();
		}

		GetWorldDataMCPService().Start(ParseServerPort());
		if (GetWorldDataMCPService().IsRunning())
		{
			GetWorldDataMCPService().RefreshConnectionFiles();
			ViewModel->ServerPortText = FString::FromInt(GetWorldDataMCPService().GetPort());
			SetLastAction(FText::Format(
				LOCTEXT("McpPortAppliedAction", "已在端口 {0} 启动。"),
				FText::FromString(FString::FromInt(GetWorldDataMCPService().GetPort()))));
		}
		else
		{
			SetLastAction(LOCTEXT("McpPortApplyFailedAction", "应用端口失败，服务器未能启动。"));
		}
		return FReply::Handled();
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
				.BorderImage(&HtmlLargeCardBrush)
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
							.Text(LOCTEXT("ModelText", "模型由 Adapter 决定"))
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
		const FSlateBrush NormalBrush = FSlateRoundedBoxBrush(FSlateColor(GetPanelBackgroundColor()), 10.0f);
		const FSlateBrush HoveredBrush = FSlateRoundedBoxBrush(FSlateColor(GetPanelSurfaceColor()), 10.0f);
		const FSlateBrush FocusedBrush = FSlateRoundedBoxBrush(FSlateColor(GetPanelBackgroundColor()), 10.0f, FSlateColor(GetAccentBorderColor()), 1.0f);

		LightTextBoxStyle
			.SetBackgroundImageNormal(NormalBrush)
			.SetBackgroundImageHovered(HoveredBrush)
			.SetBackgroundImageFocused(FocusedBrush)
			.SetBackgroundImageReadOnly(NormalBrush);

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
		const FSlateBrush NormalBrush = FSlateRoundedBoxBrush(FSlateColor(UEBridgeMCP::Palette::Background()), 7.0f);
		const FSlateBrush HoveredBrush = FSlateRoundedBoxBrush(FSlateColor(UEBridgeMCP::Palette::ControlHover()), 7.0f);
		const FSlateBrush PressedBrush = FSlateRoundedBoxBrush(FSlateColor(UEBridgeMCP::Palette::ControlPressed()), 7.0f);
		const FSlateBrush DisabledBrush = FSlateRoundedBoxBrush(FSlateColor(UEBridgeMCP::Palette::Surface()), 7.0f);

		ComposerButtonStyle
			.SetNormal(NormalBrush)
			.SetHovered(HoveredBrush)
			.SetPressed(PressedBrush)
			.SetDisabled(DisabledBrush);

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

	void LoadSettings()
	{
		SettingsColor = FLinearColor::White;

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

		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("color"), ColorObject);

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
		ViewModel->CurrentDetailText = PrettyText;
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
		ViewModel->LastAction = Text;
	}

	void RebuildConversationMessages()
	{
		if (!ConversationScrollBox.IsValid())
		{
			return;
		}

		ConversationScrollBox->ClearChildren();
		for (int32 MessageIndex = 0; MessageIndex < ConversationMessages.Num(); ++MessageIndex)
		{
			ConversationScrollBox->AddSlot()
				.Padding(FMargin(0.0f, 0.0f, 0.0f, 10.0f))
				[
					BuildConversationMessageWidget(MessageIndex)
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

	void TrimConversationHistory()
	{
		static constexpr int32 MaxMessagesPerConversation = 128;
		static constexpr int32 MaxMessageCharacters = 128 * 1024;
		static constexpr int32 MaxTranscriptCharacters = 256 * 1024;

		const int32 RemovedMessageCount = FMath::Max(0, ConversationMessages.Num() - MaxMessagesPerConversation);
		if (RemovedMessageCount > 0)
		{
			ConversationMessages.RemoveAt(0, RemovedMessageCount);
			if (ActiveAssistantMessageIndex != INDEX_NONE)
			{
				ActiveAssistantMessageIndex -= RemovedMessageCount;
				if (!ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
				{
					ActiveAssistantMessageIndex = INDEX_NONE;
				}
			}
		}

		for (FConversationMessage& Message : ConversationMessages)
		{
			if (Message.Text.Len() > MaxMessageCharacters)
			{
				Message.Text = TEXT("[Earlier content in this message was trimmed.]\n")
					+ Message.Text.Right(MaxMessageCharacters);
			}
		}

		if (ConversationTranscript.Len() > MaxTranscriptCharacters)
		{
			ConversationTranscript = TEXT("[Earlier conversation content was trimmed to keep this editor session responsive.]\n")
				+ ConversationTranscript.Right(MaxTranscriptCharacters);
		}
	}

	void TrimStoredConversations()
	{
		static constexpr int32 MaxStoredConversations = 32;
		if (Conversations.Num() > MaxStoredConversations)
		{
			Conversations.SetNum(MaxStoredConversations);
		}
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
		TrimConversationHistory();
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
		TrimConversationHistory();
		RefreshConversationText(false);
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
		TrimConversationHistory();
		RefreshConversationText();
	}

	void FinalizeActiveRequestAsCancelled()
	{
		if (!AgentBackend.IsValid() || !AgentBackend->IsProcessing())
		{
			return;
		}

		if (ActiveAssistantMessageIndex != INDEX_NONE && ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
		{
			ConversationMessages[ActiveAssistantMessageIndex].bStreaming = false;
		}
		ActiveAssistantMessageIndex = INDEX_NONE;

		const FString CancellationMessage = TEXT("上一条 Codex 请求已取消。");
		AddConversationMessage(EConversationMessageRole::System, CancellationMessage);
		if (!ConversationTranscript.IsEmpty())
		{
			ConversationTranscript += TEXT("\n\n");
		}
		ConversationTranscript += FString::Printf(TEXT("[系统] %s\n"), *CancellationMessage);
		TrimConversationHistory();
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

	void RefreshConversationText(const bool bRebuildMessages = true)
	{
		bShowSettings = false;
		bShowDetail = true;
		bDetailIsConversation = true;
		ViewModel->CurrentDetailText = ConversationTranscript;
		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(LOCTEXT("CodexConversationTitle", "Codex 会话"));
		}
		if (bRebuildMessages)
		{
			RebuildConversationMessages();
		}
		else if (ConversationScrollBox.IsValid())
		{
			ConversationScrollBox->ScrollToEnd();
		}
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
		if (AgentBackend.IsValid())
		{
			AgentBackend->RespondToPermission(PendingPermissionId, SelectedOptionId);
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
		if (AgentBackend.IsValid())
		{
			AgentBackend->Stop();
		}

		FConversation NewConversation;
		NewConversation.Title = LOCTEXT("NewConversationTitle", "新对话");
		NewConversation.CreatedAt = FDateTime::Now();
		NewConversation.TaskId = FString::Printf(TEXT("task_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		NewConversation.ThreadId = FString::Printf(TEXT("thread_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		Conversations.Insert(NewConversation, 0);
		TrimStoredConversations();
		ActiveConversationIndex = 0;

		bShowSettings = false;
		bShowDetail = false;
		bDetailIsConversation = false;
		ClearPendingPermission();
		ViewModel->CurrentDetailText.Empty();
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
		// Conversation history is local to this panel. Do not attach a restored
		// transcript to the ACP context from another conversation.
		if (AgentBackend.IsValid())
		{
			AgentBackend->Stop();
		}
		ActiveConversationIndex = Index;

		const FConversation& Conversation = Conversations[Index];
		ConversationMessages = Conversation.Messages;
		ConversationTranscript = Conversation.Transcript;
		ActiveAssistantMessageIndex = Conversation.ActiveAssistantMessageIndex;
		TrimConversationHistory();
		ViewModel->CurrentDetailText = ConversationTranscript;

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
		SetLastAction(FText::Format(LOCTEXT("SwitchedConversationAction", "已切换到对话：{0}。继续发送将创建独立的 Codex 会话。"), GetConversationTitle(Index)));
	}

	FReply OnSelectConversation(int32 Index)
	{
		if (Index != ActiveConversationIndex)
		{
			const bool bCancellingActiveRequest = AgentBackend.IsValid() && AgentBackend->IsProcessing();
			if (bCancellingActiveRequest)
			{
				FinalizeActiveRequestAsCancelled();
			}
			LoadConversation(Index);
			if (bCancellingActiveRequest)
			{
				SetLastAction(LOCTEXT("ConversationSwitchedAndCancelledAction", "已切换对话，上一条 Codex 请求已取消。"));
			}
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
		SetDetail(LOCTEXT("ProjectInfoTitle", "项目信息"), GetWorldDataMCPService().GetProjectInfoJson());
	}

	FReply OnNewConversationClicked()
	{
		if (AgentBackend.IsValid() && AgentBackend->IsProcessing())
		{
			const FText Confirmation = LOCTEXT("NewConversationCancelsCurrentConfirmation", "Starting a new conversation cancels the active Codex request in this window only. Continue?");
			if (FMessageDialog::Open(EAppMsgType::YesNo, Confirmation) != EAppReturnType::Yes)
			{
				return FReply::Handled();
			}
			FinalizeActiveRequestAsCancelled();
		}
		ResetConversationView();
		return FReply::Handled();
	}

	FReply OnSettingsClicked()
	{
		RefreshCodexAcpSetupStatus();
		bShowSettings = true;
		SetLastAction(LOCTEXT("SettingsOpenedAction", "已打开设置。"));
		return FReply::Handled();
	}

	FReply OnShowChatClicked()
	{
		bShowSettings = false;
		SetLastAction(LOCTEXT("HtmlParityChatOpened", "已返回聊天。"));
		return FReply::Handled();
	}

	FReply OnRetryConversationClicked()
	{
		if (!ComposerTextBox.IsValid() || !HasRetryableError())
		{
			return FReply::Handled();
		}

		for (int32 Index = ConversationMessages.Num() - 1; Index >= 0; --Index)
		{
			if (ConversationMessages[Index].Role == EConversationMessageRole::User)
			{
				ComposerTextBox->SetText(FText::FromString(ConversationMessages[Index].Text));
				SetLastAction(LOCTEXT("HtmlParityRetryPrepared", "已将上一条请求填入输入框，可再次发送。"));
				break;
			}
		}
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

		if (AgentBackend.IsValid() && AgentBackend->IsProcessing())
		{
			SetLastAction(LOCTEXT("CodexBusyAction", "Codex 正在处理上一条消息，请稍后再发送。"));
			return FReply::Handled();
		}

		StartConversationTurn(Message);

		if (ComposerTextBox.IsValid())
		{
			ComposerTextBox->SetText(FText::GetEmpty());
		}

		if (!AgentBackend.IsValid())
		{
			HandleAcpError(TEXT("Configured Agent Backend is not initialized."));
			return FReply::Handled();
		}

		SetLastAction(FText::FromString(FString::Printf(TEXT("Sending to %s..."), *AgentBackend->GetDisplayName())));
		AgentBackend->SetPermissionMode(CurrentMode);
		if (Conversations.IsValidIndex(ActiveConversationIndex))
		{
			FConversation& Conversation = Conversations[ActiveConversationIndex];
			if (Conversation.TaskId.IsEmpty())
			{
				Conversation.TaskId = FString::Printf(TEXT("task_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			}
			if (Conversation.ThreadId.IsEmpty())
			{
				Conversation.ThreadId = FString::Printf(TEXT("thread_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
			}
			AgentBackend->SetConversationIdentity(Conversation.TaskId, Conversation.ThreadId);
		}
		AgentBackend->SendPrompt(Message);
		return FReply::Handled();
	}

	FReply OnStartClicked()
	{
		if (!GetWorldDataMCPService().IsRunning())
		{
			GetWorldDataMCPService().Start(GetWorldDataMCPService().LoadConfiguredPort());
		}
		if (GetWorldDataMCPService().IsRunning())
		{
			GetWorldDataMCPService().RefreshConnectionFiles();
		}
		SetLastAction(GetWorldDataMCPService().IsRunning() ? LOCTEXT("StartedAction", "已启动并刷新连接文件。") : LOCTEXT("StartFailedAction", "启动失败。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnStopClicked()
	{
		GetWorldDataMCPService().Stop();
		SetLastAction(LOCTEXT("StoppedAction", "已停止。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnRefreshClicked()
	{
		GetWorldDataMCPService().RefreshConnectionFiles();
		SetLastAction(LOCTEXT("RefreshedAction", "连接文件已刷新。"));
		ShowProjectInfo();
		return FReply::Handled();
	}

	FReply OnSetupCliClicked()
	{
		if (!GetWorldDataMCPService().IsRunning())
		{
			GetWorldDataMCPService().Start(GetWorldDataMCPService().LoadConfiguredPort());
		}

		if (!GetWorldDataMCPService().IsRunning())
		{
			SetLastAction(LOCTEXT("SetupCliStartFailedAction", "MCP 服务器启动失败，无法配置 CLI。"));
			SetDetail(LOCTEXT("SetupCliFailTitle", "一键配置 CLI 失败"), GetWorldDataMCPService().GetStatusJson());
			return FReply::Handled();
		}

		GetWorldDataMCPService().ProvisionClientConfigurations();
		SetLastAction(LOCTEXT("SetupCliDoneAction", "已更新允许写入的本地 CLI 连接配置；项目级配置默认关闭以避免在工作区保存 Token。"));
		UEBridgeMCP::Notify(LOCTEXT("SetupCliDoneNotification", "本地 CLI 连接配置已更新；项目级配置依安全策略决定是否生成。"));
		SetDetail(LOCTEXT("SetupCliReportTitle", "CLI 配置结果"), GetWorldDataMCPService().GetCliSetupReportJson());
		return FReply::Handled();
	}

	FReply OnShowAgentBackendDiagnosticsClicked()
	{
		SetDetail(LOCTEXT("AgentBackendDiagnosticsTitle", "Agent Backend Diagnostics"), FWorldDataAgentBackendFactory::GetDiagnosticsJson());
		SetLastAction(LOCTEXT("AgentBackendDiagnosticsAction", "Displayed Agent Backend diagnostics."));
		return FReply::Handled();
	}

	void RefreshCodexAcpSetupStatus()
	{
		const FWorldDataAcpProfileConfiguration Configuration = FWorldDataCodexACPClient::GetProfileConfiguration(TEXT("codex"));
		bCodexAcpConfigured = Configuration.bIsConfigured;
		if (Configuration.bIsConfigured)
		{
			CodexAcpSetupStatus = FString::Printf(TEXT("已配置并验证：%s"), *Configuration.ExecutablePath);
		}
		else if (!Configuration.FailureReason.IsEmpty())
		{
			CodexAcpSetupStatus = FString::Printf(TEXT("需要配置：%s"), *Configuration.FailureReason);
		}
		else
		{
			CodexAcpSetupStatus = TEXT("需要配置 Codex ACP。");
		}
	}

	FReply OnConfigureCodexAcpClicked()
	{
		if (bCodexAcpSetupInProgress)
		{
			return FReply::Handled();
		}

		RefreshCodexAcpSetupStatus();
		if (bCodexAcpConfigured)
		{
			const FString BackendId = FWorldDataAgentBackendFactory::GetConfiguredBackendId();
			if (BackendId == TEXT("auto") || BackendId == TEXT("acp_codex"))
			{
				RestartAgentBackend(LOCTEXT("CodexAcpAlreadyReadyAction", "Codex ACP 已验证；已重新初始化聊天后端。"));
			}
			else
			{
				SetLastAction(LOCTEXT("CodexAcpAlreadyConfiguredAction", "Codex ACP 已配置并验证。"));
			}
			return FReply::Handled();
		}

		bCodexAcpSetupInProgress = true;
		CodexAcpSetupStatus = TEXT("正在检测或下载 Codex ACP…");
		SetLastAction(LOCTEXT("CodexAcpSetupStartedAction", "正在检测 Codex ACP；若未安装将下载到项目 Saved 目录。"));

		const TWeakPtr<SUEBridgeMCPPanel> WeakThis = StaticCastSharedRef<SUEBridgeMCPPanel>(AsShared());
		Async(EAsyncExecution::ThreadPool, [WeakThis]()
		{
			FWorldDataCodexAcpBootstrapResult Result = FWorldDataCodexAcpBootstrap::FindOrInstall();
			AsyncTask(ENamedThreads::GameThread, [WeakThis, Result = MoveTemp(Result)]() mutable
			{
				const TSharedPtr<SUEBridgeMCPPanel> Self = WeakThis.Pin();
				if (!Self.IsValid())
				{
					return;
				}

				Self->bCodexAcpSetupInProgress = false;
				if (!Result.bSuccess)
				{
					Self->bCodexAcpConfigured = false;
					Self->CodexAcpSetupStatus = Result.Message.IsEmpty() ? TEXT("Codex ACP 自动配置失败。") : Result.Message;
					Self->SetLastAction(FText::FromString(Self->CodexAcpSetupStatus));
					UEBridgeMCP::Notify(LOCTEXT("CodexAcpSetupFailedNotification", "Codex ACP 自动配置失败。"));
					return;
				}

				FString PinError;
				if (!FWorldDataCodexACPClient::PinProfileExecutable(TEXT("codex"), Result.ExecutablePath, PinError))
				{
					Self->bCodexAcpConfigured = false;
					Self->CodexAcpSetupStatus = FString::Printf(TEXT("适配器已找到，但安全固定失败：%s"), *PinError);
					Self->SetLastAction(FText::FromString(Self->CodexAcpSetupStatus));
					UEBridgeMCP::Notify(LOCTEXT("CodexAcpPinFailedNotification", "Codex ACP 的安全配置失败。"));
					return;
				}

				Self->RefreshCodexAcpSetupStatus();
				const FString BackendId = FWorldDataAgentBackendFactory::GetConfiguredBackendId();
				if (BackendId == TEXT("auto") || BackendId == TEXT("acp_codex"))
				{
					Self->RestartAgentBackend(Result.bInstalled
						? LOCTEXT("CodexAcpDownloadedAction", "Codex ACP 已下载、验证并配置；聊天后端已重新初始化。")
						: LOCTEXT("CodexAcpDetectedAction", "Codex ACP 已检测、验证并配置；聊天后端已重新初始化。"));
				}
				else
				{
					Self->SetLastAction(Result.bInstalled
						? LOCTEXT("CodexAcpDownloadedConfiguredAction", "Codex ACP 已下载、验证并配置。")
						: LOCTEXT("CodexAcpDetectedConfiguredAction", "Codex ACP 已检测、验证并配置。"));
				}
				UEBridgeMCP::Notify(LOCTEXT("CodexAcpSetupCompleteNotification", "Codex ACP 已配置完成。"));
			});
		});
		return FReply::Handled();
	}

	FReply OnRotateAccessTokenClicked()
	{
		const FText Confirmation = LOCTEXT("RotateMcpTokenConfirmation", "This revokes all active MCP sessions and replaces the token in configured local clients. Continue?");
		if (FMessageDialog::Open(EAppMsgType::YesNo, Confirmation) != EAppReturnType::Yes)
		{
			return FReply::Handled();
		}

		FString Error;
		if (!GetWorldDataMCPService().RotateAccessToken(Error))
		{
			SetLastAction(FText::FromString(Error));
			return FReply::Handled();
		}

		// This button is an explicit user action, so it may update local client
		// files with the newly rotated token.
		GetWorldDataMCPService().ProvisionClientConfigurations();
		SetLastAction(LOCTEXT("RotateMcpTokenDone", "MCP token rotated and local client configuration updated."));
		UEBridgeMCP::Notify(LOCTEXT("RotateMcpTokenNotification", "MCP token rotated. Existing clients must reconnect."));
		SetDetail(LOCTEXT("RotateMcpTokenReport", "MCP Token Rotation"), GetWorldDataMCPService().GetCliSetupReportJson());
		return FReply::Handled();
	}

	FReply OnStatusClicked()
	{
		SetLastAction(LOCTEXT("ViewedStatusAction", "已查看状态。"));
		SetDetail(LOCTEXT("StatusTitle", "服务状态"), GetWorldDataMCPService().GetStatusJson());
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
		SetDetail(LOCTEXT("BootstrapTitle", "启动上下文"), GetWorldDataMCPService().ReadResource(TEXT("worlddata://context/bootstrap")));
		return FReply::Handled();
	}

	FReply OnPolicyClicked()
	{
		SetLastAction(LOCTEXT("ViewedPolicyAction", "已查看 Codex 策略快照。"));
		SetDetail(LOCTEXT("PolicyTitle", "Codex 策略快照"), GetWorldDataMCPService().ReadResource(TEXT("worlddata://codex/policy-snapshot")));
		return FReply::Handled();
	}

	FReply OnToolsClicked()
	{
		SetLastAction(LOCTEXT("ViewedToolsAction", "已查看工具列表。"));
		SetDetail(LOCTEXT("ToolsTitle", "工具列表"), GetWorldDataMCPService().GetToolDefinitionsJson());
		return FReply::Handled();
	}

	FReply OnResourcesClicked()
	{
		SetLastAction(LOCTEXT("ViewedResourcesAction", "已查看资源列表。"));
		SetDetail(LOCTEXT("ResourcesTitle", "资源列表"), GetWorldDataMCPService().GetResourceListJson());
		return FReply::Handled();
	}

	FReply OnCopyUrlClicked()
	{
		if (GetWorldDataMCPService().IsRunning())
		{
			UEBridgeMCP::CopyToClipboard(GetWorldDataMCPService().GetMcpUrl());
			SetLastAction(LOCTEXT("CopiedUrlAction", "已复制 MCP 地址。"));
			UEBridgeMCP::Notify(LOCTEXT("CopiedUrlNotification", "MCP 地址已复制。"));
		}
		return FReply::Handled();
	}

	FReply OnCopyConfigClicked()
	{
		if (GetWorldDataMCPService().IsRunning())
		{
			UEBridgeMCP::CopyToClipboard(UEBridgeMCP::BuildClientConfigSnippet());
			SetLastAction(LOCTEXT("CopiedConfigAction", "已复制 MCP 配置。"));
			UEBridgeMCP::Notify(LOCTEXT("CopiedConfigNotification", "MCP 配置已复制。"));
		}
		return FReply::Handled();
	}

	FReply OnCopyUnsafePythonTokenClicked()
	{
		const FString Token = GetWorldDataMCPService().GetUnsafePythonCapabilityToken();
		if (Token.IsEmpty())
		{
			SetLastAction(LOCTEXT("UnsafePythonTokenUnavailable", "Unsafe Python is disabled or the server is not running."));
			return FReply::Handled();
		}

		UEBridgeMCP::CopyToClipboard(Token);
		SetLastAction(LOCTEXT("UnsafePythonTokenCopied", "A Python capability token was copied. It expires in ten minutes or when the editor MCP server stops."));
		UEBridgeMCP::Notify(LOCTEXT("UnsafePythonTokenCopiedNotification", "Python capability token copied."));
		return FReply::Handled();
	}

	FReply OnCopyCurrentClicked()
	{
		if (!ViewModel->CurrentDetailText.IsEmpty())
		{
			UEBridgeMCP::CopyToClipboard(ViewModel->CurrentDetailText);
			SetLastAction(LOCTEXT("CopiedViewAction", "已复制当前内容。"));
			UEBridgeMCP::Notify(LOCTEXT("CopiedViewNotification", "当前面板内容已复制。"));
		}
		return FReply::Handled();
	}

	FReply OnOpenProjectFolderClicked()
	{
		UEBridgeMCP::ExploreFileParent(GetWorldDataMCPService().GetClientConfigFilePath());
		SetLastAction(LOCTEXT("OpenedProjectFolderAction", "已打开项目目录。"));
		return FReply::Handled();
	}

	FReply OnOpenSavedFolderClicked()
	{
		UEBridgeMCP::ExploreFileParent(GetWorldDataMCPService().GetConnectionFilePath());
		SetLastAction(LOCTEXT("OpenedSavedFolderAction", "已打开 Saved 目录。"));
		return FReply::Handled();
	}

	TUniquePtr<FUEBridgeMCPPanelViewModel> ViewModel;
	FString ConversationTranscript;
	TArray<FConversationMessage> ConversationMessages;
	FLinearColor SettingsColor = FLinearColor::White;
	FString CodexAcpSetupStatus;
	bool bCodexAcpConfigured = false;
	bool bCodexAcpSetupInProgress = false;
	FSlateRoundedBoxBrush HtmlShellBrush{ FLinearColor::White, 20.0f };
	FSlateRoundedBoxBrush HtmlLargeCardBrush{ FLinearColor::White, 16.0f };
	FSlateRoundedBoxBrush HtmlCardBrush{ FLinearColor::White, 12.0f };
	FSlateRoundedBoxBrush HtmlControlBrush{ FLinearColor::White, 8.0f };
	FEditableTextBoxStyle LightTextBoxStyle;
	FButtonStyle ComposerButtonStyle;
	FComboButtonStyle ComposerComboButtonStyle;
	EWorldDataCodexPermissionMode CurrentMode = EWorldDataCodexPermissionMode::Default;
	TSharedPtr<IWorldDataAgentBackend> AgentBackend;
	TUniquePtr<FUEBridgeMCPAgentController> AgentController;
	TUniquePtr<FUEBridgeMCPApprovalController> ApprovalController;
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
	TArray<FString> CachedToolNames;
	TSharedPtr<STextBlock> DetailTitleText;
	TSharedPtr<SMultiLineEditableTextBox> DetailTextBox;
	TSharedPtr<SMultiLineEditableTextBox> ComposerTextBox;
	TSharedPtr<SEditableTextBox> ModelTextBox;
};

TSharedRef<SWidget> CreateUEBridgeMCPPanel()
{
	return SNew(SUEBridgeMCPWebPanel);
}

#undef LOCTEXT_NAMESPACE
