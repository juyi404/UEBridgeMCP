#pragma once

#include "IWorldDataAgentSecurity.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class WORLDDATAAGENTSECURITY_API IWorldDataAgentSecurityModule : public IModuleInterface
{
public:
	virtual IWorldDataAgentSecurity& GetSecurity() = 0;

	static IWorldDataAgentSecurityModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IWorldDataAgentSecurityModule>(TEXT("WorldDataAgentSecurity"));
	}
};
