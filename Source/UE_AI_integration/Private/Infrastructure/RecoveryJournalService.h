#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

namespace UEAIIntegration::Infrastructure
{
/** Bounded, metadata-only view over durable recovery records. */
class FRecoveryJournalService
{
public:
	bool Handles(const FString& CapabilityId) const;
	FMCPToolResult Execute(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params) const;

private:
	FMCPToolResult List(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult Get(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult Cleanup(const TSharedPtr<FJsonObject>& Params) const;
	static bool IsSafeId(const FString& Value);
	static TSharedPtr<FJsonObject> ReadRecord(const FString& Path);
	static TSharedPtr<FJsonObject> Summarize(
		const FString& Path,
		const TSharedPtr<FJsonObject>& Record);
};
}
