#include "Modules/ModuleManager.h"

#include "WorldDataMCPToolRegistry.h"
#include "WorldDataMCPPCGTools.h"

// Self-registering PCG tool module. Registration order across modules does not matter:
// the core registry stable-sorts by Priority, so 90 keeps the PCG group in its legacy
// slot (after PcgKnowledge=70 / Knowledge=80, before StateTree=100). All tool modules
// finish StartupModule during editor init, long before any MCP client connects.
class FUEBridgeMCPPCGToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		WorldDataMCP::RegisterMCPToolModule(
			90,
			&WorldDataMCP::PCGTools::GetToolDefinitionsJson,
			&WorldDataMCP::PCGTools::Dispatch);
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPPCGToolsModule, UEBridgeMCPPCGTools)
