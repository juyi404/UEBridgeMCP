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

void SUEBridgeMCPPanel::SetDetail(const FText& Title, const FString& Text)
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

void SUEBridgeMCPPanel::SetLastAction(const FText& Text)
{
	LastAction = Text;
}

void SUEBridgeMCPPanel::RebuildConversationMessages()
{
	if (!ConversationScrollBox.IsValid())
	{
		return;
	}

	ConversationScrollBox->ClearChildren();
	int32 Index = 0;
	while (Index < ConversationMessages.Num())
	{
		// Group consecutive tool calls into one horizontal wrap row of compact pills.
		if (ConversationMessages[Index].Role == EConversationMessageRole::Tool)
		{
			TSharedRef<SWrapBox> Row = SNew(SWrapBox).UseAllottedSize(true);
			while (Index < ConversationMessages.Num() && ConversationMessages[Index].Role == EConversationMessageRole::Tool)
			{
				Row->AddSlot()
					.Padding(FMargin(0.0f, 0.0f, 6.0f, 6.0f))
					[
						BuildToolPill(ConversationMessages[Index])
					];
				++Index;
			}
			ConversationScrollBox->AddSlot()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 8.0f))
				[
					Row
				];
			continue;
		}

		ConversationScrollBox->AddSlot()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 10.0f))
			[
				BuildConversationMessageWidget(ConversationMessages[Index])
			];
		++Index;
	}
	ConversationScrollBox->ScrollToEnd();
}

int32 SUEBridgeMCPPanel::AddConversationMessage(EConversationMessageRole Role, const FString& Text, bool bStreaming)
{
	return AddConversationMessageToConversation(ActiveConversationIndex, Role, Text, bStreaming);
}

int32 SUEBridgeMCPPanel::AddConversationMessageToConversation(int32 ConversationIndex, EConversationMessageRole Role, const FString& Text, bool bStreaming)
{
	FConversationMessage Message;
	Message.Role = Role;
	Message.Text = Text;
	Message.bStreaming = bStreaming;

	if (ConversationIndex == ActiveConversationIndex)
	{
		const int32 Index = ConversationMessages.Add(MoveTemp(Message));
		RebuildConversationMessages();
		return Index;
	}

	if (!Conversations.IsValidIndex(ConversationIndex))
	{
		return INDEX_NONE;
	}

	const int32 Index = Conversations[ConversationIndex].Messages.Add(MoveTemp(Message));
	RebuildSidebar();
	return Index;
}

FString SUEBridgeMCPPanel::TrimConversationEventText(const FString& Text)
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

bool SUEBridgeMCPPanel::TryExtractConversationEvent(const FString& Text, EConversationMessageRole& OutRole, FString& OutText) const
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

FString SUEBridgeMCPPanel::BuildPromptWithConversationMemory(const FString& UserMessage) const
{
	return BuildPromptWithConversationMemoryForConversation(ActiveConversationIndex, UserMessage);
}

FString SUEBridgeMCPPanel::BuildPromptWithConversationMemoryForConversation(int32 ConversationIndex, const FString& UserMessage) const
{
	constexpr int32 MaxMemoryMessages = 14;
	constexpr int32 MaxMemoryChars = 12000;
	constexpr int32 MaxSingleMessageChars = 2500;

	const TArray<FConversationMessage>* SourceMessages = nullptr;
	if (ConversationIndex == ActiveConversationIndex)
	{
		SourceMessages = &ConversationMessages;
	}
	else if (Conversations.IsValidIndex(ConversationIndex))
	{
		SourceMessages = &Conversations[ConversationIndex].Messages;
	}
	if (!SourceMessages)
	{
		return UserMessage;
	}

	TArray<FString> Entries;
	int32 UsedChars = 0;
	int32 UsedMessages = 0;

	auto ClipText = [](FString Text, int32 MaxChars)
	{
		Text.TrimStartAndEndInline();
		if (Text.Len() > MaxChars)
		{
			Text = Text.Left(MaxChars) + TEXT("\n[已截断]");
		}
		return Text;
	};

	for (int32 Index = SourceMessages->Num() - 1; Index >= 0 && UsedMessages < MaxMemoryMessages; --Index)
	{
		const FConversationMessage& Message = (*SourceMessages)[Index];
		FString RoleLabel;
		switch (Message.Role)
		{
		case EConversationMessageRole::User:
			RoleLabel = TEXT("用户");
			break;
		case EConversationMessageRole::Assistant:
			RoleLabel = TEXT("助手");
			break;
		default:
			continue;
		}

		const FString MemoryText = ClipText(Message.Text, MaxSingleMessageChars);
		if (MemoryText.IsEmpty())
		{
			continue;
		}

		FString Entry = FString::Printf(TEXT("%s：%s"), *RoleLabel, *MemoryText);
		if (UsedChars + Entry.Len() > MaxMemoryChars)
		{
			const int32 RemainingChars = MaxMemoryChars - UsedChars;
			if (RemainingChars < 300)
			{
				break;
			}
			Entry = Entry.Left(RemainingChars) + TEXT("\n[已截断]");
		}

		Entries.Insert(Entry, 0);
		UsedChars += Entry.Len();
		++UsedMessages;
	}

	if (Entries.IsEmpty())
	{
		return UserMessage;
	}

	return FString::Printf(
		TEXT("以下是当前对话的历史记忆。请把它作为同一对话的上下文，不要逐字复述；如果历史与当前消息冲突，以当前消息为准。\n\n")
		TEXT("---\n对话历史：\n%s\n\n")
		TEXT("---\n当前用户消息：\n%s"),
		*FString::Join(Entries, TEXT("\n\n")),
		*UserMessage);
}

FString SUEBridgeMCPPanel::BuildFocusRoundPrompt(int64 ConvId, EFocusWorkflowState Round) const
{
	// Read this conversation's focus state via local aliases so the prompt body below is
	// unchanged. A missing runtime yields empty fields (treated as a fresh plan).
	static const FString EmptyFocusField;
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	const FString& FocusReplanNote = Runtime ? Runtime->FocusReplanNote : EmptyFocusField;
	const FString& FocusOriginalRequest = Runtime ? Runtime->FocusOriginalRequest : EmptyFocusField;
	const FString& FocusUnderstandingSummary = Runtime ? Runtime->FocusUnderstandingSummary : EmptyFocusField;
	const FString& FocusOptionsSummary = Runtime ? Runtime->FocusOptionsSummary : EmptyFocusField;
	const FString& FocusCritiqueSummary = Runtime ? Runtime->FocusCritiqueSummary : EmptyFocusField;
	const FString& FocusFinalPlan = Runtime ? Runtime->FocusFinalPlan : EmptyFocusField;

	const FString ReplanBlock = FocusReplanNote.IsEmpty()
		? FString()
		: FString::Printf(TEXT("\n\n重新规划要求：\n%s"), *FocusReplanNote);

	switch (Round)
	{
	case EFocusWorkflowState::Understanding:
		return FString::Printf(
			TEXT("【专注模式 · 第 1 轮/4：需求理解与只读上下文】\n")
			TEXT("这是自动多轮规划的第一轮，不是执行阶段。可以读取必要的项目文本文件或资源来澄清上下文，但不要修改项目，不要调用会改变状态的工具。\n")
			TEXT("请输出可共享的推演摘要，不要输出隐藏推理链。\n\n")
			TEXT("请覆盖：\n")
			TEXT("1. 需求复述\n")
			TEXT("2. 目标与非目标\n")
			TEXT("3. 成功标准\n")
			TEXT("4. 约束、风险和歧义\n")
			TEXT("5. 执行阶段需要读取/确认的上下文\n\n")
			TEXT("用户任务：\n%s%s"),
			*FocusOriginalRequest,
			*ReplanBlock);

	case EFocusWorkflowState::ExploringOptions:
		return FString::Printf(
			TEXT("【专注模式 · 第 2 轮/4：实现路径对比】\n")
			TEXT("基于第 1 轮理解，提出至少两种可行实现路径，比较取舍，然后选择一个主方案。仍然不要修改项目。\n\n")
			TEXT("必须输出：\n")
			TEXT("1. 方案 A：步骤、优点、缺点、适用条件\n")
			TEXT("2. 方案 B：步骤、优点、缺点、适用条件\n")
			TEXT("3. 如有必要，方案 C\n")
			TEXT("4. 最终选择及原因\n\n")
			TEXT("用户任务：\n%s\n\n")
			TEXT("第 1 轮摘要：\n%s%s"),
			*FocusOriginalRequest,
			*FocusUnderstandingSummary,
			*ReplanBlock);

	case EFocusWorkflowState::Critiquing:
		return FString::Printf(
			TEXT("【专注模式 · 第 3 轮/4：批判审查】\n")
			TEXT("请扮演审查者，专门挑出已选方案的漏洞。不要修改项目。\n\n")
			TEXT("必须审查：\n")
			TEXT("1. 哪些步骤不够落地\n")
			TEXT("2. 哪些文件/类/函数/状态字段还没有明确\n")
			TEXT("3. 哪些 UI/交互/持久化/权限边界可能遗漏\n")
			TEXT("4. 哪些验证不足\n")
			TEXT("5. 需要如何修订方案\n\n")
			TEXT("用户任务：\n%s\n\n")
			TEXT("第 1 轮摘要：\n%s\n\n")
			TEXT("第 2 轮方案：\n%s%s"),
			*FocusOriginalRequest,
			*FocusUnderstandingSummary,
			*FocusOptionsSummary,
			*ReplanBlock);

	case EFocusWorkflowState::RepairingPlan:
		return FString::Printf(
			TEXT("【专注模式 · 最终计划补全】\n")
			TEXT("上一版最终计划缺少关键字段。请只输出补全后的最终落地 Plan，不要执行。\n")
			TEXT("必须包含这些标题：目标、影响范围、涉及文件/类/函数、实现步骤、验证步骤、风险、回退方案、执行确认。\n\n")
			TEXT("上一版最终计划：\n%s\n\n")
			TEXT("用户任务：\n%s"),
			*FocusFinalPlan,
			*FocusOriginalRequest);

	case EFocusWorkflowState::RefiningPlan:
	default:
		return FString::Printf(
			TEXT("【专注模式 · 第 4 轮/4：最终落地 Plan】\n")
			TEXT("请基于前三轮结果，输出最终可执行计划。不要修改项目，不要调用写入工具。\n")
			TEXT("最终计划必须足够落地，尽量具体到文件、类、函数、状态字段、UI 控件、配置项、命令或测试点。\n\n")
			TEXT("必须使用以下标题：\n")
			TEXT("目标\n")
			TEXT("影响范围\n")
			TEXT("涉及文件/类/函数\n")
			TEXT("实现步骤\n")
			TEXT("验证步骤\n")
			TEXT("风险\n")
			TEXT("回退方案\n")
			TEXT("执行确认\n\n")
			TEXT("执行确认部分必须单独询问：是否执行这个计划？回复“执行”开始，回复“重新规划”重做方案，回复“不执行”取消。\n\n")
			TEXT("用户任务：\n%s\n\n")
			TEXT("第 1 轮摘要：\n%s\n\n")
			TEXT("第 2 轮方案：\n%s\n\n")
			TEXT("第 3 轮审查：\n%s%s"),
			*FocusOriginalRequest,
			*FocusUnderstandingSummary,
			*FocusOptionsSummary,
			*FocusCritiqueSummary,
			*ReplanBlock);
	}
}

