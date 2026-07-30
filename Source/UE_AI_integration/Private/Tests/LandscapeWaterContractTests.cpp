#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/LandscapeWaterService.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "PCGComponent.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstanceWithAsset.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
using UEAIIntegration::Infrastructure::FLandscapeWaterService;

TSharedPtr<FJsonObject> MakeLandscapeSnapshot(const FString& Material)
{
	TSharedRef<FJsonObject> Landscape = MakeShared<FJsonObject>();
	Landscape->SetStringField(
		TEXT("landscapeId"),
		TEXT("00000000-0000-0000-0000-000000000001"));
	Landscape->SetStringField(TEXT("material"), Material);
	TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("world"), TEXT("/Game/Tests/WP_Landscape"));
	TArray<TSharedPtr<FJsonValue>> Landscapes;
	Landscapes.Add(MakeShared<FJsonValueObject>(Landscape));
	State->SetArrayField(TEXT("landscapes"), Landscapes);
	State->SetArrayField(
		TEXT("water"),
		TArray<TSharedPtr<FJsonValue>>());
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema"), TEXT("ue.landscape-snapshot.v1"));
	Snapshot->SetObjectField(TEXT("state"), State);
	Snapshot->SetStringField(
		TEXT("snapshotDigest"),
		UEAIIntegration::Infrastructure::DigestJson(State));
	return Snapshot;
}

TSharedRef<FJsonObject> MakeTransformJson(
	const FVector& Location,
	const FRotator& Rotation = FRotator::ZeroRotator,
	const FVector& Scale = FVector::OneVector)
{
	auto VectorJson = [](const FVector& Value)
	{
		return TArray<TSharedPtr<FJsonValue>>{
			MakeShared<FJsonValueNumber>(Value.X),
			MakeShared<FJsonValueNumber>(Value.Y),
			MakeShared<FJsonValueNumber>(Value.Z)
		};
	};
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("location"), VectorJson(Location));
	Result->SetArrayField(
		TEXT("rotation"),
		VectorJson(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
	Result->SetArrayField(TEXT("scale"), VectorJson(Scale));
	return Result;
}

TSharedRef<FJsonObject> MakeChangeParams(
	const TSharedRef<FJsonObject>& Operation)
{
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetArrayField(
		TEXT("operations"),
		{MakeShared<FJsonValueObject>(Operation)});
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("request"), Request);
	return Params;
}

/**
 * Creates, saves, and reloads an isolated World Partition map under
 * /Game/Automation. The previous persisted Editor map is restored when one is
 * available; otherwise cleanup switches to a new blank map. Every
 * map/external-actor artifact is removed during cleanup.
 */
class FScopedWorldPartitionLandscapeFixture
{
public:
	~FScopedWorldPartitionLandscapeFixture()
	{
		FString IgnoredError;
		Cleanup(IgnoredError);
	}

	bool Cleanup(FString& OutError)
	{
		OutError.Reset();
		if (bCleaned)
		{
			return true;
		}
		if (bCleanupInProgress)
		{
			OutError = TEXT("Landscape fixture cleanup is already in progress.");
			return false;
		}
		if (PackageName.IsEmpty() && !World && !Package)
		{
			bCleaned = true;
			return true;
		}
		bCleanupInProgress = true;
		TArray<FString> CleanupErrors;
		for (const FString& File : ArtifactFiles)
		{
			DeleteArtifactFile(File);
		}
		for (const FString& Directory : ArtifactDirectories)
		{
			DeleteArtifactDirectory(Directory);
		}
		LayerInfo.Reset();
		AlternateMaterial.Reset();
		DataLayerAsset.Reset();
		InstancedHLODLayer.Reset();
		MergedHLODLayer.Reset();
		PCGComponent = nullptr;
		PCGActor = nullptr;

		TSet<FString> FixturePackageNames;
		if (!PackageName.IsEmpty())
		{
			FixturePackageNames.Add(PackageName);
		}
		if (!LayerInfoPackageName.IsEmpty())
		{
			FixturePackageNames.Add(LayerInfoPackageName);
		}
		if (!DataLayerAssetPackageName.IsEmpty())
		{
			FixturePackageNames.Add(DataLayerAssetPackageName);
		}
		for (const FString& HLODPackageName : HLODLayerPackageNames)
		{
			FixturePackageNames.Add(HLODPackageName);
		}
		if (Package)
		{
			for (UPackage* ExternalPackage : Package->GetExternalPackages())
			{
				if (ExternalPackage)
				{
					FixturePackageNames.Add(ExternalPackage->GetName());
				}
			}
		}
		CollectPackageFiles(
			ExternalActorDirectory,
			FixturePackageNames,
			FixturePackageFiles);
		CollectPackageFiles(
			ExternalObjectDirectory,
			FixturePackageNames,
			FixturePackageFiles);
		for (const FString& FixturePackageName : FixturePackageNames)
		{
			if (UPackage* FixturePackage =
					FindPackage(nullptr, *FixturePackageName))
			{
				FixturePackage->SetDirtyFlag(false);
			}
		}

		bool bOriginalMapRestored = OriginalMapFilename.IsEmpty();
		if (EditorContext)
		{
			UWorld* CurrentWorld = EditorContext->World();
			if (CurrentWorld && !OriginalMapPackageName.IsEmpty()
				&& CurrentWorld->GetOutermost()->GetName()
					== OriginalMapPackageName)
			{
				bOriginalMapRestored = true;
			}
			if (OriginalMapFilename.IsEmpty()
				&& CurrentWorld
				&& CurrentWorld->GetOutermost()->GetName()
					== PackageName
				&& !UEditorLoadingAndSavingUtils::NewBlankMap(false))
			{
				CleanupErrors.Add(
					TEXT("The fixture map could not be released through a safe blank-map transition."));
			}
			if (!bOriginalMapRestored
				&& !OriginalMapFilename.IsEmpty()
				&& IFileManager::Get().FileExists(
					*OriginalMapFilename))
			{
				if (GEditor)
				{
					GEditor->SelectNone(false, true, false);
				}
				UWorld* RestoredWorld =
					UEditorLoadingAndSavingUtils::LoadMap(
						OriginalMapFilename);
				bOriginalMapRestored =
					RestoredWorld
					&& RestoredWorld->GetOutermost()->GetName()
						== OriginalMapPackageName;
			}
			if (!bOriginalMapRestored)
			{
				if (!OriginalMapFilename.IsEmpty())
				{
					CleanupErrors.Add(
						FString::Printf(
							TEXT("The original Editor map '%s' could not be restored after the fixture."),
							*OriginalMapFilename));
				}
				if (!UEditorLoadingAndSavingUtils::NewBlankMap(false))
				{
					CleanupErrors.Add(
						TEXT("The fixture map could not be unloaded through a safe map transition."));
				}
			}
		}
		else
		{
			CleanupErrors.Add(
				TEXT("The Editor world context became unavailable during fixture cleanup."));
		}

		World = nullptr;
		Landscape = nullptr;
		Package = nullptr;

		for (const FString& FixturePackageName : FixturePackageNames)
		{
			if (UPackage* FixturePackage =
					FindPackage(nullptr, *FixturePackageName))
			{
				NotifyAssetsDeleted(FixturePackage);
				FixturePackage->SetDirtyFlag(false);
				TArray<UPackage*> PackagesToUnload{FixturePackage};
				UPackageTools::FUnloadPackageParams UnloadParams(
					PackagesToUnload);
				UnloadParams.bUnloadDirtyPackages = true;
				UnloadParams.bResetTransBuffer = false;
				if (!UPackageTools::UnloadPackages(UnloadParams)
					&& !UnloadParams.OutErrorMessage.IsEmpty())
				{
					CleanupErrors.Add(
						FString::Printf(
							TEXT("Package '%s' could not be unloaded: %s"),
							*FixturePackageName,
							*UnloadParams.OutErrorMessage.ToString()));
				}
			}
		}

		TArray<FString> FilesToDelete = FixturePackageFiles;
		if (!MapFilename.IsEmpty())
		{
			FilesToDelete.AddUnique(MapFilename);
		}
		if (!LayerInfoFilename.IsEmpty())
		{
			FilesToDelete.AddUnique(LayerInfoFilename);
		}
		if (!DataLayerAssetFilename.IsEmpty())
		{
			FilesToDelete.AddUnique(DataLayerAssetFilename);
		}
		for (const FString& HLODLayerFilename : HLODLayerFilenames)
		{
			FilesToDelete.AddUnique(HLODLayerFilename);
		}
		for (const FString& FileToDelete : FilesToDelete)
		{
			if (IFileManager::Get().FileExists(*FileToDelete)
				&& !IFileManager::Get().Delete(
					*FileToDelete,
					false,
					true,
					true))
			{
				CleanupErrors.Add(
					FString::Printf(
						TEXT("Fixture file '%s' could not be deleted."),
						*FileToDelete));
			}
		}
		for (const FString& Directory :
			{ExternalActorDirectory, ExternalObjectDirectory})
		{
			if (!Directory.IsEmpty()
				&& IFileManager::Get().DirectoryExists(*Directory)
				&& !IFileManager::Get().DeleteDirectory(
					*Directory,
					false,
					true))
			{
				CleanupErrors.Add(
					FString::Printf(
						TEXT("Fixture package directory '%s' could not be deleted."),
						*Directory));
			}
		}
		if (!FilesToDelete.IsEmpty())
		{
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles(
				FilesToDelete);
		}
		TArray<FAssetData> RemainingAssets;
		for (const FString& FixturePackageName : FixturePackageNames)
		{
			TArray<FAssetData> PackageAssets;
			IAssetRegistry::GetChecked().GetAssetsByPackageName(
				FName(*FixturePackageName),
				PackageAssets);
			RemainingAssets.Append(PackageAssets);
		}
		TArray<FString> RemainingFiles;
		for (const FString& FileToDelete : FilesToDelete)
		{
			if (IFileManager::Get().FileExists(*FileToDelete))
			{
				RemainingFiles.Add(FileToDelete);
			}
		}
		const bool bExternalClean =
			(ExternalActorDirectory.IsEmpty()
				|| !IFileManager::Get().DirectoryExists(
					*ExternalActorDirectory))
			&& (ExternalObjectDirectory.IsEmpty()
				|| !IFileManager::Get().DirectoryExists(
					*ExternalObjectDirectory));
		if (!RemainingFiles.IsEmpty()
			|| !bExternalClean
			|| !RemainingAssets.IsEmpty())
		{
			CleanupErrors.Add(
				FString::Printf(
					TEXT("The saved World Partition fixture left %d files, external-package directories=%s, or %d Asset Registry entries."),
					RemainingFiles.Num(),
					bExternalClean ? TEXT("clean") : TEXT("present"),
					RemainingAssets.Num()));
		}

		RestoreLastLevel(CleanupErrors);
		bCleaned = CleanupErrors.IsEmpty();
		bCleanupInProgress = false;
		if (!bCleaned)
		{
			OutError = FString::Join(CleanupErrors, TEXT(" "));
			return false;
		}
		return true;
	}

	bool Initialize(FString& OutError)
	{
		OutError.Reset();
		if (!GEditor)
		{
			OutError = TEXT("GEditor is unavailable.");
			return false;
		}
		if (GEditor->IsPlaySessionInProgress())
		{
			OutError =
				TEXT("The Landscape fixture cannot replace the Editor world while PIE is running or queued.");
			return false;
		}

		EditorContext = &GEditor->GetEditorWorldContext();
		OriginalWorld = EditorContext->World();
		if (!OriginalWorld)
		{
			OutError =
				TEXT("The original Editor world is unavailable.");
			return false;
		}
		TArray<FString> DirtyOriginalPackages;
		CollectDirtyWorldPackages(
			OriginalWorld,
			DirtyOriginalPackages);
		if (!DirtyOriginalPackages.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("The Landscape fixture refuses to replace an Editor world with dirty map or external packages: %s"),
				*FString::Join(
					DirtyOriginalPackages,
					TEXT(", ")));
			return false;
		}

