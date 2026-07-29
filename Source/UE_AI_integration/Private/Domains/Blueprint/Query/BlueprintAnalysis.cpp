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
#include "K2Node_FunctionEntry.h"
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
constexpr int32 MaxRawFindingLimit = 10000;
constexpr int32 DefaultGraphNodeLimit = 1000;
constexpr int32 MaxGraphNodeLimit = 5000;
constexpr int32 MaxStoredScans = 32;
constexpr const TCHAR* DefaultBlueprintPathPrefix = TEXT("/Game");
constexpr const TCHAR* DisconnectedOutputRule =
	TEXT("blueprint.exec.disconnected_output");

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

FString NormalizeDirectoryPath(FString Path)
{
	Path.TrimStartAndEndInline();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1);
	}
	return Path;
}

bool IsPackageInDirectory(
	const FString& PackagePath,
	const FString& DirectoryPath)
{
	const FString NormalizedPackage = NormalizeDirectoryPath(PackagePath);
	const FString NormalizedDirectory = NormalizeDirectoryPath(DirectoryPath);
	if (NormalizedDirectory.IsEmpty())
	{
		return true;
	}
	return NormalizedPackage.Equals(
			NormalizedDirectory,
			ESearchCase::IgnoreCase)
		|| NormalizedPackage.StartsWith(
			NormalizedDirectory + TEXT("/"),
			ESearchCase::IgnoreCase);
}

void ResolveScopeFields(
	const TSharedPtr<FJsonObject>& Params,
	FString& OutRequestedAsset,
	FString& OutPathPrefix,
	bool& OutDefaultedToGame)
{
	OutRequestedAsset.Reset();
	OutPathPrefix.Reset();
	OutDefaultedToGame = false;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("asset"), OutRequestedAsset);
		Params->TryGetStringField(TEXT("pathPrefix"), OutPathPrefix);
	}
	OutRequestedAsset.TrimStartAndEndInline();
	OutPathPrefix = NormalizeDirectoryPath(OutPathPrefix);

	// An empty request and a bare asset name are deliberately rooted in /Game.
	// A fully-qualified non-/Game asset remains an explicit, auditable request.
	if (OutPathPrefix.IsEmpty()
		&& (OutRequestedAsset.IsEmpty()
			|| !OutRequestedAsset.StartsWith(TEXT("/"))))
	{
		OutPathPrefix = DefaultBlueprintPathPrefix;
		OutDefaultedToGame = true;
	}
}

bool MatchesScopePaths(
	const FString& PackagePath,
	const FString& ObjectPath,
	const FString& AssetName,
	const FString& RequestedAsset,
	const FString& PathPrefix)
{
	if (!RequestedAsset.IsEmpty()
		&& !PackagePath.Equals(RequestedAsset, ESearchCase::IgnoreCase)
		&& !ObjectPath.Equals(RequestedAsset, ESearchCase::IgnoreCase)
		&& !AssetName.Equals(RequestedAsset, ESearchCase::IgnoreCase))
	{
		return false;
	}
	return IsPackageInDirectory(PackagePath, PathPrefix);
}

bool MatchesScope(
	const FAssetData& Asset,
	const FString& RequestedAsset,
	const FString& PathPrefix)
{
	return MatchesScopePaths(
		Asset.PackageName.ToString(),
		Asset.GetObjectPathString(),
		Asset.AssetName.ToString(),
		RequestedAsset,
		PathPrefix);
}

