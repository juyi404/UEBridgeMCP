#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class SWidget;

class WORLDDATAAGENTUI_API IWorldDataAgentUIModule : public IModuleInterface
{
public:
	virtual TSharedRef<SWidget> CreatePanel() = 0;

	static IWorldDataAgentUIModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IWorldDataAgentUIModule>(TEXT("WorldDataAgentUI"));
	}
};
