#pragma once

#include "CoreMinimal.h"

class FJsonObject;

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
	FString ChangeSummaryFingerprint;
	FString TargetRevision;
	FString OwnerSessionId;
	FDateTime CreatedAtUtc;
	FDateTime ExpiresAtUtc;
	bool bReadyForDecision = false;
};

// Small helpers shared between the MCP server and the MCP tool implementations.
// Keeping these in one place avoids duplicating JSON serialization and project
// identity logic across translation units.
namespace WorldDataMCP
{
	UEBRIDGEMCPCORE_API FString JsonObjectToString(const TSharedRef<FJsonObject>& Json, bool bPretty = false);
	// Shared JSON-object parser used by the restored tool groups and Core.
	// Returns nullptr when the payload is malformed or is not an object.
	UEBRIDGEMCPCORE_API TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonText);
	UEBRIDGEMCPCORE_API FString ErrorJson(const FString& Message);
	UEBRIDGEMCPCORE_API FString SuccessJson(const TSharedRef<FJsonObject>& Result);
	UEBRIDGEMCPCORE_API FString GetProjectName();
}
