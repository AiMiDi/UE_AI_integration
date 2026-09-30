#if WITH_DEV_AUTOMATION_TESTS

#include "Materials/Material.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
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
	TSharedRef<FJsonObject> MaterialPathParams(const FString& AssetPath)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), AssetPath);
		return Params;
	}

	TSharedRef<FJsonObject> ApprovedWorkflowParams(const FString& AssetPath)
	{
		TSharedRef<FJsonObject> Params = MaterialPathParams(AssetPath);
		TSharedRef<FJsonObject> WorkflowContext = MakeShared<FJsonObject>();
		WorkflowContext->SetBoolField(TEXT("approvedPlan"), true);
		Params->SetObjectField(TEXT("__ueWorkflow"), WorkflowContext);
		return Params;
	}

	FMCPToolBase* FindTool(FMCPToolRegistry& Registry, const TCHAR* CapabilityId)
	{
		return Registry.FindTool(CapabilityId);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialUtilityContractTest,
	"UE_AI_integration.MaterialUtility.ReadValidationAndWorkflowSaveBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialUtilityContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialUtilityTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* SaveTool = FindTool(Registry, TEXT("content.material.save"));
	FMCPToolBase* RecompileTool = FindTool(Registry, TEXT("content.material.recompile"));
	FMCPToolBase* StatsTool = FindTool(Registry, TEXT("content.material.compilation.stats"));
	FMCPToolBase* TextureTool = FindTool(Registry, TEXT("content.material.texture.properties"));
	FMCPToolBase* ImportTextureTool = FindTool(Registry, TEXT("content.material.texture.import"));
	if (!TestNotNull(TEXT("Material save utility is registered"), SaveTool)
		|| !TestNotNull(TEXT("Material recompile utility is registered"), RecompileTool)
		|| !TestNotNull(TEXT("Material compilation stats utility is registered"), StatsTool)
		|| !TestNotNull(TEXT("Material texture utility is registered"), TextureTool)
		|| !TestNotNull(TEXT("Material texture import utility is registered"), ImportTextureTool))
	{
		return false;
	}

	const FMCPToolResult MissingImportSource = ImportTextureTool->Execute(MakeShared<FJsonObject>());
	TestFalse(TEXT("Texture import rejects a missing source before creating an asset"), MissingImportSource.bSuccess);
	TestEqual(TEXT("Missing import source uses the validation error boundary"),
	          MissingImportSource.ErrorCode, FString(TEXT("invalid_material_utility")));

	const FMCPToolResult RelativePath =
		SaveTool->Execute(MaterialPathParams(TEXT("Material/RelativePath")));
	TestFalse(TEXT("Save rejects a relative Unreal path"), RelativePath.bSuccess);
	TestEqual(TEXT("Relative path uses the material-not-found boundary"),
	          RelativePath.ErrorCode,
	          FString(TEXT("material_not_found")));

	const FString DefaultTexturePath =
		TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture");
	const FMCPToolResult WrongAssetType =
		SaveTool->Execute(MaterialPathParams(DefaultTexturePath));
	TestFalse(TEXT("Save rejects a texture asset"), WrongAssetType.bSuccess);
	TestEqual(TEXT("Wrong asset type uses the material-not-found boundary"),
	          WrongAssetType.ErrorCode,
	          FString(TEXT("material_not_found")));

	const FString TransientName =
		TEXT("UEAI_MaterialUtility_Transient_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString TransientPackageName =
		TEXT("/Game/Automation/UEAI_MaterialUtility_Transient_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* TransientPackage = CreatePackage(*TransientPackageName);
	UMaterial* TransientMaterial = NewObject<UMaterial>(
		TransientPackage,
		*TransientName,
		RF_Public | RF_Standalone | RF_Transient | RF_Transactional);
	FGCObjectScopeGuard TransientMaterialGuard(TransientMaterial);
	ON_SCOPE_EXIT
	{
		if (TransientPackage)
		{
			TransientPackage->SetDirtyFlag(false);
		}
		if (TransientMaterial)
		{
			TransientMaterial->MarkAsGarbage();
		}
	};
	TestNotNull(TEXT("Transient material fixture exists"), TransientMaterial);
	if (!TransientMaterial)
	{
		return false;
	}
	const FMCPToolResult NonPersistable =
		SaveTool->Execute(MaterialPathParams(TransientMaterial->GetPathName()));
	TestFalse(TEXT("Save rejects a transient material before persistence"),
	          NonPersistable.bSuccess);
	TestEqual(TEXT("Transient save has a stable persistence error"),
	          NonPersistable.ErrorCode,
	          FString(TEXT("material_save_not_persistable")));

	const FString PackageName =
		TEXT("/Game/Automation/UEAI_MaterialUtility_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
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
	};
	Package->SetDirtyFlag(false);

	const bool bCleanBeforeStats = Package->IsDirty();
	const FMCPToolResult InitialStats =
		StatsTool->Execute(MaterialPathParams(Material->GetPathName()));
	if (TestTrue(TEXT("Compilation stats read an in-memory material"), InitialStats.bSuccess)
		&& TestNotNull(TEXT("Compilation stats data exists"), InitialStats.Data.Get()))
	{
		TestFalse(TEXT("Stats response reports no compile request"),
		          InitialStats.Data->GetBoolField(TEXT("compileTriggered")));
		TestTrue(TEXT("Stats response exposes resource state"),
		         InitialStats.Data->HasField(TEXT("resourceAvailable")));
		TestEqual(TEXT("Stats query preserves the material package dirty state"),
		          Package->IsDirty(), bCleanBeforeStats);
	}

	UTexture2D* DefaultTexture = LoadObject<UTexture2D>(
		nullptr,
		*DefaultTexturePath,
		nullptr,
		LOAD_NoWarn);
	if (!TestNotNull(TEXT("Default texture fixture exists"), DefaultTexture))
	{
		return false;
	}
	UPackage* TexturePackage = DefaultTexture->GetOutermost();
	const bool bTextureDirtyBeforeQuery =
		TexturePackage && TexturePackage->IsDirty();
	const FMCPToolResult TextureProperties =
		TextureTool->Execute(MaterialPathParams(DefaultTexturePath));
	if (TestTrue(TEXT("Texture properties read the engine texture fixture"),
	             TextureProperties.bSuccess)
		&& TestNotNull(TEXT("Texture properties data exists"), TextureProperties.Data.Get()))
	{
		TestTrue(TEXT("Texture properties expose a texture class"),
		         TextureProperties.Data->HasTypedField<EJson::String>(TEXT("assetClass")));
		TestTrue(TEXT("Texture properties expose dimensions"),
		         TextureProperties.Data->HasField(TEXT("width"))
		         && TextureProperties.Data->HasField(TEXT("height")));
		TestEqual(TEXT("Texture query preserves the texture package dirty state"),
		          TexturePackage && TexturePackage->IsDirty(), bTextureDirtyBeforeQuery);
	}

	Package->SetDirtyFlag(true);
	const bool bDirtyBeforeStats = Package->IsDirty();
	const FMCPToolResult DirtyStats =
		StatsTool->Execute(MaterialPathParams(Material->GetPathName()));
	TestTrue(TEXT("Stats query still succeeds for a dirty material"), DirtyStats.bSuccess);
	TestEqual(TEXT("Read-only stats preserve an existing dirty package"),
	          Package->IsDirty(), bDirtyBeforeStats);

	const FMCPToolResult WorkflowSave =
		SaveTool->Execute(ApprovedWorkflowParams(Material->GetPathName()));
	TestFalse(TEXT("Immediate save is refused inside approved Workflow execution"),
	          WorkflowSave.bSuccess);
	TestEqual(TEXT("Workflow save refusal has a stable error code"),
	          WorkflowSave.ErrorCode,
	          FString(TEXT("material_save_workflow_forbidden")));
	TestTrue(TEXT("Workflow save refusal leaves the dirty package untouched"),
	         Package->IsDirty());

	return true;
}

#endif
