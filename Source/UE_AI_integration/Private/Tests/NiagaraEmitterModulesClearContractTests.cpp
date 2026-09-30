// Contract tests for content.niagara.emitter.modules.clear.
//
// The clear command removes every function-call module node from every stack
// of one emitter. These tests exercise the resolve / write-guard / idempotency
// contract through the public tool registry: an unknown system or emitter
// yields a stable error code, an empty emitter is an idempotent no-op success
// (removedCount 0), and a real Module-script round trip (add one module, then
// clear) verifies the removedCount/remainingCount read-back. The module round
// trip is skipped with an AddInfo note when no Module script asset is
// available, rather than assuming a specific asset. No runtime/PIE is assumed.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
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
struct FNiagaraEmitterModulesClearFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FString EmitterName;
	FString EmitterId;
	UNiagaraGraph* Graph = nullptr;
};

bool CreateNiagaraEmitterModulesClearFixture(FNiagaraEmitterModulesClearFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_EmitterModulesClear_")
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

	// An "empty" emitter: InitializeEmitter builds the output nodes and their
	// input-connector stack sources, but no function-call modules.
	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		OutFixture.System, TEXT("EmptyEmitter"), RF_Transactional);
	if (!Emitter)
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FNiagaraEmitterHandle LocalHandle(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	OutFixture.System->AddEmitterHandleDirect(LocalHandle);

	// Re-read the handle from the System so name/id match what the command
	// resolves when it enumerates GetEmitterHandles().
	const FNiagaraEmitterHandle* Handle = nullptr;
	for (const FNiagaraEmitterHandle& Candidate : OutFixture.System->GetEmitterHandles())
	{
		Handle = &Candidate;
		break;
	}
	if (!Handle)
	{
		return false;
	}
	OutFixture.EmitterName = Handle->GetName().ToString();
	OutFixture.EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
	if (!Data)
	{
		return false;
	}
	OutFixture.Graph = Cast<UNiagaraScriptSource>(Data->GraphSource)->NodeGraph;
	OutFixture.Package->SetDirtyFlag(false);
	return OutFixture.Graph != nullptr;
}

bool DeleteNiagaraEmitterModulesClearFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

TSharedRef<FJsonObject> MakeNiagaraEmitterModulesClearParams(
	const FString& SystemPath,
	const FString& EmitterSelector)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), SystemPath);
	Params->SetStringField(TEXT("emitter"), EmitterSelector);
	return Params;
}

UNiagaraNodeOutput* FindNiagaraEmitterModulesClearOutput(
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

UNiagaraScript* FindNiagaraEmitterModulesClearScript()
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
	FNiagaraEmitterModulesClearContractTest,
	"UE_AI_integration.Niagara.EmitterModulesClear.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEmitterModulesClearContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	// (a) unknown system -> system_not_found.
	{
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.emitter.modules.clear"),
			MakeNiagaraEmitterModulesClearParams(
				TEXT("/Game/Automation/UEAI_DoesNotExist.Missing"),
				TEXT("Anything")));
		TestFalse(TEXT("Unknown system is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown system uses a stable code"),
			Result.ErrorCode, FString(TEXT("system_not_found")));
	}

	FNiagaraEmitterModulesClearFixture Fixture;
	ON_SCOPE_EXIT
	{
		TestTrue(
			TEXT("Emitter-modules-clear fixture and package are deleted"),
			DeleteNiagaraEmitterModulesClearFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Emitter-modules-clear fixture builds"), CreateNiagaraEmitterModulesClearFixture(Fixture))
		|| !TestNotNull(TEXT("Emitter-modules-clear system"), Fixture.System)
		|| !TestNotNull(TEXT("Emitter-modules-clear graph"), Fixture.Graph))
	{
		return false;
	}

	// (b) unknown emitter -> emitter_not_found.
	{
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.emitter.modules.clear"),
			MakeNiagaraEmitterModulesClearParams(
				Fixture.System->GetPathName(),
				TEXT("EmitterThatDoesNotExist")));
		TestFalse(TEXT("Unknown emitter is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unknown emitter uses a stable code"),
			Result.ErrorCode, FString(TEXT("emitter_not_found")));
	}

	// (c) empty emitter (no modules) -> idempotent success with removedCount 0.
	{
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("content.niagara.emitter.modules.clear"),
			MakeNiagaraEmitterModulesClearParams(
				Fixture.System->GetPathName(),
				Fixture.EmitterName));
		if (!TestTrue(TEXT("Empty emitter clear succeeds idempotently"), Result.bSuccess) || !Result.Data)
		{
			return false;
		}
		TestEqual(TEXT("Empty emitter reports zero removed modules"),
			Result.Data->GetIntegerField(TEXT("removedCount")), 0);
		TestEqual(TEXT("Empty emitter reports zero remaining modules"),
			Result.Data->GetIntegerField(TEXT("remainingCount")), 0);
		TestEqual(TEXT("Clear echoes the emitter name"),
			Result.Data->GetStringField(TEXT("emitter")), Fixture.EmitterName);
		TestEqual(TEXT("Clear echoes the emitter handle id"),
			Result.Data->GetStringField(TEXT("emitterId")), Fixture.EmitterId);
		TestFalse(TEXT("Clear never saves"),
			Result.Data->GetBoolField(TEXT("saved")));
	}

	// (d) if a real Module script is found, add one module then clear and
	// assert removedCount >= 1 and remainingCount == 0.
	UNiagaraScript* ModuleScript = FindNiagaraEmitterModulesClearScript();
	if (!ModuleScript)
	{
		AddInfo(TEXT("No Niagara Module script asset was found; skipping the add-one-module-then-clear round trip (requires a real Module asset)."));
		return true;
	}
	UNiagaraNodeOutput* UpdateOutput = FindNiagaraEmitterModulesClearOutput(
		Fixture.Graph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!TestTrue(TEXT("Particle-update output resolves for module round trip"), UpdateOutput != nullptr))
	{
		return false;
	}
	UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *UpdateOutput, 0);
	if (!Added)
	{
		AddInfo(TEXT("AddScriptModuleToStack did not produce a node on the synthetic system; skipping the clear round trip (compile environment required)."));
		return true;
	}

	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.modules.clear"),
		MakeNiagaraEmitterModulesClearParams(
			Fixture.System->GetPathName(),
			Fixture.EmitterName));
	if (!TestTrue(TEXT("Module clear succeeds after adding a module"), Result.bSuccess) || !Result.Data)
	{
		return false;
	}
	TestTrue(TEXT("Clear removed at least one module"),
		Result.Data->GetIntegerField(TEXT("removedCount")) >= 1);
	TestEqual(TEXT("Clear leaves zero modules"),
		Result.Data->GetIntegerField(TEXT("remainingCount")), 0);
	TestTrue(TEXT("Clear reports the authored stack compiled"),
		Result.Data->GetBoolField(TEXT("compiled")));
	TestFalse(TEXT("Clear reports dirty-only persistence"),
		Result.Data->GetBoolField(TEXT("saved")));
	return true;
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
