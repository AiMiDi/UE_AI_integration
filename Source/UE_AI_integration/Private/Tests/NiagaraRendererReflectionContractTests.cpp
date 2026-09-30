#if WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "Dom/JsonObject.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraComponentRendererProperties.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UEAIIntegrationTools
{
void RegisterNiagaraRendererMaterialTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FRendererReflectionFixture
{
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	UNiagaraEmitter* Emitter = nullptr;
	FGuid EmitterId;
	UNiagaraSpriteRendererProperties* Sprite = nullptr;
	UNiagaraMeshRendererProperties* Mesh = nullptr;
	UNiagaraComponentRendererProperties* Component = nullptr;

	bool Create(FAutomationTestBase& Test)
	{
		const FString PackageName = TEXT("/Game/Automation/UEAI_RendererReflection_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		Package = CreatePackage(*PackageName);
		System = NewObject<UNiagaraSystem>(
			Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Test.TestNotNull(TEXT("Owned Niagara renderer reflection fixture"), System))
		{
			return false;
		}
		UNiagaraSystemFactoryNew::InitializeSystem(System, true);
		FAssetRegistryModule::AssetCreated(System);

		UNiagaraEmitter* Template = LoadObject<UNiagaraEmitter>(
			nullptr,
			TEXT("/Niagara/DefaultAssets/Templates/Emitters/SingleLoopingParticle.SingleLoopingParticle"));
		if (!Test.TestNotNull(TEXT("Stock emitter template"), Template))
		{
			return false;
		}
		FNiagaraEditorUtilities::AddEmitterToSystem(
			*System,
			*Template,
			Template->GetExposedVersion().VersionGuid);
		if (!Test.TestEqual(TEXT("Exactly one emitter is added"), System->GetEmitterHandles().Num(), 1))
		{
			return false;
		}

		const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[0];
		EmitterId = Handle.GetId();
		Emitter = Handle.GetInstance().Emitter;
		const FGuid Version = Handle.GetInstance().Version;
		if (!Test.TestNotNull(TEXT("Owned emitter instance"), Emitter))
		{
			return false;
		}

		Sprite = NewObject<UNiagaraSpriteRendererProperties>(Emitter, NAME_None, RF_Transactional);
		Mesh = NewObject<UNiagaraMeshRendererProperties>(Emitter, NAME_None, RF_Transactional);
		Component = NewObject<UNiagaraComponentRendererProperties>(Emitter, NAME_None, RF_Transactional);
		if (!Test.TestNotNull(TEXT("Sprite renderer fixture"), Sprite)
			|| !Test.TestNotNull(TEXT("Mesh renderer fixture"), Mesh)
			|| !Test.TestNotNull(TEXT("Component renderer fixture"), Component))
		{
			return false;
		}
		Emitter->AddRenderer(Sprite, Version);
		Emitter->AddRenderer(Mesh, Version);
		Emitter->AddRenderer(Component, Version);
		System->WaitForCompilationComplete(false, false);
		Package->SetDirtyFlag(false);
		return true;
	}

	void Cleanup(FAutomationTestBase& Test)
	{
		if (System)
		{
			System->WaitForCompilationComplete(false, false);
		}
		const FString PackageName = Package ? Package->GetName() : FString();
		const bool bDeleted = PackageName.IsEmpty()
			|| !UEditorAssetLibrary::DoesAssetExist(PackageName)
			|| UEditorAssetLibrary::DeleteAsset(PackageName);
		Test.TestTrue(
			TEXT("Renderer reflection fixture is deleted"),
			bDeleted && (PackageName.IsEmpty() || !FPackageName::DoesPackageExist(PackageName)));
	}
};

TSharedRef<FJsonObject> PropertyParams(
	const FRendererReflectionFixture& Fixture,
	const UObject* Renderer,
	const TCHAR* Property)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	Params->SetStringField(TEXT("emitter"), Fixture.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Params->SetStringField(TEXT("rendererPath"), Renderer->GetPathName());
	Params->SetStringField(TEXT("property"), Property);
	return Params;
}

void AssertUnsupported(
	FAutomationTestBase& Test,
	FMCPToolRegistry& Registry,
	const FRendererReflectionFixture& Fixture,
	const UObject* Renderer,
	const TCHAR* Property,
	const TFunctionRef<void(TSharedRef<FJsonObject>&)>& SetValue)
{
	TSharedRef<FJsonObject> Params = PropertyParams(Fixture, Renderer, Property);
	SetValue(Params);
	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.property.set"), Params);
	Test.TestFalse(
		FString::Printf(TEXT("Unknown editable property '%s' is rejected"), Property),
		Result.bSuccess);
	Test.TestEqual(
		FString::Printf(TEXT("Unknown editable property '%s' uses renderer_property_unsupported"), Property),
		Result.ErrorCode,
		FString(TEXT("renderer_property_unsupported")));
	Test.TestFalse(
		FString::Printf(TEXT("Rejected property '%s' leaves the package clean"), Property),
		Fixture.Package->IsDirty());
	Test.TestFalse(
		FString::Printf(TEXT("Rejected property '%s' does not leave a compile request"), Property),
		Fixture.System->HasOutstandingCompilationRequests(false));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraRendererReflectionContractTest,
	"UE_AI_integration.Niagara.RendererReflection.BoundedPropertyContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraRendererReflectionContractTest::RunTest(const FString&)
{
	FRendererReflectionFixture Fixture;
	if (!Fixture.Create(*this))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Fixture.Cleanup(*this);
	};

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRendererMaterialTools(Registry);
	Registry.EndDomainRegistration();

	// These names are deliberately valid CPF_Edit fields on stock renderer
	// classes, but are outside the public enumerated property contract. They
	// must never become an arbitrary UProperty write surface.
	Fixture.Sprite->bSortOnlyWhenTranslucent = true;
	Fixture.Sprite->SortMode = ENiagaraSortMode::None;
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Sprite,
		TEXT("bSortOnlyWhenTranslucent"),
		[](TSharedRef<FJsonObject>& Params) { Params->SetBoolField(TEXT("value"), false); });
	TestTrue(
		TEXT("EditCondition-gated unknown property remains unchanged"),
		Fixture.Sprite->bSortOnlyWhenTranslucent);

	// CutoutTexture is CPF_Edit but hidden while bUseMaterialCutoutTexture is
	// false. A generic reflection path currently edits it without consulting
	// CanEditChange; the contract requires the same rejection boundary.
	Fixture.Sprite->bUseMaterialCutoutTexture = false;
	Fixture.Sprite->CutoutTexture = nullptr;
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Sprite,
		TEXT("CutoutTexture"),
		[](TSharedRef<FJsonObject>& Params)
		{
			Params->SetStringField(
				TEXT("value"),
				TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
		});
	TestNull(TEXT("EditCondition-gated CutoutTexture remains unset"), Fixture.Sprite->CutoutTexture);

	// Structs and nested containers are intentionally outside the bounded
	// renderer property surface. The value is syntactically plausible so a
	// reflected ImportText implementation cannot turn this into a write.
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Sprite,
		TEXT("MaterialUserParamBinding"),
		[](TSharedRef<FJsonObject>& Params) { Params->SetStringField(TEXT("value"), TEXT("()")); });
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Sprite,
		TEXT("MaterialParameters"),
		[](TSharedRef<FJsonObject>& Params) { Params->SetStringField(TEXT("value"), TEXT("()")); });

	// Object and class references are also not part of the enumerated aliases.
	// In particular, accepting these would allow arbitrary asset/class loading
	// through the renderer command even when the editor would hide the field.
	Fixture.Sprite->Material = nullptr;
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Sprite,
		TEXT("Material"),
		[](TSharedRef<FJsonObject>& Params)
		{
			Params->SetStringField(
				TEXT("value"),
				TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
		});
	TestNull(TEXT("Rejected material reference remains unset"), Fixture.Sprite->Material);

	Fixture.Component->ComponentType = nullptr;
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Component,
		TEXT("ComponentType"),
		[](TSharedRef<FJsonObject>& Params)
		{
			Params->SetStringField(TEXT("value"), TEXT("/Script/Engine.SceneComponent"));
		});
	TestTrue(TEXT("Rejected component class remains unset"), Fixture.Component->ComponentType == nullptr);

	// Arrays remain rejected even when empty. This guards the fixed/nested
	// container boundary independently of the scalar cases above.
	Fixture.Mesh->OverrideMaterials.Reset();
	Fixture.Package->SetDirtyFlag(false);
	AssertUnsupported(
		*this,
		Registry,
		Fixture,
		Fixture.Mesh,
		TEXT("OverrideMaterials"),
		[](TSharedRef<FJsonObject>& Params)
		{
			Params->SetArrayField(TEXT("value"), TArray<TSharedPtr<FJsonValue>>());
		});
	TestEqual(TEXT("Rejected renderer array remains empty"), Fixture.Mesh->OverrideMaterials.Num(), 0);

	// The parser must reject trailing text rather than partially accepting a
	// reflected struct value. This is a positive error-boundary check and must
	// leave the fixture clean even before the unknown-property fix lands.
	Fixture.Package->SetDirtyFlag(false);
	TSharedRef<FJsonObject> Trailing = PropertyParams(
		Fixture, Fixture.Sprite, TEXT("MaterialUserParamBinding"));
	Trailing->SetStringField(TEXT("value"), TEXT("() trailing"));
	const FMCPToolResult TrailingResult = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.property.set"), Trailing);
	TestFalse(TEXT("ImportText trailing text is rejected"), TrailingResult.bSuccess);
	TestTrue(
		TEXT("ImportText trailing text uses a value or bounded-property error"),
		TrailingResult.ErrorCode == TEXT("property_value_invalid")
		|| TrailingResult.ErrorCode == TEXT("renderer_property_unsupported"));
	TestFalse(TEXT("Trailing text rejection leaves the package clean"), Fixture.Package->IsDirty());

	// Numeric overflow must be rejected before a renderer mutation. The value
	// is deliberately beyond uint32 and beyond the exact JSON integer range.
	Fixture.Component->ComponentCountLimit = 1;
	Fixture.Package->SetDirtyFlag(false);
	TSharedRef<FJsonObject> Overflow = PropertyParams(
		Fixture, Fixture.Component, TEXT("ComponentCountLimit"));
	Overflow->SetNumberField(TEXT("value"), 9007199254740992.0);
	const FMCPToolResult OverflowResult = Registry.ExecuteTool(
		TEXT("content.niagara.renderer.property.set"), Overflow);
	TestFalse(TEXT("Out-of-range integer is rejected"), OverflowResult.bSuccess);
	TestEqual(
		TEXT("Out-of-range integer uses property_value_invalid"),
		OverflowResult.ErrorCode,
		FString(TEXT("property_value_invalid")));
	TestEqual(TEXT("Integer overflow leaves the value unchanged"), Fixture.Component->ComponentCountLimit, 1u);
	TestFalse(TEXT("Integer overflow leaves the package clean"), Fixture.Package->IsDirty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
