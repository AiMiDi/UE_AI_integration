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
#include "NiagaraRibbonRendererProperties.h"
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
	UNiagaraRibbonRendererProperties* Ribbon = nullptr;
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
		Ribbon = NewObject<UNiagaraRibbonRendererProperties>(Emitter, NAME_None, RF_Transactional);
		Component = NewObject<UNiagaraComponentRendererProperties>(Emitter, NAME_None, RF_Transactional);
		if (!Test.TestNotNull(TEXT("Sprite renderer fixture"), Sprite)
			|| !Test.TestNotNull(TEXT("Mesh renderer fixture"), Mesh)
			|| !Test.TestNotNull(TEXT("Ribbon renderer fixture"), Ribbon)
			|| !Test.TestNotNull(TEXT("Component renderer fixture"), Component))
		{
			return false;
		}
		Emitter->AddRenderer(Sprite, Version);
		Emitter->AddRenderer(Mesh, Version);
		Emitter->AddRenderer(Ribbon, Version);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraRendererMeshRibbonSubUVContractTest,
	"UE_AI_integration.Niagara.RendererReflection.MeshRibbonSubUVContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraRendererMeshRibbonSubUVContractTest::RunTest(const FString&)
{
	FRendererReflectionFixture Fixture;
	ON_SCOPE_EXIT
	{
		Fixture.Cleanup(*this);
	};
	if (!Fixture.Create(*this))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRendererMaterialTools(Registry);
	Registry.EndDomainRegistration();
	TestNotNull(TEXT("Renderer property command is registered"),
		Registry.FindTool(TEXT("content.niagara.renderer.property.set")));

	const auto SetProperty = [&](const UObject* Renderer,
		const TCHAR* Property,
		const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedRef<FJsonObject> Params = PropertyParams(Fixture, Renderer, Property);
		Params->SetField(TEXT("value"), Value);
		return Registry.ExecuteTool(TEXT("content.niagara.renderer.property.set"), Params);
	};
	const auto AssertAuthoredReceipt = [&](const TCHAR* Label, const FMCPToolResult& Result)
	{
		if (!TestTrue(FString::Printf(TEXT("%s succeeds"), Label), Result.bSuccess)
			|| !TestNotNull(FString::Printf(TEXT("%s has a receipt"), Label), Result.Data.Get()))
		{
			AddError(Result.ErrorMessage);
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s uses the property receipt schema"), Label),
			Result.Data->GetStringField(TEXT("schema")),
			FString(TEXT("ue.niagara.renderer-property.v1")));
		TestFalse(FString::Printf(TEXT("%s never claims persistence"), Label),
			Result.Data->GetBoolField(TEXT("saved")));
		TestTrue(FString::Printf(TEXT("%s declares its runtime verification boundary"), Label),
			Result.Data->GetStringField(TEXT("scope")).Contains(TEXT("unverified")));
		return true;
	};
	const auto SubImageValue = [](double X, double Y)
	{
		TSharedRef<FJsonObject> Size = MakeShared<FJsonObject>();
		Size->SetNumberField(TEXT("x"), X);
		Size->SetNumberField(TEXT("y"), Y);
		return MakeShared<FJsonValueObject>(Size);
	};

	// SubUV aliases must dispatch to both sprite and mesh properties and publish
	// their authored readback. These receipts prove configuration, not rendering.
	const FMCPToolResult SpriteSubUV = SetProperty(
		Fixture.Sprite, TEXT("subImageSize"), SubImageValue(2.0, 4.0));
	const FMCPToolResult MeshSubUV = SetProperty(
		Fixture.Mesh, TEXT("subImageSize"), SubImageValue(4.0, 2.0));
	if (!AssertAuthoredReceipt(TEXT("Sprite SubUV size"), SpriteSubUV)
		|| !AssertAuthoredReceipt(TEXT("Mesh SubUV size"), MeshSubUV))
	{
		return false;
	}
	TestTrue(TEXT("Sprite SubUV reaches its native property"),
		Fixture.Sprite->SubImageSize.Equals(FVector2D(2.0, 4.0)));
	TestTrue(TEXT("Mesh SubUV reaches its native property"),
		Fixture.Mesh->SubImageSize.Equals(FVector2D(4.0, 2.0)));
	const TSharedPtr<FJsonObject> MeshSizeReadback = MeshSubUV.Data->GetObjectField(TEXT("readback"));
	if (TestNotNull(TEXT("Mesh SubUV readback is a vector object"), MeshSizeReadback.Get()))
	{
		double ReadbackX = 0.0;
		double ReadbackY = 0.0;
		const bool bReadbackX = MeshSizeReadback->TryGetNumberField(TEXT("x"), ReadbackX)
			|| MeshSizeReadback->TryGetNumberField(TEXT("X"), ReadbackX);
		const bool bReadbackY = MeshSizeReadback->TryGetNumberField(TEXT("y"), ReadbackY)
			|| MeshSizeReadback->TryGetNumberField(TEXT("Y"), ReadbackY);
		TestTrue(TEXT("Mesh SubUV readback exposes x"), bReadbackX);
		TestTrue(TEXT("Mesh SubUV readback exposes y"), bReadbackY);
		if (bReadbackX) TestEqual(TEXT("Mesh SubUV readback x matches"), ReadbackX, 4.0);
		if (bReadbackY) TestEqual(TEXT("Mesh SubUV readback y matches"), ReadbackY, 2.0);
	}
	const FMCPToolResult MeshBlend = SetProperty(
		Fixture.Mesh, TEXT("subImageBlend"), MakeShared<FJsonValueBoolean>(true));
	if (!AssertAuthoredReceipt(TEXT("Mesh SubUV blend"), MeshBlend)) return false;
	TestTrue(TEXT("Mesh SubUV blend reaches its native property"), Fixture.Mesh->bSubImageBlend != 0);
	TestTrue(TEXT("Mesh SubUV blend is read back"), MeshBlend.Data->GetBoolField(TEXT("readback")));

	const FMCPToolResult MeshSort = SetProperty(
		Fixture.Mesh, TEXT("sortMode"), MakeShared<FJsonValueString>(TEXT("ViewDepth")));
	if (!AssertAuthoredReceipt(TEXT("Mesh sorting"), MeshSort)) return false;
	TestTrue(TEXT("Mesh sort mode reaches its native enum"), Fixture.Mesh->SortMode == ENiagaraSortMode::ViewDepth);

	// Ribbon-only aliases must update the ribbon, including the dependent shape
	// and tessellation settings needed to make the numeric fields editable.
	const FMCPToolResult RibbonShape = SetProperty(
		Fixture.Ribbon, TEXT("shape"), MakeShared<FJsonValueString>(TEXT("Tube")));
	if (!AssertAuthoredReceipt(TEXT("Ribbon shape"), RibbonShape)) return false;
	TestTrue(TEXT("Ribbon shape reaches its native enum"), Fixture.Ribbon->Shape == ENiagaraRibbonShapeMode::Tube);
	const FMCPToolResult RibbonTube = SetProperty(
		Fixture.Ribbon, TEXT("tubeSubdivisions"), MakeShared<FJsonValueNumber>(6));
	if (!AssertAuthoredReceipt(TEXT("Ribbon tube subdivisions"), RibbonTube)) return false;
	TestEqual(TEXT("Ribbon tube subdivisions reach the native property"), Fixture.Ribbon->TubeSubdivisions, 6);
	TestEqual(TEXT("Ribbon tube subdivisions are read back"), RibbonTube.Data->GetIntegerField(TEXT("readback")), 6);
	const FMCPToolResult RibbonMode = SetProperty(
		Fixture.Ribbon, TEXT("tessellationMode"), MakeShared<FJsonValueString>(TEXT("Custom")));
	if (!AssertAuthoredReceipt(TEXT("Ribbon tessellation mode"), RibbonMode)) return false;
	TestTrue(TEXT("Ribbon tessellation mode reaches its native enum"),
		Fixture.Ribbon->TessellationMode == ENiagaraRibbonTessellationMode::Custom);
	const FMCPToolResult RibbonFactor = SetProperty(
		Fixture.Ribbon, TEXT("tessellationFactor"), MakeShared<FJsonValueNumber>(4));
	if (!AssertAuthoredReceipt(TEXT("Ribbon tessellation factor"), RibbonFactor)) return false;
	TestEqual(TEXT("Ribbon tessellation factor reaches the native property"), Fixture.Ribbon->TessellationFactor, 4);
	TestEqual(TEXT("Ribbon tessellation factor is read back"), RibbonFactor.Data->GetIntegerField(TEXT("readback")), 4);

	// A repeated value is an authored no-op and must not queue another compile.
	const FMCPToolResult MeshNoOp = SetProperty(
		Fixture.Mesh, TEXT("subImageSize"), SubImageValue(4.0, 2.0));
	if (!AssertAuthoredReceipt(TEXT("Repeated mesh SubUV size"), MeshNoOp)) return false;
	TestFalse(TEXT("Repeated mesh SubUV size is a no-op"), MeshNoOp.Data->GetBoolField(TEXT("changed")));
	TestFalse(TEXT("Repeated mesh SubUV size does not request compilation"),
		MeshNoOp.Data->GetBoolField(TEXT("compileRequested")));

	Fixture.System->WaitForCompilationComplete(false, false);
	Fixture.Package->SetDirtyFlag(false);
	const FMCPToolResult RibbonSubUV = SetProperty(
		Fixture.Ribbon, TEXT("subImageSize"), SubImageValue(2.0, 2.0));
	TestFalse(TEXT("SubUV rejects a ribbon renderer"), RibbonSubUV.bSuccess);
	TestEqual(TEXT("SubUV on ribbon has a stable dispatch rejection"), RibbonSubUV.ErrorCode,
		FString(TEXT("property_unsupported_for_renderer")));
	const FMCPToolResult MeshTube = SetProperty(
		Fixture.Mesh, TEXT("tubeSubdivisions"), MakeShared<FJsonValueNumber>(6));
	TestFalse(TEXT("Ribbon tube subdivisions reject a mesh renderer"), MeshTube.bSuccess);
	TestEqual(TEXT("Ribbon property on mesh has a stable dispatch rejection"), MeshTube.ErrorCode,
		FString(TEXT("property_unsupported_for_renderer")));
	const FMCPToolResult InvalidTube = SetProperty(
		Fixture.Ribbon, TEXT("tubeSubdivisions"), MakeShared<FJsonValueNumber>(2));
	TestFalse(TEXT("Ribbon tube subdivisions reject the value below their minimum"), InvalidTube.bSuccess);
	TestEqual(TEXT("Ribbon tube range rejection is explicit"), InvalidTube.ErrorCode,
		FString(TEXT("property_value_out_of_range")));
	TestEqual(TEXT("Rejected tube range preserves the authored value"), Fixture.Ribbon->TubeSubdivisions, 6);
	TestFalse(TEXT("Dispatch and range rejections leave the package clean"), Fixture.Package->IsDirty());
	TestFalse(TEXT("Dispatch and range rejections do not queue compilation"),
		Fixture.System->HasOutstandingCompilationRequests(false));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