FString SUEBridgeMCPPanel::BuildFocusExecutionPrompt(int64 ConvId, const FString& UserConfirmText) const
{
	static const FString EmptyFocusField;
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	const FString& FocusFinalPlan = Runtime ? Runtime->FocusFinalPlan : EmptyFocusField;

	const FString ConfirmText = UserConfirmText.TrimStartAndEnd().IsEmpty() ? TEXT("执行") : UserConfirmText;
	return FString::Printf(
		TEXT("【专注模式：用户已确认执行】\n")
		TEXT("用户已经确认执行下方最终落地计划。请严格按计划执行；如果执行阶段发现上下文与计划冲突，以实际读取结果为准，并在回复中说明调整。")
		TEXT("需要工具权限时走正常权限流程。完成后说明实际修改、验证结果和剩余风险。\n\n")
		TEXT("用户确认消息：\n%s\n\n")
		TEXT("已确认最终计划：\n%s"),
		*ConfirmText,
		*FocusFinalPlan);
}

bool SUEBridgeMCPPanel::IsFocusPlanningState(int64 ConvId) const
{
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	if (!Runtime)
	{
		return false;
	}
	const EFocusWorkflowState State = Runtime->FocusWorkflowState;
	return State == EFocusWorkflowState::Understanding
		|| State == EFocusWorkflowState::ExploringOptions
		|| State == EFocusWorkflowState::Critiquing
		|| State == EFocusWorkflowState::RefiningPlan
		|| State == EFocusWorkflowState::RepairingPlan;
}

bool SUEBridgeMCPPanel::IsFocusAwaitingHumanInput(int64 ConvId) const
{
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	return Runtime
		&& Runtime->FocusWorkflowState == EFocusWorkflowState::AwaitingHumanInput
		&& Runtime->PendingFocusNextRound != EFocusWorkflowState::Idle;
}

bool SUEBridgeMCPPanel::IsFocusFinalPlanComplete(const FString& PlanText) const
{
	if (PlanText.TrimStartAndEnd().Len() < 200)
	{
		return false;
	}

	static const TArray<FString> RequiredHeadings = {
		TEXT("目标"),
		TEXT("影响范围"),
		TEXT("涉及文件"),
		TEXT("实现步骤"),
		TEXT("验证步骤"),
		TEXT("风险"),
		TEXT("回退方案"),
		TEXT("执行确认")
	};
	for (const FString& Heading : RequiredHeadings)
	{
		if (!PlanText.Contains(Heading, ESearchCase::IgnoreCase))
		{
			return false;
		}
	}
	return true;
}

void SUEBridgeMCPPanel::ResetFocusWorkflow(int64 ConvId)
{
	// Use Find (not GetRuntime) so resetting a conversation that never started a focus
	// workflow doesn't create a stray runtime entry.
	FConversationRuntime* Runtime = ConvId != 0 ? ConversationRuntimes.Find(ConvId) : nullptr;
	if (!Runtime)
	{
		return;
	}
	Runtime->bFocusPlanAwaitingConfirmation = false;
	Runtime->bFocusPlanPending = false;
	Runtime->FocusWorkflowState = EFocusWorkflowState::Idle;
	Runtime->PendingFocusNextRound = EFocusWorkflowState::Idle;
	Runtime->FocusOriginalRequest.Empty();
	Runtime->FocusReplanNote.Empty();
	Runtime->FocusUnderstandingSummary.Empty();
	Runtime->FocusOptionsSummary.Empty();
	Runtime->FocusCritiqueSummary.Empty();
	Runtime->FocusFinalPlan.Empty();
	Runtime->FocusPlanRepairAttempts = 0;
}

void SUEBridgeMCPPanel::StartFocusPlanningWorkflow(const FString& PromptMessage, const FString& VisibleMessage, const FString& ReplanNote)
{
	// Focus planning always starts on the active conversation (driven from OnSendClicked).
	const int64 ConvId = EnsureConversationId(ActiveConversationIndex);
	ResetFocusWorkflow(ConvId);
	FConversationRuntime& Runtime = GetRuntime(ConvId);
	Runtime.FocusOriginalRequest = PromptMessage;
	Runtime.FocusReplanNote = ReplanNote;
	Runtime.FocusWorkflowState = EFocusWorkflowState::Understanding;
	Runtime.bFocusPlanPending = true;

	const FString InitialPrompt = BuildFocusRoundPrompt(ConvId, EFocusWorkflowState::Understanding);
	const FString PromptWithMemory = BuildPromptWithConversationMemory(InitialPrompt);
	StartConversationTurn(VisibleMessage);
	SetLastAction(LOCTEXT("FocusRound1Action", "专注模式：第 1 轮，理解需求与上下文..."));

	if (FWorldDataCodexACPClient* Client = EnsureClientForConversation(ConvId))
	{
		Client->SetPermissionMode(EWorldDataCodexPermissionMode::Focus);
		Client->SendPrompt(PromptWithMemory);
	}
}

