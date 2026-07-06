#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Runtime registry for MCP tool modules.
//
// Each tool module exposes two free functions: one that returns its tools/list
// definitions (a JSON array string) and one that dispatches tools/call for the
// names it owns. Historically these were wired into a compile-time table that
// directly referenced every module's symbols, which forced the MCP server core to
// depend (at compile + link time) on every tool group and, transitively, on every
// engine module those groups pull in.
//
// Instead, modules register themselves here at runtime (from their owning UE
// module's StartupModule). The server core iterates the registry and never needs a
// compile-time list of tool groups -- the prerequisite for splitting the tool layer
// into independently-built UE modules whose heavy engine dependencies stay local.
namespace WorldDataMCP
{
	// One self-contained tool module.
	struct FMCPToolModule
	{
		// Lower Priority is listed first during aggregation and tried first during
		// dispatch, so prefix-claiming modules (e.g. PcgKnowledge before PCG) keep
		// their legacy ordering. Modules sharing a Priority keep registration order.
		int32 Priority = 0;
		FString (*GetDefinitions)() = nullptr;
		bool (*Dispatch)(const FString&, const TSharedPtr<FJsonObject>&, FString&) = nullptr;
	};

	// Register one tool module. Re-registering the same definition/dispatch pair is ignored,
	// which keeps Live Coding or repeated StartupModule paths from duplicating tool entries.
	// Null function pointers are ignored.
	UEBRIDGEMCPCORE_API void RegisterMCPToolModule(const FMCPToolModule& Module);

	inline void RegisterMCPToolModule(
		int32 Priority,
		FString (*GetDefinitions)(),
		bool (*Dispatch)(const FString&, const TSharedPtr<FJsonObject>&, FString&))
	{
		RegisterMCPToolModule(FMCPToolModule{ Priority, GetDefinitions, Dispatch });
	}

	// All registered modules, stable-sorted by ascending Priority.
	UEBRIDGEMCPCORE_API const TArray<FMCPToolModule>& GetRegisteredMCPToolModules();
}
