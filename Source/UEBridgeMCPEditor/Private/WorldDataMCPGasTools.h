#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Native Gameplay Ability System authoring MCP tools. Reimplemented directly against
// GameplayAbilities (NOT bridged to nwiro). Authoring-time only and intentionally limited to
// the reliable subset: create GameplayEffect / GameplayAbility / AttributeSet blueprints, add a
// FGameplayAttributeData member to an attribute set, add an AbilitySystemComponent to a
// blueprint, and add/update a modifier on a GameplayEffect's CDO. The churny runtime ops
// (apply_effect / set_attribute) and the most version-fragile CDO/tag plumbing are skipped.
// Self-contained: the world_data server merges GetToolDefinitionsJson() and routes via Dispatch().
namespace WorldDataMCP
{
namespace GasTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
