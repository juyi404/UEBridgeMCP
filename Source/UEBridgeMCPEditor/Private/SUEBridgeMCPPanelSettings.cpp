#include "SUEBridgeMCPPanel.h"

#include "WorldDataMCPServer.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Input/DragAndDrop.h"
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

DECLARE_DELEGATE_RetVal_TwoParams(FReply, FUEBridgeComposerDragDropHandler, const FGeometry&, const FDragDropEvent&);

class SUEBridgeComposerDropZone : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEBridgeComposerDropZone) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_EVENT(FUEBridgeComposerDragDropHandler, OnDragOver)
		SLATE_EVENT(FUEBridgeComposerDragDropHandler, OnDrop)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnDragOverHandler = InArgs._OnDragOver;
		OnDropHandler = InArgs._OnDrop;
		ChildSlot
		[
			InArgs._Content.Widget
		];
	}

	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
	{
		return OnDragOverHandler.IsBound()
			? OnDragOverHandler.Execute(MyGeometry, DragDropEvent)
			: FReply::Unhandled();
	}

	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
	{
		return OnDropHandler.IsBound()
			? OnDropHandler.Execute(MyGeometry, DragDropEvent)
			: FReply::Unhandled();
	}

private:
	FUEBridgeComposerDragDropHandler OnDragOverHandler;
	FUEBridgeComposerDragDropHandler OnDropHandler;
};

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildCliSettingsPanel()
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
				.Text(LOCTEXT("CliSettingsSubtitle", "配置本机 Codex CLI、Cursor Agent CLI 与 Claude Code CLI。未填写时会尝试从 PATH 自动发现。"))
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
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 12.0f, 0.0f, 12.0f)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				BuildCliSettingsRow(ECliTool::ClaudeCode)
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildCliSettingsRow(ECliTool Tool)
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildCliStatusBadge(ECliTool Tool) const
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildFocusConfirmBar()
{
	return SNew(SBox)
		.Visibility_Lambda([this]
		{
			return (ActiveFocusAwaitingConfirmation() || IsFocusAwaitingHumanInput(GetActiveConversationId())) ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.16f)); })
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("FocusConfirmHint", "专注模式：计划已就绪，确认执行？"))
					.Text_Lambda([this]
					{
						return IsFocusAwaitingHumanInput(GetActiveConversationId())
							? LOCTEXT("FocusHumanInputHint", "专注模式：请在输入框补充/选择，或直接继续下一轮。")
							: LOCTEXT("FocusConfirmReadyHint", "专注模式：计划已就绪，确认执行？");
					})
					.AutoWrapText(true)
					.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelTextColor()); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this] { return IsFocusAwaitingHumanInput(GetActiveConversationId()) ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						BuildPermissionActionButton(LOCTEXT("FocusContinueButton", "继续下一轮"), true,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnFocusContinueClicked))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(8.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this] { return ActiveFocusAwaitingConfirmation() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						BuildPermissionActionButton(LOCTEXT("FocusExecuteButton", "执行"), true,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnFocusExecuteClicked))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this] { return ActiveFocusAwaitingConfirmation() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						BuildPermissionActionButton(LOCTEXT("FocusReplanButton", "重新规划"), false,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnFocusReplanClicked))
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(6.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.Visibility_Lambda([this] { return ActiveFocusAwaitingConfirmation() ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						BuildPermissionActionButton(LOCTEXT("FocusCancelButton", "取消"), false,
							FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnFocusCancelClicked))
					]
				]
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildComposer()
{
	return SNew(SBorder)
		.Padding(1.0f)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetPanelBorderColor()); })
		[
			SNew(SBorder)
			.Padding(FMargin(10.0f, 9.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetPanelSurfaceColor()))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					BuildFocusConfirmBar()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBorder)
					.Padding(1.0f)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetPanelBorderColor()); })
					[
						SNew(SBorder)
						.Padding(0.0f)
						.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
						.BorderBackgroundColor(FSlateColor(GetPanelBackgroundColor()))
						[
							SNew(SUEBridgeComposerDropZone)
							.Cursor(EMouseCursor::Default)
							.OnDragOver(FUEBridgeComposerDragDropHandler::CreateSP(this, &SUEBridgeMCPPanel::OnComposerDragOver))
							.OnDrop(FUEBridgeComposerDragDropHandler::CreateSP(this, &SUEBridgeMCPPanel::OnComposerDrop))
							[
								SNew(SBox)
								.MinDesiredHeight(76.0f)
								.MaxDesiredHeight(240.0f)
								.Cursor(EMouseCursor::Default)
								[
									SAssignNew(ComposerTextBox, SMultiLineEditableTextBox)
									.Cursor(EMouseCursor::Default)
									.Style(&LightTextBoxStyle)
									.Text(FText::FromString(ComposerDraftText))
									.HintText(LOCTEXT("ComposerHint", "输入消息...（可拖拽、粘贴或选择文件）"))
									.AutoWrapText(true)
									.OnTextChanged_Lambda([this](const FText& Text)
									{
										ComposerDraftText = Text.ToString();
									})
									.OnKeyDownHandler(FOnKeyDown::CreateSP(this, &SUEBridgeMCPPanel::OnComposerKeyDown))
									.BackgroundColor(FSlateColor(GetPanelBackgroundColor()))
									.ForegroundColor(FSlateColor(GetPanelTextColor()))
								]
							]
						]
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 8.0f, 0.0f, 0.0f)
				[
					BuildComposerAttachmentList()
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 9.0f, 0.0f, 0.0f)
				[
					SNew(SBox)
					.MinDesiredHeight(32.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							BuildIconTextButton(LOCTEXT("AttachButton", "+"), FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnAttachFileClicked), LOCTEXT("AttachTooltip", "选择要附加的文件"))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							BuildAgentCombo()
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 6.0f, 0.0f)
						[
							BuildModelCombo()
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, 10.0f, 0.0f)
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
							SNew(SBox)
							.WidthOverride(42.0f)
							.HeightOverride(30.0f)
							[
								SNew(SButton)
								.HAlign(HAlign_Center)
								.VAlign(VAlign_Center)
								.ContentPadding(FMargin(0.0f))
								.ButtonStyle(&ComposerButtonStyle)
								.ButtonColorAndOpacity_Lambda([this]
								{
									return FSlateColor(IsComposerBusy() ? UEBridgeMCP::Palette::Danger() : GetAccentButtonColor());
								})
								.ForegroundColor_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
								.ToolTipText(LOCTEXT("SendTooltip", "发送到 Codex ACP"))
								.ToolTipText_Lambda([this]
								{
									return IsComposerBusy()
										? LOCTEXT("StopAnswerTooltip", "终止本次回答")
										: LOCTEXT("SendTooltipDynamic", "发送到 Codex ACP");
								})
								.OnClicked(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSendClicked))
								[
									BuildComposerActionContent()
								]
							]
						]
					]
				]
			]
	];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildComposerAttachmentList()
{
	TSharedRef<SWrapBox> WrapBox = SAssignNew(ComposerAttachmentWrapBox, SWrapBox)
		.UseAllottedSize(true)
		.Visibility_Lambda([this]
		{
			return ComposerAttachments.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
		});

	RebuildComposerAttachments();
	return WrapBox;
}

