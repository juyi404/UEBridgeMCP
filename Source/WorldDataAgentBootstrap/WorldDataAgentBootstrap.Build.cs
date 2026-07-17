using UnrealBuildTool;

public class WorldDataAgentBootstrap : ModuleRules
{
	public WorldDataAgentBootstrap(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "WorldDataAgentContracts" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"WorldDataAgentClient",
			"WorldDataAgentDiagnostics",
			"WorldDataAgentRuntime",
			"WorldDataAgentSecurity"
		});
	}
}
