// Blueprint Read Tools — list, get, search, describe blueprints (read-only)
#include "Containers/StringConv.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Domains/Blueprint/BlueprintGraphEditorSupport.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Infrastructure/Sha256.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// The Asset Registry can expose package dependencies without exposing an
	// object-level node for an inherited Blueprint class.  When the exact query
	// has no registry node, inspect the loaded Blueprint references as a bounded
	// read-only fallback.  This keeps exact object evidence separate from the
	// package fallback and covers native/inherited Blueprint references that are
	// serialized through GeneratedClass rather than a searchable object field.
	static bool CollectLoadedBlueprintObjectReferencers(
		const FString& AssetPath,
		const TArray<FAssetData>& BlueprintAssets,
		TSet<FString>& OutReferencerIdentities)
	{
		UBlueprint* TargetBlueprint = Cast<UBlueprint>(
			StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath));
		if (!TargetBlueprint)
		{
			return false;
		}

		UClass* TargetGeneratedClass = TargetBlueprint->GeneratedClass;
		if (!TargetGeneratedClass)
		{
			return false;
		}
		for (const FAssetData& Candidate : BlueprintAssets)
		{
			// Keep the fallback read-only and bounded.  The package-level query has
			// already provided the candidate set; only inspect Blueprint objects that
			// are resident in this Editor instead of loading every project asset.
			UBlueprint* CandidateBlueprint = Cast<UBlueprint>(FindObject<UBlueprint>(
				nullptr,
				*Candidate.GetSoftObjectPath().ToString()));
			if (!CandidateBlueprint || CandidateBlueprint == TargetBlueprint)
			{
				continue;
			}

			bool bReferencesTarget =
				CandidateBlueprint->ParentClass == TargetGeneratedClass;
			if (!bReferencesTarget)
			{
				TArray<UObject*> ReferencedObjects;
				FReferenceFinder Finder(
					ReferencedObjects,
					nullptr,
					false,
					true,
					true,
					true);
				Finder.FindReferences(CandidateBlueprint);
				bReferencesTarget = ReferencedObjects.Contains(TargetBlueprint)
					|| ReferencedObjects.Contains(TargetGeneratedClass);
			}
			if (bReferencesTarget)
			{
				OutReferencerIdentities.Add(
					FAssetIdentifier(
						Candidate.PackageName,
						Candidate.AssetName).ToString());
			}
		}
		return OutReferencerIdentities.Num() > 0;
	}

	bool ReadBlueprintListPageInteger(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		int32 DefaultValue,
		int32 Minimum,
		int32 Maximum,
		int32& OutValue)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Field))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(Field, Number)
			|| !FMath::IsFinite(Number)
			|| Number < Minimum || Number > Maximum
			|| Number != FMath::FloorToDouble(Number))
		{
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	struct FBlueprintListEntry
	{
		FString Name;
		FString Path;
		FString ParentClass;
		bool bIsLevelBlueprint = false;
	};

	struct FBlueprintExecutionEdge
	{
		UEdGraphNode* FromNode = nullptr;
		UEdGraphPin* FromPin = nullptr;
		UEdGraphNode* ToNode = nullptr;
		UEdGraphPin* ToPin = nullptr;
	};

	FMCPToolResult DescribeBlueprintExecutionFlow(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FString& RequestedEntryPoint,
		const FString& RequestedEntryNodeId,
		const int32 MaxNodes,
		const int32 MaxEdges,
		const int32 NodeOffset,
		const int32 EdgeOffset,
		const int32 MaxScannedNodes,
		const int32 MaxScannedPins,
		const int32 MaxScannedLinks)
	{
		TArray<UEdGraphNode*> ExecutionNodes;
		TArray<FBlueprintExecutionEdge> ExecutionEdges;
		TMap<UEdGraphNode*, int32> IncomingEdgeCounts;
		TArray<UEdGraphNode*> NodesToScan;
		const int32 GraphNodeSlotCount = Graph->Nodes.Num();
		const int32 NodeSlotScanCount = FMath::Min(
			GraphNodeSlotCount,
			MaxScannedNodes);
		NodesToScan.Reserve(NodeSlotScanCount);
		for (int32 NodeIndex = 0;
		     NodeIndex < NodeSlotScanCount;
		     ++NodeIndex)
		{
			UEdGraphNode* Node = Graph->Nodes[NodeIndex];
			if (Node)
			{
				NodesToScan.Add(Node);
			}
		}
		NodesToScan.Sort([](
			const UEdGraphNode& Left,
			const UEdGraphNode& Right)
			{
				return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
			});
		TSet<UEdGraphNode*> ScannedNodeSet;
		int32 ScannedNodeCount = 0;
		int32 ScannedPinCount = 0;
		int32 ScannedLinkCount = 0;
		bool bScanExhausted = false;
		for (UEdGraphNode* Node : NodesToScan)
		{
			if (ScannedNodeCount >= MaxScannedNodes)
			{
				bScanExhausted = true;
				break;
			}
			++ScannedNodeCount;
			// UE seeds actor event graphs with ReceiveBeginPlay,
			// ReceiveActorBeginOverlap, and ReceiveTick as DefaultGraphNode
			// metadata. They are editor scaffolding rather than authored flow and
			// must not inflate the user-visible execution graph totals.
			if (Node->GetOutermost()->GetMetaData()->HasValue(
				Node,
				FNodeMetadata::DefaultGraphNode))
			{
				continue;
			}
			ScannedNodeSet.Add(Node);
			bool bHasExecutionPin = false;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (ScannedPinCount >= MaxScannedPins)
				{
					bScanExhausted = true;
					break;
				}
				++ScannedPinCount;
				if (!Pin || Pin->PinType.PinCategory
					!= UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				bHasExecutionPin = true;
				if (Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (ScannedLinkCount >= MaxScannedLinks)
					{
						bScanExhausted = true;
						break;
					}
					++ScannedLinkCount;
					UEdGraphNode* LinkedNode =
						LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
					if (!LinkedPin || !LinkedNode
						|| LinkedNode->GetGraph() != Graph
						|| LinkedPin->Direction != EGPD_Input
						|| LinkedPin->PinType.PinCategory
						!= UEdGraphSchema_K2::PC_Exec)
					{
						continue;
					}
					ExecutionEdges.Add({Node, Pin, LinkedNode, LinkedPin});
					IncomingEdgeCounts.FindOrAdd(LinkedNode)++;
				}
				if (bScanExhausted)
				{
					break;
				}
			}
			if (bHasExecutionPin)
			{
				ExecutionNodes.Add(Node);
				IncomingEdgeCounts.FindOrAdd(Node);
			}
			if (bScanExhausted)
			{
				break;
			}
		}
		bScanExhausted |= NodeSlotScanCount < GraphNodeSlotCount;
		ExecutionEdges.RemoveAll([&](const FBlueprintExecutionEdge& Edge)
		{
			return !ScannedNodeSet.Contains(Edge.ToNode);
		});
		IncomingEdgeCounts.Reset();
		for (UEdGraphNode* Node : ExecutionNodes)
		{
			IncomingEdgeCounts.Add(Node, 0);
		}
		for (const FBlueprintExecutionEdge& Edge : ExecutionEdges)
		{
			IncomingEdgeCounts.FindOrAdd(Edge.ToNode)++;
		}
		ExecutionNodes.Sort([](
			const UEdGraphNode& Left,
			const UEdGraphNode& Right)
			{
				return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
			});
		ExecutionEdges.Sort([](
			const FBlueprintExecutionEdge& Left,
			const FBlueprintExecutionEdge& Right)
			{
				const FString LeftKey =
					Left.FromNode->NodeGuid.ToString()
					+ TEXT("|") + Left.FromPin->PinName.ToString()
					+ TEXT("|") + Left.FromPin->PinId.ToString()
					+ TEXT("|") + Left.ToNode->NodeGuid.ToString()
					+ TEXT("|") + Left.ToPin->PinId.ToString();
				const FString RightKey =
					Right.FromNode->NodeGuid.ToString()
					+ TEXT("|") + Right.FromPin->PinName.ToString()
					+ TEXT("|") + Right.FromPin->PinId.ToString()
					+ TEXT("|") + Right.ToNode->NodeGuid.ToString()
					+ TEXT("|") + Right.ToPin->PinId.ToString();
				return LeftKey < RightKey;
			});

		TMap<UEdGraphNode*, TArray<int32>> OutgoingEdges;
		for (int32 EdgeIndex = 0;
		     EdgeIndex < ExecutionEdges.Num();
		     ++EdgeIndex)
		{
			OutgoingEdges.FindOrAdd(
				ExecutionEdges[EdgeIndex].FromNode).Add(EdgeIndex);
		}
		TArray<UEdGraphNode*> GraphEntryNodes;
		TMap<UEdGraphNode*, FString> GraphEntryKinds;
		for (UEdGraphNode* Node : ExecutionNodes)
		{
			FString EntryKind;
			if (Node->IsA<UK2Node_FunctionEntry>())
			{
				EntryKind = TEXT("functionEntry");
			}
			else if (Node->IsA<UK2Node_Event>()
				|| Node->IsA<UK2Node_CustomEvent>())
			{
				EntryKind = TEXT("event");
			}
			else if (IncomingEdgeCounts.FindRef(Node) == 0)
			{
				EntryKind = TEXT("executionRoot");
			}
			if (!EntryKind.IsEmpty())
			{
				GraphEntryNodes.Add(Node);
				GraphEntryKinds.Add(Node, EntryKind);
			}
		}

		UEdGraphNode* SelectedEntryNode = nullptr;
		FString EntryMatchKind;
		FString ResolvedEntryName;
		const bool bEntryPointSelected = !RequestedEntryPoint.IsEmpty();
		const bool bEntryNodeSelected = !RequestedEntryNodeId.IsEmpty();
		if (bEntryPointSelected && bEntryNodeSelected)
		{
			return FMCPToolResult::Error(
				TEXT("entryPoint and entryNodeId are mutually exclusive."),
				TEXT("invalid_params"),
				422);
		}
		if (bEntryNodeSelected)
		{
			FGuid RequestedGuid;
			if (!FGuid::Parse(RequestedEntryNodeId, RequestedGuid)
				|| !RequestedGuid.IsValid())
			{
				return FMCPToolResult::Error(
					TEXT("entryNodeId must be a valid non-zero Node GUID."),
					TEXT("invalid_params"),
					422);
			}
			for (UEdGraphNode* Node : ExecutionNodes)
			{
				if (Node->NodeGuid == RequestedGuid)
				{
					SelectedEntryNode = Node;
					break;
				}
			}
			if (!SelectedEntryNode)
			{
				if (bScanExhausted)
				{
					return FMCPToolResult::Error(
						TEXT(
							"The execution scan reached a safety limit before the requested entryNodeId could be resolved. Narrow the graph or raise the scan limits."),
						TEXT("execution_entry_scan_incomplete"),
						413);
				}
				for (UEdGraphNode* Node : NodesToScan)
				{
					if (Node && Node->NodeGuid == RequestedGuid)
					{
						return FMCPToolResult::Error(
							TEXT("entryNodeId resolves to a node without K2 execution pins."),
							TEXT("execution_entry_not_executable"),
							422);
					}
				}
				return FMCPToolResult::Error(
					TEXT("entryNodeId was not found in the selected graph."),
					TEXT("execution_entry_not_found"),
					404);
			}
			EntryMatchKind = TEXT("nodeGuid");
			ResolvedEntryName = SelectedEntryNode->GetNodeTitle(
				ENodeTitleType::ListView).ToString();
		}
		else if (bEntryPointSelected)
		{
			struct FEntryPointMatch
			{
				UEdGraphNode* Node = nullptr;
				FString MatchKind;
				FString CanonicalName;
			};
			TArray<FEntryPointMatch> Matches;
			auto AddMatch = [&](
				UEdGraphNode* Node,
				const FString& Candidate,
				const TCHAR* MatchKind)
			{
				if (!Node || Candidate.IsEmpty()
					|| !Candidate.Equals(
						RequestedEntryPoint,
						ESearchCase::IgnoreCase))
				{
					return;
				}
				for (const FEntryPointMatch& Existing : Matches)
				{
					if (Existing.Node == Node)
					{
						return;
					}
				}
				Matches.Add({Node, MatchKind, Candidate});
			};
			for (UEdGraphNode* Node : ExecutionNodes)
			{
				if (const UK2Node_CustomEvent* CustomEvent =
					Cast<UK2Node_CustomEvent>(Node))
				{
					AddMatch(
						Node,
						CustomEvent->CustomFunctionName.ToString(),
						TEXT("customEventName"));
				}
				else if (const UK2Node_Event* Event =
					Cast<UK2Node_Event>(Node))
				{
					AddMatch(
						Node,
						Event->EventReference.GetMemberName().ToString(),
						TEXT("eventMemberName"));
					AddMatch(
						Node,
						Event->CustomFunctionName.ToString(),
						TEXT("eventCustomFunctionName"));
				}
				else if (Node->IsA<UK2Node_FunctionEntry>())
				{
					AddMatch(
						Node,
						Graph->GetName(),
						TEXT("functionGraphName"));
				}
				if (Node->IsA<UK2Node_Event>()
					|| Node->IsA<UK2Node_CustomEvent>()
					|| Node->IsA<UK2Node_FunctionEntry>())
				{
					AddMatch(
						Node,
						Node->GetNodeTitle(
							ENodeTitleType::ListView).ToString(),
						TEXT("entryTitle"));
					AddMatch(
						Node,
						Node->GetName(),
						TEXT("entryObjectName"));
				}
			}
			if (Matches.IsEmpty())
			{
				if (bScanExhausted)
				{
					return FMCPToolResult::Error(
						TEXT(
							"The execution scan reached a safety limit before the requested entryPoint could be resolved. Narrow the graph or raise the scan limits."),
						TEXT("execution_entry_scan_incomplete"),
						413);
				}
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT(
							"Exact execution entry point '%s' was not found in graph '%s'. Use an event/custom-event name, a function graph name, or entryNodeId."),
						*RequestedEntryPoint,
						*Graph->GetName()),
					TEXT("execution_entry_not_found"),
					404);
			}
			if (Matches.Num() > 1)
			{
				TArray<FString> CandidateIds;
				const int32 CandidateLimit = FMath::Min(Matches.Num(), 8);
				for (int32 Index = 0; Index < CandidateLimit; ++Index)
				{
					CandidateIds.Add(Matches[Index].Node->NodeGuid.ToString());
				}
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Execution entry point '%s' is ambiguous across %d nodes (%s%s). Retry with entryNodeId."),
						*RequestedEntryPoint,
						Matches.Num(),
						*FString::Join(CandidateIds, TEXT(", ")),
						Matches.Num() > CandidateLimit ? TEXT(", ...") : TEXT("")),
					TEXT("execution_entry_ambiguous"),
					409);
			}
			SelectedEntryNode = Matches[0].Node;
			EntryMatchKind = Matches[0].MatchKind;
			ResolvedEntryName = Matches[0].CanonicalName;
		}

		TArray<UEdGraphNode*> OrderedNodes;
		TSet<UEdGraphNode*> VisitedNodes;
		TSet<UEdGraphNode*> QueuedNodes;
		auto TraverseFrom = [&](UEdGraphNode* StartNode)
		{
			TArray<UEdGraphNode*> Queue;
			Queue.Add(StartNode);
			QueuedNodes.Add(StartNode);
			int32 QueueIndex = 0;
			while (QueueIndex < Queue.Num())
			{
				UEdGraphNode* Current = Queue[QueueIndex++];
				if (!Current || VisitedNodes.Contains(Current))
				{
					continue;
				}
				VisitedNodes.Add(Current);
				OrderedNodes.Add(Current);
				if (const TArray<int32>* EdgeIndices =
					OutgoingEdges.Find(Current))
				{
					for (const int32 EdgeIndex : *EdgeIndices)
					{
						UEdGraphNode* Next = ExecutionEdges[EdgeIndex].ToNode;
						if (!VisitedNodes.Contains(Next)
							&& !QueuedNodes.Contains(Next))
						{
							Queue.Add(Next);
							QueuedNodes.Add(Next);
						}
					}
				}
			}
		};
		TArray<UEdGraphNode*> TraversalEntryNodes;
		TMap<UEdGraphNode*, FString> ProjectedEntryKinds = GraphEntryKinds;
		if (SelectedEntryNode)
		{
			TraversalEntryNodes.Add(SelectedEntryNode);
			TraverseFrom(SelectedEntryNode);
		}
		else
		{
			TraversalEntryNodes = GraphEntryNodes;
			for (UEdGraphNode* EntryNode : TraversalEntryNodes)
			{
				TraverseFrom(EntryNode);
			}
			for (UEdGraphNode* Node : ExecutionNodes)
			{
				if (!VisitedNodes.Contains(Node))
				{
					ProjectedEntryKinds.Add(Node, TEXT("disconnected"));
					TraversalEntryNodes.Add(Node);
					TraverseFrom(Node);
				}
			}
		}

		TSet<UEdGraphNode*> SelectedNodes;
		for (UEdGraphNode* Node : OrderedNodes)
		{
			SelectedNodes.Add(Node);
		}
		TArray<const FBlueprintExecutionEdge*> SelectedEdges;
		TMap<UEdGraphNode*, int32> SelectedOutgoingEdgeCounts;
		for (const FBlueprintExecutionEdge& Edge : ExecutionEdges)
		{
			if (SelectedNodes.Contains(Edge.FromNode)
				&& SelectedNodes.Contains(Edge.ToNode))
			{
				SelectedEdges.Add(&Edge);
				SelectedOutgoingEdgeCounts.FindOrAdd(Edge.FromNode)++;
			}
		}

		const int32 NodePageStart = FMath::Min(NodeOffset, OrderedNodes.Num());
		const int32 NodePageEnd = NodePageStart
			+ FMath::Min(MaxNodes, OrderedNodes.Num() - NodePageStart);
		TSet<UEdGraphNode*> IncludedNodes;
		for (int32 Index = NodePageStart; Index < NodePageEnd; ++Index)
		{
			IncludedNodes.Add(OrderedNodes[Index]);
		}
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		TArray<TSharedPtr<FJsonValue>> DescriptionLines;
		TArray<FString> DescriptionText;
		for (int32 Order = NodePageStart; Order < NodePageEnd; ++Order)
		{
			UEdGraphNode* Node = OrderedNodes[Order];
			FString Title = Node->GetNodeTitle(
				ENodeTitleType::ListView).ToString();
			const bool bTitleTruncated = Title.Len() > 512;
			if (bTitleTruncated)
			{
				Title.LeftInline(512, false);
			}
			TSharedRef<FJsonObject> NodeValue = MakeShared<FJsonObject>();
			NodeValue->SetNumberField(TEXT("order"), Order);
			NodeValue->SetStringField(
				TEXT("nodeId"), Node->NodeGuid.ToString());
			NodeValue->SetStringField(
				TEXT("nodeClass"), Node->GetClass()->GetPathName());
			NodeValue->SetStringField(TEXT("title"), Title);
			NodeValue->SetBoolField(
				TEXT("titleTruncated"), bTitleTruncated);
			const FString* EntryKind = ProjectedEntryKinds.Find(Node);
			NodeValue->SetBoolField(
				TEXT("isEntryPoint"), EntryKind != nullptr);
			if (EntryKind)
			{
				NodeValue->SetStringField(TEXT("entryKind"), *EntryKind);
			}
			NodeValue->SetBoolField(
				TEXT("isSelectedEntry"), Node == SelectedEntryNode);
			const int32 OutgoingCount =
				SelectedOutgoingEdgeCounts.FindRef(Node);
			NodeValue->SetNumberField(
				TEXT("outgoingExecutionEdgeCount"), OutgoingCount);
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeValue));
			const FString DescriptionLine = FString::Printf(
				TEXT("%d. %s [%s] -> %d execution edge(s)"),
				Order + 1,
				*Title,
				*Node->NodeGuid.ToString(),
				OutgoingCount);
			DescriptionLines.Add(
				MakeShared<FJsonValueString>(DescriptionLine));
			DescriptionText.Add(DescriptionLine);
		}

		TArray<TSharedPtr<FJsonValue>> EdgeValues;
		const int32 EdgePageStart = FMath::Min(EdgeOffset, SelectedEdges.Num());
		const int32 EdgePageEnd = EdgePageStart
			+ FMath::Min(MaxEdges, SelectedEdges.Num() - EdgePageStart);
		for (int32 Index = EdgePageStart; Index < EdgePageEnd; ++Index)
		{
			const FBlueprintExecutionEdge& Edge = *SelectedEdges[Index];
			TSharedRef<FJsonObject> EdgeValue = MakeShared<FJsonObject>();
			EdgeValue->SetStringField(
				TEXT("edgeId"),
				Edge.FromNode->NodeGuid.ToString()
				+ TEXT(":") + Edge.FromPin->PinId.ToString()
				+ TEXT("->") + Edge.ToNode->NodeGuid.ToString()
				+ TEXT(":") + Edge.ToPin->PinId.ToString());
			EdgeValue->SetStringField(
				TEXT("fromNodeId"), Edge.FromNode->NodeGuid.ToString());
			EdgeValue->SetStringField(
				TEXT("fromPinId"), Edge.FromPin->PinId.ToString());
			EdgeValue->SetStringField(
				TEXT("fromPin"), Edge.FromPin->PinName.ToString());
			EdgeValue->SetStringField(
				TEXT("toNodeId"), Edge.ToNode->NodeGuid.ToString());
			EdgeValue->SetStringField(
				TEXT("toPinId"), Edge.ToPin->PinId.ToString());
			EdgeValue->SetStringField(
				TEXT("toPin"), Edge.ToPin->PinName.ToString());
			EdgeValue->SetBoolField(
				TEXT("sourceIncluded"), IncludedNodes.Contains(Edge.FromNode));
			EdgeValue->SetBoolField(
				TEXT("targetIncluded"), IncludedNodes.Contains(Edge.ToNode));
			EdgeValues.Add(MakeShared<FJsonValueObject>(EdgeValue));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph"), Graph->GetName());
		Result->SetStringField(
			TEXT("selectionMode"),
			bEntryPointSelected
				? TEXT("entryPoint")
				: bEntryNodeSelected
				? TEXT("entryNodeId")
				: TEXT("allEntries"));
		if (SelectedEntryNode)
		{
			const bool bResolvedEntryNameTruncated =
				ResolvedEntryName.Len() > 512;
			if (bResolvedEntryNameTruncated)
			{
				ResolvedEntryName.LeftInline(512, false);
			}
			Result->SetStringField(
				TEXT("resolvedEntryNodeId"),
				SelectedEntryNode->NodeGuid.ToString());
			Result->SetStringField(TEXT("resolvedEntryName"), ResolvedEntryName);
			Result->SetBoolField(
				TEXT("resolvedEntryNameTruncated"),
				bResolvedEntryNameTruncated);
			Result->SetStringField(TEXT("entryMatchKind"), EntryMatchKind);
		}
		Result->SetStringField(
			TEXT("description"), FString::Join(DescriptionText, TEXT("\n")));
		Result->SetNumberField(
			TEXT("executionNodeTotal"), ExecutionNodes.Num());
		Result->SetNumberField(
			TEXT("executionEdgeTotal"), ExecutionEdges.Num());
		Result->SetNumberField(
			TEXT("selectedExecutionNodeTotal"), OrderedNodes.Num());
		Result->SetNumberField(
			TEXT("selectedExecutionEdgeTotal"), SelectedEdges.Num());
		Result->SetNumberField(TEXT("nodeCount"), NodeValues.Num());
		Result->SetNumberField(TEXT("edgeCount"), EdgeValues.Num());
		Result->SetNumberField(
			TEXT("entryPointCount"), TraversalEntryNodes.Num());
		Result->SetNumberField(
			TEXT("graphEntryPointCount"), GraphEntryNodes.Num());
		Result->SetNumberField(TEXT("maxNodes"), MaxNodes);
		Result->SetNumberField(TEXT("maxEdges"), MaxEdges);
		Result->SetNumberField(TEXT("nodeOffset"), NodeOffset);
		Result->SetNumberField(TEXT("edgeOffset"), EdgeOffset);
		Result->SetNumberField(TEXT("maxScannedNodes"), MaxScannedNodes);
		Result->SetNumberField(TEXT("maxScannedPins"), MaxScannedPins);
		Result->SetNumberField(TEXT("maxScannedLinks"), MaxScannedLinks);
		Result->SetNumberField(TEXT("scannedNodeCount"), ScannedNodeCount);
		Result->SetNumberField(TEXT("scannedPinCount"), ScannedPinCount);
		Result->SetNumberField(TEXT("scannedLinkCount"), ScannedLinkCount);
		Result->SetNumberField(TEXT("graphNodeTotal"), GraphNodeSlotCount);
		Result->SetNumberField(
			TEXT("scannedNodeSlotCount"), NodeSlotScanCount);
		Result->SetBoolField(TEXT("scanExhausted"), bScanExhausted);
		Result->SetBoolField(TEXT("executionTotalsComplete"), !bScanExhausted);
		Result->SetBoolField(
			TEXT("nodesTruncated"),
			NodePageStart > 0 || NodePageEnd < OrderedNodes.Num());
		Result->SetBoolField(
			TEXT("edgesTruncated"),
			EdgePageStart > 0 || EdgePageEnd < SelectedEdges.Num());
		Result->SetBoolField(
			TEXT("nodesHasMore"), NodePageEnd < OrderedNodes.Num());
		Result->SetBoolField(
			TEXT("edgesHasMore"), EdgePageEnd < SelectedEdges.Num());
		if (NodePageEnd < OrderedNodes.Num())
		{
			Result->SetNumberField(TEXT("nextNodeOffset"), NodePageEnd);
		}
		if (EdgePageEnd < SelectedEdges.Num())
		{
			Result->SetNumberField(TEXT("nextEdgeOffset"), EdgePageEnd);
		}
		Result->SetBoolField(
			TEXT("partial"),
			bScanExhausted
			|| NodePageStart > 0 || NodePageEnd < OrderedNodes.Num()
			|| EdgePageStart > 0 || EdgePageEnd < SelectedEdges.Num());
		Result->SetArrayField(TEXT("nodes"), NodeValues);
		Result->SetArrayField(TEXT("executionEdges"), EdgeValues);
		Result->SetArrayField(TEXT("descriptionLines"), DescriptionLines);
		return FMCPToolResult::Ok(Result);
	}

	FString TypeNameWithoutUnrealPrefix(const FString& TypeName)
	{
		return TypeName.Len() > 1
		       && (TypeName[0] == TEXT('F')
			       || TypeName[0] == TEXT('E')
			       || TypeName[0] == TEXT('U')
			       || TypeName[0] == TEXT('A'))
			       ? TypeName.Mid(1)
			       : TypeName;
	}

	bool PinTypeMatchesRequestedType(
		const FEdGraphPinType& PinType,
		const FString& RequestedType,
		const FString& RequestedTypeWithoutPrefix)
	{
		auto MatchesCandidate = [&](const FString& Candidate)
		{
			return !Candidate.IsEmpty()
				&& (Candidate.Equals(
						RequestedType,
						ESearchCase::IgnoreCase)
					|| Candidate.Equals(
						RequestedTypeWithoutPrefix,
						ESearchCase::IgnoreCase)
					|| TypeNameWithoutUnrealPrefix(Candidate).Equals(
						RequestedTypeWithoutPrefix,
						ESearchCase::IgnoreCase));
		};
		if (MatchesCandidate(PinType.PinCategory.ToString())
			|| MatchesCandidate(PinType.PinSubCategory.ToString()))
		{
			return true;
		}
		if (const UObject* TypeObject = PinType.PinSubCategoryObject.Get())
		{
			return MatchesCandidate(TypeObject->GetName())
				|| MatchesCandidate(TypeObject->GetPathName());
		}
		return false;
	}

	void AddPinTypeIdentity(
		const FEdGraphPinType& PinType,
		const TSharedRef<FJsonObject>& Result)
	{
		Result->SetStringField(
			TEXT("currentType"), PinType.PinCategory.ToString());
		if (!PinType.PinSubCategory.IsNone())
		{
			Result->SetStringField(
				TEXT("currentSubcategory"),
				PinType.PinSubCategory.ToString());
		}
		if (const UObject* TypeObject = PinType.PinSubCategoryObject.Get())
		{
			Result->SetStringField(TEXT("typeObject"), TypeObject->GetPathName());
			Result->SetStringField(
				TEXT("canonicalTypePath"), TypeObject->GetPathName());
		}
		else
		{
			Result->SetStringField(
				TEXT("canonicalTypePath"), PinType.PinCategory.ToString());
		}
		const TCHAR* Container = TEXT("none");
		switch (PinType.ContainerType)
		{
		case EPinContainerType::Array:
			Container = TEXT("array");
			break;
		case EPinContainerType::Set:
			Container = TEXT("set");
			break;
		case EPinContainerType::Map:
			Container = TEXT("map");
			break;
		default:
			break;
		}
		Result->SetStringField(TEXT("containerType"), Container);
	}

	bool IsBlueprintParameterNode(const UEdGraphNode* Node)
	{
		return Node
			&& (Node->IsA<UK2Node_FunctionEntry>()
				|| Node->IsA<UK2Node_FunctionResult>()
				|| Node->IsA<UK2Node_CustomEvent>()
				|| Node->IsA<UK2Node_Event>());
	}
}

