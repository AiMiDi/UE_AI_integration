// Contract tests for the read-only Niagara stack module input enumeration
// (content.niagara.graph.module.inputs.list).
//
// The query resolves a stack module and enumerates its authored inputs through
// the exported FNiagaraStackGraphUtilities::GetStackFunctionInputs. It never
// compiles or saves, so these tests do not assume a runtime/PIE session. Full
// enumeration needs a real Niagara Module script; the test locates one at
// runtime (engine default + asset-registry fallback) and skips with AddInfo when
// none is available, mirroring NiagaraModuleStackContractTests.cpp.
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
struct FNiagaraModuleInputsFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FString EmitterName;
	UNiagaraGraph* Graph = nullptr;
};

bool CreateModuleInputsFixture(FNiagaraModuleInputsFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_ModuleInputs_")
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
	// Fully initialize the System scripts so the system is coherent even though
	// the read-only input query never compiles or saves.
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);

	// An "empty" emitter: ResetGraphForOutput builds the output nodes and their
	// input-connector stack sources, but no function-call modules.
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

bool DeleteModuleInputsFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

UNiagaraNodeOutput* FindOutputNodeForUsage(UNiagaraGraph* Graph, const ENiagaraScriptUsage Usage)
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

TSharedRef<FJsonObject> MakeInputsParams(
	const FNiagaraModuleInputsFixture& Fixture,
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

UNiagaraScript* FindModuleScriptForInputs()
{
	// The engine always ships its default Module scripts under /Niagara/Modules;
	// fall back to the asset registry when that exact path is unavailable.
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraModuleInputsContractTest,
	"UE_AI_integration.Niagara.ModuleInputsContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleInputsContractTest::RunTest(const FString&)
{
	FNiagaraModuleInputsFixture Fixture;
	ON_SCOPE_EXIT
	{
		TestTrue(
			TEXT("Module-inputs fixture and package are deleted"),
			DeleteModuleInputsFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Owned module-inputs fixture builds"), CreateModuleInputsFixture(Fixture))
		|| !TestNotNull(TEXT("Module-inputs system"), Fixture.System)
		|| !TestNotNull(TEXT("Module-inputs graph"), Fixture.Graph))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	UNiagaraNodeOutput* Output = FindOutputNodeForUsage(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestNotNull(TEXT("Particle-update output resolves"), Output))
	{
		return false;
	}
	const FString ParticleUpdatePath = Output->GetPathName();

	// (a) unknown moduleSelector is rejected with the stable module_not_found
	// error surfaced by ResolveStackModule.
	const FMCPToolResult Unknown = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.inputs.list"),
		MakeInputsParams(Fixture, ParticleUpdatePath, TEXT("DoesNotExist")));
	TestFalse(TEXT("Unknown module selector is rejected"), Unknown.bSuccess);
	TestEqual(TEXT("Unknown module selector uses a stable code"),
		Unknown.ErrorCode, FString(TEXT("module_not_found")));

	// (b) a real Module script drives input enumeration. The node is inserted
	// directly through the exported AddScriptModuleToStack so the read-only
	// query never depends on a compile environment.
	UNiagaraScript* ModuleScript = FindModuleScriptForInputs();
	if (!ModuleScript || !ModuleScript->GetLatestSource())
	{
		AddInfo(TEXT("No Niagara Module script asset was found; skipping input enumeration (requires a real Module asset)."));
		return true;
	}

	UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *Output);
	if (!TestNotNull(TEXT("Module added to the stack"), Added))
	{
		AddInfo(TEXT("AddScriptModuleToStack returned no node; skipping input enumeration."));
		return true;
	}

	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.inputs.list"),
		MakeInputsParams(Fixture, ParticleUpdatePath, ModuleScript->GetPathName()));
	if (!TestTrue(TEXT("Module inputs list succeeds"), Result.bSuccess) || !Result.Data)
	{
		return false;
	}
	TestEqual(TEXT("Module inputs use the module-inputs schema"),
		Result.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.niagara.module-inputs.v1")));
	TestTrue(TEXT("Module inputs enumerate at least one input"),
		Result.Data->GetIntegerField(TEXT("inputCount")) > 0);
	TestFalse(TEXT("Read-only input readback never reports saved"),
		Result.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Read-only input readback never reports compiled"),
		Result.Data->GetBoolField(TEXT("compiled")));
	TestFalse(TEXT("Read-only input readback never reports runtime verification"),
		Result.Data->GetBoolField(TEXT("runtimeVerified")));
	TestTrue(TEXT("Read-only input readback reports bounded output"),
		Result.Data->GetBoolField(TEXT("bounded")));
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
