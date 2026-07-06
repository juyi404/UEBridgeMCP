#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Graph-editing MCP tools: Blueprint K2 node add/connect/find/delete and Material expression
// add/connect/inspect. Ported natively from the nwiro handlers; material side uses the official
// UMaterialEditingLibrary. Self-contained like the other tool modules: the server merges
// GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace GraphTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