// ============================================================
// list_blueprints
// ============================================================
class FTool_ListBlueprints : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Filter;
		FString ParentClassFilter;
		FString TypeFilter;
		if (!Params.IsValid()
			|| (Params->HasField(TEXT("filter"))
				&& !Params->TryGetStringField(TEXT("filter"), Filter))
			|| (Params->HasField(TEXT("parentClass"))
				&& !Params->TryGetStringField(
					TEXT("parentClass"), ParentClassFilter))
			|| (Params->HasField(TEXT("type"))
				&& !Params->TryGetStringField(TEXT("type"), TypeFilter))
			|| Filter.Len() > 512
			|| ParentClassFilter.Len() > 256
			|| TypeFilter.Len() > 16)
		{
			return FMCPToolResult::Error(
				TEXT("Invalid or oversized Blueprint list filter."),
				TEXT("invalid_params"),
				422);
		}
		int32 Limit = 50;
		int32 Offset = 0;
		int32 MaxScannedAssets = 5000;
		if (!ReadBlueprintListPageInteger(Params, TEXT("limit"), 50, 1, 200, Limit)
			|| !ReadBlueprintListPageInteger(Params, TEXT("offset"), 0, 0, MAX_int32, Offset)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedAssets"),
				5000,
				1,
				50000,
				MaxScannedAssets))
		{
			return FMCPToolResult::Error(
				TEXT("limit must be an integer in [1, 200] and offset must be an integer in [0, 2147483647]."),
				TEXT("invalid_params"),
				422);
		}
		if (!TypeFilter.IsEmpty() && TypeFilter != TEXT("all")
			&& TypeFilter != TEXT("regular") && TypeFilter != TEXT("level"))
		{
			return FMCPToolResult::Error(
				TEXT("type must be all, regular, or level."), TEXT("invalid_params"), 422);
		}

		const bool bIncludeRegular = TypeFilter.IsEmpty() || TypeFilter == TEXT("all") || TypeFilter == TEXT("regular");
		const bool bIncludeLevel = TypeFilter.IsEmpty() || TypeFilter == TEXT("all") || TypeFilter == TEXT("level");

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FBlueprintListEntry> Matches;
		int32 ScannedAssetCount = 0;
		bool bScanExhausted = false;

		if (bIncludeRegular)
		{
			TArray<FAssetData> AllBP;
			Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBP, true);
			AllBP.Sort([](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.PackageName.LexicalLess(Right.PackageName);
			});

			for (const FAssetData& Asset : AllBP)
			{
				if (ScannedAssetCount >= MaxScannedAssets)
				{
					bScanExhausted = true;
					break;
				}
				++ScannedAssetCount;
				FString Name = Asset.AssetName.ToString();
				FString Path = Asset.PackageName.ToString();

				if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase) && !Path.Contains(
					Filter, ESearchCase::IgnoreCase))
					continue;

				FString ParentClass;
				Asset.GetTagValue(FName(TEXT("ParentClass")), ParentClass);
				int32 DotIndex;
				if (ParentClass.FindLastChar('.', DotIndex))
					ParentClass = ParentClass.Mid(DotIndex + 1);

				if (!ParentClassFilter.IsEmpty() && !ParentClass.Contains(ParentClassFilter, ESearchCase::IgnoreCase))
					continue;

				FBlueprintListEntry& Entry = Matches.AddDefaulted_GetRef();
				Entry.Name = MoveTemp(Name);
				Entry.Path = MoveTemp(Path);
				Entry.ParentClass = MoveTemp(ParentClass);
			}
		}

		if (bIncludeLevel && !bScanExhausted)
		{
			TArray<FAssetData> AllMaps;
			Registry.GetAssetsByClass(UWorld::StaticClass()->GetClassPathName(), AllMaps, false);
			AllMaps.Sort([](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.PackageName.LexicalLess(Right.PackageName);
			});

			for (const FAssetData& Asset : AllMaps)
			{
				if (ScannedAssetCount >= MaxScannedAssets)
				{
					bScanExhausted = true;
					break;
				}
				++ScannedAssetCount;
				FString Name = Asset.AssetName.ToString();
				FString Path = Asset.PackageName.ToString();

				if (!Filter.IsEmpty() && !Name.Contains(Filter, ESearchCase::IgnoreCase) && !Path.Contains(
					Filter, ESearchCase::IgnoreCase))
					continue;

				if (!ParentClassFilter.IsEmpty() && !FString(TEXT("LevelScriptActor")).Contains(
					ParentClassFilter, ESearchCase::IgnoreCase))
					continue;

				FBlueprintListEntry& Entry = Matches.AddDefaulted_GetRef();
				Entry.Name = MoveTemp(Name);
				Entry.Path = MoveTemp(Path);
				Entry.ParentClass = TEXT("LevelScriptActor");
				Entry.bIsLevelBlueprint = true;
			}
		}

		Matches.Sort([](const FBlueprintListEntry& Left, const FBlueprintListEntry& Right)
		{
			return Left.Path.Compare(Right.Path, ESearchCase::CaseSensitive) < 0;
		});
		const int32 Total = Matches.Num();
		const int32 PageStart = FMath::Min(Offset, Total);
		const int32 PageEnd = PageStart + FMath::Min(Limit, Total - PageStart);
		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Reserve(PageEnd - PageStart);
		for (int32 Index = PageStart; Index < PageEnd; ++Index)
		{
			const FBlueprintListEntry& Match = Matches[Index];
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Match.Name);
			Entry->SetStringField(TEXT("path"), Match.Path);
			Entry->SetStringField(TEXT("parentClass"), Match.ParentClass);
			Entry->SetBoolField(TEXT("isLevelBlueprint"), Match.bIsLevelBlueprint);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), Entries.Num());
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(
			TEXT("scannedAssetCount"), ScannedAssetCount);
		Result->SetNumberField(
			TEXT("maxScannedAssets"), MaxScannedAssets);
		Result->SetBoolField(TEXT("scanExhausted"), bScanExhausted);
		Result->SetBoolField(TEXT("totalComplete"), !bScanExhausted);
		Result->SetBoolField(TEXT("partial"), bScanExhausted);
		Result->SetBoolField(
			TEXT("hasMore"), PageEnd < Total || bScanExhausted);
		if (!bScanExhausted && PageEnd < Total)
		{
			Result->SetNumberField(TEXT("nextOffset"), PageEnd);
		}
		Result->SetArrayField(TEXT("blueprints"), Entries);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// get_blueprint
