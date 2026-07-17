#include "UEBridgeMCPCoreModule.h"

namespace
{
	class FWorldDataMCPService final : public IWorldDataMCPService
	{
	public:
		virtual void StartConfigured() override { FWorldDataMCPServer::Start(FWorldDataMCPServer::LoadConfiguredPort()); }
		virtual void Start(int32 Port) override { FWorldDataMCPServer::Start(Port); }
		virtual void Stop() override { FWorldDataMCPServer::Stop(); }
		virtual bool IsRunning() const override { return FWorldDataMCPServer::IsRunning(); }
		virtual int32 GetPort() const override { return FWorldDataMCPServer::GetPort(); }
		virtual int32 LoadConfiguredPort() const override { return FWorldDataMCPServer::LoadConfiguredPort(); }
		virtual FString GetServerName() const override { return FWorldDataMCPServer::GetServerName(); }
		virtual FString GetProjectId() const override { return FWorldDataMCPServer::GetProjectId(); }
		virtual FString GetMcpUrl() const override { return FWorldDataMCPServer::GetMcpUrl(); }
		virtual FString GetAccessTokenHeaderName() const override { return FWorldDataMCPServer::GetAccessTokenHeaderName(); }
		virtual FString GetAccessToken() const override { return FWorldDataMCPServer::GetAccessToken(); }
		virtual FString GetStatusJson() const override { return FWorldDataMCPServer::GetStatusJson(); }
		virtual FString GetProjectInfoJson() const override { return FWorldDataMCPServer::GetProjectInfoJson(); }
		virtual FString GetToolDefinitionsJson() const override { return FWorldDataMCPServer::GetToolDefinitionsJson(); }
		virtual FString GetResourceListJson() const override { return FWorldDataMCPServer::GetResourceListJson(); }
		virtual FString ReadResource(const FString& Uri) const override { return FWorldDataMCPServer::ReadResource(Uri); }
		virtual FString GetCliSetupReportJson() const override { return FWorldDataMCPServer::GetCliSetupReportJson(); }
		virtual FString GetClientConfigFilePath() const override { return FWorldDataMCPServer::GetClientConfigFilePath(); }
		virtual FString GetConnectionFilePath() const override { return FWorldDataMCPServer::GetConnectionFilePath(); }
		virtual bool IsUnsafePythonEnabled() const override { return FWorldDataMCPServer::IsUnsafePythonEnabled(); }
		virtual FString GetUnsafePythonCapabilityToken() const override { return FWorldDataMCPServer::GetUnsafePythonCapabilityToken(); }
		virtual void RefreshConnectionFiles() override { FWorldDataMCPServer::RefreshConnectionFiles(); }
		virtual void ProvisionClientConfigurations() override { FWorldDataMCPServer::ProvisionClientConfigurations(); }
		virtual bool RotateAccessToken(FString& OutError) override { return FWorldDataMCPServer::RotateAccessToken(OutError); }
		virtual TArray<FWorldDataMCPApprovalSummary> GetPendingApprovals() const override { return FWorldDataMCPServer::GetPendingApprovals(); }
		virtual bool ResolvePendingApproval(const FString& ApprovalId, bool bApprove, FString& OutError) override { return FWorldDataMCPServer::ResolvePendingApproval(ApprovalId, bApprove, OutError); }
	};
}

class FUEBridgeMCPCoreModule final : public IUEBridgeMCPCoreModule
{
	virtual IWorldDataMCPService& GetService() override { return Service; }

private:
	FWorldDataMCPService Service;
};

IMPLEMENT_MODULE(FUEBridgeMCPCoreModule, UEBridgeMCPCore)
