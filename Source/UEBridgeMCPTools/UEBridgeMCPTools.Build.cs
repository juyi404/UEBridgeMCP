using UnrealBuildTool;

public class UEBridgeMCPTools : ModuleRules
{
	public UEBridgeMCPTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetRegistry",
			"CoreUObject",
			"Engine",
			"Json",
			"JsonUtilities",
			"Projects",
			"PythonScriptPlugin",
			"UEBridgeMCPCore",
			"UnrealEd"
		});
	}
}
