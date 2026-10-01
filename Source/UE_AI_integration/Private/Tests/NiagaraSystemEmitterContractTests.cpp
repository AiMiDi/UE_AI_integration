#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraTools(FMCPToolRegistry& Registry);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemEmitterRegistrationTest,
	"UE_AI_integration.Niagara.SystemEmitter.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemEmitterRegistrationTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	static const TCHAR* ExpectedCapabilities[] = {
		TEXT("content.niagara.system.save"),
		TEXT("content.niagara.emitter.remove"),
		TEXT("content.niagara.emitter.set_enabled"),
		TEXT("content.niagara.emitter.duplicate"),
	};
	for (const TCHAR* Capability : ExpectedCapabilities)
	{
		TestNotNull(Capability, Registry.FindTool(Capability));
	}
	return true;
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FNiagaraSystemEmitterFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
};

FNiagaraSystemEmitterFixture CreateNiagaraSystemEmitterFixture(const int32 EmitterCount)
{
	FNiagaraSystemEmitterFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/UEAI_SystemEmitter_")
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

	if (EmitterCount > 0)
	{
		UNiagaraEmitter* Template = LoadObject<UNiagaraEmitter>(
			nullptr,
			TEXT("/Niagara/DefaultAssets/Templates/Emitters/SingleLoopingParticle.SingleLoopingParticle"));
		if (Template)
		{
			for (int32 Index = 0; Index < EmitterCount; ++Index)
			{
				// FNiagaraEditorUtilities::AddEmitterToSystem uniquifies the
				// handle name (Empty -> Empty001 -> Empty002) and generates a
				// fresh handle id for each added emitter.
				FNiagaraEditorUtilities::AddEmitterToSystem(
					*Fixture.System,
					*Template,
					Template->GetExposedVersion().VersionGuid);
			}
		}
	}
	Fixture.Package->SetDirtyFlag(false);
	return Fixture;
}

bool DeleteNiagaraSystemEmitterFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	// The save capability writes an on-disk artifact; remove any residual file
	// directly so a save test cannot leak content between automation runs.
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(Filename))
	{
		IFileManager::Get().Delete(*Filename);
	}
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName)
		&& !FPaths::FileExists(Filename);
}

TSharedRef<FJsonObject> MakeSystemParams(const UNiagaraSystem* System)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), System->GetPathName());
	return Params;
}

