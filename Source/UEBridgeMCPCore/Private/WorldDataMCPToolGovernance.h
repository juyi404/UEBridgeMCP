#pragma once

#include "CoreMinimal.h"
#include "WorldDataMCPToolRegistry.h"

class FJsonObject;

// Server-side governance for editor automation. It deliberately lives next to
// the MCP server rather than in a client so local policies cannot be bypassed
// by a client that ignores tool annotations.
namespace WorldDataMCP::ToolGovernance
{
	using EToolRisk = WorldDataMCP::EToolRisk;

	struct FCallerContext
	{
		FString SessionId;
		FString Principal;
		FString ClientLabel;
		FString ClientVersion;
		FString Scope;
		FString TaskId;
		FString ThreadId;
		FString RunId;
		FString TransactionId;
		FString ExpectedWorldRevision;
		FString ExpectedObjectRevision;
	};

	struct FInvocation
	{
		FString Id;
		FString ToolName;
		EToolRisk Risk = EToolRisk::ReadOnly;
		FDateTime StartedAtUtc;
		TArray<FString> ArgumentNames;
	FString ArgumentFingerprint;
	FCallerContext Caller;
	FString ApprovalId;
	FString ChangeSummaryHash;
	FString TargetRevision;
	FString WorldRevision;
	};

	EToolRisk GetRisk(const FString& ToolName);
	FString GetRiskName(EToolRisk Risk);
	bool RequiresInteractiveApproval(EToolRisk Risk);
	bool RequiresInteractiveApproval(const FString& ToolName);

	// Starts an audit record. Argument values are intentionally not retained.
	FInvocation BeginInvocation(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments, const FCallerContext& Caller = FCallerContext());

	// Records lifecycle transitions for the editor-owned non-modal approval
	// queue. Values and secret-capable argument contents are never written.
	void RecordApprovalEvent(const FInvocation& Invocation, const FString& Outcome, const FString& Detail);

	// Writes a redacted JSONL audit record for every completed or denied call.
	void CompleteInvocation(const FInvocation& Invocation, const FString& ResultJson);

	// Exposes the registered tool policy to MCP clients. The registry is the
	// source of truth for the risk/capability/revision metadata in this snapshot.
	FString GetPolicySnapshotJson();
}
