#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/MCPToolRegistry.h"
#include "Transport/MCPHttpContract.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Workflow/UEWorkflowRuntime.h"

namespace
{
TSharedPtr<FJsonObject> ParseBody(const FString& Body)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	FJsonSerializer::Deserialize(Reader, Json);
	return Json;
}

TSharedPtr<FJsonObject> ParseBody(const FHttpServerResponse& Response)
{
	const FUTF8ToTCHAR Converter(
		reinterpret_cast<const ANSICHAR*>(Response.Body.GetData()),
		Response.Body.Num());
	return ParseBody(FString(Converter.Length(), Converter.Get()));
}

FString SerializeBody(const TSharedPtr<FJsonObject>& Object)
{
	FString Body;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Body;
}

TSharedPtr<FJsonObject> MakeBlueprintEditWorkflow(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Scope = MakeShared<FJsonObject>();
	Scope->SetStringField(TEXT("kind"), TEXT("blueprint"));
	Scope->SetStringField(TEXT("asset"), AssetPath);
	Scope->SetBoolField(TEXT("createIfMissing"), false);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("variableName"), TEXT("HttpWorkflowValue"));
	Params->SetStringField(TEXT("variableType"), TEXT("Float"));
	TSharedPtr<FJsonObject> Operation = MakeShared<FJsonObject>();
	Operation->SetStringField(TEXT("id"), TEXT("addVariable"));
	Operation->SetStringField(
		TEXT("type"),
		TEXT("blueprint.variable.add"));
	Operation->SetObjectField(TEXT("params"), Params);

	// Compatibility-shaped author input must not bypass the v1 mandatory
	// compile/read-back/diff finalization chain.
	TSharedPtr<FJsonObject> Verify = MakeShared<FJsonObject>();
	Verify->SetBoolField(TEXT("compile"), false);
	Verify->SetArrayField(
		TEXT("readBack"),
		TArray<TSharedPtr<FJsonValue>>{});

	TSharedPtr<FJsonObject> Workflow = MakeShared<FJsonObject>();
	Workflow->SetStringField(TEXT("dsl"), TEXT("ue.workflow"));
	Workflow->SetStringField(TEXT("dslVersion"), TEXT("1.0"));
	Workflow->SetStringField(TEXT("workflowKind"), TEXT("assetEdit"));
	Workflow->SetStringField(
		TEXT("workflowId"),
		TEXT("http-workflow-execute-e2e"));
	Workflow->SetObjectField(TEXT("scope"), Scope);
	Workflow->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
	Workflow->SetArrayField(
		TEXT("operations"),
		{MakeShared<FJsonValueObject>(Operation)});
	Workflow->SetObjectField(TEXT("verify"), Verify);
	return Workflow;
}

TSharedPtr<FJsonObject> MakeWorkflowExecuteEnvelope(
	const TSharedPtr<FJsonObject>& Workflow,
	const FString& PlanDigest)
{
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("action"), TEXT("execute"));
	Envelope->SetObjectField(TEXT("workflow"), Workflow);
	Envelope->SetStringField(TEXT("approvePlanDigest"), PlanDigest);
	Envelope->SetBoolField(TEXT("saveOnSuccess"), false);
	return Envelope;
}

bool CreateBlueprintFixture(
	FMCPToolRegistry& Registry,
	const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("blueprintName"),
		FPackageName::GetShortName(PackageName));
	Params->SetStringField(
		TEXT("packagePath"),
		FPackageName::GetLongPackagePath(PackageName));
	Params->SetStringField(TEXT("parentClass"), TEXT("Actor"));
	const FMCPToolResult CreateResult = Registry.ExecuteTool(
		TEXT("blueprint.asset.create"),
		Params);
	if (!CreateResult.bSuccess)
	{
		return false;
	}

	// Connected planning deliberately rejects Dirty assets. Persist the
	// fixture before deriving its approval digest so this test exercises an
	// actual existing-asset baseline instead of bypassing that safety gate.
	TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
	SaveParams->SetStringField(TEXT("blueprint"), AssetPath);
	return Registry.ExecuteTool(
		TEXT("blueprint.asset.save"),
		SaveParams).bSuccess;
}

