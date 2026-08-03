#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.h"
#include "Workflow/UEWorkflowRuntime.h"

namespace
{
TSharedRef<FJsonObject> MakeBuildNode(
	const TCHAR* Ref,
	const TCHAR* NodeType,
	const int32 X,
	const int32 Y)
{
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("ref"), Ref);
	Node->SetStringField(TEXT("nodeType"), NodeType);
	Node->SetNumberField(TEXT("posX"), X);
	Node->SetNumberField(TEXT("posY"), Y);
	return Node;
}

TSharedRef<FJsonObject> MakeBuildDefinition(
	const FString& BlueprintPath,
	const FString& Graph,
	const FString& Mode,
	const bool bIncludeSequence)
{
	TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
	Definition->SetStringField(TEXT("schema"), TEXT("ue.blueprint-buildgraph.v1"));
	Definition->SetStringField(TEXT("buildId"), TEXT("automation-build"));
	Definition->SetStringField(TEXT("blueprint"), BlueprintPath);
	Definition->SetStringField(TEXT("graph"), Graph);
	Definition->SetStringField(TEXT("mode"), Mode);
	TArray<TSharedPtr<FJsonValue>> Nodes = {
		MakeShared<FJsonValueObject>(
			MakeBuildNode(TEXT("decision"), TEXT("Branch"), 200, 100)),
	};
	if (bIncludeSequence)
	{
		Nodes.Add(MakeShared<FJsonValueObject>(
			MakeBuildNode(TEXT("sequence"), TEXT("Sequence"), 500, 100)));
	}
	Definition->SetArrayField(TEXT("nodes"), Nodes);
	if (bIncludeSequence)
	{
		TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
		Connection->SetStringField(TEXT("sourceRef"), TEXT("decision"));
		Connection->SetStringField(TEXT("sourcePin"), TEXT("then"));
		Connection->SetStringField(TEXT("targetRef"), TEXT("sequence"));
		Connection->SetStringField(TEXT("targetPin"), TEXT("execute"));
		Definition->SetArrayField(
			TEXT("connections"),
			{MakeShared<FJsonValueObject>(Connection)});
	}
	return Definition;
}

int32 BlueprintNodeCount(const UBlueprint* Blueprint)
{
	int32 Count = 0;
	if (Blueprint)
	{
		for (const UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			Count += Graph ? Graph->Nodes.Num() : 0;
		}
		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			Count += Graph ? Graph->Nodes.Num() : 0;
		}
	}
	return Count;
}

