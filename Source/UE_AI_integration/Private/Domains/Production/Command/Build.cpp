// Build / Package / Lighting tools for UE_AI_integration
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorBuildUtils.h"
#include "Engine/World.h"
#include "NavigationSystem.h"

// ─────────────────────────────────────────────────────────────
// build_lighting
// ─────────────────────────────────────────────────────────────
class FTool_BuildLighting : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.build.lighting");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Quality = Params->HasField(TEXT("quality")) ? Params->GetStringField(TEXT("quality")).ToLower() : TEXT("preview");

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Error(TEXT("No editor world available."));
		}

		// Set lighting quality via config
		int32 QualityLevel = 0; // Preview
		if (Quality == TEXT("medium")) QualityLevel = 1;
		else if (Quality == TEXT("high")) QualityLevel = 2;
		else if (Quality == TEXT("production")) QualityLevel = 3;

		// Use EditorBuildUtils to build lighting
		FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("lighting_build_initiated"));
		Result->SetStringField(TEXT("quality"), Quality);
		Result->SetNumberField(TEXT("quality_level"), QualityLevel);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// build_navigation_only
// ─────────────────────────────────────────────────────────────
class FTool_BuildNavigationOnly : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.build.navigation");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Error(TEXT("No editor world available."));
		}

		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!NavSys)
		{
			return FMCPToolResult::Error(TEXT("Navigation system not available."));
		}

		NavSys->Build();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("navigation_build_complete"));
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// get_build_status
// ─────────────────────────────────────────────────────────────
class FTool_GetBuildStatus : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.build.status");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Check if lighting build is running
		bool bLightingInProgress = GEditor ? GEditor->IsLightingBuildCurrentlyRunning() : false;
		Result->SetBoolField(TEXT("lighting_build_active"), bLightingInProgress);

		// Check for any build (map, lighting, etc.)
		bool bAnyBuildInProgress = FEditorBuildUtils::IsBuildCurrentlyRunning();
		Result->SetBoolField(TEXT("map_build_active"), bAnyBuildInProgress);
		Result->SetBoolField(TEXT("any_build_active"), bLightingInProgress || bAnyBuildInProgress);

		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────
namespace UEAIIntegrationTools
{
	void RegisterBuildTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_BuildLighting>());
		Registry.Register(MakeShared<FTool_BuildNavigationOnly>());
		Registry.Register(MakeShared<FTool_GetBuildStatus>());
		// Cook, package, and commandlet retain their public capability IDs but
		// are registered by RegisterProductionRuntimeTools so they share the
		// durable job/artifact runtime.
	}
}
