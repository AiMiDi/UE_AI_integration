// Atomic Blueprint Graph layout planning and application.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Domains/Blueprint/BlueprintGraphEditorSupport.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Misc/ScopeExit.h"
#include "SGraphPanel.h"
#include "ScopedTransaction.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWindow.h"
#include "Workflow/UEWorkflowExecutionContext.h"

namespace
{
using namespace UEAIIntegration::BlueprintGraph;

constexpr int32 MaxGroups = 32;
constexpr int32 MaxActionsPerGroup = 8;
constexpr int32 MaxNodesPerGroup = 128;
constexpr int32 MaxTotalNodes = 256;
constexpr const TCHAR* LayoutAlgorithmSemantics =
	TEXT("native-sgraph-editor-v1");

struct FLayoutAction
{
	FString Kind;
	FString Value;
};

struct FLayoutGroup
{
	FString Id;
	TArray<FGuid> NodeIds;
	TArray<FLayoutAction> Actions;
	bool bCreateComment = false;
	FString CommentText;
	double CommentPadding = 50.0;
};

struct FNodeSnapshot
{
	int32 X = 0;
	int32 Y = 0;
	int32 Width = 0;
	int32 Height = 0;
};

struct FLayoutExecution
{
	TArray<FGuid> CreatedCommentIds;
	TMap<FGuid, FString> CreatedCommentGroups;
	TArray<TSharedPtr<FJsonValue>> CommentPreviews;
};

struct FLayoutPrediction
{
	TArray<TSharedPtr<FJsonValue>> PositionChanges;
	TArray<TSharedPtr<FJsonValue>> CommentPreviews;
	TArray<TSharedPtr<FJsonValue>> LayoutDiagnostics;
	TSharedPtr<FJsonObject> GeometryFingerprint;
	FString PredictedLayoutHash;
};

UEdGraphNode_Comment* CreateNativeComment(
	UEdGraph* Graph,
	const FSlateRect& Bounds,
	const FString& Text)
{
	if (!Graph)
	{
		return nullptr;
	}

	UEdGraphNode_Comment* Comment =
		NewObject<UEdGraphNode_Comment>(
			Graph,
			NAME_None,
			RF_Transactional
				| (Graph->HasAnyFlags(RF_Transient) ? RF_Transient : RF_NoFlags));
	if (!Comment)
	{
		return nullptr;
	}
	Comment->CreateNewGuid();
	Comment->SetBounds(Bounds);
	Comment->OnRenameNode(Text);
	Comment->MoveMode = ECommentBoxMode::GroupMovement;
	Graph->AddNode(Comment, true, false);
	return Comment;
}

bool ParseGroups(
	const TSharedPtr<FJsonObject>& Params,
	TArray<FLayoutGroup>& OutGroups,
	TSharedPtr<FJsonObject>& OutNormalized,
	FString& OutError)
{
	OutGroups.Reset();
	OutNormalized = MakeShared<FJsonObject>();
	OutError.Reset();

	const TArray<TSharedPtr<FJsonValue>>* GroupValues = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetArrayField(TEXT("groups"), GroupValues)
		|| !GroupValues
		|| GroupValues->IsEmpty()
		|| GroupValues->Num() > MaxGroups)
	{
		OutError = TEXT("groups must contain between 1 and 32 entries.");
		return false;
	}

	TSet<FString> GroupIds;
	TSet<FGuid> AllNodeIds;
	TArray<TSharedPtr<FJsonValue>> NormalizedGroups;
	for (const TSharedPtr<FJsonValue>& GroupValue : *GroupValues)
	{
		const TSharedPtr<FJsonObject> GroupJson = GroupValue->AsObject();
		if (!GroupJson.IsValid())
		{
			OutError = TEXT("Every groups entry must be an object.");
			return false;
		}

		FLayoutGroup Group;
		if (!GroupJson->TryGetStringField(TEXT("id"), Group.Id)
			|| Group.Id.TrimStartAndEnd().IsEmpty()
			|| Group.Id.Len() > 64
			|| GroupIds.Contains(Group.Id))
		{
			OutError =
				TEXT("Every group requires a unique, non-empty id of at most "
					"64 characters.");
			return false;
		}
		Group.Id = Group.Id.TrimStartAndEnd();
		GroupIds.Add(Group.Id);

		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GroupJson->TryGetArrayField(TEXT("nodeIds"), NodeValues)
			|| !NodeValues
			|| NodeValues->IsEmpty()
			|| NodeValues->Num() > MaxNodesPerGroup)
		{
			OutError = FString::Printf(
				TEXT("Group '%s' nodeIds must contain between 1 and 128 "
					"entries."),
				*Group.Id);
			return false;
		}

