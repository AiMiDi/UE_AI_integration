#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Tools/MCPToolBase.h"

namespace UEAIIntegration::Infrastructure
{
/**
 * Game-thread-only Landscape and Water authoring service.
 *
 * Landscape raster writes are deliberately limited to complete, deterministic
 * little-endian raw imports. Water writes are limited to actors created through
 * the enabled Water editor factories, and destructive delete is admitted only
 * for actors tagged as managed by this service.
 */
class FLandscapeWaterService
{
public:
	struct FChangeRecord;

	static FLandscapeWaterService& Get();

	FMCPToolResult ListLandscapes(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetLandscape(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetLandscapeLayers(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ValidateLandscape(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult SnapshotLandscape(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult DiffLandscapeSnapshots(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ExportHeightmap(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ExportWeightmap(const TSharedPtr<FJsonObject>& Params) const;

	FMCPToolResult PlanChange(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ExecuteChange(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult RollbackChange(const TSharedPtr<FJsonObject>& Params);

	FMCPToolResult ListWater(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetWater(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult ValidateWater(const TSharedPtr<FJsonObject>& Params) const;

	/** Pure contract helper used by Automation tests. */
	static FMCPToolResult DiffSnapshotObjects(
		const TSharedPtr<FJsonObject>& Before,
		const TSharedPtr<FJsonObject>& After);

#if WITH_DEV_AUTOMATION_TESTS
	/**
	 * One-shot failure injection used only by the real Editor Automation lane.
	 * Supported values are afterFirstOperation and
	 * afterFirstOperationRollbackArtifact.
	 */
	void SetAutomationFailurePoint(FName FailurePoint);
#endif

private:
	FLandscapeWaterService() = default;
	~FLandscapeWaterService() = default;
	FLandscapeWaterService(const FLandscapeWaterService&) = delete;
	FLandscapeWaterService& operator=(const FLandscapeWaterService&) = delete;

	TMap<FString, TSharedPtr<FChangeRecord>> Runs;
	TMap<FString, FString> RequestRuns;

#if WITH_DEV_AUTOMATION_TESTS
	FName AutomationFailurePoint;
#endif
};
}
