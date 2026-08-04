#include "Infrastructure/DurablePackageRecovery.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Sha256.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
constexpr int64 MaxRecoveryBytes = 20LL * 1024LL * 1024LL * 1024LL;
constexpr uint64 MinimumFreeHeadroom = 64ULL * 1024ULL * 1024ULL;
constexpr int32 RetentionDays = 7;

FString RecoveryRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration/Recovery")));
}

FString RecoveryStringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

bool JsonStringify(
	const TSharedPtr<FJsonObject>& Object,
	FString& OutText)
{
	OutText.Reset();
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&OutText);
	return Object.IsValid()
		&& FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}

bool WriteJsonAtomically(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Object,
	FString& OutError)
{
	FString Text;
	if (!JsonStringify(Object, Text))
	{
		OutError = TEXT("Recovery manifest could not be serialized.");
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
	{
		OutError = TEXT("Recovery directory could not be created.");
		return false;
	}
	const FString Temporary = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(
			Text,
			*Temporary,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(
			*Path,
			*Temporary,
			true,
			true,
			false,
			true))
	{
		IFileManager::Get().Delete(*Temporary, false, true, true);
		OutError = TEXT("Recovery manifest could not be published atomically.");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> ReadJson(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)
		|| Text.Len() > 16 * 1024 * 1024)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
		? Object
		: nullptr;
}

bool HashFile(const FString& Filename, FString& OutHash)
{
	OutHash.Reset();
	TArray<uint8> Bytes;
	FString Hex;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename)
		|| !TrySha256Hex(Bytes, Hex))
	{
		return false;
	}
	OutHash = TEXT("sha256:") + Hex;
	return true;
}

FString ExistingOrDefaultPackageFilename(const FString& PackageName)
{
	FString Existing;
	if (FPackageName::DoesPackageExist(PackageName, &Existing))
	{
		return FPaths::ConvertRelativePathToFull(Existing);
	}
	return FPaths::ConvertRelativePathToFull(
		FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension()));
}

TArray<FString> PackageFileCandidates(const FString& PackageName)
{
	const FString Main = ExistingOrDefaultPackageFilename(PackageName);
	TArray<FString> Files = {Main};
	const FString Base = FPaths::ChangeExtension(Main, TEXT(""));
	Files.Add(Base + TEXT(".uexp"));
	Files.Add(Base + TEXT(".ubulk"));
	Files.Add(Base + TEXT(".uptnl"));
	return Files;
}

int64 DirectoryBytes(const FString& Directory)
{
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(
		Files,
		*Directory,
		TEXT("*"),
		true,
		false);
	int64 Bytes = 0;
	for (const FString& File : Files)
	{
		Bytes += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
	}
	return Bytes;
}

bool CopyAndVerify(
	const FString& Source,
	const FString& Destination,
	FString& OutHash,
	FString& OutError)
{
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true)
		|| IFileManager::Get().Copy(
			*Destination,
			*Source,
			true,
			true) != COPY_OK
		|| !HashFile(Destination, OutHash))
	{
		OutError = FString::Printf(
			TEXT("Recovery snapshot '%s' could not be written and verified."),
			*Destination);
		return false;
	}
	FString SourceHash;
	if (!HashFile(Source, SourceHash) || SourceHash != OutHash)
	{
		OutError = FString::Printf(
			TEXT("Recovery snapshot '%s' does not match its source."),
			*Destination);
		return false;
	}
	return true;
}

