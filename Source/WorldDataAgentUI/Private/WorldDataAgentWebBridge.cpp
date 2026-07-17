#include "WorldDataAgentWebBridge.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "IWorldDataAgentBootstrapModule.h"
#include "IWorldDataAgentGateway.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "WorldDataMCPToolRegistry.h"

#include <UEBridgeMCPCoreModule.h>

namespace
{
	FString ConnectionStateName(const EWorldDataAgentConnectionState State)
	{
		switch (State)
		{
		case EWorldDataAgentConnectionState::NotInstalled: return TEXT("notInstalled");
		case EWorldDataAgentConnectionState::Installing: return TEXT("installing");
		case EWorldDataAgentConnectionState::StartingMcp: return TEXT("startingMcp");
		case EWorldDataAgentConnectionState::StartingAgentHost: return TEXT("startingHost");
		case EWorldDataAgentConnectionState::StartingCodex: return TEXT("startingCodex");
		case EWorldDataAgentConnectionState::Handshaking: return TEXT("handshaking");
		case EWorldDataAgentConnectionState::CheckingAuth: return TEXT("checkingAuth");
		case EWorldDataAgentConnectionState::CheckingMcp: return TEXT("checkingMcp");
		case EWorldDataAgentConnectionState::Ready: return TEXT("ready");
		case EWorldDataAgentConnectionState::Busy: return TEXT("busy");
		case EWorldDataAgentConnectionState::Degraded: return TEXT("degraded");
		case EWorldDataAgentConnectionState::Fatal: return TEXT("fatal");
		default: return TEXT("unknown");
		}
	}

	TSharedRef<FJsonObject> ThreadJson(const FWorldDataThreadSummary& Thread)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Thread.Id);
		Json->SetStringField(TEXT("title"), Thread.Title);
		Json->SetStringField(TEXT("preview"), Thread.Preview);
		Json->SetStringField(TEXT("workingDirectory"), Thread.WorkingDirectory);
		Json->SetNumberField(TEXT("createdAt"), static_cast<double>(Thread.CreatedAt));
		Json->SetNumberField(TEXT("updatedAt"), static_cast<double>(Thread.UpdatedAt));
		Json->SetNumberField(TEXT("recencyAt"), static_cast<double>(Thread.RecencyAt));
		Json->SetStringField(TEXT("threadSource"), Thread.ThreadSource);
		Json->SetStringField(TEXT("status"), Thread.Status);
		return Json;
	}

	TSharedRef<FJsonObject> ConversationItemJson(const FWorldDataConversationItem& Item)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("id"), Item.Id);
		Json->SetStringField(TEXT("turnId"), Item.TurnId);
		Json->SetStringField(TEXT("kind"), Item.Kind);
		Json->SetStringField(TEXT("role"), Item.Role);
		Json->SetStringField(TEXT("text"), Item.Text);
		Json->SetStringField(TEXT("toolName"), Item.ToolName);
		Json->SetStringField(TEXT("status"), Item.Status);
		return Json;
	}

	TSharedRef<FJsonObject> ToolJson(const WorldDataMCP::FToolMetadata& Tool)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Tool.Name);
		Json->SetStringField(TEXT("provider"), Tool.ProviderName);
		Json->SetStringField(TEXT("risk"), WorldDataMCP::GetToolRiskName(Tool.Risk));
		Json->SetBoolField(TEXT("requiresApproval"), Tool.bRequiresInteractiveApproval);
		Json->SetBoolField(TEXT("audited"), Tool.bAudited);
		Json->SetStringField(TEXT("revisionPolicy"), WorldDataMCP::GetToolRevisionPolicyName(Tool.RevisionPolicy));

		TArray<TSharedPtr<FJsonValue>> Capabilities;
		for (const FString& Capability : Tool.RequiredCapabilities)
		{
			Capabilities.Add(MakeShared<FJsonValueString>(Capability));
		}
		Json->SetArrayField(TEXT("requiredCapabilities"), Capabilities);
		return Json;
	}
}

void UWorldDataAgentWebBridge::Initialize()
{
	IWorldDataAgentGateway& Gateway = IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway();
	EventHandle = Gateway.OnEvent().AddUObject(this, &UWorldDataAgentWebBridge::HandleAgentEvent);
	if (Gateway.GetStatus().State == EWorldDataAgentConnectionState::Ready)
	{
		RefreshThreads();
	}
}

