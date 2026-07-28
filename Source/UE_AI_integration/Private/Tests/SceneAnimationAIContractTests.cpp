#if WITH_DEV_AUTOMATION_TESTS

#include "Domains/AI/AIInspection.h"
#include "Domains/Animation/AnimationInspection.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Misc/AutomationTest.h"

using namespace UEAIIntegration::Infrastructure;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDomainChangeApprovalGateTest,
	"UE_AI_integration.EngineeringDomains.ChangeApprovalGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDomainChangeApprovalGateTest::RunTest(const FString& Parameters)
{
	const FString Digest =
		TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	FString ErrorCode;
	FString ErrorMessage;

	TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
	TestFalse(
		TEXT("Approval is required"),
		ValidateChangeApproval(Missing, Digest, ErrorCode, ErrorMessage));
	TestEqual(
		TEXT("Missing approval has a stable code"),
		ErrorCode,
		FString(TEXT("approval_required")));

	TSharedRef<FJsonObject> Wrong = MakeShared<FJsonObject>();
	Wrong->SetStringField(
		TEXT("approvePlanDigest"),
		TEXT("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
	Wrong->SetBoolField(TEXT("confirmWrite"), true);
	Wrong->SetStringField(TEXT("requestId"), TEXT("request-a"));
	TestFalse(
		TEXT("Wrong digest is rejected"),
		ValidateChangeApproval(Wrong, Digest, ErrorCode, ErrorMessage));
	TestEqual(
		TEXT("Digest mismatch has a stable code"),
		ErrorCode,
		FString(TEXT("plan_digest_mismatch")));

	TSharedRef<FJsonObject> Valid = MakeShared<FJsonObject>();
	Valid->SetStringField(TEXT("approvePlanDigest"), Digest);
	Valid->SetBoolField(TEXT("confirmWrite"), true);
	Valid->SetStringField(TEXT("requestId"), TEXT("request-a"));
	TestTrue(
		TEXT("Exact digest, confirmation, and request ID pass"),
		ValidateChangeApproval(Valid, Digest, ErrorCode, ErrorMessage));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDomainFindingContractTest,
	"UE_AI_integration.EngineeringDomains.AnimationAIFindingContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDomainFindingContractTest::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> AnimationFindings;
	UEAIIntegration::AnimationInspection::AddFinding(
		AnimationFindings,
		TEXT("animation.fixture"),
		TEXT("error"),
		TEXT("Fixture animation finding."),
		TEXT("/Game/Tests/ABP_Fixture"),
		TEXT("Locomotion"),
		TEXT("00000000-0000-0000-0000-000000000001"));

	TArray<TSharedPtr<FJsonValue>> AIFindings;
	UEAIIntegration::AIInspection::AddFinding(
		AIFindings,
		TEXT("ai.fixture"),
		TEXT("warning"),
		TEXT("Fixture AI finding."),
		TEXT("/Game/Tests/BT_Fixture"),
		TEXT("BehaviorTree"),
		TEXT("0.1"));

	for (const TArray<TSharedPtr<FJsonValue>>* Findings :
		{&AnimationFindings, &AIFindings})
	{
		TestEqual(TEXT("One finding is emitted"), Findings->Num(), 1);
		const TSharedPtr<FJsonObject> Finding = (*Findings)[0]->AsObject();
		TestEqual(
			TEXT("Finding schema"),
			Finding->GetStringField(TEXT("schema")),
			FString(TEXT("ue.finding.v1")));
		TestTrue(
			TEXT("Finding has a stable ID"),
			!Finding->GetStringField(TEXT("findingId")).IsEmpty());
		TestTrue(
			TEXT("Confidence is numeric and bounded"),
			Finding->GetNumberField(TEXT("confidence")) >= 0.0
				&& Finding->GetNumberField(TEXT("confidence")) <= 1.0);
		TestTrue(
			TEXT("Location is an object"),
			Finding->GetObjectField(TEXT("location")).IsValid());
		TestTrue(
			TEXT("Evidence is an object"),
			Finding->GetObjectField(TEXT("evidence")).IsValid());
		TestEqual(
			TEXT("Static finding status"),
			Finding->GetStringField(TEXT("runtimeStatus")),
			FString(TEXT("hypothesis")));
	}
	return true;
}

#endif
