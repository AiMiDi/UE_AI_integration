#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UEAI::TraceWorker
{
class FTraceStore;

/**
 * Constrained Development trace launcher used by one-shot worker processes.
 * Plans and jobs are persisted under the current user's Trace Worker store.
 */
class FTraceWorkerLaunch
{
public:
	explicit FTraceWorkerLaunch(
		const FString& CommandLine,
		const FTraceStore& TraceStore);

	bool Handles(const FString& Capability) const;
	bool Execute(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestId,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);

private:
	struct FLaunchProfile
	{
		FString Id;
		FString ExecutableKind;
		TArray<FString> Configurations;
		TArray<FString> AllowedMaps;
		TArray<FString> FixedArguments;
		TSet<FString> AllowedCvars;
		double MaxDurationSeconds = 3600.0;
		int64 MaxFileSizeMiB = 16384;
		double StartupTimeoutSeconds = 180.0;
		double ShutdownTimeoutSeconds = 60.0;
		bool bAllowForcedTermination = false;
	};

	struct FLaunchPlan
	{
		FString Digest;
		FString ProfileId;
		FString ProjectPath;
		FString ProjectSha256;
		FString Map;
		FString Preset;
		FString ExecutablePath;
		FString ExecutableSha256;
		FString Configuration;
		FString PostStop = TEXT("artifactOnly");
		TArray<FString> Channels;
		TArray<FString> FixedArguments;
		TMap<FString, FString> Cvars;
		double MaxDurationSeconds = 120.0;
		int64 MaxFileSizeMiB = 4096;
		double StartupTimeoutSeconds = 180.0;
		double ShutdownTimeoutSeconds = 60.0;
		bool bAllowForcedTermination = false;
	};

	struct FLaunchJob
	{
		FString JobId;
		FString PlanDigest;
		FString RequestId;
		FString ProjectPath;
		FString Map;
		FString JobDirectory;
		FString DescriptorPath;
		FString ReceiptPath;
		FString StopPath;
		FString TracePath;
		FString LogPath;
		FString StopNonce;
		FString LaunchedAtUtc;
		FString StopRequestedAtUtc;
		FString ArtifactTraceId;
		FString PostStop = TEXT("artifactOnly");
		FString PostStopAnalysisId;
		FString PostStopAnalysisStatus = TEXT("pending");
		FString ExecutablePath;
		FString ExecutableSha256;
		FString LaunchNonce;
		FString ProcessCreationIdentity;
		FString ProcessIdentityStatus = TEXT("pending");
		FString ErrorCode;
		FString ErrorMessage;
		FString LastHeartbeatAtUtc;
		FString Phase = TEXT("launching");
		TArray<FString> PhaseHistory = {TEXT("launching")};
		FString Status = TEXT("running");
		uint32 ProcessId = 0;
		double StopRequestedUnixSeconds = 0.0;
		double StartupTimeoutSeconds = 180.0;
		double ShutdownTimeoutSeconds = 60.0;
		bool bAllowForcedTermination = false;
		bool bForcedTermination = false;
		bool bPartial = false;
	};

	bool LoadProfiles(
		TArray<FLaunchProfile>& OutProfiles,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	bool LoadPlan(
		const FString& Digest,
		FLaunchPlan& OutPlan,
		FString& OutError) const;
	bool SavePlan(const FLaunchPlan& Plan, FString& OutError) const;
	bool LoadJob(
		const FString& JobId,
		FLaunchJob& OutJob,
		FString& OutError) const;
	bool SaveJob(const FLaunchJob& Job, FString& OutError) const;
	static void TransitionPhase(FLaunchJob& Job, const FString& Phase);

	bool TargetList(
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	bool ChannelList(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	bool LaunchPlan(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool Start(
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestId,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool Status(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool Stop(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool RouteGenericJob(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool ReadJobLog(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool ReadJobArtifact(
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	FString ResolveProfilesPath() const;
	FString PlanPath(const FString& Digest) const;
	FString JobPath(const FString& JobId) const;
	FString RequestPath(const FString& RequestId) const;
	FString CanonicalPlan(const FLaunchPlan& Plan) const;
	TSharedPtr<FJsonObject> EffectiveConfig(const FLaunchPlan& Plan) const;
	bool ValidateStartMatchesPlan(
		const TSharedPtr<FJsonObject>& Params,
		const FLaunchPlan& Plan,
		FString& OutError) const;
	bool ClaimRequest(
		const FString& RequestId,
		const FString& PlanDigest,
		const FString& JobId,
		bool& bOutCreated,
		FString& OutExistingPlanDigest,
		FString& OutExistingJobId,
		FString& OutError) const;
	bool ResolveProjectExecutable(
		const FString& ProjectPath,
		const FLaunchProfile& Profile,
		FString& OutExecutable,
		FString& OutConfiguration) const;
	bool HashFile(
		const FString& Path,
		FString& OutSha256,
		FString& OutError) const;
	bool HashText(const FString& Text, FString& OutSha256) const;
	bool PublishJson(
		const FString& Path,
		const TSharedPtr<FJsonObject>& Object,
		FString& OutError) const;
	bool RefreshJob(
		FLaunchJob& Job,
		TSharedPtr<FJsonObject>& OutReceipt,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	bool OpenVerifiedProcess(
		FLaunchJob& Job,
		FProcHandle& OutProcess,
		bool& bOutMustClose,
		FString& OutReason) const;
	void StopUntrackedProcess(
		const FLaunchJob& Job,
		FProcHandle& Process) const;
	TSharedPtr<FJsonObject> JobToResult(
		const FLaunchJob& Job,
		const TSharedPtr<FJsonObject>& Receipt) const;

	FString CommandLine;
	FString StoreRoot;
	const FTraceStore& TraceStore;
};
}
