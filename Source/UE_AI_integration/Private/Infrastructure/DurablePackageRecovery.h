#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAIIntegration::Infrastructure
{
struct FDurablePackageRecoveryRequest
{
	FString ChangeSetId;
	FString Kind;
	FString RequestId;
	FString PlanDigest;
	FString ContractDigest;
	FString BeforeHash;
	TArray<FString> PackageNames;
	TArray<FString> ExternalSourceFiles;
};

/**
 * Restart-durable, raw-package recovery for change-plan handlers.
 *
 * The service intentionally operates only on project package files and their
 * standard sidecars. Callers must Prepare before touching any UObject, then
 * Seal after all intended files have reached a stable on-disk state. Rollback
 * refuses to replace dirty loaded packages or files that no longer match the
 * sealed post-change hashes.
 */
class FDurablePackageRecovery
{
public:
	static bool Prepare(
		const FDurablePackageRecoveryRequest& Request,
		TSharedPtr<FJsonObject>& OutManifest,
		FString& OutErrorCode,
		FString& OutError);

	static bool Seal(
		const FString& ChangeSetId,
		const FString& Status,
		const FString& AfterHash,
		TSharedPtr<FJsonObject>& OutManifest,
		FString& OutErrorCode,
		FString& OutError);

	static bool Rollback(
		const FString& ChangeSetId,
		const FString& RequestId,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutErrorCode,
		FString& OutError,
		bool bAllowDirtyOwnedPackages = false);

	static FString ManifestPath(const FString& ChangeSetId);
	static bool IsValidChangeSetId(const FString& ChangeSetId);
};
}
