#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/EngineeringContractUtils.h"
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#include "Algo/Reverse.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet/GameplayStatics.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintAnalysisTools(FMCPToolRegistry& Registry);

namespace BlueprintAnalysisTesting
{
bool MatchesScopePathsForTesting(
	const FString& PackagePath,
	const FString& ObjectPath,
	const FString& AssetName,
	const FString& RequestedAsset,
	const FString& PathPrefix);
TArray<TSharedPtr<FJsonObject>> ScanBlueprintForTesting(
	UBlueprint* Blueprint,
	const FString& AssetPath);
TSharedRef<FJsonObject> ProjectFindingsForTesting(
	const TArray<TSharedPtr<FJsonObject>>& Findings,
	const TSharedPtr<FJsonObject>& Params);
FString RuntimeStatusForTesting(
	bool bObserved,
	bool bHasDebugTrace,
	bool bCompleteCoverage,
	bool bBoundedRange);
}
}

namespace
{
using UEAIIntegration::Infrastructure::DigestJson;
using UEAIIntegration::Infrastructure::MakeFinding;
using namespace UEAIIntegrationTools::BlueprintAnalysisTesting;

template <typename TNode>
TNode* AddAnalysisNode(UEdGraph* Graph)
{
	TNode* Node = NewObject<TNode>(Graph);
	Graph->AddNode(Node, false, false);
	Node->CreateNewGuid();
	return Node;
}

UEdGraphPin* AddExecPin(
	UEdGraphNode* Node,
	const EEdGraphPinDirection Direction,
	const FName Name)
{
	return Node->CreatePin(
		Direction,
		UEdGraphSchema_K2::PC_Exec,
		Name);
}

void LinkExec(UEdGraphPin* Output, UEdGraphPin* Input)
{
	if (Output && Input)
	{
		Output->MakeLinkTo(Input);
	}
}

UBlueprint* MakeReachabilityGoldenFixture()
{
	UBlueprint* Blueprint =
		NewObject<UBlueprint>(GetTransientPackage());

	UEdGraph* EventGraph = NewObject<UEdGraph>(
		Blueprint,
		TEXT("EventGraph"));
	EventGraph->Schema = UEdGraphSchema_K2::StaticClass();
	Blueprint->UbergraphPages.Add(EventGraph);

	UK2Node_Event* Tick = AddAnalysisNode<UK2Node_Event>(EventGraph);
	Tick->EventReference.SetExternalMember(
		TEXT("ReceiveTick"),
		AActor::StaticClass());
	UEdGraphPin* TickExec =
		AddExecPin(Tick, EGPD_Output, UEdGraphSchema_K2::PN_Then);

	UK2Node_CallFunction* LocalCall =
		AddAnalysisNode<UK2Node_CallFunction>(EventGraph);
	LocalCall->FunctionReference.SetSelfMember(TEXT("HotPath"));
	UEdGraphPin* LocalExecIn =
		AddExecPin(LocalCall, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
	AddExecPin(LocalCall, EGPD_Output, UEdGraphSchema_K2::PN_Then);
	LinkExec(TickExec, LocalExecIn);

	UEdGraph* FunctionGraph = NewObject<UEdGraph>(
		Blueprint,
		TEXT("HotPath"));
	FunctionGraph->Schema = UEdGraphSchema_K2::StaticClass();
	Blueprint->FunctionGraphs.Add(FunctionGraph);
	UK2Node_FunctionEntry* Entry =
		AddAnalysisNode<UK2Node_FunctionEntry>(FunctionGraph);
	UEdGraphPin* EntryExec =
		AddExecPin(Entry, EGPD_Output, UEdGraphSchema_K2::PN_Then);

	UFunction* TraversalFunction =
		UGameplayStatics::StaticClass()->FindFunctionByName(
			GET_FUNCTION_NAME_CHECKED(
				UGameplayStatics,
				GetAllActorsOfClass));
	UK2Node_CallFunction* ReachableTraversal =
		AddAnalysisNode<UK2Node_CallFunction>(FunctionGraph);
	ReachableTraversal->FunctionReference.SetFromField<UFunction>(
		TraversalFunction,
		false);
	UEdGraphPin* ReachableExecIn = AddExecPin(
		ReachableTraversal,
		EGPD_Input,
		UEdGraphSchema_K2::PN_Execute);
	AddExecPin(
		ReachableTraversal,
		EGPD_Output,
		UEdGraphSchema_K2::PN_Then);
	LinkExec(EntryExec, ReachableExecIn);

	UK2Node_CallFunction* UnreachableTraversal =
		AddAnalysisNode<UK2Node_CallFunction>(EventGraph);
	UnreachableTraversal->FunctionReference.SetFromField<UFunction>(
		TraversalFunction,
		false);
	AddExecPin(
		UnreachableTraversal,
		EGPD_Input,
		UEdGraphSchema_K2::PN_Execute);
	AddExecPin(
		UnreachableTraversal,
		EGPD_Output,
		UEdGraphSchema_K2::PN_Then);
	return Blueprint;
}

TSharedPtr<FJsonObject> FindFinding(
	const TArray<TSharedPtr<FJsonObject>>& Findings,
	const FString& RuleId,
	const FString& Severity)
{
	for (const TSharedPtr<FJsonObject>& Finding : Findings)
	{
		if (Finding->GetStringField(TEXT("ruleId")) == RuleId
			&& Finding->GetStringField(TEXT("severity")) == Severity)
		{
			return Finding;
		}
	}
	return nullptr;
}

TSharedRef<FJsonObject> MakeFixtureFinding(
	const FString& RuleId,
	const FString& Severity,
	const FString& AssetPath,
	const FString& NodeGuid)
{
	TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(
		TEXT("nodeId"),
		TEXT("bpnode-") + NodeGuid);
	return MakeFinding(
		RuleId,
		Severity,
		0.9,
		AssetPath,
		TEXT("EventGraph"),
		NodeGuid,
		TEXT("Golden corpus finding."),
		Evidence);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintAnalysisScopeAndProjectionTest,
	"UE_AI_integration.BlueprintAnalysis.ScopeAndProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAnalysisScopeAndProjectionTest::RunTest(
	const FString& Parameters)
{
	TestTrue(
		TEXT("Directory includes its exact package"),
		MatchesScopePathsForTesting(
			TEXT("/Game/UI"),
			TEXT("/Game/UI.UI"),
			TEXT("UI"),
			FString(),
			TEXT("/Game/UI")));
	TestTrue(
		TEXT("Directory includes descendants"),
		MatchesScopePathsForTesting(
			TEXT("/Game/UI/WBP_Login"),
			TEXT("/Game/UI/WBP_Login.WBP_Login"),
			TEXT("WBP_Login"),
			FString(),
			TEXT("/Game/UI")));
	TestFalse(
		TEXT("Directory boundary excludes sibling prefixes"),
		MatchesScopePathsForTesting(
			TEXT("/Game/UI2/WBP_Noise"),
			TEXT("/Game/UI2/WBP_Noise.WBP_Noise"),
			TEXT("WBP_Noise"),
			FString(),
			TEXT("/Game/UI")));
	TestFalse(
		TEXT("/Game scope excludes Engine assets"),
		MatchesScopePathsForTesting(
			TEXT("/Engine/EditorBlueprintResources/StandardMacros"),
			TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
			TEXT("StandardMacros"),
			FString(),
			TEXT("/Game")));

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintAnalysisTools(Registry);
	Registry.EndDomainRegistration();
	TSharedRef<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	EmptyParams->SetNumberField(TEXT("assetLimit"), 1);
	EmptyParams->SetNumberField(TEXT("findingLimit"), 1);
	const FMCPToolResult DefaultScan =
		Registry.ExecuteTool(TEXT("blueprint.scan"), EmptyParams);
	TestTrue(TEXT("Scope-less scan succeeds"), DefaultScan.bSuccess);
	if (DefaultScan.bSuccess)
	{
		const TSharedPtr<FJsonObject> Scope =
			DefaultScan.Data->GetObjectField(TEXT("scope"));
		TestEqual(
			TEXT("Scope-less scan defaults to /Game"),
			Scope->GetStringField(TEXT("pathPrefix")),
			FString(TEXT("/Game")));
		TestTrue(
			TEXT("Default scope is explicit in response"),
			Scope->GetBoolField(TEXT("defaultedToGame")));
		for (const TSharedPtr<FJsonValue>& Asset :
			DefaultScan.Data->GetArrayField(TEXT("assets")))
		{
			TestTrue(
				TEXT("Default scan cannot return mounted Engine/plugin assets"),
				Asset->AsString().Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
					|| Asset->AsString().StartsWith(
						TEXT("/Game/"),
						ESearchCase::IgnoreCase));
		}
	}

	TArray<TSharedPtr<FJsonObject>> Findings = {
		MakeFixtureFinding(
			TEXT("blueprint.golden.high"),
			TEXT("high"),
			TEXT("/Game/Golden/BP_A"),
			TEXT("00000000-0000-0000-0000-000000000001")),
		MakeFixtureFinding(
			TEXT("blueprint.golden.suppressed"),
			TEXT("medium"),
			TEXT("/Game/Golden/BP_A"),
			TEXT("00000000-0000-0000-0000-000000000002")),
		MakeFixtureFinding(
			TEXT("blueprint.golden.baseline"),
			TEXT("medium"),
			TEXT("/Game/Golden/BP_B"),
			TEXT("00000000-0000-0000-0000-000000000003")),
		MakeFixtureFinding(
			TEXT("blueprint.golden.returned"),
			TEXT("medium"),
			TEXT("/Game/Golden/BP_C"),
			TEXT("00000000-0000-0000-0000-000000000004")),
	};
	TSharedRef<FJsonObject> Filter = MakeShared<FJsonObject>();
	Filter->SetStringField(TEXT("severity"), TEXT("medium"));
	Filter->SetStringField(TEXT("runtimeStatus"), TEXT("hypothesis"));
	TSharedRef<FJsonObject> Suppression = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SuppressedFindingIds;
	SuppressedFindingIds.Add(
		MakeShared<FJsonValueString>(
			Findings[1]->GetStringField(TEXT("findingId"))));
	Suppression->SetArrayField(
		TEXT("findingIds"),
		SuppressedFindingIds);
	Filter->SetObjectField(TEXT("suppression"), Suppression);
	TSharedRef<FJsonObject> Baseline = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> BaselineFindingIds;
	BaselineFindingIds.Add(
		MakeShared<FJsonValueString>(
			Findings[2]->GetStringField(TEXT("findingId"))));
	Baseline->SetArrayField(
		TEXT("findingIds"),
		BaselineFindingIds);
	Baseline->SetStringField(TEXT("mode"), TEXT("excludeKnown"));
	Filter->SetObjectField(TEXT("baseline"), Baseline);
	Filter->SetNumberField(TEXT("findingLimit"), 1);
	const TSharedRef<FJsonObject> Filtered =
		ProjectFindingsForTesting(Findings, Filter);
	const TSharedPtr<FJsonObject> Stats =
		Filtered->GetObjectField(TEXT("findingStats"));
	TestEqual(TEXT("Stats preserve raw count"), Stats->GetIntegerField(TEXT("raw")), 4);
	TestEqual(TEXT("Severity filter runs before paging"), Stats->GetIntegerField(TEXT("filtered")), 3);
	TestEqual(TEXT("Suppression count is explicit"), Stats->GetIntegerField(TEXT("suppressed")), 1);
	TestEqual(TEXT("Baseline count is explicit"), Stats->GetIntegerField(TEXT("baselineExcluded")), 1);
	TestEqual(TEXT("Returned count reflects page"), Stats->GetIntegerField(TEXT("returned")), 1);
	TestEqual(
		TEXT("Remaining finding is returned"),
		Filtered->GetArrayField(TEXT("findings"))[0]
			->AsObject()
			->GetStringField(TEXT("ruleId")),
		FString(TEXT("blueprint.golden.returned")));

	TArray<TSharedPtr<FJsonObject>> Reversed = Findings;
	Algo::Reverse(Reversed);
	const TSharedRef<FJsonObject> StableA =
		ProjectFindingsForTesting(Findings, MakeShared<FJsonObject>());
	const TSharedRef<FJsonObject> StableB =
		ProjectFindingsForTesting(Reversed, MakeShared<FJsonObject>());
	TestEqual(
		TEXT("Projection ordering is independent of input order"),
		DigestJson(StableA),
		DigestJson(StableB));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintAnalysisGoldenReachabilityTest,
	"UE_AI_integration.BlueprintAnalysis.GoldenCorpus.TickReachability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAnalysisGoldenReachabilityTest::RunTest(
	const FString& Parameters)
{
	UBlueprint* Blueprint = MakeReachabilityGoldenFixture();
	const TArray<TSharedPtr<FJsonObject>> Findings =
		ScanBlueprintForTesting(
			Blueprint,
			TEXT("/Game/Golden/BP_TickReachability"));
	const TSharedPtr<FJsonObject> Reachable = FindFinding(
		Findings,
		TEXT("blueprint.call.global_traversal"),
		TEXT("high"));
	const TSharedPtr<FJsonObject> Unreachable = FindFinding(
		Findings,
		TEXT("blueprint.call.global_traversal"),
		TEXT("medium"));
	TestNotNull(
		TEXT("Reachable expensive call is upgraded"),
		Reachable.Get());
	TestNotNull(
		TEXT("Unreachable call in the same Tick Blueprint is not upgraded"),
		Unreachable.Get());
	if (Reachable.IsValid())
	{
		const TSharedPtr<FJsonObject> Evidence =
			Reachable->GetObjectField(TEXT("evidence"));
		TestTrue(
			TEXT("Reachable finding carries proof"),
			Evidence->GetBoolField(TEXT("tickReachable")));
		TestTrue(
			TEXT("Proof crosses Tick call and local function entry"),
			Evidence->GetArrayField(TEXT("executionPath")).Num() >= 4);
		TestEqual(
			TEXT("Static reachable finding remains a hypothesis"),
			Reachable->GetStringField(TEXT("runtimeStatus")),
			FString(TEXT("hypothesis")));
	}
	if (Unreachable.IsValid())
	{
		const TSharedPtr<FJsonObject> Evidence =
			Unreachable->GetObjectField(TEXT("evidence"));
		TestFalse(
			TEXT("Same-graph presence is not reachability"),
			Evidence->GetBoolField(TEXT("tickReachable")));
		TestFalse(
			TEXT("Unreachable evidence has no invented path"),
			Evidence->HasField(TEXT("executionPath")));
	}

	const TSharedRef<FJsonObject> DefaultProjection =
		ProjectFindingsForTesting(Findings, MakeShared<FJsonObject>());
	TestTrue(
		TEXT("Disconnected outputs are summarized by default"),
		DefaultProjection->GetObjectField(TEXT("findingStats"))
			->GetIntegerField(TEXT("summarized")) > 0);
	for (const TSharedPtr<FJsonValue>& Finding :
		DefaultProjection->GetArrayField(TEXT("findings")))
	{
		TestNotEqual(
			TEXT("Default findings omit disconnected-output noise"),
			Finding->AsObject()->GetStringField(TEXT("ruleId")),
			FString(TEXT("blueprint.exec.disconnected_output")));
	}

	TSharedRef<FJsonObject> ExpandDisconnected = MakeShared<FJsonObject>();
	ExpandDisconnected->SetStringField(
		TEXT("rule"),
		TEXT("blueprint.exec.disconnected_output"));
	const TSharedRef<FJsonObject> Expanded =
		ProjectFindingsForTesting(Findings, ExpandDisconnected);
	TestTrue(
		TEXT("Explicit disconnected rule expands individual pins"),
		!Expanded->GetArrayField(TEXT("findings")).IsEmpty());
	for (const TSharedPtr<FJsonValue>& Finding :
		Expanded->GetArrayField(TEXT("findings")))
	{
		const TSharedPtr<FJsonObject> Evidence =
			Finding->AsObject()->GetObjectField(TEXT("evidence"));
		TestTrue(
			TEXT("Expanded disconnected output names its pin"),
			Evidence->HasTypedField<EJson::String>(TEXT("pinName"))
				&& !Evidence->GetStringField(TEXT("pinName")).IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintAnalysisRuntimeCoverageTest,
	"UE_AI_integration.BlueprintAnalysis.RuntimeEvidenceCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAnalysisRuntimeCoverageTest::RunTest(
	const FString& Parameters)
{
	TestEqual(
		TEXT("Observed nodes are corroborated even in partial evidence"),
		RuntimeStatusForTesting(true, true, false, true),
		FString(TEXT("corroborated")));
	TestEqual(
		TEXT("Unobserved nodes remain hypotheses in partial evidence"),
		RuntimeStatusForTesting(false, true, false, true),
		FString(TEXT("hypothesis")));
	TestEqual(
		TEXT("A complete claim without a bounded trace remains partial"),
		RuntimeStatusForTesting(false, true, true, false),
		FString(TEXT("hypothesis")));
	TestEqual(
		TEXT("Only complete bounded trace can establish notObserved"),
		RuntimeStatusForTesting(false, true, true, true),
		FString(TEXT("notObserved")));
	TestEqual(
		TEXT("Manual evidence cannot establish negative coverage"),
		RuntimeStatusForTesting(false, false, true, true),
		FString(TEXT("hypothesis")));
	return true;
}

#endif
