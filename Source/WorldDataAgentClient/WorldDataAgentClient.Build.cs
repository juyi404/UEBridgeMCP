using UnrealBuildTool;

public class WorldDataAgentClient : ModuleRules
{
	public WorldDataAgentClient(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "WorldDataAgentContracts" });
		PrivateDependencyModuleNames.AddRange(new string[] { "Json" });
	}
}