TArray<FAssetData> ResolveBlueprintAssets(
	const TSharedPtr<FJsonObject>& Params,
	int32& OutMatchedTotal,
	const int32 AssetLimit)
{
	FString RequestedAsset;
	FString PathPrefix;
	bool bDefaultedToGame = false;
	ResolveScopeFields(
		Params,
		RequestedAsset,
		PathPrefix,
		bDefaultedToGame);

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

bool IsTickEvent(const UEdGraphNode* Node)
{
	const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
	if (!Event)
	{
		return false;
	}
	const FString EventName = Event->EventReference.GetMemberName().ToString();
	return EventName.Equals(TEXT("ReceiveTick"), ESearchCase::IgnoreCase)
		|| EventName.Equals(TEXT("Tick"), ESearchCase::IgnoreCase);
}

FString GraphNameForNode(const UEdGraphNode* Node)
{
	const UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
	return Graph ? Graph->GetName() : FString();
}

FString NodeTraversalKey(const UEdGraphNode* Node)
{
	return FString::Printf(
		TEXT("%s|%s|%s|%s"),
		*GraphNameForNode(Node),
		*GuidString(Node),
		Node ? *Node->GetClass()->GetName() : TEXT(""),
		Node
			? *Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()
			: TEXT(""));
}

struct FExecutionPath
{
	TArray<const UEdGraphNode*> Nodes;
};

struct FTickReachability
{
	TMap<const UEdGraphNode*, FExecutionPath> Paths;
};

void SortNodes(TArray<const UEdGraphNode*>& Nodes)
{
	Nodes.Sort(
		[](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return NodeTraversalKey(&Left) < NodeTraversalKey(&Right);
		});
}

UEdGraph* FindLocalFunctionGraph(
	const UBlueprint* Blueprint,
	const UK2Node_CallFunction* Call)
{
	if (!Blueprint || !Call)
	{
		return nullptr;
	}
	const FName FunctionName = Call->GetFunctionName();
	if (FunctionName.IsNone())
	{
		return nullptr;
	}
	const UFunction* Target = Call->GetTargetFunction();
	if (Target)
	{
		const UClass* OwnerClass = Target->GetOwnerClass();
		if (!OwnerClass || OwnerClass->ClassGeneratedBy != Blueprint)
		{
			return nullptr;
		}
	}
	else if (!Call->FunctionReference.IsSelfContext())
	{
		return nullptr;
	}
	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (FunctionGraph && FunctionGraph->GetFName() == FunctionName)
		{
			return FunctionGraph;
		}
	}
	return nullptr;
}

FTickReachability BuildTickReachability(UBlueprint* Blueprint)
{
	FTickReachability Result;
	if (!Blueprint)
	{
		return Result;
	}

	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	Graphs.Sort(
		[](const UEdGraph& Left, const UEdGraph& Right)
		{
			return Left.GetName() < Right.GetName();
		});

	TArray<const UEdGraphNode*> Roots;
	for (const UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsTickEvent(Node))
			{
				Roots.Add(Node);
			}
		}
	}
	SortNodes(Roots);

	TArray<const UEdGraphNode*> Queue;
	auto Enqueue =
		[&Result, &Queue](
			const UEdGraphNode* Node,
			const FExecutionPath& Path)
		{
			if (!Node || Result.Paths.Contains(Node))
			{
				return;
			}
			Result.Paths.Add(Node, Path);
			Queue.Add(Node);
		};
	for (const UEdGraphNode* Root : Roots)
	{
		FExecutionPath RootPath;
		RootPath.Nodes.Add(Root);
		Enqueue(Root, RootPath);
	}

	for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
	{
		const UEdGraphNode* Current = Queue[QueueIndex];
		const FExecutionPath CurrentPath = Result.Paths.FindChecked(Current);
		TArray<const UEdGraphNode*> NextNodes;
		TSet<const UEdGraphNode*> UniqueNextNodes;
		for (const UEdGraphPin* Pin : Current->Pins)
		{
			if (!Pin
				|| Pin->Direction != EGPD_Output
				|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				const UEdGraphNode* Next =
					LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (Next && !UniqueNextNodes.Contains(Next))
				{
					UniqueNextNodes.Add(Next);
					NextNodes.Add(Next);
				}
			}
		}
		SortNodes(NextNodes);
		for (const UEdGraphNode* Next : NextNodes)
		{
			FExecutionPath NextPath = CurrentPath;
			NextPath.Nodes.Add(Next);
			Enqueue(Next, NextPath);
		}

		const UK2Node_CallFunction* Call =
			Cast<UK2Node_CallFunction>(Current);
		const UEdGraph* FunctionGraph =
			FindLocalFunctionGraph(Blueprint, Call);
		if (!FunctionGraph)
		{
			continue;
		}
		TArray<const UEdGraphNode*> Entries;
		for (const UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (Cast<UK2Node_FunctionEntry>(Node))
			{
				Entries.Add(Node);
			}
		}
		SortNodes(Entries);
		for (const UEdGraphNode* Entry : Entries)
		{
			FExecutionPath EntryPath = CurrentPath;
			EntryPath.Nodes.Add(Entry);
			Enqueue(Entry, EntryPath);
		}
	}
	return Result;
}

