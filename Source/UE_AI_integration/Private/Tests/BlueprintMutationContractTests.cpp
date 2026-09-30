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
#include "UObject/UnrealType.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintMutationFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
};

FBlueprintMutationFixture CreateBlueprintMutationFixture(const FString& Prefix)
{
	FBlueprintMutationFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/") + Prefix + TEXT("_")
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
			FName(TEXT("UEAI.BlueprintMutationContract")))
		: nullptr;
	Fixture.Graph = Fixture.Blueprint
		&& !Fixture.Blueprint->UbergraphPages.IsEmpty()
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	return Fixture;
}

bool SaveBlueprintMutationFixture(UBlueprint* Blueprint, FString& OutError)
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
		OutError = TEXT("Blueprint fixture did not compile.");
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

bool DeleteBlueprintMutationAsset(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

UEdGraphNode* FindMutationContractNode(UBlueprint* Blueprint, const FGuid& Guid)
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

TSharedRef<FJsonObject> MakeBlueprintMutationParams(
	const FString& Blueprint,
	const FString& Variable,
	const FString& Value)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	Params->SetStringField(TEXT("variable"), Variable);
	Params->SetStringField(TEXT("value"), Value);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintNodeDuplicateContractTest,
	"UE_AI_integration.Blueprint.Mutation.NodeDuplicateContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintNodeDuplicateContractTest::RunTest(const FString&)
{
	const FBlueprintMutationFixture Fixture =
		CreateBlueprintMutationFixture(TEXT("UEAI_Duplicate"));
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Duplicate fixture and package are deleted"),
			DeleteBlueprintMutationAsset(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Duplicate Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Duplicate graph fixture"), Fixture.Graph))
	{
		return false;
	}

	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	const FName VariableName(TEXT("StableValue"));
	if (!TestTrue(TEXT("Integer member is created"),
		FBlueprintEditorUtils::AddMemberVariable(
			Fixture.Blueprint, VariableName, PinType)))
	{
		return false;
	}
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
	UEdGraphPin* GetterValue = Getter->FindPin(VariableName);
	UEdGraphPin* SetterValue = Setter->FindPin(VariableName);
	if (!TestNotNull(TEXT("Getter value pin"), GetterValue)
		|| !TestNotNull(TEXT("Setter value pin"), SetterValue)
		|| !TestTrue(TEXT("Fixture nodes are internally connected"),
			Fixture.Graph->GetSchema()->TryCreateConnection(
				GetterValue, SetterValue)))
	{
		return false;
	}
	const FGuid GetterId = Getter->NodeGuid;
	const FGuid SetterId = Setter->NodeGuid;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture.Blueprint);
	FString SaveError;
	if (!TestTrue(TEXT("Duplicate baseline saves"),
		SaveBlueprintMutationFixture(Fixture.Blueprint, SaveError)))
	{
		AddError(SaveError);
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	Params->SetArrayField(TEXT("nodeIds"), {
		MakeShared<FJsonValueString>(GetterId.ToString()),
		MakeShared<FJsonValueString>(SetterId.ToString())});
	Params->SetNumberField(TEXT("offsetX"), 128);
	Params->SetNumberField(TEXT("offsetY"), -64);
	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("blueprint.node.duplicate"), Params);
	if (!TestTrue(TEXT("Connected node duplication succeeds"), Result.bSuccess)
		|| !Result.Data)
	{
		return false;
	}
	TestEqual(TEXT("Two nodes are duplicated"),
		Result.Data->GetIntegerField(TEXT("duplicatedCount")), 2);
	TestTrue(TEXT("Duplicate compiles before returning"),
		Result.Data->GetBoolField(TEXT("compiled")));
	TestTrue(TEXT("Duplicate persists before returning"),
		Result.Data->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("Duplicate package exists on disk"),
		FPackageName::DoesPackageExist(Fixture.PackageName));

	TMap<FGuid, FGuid> Mapping;
	FString PreviousSource;
	for (const TSharedPtr<FJsonValue>& Value :
		Result.Data->GetArrayField(TEXT("nodeIdMappings")))
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		FGuid SourceId;
		FGuid DuplicateId;
		const FString SourceText = Row->GetStringField(TEXT("sourceNodeId"));
		TestTrue(TEXT("Mappings are sorted by source GUID"),
			PreviousSource.IsEmpty() || PreviousSource < SourceText);
		PreviousSource = SourceText;
		TestTrue(TEXT("Source mapping GUID parses"), FGuid::Parse(SourceText, SourceId));
		TestTrue(TEXT("Duplicate mapping GUID parses"), FGuid::Parse(
			Row->GetStringField(TEXT("duplicateNodeId")), DuplicateId));
		TestTrue(TEXT("Duplicate receives a fresh GUID"),
			DuplicateId.IsValid() && DuplicateId != SourceId);
		Mapping.Add(SourceId, DuplicateId);
	}
	UEdGraphNode* DuplicateGetter = FindMutationContractNode(
		Fixture.Blueprint, Mapping.FindRef(GetterId));
	UEdGraphNode* DuplicateSetter = FindMutationContractNode(
		Fixture.Blueprint, Mapping.FindRef(SetterId));
	if (TestNotNull(TEXT("Duplicated getter read-back"), DuplicateGetter)
		&& TestNotNull(TEXT("Duplicated setter read-back"), DuplicateSetter))
	{
		TestEqual(TEXT("Getter X offset is applied"),
			DuplicateGetter->NodePosX, 228);
		TestEqual(TEXT("Getter Y offset is applied"),
			DuplicateGetter->NodePosY, 136);
		UEdGraphPin* DuplicateGetterValue = DuplicateGetter->FindPin(VariableName);
		UEdGraphPin* DuplicateSetterValue = DuplicateSetter->FindPin(VariableName);
		TestTrue(TEXT("Internal duplicate connection is preserved"),
			DuplicateGetterValue && DuplicateSetterValue
			&& DuplicateGetterValue->LinkedTo.Contains(DuplicateSetterValue));
	}

	const int32 NodeCountBeforeReject = Fixture.Graph->Nodes.Num();
	const bool bDirtyBeforeReject = Fixture.Package->IsDirty();
	auto DuplicateIds = MakeShared<FJsonObject>();
	DuplicateIds->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	DuplicateIds->SetArrayField(TEXT("nodeIds"), {
		MakeShared<FJsonValueString>(GetterId.ToString()),
		MakeShared<FJsonValueString>(GetterId.ToString())});
	const FMCPToolResult Rejected = Registry.ExecuteTool(
		TEXT("blueprint.node.duplicate"), DuplicateIds);
	TestFalse(TEXT("Duplicate source IDs are rejected"), Rejected.bSuccess);
	TestEqual(TEXT("Duplicate ID rejection is classified"),
		Rejected.ErrorCode, FString(TEXT("invalid_params")));
	TestEqual(TEXT("Rejected duplicate performs zero graph writes"),
		Fixture.Graph->Nodes.Num(), NodeCountBeforeReject);
	TestEqual(TEXT("Rejected duplicate preserves dirty state"),
		Fixture.Package->IsDirty(), bDirtyBeforeReject);

	const FGuid PersistedGetterId = Mapping.FindRef(GetterId);
	const FGuid PersistedSetterId = Mapping.FindRef(SetterId);
	UPackage* DuplicatePackage = Fixture.Blueprint->GetOutermost();
	TArray<UPackage*> PackagesToReload{DuplicatePackage};
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
	TestTrue(TEXT("Duplicated Blueprint reloads from disk"), bReloaded);
	if (bReloaded)
	{
		const FString ObjectPath = Fixture.PackageName + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(Fixture.PackageName);
		UBlueprint* ReloadedBlueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		UEdGraphNode* ReloadedGetter = FindMutationContractNode(
			ReloadedBlueprint, PersistedGetterId);
		UEdGraphNode* ReloadedSetter = FindMutationContractNode(
			ReloadedBlueprint, PersistedSetterId);
		if (TestNotNull(TEXT("Persisted duplicate getter"), ReloadedGetter)
			&& TestNotNull(TEXT("Persisted duplicate setter"), ReloadedSetter))
		{
			UEdGraphPin* ReloadedGetterValue = ReloadedGetter->FindPin(VariableName);
			UEdGraphPin* ReloadedSetterValue = ReloadedSetter->FindPin(VariableName);
			TestTrue(TEXT("Internal connection survives disk reload"),
				ReloadedGetterValue && ReloadedSetterValue
				&& ReloadedGetterValue->LinkedTo.Contains(ReloadedSetterValue));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintDefaultSetContractTest,
	"UE_AI_integration.Blueprint.Mutation.DefaultSetContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDefaultSetContractTest::RunTest(const FString&)
{
	const FBlueprintMutationFixture Fixture =
		CreateBlueprintMutationFixture(TEXT("UEAI_Default"));
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Default fixture and package are deleted"),
			DeleteBlueprintMutationAsset(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Default Blueprint fixture"), Fixture.Blueprint))
	{
		return false;
	}
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	const FName VariableName(TEXT("StableValue"));
	FBlueprintEditorUtils::AddMemberVariable(
		Fixture.Blueprint, VariableName, PinType);
	FString SaveError;
	if (!TestTrue(TEXT("Default baseline saves"),
		SaveBlueprintMutationFixture(Fixture.Blueprint, SaveError)))
	{
		AddError(SaveError);
		return false;
	}
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();

	UObject* CDO = Fixture.Blueprint->GeneratedClass->GetDefaultObject();
	FIntProperty* Property = CastField<FIntProperty>(
		Fixture.Blueprint->GeneratedClass->FindPropertyByName(VariableName));
	if (!TestNotNull(TEXT("Integer CDO property"), Property))
	{
		return false;
	}
	const int32 InitialValue = Property->GetPropertyValue_InContainer(CDO);
	const bool bInitialDirty = Fixture.Package->IsDirty();
	int32 InvalidPropertyChangeEvents = 0;
	const FDelegateHandle PropertyChangedHandle =
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
			[CDO, VariableName, &InvalidPropertyChangeEvents](
				UObject* ChangedObject,
				FPropertyChangedEvent& Event)
			{
				if (ChangedObject == CDO
					&& Event.GetPropertyName() == VariableName)
				{
					++InvalidPropertyChangeEvents;
				}
			});
	const FMCPToolResult Invalid = Registry.ExecuteTool(
		TEXT("blueprint.default.set"),
		MakeBlueprintMutationParams(
			Fixture.PackageName, TEXT("stablevalue"), TEXT("47 trailing")));
	TestFalse(TEXT("Partially consumed Unreal text is rejected"), Invalid.bSuccess);
	TestEqual(TEXT("Invalid text reports a stable code"),
		Invalid.ErrorCode, FString(TEXT("property_value_invalid")));
	CDO = Fixture.Blueprint->GeneratedClass->GetDefaultObject();
	Property = CastField<FIntProperty>(
		Fixture.Blueprint->GeneratedClass->FindPropertyByName(VariableName));
	TestEqual(TEXT("Invalid import rolls the CDO value back"),
		Property->GetPropertyValue_InContainer(CDO), InitialValue);
	TestEqual(TEXT("Invalid import and guard restore dirty state"),
		Fixture.Package->IsDirty(), bInitialDirty);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	TestEqual(TEXT("Scratch validation emits no CDO property change event"),
		InvalidPropertyChangeEvents, 0);

	const FMCPToolResult Success = Registry.ExecuteTool(
		TEXT("blueprint.default.set"),
		MakeBlueprintMutationParams(
			Fixture.PackageName, TEXT("stablevalue"), TEXT("41")));
	if (TestTrue(TEXT("Case-insensitive default set succeeds"), Success.bSuccess)
		&& Success.Data)
	{
		TestEqual(TEXT("Read-back returns canonical property name"),
			Success.Data->GetStringField(TEXT("variable")),
			VariableName.ToString());
		TestEqual(TEXT("Receipt preserves old value"),
			Success.Data->GetStringField(TEXT("oldValue")),
			FString::FromInt(InitialValue));
		TestEqual(TEXT("Receipt returns normalized new value"),
			Success.Data->GetStringField(TEXT("newValue")), FString(TEXT("41")));
		TestTrue(TEXT("Default mutation compiles"),
			Success.Data->GetBoolField(TEXT("compiled")));
		TestTrue(TEXT("Default mutation saves"),
			Success.Data->GetBoolField(TEXT("saved")));
	}
	CDO = Fixture.Blueprint->GeneratedClass->GetDefaultObject();
	Property = CastField<FIntProperty>(
		Fixture.Blueprint->GeneratedClass->FindPropertyByName(VariableName));
	TestEqual(TEXT("CDO read-back survives compile"),
		Property->GetPropertyValue_InContainer(CDO), 41);
	TestTrue(TEXT("Saved default package exists on disk"),
		FPackageName::DoesPackageExist(Fixture.PackageName));
	TestFalse(TEXT("Saved default leaves package clean"), Fixture.Package->IsDirty());

	const int32 StableValue = Property->GetPropertyValue_InContainer(CDO);
	const FMCPToolResult Missing = Registry.ExecuteTool(
		TEXT("blueprint.default.set"),
		MakeBlueprintMutationParams(
			Fixture.PackageName, TEXT("MissingProperty"), TEXT("9")));
	TestFalse(TEXT("Unknown CDO property is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Unknown property has stable code"),
		Missing.ErrorCode, FString(TEXT("property_not_found")));
	TestEqual(TEXT("Unknown property performs zero CDO writes"),
		Property->GetPropertyValue_InContainer(CDO), StableValue);

	UPackage* DefaultPackage = Fixture.Blueprint->GetOutermost();
	TArray<UPackage*> PackagesToReload{DefaultPackage};
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
	TestTrue(TEXT("Default Blueprint reloads from disk"), bReloaded);
	if (bReloaded)
	{
		const FString ObjectPath = Fixture.PackageName + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(Fixture.PackageName);
		UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		TestNotNull(TEXT("Default Blueprint reopens"), Reloaded);
		if (Reloaded && Reloaded->GeneratedClass)
		{
			FIntProperty* ReloadedProperty = CastField<FIntProperty>(
				Reloaded->GeneratedClass->FindPropertyByName(VariableName));
			TestNotNull(TEXT("Persisted CDO property"), ReloadedProperty);
			if (ReloadedProperty)
			{
				TestEqual(TEXT("CDO default survives disk reload"),
					ReloadedProperty->GetPropertyValue_InContainer(
						Reloaded->GeneratedClass->GetDefaultObject()), 41);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintAssetRenameContractTest,
	"UE_AI_integration.Blueprint.Mutation.AssetRenameContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAssetRenameContractTest::RunTest(const FString&)
{
	const FBlueprintMutationFixture Source =
		CreateBlueprintMutationFixture(TEXT("UEAI_RenameSource"));
	const FBlueprintMutationFixture Collision =
		CreateBlueprintMutationFixture(TEXT("UEAI_RenameCollision"));
	const FString RenamedPackage = Source.PackageName + TEXT("_Moved");
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Renamed destination is deleted"),
			DeleteBlueprintMutationAsset(RenamedPackage));
		TestTrue(TEXT("Source redirector is deleted"),
			DeleteBlueprintMutationAsset(Source.PackageName));
		TestTrue(TEXT("Collision fixture is deleted"),
			DeleteBlueprintMutationAsset(Collision.PackageName));
	};
	if (!TestNotNull(TEXT("Rename source fixture"), Source.Blueprint)
		|| !TestNotNull(TEXT("Rename collision fixture"), Collision.Blueprint))
	{
		return false;
	}
	FString SaveError;
	if (!SaveBlueprintMutationFixture(Source.Blueprint, SaveError)
		|| !SaveBlueprintMutationFixture(Collision.Blueprint, SaveError))
	{
		AddError(TEXT("Rename fixture save failed: ") + SaveError);
		return false;
	}
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();
	auto Rename = [&Registry](const FString& AssetPath, const FString& NewPath)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), AssetPath);
		Params->SetStringField(TEXT("newPath"), NewPath);
		return Registry.ExecuteTool(TEXT("blueprint.asset.rename"), Params);
	};

	const FMCPToolResult Invalid = Rename(
		Source.PackageName, TEXT("/Game/Automation/Invalid/"));
	TestFalse(TEXT("Folder-only rename target is rejected"), Invalid.bSuccess);
	TestEqual(TEXT("Invalid rename has stable code"),
		Invalid.ErrorCode, FString(TEXT("invalid_asset_path")));
	TestEqual(TEXT("Invalid rename keeps source package identity"),
		Source.Blueprint->GetOutermost()->GetName(), Source.PackageName);

	const FMCPToolResult Conflict = Rename(
		Source.PackageName, Collision.PackageName);
	TestFalse(TEXT("Existing destination is rejected"), Conflict.bSuccess);
	TestEqual(TEXT("Rename conflict has stable code"),
		Conflict.ErrorCode, FString(TEXT("asset_already_exists")));
	TestEqual(TEXT("Conflict performs zero source rename writes"),
		Source.Blueprint->GetOutermost()->GetName(), Source.PackageName);

	const FMCPToolResult Success = Rename(Source.PackageName, RenamedPackage);
	if (TestTrue(TEXT("Long-package rename succeeds"), Success.bSuccess)
		&& Success.Data)
	{
		const FString RenamedObject = RenamedPackage + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(RenamedPackage);
		TestTrue(TEXT("Rename is verified by AssetTools read-back"),
			Success.Data->GetBoolField(TEXT("verified")));
		TestTrue(TEXT("Rename reports a real move"),
			Success.Data->GetBoolField(TEXT("renamed")));
		TestEqual(TEXT("Rename returns destination object path"),
			Success.Data->GetStringField(TEXT("newPath")), RenamedObject);
		TestEqual(TEXT("Live UObject moved to destination package"),
			Source.Blueprint->GetOutermost()->GetName(), RenamedPackage);
		TestTrue(TEXT("Destination asset is discoverable"),
			UEditorAssetLibrary::DoesAssetExist(RenamedPackage));
		TestTrue(TEXT("Rename persists destination package"),
			FPackageName::DoesPackageExist(RenamedPackage));
		TestFalse(TEXT("Renamed package is clean after AssetTools save"),
			Source.Blueprint->GetOutermost()->IsDirty());
		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")).Get();
		const FString SourceObject = Source.PackageName + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(Source.PackageName);
		const FAssetData SourceState = AssetRegistry.GetAssetByObjectPath(
			FSoftObjectPath(SourceObject));
		TestEqual(TEXT("Redirector receipt matches Asset Registry"),
			Success.Data->GetBoolField(TEXT("redirectorCreated")),
			SourceState.IsValid() && SourceState.IsRedirector());
	}

	const FMCPToolResult NoOp = Rename(RenamedPackage, RenamedPackage);
	if (TestTrue(TEXT("Same-path rename is idempotent"), NoOp.bSuccess)
		&& NoOp.Data)
	{
		TestFalse(TEXT("Same-path rename reports no move"),
			NoOp.Data->GetBoolField(TEXT("renamed")));
		TestTrue(TEXT("Same-path rename is still verified"),
			NoOp.Data->GetBoolField(TEXT("verified")));
	}

	UPackage* RenamedPackageObject = Source.Blueprint->GetOutermost();
	TArray<UPackage*> PackagesToReload{RenamedPackageObject};
	FText ReloadError;
	const bool bReloaded = UPackageTools::ReloadPackages(
		PackagesToReload,
		ReloadError,
		EReloadPackagesInteractionMode::AssumePositive);
	TestTrue(TEXT("Renamed Blueprint reloads from destination package"), bReloaded);
	if (bReloaded)
	{
		const FString RenamedObject = RenamedPackage + TEXT(".")
			+ FPackageName::GetLongPackageAssetName(RenamedPackage);
		TestNotNull(TEXT("Renamed Blueprint reopens from disk"),
			LoadObject<UBlueprint>(nullptr, *RenamedObject));
	}
	return true;
}

#endif
