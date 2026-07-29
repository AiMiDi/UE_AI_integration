#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Tools/MCPToolBase.h"

namespace TraceServices
{
class IAnalysisSession;
}

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
	using FScenarioOperation =
		TFunction<FMCPToolResult(const TSharedPtr<FJsonObject>&)>;

	explicit FProductionJobRuntime(
		FScenarioOperation InStartScenario = FScenarioOperation(),
		FScenarioOperation InGetScenarioStatus = FScenarioOperation(),
		FScenarioOperation InGetScenarioResult = FScenarioOperation(),
		FScenarioOperation InCancelScenario = FScenarioOperation());
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
	static TSharedPtr<FJsonObject> ComparePerformanceResults(
		const TSharedPtr<FJsonObject>& BaselineResult,
		const TSharedPtr<FJsonObject>& CandidateResult,
		const TSharedPtr<FJsonObject>& Params);
	static TSharedPtr<FJsonObject> ResolveProcessTimeoutPolicy(
		const TSharedPtr<FJsonObject>& Params,
		double DefaultExecutionTimeoutSeconds);
	static FString BuildHeadlessProfileArguments(
		const FString& Profile,
		FString& OutError);
	static FString InferAutomationPhase(
		const FString& CurrentPhase,
		const FString& LogChunk);

private:
	struct FProcessLaunchSpec
	{
		FString Kind;
		FString Executable;
		FString Arguments;
		FString WorkingDirectory;
		FString PostProcess;
		double DefaultExecutionTimeoutSeconds = 1800.0;
		bool bEditorProcess = false;
		bool bAutomationProcess = false;
		TFunction<FString(
			const FString& JobId,
			const FString& JobDirectory,
			const FString& EditorLogPath,
			const FString& ReportDirectory)> BuildArguments;
	};

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
		double CreatedAtSeconds = 0.0;
		double StartedAtSeconds = 0.0;
		double TimeoutAtSeconds = 0.0;
		double PhaseStartedAtSeconds = 0.0;
		double StartupTimeoutSeconds = 300.0;
		double ExecutionTimeoutSeconds = 1800.0;
		double ShutdownTimeoutSeconds = 60.0;
		double HardTimeoutSeconds = 2160.0;
		double StartupDeadlineSeconds = 0.0;
		double ExecutionDeadlineSeconds = 0.0;
		double ShutdownDeadlineSeconds = 0.0;
		double HardDeadlineSeconds = 0.0;
		uint32 ProcessId = 0;
		int32 ReturnCode = INDEX_NONE;
		FProcHandle ProcessHandle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FString Executable;
		FString Arguments;
		FString WorkingDirectory;
		FString EditorLogPath;
		FString ReportDirectory;
		FString Output;
		int64 LogBaseCursor = 0;
		int64 LogTotalChars = 0;
		FString PostProcess;
		FString HeadlessProfile;
		bool bEditorProcess = false;
		bool bAutomationProcess = false;
		bool bTerminationRequested = false;
		TMap<FString, double> PhaseDurationsSeconds;
		TArray<FString> PhaseHistory;
		TSharedPtr<FJsonObject> Input;
		TSharedPtr<FJsonObject> Result;
		TArray<FArtifact> Artifacts;
		int64 SynchronousArtifactHashBytes = 0;
		TSharedPtr<const TraceServices::IAnalysisSession> TraceAnalysisSession;

		// In-Editor performance sampler state.
		FString PerformanceMode = TEXT("window");
		int32 RepeatCount = 1;
		int32 RepeatIndex = 0;
		double WarmupSeconds = 0.0;
		double SampleSeconds = 0.0;
		double WarmupUntilSeconds = 0.0;
		double SamplingUntilSeconds = 0.0;
		double BudgetMs = 16.6667;
		TMap<FString, TArray<double>> MetricSamples;
		TMap<FString, TArray<double>> AggregateMetricSamples;
		TArray<TSharedPtr<FJsonValue>> Repetitions;
		TSharedPtr<FJsonObject> PendingIterationResult;
		FString ScenarioRunId;
		bool bScenarioMetricsWasActive = false;
		bool bScenarioMetricsObserved = false;
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
	TSharedPtr<FJob> StartProcessJob(
		const FProcessLaunchSpec& Spec,
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
	void TransitionProcessPhase(
		FJob& Job,
		const FString& NewPhase,
		double NowSeconds = 0.0);
	void UpdateProcessPhaseFromLog(
		FJob& Job,
		const FString& LogChunk);
	void TerminateProcessTree(FJob& Job);
	void TickPerformanceJob(FJob& Job);
	void TickTraceAnalysisJob(FJob& Job);
	void SamplePerformanceFrame(FJob& Job);
	void BeginWindowIteration(FJob& Job, double Now);
	void CompletePerformanceIteration(FJob& Job);
	bool StartScenarioIteration(FJob& Job, FString& OutError);
	void FinishPerformanceRun(FJob& Job);
	void FinishJob(
		FJob& Job,
		const FString& Status,
		const FString& ErrorCode = FString(),
		const FString& Message = FString());
	void PostProcessJob(FJob& Job);
	void WriteTestReports(FJob& Job);
	bool WritePerformanceReport(FJob& Job);
	bool WriteTraceAnalysisReport(FJob& Job);

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
	FScenarioOperation StartScenario;
	FScenarioOperation GetScenarioStatus;
	FScenarioOperation GetScenarioResult;
	FScenarioOperation CancelScenario;
};
}
