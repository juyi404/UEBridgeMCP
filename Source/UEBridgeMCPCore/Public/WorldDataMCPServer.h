#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "IHttpRouter.h"

class FJsonObject;
class FJsonValue;

struct UEBRIDGEMCPCORE_API FWorldDataMCPSessionState
{
	FString ProtocolVersion;
	FString ClientLabel;
	FString ClientVersion;
	FString Principal;
	FString Scope;
	FString TaskId;
	FString ThreadId;
	FDateTime CreatedAtUtc;
	FDateTime LastActivityAtUtc;
};

struct UEBRIDGEMCPCORE_API FWorldDataMCPApprovalSummary
{
	FString ApprovalId;
	FString ToolName;
	FString Risk;
	FString TargetSummary;
	FString ChangeSummaryHash;
	FString TargetRevision;
	FString OwnerSessionId;
	FDateTime CreatedAtUtc;
	FDateTime ExpiresAtUtc;
	bool bReadyForDecision = false;
};

class UEBRIDGEMCPCORE_API FWorldDataMCPServer
{
public:
	static void Start(int32 Port);
	static void Stop();
	static bool IsRunning();
	static int32 GetPort();
	static int32 LoadConfiguredPort();

	static FString GetServerName();
	static FString GetProjectId();
	static FString GetMcpUrl();
	static FString GetAccessTokenHeaderName();
	static FString GetAccessToken();
	// Revokes current MCP sessions and persists a new access token. Client
	// configuration remains an explicit, separate user action.
	static bool RotateAccessToken(FString& OutError);
	static bool IsUnsafePythonEnabled();
	static FString GetUnsafePythonCapabilityToken();
	static bool ValidateUnsafePythonCapability(const FString& Candidate);
	static FString GetProjectInfoJson();
	static FString GetStatusJson();
	static FString GetLastError();
	static FDateTime GetStartedAtUtc();
	static FDateTime GetLastRefreshAtUtc();
	static FString GetClientConfigFilePath();
	static FString GetCodexClientConfigFilePath();
	static FString GetClaudeSettingsFilePath();
	static FString GetCliSetupReportJson();
	static FString GetSavedConfigFilePath();
	static FString GetConnectionFilePath();
	// These are editor-panel-only APIs. MCP clients cannot approve their own
	// mutations; they can only observe the returned approval/job state.
	static TArray<FWorldDataMCPApprovalSummary> GetPendingApprovals();
	static bool ResolvePendingApproval(const FString& ApprovalId, bool bApprove, FString& OutError);
	static FString GetToolDefinitionsJson();
	static FString GetResourceListJson();
	static FString ReadResource(const FString& Uri);
	// Updates only Saved/UEBridgeMCP state. It never writes client configuration.
	static void RefreshConnectionFiles();
	// Explicit opt-in action for generating local project/client configuration files.
	static void ProvisionClientConfigurations();
	static TSharedPtr<FJsonObject> ProcessJsonRpc(const TSharedPtr<FJsonObject>& Request);

private:
	static bool HandleMCPPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	static TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> HandleResourcesList(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> HandleResourcesRead(const TSharedPtr<FJsonObject>& Params);

	static FString DispatchTool(const FString& ToolName, const FString& ArgsJson);

	static FString NegotiateProtocolVersion(const FString& RequestedVersion);
	static FString GetServerInstructions();
	static void EnsureAccessToken();

	static TUniquePtr<FHttpServerResponse> MakeJsonResponse(int32 Code, const FString& Body);
	static TSharedPtr<FJsonObject> MakeJsonRpcError(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message);

	static void SaveConfiguredPort(int32 Port);
	static void WriteClientConfig();
	static void WriteCodexClientConfig();
	static void WriteProjectConnectionFile();
	static FString RegisterSession(const FString& ProtocolVersion, const FString& ClientLabel, const FString& ClientVersion, const FString& RequestedTaskId, const FString& RequestedThreadId);
	static bool IsSessionActiveForApproval(const FString& SessionId);

	static TSharedPtr<IHttpRouter> HttpRouter;
	static TArray<FHttpRouteHandle> RouteHandles;
	static int32 BoundPort;
	static bool bRunning;
	static TMap<FString, FWorldDataMCPSessionState> SessionProtocolVersions;
	static FString AccessToken;
	static FString UnsafePythonCapabilityToken;
	static FDateTime UnsafePythonCapabilityExpiresAtUtc;
	static FString LastError;
	static FDateTime StartedAtUtc;
	static FDateTime LastRefreshAtUtc;
};
