#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorModule.h"
#include "Domains/Blueprint/BlueprintGraphEditorSupport.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationSubsystem.h"
#include "Workflow/UEWorkflowRuntime.h"

namespace {
TSharedPtr<FJsonObject> MakeBlueprintGraphParams(const FString &Blueprint,
                                                 const FString &Graph) {
  TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
  Params->SetStringField(TEXT("blueprint"), Blueprint);
  Params->SetStringField(TEXT("graph"), Graph);
  return Params;
}

TSharedPtr<FJsonObject>
MakeSelectionParams(const FString &Blueprint, const FString &Graph,
                    const TArray<UEdGraphNode *> &Nodes) {
  TSharedPtr<FJsonObject> Params = MakeBlueprintGraphParams(Blueprint, Graph);
  TArray<TSharedPtr<FJsonValue>> NodeIds;
  for (const UEdGraphNode *Node : Nodes) {
    NodeIds.Add(MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
  }
  Params->SetArrayField(TEXT("nodeIds"), NodeIds);
  return Params;
}

bool JsonStringArrayContains(const TSharedPtr<FJsonObject> &Object,
                             const TCHAR *Field,
                             const FString &Expected) {
  if (!Object.IsValid() ||
      !Object->HasTypedField<EJson::Array>(Field)) {
    return false;
  }
  return Object->GetArrayField(Field).ContainsByPredicate(
      [&Expected](const TSharedPtr<FJsonValue> &Value) {
        FString Actual;
        return Value.IsValid() && Value->TryGetString(Actual) &&
               Actual == Expected;
      });
}

UK2Node_IfThenElse *AddBranchNode(UEdGraph *Graph, const int32 X,
                                  const int32 Y) {
  UK2Node_IfThenElse *Node =
      NewObject<UK2Node_IfThenElse>(Graph, NAME_None, RF_Transactional);
  Node->CreateNewGuid();
  Node->NodePosX = X;
  Node->NodePosY = Y;
  Graph->AddNode(Node, false, false);
  Node->AllocateDefaultPins();
  return Node;
}

bool CleanupBlueprint(UBlueprint *Blueprint) {
  if (!Blueprint) {
    return true;
  }
  if (!FApp::CanEverRender()) {
    // Closing a standalone asset editor under NullRHI reaches
    // FGenericWindow::GetRestoredDimensions(), which is intentionally
    // unsupported. Remove the saved fixture from the asset registry and disk;
    // the isolated automation process releases the still-open editor on exit.
    UPackage *Package = Blueprint->GetOutermost();
    Package->SetDirtyFlag(false);
    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());
    FAssetRegistryModule::AssetDeleted(Blueprint);
    return !IFileManager::Get().FileExists(*PackageFilename) ||
           IFileManager::Get().Delete(*PackageFilename, false, true);
  }
  const FString PackageName = Blueprint->GetOutermost()->GetName();
  const FString ObjectPath = Blueprint->GetPathName();
  if (GEditor) {
    if (UAssetEditorSubsystem *AssetEditorSubsystem =
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()) {
      AssetEditorSubsystem->CloseAllEditorsForAsset(Blueprint);
    }
  }
  Blueprint->GetOutermost()->SetDirtyFlag(false);
  TArray<UObject *> Assets{Blueprint};
  const int32 DeletedCount = ObjectTools::ForceDeleteObjects(Assets, false);
  const bool bRegistryRemoved =
      !FAssetRegistryModule::GetRegistry()
           .GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true)
           .IsValid();
  return DeletedCount == 1 && bRegistryRemoved &&
         !FPackageName::DoesPackageExist(PackageName);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBlueprintEditorNativeLayoutCommandsTest,
    "UE_AI_integration.Blueprint.EditorLayout.NativeCommandLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintEditorNativeLayoutCommandsTest::RunTest(
    const FString &Parameters) {
  if (!FApp::CanEverRender()) {
    AddInfo(TEXT(
        "Skipped: native Graph Editor layout and capture verification requires "
        "a rendered Slate window; run this test without -nullrhi."));
    return true;
  }

  UUEAIIntegrationSubsystem *Subsystem =
      GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
              : nullptr;
  if (!Subsystem || !Subsystem->GetRegistry()) {
    AddError(TEXT("UE integration subsystem is not initialized."));
    return false;
  }
  FMCPToolRegistry &Registry = *Subsystem->GetRegistry();

  const FString AssetName =
      FString::Printf(TEXT("BP_EditorLayout_%s"),
                      *FGuid::NewGuid().ToString(EGuidFormats::Digits));
  const FString PackagePath = TEXT("/Game/Automation");
  const FString BlueprintPath = PackagePath + TEXT("/") + AssetName;
  TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
  CreateParams->SetStringField(TEXT("blueprintName"), AssetName);
  CreateParams->SetStringField(TEXT("packagePath"), PackagePath);
  CreateParams->SetStringField(TEXT("parentClass"), TEXT("Actor"));
  const FMCPToolResult CreateResult =
      Registry.ExecuteTool(TEXT("blueprint.asset.create"), CreateParams);
  if (!CreateResult.bSuccess) {
    AddError(FString::Printf(TEXT("Could not create Blueprint fixture: %s"),
                             *CreateResult.ErrorMessage));
    return false;
  }

  UBlueprint *Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
  if (!Blueprint || Blueprint->UbergraphPages.IsEmpty()) {
    AddError(TEXT("Blueprint fixture has no Event Graph."));
    CleanupBlueprint(Blueprint);
    return false;
  }
  UEdGraph *Graph = Blueprint->UbergraphPages[0];
  const FString GraphName = Graph->GetName();
  // The Actor Blueprint factory may seed EventGraph with default events.
  // This test validates the layout commands against a controlled graph, so
  // remove those unrelated nodes instead of accidentally treating a factory
  // overlap as a layout-command regression.
  const TArray<UEdGraphNode *> SeededNodes = Graph->Nodes;
  for (UEdGraphNode *SeededNode : SeededNodes) {
    if (SeededNode) {
      Graph->RemoveNode(SeededNode);
    }
  }
  UK2Node_IfThenElse *First = AddBranchNode(Graph, 0, 100);
  UK2Node_IfThenElse *Second = AddBranchNode(Graph, 300, 350);
  UK2Node_IfThenElse *Third = AddBranchNode(Graph, 900, 600);
  const UEdGraphSchema *Schema = Graph->GetSchema();
  UEdGraphPin *FirstThen = First->FindPin(UEdGraphSchema_K2::PN_Then);
  UEdGraphPin *SecondExecute = Second->FindPin(UEdGraphSchema_K2::PN_Execute);
  if (!Schema || !FirstThen || !SecondExecute ||
      !Schema->TryCreateConnection(FirstThen, SecondExecute)) {
    AddError(TEXT("Could not connect layout fixture nodes."));
    CleanupBlueprint(Blueprint);
    return false;
  }
  FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

  if (UAssetEditorSubsystem *AssetEditorSubsystem =
          GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()) {
    AssetEditorSubsystem->CloseAllEditorsForAsset(Blueprint);
  }

  const TArray<UEdGraphNode *> AllNodes{First, Second, Third};
  FMCPToolResult SelectionResult = Registry.ExecuteTool(
      TEXT("blueprint.selection.set"),
      MakeSelectionParams(BlueprintPath, GraphName, AllNodes));
  TestTrue(TEXT("Selection command opens and selects nodes"),
           SelectionResult.bSuccess);
  if (!SelectionResult.bSuccess) {
    AddError(SelectionResult.ErrorMessage);
    CleanupBlueprint(Blueprint);
    return false;
  }
  AddInfo(FString::Printf(
      TEXT("Selection editorOpened=%s (the asset editor may already have been "
           "opened asynchronously by asset creation)."),
      SelectionResult.Data->GetBoolField(TEXT("editorOpened"))
          ? TEXT("true")
          : TEXT("false")));
  TestEqual(TEXT("Selection returns all selected nodes"),
            static_cast<int32>(
                SelectionResult.Data->GetNumberField(TEXT("selectedCount"))),
            3);

  TSharedPtr<FJsonObject> DuplicateSelection =
      MakeSelectionParams(BlueprintPath, GraphName, {First, First});
  const FMCPToolResult DuplicateResult =
      Registry.ExecuteTool(TEXT("blueprint.selection.set"), DuplicateSelection);
  TestFalse(TEXT("Duplicate selection is rejected"), DuplicateResult.bSuccess);
  TSharedPtr<IBlueprintEditor> BlueprintEditor =
      FKismetEditorUtilities::GetIBlueprintEditorForObject(Blueprint, false);
  TSharedPtr<FBlueprintEditor> ConcreteBlueprintEditor =
      StaticCastSharedPtr<FBlueprintEditor>(BlueprintEditor);
  TestTrue(TEXT("Selection resolves a live Blueprint Editor"),
           ConcreteBlueprintEditor.IsValid());
  TestTrue(TEXT("Selection focuses the requested Graph"),
           ConcreteBlueprintEditor.IsValid() &&
               ConcreteBlueprintEditor->GetFocusedGraph() == Graph);
  auto RefreshLiveGraphEditor = [&]() {
    if (!ConcreteBlueprintEditor.IsValid()) {
      return;
    }
    const TSharedPtr<SGraphEditor> GraphEditor =
        ConcreteBlueprintEditor->OpenGraphAndBringToFront(Graph, false);
    if (GraphEditor.IsValid()) {
      UEAIIntegration::BlueprintGraph::RefreshGraphEditorLayout(GraphEditor);
    }
  };
  TestTrue(TEXT("Rejected selection preserves the previous selection"),
           ConcreteBlueprintEditor.IsValid() &&
               ConcreteBlueprintEditor->GetSelectedNodes().Num() == 3);

  TSharedPtr<FJsonObject> AlignParams =
      MakeBlueprintGraphParams(BlueprintPath, GraphName);
  AlignParams->SetStringField(TEXT("alignment"), TEXT("left"));
  const FMCPToolResult AlignResult =
      Registry.ExecuteTool(TEXT("blueprint.layout.align"), AlignParams);
  TestTrue(TEXT("Native align succeeds"), AlignResult.bSuccess);
  TestEqual(TEXT("First aligned X"), First->NodePosX, 0);
  TestEqual(TEXT("Second aligned X"), Second->NodePosX, 0);
  TestEqual(TEXT("Third aligned X"), Third->NodePosX, 0);

  TestTrue(TEXT("Native align participates in Undo"),
           GEditor->UndoTransaction());
  TestEqual(TEXT("Undo restores second X"), Second->NodePosX, 300);
  TestEqual(TEXT("Undo restores third X"), Third->NodePosX, 900);
  TestTrue(TEXT("Native align participates in Redo"),
           GEditor->RedoTransaction());
  TestEqual(TEXT("Redo aligns second X"), Second->NodePosX, 0);
  TestEqual(TEXT("Redo aligns third X"), Third->NodePosX, 0);

  First->NodePosX = 0;
  Second->NodePosX = 100;
  Third->NodePosX = 900;
  SelectionResult = Registry.ExecuteTool(
      TEXT("blueprint.selection.set"),
      MakeSelectionParams(BlueprintPath, GraphName, AllNodes));
  TestTrue(TEXT("Nodes can be reselected after Undo/Redo"),
           SelectionResult.bSuccess);
  TSharedPtr<FJsonObject> DistributeParams =
      MakeBlueprintGraphParams(BlueprintPath, GraphName);
  DistributeParams->SetStringField(TEXT("orientation"), TEXT("horizontal"));
  const FMCPToolResult DistributeResult = Registry.ExecuteTool(
      TEXT("blueprint.layout.distribute"), DistributeParams);
  if (!DistributeResult.bSuccess) {
    AddInfo(TEXT("Distribution error: ") + DistributeResult.ErrorMessage);
  }
  TestTrue(TEXT("Native horizontal distribution succeeds"),
           DistributeResult.bSuccess);
  TestEqual(TEXT("Distribution keeps first endpoint"), First->NodePosX, 0);
  TestEqual(TEXT("Distribution keeps last endpoint"), Third->NodePosX, 900);
  TestTrue(TEXT("Distribution places the middle node between endpoints"),
           Second->NodePosX > First->NodePosX &&
               Second->NodePosX < Third->NodePosX);

  SelectionResult = Registry.ExecuteTool(
      TEXT("blueprint.selection.set"),
      MakeSelectionParams(BlueprintPath, GraphName, {First, Second}));
  TestTrue(TEXT("Connected nodes can be selected"), SelectionResult.bSuccess);
  TSharedPtr<FJsonObject> StraightenParams =
      MakeBlueprintGraphParams(BlueprintPath, GraphName);
  const FMCPToolResult StraightenResult = Registry.ExecuteTool(
      TEXT("blueprint.layout.straighten"), StraightenParams);
  TestTrue(TEXT("Native straighten succeeds for connected selected nodes"),
           StraightenResult.bSuccess);

  TSharedPtr<SGraphEditor> CaptureGraphEditor;
  if (FApp::CanEverRender()) {
    SelectionResult = Registry.ExecuteTool(
        TEXT("blueprint.selection.set"),
        MakeSelectionParams(BlueprintPath, GraphName, AllNodes));
    TestTrue(TEXT("Nodes can be reselected for Comment creation"),
             SelectionResult.bSuccess);
    TSharedPtr<FJsonObject> CommentParams =
        MakeBlueprintGraphParams(BlueprintPath, GraphName);
    CommentParams->SetStringField(TEXT("text"), TEXT("Native Layout Group"));
    const FMCPToolResult CommentResult = Registry.ExecuteTool(
        TEXT("blueprint.comment.create_from_selection"), CommentParams);
    if (!CommentResult.bSuccess) {
      AddInfo(TEXT("Comment creation error: ") + CommentResult.ErrorMessage);
    }
    TestTrue(TEXT("Native Comment creation succeeds"), CommentResult.bSuccess);

    UEdGraphNode_Comment *Comment = nullptr;
    if (CommentResult.bSuccess) {
      const FString CommentNodeId =
          CommentResult.Data->GetStringField(TEXT("commentNodeId"));
      Comment = Cast<UEdGraphNode_Comment>(
          MCPHelpers::FindNodeByGuid(Blueprint, CommentNodeId));
      TestNotNull(TEXT("Created node is a Comment"), Comment);
      if (Comment) {
        TestEqual(TEXT("Comment text is applied"), Comment->NodeComment,
                  FString(TEXT("Native Layout Group")));
        TestEqual(TEXT("Comment uses group movement"),
                  static_cast<int32>(Comment->MoveMode.GetValue()),
                  static_cast<int32>(ECommentBoxMode::GroupMovement));
        AddInfo(FString::Printf(
            TEXT("Comment bounds=(%d,%d,%d,%d); nodes=(%d,%d),(%d,%d),(%d,%d)"),
            Comment->NodePosX, Comment->NodePosY, Comment->NodeWidth,
            Comment->NodeHeight, First->NodePosX, First->NodePosY,
            Second->NodePosX, Second->NodePosY, Third->NodePosX,
            Third->NodePosY));
        TestTrue(TEXT("Comment bounds contain selected node positions"),
                 Comment->NodePosX <= First->NodePosX &&
                     Comment->NodePosY <= First->NodePosY &&
                     Comment->NodePosX + Comment->NodeWidth >= Third->NodePosX &&
                     Comment->NodePosY + Comment->NodeHeight >= Third->NodePosY);
        TestEqual(TEXT("Comment uses native 50-unit left padding"),
                  Comment->NodePosX,
                  FMath::Min3(First->NodePosX, Second->NodePosX,
                              Third->NodePosX) -
                      50);
        TestEqual(TEXT("Comment uses native 50-unit top padding"),
                  Comment->NodePosY,
                  FMath::Min3(First->NodePosY, Second->NodePosY,
                              Third->NodePosY) -
                      50);

        TSharedPtr<FJsonObject> BoundsParams =
            MakeBlueprintGraphParams(BlueprintPath, GraphName);
        BoundsParams->SetStringField(TEXT("commentNodeId"), CommentNodeId);
        BoundsParams->SetNumberField(TEXT("x"), -100);
        BoundsParams->SetNumberField(TEXT("y"), -200);
        BoundsParams->SetNumberField(TEXT("width"), 1400);
        BoundsParams->SetNumberField(TEXT("height"), 1000);
        const FMCPToolResult BoundsResult = Registry.ExecuteTool(
            TEXT("blueprint.comment.bounds.set"), BoundsParams);
        TestTrue(TEXT("Comment bounds update succeeds"), BoundsResult.bSuccess);
        TestEqual(TEXT("Comment bounds X"), Comment->NodePosX, -100);
        TestEqual(TEXT("Comment bounds Y"), Comment->NodePosY, -200);
        TestEqual(TEXT("Comment bounds width"), Comment->NodeWidth, 1400);
        TestEqual(TEXT("Comment bounds height"), Comment->NodeHeight, 1000);
      }
    }

    // Place a fourth ordinary node across the right edge of the explicit
    // Comment bounds. This makes the contained/intersecting contracts
    // independently observable instead of merely checking that an array is
    // non-empty.
    UK2Node_IfThenElse *IntersectingNode =
        AddBranchNode(Graph, 1250, 0);
    Graph->NotifyGraphChanged();
    RefreshLiveGraphEditor();

    TSharedPtr<FJsonObject> GraphGetParams = MakeShared<FJsonObject>();
    GraphGetParams->SetStringField(TEXT("name"), BlueprintPath);
    GraphGetParams->SetStringField(TEXT("graph"), GraphName);
    GraphGetParams->SetStringField(TEXT("geometryMode"), TEXT("editor"));
    const FMCPToolResult GraphGetResult =
        Registry.ExecuteTool(TEXT("blueprint.graph.get"), GraphGetParams);
    if (!GraphGetResult.bSuccess) {
      AddInfo(FString::Printf(
          TEXT("Exact Graph query error [%s]: %s"),
          *GraphGetResult.ErrorCode, *GraphGetResult.ErrorMessage));
    }
    TestTrue(TEXT("Graph query returns exact Editor geometry"),
             GraphGetResult.bSuccess);
    if (GraphGetResult.bSuccess) {
      TestEqual(TEXT("Graph geometry is exact"),
                GraphGetResult.Data->GetStringField(TEXT("geometryStatus")),
                FString(TEXT("exact")));
      TestTrue(TEXT("Graph query includes a stable graph hash"),
               GraphGetResult.Data->GetStringField(TEXT("graphHash"))
                   .StartsWith(TEXT("sha256:")));
      TSharedPtr<FJsonObject> FirstNodeJson;
      TSharedPtr<FJsonObject> CommentNodeJson;
      for (const TSharedPtr<FJsonValue> &NodeValue :
           GraphGetResult.Data->GetArrayField(TEXT("nodes"))) {
        const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
        if (!Node.IsValid()) {
          continue;
        }
        if (Node->GetStringField(TEXT("nodeId")) ==
            First->NodeGuid.ToString()) {
          FirstNodeJson = Node;
        }
        if (Node->GetStringField(TEXT("nodeClass"))
                .Contains(TEXT("Comment"))) {
          CommentNodeJson = Node;
        }
      }
      TestTrue(TEXT("Graph query returns the requested ordinary node"),
               FirstNodeJson.IsValid());
      if (FirstNodeJson.IsValid()) {
        const TSharedPtr<FJsonObject> Bounds =
            FirstNodeJson->GetObjectField(TEXT("bounds"));
        TestTrue(TEXT("Ordinary node has a measured width"),
                 Bounds->GetNumberField(TEXT("width")) > 0.0);
        TestTrue(TEXT("Ordinary node has a measured height"),
                 Bounds->GetNumberField(TEXT("height")) > 0.0);
        TestEqual(TEXT("Ordinary node geometry comes from SGraphEditor"),
                  Bounds->GetStringField(TEXT("source")),
                  FString(TEXT("editor")));
        TestTrue(TEXT("Ordinary node geometry is explicitly exact"),
                 Bounds->GetBoolField(TEXT("exact")));
      }
      TestTrue(TEXT("Graph query returns the native Comment"),
               CommentNodeJson.IsValid());
      if (CommentNodeJson.IsValid()) {
        const TSharedPtr<FJsonObject> Bounds =
            CommentNodeJson->GetObjectField(TEXT("bounds"));
        TestEqual(TEXT("Comment geometry width is read back"),
                  static_cast<int32>(
                      Bounds->GetNumberField(TEXT("width"))),
                  1400);
        TestEqual(TEXT("Comment geometry height is read back"),
                  static_cast<int32>(
                      Bounds->GetNumberField(TEXT("height"))),
                  1000);
        TestEqual(TEXT("Comment geometry comes from SGraphEditor"),
                  Bounds->GetStringField(TEXT("source")),
                  FString(TEXT("editor")));
        TestTrue(TEXT("Comment geometry is explicitly exact"),
                 Bounds->GetBoolField(TEXT("exact")));
        TestTrue(TEXT("Comment contains the first selected node"),
                 JsonStringArrayContains(
                     CommentNodeJson, TEXT("containedNodeIds"),
                     First->NodeGuid.ToString()));
        TestTrue(TEXT("Comment contains the second selected node"),
                 JsonStringArrayContains(
                     CommentNodeJson, TEXT("containedNodeIds"),
                     Second->NodeGuid.ToString()));
        TestTrue(TEXT("Comment contains the third selected node"),
                 JsonStringArrayContains(
                     CommentNodeJson, TEXT("containedNodeIds"),
                     Third->NodeGuid.ToString()));
        TestTrue(TEXT("Comment reports the edge-crossing node separately"),
                 JsonStringArrayContains(
                     CommentNodeJson, TEXT("intersectingNodeIds"),
                     IntersectingNode->NodeGuid.ToString()));
        TestFalse(TEXT("Edge-crossing node is not reported as contained"),
                  JsonStringArrayContains(
                      CommentNodeJson, TEXT("containedNodeIds"),
                      IntersectingNode->NodeGuid.ToString()));
        TestEqual(TEXT("Comment containment uses complete Editor geometry"),
                  CommentNodeJson->GetStringField(
                      TEXT("containmentStatus")),
                  FString(TEXT("exact")));
      }

      GraphGetParams->SetStringField(TEXT("geometryMode"), TEXT("stored"));
      const FMCPToolResult StoredGraphGetResult =
          Registry.ExecuteTool(TEXT("blueprint.graph.get"), GraphGetParams);
      TestTrue(TEXT("Stored Graph geometry remains queryable"),
               StoredGraphGetResult.bSuccess);
      bool bFoundIncompleteContainment = false;
      if (StoredGraphGetResult.bSuccess) {
        TestTrue(
            TEXT("Stored ordinary node geometry is not labeled exact"),
            StoredGraphGetResult.Data->GetStringField(
                TEXT("geometryStatus")) != TEXT("exact"));
        for (const TSharedPtr<FJsonValue> &NodeValue :
             StoredGraphGetResult.Data->GetArrayField(TEXT("nodes"))) {
          const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
          if (Node.IsValid() &&
              Node->GetStringField(TEXT("nodeClass")).Contains(
                  TEXT("Comment"))) {
            bFoundIncompleteContainment =
                Node->GetStringField(TEXT("containmentStatus")) ==
                    TEXT("incomplete") &&
                !Node->GetArrayField(
                         TEXT("unresolvedContainmentNodeIds"))
                     .IsEmpty();
            if (bFoundIncompleteContainment) {
              break;
            }
          }
        }
      }
      TestTrue(
          TEXT("Stored-only nodes make Comment containment incomplete"),
          bFoundIncompleteContainment);

      UEdGraph *EmptyGraph = FBlueprintEditorUtils::CreateNewGraph(
          Blueprint, TEXT("EmptyGeometryGraph"), UEdGraph::StaticClass(),
          UEdGraphSchema_K2::StaticClass());
      if (EmptyGraph) {
        Blueprint->FunctionGraphs.Add(EmptyGraph);
        TSharedPtr<FJsonObject> EmptyGraphParams =
            MakeShared<FJsonObject>();
        EmptyGraphParams->SetStringField(TEXT("name"), BlueprintPath);
        EmptyGraphParams->SetStringField(
            TEXT("graph"), EmptyGraph->GetName());
        EmptyGraphParams->SetStringField(
            TEXT("geometryMode"), TEXT("editor"));
        const FMCPToolResult EmptyGraphResult = Registry.ExecuteTool(
            TEXT("blueprint.graph.get"), EmptyGraphParams);
        TestTrue(TEXT("An empty rendered Graph has valid geometry"),
                 EmptyGraphResult.bSuccess);
        if (EmptyGraphResult.bSuccess) {
          TestEqual(
              TEXT("Empty editor Graph geometry is exact"),
              EmptyGraphResult.Data->GetStringField(
                  TEXT("geometryStatus")),
              FString(TEXT("exact")));
        }
        FBlueprintEditorUtils::RemoveGraph(
            Blueprint, EmptyGraph, EGraphRemoveFlags::MarkTransient);
      } else {
        AddError(TEXT("Could not create the empty geometry fixture."));
      }
      GraphGetParams->SetStringField(TEXT("geometryMode"), TEXT("editor"));
    }
    IntersectingNode->NodePosX = 6000;
    IntersectingNode->NodePosY = 6000;
    Graph->NotifyGraphChanged();
    RefreshLiveGraphEditor();

    TSharedPtr<FJsonObject> LayoutValidateParams =
        MakeBlueprintGraphParams(BlueprintPath, GraphName);
    LayoutValidateParams->SetStringField(TEXT("geometryMode"),
                                         TEXT("editor"));
    LayoutValidateParams->SetNumberField(TEXT("commentPadding"), 20);
    const FMCPToolResult LayoutValidateResult = Registry.ExecuteTool(
        TEXT("blueprint.layout.validate"), LayoutValidateParams);
    TestTrue(TEXT("Layout validation succeeds with exact geometry"),
             LayoutValidateResult.bSuccess);
    if (LayoutValidateResult.bSuccess) {
      for (const TSharedPtr<FJsonValue> &Diagnostic :
           LayoutValidateResult.Data->GetArrayField(TEXT("diagnostics"))) {
        AddInfo(TEXT("Layout validation diagnostic: ") +
                Diagnostic->AsObject()->GetStringField(TEXT("rule")));
      }
      TestEqual(TEXT("Layout validation geometry is exact"),
                LayoutValidateResult.Data->GetStringField(
                    TEXT("geometryStatus")),
                FString(TEXT("exact")));
      TestEqual(TEXT("Complete layout validation has a conclusion"),
                LayoutValidateResult.Data->GetStringField(
                    TEXT("conclusion")),
                FString(TEXT("valid")));
    }
    TSharedPtr<FJsonObject> StoredValidationParams =
        MakeSelectionParams(BlueprintPath, GraphName, {First, Second});
    StoredValidationParams->SetStringField(
        TEXT("geometryMode"), TEXT("stored"));
    const FMCPToolResult StoredValidation = Registry.ExecuteTool(
        TEXT("blueprint.layout.validate"), StoredValidationParams);
    TestTrue(TEXT("Stored-only layout validation returns evidence"),
             StoredValidation.bSuccess);
    if (StoredValidation.bSuccess) {
      TestEqual(
          TEXT("Incomplete geometry is explicitly inconclusive"),
          StoredValidation.Data->GetStringField(TEXT("conclusion")),
          FString(TEXT("inconclusive")));
      TestTrue(
          TEXT("Inconclusive layout validity is null"),
          StoredValidation.Data->HasTypedField<EJson::Null>(
              TEXT("valid")));
    }

    const int32 SavedSecondX = Second->NodePosX;
    const int32 SavedSecondY = Second->NodePosY;
    Second->NodePosX = First->NodePosX;
    Second->NodePosY = First->NodePosY;
    Graph->NotifyGraphChanged();
    if (ConcreteBlueprintEditor.IsValid()) {
      if (const TSharedPtr<SGraphEditor> GraphEditor =
              ConcreteBlueprintEditor->OpenGraphAndBringToFront(Graph, false)) {
        GraphEditor->SlatePrepass(1.0f);
      }
    }
    const FMCPToolResult OverlapValidation = Registry.ExecuteTool(
        TEXT("blueprint.layout.validate"), LayoutValidateParams);
    bool bFoundOverlapDiagnostic = false;
    if (OverlapValidation.bSuccess) {
      for (const TSharedPtr<FJsonValue> &Diagnostic :
           OverlapValidation.Data->GetArrayField(TEXT("diagnostics"))) {
        bFoundOverlapDiagnostic =
            Diagnostic->AsObject()->GetStringField(TEXT("rule")) ==
            TEXT("node_overlap");
        if (bFoundOverlapDiagnostic) {
          break;
        }
      }
    }
    TestTrue(TEXT("Layout validation reports a real node overlap"),
             OverlapValidation.bSuccess && bFoundOverlapDiagnostic);
    Second->NodePosX = SavedSecondX;
    Second->NodePosY = SavedSecondY;
    Graph->NotifyGraphChanged();

    First->NodePosX = 0;
    First->NodePosY = 0;
    Second->NodePosX = 350;
    Second->NodePosY = 240;
    Third->NodePosX = 900;
    Third->NodePosY = 500;
    First->NodeComment = TEXT("Geometry fingerprint fixture");
    First->bCommentBubbleVisible = false;
    Graph->NotifyGraphChanged();

    TSharedPtr<FJsonObject> OrganizeParams =
        MakeBlueprintGraphParams(BlueprintPath, GraphName);
    TSharedRef<FJsonObject> Group = MakeShared<FJsonObject>();
    Group->SetStringField(TEXT("id"), TEXT("native-layout"));
    TArray<TSharedPtr<FJsonValue>> GroupNodeIds;
    for (const UEdGraphNode *Node : AllNodes) {
      GroupNodeIds.Add(
          MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
    }
    Group->SetArrayField(TEXT("nodeIds"), GroupNodeIds);
    TArray<TSharedPtr<FJsonValue>> Actions;
    TSharedRef<FJsonObject> AlignAction = MakeShared<FJsonObject>();
    AlignAction->SetStringField(TEXT("kind"), TEXT("align"));
    AlignAction->SetStringField(TEXT("alignment"), TEXT("top"));
    Actions.Add(MakeShared<FJsonValueObject>(AlignAction));
    TSharedRef<FJsonObject> DistributeAction = MakeShared<FJsonObject>();
    DistributeAction->SetStringField(TEXT("kind"), TEXT("distribute"));
    DistributeAction->SetStringField(TEXT("orientation"),
                                     TEXT("horizontal"));
    Actions.Add(MakeShared<FJsonValueObject>(DistributeAction));
    Group->SetArrayField(TEXT("actions"), Actions);
    TSharedRef<FJsonObject> OrganizeComment = MakeShared<FJsonObject>();
    OrganizeComment->SetStringField(TEXT("text"),
                                    TEXT("Atomic Layout Group"));
    OrganizeComment->SetNumberField(TEXT("padding"), 60);
    Group->SetObjectField(TEXT("comment"), OrganizeComment);
    OrganizeParams->SetArrayField(
        TEXT("groups"),
        {MakeShared<FJsonValueObject>(Group)});
    OrganizeParams->SetBoolField(TEXT("dryRun"), true);

    // First ask the real organizer where it will put an affected node. The
    // external collision fixture is then placed at that predicted coordinate,
    // rather than assuming that a particular native layout implementation will
    // leave the first node at its stored position.
    if (Comment) {
      Comment->NodePosX = 5000;
      Comment->NodePosY = 5000;
      Comment->NodeWidth = 600;
      Comment->NodeHeight = 400;
    }
    Graph->NotifyGraphChanged();
    RefreshLiveGraphEditor();
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    const FMCPToolResult ObstaclePlacementPreview = Registry.ExecuteTool(
        TEXT("blueprint.layout.organize"), OrganizeParams);
    TestTrue(TEXT("Organizer predicts positions for collision fixture"),
             ObstaclePlacementPreview.bSuccess);
    int32 PredictedObstacleX = Second->NodePosX;
    int32 PredictedObstacleY = Second->NodePosY;
    bool bFoundPredictedPosition = false;
    if (ObstaclePlacementPreview.bSuccess) {
      for (const TSharedPtr<FJsonValue> &ChangeValue :
           ObstaclePlacementPreview.Data->GetArrayField(
               TEXT("positionChanges"))) {
        const TSharedPtr<FJsonObject> Change = ChangeValue->AsObject();
        if (!Change.IsValid()) {
          continue;
        }
        const TSharedPtr<FJsonObject> After =
            Change->GetObjectField(TEXT("after"));
        PredictedObstacleX =
            static_cast<int32>(After->GetNumberField(TEXT("x")));
        PredictedObstacleY =
            static_cast<int32>(After->GetNumberField(TEXT("y")));
        bFoundPredictedPosition = true;
        break;
      }
    }
    TestTrue(TEXT("Organizer returns a changed node prediction"),
             bFoundPredictedPosition);

    UK2Node_IfThenElse *ExternalObstacle = IntersectingNode;
    ExternalObstacle->NodePosX = PredictedObstacleX;
    ExternalObstacle->NodePosY = PredictedObstacleY;
    if (Comment) {
      Comment->NodePosX = PredictedObstacleX - 150;
      Comment->NodePosY = PredictedObstacleY - 150;
      Comment->NodeWidth = 700;
      Comment->NodeHeight = 600;
    }
    Graph->NotifyGraphChanged();
    RefreshLiveGraphEditor();
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    const FString BeforeCollisionPreviewHash =
        UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph);
    const FMCPToolResult CollisionPreview = Registry.ExecuteTool(
        TEXT("blueprint.layout.organize"), OrganizeParams);
    if (!CollisionPreview.bSuccess) {
      AddInfo(FString::Printf(
          TEXT("Collision preview error [%s]: %s"),
          *CollisionPreview.ErrorCode, *CollisionPreview.ErrorMessage));
    }
    bool bFoundExternalNodeOverlap = false;
    bool bFoundExternalCommentContainment = false;
    if (CollisionPreview.bSuccess) {
      for (const TSharedPtr<FJsonValue> &Diagnostic :
           CollisionPreview.Data->GetArrayField(TEXT("layoutDiagnostics"))) {
        const TSharedPtr<FJsonObject> DiagnosticObject =
            Diagnostic->AsObject();
        const FString Rule =
            DiagnosticObject->GetStringField(TEXT("rule"));
        if (Rule == TEXT("node_overlap")) {
          bool bHasObstacle = false;
          for (const TSharedPtr<FJsonValue> &NodeId :
               DiagnosticObject->GetArrayField(TEXT("nodeIds"))) {
            FString Value;
            bHasObstacle |= NodeId->TryGetString(Value) &&
                            Value == ExternalObstacle->NodeGuid.ToString();
          }
          bFoundExternalNodeOverlap |= bHasObstacle;
        } else if (
            Rule == TEXT("affected_node_inside_external_comment")) {
          bFoundExternalCommentContainment = true;
        }
        AddInfo(TEXT("Collision preview diagnostic: ") + Rule);
      }
    }
    TestTrue(
        TEXT("Organizer preview cross-checks an ungrouped node obstacle"),
        CollisionPreview.bSuccess && bFoundExternalNodeOverlap);
    TestTrue(
        TEXT("Organizer preview cross-checks an ungrouped Comment"),
        CollisionPreview.bSuccess && bFoundExternalCommentContainment);
    TestEqual(
        TEXT("Collision preview does not mutate the live Graph"),
        UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
        BeforeCollisionPreviewHash);
    if (CollisionPreview.bSuccess) {
      TSharedPtr<FJsonObject> CollisionApplyParams =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      CollisionApplyParams->SetArrayField(
          TEXT("groups"),
          {MakeShared<FJsonValueObject>(Group)});
      CollisionApplyParams->SetBoolField(TEXT("dryRun"), false);
      CollisionApplyParams->SetStringField(
          TEXT("expectedGraphHash"),
          CollisionPreview.Data->GetStringField(TEXT("graphHash")));
      CollisionApplyParams->SetStringField(
          TEXT("approvePlanDigest"),
          CollisionPreview.Data->GetStringField(TEXT("planDigest")));
      const FMCPToolResult CollisionApply = Registry.ExecuteTool(
          TEXT("blueprint.layout.organize"), CollisionApplyParams);
      TestFalse(
          TEXT("Approved layout with an external collision is rejected"),
          CollisionApply.bSuccess);
      TestEqual(
          TEXT("External collision rejection is explicit"),
          CollisionApply.ErrorCode,
          FString(TEXT("layout_validation_failed")));
      TestEqual(
          TEXT("External collision rejection restores the complete Graph"),
          UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
          BeforeCollisionPreviewHash);
      TestFalse(
          TEXT("External collision rejection restores package cleanliness"),
          Blueprint->GetOutermost()->IsDirty());
    }

    Graph->RemoveNode(ExternalObstacle);
    ExternalObstacle = nullptr;
    if (Comment) {
      Graph->RemoveNode(Comment);
      Comment = nullptr;
    }
    Graph->NotifyGraphChanged();
    RefreshLiveGraphEditor();
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    const FString BeforeOrganizeHash =
        UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph);
    const FString BeforePreviewUndoName =
        GEditor->GetTransactionName().ToString();
    const int32 BeforePreviewSelectionCount =
        ConcreteBlueprintEditor.IsValid()
            ? ConcreteBlueprintEditor->GetSelectedNodes().Num()
            : 0;

    const FMCPToolResult PreviewResult = Registry.ExecuteTool(
        TEXT("blueprint.layout.organize"), OrganizeParams);
    if (!PreviewResult.bSuccess) {
      AddInfo(FString::Printf(
          TEXT("Organizer preview error [%s]: %s"),
          *PreviewResult.ErrorCode, *PreviewResult.ErrorMessage));
    } else {
      for (const TSharedPtr<FJsonValue> &Diagnostic :
           PreviewResult.Data->GetArrayField(TEXT("layoutDiagnostics"))) {
        AddInfo(TEXT("Organizer preview diagnostic: ") +
                Diagnostic->AsObject()->GetStringField(TEXT("rule")));
      }
    }
    TestTrue(TEXT("Atomic organizer dry-run succeeds"),
             PreviewResult.bSuccess);
    TestEqual(TEXT("Dry-run preserves graph hash"),
              UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
              BeforeOrganizeHash);
    TestFalse(TEXT("Dry-run preserves the clean package state"),
              Blueprint->GetOutermost()->IsDirty());
    TestEqual(TEXT("Dry-run does not append an Undo transaction"),
              GEditor->GetTransactionName().ToString(),
              BeforePreviewUndoName);
    TestEqual(
        TEXT("Dry-run preserves the live Graph Editor selection"),
        ConcreteBlueprintEditor.IsValid()
            ? ConcreteBlueprintEditor->GetSelectedNodes().Num()
            : 0,
        BeforePreviewSelectionCount);
    if (PreviewResult.bSuccess) {
      TestTrue(
          TEXT("Dry-run returns a predicted layout hash"),
          PreviewResult.Data
              ->GetStringField(TEXT("predictedLayoutHash"))
              .StartsWith(TEXT("sha256:")));
      const TSharedPtr<FJsonObject> GeometryFingerprint =
          PreviewResult.Data->GetObjectField(TEXT("geometryFingerprint"));
      TestTrue(
          TEXT("Dry-run returns an exact geometry fingerprint"),
          GeometryFingerprint->GetBoolField(TEXT("exact")) &&
              GeometryFingerprint->GetStringField(TEXT("boundsHash"))
                  .StartsWith(TEXT("sha256:")));
      const TSharedPtr<FJsonObject> ReturnedPlan =
          PreviewResult.Data->GetObjectField(TEXT("plan"));
      TestEqual(
          TEXT("Plan binds the predicted layout hash"),
          ReturnedPlan->GetStringField(TEXT("predictedLayoutHash")),
          PreviewResult.Data->GetStringField(TEXT("predictedLayoutHash")));
      FString ReturnedPlanDigest;
      const bool bReturnedPlanDigestReady =
          UEAIIntegration::Infrastructure::TryDigestJson(
              ReturnedPlan,
              ReturnedPlanDigest);
      TestTrue(TEXT("Returned organizer plan is digestible"),
               bReturnedPlanDigestReady);
      TestEqual(
          TEXT("Plan digest covers the complete returned plan"),
          PreviewResult.Data->GetStringField(TEXT("planDigest")),
          TEXT("sha256:") + ReturnedPlanDigest);
      const FMCPToolResult RepeatPreview = Registry.ExecuteTool(
          TEXT("blueprint.layout.organize"), OrganizeParams);
      TestTrue(TEXT("Repeated organizer preview succeeds"),
               RepeatPreview.bSuccess);
      if (RepeatPreview.bSuccess) {
        TestEqual(
            TEXT("Prediction hash is stable across transient Comment GUIDs"),
            RepeatPreview.Data->GetStringField(
                TEXT("predictedLayoutHash")),
            PreviewResult.Data->GetStringField(
                TEXT("predictedLayoutHash")));
        TestEqual(
            TEXT("Plan digest is stable across transient Comment GUIDs"),
            RepeatPreview.Data->GetStringField(TEXT("planDigest")),
            PreviewResult.Data->GetStringField(TEXT("planDigest")));
      }
      Blueprint->GetOutermost()->SetDirtyFlag(false);
      TestFalse(TEXT("Dry-run does not modify the asset"),
                PreviewResult.Data->GetBoolField(TEXT("assetModified")));
      TestFalse(TEXT("Dry-run does not modify the Undo stack"),
                PreviewResult.Data->GetBoolField(
                    TEXT("undoStackModified")));
      OrganizeParams->SetBoolField(TEXT("dryRun"), false);
      OrganizeParams->SetStringField(
          TEXT("expectedGraphHash"),
          TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
      OrganizeParams->SetStringField(
          TEXT("approvePlanDigest"),
          PreviewResult.Data->GetStringField(TEXT("planDigest")));
      const FMCPToolResult StaleHashResult = Registry.ExecuteTool(
          TEXT("blueprint.layout.organize"), OrganizeParams);
      TestFalse(TEXT("Organizer rejects a stale Graph hash"),
                StaleHashResult.bSuccess);
      TestEqual(TEXT("Stale hash rejection is explicit"),
                StaleHashResult.ErrorCode,
                FString(TEXT("graph_hash_mismatch")));
      TestEqual(TEXT("Stale hash rejection leaves the Graph untouched"),
                UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
                BeforeOrganizeHash);

      OrganizeParams->SetStringField(
          TEXT("expectedGraphHash"),
          PreviewResult.Data->GetStringField(TEXT("graphHash")));
      OrganizeParams->SetStringField(
          TEXT("approvePlanDigest"),
          TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"));
      const FMCPToolResult StaleDigestResult = Registry.ExecuteTool(
          TEXT("blueprint.layout.organize"), OrganizeParams);
      TestFalse(TEXT("Organizer rejects a stale plan digest"),
                StaleDigestResult.bSuccess);
      TestEqual(TEXT("Stale digest rejection is explicit"),
                StaleDigestResult.ErrorCode,
                FString(TEXT("plan_digest_mismatch")));
      TestEqual(TEXT("Stale digest rejection leaves the Graph untouched"),
                UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
                BeforeOrganizeHash);

      TSharedPtr<FJsonObject> MissingWorkflowApproval =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      MissingWorkflowApproval->SetArrayField(
          TEXT("groups"),
          {MakeShared<FJsonValueObject>(Group)});
      TSharedPtr<FJsonObject> MissingApprovalContext =
          MakeShared<FJsonObject>();
      MissingApprovalContext->SetBoolField(TEXT("approvedPlan"), true);
      MissingApprovalContext->SetBoolField(TEXT("deferCompile"), true);
      MissingWorkflowApproval->SetObjectField(
          TEXT("__ueWorkflow"),
          MissingApprovalContext);
      const FMCPToolResult MissingWorkflowApprovalResult =
          Registry.ExecuteTool(
              TEXT("blueprint.layout.organize"),
              MissingWorkflowApproval);
      TestFalse(
          TEXT("Workflow context cannot bypass layout Graph approval"),
          MissingWorkflowApprovalResult.bSuccess);
      TestEqual(
          TEXT("Missing Workflow Graph hash is explicit"),
          MissingWorkflowApprovalResult.ErrorCode,
          FString(TEXT("graph_hash_mismatch")));

      UK2Node_IfThenElse *FailureNode =
          AddBranchNode(Graph, 3000, 3000);
      Graph->NotifyGraphChanged();
      RefreshLiveGraphEditor();
      Blueprint->GetOutermost()->SetDirtyFlag(false);
      const FString BeforeFailingPreviewHash =
          UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph);
      TSharedPtr<FJsonObject> FailingParams =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      TSharedRef<FJsonObject> FirstFailingGroup =
          MakeShared<FJsonObject>();
      FirstFailingGroup->SetStringField(
          TEXT("id"), TEXT("partial-change"));
      FirstFailingGroup->SetArrayField(
          TEXT("nodeIds"),
          {MakeShared<FJsonValueString>(First->NodeGuid.ToString()),
           MakeShared<FJsonValueString>(Second->NodeGuid.ToString())});
      FirstFailingGroup->SetArrayField(
          TEXT("actions"),
          {MakeShared<FJsonValueObject>(AlignAction)});
      TSharedRef<FJsonObject> SecondFailingGroup =
          MakeShared<FJsonObject>();
      SecondFailingGroup->SetStringField(
          TEXT("id"), TEXT("forced-failure"));
      SecondFailingGroup->SetArrayField(
          TEXT("nodeIds"),
          {MakeShared<FJsonValueString>(Third->NodeGuid.ToString()),
           MakeShared<FJsonValueString>(FailureNode->NodeGuid.ToString())});
      TSharedRef<FJsonObject> StraightenAction =
          MakeShared<FJsonObject>();
      StraightenAction->SetStringField(TEXT("kind"), TEXT("straighten"));
      SecondFailingGroup->SetArrayField(
          TEXT("actions"),
          {MakeShared<FJsonValueObject>(StraightenAction)});
      FailingParams->SetArrayField(
          TEXT("groups"),
          {MakeShared<FJsonValueObject>(FirstFailingGroup),
           MakeShared<FJsonValueObject>(SecondFailingGroup)});
      FailingParams->SetBoolField(TEXT("dryRun"), true);
      const FMCPToolResult FailingResult = Registry.ExecuteTool(
          TEXT("blueprint.layout.organize"), FailingParams);
      TestFalse(
          TEXT("Organizer rejects a deterministic action failure in preview"),
          FailingResult.bSuccess);
      TestEqual(TEXT("Preview action failure is explicit"),
                FailingResult.ErrorCode,
                FString(TEXT("invalid_request")));
      TestEqual(TEXT("Failed preview preserves the complete Graph hash"),
                UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
                BeforeFailingPreviewHash);
      TestFalse(TEXT("Late failure restores the clean package state"),
                Blueprint->GetOutermost()->IsDirty());
      Graph->RemoveNode(FailureNode);
      Graph->NotifyGraphChanged();
      RefreshLiveGraphEditor();
      Blueprint->GetOutermost()->SetDirtyFlag(false);
      TestEqual(TEXT("Failure fixture restores the organizer baseline"),
                UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
                BeforeOrganizeHash);

      const FMCPToolResult SelectionBeforeApply = Registry.ExecuteTool(
          TEXT("blueprint.selection.set"),
          MakeSelectionParams(BlueprintPath, GraphName, {First, Third}));
      TestTrue(TEXT("Organizer selection baseline is established"),
               SelectionBeforeApply.bSuccess);
      OrganizeParams->SetStringField(
          TEXT("approvePlanDigest"),
          PreviewResult.Data->GetStringField(TEXT("planDigest")));
      const FMCPToolResult OrganizeResult = Registry.ExecuteTool(
          TEXT("blueprint.layout.organize"), OrganizeParams);
      if (!OrganizeResult.bSuccess) {
        AddInfo(FString::Printf(
            TEXT("Organizer apply error [%s]: %s"),
            *OrganizeResult.ErrorCode, *OrganizeResult.ErrorMessage));
      }
      TestTrue(TEXT("Approved atomic organizer succeeds"),
               OrganizeResult.bSuccess);
      if (OrganizeResult.bSuccess) {
        TestTrue(TEXT("Organizer changes graph structure hash"),
                 OrganizeResult.Data->GetStringField(
                     TEXT("afterGraphHash")) != BeforeOrganizeHash);
        TestEqual(TEXT("Organizer aligns all nodes at the top"),
                  First->NodePosY, Second->NodePosY);
        TestEqual(TEXT("Organizer aligns third node at the top"),
                  First->NodePosY, Third->NodePosY);
        TestEqual(TEXT("Organizer creates one native Comment"),
                  OrganizeResult.Data
                      ->GetArrayField(TEXT("createdCommentIds"))
                      .Num(),
                   1);
        TestEqual(
            TEXT("Organizer restores the original live selection"),
            ConcreteBlueprintEditor.IsValid()
                ? ConcreteBlueprintEditor->GetSelectedNodes().Num()
                : 0,
            2);
        TestTrue(TEXT("One Undo restores the whole organize transaction"),
                  GEditor->UndoTransaction());
        TestEqual(TEXT("Undo restores organizer graph hash"),
                   UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
                   BeforeOrganizeHash);

        const int32 NoOpSecondY = Second->NodePosY;
        Second->NodePosY = First->NodePosY;
        Graph->NotifyGraphChanged();
        Blueprint->GetOutermost()->SetDirtyFlag(false);
        TSharedRef<FJsonObject> NoOpGroup = MakeShared<FJsonObject>();
        NoOpGroup->SetStringField(TEXT("id"), TEXT("already-aligned"));
        NoOpGroup->SetArrayField(
            TEXT("nodeIds"),
            {MakeShared<FJsonValueString>(First->NodeGuid.ToString()),
             MakeShared<FJsonValueString>(Second->NodeGuid.ToString())});
        NoOpGroup->SetArrayField(
            TEXT("actions"),
            {MakeShared<FJsonValueObject>(AlignAction)});
        TSharedPtr<FJsonObject> NoOpParams =
            MakeBlueprintGraphParams(BlueprintPath, GraphName);
        NoOpParams->SetArrayField(
            TEXT("groups"),
            {MakeShared<FJsonValueObject>(NoOpGroup)});
        NoOpParams->SetBoolField(TEXT("dryRun"), true);
        const FMCPToolResult NoOpPreview = Registry.ExecuteTool(
            TEXT("blueprint.layout.organize"), NoOpParams);
        TestTrue(TEXT("Already-aligned organizer preview succeeds"),
                 NoOpPreview.bSuccess);
        if (NoOpPreview.bSuccess) {
          NoOpParams->SetBoolField(TEXT("dryRun"), false);
          NoOpParams->SetStringField(
              TEXT("expectedGraphHash"),
              NoOpPreview.Data->GetStringField(TEXT("graphHash")));
          NoOpParams->SetStringField(
              TEXT("approvePlanDigest"),
              NoOpPreview.Data->GetStringField(TEXT("planDigest")));
          const FString BeforeNoOpTransaction =
              GEditor->GetTransactionName().ToString();
          const FMCPToolResult NoOpApply = Registry.ExecuteTool(
              TEXT("blueprint.layout.organize"), NoOpParams);
          TestTrue(TEXT("Already-aligned organizer apply succeeds"),
                   NoOpApply.bSuccess);
          TestFalse(
              TEXT("Already-aligned organizer reports no mutation"),
              NoOpApply.bSuccess
                  ? NoOpApply.Data->GetObjectField(TEXT("mutation"))
                        ->GetBoolField(TEXT("changed"))
                  : true);
          TestFalse(TEXT("Already-aligned organizer preserves clean package"),
                    Blueprint->GetOutermost()->IsDirty());
          TestEqual(
              TEXT("Already-aligned organizer does not append Undo"),
              GEditor->GetTransactionName().ToString(),
              BeforeNoOpTransaction);

          FScopedTransaction UnrelatedTransaction(
              NSLOCTEXT(
                  "UEAIIntegrationTests",
                  "UnrelatedEditorTransaction",
                  "Unrelated Editor Transaction"));
          const FMCPToolResult BusyApply = Registry.ExecuteTool(
              TEXT("blueprint.layout.organize"), NoOpParams);
          TestFalse(
              TEXT("Direct organizer rejects an unrelated active transaction"),
              BusyApply.bSuccess);
          TestEqual(
              TEXT("Busy Editor transaction has a stable error"),
              BusyApply.ErrorCode,
              FString(TEXT("editor_transaction_busy")));
          UnrelatedTransaction.Cancel();
        }
        Second->NodePosY = NoOpSecondY;
        Graph->NotifyGraphChanged();
        Blueprint->GetOutermost()->SetDirtyFlag(false);
        TestEqual(
            TEXT("No-op fixture restores the Workflow baseline hash"),
            UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
            BeforeOrganizeHash);

        Blueprint->GetOutermost()->SetDirtyFlag(false);
        TSharedPtr<FJsonObject> Workflow = MakeShared<FJsonObject>();
        Workflow->SetStringField(TEXT("dsl"), TEXT("ue.workflow"));
        Workflow->SetStringField(TEXT("dslVersion"), TEXT("1.0"));
        Workflow->SetStringField(TEXT("workflowKind"), TEXT("assetEdit"));
        Workflow->SetStringField(
            TEXT("workflowId"),
            TEXT("organize-blueprint-layout"));
        Workflow->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
        TSharedPtr<FJsonObject> ScopeObject = MakeShared<FJsonObject>();
        ScopeObject->SetStringField(TEXT("kind"), TEXT("blueprint"));
        ScopeObject->SetStringField(TEXT("asset"), BlueprintPath);
        ScopeObject->SetBoolField(TEXT("createIfMissing"), false);
        Workflow->SetObjectField(TEXT("scope"), ScopeObject);
        TSharedPtr<FJsonObject> WorkflowOperation =
            MakeShared<FJsonObject>();
        WorkflowOperation->SetStringField(TEXT("id"), TEXT("organize"));
        WorkflowOperation->SetStringField(
            TEXT("type"),
            TEXT("blueprint.layout.organize"));
        TSharedPtr<FJsonObject> WorkflowParams = MakeShared<FJsonObject>();
        WorkflowParams->SetStringField(TEXT("graph"), GraphName);
        WorkflowParams->SetArrayField(
            TEXT("groups"),
            {MakeShared<FJsonValueObject>(Group)});
        WorkflowParams->SetStringField(
            TEXT("expectedGraphHash"),
            PreviewResult.Data->GetStringField(TEXT("graphHash")));
        WorkflowParams->SetStringField(
            TEXT("approvePlanDigest"),
            PreviewResult.Data->GetStringField(TEXT("planDigest")));
        WorkflowOperation->SetObjectField(TEXT("params"), WorkflowParams);
        Workflow->SetArrayField(
            TEXT("operations"),
            {MakeShared<FJsonValueObject>(WorkflowOperation)});
        TSharedPtr<FJsonObject> Verify = MakeShared<FJsonObject>();
        Verify->SetBoolField(TEXT("compile"), true);
        Verify->SetArrayField(
            TEXT("readBack"),
            {MakeShared<FJsonValueString>(TEXT("graphs"))});
        Workflow->SetObjectField(TEXT("verify"), Verify);

        UEAIIntegration::Workflow::FWorkflowRuntime WorkflowRuntime(
            Registry);
        TSharedPtr<FJsonObject> PlanRequest = MakeShared<FJsonObject>();
        PlanRequest->SetStringField(TEXT("action"), TEXT("plan"));
        PlanRequest->SetObjectField(TEXT("workflow"), Workflow);
        const FMCPResult WorkflowPlan =
            WorkflowRuntime.HandleRequest(PlanRequest);
        if (!WorkflowPlan.bOk) {
          AddInfo(FString::Printf(
              TEXT("Layout Workflow plan error [%s]: %s"),
              *WorkflowPlan.Error.Code,
              *WorkflowPlan.Error.Message));
        }
        TestTrue(TEXT("Layout organizer Workflow plans"),
                 WorkflowPlan.bOk);
        if (WorkflowPlan.bOk) {
          const FString WorkflowDigest =
              WorkflowPlan.Data->GetStringField(TEXT("planDigest"));
          TSharedPtr<FJsonObject> ExecuteRequest =
              MakeShared<FJsonObject>();
          ExecuteRequest->SetStringField(TEXT("action"), TEXT("execute"));
          ExecuteRequest->SetObjectField(TEXT("workflow"), Workflow);
          ExecuteRequest->SetStringField(
              TEXT("approvePlanDigest"),
              WorkflowDigest);
          ExecuteRequest->SetStringField(TEXT("detailLevel"), TEXT("full"));
          const FMCPResult WorkflowResult =
              WorkflowRuntime.HandleRequest(ExecuteRequest);
          TestTrue(
              TEXT("Workflow forces organizer apply instead of dry-run"),
              WorkflowResult.bOk);
          if (WorkflowResult.bOk) {
            TestTrue(
                TEXT("Workflow organizer changes the live Graph"),
                UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph)
                    != BeforeOrganizeHash);
            TSharedPtr<FJsonObject> RollbackRequest =
                MakeShared<FJsonObject>();
            RollbackRequest->SetStringField(
                TEXT("action"),
                TEXT("rollback"));
            RollbackRequest->SetStringField(
                TEXT("runId"),
                WorkflowResult.Data->GetStringField(TEXT("runId")));
            RollbackRequest->SetStringField(
                TEXT("approvePlanDigest"),
                WorkflowDigest);
            const FMCPResult RollbackResult =
                WorkflowRuntime.HandleRequest(RollbackRequest);
            TestTrue(TEXT("Layout Workflow rollback succeeds"),
                     RollbackResult.bOk);
            TestEqual(
                TEXT("Layout Workflow rollback restores Graph hash"),
                UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph),
                BeforeOrganizeHash);
          }
        }
      }
    }

    CaptureGraphEditor =
        ConcreteBlueprintEditor.IsValid()
            ? ConcreteBlueprintEditor->OpenGraphAndBringToFront(Graph, false)
            : nullptr;
    TestTrue(TEXT("Capture target is the requested SGraphEditor"),
             CaptureGraphEditor.IsValid() &&
                 CaptureGraphEditor->GetCurrentGraph() == Graph);
    if (CaptureGraphEditor.IsValid()) {
      CaptureGraphEditor->SetViewLocation(FVector2D(-300.0, -300.0), 0.5f);
      UEAIIntegration::BlueprintGraph::RefreshGraphEditorLayout(
          CaptureGraphEditor);
    }
    const FMCPToolResult CaptureSelectionResult = Registry.ExecuteTool(
        TEXT("blueprint.selection.set"),
        MakeSelectionParams(BlueprintPath, GraphName, {First, Third}));
    TestTrue(TEXT("Capture selection baseline is established"),
             CaptureSelectionResult.bSuccess);
    TestEqual(
        TEXT("Capture Graph Editor owns the expected live selection"),
        CaptureGraphEditor.IsValid()
            ? CaptureGraphEditor->GetSelectedNodes().Num()
            : 0,
        2);
    FVector2D ViewBeforeCapture;
    float ZoomBeforeCapture = 1.0f;
    if (CaptureGraphEditor.IsValid()) {
      CaptureGraphEditor->GetViewLocation(ViewBeforeCapture,
                                          ZoomBeforeCapture);
    }

    TArray<FString> CaptureIdsToDelete;
    auto TrackCapture =
        [&CaptureIdsToDelete](const FMCPToolResult &Result) -> FString {
      if (!Result.bSuccess || !Result.Data.IsValid()) {
        return FString();
      }
      const FString CaptureId =
          Result.Data->GetStringField(TEXT("captureId"));
      CaptureIdsToDelete.Add(CaptureId);
      return CaptureId;
    };
    TSharedPtr<FJsonObject> CaptureParams =
        MakeBlueprintGraphParams(BlueprintPath, GraphName);
    CaptureParams->SetStringField(TEXT("scope"), TEXT("currentView"));
    CaptureParams->SetStringField(TEXT("format"), TEXT("png"));
    CaptureParams->SetNumberField(TEXT("width"), 640);
    CaptureParams->SetNumberField(TEXT("height"), 360);
    const FString CaptureGraphHash =
        UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph);
    const FMCPToolResult BeforeCapture = Registry.ExecuteTool(
        TEXT("blueprint.graph.capture"), CaptureParams);
    if (!BeforeCapture.bSuccess) {
      AddInfo(FString::Printf(
          TEXT("Graph capture error [%s]: %s"),
          *BeforeCapture.ErrorCode, *BeforeCapture.ErrorMessage));
    }
    TestTrue(TEXT("Graph Editor capture succeeds"), BeforeCapture.bSuccess);
    if (BeforeCapture.bSuccess) {
      const FString CaptureProvider =
          BeforeCapture.Data->GetStringField(TEXT("captureProvider"));
      TestTrue(
          TEXT("Capture identifies its exact Graph rendering provider"),
          CaptureProvider == TEXT("slateLdr") ||
              CaptureProvider == TEXT("slateHdrFallback") ||
              CaptureProvider == TEXT("widgetRendererFallback"));
      TestFalse(TEXT("Capture provider is not a Level Viewport provider"),
                CaptureProvider.Contains(TEXT("viewport"),
                                         ESearchCase::IgnoreCase));
      TestEqual(TEXT("Capture metadata identifies the requested Blueprint"),
                BeforeCapture.Data->GetStringField(TEXT("blueprint")),
                Blueprint->GetPathName());
      TestEqual(TEXT("Capture metadata identifies the requested Graph"),
                BeforeCapture.Data->GetStringField(TEXT("graph")),
                GraphName);
      TestEqual(TEXT("Capture metadata binds the current Graph hash"),
                BeforeCapture.Data->GetStringField(TEXT("graphHash")),
                CaptureGraphHash);
      TestEqual(
          TEXT("Render fingerprint binds the capture provider"),
          BeforeCapture.Data
              ->GetObjectField(TEXT("renderFingerprint"))
              ->GetStringField(TEXT("captureProvider")),
          CaptureProvider);
      const TSharedPtr<FJsonObject> Fingerprint =
          BeforeCapture.Data->GetObjectField(TEXT("renderFingerprint"));
      TestEqual(TEXT("Render fingerprint identifies the requested Graph"),
                Fingerprint->GetStringField(TEXT("graph")), GraphName);
      if (CaptureGraphEditor.IsValid()) {
        const FVector2D GraphEditorSize =
            CaptureGraphEditor->GetCachedGeometry().GetLocalSize();
        TestTrue(
            TEXT("Capture fingerprint comes from the specified SGraphEditor"),
            FMath::IsNearlyEqual(
                Fingerprint->GetNumberField(TEXT("graphEditorWidth")),
                GraphEditorSize.X, 1.0) &&
                FMath::IsNearlyEqual(
                    Fingerprint->GetNumberField(TEXT("graphEditorHeight")),
                    GraphEditorSize.Y, 1.0));
      }
    }
    const FString BeforeCaptureId = TrackCapture(BeforeCapture);
    TestEqual(
        TEXT("Capture restores the live selection"),
        ConcreteBlueprintEditor.IsValid()
            ? ConcreteBlueprintEditor->GetSelectedNodes().Num()
            : 0,
        2);
    if (CaptureGraphEditor.IsValid()) {
      FVector2D ViewAfterCapture;
      float ZoomAfterCapture = 1.0f;
      CaptureGraphEditor->GetViewLocation(ViewAfterCapture,
                                          ZoomAfterCapture);
      TestTrue(TEXT("Capture restores Graph view location"),
               ViewAfterCapture.Equals(ViewBeforeCapture));
      TestEqual(TEXT("Capture restores Graph zoom"),
                ZoomAfterCapture, ZoomBeforeCapture);
    }

    if (BeforeCapture.bSuccess) {
      TSharedPtr<FJsonObject> GetCaptureParams =
          MakeShared<FJsonObject>();
      GetCaptureParams->SetStringField(
          TEXT("captureId"), BeforeCaptureId);
      const FMCPToolResult GetCaptureResult = Registry.ExecuteTool(
          TEXT("blueprint.graph.capture.get"), GetCaptureParams);
      TestTrue(TEXT("Persisted Graph capture can be retrieved"),
               GetCaptureResult.bSuccess);

      TSharedPtr<FJsonObject> NodesCaptureParams =
          MakeSelectionParams(BlueprintPath, GraphName, {First, Second});
      NodesCaptureParams->SetStringField(TEXT("scope"), TEXT("nodes"));
      NodesCaptureParams->SetStringField(TEXT("format"), TEXT("png"));
      NodesCaptureParams->SetNumberField(TEXT("width"), 640);
      NodesCaptureParams->SetNumberField(TEXT("height"), 360);
      const FMCPToolResult NodesCapture = Registry.ExecuteTool(
          TEXT("blueprint.graph.capture"), NodesCaptureParams);
      TestTrue(TEXT("Specified-node Graph capture succeeds"),
               NodesCapture.bSuccess);
      TrackCapture(NodesCapture);
      if (NodesCapture.bSuccess) {
        TestEqual(TEXT("Node capture records its exact target set"),
                  NodesCapture.Data
                      ->GetArrayField(TEXT("capturedNodeIds"))
                      .Num(),
                  2);
        TestEqual(TEXT("Node capture records requested-node selection"),
                  NodesCapture.Data->GetStringField(
                      TEXT("selectionPolicy")),
                  FString(TEXT("requestedNodes")));
      }
      TestEqual(
          TEXT("Node capture restores the prior selection"),
          ConcreteBlueprintEditor.IsValid()
              ? ConcreteBlueprintEditor->GetSelectedNodes().Num()
              : 0,
          2);

      TSharedPtr<FJsonObject> AllCaptureParams =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      AllCaptureParams->SetStringField(TEXT("scope"), TEXT("all"));
      AllCaptureParams->SetStringField(TEXT("format"), TEXT("png"));
      AllCaptureParams->SetNumberField(TEXT("width"), 640);
      AllCaptureParams->SetNumberField(TEXT("height"), 360);
      const FMCPToolResult AllCapture = Registry.ExecuteTool(
          TEXT("blueprint.graph.capture"), AllCaptureParams);
      if (!AllCapture.bSuccess) {
        AddInfo(FString::Printf(
            TEXT("Whole-Graph capture error [%s]: %s"),
            *AllCapture.ErrorCode, *AllCapture.ErrorMessage));
      }
      TestTrue(TEXT("Whole-Graph capture succeeds"), AllCapture.bSuccess);
      TrackCapture(AllCapture);
      if (AllCapture.bSuccess) {
        TestEqual(TEXT("Whole-Graph capture records every node"),
                  AllCapture.Data
                      ->GetArrayField(TEXT("capturedNodeIds"))
                      .Num(),
                  Graph->Nodes.Num());
      }

      const int32 OriginalFirstX = First->NodePosX;
      First->NodePosX += 180;
      Graph->NotifyGraphChanged();
      const FString MovedGraphHash =
          UEAIIntegration::BlueprintGraph::ComputeGraphHash(Graph);
      const FMCPToolResult AfterCapture = Registry.ExecuteTool(
          TEXT("blueprint.graph.capture"), CaptureParams);
      TestTrue(TEXT("Second Graph Editor capture succeeds"),
               AfterCapture.bSuccess);
      const FString AfterCaptureId = TrackCapture(AfterCapture);
      if (AfterCapture.bSuccess) {
        TestEqual(TEXT("Second capture records the moved Graph"),
                  AfterCapture.Data->GetStringField(TEXT("graphHash")),
                  MovedGraphHash);
        TestTrue(TEXT("Moving a node changes the captured Graph hash"),
                 MovedGraphHash != CaptureGraphHash);
      }
      First->NodePosX = OriginalFirstX;
      Graph->NotifyGraphChanged();
      if (AfterCapture.bSuccess) {
        TSharedPtr<FJsonObject> CompareParams = MakeShared<FJsonObject>();
        CompareParams->SetStringField(TEXT("beforeCaptureId"),
                                      BeforeCaptureId);
        CompareParams->SetStringField(TEXT("afterCaptureId"),
                                      AfterCaptureId);
        const FMCPToolResult CompareResult = Registry.ExecuteTool(
            TEXT("blueprint.graph.visual.compare"), CompareParams);
        TestTrue(TEXT("Graph visual comparison succeeds"),
                 CompareResult.bSuccess);
        TrackCapture(CompareResult);
        if (CompareResult.bSuccess) {
          TestTrue(TEXT("Visual comparison is fingerprint compatible"),
                   CompareResult.Data->GetBoolField(TEXT("compatible")));
          TestTrue(TEXT("Visual diff identifies changed pixels"),
                   CompareResult.Data->GetNumberField(
                       TEXT("changedPixelCount")) > 0.0);
          TestTrue(TEXT("Visual diff localizes a changed region"),
                   CompareResult.Data->HasTypedField<EJson::Object>(
                       TEXT("changedBounds")));
          if (CompareResult.Data->HasTypedField<EJson::Object>(
                  TEXT("changedBounds"))) {
            const TSharedPtr<FJsonObject> ChangedBounds =
                CompareResult.Data->GetObjectField(TEXT("changedBounds"));
            const double ChangedX =
                ChangedBounds->GetNumberField(TEXT("x"));
            const double ChangedY =
                ChangedBounds->GetNumberField(TEXT("y"));
            const double ChangedWidth =
                ChangedBounds->GetNumberField(TEXT("width"));
            const double ChangedHeight =
                ChangedBounds->GetNumberField(TEXT("height"));
            TestTrue(TEXT("Moved-node diff region has positive area"),
                     ChangedWidth > 0.0 && ChangedHeight > 0.0);
            TestTrue(TEXT("Moved-node diff region is inside the PNG"),
                     ChangedX >= 0.0 && ChangedY >= 0.0 &&
                         ChangedX + ChangedWidth <=
                             CompareResult.Data->GetNumberField(
                                 TEXT("width")) &&
                         ChangedY + ChangedHeight <=
                             CompareResult.Data->GetNumberField(
                                 TEXT("height")));
            TestTrue(TEXT("Moved-node diff is localized, not full-frame"),
                     ChangedWidth * ChangedHeight <
                         CompareResult.Data->GetNumberField(TEXT("width")) *
                             CompareResult.Data->GetNumberField(
                                 TEXT("height")));
          }
          const double ChangedPixelRatio =
              CompareResult.Data->GetNumberField(
                  TEXT("changedPixelRatio"));
          TestTrue(TEXT("Moved-node pixel ratio is bounded"),
                   ChangedPixelRatio > 0.0 &&
                       ChangedPixelRatio < 1.0);
          TestTrue(TEXT("Visual diff returns a PNG image"),
                   !CompareResult.Data
                        ->GetStringField(TEXT("image_base64"))
                        .IsEmpty());
        }
      }

      TSharedPtr<FJsonObject> IncompatibleCaptureParams =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      IncompatibleCaptureParams->SetStringField(
          TEXT("scope"), TEXT("currentView"));
      IncompatibleCaptureParams->SetStringField(
          TEXT("format"), TEXT("png"));
      IncompatibleCaptureParams->SetNumberField(TEXT("width"), 800);
      IncompatibleCaptureParams->SetNumberField(TEXT("height"), 360);
      const FMCPToolResult IncompatibleCapture = Registry.ExecuteTool(
          TEXT("blueprint.graph.capture"), IncompatibleCaptureParams);
      TestTrue(TEXT("Different-size Graph capture succeeds"),
               IncompatibleCapture.bSuccess);
      const FString IncompatibleCaptureId =
          TrackCapture(IncompatibleCapture);
      if (IncompatibleCapture.bSuccess) {
        TSharedPtr<FJsonObject> IncompatibleCompareParams =
            MakeShared<FJsonObject>();
        IncompatibleCompareParams->SetStringField(
            TEXT("beforeCaptureId"), BeforeCaptureId);
        IncompatibleCompareParams->SetStringField(
            TEXT("afterCaptureId"), IncompatibleCaptureId);
        const FMCPToolResult IncompatibleCompare =
            Registry.ExecuteTool(
                TEXT("blueprint.graph.visual.compare"),
                IncompatibleCompareParams);
        TestTrue(TEXT("Incompatible comparison returns evidence"),
                 IncompatibleCompare.bSuccess);
        TrackCapture(IncompatibleCompare);
        if (IncompatibleCompare.bSuccess) {
          TestFalse(TEXT("Different render fingerprints are incompatible"),
                    IncompatibleCompare.Data->GetBoolField(
                        TEXT("compatible")));
          TestEqual(TEXT("Incompatible comparison is inconclusive"),
                    IncompatibleCompare.Data->GetStringField(
                        TEXT("comparison")),
                    FString(TEXT("inconclusive")));
          TestTrue(TEXT("Incompatibility includes concrete reasons"),
                   !IncompatibleCompare.Data
                        ->GetArrayField(TEXT("incompatibilityReasons"))
                        .IsEmpty());
          double InvalidMetric = 0.0;
          TestFalse(TEXT("Inconclusive ratio is null, not a false zero"),
                    IncompatibleCompare.Data->TryGetNumberField(
                        TEXT("changedPixelRatio"), InvalidMetric));
        }
      }

      TSharedPtr<FJsonObject> JpegCaptureParams =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      JpegCaptureParams->SetStringField(
          TEXT("scope"), TEXT("currentView"));
      JpegCaptureParams->SetStringField(TEXT("format"), TEXT("jpeg"));
      JpegCaptureParams->SetNumberField(TEXT("width"), 640);
      JpegCaptureParams->SetNumberField(TEXT("height"), 360);
      const FMCPToolResult JpegCapture = Registry.ExecuteTool(
          TEXT("blueprint.graph.capture"), JpegCaptureParams);
      TestTrue(TEXT("JPEG Graph capture remains available for inspection"),
               JpegCapture.bSuccess);
      const FString JpegCaptureId = TrackCapture(JpegCapture);
      if (JpegCapture.bSuccess) {
        TSharedPtr<FJsonObject> LossyCompareParams =
            MakeShared<FJsonObject>();
        LossyCompareParams->SetStringField(
            TEXT("beforeCaptureId"), BeforeCaptureId);
        LossyCompareParams->SetStringField(
            TEXT("afterCaptureId"), JpegCaptureId);
        const FMCPToolResult LossyCompare = Registry.ExecuteTool(
            TEXT("blueprint.graph.visual.compare"),
            LossyCompareParams);
        TestTrue(TEXT("Lossy comparison returns bounded evidence"),
                 LossyCompare.bSuccess);
        TrackCapture(LossyCompare);
        if (LossyCompare.bSuccess) {
          TestFalse(TEXT("JPEG cannot enter pixel-regression comparison"),
                    LossyCompare.Data->GetBoolField(TEXT("compatible")));
          TestEqual(TEXT("JPEG comparison is inconclusive"),
                    LossyCompare.Data->GetStringField(
                        TEXT("comparison")),
                    FString(TEXT("inconclusive")));
          TestTrue(
              TEXT("JPEG incompatibility has an explicit reason"),
              LossyCompare.Data
                  ->GetArrayField(TEXT("incompatibilityReasons"))
                  .ContainsByPredicate(
                      [](const TSharedPtr<FJsonValue> &Reason) {
                        return Reason.IsValid() &&
                               Reason->AsString() ==
                                   TEXT("visualCompareRequiresPng");
                      }));
          double InvalidLossyRatio = 0.0;
          TestFalse(
              TEXT("JPEG comparison does not expose a false pixel ratio"),
              LossyCompare.Data->TryGetNumberField(
                  TEXT("changedPixelRatio"), InvalidLossyRatio));
        }
      }
    }
    for (const FString &CaptureId : CaptureIdsToDelete) {
      IFileManager::Get().Delete(
          *FPaths::Combine(FPaths::ProjectSavedDir(),
                           TEXT("UE_AI_integration"),
                           TEXT("BlueprintGraphCaptures"),
                           CaptureId + TEXT(".png")));
      IFileManager::Get().Delete(
          *FPaths::Combine(FPaths::ProjectSavedDir(),
                           TEXT("UE_AI_integration"),
                           TEXT("BlueprintGraphCaptures"),
                           CaptureId + TEXT(".jpg")));
      IFileManager::Get().Delete(
          *FPaths::Combine(FPaths::ProjectSavedDir(),
                           TEXT("UE_AI_integration"),
                           TEXT("BlueprintGraphCaptures"),
                           CaptureId + TEXT(".json")));
    }
  } else {
    AddInfo(TEXT("Comment bounds require a rendered Graph Editor; covered by "
                 "the live Editor MCP closed-loop test."));
  }

  TSharedPtr<FJsonObject> ValidateParams = MakeShared<FJsonObject>();
  ValidateParams->SetStringField(TEXT("blueprint"), BlueprintPath);
  const FMCPToolResult ValidateResult =
      Registry.ExecuteTool(TEXT("blueprint.asset.validate"), ValidateParams);
  TestTrue(TEXT("Edited Blueprint validates"), ValidateResult.bSuccess);
  if (ValidateResult.bSuccess) {
    TestEqual(TEXT("Blueprint status is UpToDate"),
              ValidateResult.Data->GetStringField(TEXT("status")),
              FString(TEXT("UpToDate")));
    TestEqual(TEXT("Blueprint has no errors"),
              static_cast<int32>(
                  ValidateResult.Data->GetNumberField(TEXT("errorCount"))),
              0);
    TestEqual(TEXT("Blueprint has no warnings"),
              static_cast<int32>(
                  ValidateResult.Data->GetNumberField(TEXT("warningCount"))),
              0);
  }

  UEAIIntegration::BlueprintGraph::GraphEditorCache.Remove(
      Graph->GetPathName());
  CaptureGraphEditor.Reset();
  ConcreteBlueprintEditor.Reset();
  BlueprintEditor.Reset();
  TestTrue(TEXT("Temporary Blueprint is removed"), CleanupBlueprint(Blueprint));
  return true;
}

#endif
