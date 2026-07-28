#pragma once

#include "CoreMinimal.h"
#include "Core/MCPExecutionTypes.h"
#include "Dom/JsonObject.h"

namespace UEAIIntegration
{
namespace Core
{
struct FCapabilityCatalogData
{
	TArray<FMCPCapabilityDescriptor> Descriptors;
	TMap<FString, TSharedPtr<FJsonObject>> InputSchemas;
	TMap<FString, int32> DomainCounts;
};

/** Load and structurally validate all six capability manifests. */
bool LoadCapabilityCatalog(
	const FString& Directory,
	FCapabilityCatalogData& OutCatalog,
	TArray<FString>& OutErrors);

/** Validate a parameter object against the supported JSON Schema subset used by the manifests. */
bool ValidateCapabilityParams(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutErrors);

/** Dotted public IDs use lowercase alphanumeric or underscore segments. */
bool IsDottedCapabilityId(const FString& Id);
}
}
