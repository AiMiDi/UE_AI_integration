#include "Infrastructure/NiagaraSimCacheObservation.h"
#include "Infrastructure/Sha256.h"
#include "Tools/MCPToolRegistry.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/StrongObjectPtr.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif
#if WITH_UEAI_NIAGARA
#include "Engine/World.h"
#include "NiagaraComponent.h"
#include "NiagaraSimCache.h"
#include "NiagaraSystem.h"
#endif

namespace UEAINiagaraRuntimePrivate { UWorld* FindWorld(const FString& Path); }

namespace UEAINiagaraSimCachePrivate
{
static FMCPToolResult Invalid(const FString& Message)
{
	return FMCPToolResult::Error(Message, TEXT("invalid_request"), 400);
}

static bool ReadInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Default,
	int32 Min, int32 Max, int32& Out)
{
	Out = Default;
	if (!Params->HasField(Name)) return true;
	double Value;
	if (!Params->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value)
		|| Value < Min || Value > Max || Value != FMath::FloorToDouble(Value)) return false;
	Out = static_cast<int32>(Value);
	return true;
}

FMCPToolResult MakeAttributePage(const FString& Type, int32 Instances, int32 FloatCount,
	int32 HalfCount, int32 IntCount, const TArray<float>& Floats, const TArray<FFloat16>& Halfs,
	const TArray<int32>& Ints, int32 Offset, int32 Limit)
{
	if (Instances < 0 || FloatCount < 0 || HalfCount < 0 || IntCount < 0 || Offset < 0
		|| Limit < 1 || Limit > 256 || FloatCount + HalfCount + IntCount > 64
		|| int64(Instances) * FloatCount != Floats.Num()
		|| int64(Instances) * HalfCount != Halfs.Num()
		|| int64(Instances) * IntCount != Ints.Num())
		return FMCPToolResult::Error(TEXT("Attribute buffer shape does not match the recorded layout."), TEXT("simcache_layout_mismatch"));
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);
	Result->SetNumberField(TEXT("instanceCount"), Instances);
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("floatComponents"), FloatCount);
	Result->SetNumberField(TEXT("halfComponents"), HalfCount);
	Result->SetNumberField(TEXT("intComponents"), IntCount);
	TArray<TSharedPtr<FJsonValue>> Rows;
	int32 NonFinite = 0;
	const int32 End = static_cast<int32>(FMath::Min<int64>(Instances, int64(Offset) + Limit));
	for (int32 Index = Offset; Index < End; ++Index)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Index);
		TArray<TSharedPtr<FJsonValue>> FloatValues, HalfValues, IntValues;
		auto AddNumber = [&NonFinite](TArray<TSharedPtr<FJsonValue>>& Values, double Value)
		{
			if (FMath::IsFinite(Value)) Values.Add(MakeShared<FJsonValueNumber>(Value));
			else { Values.Add(MakeShared<FJsonValueNull>()); ++NonFinite; }
		};
		for (int32 C = 0; C < FloatCount; ++C) AddNumber(FloatValues, Floats[C * Instances + Index]);
		for (int32 C = 0; C < HalfCount; ++C) AddNumber(HalfValues, Halfs[C * Instances + Index].GetFloat());
		for (int32 C = 0; C < IntCount; ++C) IntValues.Add(MakeShared<FJsonValueNumber>(Ints[C * Instances + Index]));
		Row->SetArrayField(TEXT("floats"), FloatValues);
		Row->SetArrayField(TEXT("halfs"), HalfValues);
		Row->SetArrayField(TEXT("ints"), IntValues);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("instances"), Rows);
	Result->SetNumberField(TEXT("returned"), Rows.Num());
	Result->SetNumberField(TEXT("nonFiniteValues"), NonFinite);
	Result->SetBoolField(TEXT("hasMore"), End < Instances);
	if (End < Instances) Result->SetNumberField(TEXT("nextOffset"), End);
	Result->SetStringField(TEXT("coordinates"), TEXT("raw stored simulation values; no world-space conversion or rebasing"));
	Result->SetStringField(TEXT("identity"), TEXT("index is frame-local, not a persistent particle ID; read the recorded ID attribute for correlation"));
	return FMCPToolResult::Ok(Result);
}

