using UnrealBuildTool;

public class UEAITraceRuntime : ModuleRules
{
	public UEAITraceRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		bool bEnabledTarget =
			Target.Type == TargetType.Game
			&& (Target.Configuration == UnrealTargetConfiguration.Development
				|| Target.Configuration == UnrealTargetConfiguration.DebugGame);
		PublicDefinitions.Add(
			"WITH_UEAI_TRACE_RUNTIME=" + (bEnabledTarget ? "1" : "0"));

		PublicDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Json",
			// Net Trace must be armed only after the managed file connection
			// exists so its version/init event is present in the .utrace.
			"NetCore",
			// Low-level writer drain after FTraceAuxiliary::Stop.
			"TraceLog"
		});
	}
}