		TArray<TSharedPtr<FJsonValue>> NormalizedNodeIds;
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			FString NodeId;
			FGuid NodeGuid;
			FString GuidError;
			if (!NodeValue.IsValid()
				|| !NodeValue->TryGetString(NodeId)
				|| !TryParseNodeGuid(NodeId, NodeGuid, GuidError))
			{
				OutError = GuidError.IsEmpty()
					? TEXT("Every nodeIds entry must be a Node GUID.")
					: GuidError;
				return false;
			}
			if (AllNodeIds.Contains(NodeGuid))
			{
				OutError = FString::Printf(
					TEXT("Node '%s' belongs to more than one group."),
					*NodeId);
				return false;
			}
			AllNodeIds.Add(NodeGuid);
			Group.NodeIds.Add(NodeGuid);
		}
		Group.NodeIds.Sort(
			[](const FGuid& Left, const FGuid& Right)
			{
				return Left.ToString() < Right.ToString();
			});
		for (const FGuid& NodeGuid : Group.NodeIds)
		{
			NormalizedNodeIds.Add(
				MakeShared<FJsonValueString>(NodeGuid.ToString()));
		}

		const TArray<TSharedPtr<FJsonValue>>* ActionValues = nullptr;
		if (GroupJson->TryGetArrayField(TEXT("actions"), ActionValues)
			&& ActionValues)
		{
			if (ActionValues->Num() > MaxActionsPerGroup)
			{
				OutError = FString::Printf(
					TEXT("Group '%s' is limited to 8 actions."),
					*Group.Id);
				return false;
			}
			for (const TSharedPtr<FJsonValue>& ActionValue : *ActionValues)
			{
				const TSharedPtr<FJsonObject> ActionJson =
					ActionValue->AsObject();
				FLayoutAction Action;
				if (!ActionJson.IsValid()
					|| !ActionJson->TryGetStringField(
						TEXT("kind"),
						Action.Kind))
				{
					OutError = TEXT("Every action requires kind.");
					return false;
				}
				if (Action.Kind == TEXT("align"))
				{
					if (!ActionJson->TryGetStringField(
							TEXT("alignment"),
							Action.Value)
						|| (Action.Value != TEXT("top")
							&& Action.Value != TEXT("middle")
							&& Action.Value != TEXT("bottom")
							&& Action.Value != TEXT("left")
							&& Action.Value != TEXT("center")
							&& Action.Value != TEXT("right")))
					{
						OutError =
							TEXT("align requires a supported alignment.");
						return false;
					}
				}
				else if (Action.Kind == TEXT("distribute"))
				{
					if (!ActionJson->TryGetStringField(
							TEXT("orientation"),
							Action.Value)
						|| (Action.Value != TEXT("horizontal")
							&& Action.Value != TEXT("vertical")))
					{
						OutError =
							TEXT("distribute requires horizontal or vertical "
								"orientation.");
						return false;
					}
				}
				else if (Action.Kind != TEXT("straighten"))
				{
					OutError = FString::Printf(
						TEXT("Unsupported action kind '%s'."),
						*Action.Kind);
					return false;
				}
				Group.Actions.Add(MoveTemp(Action));
			}
		}

		const TSharedPtr<FJsonObject>* Comment = nullptr;
		if (GroupJson->TryGetObjectField(TEXT("comment"), Comment)
			&& Comment
			&& Comment->IsValid())
		{
			Group.bCreateComment = true;
			if (!(*Comment)->TryGetStringField(
					TEXT("text"),
					Group.CommentText)
				|| Group.CommentText.TrimStartAndEnd().IsEmpty()
				|| Group.CommentText.Len() > 512)
			{
				OutError =
					TEXT("comment.text must contain between 1 and 512 "
						"characters.");
				return false;
			}
			Group.CommentText = Group.CommentText.TrimStartAndEnd();
			(*Comment)->TryGetNumberField(
				TEXT("padding"),
				Group.CommentPadding);
			if (!FMath::IsFinite(Group.CommentPadding)
				|| Group.CommentPadding < 0.0
				|| Group.CommentPadding > 512.0)
			{
				OutError =
					TEXT("comment.padding must be between 0 and 512.");
				return false;
			}
		}
		if (Group.Actions.IsEmpty() && !Group.bCreateComment)
		{
			OutError = FString::Printf(
				TEXT("Group '%s' must contain an action or comment."),
				*Group.Id);
			return false;
		}

		TSharedRef<FJsonObject> NormalizedGroup = MakeShared<FJsonObject>();
		NormalizedGroup->SetStringField(TEXT("id"), Group.Id);
		NormalizedGroup->SetArrayField(
			TEXT("nodeIds"),
			NormalizedNodeIds);
		TArray<TSharedPtr<FJsonValue>> NormalizedActions;
		for (const FLayoutAction& Action : Group.Actions)
		{
			TSharedRef<FJsonObject> NormalizedAction =
				MakeShared<FJsonObject>();
			NormalizedAction->SetStringField(TEXT("kind"), Action.Kind);
			if (Action.Kind == TEXT("align"))
			{
				NormalizedAction->SetStringField(
					TEXT("alignment"),
					Action.Value);
			}
			else if (Action.Kind == TEXT("distribute"))
			{
				NormalizedAction->SetStringField(
					TEXT("orientation"),
					Action.Value);
			}
			NormalizedActions.Add(
				MakeShared<FJsonValueObject>(NormalizedAction));
		}
		NormalizedGroup->SetArrayField(
			TEXT("actions"),
			NormalizedActions);
		if (Group.bCreateComment)
		{
			TSharedRef<FJsonObject> NormalizedComment =
				MakeShared<FJsonObject>();
			NormalizedComment->SetStringField(
				TEXT("text"),
				Group.CommentText);
			NormalizedComment->SetNumberField(
				TEXT("padding"),
				Group.CommentPadding);
			NormalizedGroup->SetObjectField(
				TEXT("comment"),
				NormalizedComment);
		}
		NormalizedGroups.Add(
			MakeShared<FJsonValueObject>(NormalizedGroup));
		OutGroups.Add(MoveTemp(Group));
	}

	if (AllNodeIds.Num() > MaxTotalNodes)
	{
		OutError = TEXT("A layout plan is limited to 256 unique nodes.");
		return false;
	}
	OutNormalized->SetArrayField(TEXT("groups"), NormalizedGroups);
	return true;
}

bool ResolveGroupNodes(
	UEdGraph* Graph,
	const FLayoutGroup& Group,
	TArray<UEdGraphNode*>& OutNodes,
	FString& OutError)
{
	OutNodes.Reset();
	for (const FGuid& NodeId : Group.NodeIds)
	{
		UEdGraphNode* Node = FindNode(Graph, NodeId);
		if (!Node)
		{
			OutError = FString::Printf(
				TEXT("Node '%s' was not found in graph '%s'."),
				*NodeId.ToString(),
				Graph ? *Graph->GetName() : TEXT(""));
			return false;
		}
		OutNodes.Add(Node);
	}
	return true;
}

bool TryBounds(
	const TSharedRef<SGraphEditor>& Editor,
	const UEdGraphNode* Node,
	FNodeBounds& OutBounds)
{
	FSlateRect Rect;
	if (Editor->GetBoundsForNode(Node, Rect, 0.0f)
		&& Rect.Right > Rect.Left
		&& Rect.Bottom > Rect.Top)
	{
		OutBounds.X = Rect.Left;
		OutBounds.Y = Rect.Top;
		OutBounds.Width = Rect.Right - Rect.Left;
		OutBounds.Height = Rect.Bottom - Rect.Top;
		OutBounds.Source = TEXT("editor");
		OutBounds.bExact = true;
		return true;
	}
	OutBounds.X = Node->NodePosX;
	OutBounds.Y = Node->NodePosY;
	OutBounds.Width = FMath::Max(0, Node->NodeWidth);
	OutBounds.Height = FMath::Max(0, Node->NodeHeight);
	OutBounds.Source = TEXT("stored");
	OutBounds.bExact = false;
	return false;
}

bool TryGetExactGroupBounds(
	const TSharedRef<SGraphEditor>& Editor,
	const TArray<UEdGraphNode*>& Nodes,
	const double Padding,
	FSlateRect& OutBounds)
{
	if (Nodes.IsEmpty() || !RefreshGraphEditorLayout(Editor))
	{
		return false;
	}

	double Left = TNumericLimits<double>::Max();
	double Top = TNumericLimits<double>::Max();
	double Right = TNumericLimits<double>::Lowest();
	double Bottom = TNumericLimits<double>::Lowest();
	for (const UEdGraphNode* Node : Nodes)
	{
		FNodeBounds Bounds;
		if (!TryBounds(Editor, Node, Bounds) || !Bounds.bExact)
		{
			return false;
		}
		Left = FMath::Min(Left, Bounds.X);
		Top = FMath::Min(Top, Bounds.Y);
		Right = FMath::Max(Right, Bounds.Right());
		Bottom = FMath::Max(Bottom, Bounds.Bottom());
	}

	// UEdGraphNode stores positions and Comment dimensions as integers. Round
	// outward so converting the native floating-point geometry cannot shave a
	// pixel from the right or bottom edge and create a false partial overlap.
	OutBounds.Left = FMath::FloorToDouble(Left - Padding);
	OutBounds.Top = FMath::FloorToDouble(Top - Padding);
	OutBounds.Right = FMath::CeilToDouble(Right + Padding);
	OutBounds.Bottom = FMath::CeilToDouble(Bottom + Padding);
	return OutBounds.Right > OutBounds.Left
		&& OutBounds.Bottom > OutBounds.Top;
}

