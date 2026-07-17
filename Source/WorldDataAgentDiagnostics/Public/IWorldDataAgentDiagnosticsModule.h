#pragma once

#include "IWorldDataAgentDiagnostics.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class WORLDDATAAGENTDIAGNOSTICS_API IWorldDataAgentDiagnosticsModule : public IModuleInterface
{
public:
	virtual IWorldDataAgentDiagnostics& GetDiagnostics() = 0;

	static IWorldDataAgentDiagnosticsModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IWorldDataAgentDiagnosticsModule>(TEXT("WorldDataAgentDiagnostics"));
	}
};
