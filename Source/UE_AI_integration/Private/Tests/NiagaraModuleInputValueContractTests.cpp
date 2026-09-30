// Contract tests for Niagara stack module input inline value authoring
// (content.niagara.graph.module.input.value.plan / .apply / .rollback).
//
// The write path resolves a stack module and one of its authored inputs via
// the exported FNiagaraStackGraphUtilities::GetStackFunctionInputs /
// GetStackFunctionStaticSwitchPins, and writes the value through
// GetOrCreateStackFunctionInputOverridePin. It never saves and never assumes a
// runtime/PIE session. The round-trip needs a real Niagara Module script; the
// test locates one at runtime and skips with AddInfo when none is available,
// mirroring NiagaraModuleInputContractTests.cpp. All helper names in the
// anonymous namespace are prefixed with ModuleInputValue to avoid unity-build
// collisions with the other Niagara contract test files.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FModuleInputValueFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FString EmitterName;
	UNiagaraGraph* Graph = nullptr;
};

bool CreateModuleInputValueFixture(FModuleInputValueFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_ModuleInputValue_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(OutFixture.PackageName);
	OutFixture.Package = CreatePackage(*OutFixture.PackageName);
	OutFixture.System = OutFixture.Package
		? NewObject<UNiagaraSystem>(
			OutFixture.Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!OutFixture.System)
	{
		return false;
	}
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);

	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		OutFixture.System, TEXT("EmptyEmitter"), RF_Transactional);
	if (!Emitter)
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FNiagaraEmitterHandle Handle(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	OutFixture.System->AddEmitterHandleDirect(Handle);
	OutFixture.EmitterName = Handle.GetName().ToString();

	FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
	if (!Data)
	{
		return false;
	}
	OutFixture.Graph = Cast<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph;
	OutFixture.Package->SetDirtyFlag(false);
	return OutFixture.Graph != nullptr;
}

bool DeleteModuleInputValueFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

UNiagaraNodeOutput* FindModuleInputValueOutput(
	UNiagaraGraph* Graph,
	const ENiagaraScriptUsage Usage)
{
	if (!Graph)
	{
		return nullptr;
	}
	TArray<UNiagaraNodeOutput*> Outputs;
	Graph->GetNodesOfClass(Outputs);
	for (UNiagaraNodeOutput* Output : Outputs)
	{
		if (Output && Output->GetUsage() == Usage)
		{
			return Output;
		}
	}
	return nullptr;
}

UNiagaraScript* FindModuleInputValueScript()
{
	UNiagaraScript* Script = LoadObject<UNiagaraScript>(
		nullptr,
		TEXT("/Niagara/Modules/Update/Lifetime/UpdateAge.UpdateAge"));
	if (Script && Script->GetUsage() == ENiagaraScriptUsage::Module)
	{
		return Script;
	}
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);
	for (const FAssetData& Asset : Assets)
	{
		UNiagaraScript* Candidate = Cast<UNiagaraScript>(Asset.GetAsset());
		if (Candidate && Candidate->GetUsage() == ENiagaraScriptUsage::Module)
		{
			return Candidate;
		}
	}
	return nullptr;
}

bool ModuleInputValueTypeSettable(const FNiagaraTypeDefinition& Type)
{
	return Type == FNiagaraTypeDefinition::GetFloatDef()
		|| Type == FNiagaraTypeDefinition::GetIntDef()
		|| Type == FNiagaraTypeDefinition::GetBoolDef()
		|| Type == FNiagaraTypeDefinition::GetVec2Def()
		|| Type == FNiagaraTypeDefinition::GetVec3Def()
		|| Type == FNiagaraTypeDefinition::GetPositionDef()
		|| Type == FNiagaraTypeDefinition::GetVec4Def()
		|| Type == FNiagaraTypeDefinition::GetQuatDef()
		|| Type == FNiagaraTypeDefinition::GetColorDef();
}

