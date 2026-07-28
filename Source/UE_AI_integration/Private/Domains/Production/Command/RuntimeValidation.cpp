#include "Infrastructure/ProductionRuntimeController.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
class FProductionControllerTool : public FMCPToolBase
{
public:
	explicit FProductionControllerTool(
		UEAIIntegration::Infrastructure::FProductionRuntimeController& InController)
		: Controller(InController)
	{
	}

protected:
	UEAIIntegration::Infrastructure::FProductionRuntimeController& Controller;
};

class FScenarioValidateTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.scenario.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.ValidateScenario(Params);
	}
};

class FScenarioStartTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.scenario.start"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.StartScenario(Params);
	}
};

class FScenarioStatusTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.scenario.status"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.GetScenarioStatus(Params);
	}
};

class FScenarioCancelTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.scenario.cancel"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.CancelScenario(Params);
	}
};

class FScenarioResultTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.scenario.result.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.GetScenarioResult(Params);
	}
};

class FScenarioArtifactTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.scenario.artifact.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.GetScenarioArtifact(Params);
	}
};

class FModuleLoadedTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.module.loaded.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.GetLoadedModule(Params);
	}
};

class FBuildTargetTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.build.target"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.StartBuild(Params);
	}
};

class FBuildJobTool final : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;
	FString GetCapabilityId() const override { return TEXT("production.build.job.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.GetBuildJob(Params);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterProductionRuntimeTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FProductionRuntimeController& Controller)
{
	Registry.Register(MakeShared<FScenarioValidateTool>(Controller));
	Registry.Register(MakeShared<FScenarioStartTool>(Controller));
	Registry.Register(MakeShared<FScenarioStatusTool>(Controller));
	Registry.Register(MakeShared<FScenarioCancelTool>(Controller));
	Registry.Register(MakeShared<FScenarioResultTool>(Controller));
	Registry.Register(MakeShared<FScenarioArtifactTool>(Controller));
	Registry.Register(MakeShared<FModuleLoadedTool>(Controller));
	Registry.Register(MakeShared<FBuildTargetTool>(Controller));
	Registry.Register(MakeShared<FBuildJobTool>(Controller));
}
}