void UWorldDataAgentWebBridge::Shutdown()
{
	if (!EventHandle.IsValid()) return;
	IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().OnEvent().Remove(EventHandle);
	EventHandle.Reset();
}

FString UWorldDataAgentWebBridge::GetState()
{
	IWorldDataAgentSubsystem& Subsystem = IWorldDataAgentBootstrapModule::Get().GetSubsystem();
	const FWorldDataAgentStatusSnapshot Status = Subsystem.GetGateway().GetStatus();
	const FWorldDataAgentRuntimeStatus Runtime = Subsystem.GetRuntimeStatus();
	const FConversationSession* ActiveSession = FindSession(ActiveThreadId);
	const int32 RegisteredToolCount = WorldDataMCP::FToolRegistry::Get().GetRegisteredToolMetadata().Num();

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 2);
	Root->SetBoolField(TEXT("configuring"), bConfiguring);
	Root->SetBoolField(TEXT("busy"), ActiveSession && ActiveSession->IsBusy());
	Root->SetStringField(TEXT("activeThreadId"), ActiveThreadId);
	Root->SetStringField(TEXT("selectedModel"), SelectedModel);

	TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
	Connection->SetStringField(TEXT("state"), ConnectionStateName(Status.State));
	Connection->SetStringField(TEXT("statusText"), Status.StatusText);
	Connection->SetBoolField(TEXT("authenticated"), Status.bAuthenticated);
	Connection->SetBoolField(TEXT("mcpConnected"), Status.bMcpConnected);
	Connection->SetNumberField(TEXT("mcpToolCount"), Status.bMcpConnected ? RegisteredToolCount : Status.McpToolCount);
	Connection->SetStringField(TEXT("mcpStatus"), Status.McpStatus);
	Connection->SetStringField(TEXT("hostVersion"), Status.AgentHostVersion);
	Connection->SetStringField(TEXT("codexVersion"), Status.CodexVersion);
	TArray<TSharedPtr<FJsonValue>> Models;
	for (const FWorldDataAgentModel& Model : Status.Models)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("id"), Model.Id);
		Item->SetStringField(TEXT("displayName"), Model.DisplayName);
		Item->SetStringField(TEXT("description"), Model.Description);
		Item->SetBoolField(TEXT("isDefault"), Model.bDefault);
		Models.Add(MakeShared<FJsonValueObject>(Item));
	}
	Connection->SetArrayField(TEXT("models"), Models);
	Root->SetObjectField(TEXT("connection"), Connection);

	TSharedRef<FJsonObject> RuntimeJson = MakeShared<FJsonObject>();
	RuntimeJson->SetBoolField(TEXT("configured"), Runtime.bConfigured);
	RuntimeJson->SetBoolField(TEXT("verified"), Runtime.bVerified);
	RuntimeJson->SetStringField(TEXT("hostVersion"), Runtime.AgentHostVersion);
	RuntimeJson->SetStringField(TEXT("codexVersion"), Runtime.CodexVersion);
	RuntimeJson->SetStringField(TEXT("manifestPath"), Runtime.ManifestPath);
	RuntimeJson->SetStringField(TEXT("hostSha256"), Runtime.AgentHostSha256);
	RuntimeJson->SetStringField(TEXT("codexSha256"), Runtime.CodexSha256);
	Root->SetObjectField(TEXT("runtime"), RuntimeJson);

	TArray<TSharedPtr<FJsonValue>> ToolValues;
	for (const WorldDataMCP::FToolMetadata& Tool : WorldDataMCP::FToolRegistry::Get().GetRegisteredToolMetadata())
	{
		ToolValues.Add(MakeShared<FJsonValueObject>(ToolJson(Tool)));
	}
	Root->SetArrayField(TEXT("tools"), ToolValues);

	TArray<TSharedPtr<FJsonValue>> ThreadValues;
	for (const FWorldDataThreadSummary& Thread : Threads)
	{
		FWorldDataThreadSummary PresentedThread = Thread;
		if (const FConversationSession* Session = FindSession(Thread.Id))
		{
			if (Session->PendingApproval.Type == EWorldDataAgentEventType::ApprovalRequested) PresentedThread.Status = TEXT("approval");
			else if (Session->bCreating) PresentedThread.Status = TEXT("creating");
			else if (Session->bLoading) PresentedThread.Status = TEXT("loading");
			else if (Session->bTurnActive) PresentedThread.Status = TEXT("running");
		}
		ThreadValues.Add(MakeShared<FJsonValueObject>(ThreadJson(PresentedThread)));
	}
	Root->SetArrayField(TEXT("threads"), ThreadValues);
	TArray<TSharedPtr<FJsonValue>> ConversationValues;
	if (ActiveSession)
	{
		for (const FWorldDataConversationItem& Item : ActiveSession->Items)
		{
			ConversationValues.Add(MakeShared<FJsonValueObject>(ConversationItemJson(Item)));
		}
	}
	Root->SetArrayField(TEXT("conversation"), ConversationValues);

	TSharedRef<FJsonObject> Approval = MakeShared<FJsonObject>();
	const TArray<FWorldDataMCPApprovalSummary> McpApprovals =
		IUEBridgeMCPCoreModule::Get().GetService().GetPendingApprovals();
	const FWorldDataMCPApprovalSummary* McpApproval = McpApprovals.FindByPredicate(
		[](const FWorldDataMCPApprovalSummary& Candidate)
		{
			return Candidate.bReadyForDecision;
		});
	if (McpApproval)
	{
		// Prefix Core approval ids so ResolveApproval can distinguish them from
		// Agent Host approval request ids without changing the web UI contract.
		Approval->SetBoolField(TEXT("pending"), true);
		Approval->SetStringField(TEXT("requestId"), TEXT("mcp:") + McpApproval->ApprovalId);
		Approval->SetStringField(TEXT("threadId"), FString());
		Approval->SetStringField(TEXT("turnId"), FString());
		Approval->SetStringField(TEXT("text"), FString::Printf(
			TEXT("Risk: %s\nTarget: %s\nApproval: %s\nExpires: %s"),
			*McpApproval->Risk,
			*McpApproval->TargetSummary,
			*McpApproval->ApprovalId,
			*McpApproval->ExpiresAtUtc.ToIso8601()));
		Approval->SetStringField(TEXT("toolName"), McpApproval->ToolName);
	}
	else
	{
		const FWorldDataAgentEvent EmptyApproval;
		const FWorldDataAgentEvent& PendingApproval = ActiveSession ? ActiveSession->PendingApproval : EmptyApproval;
		Approval->SetBoolField(TEXT("pending"), PendingApproval.Type == EWorldDataAgentEventType::ApprovalRequested);
		Approval->SetStringField(TEXT("requestId"), PendingApproval.RequestId);
		Approval->SetStringField(TEXT("threadId"), PendingApproval.ThreadId);
		Approval->SetStringField(TEXT("turnId"), PendingApproval.TurnId);
		Approval->SetStringField(TEXT("text"), PendingApproval.Text);
		Approval->SetStringField(TEXT("toolName"), PendingApproval.ToolName);
	}
	Root->SetObjectField(TEXT("approval"), Approval);

	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	const FString ErrorCode = ActiveSession && !ActiveSession->ErrorCode.IsEmpty()
		? ActiveSession->ErrorCode
		: (!UiErrorCode.IsEmpty() ? UiErrorCode : Status.Error.Code);
	const FString ErrorMessage = ActiveSession && !ActiveSession->ErrorMessage.IsEmpty()
		? ActiveSession->ErrorMessage
		: (!UiErrorMessage.IsEmpty() ? UiErrorMessage : Status.Error.Message);
	Error->SetBoolField(TEXT("present"), !ErrorCode.IsEmpty() || !ErrorMessage.IsEmpty());
	Error->SetStringField(TEXT("code"), ErrorCode);
	Error->SetStringField(TEXT("message"), ErrorMessage);
	Root->SetObjectField(TEXT("error"), Error);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	return Json;
}

