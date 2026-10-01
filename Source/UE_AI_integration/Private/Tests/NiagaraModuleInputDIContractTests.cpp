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
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
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
#include "NiagaraUserRedirectionParameterStore.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
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
	// Avoid loading the optional RequiredSystemUpdate module in the isolated
	// HostProject.  It is unrelated to module-input DI authoring and may emit
	// serialization errors when only a subset of Niagara content is mounted.
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
		TEXT("/Niagara/Modules/Update/Lifetime/UpdateAge.UpdateAge"),
		nullptr,
		LOAD_NoWarn);
	if (Script && Script->GetUsage() == ENiagaraScriptUsage::Module)
	{
		return Script;
	}
	// Do not fall back to loading every Niagara script in the AssetRegistry.
	// Minimal HostProjects often mount only a subset of the engine content; a
	// broad GetAsset() sweep then emits load errors for unrelated default assets
	// and turns an otherwise valid capability skip into a failed automation run.
	// The known module path above is the only deterministic fixture dependency.
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
		UNiagaraScript* Candidate = LoadObject<UNiagaraScript>(nullptr, Path, nullptr, LOAD_NoWarn);
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

	// Keep the fixture independent of optional Niagara content.  Loading all
	// registered scripts is unsafe in an isolated HostProject because stale or
	// partially mounted engine packages can report serialization errors.  A
	// missing common asset is an explicit environment skip below.
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

TSharedPtr<FJsonObject> NiagaraModuleInputDICloneSpec(const TSharedPtr<FJsonObject>& Spec)
{
	if (!Spec.IsValid())
	{
		return nullptr;
	}
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Spec.ToSharedRef(), Writer))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Clone;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
	return FJsonSerializer::Deserialize(Reader, Clone) ? Clone : nullptr;
}