		OriginalMapPackageName =
			OriginalWorld->GetOutermost()->GetName();
		FPackageName::DoesPackageExist(
			OriginalMapPackageName,
			&OriginalMapFilename);
		if (!OriginalMapFilename.IsEmpty())
		{
			OriginalMapFilename =
				FPaths::ConvertRelativePathToFull(
					OriginalMapFilename);
			if (!IFileManager::Get().FileExists(
					*OriginalMapFilename))
			{
				OriginalMapFilename.Reset();
			}
		}
		if (GConfig)
		{
			bCapturedLastLevelConfig = true;
			bHadOriginalLastLevel =
				GConfig->GetString(
					TEXT("EditorStartup"),
					TEXT("LastLevel"),
					OriginalLastLevel,
					GEditorPerProjectIni);
		}
		const FString Suffix =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		PackageName =
			TEXT("/Game/Automation/UEAI_LandscapeWater_") + Suffix;
		LayerInfoPackageName = PackageName + TEXT("_WeightLayerInfo");
		LayerInfoFilename =
			FPackageName::LongPackageNameToFilename(
				LayerInfoPackageName,
				FPackageName::GetAssetPackageExtension());
		DataLayerAssetPackageName =
			PackageName + TEXT("_RuntimeDataLayer");
		DataLayerAssetFilename =
			FPackageName::LongPackageNameToFilename(
				DataLayerAssetPackageName,
				FPackageName::GetAssetPackageExtension());
		HLODLayerPackageNames = {
			PackageName + TEXT("_HLODLayer_Instanced"),
			PackageName + TEXT("_HLODLayer_Merged")
		};
		for (const FString& HLODLayerPackageName :
			HLODLayerPackageNames)
		{
			HLODLayerFilenames.Add(
				FPackageName::LongPackageNameToFilename(
					HLODLayerPackageName,
					FPackageName::GetAssetPackageExtension()));
		}
		GEditor->CreateNewMapForEditing(
			/*bPromptUserToSave=*/false,
			/*bIsPartitionedWorld=*/true);
		World = EditorContext->World();
		if (!World)
		{
			OutError =
				TEXT("The Editor could not create the fixture world.");
			return false;
		}
		Package = World->GetOutermost();
		if (!World->GetWorldPartition())
		{
			OutError =
				TEXT("The fixture world is not backed by World Partition.");
			return false;
		}

		Landscape = CreateMinimalLandscape(OutError);
		if (!Landscape)
		{
			return false;
		}
		if (!CreateImpactEvidence(OutError))
		{
			return false;
		}
		UPackage* LayerInfoPackage =
			LayerInfo.IsValid()
			? LayerInfo->GetOutermost()
			: nullptr;
		if (!LayerInfoPackage
			|| LayerInfoPackage->GetName() != LayerInfoPackageName)
		{
			OutError =
				TEXT("The Landscape Layer Info asset has an invalid package.");
			return false;
		}
		LayerInfoPackage->SetDirtyFlag(true);
		FSavePackageArgs LayerSaveArgs;
		LayerSaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(
				LayerInfoPackage,
				LayerInfo.Get(),
				*LayerInfoFilename,
				LayerSaveArgs))
		{
			OutError =
				TEXT("The Landscape Layer Info asset could not be saved.");
			return false;
		}
		LayerInfoPackage->ClearPackageFlags(PKG_NewlyCreated);
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (UPackage* ExternalPackage = It->GetExternalPackage())
			{
				if (ExternalPackage != Package)
				{
					ExternalPackage->SetDirtyFlag(true);
				}
			}
		}
		if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
		{
			OutError =
				TEXT("The World Partition fixture map could not be saved.");
			return false;
		}
		Package = World->GetOutermost();
		if (!Package || Package->GetName() != PackageName)
		{
			OutError =
				TEXT("The saved World Partition fixture did not adopt the requested package identity.");
			return false;
		}
		// UEditorLoadingAndSavingUtils::SaveMap can retain PKG_NewlyCreated for
		// programmatically-created World packages in commandlet-style Editor
		// sessions even after the map and its external actors reached disk.
		// ReloadPackages consequently classifies the package as in-memory-only.
		// Clear only that transient creation flag after proving the save
		// succeeded; serialized package flags and disk content remain unchanged.
		Package->ClearPackageFlags(PKG_NewlyCreated);
		for (UPackage* ExternalPackage : Package->GetExternalPackages())
		{
			if (ExternalPackage)
			{
				ExternalPackage->ClearPackageFlags(PKG_NewlyCreated);
			}
		}
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				MapFilename,
				FPackageName::GetMapPackageExtension()))
		{
			OutError =
				TEXT("The fixture map filename could not be resolved.");
			return false;
		}
		const FString MapAssetName =
			FPackageName::GetLongPackageAssetName(PackageName);
		const FString MapPath =
			FPackageName::GetLongPackagePath(PackageName);
		const FString RelativeMapPath =
			MapPath.StartsWith(TEXT("/Game/"))
				? MapPath.RightChop(6)
				: FString();
		ExternalActorDirectory =
			FPackageName::LongPackageNameToFilename(
				TEXT("/Game/__ExternalActors__/")
				+ RelativeMapPath
				+ TEXT("/")
				+ MapAssetName);
		ExternalObjectDirectory =
			FPackageName::LongPackageNameToFilename(
				TEXT("/Game/__ExternalObjects__/")
				+ RelativeMapPath
				+ TEXT("/")
				+ MapAssetName);
		return ReloadFromDisk(OutError);
	}

	UWorld* GetWorld() const
	{
		return World;
	}

	ALandscape* GetLandscape() const
	{
		return Landscape;
	}

	const FString& GetDataLayerAssetPath() const
	{
		return DataLayerAssetPath;
	}

	const FString& GetHLODLayerPath() const
	{
		return InstancedHLODLayerPath;
	}

	FString GetPCGComponentPath() const
	{
		return PCGComponent
			? PCGComponent->GetPathName()
			: FString();
	}

	bool WasReloadedFromDisk() const
	{
		return bReloadedFromDisk;
	}

	UMaterialInterface* GetOrCreateAlternateMaterial()
	{
		if (!AlternateMaterial.IsValid() && Package)
		{
			AlternateMaterial.Reset(
				NewObject<UMaterial>(
					Package,
					TEXT("UEAI_AlternateLandscapeMaterial"),
					RF_Public | RF_Transient | RF_Transactional));
		}
		return AlternateMaterial.Get();
	}

	void AddArtifactDirectory(const FString& Directory)
	{
		if (!Directory.IsEmpty())
		{
			ArtifactDirectories.AddUnique(Directory);
		}
	}

	void AddArtifactFile(const FString& File)
	{
		if (!File.IsEmpty())
		{
			ArtifactFiles.AddUnique(File);
		}
	}

