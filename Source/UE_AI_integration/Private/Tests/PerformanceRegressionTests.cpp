#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/PerformanceRegressionService.h"

#include "Misc/AutomationTest.h"

namespace
{
using UEAIIntegration::Infrastructure::FPerformanceRegressionService;

TSharedPtr<FJsonObject> MakeStandardRequest()
{
	TSharedPtr<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("profile"), TEXT("standardScenario"));
	Request->SetNumberField(TEXT("repeatCount"), 1);
	Request->SetNumberField(TEXT("warmupSeconds"), 3.0);
	Request->SetNumberField(TEXT("sampleSeconds"), 2.0);

	TSharedPtr<FJsonObject> Profile = MakeShared<FJsonObject>();
	Profile->SetStringField(TEXT("name"), TEXT("GoldenPIE"));
	Profile->SetStringField(TEXT("map"), TEXT("/Game/Maps/Golden"));
	TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();
	Camera->SetStringField(TEXT("name"), TEXT("PerfCamera"));
	Camera->SetArrayField(
		TEXT("location"),
		{
			MakeShared<FJsonValueNumber>(100.0),
			MakeShared<FJsonValueNumber>(200.0),
			MakeShared<FJsonValueNumber>(300.0)
		});
	Camera->SetArrayField(
		TEXT("rotation"),
		{
			MakeShared<FJsonValueNumber>(0.0),
			MakeShared<FJsonValueNumber>(90.0),
			MakeShared<FJsonValueNumber>(0.0)
		});
	Profile->SetObjectField(TEXT("camera"), Camera);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("id"), TEXT("drive-forward"));
	Input->SetStringField(TEXT("action"), TEXT("input.key"));
	TSharedPtr<FJsonObject> InputParams = MakeShared<FJsonObject>();
	InputParams->SetStringField(TEXT("action"), TEXT("press"));
	InputParams->SetStringField(TEXT("key"), TEXT("W"));
	Input->SetObjectField(TEXT("params"), InputParams);
	Profile->SetArrayField(
		TEXT("inputSteps"),
		{MakeShared<FJsonValueObject>(Input)});
	Request->SetObjectField(TEXT("standardProfile"), Profile);
	return Request;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPerformanceStandardProfileContractTest,
	"UE_AI_integration.Performance.StandardProfileContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPerformanceStandardProfileContractTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<FJsonObject> Normalized;
	TSharedPtr<FJsonObject> Profile;
	FString Error;
	TestTrue(
		TEXT("standard profile normalizes"),
		FPerformanceRegressionService::NormalizeRunRequest(
			MakeStandardRequest(),
			TEXT("/Game/Maps/Golden"),
			Normalized,
			Profile,
			Error));
	TestTrue(TEXT("no normalization error"), Error.IsEmpty());
	TestEqual(
		TEXT("standard profile uses scenario mode"),
		Normalized->GetStringField(TEXT("mode")),
		FString(TEXT("scenario")));
	TestEqual(
		TEXT("standard profile defaults to at least five repeats"),
		static_cast<int32>(
			Normalized->GetNumberField(TEXT("repeatCount"))),
		5);
	TestEqual(
		TEXT("warmup is represented before metrics.begin"),
		Normalized->GetNumberField(TEXT("warmupSeconds")),
		0.0);
	TestTrue(
		TEXT("profile has deterministic digest"),
		Profile->GetStringField(TEXT("digest")).StartsWith(
			TEXT("sha256:")));

	const TSharedPtr<FJsonObject> Scenario =
		Normalized->GetObjectField(TEXT("scenario"));
	const TArray<TSharedPtr<FJsonValue>>& Steps =
		Scenario->GetArrayField(TEXT("steps"));
	int32 BeginCount = 0;
	int32 EndCount = 0;
	int32 BeginIndex = INDEX_NONE;
	int32 EndIndex = INDEX_NONE;
	int32 InputIndex = INDEX_NONE;
	int32 CameraIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Step = Steps[Index]->AsObject();
		const FString Id = Step->GetStringField(TEXT("id"));
		const FString Action = Step->GetStringField(TEXT("action"));
		if (Action == TEXT("metrics.begin"))
		{
			++BeginCount;
			BeginIndex = Index;
		}
		else if (Action == TEXT("metrics.end"))
		{
			++EndCount;
			EndIndex = Index;
		}
		if (Id == TEXT("drive-forward"))
		{
			InputIndex = Index;
		}
		if (Id == TEXT("standard.camera.find"))
		{
			CameraIndex = Index;
		}
	}
	TestEqual(TEXT("one metrics.begin"), BeginCount, 1);
	TestEqual(TEXT("one metrics.end"), EndCount, 1);
	TestTrue(
		TEXT("camera is verified before deterministic input"),
		CameraIndex >= 0 && CameraIndex < InputIndex);
	TestTrue(
		TEXT("input precedes bounded measurement"),
		InputIndex >= 0 && InputIndex < BeginIndex);
	TestTrue(
		TEXT("metrics markers are ordered"),
		BeginIndex >= 0 && BeginIndex < EndIndex);
	TestEqual(
		TEXT("cleanup stops PIE"),
		Scenario->GetObjectField(TEXT("cleanup"))->GetBoolField(
			TEXT("stopPie")),
		true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPerformanceStandardProfileValidationTest,
	"UE_AI_integration.Performance.StandardProfileValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPerformanceStandardProfileValidationTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<FJsonObject> Normalized;
	TSharedPtr<FJsonObject> Profile;
	FString Error;
	TestFalse(
		TEXT("map mismatch is rejected instead of silently switching maps"),
		FPerformanceRegressionService::NormalizeRunRequest(
			MakeStandardRequest(),
			TEXT("/Game/Maps/Other"),
			Normalized,
			Profile,
			Error));
	TestTrue(
		TEXT("map mismatch is actionable"),
		Error.Contains(TEXT("Open the fixed map")));

	TSharedPtr<FJsonObject> Unsafe = MakeStandardRequest();
	TSharedPtr<FJsonObject> UnsafeStep = MakeShared<FJsonObject>();
	UnsafeStep->SetStringField(TEXT("id"), TEXT("nested-pie"));
	UnsafeStep->SetStringField(TEXT("action"), TEXT("pie.restart"));
	Unsafe->GetObjectField(TEXT("standardProfile"))->SetArrayField(
		TEXT("inputSteps"),
		{MakeShared<FJsonValueObject>(UnsafeStep)});
	Error.Reset();
	TestFalse(
		TEXT("profile input cannot override generated PIE lifecycle"),
		FPerformanceRegressionService::NormalizeRunRequest(
			Unsafe,
			TEXT("/Game/Maps/Golden"),
			Normalized,
			Profile,
			Error));
	TestTrue(
		TEXT("unsafe input error names deterministic boundary"),
		Error.Contains(TEXT("not deterministic input")));

	TSharedPtr<FJsonObject> Custom = MakeShared<FJsonObject>();
	Custom->SetStringField(TEXT("mode"), TEXT("window"));
	Custom->SetNumberField(TEXT("repeatCount"), 1);
	Error.Reset();
	TestTrue(
		TEXT("custom window behavior remains supported"),
		FPerformanceRegressionService::NormalizeRunRequest(
			Custom,
			TEXT("/Game/Maps/Golden"),
			Normalized,
			Profile,
			Error));
	TestEqual(
		TEXT("custom repeat count is not raised"),
		static_cast<int32>(
			Normalized->GetNumberField(TEXT("repeatCount"))),
		1);
	TestFalse(TEXT("custom profile stays absent"), Profile.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPerformanceRegressionReportContractTest,
	"UE_AI_integration.Performance.RegressionReportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPerformanceRegressionReportContractTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<FJsonObject> Comparison = MakeShared<FJsonObject>();
	Comparison->SetStringField(TEXT("verdict"), TEXT("regression"));
	Comparison->SetBoolField(TEXT("fingerprintCompatible"), true);

	TSharedPtr<FJsonObject> Passed = MakeShared<FJsonObject>();
	Passed->SetStringField(TEXT("metric"), TEXT("gameMs"));
	Passed->SetStringField(TEXT("statistic"), TEXT("p95"));
	Passed->SetStringField(TEXT("verdict"), TEXT("pass"));
	TSharedPtr<FJsonObject> Failed = MakeShared<FJsonObject>();
	Failed->SetStringField(TEXT("metric"), TEXT("gpuMs"));
	Failed->SetStringField(TEXT("statistic"), TEXT("p99"));
	Failed->SetStringField(TEXT("verdict"), TEXT("regression"));
	Failed->SetNumberField(TEXT("baseline"), 10.0);
	Failed->SetNumberField(TEXT("candidate"), 12.0);
	Failed->SetNumberField(TEXT("regressionPercent"), 20.0);
	TSharedPtr<FJsonObject> Skipped = MakeShared<FJsonObject>();
	Skipped->SetStringField(TEXT("metric"), TEXT("rhiMs"));
	Skipped->SetStringField(TEXT("statistic"), TEXT("p50"));
	Skipped->SetStringField(TEXT("verdict"), TEXT("inconclusive"));
	Comparison->SetArrayField(
		TEXT("checks"),
		{
			MakeShared<FJsonValueObject>(Passed),
			MakeShared<FJsonValueObject>(Failed),
			MakeShared<FJsonValueObject>(Skipped)
		});

	const TSharedPtr<FJsonObject> Summary =
		FPerformanceRegressionService::BuildRegressionSummary(Comparison);
	TestEqual(
		TEXT("summary check count"),
		static_cast<int32>(Summary->GetNumberField(TEXT("checkCount"))),
		3);
	TestEqual(
		TEXT("summary regression count"),
		static_cast<int32>(
			Summary->GetNumberField(TEXT("regressionCount"))),
		1);
	TestEqual(
		TEXT("summary inconclusive count"),
		static_cast<int32>(
			Summary->GetNumberField(TEXT("inconclusiveCount"))),
		1);

	const FString JUnit =
		FPerformanceRegressionService::BuildJUnitReport(
			TEXT("comparison-test"),
			Comparison);
	TestTrue(TEXT("JUnit declares all checks"), JUnit.Contains(TEXT("tests=\"3\"")));
	TestTrue(TEXT("JUnit declares regression"), JUnit.Contains(TEXT("failures=\"1\"")));
	TestTrue(TEXT("JUnit declares inconclusive"), JUnit.Contains(TEXT("skipped=\"1\"")));
	TestTrue(TEXT("JUnit includes failing metric"), JUnit.Contains(TEXT("gpuMs.p99")));
	return true;
}

#endif