TSharedPtr<FJsonObject> NiagaraModuleInputDIFindSpecModule(
	const TSharedPtr<FJsonObject>& Object, const FString& ModuleGuid)
{
	if (!Object.IsValid())
	{
		return nullptr;
	}
	FString NodeGuid;
	if (Object->TryGetStringField(TEXT("nodeGuid"), NodeGuid)
		&& NodeGuid.Equals(ModuleGuid, ESearchCase::IgnoreCase))
	{
		return Object;
	}
	// Only the spec hierarchy owns modules. Do not search a dynamic-input tree
	// by script name: two modules may use the same script in this graph.
	for (const TCHAR* Collection : {TEXT("emitters"), TEXT("stacks"), TEXT("modules")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!Object->TryGetArrayField(Collection, Rows) || !Rows)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Rows)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Found = NiagaraModuleInputDIFindSpecModule(Value->AsObject(), ModuleGuid);
			if (Found.IsValid())
			{
				return Found;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> NiagaraModuleInputDIFindSpecInput(
	const TSharedPtr<FJsonObject>& Owner, const FString& InputName)
{
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (!Owner.IsValid() || !Owner->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Inputs)
	{
		const TSharedPtr<FJsonObject> Input = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		FString Name;
		if (Input.IsValid() && Input->TryGetStringField(TEXT("name"), Name))
		{
			Name.RemoveFromStart(TEXT("Module."));
			if (Name.Equals(InputName, ESearchCase::IgnoreCase))
			{
				return Input;
			}
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> NiagaraModuleInputDIGetSpecTree(const TSharedPtr<FJsonObject>& Input)
{
	const TSharedPtr<FJsonObject>* Tree = nullptr;
	return Input.IsValid() && Input->TryGetObjectField(TEXT("dynamicInputTree"), Tree) && Tree
		? *Tree : nullptr;
}

void NiagaraModuleInputDIClearSpecInputSource(const TSharedPtr<FJsonObject>& Input)
{
	if (!Input.IsValid())
	{
		return;
	}
	// The row remains in the complete tree; absence of a source is the explicit
	// clear operation. Removing the row would make the snapshot incomplete.
	for (const TCHAR* Field : {TEXT("value"), TEXT("binding"), TEXT("dynamicInput"),
		TEXT("dynamicInputGuid"), TEXT("dynamicInputTree")})
	{
		Input->RemoveField(Field);
	}
}

void NiagaraModuleInputDISyncSpecChildren(const TSharedPtr<FJsonObject>& Tree)
{
	TArray<TSharedPtr<FJsonValue>> Children;
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	if (!Tree.IsValid() || !Tree->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Inputs)
	{
		const TSharedPtr<FJsonObject> Input = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		const TSharedPtr<FJsonObject> ChildTree = NiagaraModuleInputDIGetSpecTree(Input);
		if (!ChildTree.IsValid())
		{
			continue;
		}
		auto Child = MakeShared<FJsonObject>();
		Child->SetStringField(TEXT("input"), Input->GetStringField(TEXT("name")));
		for (const TCHAR* Identity : {TEXT("name"), TEXT("guid"), TEXT("scriptPath"), TEXT("truncated")})
		{
			if (ChildTree->HasField(Identity))
			{
				Child->SetField(Identity, ChildTree->TryGetField(Identity));
			}
		}
		Children.Add(MakeShared<FJsonValueObject>(Child));
	}
	Tree->SetArrayField(TEXT("children"), MoveTemp(Children));
}

UNiagaraNodeFunctionCall* NiagaraModuleInputDIFindGraphDynamicNode(UNiagaraGraph* Graph, const FString& GuidString)
{
	FGuid Guid;
	if (!Graph || !FGuid::Parse(GuidString, Guid))
	{
		return nullptr;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == Guid)
		{
			return Cast<UNiagaraNodeFunctionCall>(Node);
		}
	}
	return nullptr;
}

UEdGraphNode* NiagaraModuleInputDIFindOverrideNode(UNiagaraNodeFunctionCall* Function)
{
	if (!Function)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Function->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() == 1 && Pin->LinkedTo[0])
		{
			UEdGraphNode* Override = Pin->LinkedTo[0]->GetOwningNodeUnchecked();
			if (Override && Override->GetClass()->GetFName() == TEXT("NiagaraNodeParameterMapSet"))
			{
				return Override;
			}
		}
	}
	return nullptr;
}

UNiagaraScript* NiagaraModuleInputDICreateBrokenScript(
	UNiagaraScript* CompatibleScript, UPackage* FixturePackage)
{
	if (!CompatibleScript || !FixturePackage)
	{
		return nullptr;
	}
	UNiagaraScript* BrokenScript = DuplicateObject<UNiagaraScript>(
		CompatibleScript, FixturePackage,
		FName(*(FString(TEXT("UEAI_BrokenDynamicInput_")) + FGuid::NewGuid().ToString(EGuidFormats::Digits))));
	if (BrokenScript && BrokenScript->IsVersioningEnabled())
	{
		BrokenScript->DisableVersioning(BrokenScript->GetExposedVersion().VersionGuid);
	}
	UNiagaraScriptSource* OriginalSource = BrokenScript ? Cast<UNiagaraScriptSource>(BrokenScript->GetLatestSource()) : nullptr;
	UNiagaraScriptSource* Source = OriginalSource && BrokenScript
		? DuplicateObject<UNiagaraScriptSource>(OriginalSource, BrokenScript, TEXT("UEAI_BrokenSource")) : nullptr;
	UNiagaraGraph* Graph = Source ? Source->NodeGraph : nullptr;
	if (Graph && (!Graph->IsIn(Source) || Graph == OriginalSource->NodeGraph))
	{
		Graph = DuplicateObject<UNiagaraGraph>(Graph, Source, TEXT("UEAI_BrokenGraph"));
		Source->NodeGraph = Graph;
	}
	if (BrokenScript && Source && Graph && Graph->GetOutermost() == FixturePackage)
	{
		BrokenScript->SetLatestSource(Source);
	}
	else
	{
		Graph = nullptr;
	}
	UNiagaraNodeOutput* Output = FindNiagaraModuleInputDIOutput(Graph, ENiagaraScriptUsage::DynamicInput);
	UEdGraphPin* OutputValue = nullptr;
	if (Output && Output->GetOutputs().Num() > 0)
	{
		for (UEdGraphPin* Pin : Output->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->GetFName() == Output->GetOutputs()[0].GetName())
			{
				OutputValue = Pin;
				break;
			}
		}
	}
	if (!OutputValue)
	{
		if (BrokenScript)
		{
			BrokenScript->ClearFlags(RF_Public | RF_Standalone);
			BrokenScript->MarkAsGarbage();
		}
		return nullptr;
	}
	// This is a typed, reachable compile error, not an invalid selector. The
	// compiler's real FunctionCall::Compile missing-script/signature branch
	// reports UnknownFunction after input.di.set has replaced the authored tree.
	FGraphNodeCreator<UNiagaraNodeFunctionCall> Creator(*Graph);
	UNiagaraNodeFunctionCall* InvalidFunction = Creator.CreateNode();
	InvalidFunction->FunctionScript = nullptr;
	InvalidFunction->Signature = FNiagaraFunctionSignature();
	Creator.Finalize();
	UEdGraphPin* InvalidOutput = InvalidFunction->CreatePin(EGPD_Output, OutputValue->PinType, TEXT("UEAI_MissingFunctionResult"));
	OutputValue->BreakAllPinLinks();
	InvalidOutput->MakeLinkTo(OutputValue);
	Graph->NotifyGraphChanged();
	return BrokenScript;
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

	// Exercise authored edits through system.spec.import, independently of the
	// direct setters. Each case restores the exported baseline before the next
	// case so the direct remove assertions below still use the original GUIDs.
	if (!MountedDynamicInputGuid.IsEmpty())
	{
		const FString ModuleGuid = Added->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		const FNiagaraVariable ValidationUserFloat(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.FutureTreeValidation"));
		if (!TestTrue(TEXT("Future-tree validation has an authored user default"),
			Fixture.System->GetExposedParameters().AddParameter(ValidationUserFloat)
			&& Fixture.System->GetExposedParameters().SetParameterValue<float>(1.0f, ValidationUserFloat, false)))
		{
			return false;
		}
		const FMCPToolResult BaselineExport = Registry.ExecuteTool(
			TEXT("content.niagara.system.spec.export"), SpecExportParams);
		const TSharedPtr<FJsonObject> Baseline = NiagaraModuleInputDICloneSpec(BaselineExport.Data);
		const TSharedPtr<FJsonObject> BaselineModule = NiagaraModuleInputDIFindSpecModule(Baseline, ModuleGuid);
		const TSharedPtr<FJsonObject> BaselineInput = NiagaraModuleInputDIFindSpecInput(BaselineModule, LinkableName);
		const TSharedPtr<FJsonObject> BaselineTree = NiagaraModuleInputDIGetSpecTree(BaselineInput);
		if (!TestTrue(TEXT("Dynamic-input edit baseline exports"), BaselineExport.bSuccess)
			|| !TestNotNull(TEXT("Dynamic-input edit baseline has the exact module input tree"), BaselineTree.Get()))
		{
			return false;
		}
		TestEqual(TEXT("Dynamic-input spec has a global 512-node budget"),
			Baseline->GetIntegerField(TEXT("dynamicInputNodesLimit")), 512);
		TestEqual(TEXT("Global budget counts each authored dynamic node once"),
			Baseline->GetIntegerField(TEXT("dynamicInputNodesWritten")), MountedNestedDynamicInputGuid.IsEmpty() ? 1 : 2);
		TestFalse(TEXT("Small authored dynamic tree is untruncated"),
			Baseline->GetBoolField(TEXT("dynamicInputsTruncated")));
		// The standalone tree setter consumes exactly the exported tree object and
		// proves the complete nested edit path independently of system.spec.import.
		if (!MountedDynamicInputGuid.IsEmpty())
		{
			auto TreeSetParams = MakeMountedDynamicInputParams();
			TreeSetParams->SetStringField(TEXT("dynamicInputGuid"), MountedDynamicInputGuid);
			TreeSetParams->SetObjectField(TEXT("dynamicInputTree"),
				NiagaraModuleInputDICloneSpec(BaselineTree));
			const FMCPToolResult TreeSet = Registry.ExecuteTool(
				TEXT("content.niagara.graph.module.dynamic_inputs.tree.set"), TreeSetParams);
			TestTrue(TEXT("Standalone complete Dynamic Input tree set succeeds"), TreeSet.bSuccess);
			if (TreeSet.Data)
			{
				TestTrue(TEXT("Standalone tree set reports compiled read-back"),
					TreeSet.Data->GetBoolField(TEXT("compiled"))
					&& TreeSet.Data->GetBoolField(TEXT("verified")));
			}
			auto MismatchedTreeParams = MakeMountedDynamicInputParams();
			MismatchedTreeParams->SetStringField(TEXT("dynamicInputGuid"), MountedDynamicInputGuid);
			const TSharedPtr<FJsonObject> MismatchedTree = NiagaraModuleInputDICloneSpec(BaselineTree);
			MismatchedTree->SetStringField(
				TEXT("guid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			MismatchedTreeParams->SetObjectField(TEXT("dynamicInputTree"), MismatchedTree);
			const FMCPToolResult MismatchedTreeResult = Registry.ExecuteTool(
				TEXT("content.niagara.graph.module.dynamic_inputs.tree.set"), MismatchedTreeParams);
			TestFalse(TEXT("Tree set rejects a mismatched root identity before mutation"), MismatchedTreeResult.bSuccess);
			TestEqual(TEXT("Mismatched tree identity has a stable error code"),
				MismatchedTreeResult.ErrorCode, FString(TEXT("dynamic_input_tree_identity_mismatch")));
		}

		FString NestedInputName;
		if (!MountedNestedDynamicInputGuid.IsEmpty())
		{
			for (const TSharedPtr<FJsonValue>& InputValue : BaselineTree->GetArrayField(TEXT("inputs")))
			{
				const TSharedPtr<FJsonObject> Input = InputValue.IsValid() ? InputValue->AsObject() : nullptr;
				FString Guid;
				if (Input.IsValid() && Input->TryGetStringField(TEXT("dynamicInputGuid"), Guid)
					&& Guid.Equals(MountedNestedDynamicInputGuid, ESearchCase::IgnoreCase))
				{
					Input->TryGetStringField(TEXT("name"), NestedInputName);
					NestedInputName.RemoveFromStart(TEXT("Module."));
					break;
				}
			}
			TestTrue(TEXT("Spec locates the exact mounted nested input"), !NestedInputName.IsEmpty());
		}
		auto GetRootSpecInput = [&](const TSharedPtr<FJsonObject>& Snapshot)
		{
			return NiagaraModuleInputDIFindSpecInput(
				NiagaraModuleInputDIFindSpecModule(Snapshot, ModuleGuid), LinkableName);
		};
		auto ImportSnapshot = [&](const TSharedPtr<FJsonObject>& Snapshot, const TCHAR* RequestId)
		{
			auto ImportParams = MakeShared<FJsonObject>();
			ImportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
			ImportParams->SetObjectField(TEXT("spec"), Snapshot);
			ImportParams->SetStringField(TEXT("requestId"), RequestId);
			ImportParams->SetBoolField(TEXT("confirmWrite"), true);
			return Registry.ExecuteTool(TEXT("content.niagara.system.spec.import"), ImportParams);
		};
		auto RestoreBaseline = [&]()
		{
			const FMCPToolResult Restore = ImportSnapshot(
				NiagaraModuleInputDICloneSpec(Baseline), TEXT("module-input-di-restore-baseline"));
			const bool bRestored = TestTrue(TEXT("Spec edit restores the original dynamic-input baseline"), Restore.bSuccess);
			if (!Restore.bSuccess)
			{
				AddError(FString::Printf(TEXT("Dynamic-input baseline restore failed: %s %s"),
					*Restore.ErrorCode, *Restore.ErrorMessage));
			}
			TestNotNull(TEXT("Baseline restore retains the root GUID"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
			if (!MountedNestedDynamicInputGuid.IsEmpty())
			{
				TestNotNull(TEXT("Baseline restore retains the descendant GUID"),
					NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid));
			}
			return bRestored;
		};
		auto VerifyTruncatedTreeRejected = [&](const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject> Snapshot = NiagaraModuleInputDICloneSpec(Baseline);
			NiagaraModuleInputDIGetSpecTree(GetRootSpecInput(Snapshot))->SetBoolField(Field, true);
			const FGuid BeforeChangeId = Fixture.Graph->GetChangeID();
			const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
			const bool bBeforeDirty = Fixture.Package->IsDirty();
			const FMCPToolResult Rejected = ImportSnapshot(Snapshot, TEXT("module-input-di-truncated-tree"));
			TestFalse(FString::Printf(TEXT("Dynamic tree %s is rejected"), Field), Rejected.bSuccess);
			TestEqual(TEXT("Truncated tree uses spec_invalid"), Rejected.ErrorCode, FString(TEXT("spec_invalid")));
			TestEqual(TEXT("Truncated tree uses HTTP 422"), Rejected.HttpStatus, 422);
			TestEqual(TEXT("Truncated tree preserves graph change ID"), Fixture.Graph->GetChangeID(), BeforeChangeId);
			TestEqual(TEXT("Truncated tree preserves graph node count"), Fixture.Graph->Nodes.Num(), BeforeNodeCount);
			TestEqual(TEXT("Truncated tree preserves package dirty state"), Fixture.Package->IsDirty(), bBeforeDirty);
		};
		VerifyTruncatedTreeRejected(TEXT("truncated"));
		VerifyTruncatedTreeRejected(TEXT("inputsTruncated"));
		VerifyTruncatedTreeRejected(TEXT("childrenTruncated"));

		auto VerifyInvalidFutureTreeRejected = [&](const TCHAR* Case)
		{
			const TSharedPtr<FJsonObject> Snapshot = NiagaraModuleInputDICloneSpec(Baseline);
			const TSharedPtr<FJsonObject> Input = GetRootSpecInput(Snapshot);
			const TSharedPtr<FJsonObject> Tree = NiagaraModuleInputDIGetSpecTree(Input);
			TArray<TSharedPtr<FJsonValue>> Inputs = Tree->GetArrayField(TEXT("inputs"));
			if (FCString::Strcmp(Case, TEXT("invalid-guid")) == 0)
			{
				Input->SetStringField(TEXT("dynamicInputGuid"), TEXT("not-a-guid"));
			}
			else if (FCString::Strcmp(Case, TEXT("guid-collision")) == 0)
			{
				Input->SetStringField(TEXT("dynamicInputGuid"), ModuleGuid);
				Tree->SetStringField(TEXT("guid"), ModuleGuid);
			}
			else if (FCString::Strcmp(Case, TEXT("invalid-children")) == 0)
			{
				auto Child = MakeShared<FJsonObject>();
				Child->SetStringField(TEXT("input"), TEXT("DefinitelyNotAnAuthoredInput"));
				Tree->SetArrayField(TEXT("children"), {MakeShared<FJsonValueObject>(Child)});
			}
			else if (Inputs.Num() > 0)
			{
				if (FCString::Strcmp(Case, TEXT("duplicate-input")) == 0)
				{
					Inputs.Add(MakeShared<FJsonValueObject>(NiagaraModuleInputDICloneSpec(Inputs[0]->AsObject())));
					Tree->SetArrayField(TEXT("inputs"), Inputs);
				}
				else
				{
					Inputs[0]->AsObject()->SetStringField(TEXT("type"), TEXT("DefinitelyNotTheScriptInputType"));
				}
			}
			else
			{
				AddError(TEXT("Future-tree fixture has no input for duplicate/type validation."));
				return;
			}
			// A valid pending user-default edit must also remain unapplied when
			// a later graph edit fails validation before the transaction starts.
			for (const TSharedPtr<FJsonValue>& Value : Snapshot->GetArrayField(TEXT("userParameters")))
			{
				const TSharedPtr<FJsonObject> Parameter = Value->AsObject();
				if (Parameter->GetStringField(TEXT("name")).EndsWith(TEXT("FutureTreeValidation")))
					Parameter->SetNumberField(TEXT("default"), 99.0);
			}
			Fixture.Package->SetDirtyFlag(false);
			const FGuid BeforeChangeId = Fixture.Graph->GetChangeID();
			const TArray<UEdGraphNode*> BeforeNodes = Fixture.Graph->Nodes;
			TMap<UEdGraphPin*, TArray<UEdGraphPin*>> BeforeLinks;
			for (UEdGraphNode* Node : BeforeNodes)
				for (UEdGraphPin* Pin : Node->Pins)
					BeforeLinks.Add(Pin, Pin->LinkedTo);
			const bool bBeforeDirty = Fixture.Package->IsDirty();
			UNiagaraNodeFunctionCall* BeforeRoot = NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid);
			UNiagaraScript* BeforeScript = BeforeRoot ? BeforeRoot->FunctionScript : nullptr;
			const FMCPToolResult Rejected = ImportSnapshot(Snapshot, Case);
			TestFalse(FString::Printf(TEXT("Future tree %s is rejected"), Case), Rejected.bSuccess);
			TestEqual(TEXT("Future-tree validation uses a stable error code"), Rejected.ErrorCode, FString(TEXT("dynamic_input_state_invalid")));
			TestEqual(TEXT("Future-tree validation uses HTTP 422"), Rejected.HttpStatus, 422);
			TestEqual(TEXT("Future-tree rejection preserves graph version"), Fixture.Graph->GetChangeID(), BeforeChangeId);
			TestTrue(TEXT("Future-tree rejection preserves all original graph objects"), TArray<UEdGraphNode*>(Fixture.Graph->Nodes) == BeforeNodes);
			TestTrue(TEXT("Future-tree rejection preserves the original script root"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid) == BeforeRoot
				&& BeforeRoot && BeforeRoot->FunctionScript == BeforeScript);
			for (UEdGraphNode* Node : BeforeNodes)
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					const TArray<UEdGraphPin*>* Links = BeforeLinks.Find(Pin);
					TestTrue(TEXT("Future-tree rejection preserves original pins and connections"), Links && *Links == Pin->LinkedTo);
				}
			}
			TestEqual(TEXT("Future-tree rejection preserves pending user defaults"),
				Fixture.System->GetExposedParameters().GetParameterValue<float>(ValidationUserFloat), 1.0f);
			TestEqual(TEXT("Future-tree rejection preserves live package dirty state"), Fixture.Package->IsDirty(), bBeforeDirty);
			const FMCPToolResult AfterReject = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), SpecExportParams);
			if (TestTrue(TEXT("System still exports after rejected future tree"), AfterReject.bSuccess) && AfterReject.Data)
				TestEqual(TEXT("Future-tree rejection preserves the complete authored spec digest"),
					AfterReject.Data->GetStringField(TEXT("specDigest")), Baseline->GetStringField(TEXT("specDigest")));
		};
		for (const TCHAR* Case : {TEXT("invalid-guid"), TEXT("guid-collision"), TEXT("duplicate-input"), TEXT("invalid-type"), TEXT("invalid-children")})
			VerifyInvalidFutureTreeRejected(Case);

		UEdGraphNode* DirectReplacementOldOverride = NiagaraModuleInputDIFindOverrideNode(
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		const FMCPToolResult DirectReplacement = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.input.di.set"),
			MakeNiagaraModuleInputDIParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
				LinkableName, DynamicInputScript->GetPathName()));
		TestTrue(TEXT("Direct input.di.set replaces an existing root tree"), DirectReplacement.bSuccess);
		TestNull(TEXT("Direct root replacement removes the old root from the graph"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		if (!MountedNestedDynamicInputGuid.IsEmpty())
		{
			TestNull(TEXT("Direct root replacement removes the entire old descendant tree"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid));
		}
		TestFalse(TEXT("Direct root replacement removes descendant override scaffolds"),
			DirectReplacementOldOverride && Fixture.Graph->Nodes.Contains(DirectReplacementOldOverride));
		const FMCPToolResult DirectReplacementExport = Registry.ExecuteTool(
			TEXT("content.niagara.system.spec.export"), SpecExportParams);
		const TSharedPtr<FJsonObject> DirectReplacementInput = GetRootSpecInput(DirectReplacementExport.Data);
		FString DirectReplacementGuid;
		TestTrue(TEXT("Direct replacement readback exposes a different mounted GUID"),
			DirectReplacementExport.bSuccess && DirectReplacementInput.IsValid()
			&& DirectReplacementInput->TryGetStringField(TEXT("dynamicInputGuid"), DirectReplacementGuid)
			&& !DirectReplacementGuid.IsEmpty() && DirectReplacementGuid != MountedDynamicInputGuid);
		TestNotNull(TEXT("Direct replacement readback GUID identifies a real graph node"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, DirectReplacementGuid));
		if (!RestoreBaseline())
		{
			return false;
		}

		UNiagaraScript* BrokenScript = NiagaraModuleInputDICreateBrokenScript(DynamicInputScript, Fixture.Package);
		if (!TestNotNull(TEXT("Compile-failure fixture creates a typed broken Dynamic Input script"), BrokenScript))
		{
			return false;
		}
		ON_SCOPE_EXIT
		{
			BrokenScript->ClearFlags(RF_Public | RF_Standalone);
			BrokenScript->MarkAsGarbage();
		};
		const FMCPToolResult BeforeFailedSet = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), SpecExportParams);
		if (!TestTrue(TEXT("Compile-failure baseline exports"), BeforeFailedSet.bSuccess) || !BeforeFailedSet.Data)
		{
			return false;
		}
		const FString BeforeFailedSetDigest = BeforeFailedSet.Data->GetStringField(TEXT("specDigest"));
		const int32 BeforeFailedSetNodeCount = Fixture.Graph->Nodes.Num();
		const bool bBeforeFailedSetDirty = Fixture.Package->IsDirty();
		AddExpectedErrorPlain(TEXT("Unknown Function Call! Missing Script or Data Interface Signature."),
			EAutomationExpectedErrorFlags::Contains, 0);
		const FMCPToolResult FailedSet = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.input.di.set"),
			MakeNiagaraModuleInputDIParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
				LinkableName, BrokenScript->GetPathName()));
		TestFalse(TEXT("Direct Dynamic Input compile failure is rejected"), FailedSet.bSuccess);
		TestEqual(TEXT("Direct Dynamic Input compile failure uses compile_failed after restoration"),
			FailedSet.ErrorCode, FString(TEXT("compile_failed")));
		TestEqual(TEXT("Direct compile failure restores graph node count"), Fixture.Graph->Nodes.Num(), BeforeFailedSetNodeCount);
		TestEqual(TEXT("Direct compile failure restores package dirty state"), Fixture.Package->IsDirty(), bBeforeFailedSetDirty);
		TestNotNull(TEXT("Direct compile failure restores the old root GUID"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		if (!MountedNestedDynamicInputGuid.IsEmpty())
		{
			TestNotNull(TEXT("Direct compile failure restores the old descendant GUID"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid));
		}
		const FMCPToolResult AfterFailedSet = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), SpecExportParams);
		TestTrue(TEXT("Export succeeds after direct compile-failure restoration"), AfterFailedSet.bSuccess);
		if (AfterFailedSet.Data)
		{
			TestEqual(TEXT("Direct compile failure restores the complete authored spec digest"),
				AfterFailedSet.Data->GetStringField(TEXT("specDigest")), BeforeFailedSetDigest);
		}
		if (!RestoreBaseline())
		{
			return false;
		}

		// Build the requested broken tree on a separate System first. Its actual
		// script inputs are exported rather than guessed from the old script.
		TStrongObjectPtr<UNiagaraSystem> ScratchSystem(DuplicateObject<UNiagaraSystem>(
			Fixture.System, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
				UNiagaraSystem::StaticClass(), FName(TEXT("UEAI_SpecCompileFailurePreview")))));
		if (!TestNotNull(TEXT("Spec compile-failure preview System is isolated"), ScratchSystem.Get())
			|| !TestEqual(TEXT("Spec preview belongs to the transient package"), ScratchSystem->GetOutermost(), GetTransientPackage())
			|| !TestEqual(TEXT("Spec preview has the owned emitter"), ScratchSystem->GetEmitterHandles().Num(), 1))
		{
			return false;
		}
		ScratchSystem->SetFlags(RF_Transient);
		ScratchSystem->ClearFlags(RF_Public | RF_Standalone);
		const FVersionedNiagaraEmitterData* ScratchData = ScratchSystem->GetEmitterHandles()[0].GetEmitterData();
		const UNiagaraScriptSource* ScratchSource = ScratchData ? Cast<UNiagaraScriptSource>(ScratchData->GraphSource) : nullptr;
		UNiagaraGraph* ScratchGraph = ScratchSource ? ScratchSource->NodeGraph : nullptr;
		if (!TestNotNull(TEXT("Spec preview has its own graph"), ScratchGraph)
			|| !TestTrue(TEXT("Spec preview never edits the live graph"), ScratchGraph != Fixture.Graph))
		{
			return false;
		}
		UNiagaraNodeFunctionCall* ScratchModule = NiagaraModuleInputDIFindGraphDynamicNode(ScratchGraph, ModuleGuid);
		if (!TestNotNull(TEXT("Spec preview resolves the exact module GUID"), ScratchModule))
			return false;
		const FNiagaraParameterHandle ScratchAliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(Linkable.GetName()), ScratchModule);
		UEdGraphPin& ScratchOverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
			*ScratchModule, ScratchAliased, Linkable.GetType(), FGuid(), FGuid());
		ScratchOverridePin.BreakAllPinLinks(false);
		ScratchOverridePin.DefaultValue.Reset();
		UNiagaraNodeFunctionCall* ScratchBrokenRoot = nullptr;
		FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(ScratchOverridePin, BrokenScript, ScratchBrokenRoot);
		if (!TestNotNull(TEXT("Spec preview mounts the actual broken script"), ScratchBrokenRoot))
			return false;
		auto ScratchExportParams = MakeShared<FJsonObject>();
		ScratchExportParams->SetStringField(TEXT("system"), ScratchSystem->GetPathName());
		const FMCPToolResult ScratchExport = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), ScratchExportParams);
		const TSharedPtr<FJsonObject> ScratchInput = NiagaraModuleInputDIFindSpecInput(
			NiagaraModuleInputDIFindSpecModule(ScratchExport.Data, ModuleGuid), LinkableName);
		const TSharedPtr<FJsonObject> ScratchTree = NiagaraModuleInputDIGetSpecTree(ScratchInput);
		if (!TestTrue(TEXT("Broken spec future tree exports without compiling"), ScratchExport.bSuccess)
			|| !TestNotNull(TEXT("Broken spec has the complete actual script input tree"), ScratchTree.Get()))
		{
			return false;
		}
		const FMCPToolResult BeforeFailedImport = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), SpecExportParams);
		if (!TestTrue(TEXT("Spec compile-failure baseline exports"), BeforeFailedImport.bSuccess) || !BeforeFailedImport.Data)
			return false;
		const TSharedPtr<FJsonObject> BrokenSpec = NiagaraModuleInputDICloneSpec(BeforeFailedImport.Data);
		const TSharedPtr<FJsonObject> BrokenSpecInput = GetRootSpecInput(BrokenSpec);
		NiagaraModuleInputDIClearSpecInputSource(BrokenSpecInput);
		BrokenSpecInput->SetStringField(TEXT("dynamicInput"), BrokenScript->GetPathName());
		BrokenSpecInput->SetStringField(TEXT("dynamicInputGuid"), ScratchInput->GetStringField(TEXT("dynamicInputGuid")));
		const TSharedPtr<FJsonObject> RequestedBrokenTree = NiagaraModuleInputDICloneSpec(ScratchTree);
		RequestedBrokenTree->RemoveField(TEXT("name"));
		BrokenSpecInput->SetObjectField(TEXT("dynamicInputTree"), RequestedBrokenTree);
		for (const TSharedPtr<FJsonValue>& Value : BrokenSpec->GetArrayField(TEXT("userParameters")))
		{
			const TSharedPtr<FJsonObject> Parameter = Value->AsObject();
			if (Parameter->GetStringField(TEXT("name")).EndsWith(TEXT("FutureTreeValidation")))
				Parameter->SetNumberField(TEXT("default"), 99.0);
		}
		Fixture.Package->SetDirtyFlag(false);
		const TArray<UEdGraphNode*> BeforeFailedImportNodes = Fixture.Graph->Nodes;
		TMap<UEdGraphNode*, TArray<UEdGraphPin*>> BeforeFailedImportPins;
		TMap<UEdGraphPin*, TArray<UEdGraphPin*>> BeforeFailedImportLinks;
		TMap<UEdGraphPin*, FString> BeforeFailedImportDefaults;
		for (UEdGraphNode* Node : BeforeFailedImportNodes)
		{
			BeforeFailedImportPins.Add(Node, Node->Pins);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				BeforeFailedImportLinks.Add(Pin, Pin->LinkedTo);
				BeforeFailedImportDefaults.Add(Pin, Pin->DefaultValue);
			}
		}
		UNiagaraNodeFunctionCall* BeforeFailedImportRoot = NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid);
		UNiagaraNodeFunctionCall* BeforeFailedImportDescendant = NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid);
		const FMCPToolResult FailedImport = ImportSnapshot(BrokenSpec, TEXT("spec-real-compile-failure"));
		TestFalse(TEXT("Spec import rejects a real reachable compile error"), FailedImport.bSuccess);
		if (!TestEqual(TEXT("Spec compile failure is verified restored"), FailedImport.ErrorCode, FString(TEXT("compile_failed"))))
		{
			AddError(FString::Printf(TEXT("Spec compile-failure receipt: %s"), *FailedImport.ErrorMessage));
			return false;
		}
		TestTrue(TEXT("Spec compile failure restores the original node objects and order"),
			TArray<UEdGraphNode*>(Fixture.Graph->Nodes) == BeforeFailedImportNodes);
		TestTrue(TEXT("Spec compile failure restores the actual original root object"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid) == BeforeFailedImportRoot);
		if (!MountedNestedDynamicInputGuid.IsEmpty())
			TestTrue(TEXT("Spec compile failure restores the actual original descendant object"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid) == BeforeFailedImportDescendant);
		for (UEdGraphNode* Node : BeforeFailedImportNodes)
		{
			if (!TestTrue(TEXT("Spec rollback keeps each original node alive in the graph"), Fixture.Graph->Nodes.Contains(Node))
				|| !TestTrue(TEXT("Spec rollback restores the original pin set and order"), Node->Pins == BeforeFailedImportPins.FindChecked(Node)))
			{
				return false;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				TestTrue(TEXT("Spec rollback restores complete original connections"), Pin->LinkedTo == BeforeFailedImportLinks.FindChecked(Pin));
				TestEqual(TEXT("Spec rollback restores original pin defaults"), Pin->DefaultValue, BeforeFailedImportDefaults.FindChecked(Pin));
			}
		}
		TestEqual(TEXT("Spec compile failure restores the pending user-default edit"),
			Fixture.System->GetExposedParameters().GetParameterValue<float>(ValidationUserFloat), 1.0f);
		TestFalse(TEXT("Verified spec rollback restores the original clean package state"), Fixture.Package->IsDirty());
		const FMCPToolResult AfterFailedImport = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), SpecExportParams);
		if (TestTrue(TEXT("Spec exports after compile-failure rollback"), AfterFailedImport.bSuccess) && AfterFailedImport.Data)
			TestEqual(TEXT("Spec compile failure restores the complete original authored digest"),
				AfterFailedImport.Data->GetStringField(TEXT("specDigest")), BeforeFailedImport.Data->GetStringField(TEXT("specDigest")));

		// A shared output is deliberately malformed authoring, so use a tiny
		// test-owned consumer with one matching pin. Rejection must happen before
		// compile or subtree deletion; teardown removes the injected edge.
		auto VerifySharedTreeRejected = [&](const FString& SharedGuid, const bool bReplaceRoot,
			const bool bClearNested, const TCHAR* CaseName)
		{
			UNiagaraNodeFunctionCall* SharedNode = NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, SharedGuid);
			if (!TestNotNull(FString::Printf(TEXT("%s shared node exists"), CaseName), SharedNode))
			{
				return;
			}
			UEdGraphPin* SharedOutput = nullptr;
			for (UEdGraphPin* Pin : SharedNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output)
				{
					SharedOutput = Pin;
					break;
				}
			}
			if (!TestNotNull(TEXT("Shared dynamic node exposes an output"), SharedOutput))
			{
				return;
			}
			UEdGraphNode* Consumer = NewObject<UEdGraphNode>(Fixture.Graph, NAME_None, RF_Transactional);
			Consumer->CreateNewGuid();
			Fixture.Graph->AddNode(Consumer, false, false);
			UEdGraphPin* ConsumerInput = Consumer->CreatePin(EGPD_Input, SharedOutput->PinType, TEXT("UEAI_ExternalConsumer"));
			SharedOutput->MakeLinkTo(ConsumerInput);
			ON_SCOPE_EXIT
			{
				Consumer->BreakAllNodeLinks();
				Fixture.Graph->RemoveNode(Consumer);
			};
			const TSharedPtr<FJsonObject> Snapshot = NiagaraModuleInputDICloneSpec(Baseline);
			const TSharedPtr<FJsonObject> RootInput = GetRootSpecInput(Snapshot);
			if (bClearNested)
			{
				const TSharedPtr<FJsonObject> Tree = NiagaraModuleInputDIGetSpecTree(RootInput);
				NiagaraModuleInputDIClearSpecInputSource(NiagaraModuleInputDIFindSpecInput(Tree, NestedInputName));
				NiagaraModuleInputDISyncSpecChildren(Tree);
			}
			else if (bReplaceRoot)
			{
				const FString NewGuid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
				RootInput->SetStringField(TEXT("dynamicInputGuid"), NewGuid);
				NiagaraModuleInputDIGetSpecTree(RootInput)->SetStringField(TEXT("guid"), NewGuid);
			}
			else
			{
				NiagaraModuleInputDIClearSpecInputSource(RootInput);
			}
			const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
			const int32 BeforeConsumerCount = SharedOutput->LinkedTo.Num();
			const FMCPToolResult Rejected = ImportSnapshot(Snapshot, CaseName);
			TestFalse(FString::Printf(TEXT("%s rejects shared subtree mutation"), CaseName), Rejected.bSuccess);
			TestEqual(FString::Printf(TEXT("%s preserves all graph nodes"), CaseName), Fixture.Graph->Nodes.Num(), BeforeNodeCount);
			TestTrue(FString::Printf(TEXT("%s preserves shared node identity"), CaseName),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, SharedGuid) == SharedNode);
			TestEqual(FString::Printf(TEXT("%s preserves output consumers"), CaseName), SharedOutput->LinkedTo.Num(), BeforeConsumerCount);
			TestTrue(FString::Printf(TEXT("%s preserves the external link"), CaseName), SharedOutput->LinkedTo.Contains(ConsumerInput));
		};
		VerifySharedTreeRejected(MountedDynamicInputGuid, false, false, TEXT("spec-shared-root-clear"));
		VerifySharedTreeRejected(MountedDynamicInputGuid, true, false, TEXT("spec-shared-root-replacement"));
		if (!NestedInputName.IsEmpty())
		{
			VerifySharedTreeRejected(MountedNestedDynamicInputGuid, false, false, TEXT("spec-shared-descendant-root-clear"));
			VerifySharedTreeRejected(MountedNestedDynamicInputGuid, true, false, TEXT("spec-shared-descendant-root-replacement"));
			VerifySharedTreeRejected(MountedDynamicInputGuid, false, true, TEXT("spec-shared-root-same-script-edit"));
			VerifySharedTreeRejected(MountedNestedDynamicInputGuid, false, true, TEXT("spec-shared-descendant-clear"));

			const TSharedPtr<FJsonObject> NestedClearSpec = NiagaraModuleInputDICloneSpec(Baseline);
			const TSharedPtr<FJsonObject> Tree = NiagaraModuleInputDIGetSpecTree(GetRootSpecInput(NestedClearSpec));
			const TSharedPtr<FJsonObject> NestedInput = NiagaraModuleInputDIFindSpecInput(Tree, NestedInputName);
			const int32 BeforeInputCount = Tree->GetArrayField(TEXT("inputs")).Num();
			NiagaraModuleInputDIClearSpecInputSource(NestedInput);
			NiagaraModuleInputDISyncSpecChildren(Tree);
			TestEqual(TEXT("Explicit clear retains every complete-tree input row"), Tree->GetArrayField(TEXT("inputs")).Num(), BeforeInputCount);
			TestTrue(TEXT("Explicit clear retains nested input name and type"),
				NestedInput.IsValid() && NestedInput->HasField(TEXT("name")) && NestedInput->HasField(TEXT("type")));
			UEdGraphNode* NestedOverride = NiagaraModuleInputDIFindOverrideNode(
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
			TestNotNull(TEXT("Mounted nested input has an authored override node"), NestedOverride);
			const FMCPToolResult NestedClear = ImportSnapshot(NestedClearSpec, TEXT("module-input-di-spec-nested-clear"));
			TestTrue(TEXT("Complete-tree empty source row clears the nested override"), NestedClear.bSuccess);
			TestNull(TEXT("Nested clear removes the descendant from the actual graph"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid));
			TestNotNull(TEXT("Nested clear preserves the enclosing root in the actual graph"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
			TestFalse(TEXT("Nested clear removes its now-empty override scaffold"),
				NestedOverride && Fixture.Graph->Nodes.Contains(NestedOverride));
			const FMCPToolResult ClearedExport = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), SpecExportParams);
			const TSharedPtr<FJsonObject> ClearedTree = NiagaraModuleInputDIGetSpecTree(GetRootSpecInput(ClearedExport.Data));
			const TSharedPtr<FJsonObject> ClearedInput = NiagaraModuleInputDIFindSpecInput(ClearedTree, NestedInputName);
			TestTrue(TEXT("Nested clear readback retains a row with no authored source"),
				ClearedExport.bSuccess && ClearedInput.IsValid()
				&& !ClearedInput->HasField(TEXT("value")) && !ClearedInput->HasField(TEXT("binding"))
				&& !ClearedInput->HasField(TEXT("dynamicInput")) && !ClearedInput->HasField(TEXT("dynamicInputTree")));
			if (!RestoreBaseline())
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject> RootClearSpec = NiagaraModuleInputDICloneSpec(Baseline);
		NiagaraModuleInputDIClearSpecInputSource(GetRootSpecInput(RootClearSpec));
		UEdGraphNode* RootClearModuleOverride = NiagaraModuleInputDIFindOverrideNode(Added);
		UEdGraphNode* RootClearChildOverride = NiagaraModuleInputDIFindOverrideNode(
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		const FMCPToolResult RootClear = ImportSnapshot(RootClearSpec, TEXT("module-input-di-spec-root-clear"));
		TestTrue(TEXT("System spec clears the root input"), RootClear.bSuccess);
		TestNull(TEXT("Root clear removes its root from the actual graph"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		TestFalse(TEXT("Root clear removes the now-empty module override scaffold"),
			RootClearModuleOverride && Fixture.Graph->Nodes.Contains(RootClearModuleOverride));
		TestFalse(TEXT("Root clear removes descendant override scaffolds"),
			RootClearChildOverride && Fixture.Graph->Nodes.Contains(RootClearChildOverride));
		if (!MountedNestedDynamicInputGuid.IsEmpty())
		{
			TestNull(TEXT("Root clear removes all mounted descendants from the actual graph"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid));
		}
		if (!RestoreBaseline())
		{
			return false;
		}

		const TSharedPtr<FJsonObject> ReplacementSpec = NiagaraModuleInputDICloneSpec(Baseline);
		const TSharedPtr<FJsonObject> ReplacementInput = GetRootSpecInput(ReplacementSpec);
		const TSharedPtr<FJsonObject> ReplacementTree = NiagaraModuleInputDIGetSpecTree(ReplacementInput);
		const FString ReplacementGuid = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		ReplacementInput->SetStringField(TEXT("dynamicInputGuid"), ReplacementGuid);
		ReplacementTree->SetStringField(TEXT("guid"), ReplacementGuid);
		for (const TSharedPtr<FJsonValue>& InputValue : ReplacementTree->GetArrayField(TEXT("inputs")))
		{
			NiagaraModuleInputDIClearSpecInputSource(InputValue->AsObject());
		}
		NiagaraModuleInputDISyncSpecChildren(ReplacementTree);
		UEdGraphNode* ReplacementOldOverride = NiagaraModuleInputDIFindOverrideNode(
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		const FMCPToolResult Replacement = ImportSnapshot(ReplacementSpec, TEXT("module-input-di-spec-root-replacement"));
		TestTrue(TEXT("System spec replaces a root with a new GUID"), Replacement.bSuccess);
		TestNotNull(TEXT("Root replacement mounts the requested GUID"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, ReplacementGuid));
		TestNull(TEXT("Root replacement removes the old root from the actual graph"),
			NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedDynamicInputGuid));
		TestFalse(TEXT("Root replacement removes old descendant override scaffolds"),
			ReplacementOldOverride && Fixture.Graph->Nodes.Contains(ReplacementOldOverride));
		if (!MountedNestedDynamicInputGuid.IsEmpty())
		{
			TestNull(TEXT("Root replacement removes every old descendant from the actual graph"),
				NiagaraModuleInputDIFindGraphDynamicNode(Fixture.Graph, MountedNestedDynamicInputGuid));
		}
		if (!RestoreBaseline())
		{
			return false;
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
