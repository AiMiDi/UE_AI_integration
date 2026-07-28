#pragma once

#include "CoreMinimal.h"

namespace UEAIIntegration::Infrastructure
{
/** Cross-version SHA-256 that does not depend on platform misc implementations. */
bool TrySha256Hex(const void* Data, uint64 Size, FString& OutHex);

inline bool TrySha256Hex(const TArray<uint8>& Data, FString& OutHex)
{
	return TrySha256Hex(Data.GetData(), static_cast<uint64>(Data.Num()), OutHex);
}
}
