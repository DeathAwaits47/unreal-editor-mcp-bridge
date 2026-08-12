// Copyright Natali Caggiano. All Rights Reserved.

using UnrealBuildTool;

public class UnrealClaude : ModuleRules
{
	public UnrealClaude(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		
		PublicIncludePaths.AddRange(
			new string[] {
			}
		);
				
		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);
			
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"Slate",
				"SlateCore",
				"EditorStyle",
				"UnrealEd",
				"ToolMenus",
				"Projects",
				"EditorFramework",
				"WorkspaceMenuStructure"
			}
		);

		// Public UE types used by the editor-side world builder.
		PublicDependencyModuleNames.Add("Foliage");
			
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				// Project Settings -> Plugins -> Unreal Claude (UUnrealClaudeSettings)
				"DeveloperSettings",
				"Json",
				"JsonUtilities",
				"HTTP",
				"HTTPServer",
				"Sockets",
				"Networking",
				"ImageWrapper",
				// Blueprint manipulation
				"Kismet",
				"KismetCompiler",
				"BlueprintGraph",
				"GraphEditor",
				"AssetRegistry",
				"AssetTools",
				// Animation Blueprint manipulation
				"AnimGraph",
				"AnimGraphRuntime",
				// UMG widget-blueprint (designer) editing
				"UMG",
				"UMGEditor",
				// Material graph/node editing
				"MaterialEditor",
				// Asset saving
				"EditorScriptingUtilities",
				// Sequencer automation
				"LevelSequence",
				"MovieScene",
				"MovieSceneTracks",
				"MovieSceneTools",
				"SequencerScripting",
				"ControlRig",
				"ControlRigEditor",
				"Landscape",
				// PIE performance capture
				"RenderCore",
				"RHI",
				// Enhanced Input
				"EnhancedInput"
			}
		);

		// Clipboard support (FPlatformApplicationMisc) on all platforms
		PrivateDependencyModuleNames.Add("ApplicationCore");

		// Windows only
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// LiveCoding is only available in editor builds on Windows
			if (Target.bBuildEditor)
			{
				PrivateDependencyModuleNames.Add("LiveCoding");
			}
		}
	}
}
