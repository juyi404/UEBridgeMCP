#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/InteractiveProcess.h"
#include "WorldDataAgentBackend.h"

struct FWorldDataCodexAcpLaunchSpec
{
	FString Executable;
	FString Arguments;
	FString DisplayPath;
};

class FWorldDataCodexACPClient : public IWorldDataAgentBackend, public TSharedFromThis<FWorldDataCodexACPClient>
{
public:
	~FWorldDataCodexACPClient();

	virtual FString GetBackendId() const override { return TEXT("acp"); }
	virtual FString GetDisplayName() const override { return TEXT("ACP Adapter (compatibility)"); }
	virtual FWorldDataAgentBackendCapabilities GetCapabilities() const override
	{
		FWorldDataAgentBackendCapabilities Capabilities;
		Capabilities.bSupportsPermissionRequests = true;
		return Capabilities;
	}
	virtual void ConfigureMcpConnection(const FWorldDataAgentMcpConnection& Connection) override { McpConnection = Connection; }
	virtual void SendPrompt(const FString& Prompt) override;
	virtual void Stop() override;
	virtual void SetPermissionMode(EWorldDataCodexPermissionMode InMode) override;
	virtual void SetConversationIdentity(const FString& TaskId, const FString& ThreadId) override { SetContextIdentity(TaskId, ThreadId); }
	void SetContextIdentity(const FString& InTaskId, const FString& InThreadId);
	virtual EWorldDataCodexPermissionMode GetPermissionMode() const override;
	virtual void RespondToPermission(int32 RequestId, const FString& OptionId) override;

	virtual bool IsRunning() const override;
	virtual bool IsReady() const override;
	virtual bool IsProcessing() const override;
	virtual FString GetLastError() const override;

private:
	bool EnsureProcess();
	bool StartSessionIfReady();
	void SendPendingPromptIfReady();

	int32 SendRpc(const FString& Method, const TSharedPtr<FJsonObject>& Params = nullptr);
	void SendRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result);
	void SendRpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message);
	void SendPermissionOutcome(const TSharedPtr<FJsonValue>& Id, const FString& OptionId);
	void SendRaw(const FString& Json);

	void ConsumeOutput(const FString& Output);
	void ProcessLine(const FString& Line);
	void HandleRpcResponse(int32 Id, const TSharedPtr<FJsonObject>& Result, const TSharedPtr<FJsonObject>& Error);
	void HandleMethod(const FString& Method, const TSharedPtr<FJsonObject>& Message);
	void HandleSessionUpdate(const FString& AcpSessionId, const TSharedPtr<FJsonObject>& Update);

	bool FindAdapterLaunch(FWorldDataCodexAcpLaunchSpec& OutLaunchSpec) const;
	FString JsonToString(const TSharedPtr<FJsonObject>& Object) const;
	void Fail(const FString& Message);
	void EmitStatus(const FString& Message);
	void EmitText(const FString& Text);

	TSharedPtr<FInteractiveProcess> Process;
	FString StdoutBuffer;
	FString SessionId;
	FString PendingPrompt;
	FString LastError;
	FString ActiveAdapterDisplayPath;
	FString ContextTaskId;
	FString ContextThreadId;
	FWorldDataAgentMcpConnection McpConnection;
	EWorldDataCodexPermissionMode PermissionMode = EWorldDataCodexPermissionMode::Default;

	int32 NextRpcId = 1;
	int32 InitRpcId = 0;
	int32 SessionRpcId = 0;
	int32 PromptRpcId = 0;
	int32 NextPermissionRequestId = 1;
	uint64 ProcessGeneration = 0;
	TMap<int32, TSharedPtr<FJsonValue>> PendingPermissionIds;

	bool bInitialized = false;
	bool bCreatingSession = false;
	bool bPromptInFlight = false;
};
