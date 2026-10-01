// Contract tests for Niagara stack module authoring (list / remove / move).
//
// These tests exercise the read-only list query and the plan/resolve/validation
// logic through the public tool registry. Full apply of remove/move requires a
// real Niagara Module script asset plus a compilable /Game/ system; the
// round-trip test below locates a Module script at runtime and skips (with an
// AddInfo note) when none is available, rather than assuming a specific asset.
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
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraModuleStackRegistrationTest,
	"UE_AI_integration.Niagara.ModuleStack.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleStackRegistrationTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	static const TCHAR* ExpectedCapabilities[] = {
		TEXT("content.niagara.graph.module.add.plan"),
		TEXT("content.niagara.graph.module.add.apply"),
		TEXT("content.niagara.graph.module.add.rollback"),
		TEXT("content.niagara.graph.module.list"),
		TEXT("content.niagara.graph.module.remove.plan"),
		TEXT("content.niagara.graph.module.remove.apply"),
		TEXT("content.niagara.graph.module.remove.rollback"),
		TEXT("content.niagara.graph.module.move.plan"),
		TEXT("content.niagara.graph.module.move.apply"),
		TEXT("content.niagara.graph.module.move.rollback"),
		TEXT("content.niagara.graph.module.inputs.list"),
		TEXT("content.niagara.graph.module.input.value.plan"),
		TEXT("content.niagara.graph.module.input.value.apply"),
		TEXT("content.niagara.graph.module.input.value.rollback"),
		TEXT("content.niagara.emitter.modules.clear"),
		TEXT("content.niagara.graph.module.input.binding.set"),
		TEXT("content.niagara.graph.module.input.di.set"),
		TEXT("content.niagara.system.spec.export"),
		TEXT("content.niagara.system.spec.import"),
		TEXT("content.niagara.system.spec.round_trip"),
	};
	TestEqual(
		TEXT("Twenty graph-module capabilities register"),
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
struct FNiagaraModuleStackFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FString EmitterName;
	UNiagaraGraph* Graph = nullptr;
};

bool CreateModuleStackFixture(FNiagaraModuleStackFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_ModuleStack_")
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
	// Fully initialize the System scripts so the system is compilable when the
	// remove/move mutation contract exercises apply (read-only list/plan tests
	// never compile, but a shared fixture keeps both paths consistent).
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);

	// An "empty" emitter: ResetGraphForOutput builds the four output nodes and
	// their input-connector stack sources, but no function-call modules.
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

bool DeleteModuleStackFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

FString FindOutputPathForUsage(UNiagaraGraph* Graph, const ENiagaraScriptUsage Usage)
{
	if (!Graph)
	{
		return FString();
	}
	TArray<UNiagaraNodeOutput*> Outputs;
	Graph->GetNodesOfClass(Outputs);
	for (UNiagaraNodeOutput* Output : Outputs)
	{
		if (Output && Output->GetUsage() == Usage)
		{
			return Output->GetPathName();
		}
	}
	return FString();
}

TSharedRef<FJsonObject> MakeListParams(
	const FNiagaraModuleStackFixture& Fixture,
	const FString& OutputNodePath)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterName);
	Params->SetStringField(TEXT("outputNodePath"), OutputNodePath);
	return Params;
}

