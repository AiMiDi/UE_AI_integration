#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UPackage;
class UWorld;

namespace UEAIIntegration::Infrastructure
{
enum class EBlueprintPackageKind : uint8
{
	Asset,
	Map,
};

struct FBlueprintPersistenceError
{
	FString Code;
	FString Message;
	int32 HttpStatus = 422;

	void Reset()
	{
		Code.Reset();
		Message.Reset();
		HttpStatus = 422;
	}
};

/**
 * Separates the logical Blueprint being edited from the top-level object that
 * owns the package on disk. For a LevelScriptBlueprint those objects are the
 * LevelScriptBlueprint and its editor UWorld respectively.
 */
struct FBlueprintPersistenceTarget
{
	UBlueprint* Blueprint = nullptr;
	UPackage* Package = nullptr;
	UObject* TopLevelObject = nullptr;
	UWorld* World = nullptr;
	FString PackageName;
	FString Filename;
	EBlueprintPackageKind Kind = EBlueprintPackageKind::Asset;

	bool IsLevelBlueprint() const
	{
		return Kind == EBlueprintPackageKind::Map;
	}
};

const TCHAR* BlueprintPackageKindName(EBlueprintPackageKind Kind);

bool ResolveBlueprintPersistenceTarget(
	UBlueprint* Blueprint,
	FBlueprintPersistenceTarget& OutTarget,
	FBlueprintPersistenceError& OutError);

/** Save only. Compilation is deliberately a separate lifecycle operation. */
bool SaveBlueprintPackage(
	UBlueprint* Blueprint,
	FBlueprintPersistenceTarget* OutTarget,
	FBlueprintPersistenceError& OutError);

/** Compatibility helper for single-request commands that compile then save. */
bool CompileAndSaveBlueprintPackage(
	UBlueprint* Blueprint,
	FBlueprintPersistenceTarget* OutTarget,
	FBlueprintPersistenceError& OutError);
}
