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
void RegisterNiagaraDynamicInputTools(FMCPToolRegistry& Registry);
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

// A mounted Dynamic Input is itself a stack function call.  Use the same
// engine input enumeration as the command under test so the nested edit is
// driven through a real authored input rather than a guessed pin name.
bool FindNiagaraModuleInputDINestedLinkable(
	UNiagaraSystem* System,
	const FString& EmitterName,
	UNiagaraNodeFunctionCall* DynamicNode,
	FNiagaraVariable& OutInput)
{
	if (!System || !DynamicNode)
	{
		return false;
	}
	const ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
	FCompileConstantResolver ConstantResolver(System, Usage);
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (Handle.GetName().ToString().Equals(EmitterName, ESearchCase::CaseSensitive))
		{
			ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
			break;
		}
	}
	TArray<FNiagaraVariable> Inputs;
	TSet<FNiagaraVariable> HiddenInputs;
	FNiagaraStackGraphUtilities::GetStackFunctionInputs(
		*DynamicNode,
		Inputs,
		HiddenInputs,
		ConstantResolver,
		FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
	TArray<UEdGraphPin*> StaticSwitchPins;
	TSet<UEdGraphPin*> HiddenSwitchPins;
	FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
		*DynamicNode, StaticSwitchPins, HiddenSwitchPins, ConstantResolver);
	for (const FNiagaraVariable& Candidate : Inputs)
	{
		if (Candidate.IsDataInterface() || Candidate.IsUObject())
		{
			continue;
		}
		bool bStaticSwitch = false;
		for (UEdGraphPin* Pin : StaticSwitchPins)
		{
			if (Pin && Pin->GetFName() == Candidate.GetName())
			{
				bStaticSwitch = true;
				break;
			}
		}
		if (!bStaticSwitch)
		{
			OutInput = Candidate;
			return true;
		}
	}
	return false;
}

