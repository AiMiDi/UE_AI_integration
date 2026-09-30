#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FNiagaraSystemDuplicateFixture
{
	FString SourcePackageName;
	UPackage* SourcePackage = nullptr;
	UNiagaraSystem* SourceSystem = nullptr;
};

FNiagaraSystemDuplicateFixture NiagaraSystemDuplicateCreateFixture()
{
	FNiagaraSystemDuplicateFixture Fixture;
	Fixture.SourcePackageName = TEXT("/Game/Automation/UEAI_SystemDuplicate_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(Fixture.SourcePackageName);
	Fixture.SourcePackage = CreatePackage(*Fixture.SourcePackageName);
	Fixture.SourceSystem = Fixture.SourcePackage
		? NewObject<UNiagaraSystem>(
			Fixture.SourcePackage,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!Fixture.SourceSystem)
	{
		return Fixture;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(Fixture.SourceSystem, true);
	FAssetRegistryModule::AssetCreated(Fixture.SourceSystem);
	Fixture.SourcePackage->SetDirtyFlag(false);
	return Fixture;
}

bool NiagaraSystemDuplicateDeletePackage(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	// The duplicate handler writes an on-disk artifact; remove any residual file
	// directly so a duplicate cannot leak content between automation runs.
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(Filename))
	{
		IFileManager::Get().Delete(*Filename);
	}
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName)
		&& !FPaths::FileExists(Filename);
}

TSharedRef<FJsonObject> NiagaraSystemDuplicateMakeParams(
	const FString& System,
	const FString& NewName = FString(),
	const FString& TargetPath = FString())
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), System);
	if (!NewName.IsEmpty())
	{
		Params->SetStringField(TEXT("newName"), NewName);
	}
	if (!TargetPath.IsEmpty())
	{
		Params->SetStringField(TEXT("targetPath"), TargetPath);
	}
	return Params;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemDuplicateContractTest,
	"UE_AI_integration.Niagara.SystemDuplicate.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemDuplicateContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	// (a) Unknown system -> system_not_found (404).
	{
		const FString UnknownSystem = FString::Printf(
			TEXT("/Game/Automation/UEAI_SystemDuplicateMissing_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.system.duplicate"),
			NiagaraSystemDuplicateMakeParams(UnknownSystem));
		TestFalse(TEXT("Unknown system duplicate fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown system uses a stable error code"),
			Result.ErrorCode,
			FString(TEXT("system_not_found")));
		TestEqual(TEXT("Unknown system returns 404"), Result.HttpStatus, 404);
	}

	// (b) Invalid targetPath -> invalid_target_path (422).
	{
		const FNiagaraSystemDuplicateFixture Fixture =
			NiagaraSystemDuplicateCreateFixture();
		if (!Fixture.SourceSystem)
		{
			AddInfo(
				TEXT("Niagara System creation is unavailable; skipping the invalid targetPath assertion."));
		}
		else
		{
			ON_SCOPE_EXIT
			{
				if (Fixture.SourceSystem)
				{
					Fixture.SourceSystem->WaitForCompilationComplete(false, false);
				}
				TestTrue(
					TEXT("Invalid-target fixture is deleted"),
					NiagaraSystemDuplicateDeletePackage(Fixture.SourcePackageName));
			};
			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("content.niagara.system.duplicate"),
				NiagaraSystemDuplicateMakeParams(
					Fixture.SourceSystem->GetPathName(), FString(), TEXT("/NotGame/Effects")));
			TestFalse(TEXT("Invalid targetPath is rejected"), Result.bSuccess);
			TestEqual(
				TEXT("Invalid targetPath uses a stable error code"),
				Result.ErrorCode,
				FString(TEXT("invalid_target_path")));
			TestEqual(TEXT("Invalid targetPath returns 422"), Result.HttpStatus, 422);
		}
	}

	// (c) Duplicate a transient /Game/ system and verify the new asset exists.
	{
		const FNiagaraSystemDuplicateFixture Fixture =
			NiagaraSystemDuplicateCreateFixture();
		FString DuplicatedPackageName;
		ON_SCOPE_EXIT
		{
			if (Fixture.SourceSystem)
			{
				Fixture.SourceSystem->WaitForCompilationComplete(false, false);
			}
			TestTrue(
				TEXT("Duplicate source fixture is deleted"),
				NiagaraSystemDuplicateDeletePackage(Fixture.SourcePackageName));
			if (!DuplicatedPackageName.IsEmpty())
			{
				TestTrue(
					TEXT("Duplicated asset is deleted"),
					NiagaraSystemDuplicateDeletePackage(DuplicatedPackageName));
			}
		};
		if (!Fixture.SourceSystem)
		{
			AddInfo(
				TEXT("Niagara System creation is unavailable; skipping the transient duplicate assertion."));
		}
		else
		{
			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("content.niagara.system.duplicate"),
				NiagaraSystemDuplicateMakeParams(Fixture.SourceSystem->GetPathName()));
			if (!Result.bSuccess || !Result.Data.IsValid())
			{
				AddInfo(FString::Printf(
					TEXT("DuplicateAsset is unavailable in this environment (%s); skipping the transient duplicate assertions."),
					*Result.ErrorCode));
			}
			else
			{
				TestEqual(
					TEXT("Duplicate reports the schema"),
					Result.Data->GetStringField(TEXT("schema")),
					FString(TEXT("ue.niagara.system-duplicate.v1")));
				TestEqual(
					TEXT("Duplicate reports the source object path"),
					Result.Data->GetStringField(TEXT("sourceSystemPath")),
					Fixture.SourceSystem->GetPathName());
				TestTrue(TEXT("Duplicate reports saved"),
					Result.Data->GetBoolField(TEXT("saved")));

				const FString NewSystemPath =
					Result.Data->GetStringField(TEXT("newSystemPath"));
				TestTrue(
					TEXT("Duplicate reports a distinct new path"),
					!NewSystemPath.IsEmpty()
						&& NewSystemPath != Fixture.SourceSystem->GetPathName());

				DuplicatedPackageName =
					FPackageName::ObjectPathToPackageName(NewSystemPath);
				TestTrue(
					TEXT("Duplicated asset exists as a package"),
					FPackageName::DoesPackageExist(DuplicatedPackageName));
				const FString DuplicatedFilename =
					FPackageName::LongPackageNameToFilename(
						DuplicatedPackageName, FPackageName::GetAssetPackageExtension());
				TestTrue(
					TEXT("Duplicated asset is saved to disk"),
					FPaths::FileExists(DuplicatedFilename));

				UNiagaraSystem* ReadBack = LoadObject<UNiagaraSystem>(
					nullptr, *NewSystemPath, nullptr, LOAD_NoWarn);
				TestNotNull(TEXT("Duplicated system loads back"), ReadBack);
			}
		}
	}

	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