void SUEBridgeMCPPanel::SendFocusRoundPrompt(int64 ConvId, EFocusWorkflowState Round)
{
	FWorldDataCodexACPClient* Client = EnsureClientForConversation(ConvId);
	if (!Client)
	{
		HandleAcpError(TEXT("Codex ACP 客户端未初始化。"), ConvId);
		return;
	}

	FConversationRuntime& Runtime = GetRuntime(ConvId);
	Runtime.FocusWorkflowState = Round;
	Runtime.PendingFocusNextRound = EFocusWorkflowState::Idle;
	Runtime.bFocusPlanPending = true;
	Runtime.bFocusPlanAwaitingConfirmation = false;

	FText StatusText = LOCTEXT("FocusRoundAction", "专注模式：继续多轮规划...");
	switch (Round)
	{
	case EFocusWorkflowState::ExploringOptions:
		StatusText = LOCTEXT("FocusRound2Action", "专注模式：第 2 轮，对比实现路径...");
		break;
	case EFocusWorkflowState::Critiquing:
		StatusText = LOCTEXT("FocusRound3Action", "专注模式：第 3 轮，审查风险与遗漏...");
		break;
	case EFocusWorkflowState::RefiningPlan:
		StatusText = LOCTEXT("FocusRound4Action", "专注模式：第 4 轮，收敛最终落地计划...");
		break;
	case EFocusWorkflowState::RepairingPlan:
		StatusText = LOCTEXT("FocusRepairAction", "专注模式：补全最终计划字段...");
		break;
	default:
		break;
	}
	const int32 TargetConversationIndex = FindConversationIndexById(ConvId);
	AppendConversationEventToConversation(TargetConversationIndex, EConversationMessageRole::System, StatusText.ToString());
	SetLastAction(StatusText);

	const FString PromptWithMemory = BuildPromptWithConversationMemoryForConversation(TargetConversationIndex, BuildFocusRoundPrompt(ConvId, Round));
	Client->SetPermissionMode(EWorldDataCodexPermissionMode::Focus);
	Client->SendPrompt(PromptWithMemory);
}

void SUEBridgeMCPPanel::PauseFocusWorkflowForHumanInput(int64 ConvId, EFocusWorkflowState NextRound, const FText& StatusText)
{
	FConversationRuntime& Runtime = GetRuntime(ConvId);
	Runtime.PendingFocusNextRound = NextRound;
	Runtime.bFocusPlanPending = false;
	Runtime.bFocusPlanAwaitingConfirmation = false;
	Runtime.FocusWorkflowState = EFocusWorkflowState::AwaitingHumanInput;
	AppendConversationEventToConversation(FindConversationIndexById(ConvId), EConversationMessageRole::System, StatusText.ToString());
	SetLastAction(StatusText);
}

void SUEBridgeMCPPanel::ContinueFocusWorkflowWithHumanInput(const FString& HumanInput, const FString& VisibleMessage)
{
	const int64 ConvId = GetActiveConversationId();
	if (!IsFocusAwaitingHumanInput(ConvId))
	{
		return;
	}
	FWorldDataCodexACPClient* Client = EnsureClientForConversation(ConvId);
	if (!Client)
	{
		HandleAcpError(TEXT("Codex ACP 客户端未初始化。"), ConvId);
		return;
	}
	if (Client->IsProcessing())
	{
		SetLastAction(LOCTEXT("CodexBusyAction", "Codex 正在处理上一条消息，请稍后再发送。"));
		return;
	}

	FConversationRuntime& Runtime = GetRuntime(ConvId);
	const EFocusWorkflowState NextRound = Runtime.PendingFocusNextRound;
	const FString TrimmedInput = HumanInput.TrimStartAndEnd();
	const FString DecisionText = TrimmedInput.IsEmpty()
		? TEXT("用户未补充，按上一轮结论继续。")
		: TrimmedInput;
	const FString HumanDecisionBlock = FString::Printf(TEXT("\n\n人工确认/补充：\n%s"), *DecisionText);

	switch (NextRound)
	{
	case EFocusWorkflowState::ExploringOptions:
		Runtime.FocusUnderstandingSummary += HumanDecisionBlock;
		break;
	case EFocusWorkflowState::Critiquing:
		Runtime.FocusOptionsSummary += HumanDecisionBlock;
		break;
	case EFocusWorkflowState::RefiningPlan:
		Runtime.FocusCritiqueSummary += HumanDecisionBlock;
		break;
	default:
		break;
	}

	Runtime.PendingFocusNextRound = EFocusWorkflowState::Idle;
	const FString TurnText = VisibleMessage.TrimStartAndEnd().IsEmpty()
		? TEXT("继续下一轮")
		: VisibleMessage;
	StartConversationTurn(TurnText);
	SendFocusRoundPrompt(ConvId, NextRound);
}

void SUEBridgeMCPPanel::HandleFocusRoundComplete(int64 ConvId, const FString& AssistantTurnText)
{
	FConversationRuntime& Runtime = GetRuntime(ConvId);
	const FString CleanText = AssistantTurnText.TrimStartAndEnd();
	if (CleanText.IsEmpty())
	{
		ResetFocusWorkflow(ConvId);
		SetLastAction(LOCTEXT("FocusRoundEmptyAction", "专注模式：本轮没有生成内容，已停止规划。"));
		return;
	}

	switch (Runtime.FocusWorkflowState)
	{
	case EFocusWorkflowState::Understanding:
		Runtime.FocusUnderstandingSummary = CleanText;
		PauseFocusWorkflowForHumanInput(
			ConvId,
			EFocusWorkflowState::ExploringOptions,
			LOCTEXT("FocusAwaitHumanRound2Action", "专注模式：第 1 轮已完成，请补充/选择后进入第 2 轮。"));
		return;
	case EFocusWorkflowState::ExploringOptions:
		Runtime.FocusOptionsSummary = CleanText;
		PauseFocusWorkflowForHumanInput(
			ConvId,
			EFocusWorkflowState::Critiquing,
			LOCTEXT("FocusAwaitHumanRound3Action", "专注模式：第 2 轮已完成，请确认方案取舍后进入第 3 轮。"));
		return;
	case EFocusWorkflowState::Critiquing:
		Runtime.FocusCritiqueSummary = CleanText;
		PauseFocusWorkflowForHumanInput(
			ConvId,
			EFocusWorkflowState::RefiningPlan,
			LOCTEXT("FocusAwaitHumanRound4Action", "专注模式：第 3 轮已完成，请确认风险修订后进入最终计划。"));
		return;
	case EFocusWorkflowState::RefiningPlan:
		Runtime.FocusFinalPlan = CleanText;
		if (!IsFocusFinalPlanComplete(Runtime.FocusFinalPlan) && Runtime.FocusPlanRepairAttempts < 1)
		{
			++Runtime.FocusPlanRepairAttempts;
			SendFocusRoundPrompt(ConvId, EFocusWorkflowState::RepairingPlan);
			return;
		}
		break;
	case EFocusWorkflowState::RepairingPlan:
		Runtime.FocusFinalPlan = CleanText;
		break;
	default:
		return;
	}

	Runtime.bFocusPlanPending = false;
	Runtime.bFocusPlanAwaitingConfirmation = true;
	Runtime.FocusWorkflowState = EFocusWorkflowState::AwaitingConfirmation;
	SetLastAction(LOCTEXT("FocusPlanReadyAction", "专注模式：最终计划已就绪，请确认是否执行。"));
}

namespace
{
	// Strip whitespace, surrounding punctuation and common Chinese/English filler
	// particles so short polite replies ("那就执行吧", "ok 执行", "好的") collapse to
	// their canonical confirm/cancel token. Longer task descriptions survive the
	// stripping unchanged and therefore never match the canonical sets.
	FString NormalizeFocusReply(const FString& Message)
	{
		FString S = Message.TrimStartAndEnd().ToLower();
		// Remove all internal whitespace.
		S.ReplaceInline(TEXT(" "), TEXT(""));
		S.ReplaceInline(TEXT("\t"), TEXT(""));
		S.ReplaceInline(TEXT("\r"), TEXT(""));
		S.ReplaceInline(TEXT("\n"), TEXT(""));
		// Drop punctuation that only carries tone.
		static const TCHAR* const Punct[] = {
			TEXT("。"), TEXT("．"), TEXT("."), TEXT("!"), TEXT("！"), TEXT("?"), TEXT("？"),
			TEXT("~"), TEXT("～"), TEXT(","), TEXT("，"), TEXT("、"), TEXT(";"), TEXT("；"),
			TEXT(":"), TEXT("："), TEXT("…") };
		for (const TCHAR* P : Punct)
		{
			S.ReplaceInline(P, TEXT(""));
		}

		// Iteratively peel leading/trailing filler. Longer tokens listed first so a
		// match consumes the whole particle. Never consume the entire string.
		static const TArray<FString> LeadFillers = {
			TEXT("那就"), TEXT("那么"), TEXT("好的"), TEXT("帮我"), TEXT("麻烦"),
			TEXT("ok"), TEXT("okay"), TEXT("那"), TEXT("就"), TEXT("请"), TEXT("嗯") };
		static const TArray<FString> TailFillers = {
			TEXT("一下"), TEXT("吧"), TEXT("啊"), TEXT("呗"), TEXT("了"), TEXT("咯"),
			TEXT("哈"), TEXT("呀"), TEXT("嘛"), TEXT("噢"), TEXT("哦") };

		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (const FString& Lead : LeadFillers)
			{
				if (S.Len() > Lead.Len() && S.StartsWith(Lead, ESearchCase::CaseSensitive))
				{
					S = S.RightChop(Lead.Len());
					bChanged = true;
					break;
				}
			}
			for (const FString& Tail : TailFillers)
			{
				if (S.Len() > Tail.Len() && S.EndsWith(Tail, ESearchCase::CaseSensitive))
				{
					S = S.LeftChop(Tail.Len());
					bChanged = true;
					break;
				}
			}
		}
		return S;
	}
}

