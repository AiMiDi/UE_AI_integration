#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/EngineVersion.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif
#if WITH_UEAI_NIAGARA
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceAsyncGpuTrace.h"
#include "NiagaraSystem.h"
#if __has_include("NiagaraAsyncGpuTraceDiagnostics.h")
#include "NiagaraAsyncGpuTraceDiagnostics.h"
#endif
#endif

namespace UEAINiagaraRuntimePrivate
{
using UEAIIntegration::Infrastructure::FPIESessionController;

UWorld* FindWorld(const FString& Path)
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && World->GetPathName() == Path
			&& (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)) return World;
	}
	return nullptr;
}

FMCPToolResult MissingBridge()
{
	return FMCPToolResult::Error(TEXT("This build does not include the optional Editor Niagara AsyncGpuTrace evidence bridge. Runtime inspection remains available when Niagara is enabled."),
		TEXT("niagara_runtime_evidence_unavailable"), 503);
}

class FTool_NiagaraRuntimeInspect final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.runtime.inspect"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
#if WITH_UEAI_NIAGARA
		FString WorldPath;
		Params->TryGetStringField(TEXT("world"), WorldPath);
		UWorld* SelectedWorld = WorldPath.IsEmpty() ? nullptr : FindWorld(WorldPath);
		if (!WorldPath.IsEmpty() && !SelectedWorld) return FMCPToolResult::Error(TEXT("Select a loaded Editor or PIE world returned by runtime.inspect."), TEXT("world_not_found"), 404);
		const int32 Offset = Params->HasField(TEXT("offset")) ? FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("offset"))), 0, 100000) : 0;
		const int32 Limit = Params->HasField(TEXT("limit")) ? FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("limit"))), 1, 128) : 32;
		TArray<TSharedPtr<FJsonValue>> Worlds;
		if (GEngine) for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::PIE)) continue;
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), World->GetPathName());
			Row->SetStringField(TEXT("type"), World->WorldType == EWorldType::PIE ? TEXT("pie") : TEXT("editor"));
			Row->SetBoolField(TEXT("sceneAvailable"), World->Scene != nullptr);
			Worlds.Add(MakeShared<FJsonValueObject>(Row));
		}
		TArray<UNiagaraComponent*> Components;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			if (!IsValid(*It) || It->IsTemplate() || !It->GetWorld() || !FindWorld(It->GetWorld()->GetPathName())) continue;
			if (SelectedWorld && It->GetWorld() != SelectedWorld) continue;
			Components.Add(*It);
		}
		Components.Sort([](const auto& A, const auto& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> ComponentRows;
		for (int32 Index = Offset; Index < FMath::Min(Components.Num(), Offset + Limit); ++Index)
		{
			UNiagaraComponent* Component = Components[Index];
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), Component->GetPathName());
			Row->SetStringField(TEXT("world"), Component->GetWorld()->GetPathName());
			Row->SetStringField(TEXT("system"), Component->GetAsset() ? Component->GetAsset()->GetPathName() : TEXT(""));
			Row->SetBoolField(TEXT("active"), Component->IsActive());
			Row->SetBoolField(TEXT("paused"), Component->IsPaused());
			ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		TArray<UNiagaraDataInterfaceAsyncGpuTrace*> Interfaces;
		for (TObjectIterator<UNiagaraDataInterfaceAsyncGpuTrace> It; It; ++It)
		{
			if (!IsValid(*It) || It->HasAnyFlags(RF_ClassDefaultObject) || !It->GetProxy()) continue;
			const UNiagaraComponent* Owner = It->GetTypedOuter<UNiagaraComponent>();
			if (Owner && SelectedWorld && Owner->GetWorld() != SelectedWorld) continue;
			Interfaces.Add(*It);
		}
		Interfaces.Sort([](const auto& A, const auto& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> InterfaceRows;
		for (int32 Index = Offset; Index < FMath::Min(Interfaces.Num(), Offset + Limit); ++Index)
		{
			const auto* DI = Interfaces[Index];
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), DI->GetPathName());
			Row->SetStringField(TEXT("outer"), DI->GetOuter() ? DI->GetOuter()->GetPathName() : TEXT(""));
			Row->SetNumberField(TEXT("configuredProviderValue"), static_cast<int32>(DI->TraceProvider.GetValue()));
			Row->SetStringField(TEXT("bindingEvidence"), TEXT("loadedObjectOnly"));
			InterfaceRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-runtime-inventory.v1"));
		Result->SetArrayField(TEXT("worlds"), Worlds);
		Result->SetArrayField(TEXT("components"), ComponentRows);
		Result->SetArrayField(TEXT("dataInterfaces"), InterfaceRows);
		Result->SetNumberField(TEXT("componentCount"), Components.Num());
		Result->SetNumberField(TEXT("dataInterfaceCount"), Interfaces.Num());
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetBoolField(TEXT("hasMore"), Offset + Limit < FMath::Max(Components.Num(), Interfaces.Num()));
		Result->SetStringField(TEXT("scope"), TEXT("Current loaded objects; asset DIs may be shared across components and worlds. A matching capture is required to establish execution."));
#if defined(NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION)
		Result->SetNumberField(TEXT("evidenceBridgeVersion"), NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION);
#else
		Result->SetNumberField(TEXT("evidenceBridgeVersion"), 0);
#endif
		return FMCPToolResult::Ok(Result);
#else
		return MissingBridge();
#endif
	}
};

