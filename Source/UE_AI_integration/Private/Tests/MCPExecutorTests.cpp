#if WITH_DEV_AUTOMATION_TESTS

#include "Core/MCPExecutor.h"
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
class FExecutorTestTool final : public FMCPToolBase
{
public:
	FExecutorTestTool(FString InId, bool bInFails)
		: Id(MoveTemp(InId))
		, bFails(bInFails)
	{
	}

	virtual FString GetCapabilityId() const override
	{
		return Id;
	}

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		++ExecuteCount;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("requestId"), LastRequestId);
		}
		if (bFails)
		{
			return FMCPToolResult::Error(TEXT("Expected test failure."));
		}
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}

	int32 GetExecuteCount() const { return ExecuteCount; }
	const FString& GetLastRequestId() const { return LastRequestId; }

private:
	FString Id;
	bool bFails = false;
	int32 ExecuteCount = 0;
	FString LastRequestId;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPExecutorContractTest,
	"UE_AI_integration.Core.ExecutorErrorContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPExecutorContractTest::RunTest(const FString& Parameters)
{
	AddExpectedError(
		TEXT("Capability catalog is degraded"),
		EAutomationExpectedErrorFlags::Contains,
		2);

	FMCPToolRegistry DegradedRegistry;
	DegradedRegistry.LoadCapabilityManifests();
	FMCPExecutor DegradedExecutor(DegradedRegistry);
	const FMCPResult DegradedResult = DegradedExecutor.Execute(
		{TEXT("blueprint.asset.list"), MakeShared<FJsonObject>()});
	TestFalse(TEXT("Degraded registry prohibits execution"), DegradedResult.bOk);
	TestEqual(TEXT("Degraded status"), DegradedResult.Error.HttpStatus, 503);
	TestEqual(
		TEXT("Degraded code"),
		DegradedResult.Error.Code,
		FString(TEXT("service_degraded")));

	FMCPToolRegistry Registry;
	Registry.LoadCapabilityManifests();
	TSharedPtr<FExecutorTestTool> IdempotentTool;
	TSharedPtr<FExecutorTestTool> ContextRequestIdTool;
	for (const TSharedPtr<FJsonObject>& Descriptor : Registry.GetCapabilityDescriptors())
	{
		const FString Id = Descriptor->GetStringField(TEXT("id"));
		Registry.BeginDomainRegistration(Descriptor->GetStringField(TEXT("domain")));
		TSharedPtr<FExecutorTestTool> Tool =
			MakeShared<FExecutorTestTool>(
				Id,
				Id == TEXT("blueprint.asset.list"));
		if (Id == TEXT("production.build.status"))
		{
			IdempotentTool = Tool;
		}
		if (Id == TEXT("content.asset.change.rollback"))
		{
			ContextRequestIdTool = Tool;
		}
		Registry.Register(Tool);
		Registry.EndDomainRegistration();
	}
	TestTrue(TEXT("Executor fixture has exact bindings"), Registry.LoadCapabilityManifests());

	FMCPExecutor Executor(Registry);
	const FMCPResult Unknown = Executor.Execute(
		{TEXT("blueprint.unknown.operation"), MakeShared<FJsonObject>()});
	TestFalse(TEXT("Unknown capability fails"), Unknown.bOk);
	TestEqual(TEXT("Unknown capability status"), Unknown.Error.HttpStatus, 404);
	TestEqual(
		TEXT("Unknown capability code"),
		Unknown.Error.Code,
		FString(TEXT("capability_not_found")));

	const FMCPResult InvalidParams = Executor.Execute(
		{TEXT("blueprint.asset.create"), MakeShared<FJsonObject>()});
	TestFalse(TEXT("Invalid params fail"), InvalidParams.bOk);
	TestEqual(TEXT("Invalid params status"), InvalidParams.Error.HttpStatus, 422);
	TestEqual(
		TEXT("Invalid params code"),
		InvalidParams.Error.Code,
		FString(TEXT("invalid_params")));

	const FMCPResult ExecutionFailure = Executor.Execute(
		{TEXT("blueprint.asset.list"), MakeShared<FJsonObject>()});
	TestFalse(TEXT("Domain execution failure is preserved"), ExecutionFailure.bOk);
	TestEqual(TEXT("Execution failure status"), ExecutionFailure.Error.HttpStatus, 500);
	TestEqual(
		TEXT("Execution failure code"),
		ExecutionFailure.Error.Code,
		FString(TEXT("execution_failed")));

	FMCPExecutionContext FirstIdempotentRequest;
	FirstIdempotentRequest.Capability = TEXT("production.build.status");
	FirstIdempotentRequest.Params = MakeShared<FJsonObject>();
	FirstIdempotentRequest.RequestId = TEXT("executor-test-request");
	const FMCPResult FirstIdempotentResult = Executor.Execute(FirstIdempotentRequest);
	const FMCPResult ReplayedIdempotentResult = Executor.Execute(FirstIdempotentRequest);
	TestTrue(TEXT("Initial idempotent request succeeds"), FirstIdempotentResult.bOk);
	TestTrue(TEXT("Identical idempotent request replays"), ReplayedIdempotentResult.bOk);
	TestTrue(TEXT("Idempotency fixture tool is registered"), IdempotentTool.IsValid());
	if (IdempotentTool.IsValid())
	{
		TestEqual(
			TEXT("Replayed request does not execute the handler twice"),
			IdempotentTool->GetExecuteCount(),
			1);
	}

	FMCPExecutionContext ConflictingRequest = FirstIdempotentRequest;
	ConflictingRequest.Params = MakeShared<FJsonObject>();
	ConflictingRequest.Params->SetStringField(TEXT("different"), TEXT("payload"));
	const FMCPResult ConflictResult = Executor.Execute(ConflictingRequest);
	TestFalse(TEXT("requestId payload conflict fails"), ConflictResult.bOk);
	TestEqual(TEXT("requestId conflict status"), ConflictResult.Error.HttpStatus, 409);
	TestEqual(
		TEXT("requestId conflict code"),
		ConflictResult.Error.Code,
		FString(TEXT("idempotency_conflict")));

	TSharedPtr<FJsonObject> ChangeParams = MakeShared<FJsonObject>();
	ChangeParams->SetStringField(TEXT("receiptId"), TEXT("asset-run-fixture"));
	ChangeParams->SetBoolField(TEXT("confirmWrite"), true);
	FMCPExecutionContext MissingContextRequestId;
	MissingContextRequestId.Capability = TEXT("content.asset.change.rollback");
	MissingContextRequestId.Params = ChangeParams;
	const FMCPResult MissingRequestId =
		Executor.Execute(MissingContextRequestId);
	TestFalse(TEXT("Context requestId is required when declared"), MissingRequestId.bOk);
	TestEqual(
		TEXT("Missing context requestId code"),
		MissingRequestId.Error.Code,
		FString(TEXT("request_id_required")));

	FMCPExecutionContext InjectedContextRequestId = MissingContextRequestId;
	InjectedContextRequestId.RequestId = TEXT("asset-change-request");
	const FMCPResult InjectedRequestId =
		Executor.Execute(InjectedContextRequestId);
	TestTrue(TEXT("Context requestId is injected into capability params"), InjectedRequestId.bOk);
	TestTrue(TEXT("RequestId injection fixture tool is registered"), ContextRequestIdTool.IsValid());
	if (ContextRequestIdTool.IsValid())
	{
		TestEqual(
			TEXT("Handler receives the envelope requestId"),
			ContextRequestIdTool->GetLastRequestId(),
			InjectedContextRequestId.RequestId);
	}
	FMCPExecutionContext LegacyDuplicatedRequestId = InjectedContextRequestId;
	LegacyDuplicatedRequestId.Params =
		MakeShared<FJsonObject>(*ChangeParams);
	LegacyDuplicatedRequestId.Params->SetStringField(
		TEXT("requestId"),
		InjectedContextRequestId.RequestId);
	const FMCPResult LegacyDuplicatedReplay =
		Executor.Execute(LegacyDuplicatedRequestId);
	TestTrue(
		TEXT("Legacy matching params.requestId replays the envelope request"),
		LegacyDuplicatedReplay.bOk);
	if (ContextRequestIdTool.IsValid())
	{
		TestEqual(
			TEXT("Legacy duplicate does not execute the handler twice"),
			ContextRequestIdTool->GetExecuteCount(),
			1);
	}

	FMCPExecutionContext MismatchedContextRequestId = MissingContextRequestId;
	MismatchedContextRequestId.RequestId = TEXT("asset-change-request-mismatch");
	MismatchedContextRequestId.Params =
		MakeShared<FJsonObject>(*ChangeParams);
	MismatchedContextRequestId.Params->SetStringField(
		TEXT("requestId"),
		TEXT("different-request"));
	const FMCPResult MismatchedRequestId =
		Executor.Execute(MismatchedContextRequestId);
	TestFalse(TEXT("Conflicting params requestId is rejected"), MismatchedRequestId.bOk);
	TestEqual(
		TEXT("Conflicting params requestId code"),
		MismatchedRequestId.Error.Code,
		FString(TEXT("request_id_mismatch")));

	TSharedPtr<FJsonObject> LegacyCookParams = MakeShared<FJsonObject>();
	LegacyCookParams->SetStringField(TEXT("platform"), TEXT("Win64"));
	const FMCPResult LegacyCook = Executor.Execute(
		{TEXT("production.project.cook"), LegacyCookParams});
	TestTrue(
		TEXT("Legacy cook remains callable without requestId"),
		LegacyCook.bOk);

	TSharedPtr<FJsonObject> LegacyPackageParams = MakeShared<FJsonObject>();
	LegacyPackageParams->SetStringField(TEXT("platform"), TEXT("Win64"));
	LegacyPackageParams->SetStringField(TEXT("output_dir"), TEXT("LegacyPackage"));
	const FMCPResult LegacyPackage = Executor.Execute(
		{TEXT("production.project.package"), LegacyPackageParams});
	TestTrue(
		TEXT("Legacy package remains callable without requestId"),
		LegacyPackage.bOk);

	TSharedPtr<FJsonObject> LegacyCommandletParams = MakeShared<FJsonObject>();
	LegacyCommandletParams->SetStringField(
		TEXT("commandlet_name"),
		TEXT("ResavePackages"));
	const FMCPResult LegacyCommandlet = Executor.Execute(
		{TEXT("production.commandlet.run"), LegacyCommandletParams});
	TestTrue(
		TEXT("Allowlisted legacy commandlet remains callable without requestId"),
		LegacyCommandlet.bOk);

	return true;
}

#endif
