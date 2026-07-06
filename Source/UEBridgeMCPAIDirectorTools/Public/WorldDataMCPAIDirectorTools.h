#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace WorldDataMCP
{
namespace AIDirectorTools
{
	FString GetToolDefinitionsJson();
	bool Dispatch(const FString& ToolName, const TSharedPtr<FJsonObject>& Args, FString& OutResult);
}
}