#if WITH_UEAI_NIAGARA
static constexpr int64 MaxLogicalBytes = 64ll * 1024 * 1024;
static constexpr int64 MaxReadBytes = 16ll * 1024 * 1024;

struct FRecording
{
	TStrongObjectPtr<UNiagaraSimCache> Cache;
	FString Id, World, Component, System, Status, Reason;
	TArray<FString> RequestedAttributes, Feedback;
	int32 RequestedFrames = 0;
	int64 LogicalBytes = 0;
};

struct FStore
{
	// No silent eviction: callers release one of the four slots explicitly.
	TMap<FString, TSharedPtr<FRecording>> Recordings;
};

static void AddFeedback(FRecording& Recording, const FNiagaraSimCacheFeedbackContext& Feedback)
{
	for (const auto* Messages : { &Feedback.Errors, &Feedback.Warnings })
		for (const FString& Message : *Messages)
			if (Recording.Feedback.Num() < 16) Recording.Feedback.AddUnique(Message.Left(512));
}

static TSharedPtr<FJsonObject> Describe(const FRecording& R)
{
	auto Result = MakeShared<FJsonObject>();
	const UNiagaraSimCache* Cache = R.Cache.Get();
	Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-simcache.v1"));
	Result->SetStringField(TEXT("captureId"), R.Id);
	Result->SetStringField(TEXT("status"), R.Status);
	Result->SetStringField(TEXT("reason"), R.Reason);
	Result->SetStringField(TEXT("world"), R.World);
	Result->SetStringField(TEXT("component"), R.Component);
	Result->SetStringField(TEXT("system"), R.System);
	Result->SetBoolField(TEXT("cacheValid"), Cache->IsCacheValid());
	Result->SetNumberField(TEXT("requestedFrames"), R.RequestedFrames);
	Result->SetNumberField(TEXT("frameCount"), Cache->GetNumFrames());
	Result->SetNumberField(TEXT("startSeconds"), Cache->GetStartSeconds());
	Result->SetNumberField(TEXT("durationSeconds"), Cache->GetDurationSeconds());
	Result->SetNumberField(TEXT("logicalAttributeBytes"), double(R.LogicalBytes));
	Result->SetStringField(TEXT("memoryLimitScope"), TEXT("64 MiB recorded attribute payload checked after each frame; UE allocations, ID tables and GPU readback staging are additional and a single WriteFrame cannot be preempted"));
	Result->SetStringField(TEXT("retention"), TEXT("transient until release or plugin shutdown; at most four recordings"));
	Result->SetStringField(TEXT("scope"), TEXT("Public SimCache particle/system attributes. No AsyncGpuTrace private buffers, provider selection, TLAS, dispatch or GPU timing evidence. GPU capture may flush pending ticks and stall for readback."));
	TArray<TSharedPtr<FJsonValue>> Feedback, Requested;
	for (const FString& Message : R.Feedback) Feedback.Add(MakeShared<FJsonValueString>(Message));
	for (const FString& Name : R.RequestedAttributes) Requested.Add(MakeShared<FJsonValueString>(Name));
	Result->SetArrayField(TEXT("feedback"), Feedback);
	Result->SetArrayField(TEXT("requestedAttributes"), Requested);
	TArray<TSharedPtr<FJsonValue>> Emitters;
	for (int32 Emitter = INDEX_NONE; Emitter < Cache->GetNumEmitters(); ++Emitter)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetNumberField(TEXT("index"), Emitter);
		Row->SetStringField(TEXT("name"), Emitter == INDEX_NONE ? TEXT("") : Cache->GetEmitterName(Emitter).ToString());
		TArray<TSharedPtr<FJsonValue>> Attributes, Counts;
		Cache->ForEachEmitterAttribute(Emitter, [&](const FNiagaraSimCacheVariable& V)
		{
			auto Attribute = MakeShared<FJsonObject>();
			Attribute->SetStringField(TEXT("name"), V.Variable.GetName().ToString());
			Attribute->SetStringField(TEXT("type"), V.Variable.GetType().GetName());
			Attribute->SetNumberField(TEXT("floatComponents"), V.FloatCount);
			Attribute->SetNumberField(TEXT("halfComponents"), V.HalfCount);
			Attribute->SetNumberField(TEXT("intComponents"), V.Int32Count);
			Attributes.Add(MakeShared<FJsonValueObject>(Attribute));
			return true;
		});
		for (int32 Frame = 0; Frame < Cache->GetNumFrames(); ++Frame)
			Counts.Add(MakeShared<FJsonValueNumber>(Cache->GetEmitterNumInstances(Emitter, Frame)));
		Row->SetArrayField(TEXT("attributes"), Attributes);
		Row->SetArrayField(TEXT("frameInstanceCounts"), Counts);
		Emitters.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("emitters"), Emitters);
	return Result;
}