FString NiagaraModuleInputDIShortNameFromVariable(const FNiagaraVariable& Input)
{
	return NiagaraModuleInputDIShortName(Input.GetName());
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
	UEAIIntegrationTools::RegisterNiagaraDynamicInputTools(Registry);
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

	{
		auto SearchParams = MakeShared<FJsonObject>();
		SearchParams->SetStringField(TEXT("query"), DynamicInputScript->GetName());
		SearchParams->SetNumberField(TEXT("limit"), 16);
		const FMCPToolResult SearchResult = Registry.ExecuteTool(
			TEXT("content.niagara.graph.dynamic_input.search"), SearchParams);
		TestTrue(TEXT("Dynamic input search succeeds"), SearchResult.bSuccess);
		bool bFoundScript = false;
		if (SearchResult.Data)
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (SearchResult.Data->TryGetArrayField(TEXT("dynamicInputs"), Rows) && Rows)
			{
				for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
				{
					const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
					FString ObjectPath;
					if (Row.IsValid() && Row->TryGetStringField(TEXT("path"), ObjectPath)
						&& ObjectPath.Equals(DynamicInputScript->GetPathName(), ESearchCase::IgnoreCase))
					{
						bFoundScript = true;
						break;
					}
				}
			}
		}
		TestTrue(TEXT("Dynamic input search returns the mounted script asset"), bFoundScript);
	}

	// Independent read-back: the override pin is now linked to a dynamic-input
	// function-call node.
	const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(Linkable.GetName()), Added);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Added, Aliased, Linkable.GetType(), FGuid(), FGuid());
	UNiagaraNodeFunctionCall* RootDynamicInputNode = nullptr;
	bool bDIReadBack = false;
	if (OverridePin.LinkedTo.Num() == 1 && OverridePin.LinkedTo[0] != nullptr)
	{
		RootDynamicInputNode =
			Cast<UNiagaraNodeFunctionCall>(OverridePin.LinkedTo[0]->GetOwningNodeUnchecked());
		bDIReadBack = RootDynamicInputNode != nullptr
			&& RootDynamicInputNode->FunctionScript != nullptr
			&& RootDynamicInputNode->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput;
	}
	TestTrue(TEXT("Override pin read-back is linked to a dynamic-input node"), bDIReadBack);

	// Exercise the complete mounted-Dynamic-Input query/edit surface against
	// the same isolated authored graph.  The list/tree calls must expose the
	// stable node GUID used by value.get and remove; no result here is accepted
	// merely because the transport returned ok=true.
	auto MakeMountedDynamicInputParams = [&]()
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		Params->SetStringField(TEXT("emitter"), Fixture.EmitterName);
		Params->SetStringField(TEXT("outputNodePath"), ParticleUpdatePath);
		Params->SetStringField(TEXT("moduleSelector"), ModuleScript->GetPathName());
		return Params;
	};
	FString MountedDynamicInputGuid;
	FString MountedNestedDynamicInputGuid;
	{
		const FMCPToolResult ListResult = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.dynamic_inputs.list"),
			MakeMountedDynamicInputParams());
		TestTrue(TEXT("Mounted dynamic input list succeeds"), ListResult.bSuccess);
		if (ListResult.Data)
		{
			const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
			if (ListResult.Data->TryGetArrayField(TEXT("dynamicInputs"), Entries)
				&& Entries && Entries->Num() > 0)
			{
				const TSharedPtr<FJsonObject> Entry = (*Entries)[0].IsValid()
					? (*Entries)[0]->AsObject() : nullptr;
				if (Entry.IsValid())
				{
					Entry->TryGetStringField(TEXT("dynamicInputGuid"), MountedDynamicInputGuid);
				}
			}
		}
	}
	TestTrue(TEXT("Mounted dynamic input list returns a stable GUID"), !MountedDynamicInputGuid.IsEmpty());
	if (!MountedDynamicInputGuid.IsEmpty())
	{
		const FMCPToolResult TreeResult = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.dynamic_inputs.tree"),
			MakeMountedDynamicInputParams());
		TestTrue(TEXT("Mounted dynamic input tree succeeds"), TreeResult.bSuccess);
		if (TreeResult.Data)
		{
			const TArray<TSharedPtr<FJsonValue>>* Roots = nullptr;
			bool bGuidFoundInTree = false;
			if (TreeResult.Data->TryGetArrayField(TEXT("dynamicInputs"), Roots) && Roots)
			{
				for (const TSharedPtr<FJsonValue>& RootValue : *Roots)
				{
					const TSharedPtr<FJsonObject> Root = RootValue.IsValid() ? RootValue->AsObject() : nullptr;
					FString RootGuid;
					if (Root.IsValid() && Root->TryGetStringField(TEXT("dynamicInputGuid"), RootGuid)
						&& RootGuid.Equals(MountedDynamicInputGuid, ESearchCase::IgnoreCase))
					{
						bGuidFoundInTree = true;
						break;
					}
				}
			}
			TestTrue(TEXT("Dynamic input tree preserves the list GUID"), bGuidFoundInTree);
		}

		const FMCPToolResult ScriptInputs = Registry.ExecuteTool(
			TEXT("content.niagara.graph.dynamic_input.inputs.get"),
			MakeShared<FJsonObject>());
		// This intentionally validates the stable error contract for a malformed
		// query before issuing value.get; the latter needs a script sub-input
		// name that varies across Niagara engine versions.
		TestFalse(TEXT("Dynamic input inputs query rejects a missing script path"), ScriptInputs.bSuccess);
		TestEqual(TEXT("Missing dynamic input script uses the required-field code"),
			ScriptInputs.ErrorCode, FString(TEXT("script_path_required")));
		auto ValidScriptParams = MakeShared<FJsonObject>();
		ValidScriptParams->SetStringField(TEXT("scriptPath"), DynamicInputScript->GetPathName());
		const FMCPToolResult ValidScriptInputs = Registry.ExecuteTool(
			TEXT("content.niagara.graph.dynamic_input.inputs.get"), ValidScriptParams);
		TestTrue(TEXT("Dynamic input script inputs query succeeds"), ValidScriptInputs.bSuccess);
		if (ValidScriptInputs.Data)
			TestEqual(TEXT("Dynamic input inputs query identifies the script"),
				ValidScriptInputs.Data->GetStringField(TEXT("scriptPath")), DynamicInputScript->GetPathName());

	}

	// Exercise a second level when the mounted script exposes a compatible
	// authored input. The setter receives the root GUID as a scope and resolves
	// the child beneath it, proving complete-tree edits stay module-local.
	if (RootDynamicInputNode && !MountedDynamicInputGuid.IsEmpty())
	{
		FNiagaraVariable NestedInput;
		if (FindNiagaraModuleInputDINestedLinkable(
			Fixture.System, Fixture.EmitterName, RootDynamicInputNode, NestedInput))
		{
			UNiagaraScript* NestedScript = FindNiagaraModuleInputDIScriptForType(NestedInput.GetType());
			if (NestedScript)
			{
				auto NestedSetParams = MakeNiagaraModuleInputDIParams(
					Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
					NiagaraModuleInputDIShortNameFromVariable(NestedInput),
					NestedScript->GetPathName());
				NestedSetParams->SetStringField(TEXT("dynamicInputGuid"), MountedDynamicInputGuid);
				const FMCPToolResult NestedSet = Registry.ExecuteTool(
					TEXT("content.niagara.graph.module.input.di.set"), NestedSetParams);
				if (TestTrue(TEXT("Nested Dynamic Input set succeeds"), NestedSet.bSuccess))
				{
					const FMCPToolResult NestedValue = Registry.ExecuteTool(
						TEXT("content.niagara.graph.module.dynamic_input.value.get"), NestedSetParams);
					if (TestTrue(TEXT("Nested Dynamic Input value read succeeds"), NestedValue.bSuccess)
						&& NestedValue.Data)
					{
						TestEqual(TEXT("Nested Dynamic Input value reports a dynamic source"),
							NestedValue.Data->GetStringField(TEXT("source")), FString(TEXT("dynamicInput")));
						TestEqual(TEXT("Nested Dynamic Input value reports the child script"),
							NestedValue.Data->GetStringField(TEXT("nestedDynamicInputPath")),
							NestedScript->GetPathName());
					}

					// Discover the child GUID from the complete tree and remove only
					// that child. The root remains mounted, proving subtree removal
					// does not accidentally clear the enclosing module input.
					const FMCPToolResult NestedTree = Registry.ExecuteTool(
						TEXT("content.niagara.graph.module.dynamic_inputs.tree"),
						MakeMountedDynamicInputParams());
					FString NestedGuid;
					if (NestedTree.Data)
					{
						const TArray<TSharedPtr<FJsonValue>>* Roots = nullptr;
						if (NestedTree.Data->TryGetArrayField(TEXT("dynamicInputs"), Roots) && Roots)
						{
							for (const TSharedPtr<FJsonValue>& RootValue : *Roots)
							{
								const TSharedPtr<FJsonObject> Root = RootValue.IsValid()
									? RootValue->AsObject() : nullptr;
								if (!Root.IsValid()
									|| !Root->GetStringField(TEXT("dynamicInputGuid"))
										.Equals(MountedDynamicInputGuid, ESearchCase::IgnoreCase))
								{
									continue;
								}
								const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
								if (Root->TryGetArrayField(TEXT("children"), Children) && Children
									&& Children->Num() > 0)
								{
									const TSharedPtr<FJsonObject> Child = (*Children)[0].IsValid()
										? (*Children)[0]->AsObject() : nullptr;
									if (Child.IsValid())
										Child->TryGetStringField(TEXT("dynamicInputGuid"), NestedGuid);
								}
								break;
							}
						}
					}
					if (TestTrue(TEXT("Nested tree exposes a child GUID"), !NestedGuid.IsEmpty()))
					{
						MountedNestedDynamicInputGuid = NestedGuid;
					}
				}
			}
			else
			{
				AddInfo(TEXT("No type-compatible child Dynamic Input script was found; nested-tree edit coverage was skipped."));
			}
		}
		else
		{
			AddInfo(TEXT("The mounted Dynamic Input exposes no linkable child input; nested-tree edit coverage was skipped."));
		}
	}

	// The authored System Spec exporter must carry the mounted Dynamic Input
	// tree, and importing that unchanged tree must preserve its node identity.
	// This exercises the full spec path independently of the direct DI setter.
	auto SpecExportParams = MakeShared<FJsonObject>();
	SpecExportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	const FMCPToolResult SpecExport = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.export"), SpecExportParams);
	if (TestTrue(TEXT("System spec export includes the mounted dynamic input"), SpecExport.bSuccess)
		&& SpecExport.Data)
	{
		FString ExportedDynamicGuid;
		FString ExportedTreeGuid;
		FString ExportedNestedGuid;
		bool bFoundDynamicTree = false;
		bool bFoundNestedTree = false;
		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (SpecExport.Data->TryGetArrayField(TEXT("emitters"), Emitters) && Emitters)
		{
			for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
			{
				const TSharedPtr<FJsonObject> Emitter = EmitterValue.IsValid() ? EmitterValue->AsObject() : nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Stacks = nullptr;
				if (!Emitter.IsValid() || !Emitter->TryGetArrayField(TEXT("stacks"), Stacks) || !Stacks) continue;
				for (const TSharedPtr<FJsonValue>& StackValue : *Stacks)
				{
					const TSharedPtr<FJsonObject> Stack = StackValue.IsValid() ? StackValue->AsObject() : nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
					if (!Stack.IsValid() || !Stack->TryGetArrayField(TEXT("modules"), Modules) || !Modules) continue;
					for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
					{
						const TSharedPtr<FJsonObject> Module = ModuleValue.IsValid() ? ModuleValue->AsObject() : nullptr;
						const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
						if (!Module.IsValid() || !Module->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs) continue;
						for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
						{
							const TSharedPtr<FJsonObject> Input = InputValue.IsValid() ? InputValue->AsObject() : nullptr;
							const TSharedPtr<FJsonObject>* Tree = nullptr;
							if (!Input.IsValid() || !Input->TryGetObjectField(TEXT("dynamicInputTree"), Tree)
								|| !Tree || !Tree->IsValid()) continue;
							Input->TryGetStringField(TEXT("dynamicInputGuid"), ExportedDynamicGuid);
							(*Tree)->TryGetStringField(TEXT("guid"), ExportedTreeGuid);
							bFoundDynamicTree = !ExportedDynamicGuid.IsEmpty() && ExportedDynamicGuid == ExportedTreeGuid;
							const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
							if ((*Tree)->TryGetArrayField(TEXT("children"), Children) && Children)
							{
								for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
								{
									const TSharedPtr<FJsonObject> Child = ChildValue.IsValid()
										? ChildValue->AsObject() : nullptr;
									if (Child.IsValid() && Child->TryGetStringField(TEXT("guid"), ExportedNestedGuid)
										&& !ExportedNestedGuid.IsEmpty())
									{
										bFoundNestedTree = true;
										break;
									}
								}
							}
							break;
						}
						if (bFoundDynamicTree) break;
					}
					if (bFoundDynamicTree) break;
				}
				if (bFoundDynamicTree) break;
			}
		}
		TestTrue(TEXT("System spec preserves the dynamic input GUID in its tree root"), bFoundDynamicTree);
		if (!MountedNestedDynamicInputGuid.IsEmpty())
		{
			TestTrue(TEXT("System spec preserves the nested dynamic input tree"), bFoundNestedTree);
			if (bFoundNestedTree)
			{
				TestEqual(TEXT("System spec preserves the nested dynamic input GUID"),
					ExportedNestedGuid, MountedNestedDynamicInputGuid);
			}
		}
		if (bFoundDynamicTree)
		{
			const FString OriginalDynamicSpecDigest = SpecExport.Data->GetStringField(TEXT("specDigest"));
			auto SpecImportParams = MakeShared<FJsonObject>();
			SpecImportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
			SpecImportParams->SetObjectField(TEXT("spec"), SpecExport.Data);
			SpecImportParams->SetStringField(TEXT("requestId"), TEXT("module-input-di-system-spec-round-trip"));
			SpecImportParams->SetBoolField(TEXT("confirmWrite"), true);
			const FMCPToolResult SpecImport = Registry.ExecuteTool(
				TEXT("content.niagara.system.spec.import"), SpecImportParams);
			TestTrue(TEXT("System spec imports a mounted dynamic input tree"), SpecImport.bSuccess);
			if (!SpecImport.bSuccess)
			{
				AddError(FString::Printf(
					TEXT("System spec import failed: code=%s status=%d message=%s"),
					*SpecImport.ErrorCode,
					SpecImport.HttpStatus,
					*SpecImport.ErrorMessage));
			}
			if (SpecImport.Data)
				TestTrue(TEXT("System spec dynamic tree import verifies readback"), SpecImport.Data->GetBoolField(TEXT("readbackVerified")));
			if (SpecImport.bSuccess)
			{
				auto VerifySpecExportParams = MakeShared<FJsonObject>();
				VerifySpecExportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
				const FMCPToolResult VerifySpecExport = Registry.ExecuteTool(
					TEXT("content.niagara.system.spec.export"), VerifySpecExportParams);
				TestTrue(TEXT("System spec export after dynamic tree import succeeds"), VerifySpecExport.bSuccess);
				if (VerifySpecExport.Data)
				{
					TestEqual(
						TEXT("Unchanged dynamic tree import preserves the canonical spec digest"),
						VerifySpecExport.Data->GetStringField(TEXT("specDigest")),
						OriginalDynamicSpecDigest);
				}
			}
		}
	}

	// Remove the nested node only after spec export/import has verified that its
	// GUID and child tree survive a round trip. The enclosing root must remain.
	if (!MountedNestedDynamicInputGuid.IsEmpty())
	{
		auto NestedRemoveParams = MakeMountedDynamicInputParams();
		NestedRemoveParams->SetStringField(TEXT("dynamicInputGuid"), MountedNestedDynamicInputGuid);
		const FMCPToolResult NestedRemove = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.dynamic_input.remove"), NestedRemoveParams);
		TestTrue(TEXT("Nested Dynamic Input remove succeeds"), NestedRemove.bSuccess);
		const FMCPToolResult RootAfterRemove = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.dynamic_inputs.list"),
			MakeMountedDynamicInputParams());
		TestTrue(TEXT("Root Dynamic Input remains after nested remove"), RootAfterRemove.bSuccess
			&& RootAfterRemove.Data
			&& RootAfterRemove.Data->GetIntegerField(TEXT("dynamicInputCount")) == 1);
	}

	// Remove only after the spec round-trip assertions above have observed the
	// mounted tree.  The read-back must prove that the GUID is gone as well as
	// returning a successful mutation result.
	if (!MountedDynamicInputGuid.IsEmpty())
	{
		auto RemoveParams = MakeMountedDynamicInputParams();
		RemoveParams->SetStringField(TEXT("dynamicInputGuid"), MountedDynamicInputGuid);
		RemoveParams->SetBoolField(TEXT("confirmWrite"), true);
		const FMCPToolResult RemoveResult = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.dynamic_input.remove"), RemoveParams);
		TestTrue(TEXT("Mounted dynamic input remove succeeds"), RemoveResult.bSuccess);
		if (RemoveResult.bSuccess)
		{
			const FMCPToolResult VerifyList = Registry.ExecuteTool(
				TEXT("content.niagara.graph.module.dynamic_inputs.list"),
				MakeMountedDynamicInputParams());
			TestTrue(TEXT("Mounted dynamic input list succeeds after remove"), VerifyList.bSuccess);
			if (VerifyList.Data)
				TestEqual(TEXT("Removed dynamic input no longer appears in list"),
					VerifyList.Data->GetNumberField(TEXT("dynamicInputCount")), 0.0);
		}
	}
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