// ============================================================
class FTool_GetBlueprint : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'name' parameter"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(Name, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("asset_not_found"),
				404);
		}

		return FMCPToolResult::Ok(MCPHelpers::SerializeBlueprint(BP));
	}
};

// ============================================================
// get_blueprint_graph
// ============================================================
class FTool_GetBlueprintGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		using namespace UEAIIntegration::BlueprintGraph;

		FString GeometryMode = TEXT("auto");
		Params->TryGetStringField(TEXT("geometryMode"), GeometryMode);
		if (GeometryMode != TEXT("auto")
			&& GeometryMode != TEXT("stored")
			&& GeometryMode != TEXT("editor"))
		{
			return InvalidRequest(
				FString::Printf(
					TEXT("Unsupported geometryMode '%s'."),
					*GeometryMode));
		}

		FContext Context;
		FMCPToolResult ContextResult = ResolveBlueprintGraph(Params, Context);
		if (!ContextResult.bSuccess)
		{
			return ContextResult;
		}

		bool bUseEditorGeometry = false;
		if (GeometryMode == TEXT("editor"))
		{
			ContextResult = ResolveGraphEditor(Context, true);
			if (!ContextResult.bSuccess)
			{
				return EditorUnavailable(
					TEXT("Exact Graph Editor geometry is unavailable."),
					TEXT("graph_geometry_unavailable"));
			}
			bUseEditorGeometry = true;
		}
		else if (GeometryMode == TEXT("auto"))
		{
			ContextResult = ResolveGraphEditor(Context, false);
			bUseEditorGeometry = ContextResult.bSuccess;
		}

		TSharedPtr<FJsonObject> GraphJson =
			MCPHelpers::SerializeGraph(Context.Graph);
		if (!GraphJson.IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("Could not serialize Blueprint graph."));
		}

		TMap<FGuid, FNodeBounds> BoundsByNode;
		TArray<FString> UnresolvedEditorNodes;
		int32 ExactCount = 0;
		for (UEdGraphNode* Node : Context.Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			FNodeBounds Bounds;
			TryGetNodeBounds(Context, Node, bUseEditorGeometry, Bounds);
			if (Bounds.bExact)
			{
				++ExactCount;
			}
			else if (bUseEditorGeometry
				&& UnresolvedEditorNodes.Num() < 8)
			{
				FSlateRect RawRect;
				const bool bHasWidget =
					Context.GraphEditor.IsValid()
					&& Context.GraphEditor->GetBoundsForNode(
						Node,
						RawRect,
						0.0f);
				UnresolvedEditorNodes.Add(
					FString::Printf(
						TEXT(
							"%s (%s, %s, widget=%s, rect=%.1f,%.1f,"
							"%.1f,%.1f, stored=%dx%d)"),
						*Node->NodeGuid.ToString(),
						*Node->GetClass()->GetName(),
						*Node->GetNodeTitle(
							ENodeTitleType::ListView).ToString(),
						bHasWidget ? TEXT("true") : TEXT("false"),
						RawRect.Left,
						RawRect.Top,
						RawRect.Right,
						RawRect.Bottom,
						Node->NodeWidth,
						Node->NodeHeight));
			}
			BoundsByNode.Add(Node->NodeGuid, Bounds);
		}

		const TArray<TSharedPtr<FJsonValue>>& NodeValues =
			GraphJson->GetArrayField(TEXT("nodes"));
		for (const TSharedPtr<FJsonValue>& NodeValue : NodeValues)
		{
			const TSharedPtr<FJsonObject> NodeJson = NodeValue->AsObject();
			FGuid NodeGuid;
			if (!NodeJson.IsValid()
				|| !FGuid::Parse(
					NodeJson->GetStringField(TEXT("nodeId")),
					NodeGuid))
			{
				continue;
			}
			const FNodeBounds* Bounds = BoundsByNode.Find(NodeGuid);
			if (!Bounds)
			{
				continue;
			}
			NodeJson->SetObjectField(TEXT("bounds"), BoundsToJson(*Bounds));

			UEdGraphNode_Comment* Comment =
				Cast<UEdGraphNode_Comment>(FindNode(Context.Graph, NodeGuid));
			if (!Comment)
			{
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Contained;
			TArray<TSharedPtr<FJsonValue>> Intersecting;
			TArray<TSharedPtr<FJsonValue>> Unresolved;
			for (const TPair<FGuid, FNodeBounds>& Pair : BoundsByNode)
			{
				if (Pair.Key == NodeGuid)
				{
					continue;
				}
				if (!Bounds->bExact || !Pair.Value.bExact)
				{
					Unresolved.Add(
						MakeShared<FJsonValueString>(Pair.Key.ToString()));
				}
				else if (Contains(*Bounds, Pair.Value))
				{
					Contained.Add(
						MakeShared<FJsonValueString>(Pair.Key.ToString()));
				}
				else if (Intersects(*Bounds, Pair.Value))
				{
					Intersecting.Add(
						MakeShared<FJsonValueString>(Pair.Key.ToString()));
				}
			}
			Contained.Sort(
				[](const TSharedPtr<FJsonValue>& Left,
				   const TSharedPtr<FJsonValue>& Right)
				{
					return Left->AsString() < Right->AsString();
				});
			Intersecting.Sort(
				[](const TSharedPtr<FJsonValue>& Left,
				   const TSharedPtr<FJsonValue>& Right)
				{
					return Left->AsString() < Right->AsString();
				});
			Unresolved.Sort(
				[](const TSharedPtr<FJsonValue>& Left,
				   const TSharedPtr<FJsonValue>& Right)
				{
					return Left->AsString() < Right->AsString();
				});
			NodeJson->SetArrayField(TEXT("containedNodeIds"), Contained);
			NodeJson->SetArrayField(
				TEXT("intersectingNodeIds"),
				Intersecting);
			NodeJson->SetArrayField(
				TEXT("unresolvedContainmentNodeIds"),
				Unresolved);
			NodeJson->SetStringField(
				TEXT("containmentStatus"),
				Bounds->bExact && Unresolved.IsEmpty()
					? TEXT("exact")
					: TEXT("incomplete"));
		}

		const int32 NodeCount = BoundsByNode.Num();
		const FString GeometryStatus =
			!bUseEditorGeometry
				? TEXT("storedOnly")
				: NodeCount == 0
				? TEXT("exact")
				: NodeCount > 0 && ExactCount == NodeCount
				? TEXT("exact")
				: ExactCount > 0
				? TEXT("partial")
				: TEXT("storedOnly");
		if (GeometryMode == TEXT("editor")
			&& GeometryStatus != TEXT("exact"))
		{
			return EditorUnavailable(
				FString::Printf(
					TEXT(
						"The Graph Editor could not resolve every node "
						"geometry. Unresolved: %s"),
					*FString::Join(UnresolvedEditorNodes, TEXT("; "))),
				TEXT("graph_geometry_unavailable"));
		}
		GraphJson->SetStringField(TEXT("geometryMode"), GeometryMode);
		GraphJson->SetStringField(
			TEXT("geometryStatus"),
			GeometryStatus);
		GraphJson->SetStringField(
			TEXT("graphHash"),
			ComputeGraphHash(Context.Graph));
		return FMCPToolResult::Ok(GraphJson);
	}
};

