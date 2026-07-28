#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAIIntegration::Infrastructure
{
class FPIESessionController;

struct FBlueprintDebugResult
{
	bool bSuccess = true;
	TSharedPtr<FJsonObject> Data;
	FString ErrorCode;
	FString ErrorMessage;
	int32 HttpStatus = 200;

	static FBlueprintDebugResult Ok(const TSharedPtr<FJsonObject>& InData);
	static FBlueprintDebugResult Error(
		const FString& InCode,
		const FString& InMessage,
		int32 InHttpStatus);
};

/**
 * Thread-safe facade over UE's Blueprint debugger.
 *
 * UObject and FKismetDebugUtilities access is confined to the Game Thread.
 * HTTP callbacks that run while Kismet has the Game Thread paused can only
 * enqueue value-only commands or read immutable snapshots copied under a lock.
 * Commands are consumed from Slate pre-tick, which continues to run inside
 * Unreal's intra-frame Blueprint debugging loop.
 */
class FBlueprintDebugService
{
public:
	explicit FBlueprintDebugService(FPIESessionController& InPIEController);
	~FBlueprintDebugService();

	/** Consume commands and publish a fresh immutable snapshot on the Game Thread. */
	void Tick();

	FBlueprintDebugResult GetSession(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult GetTrace(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult ListBreakpoints(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult SetBreakpoint(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult RemoveBreakpoint(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult ListWatches(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult SetWatch(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult RemoveWatch(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult GetWatchValue(const TSharedPtr<FJsonObject>& Params);
	FBlueprintDebugResult Control(
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestId = FString());

	/**
	 * Handle one of the ten Blueprint debug capabilities while the normal
	 * subsystem tick is blocked by an intra-frame breakpoint.
	 */
	bool TryHandlePausedRequest(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestId,
		FBlueprintDebugResult& OutResult);

	/** True when only the paused-debug HTTP fast path can make progress. */
	bool IsPausedTransportActive() const;

	/**
	 * Resolve real Kismet trace evidence for finding correlation. Cursor bounds
	 * are inclusive; empty bounds select the entire retained debug session.
	 */
	FBlueprintDebugResult CollectObservedNodeIds(
		const FString& RequestedDebugSessionId,
		const FString& CursorStart,
		const FString& CursorEnd,
		TSet<FString>& OutNodeIds) const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Value-only test seam for queue/session contract tests. */
	void SetSessionForTesting(
		const FString& InSessionId,
		uint64 InGeneration,
		const FString& InDebugSessionId,
		bool bInActive,
		bool bInPaused);
	void AddTraceForTesting(uint64 Cursor, const FString& NodeId);
	int32 GetTraceDedupeKeyCountForTesting() const;
	int32 GetBlueprintScanCountForTesting() const;
	void GetPublishedSessionForTesting(
		FString& OutSessionId,
		uint64& OutGeneration,
		FString& OutDebugSessionId,
		bool& bOutActive,
		bool& bOutPaused,
		FString& OutCurrentNodeGuid) const;
#endif

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
}