bool AlignNodes(
	const TSharedRef<SGraphEditor>& Editor,
	const TArray<UEdGraphNode*>& Nodes,
	const FString& Alignment,
	FString& OutError)
{
	if (Nodes.Num() < 2)
	{
		OutError = TEXT("align requires at least two nodes.");
		return false;
	}
	if (Alignment == TEXT("top"))
	{
		Editor->OnAlignTop();
	}
	else if (Alignment == TEXT("middle"))
	{
		Editor->OnAlignMiddle();
	}
	else if (Alignment == TEXT("bottom"))
	{
		Editor->OnAlignBottom();
	}
	else if (Alignment == TEXT("left"))
	{
		Editor->OnAlignLeft();
	}
	else if (Alignment == TEXT("center"))
	{
		Editor->OnAlignCenter();
	}
	else if (Alignment == TEXT("right"))
	{
		Editor->OnAlignRight();
	}
	else
	{
		OutError = FString::Printf(
			TEXT("Unsupported alignment '%s'."),
			*Alignment);
		return false;
	}
	return true;
}

bool DistributeNodes(
	const TSharedRef<SGraphEditor>& Editor,
	TArray<UEdGraphNode*> Nodes,
	const FString& Orientation,
	FString& OutError)
{
	if (Nodes.Num() < 3)
	{
		OutError = TEXT("distribute requires at least three nodes.");
		return false;
	}
	if (Orientation == TEXT("horizontal"))
	{
		Editor->OnDistributeNodesH();
	}
	else if (Orientation == TEXT("vertical"))
	{
		Editor->OnDistributeNodesV();
	}
	else
	{
		OutError = FString::Printf(
			TEXT("Unsupported distribution orientation '%s'."),
			*Orientation);
		return false;
	}
	return true;
}

bool HasSelectedConnection(const TArray<UEdGraphNode*>& Nodes)
{
	TSet<const UEdGraphNode*> Selected;
	for (const UEdGraphNode* Node : Nodes)
	{
		Selected.Add(Node);
	}
	for (const UEdGraphNode* Node : Nodes)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked
					&& Selected.Contains(Linked->GetOwningNode()))
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool ExecuteGroups(
	const TSharedRef<SGraphEditor>& Editor,
	UEdGraph* Graph,
	const TArray<FLayoutGroup>& Groups,
	FLayoutExecution& OutExecution,
	FString& OutError)
{
	for (const FLayoutGroup& Group : Groups)
	{
		TArray<UEdGraphNode*> Nodes;
		if (!ResolveGroupNodes(Graph, Group, Nodes, OutError))
		{
			return false;
		}
		Editor->ClearSelectionSet();
		for (UEdGraphNode* Node : Nodes)
		{
			Editor->SetNodeSelection(Node, true);
		}

		for (const FLayoutAction& Action : Group.Actions)
		{
			if (Action.Kind == TEXT("align"))
			{
				if (!AlignNodes(Editor, Nodes, Action.Value, OutError))
				{
					return false;
				}
			}
			else if (Action.Kind == TEXT("distribute"))
			{
				if (!DistributeNodes(
						Editor,
						Nodes,
						Action.Value,
						OutError))
				{
					return false;
				}
			}
			else if (Action.Kind == TEXT("straighten"))
			{
				if (!HasSelectedConnection(Nodes))
				{
					OutError =
						TEXT("straighten requires a connection between "
							"selected nodes.");
					return false;
				}
				SGraphPanel* GraphPanel = Editor->GetGraphPanel();
				if (!GraphPanel)
				{
					OutError = TEXT("Graph panel is unavailable.");
					return false;
				}
				GraphPanel->StraightenConnections();
			}
			if (!RefreshGraphEditorLayout(Editor))
			{
				OutError =
					TEXT("The native Graph Editor did not settle after a "
						"layout command.");
				return false;
			}
		}

		if (Group.bCreateComment)
		{
			FSlateRect SelectionBounds;
			if (!TryGetExactGroupBounds(
					Editor,
					Nodes,
					Group.CommentPadding,
					SelectionBounds))
			{
				OutError =
					TEXT("Could not calculate Comment bounds for group '")
					+ Group.Id
					+ TEXT("'.");
				return false;
			}
			UEdGraphNode_Comment* Comment =
				CreateNativeComment(
					Graph,
					SelectionBounds,
					Group.CommentText);
			if (!Comment)
			{
				OutError =
					TEXT("Native Comment creation failed for group '")
					+ Group.Id
					+ TEXT("'.");
				return false;
			}
			OutExecution.CreatedCommentIds.Add(Comment->NodeGuid);
			OutExecution.CreatedCommentGroups.Add(
				Comment->NodeGuid,
				Group.Id);

			FNodeBounds Bounds;
			Bounds.X = Comment->NodePosX;
			Bounds.Y = Comment->NodePosY;
			Bounds.Width = Comment->NodeWidth;
			Bounds.Height = Comment->NodeHeight;
			Bounds.Source = TEXT("editor");
			Bounds.bExact = true;
			TSharedRef<FJsonObject> Preview = MakeShared<FJsonObject>();
			Preview->SetStringField(TEXT("groupId"), Group.Id);
			Preview->SetStringField(TEXT("text"), Group.CommentText);
			Preview->SetObjectField(TEXT("bounds"), BoundsToJson(Bounds));
			OutExecution.CommentPreviews.Add(
				MakeShared<FJsonValueObject>(Preview));
		}
		if (!RefreshGraphEditorLayout(Editor))
		{
			OutError =
				TEXT("The native Graph Editor did not settle after creating "
					"the group Comment.");
			return false;
		}
	}
	return true;
}

TMap<FGuid, FNodeSnapshot> CaptureNodeSnapshots(UEdGraph* Graph)
{
	TMap<FGuid, FNodeSnapshot> Snapshots;
	for (const UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		FNodeSnapshot Snapshot;
		Snapshot.X = Node->NodePosX;
		Snapshot.Y = Node->NodePosY;
		Snapshot.Width = Node->NodeWidth;
		Snapshot.Height = Node->NodeHeight;
		Snapshots.Add(Node->NodeGuid, Snapshot);
	}
	return Snapshots;
}

