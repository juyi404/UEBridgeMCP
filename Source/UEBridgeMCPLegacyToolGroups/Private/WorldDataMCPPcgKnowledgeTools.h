#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Read-only PCG knowledge-base MCP tools. Serves a distilled UE5 PCG corpus
// (nodes, parameters, gotchas, reusable patterns, workflows + a BM25 search index)
// loaded once from Plugins/UEBridgeMCP/Data/pcg_knowledge.json. Lets an agent look up
// how PCG nodes work, common pitfalls, and step-by-step recipes while authoring graphs.
// Self-contained like SceneTools/EditorTools: the server merges GetToolDefinitionsJson()
// and routes through Dispatch(). All tools are read-only (no trusted-client gate).
namespace WorldDataMCP
{
namespace PcgKnowledgeTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
