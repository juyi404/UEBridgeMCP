#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Animation asset-editing MCP tools (montage create/sections/links, notifies, curves,
// virtual bones, sockets). Reimplemented directly against the anim engine API (NOT bridged to
// nwiro). Complements the AnimBlueprint/state-machine tools in AIAnimTools. Self-contained: the
// world_data server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace AnimTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
