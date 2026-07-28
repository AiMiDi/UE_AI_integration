#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Workflow/UEWorkflowRuntime.h"

namespace
{
TSharedPtr<FJsonObject> MakeScope(
	const FString& Kind,
	const FString& Asset,
	bool bCreateIfMissing = true)
{
	TSharedPtr<FJsonObject> Scope = MakeShared<FJsonObject>();
	Scope->SetStringField(TEXT("kind"), Kind);
	Scope->SetStringField(TEXT("asset"), Asset);
	Scope->SetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
	return Scope;
}

TSharedPtr<FJsonObject> MakeOperation(
	const FString& Id,
	const FString& Type,
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Operation = MakeShared<FJsonObject>();
	Operation->SetStringField(TEXT("id"), Id);
	Operation->SetStringField(TEXT("type"), Type);
	Operation->SetObjectField(
		TEXT("params"),
		Params.IsValid() ? Params : MakeShared<FJsonObject>());
	return Operation;
}

TSharedPtr<FJsonObject> MakeWorkflow(
	const FString& WorkflowId,
	const TSharedPtr<FJsonObject>& Scope,
	const TArray<TSharedPtr<FJsonValue>>& Operations)
{
	TSharedPtr<FJsonObject> Workflow = MakeShared<FJsonObject>();
	Workflow->SetStringField(TEXT("dsl"), TEXT("ue.workflow"));
	Workflow->SetStringField(TEXT("dslVersion"), TEXT("1.0"));
	Workflow->SetStringField(TEXT("workflowKind"), TEXT("assetEdit"));
	Workflow->SetStringField(TEXT("workflowId"), WorkflowId);
	Workflow->SetObjectField(TEXT("scope"), Scope);
	Workflow->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
	Workflow->SetArrayField(TEXT("operations"), Operations);

	TSharedPtr<FJsonObject> Verify = MakeShared<FJsonObject>();
	Verify->SetBoolField(TEXT("compile"), true);
	Workflow->SetObjectField(TEXT("verify"), Verify);
	return Workflow;
}

FMCPResult PlanWorkflow(
	UEAIIntegration::Workflow::FWorkflowRuntime& Runtime,
	const TSharedPtr<FJsonObject>& Workflow)
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("action"), TEXT("plan"));
	Request->SetObjectField(TEXT("workflow"), Workflow);
	return Runtime.HandleRequest(Request);
}

FMCPResult ExecuteWorkflow(UEAIIntegration::Workflow::FWorkflowRuntime& Runtime,
                           const TSharedPtr<FJsonObject>& Workflow, const FString& PlanDigest,
                           bool bSaveOnSuccess = false, const FString& DetailLevel = TEXT("full"))
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("action"), TEXT("execute"));
	Request->SetObjectField(TEXT("workflow"), Workflow);
	Request->SetStringField(TEXT("approvePlanDigest"), PlanDigest);
	Request->SetStringField(TEXT("detailLevel"), DetailLevel);
	if (bSaveOnSuccess)
	{
		Request->SetBoolField(TEXT("saveOnSuccess"), true);
	}
	return Runtime.HandleRequest(Request);
}

const TSharedPtr<FJsonObject>* GetResultSections(const TSharedPtr<FJsonObject>& Result)
{
	const TSharedPtr<FJsonObject>* Sections = nullptr;
	return Result.IsValid() && Result->TryGetObjectField(TEXT("sections"), Sections) && Sections &&
	               Sections->IsValid()
	           ? Sections
	           : nullptr;
}

bool TryGetResultArray(const TSharedPtr<FJsonObject>& Result, const FString& SectionName,
                       const TArray<TSharedPtr<FJsonValue>>*& OutValues)
{
	const TSharedPtr<FJsonObject>* Sections = GetResultSections(Result);
	return Sections && (*Sections)->TryGetArrayField(SectionName, OutValues) && OutValues;
}

bool TryGetResultObject(const TSharedPtr<FJsonObject>& Result, const FString& SectionName,
                        const TSharedPtr<FJsonObject>*& OutValue)
{
	const TSharedPtr<FJsonObject>* Sections = GetResultSections(Result);
	return Sections && (*Sections)->TryGetObjectField(SectionName, OutValue) && OutValue &&
	       OutValue->IsValid();
}

FString SerializeJsonObject(const TSharedPtr<FJsonObject>& Object)
{
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Json;
}

UObject* FindAssetWithoutLoading(const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	if (PackageName.IsEmpty())
	{
		return nullptr;
	}

	UPackage* Package = FindPackage(nullptr, *PackageName);
	UObject* Asset = Package
		? FindObject<UObject>(
			Package,
			*FPackageName::GetShortName(PackageName))
		: nullptr;
	return IsValid(Asset) && Asset->IsAsset() ? Asset : nullptr;
}

FAssetData FindOnDiskAssetWithoutLoading(const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	if (PackageName.IsEmpty()
		|| !FPackageName::DoesPackageExist(PackageName))
	{
		return FAssetData();
	}

	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*PackageName,
		*FPackageName::GetShortName(PackageName));
	return FAssetRegistryModule::GetRegistry().GetAssetByObjectPath(
		FSoftObjectPath(ObjectPath),
		true);
}

bool AssetExistsWithoutLoading(const FString& AssetPath)
{
	UObject* LoadedAsset = FindAssetWithoutLoading(AssetPath);
	return (IsValid(LoadedAsset) && LoadedAsset->IsAsset())
		|| FindOnDiskAssetWithoutLoading(AssetPath).IsValid();
}

bool CleanupAsset(const FString& AssetPath)
{
	if (!AssetPath.StartsWith(TEXT("/Game/Automation/")))
	{
		return false;
	}
	FAssetData AssetData;
	if (UObject* LoadedAsset = FindAssetWithoutLoading(AssetPath);
		IsValid(LoadedAsset) && LoadedAsset->IsAsset())
	{
		// Test fixtures intentionally exercise dirty-only workflows. Clear the
		// package dirty bit before deletion so ValidateOnSave cannot race the
		// teardown and recreate the just-deleted asset on the next Editor tick.
		LoadedAsset->GetOutermost()->SetDirtyFlag(false);
		AssetData = FAssetData(LoadedAsset);
	}
	else
	{
		AssetData = FindOnDiskAssetWithoutLoading(AssetPath);
	}
	if (AssetData.IsValid())
	{
		TArray<FAssetData> AssetsToDelete{AssetData};
		ObjectTools::DeleteAssets(AssetsToDelete, false);
	}
	if (AssetExistsWithoutLoading(AssetPath))
	{
		// The save-validation queue can temporarily count a just-created Widget
		// Blueprint as referenced even after all test-owned strong references are
		// gone. This fallback is restricted to GUID-named Automation fixtures,
		// clears the dirty bit, and still requires the read-back postcondition.
		if (UObject* LoadedAsset = FindAssetWithoutLoading(AssetPath);
			IsValid(LoadedAsset) && LoadedAsset->IsAsset())
		{
			LoadedAsset->GetOutermost()->SetDirtyFlag(false);
			TArray<UObject*> AssetsToDelete{LoadedAsset};
			ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
		}
	}
	return !AssetExistsWithoutLoading(AssetPath);
}

FString UniqueAssetPath(const FString& Prefix)
{
	return FString::Printf(
		TEXT("/Game/Automation/%s_%s"),
		*Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

bool GetPlanDigest(const FMCPResult& Plan, FString& OutDigest)
{
	return Plan.bOk
		&& Plan.Data.IsValid()
		&& Plan.Data->TryGetStringField(TEXT("planDigest"), OutDigest)
		&& !OutDigest.IsEmpty();
}

int32 CountFinalizersByKind(
	const TSharedPtr<FJsonObject>& Plan,
	const FString& Kind)
{
	const TArray<TSharedPtr<FJsonValue>>* Finalizers = nullptr;
	if (!Plan.IsValid()
		|| !Plan->TryGetArrayField(TEXT("finalizers"), Finalizers)
		|| !Finalizers)
	{
		return 0;
	}

	int32 Count = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Finalizers)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		FString ActualKind;
		if (Value->AsObject()->TryGetStringField(TEXT("kind"), ActualKind)
			&& ActualKind == Kind)
		{
			++Count;
		}
	}
	return Count;
}