void SUEBridgeMCPPanel::FocusComposer()
{
	if (ComposerTextBox.IsValid())
	{
		FSlateApplication::Get().SetKeyboardFocus(ComposerTextBox, EFocusCause::SetDirectly);
	}
}

void SUEBridgeMCPPanel::ClearComposerDraft()
{
	ComposerDraftText.Reset();
	if (ComposerTextBox.IsValid())
	{
		ComposerTextBox->SetText(FText::GetEmpty());
	}
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildComposerAttachmentChip(int32 AttachmentIndex)
{
	if (!ComposerAttachments.IsValidIndex(AttachmentIndex))
	{
		return SNew(SBox);
	}

	const FComposerAttachment& Attachment = ComposerAttachments[AttachmentIndex];
	const FString StatusText = Attachment.bInlineContent
		? TEXT("已读取")
		: (Attachment.Error.IsEmpty() ? TEXT("仅路径") : Attachment.Error);
	const FString Label = FString::Printf(TEXT("%s  %s"), *Attachment.Name, *FormatAttachmentSize(Attachment.SizeBytes));
	const FString Tooltip = FString::Printf(TEXT("%s\n%s"), *Attachment.Path, *StatusText);

	return SNew(SBorder)
		.Padding(FMargin(8.0f, 4.0f))
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([this]
		{
			return FSlateColor(GetAccentFillColor(0.14f));
		})
		.ToolTipText(FText::FromString(Tooltip))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SBox)
				.MaxDesiredWidth(260.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.ColorAndOpacity(FSlateColor(GetPanelTextColor()))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(8.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(StatusText))
				.ColorAndOpacity(FSlateColor(GetPanelMutedTextColor()))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(6.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.ButtonStyle(&ComposerButtonStyle)
				.ContentPadding(FMargin(6.0f, 1.0f))
				.Text(LOCTEXT("RemoveAttachmentButton", "x"))
				.ToolTipText(LOCTEXT("RemoveAttachmentTooltip", "移除此附件"))
				.OnClicked(FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnRemoveAttachmentClicked, AttachmentIndex))
			]
		];
}