bool SUEBridgeMCPPanel::IsFocusExecutionCancellation(const FString& Message)
{
	const FString Normalized = NormalizeFocusReply(Message);
	static const TSet<FString> CancelTokens = {
		TEXT("不执行"), TEXT("不要执行"), TEXT("不用执行"), TEXT("别执行"),
		TEXT("先不执行"), TEXT("暂不执行"), TEXT("先不"), TEXT("暂不"),
		TEXT("取消"), TEXT("不"), TEXT("不要"), TEXT("不行"), TEXT("不了"),
		TEXT("算了"), TEXT("算"), TEXT("停"), TEXT("停止"), TEXT("等等"),
		TEXT("no"), TEXT("n"), TEXT("cancel"), TEXT("stop"), TEXT("abort") };
	return CancelTokens.Contains(Normalized);
}

bool SUEBridgeMCPPanel::IsFocusReplanRequest(const FString& Message)
{
	const FString Normalized = NormalizeFocusReply(Message);
	static const TSet<FString> ReplanTokens = {
		TEXT("重新规划"), TEXT("重做规划"), TEXT("重做方案"), TEXT("重新计划"),
		TEXT("再想想"), TEXT("换个方案"), TEXT("重新推演"), TEXT("replan"),
		TEXT("redo"), TEXT("revise") };
	return ReplanTokens.Contains(Normalized);
}

bool SUEBridgeMCPPanel::IsFocusExecutionConfirmation(const FString& Message)
{
	const FString Normalized = NormalizeFocusReply(Message);
	if (Normalized.IsEmpty() || IsFocusExecutionCancellation(Message))
	{
		return false;
	}

	static const TSet<FString> ConfirmTokens = {
		TEXT("执行"), TEXT("开始执行"), TEXT("确认执行"), TEXT("按计划执行"),
		TEXT("执行计划"), TEXT("可以执行"), TEXT("继续"), TEXT("开始"),
		TEXT("可以"), TEXT("行"), TEXT("好"), TEXT("好的"), TEXT("同意"),
		TEXT("确认"), TEXT("没问题"), TEXT("就这样"), TEXT("这样"), TEXT("干"), TEXT("上"),
		TEXT("yes"), TEXT("y"), TEXT("ok"), TEXT("okay"), TEXT("go"),
		TEXT("run"), TEXT("proceed"), TEXT("confirm") };
	return ConfirmTokens.Contains(Normalized);
}

bool SUEBridgeMCPPanel::GetConversationStateForUpdate(
	int32 ConversationIndex,
	TArray<FConversationMessage>*& OutMessages,
	FString*& OutTranscript,
	int32*& OutAssistantMessageIndex,
	int32*& OutThoughtMessageIndex)
{
	if (ConversationIndex == ActiveConversationIndex)
	{
		OutMessages = &ConversationMessages;
		OutTranscript = &ConversationTranscript;
		OutAssistantMessageIndex = &ActiveAssistantMessageIndex;
		OutThoughtMessageIndex = &ActiveThoughtMessageIndex;
		return true;
	}

	if (Conversations.IsValidIndex(ConversationIndex))
	{
		FConversation& Conversation = Conversations[ConversationIndex];
		OutMessages = &Conversation.Messages;
		OutTranscript = &Conversation.Transcript;
		OutAssistantMessageIndex = &Conversation.ActiveAssistantMessageIndex;
		OutThoughtMessageIndex = &Conversation.ActiveThoughtMessageIndex;
		return true;
	}

	return false;
}

void SUEBridgeMCPPanel::SaveConversationIndex(int32 ConversationIndex)
{
	if (ConversationIndex == ActiveConversationIndex)
	{
		SaveActiveConversation();
	}
	else
	{
		SaveConversations();
	}
}

void SUEBridgeMCPPanel::StartConversationTurn(const FString& UserMessage)
{
	if (ActiveAssistantMessageIndex != INDEX_NONE && ConversationMessages.IsValidIndex(ActiveAssistantMessageIndex))
	{
		ConversationMessages[ActiveAssistantMessageIndex].bStreaming = false;
	}
	ActiveAssistantMessageIndex = INDEX_NONE;
	if (ActiveThoughtMessageIndex != INDEX_NONE && ConversationMessages.IsValidIndex(ActiveThoughtMessageIndex))
	{
		ConversationMessages[ActiveThoughtMessageIndex].bStreaming = false;
	}
	ActiveThoughtMessageIndex = INDEX_NONE;

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
	SaveActiveConversation();
}

void SUEBridgeMCPPanel::AppendAssistantText(const FString& Text)
{
	AppendAssistantTextToConversation(ActiveConversationIndex, Text);
}

void SUEBridgeMCPPanel::AppendAssistantTextToConversation(int32 ConversationIndex, const FString& Text)
{
	TArray<FConversationMessage>* Messages = nullptr;
	FString* Transcript = nullptr;
	int32* AssistantMessageIndex = nullptr;
	int32* ThoughtMessageIndex = nullptr;
	if (!GetConversationStateForUpdate(ConversationIndex, Messages, Transcript, AssistantMessageIndex, ThoughtMessageIndex))
	{
		return;
	}

	if (*ThoughtMessageIndex != INDEX_NONE && Messages->IsValidIndex(*ThoughtMessageIndex))
	{
		(*Messages)[*ThoughtMessageIndex].bStreaming = false;
		*ThoughtMessageIndex = INDEX_NONE;
	}

	if (*AssistantMessageIndex == INDEX_NONE || !Messages->IsValidIndex(*AssistantMessageIndex))
	{
		if (!Transcript->IsEmpty())
		{
			*Transcript += TEXT("\n\nCodex：");
		}
		*AssistantMessageIndex = AddConversationMessageToConversation(ConversationIndex, EConversationMessageRole::Assistant, FString(), true);
	}
	if (*AssistantMessageIndex == INDEX_NONE || !Messages->IsValidIndex(*AssistantMessageIndex))
	{
		return;
	}

	// Some ACP adapters stream deltas and then repeat the complete answer once.
	const FString& ExistingAssistantText = (*Messages)[*AssistantMessageIndex].Text;
	if (!ExistingAssistantText.IsEmpty() && Text == ExistingAssistantText)
	{
		(*Messages)[*AssistantMessageIndex].bStreaming = true;
		if (ConversationIndex == ActiveConversationIndex)
		{
			RebuildConversationMessages();
		}
		SaveConversationIndex(ConversationIndex);
		return;
	}

	(*Messages)[*AssistantMessageIndex].Text += Text;
	(*Messages)[*AssistantMessageIndex].bStreaming = true;
	*Transcript += Text;
	if (ConversationIndex == ActiveConversationIndex)
	{
		RefreshConversationText();
	}
	else
	{
		SaveConversationIndex(ConversationIndex);
	}
}

void SUEBridgeMCPPanel::AppendThoughtText(const FString& Text)
{
	AppendThoughtTextToConversation(ActiveConversationIndex, Text);
}

void SUEBridgeMCPPanel::AppendThoughtTextToConversation(int32 ConversationIndex, const FString& Text)
{
	TArray<FConversationMessage>* Messages = nullptr;
	FString* Transcript = nullptr;
	int32* AssistantMessageIndex = nullptr;
	int32* ThoughtMessageIndex = nullptr;
	if (!GetConversationStateForUpdate(ConversationIndex, Messages, Transcript, AssistantMessageIndex, ThoughtMessageIndex))
	{
		return;
	}

	if (*AssistantMessageIndex != INDEX_NONE && Messages->IsValidIndex(*AssistantMessageIndex))
	{
		(*Messages)[*AssistantMessageIndex].bStreaming = false;
		*AssistantMessageIndex = INDEX_NONE;
	}

	if (*ThoughtMessageIndex == INDEX_NONE || !Messages->IsValidIndex(*ThoughtMessageIndex))
	{
		*ThoughtMessageIndex = AddConversationMessageToConversation(ConversationIndex, EConversationMessageRole::Thought, FString(), true);
	}
	if (*ThoughtMessageIndex == INDEX_NONE || !Messages->IsValidIndex(*ThoughtMessageIndex))
	{
		return;
	}

	(*Messages)[*ThoughtMessageIndex].Text += Text;
	(*Messages)[*ThoughtMessageIndex].bStreaming = true;
	if (ConversationIndex == ActiveConversationIndex)
	{
		RefreshConversationText();
	}
	else
	{
		SaveConversationIndex(ConversationIndex);
	}
}

