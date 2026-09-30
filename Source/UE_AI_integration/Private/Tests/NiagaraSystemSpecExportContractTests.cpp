// Contract tests for content.niagara.system.spec.export.
//
// The export is a read-only authored-system snapshot, so the tests never
// compile, never save, and never assume runtime/PIE. The unknown-system path is
// asserted directly; the valid-system path builds a transient /Game/ system +
// empty emitter fixture (mirroring NiagaraModuleStackContractTests.cpp) and
// verifies the schema envelope plus the presence of the emitters and
// userParameters arrays, skipping with AddInfo when the fixture cannot build.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FNiagaraSystemSpecExportFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
};

bool NiagaraSystemSpecExportCreateFixture(FNiagaraSystemSpecExportFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_SystemSpecExport_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(OutFixture.PackageName);
	OutFixture.Package = CreatePackage(*OutFixture.PackageName);
	OutFixture.System = OutFixture.Package
		? NewObject<UNiagaraSystem>(
			OutFixture.Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!OutFixture.System)
	{
		return false;
	}
	// Fully initialize the System scripts so the system carries the authored
	// spawn/update stacks the export walks (the export itself never compiles).
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);

	// An "empty" emitter: ResetGraphForOutput builds the four output nodes and
	// their input-connector stack sources, but no function-call modules.
	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		OutFixture.System, TEXT("EmptyEmitter"), RF_Transactional);
	if (!Emitter)
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FNiagaraEmitterHandle Handle(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	OutFixture.System->AddEmitterHandleDirect(Handle);
	OutFixture.Package->SetDirtyFlag(false);
	return true;
}

bool NiagaraSystemSpecExportDeleteFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSpecExportUnknownSystemTest,
	"UE_AI_integration.Niagara.SystemSpecExport.UnknownSystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSpecExportUnknownSystemTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("system"),
		TEXT("/Game/Automation/UEAI_SystemSpecExport_Missing.UEAI_SystemSpecExport_Missing"));
	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.export"),
		Params);
	TestFalse(TEXT("Unknown system export is rejected"), Result.bSuccess);
	TestEqual(
		TEXT("Unknown system uses system_not_found"),
		Result.ErrorCode,
		FString(TEXT("system_not_found")));
	TestEqual(
		TEXT("Unknown system maps to HTTP 404"),
		Result.HttpStatus,
		404);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSpecExportContractTest,
	"UE_AI_integration.Niagara.SystemSpecExport.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSpecExportContractTest::RunTest(const FString&)
{
	FNiagaraSystemSpecExportFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("System-spec-export fixture and package are deleted"),
			NiagaraSystemSpecExportDeleteFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("System-spec-export fixture builds"), NiagaraSystemSpecExportCreateFixture(Fixture))
		|| !TestNotNull(TEXT("System-spec-export system"), Fixture.System))
	{
		AddInfo(TEXT("The /Game/ system + empty emitter fixture could not be built; skipping the spec-export contract."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.export"),
		Params);
	if (!TestTrue(TEXT("System spec export succeeds"), Result.bSuccess) || !Result.Data)
	{
		return false;
	}

	TestEqual(
		TEXT("Spec schema is the system-spec envelope"),
		Result.Data->GetStringField(TEXT("schema")),
		FString(TEXT("ue.niagara.system-spec.v1")));
	TestEqual(
		TEXT("Spec version is 1"),
		Result.Data->GetIntegerField(TEXT("specVersion")),
		1);

	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	TestTrue(
		TEXT("Spec exposes a non-empty emitters array"),
		Result.Data->TryGetArrayField(TEXT("emitters"), Emitters)
			&& Emitters != nullptr
			&& Emitters->Num() >= 1);

	const TArray<TSharedPtr<FJsonValue>>* UserParameters = nullptr;
	TestTrue(
		TEXT("Spec exposes a userParameters array"),
		Result.Data->TryGetArrayField(TEXT("userParameters"), UserParameters)
			&& UserParameters != nullptr);

	TestFalse(TEXT("Read-only export never reports saved"), Result.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Read-only export never reports compiled"), Result.Data->GetBoolField(TEXT("compiled")));
	TestTrue(TEXT("Export reports bounded output"), Result.Data->GetBoolField(TEXT("bounded")));
	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
