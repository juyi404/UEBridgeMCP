#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Scene-authoring MCP tools (environment & lighting, level diagnostics, splines &
// foliage, physics/collision/audio), ported natively from the nwiro/ue-mcp handler set
// so the world_data server is self-contained. Self-contained like EditorTools: the server
// merges GetToolDefinitionsJson() and routes through Dispatch().
namespace WorldDataMCP
{
namespace SceneTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
