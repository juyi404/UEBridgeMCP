using UnrealBuildTool;

public class WorldDataAgentContracts : ModuleRules
{
	public WorldDataAgentContracts(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
	}
}