class FTool_NiagaraRuntimeCapture final : public FMCPToolBase
{
public:
	explicit FTool_NiagaraRuntimeCapture(FPIESessionController& InController) : Controller(InController) {}
	~FTool_NiagaraRuntimeCapture() override { CancelAsyncExecution(TEXT("toolShutdown")); }
	FString GetCapabilityId() const override { return TEXT("content.niagara.runtime.capture"); }
	bool SupportsAsyncExecution() const override { return true; }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(TEXT("Niagara capture requires asynchronous execution."), TEXT("async_execution_required"), 409);
	}
	bool BeginExecuteAsync(const TSharedPtr<FJsonObject>& Params, FMCPToolAsyncCompletion InCompletion) override
	{
#if defined(NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION)
		if (Completion) { InCompletion(FMCPToolResult::Error(TEXT("A Niagara evidence capture is already pending."), TEXT("capture_busy"), 409)); return true; }
		FString WorldPath, DIPath;
		Params->TryGetStringField(TEXT("world"), WorldPath);
		Params->TryGetStringField(TEXT("dataInterface"), DIPath);
		UWorld* World = FindWorld(WorldPath);
		auto* DI = FindObject<UNiagaraDataInterfaceAsyncGpuTrace>(nullptr, *DIPath);
		if (!World || !World->Scene || !IsValid(DI) || DI->GetPathName() != DIPath || !DI->GetProxy())
		{
			InCompletion(FMCPToolResult::Error(TEXT("Capture requires exact loaded world and AsyncGpuTrace Data Interface paths."), TEXT("runtime_target_not_found"), 404)); return true;
		}
		if (const auto* Owner = DI->GetTypedOuter<UNiagaraComponent>(); Owner && Owner->GetWorld() != World)
		{
			InCompletion(FMCPToolResult::Error(TEXT("The component-owned Data Interface belongs to another world."), TEXT("runtime_target_world_mismatch"), 409)); return true;
		}
		SessionId.Empty();
		Generation = 0;
		if (World->WorldType == EWorldType::PIE)
		{
			auto& Runtime = Controller.GetRuntimeService();
			FString RequestedSession;
			Params->TryGetStringField(TEXT("sessionId"), RequestedSession);
			const double RequestedGeneration = Params->HasField(TEXT("generation")) ? Params->GetNumberField(TEXT("generation")) : 0;
			if (!Runtime.IsSessionActive() || RequestedSession != Runtime.GetSessionId() || RequestedGeneration != static_cast<double>(Runtime.GetGeneration()))
			{
				InCompletion(FMCPToolResult::Error(TEXT("PIE capture requires the current sessionId and generation from scene.pie.status."), TEXT("stale_session_handle"), 409)); return true;
			}
			SessionId = RequestedSession;
			Generation = Runtime.GetGeneration();
		}
		const int32 ResultLimit = Params->HasField(TEXT("resultLimit")) ? static_cast<int32>(Params->GetNumberField(TEXT("resultLimit"))) : 64;
		const double Timeout = Params->HasField(TEXT("timeoutSeconds")) ? Params->GetNumberField(TEXT("timeoutSeconds")) : 5;
		Capture = NiagaraAsyncGpuTraceDiagnostics::RequestCapture(World->Scene, DI->GetProxy(), ResultLimit, Timeout);
		if (!Capture) { InCompletion(FMCPToolResult::Error(TEXT("Capture budget is exhausted or its limits are invalid."), TEXT("capture_budget_exceeded"), 429)); return true; }
		TargetWorld = World;
		TargetDI.Reset(DI);
		Completion = MoveTemp(InCompletion);
		WorldCleanup = FWorldDelegates::OnWorldCleanup.AddLambda([this](UWorld* CleaningWorld, bool, bool)
		{
			if (TargetWorld.Get() == CleaningWorld) CancelAsyncExecution(TEXT("targetWorldCleanup"));
		});
		Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
		{
			if (!SessionId.IsEmpty() && (!Controller.GetRuntimeService().IsSessionActive()
				|| Controller.GetRuntimeService().GetGeneration() != Generation))
			{
				CancelAsyncExecution(TEXT("staleSession")); return false;
			}
			const auto Snapshot = NiagaraAsyncGpuTraceDiagnostics::GetSnapshot(Capture);
			if (Snapshot.State == TEXT("pendingDispatch") || Snapshot.State == TEXT("pendingReadback")) return true;
			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-runtime-evidence.v1"));
			Result->SetStringField(TEXT("state"), Snapshot.State);
			Result->SetStringField(TEXT("reason"), Snapshot.Reason);
			Result->SetStringField(TEXT("world"), TargetWorld.IsValid() ? TargetWorld->GetPathName() : TEXT(""));
			Result->SetStringField(TEXT("dataInterface"), TargetDI.IsValid() ? TargetDI->GetPathName() : TEXT(""));
			Result->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
			Result->SetStringField(TEXT("sessionId"), SessionId);
			Result->SetNumberField(TEXT("generation"), static_cast<double>(Generation));
			Result->SetStringField(TEXT("scope"), TEXT("oneDataInterfaceDispatchAggregatedAcrossInstances"));
			Result->SetStringField(TEXT("configuredProvider"), Snapshot.ConfiguredProvider);
			Result->SetStringField(TEXT("selectedProvider"), Snapshot.SelectedProvider);
			const bool bDispatchObserved = Snapshot.DispatchFrame != MAX_uint32;
			if (bDispatchObserved)
			{
				Result->SetBoolField(TEXT("resultsCleared"), Snapshot.bResultsCleared);
				Result->SetBoolField(TEXT("previousResultAllocationPresent"), Snapshot.bPreviousResultAllocation);
				Result->SetNumberField(TEXT("capacity"), Snapshot.Capacity);
			}
			else for (const TCHAR* Name : {TEXT("resultsCleared"), TEXT("previousResultAllocationPresent"), TEXT("capacity")})
			{
				Result->SetField(Name, MakeShared<FJsonValueNull>());
			}
			TArray<TSharedPtr<FJsonValue>> Providers;
			for (const auto& State : Snapshot.Providers)
			{
				auto Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("provider"), State.Provider);
				Row->SetStringField(TEXT("availabilityReason"), State.AvailabilityReason);
				Row->SetStringField(TEXT("dispatchPath"), State.DispatchPath);
				Row->SetBoolField(TEXT("available"), State.bAvailable);
				Row->SetBoolField(TEXT("viewUniformPresent"), State.bViewUniformPresent);
				if (State.Provider == TEXT("hardwareRayTracing")) Row->SetBoolField(TEXT("tlasViewPresent"), State.bTlasViewPresent);
				if (State.Provider == TEXT("globalDistanceField")) Row->SetBoolField(TEXT("distanceFieldResourcesPresent"), State.bDistanceFieldResourcesPresent);
				Providers.Add(MakeShared<FJsonValueObject>(Row));
			}
			Result->SetArrayField(TEXT("providersInSelectionOrder"), Providers);
			auto Frame = [&](const TCHAR* Name, uint32 Value)
			{
				if (Value != MAX_uint32) Result->SetNumberField(Name, Value);
				else Result->SetField(Name, MakeShared<FJsonValueNull>());
			};
			Frame(TEXT("providerSetupRenderFrame"), Snapshot.SetupRenderFrame);
			Frame(TEXT("dispatchRenderFrame"), Snapshot.DispatchFrame);
			Frame(TEXT("readbackRenderFrame"), Snapshot.ReadbackFrame);
			Frame(TEXT("providerSetupViewFamilyFrame"), Snapshot.SetupViewFamilyFrame);
			Frame(TEXT("previousAllocationRenderFrame"), Snapshot.PreviousAllocationFrame);
			Result->SetNumberField(TEXT("providerSetupViewCount"), Snapshot.SetupViewCount);
			const bool bComplete = Snapshot.State == TEXT("complete");
			auto Metric = [&](const TCHAR* Name, uint32 Value)
			{
				if (bComplete) Result->SetNumberField(Name, Value);
				else Result->SetField(Name, MakeShared<FJsonValueNull>());
			};
			Metric(TEXT("queryHighWatermark"), Snapshot.QueryHighWatermark);
			Metric(TEXT("sampledSlots"), Snapshot.SampledSlots);
			Metric(TEXT("positiveHitSlots"), Snapshot.PositiveHitSlots);
			Metric(TEXT("nonFiniteSlots"), Snapshot.NonFiniteSlots);
			TArray<TSharedPtr<FJsonValue>> Distances;
			for (float Value : Snapshot.HitDistances)
			{
				if (FMath::IsFinite(Value)) Distances.Add(MakeShared<FJsonValueNumber>(Value));
				else Distances.Add(MakeShared<FJsonValueNull>());
			}
			Result->SetArrayField(TEXT("hitDistancesByQueryIndex"), Distances);
			if (bComplete) Result->SetBoolField(TEXT("sampleTruncated"), Snapshot.SampledSlots < FMath::Min(Snapshot.Capacity, Snapshot.QueryHighWatermark));
			else Result->SetField(TEXT("sampleTruncated"), MakeShared<FJsonValueNull>());
			Result->SetStringField(TEXT("countSemantics"), TEXT("GPU query high-water mark includes reservations and possible holes; it is not the number of issued rays. Results sample the prefix of this range, not a random sample or a per-emitter collision rate."));
			Result->SetStringField(TEXT("previousFrameSemantics"), TEXT("Previous allocation presence does not prove valid previous-frame Query IDs or consumption by particles."));
			Result->SetStringField(TEXT("providerEvidence"), TEXT("Provider availability and reference presence are sampled at dispatch; setup identifies the last PostRenderOpaque call and its first ViewFamily. Presence does not prove TLAS build completion, lifetime or ownership. Missing providers may be unsupported or unconfigured. Configured provider is the resolved dispatch configuration, not necessarily the original asset setting."));
			Finish(FMCPToolResult::Ok(Result));
			return false;
		}), 0.01f);
		return true;
