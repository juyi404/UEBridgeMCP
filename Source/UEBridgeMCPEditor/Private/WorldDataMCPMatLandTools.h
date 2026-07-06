#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Material-Function + Landscape MCP tools. Reimplemented directly against
// UMaterialEditingLibrary / the Landscape API (NOT bridged to nwiro). Self-contained: the
// world_data server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace MatLandTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
