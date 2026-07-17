using UnrealBuildTool;

public class WorldDataAgentDiagnostics : ModuleRules
{
	public WorldDataAgentDiagnostics(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "WorldDataAgentContracts" });
		PrivateDependencyModuleNames.AddRange(new string[] { "Json" });
	}
}
