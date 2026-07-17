#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace WorldDataMCP
{
namespace ExtractedTools
{
	FString GetToolDefinitionsJson();
	FString ReadLog(const TSharedPtr<FJsonObject>& Args);
	FString ExecutePython(const TSharedPtr<FJsonObject>& Args);

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
