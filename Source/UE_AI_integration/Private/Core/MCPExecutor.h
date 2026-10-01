#pragma once

#include "CoreMinimal.h"
#include "Core/MCPExecutionTypes.h"

class FMCPToolRegistry;
struct FMCPToolResult;

/** Core execution boundary between HTTP transport and domain handlers. */
class FMCPExecutor
{
public:
	explicit FMCPExecutor(FMCPToolRegistry& InRegistry);

	FMCPResult Execute(const FMCPExecutionContext& Context);

	/**
	 * Begin a handler-owned async operation. Returns true when completion is
	 * owned by the handler; otherwise OutImmediate contains the final result.
	 */
	bool BeginExecuteAsync(
		const FMCPExecutionContext& Context,
		TFunction<void(FMCPResult&&)> Completion,
		FMCPResult& OutImmediate);

	/** Cancel active async tools synchronously before transport destruction. */
	void CancelAsyncOperations(const FString& Reason);

	/** Cancel one in-flight request at the capability's next safe boundary. */
	FMCPResult CancelAsyncOperation(
		const FString& RequestId,
		const FString& Reason);

	/**
	 * Attach the transport-wide capability verification state to a result
	 * produced by a transport-side capability path.  The normal execute path
	 * calls this internally; HTTP debug/workflow bypasses use the same helper
	 * so their result contract cannot drift.
	 */
	void AnnotateVerification(
		const FMCPExecutionContext& Context,
		FMCPResult& Result,
		bool bExecuted) const;

private:
	struct FIdempotencyRecord
	{
		FString PayloadKey;
		FMCPResult Result;
		FDateTime LastAccessUtc;
	};

	struct FInFlightRecord
	{
		FString PayloadKey;
		FString Capability;
	};

	FMCPResult ExecuteUncached(const FMCPExecutionContext& Context) const;
	bool CheckProtectedLease(
		const FMCPExecutionContext& Context,
		FMCPResult& OutFailure) const;
	bool PrepareExecution(
		const FMCPExecutionContext& Context,
		TSharedPtr<FJsonObject>& OutEffectiveParams,
		FMCPResult& OutFailure) const;
	static FMCPResult ConvertToolResult(FMCPToolResult&& ToolResult);
	void StoreIdempotencyResult(
		const FString& RequestId,
		const FString& PayloadKey,
		const FMCPResult& Result,
		const FDateTime& NowUtc);
	static FString MakePayloadKey(const FMCPExecutionContext& Context);
	static TSharedPtr<FJsonObject> MakeValidationDetails(const TArray<FString>& Errors);
	void PruneIdempotencyCache(const FDateTime& NowUtc);

	FMCPToolRegistry& Registry;
	TMap<FString, FIdempotencyRecord> IdempotencyCache;
	TMap<FString, FInFlightRecord> InFlightRequests;
};
