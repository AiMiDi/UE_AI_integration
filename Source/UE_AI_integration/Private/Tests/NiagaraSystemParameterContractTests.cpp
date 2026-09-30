#if WITH_DEV_AUTOMATION_TESTS

#include <limits>

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraParameterStore.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraSystemParameterTools(FMCPToolRegistry& Registry);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemParameterRegistrationTest,
	"UE_AI_integration.Niagara.SystemParameter.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemParameterRegistrationTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSystemParameterTools(Registry);
	Registry.EndDomainRegistration();

	static const TCHAR* ExpectedCapabilities[] = {
		TEXT("content.niagara.system.parameter.add"),
		TEXT("content.niagara.system.parameter.remove"),
		TEXT("content.niagara.system.parameter.get"),
		TEXT("content.niagara.system.parameter.plan"),
		TEXT("content.niagara.system.parameter.apply"),
		TEXT("content.niagara.system.parameter.rollback"),
		TEXT("content.niagara.system.parameter.receipt.release"),
	};
	TestEqual(
		TEXT("Exactly seven system-parameter capabilities register"),
		Registry.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedCapabilities)));
	for (const TCHAR* Capability : ExpectedCapabilities)
	{
		TestNotNull(Capability, Registry.FindTool(Capability));
	}
	return true;
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FAuthoredNiagaraParameter
{
	FNiagaraVariable Variable;
	UNiagaraScriptVariable* ScriptVariable = nullptr;
};

struct FNiagaraSystemParameterFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FAuthoredNiagaraParameter Float;
	FAuthoredNiagaraParameter Bool;
	FAuthoredNiagaraParameter Vector;
	FAuthoredNiagaraParameter Position;
	FAuthoredNiagaraParameter UnsupportedMatrix;
};

template <typename TValue>
bool AddAuthoredParameter(
	UNiagaraSystem* System,
	const FNiagaraTypeDefinition& Type,
	const FName Name,
	const TValue& Value,
	FAuthoredNiagaraParameter& OutParameter)
{
	if (!System)
	{
		return false;
	}
	OutParameter.Variable = FNiagaraVariable(Type, Name);
	// FNiagaraEditorUtilities::AddParameter is not exported from the NiagaraEditor
	// module (no NIAGARAEDITOR_API), so the fixture adds the user parameter to the
	// exposed store directly. Test parameters use unique names and need no undo.
	if (!System->GetExposedParameters().AddParameter(OutParameter.Variable))
	{
		return false;
	}
	UNiagaraSystemEditorData* EditorData =
		Cast<UNiagaraSystemEditorData>(System->GetEditorData());
	if (!EditorData)
	{
		return false;
	}
	EditorData->SyncUserScriptVariables(System);
	OutParameter.ScriptVariable = EditorData->FindOrAddUserScriptVariable(
		OutParameter.Variable, *System);
	if (!OutParameter.ScriptVariable)
	{
		return false;
	}
	OutParameter.ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
	OutParameter.ScriptVariable->SetDefaultValueData(
		reinterpret_cast<const uint8*>(&Value));
	return System->GetExposedParameters().SetParameterValue<TValue>(
		Value, OutParameter.Variable, false);
}

template <typename TValue>
bool SetAuthoredValue(
	UNiagaraSystem* System,
	const FAuthoredNiagaraParameter& Parameter,
	const TValue& Value)
{
	if (!System || !Parameter.ScriptVariable)
	{
		return false;
	}
	Parameter.ScriptVariable->SetDefaultValueData(
		reinterpret_cast<const uint8*>(&Value));
	Parameter.ScriptVariable->UpdateChangeId();
	return System->GetExposedParameters().SetParameterValue<TValue>(
		Value, Parameter.Variable, false);
}

template <typename TValue>
bool ReadAuthoredValue(
	const UNiagaraSystem* System,
	const FAuthoredNiagaraParameter& Parameter,
	TValue& OutStore,
	TValue& OutScript)
{
	if (!System || !Parameter.ScriptVariable)
	{
		return false;
	}
	const uint8* StoreData =
		System->GetExposedParameters().GetParameterData(Parameter.Variable);
	const uint8* ScriptData = Parameter.ScriptVariable->GetDefaultValueData();
	if (!StoreData || !ScriptData)
	{
		return false;
	}
	FMemory::Memcpy(&OutStore, StoreData, sizeof(TValue));
	FMemory::Memcpy(&OutScript, ScriptData, sizeof(TValue));
	return true;
}