// ============================================================
// search_blueprints
// ============================================================
class FTool_SearchBlueprints : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.search");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Query;
		FString PathFilter;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("query"), Query)
			|| Query.IsEmpty()
			|| (Params->HasField(TEXT("path"))
				&& !Params->TryGetStringField(TEXT("path"), PathFilter))
			|| Query.Len() > 512
			|| PathFilter.Len() > 1024)
		{
			return FMCPToolResult::Error(
				TEXT("query must be a string of 1..512 characters and path at most 1024."),
				TEXT("invalid_params"),
				422);
		}
		int32 MaxResults = 50;
		int32 MaxAssets = 500;
		int32 MaxScannedNodes = 50000;
		if (!ReadBlueprintListPageInteger(
				Params, TEXT("maxResults"), 50, 1, 200, MaxResults)
			|| !ReadBlueprintListPageInteger(
				Params, TEXT("maxAssets"), 500, 1, 5000, MaxAssets)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedNodes"),
				50000,
				1,
				200000,
				MaxScannedNodes))
		{
			return FMCPToolResult::Error(
				TEXT("Invalid Blueprint search result or scan limit."),
				TEXT("invalid_params"),
				422);
		}

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AllBP;
		Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBP, true);
		AllBP.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.LexicalLess(Right.PackageName);
		});

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 ScannedAssetCount = 0;
		int32 SkippedLoadCount = 0;
		int32 ScannedNodeCount = 0;
		bool bResultTruncated = false;
		bool bScanExhausted = false;

		for (const FAssetData& Asset : AllBP)
		{
			if (bResultTruncated || bScanExhausted)
			{
				break;
			}
			FString Path = Asset.PackageName.ToString();
			if (!PathFilter.IsEmpty()
				&& !Path.Contains(PathFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (ScannedAssetCount >= MaxAssets)
			{
				bScanExhausted = true;
				break;
			}
			++ScannedAssetCount;

			UBlueprint* BP = Cast<UBlueprint>(const_cast<FAssetData&>(Asset).GetAsset());
			if (!BP)
			{
				++SkippedLoadCount;
				continue;
			}

			TArray<UEdGraph*> Graphs;
			BP->GetAllGraphs(Graphs);
			Graphs.RemoveAll([](const UEdGraph* Graph)
			{
				return Graph == nullptr;
			});
			Graphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
			{
				return Left.GraphGuid.ToString() < Right.GraphGuid.ToString();
			});

			for (UEdGraph* Graph : Graphs)
			{
				const int32 RemainingNodeBudget = FMath::Max(
					0,
					MaxScannedNodes - ScannedNodeCount);
				const int32 NodeSlotScanCount = FMath::Min(
					Graph->Nodes.Num(),
					RemainingNodeBudget);
				const bool bGraphNodeScanExhausted =
					NodeSlotScanCount < Graph->Nodes.Num();
				TArray<UEdGraphNode*> Nodes;
				Nodes.Reserve(NodeSlotScanCount);
				for (int32 NodeIndex = 0;
				     NodeIndex < NodeSlotScanCount;
				     ++NodeIndex)
				{
					++ScannedNodeCount;
					UEdGraphNode* Node = Graph->Nodes[NodeIndex];
					if (Node)
					{
						Nodes.Add(Node);
					}
				}
				Nodes.Sort([](
					const UEdGraphNode& Left,
					const UEdGraphNode& Right)
					{
						return Left.NodeGuid.ToString()
							< Right.NodeGuid.ToString();
					});
				for (UEdGraphNode* Node : Nodes)
				{
					FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

					FString FuncName, EventName, VarName;
					if (auto* CF = Cast<UK2Node_CallFunction>(Node))
						FuncName = CF->FunctionReference.GetMemberName().ToString();
					else if (auto* Ev = Cast<UK2Node_Event>(Node))
						EventName = Ev->EventReference.GetMemberName().ToString();
					else if (auto* CE = Cast<UK2Node_CustomEvent>(Node))
						EventName = CE->CustomFunctionName.ToString();
					else if (auto* VG = Cast<UK2Node_VariableGet>(Node))
						VarName = VG->GetVarName().ToString();
					else if (auto* VS = Cast<UK2Node_VariableSet>(Node))
						VarName = VS->GetVarName().ToString();

					bool bMatch = Title.Contains(Query, ESearchCase::IgnoreCase) ||
						(!FuncName.IsEmpty() && FuncName.Contains(Query, ESearchCase::IgnoreCase)) ||
						(!EventName.IsEmpty() && EventName.Contains(Query, ESearchCase::IgnoreCase)) ||
						(!VarName.IsEmpty() && VarName.Contains(Query, ESearchCase::IgnoreCase));

					if (bMatch)
					{
						if (Results.Num() >= MaxResults)
						{
							bResultTruncated = true;
							break;
						}
						TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
						R->SetStringField(TEXT("blueprint"), Asset.AssetName.ToString());
						R->SetStringField(TEXT("blueprintPath"), Path);
						R->SetStringField(TEXT("graph"), Graph->GetName());
						R->SetStringField(
							TEXT("graphId"), Graph->GraphGuid.ToString());
						R->SetStringField(
							TEXT("nodeId"), Node->NodeGuid.ToString());
						const bool bTitleTruncated = Title.Len() > 512;
						if (bTitleTruncated)
						{
							Title.LeftInline(512, false);
						}
						R->SetStringField(TEXT("nodeTitle"), Title);
						R->SetBoolField(
							TEXT("nodeTitleTruncated"), bTitleTruncated);
						R->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetName());
						if (!FuncName.IsEmpty()) R->SetStringField(TEXT("functionName"), FuncName);
						if (!EventName.IsEmpty()) R->SetStringField(TEXT("eventName"), EventName);
						if (!VarName.IsEmpty()) R->SetStringField(TEXT("variableName"), VarName);
						Results.Add(MakeShared<FJsonValueObject>(R));
					}
				}
				bScanExhausted |= bGraphNodeScanExhausted;
				if (bResultTruncated || bScanExhausted)
				{
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("query"), Query);
		Result->SetNumberField(TEXT("resultCount"), Results.Num());
		Result->SetNumberField(TEXT("maxResults"), MaxResults);
		Result->SetNumberField(TEXT("maxAssets"), MaxAssets);
		Result->SetNumberField(
			TEXT("maxScannedNodes"), MaxScannedNodes);
		Result->SetNumberField(
			TEXT("scannedAssetCount"), ScannedAssetCount);
		Result->SetNumberField(TEXT("skippedLoadCount"), SkippedLoadCount);
		Result->SetNumberField(TEXT("scannedNodeCount"), ScannedNodeCount);
		Result->SetBoolField(TEXT("resultTruncated"), bResultTruncated);
		Result->SetBoolField(TEXT("scanExhausted"), bScanExhausted);
		Result->SetBoolField(
			TEXT("partial"), bResultTruncated || bScanExhausted);
		Result->SetArrayField(TEXT("results"), Results);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// get_blueprint_summary — brief overview without full graph data
// ============================================================
class FTool_GetBlueprintSummary : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.summary");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'name' parameter"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(Name, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		return FMCPToolResult::Ok(MCPHelpers::SerializeBlueprint(BP));
	}
};

// ============================================================
// describe_graph — human-readable description of graph flow
// ============================================================
class FTool_DescribeGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.describe");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name;
		FString GraphName;
		FString EntryPoint;
		FString EntryNodeId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("name"), Name)
			|| !Params->TryGetStringField(TEXT("graph"), GraphName)
			|| (Params->HasField(TEXT("entryPoint"))
				&& !Params->TryGetStringField(
					TEXT("entryPoint"), EntryPoint))
			|| (Params->HasField(TEXT("entryNodeId"))
				&& !Params->TryGetStringField(
					TEXT("entryNodeId"), EntryNodeId))
			|| Name.IsEmpty() || Name.Len() > 1024
			|| GraphName.IsEmpty() || GraphName.Len() > 256
			|| EntryPoint.Len() > 256
			|| EntryNodeId.Len() > 36
			|| (!EntryPoint.IsEmpty() && !EntryNodeId.IsEmpty()))
		{
			return FMCPToolResult::Error(
				TEXT(
					"name must be a string of 1..1024 characters, graph a string of 1..256 characters, and at most one bounded entry selector may be supplied."),
				TEXT("invalid_params"),
				422);
		}
		if (Params->HasField(TEXT("entryPoint")) && EntryPoint.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("entryPoint must contain between 1 and 256 characters when supplied."),
				TEXT("invalid_params"),
				422);
		}
		if (Params->HasField(TEXT("entryNodeId"))
			&& (EntryNodeId.Len() < 32 || EntryNodeId.Len() > 36))
		{
			return FMCPToolResult::Error(
				TEXT("entryNodeId must contain a Node GUID."),
				TEXT("invalid_params"),
				422);
		}
		int32 MaxNodes = 200;
		int32 MaxEdges = 1000;
		int32 NodeOffset = 0;
		int32 EdgeOffset = 0;
		int32 MaxScannedNodes = 5000;
		int32 MaxScannedPins = 50000;
		int32 MaxScannedLinks = 100000;
		if (!ReadBlueprintListPageInteger(
				Params,
				TEXT("maxNodes"),
				200,
				1,
				500,
				MaxNodes)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxEdges"),
				1000,
				1,
				2000,
				MaxEdges)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("nodeOffset"),
				0,
				0,
				MAX_int32,
				NodeOffset)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("edgeOffset"),
				0,
				0,
				MAX_int32,
				EdgeOffset)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedNodes"),
				5000,
				1,
				20000,
				MaxScannedNodes)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedPins"),
				50000,
				1,
				200000,
				MaxScannedPins)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedLinks"),
				100000,
				1,
				500000,
				MaxScannedLinks))
		{
			return FMCPToolResult::Error(
				TEXT("Invalid execution-flow result or scan limit."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(Name, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				return DescribeBlueprintExecutionFlow(
					BP,
					Graph,
					EntryPoint,
					EntryNodeId,
					MaxNodes,
					MaxEdges,
					NodeOffset,
					EdgeOffset,
					MaxScannedNodes,
					MaxScannedPins,
					MaxScannedLinks);
			}
		}
		return FMCPToolResult::Error(FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
	}
};

// ============================================================
// find_asset_references
// ============================================================
class FTool_FindAssetReferences : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.references");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath = Params->GetStringField(TEXT("assetPath"));
		if (AssetPath.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'assetPath' parameter"));

		IAssetRegistry& Registry = *IAssetRegistry::Get();
		TArray<FName> Referencers;
		const FString TargetPackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
		Registry.GetReferencers(FName(*TargetPackagePath), Referencers);
		Referencers.Sort(FNameLexicalLess());
		const FAssetIdentifier TargetIdentifier =
			FAssetIdentifier::FromString(AssetPath);
		const bool bExactObjectQueryAttempted = TargetIdentifier.IsObject()
			|| TargetIdentifier.IsValue();
		TArray<FAssetIdentifier> ExactReferencers;
		bool bExactObjectReferenceAvailable =
			bExactObjectQueryAttempted
			&& Registry.GetReferencers(
				TargetIdentifier,
				ExactReferencers,
				UE::AssetRegistry::EDependencyCategory::All);
		ExactReferencers.Sort([](
			const FAssetIdentifier& Left,
			const FAssetIdentifier& Right)
		{
			return Left.ToString() < Right.ToString();
		});
		TSet<FString> ExactReferencerIdentities;
		TArray<TSharedPtr<FJsonValue>> ExactReferencerValues;
		for (const FAssetIdentifier& ExactReferencer : ExactReferencers)
		{
			const FString Identity = ExactReferencer.ToString();
			ExactReferencerIdentities.Add(Identity);
			ExactReferencerValues.Add(
				MakeShared<FJsonValueString>(Identity));
		}

		TArray<FAssetData> AllBP;
		Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBP, true);
		if (bExactObjectQueryAttempted && !bExactObjectReferenceAvailable)
		{
			if (CollectLoadedBlueprintObjectReferencers(
				AssetPath,
				AllBP,
				ExactReferencerIdentities))
			{
				bExactObjectReferenceAvailable = true;
			}
		}
		if (bExactObjectReferenceAvailable)
		{
			ExactReferencers.Reset();
			ExactReferencerValues.Reset();
			TArray<FString> SortedExactReferencers =
				ExactReferencerIdentities.Array();
			SortedExactReferencers.Sort();
			for (const FString& Identity : SortedExactReferencers)
			{
				ExactReferencers.Add(FAssetIdentifier::FromString(Identity));
				ExactReferencerValues.Add(
					MakeShared<FJsonValueString>(Identity));
			}
		}
		TSet<FString> BlueprintPackages;
		for (const FAssetData& A : AllBP)
			BlueprintPackages.Add(A.PackageName.ToString());

		TArray<TSharedPtr<FJsonValue>> BPRefs, OtherRefs;
		TArray<TSharedPtr<FJsonValue>> ReferenceRecords;
		TArray<FString> ReferenceIdentities;
		int32 CandidateReferenceCount = 0;
		int32 AmbiguousReferencerCount = 0;
		int32 UnresolvedReferencerCount = 0;
		bool bIdentityComplete = true;
		for (const FName& Ref : Referencers)
		{
			FString RefStr = Ref.ToString();
			if (BlueprintPackages.Contains(RefStr))
				BPRefs.Add(MakeShared<FJsonValueString>(RefStr));
			else
				OtherRefs.Add(MakeShared<FJsonValueString>(RefStr));

			// Package names alone are ambiguous when a package contains more than
			// one asset.  Return every candidate instead of silently choosing the
			// lexically first asset.  The package-level identityComplete flag stays
			// false whenever the actual source object cannot be uniquely resolved.
			TArray<FAssetData> PackageAssets;
			Registry.GetAssetsByPackageName(Ref, PackageAssets, true);
			PackageAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.GetSoftObjectPath().ToString()
					< Right.GetSoftObjectPath().ToString();
			});
			const int32 CandidateCount = PackageAssets.Num();
			CandidateReferenceCount += CandidateCount;
			const bool bAmbiguous = CandidateCount > 1;
			const bool bUnresolved = CandidateCount == 0;
			if (bAmbiguous)
			{
				++AmbiguousReferencerCount;
			}
			if (bUnresolved)
			{
				++UnresolvedReferencerCount;
			}
			bIdentityComplete &= CandidateCount == 1;

			const FString PackageKind = BlueprintPackages.Contains(RefStr)
				? TEXT("blueprint")
				: TEXT("asset");
			if (PackageAssets.IsEmpty())
			{
				// Keep one row for an unresolved package referencer, but do not put
				// the package path in objectPath: that would falsely claim object
				// identity and make identityComplete appear trustworthy.
				const FString StableIdentity = RefStr + TEXT("||");
				ReferenceIdentities.Add(StableIdentity);
				TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
				Record->SetStringField(TEXT("packagePath"), RefStr);
				Record->SetStringField(TEXT("objectPath"), FString());
				Record->SetStringField(TEXT("classPath"), FString());
				Record->SetStringField(TEXT("kind"), PackageKind);
				Record->SetStringField(TEXT("sourcePackagePath"), RefStr);
				Record->SetStringField(TEXT("sourceObjectPath"), FString());
				Record->SetStringField(TEXT("sourceClassPath"), FString());
				Record->SetStringField(TEXT("sourceIdentity"), StableIdentity);
				Record->SetStringField(TEXT("sourceAssetIdentifier"), FString());
				Record->SetBoolField(TEXT("sourceObjectReferenceExact"), false);
				Record->SetStringField(TEXT("identityStatus"), TEXT("unresolved"));
				Record->SetNumberField(TEXT("candidateCount"), 0);
				Record->SetBoolField(TEXT("ambiguous"), false);
				Record->SetBoolField(TEXT("identityComplete"), false);
				ReferenceRecords.Add(MakeShared<FJsonValueObject>(Record));
				continue;
			}

			for (const FAssetData& Candidate : PackageAssets)
			{
				const FString ObjectPath = Candidate.GetSoftObjectPath().ToString();
				const FString ClassPath = Candidate.AssetClassPath.ToString();
				const FAssetIdentifier CandidateIdentifier =
					FAssetIdentifier(Candidate.PackageName, Candidate.AssetName);
				const FString CandidateIdentity = CandidateIdentifier.ToString();
				const bool bSourceObjectReferenceExact =
					ExactReferencerIdentities.Contains(CandidateIdentity);
				const bool bCandidateIdentityComplete = !RefStr.IsEmpty()
					&& !ObjectPath.IsEmpty()
					&& !ClassPath.IsEmpty();
				const FString StableIdentity = RefStr + TEXT("|") + ObjectPath
					+ TEXT("|") + ClassPath;
				ReferenceIdentities.Add(StableIdentity);
				bIdentityComplete &= bCandidateIdentityComplete;

				TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
				Record->SetStringField(TEXT("packagePath"), RefStr);
				Record->SetStringField(TEXT("objectPath"), ObjectPath);
				Record->SetStringField(TEXT("classPath"), ClassPath);
				Record->SetStringField(TEXT("kind"), PackageKind);
				Record->SetStringField(TEXT("sourcePackagePath"), RefStr);
				Record->SetStringField(TEXT("sourceObjectPath"), ObjectPath);
				Record->SetStringField(TEXT("sourceClassPath"), ClassPath);
				Record->SetStringField(TEXT("sourceIdentity"), StableIdentity);
				Record->SetStringField(
					TEXT("sourceAssetIdentifier"), CandidateIdentity);
				Record->SetBoolField(
					TEXT("sourceObjectReferenceExact"),
					bSourceObjectReferenceExact);
				Record->SetStringField(
					TEXT("identityStatus"),
					bAmbiguous
						? TEXT("ambiguous")
						: (bCandidateIdentityComplete
							? TEXT("resolved")
							: TEXT("incomplete")));
				Record->SetNumberField(TEXT("candidateCount"), CandidateCount);
				Record->SetBoolField(TEXT("ambiguous"), bAmbiguous);
				Record->SetBoolField(
					TEXT("identityComplete"),
					bCandidateIdentityComplete && !bAmbiguous);
				ReferenceRecords.Add(MakeShared<FJsonValueObject>(Record));
			}
		}
		ReferenceIdentities.Sort();
		FString IdentityDigest;
		const FString CanonicalIdentities = FString::Join(ReferenceIdentities, TEXT("\n"));
		const FTCHARToUTF8 IdentityUtf8(*CanonicalIdentities);
		UEAIIntegration::Infrastructure::TrySha256Hex(
			IdentityUtf8.Get(), IdentityUtf8.Length(), IdentityDigest);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint.asset-references.v2"));
		Result->SetStringField(TEXT("assetPath"), AssetPath);
		Result->SetStringField(TEXT("targetPackagePath"), TargetPackagePath);
		Result->SetNumberField(TEXT("totalReferencers"), Referencers.Num());
		Result->SetNumberField(TEXT("blueprintReferencerCount"), BPRefs.Num());
		Result->SetArrayField(TEXT("blueprintReferencers"), BPRefs);
		Result->SetNumberField(TEXT("otherReferencerCount"), OtherRefs.Num());
		Result->SetArrayField(TEXT("otherReferencers"), OtherRefs);
		Result->SetArrayField(TEXT("references"), ReferenceRecords);
		Result->SetStringField(TEXT("targetIdentity"), TargetIdentifier.ToString());
		Result->SetBoolField(
			TEXT("exactObjectQueryAttempted"),
			bExactObjectQueryAttempted);
		Result->SetBoolField(
			TEXT("exactObjectReferenceAvailable"),
			bExactObjectReferenceAvailable);
		Result->SetNumberField(
			TEXT("exactObjectReferencerCount"),
			ExactReferencers.Num());
		Result->SetArrayField(
			TEXT("exactObjectReferencers"),
			ExactReferencerValues);
		Result->SetNumberField(TEXT("candidateReferenceCount"), CandidateReferenceCount);
		Result->SetNumberField(TEXT("ambiguousReferencerCount"), AmbiguousReferencerCount);
		Result->SetNumberField(TEXT("unresolvedReferencerCount"), UnresolvedReferencerCount);
		Result->SetStringField(TEXT("referenceIdentityDigest"), IdentityDigest);
		Result->SetBoolField(TEXT("identityComplete"), bIdentityComplete);
		Result->SetStringField(
			TEXT("identityStatus"),
			bIdentityComplete
				? TEXT("complete")
				: (AmbiguousReferencerCount > 0 ? TEXT("ambiguous") : TEXT("incomplete")));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// verify_runtime_instance — runtime acceptance boundary
