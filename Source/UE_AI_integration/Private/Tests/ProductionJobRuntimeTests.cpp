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
	for (const TSharedPtr<FJsonValue>& Value :
		Root->GetArrayField(TEXT("capabilities")))
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString Id = Capability->GetStringField(TEXT("id"));
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
	return true;
}

#endif