void SUEBridgeMCPPanel::AppendConversationEvent(EConversationMessageRole Role, const FString& Text)
{
	AppendConversationEventToConversation(ActiveConversationIndex, Role, Text);
}

void SUEBridgeMCPPanel::AppendConversationEventToConversation(int32 ConversationIndex, EConversationMessageRole Role, const FString& Text)
{
	TArray<FConversationMessage>* Messages = nullptr;
	FString* Transcript = nullptr;
	int32* AssistantMessageIndex = nullptr;
	int32* ThoughtMessageIndex = nullptr;
	if (!GetConversationStateForUpdate(ConversationIndex, Messages, Transcript, AssistantMessageIndex, ThoughtMessageIndex))
	{
		return;
	}

	if (*AssistantMessageIndex != INDEX_NONE && Messages->IsValidIndex(*AssistantMessageIndex))
	{
		(*Messages)[*AssistantMessageIndex].bStreaming = false;
		*AssistantMessageIndex = INDEX_NONE;
	}
	if (*ThoughtMessageIndex != INDEX_NONE && Messages->IsValidIndex(*ThoughtMessageIndex))
	{
		(*Messages)[*ThoughtMessageIndex].bStreaming = false;
		*ThoughtMessageIndex = INDEX_NONE;
	}

	AddConversationMessageToConversation(ConversationIndex, Role, Text);
	const FString RoleLabel = GetConversationRoleLabel(Role).ToString();
	*Transcript += FString::Printf(TEXT("\n\n[%s] %s\n"),
		*RoleLabel,
		*Text);
	if (ConversationIndex == ActiveConversationIndex)
	{
		RefreshConversationText();
	}
	SaveConversationIndex(ConversationIndex);
}

void SUEBridgeMCPPanel::AppendConversationText(const FString& Text)
{
	AppendConversationTextToConversation(ActiveConversationIndex, Text);
}

void SUEBridgeMCPPanel::AppendConversationTextToConversation(int32 ConversationIndex, const FString& Text)
{
	if (Text.IsEmpty())
	{
		return;
	}

	EConversationMessageRole EventRole = EConversationMessageRole::System;
	FString EventText;
	if (TryExtractConversationEvent(Text, EventRole, EventText))
	{
		AppendConversationEventToConversation(ConversationIndex, EventRole, EventText);
		return;
	}

	AppendAssistantTextToConversation(ConversationIndex, Text);
}

void SUEBridgeMCPPanel::RefreshConversationText()
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

void SUEBridgeMCPPanel::HandleAcpText(const FString& Text, int64 ConvId)
{
	AppendConversationTextToConversation(FindConversationIndexById(ConvId), Text);
}

void SUEBridgeMCPPanel::HandleAcpThought(const FString& Text, int64 ConvId)
{
	if (!Text.IsEmpty())
	{
		AppendThoughtTextToConversation(FindConversationIndexById(ConvId), Text);
	}
}

void SUEBridgeMCPPanel::HandleAcpPermission(const FWorldDataAcpPermissionRequest& Request, int64 ConvId)
{
	FConversationRuntime& Runtime = GetRuntime(ConvId);
	Runtime.bHasPendingPermission = true;
	Runtime.PendingPermissionId = Request.RequestId;
	Runtime.PendingPermissionTitle = Request.Title;
	Runtime.PendingPermissionToolName = !Request.ToolName.IsEmpty() ? Request.ToolName : Request.ToolCallId;
	Runtime.PendingAllowOptionId = Request.AllowOptionId.IsEmpty() ? TEXT("allow") : Request.AllowOptionId;
	Runtime.PendingDenyOptionId = Request.DenyOptionId.IsEmpty() ? TEXT("deny") : Request.DenyOptionId;

	// Only the conversation on screen owns the permission card; background requests pause
	// their own turn and surface via the sidebar "awaiting approval" marker.
	if (ConvId == GetActiveConversationId())
	{
		bShowSettings = false;
		bShowDetail = true;
		if (DetailTitleText.IsValid())
		{
			DetailTitleText->SetText(LOCTEXT("CodexConversationTitle", "Codex 会话"));
		}
	}

	SetLastAction(FText::Format(
		LOCTEXT("PermissionWaitingAction", "等待权限确认：{0}"),
		FText::FromString(Runtime.PendingPermissionTitle)));
}

void SUEBridgeMCPPanel::HandleAcpStatus(const FString& Text, int64 ConvId)
{
	// Keep the status line tied to the conversation on screen; background chatter would
	// otherwise overwrite it on every tool call.
	if (ConvId == GetActiveConversationId())
	{
		SetLastAction(FText::FromString(Text));
	}
}

void SUEBridgeMCPPanel::HandleAcpTurnComplete(int64 ConvId)
{
	const int32 TargetConversationIndex = FindConversationIndexById(ConvId);
	TArray<FConversationMessage>* Messages = nullptr;
	FString* Transcript = nullptr;
	int32* AssistantMessageIndex = nullptr;
	int32* ThoughtMessageIndex = nullptr;
	if (!GetConversationStateForUpdate(TargetConversationIndex, Messages, Transcript, AssistantMessageIndex, ThoughtMessageIndex))
	{
		return;
	}

	// Capture whether the assistant actually produced a plan this turn before we
	// clear the active index.
	bool bAssistantHadContent = false;
	FString AssistantTurnText;
	if (*AssistantMessageIndex != INDEX_NONE && Messages->IsValidIndex(*AssistantMessageIndex))
	{
		AssistantTurnText = (*Messages)[*AssistantMessageIndex].Text;
		bAssistantHadContent = !AssistantTurnText.TrimStartAndEnd().IsEmpty();
	}

	bool bChanged = false;
	if (*AssistantMessageIndex != INDEX_NONE && Messages->IsValidIndex(*AssistantMessageIndex))
	{
		(*Messages)[*AssistantMessageIndex].bStreaming = false;
		*AssistantMessageIndex = INDEX_NONE;
		bChanged = true;
	}
	if (*ThoughtMessageIndex != INDEX_NONE && Messages->IsValidIndex(*ThoughtMessageIndex))
	{
		(*Messages)[*ThoughtMessageIndex].bStreaming = false;
		*ThoughtMessageIndex = INDEX_NONE;
		bChanged = true;
	}
	if (bChanged)
	{
		if (TargetConversationIndex == ActiveConversationIndex)
		{
			RebuildConversationMessages();
		}
		SaveConversationIndex(TargetConversationIndex);
	}

	FConversationRuntime& Runtime = GetRuntime(ConvId);
	if (Runtime.FocusWorkflowState == EFocusWorkflowState::Executing)
	{
		Runtime.FocusWorkflowState = EFocusWorkflowState::Idle;
		if (Runtime.Client.IsValid())
		{
			Runtime.Client->SetPermissionMode(CurrentMode);
		}
		SetLastAction(LOCTEXT("FocusExecutionCompleteAction", "专注模式：执行完成。"));
		RebuildSidebar();
		return;
	}

	if (IsFocusPlanningState(ConvId))
	{
		if (bAssistantHadContent && CurrentMode == EWorldDataCodexPermissionMode::Focus)
		{
			HandleFocusRoundComplete(ConvId, AssistantTurnText);
		}
		else
		{
			ResetFocusWorkflow(ConvId);
			SetLastAction(LOCTEXT("FocusPlanningStoppedAction", "专注模式：规划未完成。"));
		}
	}

	// Refresh the sidebar so this conversation's running marker clears.
	RebuildSidebar();
}

void SUEBridgeMCPPanel::HandleAcpError(const FString& Text, int64 ConvId)
{
	const int32 TargetConversationIndex = FindConversationIndexById(ConvId);
	ClearPendingPermission(ConvId);
	// A failed planning turn never produced a plan; don't leave it armed for "执行".
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	if (IsFocusPlanningState(ConvId) || (Runtime && Runtime->bFocusPlanPending))
	{
		ResetFocusWorkflow(ConvId);
	}
	AppendConversationTextToConversation(TargetConversationIndex, FString::Printf(TEXT("\n\n[错误] %s\n"), *Text));
	SetLastAction(FText::FromString(Text));
	RebuildSidebar();
}

FReply SUEBridgeMCPPanel::OnAllowPermissionClicked()
{
	return ResolvePendingPermission(true);
}

FReply SUEBridgeMCPPanel::OnDenyPermissionClicked()
{
	return ResolvePendingPermission(false);
}

