#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraRendererMaterialTools(FMCPToolRegistry& Registry);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraRendererMaterialRegistrationTest,
	"UE_AI_integration.Niagara.RendererMaterial.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraRendererMaterialRegistrationTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRendererMaterialTools(Registry);
	Registry.EndDomainRegistration();

	static const TCHAR* ExpectedCapabilities[] = {
		TEXT("content.niagara.renderer.list"),
		TEXT("content.niagara.renderer.materials.get"),
		TEXT("content.niagara.renderer.material.plan"),
		TEXT("content.niagara.renderer.material.apply"),
		TEXT("content.niagara.renderer.material.rollback"),
		TEXT("content.niagara.renderer.material.receipt.release"),
		TEXT("content.niagara.renderer.add"),
		TEXT("content.niagara.renderer.remove"),
		TEXT("content.niagara.renderer.property.set"),
		TEXT("content.niagara.renderer.bindings.get"),
		TEXT("content.niagara.renderer.bindings.set"),
	};
	TestEqual(TEXT("Exactly eleven renderer material and renderer-structure capabilities register"),
		Registry.Num(), static_cast<int32>(UE_ARRAY_COUNT(ExpectedCapabilities)));
	for (const TCHAR* Capability : ExpectedCapabilities)
	{
		TestNotNull(Capability, Registry.FindTool(Capability));
	}
	return true;
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
TSharedRef<FJsonObject> MakeRendererMaterialParams(
	UNiagaraSystem* System,
	const FNiagaraEmitterHandle& Handle,
	UNiagaraRendererProperties* Renderer,
	const FString& Material,
	bool bEnableOverrides = false)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), System->GetPathName());
	Params->SetStringField(
		TEXT("emitter"),
		Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetStringField(TEXT("rendererPath"), Renderer->GetPathName());
	Params->SetNumberField(TEXT("materialSlot"), 0);
	Params->SetStringField(TEXT("material"), Material);
	if (bEnableOverrides)
	{
		Params->SetBoolField(TEXT("enableMaterialOverrides"), true);
	}
	return Params;
}

FMCPToolResult PlanAndApproveRendererMaterial(
	FMCPToolRegistry& Registry,
	const TSharedRef<FJsonObject>& Params,
	const FString& RequestId)
{
	// Registry dispatch intentionally bypasses manifest validation. Keep the
	// plan request transport-valid when this helper is reused after an apply.
	Params->RemoveField(TEXT("approvePlanDigest"));
	Params->RemoveField(TEXT("confirmWrite"));
	Params->RemoveField(TEXT("requestId"));
	const FMCPToolResult Plan = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.plan"), Params);
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

FMCPToolResult RollbackRendererMaterial(
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
		TEXT("content.niagara.renderer.material.rollback"), Params);
}

