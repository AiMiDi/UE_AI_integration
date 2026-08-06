#include "Infrastructure/BlueprintPersistence.h"

#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
bool Fail(
	FBlueprintPersistenceError& OutError,
	const TCHAR* Code,
	const FString& Message,
	const int32 HttpStatus = 422)
{
	OutError.Code = Code;
	OutError.Message = Message;
	OutError.HttpStatus = HttpStatus;
	return false;
}
}

const TCHAR* BlueprintPackageKindName(const EBlueprintPackageKind Kind)
{
	return Kind == EBlueprintPackageKind::Map ? TEXT("map") : TEXT("asset");
}

bool ResolveBlueprintPersistenceTarget(
	UBlueprint* Blueprint,
	FBlueprintPersistenceTarget& OutTarget,
	FBlueprintPersistenceError& OutError)
{
	OutTarget = FBlueprintPersistenceTarget();
	OutError.Reset();
	if (!Blueprint || !IsValid(Blueprint))
	{
		return Fail(
			OutError,
			TEXT("target_asset_type_unsupported"),
			TEXT("A valid Blueprint is required."));
	}

	UPackage* Package = Blueprint->GetOutermost();
	if (!Package)
	{
		return Fail(
			OutError,
			TEXT("asset_package_missing"),
			TEXT("The Blueprint has no owning package."),
			500);
	}
	const FString PackageName = Package->GetName();
	if (!FPackageName::IsValidLongPackageName(PackageName)
		|| PackageName.StartsWith(TEXT("/Temp/"))
		|| PackageName.StartsWith(TEXT("/Engine/Transient"))
		|| Package == GetTransientPackage()
		|| Package->HasAnyPackageFlags(PKG_PlayInEditor))
	{
		return Fail(
			OutError,
			TEXT("target_asset_type_unsupported"),
			TEXT("Transient and PIE Blueprint packages cannot be persisted."),
			409);
	}

	const bool bLevelBlueprint = Blueprint->IsA<ULevelScriptBlueprint>();
	const bool bMapPackage = Package->ContainsMap();
	if (bLevelBlueprint != bMapPackage)
	{
		return Fail(
			OutError,
			TEXT("asset_package_type_mismatch"),
			TEXT("Blueprint class and package map flags disagree; saving was refused before SavePackage."));
	}

	OutTarget.Blueprint = Blueprint;
	OutTarget.Package = Package;
	OutTarget.PackageName = PackageName;
	if (!bLevelBlueprint)
	{
		OutTarget.Kind = EBlueprintPackageKind::Asset;
		OutTarget.TopLevelObject = Blueprint;
		OutTarget.Filename = FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
		return true;
	}

	ULevel* Level = Blueprint->GetTypedOuter<ULevel>();
	UWorld* World = Level ? Level->GetWorld() : nullptr;
	if (!Level || !World || World->PersistentLevel != Level
		|| World->GetOutermost() != Package)
	{
		return Fail(
			OutError,
			TEXT("map_world_unavailable"),
			TEXT("The LevelScriptBlueprint is not owned by the persistent level of its map package."),
			409);
	}
	if (World->WorldType != EWorldType::Editor)
	{
		return Fail(
			OutError,
			TEXT("target_asset_type_unsupported"),
			TEXT("Only an Editor world LevelScriptBlueprint can be persisted."),
			409);
	}

	OutTarget.Kind = EBlueprintPackageKind::Map;
	OutTarget.TopLevelObject = World;
	OutTarget.World = World;
	OutTarget.Filename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetMapPackageExtension());
	return true;
}

bool SaveBlueprintPackage(
	UBlueprint* Blueprint,
	FBlueprintPersistenceTarget* OutTarget,
	FBlueprintPersistenceError& OutError)
{
	FBlueprintPersistenceTarget Target;
	if (!ResolveBlueprintPersistenceTarget(Blueprint, Target, OutError))
	{
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (!UPackage::SavePackage(
			Target.Package,
			Target.TopLevelObject,
			*Target.Filename,
			SaveArgs))
	{
		return Fail(
			OutError,
			TEXT("asset_save_failed"),
			FString::Printf(
				TEXT("Failed to save %s package '%s'."),
				BlueprintPackageKindName(Target.Kind),
				*Target.PackageName),
			500);
	}
	if (!FPaths::FileExists(Target.Filename) || Target.Package->IsDirty())
	{
		return Fail(
			OutError,
			TEXT("post_save_verification_failed"),
			TEXT("SavePackage returned successfully, but the package file or dirty-state verification failed."),
			500);
	}
	if (OutTarget)
	{
		*OutTarget = MoveTemp(Target);
	}
	return true;
}

bool CompileAndSaveBlueprintPackage(
	UBlueprint* Blueprint,
	FBlueprintPersistenceTarget* OutTarget,
	FBlueprintPersistenceError& OutError)
{
	if (!Blueprint || !IsValid(Blueprint))
	{
		return Fail(
			OutError,
			TEXT("target_asset_type_unsupported"),
			TEXT("A valid Blueprint is required."));
	}
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint,
		EBlueprintCompileOptions::SkipSave);
	if (Blueprint->Status == BS_Error)
	{
		return Fail(
			OutError,
			TEXT("asset_compile_failed"),
			TEXT("Blueprint compilation failed; the package was not saved."),
			500);
	}
	return SaveBlueprintPackage(Blueprint, OutTarget, OutError);
}
}
