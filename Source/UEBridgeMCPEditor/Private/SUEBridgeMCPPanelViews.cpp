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

namespace
{
	FSlateFontInfo MakeFont(const FName& StyleName, int32 SizeDelta = 0)
	{
		FSlateFontInfo Font = FAppStyle::GetFontStyle(StyleName);
		Font.Size = FMath::Max(7, Font.Size + SizeDelta);
		return Font;
	}

	FString StripInlineMarkdown(FString Text)
	{
		Text.ReplaceInline(TEXT("**"), TEXT(""));
		Text.ReplaceInline(TEXT("__"), TEXT(""));
		Text.ReplaceInline(TEXT("`"), TEXT(""));
		return Text.TrimStartAndEnd();
	}

	bool IsMarkdownSeparator(const FString& Trimmed)
	{
		if (Trimmed.Len() < 3)
		{
			return false;
		}
		for (int32 Index = 0; Index < Trimmed.Len(); ++Index)
		{
			const TCHAR Ch = Trimmed[Index];
			if (Ch != TEXT('-') && Ch != TEXT('_') && Ch != TEXT('*'))
			{
				return false;
			}
		}
		return true;
	}

	bool TryStripPrefix(const FString& Trimmed, const FString& Prefix, FString& OutText)
	{
		if (!Trimmed.StartsWith(Prefix))
		{
			return false;
		}
		OutText = Trimmed.Mid(Prefix.Len()).TrimStartAndEnd();
		return true;
	}

	bool TryParseMarkdownHeading(const FString& Trimmed, int32& OutLevel, FString& OutText)
	{
		OutLevel = 0;
		while (OutLevel < Trimmed.Len() && Trimmed[OutLevel] == TEXT('#'))
		{
			++OutLevel;
		}
		if (OutLevel <= 0 || OutLevel > 6 || OutLevel >= Trimmed.Len() || !FChar::IsWhitespace(Trimmed[OutLevel]))
		{
			return false;
		}
		OutText = StripInlineMarkdown(Trimmed.Mid(OutLevel).TrimStartAndEnd());
		return !OutText.IsEmpty();
	}

