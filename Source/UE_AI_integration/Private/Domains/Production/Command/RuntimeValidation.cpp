#include "Infrastructure/ProductionRuntimeController.h"
#include "Infrastructure/ClientActivityService.h"
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

class FSessionRosterTool final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("production.session.roster.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		auto* Service = UEAIIntegration::Infrastructure::FClientActivityService::GetActiveService();
		return Service
			? FMCPToolResult::Ok(Service->MakeSnapshot(20, 20))
			: FMCPToolResult::Error(TEXT("Client activity service is unavailable."), TEXT("session_service_unavailable"), 503);
	}
};

class FLeaseStatusTool final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("production.lease.status"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		auto* Service = UEAIIntegration::Infrastructure::FClientActivityService::GetActiveService();
		return Service
			? FMCPToolResult::Ok(Service->MakeLeaseSnapshot())
			: FMCPToolResult::Error(TEXT("Client activity service is unavailable."), TEXT("session_service_unavailable"), 503);
	}
};

class FLeaseAcquireTool final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("production.lease.acquire"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		auto* Service = UEAIIntegration::Infrastructure::FClientActivityService::GetActiveService();
		if (!Service) return FMCPToolResult::Error(TEXT("Client activity service is unavailable."), TEXT("session_service_unavailable"), 503);
		FString Type;
		FString SessionId;
		FString ApprovePlanDigest;
		bool bOverride = false;
		Params->TryGetStringField(TEXT("type"), Type);
		Params->TryGetStringField(TEXT("__callerSessionId"), SessionId);
		Params->TryGetStringField(TEXT("approvePlanDigest"), ApprovePlanDigest);
		Params->TryGetBoolField(TEXT("override"), bOverride);
		TSharedPtr<FJsonObject> Lease;
		FString Error;
		int32 HttpStatus = 200;
		if (!Service->AcquireLease(Type, SessionId, bOverride, ApprovePlanDigest, Lease, Error, HttpStatus))
		{
			FMCPToolResult Result = FMCPToolResult::Error(
				Error,
				HttpStatus == 409 ? TEXT("lease_conflict") : TEXT("lease_invalid"),
				HttpStatus);
			Result.Data = Lease;
			return Result;
		}
		return FMCPToolResult::Ok(Lease);
	}
};

