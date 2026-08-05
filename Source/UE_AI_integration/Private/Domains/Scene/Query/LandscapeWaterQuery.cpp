#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/LandscapeWaterService.h"

namespace UEAISceneLandscapeWaterQueryPrivate
{
using UEAIIntegration::Infrastructure::FLandscapeWaterService;

class FTool_LandscapeList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ListLandscapes(Params);
	}
};

class FTool_LandscapeGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().GetLandscape(Params);
	}
};

class FTool_LandscapeLayersGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.layers.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().GetLandscapeLayers(Params);
	}
};

class FTool_LandscapeSnapshot final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.snapshot"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().SnapshotLandscape(Params);
	}
};

class FTool_LandscapeDiff final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.diff"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().DiffLandscapeSnapshots(Params);
	}
};

class FTool_LandscapeHeightmapExport final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.heightmap.export"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ExportHeightmap(Params);
	}
};

class FTool_LandscapeWeightmapExport final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.weightmap.export"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ExportWeightmap(Params);
	}
};

class FTool_WaterList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.water.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ListWater(Params);
	}
};

class FTool_WaterGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.water.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().GetWater(Params);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneLandscapeWaterQueryTools(FMCPToolRegistry& Registry)
{
	using namespace UEAISceneLandscapeWaterQueryPrivate;
	Registry.Register(MakeShared<FTool_LandscapeList>());
	Registry.Register(MakeShared<FTool_LandscapeGet>());
	Registry.Register(MakeShared<FTool_LandscapeLayersGet>());
	Registry.Register(MakeShared<FTool_LandscapeSnapshot>());
	Registry.Register(MakeShared<FTool_LandscapeDiff>());
	Registry.Register(MakeShared<FTool_LandscapeHeightmapExport>());
	Registry.Register(MakeShared<FTool_LandscapeWeightmapExport>());
	Registry.Register(MakeShared<FTool_WaterList>());
	Registry.Register(MakeShared<FTool_WaterGet>());
}
}