bool RestoreFileAtomically(
	const FString& Snapshot,
	const FString& ExpectedHash,
	const FString& Destination,
	FString& OutError)
{
	FString SnapshotHash;
	if (!HashFile(Snapshot, SnapshotHash) || SnapshotHash != ExpectedHash)
	{
		OutError = FString::Printf(
			TEXT("Recovery snapshot '%s' is missing or corrupt."),
			*Snapshot);
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
	const FString Temporary = FString::Printf(
		TEXT("%s.ueai-restore-%s.tmp"),
		*Destination,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString TemporaryHash;
	if (IFileManager::Get().Copy(*Temporary, *Snapshot, true, true) != COPY_OK
		|| !HashFile(Temporary, TemporaryHash)
		|| TemporaryHash != ExpectedHash)
	{
		IFileManager::Get().Delete(*Temporary, false, true, true);
		OutError = TEXT("A verified temporary recovery file could not be created.");
		return false;
	}
	FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
		*Destination,
		false);
	if (!IFileManager::Get().Move(
			*Destination,
			*Temporary,
			true,
			true,
			false,
			true))
	{
		IFileManager::Get().Delete(*Temporary, false, true, true);
		OutError = FString::Printf(
			TEXT("Recovery file could not replace '%s'."),
			*Destination);
		return false;
	}
	FString RestoredHash;
	if (!HashFile(Destination, RestoredHash) || RestoredHash != ExpectedHash)
	{
		OutError = FString::Printf(
			TEXT("Restored file '%s' failed checksum validation."),
			*Destination);
		return false;
	}
	return true;
}

bool CurrentFileMatches(
	const TSharedPtr<FJsonObject>& File,
	const TCHAR* ExistsField,
	const TCHAR* HashField)
{
	bool bExpectedExists = false;
	File->TryGetBoolField(ExistsField, bExpectedExists);
	const FString Filename = RecoveryStringField(File, TEXT("filename"));
	const bool bExists = IFileManager::Get().FileExists(*Filename);
	if (bExists != bExpectedExists)
	{
		return false;
	}
	if (!bExists)
	{
		return true;
	}
	FString ActualHash;
	return HashFile(Filename, ActualHash)
		&& ActualHash == RecoveryStringField(File, HashField);
}

bool AllFilesMatch(
	const TSharedPtr<FJsonObject>& Manifest,
	const TCHAR* ExistsField,
	const TCHAR* HashField)
{
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!Manifest->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
	{
		if (!AssetValue.IsValid() || AssetValue->Type != EJson::Object)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!AssetValue->AsObject()->TryGetArrayField(TEXT("files"), Files)
			|| !Files)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& FileValue : *Files)
		{
			if (!FileValue.IsValid() || FileValue->Type != EJson::Object
				|| !CurrentFileMatches(
					FileValue->AsObject(),
					ExistsField,
					HashField))
			{
				return false;
			}
		}
	}
	return true;
}

bool ValidateExternalSources(
	const TSharedPtr<FJsonObject>& Manifest,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
	if (!Manifest->TryGetArrayField(TEXT("externalSources"), Sources)
		|| !Sources)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Sources)
	{
		const TSharedPtr<FJsonObject> Source =
			Value.IsValid() && Value->Type == EJson::Object
			? Value->AsObject()
			: nullptr;
		FString Current;
		if (!Source.IsValid()
			|| !HashFile(RecoveryStringField(Source, TEXT("filename")), Current)
			|| Current != RecoveryStringField(Source, TEXT("sha256")))
		{
			OutError = TEXT("An external reimport source changed after recovery preparation.");
			return false;
		}
	}
	return true;
}

bool UnloadRecoveryPackages(
	const TSharedPtr<FJsonObject>& Manifest,
	const bool bAllowDirtyOwnedPackages,
	FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!Manifest->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
	{
		OutError = TEXT("Recovery manifest has no asset set.");
		return false;
	}
	TArray<UPackage*> Loaded;
	for (const TSharedPtr<FJsonValue>& Value : *Assets)
	{
		const TSharedPtr<FJsonObject> Asset = Value->AsObject();
		const FString PackageName = RecoveryStringField(Asset, TEXT("package"));
		if (UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (Package->IsDirty() && !bAllowDirtyOwnedPackages)
			{
				OutError = FString::Printf(
					TEXT("Package '%s' is dirty and cannot be overwritten by rollback."),
					*PackageName);
				return false;
			}
			Loaded.AddUnique(Package);
		}
	}
	if (Loaded.IsEmpty())
	{
		return true;
	}
	FText UnloadError;
	if (!UPackageTools::UnloadPackages(
			Loaded,
			UnloadError,
			bAllowDirtyOwnedPackages))
	{
		OutError = UnloadError.IsEmpty()
			? TEXT("Recovery packages could not be unloaded safely.")
			: UnloadError.ToString();
		return false;
	}
	return true;
}
}

