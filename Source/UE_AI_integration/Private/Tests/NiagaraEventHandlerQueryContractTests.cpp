// Contract coverage for content.niagara.event_handler.list.
//
// The query is read-only and bounded.  This test builds a minimal /Game
// Niagara system with one authored event-handler entry, then verifies the
// stable identity/options envelope and page controls without saving or
// requiring runtime event delivery.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraEventHandlerTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace UEAINiagaraEventHandlerPrivate
{
void SetUpdateFailureForTests(bool bCompileFailure, bool bReadbackFailure);
}

namespace
{
struct FNiagaraEventHandlerQueryFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	UNiagaraEmitter* Emitter = nullptr;
	FGuid EmitterVersion;
	FGuid EmitterHandleId;
	FGuid UsageId;
};

bool CreateNiagaraEventHandlerQueryFixture(
	FNiagaraEventHandlerQueryFixture& OutFixture,
	const bool bAddEventHandler = true)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_EventHandlerQuery_")
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
	// Keep the fixture independent of the optional RequiredSystemUpdate module;
	// only the emitter graph and event-handler output are under test.
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
	OutFixture.Emitter = NewObject<UNiagaraEmitter>(OutFixture.System, TEXT("EventEmitter"), RF_Transactional);
	if (!OutFixture.Emitter)
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(OutFixture.Emitter, false);
	OutFixture.EmitterVersion = OutFixture.Emitter->GetExposedVersion().VersionGuid;
	FNiagaraEmitterHandle Handle(*OutFixture.Emitter, OutFixture.EmitterVersion);
	OutFixture.EmitterHandleId = Handle.GetId();
	OutFixture.System->AddEmitterHandleDirect(Handle);

	if (!bAddEventHandler)
	{
		OutFixture.Package->SetDirtyFlag(false);
		return true;
	}

	OutFixture.UsageId = FGuid::NewGuid();
	FNiagaraEventScriptProperties Properties;
	Properties.ExecutionMode = EScriptExecutionMode::SpawnedParticles;
	Properties.SpawnNumber = 4;
	Properties.MinSpawnNumber = 2;
	Properties.MaxEventsPerFrame = 32;
	Properties.bRandomSpawnNumber = true;
	Properties.UpdateAttributeInitialValues = false;
	Properties.SourceEventName = FName(TEXT("Burst"));
	Properties.Script = NewObject<UNiagaraScript>(OutFixture.Emitter, TEXT("EventScript"), RF_Transactional);
	if (!Properties.Script)
	{
		return false;
	}
	Properties.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	Properties.Script->SetUsageId(OutFixture.UsageId);
	FVersionedNiagaraEmitterData* EmitterData = OutFixture.Emitter->GetEmitterData(OutFixture.EmitterVersion);
	Properties.Script->SetLatestSource(EmitterData ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource) : nullptr);
	OutFixture.Emitter->AddEventHandler(Properties, OutFixture.EmitterVersion);
	OutFixture.Package->SetDirtyFlag(false);
	return true;
}

bool DeleteNiagaraEventHandlerQueryFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted && !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraEventHandlerQueryContractTest,
	"UE_AI_integration.Niagara.EventHandlerQuery.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEventHandlerQueryContractTest::RunTest(const FString&)
{
	FNiagaraEventHandlerQueryFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Event-handler query fixture and package are deleted"),
			DeleteNiagaraEventHandlerQueryFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Event-handler query fixture builds"), CreateNiagaraEventHandlerQueryFixture(Fixture)))
	{
		AddInfo(TEXT("The /Game/ event-handler fixture could not be built; skipping the query contract."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraEventHandlerTools(Registry);
	Registry.EndDomainRegistration();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetNumberField(TEXT("offset"), 0);
	Params->SetNumberField(TEXT("limit"), 1);
	const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), Params);
	if (!TestTrue(TEXT("Event-handler list succeeds"), Result.bSuccess) || !Result.Data)
	{
		return false;
	}

	TestEqual(
		TEXT("Event-handler list schema is stable"),
		Result.Data->GetStringField(TEXT("schema")),
		FString(TEXT("ue.niagara.event-handlers.v1")));
	TestEqual(TEXT("Event-handler total is one"), Result.Data->GetIntegerField(TEXT("total")), 1);
	TestEqual(TEXT("Event-handler page offset is zero"), Result.Data->GetIntegerField(TEXT("offset")), 0);
	TestEqual(TEXT("Event-handler page limit is one"), Result.Data->GetIntegerField(TEXT("limit")), 1);
	TestFalse(TEXT("Single event-handler page has no continuation"), Result.Data->GetBoolField(TEXT("hasMore")));
	TestEqual(TEXT("Event-handler next offset is one"), Result.Data->GetIntegerField(TEXT("nextOffset")), 1);

	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!TestTrue(
		TEXT("Event-handler list contains a bounded row"),
		Result.Data->TryGetArrayField(TEXT("eventHandlers"), Rows) && Rows != nullptr && Rows->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
	TestEqual(
		TEXT("Event-handler usage ID is stable"),
		Row->GetStringField(TEXT("usageId")),
		Fixture.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	TestEqual(TEXT("Event-handler execution mode is read back"), Row->GetStringField(TEXT("executionMode")), FString(TEXT("SpawnedParticles")));
	TestEqual(TEXT("Event-handler spawn count is read back"), Row->GetIntegerField(TEXT("spawnNumber")), 4);
	TestEqual(TEXT("Event-handler min spawn count is read back"), Row->GetIntegerField(TEXT("minSpawnNumber")), 2);
	TestEqual(TEXT("Event-handler max events is read back"), Row->GetIntegerField(TEXT("maxEventsPerFrame")), 32);
	TestTrue(TEXT("Event-handler random spawn option is read back"), Row->GetBoolField(TEXT("randomSpawnNumber")));
	TestFalse(TEXT("Event-handler initial values option is read back"), Row->GetBoolField(TEXT("updateAttributeInitialValues")));
	TestFalse(TEXT("Handler without an output reports missing graph"), Row->GetBoolField(TEXT("graphPresent")));
	TestFalse(TEXT("Read-only event-handler query leaves the package clean"), Fixture.Package->IsDirty());

	Params->SetNumberField(TEXT("offset"), 1);
	const FMCPToolResult EmptyPage = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), Params);
	TestTrue(TEXT("Offset at total produces an empty page"), EmptyPage.bSuccess);
	const TArray<TSharedPtr<FJsonValue>>* EmptyRows = nullptr;
	TestTrue(
		TEXT("Empty page has no event-handler rows"),
		EmptyPage.Data && EmptyPage.Data->TryGetArrayField(TEXT("eventHandlers"), EmptyRows)
			&& EmptyRows && EmptyRows->IsEmpty());

	Params->SetNumberField(TEXT("limit"), 0);
	const FMCPToolResult InvalidPage = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), Params);
	TestFalse(TEXT("Zero page limit is rejected"), InvalidPage.bSuccess);
	TestEqual(TEXT("Invalid page uses a stable error code"), InvalidPage.ErrorCode, FString(TEXT("page_invalid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraEventHandlerAddApplyContractTest,
	"UE_AI_integration.Niagara.EventHandlerQuery.AddApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEventHandlerAddApplyContractTest::RunTest(const FString&)
{
	FNiagaraEventHandlerQueryFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Event-handler apply fixture and package are deleted"),
			DeleteNiagaraEventHandlerQueryFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Event-handler apply fixture builds"), CreateNiagaraEventHandlerQueryFixture(Fixture, false)))
	{
		AddInfo(TEXT("The /Game/ event-handler fixture could not be built; skipping add/apply contract."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraEventHandlerTools(Registry);
	Registry.EndDomainRegistration();

	const FGuid HandleId = Fixture.System->GetEmitterHandles()[0].GetId();
	const FGuid UsageId = FGuid::NewGuid();
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), HandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetStringField(TEXT("sourceEmitterId"), TEXT("00000000-0000-0000-0000-000000000000"));
	const FMCPToolResult InvalidSourceEmitter = Registry.ExecuteTool(
		TEXT("content.niagara.event_handler.add.plan"), Params);
	TestFalse(TEXT("Event-handler add rejects the zero source-emitter GUID"), InvalidSourceEmitter.bSuccess);
	TestEqual(TEXT("Zero source-emitter GUID uses a stable error code"),
		InvalidSourceEmitter.ErrorCode, FString(TEXT("source_emitter_id_invalid")));
	Params->SetStringField(TEXT("sourceEmitterId"), TEXT(""));
	Params->SetStringField(TEXT("sourceEventName"), TEXT("Burst"));
	Params->SetStringField(TEXT("executionMode"), TEXT("SpawnedParticles"));
	Params->SetNumberField(TEXT("spawnNumber"), 4);
	Params->SetNumberField(TEXT("minSpawnNumber"), 2);
	Params->SetNumberField(TEXT("maxEventsPerFrame"), 32);
	Params->SetBoolField(TEXT("randomSpawnNumber"), true);
	Params->SetBoolField(TEXT("updateAttributeInitialValues"), false);

	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.niagara.event_handler.add.plan"), Params);
	if (!TestTrue(TEXT("Event-handler add plan succeeds"), Plan.bSuccess) || !Plan.Data)
	{
		return false;
	}
	const FString PlanDigest = Plan.Data->GetStringField(TEXT("planDigest"));
	TestTrue(TEXT("Event-handler add plan returns a digest"), !PlanDigest.IsEmpty());
	const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Params->SetStringField(TEXT("requestId"), RequestId);
	Params->SetBoolField(TEXT("confirmWrite"), true);
	Params->SetStringField(TEXT("approvePlanDigest"), PlanDigest);

	const FMCPToolResult Applied = Registry.ExecuteTool(
		TEXT("content.niagara.event_handler.add.apply"), Params);
	if (!TestTrue(TEXT("Event-handler add apply succeeds"), Applied.bSuccess) || !Applied.Data)
	{
		return false;
	}
	TestTrue(TEXT("Event-handler add apply is verified"), Applied.Data->GetBoolField(TEXT("verified")));
	const FString ReceiptId = Applied.Data->GetStringField(TEXT("receiptId"));
	TestTrue(TEXT("Event-handler add apply returns a receipt"), !ReceiptId.IsEmpty());

	const FMCPToolResult Listed = Registry.ExecuteTool(
		TEXT("content.niagara.event_handler.list"), Params);
	if (!TestTrue(TEXT("Event-handler list reads the applied handler"), Listed.bSuccess) || !Listed.Data)
	{
		return false;
	}
	TestEqual(TEXT("Applied event-handler list total is one"), Listed.Data->GetIntegerField(TEXT("total")), 1);
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	bool bAppliedGraphPresent = false;
	if (Listed.Data->TryGetArrayField(TEXT("eventHandlers"), Rows) && Rows != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Rows)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			if (Row.IsValid()
				&& Row->GetStringField(TEXT("usageId"))
					== UsageId.ToString(EGuidFormats::DigitsWithHyphensLower))
			{
				bAppliedGraphPresent = Row->GetBoolField(TEXT("graphPresent"));
				break;
			}
		}
	}
	TestTrue(TEXT("Applied event-handler list has a graph-backed row"), bAppliedGraphPresent);

	auto Rollback = MakeShared<FJsonObject>();
	Rollback->SetStringField(TEXT("rollbackId"), ReceiptId);
	Rollback->SetStringField(TEXT("requestId"), RequestId);
	Rollback->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult RolledBack = Registry.ExecuteTool(
		TEXT("content.niagara.event_handler.add.rollback"), Rollback);
	if (!RolledBack.bSuccess)
	{
		AddError(FString::Printf(
			TEXT("Event-handler rollback returned %s: %s"),
			*RolledBack.ErrorCode,
			*RolledBack.ErrorMessage));
	}
	TestTrue(TEXT("Event-handler add rollback succeeds"), RolledBack.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraEventHandlerUpdateRemoveContractTest,
	"UE_AI_integration.Niagara.EventHandlerQuery.UpdateRemove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEventHandlerUpdateRemoveContractTest::RunTest(const FString&)
{
	FNiagaraEventHandlerQueryFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(TEXT("Event-handler update/remove fixture and package are deleted"), DeleteNiagaraEventHandlerQueryFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Event-handler update/remove fixture builds"), CreateNiagaraEventHandlerQueryFixture(Fixture, false)))
	{
		AddInfo(TEXT("The /Game/ event-handler fixture could not be built; skipping update/remove contract."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraEventHandlerTools(Registry);
	Registry.EndDomainRegistration();

	const FGuid UsageId = FGuid::NewGuid();
	TSharedPtr<FJsonObject> AddParams = MakeShared<FJsonObject>();
	AddParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	AddParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	AddParams->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	AddParams->SetStringField(TEXT("sourceEventName"), TEXT("Burst"));
	AddParams->SetStringField(TEXT("executionMode"), TEXT("SpawnedParticles"));
	AddParams->SetNumberField(TEXT("spawnNumber"), 4);
	AddParams->SetNumberField(TEXT("minSpawnNumber"), 2);
	AddParams->SetNumberField(TEXT("maxEventsPerFrame"), 32);
	AddParams->SetBoolField(TEXT("randomSpawnNumber"), true);
	AddParams->SetBoolField(TEXT("updateAttributeInitialValues"), false);
	const FMCPToolResult AddPlan = Registry.ExecuteTool(TEXT("content.niagara.event_handler.add.plan"), AddParams);
	if (!TestTrue(TEXT("Event-handler setup plan succeeds"), AddPlan.bSuccess) || !AddPlan.Data)
	{
		return false;
	}
	AddParams->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	AddParams->SetBoolField(TEXT("confirmWrite"), true);
	AddParams->SetStringField(TEXT("approvePlanDigest"), AddPlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult Added = Registry.ExecuteTool(TEXT("content.niagara.event_handler.add.apply"), AddParams);
	if (!TestTrue(TEXT("Event-handler setup apply succeeds"), Added.bSuccess))
	{
		return false;
	}

	// Inject a verdict failure only after a real option update and compilation.
	// Recovery must recompile the original options, preserve both clean/dirty
	// package baselines, and leave every authored handler option unchanged.
	ON_SCOPE_EXIT { UEAINiagaraEventHandlerPrivate::SetUpdateFailureForTests(false, false); };
	const bool bOriginalDirty = Fixture.Package->IsDirty();
	for (bool bCompileFailure : {true, false})
	{
		Fixture.Package->SetDirtyFlag(!bCompileFailure);
		const bool bBeforeDirty = Fixture.Package->IsDirty();
		auto ReadParams = MakeShared<FJsonObject>();
		ReadParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		ReadParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
		const FMCPToolResult Before = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), ReadParams);
		FString BeforeDigest;
		if (!TestTrue(TEXT("Failure-recovery event baseline is readable"), Before.bSuccess && Before.Data
			&& UEAIIntegration::Infrastructure::TryDigestJson(Before.Data, BeforeDigest))) return false;
		auto FailedParams = MakeShared<FJsonObject>();
		FailedParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		FailedParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
		FailedParams->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		FailedParams->SetStringField(TEXT("sourceEventName"), TEXT("FailedUpdate"));
		FailedParams->SetNumberField(TEXT("spawnNumber"), 8);
		FailedParams->SetNumberField(TEXT("minSpawnNumber"), 3);
		const FMCPToolResult FailedPlan = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.plan"), FailedParams);
		if (!TestTrue(TEXT("Failure-recovery event update plans"), FailedPlan.bSuccess && FailedPlan.Data)) return false;
		FailedParams->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		FailedParams->SetBoolField(TEXT("confirmWrite"), true);
		FailedParams->SetStringField(TEXT("approvePlanDigest"), FailedPlan.Data->GetStringField(TEXT("planDigest")));
		UEAINiagaraEventHandlerPrivate::SetUpdateFailureForTests(bCompileFailure, !bCompileFailure);
		const FMCPToolResult Failed = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.apply"), FailedParams);
		TestFalse(TEXT("Post-write event verdict failure is returned"), Failed.bSuccess);
		TestEqual(TEXT("Event failure preserves the original verdict code"), Failed.ErrorCode,
			FString(bCompileFailure ? TEXT("compile_failed") : TEXT("verification_failed")));
		TestTrue(TEXT("Event failure verifies complete restoration"), Failed.Data
			&& Failed.Data->GetBoolField(TEXT("restorationVerified")));
		TestTrue(TEXT("Event failure recompiles the restored options"), Failed.Data
			&& Failed.Data->GetBoolField(TEXT("restorationCompiled")));
		TestEqual(TEXT("Event failure preserves the original package dirty state"), Fixture.Package->IsDirty(), bBeforeDirty);
		const FMCPToolResult After = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), ReadParams);
		FString AfterDigest;
		TestTrue(TEXT("Event failure restores all exposed options and stable identities"), After.bSuccess && After.Data
			&& UEAIIntegration::Infrastructure::TryDigestJson(After.Data, AfterDigest) && BeforeDigest == AfterDigest);
	}
	Fixture.Package->SetDirtyFlag(bOriginalDirty);

	TSharedPtr<FJsonObject> UpdateParams = MakeShared<FJsonObject>();
	UpdateParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	UpdateParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	UpdateParams->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	UpdateParams->SetStringField(TEXT("sourceEventName"), TEXT("EditedBurst"));
	UpdateParams->SetNumberField(TEXT("minSpawnNumber"), 3);
	const FMCPToolResult UpdatePlan = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.plan"), UpdateParams);
	if (!TestTrue(TEXT("Event-handler update plan succeeds"), UpdatePlan.bSuccess) || !UpdatePlan.Data)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> UpdatedOptions = UpdatePlan.Data->GetObjectField(TEXT("after"));
	TestEqual(TEXT("A partial spawn-count update inherits the authored execution mode"),
		UpdatedOptions->GetStringField(TEXT("executionMode")), FString(TEXT("SpawnedParticles")));
	TestEqual(TEXT("A partial spawn-count update inherits the authored maximum spawn count"),
		UpdatedOptions->GetIntegerField(TEXT("spawnNumber")), 4);
	TestEqual(TEXT("A partial spawn-count update changes only the requested minimum"),
		UpdatedOptions->GetIntegerField(TEXT("minSpawnNumber")), 3);

	TSharedPtr<FJsonObject> InvalidOptions = MakeShared<FJsonObject>();
	InvalidOptions->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	InvalidOptions->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	InvalidOptions->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	InvalidOptions->SetNumberField(TEXT("minSpawnNumber"), 5);
	const FMCPToolResult InvalidRange = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.plan"), InvalidOptions);
	TestFalse(TEXT("A partial count update cannot exceed the inherited spawn count"), InvalidRange.bSuccess);
	TestEqual(TEXT("Merged spawn-range validation returns its stable code"), InvalidRange.ErrorCode, FString(TEXT("spawn_range_invalid")));
	InvalidOptions->RemoveField(TEXT("minSpawnNumber"));
	InvalidOptions->SetStringField(TEXT("executionMode"), TEXT("EveryParticle"));
	const FMCPToolResult InvalidMode = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.plan"), InvalidOptions);
	TestFalse(TEXT("A mode change cannot silently inherit incompatible spawn options"), InvalidMode.bSuccess);
	TestEqual(TEXT("Merged execution-mode validation returns its stable code"), InvalidMode.ErrorCode, FString(TEXT("spawn_options_invalid")));
	InvalidOptions->SetNumberField(TEXT("spawnNumber"), 0);
	InvalidOptions->SetNumberField(TEXT("minSpawnNumber"), 0);
	InvalidOptions->SetBoolField(TEXT("randomSpawnNumber"), false);
	const FMCPToolResult ClearedMode = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.plan"), InvalidOptions);
	TestTrue(TEXT("A mode change can explicitly clear incompatible spawn options"), ClearedMode.bSuccess);
	const FString UpdateRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	UpdateParams->SetStringField(TEXT("requestId"), UpdateRequestId);
	UpdateParams->SetBoolField(TEXT("confirmWrite"), true);
	UpdateParams->SetStringField(TEXT("approvePlanDigest"), UpdatePlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult Updated = Registry.ExecuteTool(TEXT("content.niagara.event_handler.update.apply"), UpdateParams);
	if (!TestTrue(TEXT("Event-handler update apply succeeds with omitted options preserved"), Updated.bSuccess))
	{
		return false;
	}

	const FVersionedNiagaraEmitterData* EmitterData = Fixture.Emitter->GetEmitterData(Fixture.EmitterVersion);
	const UNiagaraScriptSource* Source = EmitterData ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource) : nullptr;
	UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
	if (!TestNotNull(TEXT("Event-handler update/remove graph is available"), Graph))
	{
		return false;
	}
	const TArray<UEdGraphNode*> OriginalNodes = Graph->Nodes;
	TMap<UEdGraphPin*, TArray<UEdGraphPin*>> OriginalLinks;
	for (UEdGraphNode* Node : OriginalNodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			OriginalLinks.Add(Pin, Pin->LinkedTo);
		}
	}
	UNiagaraNodeOutput* OriginalOutput = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleEventScript, UsageId);
	if (!TestNotNull(TEXT("The original event output is retained for identity verification"), OriginalOutput)
		|| !TestTrue(TEXT("The original event output has a pin"), OriginalOutput->Pins.Num() > 0))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
	RemoveParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	RemoveParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	RemoveParams->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
	const FMCPToolResult RemovePlan = Registry.ExecuteTool(TEXT("content.niagara.event_handler.remove.plan"), RemoveParams);
	if (!TestTrue(TEXT("Event-handler remove plan succeeds"), RemovePlan.bSuccess) || !RemovePlan.Data)
	{
		return false;
	}
	const FString RemoveRequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	RemoveParams->SetStringField(TEXT("requestId"), RemoveRequestId);
	RemoveParams->SetBoolField(TEXT("confirmWrite"), true);
	RemoveParams->SetStringField(TEXT("approvePlanDigest"), RemovePlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult Removed = Registry.ExecuteTool(TEXT("content.niagara.event_handler.remove.apply"), RemoveParams);
	if (!TestTrue(TEXT("Event-handler remove apply succeeds"), Removed.bSuccess) || !Removed.Data)
	{
		return false;
	}
	const FString RemoveReceipt = Removed.Data->GetStringField(TEXT("receiptId"));

	TSharedPtr<FJsonObject> ListParams = MakeShared<FJsonObject>();
	ListParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	ListParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	const FMCPToolResult EmptyList = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), ListParams);
	TestTrue(TEXT("Event-handler list succeeds after removal"), EmptyList.bSuccess);
	TestEqual(TEXT("Removed event-handler list is empty"), EmptyList.Data->GetIntegerField(TEXT("total")), 0);

	TSharedPtr<FJsonObject> RollbackParams = MakeShared<FJsonObject>();
	RollbackParams->SetStringField(TEXT("rollbackId"), RemoveReceipt);
	RollbackParams->SetStringField(TEXT("requestId"), RemoveRequestId);
	RollbackParams->SetBoolField(TEXT("confirmWrite"), true);
	// A removed node can survive while its pins are reconstructed. Exercise
	// that receipt hazard without notifying or mutating the surviving graph.
	UEdGraphPin* OriginalOutputPin = OriginalOutput->Pins[0];
	OriginalOutput->Pins[0] = nullptr;
	const TArray<UEdGraphNode*> NodesBeforeRefusedRollback = Graph->Nodes;
	const FMCPToolResult RefusedRollback = Registry.ExecuteTool(TEXT("content.niagara.event_handler.remove.rollback"), RollbackParams);
	OriginalOutput->Pins[0] = OriginalOutputPin;
	TestFalse(TEXT("Removal rollback refuses a snapshot whose original pin is no longer a member"), RefusedRollback.bSuccess);
	TestEqual(TEXT("Invalid snapshot rollback has a stable failure code"), RefusedRollback.ErrorCode, FString(TEXT("rollback_snapshot_invalid")));
	TestTrue(TEXT("Rejected snapshot rollback preserves all surviving graph nodes"),
		TArray<UEdGraphNode*>(Graph->Nodes) == NodesBeforeRefusedRollback);
	TestNull(TEXT("Rejected snapshot rollback does not create an empty event output"),
		Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleEventScript, UsageId));
	const FMCPToolResult StillEmpty = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), ListParams);
	if (TestTrue(TEXT("The handler list remains readable after rejected rollback"), StillEmpty.bSuccess) && StillEmpty.Data)
	{
		TestEqual(TEXT("Rejected rollback does not restore properties without the graph"), StillEmpty.Data->GetIntegerField(TEXT("total")), 0);
	}

	const FMCPToolResult RolledBack = Registry.ExecuteTool(TEXT("content.niagara.event_handler.remove.rollback"), RollbackParams);
	if (!RolledBack.bSuccess)
	{
		AddError(FString::Printf(TEXT("Event-handler removal rollback returned %s: %s"), *RolledBack.ErrorCode, *RolledBack.ErrorMessage));
	}
	if (!TestTrue(TEXT("Event-handler removal rollback succeeds"), RolledBack.bSuccess))
	{
		return false;
	}
	TestTrue(TEXT("Removal rollback restores every original graph node in its original order"),
		TArray<UEdGraphNode*>(Graph->Nodes) == OriginalNodes);
	TestTrue(TEXT("Removal rollback restores the original event output rather than an empty replacement"),
		Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleEventScript, UsageId) == OriginalOutput);
	for (UEdGraphNode* Node : OriginalNodes)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			const TArray<UEdGraphPin*>* ExpectedLinks = OriginalLinks.Find(Pin);
			TestTrue(TEXT("Removal rollback restores original pin identities and complete connections"),
				ExpectedLinks && *ExpectedLinks == Pin->LinkedTo);
		}
	}
	const FMCPToolResult RestoredList = Registry.ExecuteTool(TEXT("content.niagara.event_handler.list"), ListParams);
	TestTrue(TEXT("Event-handler list succeeds after rollback"), RestoredList.bSuccess);
	TestEqual(TEXT("Rollback restores one event-handler"), RestoredList.Data->GetIntegerField(TEXT("total")), 1);
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (RestoredList.Data->TryGetArrayField(TEXT("eventHandlers"), Rows) && Rows && Rows->Num() == 1)
	{
		TestEqual(TEXT("Rollback restores the updated source event"), (*Rows)[0]->AsObject()->GetStringField(TEXT("sourceEventName")), FString(TEXT("EditedBurst")));
		TestTrue(TEXT("Rollback restores the event graph"), (*Rows)[0]->AsObject()->GetBoolField(TEXT("graphPresent")));
	}

	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	InvalidParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	InvalidParams->SetStringField(TEXT("emitter"), Fixture.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
	InvalidParams->SetStringField(TEXT("usageId"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	const FMCPToolResult Invalid = Registry.ExecuteTool(TEXT("content.niagara.event_handler.remove.plan"), InvalidParams);
	TestFalse(TEXT("Event-handler remove rejects an unknown stable identity"), Invalid.bSuccess);
	TestEqual(TEXT("Unknown event-handler uses a stable error code"), Invalid.ErrorCode, FString(TEXT("handler_not_found")));
	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