TSharedRef<FJsonObject> MakeNodeEvidence(
	const UEdGraphNode* Node,
	const FString& NodeId,
	const FString& AssetPath,
	const FString& Detail,
	const FExecutionPath* ExecutionPath = nullptr,
	const FString& PinName = FString())
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
	if (!PinName.IsEmpty())
	{
		Evidence->SetStringField(TEXT("pinName"), PinName);
	}
	Evidence->SetStringField(
		TEXT("reachabilityModel"),
		TEXT("tickExecAndLocalFunctionCalls"));
	Evidence->SetBoolField(TEXT("tickReachable"), ExecutionPath != nullptr);
	if (ExecutionPath)
	{
		TArray<TSharedPtr<FJsonValue>> PathValues;
		PathValues.Reserve(ExecutionPath->Nodes.Num());
		for (const UEdGraphNode* PathNode : ExecutionPath->Nodes)
		{
			const FString PathGraph = GraphNameForNode(PathNode);
			TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
			Step->SetStringField(TEXT("assetPath"), AssetPath);
			Step->SetStringField(TEXT("graph"), PathGraph);
			Step->SetStringField(TEXT("nodeGuid"), GuidString(PathNode));
			Step->SetStringField(
				TEXT("nodeId"),
				NodeStableId(AssetPath, PathGraph, PathNode));
			PathValues.Add(MakeShared<FJsonValueObject>(Step));
		}
		Evidence->SetArrayField(TEXT("executionPath"), PathValues);
		Evidence->SetNumberField(
			TEXT("executionPathLength"),
			PathValues.Num());
	}
	return Evidence;
}

