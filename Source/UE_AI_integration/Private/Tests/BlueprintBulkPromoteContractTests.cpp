#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
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
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry);
}

namespace
{
UEdGraphNode* FindNodeByIdentity(UBlueprint* Blueprint, const FGuid& Guid)
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

// A real /Game/Automation/ Blueprint fixture so the handlers can load, compile,
// save, and roll back through the same Editor path as production.  Full Editor
// wiring (GEditor + AssetRegistry + FScopedTransaction) is available in the
// EditorContext automation flag, so the handlers are exercised end-to-end.
struct FBlueprintFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* EventGraph = nullptr;
	FMCPToolRegistry Registry;

	bool Build(const FString& Tag)
	{
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		PackageName = TEXT("/Game/Automation/UEAI_BulkPromote_") + Tag + TEXT("_") + Suffix;
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		Package = CreatePackage(*PackageName);
		Blueprint = Package
			? FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				Package,
				*AssetName,
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(),
				FName(TEXT("UEAI.BulkPromoteContractTest")))
			: nullptr;
		EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
			? Blueprint->UbergraphPages[0]
			: nullptr;
		if (!Blueprint || !EventGraph)
		{
			return false;
		}
		Registry.BeginDomainRegistration(TEXT("blueprint"));
		UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
		Registry.EndDomainRegistration();
		return true;
	}

	void Delete()
	{
		if (!PackageName.IsEmpty() && UEditorAssetLibrary::DoesAssetExist(PackageName))
		{
			UEditorAssetLibrary::DeleteAsset(PackageName);
		}
	}

	bool AddIntVariable(const FName& Name)
	{
		FEdGraphPinType IntType;
		IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, IntType);
	}

	bool Save()
	{
		using namespace UEAIIntegration::Infrastructure;
		FBlueprintPersistenceError Error;
		return SaveBlueprintPackage(Blueprint, nullptr, Error);
	}
};
} // namespace

