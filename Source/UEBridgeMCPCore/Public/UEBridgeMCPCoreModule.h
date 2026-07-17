#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "WorldDataMCPCommon.h"

class UEBRIDGEMCPCORE_API IWorldDataMCPService
{
public:
	virtual ~IWorldDataMCPService() = default;

	virtual void StartConfigured() = 0;
	virtual void Start(int32 Port) = 0;
	virtual void Stop() = 0;
	virtual bool IsRunning() const = 0;
	virtual int32 GetPort() const = 0;
	virtual int32 LoadConfiguredPort() const = 0;
	virtual FString GetServerName() const = 0;
	virtual FString GetProjectId() const = 0;
	virtual FString GetMcpUrl() const = 0;
	virtual FString GetAccessTokenHeaderName() const = 0;
	virtual FString GetAccessToken() const = 0;
	virtual FString GetStatusJson() const = 0;
	virtual FString GetProjectInfoJson() const = 0;
	virtual FString GetToolDefinitionsJson() const = 0;
	virtual FString GetResourceListJson() const = 0;
	virtual FString ReadResource(const FString& Uri) const = 0;
	virtual FString GetCliSetupReportJson() const = 0;
	virtual FString GetClientConfigFilePath() const = 0;
	virtual FString GetConnectionFilePath() const = 0;
	virtual bool IsUnsafePythonEnabled() const = 0;
	virtual FString GetUnsafePythonCapabilityToken() const = 0;
	virtual bool ValidateUnsafePythonCapability(const FString& Candidate) const = 0;
	virtual void RefreshConnectionFiles() = 0;
	virtual void ProvisionClientConfigurations() = 0;
	virtual bool RotateAccessToken(FString& OutError) = 0;
	virtual TArray<FWorldDataMCPApprovalSummary> GetPendingApprovals() const = 0;
	virtual bool ResolvePendingApproval(const FString& ApprovalId, bool bApprove, FString& OutError) = 0;
};

class UEBRIDGEMCPCORE_API IUEBridgeMCPCoreModule : public IModuleInterface
{
public:
	virtual IWorldDataMCPService& GetService() = 0;

	static IUEBridgeMCPCoreModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IUEBridgeMCPCoreModule>(TEXT("UEBridgeMCPCore"));
	}
};

inline IWorldDataMCPService& GetWorldDataMCPService()
{
	return IUEBridgeMCPCoreModule::Get().GetService();
}
