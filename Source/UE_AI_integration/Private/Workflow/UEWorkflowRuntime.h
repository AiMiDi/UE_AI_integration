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

	FMCPResult MakeHandshake() const;
	FMCPResult HandleRequest(const TSharedPtr<FJsonObject>& Request);

	const FString& GetServerInstanceId() const { return ServerInstanceId; }
	/** Stable SHA-256 of the asset structure used by mutation receipts. */
	static FString ComputeAssetStructureHash(UObject* Asset);

private:
	struct FRunRecord
	{
		FString RunId;
		FString ServerInstanceId;
		FString WorkflowId;
		FString ScopeAsset;
		FString PlanDigest;
		FString ContractSetDigest;
		FString TransactionId;
		FString Status;
		FString RollbackStatus;
		FString StructureHashBefore;
		FString StructureHashAfter;
		FString StructureHashAfterRollback;
		bool bSaveOnSuccess = false;
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
		TSharedPtr<FJsonObject> ReadBack;
		TSharedPtr<FJsonObject> AssetDiff;
		TSharedPtr<FJsonObject> StructureBefore;
		TSharedPtr<FJsonObject> StructureAfter;
		TSharedPtr<FJsonObject> StructureAfterRollback;

		TSharedPtr<FJsonObject> ToJson() const;
		TSharedPtr<FJsonObject> ToResultJson() const;
		static bool FromJson(const TSharedPtr<FJsonObject>& Json, FRunRecord& OutRecord);
	};

	FMCPResult ValidateWorkflow(const TSharedPtr<FJsonObject>& Workflow) const;
	FMCPResult PlanWorkflow(const TSharedPtr<FJsonObject>& Workflow) const;
	FMCPResult ExecuteWorkflow(const TSharedPtr<FJsonObject>& Request);
	FMCPResult GetStatus(const FString& RunId);
	FMCPResult ResumeRun(const FString& RunId);
	FMCPResult Rollback(const TSharedPtr<FJsonObject>& Request);

	bool TryPlan(
		const TSharedPtr<FJsonObject>& Workflow,
		TSharedPtr<FJsonObject>& OutPlan,
		FMCPResult& OutFailure) const;
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
	bool SaveRun(const FRunRecord& Record, FString& OutError) const;
	FString GetRunPath(const FString& RunId) const;

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
	TMap<FString, FRunRecord> Runs;
	TMap<FString, TArray<FWorkflowObjectMemorySnapshot>>
		RollbackMemorySnapshots;
};
}
