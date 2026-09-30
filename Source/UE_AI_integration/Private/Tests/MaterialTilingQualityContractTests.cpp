#if WITH_DEV_AUTOMATION_TESTS

#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
	void RegisterMaterialUtilityTools(FMCPToolRegistry& Registry);
}

namespace
{
	TSharedRef<FJsonObject> TilingMaterialPathParams(const FString& AssetPath)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), AssetPath);
		return Params;
	}

	FMCPToolBase* FindTilingTool(
		FMCPToolRegistry& Registry,
		const TCHAR* CapabilityId)
	{
		return Registry.FindTool(CapabilityId);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialTilingQualityContractTest,
	"UE_AI_integration.MaterialUtility.TilingQualityReadOnlyContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialTilingQualityContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialUtilityTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* TilingTool = FindTilingTool(
		Registry,
		TEXT("content.material.tiling.quality"));
	if (!TestNotNull(TEXT("Material tiling quality tool is registered"), TilingTool))
	{
		return false;
	}

	// Invalid input must fail before any object lookup or editor/material work.
	const FMCPToolResult MissingPath = TilingTool->Execute(
		TilingMaterialPathParams(TEXT("Material/RelativePath")));
	TestFalse(TEXT("Tiling quality rejects a relative asset path"), MissingPath.bSuccess);
	TestEqual(
		TEXT("Relative path uses the shared material-not-found boundary"),
		MissingPath.ErrorCode,
		FString(TEXT("material_not_found")));
	TestEqual(
		TEXT("Relative path reports HTTP not-found status"),
		MissingPath.HttpStatus,
		404);

	const FString MissingAssetPath =
		TEXT("/Game/Automation/UEAI_MaterialTiling_Missing_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
		+ TEXT(".Material");
	const FMCPToolResult MissingAsset = TilingTool->Execute(
		TilingMaterialPathParams(MissingAssetPath));
	TestFalse(TEXT("Tiling quality rejects a missing material asset"), MissingAsset.bSuccess);
	TestEqual(
		TEXT("Missing material uses the material-not-found boundary"),
		MissingAsset.ErrorCode,
		FString(TEXT("material_not_found")));
	TestEqual(
		TEXT("Missing material reports HTTP not-found status"),
		MissingAsset.HttpStatus,
		404);

	const FString PackageName =
		TEXT("/Game/Automation/UEAI_MaterialTiling_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString PackageFilename;
	if (!TestTrue(
		TEXT("Material fixture package has a valid persistence path"),
		FPackageName::TryConvertLongPackageNameToFilename(
			PackageName,
			PackageFilename,
			FPackageName::GetAssetPackageExtension())))
	{
		return false;
	}
	TestFalse(
		TEXT("Material fixture is not saved before analysis"),
		IFileManager::Get().FileExists(*PackageFilename));
	UPackage* Package = CreatePackage(*PackageName);
	UMaterial* Material = Package
		                      ? NewObject<UMaterial>(
			                      Package,
			                      TEXT("Material"),
			                      RF_Public | RF_Standalone | RF_Transactional)
		                      : nullptr;
	FGCObjectScopeGuard MaterialGuard(Material);
	if (!TestNotNull(TEXT("In-memory material fixture exists"), Material))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Package->SetDirtyFlag(false);
		Material->ClearFlags(RF_Public | RF_Standalone);
		Material->MarkAsGarbage();
		if (IFileManager::Get().FileExists(*PackageFilename))
		{
			IFileManager::Get().Delete(*PackageFilename, false, true, true);
		}
	};

	// A texture sample with default coordinates gives the analyzer one bounded,
	// deterministic direct-UV finding without requiring shader compilation.
	UMaterialExpressionTextureSample* TextureSample = NewObject<
		UMaterialExpressionTextureSample>(
		Material,
		TEXT("TextureSample"),
		RF_Transactional);
	if (!TestNotNull(TEXT("Texture sample fixture exists"), TextureSample))
	{
		return false;
	}
	Material->GetExpressionCollection().AddExpression(TextureSample);
	TextureSample->Material = Material;
	Package->SetDirtyFlag(false);

	const EObjectFlags BeforeFlags = Material->GetFlags();
	const EObjectFlags BeforeExpressionFlags = TextureSample->GetFlags();
	const uint32 BeforePackageFlags = Package->GetPackageFlags();
	const bool bDirtyBefore = Package->IsDirty();
	auto* ResourceBefore =
		Material->GetMaterialResource(GMaxRHIFeatureLevel);
	const FMCPToolResult Result = TilingTool->Execute(
		TilingMaterialPathParams(Material->GetPathName()));

	if (!TestTrue(TEXT("Tiling quality reads the material fixture"), Result.bSuccess)
		|| !TestNotNull(TEXT("Tiling quality returns data"), Result.Data.Get()))
	{
		return false;
	}

	TestEqual(
		TEXT("Tiling quality resolves the requested material"),
		Result.Data->GetStringField(TEXT("assetPath")),
		Material->GetPathName());
	TestEqual(
		TEXT("Tiling quality reports the material class"),
		Result.Data->GetStringField(TEXT("assetClass")),
		FString(TEXT("Material")));
	TestEqual(
		TEXT("Tiling quality reports one inspected expression"),
		Result.Data->GetIntegerField(TEXT("expressionCount")),
		1);
	TestEqual(
		TEXT("Tiling quality reports one texture sample"),
		Result.Data->GetIntegerField(TEXT("textureSampleCount")),
		1);
	TestEqual(
		TEXT("Direct UV analysis reports one tiling issue"),
		Result.Data->GetIntegerField(TEXT("tilingIssueCount")),
		1);
	TestFalse(
		TEXT("Default fixture has no anti-tiling signal"),
		Result.Data->GetBoolField(TEXT("hasAntiTiling")));
	TestFalse(
		TEXT("Tiling analysis is not truncated"),
		Result.Data->GetBoolField(TEXT("analysisTruncated")));
	TestFalse(
		TEXT("Read-only tiling analysis does not trigger compilation"),
		Result.Data->GetBoolField(TEXT("compileTriggered")));
	TestFalse(
		TEXT("Read-only tiling analysis does not save"),
		Result.Data->GetBoolField(TEXT("saved")));
	TestEqual(
		TEXT("Tiling analysis preserves material object flags"),
		Material->GetFlags(),
		BeforeFlags);
	TestEqual(
		TEXT("Tiling analysis preserves expression object flags"),
		TextureSample->GetFlags(),
		BeforeExpressionFlags);
	TestEqual(
		TEXT("Tiling analysis preserves package flags"),
		Package->GetPackageFlags(),
		BeforePackageFlags);
	TestEqual(
		TEXT("Tiling analysis preserves package dirty state"),
		Package->IsDirty(),
		bDirtyBefore);
	TestTrue(
		TEXT("Tiling analysis does not materialize or replace a compile resource"),
		Material->GetMaterialResource(GMaxRHIFeatureLevel) == ResourceBefore);
	TestFalse(
		TEXT("Tiling analysis does not save a package file"),
		IFileManager::Get().FileExists(*PackageFilename));

	return true;
}

#endif