void RestoreSnapshot(
	UEdGraph* Graph,
	const TMap<FGuid, FNodeSnapshot>& Snapshots)
{
	TArray<UEdGraphNode*> AddedNodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && !Snapshots.Contains(Node->NodeGuid))
		{
			AddedNodes.Add(Node);
		}
	}
	for (UEdGraphNode* AddedNode : AddedNodes)
	{
		Graph->RemoveNode(AddedNode);
	}
	for (const TPair<FGuid, FNodeSnapshot>& Pair : Snapshots)
	{
		if (UEdGraphNode* Node = FindNode(Graph, Pair.Key))
		{
			Node->NodePosX = Pair.Value.X;
			Node->NodePosY = Pair.Value.Y;
			Node->NodeWidth = Pair.Value.Width;
			Node->NodeHeight = Pair.Value.Height;
		}
	}
	Graph->NotifyGraphChanged();
}

TArray<TSharedPtr<FJsonValue>> BuildPositionChanges(
	const TMap<FGuid, FNodeSnapshot>& Before,
	UEdGraph* AfterGraph)
{
	TArray<TSharedPtr<FJsonValue>> Changes;
	for (const TPair<FGuid, FNodeSnapshot>& Pair : Before)
	{
		const UEdGraphNode* Node = FindNode(AfterGraph, Pair.Key);
		if (!Node)
		{
			continue;
		}
		if (Node->NodePosX == Pair.Value.X
			&& Node->NodePosY == Pair.Value.Y)
		{
			continue;
		}
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("nodeId"), Pair.Key.ToString());
		TSharedRef<FJsonObject> BeforeJson = MakeShared<FJsonObject>();
		BeforeJson->SetNumberField(TEXT("x"), Pair.Value.X);
		BeforeJson->SetNumberField(TEXT("y"), Pair.Value.Y);
		TSharedRef<FJsonObject> AfterJson = MakeShared<FJsonObject>();
		AfterJson->SetNumberField(TEXT("x"), Node->NodePosX);
		AfterJson->SetNumberField(TEXT("y"), Node->NodePosY);
		Change->SetObjectField(TEXT("before"), BeforeJson);
		Change->SetObjectField(TEXT("after"), AfterJson);
		Changes.Add(MakeShared<FJsonValueObject>(Change));
	}
	Changes.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("nodeId"))
				< Right->AsObject()->GetStringField(TEXT("nodeId"));
		});
	return Changes;
}

TArray<TSharedPtr<FJsonValue>> BuildLayoutDiagnostics(
	const TSharedRef<SGraphEditor>& Editor,
	UEdGraph* Graph,
	const TArray<FLayoutGroup>& Groups,
	const FLayoutExecution& Execution)
{
	RefreshGraphEditorLayout(Editor);
	constexpr int32 MaxReturnedDiagnostics = 100;
	TSet<FGuid> AffectedIds;
	for (const FLayoutGroup& Group : Groups)
	{
		AffectedIds.Append(Group.NodeIds);
	}
	AffectedIds.Append(Execution.CreatedCommentIds);

	FContext Context;
	Context.Graph = Graph;
	Context.GraphEditor = Editor;
	TArray<UEdGraphNode*> Nodes;
	TMap<FGuid, FNodeBounds> Bounds;
	bool bHasIncompleteGeometry = false;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		FNodeBounds NodeBounds;
		TryGetNodeBounds(Context, Node, true, NodeBounds);
		bHasIncompleteGeometry |= !NodeBounds.bExact;
		Nodes.Add(Node);
		Bounds.Add(Node->NodeGuid, NodeBounds);
	}
	auto StableNodeId =
		[&Execution](const UEdGraphNode* Node)
		{
			if (!Node)
			{
				return FString();
			}
			if (const FString* GroupId =
					Execution.CreatedCommentGroups.Find(Node->NodeGuid))
			{
				return TEXT("planned-comment:") + *GroupId;
			}
			return Node->NodeGuid.ToString();
		};
	Nodes.Sort(
		[&StableNodeId](
			const UEdGraphNode& Left,
			const UEdGraphNode& Right)
		{
			return StableNodeId(&Left) < StableNodeId(&Right);
		});

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	auto Add =
		[&Diagnostics, &StableNodeId](
			const FString& Rule,
			const FString& Severity,
			const FString& Message,
			const UEdGraphNode* Left,
			const UEdGraphNode* Right)
		{
			if (Diagnostics.Num() >= MaxReturnedDiagnostics)
			{
				return;
			}
			TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
			Diagnostic->SetStringField(TEXT("rule"), Rule);
			Diagnostic->SetStringField(TEXT("severity"), Severity);
			Diagnostic->SetStringField(TEXT("message"), Message);
			TArray<TSharedPtr<FJsonValue>> NodeIds;
			if (Left)
			{
				NodeIds.Add(
					MakeShared<FJsonValueString>(
						StableNodeId(Left)));
			}
			if (Right)
			{
				NodeIds.Add(
					MakeShared<FJsonValueString>(
						StableNodeId(Right)));
			}
			Diagnostic->SetArrayField(TEXT("nodeIds"), NodeIds);
			Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
		};
	if (bHasIncompleteGeometry)
	{
		Add(
			TEXT("geometry_incomplete"),
			TEXT("warning"),
			TEXT("Some affected node bounds are not exact."),
			nullptr,
			nullptr);
	}

	for (int32 LeftIndex = 0; LeftIndex < Nodes.Num(); ++LeftIndex)
	{
		UEdGraphNode* LeftNode = Nodes[LeftIndex];
		const FNodeBounds& Left =
			Bounds.FindChecked(LeftNode->NodeGuid);
		if (!Left.bExact)
		{
			continue;
		}
		for (int32 RightIndex = LeftIndex + 1;
			RightIndex < Nodes.Num();
			++RightIndex)
		{
			UEdGraphNode* RightNode = Nodes[RightIndex];
			const bool bLeftAffected =
				AffectedIds.Contains(LeftNode->NodeGuid);
			const bool bRightAffected =
				AffectedIds.Contains(RightNode->NodeGuid);
			if (!bLeftAffected && !bRightAffected)
			{
				continue;
			}
			const FNodeBounds& Right =
				Bounds.FindChecked(RightNode->NodeGuid);
			if (!Right.bExact || !Intersects(Left, Right))
			{
				continue;
			}
			const bool bLeftComment =
				Cast<UEdGraphNode_Comment>(LeftNode) != nullptr;
			const bool bRightComment =
				Cast<UEdGraphNode_Comment>(RightNode) != nullptr;
			if (!bLeftComment && !bRightComment)
			{
				Add(
					TEXT("node_overlap"),
					TEXT("error"),
					TEXT("Affected Graph nodes overlap."),
					LeftNode,
					RightNode);
			}
			else if (bLeftComment && bRightComment)
			{
				if (!Contains(Left, Right) && !Contains(Right, Left))
				{
					Add(
						TEXT("comment_overlap"),
						TEXT("warning"),
						TEXT("Created or affected Comment boxes overlap "
							"without nesting."),
						LeftNode,
						RightNode);
				}
			}
			else
			{
				const FNodeBounds& CommentBounds =
					bLeftComment ? Left : Right;
				const FNodeBounds& NodeBounds =
					bLeftComment ? Right : Left;
				const bool bCommentAffected =
					bLeftComment ? bLeftAffected : bRightAffected;
				const bool bNodeAffected =
					bLeftComment ? bRightAffected : bLeftAffected;
				if (!Contains(CommentBounds, NodeBounds))
				{
					Add(
						TEXT("comment_partial_overlap"),
						TEXT("error"),
						TEXT("A Comment intersects an affected node without "
							"fully containing it."),
						LeftNode,
						RightNode);
				}
				else if (bCommentAffected && !bNodeAffected)
				{
					Add(
						TEXT("comment_contains_external_node"),
						TEXT("warning"),
						TEXT("An affected Comment fully contains a node "
							"outside the requested groups."),
						LeftNode,
						RightNode);
				}
				else if (!bCommentAffected && bNodeAffected)
				{
					Add(
						TEXT("affected_node_inside_external_comment"),
						TEXT("warning"),
						TEXT("An affected node is already contained by a "
							"Comment outside the requested groups."),
						LeftNode,
						RightNode);
				}
			}
		}
	}
	return Diagnostics;
}