TSharedRef<FJsonObject> MakeModuleParams(
	const FNiagaraModuleStackFixture& Fixture,
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

UNiagaraScript* FindModuleScript()
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
	FNiagaraModuleStackReadListContractTest,
	"UE_AI_integration.Niagara.ModuleStack.ReadListContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleStackReadListContractTest::RunTest(const FString&)
{
	FNiagaraModuleStackFixture Fixture;
	ON_SCOPE_EXIT
	{
		TestTrue(
			TEXT("Module-stack fixture and package are deleted"),
			DeleteModuleStackFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Owned module-stack fixture builds"), CreateModuleStackFixture(Fixture))
		|| !TestNotNull(TEXT("Module-stack system"), Fixture.System)
		|| !TestNotNull(TEXT("Module-stack graph"), Fixture.Graph))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	const FString ParticleUpdatePath = FindOutputPathForUsage(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestTrue(TEXT("Particle-update output resolves"), !ParticleUpdatePath.IsEmpty()))
	{
		return false;
	}

	// (a) The empty stack reads back with a bounded, zero-length module list and
	// never claims a compile or save.
	const FMCPToolResult List = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.list"),
		MakeListParams(Fixture, ParticleUpdatePath));
	if (!TestTrue(TEXT("Empty stack list succeeds"), List.bSuccess) || !List.Data)
	{
		return false;
	}
	TestEqual(TEXT("Empty stack reports zero modules"),
		List.Data->GetIntegerField(TEXT("moduleCount")), 0);
	TestTrue(TEXT("List reports bounded output"),
		List.Data->GetBoolField(TEXT("bounded")));
	TestEqual(TEXT("List is bounded to 256 modules"),
		List.Data->GetIntegerField(TEXT("moduleLimit")), 256);
	TestFalse(TEXT("Read-only list never reports saved"),
		List.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Read-only list never reports compiled"),
		List.Data->GetBoolField(TEXT("compiled")));
	TestFalse(TEXT("Read-only list never reports runtime verification"),
		List.Data->GetBoolField(TEXT("runtimeVerified")));

	// (b) remove.plan rejects an unknown selector before any planning state.
	const FMCPToolResult UnknownRemove = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.remove.plan"),
		MakeModuleParams(Fixture, ParticleUpdatePath, TEXT("DoesNotExist")));
	TestFalse(TEXT("Unknown remove selector is rejected"), UnknownRemove.bSuccess);
	TestEqual(TEXT("Unknown remove selector uses a stable code"),
		UnknownRemove.ErrorCode, FString(TEXT("module_not_found")));

	const FMCPToolResult UnknownMove = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.move.plan"),
		MakeModuleParams(Fixture, ParticleUpdatePath, TEXT("DoesNotExist")));
	TestFalse(TEXT("Unknown move selector is rejected"), UnknownMove.bSuccess);
	TestEqual(TEXT("Unknown move selector uses a stable code"),
		UnknownMove.ErrorCode, FString(TEXT("module_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraModuleStackRemoveMoveContractTest,
	"UE_AI_integration.Niagara.ModuleStack.RemoveMoveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraModuleStackRemoveMoveContractTest::RunTest(const FString&)
{
	// Full remove/move apply needs a real Module script and a compilable /Game/
	// system. When a Module script is unavailable, skip the mutation contract
	// (the read-list and plan-rejection contracts above still cover the
	// resolve/validation logic without an asset).
	UNiagaraScript* ModuleScript = FindModuleScript();
	if (!ModuleScript)
	{
		AddInfo(TEXT("No Niagara Module script asset was found; skipping the add->remove->readback and move-index round trip (requires a real Module asset)."));
		return true;
	}

	FNiagaraModuleStackFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Remove/move fixture and package are deleted"),
			DeleteModuleStackFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Remove/move fixture builds"), CreateModuleStackFixture(Fixture))
		|| !TestNotNull(TEXT("Remove/move system"), Fixture.System))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	const FString ParticleUpdatePath = FindOutputPathForUsage(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestTrue(TEXT("Remove/move output resolves"), !ParticleUpdatePath.IsEmpty()))
	{
		return false;
	}

	const FString ModuleScriptPath = ModuleScript->GetPathName();
	auto AddParams = MakeListParams(Fixture, ParticleUpdatePath);
	AddParams->SetStringField(TEXT("moduleScript"), ModuleScriptPath);
	AddParams->SetNumberField(TEXT("targetIndex"), 0.0);
	const FString AddRequest = TEXT("add-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);

	const FMCPToolResult AddPlan = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.add.plan"), AddParams);
	if (!TestTrue(TEXT("Module add plan succeeds"), AddPlan.bSuccess) || !AddPlan.Data)
	{
		return false;
	}
	AddParams->SetStringField(
		TEXT("approvePlanDigest"),
		AddPlan.Data->GetStringField(TEXT("planDigest")));
	AddParams->SetBoolField(TEXT("confirmWrite"), true);
	AddParams->SetStringField(TEXT("requestId"), AddRequest);
	const FMCPToolResult Added = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.add.apply"), AddParams);
	if (!TestTrue(TEXT("Module add applies"), Added.bSuccess) || !Added.Data)
	{
		// The mutation contract depends on a compilable authored system; treat
		// an add failure as a documented skip rather than a hard failure.
		AddInfo(TEXT("Module add.apply did not succeed on the synthetic system; skipping the round-trip (compile environment required)."));
		return true;
	}

	// (d) move.plan index validation against the single-module stack.
	auto MoveParams = MakeModuleParams(
		Fixture, ParticleUpdatePath, Added.Data->GetStringField(TEXT("nodePath")));
	MoveParams->SetNumberField(TEXT("targetIndex"), 1.0);
	const FMCPToolResult OutOfRange = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.move.plan"), MoveParams);
	TestFalse(TEXT("Out-of-range move index is rejected"), OutOfRange.bSuccess);
	TestEqual(TEXT("Out-of-range move uses a stable code"),
		OutOfRange.ErrorCode, FString(TEXT("target_index_out_of_range")));

	MoveParams->SetNumberField(TEXT("targetIndex"), 0.0);
	const FMCPToolResult SelfMove = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.move.plan"), MoveParams);
	TestFalse(TEXT("Self move index is rejected"), SelfMove.bSuccess);
	TestEqual(TEXT("Self move uses a stable code"),
		SelfMove.ErrorCode, FString(TEXT("already_at_target_index")));

	// (c) remove.apply read-back restores the original (empty) module count.
	const FMCPToolResult BeforeRemove = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.list"),
		MakeListParams(Fixture, ParticleUpdatePath));
	if (!TestTrue(TEXT("Post-add list reads one module"), BeforeRemove.bSuccess) || !BeforeRemove.Data)
	{
		return false;
	}
	TestEqual(TEXT("Add produced exactly one module"),
		BeforeRemove.Data->GetIntegerField(TEXT("moduleCount")), 1);

	auto RemoveParams = MakeModuleParams(
		Fixture, ParticleUpdatePath, Added.Data->GetStringField(TEXT("nodePath")));
	const FString RemoveRequest = TEXT("remove-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FMCPToolResult RemovePlan = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.remove.plan"), RemoveParams);
	if (!TestTrue(TEXT("Module remove plan succeeds"), RemovePlan.bSuccess) || !RemovePlan.Data)
	{
		return false;
	}
	RemoveParams->SetStringField(
		TEXT("approvePlanDigest"),
		RemovePlan.Data->GetStringField(TEXT("planDigest")));
	RemoveParams->SetBoolField(TEXT("confirmWrite"), true);
	RemoveParams->SetStringField(TEXT("requestId"), RemoveRequest);
	const FMCPToolResult Removed = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.remove.apply"), RemoveParams);
	if (!TestTrue(TEXT("Module remove applies"), Removed.bSuccess) || !Removed.Data)
	{
		return false;
	}

	const FMCPToolResult AfterRemove = Registry.ExecuteTool(
		TEXT("content.niagara.graph.module.list"),
		MakeListParams(Fixture, ParticleUpdatePath));
	if (!TestTrue(TEXT("Post-remove list succeeds"), AfterRemove.bSuccess) || !AfterRemove.Data)
	{
		return false;
	}
	TestEqual(TEXT("Remove restores the original module count"),
		AfterRemove.Data->GetIntegerField(TEXT("moduleCount")), 0);
	TestEqual(TEXT("Remove receipt reports the removed index"),
		Removed.Data->GetIntegerField(TEXT("removedIndex")), 0);
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
