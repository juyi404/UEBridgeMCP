#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Blueprint-authoring MCP tools that complement AuthoringTools/GraphTools: SCS components,
// interfaces, event dispatchers, function graphs, custom events, function parameters, local
// variables, and reparenting. Reimplemented directly against Kismet/SubobjectData (NOT bridged to
// nwiro). Self-contained: the world_data server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace BlueprintTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
