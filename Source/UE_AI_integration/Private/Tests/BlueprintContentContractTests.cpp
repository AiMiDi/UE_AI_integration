#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/EngineeringContractUtils.h"
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

using namespace UEAIIntegration::Infrastructure;

namespace UEAIIntegrationTools
{
void RegisterBlueprintAnalysisTools(FMCPToolRegistry& Registry);
void RegisterContentAssetReadTools(FMCPToolRegistry& Registry);
void RegisterContentAssetChangeTools(FMCPToolRegistry& Registry);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEngineeringContractDeterminismTest,
	"UE_AI_integration.EngineeringContracts.DeterministicDigest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEngineeringContractDeterminismTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> First = MakeShared<FJsonObject>();
	First->SetStringField(TEXT("z"), TEXT("last"));
	First->SetNumberField(TEXT("a"), 7);
	TSharedRef<FJsonObject> Second = MakeShared<FJsonObject>();
	Second->SetNumberField(TEXT("a"), 7);
	Second->SetStringField(TEXT("z"), TEXT("last"));

	const FString FirstDigest = DigestJson(First);
	const FString SecondDigest = DigestJson(Second);
	TestEqual(
		TEXT("Object insertion order does not affect digest"),
		FirstDigest,
		SecondDigest);
	TestEqual(TEXT("SHA-256 digest length"), FirstDigest.Len(), 64);