FMCPToolResult ExecuteSystemTool(
	FMCPToolRegistry& Registry,
	const TCHAR* Capability,
	const UNiagaraSystem* System)
{
	return Registry.ExecuteTool(Capability, MakeSystemParams(System));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemInspectExecutionContextPagingContractTest,
	"UE_AI_integration.Niagara.SystemEmitter.InspectExecutionContextPaging",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemInspectExecutionContextPagingContractTest::RunTest(const FString&)
{
	const FNiagaraSystemEmitterFixture Fixture = CreateNiagaraSystemEmitterFixture(1);
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Inspect paging fixture and package are deleted"),
			DeleteNiagaraSystemEmitterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Inspect paging Niagara System fixture"), Fixture.System)
		|| Fixture.System->GetEmitterHandles().Num() == 0)
	{
		AddInfo(TEXT("The Niagara template emitter is unavailable; skipping execution-context paging."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	auto Params = MakeSystemParams(Fixture.System);
	Params->SetNumberField(TEXT("eventHandlerOffset"), 0);
	Params->SetNumberField(TEXT("eventHandlerLimit"), 1);
	Params->SetNumberField(TEXT("simulationStageOffset"), 0);
	Params->SetNumberField(TEXT("simulationStageLimit"), 1);
	const FMCPToolResult FirstPage = Registry.ExecuteTool(
		TEXT("content.niagara.system.inspect"), Params);
	if (!TestTrue(TEXT("System inspect execution-context page succeeds"), FirstPage.bSuccess)
		|| !TestNotNull(TEXT("System inspect returns data"), FirstPage.Data.Get()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	if (!TestTrue(TEXT("System inspect returns emitter rows"),
		FirstPage.Data->TryGetArrayField(TEXT("emitters"), Emitters)
			&& Emitters != nullptr && Emitters->Num() == 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Emitter = (*Emitters)[0]->AsObject();
	if (!TestTrue(TEXT("System inspect emitter row is an object"), Emitter.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Event-handler page offset is echoed"),
		Emitter->GetIntegerField(TEXT("eventHandlerOffset")), 0);
	TestEqual(TEXT("Event-handler page limit is echoed"),
		Emitter->GetIntegerField(TEXT("eventHandlerLimit")), 1);
	TestEqual(TEXT("Simulation-stage page offset is echoed"),
		Emitter->GetIntegerField(TEXT("simulationStageOffset")), 0);
	TestEqual(TEXT("Simulation-stage page limit is echoed"),
		Emitter->GetIntegerField(TEXT("simulationStageLimit")), 1);
	TestTrue(TEXT("Event-handler pagination reports a continuation state"),
		Emitter->HasField(TEXT("eventHandlersHasMore"))
			&& Emitter->HasField(TEXT("eventHandlersNextOffset")));
	TestTrue(TEXT("Simulation-stage pagination reports a continuation state"),
		Emitter->HasField(TEXT("simulationStagesHasMore"))
			&& Emitter->HasField(TEXT("simulationStagesNextOffset")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSaveContractTest,
	"UE_AI_integration.Niagara.SystemEmitter.SaveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSaveContractTest::RunTest(const FString&)
{
	const FNiagaraSystemEmitterFixture Fixture =
		CreateNiagaraSystemEmitterFixture(0);
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Save fixture and package are deleted"),
			DeleteNiagaraSystemEmitterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Save Niagara System fixture"), Fixture.System)
		|| !TestNotNull(TEXT("Save fixture package"), Fixture.Package))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	// Drain any initial compilation so the save itself never has to block on the
	// Niagara compile pipeline (UNiagaraSystem::PreSave waits for compilation).
	Fixture.System->WaitForCompilationComplete(false, false);
	Fixture.Package->MarkPackageDirty();
	TestTrue(TEXT("Fixture package is dirty before save"), Fixture.Package->IsDirty());

	const FMCPToolResult Saved = ExecuteSystemTool(
		Registry, TEXT("content.niagara.system.save"), Fixture.System);
	if (!TestTrue(TEXT("Niagara System save succeeds"), Saved.bSuccess)
		|| !TestNotNull(TEXT("Save returns data"), Saved.Data.Get()))
	{
		return false;
	}
	TestTrue(TEXT("Save reports saved"), Saved.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Save reports clean dirty state"), Saved.Data->GetBoolField(TEXT("dirty")));
	TestFalse(TEXT("Save clears the package dirty flag"), Fixture.Package->IsDirty());
	const FString FilePath = Saved.Data->GetStringField(TEXT("file"));
	TestTrue(TEXT("Save reports a file path"), !FilePath.IsEmpty());
	TestTrue(TEXT("Save reports the package path"),
		Saved.Data->GetStringField(TEXT("package")) == Fixture.PackageName);
	TestEqual(TEXT("Save reports a SHA-256 file digest"),
		Saved.Data->GetStringField(TEXT("sha256")).Len(), 64);
	TestTrue(TEXT("Saved file exists on disk"), FPaths::FileExists(FilePath));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraEmitterRemoveContractTest,
	"UE_AI_integration.Niagara.SystemEmitter.RemoveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEmitterRemoveContractTest::RunTest(const FString&)
{
	const FNiagaraSystemEmitterFixture Fixture =
		CreateNiagaraSystemEmitterFixture(2);
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Remove fixture and package are deleted"),
			DeleteNiagaraSystemEmitterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Remove Niagara System fixture"), Fixture.System)
		|| !TestEqual(TEXT("Two owned emitters are added"),
			Fixture.System->GetEmitterHandles().Num(), 2))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	const FString RemovedId =
		Fixture.System->GetEmitterHandles()[0].GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString RemainingId =
		Fixture.System->GetEmitterHandles()[1].GetId().ToString(EGuidFormats::DigitsWithHyphensLower);

	auto Params = MakeSystemParams(Fixture.System);
	Params->SetStringField(TEXT("emitterHandleId"), RemovedId);
	const FMCPToolResult Removed = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.remove"), Params);
	if (!TestTrue(TEXT("Emitter remove succeeds"), Removed.bSuccess)
		|| !TestNotNull(TEXT("Remove returns data"), Removed.Data.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Remove reports one remaining emitter"),
		Removed.Data->GetIntegerField(TEXT("emitterCount")), 1);
	TestEqual(TEXT("Remove reports the removed handle id"),
		Removed.Data->GetStringField(TEXT("removedHandleId")), RemovedId);
	TestEqual(TEXT("System actually has one remaining emitter"),
		Fixture.System->GetEmitterHandles().Num(), 1);
	TestTrue(TEXT("Remaining emitter is the expected one"),
		Fixture.System->GetEmitterHandles()[0].GetId().ToString(EGuidFormats::DigitsWithHyphensLower)
			== RemainingId);
	TestTrue(TEXT("Remove dirties the package"), Fixture.Package->IsDirty());

	// Unknown-but-valid GUIDs are a not-found error; malformed GUIDs are invalid.
	auto MissingParams = MakeSystemParams(Fixture.System);
	MissingParams->SetStringField(TEXT("emitterHandleId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	const FMCPToolResult Missing = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.remove"), MissingParams);
	TestFalse(TEXT("Unknown handle id is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Unknown handle id uses a stable code"),
		Missing.ErrorCode, FString(TEXT("emitter_handle_not_found")));
	TestEqual(TEXT("Unknown handle id does not change the emitter count"),
		Fixture.System->GetEmitterHandles().Num(), 1);

	auto InvalidParams = MakeSystemParams(Fixture.System);
	InvalidParams->SetStringField(TEXT("emitterHandleId"), TEXT("not-a-guid"));
	const FMCPToolResult Invalid = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.remove"), InvalidParams);
	TestFalse(TEXT("Malformed handle id is rejected"), Invalid.bSuccess);
	TestEqual(TEXT("Malformed handle id uses a stable code"),
		Invalid.ErrorCode, FString(TEXT("invalid_emitter_handle_id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraEmitterSetEnabledContractTest,
	"UE_AI_integration.Niagara.SystemEmitter.SetEnabledContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEmitterSetEnabledContractTest::RunTest(const FString&)
{
	const FNiagaraSystemEmitterFixture Fixture =
		CreateNiagaraSystemEmitterFixture(1);
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Set-enabled fixture and package are deleted"),
			DeleteNiagaraSystemEmitterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Set-enabled Niagara System fixture"), Fixture.System)
		|| !TestEqual(TEXT("One owned emitter is added"),
			Fixture.System->GetEmitterHandles().Num(), 1))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	const FNiagaraEmitterHandle& Handle = Fixture.System->GetEmitterHandles()[0];
	const FString HandleId =
		Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	TestTrue(TEXT("New emitter is enabled by default"), Handle.GetIsEnabled());

	auto Params = MakeSystemParams(Fixture.System);
	Params->SetStringField(TEXT("emitterHandleId"), HandleId);
	Params->SetBoolField(TEXT("enabled"), false);
	const FMCPToolResult Disabled = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.set_enabled"), Params);
	if (!TestTrue(TEXT("Disable emitter succeeds"), Disabled.bSuccess)
		|| !TestNotNull(TEXT("Disable returns data"), Disabled.Data.Get()))
	{
		return false;
	}
	TestFalse(TEXT("Disable reports disabled state"),
		Disabled.Data->GetBoolField(TEXT("enabled")));
	TestTrue(TEXT("Disable reports a state change"),
		Disabled.Data->GetBoolField(TEXT("changed")));
	TestEqual(TEXT("Disable reports the handle id"),
		Disabled.Data->GetStringField(TEXT("emitterHandleId")), HandleId);
	TestFalse(TEXT("Emitter is actually disabled"),
		Fixture.System->GetEmitterHandles()[0].GetIsEnabled());
	TestTrue(TEXT("Disable dirties the package"), Fixture.Package->IsDirty());

	// Re-requesting the same state is a no-op that still succeeds.
	const FMCPToolResult NoOp = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.set_enabled"), Params);
	if (TestTrue(TEXT("No-op disable succeeds"), NoOp.bSuccess) && NoOp.Data)
	{
		TestFalse(TEXT("No-op disable reports no state change"),
			NoOp.Data->GetBoolField(TEXT("changed")));
	}

	Params->SetBoolField(TEXT("enabled"), true);
	const FMCPToolResult ReEnabled = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.set_enabled"), Params);
	if (TestTrue(TEXT("Re-enable emitter succeeds"), ReEnabled.bSuccess)
		&& ReEnabled.Data)
	{
		TestTrue(TEXT("Re-enable reports enabled state"),
			ReEnabled.Data->GetBoolField(TEXT("enabled")));
		TestTrue(TEXT("Re-enable reports a state change"),
			ReEnabled.Data->GetBoolField(TEXT("changed")));
	}
	TestTrue(TEXT("Emitter is actually re-enabled"),
		Fixture.System->GetEmitterHandles()[0].GetIsEnabled());

	// Malformed GUIDs and unknown handles are rejected with stable codes.
	auto InvalidParams = MakeSystemParams(Fixture.System);
	InvalidParams->SetStringField(TEXT("emitterHandleId"), TEXT("bad-guid"));
	InvalidParams->SetBoolField(TEXT("enabled"), false);
	const FMCPToolResult Invalid = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.set_enabled"), InvalidParams);
	TestFalse(TEXT("Malformed handle id is rejected"), Invalid.bSuccess);
	TestEqual(TEXT("Malformed handle id uses a stable code"),
		Invalid.ErrorCode, FString(TEXT("invalid_emitter_handle_id")));

	auto MissingParams = MakeSystemParams(Fixture.System);
	MissingParams->SetStringField(TEXT("emitterHandleId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	MissingParams->SetBoolField(TEXT("enabled"), false);
	const FMCPToolResult Missing = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.set_enabled"), MissingParams);
	TestFalse(TEXT("Unknown handle id is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Unknown handle id uses a stable code"),
		Missing.ErrorCode, FString(TEXT("emitter_handle_not_found")));

	// A missing enabled field is rejected before any handle resolution.
	const FMCPToolResult MissingEnabled = ExecuteSystemTool(
		Registry, TEXT("content.niagara.emitter.set_enabled"), Fixture.System);
	TestFalse(TEXT("Missing enabled field is rejected"), MissingEnabled.bSuccess);
	TestEqual(TEXT("Missing enabled field uses a stable code"),
		MissingEnabled.ErrorCode, FString(TEXT("invalid_enabled_state")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraEmitterDuplicateContractTest,
	"UE_AI_integration.Niagara.SystemEmitter.DuplicateContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraEmitterDuplicateContractTest::RunTest(const FString&)
{
	const FNiagaraSystemEmitterFixture Fixture =
		CreateNiagaraSystemEmitterFixture(1);
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("Duplicate fixture and package are deleted"),
			DeleteNiagaraSystemEmitterFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Duplicate Niagara System fixture"), Fixture.System)
		|| !TestEqual(TEXT("One owned emitter is added"),
			Fixture.System->GetEmitterHandles().Num(), 1))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();

	const FNiagaraEmitterHandle& Source = Fixture.System->GetEmitterHandles()[0];
	const FString SourceId =
		Source.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString SourceName = Source.GetName().ToString();

	auto Params = MakeSystemParams(Fixture.System);
	Params->SetStringField(TEXT("emitterHandleId"), SourceId);
	const FMCPToolResult Duplicated = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.duplicate"), Params);
	if (!TestTrue(TEXT("Emitter duplicate succeeds"), Duplicated.bSuccess)
		|| !TestNotNull(TEXT("Duplicate returns data"), Duplicated.Data.Get()))
	{
		return false;
	}
	const FString NewId = Duplicated.Data->GetStringField(TEXT("emitterHandleId"));
	const FString NewName = Duplicated.Data->GetStringField(TEXT("emitterName"));
	TestEqual(TEXT("Duplicate reports the source handle id"),
		Duplicated.Data->GetStringField(TEXT("sourceEmitterHandleId")), SourceId);
	TestNotEqual(TEXT("Duplicate returns a distinct handle id"), NewId, SourceId);
	TestNotEqual(TEXT("Duplicate returns a distinct handle name"), NewName, SourceName);
	TestTrue(TEXT("Duplicate name uses the _Copy suffix"),
		NewName.Contains(TEXT("_Copy")));
	TestEqual(TEXT("Duplicate reports two emitters"),
		Duplicated.Data->GetIntegerField(TEXT("emitterCount")), 2);
	TestEqual(TEXT("System actually has two emitters"),
		Fixture.System->GetEmitterHandles().Num(), 2);
	TestTrue(TEXT("Duplicate dirties the package"), Fixture.Package->IsDirty());

	// A second duplicate must avoid a name collision with the first copy.
	const FMCPToolResult Reduplicated = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.duplicate"), Params);
	if (TestTrue(TEXT("Second emitter duplicate succeeds"), Reduplicated.bSuccess)
		&& Reduplicated.Data)
	{
		TestNotEqual(
			TEXT("Second duplicate gets a distinct name"),
			Reduplicated.Data->GetStringField(TEXT("emitterName")), NewName);
		TestEqual(TEXT("Second duplicate reports three emitters"),
			Reduplicated.Data->GetIntegerField(TEXT("emitterCount")), 3);
	}
	TestEqual(TEXT("System actually has three emitters"),
		Fixture.System->GetEmitterHandles().Num(), 3);

	// Malformed GUIDs and unknown handles are rejected with stable codes.
	auto InvalidParams = MakeSystemParams(Fixture.System);
	InvalidParams->SetStringField(TEXT("emitterHandleId"), TEXT("bad-guid"));
	const FMCPToolResult Invalid = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.duplicate"), InvalidParams);
	TestFalse(TEXT("Malformed handle id is rejected"), Invalid.bSuccess);
	TestEqual(TEXT("Malformed handle id uses a stable code"),
		Invalid.ErrorCode, FString(TEXT("invalid_emitter_handle_id")));

	auto MissingParams = MakeSystemParams(Fixture.System);
	MissingParams->SetStringField(TEXT("emitterHandleId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	const FMCPToolResult Missing = Registry.ExecuteTool(
		TEXT("content.niagara.emitter.duplicate"), MissingParams);
	TestFalse(TEXT("Unknown handle id is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Unknown handle id uses a stable code"),
		Missing.ErrorCode, FString(TEXT("emitter_handle_not_found")));
	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
