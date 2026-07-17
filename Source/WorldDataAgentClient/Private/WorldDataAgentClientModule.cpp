#include "IWorldDataAgentClientModule.h"
#include "WorldDataAgentHostProcess.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr int32 MaxHostFrameCharacters = 1024 * 1024;
	const FString AgentMcpTokenEnvironmentVariable = TEXT("WORLDDATA_MCP_TOKEN");

	FString SerializeJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Result;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result);
		FJsonSerializer::Serialize(Object, Writer);
		return Result;
	}

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Parent.IsValid() && Parent->TryGetObjectField(Name, Value) && Value && Value->IsValid() ? *Value : nullptr;
	}

	bool ParseThreadSummary(const TSharedPtr<FJsonObject>& Object, FWorldDataThreadSummary& OutThread)
	{
		if (!Object.IsValid() || !Object->TryGetStringField(TEXT("id"), OutThread.Id) || OutThread.Id.IsEmpty()) return false;
		Object->TryGetStringField(TEXT("title"), OutThread.Title);
		Object->TryGetStringField(TEXT("preview"), OutThread.Preview);
		Object->TryGetStringField(TEXT("workingDirectory"), OutThread.WorkingDirectory);
		Object->TryGetStringField(TEXT("status"), OutThread.Status);
		double CreatedAt = 0.0;
		double UpdatedAt = 0.0;
		double RecencyAt = 0.0;
		Object->TryGetNumberField(TEXT("createdAt"), CreatedAt);
		Object->TryGetNumberField(TEXT("updatedAt"), UpdatedAt);
		Object->TryGetNumberField(TEXT("recencyAt"), RecencyAt);
		OutThread.CreatedAt = static_cast<int64>(CreatedAt);
		OutThread.UpdatedAt = static_cast<int64>(UpdatedAt);
		OutThread.RecencyAt = static_cast<int64>(RecencyAt);
		return true;
	}

	void ParseConversationItems(const TSharedPtr<FJsonObject>& Payload, TArray<FWorldDataConversationItem>& OutItems)
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("items"), Items) || !Items) return;
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Object.IsValid()) continue;
			FWorldDataConversationItem Item;
			Object->TryGetStringField(TEXT("id"), Item.Id);
			Object->TryGetStringField(TEXT("turnId"), Item.TurnId);
			Object->TryGetStringField(TEXT("kind"), Item.Kind);
			Object->TryGetStringField(TEXT("role"), Item.Role);
			Object->TryGetStringField(TEXT("text"), Item.Text);
			Object->TryGetStringField(TEXT("toolName"), Item.ToolName);
			Object->TryGetStringField(TEXT("status"), Item.Status);
			OutItems.Add(MoveTemp(Item));
		}
	}

	class FWorldDataAgentGateway final
		: public IWorldDataAgentGateway
		, public TSharedFromThis<FWorldDataAgentGateway>
	{
	public:
		FWorldDataAgentGateway(IWorldDataAgentSecurity& InSecurity, IWorldDataAgentDiagnostics& InDiagnostics)
			: Security(InSecurity)
			, Diagnostics(InDiagnostics)
		{
		}

		virtual ~FWorldDataAgentGateway() override
		{
			if (Process.IsValid() && Process->IsRunning())
			{
				Process->RequestStop(true);
			}
		}

		virtual void Connect(const FWorldDataAgentConnectionOptions& Options) override
		{
			DisconnectProcessForReconnect();
			ConnectionOptions = Options;
			if (Options.ProtocolVersion != WorldDataAgentProtocol::CurrentVersion)
			{
				Fail(TEXT("host.protocol_mismatch"), TEXT("The configured Agent Host protocol version is not supported."), false);
				return;
			}
			if (Options.AgentHostExecutable.IsEmpty() || Options.CodexExecutable.IsEmpty())
			{
				Fail(TEXT("runtime.not_installed"), TEXT("Agent Host and Codex runtimes have not been configured."), true);
				return;
			}
			if (FPaths::IsRelative(Options.AgentHostExecutable) || !FPaths::FileExists(Options.AgentHostExecutable))
			{
				Fail(TEXT("agent_host.not_found"), TEXT("The configured Agent Host executable does not exist."), true);
				return;
			}
			if (FPaths::IsRelative(Options.CodexExecutable) || !FPaths::FileExists(Options.CodexExecutable))
			{
				Fail(TEXT("codex.not_found"), TEXT("The configured Codex executable does not exist."), true);
				return;
			}

			FString McpToken;
			FString SecretError;
			if (!Security.ResolveSecretForTrustedChild(Options.McpSecretHandle, McpToken, SecretError))
			{
				Fail(TEXT("mcp.secret_unavailable"), SecretError, true);
				return;
			}

			SetStatus(EWorldDataAgentConnectionState::StartingAgentHost, TEXT("Starting WorldData Agent Host."));
			const uint64 Generation = ++ProcessGeneration;
			Process = MakeShared<FWorldDataAgentHostProcess>();
			const TWeakPtr<FWorldDataAgentGateway> WeakSelf = AsShared();
			Process->OnStdout().BindLambda([WeakSelf, Generation](const TArray<uint8>& Output)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakSelf, Output, Generation]()
				{
					if (const TSharedPtr<FWorldDataAgentGateway> Self = WeakSelf.Pin(); Self.IsValid() && Self->ProcessGeneration == Generation)
					{
						Self->ConsumeOutput(Output);
					}
				});
			});
			Process->OnStderr().BindLambda([WeakSelf, Generation](const TArray<uint8>& Output)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakSelf, Output, Generation]()
				{
					if (const TSharedPtr<FWorldDataAgentGateway> Self = WeakSelf.Pin(); Self.IsValid() && Self->ProcessGeneration == Generation)
					{
						Self->ConsumeStderr(Output);
					}
				});
			});
			Process->OnCompleted().BindLambda([WeakSelf, Generation](const int32 ReturnCode, const bool bCancelled)
			{
				AsyncTask(ENamedThreads::GameThread, [WeakSelf, ReturnCode, bCancelled, Generation]()
				{
					if (const TSharedPtr<FWorldDataAgentGateway> Self = WeakSelf.Pin(); Self.IsValid() && Self->ProcessGeneration == Generation)
					{
						Self->HandleProcessCompleted(ReturnCode, bCancelled);
					}
				});
			});
			if (!Process->Launch(
				Options.AgentHostExecutable,
				Options.ProjectRoot,
				AgentMcpTokenEnvironmentVariable,
				McpToken))
			{
				Process.Reset();
				Fail(TEXT("agent_host.launch_failed"), TEXT("Could not launch WorldData Agent Host."), true);
				return;
			}

			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("codexExecutable"), Options.CodexExecutable);
			Payload->SetStringField(TEXT("projectRoot"), Options.ProjectRoot);
			Payload->SetStringField(TEXT("mcpUrl"), Options.McpUrl);
			Payload->SetStringField(TEXT("clientVersion"), TEXT("0.3.0"));
			SendCommand(TEXT("connect"), Payload, TEXT("connect"));
		}

		virtual void Disconnect() override
		{
			bIntentionalShutdown = true;
			if (Process.IsValid() && Process->IsRunning())
			{
				SendCommand(TEXT("shutdown"), MakeShared<FJsonObject>(), TEXT("shutdown"));
			}
			else
			{
				SetStatus(EWorldDataAgentConnectionState::NotInstalled, TEXT("Agent Host is stopped."));
			}
		}

		virtual FString CreateThread(const FWorldDataCreateThreadRequest& Request) override
		{
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("clientConversationId"), Request.ClientConversationId);
			Payload->SetStringField(TEXT("workingDirectory"), Request.WorkingDirectory);
			if (!Request.Model.IsEmpty()) Payload->SetStringField(TEXT("model"), Request.Model);
			if (!Request.ApprovalPolicy.IsEmpty()) Payload->SetStringField(TEXT("approvalPolicy"), Request.ApprovalPolicy);
			if (!Request.SandboxMode.IsEmpty()) Payload->SetStringField(TEXT("sandboxMode"), Request.SandboxMode);
			Payload->SetBoolField(TEXT("ephemeral"), Request.bEphemeral);
			return SendCommand(TEXT("createThread"), Payload);
		}

		virtual FString ListThreads(const FWorldDataListThreadsRequest& Request) override
		{
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			if (!Request.Cursor.IsEmpty()) Payload->SetStringField(TEXT("cursor"), Request.Cursor);
			Payload->SetNumberField(TEXT("limit"), FMath::Clamp(Request.Limit, 1, 100));
			return SendCommand(TEXT("listThreads"), Payload);
		}

		virtual FString ResumeThread(const FWorldDataResumeThreadRequest& Request) override
		{
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("threadId"), Request.ThreadId);
			Payload->SetStringField(TEXT("workingDirectory"), Request.WorkingDirectory);
			if (!Request.Model.IsEmpty()) Payload->SetStringField(TEXT("model"), Request.Model);
			if (!Request.ApprovalPolicy.IsEmpty()) Payload->SetStringField(TEXT("approvalPolicy"), Request.ApprovalPolicy);
			if (!Request.SandboxMode.IsEmpty()) Payload->SetStringField(TEXT("sandboxMode"), Request.SandboxMode);
			return SendCommand(TEXT("resumeThread"), Payload);
		}

		virtual FString SendTurn(const FWorldDataSendTurnRequest& Request) override
		{
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("threadId"), Request.ThreadId);
			Payload->SetStringField(TEXT("clientTurnId"), Request.ClientTurnId);
			Payload->SetStringField(TEXT("text"), Request.Text);
			return SendCommand(TEXT("sendTurn"), Payload);
		}

		virtual void InterruptTurn(const FString& ThreadId, const FString& TurnId) override
		{
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("threadId"), ThreadId);
			Payload->SetStringField(TEXT("turnId"), TurnId);
			SendCommand(TEXT("interruptTurn"), Payload);
		}

		virtual void ResolveApproval(const FWorldDataApprovalDecision& Decision) override
		{
			TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("requestId"), Decision.RequestId);
			Payload->SetBoolField(TEXT("approved"), Decision.bApproved);
			SendCommand(TEXT("resolveApproval"), Payload);
		}

		virtual FWorldDataAgentStatusSnapshot GetStatus() const override
		{
			FScopeLock Lock(&StatusMutex);
			return Status;
		}

		virtual FWorldDataAgentEventDelegate& OnEvent() override { return EventDelegate; }

	private:
		FString SendCommand(const FString& Type, const TSharedRef<FJsonObject>& Payload, const FString& FixedRequestId = FString())
		{
			if (!Process.IsValid() || !Process->IsRunning())
			{
				Fail(TEXT("agent_host.not_running"), TEXT("WorldData Agent Host is not running."), true);
				return FString();
			}
			const FString RequestId = FixedRequestId.IsEmpty()
				? FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)
				: FixedRequestId;
			TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
			Command->SetNumberField(TEXT("protocolVersion"), WorldDataAgentProtocol::CurrentVersion);
			Command->SetStringField(TEXT("id"), RequestId);
			Command->SetStringField(TEXT("type"), Type);
			Command->SetObjectField(TEXT("payload"), Payload);
			const FString Json = SerializeJson(Command);
			if (Json.Len() > MaxHostFrameCharacters)
			{
				Fail(TEXT("host.frame_too_large"), TEXT("Agent Host command exceeds the configured limit."), false);
				return FString();
			}
			if (!Process->SendJsonLine(Json))
			{
				Fail(TEXT("agent_host.write_failed"), TEXT("Could not write the command to Agent Host."), true);
				return FString();
			}
			return RequestId;
		}

		void ConsumeOutput(const TArray<uint8>& Output)
		{
			if (Output.Num() > MaxHostFrameCharacters || StdoutBuffer.Num() + Output.Num() > MaxHostFrameCharacters * 2)
			{
				Fail(TEXT("host.output_overflow"), TEXT("Agent Host output exceeded the bounded buffer."), false);
				return;
			}
			StdoutBuffer.Append(Output);
			int32 LineStart = 0;
			for (int32 Index = 0; Index < StdoutBuffer.Num(); ++Index)
			{
				if (StdoutBuffer[Index] != static_cast<uint8>('\n')) continue;
				const FUTF8ToTCHAR Converted(
					reinterpret_cast<const ANSICHAR*>(StdoutBuffer.GetData() + LineStart),
					Index - LineStart);
				const FString Line(Converted.Length(), Converted.Get());
				if (!Line.IsEmpty()) ProcessLine(Line);
				LineStart = Index + 1;
			}
			if (LineStart > 0) StdoutBuffer.RemoveAt(0, LineStart, EAllowShrinking::No);
		}

		void ConsumeStderr(const TArray<uint8>& Output)
		{
			const int32 SafeLength = FMath::Min(Output.Num(), 4096);
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Output.GetData()), SafeLength);
			RecordDiagnostic(
				EWorldDataAgentLogLevel::Warning,
				TEXT("agent_host.stderr"),
				FString(Converted.Length(), Converted.Get()));
		}

		void ProcessLine(const FString& Line)
		{
			TSharedPtr<FJsonObject> Message;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
			if (!FJsonSerializer::Deserialize(Reader, Message) || !Message.IsValid())
			{
				RecordDiagnostic(EWorldDataAgentLogLevel::Warning, TEXT("host.invalid_json"), TEXT("Agent Host emitted a non-JSON frame."));
				return;
			}
			double ProtocolVersion = 0;
			if (!Message->TryGetNumberField(TEXT("protocolVersion"), ProtocolVersion)
				|| static_cast<int32>(ProtocolVersion) != WorldDataAgentProtocol::CurrentVersion)
			{
				Fail(TEXT("host.protocol_mismatch"), TEXT("Agent Host emitted an unsupported protocol version."), false);
				return;
			}
			FString Type;
			Message->TryGetStringField(TEXT("type"), Type);
			FString RequestId;
			Message->TryGetStringField(TEXT("requestId"), RequestId);
			const TSharedPtr<FJsonObject> Payload = ObjectField(Message, TEXT("payload"));
			if (Type == TEXT("host.started"))
			{
				SetStatus(EWorldDataAgentConnectionState::StartingCodex, TEXT("Agent Host started; connecting to Codex app-server."));
				return;
			}
			if (Type == TEXT("connect.completed"))
			{
				HandleConnected(Payload);
				return;
			}
			if (Type == TEXT("diagnostic"))
			{
				RecordDiagnostic(EWorldDataAgentLogLevel::Info, TEXT("codex.diagnostic"), Payload.IsValid() ? Payload->GetStringField(TEXT("text")) : FString());
				return;
			}
			if (Type == TEXT("error"))
			{
				HandleError(RequestId, Payload);
				return;
			}
			HandleAgentEvent(Type, RequestId, Payload);
		}

		void HandleConnected(const TSharedPtr<FJsonObject>& Payload)
		{
			FWorldDataAgentStatusSnapshot Next = GetStatus();
			Next.bAuthenticated = Payload.IsValid() && Payload->GetBoolField(TEXT("authenticated"));
			Next.State = Next.bAuthenticated ? EWorldDataAgentConnectionState::Ready : EWorldDataAgentConnectionState::CheckingAuth;
			Next.StatusText = Next.bAuthenticated ? TEXT("Codex app-server is ready.") : TEXT("Codex authentication is required.");
			Next.Error = FWorldDataAgentError();
			if (Payload.IsValid())
			{
				FString UserAgent;
				Payload->TryGetStringField(TEXT("userAgent"), UserAgent);
				Next.CodexVersion = UserAgent;
				const TSharedPtr<FJsonObject> Models = ObjectField(Payload, TEXT("models"));
				const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
				if (Models.IsValid() && Models->TryGetArrayField(TEXT("data"), Data) && Data)
				{
					Next.Models.Empty();
					for (const TSharedPtr<FJsonValue>& Value : *Data)
					{
						const TSharedPtr<FJsonObject> Model = Value.IsValid() ? Value->AsObject() : nullptr;
						if (!Model.IsValid()) continue;
						FWorldDataAgentModel Item;
						Model->TryGetStringField(TEXT("id"), Item.Id);
						Model->TryGetStringField(TEXT("displayName"), Item.DisplayName);
						Model->TryGetStringField(TEXT("description"), Item.Description);
						Model->TryGetBoolField(TEXT("isDefault"), Item.bDefault);
						if (!Item.Id.IsEmpty()) Next.Models.Add(MoveTemp(Item));
					}
				}
			}
			UpdateStatus(Next);
		}

		void HandleAgentEvent(const FString& Type, const FString& RequestId, const TSharedPtr<FJsonObject>& Payload)
		{
			FWorldDataAgentEvent Event;
			Event.RequestId = RequestId;
			if (Payload.IsValid())
			{
				Payload->TryGetStringField(TEXT("threadId"), Event.ThreadId);
				Payload->TryGetStringField(TEXT("turnId"), Event.TurnId);
				Payload->TryGetStringField(TEXT("itemId"), Event.ItemId);
				Payload->TryGetStringField(TEXT("text"), Event.Text);
				Payload->TryGetStringField(TEXT("toolName"), Event.ToolName);
				if (Event.RequestId.IsEmpty()) Payload->TryGetStringField(TEXT("requestId"), Event.RequestId);
			}
			if (Type == TEXT("threads.listed"))
			{
				Event.Type = EWorldDataAgentEventType::ThreadsListed;
				if (Payload.IsValid())
				{
					Payload->TryGetStringField(TEXT("nextCursor"), Event.NextCursor);
					const TArray<TSharedPtr<FJsonValue>>* Threads = nullptr;
					if (Payload->TryGetArrayField(TEXT("threads"), Threads) && Threads)
					{
						for (const TSharedPtr<FJsonValue>& Value : *Threads)
						{
							FWorldDataThreadSummary Thread;
							if (ParseThreadSummary(Value.IsValid() ? Value->AsObject() : nullptr, Thread)) Event.Threads.Add(MoveTemp(Thread));
						}
					}
				}
			}
			else if (Type == TEXT("thread.resumed") || Type == TEXT("thread.loaded"))
			{
				Event.Type = EWorldDataAgentEventType::ThreadLoaded;
				ParseThreadSummary(ObjectField(Payload, TEXT("thread")), Event.Thread);
				ParseConversationItems(Payload, Event.ConversationItems);
				if (Type == TEXT("thread.resumed"))
				{
					FWorldDataAgentStatusSnapshot Next = GetStatus();
					Next.ActiveThreadId = Event.ThreadId;
					const TSharedPtr<FJsonObject> Mcp = ObjectField(Payload, TEXT("mcp"));
					Next.bMcpConnected = Mcp.IsValid() && Mcp->GetBoolField(TEXT("connected"));
					Next.State = Next.bMcpConnected ? EWorldDataAgentConnectionState::Ready : EWorldDataAgentConnectionState::Degraded;
					Next.StatusText = Next.bMcpConnected ? TEXT("Codex thread resumed with WorldData MCP.") : TEXT("WorldData MCP was not restored for the resumed thread.");
					if (Next.bMcpConnected) Next.Error = FWorldDataAgentError();
					else Next.Error = { TEXT("mcp.not_ready"), Next.StatusText, TEXT("WorldDataAgentClient"), true };
					UpdateStatus(Next);
				}
			}
			else if (Type == TEXT("thread.created"))
			{
				Event.Type = EWorldDataAgentEventType::ThreadCreated;
				FWorldDataAgentStatusSnapshot Next = GetStatus();
				Next.ActiveThreadId = Event.ThreadId;
				const TSharedPtr<FJsonObject> Mcp = ObjectField(Payload, TEXT("mcp"));
				Next.bMcpConnected = Mcp.IsValid() && Mcp->GetBoolField(TEXT("connected"));
				if (!Next.bMcpConnected)
				{
					FString McpError = TEXT("WorldData MCP did not become ready for the Codex thread.");
					if (Mcp.IsValid()) Mcp->TryGetStringField(TEXT("error"), McpError);
					Next.State = EWorldDataAgentConnectionState::Degraded;
					Next.StatusText = McpError;
					Next.Error = { TEXT("mcp.not_ready"), McpError, TEXT("WorldDataAgentClient"), true };
				}
				else
				{
					Next.State = EWorldDataAgentConnectionState::Ready;
					Next.StatusText = TEXT("Codex and WorldData MCP are ready.");
					Next.Error = FWorldDataAgentError();
				}
				UpdateStatus(Next);
			}
			else if (Type == TEXT("turn.accepted") || Type == TEXT("turn.started")) Event.Type = EWorldDataAgentEventType::TurnStarted;
			else if (Type == TEXT("message.delta")) Event.Type = EWorldDataAgentEventType::MessageDelta;
			else if (Type == TEXT("item.started")) Event.Type = EWorldDataAgentEventType::ToolStarted;
			else if (Type == TEXT("item.completed")) Event.Type = EWorldDataAgentEventType::ToolCompleted;
			else if (Type == TEXT("approval.requested")) Event.Type = EWorldDataAgentEventType::ApprovalRequested;
			else if (Type == TEXT("turn.completed")) Event.Type = EWorldDataAgentEventType::TurnCompleted;
			else return;
			EventDelegate.Broadcast(Event);
		}

		void HandleError(const FString& RequestId, const TSharedPtr<FJsonObject>& Payload)
		{
			FWorldDataAgentError Error;
			if (Payload.IsValid())
			{
				Payload->TryGetStringField(TEXT("code"), Error.Code);
				Payload->TryGetStringField(TEXT("message"), Error.Message);
				Payload->TryGetStringField(TEXT("component"), Error.Component);
				Payload->TryGetBoolField(TEXT("retryable"), Error.bRetryable);
			}
			FWorldDataAgentStatusSnapshot Next = GetStatus();
			Next.State = Error.bRetryable ? EWorldDataAgentConnectionState::Degraded : EWorldDataAgentConnectionState::Fatal;
			Next.StatusText = Error.Message;
			Next.Error = Error;
			UpdateStatus(Next);
			FWorldDataAgentEvent Event;
			Event.Type = EWorldDataAgentEventType::TurnFailed;
			Event.RequestId = RequestId;
			Event.Error = Error;
			EventDelegate.Broadcast(Event);
			RecordDiagnostic(EWorldDataAgentLogLevel::Error, Error.Code, Error.Message);
		}

		void SetStatus(const EWorldDataAgentConnectionState State, const FString& Message)
		{
			FWorldDataAgentStatusSnapshot Next = GetStatus();
			Next.State = State;
			Next.StatusText = Message;
			UpdateStatus(Next);
		}

		void UpdateStatus(const FWorldDataAgentStatusSnapshot& Next)
		{
			{
				FScopeLock Lock(&StatusMutex);
				Status = Next;
			}
			FWorldDataAgentEvent Event;
			Event.Type = EWorldDataAgentEventType::ConnectionChanged;
			Event.Status = Next;
			EventDelegate.Broadcast(Event);
		}

		void Fail(const FString& Code, const FString& Message, const bool bRetryable)
		{
			FWorldDataAgentStatusSnapshot Next = GetStatus();
			Next.State = bRetryable ? EWorldDataAgentConnectionState::Degraded : EWorldDataAgentConnectionState::Fatal;
			Next.StatusText = Message;
			Next.Error = { Code, Message, TEXT("WorldDataAgentClient"), bRetryable };
			UpdateStatus(Next);
			RecordDiagnostic(EWorldDataAgentLogLevel::Error, Code, Message);
		}

		void RecordDiagnostic(const EWorldDataAgentLogLevel Level, const FString& Code, const FString& Message)
		{
			FWorldDataAgentDiagnosticEntry Entry;
			Entry.TimestampUtc = FDateTime::UtcNow();
			Entry.Level = Level;
			Entry.Component = TEXT("WorldDataAgentClient");
			Entry.Code = Code;
			Entry.Message = Message;
			Diagnostics.Record(Entry);
		}

		void HandleProcessCompleted(const int32 ReturnCode, const bool bCancelled)
		{
			Process.Reset();
			StdoutBuffer.Empty();
			if (bIntentionalShutdown)
			{
				bIntentionalShutdown = false;
				SetStatus(EWorldDataAgentConnectionState::NotInstalled, TEXT("Agent Host is stopped."));
				return;
			}
			Fail(TEXT("agent_host.exited"), FString::Printf(TEXT("Agent Host exited unexpectedly with code %d (cancelled=%s)."), ReturnCode, bCancelled ? TEXT("true") : TEXT("false")), true);
			FString DiagnosticPath;
			FString DiagnosticError;
			Diagnostics.ExportRedacted(DiagnosticPath, DiagnosticError);
		}

		void DisconnectProcessForReconnect()
		{
			++ProcessGeneration;
			if (Process.IsValid() && Process->IsRunning()) Process->RequestStop(true);
			Process.Reset();
			StdoutBuffer.Empty();
			bIntentionalShutdown = false;
		}

		IWorldDataAgentSecurity& Security;
		IWorldDataAgentDiagnostics& Diagnostics;
		FWorldDataAgentConnectionOptions ConnectionOptions;
		TSharedPtr<FWorldDataAgentHostProcess> Process;
		TArray<uint8> StdoutBuffer;
		mutable FCriticalSection StatusMutex;
		FWorldDataAgentStatusSnapshot Status;
		FWorldDataAgentEventDelegate EventDelegate;
		bool bIntentionalShutdown = false;
		uint64 ProcessGeneration = 0;
	};
}

class FWorldDataAgentClientModule final : public IWorldDataAgentClientModule
{
public:
	virtual TSharedRef<IWorldDataAgentGateway> CreateGateway(
		IWorldDataAgentSecurity& Security,
		IWorldDataAgentDiagnostics& Diagnostics) override
	{
		return MakeShared<FWorldDataAgentGateway>(Security, Diagnostics);
	}
};

IMPLEMENT_MODULE(FWorldDataAgentClientModule, WorldDataAgentClient)
