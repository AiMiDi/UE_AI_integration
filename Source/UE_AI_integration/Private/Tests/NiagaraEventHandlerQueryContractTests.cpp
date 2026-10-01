// Contract coverage for content.niagara.event_handler.list.
//
// The query is read-only and bounded.  This test builds a minimal /Game
// Niagara system with one authored event-handler entry, then verifies the
// stable identity/options envelope and page controls without saving or
// requiring runtime event delivery.
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
#include "NiagaraGraph.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraEventHandlerTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
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
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);
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
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
