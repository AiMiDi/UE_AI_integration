#if WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA
#include "Misc/AutomationTest.h"
#include "Infrastructure/NiagaraSimCacheObservation.h"
#include "Tools/MCPToolRegistry.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Infrastructure/Sha256.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

namespace UEAIIntegrationTools { void RegisterNiagaraSimCacheTools(FMCPToolRegistry&); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSimCachePageTest,
	"UE_AI_integration.Niagara.SimCache.AttributePage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNiagaraSimCachePageTest::RunTest(const FString&)
{
	using namespace UEAINiagaraSimCachePrivate;
	const TArray<float> Floats = {1, 2, 3, 10, 20, 30, 100, 200, std::numeric_limits<float>::infinity()};
	const TArray<FFloat16> Halfs = {FFloat16(0.5f), FFloat16(1.5f), FFloat16(2.5f)};
	const TArray<int32> Ints = {11, 12, 13, -1, -2, -3};
	auto Page = MakeAttributePage(TEXT("mixed"), 3, 3, 1, 2, Floats, Halfs, Ints, 1, 1);
	if (!TestTrue(TEXT("Mixed layout page succeeds"), Page.bSuccess)) return false;
	const auto& Rows = Page.Data->GetArrayField(TEXT("instances"));
	TestEqual(TEXT("Page size"), Rows.Num(), 1);
	const auto Row = Rows[0]->AsObject();
	TestEqual(TEXT("Original frame index retained"), Row->GetIntegerField(TEXT("index")), 1);
	TestEqual(TEXT("Vector X component"), Row->GetArrayField(TEXT("floats"))[0]->AsNumber(), 2.0);
	TestEqual(TEXT("Vector Y component"), Row->GetArrayField(TEXT("floats"))[1]->AsNumber(), 20.0);
	TestEqual(TEXT("Vector Z component"), Row->GetArrayField(TEXT("floats"))[2]->AsNumber(), 200.0);
	TestEqual(TEXT("Half component"), Row->GetArrayField(TEXT("halfs"))[0]->AsNumber(), 1.5);
	TestEqual(TEXT("Integer ID acquire tag"), Row->GetArrayField(TEXT("ints"))[1]->AsNumber(), -2.0);
	TestEqual(TEXT("Continuation"), Page.Data->GetIntegerField(TEXT("nextOffset")), 2);
	Page = MakeAttributePage(TEXT("mixed"), 3, 3, 1, 2, Floats, Halfs, Ints, 2, 256);
	TestEqual(TEXT("Nonfinite count"), Page.Data->GetIntegerField(TEXT("nonFiniteValues")), 1);
	TestTrue(TEXT("Nonfinite JSON is null"), Page.Data->GetArrayField(TEXT("instances"))[0]->AsObject()->GetArrayField(TEXT("floats"))[2]->IsNull());
	TestFalse(TEXT("Last page has no continuation"), Page.Data->GetBoolField(TEXT("hasMore")));
	Page = MakeAttributePage(TEXT("mixed"), 3, 3, 1, 2, Floats, Halfs, Ints, 99, 1);
	TestEqual(TEXT("Beyond end empty"), Page.Data->GetArrayField(TEXT("instances")).Num(), 0);
	TestTrue(TEXT("Empty frame is a valid observation"), MakeAttributePage(TEXT("vec3"), 0, 3, 0, 0, {}, {}, {}, 0, 1).bSuccess);
	TestFalse(TEXT("Mismatched shape fails"), MakeAttributePage(TEXT("vec3"), 3, 2, 1, 2, Floats, Halfs, Ints, 0, 1).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSimCachePreflightTest,
	"UE_AI_integration.Niagara.SimCache.Preflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNiagaraSimCachePreflightTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSimCacheTools(Registry);
	Registry.EndDomainRegistration();
	auto* Capture = Registry.FindTool(TEXT("content.niagara.simcache.capture"));
	if (!TestNotNull(TEXT("Capture registered"), Capture)) return false;
	TestTrue(TEXT("Real frame async boundary"), Capture->SupportsAsyncExecution());
	auto Params = MakeShared<FJsonObject>();
	TestEqual(TEXT("Synchronous capture rejected"), Capture->Execute(Params).ErrorCode, FString(TEXT("async_execution_required")));
	int32 Completions = 0;
	FMCPToolResult Result;
	auto Run = [&]()
	{
		const int32 Before = Completions;
		TestTrue(TEXT("Completion accepted"), Capture->BeginExecuteAsync(Params, [&](FMCPToolResult&& Value) { ++Completions; Result = MoveTemp(Value); }));
		TestEqual(TEXT("Rejected request completes exactly once"), Completions, Before + 1);
	};
	Run();
	TestEqual(TEXT("Required parameters"), Result.ErrorCode, FString(TEXT("invalid_request")));
	Params->SetStringField(TEXT("world"), TEXT("/MissingSimCacheWorld.World"));
	Params->SetStringField(TEXT("component"), TEXT("/MissingSimCacheWorld.Component"));
	Params->SetArrayField(TEXT("attributes"), {MakeShared<FJsonValueString>(TEXT("Emitter.Particles.Position"))});
	Params->SetNumberField(TEXT("frames"), 1.5);
	Run();
	TestEqual(TEXT("Fractional bounds rejected before world resolution"), Result.ErrorCode, FString(TEXT("invalid_request")));
	Params->SetNumberField(TEXT("frames"), 2);
	Run();
	TestEqual(TEXT("Exact world is required"), Result.ErrorCode, FString(TEXT("world_not_found")));
	Capture->CancelAsyncExecution(TEXT("test_cancel"));
	TestEqual(TEXT("Idle cancellation never completes again"), Completions, 3);
	for (const TCHAR* Id : {TEXT("content.niagara.simcache.inspect"), TEXT("content.niagara.simcache.read"), TEXT("content.niagara.simcache.export"), TEXT("content.niagara.simcache.release")})
	{
		auto* Tool = Registry.FindTool(Id);
		if (!TestNotNull(Id, Tool)) return false;
		auto Missing = MakeShared<FJsonObject>();
		Missing->SetStringField(TEXT("captureId"), TEXT("missing"));
		TestEqual(TEXT("No cross-session or implicit cache fallback"), Tool->Execute(Missing).ErrorCode, FString(TEXT("simcache_not_found")));
	}
	return true;
}

namespace
{
struct FSimCacheLifecycleState
{
	FMCPToolRegistry Registry;
	TStrongObjectPtr<UNiagaraSystem> System;
	TStrongObjectPtr<UNiagaraComponent> Component;
	TSharedPtr<FJsonObject> Params;
	FMCPToolResult Result;
	bool Done = false;
	int32 Completions = 0;
	~FSimCacheLifecycleState()
	{
		if (auto* Capture = Registry.FindTool(TEXT("content.niagara.simcache.capture"))) Capture->CancelAsyncExecution(TEXT("test_cleanup"));
		if (Component.IsValid()) Component->DestroyComponent();
	}
};

class FSimCacheLifecycleCommand : public IAutomationLatentCommand
{
public:
	FSimCacheLifecycleCommand(FAutomationTestBase* InTest, TSharedRef<FSimCacheLifecycleState> InState)
		: Test(InTest), State(InState), Deadline(FPlatformTime::Seconds() + 25) {}
	bool Update() override
	{
		CancelIfReady();
		if (!State->Done)
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				Test->AddError(TEXT("SimCache lifecycle completion deadline exceeded."));
				State->Registry.FindTool(TEXT("content.niagara.simcache.capture"))->CancelAsyncExecution(TEXT("test_deadline"));
				return true;
			}
			// Only this test-owned transient fixture is advanced. The capture handler never advances simulation.
			State->Component->AdvanceSimulation(1, 1.0f / 60.0f);
			return false;
		}
		if (!State->Result.bSuccess || !State->Result.Data.IsValid())
		{
			Test->AddError(TEXT("Capture failed: ") + State->Result.ErrorCode + TEXT(": ") + State->Result.ErrorMessage);
			return true;
		}
		auto Data = State->Result.Data;
		if (Phase == 0)
		{
			if (!Test->TestEqual(TEXT("Two real frames complete"), Data->GetStringField(TEXT("status")), FString(TEXT("complete")))) return true;
			Test->TestEqual(TEXT("Captured frame count"), Data->GetIntegerField(TEXT("frameCount")), 2);
			Test->TestEqual(TEXT("Completion exactly once"), State->Completions, 1);
			auto Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("captureId"), Data->GetStringField(TEXT("captureId")));
			const auto Inspect = State->Registry.FindTool(TEXT("content.niagara.simcache.inspect"))->Execute(Params);
			if (!Test->TestTrue(TEXT("Recorded layout inspect"), Inspect.bSuccess)) return true;
			Test->TestEqual(TEXT("System and one emitter recorded"), Inspect.Data->GetArrayField(TEXT("emitters")).Num(), 2);
			Params->SetNumberField(TEXT("frame"), 1);
			Params->SetNumberField(TEXT("emitterIndex"), 0);
			Params->SetStringField(TEXT("attribute"), TEXT("Position"));
			Params->SetNumberField(TEXT("limit"), 1);
			const auto Read = State->Registry.FindTool(TEXT("content.niagara.simcache.read"))->Execute(Params);
			if (!Test->TestTrue(TEXT("Read actual particle frame"), Read.bSuccess)) return true;
			Test->TestTrue(TEXT("Emitter produced particles"), Read.Data->GetIntegerField(TEXT("instanceCount")) > 0);
			Test->TestEqual(TEXT("One particle page"), Read.Data->GetArrayField(TEXT("instances")).Num(), 1);
			Test->TestEqual(TEXT("Position has three floats"), Read.Data->GetIntegerField(TEXT("floatComponents")), 3);
			Params->SetNumberField(TEXT("frame"), 31);
			Test->TestEqual(TEXT("Out-of-range recorded frame rejected"), State->Registry.FindTool(TEXT("content.niagara.simcache.read"))->Execute(Params).ErrorCode, FString(TEXT("simcache_index_out_of_range")));
			Params->SetNumberField(TEXT("frame"), 1);
			const auto Export = State->Registry.FindTool(TEXT("content.niagara.simcache.export"))->Execute(Params);
			if (!Test->TestTrue(TEXT("Export evidence page"), Export.bSuccess)) return true;
			const FString Path = Export.Data->GetStringField(TEXT("path"));
			TArray<uint8> Bytes;
			Test->TestTrue(TEXT("Read exported file"), FFileHelper::LoadFileToArray(Bytes, *Path));
			FString Hash;
			UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, Hash);
			Test->TestEqual(TEXT("Artifact hash matches actual bytes"), Hash, Export.Data->GetStringField(TEXT("sha256")));
			FString Json;
			FFileHelper::LoadFileToString(Json, *Path);
			TSharedPtr<FJsonObject> Document;
			if (Test->TestTrue(TEXT("Artifact is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Document)))
				Test->TestEqual(TEXT("Exported frame retained"), Document->GetObjectField(TEXT("page"))->GetIntegerField(TEXT("frame")), 1);
			// Delete only the exact artifact returned for this test's unique recording.
			IFileManager::Get().Delete(*Path);
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Path), false, false);
			Test->TestTrue(TEXT("Release recorded cache"), State->Registry.FindTool(TEXT("content.niagara.simcache.release"))->Execute(Params).bSuccess);
			Test->TestEqual(TEXT("Released cache inaccessible"), State->Registry.FindTool(TEXT("content.niagara.simcache.inspect"))->Execute(Params).ErrorCode, FString(TEXT("simcache_not_found")));
			State->Done = false;
			State->Params->SetNumberField(TEXT("frames"), 32);
			State->Component->AdvanceSimulation(1, 1.0f / 60.0f);
			State->Registry.FindTool(TEXT("content.niagara.simcache.capture"))->BeginExecuteAsync(State->Params,
				[S = &State.Get()](FMCPToolResult&& R) { S->Result = MoveTemp(R); S->Done = true; ++S->Completions; });
			Phase = 1;
			return false;
		}
		Test->TestEqual(TEXT("Cancelled capture is explicitly partial"), Data->GetStringField(TEXT("status")), FString(TEXT("partial")));
		Test->TestEqual(TEXT("Cancellation reason retained"), Data->GetStringField(TEXT("reason")), FString(TEXT("test_cancel")));
		Test->TestEqual(TEXT("One completion per request"), State->Completions, 2);
		return true;
	}
	// Cancellation is driven on a subsequent Automation update after the recorder has had a real ticker boundary.
	void CancelIfReady()
	{
		if (Phase == 1 && !State->Done) State->Registry.FindTool(TEXT("content.niagara.simcache.capture"))->CancelAsyncExecution(TEXT("test_cancel"));
	}
private:
	FAutomationTestBase* Test;
	TSharedRef<FSimCacheLifecycleState> State;
	double Deadline;
	int32 Phase = 0;
};
}

