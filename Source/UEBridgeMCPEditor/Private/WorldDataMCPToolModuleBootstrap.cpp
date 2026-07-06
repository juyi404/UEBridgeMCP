#include "WorldDataMCPToolRegistry.h"

// The single translation unit that knows the full set of built-in tool groups.
// Every tool-group header is #included here and nowhere in the server core, so the
// core's compile dependency on the tool layer is confined to this file. When a tool
// group is promoted to its own UE module, delete its include + RegisterMCPToolModule
// line here and let that module self-register from its own StartupModule.
#include "UEBridgeMCPExtractedTools.h"
#include "WorldDataMCPEditorTools.h"
#include "WorldDataMCPSceneTools.h"
#include "WorldDataMCPAuthoringTools.h"
#include "WorldDataMCPGraphTools.h"
#include "WorldDataMCPAIAnimTools.h"
#include "WorldDataMCPPcgKnowledgeTools.h"
#include "WorldDataMCPKnowledgeTools.h"
#include "WorldDataMCPStateTreeTools.h"
#include "WorldDataMCPBlueprintTools.h"
#include "WorldDataMCPAnimTools.h"
#include "WorldDataMCPMatLandTools.h"
#include "WorldDataMCPNiagaraTools.h"
#include "WorldDataMCPFoliageTools.h"
#include "WorldDataMCPSequencerTools.h"
#include "WorldDataMCPGameplayTools.h"
#include "WorldDataMCPUIDataTools.h"
#include "WorldDataMCPGasTools.h"
#include "WorldDataMCPMaterialInstanceTools.h"

namespace WorldDataMCP
{
	void RegisterBuiltinMCPToolModules()
	{
		// Priorities ascend in steps of 10, preserving the legacy table order. The order
		// matters for prefix-claiming groups: PcgKnowledge (70) is tried before
		// Knowledge (80) and PCG (90) so it owns its prefixed tool names first.
		RegisterMCPToolModule(10,  &ExtractedTools::GetToolDefinitionsJson,        &ExtractedTools::Dispatch);
		RegisterMCPToolModule(20,  &EditorTools::GetToolDefinitionsJson,           &EditorTools::Dispatch);
		RegisterMCPToolModule(30,  &SceneTools::GetToolDefinitionsJson,            &SceneTools::Dispatch);
		RegisterMCPToolModule(40,  &AuthoringTools::GetToolDefinitionsJson,        &AuthoringTools::Dispatch);
		RegisterMCPToolModule(50,  &GraphTools::GetToolDefinitionsJson,            &GraphTools::Dispatch);
		RegisterMCPToolModule(60,  &AIAnimTools::GetToolDefinitionsJson,           &AIAnimTools::Dispatch);
		RegisterMCPToolModule(70,  &PcgKnowledgeTools::GetToolDefinitionsJson,     &PcgKnowledgeTools::Dispatch);
		RegisterMCPToolModule(80,  &KnowledgeTools::GetToolDefinitionsJson,        &KnowledgeTools::Dispatch);
		// PCGTools (priority 90) now self-registers from the UEBridgeMCPPCGTools module.
		RegisterMCPToolModule(100, &StateTreeTools::GetToolDefinitionsJson,        &StateTreeTools::Dispatch);
		RegisterMCPToolModule(110, &BlueprintTools::GetToolDefinitionsJson,        &BlueprintTools::Dispatch);
		RegisterMCPToolModule(120, &AnimTools::GetToolDefinitionsJson,             &AnimTools::Dispatch);
		RegisterMCPToolModule(130, &MatLandTools::GetToolDefinitionsJson,          &MatLandTools::Dispatch);
		RegisterMCPToolModule(140, &NiagaraTools::GetToolDefinitionsJson,          &NiagaraTools::Dispatch);
		RegisterMCPToolModule(150, &FoliageTools::GetToolDefinitionsJson,          &FoliageTools::Dispatch);
		RegisterMCPToolModule(160, &SequencerTools::GetToolDefinitionsJson,        &SequencerTools::Dispatch);
		RegisterMCPToolModule(170, &GameplayTools::GetToolDefinitionsJson,         &GameplayTools::Dispatch);
		RegisterMCPToolModule(180, &UIDataTools::GetToolDefinitionsJson,           &UIDataTools::Dispatch);
		RegisterMCPToolModule(190, &GasTools::GetToolDefinitionsJson,              &GasTools::Dispatch);
		RegisterMCPToolModule(200, &MaterialInstanceTools::GetToolDefinitionsJson, &MaterialInstanceTools::Dispatch);
		// SpatialTools (priority 210) now self-registers from the UEBridgeMCPSpatialTools module.
	}
}
