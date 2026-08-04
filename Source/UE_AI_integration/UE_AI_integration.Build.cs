using UnrealBuildTool;
using System;
using System.IO;
using System.Linq;

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

		PublicDefinitions.Add("UE_AI_INTEGRATION_VERSION=\"0.9.0\"");

		bool bWithNiagara = IsOptionalFeatureEnabled(Target, "Niagara");
		bool bWithWater = IsOptionalFeatureEnabled(Target, "Water");
		bool bWithPCG = IsOptionalFeatureEnabled(Target, "PCG");
		PublicDefinitions.Add("WITH_UEAI_NIAGARA=" + (bWithNiagara ? "1" : "0"));
		PublicDefinitions.Add("WITH_UEAI_WATER=" + (bWithWater ? "1" : "0"));
		PublicDefinitions.Add("WITH_UEAI_PCG=" + (bWithPCG ? "1" : "0"));

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
			// Shared, UI-independent TraceServices semantic query layer.
			"UEAITraceAnalysisCore",

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

			// Viewport capture
			"ImageWrapper",

			// Sequencer
			"LevelSequence",
			"MovieScene",
			"MovieSceneTracks",
			"Sequencer",

			// AI
			"AIModule",
			"GameplayTasks",
			"NavigationSystem",

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
			// Re-arm Net Trace after the managed file transport connects.
			"NetCore",
			// Low-level writer drain after FTraceAuxiliary::Stop.
			"TraceLog",

			// Cross-version SHA-256 implementation
			"SSL"
		});

		if (bWithNiagara)
		{
			PrivateDependencyModuleNames.AddRange(new string[]
			{
				"NiagaraCore",
				"Niagara",
				"NiagaraEditor"
			});
		}
		if (bWithWater)
		{
			PrivateDependencyModuleNames.Add("Water");
		}
		// PCG handlers use reflection so the production module never needs to
		// link PCG. WITH_UEAI_PCG only enables the typed Automation coverage.
		if (bWithPCG)
		{
			PrivateDependencyModuleNames.Add("PCG");
		}
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

	private static bool IsOptionalFeatureEnabled(
		ReadOnlyTargetRules Target,
		string FeatureName)
	{
		string FeatureOverride = Environment.GetEnvironmentVariable(
			"UEAI_OPTIONAL_FEATURES") ?? String.Empty;
		string[] RequestedFeatures = FeatureOverride.Split(
			new char[] { ',', ';' },
			StringSplitOptions.RemoveEmptyEntries);
		if (RequestedFeatures.Any(
			Feature => Feature.Trim().Equals("all", StringComparison.OrdinalIgnoreCase)
				|| Feature.Trim().Equals(FeatureName, StringComparison.OrdinalIgnoreCase)))
		{
			return true;
		}

		if (Target.DisablePlugins.Any(
			Plugin => Plugin.Equals(FeatureName, StringComparison.OrdinalIgnoreCase)))
		{
			return false;
		}
		if (Target.EnablePlugins.Any(
			Plugin => Plugin.Equals(FeatureName, StringComparison.OrdinalIgnoreCase)))
		{
			return true;
		}

		if (Target.ProjectFile != null)
		{
			ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
			PluginReferenceDescriptor Reference = Project.Plugins == null
				? null
				: Project.Plugins.LastOrDefault(
					Plugin => Plugin.Name.Equals(
						FeatureName,
						StringComparison.OrdinalIgnoreCase));
			return Reference != null && Reference.bEnabled;
		}

		return false;
	}
}