bool BuildGeometryFingerprint(
	const TSharedRef<SGraphEditor>& Editor,
	UEdGraph* Graph,
	const FString& GraphHash,
	TSharedPtr<FJsonObject>& OutFingerprint,
	FString& OutError)
{
	TArray<UEdGraphNode*> Nodes;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Nodes.Add(Node);
		}
	}
	Nodes.Sort(
		[](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});

	TArray<TSharedPtr<FJsonValue>> BoundsValues;
	for (const UEdGraphNode* Node : Nodes)
	{
		FNodeBounds Bounds;
		if (!TryBounds(Editor, Node, Bounds) || !Bounds.bExact)
		{
			OutError = FString::Printf(
				TEXT("Exact Graph geometry is unavailable for node '%s'."),
				*Node->NodeGuid.ToString());
			return false;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
		Entry->SetNumberField(TEXT("x"), Bounds.X);
		Entry->SetNumberField(TEXT("y"), Bounds.Y);
		Entry->SetNumberField(TEXT("width"), Bounds.Width);
		Entry->SetNumberField(TEXT("height"), Bounds.Height);
		BoundsValues.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(
		TEXT("schema"),
		TEXT("ue.blueprint-layout-geometry-evidence.v1"));
	Evidence->SetStringField(
		TEXT("algorithmSemantics"),
		LayoutAlgorithmSemantics);
	Evidence->SetStringField(TEXT("graphHash"), GraphHash);
	Evidence->SetArrayField(TEXT("nodeBounds"), BoundsValues);
	FString BoundsDigest;
	if (!UEAIIntegration::Infrastructure::TryDigestJson(
			Evidence,
			BoundsDigest))
	{
		OutError = TEXT("Could not compute the Graph geometry fingerprint.");
		return false;
	}

	OutFingerprint = MakeShared<FJsonObject>();
	OutFingerprint->SetStringField(
		TEXT("schema"),
		TEXT("ue.blueprint-layout-geometry-fingerprint.v1"));
	OutFingerprint->SetStringField(
		TEXT("algorithmSemantics"),
		LayoutAlgorithmSemantics);
	OutFingerprint->SetStringField(TEXT("graphHash"), GraphHash);
	OutFingerprint->SetNumberField(TEXT("nodeCount"), Nodes.Num());
	OutFingerprint->SetBoolField(TEXT("exact"), true);
	OutFingerprint->SetStringField(
		TEXT("boundsHash"),
		TEXT("sha256:") + BoundsDigest);
	return true;
}

bool BuildLayoutPrediction(
	UBlueprint* SourceBlueprint,
	UEdGraph* SourceGraph,
	const FString& GraphHash,
	const TArray<FLayoutGroup>& Groups,
	FLayoutPrediction& OutPrediction,
	FString& OutError,
	FString& OutErrorCode)
{
	OutPrediction = FLayoutPrediction();
	OutError.Reset();
	OutErrorCode.Reset();
	if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
	{
		OutError =
			TEXT("The native Graph layout preview requires Slate.");
		OutErrorCode = TEXT("graph_geometry_unavailable");
		return false;
	}

	const FName PreviewOwnerName(
		*FString::Printf(
			TEXT("UEAILayoutPreviewOwner_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UBlueprint> PreviewOwner(
		NewObject<UBlueprint>(
			GetTransientPackage(),
			SourceBlueprint->GetClass(),
			PreviewOwnerName,
			RF_Transient));
	if (!PreviewOwner.IsValid())
	{
		OutError =
			TEXT("Could not create the transient Blueprint owner for the "
				"Graph preview.");
		return false;
	}
	// UK2Node_Event::PostDuplicate resolves its owning Blueprint and asserts
	// if a copied Graph is parented directly to the transient package.
	PreviewOwner->ParentClass = SourceBlueprint->ParentClass;
	PreviewOwner->BlueprintType = SourceBlueprint->BlueprintType;
	PreviewOwner->SkeletonGeneratedClass =
		SourceBlueprint->SkeletonGeneratedClass;
	PreviewOwner->GeneratedClass = SourceBlueprint->GeneratedClass;
	UEdGraph* PreviewGraph = Cast<UEdGraph>(
		StaticDuplicateObject(
			SourceGraph,
			PreviewOwner.Get(),
			SourceGraph->GetFName()));
	if (!PreviewGraph)
	{
		OutError = TEXT("Could not create the transient Graph preview.");
		return false;
	}
	PreviewGraph->SetFlags(RF_Transient);
	for (UEdGraphNode* PreviewNode : PreviewGraph->Nodes)
	{
		if (PreviewNode)
		{
			PreviewNode->SetFlags(RF_Transient);
		}
	}

	TSharedRef<SGraphEditor> PreviewEditor =
		SNew(SGraphEditor)
		.GraphToEdit(PreviewGraph)
		.IsEditable(true)
		.DisplayAsReadOnly(false);
	TSharedRef<SWindow> PreviewWindow =
		SNew(SWindow)
		.Type(EWindowType::Normal)
		.AutoCenter(EAutoCenter::None)
		.ScreenPosition(FVector2D(-32000.0f, -32000.0f))
		.ClientSize(FVector2D(1920.0f, 1080.0f))
		.AdjustInitialSizeAndPositionForDPIScale(false)
		.SizingRule(ESizingRule::FixedSize)
		.IsPopupWindow(true)
		.IsTopmostWindow(false)
		.FocusWhenFirstShown(false)
		.ActivationPolicy(EWindowActivationPolicy::Never)
		.UseOSWindowBorder(false)
		.HasCloseButton(false)
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.CreateTitleBar(false)
		.SaneWindowPlacement(false)
		[
			PreviewEditor
		];
	FSlateApplication& SlateApplication = FSlateApplication::Get();
	SlateApplication.AddWindow(PreviewWindow, true);
	ON_SCOPE_EXIT
	{
		SlateApplication.RequestDestroyWindow(PreviewWindow);
	};
	RefreshGraphEditorLayout(PreviewEditor);
	RefreshGraphEditorLayout(PreviewEditor);

	if (!BuildGeometryFingerprint(
			PreviewEditor,
			PreviewGraph,
			GraphHash,
			OutPrediction.GeometryFingerprint,
			OutError))
	{
		OutErrorCode = TEXT("graph_geometry_unavailable");
		return false;
	}

	const TMap<FGuid, FNodeSnapshot> Before =
		CaptureNodeSnapshots(PreviewGraph);
	FLayoutExecution PreviewExecution;
	if (!ExecuteGroups(
			PreviewEditor,
			PreviewGraph,
			Groups,
			PreviewExecution,
			OutError))
	{
		OutErrorCode = TEXT("invalid_request");
		return false;
	}
	RefreshGraphEditorLayout(PreviewEditor);

	OutPrediction.PositionChanges =
		BuildPositionChanges(Before, PreviewGraph);
	OutPrediction.CommentPreviews =
		PreviewExecution.CommentPreviews;
	OutPrediction.LayoutDiagnostics =
		BuildLayoutDiagnostics(
			PreviewEditor,
			PreviewGraph,
			Groups,
			PreviewExecution);

	TSharedRef<FJsonObject> PredictionEvidence = MakeShared<FJsonObject>();
	PredictionEvidence->SetStringField(
		TEXT("schema"),
		TEXT("ue.blueprint-layout-prediction.v1"));
	PredictionEvidence->SetStringField(
		TEXT("algorithmSemantics"),
		LayoutAlgorithmSemantics);
	PredictionEvidence->SetObjectField(
		TEXT("geometryFingerprint"),
		OutPrediction.GeometryFingerprint);
	PredictionEvidence->SetArrayField(
		TEXT("positionChanges"),
		OutPrediction.PositionChanges);
	PredictionEvidence->SetArrayField(
		TEXT("commentPreviews"),
		OutPrediction.CommentPreviews);
	PredictionEvidence->SetArrayField(
		TEXT("layoutDiagnostics"),
		OutPrediction.LayoutDiagnostics);
	FString PredictionDigest;
	if (!UEAIIntegration::Infrastructure::TryDigestJson(
			PredictionEvidence,
			PredictionDigest))
	{
		OutError = TEXT("Could not compute the predicted layout hash.");
		return false;
	}
	OutPrediction.PredictedLayoutHash =
		TEXT("sha256:") + PredictionDigest;
	return true;
}

bool ApplyLayoutPrediction(
	UEdGraph* Graph,
	const FLayoutPrediction& Prediction,
	FLayoutExecution& OutExecution,
	FString& OutError)
{
	const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
	if (!Graph || !Schema)
	{
		OutError = TEXT("Graph schema is unavailable.");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& ChangeValue :
		Prediction.PositionChanges)
	{
		const TSharedPtr<FJsonObject> Change = ChangeValue->AsObject();
		const TSharedPtr<FJsonObject>* After = nullptr;
		FString NodeId;
		double X = 0.0;
		double Y = 0.0;
		FGuid NodeGuid;
		FString GuidError;
		if (!Change.IsValid()
			|| !Change->TryGetStringField(TEXT("nodeId"), NodeId)
			|| !Change->TryGetObjectField(TEXT("after"), After)
			|| !After
			|| !After->IsValid()
			|| !(*After)->TryGetNumberField(TEXT("x"), X)
			|| !(*After)->TryGetNumberField(TEXT("y"), Y)
			|| !FMath::IsFinite(X)
			|| !FMath::IsFinite(Y)
			|| !TryParseNodeGuid(NodeId, NodeGuid, GuidError))
		{
			OutError = TEXT("The approved layout contains an invalid node "
				"position.");
			return false;
		}
		UEdGraphNode* Node = FindNode(Graph, NodeGuid);
		if (!Node)
		{
			OutError = FString::Printf(
				TEXT("Approved layout node '%s' no longer exists."),
				*NodeId);
			return false;
		}
		Schema->SetNodePosition(
			Node,
			FVector2D(
				FMath::RoundToInt(X),
				FMath::RoundToInt(Y)));
	}

	for (const TSharedPtr<FJsonValue>& CommentValue :
		Prediction.CommentPreviews)
	{
		const TSharedPtr<FJsonObject> CommentJson =
			CommentValue->AsObject();
		const TSharedPtr<FJsonObject>* BoundsJson = nullptr;
		FString GroupId;
		FString Text;
		double X = 0.0;
		double Y = 0.0;
		double Width = 0.0;
		double Height = 0.0;
		if (!CommentJson.IsValid()
			|| !CommentJson->TryGetStringField(TEXT("groupId"), GroupId)
			|| !CommentJson->TryGetStringField(TEXT("text"), Text)
			|| !CommentJson->TryGetObjectField(
				TEXT("bounds"),
				BoundsJson)
			|| !BoundsJson
			|| !BoundsJson->IsValid()
			|| !(*BoundsJson)->TryGetNumberField(TEXT("x"), X)
			|| !(*BoundsJson)->TryGetNumberField(TEXT("y"), Y)
			|| !(*BoundsJson)->TryGetNumberField(TEXT("width"), Width)
			|| !(*BoundsJson)->TryGetNumberField(TEXT("height"), Height)
			|| !FMath::IsFinite(X)
			|| !FMath::IsFinite(Y)
			|| !FMath::IsFinite(Width)
			|| !FMath::IsFinite(Height)
			|| Width <= 0.0
			|| Height <= 0.0)
		{
			OutError = TEXT("The approved layout contains invalid Comment "
				"bounds.");
			return false;
		}
		UEdGraphNode_Comment* Comment = CreateNativeComment(
			Graph,
			FSlateRect(X, Y, X + Width, Y + Height),
			Text);
		if (!Comment)
		{
			OutError = FString::Printf(
				TEXT("Native Comment creation failed for group '%s'."),
				*GroupId);
			return false;
		}
		OutExecution.CreatedCommentIds.Add(Comment->NodeGuid);
		OutExecution.CreatedCommentGroups.Add(
			Comment->NodeGuid,
			GroupId);
	}
	OutExecution.CommentPreviews = Prediction.CommentPreviews;
	Graph->NotifyGraphChanged();
	return true;
}

bool HasErrorDiagnostics(
	const TArray<TSharedPtr<FJsonValue>>& Diagnostics)
{
	for (const TSharedPtr<FJsonValue>& Value : Diagnostics)
	{
		const TSharedPtr<FJsonObject> Diagnostic = Value->AsObject();
		if (Diagnostic.IsValid()
			&& Diagnostic->GetStringField(TEXT("severity"))
				== TEXT("error"))
		{
			return true;
		}
	}
	return false;
}

void RestoreSelection(
	const TSharedRef<SGraphEditor>& Editor,
	UEdGraph* Graph,
	const TArray<FGuid>& OriginalSelection)
{
	// Native graph commands schedule a deferred panel refresh. Settle that
	// refresh before restoring the caller's selection, otherwise the pending
	// node-widget rebuild clears the just-restored selection on the next Slate
	// tick.
	RefreshGraphEditorLayout(Editor);
	Editor->ClearSelectionSet();
	for (const FGuid& NodeId : OriginalSelection)
	{
		if (UEdGraphNode* Node = FindNode(Graph, NodeId))
		{
			Editor->SetNodeSelection(Node, true);
		}
	}
}

class FTool_OrganizeBlueprintLayout final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.layout.organize");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		TArray<FLayoutGroup> Groups;
		TSharedPtr<FJsonObject> NormalizedGroups;
		FString ParseError;
		if (!ParseGroups(
				Params,
				Groups,
				NormalizedGroups,
				ParseError))
		{
			return InvalidRequest(ParseError);
		}

		FContext Context;
		FMCPToolResult ContextResult =
			ResolveBlueprintGraph(Params, Context);
		if (!ContextResult.bSuccess)
		{
			return ContextResult;
		}
		for (const FLayoutGroup& Group : Groups)
		{
			TArray<UEdGraphNode*> Nodes;
			FString ResolveError;
			if (!ResolveGroupNodes(
					Context.Graph,
					Group,
					Nodes,
					ResolveError))
			{
				return NotFound(ResolveError);
			}
		}

		const FString GraphHash = ComputeGraphHash(Context.Graph);
		FLayoutPrediction Prediction;
		FString PredictionError;
		FString PredictionErrorCode;
		if (!BuildLayoutPrediction(
				Context.Blueprint,
				Context.Graph,
				GraphHash,
				Groups,
				Prediction,
				PredictionError,
				PredictionErrorCode))
		{
			return FMCPToolResult::Error(
				PredictionError,
				PredictionErrorCode.IsEmpty()
					? TEXT("layout_prediction_failed")
					: PredictionErrorCode,
				PredictionErrorCode
						== TEXT("graph_geometry_unavailable")
					? 409
					: 422);
		}

		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(
			TEXT("schema"),
			TEXT("ue.blueprint-layout-plan.v1"));
		Plan->SetStringField(
			TEXT("algorithmSemantics"),
			LayoutAlgorithmSemantics);
		Plan->SetStringField(
			TEXT("blueprint"),
			Context.Blueprint->GetPathName());
		Plan->SetStringField(TEXT("graph"), Context.GraphName);
		Plan->SetStringField(TEXT("expectedGraphHash"), GraphHash);
		Plan->SetArrayField(
			TEXT("groups"),
			NormalizedGroups->GetArrayField(TEXT("groups")));
		Plan->SetStringField(
			TEXT("predictedLayoutHash"),
			Prediction.PredictedLayoutHash);
		Plan->SetObjectField(
			TEXT("geometryFingerprint"),
			Prediction.GeometryFingerprint);
		FString Digest;
		if (!UEAIIntegration::Infrastructure::TryDigestJson(Plan, Digest))
		{
			return FMCPToolResult::Error(
				TEXT("Could not compute layout plan digest."));
		}
		const FString PlanDigest = TEXT("sha256:") + Digest;

		const bool bApprovedWorkflowExecution =
			UEAIIntegration::Workflow::IsApprovedWorkflowExecution(Params);
		bool bDryRun = true;
		Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
		// A Workflow editStep must never silently turn into a successful
		// preview. The outer Workflow digest approves the multi-step workflow,
		// while the organizer digest separately approves the exact native
		// coordinates calculated against this Graph hash. Both are required.
		if (bApprovedWorkflowExecution)
		{
			bDryRun = false;
		}
		if (!bDryRun)
		{
			FString ExpectedHash;
			FString ApprovedDigest;
			const bool bHasExpectedHash = Params->TryGetStringField(
				TEXT("expectedGraphHash"),
				ExpectedHash);
			const bool bHasApprovedDigest = Params->TryGetStringField(
				TEXT("approvePlanDigest"),
				ApprovedDigest);
			if (!bHasExpectedHash || ExpectedHash != GraphHash)
			{
				return FMCPToolResult::Error(
					TEXT("expectedGraphHash does not match the current graph."),
					TEXT("graph_hash_mismatch"),
					409);
			}
			if (!bHasApprovedDigest || ApprovedDigest != PlanDigest)
			{
				return FMCPToolResult::Error(
					TEXT("approvePlanDigest does not match the current layout "
						"plan."),
					TEXT("plan_digest_mismatch"),
					409);
			}
		}

		if (bDryRun)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("schema"),
				TEXT("ue.blueprint-layout-preview.v1"));
			Result->SetBoolField(TEXT("dryRun"), true);
			Result->SetBoolField(
				TEXT("changed"),
				!Prediction.PositionChanges.IsEmpty()
					|| !Prediction.CommentPreviews.IsEmpty());
			Result->SetStringField(TEXT("graphHash"), GraphHash);
			Result->SetStringField(TEXT("planDigest"), PlanDigest);
			Result->SetStringField(
				TEXT("predictedLayoutHash"),
				Prediction.PredictedLayoutHash);
			Result->SetObjectField(
				TEXT("geometryFingerprint"),
				Prediction.GeometryFingerprint);
			Result->SetObjectField(TEXT("plan"), Plan);
			Result->SetNumberField(
				TEXT("changedNodeCount"),
				Prediction.PositionChanges.Num());
			Result->SetArrayField(
				TEXT("positionChanges"),
				Prediction.PositionChanges);
			Result->SetArrayField(
				TEXT("commentPreviews"),
				Prediction.CommentPreviews);
			Result->SetNumberField(
				TEXT("layoutDiagnosticCount"),
				Prediction.LayoutDiagnostics.Num());
			Result->SetArrayField(
				TEXT("layoutDiagnostics"),
				Prediction.LayoutDiagnostics);
			Result->SetBoolField(TEXT("assetModified"), false);
			Result->SetBoolField(TEXT("undoStackModified"), false);
			return FMCPToolResult::Ok(Result);
		}

		ContextResult = ResolveGraphEditor(Context, true);
		if (!ContextResult.bSuccess)
		{
			return ContextResult;
		}
		Context.GraphEditor->SlatePrepass(1.0f);
		TArray<FGuid> OriginalSelection;
		for (UObject* Selected : Context.GraphEditor->GetSelectedNodes())
		{
			if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Selected))
			{
				OriginalSelection.Add(Node->NodeGuid);
			}
		}
		const TMap<FGuid, FNodeSnapshot> Before =
			CaptureNodeSnapshots(Context.Graph);
		const bool bWasDirty = Context.Blueprint->GetOutermost()->IsDirty();
		FLayoutExecution Execution;
		FString ExecuteError;
		if (GEditor
			&& GEditor->IsTransactionActive()
			&& !bApprovedWorkflowExecution)
		{
			return FMCPToolResult::Error(
				TEXT("Another Editor transaction is active. Retry after it "
					"finishes or execute the approved operation through UE "
					"Workflow."),
				TEXT("editor_transaction_busy"),
				409);
		}
		const bool bOwnTransaction =
			!GEditor || !GEditor->IsTransactionActive();
		FScopedTransaction Transaction(
			NSLOCTEXT(
				"UEAIIntegration",
				"OrganizeBlueprintLayout",
				"Organize Blueprint Layout"),
			bOwnTransaction);
		Context.Blueprint->Modify();
		Context.Graph->Modify();
		Context.GraphEditor->ClearSelectionSet();
		TSet<FGuid> AppliedSelection;
		for (const FLayoutGroup& Group : Groups)
		{
			for (const FGuid& NodeId : Group.NodeIds)
			{
				if (!AppliedSelection.Contains(NodeId))
				{
					if (UEdGraphNode* Node =
							FindNode(Context.Graph, NodeId))
					{
						Context.GraphEditor->SetNodeSelection(Node, true);
						AppliedSelection.Add(NodeId);
					}
				}
			}
		}
		auto RestoreFailedApply =
			[&]()
			{
				RestoreSnapshot(Context.Graph, Before);
				RestoreSelection(
					Context.GraphEditor.ToSharedRef(),
					Context.Graph,
					OriginalSelection);
				Context.Blueprint->GetOutermost()->SetDirtyFlag(bWasDirty);
				if (bOwnTransaction)
				{
					Transaction.Cancel();
				}
				return ComputeGraphHash(Context.Graph) == GraphHash;
			};
		if (!ApplyLayoutPrediction(
				Context.Graph,
				Prediction,
				Execution,
				ExecuteError))
		{
			if (!RestoreFailedApply())
			{
				return FMCPToolResult::Error(
					TEXT("Layout failed and the graph snapshot could not be "
						"verified after restoration."),
					TEXT("verification_failed"),
					500);
			}
			return FMCPToolResult::Error(
				ExecuteError,
				TEXT("layout_failed"),
				422);
		}

		Context.GraphEditor->NotifyGraphChanged();
		RefreshGraphEditorLayout(Context.GraphEditor);
		TArray<TSharedPtr<FJsonValue>> LayoutDiagnostics =
			BuildLayoutDiagnostics(
				Context.GraphEditor.ToSharedRef(),
				Context.Graph,
				Groups,
				Execution);
		if (HasErrorDiagnostics(LayoutDiagnostics))
		{
			if (!RestoreFailedApply())
			{
				return FMCPToolResult::Error(
					TEXT("Layout verification failed and the graph snapshot "
						"could not be restored."),
					TEXT("verification_failed"),
					500);
			}
			return FMCPToolResult::Error(
				TEXT("The approved layout intersects nodes or Comments "
					"outside its groups. The complete graph snapshot was "
					"restored."),
				TEXT("layout_validation_failed"),
				422);
		}

		const FString AfterHash = ComputeGraphHash(Context.Graph);
		const bool bChanged = AfterHash != GraphHash;
		if (bChanged)
		{
			Context.Blueprint->MarkPackageDirty();
		}
		else
		{
			Context.Blueprint->GetOutermost()->SetDirtyFlag(bWasDirty);
			if (bOwnTransaction)
			{
				Transaction.Cancel();
			}
		}
		TArray<TSharedPtr<FJsonValue>> PositionChanges =
			BuildPositionChanges(Before, Context.Graph);
		TArray<TSharedPtr<FJsonValue>> CreatedCommentIds;
		for (const FGuid& CommentId : Execution.CreatedCommentIds)
		{
			CreatedCommentIds.Add(
				MakeShared<FJsonValueString>(CommentId.ToString()));
		}
		RestoreSelection(
			Context.GraphEditor.ToSharedRef(),
			Context.Graph,
			OriginalSelection);

		TSharedRef<FJsonObject> Mutation = MakeShared<FJsonObject>();
		Mutation->SetBoolField(
			TEXT("changed"),
			bChanged);
		Mutation->SetBoolField(TEXT("compiled"), false);
		Mutation->SetField(
			TEXT("saved"),
			MakeShared<FJsonValueNull>());
		Mutation->SetField(
			TEXT("reloaded"),
			MakeShared<FJsonValueNull>());
		Mutation->SetBoolField(TEXT("verified"), true);
		Mutation->SetStringField(TEXT("beforeHash"), GraphHash);
		Mutation->SetStringField(TEXT("afterHash"), AfterHash);
		Mutation->SetArrayField(
			TEXT("warnings"),
			TArray<TSharedPtr<FJsonValue>>());
		Mutation->SetArrayField(
			TEXT("errors"),
			TArray<TSharedPtr<FJsonValue>>());

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.blueprint-layout-result.v1"));
		Result->SetBoolField(TEXT("dryRun"), false);
		Result->SetStringField(TEXT("planDigest"), PlanDigest);
		Result->SetStringField(
			TEXT("predictedLayoutHash"),
			Prediction.PredictedLayoutHash);
		Result->SetObjectField(
			TEXT("geometryFingerprint"),
			Prediction.GeometryFingerprint);
		Result->SetStringField(
			TEXT("approvalSource"),
			bApprovedWorkflowExecution
				? TEXT("workflowPreparedPlan")
				: TEXT("layoutPlanDigest"));
		Result->SetStringField(TEXT("beforeGraphHash"), GraphHash);
		Result->SetStringField(TEXT("afterGraphHash"), AfterHash);
		Result->SetNumberField(
			TEXT("changedNodeCount"),
			PositionChanges.Num());
		Result->SetArrayField(
			TEXT("positionChanges"),
			PositionChanges);
		Result->SetArrayField(
			TEXT("createdCommentIds"),
			CreatedCommentIds);
		Result->SetNumberField(
			TEXT("layoutDiagnosticCount"),
			LayoutDiagnostics.Num());
		Result->SetArrayField(
			TEXT("layoutDiagnostics"),
			LayoutDiagnostics);
		Result->SetObjectField(TEXT("mutation"), Mutation);
		return FMCPToolResult::Ok(Result);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintLayoutOrganizeTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_OrganizeBlueprintLayout>());
}
}
