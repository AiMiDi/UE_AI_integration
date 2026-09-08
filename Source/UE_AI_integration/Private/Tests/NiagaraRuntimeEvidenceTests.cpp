#if WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA
#include "Misc/AutomationTest.h"
#include "Infrastructure/PIESessionController.h"
#include "Tools/MCPToolRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "NiagaraDataInterfaceAsyncGpuTrace.h"
#include "UObject/StrongObjectPtr.h"
#if __has_include("NiagaraAsyncGpuTraceDiagnostics.h")
#include "NiagaraAsyncGpuTraceDiagnostics.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraRuntimeTools(FMCPToolRegistry&, UEAIIntegration::Infrastructure::FPIESessionController&);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraRuntimePreflightTest,
	"UE_AI_integration.Niagara.Runtime.Preflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNiagaraRuntimePreflightTest::RunTest(const FString&)
{
	UEAIIntegration::Infrastructure::FPIESessionController Controller;
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRuntimeTools(Registry, Controller);
	Registry.EndDomainRegistration();
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("world"), TEXT("/MissingEvidenceWorld.NoWorld"));
	Params->SetStringField(TEXT("dataInterface"), TEXT("/MissingEvidenceSystem.NoDI"));
	auto* InspectTool = Registry.FindTool(TEXT("content.niagara.runtime.inspect"));
	if (!TestNotNull(TEXT("Inspection registered"), InspectTool)) return false;
	const auto Inspect = InspectTool->Execute(Params);
	TestEqual(TEXT("Inspection does not silently change world"), Inspect.ErrorCode, FString(TEXT("world_not_found")));
	int32 Completions = 0;
	FString Code;
	auto* Capture = Registry.FindTool(TEXT("content.niagara.runtime.capture"));
	if (!TestNotNull(TEXT("Capture registered"), Capture)) return false;
	Capture->BeginExecuteAsync(Params, [&](FMCPToolResult&& Result) { ++Completions; Code = Result.ErrorCode; });
	Capture->CancelAsyncExecution(TEXT("lateCancel"));
	TestEqual(TEXT("Preflight completes exactly once"), Completions, 1);
#if defined(NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION)
	TestEqual(TEXT("Missing paths rejected without loading assets"), Code, FString(TEXT("runtime_target_not_found")));
#else
	TestEqual(TEXT("Stock engine has explicit unavailable result"), Code, FString(TEXT("niagara_runtime_evidence_unavailable")));
#endif
	return true;
}

#if defined(NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraRuntimeCancellationTest,
	"UE_AI_integration.Niagara.Runtime.Cancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNiagaraRuntimeCancellationTest::RunTest(const FString&)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestTrue(TEXT("Editor world with scene required"), World && World->Scene)) return false;
	// This transient DI has no dispatch, so cancellation exercises a pending
	// request deterministically without touching a project asset or starting PIE.
	TStrongObjectPtr<UNiagaraDataInterfaceAsyncGpuTrace> DI(NewObject<UNiagaraDataInterfaceAsyncGpuTrace>());
	UEAIIntegration::Infrastructure::FPIESessionController Controller;
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRuntimeTools(Registry, Controller);
	Registry.EndDomainRegistration();
	auto* Tool = Registry.FindTool(TEXT("content.niagara.runtime.capture"));
	if (!TestNotNull(TEXT("Capture registered"), Tool)) return false;
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("world"), World->GetPathName());
	Params->SetStringField(TEXT("dataInterface"), DI->GetPathName());
	int32 Completions = 0;
	FString Code;
	Tool->BeginExecuteAsync(Params, [&](FMCPToolResult&& Result) { ++Completions; Code = Result.ErrorCode; });
	TestEqual(TEXT("Capture waits for a matching render dispatch"), Completions, 0);
	FString BusyCode;
	Tool->BeginExecuteAsync(Params, [&](FMCPToolResult&& Result) { BusyCode = Result.ErrorCode; });
	TestEqual(TEXT("Second caller cannot replace active capture"), BusyCode, FString(TEXT("capture_busy")));
	Tool->CancelAsyncExecution(TEXT("testCancellation"));
	Tool->CancelAsyncExecution(TEXT("duplicateCancellation"));
	TestEqual(TEXT("Cancellation completes exactly once"), Completions, 1);
	TestEqual(TEXT("Cancellation remains distinct from zero GPU queries"), Code, FString(TEXT("niagara_capture_cancelled")));
	Tool->BeginExecuteAsync(Params, [&](FMCPToolResult&& Result) { ++Completions; Code = Result.ErrorCode; });
	Registry.CancelAsyncTools(TEXT("testShutdown"));
	TestEqual(TEXT("Shutdown cancels a fresh request after reuse"), Completions, 2);
	return true;
}
#endif
#endif
