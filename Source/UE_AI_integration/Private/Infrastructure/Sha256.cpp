#include "Infrastructure/Sha256.h"

#if WITH_SSL
#include <openssl/sha.h>
#endif

namespace UEAIIntegration::Infrastructure
{
bool TrySha256Hex(const void* Data, const uint64 Size, FString& OutHex)
{
	OutHex.Reset();
#if WITH_SSL
	if (Size > static_cast<uint64>(TNumericLimits<SIZE_T>::Max()))
	{
		return false;
	}
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	const uint8 Empty = 0;
	const void* Input = Size == 0 ? &Empty : Data;
	if (!Input
		|| !::SHA256(
			static_cast<const unsigned char*>(Input),
			static_cast<size_t>(Size),
			Digest))
	{
		return false;
	}
	OutHex = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	return OutHex.Len() == SHA256_DIGEST_LENGTH * 2;
#else
	return false;
#endif
}
}
