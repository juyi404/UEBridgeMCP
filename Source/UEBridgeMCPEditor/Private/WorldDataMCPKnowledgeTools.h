#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Knowledge + perception tools — the "general solution" layer on top of the mechanical verbs:
//   A) ue_web_search / ue_fetch_doc  -> runtime knowledge retrieval (the agent looks up the
//      UE-correct approach for ANY intent instead of guessing).
//   B) describe_scene                -> driver-aware state readout; paired with capture_viewport
//      it forms the perceive -> act -> perceive loop.
// Self-contained like the other tool modules: the server merges GetToolDefinitionsJson() and
// routes via Dispatch().
namespace WorldDataMCP
{
namespace KnowledgeTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