	const FString FirstId =
		MakeStableId(TEXT("fixture"), {TEXT("/Game/A"), TEXT("Graph"), TEXT("Node")});
	const FString SecondId =
		MakeStableId(TEXT("fixture"), {TEXT("/Game/A"), TEXT("Graph"), TEXT("Node")});
	TestEqual(TEXT("Stable identifiers are repeatable"), FirstId, SecondId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEngineeringFindingContractTest,
	"UE_AI_integration.EngineeringContracts.FindingAndBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEngineeringFindingContractTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(TEXT("nodeId"), TEXT("bpnode-fixture"));
	TSharedRef<FJsonObject> Finding = MakeFinding(
		TEXT("blueprint.fixture.rule"),
		TEXT("warning"),
		0.75,
		TEXT("/Game/Tests/BP_Fixture"),
		TEXT("EventGraph"),
		TEXT("00000000-0000-0000-0000-000000000001"),
		TEXT("Fixture finding."),
		Evidence);

	TestEqual(
		TEXT("Finding schema is stable"),
		Finding->GetStringField(TEXT("schema")),
		FString(TEXT("ue.finding.v1")));
	TestEqual(
		TEXT("Static finding starts as a hypothesis"),
		Finding->GetStringField(TEXT("runtimeStatus")),
		FString(TEXT("hypothesis")));
	TestEqual(
		TEXT("Legacy warning severity normalizes to the shared contract"),
		Finding->GetStringField(TEXT("severity")),
		FString(TEXT("medium")));
	TestEqual(
		TEXT("Diagnostic severity is normalized to the shared scale"),
		Finding->GetStringField(TEXT("severity")),
		FString(TEXT("medium")));
	TestTrue(
		TEXT("Finding ID is namespaced"),
		Finding->GetStringField(TEXT("findingId")).StartsWith(TEXT("finding-")));

	TArray<TSharedPtr<FJsonValue>> Values = {
		MakeShared<FJsonValueNumber>(1),
		MakeShared<FJsonValueNumber>(2),
		MakeShared<FJsonValueNumber>(3),
	};
	TSharedRef<FJsonObject> Bounded = MakeShared<FJsonObject>();
	SetBoundedArray(Bounded, TEXT("items"), Values, 3, 2);
	TestEqual(TEXT("Bounded array count"), Bounded->GetArrayField(TEXT("items")).Num(), 2);
	TestEqual(TEXT("Bounded total is preserved"), Bounded->GetIntegerField(TEXT("itemsTotal")), 3);
	TestTrue(TEXT("Bounded array reports truncation"), Bounded->GetBoolField(TEXT("itemsTruncated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintContentRegistrarContractTest,
	"UE_AI_integration.EngineeringContracts.BlueprintContentRegistrars",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintContentRegistrarContractTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintAnalysisTools(Registry);
	Registry.EndDomainRegistration();
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetReadTools(Registry);
	UEAIIntegrationTools::RegisterContentAssetChangeTools(Registry);
	Registry.EndDomainRegistration();

	TestEqual(TEXT("All Blueprint and Content engineering tools register"), Registry.Num(), 13);
	TestNotNull(TEXT("Blueprint scan is registered"), Registry.FindTool(TEXT("blueprint.scan")));
	TestNotNull(
		TEXT("Content plan is registered"),
		Registry.FindTool(TEXT("content.asset.change.plan")));
	TestNotNull(
		TEXT("Content rollback is registered"),
		Registry.FindTool(TEXT("content.asset.change.rollback")));

	TSharedRef<FJsonObject> EmptyScope = MakeShared<FJsonObject>();
	EmptyScope->SetStringField(
		TEXT("pathPrefix"),
		TEXT("/Game/__UEAIContractMissing"));
	EmptyScope->SetNumberField(TEXT("assetLimit"), 1);
	EmptyScope->SetNumberField(TEXT("findingLimit"), 1);
	const FMCPToolResult EmptyScan =
		Registry.ExecuteTool(TEXT("blueprint.scan"), EmptyScope);
	TestTrue(TEXT("Bounded empty Blueprint scan succeeds"), EmptyScan.bSuccess);
	if (EmptyScan.bSuccess && EmptyScan.Data.IsValid())
	{
		TestEqual(
			TEXT("Blueprint scan emits stable schema"),
			EmptyScan.Data->GetStringField(TEXT("schema")),
			FString(TEXT("ue.blueprint-scan.v1")));
		TestEqual(
			TEXT("Missing Blueprint scope has no assets"),
			EmptyScan.Data->GetIntegerField(TEXT("assetsCount")),
			0);
	}

	TSharedRef<FJsonObject> UnknownCorrelation = MakeShared<FJsonObject>();
	UnknownCorrelation->SetStringField(TEXT("scanId"), TEXT("bpscan-missing"));
	UnknownCorrelation->SetStringField(TEXT("runId"), TEXT("run-fixture"));
	const FMCPToolResult MissingScan = Registry.ExecuteTool(
		TEXT("blueprint.findings.correlate"),
		UnknownCorrelation);
	TestFalse(TEXT("Unknown scan cannot be correlated"), MissingScan.bSuccess);
	TestEqual(
		TEXT("Unknown scan returns stable error"),
		MissingScan.ErrorCode,
		FString(TEXT("scan_not_found")));

	TArray<TSharedPtr<FJsonValue>> TooManyActions;
	for (int32 Index = 0; Index < 101; ++Index)
	{
		TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("action"), TEXT("fixRedirectors"));
		TooManyActions.Add(MakeShared<FJsonValueObject>(Action));
	}
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetArrayField(TEXT("actions"), TooManyActions);
	TSharedRef<FJsonObject> PlanParams = MakeShared<FJsonObject>();
	PlanParams->SetObjectField(TEXT("request"), Request);
	const FMCPToolResult OversizedPlan = Registry.ExecuteTool(
		TEXT("content.asset.change.plan"),
		PlanParams);
	TestFalse(TEXT("Asset plan rejects more than 100 actions"), OversizedPlan.bSuccess);
	TestEqual(
		TEXT("Oversized asset plan has stable error"),
		OversizedPlan.ErrorCode,
		FString(TEXT("plan_invalid")));

	const FMCPToolResult UnconfirmedExecute = Registry.ExecuteTool(
		TEXT("content.asset.change.execute"),
		MakeShared<FJsonObject>());
	TestFalse(TEXT("Asset execute requires approval and confirmation"), UnconfirmedExecute.bSuccess);
	TestEqual(
		TEXT("Unconfirmed write has stable error"),
		UnconfirmedExecute.ErrorCode,
		FString(TEXT("write_confirmation_required")));
	return true;
}

#endif
