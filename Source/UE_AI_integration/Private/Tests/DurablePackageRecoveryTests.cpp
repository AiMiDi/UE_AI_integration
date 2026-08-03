#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Infrastructure/DurablePackageRecovery.h"
#include "Infrastructure/Sha256.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
using UEAIIntegration::Infrastructure::FDurablePackageRecovery;
using UEAIIntegration::Infrastructure::FDurablePackageRecoveryRequest;

bool SaveTexturePackage(UTexture2D* Texture, UPackage* Package)
{
	if (!Texture || !Package)
	{
		return false;
	}
	Package->MarkPackageDirty();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(
		Package,
		Texture,
		*Filename,
		SaveArgs);
	if (bSaved)
	{
		Package->ClearPackageFlags(PKG_NewlyCreated);
		Package->SetDirtyFlag(false);
	}
	return bSaved;
}

UTexture2D* CreateRecoveryTexture(
	const FString& PackageName,
	const bool bSrgb,
	UPackage*& OutPackage)
{
	OutPackage = CreatePackage(*PackageName);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UTexture2D* Texture = NewObject<UTexture2D>(
		OutPackage,
		*AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	const uint8 Pixel[4] = {255, 255, 255, 255};
	Texture->Source.Init(1, 1, 1, 1, TSF_BGRA8, Pixel);
	Texture->SRGB = bSrgb;
	FAssetRegistryModule::AssetCreated(Texture);
	return Texture;
}

FString FileHash(const FString& Filename)
{
	TArray<uint8> Bytes;
	FString Hash;
	return FFileHelper::LoadFileToArray(Bytes, *Filename)
		&& UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, Hash)
		? TEXT("sha256:") + Hash
		: FString();
}

void UnloadFixture(const FString& PackageName)
{
	if (UPackage* Package = FindPackage(nullptr, *PackageName))
	{
		Package->SetDirtyFlag(false);
		TArray<UPackage*> Packages{Package};
		UPackageTools::FUnloadPackageParams Params(Packages);
		Params.bUnloadDirtyPackages = true;
		Params.bResetTransBuffer = false;
		UPackageTools::UnloadPackages(Params);
	}
}