FReply SUEBridgeMCPPanel::ResolvePendingPermission(bool bAllow)
{
	const int64 ConvId = GetActiveConversationId();
	FConversationRuntime* Runtime = ConvId != 0 ? ConversationRuntimes.Find(ConvId) : nullptr;
	if (!Runtime || !Runtime->bHasPendingPermission)
	{
		return FReply::Handled();
	}

	const FString SelectedOptionId = bAllow ? Runtime->PendingAllowOptionId : Runtime->PendingDenyOptionId;
	if (Runtime->Client.IsValid())
	{
		Runtime->Client->RespondToPermission(Runtime->PendingPermissionId, SelectedOptionId);
	}

	ClearPendingPermission(ConvId);
	SetLastAction(bAllow
		? LOCTEXT("PermissionAllowedAction", "已允许权限请求。")
		: LOCTEXT("PermissionDeniedAction", "已拒绝权限请求。"));
	return FReply::Handled();
}

void SUEBridgeMCPPanel::ClearPendingPermission(int64 ConvId)
{
	FConversationRuntime* Runtime = ConvId != 0 ? ConversationRuntimes.Find(ConvId) : nullptr;
	if (!Runtime)
	{
		return;
	}
	Runtime->bHasPendingPermission = false;
	Runtime->PendingPermissionId = 0;
	Runtime->PendingPermissionTitle.Empty();
	Runtime->PendingPermissionToolName.Empty();
	Runtime->PendingAllowOptionId.Empty();
	Runtime->PendingDenyOptionId.Empty();
}

FString SUEBridgeMCPPanel::GetConversationStoragePath()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEBridgeMCP"), TEXT("conversations.json"));
}

void SUEBridgeMCPPanel::LoadConversations()
{
	Conversations.Empty();
	ActiveConversationIndex = INDEX_NONE;

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *GetConversationStoragePath()))
	{
		return;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	double SavedActiveIndex = 0.0;
	if (Root->TryGetNumberField(TEXT("activeIndex"), SavedActiveIndex))
	{
		ActiveConversationIndex = static_cast<int32>(SavedActiveIndex);
	}

	const TArray<TSharedPtr<FJsonValue>>* ConversationArray = nullptr;
	if (!Root->TryGetArrayField(TEXT("conversations"), ConversationArray) || !ConversationArray)
	{
		return;
	}

	auto ParseRole = [](const FString& RoleName, EConversationMessageRole& OutRole)
	{
		if (RoleName == TEXT("user")) { OutRole = EConversationMessageRole::User; return true; }
		if (RoleName == TEXT("assistant")) { OutRole = EConversationMessageRole::Assistant; return true; }
		if (RoleName == TEXT("system")) { OutRole = EConversationMessageRole::System; return true; }
		if (RoleName == TEXT("tool")) { OutRole = EConversationMessageRole::Tool; return true; }
		if (RoleName == TEXT("error")) { OutRole = EConversationMessageRole::Error; return true; }
		if (RoleName == TEXT("thought")) { OutRole = EConversationMessageRole::Thought; return true; }
		return false;
	};

	const int32 MaxLoadedConversations = 80;
	for (const TSharedPtr<FJsonValue>& ConversationValue : *ConversationArray)
	{
		if (Conversations.Num() >= MaxLoadedConversations || !ConversationValue.IsValid() || ConversationValue->Type != EJson::Object)
		{
			break;
		}

		const TSharedPtr<FJsonObject> ConversationObject = ConversationValue->AsObject();
		if (!ConversationObject.IsValid())
		{
			continue;
		}

		FConversation Conversation;
		FString Title;
		if (ConversationObject->TryGetStringField(TEXT("title"), Title) && !Title.IsEmpty())
		{
			Conversation.Title = FText::FromString(Title);
		}
		else
		{
			Conversation.Title = LOCTEXT("NewConversationTitle", "新对话");
		}

		double CreatedAtUnix = 0.0;
		if (ConversationObject->TryGetNumberField(TEXT("createdAt"), CreatedAtUnix))
		{
			Conversation.CreatedAt = FDateTime::FromUnixTimestamp(static_cast<int64>(CreatedAtUnix));
		}
		else
		{
			Conversation.CreatedAt = FDateTime::Now();
		}

		double IdValue = 0.0;
		if (ConversationObject->TryGetNumberField(TEXT("id"), IdValue))
		{
			Conversation.Id = static_cast<int64>(IdValue);
		}

		ConversationObject->TryGetBoolField(TEXT("hasCustomTitle"), Conversation.bHasCustomTitle);
		ConversationObject->TryGetStringField(TEXT("transcript"), Conversation.Transcript);
		Conversation.ActiveAssistantMessageIndex = INDEX_NONE;
		Conversation.ActiveThoughtMessageIndex = INDEX_NONE;

		const TArray<TSharedPtr<FJsonValue>>* MessageArray = nullptr;
		if (ConversationObject->TryGetArrayField(TEXT("messages"), MessageArray) && MessageArray)
		{
			for (const TSharedPtr<FJsonValue>& MessageValue : *MessageArray)
			{
				if (!MessageValue.IsValid() || MessageValue->Type != EJson::Object)
				{
					continue;
				}

				const TSharedPtr<FJsonObject> MessageObject = MessageValue->AsObject();
				FString RoleName;
				FString MessageText;
				EConversationMessageRole Role = EConversationMessageRole::Assistant;
				if (!MessageObject.IsValid()
					|| !MessageObject->TryGetStringField(TEXT("role"), RoleName)
					|| !ParseRole(RoleName, Role)
					|| !MessageObject->TryGetStringField(TEXT("text"), MessageText))
				{
					continue;
				}

				FConversationMessage Message;
				Message.Role = Role;
				Message.Text = MessageText;
				Message.bStreaming = false;
				Conversation.Messages.Add(MoveTemp(Message));
			}
		}

		Conversations.Add(MoveTemp(Conversation));
	}

	// Assign stable ids to any legacy/missing entries and advance the id counter past them.
	int64 MaxId = 0;
	for (const FConversation& Conversation : Conversations)
	{
		MaxId = FMath::Max(MaxId, Conversation.Id);
	}
	NextConversationId = MaxId + 1;
	for (FConversation& Conversation : Conversations)
	{
		if (Conversation.Id == 0)
		{
			Conversation.Id = NextConversationId++;
		}
	}

	if (!Conversations.IsValidIndex(ActiveConversationIndex))
	{
		ActiveConversationIndex = Conversations.Num() > 0 ? 0 : INDEX_NONE;
	}
}