FNiagaraSystemParameterFixture CreateNiagaraSystemParameterFixture()
{
	FNiagaraSystemParameterFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/UEAI_SystemParameter_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(Fixture.PackageName);
	Fixture.Package = CreatePackage(*Fixture.PackageName);
	Fixture.System = Fixture.Package
		? NewObject<UNiagaraSystem>(
			Fixture.Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!Fixture.System)
	{
		return Fixture;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(Fixture.System, true);
	FAssetRegistryModule::AssetCreated(Fixture.System);

	FNiagaraBool InitialBool;
	InitialBool.SetValue(false);
	const FVector3f InitialVector(1.0f, 2.0f, 3.0f);
	const FVector3f InitialPosition(100.0f, 200.0f, 300.0f);
	const FMatrix44f InitialMatrix = FMatrix44f::Identity;
	const bool bAdded =
		AddAuthoredParameter(
			Fixture.System,
			FNiagaraTypeDefinition::GetFloatDef(),
			FName(TEXT("User.ContractFloat")),
			1.25f,
			Fixture.Float)
		&& AddAuthoredParameter(
			Fixture.System,
			FNiagaraTypeDefinition::GetBoolDef(),
			FName(TEXT("User.ContractBool")),
			InitialBool,
			Fixture.Bool)
		&& AddAuthoredParameter(
			Fixture.System,
			FNiagaraTypeDefinition::GetVec3Def(),
			FName(TEXT("User.ContractVector")),
			InitialVector,
			Fixture.Vector)
		&& AddAuthoredParameter(
			Fixture.System,
			FNiagaraTypeDefinition::GetPositionDef(),
			FName(TEXT("User.ContractPosition")),
			InitialPosition,
			Fixture.Position)
		&& AddAuthoredParameter(
			Fixture.System,
			FNiagaraTypeDefinition::GetMatrix4Def(),
			FName(TEXT("User.UnsupportedMatrix")),
			InitialMatrix,
			Fixture.UnsupportedMatrix);
	if (!bAdded)
	{
		Fixture.Float.ScriptVariable = nullptr;
	}
	Fixture.Package->SetDirtyFlag(false);
	return Fixture;
}

bool DeleteNiagaraSystemParameterFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

TSharedRef<FJsonObject> MakeParameterParams(
	const UNiagaraSystem* System,
	const FString& Parameter,
	const TSharedPtr<FJsonValue>& Value = nullptr)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), System->GetPathName());
	Params->SetStringField(TEXT("parameter"), Parameter);
	if (Value)
	{
		Params->SetField(TEXT("value"), Value);
	}
	return Params;
}

TSharedRef<FJsonValueObject> VectorValue(
	const float X,
	const float Y,
	const float Z)
{
	auto Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), X);
	Object->SetNumberField(TEXT("y"), Y);
	Object->SetNumberField(TEXT("z"), Z);
	return MakeShared<FJsonValueObject>(Object);
}

FMCPToolResult PlanAndApproveParameter(
	FMCPToolRegistry& Registry,
	const TSharedRef<FJsonObject>& Params,
	const FString& RequestId)
{
	Params->RemoveField(TEXT("approvePlanDigest"));
	Params->RemoveField(TEXT("confirmWrite"));
	Params->RemoveField(TEXT("requestId"));
	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.plan"), Params);
	if (Plan.bSuccess && Plan.Data)
	{
		Params->SetStringField(
			TEXT("approvePlanDigest"),
			Plan.Data->GetStringField(TEXT("planDigest")));
		Params->SetBoolField(TEXT("confirmWrite"), true);
		Params->SetStringField(TEXT("requestId"), RequestId);
	}
	return Plan;
}

FMCPToolResult RollbackParameter(
	FMCPToolRegistry& Registry,
	const FMCPToolResult& Applied,
	const FString& RequestId)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("rollbackId"),
		Applied.Data->GetStringField(TEXT("rollbackId")));
	Params->SetStringField(TEXT("requestId"), RequestId);
	Params->SetBoolField(TEXT("confirmWrite"), true);
	return Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.rollback"), Params);
}

