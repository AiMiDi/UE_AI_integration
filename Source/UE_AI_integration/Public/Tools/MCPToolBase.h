// Adapted from UnrealClaude's polymorphic tool pattern + BlueprintMCP's queued model
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Result from executing a tool.
 */
struct FMCPToolResult
{
	bool bSuccess = true;
	TSharedPtr<FJsonObject> Data;
	FString ErrorMessage;
	FString ErrorCode;
	int32 HttpStatus = 200;

	static FMCPToolResult Ok(TSharedPtr<FJsonObject> InData)
	{
		FMCPToolResult R;
		R.bSuccess = true;
		R.Data = InData;
		R.HttpStatus = 200;
		return R;
	}

	static FMCPToolResult Error(
		const FString& Msg,
		const FString& Code = TEXT("execution_failed"),
		int32 InHttpStatus = 500)
	{
		FMCPToolResult R;
		R.bSuccess = false;
		R.ErrorMessage = Msg;
		R.ErrorCode = Code;
		R.HttpStatus = InHttpStatus;
		return R;
	}
};

/**
 * Completion for the small set of tools that must span real Editor frames.
 * Implementations must invoke it on the Game Thread exactly once after a
 * successful BeginExecuteAsync call, including cancellation and failure.
 */
using FMCPToolAsyncCompletion = TFunction<void(FMCPToolResult&&)>;

/**
 * Base class for all MCP tools. Each tool category registers concrete subclasses.
 * Pattern: BlueprintMCP's handler-per-file + UnrealClaude's registry dispatch.
 */
class FMCPToolBase
{
public:
	virtual ~FMCPToolBase() = default;

	/** Return only the implementation binding ID; all public metadata lives in the manifest. */
	virtual FString GetCapabilityId() const = 0;

	/** Execute the tool on the game thread. Params come from the MCP client. */
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) = 0;

	/** True only for handlers whose result intentionally spans Editor frames. */
	virtual bool SupportsAsyncExecution() const { return false; }

	/**
	 * Start a bounded asynchronous execution. Returning true transfers exactly
	 * one completion obligation to the tool. Synchronous tools keep the default.
	 */
	virtual bool BeginExecuteAsync(
		const TSharedPtr<FJsonObject>& Params,
		FMCPToolAsyncCompletion Completion)
	{
		return false;
	}

	/** Cancel any active async execution and complete it before returning. */
	virtual void CancelAsyncExecution(const FString& Reason) {}
};