bool AllSucceededStepsReportDeferredAndUnsaved(
	const TSharedPtr<FJsonObject>& Receipt)
{
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Receipt.IsValid() || !TryGetResultArray(Receipt, TEXT("operations"), Operations) ||
	    !Operations || Operations->IsEmpty())
	{
		return false;
	}

	int32 SucceededCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Operations)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Operation = Value->AsObject();
		if (Operation->GetStringField(TEXT("status")) != TEXT("succeeded"))
		{
			continue;
		}
		++SucceededCount;
		const TSharedPtr<FJsonObject>* Data = nullptr;
		bool bSaved = true;
		bool bDeferredCompile = false;
		if (!Operation->TryGetObjectField(TEXT("data"), Data)
			|| !Data
			|| !Data->IsValid()
			|| !(*Data)->TryGetBoolField(TEXT("saved"), bSaved)
			|| bSaved
			|| !(*Data)->TryGetBoolField(
				TEXT("deferredCompile"),
				bDeferredCompile)
			|| !bDeferredCompile)
		{
			return false;
		}
	}
	return SucceededCount > 0;
}

bool AllExecutableFinalizersReportSucceededWithoutOutput(const TSharedPtr<FJsonObject>& Receipt)
{
	const TArray<TSharedPtr<FJsonValue>>* Finalizers = nullptr;
	if (!Receipt.IsValid() || !TryGetResultArray(Receipt, TEXT("finalizers"), Finalizers) ||
	    !Finalizers)
	{
		return false;
	}

	int32 ExecutableCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Finalizers)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Finalizer = Value->AsObject();
		FString Type;
		Finalizer->TryGetStringField(TEXT("type"), Type);
		if (Type.IsEmpty())
		{
			continue;
		}
		++ExecutableCount;
		if (Finalizer->GetStringField(TEXT("status")) != TEXT("succeeded") ||
		    Finalizer->HasField(TEXT("data")))
		{
			return false;
		}
	}
	return ExecutableCount > 0;
}

bool HasDirtyPackages(const TSharedPtr<FJsonObject>& Receipt)
{
	const TSharedPtr<FJsonObject>* Summary = nullptr;
	double DirtyPackageCount = 0.0;
	return Receipt.IsValid() && Receipt->TryGetObjectField(TEXT("summary"), Summary) && Summary &&
	       Summary->IsValid() &&
	       (*Summary)->TryGetNumberField(TEXT("dirtyPackageCount"), DirtyPackageCount) &&
	       DirtyPackageCount > 0.0;
}

bool OperationReportsBool(
	const TSharedPtr<FJsonObject>& Receipt,
	const FString& OperationId,
	const FString& Field,
	bool Expected)
{
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Receipt.IsValid() || !TryGetResultArray(Receipt, TEXT("operations"), Operations) ||
	    !Operations)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Operations)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Operation = Value->AsObject();
		FString Id;
		const TSharedPtr<FJsonObject>* Data = nullptr;
		bool Actual = !Expected;
		if (Operation->TryGetStringField(TEXT("id"), Id)
			&& Id == OperationId
			&& Operation->TryGetObjectField(TEXT("data"), Data)
			&& Data
			&& Data->IsValid()
			&& (*Data)->TryGetBoolField(Field, Actual))
		{
			return Actual == Expected;
		}
	}
	return false;
}

bool GetOperationString(
	const TSharedPtr<FJsonObject>& Receipt,
	const FString& OperationId,
	const FString& Field,
	FString& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Receipt.IsValid() || !TryGetResultArray(Receipt, TEXT("operations"), Operations) ||
	    !Operations)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Operations)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Operation = Value->AsObject();
		FString Id;
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Operation->TryGetStringField(TEXT("id"), Id)
			&& Id == OperationId
			&& Operation->TryGetObjectField(TEXT("data"), Data)
			&& Data
			&& Data->IsValid())
		{
			return (*Data)->TryGetStringField(Field, OutValue)
				&& !OutValue.IsEmpty();
		}
	}
	return false;
}

bool CreateFixtureAsset(
	FMCPToolRegistry& Registry,
	const FString& Capability,
	const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	const FString PackagePath =
		FPackageName::GetLongPackagePath(PackageName);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	if (Capability == TEXT("blueprint.asset.create"))
	{
		Params->SetStringField(TEXT("blueprintName"), AssetName);
		Params->SetStringField(TEXT("packagePath"), PackagePath);
		Params->SetStringField(TEXT("parentClass"), TEXT("Actor"));
	}
	else
	{
		Params->SetStringField(TEXT("name"), AssetName);
		Params->SetStringField(
			Capability == TEXT("content.widget.blueprint.create")
				? TEXT("package_path")
				: TEXT("packagePath"),
			PackagePath);
	}
	return Registry.ExecuteTool(Capability, Params).bSuccess;
}

class FScopedBlueprintCompileCounter
{
public:
	explicit FScopedBlueprintCompileCounter(const FString& InAssetPath)
		: PackageName(FPackageName::ObjectPathToPackageName(InAssetPath))
	{
		if (GEditor)
		{
			Handle = GEditor->OnBlueprintPreCompile().AddLambda(
				[this](UBlueprint* Blueprint)
				{
					if (Blueprint
						&& FPackageName::ObjectPathToPackageName(
							Blueprint->GetPathName()) == PackageName)
					{
						++Count;
					}
				});
		}
	}

	~FScopedBlueprintCompileCounter()
	{
		if (GEditor && Handle.IsValid())
		{
			GEditor->OnBlueprintPreCompile().Remove(Handle);
		}
	}