FMCPToolResult ReleaseParameterReceipt(
	FMCPToolRegistry& Registry,
	const FMCPToolResult& Applied,
	const FString& RequestId,
	const bool bConfirmDiscardRollback = false)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("rollbackId"),
		Applied.Data->GetStringField(TEXT("rollbackId")));
	Params->SetStringField(TEXT("requestId"), RequestId);
	Params->SetBoolField(TEXT("confirmWrite"), true);
	if (bConfirmDiscardRollback)
	{
		Params->SetBoolField(TEXT("confirmDiscardRollback"), true);
	}
	return Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.receipt.release"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemParameterSemanticContractTest,
	"UE_AI_integration.Niagara.SystemParameter.SemanticContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemParameterSemanticContractTest::RunTest(const FString&)
{
	const FNiagaraSystemParameterFixture Fixture =
		CreateNiagaraSystemParameterFixture();
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("System-parameter fixture and package are deleted"),
			DeleteNiagaraSystemParameterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Owned Niagara System fixture"), Fixture.System)
		|| !TestNotNull(TEXT("System-parameter fixture package"), Fixture.Package)
		|| !TestNotNull(
			TEXT("Authored float metadata fixture"),
			Fixture.Float.ScriptVariable))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSystemParameterTools(Registry);
	Registry.EndDomainRegistration();

	// Reads report both the exposed store and the authored ScriptVariable
	// default, never implying that loaded component overrides were inspected.
	const FMCPToolResult FloatGet = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.get"),
		MakeParameterParams(Fixture.System, TEXT("ContractFloat")));
	if (!TestTrue(TEXT("Float authored-default read succeeds"), FloatGet.bSuccess)
		|| !FloatGet.Data)
	{
		return false;
	}
	TestTrue(
		TEXT("Float store and authored metadata are aligned"),
		FloatGet.Data->GetBoolField(TEXT("valuesAligned")));
	TestEqual(
		TEXT("Float store value is reported"),
		FloatGet.Data->GetNumberField(TEXT("storeValue")),
		1.25);
	TestEqual(
		TEXT("Float authored default is reported"),
		FloatGet.Data->GetNumberField(TEXT("authoredDefaultValue")),
		1.25);
	TestTrue(
		TEXT("Read scope does not claim component overrides"),
		FloatGet.Data->GetStringField(TEXT("scope"))
			.Contains(TEXT("not inspected")));
	TestTrue(
		TEXT("Read returns a stable state digest"),
		!FloatGet.Data->GetStringField(TEXT("stateDigest")).IsEmpty());

	const FMCPToolResult BoolGet = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.get"),
		MakeParameterParams(Fixture.System, TEXT("User.ContractBool")));
	if (TestTrue(TEXT("Bool authored-default read succeeds"), BoolGet.bSuccess)
		&& BoolGet.Data)
	{
		TestFalse(
			TEXT("Bool store value preserves boolean type"),
			BoolGet.Data->GetBoolField(TEXT("storeValue")));
		TestFalse(
			TEXT("Bool authored default preserves boolean type"),
			BoolGet.Data->GetBoolField(TEXT("authoredDefaultValue")));
	}

	for (const FString Parameter : {
		FString(TEXT("ContractVector")),
		FString(TEXT("ContractPosition"))})
	{
		const FMCPToolResult Get = Registry.ExecuteTool(
			TEXT("content.niagara.system.parameter.get"),
			MakeParameterParams(Fixture.System, Parameter));
		if (TestTrue(*FString::Printf(TEXT("%s read succeeds"), *Parameter), Get.bSuccess)
			&& Get.Data)
		{
			const TSharedPtr<FJsonObject> Store =
				Get.Data->GetObjectField(TEXT("storeValue"));
			const TSharedPtr<FJsonObject> Authored =
				Get.Data->GetObjectField(TEXT("authoredDefaultValue"));
			TestNotNull(TEXT("Vector store projection"), Store.Get());
			TestNotNull(TEXT("Vector authored projection"), Authored.Get());
			TestTrue(
				TEXT("Vector-like store and authored metadata align"),
				Get.Data->GetBoolField(TEXT("valuesAligned")));
		}
	}

	const FMCPToolResult Unsupported = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.get"),
		MakeParameterParams(Fixture.System, TEXT("UnsupportedMatrix")));
	TestFalse(TEXT("Unsupported Matrix default is rejected"), Unsupported.bSuccess);
	TestEqual(
		TEXT("Unsupported type has a stable code"),
		Unsupported.ErrorCode,
		FString(TEXT("parameter_type_unsupported")));

	// Plan is read-only. A wrong approval digest must perform zero writes and
	// must not reserve the request ID.
	const FString FloatRequest = TEXT("float-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto FloatParams = MakeParameterParams(
		Fixture.System,
		TEXT("ContractFloat"),
		MakeShared<FJsonValueNumber>(2.5));
	const FMCPToolResult FloatPlan = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.plan"), FloatParams);
	if (!TestTrue(TEXT("Float parameter plan succeeds"), FloatPlan.bSuccess)
		|| !FloatPlan.Data)
	{
		return false;
	}
	TestTrue(
		TEXT("Float plan predicts a state change"),
		FloatPlan.Data->GetBoolField(TEXT("changesState")));
	TestTrue(
		TEXT("Float plan predicts only a compile request"),
		FloatPlan.Data->GetBoolField(TEXT("compileWillBeRequested")));
	TestEqual(
		TEXT("Float plan is dirty-only"),
		FloatPlan.Data->GetStringField(TEXT("persistence")),
		FString(TEXT("dirtyOnly")));
	TestFalse(TEXT("Planning preserves clean package"), Fixture.Package->IsDirty());

	FloatParams->SetStringField(TEXT("requestId"), FloatRequest);
	FloatParams->SetStringField(
		TEXT("approvePlanDigest"),
		TEXT("0000000000000000000000000000000000000000000000000000000000000000"));
	FloatParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult WrongDigest = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), FloatParams);
	TestFalse(TEXT("Wrong parameter plan digest is rejected"), WrongDigest.bSuccess);
	TestEqual(
		TEXT("Wrong parameter plan digest uses a stable code"),
		WrongDigest.ErrorCode,
		FString(TEXT("plan_digest_mismatch")));
	float StoreFloat = 0.0f;
	float ScriptFloat = 0.0f;
	TestTrue(
		TEXT("Float store and metadata remain readable after rejection"),
		ReadAuthoredValue(
			Fixture.System, Fixture.Float, StoreFloat, ScriptFloat));
	TestEqual(TEXT("Wrong digest leaves store unchanged"), StoreFloat, 1.25f);
	TestEqual(TEXT("Wrong digest leaves metadata unchanged"), ScriptFloat, 1.25f);
	TestFalse(TEXT("Wrong digest leaves package clean"), Fixture.Package->IsDirty());

	const FMCPToolResult FreshFloatPlan = PlanAndApproveParameter(
		Registry, FloatParams, FloatRequest);
	if (!TestTrue(TEXT("Fresh float plan succeeds"), FreshFloatPlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult FloatApplied = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), FloatParams);
	if (!TestTrue(TEXT("Approved float parameter applies"), FloatApplied.bSuccess)
		|| !FloatApplied.Data)
	{
		return false;
	}
	TestTrue(TEXT("Float write reports changed"), FloatApplied.Data->GetBoolField(TEXT("changed")));
	TestTrue(
		TEXT("Float write requests compilation"),
		FloatApplied.Data->GetBoolField(TEXT("compileRequested")));
	TestFalse(
		TEXT("Async compile request is not reported complete"),
		FloatApplied.Data->GetBoolField(TEXT("compiled")));
	TestFalse(
		TEXT("Dirty-only float write is not reported saved"),
		FloatApplied.Data->GetBoolField(TEXT("saved")));
	TestFalse(
		TEXT("Authored edit is not reported runtime verified"),
		FloatApplied.Data->GetBoolField(TEXT("runtimeVerified")));
	TestEqual(
		TEXT("System-parameter receipt capacity is bounded"),
		FloatApplied.Data->GetIntegerField(TEXT("activeReceiptLimit")),
		256);
	TestTrue(TEXT("Float apply dirties the package"), Fixture.Package->IsDirty());
	ReadAuthoredValue(Fixture.System, Fixture.Float, StoreFloat, ScriptFloat);
	TestEqual(TEXT("Float store receives desired value"), StoreFloat, 2.5f);
	TestEqual(TEXT("Float metadata receives desired value"), ScriptFloat, 2.5f);

	const FMCPToolResult FloatReplay = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), FloatParams);
	if (TestTrue(TEXT("Exact float request replays"), FloatReplay.bSuccess)
		&& FloatReplay.Data)
	{
		TestTrue(
			TEXT("Float replay is explicit"),
			FloatReplay.Data->GetBoolField(TEXT("idempotentReplay")));
		TestEqual(
			TEXT("Float replay returns the original receipt"),
			FloatReplay.Data->GetStringField(TEXT("receiptId")),
			FloatApplied.Data->GetStringField(TEXT("receiptId")));
	}

	auto ConflictingRequest = MakeParameterParams(
		Fixture.System,
		TEXT("ContractFloat"),
		MakeShared<FJsonValueNumber>(3.5));
	ConflictingRequest->SetStringField(TEXT("requestId"), FloatRequest);
	ConflictingRequest->SetStringField(
		TEXT("approvePlanDigest"),
		FloatApplied.Data->GetStringField(TEXT("planDigest")));
	ConflictingRequest->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult RequestConflict = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), ConflictingRequest);
	TestFalse(TEXT("Reused request ID with new arguments is rejected"), RequestConflict.bSuccess);
	TestEqual(
		TEXT("Request ID conflict uses a stable code"),
		RequestConflict.ErrorCode,
		FString(TEXT("request_id_conflict")));

	// Receipt replay and rollback both refuse authored-state drift.
	TestTrue(
		TEXT("External float drift is injected"),
		SetAuthoredValue(Fixture.System, Fixture.Float, 42.0f));
	const FMCPToolResult StaleReplay = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), FloatParams);
	TestFalse(TEXT("Stale receipt replay is rejected"), StaleReplay.bSuccess);
	TestEqual(
		TEXT("Stale receipt replay uses a stable code"),
		StaleReplay.ErrorCode,
		FString(TEXT("receipt_state_changed")));
	const FMCPToolResult StaleRollback = RollbackParameter(
		Registry, FloatApplied, FloatRequest);
	TestFalse(TEXT("Stale receipt rollback is rejected"), StaleRollback.bSuccess);
	TestEqual(
		TEXT("Stale rollback uses a stable code"),
		StaleRollback.ErrorCode,
		FString(TEXT("rollback_conflict")));
	TestTrue(
		TEXT("Expected post-apply float state is restored for rollback"),
		SetAuthoredValue(Fixture.System, Fixture.Float, 2.5f));

	// Package dirtiness may include unrelated user work. Manual rollback must
	// restore semantic state without clearing that dirty flag.
	Fixture.Package->SetDirtyFlag(true);
	const FMCPToolResult FloatRolledBack = RollbackParameter(
		Registry, FloatApplied, FloatRequest);
	if (!TestTrue(TEXT("Float receipt rolls back"), FloatRolledBack.bSuccess)
		|| !FloatRolledBack.Data)
	{
		return false;
	}
	TestTrue(
		TEXT("Float rollback verifies semantic restoration"),
		FloatRolledBack.Data->GetBoolField(TEXT("semanticRollbackVerified")));
	TestFalse(
		TEXT("Manual rollback does not claim dirty restoration"),
		FloatRolledBack.Data->GetBoolField(TEXT("dirtyRestored")));
	TestTrue(
		TEXT("Manual rollback preserves possible external dirtiness"),
		FloatRolledBack.Data->GetBoolField(TEXT("dirtyPreserved")));
	TestFalse(
		TEXT("Manual rollback does not claim full package restoration"),
		FloatRolledBack.Data->GetBoolField(TEXT("fullPackageStateRestored")));
	TestEqual(
		TEXT("Manual rollback reports the conservative outcome"),
		FloatRolledBack.Data->GetStringField(TEXT("outcome")),
		FString(TEXT("semantic_restored_dirty_preserved")));
	ReadAuthoredValue(Fixture.System, Fixture.Float, StoreFloat, ScriptFloat);
	TestEqual(TEXT("Float rollback restores store"), StoreFloat, 1.25f);
	TestEqual(TEXT("Float rollback restores metadata"), ScriptFloat, 1.25f);
	TestTrue(TEXT("Float rollback preserves package dirty"), Fixture.Package->IsDirty());

	const FMCPToolResult FloatReleased = ReleaseParameterReceipt(
		Registry, FloatApplied, FloatRequest);
	if (TestTrue(TEXT("Rolled-back float receipt releases"), FloatReleased.bSuccess)
		&& FloatReleased.Data)
	{
		TestTrue(TEXT("Float receipt is released"), FloatReleased.Data->GetBoolField(TEXT("released")));
		TestFalse(TEXT("Receipt release does not mutate asset"), FloatReleased.Data->GetBoolField(TEXT("assetMutated")));
		TestTrue(
			TEXT("Released request history is bounded"),
			FloatReleased.Data->GetIntegerField(TEXT("terminalHistoryLimit")) > 0);
	}
	const FMCPToolResult FloatReleaseReplay = ReleaseParameterReceipt(
		Registry, FloatApplied, FloatRequest);
	if (TestTrue(TEXT("Float receipt release replays"), FloatReleaseReplay.bSuccess)
		&& FloatReleaseReplay.Data)
	{
		TestTrue(
			TEXT("Float release replay is explicit"),
			FloatReleaseReplay.Data->GetBoolField(TEXT("idempotentReplay")));
	}

	// Bool write exercises non-number input and active receipt discard rules.
	const FString BoolRequest = TEXT("bool-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto BoolParams = MakeParameterParams(
		Fixture.System,
		TEXT("ContractBool"),
		MakeShared<FJsonValueBoolean>(true));
	const FMCPToolResult BoolPlan = PlanAndApproveParameter(
		Registry, BoolParams, BoolRequest);
	if (!TestTrue(TEXT("Bool plan succeeds"), BoolPlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult BoolApplied = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), BoolParams);
	if (!TestTrue(TEXT("Bool parameter applies"), BoolApplied.bSuccess)
		|| !BoolApplied.Data)
	{
		return false;
	}
	FNiagaraBool StoreBool;
	FNiagaraBool ScriptBool;
	ReadAuthoredValue(Fixture.System, Fixture.Bool, StoreBool, ScriptBool);
	TestTrue(TEXT("Bool store receives desired value"), StoreBool.GetValue());
	TestTrue(TEXT("Bool metadata receives desired value"), ScriptBool.GetValue());
	const FMCPToolResult BoolReleaseRejected = ReleaseParameterReceipt(
		Registry, BoolApplied, BoolRequest);
	TestFalse(
		TEXT("Changed active receipt requires discard confirmation"),
		BoolReleaseRejected.bSuccess);
	TestEqual(
		TEXT("Active receipt discard has a stable code"),
		BoolReleaseRejected.ErrorCode,
		FString(TEXT("rollback_discard_confirmation_required")));
	const FMCPToolResult BoolReleased = ReleaseParameterReceipt(
		Registry, BoolApplied, BoolRequest, true);
	TestTrue(
		TEXT("Explicit discard confirmation releases active bool receipt"),
		BoolReleased.bSuccess);

	// Vec3 write verifies strict object input and two-layer rollback.
	const FString VectorRequest = TEXT("vec3-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto VectorParams = MakeParameterParams(
		Fixture.System,
		TEXT("ContractVector"),
		VectorValue(4.0f, 5.0f, 6.0f));
	const FMCPToolResult VectorPlan = PlanAndApproveParameter(
		Registry, VectorParams, VectorRequest);
	if (!TestTrue(TEXT("Vec3 plan succeeds"), VectorPlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult VectorApplied = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.apply"), VectorParams);
	if (!TestTrue(TEXT("Vec3 parameter applies"), VectorApplied.bSuccess)
		|| !VectorApplied.Data)
	{
		return false;
	}
	FVector3f StoreVector;
	FVector3f ScriptVector;
	ReadAuthoredValue(Fixture.System, Fixture.Vector, StoreVector, ScriptVector);
	TestEqual(TEXT("Vec3 store receives X"), StoreVector.X, 4.0f);
	TestEqual(TEXT("Vec3 store receives Y"), StoreVector.Y, 5.0f);
	TestEqual(TEXT("Vec3 store receives Z"), StoreVector.Z, 6.0f);
	TestEqual(TEXT("Vec3 metadata receives X"), ScriptVector.X, 4.0f);
	const FMCPToolResult VectorRolledBack = RollbackParameter(
		Registry, VectorApplied, VectorRequest);
	TestTrue(TEXT("Vec3 receipt rolls back"), VectorRolledBack.bSuccess);
	ReadAuthoredValue(Fixture.System, Fixture.Vector, StoreVector, ScriptVector);
	TestEqual(TEXT("Vec3 rollback restores store X"), StoreVector.X, 1.0f);
	TestEqual(TEXT("Vec3 rollback restores metadata Z"), ScriptVector.Z, 3.0f);
	const FMCPToolResult VectorReleased = ReleaseParameterReceipt(
		Registry, VectorApplied, VectorRequest);
	TestTrue(TEXT("Rolled-back Vec3 receipt releases"), VectorReleased.bSuccess);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemParameterNonFiniteDigestContractTest,
	"UE_AI_integration.Niagara.SystemParameter.NonFiniteDigestContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemParameterNonFiniteDigestContractTest::RunTest(const FString&)
{
	const FNiagaraSystemParameterFixture Fixture =
		CreateNiagaraSystemParameterFixture();
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Non-finite fixture and package are deleted"),
			DeleteNiagaraSystemParameterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Non-finite Niagara fixture"), Fixture.System)
		|| !TestNotNull(
			TEXT("Non-finite authored metadata fixture"),
			Fixture.Float.ScriptVariable))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSystemParameterTools(Registry);
	Registry.EndDomainRegistration();

	const float QuietNaN = std::numeric_limits<float>::quiet_NaN();
	const float Infinity = std::numeric_limits<float>::infinity();
	TestTrue(
		TEXT("NaN authored state is injected"),
		SetAuthoredValue(Fixture.System, Fixture.Float, QuietNaN));
	const FMCPToolResult NaNPlan = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.plan"),
		MakeParameterParams(
			Fixture.System,
			TEXT("ContractFloat"),
			MakeShared<FJsonValueNumber>(1.0)));
	TestTrue(
		TEXT("Infinity authored state is injected"),
		SetAuthoredValue(Fixture.System, Fixture.Float, Infinity));
	const FMCPToolResult InfinityPlan = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.plan"),
		MakeParameterParams(
			Fixture.System,
			TEXT("ContractFloat"),
			MakeShared<FJsonValueNumber>(1.0)));

	if (!TestTrue(
			TEXT("NaN authored state remains inspectable through raw identity"),
			NaNPlan.bSuccess)
		|| !TestTrue(
			TEXT("Infinity authored state remains inspectable through raw identity"),
			InfinityPlan.bSuccess)
		|| !TestNotNull(TEXT("NaN plan data"), NaNPlan.Data.Get())
		|| !TestNotNull(TEXT("Infinity plan data"), InfinityPlan.Data.Get()))
	{
		return false;
	}
	TestFalse(TEXT("NaN store projection is explicitly non-representable"),
		NaNPlan.Data->GetBoolField(TEXT("beforeStoreValueRepresentable")));
	TestFalse(TEXT("NaN authored projection is explicitly non-representable"),
		NaNPlan.Data->GetBoolField(
			TEXT("beforeAuthoredDefaultValueRepresentable")));
	TestFalse(TEXT("Infinity store projection is explicitly non-representable"),
		InfinityPlan.Data->GetBoolField(TEXT("beforeStoreValueRepresentable")));
	TestFalse(TEXT("Infinity authored projection is explicitly non-representable"),
		InfinityPlan.Data->GetBoolField(
			TEXT("beforeAuthoredDefaultValueRepresentable")));
	const TSharedPtr<FJsonValue>* NaNValue =
		NaNPlan.Data->Values.Find(TEXT("beforeStoreValue"));
	const TSharedPtr<FJsonValue>* InfinityValue =
		InfinityPlan.Data->Values.Find(TEXT("beforeStoreValue"));
	TestTrue(TEXT("NaN JSON projection is null"),
		NaNValue && NaNValue->IsValid() && (*NaNValue)->Type == EJson::Null);
	TestTrue(TEXT("Infinity JSON projection is null"),
		InfinityValue && InfinityValue->IsValid()
			&& (*InfinityValue)->Type == EJson::Null);
	const FString NaNStoreRawDigest =
		NaNPlan.Data->GetStringField(TEXT("beforeStoreRawDigest"));
	const FString InfinityStoreRawDigest =
		InfinityPlan.Data->GetStringField(TEXT("beforeStoreRawDigest"));
	TestEqual(TEXT("NaN raw state uses a SHA-256 digest"),
		NaNStoreRawDigest.Len(), 64);
	TestEqual(TEXT("Infinity raw state uses a SHA-256 digest"),
		InfinityStoreRawDigest.Len(), 64);
	TestNotEqual(TEXT("NaN and Infinity raw identities do not collide"),
		NaNStoreRawDigest, InfinityStoreRawDigest);
	TestNotEqual(TEXT("NaN and Infinity state digests do not collide"),
		NaNPlan.Data->GetStringField(TEXT("stateDigest")),
		InfinityPlan.Data->GetStringField(TEXT("stateDigest")));
	TestNotEqual(TEXT("NaN and Infinity plan approvals do not collide"),
		NaNPlan.Data->GetStringField(TEXT("planDigest")),
		InfinityPlan.Data->GetStringField(TEXT("planDigest")));
	TestFalse(TEXT("Read-only non-finite planning preserves package dirtiness"),
		Fixture.Package->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemParameterAddContractTest,
	"UE_AI_integration.Niagara.SystemParameter.AddContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemParameterAddContractTest::RunTest(const FString&)
{
	const FNiagaraSystemParameterFixture Fixture =
		CreateNiagaraSystemParameterFixture();
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Add-parameter fixture and package are deleted"),
			DeleteNiagaraSystemParameterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Add-parameter Niagara System fixture"), Fixture.System)
		|| !TestNotNull(TEXT("Add-parameter fixture package"), Fixture.Package))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSystemParameterTools(Registry);
	Registry.EndDomainRegistration();

	auto MakeAddParams = [&Fixture](const FString& Name, const FString& Type,
		const TSharedPtr<FJsonValue>& Value)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		Params->SetStringField(TEXT("name"), Name);
		Params->SetStringField(TEXT("type"), Type);
		Params->SetField(TEXT("value"), Value);
		return Params;
	};

	auto ColorValue = [](const float R, const float G, const float B, const float A)
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("r"), R);
		Object->SetNumberField(TEXT("g"), G);
		Object->SetNumberField(TEXT("b"), B);
		Object->SetNumberField(TEXT("a"), A);
		return MakeShared<FJsonValueObject>(Object);
	};

	const FMCPToolResult Added = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.add"),
		MakeAddParams(TEXT("AddedColor"), TEXT("color"), ColorValue(1.0f, 0.5f, 0.25f, 0.75f)));
	if (!TestTrue(TEXT("Color user parameter add succeeds"), Added.bSuccess)
		|| !TestNotNull(TEXT("Add returns data"), Added.Data.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Add echoes the User.-prefixed name"),
		Added.Data->GetStringField(TEXT("parameter")), FString(TEXT("User.AddedColor")));
	TestFalse(TEXT("Add never reports saved"), Added.Data->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("Add dirties the package"), Fixture.Package->IsDirty());

	const FNiagaraVariable AddedVar(
		FNiagaraTypeDefinition::GetColorDef(), FName(TEXT("User.AddedColor")));
	const FLinearColor StoreColor =
		Fixture.System->GetExposedParameters().GetParameterValue<FLinearColor>(AddedVar);
	TestEqual(TEXT("Color store receives R"), StoreColor.R, 1.0f);
	TestEqual(TEXT("Color store receives G"), StoreColor.G, 0.5f);
	TestEqual(TEXT("Color store receives B"), StoreColor.B, 0.25f);
	TestEqual(TEXT("Color store receives A"), StoreColor.A, 0.75f);

	// The authored ScriptVariable default must also receive the value so it
	// survives recompiles and re-syncs.
	UNiagaraSystemEditorData* EditorData =
		Cast<UNiagaraSystemEditorData>(Fixture.System->GetEditorData());
	UNiagaraScriptVariable* ScriptVariable = EditorData
		? EditorData->FindOrAddUserScriptVariable(AddedVar, *Fixture.System)
		: nullptr;
	if (TestNotNull(TEXT("Added parameter has authored metadata"), ScriptVariable)
		&& ScriptVariable)
	{
		TestEqual(TEXT("Added parameter default mode is inline value"),
			static_cast<int32>(ScriptVariable->DefaultMode),
			static_cast<int32>(ENiagaraDefaultMode::Value));
		FLinearColor ScriptColor;
		FMemory::Memcpy(&ScriptColor, ScriptVariable->GetDefaultValueData(), sizeof(FLinearColor));
		TestEqual(TEXT("Authored metadata receives R"), ScriptColor.R, 1.0f);
		TestEqual(TEXT("Authored metadata receives A"), ScriptColor.A, 0.75f);
	}

	// Duplicate names are rejected case-insensitively and never mutate the store.
	const FMCPToolResult Duplicate = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.add"),
		MakeAddParams(TEXT("addedcolor"), TEXT("float"), MakeShared<FJsonValueNumber>(1.0)));
	TestFalse(TEXT("Duplicate parameter name is rejected"), Duplicate.bSuccess);
	TestEqual(TEXT("Duplicate parameter uses a stable code"),
		Duplicate.ErrorCode, FString(TEXT("parameter_exists")));

	// Unsupported types are rejected before any store mutation.
	const FMCPToolResult Unsupported = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.add"),
		MakeAddParams(TEXT("AddedMatrix"), TEXT("matrix"), MakeShared<FJsonValueNull>()));
	TestFalse(TEXT("Unsupported parameter type is rejected"), Unsupported.bSuccess);
	TestEqual(TEXT("Unsupported parameter type uses a stable code"),
		Unsupported.ErrorCode, FString(TEXT("parameter_type_unsupported")));

	// A value that does not match the requested type is rejected.
	const FMCPToolResult BadValue = Registry.ExecuteTool(
		TEXT("content.niagara.system.parameter.add"),
		MakeAddParams(TEXT("AddedFloat"), TEXT("float"), ColorValue(1.0f, 0.0f, 0.0f, 1.0f)));
	TestFalse(TEXT("Mismatched parameter value is rejected"), BadValue.bSuccess);
	TestEqual(TEXT("Mismatched parameter value uses a stable code"),
		BadValue.ErrorCode, FString(TEXT("parameter_value_invalid")));
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