// ============================================================
class FTool_VerifyBlueprintRuntimeInstance : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.runtime.verify");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("blueprint"), BlueprintInput)
			|| BlueprintInput.IsEmpty()
			|| BlueprintInput.Len() > 1024)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint must be a non-empty string of at most 1024 characters."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* Blueprint = MCPHelpers::LoadBlueprintByName(
			BlueprintInput,
			LoadError);
		if (!Blueprint)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("blueprint_not_found"),
				404);
		}
		if (!Blueprint->GeneratedClass)
		{
			return FMCPToolResult::Error(
				TEXT("The Blueprint has no generated class. Compile it before runtime acceptance."),
				TEXT("generated_class_unavailable"),
				409);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint.runtime-acceptance.v1"));
		Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("generatedClass"), Blueprint->GeneratedClass->GetPathName());
		Result->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
		Result->SetBoolField(TEXT("runtimeVerified"), false);
		Result->SetNumberField(TEXT("instanceCount"), 0);
		Result->SetNumberField(TEXT("runtimeWorldCount"), 0);
		Result->SetStringField(TEXT("worldType"), FString());
		Result->SetStringField(TEXT("worldIdentity"), FString());
		Result->SetStringField(TEXT("instanceClass"), FString());
		Result->SetStringField(
			TEXT("observedGeneratedClass"),
			Blueprint->GeneratedClass->GetPathName());
		Result->SetBoolField(TEXT("instanceIdentityComplete"), false);

		if (!Blueprint->GeneratedClass->IsChildOf(AActor::StaticClass()))
		{
			Result->SetStringField(TEXT("runtimeVerificationReason"), TEXT("unsupported_runtime_class"));
			Result->SetStringField(TEXT("scope"), TEXT("PIE actor instance acceptance; no CDO or compile-only inference"));
			return FMCPToolResult::Ok(Result);
		}

		int32 InstanceCount = 0;
		int32 RuntimeWorldCount = 0;
		FString ObservedWorld;
		FString ObservedInstance;
		FString ObservedWorldType;
		FString ObservedInstanceClass;
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (Context.WorldType != EWorldType::PIE
					&& Context.WorldType != EWorldType::Game)
				{
					continue;
				}
				UWorld* World = Context.World();
				if (!World)
				{
					continue;
				}
				++RuntimeWorldCount;
				const TCHAR* WorldType = Context.WorldType == EWorldType::PIE
					? TEXT("pie")
					: TEXT("game");
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor || !Actor->IsA(Blueprint->GeneratedClass))
					{
						continue;
					}
					++InstanceCount;
					if (ObservedWorld.IsEmpty())
					{
						ObservedWorld = World->GetPathName();
						ObservedInstance = Actor->GetPathName();
						ObservedWorldType = WorldType;
						ObservedInstanceClass = Actor->GetClass()->GetPathName();
					}
				}
			}
		}
		Result->SetNumberField(TEXT("instanceCount"), InstanceCount);
		Result->SetNumberField(TEXT("runtimeWorldCount"), RuntimeWorldCount);
		Result->SetBoolField(TEXT("runtimeVerified"), InstanceCount > 0);
		Result->SetStringField(
			TEXT("runtimeVerificationReason"),
			InstanceCount > 0
				? TEXT("runtime_instance_observed")
				: (RuntimeWorldCount > 0
					? TEXT("no_runtime_instance_observed")
					: TEXT("no_runtime_world")));
		if (!ObservedWorld.IsEmpty())
		{
			Result->SetStringField(TEXT("world"), ObservedWorld);
			Result->SetStringField(TEXT("instance"), ObservedInstance);
			Result->SetStringField(TEXT("worldType"), ObservedWorldType);
			Result->SetStringField(TEXT("worldIdentity"), ObservedWorld);
			Result->SetStringField(TEXT("instanceClass"), ObservedInstanceClass);
			Result->SetBoolField(
				TEXT("instanceIdentityComplete"),
				!ObservedInstance.IsEmpty() && !ObservedInstanceClass.IsEmpty());
		}
		Result->SetStringField(TEXT("scope"), TEXT("PIE actor instance acceptance; compile and asset persistence are reported separately"));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// search_by_type