TSharedPtr<FJsonValue> ModuleInputValueTestValue(const FNiagaraTypeDefinition& Type)
{
	if (Type == FNiagaraTypeDefinition::GetFloatDef())
	{
		return MakeShared<FJsonValueNumber>(2.5);
	}
	if (Type == FNiagaraTypeDefinition::GetIntDef())
	{
		return MakeShared<FJsonValueNumber>(7.0);
	}
	if (Type == FNiagaraTypeDefinition::GetBoolDef())
	{
		return MakeShared<FJsonValueBoolean>(true);
	}
	if (Type == FNiagaraTypeDefinition::GetVec2Def())
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), 1.0);
		Object->SetNumberField(TEXT("y"), 2.0);
		return MakeShared<FJsonValueObject>(Object);
	}
	if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), 1.0);
		Object->SetNumberField(TEXT("y"), 2.0);
		Object->SetNumberField(TEXT("z"), 3.0);
		return MakeShared<FJsonValueObject>(Object);
	}
	if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetQuatDef())
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("x"), 1.0);
		Object->SetNumberField(TEXT("y"), 2.0);
		Object->SetNumberField(TEXT("z"), 3.0);
		Object->SetNumberField(TEXT("w"), 4.0);
		return MakeShared<FJsonValueObject>(Object);
	}
	if (Type == FNiagaraTypeDefinition::GetColorDef())
	{
		auto Object = MakeShared<FJsonObject>();
		Object->SetNumberField(TEXT("r"), 0.1);
		Object->SetNumberField(TEXT("g"), 0.2);
		Object->SetNumberField(TEXT("b"), 0.3);
		Object->SetNumberField(TEXT("a"), 1.0);
		return MakeShared<FJsonValueObject>(Object);
	}
	return nullptr;
}

FString ModuleInputValueShortName(const FName& FullName)
{
	FString Name = FullName.ToString();
	Name.RemoveFromStart(TEXT("Module."));
	return Name;
}

