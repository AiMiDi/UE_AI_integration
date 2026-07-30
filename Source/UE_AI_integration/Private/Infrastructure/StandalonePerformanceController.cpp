#include "Infrastructure/StandalonePerformanceController.h"

#include "Camera/CameraActor.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
FString ReadConsoleVariable(const TCHAR* Name)
{
	if (const IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(Name))
	{
		return Variable->GetString();
	}
	return TEXT("unavailable");
}

UWorld* FindGameWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if ((Context.WorldType == EWorldType::Game
				|| Context.WorldType == EWorldType::PIE)
			&& Context.World())
		{
			return Context.World();
		}
	}
	return nullptr;
}
}

FStandalonePerformanceController::FStandalonePerformanceController() = default;

FStandalonePerformanceController::~FStandalonePerformanceController()
{
	Stop();
}

bool FStandalonePerformanceController::StartFromCommandLine()
{
	const TCHAR* CommandLine = FCommandLine::Get();
	if (!FParse::Param(CommandLine, TEXT("UEAIPerformanceChild")))
	{
		return false;
	}
	if (!FParse::Value(CommandLine, TEXT("UEAIPerfJob="), JobId)
		|| !FParse::Value(
			CommandLine,
			TEXT("UEAIPerfJobDirectory="),
			JobDirectory)
		|| JobId.IsEmpty()
		|| JobDirectory.IsEmpty())
	{
		return false;
	}
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraName="), CameraName);
	FParse::Value(
		CommandLine,
		TEXT("UEAIPerfGameInstanceMode="),
		GameInstanceMode);
	if (GameInstanceMode != TEXT("project")
		&& GameInstanceMode != TEXT("minimal"))
	{
		GameInstanceMode = TEXT("project");
	}
	FParse::Value(
		CommandLine,
		TEXT("UEAIPerfWarmupSeconds="),
		WarmupSeconds);
	FParse::Value(
		CommandLine,
		TEXT("UEAIPerfSampleSeconds="),
		SampleSeconds);
	FParse::Value(
		CommandLine,
		TEXT("UEAIPerfStartupTimeoutSeconds="),
		StartupTimeoutSeconds);
	FParse::Value(CommandLine, TEXT("UEAIPerfRepeatCount="), RepeatCount);
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraX="), CameraLocation.X);
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraY="), CameraLocation.Y);
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraZ="), CameraLocation.Z);
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraPitch="), CameraRotation.Pitch);
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraYaw="), CameraRotation.Yaw);
	FParse::Value(CommandLine, TEXT("UEAIPerfCameraRoll="), CameraRotation.Roll);
	bCaptureTrace =
		FParse::Param(CommandLine, TEXT("UEAIPerfCaptureTrace"));
	WarmupSeconds = FMath::Clamp(WarmupSeconds, 0.0, 300.0);
	SampleSeconds = FMath::Clamp(SampleSeconds, 0.1, 3600.0);
	StartupTimeoutSeconds =
		FMath::Clamp(StartupTimeoutSeconds, 1.0, 3600.0);
	RepeatCount = FMath::Clamp(RepeatCount, 1, 20);
	JobDirectory = FPaths::ConvertRelativePathToFull(JobDirectory);
	IFileManager::Get().MakeDirectory(*JobDirectory, true);
	ReceiptPath =
		FPaths::Combine(JobDirectory, TEXT("standalone-child.json"));
	TracePath =
		FPaths::Combine(JobDirectory, TEXT("standalone.utrace"));
	StartupDeadlineSeconds =
		FPlatformTime::Seconds() + StartupTimeoutSeconds;
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(
			this,
			&FStandalonePerformanceController::Tick));
	return true;
}