FMCPToolResult ReleaseRendererMaterialReceipt(
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
		TEXT("content.niagara.renderer.material.receipt.release"), Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraRendererMaterialSemanticContractTest,
	"UE_AI_integration.Niagara.RendererMaterial.SemanticContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraRendererMaterialSemanticContractTest::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/Automation/UEAI_RendererMaterial_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	ON_SCOPE_EXIT
	{
		if (System)
		{
			System->WaitForCompilationComplete(false, false);
		}
		const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
			|| UEditorAssetLibrary::DeleteAsset(PackageName);
		TestTrue(TEXT("Renderer material fixture is deleted"),
			bDeleted
			&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
			&& !FPackageName::DoesPackageExist(PackageName));
	};
	if (!TestNotNull(TEXT("Owned Niagara System fixture"), System))
	{
		return false;
	}
	// Keep this fixture independent of optional Niagara DefaultAssets content.
	UNiagaraSystemFactoryNew::InitializeSystem(System, false);
	UNiagaraScript* SystemSpawnScript = System->GetSystemSpawnScript();
	UNiagaraScript* SystemUpdateScript = System->GetSystemUpdateScript();
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
	FAssetRegistryModule::AssetCreated(System);

	// Keep this fixture self-contained.  Isolated HostProjects do not mount the
	// optional Niagara DefaultAssets bundle, and renderer material authoring only
	// needs an owned material interface to exercise slot/rollback semantics.
	UNiagaraEmitter* EmitterFixture = NewObject<UNiagaraEmitter>(
		System, TEXT("MaterialEmitter"), RF_Transactional);
	UMaterial* MaterialFixture = NewObject<UMaterial>(
		Package, TEXT("MaterialFixture"), RF_Transactional);
	if (!TestNotNull(TEXT("Owned emitter fixture"), EmitterFixture)
		|| !TestNotNull(TEXT("Owned material fixture"), MaterialFixture))
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(EmitterFixture, false);
	const FGuid EmitterVersion = EmitterFixture->GetExposedVersion().VersionGuid;
	FNiagaraEmitterHandle EmitterHandle(*EmitterFixture, EmitterVersion);
	System->AddEmitterHandleDirect(EmitterHandle);
	if (!TestEqual(TEXT("One owned emitter is added"),
		System->GetEmitterHandles().Num(), 1))
	{
		return false;
	}
	const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[0];
	UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
	const FGuid Version = Handle.GetInstance().Version;
	if (!TestNotNull(TEXT("Owned emitter instance"), Emitter))
	{
		return false;
	}
	auto* Mesh = NewObject<UNiagaraMeshRendererProperties>(
		Emitter, NAME_None, RF_Transactional);
	Mesh->OverrideMaterials.Reset();
	Mesh->bOverrideMaterials = false;
	Emitter->AddRenderer(Mesh, Version);
	auto* Sprite = NewObject<UNiagaraSpriteRendererProperties>(
		Emitter, NAME_None, RF_Transactional);
	Sprite->Material = nullptr;
	Sprite->MaterialUserParamBinding.Parameter = FNiagaraVariable(
		FNiagaraTypeDefinition::GetUMaterialDef(),
		TEXT("User.UEAITestMaterial"));
	Emitter->AddRenderer(Sprite, Version);
	Package->SetDirtyFlag(false);
	const FNiagaraUserParameterBinding SpriteBinding =
		Sprite->MaterialUserParamBinding;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRendererMaterialTools(Registry);
	Registry.EndDomainRegistration();

	// Plan must allow the prospective mesh slot without mutating it.
	const FString MeshRequest = TEXT("mesh-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto MeshParams = MakeRendererMaterialParams(
		System, Handle, Mesh, MaterialFixture->GetPathName(), true);
	const FMCPToolResult MeshPlan = PlanAndApproveRendererMaterial(
		Registry, MeshParams, MeshRequest);
	if (!TestTrue(TEXT("Empty mesh slot plans"), MeshPlan.bSuccess)
		|| !MeshPlan.Data)
	{
		return false;
	}
	TestTrue(TEXT("Plan identifies prospective slot"),
		MeshPlan.Data->GetBoolField(TEXT("beforeSlotMissing")));
	TestTrue(TEXT("Plan predicts authored state change"),
		MeshPlan.Data->GetBoolField(TEXT("changesState")));
	TestTrue(TEXT("Plan predicts async compile request"),
		MeshPlan.Data->GetBoolField(TEXT("compileWillBeRequested")));
	TestEqual(TEXT("Planning does not allocate mesh slot"),
		Mesh->OverrideMaterials.Num(), 0);
	TestFalse(TEXT("Planning preserves clean package"), Package->IsDirty());

	// A stale approval must fail before reserving the request or writing.
	Mesh->bSortOnlyWhenTranslucent = !Mesh->bSortOnlyWhenTranslucent;
	const FMCPToolResult Stale = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), MeshParams);
	TestFalse(TEXT("Stale renderer plan is rejected"), Stale.bSuccess);
	TestEqual(TEXT("Stale plan has stable code"),
		Stale.ErrorCode, FString(TEXT("plan_digest_mismatch")));
	TestEqual(TEXT("Stale rejection leaves slot absent"),
		Mesh->OverrideMaterials.Num(), 0);
	Mesh->bSortOnlyWhenTranslucent = !Mesh->bSortOnlyWhenTranslucent;
	const FMCPToolResult FreshPlan = PlanAndApproveRendererMaterial(
		Registry, MeshParams, MeshRequest);
	if (!TestTrue(TEXT("Mesh plan refresh succeeds"), FreshPlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult MeshApplied = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), MeshParams);
	if (!TestTrue(TEXT("Empty mesh slot apply succeeds"), MeshApplied.bSuccess)
		|| !MeshApplied.Data)
	{
		return false;
	}
	TestEqual(TEXT("Apply creates exactly one mesh slot"),
		Mesh->OverrideMaterials.Num(), 1);
	TestEqual(TEXT("Created slot stores explicit material"),
		Mesh->OverrideMaterials[0].ExplicitMat.Get(), MaterialFixture);
	TestTrue(TEXT("Apply enables mesh overrides"), Mesh->bOverrideMaterials != 0);
	TestTrue(TEXT("Receipt records async compile request"),
		MeshApplied.Data->GetBoolField(TEXT("compileRequested")));
	TestFalse(TEXT("Receipt does not claim compile completion"),
		MeshApplied.Data->GetBoolField(TEXT("compiled")));
	TestTrue(TEXT("Material write dirties package"), Package->IsDirty());
	System->WaitForCompilationComplete(false, false);
	TestFalse(TEXT("Requested mesh compilation reaches a terminal state"),
		System->HasOutstandingCompilationRequests(false));

	const FMCPToolResult MeshReplay = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), MeshParams);
	if (TestTrue(TEXT("Identical mesh request replays"), MeshReplay.bSuccess)
		&& MeshReplay.Data)
	{
		TestTrue(TEXT("Replay is explicitly labelled"),
			MeshReplay.Data->GetBoolField(TEXT("idempotentReplay")));
		TestEqual(TEXT("Replay does not allocate another slot"),
			Mesh->OverrideMaterials.Num(), 1);
	}
	auto ConflictParams = MakeRendererMaterialParams(
		System, Handle, Mesh, FString(), true);
	ConflictParams->SetStringField(TEXT("requestId"), MeshRequest);
	ConflictParams->SetStringField(TEXT("approvePlanDigest"),
		MeshApplied.Data->GetStringField(TEXT("planDigest")));
	ConflictParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult Conflict = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), ConflictParams);
	TestFalse(TEXT("Request ID cannot change material arguments"), Conflict.bSuccess);
	TestEqual(TEXT("Request conflict has stable code"),
		Conflict.ErrorCode, FString(TEXT("request_id_conflict")));

	const ENiagaraSortMode OriginalSort = Mesh->SortMode;
	Mesh->SortMode = ENiagaraSortMode::ViewDepth;
	const FMCPToolResult DriftReplay = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), MeshParams);
	TestFalse(TEXT("Replay rejects authored state drift"), DriftReplay.bSuccess);
	TestEqual(TEXT("Replay drift has stable code"),
		DriftReplay.ErrorCode, FString(TEXT("receipt_state_changed")));
	Mesh->SortMode = OriginalSort;

	const FMCPToolResult MeshRolledBack = RollbackRendererMaterial(
		Registry, MeshApplied, MeshRequest);
	if (TestTrue(TEXT("Mesh rollback succeeds"), MeshRolledBack.bSuccess)
		&& MeshRolledBack.Data)
	{
		TestTrue(TEXT("Rollback restores semantic digest"),
			MeshRolledBack.Data->GetBoolField(TEXT("semanticRollbackVerified")));
		TestFalse(TEXT("Manual rollback does not claim full dirty restoration"),
			MeshRolledBack.Data->GetBoolField(TEXT("dirtyRestored")));
		TestTrue(TEXT("Manual rollback conservatively preserves package dirt"),
			MeshRolledBack.Data->GetBoolField(TEXT("dirtyPreserved")));
		TestFalse(TEXT("Manual rollback does not claim full package restoration"),
			MeshRolledBack.Data->GetBoolField(TEXT("fullPackageStateRestored")));
		TestEqual(TEXT("Manual rollback reports its conservative outcome"),
			MeshRolledBack.Data->GetStringField(TEXT("outcome")),
			FString(TEXT("semantic_restored_dirty_preserved")));
		TestTrue(TEXT("Receipt reports rolled back"),
			MeshRolledBack.Data->GetBoolField(TEXT("rolledBack")));
	}
	TestEqual(TEXT("Rollback removes prospective mesh slot"),
		Mesh->OverrideMaterials.Num(), 0);
	TestFalse(TEXT("Rollback restores disabled overrides"),
		Mesh->bOverrideMaterials != 0);
	TestTrue(TEXT("Manual rollback does not clear potentially external package dirt"),
		Package->IsDirty());
	const FMCPToolResult MeshReleased = ReleaseRendererMaterialReceipt(
		Registry, MeshApplied, MeshRequest);
	if (TestTrue(TEXT("Rolled-back mesh receipt can be released"),
		MeshReleased.bSuccess) && MeshReleased.Data)
	{
		TestTrue(TEXT("Receipt release is explicit"),
			MeshReleased.Data->GetBoolField(TEXT("released")));
		TestFalse(TEXT("Receipt release does not mutate the asset"),
			MeshReleased.Data->GetBoolField(TEXT("assetMutated")));
		TestEqual(TEXT("Receipt release exposes bounded active retention"),
			MeshReleased.Data->GetIntegerField(TEXT("activeReceiptLimit")), 256);
		TestTrue(TEXT("Receipt release exposes bounded terminal retention"),
			MeshReleased.Data->GetIntegerField(TEXT("terminalHistoryLimit")) > 0);
	}
	const FMCPToolResult MeshReleaseReplay = ReleaseRendererMaterialReceipt(
		Registry, MeshApplied, MeshRequest);
	if (TestTrue(TEXT("Receipt release is idempotent in recent history"),
		MeshReleaseReplay.bSuccess) && MeshReleaseReplay.Data)
	{
		TestTrue(TEXT("Release replay is labelled"),
			MeshReleaseReplay.Data->GetBoolField(TEXT("idempotentReplay")));
	}
	System->WaitForCompilationComplete(false, false);
	TestFalse(TEXT("Mesh rollback compilation reaches a terminal state"),
		System->HasOutstandingCompilationRequests(false));
	Package->SetDirtyFlag(false);

	// Enabling overrides without an explicit material must not invent a slot.
	const FString OverrideRequest = TEXT("override-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto OverrideParams = MakeRendererMaterialParams(
		System, Handle, Mesh, FString(), true);
	const FMCPToolResult OverridePlan = PlanAndApproveRendererMaterial(
		Registry, OverrideParams, OverrideRequest);
	if (!TestTrue(TEXT("Override-only plan succeeds"), OverridePlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult OverrideApplied = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), OverrideParams);
	if (!TestTrue(TEXT("Override-only apply succeeds"), OverrideApplied.bSuccess))
	{
		return false;
	}
	TestEqual(TEXT("Override-only apply leaves slot array empty"),
		Mesh->OverrideMaterials.Num(), 0);
	TestTrue(TEXT("Override-only apply still requests compile"),
		OverrideApplied.Data->GetBoolField(TEXT("compileRequested")));
	const FMCPToolResult ActiveReleaseRejected = ReleaseRendererMaterialReceipt(
		Registry, OverrideApplied, OverrideRequest);
	TestFalse(TEXT("Changed receipt cannot silently discard rollback ownership"),
		ActiveReleaseRejected.bSuccess);
	TestEqual(TEXT("Active receipt release requires explicit discard confirmation"),
		ActiveReleaseRejected.ErrorCode,
		FString(TEXT("rollback_discard_confirmation_required")));
	const FMCPToolResult OverrideRollback = RollbackRendererMaterial(
		Registry, OverrideApplied, OverrideRequest);
	TestTrue(TEXT("Override-only rollback succeeds"), OverrideRollback.bSuccess);
	TestEqual(TEXT("Override-only rollback keeps array empty"),
		Mesh->OverrideMaterials.Num(), 0);
	TestTrue(TEXT("Override-only manual rollback preserves package dirt"),
		Package->IsDirty());
	TestTrue(TEXT("Override-only rolled-back receipt releases"),
		ReleaseRendererMaterialReceipt(
			Registry, OverrideApplied, OverrideRequest).bSuccess);
	System->WaitForCompilationComplete(false, false);
	TestFalse(TEXT("Override rollback compilation reaches a terminal state"),
		System->HasOutstandingCompilationRequests(false));
	Package->SetDirtyFlag(false);

	// Sprite renderer uses the same plan/apply/readback/rollback discipline and
	// must preserve its authored user-parameter binding.
	const FString SpriteRequest = TEXT("sprite-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto SpriteParams = MakeRendererMaterialParams(
		System, Handle, Sprite, MaterialFixture->GetPathName());
	const FMCPToolResult SpritePlan = PlanAndApproveRendererMaterial(
		Registry, SpriteParams, SpriteRequest);
	if (!TestTrue(TEXT("Sprite material plan succeeds"), SpritePlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult SpriteApplied = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), SpriteParams);
	if (!TestTrue(TEXT("Sprite material apply succeeds"), SpriteApplied.bSuccess)
		|| !SpriteApplied.Data)
	{
		return false;
	}
	TestEqual(TEXT("Sprite explicit material read-back"),
		Sprite->Material.Get(), MaterialFixture);
	TestTrue(TEXT("Sprite mutation requests compile"),
		SpriteApplied.Data->GetBoolField(TEXT("compileRequested")));
	TestTrue(TEXT("Sprite binding is preserved after apply"),
		Sprite->MaterialUserParamBinding == SpriteBinding);
	const ENiagaraSortMode ExternalSortBefore = Mesh->SortMode;
	Mesh->SortMode = ExternalSortBefore == ENiagaraSortMode::ViewDepth
		? ENiagaraSortMode::ViewDistance
		: ENiagaraSortMode::ViewDepth;
	Package->MarkPackageDirty();
	const FMCPToolResult SpriteRollback = RollbackRendererMaterial(
		Registry, SpriteApplied, SpriteRequest);
	TestTrue(TEXT("Sprite rollback succeeds"), SpriteRollback.bSuccess);
	TestNull(TEXT("Sprite material is restored"), Sprite->Material.Get());
	TestTrue(TEXT("Sprite binding is preserved after rollback"),
		Sprite->MaterialUserParamBinding == SpriteBinding);
	TestTrue(TEXT("Rollback preserves another renderer's unsaved edit"),
		Package->IsDirty());
	TestTrue(TEXT("Sprite rolled-back receipt releases"),
		ReleaseRendererMaterialReceipt(
			Registry, SpriteApplied, SpriteRequest).bSuccess);
	Mesh->SortMode = ExternalSortBefore;
	Package->SetDirtyFlag(false);
	System->WaitForCompilationComplete(false, false);
	TestFalse(TEXT("Sprite rollback compilation reaches a terminal state"),
		System->HasOutstandingCompilationRequests(false));

	// A no-op receipt must likewise never clear later user-owned package dirt.
	const FString NoOpRequest = TEXT("noop-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto NoOpParams = MakeRendererMaterialParams(
		System, Handle, Sprite, FString());
	const FMCPToolResult NoOpPlan = PlanAndApproveRendererMaterial(
		Registry, NoOpParams, NoOpRequest);
	if (!TestTrue(TEXT("Sprite no-op plan succeeds"), NoOpPlan.bSuccess))
	{
		return false;
	}
	const FMCPToolResult NoOpApplied = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.material.apply"), NoOpParams);
	if (!TestTrue(TEXT("Sprite no-op apply succeeds"), NoOpApplied.bSuccess)
		|| !NoOpApplied.Data)
	{
		return false;
	}
	TestFalse(TEXT("No-op receipt reports no authored change"),
		NoOpApplied.Data->GetBoolField(TEXT("changed")));
	TestFalse(TEXT("No-op receipt does not request compile"),
		NoOpApplied.Data->GetBoolField(TEXT("compileRequested")));
	Mesh->SortMode = ExternalSortBefore == ENiagaraSortMode::ViewDepth
		? ENiagaraSortMode::ViewDistance
		: ENiagaraSortMode::ViewDepth;
	Package->MarkPackageDirty();
	const FMCPToolResult NoOpRollback = RollbackRendererMaterial(
		Registry, NoOpApplied, NoOpRequest);
	TestTrue(TEXT("No-op rollback succeeds"), NoOpRollback.bSuccess);
	TestTrue(TEXT("No-op rollback preserves later user package dirt"),
		Package->IsDirty());
	TestTrue(TEXT("No-op rolled-back receipt releases"),
		ReleaseRendererMaterialReceipt(
			Registry, NoOpApplied, NoOpRequest).bSuccess);
	Mesh->SortMode = ExternalSortBefore;
	Package->SetDirtyFlag(false);
	return true;
}
#endif

#endif