static bool RunSimCacheLifecycleTest(FAutomationTestBase& Test, bool bGPU)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!Test.TestNotNull(TEXT("Editor world"), World)) return false;
	UNiagaraEmitter* Template = LoadObject<UNiagaraEmitter>(nullptr, TEXT("/Niagara/DefaultAssets/Templates/Emitters/SingleLoopingParticle.SingleLoopingParticle"));
	if (!Test.TestNotNull(TEXT("Stock Niagara fixture template"), Template)) return false;
	auto State = MakeShared<FSimCacheLifecycleState>();
	State->System.Reset(NewObject<UNiagaraSystem>(GetTransientPackage()));
	UNiagaraSystemFactoryNew::InitializeSystem(State->System.Get(), true);
	FNiagaraEditorUtilities::AddEmitterToSystem(*State->System, *Template, Template->GetExposedVersion().VersionGuid);
	State->System->GetEmitterHandles()[0].GetInstance().GetEmitterData()->SimTarget = bGPU ? ENiagaraSimTarget::GPUComputeSim : ENiagaraSimTarget::CPUSim;
	State->System->RequestCompile(false);
	State->System->WaitForCompilationComplete(bGPU, false);
	State->Component.Reset(NewObject<UNiagaraComponent>(World, NAME_None, RF_Transient));
	State->Component->SetAutoActivate(false);
	State->Component->SetForceSolo(true);
	State->Component->SetAsset(State->System.Get());
	State->Component->RegisterComponentWithWorld(World);
	State->Component->Activate(true);
	State->Component->AdvanceSimulation(3, 1.0f / 60.0f);
	State->Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSimCacheTools(State->Registry);
	State->Registry.EndDomainRegistration();
	State->Params = MakeShared<FJsonObject>();
	State->Params->SetStringField(TEXT("world"), World->GetPathName());
	State->Params->SetStringField(TEXT("component"), State->Component->GetPathName());
	State->Params->SetNumberField(TEXT("frames"), 2);
	State->Params->SetNumberField(TEXT("timeoutSeconds"), 20);
	State->Params->SetArrayField(TEXT("attributes"), {MakeShared<FJsonValueString>(State->System->GetEmitterHandles()[0].GetUniqueInstanceName() + TEXT(".Particles.Position"))});
	State->Registry.FindTool(TEXT("content.niagara.simcache.capture"))->BeginExecuteAsync(State->Params,
		[S = &State.Get()](FMCPToolResult&& R) { S->Result = MoveTemp(R); S->Done = true; ++S->Completions; });
	ADD_LATENT_AUTOMATION_COMMAND(FSimCacheLifecycleCommand(&Test, State));
	return true;
}

// NiagaraComponent::Activate requires CanEverRender even for CPU simulation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSimCacheLifecycleTest,
	"UE_AI_integration.Niagara.SimCache.CpuLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FNiagaraSimCacheLifecycleTest::RunTest(const FString&) { return RunSimCacheLifecycleTest(*this, false); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSimCacheGpuLifecycleTest,
	"UE_AI_integration.Niagara.SimCache.GpuLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FNiagaraSimCacheGpuLifecycleTest::RunTest(const FString&) { return RunSimCacheLifecycleTest(*this, true); }
#endif