// ============================================================
// (a) blueprint.pin.promote rejects missing/ambiguous pins
// ============================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintPinPromoteContractTest,
	"UE_AI_integration.Blueprint.PinPromoteContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintPinPromoteContractTest::RunTest(const FString&)
{
	FBlueprintFixture Fixture;
	if (!Fixture.Build(TEXT("Promote")))
	{
		AddError(TEXT("Could not create the promote Blueprint fixture."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		Fixture.Delete();
	};

	if (!Fixture.AddIntVariable(FName(TEXT("PromoteValue"))))
	{
		AddError(TEXT("Could not create the integer variable fixture."));
		return false;
	}
	FGraphNodeCreator<UK2Node_VariableSet> Creator(*Fixture.EventGraph);
	UK2Node_VariableSet* VariableSet = Creator.CreateNode();
	VariableSet->VariableReference.SetSelfMember(FName(TEXT("PromoteValue")));
	Creator.Finalize();
	const FGuid NodeGuid = VariableSet->NodeGuid;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture.Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		Fixture.Blueprint, EBlueprintCompileOptions::SkipSave);
	if (!Fixture.Save())
	{
		AddError(TEXT("Could not save the promote fixture."));
		return false;
	}

	const FString NodeId = NodeGuid.ToString();
	auto Promote = [&](const FString& PinName, const FString* PinId = nullptr)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), Fixture.PackageName);
		Params->SetStringField(TEXT("nodeId"), NodeId);
		if (PinId)
		{
			Params->SetStringField(TEXT("pinId"), *PinId);
		}
		else
		{
			Params->SetStringField(TEXT("pinName"), PinName);
		}
		return Fixture.Registry.ExecuteTool(TEXT("blueprint.pin.promote"), Params);
	};

	const FMCPToolResult MissingNode = Fixture.Registry.ExecuteTool(
		TEXT("blueprint.pin.promote"),
		[&]()
		{
			TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("blueprint"), Fixture.PackageName);
			Params->SetStringField(TEXT("nodeId"), FGuid::NewGuid().ToString());
			Params->SetStringField(TEXT("pinName"), TEXT("PromoteValue"));
			return Params;
		}());
	TestFalse(TEXT("promote rejects a missing node"), MissingNode.bSuccess);
	TestEqual(TEXT("missing node is classified as node_not_found"),
		MissingNode.ErrorCode, FString(TEXT("node_not_found")));

	const FMCPToolResult MissingPin = Promote(TEXT("DefinitelyNotAPin"));
	TestFalse(TEXT("promote rejects a missing pin"), MissingPin.bSuccess);
	TestEqual(TEXT("missing pin is classified as pin_not_found"),
		MissingPin.ErrorCode, FString(TEXT("pin_not_found")));

	// Duplicate the value-pin name on the exec input to force ambiguity, then
	// restore it.  Mirrors the pin-identity contract fixture.
	VariableSet = Cast<UK2Node_VariableSet>(FindNodeByIdentity(Fixture.Blueprint, NodeGuid));
	UEdGraphPin* ValuePin = VariableSet ? VariableSet->FindPin(FName(TEXT("PromoteValue"))) : nullptr;
	UEdGraphPin* OtherInput = nullptr;
	if (VariableSet)
	{
		for (UEdGraphPin* Candidate : VariableSet->Pins)
		{
			if (Candidate && Candidate != ValuePin && Candidate->Direction == EGPD_Input)
			{
				OtherInput = Candidate;
				break;
			}
		}
	}
	if (TestNotNull(TEXT("A second input pin is available for ambiguity"), OtherInput))
	{
		const FName OriginalExecName = OtherInput->PinName;
		OtherInput->PinName = FName(TEXT("PromoteValue"));
		const FMCPToolResult Ambiguous = Promote(TEXT("PromoteValue"));
		OtherInput->PinName = OriginalExecName;
		TestFalse(TEXT("promote rejects an ambiguous pin name"), Ambiguous.bSuccess);
		TestEqual(TEXT("ambiguous pin is classified as pin_ambiguous"),
			Ambiguous.ErrorCode, FString(TEXT("pin_ambiguous")));
	}

	const FMCPToolResult MissingIdentity = [&]()
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), Fixture.PackageName);
		Params->SetStringField(TEXT("nodeId"), NodeId);
		return Fixture.Registry.ExecuteTool(TEXT("blueprint.pin.promote"), Params);
	}();
	TestFalse(TEXT("promote rejects a missing pin identity"), MissingIdentity.bSuccess);
	TestEqual(TEXT("missing pin identity is classified as invalid_params"),
		MissingIdentity.ErrorCode, FString(TEXT("invalid_params")));

	return true;
}

