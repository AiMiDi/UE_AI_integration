// Contract coverage for bounded Simulation Stage query and reversible authoring.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraSimulationStageTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FNiagaraSimulationStageFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	UNiagaraEmitter* Emitter = nullptr;
	FGuid EmitterVersion;
	FGuid EmitterHandleId;
};

bool CreateNiagaraSimulationStageFixture(FNiagaraSimulationStageFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_SimulationStage_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(OutFixture.PackageName);
	OutFixture.Package = CreatePackage(*OutFixture.PackageName);
	OutFixture.System = OutFixture.Package
		? NewObject<UNiagaraSystem>(OutFixture.Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!OutFixture.System)
	{
		return false;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);
	OutFixture.Emitter = NewObject<UNiagaraEmitter>(OutFixture.System, TEXT("SimulationEmitter"), RF_Transactional);
	if (!OutFixture.Emitter)
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(OutFixture.Emitter, false);
	OutFixture.EmitterVersion = OutFixture.Emitter->GetExposedVersion().VersionGuid;
	FNiagaraEmitterHandle Handle(*OutFixture.Emitter, OutFixture.EmitterVersion);
	OutFixture.EmitterHandleId = Handle.GetId();
	OutFixture.System->AddEmitterHandleDirect(Handle);
	OutFixture.Package->SetDirtyFlag(false);
	return true;
}

bool DeleteNiagaraSimulationStageFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted && !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

