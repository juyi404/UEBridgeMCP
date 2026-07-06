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

struct FWorldDataApprovedMcpToolCall
{
	FString ToolName;
	FString ToolCallId;
	FString SessionId;
	FString InputFingerprint;
};

DECLARE_DELEGATE_OneParam(FWorldDataAcpTextDelegate, const FString&);
DECLARE_DELEGATE_OneParam(FWorldDataAcpStatusDelegate, const FString&);
DECLARE_DELEGATE_OneParam(FWorldDataAcpErrorDelegate, const FString&);
DECLARE_DELEGATE_OneParam(FWorldDataAcpPermissionDelegate, const FWorldDataAcpPermissionRequest&);
// Fires once when a prompt turn finishes (the prompt RPC resolves). A semantic
// turn-end signal so listeners need not string-match the status text.
DECLARE_DELEGATE(FWorldDataAcpTurnCompleteDelegate);

enum class EWorldDataCodexPermissionMode : uint8
{
	Default,
	Plan,
	Focus,
	Bypass
};

// Which local ACP adapter the in-panel chat drives. Both speak the standard
// Agent Client Protocol over stdio, so only the launched adapter binary differs.
enum class EWorldDataAcpAgent : uint8
{
	Codex,      // codex-acp adapter
	Cursor,     // Cursor CLI ACP server: cursor-agent/agent acp
	ClaudeCode  // claude-agent-acp adapter; legacy acp-claude-code is still accepted
};

class FWorldDataCodexACPClient : public TSharedFromThis<FWorldDataCodexACPClient>
{
public:
	~FWorldDataCodexACPClient();

	void SendPrompt(const FString& Prompt);
	void Stop();
	void SetPermissionMode(EWorldDataCodexPermissionMode InMode);
	EWorldDataCodexPermissionMode GetPermissionMode() const;
	// Selects which ACP adapter to launch. Switching while a session is live tears the
	// current process down so the next prompt relaunches the newly selected adapter.
	void SetAgent(EWorldDataAcpAgent InAgent);
	EWorldDataAcpAgent GetAgent() const;

	// Selects the launched CLI model. Empty = adapter default.
	// Claude Code receives ANTHROPIC_MODEL; Codex/Cursor receive --model at launch.
	// Changing it while a session is live tears the process down so the next prompt relaunches.
	void SetModel(const FString& InModel);
	FString GetModel() const;

	// Resolve the ACP adapter binary for an agent (env override, plugin/project Binaries,
	// then PATH). Returns empty if not found. Exposed so the panel can show adapter status
	// without duplicating the search logic.
	static FString FindAdapterBinaryForAgent(EWorldDataAcpAgent InAgent);
	static FString GetAdapterBaseName(EWorldDataAcpAgent InAgent);
	static FString FindClaudeCodeExecutable();
	void RespondToPermission(int32 RequestId, const FString& OptionId);

	bool IsRunning() const;
	bool IsReady() const;
	bool IsProcessing() const;
	FString GetLastError() const;

	FWorldDataAcpTextDelegate OnText;
	FWorldDataAcpTextDelegate OnThought;
	FWorldDataAcpStatusDelegate OnStatus;
	FWorldDataAcpErrorDelegate OnError;
	FWorldDataAcpPermissionDelegate OnPermission;
	FWorldDataAcpTurnCompleteDelegate OnTurnComplete;

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
	FString GetAgentDisplayName() const;
	static FString ResolveOnPath(const FString& Command);
	FString JsonToString(const TSharedPtr<FJsonObject>& Object) const;
	void Fail(const FString& Message);
	void EmitStatus(const FString& Message);
	void EmitText(const FString& Text);
	void EmitThought(const FString& Text);

	TSharedPtr<FInteractiveProcess> Process;
	FString StdoutBuffer;
	FString SessionId;
	FString PendingPrompt;
	FString InFlightPrompt;
	FString LastError;
	EWorldDataCodexPermissionMode PermissionMode = EWorldDataCodexPermissionMode::Default;
	EWorldDataAcpAgent Agent = EWorldDataAcpAgent::Cursor;
	FString Model;

	int32 NextRpcId = 1;
	int32 InitRpcId = 0;
	int32 SessionRpcId = 0;
	int32 PromptRpcId = 0;
	int32 NextPermissionRequestId = 1;
	TMap<int32, TSharedPtr<FJsonValue>> PendingPermissionIds;
	TMap<int32, FString> PendingPermissionAllowOptionIds;
	TMap<int32, FWorldDataApprovedMcpToolCall> PendingWorldDataPermissionRequests;
	TArray<FWorldDataApprovedMcpToolCall> ApprovedWorldDataMcpToolCalls;

	bool bInitialized = false;
	bool bCreatingSession = false;
	bool bPromptInFlight = false;
};