TSharedPtr<FJsonObject> AppendFinding(
	TArray<TSharedPtr<FJsonObject>>& Findings,
	const FString& RuleId,
	const FString& Severity,
	const double Confidence,
	const FString& AssetPath,
	const FString& GraphName,
	const UEdGraphNode* Node,
	const FString& Message,
	const FString& Detail = FString(),
	const FExecutionPath* ExecutionPath = nullptr,
	const FString& PinName = FString())
{
	// Keep analysis memory bounded even if a broad scope contains pathological
	// graphs. One extra entry lets response projection report truncation.
	if (Findings.Num() >= MaxRawFindingLimit + 1)
	{
		return nullptr;
	}
	const FString NodeId = NodeStableId(AssetPath, GraphName, Node);
	TSharedRef<FJsonObject> Finding = MakeFinding(
		RuleId,
		Severity,
		Confidence,
		AssetPath,
		GraphName,
		GuidString(Node),
		Message,
		MakeNodeEvidence(
			Node,
			NodeId,
			AssetPath,
			Detail,
			ExecutionPath,
			PinName));
	Findings.Add(Finding);
	return Finding;
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

TArray<const UEdGraphPin*> GetDisconnectedExecOutputs(
	const UEdGraphNode* Node)
{
	TArray<const UEdGraphPin*> Result;
	if (!Node)
	{
		return Result;
	}
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin
			&& Pin->Direction == EGPD_Output
			&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
			&& Pin->LinkedTo.IsEmpty()
			&& !Pin->bHidden)
		{
			Result.Add(Pin);
		}
	}
	Result.Sort(
		[](const UEdGraphPin& Left, const UEdGraphPin& Right)
		{
			const FString LeftKey =
				Left.PinName.ToString() + TEXT("|")
				+ Left.PinId.ToString(EGuidFormats::Digits);
			const FString RightKey =
				Right.PinName.ToString() + TEXT("|")
				+ Right.PinId.ToString(EGuidFormats::Digits);
			return LeftKey < RightKey;
		});
	return Result;
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
	const FTickReachability TickReachability =
		BuildTickReachability(Blueprint);

	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}

		const FString GraphName = Graph->GetName();
		TArray<const UEdGraphNode*> TickNodes;
		for (const UEdGraphNode* Candidate : Graph->Nodes)
		{
			if (IsTickEvent(Candidate))
			{
				TickNodes.Add(Candidate);
			}
		}
		SortNodes(TickNodes);
		for (const UEdGraphNode* TickNode : TickNodes)
		{
			AppendFinding(
				Findings,
				TEXT("blueprint.tick.present"),
				TEXT("info"),
				1.0,
				AssetPath,
				GraphName,
				TickNode,
				TEXT("Blueprint contains an Event Tick entry point."),
				FString(),
				TickReachability.Paths.Find(TickNode));
		}

		TMap<FString, TArray<const UEdGraphNode*>> DelegateBindings;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const FExecutionPath* ExecutionPath =
				TickReachability.Paths.Find(Node);
			const bool bTickReachable = ExecutionPath != nullptr;

			if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				const FString FunctionName = Call->GetFunctionName().ToString();
				if (IsGlobalTraversal(FunctionName))
				{
					AppendFinding(
						Findings,
						TEXT("blueprint.call.global_traversal"),
						bTickReachable ? TEXT("error") : TEXT("warning"),
						0.98,
						AssetPath,
						GraphName,
						Node,
						FString::Printf(
							TEXT("Global traversal call '%s' can scale with world size."),
							*FunctionName),
						bTickReachable
							? TEXT("Call is reachable from Event Tick through execution pins.")
							: TEXT("No Event Tick execution path reaches this call."),
						ExecutionPath);
				}
				if (IsSynchronousLoad(FunctionName))
				{
					AppendFinding(
						Findings,
						TEXT("blueprint.call.synchronous_load"),
						bTickReachable ? TEXT("error") : TEXT("warning"),
						0.9,
						AssetPath,
						GraphName,
						Node,
						FString::Printf(
							TEXT("Synchronous load call '%s' can stall the game thread."),
							*FunctionName),
						bTickReachable
							? TEXT("Call is reachable from Event Tick through execution pins.")
							: TEXT("No Event Tick execution path reaches this call."),
						ExecutionPath);
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
					bTickReachable ? TEXT("warning") : TEXT("info"),
					bTickReachable ? 0.9 : 0.65,
					AssetPath,
					GraphName,
					Node,
					bTickReachable
						? TEXT("Dynamic cast is reachable from Event Tick.")
						: TEXT("Dynamic cast should be reviewed for coupling and call frequency."),
					FString(),
					ExecutionPath);
			}

			if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node);
				IsLoopMacro(Macro))
			{
				AppendFinding(
					Findings,
					TEXT("blueprint.loop.review"),
					bTickReachable ? TEXT("warning") : TEXT("info"),
					0.7,
					AssetPath,
					GraphName,
					Node,
					TEXT("Loop bounds are data-dependent and should be reviewed."),
					FString(),
					ExecutionPath);
			}

			if (Cast<UK2Node_AddDelegate>(Node))
			{
				DelegateBindings.FindOrAdd(
					Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()).Add(Node);
			}

			if (!Cast<UK2Node_Event>(Node))
			{
				for (const UEdGraphPin* Pin :
					GetDisconnectedExecOutputs(Node))
				{
					const FString PinName = Pin->PinName.ToString();
					AppendFinding(
						Findings,
						DisconnectedOutputRule,
						TEXT("info"),
						0.8,
						AssetPath,
						GraphName,
						Node,
						FString::Printf(
							TEXT("Execution output '%s' is not connected."),
							*PinName),
						FString(),
						ExecutionPath,
						PinName);
				}
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
			if (Findings.Num() >= MaxRawFindingLimit + 1)
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

void ReadStringFilter(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	TSet<FString>& OutValues,
	const bool bLowercase)
{
	if (!Params.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonValue>* Value = Params->Values.Find(Field);
	if (!Value || !Value->IsValid())
	{
		return;
	}
	auto AddValue =
		[&OutValues, bLowercase](FString Text)
		{
			Text.TrimStartAndEndInline();
			if (bLowercase)
			{
				Text.ToLowerInline();
			}
			if (!Text.IsEmpty())
			{
				OutValues.Add(Text);
			}
		};
	if ((*Value)->Type == EJson::String)
	{
		AddValue((*Value)->AsString());
		return;
	}
	if ((*Value)->Type != EJson::Array)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Item : (*Value)->AsArray())
	{
		if (Item.IsValid() && Item->Type == EJson::String)
		{
			AddValue(Item->AsString());
		}
	}
}

FString FindingAssetPath(const TSharedPtr<FJsonObject>& Finding)
{
	const TSharedPtr<FJsonObject>* Location = nullptr;
	FString AssetPath;
	if (Finding.IsValid()
		&& Finding->TryGetObjectField(TEXT("location"), Location)
		&& Location
		&& Location->IsValid())
	{
		(*Location)->TryGetStringField(TEXT("assetPath"), AssetPath);
	}
	return AssetPath;
}

FString FindingGraph(const TSharedPtr<FJsonObject>& Finding)
{
	const TSharedPtr<FJsonObject>* Location = nullptr;
	FString Graph;
	if (Finding.IsValid()
		&& Finding->TryGetObjectField(TEXT("location"), Location)
		&& Location
		&& Location->IsValid())
	{
		(*Location)->TryGetStringField(TEXT("graph"), Graph);
	}
	return Graph;
}

FString FindingPinName(const TSharedPtr<FJsonObject>& Finding)
{
	const TSharedPtr<FJsonObject>* Evidence = nullptr;
	FString PinName;
	if (Finding.IsValid()
		&& Finding->TryGetObjectField(TEXT("evidence"), Evidence)
		&& Evidence
		&& Evidence->IsValid())
	{
		(*Evidence)->TryGetStringField(TEXT("pinName"), PinName);
	}
	return PinName;
}

int32 SeverityRank(const FString& Severity)
{
	if (Severity == TEXT("critical"))
	{
		return 0;
	}
	if (Severity == TEXT("high"))
	{
		return 1;
	}
	if (Severity == TEXT("medium"))
	{
		return 2;
	}
	if (Severity == TEXT("low"))
	{
		return 3;
	}
	return 4;
}

bool FindingLess(
	const TSharedPtr<FJsonObject>& Left,
	const TSharedPtr<FJsonObject>& Right)
{
	const FString LeftSeverity =
		Left->GetStringField(TEXT("severity")).ToLower();
	const FString RightSeverity =
		Right->GetStringField(TEXT("severity")).ToLower();
	const int32 LeftRank = SeverityRank(LeftSeverity);
	const int32 RightRank = SeverityRank(RightSeverity);
	if (LeftRank != RightRank)
	{
		return LeftRank < RightRank;
	}
	const FString LeftKey = FString::Printf(
		TEXT("%s|%s|%s|%s|%s"),
		*Left->GetStringField(TEXT("ruleId")),
		*FindingAssetPath(Left),
		*FindingGraph(Left),
		*FindingPinName(Left),
		*Left->GetStringField(TEXT("findingId")));
	const FString RightKey = FString::Printf(
		TEXT("%s|%s|%s|%s|%s"),
		*Right->GetStringField(TEXT("ruleId")),
		*FindingAssetPath(Right),
		*FindingGraph(Right),
		*FindingPinName(Right),
		*Right->GetStringField(TEXT("findingId")));
	return LeftKey < RightKey;
}

struct FFindingSuppression
{
	TSet<FString> FindingIds;
	TSet<FString> RuleIds;
	TSet<FString> AssetPaths;
	TSet<FString> AssetPathPrefixes;
};

void ReadSuppression(
	const TSharedPtr<FJsonObject>& Params,
	FFindingSuppression& OutSuppression)
{
	const TSharedPtr<FJsonObject>* Suppression = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetObjectField(TEXT("suppression"), Suppression)
		|| !Suppression
		|| !Suppression->IsValid())
	{
		return;
	}
	ReadStringFilter(
		*Suppression,
		TEXT("findingIds"),
		OutSuppression.FindingIds,
		false);
	ReadStringFilter(
		*Suppression,
		TEXT("ruleIds"),
		OutSuppression.RuleIds,
		true);
	ReadStringFilter(
		*Suppression,
		TEXT("assetPaths"),
		OutSuppression.AssetPaths,
		false);
	ReadStringFilter(
		*Suppression,
		TEXT("assetPathPrefixes"),
		OutSuppression.AssetPathPrefixes,
		false);
}

