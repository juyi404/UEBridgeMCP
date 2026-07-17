using UnrealBuildTool;

public class WorldDataAgentUI : ModuleRules
{
	public WorldDataAgentUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "SlateCore" });
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"HTTP",
			"Json",
			"Projects",
			"Slate",
			"UEBridgeMCPCore",
			"WebBrowser",
			"WorldDataAgentBootstrap",
			"WorldDataAgentContracts"
		});

		// These files are loaded by the embedded browser at runtime rather than by
		// Unreal's asset system. Declare each one explicitly so an installed plugin
		// cannot end up with a loadable index page whose styles or bridge script are
		// absent.
		RuntimeDependencies.Add("$(PluginDir)/Resources/Web/index.html", StagedFileType.UFS);
		RuntimeDependencies.Add("$(PluginDir)/Resources/Web/console.css", StagedFileType.UFS);
		RuntimeDependencies.Add("$(PluginDir)/Resources/Web/layout.css", StagedFileType.UFS);
		RuntimeDependencies.Add("$(PluginDir)/Resources/Web/message.css", StagedFileType.UFS);
		RuntimeDependencies.Add("$(PluginDir)/Resources/Web/app.js", StagedFileType.UFS);
	}
}