void FStandalonePerformanceController::Stop()
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (FCsvProfiler::Get()->IsCapturing())
	{
		FCsvProfiler::Get()->EndCapture();
	}
	if (bTraceStarted && FTraceAuxiliary::IsConnected())
	{
		FTraceAuxiliary::Stop();
	}
	bTraceStarted = false;
	if (!JobId.IsEmpty()
		&& !ReceiptPath.IsEmpty()
		&& Phase != EPhase::Complete)
	{
		WriteReceipt(
			TEXT("failed"),
			TEXT("standalone_child_interrupted"),
			TEXT(
				"The standalone child began a managed measurement but "
				"the process shut down before the bounded evidence was "
				"completed."));
		Phase = EPhase::Complete;
	}
}

bool FStandalonePerformanceController::Tick(const float)
{
	const double Now = FPlatformTime::Seconds();
	if (Phase == EPhase::WaitingForWorld)
	{
		if (!ResolveAndLockCamera())
		{
			if (bCameraResolutionAmbiguous)
			{
				FinishFailure(
					TEXT("standalone_camera_ambiguous"),
					TEXT(
						"The declared standalone camera label matched "
						"multiple Camera Actors; use a unique Actor object "
						"name or label."));
				return false;
			}
			if (Now >= StartupDeadlineSeconds)
			{
				FinishFailure(
					TEXT("standalone_camera_unavailable"),
					TEXT(
						"The declared standalone camera could not be "
						"resolved and locked before the startup deadline."));
				return false;
			}
			return true;
		}
		if (bCaptureTrace)
		{
			FTraceAuxiliary::FOptions Options;
			Options.bTruncateFile = true;
			Options.bExcludeTail = true;
			bTraceStarted = FTraceAuxiliary::Start(
				FTraceAuxiliary::EConnectionType::File,
				*TracePath,
				TEXT("cpu,gpu,frame,bookmark,log"),
				&Options);
			if (!bTraceStarted)
			{
				FinishFailure(
					TEXT("trace_unavailable"),
					TEXT(
						"The standalone child could not start the "
						"requested Unreal Trace."));
				return false;
			}
		}
		BeginWarmup(Now);
		return true;
	}

	if (!ResolveAndLockCamera())
	{
		if (bCameraResolutionAmbiguous)
		{
			FinishFailure(
				TEXT("standalone_camera_ambiguous"),
				TEXT(
					"The declared standalone camera became ambiguous "
					"during the measurement."));
			return false;
		}
		FinishFailure(
			TEXT("standalone_camera_lost"),
			TEXT(
				"The declared camera or its player view target was lost "
				"during the standalone measurement."));
		return false;
	}
	if (Phase == EPhase::Warmup
		&& Now - PhaseStartedSeconds >= WarmupSeconds)
	{
		BeginSample(Now);
	}
	else if (Phase == EPhase::StartingCsv)
	{
		if (FCsvProfiler::Get()->IsCapturing())
		{
			Phase = EPhase::Sampling;
			PhaseStartedSeconds = Now;
		}
		else if (Now - PhaseStartedSeconds >= 10.0)
		{
			FinishFailure(
				TEXT("performance_csv_unavailable"),
				TEXT(
					"The standalone child could not begin CSV capture "
					"before the bounded deadline."));
			return false;
		}
	}
	else if (Phase == EPhase::Sampling
		&& Now - PhaseStartedSeconds >= SampleSeconds)
	{
		EndSample(Now);
	}
	else if (Phase == EPhase::WaitingForCsv
		&& CsvWriteFuture.IsSet()
		&& CsvWriteFuture->IsReady())
	{
		CompleteCsvWrite(Now);
	}
	else if (Phase == EPhase::FinalizingTrace)
	{
		FinalizeTrace(Now);
	}
	return Phase != EPhase::Complete;
}

