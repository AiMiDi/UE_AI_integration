// Contract coverage for bounded Simulation Stage query and reversible authoring.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "Infrastructure/NiagaraGraphNotifications.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectHash.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraSimulationStageTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace UEAINiagaraSimulationStagePrivate
{
void SetUpdatePropertyFailureForTests(bool bEnabled);
}

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
	// Keep the fixture independent of optional Niagara default modules.  The
	// simulation-stage contract only needs a valid system graph and an emitter;
	// loading RequiredSystemUpdate from the engine content package makes an
	// isolated HostProject fail before the stage authoring path is exercised.
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, false);
	UNiagaraScript* SystemSpawnScript = OutFixture.System->GetSystemSpawnScript();
	UNiagaraScript* SystemUpdateScript = OutFixture.System->GetSystemUpdateScript();
	UNiagaraScriptSource* SystemSource = SystemSpawnScript
		? Cast<UNiagaraScriptSource>(SystemSpawnScript->GetLatestSource())
		: nullptr;
	if (!SystemSpawnScript || !SystemUpdateScript || !SystemSource || !SystemSource->NodeGraph
		|| !FNiagaraStackGraphUtilities::ResetGraphForOutput(
			*SystemSource->NodeGraph,
			ENiagaraScriptUsage::SystemSpawnScript,
			SystemSpawnScript->GetUsageId())
		|| !FNiagaraStackGraphUtilities::ResetGraphForOutput(
			*SystemSource->NodeGraph,
			ENiagaraScriptUsage::SystemUpdateScript,
			SystemUpdateScript->GetUsageId()))
	{
		return false;
	}
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