void DeleteFixture(const FString& PackageName, const FString& ChangeSetId)
{
	UnloadFixture(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*Filename, false, true, true);
	IFileManager::Get().DeleteDirectory(
		*FPaths::GetPath(FDurablePackageRecovery::ManifestPath(ChangeSetId)),
		false,
		true);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDurablePackageRecoveryRestartRollbackTest,
	"UE_AI_integration.Recovery.Package.RestartRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDurablePackageRecoveryRestartRollbackTest::RunTest(const FString&)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FString PackageName = TEXT("/Game/__UEAIRecoveryTests/T_Recovery_") + Suffix;
	const FString ObjectPath = PackageName + TEXT(".")
		+ FPackageName::GetLongPackageAssetName(PackageName);
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	const FString ChangeSetId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	UPackage* Package = nullptr;
	UTexture2D* Texture = CreateRecoveryTexture(PackageName, true, Package);
	if (!TestTrue(TEXT("Initial recovery fixture saves"), SaveTexturePackage(Texture, Package)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}
	const FString OriginalFileHash = FileHash(Filename);

	FDurablePackageRecoveryRequest Request;
	Request.ChangeSetId = ChangeSetId;
	Request.Kind = TEXT("test.texture.settings");
	Request.RequestId = TEXT("durable-recovery-prepare");
	Request.PlanDigest = FString::ChrN(64, TEXT('a'));
	Request.ContractDigest = FString::ChrN(64, TEXT('b'));
	Request.BeforeHash = OriginalFileHash;
	Request.PackageNames = {PackageName};
	TSharedPtr<FJsonObject> Manifest;
	FString ErrorCode;
	FString Error;
	if (!TestTrue(
			FString::Printf(TEXT("Prepare succeeds (%s: %s)"), *ErrorCode, *Error),
			FDurablePackageRecovery::Prepare(
				Request,
				Manifest,
				ErrorCode,
				Error)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}

	Texture->SRGB = false;
	Texture->PostEditChange();
	if (!TestTrue(TEXT("Changed recovery fixture saves"), SaveTexturePackage(Texture, Package)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}
	const FString ChangedFileHash = FileHash(Filename);
	TestNotEqual(TEXT("The change produces a different package hash"), ChangedFileHash, OriginalFileHash);
	if (!TestTrue(
			TEXT("Post-change state seals"),
			FDurablePackageRecovery::Seal(
				ChangeSetId,
				TEXT("completed"),
				ChangedFileHash,
				Manifest,
				ErrorCode,
				Error)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}

	Texture = nullptr;
	Package = nullptr;
	TSharedPtr<FJsonObject> Rollback;
	if (!TestTrue(
			FString::Printf(TEXT("Restart-style rollback succeeds (%s: %s)"), *ErrorCode, *Error),
			FDurablePackageRecovery::Rollback(
				ChangeSetId,
				TEXT("durable-recovery-rollback"),
				Rollback,
				ErrorCode,
				Error)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}
	TestTrue(TEXT("Rollback is checksum verified"), Rollback->GetBoolField(TEXT("rollbackVerified")));
	TestEqual(TEXT("Original package bytes are restored"), FileHash(Filename), OriginalFileHash);
	UTexture2D* Restored = LoadObject<UTexture2D>(nullptr, *ObjectPath);
	TestNotNull(TEXT("Restored package reloads"), Restored);
	if (Restored)
	{
		TestTrue(TEXT("Restored object has its original setting"), Restored->SRGB);
	}

	Restored = nullptr;
	Rollback.Reset();
	TestTrue(
		TEXT("Rollback is idempotent after restart"),
		FDurablePackageRecovery::Rollback(
			ChangeSetId,
			TEXT("durable-recovery-idempotent"),
			Rollback,
			ErrorCode,
			Error));
	TestEqual(
		TEXT("Idempotent rollback is explicit"),
		Rollback->GetStringField(TEXT("status")),
		FString(TEXT("alreadyRolledBack")));

	DeleteFixture(PackageName, ChangeSetId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDurablePackageRecoveryNewAssetAndConflictTest,
	"UE_AI_integration.Recovery.Package.NewAssetAndConflict",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDurablePackageRecoveryNewAssetAndConflictTest::RunTest(const FString&)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12);
	const FString PackageName = TEXT("/Game/__UEAIRecoveryTests/T_New_") + Suffix;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	const FString ChangeSetId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);

	FDurablePackageRecoveryRequest Request;
	Request.ChangeSetId = ChangeSetId;
	Request.Kind = TEXT("test.asset.create");
	Request.RequestId = TEXT("durable-new-prepare");
	Request.PlanDigest = FString::ChrN(64, TEXT('c'));
	Request.ContractDigest = FString::ChrN(64, TEXT('d'));
	Request.BeforeHash = TEXT("sha256:absent");
	Request.PackageNames = {PackageName};
	TSharedPtr<FJsonObject> Manifest;
	FString ErrorCode;
	FString Error;
	if (!TestTrue(
			TEXT("Absent package can be prepared"),
			FDurablePackageRecovery::Prepare(
				Request,
				Manifest,
				ErrorCode,
				Error)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}

	UPackage* Package = nullptr;
	UTexture2D* Texture = CreateRecoveryTexture(PackageName, true, Package);
	if (!TestTrue(TEXT("New recovery fixture saves"), SaveTexturePackage(Texture, Package)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}
	const FString SealedHash = FileHash(Filename);
	const FString SealedCopy = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration/RecoveryTests"),
		FPackageName::GetLongPackageAssetName(PackageName) + TEXT(".sealed.uasset"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SealedCopy), true);
	TestEqual(
		TEXT("Sealed package bytes are retained for exact conflict recovery"),
		IFileManager::Get().Copy(*SealedCopy, *Filename, true, true),
		COPY_OK);
	if (!TestTrue(
			TEXT("New asset state seals"),
			FDurablePackageRecovery::Seal(
				ChangeSetId,
				TEXT("completed"),
				SealedHash,
				Manifest,
				ErrorCode,
				Error)))
	{
		DeleteFixture(PackageName, ChangeSetId);
		return false;
	}

	Texture->SRGB = false;
	Texture->PostEditChange();
	TestTrue(TEXT("External package edit saves"), SaveTexturePackage(Texture, Package));
	Texture = nullptr;
	Package = nullptr;
	TSharedPtr<FJsonObject> Rollback;
	TestFalse(
		TEXT("Rollback refuses external post-change edits"),
		FDurablePackageRecovery::Rollback(
			ChangeSetId,
			TEXT("durable-new-conflict"),
			Rollback,
			ErrorCode,
			Error));
	TestEqual(
		TEXT("External edit uses rollback_conflict"),
		ErrorCode,
		FString(TEXT("rollback_conflict")));

	UnloadFixture(PackageName);
	TestEqual(
		TEXT("Exact sealed bytes can be restored after a conflict"),
		IFileManager::Get().Copy(*Filename, *SealedCopy, true, true),
		COPY_OK);
	TestEqual(TEXT("Restored package matches the sealed hash"), FileHash(Filename), SealedHash);
	Rollback.Reset();
	const bool bRollbackSucceeded = FDurablePackageRecovery::Rollback(
			ChangeSetId,
			TEXT("durable-new-rollback"),
			Rollback,
			ErrorCode,
			Error);
	TestTrue(
		FString::Printf(
			TEXT("Rollback deletes a Workflow-created package (%s: %s)"),
			*ErrorCode,
			*Error),
		bRollbackSucceeded);
	TestFalse(TEXT("New package file is removed"), IFileManager::Get().FileExists(*Filename));
	IFileManager::Get().Delete(*SealedCopy, false, true, true);

	DeleteFixture(PackageName, ChangeSetId);
	return true;
}

#endif