	bool TryParseNumberedLine(const FString& Trimmed, FString& OutMarker, FString& OutText)
	{
		int32 Index = 0;
		while (Index < Trimmed.Len() && FChar::IsDigit(Trimmed[Index]))
		{
			++Index;
		}
		if (Index <= 0 || Index >= Trimmed.Len())
		{
			return false;
		}
		const TCHAR Separator = Trimmed[Index];
		if (Separator != TEXT('.') && Separator != TEXT(')'))
		{
			return false;
		}
		if (Index + 1 < Trimmed.Len() && !FChar::IsWhitespace(Trimmed[Index + 1]))
		{
			return false;
		}
		OutMarker = Trimmed.Left(Index + 1);
		OutText = StripInlineMarkdown(Trimmed.Mid(Index + 1).TrimStartAndEnd());
		return !OutText.IsEmpty();
	}
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildConversationDetail()
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
			.Visibility_Lambda([this] { return ActiveHasPendingPermission() ? EVisibility::Visible : EVisibility::Collapsed; })
			[
				BuildPermissionRequestCard()
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			BuildRunningIndicator()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildConversationMessagesView()
{
	return SNew(SBorder)
		.Padding(FMargin(0.0f, 4.0f))
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		[
			SAssignNew(ConversationScrollBox, SScrollBox)
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildConversationMessageWidget(const FConversationMessage& Message) const
{
	const FString StreamingPlaceholder = Message.Role == EConversationMessageRole::Thought
		? FString(TEXT("正在生成推理摘要..."))
		: FString(TEXT("正在回复..."));
	const FString DisplayText = Message.Text.IsEmpty() && Message.bStreaming
		? StreamingPlaceholder
		: Message.Text;

	// Tool calls render as compact pills (grouped into rows by RebuildConversationMessages);
	// this is the fallback if one is laid out on its own.
	if (Message.Role == EConversationMessageRole::Tool)
	{
		return BuildToolPill(Message);
	}

	// User: right-aligned accent bubble.
	if (Message.Role == EConversationMessageRole::User)
	{
		return SNew(SBox)
			.HAlign(HAlign_Right)
			[
				SNew(SBox)
				.MaxDesiredWidth(720.0f)
				[
					SNew(SBorder)
					.Padding(FMargin(12.0f, 9.0f))
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this] { return FSlateColor(GetAccentFillColor(0.16f)); })
					[
						SNew(STextBlock)
						.Text(FText::FromString(DisplayText))
						.ColorAndOpacity_Lambda([this] { return FSlateColor(GetPanelTextColor()); })
						.AutoWrapText(true)
					]
				]
			];
	}

	// System / Error: subtle full-width notice card.
	if (Message.Role == EConversationMessageRole::System || Message.Role == EConversationMessageRole::Error)
	{
		return SNew(SBorder)
			.Padding(FMargin(10.0f, 7.0f))
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FSlateColor(GetConversationMessageBackgroundColor(Message.Role)))
			[
				SNew(STextBlock)
				.Text(FText::FromString(DisplayText))
				.ColorAndOpacity(FSlateColor(GetConversationMessageTextColor(Message.Role)))
				.AutoWrapText(true)
			];
	}

	// Assistant / Thought: flowing prose, no card chrome (thought reads as dimmed text).
	return SNew(SBox)
		.HAlign(HAlign_Left)
		.Padding(FMargin(2.0f, 0.0f))
		[
			SNew(SBox)
			.MaxDesiredWidth(1120.0f)
			[
				BuildConversationMarkdownContent(DisplayText, Message.Role)
			]
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildConversationMarkdownContent(const FString& Text, EConversationMessageRole Role) const
{
	const FLinearColor TextColor = GetConversationMessageTextColor(Role);
	const FLinearColor MutedColor = GetConversationMessageMutedColor(Role);
	const FLinearColor AccentColor = GetEffectiveAccentColor();
	const FLinearColor SoftAccentColor = GetAccentFillColor(0.13f);
	const FLinearColor SurfaceColor = Role == EConversationMessageRole::Thought
		? UEBridgeMCP::Palette::Surface()
		: UEBridgeMCP::Palette::Background();

	TSharedRef<SVerticalBox> Root = SNew(SVerticalBox);

	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	if (Lines.IsEmpty())
	{
		Lines.Add(Text);
	}

	bool bInCodeBlock = false;
	TArray<FString> CodeLines;

	auto AddSpacer = [Root](float Height)
	{
		Root->AddSlot()
			.AutoHeight()
			[
				SNew(SBox).HeightOverride(Height)
			];
	};

	auto AddTextLine = [Root](const FString& LineText, const FSlateFontInfo& Font, FLinearColor Color, FMargin Padding)
	{
		Root->AddSlot()
			.AutoHeight()
			.Padding(Padding)
			[
				SNew(STextBlock)
				.Text(FText::FromString(LineText))
				.ColorAndOpacity(FSlateColor(Color))
				.Font(Font)
				.AutoWrapText(true)
			];
	};

	auto FlushCodeBlock = [&]()
	{
		if (CodeLines.IsEmpty())
		{
			return;
		}

		Root->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 5.0f, 0.0f, 7.0f))
			[
				SNew(SBorder)
				.Padding(FMargin(10.0f, 8.0f))
				.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FSlateColor(UEBridgeMCP::Palette::SurfaceRaised()))
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Join(CodeLines, TEXT("\n"))))
					.ColorAndOpacity(FSlateColor(TextColor))
					.Font(FAppStyle::GetFontStyle("MonospacedText"))
					.AutoWrapText(true)
				]
			];
		CodeLines.Reset();
	};

	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine;
		Line.TrimEndInline();
		const FString Trimmed = Line.TrimStartAndEnd();

		if (Trimmed.StartsWith(TEXT("```")))
		{
			if (bInCodeBlock)
			{
				bInCodeBlock = false;
				FlushCodeBlock();
			}
			else
			{
				bInCodeBlock = true;
			}
			continue;
		}

		if (bInCodeBlock)
		{
			CodeLines.Add(Line);
			continue;
		}

		if (Trimmed.IsEmpty())
		{
			AddSpacer(6.0f);
			continue;
		}

		if (IsMarkdownSeparator(Trimmed))
		{
			Root->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 8.0f, 0.0f, 10.0f))
				[
					SNew(SSeparator)
					.Thickness(1.0f)
					.ColorAndOpacity(FSlateColor(UEBridgeMCP::Palette::Border()))
				];
			continue;
		}

		int32 HeadingLevel = 0;
		FString ParsedText;
		if (TryParseMarkdownHeading(Trimmed, HeadingLevel, ParsedText))
		{
			const int32 SizeDelta = HeadingLevel <= 1 ? 3 : (HeadingLevel == 2 ? 2 : 1);
			AddTextLine(
				ParsedText,
				MakeFont("NormalFontBold", SizeDelta),
				HeadingLevel <= 2 ? AccentColor : TextColor,
				FMargin(0.0f, HeadingLevel <= 2 ? 7.0f : 5.0f, 0.0f, 4.0f));
			continue;
		}

		if (Trimmed.StartsWith(TEXT("**")) && Trimmed.EndsWith(TEXT("**")) && Trimmed.Len() > 4)
		{
			AddTextLine(
				StripInlineMarkdown(Trimmed),
				MakeFont("NormalFontBold", 1),
				AccentColor,
				FMargin(0.0f, 7.0f, 0.0f, 3.0f));
			continue;
		}

		if (TryStripPrefix(Trimmed, TEXT(">"), ParsedText))
		{
			Root->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 5.0f, 0.0f, 7.0f))
				[
					SNew(SBorder)
					.Padding(FMargin(10.0f, 7.0f))
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FSlateColor(SoftAccentColor))
					[
						SNew(STextBlock)
						.Text(FText::FromString(StripInlineMarkdown(ParsedText)))
						.ColorAndOpacity(FSlateColor(TextColor))
						.AutoWrapText(true)
					]
				];
			continue;
		}

		FString BulletText;
		if (TryStripPrefix(Trimmed, TEXT("- "), BulletText) || TryStripPrefix(Trimmed, TEXT("* "), BulletText))
		{
			Root->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 2.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(2.0f, 0.0f, 8.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Chr(0x2022)))
						.ColorAndOpacity(FSlateColor(AccentColor))
						.Font(MakeFont("NormalFontBold", 1))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(StripInlineMarkdown(BulletText)))
						.ColorAndOpacity(FSlateColor(TextColor))
						.AutoWrapText(true)
					]
				];
			continue;
		}

		FString NumberMarker;
		if (TryParseNumberedLine(Trimmed, NumberMarker, ParsedText))
		{
			Root->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 2.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(NumberMarker))
						.ColorAndOpacity(FSlateColor(AccentColor))
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(ParsedText))
						.ColorAndOpacity(FSlateColor(TextColor))
						.AutoWrapText(true)
					]
				];
			continue;
		}

		const bool bLooksLikeInlineCode = Trimmed.Contains(TEXT("`")) || Trimmed.Contains(TEXT("://"));
		AddTextLine(
			StripInlineMarkdown(Trimmed),
			bLooksLikeInlineCode ? FAppStyle::GetFontStyle("MonospacedText") : FAppStyle::GetFontStyle("NormalText"),
			bLooksLikeInlineCode ? MutedColor : TextColor,
			FMargin(0.0f, 1.5f, 0.0f, 1.5f));
	}

	FlushCodeBlock();

	return SNew(SBorder)
		.Padding(FMargin(2.0f, 1.0f))
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.BorderBackgroundColor(FSlateColor(SurfaceColor))
		[
			Root
		];
}

