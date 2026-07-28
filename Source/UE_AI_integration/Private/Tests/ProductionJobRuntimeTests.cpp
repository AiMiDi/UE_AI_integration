#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/ProductionJobRuntime.h"

#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

	const TSharedPtr<FJsonObject> EmptyMetric =
		FProductionJobRuntime::SummarizeMetric(TArray<double>(), 16.6667);
	TestFalse(TEXT("Empty metric is unavailable"), EmptyMetric->GetBoolField(TEXT("available")));

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
			TestEqual(
				*FString::Printf(TEXT("%s is excluded from Workflow"), *Id),
				Capability->GetObjectField(TEXT("dsl"))
					->GetStringField(TEXT("admission")),
				FString(TEXT("none")));
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
	TestEqual(
		TEXT("Production manifest includes the migrated and new operations"),
		Root->GetArrayField(TEXT("capabilities")).Num(),
		47);

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

#endif
