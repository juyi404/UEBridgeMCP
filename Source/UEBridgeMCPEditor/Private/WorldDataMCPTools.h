#pragma once

#include "CoreMinimal.h"

class FJsonObject;

// Implementations of the editor-mutating / world-inspecting MCP tools.
// These run on the game thread (the MCP server marshals calls accordingly) and
// return JSON strings ready to embed in an MCP tool/resource response.
namespace WorldDataMCP
{
	namespace Tools
	{
		FString ListLevelActors(const TSharedPtr<FJsonObject>& Args);
		FString GetSelectedActors(const TSharedPtr<FJsonObject>& Args);
		FString GetActorDetails(const TSharedPtr<FJsonObject>& Args);
		FString FindAssets(const TSharedPtr<FJsonObject>& Args);
		FString GetContentSummary(const TSharedPtr<FJsonObject>& Args);
		FString ReadAsset(const TSharedPtr<FJsonObject>& Args);
		FString SelectActor(const TSharedPtr<FJsonObject>& Args);
		FString SpawnActor(const TSharedPtr<FJsonObject>& Args);
		FString TransformActor(const TSharedPtr<FJsonObject>& Args);
		FString DeleteActor(const TSharedPtr<FJsonObject>& Args);
		FString AttachActor(const TSharedPtr<FJsonObject>& Args);
		FString SetActorProperty(const TSharedPtr<FJsonObject>& Args);
		FString SaveCurrentLevel(const TSharedPtr<FJsonObject>& Args);
		FString CreateAsset(const TSharedPtr<FJsonObject>& Args);
		FString CreateBlueprintAsset(const TSharedPtr<FJsonObject>& Args);
		FString ModifyMaterialInstance(const TSharedPtr<FJsonObject>& Args);
		FString CreatePcgGraphFromRecipe(const TSharedPtr<FJsonObject>& Args);
		FString GetBootstrapContextJson();
	}
}