void UWorldDataAgentWebBridge::ConfigureRuntime()
{
	if (bConfiguring) return;
	bConfiguring = true;
	UiErrorCode.Empty();
	UiErrorMessage.Empty();
	const TWeakObjectPtr<UWorldDataAgentWebBridge> WeakThis(this);
	Async(EAsyncExecution::ThreadPool, [WeakThis]()
	{
		FString Error;
		const bool bSuccess = IWorldDataAgentBootstrapModule::Get().GetSubsystem().ConfigureRuntime(Error);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, bSuccess, Error]()
		{
			if (!WeakThis.IsValid()) return;
			WeakThis->bConfiguring = false;
			if (!bSuccess) WeakThis->SetUiError(TEXT("runtime.configure_failed"), Error);
		});
	});
}

void UWorldDataAgentWebBridge::RefreshThreads()
{
	FWorldDataListThreadsRequest Request;
	Request.Limit = 50;
	IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().ListThreads(Request);
}

void UWorldDataAgentWebBridge::NewConversation()
{
	UiErrorCode.Empty();
	UiErrorMessage.Empty();
	const FString DraftId = AddDraftConversation();
	StartThread(DraftId);
}

void UWorldDataAgentWebBridge::ResumeThread(const FString& ThreadId, const FString& Model)
{
	if (ThreadId.IsEmpty()) return;
	SelectedModel = Model;
	UiErrorCode.Empty();
	UiErrorMessage.Empty();
	ActiveThreadId = ThreadId;
	FConversationSession& Session = GetOrAddSession(ThreadId);
	if (Session.bDraft || Session.bLoaded || Session.bLoading) return;

	Session.bLoading = true;
	Session.ErrorCode.Empty();
	Session.ErrorMessage.Empty();
	FWorldDataResumeThreadRequest Request;
	Request.ThreadId = ThreadId;
	Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Request.Model = Model;
	Request.ApprovalPolicy = TEXT("on-request");
	Request.SandboxMode = TEXT("workspace-write");
	const FString RequestId = IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().ResumeThread(Request);
	if (RequestId.IsEmpty())
	{
		Session.bLoading = false;
		Session.ErrorCode = TEXT("thread.resume_not_started");
		Session.ErrorMessage = TEXT("The conversation could not be loaded.");
	}
	else
	{
		PendingRequestThreadIds.Add(RequestId, ThreadId);
	}
}

