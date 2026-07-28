// Blueprint engineering analysis: bounded scan, cross-asset call graph and
// explicit correlation with runtime evidence.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/Runtime/BlueprintDebugService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_MacroInstance.h"

namespace
{
using UEAIIntegration::Infrastructure::DigestJson;
using UEAIIntegration::Infrastructure::MakeFinding;
using UEAIIntegration::Infrastructure::MakeStableId;
using UEAIIntegration::Infrastructure::SetBoundedArray;

constexpr int32 DefaultAssetLimit = 50;
constexpr int32 MaxAssetLimit = 200;
constexpr int32 DefaultFindingLimit = 200;
constexpr int32 MaxFindingLimit = 1000;
constexpr int32 DefaultGraphNodeLimit = 1000;
constexpr int32 MaxGraphNodeLimit = 5000;
constexpr int32 MaxStoredScans = 32;

struct FScanRecord
{
	FString ScanId;
	FString ScopeDigest;
	TArray<TSharedPtr<FJsonObject>> Findings;
	FDateTime CreatedAt;
};

TMap<FString, FScanRecord>& GetScanRecords()
{
	static TMap<FString, FScanRecord> Records;
	return Records;
}

FString GuidString(const UEdGraphNode* Node)
{
	return Node
		? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
		: FString();
}

FString NodeStableId(
	const FString& AssetPath,
	const FString& GraphName,
	const UEdGraphNode* Node)
{
	return MakeStableId(
		TEXT("bpnode"),
		{AssetPath, GraphName, GuidString(Node)});
}

int32 ReadBoundedInteger(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	const int32 Default,
	const int32 Minimum,
	const int32 Maximum)
{
	double Value = Default;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(Field, Value);
	}
	return FMath::Clamp(static_cast<int32>(Value), Minimum, Maximum);
}

bool MatchesScope(
	const FAssetData& Asset,
	const FString& RequestedAsset,
	const FString& PathPrefix)
{
	const FString PackagePath = Asset.PackageName.ToString();
	const FString ObjectPath = Asset.GetObjectPathString();
	if (!RequestedAsset.IsEmpty()
		&& !PackagePath.Equals(RequestedAsset, ESearchCase::IgnoreCase)
		&& !ObjectPath.Equals(RequestedAsset, ESearchCase::IgnoreCase)
		&& !Asset.AssetName.ToString().Equals(RequestedAsset, ESearchCase::IgnoreCase))
	{
		return false;
	}
	return PathPrefix.IsEmpty()
		|| PackagePath.StartsWith(PathPrefix, ESearchCase::IgnoreCase);
}

TArray<FAssetData> ResolveBlueprintAssets(
	const TSharedPtr<FJsonObject>& Params,
	int32& OutMatchedTotal,
	const int32 AssetLimit)
{
	FString RequestedAsset;
	FString PathPrefix;
	Params->TryGetStringField(TEXT("asset"), RequestedAsset);
	Params->TryGetStringField(TEXT("pathPrefix"), PathPrefix);

	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(
		UBlueprint::StaticClass()->GetClassPathName(),
		Assets,
		true);
	Assets.Sort(
		[](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.LexicalLess(Right.PackageName);
		});

	TArray<FAssetData> Selected;
	OutMatchedTotal = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (!MatchesScope(Asset, RequestedAsset, PathPrefix))
		{
			continue;
		}
		++OutMatchedTotal;
		if (Selected.Num() < AssetLimit)
		{
			Selected.Add(Asset);
		}
	}
	return Selected;
}

