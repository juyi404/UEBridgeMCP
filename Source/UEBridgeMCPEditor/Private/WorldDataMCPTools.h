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
		FString AnalyzeScenePerformance(const TSharedPtr<FJsonObject>& Args);
		FString ReadAsset(const TSharedPtr<FJsonObject>& Args);
		FString SelectActor(const TSharedPtr<FJsonObject>& Args);
		FString SpawnActor(const TSharedPtr<FJsonObject>& Args);
		FString GetObjectProperties(const TSharedPtr<FJsonObject>& Args);
		FString SetObjectProperty(const TSharedPtr<FJsonObject>& Args);
		FString SetActorTransform(const TSharedPtr<FJsonObject>& Args);
		FString DeleteActor(const TSharedPtr<FJsonObject>& Args);
		FString AddComponent(const TSharedPtr<FJsonObject>& Args);
		FString AttachActor(const TSharedPtr<FJsonObject>& Args);
		FString DuplicateActor(const TSharedPtr<FJsonObject>& Args);
		FString CaptureViewport(const TSharedPtr<FJsonObject>& Args);
		FString GetBootstrapContextJson();

		// Scene-generation gate: the design CONCEPT BRIEF, enforced as a precondition.
		FString SetSceneBrief(const TSharedPtr<FJsonObject>& Args);
		FString GetSceneBrief(const TSharedPtr<FJsonObject>& Args);
		FString ClearSceneBrief(const TSharedPtr<FJsonObject>& Args);
	}
}
