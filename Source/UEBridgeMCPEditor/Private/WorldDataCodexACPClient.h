#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/InteractiveProcess.h"

struct FWorldDataAcpPermissionOption
{
	FString OptionId;
	FString Name;
	FString Kind;
};

struct FWorldDataAcpPermissionRequest
{
	int32 RequestId = 0;
	FString Title;
	FString ToolName;
	FString ToolCallId;
	FString SessionId;
	FString AllowOptionId;
	FString DenyOptionId;
	TArray<FWorldDataAcpPermissionOption> Options;
};

DECLARE_DELEGATE_OneParam(FWorldDataAcpTextDelegate, const FString&);
DECLARE_DELEGATE_OneParam(FWorldDataAcpStatusDelegate, const FString&);
DECLARE_DELEGATE_OneParam(FWorldDataAcpErrorDelegate, const FString&);
DECLARE_DELEGATE_OneParam(FWorldDataAcpPermissionDelegate, const FWorldDataAcpPermissionRequest&);

enum class EWorldDataCodexPermissionMode : uint8
{
	Default,
	Plan,
	Bypass
};

class FWorldDataCodexACPClient : public TSharedFromThis<FWorldDataCodexACPClient>
{
public:
	~FWorldDataCodexACPClient();

	void SendPrompt(const FString& Prompt);
	void Stop();
	void SetPermissionMode(EWorldDataCodexPermissionMode InMode);
	EWorldDataCodexPermissionMode GetPermissionMode() const;
	void RespondToPermission(int32 RequestId, const FString& OptionId);

	bool IsRunning() const;
	bool IsReady() const;
	bool IsProcessing() const;
	FString GetLastError() const;

	FWorldDataAcpTextDelegate OnText;
	FWorldDataAcpStatusDelegate OnStatus;
	FWorldDataAcpErrorDelegate OnError;
	FWorldDataAcpPermissionDelegate OnPermission;

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

	FString FindAdapterBinary() const;
	FString ResolveOnPath(const FString& Command) const;
	FString JsonToString(const TSharedPtr<FJsonObject>& Object) const;
	void Fail(const FString& Message);
	void EmitStatus(const FString& Message);
	void EmitText(const FString& Text);

	TSharedPtr<FInteractiveProcess> Process;
	FString StdoutBuffer;
	FString SessionId;
	FString PendingPrompt;
	FString LastError;
	EWorldDataCodexPermissionMode PermissionMode = EWorldDataCodexPermissionMode::Default;

	int32 NextRpcId = 1;
	int32 InitRpcId = 0;
	int32 SessionRpcId = 0;
	int32 PromptRpcId = 0;
	int32 NextPermissionRequestId = 1;
	TMap<int32, TSharedPtr<FJsonValue>> PendingPermissionIds;

	bool bInitialized = false;
	bool bCreatingSession = false;
	bool bPromptInFlight = false;
};
