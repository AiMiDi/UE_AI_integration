// Thread-safe client sessions, execution activity, and Editor-session statistics.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"

namespace UEAIIntegration::Infrastructure
{
struct FCallerContext
{
	FString ClientKind = TEXT("legacy");
	FString Name = TEXT("Legacy HTTP");
	FString Version;
	FString Transport = TEXT("http");
	FString Command;
	FString InstanceId;
	FString InvocationId;
	FString SessionId;
	uint32 Pid = 0;
};

struct FClientRegistration
{
	FString ClientKind;
	FString Name;
	FString Version;
	FString Transport;
	FString Command;
	FString InstanceId;
	FString InvocationId;
	uint32 Pid = 0;
};

/**
 * Owns caller presence, a bounded metadata-only activity ring, and independent
 * cumulative counters for the current Editor session.
 *
 * Public methods are safe on HTTP worker threads. The service owns no UObject
 * references and never stores request params, response bodies, or asset data.
 */
class FClientActivityService
{
public:
	static constexpr int32 MaxSessions = 128;
	static constexpr int32 ActivityCapacity = 500;
	static constexpr int32 MaxRecentCliInvocations = 100;
	static constexpr int32 MaxDurationSamples = 4096;
	static constexpr int32 MaxTrackedRuns = 2048;
	static constexpr double SessionTimeoutSeconds = 15.0;
	static constexpr int32 HeartbeatIntervalMs = 5000;
	static constexpr int32 ExpiresAfterMs = 15000;

	FClientActivityService() = default;

	bool RegisterClient(
		const FClientRegistration& Registration,
		FString& OutSessionId,
		FString& OutError);
	bool Heartbeat(const FString& SessionId, FString& OutError);
	bool UnregisterClient(const FString& SessionId, FString& OutError);
	void DisconnectAllSessions();

	/** Starts an HTTP request and pins a registered session until EndRequest. */
	bool BeginRequest(FCallerContext& Caller, FString& OutError);
	void EndRequest(const FCallerContext& Caller);
	void ExpireSessions(double CurrentSeconds = -1.0);

	FString BeginActivity(
		const FCallerContext& Caller,
		const FString& Kind);
	void MarkActivityStarted(const FString& EventId);
	void UpdateCapabilityActivity(
		const FString& EventId,
		const FString& Capability,
		const FString& RequestId,
		const FString& Risk);
	void UpdateWorkflowActivity(
		const FString& EventId,
		const FString& WorkflowAction,
		const FString& RequestId,
		const FString& Risk);
	void MarkActivityRejected(const FString& EventId);
	void CompleteActivityFromHttp(
		const FString& EventId,
		int32 HttpStatus,
		const TArray<uint8>& ResponseBody);

	/** Snapshot is sorted for direct menu rendering and bounded by both limits. */
	TSharedPtr<FJsonObject> MakeSnapshot(
		int32 RecentExecutionLimit = 10,
		int32 RecentCliLimit = 5) const;
	int32 GetOnlineMcpCount() const;
	int32 GetRunningCliCount() const;
	FString GetLastExecutionResult() const;

private:
	struct FSession
	{
		FString SessionId;
		FClientRegistration Registration;
		FString RegisteredAtUtc;
		FString LastActivityAtUtc;
		double RegisteredSeconds = 0.0;
		double LastActivitySeconds = 0.0;
		int32 ActiveRequestCount = 0;
		int64 CallCount = 0;
	};

	struct FCliInvocation
	{
		FString InvocationId;
		FString SessionId;
		FString Name;
		FString Version;
		FString Command;
		uint32 Pid = 0;
		FString Status = TEXT("running");
		FString StartedAtUtc;
		FString FinishedAtUtc;
		double StartedSeconds = 0.0;
		double DurationMs = 0.0;
		int32 ActiveRequestCount = 0;
		bool bHadFailure = false;
	};

	struct FActivity
	{
		FString EventId;
		FCallerContext Caller;
		FString Kind;
		FString Capability;
		FString WorkflowAction;
		FString RequestId;
		FString RunId;
		FString JobId;
		FString Risk;
		FString Status = TEXT("queued");
		FString ErrorCode;
		FString QueuedAtUtc;
		FString StartedAtUtc;
		FString FinishedAtUtc;
		double QueuedSeconds = 0.0;
		double StartedSeconds = 0.0;
		double DurationMs = 0.0;
		int32 HttpStatus = 0;
	};

	struct FRunStatistics
	{
		FString Status;
		int64 OperationTotal = 0;
		int64 OperationSucceeded = 0;
		uint64 LastTouchedOrdinal = 0;
	};

	struct FStatistics
	{
		int64 CapabilityCalls = 0;
		int64 CapabilitySucceeded = 0;
		int64 CapabilityFailed = 0;
		int64 WorkflowApiCalls = 0;
		int64 DslRuns = 0;
		int64 DslCompleted = 0;
		int64 DslFailed = 0;
		int64 DslBlocked = 0;
		int64 Rollbacks = 0;
		int64 OperationTotal = 0;
		int64 OperationSucceeded = 0;
		int64 CliInvocations = 0;
		TArray<double> CapabilityDurationsMs;
		TArray<double> DslRunDurationsMs;
		TMap<FString, FRunStatistics> Runs;
	};

	static bool IsBoundedToken(
		const FString& Value,
		int32 MaxLength,
		bool bAllowEmpty = false);
	static double Percentile(TArray<double> Values, double Quantile);
	static TSharedPtr<FJsonObject> SessionToJson(
		const FSession& Session,
		double NowSeconds);
	static TSharedPtr<FJsonObject> InvocationToJson(
		const FCliInvocation& Invocation,
		double NowSeconds);
	static TSharedPtr<FJsonObject> ActivityToJson(const FActivity& Activity);

	void FinishInvocationLocked(
		const FString& InvocationId,
		const FString& Status,
		double FinishedSeconds,
		const FString& FinishedAtUtc);
	void RecordCliInvocationLocked(const FCallerContext& Caller);
	void UpdateStatisticsLocked(
		const FActivity& Activity,
		const TSharedPtr<FJsonObject>& Data,
		const FString& RemoteStatus);
	void UpdateRunStatisticsLocked(
		const FString& RunId,
		const FString& Status,
		const TSharedPtr<FJsonObject>& Data);
	void PruneActivitiesLocked();
	void PruneCliInvocationsLocked();
	void PruneRunStatisticsLocked();
	static void AddDurationSample(
		TArray<double>& Samples,
		double DurationMs);
	FActivity* FindActivityLocked(const FString& EventId);

	mutable FCriticalSection Mutex;
	TMap<FString, FSession> Sessions;
	TMap<FString, FString> SessionByInstanceId;
	TArray<FCliInvocation> CliInvocations;
	TArray<FActivity> Activities;
	FStatistics Statistics;
	uint64 RunTouchOrdinal = 0;
};
}
