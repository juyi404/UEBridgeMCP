#pragma once

#include "WorldDataCodexACPClient.h"
#include "UEBridgeMCPStyle.h"

#include "Framework/SlateDelegates.h"
#include "Misc/DateTime.h"
#include "Styling/SlateTypes.h"
#include "Widgets/SCompoundWidget.h"

class SButton;
class SScrollBox;
class SMultiLineEditableTextBox;
class STextBlock;
class SWidget;
class SWidgetSwitcher;
class SWrapBox;
class FDragDropEvent;
struct FKeyEvent;

class SUEBridgeMCPPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SUEBridgeMCPPanel)
		: _StartNewConversation(false)
	{}
		SLATE_ARGUMENT(bool, StartNewConversation)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	~SUEBridgeMCPPanel();

private:
	enum class EConversationMessageRole : uint8
	{
		User,
		Assistant,
		System,
		Tool,
		Error,
		Thought
	};

	enum class ECliTool : uint8
	{
		Codex,
		Cursor,
		ClaudeCode
	};

	enum class EFocusWorkflowState : uint8
	{
		Idle,
		Understanding,
		ExploringOptions,
		Critiquing,
		RefiningPlan,
		RepairingPlan,
		AwaitingHumanInput,
		AwaitingConfirmation,
		Executing
	};

	struct FConversationMessage
	{
		EConversationMessageRole Role = EConversationMessageRole::Assistant;
		FString Text;
		bool bStreaming = false;
	};

	struct FConversation
	{
		// Stable identity, assigned once and persisted. Used to key the per-conversation
		// runtime (and to route ACP callbacks) so inserting/reordering sidebar entries
		// never mis-addresses a background turn. 0 = unassigned (legacy/just-created).
		int64 Id = 0;
		FText Title;
		FDateTime CreatedAt = FDateTime::Now();
		bool bHasCustomTitle = false;
		TArray<FConversationMessage> Messages;
		FString Transcript;
		int32 ActiveAssistantMessageIndex = INDEX_NONE;
		int32 ActiveThoughtMessageIndex = INDEX_NONE;
	};

	// Per-conversation runtime state that is NOT shared across conversations and is never
	// serialized: the conversation's own ACP client (its own adapter process + ACP session)
	// plus the focus-mode planning workflow and the in-flight tool-permission request. Stored
	// in ConversationRuntimes keyed by FConversation::Id and addressed by ACP callbacks via a
	// payload-bound ConvId, so each conversation runs and streams independently of which one
	// is currently on screen.
	struct FConversationRuntime
	{
		TSharedPtr<FWorldDataCodexACPClient> Client;

		// Focus-mode (专注模式) multi-round planning workflow for THIS conversation.
		EFocusWorkflowState FocusWorkflowState = EFocusWorkflowState::Idle;
		EFocusWorkflowState PendingFocusNextRound = EFocusWorkflowState::Idle;
		bool bFocusPlanAwaitingConfirmation = false;
		// True after a focus-mode planning prompt is sent, until the assistant's plan
		// actually arrives. Only then does bFocusPlanAwaitingConfirmation flip on, so a
		// stray "执行" cannot be confirmed against a plan that never materialised.
		bool bFocusPlanPending = false;
		FString FocusOriginalRequest;
		FString FocusReplanNote;
		FString FocusUnderstandingSummary;
		FString FocusOptionsSummary;
		FString FocusCritiqueSummary;
		FString FocusFinalPlan;
		int32 FocusPlanRepairAttempts = 0;

		// Pending tool-permission request awaiting the user's Allow/Deny for THIS conversation.
		bool bHasPendingPermission = false;
		int32 PendingPermissionId = 0;
		FString PendingPermissionTitle;
		FString PendingPermissionToolName;
		FString PendingAllowOptionId;
		FString PendingDenyOptionId;
	};

	struct FComposerAttachment
	{
		FString Path;
		FString Name;
		FString Content;
		FString Error;
		int64 SizeBytes = 0;
		bool bInlineContent = false;
	};

	TSharedRef<SWidget> BuildUEBridgeStyleLayout();
	TSharedRef<SWidget> BuildUEBridgeTopBar();
	TSharedRef<SWidget> BuildUEBridgeSidebar();
	TSharedRef<SWidget> BuildUEBridgeMainArea();
	TSharedRef<SWidget> BuildBadge(const FText& Text) const;
	TSharedRef<SWidget> BuildToolbarButton(const FText& Label, FOnClicked OnClicked, TAttribute<bool> bIsEnabled = TAttribute<bool>(true)) const;
	TSharedRef<SWidget> BuildIconTextButton(const FText& Label, FOnClicked OnClicked, const FText& Tooltip = FText::GetEmpty()) const;
	TSharedRef<SWidget> BuildModeCombo();
	TSharedRef<SWidget> BuildModeMenu();
	TSharedRef<SWidget> BuildModeMenuItem(EWorldDataCodexPermissionMode Mode, const FText& Label, const FText& Description);
	FText GetModeLabel(EWorldDataCodexPermissionMode Mode) const;
	TSharedRef<SWidget> BuildAgentCombo();
	TSharedRef<SWidget> BuildAgentMenu();
	TSharedRef<SWidget> BuildAgentMenuItem(EWorldDataAcpAgent Agent, const FText& Label, const FText& Description);
	FText GetAgentLabel(EWorldDataAcpAgent Agent) const;
	// Model selector for the launched CLI. Empty id = adapter default.
	TSharedRef<SWidget> BuildModelCombo();
	TSharedRef<SWidget> BuildModelMenu();
	TSharedRef<SWidget> BuildModelMenuItem(const FString& ModelId, const FText& Label, const FText& Description);
	FText GetModelLabel(const FString& ModelId) const;
	FText GetModelMenuTitle() const;
	bool IsModelValidForAgent(EWorldDataAcpAgent Agent, const FString& ModelId) const;
	FString SanitizeModelForAgent(EWorldDataAcpAgent Agent, const FString& ModelId) const;
	void NormalizeAgentModels();
	FString GetCurrentAgentModel() const;
	void SetCurrentAgentModel(const FString& ModelId);
	TSharedRef<SWidget> BuildDateLabel(const FText& Label) const;
	TSharedRef<SWidget> BuildConversationItem(const FText& Title, TAttribute<FText> Age, bool bActive, FOnClicked OnClicked = FOnClicked(), TAttribute<bool> IsBusy = TAttribute<bool>(false)) const;
	TSharedRef<SWidget> BuildConversationDetail();
	TSharedRef<SWidget> BuildConversationMessagesView();
	TSharedRef<SWidget> BuildConversationMessageWidget(const FConversationMessage& Message) const;
	TSharedRef<SWidget> BuildConversationMarkdownContent(const FString& Text, EConversationMessageRole Role) const;
	TSharedRef<SWidget> BuildToolPill(const FConversationMessage& Message) const;
	TSharedRef<SWidget> BuildRunningIndicator() const;
	bool IsAgentWorking() const;
	FText GetRunningIndicatorText() const;
	static FString HumanizeToolName(const FString& Raw);
	FText GetConversationRoleLabel(EConversationMessageRole Role) const;
	FLinearColor GetConversationMessageBackgroundColor(EConversationMessageRole Role) const;
	FLinearColor GetConversationMessageBorderColor(EConversationMessageRole Role) const;
	FLinearColor GetConversationMessageTextColor(EConversationMessageRole Role) const;
	FLinearColor GetConversationMessageMutedColor(EConversationMessageRole Role) const;
	TSharedRef<SWidget> BuildPermissionRequestCard();
	TSharedRef<SWidget> BuildPermissionActionButton(const FText& Label, bool bPrimary, FOnClicked OnClicked) const;
	TSharedRef<SWidget> BuildSettingsPanel();
	TSharedRef<SWidget> BuildSettingsContent();
	TSharedRef<SWidget> BuildMcpServerPanel();
	TSharedRef<SWidget> BuildServerStatusCard();
	TSharedRef<SWidget> BuildServerClientsBanner();
	TSharedRef<SWidget> BuildServerPortCard();
	TSharedRef<SWidget> BuildRegisteredToolsCard();
	TSharedRef<SWidget> BuildToolChip(const FString& ToolName);
	FText GetServerStatusText() const;
	FText GetServerToggleText() const;
	int32 ParseServerPort() const;
	const TArray<FString>& GetRegisteredToolNames();
	FReply OnToggleServerClicked();
	FReply OnApplyPortClicked();
	TSharedRef<SWidget> BuildCliSettingsPanel();
	TSharedRef<SWidget> BuildCliSettingsRow(ECliTool Tool);
	TSharedRef<SWidget> BuildCliStatusBadge(ECliTool Tool) const;
	TSharedRef<SWidget> BuildComposer();
	TSharedRef<SWidget> BuildComposerAttachmentList();
	TSharedRef<SWidget> BuildComposerAttachmentChip(int32 AttachmentIndex);
	TSharedRef<SWidget> BuildComposerActionContent() const;
	void RebuildComposerAttachments();
	void FocusComposer();
	void ClearComposerDraft();
	bool AddComposerAttachment(const FString& Path, FString* OutError = nullptr);
	int32 AddComposerAttachments(const TArray<FString>& Paths);
	int32 AddComposerAttachmentsFromClipboard();
	void ClearComposerAttachments();
	FString BuildAttachmentPromptBlock() const;
	FString BuildVisibleAttachmentSummary() const;
	FString BuildFocusRoundPrompt(int64 ConvId, EFocusWorkflowState Round) const;
	FString BuildFocusExecutionPrompt(int64 ConvId, const FString& UserConfirmText) const;
	// Focus-mode workflow operates on a specific conversation's runtime. The user-initiated
	// entry points act on the active conversation; the callback-driven steps (round-complete,
	// next-round, reset, state queries) take the owning ConvId so a background conversation can
	// keep advancing its own plan.
	void StartFocusPlanningWorkflow(const FString& PromptMessage, const FString& VisibleMessage, const FString& ReplanNote = FString());
	void SendFocusRoundPrompt(int64 ConvId, EFocusWorkflowState Round);
	void HandleFocusRoundComplete(int64 ConvId, const FString& AssistantTurnText);
	void PauseFocusWorkflowForHumanInput(int64 ConvId, EFocusWorkflowState NextRound, const FText& StatusText);
	void ContinueFocusWorkflowWithHumanInput(const FString& HumanInput, const FString& VisibleMessage);
	void ResetFocusWorkflow(int64 ConvId);
	bool IsFocusAwaitingHumanInput(int64 ConvId) const;
	bool IsFocusPlanningState(int64 ConvId) const;
	bool IsFocusFinalPlanComplete(const FString& PlanText) const;
	static bool IsFocusExecutionConfirmation(const FString& Message);
	static bool IsFocusExecutionCancellation(const FString& Message);
	static bool IsFocusReplanRequest(const FString& Message);
	void ConfirmFocusPlanExecution(const FString& UserConfirmText);
	void CancelFocusPlanExecution(const FString& VisibleMessage);
	void ReplanFocusWorkflow(const FString& VisibleMessage);
	TSharedRef<SWidget> BuildFocusConfirmBar();
	static bool TryExtractPastedFilePaths(const FString& ClipboardText, TArray<FString>& OutPaths);
	static bool IsAttachmentTextFile(const FString& FullPath);
	static FString FormatAttachmentSize(int64 SizeBytes);
	bool IsComposerBusy() const;
	void StopActiveAnswer();
	void ConfigureLightTextBoxStyle();
	void ConfigureComposerButtonStyle();
	FLinearColor GetAccentFillColor(float Alpha) const;
	FLinearColor GetAccentSurfaceColor() const;
	FLinearColor GetAccentControlColor() const;
	FLinearColor GetAccentBorderColor() const;
	FLinearColor GetAccentButtonColor() const;
	FLinearColor GetAccentButtonTextColor() const;
	FLinearColor GetReadableAccentTextColor() const;
	FLinearColor GetPanelSubduedTextColor() const;
	FLinearColor GetEffectiveAccentColor() const;
	static FLinearColor ResolveAccentColor(const FLinearColor& Color);
	static FLinearColor GetPanelBackgroundColor();
	static FLinearColor GetPanelSurfaceColor();
	static FLinearColor GetPanelBorderColor();
	static FLinearColor GetPanelTextColor();
	static FLinearColor GetPanelMutedTextColor();
	bool IsSettingsColorSelected(const FLinearColor& Color) const;
	TSharedRef<SWidget> BuildColorPresetButton(const FLinearColor& Color, const FText& Tooltip);
	FText GetCliTitle(ECliTool Tool) const;
	FText GetCliDescription(ECliTool Tool) const;
	FString GetCliCommandName(ECliTool Tool) const;
	FString GetCliConfiguredPath(ECliTool Tool) const;
	FString GetCliDetectedPath(ECliTool Tool) const;
	FString GetCliEffectivePath(ECliTool Tool) const;
	bool IsCliAvailable(ECliTool Tool) const;
	FText GetCliPathSummary(ECliTool Tool) const;
	void RefreshCliDetections();
	void SetCliConfiguredPath(ECliTool Tool, const FString& NewPath);
	FReply OnDetectCliClicked(ECliTool Tool);
	FReply OnClearCliClicked(ECliTool Tool);
	FReply OnDownloadCliClicked(ECliTool Tool);
	void LoadSettings();
	void ApplySettingsColor(const FLinearColor& NewColor);
	void SaveSettings() const;
	FText GetSettingsColorText() const;
	void OpenSettingsColorPicker();
	void HandleSettingsColorChanged(FLinearColor NewColor);
	FReply OnSettingsColorBlockClicked(const FGeometry& Geometry, const FPointerEvent& MouseEvent);
	FReply OnPickSettingsColorClicked();
	FReply OnResetSettingsColorClicked();
	FReply OnSettingsBackClicked();
	void SetDetail(const FText& Title, const FString& Text);
	void SetLastAction(const FText& Text);
	void RebuildConversationMessages();
	int32 AddConversationMessage(EConversationMessageRole Role, const FString& Text, bool bStreaming = false);
	int32 AddConversationMessageToConversation(int32 ConversationIndex, EConversationMessageRole Role, const FString& Text, bool bStreaming = false);
	static FString TrimConversationEventText(const FString& Text);
	bool TryExtractConversationEvent(const FString& Text, EConversationMessageRole& OutRole, FString& OutText) const;
	FString BuildPromptWithConversationMemory(const FString& UserMessage) const;
	FString BuildPromptWithConversationMemoryForConversation(int32 ConversationIndex, const FString& UserMessage) const;
	void StartConversationTurn(const FString& UserMessage);
	bool GetConversationStateForUpdate(int32 ConversationIndex, TArray<FConversationMessage>*& OutMessages, FString*& OutTranscript, int32*& OutAssistantMessageIndex, int32*& OutThoughtMessageIndex);
	void SaveConversationIndex(int32 ConversationIndex);

	// Per-conversation identity / runtime plumbing.
	int64 GetActiveConversationId() const;
	int64 EnsureConversationId(int32 ConversationIndex);
	int32 FindConversationIndexById(int64 ConvId) const;
	FConversationRuntime& GetRuntime(int64 ConvId);
	const FConversationRuntime* FindRuntime(int64 ConvId) const;
	const FConversationRuntime* FindActiveRuntime() const;
	// Lazily creates the conversation's own ACP client (adapter process + session) and binds
	// its delegates with the ConvId payload. Returns null only if ConvId is unresolved.
	FWorldDataCodexACPClient* EnsureClientForConversation(int64 ConvId);
	bool IsConversationBusy(int64 ConvId) const;
	bool ActiveHasPendingPermission() const;
	bool ActiveFocusAwaitingConfirmation() const;
	void StopAllClients();
	// Propagates the global mode/agent/model selection to every live conversation client.
	void ApplyAgentSettingsToAllClients();
	void AppendAssistantText(const FString& Text);
	void AppendAssistantTextToConversation(int32 ConversationIndex, const FString& Text);
	void AppendThoughtText(const FString& Text);
	void AppendThoughtTextToConversation(int32 ConversationIndex, const FString& Text);
	void AppendConversationEvent(EConversationMessageRole Role, const FString& Text);
	void AppendConversationEventToConversation(int32 ConversationIndex, EConversationMessageRole Role, const FString& Text);
	void AppendConversationText(const FString& Text);
	void AppendConversationTextToConversation(int32 ConversationIndex, const FString& Text);
	void RefreshConversationText();
	// ACP callbacks carry the owning conversation's stable Id (payload-bound at BindSP time)
	// so a background conversation's stream/turn/permission lands in its own runtime no matter
	// which conversation is currently displayed.
	void HandleAcpText(const FString& Text, int64 ConvId);
	void HandleAcpThought(const FString& Text, int64 ConvId);
	void HandleAcpPermission(const FWorldDataAcpPermissionRequest& Request, int64 ConvId);
	void HandleAcpStatus(const FString& Text, int64 ConvId);
	void HandleAcpTurnComplete(int64 ConvId);
	void HandleAcpError(const FString& Text, int64 ConvId);
	FReply OnAllowPermissionClicked();
	FReply OnDenyPermissionClicked();
	FReply ResolvePendingPermission(bool bAllow);
	void ClearPendingPermission(int64 ConvId);
	static FString GetConversationStoragePath();
	void LoadConversations();
	void SaveConversations() const;
	void ResetConversationView();
	void SaveActiveConversation();
	void LoadConversation(int32 Index);
	FReply OnSelectConversation(int32 Index);
	FText GetConversationTitle(int32 Index) const;
	FText MakeConversationTitle(const FString& Message) const;
	FText GetConversationAgeText(FDateTime CreatedAt) const;
	TSharedRef<SWidget> BuildConversationEntry(int32 Index);
	void RebuildSidebar();
	void ShowProjectInfo();
	FReply OnNewConversationClicked();
	FReply OnSettingsClicked();
	FReply OnSendClicked();
	FReply OnFocusContinueClicked();
	FReply OnFocusExecuteClicked();
	FReply OnFocusReplanClicked();
	FReply OnFocusCancelClicked();
	FReply OnStartClicked();
	FReply OnStopClicked();
	FReply OnRefreshClicked();
	FReply OnStatusClicked();
	FReply OnProjectInfoClicked();
	FReply OnBootstrapClicked();
	FReply OnPolicyClicked();
	FReply OnToolsClicked();
	FReply OnResourcesClicked();
	FReply OnCopyUrlClicked();
	FReply OnCopyConfigClicked();
	FReply OnCopyCurrentClicked();
	FReply OnAttachFileClicked();
	FReply OnRemoveAttachmentClicked(int32 AttachmentIndex);
	FReply OnComposerKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	FReply OnComposerDragOver(const FGeometry& Geometry, const FDragDropEvent& DragDropEvent);
	FReply OnComposerDrop(const FGeometry& Geometry, const FDragDropEvent& DragDropEvent);
	FReply OnOpenProjectFolderClicked();
	FReply OnOpenSavedFolderClicked();

	FText LastAction;
	FString CurrentDetailText;
	FString ConversationTranscript;
	TArray<FConversationMessage> ConversationMessages;
	FLinearColor SettingsColor = FLinearColor::White;
	FString CodexCliPath;
	FString CursorCliPath;
	FString ClaudeCliPath;
	FString DetectedCodexCliPath;
	FString DetectedCursorCliPath;
	FString DetectedClaudeCliPath;
	FEditableTextBoxStyle LightTextBoxStyle;
	FButtonStyle ComposerButtonStyle;
	FComboButtonStyle ComposerComboButtonStyle;
	EWorldDataCodexPermissionMode CurrentMode = EWorldDataCodexPermissionMode::Default;
	EWorldDataAcpAgent CurrentAcpAgent = EWorldDataAcpAgent::Codex;
	// Active CLI model id for the current agent; empty = adapter default.
	FString CurrentModel;
	FString CurrentCodexModel;
	FString CurrentCursorModel;
	FString CurrentClaudeModel;
	// One ACP client + focus/permission state per conversation, keyed by FConversation::Id.
	// Lazily populated on first send and never serialized. Replaces the former single shared
	// AcpClient so conversations are isolated and can run concurrently.
	TMap<int64, FConversationRuntime> ConversationRuntimes;
	// Monotonic source of stable conversation Ids (see FConversation::Id).
	int64 NextConversationId = 1;
	bool bShowDetail = false;
	bool bShowSettings = false;
	bool bDetailIsConversation = false;
	// Streaming cursor for the ACTIVE conversation (mirrored to/from FConversation on switch;
	// background conversations stream via their own FConversation indices — see
	// GetConversationStateForUpdate).
	int32 ActiveAssistantMessageIndex = INDEX_NONE;
	int32 ActiveThoughtMessageIndex = INDEX_NONE;
	TSharedPtr<SWidgetSwitcher> ContentSwitcher;
	TSharedPtr<SScrollBox> ConversationScrollBox;
	TSharedPtr<SScrollBox> SidebarListScrollBox;
	TArray<FConversation> Conversations;
	TArray<FComposerAttachment> ComposerAttachments;
	FString ComposerDraftText;
	int32 ActiveConversationIndex = INDEX_NONE;
	FString ServerPortText;
	TArray<FString> CachedToolNames;
	TSharedPtr<STextBlock> DetailTitleText;
	TSharedPtr<SMultiLineEditableTextBox> DetailTextBox;
	TSharedPtr<SMultiLineEditableTextBox> ComposerTextBox;
	TSharedPtr<SWrapBox> ComposerAttachmentWrapBox;
	bool bStartNewConversationOnConstruct = false;
};