bool DeleteNiagaraSimulationStageFixture(FNiagaraSimulationStageFixture& Fixture)
{
	if (Fixture.Package)
	{
		Fixture.Package->SetDirtyFlag(false);
	}
	if (Fixture.System)
	{
		// This fixture is intentionally never saved.  Deleting an unsaved
		// NiagaraSystem through the editor asset subsystem invokes ForceDeleteObjects,
		// which can reject the package while VersionedNiagaraScriptData still holds
		// an internal Source reference.  Remove the transient asset from the
		// registry and let normal GC release its subobjects instead.
		TArray<UObject*> PackageObjects;
		GetObjectsWithOuter(Fixture.System->GetOutermost(), PackageObjects, true);
		for (UObject* Object : PackageObjects)
		{
			if (Object)
			{
				FAssetRegistryModule::AssetDeleted(Object);
			}
		}
		FAssetRegistryModule::AssetDeleted(Fixture.System);
		Fixture.System->ClearFlags(RF_Public | RF_Standalone);
		Fixture.System->Rename(
			*FString::Printf(TEXT("UEAI_TransientNiagaraSystem_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)),
			GetTransientPackage(),
			REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
		Fixture.System->MarkAsGarbage();
		Fixture.System = nullptr;
	}
	Fixture.Emitter = nullptr;
	Fixture.Package = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	return !FPackageName::DoesPackageExist(Fixture.PackageName)
		&& !UEditorAssetLibrary::DoesAssetExist(Fixture.PackageName);
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

UNiagaraGraph* FixtureStageGraph(const FNiagaraSimulationStageFixture& Fixture)
{
	FVersionedNiagaraEmitterData* Data = Fixture.Emitter->GetEmitterData(Fixture.EmitterVersion);
	UNiagaraScriptSource* Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
	return Source ? Source->NodeGraph : nullptr;
}

UEdGraphPin* FirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	if (Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
	}
	return nullptr;
}

FString ExportStageGraph(UNiagaraGraph* Graph)
{
	TSet<UObject*> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Nodes.Add(Node);
	}
	FString Export;
	FEdGraphUtilities::ExportNodesToText(Nodes, Export);
	return Export;
}

FMCPToolResult ApplyStageOperation(FMCPToolRegistry& Registry, const TCHAR* Action,
	const FNiagaraSimulationStageFixture& Fixture, const FGuid& UsageId)
{
	TSharedPtr<FJsonObject> Params = BaseStageParams(Fixture, UsageId);
	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.niagara.simulation_stage.") + FString(Action) + TEXT(".plan"), Params);
	if (!Plan.bSuccess || !Plan.Data)
	{
		return Plan;
	}
	Params->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetBoolField(TEXT("confirmWrite"), true);
	Params->SetStringField(TEXT("approvePlanDigest"), Plan.Data->GetStringField(TEXT("planDigest")));
	return Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.") + FString(Action) + TEXT(".apply"), Params);
}

FMCPToolResult RollbackStageRemoval(FMCPToolRegistry& Registry, const FMCPToolResult& Removed)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("rollbackId"), Removed.Data->GetStringField(TEXT("receiptId")));
	Params->SetStringField(TEXT("requestId"), Removed.Data->GetStringField(TEXT("requestId")));
	Params->SetBoolField(TEXT("confirmWrite"), true);
	return Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.remove.rollback"), Params);
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
		TestTrue(TEXT("Simulation-stage fixture and package are deleted"), DeleteNiagaraSimulationStageFixture(Fixture));
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
	UpdateParams->SetNumberField(TEXT("numIterations"), 4);
	UpdateParams->SetStringField(TEXT("numIterationsBinding"), TEXT(""));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetStringField(TEXT("iterationSource"), TEXT("DirectSet"));
	Properties->SetStringField(TEXT("executeBehavior"), TEXT("OnSimulationReset"));
	Properties->SetStringField(TEXT("directDispatchType"), TEXT("TwoD"));
	Properties->SetBoolField(TEXT("disablePartialParticleUpdate"), true);
	UpdateParams->SetObjectField(TEXT("properties"), Properties);
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
	if (!TestTrue(TEXT("Simulation-stage update apply succeeds"), Updated.bSuccess) || !Updated.Data)
	{
		return false;
	}
	const FString UpdateReceipt = Updated.Data->GetStringField(TEXT("receiptId"));
	TestTrue(TEXT("Simulation-stage update returns a receipt"), !UpdateReceipt.IsEmpty());
	if (Updated.Data)
	{
		TestEqual(TEXT("Updated stage exposes its iteration count"), Updated.Data->GetIntegerField(TEXT("numIterations")), 4);
		const TSharedPtr<FJsonObject> ReadBackProperties = Updated.Data->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("Updated stage exposes its iteration source"), ReadBackProperties->GetStringField(TEXT("iterationSource")), FString(TEXT("DirectSet")));
		TestEqual(TEXT("Updated stage exposes its execute behavior"), ReadBackProperties->GetStringField(TEXT("executeBehavior")), FString(TEXT("OnSimulationReset")));
		TestEqual(TEXT("Updated stage exposes its dispatch dimensions"), ReadBackProperties->GetStringField(TEXT("directDispatchType")), FString(TEXT("TwoD")));
	}

	TSharedPtr<FJsonObject> UnsupportedProperties = MakeShared<FJsonObject>();
	UnsupportedProperties->SetNumberField(TEXT("unsupportedField"), 1);
	TSharedPtr<FJsonObject> UnsupportedParams = BaseStageParams(Fixture, UsageId);
	UnsupportedParams->SetObjectField(TEXT("properties"), UnsupportedProperties);
	const FMCPToolResult UnsupportedUpdate = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.plan"), UnsupportedParams);
	TestFalse(TEXT("Simulation-stage update rejects unsupported properties"), UnsupportedUpdate.bSuccess);
	TestEqual(TEXT("Unsupported property uses a stable error code"), UnsupportedUpdate.ErrorCode, FString(TEXT("property_unsupported")));

	TSharedPtr<FJsonObject> SecondUpdateParams = BaseStageParams(Fixture, UsageId);
	SecondUpdateParams->SetStringField(TEXT("name"), TEXT("SecondStage"));
	SecondUpdateParams->SetNumberField(TEXT("numIterations"), 7);
	const FMCPToolResult SecondUpdatePlan = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.plan"), SecondUpdateParams);
	if (!TestTrue(TEXT("Second simulation-stage update plan succeeds"), SecondUpdatePlan.bSuccess) || !SecondUpdatePlan.Data)
	{
		return false;
	}
	const FString SecondUpdateRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	SecondUpdateParams->SetStringField(TEXT("requestId"), SecondUpdateRequestId);
	SecondUpdateParams->SetBoolField(TEXT("confirmWrite"), true);
	SecondUpdateParams->SetStringField(TEXT("approvePlanDigest"), SecondUpdatePlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult SecondUpdated = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.apply"), SecondUpdateParams);
	if (!TestTrue(TEXT("Second simulation-stage update apply succeeds"), SecondUpdated.bSuccess) || !SecondUpdated.Data)
	{
		return false;
	}
	TSharedPtr<FJsonObject> UpdateRollbackParams = MakeShared<FJsonObject>();
	UpdateRollbackParams->SetStringField(TEXT("rollbackId"), SecondUpdated.Data->GetStringField(TEXT("receiptId")));
	UpdateRollbackParams->SetStringField(TEXT("requestId"), SecondUpdateRequestId);
	UpdateRollbackParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult UpdateRolledBack = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.rollback"), UpdateRollbackParams);
	if (!TestTrue(TEXT("Simulation-stage update rollback succeeds"), UpdateRolledBack.bSuccess))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AfterUpdateRollbackListParams = BaseStageParams(Fixture, UsageId);
	const FMCPToolResult AfterUpdateRollback = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.list"), AfterUpdateRollbackListParams);
	if (AfterUpdateRollback.bSuccess)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (AfterUpdateRollback.Data->TryGetArrayField(TEXT("simulationStages"), Rows) && Rows && Rows->Num() == 1)
		{
			TestEqual(TEXT("Update rollback restores the previous stage name"), (*Rows)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("UpdatedStage")));
			TestEqual(TEXT("Update rollback restores the previous iteration count"), (*Rows)[0]->AsObject()->GetIntegerField(TEXT("numIterations")), 4);
		}
	}

	// Fail after the complete requested property set was applied. The failed
	// operation must restore the native name, enabled state, reflected fields,
	// iteration binding/value and original package dirty state together.
	ON_SCOPE_EXIT { UEAINiagaraSimulationStagePrivate::SetUpdatePropertyFailureForTests(false); };
	const bool bOriginalDirty = Fixture.Package->IsDirty();
	for (bool bDirtyBaseline : {false, true})
	{
		Fixture.Package->SetDirtyFlag(bDirtyBaseline);
		const FMCPToolResult Before = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.list"), BaseStageParams(Fixture, UsageId));
		FString BeforeDigest;
		if (!TestTrue(TEXT("Stage failure-recovery baseline is readable"), Before.bSuccess && Before.Data
			&& UEAIIntegration::Infrastructure::TryDigestJson(Before.Data, BeforeDigest))) return false;
		UNiagaraGraph* BeforeGraph = FixtureStageGraph(Fixture);
		if (!TestNotNull(TEXT("Stage failure-recovery graph is available"), BeforeGraph)) return false;
		const TArray<UEdGraphNode*> BeforeNodes = BeforeGraph->Nodes;
		const FString BeforeGraphExport = ExportStageGraph(BeforeGraph);
		auto FailedParams = BaseStageParams(Fixture, UsageId);
		FailedParams->SetStringField(TEXT("name"), TEXT("FailedStage"));
		FailedParams->SetBoolField(TEXT("enabled"), false);
		FailedParams->SetNumberField(TEXT("numIterations"), 9);
		FailedParams->SetStringField(TEXT("numIterationsBinding"), TEXT(""));
		auto FailedProperties = MakeShared<FJsonObject>();
		FailedProperties->SetStringField(TEXT("directDispatchType"), TEXT("ThreeD"));
		FailedProperties->SetBoolField(TEXT("disablePartialParticleUpdate"), false);
		FailedParams->SetObjectField(TEXT("properties"), FailedProperties);
		const FMCPToolResult FailedPlan = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.plan"), FailedParams);
		if (!TestTrue(TEXT("Stage failure-recovery update plans"), FailedPlan.bSuccess && FailedPlan.Data)) return false;
		FailedParams->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		FailedParams->SetBoolField(TEXT("confirmWrite"), true);
		FailedParams->SetStringField(TEXT("approvePlanDigest"), FailedPlan.Data->GetStringField(TEXT("planDigest")));
		UEAINiagaraSimulationStagePrivate::SetUpdatePropertyFailureForTests(true);
		const FMCPToolResult Failed = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.apply"), FailedParams);
		TestFalse(TEXT("Stage post-property failure is returned"), Failed.bSuccess);
		TestEqual(TEXT("Stage property failure preserves its original error code"), Failed.ErrorCode, FString(TEXT("property_invalid")));
		TestTrue(TEXT("Stage property failure verifies complete restoration"), Failed.Data
			&& Failed.Data->GetBoolField(TEXT("restorationVerified")));
		TestEqual(TEXT("Stage property failure preserves the original dirty state"), Fixture.Package->IsDirty(), bDirtyBaseline);
		const FMCPToolResult After = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.list"), BaseStageParams(Fixture, UsageId));
		FString AfterDigest;
		TestTrue(TEXT("Stage failure restores all exposed properties and identities"), After.bSuccess && After.Data
			&& UEAIIntegration::Infrastructure::TryDigestJson(After.Data, AfterDigest) && BeforeDigest == AfterDigest);
		TestTrue(TEXT("Stage property failure preserves every graph node identity"), TArray<UEdGraphNode*>(BeforeGraph->Nodes) == BeforeNodes);
		TestEqual(TEXT("Stage property failure preserves the exact graph state"), ExportStageGraph(BeforeGraph), BeforeGraphExport);
	}
	Fixture.Package->SetDirtyFlag(bOriginalDirty);

	const FMCPToolResult InvalidUpdate = Registry.ExecuteTool(TEXT("content.niagara.simulation_stage.update.plan"), BaseStageParams(Fixture, FGuid::NewGuid()));
	TestFalse(TEXT("Simulation-stage update rejects an unknown stable identity"), InvalidUpdate.bSuccess);
	TestEqual(TEXT("Unknown stage uses a stable error code"), InvalidUpdate.ErrorCode, FString(TEXT("stage_not_found")));

	UNiagaraGraph* Graph = FixtureStageGraph(Fixture);
	if (!TestNotNull(TEXT("Simulation-stage fixture graph"), Graph))
	{
		return false;
	}
	UNiagaraNodeOutput* StageOutput = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId);
	UEdGraphPin* StageInput = FirstPin(StageOutput, EGPD_Input);
	if (!TestNotNull(TEXT("Simulation-stage parameter-map input"), StageInput)
		|| !TestEqual(TEXT("The stage has one exclusive upstream input"), StageInput->LinkedTo.Num(), 1))
	{
		return false;
	}
	UEdGraphNode* ExclusiveInput = StageInput->LinkedTo[0]->GetOwningNode();
	const TArray<UEdGraphNode*> OriginalNodes = Graph->Nodes;
	const FString OriginalGraphExport = ExportStageGraph(Graph);
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
	TestFalse(TEXT("Removal deletes its output"), Graph->Nodes.Contains(StageOutput));
	TestFalse(TEXT("Removal deletes exclusive upstream nodes"), Graph->Nodes.Contains(ExclusiveInput));
	TestEqual(TEXT("Unrelated emitter graph nodes survive removal"), Graph->Nodes.Num(), OriginalNodes.Num() - 2);

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
	TestTrue(TEXT("Rollback verifies the complete original graph"), RolledBack.Data->GetBoolField(TEXT("fullGraphRestored")));
	TestEqual(TEXT("Rollback restores all authored properties and connections"), ExportStageGraph(Graph), OriginalGraphExport);
	TestTrue(TEXT("Rollback retains the original output identity"), Graph->Nodes.Contains(StageOutput));
	TestTrue(TEXT("Rollback retains the original upstream identity"), Graph->Nodes.Contains(ExclusiveInput));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSimulationStageSharedGraphRecoveryTest,
	"UE_AI_integration.Niagara.SimulationStage.SharedGraphRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSimulationStageSharedGraphRecoveryTest::RunTest(const FString&)
{
	FNiagaraSimulationStageFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(TEXT("Shared stage fixture is deleted"), DeleteNiagaraSimulationStageFixture(Fixture));
	};
	if (!TestTrue(TEXT("Shared stage fixture builds"), CreateNiagaraSimulationStageFixture(Fixture)))
	{
		return false;
	}
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSimulationStageTools(Registry);
	Registry.EndDomainRegistration();
	const FGuid FirstUsage = FGuid::NewGuid();
	const FGuid OtherUsage = FGuid::NewGuid();
	const FMCPToolResult FirstAdded = ApplyStageOperation(Registry, TEXT("add"), Fixture, FirstUsage);
	const FMCPToolResult OtherAdded = ApplyStageOperation(Registry, TEXT("add"), Fixture, OtherUsage);
	if (!TestTrue(TEXT("First stage is added"), FirstAdded.bSuccess)
		|| !TestTrue(TEXT("Other stage is added"), OtherAdded.bSuccess))
	{
		return false;
	}
	UNiagaraGraph* Graph = FixtureStageGraph(Fixture);
	if (!TestNotNull(TEXT("Shared stage graph"), Graph))
	{
		return false;
	}
	UNiagaraNodeOutput* FirstOutput = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, FirstUsage);
	UNiagaraNodeOutput* OtherOutput = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, OtherUsage);
	UEdGraphPin* FirstInput = FirstPin(FirstOutput, EGPD_Input);
	UEdGraphPin* OtherInput = FirstPin(OtherOutput, EGPD_Input);
	if (!TestNotNull(TEXT("First stage output input"), FirstInput)
		|| !TestNotNull(TEXT("Other stage output input"), OtherInput)
		|| !TestEqual(TEXT("Other stage has one map input"), OtherInput->LinkedTo.Num(), 1))
	{
		return false;
	}
	UEdGraphPin* SharedMap = OtherInput->LinkedTo[0];
	UEdGraphNode* SharedNode = SharedMap->GetOwningNode();
	// Instantiate the native reroute through reflection: its private Niagara
	// header exposes non-exported methods, while UNiagaraNode's virtual API is public.
	TestTrue(TEXT("NiagaraEditor module is loaded for native reroute registration"),
		FModuleManager::Get().LoadModule(TEXT("NiagaraEditor")) != nullptr);
	UClass* RerouteClass = LoadObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeReroute"));
	if (!TestNotNull(TEXT("Native reroute class"), RerouteClass))
	{
		return false;
	}
	UNiagaraNode* ExclusiveBranch = NewObject<UNiagaraNode>(Graph, RerouteClass, NAME_None, RF_Transactional);
	Graph->AddNode(ExclusiveBranch, false, false);
	ExclusiveBranch->CreateNewGuid();
	ExclusiveBranch->AllocateDefaultPins();
	UEdGraphPin* BranchInput = FirstPin(ExclusiveBranch, EGPD_Input);
	UEdGraphPin* BranchOutput = FirstPin(ExclusiveBranch, EGPD_Output);
	if (!TestNotNull(TEXT("Exclusive branch input"), BranchInput)
		|| !TestNotNull(TEXT("Exclusive branch output"), BranchOutput))
	{
		return false;
	}
	BranchInput->PinType = SharedMap->PinType;
	BranchOutput->PinType = SharedMap->PinType;
	FirstInput->BreakAllPinLinks();
	BranchInput->MakeLinkTo(SharedMap);
	BranchOutput->MakeLinkTo(FirstInput);
	UEAIIntegration::NiagaraEditing::NotifyRestoredGraph(Graph);
	const FString BeforeExport = ExportStageGraph(Graph);
	const int32 BeforeNodeCount = Graph->Nodes.Num();
	const FMCPToolResult Removed = ApplyStageOperation(Registry, TEXT("remove"), Fixture, FirstUsage);
	if (!TestTrue(TEXT("Stage with a shared upstream branch is removed"), Removed.bSuccess) || !Removed.Data)
	{
		return false;
	}
	TestFalse(TEXT("Removed output leaves the graph"), Graph->Nodes.Contains(FirstOutput));
	TestFalse(TEXT("Exclusive upstream branch leaves the graph"), Graph->Nodes.Contains(ExclusiveBranch));
	TestTrue(TEXT("Shared upstream dependency remains"), Graph->Nodes.Contains(SharedNode));
	TestTrue(TEXT("The other output remains"), Graph->Nodes.Contains(OtherOutput));
	TestTrue(TEXT("The other output preserves its map link"), OtherInput->LinkedTo.Contains(SharedMap));
	TestEqual(TEXT("Only exclusive nodes are removed"), Graph->Nodes.Num(), BeforeNodeCount - 2);
	const FMCPToolResult Restored = RollbackStageRemoval(Registry, Removed);
	if (!TestTrue(TEXT("Shared graph removal rollback succeeds"), Restored.bSuccess) || !Restored.Data)
	{
		return false;
	}
	TestTrue(TEXT("Shared graph has complete restoration proof"), Restored.Data->GetBoolField(TEXT("fullGraphRestored")));
	TestEqual(TEXT("Every graph node property and link is restored"), ExportStageGraph(Graph), BeforeExport);
	TestTrue(TEXT("Original exclusive node identity is restored"), Graph->Nodes.Contains(ExclusiveBranch));
	TestTrue(TEXT("Original shared node identity is preserved"), Graph->Nodes.Contains(SharedNode));

	const FMCPToolResult RemovedAgain = ApplyStageOperation(Registry, TEXT("remove"), Fixture, FirstUsage);
	if (!TestTrue(TEXT("Restored stage can be removed again"), RemovedAgain.bSuccess) || !RemovedAgain.Data)
	{
		return false;
	}
	const FString RemovedGraphExport = ExportStageGraph(Graph);
	// Change a detached, receipt-retained node's pin set. The current emitter
	// graph digest is unchanged, so rollback reaches the retained-pin safety gate.
	ExclusiveBranch->CreatePin(EGPD_Input, BranchInput->PinType, TEXT("ReconstructedPin"));
	const FMCPToolResult FailedRestore = RollbackStageRemoval(Registry, RemovedAgain);
	TestFalse(TEXT("A changed retained pin set cannot claim successful restoration"), FailedRestore.bSuccess);
	TestEqual(TEXT("Failed full restoration has a stable error"), FailedRestore.ErrorCode,
		FString(TEXT("rollback_verification_failed")));
	TestEqual(TEXT("Failed restoration preserves the current complete graph"), ExportStageGraph(Graph), RemovedGraphExport);
	TestTrue(TEXT("Failed restoration preserves the other output"), Graph->Nodes.Contains(OtherOutput));
	TestTrue(TEXT("Failed restoration preserves the other output's link"), OtherInput->LinkedTo.Contains(SharedMap));
	TestFalse(TEXT("Failed restoration never synthesizes an empty output"), Graph->Nodes.Contains(FirstOutput));
	TestEqual(TEXT("Failed restoration keeps the removed stage absent"),
		Fixture.Emitter->GetEmitterData(Fixture.EmitterVersion)->GetSimulationStages().Num(), 1);
	TestTrue(TEXT("Failed restoration retains Editor Undo"), FailedRestore.ErrorMessage.Contains(TEXT("Editor Undo")));
	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
