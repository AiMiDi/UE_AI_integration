#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/**
 * Child-process controller used only by production.performance.run with
 * executionTarget=standalone. It owns the wall-clock CSV windows, locks the
 * declared camera, records the child runtime fingerprint, and exits the child
 * only after all evidence has been flushed.
 */
class FStandalonePerformanceController
{
public:
	FStandalonePerformanceController();
	~FStandalonePerformanceController();

	/** Returns true when the current process is a managed performance child. */
	bool StartFromCommandLine();
	void Stop();

	/** Pure name/label resolver used by the runtime and contract tests. */
	static int32 FindUniqueCameraMatch(
		const TArray<FString>& ObjectNames,
		const TArray<FString>& ActorLabels,
		const FString& RequestedName,
		bool& bOutAmbiguous);

private:
	enum class EPhase : uint8
	{
		WaitingForWorld,
		Warmup,
		StartingCsv,
		Sampling,
		WaitingForCsv,
		FinalizingTrace,
		Complete
	};

	bool Tick(float DeltaTime);
	bool ResolveAndLockCamera();
	void BeginWarmup(double NowSeconds);
	void BeginSample(double NowSeconds);
	void EndSample(double NowSeconds);
	void CompleteCsvWrite(double NowSeconds);
	void FinalizeTrace(double NowSeconds);
	void FinishSuccess();
	void FinishFailure(const FString& Code, const FString& Message);
	void WriteReceipt(
		const FString& Status,
		const FString& ErrorCode = FString(),
		const FString& Message = FString());
	TSharedPtr<FJsonObject> MakeRuntimeFingerprint() const;

	FTSTicker::FDelegateHandle TickHandle;
	EPhase Phase = EPhase::WaitingForWorld;
	FString JobId;
	FString JobDirectory;
	FString ReceiptPath;
	FString CameraName;
	FString GameInstanceMode = TEXT("project");
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	double WarmupSeconds = 0.0;
	double SampleSeconds = 0.0;
	double StartupTimeoutSeconds = 300.0;
	double StartupDeadlineSeconds = 0.0;
	double PhaseStartedSeconds = 0.0;
	double TraceFinalizeDeadlineSeconds = 0.0;
	int32 RepeatCount = 1;
	int32 RepeatIndex = 0;
	bool bCaptureTrace = false;
	bool bTraceStarted = false;
	bool bCameraResolutionAmbiguous = false;
	bool bCameraLockedAndVerified = false;
	FString TracePath;
	TWeakObjectPtr<class ACameraActor> CameraActor;
	TOptional<TSharedFuture<FString>> CsvWriteFuture;
	TArray<FString> CsvPaths;
	TArray<TSharedPtr<FJsonValue>> RepeatReceipts;
};
