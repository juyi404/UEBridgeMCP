#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Level Sequence / Sequencer editing MCP tools. Reimplemented directly against
// LevelSequence + MovieScene (NOT bridged to nwiro). Focused on the capability nwiro's
// handler set lacks: actual KEYFRAMING. Create a sequence, possess a level actor, add a 3D
// transform track + section and write location/rotation/scale keys at given times (so you can
// animate e.g. a directional light's rotation for a sunset, or a camera move), and set the
// playback range. Self-contained: the world_data server merges GetToolDefinitionsJson() and
// routes via Dispatch().
namespace WorldDataMCP
{
namespace SequencerTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
