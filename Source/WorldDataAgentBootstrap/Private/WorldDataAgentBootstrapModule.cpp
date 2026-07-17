#include "IWorldDataAgentBootstrapModule.h"

#include "IWorldDataAgentClientModule.h"
#include "IWorldDataAgentDiagnosticsModule.h"
#include "IWorldDataAgentRuntimeModule.h"
#include "IWorldDataAgentSecurityModule.h"
#include "Async/Async.h"

namespace
{
	class FWorldDataAgentSubsystem final : public IWorldDataAgentSubsystem
	{
	public:
		FWorldDataAgentSubsystem()
			: Security(IWorldDataAgentSecurityModule::Get().GetSecurity())
			, Diagnostics(IWorldDataAgentDiagnosticsModule::Get().GetDiagnostics())
			, Runtime(IWorldDataAgentRuntimeModule::Get().CreateRuntime(Security, Diagnostics))
			, Gateway(IWorldDataAgentClientModule::Get().CreateGateway(Security, Diagnostics))
		{
		}

		virtual void Initialize(const FWorldDataAgentConnectionOptions& Options) override
		{
			LastOptions = Options;
			FWorldDataAgentDiagnosticEntry Entry;
			Entry.Level = EWorldDataAgentLogLevel::Info;
			Entry.Component = TEXT("WorldDataAgentBootstrap");
			Entry.Code = TEXT("bootstrap.initialize");
			Entry.Message = FString::Printf(TEXT("Initializing modular agent protocol v%d."), Options.ProtocolVersion);
			Diagnostics.Record(Entry);

			FString RuntimeError;
			if (Runtime->LoadAndVerify(RuntimeError))
			{
				ApplyVerifiedRuntime();
			}
			Gateway->Connect(LastOptions);
		}

		virtual bool ConfigureRuntime(FString& OutError) override
		{
			if (!Runtime->ConfigureLocalRuntime(OutError))
			{
				return false;
			}
			ApplyVerifiedRuntime();
			ConnectGatewayOnGameThread();
			return true;
		}

		virtual void Shutdown() override
		{
			Gateway->Disconnect();
			if (!LastOptions.McpSecretHandle.IsEmpty())
			{
				Security.RevokeSecret(LastOptions.McpSecretHandle);
				LastOptions.McpSecretHandle.Empty();
			}
		}

		virtual IWorldDataAgentGateway& GetGateway() override { return Gateway.Get(); }
		virtual IWorldDataAgentSecurity& GetSecurity() override { return Security; }
		virtual IWorldDataAgentDiagnostics& GetDiagnostics() override { return Diagnostics; }
		virtual FWorldDataAgentRuntimeStatus GetRuntimeStatus() const override { return Runtime->GetStatus(); }

	private:
		void ApplyVerifiedRuntime()
		{
			const FWorldDataAgentRuntimeStatus RuntimeStatus = Runtime->GetStatus();
			if (!RuntimeStatus.bVerified) return;
			LastOptions.AgentHostExecutable = RuntimeStatus.AgentHostExecutable;
			LastOptions.CodexExecutable = RuntimeStatus.CodexExecutable;
			LastOptions.RuntimeManifestPath = RuntimeStatus.ManifestPath;
		}

		void ConnectGatewayOnGameThread()
		{
			const TSharedRef<IWorldDataAgentGateway> GatewayRef = Gateway;
			const FWorldDataAgentConnectionOptions Options = LastOptions;
			if (IsInGameThread())
			{
				GatewayRef->Connect(Options);
				return;
			}
			AsyncTask(ENamedThreads::GameThread, [GatewayRef, Options]()
			{
				GatewayRef->Connect(Options);
			});
		}

		IWorldDataAgentSecurity& Security;
		IWorldDataAgentDiagnostics& Diagnostics;
		TSharedRef<IWorldDataAgentRuntime> Runtime;
		TSharedRef<IWorldDataAgentGateway> Gateway;
		FWorldDataAgentConnectionOptions LastOptions;
	};
}

class FWorldDataAgentBootstrapModule final : public IWorldDataAgentBootstrapModule
{
public:
	virtual void StartupModule() override
	{
		Subsystem = MakeUnique<FWorldDataAgentSubsystem>();
	}

	virtual void ShutdownModule() override
	{
		if (Subsystem)
		{
			Subsystem->Shutdown();
			Subsystem.Reset();
		}
	}

	virtual IWorldDataAgentSubsystem& GetSubsystem() override
	{
		check(Subsystem);
		return *Subsystem;
	}

private:
	TUniquePtr<FWorldDataAgentSubsystem> Subsystem;
};

IMPLEMENT_MODULE(FWorldDataAgentBootstrapModule, WorldDataAgentBootstrap)
