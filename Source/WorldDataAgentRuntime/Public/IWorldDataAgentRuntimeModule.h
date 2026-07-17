#pragma once

#include "IWorldDataAgentDiagnostics.h"
#include "IWorldDataAgentRuntime.h"
#include "IWorldDataAgentSecurity.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class WORLDDATAAGENTRUNTIME_API IWorldDataAgentRuntimeModule : public IModuleInterface
{
public:
	virtual TSharedRef<IWorldDataAgentRuntime> CreateRuntime(
		IWorldDataAgentSecurity& Security,
		IWorldDataAgentDiagnostics& Diagnostics) = 0;

	static IWorldDataAgentRuntimeModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IWorldDataAgentRuntimeModule>(TEXT("WorldDataAgentRuntime"));
	}
};
