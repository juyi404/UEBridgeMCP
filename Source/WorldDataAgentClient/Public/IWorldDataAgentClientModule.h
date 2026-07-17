#pragma once

#include "IWorldDataAgentGateway.h"
#include "IWorldDataAgentDiagnostics.h"
#include "IWorldDataAgentSecurity.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class WORLDDATAAGENTCLIENT_API IWorldDataAgentClientModule : public IModuleInterface
{
public:
	virtual TSharedRef<IWorldDataAgentGateway> CreateGateway(
		IWorldDataAgentSecurity& Security,
		IWorldDataAgentDiagnostics& Diagnostics) = 0;

	static IWorldDataAgentClientModule& Get()
	{
		return FModuleManager::LoadModuleChecked<IWorldDataAgentClientModule>(TEXT("WorldDataAgentClient"));
	}
};
