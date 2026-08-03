#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/WorldFactory.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Infrastructure/PerformanceSuiteService.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

using UEAIIntegration::Infrastructure::FPerformanceSuiteService;

namespace
{
const TArray<FString> PortableMapPackages = {
	TEXT("/Game/UEAI_Performance/EmptyBaseline"),
	TEXT("/Game/UEAI_Performance/BlueprintHotPath"),
	TEXT("/Game/UEAI_Performance/UMGInteraction")};

bool CreatePortableMapFixture(const FString& PackageName, FString& OutError)
{
	OutError.Reset();
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		OutError = FString::Printf(TEXT("Could not create package %s."), *PackageName);
		return false;
	}
	UWorldFactory* Factory = NewObject<UWorldFactory>();
	UWorld* World = Cast<UWorld>(Factory->FactoryCreateNew(
		UWorld::StaticClass(),
		Package,
		FName(*AssetName),
		RF_Public | RF_Standalone,
		nullptr,
		GWarn));
	if (!World)
	{
		OutError = FString::Printf(TEXT("Could not create world %s."), *PackageName);
		return false;
	}
	FAssetRegistryModule::AssetCreated(World);
	Package->MarkPackageDirty();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, World, *Filename, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Could not save world %s."), *PackageName);
		return false;
	}
	Package->SetDirtyFlag(false);
	return true;
}

void CleanupPortableMapFixtures()
{
	for (const FString& PackageName : PortableMapPackages)
	{
		TArray<UObject*> WorldAssets;
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			TArray<UObject*> Objects;
			GetObjectsWithOuter(Package, Objects, false);
			for (UObject* Object : Objects)
			{
				if (Object && Object->IsA<UWorld>())
				{
					WorldAssets.Add(Object);
				}
			}
			Package->SetDirtyFlag(false);
		}
		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetMapPackageExtension());
		if (IFileManager::Get().FileExists(*Filename))
		{
			IFileManager::Get().Delete(*Filename, false, true, true);
		}
		for (UObject* WorldAsset : WorldAssets)
		{
			FAssetRegistryModule::AssetDeleted(WorldAsset);
		}
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPerformanceSuiteContractTest,
	"UE_AI_integration.Performance.Suite.ContractAndCVarRestore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPerformanceSuiteContractTest::RunTest(const FString& Parameters)
{
	CleanupPortableMapFixtures();
	ON_SCOPE_EXIT
	{
		CleanupPortableMapFixtures();
	};
	for (const FString& PackageName : PortableMapPackages)
	{
		FString FixtureError;
		if (!CreatePortableMapFixture(PackageName, FixtureError))
		{
			AddError(FixtureError);
			CleanupPortableMapFixtures();
			return false;
		}
	}

	int32 RunCounter = 0;
	FPerformanceSuiteService Service(
		[&RunCounter](
			const FString& Capability,
			const TSharedPtr<FJsonObject>& Params)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			if (Capability == TEXT("production.performance.run"))
			{
				++RunCounter;
				Data->SetStringField(
					TEXT("runId"),
					FString::Printf(TEXT("fake-run-%d"), RunCounter));
				Data->SetStringField(TEXT("status"), TEXT("running"));
				return FMCPToolResult::Ok(Data);
			}
			if (Capability == TEXT("production.performance.result.get"))
			{
				Data->SetStringField(TEXT("status"), TEXT("succeeded"));
				TSharedRef<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
				Fingerprint->SetStringField(TEXT("group"), TEXT("automation"));
				Data->SetObjectField(TEXT("environmentFingerprint"), Fingerprint);
				return FMCPToolResult::Ok(Data);
			}
			if (Capability == TEXT("production.performance.report.generate"))
			{
				Data->SetStringField(
					TEXT("jobId"),
					FString::Printf(TEXT("report-%d"), RunCounter));
				return FMCPToolResult::Ok(Data);
			}
			return FMCPToolResult::Error(
				TEXT("Unexpected fake operation."),
				TEXT("unexpected_operation"),
				500);
		});

	const FMCPToolResult Listed = Service.Execute(
		TEXT("production.performance.suite.list"),
		MakeShared<FJsonObject>());
	TestTrue(TEXT("Versioned suites load"), Listed.bSuccess);
	if (!Listed.bSuccess || !Listed.Data.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("Portable and D5 suites are present"),
		Listed.Data->GetIntegerField(TEXT("total")) >= 2);
	TSharedRef<FJsonObject> Validate = MakeShared<FJsonObject>();
	Validate->SetStringField(TEXT("suiteId"), TEXT("portable.standard"));
	const FMCPToolResult Validated = Service.Execute(
		TEXT("production.performance.suite.validate"),
		Validate);
	TestTrue(TEXT("Portable suite validates against real map packages"), Validated.bSuccess);

	IConsoleVariable* VSync =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
	IConsoleVariable* MaxFps =
		IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
	const FString BeforeVSync = VSync ? VSync->GetString() : FString();
	const FString BeforeMaxFps = MaxFps ? MaxFps->GetString() : FString();
	FString BeforeMap;
	if (GEditor && GEditor->GetEditorWorldContext().World())
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		BeforeMap = World->GetOutermost()->GetName();
		World->GetOutermost()->SetDirtyFlag(false);
	}

	TSharedRef<FJsonObject> Run = MakeShared<FJsonObject>();
	Run->SetStringField(TEXT("suiteId"), TEXT("portable.standard"));
	const FMCPToolResult Started = Service.Execute(
		TEXT("production.performance.suite.run"),
		Run);
	TestTrue(TEXT("Suite starts"), Started.bSuccess);
	if (!Started.bSuccess || !Started.Data.IsValid())
	{
		return false;
	}
	const FString SuiteRunId =
		Started.Data->GetStringField(TEXT("suiteRunId"));
	for (int32 Index = 0; Index < 32; ++Index)
	{
		Service.Tick();
	}
	TSharedRef<FJsonObject> ResultParams = MakeShared<FJsonObject>();
	ResultParams->SetStringField(TEXT("suiteRunId"), SuiteRunId);
	const FMCPToolResult Result = Service.Execute(
		TEXT("production.performance.suite.result.get"),
		ResultParams);
	TestTrue(TEXT("Suite result can be read"), Result.bSuccess);
	if (Result.bSuccess && Result.Data.IsValid())
	{
		TestEqual(
			TEXT("All unbaselined scenarios complete"),
			Result.Data->GetStringField(TEXT("status")),
			FString(TEXT("succeeded")));
		TestEqual(
			TEXT("Every portable scenario ran"),
			Result.Data->GetArrayField(TEXT("scenarios")).Num(),
			3);
	}
	if (VSync)
	{
		TestEqual(TEXT("r.VSync is restored"), VSync->GetString(), BeforeVSync);
	}
	if (MaxFps)
	{
		TestEqual(TEXT("t.MaxFPS is restored"), MaxFps->GetString(), BeforeMaxFps);
	}
	if (!BeforeMap.IsEmpty() && GEditor
		&& GEditor->GetEditorWorldContext().World())
	{
		TestEqual(
			TEXT("The original Editor map is restored"),
			GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName(),
			BeforeMap);
	}
	return true;
}

#endif