UObject* FindLoadedFixture(const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	UPackage* Package = FindPackage(nullptr, *PackageName);
	UObject* Asset = Package
		? FindObject<UObject>(
			Package,
			*FPackageName::GetShortName(PackageName))
		: nullptr;
	return IsValid(Asset) && Asset->IsAsset() ? Asset : nullptr;
}

bool CleanupBlueprintFixture(const FString& AssetPath)
{
	if (!AssetPath.StartsWith(TEXT("/Game/Automation/BP_HttpWorkflow_")))
	{
		return false;
	}
	if (UObject* Asset = FindLoadedFixture(AssetPath))
	{
		Asset->GetOutermost()->SetDirtyFlag(false);
		TArray<UObject*> AssetsToDelete{Asset};
		ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
	}
	return FindLoadedFixture(AssetPath) == nullptr
		&& !FPackageName::DoesPackageExist(
			FPackageName::ObjectPathToPackageName(AssetPath));
}

struct FWorkflowHttpE2EState
{
	FUEAIIntegrationServer* Server = nullptr;
	TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
	FString AssetPath;
	FDelegateHandle CompileHandle;
	int32 HttpCompletionCount = 0;
	int32 RuntimeDispatchCount = 0;
	int32 BlueprintCompileCount = 0;
	int32 ResponseCode = 0;
	FString ResponseBody;
	double DeadlineSeconds = 0.0;
	double CompletionSeconds = 0.0;
	bool bHttpCompleted = false;
	bool bTransportSucceeded = false;
};

using FWorkflowHttpE2EStateRef =
	TSharedRef<FWorkflowHttpE2EState, ESPMode::ThreadSafe>;

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FWaitForWorkflowHttpE2E,
	FWorkflowHttpE2EStateRef,
	State,
	FAutomationTestBase*,
	Test);

