#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Editor-automation MCP tools ported from common open-source UE MCP servers
// (console/CVar control, level + outliner management, asset inspection, material
// editing, editor undo/redo/save). Implemented natively against UnrealEd/Engine so
// they don't depend on the Python bridge. Kept self-contained like ExtractedTools:
// the server merges GetToolDefinitionsJson() and routes through Dispatch().
namespace WorldDataMCP
{
namespace EditorTools
{
	FString GetToolDefinitionsJson();
	// Returns true and fills OutResult if ToolName belongs to this module; false otherwise
	// so the server can fall through to its other tool tables.
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
