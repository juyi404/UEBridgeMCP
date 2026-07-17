#pragma once

#include "WorldDataAgentTypes.h"

class IWorldDataAgentRuntime
{
public:
	virtual ~IWorldDataAgentRuntime() = default;

	virtual bool ConfigureLocalRuntime(FString& OutError) = 0;
	virtual bool LoadAndVerify(FString& OutError) = 0;
	virtual FWorldDataAgentRuntimeStatus GetStatus() const = 0;
};