bool GraphHasTickEvent(const UEdGraph* Graph)
{
	if (!Graph)
	{
		return false;
	}
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
		if (!Event)
		{
			continue;
		}
		const FString EventName = Event->EventReference.GetMemberName().ToString();
		if (EventName.Equals(TEXT("ReceiveTick"), ESearchCase::IgnoreCase)
			|| EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

TSharedRef<FJsonObject> MakeNodeEvidence(
	const UEdGraphNode* Node,
	const FString& NodeId,
	const FString& Detail)
{
	TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(TEXT("nodeId"), NodeId);
	Evidence->SetStringField(
		TEXT("nodeClass"),
		Node ? Node->GetClass()->GetName() : FString());
	Evidence->SetStringField(
		TEXT("nodeTitle"),
		Node ? Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : FString());
	if (!Detail.IsEmpty())
	{
		Evidence->SetStringField(TEXT("detail"), Detail);
	}
	return Evidence;
}

void AppendFinding(
	TArray<TSharedPtr<FJsonObject>>& Findings,
	const FString& RuleId,
	const FString& Severity,
	const double Confidence,
	const FString& AssetPath,
	const FString& GraphName,
	const UEdGraphNode* Node,
	const FString& Message,
	const FString& Detail = FString())
{
	// Keep analysis memory bounded even if a broad scope contains pathological
	// graphs. One extra entry lets response projection report truncation.
	if (Findings.Num() >= MaxFindingLimit + 1)
	{
		return;
	}
	const FString NodeId = NodeStableId(AssetPath, GraphName, Node);
	Findings.Add(
		MakeFinding(
			RuleId,
			Severity,
			Confidence,
			AssetPath,
			GraphName,
			GuidString(Node),
			Message,
			MakeNodeEvidence(Node, NodeId, Detail)));
}

bool IsGlobalTraversal(const FString& FunctionName)
{
	static const TSet<FString> Names = {
		TEXT("GetAllActorsOfClass"),
		TEXT("GetAllActorsWithInterface"),
		TEXT("GetAllActorsOfClassWithTag"),
		TEXT("GetAllWidgetsOfClass"),
		TEXT("GetAllActorsWithTag"),
	};
	return Names.Contains(FunctionName);
}

bool IsSynchronousLoad(const FString& FunctionName)
{
	return FunctionName.Contains(TEXT("LoadAsset"), ESearchCase::IgnoreCase)
		|| FunctionName.Contains(TEXT("LoadClass"), ESearchCase::IgnoreCase)
		|| FunctionName.Contains(TEXT("StaticLoad"), ESearchCase::IgnoreCase)
		|| FunctionName.Equals(TEXT("LoadObject"), ESearchCase::IgnoreCase);
}

bool IsLoopMacro(const UK2Node_MacroInstance* Macro)
{
	if (!Macro)
	{
		return false;
	}
	const FString Title = Macro->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	return Title.Contains(TEXT("ForLoop"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("ForEachLoop"), ESearchCase::IgnoreCase)
		|| Title.Contains(TEXT("WhileLoop"), ESearchCase::IgnoreCase);
}

bool HasDisconnectedExecOutput(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return false;
	}
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin
			&& Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->LinkedTo.IsEmpty()
			&& !Pin->bHidden)
		{
			return true;
		}
	}
	return false;
}