void SUEBridgeMCPPanel::SaveConversations() const
{
	const FString StoragePath = GetConversationStoragePath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(StoragePath), true);

	auto RoleToString = [](EConversationMessageRole Role) -> const TCHAR*
	{
		switch (Role)
		{
		case EConversationMessageRole::User: return TEXT("user");
		case EConversationMessageRole::Assistant: return TEXT("assistant");
		case EConversationMessageRole::System: return TEXT("system");
		case EConversationMessageRole::Tool: return TEXT("tool");
		case EConversationMessageRole::Error: return TEXT("error");
		case EConversationMessageRole::Thought: return TEXT("thought");
		default: return TEXT("assistant");
		}
	};

	TArray<TSharedPtr<FJsonValue>> ConversationArray;
	for (int32 Index = 0; Index < Conversations.Num(); ++Index)
	{
		const FConversation& Conversation = Conversations[Index];
		const bool bActive = Index == ActiveConversationIndex;
		const TArray<FConversationMessage>& SourceMessages = bActive ? ConversationMessages : Conversation.Messages;
		const FString& SourceTranscript = bActive ? ConversationTranscript : Conversation.Transcript;

		TSharedPtr<FJsonObject> ConversationObject = MakeShared<FJsonObject>();
		ConversationObject->SetNumberField(TEXT("id"), static_cast<double>(Conversation.Id));
		ConversationObject->SetStringField(TEXT("title"), Conversation.Title.ToString());
		ConversationObject->SetNumberField(TEXT("createdAt"), static_cast<double>(Conversation.CreatedAt.ToUnixTimestamp()));
		ConversationObject->SetBoolField(TEXT("hasCustomTitle"), Conversation.bHasCustomTitle);
		ConversationObject->SetStringField(TEXT("transcript"), SourceTranscript);

		TArray<TSharedPtr<FJsonValue>> MessageArray;
		for (const FConversationMessage& Message : SourceMessages)
		{
			TSharedPtr<FJsonObject> MessageObject = MakeShared<FJsonObject>();
			MessageObject->SetStringField(TEXT("role"), RoleToString(Message.Role));
			MessageObject->SetStringField(TEXT("text"), Message.Text);
			MessageArray.Add(MakeShared<FJsonValueObject>(MessageObject));
		}
		ConversationObject->SetArrayField(TEXT("messages"), MessageArray);
		ConversationArray.Add(MakeShared<FJsonValueObject>(ConversationObject));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetNumberField(TEXT("activeIndex"), ActiveConversationIndex);
	Root->SetArrayField(TEXT("conversations"), ConversationArray);

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(Out, *StoragePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

void SUEBridgeMCPPanel::ResetConversationView()
{
	SaveActiveConversation();

	FConversation NewConversation;
	NewConversation.Id = NextConversationId++;
	NewConversation.Title = LOCTEXT("NewConversationTitle", "新对话");
	NewConversation.CreatedAt = FDateTime::Now();
	Conversations.Insert(NewConversation, 0);
	ActiveConversationIndex = 0;

	// The new conversation starts with a fresh (empty) runtime created on first send; any
	// background conversation keeps its own client, focus workflow and pending permission.
	bShowSettings = false;
	bShowDetail = false;
	bDetailIsConversation = false;
	CurrentDetailText.Empty();
	ConversationTranscript.Empty();
	ConversationMessages.Empty();
	ActiveAssistantMessageIndex = INDEX_NONE;
	ActiveThoughtMessageIndex = INDEX_NONE;
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
	ClearComposerDraft();
	ClearComposerAttachments();
	SaveConversations();
	SetLastAction(LOCTEXT("NewConversationAction", "新对话已准备好。"));
}

void SUEBridgeMCPPanel::SaveActiveConversation()
{
	if (Conversations.IsValidIndex(ActiveConversationIndex))
	{
		FConversation& Conversation = Conversations[ActiveConversationIndex];
		Conversation.Messages = ConversationMessages;
		Conversation.Transcript = ConversationTranscript;
		Conversation.ActiveAssistantMessageIndex = ActiveAssistantMessageIndex;
		Conversation.ActiveThoughtMessageIndex = ActiveThoughtMessageIndex;
		SaveConversations();
	}
}

void SUEBridgeMCPPanel::LoadConversation(int32 Index)
{
	if (!Conversations.IsValidIndex(Index))
	{
		return;
	}

	SaveActiveConversation();
	EnsureConversationId(Index);
	ActiveConversationIndex = Index;

	const FConversation& Conversation = Conversations[Index];
	ConversationMessages = Conversation.Messages;
	ConversationTranscript = Conversation.Transcript;
	ActiveAssistantMessageIndex = Conversation.ActiveAssistantMessageIndex;
	ActiveThoughtMessageIndex = Conversation.ActiveThoughtMessageIndex;
	CurrentDetailText = Conversation.Transcript;

	// Switching only changes which conversation is on screen; its own runtime carries its
	// focus workflow and any pending permission, so nothing is reset here.
	const FConversationRuntime* Runtime = FindRuntime(Conversation.Id);
	bShowSettings = false;
	bShowDetail = ConversationMessages.Num() > 0 || (Runtime && Runtime->bHasPendingPermission);
	bDetailIsConversation = true;

	RebuildConversationMessages();
	RebuildSidebar();
	if (DetailTitleText.IsValid())
	{
		DetailTitleText->SetText(ConversationMessages.Num() > 0
			? LOCTEXT("CodexConversationTitle", "Codex 会话")
			: LOCTEXT("NewConversationTitle", "新对话"));
	}
	ClearComposerDraft();
	ClearComposerAttachments();
	SaveConversations();
	SetLastAction(FText::Format(LOCTEXT("SwitchedConversationAction", "已切换到对话：{0}"), GetConversationTitle(Index)));
}

FReply SUEBridgeMCPPanel::OnSelectConversation(int32 Index)
{
	if (Index != ActiveConversationIndex)
	{
		LoadConversation(Index);
	}
	return FReply::Handled();
}

FText SUEBridgeMCPPanel::GetConversationTitle(int32 Index) const
{
	return Conversations.IsValidIndex(Index)
		? Conversations[Index].Title
		: LOCTEXT("NewConversationTitle", "新对话");
}

FText SUEBridgeMCPPanel::MakeConversationTitle(const FString& Message) const
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

FText SUEBridgeMCPPanel::GetConversationAgeText(FDateTime CreatedAt) const
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

TSharedRef<SWidget> SUEBridgeMCPPanel::BuildConversationEntry(int32 Index)
{
	const FConversation& Conversation = Conversations[Index];
	const FDateTime CreatedAt = Conversation.CreatedAt;
	const int64 ConvId = Conversation.Id;
	return BuildConversationItem(
		Conversation.Title,
		TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateSP(this, &SUEBridgeMCPPanel::GetConversationAgeText, CreatedAt)),
		Index == ActiveConversationIndex,
		FOnClicked::CreateSP(this, &SUEBridgeMCPPanel::OnSelectConversation, Index),
		TAttribute<bool>::Create(TAttribute<bool>::FGetter::CreateSP(this, &SUEBridgeMCPPanel::IsConversationBusy, ConvId)));
}

void SUEBridgeMCPPanel::RebuildSidebar()
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

void SUEBridgeMCPPanel::ShowProjectInfo()
{
	SetDetail(LOCTEXT("ProjectInfoTitle", "项目信息"), FWorldDataMCPServer::GetProjectInfoJson());
}

FReply SUEBridgeMCPPanel::OnNewConversationClicked()
{
	ResetConversationView();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnSettingsClicked()
{
	bShowSettings = true;
	SetLastAction(LOCTEXT("SettingsOpenedAction", "已打开设置。"));
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnSendClicked()
{
	if (IsComposerBusy())
	{
		StopActiveAnswer();
		return FReply::Handled();
	}

	const int64 ConvId = EnsureConversationId(ActiveConversationIndex);
	const FString Message = ComposerTextBox.IsValid() ? ComposerTextBox->GetText().ToString().TrimStartAndEnd() : FString();
	const bool bHasAttachments = ComposerAttachments.Num() > 0;
	const bool bFocusMode = CurrentMode == EWorldDataCodexPermissionMode::Focus;
	const bool bFocusHumanInput = bFocusMode && IsFocusAwaitingHumanInput(ConvId);
	if (Message.IsEmpty() && !bHasAttachments && !bFocusHumanInput)
	{
		SetLastAction(LOCTEXT("EmptyMessageAction", "请输入消息或附加文件后再发送。"));
		return FReply::Handled();
	}

	// Confirm / cancel are only meaningful while THIS conversation's plan awaits an answer.
	const FConversationRuntime* ActiveRuntime = FindRuntime(ConvId);
	const bool bAwaitingConfirmation = ActiveRuntime && ActiveRuntime->bFocusPlanAwaitingConfirmation;
	if (bFocusMode && bAwaitingConfirmation && !bHasAttachments)
	{
		if (IsFocusReplanRequest(Message))
		{
			ClearComposerDraft();
			ClearComposerAttachments();
			ReplanFocusWorkflow(Message);
			return FReply::Handled();
		}
		if (IsFocusExecutionCancellation(Message))
		{
			ClearComposerDraft();
			ClearComposerAttachments();
			CancelFocusPlanExecution(Message);
			return FReply::Handled();
		}
		if (IsFocusExecutionConfirmation(Message))
		{
			ClearComposerDraft();
			ClearComposerAttachments();
			ConfirmFocusPlanExecution(Message);
			return FReply::Handled();
		}
	}

	FWorldDataCodexACPClient* Client = EnsureClientForConversation(ConvId);
	if (!Client)
	{
		HandleAcpError(TEXT("Codex ACP 客户端未初始化。"), ConvId);
		return FReply::Handled();
	}
	// Only this conversation's own client gates the send, so other conversations can run
	// their turns at the same time.
	if (Client->IsProcessing())
	{
		SetLastAction(LOCTEXT("CodexBusyAction", "Codex 正在处理上一条消息，请稍后再发送。"));
		return FReply::Handled();
	}

	FString PromptMessage = Message.IsEmpty() ? TEXT("请查看我附加的文件。") : Message;
	const FString AttachmentBlock = BuildAttachmentPromptBlock();
	if (!AttachmentBlock.IsEmpty())
	{
		PromptMessage += TEXT("\n\n");
		PromptMessage += AttachmentBlock;
	}
	FString VisibleMessage = Message.IsEmpty() ? TEXT("请查看附件。") : Message;
	VisibleMessage += BuildVisibleAttachmentSummary();
	if (bFocusHumanInput && Message.IsEmpty() && !bHasAttachments)
	{
		PromptMessage.Reset();
		VisibleMessage = TEXT("继续下一轮");
	}

	ClearComposerDraft();
	ClearComposerAttachments();

	if (bFocusHumanInput)
	{
		ContinueFocusWorkflowWithHumanInput(PromptMessage, VisibleMessage);
		return FReply::Handled();
	}

	if (bFocusMode)
	{
		StartFocusPlanningWorkflow(PromptMessage, VisibleMessage);
		return FReply::Handled();
	}

	ResetFocusWorkflow(ConvId);
	const FString PromptWithMemory = BuildPromptWithConversationMemory(PromptMessage);
	StartConversationTurn(VisibleMessage);
	SetLastAction(LOCTEXT("SendingToCodexAction", "正在发送到 Codex ACP..."));

	Client->SetPermissionMode(CurrentMode);
	Client->SendPrompt(PromptWithMemory);
	RebuildSidebar();
	return FReply::Handled();
}

void SUEBridgeMCPPanel::ConfirmFocusPlanExecution(const FString& UserConfirmText)
{
	const int64 ConvId = GetActiveConversationId();
	FConversationRuntime* Runtime = ConversationRuntimes.Find(ConvId);
	if (!Runtime || !Runtime->bFocusPlanAwaitingConfirmation)
	{
		return;
	}
	FWorldDataCodexACPClient* Client = EnsureClientForConversation(ConvId);
	if (!Client)
	{
		HandleAcpError(TEXT("Codex ACP 客户端未初始化。"), ConvId);
		return;
	}
	if (Client->IsProcessing())
	{
		SetLastAction(LOCTEXT("CodexBusyAction", "Codex 正在处理上一条消息，请稍后再发送。"));
		return;
	}

	const FString ConfirmText = UserConfirmText.TrimStartAndEnd().IsEmpty() ? TEXT("执行") : UserConfirmText;
	const FString PromptMessage = BuildFocusExecutionPrompt(ConvId, ConfirmText);
	const FString PromptWithMemory = BuildPromptWithConversationMemory(PromptMessage);

	StartConversationTurn(ConfirmText);
	Runtime->bFocusPlanAwaitingConfirmation = false;
	Runtime->bFocusPlanPending = false;
	Runtime->FocusWorkflowState = EFocusWorkflowState::Executing;
	SetLastAction(LOCTEXT("FocusExecutingAction", "专注模式：已确认，开始执行计划..."));

	// Drop to Default for the execution turn so tool calls go through the normal
	// per-call permission flow instead of the focus/plan hard block.
	Client->SetPermissionMode(EWorldDataCodexPermissionMode::Default);
	Client->SendPrompt(PromptWithMemory);
	RebuildSidebar();
}

void SUEBridgeMCPPanel::CancelFocusPlanExecution(const FString& VisibleMessage)
{
	const int64 ConvId = GetActiveConversationId();
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	if (!Runtime || !Runtime->bFocusPlanAwaitingConfirmation)
	{
		return;
	}

	const FString TurnText = VisibleMessage.TrimStartAndEnd().IsEmpty() ? TEXT("不执行") : VisibleMessage;
	StartConversationTurn(TurnText);
	ResetFocusWorkflow(ConvId);
	AppendConversationEvent(EConversationMessageRole::System, TEXT("专注模式：已取消执行计划。"));
	SetLastAction(LOCTEXT("FocusPlanCancelledAction", "专注模式：已取消执行计划。"));
}

void SUEBridgeMCPPanel::ReplanFocusWorkflow(const FString& VisibleMessage)
{
	const int64 ConvId = GetActiveConversationId();
	const FConversationRuntime* Runtime = FindRuntime(ConvId);
	if (!Runtime || !Runtime->bFocusPlanAwaitingConfirmation)
	{
		return;
	}
	FWorldDataCodexACPClient* Client = EnsureClientForConversation(ConvId);
	if (!Client)
	{
		HandleAcpError(TEXT("Codex ACP 客户端未初始化。"), ConvId);
		return;
	}
	if (Client->IsProcessing())
	{
		SetLastAction(LOCTEXT("CodexBusyAction", "Codex 正在处理上一条消息，请稍后再发送。"));
		return;
	}

	const FString OriginalRequest = Runtime->FocusOriginalRequest.IsEmpty() ? TEXT("请根据上一轮需求重新规划。") : Runtime->FocusOriginalRequest;
	const FString PreviousPlan = Runtime->FocusFinalPlan;
	const FString TurnText = VisibleMessage.TrimStartAndEnd().IsEmpty() ? TEXT("重新规划") : VisibleMessage;
	const FString ReplanNote = FString::Printf(
		TEXT("用户要求重新规划。上一版最终计划仅作为反例和上下文参考，请重新审视取舍，输出更落地、更稳妥的方案。\n\n上一版最终计划：\n%s"),
		*PreviousPlan);
	StartFocusPlanningWorkflow(OriginalRequest, TurnText, ReplanNote);
}

FReply SUEBridgeMCPPanel::OnFocusContinueClicked()
{
	return IsFocusAwaitingHumanInput(GetActiveConversationId()) ? OnSendClicked() : FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnFocusExecuteClicked()
{
	ConfirmFocusPlanExecution(FString());
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnFocusReplanClicked()
{
	ReplanFocusWorkflow(FString());
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnFocusCancelClicked()
{
	CancelFocusPlanExecution(FString());
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnStartClicked()
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

FReply SUEBridgeMCPPanel::OnStopClicked()
{
	FWorldDataMCPServer::Stop();
	SetLastAction(LOCTEXT("StoppedAction", "已停止。"));
	ShowProjectInfo();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnRefreshClicked()
{
	FWorldDataMCPServer::RefreshConnectionFiles();
	SetLastAction(LOCTEXT("RefreshedAction", "连接文件已刷新。"));
	ShowProjectInfo();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnStatusClicked()
{
	SetLastAction(LOCTEXT("ViewedStatusAction", "已查看状态。"));
	SetDetail(LOCTEXT("StatusTitle", "服务状态"), FWorldDataMCPServer::GetStatusJson());
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnProjectInfoClicked()
{
	SetLastAction(LOCTEXT("ViewedProjectInfoAction", "已查看项目信息。"));
	ShowProjectInfo();
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnBootstrapClicked()
{
	SetLastAction(LOCTEXT("ViewedBootstrapAction", "已查看启动上下文。"));
	SetDetail(LOCTEXT("BootstrapTitle", "启动上下文"), FWorldDataMCPServer::ReadResource(TEXT("worlddata://context/bootstrap")));
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnPolicyClicked()
{
	SetLastAction(LOCTEXT("ViewedPolicyAction", "已查看 Codex 策略快照。"));
	SetDetail(LOCTEXT("PolicyTitle", "Codex 策略快照"), FWorldDataMCPServer::ReadResource(TEXT("worlddata://codex/policy-snapshot")));
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnToolsClicked()
{
	SetLastAction(LOCTEXT("ViewedToolsAction", "已查看工具列表。"));
	SetDetail(LOCTEXT("ToolsTitle", "工具列表"), FWorldDataMCPServer::GetToolDefinitionsJson());
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnResourcesClicked()
{
	SetLastAction(LOCTEXT("ViewedResourcesAction", "已查看资源列表。"));
	SetDetail(LOCTEXT("ResourcesTitle", "资源列表"), FWorldDataMCPServer::GetResourceListJson());
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnCopyUrlClicked()
{
	if (FWorldDataMCPServer::IsRunning())
	{
		UEBridgeMCP::CopyToClipboard(FWorldDataMCPServer::GetMcpUrl());
		SetLastAction(LOCTEXT("CopiedUrlAction", "已复制 MCP 地址。"));
		UEBridgeMCP::Notify(LOCTEXT("CopiedUrlNotification", "MCP 地址已复制。"));
	}
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnCopyConfigClicked()
{
	if (FWorldDataMCPServer::IsRunning())
	{
		UEBridgeMCP::CopyToClipboard(UEBridgeMCP::BuildClientConfigSnippet());
		SetLastAction(LOCTEXT("CopiedConfigAction", "已复制 MCP 配置。"));
		UEBridgeMCP::Notify(LOCTEXT("CopiedConfigNotification", "MCP 配置已复制。"));
	}
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnCopyCurrentClicked()
{
	if (!CurrentDetailText.IsEmpty())
	{
		UEBridgeMCP::CopyToClipboard(CurrentDetailText);
		SetLastAction(LOCTEXT("CopiedViewAction", "已复制当前内容。"));
		UEBridgeMCP::Notify(LOCTEXT("CopiedViewNotification", "当前面板内容已复制。"));
	}
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnOpenProjectFolderClicked()
{
	UEBridgeMCP::ExploreFileParent(FWorldDataMCPServer::GetClientConfigFilePath());
	SetLastAction(LOCTEXT("OpenedProjectFolderAction", "已打开项目目录。"));
	return FReply::Handled();
}

FReply SUEBridgeMCPPanel::OnOpenSavedFolderClicked()
{
	UEBridgeMCP::ExploreFileParent(FWorldDataMCPServer::GetConnectionFilePath());
	SetLastAction(LOCTEXT("OpenedSavedFolderAction", "已打开 Saved 目录。"));
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
