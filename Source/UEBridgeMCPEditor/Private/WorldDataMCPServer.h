#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HttpRouteHandle.h"
#include "IHttpRouter.h"

class FJsonObject;
class FJsonValue;

class FWorldDataMCPServer
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
	static FString GetProjectInfoJson();
	static FString GetStatusJson();
	static FString GetLastError();
	static FDateTime GetStartedAtUtc();
	static FDateTime GetLastRefreshAtUtc();
	static FString GetClientConfigFilePath();
	static FString GetSavedConfigFilePath();
	static FString GetConnectionFilePath();
	static FString GetToolDefinitionsJson();
	static FString GetResourceListJson();
	// The trusted-client token (X-WorldData-MCP-Token) required by sensitive HTTP tools.
	// Exposed so the in-editor ACP panel can hand it to the agent's MCP connection.
	static FString GetAccessToken();
	static FString ReadResource(const FString& Uri);
	static void RefreshConnectionFiles();
	static TSharedPtr<FJsonObject> ProcessJsonRpc(const TSharedPtr<FJsonObject>& Request, bool bTrustedToolAccess = false, FString* OutNewSessionId = nullptr);

private:
	static bool HandleMCPPost(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPGet(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPDelete(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	static bool HandleMCPOptions(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	static TSharedPtr<FJsonObject> HandleInitialize(const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> HandleToolsList(const TSharedPtr<FJsonObject>& Params, FString* OutError = nullptr);
	static TSharedPtr<FJsonObject> HandleToolsCall(const TSharedPtr<FJsonObject>& Params, bool bTrustedToolAccess);
	static TSharedPtr<FJsonObject> HandleResourcesList(const TSharedPtr<FJsonObject>& Params, FString* OutError = nullptr);
	static TSharedPtr<FJsonObject> HandleResourcesRead(const TSharedPtr<FJsonObject>& Params);

	static FString DispatchTool(const FString& ToolName, const FString& ArgsJson, bool bTrustedToolAccess);

	static FString NegotiateProtocolVersion(const FString& RequestedVersion);
	static FString GetServerInstructions();

	static TUniquePtr<FHttpServerResponse> MakeJsonResponse(int32 Code, const FString& Body);
	static TSharedPtr<FJsonObject> MakeJsonRpcError(TSharedPtr<FJsonValue> Id, int32 Code, const FString& Message);

	static void SaveConfiguredPort(int32 Port);
	static void WriteClientConfig();
	static void WriteProjectConnectionFile();

	static TSharedPtr<IHttpRouter> HttpRouter;
	static TArray<FHttpRouteHandle> RouteHandles;
	static int32 BoundPort;
	static bool bRunning;
	static TSet<FString> SessionIds;
	static FString NegotiatedProtocolVersion;
	static FCriticalSection SessionMutex;
	static FString LastError;
	static FDateTime StartedAtUtc;
	static FDateTime LastRefreshAtUtc;
};