// ============================================================
// (b) blueprint.node.bulk.add rejects an invalid class atomically
// ============================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintBulkAddContractTest,
	"UE_AI_integration.Blueprint.BulkAddContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBulkAddContractTest::RunTest(const FString&)
{
	FBlueprintFixture Fixture;
	if (!Fixture.Build(TEXT("BulkAdd")))
	{
		AddError(TEXT("Could not create the bulk-add Blueprint fixture."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		Fixture.Delete();
	};
	if (!Fixture.Save())
	{
		AddError(TEXT("Could not save the bulk-add fixture."));
		return false;
	}

	const int32 NodesBefore = Fixture.EventGraph->Nodes.Num();

	// First entry is valid, second is not: the whole request must be rejected
	// with no partial nodes left behind.
	TSharedRef<FJsonObject> BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	TArray<TSharedPtr<FJsonValue>> BadNodes;
	TSharedRef<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetStringField(TEXT("class"), TEXT("Branch"));
	TSharedRef<FJsonObject> Bad = MakeShared<FJsonObject>();
	Bad->SetStringField(TEXT("class"), TEXT("DefinitelyNotARealNodeType"));
	BadNodes.Add(MakeShared<FJsonValueObject>(Good));
	BadNodes.Add(MakeShared<FJsonValueObject>(Bad));
	BadParams->SetArrayField(TEXT("nodes"), BadNodes);
	const FMCPToolResult BadResult = Fixture.Registry.ExecuteTool(
		TEXT("blueprint.node.bulk.add"), BadParams);
	TestFalse(TEXT("bulk.add rejects an invalid node class"), BadResult.bSuccess);
	TestEqual(TEXT("bulk.add failure adds no nodes (atomicity)"),
		Fixture.EventGraph->Nodes.Num(), NodesBefore);

	// A single valid entry succeeds and adds exactly one node.
	TSharedRef<FJsonObject> GoodParams = MakeShared<FJsonObject>();
	GoodParams->SetStringField(TEXT("blueprint"), Fixture.PackageName);
	TArray<TSharedPtr<FJsonValue>> GoodNodes;
	TSharedRef<FJsonObject> Only = MakeShared<FJsonObject>();
	Only->SetStringField(TEXT("class"), TEXT("Branch"));
	GoodNodes.Add(MakeShared<FJsonValueObject>(Only));
	GoodParams->SetArrayField(TEXT("nodes"), GoodNodes);
	const FMCPToolResult GoodResult = Fixture.Registry.ExecuteTool(
		TEXT("blueprint.node.bulk.add"), GoodParams);
	TestTrue(TEXT("bulk.add succeeds for a valid node"), GoodResult.bSuccess);
	TestEqual(TEXT("bulk.add adds exactly one node"),
		Fixture.EventGraph->Nodes.Num(), NodesBefore + 1);

	return true;
}

// ============================================================
// (c) blueprint.pin.bulk.connect rejects self/already-connected pairs
// ============================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintBulkConnectContractTest,
	"UE_AI_integration.Blueprint.BulkConnectContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBulkConnectContractTest::RunTest(const FString&)
{
	FBlueprintFixture Fixture;
	if (!Fixture.Build(TEXT("BulkConnect")))
	{
		AddError(TEXT("Could not create the bulk-connect Blueprint fixture."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		Fixture.Delete();
	};

	if (!Fixture.AddIntVariable(FName(TEXT("ConnectValue"))))
	{
		AddError(TEXT("Could not create the integer variable fixture."));
		return false;
	}
	FGraphNodeCreator<UK2Node_VariableGet> GetCreator(*Fixture.EventGraph);
	UK2Node_VariableGet* GetNode = GetCreator.CreateNode();
	GetNode->VariableReference.SetSelfMember(FName(TEXT("ConnectValue")));
	GetCreator.Finalize();
	const FGuid GetGuid = GetNode->NodeGuid;

	FGraphNodeCreator<UK2Node_VariableSet> SetCreator(*Fixture.EventGraph);
	UK2Node_VariableSet* SetNode = SetCreator.CreateNode();
	SetNode->VariableReference.SetSelfMember(FName(TEXT("ConnectValue")));
	SetCreator.Finalize();
	const FGuid SetGuid = SetNode->NodeGuid;

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture.Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		Fixture.Blueprint, EBlueprintCompileOptions::SkipSave);
	if (!Fixture.Save())
	{
		AddError(TEXT("Could not save the bulk-connect fixture."));
		return false;
	}

	auto Connect = [&](const FString& SourceNodeId, const FString& SourcePin,
		const FString& TargetNodeId, const FString& TargetPin)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), Fixture.PackageName);
		TArray<TSharedPtr<FJsonValue>> Connections;
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("sourceNodeId"), SourceNodeId);
		Row->SetStringField(TEXT("sourcePinName"), SourcePin);
		Row->SetStringField(TEXT("targetNodeId"), TargetNodeId);
		Row->SetStringField(TEXT("targetPinName"), TargetPin);
		Connections.Add(MakeShared<FJsonValueObject>(Row));
		Params->SetArrayField(TEXT("connections"), Connections);
		return Fixture.Registry.ExecuteTool(TEXT("blueprint.pin.bulk.connect"), Params);
	};

	const FString PinName(TEXT("ConnectValue"));
	const FMCPToolResult Self = Connect(GetGuid.ToString(), PinName, GetGuid.ToString(), PinName);
	TestFalse(TEXT("bulk.connect rejects a self pair"), Self.bSuccess);
	TestEqual(TEXT("self pair is classified as pin_self_connection"),
		Self.ErrorCode, FString(TEXT("pin_self_connection")));

	const FMCPToolResult First = Connect(GetGuid.ToString(), PinName, SetGuid.ToString(), PinName);
	TestTrue(TEXT("bulk.connect succeeds for a compatible pair"), First.bSuccess);

	const FMCPToolResult Again = Connect(GetGuid.ToString(), PinName, SetGuid.ToString(), PinName);
	TestFalse(TEXT("bulk.connect rejects an already-connected pair"), Again.bSuccess);
	TestEqual(TEXT("already-connected pair is classified as pin_already_connected"),
		Again.ErrorCode, FString(TEXT("pin_already_connected")));

	return true;
}

