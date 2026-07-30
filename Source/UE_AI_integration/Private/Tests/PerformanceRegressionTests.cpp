#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/PerformanceRegressionService.h"
#include "Infrastructure/StandalonePerformanceController.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

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

TSharedPtr<FJsonObject> MakeMetric(
	const double P50,
	const double P95,
	const double P99,
	const double Max)
{
	TSharedPtr<FJsonObject> Metric = MakeShared<FJsonObject>();
	Metric->SetBoolField(TEXT("available"), true);
	Metric->SetNumberField(TEXT("sampleCount"), 60);
	Metric->SetNumberField(TEXT("min"), P50);
	Metric->SetNumberField(TEXT("mean"), P50);
	Metric->SetNumberField(TEXT("p50"), P50);
	Metric->SetNumberField(TEXT("p95"), P95);
	Metric->SetNumberField(TEXT("p99"), P99);
	Metric->SetNumberField(TEXT("max"), Max);
	Metric->SetNumberField(TEXT("overBudgetFrames"), 0);
	return Metric;
}

TSharedPtr<FJsonObject> MakePerformanceResult(
	const double Frame,
	const double Game,
	const double Render,
	const double Rhi,
	const double Gpu)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	Metrics->SetObjectField(
		TEXT("frameMs"),
		MakeMetric(Frame, Frame, Frame + 0.05, Frame + 0.1));
	Metrics->SetObjectField(
		TEXT("gameMs"),
		MakeMetric(Game, Game, Game, Game));
	Metrics->SetObjectField(
		TEXT("renderMs"),
		MakeMetric(Render, Render, Render, Render));
	Metrics->SetObjectField(
		TEXT("rhiMs"),
		MakeMetric(Rhi, Rhi, Rhi, Rhi));
	Metrics->SetObjectField(
		TEXT("gpuMs"),
		MakeMetric(Gpu, Gpu, Gpu, Gpu));
	Result->SetObjectField(TEXT("metrics"), Metrics);
	return Result;
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
	TestEqual(
		TEXT("standard profile defaults to the project GameInstance"),
		Profile->GetStringField(TEXT("gameInstanceMode")),
		FString(TEXT("project")));

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

	TSharedPtr<FJsonObject> Standalone = MakeStandardRequest();
	Standalone->SetStringField(
		TEXT("executionTarget"),
		TEXT("standalone"));
	Standalone->SetNumberField(TEXT("startupTimeoutSeconds"), 900.0);
	Standalone->GetObjectField(TEXT("standardProfile"))->SetArrayField(
		TEXT("inputSteps"),
		TArray<TSharedPtr<FJsonValue>>());
	Standalone->GetObjectField(TEXT("standardProfile"))->SetStringField(
		TEXT("gameInstanceMode"),
		TEXT("minimal"));
	Error.Reset();
	TestTrue(
		TEXT("standalone fixed map does not require the map to be open"),
		FPerformanceRegressionService::NormalizeRunRequest(
			Standalone,
			TEXT("/Game/Maps/Other"),
			Normalized,
			Profile,
			Error));
	TestEqual(
		TEXT("standalone target survives normalization"),
		Normalized->GetStringField(TEXT("executionTarget")),
		FString(TEXT("standalone")));
	TestEqual(
		TEXT("standalone preserves per-repeat warmup for CSV windowing"),
		Normalized->GetNumberField(TEXT("warmupSeconds")),
		3.0);
	TestEqual(
		TEXT("standalone preserves its requested startup budget"),
		Normalized->GetNumberField(TEXT("startupTimeoutSeconds")),
		900.0);
	TestEqual(
		TEXT("standalone preserves the explicit lifecycle isolation mode"),
		Normalized->GetObjectField(TEXT("standardProfile"))
			->GetStringField(TEXT("gameInstanceMode")),
		FString(TEXT("minimal")));

	TSharedPtr<FJsonObject> InvalidGameInstance = MakeStandardRequest();
	InvalidGameInstance->SetStringField(
		TEXT("executionTarget"),
		TEXT("standalone"));
	InvalidGameInstance->GetObjectField(TEXT("standardProfile"))
		->SetArrayField(
			TEXT("inputSteps"),
			TArray<TSharedPtr<FJsonValue>>());
	InvalidGameInstance->GetObjectField(TEXT("standardProfile"))
		->SetStringField(TEXT("gameInstanceMode"), TEXT("external"));
	Error.Reset();
	TestFalse(
		TEXT("unknown standalone GameInstance isolation mode is rejected"),
		FPerformanceRegressionService::NormalizeRunRequest(
			InvalidGameInstance,
			TEXT("/Game/Maps/Golden"),
			Normalized,
			Profile,
			Error));
	TestTrue(
		TEXT("GameInstance isolation error is actionable"),
		Error.Contains(TEXT("gameInstanceMode")));

	TSharedPtr<FJsonObject> StandaloneInput = MakeStandardRequest();
	StandaloneInput->SetStringField(
		TEXT("executionTarget"),
		TEXT("standalone"));
	Error.Reset();
	TestFalse(
		TEXT("standalone never silently ignores input replay"),
		FPerformanceRegressionService::NormalizeRunRequest(
			StandaloneInput,
			TEXT("/Game/Maps/Golden"),
			Normalized,
			Profile,
			Error));
	TestTrue(
		TEXT("standalone input error is actionable"),
		Error.Contains(TEXT("inputSteps")));

	TSharedPtr<FJsonObject> LongCamera = MakeStandardRequest();
	LongCamera->GetObjectField(TEXT("standardProfile"))
		->GetObjectField(TEXT("camera"))
		->SetStringField(TEXT("name"), FString::ChrN(129, TCHAR('C')));
	Error.Reset();
	TestFalse(
		TEXT("camera names longer than the public 128 character bound fail"),
		FPerformanceRegressionService::NormalizeRunRequest(
			LongCamera,
			TEXT("/Game/Maps/Golden"),
			Normalized,
			Profile,
			Error));
	TestTrue(
		TEXT("camera name failure reports the shared bound"),
		Error.Contains(TEXT("1-128")));

	bool bAmbiguous = false;
	TestEqual(
		TEXT("duplicate camera labels are rejected"),
		FStandalonePerformanceController::FindUniqueCameraMatch(
			TArray<FString>{TEXT("CameraActor_0"), TEXT("CameraActor_1")},
			TArray<FString>{TEXT("SharedCamera"), TEXT("SharedCamera")},
			TEXT("SharedCamera"),
			bAmbiguous),
		INDEX_NONE);
	TestTrue(
		TEXT("duplicate camera labels report ambiguity"),
		bAmbiguous);
	TestEqual(
		TEXT("an exact object name takes precedence over labels"),
		FStandalonePerformanceController::FindUniqueCameraMatch(
			TArray<FString>{TEXT("CameraActor_0"), TEXT("CameraActor_1")},
			TArray<FString>{TEXT("CameraActor_0"), TEXT("CameraActor_0")},
			TEXT("CameraActor_0"),
			bAmbiguous),
		0);
	TestFalse(
		TEXT("a unique exact object name is not ambiguous"),
		bAmbiguous);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPerformanceDiagnosisContractTest,
	"UE_AI_integration.Performance.DiagnosisContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPerformanceDiagnosisContractTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<FJsonObject> LimitedFingerprint = MakeShared<FJsonObject>();
	LimitedFingerprint->SetStringField(TEXT("vsync"), TEXT("1"));
	LimitedFingerprint->SetStringField(TEXT("fpsCap"), TEXT("60"));
	const TSharedPtr<FJsonObject> Limited =
		FPerformanceRegressionService::BuildPerformanceDiagnosis(
			MakePerformanceResult(16.6667, 4.0, 3.0, 1.0, 5.0),
			LimitedFingerprint);
	TestEqual(
		TEXT("fixed cadence is frame-limited"),
		Limited->GetStringField(TEXT("verdict")),
		FString(TEXT("frameLimited")));
	TestTrue(
		TEXT("limiter evidence is explicit"),
		Limited->GetObjectField(TEXT("frameLimiter"))->GetBoolField(
			TEXT("detected")));

	TSharedPtr<FJsonObject> VSync120Fingerprint =
		MakeShared<FJsonObject>();
	VSync120Fingerprint->SetStringField(TEXT("vsync"), TEXT("1"));
	VSync120Fingerprint->SetStringField(TEXT("fpsCap"), TEXT("0"));
	const TSharedPtr<FJsonObject> VSync120 =
		FPerformanceRegressionService::BuildPerformanceDiagnosis(
			MakePerformanceResult(8.3333, 3.0, 2.0, 1.0, 4.0),
			VSync120Fingerprint);
	TestEqual(
		TEXT("stable 120 Hz VSync cadence is frame-limited"),
		VSync120->GetStringField(TEXT("verdict")),
		FString(TEXT("frameLimited")));
	const TSharedPtr<FJsonObject> VSync120Limiter =
		VSync120->GetObjectField(TEXT("frameLimiter"));
	TestTrue(
		TEXT("120 Hz limiter evidence is explicit"),
		VSync120Limiter->GetBoolField(TEXT("detected")));
	TestEqual(
		TEXT("120 Hz refresh is inferred from VSync cadence"),
		VSync120Limiter->GetNumberField(TEXT("inferredRefreshHz")),
		120.0);

	TSharedPtr<FJsonObject> UncappedFingerprint = MakeShared<FJsonObject>();
	UncappedFingerprint->SetStringField(TEXT("vsync"), TEXT("0"));
	UncappedFingerprint->SetStringField(TEXT("fpsCap"), TEXT("0"));
	UncappedFingerprint->SetStringField(TEXT("mapPackage"), TEXT("/Game/Test"));
	UncappedFingerprint->SetStringField(TEXT("gitRevision"), TEXT("abc123"));
	const TSharedPtr<FJsonObject> StableUncapped =
		FPerformanceRegressionService::BuildPerformanceDiagnosis(
			MakePerformanceResult(16.6667, 10.0, 4.0, 2.0, 5.0),
			UncappedFingerprint);
	TestEqual(
		TEXT("stable 16.67 ms without limiter evidence is inconclusive"),
		StableUncapped->GetStringField(TEXT("verdict")),
		FString(TEXT("inconclusive")));
	const TSharedPtr<FJsonObject> StableUncappedLimiter =
		StableUncapped->GetObjectField(TEXT("frameLimiter"));
	TestFalse(
		TEXT("uncapped stable cadence does not fabricate a limiter"),
		StableUncappedLimiter->GetBoolField(TEXT("detected")));
	TestTrue(
		TEXT("uncapped stable cadence is marked as a suspected limiter"),
		StableUncappedLimiter->GetBoolField(TEXT("suspected")));
	TestEqual(
		TEXT("uncapped stable cadence is retained as an observation"),
		StableUncappedLimiter->GetStringField(TEXT("observation")),
		FString(TEXT("stable60HzWithoutExplicitLimiterEvidence")));
	TestTrue(
		TEXT("uncapped stable cadence explains why attribution is withheld"),
		StableUncapped->GetStringField(TEXT("reason")).Contains(
			TEXT("cannot support CPU/GPU attribution")));
	TSharedPtr<FJsonObject> MissingLimiterMetadata =
		MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> StableWithoutLimiterMetadata =
		FPerformanceRegressionService::BuildPerformanceDiagnosis(
			MakePerformanceResult(16.6667, 10.0, 4.0, 2.0, 5.0),
			MissingLimiterMetadata);
	TestEqual(
		TEXT("stable cadence without limiter metadata also withholds attribution"),
		StableWithoutLimiterMetadata->GetStringField(TEXT("verdict")),
		FString(TEXT("inconclusive")));
	TestTrue(
		TEXT("missing limiter metadata retains suspected limiter evidence"),
		StableWithoutLimiterMetadata
			->GetObjectField(TEXT("frameLimiter"))
			->GetBoolField(TEXT("suspected")));
	const TSharedPtr<FJsonObject> MissingLogHealth =
		StableUncapped->GetObjectField(TEXT("logHealth"));
	TestFalse(
		TEXT("missing log windows are not reported as clean"),
		MissingLogHealth->GetBoolField(TEXT("available")));
	TestEqual(
		TEXT("missing log windows report unavailable health"),
		MissingLogHealth->GetStringField(TEXT("status")),
		FString(TEXT("unavailable")));

	TSharedPtr<FJsonObject> CpuPerformance =
		MakePerformanceResult(12.0, 10.0, 4.0, 2.0, 5.0);
	TSharedPtr<FJsonObject> LogWindow = MakeShared<FJsonObject>();
	LogWindow->SetBoolField(TEXT("available"), true);
	LogWindow->SetStringField(
		TEXT("content"),
		TEXT("LogTemp: Warning: bounded warning\nLogTemp: Error: bounded error"));
	CpuPerformance->SetArrayField(
		TEXT("logWindows"),
		{MakeShared<FJsonValueObject>(LogWindow)});
	TSharedPtr<FJsonObject> CpuTimer = MakeShared<FJsonObject>();
	CpuTimer->SetStringField(TEXT("name"), TEXT("GameThread Scope"));
	CpuTimer->SetBoolField(TEXT("gpu"), false);
	CpuTimer->SetNumberField(TEXT("totalInclusiveMs"), 25.0);
	CpuTimer->SetNumberField(TEXT("maxInclusiveMs"), 10.0);
	CpuTimer->SetNumberField(TEXT("instanceCount"), 3);
	TSharedPtr<FJsonObject> GpuTimer = MakeShared<FJsonObject>();
	GpuTimer->SetStringField(TEXT("name"), TEXT("GPU Frame Interval"));
	GpuTimer->SetBoolField(TEXT("gpu"), true);
	GpuTimer->SetNumberField(TEXT("totalInclusiveMs"), 18.0);
	GpuTimer->SetNumberField(TEXT("maxInclusiveMs"), 9.0);
	GpuTimer->SetNumberField(TEXT("instanceCount"), 2);
	TSharedPtr<FJsonObject> GpuAggregate = MakeShared<FJsonObject>();
	GpuAggregate->SetBoolField(TEXT("available"), true);
	GpuAggregate->SetNumberField(TEXT("timerCount"), 1);
	GpuAggregate->SetNumberField(TEXT("totalExclusiveMs"), 8.5);
	TSharedPtr<FJsonObject> ThreadAggregates = MakeShared<FJsonObject>();
	ThreadAggregates->SetObjectField(TEXT("gpu"), GpuAggregate);
	TSharedPtr<FJsonObject> TraceAnalysis = MakeShared<FJsonObject>();
	TraceAnalysis->SetNumberField(TEXT("intervalStartSeconds"), 2.0);
	TraceAnalysis->SetNumberField(TEXT("intervalEndSeconds"), 4.0);
	TraceAnalysis->SetArrayField(
		TEXT("timers"),
		{
			MakeShared<FJsonValueObject>(CpuTimer),
			MakeShared<FJsonValueObject>(GpuTimer)
		});
	TraceAnalysis->SetObjectField(
		TEXT("threadAggregates"),
		ThreadAggregates);

	const TSharedPtr<FJsonObject> Cpu =
		FPerformanceRegressionService::BuildPerformanceDiagnosis(
			CpuPerformance,
			UncappedFingerprint,
			TraceAnalysis);
	TestEqual(
		TEXT("uncapped CPU-heavy sample is CPU-bound"),
		Cpu->GetStringField(TEXT("verdict")),
		FString(TEXT("cpuBound")));
	const TSharedPtr<FJsonObject> TraceEvidence =
		Cpu->GetObjectField(TEXT("traceEvidence"));
	TestEqual(
		TEXT("CPU evidence excludes GPU timers"),
		TraceEvidence->GetArrayField(TEXT("topCpuScopes")).Num(),
		1);
	TestEqual(
		TEXT("GPU intervals are exposed separately"),
		TraceEvidence->GetArrayField(TEXT("gpuIntervals")).Num(),
		1);

	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("title"), TEXT("性能回归"));
	Report->SetStringField(TEXT("runId"), TEXT("run-test"));
	Report->SetObjectField(
		TEXT("performance"),
		CpuPerformance);
	Report->SetObjectField(TEXT("diagnosis"), Cpu);
	Report->SetObjectField(
		TEXT("environmentFingerprint"),
		UncappedFingerprint);
	TSharedPtr<FJsonObject> SourceArtifact = MakeShared<FJsonObject>();
	SourceArtifact->SetStringField(
		TEXT("artifactId"),
		TEXT("trace\"><script>alert(1)</script>"));
	SourceArtifact->SetStringField(
		TEXT("name"),
		TEXT("Trace <evidence>"));
	SourceArtifact->SetStringField(
		TEXT("mimeType"),
		TEXT("application/x-unreal-trace"));
	SourceArtifact->SetStringField(
		TEXT("path"),
		TEXT("C:\\private\\trace.utrace"));
	SourceArtifact->SetNumberField(TEXT("sizeBytes"), 4096);
	Report->SetArrayField(
		TEXT("sourceArtifacts"),
		{MakeShared<FJsonValueObject>(SourceArtifact)});
	const FString Html =
		FPerformanceRegressionService::BuildHtmlPerformanceReport(
			TEXT("report-test"),
			Report);
	TestTrue(
		TEXT("HTML is self-contained"),
		Html.Contains(TEXT("<!doctype html>"))
			&& Html.Contains(TEXT("<style>")));
	TestTrue(
		TEXT("HTML contains diagnosis"),
		Html.Contains(TEXT("cpuBound")));
	TestTrue(
		TEXT("HTML renders run-bounded log health"),
		Html.Contains(TEXT("Run-bounded log health"))
			&& Html.Contains(TEXT("<strong>errors</strong>"))
			&& Html.Contains(TEXT("bounded error")) == false);
	TestTrue(
		TEXT("HTML renders top CPU scope evidence"),
		Html.Contains(TEXT("Top CPU scopes"))
			&& Html.Contains(TEXT("GameThread Scope")));
	TestTrue(
		TEXT("HTML renders GPU interval evidence"),
		Html.Contains(TEXT("GPU intervals and evidence"))
			&& Html.Contains(TEXT("GPU Frame Interval"))
			&& Html.Contains(TEXT("Total exclusive: 8.500 ms"))
			&& Html.Contains(TEXT("Analysis interval: 2.000–4.000 s")));
	TestTrue(
		TEXT("HTML renders bounded next steps"),
		Html.Contains(TEXT("Next steps"))
			&& Html.Contains(TEXT("top Game/Render/RHI scopes")));
	TestTrue(
		TEXT("artifact references are navigable and use the retrieval contract"),
		Html.Contains(TEXT("href=\"#artifact-"))
			&& Html.Contains(TEXT("id=\"artifact-"))
			&& Html.Contains(TEXT("production.job.artifact.get"))
			&& Html.Contains(TEXT("jobId <code>run-test</code>")));
	TestTrue(
		TEXT("artifact metadata is escaped and no executable link is emitted"),
		!Html.Contains(TEXT("<script>alert(1)</script>"))
			&& Html.Contains(TEXT("&lt;script&gt;alert(1)&lt;/script&gt;"))
			&& !Html.Contains(TEXT("file://"))
			&& !Html.Contains(TEXT("javascript:"))
			&& !Html.Contains(TEXT("C:\\private")));
	TestTrue(
		TEXT("HTML is UTF-16 in memory without replacement"),
		Html.Contains(TEXT("性能回归")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPerformanceStandaloneEvidenceContractTest,
	"UE_AI_integration.Performance.StandaloneEvidenceContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPerformanceStandaloneEvidenceContractTest::RunTest(
	const FString& Parameters)
{
	const FString RunId = FString::Printf(
		TEXT("automation-performance-%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString OwnedTraceId = TEXT("trace-owned-by-performance-run");
	bool bReturnMatchingTrace = false;

	FPerformanceRegressionService Service(
		[&RunId, &OwnedTraceId, &bReturnMatchingTrace](
			const FString& CapabilityId,
			const TSharedPtr<FJsonObject>& Params) -> FMCPToolResult
		{
			if (CapabilityId == TEXT("production.performance.run"))
			{
				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("runId"), RunId);
				Data->SetStringField(TEXT("jobId"), RunId);
				Data->SetStringField(TEXT("status"), TEXT("running"));
				return FMCPToolResult::Ok(Data);
			}
			if (CapabilityId == TEXT("production.performance.result.get"))
			{
				TSharedPtr<FJsonObject> Performance =
					MakePerformanceResult(12.0, 9.0, 4.0, 2.0, 5.0);
				Performance->SetStringField(TEXT("traceId"), OwnedTraceId);

				TSharedPtr<FJsonObject> RuntimeFingerprint =
					MakeShared<FJsonObject>();
				RuntimeFingerprint->SetStringField(TEXT("rhi"), TEXT("D3D12"));
				RuntimeFingerprint->SetStringField(
					TEXT("gpuAdapter"),
					TEXT("Automation GPU"));
				RuntimeFingerprint->SetStringField(
					TEXT("gpuDriver"),
					TEXT("automation-driver"));
				RuntimeFingerprint->SetStringField(
					TEXT("resolution"),
					TEXT("1920x1080"));
				RuntimeFingerprint->SetStringField(
					TEXT("windowMode"),
					TEXT("windowed"));
				RuntimeFingerprint->SetStringField(TEXT("vsync"), TEXT("0"));
				RuntimeFingerprint->SetStringField(TEXT("fpsCap"), TEXT("0"));
				RuntimeFingerprint->SetObjectField(
					TEXT("cvars"),
					MakeShared<FJsonObject>());
				Performance->SetObjectField(
					TEXT("runtimeFingerprint"),
					RuntimeFingerprint);

				TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();
				Camera->SetStringField(TEXT("name"), TEXT("PerfCamera"));
				Camera->SetBoolField(TEXT("lockedAndVerified"), true);
				Performance->SetObjectField(TEXT("camera"), Camera);
				TSharedPtr<FJsonObject> ChildReceipt =
					MakeShared<FJsonObject>();
				ChildReceipt->SetStringField(TEXT("status"), TEXT("succeeded"));
				ChildReceipt->SetStringField(
					TEXT("tracePath"),
					TEXT("standalone.utrace"));
				Performance->SetObjectField(
					TEXT("standaloneChildReceipt"),
					ChildReceipt);
				Performance->SetObjectField(
					TEXT("context"),
					MakeShared<FJsonObject>());

				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("status"), TEXT("succeeded"));
				Data->SetStringField(
					TEXT("kind"),
					TEXT("performanceStandalone"));
				Data->SetObjectField(TEXT("result"), Performance);
				return FMCPToolResult::Ok(Data);
			}
			if (CapabilityId == TEXT("production.job.result.get"))
			{
				TSharedPtr<FJsonObject> Analysis = MakeShared<FJsonObject>();
				Analysis->SetStringField(
					TEXT("traceId"),
					bReturnMatchingTrace
						? OwnedTraceId
						: TEXT("trace-from-another-run"));
				Analysis->SetObjectField(
					TEXT("analysis"),
					MakeShared<FJsonObject>());
				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("status"), TEXT("succeeded"));
				Data->SetStringField(TEXT("kind"), TEXT("traceAnalysis"));
				Data->SetObjectField(TEXT("result"), Analysis);
				return FMCPToolResult::Ok(Data);
			}
			if (CapabilityId == TEXT("production.performance.compare"))
			{
				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(
					TEXT("baselineRunId"),
					Params->GetStringField(TEXT("baselineRunId")));
				Data->SetStringField(
					TEXT("candidateRunId"),
					Params->GetStringField(TEXT("candidateRunId")));
				Data->SetStringField(TEXT("verdict"), TEXT("pass"));
				Data->SetArrayField(
					TEXT("checks"),
					TArray<TSharedPtr<FJsonValue>>());
				return FMCPToolResult::Ok(Data);
			}
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Unexpected test operation: %s"),
					*CapabilityId),
				TEXT("unexpected_test_operation"),
				500);
		});

	TSharedPtr<FJsonObject> RunRequest = MakeStandardRequest();
	RunRequest->SetStringField(TEXT("executionTarget"), TEXT("standalone"));
	RunRequest->GetObjectField(TEXT("standardProfile"))->SetArrayField(
		TEXT("inputSteps"),
		TArray<TSharedPtr<FJsonValue>>());
	const FMCPToolResult Started = Service.Execute(
		TEXT("production.performance.run"),
		RunRequest);
	TestTrue(TEXT("standalone run is accepted"), Started.bSuccess);

	TSharedPtr<FJsonObject> ResultRequest = MakeShared<FJsonObject>();
	ResultRequest->SetStringField(TEXT("runId"), RunId);
	const FMCPToolResult Result = Service.Execute(
		TEXT("production.performance.result.get"),
		ResultRequest);
	if (TestTrue(TEXT("standalone result is available"), Result.bSuccess)
		&& Result.Data.IsValid())
	{
		const bool bHasFingerprint =
			Result.Data->HasTypedField<EJson::Object>(
				TEXT("environmentFingerprint"));
		if (TestTrue(
			TEXT("standalone result carries an environment fingerprint"),
			bHasFingerprint))
		{
			const TSharedPtr<FJsonObject> Fingerprint =
				Result.Data->GetObjectField(
					TEXT("environmentFingerprint"));
			TestEqual(
				TEXT("child runtime fingerprint replaces launch intent"),
				Fingerprint->GetStringField(TEXT("provenance")),
				FString(TEXT("standaloneChildRuntime")));
			TestEqual(
				TEXT("child RHI evidence is retained"),
				Fingerprint->GetStringField(TEXT("rhi")),
				FString(TEXT("D3D12")));
			TestEqual(
				TEXT("declared map remains part of the merged fingerprint"),
				Fingerprint->GetStringField(TEXT("mapPackage")),
				FString(TEXT("/Game/Maps/Golden")));
		}
		const bool bHasPerformance =
			Result.Data->HasTypedField<EJson::Object>(TEXT("result"));
		if (TestTrue(
			TEXT("standalone result has its durable result payload"),
			bHasPerformance))
		{
			const TSharedPtr<FJsonObject> Performance =
				Result.Data->GetObjectField(TEXT("result"));
			const bool bHasReceipt =
				Performance->HasTypedField<EJson::Object>(
					TEXT("standaloneChildReceipt"));
			if (TestTrue(
				TEXT("child receipt remains attached to the durable result"),
				bHasReceipt))
			{
				TestEqual(
					TEXT("child receipt reached a successful terminal state"),
					Performance->GetObjectField(
						TEXT("standaloneChildReceipt"))
						->GetStringField(TEXT("status")),
					FString(TEXT("succeeded")));
			}
			const bool bHasCamera =
				Performance->HasTypedField<EJson::Object>(TEXT("camera"));
			if (TestTrue(
				TEXT("camera evidence remains attached"),
				bHasCamera))
			{
				TestTrue(
					TEXT("camera lock was verified by the child runtime"),
					Performance->GetObjectField(TEXT("camera"))
						->GetBoolField(TEXT("lockedAndVerified")));
			}
		}
	}

	TSharedPtr<FJsonObject> DiagnoseRequest = MakeShared<FJsonObject>();
	DiagnoseRequest->SetStringField(TEXT("runId"), RunId);
	DiagnoseRequest->SetStringField(
		TEXT("traceAnalysisJobId"),
		TEXT("trace-analysis-job"));
	const FMCPToolResult Mismatched = Service.Execute(
		TEXT("production.performance.diagnose"),
		DiagnoseRequest);
	TestFalse(
		TEXT("diagnosis rejects trace analysis from another run"),
		Mismatched.bSuccess);
	TestEqual(
		TEXT("trace provenance mismatch is structured"),
		Mismatched.ErrorCode,
		FString(TEXT("trace_analysis_provenance_mismatch")));

	bReturnMatchingTrace = true;
	const FMCPToolResult Matched = Service.Execute(
		TEXT("production.performance.diagnose"),
		DiagnoseRequest);
	if (TestTrue(
			TEXT("diagnosis accepts owned trace analysis"),
			Matched.bSuccess)
		&& Matched.Data.IsValid())
	{
		TestTrue(
			TEXT("accepted trace is exposed as diagnosis evidence"),
			Matched.Data->GetObjectField(TEXT("traceEvidence"))
				->GetBoolField(TEXT("available")));
		TestEqual(
			TEXT("diagnosis records the accepted analysis job"),
			Matched.Data->GetStringField(TEXT("traceAnalysisJobId")),
			FString(TEXT("trace-analysis-job")));
	}

	TSharedPtr<FJsonObject> CompareRequest = MakeShared<FJsonObject>();
	CompareRequest->SetStringField(
		TEXT("baselineRunId"),
		TEXT("comparison-baseline"));
	CompareRequest->SetStringField(
		TEXT("candidateRunId"),
		TEXT("comparison-candidate"));
	CompareRequest->SetArrayField(
		TEXT("checks"),
		TArray<TSharedPtr<FJsonValue>>());
	const FMCPToolResult Comparison = Service.Execute(
		TEXT("production.performance.compare"),
		CompareRequest);
	FString ComparisonId;
	if (TestTrue(
			TEXT("comparison fixture is created"),
			Comparison.bSuccess && Comparison.Data.IsValid()))
	{
		ComparisonId =
			Comparison.Data->GetStringField(TEXT("comparisonId"));
		TSharedPtr<FJsonObject> ReportRequest = MakeShared<FJsonObject>();
		ReportRequest->SetStringField(TEXT("runId"), RunId);
		ReportRequest->SetStringField(
			TEXT("comparisonId"),
			ComparisonId);
		const FMCPToolResult MismatchedReport = Service.Execute(
			TEXT("production.performance.report.generate"),
			ReportRequest);
		TestFalse(
			TEXT("report rejects a comparison for another candidate run"),
			MismatchedReport.bSuccess);
		TestEqual(
			TEXT("comparison/run mismatch is structured"),
			MismatchedReport.ErrorCode,
			FString(TEXT("performance_comparison_candidate_mismatch")));
	}

	if (!ComparisonId.IsEmpty())
	{
		const FString ComparisonDirectory = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UE_AI_integration"),
			TEXT("PerformanceRegressions"),
			ComparisonId);
		TestTrue(
			TEXT("comparison fixture is cleaned"),
			!IFileManager::Get().DirectoryExists(*ComparisonDirectory)
				|| IFileManager::Get().DeleteDirectory(
					*ComparisonDirectory,
					false,
					true));
	}

	const FString FingerprintDirectory = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("Performance"),
		RunId);
	const bool bFixtureRemoved =
		!IFileManager::Get().DirectoryExists(*FingerprintDirectory)
		|| IFileManager::Get().DeleteDirectory(
			*FingerprintDirectory,
			false,
			true);
	TestTrue(
		TEXT("standalone fingerprint fixture is cleaned"),
		bFixtureRemoved);
	return true;
}

#endif