bool FDurablePackageRecovery::IsValidChangeSetId(const FString& ChangeSetId)
{
	FGuid Parsed;
	return FGuid::Parse(ChangeSetId, Parsed);
}

FString FDurablePackageRecovery::ManifestPath(const FString& ChangeSetId)
{
	return FPaths::Combine(
		RecoveryRoot(),
		ChangeSetId,
		TEXT("recovery.manifest.json"));
}

bool FDurablePackageRecovery::Prepare(
	const FDurablePackageRecoveryRequest& Request,
	TSharedPtr<FJsonObject>& OutManifest,
	FString& OutErrorCode,
	FString& OutError)
{
	OutManifest.Reset();
	OutErrorCode.Reset();
	OutError.Reset();
	if (!IsValidChangeSetId(Request.ChangeSetId)
		|| Request.Kind.IsEmpty()
		|| Request.PlanDigest.IsEmpty()
		|| Request.PackageNames.IsEmpty())
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("Durable recovery requires a UUID changeSetId, kind, plan digest and packages.");
		return false;
	}

	TSet<FString> UniquePackages;
	for (const FString& PackageName : Request.PackageNames)
	{
		if (!PackageName.StartsWith(TEXT("/Game/"))
			|| !FPackageName::IsValidLongPackageName(PackageName))
		{
			OutErrorCode = TEXT("unsupported_durable_rollback");
			OutError = TEXT("Durable recovery is restricted to valid /Game packages.");
			return false;
		}
		if (UPackage* Loaded = FindPackage(nullptr, *PackageName))
		{
			if (Loaded->IsDirty())
			{
				OutErrorCode = TEXT("rollback_conflict");
				OutError = FString::Printf(
					TEXT("Package '%s' already contains unsaved changes."),
					*PackageName);
				return false;
			}
		}
		UniquePackages.Add(PackageName);
	}

	TArray<FString> Packages = UniquePackages.Array();
	Packages.Sort();
	int64 SnapshotBytes = 0;
	for (const FString& Package : Packages)
	{
		for (const FString& File : PackageFileCandidates(Package))
		{
			if (IFileManager::Get().FileExists(*File))
			{
				SnapshotBytes += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
			}
		}
	}
	const int64 CurrentRecoveryBytes = DirectoryBytes(RecoveryRoot());
	if (!IFileManager::Get().MakeDirectory(*RecoveryRoot(), true))
	{
		OutErrorCode = TEXT("recovery_storage_unavailable");
		OutError = TEXT("Recovery root could not be created.");
		return false;
	}
	uint64 TotalDiskBytes = 0;
	uint64 FreeDiskBytes = 0;
	if (CurrentRecoveryBytes + SnapshotBytes > MaxRecoveryBytes
		|| !FPlatformMisc::GetDiskTotalAndFreeSpace(
			RecoveryRoot(),
			TotalDiskBytes,
			FreeDiskBytes)
		|| FreeDiskBytes < static_cast<uint64>(SnapshotBytes) + MinimumFreeHeadroom)
	{
		OutErrorCode = TEXT("recovery_storage_unavailable");
		OutError = TEXT("Recovery storage capacity or free-space precondition was not satisfied.");
		return false;
	}

	const FString Directory = FPaths::GetPath(ManifestPath(Request.ChangeSetId));
	const FString SnapshotDirectory = FPaths::Combine(Directory, TEXT("Snapshots"));
	if (!IFileManager::Get().MakeDirectory(*SnapshotDirectory, true))
	{
		OutErrorCode = TEXT("recovery_storage_unavailable");
		OutError = TEXT("Recovery snapshot directory could not be created.");
		return false;
	}

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("schema"), TEXT("ue.recovery-journal.v1"));
	Manifest->SetStringField(TEXT("codecVersion"), TEXT("1"));
	Manifest->SetStringField(TEXT("kind"), Request.Kind);
	Manifest->SetStringField(TEXT("changeSetId"), Request.ChangeSetId);
	Manifest->SetStringField(TEXT("requestId"), Request.RequestId);
	Manifest->SetStringField(TEXT("planDigest"), Request.PlanDigest);
	Manifest->SetStringField(TEXT("contractDigest"), Request.ContractDigest);
	Manifest->SetStringField(TEXT("status"), TEXT("prepared"));
	Manifest->SetStringField(TEXT("recoveryState"), TEXT("prepared"));
	Manifest->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
	Manifest->SetBoolField(TEXT("rollbackAvailable"), true);
	Manifest->SetBoolField(TEXT("rollbackVerified"), false);
	Manifest->SetStringField(TEXT("beforeHash"), Request.BeforeHash);
	Manifest->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetStringField(
		TEXT("recoveryExpiresAt"),
		(FDateTime::UtcNow() + FTimespan::FromDays(RetentionDays)).ToIso8601());

	TArray<TSharedPtr<FJsonValue>> Assets;
	for (int32 PackageIndex = 0; PackageIndex < Packages.Num(); ++PackageIndex)
	{
		const FString& Package = Packages[PackageIndex];
		TSharedRef<FJsonObject> Asset = MakeShared<FJsonObject>();
		Asset->SetStringField(TEXT("package"), Package);
		TArray<TSharedPtr<FJsonValue>> Files;
		const TArray<FString> Candidates = PackageFileCandidates(Package);
		for (int32 FileIndex = 0; FileIndex < Candidates.Num(); ++FileIndex)
		{
			const FString& Filename = Candidates[FileIndex];
			const bool bExists = IFileManager::Get().FileExists(*Filename);
			TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
			File->SetStringField(TEXT("filename"), Filename);
			File->SetBoolField(TEXT("beforeExists"), bExists);
			if (bExists)
			{
				const FString Snapshot = FPaths::Combine(
					SnapshotDirectory,
					FString::Printf(
						TEXT("%03d-%d%s"),
						PackageIndex,
						FileIndex,
						*FPaths::GetExtension(Filename, true)));
				FString SnapshotHash;
				if (!CopyAndVerify(Filename, Snapshot, SnapshotHash, OutError))
				{
					OutErrorCode = TEXT("recovery_snapshot_failed");
					return false;
				}
				File->SetStringField(TEXT("beforeSha256"), SnapshotHash);
				File->SetStringField(TEXT("snapshotFilename"), Snapshot);
				File->SetStringField(TEXT("snapshotSha256"), SnapshotHash);
			}
			Files.Add(MakeShared<FJsonValueObject>(File));
		}
		Asset->SetArrayField(TEXT("files"), Files);
		Assets.Add(MakeShared<FJsonValueObject>(Asset));
	}
	Manifest->SetArrayField(TEXT("assets"), Assets);

	TArray<TSharedPtr<FJsonValue>> ExternalSources;
	TSet<FString> UniqueSources;
	for (const FString& Source : Request.ExternalSourceFiles)
	{
		UniqueSources.Add(Source);
	}
	TArray<FString> SortedSources = UniqueSources.Array();
	SortedSources.Sort();
	for (const FString& Source : SortedSources)
	{
		FString SourceHash;
		if (!IFileManager::Get().FileExists(*Source) || !HashFile(Source, SourceHash))
		{
			OutErrorCode = TEXT("unsupported_durable_rollback");
			OutError = FString::Printf(
				TEXT("External source '%s' could not be hashed before execution."),
				*Source);
			return false;
		}
		TSharedRef<FJsonObject> SourceRecord = MakeShared<FJsonObject>();
		SourceRecord->SetStringField(TEXT("filename"), Source);
		SourceRecord->SetStringField(TEXT("sha256"), SourceHash);
		ExternalSources.Add(MakeShared<FJsonValueObject>(SourceRecord));
	}
	Manifest->SetArrayField(TEXT("externalSources"), ExternalSources);

	if (!WriteJsonAtomically(ManifestPath(Request.ChangeSetId), Manifest, OutError))
	{
		OutErrorCode = TEXT("recovery_snapshot_failed");
		return false;
	}
	OutManifest = Manifest;
	return true;
}

