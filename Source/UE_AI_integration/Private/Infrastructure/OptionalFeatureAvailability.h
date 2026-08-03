#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAIIntegration::Infrastructure
{
/** True only when an optional feature pack was included by UBT. */
bool IsOptionalFeatureCompiled(const FString& FeatureName);

/**
 * Evaluate compile-time feature, plugin, module, platform, and engine
 * requirements declared by a capability descriptor.
 */
TArray<FString> GetCapabilityUnavailableReasons(
	const TSharedPtr<FJsonObject>& Descriptor);
}