#else
		InCompletion(MissingBridge());
		return true;
#endif
	}
	void CancelAsyncExecution(const FString& Reason) override
	{
#if defined(NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION)
		if (!Completion) return;
		NiagaraAsyncGpuTraceDiagnostics::CancelCapture(Capture);
		Finish(FMCPToolResult::Error(Reason, TEXT("niagara_capture_cancelled"), 409));
#endif
	}
private:
	FPIESessionController& Controller;
#if defined(NIAGARA_ASYNC_GPU_TRACE_DIAGNOSTICS_VERSION)
	void Finish(FMCPToolResult Result)
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
		Ticker.Reset();
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanup);
		WorldCleanup.Reset();
		Capture.Reset();
		TargetDI.Reset();
		TargetWorld.Reset();
		auto Done = MoveTemp(Completion);
		Completion = nullptr;
		if (Done) Done(MoveTemp(Result));
	}
	NiagaraAsyncGpuTraceDiagnostics::FCaptureHandle Capture;
	TStrongObjectPtr<UNiagaraDataInterfaceAsyncGpuTrace> TargetDI;
	TWeakObjectPtr<UWorld> TargetWorld;
	FString SessionId;
	uint64 Generation = 0;
	FTSTicker::FDelegateHandle Ticker;
	FDelegateHandle WorldCleanup;
	FMCPToolAsyncCompletion Completion;
#endif
};
}

namespace UEAIIntegrationTools
{
void RegisterNiagaraRuntimeTools(FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller)
{
	Registry.Register(MakeShared<UEAINiagaraRuntimePrivate::FTool_NiagaraRuntimeInspect>());
	Registry.Register(MakeShared<UEAINiagaraRuntimePrivate::FTool_NiagaraRuntimeCapture>(Controller));
}
}
