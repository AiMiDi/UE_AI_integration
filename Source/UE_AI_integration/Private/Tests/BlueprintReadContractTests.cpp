#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
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
void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintReadFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
};

FBlueprintReadFixture CreateBlueprintReadFixture(const FString& Prefix)
{
	FBlueprintReadFixture Fixture;
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
			FName(TEXT("UEAI.BlueprintReadContract")))
		: nullptr;
	Fixture.Graph = Fixture.Blueprint
		&& !Fixture.Blueprint->UbergraphPages.IsEmpty()
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	return Fixture;
}

bool DeleteBlueprintReadFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

bool SaveBlueprintReadFixture(UBlueprint* Blueprint, FString& OutError)
{
	if (!Blueprint)
	{
		OutError = TEXT("Blueprint fixture is null.");
		return false;
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
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

template <typename T>
T* AddReadNode(UEdGraph* Graph, const uint32 StableId)
{
	T* Node = NewObject<T>(Graph, NAME_None, RF_Transactional);
	Node->NodeGuid = FGuid(0, 0, 0, StableId);
	Graph->AddNode(Node, false, false);
	Node->AllocateDefaultPins();
	return Node;
}

UK2Node_CustomEvent* AddCustomEvent(
	UEdGraph* Graph,
	const uint32 StableId,
	const FName EventName)
{
	UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(
		Graph, NAME_None, RF_Transactional);
	Node->CustomFunctionName = EventName;
	Node->NodeGuid = FGuid(0, 0, 0, StableId);
	Graph->AddNode(Node, false, false);
	Node->AllocateDefaultPins();
	return Node;
}

UEdGraphPin* FindTypedPin(
	UEdGraphNode* Node,
	const FName Category,
	const EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction
			&& Pin->PinType.PinCategory == Category)
		{
			return Pin;
		}
	}
	return nullptr;
}

TSharedRef<FJsonObject> MakeDescribeParams(
	const FBlueprintReadFixture& Fixture,
	const int32 MaxNodes,
	const int32 MaxEdges)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), Fixture.PackageName);
	Params->SetStringField(TEXT("graph"), Fixture.Graph->GetName());
	Params->SetNumberField(TEXT("maxNodes"), MaxNodes);
	Params->SetNumberField(TEXT("maxEdges"), MaxEdges);
	return Params;
}

const TSharedPtr<FJsonObject> FindResultByField(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const FString& Field,
	const FString& Expected)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object =
			Value.IsValid() ? Value->AsObject() : nullptr;
		if (Object.IsValid()
			&& Object->HasTypedField<EJson::String>(Field)
			&& Object->GetStringField(Field) == Expected)
		{
			return Object;
		}
	}
	return nullptr;
}

