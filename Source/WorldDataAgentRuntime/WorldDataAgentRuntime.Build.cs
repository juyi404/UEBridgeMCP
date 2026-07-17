using UnrealBuildTool;

public class WorldDataAgentRuntime : ModuleRules
{
	public WorldDataAgentRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "WorldDataAgentContracts" });
		PrivateDependencyModuleNames.AddRange(new string[] { "Json", "Projects" });
	}
}