class FCapture final : public FMCPToolBase
{
public:
	explicit FCapture(TSharedRef<FStore> InStore) : Store(InStore) {}
	~FCapture() override { CancelAsyncExecution(TEXT("handler_shutdown")); }
	FString GetCapabilityId() const override { return TEXT("content.niagara.simcache.capture"); }
	bool SupportsAsyncExecution() const override { return true; }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		return FMCPToolResult::Error(TEXT("Capture spans real Editor frames; use queued async execution."), TEXT("async_execution_required"), 409);
	}
	bool BeginExecuteAsync(const TSharedPtr<FJsonObject>& Params, FMCPToolAsyncCompletion Completion) override
	{
		check(IsInGameThread());
		if (!Completion) return false;
		auto Reject = [&Completion](FMCPToolResult Result) { Completion(MoveTemp(Result)); return true; };
		if (Active) return Reject(FMCPToolResult::Error(TEXT("A SimCache capture is active."), TEXT("simcache_busy"), 409));
		if (!Params) return Reject(Invalid(TEXT("Capture parameters required.")));
		FString WorldPath, ComponentPath;
		int32 Frames, Rate, Timeout;
		const TArray<TSharedPtr<FJsonValue>>* Attributes = nullptr;
		if (!Params->TryGetStringField(TEXT("world"), WorldPath) || WorldPath.IsEmpty() || WorldPath.Len() > 1024
			|| !Params->TryGetStringField(TEXT("component"), ComponentPath) || ComponentPath.IsEmpty() || ComponentPath.Len() > 1024
			|| !Params->TryGetArrayField(TEXT("attributes"), Attributes) || Attributes->Num() < 1 || Attributes->Num() > 32
			|| !ReadInt(Params, TEXT("frames"), 8, 1, 32, Frames)
			|| !ReadInt(Params, TEXT("captureRate"), 1, 1, 16, Rate)
			|| !ReadInt(Params, TEXT("timeoutSeconds"), 10, 1, 20, Timeout))
			return Reject(Invalid(TEXT("Exact world/component, 1-32 qualified attributes and bounded capture parameters are required.")));
		FNiagaraSimCacheCreateParameters Create;
		Create.AttributeCaptureMode = ENiagaraSimCacheAttributeCaptureMode::ExplicitAttributes;
		Create.bAllowDataInterfaceCaching = false;
		Create.bAllowRebasing = false;
		Create.bAllowInterpolation = false;
		Create.bAllowVelocityExtrapolation = false;
		Create.bIncludeDebugData = false;
		TArray<FString> Requested;
		for (const auto& Value : *Attributes)
		{
			FString Name;
			if (!Value || !Value->TryGetString(Name) || Name.IsEmpty() || Name.Len() > 256)
				return Reject(Invalid(TEXT("Each attribute must be a non-empty qualified name of at most 256 characters.")));
			Requested.AddUnique(Name);
			Create.ExplicitCaptureAttributes.AddUnique(FName(*Name));
		}
		UWorld* World = UEAINiagaraRuntimePrivate::FindWorld(WorldPath);
		if (!World) return Reject(FMCPToolResult::Error(TEXT("Exact loaded Editor or PIE world not found."), TEXT("world_not_found"), 404));
		UNiagaraComponent* Component = FindObject<UNiagaraComponent>(nullptr, *ComponentPath);
		if (!IsValid(Component) || Component->IsTemplate() || Component->GetWorld() != World || Component->GetPathName() != ComponentPath)
			return Reject(FMCPToolResult::Error(TEXT("Exact component not found in requested world."), TEXT("component_not_found"), 404));
		if (!Component->IsRegistered() || !Component->IsActive() || Component->IsPaused() || !Component->GetAsset())
			return Reject(FMCPToolResult::Error(TEXT("Component must already be registered, active and unpaused."), TEXT("simcache_component_not_running"), 409));
		if (Component->GetAsset()->GetEmitterHandles().Num() > 64)
			return Reject(Invalid(TEXT("At most 64 emitters can be observed per capture.")));
		if (Store->Recordings.Num() >= 4)
			return Reject(FMCPToolResult::Error(TEXT("Release a retained recording before capturing again."), TEXT("simcache_store_full"), 409));
		auto Recording = MakeShared<FRecording>();
		Recording->Cache.Reset(NewObject<UNiagaraSimCache>(GetTransientPackage()));
		Recording->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Recording->World = WorldPath;
		Recording->Component = ComponentPath;
		Recording->System = Component->GetAsset()->GetPathName();
		Recording->RequestedFrames = Frames;
		Recording->RequestedAttributes = Requested;
		FNiagaraSimCacheFeedbackContext Feedback(false);
		const bool Begun = Recording->Cache->BeginWrite(Create, Component, Feedback);
		AddFeedback(*Recording, Feedback);
		if (!Begun)
			return Reject(FMCPToolResult::Error(TEXT("SimCache BeginWrite failed: ") + FString::Join(Recording->Feedback, TEXT("; ")), TEXT("simcache_begin_failed"), 409));
		// Catch misspelled/optimized-away attributes rather than returning an apparently valid empty observation.
		TSet<FName> Captured;
		for (int32 E = INDEX_NONE; E < Recording->Cache->GetNumEmitters(); ++E)
		{
			const FString Prefix = E == INDEX_NONE ? TEXT("") : Component->GetAsset()->GetEmitterHandles()[E].GetUniqueInstanceName() + TEXT(".Particles.");
			Recording->Cache->ForEachEmitterAttribute(E, [&](const FNiagaraSimCacheVariable& V)
			{
				Captured.Add(FName(*(Prefix + V.Variable.GetName().ToString())));
				return true;
			});
		}
		for (FName Name : Create.ExplicitCaptureAttributes)
		{
			if (!Captured.Contains(Name))
			{
				Recording->Cache->EndWrite();
				return Reject(FMCPToolResult::Error(TEXT("Attribute is not in the compiled cache layout: ") + Name.ToString(), TEXT("simcache_attribute_not_captured"), 404));
			}
		}
		Active = Recording;
		Target = Component;
		TargetWorld = World;
		TargetSystem = Component->GetAsset();
		CaptureRate = Rate;
		TickIndex = 0;
		Deadline = FPlatformTime::Seconds() + Timeout;
		OnComplete = MoveTemp(Completion);
		// Same core-ticker boundary used by UE's public FNiagaraSimCacheCapture.
		Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FCapture::Tick));
		return true;
	}
	void CancelAsyncExecution(const FString& Reason) override { if (Active) Finish(Reason); }

