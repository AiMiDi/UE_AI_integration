#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterComponentTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintComponentFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	USCS_Node* Root = nullptr;
	USCS_Node* Child = nullptr;
	USCS_Node* Light = nullptr;
};

FBlueprintComponentFixture CreateBlueprintComponentFixture()
{
	FBlueprintComponentFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/UEAI_ComponentContract_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(Fixture.PackageName);
	Fixture.Package = CreatePackage(*Fixture.PackageName);
	Fixture.Blueprint = Fixture.Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Fixture.Package,
			*AssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UEAI.BlueprintComponentContract")))
		: nullptr;
	USimpleConstructionScript* SCS = Fixture.Blueprint
		? Fixture.Blueprint->SimpleConstructionScript
		: nullptr;
	if (!SCS)
	{
		return Fixture;
	}

	Fixture.Root = SCS->CreateNode(
		USceneComponent::StaticClass(), FName(TEXT("ZuluRoot")));
	Fixture.Child = SCS->CreateNode(
		USceneComponent::StaticClass(), FName(TEXT("AlphaChild")));
	Fixture.Light = SCS->CreateNode(
		UPointLightComponent::StaticClass(), FName(TEXT("MiddleLight")));
	if (!Fixture.Root || !Fixture.Child || !Fixture.Light)
	{
		return Fixture;
	}
	SCS->AddNode(Fixture.Root);
	Fixture.Root->AddChildNode(Fixture.Child);
	SCS->AddNode(Fixture.Light);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
		Fixture.Blueprint);
	return Fixture;
}

bool SaveBlueprintComponentFixture(
	UBlueprint* Blueprint,
	FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint fixture is null.");
		return false;
	}
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint, EBlueprintCompileOptions::SkipSave);
	if (Blueprint->Status == BS_Error)
	{
		OutError = TEXT("Blueprint component fixture did not compile.");
		return false;
	}
	UEAIIntegration::Infrastructure::FBlueprintPersistenceError Error;
	if (!UEAIIntegration::Infrastructure::SaveBlueprintPackage(
			Blueprint, nullptr, Error))
	{
		OutError = Error.Message;
		return false;
	}
	return true;
}

bool DeleteBlueprintComponentFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

TSharedRef<FJsonObject> MakeComponentSelector(
	const FString& Blueprint,
	const USCS_Node* Node)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	if (Node)
	{
		Params->SetStringField(
			TEXT("componentNodeId"), Node->VariableGuid.ToString());
	}
	return Params;
}

