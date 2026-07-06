// Copyright uuuuzz 2024-2026. All Rights Reserved.

using UnrealBuildTool;

public class UEBridgeMCP : ModuleRules
{
	public UEBridgeMCP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"JsonUtilities"
		});
	}
}