private:
	bool Tick(float)
	{
		if (FPlatformTime::Seconds() >= Deadline) { Finish(TEXT("timeout")); return false; }
		UNiagaraComponent* Component = Target.Get();
		if (!IsValid(Component) || !TargetWorld.IsValid() || TargetWorld->bIsTearingDown
			|| Component->GetWorld() != TargetWorld.Get()
			|| UEAINiagaraRuntimePrivate::FindWorld(Active->World) != TargetWorld.Get()
			|| Component->GetAsset() != TargetSystem.Get() || !Component->IsRegistered())
		{ Finish(TEXT("target_changed_or_destroyed")); return false; }
		if (!Component->IsActive()) { Finish(TEXT("component_inactive")); return false; }
		if ((TickIndex++ % CaptureRate) != 0 || Component->IsPaused()) return true;
		FNiagaraSimCacheFeedbackContext Feedback(false);
		const bool Written = Active->Cache->WriteFrame(Component, Feedback);
		AddFeedback(*Active, Feedback);
		if (!Active->Cache->IsCacheValid()) { Finish(TEXT("cache_invalidated")); return false; }
		if (!Written) return true; // Duplicate simulation ticks are not frames; wall-clock deadline still applies.
		const int32 Frame = Active->Cache->GetNumFrames() - 1;
		for (int32 E = INDEX_NONE; E < Active->Cache->GetNumEmitters(); ++E)
		{
			int64 Stride = 0;
			Active->Cache->ForEachEmitterAttribute(E, [&Stride](const FNiagaraSimCacheVariable& V)
			{ Stride += int64(V.FloatCount) * 4 + int64(V.HalfCount) * 2 + int64(V.Int32Count) * 4; return true; });
			Active->LogicalBytes += Stride * Active->Cache->GetEmitterNumInstances(E, Frame);
		}
		if (Active->LogicalBytes > MaxLogicalBytes)
		{
			// Drop the oversized cache instead of retaining a recording beyond the payload budget.
			Finish(TEXT("payload_budget_exceeded"), false);
			return false;
		}
		if (FPlatformTime::Seconds() >= Deadline) { Finish(TEXT("timeout")); return false; }
		if (Active->Cache->GetNumFrames() >= Active->RequestedFrames) { Finish(TEXT("frame_limit")); return false; }
		return true;
	}
	void Finish(const FString& Reason, bool Retain = true)
	{
		FTSTicker::RemoveTicker(Ticker);
		Ticker.Reset();
		auto Recording = MoveTemp(Active);
		Recording->Cache->EndWrite();
		Recording->Reason = Reason.Left(512);
		const bool Valid = Recording->Cache->IsCacheValid() && Recording->Cache->GetNumFrames() > 0;
		Recording->Status = Valid && Retain ? (Reason == TEXT("frame_limit") ? TEXT("complete") : TEXT("partial")) : TEXT("failed");
		if (Retain && Valid) Store->Recordings.Add(Recording->Id, Recording);
		auto Result = Describe(*Recording);
		Result->SetBoolField(TEXT("retained"), Retain && Valid);
		auto Completion = MoveTemp(OnComplete);
		Target.Reset(); TargetWorld.Reset(); TargetSystem.Reset();
		// Clear all active state before invoking a completion that may destroy this handler.
		Completion(FMCPToolResult::Ok(Result));
	}
	TSharedRef<FStore> Store;
	TSharedPtr<FRecording> Active;
	TWeakObjectPtr<UNiagaraComponent> Target;
	TWeakObjectPtr<UWorld> TargetWorld;
	TWeakObjectPtr<UNiagaraSystem> TargetSystem;
	FTSTicker::FDelegateHandle Ticker;
	FMCPToolAsyncCompletion OnComplete;
	double Deadline = 0;
	int32 CaptureRate = 1, TickIndex = 0;
};

