#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native PCG graph-authoring MCP tools — reimplemented directly against the PCG/PCGEditor
// engine API (NOT bridged to the nwiro server; the two stay fully isolated). Covers the full
// authoring loop: list/create/read graphs, add/connect/remove nodes, set node settings, set
// StaticMeshSpawner meshes, spawn a PCG volume, and regenerate. Self-contained like the other
// tool modules: the world_data server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace PCGTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
