#include "Modules/ModuleManager.h"

#include "WorldDataMCPAIDirectorTools.h"
#include "WorldDataMCPToolRegistry.h"

class FUEBridgeMCPAIDirectorToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		WorldDataMCP::RegisterMCPToolModule(
			220,
			&WorldDataMCP::AIDirectorTools::GetToolDefinitionsJson,
			&WorldDataMCP::AIDirectorTools::Dispatch);
	}
};

IMPLEMENT_MODULE(FUEBridgeMCPAIDirectorToolsModule, UEBridgeMCPAIDirectorTools)