bool UWorldDataAgentWebBridge::SendMessage(const FString& ThreadId, const FString& Text, const FString& Model)
{
	FString Message = Text;
	Message.TrimStartAndEndInline();
	if (Message.IsEmpty()) return false;
	SelectedModel = Model;
	UiErrorCode.Empty();
	UiErrorMessage.Empty();
	if (!ThreadId.IsEmpty() && ThreadId != ActiveThreadId)
	{
		SetUiError(TEXT("thread.not_resumed"), TEXT("Resume the selected conversation before sending a message."));
		return false;
	}
	if (ActiveThreadId.IsEmpty())
	{
		ActiveThreadId = AddDraftConversation();
	}
	FConversationSession& Session = GetOrAddSession(ActiveThreadId);
	if (Session.IsBusy())
	{
		SetUiError(TEXT("thread.busy"), TEXT("Wait for the current conversation operation to finish before sending another message."));
		return false;
	}
	if (!Session.bDraft && !IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().GetStatus().bMcpConnected)
	{
		SetUiError(TEXT("mcp.not_ready"), TEXT("WorldData MCP is not ready for this thread yet."));
		return false;
	}
	Session.ErrorCode.Empty();
	Session.ErrorMessage.Empty();

	FWorldDataConversationItem UserItem;
	UserItem.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	UserItem.Kind = TEXT("message");
	UserItem.Role = TEXT("user");
	UserItem.Text = Message;
	const int32 UserItemIndex = Session.Items.Add(MoveTemp(UserItem));
	if (Session.bDraft)
	{
		Session.PendingFirstMessage = Message;
		if (!Session.bCreating && !StartThread(ActiveThreadId))
		{
			Session.PendingFirstMessage.Empty();
			Session.Items.RemoveAt(UserItemIndex);
			return false;
		}
		return true;
	}
	if (!SendTurn(ActiveThreadId, Message))
	{
		Session.Items.RemoveAt(UserItemIndex);
		return false;
	}
	return true;
}

void UWorldDataAgentWebBridge::ResolveApproval(const FString& RequestId, const bool bApproved)
{
	static const FString McpApprovalPrefix(TEXT("mcp:"));
	if (RequestId.StartsWith(McpApprovalPrefix))
	{
		const FString ApprovalId = RequestId.RightChop(McpApprovalPrefix.Len());
		FString Error;
		if (!IUEBridgeMCPCoreModule::Get().GetService().ResolvePendingApproval(ApprovalId, bApproved, Error))
		{
			SetUiError(TEXT("mcp.approval_resolution_failed"), Error);
		}
		else
		{
			UiErrorCode.Empty();
			UiErrorMessage.Empty();
		}
		return;
	}

	FConversationSession* Session = nullptr;
	for (TPair<FString, FConversationSession>& Pair : ConversationSessions)
	{
		if (Pair.Value.PendingApproval.Type == EWorldDataAgentEventType::ApprovalRequested
			&& Pair.Value.PendingApproval.RequestId == RequestId)
		{
			Session = &Pair.Value;
			break;
		}
	}
	if (!Session) return;
	FWorldDataApprovalDecision Decision;
	Decision.RequestId = Session->PendingApproval.RequestId;
	Decision.ThreadId = Session->PendingApproval.ThreadId;
	Decision.TurnId = Session->PendingApproval.TurnId;
	Decision.bApproved = bApproved;
	IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().ResolveApproval(Decision);
	Session->PendingApproval = FWorldDataAgentEvent();
}