void ScanBlueprint(
	UBlueprint* Blueprint,
	const FString& AssetPath,
	TArray<TSharedPtr<FJsonObject>>& Findings)
{
	if (!Blueprint)
	{
		return;
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	Graphs.Sort(
		[](const UEdGraph& Left, const UEdGraph& Right)
		{
			return Left.GetName() < Right.GetName();
		});

	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}

		const FString GraphName = Graph->GetName();
		const bool bTickGraph = GraphHasTickEvent(Graph);
		if (bTickGraph)
		{
			const UEdGraphNode* TickNode = nullptr;
			for (const UEdGraphNode* Candidate : Graph->Nodes)
			{
				const UK2Node_Event* Event = Cast<UK2Node_Event>(Candidate);
				if (Event
					&& (Event->EventReference.GetMemberName() == TEXT("ReceiveTick")
						|| Event->EventReference.GetMemberName() == TEXT("Tick")))
				{
					TickNode = Candidate;
					break;
				}
			}
			AppendFinding(
				Findings,
				TEXT("blueprint.tick.present"),
				TEXT("info"),
				1.0,
				AssetPath,
				GraphName,
				TickNode,
				TEXT("Blueprint contains an Event Tick entry point."));
		}

		TMap<FString, TArray<const UEdGraphNode*>> DelegateBindings;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				const FString FunctionName = Call->GetFunctionName().ToString();
				if (IsGlobalTraversal(FunctionName))
				{
					AppendFinding(
						Findings,
						TEXT("blueprint.call.global_traversal"),
						bTickGraph ? TEXT("error") : TEXT("warning"),
						0.98,
						AssetPath,
						GraphName,
						Node,
						FString::Printf(
							TEXT("Global traversal call '%s' can scale with world size."),
							*FunctionName),
						bTickGraph ? TEXT("Call is in a graph containing Event Tick.") : FString());
				}
				if (IsSynchronousLoad(FunctionName))
				{
					AppendFinding(
						Findings,
						TEXT("blueprint.call.synchronous_load"),
						bTickGraph ? TEXT("error") : TEXT("warning"),
						0.9,
						AssetPath,
						GraphName,
						Node,
						FString::Printf(
							TEXT("Synchronous load call '%s' can stall the game thread."),
							*FunctionName));
				}
				if (!Call->GetTargetFunction() && !FunctionName.IsEmpty())
				{
					AppendFinding(
						Findings,
						TEXT("blueprint.reference.unresolved_function"),
						TEXT("error"),
						0.95,
						AssetPath,
						GraphName,
						Node,
						FString::Printf(
							TEXT("Function reference '%s' cannot be resolved."),
							*FunctionName));
				}
			}

			if (Cast<UK2Node_DynamicCast>(Node))
			{
				AppendFinding(
					Findings,
					TEXT("blueprint.cast.dynamic"),
					bTickGraph ? TEXT("warning") : TEXT("info"),
					bTickGraph ? 0.9 : 0.65,
					AssetPath,
					GraphName,
					Node,
					bTickGraph
						? TEXT("Dynamic cast is in a graph containing Event Tick.")
						: TEXT("Dynamic cast should be reviewed for coupling and call frequency."));
			}

			if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node);
				IsLoopMacro(Macro))
			{
				AppendFinding(
					Findings,
					TEXT("blueprint.loop.review"),
					bTickGraph ? TEXT("warning") : TEXT("info"),
					0.7,
					AssetPath,
					GraphName,
					Node,
					TEXT("Loop bounds are data-dependent and should be reviewed."));
			}

			if (Cast<UK2Node_AddDelegate>(Node))
			{
				DelegateBindings.FindOrAdd(
					Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()).Add(Node);
			}

			if (HasDisconnectedExecOutput(Node)
				&& !Cast<UK2Node_Event>(Node))
			{
				AppendFinding(
					Findings,
					TEXT("blueprint.exec.disconnected_output"),
					TEXT("info"),
					0.8,
					AssetPath,
					GraphName,
					Node,
					TEXT("Execution output is not connected."));
			}
		}

		for (const TPair<FString, TArray<const UEdGraphNode*>>& Pair : DelegateBindings)
		{
			if (Pair.Value.Num() < 2)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Pair.Value)
			{
				AppendFinding(
					Findings,
					TEXT("blueprint.delegate.duplicate_binding"),
					TEXT("warning"),
					0.75,
					AssetPath,
					GraphName,
					Node,
					FString::Printf(
						TEXT("Delegate binding '%s' appears %d times in the same graph."),
						*Pair.Key,
						Pair.Value.Num()));
			}
		}
	}
}

void AppendDependencyCycleFindings(
	const TArray<FAssetData>& Assets,
	TArray<TSharedPtr<FJsonObject>>& Findings)
{
	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TSet<FName> SelectedPackages;
	for (const FAssetData& Asset : Assets)
	{
		SelectedPackages.Add(Asset.PackageName);
	}

	TMap<FName, TSet<FName>> Dependencies;
	for (const FName& Package : SelectedPackages)
	{
		TArray<FName> Related;
		Registry.GetDependencies(
			Package,
			Related,
			UE::AssetRegistry::EDependencyCategory::Package);
		TSet<FName>& SelectedRelated = Dependencies.FindOrAdd(Package);
		for (const FName& Dependency : Related)
		{
			if (SelectedPackages.Contains(Dependency))
			{
				SelectedRelated.Add(Dependency);
			}
		}
	}

	TArray<FName> Packages = SelectedPackages.Array();
	Packages.Sort(FNameLexicalLess());
	for (const FName& Package : Packages)
	{
		TArray<FName> Related = Dependencies.FindRef(Package).Array();
		Related.Sort(FNameLexicalLess());
		for (const FName& Dependency : Related)
		{
			// Emit each direct cycle once. Longer cycles remain represented in
			// content.asset.dependencies for a dedicated graph traversal.
			if (!Package.LexicalLess(Dependency)
				|| !Dependencies.FindRef(Dependency).Contains(Package))
			{
				continue;
			}
			TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
			Evidence->SetStringField(TEXT("otherAsset"), Dependency.ToString());
			Evidence->SetStringField(TEXT("cycleKind"), TEXT("directPackageDependency"));
			Findings.Add(
				MakeFinding(
					TEXT("blueprint.dependency.cycle"),
					TEXT("high"),
					1.0,
					Package.ToString(),
					FString(),
					FString(),
					FString::Printf(
						TEXT("Blueprint packages '%s' and '%s' depend on each other."),
						*Package.ToString(),
						*Dependency.ToString()),
					Evidence));
			if (Findings.Num() >= MaxFindingLimit + 1)
			{
				return;
			}
		}
	}
}