	int32 GetCount() const { return Count; }

private:
	FString PackageName;
	FDelegateHandle Handle;
	int32 Count = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowActionContractTest,
	"UE_AI_integration.Workflow.ActionEnvelopeAndApproval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowActionContractTest::RunTest(const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	TestNotNull(TEXT("UE integration subsystem is initialized"), Subsystem);
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		return false;
	}

	UEAIIntegration::Workflow::FWorkflowRuntime Runtime(
		*Subsystem->GetRegistry());
	const FMCPResult Handshake = Runtime.MakeHandshake();
	TestTrue(TEXT("Workflow handshake succeeds"), Handshake.bOk);
	TestEqual(
		TEXT("Workflow handshake DSL"),
		Handshake.Data->GetStringField(TEXT("dsl")),
		FString(TEXT("ue.workflow")));
	TestEqual(
		TEXT("Workflow handshake advertises terminal reattach resume"),
		Handshake.Data->GetObjectField(TEXT("features"))
			->GetStringField(TEXT("resume")),
		FString(TEXT("sameInstanceTerminalReattach")));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("property"), TEXT("twoSided"));
	Params->SetStringField(TEXT("value"), TEXT("true"));
	const TSharedPtr<FJsonObject> Workflow = MakeWorkflow(
		TEXT("approval-contract"),
		MakeScope(
			TEXT("material"),
			UniqueAssetPath(TEXT("M_Approval"))),
		{MakeShared<FJsonValueObject>(
			MakeOperation(
				TEXT("setProperty"),
				TEXT("content.material.property.set"),
				Params))});

	const FMCPResult Plan = PlanWorkflow(Runtime, Workflow);
	FString Digest;
	TestTrue(TEXT("Workflow plan succeeds"), GetPlanDigest(Plan, Digest));
	TestEqual(TEXT("Plan defaults to standard detail"),
	          Plan.Data->GetStringField(TEXT("detailLevel")), FString(TEXT("standard")));

	TSharedPtr<FJsonObject> SummaryPlanRequest = MakeShared<FJsonObject>();
	SummaryPlanRequest->SetStringField(TEXT("action"), TEXT("plan"));
	SummaryPlanRequest->SetObjectField(TEXT("workflow"), Workflow);
	SummaryPlanRequest->SetStringField(TEXT("detailLevel"), TEXT("summary"));
	const FMCPResult SummaryPlan = Runtime.HandleRequest(SummaryPlanRequest);
	TestTrue(TEXT("Explicit summary plan succeeds"), SummaryPlan.bOk);
	TestFalse(TEXT("Summary plan omits normalized workflow"),
	          SummaryPlan.Data->HasField(TEXT("normalizedWorkflow")));
	TestFalse(TEXT("Summary plan omits operation records by default"),
	          SummaryPlan.Data->HasField(TEXT("operations")));

	TSharedPtr<FJsonObject> LegacySummaryPlanRequest = MakeShared<FJsonObject>();
	LegacySummaryPlanRequest->SetStringField(TEXT("action"), TEXT("plan"));
	LegacySummaryPlanRequest->SetObjectField(TEXT("workflow"), Workflow);
	LegacySummaryPlanRequest->SetBoolField(TEXT("details"), false);
	const FMCPResult LegacySummaryPlan = Runtime.HandleRequest(LegacySummaryPlanRequest);
	TestTrue(TEXT("details=false remains accepted"), LegacySummaryPlan.bOk);
	TestEqual(TEXT("details=false maps to summary"),
	          LegacySummaryPlan.Data->GetStringField(TEXT("detailLevel")),
	          FString(TEXT("summary")));
	TestTrue(TEXT("Deprecated details alias is reported"),
	         LegacySummaryPlan.Data->HasTypedField<EJson::Array>(TEXT("deprecations")));

	TSharedPtr<FJsonObject> ConflictingDetailRequest = MakeShared<FJsonObject>();
	ConflictingDetailRequest->SetStringField(TEXT("action"), TEXT("plan"));
	ConflictingDetailRequest->SetObjectField(TEXT("workflow"), Workflow);
	ConflictingDetailRequest->SetBoolField(TEXT("details"), false);
	ConflictingDetailRequest->SetStringField(TEXT("detailLevel"), TEXT("summary"));
	const FMCPResult ConflictingDetail = Runtime.HandleRequest(ConflictingDetailRequest);
	TestFalse(TEXT("details and detailLevel conflict is rejected"), ConflictingDetail.bOk);
	TestEqual(TEXT("Conflicting detail error code"), ConflictingDetail.Error.Code,
	          FString(TEXT("invalid_workflow_request")));
	TestEqual(TEXT("Conflicting detail HTTP status"), ConflictingDetail.Error.HttpStatus, 422);

	TSharedPtr<FJsonObject> MissingApproval = MakeShared<FJsonObject>();
	MissingApproval->SetStringField(TEXT("action"), TEXT("execute"));
	MissingApproval->SetObjectField(TEXT("workflow"), Workflow);
	const FMCPResult MissingApprovalResult =
		Runtime.HandleRequest(MissingApproval);
	TestFalse(TEXT("Execute without approval is rejected"), MissingApprovalResult.bOk);
	TestEqual(
		TEXT("Missing approval code"),
		MissingApprovalResult.Error.Code,
		FString(TEXT("plan_approval_required")));

	MissingApproval->SetStringField(
		TEXT("approvePlanDigest"),
		TEXT("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
	const FMCPResult WrongApprovalResult =
		Runtime.HandleRequest(MissingApproval);
	TestFalse(TEXT("Wrong approval digest is rejected"), WrongApprovalResult.bOk);
	TestEqual(
		TEXT("Wrong approval code"),
		WrongApprovalResult.Error.Code,
		FString(TEXT("plan_digest_mismatch")));

	TSharedPtr<FJsonObject> InvalidRun = MakeShared<FJsonObject>();
	InvalidRun->SetStringField(TEXT("action"), TEXT("status"));
	InvalidRun->SetStringField(TEXT("runId"), TEXT("../outside"));
	const FMCPResult InvalidRunResult = Runtime.HandleRequest(InvalidRun);
	TestFalse(TEXT("Non-GUID runId is rejected"), InvalidRunResult.bOk);
	TestEqual(
		TEXT("Invalid runId status"),
		InvalidRunResult.Error.HttpStatus,
		422);

	TSharedPtr<FJsonObject> UnknownResume = MakeShared<FJsonObject>();
	UnknownResume->SetStringField(TEXT("action"), TEXT("resume"));
	UnknownResume->SetStringField(
		TEXT("runId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	const FMCPResult UnknownResumeResult =
		Runtime.HandleRequest(UnknownResume);
	TestFalse(
		TEXT("Resume rejects a run not recorded in this instance"),
		UnknownResumeResult.bOk);
	TestEqual(
		TEXT("Unknown resume has a structured error"),
		UnknownResumeResult.Error.Code,
		FString(TEXT("workflow_run_not_recorded")));
	TestTrue(
		TEXT("Unknown resume includes structured details"),
		UnknownResumeResult.Error.Details.IsValid()
			&& UnknownResumeResult.Error.Details->GetStringField(
				TEXT("resumeMode")) == TEXT("rejectedUnknownRun")
			&& !UnknownResumeResult.Error.Details->GetBoolField(
				TEXT("reattached"))
			&& !UnknownResumeResult.Error.Details->GetBoolField(
				TEXT("resumedExecution")));

	TSharedPtr<FJsonObject> UnsupportedAction = MakeShared<FJsonObject>();
	UnsupportedAction->SetStringField(TEXT("action"), TEXT("debug"));
	const FMCPResult UnsupportedActionResult =
		Runtime.HandleRequest(UnsupportedAction);
	TestFalse(
		TEXT("Unsupported action is rejected"),
		UnsupportedActionResult.bOk);
	TestTrue(
		TEXT("Unsupported action error is reported before runId validation"),
		UnsupportedActionResult.Error.Message.Contains(
			TEXT("Unsupported workflow action")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowAssetEditE2ETest,
	"UE_AI_integration.Workflow.BlueprintWidgetMaterialE2E",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowAssetEditE2ETest::RunTest(const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	TestNotNull(TEXT("UE integration subsystem is initialized"), Subsystem);
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		return false;
	}

	FString BlueprintPath;
	FString WidgetPath;
	FString MaterialPath;
	{
		UEAIIntegration::Workflow::FWorkflowRuntime Runtime(
			*Subsystem->GetRegistry());

	// Asset creation is test setup because UE 5.3 CreateBlueprint performs its
	// own bootstrap compile. The measured workflow starts from a loaded scope
	// and verifies that all edit steps defer to exactly one final compile.
	BlueprintPath = UniqueAssetPath(TEXT("BP_Workflow"));
	TestTrue(
		TEXT("Blueprint fixture is created"),
		CreateFixtureAsset(
			*Subsystem->GetRegistry(),
			TEXT("blueprint.asset.create"),
			BlueprintPath));
	TSharedPtr<FJsonObject> VariableParams = MakeShared<FJsonObject>();
	VariableParams->SetStringField(TEXT("variableName"), TEXT("WorkflowValue"));
	VariableParams->SetStringField(TEXT("variableType"), TEXT("Float"));
	TSharedPtr<FJsonObject> GraphParams = MakeShared<FJsonObject>();
	GraphParams->SetStringField(TEXT("graphName"), TEXT("ComputeWorkflowValue"));
	GraphParams->SetStringField(TEXT("graphType"), TEXT("function"));
	TSharedPtr<FJsonObject> SecondGraphParams = MakeShared<FJsonObject>();
	SecondGraphParams->SetStringField(
		TEXT("graphName"),
		TEXT("ComputeOtherValue"));
	SecondGraphParams->SetStringField(TEXT("graphType"), TEXT("function"));
	const TSharedPtr<FJsonObject> BlueprintWorkflow = MakeWorkflow(
		TEXT("blueprint-e2e"),
		MakeScope(TEXT("blueprint"), BlueprintPath, false),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("addVariable"),
					TEXT("blueprint.variable.add"),
					VariableParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("addGraph"),
					TEXT("blueprint.graph.create"),
					GraphParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("addSecondGraph"),
					TEXT("blueprint.graph.create"),
					SecondGraphParams)),
		});
	BlueprintWorkflow->GetObjectField(TEXT("verify"))->SetArrayField(
		TEXT("readBack"),
		{MakeShared<FJsonValueString>(TEXT("graphs"))});
	const FMCPResult BlueprintPlan =
		PlanWorkflow(Runtime, BlueprintWorkflow);
	FString BlueprintDigest;
	TestTrue(
		TEXT("Blueprint workflow plans"),
		GetPlanDigest(BlueprintPlan, BlueprintDigest));
	TestEqual(
		TEXT("Blueprint plan has one compile finalizer"),
		CountFinalizersByKind(BlueprintPlan.Data, TEXT("compile")),
		1);
	FMCPResult BlueprintResult;
	int32 BlueprintCompileCount = 0;
	{
		FScopedBlueprintCompileCounter CompileCounter(BlueprintPath);
		BlueprintResult =
			ExecuteWorkflow(Runtime, BlueprintWorkflow, BlueprintDigest);
		BlueprintCompileCount = CompileCounter.GetCount();
	}
	TestTrue(TEXT("Blueprint workflow executes"), BlueprintResult.bOk);
	TestEqual(
		TEXT("Blueprint workflow compiles exactly once"),
		BlueprintCompileCount,
		1);
	if (BlueprintResult.bOk)
	{
		TestEqual(
			TEXT("Blueprint receipt is completed"),
			BlueprintResult.Data->GetStringField(TEXT("status")),
			FString(TEXT("completed")));
		TestTrue(
			TEXT("Blueprint steps do not save individually"),
			AllSucceededStepsReportDeferredAndUnsaved(BlueprintResult.Data));
		TestTrue(TEXT("Blueprint finalizers report completed compile/read-back"),
			     AllExecutableFinalizersReportSucceededWithoutOutput(BlueprintResult.Data));
		TestTrue(
			TEXT("Blueprint receipt reports dirty package"),
			HasDirtyPackages(BlueprintResult.Data));
		const TSharedPtr<FJsonObject>* AssetDiff = nullptr;
		TestTrue(TEXT("Blueprint receipt contains readable structural diff"),
			     TryGetResultObject(BlueprintResult.Data, TEXT("assetDiff"), AssetDiff) &&
			         AssetDiff &&
			         (*AssetDiff)->GetStringField(TEXT("snapshotKind")) ==
			             TEXT("structuralVerification") &&
			         (*AssetDiff)->HasTypedField<EJson::Array>(TEXT("added")) &&
			         (*AssetDiff)->HasTypedField<EJson::Array>(TEXT("changedFields")));
		TestTrue(
			TEXT("Blueprint remains dirty"),
			UEditorAssetLibrary::LoadAsset(BlueprintPath)
				&& UEditorAssetLibrary::LoadAsset(BlueprintPath)
					->GetOutermost()->IsDirty());
		const TSharedPtr<FJsonObject>* BlueprintReadBack = nullptr;
		const TSharedPtr<FJsonObject>* GraphReadBack = nullptr;
		TestTrue(TEXT("Blueprint receipt exposes read-back"),
			     TryGetResultObject(BlueprintResult.Data, TEXT("readBack"), BlueprintReadBack) &&
			         BlueprintReadBack &&
			         (*BlueprintReadBack)->TryGetObjectField(TEXT("graphs"), GraphReadBack) &&
			         GraphReadBack &&
			         (*GraphReadBack)->HasTypedField<EJson::Object>(TEXT("ComputeWorkflowValue")) &&
			         (*GraphReadBack)->HasTypedField<EJson::Object>(TEXT("ComputeOtherValue")));

		const FString BlueprintRunId =
			BlueprintResult.Data->GetStringField(TEXT("runId"));
		TSharedPtr<FJsonObject> CompactStatusRequest = MakeShared<FJsonObject>();
		CompactStatusRequest->SetStringField(TEXT("action"), TEXT("status"));
		CompactStatusRequest->SetStringField(TEXT("runId"), BlueprintRunId);
		const FMCPResult CompactStatus = Runtime.HandleRequest(CompactStatusRequest);
		TestTrue(TEXT("Default status projection succeeds"), CompactStatus.bOk);
		if (CompactStatus.bOk)
		{
			TestEqual(TEXT("Status defaults to summary detail"),
				      CompactStatus.Data->GetStringField(TEXT("detailLevel")),
				      FString(TEXT("summary")));
			TestFalse(TEXT("Summary status omits full sections"),
				      CompactStatus.Data->HasField(TEXT("sections")));
			TestFalse(TEXT("Summary status does not duplicate operations"),
				      CompactStatus.Data->HasField(TEXT("operations")));
			const TSharedPtr<FJsonObject>* CompactReceipt = nullptr;
			TestTrue(TEXT("Summary carries compact receipt"),
				     CompactStatus.Data->TryGetObjectField(TEXT("receipt"), CompactReceipt) &&
				         CompactReceipt && !(*CompactReceipt)->HasField(TEXT("operations")) &&
				         !(*CompactReceipt)->HasField(TEXT("readBack")) &&
				         !(*CompactReceipt)->HasField(TEXT("assetDiff")));
			const int32 CompactBytes =
				FTCHARToUTF8(*SerializeJsonObject(CompactStatus.Data)).Length();
			TestTrue(TEXT("Default workflow status stays below 8 KiB"), CompactBytes <= 8 * 1024);
		}

		TSharedPtr<FJsonObject> DiffOnlyStatusRequest = MakeShared<FJsonObject>();
		DiffOnlyStatusRequest->SetStringField(TEXT("action"), TEXT("status"));
		DiffOnlyStatusRequest->SetStringField(TEXT("runId"), BlueprintRunId);
		DiffOnlyStatusRequest->SetStringField(TEXT("detailLevel"), TEXT("summary"));
		DiffOnlyStatusRequest->SetArrayField(TEXT("sections"),
			                                 {MakeShared<FJsonValueString>(TEXT("assetDiff"))});
		const FMCPResult DiffOnlyStatus = Runtime.HandleRequest(DiffOnlyStatusRequest);
		const TSharedPtr<FJsonObject>* DiffOnlySections = nullptr;
		TestTrue(TEXT("Explicit assetDiff section is available from summary"),
			     DiffOnlyStatus.bOk &&
			         DiffOnlyStatus.Data->TryGetObjectField(TEXT("sections"), DiffOnlySections) &&
			         DiffOnlySections && (*DiffOnlySections)->Values.Num() == 1 &&
			         (*DiffOnlySections)->HasTypedField<EJson::Object>(TEXT("assetDiff")));
		TSharedPtr<FJsonObject> ResumeRequest = MakeShared<FJsonObject>();
		ResumeRequest->SetStringField(TEXT("action"), TEXT("resume"));
		ResumeRequest->SetStringField(TEXT("runId"), BlueprintRunId);
		const FMCPResult ResumeResult =
			Runtime.HandleRequest(ResumeRequest);
		TestTrue(
			TEXT("Completed run supports same-instance terminal reattach"),
			ResumeResult.bOk);
		if (ResumeResult.bOk)
		{
			TestEqual(
				TEXT("Resume reports terminal reattach mode"),
				ResumeResult.Data->GetStringField(TEXT("resumeMode")),
				FString(TEXT("terminalReattach")));
			TestTrue(
				TEXT("Resume reports receipt reattached"),
				ResumeResult.Data->GetBoolField(TEXT("reattached")));
			TestFalse(
				TEXT("Resume never claims execution continued"),
				ResumeResult.Data->GetBoolField(TEXT("resumedExecution")));
			TestEqual(
				TEXT("Resume preserves terminal status"),
				ResumeResult.Data->GetStringField(TEXT("status")),
				FString(TEXT("completed")));
		}

		TSharedPtr<FJsonObject> ModifiedResume =
			MakeShared<FJsonObject>();
		ModifiedResume->SetStringField(TEXT("action"), TEXT("resume"));
		ModifiedResume->SetStringField(TEXT("runId"), BlueprintRunId);
		ModifiedResume->SetObjectField(
			TEXT("workflow"),
			BlueprintWorkflow);
		const FMCPResult ModifiedResumeResult =
			Runtime.HandleRequest(ModifiedResume);
		TestFalse(
			TEXT("Resume rejects a modified workflow payload"),
			ModifiedResumeResult.bOk);
		TestEqual(
			TEXT("Modified resume fails envelope validation"),
			ModifiedResumeResult.Error.Code,
			FString(TEXT("invalid_params")));

		UEAIIntegration::Workflow::FWorkflowRuntime JournalReader(
			*Subsystem->GetRegistry());
		TSharedPtr<FJsonObject> StatusRequest = MakeShared<FJsonObject>();
		StatusRequest->SetStringField(TEXT("action"), TEXT("status"));
		StatusRequest->SetStringField(TEXT("runId"), BlueprintRunId);
		const FMCPResult SerializedReceipt =
			JournalReader.HandleRequest(StatusRequest);
		TestTrue(
			TEXT("Receipt round-trips through Saved/UEWorkflow"),
			SerializedReceipt.bOk);
		if (SerializedReceipt.bOk)
		{
			TestEqual(
				TEXT("Serialized receipt schema"),
				SerializedReceipt.Data->GetStringField(TEXT("schema")),
				FString(TEXT("ue.workflow-result.v1")));
			const TSharedPtr<FJsonObject>* Receipt = nullptr;
			TestTrue(
				TEXT("Status result carries a journal receipt"),
				SerializedReceipt.Data->TryGetObjectField(
					TEXT("receipt"),
					Receipt)
					&& Receipt
					&& (*Receipt)->GetStringField(TEXT("schema"))
						== TEXT("ue.workflow-run.v1"));
			TestEqual(
				TEXT("Serialized receipt status"),
				SerializedReceipt.Data->GetStringField(TEXT("status")),
				FString(TEXT("completed")));
			const TSharedPtr<FJsonObject>* SerializedSummary = nullptr;
			TestTrue(
				TEXT("Serialized receipt contains rollback summary"),
				SerializedReceipt.Data->TryGetObjectField(TEXT("summary"), SerializedSummary) &&
				    SerializedSummary &&
				    (*SerializedSummary)->HasTypedField<EJson::Object>(TEXT("rollback")));
		}

		TSharedPtr<FJsonObject> CrossInstanceResume =
			MakeShared<FJsonObject>();
		CrossInstanceResume->SetStringField(TEXT("action"), TEXT("resume"));
		CrossInstanceResume->SetStringField(
			TEXT("runId"),
			BlueprintRunId);
		const FMCPResult CrossInstanceResumeResult =
			JournalReader.HandleRequest(CrossInstanceResume);
		TestFalse(
			TEXT("Journal-only run cannot resume in another instance"),
			CrossInstanceResumeResult.bOk);
		TestEqual(
			TEXT("Cross-instance resume has a structured error"),
			CrossInstanceResumeResult.Error.Code,
			FString(TEXT("workflow_instance_changed")));
		TestTrue(
			TEXT("Cross-instance resume includes structured details"),
			CrossInstanceResumeResult.Error.Details.IsValid());
		if (CrossInstanceResumeResult.Error.Details.IsValid())
		{
			TestEqual(
				TEXT("Cross-instance resume reports rejected mode"),
				CrossInstanceResumeResult.Error.Details->GetStringField(
					TEXT("resumeMode")),
				FString(TEXT("rejectedDifferentInstance")));
			TestFalse(
				TEXT("Cross-instance resume does not claim reattachment"),
				CrossInstanceResumeResult.Error.Details->GetBoolField(
					TEXT("reattached")));
			TestFalse(
				TEXT("Cross-instance resume does not claim execution continued"),
				CrossInstanceResumeResult.Error.Details->GetBoolField(
					TEXT("resumedExecution")));
		}
	}

	// Widget: create -> child -> typed binding -> layout -> read-back.
	WidgetPath = UniqueAssetPath(TEXT("WBP_Workflow"));
	TestTrue(
		TEXT("Widget fixture is created"),
		CreateFixtureAsset(
			*Subsystem->GetRegistry(),
			TEXT("content.widget.blueprint.create"),
			WidgetPath));
	TSharedPtr<FJsonObject> ChildParams = MakeShared<FJsonObject>();
	ChildParams->SetStringField(TEXT("parent"), TEXT("RootCanvas"));
	ChildParams->SetStringField(TEXT("class"), TEXT("TextBlock"));
	ChildParams->SetStringField(TEXT("name"), TEXT("Title"));
	TSharedPtr<FJsonObject> LayoutParams = MakeShared<FJsonObject>();
	LayoutParams->SetArrayField(
		TEXT("anchors"),
		{
			MakeShared<FJsonValueNumber>(0.5),
			MakeShared<FJsonValueNumber>(0.0),
			MakeShared<FJsonValueNumber>(0.5),
			MakeShared<FJsonValueNumber>(0.0),
		});
	LayoutParams->SetArrayField(
		TEXT("alignment"),
		{
			MakeShared<FJsonValueNumber>(0.5),
			MakeShared<FJsonValueNumber>(0.0),
		});
	LayoutParams->SetArrayField(
		TEXT("offsets"),
		{
			MakeShared<FJsonValueNumber>(-200.0),
			MakeShared<FJsonValueNumber>(40.0),
			MakeShared<FJsonValueNumber>(400.0),
			MakeShared<FJsonValueNumber>(64.0),
		});
	TSharedPtr<FJsonObject> LayoutOperation = MakeOperation(
		TEXT("layoutTitle"),
		TEXT("content.widget.slot.layout.set"),
		LayoutParams);
	TSharedPtr<FJsonObject> Binding = MakeShared<FJsonObject>();
	Binding->SetStringField(TEXT("from"), TEXT("addTitle"));
	Binding->SetStringField(TEXT("path"), TEXT("/widgetRef"));
	TSharedPtr<FJsonObject> Bindings = MakeShared<FJsonObject>();
	Bindings->SetObjectField(TEXT("/target"), Binding);
	LayoutOperation->SetObjectField(TEXT("bindings"), Bindings);
	TSharedPtr<FJsonObject> PropertyParams = MakeShared<FJsonObject>();
	PropertyParams->SetStringField(TEXT("property"), TEXT("Text"));
	PropertyParams->SetStringField(TEXT("value"), TEXT("Workflow title"));
	TSharedPtr<FJsonObject> PropertyOperation = MakeOperation(
		TEXT("setTitleText"),
		TEXT("content.widget.property.set"),
		PropertyParams);
	TSharedPtr<FJsonObject> PropertyBinding = MakeShared<FJsonObject>();
	PropertyBinding->SetStringField(TEXT("from"), TEXT("addTitle"));
	PropertyBinding->SetStringField(TEXT("path"), TEXT("/widgetRef/name"));
	TSharedPtr<FJsonObject> PropertyBindings = MakeShared<FJsonObject>();
	PropertyBindings->SetObjectField(
		TEXT("/widget_name"),
		PropertyBinding);
	PropertyOperation->SetObjectField(TEXT("bindings"), PropertyBindings);
	const TSharedPtr<FJsonObject> WidgetWorkflow = MakeWorkflow(
		TEXT("widget-e2e"),
		MakeScope(TEXT("widgetBlueprint"), WidgetPath, false),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("addTitle"),
					TEXT("content.widget.child.add"),
					ChildParams)),
			MakeShared<FJsonValueObject>(LayoutOperation),
			MakeShared<FJsonValueObject>(PropertyOperation),
		});
	const FMCPResult WidgetPlan = PlanWorkflow(Runtime, WidgetWorkflow);
	FString WidgetDigest;
	TestTrue(
		TEXT("Widget workflow plans"),
		GetPlanDigest(WidgetPlan, WidgetDigest));
	TestEqual(
		TEXT("Widget plan has one compile finalizer"),
		CountFinalizersByKind(WidgetPlan.Data, TEXT("compile")),
		1);
	FMCPResult WidgetResult;
	int32 WidgetCompileCount = 0;
	{
		FScopedBlueprintCompileCounter CompileCounter(WidgetPath);
		WidgetResult =
			ExecuteWorkflow(Runtime, WidgetWorkflow, WidgetDigest);
		WidgetCompileCount = CompileCounter.GetCount();
	}
	TestTrue(TEXT("Widget workflow executes"), WidgetResult.bOk);
	TestEqual(
		TEXT("Widget workflow compiles exactly once"),
		WidgetCompileCount,
		1);
	if (WidgetResult.bOk)
	{
		TestEqual(
			TEXT("Widget receipt is completed"),
			WidgetResult.Data->GetStringField(TEXT("status")),
			FString(TEXT("completed")));
		TestTrue(
			TEXT("Widget steps do not save individually"),
			AllSucceededStepsReportDeferredAndUnsaved(WidgetResult.Data));
		TestTrue(TEXT("Widget finalizers report completed compile/read-back"),
			     AllExecutableFinalizersReportSucceededWithoutOutput(WidgetResult.Data));
		TestTrue(
			TEXT("Widget receipt reports dirty package"),
			HasDirtyPackages(WidgetResult.Data));
		const TSharedPtr<FJsonObject>* WidgetReadBack = nullptr;
		TestTrue(TEXT("Widget receipt contains one projected readBack section"),
			     TryGetResultObject(WidgetResult.Data, TEXT("readBack"), WidgetReadBack) &&
			         WidgetReadBack &&
			         (*WidgetReadBack)->HasTypedField<EJson::Object>(TEXT("widgetTree")) &&
			         (*WidgetReadBack)->HasTypedField<EJson::Array>(TEXT("bindings")) &&
			         (*WidgetReadBack)->HasTypedField<EJson::Array>(TEXT("layout")));
	}

	// Material: create -> property edits -> one material compile finalizer.
	MaterialPath = UniqueAssetPath(TEXT("M_Workflow"));
	TestTrue(
		TEXT("Material fixture is created"),
		CreateFixtureAsset(
			*Subsystem->GetRegistry(),
			TEXT("content.material.create"),
			MaterialPath));
	TSharedPtr<FJsonObject> TwoSidedParams = MakeShared<FJsonObject>();
	TwoSidedParams->SetStringField(TEXT("property"), TEXT("twoSided"));
	TwoSidedParams->SetStringField(TEXT("value"), TEXT("true"));
	TSharedPtr<FJsonObject> BlendParams = MakeShared<FJsonObject>();
	BlendParams->SetStringField(TEXT("property"), TEXT("blendMode"));
	BlendParams->SetStringField(TEXT("value"), TEXT("Masked"));
	TSharedPtr<FJsonObject> ConstantParams = MakeShared<FJsonObject>();
	ConstantParams->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
	ConstantParams->SetNumberField(TEXT("posX"), -300);
	ConstantParams->SetNumberField(TEXT("posY"), 0);
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("expressionClass"), TEXT("Add"));
	AddParams->SetNumberField(TEXT("posX"), 0);
	AddParams->SetNumberField(TEXT("posY"), 0);
	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("sourcePinName"), TEXT("Output"));
	ConnectParams->SetStringField(TEXT("targetPinName"), TEXT("A"));
	TSharedPtr<FJsonObject> ConnectOperation = MakeOperation(
		TEXT("connectNodes"),
		TEXT("content.material.pin.connect"),
		ConnectParams);
	TSharedPtr<FJsonObject> SourceNodeBinding = MakeShared<FJsonObject>();
	SourceNodeBinding->SetStringField(TEXT("from"), TEXT("constantNode"));
	SourceNodeBinding->SetStringField(TEXT("path"), TEXT("/nodeId"));
	TSharedPtr<FJsonObject> TargetNodeBinding = MakeShared<FJsonObject>();
	TargetNodeBinding->SetStringField(TEXT("from"), TEXT("addNode"));
	TargetNodeBinding->SetStringField(TEXT("path"), TEXT("/nodeId"));
	TSharedPtr<FJsonObject> ConnectBindings = MakeShared<FJsonObject>();
	ConnectBindings->SetObjectField(
		TEXT("/sourceNodeId"),
		SourceNodeBinding);
	ConnectBindings->SetObjectField(
		TEXT("/targetNodeId"),
		TargetNodeBinding);
	ConnectOperation->SetObjectField(TEXT("bindings"), ConnectBindings);
	const TSharedPtr<FJsonObject> MaterialWorkflow = MakeWorkflow(
		TEXT("material-e2e"),
		MakeScope(TEXT("material"), MaterialPath, false),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("twoSided"),
					TEXT("content.material.property.set"),
					TwoSidedParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("masked"),
					TEXT("content.material.property.set"),
					BlendParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("constantNode"),
					TEXT("content.material.expression.add"),
					ConstantParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("addNode"),
					TEXT("content.material.expression.add"),
					AddParams)),
			MakeShared<FJsonValueObject>(ConnectOperation),
		});
	const FMCPResult MaterialPlan = PlanWorkflow(Runtime, MaterialWorkflow);
	FString MaterialDigest;
	TestTrue(
		TEXT("Material workflow plans"),
		GetPlanDigest(MaterialPlan, MaterialDigest));
	TestEqual(
		TEXT("Material plan has one compile finalizer"),
		CountFinalizersByKind(MaterialPlan.Data, TEXT("compile")),
		1);
	UEAIIntegration::Workflow::ResetMaterialCompileFinalizerCountForTests();
	const FMCPResult MaterialResult =
		ExecuteWorkflow(Runtime, MaterialWorkflow, MaterialDigest);
	TestTrue(TEXT("Material workflow executes"), MaterialResult.bOk);
	TestEqual(
		TEXT("Material workflow compiles exactly once"),
		UEAIIntegration::Workflow::GetMaterialCompileFinalizerCountForTests(),
		1);
	if (MaterialResult.bOk)
	{
		TestEqual(
			TEXT("Receipt schema"),
			MaterialResult.Data->GetStringField(TEXT("schema")),
			FString(TEXT("ue.workflow-result.v1")));
		const TSharedPtr<FJsonObject>* MaterialReceipt = nullptr;
		TestTrue(
			TEXT("Execution result carries receipt contract"),
			MaterialResult.Data->TryGetObjectField(
				TEXT("receipt"),
				MaterialReceipt)
				&& MaterialReceipt
				&& (*MaterialReceipt)->GetStringField(TEXT("schema"))
					== TEXT("ue.workflow-run.v1"));
		TestEqual(
			TEXT("Receipt contract digest"),
			MaterialResult.Data->GetStringField(TEXT("contractSetDigest")),
			MaterialPlan.Data->GetStringField(TEXT("contractSetDigest")));
		TestTrue(
			TEXT("Material steps do not save individually"),
			AllSucceededStepsReportDeferredAndUnsaved(MaterialResult.Data));
		TestTrue(TEXT("Material finalizers report completed compile/read-back"),
			     AllExecutableFinalizersReportSucceededWithoutOutput(MaterialResult.Data));
		TestTrue(
			TEXT("Material receipt reports dirty package"),
			HasDirtyPackages(MaterialResult.Data));
		TestTrue(
			TEXT("Two incrementally-added material nodes connect"),
			OperationReportsBool(
				MaterialResult.Data,
				TEXT("connectNodes"),
				TEXT("connected"),
				true));

		TSharedPtr<FJsonObject> RollbackRequest = MakeShared<FJsonObject>();
		RollbackRequest->SetStringField(TEXT("action"), TEXT("rollback"));
		RollbackRequest->SetStringField(
			TEXT("runId"),
			MaterialResult.Data->GetStringField(TEXT("runId")));
		RollbackRequest->SetStringField(
			TEXT("approvePlanDigest"),
			MaterialDigest);
		const FMCPResult RollbackResult =
			Runtime.HandleRequest(RollbackRequest);
		TestTrue(TEXT("Material workflow rollback succeeds"), RollbackResult.bOk);
		if (RollbackResult.bOk)
		{
			TestTrue(
				TEXT("Rollback is structurally verified"),
				RollbackResult.Data->GetBoolField(TEXT("rollbackVerified")));
			TestTrue(
				TEXT("Rollback keeps the pre-existing material scope"),
				UEditorAssetLibrary::DoesAssetExist(MaterialPath));
			UMaterial* RolledBackMaterial = Cast<UMaterial>(
				UEditorAssetLibrary::LoadAsset(MaterialPath));
			TestNotNull(
				TEXT("Rolled-back material reloads"),
				RolledBackMaterial);
			if (RolledBackMaterial)
			{
				TestEqual(
					TEXT("Rollback removes workflow-added expressions"),
					RolledBackMaterial->GetExpressions().Num(),
					0);
			}
		}
	}

	}
	// The runtime retains strong object snapshots for explicit rollback. Destroy
	// it before deleting the fixtures so the safe asset-delete model can verify
	// that their packages have no remaining native references.
	TestTrue(TEXT("Blueprint fixture is removed"), CleanupAsset(BlueprintPath));
	TestTrue(TEXT("Widget fixture is removed"), CleanupAsset(WidgetPath));
	TestTrue(TEXT("Material fixture is removed"), CleanupAsset(MaterialPath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowFailureRollbackTest,
	"UE_AI_integration.Workflow.FailureRollbackLeavesNoNewAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowFailureRollbackTest::RunTest(const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		AddError(TEXT("UE integration subsystem is not initialized."));
		return false;
	}

	UEAIIntegration::Workflow::FWorkflowRuntime Runtime(
		*Subsystem->GetRegistry());
	const FString AssetPath = UniqueAssetPath(TEXT("BP_Rollback"));
	TSharedPtr<FJsonObject> FirstGraph = MakeShared<FJsonObject>();
	FirstGraph->SetStringField(TEXT("graphName"), TEXT("DuplicateName"));
	FirstGraph->SetStringField(TEXT("graphType"), TEXT("function"));
	TSharedPtr<FJsonObject> SecondGraph = MakeShared<FJsonObject>();
	SecondGraph->SetStringField(TEXT("graphName"), TEXT("DuplicateName"));
	SecondGraph->SetStringField(TEXT("graphType"), TEXT("function"));
	const TSharedPtr<FJsonObject> Workflow = MakeWorkflow(
		TEXT("failure-rollback"),
		MakeScope(TEXT("blueprint"), AssetPath),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("firstGraph"),
					TEXT("blueprint.graph.create"),
					FirstGraph)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("duplicateGraph"),
					TEXT("blueprint.graph.create"),
					SecondGraph)),
		});
	const FMCPResult Plan = PlanWorkflow(Runtime, Workflow);
	FString Digest;
	if (!GetPlanDigest(Plan, Digest))
	{
		AddError(TEXT("Rollback fixture did not plan."));
		return false;
	}

	const FMCPResult Result = ExecuteWorkflow(Runtime, Workflow, Digest);
	TestFalse(TEXT("Duplicate graph fails execution"), Result.bOk);
	TestEqual(
		TEXT("Workflow failure code"),
		Result.Error.Code,
		FString(TEXT("workflow_execution_failed")));
	if (Result.Error.Details.IsValid())
	{
		TestTrue(
			TEXT("Failure rollback is verified"),
			Result.Error.Details->GetBoolField(TEXT("rollbackVerified")));
		TSharedPtr<FJsonObject> ResumeRequest =
			MakeShared<FJsonObject>();
		ResumeRequest->SetStringField(TEXT("action"), TEXT("resume"));
		ResumeRequest->SetStringField(
			TEXT("runId"),
			Result.Error.Details->GetStringField(TEXT("runId")));
		const FMCPResult ResumeResult =
			Runtime.HandleRequest(ResumeRequest);
		TestTrue(
			TEXT("Failed terminal run supports idempotent reattach"),
			ResumeResult.bOk);
		if (ResumeResult.bOk)
		{
			TestEqual(
				TEXT("Failed resume preserves failed status"),
				ResumeResult.Data->GetStringField(TEXT("status")),
				FString(TEXT("failed")));
			TestEqual(
				TEXT("Failed resume reports terminal reattach"),
				ResumeResult.Data->GetStringField(TEXT("resumeMode")),
				FString(TEXT("terminalReattach")));
			TestFalse(
				TEXT("Failed resume does not continue execution"),
				ResumeResult.Data->GetBoolField(TEXT("resumedExecution")));
		}
	}
	TestFalse(
		TEXT("Failed createIfMissing workflow leaves no asset"),
		AssetExistsWithoutLoading(AssetPath));
	CleanupAsset(AssetPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowInternalContextTest,
	"UE_AI_integration.Workflow.InternalExecutionContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowInternalContextTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::Workflow;
	const TSharedPtr<FJsonObject> Legacy = MakeShared<FJsonObject>();
	TestFalse(TEXT("Legacy handler saves immediately=false is not set"), ShouldDeferCompile(Legacy));
	TestTrue(TEXT("Legacy handler keeps immediate save"), ShouldSaveImmediately(Legacy));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
	Context->SetBoolField(TEXT("deferCompile"), true);
	Context->SetBoolField(TEXT("saveOnSuccess"), false);
	Params->SetObjectField(TEXT("__ueWorkflow"), Context);
	TestTrue(TEXT("Workflow defers compile"), ShouldDeferCompile(Params));
	TestFalse(TEXT("Workflow suppresses step save"), ShouldSaveImmediately(Params));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowExistingMaterialFailureRollbackTest,
	"UE_AI_integration.Workflow.ExistingMaterialFailureRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowExistingMaterialFailureRollbackTest::RunTest(
	const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		AddError(TEXT("UE integration subsystem is not initialized."));
		return false;
	}

	const FString MaterialPath =
		UniqueAssetPath(TEXT("M_ExistingRollback"));
	CleanupAsset(MaterialPath);
	if (!CreateFixtureAsset(
		*Subsystem->GetRegistry(),
		TEXT("content.material.create"),
		MaterialPath))
	{
		AddError(TEXT("Failed to create existing-material fixture."));
		return false;
	}

	UEAIIntegration::Workflow::FWorkflowRuntime Runtime(
		*Subsystem->GetRegistry());
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
	AddParams->SetNumberField(TEXT("posX"), -100);
	AddParams->SetNumberField(TEXT("posY"), 20);
	const TSharedPtr<FJsonObject> SeedWorkflow = MakeWorkflow(
		TEXT("seed-existing-material"),
		MakeScope(TEXT("material"), MaterialPath, false),
		{MakeShared<FJsonValueObject>(
			MakeOperation(
				TEXT("seedNode"),
				TEXT("content.material.expression.add"),
				AddParams))});
	const FMCPResult SeedPlan = PlanWorkflow(Runtime, SeedWorkflow);
	FString SeedDigest;
	if (!GetPlanDigest(SeedPlan, SeedDigest))
	{
		AddError(TEXT("Seed material workflow did not plan."));
		CleanupAsset(MaterialPath);
		return false;
	}
	const FMCPResult SeedResult =
		ExecuteWorkflow(Runtime, SeedWorkflow, SeedDigest, true);
	FString SeedNodeId;
	if (!SeedResult.bOk
		|| !GetOperationString(
			SeedResult.Data,
			TEXT("seedNode"),
			TEXT("nodeId"),
			SeedNodeId))
	{
		AddError(TEXT("Seed material workflow did not return nodeId."));
		CleanupAsset(MaterialPath);
		return false;
	}

	TSharedPtr<FJsonObject> MoveParams = MakeShared<FJsonObject>();
	MoveParams->SetStringField(TEXT("nodeId"), SeedNodeId);
	MoveParams->SetNumberField(TEXT("posX"), 700);
	MoveParams->SetNumberField(TEXT("posY"), 300);
	TSharedPtr<FJsonObject> MissingMoveParams = MakeShared<FJsonObject>();
	MissingMoveParams->SetStringField(
		TEXT("nodeId"),
		FGuid::NewGuid().ToString());
	MissingMoveParams->SetNumberField(TEXT("posX"), 1);
	MissingMoveParams->SetNumberField(TEXT("posY"), 1);
	const TSharedPtr<FJsonObject> FailingWorkflow = MakeWorkflow(
		TEXT("rollback-existing-material"),
		MakeScope(TEXT("material"), MaterialPath, false),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("moveExisting"),
					TEXT("content.material.expression.move"),
					MoveParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("moveMissing"),
					TEXT("content.material.expression.move"),
					MissingMoveParams)),
		});
	const FMCPResult FailingPlan =
		PlanWorkflow(Runtime, FailingWorkflow);
	FString FailingDigest;
	if (!GetPlanDigest(FailingPlan, FailingDigest))
	{
		AddError(TEXT("Failing material workflow did not plan."));
		CleanupAsset(MaterialPath);
		return false;
	}

	const FMCPResult Failure =
		ExecuteWorkflow(Runtime, FailingWorkflow, FailingDigest);
	TestFalse(TEXT("Second material operation fails"), Failure.bOk);
	TestTrue(
		TEXT("Existing material rollback is structurally verified"),
		Failure.Error.Details.IsValid()
			&& Failure.Error.Details->GetBoolField(
				TEXT("rollbackVerified")));
	UMaterial* Material = Cast<UMaterial>(
		UEditorAssetLibrary::LoadAsset(MaterialPath));
	TestNotNull(TEXT("Existing material remains"), Material);
	if (Material && Material->GetExpressions().Num() == 1)
	{
		TestEqual(
			TEXT("Failed workflow restores expression X"),
			Material->GetExpressions()[0]->MaterialExpressionEditorX,
			-100);
		TestEqual(
			TEXT("Failed workflow restores expression Y"),
			Material->GetExpressions()[0]->MaterialExpressionEditorY,
			20);
	}
	else
	{
		AddError(TEXT("Existing material expression count changed after rollback."));
	}

	CleanupAsset(MaterialPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowMaterialConnectionFailureRollbackTest,
	"UE_AI_integration.Workflow.MaterialConnectionFailureRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowMaterialConnectionFailureRollbackTest::RunTest(
	const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		AddError(TEXT("UE integration subsystem is not initialized."));
		return false;
	}

	const FString MaterialPath =
		UniqueAssetPath(TEXT("M_ConnectionRollback"));
	CleanupAsset(MaterialPath);
	if (!CreateFixtureAsset(
		*Subsystem->GetRegistry(),
		TEXT("content.material.create"),
		MaterialPath))
	{
		AddError(TEXT("Failed to create connection rollback fixture."));
		return false;
	}

	TSharedPtr<FJsonObject> FirstParams = MakeShared<FJsonObject>();
	FirstParams->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
	FirstParams->SetNumberField(TEXT("posX"), -300);
	FirstParams->SetNumberField(TEXT("posY"), -100);
	TSharedPtr<FJsonObject> SecondParams = MakeShared<FJsonObject>();
	SecondParams->SetStringField(TEXT("expressionClass"), TEXT("Constant"));
	SecondParams->SetNumberField(TEXT("posX"), -300);
	SecondParams->SetNumberField(TEXT("posY"), 100);
	TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
	ConnectParams->SetStringField(TEXT("sourcePinName"), TEXT("Output"));
	ConnectParams->SetStringField(TEXT("targetPinName"), TEXT("Output"));
	TSharedPtr<FJsonObject> Connect = MakeOperation(
		TEXT("invalidConnect"),
		TEXT("content.material.pin.connect"),
		ConnectParams);
	TSharedPtr<FJsonObject> SourceBinding = MakeShared<FJsonObject>();
	SourceBinding->SetStringField(TEXT("from"), TEXT("firstConstant"));
	SourceBinding->SetStringField(TEXT("path"), TEXT("/nodeId"));
	TSharedPtr<FJsonObject> TargetBinding = MakeShared<FJsonObject>();
	TargetBinding->SetStringField(TEXT("from"), TEXT("secondConstant"));
	TargetBinding->SetStringField(TEXT("path"), TEXT("/nodeId"));
	TSharedPtr<FJsonObject> ConnectBindings = MakeShared<FJsonObject>();
	ConnectBindings->SetObjectField(
		TEXT("/sourceNodeId"),
		SourceBinding);
	ConnectBindings->SetObjectField(
		TEXT("/targetNodeId"),
		TargetBinding);
	Connect->SetObjectField(TEXT("bindings"), ConnectBindings);

	const TSharedPtr<FJsonObject> Workflow = MakeWorkflow(
		TEXT("material-connection-failure"),
		MakeScope(TEXT("material"), MaterialPath, false),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("firstConstant"),
					TEXT("content.material.expression.add"),
					FirstParams)),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("secondConstant"),
					TEXT("content.material.expression.add"),
					SecondParams)),
			MakeShared<FJsonValueObject>(Connect),
		});

	UEAIIntegration::Workflow::FWorkflowRuntime Runtime(
		*Subsystem->GetRegistry());
	const FMCPResult Plan = PlanWorkflow(Runtime, Workflow);
	FString Digest;
	if (!GetPlanDigest(Plan, Digest))
	{
		AddError(TEXT("Connection failure workflow did not plan."));
		CleanupAsset(MaterialPath);
		return false;
	}

	const FMCPResult Result = ExecuteWorkflow(Runtime, Workflow, Digest);
	TestFalse(
		TEXT("Incompatible material connection fails the workflow"),
		Result.bOk);
	TestEqual(
		TEXT("Connection failure is promoted to workflow failure"),
		Result.Error.Code,
		FString(TEXT("workflow_execution_failed")));
	TestTrue(
		TEXT("Connection failure rollback is verified"),
		Result.Error.Details.IsValid()
			&& Result.Error.Details->GetBoolField(TEXT("rollbackVerified")));
	TestTrue(
		TEXT("Existing-scope execution captured an in-memory fallback"),
		Result.Error.Details.IsValid()
			&& Result.Error.Details->GetBoolField(
				TEXT("memorySnapshotCaptured")));

	UMaterial* Material = Cast<UMaterial>(
		UEditorAssetLibrary::LoadAsset(MaterialPath));
	TestNotNull(TEXT("Material remains after failed connection"), Material);
	if (Material)
	{
		TestEqual(
			TEXT("Failed connection workflow leaves no added expressions"),
			Material->GetExpressions().Num(),
			0);
	}
	CleanupAsset(MaterialPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEWorkflowExplicitScopeInitializerTest,
	"UE_AI_integration.Workflow.ExplicitScopeInitializer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEWorkflowExplicitScopeInitializerTest::RunTest(
	const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		AddError(TEXT("UE integration subsystem is not initialized."));
		return false;
	}

	const FString MaterialPath =
		UniqueAssetPath(TEXT("M_ExplicitInitializer"));
	CleanupAsset(MaterialPath);
	TSharedPtr<FJsonObject> PropertyParams = MakeShared<FJsonObject>();
	PropertyParams->SetStringField(TEXT("property"), TEXT("twoSided"));
	PropertyParams->SetStringField(TEXT("value"), TEXT("true"));
	const TSharedPtr<FJsonObject> Workflow = MakeWorkflow(
		TEXT("explicit-scope-initializer"),
		MakeScope(TEXT("material"), MaterialPath, true),
		{
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("createMaterial"),
					TEXT("content.material.create"),
					MakeShared<FJsonObject>())),
			MakeShared<FJsonValueObject>(
				MakeOperation(
					TEXT("setTwoSided"),
					TEXT("content.material.property.set"),
					PropertyParams)),
		});

	UEAIIntegration::Workflow::FWorkflowRuntime Runtime(
		*Subsystem->GetRegistry());
	const FMCPResult Plan = PlanWorkflow(Runtime, Workflow);
	FString Digest;
	if (!GetPlanDigest(Plan, Digest))
	{
		AddError(TEXT("Explicit initializer workflow did not plan."));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* PlannedOperations = nullptr;
	FString FirstKind;
	TestTrue(
		TEXT("Planner marks authored scope initializer"),
		Plan.Data->TryGetArrayField(
			TEXT("operations"),
			PlannedOperations)
			&& PlannedOperations
			&& !PlannedOperations->IsEmpty()
			&& (*PlannedOperations)[0]->AsObject()->TryGetStringField(
				TEXT("kind"),
				FirstKind)
			&& FirstKind == TEXT("scopeInitializer"));

	const FMCPResult Result = ExecuteWorkflow(Runtime, Workflow, Digest);
	TestTrue(TEXT("Explicit initializer executes"), Result.bOk);
	TestTrue(
		TEXT("Explicit initializer creates exact scoped material"),
		AssetExistsWithoutLoading(MaterialPath));
	UMaterial* Material = Cast<UMaterial>(
		FindAssetWithoutLoading(MaterialPath));
	TestNotNull(TEXT("Explicitly created material loads"), Material);
	if (Material)
	{
		TestTrue(
			TEXT("Following edit step targets initialized scope"),
			Material->TwoSided != 0);
	}
	CleanupAsset(MaterialPath);
	return true;
}

#endif
