using UnrealBuildTool;

// Spatial-perception + placement tools for the World Data MCP bridge. Split out of
// UEBridgeMCPEditor so the large SpatialTools translation unit (~3k lines: scene digest,
// region/nearest queries, scene-map capture, placement/clearance, the unified placed-object
// model) builds independently and the editor server module shrinks. Self-registers via its
// StartupModule. Dependency list is the subset SpatialTools actually needs (trim/extend as the
// single-file compile dictates) — Core stays public because the public header exposes FBox/FString.
public class UEBridgeMCPSpatialTools : ModuleRules
{
	public UEBridgeMCPSpatialTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UEBridgeMCPCore",
			"CoreUObject",
			"Engine",
			"Json",
			"UnrealEd",
			"NavigationSystem",
			"Foliage",
			"LevelEditor",
			"RenderCore",
			"RHI",
			"EditorScriptingUtilities"
		});
	}
}
