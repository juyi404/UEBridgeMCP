using UnrealBuildTool;

public class UEBridgeMCPAIDirectorTools : ModuleRules
{
	public UEBridgeMCPAIDirectorTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UEBridgeMCPCore",
			"CoreUObject",
			"Engine",
			"Json",
			"UnrealEd",
			"AssetRegistry",
			"EditorScriptingUtilities",
			"LevelSequence",
			"AIDirectorCore",
			"AIDirectorSequencer",
			"AIDirectorExport"
		});
	}
}
