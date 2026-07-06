#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Animation-Blueprint / Behavior-Tree-Blackboard / StateTree MCP tools, ported natively from the
// nwiro handlers. Scope: asset creation, a few key authoring mutations (add state machine + states,
// add blackboard key, add state-tree state), and read-back. Self-contained like the other tool
// modules: the server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace AIAnimTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