private:
	static void CollectDirtyWorldPackages(
		UWorld* CandidateWorld,
		TArray<FString>& OutDirtyPackageNames)
	{
		OutDirtyPackageNames.Reset();
		if (!CandidateWorld)
		{
			return;
		}
		TSet<UPackage*> Packages;
		UPackage* WorldPackage = CandidateWorld->GetOutermost();
		if (WorldPackage)
		{
			Packages.Add(WorldPackage);
			for (UPackage* ExternalPackage :
				WorldPackage->GetExternalPackages())
			{
				if (ExternalPackage)
				{
					Packages.Add(ExternalPackage);
				}
			}
		}
		for (TActorIterator<AActor> It(CandidateWorld); It; ++It)
		{
			if (UPackage* ExternalPackage =
					It->GetExternalPackage())
			{
				Packages.Add(ExternalPackage);
			}
		}
		for (UPackage* CandidatePackage : Packages)
		{
			if (CandidatePackage && CandidatePackage->IsDirty())
			{
				OutDirtyPackageNames.Add(
					CandidatePackage->GetName());
			}
		}
		OutDirtyPackageNames.Sort();
	}

	static void CollectPackageFiles(
		const FString& Directory,
		TSet<FString>& OutPackageNames,
		TArray<FString>& OutFiles)
	{
		if (Directory.IsEmpty()
			|| !IFileManager::Get().DirectoryExists(*Directory))
		{
			return;
		}
		TArray<FString> CandidateFiles;
		IFileManager::Get().FindFilesRecursive(
			CandidateFiles,
			*Directory,
			TEXT("*"),
			true,
			false,
			false);
		for (const FString& CandidateFile : CandidateFiles)
		{
			const FString Extension =
				FPaths::GetExtension(CandidateFile, true);
			if (!Extension.Equals(
					FPackageName::GetAssetPackageExtension(),
					ESearchCase::IgnoreCase)
				&& !Extension.Equals(
					FPackageName::GetMapPackageExtension(),
					ESearchCase::IgnoreCase))
			{
				continue;
			}
			OutFiles.AddUnique(CandidateFile);
			FString CandidatePackageName;
			if (FPackageName::TryConvertFilenameToLongPackageName(
					CandidateFile,
					CandidatePackageName))
			{
				OutPackageNames.Add(CandidatePackageName);
			}
		}
	}

	static void NotifyAssetsDeleted(UPackage* CandidatePackage)
	{
		if (!CandidatePackage)
		{
			return;
		}
		TArray<UObject*> PackageObjects;
		GetObjectsWithPackage(
			CandidatePackage,
			PackageObjects,
			false);
		for (UObject* Object : PackageObjects)
		{
			if (Object && Object->IsAsset())
			{
				FAssetRegistryModule::AssetDeleted(Object);
			}
		}
	}

	void RestoreLastLevel(TArray<FString>& OutErrors) const
	{
		if (!bCapturedLastLevelConfig)
		{
			return;
		}
		if (!GConfig)
		{
			OutErrors.Add(
				TEXT("The Editor config cache became unavailable before LastLevel could be restored."));
			return;
		}
		if (bHadOriginalLastLevel)
		{
			GConfig->SetString(
				TEXT("EditorStartup"),
				TEXT("LastLevel"),
				*OriginalLastLevel,
				GEditorPerProjectIni);
		}
		else
		{
			GConfig->RemoveKey(
				TEXT("EditorStartup"),
				TEXT("LastLevel"),
				GEditorPerProjectIni);
		}
		GConfig->Flush(false, GEditorPerProjectIni);

		FString RestoredLastLevel;
		const bool bHasRestoredLastLevel =
			GConfig->GetString(
				TEXT("EditorStartup"),
				TEXT("LastLevel"),
				RestoredLastLevel,
				GEditorPerProjectIni);
		if (bHasRestoredLastLevel != bHadOriginalLastLevel
			|| (bHadOriginalLastLevel
				&& RestoredLastLevel != OriginalLastLevel))
		{
			OutErrors.Add(
				TEXT("EditorStartup.LastLevel did not return to its pre-fixture value."));
		}
	}

	bool ReloadFromDisk(FString& OutError)
	{
		OutError.Reset();
		if (!EditorContext
			|| !Package
			|| !LayerInfo.IsValid()
			|| MapFilename.IsEmpty()
			|| LayerInfoFilename.IsEmpty())
		{
			OutError =
				TEXT("The saved World Partition fixture is incomplete.");
			return false;
		}
		UPackage* PreviousLayerPackage =
			LayerInfo->GetOutermost();
		TWeakObjectPtr<ULandscapeLayerInfoObject> PreviousLayerInfo =
			LayerInfo.Get();
		if (!PreviousLayerPackage
			|| PreviousLayerPackage->GetName()
				!= LayerInfoPackageName
			|| PreviousLayerPackage->IsDirty())
		{
			OutError =
				TEXT("The saved Layer Info package is missing, misidentified, or still dirty before disk reload.");
			return false;
		}
		LayerInfo.Reset();
		Landscape = nullptr;
		World = nullptr;
		Package = nullptr;
		if (!UEditorLoadingAndSavingUtils::NewBlankMap(false))
		{
			OutError =
				TEXT("The fixture map could not be released through a blank-map transition.");
			return false;
		}

		UPackage* LayerPackageToUnload =
			FindPackage(nullptr, *LayerInfoPackageName);
		if (!LayerPackageToUnload)
		{
			LayerPackageToUnload =
				LoadPackage(
					nullptr,
					*LayerInfoPackageName,
					LOAD_None);
		}
		if (!LayerPackageToUnload)
		{
			OutError =
				TEXT("The separate Layer Info package could not be resolved for explicit unload.");
			return false;
		}
		LayerPackageToUnload->SetDirtyFlag(false);
		TArray<UPackage*> PackagesToUnload{LayerPackageToUnload};
		UPackageTools::FUnloadPackageParams UnloadParams(
			PackagesToUnload);
		UnloadParams.bUnloadDirtyPackages = false;
		UnloadParams.bResetTransBuffer = false;
		if (!UPackageTools::UnloadPackages(UnloadParams))
		{
			OutError = FString::Printf(
				TEXT("The separate Layer Info package could not be explicitly unloaded: %s"),
				*UnloadParams.OutErrorMessage.ToString());
			return false;
		}
		if (PreviousLayerInfo.IsValid()
			|| FindObject<ULandscapeLayerInfoObject>(
				nullptr,
				*(LayerInfoPackageName
					+ TEXT(".UEAI_WeightLayerInfo"))))
		{
			OutError =
				TEXT("The previous in-memory Layer Info object survived the explicit package unload.");
			return false;
		}

		World = UEditorLoadingAndSavingUtils::LoadMap(MapFilename);
		if (!World)
		{
			OutError =
				TEXT("The World Partition fixture map could not be loaded from disk.");
			return false;
		}
		Package = World->GetOutermost();
		if (!World || !World->GetWorldPartition())
		{
			OutError =
				TEXT("The reloaded fixture is not a World Partition map.");
			return false;
		}
		if (EditorContext->World() != World)
		{
			OutError =
				TEXT("Loading the fixture map did not update the Editor world context.");
			return false;
		}
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			Landscape = *It;
			break;
		}
		if (!Landscape)
		{
			OutError =
				TEXT("The reloaded World Partition map lost its Landscape actor.");
			return false;
		}
		if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
		{
			for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
			{
				if (Layer.LayerInfoObj
					&& Layer.LayerInfoObj->LayerName
						== FName(TEXT("UEAI_Weight")))
				{
					LayerInfo.Reset(Layer.LayerInfoObj);
					break;
				}
			}
		}
		if (!LayerInfo.IsValid())
		{
			OutError =
				TEXT("The reloaded World Partition map lost its Layer Info object.");
			return false;
		}
		DataLayerAsset.Reset(
			LoadObject<UDataLayerAsset>(
				nullptr,
				*DataLayerAssetPath));
		InstancedHLODLayer.Reset(
			LoadObject<UHLODLayer>(
				nullptr,
				*InstancedHLODLayerPath));
		MergedHLODLayer.Reset(
			LoadObject<UHLODLayer>(
				nullptr,
				*MergedHLODLayerPath));
		PCGComponent =
			nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->GetFName()
				== FName(TEXT("UEAI_PCGImpactFixture")))
			{
				PCGActor = *It;
				PCGComponent =
					PCGActor
						->FindComponentByClass<UPCGComponent>();
				break;
			}
		}
		const bool bHasExpectedDataLayer =
			Landscape->GetDataLayerAssets().ContainsByPredicate(
				[this](const UDataLayerAsset* Asset)
				{
					return Asset
						&& Asset->GetPathName()
							== DataLayerAssetPath;
				});
		if (!DataLayerAsset.IsValid()
			|| !bHasExpectedDataLayer)
		{
			OutError =
				TEXT("The reloaded Landscape lost its runtime Data Layer assignment.");
			return false;
		}
		if (!InstancedHLODLayer.IsValid()
			|| !MergedHLODLayer.IsValid()
			|| !Landscape->GetHLODLayer()
			|| Landscape->GetHLODLayer()->GetPathName()
				!= InstancedHLODLayerPath)
		{
			OutError =
				TEXT("The reloaded Landscape lost its HLOD Layer assignment.");
			return false;
		}
		if (!PCGActor || !PCGComponent)
		{
			OutError =
				TEXT("The reloaded Landscape lost its serialized PCG Component.");
			return false;
		}
		if (LayerInfo->GetOutermost()->GetName()
				!= LayerInfoPackageName
			|| !LayerInfo->HasAnyFlags(RF_WasLoaded))
		{
			OutError =
				TEXT("The reloaded Layer Info was not loaded from its separate on-disk package.");
			return false;
		}
		bReloadedFromDisk = true;
		return true;
	}

	bool CreateImpactEvidence(FString& OutError)
	{
		OutError.Reset();
		AWorldDataLayers* WorldDataLayers =
			World ? World->GetWorldDataLayers() : nullptr;
		if (!Landscape || !WorldDataLayers)
		{
			OutError =
				TEXT("The World Partition fixture has no WorldDataLayers actor.");
			return false;
		}

		UPackage* DataLayerPackage =
			CreatePackage(*DataLayerAssetPackageName);
		const FString DataLayerAssetName =
			FPackageName::GetLongPackageAssetName(
				DataLayerAssetPackageName);
		DataLayerAsset.Reset(
			NewObject<UDataLayerAsset>(
				DataLayerPackage,
				*DataLayerAssetName,
				RF_Public | RF_Standalone | RF_Transactional));
		if (!DataLayerAsset.IsValid())
		{
			OutError =
				TEXT("The runtime Data Layer asset could not be created.");
			return false;
		}
		DataLayerAsset->SetType(EDataLayerType::Runtime);
		DataLayerAsset->SetDebugColor(FColor(40, 160, 240));
		DataLayerAssetPath = DataLayerAsset->GetPathName();
		FAssetRegistryModule::AssetCreated(DataLayerAsset.Get());
		UDataLayerInstanceWithAsset* DataLayerInstance =
			WorldDataLayers
				->CreateDataLayer<UDataLayerInstanceWithAsset>(
					DataLayerAsset.Get());
		if (!DataLayerInstance
			|| !Landscape->AddDataLayer(DataLayerInstance))
		{
			OutError =
				TEXT("The Landscape could not be assigned to the runtime Data Layer.");
			return false;
		}

		auto CreateHLODLayer =
			[&](const FString& HLODPackageName,
				const EHLODLayerType LayerType,
				TStrongObjectPtr<UHLODLayer>& OutLayer,
				FString& OutPath) -> bool
			{
				UPackage* HLODPackage =
					CreatePackage(*HLODPackageName);
				const FString HLODAssetName =
					FPackageName::GetLongPackageAssetName(
						HLODPackageName);
				OutLayer.Reset(
					NewObject<UHLODLayer>(
						HLODPackage,
						*HLODAssetName,
						RF_Public
							| RF_Standalone
							| RF_Transactional));
				if (!OutLayer.IsValid())
				{
					return false;
				}
				OutLayer->SetLayerType(LayerType);
				OutLayer->SetIsSpatiallyLoaded(true);
				OutPath = OutLayer->GetPathName();
				FAssetRegistryModule::AssetCreated(OutLayer.Get());
				return true;
			};
		if (!CreateHLODLayer(
				HLODLayerPackageNames[0],
				EHLODLayerType::Instancing,
				InstancedHLODLayer,
				InstancedHLODLayerPath)
			|| !CreateHLODLayer(
				HLODLayerPackageNames[1],
				EHLODLayerType::MeshMerge,
				MergedHLODLayer,
				MergedHLODLayerPath))
		{
			OutError =
				TEXT("The fixture HLOD Layer assets could not be created.");
			return false;
		}
		InstancedHLODLayer->SetParentLayer(
			MergedHLODLayer.Get());
		Landscape->SetHLODLayer(InstancedHLODLayer.Get());

		FActorSpawnParameters PCGActorSpawn;
		PCGActorSpawn.Name =
			TEXT("UEAI_PCGImpactFixture");
		PCGActorSpawn.ObjectFlags =
			RF_Transactional;
		PCGActor =
			World->SpawnActor<AActor>(
				FVector(350.0, 350.0, 0.0),
				FRotator::ZeroRotator,
				PCGActorSpawn);
		if (!PCGActor)
		{
			OutError =
				TEXT("The PCG impact Actor could not be created.");
			return false;
		}
		// Keep the evidence actor loaded after the World Partition map reload;
		// its bounded component is still spatial evidence for the overlap query.
		PCGActor->SetIsSpatiallyLoaded(false);
		UBoxComponent* BoundsComponent =
			NewObject<UBoxComponent>(
				PCGActor,
				TEXT("UEAI_PCGImpactBounds"),
				RF_Transactional);
		if (!BoundsComponent)
		{
			OutError =
				TEXT("The PCG impact bounds could not be created.");
			return false;
		}
		BoundsComponent->SetBoxExtent(
			FVector(100.0, 100.0, 100.0));
		PCGActor->SetRootComponent(BoundsComponent);
		PCGActor->AddInstanceComponent(BoundsComponent);
		BoundsComponent->RegisterComponent();
		PCGComponent =
			NewObject<UPCGComponent>(
				PCGActor,
				TEXT("UEAI_PCGFixture"),
				RF_Transactional);
		if (!PCGComponent)
		{
			OutError =
				TEXT("The PCG Component could not be created.");
			return false;
		}
		PCGActor->AddInstanceComponent(PCGComponent);
		PCGComponent->RegisterComponent();

		if (!SaveStandaloneAsset(
				DataLayerAsset.Get(),
				DataLayerAssetFilename,
				OutError)
			|| !SaveStandaloneAsset(
				MergedHLODLayer.Get(),
				HLODLayerFilenames[1],
				OutError)
			|| !SaveStandaloneAsset(
				InstancedHLODLayer.Get(),
				HLODLayerFilenames[0],
				OutError))
		{
			return false;
		}
		Landscape->MarkPackageDirty();
		return true;
	}

	static bool SaveStandaloneAsset(
		UObject* Asset,
		const FString& Filename,
		FString& OutError)
	{
		UPackage* AssetPackage =
			Asset ? Asset->GetOutermost() : nullptr;
		if (!AssetPackage)
		{
			OutError =
				TEXT("A fixture asset has no package.");
			return false;
		}
		AssetPackage->SetDirtyFlag(true);
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags =
			RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(
				AssetPackage,
				Asset,
				*Filename,
				SaveArgs))
		{
			OutError = FString::Printf(
				TEXT("Fixture asset '%s' could not be saved."),
				*Asset->GetPathName());
			return false;
		}
		AssetPackage->ClearPackageFlags(PKG_NewlyCreated);
		return true;
	}

	ALandscape* CreateMinimalLandscape(FString& OutError)
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Name = TEXT("UEAI_LandscapeFixture");
		SpawnParameters.ObjectFlags = RF_Transactional;
		ALandscape* NewLandscape =
			World->SpawnActor<ALandscape>(
				FVector::ZeroVector,
				FRotator::ZeroRotator,
				SpawnParameters);
		if (!NewLandscape)
		{
			OutError =
				TEXT("The minimal Landscape actor could not be spawned.");
			return nullptr;
		}
		NewLandscape->LandscapeMaterial =
			UMaterial::GetDefaultMaterial(MD_Surface);

		constexpr int32 Size = 8;
		TArray<uint16> Heights;
		Heights.Init(32768, Size * Size);
		TMap<FGuid, TArray<uint16>> HeightDataPerLayer;
		HeightDataPerLayer.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>>
			MaterialDataPerLayer;
		LayerInfo.Reset(
			NewObject<ULandscapeLayerInfoObject>(
				CreatePackage(*LayerInfoPackageName),
				TEXT("UEAI_WeightLayerInfo"),
				RF_Public | RF_Standalone | RF_Transactional));
		if (!LayerInfo.IsValid())
		{
			OutError =
				TEXT("The fixture Landscape Layer Info could not be created.");
			return nullptr;
		}
		LayerInfo->LayerName = TEXT("UEAI_Weight");
		LayerInfo->bNoWeightBlend = false;
		FAssetRegistryModule::AssetCreated(LayerInfo.Get());
		FLandscapeImportLayerInfo WeightLayer(TEXT("UEAI_Weight"));
		WeightLayer.LayerInfo = LayerInfo.Get();
		WeightLayer.LayerData.Init(255, Size * Size);
		TArray<FLandscapeImportLayerInfo> ImportLayers;
		ImportLayers.Add(MoveTemp(WeightLayer));
		MaterialDataPerLayer.Add(FGuid(), MoveTemp(ImportLayers));
		NewLandscape->Import(
			FGuid::NewGuid(),
			0,
			0,
			Size - 1,
			Size - 1,
			1,
			Size - 1,
			HeightDataPerLayer,
			TEXT(""),
			MaterialDataPerLayer,
			ELandscapeImportAlphamapType::Additive);
		// ALandscape::PostEditChangeProperty asserts until Import has assigned
		// a valid LandscapeGuid. SetActorLabel triggers that path, so label the
		// fixture only after its initial landscape data has been imported.
		NewLandscape->SetActorLabel(TEXT("UEAI Landscape Fixture"));

		ULandscapeInfo* Info = NewLandscape->GetLandscapeInfo();
		if (!Info)
		{
			Info = NewLandscape->CreateLandscapeInfo(false, true);
		}
		if (!Info)
		{
			OutError =
				TEXT("The minimal Landscape did not produce LandscapeInfo.");
			return nullptr;
		}
		Info->UpdateLayerInfoMap(NewLandscape);
		NewLandscape->RegisterAllComponents();
		return NewLandscape;
	}

	static void DeleteArtifactDirectory(const FString& Directory)
	{
		FString FullDirectory =
			FPaths::ConvertRelativePathToFull(Directory);
		FString Root = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UE_AI_integration"),
				TEXT("Landscape")));
		FPaths::NormalizeDirectoryName(FullDirectory);
		FPaths::NormalizeDirectoryName(Root);
		const FString RootPrefix = Root + TEXT("/");
		if (FullDirectory.StartsWith(
				RootPrefix,
				ESearchCase::IgnoreCase))
		{
			IFileManager::Get().DeleteDirectory(
				*FullDirectory,
				false,
				true);
		}
	}

	static void DeleteArtifactFile(const FString& File)
	{
		FString FullFile = FPaths::ConvertRelativePathToFull(File);
		FString Root = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UE_AI_integration"),
				TEXT("Landscape")));
		FPaths::NormalizeFilename(FullFile);
		FPaths::NormalizeDirectoryName(Root);
		const FString RootPrefix = Root + TEXT("/");
		if (FullFile.StartsWith(
				RootPrefix,
				ESearchCase::IgnoreCase))
		{
			IFileManager::Get().Delete(*FullFile);
		}
	}

	FWorldContext* EditorContext = nullptr;
	UWorld* OriginalWorld = nullptr;
	FString OriginalMapPackageName;
	FString OriginalMapFilename;
	FString OriginalLastLevel;
	UWorld* World = nullptr;
	UPackage* Package = nullptr;
	ALandscape* Landscape = nullptr;
	FString PackageName;
	FString MapFilename;
	FString LayerInfoPackageName;
	FString LayerInfoFilename;
	FString DataLayerAssetPackageName;
	FString DataLayerAssetFilename;
	FString DataLayerAssetPath;
	FString ExternalActorDirectory;
	FString ExternalObjectDirectory;
	TArray<FString> HLODLayerPackageNames;
	TArray<FString> HLODLayerFilenames;
	FString InstancedHLODLayerPath;
	FString MergedHLODLayerPath;
	TArray<FString> FixturePackageFiles;
	bool bReloadedFromDisk = false;
	bool bCleaned = false;
	bool bCleanupInProgress = false;
	bool bCapturedLastLevelConfig = false;
	bool bHadOriginalLastLevel = false;
	TStrongObjectPtr<ULandscapeLayerInfoObject> LayerInfo;
	TStrongObjectPtr<UMaterial> AlternateMaterial;
	TStrongObjectPtr<UDataLayerAsset> DataLayerAsset;
	TStrongObjectPtr<UHLODLayer> InstancedHLODLayer;
	TStrongObjectPtr<UHLODLayer> MergedHLODLayer;
	UPCGComponent* PCGComponent = nullptr;
	AActor* PCGActor = nullptr;
	TArray<FString> ArtifactDirectories;
	TArray<FString> ArtifactFiles;
};