// ============================================================
class FTool_SearchByType : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.search_by_type");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TypeName;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("typeName"), TypeName)
			|| TypeName.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("typeName must be a non-empty string."),
				TEXT("invalid_params"),
				422);
		}
		FString FilterStr;
		if (Params->HasField(TEXT("filter"))
			&& !Params->TryGetStringField(TEXT("filter"), FilterStr))
		{
			return FMCPToolResult::Error(
				TEXT("filter must be a string."),
				TEXT("invalid_params"),
				422);
		}
		int32 MaxResults = 200;
		int32 MaxConnectionsPerPin = 16;
		int32 MaxAssets = 500;
		int32 AssetOffset = 0;
		int32 MaxScannedVariables = 10000;
		int32 MaxScannedNodes = 5000;
		int32 MaxScannedPins = 50000;
		int32 MaxScannedLinks = 100000;
		if (!ReadBlueprintListPageInteger(
				Params,
				TEXT("maxResults"),
				200,
				1,
				500,
				MaxResults)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxConnectionsPerPin"),
				16,
				1,
				64,
				MaxConnectionsPerPin)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxAssets"),
				500,
				1,
				5000,
				MaxAssets)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("assetOffset"),
				0,
				0,
				MAX_int32,
				AssetOffset)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedVariables"),
				10000,
				1,
				100000,
				MaxScannedVariables)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedNodes"),
				5000,
				1,
				50000,
				MaxScannedNodes)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedPins"),
				50000,
				1,
				200000,
				MaxScannedPins)
			|| !ReadBlueprintListPageInteger(
				Params,
				TEXT("maxScannedLinks"),
				100000,
				1,
				500000,
				MaxScannedLinks))
		{
			return FMCPToolResult::Error(
				TEXT("Invalid result, asset-page, or source-scan limit."),
				TEXT("invalid_params"),
				422);
		}
		if (TypeName.Len() > 256 || FilterStr.Len() > 512)
		{
			return FMCPToolResult::Error(
				TEXT("typeName may contain at most 256 characters and filter at most 512."),
				TEXT("invalid_params"),
				422);
		}
		const FString TypeNameNoPrefix =
			TypeNameWithoutUnrealPrefix(TypeName);

		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> AllBP;
		Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), AllBP, true);
		AllBP.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			return Left.PackageName.LexicalLess(Right.PackageName);
		});
		TArray<const FAssetData*> CandidateAssets;
		for (const FAssetData& Asset : AllBP)
		{
			const FString Path = Asset.PackageName.ToString();
			const FString BPName = Asset.AssetName.ToString();
			if (FilterStr.IsEmpty()
				|| BPName.Contains(FilterStr, ESearchCase::IgnoreCase)
				|| Path.Contains(FilterStr, ESearchCase::IgnoreCase))
			{
				CandidateAssets.Add(&Asset);
			}
		}
		const int32 AssetStart = FMath::Min(
			AssetOffset,
			CandidateAssets.Num());
		const int32 AssetEnd = AssetStart + FMath::Min(
			MaxAssets,
			CandidateAssets.Num() - AssetStart);

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 ScannedBlueprintCount = 0;
		int32 SkippedLoadCount = 0;
		int32 ScannedVariableCount = 0;
		int32 ScannedNodeCount = 0;
		int32 ScannedPinCount = 0;
		int32 ScannedLinkCount = 0;
		bool bResultTruncated = false;
		bool bScanExhausted = false;
		auto AddResult = [&](const TSharedRef<FJsonObject>& Result)
		{
			if (Results.Num() >= MaxResults)
			{
				bResultTruncated = true;
				return false;
			}
			Results.Add(MakeShared<FJsonValueObject>(Result));
			return true;
		};

		for (int32 AssetIndex = AssetStart;
		     AssetIndex < AssetEnd;
		     ++AssetIndex)
		{
			if (bResultTruncated || bScanExhausted)
			{
				break;
			}

			const FAssetData& Asset = *CandidateAssets[AssetIndex];
			const FString Path = Asset.PackageName.ToString();
			const FString BPName = Asset.AssetName.ToString();

			UBlueprint* BP = Cast<UBlueprint>(const_cast<FAssetData&>(Asset).GetAsset());
			if (!BP)
			{
				++SkippedLoadCount;
				continue;
			}
			++ScannedBlueprintCount;

			const int32 RemainingVariableBudget = FMath::Max(
				0,
				MaxScannedVariables - ScannedVariableCount);
			const int32 VariableScanCount = FMath::Min(
				BP->NewVariables.Num(),
				RemainingVariableBudget);
			const bool bVariableScanExhausted =
				VariableScanCount < BP->NewVariables.Num();
			TArray<const FBPVariableDescription*> Variables;
			Variables.Reserve(VariableScanCount);
			for (int32 VariableIndex = 0;
			     VariableIndex < VariableScanCount;
			     ++VariableIndex)
			{
				++ScannedVariableCount;
				Variables.Add(&BP->NewVariables[VariableIndex]);
			}
			Variables.Sort([](
				const FBPVariableDescription& Left,
				const FBPVariableDescription& Right)
				{
					const FString LeftKey =
						Left.VarName.ToString() + TEXT("|") + Left.VarGuid.ToString();
					const FString RightKey =
						Right.VarName.ToString() + TEXT("|") + Right.VarGuid.ToString();
					return LeftKey < RightKey;
				});
			for (const FBPVariableDescription* Variable : Variables)
			{
				if (PinTypeMatchesRequestedType(
					Variable->VarType,
					TypeName,
					TypeNameNoPrefix))
				{
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetStringField(TEXT("blueprint"), BPName);
					R->SetStringField(TEXT("blueprintPath"), Path);
					R->SetStringField(TEXT("usage"), TEXT("variable"));
					R->SetStringField(
						TEXT("location"), Variable->VarName.ToString());
					R->SetStringField(
						TEXT("variableGuid"), Variable->VarGuid.ToString());
					AddPinTypeIdentity(Variable->VarType, R);
					if (!AddResult(R))
					{
						break;
					}
				}
			}
			bScanExhausted |= bVariableScanExhausted;
			if (bResultTruncated || bScanExhausted)
			{
				break;
			}

			TArray<UEdGraph*> Graphs;
			BP->GetAllGraphs(Graphs);
			Graphs.RemoveAll([](const UEdGraph* Graph)
			{
				return Graph == nullptr;
			});
			Graphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
			{
				const FString LeftKey =
					Left.GetName() + TEXT("|") + Left.GraphGuid.ToString();
				const FString RightKey =
					Right.GetName() + TEXT("|") + Right.GraphGuid.ToString();
				return LeftKey < RightKey;
			});
			for (UEdGraph* Graph : Graphs)
			{
				const int32 RemainingNodeBudget = FMath::Max(
					0,
					MaxScannedNodes - ScannedNodeCount);
				const int32 NodeSlotScanCount = FMath::Min(
					Graph->Nodes.Num(),
					RemainingNodeBudget);
				const bool bGraphNodeScanExhausted =
					NodeSlotScanCount < Graph->Nodes.Num();
				TArray<UEdGraphNode*> Nodes;
				Nodes.Reserve(NodeSlotScanCount);
				for (int32 NodeIndex = 0;
				     NodeIndex < NodeSlotScanCount;
				     ++NodeIndex)
				{
					++ScannedNodeCount;
					UEdGraphNode* Node = Graph->Nodes[NodeIndex];
					if (Node)
					{
						Nodes.Add(Node);
					}
				}
				Nodes.Sort([](
					const UEdGraphNode& Left,
					const UEdGraphNode& Right)
					{
						return Left.NodeGuid.ToString()
							< Right.NodeGuid.ToString();
					});
				for (UEdGraphNode* Node : Nodes)
				{
					const int32 RemainingPinBudget = FMath::Max(
						0,
						MaxScannedPins - ScannedPinCount);
					const int32 PinSlotScanCount = FMath::Min(
						Node->Pins.Num(),
						RemainingPinBudget);
					const bool bNodePinScanExhausted =
						PinSlotScanCount < Node->Pins.Num();
					TArray<UEdGraphPin*> Pins;
					Pins.Reserve(PinSlotScanCount);
					for (int32 PinIndex = 0;
					     PinIndex < PinSlotScanCount;
					     ++PinIndex)
					{
						++ScannedPinCount;
						UEdGraphPin* Pin = Node->Pins[PinIndex];
						if (Pin)
						{
							Pins.Add(Pin);
						}
					}
					Pins.Sort([](
						const UEdGraphPin& Left,
						const UEdGraphPin& Right)
						{
							const FString LeftKey =
								Left.PinName.ToString() + TEXT("|")
								+ FString::FromInt(Left.Direction) + TEXT("|")
								+ Left.PinId.ToString();
							const FString RightKey =
								Right.PinName.ToString() + TEXT("|")
								+ FString::FromInt(Right.Direction) + TEXT("|")
								+ Right.PinId.ToString();
							return LeftKey < RightKey;
						});
					for (UEdGraphPin* Pin : Pins)
					{
						if (!PinTypeMatchesRequestedType(
							Pin->PinType,
							TypeName,
							TypeNameNoPrefix))
						{
							continue;
						}

						auto MakePinResult = [&](const TCHAR* Usage)
						{
							TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
							R->SetStringField(TEXT("blueprint"), BPName);
							R->SetStringField(TEXT("blueprintPath"), Path);
							R->SetStringField(TEXT("usage"), Usage);
							R->SetStringField(TEXT("graph"), Graph->GetName());
							R->SetStringField(
								TEXT("graphId"), Graph->GraphGuid.ToString());
							R->SetStringField(
								TEXT("nodeId"), Node->NodeGuid.ToString());
							R->SetStringField(
								TEXT("nodeClass"), Node->GetClass()->GetPathName());
							R->SetStringField(
								TEXT("pinId"), Pin->PinId.ToString());
							R->SetStringField(
								TEXT("pinName"), Pin->PinName.ToString());
							R->SetStringField(
								TEXT("direction"),
								Pin->Direction == EGPD_Input
									? TEXT("input")
									: TEXT("output"));
							AddPinTypeIdentity(Pin->PinType, R);
							return R;
						};

						if (IsBlueprintParameterNode(Node)
							&& Pin->PinType.PinCategory
							!= UEdGraphSchema_K2::PC_Exec)
						{
							TSharedRef<FJsonObject> Parameter =
								MakePinResult(TEXT("parameter"));
							Parameter->SetStringField(
								TEXT("parameterDirection"),
								Node->IsA<UK2Node_FunctionResult>()
									? TEXT("output")
									: TEXT("input"));
							if (!AddResult(Parameter))
							{
								break;
							}
						}
						if (!Pin->LinkedTo.IsEmpty())
						{
							TSharedRef<FJsonObject> ConnectionResult =
								MakePinResult(TEXT("pinConnection"));
							const int32 RemainingLinkBudget = FMath::Max(
								0,
								MaxScannedLinks - ScannedLinkCount);
							const int32 LinkSlotScanCount = FMath::Min(
								Pin->LinkedTo.Num(),
								RemainingLinkBudget);
							const bool bPinLinkScanExhausted =
								LinkSlotScanCount < Pin->LinkedTo.Num();
							TArray<UEdGraphPin*> LinkedPins;
							LinkedPins.Reserve(LinkSlotScanCount);
							for (int32 LinkIndex = 0;
							     LinkIndex < LinkSlotScanCount;
							     ++LinkIndex)
							{
								++ScannedLinkCount;
								UEdGraphPin* LinkedPin =
									Pin->LinkedTo[LinkIndex];
								if (LinkedPin && LinkedPin->GetOwningNode())
								{
									LinkedPins.Add(LinkedPin);
								}
							}
							LinkedPins.Sort([](
								const UEdGraphPin& Left,
								const UEdGraphPin& Right)
								{
									const FString LeftKey =
										Left.GetOwningNode()->NodeGuid.ToString()
										+ TEXT("|") + Left.PinId.ToString();
									const FString RightKey =
										Right.GetOwningNode()->NodeGuid.ToString()
										+ TEXT("|") + Right.PinId.ToString();
									return LeftKey < RightKey;
								});
							TArray<TSharedPtr<FJsonValue>> Connections;
							const int32 ConnectionLimit = FMath::Min(
								MaxConnectionsPerPin,
								LinkedPins.Num());
							for (int32 Index = 0;
							     Index < ConnectionLimit;
							     ++Index)
							{
								UEdGraphPin* LinkedPin = LinkedPins[Index];
								TSharedRef<FJsonObject> Connection =
									MakeShared<FJsonObject>();
								Connection->SetStringField(
									TEXT("nodeId"),
									LinkedPin->GetOwningNode()->NodeGuid.ToString());
								Connection->SetStringField(
									TEXT("pinId"), LinkedPin->PinId.ToString());
								Connection->SetStringField(
									TEXT("pinName"),
									LinkedPin->PinName.ToString());
								Connections.Add(
									MakeShared<FJsonValueObject>(Connection));
							}
							ConnectionResult->SetNumberField(
								TEXT("connectionCount"), Pin->LinkedTo.Num());
							ConnectionResult->SetNumberField(
								TEXT("scannedValidConnectionCount"),
								LinkedPins.Num());
							ConnectionResult->SetBoolField(
								TEXT("connectionsTruncated"),
								bPinLinkScanExhausted
								|| LinkedPins.Num() > ConnectionLimit);
							ConnectionResult->SetArrayField(
								TEXT("connections"), Connections);
							if (!AddResult(ConnectionResult))
							{
								break;
							}
							bScanExhausted |= bPinLinkScanExhausted;
						}
						if (bResultTruncated || bScanExhausted)
						{
							break;
						}
					}
					bScanExhausted |= bNodePinScanExhausted;
					if (bResultTruncated || bScanExhausted)
					{
						break;
					}
				}
				bScanExhausted |= bGraphNodeScanExhausted;
				if (bResultTruncated || bScanExhausted)
				{
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("typeName"), TypeName);
		Result->SetNumberField(TEXT("resultCount"), Results.Num());
		Result->SetNumberField(TEXT("maxResults"), MaxResults);
		Result->SetNumberField(
			TEXT("maxConnectionsPerPin"), MaxConnectionsPerPin);
		Result->SetNumberField(TEXT("maxAssets"), MaxAssets);
		Result->SetNumberField(TEXT("assetOffset"), AssetOffset);
		Result->SetNumberField(
			TEXT("candidateAssetTotal"), CandidateAssets.Num());
		Result->SetNumberField(
			TEXT("scannedBlueprintCount"), ScannedBlueprintCount);
		Result->SetNumberField(TEXT("skippedLoadCount"), SkippedLoadCount);
		Result->SetNumberField(
			TEXT("scannedVariableCount"), ScannedVariableCount);
		Result->SetNumberField(TEXT("scannedNodeCount"), ScannedNodeCount);
		Result->SetNumberField(TEXT("scannedPinCount"), ScannedPinCount);
		Result->SetNumberField(TEXT("scannedLinkCount"), ScannedLinkCount);
		Result->SetNumberField(
			TEXT("maxScannedVariables"), MaxScannedVariables);
		Result->SetNumberField(TEXT("maxScannedNodes"), MaxScannedNodes);
		Result->SetNumberField(TEXT("maxScannedPins"), MaxScannedPins);
		Result->SetNumberField(TEXT("maxScannedLinks"), MaxScannedLinks);
		Result->SetBoolField(TEXT("resultTruncated"), bResultTruncated);
		Result->SetBoolField(TEXT("scanExhausted"), bScanExhausted);
		Result->SetBoolField(
			TEXT("partial"), bResultTruncated || bScanExhausted);
		const bool bAssetPageHasMore = AssetEnd < CandidateAssets.Num();
		Result->SetBoolField(TEXT("assetPageHasMore"), bAssetPageHasMore);
		Result->SetBoolField(
			TEXT("assetsHasMore"),
			bAssetPageHasMore);
		if (!bResultTruncated && !bScanExhausted && bAssetPageHasMore)
		{
			Result->SetNumberField(TEXT("nextAssetOffset"), AssetEnd);
		}
		Result->SetStringField(
			TEXT("resumeMode"),
			bResultTruncated || bScanExhausted
				? TEXT("restartWithNarrowerFilterOrHigherLimits")
				: bAssetPageHasMore
				? TEXT("nextAssetOffset")
				: TEXT("complete"));
		Result->SetArrayField(TEXT("results"), Results);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// get_blueprint_graph_export — full structural graph export (read-only)
// ============================================================
namespace
{
	constexpr int32 BlueprintGraphExportMaxNodes = 4096;
	constexpr int32 BlueprintGraphExportMaxPins = 16384;
	constexpr int32 BlueprintGraphExportMaxLinks = 16384;

	UEdGraph* ResolveBlueprintGraphExportTarget(
		UBlueprint* Blueprint,
		const FString& GraphInput)
	{
		if (!Blueprint)
		{
			return nullptr;
		}
		if (GraphInput.IsEmpty())
		{
			// Default to the primary event graph, then any authored graph.
			for (UEdGraph* Graph : Blueprint->UbergraphPages)
			{
				if (Graph)
				{
					return Graph;
				}
			}
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (Graph)
				{
					return Graph;
				}
			}
			return nullptr;
		}
		const FString DecodedGraph = MCPHelpers::UrlDecode(GraphInput);
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph
				&& (Graph->GetName().Equals(
						DecodedGraph, ESearchCase::IgnoreCase)
					|| Graph->GetPathName().Equals(
						DecodedGraph, ESearchCase::IgnoreCase)))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	struct FBlueprintGraphExportLink
	{
		FString FromPinId;
		FString ToPinId;
	};

	TSharedRef<FJsonObject> BuildBlueprintGraphExportResult(
		UBlueprint* Blueprint,
		UEdGraph* Graph)
	{
		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(Node);
			}
		}
		Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});

		const bool bNodesTruncated = Nodes.Num() > BlueprintGraphExportMaxNodes;
		const int32 NodeLimit = FMath::Min(
			Nodes.Num(),
			BlueprintGraphExportMaxNodes);

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		NodeValues.Reserve(NodeLimit);
		for (int32 NodeIndex = 0; NodeIndex < NodeLimit; ++NodeIndex)
		{
			UEdGraphNode* Node = Nodes[NodeIndex];
			FString Title = Node->GetNodeTitle(
				ENodeTitleType::ListView).ToString();
			if (Title.Len() > 512)
			{
				Title.LeftInline(512, false);
			}
			TSharedRef<FJsonObject> NodeJson = MakeShared<FJsonObject>();
			NodeJson->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
			NodeJson->SetStringField(
				TEXT("class"), Node->GetClass()->GetName());
			NodeJson->SetStringField(TEXT("title"), Title);
			NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
			NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
			NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);
			NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
		}

		TArray<TSharedPtr<FJsonValue>> PinValues;
		TArray<FBlueprintGraphExportLink> Links;
		TSet<FString> LinkKeys;
		int32 PinCount = 0;
		bool bPinsTruncated = false;
		bool bLinksTruncated = false;
		bool bPinOrLinkExhausted = false;
		for (int32 NodeIndex = 0;
		     NodeIndex < NodeLimit && !bPinOrLinkExhausted;
		     ++NodeIndex)
		{
			UEdGraphNode* Node = Nodes[NodeIndex];
			TArray<UEdGraphPin*> Pins;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin)
				{
					Pins.Add(Pin);
				}
			}
			Pins.Sort([](const UEdGraphPin& Left, const UEdGraphPin& Right)
			{
				const FString LeftId = Left.PinId.ToString();
				const FString RightId = Right.PinId.ToString();
				if (LeftId != RightId)
				{
					return LeftId < RightId;
				}
				if (Left.Direction != Right.Direction)
				{
					return static_cast<int32>(Left.Direction)
						< static_cast<int32>(Right.Direction);
				}
				return Left.PinName.ToString() < Right.PinName.ToString();
			});
			for (UEdGraphPin* Pin : Pins)
			{
				if (PinCount >= BlueprintGraphExportMaxPins)
				{
					bPinsTruncated = true;
					bPinOrLinkExhausted = true;
					break;
				}
				++PinCount;
				TSharedRef<FJsonObject> PinJson = MakeShared<FJsonObject>();
				PinJson->SetStringField(TEXT("id"), Pin->PinId.ToString());
				PinJson->SetStringField(
					TEXT("nodeId"), Node->NodeGuid.ToString());
				PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
				PinJson->SetStringField(
					TEXT("direction"),
					Pin->Direction == EGPD_Input
						? TEXT("input")
						: TEXT("output"));
				PinJson->SetStringField(
					TEXT("category"), Pin->PinType.PinCategory.ToString());
				PinJson->SetBoolField(
					TEXT("isLink"), !Pin->LinkedTo.IsEmpty());
				PinValues.Add(MakeShared<FJsonValueObject>(PinJson));

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}
					// Orient each link from the output (source) pin to the input
					// (destination) pin, matching the capture snapshot convention.
					UEdGraphPin* FromPin = nullptr;
					UEdGraphPin* ToPin = nullptr;
					if (Pin->Direction == EGPD_Output
						&& LinkedPin->Direction == EGPD_Input)
					{
						FromPin = Pin;
						ToPin = LinkedPin;
					}
					else if (Pin->Direction == EGPD_Input
						&& LinkedPin->Direction == EGPD_Output)
					{
						FromPin = LinkedPin;
						ToPin = Pin;
					}
					else
					{
						const FString PinId = Pin->PinId.ToString();
						const FString LinkedId = LinkedPin->PinId.ToString();
						if (PinId <= LinkedId)
						{
							FromPin = Pin;
							ToPin = LinkedPin;
						}
						else
						{
							FromPin = LinkedPin;
							ToPin = Pin;
						}
					}
					const FString LinkKey = FromPin->PinId.ToString()
						+ TEXT("|") + ToPin->PinId.ToString();
					if (LinkKeys.Contains(LinkKey))
					{
						continue;
					}
					if (Links.Num() >= BlueprintGraphExportMaxLinks)
					{
						bLinksTruncated = true;
						bPinOrLinkExhausted = true;
						break;
					}
					LinkKeys.Add(LinkKey);
					Links.Add(
						{
							FromPin->PinId.ToString(),
							ToPin->PinId.ToString()
						});
				}
				if (bPinOrLinkExhausted)
				{
					break;
				}
			}
		}

		Links.Sort([](
			const FBlueprintGraphExportLink& Left,
			const FBlueprintGraphExportLink& Right)
			{
				const FString LeftKey = Left.FromPinId + TEXT("|") + Left.ToPinId;
				const FString RightKey = Right.FromPinId + TEXT("|") + Right.ToPinId;
				return LeftKey < RightKey;
			});
		TArray<TSharedPtr<FJsonValue>> LinkValues;
		LinkValues.Reserve(Links.Num());
		for (const FBlueprintGraphExportLink& Link : Links)
		{
			TSharedRef<FJsonObject> LinkJson = MakeShared<FJsonObject>();
			LinkJson->SetStringField(TEXT("fromPinId"), Link.FromPinId);
			LinkJson->SetStringField(TEXT("toPinId"), Link.ToPinId);
			LinkValues.Add(MakeShared<FJsonValueObject>(LinkJson));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"), TEXT("ue.blueprint.graph-export.v1"));
		Result->SetStringField(TEXT("blueprint"), Blueprint->GetName());
		Result->SetStringField(
			TEXT("blueprintPath"), Blueprint->GetPathName());
		Result->SetStringField(TEXT("graph"), Graph->GetName());
		Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
		Result->SetNumberField(TEXT("nodeCount"), NodeValues.Num());
		Result->SetNumberField(TEXT("pinCount"), PinValues.Num());
		Result->SetNumberField(TEXT("linkCount"), LinkValues.Num());
		Result->SetArrayField(TEXT("nodes"), NodeValues);
		Result->SetArrayField(TEXT("pins"), PinValues);
		Result->SetArrayField(TEXT("links"), LinkValues);
		Result->SetBoolField(TEXT("nodesTruncated"), bNodesTruncated);
		Result->SetBoolField(TEXT("pinsTruncated"), bPinsTruncated);
		Result->SetBoolField(TEXT("linksTruncated"), bLinksTruncated);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compiled"), false);
		Result->SetStringField(
			TEXT("scope"),
			TEXT("authored structural export; runtime/debugger state not included"));
		return Result;
	}
} // namespace