TArray<FString> BuildStableUsageKeys(
	const TArray<TSharedPtr<FJsonValue>>& Values)
{
	TArray<FString> Keys;
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object =
			Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			continue;
		}
		auto StringOrEmpty = [&Object](const TCHAR* Field)
		{
			FString Result;
			Object->TryGetStringField(Field, Result);
			return Result;
		};
		Keys.Add(
			StringOrEmpty(TEXT("blueprintPath")) + TEXT("|")
			+ StringOrEmpty(TEXT("usage")) + TEXT("|")
			+ StringOrEmpty(TEXT("graphId")) + TEXT("|")
			+ StringOrEmpty(TEXT("nodeId")) + TEXT("|")
			+ StringOrEmpty(TEXT("pinId")) + TEXT("|")
			+ StringOrEmpty(TEXT("variableGuid")));
	}
	return Keys;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintExecutionFlowReadContractTest,
	"UE_AI_integration.Blueprint.Query.ExecutionFlowContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintExecutionFlowReadContractTest::RunTest(const FString&)
{
	const FBlueprintReadFixture Fixture =
		CreateBlueprintReadFixture(TEXT("UEAI_ExecutionFlow"));
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Execution-flow fixture and package are deleted"),
			DeleteBlueprintReadFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Execution-flow Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Execution-flow graph fixture"), Fixture.Graph))
	{
		return false;
	}

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	const FName BoolVariable(TEXT("DataOnlyCondition"));
	if (!TestTrue(TEXT("Data-only member is created"),
		FBlueprintEditorUtils::AddMemberVariable(
			Fixture.Blueprint, BoolVariable, BoolType)))
	{
		return false;
	}

	UK2Node_CustomEvent* Event = AddCustomEvent(
		Fixture.Graph, 1, TEXT("ExecutionStart"));
	UK2Node_IfThenElse* FirstBranch =
		AddReadNode<UK2Node_IfThenElse>(Fixture.Graph, 2);
	UK2Node_IfThenElse* SecondBranch =
		AddReadNode<UK2Node_IfThenElse>(Fixture.Graph, 3);
	UK2Node_IfThenElse* RootBranch =
		AddReadNode<UK2Node_IfThenElse>(Fixture.Graph, 4);
	UK2Node_IfThenElse* CycleA =
		AddReadNode<UK2Node_IfThenElse>(Fixture.Graph, 5);
	UK2Node_IfThenElse* CycleB =
		AddReadNode<UK2Node_IfThenElse>(Fixture.Graph, 6);
	UK2Node_VariableGet* DataOnly =
		AddReadNode<UK2Node_VariableGet>(Fixture.Graph, 7);
	DataOnly->VariableReference.SetSelfMember(BoolVariable);
	DataOnly->ReconstructNode();

	UEdGraphPin* EventOutput = FindTypedPin(
		Event, UEdGraphSchema_K2::PC_Exec, EGPD_Output);
	UEdGraphPin* FirstInput = FindTypedPin(
		FirstBranch, UEdGraphSchema_K2::PC_Exec, EGPD_Input);
	UEdGraphPin* FirstOutput = FindTypedPin(
		FirstBranch, UEdGraphSchema_K2::PC_Exec, EGPD_Output);
	UEdGraphPin* SecondInput = FindTypedPin(
		SecondBranch, UEdGraphSchema_K2::PC_Exec, EGPD_Input);
	UEdGraphPin* CycleAInput = FindTypedPin(
		CycleA, UEdGraphSchema_K2::PC_Exec, EGPD_Input);
	UEdGraphPin* CycleAOutput = FindTypedPin(
		CycleA, UEdGraphSchema_K2::PC_Exec, EGPD_Output);
	UEdGraphPin* CycleBInput = FindTypedPin(
		CycleB, UEdGraphSchema_K2::PC_Exec, EGPD_Input);
	UEdGraphPin* CycleBOutput = FindTypedPin(
		CycleB, UEdGraphSchema_K2::PC_Exec, EGPD_Output);
	UEdGraphPin* DataOutput = DataOnly->FindPin(BoolVariable);
	UEdGraphPin* ConditionInput = FirstBranch->FindPin(
		UEdGraphSchema_K2::PN_Condition);
	if (!TestNotNull(TEXT("Event execution output"), EventOutput)
		|| !TestNotNull(TEXT("First branch execution input"), FirstInput)
		|| !TestNotNull(TEXT("First branch execution output"), FirstOutput)
		|| !TestNotNull(TEXT("Second branch execution input"), SecondInput)
		|| !TestNotNull(TEXT("Cycle A execution input"), CycleAInput)
		|| !TestNotNull(TEXT("Cycle A execution output"), CycleAOutput)
		|| !TestNotNull(TEXT("Cycle B execution input"), CycleBInput)
		|| !TestNotNull(TEXT("Cycle B execution output"), CycleBOutput)
		|| !TestNotNull(TEXT("Pure data output"), DataOutput)
		|| !TestNotNull(TEXT("Branch condition input"), ConditionInput))
	{
		return false;
	}
	if (!TestTrue(TEXT("Event connects to the first branch"),
		Fixture.Graph->GetSchema()->TryCreateConnection(
			EventOutput, FirstInput))
		|| !TestTrue(TEXT("First branch connects to the second branch"),
			Fixture.Graph->GetSchema()->TryCreateConnection(
				FirstOutput, SecondInput))
		|| !TestTrue(TEXT("Pure data connects to a condition pin"),
			Fixture.Graph->GetSchema()->TryCreateConnection(
				DataOutput, ConditionInput)))
	{
		return false;
	}
	// K2 cycle validation rejects an authored execution cycle.  The reader must
	// still bound and classify a malformed or partially reconstructed graph, so
	// construct the two links directly in this transient test fixture.
	CycleAOutput->MakeLinkTo(CycleBInput);
	CycleBOutput->MakeLinkTo(CycleAInput);

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(Registry);
	Registry.EndDomainRegistration();
	const FMCPToolResult Full = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"),
		MakeDescribeParams(Fixture, 100, 100));
	if (!TestTrue(TEXT("Execution-flow description succeeds"), Full.bSuccess)
		|| !TestNotNull(TEXT("Execution-flow result data"), Full.Data.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Only nodes with execution pins are counted"),
		Full.Data->GetIntegerField(TEXT("executionNodeTotal")), 6);
	TestEqual(TEXT("Only execution-pin links are counted"),
		Full.Data->GetIntegerField(TEXT("executionEdgeTotal")), 4);
	TestEqual(TEXT("Full node page returns every execution node"),
		Full.Data->GetIntegerField(TEXT("nodeCount")), 6);
	TestEqual(TEXT("Full edge page returns every execution edge"),
		Full.Data->GetIntegerField(TEXT("edgeCount")), 4);
	TestFalse(TEXT("Full node page is not truncated"),
		Full.Data->GetBoolField(TEXT("nodesTruncated")));
	TestFalse(TEXT("Full edge page is not truncated"),
		Full.Data->GetBoolField(TEXT("edgesTruncated")));
	TestEqual(TEXT("Event, root, and disconnected cycle are entry points"),
		Full.Data->GetIntegerField(TEXT("entryPointCount")), 3);

	const TArray<TSharedPtr<FJsonValue>>& Nodes =
		Full.Data->GetArrayField(TEXT("nodes"));
	TestNull(TEXT("Pure data node is excluded from execution flow"),
		FindResultByField(Nodes, TEXT("nodeId"), DataOnly->NodeGuid.ToString()).Get());
	const TSharedPtr<FJsonObject> EventRow = FindResultByField(
		Nodes, TEXT("nodeId"), Event->NodeGuid.ToString());
	if (TestNotNull(TEXT("Event entry is returned"), EventRow.Get()))
	{
		TestTrue(TEXT("Event is marked as an entry point"),
			EventRow->GetBoolField(TEXT("isEntryPoint")));
		TestEqual(TEXT("Event entry kind is explicit"),
			EventRow->GetStringField(TEXT("entryKind")), FString(TEXT("event")));
	}
	const TSharedPtr<FJsonObject> RootRow = FindResultByField(
		Nodes, TEXT("nodeId"), RootBranch->NodeGuid.ToString());
	if (TestNotNull(TEXT("Unlinked execution root is returned"), RootRow.Get()))
	{
		TestEqual(TEXT("Unlinked execution node is classified as a root"),
			RootRow->GetStringField(TEXT("entryKind")),
			FString(TEXT("executionRoot")));
	}
	const TSharedPtr<FJsonObject> CycleEntry = FindResultByField(
		Nodes, TEXT("nodeId"), CycleA->NodeGuid.ToString());
	if (TestNotNull(TEXT("Disconnected cycle is returned"), CycleEntry.Get()))
	{
		TestEqual(TEXT("Cycle traversal has a deterministic synthetic entry"),
			CycleEntry->GetStringField(TEXT("entryKind")),
			FString(TEXT("disconnected")));
	}

	const FMCPToolResult Replay = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"),
		MakeDescribeParams(Fixture, 100, 100));
	if (TestTrue(TEXT("Execution-flow replay succeeds"), Replay.bSuccess)
		&& Replay.Data)
	{
		TestEqual(TEXT("Execution-flow ordering is deterministic"),
			Replay.Data->GetStringField(TEXT("description")),
			Full.Data->GetStringField(TEXT("description")));
	}

	// Scan budgets bound source inspection independently from result paging.
	// A partial prefix must never be presented as complete execution totals.
	auto ScanLimitedParams = MakeDescribeParams(Fixture, 100, 100);
	ScanLimitedParams->SetNumberField(TEXT("maxScannedNodes"), 2);
	const FMCPToolResult ScanLimited = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"), ScanLimitedParams);
	if (TestTrue(TEXT("Bounded execution scan returns its inspected prefix"),
			ScanLimited.bSuccess)
		&& ScanLimited.Data)
	{
		TestEqual(TEXT("Graph node total remains explicit"),
			ScanLimited.Data->GetIntegerField(TEXT("graphNodeTotal")),
			Fixture.Graph->Nodes.Num());
		TestEqual(TEXT("Only the declared node-slot prefix is inspected"),
			ScanLimited.Data->GetIntegerField(TEXT("scannedNodeSlotCount")),
			2);
		TestTrue(TEXT("Node-slot exhaustion is explicit"),
			ScanLimited.Data->GetBoolField(TEXT("scanExhausted")));
		TestFalse(TEXT("Partial scan does not claim complete execution totals"),
			ScanLimited.Data->GetBoolField(TEXT("executionTotalsComplete")));
		TestTrue(TEXT("Partial source scan marks the response partial"),
			ScanLimited.Data->GetBoolField(TEXT("partial")));
	}

	// Exact entry selection outside an exhausted prefix must fail closed rather
	// than claiming the node does not exist or silently selecting another root.
	auto UnscannedEntryParams = MakeDescribeParams(Fixture, 100, 100);
	UnscannedEntryParams->SetNumberField(TEXT("maxScannedNodes"), 2);
	UnscannedEntryParams->SetStringField(
		TEXT("entryNodeId"), CycleA->NodeGuid.ToString());
	const FMCPToolResult UnscannedEntry = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"), UnscannedEntryParams);
	TestFalse(TEXT("Entry outside an exhausted scan prefix is rejected"),
		UnscannedEntry.bSuccess);
	TestEqual(TEXT("Incomplete entry lookup uses a stable 413 code"),
		UnscannedEntry.ErrorCode,
		FString(TEXT("execution_entry_scan_incomplete")));
	TestEqual(TEXT("Incomplete entry lookup uses payload-too-large status"),
		UnscannedEntry.HttpStatus,
		413);

	const FMCPToolResult NodeLimited = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"),
		MakeDescribeParams(Fixture, 2, 100));
	if (TestTrue(TEXT("Bounded node page succeeds"), NodeLimited.bSuccess)
		&& NodeLimited.Data)
	{
		TestEqual(TEXT("Node bound is enforced"),
			NodeLimited.Data->GetIntegerField(TEXT("nodeCount")), 2);
		TestTrue(TEXT("Node truncation is explicit"),
			NodeLimited.Data->GetBoolField(TEXT("nodesTruncated")));
	}
	const FMCPToolResult EdgeLimited = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"),
		MakeDescribeParams(Fixture, 100, 1));
	if (TestTrue(TEXT("Bounded edge page succeeds"), EdgeLimited.bSuccess)
		&& EdgeLimited.Data)
	{
		TestEqual(TEXT("Edge bound is enforced"),
			EdgeLimited.Data->GetIntegerField(TEXT("edgeCount")), 1);
		TestTrue(TEXT("Edge truncation is explicit"),
			EdgeLimited.Data->GetBoolField(TEXT("edgesTruncated")));
	}
	auto Fractional = MakeDescribeParams(Fixture, 100, 100);
	Fractional->SetNumberField(TEXT("maxNodes"), 1.5);
	const FMCPToolResult Invalid = Registry.ExecuteTool(
		TEXT("blueprint.graph.describe"), Fractional);
	TestFalse(TEXT("Fractional node bounds are rejected"), Invalid.bSuccess);
	TestEqual(TEXT("Fractional node bound has a stable error code"),
		Invalid.ErrorCode, FString(TEXT("invalid_params")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintTypeUsageReadContractTest,
	"UE_AI_integration.Blueprint.Query.TypeUsageContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintTypeUsageReadContractTest::RunTest(const FString&)
{
	const FBlueprintReadFixture Fixture =
		CreateBlueprintReadFixture(TEXT("UEAI_TypeUsage"));
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Type-usage fixture and package are deleted"),
			DeleteBlueprintReadFixture(Fixture.PackageName));
	};
	if (!TestNotNull(TEXT("Type-usage Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Type-usage graph fixture"), Fixture.Graph))
	{
		return false;
	}

	FEdGraphPinType IntType;
	IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
	const FName VariableName(TEXT("SearchedInteger"));
	if (!TestTrue(TEXT("Search member variable is created"),
		FBlueprintEditorUtils::AddMemberVariable(
			Fixture.Blueprint, VariableName, IntType)))
	{
		return false;
	}
	UK2Node_CustomEvent* Event = AddCustomEvent(
		Fixture.Graph, 100, TEXT("TypedEvent"));
	UEdGraphPin* ParameterPin = Event->CreateUserDefinedPin(
		TEXT("InputNumber"), IntType, EGPD_Output);
	UK2Node_VariableGet* Getter =
		AddReadNode<UK2Node_VariableGet>(Fixture.Graph, 101);
	Getter->VariableReference.SetSelfMember(VariableName);
	Getter->ReconstructNode();
	TArray<UK2Node_VariableSet*> Setters;
	for (uint32 StableId = 102; StableId <= 104; ++StableId)
	{
		UK2Node_VariableSet* Setter =
			AddReadNode<UK2Node_VariableSet>(Fixture.Graph, StableId);
		Setter->VariableReference.SetSelfMember(VariableName);
		Setter->ReconstructNode();
		Setters.Add(Setter);
	}
	UEdGraphPin* GetterValue = Getter->FindPin(VariableName);
	if (!TestNotNull(TEXT("Custom-event parameter pin"), ParameterPin)
		|| !TestNotNull(TEXT("Getter value pin"), GetterValue)
		|| !TestEqual(TEXT("Three setter fixtures are created"), Setters.Num(), 3))
	{
		return false;
	}
	for (UK2Node_VariableSet* Setter : Setters)
	{
		UEdGraphPin* SetterValue = Setter ? Setter->FindPin(VariableName) : nullptr;
		if (!TestNotNull(TEXT("Setter value pin"), SetterValue)
			|| !TestTrue(TEXT("Getter connects to each setter"),
				Fixture.Graph->GetSchema()->TryCreateConnection(
					GetterValue, SetterValue)))
		{
			return false;
		}
	}
	FString SaveError;
	if (!TestTrue(TEXT("Type-usage fixture saves"),
		SaveBlueprintReadFixture(Fixture.Blueprint, SaveError)))
	{
		AddError(SaveError);
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(Registry);
	Registry.EndDomainRegistration();
	auto MakeSearchParams = [&Fixture](
		const int32 MaxResults,
		const int32 MaxConnectionsPerPin)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("typeName"), TEXT("int"));
		Params->SetStringField(
			TEXT("filter"),
			FPackageName::GetLongPackageAssetName(Fixture.PackageName));
		Params->SetNumberField(TEXT("maxResults"), MaxResults);
		Params->SetNumberField(
			TEXT("maxConnectionsPerPin"), MaxConnectionsPerPin);
		return Params;
	};
	const FMCPToolResult Full = Registry.ExecuteTool(
		TEXT("blueprint.asset.search_by_type"),
		MakeSearchParams(100, 2));
	if (!TestTrue(TEXT("Type-usage search succeeds"), Full.bSuccess)
		|| !TestNotNull(TEXT("Type-usage search data"), Full.Data.Get()))
	{
		return false;
	}
	TestFalse(TEXT("Complete usage result is not truncated"),
		Full.Data->GetBoolField(TEXT("resultTruncated")));
	TestFalse(TEXT("Complete usage scan stays within its budgets"),
		Full.Data->GetBoolField(TEXT("scanExhausted")));
	TestFalse(TEXT("Complete usage result is not partial"),
		Full.Data->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("Connection page bound is echoed"),
		Full.Data->GetIntegerField(TEXT("maxConnectionsPerPin")), 2);
	const TArray<TSharedPtr<FJsonValue>>& Results =
		Full.Data->GetArrayField(TEXT("results"));
	const TSharedPtr<FJsonObject> VariableResult = FindResultByField(
		Results, TEXT("usage"), TEXT("variable"));
	if (TestNotNull(TEXT("Member-variable usage is returned"),
		VariableResult.Get()))
	{
		TestEqual(TEXT("Variable usage keeps member identity"),
			VariableResult->GetStringField(TEXT("location")),
			VariableName.ToString());
		TestFalse(TEXT("Variable usage keeps its stable GUID"),
			VariableResult->GetStringField(TEXT("variableGuid")).IsEmpty());
	}
	const TSharedPtr<FJsonObject> ParameterResult = FindResultByField(
		Results, TEXT("usage"), TEXT("parameter"));
	if (TestNotNull(TEXT("Function/event parameter usage is returned"),
		ParameterResult.Get()))
	{
		TestEqual(TEXT("Parameter pin name is explicit"),
			ParameterResult->GetStringField(TEXT("pinName")),
			FString(TEXT("InputNumber")));
		TestEqual(TEXT("Custom-event parameter direction is input"),
			ParameterResult->GetStringField(TEXT("parameterDirection")),
			FString(TEXT("input")));
	}

	TSharedPtr<FJsonObject> GetterConnection;
	for (const TSharedPtr<FJsonValue>& Value : Results)
	{
		const TSharedPtr<FJsonObject> Object =
			Value.IsValid() ? Value->AsObject() : nullptr;
		if (Object.IsValid()
			&& Object->GetStringField(TEXT("usage")) == TEXT("pinConnection")
			&& Object->GetStringField(TEXT("nodeId")) == Getter->NodeGuid.ToString())
		{
			GetterConnection = Object;
			break;
		}
	}
	if (TestNotNull(TEXT("Getter pin connection usage is returned"),
		GetterConnection.Get()))
	{
		TestEqual(TEXT("Full connection count is preserved"),
			GetterConnection->GetIntegerField(TEXT("connectionCount")), 3);
		TestTrue(TEXT("Connection sub-page truncation is explicit"),
			GetterConnection->GetBoolField(TEXT("connectionsTruncated")));
		const TArray<TSharedPtr<FJsonValue>>& Connections =
			GetterConnection->GetArrayField(TEXT("connections"));
		TestEqual(TEXT("Connection sub-page respects its bound"),
			Connections.Num(), 2);
		FString PreviousKey;
		for (const TSharedPtr<FJsonValue>& Value : Connections)
		{
			const TSharedPtr<FJsonObject> Connection =
				Value.IsValid() ? Value->AsObject() : nullptr;
			if (!TestNotNull(TEXT("Connection row is an object"),
				Connection.Get()))
			{
				continue;
			}
			const FString Key = Connection->GetStringField(TEXT("nodeId"))
				+ TEXT("|") + Connection->GetStringField(TEXT("pinId"));
			TestTrue(TEXT("Connections have stable identity ordering"),
				PreviousKey.IsEmpty() || PreviousKey < Key);
			PreviousKey = Key;
		}
	}

	const FMCPToolResult Replay = Registry.ExecuteTool(
		TEXT("blueprint.asset.search_by_type"),
		MakeSearchParams(100, 2));
	if (TestTrue(TEXT("Type-usage replay succeeds"), Replay.bSuccess)
		&& Replay.Data)
	{
		const TArray<FString> FirstKeys = BuildStableUsageKeys(Results);
		const TArray<FString> ReplayKeys = BuildStableUsageKeys(
			Replay.Data->GetArrayField(TEXT("results")));
		if (TestEqual(TEXT("Usage replay returns the same number of rows"),
			ReplayKeys.Num(), FirstKeys.Num()))
		{
			for (int32 Index = 0; Index < FirstKeys.Num(); ++Index)
			{
				TestEqual(TEXT("Usage result ordering is deterministic"),
					ReplayKeys[Index], FirstKeys[Index]);
			}
		}
	}
	const FMCPToolResult Limited = Registry.ExecuteTool(
		TEXT("blueprint.asset.search_by_type"),
		MakeSearchParams(1, 2));
	if (TestTrue(TEXT("Bounded type-usage search succeeds"), Limited.bSuccess)
		&& Limited.Data)
	{
		TestEqual(TEXT("Result bound is enforced"),
			Limited.Data->GetIntegerField(TEXT("resultCount")), 1);
		TestTrue(TEXT("Result truncation is explicit"),
			Limited.Data->GetBoolField(TEXT("resultTruncated")));
		TestTrue(TEXT("Result truncation marks the response partial"),
			Limited.Data->GetBoolField(TEXT("partial")));
	}
	auto Fractional = MakeSearchParams(100, 2);
	Fractional->SetNumberField(TEXT("maxResults"), 1.5);
	const FMCPToolResult FractionalResult = Registry.ExecuteTool(
		TEXT("blueprint.asset.search_by_type"), Fractional);
	TestFalse(TEXT("Fractional result limits are rejected"),
		FractionalResult.bSuccess);
	TestEqual(TEXT("Fractional result limit has a stable error code"),
		FractionalResult.ErrorCode, FString(TEXT("invalid_params")));
	auto ExcessiveConnections = MakeSearchParams(100, 65);
	const FMCPToolResult Excessive = Registry.ExecuteTool(
		TEXT("blueprint.asset.search_by_type"), ExcessiveConnections);
	TestFalse(TEXT("Excessive connection limits are rejected"),
		Excessive.bSuccess);
	TestEqual(TEXT("Excessive connection limit has a stable error code"),
		Excessive.ErrorCode, FString(TEXT("invalid_params")));
	return true;
}

#endif
