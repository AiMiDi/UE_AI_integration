#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "FileHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Infrastructure/BlueprintMutationGuard.h"
#include "Infrastructure/Sha256.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "WidgetBlueprint.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
UBlueprint* CreateTransientTestBlueprint(
	const FString& PackageName,
	const FString& AssetName,
	UClass* ParentClass,
	UClass* BlueprintClass,
	UClass* GeneratedClass)
{
	UPackage* Package = CreatePackage(*PackageName);
	return Package
		? FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			*AssetName,
			BPTYPE_Normal,
			BlueprintClass,
			GeneratedClass,
			FName(TEXT("UEAI.BlueprintPersistenceTest")))
		: nullptr;
}

void DeleteFixtureFile(const FString& Filename)
{
	if (!Filename.IsEmpty() && FPaths::FileExists(Filename))
	{
		IFileManager::Get().Delete(*Filename, false, true, true);
	}
}

UK2Node_VariableSet* AddVariableSetNode(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	const FName VariableName,
	UScriptStruct* Struct)
{
	if (!Blueprint || !Graph || !Struct)
	{
		return nullptr;
	}
	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	PinType.PinSubCategoryObject = Struct;
	if (!FBlueprintEditorUtils::AddMemberVariable(
			Blueprint,
			VariableName,
			PinType))
	{
		return nullptr;
	}
	FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
	UK2Node_VariableSet* Node = Creator.CreateNode();
	Node->VariableReference.SetSelfMember(VariableName);
	Creator.Finalize();
	return Node;
}

TSharedRef<FJsonObject> NumberObject(
	std::initializer_list<TPair<const TCHAR*, double>> Values)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const TPair<const TCHAR*, double>& Value : Values)
	{
		Result->SetNumberField(Value.Key, Value.Value);
	}
	return Result;
}

TSharedRef<FJsonObject> TypedValue(
	const FString& Type,
	const TSharedPtr<FJsonObject>& Value)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);
	Result->SetObjectField(TEXT("value"), Value);
	return Result;
}