FString SUEBridgeMCPPanel::HumanizeToolName(const FString& Raw)
{
	FString Spaced = Raw;
	Spaced.ReplaceInline(TEXT("_"), TEXT(" "));
	TArray<FString> Words;
	Spaced.ParseIntoArray(Words, TEXT(" "), true);
	for (FString& Word : Words)
	{
		if (Word.Len() > 0)
		{
			Word = Word.Left(1).ToUpper() + Word.Mid(1);
		}
	}
	const FString Out = FString::Join(Words, TEXT(" "));
	return Out.IsEmpty() ? Raw : Out;
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildToolPill(const FConversationMessage& Message) const
{
	// The tool message text is "<name>\n<input digest>" — first line is the pill label, the rest
	// (e.g. the execute_python script captured for auditing) is shown as a hover tooltip.
	FString FirstLine = Message.Text;
	FString Detail;
	int32 NewlineIdx = INDEX_NONE;
	if (Message.Text.FindChar(TCHAR('\n'), NewlineIdx))
	{
		FirstLine = Message.Text.Left(NewlineIdx);
		Detail = Message.Text.Mid(NewlineIdx + 1).TrimStartAndEnd();
	}

	const bool bFailed = FirstLine.Contains(TEXT("失败"));
	FString Name = FirstLine;
	Name.ReplaceInline(TEXT("调用失败"), TEXT(""));
	Name = HumanizeToolName(Name.TrimStartAndEnd());

	const FLinearColor BgColor = bFailed
		? FLinearColor(0.99f, 0.92f, 0.92f, 1.0f)
		: FLinearColor(0.90f, 0.97f, 0.91f, 1.0f);
	const FLinearColor FgColor = bFailed
		? FLinearColor(0.78f, 0.22f, 0.22f, 1.0f)
		: FLinearColor(0.16f, 0.55f, 0.30f, 1.0f);
	const FText Mark = FText::FromString(FString::Chr(bFailed ? 0x2717 : 0x2713));

	return SNew(SBorder)
		.Padding(FMargin(9.0f, 4.0f))
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(FSlateColor(BgColor))
		.ToolTipText(Detail.IsEmpty() ? FText::GetEmpty() : FText::FromString(Detail))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 5.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(Mark)
				.ColorAndOpacity(FSlateColor(FgColor))
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Name))
				.ColorAndOpacity(FSlateColor(FgColor))
			]
		];
}

