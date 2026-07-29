#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationSubsystem.h"

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
    // unsupported. The fixture is never saved, so clearing its dirty flag is
    // sufficient for an isolated headless automation process.
    Blueprint->GetOutermost()->SetDirtyFlag(false);
    return true;
  }
  if (GEditor) {
    if (UAssetEditorSubsystem *AssetEditorSubsystem =
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()) {
      AssetEditorSubsystem->CloseAllEditorsForAsset(Blueprint);
    }
  }
  Blueprint->GetOutermost()->SetDirtyFlag(false);
  TArray<FAssetData> Assets{FAssetData(Blueprint)};
  ObjectTools::DeleteAssets(Assets, false);
  return !IsValid(Blueprint);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FBlueprintEditorNativeLayoutCommandsTest,
    "UE_AI_integration.Blueprint.EditorLayout.NativeCommandLoop",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintEditorNativeLayoutCommandsTest::RunTest(
    const FString &Parameters) {
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
  TestTrue(TEXT("Selection opened the Blueprint Editor"),
           SelectionResult.Data->GetBoolField(TEXT("editorOpened")));
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
  TSharedPtr<FJsonObject> DistributeParams =
      MakeBlueprintGraphParams(BlueprintPath, GraphName);
  DistributeParams->SetStringField(TEXT("orientation"), TEXT("horizontal"));
  const FMCPToolResult DistributeResult = Registry.ExecuteTool(
      TEXT("blueprint.layout.distribute"), DistributeParams);
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
      TestTrue(TEXT("Comment bounds contain selected node positions"),
               Comment->NodePosX <= First->NodePosX &&
                   Comment->NodePosY <= First->NodePosY &&
                   Comment->NodePosX + Comment->NodeWidth >= Third->NodePosX &&
                   Comment->NodePosY + Comment->NodeHeight >= Third->NodePosY);

      TSharedPtr<FJsonObject> BoundsParams =
          MakeBlueprintGraphParams(BlueprintPath, GraphName);
      BoundsParams->SetStringField(TEXT("commentNodeId"), CommentNodeId);
      BoundsParams->SetNumberField(TEXT("x"), -100);
      BoundsParams->SetNumberField(TEXT("y"), -200);
      BoundsParams->SetNumberField(TEXT("width"), 1200);
      BoundsParams->SetNumberField(TEXT("height"), 800);
      const FMCPToolResult BoundsResult = Registry.ExecuteTool(
          TEXT("blueprint.comment.bounds.set"), BoundsParams);
      TestTrue(TEXT("Comment bounds update succeeds"), BoundsResult.bSuccess);
      TestEqual(TEXT("Comment bounds X"), Comment->NodePosX, -100);
      TestEqual(TEXT("Comment bounds Y"), Comment->NodePosY, -200);
      TestEqual(TEXT("Comment bounds width"), Comment->NodeWidth, 1200);
      TestEqual(TEXT("Comment bounds height"), Comment->NodeHeight, 800);
    }
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

  TestTrue(TEXT("Temporary Blueprint is removed"), CleanupBlueprint(Blueprint));
  return true;
}

#endif
