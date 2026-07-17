#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native data-table + UMG widget-tree editing MCP tools. Reimplemented directly against
// Engine (UDataTable) and UMG/UMGEditor (UWidgetBlueprint/UWidgetTree), NOT bridged to nwiro.
// DataTable: create / read-as-JSON / import-JSON / set-row / remove-row. Widget: create widget
// blueprint, add a widget to the tree, read the tree, set a widget property. GAS was
// intentionally skipped this batch (the gameplay-ability CDO/tag/modifier reflection is
// version-fragile). Self-contained: the world_data server merges GetToolDefinitionsJson() and
// routes via Dispatch().
namespace WorldDataMCP
{
namespace UIDataTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
