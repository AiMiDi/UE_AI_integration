using UnrealBuildTool;

[SupportedPlatforms(UnrealPlatformClass.Desktop)]
public class UEAITraceWorkerTarget : TargetRules
{
	public UEAITraceWorkerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Program;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		// Do not add every dependency subdirectory as a bare include root. In
		// UE 5.3 that makes TraceServices/Model/memory.h shadow the CRT
		// <memory.h> while Core builds SharedPointer/AutoRTFM.
		bLegacyPublicIncludePaths = false;
		bLegacyParentIncludePaths = false;
		LinkType = TargetLinkType.Monolithic;
		LaunchModuleName = "UEAITraceWorker";

		bBuildDeveloperTools = true;
		// ApplicationCore supplies the standard Program bootstrap without
		// bringing in Slate, Engine, or UnrealEd.
		bCompileAgainstApplicationCore = true;
		bCompileAgainstCoreUObject = true;
		bCompileAgainstEngine = false;
		// This process consumes trace events through TraceServices. It does not
		// emit UE Stats, so the Stats macro surface is intentionally disabled.
		bCompileWithStatsWithoutEngine = false;
		bCompileWithPluginSupport = true;
		bIncludePluginsForTargetPlatforms = true;
		// Protocol input/output is UTF-8 JSON and requires no localization data.
		bCompileICU = false;
		bForceDisableAutomationTests = true;
		bHasExports = false;
		bIsBuildingConsoleApplication = true;
		bUseLoggingInShipping = true;
		// The Worker primarily consumes traces, but the release fixture must also
		// emit a real .utrace without an Editor process. Program targets do not
		// enable TraceLog by default in UE 5.3.
		GlobalDefinitions.Add("UE_TRACE_ENABLED=1");

		EnablePlugins.Add("UE_AI_integration");

		// Plugin-hosted Program targets in UE 5.3 do not always receive the
		// project crypto registration definitions that engine Programs get.
		// This worker never mounts encrypted or signed pak files, so register
		// the explicit empty implementations required by IMPLEMENT_APPLICATION.
		ProjectDefinitions.Add("IMPLEMENT_ENCRYPTION_KEY_REGISTRATION()=");
		ProjectDefinitions.Add("IMPLEMENT_SIGNING_KEY_REGISTRATION()=");
	}
}