int32 FStandalonePerformanceController::FindUniqueCameraMatch(
	const TArray<FString>& ObjectNames,
	const TArray<FString>& ActorLabels,
	const FString& RequestedName,
	bool& bOutAmbiguous)
{
	bOutAmbiguous = false;
	int32 MatchIndex = INDEX_NONE;
	for (int32 Index = 0; Index < ObjectNames.Num(); ++Index)
	{
		if (ObjectNames[Index] != RequestedName)
		{
			continue;
		}
		if (MatchIndex != INDEX_NONE)
		{
			bOutAmbiguous = true;
			return INDEX_NONE;
		}
		MatchIndex = Index;
	}
	if (MatchIndex != INDEX_NONE)
	{
		return MatchIndex;
	}
	for (int32 Index = 0; Index < ActorLabels.Num(); ++Index)
	{
		if (ActorLabels[Index] != RequestedName)
		{
			continue;
		}
		if (MatchIndex != INDEX_NONE)
		{
			bOutAmbiguous = true;
			return INDEX_NONE;
		}
		MatchIndex = Index;
	}
	return MatchIndex;
}

bool FStandalonePerformanceController::ResolveAndLockCamera()
{
	UWorld* World = FindGameWorld();
	if (!World)
	{
		return false;
	}
	ACameraActor* Camera = CameraActor.Get();
	if (!Camera)
	{
		TArray<ACameraActor*> Candidates;
		TArray<FString> ObjectNames;
		TArray<FString> ActorLabels;
		for (TActorIterator<ACameraActor> It(World); It; ++It)
		{
			Candidates.Add(*It);
			ObjectNames.Add(It->GetName());
#if WITH_EDITOR
			ActorLabels.Add(It->GetActorLabel());
#else
			ActorLabels.Add(FString());
#endif
		}
		const int32 MatchIndex = FindUniqueCameraMatch(
			ObjectNames,
			ActorLabels,
			CameraName,
			bCameraResolutionAmbiguous);
		if (Candidates.IsValidIndex(MatchIndex))
		{
			Camera = Candidates[MatchIndex];
			CameraActor = Camera;
		}
	}
	APlayerController* Controller = World->GetFirstPlayerController();
	if (!Camera || !Controller)
	{
		return false;
	}
	Camera->SetActorLocationAndRotation(
		CameraLocation,
		CameraRotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	if (Controller->GetViewTarget() != Camera)
	{
		Controller->SetViewTarget(Camera);
	}
	const bool bLocationMatches =
		Camera->GetActorLocation().Equals(CameraLocation, 0.1);
	const bool bRotationMatches =
		Camera->GetActorRotation().Equals(CameraRotation, 0.1);
	bCameraLockedAndVerified =
		Controller->GetViewTarget() == Camera
		&& bLocationMatches
		&& bRotationMatches;
	return bCameraLockedAndVerified;
}

void FStandalonePerformanceController::BeginWarmup(const double NowSeconds)
{
	Phase = EPhase::Warmup;
	PhaseStartedSeconds = NowSeconds;
}

void FStandalonePerformanceController::BeginSample(const double NowSeconds)
{
	const FString Filename = FString::Printf(
		TEXT("standalone-repeat-%02d.csv"),
		RepeatIndex + 1);
	FCsvProfiler::Get()->BeginCapture(
		-1,
		JobDirectory,
		Filename,
		ECsvProfilerFlags::None);
	Phase = EPhase::StartingCsv;
	PhaseStartedSeconds = NowSeconds;
}

void FStandalonePerformanceController::EndSample(const double NowSeconds)
{
	const double ActualSeconds =
		FMath::Max(0.0, NowSeconds - PhaseStartedSeconds);
	TSharedPtr<FJsonObject> Repeat = MakeShared<FJsonObject>();
	Repeat->SetNumberField(TEXT("repeatIndex"), RepeatIndex + 1);
	Repeat->SetNumberField(TEXT("requestedSampleSeconds"), SampleSeconds);
	Repeat->SetNumberField(TEXT("actualSampleSeconds"), ActualSeconds);
	RepeatReceipts.Add(MakeShared<FJsonValueObject>(Repeat));
	CsvWriteFuture = FCsvProfiler::Get()->EndCapture();
	Phase = EPhase::WaitingForCsv;
	PhaseStartedSeconds = NowSeconds;
}

void FStandalonePerformanceController::CompleteCsvWrite(
	const double NowSeconds)
{
	const FString CsvPath = CsvWriteFuture->Get();
	CsvWriteFuture.Reset();
	if (CsvPath.IsEmpty() || !IFileManager::Get().FileExists(*CsvPath))
	{
		FinishFailure(
			TEXT("performance_csv_unavailable"),
			TEXT(
				"The standalone child finished a sample window but its "
				"CSV evidence was not written."));
		return;
	}
	CsvPaths.Add(FPaths::ConvertRelativePathToFull(CsvPath));
	++RepeatIndex;
	if (RepeatIndex < RepeatCount)
	{
		BeginWarmup(NowSeconds);
		return;
	}
	if (bTraceStarted)
	{
		if (!FTraceAuxiliary::Stop())
		{
			FinishFailure(
				TEXT("trace_stop_failed"),
				TEXT("The standalone child could not stop Unreal Trace."));
			return;
		}
		bTraceStarted = false;
		Phase = EPhase::FinalizingTrace;
		TraceFinalizeDeadlineSeconds = NowSeconds + 15.0;
		return;
	}
	FinishSuccess();
}

void FStandalonePerformanceController::FinalizeTrace(
	const double NowSeconds)
{
	if (IFileManager::Get().FileSize(*TracePath) > 0)
	{
		FinishSuccess();
		return;
	}
	if (NowSeconds >= TraceFinalizeDeadlineSeconds)
	{
		FinishFailure(
			TEXT("trace_artifact_unavailable"),
			TEXT(
				"The standalone child stopped Unreal Trace but the "
				"artifact was not flushed before the deadline."));
	}
}

void FStandalonePerformanceController::FinishSuccess()
{
	WriteReceipt(TEXT("succeeded"));
	Phase = EPhase::Complete;
	FPlatformMisc::RequestExit(false);
}

void FStandalonePerformanceController::FinishFailure(
	const FString& Code,
	const FString& Message)
{
	WriteReceipt(TEXT("failed"), Code, Message);
	Phase = EPhase::Complete;
	FPlatformMisc::RequestExit(false);
}

void FStandalonePerformanceController::WriteReceipt(
	const FString& Status,
	const FString& ErrorCode,
	const FString& Message)
{
	TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(
		TEXT("schema"),
		TEXT("ue.performance-standalone-child.v1"));
	Receipt->SetStringField(TEXT("jobId"), JobId);
	Receipt->SetStringField(TEXT("status"), Status);
	Receipt->SetStringField(TEXT("errorCode"), ErrorCode);
	Receipt->SetStringField(TEXT("message"), Message);
	Receipt->SetStringField(
		TEXT("completedAtUtc"),
		FDateTime::UtcNow().ToIso8601());
	Receipt->SetNumberField(TEXT("completedRepeatCount"), RepeatIndex);
	Receipt->SetNumberField(
		TEXT("startupTimeoutSeconds"),
		StartupTimeoutSeconds);
	Receipt->SetArrayField(TEXT("repetitions"), RepeatReceipts);
	TArray<TSharedPtr<FJsonValue>> CsvValues;
	for (const FString& CsvPath : CsvPaths)
	{
		CsvValues.Add(MakeShared<FJsonValueString>(CsvPath));
	}
	Receipt->SetArrayField(TEXT("csvFiles"), CsvValues);
	Receipt->SetBoolField(
		TEXT("traceAvailable"),
		bCaptureTrace && IFileManager::Get().FileSize(*TracePath) > 0);
	Receipt->SetStringField(TEXT("tracePath"), TracePath);
	TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();
	Camera->SetStringField(TEXT("requestedName"), CameraName);
	if (const ACameraActor* ResolvedCamera = CameraActor.Get())
	{
		Camera->SetStringField(TEXT("objectName"), ResolvedCamera->GetName());
#if WITH_EDITOR
		Camera->SetStringField(TEXT("actorLabel"), ResolvedCamera->GetActorLabel());
#endif
		const FVector Location = ResolvedCamera->GetActorLocation();
		const FRotator Rotation = ResolvedCamera->GetActorRotation();
		Camera->SetArrayField(
			TEXT("location"),
			{
				MakeShared<FJsonValueNumber>(Location.X),
				MakeShared<FJsonValueNumber>(Location.Y),
				MakeShared<FJsonValueNumber>(Location.Z)
			});
		Camera->SetArrayField(
			TEXT("rotation"),
			{
				MakeShared<FJsonValueNumber>(Rotation.Pitch),
				MakeShared<FJsonValueNumber>(Rotation.Yaw),
				MakeShared<FJsonValueNumber>(Rotation.Roll)
			});
	}
	Camera->SetBoolField(
		TEXT("lockedAndVerified"),
		bCameraLockedAndVerified);
	Receipt->SetObjectField(TEXT("camera"), Camera);
	Receipt->SetObjectField(
		TEXT("runtimeFingerprint"),
		MakeRuntimeFingerprint());

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (FJsonSerializer::Serialize(Receipt.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(Json, *ReceiptPath);
	}
}

TSharedPtr<FJsonObject>
FStandalonePerformanceController::MakeRuntimeFingerprint() const
{
	TSharedPtr<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(
		TEXT("provenance"),
		TEXT("standaloneChildRuntime"));
	Fingerprint->SetStringField(
		TEXT("rhi"),
		GDynamicRHI ? FString(GDynamicRHI->GetName()) : TEXT("unavailable"));
	Fingerprint->SetStringField(TEXT("gpuAdapter"), GRHIAdapterName);
	Fingerprint->SetStringField(
		TEXT("gpuDriver"),
		GRHIAdapterUserDriverVersion);
	Fingerprint->SetStringField(
		TEXT("gpuDriverInternal"),
		GRHIAdapterInternalDriverVersion);
	Fingerprint->SetNumberField(TEXT("gpuVendorId"), GRHIVendorId);
	Fingerprint->SetNumberField(TEXT("gpuDeviceId"), GRHIDeviceId);
	Fingerprint->SetStringField(
		TEXT("resolution"),
		FString::Printf(
			TEXT("%dx%d"),
			GSystemResolution.ResX,
			GSystemResolution.ResY));
	Fingerprint->SetStringField(TEXT("windowMode"), TEXT("visibleWindow"));
	Fingerprint->SetStringField(
		TEXT("gameInstanceMode"),
		GameInstanceMode);
	TSharedPtr<FJsonObject> CVars = MakeShared<FJsonObject>();
	static const TCHAR* Names[] = {
		TEXT("r.VSync"),
		TEXT("t.MaxFPS"),
		TEXT("r.ScreenPercentage"),
		TEXT("r.DynamicRes.OperationMode"),
		TEXT("sg.ResolutionQuality"),
		TEXT("sg.ViewDistanceQuality"),
		TEXT("sg.AntiAliasingQuality"),
		TEXT("sg.ShadowQuality"),
		TEXT("sg.GlobalIlluminationQuality"),
		TEXT("sg.ReflectionQuality"),
		TEXT("sg.PostProcessQuality"),
		TEXT("sg.TextureQuality"),
		TEXT("sg.EffectsQuality"),
		TEXT("sg.FoliageQuality"),
		TEXT("sg.ShadingQuality")
	};
	for (const TCHAR* Name : Names)
	{
		CVars->SetStringField(Name, ReadConsoleVariable(Name));
	}
	Fingerprint->SetObjectField(TEXT("cvars"), CVars);
	Fingerprint->SetStringField(
		TEXT("vsync"),
		ReadConsoleVariable(TEXT("r.VSync")));
	Fingerprint->SetStringField(
		TEXT("fpsCap"),
		ReadConsoleVariable(TEXT("t.MaxFPS")));
	Fingerprint->SetStringField(
		TEXT("screenPercentage"),
		ReadConsoleVariable(TEXT("r.ScreenPercentage")));
	return Fingerprint;
}
