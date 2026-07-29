using UnrealBuildTool;
using System.IO;

public class UE_AI_integration : ModuleRules
{
	public UE_AI_integration(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;
		bEnableExceptions = true;

		PrivateIncludePaths.Add(
			Path.Combine(ModuleDirectory, "..", "..", "Workflow", "include"));
		PrivateIncludePaths.Add(
			Path.Combine(ModuleDirectory, "..", "..", "Workflow", "ThirdParty"));

		PublicDefinitions.Add("UE_AI_INTEGRATION_VERSION=\"0.6.0\"");

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"UnrealEd",
			"BlueprintGraph",
			"Json",
			"JsonUtilities",
			"HTTP",
			"HTTPServer",
			"Sockets",
			"Networking",
			"DeveloperSettings"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// Asset system
			"AssetRegistry",
			"AssetTools",

			// Blueprint editing
			"GraphEditor",
			"Kismet",
			"KismetCompiler",
			"EditorSubsystem",

			// Materials
			"MaterialEditor",
			"RHI",
			"RenderCore",

			// Animation
			"AnimGraph",
			"AnimGraphRuntime",

			// UI / Slate
			"Slate",
			"SlateCore",
			"ApplicationCore",
			"AutomationDriver",
			"UMG",
			"UMGEditor",
			"LevelEditor",
			"ToolMenus",
			"Settings",

			// Enhanced Input
			"EnhancedInput",
			"InputBlueprintNodes",

			// Viewport capture
			"ImageWrapper",

			// Sequencer
			"LevelSequence",
			"LevelSequenceEditor",
			"MovieScene",
			"MovieSceneTracks",
			"Sequencer",

			// AI
			"AIModule",
			"GameplayTasks",
			"NavigationSystem",

			// Niagara
			"NiagaraCore",
			"Niagara",
			"NiagaraEditor",

			// Foliage
			"Foliage",

			// Landscape
			"Landscape",

			// Scripting utilities
			"EditorScriptingUtilities",

			// Plugin descriptor/version lookup
			"Projects",

			// Bounded .utrace provider analysis
			"TraceServices",

			// Cross-version SHA-256 implementation
			"SSL"
		});
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");

		// Windows-only: Live Coding support
		if (Target.Platform == UnrealTargetPlatform.Win64 && Target.bBuildEditor)
		{
			PrivateDependencyModuleNames.Add("LiveCoding");
			PublicSystemLibraries.AddRange(new string[]
			{
				"gdi32.lib",
				"user32.lib"
			});
			PublicDefinitions.Add("WITH_LIVE_CODING=1");
		}
		else
		{
			PublicDefinitions.Add("WITH_LIVE_CODING=0");
		}
	}
}
