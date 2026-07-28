#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Tools/MCPToolBase.h"

namespace UEAIIntegration::Infrastructure
{
/**
 * Durable, subsystem-owned runtime for bounded production jobs and artifacts.
 *
 * Public operations never provide an executable or raw command line. Each
 * operation is translated to a constrained process/sampler description before
 * it reaches this runtime.
 */
class FProductionJobRuntime
{
public:
	FProductionJobRuntime();
	~FProductionJobRuntime();

	void Tick(float DeltaTime);

	/** Dispatch one manifest-backed production operation. */
	FMCPToolResult Execute(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params);

	/** Testable contract helpers used by ProductionJobRuntimeTests. */
	static FString ComputeChangePlanDigest(
		const TSharedPtr<FJsonObject>& Request);
	static bool IsPathWithin(
		const FString& Candidate,
		const FString& AllowedRoot);
	static TSharedPtr<FJsonObject> SummarizeMetric(
		const TArray<double>& Samples,
		double BudgetMs);

private:
	struct FArtifact
	{
		FString Id;
		FString Kind;
		FString Name;
		FString Path;
		FString MimeType;
		FString Sha256;
		int64 Size = 0;
		bool bSha256Deferred = false;
		FDateTime RegisteredModifiedAtUtc;
	};

	struct FJob
	{
		FString Id;
		FString Kind;
		FString Status;
		FString Phase;
		FString Message;
		FString ErrorCode;
		FString CreatedAtUtc;
		FString StartedAtUtc;
		FString CompletedAtUtc;
		FString RequestId;
		FString InputDigest;
		double Progress = 0.0;
		double StartedAtSeconds = 0.0;
		double TimeoutAtSeconds = 0.0;
		uint32 ProcessId = 0;
		int32 ReturnCode = INDEX_NONE;
		FProcHandle ProcessHandle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FString Executable;
		FString Arguments;
		FString WorkingDirectory;
		FString Output;
		int64 LogBaseCursor = 0;
		int64 LogTotalChars = 0;
		FString PostProcess;
		TSharedPtr<FJsonObject> Input;
		TSharedPtr<FJsonObject> Result;
		TArray<FArtifact> Artifacts;
		int64 SynchronousArtifactHashBytes = 0;

		// In-Editor performance sampler state.
		double WarmupUntilSeconds = 0.0;
		double SamplingUntilSeconds = 0.0;
		double BudgetMs = 16.6667;
		TMap<FString, TArray<double>> MetricSamples;
		bool bOwnsTrace = false;
		FString TraceJobId;
	};

	FMCPToolResult GetJobStatus(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult CancelJob(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetJobResult(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetJobLog(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetJobArtifact(const TSharedPtr<FJsonObject>& Params) const;

	FMCPToolResult StartTrace(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetTraceStatus(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StopTrace(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult AnalyzeTrace(const TSharedPtr<FJsonObject>& Params);

	FMCPToolResult StartPerformanceRun(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetPerformanceResult(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ComparePerformanceRuns(const TSharedPtr<FJsonObject>& Params) const;

	FMCPToolResult ListTests(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StartTestRun(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetTestResult(const TSharedPtr<FJsonObject>& Params) const;

	FMCPToolResult StartCook(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult StartPackage(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult StartCommandlet(const TSharedPtr<FJsonObject>& Params);

	FMCPToolResult GetSourceControlRepository(
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetSourceControlStatus(
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetSourceControlDiff(
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult PlanSourceControlChange(
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ExecuteSourceControlChange(
		const TSharedPtr<FJsonObject>& Params);

	FMCPToolResult GetDdcStatus(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StartDdcJob(const TSharedPtr<FJsonObject>& Params);

	FMCPToolResult ValidateBuildGraph(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StartBuildGraph(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetHordeContext(const TSharedPtr<FJsonObject>& Params) const;

	TSharedPtr<FJob> StartProcessJob(
		const FString& Kind,
		const FString& Executable,
		const FString& Arguments,
		const FString& WorkingDirectory,
		double TimeoutSeconds,
		const FString& PostProcess,
		const TSharedPtr<FJsonObject>& Input,
		FString& OutError);
	TSharedPtr<FJob> CreateJob(
		const FString& Kind,
		const TSharedPtr<FJsonObject>& Input);
	TSharedPtr<FJob> FindIdempotentJob(
		const FString& Kind,
		const TSharedPtr<FJsonObject>& Input,
		bool& bOutConflict) const;
	void TickProcessJob(FJob& Job);
	void TickPerformanceJob(FJob& Job);
	void FinishJob(
		FJob& Job,
		const FString& Status,
		const FString& ErrorCode = FString(),
		const FString& Message = FString());
	void PostProcessJob(FJob& Job);
	void WriteTestReports(FJob& Job);
	void WritePerformanceReport(FJob& Job);

	FArtifact* AddArtifact(
		FJob& Job,
		const FString& Path,
		const FString& Name,
		const FString& MimeType);
	TSharedPtr<FJsonObject> MakeJobSummary(
		const FJob& Job,
		bool bIncludeResult) const;
	static TSharedPtr<FJsonObject> MakeArtifactSummary(
		const FArtifact& Artifact);
	static TSharedPtr<FJsonObject> MakeRuntimeContext();

	void LoadJournals();
	void SaveJournal(const FJob& Job) const;
	FString JobDirectory(const FString& JobId) const;
	static FString JobsRoot();
	static FString NewOpaqueId(const TCHAR* Prefix);
	static FString GetStringFieldOr(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& DefaultValue = FString());
	static double GetNumberFieldOr(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		double DefaultValue);
	static bool GetBoolFieldOr(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		bool DefaultValue);
	static bool IsSafeToken(const FString& Value, int32 MaxLength = 128);
	static bool IsTerminalStatus(const FString& Status);
	static FString QuoteArgument(const FString& Value);
	static bool RunGitSync(
		const FString& Arguments,
		FString& OutStdOut,
		FString& OutStdErr,
		int32& OutReturnCode);
	static int64 DirectorySize(const FString& Directory);

	TMap<FString, TSharedPtr<FJob>> Jobs;
	FString ActiveHeavyJobId;
	FString ActiveTraceJobId;
};
}
