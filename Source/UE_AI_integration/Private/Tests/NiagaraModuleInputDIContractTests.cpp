// Contract tests for the direct Niagara stack module input dynamic-input command
// (content.niagara.graph.module.input.di.set).
//
// The write path resolves a stack module and one of its authored inputs via
// ResolveModuleInput / FNiagaraStackGraphUtilities::GetStackFunctionInputs,
// loads the dynamic-input script and validates usage == DynamicInput, then sets
// it through FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput. It
// never saves and never assumes a runtime/PIE session. The round trip needs a
// real Niagara Module script and a real Dynamic Input script; the test locates
// them at runtime and skips with AddInfo when either is unavailable, mirroring
// NiagaraModuleInputBindingContractTests.cpp. All helper names in the anonymous
// namespace are prefixed with NiagaraModuleInputDI to avoid unity-build
// collisions with the other Niagara contract test files.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Misc/Optional.h"
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
struct FNiagaraModuleInputDIFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FString EmitterName;
	UNiagaraGraph* Graph = nullptr;
};

bool CreateNiagaraModuleInputDIFixture(FNiagaraModuleInputDIFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_ModuleInputDI_")
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

bool DeleteNiagaraModuleInputDIFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

UNiagaraNodeOutput* FindNiagaraModuleInputDIOutput(
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

UNiagaraScript* FindNiagaraModuleInputDIModuleScript()
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

TOptional<FNiagaraTypeDefinition> NiagaraModuleInputDIOutputType(UNiagaraScript* Script)
{
	if (!Script)
	{
		return TOptional<FNiagaraTypeDefinition>();
	}
	UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
	if (!Source || !Source->NodeGraph)
	{
		return TOptional<FNiagaraTypeDefinition>();
	}
	TArray<UNiagaraNodeOutput*> Outputs;
	Source->NodeGraph->GetNodesOfClass(Outputs);
	for (UNiagaraNodeOutput* Output : Outputs)
	{
		if (Output && Output->GetUsage() == ENiagaraScriptUsage::DynamicInput
			&& Output->GetOutputs().Num() > 0)
		{
			return TOptional<FNiagaraTypeDefinition>(Output->GetOutputs()[0].GetType());
		}
	}
	return TOptional<FNiagaraTypeDefinition>();
}

// Position is a flagged Vec3, so a Vec3 dynamic input can drive a Position input
// and vice versa.
bool NiagaraModuleInputDITypesCompatible(
	const FNiagaraTypeDefinition& A,
	const FNiagaraTypeDefinition& B)
{
	if (A == B)
	{
		return true;
	}
	const FNiagaraTypeDefinition& Position = FNiagaraTypeDefinition::GetPositionDef();
	const FNiagaraTypeDefinition& Vec3 = FNiagaraTypeDefinition::GetVec3Def();
	return (A == Position && B == Vec3) || (A == Vec3 && B == Position);
}

UNiagaraScript* FindNiagaraModuleInputDIScriptForType(const FNiagaraTypeDefinition& Type)
{
	// Fast path for common engine dynamic inputs.
	static const TCHAR* CommonPaths[] = {
		TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Integer.Add_Integer"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Vector2.Add_Vector2"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Vector.Add_Vector"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Vector4.Add_Vector4"),
		TEXT("/Niagara/DynamicInputs/Add/Add_LinearColor.Add_LinearColor"),
		TEXT("/Niagara/DynamicInputs/Clamp/ClampFloat.ClampFloat"),
		TEXT("/Niagara/DynamicInputs/Bool/InvertBool.InvertBool"),
	};
	for (const TCHAR* Path : CommonPaths)
	{
		UNiagaraScript* Candidate = LoadObject<UNiagaraScript>(nullptr, Path);
		if (!Candidate || Candidate->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			continue;
		}
		const TOptional<FNiagaraTypeDefinition> OutputType = NiagaraModuleInputDIOutputType(Candidate);
		if (OutputType.IsSet() && NiagaraModuleInputDITypesCompatible(OutputType.GetValue(), Type))
		{
			return Candidate;
		}
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
		if (!Candidate || Candidate->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			continue;
		}
		const TOptional<FNiagaraTypeDefinition> OutputType = NiagaraModuleInputDIOutputType(Candidate);
		if (OutputType.IsSet() && NiagaraModuleInputDITypesCompatible(OutputType.GetValue(), Type))
		{
			return Candidate;
		}
	}
	return nullptr;
}

FString NiagaraModuleInputDIShortName(const FName& FullName)
{
	FString Name = FullName.ToString();
	Name.RemoveFromStart(TEXT("Module."));
	return Name;
}

TSharedRef<FJsonObject> MakeNiagaraModuleInputDIParams(
	const FNiagaraModuleInputDIFixture& Fixture,
	const FString& OutputNodePath,
	const FString& ModuleSelector,
	const FString& Input,
	const FString& DynamicInput)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterName);
	Params->SetStringField(TEXT("outputNodePath"), OutputNodePath);
	Params->SetStringField(TEXT("moduleSelector"), ModuleSelector);
	Params->SetStringField(TEXT("input"), Input);
	Params->SetStringField(TEXT("dynamicInput"), DynamicInput);
	return Params;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraModuleInputDIContractTest,
	"UE_AI_integration.Niagara.ModuleInputDIContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleInputDIContractTest::RunTest(const FString&)
{
	FNiagaraModuleInputDIFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Module-input-DI fixture and package are deleted"),
			DeleteNiagaraModuleInputDIFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Module-input-DI fixture builds"), CreateNiagaraModuleInputDIFixture(Fixture))
		|| !TestNotNull(TEXT("Module-input-DI system"), Fixture.System)
		|| !TestNotNull(TEXT("Module-input-DI graph"), Fixture.Graph))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	TestNotNull(TEXT("Module-input-DI tool registers"),
		Registry.FindTool(TEXT("content.niagara.graph.module.input.di.set")));

	UNiagaraNodeOutput* Output = FindNiagaraModuleInputDIOutput(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestNotNull(TEXT("Particle-update output resolves"), Output))
	{
		return false;
	}
	const FString ParticleUpdatePath = Output->GetPathName();

	UNiagaraScript* ModuleScript = FindNiagaraModuleInputDIModuleScript();
	if (!ModuleScript || !ModuleScript->GetLatestSource())
	{
		AddInfo(TEXT("No Niagara Module script asset was found; skipping the module input dynamic-input round trip (requires a real Module asset)."));
		return true;
	}

	UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *Output);
	if (!TestNotNull(TEXT("Module added to the stack"), Added))
	{
		AddInfo(TEXT("AddScriptModuleToStack returned no node; skipping the module input dynamic-input round trip."));
		return true;
	}

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

	auto IsStaticSwitch = [&StaticSwitchPins](const FNiagaraVariable& Input) -> bool
	{
		for (UEdGraphPin* SwitchPin : StaticSwitchPins)
		{
			if (SwitchPin && SwitchPin->GetFName() == Input.GetName())
			{
				return true;
			}
		}
		return false;
	};

	// (a) unknown input is rejected with the stable input_not_found code.
	{
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.input.di.set"),
			MakeNiagaraModuleInputDIParams(
				Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
				TEXT("DefinitelyDoesNotExist"),
				TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float")));
		TestFalse(TEXT("Unknown input is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown input uses the stable input_not_found code"),
			Result.ErrorCode, FString(TEXT("input_not_found")));
	}

	// Pick the first linkable (non data-interface/object, non static-switch)
	// input to drive the dynamic-input checks.
	FNiagaraVariable Linkable;
	bool bFoundLinkable = false;
	for (const FNiagaraVariable& Input : InputVariables)
	{
		if (Input.IsDataInterface() || Input.IsUObject() || IsStaticSwitch(Input))
		{
			continue;
		}
		Linkable = Input;
		bFoundLinkable = true;
		break;
	}
	if (!bFoundLinkable)
	{
		AddInfo(TEXT("The Module script has no linkable input; skipping the dynamic-input checks."));
		return true;
	}
	const FString LinkableName = NiagaraModuleInputDIShortName(Linkable.GetName());

	// (b) an unknown or incorrect dynamic input is rejected with
	// dynamic_input_not_found.
	{
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.input.di.set"),
			MakeNiagaraModuleInputDIParams(
				Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
				LinkableName, TEXT("DefinitelyNotARealDynamicInput.Nope")));
		TestFalse(TEXT("Unknown dynamic input is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown dynamic input uses the stable dynamic_input_not_found code"),
			Result.ErrorCode, FString(TEXT("dynamic_input_not_found")));
	}

	// (c) set the linkable input to a type-compatible dynamic input and assert
	// the read-back dynamic-input node. Skipped when no matching dynamic input
	// script exists or the synthetic system cannot compile.
	UNiagaraScript* DynamicInputScript = FindNiagaraModuleInputDIScriptForType(Linkable.GetType());
	if (!DynamicInputScript)
	{
		AddInfo(TEXT("No type-compatible Niagara Dynamic Input script was found; skipping the set dynamic input round trip."));
		return true;
	}

	const FMCPToolResult SetResult = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.input.di.set"),
		MakeNiagaraModuleInputDIParams(
			Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
			LinkableName, DynamicInputScript->GetPathName()));
	if (!TestTrue(TEXT("Module input dynamic input set succeeds"), SetResult.bSuccess) || !SetResult.Data)
	{
		AddInfo(TEXT("Module input dynamic input did not succeed on the synthetic system; skipping the read-back round trip (compile environment required)."));
		return true;
	}
	TestEqual(TEXT("Module input dynamic input uses the DI schema"),
		SetResult.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.niagara.module-input-di.v1")));
	TestEqual(TEXT("Module input dynamic input reports the input"),
		SetResult.Data->GetStringField(TEXT("input")), LinkableName);
	TestEqual(TEXT("Module input dynamic input reports the dynamic input path"),
		SetResult.Data->GetStringField(TEXT("dynamicInputPath")), DynamicInputScript->GetPathName());
	TestFalse(TEXT("Module input dynamic input never saves"), SetResult.Data->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("Module input dynamic input reports compiled"), SetResult.Data->GetBoolField(TEXT("compiled")));

	// Independent read-back: the override pin is now linked to a dynamic-input
	// function-call node.
	const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(Linkable.GetName()), Added);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Added, Aliased, Linkable.GetType(), FGuid(), FGuid());
	bool bDIReadBack = false;
	if (OverridePin.LinkedTo.Num() == 1 && OverridePin.LinkedTo[0] != nullptr)
	{
		UNiagaraNodeFunctionCall* DynamicInputNode =
			Cast<UNiagaraNodeFunctionCall>(OverridePin.LinkedTo[0]->GetOwningNodeUnchecked());
		bDIReadBack = DynamicInputNode != nullptr
			&& DynamicInputNode->FunctionScript != nullptr
			&& DynamicInputNode->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput;
	}
	TestTrue(TEXT("Override pin read-back is linked to a dynamic-input node"), bDIReadBack);
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