UEdGraphNode* FindNodeByGuid(UBlueprint* Blueprint, const FGuid& Guid)
{
	TArray<UEdGraph*> Graphs;
	if (Blueprint)
	{
		Blueprint->GetAllGraphs(Graphs);
	}
	for (UEdGraph* Graph : Graphs)
	{
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAIBlueprintPersistenceResolverTest,
	"UE_AI_integration.Blueprint.Persistence.ResolverAndMapSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAIBlueprintPersistenceResolverTest::RunTest(
	const FString& Parameters)
{
	using namespace UEAIIntegration::Infrastructure;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetPackageName =
		TEXT("/Game/Automation/UEAI_Persistence_BP_") + Suffix;
	const FString AssetName = FPackageName::GetLongPackageAssetName(
		AssetPackageName);
	UBlueprint* Blueprint = CreateTransientTestBlueprint(
		AssetPackageName,
		AssetName,
		AActor::StaticClass(),
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("ordinary Blueprint fixture"), Blueprint);

	FBlueprintPersistenceTarget Target;
	FBlueprintPersistenceError Error;
	if (Blueprint)
	{
		TestTrue(
			TEXT("ordinary Blueprint resolves"),
			ResolveBlueprintPersistenceTarget(Blueprint, Target, Error));
		TestEqual(
			TEXT("ordinary Blueprint top-level object"),
			Target.TopLevelObject,
			static_cast<UObject*>(Blueprint));
		TestEqual(
			TEXT("ordinary Blueprint extension"),
			FPaths::GetExtension(Target.Filename, true),
			FString(FPackageName::GetAssetPackageExtension()));
	}

	const FString WidgetPackageName =
		TEXT("/Game/Automation/UEAI_Persistence_WBP_") + Suffix;
	const FString WidgetName = FPackageName::GetLongPackageAssetName(
		WidgetPackageName);
	UBlueprint* WidgetBlueprint = CreateTransientTestBlueprint(
		WidgetPackageName,
		WidgetName,
		UUserWidget::StaticClass(),
		UWidgetBlueprint::StaticClass(),
		UWidgetBlueprintGeneratedClass::StaticClass());
	TestNotNull(TEXT("Widget Blueprint fixture"), WidgetBlueprint);
	if (WidgetBlueprint)
	{
		TestTrue(
			TEXT("Widget Blueprint resolves"),
			ResolveBlueprintPersistenceTarget(
				WidgetBlueprint,
				Target,
				Error));
		TestEqual(
			TEXT("Widget Blueprint remains an asset package"),
			Target.Kind,
			EBlueprintPackageKind::Asset);
		TestEqual(
			TEXT("Widget Blueprint top-level object"),
			Target.TopLevelObject,
			static_cast<UObject*>(WidgetBlueprint));
	}

	if (Blueprint && Blueprint->GetOutermost())
	{
		Blueprint->GetOutermost()->SetPackageFlags(PKG_ContainsMap);
		TestFalse(
			TEXT("ordinary Blueprint in a map-marked package is rejected"),
			ResolveBlueprintPersistenceTarget(Blueprint, Target, Error));
		TestEqual(
			TEXT("invalid asset/map combination is classified before save"),
			Error.Code,
			FString(TEXT("asset_package_type_mismatch")));
		Blueprint->GetOutermost()->ClearPackageFlags(PKG_ContainsMap);
	}

	if (!GEditor)
	{
		AddError(TEXT("Editor is unavailable for the Level Blueprint fixture."));
		return false;
	}

	const FString MapPackageName =
		TEXT("/Game/Automation/UEAI_Persistence_Map_") + Suffix;
	GEditor->CreateNewMapForEditing(
		/*bPromptUserToSave=*/false,
		/*bIsPartitionedWorld=*/false);
	UWorld* World = GEditor->GetEditorWorldContext().World();
	TestNotNull(TEXT("Editor world fixture"), World);
	if (!World || !UEditorLoadingAndSavingUtils::SaveMap(World, MapPackageName))
	{
		AddError(TEXT("Could not save the Level Blueprint map fixture."));
		return false;
	}
	World = GEditor->GetEditorWorldContext().World();
	ULevelScriptBlueprint* LevelBlueprint =
		World && World->PersistentLevel
			? World->PersistentLevel->GetLevelScriptBlueprint(false)
			: nullptr;
	TestNotNull(TEXT("LevelScriptBlueprint fixture"), LevelBlueprint);
	if (LevelBlueprint
		&& !UEditorLoadingAndSavingUtils::SaveMap(World, MapPackageName))
	{
		AddError(TEXT("Could not persist the LevelScriptBlueprint fixture."));
		return false;
	}
	FString MapFilename;
	if (LevelBlueprint)
	{
		TestTrue(
			TEXT("LevelScriptBlueprint resolves"),
			ResolveBlueprintPersistenceTarget(
				LevelBlueprint,
				Target,
				Error));
		TestEqual(
			TEXT("LevelScriptBlueprint persistence kind"),
			Target.Kind,
			EBlueprintPackageKind::Map);
		TestEqual(
			TEXT("LevelScriptBlueprint saves its UWorld"),
			Target.TopLevelObject,
			static_cast<UObject*>(World));
		TestEqual(
			TEXT("LevelScriptBlueprint extension"),
			FPaths::GetExtension(Target.Filename, true),
			FString(FPackageName::GetMapPackageExtension()));
		MapFilename = Target.Filename;
		LevelBlueprint->GetOutermost()->SetDirtyFlag(true);
		TestTrue(
			TEXT("LevelScriptBlueprint uses the map-safe save path"),
			SaveBlueprintPackage(LevelBlueprint, &Target, Error));
		TestFalse(
			TEXT("LevelScriptBlueprint map is clean after save"),
			LevelBlueprint->GetOutermost()->IsDirty());
	}

	GEditor->CreateNewMapForEditing(
		/*bPromptUserToSave=*/false,
		/*bIsPartitionedWorld=*/false);
	CollectGarbage(RF_NoFlags);
	DeleteFixtureFile(MapFilename);
	DeleteFixtureFile(FPackageName::LongPackageNameToFilename(
		AssetPackageName,
		FPackageName::GetAssetPackageExtension()));
	DeleteFixtureFile(FPackageName::LongPackageNameToFilename(
		WidgetPackageName,
		FPackageName::GetAssetPackageExtension()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAITypedPinPersistenceTest,
	"UE_AI_integration.Blueprint.Persistence.TypedStructPinRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAITypedPinPersistenceTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::Infrastructure;
	UUEAIIntegrationSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
		: nullptr;
	FMCPToolRegistry* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	if (!Registry)
	{
		AddError(TEXT("UE integration registry is unavailable."));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/Automation/UEAI_TypedPin_") + Suffix;
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UBlueprint* Blueprint = CreateTransientTestBlueprint(
		PackageName,
		AssetName,
		AActor::StaticClass(),
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	UEdGraph* Graph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0]
		: nullptr;
	TestNotNull(TEXT("typed pin Blueprint fixture"), Blueprint);
	TestNotNull(TEXT("typed pin EventGraph fixture"), Graph);
	if (!Blueprint || !Graph)
	{
		return false;
	}

	struct FPinCase
	{
		FString Type;
		FName Variable;
		UK2Node_VariableSet* Node = nullptr;
		TSharedPtr<FJsonObject> Value;
		FString Serialized;
		FGuid NodeGuid;
	};
	TArray<FPinCase> Cases;
	Cases.Add({
		TEXT("Vector"),
		TEXT("VectorValue"),
		nullptr,
		NumberObject({{TEXT("x"), 125.25}, {TEXT("y"), -64.5}, {TEXT("z"), 9.75}})});
	Cases.Add({
		TEXT("Rotator"),
		TEXT("RotatorValue"),
		nullptr,
		NumberObject({{TEXT("pitch"), 11.0}, {TEXT("yaw"), 72.5}, {TEXT("roll"), -8.0}})});
	TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
	Transform->SetObjectField(
		TEXT("translation"),
		NumberObject({{TEXT("x"), 3.0}, {TEXT("y"), 5.0}, {TEXT("z"), 7.0}}));
	Transform->SetObjectField(
		TEXT("rotation"),
		NumberObject({{TEXT("pitch"), 15.0}, {TEXT("yaw"), 25.0}, {TEXT("roll"), 35.0}}));
	Transform->SetObjectField(
		TEXT("scale3D"),
		NumberObject({{TEXT("x"), 1.25}, {TEXT("y"), 0.75}, {TEXT("z"), 2.0}}));
	Cases.Add({TEXT("Transform"), TEXT("TransformValue"), nullptr, Transform});
	Cases.Add({
		TEXT("LinearColor"),
		TEXT("ColorValue"),
		nullptr,
		NumberObject({{TEXT("r"), 0.125}, {TEXT("g"), 0.5}, {TEXT("b"), 0.875}, {TEXT("a"), 0.625}})});

	for (FPinCase& Case : Cases)
	{
		UScriptStruct* Struct = Case.Type == TEXT("Vector")
			? TBaseStructure<FVector>::Get()
			: Case.Type == TEXT("Rotator")
				? TBaseStructure<FRotator>::Get()
				: Case.Type == TEXT("Transform")
					? TBaseStructure<FTransform>::Get()
					: TBaseStructure<FLinearColor>::Get();
		Case.Node = AddVariableSetNode(
			Blueprint,
			Graph,
			Case.Variable,
			Struct);
		TestNotNull(*(Case.Type + TEXT(" variable set node")), Case.Node);
		if (Case.Node)
		{
			Case.NodeGuid = Case.Node->NodeGuid;
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint,
		EBlueprintCompileOptions::SkipSave);
	FBlueprintPersistenceTarget PersistenceTarget;
	FBlueprintPersistenceError PersistenceError;
	if (!SaveBlueprintPackage(
			Blueprint,
			&PersistenceTarget,
			PersistenceError))
	{
		AddError(TEXT("Could not save typed pin baseline: ") + PersistenceError.Message);
		return false;
	}

	auto ExecuteTypedSet = [&](const FPinCase& Case,
		const TSharedPtr<FJsonObject>& Value) -> FMCPToolResult
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), PackageName);
		Params->SetStringField(TEXT("nodeId"), Case.NodeGuid.ToString());
		Params->SetStringField(TEXT("pinName"), Case.Variable.ToString());
		Params->SetObjectField(TEXT("typedValue"), TypedValue(Case.Type, Value));
		return Registry->ExecuteTool(TEXT("blueprint.pin.default.set"), Params);
	};

	for (FPinCase& Case : Cases)
	{
		const FMCPToolResult Result = ExecuteTypedSet(Case, Case.Value);
		TestTrue(*(Case.Type + TEXT(" typed default is accepted")), Result.bSuccess);
		if (!Result.bSuccess || !Result.Data.IsValid())
		{
			AddError(Case.Type + TEXT(" failed: ") + Result.ErrorCode
				+ TEXT(" ") + Result.ErrorMessage);
			continue;
		}
		Case.Serialized = Result.Data->GetStringField(TEXT("serializedValue"));
		TestFalse(
			*(Case.Type + TEXT(" returns a serialized value")),
			Case.Serialized.IsEmpty());
		TestEqual(
			*(Case.Type + TEXT(" returns identical read-back")),
			Result.Data->GetObjectField(TEXT("readBack"))->GetStringField(
				TEXT("serializedValue")),
			Case.Serialized);
	}

	// All rejection paths must run before mutation and must preserve the clean
	// package/default state.
	FPinCase& VectorCase = Cases[0];
	UEdGraphNode* VectorNode = FindNodeByGuid(Blueprint, VectorCase.NodeGuid);
	UEdGraphPin* VectorPin = VectorNode
		? VectorNode->FindPin(VectorCase.Variable)
		: nullptr;
	const FString StableVectorDefault = VectorPin ? VectorPin->DefaultValue : FString();
	const bool bStableDirty = Blueprint->GetOutermost()->IsDirty();
	TSharedRef<FJsonObject> BadVector = NumberObject(
		{{TEXT("x"), 1.0}, {TEXT("y"), 2.0}});
	const FMCPToolResult BadFormat = ExecuteTypedSet(VectorCase, BadVector);
	TestEqual(TEXT("invalid typed format is rejected before mutation"),
		BadFormat.ErrorCode, FString(TEXT("pin_type_mismatch")));
	TestEqual(TEXT("invalid format keeps the pin default"),
		VectorPin->DefaultValue, StableVectorDefault);
	TestEqual(TEXT("invalid format keeps package Dirty"),
		Blueprint->GetOutermost()->IsDirty(), bStableDirty);

	VectorPin->PinType.bIsReference = true;
	const FMCPToolResult ByRef = ExecuteTypedSet(VectorCase, VectorCase.Value);
	VectorPin->PinType.bIsReference = false;
	TestEqual(TEXT("by-ref pin is rejected before mutation"),
		ByRef.ErrorCode, FString(TEXT("pin_by_ref")));
	TestEqual(TEXT("by-ref rejection keeps the pin default"),
		VectorPin->DefaultValue, StableVectorDefault);
	TestEqual(TEXT("by-ref rejection keeps package Dirty"),
		Blueprint->GetOutermost()->IsDirty(), bStableDirty);

	UEdGraphNode* ColorNode = FindNodeByGuid(Blueprint, Cases[3].NodeGuid);
	UEdGraphPin* ColorPin = ColorNode
		? ColorNode->FindPin(Cases[3].Variable)
		: nullptr;
	if (VectorPin && ColorPin)
	{
		VectorPin->LinkedTo.Add(ColorPin);
		const FMCPToolResult Linked = ExecuteTypedSet(VectorCase, VectorCase.Value);
		VectorPin->LinkedTo.Remove(ColorPin);
		TestEqual(TEXT("linked pin is rejected before mutation"),
			Linked.ErrorCode, FString(TEXT("pin_linked")));
		TestEqual(TEXT("linked rejection keeps the pin default"),
			VectorPin->DefaultValue, StableVectorDefault);
		TestEqual(TEXT("linked rejection keeps package Dirty"),
			Blueprint->GetOutermost()->IsDirty(), bStableDirty);
	}

	UPackage* Package = Blueprint->GetOutermost();
	TArray<UPackage*> Packages = {Package};
	FText ReloadError;
	TestTrue(
		TEXT("typed pin Blueprint reloads from disk"),
		UPackageTools::ReloadPackages(
			Packages,
			ReloadError,
			EReloadPackagesInteractionMode::AssumePositive));
	Blueprint = LoadObject<UBlueprint>(nullptr, *ObjectPath);
	TestNotNull(TEXT("typed pin Blueprint reopens"), Blueprint);
	if (Blueprint)
	{
		for (const FPinCase& Case : Cases)
		{
			UEdGraphNode* ReloadedNode = nullptr;
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* CandidateGraph : Graphs)
			{
				for (UEdGraphNode* Candidate : CandidateGraph->Nodes)
				{
					if (Candidate && Candidate->NodeGuid == Case.NodeGuid)
					{
						ReloadedNode = Candidate;
						break;
					}
				}
			}
			const UEdGraphPin* ReloadedPin = ReloadedNode
				? ReloadedNode->FindPin(Case.Variable)
				: nullptr;
			TestTrue(
				*(Case.Type + TEXT(" survives package reload")),
				ReloadedPin && ReloadedPin->DefaultValue == Case.Serialized);
		}
	}
	UEditorAssetLibrary::DeleteAsset(PackageName);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAISingleRequestMutationGuardTest,
	"UE_AI_integration.Blueprint.Persistence.SingleRequestMutationGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAISingleRequestMutationGuardTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::Infrastructure;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/Automation/UEAI_MutationGuard_") + Suffix;
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UBlueprint* Blueprint = CreateTransientTestBlueprint(
		PackageName,
		AssetName,
		AActor::StaticClass(),
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	UEdGraph* Graph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0]
		: nullptr;
	TestNotNull(TEXT("mutation guard Blueprint"), Blueprint);
	TestNotNull(TEXT("mutation guard graph"), Graph);
	if (!Blueprint || !Graph)
	{
		return false;
	}
	FGraphNodeCreator<UK2Node_Knot> FirstCreator(*Graph);
	UK2Node_Knot* First = FirstCreator.CreateNode();
	First->NodePosX = 100;
	First->NodePosY = 200;
	FirstCreator.Finalize();
	FGraphNodeCreator<UK2Node_Knot> SecondCreator(*Graph);
	UK2Node_Knot* Second = SecondCreator.CreateNode();
	Second->NodePosX = 400;
	Second->NodePosY = 200;
	SecondCreator.Finalize();
	UEdGraphPin* FirstOutput = First->GetOutputPin();
	UEdGraphPin* SecondInput = Second->GetInputPin();
	if (FirstOutput && SecondInput)
	{
		FirstOutput->MakeLinkTo(SecondInput);
	}
	TestTrue(
		TEXT("mutation guard baseline connection"),
		FirstOutput && SecondInput
			&& FirstOutput->LinkedTo.Contains(SecondInput));
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FBlueprintPersistenceTarget Target;
	FBlueprintPersistenceError PersistenceError;
	if (!SaveBlueprintPackage(Blueprint, &Target, PersistenceError))
	{
		AddError(TEXT("Could not save mutation guard baseline: ")
			+ PersistenceError.Message);
		return false;
	}
	TArray<uint8> BaselineBytes;
	TestTrue(
		TEXT("mutation guard baseline package is readable"),
		FFileHelper::LoadFileToArray(BaselineBytes, *Target.Filename));
	FString BaselineDigest;
	TestTrue(
		TEXT("mutation guard baseline package is hashable"),
		TrySha256Hex(BaselineBytes, BaselineDigest));
	const FGuid FirstGuid = First->NodeGuid;
	const FGuid SecondGuid = Second->NodeGuid;
	const bool bDirtyBefore = Blueprint->GetOutermost()->IsDirty();

	FBlueprintSingleRequestMutationGuard Guard(Blueprint);
	TestTrue(TEXT("single-request guard captures baseline"), Guard.IsValid());
	if (!Guard.IsValid())
	{
		AddError(Guard.GetErrorCode() + TEXT(": ") + Guard.GetErrorMessage());
		return false;
	}
	Guard.MarkMutationStarted();
	First->Modify();
	First->NodePosX = 9000;
	First->BreakAllNodeLinks();
	Second->Modify();
	Graph->Modify();
	Graph->RemoveNode(Second);
	UK2Node_Knot* Added = NewObject<UK2Node_Knot>(
		Graph, NAME_None, RF_Transactional);
	Added->CreateNewGuid();
	const FGuid AddedGuid = Added->NodeGuid;
	Added->AllocateDefaultPins();
	Graph->AddNode(Added, false, false);
	Blueprint->MarkPackageDirty();
	TArray<uint8> CorruptedBytes = BaselineBytes;
	CorruptedBytes.Add(0x7f);
	TestTrue(
		TEXT("test fault changes the on-disk package"),
		FFileHelper::SaveArrayToFile(CorruptedBytes, *Target.Filename));

	FString RollbackError;
	TestTrue(
		TEXT("single-request rollback verifies memory and disk"),
		Guard.Rollback(RollbackError));
	if (!RollbackError.IsEmpty())
	{
		AddError(RollbackError);
	}
	UEdGraphNode* RestoredFirst = FindNodeByGuid(Blueprint, FirstGuid);
	UEdGraphNode* RestoredSecond = FindNodeByGuid(Blueprint, SecondGuid);
	TestNotNull(TEXT("first node restored"), RestoredFirst);
	TestNotNull(TEXT("deleted node restored"), RestoredSecond);
	TestEqual(
		TEXT("node position restored"),
		RestoredFirst ? RestoredFirst->NodePosX : 0,
		100);
	const UEdGraphPin* RestoredOutput = Cast<UK2Node_Knot>(RestoredFirst)
		? Cast<UK2Node_Knot>(RestoredFirst)->GetOutputPin()
		: nullptr;
	const UEdGraphPin* RestoredInput = Cast<UK2Node_Knot>(RestoredSecond)
		? Cast<UK2Node_Knot>(RestoredSecond)->GetInputPin()
		: nullptr;
	TestTrue(
		TEXT("connection restored"),
		RestoredOutput && RestoredInput
			&& RestoredOutput->LinkedTo.Contains(RestoredInput));
	TestTrue(TEXT("new node removed"),
		FindNodeByGuid(Blueprint, AddedGuid) == nullptr);
	TestEqual(
		TEXT("package dirty state restored"),
		Blueprint->GetOutermost()->IsDirty(),
		bDirtyBefore);
	TArray<uint8> RestoredBytes;
	FString RestoredDigest;
	TestTrue(TEXT("restored package is readable"),
		FFileHelper::LoadFileToArray(RestoredBytes, *Target.Filename));
	TestTrue(TEXT("restored package is hashable"),
		TrySha256Hex(RestoredBytes, RestoredDigest));
	TestEqual(TEXT("disk package baseline restored"),
		RestoredDigest, BaselineDigest);
	// The guard and this test intentionally retain native references until the
	// stack unwinds. ForceDeleteObjects would therefore turn cleanup into an
	// unrelated ensure. Remove only the fixture-owned package file; the process
	// can reclaim the in-memory package normally after the test returns.
	DeleteFixtureFile(Target.Filename);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAIRcPrepareLevelBlueprintMapTest,
	"UE_AI_integration.Acceptance.PrepareLevelBlueprintMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAIRcPrepareLevelBlueprintMapTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::Infrastructure;
	if (FPlatformMisc::GetEnvironmentVariable(
		TEXT("UEAI_RC_PREPARE_LEVEL_MAP")) != TEXT("1"))
	{
		AddInfo(TEXT("RC Level Blueprint fixture preparation was not requested."));
		return true;
	}
	if (!GEditor)
	{
		AddError(TEXT("Editor is unavailable for RC Level Blueprint preparation."));
		return false;
	}
	const FString MapPackageName =
		TEXT("/Game/UEAI/Acceptance/LevelBlueprintFixture");
	GEditor->CreateNewMapForEditing(false, false);
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World || !UEditorLoadingAndSavingUtils::SaveMap(
		World, MapPackageName))
	{
		AddError(TEXT("Could not create the RC Level Blueprint fixture map."));
		return false;
	}
	World = GEditor->GetEditorWorldContext().World();
	ULevelScriptBlueprint* LevelBlueprint = World && World->PersistentLevel
		? World->PersistentLevel->GetLevelScriptBlueprint(false)
		: nullptr;
	FBlueprintPersistenceTarget Target;
	FBlueprintPersistenceError PersistenceError;
	if (LevelBlueprint)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
			LevelBlueprint);
	}
	if (!LevelBlueprint || !SaveBlueprintPackage(
			LevelBlueprint, &Target, PersistenceError))
	{
		AddError(TEXT("Could not persist the RC LevelScriptBlueprint baseline: ")
			+ PersistenceError.Code + TEXT(" ") + PersistenceError.Message);
		return false;
	}
	TestEqual(TEXT("RC fixture persists as a map"),
		FPaths::GetExtension(Target.Filename, true),
		FString(TEXT(".umap")));
	TestTrue(TEXT("RC fixture saves the owning world"),
		Target.TopLevelObject == World);
	TestFalse(
		TEXT("RC Level Blueprint baseline is clean"),
		LevelBlueprint->GetOutermost()->IsDirty());
	return true;
}

#endif