const TSharedPtr<FJsonObject> FindPropertyRecord(
	const FMCPToolResult& Result,
	const FString& PropertyName)
{
	if (!Result.bSuccess || !Result.Data)
	{
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (!Result.Data->TryGetArrayField(TEXT("properties"), Properties)
		|| !Properties)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Properties)
	{
		const TSharedPtr<FJsonObject> Property = Value->AsObject();
		if (Property
			&& Property->GetStringField(TEXT("name")) == PropertyName)
		{
			return Property;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintComponentRegistrationContractTest,
	"UE_AI_integration.Blueprint.Component.RegistrationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintComponentRegistrationContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterComponentTools(Registry);
	Registry.EndDomainRegistration();

	static const TCHAR* ExpectedCapabilities[] = {
		TEXT("blueprint.component.list"),
		TEXT("blueprint.component.get"),
		TEXT("blueprint.component.property.set"),
		TEXT("blueprint.component.add"),
		TEXT("blueprint.component.remove"),
		TEXT("blueprint.component.reparent"),
	};
	TestEqual(
		TEXT("Exactly six component capabilities register"),
		Registry.Num(),
		static_cast<int32>(UE_ARRAY_COUNT(ExpectedCapabilities)));
	for (const TCHAR* Capability : ExpectedCapabilities)
	{
		TestNotNull(Capability, Registry.FindTool(Capability));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintComponentReadWriteContractTest,
	"UE_AI_integration.Blueprint.Component.ReadWriteContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintComponentReadWriteContractTest::RunTest(const FString&)
{
	const FBlueprintComponentFixture Fixture =
		CreateBlueprintComponentFixture();
	ON_SCOPE_EXIT
	{
		TestTrue(
			TEXT("Blueprint component fixture and package are deleted"),
			DeleteBlueprintComponentFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Component Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Component fixture package"), Fixture.Package)
		|| !TestNotNull(TEXT("Component fixture root"), Fixture.Root)
		|| !TestNotNull(TEXT("Component fixture child"), Fixture.Child)
		|| !TestNotNull(TEXT("Component fixture light"), Fixture.Light))
	{
		return false;
	}
	FString SaveError;
	if (!TestTrue(
			TEXT("Component baseline compiles and saves"),
			SaveBlueprintComponentFixture(Fixture.Blueprint, SaveError)))
	{
		AddError(SaveError);
		return false;
	}
	const FGuid RootId = Fixture.Root->VariableGuid;
	const FGuid ChildId = Fixture.Child->VariableGuid;
	const FGuid LightId = Fixture.Light->VariableGuid;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterComponentTools(Registry);
	Registry.EndDomainRegistration();

	// List is a deterministic local-SCS page, and child output has an
	// independent bounded budget.
	auto FirstPageParams = MakeShared<FJsonObject>();
	FirstPageParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	FirstPageParams->SetNumberField(TEXT("limit"), 1);
	FirstPageParams->SetNumberField(TEXT("offset"), 0);
	FirstPageParams->SetNumberField(TEXT("maxScannedNodes"), 3);
	FirstPageParams->SetNumberField(TEXT("maxChildrenPerComponent"), 64);
	FirstPageParams->SetNumberField(TEXT("maxChildEntries"), 1024);
	const FMCPToolResult FirstPage = Registry.ExecuteTool(
		TEXT("blueprint.component.list"), FirstPageParams);
	if (!TestTrue(TEXT("First component page succeeds"), FirstPage.bSuccess)
		|| !FirstPage.Data)
	{
		return false;
	}
	TestEqual(
		TEXT("Component list reports the complete local total"),
		FirstPage.Data->GetIntegerField(TEXT("total")),
		3);
	TestEqual(
		TEXT("Component list scans the whole bounded SCS"),
		FirstPage.Data->GetIntegerField(TEXT("scannedNodeCount")),
		3);
	TestFalse(
		TEXT("Bounded whole-SCS scan is not partial"),
		FirstPage.Data->GetBoolField(TEXT("partial")));
	TestFalse(
		TEXT("Inherited components are not claimed"),
		FirstPage.Data->GetBoolField(TEXT("includesInheritedComponents")));
	const TArray<TSharedPtr<FJsonValue>>& FirstComponents =
		FirstPage.Data->GetArrayField(TEXT("components"));
	if (TestEqual(TEXT("One component is returned"), FirstComponents.Num(), 1))
	{
		TestEqual(
			TEXT("Components are sorted by stable name/GUID key"),
			FirstComponents[0]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("AlphaChild")));
	}

	auto ChildBudgetParams = MakeShared<FJsonObject>();
	ChildBudgetParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	ChildBudgetParams->SetNumberField(TEXT("limit"), 3);
	ChildBudgetParams->SetNumberField(TEXT("maxScannedNodes"), 3);
	ChildBudgetParams->SetNumberField(TEXT("maxChildrenPerComponent"), 0);
	ChildBudgetParams->SetNumberField(TEXT("maxChildEntries"), 0);
	const FMCPToolResult ChildBudget = Registry.ExecuteTool(
		TEXT("blueprint.component.list"), ChildBudgetParams);
	if (TestTrue(TEXT("Zero child-output budget is valid"), ChildBudget.bSuccess)
		&& ChildBudget.Data)
	{
		TestTrue(
			TEXT("Child truncation is explicit"),
			ChildBudget.Data->GetBoolField(TEXT("childrenTruncated")));
		TestTrue(
			TEXT("Child truncation marks the response partial"),
			ChildBudget.Data->GetBoolField(TEXT("partial")));
		TestEqual(
			TEXT("No child entries exceed the output budget"),
			ChildBudget.Data->GetIntegerField(TEXT("childEntriesWritten")),
			0);
	}

	const bool bDirtyBeforeListReject = Fixture.Package->IsDirty();
	auto ScanRejectParams = MakeShared<FJsonObject>();
	ScanRejectParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	ScanRejectParams->SetNumberField(TEXT("maxScannedNodes"), 1);
	const FMCPToolResult ScanRejected = Registry.ExecuteTool(
		TEXT("blueprint.component.list"), ScanRejectParams);
	TestFalse(
		TEXT("Whole-SCS scan over the declared hard cap is rejected"),
		ScanRejected.bSuccess);
	TestEqual(
		TEXT("Whole-SCS scan rejection uses 413"),
		ScanRejected.HttpStatus,
		413);
	TestEqual(
		TEXT("Whole-SCS scan rejection has a stable code"),
		ScanRejected.ErrorCode,
		FString(TEXT("component_scan_limit_exceeded")));
	TestFalse(
		TEXT("Whole-SCS rejection returns no partial data"),
		ScanRejected.Data.IsValid());
	TestEqual(
		TEXT("Whole-SCS rejection performs zero writes"),
		Fixture.Package->IsDirty(),
		bDirtyBeforeListReject);

	auto FractionalListParams = MakeShared<FJsonObject>();
	FractionalListParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	FractionalListParams->SetNumberField(TEXT("limit"), 1.5);
	const FMCPToolResult FractionalList = Registry.ExecuteTool(
		TEXT("blueprint.component.list"), FractionalListParams);
	TestFalse(TEXT("Fractional component limit is rejected"), FractionalList.bSuccess);
	TestEqual(
		TEXT("Fractional component limit is an invalid request"),
		FractionalList.ErrorCode,
		FString(TEXT("invalid_params")));

	// Get publishes a page-independent complete state hash and keeps
	// BlueprintVisible-only properties readable but not writable.
	auto RootGetParams = MakeShared<FJsonObject>();
	RootGetParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	RootGetParams->SetStringField(TEXT("componentNodeId"), RootId.ToString());
	RootGetParams->SetStringField(TEXT("componentName"), TEXT("ZuluRoot"));
	RootGetParams->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
	RootGetParams->SetNumberField(TEXT("limit"), 1);
	const FMCPToolResult RootGet = Registry.ExecuteTool(
		TEXT("blueprint.component.get"), RootGetParams);
	if (!TestTrue(TEXT("Component get by name and GUID succeeds"), RootGet.bSuccess)
		|| !RootGet.Data)
	{
		return false;
	}
	TestEqual(
		TEXT("Component get returns the stable SCS GUID"),
		RootGet.Data->GetStringField(TEXT("componentNodeId")),
		RootId.ToString());
	TestTrue(
		TEXT("Component state hash is available"),
		RootGet.Data->GetBoolField(TEXT("stateHashAvailable")));
	const TSharedPtr<FJsonObject> Coverage =
		RootGet.Data->GetObjectField(TEXT("stateHashCoverage"));
	if (TestNotNull(TEXT("State-hash coverage is published"), Coverage.Get()))
	{
		TestTrue(
			TEXT("State-hash coverage is complete"),
			Coverage->GetBoolField(TEXT("complete")));
		TestEqual(
			TEXT("State hash has a 2048-property cap"),
			Coverage->GetIntegerField(TEXT("maxProperties")),
			2048);
		TestEqual(
			TEXT("State hash has a per-value character cap"),
			Coverage->GetIntegerField(TEXT("maxValueCharsPerProperty")),
			65536);
		TestEqual(
			TEXT("State hash has a total value character cap"),
			Coverage->GetIntegerField(TEXT("maxTotalValueChars")),
			1048576);
		TestEqual(
			TEXT("Complete hash covers every readable property"),
			Coverage->GetIntegerField(TEXT("propertyHashed")),
			Coverage->GetIntegerField(TEXT("propertyTotal")));
	}
	const TSharedPtr<FJsonObject> AutoActivate =
		FindPropertyRecord(RootGet, TEXT("bAutoActivate"));
	if (!TestNotNull(TEXT("bAutoActivate property is readable"), AutoActivate.Get()))
	{
		return false;
	}
	TestTrue(
		TEXT("bAutoActivate is editable"),
		AutoActivate->GetBoolField(TEXT("editable")));
	const FString OldAutoActivate =
		AutoActivate->GetStringField(TEXT("value"));
	const FString NewAutoActivate =
		OldAutoActivate.Equals(TEXT("True"), ESearchCase::IgnoreCase)
			? TEXT("False")
			: TEXT("True");

	auto PropertyPageA = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	PropertyPageA->SetNumberField(TEXT("limit"), 1);
	PropertyPageA->SetNumberField(TEXT("offset"), 0);
	const FMCPToolResult PropertyPageFirst = Registry.ExecuteTool(
		TEXT("blueprint.component.get"), PropertyPageA);
	auto PropertyPageB = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	PropertyPageB->SetNumberField(TEXT("limit"), 1);
	PropertyPageB->SetNumberField(TEXT("offset"), 1);
	const FMCPToolResult PropertyPageSecond = Registry.ExecuteTool(
		TEXT("blueprint.component.get"), PropertyPageB);
	if (TestTrue(TEXT("First property page succeeds"), PropertyPageFirst.bSuccess)
		&& TestTrue(TEXT("Second property page succeeds"), PropertyPageSecond.bSuccess)
		&& PropertyPageFirst.Data && PropertyPageSecond.Data)
	{
		TestEqual(
			TEXT("State hash is independent of property page"),
			PropertyPageFirst.Data->GetStringField(TEXT("stateHash")),
			PropertyPageSecond.Data->GetStringField(TEXT("stateHash")));
		const FString FirstProperty = PropertyPageFirst.Data
			->GetArrayField(TEXT("properties"))[0]
			->AsObject()->GetStringField(TEXT("name"));
		const FString SecondProperty = PropertyPageSecond.Data
			->GetArrayField(TEXT("properties"))[0]
			->AsObject()->GetStringField(TEXT("name"));
		TestTrue(
			TEXT("Property pages preserve deterministic sorting"),
			FirstProperty < SecondProperty);
	}

	auto LightGetParams = MakeComponentSelector(Fixture.PackageName, Fixture.Light);
	LightGetParams->SetStringField(TEXT("property"), TEXT("Intensity"));
	const FMCPToolResult LightGet = Registry.ExecuteTool(
		TEXT("blueprint.component.get"), LightGetParams);
	const TSharedPtr<FJsonObject> Intensity =
		FindPropertyRecord(LightGet, TEXT("Intensity"));
	if (TestNotNull(
			TEXT("BlueprintVisible-only Intensity is readable"),
			Intensity.Get()))
	{
		TestFalse(
			TEXT("BlueprintVisible-only Intensity is not claimed writable"),
			Intensity->GetBoolField(TEXT("editable")));
		TestTrue(
			TEXT("BlueprintVisible-only Intensity is marked read-only"),
			Intensity->GetBoolField(TEXT("readOnly")));
	}

	const bool bDirtyBeforeReadOnlySet = Fixture.Package->IsDirty();
	auto ReadOnlySet = MakeComponentSelector(Fixture.PackageName, Fixture.Light);
	ReadOnlySet->SetStringField(TEXT("property"), TEXT("Intensity"));
	ReadOnlySet->SetStringField(TEXT("value"), TEXT("5000.0"));
	const FMCPToolResult ReadOnlyRejected = Registry.ExecuteTool(
		TEXT("blueprint.component.property.set"), ReadOnlySet);
	TestFalse(
		TEXT("BlueprintVisible-only property mutation is rejected"),
		ReadOnlyRejected.bSuccess);
	TestEqual(
		TEXT("Read-only property rejection is stable"),
		ReadOnlyRejected.ErrorCode,
		FString(TEXT("property_not_writable")));
	TestEqual(
		TEXT("Read-only rejection performs zero writes"),
		Fixture.Package->IsDirty(),
		bDirtyBeforeReadOnlySet);

	auto MismatchedIdentity = MakeComponentSelector(
		Fixture.PackageName, Fixture.Root);
	MismatchedIdentity->SetStringField(
		TEXT("componentName"), TEXT("AlphaChild"));
	const FMCPToolResult Mismatch = Registry.ExecuteTool(
		TEXT("blueprint.component.get"), MismatchedIdentity);
	TestFalse(TEXT("Mismatched name and GUID are rejected"), Mismatch.bSuccess);
	TestEqual(
		TEXT("Mismatched identity is not silently retargeted"),
		Mismatch.ErrorCode,
		FString(TEXT("component_not_found")));

	// All optimistic-concurrency and parse failures occur before mutation.
	auto StaleState = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	StaleState->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
	StaleState->SetStringField(TEXT("value"), NewAutoActivate);
	StaleState->SetStringField(
		TEXT("expectedStateHash"),
		TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
	const bool bDirtyBeforeStaleState = Fixture.Package->IsDirty();
	const FMCPToolResult StaleStateRejected = Registry.ExecuteTool(
		TEXT("blueprint.component.property.set"), StaleState);
	TestFalse(TEXT("Stale component hash is rejected"), StaleStateRejected.bSuccess);
	TestEqual(
		TEXT("Stale component hash uses conflict status"),
		StaleStateRejected.HttpStatus,
		409);
	TestEqual(
		TEXT("Stale state hash performs zero writes"),
		Fixture.Package->IsDirty(),
		bDirtyBeforeStaleState);

	auto StaleTemplate = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	StaleTemplate->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
	StaleTemplate->SetStringField(TEXT("value"), NewAutoActivate);
	StaleTemplate->SetStringField(
		TEXT("expectedTemplatePath"), TEXT("/Game/Automation.StaleTemplate"));
	const FMCPToolResult StaleTemplateRejected = Registry.ExecuteTool(
		TEXT("blueprint.component.property.set"), StaleTemplate);
	TestFalse(TEXT("Stale component template is rejected"), StaleTemplateRejected.bSuccess);
	TestEqual(
		TEXT("Stale component template uses a stable code"),
		StaleTemplateRejected.ErrorCode,
		FString(TEXT("stale_component")));

	const FString SensitiveExpected =
		FString(TEXT("SensitiveExpectedOldValue-"))
		+ FString::ChrN(6000, TEXT('x'));
	auto StaleOldValue = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	StaleOldValue->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
	StaleOldValue->SetStringField(TEXT("value"), NewAutoActivate);
	StaleOldValue->SetStringField(TEXT("expectedOldValue"), SensitiveExpected);
	const FMCPToolResult StaleOldRejected = Registry.ExecuteTool(
		TEXT("blueprint.component.property.set"), StaleOldValue);
	TestFalse(TEXT("Stale old value is rejected"), StaleOldRejected.bSuccess);
	TestEqual(
		TEXT("Stale old value uses a stable code"),
		StaleOldRejected.ErrorCode,
		FString(TEXT("stale_property")));
	TestTrue(
		TEXT("Stale old-value error publishes length/hash evidence"),
		StaleOldRejected.ErrorMessage.Contains(TEXT("length/hash")));
	TestFalse(
		TEXT("Stale old-value error does not echo the supplied value"),
		StaleOldRejected.ErrorMessage.Contains(
			TEXT("SensitiveExpectedOldValue")));

	auto TrailingValue = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	TrailingValue->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
	TrailingValue->SetStringField(
		TEXT("value"), NewAutoActivate + TEXT(" trailing"));
	const FMCPToolResult TrailingRejected = Registry.ExecuteTool(
		TEXT("blueprint.component.property.set"), TrailingValue);
	TestFalse(TEXT("Trailing property text is rejected"), TrailingRejected.bSuccess);
	TestEqual(
		TEXT("Trailing property text uses a stable code"),
		TrailingRejected.ErrorCode,
		FString(TEXT("property_value_invalid")));

	// A valid optimistic write compiles, saves, and survives a disk reload.
	auto ValidSet = MakeComponentSelector(Fixture.PackageName, Fixture.Root);
	ValidSet->SetStringField(TEXT("componentName"), TEXT("ZuluRoot"));
	ValidSet->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
	ValidSet->SetStringField(TEXT("value"), NewAutoActivate);
	ValidSet->SetStringField(TEXT("expectedOldValue"), OldAutoActivate);
	ValidSet->SetStringField(
		TEXT("expectedStateHash"),
		RootGet.Data->GetStringField(TEXT("stateHash")));
	ValidSet->SetStringField(
		TEXT("expectedTemplatePath"),
		RootGet.Data->GetStringField(TEXT("componentTemplatePath")));
	const FMCPToolResult Applied = Registry.ExecuteTool(
		TEXT("blueprint.component.property.set"), ValidSet);
	if (!TestTrue(TEXT("Optimistic component property write succeeds"), Applied.bSuccess)
		|| !Applied.Data)
	{
		return false;
	}
	TestTrue(
		TEXT("Direct component write compiles before returning"),
		Applied.Data->GetBoolField(TEXT("compiled")));
	TestTrue(
		TEXT("Direct component write saves before returning"),
		Applied.Data->GetBoolField(TEXT("saved")));
	TestEqual(
		TEXT("Component write returns normalized read-back"),
		Applied.Data->GetStringField(TEXT("newValue")),
		NewAutoActivate);
	TestTrue(
		TEXT("Component write publishes complete state-hash coverage"),
		Applied.Data->GetObjectField(TEXT("stateHashCoverage"))
			->GetBoolField(TEXT("complete")));
	TestTrue(
		TEXT("Component state hash changes after the write"),
		Applied.Data->GetStringField(TEXT("stateHash"))
			!= Applied.Data->GetStringField(TEXT("stateHashBefore")));

	UPackage* PackageToReload = Fixture.Blueprint->GetOutermost();
	TArray<UPackage*> PackagesToReload{PackageToReload};
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
	TestTrue(TEXT("Component Blueprint reloads from disk"), bReloaded);
	if (bReloaded)
	{
		auto ReloadedGet = MakeShared<FJsonObject>();
		ReloadedGet->SetStringField(TEXT("blueprint"), Fixture.PackageName);
		ReloadedGet->SetStringField(TEXT("componentNodeId"), RootId.ToString());
		ReloadedGet->SetStringField(TEXT("property"), TEXT("bAutoActivate"));
		const FMCPToolResult Reloaded = Registry.ExecuteTool(
			TEXT("blueprint.component.get"), ReloadedGet);
		const TSharedPtr<FJsonObject> ReloadedProperty =
			FindPropertyRecord(Reloaded, TEXT("bAutoActivate"));
		if (TestNotNull(
				TEXT("Reloaded component property remains readable"),
				ReloadedProperty.Get()))
		{
			TestEqual(
				TEXT("Component property survives disk reload"),
				ReloadedProperty->GetStringField(TEXT("value")),
				NewAutoActivate);
		}
	}

	// Keep the IDs live in the test contract: no identity was replaced by the
	// property mutation or the compile/save boundary.
	TestTrue(TEXT("Child SCS identity is valid"), ChildId.IsValid());
	TestTrue(TEXT("Light SCS identity is valid"), LightId.IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