FMCPResult ExecuteBuildWorkflow(
	UEAIIntegration::Workflow::FWorkflowRuntime& Runtime,
	const TSharedPtr<FJsonObject>& Workflow,
	const FString& Digest)
{
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("action"), TEXT("execute"));
	Request->SetObjectField(TEXT("workflow"), Workflow);
	Request->SetStringField(TEXT("approvePlanDigest"), Digest);
	Request->SetBoolField(TEXT("confirmWrite"), true);
	// A follow-up BuildGraph plan is intentionally prepared against a clean,
	// persisted baseline. This mirrors real declarative replays and keeps the
	// planner's Dirty-package conflict gate meaningful.
	Request->SetBoolField(TEXT("saveOnSuccess"), true);
	Request->SetStringField(TEXT("detailLevel"), TEXT("summary"));
	return Runtime.HandleRequest(Request);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintBuildGraphWorkflowTest,
	"UE_AI_integration.Blueprint.BuildGraph.WorkflowAndManagedNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBuildGraphWorkflowTest::RunTest(const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
		: nullptr;
	FMCPToolRegistry* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	FUEAIIntegrationServer* Server = Subsystem ? Subsystem->GetServer() : nullptr;
	auto* Runtime = Server ? Server->GetWorkflowRuntimeForTesting() : nullptr;
	if (!Registry || !Runtime)
	{
		AddError(TEXT("UE integration registry or Workflow runtime is unavailable."));
		return false;
	}

	const FString AssetName = FString::Printf(
		TEXT("BP_BuildGraph_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = TEXT("/Game/Automation");
	const FString BlueprintPath = PackagePath + TEXT("/") + AssetName;
	TSharedRef<FJsonObject> Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("blueprintName"), AssetName);
	Create->SetStringField(TEXT("packagePath"), PackagePath);
	Create->SetStringField(TEXT("parentClass"), TEXT("Actor"));
	const FMCPToolResult Created = Registry->ExecuteTool(
		TEXT("blueprint.asset.create"),
		Create);
	if (!Created.bSuccess)
	{
		AddError(TEXT("Could not create BuildGraph fixture: ") + Created.ErrorMessage);
		return false;
	}
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	if (!Blueprint || Blueprint->UbergraphPages.IsEmpty())
	{
		AddError(TEXT("BuildGraph fixture has no EventGraph."));
		UEditorAssetLibrary::DeleteAsset(BlueprintPath);
		return false;
	}
	const FString Graph = Blueprint->UbergraphPages[0]->GetName();
	const int32 ManualNodeCount = BlueprintNodeCount(Blueprint);

	const TSharedRef<FJsonObject> Definition = MakeBuildDefinition(
		BlueprintPath,
		Graph,
		TEXT("merge"),
		true);
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("definition"), Definition);
	const FMCPToolResult Validation = Registry->ExecuteTool(
		TEXT("blueprint.graph.build.validate"),
		Params);
	TestTrue(TEXT("BuildGraph definition validates"), Validation.bSuccess);
	TestTrue(
		TEXT("BuildGraph validation reports valid"),
		Validation.bSuccess && Validation.Data.IsValid()
			&& Validation.Data->GetBoolField(TEXT("valid")));

	const FMCPToolResult Planned = Registry->ExecuteTool(
		TEXT("blueprint.graph.build.plan"),
		Params);
	TestTrue(TEXT("BuildGraph compiles to Workflow v2"), Planned.bSuccess);
	if (!Planned.bSuccess || !Planned.Data.IsValid())
	{
		AddError(TEXT("BuildGraph plan failed: ") + Planned.ErrorMessage);
		UEditorAssetLibrary::DeleteAsset(BlueprintPath);
		return false;
	}
	const TSharedPtr<FJsonObject> Workflow = Planned.Data->GetObjectField(TEXT("workflow"));
	const FString Digest = Planned.Data->GetStringField(TEXT("planDigest"));
	const TSharedPtr<FJsonObject> PrimaryScope =
		Workflow->GetObjectField(TEXT("scopes"))->GetObjectField(TEXT("primary"));
	TestTrue(
		TEXT("Generated Workflow keeps verification on its named v2 scope"),
		PrimaryScope->HasTypedField<EJson::Object>(TEXT("verify")));

	const FMCPResult Executed = ExecuteBuildWorkflow(*Runtime, Workflow, Digest);
	TestTrue(TEXT("Generated Workflow executes"), Executed.bOk);
	if (!Executed.bOk)
	{
		AddError(FString::Printf(
			TEXT("BuildGraph Workflow failed: %s (%s)"),
			*Executed.Error.Code,
			*Executed.Error.Message));
		UEditorAssetLibrary::DeleteAsset(BlueprintPath);
		return false;
	}

	TSharedRef<FJsonObject> GetDefinition = MakeShared<FJsonObject>();
	GetDefinition->SetStringField(TEXT("blueprint"), BlueprintPath);
	GetDefinition->SetStringField(TEXT("graph"), Graph);
	GetDefinition->SetStringField(TEXT("buildId"), TEXT("automation-build"));
	const FMCPToolResult Stored = Registry->ExecuteTool(
		TEXT("blueprint.graph.build.definition.get"),
		GetDefinition);
	TestTrue(TEXT("Managed definition is persisted"), Stored.bSuccess);
	if (Stored.bSuccess && Stored.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> StoredDefinition =
			Stored.Data->GetObjectField(TEXT("definition"));
		TestEqual(
			TEXT("Both stable refs are persisted"),
			StoredDefinition->GetObjectField(TEXT("managedRefs"))->Values.Num(),
			2);
	}
	Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
	TestEqual(
		TEXT("BuildGraph adds exactly two managed nodes"),
		BlueprintNodeCount(Blueprint),
		ManualNodeCount + 2);

	const FMCPToolResult ReplayPlan = Registry->ExecuteTool(
		TEXT("blueprint.graph.build.plan"),
		Params);
	TestTrue(TEXT("Identical BuildGraph replay plans"), ReplayPlan.bSuccess);
	if (ReplayPlan.bSuccess && ReplayPlan.Data.IsValid())
	{
		const FMCPResult Replayed = ExecuteBuildWorkflow(
			*Runtime,
			ReplayPlan.Data->GetObjectField(TEXT("workflow")),
			ReplayPlan.Data->GetStringField(TEXT("planDigest")));
		TestTrue(TEXT("Identical BuildGraph replay executes"), Replayed.bOk);
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		TestEqual(
			TEXT("Identical replay does not duplicate managed nodes"),
			BlueprintNodeCount(Blueprint),
			ManualNodeCount + 2);
		if (Replayed.bOk && Replayed.Data.IsValid())
		{
			TSharedRef<FJsonObject> ReplayRollback = MakeShared<FJsonObject>();
			ReplayRollback->SetStringField(TEXT("action"), TEXT("rollback"));
			ReplayRollback->SetStringField(
				TEXT("runId"),
				Replayed.Data->GetStringField(TEXT("runId")));
			ReplayRollback->SetStringField(
				TEXT("approvePlanDigest"),
				ReplayPlan.Data->GetStringField(TEXT("planDigest")));
			TestTrue(
				TEXT("Identical replay uses Workflow rollback"),
				Runtime->HandleRequest(ReplayRollback).bOk);
		}
	}

	TSharedRef<FJsonObject> ReplaceParams = MakeShared<FJsonObject>();
	ReplaceParams->SetObjectField(
		TEXT("definition"),
		MakeBuildDefinition(BlueprintPath, Graph, TEXT("replaceManaged"), false));
	const FMCPToolResult ReplacePlan = Registry->ExecuteTool(
		TEXT("blueprint.graph.build.plan"),
		ReplaceParams);
	TestTrue(
		FString::Printf(
			TEXT("replaceManaged plans (%s: %s)"),
			*ReplacePlan.ErrorCode,
			*ReplacePlan.ErrorMessage),
		ReplacePlan.bSuccess);
	if (ReplacePlan.bSuccess && ReplacePlan.Data.IsValid())
	{
		const FMCPResult Replaced = ExecuteBuildWorkflow(
			*Runtime,
			ReplacePlan.Data->GetObjectField(TEXT("workflow")),
			ReplacePlan.Data->GetStringField(TEXT("planDigest")));
		TestTrue(TEXT("replaceManaged executes"), Replaced.bOk);
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		TestEqual(
			TEXT("replaceManaged removes only the obsolete managed node"),
			BlueprintNodeCount(Blueprint),
			ManualNodeCount + 1);
		if (Replaced.bOk && Replaced.Data.IsValid())
		{
			TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
			Rollback->SetStringField(TEXT("action"), TEXT("rollback"));
			Rollback->SetStringField(
				TEXT("runId"),
				Replaced.Data->GetStringField(TEXT("runId")));
			Rollback->SetStringField(
				TEXT("approvePlanDigest"),
				ReplacePlan.Data->GetStringField(TEXT("planDigest")));
			TestTrue(
				TEXT("BuildGraph uses existing Workflow rollback"),
				Runtime->HandleRequest(Rollback).bOk);
		}
	}
	TSharedRef<FJsonObject> RollbackInitial = MakeShared<FJsonObject>();
	RollbackInitial->SetStringField(TEXT("action"), TEXT("rollback"));
	RollbackInitial->SetStringField(
		TEXT("runId"),
		Executed.Data->GetStringField(TEXT("runId")));
	RollbackInitial->SetStringField(TEXT("approvePlanDigest"), Digest);
	TestTrue(
		TEXT("Initial BuildGraph Workflow also rolls back before fixture cleanup"),
		Runtime->HandleRequest(RollbackInitial).bOk);

	UEditorAssetLibrary::DeleteAsset(BlueprintPath);
	return true;
}

#endif
