#if WITH_DEV_AUTOMATION_TESTS

#include "IWorldDataAgentBootstrapModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
	struct FWorldDataAgentEndToEndState
	{
		IWorldDataAgentGateway* Gateway = nullptr;
		FDelegateHandle EventHandle;
		double StartedAt = 0.0;
		bool bThreadRequested = false;
		bool bThreadCreated = false;
		bool bMcpConnected = false;
		bool bTurnRequested = false;
		bool bToolObserved = false;
		bool bTurnCompleted = false;
		bool bListRequested = false;
		bool bThreadsListed = false;
		bool bResumeRequested = false;
		bool bThreadLoaded = false;
		FString ThreadId;
		FString ResponseText;
		TArray<FWorldDataConversationItem> LoadedItems;
		FWorldDataAgentError Failure;
	};

	class FWorldDataAgentEndToEndCommand final : public IAutomationLatentCommand
	{
	public:
		FWorldDataAgentEndToEndCommand(
			FAutomationTestBase* InTest,
			const TSharedRef<FWorldDataAgentEndToEndState>& InState)
			: Test(InTest), State(InState)
		{
		}

		virtual bool Update() override
		{
			if (FPlatformTime::Seconds() - State->StartedAt > 120.0)
			{
				Test->AddError(TEXT("Timed out waiting for the real Codex/MCP turn to complete."));
				Cleanup();
				return true;
			}
			if (State->Failure.IsSet())
			{
				Test->AddError(FString::Printf(TEXT("Agent failure [%s]: %s"), *State->Failure.Code, *State->Failure.Message));
				Cleanup();
				return true;
			}

			const FWorldDataAgentStatusSnapshot Status = State->Gateway->GetStatus();
			if (Status.Error.IsSet()
				&& (Status.State == EWorldDataAgentConnectionState::Degraded || Status.State == EWorldDataAgentConnectionState::Fatal))
			{
				Test->AddError(FString::Printf(TEXT("Gateway error [%s]: %s"), *Status.Error.Code, *Status.Error.Message));
				for (const FWorldDataAgentDiagnosticEntry& Entry : IWorldDataAgentBootstrapModule::Get().GetSubsystem().GetDiagnostics().Snapshot())
				{
					if (Entry.Level == EWorldDataAgentLogLevel::Error || Entry.Level == EWorldDataAgentLogLevel::Warning)
					{
						Test->AddInfo(FString::Printf(TEXT("Diagnostic [%s/%s]: %s"), *Entry.Component, *Entry.Code, *Entry.Message));
					}
				}
				Cleanup();
				return true;
			}
			if (!State->bThreadRequested && Status.State == EWorldDataAgentConnectionState::Ready)
			{
				FWorldDataCreateThreadRequest Request;
				Request.ClientConversationId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
				Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
				Request.ApprovalPolicy = TEXT("never");
				Request.SandboxMode = TEXT("read-only");
				State->bThreadRequested = !State->Gateway->CreateThread(Request).IsEmpty();
			}

			if (State->bThreadCreated && !State->bTurnRequested)
			{
				Test->TestTrue(TEXT("WorldData MCP is connected for the created thread"), State->bMcpConnected);
				if (!State->bMcpConnected)
				{
					Cleanup();
					return true;
				}
				FWorldDataSendTurnRequest Request;
				Request.ThreadId = State->ThreadId;
				Request.ClientTurnId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
				Request.Text = TEXT("Call the WorldData MCP tool worlddata.get_project_info exactly once, then reply with only the projectName.");
				State->bTurnRequested = !State->Gateway->SendTurn(Request).IsEmpty();
			}

			if (State->bTurnCompleted && !State->bListRequested)
			{
				Test->TestTrue(TEXT("worlddata.get_project_info tool event was observed"), State->bToolObserved);
				Test->TestTrue(TEXT("Assistant returned the current project name"), State->ResponseText.Contains(TEXT("CollectWorldData")));
				FWorldDataListThreadsRequest Request;
				Request.Limit = 25;
				State->bListRequested = !State->Gateway->ListThreads(Request).IsEmpty();
			}

			if (State->bThreadsListed && !State->bResumeRequested)
			{
				FWorldDataResumeThreadRequest Request;
				Request.ThreadId = State->ThreadId;
				Request.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
				Request.ApprovalPolicy = TEXT("never");
				Request.SandboxMode = TEXT("read-only");
				State->bResumeRequested = !State->Gateway->ResumeThread(Request).IsEmpty();
			}

			if (State->bThreadLoaded)
			{
				const bool bHistoryHasProjectName = State->LoadedItems.ContainsByPredicate([](const FWorldDataConversationItem& Item)
				{
					return Item.Text.Contains(TEXT("CollectWorldData"));
				});
				Test->TestTrue(TEXT("Resumed thread contains normalized conversation history"), bHistoryHasProjectName);
				Test->TestTrue(TEXT("WorldData MCP remains connected after resume"), State->Gateway->GetStatus().bMcpConnected);
				Cleanup();
				return true;
			}
			return false;
		}

	private:
		void Cleanup()
		{
			if (State->Gateway && State->EventHandle.IsValid())
			{
				State->Gateway->OnEvent().Remove(State->EventHandle);
				State->EventHandle.Reset();
			}
		}

		FAutomationTestBase* Test;
		TSharedRef<FWorldDataAgentEndToEndState> State;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldDataAgentRuntimeConfigureTest,
	"WorldData.Agent.Runtime.ConfigureAndVerify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldDataAgentRuntimeConfigureTest::RunTest(const FString& Parameters)
{
	IWorldDataAgentSubsystem& Subsystem = IWorldDataAgentBootstrapModule::Get().GetSubsystem();
	FString Error;
	const bool bConfigured = Subsystem.ConfigureRuntime(Error);
	TestTrue(FString::Printf(TEXT("Runtime configures successfully: %s"), *Error), bConfigured);
	const FWorldDataAgentRuntimeStatus Status = Subsystem.GetRuntimeStatus();
	TestTrue(TEXT("Runtime is configured"), Status.bConfigured);
	TestTrue(TEXT("Runtime hashes are verified"), Status.bVerified);
	TestTrue(TEXT("Agent Host path is absolute"), !Status.AgentHostExecutable.IsEmpty() && !FPaths::IsRelative(Status.AgentHostExecutable));
	TestTrue(TEXT("Codex path is absolute"), !Status.CodexExecutable.IsEmpty() && !FPaths::IsRelative(Status.CodexExecutable));
	TestTrue(TEXT("Codex schema is pinned"), Status.CodexSchemaSha256.Len() == 64);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWorldDataAgentEndToEndTest,
	"WorldData.Agent.Integration.RealCodexMcpTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldDataAgentEndToEndTest::RunTest(const FString& Parameters)
{
	IWorldDataAgentSubsystem& Subsystem = IWorldDataAgentBootstrapModule::Get().GetSubsystem();
	FString Error;
	if (!Subsystem.ConfigureRuntime(Error))
	{
		AddError(FString::Printf(TEXT("Could not configure the verified runtime: %s"), *Error));
		return false;
	}

	const TSharedRef<FWorldDataAgentEndToEndState> State = MakeShared<FWorldDataAgentEndToEndState>();
	State->Gateway = &Subsystem.GetGateway();
	State->StartedAt = FPlatformTime::Seconds();
	State->EventHandle = State->Gateway->OnEvent().AddLambda([State](const FWorldDataAgentEvent& Event)
	{
		switch (Event.Type)
		{
		case EWorldDataAgentEventType::ThreadsListed:
			State->bThreadsListed = Event.Threads.ContainsByPredicate([State](const FWorldDataThreadSummary& Thread)
			{
				return Thread.Id == State->ThreadId;
			});
			if (!State->bThreadsListed)
			{
				State->Failure = { TEXT("thread.not_listed"), TEXT("The completed Codex thread was not returned by thread/list."), TEXT("WorldDataAgentBootstrapTests"), false };
			}
			break;
		case EWorldDataAgentEventType::ThreadCreated:
			State->bThreadCreated = true;
			State->ThreadId = Event.ThreadId;
			State->bMcpConnected = State->Gateway->GetStatus().bMcpConnected;
			break;
		case EWorldDataAgentEventType::ThreadLoaded:
			State->bThreadLoaded = Event.ThreadId == State->ThreadId;
			State->LoadedItems = Event.ConversationItems;
			break;
		case EWorldDataAgentEventType::MessageDelta:
			State->ResponseText += Event.Text;
			break;
		case EWorldDataAgentEventType::ToolStarted:
		case EWorldDataAgentEventType::ToolCompleted:
			State->bToolObserved |= Event.ToolName.Contains(TEXT("worlddata.get_project_info"));
			break;
		case EWorldDataAgentEventType::TurnCompleted:
			State->bTurnCompleted = true;
			break;
		case EWorldDataAgentEventType::TurnFailed:
			State->Failure = Event.Error;
			break;
		default:
			break;
		}
	});
	ADD_LATENT_AUTOMATION_COMMAND(FWorldDataAgentEndToEndCommand(this, State));
	return true;
}

#endif
