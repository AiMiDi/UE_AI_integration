#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/ProductionJobRuntime.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Net/Core/Trace/NetTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceAnalysisContracts.h"
#include "TraceAnalysisService.h"

#include <limits>

using UEAIIntegration::Infrastructure::FProductionJobRuntime;

namespace
{
TSharedPtr<FJsonObject> MakeMetricSummary(
	const double P95,
	const int32 SampleCount = 120)
{
	TSharedPtr<FJsonObject> Metric = MakeShared<FJsonObject>();
	Metric->SetBoolField(TEXT("available"), true);
	Metric->SetNumberField(TEXT("sampleCount"), SampleCount);
	Metric->SetNumberField(TEXT("min"), P95 * 0.5);
	Metric->SetNumberField(TEXT("max"), P95 * 1.5);
	Metric->SetNumberField(TEXT("mean"), P95 * 0.8);
	Metric->SetNumberField(TEXT("p50"), P95 * 0.7);
	Metric->SetNumberField(TEXT("p95"), P95);
	Metric->SetNumberField(TEXT("p99"), P95 * 1.2);
	return Metric;
}

TSharedPtr<FJsonObject> MakePerformanceResult(
	const double FrameP95,
	const double GameP95,
	const FString& Map = TEXT("AutomationMap"))
{
	TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
	Context->SetStringField(TEXT("project"), TEXT("Automation"));
	Context->SetStringField(TEXT("map"), Map);
	Context->SetStringField(TEXT("rhi"), TEXT("D3D12"));
	Context->SetStringField(TEXT("gpu"), TEXT("Test GPU"));
	Context->SetStringField(TEXT("resolution"), TEXT("1920x1080"));
	Context->SetStringField(TEXT("configuration"), TEXT("Development"));
	Context->SetStringField(TEXT("engineVersion"), TEXT("5.3.0"));
	Context->SetStringField(TEXT("platform"), TEXT("Windows"));

	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	Metrics->SetObjectField(
		TEXT("frameMs"),
		MakeMetricSummary(FrameP95));
	Metrics->SetObjectField(
		TEXT("gameMs"),
		MakeMetricSummary(GameP95));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("context"), Context);
	Result->SetObjectField(TEXT("metrics"), Metrics);
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionJobRuntimeContractTest,
	"UE_AI_integration.Production.JobRuntime.Contracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionJobRuntimeContractTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> First = MakeShared<FJsonObject>();
	First->SetStringField(TEXT("action"), TEXT("stage"));
	First->SetArrayField(
		TEXT("files"),
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueString>(TEXT("Source/A.cpp")),
			MakeShared<FJsonValueString>(TEXT("Source/B.cpp"))
		});
	TSharedPtr<FJsonObject> Second = MakeShared<FJsonObject>();
	Second->SetArrayField(
		TEXT("files"),
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueString>(TEXT("Source/A.cpp")),
			MakeShared<FJsonValueString>(TEXT("Source/B.cpp"))
		});
	Second->SetStringField(TEXT("action"), TEXT("stage"));
	const FString FirstDigest =
		FProductionJobRuntime::ComputeChangePlanDigest(First);
	const FString SecondDigest =
		FProductionJobRuntime::ComputeChangePlanDigest(Second);
	TestTrue(TEXT("Digest uses a SHA-256 prefix"), FirstDigest.StartsWith(TEXT("sha256:")));
	TestEqual(TEXT("Digest is independent of JSON field insertion order"), FirstDigest, SecondDigest);

	const FString SavedRoot =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	const FString LocalDdc =
		FPaths::Combine(SavedRoot, TEXT("DerivedDataCache"));
	const FString Outside =
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(SavedRoot, TEXT("../Config")));
	TestTrue(
		TEXT("Project-local DDC remains inside the allowed root"),
		FProductionJobRuntime::IsPathWithin(LocalDdc, SavedRoot));
	TestFalse(
		TEXT("Collapsed parent traversal is rejected by the allowed root"),
		FProductionJobRuntime::IsPathWithin(Outside, SavedRoot));

	const TSharedPtr<FJsonObject> Metric =
		FProductionJobRuntime::SummarizeMetric(
			TArray<double>{1.0, 2.0, 3.0, 4.0, 5.0},
			3.5);
	TestTrue(TEXT("Metric is available"), Metric->GetBoolField(TEXT("available")));
	TestEqual(TEXT("Metric sample count"), Metric->GetIntegerField(TEXT("sampleCount")), 5);
	TestEqual(TEXT("Metric p50"), Metric->GetNumberField(TEXT("p50")), 3.0);
	TestEqual(TEXT("Metric over-budget count"), Metric->GetIntegerField(TEXT("overBudgetFrames")), 2);

	const TSharedPtr<FJsonObject> NonFiniteMetric =
		FProductionJobRuntime::SummarizeMetric(
			TArray<double>{
				1.0,
				std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::quiet_NaN(),
				2.0
			},
			1.5);
	TestTrue(
		TEXT("Finite samples keep the metric available"),
		NonFiniteMetric->GetBoolField(TEXT("available")));
	TestEqual(
		TEXT("Non-finite samples are excluded"),
		NonFiniteMetric->GetIntegerField(TEXT("sampleCount")),
		2);
	TestEqual(
		TEXT("Non-finite samples are counted"),
		NonFiniteMetric->GetIntegerField(TEXT("invalidSampleCount")),
		2);
	TestTrue(
		TEXT("Summaries never publish a non-finite maximum"),
		FMath::IsFinite(
			NonFiniteMetric->GetNumberField(TEXT("max"))));
	TestTrue(
		TEXT("Summaries never publish a non-finite mean"),
		FMath::IsFinite(
			NonFiniteMetric->GetNumberField(TEXT("mean"))));

	const double Extreme = TNumericLimits<double>::Max();
	const TSharedPtr<FJsonObject> ExtremeMetric =
		FProductionJobRuntime::SummarizeMetric(
			TArray<double>{-Extreme, Extreme, Extreme},
			std::numeric_limits<double>::quiet_NaN());
	TestTrue(
		TEXT("Extreme finite samples keep the mean finite"),
		FMath::IsFinite(
			ExtremeMetric->GetNumberField(TEXT("mean"))));
	TestTrue(
		TEXT("Extreme finite samples keep percentiles finite"),
		FMath::IsFinite(
			ExtremeMetric->GetNumberField(TEXT("p50")))
			&& FMath::IsFinite(
				ExtremeMetric->GetNumberField(TEXT("p95")))
			&& FMath::IsFinite(
				ExtremeMetric->GetNumberField(TEXT("p99"))));
	TestTrue(
		TEXT("Non-finite budgets are sanitized"),
		ExtremeMetric->GetBoolField(TEXT("budgetSanitized")));
	FString ExtremeMetricJson;
	const TSharedRef<TJsonWriter<>> ExtremeMetricWriter =
		TJsonWriterFactory<>::Create(&ExtremeMetricJson);
	TestTrue(
		TEXT("Extreme metric serializes"),
		FJsonSerializer::Serialize(
			ExtremeMetric.ToSharedRef(),
			ExtremeMetricWriter));
	TestFalse(
		TEXT("Serialized metrics never contain Infinity tokens"),
		ExtremeMetricJson.Contains(
			TEXT("inf"),
			ESearchCase::IgnoreCase));
	TestFalse(
		TEXT("Serialized metrics never contain NaN tokens"),
		ExtremeMetricJson.Contains(
			TEXT("nan"),
			ESearchCase::IgnoreCase));
	TSharedPtr<FJsonObject> ParsedExtremeMetric;
	const TSharedRef<TJsonReader<>> ExtremeMetricReader =
		TJsonReaderFactory<>::Create(ExtremeMetricJson);
	TestTrue(
		TEXT("Serialized extreme metric is valid JSON"),
		FJsonSerializer::Deserialize(
			ExtremeMetricReader,
			ParsedExtremeMetric)
			&& ParsedExtremeMetric.IsValid());

	const TSharedPtr<FJsonObject> AllInvalidMetric =
		FProductionJobRuntime::SummarizeMetric(
			TArray<double>{
				std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::quiet_NaN()
			},
			16.6667);
	TestFalse(
		TEXT("All-invalid metrics are unavailable"),
		AllInvalidMetric->GetBoolField(TEXT("available")));
	TestEqual(
		TEXT("All-invalid metrics retain the invalid count"),
		AllInvalidMetric->GetIntegerField(TEXT("invalidSampleCount")),
		2);

	const TSharedPtr<FJsonObject> EmptyMetric =
		FProductionJobRuntime::SummarizeMetric(TArray<double>(), 16.6667);
	TestFalse(TEXT("Empty metric is unavailable"), EmptyMetric->GetBoolField(TEXT("available")));
	TestTrue(
		TEXT("Zero is a valid performance sample"),
		FProductionJobRuntime::IsPerformanceSampleValid(0.0));
	TestFalse(
		TEXT("Negative performance samples are rejected at ingestion"),
		FProductionJobRuntime::IsPerformanceSampleValid(-0.001));
	TestFalse(
		TEXT("Infinite performance samples are rejected at ingestion"),
		FProductionJobRuntime::IsPerformanceSampleValid(
			std::numeric_limits<double>::infinity()));
	TestFalse(
		TEXT("NaN performance samples are rejected at ingestion"),
		FProductionJobRuntime::IsPerformanceSampleValid(
			std::numeric_limits<double>::quiet_NaN()));

	TSharedPtr<FJsonObject> CompareParams = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Checks;
	TSharedPtr<FJsonObject> FrameCheck = MakeShared<FJsonObject>();
	FrameCheck->SetStringField(TEXT("metric"), TEXT("frameMs"));
	FrameCheck->SetStringField(TEXT("statistic"), TEXT("p95"));
	FrameCheck->SetNumberField(TEXT("maxRegressionPercent"), 5.0);
	Checks.Add(MakeShared<FJsonValueObject>(FrameCheck));
	TSharedPtr<FJsonObject> GameCheck = MakeShared<FJsonObject>();
	GameCheck->SetStringField(TEXT("metric"), TEXT("gameMs"));
	GameCheck->SetStringField(TEXT("statistic"), TEXT("p95"));
	GameCheck->SetNumberField(TEXT("maxRegressionPercent"), 10.0);
	Checks.Add(MakeShared<FJsonValueObject>(GameCheck));
	CompareParams->SetArrayField(TEXT("checks"), Checks);
	const TSharedPtr<FJsonObject> Comparison =
		FProductionJobRuntime::ComparePerformanceResults(
			MakePerformanceResult(10.0, 8.0),
			MakePerformanceResult(12.0, 8.4),
			CompareParams);
	TestEqual(
		TEXT("Any failed threshold produces a regression verdict"),
		Comparison->GetStringField(TEXT("verdict")),
		FString(TEXT("regression")));
	TestEqual(
		TEXT("Every requested threshold is reported"),
		Comparison->GetArrayField(TEXT("checks")).Num(),
		2);
	TestEqual(
		TEXT("Passing checks remain distinguishable"),
		Comparison->GetArrayField(TEXT("checks"))[1]
			->AsObject()
			->GetStringField(TEXT("verdict")),
		FString(TEXT("pass")));

	TSharedPtr<FJsonObject> ExtremeCompareParams = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ExtremeCheck = MakeShared<FJsonObject>();
	ExtremeCheck->SetStringField(TEXT("metric"), TEXT("frameMs"));
	ExtremeCheck->SetStringField(TEXT("statistic"), TEXT("p95"));
	ExtremeCheck->SetNumberField(TEXT("maxRegressionPercent"), 5.0);
	ExtremeCompareParams->SetArrayField(
		TEXT("checks"),
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueObject>(ExtremeCheck)
		});
	const TSharedPtr<FJsonObject> ExtremeComparison =
		FProductionJobRuntime::ComparePerformanceResults(
			MakePerformanceResult(SMALL_NUMBER * 2.0, 1.0),
			MakePerformanceResult(
				TNumericLimits<double>::Max() / 2.0,
				1.0),
			ExtremeCompareParams);
	const TSharedPtr<FJsonObject> ExtremeComparisonCheck =
		ExtremeComparison->GetArrayField(TEXT("checks"))[0]->AsObject();
	TestTrue(
		TEXT("Extreme regression percentages remain finite"),
		FMath::IsFinite(
			ExtremeComparisonCheck->GetNumberField(
				TEXT("regressionPercent"))));
	TestTrue(
		TEXT("Extreme regression percentages disclose saturation"),
		ExtremeComparisonCheck->GetBoolField(
			TEXT("regressionPercentSaturated")));
	FString ExtremeComparisonJson;
	const TSharedRef<TJsonWriter<>> ExtremeComparisonWriter =
		TJsonWriterFactory<>::Create(&ExtremeComparisonJson);
	TestTrue(
		TEXT("Extreme comparison serializes as valid JSON"),
		FJsonSerializer::Serialize(
			ExtremeComparison.ToSharedRef(),
			ExtremeComparisonWriter));
	TestFalse(
		TEXT("Extreme comparison JSON contains no Infinity token"),
		ExtremeComparisonJson.Contains(
			TEXT("inf"),
			ESearchCase::IgnoreCase));

	ExtremeCheck->SetNumberField(
		TEXT("maxRegressionPercent"),
		std::numeric_limits<double>::quiet_NaN());
	const TSharedPtr<FJsonObject> InvalidThresholdComparison =
		FProductionJobRuntime::ComparePerformanceResults(
			MakePerformanceResult(10.0, 8.0),
			MakePerformanceResult(12.0, 8.0),
			ExtremeCompareParams);
	TestEqual(
		TEXT("Non-finite thresholds are inconclusive"),
		InvalidThresholdComparison->GetStringField(TEXT("verdict")),
		FString(TEXT("inconclusive")));

	const TSharedPtr<FJsonObject> Incompatible =
		FProductionJobRuntime::ComparePerformanceResults(
			MakePerformanceResult(10.0, 8.0),
			MakePerformanceResult(10.0, 8.0, TEXT("OtherMap")),
			CompareParams);
	TestEqual(
		TEXT("Environment drift makes comparisons inconclusive"),
		Incompatible->GetStringField(TEXT("verdict")),
		FString(TEXT("inconclusive")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionJobLifecycleContractTest,
	"UE_AI_integration.Production.JobRuntime.LifecycleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionJobLifecycleContractTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<FJsonObject> TimeoutParams = MakeShared<FJsonObject>();
	TimeoutParams->SetNumberField(TEXT("startupTimeoutSeconds"), 120.0);
	TimeoutParams->SetNumberField(TEXT("executionTimeoutSeconds"), 15.0);
	TimeoutParams->SetNumberField(TEXT("shutdownTimeoutSeconds"), 5.0);
	TimeoutParams->SetNumberField(TEXT("hardTimeoutSeconds"), 10.0);
	const TSharedPtr<FJsonObject> Policy =
		FProductionJobRuntime::ResolveProcessTimeoutPolicy(
			TimeoutParams,
			1800.0);
	TestEqual(
		TEXT("Startup timeout is independent"),
		Policy->GetNumberField(TEXT("startupTimeoutSeconds")),
		120.0);
	TestEqual(
		TEXT("Resolved startup timeout is forwarded as a child argument"),
		FProductionJobRuntime::BuildStandaloneStartupTimeoutArgument(
			Policy->GetNumberField(TEXT("startupTimeoutSeconds"))),
		FString(TEXT("-UEAIPerfStartupTimeoutSeconds=120")));
	TestEqual(
		TEXT("Child startup timeout argument keeps the job policy upper bound"),
		FProductionJobRuntime::BuildStandaloneStartupTimeoutArgument(7200.0),
		FString(TEXT("-UEAIPerfStartupTimeoutSeconds=3600")));
	TestEqual(
		TEXT("Execution timeout is independent"),
		Policy->GetNumberField(TEXT("executionTimeoutSeconds")),
		15.0);
	TestEqual(
		TEXT("Shutdown timeout is independent"),
		Policy->GetNumberField(TEXT("shutdownTimeoutSeconds")),
		5.0);
	TestEqual(
		TEXT("Hard timeout can terminate before phase budgets"),
		Policy->GetNumberField(TEXT("hardTimeoutSeconds")),
		10.0);
	TestTrue(
		TEXT("Policy reports when the hard timeout is limiting"),
		Policy->GetBoolField(TEXT("hardTimeoutIsLimiting")));

	TSharedPtr<FJsonObject> LegacyParams = MakeShared<FJsonObject>();
	LegacyParams->SetNumberField(TEXT("timeoutSeconds"), 321.0);
	const TSharedPtr<FJsonObject> LegacyPolicy =
		FProductionJobRuntime::ResolveProcessTimeoutPolicy(
			LegacyParams,
			1800.0);
	TestEqual(
		TEXT("Legacy timeout remains the execution timeout"),
		LegacyPolicy->GetNumberField(TEXT("executionTimeoutSeconds")),
		321.0);
	TestEqual(
		TEXT("Hard timeout has an absolute 24 hour limit"),
		LegacyPolicy->GetNumberField(TEXT("hardLimitSeconds")),
		86400.0);

	FString ProfileError;
	const FString MinimalProfile =
		FProductionJobRuntime::BuildHeadlessProfileArguments(
			TEXT("minimal"),
			ProfileError);
	TestTrue(TEXT("Minimal profile is accepted"), ProfileError.IsEmpty());
	TestTrue(
		TEXT("Minimal profile disables audio"),
		MinimalProfile.Contains(TEXT("-NoSound")));
	TestTrue(
		TEXT("Minimal profile disables OpenXR"),
		MinimalProfile.Contains(TEXT("-DisablePlugins=OpenXR")));
	TestTrue(
		TEXT("Minimal profile disables unrelated source-control plugins"),
		MinimalProfile.Contains(TEXT("PerforceSourceControl")));
	TestFalse(
		TEXT("Minimal profile preserves required Engine plugin dependencies"),
		MinimalProfile.Contains(TEXT("-NoEnginePlugins")));
	TestTrue(
		TEXT("Minimal profile uses NullRHI"),
		MinimalProfile.Contains(TEXT("-NullRHI")));

	const FString RejectedProfile =
		FProductionJobRuntime::BuildHeadlessProfileArguments(
			TEXT("minimal -ExecCmds=Quit"),
			ProfileError);
	TestTrue(
		TEXT("Unknown profile cannot inject raw arguments"),
		RejectedProfile.IsEmpty() && !ProfileError.IsEmpty());

	FString Phase = TEXT("loading");
	Phase = FProductionJobRuntime::InferAutomationPhase(
		Phase,
		TEXT("LogAutomationCommandLine: Ready to start automation"));
	TestEqual(
		TEXT("Automation readiness enters discovery"),
		Phase,
		FString(TEXT("discovering")));
	Phase = FProductionJobRuntime::InferAutomationPhase(
		Phase,
		TEXT("LogAutomationController: Display: Test Started."));
	TestEqual(
		TEXT("First real test enters execution"),
		Phase,
		FString(TEXT("running")));
	Phase = FProductionJobRuntime::InferAutomationPhase(
		Phase,
		TEXT("Automation Test Queue Empty"));
	TestEqual(
		TEXT("Queue completion enters reporting"),
		Phase,
		FString(TEXT("reporting")));
	Phase = FProductionJobRuntime::InferAutomationPhase(
		Phase,
		TEXT("Successfully wrote html results file"));
	TestEqual(
		TEXT("Completed report enters bounded Editor shutdown"),
		Phase,
		FString(TEXT("exiting")));
	Phase = FProductionJobRuntime::InferAutomationPhase(
		Phase,
		TEXT("LogExit: Exiting."));
	TestEqual(
		TEXT("Engine exit enters shutdown"),
		Phase,
		FString(TEXT("exiting")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionStandaloneChildReceiptContractTest,
	"UE_AI_integration.Production.Performance.StandaloneChildReceiptContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionStandaloneChildReceiptContractTest::RunTest(
	const FString& Parameters)
{
	const FString JobId = TEXT("job-receipt-contract");
	const int32 ExpectedRepeatCount = 2;
	TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(
		TEXT("schema"),
		TEXT("ue.performance-standalone-child.v1"));
	Receipt->SetStringField(TEXT("jobId"), JobId);
	Receipt->SetStringField(TEXT("status"), TEXT("succeeded"));
	Receipt->SetNumberField(
		TEXT("completedRepeatCount"),
		ExpectedRepeatCount);
	Receipt->SetArrayField(
		TEXT("csvFiles"),
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueString>(TEXT("repeat-1.csv")),
			MakeShared<FJsonValueString>(TEXT("repeat-2.csv"))
		});
	TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();
	Camera->SetBoolField(TEXT("lockedAndVerified"), true);
	Receipt->SetObjectField(TEXT("camera"), Camera);

	FString ReceiptError;
	TestTrue(
		TEXT("A complete successful child receipt is authoritative"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			Receipt,
			JobId,
			ExpectedRepeatCount,
			ReceiptError));
	TestTrue(
		TEXT("A valid successful receipt has no validation error"),
		ReceiptError.IsEmpty());

	const FString OverflowReceiptJson =
		TEXT(
			"{\"schema\":\"ue.performance-standalone-child.v1\","
			"\"jobId\":\"job-receipt-contract\","
			"\"status\":\"succeeded\","
			"\"completedRepeatCount\":2,"
			"\"csvFiles\":[\"repeat-1.csv\",\"repeat-2.csv\"],"
			"\"camera\":{\"lockedAndVerified\":true},"
			"\"runtimeFingerprint\":{\"nested\":{\"overflow\":1e999}}}");
	TSharedPtr<FJsonObject> OverflowReceipt;
	const TSharedRef<TJsonReader<>> OverflowReceiptReader =
		TJsonReaderFactory<>::Create(OverflowReceiptJson);
	TestTrue(
		TEXT("UE JSON reader accepts the exponent-overflow fixture"),
		FJsonSerializer::Deserialize(
			OverflowReceiptReader,
			OverflowReceipt)
			&& OverflowReceipt.IsValid());
	if (OverflowReceipt.IsValid())
	{
		TestFalse(
			TEXT("Deep exponent overflow is rejected at the receipt boundary"),
			FProductionJobRuntime::ValidateStandaloneChildReceipt(
				OverflowReceipt,
				JobId,
				ExpectedRepeatCount,
				ReceiptError));
		TestTrue(
			TEXT("Receipt validation identifies the unsafe numeric path"),
			ReceiptError.Contains(TEXT("runtimeFingerprint.nested.overflow")));
	}

	TestFalse(
		TEXT("A receipt from another job is rejected"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			Receipt,
			TEXT("job-other"),
			ExpectedRepeatCount,
			ReceiptError));
	TestFalse(
		TEXT("A mismatched completed repeat count is rejected"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			Receipt,
			JobId,
			ExpectedRepeatCount + 1,
			ReceiptError));

	Receipt->SetArrayField(
		TEXT("csvFiles"),
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueString>(TEXT("repeat-1.csv"))
		});
	TestFalse(
		TEXT("A mismatched CSV count is rejected"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			Receipt,
			JobId,
			ExpectedRepeatCount,
			ReceiptError));
	Receipt->SetArrayField(
		TEXT("csvFiles"),
		TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueString>(TEXT("repeat-1.csv")),
			MakeShared<FJsonValueString>(TEXT("repeat-2.csv"))
		});

	Camera->SetBoolField(TEXT("lockedAndVerified"), false);
	TestFalse(
		TEXT("A successful receipt without a verified camera is rejected"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			Receipt,
			JobId,
			ExpectedRepeatCount,
			ReceiptError));

	TSharedPtr<FJsonObject> FailedReceipt = MakeShared<FJsonObject>();
	FailedReceipt->SetStringField(
		TEXT("schema"),
		TEXT("ue.performance-standalone-child.v1"));
	FailedReceipt->SetStringField(TEXT("jobId"), JobId);
	FailedReceipt->SetStringField(TEXT("status"), TEXT("failed"));
	FailedReceipt->SetStringField(
		TEXT("errorCode"),
		TEXT("standalone_child_interrupted"));
	FailedReceipt->SetStringField(
		TEXT("message"),
		TEXT("The managed standalone child was interrupted."));
	TestTrue(
		TEXT("A failed terminal receipt with diagnostics is valid"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			FailedReceipt,
			JobId,
			ExpectedRepeatCount,
			ReceiptError));
	TestTrue(
		TEXT("A valid failed terminal receipt has no validation error"),
		ReceiptError.IsEmpty());
	FailedReceipt->SetStringField(TEXT("errorCode"), TEXT(""));
	TestFalse(
		TEXT("A failed terminal receipt without diagnostics is rejected"),
		FProductionJobRuntime::ValidateStandaloneChildReceipt(
			FailedReceipt,
			JobId,
			ExpectedRepeatCount,
			ReceiptError));
	TestTrue(
		TEXT("A normal zero exit may adopt a verified child terminal"),
		FProductionJobRuntime::CanStandaloneChildReceiptOverrideTerminal(
			0,
			FString()));
	TestTrue(
		TEXT("A generic non-zero process exit may adopt a verified child terminal"),
		FProductionJobRuntime::CanStandaloneChildReceiptOverrideTerminal(
			3,
			TEXT("process_failed")));
	TestFalse(
		TEXT("A cancellation remains authoritative over a child receipt"),
		FProductionJobRuntime::CanStandaloneChildReceiptOverrideTerminal(
			3,
			TEXT("job_cancelled")));
	TestFalse(
		TEXT("A timeout remains authoritative over a child receipt"),
		FProductionJobRuntime::CanStandaloneChildReceiptOverrideTerminal(
			3,
			TEXT("job_execution_timeout")));
	TestFalse(
		TEXT("A controller terminal without a process exit cannot be overridden"),
		FProductionJobRuntime::CanStandaloneChildReceiptOverrideTerminal(
			INDEX_NONE,
			FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionPerformanceScenarioWindowTest,
	"UE_AI_integration.Production.Performance.ScenarioMeasurementWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionPerformanceScenarioWindowTest::RunTest(
	const FString& Parameters)
{
	int32 ScenarioStartCount = 0;
	bool bMetricsActive = true;
	int32 MetricsBeginCount = 1;
	int32 MetricsEndCount = 0;
	FString ScenarioStatus = TEXT("running");

	auto StartScenario =
		[&](
			const TSharedPtr<FJsonObject>& ScenarioParams)
			-> FMCPToolResult
		{
			++ScenarioStartCount;
			bMetricsActive = true;
			MetricsBeginCount = 1;
			MetricsEndCount = 0;
			ScenarioStatus = TEXT("running");
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(
				TEXT("runId"),
				FString::Printf(
					TEXT("scenario-%d"),
					ScenarioStartCount));
			return FMCPToolResult::Ok(Data);
		};
	auto GetScenarioStatus =
		[&](
			const TSharedPtr<FJsonObject>& StatusParams)
			-> FMCPToolResult
		{
			TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
			Metrics->SetBoolField(TEXT("active"), bMetricsActive);
			Metrics->SetNumberField(
				TEXT("beginCount"),
				MetricsBeginCount);
			Metrics->SetNumberField(TEXT("endCount"), MetricsEndCount);
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("status"), ScenarioStatus);
			Data->SetNumberField(TEXT("currentStep"), bMetricsActive ? 1 : 2);
			Data->SetNumberField(TEXT("stepCount"), 2);
			Data->SetObjectField(TEXT("metrics"), Metrics);
			return FMCPToolResult::Ok(Data);
		};
	auto GetScenarioResult =
		[](
			const TSharedPtr<FJsonObject>& ResultParams)
			-> FMCPToolResult
		{
			TSharedPtr<FJsonObject> LogWindow = MakeShared<FJsonObject>();
			LogWindow->SetBoolField(TEXT("available"), true);
			LogWindow->SetNumberField(TEXT("startCursor"), 100);
			LogWindow->SetNumberField(TEXT("endCursor"), 200);
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("logWindow"), LogWindow);
			return FMCPToolResult::Ok(Data);
		};
	auto CancelScenario =
		[](
			const TSharedPtr<FJsonObject>& CancelParams)
			-> FMCPToolResult
		{
			return FMCPToolResult::Ok(MakeShared<FJsonObject>());
		};

	FProductionJobRuntime Runtime(
		MoveTemp(StartScenario),
		MoveTemp(GetScenarioStatus),
		MoveTemp(GetScenarioResult),
		MoveTemp(CancelScenario));
	TSharedPtr<FJsonObject> Scenario = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Steps;
	for (const TCHAR* Action : {
		TEXT("metrics.begin"),
		TEXT("metrics.end")})
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("action"), Action);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
	}
	Scenario->SetArrayField(TEXT("steps"), Steps);
	TSharedPtr<FJsonObject> RunParams = MakeShared<FJsonObject>();
	RunParams->SetStringField(TEXT("mode"), TEXT("scenario"));
	RunParams->SetObjectField(TEXT("scenario"), Scenario);
	RunParams->SetNumberField(TEXT("repeatCount"), 2);
	RunParams->SetNumberField(TEXT("warmupSeconds"), 0);
	RunParams->SetNumberField(TEXT("sampleSeconds"), 1);

	const FMCPToolResult Started = Runtime.Execute(
		TEXT("production.performance.run"),
		RunParams);
	if (!TestTrue(TEXT("Scenario performance run starts"), Started.bSuccess))
	{
		return false;
	}
	const FString RunId = Started.Data->GetStringField(TEXT("runId"));

	for (int32 RepeatIndex = 0; RepeatIndex < 2; ++RepeatIndex)
	{
		Runtime.Tick(0.0f);
		bMetricsActive = false;
		MetricsEndCount = 1;
		ScenarioStatus = TEXT("succeeded");
		Runtime.Tick(0.0f);
	}
	TestEqual(
		TEXT("Each performance repetition starts one Scenario"),
		ScenarioStartCount,
		2);

	TSharedPtr<FJsonObject> ResultParams = MakeShared<FJsonObject>();
	ResultParams->SetStringField(TEXT("runId"), RunId);
	const FMCPToolResult Result = Runtime.Execute(
		TEXT("production.performance.result.get"),
		ResultParams);
	if (!TestTrue(TEXT("Scenario performance result is available"), Result.bSuccess))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Performance =
		Result.Data->GetObjectField(TEXT("result"));
	TestEqual(
		TEXT("Both repetitions are retained"),
		Performance->GetArrayField(TEXT("repetitions")).Num(),
		2);
	TestEqual(
		TEXT("Each repetition retains its run-bounded log window"),
		Performance->GetArrayField(TEXT("logWindows")).Num(),
		2);
	TestEqual(
		TEXT("Only active marker windows contribute frame samples"),
		Performance->GetIntegerField(TEXT("sampleCount")),
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionCapabilityAdmissionTest,
	"UE_AI_integration.Production.JobRuntime.ManifestAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionCapabilityAdmissionTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	if (!TestTrue(TEXT("Plugin is available"), Plugin.IsValid()))
	{
		return false;
	}
	const FString ManifestPath =
		FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Resources/Capabilities/production.json"));
	FString Json;
	if (!TestTrue(
			TEXT("Production manifest is readable"),
			FFileHelper::LoadFileToString(Json, *ManifestPath)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Json);
	if (!TestTrue(
			TEXT("Production manifest is valid JSON"),
			FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()))
	{
		return false;
	}

	static const TSet<FString> ExpectedJobOperations = {
		TEXT("production.job.status"),
		TEXT("production.job.cancel"),
		TEXT("production.job.result.get"),
		TEXT("production.job.log.get"),
		TEXT("production.job.artifact.get"),
		TEXT("production.trace.start"),
		TEXT("production.trace.status"),
		TEXT("production.trace.stop"),
		TEXT("production.trace.analyze"),
		TEXT("production.trace.target.list"),
		TEXT("production.trace.launch.plan"),
		TEXT("production.trace.channel.list"),
		TEXT("production.trace.import"),
		TEXT("production.trace.provider.list"),
		TEXT("production.trace.timing.query"),
		TEXT("production.trace.counter.query"),
		TEXT("production.trace.memory.query"),
		TEXT("production.trace.loading.query"),
		TEXT("production.trace.network.query"),
		TEXT("production.trace.tasks.query"),
		TEXT("production.trace.context_switches.query"),
		TEXT("production.trace.log.query"),
		TEXT("production.trace.io.query"),
		TEXT("production.trace.bookmark.query"),
		TEXT("production.trace.region.query"),
		TEXT("production.trace.screenshot.query"),
		TEXT("production.trace.export"),
		TEXT("production.trace.open_in_insights"),
		TEXT("production.performance.run"),
		TEXT("production.performance.result.get"),
		TEXT("production.performance.compare"),
		TEXT("production.test.list"),
		TEXT("production.test.run"),
		TEXT("production.test.result.get"),
		TEXT("production.source_control.repository.get"),
		TEXT("production.source_control.status"),
		TEXT("production.source_control.diff"),
		TEXT("production.source_control.change.plan"),
		TEXT("production.source_control.change.execute"),
		TEXT("production.ddc.status"),
		TEXT("production.ddc.job.start"),
		TEXT("production.buildgraph.validate"),
		TEXT("production.buildgraph.run"),
		TEXT("production.horde.context.get"),
	};
	TSet<FString> Found;
	TMap<FString, TSharedPtr<FJsonObject>> CapabilitiesById;
	for (const TSharedPtr<FJsonValue>& Value :
		Root->GetArrayField(TEXT("capabilities")))
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString Id = Capability->GetStringField(TEXT("id"));
		CapabilitiesById.Add(Id, Capability);
		if (!ExpectedJobOperations.Contains(Id))
		{
			continue;
		}
		Found.Add(Id);
		if (Capability->HasTypedField<EJson::Object>(TEXT("dsl")))
		{
			const FString ExpectedAdmission =
				Id == TEXT("production.trace.open_in_insights")
					? TEXT("interactiveOnly")
					: TEXT("none");
			TestEqual(
				*FString::Printf(TEXT("%s has the expected Workflow admission"), *Id),
				Capability->GetObjectField(TEXT("dsl"))
					->GetStringField(TEXT("admission")),
				ExpectedAdmission);
		}
		else
		{
			AddError(
				FString::Printf(
					TEXT("%s must explicitly declare Workflow admission none."),
					*Id));
		}
	}
	TestEqual(
		TEXT("All production job operations are declared"),
		Found.Num(),
		ExpectedJobOperations.Num());
	TestTrue(
		TEXT("Production manifest preserves the shipped job baseline"),
		Root->GetArrayField(TEXT("capabilities")).Num() >= 49);

	const TSharedPtr<FJsonObject> TraceAnalyze =
		CapabilitiesById.FindRef(TEXT("production.trace.analyze"));
	TestTrue(
		TEXT("Trace analysis exposes bounded provider controls"),
		TraceAnalyze.IsValid()
			&& TraceAnalyze->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("maxTimers"))
			&& TraceAnalyze->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("maxCounterValues")));
	const TSharedPtr<FJsonObject> TimingQuery =
		CapabilitiesById.FindRef(TEXT("production.trace.timing.query"));
	TestTrue(
		TEXT("Trace semantic queries prefer the local Trace Worker"),
		TimingQuery.IsValid()
			&& TimingQuery->GetObjectField(TEXT("execution"))
				->GetStringField(TEXT("preferred")) == TEXT("localTrace")
			&& TimingQuery->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("cursor")));

	const TSharedPtr<FJsonObject> PerformanceRun =
		CapabilitiesById.FindRef(TEXT("production.performance.run"));
	TestTrue(
		TEXT("Performance runs expose window and Scenario controls"),
		PerformanceRun.IsValid()
			&& PerformanceRun->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("mode"))
			&& PerformanceRun->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("scenario"))
			&& PerformanceRun->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("repeatCount")));
	if (PerformanceRun.IsValid())
	{
		const TSharedPtr<FJsonObject> PerformanceProperties =
			PerformanceRun->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"));
		const TSharedPtr<FJsonObject> StartupTimeout =
			PerformanceProperties->GetObjectField(
				TEXT("startupTimeoutSeconds"));
		TestEqual(
			TEXT("Standalone startup timeout uses the runtime upper bound"),
			StartupTimeout->GetNumberField(TEXT("maximum")),
			3600.0);
		const TSharedPtr<FJsonObject> CameraName =
			PerformanceProperties->GetObjectField(TEXT("standardProfile"))
				->GetObjectField(TEXT("properties"))
				->GetObjectField(TEXT("camera"))
				->GetObjectField(TEXT("properties"))
				->GetObjectField(TEXT("name"));
		TestEqual(
			TEXT("Camera name manifest bound matches runtime"),
			CameraName->GetNumberField(TEXT("maxLength")),
			128.0);
	}

	for (const FString ProcessCapabilityId : {
		FString(TEXT("production.commandlet.run")),
		FString(TEXT("production.project.cook")),
		FString(TEXT("production.project.package")),
		FString(TEXT("production.test.run")),
		FString(TEXT("production.source_control.change.execute")),
		FString(TEXT("production.ddc.job.start")),
		FString(TEXT("production.buildgraph.run"))})
	{
		const TSharedPtr<FJsonObject> ProcessCapability =
			CapabilitiesById.FindRef(ProcessCapabilityId);
		if (!TestTrue(
				*FString::Printf(
					TEXT("%s exists"),
					*ProcessCapabilityId),
				ProcessCapability.IsValid()))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Properties =
			ProcessCapability->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"));
		for (const FString TimeoutField : {
			FString(TEXT("startupTimeoutSeconds")),
			FString(TEXT("executionTimeoutSeconds")),
			FString(TEXT("shutdownTimeoutSeconds")),
			FString(TEXT("hardTimeoutSeconds"))})
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s exposes %s"),
					*ProcessCapabilityId,
					*TimeoutField),
				Properties->HasField(TimeoutField));
		}
	}
	const TSharedPtr<FJsonObject> TestRun =
		CapabilitiesById.FindRef(TEXT("production.test.run"));
	TestTrue(
		TEXT("Automation runs expose a controlled headless profile"),
		TestRun.IsValid()
			&& TestRun->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("headlessProfile")));

	const TSharedPtr<FJsonObject> PerformanceCompare =
		CapabilitiesById.FindRef(TEXT("production.performance.compare"));
	TestTrue(
		TEXT("Performance comparison exposes multi-threshold checks"),
		PerformanceCompare.IsValid()
			&& PerformanceCompare->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->HasField(TEXT("checks")));

	for (const FString ScenarioCapabilityId : {
		FString(TEXT("production.scenario.validate")),
		FString(TEXT("production.scenario.start"))})
	{
		const TSharedPtr<FJsonObject> ScenarioCapability =
			CapabilitiesById.FindRef(ScenarioCapabilityId);
		if (!TestTrue(
				*FString::Printf(
					TEXT("%s exists"),
					*ScenarioCapabilityId),
				ScenarioCapability.IsValid()))
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>& Actions =
			ScenarioCapability->GetObjectField(TEXT("inputSchema"))
				->GetObjectField(TEXT("properties"))
				->GetObjectField(TEXT("scenario"))
				->GetObjectField(TEXT("properties"))
				->GetObjectField(TEXT("steps"))
				->GetObjectField(TEXT("items"))
				->GetObjectField(TEXT("properties"))
				->GetObjectField(TEXT("action"))
				->GetArrayField(TEXT("enum"));
		TSet<FString> ActionNames;
		for (const TSharedPtr<FJsonValue>& Action : Actions)
		{
			ActionNames.Add(Action->AsString());
		}
		TestTrue(
			*FString::Printf(
				TEXT("%s admits metrics.begin"),
				*ScenarioCapabilityId),
			ActionNames.Contains(TEXT("metrics.begin")));
		TestTrue(
			*FString::Printf(
				TEXT("%s admits metrics.end"),
				*ScenarioCapabilityId),
			ActionNames.Contains(TEXT("metrics.end")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTracePresetAndPIEBindingContractTest,
	"UE_AI_integration.Production.Trace.PresetAndPIEBindingContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTracePresetAndPIEBindingContractTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Standard =
		UEAI::Trace::GetTracePresetChannels(TEXT("standard"));
	TestTrue(TEXT("standard records managed regions"), Standard.Contains(TEXT("region")));
	TestTrue(TEXT("standard records counters"), Standard.Contains(TEXT("counter")));
	const TArray<FString> Memory =
		UEAI::Trace::GetTracePresetChannels(TEXT("memory"));
	const TArray<FString> MemoryRequired = {
		TEXT("memory"), TEXT("memalloc"), TEXT("module"),
		TEXT("callstack"), TEXT("region")};
	for (const FString& Channel : MemoryRequired)
	{
		TestTrue(
			*FString::Printf(TEXT("memory records %s"), *Channel),
			Memory.Contains(Channel));
	}
	const TArray<FString> Full =
		UEAI::Trace::GetTracePresetChannels(TEXT("fullInsights"));
	const TArray<FString> FullRequired = {
		TEXT("counter"), TEXT("module"), TEXT("callstack"),
		TEXT("screenshot"), TEXT("region")};
	for (const FString& Channel : FullRequired)
	{
		TestTrue(
			*FString::Printf(TEXT("fullInsights records %s"), *Channel),
			Full.Contains(Channel));
	}
	TestEqual(
		TEXT("full post-stop timing is deeper than summary"),
		UEAI::Trace::GetTracePostStopOperations(TEXT("timing"), true).Num(),
		3);
	TestEqual(
		TEXT("summary post-stop timing stays bounded"),
		UEAI::Trace::GetTracePostStopOperations(TEXT("timing"), false).Num(),
		1);
	FString NetworkStartErrorCode;
	FString NetworkStartErrorMessage;
	bool bNetworkPartial = false;
	FString NetworkWarningCode;
	TestTrue(
		TEXT("PIE network trace admits bounded metadata replay by default"),
		FProductionJobRuntime::ValidatePIENetworkTraceStart(
			true,
			true,
			false,
			bNetworkPartial,
			NetworkWarningCode,
			NetworkStartErrorCode,
			NetworkStartErrorMessage));
	TestTrue(
		TEXT("A PIE network session predating Trace is explicitly partial"),
		bNetworkPartial);
	TestEqual(
		TEXT("Existing PIE network state has a stable warning"),
		NetworkWarningCode,
		FString(TEXT("network_session_predates_trace")));
	TestTrue(
		TEXT("Strict complete-network mode fails closed"),
		!FProductionJobRuntime::ValidatePIENetworkTraceStart(
			true,
			true,
			true,
			bNetworkPartial,
			NetworkWarningCode,
			NetworkStartErrorCode,
			NetworkStartErrorMessage));
	TestEqual(
		TEXT("Strict complete-network mode has a stable diagnostic"),
		NetworkStartErrorCode,
		FString(TEXT("trace_network_session_predates_trace")));
	TestTrue(
		TEXT("The diagnostic directs callers to record before NetDriver creation"),
		NetworkStartErrorMessage.Contains(TEXT("before the NetDriver")));
	TestTrue(
		TEXT("PIE network trace remains admissible before network state exists"),
		FProductionJobRuntime::ValidatePIENetworkTraceStart(
			true,
			false,
			true,
			bNetworkPartial,
			NetworkWarningCode,
			NetworkStartErrorCode,
			NetworkStartErrorMessage));
	TestFalse(
		TEXT("A not-yet-created PIE network session is complete"),
		bNetworkPartial);

	FProductionJobRuntime Runtime(
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FScenarioOperation(),
		[](FString& OutSessionId, uint64& OutGeneration)
		{
			OutSessionId = TEXT("pie-current");
			OutGeneration = 7;
			return true;
		});
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("kind"), TEXT("pie"));
	Target->SetStringField(TEXT("sessionId"), TEXT("pie-stale"));
	Target->SetNumberField(TEXT("generation"), 6);
	TSharedPtr<FJsonObject> Start = MakeShared<FJsonObject>();
	Start->SetObjectField(TEXT("target"), Target);
	const FMCPToolResult Stale = Runtime.Execute(
		TEXT("production.trace.start"), Start);
	TestFalse(TEXT("stale PIE trace target is rejected"), Stale.bSuccess);
	TestEqual(
		TEXT("stale PIE generation has stable error"),
		Stale.ErrorCode,
		FString(TEXT("pie_generation_conflict")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTraceEditorConnectionGateEvidenceTest,
	"UE_AI_integration.Production.Trace.EditorConnectionGateEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTraceEditorConnectionGateEvidenceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const uint32 OriginalNetTraceVerbosity = FNetTrace::GetTraceVerbosity();
	FProductionJobRuntime Runtime(
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FScenarioOperation(),
		FProductionJobRuntime::FPIESessionSnapshot{});

	FString TraceId;
	FString TraceDirectory;
	bool bTraceMayBeActive = false;
	bool bCleanupCompletedEvidence = false;
	ON_SCOPE_EXIT
	{
		if (bTraceMayBeActive && !TraceId.IsEmpty())
		{
			TSharedPtr<FJsonObject> StopParams = MakeShared<FJsonObject>();
			StopParams->SetStringField(TEXT("traceId"), TraceId);
			Runtime.Execute(TEXT("production.trace.stop"), StopParams);
		}
		if (bCleanupCompletedEvidence && !TraceDirectory.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(
				*TraceDirectory,
				false,
				true);
		}
	};

	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("kind"), TEXT("editor"));
	TSharedPtr<FJsonObject> StartParams = MakeShared<FJsonObject>();
	StartParams->SetObjectField(TEXT("target"), Target);
	StartParams->SetArrayField(
		TEXT("channels"),
		{
			MakeShared<FJsonValueString>(TEXT("cpu")),
			MakeShared<FJsonValueString>(TEXT("frame")),
			MakeShared<FJsonValueString>(TEXT("bookmark")),
			MakeShared<FJsonValueString>(TEXT("region")),
			MakeShared<FJsonValueString>(TEXT("net"))
		});
	StartParams->SetStringField(TEXT("postStop"), TEXT("artifactOnly"));
	StartParams->SetNumberField(TEXT("maxDurationSeconds"), 15.0);
	StartParams->SetNumberField(TEXT("maxFileSizeMiB"), 256.0);
	const FMCPToolResult Start = Runtime.Execute(
		TEXT("production.trace.start"),
		StartParams);
	if (!TestTrue(
		TEXT("Editor Trace recording request is accepted"),
		Start.bSuccess && Start.Data.IsValid()))
	{
		AddError(FString::Printf(
			TEXT("Trace start failed with %s: %s"),
			*Start.ErrorCode,
			*Start.ErrorMessage));
		return false;
	}
	if (!TestTrue(
		TEXT("Trace start returns a stable trace id"),
		Start.Data->TryGetStringField(TEXT("traceId"), TraceId)
			&& !TraceId.IsEmpty()))
	{
		return false;
	}
	bTraceMayBeActive = true;
	TraceDirectory = FPaths::Combine(
		FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()),
		TEXT("UEAIIntegration/Jobs"),
		TraceId);

	TestEqual(
		TEXT("Start remains in loading until the Trace OnConnection signal is consumed"),
		Start.Data->GetStringField(TEXT("phase")),
		FString(TEXT("loading")));
	const TArray<TSharedPtr<FJsonValue>>& InitialPhaseHistory =
		Start.Data->GetArrayField(TEXT("phaseHistory"));
	const bool bRecordingWasPublishedAtStart =
		InitialPhaseHistory.ContainsByPredicate(
			[](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid()
					&& Value->Type == EJson::String
					&& Value->AsString() == TEXT("recording");
			});
	TestFalse(
		TEXT("Recording is not published before OnConnection"),
		bRecordingWasPublishedAtStart);

	TSharedPtr<FJsonObject> StatusParams = MakeShared<FJsonObject>();
	StatusParams->SetStringField(TEXT("traceId"), TraceId);
	bool bReachedRecording = false;
	const double ConnectionDeadline = FPlatformTime::Seconds() + 8.0;
	while (FPlatformTime::Seconds() < ConnectionDeadline)
	{
		Runtime.Tick(0.0f);
		const FMCPToolResult Status = Runtime.Execute(
			TEXT("production.trace.status"),
			StatusParams);
		if (!Status.bSuccess || !Status.Data.IsValid())
		{
			AddError(FString::Printf(
				TEXT("Trace status failed with %s: %s"),
				*Status.ErrorCode,
				*Status.ErrorMessage));
			break;
		}
		if (Status.Data->GetStringField(TEXT("phase")) == TEXT("recording"))
		{
			bReachedRecording = true;
			TestTrue(
				TEXT("The Trace file connection is live when recording is published"),
				Status.Data->GetBoolField(TEXT("connected")));
			break;
		}
		if (Status.Data->GetStringField(TEXT("status")) != TEXT("running"))
		{
			break;
		}
		FPlatformProcess::SleepNoStats(0.01f);
	}
	if (!TestTrue(
		TEXT("OnConnection transitions the bounded job to recording"),
		bReachedRecording))
	{
		return false;
	}
	TestTrue(
		TEXT("Net Trace is re-armed only after the managed connection"),
		FNetTrace::IsEnabled());

	// Give the managed region a measurable interval before its end marker.
	FPlatformProcess::SleepNoStats(0.05f);
	Runtime.Tick(0.0f);
	const FMCPToolResult Stop = Runtime.Execute(
		TEXT("production.trace.stop"),
		StatusParams);
	bTraceMayBeActive = false;
	if (!TestTrue(
		TEXT("Trace finalization succeeds after recording"),
		Stop.bSuccess && Stop.Data.IsValid()))
	{
		AddError(FString::Printf(
			TEXT("Trace stop failed with %s: %s"),
			*Stop.ErrorCode,
			*Stop.ErrorMessage));
		return false;
	}
	TestEqual(
		TEXT("Stopped Trace job succeeds"),
		Stop.Data->GetStringField(TEXT("status")),
		FString(TEXT("succeeded")));
	TestEqual(
		TEXT("Editor Net Trace verbosity is restored after managed stop"),
		FNetTrace::GetTraceVerbosity(),
		OriginalNetTraceVerbosity);

	const TSharedPtr<FJsonObject>* ResultPtr = nullptr;
	if (!TestTrue(
		TEXT("Stopped Trace exposes its recording evidence"),
		Stop.Data->TryGetObjectField(TEXT("result"), ResultPtr)
			&& ResultPtr && ResultPtr->IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Result = *ResultPtr;
	const FString TracePath = Result->GetStringField(TEXT("destination"));
	const FString RegionName = Result->GetStringField(TEXT("regionName"));
	const FString ExpectedMarker =
		Result->GetStringField(TEXT("managedEngineMarker"));
	TestTrue(
		TEXT("Trace artifact is a non-empty .utrace file"),
		TracePath.EndsWith(TEXT(".utrace"), ESearchCase::IgnoreCase)
			&& IFileManager::Get().FileSize(*TracePath) > 0);
	TestEqual(
		TEXT("Managed region is bound to this trace job"),
		RegionName,
		TEXT("UEAI.Trace.") + TraceId);
	TestTrue(
		TEXT("Managed marker has the engine-version prefix"),
		ExpectedMarker.StartsWith(TEXT("UEAI_TRACE_ENGINE_VERSION=")));

	UEAI::Trace::FTraceAnalysisSession Analysis;
	FString AnalysisErrorCode;
	FString AnalysisErrorMessage;
	if (!TestTrue(
		TEXT("TraceAnalysisCore strictly opens the stopped Editor Trace"),
		Analysis.Open(
			TracePath,
			60.0,
			AnalysisErrorCode,
			AnalysisErrorMessage)))
	{
		AddError(FString::Printf(
			TEXT("Trace analysis failed with %s: %s"),
			*AnalysisErrorCode,
			*AnalysisErrorMessage));
		return false;
	}
	TestEqual(
		TEXT("TraceAnalysisCore reads the marker emitted after OnConnection"),
		Analysis.GetManagedEngineMarker(),
		ExpectedMarker);
	TestTrue(
		TEXT("Engine version is matched by diagnostics or the managed marker"),
		Analysis.GetEngineVersionStatus() == TEXT("matched")
			|| Analysis.GetEngineVersionStatus()
				== TEXT("matchedManagedMarker"));

	UEAI::Trace::FTraceQueryRequest RegionRequest;
	RegionRequest.Provider = TEXT("region");
	RegionRequest.Operation = TEXT("list");
	RegionRequest.Filter = RegionName;
	RegionRequest.Page.Limit = 16;
	UEAI::Trace::FTraceQueryResult RegionResult;
	if (!TestTrue(
		TEXT("TraceAnalysisCore can query the managed region"),
		Analysis.Query(
			RegionRequest,
			RegionResult,
			AnalysisErrorCode,
			AnalysisErrorMessage)))
	{
		AddError(FString::Printf(
			TEXT("Region query failed with %s: %s"),
			*AnalysisErrorCode,
			*AnalysisErrorMessage));
		return false;
	}

	const UEAI::Trace::FTraceRow* ManagedRegion =
		RegionResult.Rows.FindByPredicate(
			[&RegionName](const UEAI::Trace::FTraceRow& Row)
			{
				const UEAI::Trace::FTraceValue* Name =
					Row.Fields.Find(TEXT("name"));
				return Name
					&& Name->Type
						== UEAI::Trace::ETraceValueType::String
					&& Name->StringValue == RegionName;
			});
	if (!TestNotNull(
		TEXT("The exact managed region is present"),
		ManagedRegion))
	{
		return false;
	}
	const UEAI::Trace::FTraceValue* Begin =
		ManagedRegion->Fields.Find(TEXT("beginSeconds"));
	const UEAI::Trace::FTraceValue* End =
		ManagedRegion->Fields.Find(TEXT("endSeconds"));
	const UEAI::Trace::FTraceValue* Duration =
		ManagedRegion->Fields.Find(TEXT("durationMs"));
	const UEAI::Trace::FTraceValue* OpenEnded =
		ManagedRegion->Fields.Find(TEXT("openEnded"));
	TestTrue(
		TEXT("Managed region has numeric begin/end evidence"),
		Begin && Begin->Type == UEAI::Trace::ETraceValueType::Number
			&& End && End->Type == UEAI::Trace::ETraceValueType::Number
			&& End->NumberValue >= Begin->NumberValue);
	TestTrue(
		TEXT("Managed region spans a measurable post-connection interval"),
		Duration
			&& Duration->Type == UEAI::Trace::ETraceValueType::Number
			&& Duration->NumberValue > 0.0);
	TestTrue(
		TEXT("Managed region is closed before the writer is stopped"),
		OpenEnded
			&& OpenEnded->Type == UEAI::Trace::ETraceValueType::Boolean
			&& !OpenEnded->BooleanValue);

	UEAI::Trace::FTraceQueryRequest NetworkRequest;
	NetworkRequest.Provider = TEXT("network");
	NetworkRequest.Operation = TEXT("connections");
	NetworkRequest.Page.Limit = 16;
	UEAI::Trace::FTraceQueryResult NetworkResult;
	if (!TestTrue(
		TEXT("Network provider parses the post-connection Net init event"),
		Analysis.Query(
			NetworkRequest,
			NetworkResult,
			AnalysisErrorCode,
			AnalysisErrorMessage)))
	{
		AddError(FString::Printf(
			TEXT("Network query failed with %s: %s"),
			*AnalysisErrorCode,
			*AnalysisErrorMessage));
		return false;
	}

	Analysis.Close();
	bCleanupCompletedEvidence = true;
	return true;
}

#endif
