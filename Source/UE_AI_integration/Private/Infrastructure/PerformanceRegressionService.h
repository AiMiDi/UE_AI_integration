#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

namespace UEAIIntegration::Infrastructure
{
/**
 * Adds deterministic performance profiles, environment fingerprints and
 * regression evidence around the durable production job runtime.
 *
 * The service deliberately composes the existing public job operations instead
 * of reaching into FProductionJobRuntime state. This keeps performance
 * regression policy separate from process/job lifecycle management.
 */
class FPerformanceRegressionService
{
public:
	using FOperation = TFunction<FMCPToolResult(
		const FString&,
		const TSharedPtr<FJsonObject>&)>;

	explicit FPerformanceRegressionService(FOperation InOperation);

	void Tick();

	FMCPToolResult Execute(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params);

	/** Pure helpers exposed for Automation contract tests. */
	static bool NormalizeRunRequest(
		const TSharedPtr<FJsonObject>& Request,
		const FString& CurrentMap,
		TSharedPtr<FJsonObject>& OutRequest,
		TSharedPtr<FJsonObject>& OutProfile,
		FString& OutError);
	static TSharedPtr<FJsonObject> BuildRegressionSummary(
		const TSharedPtr<FJsonObject>& Comparison);
	static FString BuildJUnitReport(
		const FString& ComparisonId,
		const TSharedPtr<FJsonObject>& Comparison);

private:
	struct FArtifact
	{
		FString Id;
		FString Name;
		FString Path;
		FString MimeType;
		FString Sha256;
		int64 Size = 0;
		FDateTime ModifiedAtUtc;
	};

	struct FRegressionJob
	{
		FString Id;
		FString Status = TEXT("succeeded");
		FString Phase = TEXT("complete");
		FString CreatedAtUtc;
		FString DiagnosticPerformanceRunId;
		FString TraceId;
		FString TraceAnalysisJobId;
		TSharedPtr<FJsonObject> Comparison;
		TArray<FArtifact> Artifacts;
	};

	FMCPToolResult StartPerformanceRun(
		const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetPerformanceResult(
		const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult ComparePerformanceRuns(
		const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetRegressionJob(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetRegressionArtifact(
		const TSharedPtr<FJsonObject>& Params) const;

	void TickRegressionJob(FRegressionJob& Job);
	void FinishDiagnostic(
		FRegressionJob& Job,
		const FString& Status,
		const FString& Code = FString(),
		const FString& Message = FString());
	void ApplyFingerprintCompatibility(
		const FString& BaselineRunId,
		const FString& CandidateRunId,
		TSharedPtr<FJsonObject>& Comparison) const;
	void WriteRegressionArtifacts(FRegressionJob& Job);
	void PersistFingerprint(
		const FString& RunId,
		const TSharedPtr<FJsonObject>& Fingerprint) const;
	TSharedPtr<FJsonObject> LoadFingerprint(const FString& RunId) const;

	static TSharedPtr<FJsonObject> CaptureFingerprint(
		const TSharedPtr<FJsonObject>& Profile);
	static TSharedPtr<FJsonObject> MakeArtifactSummary(
		const FArtifact& Artifact);
	static TSharedPtr<FJsonObject> MakeRegressionJobSummary(
		const FRegressionJob& Job,
		bool bIncludeResult);
	static FString CurrentMapPackage();
	static FString FingerprintDirectory(const FString& RunId);
	static FString RegressionDirectory(const FString& ComparisonId);
	static FString NewId(const TCHAR* Prefix);

	FOperation Operation;
	TMap<FString, TSharedPtr<FJsonObject>> Fingerprints;
	TMap<FString, FRegressionJob> RegressionJobs;
};
}