void SUEBridgeMCPPanel::RebuildComposerAttachments()
{
	if (!ComposerAttachmentWrapBox.IsValid())
	{
		return;
	}

	ComposerAttachmentWrapBox->ClearChildren();
	for (int32 Index = 0; Index < ComposerAttachments.Num(); ++Index)
	{
		ComposerAttachmentWrapBox->AddSlot()
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 6.0f))
			[
				BuildComposerAttachmentChip(Index)
			];
	}
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildComposerActionContent() const
{
	return SNew(SBox)
		.WidthOverride(32.0f)
		.HeightOverride(26.0f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this] { return IsComposerBusy() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text(LOCTEXT("SendButtonArrow", ">"))
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(SCircularThrobber)
				.Visibility_Lambda([this] { return IsComposerBusy() ? EVisibility::Visible : EVisibility::Collapsed; })
				.NumPieces(8)
				.Period(0.72f)
				.Radius(8.0f)
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
			]
			+ SOverlay::Slot()
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this] { return IsComposerBusy() ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text(LOCTEXT("StopButtonSquare", "■"))
				.ColorAndOpacity_Lambda([this] { return FSlateColor(GetAccentButtonTextColor()); })
				.Font(FAppStyle::GetFontStyle("SmallFontBold"))
			]
		];
}

bool SUEBridgeMCPPanel::IsComposerBusy() const
{
	// Busy state is per the conversation on screen, so an idle conversation's composer stays
	// usable while other conversations run in the background.
	const FConversationRuntime* Runtime = FindActiveRuntime();
	const bool bClientProcessing = Runtime && Runtime->Client.IsValid() && Runtime->Client->IsProcessing();
	const bool bAssistantStreaming = ActiveAssistantMessageIndex != INDEX_NONE
		&& ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex)
		&& ConversationMessages[ActiveAssistantMessageIndex].bStreaming;
	const bool bThoughtStreaming = ActiveThoughtMessageIndex != INDEX_NONE
		&& ConversationMessages.IsValidIndex(ActiveThoughtMessageIndex)
		&& ConversationMessages[ActiveThoughtMessageIndex].bStreaming;

	return bClientProcessing || ActiveHasPendingPermission() || bAssistantStreaming || bThoughtStreaming;
}

void SUEBridgeMCPPanel::StopActiveAnswer()
{
	// The Stop button only affects the conversation on screen; background turns keep running.
	const int64 ConvId = GetActiveConversationId();
	const int32 TargetConversationIndex = ActiveConversationIndex;
	FConversationRuntime* Runtime = ConvId != 0 ? ConversationRuntimes.Find(ConvId) : nullptr;
	if (Runtime && Runtime->Client.IsValid())
	{
		Runtime->Client->Stop();
	}

	ClearPendingPermission(ConvId);
	ResetFocusWorkflow(ConvId);

	TArray<FConversationMessage>* Messages = nullptr;
	FString* Transcript = nullptr;
	int32* AssistantMessageIndex = nullptr;
	int32* ThoughtMessageIndex = nullptr;
	if (!GetConversationStateForUpdate(TargetConversationIndex, Messages, Transcript, AssistantMessageIndex, ThoughtMessageIndex))
	{
		RebuildSidebar();
		SetLastAction(LOCTEXT("AnswerStoppedAction", "已终止本次回答。"));
		return;
	}

	if (*AssistantMessageIndex != INDEX_NONE && Messages->IsValidIndex(*AssistantMessageIndex))
	{
		(*Messages)[*AssistantMessageIndex].bStreaming = false;
	}
	*AssistantMessageIndex = INDEX_NONE;

	if (*ThoughtMessageIndex != INDEX_NONE && Messages->IsValidIndex(*ThoughtMessageIndex))
	{
		(*Messages)[*ThoughtMessageIndex].bStreaming = false;
	}
	*ThoughtMessageIndex = INDEX_NONE;

	if (Messages->Num() > 0)
	{
		AppendConversationEventToConversation(TargetConversationIndex, EConversationMessageRole::System, TEXT("已终止本次回答。"));
	}
	else if (TargetConversationIndex == ActiveConversationIndex)
	{
		RebuildConversationMessages();
	}

	RebuildSidebar();
	SetLastAction(LOCTEXT("AnswerStoppedAction", "已终止本次回答。"));
}

