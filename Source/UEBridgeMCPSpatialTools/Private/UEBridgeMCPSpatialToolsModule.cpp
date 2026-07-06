#include "Modules/ModuleManager.h"

#include "WorldDataMCPToolRegistry.h"
#include "WorldDataMCPSpatialTools.h"

// Self-registering spatial-tools module, promoted out of UEBridgeMCPEditor. Registration order
// across modules does not matter: the core registry stable-sorts by Priority, so 210 keeps the
// SpatialTools group in its legacy slot (last). StartupModule runs during editor init, long
// before any MCP client connects.
class FUEBridgeMCPSpatialToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		WorldDataMCP::RegisterMCPToolModule(
			210,
			&WorldDataMCP::SpatialTools::GetToolDefinitionsJson,
			&WorldDataMCP::SpatialTools::Dispatch);
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPSpatialToolsModule, UEBridgeMCPSpatialTools)
