#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Tools/MCPToolBase.h"

class FMCPToolRegistry;

namespace UEAIIntegration::Infrastructure
{
class FPIESessionController;

/**
 * Owns the asynchronous production jobs exposed by the production domain.
 *
 * The controller is subsystem-owned so scenario and build state survive individual
 * HTTP requests while still being destroyed with the Editor subsystem.
 */
class FProductionRuntimeController
{
public:
	FProductionRuntimeController(
		FMCPToolRegistry& InRegistry,
		FPIESessionController& InPIEController);
	~FProductionRuntimeController();

	void Tick(float DeltaTime);

	FMCPToolResult ValidateScenario(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StartScenario(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetScenarioStatus(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult CancelScenario(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetScenarioResult(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetScenarioArtifact(const TSharedPtr<FJsonObject>& Params) const;

	FMCPToolResult GetLoadedModule(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult StartBuild(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult GetBuildJob(const TSharedPtr<FJsonObject>& Params) const;

private:
	struct FScenarioArtifact
	{
		FString Id;
		FString Path;
		FString MimeType;
		TSharedPtr<FJsonObject> Metadata;
	};

	struct FScenarioRun
	{
		FString Id;
		FString Name;
		FString Status;
		FString ErrorCode;
		FString ErrorMessage;
		TSharedPtr<FJsonObject> Scenario;
		TArray<TSharedPtr<FJsonValue>> StepReceipts;
		TMap<FString, TSharedPtr<FJsonObject>> StepResults;
		TMap<FString, FScenarioArtifact> Artifacts;
		int32 StepIndex = 0;
		double StartedAtSeconds = 0.0;
		double DeadlineSeconds = 0.0;
		double StepStartedAtSeconds = 0.0;
		double WaitUntilSeconds = 0.0;
		bool bCancelRequested = false;
		bool bCleanupStopPIE = false;
	};

	struct FBuildJob
	{
		FString Id;
		FString Mode;
		FString Status;
		FString Message;
		FString Executable;
		FString Arguments;
		FString StartedAtUtc;
		FString CompletedAtUtc;
		uint32 ProcessId = 0;
		int32 ReturnCode = INDEX_NONE;
		FProcHandle ProcessHandle;
		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FString Output;
		FString LogPath;
		TMap<FString, FDateTime> ExistingDllTimestamps;
		FString LoadedPdbPath;
		FString LoadedPdbBackupPath;
		TSharedPtr<FJsonObject> BuiltDll;
		TSharedPtr<FJsonObject> BuiltPdb;
		bool bLiveCodingPatchCompleted = false;
	};

	FMCPToolResult ValidateScenarioObject(
		const TSharedPtr<FJsonObject>& Scenario,
		TArray<FString>& OutErrors) const;
	void TickScenario();
	void TickBuild();
	void FinishScenario(
		FScenarioRun& Run,
		const FString& Status,
		const FString& ErrorCode = FString(),
		const FString& ErrorMessage = FString());
	bool ExecuteScenarioStep(
		FScenarioRun& Run,
		const TSharedPtr<FJsonObject>& Step,
		FString& OutErrorCode,
		FString& OutErrorMessage,
		bool& bOutShouldRetry);
	bool EvaluateAssertions(
		const TSharedPtr<FJsonObject>& Step,
		const TSharedPtr<FJsonObject>& StepResult,
		FString& OutErrorMessage) const;
	void CaptureArtifact(
		FScenarioRun& Run,
		const FString& StepId,
		const TSharedPtr<FJsonObject>& StepResult);
	void WriteScenarioReceipt(const FScenarioRun& Run) const;
	TSharedPtr<FJsonObject> MakeScenarioSummary(const FScenarioRun& Run) const;
	TSharedPtr<FJsonObject> MakeBuildSummary(const FBuildJob& Job) const;

	static FString ScenarioDirectory();
	static FString MapScenarioActionToCapability(
		const FString& Action,
		TSharedPtr<FJsonObject>& InOutParams);
	static FString ComputeFileSha256(const FString& Path);
	static TSharedPtr<FJsonObject> MakeFileProvenance(const FString& Path);
	static FString GetEditorStartTimeUtc();

	FMCPToolRegistry& Registry;
	FPIESessionController& PIEController;
	TMap<FString, TSharedPtr<FScenarioRun>> ScenarioRuns;
	FString ActiveScenarioId;
	TMap<FString, TSharedPtr<FBuildJob>> BuildJobs;
	FString ActiveBuildJobId;
	FString LastLiveCodingPatchResult = TEXT("none");
	FDelegateHandle LiveCodingPatchHandle;
	FDateTime InitializedAtUtc;
};
}
