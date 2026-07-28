#pragma once

#include "CoreMinimal.h"
#include "Misc/EngineVersion.h"
#include "Runtime/Launch/Resources/Version.h"

// Keep engine-version branching at the infrastructure boundary. Domain
// implementations should call compatibility helpers instead of testing engine
// versions directly.
#ifndef UE_VERSION_OLDER_THAN
#define UE_VERSION_OLDER_THAN(MajorVersion, MinorVersion, PatchVersion) \
	((ENGINE_MAJOR_VERSION < (MajorVersion)) || \
	 (ENGINE_MAJOR_VERSION == (MajorVersion) && ENGINE_MINOR_VERSION < (MinorVersion)) || \
	 (ENGINE_MAJOR_VERSION == (MajorVersion) && ENGINE_MINOR_VERSION == (MinorVersion) && ENGINE_PATCH_VERSION < (PatchVersion)))
#endif

#ifndef UE_VERSION_NEWER_THAN
#define UE_VERSION_NEWER_THAN(MajorVersion, MinorVersion, PatchVersion) \
	((ENGINE_MAJOR_VERSION > (MajorVersion)) || \
	 (ENGINE_MAJOR_VERSION == (MajorVersion) && ENGINE_MINOR_VERSION > (MinorVersion)) || \
	 (ENGINE_MAJOR_VERSION == (MajorVersion) && ENGINE_MINOR_VERSION == (MinorVersion) && ENGINE_PATCH_VERSION > (PatchVersion)))
#endif

#ifndef UE_VERSION_AT_LEAST
#define UE_VERSION_AT_LEAST(MajorVersion, MinorVersion, PatchVersion) \
	(!UE_VERSION_OLDER_THAN(MajorVersion, MinorVersion, PatchVersion))
#endif

namespace UEAIIntegration::Compatibility
{
	static_assert(ENGINE_MAJOR_VERSION == 5, "UE_AI_integration supports Unreal Engine 5 only.");
	static_assert(ENGINE_MINOR_VERSION >= 3 && ENGINE_MINOR_VERSION <= 7,
		"UE_AI_integration supports Unreal Engine 5.3 through 5.7.");

	inline FString GetEngineVersion()
	{
		return FEngineVersion::Current().ToString();
	}
}
