#pragma once

#include "Infrastructure/Compatibility/UEVersionCompat.h"

#if WITH_UEAI_PCG_GRAPH
class FJsonObject;
class FJsonValue;
class UPCGSettings;

namespace UEAIIntegrationTools::PCGGraphSettings
{
bool Read(const UPCGSettings& Settings, TSharedPtr<FJsonObject>& Values, int32& RemainingChars);
bool Validate(const FString& ClassPath, const UPCGSettings* Instance, const TArray<TSharedPtr<FJsonValue>>& Changes, FString& Message);
bool Apply(UPCGSettings& Settings, const TArray<TSharedPtr<FJsonValue>>& Changes, TArray<FString>& EditedPaths, FString& Message);
bool RefreshAfterRestore(UPCGSettings& Settings, const TArray<FString>& EditedPaths);
}
#endif