bool FWaitForWorkflowHttpE2E::Update()
{
	const double NowSeconds = FPlatformTime::Seconds();
	if (!State->bHttpCompleted && NowSeconds < State->DeadlineSeconds)
	{
		return false;
	}
	if (State->bHttpCompleted
		&& NowSeconds - State->CompletionSeconds < 0.25)
	{
		// Keep the observer installed for a few more editor ticks so an
		// accidental duplicate queue dispatch cannot hide behind the first
		// successful HTTP completion.
		return false;
	}

	if (State->Server)
	{
		State->Server->SetWorkflowDispatchObserverForTesting(
			TFunction<void()>());
	}
	if (GEditor && State->CompileHandle.IsValid())
	{
		GEditor->OnBlueprintPreCompile().Remove(State->CompileHandle);
		State->CompileHandle.Reset();
	}

	if (!State->bHttpCompleted)
	{
		if (const FHttpRequestPtr Request = State->Request.Pin())
		{
			Request->CancelRequest();
		}
		Test->AddError(
			TEXT("Timed out waiting for POST /api/v1/workflow."));
		Test->TestTrue(
			TEXT("Timed-out HTTP fixture is removed"),
			CleanupBlueprintFixture(State->AssetPath));
		return true;
	}

	Test->TestTrue(
		TEXT("Loopback HTTP transport completed"),
		State->bTransportSucceeded);
	Test->TestEqual(
		TEXT("One POST produces one HTTP completion"),
		State->HttpCompletionCount,
		1);
	Test->TestEqual(
		TEXT("One workflow envelope dispatches to runtime exactly once"),
		State->RuntimeDispatchCount,
		1);
	Test->TestEqual(
		TEXT("Valid workflow action returns HTTP 200"),
		State->ResponseCode,
		200);
	Test->TestEqual(
		TEXT("Existing-asset workflow compiles exactly once"),
		State->BlueprintCompileCount,
		1);

	const TSharedPtr<FJsonObject> Response =
		ParseBody(State->ResponseBody);
	bool bOk = false;
	const TSharedPtr<FJsonObject>* Receipt = nullptr;
	Test->TestTrue(
		TEXT("Runtime response is a successful canonical envelope"),
		Response.IsValid()
			&& Response->TryGetBoolField(TEXT("ok"), bOk)
			&& bOk
			&& Response->TryGetObjectField(TEXT("data"), Receipt)
			&& Receipt
			&& Receipt->IsValid());
	if (Receipt && Receipt->IsValid())
	{
		FString Status;
		Test->TestTrue(
			TEXT("HTTP execute receipt is completed"),
			(*Receipt)->TryGetStringField(TEXT("status"), Status)
				&& Status == TEXT("completed"));

		Test->TestEqual(TEXT("HTTP execute defaults to summary detail"),
		                (*Receipt)->GetStringField(TEXT("detailLevel")), FString(TEXT("summary")));
		Test->TestFalse(TEXT("HTTP summary omits operation output"),
		                (*Receipt)->HasField(TEXT("operations")));
		Test->TestFalse(TEXT("HTTP summary omits full sections"),
		                (*Receipt)->HasField(TEXT("sections")));
		const TSharedPtr<FJsonObject>* Summary = nullptr;
		const TSharedPtr<FJsonObject>* Operations = nullptr;
		const TSharedPtr<FJsonObject>* Finalizers = nullptr;
		double OperationTotal = 0.0;
		double FinalizerSucceeded = 0.0;
		Test->TestTrue(
		    TEXT("HTTP summary reports operation/finalizer counts"),
		    (*Receipt)->TryGetObjectField(TEXT("summary"), Summary) && Summary &&
		        (*Summary)->TryGetObjectField(TEXT("operations"), Operations) && Operations &&
		        (*Operations)->TryGetNumberField(TEXT("total"), OperationTotal) &&
		        OperationTotal == 1.0 &&
		        (*Summary)->TryGetObjectField(TEXT("finalizers"), Finalizers) && Finalizers &&
		        (*Finalizers)->TryGetNumberField(TEXT("succeeded"), FinalizerSucceeded) &&
		        FinalizerSucceeded > 0.0);
		Test->TestTrue(TEXT("HTTP default execute response stays below 8 KiB"),
		               FTCHARToUTF8(*State->ResponseBody).Length() <= 8 * 1024);
	}
	Test->TestTrue(
		TEXT("HTTP workflow fixture is removed"),
		CleanupBlueprintFixture(State->AssetPath));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPHttpEnvelopeContractTest,
	"UE_AI_integration.Transport.HttpEnvelopeAndStatusCodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPHttpEnvelopeContractTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::Transport;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("value"), TEXT("ok"));
	const TUniquePtr<FHttpServerResponse> Success =
		MakeJsonResponse(200, MakeSuccessEnvelope(Data));
	TestEqual(TEXT("Success response status"), static_cast<int32>(Success->Code), 200);
	const TSharedPtr<FJsonObject> SuccessBody = ParseBody(*Success);
	TestTrue(TEXT("Success envelope is valid JSON"), SuccessBody.IsValid());
	TestTrue(TEXT("Success envelope ok=true"), SuccessBody->GetBoolField(TEXT("ok")));
	TestEqual(
		TEXT("Success envelope data"),
		SuccessBody->GetObjectField(TEXT("data"))->GetStringField(TEXT("value")),
		FString(TEXT("ok")));

	const TPair<int32, FString> Cases[] = {
		{400, TEXT("invalid_json")},
		{404, TEXT("capability_not_found")},
		{422, TEXT("invalid_params")},
		{500, TEXT("execution_failed")},
	};
	for (const TPair<int32, FString>& Case : Cases)
	{
		const TUniquePtr<FHttpServerResponse> Error = MakeJsonResponse(
			Case.Key,
			MakeErrorEnvelope(Case.Value, TEXT("contract test")));
		TestEqual(
			*FString::Printf(TEXT("%d status is applied to the response object"), Case.Key),
			static_cast<int32>(Error->Code),
			Case.Key);
		const TSharedPtr<FJsonObject> ErrorBody = ParseBody(*Error);
		TestTrue(TEXT("Error envelope is valid JSON"), ErrorBody.IsValid());
		TestFalse(TEXT("Error envelope ok=false"), ErrorBody->GetBoolField(TEXT("ok")));
		TestEqual(
			TEXT("Error envelope code"),
			ErrorBody->GetObjectField(TEXT("error"))->GetStringField(TEXT("code")),
			Case.Value);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPWorkflowHttpRuntimeE2ETest,
	"UE_AI_integration.Transport.WorkflowExecutePostE2E",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPWorkflowHttpRuntimeE2ETest::RunTest(const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	FUEAIIntegrationServer* Server =
		Subsystem ? Subsystem->GetServer() : nullptr;
	if (!Server || !Server->IsRunning())
	{
		AddError(TEXT("UE integration HTTP server is not running."));
		return false;
	}
	FMCPToolRegistry* Registry = Subsystem->GetRegistry();
	if (!Registry)
	{
		AddError(TEXT("UE integration registry is not available."));
		return false;
	}

	const FString AssetPath = FString::Printf(
		TEXT("/Game/Automation/BP_HttpWorkflow_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!CreateBlueprintFixture(*Registry, AssetPath))
	{
		AddError(TEXT("Could not create the HTTP workflow Blueprint fixture."));
		return false;
	}

	const TSharedPtr<FJsonObject> Workflow =
		MakeBlueprintEditWorkflow(AssetPath);
	UEAIIntegration::Workflow::FWorkflowRuntime* PlannerRuntime =
		Server->GetWorkflowRuntimeForTesting();
	if (!PlannerRuntime)
	{
		CleanupBlueprintFixture(AssetPath);
		AddError(TEXT("The server workflow runtime is not available."));
		return false;
	}
	TSharedPtr<FJsonObject> PlanRequest = MakeShared<FJsonObject>();
	PlanRequest->SetStringField(TEXT("action"), TEXT("plan"));
	PlanRequest->SetObjectField(TEXT("workflow"), Workflow);
	const FMCPResult PlanResult = PlannerRuntime->HandleRequest(PlanRequest);
	FString PlanDigest;
	if (!PlanResult.bOk
		|| !PlanResult.Data.IsValid()
		|| !PlanResult.Data->TryGetStringField(
			TEXT("planDigest"),
			PlanDigest)
		|| PlanDigest.IsEmpty())
	{
		CleanupBlueprintFixture(AssetPath);
		AddError(TEXT("Could not plan the HTTP workflow fixture."));
		return false;
	}

	const FWorkflowHttpE2EStateRef State =
		MakeShared<FWorkflowHttpE2EState, ESPMode::ThreadSafe>();
	State->Server = Server;
	State->AssetPath = AssetPath;
	State->DeadlineSeconds = FPlatformTime::Seconds() + 10.0;
	const FString FixturePackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	State->CompileHandle = GEditor->OnBlueprintPreCompile().AddLambda(
		[State, FixturePackageName](UBlueprint* Blueprint)
		{
			if (Blueprint
				&& FPackageName::ObjectPathToPackageName(
					Blueprint->GetPathName()) == FixturePackageName)
			{
				++State->BlueprintCompileCount;
			}
		});
	Server->SetWorkflowDispatchObserverForTesting(
		[State]()
		{
			++State->RuntimeDispatchCount;
		});

	const FHttpRequestRef Request =
		FHttpModule::Get().CreateRequest();
	State->Request = Request;
	Request->SetURL(
		FString::Printf(
			TEXT("http://127.0.0.1:%d/api/v1/workflow"),
			Server->GetPort()));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(
		TEXT("Content-Type"),
		TEXT("application/json"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Request->SetContentAsString(
		SerializeBody(
			MakeWorkflowExecuteEnvelope(
				Workflow,
				PlanDigest)));
	Request->OnProcessRequestComplete().BindLambda(
		[State](
			FHttpRequestPtr,
			FHttpResponsePtr Response,
			bool bSucceeded)
		{
			++State->HttpCompletionCount;
			State->bTransportSucceeded =
				bSucceeded && Response.IsValid();
			State->ResponseCode =
				Response.IsValid() ? Response->GetResponseCode() : 0;
			State->ResponseBody =
				Response.IsValid()
					? Response->GetContentAsString()
					: FString();
			State->CompletionSeconds = FPlatformTime::Seconds();
			State->bHttpCompleted = true;
		});

	if (!Request->ProcessRequest())
	{
		Server->SetWorkflowDispatchObserverForTesting(
			TFunction<void()>());
		if (GEditor && State->CompileHandle.IsValid())
		{
			GEditor->OnBlueprintPreCompile().Remove(State->CompileHandle);
			State->CompileHandle.Reset();
		}
		CleanupBlueprintFixture(AssetPath);
		AddError(TEXT("Could not start loopback workflow HTTP request."));
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(
		FWaitForWorkflowHttpE2E(State, this));
	return true;
}

#endif
