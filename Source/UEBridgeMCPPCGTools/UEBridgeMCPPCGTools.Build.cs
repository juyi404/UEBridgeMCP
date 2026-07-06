using UnrealBuildTool;

// PCG authoring tools for the World Data MCP bridge. Split out of UEBridgeMCPEditor so
// the heavy PCG / PCGEditor dependencies live only here: a break in PCG's API only
// rebuilds this module, and the server module no longer pulls PCG transitively.
public class UEBridgeMCPPCGTools : ModuleRules
{
	public UEBridgeMCPPCGTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UEBridgeMCPCore",
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"UnrealEd",
			"AssetRegistry",
			"AssetTools",
			"EditorScriptingUtilities",
			"PCG",
			"PCGEditor"
		});
	}
}
