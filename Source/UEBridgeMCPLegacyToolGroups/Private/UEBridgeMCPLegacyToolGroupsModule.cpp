#include "Modules/ModuleManager.h"

namespace WorldDataMCP::LegacyToolGroups
{
	void RegisterToolGroups();
	void UnregisterToolGroups();
}

class FUEBridgeMCPLegacyToolGroupsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		WorldDataMCP::LegacyToolGroups::RegisterToolGroups();
	}

	virtual void ShutdownModule() override
	{
		WorldDataMCP::LegacyToolGroups::UnregisterToolGroups();
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPLegacyToolGroupsModule, UEBridgeMCPLegacyToolGroups)
