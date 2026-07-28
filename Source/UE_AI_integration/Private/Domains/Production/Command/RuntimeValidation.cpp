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

class FEngineeringOperationTool : public FProductionControllerTool
{
public:
	using FProductionControllerTool::FProductionControllerTool;

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		return Controller.ExecuteProductionJobOperation(
			GetCapabilityId(),
			Params);
	}
};

class FJobStatusTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.job.status"); }
};
class FJobCancelTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.job.cancel"); }
};
class FJobResultTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.job.result.get"); }
};
class FJobLogTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.job.log.get"); }
};
class FJobArtifactTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.job.artifact.get"); }
};
class FTraceStartTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.start"); }
};
class FTraceStatusTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.status"); }
};
class FTraceStopTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.stop"); }
};
class FTraceAnalyzeTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.analyze"); }
};
class FPerformanceRunTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.run"); }
};
class FPerformanceResultTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.result.get"); }
};
class FPerformanceCompareTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.compare"); }
};
class FTestListTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.test.list"); }
};
class FTestRunTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.test.run"); }
};
class FTestResultTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.test.result.get"); }
};
class FCookProjectJobTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.project.cook"); }
};
class FPackageProjectJobTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.project.package"); }
};
class FCommandletJobTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.commandlet.run"); }
};
class FSourceRepositoryTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.repository.get"); }
};
class FSourceStatusTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.status"); }
};
class FSourceDiffTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.diff"); }
};
class FSourcePlanTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.change.plan"); }
};
class FSourceExecuteTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.change.execute"); }
};
class FDdcStatusTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.ddc.status"); }
};
class FDdcStartTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.ddc.job.start"); }
};
class FBuildGraphValidateTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.buildgraph.validate"); }
};
class FBuildGraphRunTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.buildgraph.run"); }
};
class FHordeContextTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.horde.context.get"); }
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

	Registry.Register(MakeShared<FJobStatusTool>(Controller));
	Registry.Register(MakeShared<FJobCancelTool>(Controller));
	Registry.Register(MakeShared<FJobResultTool>(Controller));
	Registry.Register(MakeShared<FJobLogTool>(Controller));
	Registry.Register(MakeShared<FJobArtifactTool>(Controller));
	Registry.Register(MakeShared<FTraceStartTool>(Controller));
	Registry.Register(MakeShared<FTraceStatusTool>(Controller));
	Registry.Register(MakeShared<FTraceStopTool>(Controller));
	Registry.Register(MakeShared<FTraceAnalyzeTool>(Controller));
	Registry.Register(MakeShared<FPerformanceRunTool>(Controller));
	Registry.Register(MakeShared<FPerformanceResultTool>(Controller));
	Registry.Register(MakeShared<FPerformanceCompareTool>(Controller));
	Registry.Register(MakeShared<FTestListTool>(Controller));
	Registry.Register(MakeShared<FTestRunTool>(Controller));
	Registry.Register(MakeShared<FTestResultTool>(Controller));
	Registry.Register(MakeShared<FCookProjectJobTool>(Controller));
	Registry.Register(MakeShared<FPackageProjectJobTool>(Controller));
	Registry.Register(MakeShared<FCommandletJobTool>(Controller));
	Registry.Register(MakeShared<FSourceRepositoryTool>(Controller));
	Registry.Register(MakeShared<FSourceStatusTool>(Controller));
	Registry.Register(MakeShared<FSourceDiffTool>(Controller));
	Registry.Register(MakeShared<FSourcePlanTool>(Controller));
	Registry.Register(MakeShared<FSourceExecuteTool>(Controller));
	Registry.Register(MakeShared<FDdcStatusTool>(Controller));
	Registry.Register(MakeShared<FDdcStartTool>(Controller));
	Registry.Register(MakeShared<FBuildGraphValidateTool>(Controller));
	Registry.Register(MakeShared<FBuildGraphRunTool>(Controller));
	Registry.Register(MakeShared<FHordeContextTool>(Controller));

}
}
