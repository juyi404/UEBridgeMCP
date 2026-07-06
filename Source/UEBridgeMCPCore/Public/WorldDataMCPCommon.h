#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Small helpers shared between the MCP server and the MCP tool implementations.
// Keeping these in one place avoids duplicating JSON serialization and project
// identity logic across translation units.
namespace WorldDataMCP
{
	UEBRIDGEMCPCORE_API FString JsonObjectToString(const TSharedRef<FJsonObject>& Json, bool bPretty = false);
	// Parse a JSON object from text. Returns nullptr on malformed input or when the
	// payload is not a JSON object. Shared so the server and tool modules don't each
	// reimplement the same reader boilerplate.
	UEBRIDGEMCPCORE_API TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonText);
	UEBRIDGEMCPCORE_API FString ErrorJson(const FString& Message);
	UEBRIDGEMCPCORE_API FString SuccessJson(const TSharedRef<FJsonObject>& Result);
	UEBRIDGEMCPCORE_API FString GetProjectName();
}
