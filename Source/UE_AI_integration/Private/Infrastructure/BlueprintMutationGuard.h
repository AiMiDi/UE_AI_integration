#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

class UBlueprint;

namespace UEAIIntegration::Infrastructure
{
/**
 * Atomic protection for one direct Blueprint mutation request.
 *
 * Workflow requests retain their own multi-step transaction. This guard is for
 * the legacy direct command path: it snapshots the logical Blueprint graph,
 * package dirty state, and on-disk package before mutation, then restores all
 * three when compile, save, or read-back fails.
 */
class FBlueprintSingleRequestMutationGuard
{
public:
	explicit FBlueprintSingleRequestMutationGuard(UBlueprint* InBlueprint);
	~FBlueprintSingleRequestMutationGuard();

	bool IsValid() const { return bValid; }
	const FString& GetErrorCode() const { return ErrorCode; }
	const FString& GetErrorMessage() const { return ErrorMessage; }

	void MarkMutationStarted() { bMutationStarted = true; }
	void Commit();
	bool Rollback(FString& OutError);

private:
	struct FObjectSnapshot
	{
		explicit FObjectSnapshot(UObject* InObject)
			: Object(InObject)
		{
		}

		TStrongObjectPtr<UObject> Object;
		TArray<uint8> Bytes;
		int32 OuterDepth = 0;
	};

	void CleanupDiskBaseline();

	TStrongObjectPtr<UBlueprint> Blueprint;
	TArray<FObjectSnapshot> ObjectSnapshots;
	FString GraphDigestBefore;
	FString PackageFilename;
	FString DiskBaselineFilename;
	FString ErrorCode;
	FString ErrorMessage;
	bool bPackageWasDirty = false;
	bool bDiskFileExisted = false;
	bool bValid = false;
	bool bMutationStarted = false;
	bool bCommitted = false;
	bool bRolledBack = false;
};
}
