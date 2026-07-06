#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Material Instance Constant authoring MCP tools. Reimplemented directly against
// UMaterialEditingLibrary (NOT bridged to nwiro). Create a UMaterialInstanceConstant from a
// parent material/instance, set scalar/vector/texture/static-switch parameters, and list the
// available parameter names. This is the "recolor / retune a material without rebuilding the
// graph" workflow — high value for scene look-dev and complementary to the Batch-5 material
// FUNCTION tools. Self-contained: the world_data server merges GetToolDefinitionsJson() and
// routes via Dispatch().
namespace WorldDataMCP
{
namespace MaterialInstanceTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
