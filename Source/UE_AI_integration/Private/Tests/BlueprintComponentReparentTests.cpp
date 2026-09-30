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
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegrationTools
{
void RegisterComponentTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintReparentFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	USCS_Node* Root = nullptr;
	USCS_Node* Child = nullptr;
	USCS_Node* GrandChild = nullptr;
	USCS_Node* Light = nullptr;
};

FBlueprintReparentFixture CreateReparentFixture()
{
	FBlueprintReparentFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/UEAI_ComponentReparent_")
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
			FName(TEXT("UEAI.ComponentReparentContract")))
		: nullptr;
	USimpleConstructionScript* SCS = Fixture.Blueprint
		? Fixture.Blueprint->SimpleConstructionScript
		: nullptr;
	if (!SCS)
	{
		return Fixture;
	}

	// A valid single-scene-root actor hierarchy:
	//   Root (ZuluRoot, scene root)
	//     -> Child (AlphaChild)
	//        -> GrandChild (BetaGrandChild)
	//     -> Light (MiddleLight)
	Fixture.Root = SCS->CreateNode(
		USceneComponent::StaticClass(), FName(TEXT("ZuluRoot")));
	Fixture.Child = SCS->CreateNode(
		USceneComponent::StaticClass(), FName(TEXT("AlphaChild")));
	Fixture.GrandChild = SCS->CreateNode(
		USceneComponent::StaticClass(), FName(TEXT("BetaGrandChild")));
	Fixture.Light = SCS->CreateNode(
		UPointLightComponent::StaticClass(), FName(TEXT("MiddleLight")));
	if (!Fixture.Root || !Fixture.Child || !Fixture.GrandChild || !Fixture.Light)
	{
		return Fixture;
	}
	SCS->AddNode(Fixture.Root);
	Fixture.Root->AddChildNode(Fixture.Child);
	Fixture.Child->AddChildNode(Fixture.GrandChild);
	Fixture.Root->AddChildNode(Fixture.Light);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
		Fixture.Blueprint);
	return Fixture;
}

bool SaveReparentFixture(UBlueprint* Blueprint, FString& OutError)
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
		OutError = TEXT("Blueprint reparent fixture did not compile.");
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

bool DeleteReparentFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

