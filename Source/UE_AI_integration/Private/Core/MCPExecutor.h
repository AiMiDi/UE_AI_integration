#pragma once

#include "CoreMinimal.h"
#include "Core/MCPExecutionTypes.h"

class FMCPToolRegistry;

/** Core execution boundary between HTTP transport and domain handlers. */
class FMCPExecutor
{
public:
	explicit FMCPExecutor(FMCPToolRegistry& InRegistry);

	FMCPResult Execute(const FMCPExecutionContext& Context);

private:
	struct FIdempotencyRecord
	{
		FString PayloadKey;
		FMCPResult Result;
		FDateTime LastAccessUtc;
	};

	FMCPResult ExecuteUncached(const FMCPExecutionContext& Context) const;
	static FString MakePayloadKey(const FMCPExecutionContext& Context);
	static TSharedPtr<FJsonObject> MakeValidationDetails(const TArray<FString>& Errors);
	void PruneIdempotencyCache(const FDateTime& NowUtc);

	FMCPToolRegistry& Registry;
	TMap<FString, FIdempotencyRecord> IdempotencyCache;
};
