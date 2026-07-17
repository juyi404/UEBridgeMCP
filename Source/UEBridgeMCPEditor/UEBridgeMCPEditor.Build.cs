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
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"Projects",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"UEBridgeMCPCore",
			"UEBridgeMCPTools",
			"WorkspaceMenuStructure",
			"UnrealEd"
		});
	}
}
