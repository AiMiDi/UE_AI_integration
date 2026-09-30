// Contract tests for the direct Niagara stack module input binding command
// (content.niagara.graph.module.input.binding.set).
//
// The write path resolves a stack module and one of its authored inputs via
// ResolveModuleInput / FNiagaraStackGraphUtilities::GetStackFunctionInputs,
// validates the linked parameter against the known system/user parameters, and
// binds it through FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput.
// It never saves and never assumes a runtime/PIE session. The round trip needs
// a real Niagara Module script; the test locates one at runtime and skips with
// AddInfo when none is available, mirroring NiagaraModuleInputValueContractTests.cpp.
// All helper names in the anonymous namespace are prefixed with
// NiagaraModuleInputBinding to avoid unity-build collisions with the other
// Niagara contract test files.
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
struct FNiagaraModuleInputBindingFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FString EmitterName;
	UNiagaraGraph* Graph = nullptr;
};

bool CreateNiagaraModuleInputBindingFixture(FNiagaraModuleInputBindingFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_ModuleInputBinding_")
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

bool DeleteNiagaraModuleInputBindingFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

UNiagaraNodeOutput* FindNiagaraModuleInputBindingOutput(
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

UNiagaraScript* FindNiagaraModuleInputBindingScript()
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

// A linkable input maps to a standard common particle attribute of a
// compatible type; the returned attribute is the known-parameter name the
// binding command validates against. Unset for types with no safe standard
// attribute (e.g. bool, vec4), so the round trip skips those.
TOptional<FString> NiagaraModuleInputBindingAttributeForType(const FNiagaraTypeDefinition& Type)
{
	if (Type == FNiagaraTypeDefinition::GetFloatDef())
	{
		return FString(TEXT("Particles.NormalizedAge"));
	}
	if (Type == FNiagaraTypeDefinition::GetIntDef())
	{
		return FString(TEXT("Particles.UniqueID"));
	}
	if (Type == FNiagaraTypeDefinition::GetVec2Def())
	{
		return FString(TEXT("Particles.SpriteSize"));
	}
	if (Type == FNiagaraTypeDefinition::GetVec3Def())
	{
		return FString(TEXT("Particles.Velocity"));
	}
	if (Type == FNiagaraTypeDefinition::GetPositionDef())
	{
		return FString(TEXT("Particles.Position"));
	}
	if (Type == FNiagaraTypeDefinition::GetColorDef())
	{
		return FString(TEXT("Particles.Color"));
	}
	if (Type == FNiagaraTypeDefinition::GetQuatDef())
	{
		return FString(TEXT("Particles.MeshOrientation"));
	}
	return TOptional<FString>();
}

FString NiagaraModuleInputBindingShortName(const FName& FullName)
{
	FString Name = FullName.ToString();
	Name.RemoveFromStart(TEXT("Module."));
	return Name;
}

TSharedRef<FJsonObject> MakeNiagaraModuleInputBindingParams(
	const FNiagaraModuleInputBindingFixture& Fixture,
	const FString& OutputNodePath,
	const FString& ModuleSelector,
	const FString& Input,
	const FString& Parameter)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterName);
	Params->SetStringField(TEXT("outputNodePath"), OutputNodePath);
	Params->SetStringField(TEXT("moduleSelector"), ModuleSelector);
	Params->SetStringField(TEXT("input"), Input);
	Params->SetStringField(TEXT("parameter"), Parameter);
	return Params;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraModuleInputBindingContractTest,
	"UE_AI_integration.Niagara.ModuleInputBindingContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleInputBindingContractTest::RunTest(const FString&)
{
	FNiagaraModuleInputBindingFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Module-input-binding fixture and package are deleted"),
			DeleteNiagaraModuleInputBindingFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Module-input-binding fixture builds"), CreateNiagaraModuleInputBindingFixture(Fixture))
		|| !TestNotNull(TEXT("Module-input-binding system"), Fixture.System)
		|| !TestNotNull(TEXT("Module-input-binding graph"), Fixture.Graph))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	TestNotNull(TEXT("Module-input-binding tool registers"),
		Registry.FindTool(TEXT("content.niagara.graph.module.input.binding.set")));

	UNiagaraNodeOutput* Output = FindNiagaraModuleInputBindingOutput(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestNotNull(TEXT("Particle-update output resolves"), Output))
	{
		return false;
	}
	const FString ParticleUpdatePath = Output->GetPathName();

	UNiagaraScript* ModuleScript = FindNiagaraModuleInputBindingScript();
	if (!ModuleScript || !ModuleScript->GetLatestSource())
	{
		AddInfo(TEXT("No Niagara Module script asset was found; skipping the module input binding round trip (requires a real Module asset)."));
		return true;
	}

	UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *Output);
	if (!TestNotNull(TEXT("Module added to the stack"), Added))
	{
		AddInfo(TEXT("AddScriptModuleToStack returned no node; skipping the module input binding round trip."));
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
			TEXT("content.niagara.graph.module.input.binding.set"),
			MakeNiagaraModuleInputBindingParams(
				Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
				TEXT("DefinitelyDoesNotExist"), TEXT("Particles.Position")));
		TestFalse(TEXT("Unknown input is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown input uses the stable input_not_found code"),
			Result.ErrorCode, FString(TEXT("input_not_found")));
	}

	// Pick the first linkable (non data-interface/object, non static-switch)
	// input to drive the parameter-level checks.
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
		AddInfo(TEXT("The Module script has no linkable input; skipping the parameter and binding checks."));
		return true;
	}
	const FString LinkableName = NiagaraModuleInputBindingShortName(Linkable.GetName());

	// (b) an unknown linked parameter is rejected with parameter_not_found.
	{
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.graph.module.input.binding.set"),
			MakeNiagaraModuleInputBindingParams(
				Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
				LinkableName, TEXT("DefinitelyNotAParameter.Nope")));
		TestFalse(TEXT("Unknown linked parameter is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown linked parameter uses the stable parameter_not_found code"),
			Result.ErrorCode, FString(TEXT("parameter_not_found")));
	}

	// (c) bind the linkable input to a type-compatible standard attribute and
	// assert the read-back binding matches. Skipped when the input type has no
	// safe standard attribute or the synthetic system cannot compile.
	const TOptional<FString> Attribute = NiagaraModuleInputBindingAttributeForType(Linkable.GetType());
	if (!Attribute.IsSet())
	{
		AddInfo(TEXT("The linkable input type has no safe standard attribute; skipping the bind round trip."));
		return true;
	}

	const FMCPToolResult Bound = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.input.binding.set"),
		MakeNiagaraModuleInputBindingParams(
			Fixture, ParticleUpdatePath, ModuleScript->GetPathName(),
			LinkableName, Attribute.GetValue()));
	if (!TestTrue(TEXT("Module input binding succeeds"), Bound.bSuccess) || !Bound.Data)
	{
		AddInfo(TEXT("Module input binding did not succeed on the synthetic system; skipping the read-back round trip (compile environment required)."));
		return true;
	}
	TestEqual(TEXT("Module input binding uses the binding schema"),
		Bound.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.niagara.module-input-binding.v1")));
	TestEqual(TEXT("Module input binding reports the input"),
		Bound.Data->GetStringField(TEXT("input")), LinkableName);
	TestEqual(TEXT("Module input binding reports the parameter"),
		Bound.Data->GetStringField(TEXT("parameter")), Attribute.GetValue());
	TestFalse(TEXT("Module input binding never saves"), Bound.Data->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("Module input binding reports compiled"), Bound.Data->GetBoolField(TEXT("compiled")));

	// Independent read-back: the override pin is now linked to the parameter.
	const FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
		FNiagaraParameterHandle(Linkable.GetName()), Added);
	UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
		*Added, Aliased, Linkable.GetType(), FGuid(), FGuid());
	const bool bLinkedReadBack = OverridePin.LinkedTo.Num() == 1
		&& OverridePin.LinkedTo[0] != nullptr
		&& OverridePin.LinkedTo[0]->PinName.ToString().Equals(Attribute.GetValue(), ESearchCase::IgnoreCase);
	TestTrue(TEXT("Override pin read-back is linked to the requested parameter"), bLinkedReadBack);
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
