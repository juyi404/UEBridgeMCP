using UnrealBuildTool;

public class WorldDataAgentSecurity : ModuleRules
{
	public WorldDataAgentSecurity(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "WorldDataAgentContracts" });
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Bcrypt.lib");
		}
	}
}
