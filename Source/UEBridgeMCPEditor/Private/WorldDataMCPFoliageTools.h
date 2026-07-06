#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native instanced-foliage editing MCP tools. Reimplemented directly against the runtime
// Foliage module (AInstancedFoliageActor / UFoliageType_InstancedStaticMesh), NOT bridged to
// nwiro. Covers the high-value loop for "paint a forest": create a foliage type from a mesh,
// list the level's foliage types + instance counts, scatter/paint instances (explicit
// transforms or procedural scatter with optional ground tracing), remove all instances of a
// type, and tweak foliage-type settings via reflection. Self-contained: the world_data server
// merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace FoliageTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
