// Blueprint Editor layout tools — drive the native graph-editor selection and
// layout commands.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "BlueprintEditorModule.h"
#include "Domains/Blueprint/BlueprintGraphEditorSupport.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"

namespace {
using FBlueprintGraphEditorContext =
    UEAIIntegration::BlueprintGraph::FContext;

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

FMCPToolResult ResolveBlueprintGraphContext(
    const TSharedPtr<FJsonObject> &Params,
    FBlueprintGraphEditorContext &OutContext) {
  return UEAIIntegration::BlueprintGraph::ResolveBlueprintGraph(
      Params, OutContext);
}

FMCPToolResult ResolveGraphEditorContext(
    const bool bOpenEditor, FBlueprintGraphEditorContext &OutContext) {
  return UEAIIntegration::BlueprintGraph::ResolveGraphEditor(
      OutContext,
      bOpenEditor,
      !bOpenEditor);
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

constexpr double MaxStraightenNodeDisplacement = 2048.0;
constexpr double MaxStraightenRegionExpansion = 2.0;
constexpr double MaxStraightenConnectionSpanGrowth = 4.0;
constexpr double ClearConnectionDirectionThreshold = 16.0;

struct FNodeLayoutSnapshot {
  int32 X = 0;
  int32 Y = 0;
  int32 Width = 0;
  int32 Height = 0;
};

struct FStraightenConnectionSnapshot {
  FGuid SourceNodeId;
  FGuid TargetNodeId;
  bool bHorizontal = true;
  int32 DirectionSign = 0;
  double Span = 0.0;
};

struct FStraightenBaseline {
  TMap<FGuid, FNodeLayoutSnapshot> Nodes;
  TArray<FGuid> Selection;
  TArray<FStraightenConnectionSnapshot> Connections;
  UEAIIntegration::BlueprintGraph::FNodeBounds Region;
  UEAIIntegration::BlueprintGraph::FEditorGeometryFingerprint Geometry;
  FString GraphHash;
  bool bPackageDirty = false;
};

FVector2D BoundsCenter(
    const UEAIIntegration::BlueprintGraph::FNodeBounds &Bounds) {
  return FVector2D(Bounds.X + Bounds.Width * 0.5,
                   Bounds.Y + Bounds.Height * 0.5);
}

bool TryComputeGraphRegion(
    const TMap<FGuid, UEAIIntegration::BlueprintGraph::FNodeBounds> &Bounds,
    UEAIIntegration::BlueprintGraph::FNodeBounds &OutRegion) {
  if (Bounds.IsEmpty()) {
    return false;
  }
  double Left = TNumericLimits<double>::Max();
  double Top = TNumericLimits<double>::Max();
  double Right = TNumericLimits<double>::Lowest();
  double Bottom = TNumericLimits<double>::Lowest();
  for (const TPair<FGuid, UEAIIntegration::BlueprintGraph::FNodeBounds> &Pair :
       Bounds) {
    Left = FMath::Min(Left, Pair.Value.X);
    Top = FMath::Min(Top, Pair.Value.Y);
    Right = FMath::Max(Right, Pair.Value.Right());
    Bottom = FMath::Max(Bottom, Pair.Value.Bottom());
  }
  OutRegion.X = Left;
  OutRegion.Y = Top;
  OutRegion.Width = Right - Left;
  OutRegion.Height = Bottom - Top;
  OutRegion.Source = TEXT("editor");
  OutRegion.bExact = OutRegion.Width > 0.0 && OutRegion.Height > 0.0;
  return OutRegion.bExact;
}

TArray<UEdGraphNode *> GetGraphNodes(UEdGraph *Graph) {
  TArray<UEdGraphNode *> Nodes;
  if (Graph) {
    for (UEdGraphNode *Node : Graph->Nodes) {
      if (Node) {
        Nodes.Add(Node);
      }
    }
  }
  return Nodes;
}

void RestoreSelection(const FBlueprintGraphEditorContext &Context,
                      const TArray<FGuid> &Selection) {
  Context.GraphEditor->ClearSelectionSet();
  for (const FGuid &NodeId : Selection) {
    if (UEdGraphNode *Node =
            UEAIIntegration::BlueprintGraph::FindNode(Context.Graph, NodeId)) {
      Context.GraphEditor->SetNodeSelection(Node, true);
    }
  }
}

bool CaptureStraightenBaseline(
    const FBlueprintGraphEditorContext &Context,
    const TArray<UEdGraphNode *> &SelectedNodes, FStraightenBaseline &OutBaseline,
    FString &OutError) {
  OutBaseline = FStraightenBaseline();
  const TArray<UEdGraphNode *> GraphNodes = GetGraphNodes(Context.Graph);
  TMap<FGuid, UEAIIntegration::BlueprintGraph::FNodeBounds> EditorBounds;
  if (!UEAIIntegration::BlueprintGraph::TryCaptureSettledEditorGeometry(
          Context, GraphNodes, EditorBounds, OutBaseline.Geometry, OutError) ||
      !TryComputeGraphRegion(EditorBounds, OutBaseline.Region)) {
    if (OutError.IsEmpty()) {
      OutError = TEXT("The Graph region could not be calculated.");
    }
    return false;
  }

  for (const UEdGraphNode *Node : GraphNodes) {
    FNodeLayoutSnapshot Snapshot;
    Snapshot.X = Node->NodePosX;
    Snapshot.Y = Node->NodePosY;
    Snapshot.Width = Node->NodeWidth;
    Snapshot.Height = Node->NodeHeight;
    OutBaseline.Nodes.Add(Node->NodeGuid, Snapshot);
  }
  TSet<const UEdGraphNode *> SelectedSet;
  for (const UEdGraphNode *Node : SelectedNodes) {
    if (Node) {
      OutBaseline.Selection.Add(Node->NodeGuid);
      SelectedSet.Add(Node);
    }
  }

  TSet<FString> SeenConnections;
  for (const UEdGraphNode *SourceNode : SelectedNodes) {
    for (const UEdGraphPin *SourcePin : SourceNode->Pins) {
      if (!SourcePin || SourcePin->Direction != EGPD_Output) {
        continue;
      }
      for (const UEdGraphPin *TargetPin : SourcePin->LinkedTo) {
        const UEdGraphNode *TargetNode =
            TargetPin ? TargetPin->GetOwningNode() : nullptr;
        if (!TargetPin || TargetPin->Direction != EGPD_Input || !TargetNode ||
            !SelectedSet.Contains(TargetNode)) {
          continue;
        }
        const FString Key = SourceNode->NodeGuid.ToString() + TEXT(":") +
                            TargetNode->NodeGuid.ToString();
        if (SeenConnections.Contains(Key)) {
          continue;
        }
        SeenConnections.Add(Key);
        const UEAIIntegration::BlueprintGraph::FNodeBounds *SourceBounds =
            EditorBounds.Find(SourceNode->NodeGuid);
        const UEAIIntegration::BlueprintGraph::FNodeBounds *TargetBounds =
            EditorBounds.Find(TargetNode->NodeGuid);
        if (!SourceBounds || !TargetBounds) {
          OutError = TEXT("Connection geometry is incomplete.");
          return false;
        }
        const FVector2D Delta =
            BoundsCenter(*TargetBounds) - BoundsCenter(*SourceBounds);
        FStraightenConnectionSnapshot Connection;
        Connection.SourceNodeId = SourceNode->NodeGuid;
        Connection.TargetNodeId = TargetNode->NodeGuid;
        Connection.bHorizontal =
            FMath::Abs(Delta.X) >= ClearConnectionDirectionThreshold;
        const double DirectionDelta = Connection.bHorizontal ? Delta.X
                                                             : Delta.Y;
        if (FMath::Abs(DirectionDelta) >= ClearConnectionDirectionThreshold) {
          Connection.DirectionSign = DirectionDelta > 0.0 ? 1 : -1;
        }
        Connection.Span = Delta.Size();
        OutBaseline.Connections.Add(Connection);
      }
    }
  }
  OutBaseline.GraphHash =
      UEAIIntegration::BlueprintGraph::ComputeGraphHash(Context.Graph);
  OutBaseline.bPackageDirty = Context.Blueprint->GetOutermost()->IsDirty();
  if (OutBaseline.GraphHash.IsEmpty()) {
    OutError = TEXT("The pre-straighten Graph hash could not be computed.");
  }
  return !OutBaseline.GraphHash.IsEmpty();
}

bool ValidateStraightenResult(
    const FBlueprintGraphEditorContext &Context,
    const FStraightenBaseline &Baseline,
    const TMap<FGuid, UEAIIntegration::BlueprintGraph::FNodeBounds> &AfterBounds,
    FString &OutError) {
  if (AfterBounds.Num() != Baseline.Nodes.Num()) {
    OutError = TEXT("Straighten changed the Graph node set.");
    return false;
  }
  for (const TPair<FGuid, FNodeLayoutSnapshot> &Pair : Baseline.Nodes) {
    const UEdGraphNode *Node =
        UEAIIntegration::BlueprintGraph::FindNode(Context.Graph, Pair.Key);
    if (!Node) {
      OutError = FString::Printf(TEXT("Node '%s' disappeared during straighten."),
                                 *Pair.Key.ToString());
      return false;
    }
    const FVector2D Displacement(Node->NodePosX - Pair.Value.X,
                                 Node->NodePosY - Pair.Value.Y);
    if (Displacement.Size() > MaxStraightenNodeDisplacement) {
      OutError = FString::Printf(
          TEXT("Node '%s' moved %.1f Graph Units; the limit is %.0f."),
          *Pair.Key.ToString(), Displacement.Size(),
          MaxStraightenNodeDisplacement);
      return false;
    }
    if (Node->NodeWidth != Pair.Value.Width ||
        Node->NodeHeight != Pair.Value.Height) {
      OutError = FString::Printf(
          TEXT("Straighten unexpectedly resized node '%s' from %dx%d to "
               "%dx%d."),
          *Pair.Key.ToString(), Pair.Value.Width, Pair.Value.Height,
          Node->NodeWidth, Node->NodeHeight);
      return false;
    }
  }

  UEAIIntegration::BlueprintGraph::FNodeBounds AfterRegion;
  if (!TryComputeGraphRegion(AfterBounds, AfterRegion)) {
    OutError = TEXT("The Graph region is unavailable after straighten.");
    return false;
  }
  if (AfterRegion.Width > Baseline.Region.Width * MaxStraightenRegionExpansion ||
      AfterRegion.Height >
          Baseline.Region.Height * MaxStraightenRegionExpansion) {
    OutError = FString::Printf(
        TEXT("Straighten expanded the Graph region from %.1fx%.1f to "
             "%.1fx%.1f; the limit is %.1fx."),
        Baseline.Region.Width, Baseline.Region.Height, AfterRegion.Width,
        AfterRegion.Height, MaxStraightenRegionExpansion);
    return false;
  }

  for (const FStraightenConnectionSnapshot &Connection :
       Baseline.Connections) {
    const UEAIIntegration::BlueprintGraph::FNodeBounds *SourceBounds =
        AfterBounds.Find(Connection.SourceNodeId);
    const UEAIIntegration::BlueprintGraph::FNodeBounds *TargetBounds =
        AfterBounds.Find(Connection.TargetNodeId);
    if (!SourceBounds || !TargetBounds) {
      OutError = TEXT("Connection geometry is unavailable after straighten.");
      return false;
    }
    const FVector2D Delta =
        BoundsCenter(*TargetBounds) - BoundsCenter(*SourceBounds);
    const double DirectionDelta =
        Connection.bHorizontal ? Delta.X : Delta.Y;
    if (Connection.DirectionSign != 0 &&
        DirectionDelta * Connection.DirectionSign < 0.0) {
      OutError = FString::Printf(
          TEXT("Straighten reversed connection '%s' -> '%s'."),
          *Connection.SourceNodeId.ToString(),
          *Connection.TargetNodeId.ToString());
      return false;
    }
    if (Connection.Span > 0.0 &&
        Delta.Size() >
            Connection.Span * MaxStraightenConnectionSpanGrowth) {
      OutError = FString::Printf(
          TEXT("Straighten grew connection '%s' -> '%s' from %.1f to %.1f "
               "Graph Units; the limit is %.1fx."),
          *Connection.SourceNodeId.ToString(),
          *Connection.TargetNodeId.ToString(), Connection.Span, Delta.Size(),
          MaxStraightenConnectionSpanGrowth);
      return false;
    }
  }
  return true;
}

bool RestoreStraightenBaseline(const FBlueprintGraphEditorContext &Context,
                               const FStraightenBaseline &Baseline) {
  if (Context.Graph->Nodes.Num() != Baseline.Nodes.Num()) {
    return false;
  }
  for (const TPair<FGuid, FNodeLayoutSnapshot> &Pair : Baseline.Nodes) {
    UEdGraphNode *Node =
        UEAIIntegration::BlueprintGraph::FindNode(Context.Graph, Pair.Key);
    if (!Node) {
      return false;
    }
    Node->NodePosX = Pair.Value.X;
    Node->NodePosY = Pair.Value.Y;
    Node->NodeWidth = Pair.Value.Width;
    Node->NodeHeight = Pair.Value.Height;
  }
  Context.Graph->NotifyGraphChanged();
  UEAIIntegration::BlueprintGraph::RefreshGraphEditorLayout(
      Context.GraphEditor);
  RestoreSelection(Context, Baseline.Selection);
  Context.Blueprint->GetOutermost()->SetDirtyFlag(Baseline.bPackageDirty);
  return UEAIIntegration::BlueprintGraph::ComputeGraphHash(Context.Graph) ==
         Baseline.GraphHash;
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

    // Materialize the Graph widgets before changing selection. A deferred
    // SGraphEditor refresh can rebuild SGraphNode widgets and clear selection;
    // refreshing after SetNodeSelection would report a successful request with
    // an empty or incomplete read-back.
    ContextResult = ResolveGraphEditorContext(true, Context);
    if (!ContextResult.bSuccess) {
      return ContextResult;
    }
    UEAIIntegration::BlueprintGraph::RefreshGraphEditorLayout(
        Context.GraphEditor);

    // Selection changes only after every requested node has been validated and
    // the Graph Editor has completed its pending refresh.
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

    if (GEditor && GEditor->IsTransactionActive()) {
      return FMCPToolResult::Error(
          TEXT("Another Editor transaction is active. Retry after it "
               "finishes or use blueprint.layout.organize for an approved "
               "atomic layout."),
          TEXT("editor_transaction_busy"), 409);
    }

    FStraightenBaseline Baseline;
    FString GeometryError;
    if (!CaptureStraightenBaseline(Context, SelectedNodes, Baseline,
                                   GeometryError)) {
      return FMCPToolResult::Error(GeometryError,
                                   TEXT("graph_geometry_unavailable"), 409);
    }
    // Geometry refreshes may rebuild SGraphNode widgets and clear the live
    // selection. Reapply the validated snapshot before invoking UE's native
    // selection-driven command.
    RestoreSelection(Context, Baseline.Selection);

    const TMap<FGuid, FIntPoint> Before = CapturePositions(SelectedNodes);
    FScopedTransaction Transaction(
        NSLOCTEXT("UEAIIntegration", "GuardBlueprintStraighten",
                  "Guard Blueprint Straighten"));
    Context.Blueprint->Modify();
    Context.Graph->Modify();
    for (UEdGraphNode *Node : GetGraphNodes(Context.Graph)) {
      Node->Modify();
    }
    Context.GraphEditor->OnStraightenConnections();
#if WITH_DEV_AUTOMATION_TESTS
    bool bInjectUnsafeDisplacement = false;
    if (Params.IsValid() &&
        Params->TryGetBoolField(TEXT("__testInjectUnsafeDisplacement"),
                                bInjectUnsafeDisplacement) &&
        bInjectUnsafeDisplacement && !SelectedNodes.IsEmpty()) {
      if (const FNodeLayoutSnapshot *Snapshot =
              Baseline.Nodes.Find(SelectedNodes[0]->NodeGuid)) {
        SelectedNodes[0]->NodePosX =
            Snapshot->X + static_cast<int32>(MaxStraightenNodeDisplacement) +
            1;
        SelectedNodes[0]->NodePosY = Snapshot->Y;
      }
    }
#endif

    TMap<FGuid, UEAIIntegration::BlueprintGraph::FNodeBounds> AfterBounds;
    UEAIIntegration::BlueprintGraph::FEditorGeometryFingerprint AfterGeometry;
    FString ValidationError;
    const TArray<UEdGraphNode *> GraphNodes = GetGraphNodes(Context.Graph);
    const bool bGeometryReady =
        UEAIIntegration::BlueprintGraph::TryCaptureSettledEditorGeometry(
            Context, GraphNodes, AfterBounds, AfterGeometry, ValidationError);
    const bool bValid = bGeometryReady && ValidateStraightenResult(
                                              Context, Baseline, AfterBounds,
                                              ValidationError);
    if (!bValid) {
      const FString FailureReason = ValidationError.IsEmpty()
                                        ? TEXT("Straighten validation failed.")
                                        : ValidationError;
      const bool bRestored = RestoreStraightenBaseline(Context, Baseline);
      Transaction.Cancel();
      if (!bRestored) {
        return FMCPToolResult::Error(
            TEXT("Straighten validation failed and the complete Graph "
                 "snapshot could not be verified after restoration. Root "
                 "cause: ") +
                FailureReason,
            TEXT("verification_failed"), 500);
      }
      return FMCPToolResult::Error(
          FailureReason +
              TEXT(" The complete Graph snapshot was restored."),
          TEXT("layout_validation_failed"), 422);
    }

    const FString AfterGraphHash =
        UEAIIntegration::BlueprintGraph::ComputeGraphHash(Context.Graph);
    const bool bChanged = AfterGraphHash != Baseline.GraphHash;
    if (!bChanged) {
      Context.Blueprint->GetOutermost()->SetDirtyFlag(
          Baseline.bPackageDirty);
      Transaction.Cancel();
    } else {
      Context.Blueprint->MarkPackageDirty();
    }
    RestoreSelection(Context, Baseline.Selection);
    TSharedRef<FJsonObject> Result =
        BuildLayoutResult(Context, SelectedNodes, Before);
    Result->SetStringField(TEXT("beforeGraphHash"), Baseline.GraphHash);
    Result->SetStringField(TEXT("afterGraphHash"), AfterGraphHash);
    Result->SetObjectField(
        TEXT("geometryFingerprint"),
        UEAIIntegration::BlueprintGraph::GeometryFingerprintToJson(
            AfterGeometry));
    return FMCPToolResult::Ok(Result);
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
