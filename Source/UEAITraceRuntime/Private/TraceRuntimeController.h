#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"

#include <atomic>

class FJsonObject;

namespace UEAI::TraceRuntime
{
class FController
{
public:
	FController();
	~FController();

	bool StartFromCommandLine();
	void Stop();

private:
	enum class EPhase : uint8
	{
		Inactive,
		Loading,
		Connecting,
		Recording,
		Complete
	};

	bool LoadAndValidateDescriptor(
		const FString& DescriptorPath,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool Tick(float DeltaSeconds);
	bool BeginRecording();
	bool ActivateRecording();
	void PublishNetworkMetadata(bool bReplayMissingHistory);
	bool HasStartedGameWorld() const;
	void HandleEngineLoopInitComplete();
	void HandleTraceConnected();
	void RemoveTraceConnectionDelegate();
	bool ReadStopRequest(FString& OutError) const;
	void Finish(
		const FString& Status,
		const FString& ErrorCode = FString(),
		const FString& Message = FString(),
		bool bPartial = false);
	bool WriteReceiptAtomic(
		const FString& Phase,
		const FString& Status,
		const FString& ErrorCode,
		const FString& Message,
		bool bPartial,
		const FString& Destination) const;

	EPhase Phase = EPhase::Inactive;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle EngineLoopInitCompleteHandle;
	FDelegateHandle TraceConnectionHandle;
	FString JobId;
	FString RequestId;
	FString LaunchPlanDigest;
	FString LaunchNonce;
	FString JobDirectory;
	FString TracePath;
	FString ReceiptPath;
	FString HeartbeatPath;
	FString StopRequestPath;
	FString StopNonce;
	FString ChannelList;
	FString RegionName;
	FString StartedAtUtc;
	FString TraceFinalizationStatus = TEXT("notStarted");
	double LaunchSeconds = 0.0;
	double TraceConnectionStartedSeconds = 0.0;
	double StartedSeconds = 0.0;
	double LastHeartbeatSeconds = 0.0;
	double MaxDurationSeconds = 120.0;
	double StartupTimeoutSeconds = 180.0;
	int64 MaxFileSizeBytes = 4ll * 1024 * 1024 * 1024;
	bool bExitOnStop = false;
	bool bTraceStarted = false;
	bool bNetTraceRequested = false;
	bool bNetTraceActivated = false;
	bool bNetworkSessionPredatesTrace = false;
	bool bNetworkReplayTruncated = false;
	int32 ReplayedNetDriverCount = 0;
	int32 ReplayedNetConnectionCount = 0;
	uint32 PreviousNetTraceVerbosity = 0;
	bool bEngineLoopInitComplete = false;
	std::atomic<bool> bTraceConnected{false};
};
}
