#pragma once

#include "CoreMinimal.h"
#include "Core/MCPExecutionTypes.h"
#include "Dom/JsonObject.h"
#include "UObject/StrongObjectPtr.h"
#include "UEWorkflowCore/WorkflowCore.h"

class FMCPToolRegistry;

namespace UEAIIntegration::Workflow
{
struct FWorkflowObjectMemorySnapshot
{
	explicit FWorkflowObjectMemorySnapshot(UObject* InObject)
		: Object(InObject)
	{
	}

	TStrongObjectPtr<UObject> Object;
	TArray<uint8> Bytes;
	int32 OuterDepth = 0;
};

/**
 * Game-thread runtime for the versioned UE Workflow HTTP endpoint.
 *
 * Validation and planning delegate to the portable UEWorkflowCore. Execution is
 * intentionally synchronous on the editor tick so UObject mutations never run on
 * an HTTP worker thread.
 */
class FWorkflowRuntime
{
public:
	explicit FWorkflowRuntime(FMCPToolRegistry& InRegistry);
	~FWorkflowRuntime();

	FMCPResult MakeHandshake() const;
	FMCPResult HandleRequest(const TSharedPtr<FJsonObject>& Request);

	const FString& GetServerInstanceId() const { return ServerInstanceId; }
	/** Stable SHA-256 of the asset structure used by mutation receipts. */
	static FString ComputeAssetStructureHash(UObject* Asset);

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Inject a deterministic fault immediately after the requested number of
	 * authored v2 operations have completed and their checkpoint is durable.
	 *
	 * When bSimulateProcessInterruption is true, execution returns without
	 * rollback and leaves the journal in a resumable running state. This hook is
	 * compiled only for Automation builds and is never part of the HTTP/MCP
	 * contract.
	 */
	void SetTestFailAfterOperation(
		int32 CompletedOperationCount,
		bool bSimulateProcessInterruption);

	/** Fail saveOnSuccess when the named v2 scope reaches the save phase. */
	void SetTestSaveFailureScope(const FString& ScopeId);

	/**
	 * Restore the staged package baseline and discard this runtime's in-memory
	 * run state, matching the asset state seen after an Editor process restart.
	 * A newly-created FWorkflowRuntime can then exercise durable resume.
	 */
	bool SimulateEditorRestartForTest(
		const FString& RunId,
		FString& OutError);
#endif

private:
	enum class EDetailLevel : uint8
	{
		Summary,
		Standard,
		Full,
	};

	struct FResponseOptions
	{
		EDetailLevel DetailLevel = EDetailLevel::Summary;
		TSet<FString> Sections;
		bool bUsedDeprecatedDetails = false;
	};

	struct FPreparedPlanCacheEntry
	{
		TSharedPtr<FJsonObject> Plan;
		double PreparedAtSeconds = 0.0;
	};

	struct FRunRecord
	{
		FString RunId;
		FString RequestId;
		FString RequestPayloadDigest;
		FString ServerInstanceId;
		FString WorkflowId;
		FString DslVersion = TEXT("1.0");
		FString PluginVersion;
		FString ScopeAsset;
		FString PlanDigest;
		FString CorePlanDigest;
		FString ContractSetDigest;
		FString TransactionId;
		FString Status;
		FString RollbackStatus;
		FString StructureHashBefore;
		FString StructureHashAfter;
		FString StructureHashAfterRollback;
		FString CurrentPhase;
		FString Durability = TEXT("session");
		FString RecoveryState = TEXT("none");
		FString CheckpointId;
		FString LastCompletedOperationId;
		FString SnapshotDigest;
		int32 NextInitializerIndex = 0;
		int32 NextOperationIndex = 0;
		int32 NextFinalizerIndex = 0;
		bool bSaveOnSuccess = false;
		bool bDurableResume = false;
		bool bScopeExistedBefore = false;
		bool bPackageDirtyBefore = false;
		bool bMemorySnapshotCaptured = false;
		bool bMemorySnapshotRestoreAttempted = false;
		bool bMemorySnapshotRestored = false;
		bool bRollbackAvailable = false;
		bool bRollbackVerified = false;
		TArray<TSharedPtr<FJsonValue>> Operations;
		TArray<TSharedPtr<FJsonValue>> Finalizers;
		TArray<TSharedPtr<FJsonValue>> DirtyPackages;
		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		TArray<TSharedPtr<FJsonValue>> RecoveryWarnings;
		TArray<TSharedPtr<FJsonValue>> Assets;
		TSharedPtr<FJsonObject> NormalizedWorkflow;
		TSharedPtr<FJsonObject> StoredPlan;
		TSharedPtr<FJsonObject> OperationOutputs;
		TSharedPtr<FJsonObject> ReadBack;
		TSharedPtr<FJsonObject> AssetDiff;
		TSharedPtr<FJsonObject> StructureBefore;
		TSharedPtr<FJsonObject> StructureAfter;
		TSharedPtr<FJsonObject> StructureAfterRollback;

		TSharedPtr<FJsonObject> ToJournalJson() const;
		TSharedPtr<FJsonObject> ToReceiptJson() const;
		TSharedPtr<FJsonObject> ToResultJson(const FResponseOptions& Options) const;
		static bool FromJson(const TSharedPtr<FJsonObject>& Json, FRunRecord& OutRecord);
	};

