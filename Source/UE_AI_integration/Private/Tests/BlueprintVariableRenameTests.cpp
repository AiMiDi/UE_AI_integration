#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
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
void RegisterVariableTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintVariableRenameFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	FGuid GetterId;
	FGuid SetterId;
};

FBlueprintVariableRenameFixture CreateVariableRenameFixture()
{
	FBlueprintVariableRenameFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/UEAI_VariableRename_")
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
			FName(TEXT("UEAI.VariableRenameContract")))
		: nullptr;
	Fixture.Graph = Fixture.Blueprint
		&& !Fixture.Blueprint->UbergraphPages.IsEmpty()
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	if (!Fixture.Blueprint || !Fixture.Graph)
	{
		return Fixture;
	}

	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	const FName VariableName(TEXT("StableValue"));
	if (!FBlueprintEditorUtils::AddMemberVariable(
			Fixture.Blueprint, VariableName, PinType))
	{
		return Fixture;
	}
	// A second member is created up-front so the duplicate-name rejection path
	// has a real collision to exercise without recompiling mid-test.
	const FName CollisionName(TEXT("CollisionValue"));
	FBlueprintEditorUtils::AddMemberVariable(
		Fixture.Blueprint, CollisionName, PinType);

	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*Fixture.Graph);
	UK2Node_VariableGet* Getter = GetCreator.CreateNode();
	Getter->VariableReference.SetSelfMember(VariableName);
	Getter->NodePosX = 100;
	Getter->NodePosY = 200;
	GetCreator.Finalize();

	FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Fixture.Graph);
	UK2Node_VariableSet* Setter = SetCreator.CreateNode();
	Setter->VariableReference.SetSelfMember(VariableName);
	Setter->NodePosX = 400;
	Setter->NodePosY = 200;
	SetCreator.Finalize();

	Fixture.GetterId = Getter->NodeGuid;
	Fixture.SetterId = Setter->NodeGuid;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
		Fixture.Blueprint);
	return Fixture;
}

bool SaveVariableRenameFixture(UBlueprint* Blueprint, FString& OutError)
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
		OutError = TEXT("Blueprint variable-rename fixture did not compile.");
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

bool DeleteVariableRenameFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

UEdGraphNode* FindRenameContractNode(UBlueprint* Blueprint, const FGuid& Guid)
{
	TArray<UEdGraph*> Graphs;
	if (Blueprint)
	{
		Blueprint->GetAllGraphs(Graphs);
	}
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
	}
	return nullptr;
}

