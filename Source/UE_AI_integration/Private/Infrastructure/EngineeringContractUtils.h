#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAIIntegration::Infrastructure
{
/** Serialize a JSON value with recursively sorted object keys for deterministic hashing. */
FString CanonicalizeJsonValue(const TSharedPtr<FJsonValue>& Value);

/** Return a lowercase SHA-256 digest of canonical JSON, or an empty string on failure. */
FString DigestJson(const TSharedPtr<FJsonObject>& Object);

/** Build a stable, namespaced identifier from ordered identity components. */
FString MakeStableId(const FString& Prefix, const TArray<FString>& Components);

/**
 * Construct the public ue.finding.v1 shape used by static analysis domains.
 * Evidence is intentionally bounded by the caller before it reaches this helper.
 */
TSharedRef<FJsonObject> MakeFinding(
	const FString& RuleId,
	const FString& Severity,
	double Confidence,
	const FString& AssetPath,
	const FString& GraphName,
	const FString& NodeGuid,
	const FString& Message,
	const TSharedPtr<FJsonObject>& Evidence = nullptr);

/** Add a bounded array plus total/count/truncated metadata to an object. */
void SetBoundedArray(
	const TSharedRef<FJsonObject>& Target,
	const FString& Field,
	const TArray<TSharedPtr<FJsonValue>>& Values,
	int32 Total,
	int32 Limit);
}
