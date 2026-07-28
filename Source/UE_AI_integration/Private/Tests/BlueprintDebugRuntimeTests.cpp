#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Runtime/BlueprintDebugService.h"
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintAnalysisTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FBlueprintDebugService* DebugService);
void RegisterBlueprintDebugQueryTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FBlueprintDebugService& Service);
void RegisterBlueprintDebugCommandTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FBlueprintDebugService& Service);
}

namespace
{
using UEAIIntegration::Infrastructure::FBlueprintDebugResult;
using UEAIIntegration::Infrastructure::FBlueprintDebugService;
using UEAIIntegration::Infrastructure::FPIESessionController;

TSharedRef<FJsonObject> MakeSessionParams(
	const FString& SessionId,
	uint64 Generation,
	const FString& DebugSessionId = FString())
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);
	Params->SetNumberField(TEXT("generation"), static_cast<double>(Generation));
	if (!DebugSessionId.IsEmpty())
	{
		Params->SetStringField(TEXT("debugSessionId"), DebugSessionId);
	}
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintDebugRegistrarTest,
	"UE_AI_integration.BlueprintDebug.RegistersExactlyTenCapabilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDebugRegistrarTest::RunTest(const FString& Parameters)
{
	FPIESessionController PIEController;
	FBlueprintDebugService Service(PIEController);
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintDebugQueryTools(Registry, Service);
	UEAIIntegrationTools::RegisterBlueprintDebugCommandTools(Registry, Service);
	Registry.EndDomainRegistration();

	TestEqual(TEXT("Exactly ten Blueprint debug tools register"), Registry.Num(), 10);
	static const TCHAR* Expected[] = {
		TEXT("blueprint.debug.session.get"),
		TEXT("blueprint.debug.trace.get"),
		TEXT("blueprint.debug.breakpoint.list"),
		TEXT("blueprint.debug.breakpoint.set"),
		TEXT("blueprint.debug.breakpoint.remove"),
		TEXT("blueprint.debug.watch.list"),
		TEXT("blueprint.debug.watch.set"),
		TEXT("blueprint.debug.watch.remove"),
		TEXT("blueprint.debug.watch.value.get"),
		TEXT("blueprint.debug.control"),
	};
	for (const TCHAR* Capability : Expected)
	{
		TestNotNull(Capability, Registry.FindTool(Capability));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintDebugPausedQueueContractTest,
	"UE_AI_integration.BlueprintDebug.PausedQueueAndImmutableSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDebugPausedQueueContractTest::RunTest(const FString& Parameters)
{
	FPIESessionController PIEController;
	FBlueprintDebugService Service(PIEController);
	const FString SessionId = TEXT("pie-test-session");
	const FString DebugSessionId = TEXT("bpdebug-test-session");
	constexpr uint64 Generation = 7;
	Service.SetSessionForTesting(
		SessionId,
		Generation,
		DebugSessionId,
		true,
		true);

	TSharedRef<FJsonObject> SessionParams =
		MakeSessionParams(SessionId, Generation);
	const FBlueprintDebugResult Session = Service.GetSession(SessionParams);
	TestTrue(TEXT("Copied paused session snapshot is readable"), Session.bSuccess);
	if (Session.bSuccess)
	{
		TestTrue(TEXT("Snapshot reports paused state"), Session.Data->GetBoolField(TEXT("paused")));
		TestEqual(
			TEXT("Debug session identity is published"),
			Session.Data->GetStringField(TEXT("debugSessionId")),
			DebugSessionId);
	}

	TSharedRef<FJsonObject> ControlParams =
		MakeSessionParams(SessionId, Generation, DebugSessionId);
	ControlParams->SetStringField(TEXT("action"), TEXT("stepOver"));
	FBlueprintDebugResult Control;
	TestTrue(
		TEXT("Paused transport recognizes Blueprint control"),
		Service.TryHandlePausedRequest(
			TEXT("blueprint.debug.control"),
			ControlParams,
			TEXT("request-debug-1"),
			Control));
	TestTrue(TEXT("Paused control is accepted without UObject access"), Control.bSuccess);
	if (Control.bSuccess)
	{
		TestEqual(
			TEXT("Envelope requestId becomes stable commandId"),
			Control.Data->GetStringField(TEXT("commandId")),
			FString(TEXT("request-debug-1")));
		TestTrue(TEXT("Control remains queued for Slate pre-tick"), Control.Data->GetBoolField(TEXT("queued")));
	}

	FBlueprintDebugResult Replay;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.control"),
		ControlParams,
		TEXT("request-debug-1"),
		Replay);
	TestTrue(TEXT("Identical command replay is idempotent"), Replay.bSuccess);

	TSharedRef<FJsonObject> ConflictingParams =
		MakeSessionParams(SessionId, Generation, DebugSessionId);
	ConflictingParams->SetStringField(TEXT("action"), TEXT("abort"));
	FBlueprintDebugResult Conflict;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.control"),
		ConflictingParams,
		TEXT("request-debug-1"),
		Conflict);
	TestFalse(TEXT("Conflicting command replay is rejected"), Conflict.bSuccess);
	TestEqual(
		TEXT("Conflict uses shared idempotency error"),
		Conflict.ErrorCode,
		FString(TEXT("idempotency_conflict")));

	TSharedRef<FJsonObject> BreakpointParams =
		MakeSessionParams(SessionId, Generation, DebugSessionId);
	BreakpointParams->SetStringField(
		TEXT("blueprint"),
		TEXT("/Game/Automation/BP_DebugQueue"));
	BreakpointParams->SetStringField(
		TEXT("nodeGuid"),
		TEXT("0123456789abcdef0123456789abcdef"));
	BreakpointParams->SetBoolField(TEXT("enabled"), true);
	FBlueprintDebugResult BreakpointAccepted;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.breakpoint.set"),
		BreakpointParams,
		TEXT("request-breakpoint-1"),
		BreakpointAccepted);
	TestTrue(
		TEXT("Paused breakpoint write inherits envelope requestId"),
		BreakpointAccepted.bSuccess
			&& BreakpointAccepted.Data->GetStringField(TEXT("commandId"))
				== TEXT("request-breakpoint-1"));
	FBlueprintDebugResult BreakpointReplay;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.breakpoint.set"),
		BreakpointParams,
		TEXT("request-breakpoint-1"),
		BreakpointReplay);
	TestTrue(TEXT("Paused breakpoint replay is idempotent"), BreakpointReplay.bSuccess);
	BreakpointParams->SetBoolField(TEXT("enabled"), false);
	FBlueprintDebugResult BreakpointConflict;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.breakpoint.set"),
		BreakpointParams,
		TEXT("request-breakpoint-1"),
		BreakpointConflict);
	TestFalse(
		TEXT("Paused breakpoint requestId rejects a changed payload"),
		BreakpointConflict.bSuccess);
	TestEqual(
		TEXT("Paused breakpoint conflict has stable error"),
		BreakpointConflict.ErrorCode,
		FString(TEXT("idempotency_conflict")));

	TSharedRef<FJsonObject> WatchParams =
		MakeSessionParams(SessionId, Generation, DebugSessionId);
	WatchParams->SetStringField(
		TEXT("blueprint"),
		TEXT("/Game/Automation/BP_DebugQueue"));
	WatchParams->SetStringField(
		TEXT("nodeGuid"),
		TEXT("0123456789abcdef0123456789abcdef"));
	WatchParams->SetStringField(
		TEXT("pinGuid"),
		TEXT("fedcba9876543210fedcba9876543210"));
	FBlueprintDebugResult WatchAccepted;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.watch.remove"),
		WatchParams,
		TEXT("request-watch-1"),
		WatchAccepted);
	TestTrue(
		TEXT("Paused watch write inherits envelope requestId"),
		WatchAccepted.bSuccess
			&& WatchAccepted.Data->GetStringField(TEXT("commandId"))
				== TEXT("request-watch-1"));
	FBlueprintDebugResult WatchReplay;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.watch.remove"),
		WatchParams,
		TEXT("request-watch-1"),
		WatchReplay);
	TestTrue(TEXT("Paused watch replay is idempotent"), WatchReplay.bSuccess);
	WatchParams->SetStringField(
		TEXT("pinGuid"),
		TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	FBlueprintDebugResult WatchConflict;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.watch.remove"),
		WatchParams,
		TEXT("request-watch-1"),
		WatchConflict);
	TestFalse(
		TEXT("Paused watch requestId rejects a changed payload"),
		WatchConflict.bSuccess);
	TestEqual(
		TEXT("Paused watch conflict has stable error"),
		WatchConflict.ErrorCode,
		FString(TEXT("idempotency_conflict")));

	Service.AddTraceForTesting(1, TEXT("bpnode-one"));
	Service.AddTraceForTesting(2, TEXT("bpnode-two"));
	Service.AddTraceForTesting(3, TEXT("bpnode-three"));
	TSharedRef<FJsonObject> TraceParams =
		MakeSessionParams(SessionId, Generation, DebugSessionId);
	TraceParams->SetStringField(TEXT("cursor"), TEXT("1"));
	TraceParams->SetNumberField(TEXT("limit"), 1);
	const FBlueprintDebugResult TracePage = Service.GetTrace(TraceParams);
	TestTrue(TEXT("Bounded trace page succeeds"), TracePage.bSuccess);
	if (TracePage.bSuccess)
	{
		TestEqual(
			TEXT("Trace page honors limit"),
			TracePage.Data->GetArrayField(TEXT("events")).Num(),
			1);
		TestEqual(
			TEXT("Trace page returns opaque next cursor"),
			TracePage.Data->GetStringField(TEXT("nextCursor")),
			FString(TEXT("2")));
		TestTrue(
			TEXT("Trace page reports remaining events"),
			TracePage.Data->GetBoolField(TEXT("hasMore")));
	}
	TSet<FString> Observed;
	const FBlueprintDebugResult Evidence = Service.CollectObservedNodeIds(
		DebugSessionId,
		TEXT("2"),
		TEXT("3"),
		Observed);
	TestTrue(TEXT("Trace range resolves real node evidence"), Evidence.bSuccess);
	TestEqual(TEXT("Trace range is inclusive and bounded"), Observed.Num(), 2);
	TestTrue(TEXT("Trace range contains second node"), Observed.Contains(TEXT("bpnode-two")));
	TestTrue(TEXT("Trace range contains third node"), Observed.Contains(TEXT("bpnode-three")));

	for (uint64 Cursor = 4; Cursor <= 4100; ++Cursor)
	{
		Service.AddTraceForTesting(
			Cursor,
			FString::Printf(TEXT("bpnode-%llu"), Cursor));
	}
	TestEqual(
		TEXT("Trace dedupe keys remain bounded with retained records"),
		Service.GetTraceDedupeKeyCountForTesting(),
		4096);
	TSharedRef<FJsonObject> RetentionParams =
		MakeSessionParams(SessionId, Generation, DebugSessionId);
	RetentionParams->SetStringField(TEXT("cursor"), TEXT("0"));
	RetentionParams->SetNumberField(TEXT("limit"), 1);
	const FBlueprintDebugResult Retention =
		Service.GetTrace(RetentionParams);
	TestTrue(TEXT("Bounded retained trace remains queryable"), Retention.bSuccess);
	if (Retention.bSuccess)
	{
		TestEqual(
			TEXT("Only the bounded trace window remains"),
			static_cast<int32>(Retention.Data->GetNumberField(TEXT("total"))),
			4096);
	}
	const int32 ScanCountBeforeInactiveTick =
		Service.GetBlueprintScanCountForTesting();
	Service.Tick();
	TestEqual(
		TEXT("Stopped PIE skips the project-wide Blueprint settings scan"),
		Service.GetBlueprintScanCountForTesting(),
		ScanCountBeforeInactiveTick);
	TestEqual(
		TEXT("Stopped PIE clears retained trace and its dedupe index"),
		Service.GetTraceDedupeKeyCountForTesting(),
		0);

	Service.SetSessionForTesting(
		TEXT("pie-next-session"),
		Generation + 1,
		TEXT("bpdebug-next-session"),
		true,
		false);
	const FBlueprintDebugResult Stale = Service.GetSession(SessionParams);
	TestFalse(TEXT("Old PIE generation is rejected"), Stale.bSuccess);
	TestEqual(
		TEXT("Old generation has stable error"),
		Stale.ErrorCode,
		FString(TEXT("stale_session_handle")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintDebugCorrelationProviderTest,
	"UE_AI_integration.BlueprintDebug.CorrelationUsesDebugSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDebugCorrelationProviderTest::RunTest(const FString& Parameters)
{
	FPIESessionController PIEController;
	FBlueprintDebugService Service(PIEController);
	Service.SetSessionForTesting(
		TEXT("pie-correlation"),
		3,
		TEXT("bpdebug-correlation"),
		true,
		false);

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintAnalysisTools(Registry, &Service);
	Registry.EndDomainRegistration();

	TSharedRef<FJsonObject> ScanParams = MakeShared<FJsonObject>();
	ScanParams->SetStringField(
		TEXT("pathPrefix"),
		TEXT("/Game/__UEAIBlueprintDebugMissing"));
	ScanParams->SetNumberField(TEXT("assetLimit"), 1);
	ScanParams->SetNumberField(TEXT("findingLimit"), 1);
	const FMCPToolResult Scan =
		Registry.ExecuteTool(TEXT("blueprint.scan"), ScanParams);
	TestTrue(TEXT("Empty bounded scan succeeds"), Scan.bSuccess);
	if (!Scan.bSuccess)
	{
		return false;
	}

	TSharedRef<FJsonObject> Correlate = MakeShared<FJsonObject>();
	Correlate->SetStringField(TEXT("scanId"), Scan.Data->GetStringField(TEXT("scanId")));
	Correlate->SetStringField(TEXT("runId"), TEXT("run-correlation"));
	Correlate->SetStringField(TEXT("debugSessionId"), TEXT("bpdebug-correlation"));
	TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
	Range->SetStringField(TEXT("cursorStart"), TEXT("0"));
	Range->SetStringField(TEXT("cursorEnd"), TEXT("100"));
	Correlate->SetObjectField(TEXT("traceRange"), Range);
	const FMCPToolResult Result =
		Registry.ExecuteTool(TEXT("blueprint.findings.correlate"), Correlate);
	TestTrue(TEXT("Real debug-session provider is accepted"), Result.bSuccess);
	if (Result.bSuccess)
	{
		TestEqual(
			TEXT("Provider produces a correlated result even with an empty trace"),
			Result.Data->GetStringField(TEXT("status")),
			FString(TEXT("correlated")));
	}

	Correlate->SetStringField(TEXT("debugSessionId"), TEXT("bpdebug-stale"));
	const FMCPToolResult Stale =
		Registry.ExecuteTool(TEXT("blueprint.findings.correlate"), Correlate);
	TestFalse(TEXT("Unknown debug session is rejected"), Stale.bSuccess);
	TestEqual(
		TEXT("Unknown debug session has stable error"),
		Stale.ErrorCode,
		FString(TEXT("debug_session_not_found")));
	return true;
}

#endif
