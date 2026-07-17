using UnrealBuildTool;

public class WorldDataAgentUI : ModuleRules
{
	public WorldDataAgentUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "SlateCore" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Json",
			"Projects",
			"Slate",
			"WebBrowser",
			"WorldDataAgentBootstrap",
			"WorldDataAgentContracts"
		});
	}
}