class FTool_GetBlueprintGraphExport : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.export");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("blueprint"), BlueprintInput)
			|| BlueprintInput.IsEmpty()
			|| BlueprintInput.Len() > 1024)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint must be a non-empty string of at most 1024 characters."),
				TEXT("invalid_params"),
				422);
		}
		FString GraphInput;
		if ((Params->HasField(TEXT("graph"))
				&& !Params->TryGetStringField(TEXT("graph"), GraphInput))
			|| GraphInput.Len() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("graph must be a string of at most 256 characters."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* Blueprint = MCPHelpers::LoadBlueprintByName(
			BlueprintInput,
			LoadError);
		if (!Blueprint)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("blueprint_not_found"),
				404);
		}

		UEdGraph* Graph = ResolveBlueprintGraphExportTarget(
			Blueprint,
			GraphInput);
		if (!Graph)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Graph '%s' was not found in Blueprint '%s'."),
					GraphInput.IsEmpty()
						? TEXT("(primary event graph)")
						: *MCPHelpers::UrlDecode(GraphInput),
					*BlueprintInput),
				TEXT("graph_not_found"),
				404);
		}

		return FMCPToolResult::Ok(
			BuildBlueprintGraphExportResult(Blueprint, Graph));
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterBlueprintReadTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_ListBlueprints>());
		Registry.Register(MakeShared<FTool_GetBlueprint>());
		Registry.Register(MakeShared<FTool_GetBlueprintGraph>());
		Registry.Register(MakeShared<FTool_SearchBlueprints>());
		Registry.Register(MakeShared<FTool_GetBlueprintSummary>());
		Registry.Register(MakeShared<FTool_DescribeGraph>());
		Registry.Register(MakeShared<FTool_FindAssetReferences>());
		Registry.Register(MakeShared<FTool_VerifyBlueprintRuntimeInstance>());
		Registry.Register(MakeShared<FTool_SearchByType>());
		Registry.Register(MakeShared<FTool_GetBlueprintGraphExport>());
	}
}
