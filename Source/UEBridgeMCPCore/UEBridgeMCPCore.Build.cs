using UnrealBuildTool;

// Lightweight core for the World Data MCP bridge: the tool registry and the shared
// JSON/project helpers. Deliberately depends on as little as possible (no editor,
// Slate, PCG, Niagara, etc.) so tool-group modules and the server module can share
// this without pulling each other's heavy engine dependencies.
public class UEBridgeMCPCore : ModuleRules
{
	public UEBridgeMCPCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Json"
		});
	}
}
