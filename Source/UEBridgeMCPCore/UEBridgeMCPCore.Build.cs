using UnrealBuildTool;

public class UEBridgeMCPCore : ModuleRules
{
	public UEBridgeMCPCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "HTTPServer" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"Projects"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Iphlpapi.lib");
			PublicSystemLibraries.Add("Crypt32.lib");
			PublicSystemLibraries.Add("Bcrypt.lib");
		}
	}
}
