// HTTP transport for the UE_AI_integration capability registry.
#pragma once

#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "Templates/Function.h"
#include "Core/MCPExecutionTypes.h"

class FMCPToolRegistry;
class FMCPExecutor;
class FJsonObject;
namespace UEAIIntegration::Workflow
{
class FWorkflowRuntime;
}
namespace UEAIIntegration::Infrastructure
{
class FBlueprintDebugService;
class FClientActivityService;
struct FCallerContext;
}

enum class EUEAIIntegrationRequestKind : uint8
{
	LegacyExecute,
	CancelExecute,
	WorkflowHandshake,
	WorkflowAction,
};

struct FPendingMCPExecuteRequest
{
	EUEAIIntegrationRequestKind Kind = EUEAIIntegrationRequestKind::LegacyExecute;
	FString Body;
	FString ActivityId;
	TSharedPtr<UEAIIntegration::Infrastructure::FCallerContext> Caller;
	FHttpResultCallback OnComplete;
};

/**
 * Bridges the TypeScript MCP wrapper to an instance-owned capability registry.
 * Discovery is immediate and immutable after startup; execution is queued to the Game Thread.
 */
class FUEAIIntegrationServer
{
public:
	FUEAIIntegrationServer(
		FMCPToolRegistry& InRegistry,
		FMCPExecutor& InExecutor,
		UEAIIntegration::Infrastructure::FClientActivityService&
			InClientActivityService,
		UEAIIntegration::Infrastructure::FBlueprintDebugService*
			InBlueprintDebugService = nullptr);
	~FUEAIIntegrationServer();

	/** Bind the legacy and versioned workflow routes and begin listening. */
	bool Start(int32 Port = 9847);

	/** Unbind only routes owned by this server. */
	void Stop();

	/** Process one queued request per editor tick on the Game Thread. */
	void Tick(float DeltaTime);

	bool IsRunning() const { return bIsRunning; }
	int32 GetPort() const { return ListenPort; }

	/** Plan an internally generated Workflow through the same Editor planner. */
	FMCPResult PlanWorkflowDefinition(
		const TSharedPtr<FJsonObject>& Workflow) const;

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * Observe the exact boundary where a queued workflow HTTP request is
	 * dispatched to FWorkflowRuntime. Automation-only; absent from production.
	 */
	void SetWorkflowDispatchObserverForTesting(
		TFunction<void()> InObserver)
	{
		WorkflowDispatchObserverForTesting = MoveTemp(InObserver);
	}

	UEAIIntegration::Workflow::FWorkflowRuntime*
	GetWorkflowRuntimeForTesting() const
	{
		return WorkflowRuntime.Get();
	}
#endif

private:
	bool HandleHealth(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleCapabilities(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleExecute(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleExecuteCancel(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleWorkflowHandshake(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleWorkflow(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleClientRegister(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleClientHeartbeat(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleClientUnregister(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	bool HandleCallerObserved(
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete,
		TFunctionRef<bool(const FHttpResultCallback&)> Handler);

	void ProcessOneRequest();
	bool QueueRequest(
		EUEAIIntegrationRequestKind Kind,
		const FHttpServerRequest& Request,
		const FHttpResultCallback& OnComplete);
	void UnbindRoutes();
	void WriteInstanceRecord();
	void RemoveInstanceRecord();
	FString InstanceRecordPath() const;

	static void SendJsonResponse(
		const FHttpResultCallback& OnComplete,
		int32 StatusCode,
		const TSharedPtr<FJsonObject>& Object);
	static void SendSuccess(
		const FHttpResultCallback& OnComplete,
		const TSharedPtr<FJsonObject>& Data,
		int32 StatusCode = 200);
	static void SendError(
		const FHttpResultCallback& OnComplete,
		int32 StatusCode,
		const FString& Code,
		const FString& Message,
		const TSharedPtr<FJsonObject>& Details = nullptr);
	static TSharedPtr<FJsonObject> MakeValidationDetails(const TArray<FString>& Errors);

	FMCPToolRegistry& Registry;
	FMCPExecutor& Executor;
	UEAIIntegration::Infrastructure::FClientActivityService&
		ClientActivityService;
	UEAIIntegration::Infrastructure::FBlueprintDebugService*
		BlueprintDebugService = nullptr;
	TUniquePtr<UEAIIntegration::Workflow::FWorkflowRuntime> WorkflowRuntime;
	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	TQueue<TSharedPtr<FPendingMCPExecuteRequest>, EQueueMode::Mpsc> RequestQueue;
	int32 ListenPort = 9847;
	bool bIsRunning = false;
	FString ServerInstanceId;
	FString ProcessStartTimeUtc;
#if WITH_DEV_AUTOMATION_TESTS
	TFunction<void()> WorkflowDispatchObserverForTesting;
#endif
};
