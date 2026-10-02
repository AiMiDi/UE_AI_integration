// Opt-in NonNullRHI runtime acceptance for authored Niagara Event Handlers and
// Simulation Stages.
//
// The system-spec persistence tests prove that authored metadata survives a
// save/reload boundary. They do not prove that Niagara can instantiate and
// tick that authored data. This test starts from a real system asset supplied
// by the host runner, creates an isolated Game world, and observes the live
// Niagara system instance after several world ticks.
#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraScript.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

namespace UEAINiagaraSystemRuntimeAcceptancePrivate
{
constexpr TCHAR RuntimeSystemEnvironment[] = TEXT("UEAI_NIAGARA_RUNTIME_SYSTEM");

FString ResolveSystemPath(const FString& Parameters)
{
	const FString ExplicitPath = Parameters.TrimStartAndEnd();
	if (!ExplicitPath.IsEmpty())
	{
		return ExplicitPath;
	}
	return FPlatformMisc::GetEnvironmentVariable(RuntimeSystemEnvironment).TrimStartAndEnd();
}

struct FAuthoredFeatureCounts
{
	int32 EnabledEmitters = 0;
	int32 EventHandlers = 0;
	int32 SimulationStages = 0;
};

FAuthoredFeatureCounts CountAuthoredFeatures(const UNiagaraSystem& System)
{
	FAuthoredFeatureCounts Counts;
	for (const FNiagaraEmitterHandle& Handle : System.GetEmitterHandles())
	{
		if (!Handle.GetIsEnabled())
		{
			continue;
		}
		++Counts.EnabledEmitters;
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data)
		{
			continue;
		}
		Counts.EventHandlers += Data->GetEventHandlers().Num();
		for (UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
		{
			Counts.SimulationStages += Stage != nullptr ? 1 : 0;
		}
	}
	return Counts;
}
} // namespace UEAINiagaraSystemRuntimeAcceptancePrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemRuntimeAcceptanceTest,
	"UE_AI_integration.Niagara.SystemSpec.NonNullRHIRuntimeAcceptance",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter
		| EAutomationTestFlags::NonNullRHI)