bool HasMemberVariable(UBlueprint* Blueprint, const FString& Name)
{
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName.ToString().Equals(Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

TSharedRef<FJsonObject> MakeVariableRenameParams(
	const FString& Blueprint,
	const FString& VariableName,
	const FString& NewName)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	Params->SetStringField(TEXT("variableName"), VariableName);
	Params->SetStringField(TEXT("newName"), NewName);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintVariableRenameContractTest,
	"UE_AI_integration.Blueprint.Variable.RenameContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintVariableRenameContractTest::RunTest(const FString&)
{
	const FBlueprintVariableRenameFixture Fixture =
		CreateVariableRenameFixture();
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Variable-rename fixture and package are deleted"),
			DeleteVariableRenameFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Variable-rename Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Variable-rename graph fixture"), Fixture.Graph))
	{
		return false;
	}
	FString SaveError;
	if (!TestTrue(TEXT("Variable-rename baseline compiles and saves"),
		SaveVariableRenameFixture(Fixture.Blueprint, SaveError)))
	{
		AddError(SaveError);
		return false;
	}
	const FGuid GetterId = Fixture.GetterId;
	const FGuid SetterId = Fixture.SetterId;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterVariableTools(Registry);
	Registry.EndDomainRegistration();

	// Missing source variable is rejected before any mutation.
	const bool bDirtyBeforeMissing = Fixture.Package->IsDirty();
	const FMCPToolResult Missing = Registry.ExecuteTool(
		TEXT("blueprint.variable.rename"),
		MakeVariableRenameParams(
			Fixture.PackageName, TEXT("DoesNotExist"), TEXT("Whatever")));
	TestFalse(TEXT("Unknown source variable is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Missing source variable has a stable code"),
		Missing.ErrorCode, FString(TEXT("execution_failed")));
	TestEqual(TEXT("Missing source variable performs zero writes"),
		Fixture.Package->IsDirty(), bDirtyBeforeMissing);

	// Invalid identifier is rejected.
	const FMCPToolResult Invalid = Registry.ExecuteTool(
		TEXT("blueprint.variable.rename"),
		MakeVariableRenameParams(
			Fixture.PackageName, TEXT("StableValue"), TEXT("Bad Name!")));
	TestFalse(TEXT("Invalid variable name is rejected"), Invalid.bSuccess);
	TestTrue(TEXT("Invalid name error explains the identifier rule"),
		Invalid.ErrorMessage.Contains(TEXT("letter or underscore")));

	// Duplicate name is rejected.
	const FMCPToolResult Duplicate = Registry.ExecuteTool(
		TEXT("blueprint.variable.rename"),
		MakeVariableRenameParams(
			Fixture.PackageName, TEXT("StableValue"), TEXT("CollisionValue")));
	TestFalse(TEXT("Duplicate variable name is rejected"), Duplicate.bSuccess);
	TestTrue(TEXT("Duplicate name error names the collision"),
		Duplicate.ErrorMessage.Contains(TEXT("already exists")));
	TestTrue(TEXT("Duplicate rename preserves the source variable"),
		HasMemberVariable(Fixture.Blueprint, TEXT("StableValue")));

	// Successful rename rewrites the member and every referencing graph node.
	const FMCPToolResult Renamed = Registry.ExecuteTool(
		TEXT("blueprint.variable.rename"),
		MakeVariableRenameParams(
			Fixture.PackageName, TEXT("StableValue"), TEXT("RenamedValue")));
	if (!TestTrue(TEXT("Variable rename succeeds"), Renamed.bSuccess)
		|| !Renamed.Data)
	{
		return false;
	}
	TestTrue(TEXT("Rename compiles and saves before returning"),
		Renamed.Data->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("Rename drops the old member name"),
		!HasMemberVariable(Fixture.Blueprint, TEXT("StableValue")));
	TestTrue(TEXT("Rename publishes the new member name"),
		HasMemberVariable(Fixture.Blueprint, TEXT("RenamedValue")));

	UK2Node_VariableGet* RenamedGetter = Cast<UK2Node_VariableGet>(
		FindRenameContractNode(Fixture.Blueprint, GetterId));
	UK2Node_VariableSet* RenamedSetter = Cast<UK2Node_VariableSet>(
		FindRenameContractNode(Fixture.Blueprint, SetterId));
	if (TestNotNull(TEXT("Renamed getter still resolvable"), RenamedGetter)
		&& TestNotNull(TEXT("Renamed setter still resolvable"), RenamedSetter))
	{
		TestEqual(TEXT("Getter reference follows the rename"),
			RenamedGetter->GetVarName().ToString(), FString(TEXT("RenamedValue")));
		TestEqual(TEXT("Setter reference follows the rename"),
			RenamedSetter->GetVarName().ToString(), FString(TEXT("RenamedValue")));
	}

	// Persistence: the rename and its graph fix-ups survive a disk reload.
	UPackage* PackageToReload = Fixture.Blueprint->GetOutermost();
	TArray<UPackage*> PackagesToReload{PackageToReload};
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
	TestTrue(TEXT("Renamed Blueprint reloads from disk"), bReloaded);
	if (bReloaded)
	{
		const FString ObjectPath = Fixture.PackageName + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(Fixture.PackageName);
		UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (TestNotNull(TEXT("Renamed Blueprint reopens"), Reloaded))
		{
			TestTrue(TEXT("Renamed member survives disk reload"),
				HasMemberVariable(Reloaded, TEXT("RenamedValue")));
			TestFalse(TEXT("Old member name does not survive disk reload"),
				HasMemberVariable(Reloaded, TEXT("StableValue")));
			UK2Node_VariableGet* ReloadedGetter = Cast<UK2Node_VariableGet>(
				FindRenameContractNode(Reloaded, GetterId));
			if (TestNotNull(TEXT("Renamed getter survives disk reload"),
				ReloadedGetter))
			{
				TestEqual(TEXT("Getter reference survives disk reload"),
					ReloadedGetter->GetVarName().ToString(),
					FString(TEXT("RenamedValue")));
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