FText SUEBridgeMCPPanel::GetConversationRoleLabel(EConversationMessageRole Role) const
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
	case EConversationMessageRole::Thought:
		return LOCTEXT("ConversationThoughtRole", "推理摘要");
	case EConversationMessageRole::Assistant:
	default:
		return LOCTEXT("ConversationAssistantRole", "Codex");
	}
}

FLinearColor SUEBridgeMCPPanel::GetConversationMessageBackgroundColor(EConversationMessageRole Role) const
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
	case EConversationMessageRole::Thought:
		return UEBridgeMCP::Palette::Surface();
	case EConversationMessageRole::Assistant:
	default:
		return UEBridgeMCP::Palette::Background();
	}
}

FLinearColor SUEBridgeMCPPanel::GetConversationMessageBorderColor(EConversationMessageRole Role) const
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

FLinearColor SUEBridgeMCPPanel::GetConversationMessageTextColor(EConversationMessageRole Role) const
{
	if (Role == EConversationMessageRole::Error)
	{
		return UEBridgeMCP::Palette::Danger();
	}
	// Reasoning reads as secondary/dim text so it's clearly distinct from the answer.
	if (Role == EConversationMessageRole::Thought)
	{
		return GetPanelMutedTextColor();
	}
	return GetPanelTextColor();
}

FLinearColor SUEBridgeMCPPanel::GetConversationMessageMutedColor(EConversationMessageRole Role) const
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildPermissionRequestCard()
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
							const FConversationRuntime* Runtime = FindActiveRuntime();
							const FString Title = Runtime ? Runtime->PendingPermissionTitle : FString();
							const FString DisplayTitle = Title.IsEmpty()
								? FString(TEXT("Codex 请求执行一个 MCP 工具。"))
								: Title;
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
							const FConversationRuntime* Runtime = FindActiveRuntime();
							const FString ToolName = Runtime ? Runtime->PendingPermissionToolName : FString();
							const FString DisplayTool = ToolName.IsEmpty()
								? FString(TEXT("默认模式会等待你确认后继续。"))
								: FString::Printf(TEXT("工具：%s"), *ToolName);
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildPermissionActionButton(const FText& Label, bool bPrimary, FOnClicked OnClicked) const
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildSettingsPanel()
{
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			BuildSettingsContent()
		];
}

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildSettingsContent()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildMcpServerPanel()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildServerStatusCard()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildServerClientsBanner()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildServerPortCard()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildRegisteredToolsCard()
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildToolChip(const FString& ToolName)
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

FText SUEBridgeMCPPanel::GetServerStatusText() const
{
	if (FWorldDataMCPServer::IsRunning())
	{
		return FText::Format(
			LOCTEXT("McpStatusRunning", "运行中 — port {0}"),
			FText::FromString(FString::FromInt(FWorldDataMCPServer::GetPort())));
	}
	return LOCTEXT("McpStatusStopped", "已停止");
}

FText SUEBridgeMCPPanel::GetServerToggleText() const
{
	return FWorldDataMCPServer::IsRunning()
		? LOCTEXT("McpToggleStop", "停止")
		: LOCTEXT("McpToggleStart", "启动");
}

int32 SUEBridgeMCPPanel::ParseServerPort() const
{
	const int32 Port = FCString::Atoi(*ServerPortText);
	return (Port >= 1024 && Port <= 65535) ? Port : FWorldDataMCPServer::LoadConfiguredPort();
}

const TArray<FString>& SUEBridgeMCPPanel::GetRegisteredToolNames()
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

FReply SUEBridgeMCPPanel::OnToggleServerClicked()
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

FReply SUEBridgeMCPPanel::OnApplyPortClicked()
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


#undef LOCTEXT_NAMESPACE
