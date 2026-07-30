// Blueprint Editor layout tools — drive the native graph-editor selection and
// layout commands.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

namespace {
TMap<FString, TWeakPtr<SGraphEditor>> GraphEditorCache;

struct FBlueprintGraphEditorContext {
  UBlueprint *Blueprint = nullptr;
  UEdGraph *Graph = nullptr;
  TSharedPtr<IBlueprintEditor> BlueprintEditor;
  TSharedPtr<SGraphEditor> GraphEditor;
  FString BlueprintInput;
  FString GraphName;
  bool bEditorOpened = false;
};

FMCPToolResult InvalidRequest(const FString &Message) {
  return FMCPToolResult::Error(Message, TEXT("invalid_request"), 400);
}

FMCPToolResult NotFound(const FString &Message) {
  return FMCPToolResult::Error(Message, TEXT("not_found"), 404);
}

FMCPToolResult EditorUnavailable(const FString &Message) {
  return FMCPToolResult::Error(Message, TEXT("editor_context_unavailable"),
                               409);
}

UEdGraph *FindBlueprintGraph(UBlueprint *Blueprint,
                             const FString &EncodedGraphName) {
  if (!Blueprint) {
    return nullptr;
  }

  const FString GraphName = MCPHelpers::UrlDecode(EncodedGraphName);
  TArray<UEdGraph *> Graphs;
  Blueprint->GetAllGraphs(Graphs);
  for (UEdGraph *Graph : Graphs) {
    if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)) {
      return Graph;
    }
  }
  return nullptr;
}

FMCPToolResult ResolveBlueprintGraphContext(
    const TSharedPtr<FJsonObject> &Params,
    FBlueprintGraphEditorContext &OutContext) {
  if (!Params.IsValid()) {
    return InvalidRequest(TEXT("Request params are required."));
  }

  FString BlueprintInput;
  FString GraphInput;
  if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintInput) ||
      BlueprintInput.IsEmpty() ||
      !Params->TryGetStringField(TEXT("graph"), GraphInput) ||
      GraphInput.IsEmpty()) {
    return InvalidRequest(
        TEXT("Parameters 'blueprint' and 'graph' are required."));
  }

  FString LoadError;
  UBlueprint *Blueprint =
      MCPHelpers::LoadBlueprintByName(BlueprintInput, LoadError);
  if (!Blueprint) {
    return NotFound(LoadError);
  }

  UEdGraph *Graph = FindBlueprintGraph(Blueprint, GraphInput);
  if (!Graph) {
    return NotFound(
        FString::Printf(TEXT("Graph '%s' was not found in Blueprint '%s'."),
                        *MCPHelpers::UrlDecode(GraphInput), *BlueprintInput));
  }

  OutContext.Blueprint = Blueprint;
  OutContext.Graph = Graph;
  OutContext.BlueprintInput = MoveTemp(BlueprintInput);
  OutContext.GraphName = Graph->GetName();
  return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

