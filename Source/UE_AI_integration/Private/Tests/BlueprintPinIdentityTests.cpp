#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry);
void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
}

namespace
{
UEdGraphNode* FindPinIdentityNodeByGuid(
	UBlueprint* Blueprint,
	const FGuid& Guid)
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

TSharedRef<FJsonObject> MakeRawPinSet(
	const FString& Blueprint,
	const FString& NodeId,
	const FString& PinId,
	const FString& Value,
	const FString* PinName = nullptr)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	Params->SetStringField(TEXT("nodeId"), NodeId);
	if (!PinId.IsEmpty())
	{
		Params->SetStringField(TEXT("pinId"), PinId);
	}
	Params->SetStringField(TEXT("value"), Value);
	if (PinName)
	{
		Params->SetStringField(TEXT("pinName"), *PinName);
	}
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintPinIdentityContractTest,
	"UE_AI_integration.Blueprint.Mutation.PinIdentityContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPinIdentityContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::Infrastructure;

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString PackageName =
		TEXT("/Game/Automation/UEAI_PinIdentity_") + Suffix;
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprint* Blueprint = Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			*AssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UEAI.BlueprintPinIdentityTest")))
		: nullptr;
	UEdGraph* Graph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0]
		: nullptr;
	TestNotNull(TEXT("Pin identity Blueprint fixture"), Blueprint);
	TestNotNull(TEXT("Pin identity graph fixture"), Graph);
	ON_SCOPE_EXIT
	{
		const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
			|| UEditorAssetLibrary::DeleteAsset(PackageName);
		TestTrue(TEXT("Pin identity fixture is deleted"),
			bDeleted && !UEditorAssetLibrary::DoesAssetExist(PackageName));
	};
	if (!Blueprint || !Graph)
	{
		return false;
	}

	FEdGraphPinType PinType;
	PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
	const FName VariableName(TEXT("StableValue"));
	if (!FBlueprintEditorUtils::AddMemberVariable(
		Blueprint, VariableName, PinType))
	{
		AddError(TEXT("Could not create the integer pin fixture."));
		return false;
	}

	FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
	UK2Node_VariableSet* VariableSet = Creator.CreateNode();
	VariableSet->VariableReference.SetSelfMember(VariableName);
	Creator.Finalize();
	const FGuid NodeGuid = VariableSet->NodeGuid;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint, EBlueprintCompileOptions::SkipSave);

	VariableSet = Cast<UK2Node_VariableSet>(
		FindPinIdentityNodeByGuid(Blueprint, NodeGuid));
	UEdGraphPin* ValuePin = VariableSet
		? VariableSet->FindPin(VariableName)
		: nullptr;
	TestNotNull(TEXT("Stable value input pin"), ValuePin);
	if (!ValuePin || !ValuePin->PinId.IsValid())
	{
		AddError(TEXT("The pin fixture did not produce a stable PinId."));
		return false;
	}

	FBlueprintPersistenceTarget PersistenceTarget;
	FBlueprintPersistenceError PersistenceError;
	if (!SaveBlueprintPackage(
		Blueprint, &PersistenceTarget, PersistenceError))
	{
		AddError(TEXT("Could not save the pin identity baseline: ")
			+ PersistenceError.Message);
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(Registry);
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();

	const FString NodeId = NodeGuid.ToString();
	TSharedRef<FJsonObject> GraphParams = MakeShared<FJsonObject>();
	// Exercise the public blueprint.graph.get contract.  The handler accepts
	// several historical aliases internally, but tests must not rely on fields
	// rejected by the shipped additionalProperties:false schema.
	GraphParams->SetStringField(TEXT("name"), PackageName);
	GraphParams->SetStringField(TEXT("graph"), Graph->GetName());
	GraphParams->SetStringField(TEXT("geometryMode"), TEXT("stored"));
	const FMCPToolResult GraphResult = Registry.ExecuteTool(
		TEXT("blueprint.graph.get"), GraphParams);
	FString PinId;
	if (TestTrue(TEXT("Graph read succeeds before pin mutation"),
		GraphResult.bSuccess)
		&& TestNotNull(TEXT("Graph read data"), GraphResult.Data.Get()))
	{
		for (const TSharedPtr<FJsonValue>& NodeValue :
			GraphResult.Data->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			if (!Node.IsValid()
				|| Node->GetStringField(TEXT("nodeId")) != NodeId)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& PinValue :
				Node->GetArrayField(TEXT("pins")))
			{
				const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
				if (Pin.IsValid()
					&& Pin->GetStringField(TEXT("name")) == VariableName.ToString()
					&& Pin->GetStringField(TEXT("direction")) == TEXT("Input"))
				{
					PinId = Pin->GetStringField(TEXT("pinId"));
					TestEqual(TEXT("Graph read exposes the integer pin type"),
						Pin->GetStringField(TEXT("type")),
						UEdGraphSchema_K2::PC_Int.ToString());
					break;
				}
			}
			break;
		}
	}
	TestFalse(TEXT("Graph read exposes a stable PinId"), PinId.IsEmpty());
	TestEqual(TEXT("Graph read PinId matches the native identity"),
		PinId, ValuePin->PinId.ToString());
	if (PinId.IsEmpty())
	{
		return false;
	}
	const FMCPToolResult ById = Registry.ExecuteTool(
		TEXT("blueprint.pin.default.set"),
		MakeRawPinSet(PackageName, NodeId, PinId, TEXT("17")));
	if (TestTrue(TEXT("PinId-only mutation succeeds"), ById.bSuccess)
		&& TestNotNull(TEXT("PinId-only mutation data"), ById.Data.Get()))
	{
		const TSharedPtr<FJsonObject> ReadBack =
			ById.Data->GetObjectField(TEXT("readBack"));
		TestEqual(TEXT("Read-back preserves the selected PinId"),
			ReadBack->GetStringField(TEXT("pinId")), PinId);
		TestEqual(TEXT("Read-back exposes the canonical pin name"),
			ReadBack->GetStringField(TEXT("pinName")), VariableName.ToString());
		TestEqual(TEXT("Read-back exposes the serialized value"),
			ReadBack->GetStringField(TEXT("serializedValue")), FString(TEXT("17")));
	}

	VariableSet = Cast<UK2Node_VariableSet>(
		FindPinIdentityNodeByGuid(Blueprint, NodeGuid));
	ValuePin = VariableSet ? VariableSet->FindPin(VariableName) : nullptr;
	const FString StableDefault = ValuePin ? ValuePin->DefaultValue : FString();
	const bool bStableDirty = Package->IsDirty();
	const FString WrongName(TEXT("DefinitelyNotStableValue"));
	const FMCPToolResult Mismatch = Registry.ExecuteTool(
		TEXT("blueprint.pin.default.set"),
		MakeRawPinSet(PackageName, NodeId, PinId, TEXT("19"), &WrongName));
	TestFalse(TEXT("PinId and pinName mismatch is rejected"), Mismatch.bSuccess);
	TestEqual(TEXT("Pin identity mismatch has a stable conflict code"),
		Mismatch.ErrorCode, FString(TEXT("pin_identity_mismatch")));
	TestEqual(TEXT("Pin identity mismatch does not mutate the value"),
		ValuePin ? ValuePin->DefaultValue : FString(), StableDefault);
	TestEqual(TEXT("Pin identity mismatch preserves dirty state"),
		Package->IsDirty(), bStableDirty);

	UEdGraphPin* DuplicateNamePin = nullptr;
	if (VariableSet)
	{
		for (UEdGraphPin* Candidate : VariableSet->Pins)
		{
			if (Candidate && Candidate != ValuePin
				&& Candidate->Direction == EGPD_Input)
			{
				DuplicateNamePin = Candidate;
				break;
			}
		}
	}
	if (TestNotNull(TEXT("A second input pin is available for ambiguity"),
		DuplicateNamePin))
	{
		const FName OriginalName = DuplicateNamePin->PinName;
		const FString DuplicatePinName = VariableName.ToString();
		DuplicateNamePin->PinName = VariableName;
		const FMCPToolResult Ambiguous = Registry.ExecuteTool(
			TEXT("blueprint.pin.default.set"),
			MakeRawPinSet(
				PackageName,
				NodeId,
				FString(),
				TEXT("21"),
				&DuplicatePinName));
		DuplicateNamePin->PinName = OriginalName;
		TestFalse(TEXT("Name-only duplicate pin identity is rejected"),
			Ambiguous.bSuccess);
		TestEqual(TEXT("Duplicate pin name has a stable ambiguity code"),
			Ambiguous.ErrorCode, FString(TEXT("pin_ambiguous")));
		TestEqual(TEXT("Ambiguous pin name does not mutate the value"),
			ValuePin ? ValuePin->DefaultValue : FString(), StableDefault);
	}

	const FMCPToolResult MissingPin = Registry.ExecuteTool(
		TEXT("blueprint.pin.default.set"),
		MakeRawPinSet(
			PackageName,
			NodeId,
			FGuid::NewGuid().ToString(),
			TEXT("23")));
	TestFalse(TEXT("Unknown PinId is rejected"), MissingPin.bSuccess);
	TestEqual(TEXT("Unknown PinId has a stable not-found code"),
		MissingPin.ErrorCode, FString(TEXT("pin_not_found")));
	TestEqual(TEXT("Unknown PinId does not mutate the value"),
		ValuePin ? ValuePin->DefaultValue : FString(), StableDefault);

	const FMCPToolResult InvalidPinId = Registry.ExecuteTool(
		TEXT("blueprint.pin.default.set"),
		MakeRawPinSet(
			PackageName, NodeId, TEXT("not-a-guid"), TEXT("29")));
	TestFalse(TEXT("Malformed PinId is rejected"), InvalidPinId.bSuccess);
	TestEqual(TEXT("Malformed PinId is classified as invalid params"),
		InvalidPinId.ErrorCode, FString(TEXT("invalid_params")));

	TSharedRef<FJsonObject> MissingIdentity = MakeShared<FJsonObject>();
	MissingIdentity->SetStringField(TEXT("blueprint"), PackageName);
	MissingIdentity->SetStringField(TEXT("nodeId"), NodeId);
	MissingIdentity->SetStringField(TEXT("value"), TEXT("31"));
	const FMCPToolResult Missing = Registry.ExecuteTool(
		TEXT("blueprint.pin.default.set"), MissingIdentity);
	TestFalse(TEXT("Missing pin identity is rejected"), Missing.bSuccess);
	TestEqual(TEXT("Missing pin identity is classified as invalid params"),
		Missing.ErrorCode, FString(TEXT("invalid_params")));

	return true;
}

#endif
