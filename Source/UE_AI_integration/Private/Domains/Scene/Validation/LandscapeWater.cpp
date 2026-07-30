#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/LandscapeWaterService.h"

namespace UEAISceneLandscapeWaterValidationPrivate
{
using UEAIIntegration::Infrastructure::FLandscapeWaterService;

class FTool_LandscapeValidate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ValidateLandscape(Params);
	}
};

class FTool_LandscapeChangePlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.change.plan"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().PlanChange(Params);
	}
};

class FTool_WaterValidate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.water.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ValidateWater(Params);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneLandscapeWaterValidationTools(FMCPToolRegistry& Registry)
{
	using namespace UEAISceneLandscapeWaterValidationPrivate;
	Registry.Register(MakeShared<FTool_LandscapeValidate>());
	Registry.Register(MakeShared<FTool_LandscapeChangePlan>());
	Registry.Register(MakeShared<FTool_WaterValidate>());
}
}
