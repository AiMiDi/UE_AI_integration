#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraRendererProperties.h"
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
	FNiagaraRendererStructureRegistrationTest,
	"UE_AI_integration.Niagara.RendererStructure.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraRendererStructureRegistrationTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRendererMaterialTools(Registry);
	Registry.EndDomainRegistration();

	// The six pre-existing renderer material capabilities plus the five new
	// renderer structure/binding capabilities registered from this file.
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
	TestEqual(TEXT("Exactly eleven renderer capabilities register"),
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
TSharedRef<FJsonObject> MakeRendererStructureParams(
	UNiagaraSystem* System,
	const FGuid& EmitterId)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), System->GetPathName());
	Params->SetStringField(TEXT("emitter"), EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
	return Params;
}

TSharedRef<FJsonObject> MakeRendererTargetParams(
	UNiagaraSystem* System,
	const FGuid& EmitterId,
	const FString& RendererPath)
{
	auto Params = MakeRendererStructureParams(System, EmitterId);
	Params->SetStringField(TEXT("rendererPath"), RendererPath);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraRendererStructureContractTest,
	"UE_AI_integration.Niagara.RendererStructure.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraRendererStructureContractTest::RunTest(const FString&)
{
	// Writes require a non-transient /Game/ System and reads require a loadable
	// long package path, so this contract test builds an owned /Game/ fixture
	// (mirroring NiagaraRendererMaterialContractTests) rather than a transient
	// one. The emitter is initialized with UNiagaraEmitterFactoryNew exactly as
	// FUEAINiagaraOutputSelectionTest does.
	const FString PackageName = TEXT("/Game/Automation/UEAI_RendererStructure_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	ON_SCOPE_EXIT
	{
		if (System)
		{
			System->WaitForCompilationComplete(false, false);
		}
		const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
			|| UEditorAssetLibrary::DeleteAsset(PackageName);
		TestTrue(TEXT("Renderer structure fixture is deleted"),
			bDeleted
			&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
			&& !FPackageName::DoesPackageExist(PackageName));
	};
	if (!TestNotNull(TEXT("Owned Niagara System fixture"), System))
	{
		return false;
	}
	// Keep the renderer structure fixture self-contained; the optional Niagara
	// DefaultAssets module is not mounted in the isolated HostProject.
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

	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(System);
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FNiagaraEmitterHandle Handle(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	System->AddEmitterHandleDirect(Handle);
	if (!TestEqual(TEXT("One owned emitter is added"), System->GetEmitterHandles().Num(), 1))
	{
		return false;
	}
	const FNiagaraEmitterHandle& AddedHandle = System->GetEmitterHandles()[0];
	const FGuid EmitterId = AddedHandle.GetId();
	if (!TestEqual(TEXT("Fresh emitter has no renderers"),
		AddedHandle.GetEmitterData()->GetRenderers().Num(), 0))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRendererMaterialTools(Registry);
	Registry.EndDomainRegistration();

	// (a) renderer.add with an unsupported class is rejected without mutation.
	{
		auto Params = MakeRendererStructureParams(System, EmitterId);
		Params->SetStringField(TEXT("class"), TEXT("hypercube"));
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.renderer.add"), Params);
		TestFalse(TEXT("Unsupported renderer class is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unsupported class has a stable code"),
			Result.ErrorCode, FString(TEXT("renderer_class_unsupported")));
		TestTrue(TEXT("Unsupported class error lists the supported set"),
			Result.ErrorMessage.Contains(TEXT("sprite")));
		TestEqual(TEXT("Unsupported class does not mutate the renderer list"),
			AddedHandle.GetEmitterData()->GetRenderers().Num(), 0);
	}

	// (b) adding a supported renderer increases the count and is read back.
	FString RendererPath;
	{
		auto Params = MakeRendererStructureParams(System, EmitterId);
		Params->SetStringField(TEXT("class"), TEXT("sprite"));
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.renderer.add"), Params);
		if (!TestTrue(TEXT("Sprite renderer add succeeds"), Result.bSuccess) || !Result.Data)
		{
			return false;
		}
		TestEqual(TEXT("Add increases the renderer count to one"),
			AddedHandle.GetEmitterData()->GetRenderers().Num(), 1);
		RendererPath = Result.Data->GetStringField(TEXT("rendererPath"));
		TestFalse(TEXT("Added renderer has an authored object path"), RendererPath.IsEmpty());
		TestTrue(TEXT("Added renderer reports sprite class key"),
			Result.Data->GetStringField(TEXT("rendererClassKey")) == TEXT("sprite"));
		TestFalse(TEXT("Add never claims a save"), Result.Data->GetBoolField(TEXT("saved")));
	}

	// bindings.get and renderer.list read back the added renderer without writing.
	{
		const FMCPToolResult Bindings = Registry.ExecuteTool(
			TEXT("content.niagara.renderer.bindings.get"),
			MakeRendererTargetParams(System, EmitterId, RendererPath));
		if (!TestTrue(TEXT("bindings.get reads the added sprite"), Bindings.bSuccess) || !Bindings.Data)
		{
			return false;
		}
		TestTrue(TEXT("bindings.get reports the sprite class"),
			Bindings.Data->GetStringField(TEXT("rendererClass")).Contains(TEXT("SpriteRendererProperties")));
		TestFalse(TEXT("bindings.get never claims a save"), Bindings.Data->GetBoolField(TEXT("saved")));
		TestFalse(TEXT("bindings.get never claims a compile"), Bindings.Data->GetBoolField(TEXT("compiled")));
		TestTrue(TEXT("bindings.get lists the renderer's attribute bindings"),
			Bindings.Data->GetArrayField(TEXT("bindings")).Num() > 0);
	}
	{
		const FMCPToolResult List = Registry.ExecuteTool(
			TEXT("content.niagara.renderer.list"), MakeRendererStructureParams(System, EmitterId));
		if (!TestTrue(TEXT("renderer.list sees the added sprite"), List.bSuccess) || !List.Data)
		{
			return false;
		}
		TestEqual(TEXT("renderer.list totals one renderer"),
			List.Data->GetIntegerField(TEXT("total")), 1);
	}

	// (c) property.set with an unsupported property is rejected with the
	// supported-set error.
	{
		auto Params = MakeRendererTargetParams(System, EmitterId, RendererPath);
		Params->SetStringField(TEXT("property"), TEXT("arbitraryUProperty"));
		Params->SetBoolField(TEXT("value"), true);
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.renderer.property.set"), Params);
		TestFalse(TEXT("Unsupported property is rejected"), Result.bSuccess);
		TestEqual(TEXT("Unsupported property has a stable code"),
			Result.ErrorCode, FString(TEXT("renderer_property_unsupported")));
		TestTrue(TEXT("Unsupported property error lists the supported set"),
			Result.ErrorMessage.Contains(TEXT("enabled")));
	}

	// Positive property.set and bindings.set exercise the write path and readback.
	{
		auto Params = MakeRendererTargetParams(System, EmitterId, RendererPath);
		Params->SetStringField(TEXT("property"), TEXT("enabled"));
		Params->SetBoolField(TEXT("value"), false);
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.renderer.property.set"), Params);
		if (!TestTrue(TEXT("enabled property.set succeeds"), Result.bSuccess) || !Result.Data)
		{
			return false;
		}
		TestTrue(TEXT("enabled property.set reports a change"), Result.Data->GetBoolField(TEXT("changed")));
		UNiagaraRendererProperties* Sprite = AddedHandle.GetEmitterData()->GetRenderers()[0];
		TestFalse(TEXT("enabled property.set disables the sprite"), Sprite->GetIsEnabled());
		TestFalse(TEXT("property.set never claims a save"), Result.Data->GetBoolField(TEXT("saved")));
	}
	{
		auto Params = MakeRendererTargetParams(System, EmitterId, RendererPath);
		TArray<TSharedPtr<FJsonValue>> Bindings;
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("attribute"), TEXT("PositionBinding"));
		Entry->SetStringField(TEXT("value"), TEXT("MyCustomPosition"));
		Bindings.Add(MakeShared<FJsonValueObject>(Entry));
		Params->SetArrayField(TEXT("bindings"), Bindings);
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.renderer.bindings.set"), Params);
		if (!TestTrue(TEXT("PositionBinding set succeeds"), Result.bSuccess) || !Result.Data)
		{
			return false;
		}
		TestTrue(TEXT("PositionBinding set reports a change"), Result.Data->GetBoolField(TEXT("changed")));
		TestFalse(TEXT("bindings.set never claims a save"), Result.Data->GetBoolField(TEXT("saved")));

		const FMCPToolResult ReadBack = Registry.ExecuteTool(
			TEXT("content.niagara.renderer.bindings.get"),
			MakeRendererTargetParams(System, EmitterId, RendererPath));
		if (TestTrue(TEXT("bindings.get reads back the applied binding"), ReadBack.bSuccess) && ReadBack.Data)
		{
			bool bFoundBound = false;
			for (const TSharedPtr<FJsonValue>& RowValue : ReadBack.Data->GetArrayField(TEXT("bindings")))
			{
				const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
				if (Row.IsValid() && Row->GetStringField(TEXT("name")) == TEXT("PositionBinding")
					&& Row->GetStringField(TEXT("boundTo")).Contains(TEXT("MyCustomPosition")))
				{
					bFoundBound = true;
					break;
				}
			}
			TestTrue(TEXT("bindings.get reports the PositionBinding target"), bFoundBound);
		}
	}

	// (d) renderer.remove on a missing path is rejected with 404.
	{
		auto Params = MakeRendererTargetParams(System, EmitterId, RendererPath + TEXT(".MissingRenderer"));
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.renderer.remove"), Params);
		TestFalse(TEXT("Remove on a missing path is rejected"), Result.bSuccess);
		TestEqual(TEXT("Missing renderer has a 404 code"),
			Result.ErrorCode, FString(TEXT("renderer_not_found")));
		TestEqual(TEXT("Missing-path remove keeps the real renderer"),
			AddedHandle.GetEmitterData()->GetRenderers().Num(), 1);
	}

	System->WaitForCompilationComplete(false, false);
	TestFalse(TEXT("Requested compiles reach a terminal state"),
		System->HasOutstandingCompilationRequests(false));
	return true;
}
#endif

#endif
