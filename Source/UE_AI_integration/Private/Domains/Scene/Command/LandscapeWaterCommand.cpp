#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/LandscapeWaterService.h"

namespace UEAISceneLandscapeWaterCommandPrivate
{
using UEAIIntegration::Infrastructure::FLandscapeWaterService;

class FTool_LandscapeChangeExecute final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.change.execute"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().ExecuteChange(Params);
	}
};

class FTool_LandscapeChangeRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.landscape.change.rollback"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FLandscapeWaterService::Get().RollbackChange(Params);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneLandscapeWaterCommandTools(FMCPToolRegistry& Registry)
{
	using namespace UEAISceneLandscapeWaterCommandPrivate;
	Registry.Register(MakeShared<FTool_LandscapeChangeExecute>());
	Registry.Register(MakeShared<FTool_LandscapeChangeRollback>());
}
}
