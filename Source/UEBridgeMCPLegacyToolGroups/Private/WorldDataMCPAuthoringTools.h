#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Asset-authoring MCP tools (Blueprint, Material, Niagara, GAS, Sequencer, Widget,
// data tables / structs / enums) ported natively from the nwiro/ue-mcp handler set so the
// world_data server can create and read these asset types without the nwiro bridge. Scope is
// asset creation + read; deep graph/node wiring stays in the nwiro server. Self-contained like
// EditorTools/SceneTools: the server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace AuthoringTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
