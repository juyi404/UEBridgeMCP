using UnrealBuildTool;

public class UEBridgeMCPEditor : ModuleRules
{
	public UEBridgeMCPEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AppFramework",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UEBridgeMCPCore",
			"UEBridgeMCPTools",
			"WorldDataAgentBootstrap",
			"WorldDataAgentContracts",
			"WorldDataAgentUI",
			"WorkspaceMenuStructure",
			"UnrealEd"
		});
	}
}
