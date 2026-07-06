#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native gameplay-framework MCP tools: Navigation (project point, find path, rebuild, info)
// and Enhanced Input (create input action / mapping context, map a key). Reimplemented
// directly against NavigationSystem + EnhancedInput (NOT bridged to nwiro). SmartObjects was
// intentionally skipped this batch (SmartObjectsModule isn't a Build.cs dep and the plugin API
// is churny). Self-contained: the world_data server merges GetToolDefinitionsJson() and routes
// via Dispatch().
namespace WorldDataMCP
{
namespace GameplayTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
