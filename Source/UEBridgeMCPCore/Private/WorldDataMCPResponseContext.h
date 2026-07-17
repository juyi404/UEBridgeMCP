#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace WorldDataMCP::ResponseContext
{
	// A single, opt-in control surface shared by every MCP tool. These controls
	// are consumed by the server and are never forwarded to editor operations.
	struct FResponseControl
	{
		int32 MaxBytes = 64 * 1024;
		int32 PageSize = 50;
		FString Cursor;
		FString IfRevision;
		TArray<FString> Fields;
	};

	// Reads and removes arguments.responseControl. Invalid controls return false
	// with a user-facing error; normal tool arguments are otherwise untouched.
	bool ExtractResponseControl(const TSharedPtr<FJsonObject>& Arguments, FResponseControl& OutControl, FString& OutError);

	// Retrieves an immutable, short-lived response snapshot for a follow-up page.
	// Cursors are scoped to the tool that created them.
	bool TryResolveCursor(const FString& ToolName, const FString& Cursor, FString& OutResultJson, FString& OutError);

	// Adds schema documentation for responseControl to the combined tool catalog.
	FString AddResponseControlToToolDefinitions(const FString& DefinitionsJson);

	// Applies the byte budget, top-level field projection, pagination, and
	// revision metadata to a tool's structured JSON result.
	FString ShapeToolResult(const FString& ToolName, const FString& ResultJson, const FResponseControl& Control, bool bReadOnlyTool);

	// Drops all cached response pages when the server stops or credentials rotate.
	void ResetSnapshots();
}
