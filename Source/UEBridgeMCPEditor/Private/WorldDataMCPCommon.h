#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Small helpers shared between the MCP server and the MCP tool implementations.
// Keeping these in one place avoids duplicating JSON serialization and project
// identity logic across translation units.
namespace WorldDataMCP
{
	FString JsonObjectToString(const TSharedRef<FJsonObject>& Json, bool bPretty = false);
	FString ErrorJson(const FString& Message);
	FString SuccessJson(const TSharedRef<FJsonObject>& Result);
	FString GetProjectName();
}
