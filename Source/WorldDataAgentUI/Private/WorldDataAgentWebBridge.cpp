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

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetBoolField(TEXT("configuring"), bConfiguring);
	Root->SetBoolField(TEXT("busy"), bBusy);
	Root->SetStringField(TEXT("activeThreadId"), ActiveThreadId);
	Root->SetStringField(TEXT("selectedModel"), SelectedModel);

	TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
	Connection->SetStringField(TEXT("state"), ConnectionStateName(Status.State));
	Connection->SetStringField(TEXT("statusText"), Status.StatusText);
	Connection->SetBoolField(TEXT("authenticated"), Status.bAuthenticated);
	Connection->SetBoolField(TEXT("mcpConnected"), Status.bMcpConnected);
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

	TArray<TSharedPtr<FJsonValue>> ThreadValues;
	for (const FWorldDataThreadSummary& Thread : Threads) ThreadValues.Add(MakeShared<FJsonValueObject>(ThreadJson(Thread)));
	Root->SetArrayField(TEXT("threads"), ThreadValues);
	TArray<TSharedPtr<FJsonValue>> ConversationValues;
	for (const FWorldDataConversationItem& Item : ConversationItems) ConversationValues.Add(MakeShared<FJsonValueObject>(ConversationItemJson(Item)));
	Root->SetArrayField(TEXT("conversation"), ConversationValues);

	TSharedRef<FJsonObject> Approval = MakeShared<FJsonObject>();
	Approval->SetBoolField(TEXT("pending"), PendingApproval.Type == EWorldDataAgentEventType::ApprovalRequested);
	Approval->SetStringField(TEXT("requestId"), PendingApproval.RequestId);
	Approval->SetStringField(TEXT("threadId"), PendingApproval.ThreadId);
	Approval->SetStringField(TEXT("turnId"), PendingApproval.TurnId);
	Approval->SetStringField(TEXT("text"), PendingApproval.Text);
	Approval->SetStringField(TEXT("toolName"), PendingApproval.ToolName);
	Root->SetObjectField(TEXT("approval"), Approval);

	TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	const FString ErrorCode = !UiErrorCode.IsEmpty() ? UiErrorCode : Status.Error.Code;
	const FString ErrorMessage = !UiErrorMessage.IsEmpty() ? UiErrorMessage : Status.Error.Message;
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
	ActiveThreadId.Empty();
	ConversationItems.Empty();
	PendingFirstMessage.Empty();
	bBusy = false;
	UiErrorCode.Empty();
	UiErrorMessage.Empty();
}

void UWorldDataAgentWebBridge::ResumeThread(const FString& ThreadId, const FString& Model)
{
	if (ThreadId.IsEmpty() || bBusy) return;
	SelectedModel = Model;
	bBusy = true;
	FWorldDataResumeThreadRequest Request;
	Request.ThreadId = ThreadId;
	Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Request.Model = Model;
	Request.ApprovalPolicy = TEXT("on-request");
	Request.SandboxMode = TEXT("workspace-write");
	if (IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().ResumeThread(Request).IsEmpty()) bBusy = false;
}

void UWorldDataAgentWebBridge::SendMessage(const FString& ThreadId, const FString& Text, const FString& Model)
{
	FString Message = Text;
	Message.TrimStartAndEndInline();
	if (Message.IsEmpty() || bBusy) return;
	SelectedModel = Model;
	UiErrorCode.Empty();
	UiErrorMessage.Empty();
	if (!ThreadId.IsEmpty() && ThreadId != ActiveThreadId)
	{
		SetUiError(TEXT("thread.not_resumed"), TEXT("Resume the selected conversation before sending a message."));
		return;
	}

	FWorldDataConversationItem UserItem;
	UserItem.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	UserItem.Kind = TEXT("message");
	UserItem.Role = TEXT("user");
	UserItem.Text = Message;
	ConversationItems.Add(MoveTemp(UserItem));
	bBusy = true;
	if (ActiveThreadId.IsEmpty())
	{
		PendingFirstMessage = Message;
		StartThreadForPendingMessage();
		return;
	}
	SendTurn(Message);
}

