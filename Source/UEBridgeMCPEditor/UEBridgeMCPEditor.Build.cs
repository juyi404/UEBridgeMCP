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
			"UEBridgeMCPCore",
			"UEBridgeMCPSpatialTools",
			"AIModule",
			"AnimGraph",
			"AnimationEditor",
			"ApplicationCore",
			"AppFramework",
			"AssetRegistry",
			"AssetTools",
			"BlueprintEditorLibrary",
			"BlueprintGraph",
			"ContentBrowser",
			"ControlRig",
			"DataValidation",
			"DesktopPlatform",
			"EditorScriptingUtilities",
			"EditorStyle",
			"EditorSubsystem",
			"EditorWidgets",
			"EnhancedInput",
			"Foliage",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"CoreUObject",
			"Engine",
			"HTTP",
			"HTTPServer",
			"IKRig",
			"InputCore",
			"Json",
			"JsonUtilities",
			"Kismet",
			"KismetCompiler",
			"Landscape",
			"LevelEditor",
			"LevelSequence",
			"MaterialEditor",
			"MeshDescription",
			"MovieScene",
			"MovieSceneTracks",
			"NavigationSystem",
			"Niagara",
			"NiagaraEditor",
			// PCG / PCGEditor moved to the UEBridgeMCPPCGTools module.
			"PropertyBindingUtils",
			"PropertyEditor",
			"Projects",
			"PythonScriptPlugin",
			"RenderCore",
			"RHI",
			"Sequencer",
			"Slate",
			"SlateCore",
			"StateTreeEditorModule",
			"StateTreeModule",
			"StaticMeshDescription",
			"SubobjectDataInterface",
			"ToolMenus",
			"UMG",
			"UMGEditor",
			"WorkspaceMenuStructure",
			"UnrealEd"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