bool ApplyLandscapeChange(
	FAutomationTestBase& Test,
	FScopedWorldPartitionLandscapeFixture& Fixture,
	const TSharedRef<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutPlan,
	TSharedPtr<FJsonObject>& OutResult,
	FString* OutRequestId = nullptr)
{
	FLandscapeWaterService& Service = FLandscapeWaterService::Get();
	const FMCPToolResult Plan = Service.PlanChange(Params);
	Test.TestTrue(TEXT("Landscape/Water change planning succeeds"), Plan.bSuccess);
	if (!Plan.bSuccess || !Plan.Data.IsValid())
	{
		Test.AddError(
			FString::Printf(
				TEXT("Landscape/Water change planning failed: %s (%s)"),
				*Plan.ErrorMessage,
				*Plan.ErrorCode));
		return false;
	}
	OutPlan = Plan.Data;

	TSharedRef<FJsonObject> ExecuteParams =
		MakeShared<FJsonObject>(*Params);
	const FString RequestId =
		TEXT("landscape-water-test-")
		+ FGuid::NewGuid().ToString(
			EGuidFormats::DigitsWithHyphensLower);
	ExecuteParams->SetStringField(
		TEXT("requestId"),
		RequestId);
	ExecuteParams->SetStringField(
		TEXT("approvePlanDigest"),
		Plan.Data->GetStringField(TEXT("planDigest")));
	ExecuteParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult Execute = Service.ExecuteChange(ExecuteParams);
	Test.TestTrue(TEXT("Landscape/Water change execution succeeds"), Execute.bSuccess);
	if (!Execute.bSuccess || !Execute.Data.IsValid())
	{
		Test.AddError(
			FString::Printf(
				TEXT("Landscape/Water change execution failed: %s (%s)"),
				*Execute.ErrorMessage,
				*Execute.ErrorCode));
		return false;
	}
	OutResult = Execute.Data;
	FString ArtifactDirectory;
	if (Execute.Data->TryGetStringField(
			TEXT("artifactDirectory"),
			ArtifactDirectory))
	{
		Fixture.AddArtifactDirectory(ArtifactDirectory);
	}
	if (OutRequestId)
	{
		*OutRequestId = RequestId;
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandscapeWaterSnapshotDiffTest,
	"UE_AI_integration.Landscape.SnapshotDiffContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeWaterSnapshotDiffTest::RunTest(const FString&)
{
	using UEAIIntegration::Infrastructure::FLandscapeWaterService;
	const TSharedPtr<FJsonObject> Before =
		MakeLandscapeSnapshot(TEXT("/Game/Materials/M_Landscape"));
	const FMCPToolResult Same =
		FLandscapeWaterService::DiffSnapshotObjects(Before, Before);
	TestTrue(TEXT("Identical verified snapshots are accepted"), Same.bSuccess);
	TestFalse(
		TEXT("Identical snapshots report no semantic change"),
		Same.Data->GetBoolField(TEXT("changed")));

	const TSharedPtr<FJsonObject> After =
		MakeLandscapeSnapshot(TEXT("/Game/Materials/M_Landscape_Changed"));
	const FMCPToolResult Changed =
		FLandscapeWaterService::DiffSnapshotObjects(Before, After);
	TestTrue(TEXT("Changed verified snapshots are accepted"), Changed.bSuccess);
	TestTrue(
		TEXT("A material change changes the semantic digest"),
		Changed.Data->GetBoolField(TEXT("changed")));
	TestTrue(
		TEXT("The bounded diff reports at least one changed path"),
		Changed.Data->GetNumberField(TEXT("changedPathCount")) >= 1.0);

	TSharedPtr<FJsonObject> Tampered = MakeShared<FJsonObject>(*After);
	Tampered->SetStringField(
		TEXT("snapshotDigest"),
		TEXT("sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
	const FMCPToolResult Invalid =
		FLandscapeWaterService::DiffSnapshotObjects(Before, Tampered);
	TestFalse(TEXT("A tampered snapshot is rejected"), Invalid.bSuccess);
	TestEqual(
		TEXT("Tampering uses the stable invalid_snapshot error"),
		Invalid.ErrorCode,
		FString(TEXT("invalid_snapshot")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandscapeWaterWriteGateTest,
	"UE_AI_integration.Landscape.WriteGateRequiresRequestId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeWaterWriteGateTest::RunTest(const FString&)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("request"), MakeShared<FJsonObject>());
	Params->SetStringField(
		TEXT("approvePlanDigest"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	Params->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult Result =
		UEAIIntegration::Infrastructure::FLandscapeWaterService::Get()
			.ExecuteChange(Params);
	TestFalse(TEXT("Execution without requestId is rejected"), Result.bSuccess);
	TestEqual(
		TEXT("The write gate returns request_id_required"),
		Result.ErrorCode,
		FString(TEXT("request_id_required")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandscapeWaterChangeRecoveryTest,
	"UE_AI_integration.Landscape.ChangeRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeWaterChangeRecoveryTest::RunTest(const FString&)
{
	FScopedWorldPartitionLandscapeFixture Fixture;
	FString FixtureError;
	if (!Fixture.Initialize(FixtureError))
	{
		AddError(FixtureError);
		return false;
	}
	FLandscapeWaterService& Service = FLandscapeWaterService::Get();
	const FMCPToolResult List =
		Service.ListLandscapes(MakeShared<FJsonObject>());
	if (!List.bSuccess || !List.Data.IsValid()
		|| List.Data->GetArrayField(TEXT("landscapes")).Num() != 1)
	{
		AddError(TEXT("The recovery fixture Landscape could not be resolved."));
		return false;
	}
	const FString LandscapeId =
		List.Data->GetArrayField(TEXT("landscapes"))[0]
			->AsObject()
			->GetStringField(TEXT("landscapeId"));
	ALandscape* Landscape = Fixture.GetLandscape();
	UMaterialInterface* OriginalMaterial =
		Landscape ? Landscape->GetLandscapeMaterial() : nullptr;
	UMaterialInterface* AlternateMaterial =
		Fixture.GetOrCreateAlternateMaterial();
	if (!Landscape || !OriginalMaterial || !AlternateMaterial
		|| AlternateMaterial == OriginalMaterial)
	{
		AddError(
			TEXT(
				"The fixture could not create a deterministic transient material "
				"distinct from the Landscape default."));
		return false;
	}

	auto MakeMaterialParams =
		[&]()
		{
			TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
			Operation->SetStringField(TEXT("action"), TEXT("setMaterial"));
			Operation->SetStringField(TEXT("landscape"), LandscapeId);
			Operation->SetStringField(
				TEXT("material"),
				AlternateMaterial->GetPathName());
			return MakeChangeParams(Operation);
		};
	auto SetFixtureMaterial =
		[Landscape](UMaterialInterface* Material)
		{
			Landscape->Modify();
			Landscape->LandscapeMaterial = Material;
			Landscape->MarkPackageDirty();
		};
	auto MakeRollbackParams =
		[](const FString& RunId, const FString& RequestId)
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("runId"), RunId);
			Params->SetStringField(TEXT("requestId"), RequestId);
			Params->SetBoolField(TEXT("confirmWrite"), true);
			return Params;
		};
	auto MakeExecutionParams =
		[](const TSharedRef<FJsonObject>& ChangeParams,
			const TSharedPtr<FJsonObject>& Plan,
			const FString& RequestId)
		{
			TSharedRef<FJsonObject> Execute =
				MakeShared<FJsonObject>(*ChangeParams);
			Execute->SetStringField(TEXT("requestId"), RequestId);
			Execute->SetStringField(
				TEXT("approvePlanDigest"),
				Plan->GetStringField(TEXT("planDigest")));
			Execute->SetBoolField(TEXT("confirmWrite"), true);
			return Execute;
		};
	auto TrackArtifact =
		[&Fixture](const TSharedPtr<FJsonObject>& Data)
		{
			if (!Data.IsValid())
			{
				return;
			}
			FString Directory;
			if (!Data->TryGetStringField(TEXT("artifactDirectory"), Directory))
			{
				Data->TryGetStringField(
					TEXT("recoveryArtifactDirectory"),
					Directory);
			}
			Fixture.AddArtifactDirectory(Directory);
		};

	TSharedPtr<FJsonObject> SuccessPlan;
	TSharedPtr<FJsonObject> SuccessResult;
	FString SuccessRequestId;
	if (!ApplyLandscapeChange(
			*this,
			Fixture,
			MakeMaterialParams(),
			SuccessPlan,
			SuccessResult,
			&SuccessRequestId))
	{
		return false;
	}
	TestTrue(
		TEXT("Successful execution is explicitly verified"),
		SuccessResult->GetBoolField(TEXT("verified")));
	TestTrue(
		TEXT("The planned material was applied"),
		Landscape->GetLandscapeMaterial() == AlternateMaterial);
	const FString SuccessRunId =
		SuccessResult->GetStringField(TEXT("runId"));
	const FString SuccessArtifactDirectory =
		SuccessResult->GetStringField(TEXT("artifactDirectory"));
	TestTrue(
		TEXT("Successful execution persists the after snapshot"),
		IFileManager::Get().FileExists(
			*FPaths::Combine(
				SuccessArtifactDirectory,
				TEXT("after.snapshot.json"))));
	TestTrue(
		TEXT("Successful execution persists the verified diff"),
		IFileManager::Get().FileExists(
			*FPaths::Combine(
				SuccessArtifactDirectory,
				TEXT("diff.json"))));

	const FMCPToolResult WrongOwner = Service.RollbackChange(
		MakeRollbackParams(
			SuccessRunId,
			TEXT("another-caller-request")));
	TestFalse(
		TEXT("A different requestId cannot own rollback"),
		WrongOwner.bSuccess);
	TestEqual(
		TEXT("Rollback owner mismatch has a stable error"),
		WrongOwner.ErrorCode,
		FString(TEXT("rollback_owner_mismatch")));

	SetFixtureMaterial(OriginalMaterial);
	const FMCPToolResult ExternalConflict =
		Service.RollbackChange(
			MakeRollbackParams(SuccessRunId, SuccessRequestId));
	TestFalse(
		TEXT("Rollback refuses an external semantic modification"),
		ExternalConflict.bSuccess);
	TestEqual(
		TEXT("External modification reports rollback_conflict"),
		ExternalConflict.ErrorCode,
		FString(TEXT("rollback_conflict")));

	SetFixtureMaterial(AlternateMaterial);
	const FMCPToolResult Rollback =
		Service.RollbackChange(
			MakeRollbackParams(SuccessRunId, SuccessRequestId));
	TestTrue(TEXT("Owned rollback succeeds"), Rollback.bSuccess);
	TestTrue(
		TEXT("Owned rollback is hash verified"),
		Rollback.bSuccess
			&& Rollback.Data->GetBoolField(TEXT("rollbackVerified")));
	TestTrue(
		TEXT("Owned rollback restores the original material"),
		Landscape->GetLandscapeMaterial() == OriginalMaterial);
	TestTrue(
		TEXT("Explicit rollback persists its evidence"),
		IFileManager::Get().FileExists(
			*FPaths::Combine(
				SuccessArtifactDirectory,
				TEXT("rollback.snapshot.json"))));
	const FMCPToolResult RollbackReplay =
		Service.RollbackChange(
			MakeRollbackParams(SuccessRunId, SuccessRequestId));
	TestTrue(TEXT("Verified rollback is idempotent"), RollbackReplay.bSuccess);
	TestTrue(
		TEXT("Idempotent rollback re-verifies current state"),
		RollbackReplay.bSuccess
			&& RollbackReplay.Data->GetBoolField(TEXT("idempotentReplay")));

	auto RunInjectedFailure =
		[&](
			const FName FailurePoint,
			const bool bExpectAutomaticEvidence,
			FString& OutRunId,
			FString& OutRequestId,
			TSharedPtr<FJsonObject>& OutFailureData) -> bool
		{
			const TSharedRef<FJsonObject> ChangeParams = MakeMaterialParams();
			const FMCPToolResult Plan = Service.PlanChange(ChangeParams);
			if (!Plan.bSuccess || !Plan.Data.IsValid())
			{
				AddError(
					FString::Printf(
						TEXT("Injected failure plan failed: %s (%s)"),
						*Plan.ErrorMessage,
						*Plan.ErrorCode));
				return false;
			}
			OutRequestId =
				TEXT("landscape-failure-")
				+ FGuid::NewGuid().ToString(
					EGuidFormats::DigitsWithHyphensLower);
			TSharedRef<FJsonObject> Execute = MakeExecutionParams(
				ChangeParams,
				Plan.Data,
				OutRequestId);
			Service.SetAutomationFailurePoint(FailurePoint);
			const FMCPToolResult Failure = Service.ExecuteChange(Execute);
			TestFalse(
				TEXT("Injected post-write failure returns an error"),
				Failure.bSuccess);
			if (Failure.bSuccess || !Failure.Data.IsValid())
			{
				AddError(TEXT("Injected failure did not return recovery details."));
				return false;
			}
			OutFailureData = Failure.Data;
			OutRunId = Failure.Data->GetStringField(TEXT("runId"));
			TrackArtifact(Failure.Data);
			TestFalse(
				TEXT("A failed execution never claims mutation verification"),
				Failure.Data->GetBoolField(TEXT("verified")));
			TestEqual(
				TEXT("Automatic rollback evidence matches the injected path"),
				Failure.Data->GetBoolField(TEXT("rollbackVerified")),
				bExpectAutomaticEvidence);
			TestTrue(
				TEXT("Automatic recovery restores the original material"),
				Landscape->GetLandscapeMaterial() == OriginalMaterial);
			const FMCPToolResult Replay = Service.ExecuteChange(Execute);
			TestFalse(
				TEXT("A failed requestId replay remains failed"),
				Replay.bSuccess);
			TestTrue(
				TEXT("A failed run remains registered and replayable"),
				Replay.Data.IsValid()
					&& Replay.Data->GetBoolField(TEXT("idempotentReplay")));
			return true;
		};

	FString AutoRunId;
	FString AutoRequestId;
	TSharedPtr<FJsonObject> AutoFailureData;
	if (!RunInjectedFailure(
			FName(TEXT("afterFirstOperation")),
			true,
			AutoRunId,
			AutoRequestId,
			AutoFailureData))
	{
		return false;
	}
	const FString AutoArtifactDirectory =
		AutoFailureData->GetStringField(
			TEXT("recoveryArtifactDirectory"));
	TestTrue(
		TEXT("Verified automatic rollback persists its evidence"),
		IFileManager::Get().FileExists(
			*FPaths::Combine(
				AutoArtifactDirectory,
				TEXT("automatic-rollback.snapshot.json"))));
	const FMCPToolResult AutoRollbackReplay =
		Service.RollbackChange(
			MakeRollbackParams(AutoRunId, AutoRequestId));
	TestTrue(
		TEXT("An automatically restored failed run remains addressable"),
		AutoRollbackReplay.bSuccess);
	TestTrue(
		TEXT("Automatic rollback replay is idempotent"),
		AutoRollbackReplay.bSuccess
			&& AutoRollbackReplay.Data->GetBoolField(
				TEXT("idempotentReplay")));

	FString RetryRunId;
	FString RetryRequestId;
	TSharedPtr<FJsonObject> RetryFailureData;
	if (!RunInjectedFailure(
			FName(TEXT("afterFirstOperationRollbackArtifact")),
			false,
			RetryRunId,
			RetryRequestId,
			RetryFailureData))
	{
		return false;
	}
	const FMCPToolResult RetriedRollback =
		Service.RollbackChange(
			MakeRollbackParams(RetryRunId, RetryRequestId));
	TestTrue(
		TEXT("A failed automatic evidence write can retry rollback by runId"),
		RetriedRollback.bSuccess);
	TestTrue(
		TEXT("Retried rollback produces verified evidence"),
		RetriedRollback.bSuccess
			&& RetriedRollback.Data->GetBoolField(
				TEXT("rollbackVerified")));
	TestTrue(
		TEXT("Retried rollback preserves the original material"),
		Landscape->GetLandscapeMaterial() == OriginalMaterial);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLandscapeWaterWorldPartitionVerticalTest,
	"UE_AI_integration.Landscape.WorldPartitionVertical",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLandscapeWaterWorldPartitionVerticalTest::RunTest(const FString&)
{
	FScopedWorldPartitionLandscapeFixture Fixture;
	FString FixtureError;
	if (!Fixture.Initialize(FixtureError))
	{
		AddError(FixtureError);
		return false;
	}
	TestNotNull(
		TEXT("The fixture is a real World Partition Editor world"),
		Fixture.GetWorld()->GetWorldPartition());
	TestTrue(
		TEXT("The fixture level uses external actor packages"),
		Fixture.GetWorld()->PersistentLevel
			&& Fixture.GetWorld()
				->PersistentLevel
				->IsUsingExternalActors());
	TestNotNull(
		TEXT("The fixture contains a real Landscape actor"),
		Fixture.GetLandscape());
	TestTrue(
		TEXT("The fixture was saved and reloaded from /Game/Automation"),
		Fixture.WasReloadedFromDisk());

	FLandscapeWaterService& Service = FLandscapeWaterService::Get();
	const TSharedRef<FJsonObject> EmptyParams = MakeShared<FJsonObject>();
	const FMCPToolResult List = Service.ListLandscapes(EmptyParams);
	TestTrue(TEXT("Landscape list succeeds"), List.bSuccess);
	if (!List.bSuccess || !List.Data.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("The isolated world contains one Landscape"),
		static_cast<int32>(List.Data->GetNumberField(TEXT("total"))),
		1);
	const TArray<TSharedPtr<FJsonValue>>& Landscapes =
		List.Data->GetArrayField(TEXT("landscapes"));
	if (Landscapes.Num() != 1 || !Landscapes[0]->AsObject().IsValid())
	{
		AddError(TEXT("Landscape list did not return the fixture Landscape."));
		return false;
	}
	const FString LandscapeId =
		Landscapes[0]->AsObject()->GetStringField(TEXT("landscapeId"));
	TestFalse(
		TEXT("Landscape list returns a stable non-empty identity"),
		LandscapeId.IsEmpty());

	TSharedRef<FJsonObject> LandscapeParams = MakeShared<FJsonObject>();
	LandscapeParams->SetStringField(TEXT("landscape"), LandscapeId);
	const FMCPToolResult Get = Service.GetLandscape(LandscapeParams);
	TestTrue(TEXT("Landscape get succeeds"), Get.bSuccess);
	if (!Get.bSuccess || !Get.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Extent =
		Get.Data->GetObjectField(TEXT("extent"));
	TestEqual(
		TEXT("The imported Landscape width is real"),
		static_cast<int32>(Extent->GetNumberField(TEXT("width"))),
		8);
	TestEqual(
		TEXT("The imported Landscape height is real"),
		static_cast<int32>(Extent->GetNumberField(TEXT("height"))),
		8);
	TestTrue(
		TEXT("The Landscape exposes loaded components"),
		Get.Data->GetNumberField(TEXT("componentCount")) >= 1.0);

	const FMCPToolResult Layers =
		Service.GetLandscapeLayers(LandscapeParams);
	TestTrue(TEXT("Landscape layers query succeeds"), Layers.bSuccess);
	TestEqual(
		TEXT("The fixture exposes one real painted weight layer"),
		Layers.bSuccess && Layers.Data.IsValid()
			? static_cast<int32>(
				Layers.Data->GetNumberField(TEXT("total")))
			: -1,
		1);
	if (Layers.bSuccess && Layers.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& LayerValues =
			Layers.Data->GetArrayField(TEXT("layers"));
		TestEqual(
			TEXT("The painted layer has the expected name"),
			LayerValues.Num() == 1
				? LayerValues[0]->AsObject()->GetStringField(TEXT("name"))
				: FString(),
			FString(TEXT("UEAI_Weight")));
		TestTrue(
			TEXT("The painted layer owns a real Layer Info object"),
			LayerValues.Num() == 1
				&& LayerValues[0]->AsObject()->GetBoolField(
					TEXT("hasLayerInfo")));
		TestFalse(
			TEXT("The Layer Info path is recorded"),
			LayerValues.Num() == 1
				? LayerValues[0]->AsObject()
					->GetStringField(TEXT("layerInfo"))
					.IsEmpty()
				: true);
	}
	TSharedRef<FJsonObject> WeightExportParams =
		MakeShared<FJsonObject>(*LandscapeParams);
	WeightExportParams->SetStringField(
		TEXT("layer"),
		TEXT("UEAI_Weight"));
	WeightExportParams->SetStringField(
		TEXT("outputName"),
		TEXT("automation-weight-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FMCPToolResult WeightExport =
		Service.ExportWeightmap(WeightExportParams);
	TestTrue(
		TEXT("A real painted weightmap can be exported"),
		WeightExport.bSuccess);
	if (!WeightExport.bSuccess || !WeightExport.Data.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("The exported r8 covers the complete 8x8 extent"),
		static_cast<int32>(
			WeightExport.Data->GetNumberField(TEXT("size"))),
		64);
	TestFalse(
		TEXT("The exported weightmap has a content digest"),
		WeightExport.Data->GetStringField(TEXT("sha256")).IsEmpty());
	const FString WeightPath =
		WeightExport.Data->GetStringField(TEXT("path"));
	const FString WeightSidecar =
		WeightExport.Data->GetStringField(TEXT("sidecarPath"));
	Fixture.AddArtifactFile(WeightPath);
	Fixture.AddArtifactFile(WeightSidecar);
	TestTrue(
		TEXT("The r8 weight artifact exists"),
		IFileManager::Get().FileExists(*WeightPath));
	TestTrue(
		TEXT("The r8 sidecar exists"),
		IFileManager::Get().FileExists(*WeightSidecar));
	TSharedRef<FJsonObject> ImportWeight =
		MakeShared<FJsonObject>();
	ImportWeight->SetStringField(TEXT("action"), TEXT("importWeightmap"));
	ImportWeight->SetStringField(TEXT("landscape"), LandscapeId);
	ImportWeight->SetStringField(TEXT("layer"), TEXT("UEAI_Weight"));
	ImportWeight->SetStringField(TEXT("sourcePath"), WeightPath);
	TSharedRef<FJsonObject> ReplaceLayer =
		MakeShared<FJsonObject>();
	ReplaceLayer->SetStringField(TEXT("action"), TEXT("replaceLayerInfo"));
	ReplaceLayer->SetStringField(TEXT("landscape"), LandscapeId);
	ReplaceLayer->SetStringField(TEXT("layer"), TEXT("UEAI_Weight"));
	ReplaceLayer->SetStringField(
		TEXT("layerInfo"),
		Layers.Data->GetArrayField(TEXT("layers"))[0]
			->AsObject()
			->GetStringField(TEXT("layerInfo")));
	TSharedRef<FJsonObject> ConflictingRequest =
		MakeShared<FJsonObject>();
	ConflictingRequest->SetArrayField(
		TEXT("operations"),
		{
			MakeShared<FJsonValueObject>(ImportWeight),
			MakeShared<FJsonValueObject>(ReplaceLayer)
		});
	TSharedRef<FJsonObject> ConflictingParams =
		MakeShared<FJsonObject>();
	ConflictingParams->SetObjectField(
		TEXT("request"),
		ConflictingRequest);
	const FMCPToolResult ConflictingLayerPlan =
		Service.PlanChange(ConflictingParams);
	TestFalse(
		TEXT("A weight import and Layer Info replacement cannot target the same layer"),
		ConflictingLayerPlan.bSuccess);
	TestEqual(
		TEXT("Same-layer mutations use the stable conflict error"),
		ConflictingLayerPlan.ErrorCode,
		FString(TEXT("change_conflict")));

	const FMCPToolResult Validate =
		Service.ValidateLandscape(LandscapeParams);
	TestTrue(TEXT("Landscape validation executes"), Validate.bSuccess);
	if (!Validate.bSuccess || !Validate.Data.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("The minimal Landscape has no structural findings"),
		static_cast<int32>(
			Validate.Data->GetNumberField(TEXT("findingCount"))),
		0);

	TSharedRef<FJsonObject> SnapshotParams =
		MakeShared<FJsonObject>(*LandscapeParams);
	SnapshotParams->SetBoolField(TEXT("includeWater"), false);
	const FMCPToolResult Before =
		Service.SnapshotLandscape(SnapshotParams);
	TestTrue(TEXT("Landscape snapshot succeeds"), Before.bSuccess);
	if (!Before.bSuccess || !Before.Data.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> BeforeState =
		Before.Data->GetObjectField(TEXT("state"));
	const FString EmptyWaterEnvironmentDigest =
		BeforeState
			->GetObjectField(TEXT("waterEnvironment"))
			->GetStringField(TEXT("digest"));
	TestFalse(
		TEXT("The baseline snapshot records Water helper and edit-layer topology"),
		EmptyWaterEnvironmentDigest.IsEmpty());
	const TArray<TSharedPtr<FJsonValue>>& LandscapeStates =
		BeforeState->GetArrayField(TEXT("landscapes"));
	if (LandscapeStates.Num() != 1
		|| !LandscapeStates[0]->AsObject().IsValid())
	{
		AddError(
			TEXT("The semantic snapshot did not contain the fixture Landscape state."));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& LayerData =
		LandscapeStates[0]->AsObject()->GetArrayField(
			TEXT("layerData"));
	TestEqual(
		TEXT("The semantic snapshot includes the painted layer"),
		LayerData.Num(),
		1);
	TestEqual(
		TEXT("The snapshot records the complete r8 layer byte count"),
		LayerData.Num() == 1
			? static_cast<int32>(
				LayerData[0]->AsObject()->GetNumberField(
					TEXT("byteLength")))
			: -1,
		64);
	TestFalse(
		TEXT("The painted layer snapshot includes a content digest"),
		LayerData.Num() == 1
			? LayerData[0]->AsObject()
				->GetStringField(TEXT("sha256"))
				.IsEmpty()
			: true);
	const TSharedPtr<FJsonObject> Impact =
		BeforeState->GetObjectField(TEXT("impact"));
	const TSharedPtr<FJsonObject> Partition =
		Impact->GetObjectField(TEXT("worldPartition"));
	TestTrue(
		TEXT("Snapshot evidence records a real partitioned world"),
		Partition->GetBoolField(TEXT("partitioned")));
	const TArray<TSharedPtr<FJsonValue>>& AffectedActors =
		Impact->GetArrayField(TEXT("affectedActors"));
	TestTrue(
		TEXT("Landscape actor assignments participate in impact analysis"),
		AffectedActors.ContainsByPredicate(
			[&Fixture](const TSharedPtr<FJsonValue>& Value)
			{
				return Value.IsValid()
					&& Value->AsString()
						== Fixture.GetLandscape()->GetPathName();
			}));
	auto StringArrayContains =
		[](const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const FString& Expected)
		{
			return Object.IsValid()
				&& Object->GetArrayField(Field).ContainsByPredicate(
					[&Expected](
						const TSharedPtr<FJsonValue>& Value)
					{
						return Value.IsValid()
							&& Value->AsString() == Expected;
					});
		};
	auto AssertImpactEvidence =
		[this, &Fixture, &StringArrayContains](
			const FString& EvidenceSource,
			const TSharedPtr<FJsonObject>& ImpactEvidence)
		{
			if (!ImpactEvidence.IsValid())
			{
				AddError(
					EvidenceSource
						+ TEXT(" did not contain an impact object."));
				return;
			}
			const TSharedPtr<FJsonObject> DataLayers =
				ImpactEvidence->GetObjectField(TEXT("dataLayers"));
			TestTrue(
				*(EvidenceSource
					+ TEXT(" counts the runtime Data Layer instance")),
				DataLayers->GetNumberField(TEXT("total")) >= 1.0
					&& DataLayers->GetNumberField(TEXT("runtime")) >= 1.0);
			TestTrue(
				*(EvidenceSource
					+ TEXT(" reports the Landscape Data Layer assignment")),
				StringArrayContains(
					DataLayers,
					TEXT("affected"),
					Fixture.GetDataLayerAssetPath()));

			const TSharedPtr<FJsonObject> HLOD =
				ImpactEvidence->GetObjectField(TEXT("hlod"));
			TestTrue(
				*(EvidenceSource
					+ TEXT(" reports the Landscape HLOD Layer assignment")),
				StringArrayContains(
					HLOD,
					TEXT("affectedLayers"),
					Fixture.GetHLODLayerPath()));
			TestTrue(
				*(EvidenceSource
					+ TEXT(" marks HLOD rebuild impact")),
				HLOD->GetBoolField(TEXT("rebuildMayBeRequired")));

			const TSharedPtr<FJsonObject> PCG =
				ImpactEvidence->GetObjectField(TEXT("pcg"));
			TestTrue(
				*(EvidenceSource
					+ TEXT(" reports the overlapping PCG Component")),
				StringArrayContains(
					PCG,
					TEXT("components"),
					Fixture.GetPCGComponentPath()));
			TestTrue(
				*(EvidenceSource
					+ TEXT(" marks PCG regeneration impact")),
				PCG->GetBoolField(
					TEXT("regenerationMayBeRequired")));
		};
	AssertImpactEvidence(
		TEXT("Landscape snapshot"),
		Impact);

	TSharedRef<FJsonObject> ImpactPlanOperation =
		MakeShared<FJsonObject>();
	ImpactPlanOperation->SetStringField(
		TEXT("action"),
		TEXT("setMaterial"));
	ImpactPlanOperation->SetStringField(
		TEXT("landscape"),
		LandscapeId);
	ImpactPlanOperation->SetStringField(
		TEXT("material"),
		Fixture.GetLandscape()
			->GetLandscapeMaterial()
			->GetPathName());
	const FMCPToolResult ImpactPlan =
		Service.PlanChange(
			MakeChangeParams(ImpactPlanOperation));
	TestTrue(
		TEXT("A real Landscape change plan succeeds"),
		ImpactPlan.bSuccess);
	if (!ImpactPlan.bSuccess || !ImpactPlan.Data.IsValid())
	{
		AddError(
			FString::Printf(
				TEXT("Landscape impact planning failed: %s (%s)"),
				*ImpactPlan.ErrorMessage,
				*ImpactPlan.ErrorCode));
		return false;
	}
	AssertImpactEvidence(
		TEXT("Landscape change plan"),
		ImpactPlan.Data->GetObjectField(TEXT("impact")));

	ULandscapeInfo* LandscapeInfo =
		Fixture.GetLandscape()->GetLandscapeInfo();
	FIntRect LandscapeExtent;
	if (!LandscapeInfo
		|| !LandscapeInfo->GetLandscapeExtent(LandscapeExtent))
	{
		AddError(TEXT("The fixture Landscape extent became unavailable."));
		return false;
	}
	const int32 Width = LandscapeExtent.Width() + 1;
	const int32 Height = LandscapeExtent.Height() + 1;
	TArray<uint16> ChangedHeights;
	ChangedHeights.Init(32768, Width * Height);
	ChangedHeights[(Height / 2) * Width + (Width / 2)] = 33024;
	{
		FHeightmapAccessor<false> Accessor(LandscapeInfo);
		Accessor.SetData(
			LandscapeExtent.Min.X,
			LandscapeExtent.Min.Y,
			LandscapeExtent.Max.X,
			LandscapeExtent.Max.Y,
			ChangedHeights.GetData());
		Accessor.Flush();
	}

	const FMCPToolResult After =
		Service.SnapshotLandscape(SnapshotParams);
	TestTrue(TEXT("Changed Landscape snapshot succeeds"), After.bSuccess);
	if (!After.bSuccess || !After.Data.IsValid())
	{
		return false;
	}
	const FMCPToolResult Diff =
		FLandscapeWaterService::DiffSnapshotObjects(
			Before.Data,
			After.Data);
	TestTrue(TEXT("Real Landscape snapshots can be diffed"), Diff.bSuccess);
	TestTrue(
		TEXT("Height data mutation changes the semantic snapshot"),
		Diff.bSuccess
			&& Diff.Data->GetBoolField(TEXT("changed")));
	TestTrue(
		TEXT("The bounded diff identifies a changed path"),
		Diff.bSuccess
			&& Diff.Data->GetNumberField(TEXT("changedPathCount")) >= 1.0);

	const FMCPToolResult WaterAvailability =
		Service.ListWater(EmptyParams);
	TestTrue(TEXT("Water discovery executes"), WaterAvailability.bSuccess);
	if (!WaterAvailability.bSuccess
		|| !WaterAvailability.Data.IsValid()
		|| !WaterAvailability.Data->GetBoolField(TEXT("available")))
	{
		AddError(
			TEXT("Water editor factories are required for the Landscape/Water release-blocking vertical test."));
		return false;
	}

	TSharedRef<FJsonObject> CreateOperation = MakeShared<FJsonObject>();
	CreateOperation->SetStringField(TEXT("action"), TEXT("waterCreate"));
	CreateOperation->SetStringField(TEXT("type"), TEXT("lake"));
	CreateOperation->SetStringField(
		TEXT("label"),
		TEXT("UEAI Water Fixture"));
	CreateOperation->SetObjectField(
		TEXT("transform"),
		MakeTransformJson(FVector(120.0, 240.0, 360.0)));
	TSharedPtr<FJsonObject> CreatePlan;
	TSharedPtr<FJsonObject> CreateResult;
	FString CreateRequestId;
	if (!ApplyLandscapeChange(
			*this,
			Fixture,
			MakeChangeParams(CreateOperation),
			CreatePlan,
			CreateResult,
			&CreateRequestId))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& CreateOperations =
		CreatePlan->GetArrayField(TEXT("operations"));
	const FString ManagedId =
		CreateOperations[0]->AsObject()->GetStringField(
			TEXT("managedId"));

	TSharedRef<FJsonObject> WaterParams = MakeShared<FJsonObject>();
	WaterParams->SetStringField(TEXT("water"), ManagedId);
	const FMCPToolResult CreatedWater = Service.GetWater(WaterParams);
	TestTrue(TEXT("Created managed Water can be read back"), CreatedWater.bSuccess);
	TestEqual(
		TEXT("Created Water label is preserved"),
		CreatedWater.bSuccess
			? CreatedWater.Data->GetStringField(TEXT("label"))
			: FString(),
		FString(TEXT("UEAI Water Fixture")));
	TestTrue(
		TEXT("Created Water uses a World Partition external actor package"),
		CreatedWater.bSuccess
			&& CreatedWater.Data->GetBoolField(TEXT("externalActor")));
	const FString CreatedExternalPackage =
		CreatedWater.bSuccess
			? CreatedWater.Data->GetStringField(TEXT("externalPackage"))
			: FString();
	TestFalse(
		TEXT("Created Water exposes its external actor package identity"),
		CreatedExternalPackage.IsEmpty());
	TestFalse(
		TEXT("Create result records the baseline package digest"),
		CreateResult->GetStringField(TEXT("beforePackageDigest")).IsEmpty());
	TestFalse(
		TEXT("Create result records the changed package digest"),
		CreateResult->GetStringField(TEXT("afterPackageDigest")).IsEmpty());
	const FMCPToolResult CreatedEnvironment =
		Service.SnapshotLandscape(SnapshotParams);
	TestTrue(
		TEXT("Water creation environment snapshot succeeds"),
		CreatedEnvironment.bSuccess);
	const FString CreatedWaterEnvironmentDigest =
		CreatedEnvironment.bSuccess && CreatedEnvironment.Data.IsValid()
			? CreatedEnvironment.Data
				->GetObjectField(TEXT("state"))
				->GetObjectField(TEXT("waterEnvironment"))
				->GetStringField(TEXT("digest"))
			: FString();
	TestNotEqual(
		TEXT("Water creation records helper actors and edit-layer side effects"),
		CreatedWaterEnvironmentDigest,
		EmptyWaterEnvironmentDigest);

	TSharedRef<FJsonObject> CreateRollbackParams =
		MakeShared<FJsonObject>();
	CreateRollbackParams->SetStringField(
		TEXT("runId"),
		CreateResult->GetStringField(TEXT("runId")));
	CreateRollbackParams->SetStringField(
		TEXT("requestId"),
		CreateRequestId);
	CreateRollbackParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult CreateRollback =
		Service.RollbackChange(CreateRollbackParams);
	if (!CreateRollback.bSuccess)
	{
		AddError(
			FString::Printf(
				TEXT("Water create rollback failed: %s (%s)"),
				*CreateRollback.ErrorMessage,
				*CreateRollback.ErrorCode));
	}
	TestTrue(
		TEXT("Water create rollback succeeds"),
		CreateRollback.bSuccess);
	TestTrue(
		TEXT("Water create rollback restores the package evidence digest"),
		CreateRollback.bSuccess
			&& CreateRollback.Data->GetStringField(TEXT("packageDigest"))
				== CreateResult->GetStringField(
					TEXT("beforePackageDigest")));
	TestFalse(
		TEXT("Water create rollback removes the managed actor"),
		Service.GetWater(WaterParams).bSuccess);
	TestFalse(
		TEXT("Water create rollback leaves no external package on disk"),
		FPackageName::DoesPackageExist(CreatedExternalPackage));
	TestFalse(
		TEXT("Water create rollback detaches the external package from the world"),
		Fixture.GetWorld()->GetPackage()->GetExternalPackages()
			.ContainsByPredicate(
				[&CreatedExternalPackage](const UPackage* Candidate)
				{
					return Candidate
						&& Candidate->GetName()
							== CreatedExternalPackage;
				}));
	const FMCPToolResult CreateRollbackEnvironment =
		Service.SnapshotLandscape(SnapshotParams);
	TestTrue(
		TEXT("Water create rollback environment snapshot succeeds"),
		CreateRollbackEnvironment.bSuccess);
	TestEqual(
		TEXT("Water create rollback restores helper actors, edit layers, and brush bindings"),
		CreateRollbackEnvironment.bSuccess
				&& CreateRollbackEnvironment.Data.IsValid()
			? CreateRollbackEnvironment.Data
				->GetObjectField(TEXT("state"))
				->GetObjectField(TEXT("waterEnvironment"))
				->GetStringField(TEXT("digest"))
			: FString(),
		EmptyWaterEnvironmentDigest);

	// Recreate the same deterministic managed identity for update/delete
	// coverage after proving that create itself is fully reversible.
	CreatePlan.Reset();
	CreateResult.Reset();
	if (!ApplyLandscapeChange(
			*this,
			Fixture,
			MakeChangeParams(CreateOperation),
			CreatePlan,
			CreateResult))
	{
		return false;
	}
	const FMCPToolResult RecreatedWater = Service.GetWater(WaterParams);
	TestTrue(
		TEXT("Managed Water can be recreated after verified rollback"),
		RecreatedWater.bSuccess);
	TestTrue(
		TEXT("Recreated Water remains an external actor"),
		RecreatedWater.bSuccess
			&& RecreatedWater.Data->GetBoolField(TEXT("externalActor")));
	const FMCPToolResult ValidWater =
		Service.ValidateWater(WaterParams);
	TestTrue(TEXT("Created managed Water can be validated"), ValidWater.bSuccess);
	TestTrue(
		TEXT("Created managed Water has its required components"),
		ValidWater.bSuccess
			&& ValidWater.Data->GetBoolField(TEXT("valid")));
	const FMCPToolResult BeforeDeleteEnvironment =
		Service.SnapshotLandscape(SnapshotParams);
	TestTrue(
		TEXT("Water delete baseline environment snapshot succeeds"),
		BeforeDeleteEnvironment.bSuccess);
	const FString BeforeDeleteWaterEnvironmentDigest =
		BeforeDeleteEnvironment.bSuccess
				&& BeforeDeleteEnvironment.Data.IsValid()
			? BeforeDeleteEnvironment.Data
				->GetObjectField(TEXT("state"))
				->GetObjectField(TEXT("waterEnvironment"))
				->GetStringField(TEXT("digest"))
			: FString();

	TSharedRef<FJsonObject> UpdateOperation = MakeShared<FJsonObject>();
	UpdateOperation->SetStringField(TEXT("action"), TEXT("waterUpdate"));
	UpdateOperation->SetStringField(TEXT("water"), ManagedId);
	UpdateOperation->SetStringField(
		TEXT("label"),
		TEXT("UEAI Water Fixture Updated"));
	UpdateOperation->SetObjectField(
		TEXT("transform"),
		MakeTransformJson(FVector(420.0, 240.0, 360.0)));
	TSharedPtr<FJsonObject> UpdatePlan;
	TSharedPtr<FJsonObject> UpdateResult;
	if (!ApplyLandscapeChange(
			*this,
			Fixture,
			MakeChangeParams(UpdateOperation),
			UpdatePlan,
			UpdateResult))
	{
		return false;
	}
	FMCPToolResult UpdatedWater = Service.GetWater(WaterParams);
	TestTrue(TEXT("Updated Water remains readable"), UpdatedWater.bSuccess);
	TestEqual(
		TEXT("Water update changes the label"),
		UpdatedWater.bSuccess
			? UpdatedWater.Data->GetStringField(TEXT("label"))
			: FString(),
		FString(TEXT("UEAI Water Fixture Updated")));
	if (!UpdatedWater.bSuccess || !UpdatedWater.Data.IsValid())
	{
		return false;
	}
	AActor* ManagedWaterActor = FindObject<AActor>(
		nullptr,
		*UpdatedWater.Data->GetStringField(TEXT("path")));
	if (!ManagedWaterActor)
	{
		AddError(
			TEXT("The managed Water actor could not be resolved for full-state recovery validation."));
		return false;
	}
	ManagedWaterActor->Modify();
	ManagedWaterActor->SetActorEnableCollision(false);
	ManagedWaterActor->MarkPackageDirty();
	UpdatedWater = Service.GetWater(WaterParams);
	TestTrue(
		TEXT("Water remains readable after a non-layout actor property change"),
		UpdatedWater.bSuccess);
	if (!UpdatedWater.bSuccess || !UpdatedWater.Data.IsValid())
	{
		return false;
	}
	TestFalse(
		TEXT("Water read-back includes a complete property digest"),
		UpdatedWater.Data->GetStringField(TEXT("propertyDigest")).IsEmpty());
	TestFalse(
		TEXT("Water read-back includes its actor GUID"),
		UpdatedWater.Data->GetStringField(TEXT("actorGuid")).IsEmpty());

	TSharedRef<FJsonObject> DeleteOperation = MakeShared<FJsonObject>();
	DeleteOperation->SetStringField(TEXT("action"), TEXT("waterDelete"));
	DeleteOperation->SetStringField(TEXT("water"), ManagedId);
	const TArray<FName> ManagedWaterTags = ManagedWaterActor->Tags;
	ManagedWaterActor->Tags.Remove(FName(TEXT("UEAI.ManagedWater")));
	const FMCPToolResult UnmanagedDeletePlan =
		Service.PlanChange(MakeChangeParams(DeleteOperation));
	TestFalse(
		TEXT("Water delete rejects actors outside service ownership"),
		UnmanagedDeletePlan.bSuccess);
	TestEqual(
		TEXT("Unmanaged Water delete has a stable error"),
		UnmanagedDeletePlan.ErrorCode,
		FString(TEXT("water_delete_not_managed")));
	ManagedWaterActor->Tags = ManagedWaterTags;

	TSharedPtr<FJsonObject> DeletePlan;
	TSharedPtr<FJsonObject> DeleteResult;
	FString DeleteRequestId;
	if (!ApplyLandscapeChange(
			*this,
			Fixture,
			MakeChangeParams(DeleteOperation),
			DeletePlan,
			DeleteResult,
			&DeleteRequestId))
	{
		return false;
	}
	const FMCPToolResult DeletedWater = Service.GetWater(WaterParams);
	TestFalse(
		TEXT("Approved Water delete removes the managed actor"),
		DeletedWater.bSuccess);
	TestEqual(
		TEXT("Deleted Water is no longer discoverable"),
		DeletedWater.ErrorCode,
		FString(TEXT("water_not_found")));
	const FMCPToolResult DeletedEnvironment =
		Service.SnapshotLandscape(SnapshotParams);
	TestEqual(
		TEXT("Water delete preserves existing helpers and Landscape bindings"),
		DeletedEnvironment.bSuccess
				&& DeletedEnvironment.Data.IsValid()
			? DeletedEnvironment.Data
				->GetObjectField(TEXT("state"))
				->GetObjectField(TEXT("waterEnvironment"))
				->GetStringField(TEXT("digest"))
			: FString(),
		BeforeDeleteWaterEnvironmentDigest);
	const FString DeleteArtifactDirectory =
		DeleteResult->GetStringField(TEXT("artifactDirectory"));
	TArray<FString> WaterActorArtifacts;
	IFileManager::Get().FindFiles(
		WaterActorArtifacts,
		*FPaths::Combine(
			DeleteArtifactDirectory,
			TEXT("*.t3d")),
		true,
		false);
	TestEqual(
		TEXT("Water delete persists exactly one complete actor recovery artifact"),
		WaterActorArtifacts.Num(),
		1);

	TSharedRef<FJsonObject> RollbackParams = MakeShared<FJsonObject>();
	RollbackParams->SetStringField(
		TEXT("runId"),
		DeleteResult->GetStringField(TEXT("runId")));
	RollbackParams->SetStringField(TEXT("requestId"), DeleteRequestId);
	RollbackParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult DeleteRollback =
		Service.RollbackChange(RollbackParams);
	if (!DeleteRollback.bSuccess)
	{
		AddError(
			FString::Printf(
				TEXT("Water delete rollback failed: %s (%s)"),
				*DeleteRollback.ErrorMessage,
				*DeleteRollback.ErrorCode));
	}
	TestTrue(
		TEXT("Water delete rollback succeeds"),
		DeleteRollback.bSuccess);
	TestTrue(
		TEXT("Water delete rollback is read-back verified"),
		DeleteRollback.bSuccess
			&& DeleteRollback.Data->GetBoolField(
				TEXT("rollbackVerified")));
	const FMCPToolResult RestoredWater = Service.GetWater(WaterParams);
	TestTrue(
		TEXT("Water delete rollback recreates the managed actor"),
		RestoredWater.bSuccess);
	const FMCPToolResult RestoredEnvironment =
		Service.SnapshotLandscape(SnapshotParams);
	TestEqual(
		TEXT("Water delete rollback creates no extra helper, layer, or brush binding"),
		RestoredEnvironment.bSuccess
				&& RestoredEnvironment.Data.IsValid()
			? RestoredEnvironment.Data
				->GetObjectField(TEXT("state"))
				->GetObjectField(TEXT("waterEnvironment"))
				->GetStringField(TEXT("digest"))
			: FString(),
		BeforeDeleteWaterEnvironmentDigest);
	if (!RestoredWater.bSuccess || !RestoredWater.Data.IsValid())
	{
		return false;
	}
	for (const TCHAR* Field :
		{
			TEXT("managedId"),
			TEXT("class"),
			TEXT("actorGuid"),
			TEXT("level"),
			TEXT("package"),
			TEXT("externalPackage"),
			TEXT("propertyDigest")
		})
	{
		TestEqual(
			FString::Printf(
				TEXT("Water delete rollback restores %s"),
				Field),
			RestoredWater.Data->GetStringField(Field),
			UpdatedWater.Data->GetStringField(Field));
	}
	TestEqual(
		TEXT("Water delete rollback restores external-actor mode"),
		RestoredWater.Data->GetBoolField(TEXT("externalActor")),
		UpdatedWater.Data->GetBoolField(TEXT("externalActor")));
	AActor* RestoredWaterActor = FindObject<AActor>(
		nullptr,
		*RestoredWater.Data->GetStringField(TEXT("path")));
	TestTrue(
		TEXT("Water delete rollback restores non-layout actor properties"),
		RestoredWaterActor
			&& !RestoredWaterActor->GetActorEnableCollision());

	const TSharedRef<FJsonObject> FailureChangeParams =
		MakeChangeParams(DeleteOperation);
	const FMCPToolResult FailurePlan =
		Service.PlanChange(FailureChangeParams);
	TestTrue(
		TEXT("Water delete failure-restoration plan succeeds"),
		FailurePlan.bSuccess);
	if (!FailurePlan.bSuccess || !FailurePlan.Data.IsValid())
	{
		return false;
	}
	const FString FailureRequestId =
		TEXT("water-delete-failure-")
		+ FGuid::NewGuid().ToString(
			EGuidFormats::DigitsWithHyphensLower);
	TSharedRef<FJsonObject> FailureExecute =
		MakeShared<FJsonObject>(*FailureChangeParams);
	FailureExecute->SetStringField(
		TEXT("requestId"),
		FailureRequestId);
	FailureExecute->SetStringField(
		TEXT("approvePlanDigest"),
		FailurePlan.Data->GetStringField(TEXT("planDigest")));
	FailureExecute->SetBoolField(TEXT("confirmWrite"), true);
	Service.SetAutomationFailurePoint(
		FName(TEXT("afterFirstOperation")));
	const FMCPToolResult InjectedDeleteFailure =
		Service.ExecuteChange(FailureExecute);
	TestFalse(
		TEXT("Injected failure after Water deletion returns an error"),
		InjectedDeleteFailure.bSuccess);
	TestTrue(
		TEXT("Injected Water delete failure reports verified automatic rollback"),
		!InjectedDeleteFailure.bSuccess
			&& InjectedDeleteFailure.Data.IsValid()
			&& InjectedDeleteFailure.Data->GetBoolField(
				TEXT("rollbackVerified")));
	if (InjectedDeleteFailure.Data.IsValid())
	{
		Fixture.AddArtifactDirectory(
			InjectedDeleteFailure.Data->GetStringField(
				TEXT("recoveryArtifactDirectory")));
	}
	const FMCPToolResult FailureRestoredWater =
		Service.GetWater(WaterParams);
	TestTrue(
		TEXT("Automatic failure recovery recreates the deleted Water actor"),
		FailureRestoredWater.bSuccess);
	if (FailureRestoredWater.bSuccess
		&& FailureRestoredWater.Data.IsValid())
	{
		TestEqual(
			TEXT("Automatic failure recovery restores full Water properties"),
			FailureRestoredWater.Data->GetStringField(
				TEXT("propertyDigest")),
			UpdatedWater.Data->GetStringField(
				TEXT("propertyDigest")));
		TestEqual(
			TEXT("Automatic failure recovery restores external package identity"),
			FailureRestoredWater.Data->GetStringField(
				TEXT("externalPackage")),
			UpdatedWater.Data->GetStringField(
				TEXT("externalPackage")));
		AActor* FailureRestoredActor = FindObject<AActor>(
			nullptr,
			*FailureRestoredWater.Data->GetStringField(TEXT("path")));
			TestTrue(
				TEXT("Automatic failure recovery restores non-layout actor properties"),
				FailureRestoredActor
					&& !FailureRestoredActor->GetActorEnableCollision());
	}
	FString CleanupError;
	const bool bCleanupSucceeded = Fixture.Cleanup(CleanupError);
	TestTrue(
		TEXT("Saved World Partition fixture leaves no disk or registry residue"),
		bCleanupSucceeded);
	if (!bCleanupSucceeded)
	{
		AddError(CleanupError);
	}
	return bCleanupSucceeded;
}

#endif