void UWorldDataAgentWebBridge::ResolveApproval(const FString& RequestId, const bool bApproved)
{
	if (PendingApproval.Type != EWorldDataAgentEventType::ApprovalRequested || PendingApproval.RequestId != RequestId) return;
	FWorldDataApprovalDecision Decision;
	Decision.RequestId = PendingApproval.RequestId;
	Decision.ThreadId = PendingApproval.ThreadId;
	Decision.TurnId = PendingApproval.TurnId;
	Decision.bApproved = bApproved;
	IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().ResolveApproval(Decision);
	PendingApproval = FWorldDataAgentEvent();
}

void UWorldDataAgentWebBridge::HandleAgentEvent(const FWorldDataAgentEvent& Event)
{
	switch (Event.Type)
	{
	case EWorldDataAgentEventType::ConnectionChanged:
		if (Event.Status.State == EWorldDataAgentConnectionState::Ready && Threads.IsEmpty()) RefreshThreads();
		break;
	case EWorldDataAgentEventType::ThreadsListed:
		Threads = Event.Threads;
		break;
	case EWorldDataAgentEventType::ThreadCreated:
		ActiveThreadId = Event.ThreadId;
		if (!PendingFirstMessage.IsEmpty())
		{
			const FString Message = MoveTemp(PendingFirstMessage);
			PendingFirstMessage.Empty();
			SendTurn(Message);
		}
		break;
	case EWorldDataAgentEventType::ThreadLoaded:
		ActiveThreadId = Event.ThreadId;
		ConversationItems = Event.ConversationItems;
		bBusy = false;
		break;
	case EWorldDataAgentEventType::TurnStarted:
		bBusy = true;
		break;
	case EWorldDataAgentEventType::MessageDelta:
		if (ConversationItems.IsEmpty() || ConversationItems.Last().Role != TEXT("assistant") || ConversationItems.Last().TurnId != Event.TurnId)
		{
			FWorldDataConversationItem Item;
			Item.Id = Event.ItemId;
			Item.TurnId = Event.TurnId;
			Item.Kind = TEXT("message");
			Item.Role = TEXT("assistant");
			ConversationItems.Add(MoveTemp(Item));
		}
		ConversationItems.Last().Text += Event.Text;
		break;
	case EWorldDataAgentEventType::ToolStarted:
	case EWorldDataAgentEventType::ToolCompleted:
	{
		FWorldDataConversationItem* Existing = ConversationItems.FindByPredicate([&Event](const FWorldDataConversationItem& Item)
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
			ConversationItems.Add(MoveTemp(Item));
			Existing = &ConversationItems.Last();
		}
		Existing->Status = Event.Type == EWorldDataAgentEventType::ToolCompleted ? TEXT("completed") : TEXT("running");
		Existing->Text = Existing->Status;
		break;
	}
	case EWorldDataAgentEventType::ApprovalRequested:
		PendingApproval = Event;
		break;
	case EWorldDataAgentEventType::TurnCompleted:
		bBusy = false;
		RefreshThreads();
		break;
	case EWorldDataAgentEventType::TurnFailed:
		bBusy = false;
		SetUiError(Event.Error.Code, Event.Error.Message);
		break;
	default:
		break;
	}
}

void UWorldDataAgentWebBridge::StartThreadForPendingMessage()
{
	FWorldDataCreateThreadRequest Request;
	Request.ClientConversationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	Request.Model = SelectedModel;
	Request.ApprovalPolicy = TEXT("on-request");
	Request.SandboxMode = TEXT("workspace-write");
	if (IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().CreateThread(Request).IsEmpty()) bBusy = false;
}

void UWorldDataAgentWebBridge::SendTurn(const FString& Text)
{
	FWorldDataSendTurnRequest Request;
	Request.ThreadId = ActiveThreadId;
	Request.ClientTurnId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Request.Text = Text;
	if (IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetGateway().SendTurn(Request).IsEmpty()) bBusy = false;
}

void UWorldDataAgentWebBridge::SetUiError(const FString& Code, const FString& Message)
{
	UiErrorCode = Code;
	UiErrorMessage = Message;
}
