#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterContentAssetChangeTools(FMCPToolRegistry& Registry);
void RegisterContentAssetSettingsTools(FMCPToolRegistry& Registry);
}

namespace
{
template <typename T>
T* CreateFixtureAsset(
	const FString& Prefix,
	UPackage*& OutPackage)
{
	const FString Suffix =
		FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FString AssetName = Prefix + Suffix;
	const FString PackageName =
		FString::Printf(TEXT("/Game/__UEAISettingsTests/%s"), *AssetName);
	OutPackage = CreatePackage(*PackageName);
	T* Asset = NewObject<T>(
		OutPackage,
		*AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	OutPackage->MarkPackageDirty();
	return Asset;
}

UStaticMesh* CreateStaticMeshFixture(UPackage*& OutPackage)
{
	const FString Suffix =
		FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FString AssetName = FString(TEXT("SM_Settings_")) + Suffix;
	const FString PackageName =
		FString::Printf(TEXT("/Game/__UEAISettingsTests/%s"), *AssetName);
	OutPackage = CreatePackage(*PackageName);
	UStaticMesh* Template = LoadObject<UStaticMesh>(
		nullptr,
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Asset = Template
		? Cast<UStaticMesh>(
			StaticDuplicateObject(
				Template,
				OutPackage,
				*AssetName,
				RF_Public | RF_Standalone | RF_Transactional))
		: NewObject<UStaticMesh>(
			OutPackage,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
	if (!Asset)
	{
		Asset = NewObject<UStaticMesh>(
			OutPackage,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
	}
	Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	OutPackage->MarkPackageDirty();
	return Asset;
}

void DestroyFixtureAsset(UObject* Asset, UPackage* Package)
{
	if (Asset)
	{
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
	}
	if (Package)
	{
		Package->SetDirtyFlag(false);
	}
}

TSharedRef<FJsonObject> MakeSettingsRequest(
	UObject* Asset,
	const TSharedRef<FJsonObject>& Settings,
	const FString& Persistence = TEXT("dirtyOnly"))
{
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("asset"), Asset->GetPathName());
	Request->SetObjectField(TEXT("settings"), Settings);
	Request->SetStringField(TEXT("persistence"), Persistence);
	return Request;
}

TSharedRef<FJsonObject> MakePlanParams(
	const TSharedRef<FJsonObject>& Request)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("request"), Request);
	return Params;
}

TSharedRef<FJsonObject> MakeApplyParams(
	const TSharedRef<FJsonObject>& Request,
	const FString& Digest,
	const FString& RequestId)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("request"), Request);
	Params->SetStringField(TEXT("approvePlanDigest"), Digest);
	Params->SetStringField(TEXT("requestId"), RequestId);
	Params->SetBoolField(TEXT("confirmWrite"), true);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FContentAssetSettingsRegistrarTest,
	"UE_AI_integration.Content.Settings.Registrar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FContentAssetSettingsRegistrarTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetSettingsTools(Registry);
	Registry.EndDomainRegistration();

	TestEqual(TEXT("Ten settings tools register"), Registry.Num(), 10);
	const TCHAR* ExpectedIds[] = {
		TEXT("content.static_mesh.settings.get"),
		TEXT("content.static_mesh.settings.plan"),
		TEXT("content.static_mesh.settings.apply"),
		TEXT("content.static_mesh.settings.rollback"),
		TEXT("content.static_mesh.settings.validate"),
		TEXT("content.texture.settings.get"),
		TEXT("content.texture.settings.plan"),
		TEXT("content.texture.settings.apply"),
		TEXT("content.texture.settings.rollback"),
		TEXT("content.texture.settings.validate"),
	};
	for (const TCHAR* Id : ExpectedIds)
	{
		TestNotNull(Id, Registry.FindTool(Id));
	}