TSharedRef<FJsonObject> MakeModuleInputValueParams(
	const FModuleInputValueFixture& Fixture,
	const FString& OutputNodePath,
	const FString& ModuleSelector)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterName);
	Params->SetStringField(TEXT("outputNodePath"), OutputNodePath);
	Params->SetStringField(TEXT("moduleSelector"), ModuleSelector);
	return Params;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraModuleInputValueContractTest,
	"UE_AI_integration.Niagara.ModuleInputValueContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleInputValueContractTest::RunTest(const FString&)
{
	UNiagaraScript* ModuleScript = FindModuleInputValueScript();
	if (!ModuleScript || !ModuleScript->GetLatestSource())
	{
		AddInfo(TEXT("No Niagara Module script asset was found; skipping the module input value round trip (requires a real Module asset)."));
		return true;
	}

	FModuleInputValueFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Module-input-value fixture and package are deleted"),
			DeleteModuleInputValueFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Module-input-value fixture builds"), CreateModuleInputValueFixture(Fixture))
		|| !TestNotNull(TEXT("Module-input-value system"), Fixture.System)
		|| !TestNotNull(TEXT("Module-input-value graph"), Fixture.Graph))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	TestNotNull(TEXT("Input-value plan tool registers"),
		Registry.FindTool(TEXT("content.niagara.graph.module.input.value.plan")));
	TestNotNull(TEXT("Input-value apply tool registers"),
		Registry.FindTool(TEXT("content.niagara.graph.module.input.value.apply")));
	TestNotNull(TEXT("Input-value rollback tool registers"),
		Registry.FindTool(TEXT("content.niagara.graph.module.input.value.rollback")));

	UNiagaraNodeOutput* Output = FindModuleInputValueOutput(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestNotNull(TEXT("Particle-update output resolves"), Output))
	{
		return false;
	}
	const FString ParticleUpdatePath = Output->GetPathName();

	UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *Output);
	if (!TestNotNull(TEXT("Module added to the stack"), Added))
	{
		AddInfo(TEXT("AddScriptModuleToStack returned no node; skipping the input value round trip."));
		return true;
	}

	// Enumerate inputs and static switches through the same exported primitives
	// the command uses.
	const ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
	FCompileConstantResolver ConstantResolver(Fixture.System, Usage);
	for (const FNiagaraEmitterHandle& Handle : Fixture.System->GetEmitterHandles())
	{
		if (Handle.GetName().ToString().Equals(Fixture.EmitterName, ESearchCase::CaseSensitive))
		{
			ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
			break;
		}
	}

	TArray<FNiagaraVariable> InputVariables;
	TSet<FNiagaraVariable> HiddenVariables;
	FNiagaraStackGraphUtilities::GetStackFunctionInputs(
		*Added, InputVariables, HiddenVariables, ConstantResolver,
		FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

	TArray<UEdGraphPin*> StaticSwitchPins;
	TSet<UEdGraphPin*> HiddenSwitchPins;
	FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
		*Added, StaticSwitchPins, HiddenSwitchPins, ConstantResolver);

	// (a) unknown input is rejected with the stable input_not_found code.
	{
		auto Params = MakeModuleInputValueParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName());
		Params->SetStringField(TEXT("input"), TEXT("DefinitelyDoesNotExist"));
		Params->SetNumberField(TEXT("value"), 1.0);
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.input.value.plan"), Params);
		TestFalse(TEXT("Unknown input is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown input uses the stable input_not_found code"),
			Result.ErrorCode, FString(TEXT("input_not_found")));
	}

	// (b) data-interface and static-switch inputs are rejected with
	// input_type_unsupported before value validation.
	{
		bool bCheckedUnsupported = false;
		for (const FNiagaraVariable& Input : InputVariables)
		{
			if (!Input.IsDataInterface() && !Input.IsUObject())
			{
				continue;
			}
			auto Params = MakeModuleInputValueParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName());
			Params->SetStringField(TEXT("input"), ModuleInputValueShortName(Input.GetName()));
			Params->SetNumberField(TEXT("value"), 1.0);
			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("content.niagara.graph.module.input.value.plan"), Params);
			TestFalse(TEXT("Data-interface input is rejected"), Result.bSuccess);
			TestEqual(TEXT("Data-interface input uses input_type_unsupported"),
				Result.ErrorCode, FString(TEXT("input_type_unsupported")));
			bCheckedUnsupported = true;
			break;
		}
		for (UEdGraphPin* SwitchPin : StaticSwitchPins)
		{
			if (!SwitchPin)
			{
				continue;
			}
			auto Params = MakeModuleInputValueParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName());
			Params->SetStringField(TEXT("input"), SwitchPin->GetFName().ToString());
			Params->SetBoolField(TEXT("value"), true);
			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("content.niagara.graph.module.input.value.plan"), Params);
			TestFalse(TEXT("Static-switch input is rejected"), Result.bSuccess);
			TestEqual(TEXT("Static-switch input uses input_type_unsupported"),
				Result.ErrorCode, FString(TEXT("input_type_unsupported")));
			bCheckedUnsupported = true;
			break;
		}
		if (!bCheckedUnsupported)
		{
			AddInfo(TEXT("The Module script has no data-interface or static-switch input; skipping the unsupported-type rejection check."));
		}
	}

	// (c) plan + apply + read back + rollback on a settable inline input.
	FNiagaraVariable Settable;
	bool bFoundSettable = false;
	for (const FNiagaraVariable& Input : InputVariables)
	{
		if (Input.IsDataInterface() || Input.IsUObject())
		{
			continue;
		}
		if (!ModuleInputValueTypeSettable(Input.GetType()))
		{
			continue;
		}
		bool bIsSwitch = false;
		for (UEdGraphPin* SwitchPin : StaticSwitchPins)
		{
			if (SwitchPin && SwitchPin->GetFName() == Input.GetName())
			{
				bIsSwitch = true;
				break;
			}
		}
		if (bIsSwitch)
		{
			continue;
		}
		Settable = Input;
		bFoundSettable = true;
		break;
	}
	if (!bFoundSettable)
	{
		AddInfo(TEXT("The Module script has no settable inline input; skipping the apply/rollback round trip."));
		return true;
	}

	const FString InputName = ModuleInputValueShortName(Settable.GetName());
	const TSharedPtr<FJsonValue> TestValue = ModuleInputValueTestValue(Settable.GetType());

	auto PlanParams = MakeModuleInputValueParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName());
	PlanParams->SetStringField(TEXT("input"), InputName);
	PlanParams->SetField(TEXT("value"), TestValue);
	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.input.value.plan"), PlanParams);
	if (!TestTrue(TEXT("Input-value plan succeeds"), Plan.bSuccess) || !Plan.Data)
	{
		AddInfo(TEXT("Input-value plan did not succeed on the synthetic system; skipping the apply/rollback round trip."));
		return true;
	}
	TestEqual(TEXT("Input-value plan uses the change-plan schema"),
		Plan.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.change-plan.v1")));
	TestEqual(TEXT("Input-value plan uses the module input value kind"),
		Plan.Data->GetStringField(TEXT("planKind")), FString(TEXT("niagaraModuleInputValue")));
	TestTrue(TEXT("Input-value plan requires write confirmation"),
		Plan.Data->GetBoolField(TEXT("confirmWriteRequired")));
	TestEqual(TEXT("Input-value plan is dirtyOnly"),
		Plan.Data->GetStringField(TEXT("persistence")), FString(TEXT("dirtyOnly")));
	TestEqual(TEXT("Input-value plan uses the same-Editor-instance rollback boundary"),
		Plan.Data->GetStringField(TEXT("rollbackBoundary")), FString(TEXT("sameEditorInstance")));
	const FString PlanDigest = Plan.Data->GetStringField(TEXT("planDigest"));
	TestFalse(TEXT("Input-value plan digest is present"), PlanDigest.IsEmpty());

	// Apply with approval.
	auto ApplyParams = MakeModuleInputValueParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName());
	ApplyParams->SetStringField(TEXT("input"), InputName);
	ApplyParams->SetField(TEXT("value"), TestValue);
	ApplyParams->SetStringField(TEXT("approvePlanDigest"), PlanDigest);
	ApplyParams->SetBoolField(TEXT("confirmWrite"), true);
	ApplyParams->SetStringField(TEXT("requestId"), TEXT("mv-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FMCPToolResult Applied = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.input.value.apply"), ApplyParams);
	if (!TestTrue(TEXT("Input-value apply succeeds"), Applied.bSuccess) || !Applied.Data)
	{
		AddInfo(TEXT("Input-value apply did not succeed on the synthetic system; skipping the rollback round trip (compile environment required)."));
		return true;
	}
	TestTrue(TEXT("Input-value apply reports verification"), Applied.Data->GetBoolField(TEXT("verified")));
	TestFalse(TEXT("Input-value apply never saves"), Applied.Data->GetBoolField(TEXT("saved")));
	TestEqual(TEXT("Input-value apply reports the input"),
		Applied.Data->GetStringField(TEXT("input")), InputName);

	// Read back the override pin through the exported override-pin API.
	const FString AfterValue = Applied.Data->GetStringField(TEXT("afterValue"));
	const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(Settable.GetName()), Added);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Added, Aliased, Settable.GetType(), FGuid(), FGuid());
	TestEqual(TEXT("Override pin read-back matches the applied value"),
		OverridePin.DefaultValue, AfterValue);

	const FString ReceiptId = Applied.Data->GetStringField(TEXT("receiptId"));
	const FString RequestId = Applied.Data->GetStringField(TEXT("requestId"));
	TestFalse(TEXT("Apply returns a receipt id"), ReceiptId.IsEmpty());

	// Rollback and verify restore.
	auto RollbackParams = MakeShared<FJsonObject>();
	RollbackParams->SetStringField(TEXT("rollbackId"), ReceiptId);
	RollbackParams->SetStringField(TEXT("requestId"), RequestId);
	RollbackParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult RolledBack = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.input.value.rollback"), RollbackParams);
	if (!TestTrue(TEXT("Input-value rollback succeeds"), RolledBack.bSuccess) || !RolledBack.Data)
	{
		return false;
	}
	TestTrue(TEXT("Input-value rollback reports the rollback"),
		RolledBack.Data->GetBoolField(TEXT("rolledBack")));
	TestTrue(TEXT("Input-value rollback reports the value restored"),
		RolledBack.Data->GetBoolField(TEXT("valueRestored")));

	// Independent restore verification: re-plan reports no override on a
	// freshly added module input.
	auto RePlanParams = MakeModuleInputValueParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName());
	RePlanParams->SetStringField(TEXT("input"), InputName);
	RePlanParams->SetField(TEXT("value"), TestValue);
	const FMCPToolResult RePlan = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.input.value.plan"), RePlanParams);
	if (TestTrue(TEXT("Re-plan after rollback succeeds"), RePlan.bSuccess) && RePlan.Data)
	{
		const TSharedPtr<FJsonObject> Before = RePlan.Data->GetObjectField(TEXT("before"));
		TestFalse(TEXT("Rollback removed the inline override"),
			Before.IsValid() && Before->GetBoolField(TEXT("overridePresent")));
	}
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
