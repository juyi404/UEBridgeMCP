using UnrealBuildTool;

// Restored editor-authoring tool groups.  They live independently from the
// lightweight UEBridgeMCPTools module so the broad UE editor API surface stays
// isolated and can be maintained as a coherent compatibility layer.
public class UEBridgeMCPLegacyToolGroups : ModuleRules
{
	public UEBridgeMCPLegacyToolGroups(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UEBridgeMCPCore",
			"UEBridgeMCPTools",
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
			"CoreUObject",
			"DataValidation",
			"DesktopPlatform",
			"EditorScriptingUtilities",
			"EditorStyle",
			"EditorSubsystem",
			"EditorWidgets",
			"EnhancedInput",
			"Engine",
			"Foliage",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"HTTP",
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
			"PCG",
			"PCGEditor",
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
			"UnrealEd",
			"WorkspaceMenuStructure"
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
		}
	}
}
