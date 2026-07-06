#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace WorldDataMCP
{
namespace ExtractedTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);

	FString ListResources();
	FString ReadResource(const TSharedPtr<FJsonObject>& Args);
	// Reads the resources whose implementations live in this module (plugins, source index,
	// level/current, components, blueprints, pcg graphs, problems, performance, log, viewport).
	// Called by FWorldDataMCPServer::ReadResource so the canonical worlddata:// catalog is unified.
	FString ReadExtendedResource(const FString& Uri);
	FString ReadLog(const TSharedPtr<FJsonObject>& Args);
	FString ExecutePython(const TSharedPtr<FJsonObject>& Args);
	FString GetCurrentTaskContext(const TSharedPtr<FJsonObject>& Args);
	FString GetRelevantContext(const TSharedPtr<FJsonObject>& Args);
	FString GetAssetReferences(const TSharedPtr<FJsonObject>& Args);
	FString SummarizeBlueprint(const TSharedPtr<FJsonObject>& Args);

	FString SearchAssets(const TSharedPtr<FJsonObject>& Args);
	FString FindStaticMeshes(const TSharedPtr<FJsonObject>& Args);
	FString GetLevelActors(const TSharedPtr<FJsonObject>& Args);
	FString GetProjectInfo(const TSharedPtr<FJsonObject>& Args);
	FString ListProjectModules(const TSharedPtr<FJsonObject>& Args);
	FString GetBuildConfiguration(const TSharedPtr<FJsonObject>& Args);

	FString ReadFile(const TSharedPtr<FJsonObject>& Args);
	FString WriteFile(const TSharedPtr<FJsonObject>& Args);
	FString DeleteFile(const TSharedPtr<FJsonObject>& Args);
	FString RenameFile(const TSharedPtr<FJsonObject>& Args);

	FString PlayInEditor(const TSharedPtr<FJsonObject>& Args);
	FString StopPIE(const TSharedPtr<FJsonObject>& Args);

	FString PcgRecipeLibraryStatus(const TSharedPtr<FJsonObject>& Args);
	FString SearchPcgRecipes(const TSharedPtr<FJsonObject>& Args);
	FString ReadPcgRecipe(const TSharedPtr<FJsonObject>& Args);
	FString ReadPcgSceneBinding(const TSharedPtr<FJsonObject>& Args);
}
}