void SUEBridgeMCPPanel::ConfigureLightTextBoxStyle()
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
		.SetPadding(FMargin(12.0f, 9.0f));
}

void SUEBridgeMCPPanel::ConfigureComposerButtonStyle()
{
	ComposerButtonStyle = FAppStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
	if (const FSlateBrush* WhiteBrush = FAppStyle::GetBrush("WhiteBrush"))
	{
		FSlateBrush NormalBrush = *WhiteBrush;
		NormalBrush.TintColor = FSlateColor(UEBridgeMCP::Palette::Control());

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

FLinearColor SUEBridgeMCPPanel::GetAccentFillColor(float Alpha) const
{
	return UEBridgeMCP::Palette::Blend(GetPanelBackgroundColor(), GetEffectiveAccentColor(), Alpha);
}

FLinearColor SUEBridgeMCPPanel::GetAccentSurfaceColor() const
{
	return GetAccentFillColor(0.08f);
}

FLinearColor SUEBridgeMCPPanel::GetAccentControlColor() const
{
	return GetAccentFillColor(0.18f);
}

FLinearColor SUEBridgeMCPPanel::GetAccentBorderColor() const
{
	return UEBridgeMCP::Palette::Blend(GetPanelBorderColor(), GetEffectiveAccentColor(), 0.40f);
}

FLinearColor SUEBridgeMCPPanel::GetAccentButtonColor() const
{
	return GetEffectiveAccentColor();
}

FLinearColor SUEBridgeMCPPanel::GetAccentButtonTextColor() const
{
	const FLinearColor ButtonColor = GetAccentButtonColor();
	const float Luma = ButtonColor.R * 0.2126f + ButtonColor.G * 0.7152f + ButtonColor.B * 0.0722f;
	return Luma > 0.60f ? GetPanelTextColor() : UEBridgeMCP::Palette::OnPrimary();
}

FLinearColor SUEBridgeMCPPanel::GetReadableAccentTextColor() const
{
	return GetPanelTextColor();
}

FLinearColor SUEBridgeMCPPanel::GetPanelSubduedTextColor() const
{
	return GetPanelMutedTextColor();
}

FLinearColor SUEBridgeMCPPanel::GetEffectiveAccentColor() const
{
	return ResolveAccentColor(SettingsColor);
}

FLinearColor SUEBridgeMCPPanel::ResolveAccentColor(const FLinearColor& Color)
{
	const float MaxChannel = FMath::Max3(Color.R, Color.G, Color.B);
	const float MinChannel = FMath::Min3(Color.R, Color.G, Color.B);
	const float Luma = Color.R * 0.2126f + Color.G * 0.7152f + Color.B * 0.0722f;
	const bool bLooksLikeWhite = Luma > 0.92f && (MaxChannel - MinChannel) < 0.06f;
	return bLooksLikeWhite ? UEBridgeMCP::Palette::Primary() : Color;
}

FLinearColor SUEBridgeMCPPanel::GetPanelBackgroundColor()
{
	return UEBridgeMCP::Palette::Background();
}

FLinearColor SUEBridgeMCPPanel::GetPanelSurfaceColor()
{
	return UEBridgeMCP::Palette::Surface();
}

FLinearColor SUEBridgeMCPPanel::GetPanelBorderColor()
{
	return UEBridgeMCP::Palette::Border();
}

FLinearColor SUEBridgeMCPPanel::GetPanelTextColor()
{
	return UEBridgeMCP::Palette::Text();
}

FLinearColor SUEBridgeMCPPanel::GetPanelMutedTextColor()
{
	return UEBridgeMCP::Palette::TextMuted();
}

bool SUEBridgeMCPPanel::IsSettingsColorSelected(const FLinearColor& Color) const
{
	return FMath::IsNearlyEqual(SettingsColor.R, Color.R, 0.01f)
		&& FMath::IsNearlyEqual(SettingsColor.G, Color.G, 0.01f)
		&& FMath::IsNearlyEqual(SettingsColor.B, Color.B, 0.01f);
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildColorPresetButton(const FLinearColor& Color, const FText& Tooltip)
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

FText SUEBridgeMCPPanel::GetCliTitle(ECliTool Tool) const
{
	switch (Tool)
	{
	case ECliTool::Cursor:     return LOCTEXT("CursorCliTitle", "Cursor CLI");
	case ECliTool::ClaudeCode: return LOCTEXT("ClaudeCliTitle", "Claude Code CLI");
	case ECliTool::Codex:
	default:                   return LOCTEXT("CodexCliTitle", "Codex CLI");
	}
}

FText SUEBridgeMCPPanel::GetCliDescription(ECliTool Tool) const
{
	switch (Tool)
	{
	case ECliTool::Cursor:
		return LOCTEXT("CursorCliDescription", "配置 Cursor Agent CLI（cursor-agent），并同步写入 .cursor/mcp.json 供 Cursor 读取 MCP。");
	case ECliTool::ClaudeCode:
		return LOCTEXT("ClaudeCliDescription", "配置 claude 命令路径；面板对话通过 claude-agent-acp 适配器接入，并写入项目 .mcp.json 供 Claude Code 读取 MCP。");
	case ECliTool::Codex:
	default:
		return LOCTEXT("CodexCliDescription", "配置 codex 命令路径；当前面板对话仍通过 codex-acp 适配器接入。");
	}
}

FString SUEBridgeMCPPanel::GetCliCommandName(ECliTool Tool) const
{
	switch (Tool)
	{
	case ECliTool::Cursor:     return TEXT("cursor-agent");
	case ECliTool::ClaudeCode: return TEXT("claude");
	case ECliTool::Codex:
	default:                   return TEXT("codex");
	}
}

FString SUEBridgeMCPPanel::GetCliConfiguredPath(ECliTool Tool) const
{
	switch (Tool)
	{
	case ECliTool::Cursor:     return CursorCliPath;
	case ECliTool::ClaudeCode: return ClaudeCliPath;
	case ECliTool::Codex:
	default:                   return CodexCliPath;
	}
}

FString SUEBridgeMCPPanel::GetCliDetectedPath(ECliTool Tool) const
{
	switch (Tool)
	{
	case ECliTool::Cursor:     return DetectedCursorCliPath;
	case ECliTool::ClaudeCode: return DetectedClaudeCliPath;
	case ECliTool::Codex:
	default:                   return DetectedCodexCliPath;
	}
}

FString SUEBridgeMCPPanel::GetCliEffectivePath(ECliTool Tool) const
{
	const FString Configured = UEBridgeMCP::StripOuterQuotes(GetCliConfiguredPath(Tool));
	if (!Configured.IsEmpty())
	{
		return UEBridgeMCP::PathExists(Configured) ? Configured : FString();
	}
	return GetCliDetectedPath(Tool);
}

bool SUEBridgeMCPPanel::IsCliAvailable(ECliTool Tool) const
{
	return !GetCliEffectivePath(Tool).IsEmpty();
}

FText SUEBridgeMCPPanel::GetCliPathSummary(ECliTool Tool) const
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

void SUEBridgeMCPPanel::RefreshCliDetections()
{
	DetectedCodexCliPath = UEBridgeMCP::ResolveCommandOnPath(TEXT("codex"));
	DetectedCursorCliPath = UEBridgeMCP::ResolveCommandOnPath(TEXT("cursor-agent"));
	DetectedClaudeCliPath = UEBridgeMCP::ResolveCommandOnPath(TEXT("claude"));
}

void SUEBridgeMCPPanel::SetCliConfiguredPath(ECliTool Tool, const FString& NewPath)
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

	switch (Tool)
	{
	case ECliTool::Cursor:     CursorCliPath = Normalized; break;
	case ECliTool::ClaudeCode: ClaudeCliPath = Normalized; break;
	case ECliTool::Codex:
	default:                   CodexCliPath = Normalized;  break;
	}

	SaveSettings();
	SetLastAction(FText::FromString(FString::Printf(TEXT("%s 路径已保存。"), *GetCliTitle(Tool).ToString())));
}

FReply SUEBridgeMCPPanel::OnDetectCliClicked(ECliTool Tool)
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

FReply SUEBridgeMCPPanel::OnClearCliClicked(ECliTool Tool)
{
	switch (Tool)
	{
	case ECliTool::Cursor:     CursorCliPath.Empty(); break;
	case ECliTool::ClaudeCode: ClaudeCliPath.Empty(); break;
	case ECliTool::Codex:
	default:                   CodexCliPath.Empty();  break;
	}

	RefreshCliDetections();
	SaveSettings();
	SetLastAction(FText::FromString(FString::Printf(TEXT("%s 路径已清空，将重新使用 PATH 自动检测。"), *GetCliTitle(Tool).ToString())));
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnDownloadCliClicked(ECliTool Tool)
{
	FString Url;
	switch (Tool)
	{
	case ECliTool::Cursor:     Url = UEBridgeMCP::GetCursorCliInstallUrl(); break;
	case ECliTool::ClaudeCode: Url = UEBridgeMCP::GetClaudeCliInstallUrl(); break;
	case ECliTool::Codex:
	default:                   Url = UEBridgeMCP::GetCodexCliInstallUrl();  break;
	}
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

void SUEBridgeMCPPanel::LoadSettings()
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
		(*CliObjectPtr)->TryGetStringField(TEXT("claudePath"), ClaudeCliPath);
		CodexCliPath = UEBridgeMCP::StripOuterQuotes(CodexCliPath);
		CursorCliPath = UEBridgeMCP::StripOuterQuotes(CursorCliPath);
		ClaudeCliPath = UEBridgeMCP::StripOuterQuotes(ClaudeCliPath);

		FString AgentName;
		if ((*CliObjectPtr)->TryGetStringField(TEXT("agent"), AgentName))
		{
			if (AgentName.Equals(TEXT("cursor"), ESearchCase::IgnoreCase))
			{
				CurrentAcpAgent = EWorldDataAcpAgent::Cursor;
			}
			else if (AgentName.Equals(TEXT("claude"), ESearchCase::IgnoreCase))
			{
				CurrentAcpAgent = EWorldDataAcpAgent::ClaudeCode;
			}
			else
			{
				CurrentAcpAgent = EWorldDataAcpAgent::Codex;
			}
		}

		FString ModeName;
		if ((*CliObjectPtr)->TryGetStringField(TEXT("mode"), ModeName))
		{
			if (ModeName.Equals(TEXT("plan"), ESearchCase::IgnoreCase))
			{
				CurrentMode = EWorldDataCodexPermissionMode::Plan;
			}
			else if (ModeName.Equals(TEXT("focus"), ESearchCase::IgnoreCase))
			{
				CurrentMode = EWorldDataCodexPermissionMode::Focus;
			}
			else if (ModeName.Equals(TEXT("bypass"), ESearchCase::IgnoreCase))
			{
				CurrentMode = EWorldDataCodexPermissionMode::Bypass;
			}
			else
			{
				CurrentMode = EWorldDataCodexPermissionMode::Default;
			}
		}
		FString LegacyModel;
		(*CliObjectPtr)->TryGetStringField(TEXT("model"), LegacyModel);
		(*CliObjectPtr)->TryGetStringField(TEXT("codexModel"), CurrentCodexModel);
		(*CliObjectPtr)->TryGetStringField(TEXT("cursorModel"), CurrentCursorModel);
		(*CliObjectPtr)->TryGetStringField(TEXT("claudeModel"), CurrentClaudeModel);
		LegacyModel = UEBridgeMCP::StripOuterQuotes(LegacyModel);
		CurrentCodexModel = UEBridgeMCP::StripOuterQuotes(CurrentCodexModel);
		CurrentCursorModel = UEBridgeMCP::StripOuterQuotes(CurrentCursorModel);
		CurrentClaudeModel = UEBridgeMCP::StripOuterQuotes(CurrentClaudeModel);

		if (!LegacyModel.IsEmpty())
		{
			if (LegacyModel.StartsWith(TEXT("claude-")) && CurrentClaudeModel.IsEmpty())
			{
				CurrentClaudeModel = LegacyModel;
			}
			else if ((LegacyModel == TEXT("sonnet-4") || LegacyModel == TEXT("sonnet-4-thinking")) && CurrentCursorModel.IsEmpty())
			{
				CurrentCursorModel = LegacyModel;
			}
			else if (CurrentAcpAgent == EWorldDataAcpAgent::Cursor && CurrentCursorModel.IsEmpty())
			{
				CurrentCursorModel = LegacyModel;
			}
			else if (CurrentCodexModel.IsEmpty())
			{
				CurrentCodexModel = LegacyModel;
			}
		}
		CurrentModel = GetCurrentAgentModel();
	}

	NormalizeAgentModels();
	SaveSettings();
}

void SUEBridgeMCPPanel::ApplySettingsColor(const FLinearColor& NewColor)
{
	SettingsColor = FLinearColor(
		FMath::Clamp(NewColor.R, 0.0f, 1.0f),
		FMath::Clamp(NewColor.G, 0.0f, 1.0f),
		FMath::Clamp(NewColor.B, 0.0f, 1.0f),
		1.0f);
	SaveSettings();
	SetLastAction(FText::Format(LOCTEXT("SettingsColorAppliedAction", "Color 已应用：{0}"), GetSettingsColorText()));
}

void SUEBridgeMCPPanel::SaveSettings() const
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
	CliObject->SetStringField(TEXT("claudePath"), ClaudeCliPath);
	const TCHAR* AgentName = TEXT("codex");
	if (CurrentAcpAgent == EWorldDataAcpAgent::Cursor)
	{
		AgentName = TEXT("cursor");
	}
	else if (CurrentAcpAgent == EWorldDataAcpAgent::ClaudeCode)
	{
		AgentName = TEXT("claude");
	}
	CliObject->SetStringField(TEXT("agent"), AgentName);
	const TCHAR* ModeName = TEXT("default");
	if (CurrentMode == EWorldDataCodexPermissionMode::Plan)
	{
		ModeName = TEXT("plan");
	}
	else if (CurrentMode == EWorldDataCodexPermissionMode::Focus)
	{
		ModeName = TEXT("focus");
	}
	else if (CurrentMode == EWorldDataCodexPermissionMode::Bypass)
	{
		ModeName = TEXT("bypass");
	}
	CliObject->SetStringField(TEXT("mode"), ModeName);
	const FString SafeCodexModel = SanitizeModelForAgent(EWorldDataAcpAgent::Codex, CurrentCodexModel);
	const FString SafeCursorModel = SanitizeModelForAgent(EWorldDataAcpAgent::Cursor, CurrentCursorModel);
	const FString SafeClaudeModel = SanitizeModelForAgent(EWorldDataAcpAgent::ClaudeCode, CurrentClaudeModel);
	const FString SafeCurrentModel = SanitizeModelForAgent(CurrentAcpAgent, GetCurrentAgentModel());
	CliObject->SetStringField(TEXT("model"), SafeCurrentModel);
	CliObject->SetStringField(TEXT("codexModel"), SafeCodexModel);
	CliObject->SetStringField(TEXT("cursorModel"), SafeCursorModel);
	CliObject->SetStringField(TEXT("claudeModel"), SafeClaudeModel);

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

FText SUEBridgeMCPPanel::GetSettingsColorText() const
{
	const FColor Color = SettingsColor.ToFColor(true);
	return FText::FromString(FString::Printf(TEXT("#%02X%02X%02X"), Color.R, Color.G, Color.B));
}

void SUEBridgeMCPPanel::OpenSettingsColorPicker()
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

void SUEBridgeMCPPanel::HandleSettingsColorChanged(FLinearColor NewColor)
{
	ApplySettingsColor(NewColor);
}

FReply SUEBridgeMCPPanel::OnSettingsColorBlockClicked(const FGeometry& Geometry, const FPointerEvent& MouseEvent)
{
	OpenSettingsColorPicker();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnPickSettingsColorClicked()
{
	OpenSettingsColorPicker();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnResetSettingsColorClicked()
{
	ApplySettingsColor(FLinearColor::White);
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnSettingsBackClicked()
{
	bShowSettings = false;
	SetLastAction(LOCTEXT("SettingsBackAction", "已返回对话。"));
	return FReply::Handled();
}


#undef LOCTEXT_NAMESPACE