bool FNiagaraSystemRuntimeAcceptanceTest::RunTest(const FString& Parameters)
{
	using namespace UEAINiagaraSystemRuntimeAcceptancePrivate;
	const FString SystemPath = ResolveSystemPath(Parameters);
	if (SystemPath.IsEmpty())
	{
		AddInfo(
			TEXT("Opt-in only: pass a Niagara System object path as Automation parameters "
				"or set UEAI_NIAGARA_RUNTIME_SYSTEM."));
		return true;
	}

	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!TestNotNull(TEXT("Runtime acceptance system loads"), System))
	{
		return false;
	}
	const FAuthoredFeatureCounts Authored = CountAuthoredFeatures(*System);
	TestTrue(TEXT("Runtime acceptance asset contains an authored Event Handler"), Authored.EventHandlers > 0);
	TestTrue(TEXT("Runtime acceptance asset contains an authored Simulation Stage"), Authored.SimulationStages > 0);
	if (Authored.EventHandlers <= 0 || Authored.SimulationStages <= 0)
	{
		AddError(
			TEXT("The supplied runtime acceptance asset must contain both authored "
				"Event Handler and Simulation Stage entries."));
		return false;
	}

	System->WaitForCompilationComplete(true, false);
	TestTrue(TEXT("Niagara System is ready to run before world activation"), System->IsReadyToRun());
	if (!System->IsReadyToRun() || !TestNotNull(TEXT("Runtime engine exists"), GEngine))
	{
		return false;
	}

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("NonNullRHI runtime world creates"), World))
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	UNiagaraComponent* Component = nullptr;
	ON_SCOPE_EXIT
	{
		if (Component)
		{
			Component->DeactivateImmediate();
			if (Component->IsRegistered())
			{
				Component->UnregisterComponent();
			}
		}
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	FURL PlayURL;
	World->InitializeActorsForPlay(PlayURL);
	World->BeginPlay();
	Component = NewObject<UNiagaraComponent>(World, TEXT("NiagaraRuntimeAcceptanceComponent"));
	if (!TestNotNull(TEXT("Runtime Niagara component creates"), Component))
	{
		return false;
	}
	Component->SetAsset(System);
	Component->RegisterComponentWithWorld(World);
	Component->Activate(true);

	constexpr int32 TickCount = 8;
	for (int32 Index = 0; Index < TickCount; ++Index)
	{
		++GFrameCounter;
		World->Tick(LEVELTICK_All, 1.0f / 60.0f);
	}

	FNiagaraSystemInstance* Instance = Component->GetSystemInstance();
	if (!TestNotNull(TEXT("Runtime Niagara system instance is created"), Instance))
	{
		return false;
	}
	TestFalse(TEXT("Runtime Niagara system instance is not disabled"), Instance->IsDisabled());
	TestTrue(TEXT("Runtime Niagara system instance received world ticks"), Instance->GetTickCount() > 0);

	int32 RuntimeEmitterCount = 0;
	int32 RuntimeEventHandlerCount = 0;
	int32 ReadyEventHandlerCount = 0;
	int32 RuntimeEventContextCount = 0;
	int32 RuntimeSimulationStageCount = 0;
	int32 ReadySimulationStageCount = 0;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		FNiagaraEmitterInstancePtr RuntimeEmitter = Instance->GetSimulationForHandle(Handle);
		if (!RuntimeEmitter.IsValid())
		{
			continue;
		}
		++RuntimeEmitterCount;
		RuntimeEventContextCount += RuntimeEmitter->GetEventExecutionContexts().Num();
		const FVersionedNiagaraEmitterData* RuntimeData = RuntimeEmitter->GetVersionedEmitter().GetEmitterData();
		if (!RuntimeData)
		{
			continue;
		}
		for (const FNiagaraEventScriptProperties& EventHandler : RuntimeData->GetEventHandlers())
		{
			++RuntimeEventHandlerCount;
			if (EventHandler.Script && EventHandler.Script->IsReadyToRun(RuntimeEmitter->GetSimTarget()))
			{
				++ReadyEventHandlerCount;
			}
		}
		for (UNiagaraSimulationStageBase* Stage : RuntimeData->GetSimulationStages())
		{
			if (!Stage)
			{
				continue;
			}
			++RuntimeSimulationStageCount;
			if (Stage->Script && Stage->Script->IsReadyToRun(RuntimeEmitter->GetSimTarget()))
			{
				++ReadySimulationStageCount;
			}
		}
	}

	TestEqual(TEXT("Every enabled authored emitter has a live runtime emitter instance"), RuntimeEmitterCount, Authored.EnabledEmitters);
	TestEqual(TEXT("Authored Event Handler metadata survives into the runtime emitter"), RuntimeEventHandlerCount, Authored.EventHandlers);
	TestEqual(TEXT("Every authored Event Handler script is ready for the runtime sim target"), ReadyEventHandlerCount, Authored.EventHandlers);
	TestTrue(TEXT("Event Handler execution contexts are allocated in the runtime instance"), RuntimeEventContextCount >= Authored.EventHandlers);
	TestEqual(TEXT("Simulation Stage metadata survives into the runtime emitter"), RuntimeSimulationStageCount, Authored.SimulationStages);
	TestEqual(TEXT("Every authored Simulation Stage script is ready for the runtime sim target"), ReadySimulationStageCount, Authored.SimulationStages);
	AddInfo(FString::Printf(
		TEXT("runtimeEvidence=nonnull_rhi_world_tick; system=%s; ticks=%d; emitters=%d; "
			"eventHandlers=%d; readyEventHandlers=%d; eventExecutionContexts=%d; simulationStages=%d; readyStages=%d; "
			"scope=live_instance_initialization_and_tick; event_delivery_and_stage_side_effects_require_project_specific_fixture"),
		*System->GetPathName(), TickCount, RuntimeEmitterCount, Authored.EventHandlers,
		ReadyEventHandlerCount, RuntimeEventContextCount, RuntimeSimulationStageCount, ReadySimulationStageCount));
	Component->DeactivateImmediate();
	Component->UnregisterComponent();
	return !HasAnyErrors();
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
