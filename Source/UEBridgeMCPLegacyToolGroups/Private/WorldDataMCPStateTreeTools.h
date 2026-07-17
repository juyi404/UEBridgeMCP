#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native StateTree deep-editing MCP tools (tasks, transitions, evaluators, global tasks,
// conditions, state parameters). Reimplemented directly against StateTreeEditor (NOT bridged
// to nwiro). Complements the basic create_state_tree / add_state_tree_state / read_state_tree
// already in AIAnimTools. Self-contained: the world_data server merges GetToolDefinitionsJson()
// and routes via Dispatch().
namespace WorldDataMCP
{
namespace StateTreeTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
