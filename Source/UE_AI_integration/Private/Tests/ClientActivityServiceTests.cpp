#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/ClientActivityService.h"

#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using UEAIIntegration::Infrastructure::FCallerContext;
using UEAIIntegration::Infrastructure::FClientActivityService;
using UEAIIntegration::Infrastructure::FClientRegistration;

namespace
{
FClientRegistration MakeMcpRegistration(
	const FString& Name,
	const FString& InstanceId,
	const uint32 Pid)
{
	FClientRegistration Registration;
	Registration.ClientKind = TEXT("mcp");
	Registration.Name = Name;
	Registration.Version = TEXT("1.2.3");
	Registration.Transport = TEXT("stdio");
	Registration.InstanceId = InstanceId;
	Registration.Pid = Pid;
	return Registration;
}

FClientRegistration MakeCliRegistration(
	const FString& InvocationId,
	const FString& InstanceId,
	const FString& Command)
{
	FClientRegistration Registration;
	Registration.ClientKind = TEXT("cli");
	Registration.Name = TEXT("ue-workflow");
	Registration.Version = TEXT("0.6.0");
	Registration.Transport = TEXT("terminal");
	Registration.Command = Command;
	Registration.InstanceId = InstanceId;
	Registration.InvocationId = InvocationId;
	Registration.Pid = 4321;
	return Registration;
}

TArray<uint8> SerializeEnvelope(
	const bool bOk,
	const TSharedPtr<FJsonObject>& Data = nullptr,
	const FString& ErrorCode = FString(),
	const FString& ErrorMessage = FString(),
	const TSharedPtr<FJsonObject>& ErrorDetails = nullptr)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), bOk);
	if (Data.IsValid())
	{
		Root->SetObjectField(TEXT("data"), Data);
	}
	if (!ErrorCode.IsEmpty())
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), ErrorCode);
		Error->SetStringField(TEXT("message"), ErrorMessage);
		if (ErrorDetails.IsValid())
		{
			Error->SetObjectField(TEXT("details"), ErrorDetails);
		}
		Root->SetObjectField(TEXT("error"), Error);
	}

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	const FTCHARToUTF8 Utf8(*Json);
	TArray<uint8> Body;
	Body.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()),
		Utf8.Length());
	return Body;
}

TSharedPtr<FJsonObject> MakeWorkflowData(
	const FString& RunId,
	const FString& Status,
	const int32 OperationTotal,
	const int32 OperationSucceeded)
{
	TSharedPtr<FJsonObject> Operations = MakeShared<FJsonObject>();
	Operations->SetNumberField(TEXT("total"), OperationTotal);
	Operations->SetNumberField(TEXT("succeeded"), OperationSucceeded);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetObjectField(TEXT("operations"), Operations);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("runId"), RunId);
	Data->SetStringField(TEXT("status"), Status);
	Data->SetObjectField(TEXT("summary"), Summary);
	return Data;
}

int32 JsonInt(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	double Value = 0.0;
	return Object.IsValid() && Object->TryGetNumberField(Field, Value)
		? static_cast<int32>(Value)
		: 0;
}

TSharedPtr<FJsonObject> FindObjectByStringField(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const TCHAR* Field,
	const FString& Expected)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object =
			Value.IsValid() ? Value->AsObject() : nullptr;
		FString Actual;
		if (Object.IsValid()
			&& Object->TryGetStringField(Field, Actual)
			&& Actual == Expected)
		{
			return Object;
		}
	}
	return nullptr;
}

FString SerializeObject(const TSharedPtr<FJsonObject>& Object)
{
	FString Json;
	if (Object.IsValid())
	{
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	}
	return Json;
}