bool IsSuppressed(
	const TSharedPtr<FJsonObject>& Finding,
	const FFindingSuppression& Suppression)
{
	const FString FindingId =
		Finding->GetStringField(TEXT("findingId"));
	const FString RuleId =
		Finding->GetStringField(TEXT("ruleId")).ToLower();
	const FString AssetPath = FindingAssetPath(Finding);
	if (Suppression.FindingIds.Contains(FindingId)
		|| Suppression.RuleIds.Contains(RuleId))
	{
		return true;
	}
	for (const FString& SuppressedAsset : Suppression.AssetPaths)
	{
		if (AssetPath.Equals(SuppressedAsset, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	for (const FString& Prefix : Suppression.AssetPathPrefixes)
	{
		if (IsPackageInDirectory(AssetPath, Prefix))
		{
			return true;
		}
	}
	return false;
}

struct FFindingBaseline
{
	TSet<FString> FindingIds;
	bool bOnlyKnown = false;
	bool bEnabled = false;
};

void ReadBaseline(
	const TSharedPtr<FJsonObject>& Params,
	FFindingBaseline& OutBaseline)
{
	const TSharedPtr<FJsonObject>* Baseline = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetObjectField(TEXT("baseline"), Baseline)
		|| !Baseline
		|| !Baseline->IsValid())
	{
		return;
	}
	OutBaseline.bEnabled = true;
	ReadStringFilter(
		*Baseline,
		TEXT("findingIds"),
		OutBaseline.FindingIds,
		false);
	FString Mode;
	(*Baseline)->TryGetStringField(TEXT("mode"), Mode);
	OutBaseline.bOnlyKnown =
		Mode.Equals(TEXT("onlyKnown"), ESearchCase::IgnoreCase);
}

bool PassesBaseline(
	const TSharedPtr<FJsonObject>& Finding,
	const FFindingBaseline& Baseline)
{
	if (!Baseline.bEnabled)
	{
		return true;
	}
	const bool bKnown = Baseline.FindingIds.Contains(
		Finding->GetStringField(TEXT("findingId")));
	return Baseline.bOnlyKnown ? bKnown : !bKnown;
}

struct FFindingProjection
{
	TArray<TSharedPtr<FJsonValue>> Page;
	TArray<TSharedPtr<FJsonValue>> Summaries;
	TArray<TSharedPtr<FJsonObject>> EligibleFindings;
	int32 Raw = 0;
	int32 Filtered = 0;
	int32 Suppressed = 0;
	int32 BaselineExcluded = 0;
	int32 Summarized = 0;
	int32 Eligible = 0;
	int32 Returned = 0;
	int32 Offset = 0;
	int32 Limit = DefaultFindingLimit;
	bool bRawTruncated = false;
};

FFindingProjection ProjectFindings(
	const TArray<TSharedPtr<FJsonObject>>& RawFindings,
	const TSharedPtr<FJsonObject>& Params)
{
	FFindingProjection Projection;
	Projection.Raw = RawFindings.Num();
	Projection.bRawTruncated = RawFindings.Num() > MaxRawFindingLimit;
	Projection.Offset = ReadBoundedInteger(
		Params,
		TEXT("findingOffset"),
		0,
		0,
		1000000);
	Projection.Limit = ReadBoundedInteger(
		Params,
		TEXT("findingLimit"),
		DefaultFindingLimit,
		1,
		MaxFindingLimit);

	TSet<FString> SeverityFilter;
	TSet<FString> RuleFilter;
	TSet<FString> RuntimeStatusFilter;
	ReadStringFilter(Params, TEXT("severity"), SeverityFilter, true);
	ReadStringFilter(Params, TEXT("rule"), RuleFilter, true);
	ReadStringFilter(
		Params,
		TEXT("runtimeStatus"),
		RuntimeStatusFilter,
		true);
	const bool bExpandDisconnected =
		RuleFilter.Contains(DisconnectedOutputRule);

	FFindingSuppression Suppression;
	ReadSuppression(Params, Suppression);
	FFindingBaseline Baseline;
	ReadBaseline(Params, Baseline);

	TArray<TSharedPtr<FJsonObject>> Eligible;
	int32 DisconnectedSummaryCount = 0;
	for (const TSharedPtr<FJsonObject>& Finding : RawFindings)
	{
		const FString Severity =
			Finding->GetStringField(TEXT("severity")).ToLower();
		const FString Rule =
			Finding->GetStringField(TEXT("ruleId")).ToLower();
		const FString RuntimeStatus =
			Finding->GetStringField(TEXT("runtimeStatus")).ToLower();
		if ((!SeverityFilter.IsEmpty()
				&& !SeverityFilter.Contains(Severity))
			|| (!RuleFilter.IsEmpty() && !RuleFilter.Contains(Rule))
			|| (!RuntimeStatusFilter.IsEmpty()
				&& !RuntimeStatusFilter.Contains(RuntimeStatus)))
		{
			continue;
		}
		++Projection.Filtered;
		if (IsSuppressed(Finding, Suppression))
		{
			++Projection.Suppressed;
			continue;
		}
		if (!PassesBaseline(Finding, Baseline))
		{
			++Projection.BaselineExcluded;
			continue;
		}
		if (!bExpandDisconnected && Rule == DisconnectedOutputRule)
		{
			++Projection.Summarized;
			++DisconnectedSummaryCount;
			continue;
		}
		Eligible.Add(Finding);
	}
	Eligible.Sort(FindingLess);
	Projection.Eligible = Eligible.Num();
	Projection.EligibleFindings = Eligible;

	if (DisconnectedSummaryCount > 0)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("ruleId"), DisconnectedOutputRule);
		Summary->SetStringField(TEXT("severity"), TEXT("info"));
		Summary->SetNumberField(TEXT("count"), DisconnectedSummaryCount);
		Summary->SetBoolField(TEXT("expanded"), false);
		Summary->SetStringField(
			TEXT("message"),
			TEXT("Disconnected execution outputs are summarized by default; request this rule explicitly to expand them."));
		Projection.Summaries.Add(MakeShared<FJsonValueObject>(Summary));
	}

	const int32 End = FMath::Min(
		Eligible.Num(),
		Projection.Offset + Projection.Limit);
	for (int32 Index = Projection.Offset; Index < End; ++Index)
	{
		Projection.Page.Add(
			MakeShared<FJsonValueObject>(Eligible[Index]));
	}
	Projection.Returned = Projection.Page.Num();
	return Projection;
}

