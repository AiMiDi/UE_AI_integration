#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAIIntegration::Infrastructure
{
/** Produces a stable JSON representation with lexicographically sorted object keys. */
FString CanonicalizeJson(const TSharedPtr<FJsonObject>& Object);

/** Computes the lowercase SHA-256 digest used by domain change plans and snapshots. */
bool TryDigestJson(const TSharedPtr<FJsonObject>& Object, FString& OutDigest);

/**
 * Validates the common write gate. The caller must re-plan first and pass the
 * current digest; this deliberately has no dependency on the Workflow planner.
 */
bool ValidateChangeApproval(
	const TSharedPtr<FJsonObject>& Params,
	const FString& CurrentDigest,
	FString& OutErrorCode,
	FString& OutErrorMessage);
}