	TSharedRef<FJsonObject> Missing = MakeShared<FJsonObject>();
	Missing->SetStringField(
		TEXT("asset"),
		TEXT("/Game/__UEAISettingsTests/Missing"));
	const FMCPToolResult MissingResult = Registry.ExecuteTool(
		TEXT("content.texture.settings.get"),
		Missing);
	TestFalse(TEXT("Missing asset is rejected"), MissingResult.bSuccess);
	TestEqual(
		TEXT("Missing asset error is stable"),
		MissingResult.ErrorCode,
		FString(TEXT("asset_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FContentStaticMeshSettingsRoundTripTest,
	"UE_AI_integration.Content.Settings.StaticMeshRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FContentStaticMeshSettingsRoundTripTest::RunTest(
	const FString& Parameters)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetSettingsTools(Registry);
	Registry.EndDomainRegistration();

	UPackage* Package = nullptr;
	UStaticMesh* Mesh = CreateStaticMeshFixture(Package);
	const bool bBefore = Mesh->bAllowCPUAccess;

	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetBoolField(TEXT("allowCpuAccess"), !bBefore);
	const TSharedRef<FJsonObject> Request =
		MakeSettingsRequest(Mesh, Settings);
	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.static_mesh.settings.plan"),
		MakePlanParams(Request));
	TestTrue(TEXT("Static Mesh plan succeeds"), Plan.bSuccess);
	if (!Plan.bSuccess || !Plan.Data.IsValid())
	{
		DestroyFixtureAsset(Mesh, Package);
		return false;
	}
	TestEqual(
		TEXT("Plan uses shared change-plan contract"),
		Plan.Data->GetStringField(TEXT("schema")),
		FString(TEXT("ue.change-plan.v1")));
	TestEqual(
		TEXT("Plan digest is SHA-256 sized"),
		Plan.Data->GetStringField(TEXT("planDigest")).Len(),
		64);

	const FMCPToolResult WrongDigest = Registry.ExecuteTool(
		TEXT("content.static_mesh.settings.apply"),
		MakeApplyParams(
			Request,
			FString::ChrN(64, TEXT('0')),
			TEXT("settings-static-wrong-digest")));
	TestFalse(TEXT("Mismatched digest is rejected"), WrongDigest.bSuccess);
	TestEqual(
		TEXT("Digest mismatch has stable error"),
		WrongDigest.ErrorCode,
		FString(TEXT("plan_digest_mismatch")));
	TestEqual(
		TEXT("Rejected apply does not mutate the mesh"),
		Mesh->bAllowCPUAccess,
		bBefore);

	const FMCPToolResult Apply = Registry.ExecuteTool(
		TEXT("content.static_mesh.settings.apply"),
		MakeApplyParams(
			Request,
			Plan.Data->GetStringField(TEXT("planDigest")),
			TEXT("settings-static-apply")));
	TestTrue(TEXT("Approved Static Mesh apply succeeds"), Apply.bSuccess);
	TestEqual(
		TEXT("Static Mesh setting changed"),
		Mesh->bAllowCPUAccess,
		!bBefore);
	if (Apply.bSuccess && Apply.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Mutation =
			Apply.Data->GetObjectField(TEXT("mutation"));
		TestTrue(
			TEXT("Static Mesh read-back is verified"),
			Mutation->GetBoolField(TEXT("verified")));
		TestFalse(
			TEXT("dirtyOnly does not save"),
			Mutation->GetBoolField(TEXT("saved")));
	}

	TSharedRef<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
	ValidateParams->SetStringField(TEXT("asset"), Mesh->GetPathName());
	ValidateParams->SetObjectField(TEXT("settings"), Settings);
	const FMCPToolResult Validate = Registry.ExecuteTool(
		TEXT("content.static_mesh.settings.validate"),
		ValidateParams);
	TestTrue(TEXT("Static Mesh validation executes"), Validate.bSuccess);
	if (Validate.bSuccess && Validate.Data.IsValid())
	{
		TestTrue(
			TEXT("Static Mesh matches expected settings"),
			Validate.Data->GetBoolField(TEXT("matchesExpected")));
	}

	DestroyFixtureAsset(Mesh, Package);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FContentTextureSettingsRoundTripTest,
	"UE_AI_integration.Content.Settings.TextureRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FContentTextureSettingsRoundTripTest::RunTest(
	const FString& Parameters)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetSettingsTools(Registry);
	Registry.EndDomainRegistration();

