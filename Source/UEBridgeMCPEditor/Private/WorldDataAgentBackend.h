#pragma once

#include "CoreMinimal.h"

// Backend-neutral UI contract. It intentionally describes only the editor
// conversation boundary; UE automation remains behind the separate HTTP MCP
// server and its own session, approval, audit, and task orchestration layers.
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

struct FWorldDataAgentBackendCapabilities
{
	bool bSupportsStreaming = true;
	bool bSupportsAttachments = false;
	bool bSupportsPermissionRequests = false;
	bool bSupportsModelSelection = false;
	bool bUsesNativeMcpConfiguration = false;
};

struct FWorldDataAgentMcpConnection
{
	FString ServerName;
	FString Url;
	FString AccessTokenHeader;
	FString AccessToken;
	bool bAvailable = false;
};

class IWorldDataAgentBackend
{
public:
	virtual ~IWorldDataAgentBackend() = default;

	virtual FString GetBackendId() const = 0;
	virtual FString GetDisplayName() const = 0;
	virtual FWorldDataAgentBackendCapabilities GetCapabilities() const = 0;
	virtual void ConfigureMcpConnection(const FWorldDataAgentMcpConnection& Connection) {}
	virtual void SendPrompt(const FString& Prompt) = 0;
	virtual void Stop() = 0;
	virtual void SetPermissionMode(EWorldDataCodexPermissionMode InMode) = 0;
	virtual void SetConversationIdentity(const FString& TaskId, const FString& ThreadId) {}
	virtual EWorldDataCodexPermissionMode GetPermissionMode() const = 0;
	virtual void RespondToPermission(int32 RequestId, const FString& OptionId) = 0;
	virtual bool IsRunning() const = 0;
	virtual bool IsReady() const = 0;
	virtual bool IsProcessing() const = 0;
	virtual FString GetLastError() const = 0;

	FWorldDataAcpTextDelegate OnText;
	FWorldDataAcpStatusDelegate OnStatus;
	FWorldDataAcpErrorDelegate OnError;
	FWorldDataAcpPermissionDelegate OnPermission;
};