void UWorldDataAgentWebBridge::HandleAgentEvent(const FWorldDataAgentEvent& Event)
{
	if (!Event.SessionId.IsEmpty())
	{
		if (Event.SessionId == LastHostSessionId && Event.Sequence <= LastHostSequence) return;
		LastHostSessionId = Event.SessionId;
		LastHostSequence = Event.Sequence;
	}
	switch (Event.Type)
	{
	case EWorldDataAgentEventType::ConnectionChanged:
		if (Event.Status.State == EWorldDataAgentConnectionState::Ready && Threads.IsEmpty()) RefreshThreads();
		if (Event.Status.State == EWorldDataAgentConnectionState::Degraded)
		{
			for (TPair<FString, FConversationSession>& Pair : ConversationSessions)
			{
				if (!Pair.Value.bTurnActive) continue;
				Pair.Value.bTurnActive = false;
				Pair.Value.ActiveTurnId.Empty();
				Pair.Value.ErrorCode = TEXT("turn.interrupted");
				Pair.Value.ErrorMessage = TEXT("The Agent Host restarted. The unfinished turn was not resent.");
			}
		}
		break;
	case EWorldDataAgentEventType::ThreadsListed:
	{
		TArray<FWorldDataThreadSummary> LocalThreads = MoveTemp(Threads);
		Threads = Event.Threads;
		for (FWorldDataThreadSummary& LocalThread : LocalThreads)
		{
			if (!Threads.ContainsByPredicate([&LocalThread](const FWorldDataThreadSummary& Thread)
			{
				return Thread.Id == LocalThread.Id;
			})) Threads.Insert(MoveTemp(LocalThread), 0);
		}
		break;
	}
	case EWorldDataAgentEventType::ThreadCreated:
	{
		FString DraftId;
		PendingRequestThreadIds.RemoveAndCopyValue(Event.RequestId, DraftId);
		if (DraftId.IsEmpty())
		{
			for (const TPair<FString, FConversationSession>& Pair : ConversationSessions)
			{
				if (Pair.Value.bDraft && Pair.Value.bCreating)
				{
					DraftId = Pair.Key;
					break;
				}
			}
		}
		if (DraftId.IsEmpty()) break;
		BindDraftConversation(DraftId, Event.ThreadId);
		FConversationSession* Session = FindSession(Event.ThreadId);
		if (!Session) break;
		Session->bCreating = false;
		Session->bDraft = false;
		Session->bLoaded = true;
		if (!IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().GetStatus().bMcpConnected)
		{
			Session->ErrorCode = TEXT("mcp.not_ready");
			Session->ErrorMessage = TEXT("The thread was created, but WorldData MCP is not ready yet.");
			break;
		}
		if (!Session->PendingFirstMessage.IsEmpty())
		{
			const FString Message = MoveTemp(Session->PendingFirstMessage);
			Session->PendingFirstMessage.Empty();
			SendTurn(Event.ThreadId, Message);
		}
		break;
	}
	case EWorldDataAgentEventType::ThreadLoaded:
	{
		PendingRequestThreadIds.Remove(Event.RequestId);
		FConversationSession& Session = GetOrAddSession(Event.ThreadId);
		Session.Items = Event.ConversationItems;
		Session.bDraft = false;
		Session.bLoading = false;
		Session.bLoaded = true;
		Session.ErrorCode.Empty();
		Session.ErrorMessage.Empty();
		break;
	}
	case EWorldDataAgentEventType::TurnStarted:
	{
		PendingRequestThreadIds.Remove(Event.RequestId);
		if (FConversationSession* Session = FindSession(Event.ThreadId))
		{
			Session->bTurnActive = true;
			Session->ActiveTurnId = Event.TurnId;
		}
		break;
	}
	case EWorldDataAgentEventType::MessageDelta:
	{
		FConversationSession* Session = FindSession(Event.ThreadId);
		if (!Session) break;
		if (Session->Items.IsEmpty() || Session->Items.Last().Role != TEXT("assistant") || Session->Items.Last().TurnId != Event.TurnId)
		{
			FWorldDataConversationItem Item;
			Item.Id = Event.ItemId;
			Item.TurnId = Event.TurnId;
			Item.Kind = TEXT("message");
			Item.Role = TEXT("assistant");
			Session->Items.Add(MoveTemp(Item));
		}
		Session->Items.Last().Text += Event.Text;
		break;
	}
	case EWorldDataAgentEventType::ToolStarted:
	case EWorldDataAgentEventType::ToolCompleted:
	{
		FConversationSession* Session = FindSession(Event.ThreadId);
		if (!Session) break;
		FWorldDataConversationItem* Existing = Session->Items.FindByPredicate([&Event](const FWorldDataConversationItem& Item)
		{
			return !Event.ItemId.IsEmpty() && Item.Id == Event.ItemId;
		});
		if (!Existing)
		{
			FWorldDataConversationItem Item;
			Item.Id = Event.ItemId;
			Item.TurnId = Event.TurnId;
			Item.Kind = TEXT("tool");
			Item.Role = TEXT("tool");
			Item.ToolName = Event.ToolName;
			Session->Items.Add(MoveTemp(Item));
			Existing = &Session->Items.Last();
		}
		if (!Event.ToolName.IsEmpty()) Existing->ToolName = Event.ToolName;
		if (!Event.Text.IsEmpty()) Existing->Text = Event.Text;
		Existing->Status = Event.Type == EWorldDataAgentEventType::ToolCompleted ? TEXT("completed") : TEXT("running");
		break;
	}
	case EWorldDataAgentEventType::ReasoningStarted:
	case EWorldDataAgentEventType::ReasoningCompleted:
	{
		FConversationSession* Session = FindSession(Event.ThreadId);
		if (!Session) break;
		FWorldDataConversationItem* Existing = Session->Items.FindByPredicate([&Event](const FWorldDataConversationItem& Item)
		{
			return !Event.ItemId.IsEmpty() && Item.Id == Event.ItemId;
		});
		if (!Existing)
		{
			FWorldDataConversationItem Item;
			Item.Id = Event.ItemId;
			Item.TurnId = Event.TurnId;
			Item.Kind = TEXT("activity");
			Item.Role = TEXT("assistant");
			Item.ToolName = TEXT("reasoning");
			Item.Text = TEXT("正在处理");
			Session->Items.Add(MoveTemp(Item));
			Existing = &Session->Items.Last();
		}
		Existing->Status = Event.Type == EWorldDataAgentEventType::ReasoningCompleted ? TEXT("completed") : TEXT("running");
		break;
	}
	case EWorldDataAgentEventType::McpStatusChanged:
		if (!Event.ThreadId.IsEmpty() && !IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().GetStatus().bMcpConnected)
		{
			if (FConversationSession* Session = FindSession(Event.ThreadId))
			{
				Session->ErrorCode = TEXT("mcp.not_ready");
				Session->ErrorMessage = TEXT("WorldData MCP is not ready for this thread.");
			}
		}
		break;
	case EWorldDataAgentEventType::ApprovalRequested:
		if (FConversationSession* Session = FindSession(Event.ThreadId)) Session->PendingApproval = Event;
		break;
	case EWorldDataAgentEventType::TurnCompleted:
		if (FConversationSession* Session = FindSession(Event.ThreadId))
		{
			Session->bTurnActive = false;
			Session->ActiveTurnId.Empty();
			Session->PendingApproval = FWorldDataAgentEvent();
		}
		RefreshThreads();
		break;
	case EWorldDataAgentEventType::TurnFailed:
	{
		FConversationSession* Session = FindSessionForEvent(Event);
		if (Session)
		{
			Session->bCreating = false;
			Session->bLoading = false;
			Session->bTurnActive = false;
			Session->ActiveTurnId.Empty();
			Session->ErrorCode = Event.Error.Code;
			Session->ErrorMessage = Event.Error.Message;
		}
		else
		{
			SetUiError(Event.Error.Code, Event.Error.Message);
		}
		break;
	}
	default:
		break;
	}
}