TSharedRef<FJsonObject> DescribeCallEndpoint(
	const FString& AssetPath,
	const FString& GraphName,
	const UEdGraphNode* Node,
	const FString& FunctionName)
{
	TSharedRef<FJsonObject> Endpoint = MakeShared<FJsonObject>();
	Endpoint->SetStringField(TEXT("assetPath"), AssetPath);
	Endpoint->SetStringField(TEXT("graph"), GraphName);
	Endpoint->SetStringField(TEXT("function"), FunctionName);
	Endpoint->SetStringField(TEXT("nodeGuid"), GuidString(Node));
	Endpoint->SetStringField(
		TEXT("nodeId"),
		NodeStableId(AssetPath, GraphName, Node));
	return Endpoint;
}

FString BlueprintPackageForClass(const UClass* Class)
{
	if (!Class)
	{
		return FString();
	}
	if (const UBlueprint* Source = Cast<UBlueprint>(Class->ClassGeneratedBy))
	{
		return Source->GetOutermost()->GetName();
	}
	return Class->GetPathName();
}

class FTool_BlueprintScan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.scan");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const int32 AssetLimit =
			ReadBoundedInteger(Params, TEXT("assetLimit"), DefaultAssetLimit, 1, MaxAssetLimit);
		const int32 FindingLimit =
			ReadBoundedInteger(Params, TEXT("findingLimit"), DefaultFindingLimit, 1, MaxFindingLimit);
		int32 MatchedAssets = 0;
		const TArray<FAssetData> Assets =
			ResolveBlueprintAssets(Params, MatchedAssets, AssetLimit);

		TArray<TSharedPtr<FJsonObject>> FindingObjects;
		TArray<TSharedPtr<FJsonValue>> AssetValues;
		for (const FAssetData& Asset : Assets)
		{
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			if (!Blueprint)
			{
				continue;
			}
			const FString AssetPath = Asset.PackageName.ToString();
			AssetValues.Add(MakeShared<FJsonValueString>(AssetPath));
			ScanBlueprint(Blueprint, AssetPath, FindingObjects);
		}
		AppendDependencyCycleFindings(Assets, FindingObjects);

		FindingObjects.Sort(
			[](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
			{
				return Left->GetStringField(TEXT("findingId"))
					< Right->GetStringField(TEXT("findingId"));
			});
		TArray<TSharedPtr<FJsonValue>> FindingValues;
		FindingValues.Reserve(FindingObjects.Num());
		for (const TSharedPtr<FJsonObject>& Finding : FindingObjects)
		{
			FindingValues.Add(MakeShared<FJsonValueObject>(Finding));
		}

		TSharedRef<FJsonObject> Scope = MakeShared<FJsonObject>();
		FString RequestedAsset;
		FString PathPrefix;
		Params->TryGetStringField(TEXT("asset"), RequestedAsset);
		Params->TryGetStringField(TEXT("pathPrefix"), PathPrefix);
		Scope->SetStringField(TEXT("asset"), RequestedAsset);
		Scope->SetStringField(TEXT("pathPrefix"), PathPrefix);
		Scope->SetArrayField(TEXT("resolvedAssets"), AssetValues);
		const FString ScopeDigest = DigestJson(Scope);

		TSharedRef<FJsonObject> ScanIdentity = MakeShared<FJsonObject>();
		ScanIdentity->SetStringField(TEXT("scopeDigest"), ScopeDigest);
		ScanIdentity->SetArrayField(TEXT("findings"), FindingValues);
		const FString ScanId =
			MakeStableId(TEXT("bpscan"), {ScopeDigest, DigestJson(ScanIdentity)});

		FScanRecord Record;
		Record.ScanId = ScanId;
		Record.ScopeDigest = ScopeDigest;
		Record.Findings = FindingObjects;
		Record.CreatedAt = FDateTime::UtcNow();
		TMap<FString, FScanRecord>& Records = GetScanRecords();
		if (Records.Num() >= MaxStoredScans && !Records.Contains(ScanId))
		{
			FString OldestId;
			FDateTime Oldest = FDateTime::MaxValue();
			for (const TPair<FString, FScanRecord>& Pair : Records)
			{
				if (Pair.Value.CreatedAt < Oldest)
				{
					Oldest = Pair.Value.CreatedAt;
					OldestId = Pair.Key;
				}
			}
			Records.Remove(OldestId);
		}
		Records.Add(ScanId, MoveTemp(Record));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint-scan.v1"));
		Result->SetStringField(TEXT("scanId"), ScanId);
		Result->SetStringField(TEXT("scopeDigest"), ScopeDigest);
		Result->SetNumberField(TEXT("matchedAssetTotal"), MatchedAssets);
		SetBoundedArray(Result, TEXT("assets"), AssetValues, MatchedAssets, AssetLimit);
		SetBoundedArray(
			Result,
			TEXT("findings"),
			FindingValues,
			FindingValues.Num(),
			FindingLimit);
		Result->SetStringField(
			TEXT("runtimeEvidenceStatus"),
			TEXT("notEvaluated"));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_BlueprintCallGraph final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.call_graph.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const int32 AssetLimit =
			ReadBoundedInteger(Params, TEXT("assetLimit"), DefaultAssetLimit, 1, MaxAssetLimit);
		const int32 NodeLimit =
			ReadBoundedInteger(
				Params,
				TEXT("nodeLimit"),
				DefaultGraphNodeLimit,
				1,
				MaxGraphNodeLimit);
		int32 MatchedAssets = 0;
		const TArray<FAssetData> Assets =
			ResolveBlueprintAssets(Params, MatchedAssets, AssetLimit);

		TMap<FString, TSharedPtr<FJsonObject>> NodesById;
		TArray<TSharedPtr<FJsonValue>> Edges;
		int32 TotalEdges = 0;

		for (const FAssetData& Asset : Assets)
		{
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			if (!Blueprint)
			{
				continue;
			}
			const FString SourceAsset = Asset.PackageName.ToString();
			TArray<UEdGraph*> Graphs;
			Blueprint->GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
					UK2Node_AddDelegate* Delegate = Cast<UK2Node_AddDelegate>(Node);
					if (!Call && !Delegate)
					{
						continue;
					}
					++TotalEdges;
					if (Edges.Num() >= NodeLimit)
					{
						continue;
					}

					const FString FunctionName = Call
						? Call->GetFunctionName().ToString()
						: Delegate->GetPropertyName().ToString();
					TSharedRef<FJsonObject> Source =
						DescribeCallEndpoint(SourceAsset, Graph->GetName(), Node, FunctionName);
					const FString SourceId = Source->GetStringField(TEXT("nodeId"));
					NodesById.FindOrAdd(SourceId) = Source;

					const UFunction* TargetFunction =
						Call ? Call->GetTargetFunction() : nullptr;
					const FProperty* TargetDelegate =
						Delegate ? Delegate->GetProperty() : nullptr;
					const UClass* OwnerClass = TargetFunction
						? TargetFunction->GetOwnerClass()
						: (TargetDelegate ? TargetDelegate->GetOwnerClass() : nullptr);
					const FString TargetAsset = BlueprintPackageForClass(OwnerClass);
					const FString TargetMember = TargetFunction
						? TargetFunction->GetName()
						: FunctionName;
					const FString TargetId = MakeStableId(
						Delegate ? TEXT("bpdelegate") : TEXT("bpfunction"),
						{
							TargetAsset,
							TargetMember,
						});
					TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
					Target->SetStringField(TEXT("nodeId"), TargetId);
					Target->SetStringField(TEXT("assetPath"), TargetAsset);
					Target->SetStringField(TEXT("function"), TargetMember);
					Target->SetStringField(
						TEXT("kind"),
						Delegate
							? (OwnerClass && OwnerClass->ClassGeneratedBy
								? TEXT("blueprintDelegate")
								: TEXT("nativeDelegate"))
							: (OwnerClass && OwnerClass->ClassGeneratedBy
								? TEXT("blueprintFunction")
								: TEXT("nativeFunction")));
					NodesById.FindOrAdd(TargetId) = Target;

					TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
					Edge->SetStringField(
						TEXT("edgeId"),
						MakeStableId(TEXT("bpedge"), {SourceId, TargetId}));
					Edge->SetStringField(TEXT("from"), SourceId);
					Edge->SetStringField(TEXT("to"), TargetId);
					Edge->SetStringField(
						TEXT("kind"),
						Delegate
							? TEXT("delegateBind")
							: (Call->bIsInterfaceCall
								? TEXT("interfaceCall")
								: TEXT("functionCall")));
					Edge->SetBoolField(
						TEXT("resolved"),
						TargetFunction != nullptr || TargetDelegate != nullptr);
					Edges.Add(MakeShared<FJsonValueObject>(Edge));
				}
			}
		}

		TArray<FString> NodeIds;
		NodesById.GetKeys(NodeIds);
		NodeIds.Sort();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const FString& Id : NodeIds)
		{
			Nodes.Add(MakeShared<FJsonValueObject>(NodesById.FindChecked(Id)));
		}

		Edges.Sort(
			[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
			{
				return Left->AsObject()->GetStringField(TEXT("edgeId"))
					< Right->AsObject()->GetStringField(TEXT("edgeId"));
			});
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint-call-graph.v1"));
		Result->SetNumberField(TEXT("matchedAssetTotal"), MatchedAssets);
		SetBoundedArray(Result, TEXT("nodes"), Nodes, Nodes.Num(), NodeLimit);
		SetBoundedArray(Result, TEXT("edges"), Edges, TotalEdges, NodeLimit);
		Result->SetStringField(
			TEXT("graphDigest"),
			DigestJson(Result));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_BlueprintFindingsCorrelate final : public FMCPToolBase
{
public:
	explicit FTool_BlueprintFindingsCorrelate(
		UEAIIntegration::Infrastructure::FBlueprintDebugService*
			InDebugService = nullptr)
		: DebugService(InDebugService)
	{
	}

	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.findings.correlate");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ScanId;
		FString RunId;
		if (!Params->TryGetStringField(TEXT("scanId"), ScanId) || ScanId.IsEmpty()
			|| !Params->TryGetStringField(TEXT("runId"), RunId) || RunId.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("scanId and runId are required."),
				TEXT("invalid_request"),
				400);
		}

		const FScanRecord* Record = GetScanRecords().Find(ScanId);
		if (!Record)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Unknown or expired scanId '%s'."), *ScanId),
				TEXT("scan_not_found"),
				404);
		}

		TSet<FString> ObservedNodeIds;
		bool bHasManualEvidence = false;
		const TArray<TSharedPtr<FJsonValue>>* ObservedValues = nullptr;
		if (Params->TryGetArrayField(TEXT("observedNodeIds"), ObservedValues)
			&& ObservedValues)
		{
			bHasManualEvidence = !ObservedValues->IsEmpty();
			for (const TSharedPtr<FJsonValue>& Value : *ObservedValues)
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					ObservedNodeIds.Add(Value->AsString());
				}
			}
		}

		FString DebugSessionId;
		bool bHasDebugTrace = false;
		FString CursorStart;
		FString CursorEnd;
		if (Params->TryGetStringField(TEXT("debugSessionId"), DebugSessionId)
			&& !DebugSessionId.IsEmpty())
		{
			if (!DebugService)
			{
				return FMCPToolResult::Error(
					TEXT("The Blueprint runtime debug provider is unavailable."),
					TEXT("debug_provider_unavailable"),
					503);
			}
			const TSharedPtr<FJsonObject>* TraceRange = nullptr;
			if (Params->TryGetObjectField(TEXT("traceRange"), TraceRange)
				&& TraceRange
				&& TraceRange->IsValid())
			{
				(*TraceRange)->TryGetStringField(TEXT("cursorStart"), CursorStart);
				(*TraceRange)->TryGetStringField(TEXT("cursorEnd"), CursorEnd);
			}
			const UEAIIntegration::Infrastructure::FBlueprintDebugResult
				TraceResult = DebugService->CollectObservedNodeIds(
					DebugSessionId,
					CursorStart,
					CursorEnd,
					ObservedNodeIds);
			if (!TraceResult.bSuccess)
			{
				return FMCPToolResult::Error(
					TraceResult.ErrorMessage,
					TraceResult.ErrorCode,
					TraceResult.HttpStatus);
			}
			bHasDebugTrace = true;
		}

		const int32 Limit =
			ReadBoundedInteger(Params, TEXT("limit"), DefaultFindingLimit, 1, MaxFindingLimit);
		TArray<TSharedPtr<FJsonValue>> Correlated;
		int32 ObservedCount = 0;
		for (const TSharedPtr<FJsonObject>& Original : Record->Findings)
		{
			TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>(*Original);
			const TSharedPtr<FJsonObject>* EvidencePtr = nullptr;
			FString NodeId;
			if (Finding->TryGetObjectField(TEXT("evidence"), EvidencePtr)
				&& EvidencePtr && EvidencePtr->IsValid())
			{
				(*EvidencePtr)->TryGetStringField(TEXT("nodeId"), NodeId);
			}

			const bool bObserved = !NodeId.IsEmpty() && ObservedNodeIds.Contains(NodeId);
			if (bObserved)
			{
				++ObservedCount;
				Finding->SetStringField(TEXT("runtimeStatus"), TEXT("corroborated"));
			}
			else
			{
				Finding->SetStringField(TEXT("runtimeStatus"), TEXT("notObserved"));
			}
			TSharedRef<FJsonObject> RuntimeEvidence = MakeShared<FJsonObject>();
			RuntimeEvidence->SetStringField(TEXT("runId"), RunId);
			RuntimeEvidence->SetStringField(
				TEXT("source"),
				bHasDebugTrace && bHasManualEvidence
					? TEXT("kismetTrace+observedNodeIds")
					: (bHasDebugTrace
						? TEXT("kismetTrace")
						: (bHasManualEvidence
							? TEXT("observedNodeIds")
							: TEXT("runtimeProviderUnavailable"))));
			RuntimeEvidence->SetBoolField(TEXT("observed"), bObserved);
			if (bHasDebugTrace)
			{
				RuntimeEvidence->SetStringField(
					TEXT("debugSessionId"),
					DebugSessionId);
				TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
				if (!CursorStart.IsEmpty())
				{
					Range->SetStringField(TEXT("cursorStart"), CursorStart);
				}
				if (!CursorEnd.IsEmpty())
				{
					Range->SetStringField(TEXT("cursorEnd"), CursorEnd);
				}
				RuntimeEvidence->SetObjectField(TEXT("traceRange"), Range);
			}
			Finding->SetObjectField(TEXT("runtimeEvidence"), RuntimeEvidence);
			Correlated.Add(MakeShared<FJsonValueObject>(Finding));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.finding-correlation.v1"));
		Result->SetStringField(TEXT("scanId"), ScanId);
		Result->SetStringField(TEXT("runId"), RunId);
		Result->SetNumberField(TEXT("observedCount"), ObservedCount);
		Result->SetStringField(
			TEXT("status"),
			!bHasDebugTrace && !bHasManualEvidence
				? TEXT("unavailable")
				: TEXT("correlated"));
		SetBoundedArray(
			Result,
			TEXT("findings"),
			Correlated,
			Correlated.Num(),
			Limit);
		return FMCPToolResult::Ok(Result);
	}

private:
	UEAIIntegration::Infrastructure::FBlueprintDebugService* DebugService;
};
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintAnalysisTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FBlueprintDebugService* DebugService)
{
	Registry.Register(MakeShared<FTool_BlueprintScan>());
	Registry.Register(MakeShared<FTool_BlueprintCallGraph>());
	Registry.Register(
		MakeShared<FTool_BlueprintFindingsCorrelate>(DebugService));
}

void RegisterBlueprintAnalysisTools(FMCPToolRegistry& Registry)
{
	RegisterBlueprintAnalysisTools(Registry, nullptr);
}
}