bool RuntimeEvidenceIsComplete(
	const TSharedPtr<FJsonObject>& Params,
	const bool bHasDebugTrace)
{
	FString Coverage;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("evidenceCoverage"), Coverage)
		|| !Coverage.Equals(TEXT("complete"), ESearchCase::IgnoreCase)
		|| !bHasDebugTrace)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Range = nullptr;
	if (!Params->TryGetObjectField(TEXT("traceRange"), Range)
		|| !Range
		|| !Range->IsValid())
	{
		return false;
	}
	FString Start;
	FString End;
	return (*Range)->TryGetStringField(TEXT("cursorStart"), Start)
		&& !Start.IsEmpty()
		&& (*Range)->TryGetStringField(TEXT("cursorEnd"), End)
		&& !End.IsEmpty();
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

		FindingObjects.Sort(FindingLess);
		const FFindingProjection Projection =
			ProjectFindings(FindingObjects, Params);

		TSharedRef<FJsonObject> Scope = MakeShared<FJsonObject>();
		FString RequestedAsset;
		FString PathPrefix;
		bool bDefaultedToGame = false;
		ResolveScopeFields(
			Params,
			RequestedAsset,
			PathPrefix,
			bDefaultedToGame);
		Scope->SetStringField(TEXT("asset"), RequestedAsset);
		Scope->SetStringField(TEXT("pathPrefix"), PathPrefix);
		Scope->SetBoolField(
			TEXT("defaultedToGame"),
			bDefaultedToGame);
		Scope->SetArrayField(TEXT("resolvedAssets"), AssetValues);
		const FString ScopeDigest = DigestJson(Scope);

		TSharedRef<FJsonObject> ScanIdentity = MakeShared<FJsonObject>();
		ScanIdentity->SetStringField(TEXT("scopeDigest"), ScopeDigest);
		TArray<TSharedPtr<FJsonValue>> EligibleFindingValues;
		EligibleFindingValues.Reserve(
			Projection.EligibleFindings.Num());
		for (const TSharedPtr<FJsonObject>& Finding :
			Projection.EligibleFindings)
		{
			EligibleFindingValues.Add(
				MakeShared<FJsonValueObject>(Finding));
		}
		ScanIdentity->SetArrayField(
			TEXT("eligibleFindings"),
			EligibleFindingValues);
		ScanIdentity->SetArrayField(
			TEXT("findingSummaries"),
			Projection.Summaries);
		const FString ScanId =
			MakeStableId(TEXT("bpscan"), {ScopeDigest, DigestJson(ScanIdentity)});

		FScanRecord Record;
		Record.ScanId = ScanId;
		Record.ScopeDigest = ScopeDigest;
		Record.Findings = Projection.EligibleFindings;
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
		Result->SetObjectField(TEXT("scope"), Scope);
		Result->SetNumberField(TEXT("matchedAssetTotal"), MatchedAssets);
		SetBoundedArray(Result, TEXT("assets"), AssetValues, MatchedAssets, AssetLimit);
		Result->SetArrayField(TEXT("findings"), Projection.Page);
		Result->SetNumberField(TEXT("findingsCount"), Projection.Returned);
		Result->SetNumberField(TEXT("findingsTotal"), Projection.Eligible);
		Result->SetNumberField(TEXT("findingsOffset"), Projection.Offset);
		Result->SetNumberField(TEXT("findingsLimit"), Projection.Limit);
		Result->SetBoolField(
			TEXT("findingsTruncated"),
			Projection.Offset > 0
				|| Projection.Offset + Projection.Returned
					< Projection.Eligible);
		Result->SetBoolField(
			TEXT("findingsHasMore"),
			Projection.Offset + Projection.Returned
				< Projection.Eligible);
		Result->SetArrayField(
			TEXT("findingSummaries"),
			Projection.Summaries);
		TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetNumberField(TEXT("raw"), Projection.Raw);
		Stats->SetNumberField(TEXT("filtered"), Projection.Filtered);
		Stats->SetNumberField(TEXT("suppressed"), Projection.Suppressed);
		Stats->SetNumberField(
			TEXT("baselineExcluded"),
			Projection.BaselineExcluded);
		Stats->SetNumberField(TEXT("summarized"), Projection.Summarized);
		Stats->SetNumberField(TEXT("eligible"), Projection.Eligible);
		Stats->SetNumberField(TEXT("returned"), Projection.Returned);
		Stats->SetNumberField(TEXT("offset"), Projection.Offset);
		Stats->SetNumberField(TEXT("limit"), Projection.Limit);
		Stats->SetBoolField(
			TEXT("exact"),
			!Projection.bRawTruncated);
		Result->SetObjectField(TEXT("findingStats"), Stats);
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
		const bool bEvidenceComplete =
			RuntimeEvidenceIsComplete(Params, bHasDebugTrace);
		TArray<TSharedPtr<FJsonValue>> Correlated;
		int32 ObservedCount = 0;
		int32 NotObservedCount = 0;
		int32 HypothesisCount = 0;
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
			else if (bEvidenceComplete)
			{
				Finding->SetStringField(TEXT("runtimeStatus"), TEXT("notObserved"));
				++NotObservedCount;
			}
			else
			{
				// Absence from a partial trace is not negative evidence.
				Finding->SetStringField(TEXT("runtimeStatus"), TEXT("hypothesis"));
				++HypothesisCount;
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
			RuntimeEvidence->SetStringField(
				TEXT("coverage"),
				bEvidenceComplete ? TEXT("complete") : TEXT("partial"));
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
		Result->SetNumberField(TEXT("notObservedCount"), NotObservedCount);
		Result->SetNumberField(TEXT("hypothesisCount"), HypothesisCount);
		Result->SetStringField(
			TEXT("evidenceCoverage"),
			bEvidenceComplete ? TEXT("complete") : TEXT("partial"));
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

#if WITH_DEV_AUTOMATION_TESTS
namespace UEAIIntegrationTools::BlueprintAnalysisTesting
{
bool MatchesScopePathsForTesting(
	const FString& PackagePath,
	const FString& ObjectPath,
	const FString& AssetName,
	const FString& RequestedAsset,
	const FString& PathPrefix)
{
	return ::MatchesScopePaths(
		PackagePath,
		ObjectPath,
		AssetName,
		RequestedAsset,
		PathPrefix);
}

TArray<TSharedPtr<FJsonObject>> ScanBlueprintForTesting(
	UBlueprint* Blueprint,
	const FString& AssetPath)
{
	TArray<TSharedPtr<FJsonObject>> Findings;
	::ScanBlueprint(Blueprint, AssetPath, Findings);
	Findings.Sort(FindingLess);
	return Findings;
}

TSharedRef<FJsonObject> ProjectFindingsForTesting(
	const TArray<TSharedPtr<FJsonObject>>& Findings,
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonObject>> Sorted = Findings;
	Sorted.Sort(FindingLess);
	const FFindingProjection Projection =
		::ProjectFindings(Sorted, Params);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("findings"), Projection.Page);
	Result->SetArrayField(TEXT("findingSummaries"), Projection.Summaries);
	TSharedRef<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(TEXT("raw"), Projection.Raw);
	Stats->SetNumberField(TEXT("filtered"), Projection.Filtered);
	Stats->SetNumberField(TEXT("suppressed"), Projection.Suppressed);
	Stats->SetNumberField(
		TEXT("baselineExcluded"),
		Projection.BaselineExcluded);
	Stats->SetNumberField(TEXT("summarized"), Projection.Summarized);
	Stats->SetNumberField(TEXT("eligible"), Projection.Eligible);
	Stats->SetNumberField(TEXT("returned"), Projection.Returned);
	Stats->SetNumberField(TEXT("offset"), Projection.Offset);
	Stats->SetNumberField(TEXT("limit"), Projection.Limit);
	Result->SetObjectField(TEXT("findingStats"), Stats);
	return Result;
}

FString RuntimeStatusForTesting(
	const bool bObserved,
	const bool bHasDebugTrace,
	const bool bCompleteCoverage,
	const bool bBoundedRange)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("evidenceCoverage"),
		bCompleteCoverage ? TEXT("complete") : TEXT("partial"));
	if (bBoundedRange)
	{
		TSharedRef<FJsonObject> Range = MakeShared<FJsonObject>();
		Range->SetStringField(TEXT("cursorStart"), TEXT("1"));
		Range->SetStringField(TEXT("cursorEnd"), TEXT("2"));
		Params->SetObjectField(TEXT("traceRange"), Range);
	}
	if (bObserved)
	{
		return TEXT("corroborated");
	}
	return RuntimeEvidenceIsComplete(Params, bHasDebugTrace)
		? TEXT("notObserved")
		: TEXT("hypothesis");
}
}
#endif

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