class FLeaseReleaseTool final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("production.lease.release"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		auto* Service = UEAIIntegration::Infrastructure::FClientActivityService::GetActiveService();
		if (!Service) return FMCPToolResult::Error(TEXT("Client activity service is unavailable."), TEXT("session_service_unavailable"), 503);
		FString Type;
		FString SessionId;
		Params->TryGetStringField(TEXT("type"), Type);
		Params->TryGetStringField(TEXT("__callerSessionId"), SessionId);
		FString Error;
		if (!Service->ReleaseLease(Type, SessionId, Error))
		{
			return FMCPToolResult::Error(Error, TEXT("lease_not_owned"), 409);
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("released"), true);
		Result->SetStringField(TEXT("type"), Type);
		return FMCPToolResult::Ok(Result);
	}
};
class FTraceTargetListTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.target.list"); }
};
class FTraceLaunchPlanTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.launch.plan"); }
};
class FTraceChannelListTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.channel.list"); }
};
class FTraceImportTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.import"); }
};
class FTraceProviderListTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.provider.list"); }
};
class FTraceTimingQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.timing.query"); }
};
class FTraceCounterQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.counter.query"); }
};
class FTraceMemoryQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.memory.query"); }
};
class FTraceLoadingQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.loading.query"); }
};
class FTraceNetworkQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.network.query"); }
};
class FTraceTasksQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.tasks.query"); }
};
class FTraceContextSwitchesQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.context_switches.query"); }
};
class FTraceLogQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.log.query"); }
};
class FTraceIoQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.io.query"); }
};
class FTraceBookmarkQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.bookmark.query"); }
};
class FTraceRegionQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.region.query"); }
};
class FTraceScreenshotQueryTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.screenshot.query"); }
};
class FTraceExportTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.export"); }
};
class FTraceOpenInsightsTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.trace.open_in_insights"); }
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
class FPerformanceDiagnoseTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.diagnose"); }
};
class FPerformanceReportGenerateTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.report.generate"); }
};
class FPerformanceSuiteListTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.suite.list"); }
};
class FPerformanceSuiteValidateTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.suite.validate"); }
};
class FPerformanceSuiteRunTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.suite.run"); }
};
class FPerformanceSuiteResultTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.suite.result.get"); }
};
class FPerformanceBaselinePromoteTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.performance.baseline.promote"); }
};
class FRecoveryListTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.recovery.list"); }
};
class FRecoveryGetTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.recovery.get"); }
};
class FRecoveryCleanupTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.recovery.cleanup"); }
};
class FReflectionTypeSearchTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.reflection.type.search"); }
};
class FReflectionTypeGetTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.reflection.type.get"); }
};
class FReflectionMemberGetTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.reflection.member.get"); }
};
class FReflectionObjectDescribeTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.reflection.object.describe"); }
};
class FReflectionSnapshotCreateTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.reflection.snapshot.create"); }
};
class FPythonInspectTool final : public FEngineeringOperationTool
{
public: using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.python.inspect"); }
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
class FSourceProviderTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.provider.get"); }
};
class FSourceRefreshTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.status.refresh"); }
};
class FSourceCheckoutTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.checkout"); }
};
class FSourceRevertUnchangedTool final : public FEngineeringOperationTool
{
public:
	using FEngineeringOperationTool::FEngineeringOperationTool;
	FString GetCapabilityId() const override { return TEXT("production.source_control.revert_unchanged"); }
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
	Registry.Register(MakeShared<FSessionRosterTool>());
	Registry.Register(MakeShared<FLeaseStatusTool>());
	Registry.Register(MakeShared<FLeaseAcquireTool>());
	Registry.Register(MakeShared<FLeaseReleaseTool>());
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
	Registry.Register(MakeShared<FTraceTargetListTool>(Controller));
	Registry.Register(MakeShared<FTraceLaunchPlanTool>(Controller));
	Registry.Register(MakeShared<FTraceChannelListTool>(Controller));
	Registry.Register(MakeShared<FTraceImportTool>(Controller));
	Registry.Register(MakeShared<FTraceProviderListTool>(Controller));
	Registry.Register(MakeShared<FTraceTimingQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceCounterQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceMemoryQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceLoadingQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceNetworkQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceTasksQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceContextSwitchesQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceLogQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceIoQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceBookmarkQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceRegionQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceScreenshotQueryTool>(Controller));
	Registry.Register(MakeShared<FTraceExportTool>(Controller));
	Registry.Register(MakeShared<FTraceOpenInsightsTool>(Controller));
	Registry.Register(MakeShared<FPerformanceRunTool>(Controller));
	Registry.Register(MakeShared<FPerformanceResultTool>(Controller));
	Registry.Register(MakeShared<FPerformanceCompareTool>(Controller));
	Registry.Register(MakeShared<FPerformanceDiagnoseTool>(Controller));
	Registry.Register(MakeShared<FPerformanceReportGenerateTool>(Controller));
	Registry.Register(MakeShared<FPerformanceSuiteListTool>(Controller));
	Registry.Register(MakeShared<FPerformanceSuiteValidateTool>(Controller));
	Registry.Register(MakeShared<FPerformanceSuiteRunTool>(Controller));
	Registry.Register(MakeShared<FPerformanceSuiteResultTool>(Controller));
	Registry.Register(MakeShared<FPerformanceBaselinePromoteTool>(Controller));
	Registry.Register(MakeShared<FRecoveryListTool>(Controller));
	Registry.Register(MakeShared<FRecoveryGetTool>(Controller));
	Registry.Register(MakeShared<FRecoveryCleanupTool>(Controller));
	Registry.Register(MakeShared<FReflectionTypeSearchTool>(Controller));
	Registry.Register(MakeShared<FReflectionTypeGetTool>(Controller));
	Registry.Register(MakeShared<FReflectionMemberGetTool>(Controller));
	Registry.Register(MakeShared<FReflectionObjectDescribeTool>(Controller));
	Registry.Register(MakeShared<FReflectionSnapshotCreateTool>(Controller));
	Registry.Register(MakeShared<FPythonInspectTool>(Controller));
	Registry.Register(MakeShared<FTestListTool>(Controller));
	Registry.Register(MakeShared<FTestRunTool>(Controller));
	Registry.Register(MakeShared<FTestResultTool>(Controller));
	Registry.Register(MakeShared<FCookProjectJobTool>(Controller));
	Registry.Register(MakeShared<FPackageProjectJobTool>(Controller));
	Registry.Register(MakeShared<FCommandletJobTool>(Controller));
	Registry.Register(MakeShared<FSourceRepositoryTool>(Controller));
	Registry.Register(MakeShared<FSourceStatusTool>(Controller));
	Registry.Register(MakeShared<FSourceDiffTool>(Controller));
	Registry.Register(MakeShared<FSourceProviderTool>(Controller));
	Registry.Register(MakeShared<FSourceRefreshTool>(Controller));
	Registry.Register(MakeShared<FSourceCheckoutTool>(Controller));
	Registry.Register(MakeShared<FSourceRevertUnchangedTool>(Controller));
	Registry.Register(MakeShared<FSourcePlanTool>(Controller));
	Registry.Register(MakeShared<FSourceExecuteTool>(Controller));
	Registry.Register(MakeShared<FDdcStatusTool>(Controller));
	Registry.Register(MakeShared<FDdcStartTool>(Controller));
	Registry.Register(MakeShared<FBuildGraphValidateTool>(Controller));
	Registry.Register(MakeShared<FBuildGraphRunTool>(Controller));
	Registry.Register(MakeShared<FHordeContextTool>(Controller));

}
}