static FMCPToolResult ReadPage(const FRecording& R, const TSharedPtr<FJsonObject>& Params)
{
	int32 Frame, Emitter, Offset, Limit;
	FString Attribute;
	if (!ReadInt(Params, TEXT("frame"), -1, 0, 31, Frame) || Frame < 0
		|| !ReadInt(Params, TEXT("emitterIndex"), -2, -1, 63, Emitter) || Emitter < -1
		|| !ReadInt(Params, TEXT("offset"), 0, 0, 10000000, Offset)
		|| !ReadInt(Params, TEXT("limit"), 64, 1, 256, Limit)
		|| !Params->TryGetStringField(TEXT("attribute"), Attribute) || Attribute.IsEmpty() || Attribute.Len() > 256)
		return Invalid(TEXT("frame, emitterIndex (-1 for system), attribute and bounded pagination are required."));
	UNiagaraSimCache* Cache = R.Cache.Get();
	if (!Cache->IsCacheValid()) return FMCPToolResult::Error(TEXT("Recorded cache is invalid."), TEXT("simcache_invalid"), 409);
	if (Frame >= Cache->GetNumFrames() || Emitter >= Cache->GetNumEmitters())
		return FMCPToolResult::Error(TEXT("Requested frame or emitter is outside this recording."), TEXT("simcache_index_out_of_range"), 404);
	FNiagaraSimCacheVariable Variable;
	bool Found = false;
	Cache->ForEachEmitterAttribute(Emitter, [&](const FNiagaraSimCacheVariable& V)
	{
		if (V.Variable.GetName() == FName(*Attribute)) { Variable = V; Found = true; return false; }
		return true;
	});
	if (!Found) return FMCPToolResult::Error(TEXT("Attribute was not recorded; inspect this cache's layout."), TEXT("simcache_attribute_not_captured"), 404);
	const int32 Instances = Cache->GetEmitterNumInstances(Emitter, Frame);
	const int64 Stride = int64(Variable.FloatCount) * 4 + int64(Variable.HalfCount) * 2 + int64(Variable.Int32Count) * 4;
	if (Stride * Instances > MaxReadBytes || Variable.FloatCount + Variable.HalfCount + Variable.Int32Count > 64)
		return FMCPToolResult::Error(TEXT("Public ReadAttribute reads the full attribute before pagination; this exceeds the 16 MiB scratch or 64 component limit."), TEXT("simcache_read_budget_exceeded"), 413);
	TArray<float> Floats;
	TArray<FFloat16> Halfs;
	TArray<int32> Ints;
	// UE 5.4's generic reader indexes an output pointer even for empty arrays.
	if (Instances > 0) Cache->ReadAttribute(Floats, Halfs, Ints, Variable.Variable.GetName(), Cache->GetEmitterName(Emitter), Frame);
	auto Page = MakeAttributePage(Variable.Variable.GetType().GetName(), Instances, Variable.FloatCount,
		Variable.HalfCount, Variable.Int32Count, Floats, Halfs, Ints, Offset, Limit);
	if (!Page.bSuccess) return Page;
	Page.Data->SetStringField(TEXT("schema"), TEXT("ue.niagara-simcache-attribute.v1"));
	Page.Data->SetStringField(TEXT("captureId"), R.Id);
	Page.Data->SetStringField(TEXT("captureStatus"), R.Status);
	Page.Data->SetNumberField(TEXT("frame"), Frame);
	Page.Data->SetNumberField(TEXT("emitterIndex"), Emitter);
	Page.Data->SetStringField(TEXT("emitter"), Emitter == INDEX_NONE ? TEXT("") : Cache->GetEmitterName(Emitter).ToString());
	Page.Data->SetStringField(TEXT("attribute"), Variable.Variable.GetName().ToString());
	return Page;
}