void CompleteWorkflowActivity(
	FClientActivityService& Service,
	const FCallerContext& Caller,
	const FString& Action,
	const FString& RequestId,
	const TSharedPtr<FJsonObject>& Data)
{
	const FString EventId =
		Service.BeginActivity(Caller, TEXT("workflow"));
	Service.MarkActivityStarted(EventId);
	Service.UpdateWorkflowActivity(
		EventId,
		Action,
		RequestId,
		TEXT("safeWrite"));
	Service.CompleteActivityFromHttp(
		EventId,
		200,
		SerializeEnvelope(true, Data));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClientActivityRegistrationLifecycleTest,
	"UE_AI_integration.Transport.ClientActivity.RegistrationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClientActivityRegistrationLifecycleTest::RunTest(
	const FString& Parameters)
{
	FClientActivityService Service;
	FString SessionId;
	FString Error;
	const FClientRegistration Codex =
		MakeMcpRegistration(TEXT("Codex"), TEXT("codex-instance"), 1234);

	TestTrue(
		TEXT("An MCP client registers"),
		Service.RegisterClient(Codex, SessionId, Error));
	TestFalse(TEXT("Registration returns an opaque session id"), SessionId.IsEmpty());

	FString ReplayedSessionId;
	TestTrue(
		TEXT("An identical registration is idempotent"),
		Service.RegisterClient(Codex, ReplayedSessionId, Error));
	TestEqual(
		TEXT("Idempotent registration reuses the session"),
		ReplayedSessionId,
		SessionId);

	FClientRegistration Conflict = Codex;
	Conflict.Name = TEXT("Different Client");
	FString ConflictSessionId;
	TestFalse(
		TEXT("An instance id cannot be rebound to another identity"),
		Service.RegisterClient(Conflict, ConflictSessionId, Error));
	TestTrue(
		TEXT("Identity conflicts return an explanation"),
		Error.Contains(TEXT("instanceId")));

	const FClientRegistration Claude =
		MakeMcpRegistration(TEXT("Claude Code"), TEXT("claude-instance"), 5678);
	FString ClaudeSessionId;
	TestTrue(
		TEXT("A second MCP client registers independently"),
		Service.RegisterClient(Claude, ClaudeSessionId, Error));
	TestEqual(
		TEXT("Two MCP clients are online"),
		Service.GetOnlineMcpCount(),
		2);

	FClientRegistration MissingInvocation =
		MakeCliRegistration(FString(), TEXT("invalid-cli"), TEXT("status"));
	FString InvalidCliSession;
	TestFalse(
		TEXT("CLI registration requires an invocation id"),
		Service.RegisterClient(
			MissingInvocation,
			InvalidCliSession,
			Error));

	const FClientRegistration Cli = MakeCliRegistration(
		TEXT("invocation-1"),
		TEXT("cli-instance"),
		TEXT("execute workflow.json"));
	FString CliSessionId;
	TestTrue(
		TEXT("A CLI invocation registers"),
		Service.RegisterClient(Cli, CliSessionId, Error));
	FString ReplayedCliSessionId;
	TestTrue(
		TEXT("An identical CLI registration is idempotent"),
		Service.RegisterClient(Cli, ReplayedCliSessionId, Error));
	TestEqual(
		TEXT("Idempotent CLI registration reuses the session"),
		ReplayedCliSessionId,
		CliSessionId);
	TestEqual(
		TEXT("CLI sessions are not counted as online MCP clients"),
		Service.GetOnlineMcpCount(),
		2);
	TestEqual(
		TEXT("The registered CLI invocation is running"),
		Service.GetRunningCliCount(),
		1);

	const TSharedPtr<FJsonObject> InitialSnapshot =
		Service.MakeSnapshot(10, 5);
	TestEqual(
		TEXT("The snapshot schema is stable"),
		InitialSnapshot->GetStringField(TEXT("schema")),
		FString(TEXT("ue.status-snapshot.v1")));
	TestTrue(
		TEXT("The snapshot exposes the MCP client list"),
		InitialSnapshot->HasTypedField<EJson::Array>(TEXT("mcpClients")));
	TestTrue(
		TEXT("The snapshot exposes CLI invocations"),
		InitialSnapshot->HasTypedField<EJson::Array>(TEXT("cliInvocations")));
	TestTrue(
		TEXT("The snapshot exposes recent executions"),
		InitialSnapshot->HasTypedField<EJson::Array>(TEXT("recentExecutions")));
	TestTrue(
		TEXT("The snapshot exposes independent statistics"),
		InitialSnapshot->HasTypedField<EJson::Object>(TEXT("statistics")));
	TestEqual(
		TEXT("Snapshot MCP count matches the live registry"),
		JsonInt(InitialSnapshot, TEXT("onlineMcpCount")),
		2);
	TestEqual(
		TEXT("Snapshot CLI count matches the invocation registry"),
		JsonInt(InitialSnapshot, TEXT("runningCliCount")),
		1);
	TestEqual(
		TEXT("Idempotent CLI registration creates one invocation"),
		JsonInt(
			InitialSnapshot->GetObjectField(TEXT("statistics")),
			TEXT("cliInvocations")),
		1);

	const TArray<TSharedPtr<FJsonValue>>& McpClients =
		InitialSnapshot->GetArrayField(TEXT("mcpClients"));
	TestEqual(TEXT("Both MCP clients are listed"), McpClients.Num(), 2);
	if (McpClients.Num() == 2)
	{
		TestEqual(
			TEXT("MCP clients are sorted by display name"),
			McpClients[0]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("Claude Code")));
		TestEqual(
			TEXT("The second MCP client retains its transport"),
			McpClients[1]->AsObject()->GetStringField(TEXT("transport")),
			FString(TEXT("stdio")));
	}

	TestTrue(
		TEXT("Heartbeat refreshes a registered session"),
		Service.Heartbeat(SessionId, Error));
	TestTrue(
		TEXT("A registered MCP client can unregister"),
		Service.UnregisterClient(SessionId, Error));
	TestFalse(
		TEXT("A removed session cannot heartbeat"),
		Service.Heartbeat(SessionId, Error));
	TestEqual(
		TEXT("The other MCP client remains online"),
		Service.GetOnlineMcpCount(),
		1);

	TestTrue(
		TEXT("Unregistering a CLI session succeeds"),
		Service.UnregisterClient(CliSessionId, Error));
	TestEqual(
		TEXT("CLI unregister completes its invocation"),
		Service.GetRunningCliCount(),
		0);
	const TSharedPtr<FJsonObject> CompletedCliSnapshot =
		Service.MakeSnapshot(10, 5);
	const TArray<TSharedPtr<FJsonValue>>& CompletedCli =
		CompletedCliSnapshot->GetArrayField(TEXT("cliInvocations"));
	const TSharedPtr<FJsonObject> CompletedInvocation =
		FindObjectByStringField(
			CompletedCli,
			TEXT("invocationId"),
			TEXT("invocation-1"));
	TestTrue(
		TEXT("The completed CLI invocation remains in recent history"),
		CompletedInvocation.IsValid());
	if (CompletedInvocation.IsValid())
	{
		TestEqual(
			TEXT("The CLI invocation is marked completed"),
			CompletedInvocation->GetStringField(TEXT("status")),
			FString(TEXT("completed")));
	}

	Service.ExpireSessions(
		FPlatformTime::Seconds()
			+ FClientActivityService::SessionTimeoutSeconds
			+ 1.0);
	TestEqual(
		TEXT("Idle MCP sessions expire"),
		Service.GetOnlineMcpCount(),
		0);

	FString ReplacementSessionId;
	TestTrue(
		TEXT("An expired instance id can register again"),
		Service.RegisterClient(Claude, ReplacementSessionId, Error));
	TestNotEqual(
		TEXT("Expired instance registration receives a new session id"),
		ReplacementSessionId,
		ClaudeSessionId);
	Service.DisconnectAllSessions();
	TestEqual(
		TEXT("Disconnecting the service clears all online MCP clients"),
		Service.GetOnlineMcpCount(),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClientActivityRequestLifetimeTest,
	"UE_AI_integration.Transport.ClientActivity.RequestLifetime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClientActivityRequestLifetimeTest::RunTest(
	const FString& Parameters)
{
	FClientActivityService Service;
	FString SessionId;
	FString Error;
	TestTrue(
		TEXT("The long-request fixture registers"),
		Service.RegisterClient(
			MakeMcpRegistration(
				TEXT("Codex"),
				TEXT("long-request-instance"),
				1234),
			SessionId,
			Error));

	FCallerContext RegisteredCaller;
	RegisteredCaller.SessionId = SessionId;
	RegisteredCaller.ClientKind = TEXT("legacy");
	RegisteredCaller.Name = TEXT("Spoofed Caller");
	TestTrue(
		TEXT("A registered session begins a request"),
		Service.BeginRequest(RegisteredCaller, Error));
	TestEqual(
		TEXT("Registered identity replaces untrusted request metadata"),
		RegisteredCaller.Name,
		FString(TEXT("Codex")));
	TestEqual(
		TEXT("Registered caller kind is restored from the session"),
		RegisteredCaller.ClientKind,
		FString(TEXT("mcp")));

	Service.ExpireSessions(
		FPlatformTime::Seconds()
			+ FClientActivityService::SessionTimeoutSeconds
			+ 1.0);
	TestEqual(
		TEXT("An active request prevents session expiry"),
		Service.GetOnlineMcpCount(),
		1);
	TestFalse(
		TEXT("An active request prevents premature unregister"),
		Service.UnregisterClient(SessionId, Error));
	TestTrue(
		TEXT("Busy unregister reports the active request"),
		Error.Contains(TEXT("active requests")));
	Service.EndRequest(RegisteredCaller);
	Service.ExpireSessions(
		FPlatformTime::Seconds()
			+ FClientActivityService::SessionTimeoutSeconds
			+ 1.0);
	TestEqual(
		TEXT("The session expires after its active request ends"),
		Service.GetOnlineMcpCount(),
		0);

	FCallerContext UnknownCaller;
	UnknownCaller.SessionId = TEXT("session-does-not-exist");
	TestFalse(
		TEXT("Unknown sessions cannot begin requests"),
		Service.BeginRequest(UnknownCaller, Error));

	FCallerContext LegacyCaller;
	TestTrue(
		TEXT("A stateless legacy caller remains supported"),
		Service.BeginRequest(LegacyCaller, Error));
	const FString LegacyEvent =
		Service.BeginActivity(LegacyCaller, TEXT("capability"));
	Service.UpdateCapabilityActivity(
		LegacyEvent,
		TEXT("blueprint.asset.get"),
		TEXT("legacy-request"),
		TEXT("readOnly"));
	Service.CompleteActivityFromHttp(
		LegacyEvent,
		200,
		SerializeEnvelope(true, MakeShared<FJsonObject>()));
	Service.EndRequest(LegacyCaller);
	TestEqual(
		TEXT("Legacy HTTP does not create a CLI invocation"),
		Service.GetRunningCliCount(),
		0);

	FCallerContext StatelessCli;
	StatelessCli.ClientKind = TEXT("cli");
	StatelessCli.Name = TEXT("ue");
	StatelessCli.Version = TEXT("0.6.0");
	StatelessCli.Transport = TEXT("http");
	StatelessCli.Command = TEXT("blueprint scan");
	StatelessCli.InstanceId = TEXT("stateless-cli-instance");
	StatelessCli.InvocationId = TEXT("stateless-cli-invocation");
	StatelessCli.Pid = 2468;
	TestTrue(
		TEXT("A stateless CLI fallback begins a request"),
		Service.BeginRequest(StatelessCli, Error));
	const FString CliEvent =
		Service.BeginActivity(StatelessCli, TEXT("capability"));
	Service.UpdateCapabilityActivity(
		CliEvent,
		TEXT("blueprint.scan"),
		TEXT("cli-request"),
		TEXT("readOnly"));
	Service.CompleteActivityFromHttp(
		CliEvent,
		200,
		SerializeEnvelope(true, MakeShared<FJsonObject>()));
	Service.EndRequest(StatelessCli);
	TestEqual(
		TEXT("A completed stateless CLI request is not left running"),
		Service.GetRunningCliCount(),
		0);

	const TSharedPtr<FJsonObject> Snapshot = Service.MakeSnapshot(10, 5);
	const TArray<TSharedPtr<FJsonValue>>& Invocations =
		Snapshot->GetArrayField(TEXT("cliInvocations"));
	const TSharedPtr<FJsonObject> StatelessInvocation =
		FindObjectByStringField(
			Invocations,
			TEXT("invocationId"),
			TEXT("stateless-cli-invocation"));
	TestTrue(
		TEXT("Stateless CLI activity remains visible as recent history"),
		StatelessInvocation.IsValid());
	if (StatelessInvocation.IsValid())
	{
		TestEqual(
			TEXT("The stateless CLI invocation reaches a terminal state"),
			StatelessInvocation->GetStringField(TEXT("status")),
			FString(TEXT("completed")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClientActivityExecutionAccountingTest,
	"UE_AI_integration.Transport.ClientActivity.ExecutionAccounting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClientActivityExecutionAccountingTest::RunTest(
	const FString& Parameters)
{
	FClientActivityService Service;
	FCallerContext Caller;
	Caller.ClientKind = TEXT("mcp");
	Caller.Name = TEXT("Codex");
	Caller.Version = TEXT("1.2.3");
	Caller.Transport = TEXT("stdio");
	Caller.InstanceId = TEXT("codex-instance");
	Caller.SessionId = TEXT("session-test");

	const FString FailedCapability =
		Service.BeginActivity(Caller, TEXT("capability"));
	Service.MarkActivityStarted(FailedCapability);
	Service.UpdateCapabilityActivity(
		FailedCapability,
		TEXT("blueprint.asset.compile"),
		TEXT("request-failed"),
		TEXT("safeWrite"));
	Service.CompleteActivityFromHttp(
		FailedCapability,
		422,
		SerializeEnvelope(
			false,
			nullptr,
			TEXT("asset_compile_failed"),
			TEXT("TOP_SECRET_RESPONSE_MESSAGE")));

	TSharedPtr<FJsonObject> JobData = MakeShared<FJsonObject>();
	JobData->SetStringField(TEXT("jobId"), TEXT("job-1"));
	JobData->SetStringField(TEXT("status"), TEXT("running"));
	JobData->SetStringField(
		TEXT("responseBody"),
		TEXT("TOP_SECRET_RESPONSE_BODY"));
	JobData->SetStringField(
		TEXT("params"),
		TEXT("TOP_SECRET_REQUEST_PARAMS"));
	const FString SubmittedCapability =
		Service.BeginActivity(Caller, TEXT("capability"));
	Service.MarkActivityStarted(SubmittedCapability);
	Service.UpdateCapabilityActivity(
		SubmittedCapability,
		TEXT("production.build.target"),
		TEXT("request-job"),
		TEXT("expensive"));
	Service.CompleteActivityFromHttp(
		SubmittedCapability,
		200,
		SerializeEnvelope(true, JobData));

	TSharedPtr<FJsonObject> PlanData = MakeShared<FJsonObject>();
	PlanData->SetStringField(TEXT("status"), TEXT("planned"));
	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("plan"),
		TEXT("workflow-plan"),
		PlanData);

	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("status"),
		TEXT("workflow-unknown-status"),
		MakeWorkflowData(TEXT("run-from-prior-editor"), TEXT("completed"), 99, 99));
	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("resume"),
		TEXT("workflow-unknown-resume"),
		MakeWorkflowData(TEXT("run-from-prior-editor"), TEXT("failed"), 99, 0));

	const FString RunId = TEXT("run-deduplicated");
	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("execute"),
		TEXT("workflow-execute"),
		MakeWorkflowData(RunId, TEXT("completed"), 3, 3));
	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("status"),
		TEXT("workflow-status"),
		MakeWorkflowData(RunId, TEXT("completed"), 3, 3));
	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("resume"),
		TEXT("workflow-resume"),
		MakeWorkflowData(RunId, TEXT("completed"), 3, 3));
	CompleteWorkflowActivity(
		Service,
		Caller,
		TEXT("rollback"),
		TEXT("workflow-rollback"),
		MakeWorkflowData(RunId, TEXT("rolledBack"), 3, 3));

	const TSharedPtr<FJsonObject> Snapshot = Service.MakeSnapshot(20, 5);
	const TSharedPtr<FJsonObject> Statistics =
		Snapshot->GetObjectField(TEXT("statistics"));
	TestEqual(
		TEXT("Both capability calls are counted"),
		JsonInt(Statistics, TEXT("capabilityCalls")),
		2);
	TestEqual(
		TEXT("Successful capability count is accurate"),
		JsonInt(Statistics, TEXT("capabilitySucceeded")),
		1);
	TestEqual(
		TEXT("Failed capability count is accurate"),
		JsonInt(Statistics, TEXT("capabilityFailed")),
		1);
	TestEqual(
		TEXT("Every online Workflow request is counted"),
		JsonInt(Statistics, TEXT("workflowApiCalls")),
		7);
	TestEqual(
		TEXT("Execute creates exactly one DSL run"),
		JsonInt(Statistics, TEXT("dslRuns")),
		1);
	TestEqual(
		TEXT("Status and resume do not duplicate completed run counts"),
		JsonInt(Statistics, TEXT("dslCompleted")),
		1);
	TestEqual(
		TEXT("The Workflow run is not counted as failed"),
		JsonInt(Statistics, TEXT("dslFailed")),
		0);
	TestEqual(
		TEXT("The Workflow run is not counted as blocked"),
		JsonInt(Statistics, TEXT("dslBlocked")),
		0);
	TestEqual(
		TEXT("Rollback is tracked without creating another run"),
		JsonInt(Statistics, TEXT("rollbacks")),
		1);
	TestEqual(
		TEXT("Unknown status/resume do not manufacture operation totals"),
		JsonInt(Statistics, TEXT("operationTotal")),
		3);
	TestEqual(
		TEXT("Unknown status/resume do not manufacture operation successes"),
		JsonInt(Statistics, TEXT("operationSucceeded")),
		3);

	const TArray<TSharedPtr<FJsonValue>>& Executions =
		Snapshot->GetArrayField(TEXT("recentExecutions"));
	const TSharedPtr<FJsonObject> FailedActivity =
		FindObjectByStringField(
			Executions,
			TEXT("requestId"),
			TEXT("request-failed"));
	TestTrue(
		TEXT("Failed capability metadata is retained"),
		FailedActivity.IsValid());
	if (FailedActivity.IsValid())
	{
		TestEqual(
			TEXT("Capability id is retained"),
			FailedActivity->GetStringField(TEXT("capability")),
			FString(TEXT("blueprint.asset.compile")));
		TestEqual(
			TEXT("Risk is retained"),
			FailedActivity->GetStringField(TEXT("risk")),
			FString(TEXT("safeWrite")));
		TestEqual(
			TEXT("HTTP failure status is retained"),
			JsonInt(FailedActivity, TEXT("httpStatus")),
			422);
		TestEqual(
			TEXT("Error code is retained"),
			FailedActivity->GetStringField(TEXT("errorCode")),
			FString(TEXT("asset_compile_failed")));
		TestEqual(
			TEXT("Activity status reflects the failed HTTP result"),
			FailedActivity->GetStringField(TEXT("status")),
			FString(TEXT("failed")));
		TestFalse(
			TEXT("Activity metadata never stores params"),
			FailedActivity->HasField(TEXT("params")));
		TestFalse(
			TEXT("Activity metadata never stores response bodies"),
			FailedActivity->HasField(TEXT("responseBody")));
		TestFalse(
			TEXT("Activity metadata never stores a generic body"),
			FailedActivity->HasField(TEXT("body")));
	}

	const TSharedPtr<FJsonObject> JobActivity =
		FindObjectByStringField(
			Executions,
			TEXT("requestId"),
			TEXT("request-job"));
	TestTrue(
		TEXT("Submitted durable job metadata is retained"),
		JobActivity.IsValid());
	if (JobActivity.IsValid())
	{
		TestEqual(
			TEXT("Accepted durable jobs are submitted, not completed"),
			JobActivity->GetStringField(TEXT("status")),
			FString(TEXT("submitted")));
		TestEqual(
			TEXT("Durable job id is correlated"),
			JobActivity->GetStringField(TEXT("jobId")),
			FString(TEXT("job-1")));
	}

	const TSharedPtr<FJsonObject> ExecuteActivity =
		FindObjectByStringField(
			Executions,
			TEXT("requestId"),
			TEXT("workflow-execute"));
	TestTrue(
		TEXT("Workflow execution metadata is retained"),
		ExecuteActivity.IsValid());
	if (ExecuteActivity.IsValid())
	{
		TestEqual(
			TEXT("Workflow action is retained"),
			ExecuteActivity->GetStringField(TEXT("workflowAction")),
			FString(TEXT("execute")));
		TestEqual(
			TEXT("Workflow run id is correlated"),
			ExecuteActivity->GetStringField(TEXT("runId")),
			RunId);
	}

	const FString SnapshotJson = SerializeObject(Snapshot);
	TestFalse(
		TEXT("Error messages are not copied into activity snapshots"),
		SnapshotJson.Contains(TEXT("TOP_SECRET_RESPONSE_MESSAGE")));
	TestFalse(
		TEXT("Response bodies are not copied into activity snapshots"),
		SnapshotJson.Contains(TEXT("TOP_SECRET_RESPONSE_BODY")));
	TestFalse(
		TEXT("Request params are not copied into activity snapshots"),
		SnapshotJson.Contains(TEXT("TOP_SECRET_REQUEST_PARAMS")));
	TestEqual(
		TEXT("Tooltip summary describes the latest Workflow action"),
		Service.GetLastExecutionResult(),
		FString(TEXT("rollback: succeeded")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClientActivityWorkflowEnvelopeBoundaryTest,
	"UE_AI_integration.Transport.ClientActivity.WorkflowEnvelopeBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClientActivityWorkflowEnvelopeBoundaryTest::RunTest(
	const FString& Parameters)
{
	FClientActivityService Service;
	FCallerContext Caller;
	Caller.ClientKind = TEXT("mcp");
	Caller.Name = TEXT("Codex");
	Caller.Transport = TEXT("stdio");

	const FString FailedRunId = TEXT("run-from-error-details");
	const FString FailedEvent =
		Service.BeginActivity(Caller, TEXT("workflow"));
	Service.MarkActivityStarted(FailedEvent);
	Service.UpdateWorkflowActivity(
		FailedEvent,
		TEXT("execute"),
		TEXT("workflow-error-details"),
		TEXT("safeWrite"));
	Service.CompleteActivityFromHttp(
		FailedEvent,
		500,
		SerializeEnvelope(
			false,
			nullptr,
			TEXT("workflow_execution_failed"),
			TEXT("The projected failure is intentionally compact."),
			MakeWorkflowData(FailedRunId, TEXT("failed"), 4, 2)));

	const FString LargeRunId = TEXT("run-from-large-envelope");
	TSharedPtr<FJsonObject> LargeData =
		MakeWorkflowData(LargeRunId, TEXT("completed"), 1, 1);
	FString Padding;
	Padding.Reserve(300 * 1024);
	for (int32 Index = 0; Index < 300 * 1024; ++Index)
	{
		Padding.AppendChar(TEXT('x'));
	}
	LargeData->SetStringField(TEXT("ignoredPayload"), Padding);
	const TArray<uint8> LargeBody = SerializeEnvelope(true, LargeData);
	TestTrue(
		TEXT("The fixture exceeds the former 256 KiB parsing limit"),
		LargeBody.Num() > 256 * 1024);
	TestTrue(
		TEXT("The fixture remains within the 4 MiB metadata parsing limit"),
		LargeBody.Num() < 4 * 1024 * 1024);

	const FString LargeEvent =
		Service.BeginActivity(Caller, TEXT("workflow"));
	Service.MarkActivityStarted(LargeEvent);
	Service.UpdateWorkflowActivity(
		LargeEvent,
		TEXT("execute"),
		TEXT("workflow-large-envelope"),
		TEXT("safeWrite"));
	Service.CompleteActivityFromHttp(LargeEvent, 200, LargeBody);

	const TSharedPtr<FJsonObject> Snapshot = Service.MakeSnapshot(10, 5);
	const TSharedPtr<FJsonObject> Statistics =
		Snapshot->GetObjectField(TEXT("statistics"));
	TestEqual(
		TEXT("Both execute calls create one distinct DSL run"),
		JsonInt(Statistics, TEXT("dslRuns")),
		2);
	TestEqual(
		TEXT("Failure status from error.details is counted"),
		JsonInt(Statistics, TEXT("dslFailed")),
		1);
	TestEqual(
		TEXT("The large successful envelope is parsed and counted"),
		JsonInt(Statistics, TEXT("dslCompleted")),
		1);
	TestEqual(
		TEXT("Operation totals include error.details and large data"),
		JsonInt(Statistics, TEXT("operationTotal")),
		5);
	TestEqual(
		TEXT("Operation successes include both parsed envelopes"),
		JsonInt(Statistics, TEXT("operationSucceeded")),
		3);

	const TArray<TSharedPtr<FJsonValue>>& Executions =
		Snapshot->GetArrayField(TEXT("recentExecutions"));
	const TSharedPtr<FJsonObject> FailedActivity =
		FindObjectByStringField(
			Executions,
			TEXT("requestId"),
			TEXT("workflow-error-details"));
	TestTrue(
		TEXT("Failed Workflow activity is retained"),
		FailedActivity.IsValid());
	if (FailedActivity.IsValid())
	{
		TestEqual(
			TEXT("Run id is extracted from error.details"),
			FailedActivity->GetStringField(TEXT("runId")),
			FailedRunId);
		TestEqual(
			TEXT("Failed Workflow keeps its error code"),
			FailedActivity->GetStringField(TEXT("errorCode")),
			FString(TEXT("workflow_execution_failed")));
		TestEqual(
			TEXT("Failed Workflow result stays failed"),
			FailedActivity->GetStringField(TEXT("status")),
			FString(TEXT("failed")));
	}

	const TSharedPtr<FJsonObject> LargeActivity =
		FindObjectByStringField(
			Executions,
			TEXT("requestId"),
			TEXT("workflow-large-envelope"));
	TestTrue(
		TEXT("Large Workflow activity is retained"),
		LargeActivity.IsValid());
	if (LargeActivity.IsValid())
	{
		TestEqual(
			TEXT("Run id is extracted from the large Workflow result"),
			LargeActivity->GetStringField(TEXT("runId")),
			LargeRunId);
	}
	TestFalse(
		TEXT("Large Workflow payload is not retained in the snapshot"),
		SerializeObject(Snapshot).Contains(Padding.Left(256)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClientActivityBoundedSnapshotTest,
	"UE_AI_integration.Transport.ClientActivity.BoundedSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClientActivityBoundedSnapshotTest::RunTest(
	const FString& Parameters)
{
	FClientActivityService Service;
	FCallerContext Caller;
	for (int32 Index = 0;
		Index < FClientActivityService::ActivityCapacity + 7;
		++Index)
	{
		const FString EventId =
			Service.BeginActivity(Caller, TEXT("capability"));
		Service.UpdateCapabilityActivity(
			EventId,
			TEXT("blueprint.asset.get"),
			FString::Printf(TEXT("request-%d"), Index),
			TEXT("readOnly"));
		Service.CompleteActivityFromHttp(
			EventId,
			200,
			TArray<uint8>());
	}

	const TSharedPtr<FJsonObject> Snapshot = Service.MakeSnapshot(7, 5);
	TestEqual(
		TEXT("The activity ring remains bounded"),
		JsonInt(Snapshot, TEXT("activityRingCount")),
		FClientActivityService::ActivityCapacity);
	TestEqual(
		TEXT("The activity ring advertises its fixed capacity"),
		JsonInt(Snapshot, TEXT("activityRingCapacity")),
		FClientActivityService::ActivityCapacity);

	const TSharedPtr<FJsonObject> Statistics =
		Snapshot->GetObjectField(TEXT("statistics"));
	TestEqual(
		TEXT("Cumulative statistics survive ring eviction"),
		JsonInt(Statistics, TEXT("capabilityCalls")),
		FClientActivityService::ActivityCapacity + 7);
	TestEqual(
		TEXT("All evicted successful calls remain in cumulative statistics"),
		JsonInt(Statistics, TEXT("capabilitySucceeded")),
		FClientActivityService::ActivityCapacity + 7);

	const TArray<TSharedPtr<FJsonValue>>& Recent =
		Snapshot->GetArrayField(TEXT("recentExecutions"));
	TestEqual(TEXT("The requested execution limit is respected"), Recent.Num(), 7);
	if (Recent.Num() == 7)
	{
		TestEqual(
			TEXT("Recent executions are newest first"),
			Recent[0]->AsObject()->GetStringField(TEXT("requestId")),
			FString::Printf(
				TEXT("request-%d"),
				FClientActivityService::ActivityCapacity + 6));
		TestEqual(
			TEXT("The bounded page retains deterministic order"),
			Recent[6]->AsObject()->GetStringField(TEXT("requestId")),
			FString::Printf(
				TEXT("request-%d"),
				FClientActivityService::ActivityCapacity));
	}
	TestEqual(
		TEXT("A zero execution limit returns no execution rows"),
		Service.MakeSnapshot(0, 5)
			->GetArrayField(TEXT("recentExecutions"))
			.Num(),
		0);

	FClientActivityService CliService;
	FString Error;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		const FClientRegistration Registration = MakeCliRegistration(
			FString::Printf(TEXT("completed-%d"), Index),
			FString::Printf(TEXT("completed-instance-%d"), Index),
			TEXT("status"));
		FString SessionId;
		TestTrue(
			FString::Printf(TEXT("CLI fixture %d registers"), Index),
			CliService.RegisterClient(
				Registration,
				SessionId,
				Error));
		TestTrue(
			FString::Printf(TEXT("CLI fixture %d unregisters"), Index),
			CliService.UnregisterClient(SessionId, Error));
	}

	FString RunningSessionId;
	TestTrue(
		TEXT("A running CLI fixture registers"),
		CliService.RegisterClient(
			MakeCliRegistration(
				TEXT("running-invocation"),
				TEXT("running-instance"),
				TEXT("execute")),
			RunningSessionId,
			Error));
	const TSharedPtr<FJsonObject> CliSnapshot =
		CliService.MakeSnapshot(10, 2);
	const TArray<TSharedPtr<FJsonValue>>& CliInvocations =
		CliSnapshot->GetArrayField(TEXT("cliInvocations"));
	TestEqual(
		TEXT("All running CLI entries plus two recent entries are returned"),
		CliInvocations.Num(),
		3);
	if (CliInvocations.Num() == 3)
	{
		TestEqual(
			TEXT("Running CLI entries sort before recent history"),
			CliInvocations[0]->AsObject()->GetStringField(TEXT("status")),
			FString(TEXT("running")));
	}
	TestEqual(
		TEXT("The recent CLI limit does not hide running invocations"),
		JsonInt(CliSnapshot, TEXT("runningCliCount")),
		1);
	TestEqual(
		TEXT("CLI invocation statistics are independent of recent limits"),
		JsonInt(
			CliSnapshot->GetObjectField(TEXT("statistics")),
			TEXT("cliInvocations")),
		6);

	FClientActivityService PruningService;
	FCallerContext ActiveCli;
	ActiveCli.ClientKind = TEXT("cli");
	ActiveCli.Name = TEXT("ue");
	ActiveCli.Version = TEXT("0.6.0");
	ActiveCli.Transport = TEXT("http");
	ActiveCli.Command = TEXT("production test");
	ActiveCli.InvocationId = TEXT("active-invocation");
	TestTrue(
		TEXT("The active CLI pruning fixture begins its request"),
		PruningService.BeginRequest(ActiveCli, Error));
	for (int32 Index = 0;
		Index < FClientActivityService::MaxRecentCliInvocations + 5;
		++Index)
	{
		FString SessionId;
		TestTrue(
			FString::Printf(TEXT("Pruning CLI fixture %d registers"), Index),
			PruningService.RegisterClient(
				MakeCliRegistration(
					FString::Printf(TEXT("pruned-%d"), Index),
					FString::Printf(TEXT("pruned-instance-%d"), Index),
					TEXT("status")),
				SessionId,
				Error));
		TestTrue(
			FString::Printf(TEXT("Pruning CLI fixture %d unregisters"), Index),
			PruningService.UnregisterClient(SessionId, Error));
	}

	const TSharedPtr<FJsonObject> ActiveOnlySnapshot =
		PruningService.MakeSnapshot(0, 0);
	const TArray<TSharedPtr<FJsonValue>>& ActiveInvocations =
		ActiveOnlySnapshot->GetArrayField(TEXT("cliInvocations"));
	TestEqual(
		TEXT("Recent CLI pruning keeps the active request"),
		ActiveInvocations.Num(),
		1);
	if (ActiveInvocations.Num() == 1)
	{
		TestEqual(
			TEXT("The surviving invocation is the active request"),
			ActiveInvocations[0]->AsObject()->GetStringField(
				TEXT("invocationId")),
			FString(TEXT("active-invocation")));
		TestEqual(
			TEXT("The active invocation remains running"),
			ActiveInvocations[0]->AsObject()->GetStringField(TEXT("status")),
			FString(TEXT("running")));
	}
	TestEqual(
		TEXT("CLI invocation totals survive recent-history pruning"),
		JsonInt(
			ActiveOnlySnapshot->GetObjectField(TEXT("statistics")),
			TEXT("cliInvocations")),
		FClientActivityService::MaxRecentCliInvocations + 6);
	PruningService.EndRequest(ActiveCli);
	TestEqual(
		TEXT("The active CLI request can complete after pruning"),
		PruningService.GetRunningCliCount(),
		0);
	return true;
}

#endif
