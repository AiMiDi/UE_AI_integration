#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

namespace UEAIIntegration::Infrastructure
{
/**
 * Versioned orchestration over the existing performance run/compare/diagnose
 * and report contracts. The service never samples frames itself.
 */
class FPerformanceSuiteService
{
public:
	using FOperation = TFunction<FMCPToolResult(
		const FString&,
		const TSharedPtr<FJsonObject>&)>;

	explicit FPerformanceSuiteService(FOperation InOperation);
	~FPerformanceSuiteService();

	void Tick();
	bool Handles(const FString& CapabilityId) const;
	FMCPToolResult Execute(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params);

	/** Pure definition validation used by Automation without starting PIE. */
	static bool ValidateDefinition(
		const TSharedPtr<FJsonObject>& Definition,
		TArray<FString>& OutErrors);

private:
	struct FScenarioRun
	{
		FString Id;
		FString Status = TEXT("pending");
		FString Verdict = TEXT("pending");
		FString RunId;
		FString ComparisonId;
		FString ReportJobId;
		FString ErrorCode;
		FString ErrorMessage;
		TSharedPtr<FJsonObject> Definition;
		TSharedPtr<FJsonObject> Result;
	};

	struct FSuiteRun
	{
		FString Id;
		FString SuiteId;
		FString Status = TEXT("running");
		FString Phase = TEXT("starting");
		FString CreatedAtUtc;
		FString FinishedAtUtc;
		int32 CurrentIndex = 0;
		TArray<FScenarioRun> Scenarios;
		TMap<FString, FString> OriginalCVars;
		FString OriginalMapPackage;
		FString EnvironmentRestoreError;
		bool bCVarsApplied = false;
		bool bMapChanged = false;
		bool bMapRestored = true;
	};

	FMCPToolResult ListSuites() const;
	FMCPToolResult ValidateSuites(
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StartSuite(
		const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetSuiteResult(
		const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult PromoteBaseline(
		const TSharedPtr<FJsonObject>& Params);

	void TickSuite(FSuiteRun& Run);
	void BeginScenario(FSuiteRun& Run, FScenarioRun& Scenario);
	void PollScenario(FSuiteRun& Run, FScenarioRun& Scenario);
	void FinishSuite(FSuiteRun& Run, const FString& Status);
	void ApplyMeasurementCVars(FSuiteRun& Run);
	bool ApplyScenarioEnvironment(
		FSuiteRun& Run,
		const FScenarioRun& Scenario,
		FString& OutError);
	bool RestoreOriginalMap(FSuiteRun& Run, FString& OutError);
	void RestoreMeasurementCVars(FSuiteRun& Run);
	void PersistSuiteRun(const FSuiteRun& Run) const;

	TArray<TSharedPtr<FJsonObject>> LoadDefinitions(
		TArray<FString>& OutErrors) const;
	TSharedPtr<FJsonObject> FindDefinition(
		const FString& SuiteId,
		const FString& ScenarioId,
		TArray<FString>& OutErrors) const;
	TSharedPtr<FJsonObject> LoadBaseline(
		const FString& SuiteId,
		const FString& ScenarioId) const;
	static TSharedPtr<FJsonObject> MakeRunSummary(
		const FSuiteRun& Run,
		bool bIncludeResults);
	static FString ExtractStatus(const TSharedPtr<FJsonObject>& Data);
	static FString StringField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& Default = FString());
	static FString NewId(const TCHAR* Prefix);
	static FString BaselinePath(
		const FString& SuiteId,
		const FString& ScenarioId);
	static FString SuiteResultPath(const FString& SuiteRunId);

	FOperation Operation;
	TMap<FString, FSuiteRun> Runs;
	FString ActiveRunId;
};
}