class FStoredTool : public FMCPToolBase
{
public:
	explicit FStoredTool(TSharedRef<FStore> InStore) : Store(InStore) {}
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Id;
		if (!Params || !Params->TryGetStringField(TEXT("captureId"), Id) || Id.IsEmpty() || Id.Len() > 64)
			return Invalid(TEXT("captureId is required."));
		const auto Recording = Store->Recordings.FindRef(Id);
		if (!Recording) return FMCPToolResult::Error(TEXT("No retained capture with that ID in this Editor instance."), TEXT("simcache_not_found"), 404);
		return ExecuteRecording(*Recording, Params);
	}
protected:
	virtual FMCPToolResult ExecuteRecording(const FRecording&, const TSharedPtr<FJsonObject>&) = 0;
	TSharedRef<FStore> Store;
};

class FInspect final : public FStoredTool
{
public:
	using FStoredTool::FStoredTool;
	FString GetCapabilityId() const override { return TEXT("content.niagara.simcache.inspect"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		if (Params && Params->HasField(TEXT("captureId"))) return FStoredTool::Execute(Params);
		auto Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Recordings;
		for (const auto& Pair : Store->Recordings) Recordings.Add(MakeShared<FJsonValueObject>(Describe(*Pair.Value)));
		Result->SetArrayField(TEXT("recordings"), Recordings);
		Result->SetNumberField(TEXT("retentionSlots"), 4);
		return FMCPToolResult::Ok(Result);
	}
	FMCPToolResult ExecuteRecording(const FRecording& R, const TSharedPtr<FJsonObject>&) override { return FMCPToolResult::Ok(Describe(R)); }
};
class FRead final : public FStoredTool
{
public:
	using FStoredTool::FStoredTool;
	FString GetCapabilityId() const override { return TEXT("content.niagara.simcache.read"); }
	FMCPToolResult ExecuteRecording(const FRecording& R, const TSharedPtr<FJsonObject>& Params) override { return ReadPage(R, Params); }
};
class FRelease final : public FStoredTool
{
public:
	using FStoredTool::FStoredTool;
	FString GetCapabilityId() const override { return TEXT("content.niagara.simcache.release"); }
	FMCPToolResult ExecuteRecording(const FRecording& R, const TSharedPtr<FJsonObject>&) override
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("captureId"), R.Id);
		Result->SetBoolField(TEXT("released"), Store->Recordings.Remove(R.Id) == 1);
		return FMCPToolResult::Ok(Result);
	}
};
class FExport final : public FStoredTool
{
public:
	using FStoredTool::FStoredTool;
	FString GetCapabilityId() const override { return TEXT("content.niagara.simcache.export"); }
	FMCPToolResult ExecuteRecording(const FRecording& R, const TSharedPtr<FJsonObject>& Params) override
	{
		auto Page = ReadPage(R, Params);
		if (!Page.bSuccess) return Page;
		auto Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), TEXT("ue.niagara-simcache-export.v1"));
		Document->SetObjectField(TEXT("capture"), Describe(R));
		Document->SetObjectField(TEXT("page"), Page.Data);
		FString Json;
		if (!FJsonSerializer::Serialize(Document, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json)))
			return FMCPToolResult::Error(TEXT("Could not serialize SimCache observation."), TEXT("simcache_export_failed"));
		FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		FString Hash;
		if (!UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, Hash)) return FMCPToolResult::Error(TEXT("Could not hash exported observation."), TEXT("simcache_export_failed"));
		const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UEAI/NiagaraSimCache") / R.Id);
		// Content-addressed page files make retries idempotent; caller-controlled paths are never accepted.
		const FString Path = Directory / (Hash + TEXT(".json"));
		if (!IFileManager::Get().MakeDirectory(*Directory, true) || !FFileHelper::SaveArrayToFile(Bytes, *Path))
			return FMCPToolResult::Error(TEXT("Could not persist exported observation."), TEXT("simcache_export_failed"));
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("captureId"), R.Id);
		Result->SetStringField(TEXT("path"), Path);
		Result->SetStringField(TEXT("sha256"), Hash);
		Result->SetNumberField(TEXT("bytes"), Bytes.Num());
		Result->SetStringField(TEXT("format"), TEXT("json attribute page with capture metadata; not a replayable SimCache asset"));
		Result->SetObjectField(TEXT("page"), Page.Data);
		return FMCPToolResult::Ok(Result);
	}
};
#endif
}

