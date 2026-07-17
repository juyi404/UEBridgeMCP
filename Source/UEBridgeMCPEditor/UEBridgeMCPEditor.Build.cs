using UnrealBuildTool;

public class UEBridgeMCPEditor : ModuleRules
{
	public UEBridgeMCPEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ApplicationCore",
			"AppFramework",
			"AssetRegistry",
			"CoreUObject",
			"Engine",
			"HTTPServer",
			"Json",
			"JsonUtilities",
			"Projects",
			"PythonScriptPlugin",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"WorkspaceMenuStructure",
			"UnrealEd"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Used to inspect the owning process's TCP listener after HTTPServer starts.
			// The MCP endpoint fails closed unless Windows reports a loopback-only bind.
			PublicSystemLibraries.Add("Iphlpapi.lib");
			PublicSystemLibraries.Add("Crypt32.lib");
			PublicSystemLibraries.Add("Bcrypt.lib");
		}
	}
}