FString UWorldDataAgentWebBridge::AddDraftConversation()
{
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	FWorldDataThreadSummary Draft;
	Draft.Id = FString::Printf(TEXT("draft-%s"), *FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	Draft.Title = TEXT("\u65B0\u5BF9\u8BDD");
	Draft.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Draft.CreatedAt = Now;
	Draft.UpdatedAt = Now;
	Draft.RecencyAt = Now;
	Draft.Status = TEXT("draft");
	ActiveThreadId = Draft.Id;
	FConversationSession& Session = GetOrAddSession(Draft.Id);
	Session = FConversationSession();
	Session.bDraft = true;
	Threads.Insert(MoveTemp(Draft), 0);
	return ActiveThreadId;
}

void UWorldDataAgentWebBridge::BindDraftConversation(const FString& DraftId, const FString& ThreadId)
{
	if (FWorldDataThreadSummary* Draft = Threads.FindByPredicate([&DraftId](const FWorldDataThreadSummary& Thread)
	{
		return Thread.Id == DraftId;
	}))
	{
		Draft->Id = ThreadId;
		Draft->Status = TEXT("active");
	}

	FConversationSession Session;
	if (ConversationSessions.RemoveAndCopyValue(DraftId, Session))
	{
		Session.bDraft = false;
		ConversationSessions.Add(ThreadId, MoveTemp(Session));
	}
	else
	{
		GetOrAddSession(ThreadId).bDraft = false;
	}
	if (ActiveThreadId == DraftId) ActiveThreadId = ThreadId;
}

bool UWorldDataAgentWebBridge::StartThread(const FString& DraftId)
{
	FConversationSession* Session = FindSession(DraftId);
	if (!Session || Session->bCreating) return false;
	Session->bCreating = true;
	FWorldDataCreateThreadRequest Request;
	Request.ClientConversationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Request.Model = SelectedModel;
	Request.ApprovalPolicy = TEXT("on-request");
	Request.SandboxMode = TEXT("workspace-write");
	const FString RequestId = IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().CreateThread(Request);
	if (RequestId.IsEmpty())
	{
		Session->bCreating = false;
		Session->ErrorCode = TEXT("thread.create_not_started");
		Session->ErrorMessage = TEXT("The conversation could not be created.");
		return false;
	}
	PendingRequestThreadIds.Add(RequestId, DraftId);
	return true;
}

bool UWorldDataAgentWebBridge::SendTurn(const FString& ThreadId, const FString& Text)
{
	FConversationSession& Session = GetOrAddSession(ThreadId);
	Session.bTurnActive = true;
	FWorldDataSendTurnRequest Request;
	Request.ThreadId = ThreadId;
	Request.ClientTurnId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Request.Text = Text;
	Session.ActiveTurnId = Request.ClientTurnId;
	const FString RequestId = IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().SendTurn(Request);
	if (RequestId.IsEmpty())
	{
		Session.bTurnActive = false;
		Session.ActiveTurnId.Empty();
		Session.ErrorCode = TEXT("turn.send_not_started");
		Session.ErrorMessage = TEXT("The message could not be sent.");
		return false;
	}
	PendingRequestThreadIds.Add(RequestId, ThreadId);
	return true;
}

UWorldDataAgentWebBridge::FConversationSession& UWorldDataAgentWebBridge::GetOrAddSession(const FString& ThreadId)
{
	return ConversationSessions.FindOrAdd(ThreadId);
}

UWorldDataAgentWebBridge::FConversationSession* UWorldDataAgentWebBridge::FindSession(const FString& ThreadId)
{
	return ThreadId.IsEmpty() ? nullptr : ConversationSessions.Find(ThreadId);
}

const UWorldDataAgentWebBridge::FConversationSession* UWorldDataAgentWebBridge::FindSession(const FString& ThreadId) const
{
	return ThreadId.IsEmpty() ? nullptr : ConversationSessions.Find(ThreadId);
}

UWorldDataAgentWebBridge::FConversationSession* UWorldDataAgentWebBridge::FindSessionForEvent(const FWorldDataAgentEvent& Event)
{
	if (FConversationSession* Session = FindSession(Event.ThreadId)) return Session;
	FString ThreadId;
	if (PendingRequestThreadIds.RemoveAndCopyValue(Event.RequestId, ThreadId)) return FindSession(ThreadId);
	return nullptr;
}

void UWorldDataAgentWebBridge::SetUiError(const FString& Code, const FString& Message)
{
	UiErrorCode = Code;
	UiErrorMessage = Message;
}