bool FDurablePackageRecovery::Seal(
	const FString& ChangeSetId,
	const FString& Status,
	const FString& AfterHash,
	TSharedPtr<FJsonObject>& OutManifest,
	FString& OutErrorCode,
	FString& OutError)
{
	OutManifest = ReadJson(ManifestPath(ChangeSetId));
	if (!OutManifest.IsValid()
		|| RecoveryStringField(OutManifest, TEXT("schema")) != TEXT("ue.recovery-journal.v1"))
	{
		OutErrorCode = TEXT("recovery_checkpoint_corrupt");
		OutError = TEXT("Recovery manifest is missing or invalid.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!OutManifest->TryGetArrayField(TEXT("assets"), Assets) || !Assets)
	{
		OutErrorCode = TEXT("recovery_checkpoint_corrupt");
		OutError = TEXT("Recovery manifest has no asset file set.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
	{
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (!AssetValue.IsValid() || AssetValue->Type != EJson::Object
			|| !AssetValue->AsObject()->TryGetArrayField(TEXT("files"), Files)
			|| !Files)
		{
			OutErrorCode = TEXT("recovery_checkpoint_corrupt");
			OutError = TEXT("Recovery manifest contains an invalid asset file set.");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& FileValue : *Files)
		{
			const TSharedPtr<FJsonObject> File = FileValue->AsObject();
			const FString Filename = RecoveryStringField(File, TEXT("filename"));
			const bool bExists = IFileManager::Get().FileExists(*Filename);
			File->SetBoolField(TEXT("afterExists"), bExists);
			if (bExists)
			{
				FString Hash;
				if (!HashFile(Filename, Hash))
				{
					OutErrorCode = TEXT("recovery_snapshot_failed");
					OutError = FString::Printf(
						TEXT("Post-change package file '%s' could not be hashed."),
						*Filename);
					return false;
				}
				File->SetStringField(TEXT("afterSha256"), Hash);
			}
		}
	}
	OutManifest->SetStringField(TEXT("status"), Status);
	OutManifest->SetStringField(TEXT("recoveryState"), TEXT("rollbackAvailable"));
	OutManifest->SetStringField(TEXT("afterHash"), AfterHash);
	OutManifest->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
	if (!WriteJsonAtomically(ManifestPath(ChangeSetId), OutManifest, OutError))
	{
		OutErrorCode = TEXT("recovery_snapshot_failed");
		return false;
	}
	return true;
}

bool FDurablePackageRecovery::Rollback(
	const FString& ChangeSetId,
	const FString& RequestId,
	TSharedPtr<FJsonObject>& OutResult,
	FString& OutErrorCode,
	FString& OutError,
	const bool bAllowDirtyOwnedPackages)
{
	OutResult.Reset();
	if (!IsValidChangeSetId(ChangeSetId))
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("changeSetId must be a UUID.");
		return false;
	}
	TSharedPtr<FJsonObject> Manifest = ReadJson(ManifestPath(ChangeSetId));
	if (!Manifest.IsValid())
	{
		OutErrorCode = TEXT("rollback_expired");
		OutError = TEXT("Durable recovery record was not found.");
		return false;
	}
	if (RecoveryStringField(Manifest, TEXT("status")) == TEXT("rolledBack")
		|| AllFilesMatch(Manifest, TEXT("beforeExists"), TEXT("beforeSha256")))
	{
		TSharedRef<FJsonObject> Idempotent = MakeShared<FJsonObject>();
		Idempotent->SetStringField(TEXT("schema"), TEXT("ue.recovery-rollback.v1"));
		Idempotent->SetStringField(TEXT("changeSetId"), ChangeSetId);
		Idempotent->SetStringField(TEXT("requestId"), RequestId);
		Idempotent->SetStringField(TEXT("status"), TEXT("alreadyRolledBack"));
		Idempotent->SetBoolField(TEXT("rollbackVerified"), true);
		OutResult = Idempotent;
		return true;
	}
	if (!AllFilesMatch(Manifest, TEXT("afterExists"), TEXT("afterSha256")))
	{
		OutErrorCode = TEXT("rollback_conflict");
		OutError = TEXT("Current package files differ from the sealed post-change state.");
		return false;
	}
	if (!ValidateExternalSources(Manifest, OutError))
	{
		OutErrorCode = TEXT("rollback_conflict");
		return false;
	}
	if (!UnloadRecoveryPackages(
			Manifest,
			bAllowDirtyOwnedPackages,
			OutError))
	{
		OutErrorCode = TEXT("rollback_conflict");
		return false;
	}

	TArray<FString> ModifiedFiles;
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	Manifest->TryGetArrayField(TEXT("assets"), Assets);
	for (const TSharedPtr<FJsonValue>& AssetValue : *Assets)
	{
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		AssetValue->AsObject()->TryGetArrayField(TEXT("files"), Files);
		for (const TSharedPtr<FJsonValue>& FileValue : *Files)
		{
			const TSharedPtr<FJsonObject> File = FileValue->AsObject();
			const FString Filename = RecoveryStringField(File, TEXT("filename"));
			bool bBeforeExists = false;
			File->TryGetBoolField(TEXT("beforeExists"), bBeforeExists);
			if (bBeforeExists)
			{
				if (!RestoreFileAtomically(
						RecoveryStringField(File, TEXT("snapshotFilename")),
						RecoveryStringField(File, TEXT("snapshotSha256")),
						Filename,
						OutError))
				{
					OutErrorCode = TEXT("recovery_snapshot_failed");
					return false;
				}
			}
			else if (IFileManager::Get().FileExists(*Filename)
				&& !IFileManager::Get().Delete(*Filename, false, true, true))
			{
				OutErrorCode = TEXT("recovery_snapshot_failed");
				OutError = FString::Printf(
					TEXT("Rollback could not delete newly created file '%s'."),
					*Filename);
				return false;
			}
			ModifiedFiles.Add(Filename);
		}
	}
	if (!AllFilesMatch(Manifest, TEXT("beforeExists"), TEXT("beforeSha256")))
	{
		OutErrorCode = TEXT("recovery_snapshot_failed");
		OutError = TEXT("Rollback package files failed final checksum verification.");
		return false;
	}

	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanModifiedAssetFiles(ModifiedFiles);
	Manifest->SetStringField(TEXT("status"), TEXT("rolledBack"));
	Manifest->SetStringField(TEXT("recoveryState"), TEXT("rolledBack"));
	Manifest->SetBoolField(TEXT("rollbackAvailable"), false);
	Manifest->SetBoolField(TEXT("rollbackVerified"), true);
	Manifest->SetStringField(TEXT("rollbackRequestId"), RequestId);
	Manifest->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
	if (!WriteJsonAtomically(ManifestPath(ChangeSetId), Manifest, OutError))
	{
		OutErrorCode = TEXT("recovery_snapshot_failed");
		return false;
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.recovery-rollback.v1"));
	Result->SetStringField(TEXT("changeSetId"), ChangeSetId);
	Result->SetStringField(TEXT("requestId"), RequestId);
	Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
	Result->SetBoolField(TEXT("rollbackVerified"), true);
	Result->SetStringField(TEXT("beforeHash"), RecoveryStringField(Manifest, TEXT("beforeHash")));
	Result->SetStringField(TEXT("afterHash"), RecoveryStringField(Manifest, TEXT("afterHash")));
	OutResult = Result;
	return true;
}
}
