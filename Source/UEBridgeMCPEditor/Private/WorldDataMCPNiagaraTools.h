#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Niagara VFX editing MCP tools — the reliable subset (add emitter to system, add
// renderer, read renderers + exposed parameters). Reimplemented directly against Niagara/
// NiagaraEditor (NOT bridged to nwiro). The fragile module-input/static-switch/HLSL ops are
// intentionally skipped. Self-contained: the world_data server merges GetToolDefinitionsJson()
// and routes via Dispatch().
namespace WorldDataMCP
{
namespace NiagaraTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
