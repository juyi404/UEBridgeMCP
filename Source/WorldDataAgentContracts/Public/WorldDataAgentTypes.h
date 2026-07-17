#pragma once

#include "CoreMinimal.h"

namespace WorldDataAgentProtocol
{
	inline constexpr int32 CurrentVersion = 1;
	inline constexpr int32 MinimumVersion = 1;
}

enum class EWorldDataAgentConnectionState : uint8
{
	NotInstalled,
	Installing,
	StartingMcp,
	StartingAgentHost,
	StartingCodex,
	Handshaking,
	CheckingAuth,
	CheckingMcp,
	Ready,
	Busy,
	Degraded,
	Fatal
};

enum class EWorldDataAgentEventType : uint8
{
	ConnectionChanged,
	ThreadsListed,
	ThreadCreated,
	ThreadLoaded,
	TurnStarted,
	MessageDelta,
	ToolStarted,
	ToolCompleted,
	ApprovalRequested,
	TurnCompleted,
	TurnFailed
};

enum class EWorldDataAgentLogLevel : uint8
{
	Debug,
	Info,
	Warning,
	Error
};

struct FWorldDataAgentError
{
	FString Code;
	FString Message;
	FString Component;
	bool bRetryable = false;

	bool IsSet() const { return !Code.IsEmpty() || !Message.IsEmpty(); }
};

struct FWorldDataAgentConnectionOptions
{
	FString ProjectRoot;
	FString RuntimeManifestPath;
	FString AgentHostExecutable;
	FString CodexExecutable;
	FString McpUrl;
	FString McpSecretHandle;
	int32 ProtocolVersion = WorldDataAgentProtocol::CurrentVersion;
};

struct FWorldDataAgentModel
{
	FString Id;
	FString DisplayName;
	FString Description;
	bool bDefault = false;
};

struct FWorldDataAgentRuntimeStatus
{
	bool bConfigured = false;
	bool bVerified = false;
	FString AgentHostExecutable;
	FString AgentHostVersion;
	FString AgentHostSha256;
	FString CodexExecutable;
	FString CodexVersion;
	FString CodexSha256;
	FString CodexSchemaPath;
	FString CodexSchemaSha256;
	FString ManifestPath;
	FWorldDataAgentError Error;
};

struct FWorldDataAgentStatusSnapshot
{
	EWorldDataAgentConnectionState State = EWorldDataAgentConnectionState::NotInstalled;
	FString StatusText;
	FString AgentHostVersion;
	FString CodexVersion;
	FString ActiveThreadId;
	TArray<FWorldDataAgentModel> Models;
	bool bAuthenticated = false;
	bool bMcpConnected = false;
	FWorldDataAgentError Error;
};

struct FWorldDataCreateThreadRequest
{
	FString ClientConversationId;
	FString WorkingDirectory;
	FString Model;
	FString ApprovalPolicy;
	FString SandboxMode;
};

struct FWorldDataListThreadsRequest
{
	FString Cursor;
	int32 Limit = 50;
};

struct FWorldDataResumeThreadRequest
{
	FString ThreadId;
	FString WorkingDirectory;
	FString Model;
	FString ApprovalPolicy;
	FString SandboxMode;
};

struct FWorldDataThreadSummary
{
	FString Id;
	FString Title;
	FString Preview;
	FString WorkingDirectory;
	int64 CreatedAt = 0;
	int64 UpdatedAt = 0;
	FString Status;
};

struct FWorldDataConversationItem
{
	FString Id;
	FString TurnId;
	FString Kind;
	FString Role;
	FString Text;
	FString ToolName;
	FString Status;
};

struct FWorldDataSendTurnRequest
{
	FString ThreadId;
	FString ClientTurnId;
	FString Text;
};

struct FWorldDataApprovalDecision
{
	FString RequestId;
	FString ThreadId;
	FString TurnId;
	bool bApproved = false;
};

struct FWorldDataAgentEvent
{
	EWorldDataAgentEventType Type = EWorldDataAgentEventType::ConnectionChanged;
	FString RequestId;
	FString ThreadId;
	FString TurnId;
	FString ItemId;
	FString Text;
	FString ToolName;
	FString NextCursor;
	FWorldDataThreadSummary Thread;
	TArray<FWorldDataThreadSummary> Threads;
	TArray<FWorldDataConversationItem> ConversationItems;
	FWorldDataAgentStatusSnapshot Status;
	FWorldDataAgentError Error;
};

struct FWorldDataAgentDiagnosticEntry
{
	FDateTime TimestampUtc;
	EWorldDataAgentLogLevel Level = EWorldDataAgentLogLevel::Info;
	FString Component;
	FString Code;
	FString Message;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FWorldDataAgentEventDelegate, const FWorldDataAgentEvent&);