FMCPToolResult ResolveGraphEditorContext(
    const bool bOpenEditor, FBlueprintGraphEditorContext &OutContext) {
  TSharedPtr<IBlueprintEditor> BlueprintEditor =
      FKismetEditorUtilities::GetIBlueprintEditorForObject(
          OutContext.Blueprint, false);
  const bool bEditorWasOpen = BlueprintEditor.IsValid();
  if (!BlueprintEditor.IsValid() && bOpenEditor) {
    BlueprintEditor = FKismetEditorUtilities::GetIBlueprintEditorForObject(
        OutContext.Blueprint, true);
  }
  if (!BlueprintEditor.IsValid()) {
    return EditorUnavailable(
        FString::Printf(TEXT("Blueprint Editor for '%s' is not open. "
                             "Call blueprint.selection.set first."),
                        *OutContext.BlueprintInput));
  }

  TSharedPtr<SGraphEditor> GraphEditor;
  const FString CacheKey = OutContext.Graph->GetPathName();
  if (bOpenEditor) {
    GraphEditor =
        BlueprintEditor->OpenGraphAndBringToFront(OutContext.Graph, true);
    if (GraphEditor.IsValid()) {
      GraphEditorCache.Add(CacheKey, GraphEditor);
    }
  } else {
    if (BlueprintEditor->GetFocusedGraph() != OutContext.Graph) {
      return EditorUnavailable(FString::Printf(
          TEXT("Graph '%s' is not the focused Graph in Blueprint '%s'. "
               "Call blueprint.selection.set first."),
          *OutContext.GraphName, *OutContext.BlueprintInput));
    }
    if (const TWeakPtr<SGraphEditor> *CachedEditor =
            GraphEditorCache.Find(CacheKey)) {
      GraphEditor = CachedEditor->Pin();
    }
  }
  if (!GraphEditor.IsValid() ||
      GraphEditor->GetCurrentGraph() != OutContext.Graph) {
    GraphEditorCache.Remove(CacheKey);
    return EditorUnavailable(FString::Printf(
        TEXT("Graph Editor context for '%s' is unavailable in Blueprint '%s'. "
             "Call blueprint.selection.set first."),
        *OutContext.GraphName, *OutContext.BlueprintInput));
  }

  OutContext.BlueprintEditor = MoveTemp(BlueprintEditor);
  OutContext.GraphEditor = MoveTemp(GraphEditor);
  OutContext.bEditorOpened = !bEditorWasOpen;
  return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

FMCPToolResult ResolveEditorContext(const TSharedPtr<FJsonObject> &Params,
                                    const bool bOpenEditor,
                                    FBlueprintGraphEditorContext &OutContext) {
  FMCPToolResult GraphResult =
      ResolveBlueprintGraphContext(Params, OutContext);
  if (!GraphResult.bSuccess) {
    return GraphResult;
  }
  return ResolveGraphEditorContext(bOpenEditor, OutContext);
}

TArray<UEdGraphNode *>
GetSelectedGraphNodes(const FBlueprintGraphEditorContext &Context) {
  TArray<UEdGraphNode *> SelectedNodes;
  if (!Context.GraphEditor.IsValid()) {
    return SelectedNodes;
  }

  for (UObject *SelectedObject : Context.GraphEditor->GetSelectedNodes()) {
    UEdGraphNode *Node = Cast<UEdGraphNode>(SelectedObject);
    if (Node && Node->GetGraph() == Context.Graph) {
      SelectedNodes.Add(Node);
    }
  }
  SelectedNodes.Sort([](const UEdGraphNode &A, const UEdGraphNode &B) {
    return A.NodeGuid.ToString() < B.NodeGuid.ToString();
  });
  return SelectedNodes;
}

TArray<TSharedPtr<FJsonValue>>
NodeIdsToJson(const TArray<UEdGraphNode *> &Nodes) {
  TArray<TSharedPtr<FJsonValue>> Values;
  Values.Reserve(Nodes.Num());
  for (const UEdGraphNode *Node : Nodes) {
    if (Node) {
      Values.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
    }
  }
  return Values;
}

TSharedRef<FJsonObject> PositionToJson(const int32 X, const int32 Y) {
  TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
  Position->SetNumberField(TEXT("x"), X);
  Position->SetNumberField(TEXT("y"), Y);
  return Position;
}

TSharedRef<FJsonObject> BoundsToJson(const UEdGraphNode_Comment *Comment) {
  TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
  Bounds->SetNumberField(TEXT("x"), Comment->NodePosX);
  Bounds->SetNumberField(TEXT("y"), Comment->NodePosY);
  Bounds->SetNumberField(TEXT("width"), Comment->NodeWidth);
  Bounds->SetNumberField(TEXT("height"), Comment->NodeHeight);
  return Bounds;
}

TMap<FGuid, FIntPoint> CapturePositions(const TArray<UEdGraphNode *> &Nodes) {
  TMap<FGuid, FIntPoint> Positions;
  for (const UEdGraphNode *Node : Nodes) {
    if (Node) {
      Positions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
    }
  }
  return Positions;
}

TSharedRef<FJsonObject>
BuildLayoutResult(const FBlueprintGraphEditorContext &Context,
                  const TArray<UEdGraphNode *> &SelectedNodes,
                  const TMap<FGuid, FIntPoint> &Before) {
  TArray<TSharedPtr<FJsonValue>> Changes;
  TArray<TSharedPtr<FJsonValue>> ChangedNodeIds;
  for (const UEdGraphNode *Node : SelectedNodes) {
    if (!Node) {
      continue;
    }

    const FIntPoint *BeforePosition = Before.Find(Node->NodeGuid);
    const FIntPoint EffectiveBefore =
        BeforePosition ? *BeforePosition
                       : FIntPoint(Node->NodePosX, Node->NodePosY);
    const bool bChanged = EffectiveBefore.X != Node->NodePosX ||
                          EffectiveBefore.Y != Node->NodePosY;

    TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
    Change->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
    Change->SetObjectField(
        TEXT("before"), PositionToJson(EffectiveBefore.X, EffectiveBefore.Y));
    Change->SetObjectField(TEXT("after"),
                           PositionToJson(Node->NodePosX, Node->NodePosY));
    Change->SetBoolField(TEXT("changed"), bChanged);
    Changes.Add(MakeShared<FJsonValueObject>(Change));

    if (bChanged) {
      ChangedNodeIds.Add(
          MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
    }
  }

  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetBoolField(TEXT("success"), true);
  Result->SetStringField(TEXT("blueprint"), Context.BlueprintInput);
  Result->SetStringField(TEXT("graph"), Context.GraphName);
  Result->SetArrayField(TEXT("selectedNodeIds"), NodeIdsToJson(SelectedNodes));
  Result->SetNumberField(TEXT("selectedCount"), SelectedNodes.Num());
  Result->SetArrayField(TEXT("changedNodeIds"), ChangedNodeIds);
  Result->SetNumberField(TEXT("changedCount"), ChangedNodeIds.Num());
  Result->SetArrayField(TEXT("positionChanges"), Changes);
  Result->SetBoolField(TEXT("saved"), false);
  return Result;
}

bool HasConnectionBetweenSelectedNodes(const TArray<UEdGraphNode *> &Nodes) {
  TSet<const UEdGraphNode *> SelectedSet;
  for (const UEdGraphNode *Node : Nodes) {
    if (Node) {
      SelectedSet.Add(Node);
    }
  }

  for (const UEdGraphNode *Node : Nodes) {
    if (!Node) {
      continue;
    }
    for (const UEdGraphPin *Pin : Node->Pins) {
      if (!Pin) {
        continue;
      }
      for (const UEdGraphPin *LinkedPin : Pin->LinkedTo) {
        if (LinkedPin && SelectedSet.Contains(LinkedPin->GetOwningNode())) {
          return true;
        }
      }
    }
  }
  return false;
}

class FTool_SetBlueprintSelection final : public FMCPToolBase {
public:
  FString GetCapabilityId() const override {
    return TEXT("blueprint.selection.set");
  }

  FMCPToolResult Execute(const TSharedPtr<FJsonObject> &Params) override {
    const TArray<TSharedPtr<FJsonValue>> *NodeIdValues = nullptr;
    if (!Params.IsValid() ||
        !Params->TryGetArrayField(TEXT("nodeIds"), NodeIdValues) ||
        !NodeIdValues) {
      return InvalidRequest(TEXT("Parameter 'nodeIds' is required."));
    }

    FBlueprintGraphEditorContext Context;
    FMCPToolResult ContextResult =
        ResolveBlueprintGraphContext(Params, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    TArray<UEdGraphNode *> NodesToSelect;
    NodesToSelect.Reserve(NodeIdValues->Num());
    TSet<FGuid> SeenNodeIds;
    for (const TSharedPtr<FJsonValue> &NodeIdValue : *NodeIdValues) {
      FString NodeId;
      if (!NodeIdValue.IsValid() || !NodeIdValue->TryGetString(NodeId) ||
          NodeId.IsEmpty()) {
        return InvalidRequest(TEXT("Every 'nodeIds' entry must be a "
                                   "non-empty Node GUID string."));
      }

      FGuid NodeGuid;
      if (!FGuid::Parse(NodeId, NodeGuid) || !NodeGuid.IsValid()) {
        return InvalidRequest(FString::Printf(
            TEXT("Node ID '%s' is not a valid GUID."), *NodeId));
      }
      if (SeenNodeIds.Contains(NodeGuid)) {
        return InvalidRequest(
            FString::Printf(TEXT("Node ID '%s' is duplicated."), *NodeId));
      }
      SeenNodeIds.Add(NodeGuid);

      UEdGraphNode *MatchingNode = nullptr;
      for (UEdGraphNode *Candidate : Context.Graph->Nodes) {
        if (Candidate && Candidate->NodeGuid == NodeGuid) {
          MatchingNode = Candidate;
          break;
        }
      }
      if (!MatchingNode) {
        UEdGraph *OtherGraph = nullptr;
        UEdGraphNode *OtherNode =
            MCPHelpers::FindNodeByGuid(Context.Blueprint, NodeId, &OtherGraph);
        if (OtherNode && OtherGraph) {
          return InvalidRequest(FString::Printf(
              TEXT("Node '%s' belongs to Graph '%s', "
                   "not '%s'."),
              *NodeId, *OtherGraph->GetName(), *Context.GraphName));
        }
        return NotFound(
            FString::Printf(TEXT("Node '%s' was not found in Blueprint "
                                 "'%s'."),
                            *NodeId, *Context.BlueprintInput));
      }
      NodesToSelect.Add(MatchingNode);
    }

    ContextResult = ResolveGraphEditorContext(true, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    // Selection changes only after every requested node has been validated.
    Context.GraphEditor->ClearSelectionSet();
    for (UEdGraphNode *Node : NodesToSelect) {
      Context.GraphEditor->SetNodeSelection(Node, true);
    }

    const TArray<UEdGraphNode *> SelectedNodes = GetSelectedGraphNodes(Context);
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("blueprint"), Context.BlueprintInput);
    Result->SetStringField(TEXT("graph"), Context.GraphName);
    Result->SetArrayField(TEXT("selectedNodeIds"),
                          NodeIdsToJson(SelectedNodes));
    Result->SetNumberField(TEXT("selectedCount"), SelectedNodes.Num());
    Result->SetBoolField(TEXT("editorOpened"), Context.bEditorOpened);
    Result->SetBoolField(TEXT("graphFocused"), true);
    return FMCPToolResult::Ok(Result);
  }
};

class FTool_AlignBlueprintSelection final : public FMCPToolBase {
public:
  FString GetCapabilityId() const override {
    return TEXT("blueprint.layout.align");
  }

  FMCPToolResult Execute(const TSharedPtr<FJsonObject> &Params) override {
    FString Alignment;
    if (!Params.IsValid() ||
        !Params->TryGetStringField(TEXT("alignment"), Alignment)) {
      return InvalidRequest(TEXT("Parameter 'alignment' is required."));
    }

    FBlueprintGraphEditorContext Context;
    FMCPToolResult ContextResult = ResolveEditorContext(Params, false, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    const TArray<UEdGraphNode *> SelectedNodes = GetSelectedGraphNodes(Context);
    if (SelectedNodes.Num() < 2) {
      return InvalidRequest(TEXT("blueprint.layout.align requires at least "
                                 "two selected nodes."));
    }

    const TMap<FGuid, FIntPoint> Before = CapturePositions(SelectedNodes);
    if (Alignment == TEXT("top")) {
      Context.GraphEditor->OnAlignTop();
    } else if (Alignment == TEXT("middle")) {
      Context.GraphEditor->OnAlignMiddle();
    } else if (Alignment == TEXT("bottom")) {
      Context.GraphEditor->OnAlignBottom();
    } else if (Alignment == TEXT("left")) {
      Context.GraphEditor->OnAlignLeft();
    } else if (Alignment == TEXT("center")) {
      Context.GraphEditor->OnAlignCenter();
    } else if (Alignment == TEXT("right")) {
      Context.GraphEditor->OnAlignRight();
    } else {
      return InvalidRequest(
          FString::Printf(TEXT("Unsupported alignment '%s'."), *Alignment));
    }

    TSharedRef<FJsonObject> Result =
        BuildLayoutResult(Context, SelectedNodes, Before);
    Result->SetStringField(TEXT("alignment"), Alignment);
    return FMCPToolResult::Ok(Result);
  }
};

class FTool_StraightenBlueprintSelection final : public FMCPToolBase {
public:
  FString GetCapabilityId() const override {
    return TEXT("blueprint.layout.straighten");
  }

  FMCPToolResult Execute(const TSharedPtr<FJsonObject> &Params) override {
    FBlueprintGraphEditorContext Context;
    FMCPToolResult ContextResult = ResolveEditorContext(Params, false, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    const TArray<UEdGraphNode *> SelectedNodes = GetSelectedGraphNodes(Context);
    if (SelectedNodes.Num() < 2 ||
        !HasConnectionBetweenSelectedNodes(SelectedNodes)) {
      return InvalidRequest(
          TEXT("blueprint.layout.straighten requires at "
               "least one connection between selected nodes."));
    }

    const TMap<FGuid, FIntPoint> Before = CapturePositions(SelectedNodes);
    Context.GraphEditor->OnStraightenConnections();
    return FMCPToolResult::Ok(
        BuildLayoutResult(Context, SelectedNodes, Before));
  }
};

class FTool_DistributeBlueprintSelection final : public FMCPToolBase {
public:
  FString GetCapabilityId() const override {
    return TEXT("blueprint.layout.distribute");
  }

  FMCPToolResult Execute(const TSharedPtr<FJsonObject> &Params) override {
    FString Orientation;
    if (!Params.IsValid() ||
        !Params->TryGetStringField(TEXT("orientation"), Orientation)) {
      return InvalidRequest(TEXT("Parameter 'orientation' is required."));
    }

    FBlueprintGraphEditorContext Context;
    FMCPToolResult ContextResult = ResolveEditorContext(Params, false, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    const TArray<UEdGraphNode *> SelectedNodes = GetSelectedGraphNodes(Context);
    if (SelectedNodes.Num() < 3) {
      return InvalidRequest(TEXT("blueprint.layout.distribute requires at "
                                 "least three selected nodes."));
    }

    const TMap<FGuid, FIntPoint> Before = CapturePositions(SelectedNodes);
    if (Orientation == TEXT("horizontal")) {
      Context.GraphEditor->OnDistributeNodesH();
    } else if (Orientation == TEXT("vertical")) {
      Context.GraphEditor->OnDistributeNodesV();
    } else {
      return InvalidRequest(
          FString::Printf(TEXT("Unsupported orientation '%s'."), *Orientation));
    }

    TSharedRef<FJsonObject> Result =
        BuildLayoutResult(Context, SelectedNodes, Before);
    Result->SetStringField(TEXT("orientation"), Orientation);
    return FMCPToolResult::Ok(Result);
  }
};

class FTool_CreateCommentFromSelection final : public FMCPToolBase {
public:
  FString GetCapabilityId() const override {
    return TEXT("blueprint.comment.create_from_selection");
  }

  FMCPToolResult Execute(const TSharedPtr<FJsonObject> &Params) override {
    FString Text;
    if (!Params.IsValid() || !Params->TryGetStringField(TEXT("text"), Text) ||
        Text.TrimStartAndEnd().IsEmpty()) {
      return InvalidRequest(TEXT("Parameter 'text' must not be empty."));
    }

    FBlueprintGraphEditorContext Context;
    FMCPToolResult ContextResult = ResolveEditorContext(Params, false, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    const TArray<UEdGraphNode *> WrappedNodes = GetSelectedGraphNodes(Context);
    if (WrappedNodes.IsEmpty()) {
      return InvalidRequest(TEXT("blueprint.comment.create_from_selection "
                                 "requires at least one selected node."));
    }

    FSlateRect NativeSelectionBounds;
    if (!Context.GraphEditor->GetBoundsForSelectedNodes(
            NativeSelectionBounds, 50.0f)) {
      return EditorUnavailable(
          TEXT("The focused Graph Editor could not calculate bounds for the "
               "current selection."));
    }

    FEdGraphSchemaAction_K2AddComment CommentAction;
    UEdGraphNode *NewNode = CommentAction.PerformAction(
        Context.Graph, nullptr, Context.GraphEditor->GetPasteLocation(), true);
    UEdGraphNode_Comment *Comment = Cast<UEdGraphNode_Comment>(NewNode);
    if (!Comment) {
      return FMCPToolResult::Error(
          TEXT("The native Blueprint comment action did "
               "not create a Comment node."));
    }

    Comment->Modify();
    // The K2 action asks the Blueprint editor's asynchronously updated focused
    // widget for the same bounds. Applying the result captured from our exact
    // SGraphEditor keeps immediate command chains deterministic while retaining
    // UE's native node-size and 50-unit padding calculation.
    Comment->SetBounds(NativeSelectionBounds);
    Comment->OnRenameNode(Text.TrimStartAndEnd());
    Comment->MoveMode = ECommentBoxMode::GroupMovement;
    Context.Blueprint->MarkPackageDirty();

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("blueprint"), Context.BlueprintInput);
    Result->SetStringField(TEXT("graph"), Context.GraphName);
    Result->SetStringField(TEXT("commentNodeId"), Comment->NodeGuid.ToString());
    Result->SetStringField(TEXT("text"), Comment->NodeComment);
    Result->SetStringField(TEXT("moveMode"), TEXT("group"));
    Result->SetObjectField(TEXT("bounds"), BoundsToJson(Comment));
    Result->SetArrayField(TEXT("wrappedNodeIds"), NodeIdsToJson(WrappedNodes));
    Result->SetBoolField(TEXT("saved"), false);
    return FMCPToolResult::Ok(Result);
  }
};

class FTool_SetCommentBounds final : public FMCPToolBase {
public:
  FString GetCapabilityId() const override {
    return TEXT("blueprint.comment.bounds.set");
  }

  FMCPToolResult Execute(const TSharedPtr<FJsonObject> &Params) override {
    FString CommentNodeId;
    double X = 0.0;
    double Y = 0.0;
    double Width = 0.0;
    double Height = 0.0;
    if (!Params.IsValid() ||
        !Params->TryGetStringField(TEXT("commentNodeId"), CommentNodeId) ||
        !Params->TryGetNumberField(TEXT("x"), X) ||
        !Params->TryGetNumberField(TEXT("y"), Y) ||
        !Params->TryGetNumberField(TEXT("width"), Width) ||
        !Params->TryGetNumberField(TEXT("height"), Height)) {
      return InvalidRequest(TEXT("Parameters 'commentNodeId', 'x', 'y', "
                                 "'width', and 'height' are required."));
    }
    const double MinInt32 =
        static_cast<double>(TNumericLimits<int32>::Lowest());
    const double MaxInt32 = static_cast<double>(TNumericLimits<int32>::Max());
    if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Width) ||
        !FMath::IsFinite(Height) || Width < 1.0 || Height < 1.0 ||
        X < MinInt32 || X > MaxInt32 || Y < MinInt32 || Y > MaxInt32 ||
        Width > MaxInt32 || Height > MaxInt32 || X + Width > MaxInt32 ||
        Y + Height > MaxInt32) {
      return InvalidRequest(TEXT("Comment bounds must be finite, positive, "
                                 "and fit in Graph int32 coordinates."));
    }

    FBlueprintGraphEditorContext Context;
    FMCPToolResult ContextResult = ResolveEditorContext(Params, false, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }

    UEdGraph *FoundGraph = nullptr;
    UEdGraphNode *FoundNode = MCPHelpers::FindNodeByGuid(
        Context.Blueprint, CommentNodeId, &FoundGraph);
    if (!FoundNode) {
      return NotFound(FString::Printf(TEXT("Comment node '%s' was not found."),
                                      *CommentNodeId));
    }
    if (FoundGraph != Context.Graph) {
      return InvalidRequest(FString::Printf(
          TEXT("Comment node '%s' belongs to Graph "
               "'%s', not '%s'."),
          *CommentNodeId, FoundGraph ? *FoundGraph->GetName() : TEXT(""),
          *Context.GraphName));
    }
    UEdGraphNode_Comment *Comment = Cast<UEdGraphNode_Comment>(FoundNode);
    if (!Comment) {
      return InvalidRequest(FString::Printf(
          TEXT("Node '%s' is not a Comment node."), *CommentNodeId));
    }

    const TSharedRef<FJsonObject> Before = BoundsToJson(Comment);
    const FScopedTransaction Transaction(
        NSLOCTEXT("UEAIIntegration", "SetBlueprintCommentBounds",
                  "Set Blueprint Comment Bounds"));
    Comment->Modify();
    Comment->SetBounds(FSlateRect(static_cast<float>(X), static_cast<float>(Y),
                                  static_cast<float>(X + Width),
                                  static_cast<float>(Y + Height)));
    Context.Blueprint->MarkPackageDirty();
    Context.GraphEditor->NotifyGraphChanged();

    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("success"), true);
    Result->SetStringField(TEXT("blueprint"), Context.BlueprintInput);
    Result->SetStringField(TEXT("graph"), Context.GraphName);
    Result->SetStringField(TEXT("commentNodeId"), Comment->NodeGuid.ToString());
    Result->SetObjectField(TEXT("before"), Before);
    Result->SetObjectField(TEXT("after"), BoundsToJson(Comment));
    Result->SetBoolField(TEXT("saved"), false);
    return FMCPToolResult::Ok(Result);
  }
};
} // namespace

namespace UEAIIntegrationTools {
void RegisterBlueprintEditorLayoutTools(FMCPToolRegistry &Registry) {
  Registry.Register(MakeShared<FTool_SetBlueprintSelection>());
  Registry.Register(MakeShared<FTool_AlignBlueprintSelection>());
  Registry.Register(MakeShared<FTool_StraightenBlueprintSelection>());
  Registry.Register(MakeShared<FTool_DistributeBlueprintSelection>());
  Registry.Register(MakeShared<FTool_CreateCommentFromSelection>());
  Registry.Register(MakeShared<FTool_SetCommentBounds>());
}
} // namespace UEAIIntegrationTools