	UPackage* Package = nullptr;
	UTexture2D* Texture =
		CreateFixtureAsset<UTexture2D>(TEXT("T_Settings_"), Package);
	const uint8 Pixel[4] = {255, 255, 255, 255};
	Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, Pixel);
	const bool bBefore = Texture->SRGB;

	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetBoolField(TEXT("sRGB"), !bBefore);
	Settings->SetNumberField(TEXT("lodBias"), 1);
	const TSharedRef<FJsonObject> Request =
		MakeSettingsRequest(Texture, Settings);
	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.texture.settings.plan"),
		MakePlanParams(Request));
	TestTrue(TEXT("Texture plan succeeds"), Plan.bSuccess);
	if (!Plan.bSuccess || !Plan.Data.IsValid())
	{
		DestroyFixtureAsset(Texture, Package);
		return false;
	}

	const FMCPToolResult Apply = Registry.ExecuteTool(
		TEXT("content.texture.settings.apply"),
		MakeApplyParams(
			Request,
			Plan.Data->GetStringField(TEXT("planDigest")),
			TEXT("settings-texture-apply")));
	TestTrue(TEXT("Approved Texture apply succeeds"), Apply.bSuccess);
	TestEqual(TEXT("Texture sRGB changed"), Texture->SRGB, !bBefore);
	TestEqual(TEXT("Texture LOD bias changed"), Texture->LODBias, 1);

	TSharedRef<FJsonObject> SaveRequest =
		MakeSettingsRequest(Texture, Settings, TEXT("saveOnSuccess"));
	const FMCPToolResult DirtyPlan = Registry.ExecuteTool(
		TEXT("content.texture.settings.plan"),
		MakePlanParams(SaveRequest));
	TestFalse(
		TEXT("saveOnSuccess rejects a pre-dirty package"),
		DirtyPlan.bSuccess);
	TestEqual(
		TEXT("Dirty package error is stable"),
		DirtyPlan.ErrorCode,
		FString(TEXT("asset_dirty")));

	DestroyFixtureAsset(Texture, Package);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FContentAssetImportPathSafetyTest,
	"UE_AI_integration.Content.AssetChange.LocalImportPathSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FContentAssetImportPathSafetyTest::RunTest(
	const FString& Parameters)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetChangeTools(Registry);
	Registry.EndDomainRegistration();

	auto MakeImportPlan =
		[&Registry](const FString& SourceFile, const FString& Destination)
		{
			TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
			Action->SetStringField(TEXT("action"), TEXT("import"));
			Action->SetStringField(TEXT("sourceFile"), SourceFile);
			Action->SetStringField(TEXT("destination"), Destination);
			TArray<TSharedPtr<FJsonValue>> Actions = {
				MakeShared<FJsonValueObject>(Action),
			};
			TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
			Request->SetArrayField(TEXT("actions"), Actions);
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetObjectField(TEXT("request"), Request);
			return Registry.ExecuteTool(
				TEXT("content.asset.change.plan"),
				Params);
		};

	const FString Destination =
		FString(TEXT("/Game/__UEAISettingsTests/Imported_"))
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FMCPToolResult Relative =
		MakeImportPlan(TEXT("relative.fbx"), Destination);
	TestFalse(TEXT("Relative source is rejected"), Relative.bSuccess);
	TestEqual(
		TEXT("Relative source has plan error"),
		Relative.ErrorCode,
		FString(TEXT("plan_invalid")));

	const FMCPToolResult Network =
		MakeImportPlan(TEXT("\\\\server\\share\\mesh.fbx"), Destination);
	TestFalse(TEXT("Network source is rejected"), Network.bSuccess);
	TestEqual(
		TEXT("Network source has plan error"),
		Network.ErrorCode,
		FString(TEXT("plan_invalid")));

	const FString AbsoluteSavedDirectory =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
	FString TempFile = FPaths::CreateTempFilename(
		*AbsoluteSavedDirectory,
		TEXT("UEAIImportPlan"),
		TEXT(".txt"));
	FPaths::NormalizeFilename(TempFile);
	TestFalse(
		TEXT("Local plan fixture path is absolute"),
		FPaths::IsRelative(TempFile));
	TestTrue(
		TEXT("Local plan fixture is created"),
		FFileHelper::SaveStringToFile(TEXT("fixture"), *TempFile));
	const FMCPToolResult Local = MakeImportPlan(TempFile, Destination);
	TestTrue(
		FString::Printf(
			TEXT("Absolute local source can be planned (%s: %s)"),
			*Local.ErrorCode,
			*Local.ErrorMessage),
		Local.bSuccess);
	if (Local.bSuccess && Local.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Preconditions =
			Local.Data->GetArrayField(TEXT("preconditions"))[0]->AsObject();
		const TSharedPtr<FJsonObject> SourceState =
			Preconditions->GetObjectField(TEXT("sourceFile"));
		TestEqual(
			TEXT("Source state hash is SHA-256 sized"),
			SourceState->GetStringField(TEXT("stateHash")).Len(),
			64);
		TestFalse(
			TEXT("Normalized source is not relative"),
			FPaths::IsRelative(SourceState->GetStringField(TEXT("path"))));
		TestEqual(
			TEXT("Normalized source preserves the approved absolute path"),
			SourceState->GetStringField(TEXT("path")),
			TempFile);
		TestFalse(
			TEXT("Normalized source contains no parent traversal"),
			SourceState->GetStringField(TEXT("path")).Contains(TEXT("..")));
	}
	IFileManager::Get().Delete(*TempFile, false, true, true);
	return true;
}

#endif