// ============================================================
// (d) blueprint.pin.default.bulk.set validates each default
// ============================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintBulkDefaultSetContractTest,
	"UE_AI_integration.Blueprint.BulkDefaultSetContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintBulkDefaultSetContractTest::RunTest(const FString&)
{
	FBlueprintFixture Fixture;
	if (!Fixture.Build(TEXT("BulkDefault")))
	{
		AddError(TEXT("Could not create the bulk-default Blueprint fixture."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		Fixture.Delete();
	};

	if (!Fixture.AddIntVariable(FName(TEXT("DefaultValue"))))
	{
		AddError(TEXT("Could not create the integer variable fixture."));
		return false;
	}
	FGraphNodeCreator<UK2Node_VariableSet> Creator(*Fixture.EventGraph);
	UK2Node_VariableSet* VariableSet = Creator.CreateNode();
	VariableSet->VariableReference.SetSelfMember(FName(TEXT("DefaultValue")));
	Creator.Finalize();
	const FGuid NodeGuid = VariableSet->NodeGuid;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture.Blueprint);
	FKismetEditorUtilities::CompileBlueprint(
		Fixture.Blueprint, EBlueprintCompileOptions::SkipSave);
	if (!Fixture.Save())
	{
		AddError(TEXT("Could not save the bulk-default fixture."));
		return false;
	}

	const FString NodeId = NodeGuid.ToString();
	VariableSet = Cast<UK2Node_VariableSet>(FindNodeByIdentity(Fixture.Blueprint, NodeGuid));
	UEdGraphPin* ValuePin = VariableSet ? VariableSet->FindPin(FName(TEXT("DefaultValue"))) : nullptr;
	TestNotNull(TEXT("Default value input pin exists"), ValuePin);
	if (!ValuePin)
	{
		return false;
	}
	const FString Baseline = ValuePin->DefaultValue;

	auto SetDefaults = [&](const TArray<FString>& Values)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), Fixture.PackageName);
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (const FString& EntryValue : Values)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("nodeId"), NodeId);
			Row->SetStringField(TEXT("pinName"), TEXT("DefaultValue"));
			Row->SetStringField(TEXT("value"), EntryValue);
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Params->SetArrayField(TEXT("defaults"), Rows);
		return Fixture.Registry.ExecuteTool(TEXT("blueprint.pin.default.bulk.set"), Params);
	};

	// Invalid integer default is rejected per-entry.
	const FMCPToolResult Invalid = SetDefaults({ TEXT("not-an-int") });
	TestFalse(TEXT("bulk.default.set rejects an invalid default"), Invalid.bSuccess);
	TestEqual(TEXT("invalid default is classified as pin_default_invalid"),
		Invalid.ErrorCode, FString(TEXT("pin_default_invalid")));

	// A valid value followed by an invalid one is rejected atomically; the
	// valid value must be rolled back.
	const FMCPToolResult Mixed = SetDefaults({ TEXT("42"), TEXT("bad") });
	TestFalse(TEXT("bulk.default.set rejects a mixed valid/invalid batch"), Mixed.bSuccess);
	VariableSet = Cast<UK2Node_VariableSet>(FindNodeByIdentity(Fixture.Blueprint, NodeGuid));
	ValuePin = VariableSet ? VariableSet->FindPin(FName(TEXT("DefaultValue"))) : nullptr;
	TestEqual(TEXT("mixed batch rolls back the valid default"),
		ValuePin ? ValuePin->DefaultValue : FString(), Baseline);

	// A single valid default persists.
	const FMCPToolResult Valid = SetDefaults({ TEXT("42") });
	TestTrue(TEXT("bulk.default.set succeeds for a valid default"), Valid.bSuccess);
	VariableSet = Cast<UK2Node_VariableSet>(FindNodeByIdentity(Fixture.Blueprint, NodeGuid));
	ValuePin = VariableSet ? VariableSet->FindPin(FName(TEXT("DefaultValue"))) : nullptr;
	TestEqual(TEXT("valid default is applied"), ValuePin ? ValuePin->DefaultValue : FString(),
		FString(TEXT("42")));

	return true;
}

#endif