TSharedPtr<FJsonObject> BaseStageParams(const FNiagaraSimulationStageFixture& Fixture, const FGuid& UsageId)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetStringField(TEXT("name"), TEXT("InitialStage"));
	Params->SetBoolField(TEXT("enabled"), true);
	return Params;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSimulationStageAuthoringContractTest,
	"UE_AI_integration.Niagara.SimulationStage.Authoring.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSimulationStageAuthoringContractTest::RunTest(const FString&)
{
	FNiagaraSimulationStageFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(TEXT("Simulation-stage fixture and package are deleted"), DeleteNiagaraSimulationStageFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Simulation-stage fixture builds"), CreateNiagaraSimulationStageFixture(Fixture)))
	{
		AddInfo(TEXT("The /Game/ simulation-stage fixture could not be built; skipping the contract."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSimulationStageTools(Registry);
	Registry.EndDomainRegistration();

	const FGuid UsageId = FGuid::NewGuid();
	TSharedPtr<FJsonObject> Params = BaseStageParams(Fixture, UsageId);
	const FMCPToolResult InvalidList = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.list"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Simulation-stage list rejects a missing system"), InvalidList.bSuccess);
	TestEqual(TEXT("Missing system uses a stable error code"), InvalidList.ErrorCode, FString(TEXT("system_required")));

	const FMCPToolResult AddPlan = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.add.plan"), Params);
	if (!TestTrue(TEXT("Simulation-stage add plan succeeds"), AddPlan.bSuccess) || !AddPlan.Data)
	{
		return false;
	}
	const FString AddDigest = AddPlan.Data->GetStringField(TEXT("planDigest"));
	Params->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetBoolField(TEXT("confirmWrite"), true);
	Params->SetStringField(TEXT("approvePlanDigest"), AddDigest);
	const FMCPToolResult Added = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.add.apply"), Params);
	if (!TestTrue(TEXT("Simulation-stage add apply succeeds"), Added.bSuccess) || !Added.Data)
	{
		return false;
	}
	const FString AddReceipt = Added.Data->GetStringField(TEXT("receiptId"));
	TestTrue(TEXT("Simulation-stage add returns a receipt"), !AddReceipt.IsEmpty());

	TSharedPtr<FJsonObject> UpdateParams = BaseStageParams(Fixture, UsageId);
	UpdateParams->SetStringField(TEXT("name"), TEXT("UpdatedStage"));
	UpdateParams->RemoveField(TEXT("enabled"));
	const FMCPToolResult UpdatePlan = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.plan"), UpdateParams);
	if (!TestTrue(TEXT("Simulation-stage update plan succeeds"), UpdatePlan.bSuccess) || !UpdatePlan.Data)
	{
		return false;
	}
	TestEqual(TEXT("Update plan preserves the omitted enabled option"), UpdatePlan.Data->GetObjectField(TEXT("after"))->GetBoolField(TEXT("enabled")), true);
	const FString UpdateRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	UpdateParams->SetStringField(TEXT("requestId"), UpdateRequestId);
	UpdateParams->SetBoolField(TEXT("confirmWrite"), true);
	UpdateParams->SetStringField(TEXT("approvePlanDigest"), UpdatePlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult Updated = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.apply"), UpdateParams);
	TestTrue(TEXT("Simulation-stage update apply succeeds"), Updated.bSuccess);

	const FMCPToolResult InvalidUpdate = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.plan"), BaseStageParams(Fixture, FGuid::NewGuid()));
	TestFalse(TEXT("Simulation-stage update rejects an unknown stable identity"), InvalidUpdate.bSuccess);
	TestEqual(TEXT("Unknown stage uses a stable error code"), InvalidUpdate.ErrorCode, FString(TEXT("stage_not_found")));

	TSharedPtr<FJsonObject> RemoveParams = BaseStageParams(Fixture, UsageId);
	const FMCPToolResult RemovePlan = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.remove.plan"), RemoveParams);
	if (!TestTrue(TEXT("Simulation-stage remove plan succeeds"), RemovePlan.bSuccess) || !RemovePlan.Data)
	{
		return false;
	}
	const FString RemoveRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	RemoveParams->SetStringField(TEXT("requestId"), RemoveRequestId);
	RemoveParams->SetBoolField(TEXT("confirmWrite"), true);
	RemoveParams->SetStringField(TEXT("approvePlanDigest"), RemovePlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult Removed = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.remove.apply"), RemoveParams);
	if (!TestTrue(TEXT("Simulation-stage remove apply succeeds"), Removed.bSuccess) || !Removed.Data)
	{
		return false;
	}
	const FString RemoveReceipt = Removed.Data->GetStringField(TEXT("receiptId"));
	TestTrue(TEXT("Simulation-stage remove returns a receipt"), !RemoveReceipt.IsEmpty());

	TSharedPtr<FJsonObject> ListParams = BaseStageParams(Fixture, UsageId);
	const FMCPToolResult EmptyList = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.list"), ListParams);
	TestTrue(TEXT("Simulation-stage list succeeds after removal"), EmptyList.bSuccess);
	TestEqual(TEXT("Removed simulation-stage list is empty"), EmptyList.Data->GetIntegerField(TEXT("total")), 0);

	TSharedPtr<FJsonObject> RollbackParams = MakeShared<FJsonObject>();
	RollbackParams->SetStringField(TEXT("rollbackId"), RemoveReceipt);
	RollbackParams->SetStringField(TEXT("requestId"), RemoveRequestId);
	RollbackParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult RolledBack = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.remove.rollback"), RollbackParams);
	if (!RolledBack.bSuccess)
	{
		AddError(FString::Printf(TEXT("Simulation-stage removal rollback returned %s: %s"), *RolledBack.ErrorCode, *RolledBack.ErrorMessage));
	}
	if (!TestTrue(TEXT("Simulation-stage removal rollback succeeds"), RolledBack.bSuccess))
	{
		return false;
	}
	const FMCPToolResult RestoredList = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.list"), ListParams);
	TestTrue(TEXT("Simulation-stage list succeeds after rollback"), RestoredList.bSuccess);
	TestEqual(TEXT("Rollback restores one simulation stage"), RestoredList.Data->GetIntegerField(TEXT("total")), 1);
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (RestoredList.Data->TryGetArrayField(TEXT("simulationStages"), Rows) && Rows && Rows->Num() == 1)
	{
		TestEqual(TEXT("Rollback restores the edited stage name"), (*Rows)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("UpdatedStage")));
		TestTrue(TEXT("Rollback restores a graph output"), (*Rows)[0]->AsObject()->GetBoolField(TEXT("graphPresent")));
	}
	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
