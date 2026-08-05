using UnrealBuildTool;

public class UEAIDevelopmentBridge : ModuleRules
{
	public UEAIDevelopmentBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		bool bEnabled = Target.Type == TargetType.Game
			&& (Target.Configuration == UnrealTargetConfiguration.Development
				|| Target.Configuration == UnrealTargetConfiguration.DebugGame);
		PublicDefinitions.Add(
			"WITH_UEAI_DEVELOPMENT_BRIDGE=" + (bEnabled ? "1" : "0"));
		PublicDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Json",
			"Projects",
			"SSL"
		});
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("advapi32.lib");
		}
	}
}
