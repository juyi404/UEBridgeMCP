#pragma once

#include "IWorldDataAgentSubsystem.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class WORLDDATAAGENTBOOTSTRAP_API IWorldDataAgentBootstrapModule : public IModuleInterface
{
public:
	virtual IWorldDataAgentSubsystem& GetSubsystem() = 0;

	static IWorldDataAgentBootstrapModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IWorldDataAgentBootstrapModule>(TEXT("WorldDataAgentBootstrap"));
	}
};
