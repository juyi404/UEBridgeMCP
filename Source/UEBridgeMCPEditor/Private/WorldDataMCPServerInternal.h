#pragma once

#include "CoreMinimal.h"
#include "WorldDataMCPToolRegistry.h"

class FJsonObject;
class FJsonValue;

// Shared, file-local helpers extracted from WorldDataMCPServer.cpp so the server can be
// split across several translation units (server lifecycle/HTTP, config/connection-file
// writing, and tool dispatch) without each one re-declaring the same anonymous-namespace
// helpers. Implementations live in WorldDataMCPServerInternal.cpp.
//
// Consumers add `using namespace WorldDataMCP::ServerInternal;` so existing call sites
// keep their bare names.
namespace WorldDataMCP
{
namespace ServerInternal
{
	// --- Protocol versions (newest first) ---
	inline constexpr const TCHAR* GSupportedProtocolVersions[] = {
		TEXT("2025-11-25"),
		TEXT("2025-06-18"),
		TEXT("2025-03-26"),
		TEXT("2024-11-05")
	};

	// --- Project identity & paths ---
	FString GetProjectFilePath();
	FString GetProjectDir();
	uint32 GetProjectHash();
	FString GetProjectHashString();
	int32 GetDefaultPort();
	FString SanitizeNamePart(const FString& InName);
	FString GetConfigPath();
	FString GetConnectionPath();
	FString GetCursorClientConfigPath();

	// --- File IO ---
	bool SaferReplaceWriteString(const FString& Content, const FString& TargetPath);
	TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path);

	// --- Access token ---
	FString GenerateAccessToken();
	bool TryGetConfigAccessToken(const TSharedPtr<FJsonObject>& Config, FString& OutToken);
	FString GetConfiguredAccessToken();
	TSharedRef<FJsonObject> MakeAccessTokenHeadersObject();

	// --- Protocol negotiation ---
	// The supported-version table is encapsulated here; callers use these accessors instead
	// of indexing a shared array across translation units.
	TArray<TSharedPtr<FJsonValue>> MakeSupportedProtocolVersionValues();
	bool IsSupportedProtocolVersion(const FString& Version);
	FString GetNewestProtocolVersion();

	// --- Codex policy snapshot ---
	FString GetCodexPolicySnapshotJson();

	// --- Tool registry & definitions ---
	// The set of tool modules iterated by both aggregation (tools/list) and dispatch
	// (tools/call). FMCPToolModule and registration live in WorldDataMCPToolRegistry.h;
	// this accessor forwards to the runtime registry so the server core stays free of any
	// compile-time list of tool groups.
	const TArray<FMCPToolModule>& GetMCPToolModules();
	FString BuildCombinedToolDefinitionsJson(const FString& LocalToolsJson);

	// --- Trusted-tool gating ---
	bool ToolRequiresTrustedClient(const FString& ToolName);
	bool ToolRequiresHumanConfirmation(const FString& ToolName);
	bool ToolArgumentsIncludeHumanConfirmation(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutError);

	// --- Shared CVar state ---
	// When non-zero, require Mcp-Session-Id on post-initialize MCP requests.
	extern int32 GWorldDataMCPRequireSessionHeader;
	// Maximum items returned by tools/list and resources/list before emitting nextCursor. 0 disables pagination.
	extern int32 GWorldDataMCPPaginationPageSize;
}
}