namespace UEAIIntegrationTools
{
#if !WITH_UEAI_NIAGARA
class FUnavailableNiagaraSimCacheTool final : public FMCPToolBase
{
public:
	explicit FUnavailableNiagaraSimCacheTool(const TCHAR* InId) : Id(InId) {}
	FString GetCapabilityId() const override { return Id; }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(TEXT("Niagara support is not enabled in this plugin build."),
			TEXT("niagara_unavailable"), 503);
	}
private:
	FString Id;
};
#endif

void RegisterNiagaraSimCacheTools(FMCPToolRegistry& Registry)
{
#if WITH_UEAI_NIAGARA
	using namespace UEAINiagaraSimCachePrivate;
	auto Store = MakeShared<FStore>();
	Registry.Register(MakeShared<FCapture>(Store));
	Registry.Register(MakeShared<FInspect>(Store));
	Registry.Register(MakeShared<FRead>(Store));
	Registry.Register(MakeShared<FExport>(Store));
	Registry.Register(MakeShared<FRelease>(Store));
#else
	// Keep exact manifest bindings even when the optional feature is omitted,
	// as the other Niagara tools do. Availability checks reject these operations
	// without putting unrelated editor operations into service_degraded.
	for (const TCHAR* Id : {TEXT("content.niagara.simcache.capture"),
		TEXT("content.niagara.simcache.inspect"), TEXT("content.niagara.simcache.read"),
		TEXT("content.niagara.simcache.export"), TEXT("content.niagara.simcache.release")})
	{
		Registry.Register(MakeShared<FUnavailableNiagaraSimCacheTool>(Id));
	}
#endif
}
}
