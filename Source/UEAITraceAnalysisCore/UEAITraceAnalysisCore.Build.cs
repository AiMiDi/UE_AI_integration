using UnrealBuildTool;

public class UEAITraceAnalysisCore : ModuleRules
{
	public UEAITraceAnalysisCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"TraceServices"
		});
	}
}