	FMCPResult ValidateWorkflow(const TSharedPtr<FJsonObject>& Workflow,
	                            const FResponseOptions& Options) const;
	FMCPResult PlanWorkflow(const TSharedPtr<FJsonObject>& Workflow,
	                        const FResponseOptions& Options);
	FMCPResult ExecuteWorkflow(const TSharedPtr<FJsonObject>& Request,
	                           const FResponseOptions& Options);
	FMCPResult ExecuteWorkflowV2(
		const TSharedPtr<FJsonObject>& Request,
		const TSharedPtr<FJsonObject>& Plan,
		const FResponseOptions& Options);
	FMCPResult ContinueWorkflowV2(
		FRunRecord& Record,
		const TSharedPtr<FJsonObject>& Plan,
		const FResponseOptions& Options,
		bool bRestartFromBaseline);
	FMCPResult GetStatus(const FString& RunId, const FResponseOptions& Options);
	FMCPResult ResumeRun(const FString& RunId, const FResponseOptions& Options);
	FMCPResult ResumeRunV2(FRunRecord& Record, const FResponseOptions& Options);
	FMCPResult Rollback(const TSharedPtr<FJsonObject>& Request, const FResponseOptions& Options);
	FMCPResult RollbackV2(
		FRunRecord& Record,
		const FResponseOptions& Options,
		bool bAutomatic);

	bool TryPlan(
		const TSharedPtr<FJsonObject>& Workflow,
		TSharedPtr<FJsonObject>& OutPlan,
		FMCPResult& OutFailure) const;
	bool AdaptV1PlanToV2(
		const TSharedPtr<FJsonObject>& Plan,
		TSharedPtr<FJsonObject>& OutPlan,
		FString& OutError) const;
	bool PreparePlanWithAssetPreconditions(
		const TSharedPtr<FJsonObject>& CorePlan,
		TSharedPtr<FJsonObject>& OutPlan,
		FMCPResult& OutFailure) const;
	bool VerifyPlanAssetPreconditions(
		const TSharedPtr<FJsonObject>& PreparedPlan,
		FMCPResult& OutFailure) const;
	void CachePreparedPlan(const TSharedPtr<FJsonObject>& PreparedPlan);
	bool FindPreparedPlan(
		const FString& BoundPlanDigest,
		TSharedPtr<FJsonObject>& OutPlan);
	void PrunePreparedPlanCache();
	bool ExecuteOperation(
		const TSharedPtr<FJsonObject>& Operation,
		const TSharedPtr<FJsonObject>& Scope,
		const TMap<FString, TSharedPtr<FJsonObject>>& PriorOutputs,
		bool bSaveOnSuccess,
		bool bDeferCompile,
		TSharedPtr<FJsonObject>& OutResult,
		FMCPResult& OutFailure) const;
	bool ExecuteFinalizer(
		const TSharedPtr<FJsonObject>& Finalizer,
		const TSharedPtr<FJsonObject>& Scope,
		const TArray<TSharedPtr<FJsonValue>>& AuthoredOperations,
		const TMap<FString, TSharedPtr<FJsonObject>>& PriorOutputs,
		TSharedPtr<FJsonObject>& OutResult,
		FMCPResult& OutFailure) const;

	bool InjectScopeParams(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Scope,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutError) const;
	bool ResolveBindings(
		const TSharedPtr<FJsonObject>& Operation,
		const TMap<FString, TSharedPtr<FJsonObject>>& PriorOutputs,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutError) const;

	bool LoadRun(const FString& RunId, FRunRecord& OutRecord) const;
	bool FindRunByRequestId(
		const FString& RequestId,
		FRunRecord& OutRecord) const;
	bool SaveRun(const FRunRecord& Record, FString& OutError) const;
	FString GetRunPath(const FString& RunId) const;
	FString GetRunDirectory(const FString& RunId) const;

	static FString JsonStringify(const TSharedPtr<FJsonObject>& Json);
	static bool ParseJsonObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject);
	static TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Object);
	static TSharedPtr<FJsonObject> MakeDiagnostics(const TArray<FString>& Errors);
	static TSharedPtr<FJsonObject> MakeCoreDiagnostics(
		const ue::workflow::Result& Result);
	static TSharedPtr<FJsonObject> CaptureAssetStructure(UObject* Asset);
	static TSharedPtr<FJsonObject> MakeStructuralDiff(
		const TSharedPtr<FJsonObject>& Before,
		const TSharedPtr<FJsonObject>& After);
	static bool ParseResponseOptions(const TSharedPtr<FJsonObject>& Request, const FString& Action,
	                                 FResponseOptions& OutOptions, FMCPResult& OutFailure);
	static TSharedPtr<FJsonObject> ProjectPlanningResponse(const TSharedPtr<FJsonObject>& Response,
	                                                       const FResponseOptions& Options);
	static bool VerifyOrCleanRollback(FRunRecord& Record);
	static FString GetStringField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field);

	FMCPToolRegistry& Registry;
	ue::workflow::Engine CoreEngine;
	ue::workflow::Result CoreLoadResult;
	FString ServerInstanceId;
	FString JournalDirectory;
	FString ContractSetDigest;
	FString ContractSetDigestV2;
	TMap<FString, FRunRecord> Runs;
	TMap<FString, TArray<FWorkflowObjectMemorySnapshot>>
		RollbackMemorySnapshots;
	TMap<FString, TArray<FWorkflowObjectMemorySnapshot>>
		MultiAssetRollbackMemorySnapshots;
	TMap<FString, FPreparedPlanCacheEntry> PreparedPlanCache;
#if WITH_DEV_AUTOMATION_TESTS
	int32 TestFailAfterOperationCount = INDEX_NONE;
	bool bTestSimulateProcessInterruption = false;
	FString TestSaveFailureScopeId;
	FString TestProcessFaultPoint;
	FString TestProcessFaultMarkerPath;
	bool bTestProcessFaultTriggered = false;
	bool TriggerProcessFaultForTest(
		const FString& Point,
		const FRunRecord& Record);
#endif
};
}