TSharedRef<FJsonObject> MakeReparentParams(
	const FString& Blueprint,
	const USCS_Node* Node,
	const USCS_Node* NewParent)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	if (Node)
	{
		Params->SetStringField(
			TEXT("componentNodeId"), Node->VariableGuid.ToString());
	}
	if (NewParent)
	{
		Params->SetStringField(
			TEXT("newParentNodeId"), NewParent->VariableGuid.ToString());
	}
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintComponentReparentContractTest,
	"UE_AI_integration.Blueprint.Component.ReparentContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintComponentReparentContractTest::RunTest(const FString&)
{
	const FBlueprintReparentFixture Fixture = CreateReparentFixture();
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Reparent fixture and package are deleted"),
			DeleteReparentFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Reparent Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Reparent fixture package"), Fixture.Package)
		|| !TestNotNull(TEXT("Reparent fixture root"), Fixture.Root)
		|| !TestNotNull(TEXT("Reparent fixture child"), Fixture.Child)
		|| !TestNotNull(TEXT("Reparent fixture grandchild"), Fixture.GrandChild)
		|| !TestNotNull(TEXT("Reparent fixture light"), Fixture.Light))
	{
		return false;
	}
	FString SaveError;
	if (!TestTrue(TEXT("Reparent baseline compiles and saves"),
		SaveReparentFixture(Fixture.Blueprint, SaveError)))
	{
		AddError(SaveError);
		return false;
	}
	const FGuid RootId = Fixture.Root->VariableGuid;
	const FGuid ChildId = Fixture.Child->VariableGuid;
	const FGuid GrandChildId = Fixture.GrandChild->VariableGuid;
	const FGuid LightId = Fixture.Light->VariableGuid;
	USimpleConstructionScript* SCS = Fixture.Blueprint->SimpleConstructionScript;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterComponentTools(Registry);
	Registry.EndDomainRegistration();

	// Self-parenting is rejected before any SCS mutation.
	const bool bDirtyBeforeSelf = Fixture.Package->IsDirty();
	const FMCPToolResult Self = Registry.ExecuteTool(
		TEXT("blueprint.component.reparent"),
		MakeReparentParams(Fixture.PackageName, Fixture.Child, Fixture.Child));
	TestFalse(TEXT("Self-parenting is rejected"), Self.bSuccess);
	TestEqual(TEXT("Self-parenting uses a stable code"),
		Self.ErrorCode, FString(TEXT("invalid_params")));
	TestEqual(TEXT("Self-parenting performs zero writes"),
		Fixture.Package->IsDirty(), bDirtyBeforeSelf);

	// Moving a node under one of its own descendants is a cycle and is rejected.
	const FMCPToolResult Cycle = Registry.ExecuteTool(
		TEXT("blueprint.component.reparent"),
		MakeReparentParams(Fixture.PackageName, Fixture.Root, Fixture.GrandChild));
	TestFalse(TEXT("Descendant cycle is rejected"), Cycle.bSuccess);
	TestEqual(TEXT("Descendant cycle uses a stable code"),
		Cycle.ErrorCode, FString(TEXT("invalid_params")));
	TestTrue(TEXT("Cycle rejection leaves the root in place"),
		SCS->GetRootNodes().Contains(Fixture.Root));

	// GrandChild moves from Child up to Root (local child -> local child).
	const FMCPToolResult MoveToRoot = Registry.ExecuteTool(
		TEXT("blueprint.component.reparent"),
		MakeReparentParams(Fixture.PackageName, Fixture.GrandChild, Fixture.Root));
	if (!TestTrue(TEXT("Child-to-child reparent succeeds"), MoveToRoot.bSuccess)
		|| !MoveToRoot.Data)
	{
		return false;
	}
	TestTrue(TEXT("Reparent reports a real change"),
		MoveToRoot.Data->GetBoolField(TEXT("changed")));
	TestEqual(TEXT("GrandChild parent is now Root"),
		SCS->FindParentNode(Fixture.GrandChild), Fixture.Root);

	// Light moves from Root down under GrandChild (local child -> local child).
	const FMCPToolResult LightUnderGrandChild = Registry.ExecuteTool(
		TEXT("blueprint.component.reparent"),
		MakeReparentParams(
			Fixture.PackageName, Fixture.Light, Fixture.GrandChild));
	if (!TestTrue(TEXT("Second child-to-child reparent succeeds"),
		LightUnderGrandChild.bSuccess)
		|| !LightUnderGrandChild.Data)
	{
		return false;
	}
	TestEqual(TEXT("Light parent is now GrandChild"),
		SCS->FindParentNode(Fixture.Light), Fixture.GrandChild);

	// Child promotes to the root. An Actor Blueprint has a single scene root, so
	// promoting a scene component makes it the scene root and nests the previous
	// scene root (ZuluRoot) underneath it — this survives compilation, which
	// otherwise re-nests an extra scene root back under the scene root.
	const FMCPToolResult PromoteChild = Registry.ExecuteTool(
		TEXT("blueprint.component.reparent"),
		MakeReparentParams(Fixture.PackageName, Fixture.Child, nullptr));
	if (!TestTrue(TEXT("Child-to-root reparent succeeds"),
		PromoteChild.bSuccess)
		|| !PromoteChild.Data)
	{
		return false;
	}
	TestTrue(TEXT("Promoted child reports itself as the root"),
		PromoteChild.Data->GetBoolField(TEXT("isRoot")));
	TestTrue(TEXT("Child is promoted to the SCS root"),
		SCS->GetRootNodes().Contains(Fixture.Child));
	TestTrue(TEXT("Promoted child has no local parent"),
		SCS->FindParentNode(Fixture.Child) == nullptr);
	TestEqual(TEXT("Previous scene root is nested under the promoted child"),
		SCS->FindParentNode(Fixture.Root), Fixture.Child);

	// Reparenting an already-root component to the root is an idempotent no-op.
	const FMCPToolResult NoOp = Registry.ExecuteTool(
		TEXT("blueprint.component.reparent"),
		MakeReparentParams(Fixture.PackageName, Fixture.Child, nullptr));
	if (!TestTrue(TEXT("Root-to-root reparent is idempotent"), NoOp.bSuccess)
		|| !NoOp.Data)
	{
		return false;
	}
	TestFalse(TEXT("Root-to-root reparent reports no change"),
		NoOp.Data->GetBoolField(TEXT("changed")));
	TestTrue(TEXT("No-op keeps the child as the root"),
		NoOp.Data->GetBoolField(TEXT("isRoot")));

	// Persistence: the final hierarchy survives a disk reload.
	UPackage* PackageToReload = Fixture.Blueprint->GetOutermost();
	TArray<UPackage*> PackagesToReload{PackageToReload};
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
	TestTrue(TEXT("Reparented Blueprint reloads from disk"), bReloaded);
	if (bReloaded)
	{
		const FString ObjectPath = Fixture.PackageName + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(Fixture.PackageName);
		UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (TestNotNull(TEXT("Reparented Blueprint reopens"), Reloaded)
			&& Reloaded->SimpleConstructionScript)
		{
			USimpleConstructionScript* ReloadedSCS =
				Reloaded->SimpleConstructionScript;
			USCS_Node* ReloadedRoot =
				ReloadedSCS->FindSCSNodeByGuid(RootId);
			USCS_Node* ReloadedChild =
				ReloadedSCS->FindSCSNodeByGuid(ChildId);
			USCS_Node* ReloadedGrandChild =
				ReloadedSCS->FindSCSNodeByGuid(GrandChildId);
			USCS_Node* ReloadedLight =
				ReloadedSCS->FindSCSNodeByGuid(LightId);
			if (TestNotNull(TEXT("Reloaded Root"), ReloadedRoot)
				&& TestNotNull(TEXT("Reloaded Child"), ReloadedChild)
				&& TestNotNull(TEXT("Reloaded GrandChild"), ReloadedGrandChild)
				&& TestNotNull(TEXT("Reloaded Light"), ReloadedLight))
			{
				TestTrue(TEXT("Promoted Child stays a root after reload"),
					ReloadedSCS->GetRootNodes().Contains(ReloadedChild));
				TestEqual(TEXT("Previous scene root stays nested after reload"),
					ReloadedSCS->FindParentNode(ReloadedRoot), ReloadedChild);
				TestEqual(TEXT("GrandChild parent survives reload"),
					ReloadedSCS->FindParentNode(ReloadedGrandChild), ReloadedRoot);
				TestEqual(TEXT("Light parent survives reload"),
					ReloadedSCS->FindParentNode(ReloadedLight), ReloadedGrandChild);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
