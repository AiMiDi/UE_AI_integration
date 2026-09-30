#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Workflow/UEWorkflowRuntime.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialEditorModule.h"
#include "MaterialEditorActions.h"
#include "IMaterialEditor.h"
#include "GraphEditor.h"
#include "Framework/Commands/UICommandList.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "UObject/GCObject.h"
#include "RHI.h"

namespace
{
using namespace MCPMaterialInfrastructure;
struct FPreviewFixture : FGCObject
{
	UObject* Asset = nullptr;
	UMaterial* Preview = nullptr;
	TSharedPtr<IMaterialEditor> Editor;
	FMCPToolRegistry* Registry = nullptr;
	FString Identity;
	bool bFunction;
	explicit FPreviewFixture(bool InFunction) : bFunction(InFunction)
	{
		Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
		auto* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/PreviewGraph_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		Asset = bFunction ? static_cast<UObject*>(NewObject<UMaterialFunction>(Package, TEXT("Function"), RF_Transactional)) : NewObject<UMaterial>(Package, TEXT("Material"), RF_Transactional);
		auto* Seed = NewObject<UMaterialExpressionConstant>(Asset, TEXT("Seed"), RF_Transactional); Seed->R = 0.2f;
		if (auto* Function = Cast<UMaterialFunction>(Asset))
		{
			auto* Output = NewObject<UMaterialExpressionFunctionOutput>(Asset, TEXT("Result"), RF_Transactional);
			Output->OutputName = TEXT("Result"); Output->Id = FGuid::NewGuid(); Output->A.Connect(0, Seed);
			Function->GetExpressionCollection().AddExpression(Seed); Function->GetExpressionCollection().AddExpression(Output); Seed->Function = Function; Output->Function = Function;
			Editor = IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, nullptr, Function);
		}
		else
		{
			auto* Material = CastChecked<UMaterial>(Asset); Material->SetShadingModel(MSM_Unlit); Seed->Material = Material;
			Material->GetExpressionCollection().AddExpression(Seed); Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Seed);
			Editor = IMaterialEditorModule::Get().CreateMaterialEditor(EToolkitMode::Standalone, nullptr, Material);
		}
		Preview = CastChecked<UMaterial>(Editor->GetMaterialInterface());
		const auto Context = Call(TEXT("content.material.editor.context.get"), Scope(false));
		if (Context.bSuccess) Identity = Context.Data->GetStringField(TEXT("previewId"));
		Package->SetDirtyFlag(false);
	}
	~FPreviewFixture() { if (Editor) Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted); }
	void AddReferencedObjects(FReferenceCollector& Collector) override { Collector.AddReferencedObject(Asset); }
	FString GetReferencerName() const override { return TEXT("FUEAIMaterialPreviewFixture"); }
	TSharedRef<FJsonObject> Scope(bool bPreview = true) const
	{
		auto P = MakeShared<FJsonObject>(); P->SetStringField(bFunction ? TEXT("materialFunction") : TEXT("material"), Asset->GetPathName());
		if (bPreview) { P->SetStringField(TEXT("targetContext"), TEXT("editorPreview")); P->SetStringField(TEXT("expectedPreviewId"), Identity); }
		return P;
	}
	FMCPToolResult Call(const TCHAR* Id, const TSharedPtr<FJsonObject>& P) const { return Registry->FindTool(Id)->Execute(P); }
	FString Hash() const { return UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Preview); }
	UMaterialExpression* Find(const FString& Id) const { for (UMaterialExpression* E : Preview->GetExpressions()) if (E && ExpressionNodeId(E) == Id) return E; return nullptr; }
	FString Add(const TCHAR* Class) const { auto P = Scope(); P->SetStringField(TEXT("expressionClass"), Class); const auto R = Call(TEXT("content.material.expression.add"), P); return R.bSuccess ? R.Data->GetStringField(TEXT("nodeId")) : FString(); }
	TSharedRef<FJsonObject> Wire(const FString& From, const TCHAR* Out, const FString& To, const TCHAR* In) const
	{
		auto P = Scope(); P->SetStringField(TEXT("sourceNodeId"), From); P->SetStringField(TEXT("sourcePinName"), Out); P->SetStringField(TEXT("targetNodeId"), To); P->SetStringField(TEXT("targetPinName"), In); return P;
	}
	TSharedRef<FJsonObject> Batch(const TArray<TSharedPtr<FJsonValue>>& Ops, bool Refresh = false) const
	{
		auto P = Scope(false); P->SetStringField(TEXT("expectedPreviewId"), Identity); P->SetArrayField(TEXT("operations"), Ops); P->SetBoolField(TEXT("refresh"), Refresh); return P;
	}
};
TSharedPtr<FJsonValue> Operation(const TCHAR* Id, const TCHAR* Capability, const TSharedRef<FJsonObject>& Params)
{
	auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("id"), Id); P->SetStringField(TEXT("capability"), Capability); P->SetObjectField(TEXT("params"), Params); return MakeShared<FJsonValueObject>(P);
}
TSharedRef<FJsonObject> Fields(std::initializer_list<TPair<const TCHAR*, FString>> Values)
{
	auto P = MakeShared<FJsonObject>(); for (const auto& Pair : Values) P->SetStringField(Pair.Key, Pair.Value); return P;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewGraphCrudTest, "UE_AI_integration.MaterialEditor.GraphCrudAndSnapshots", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewGraphCrudTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Native graph preview test requires a rendering Editor.")); return true; }
	for (bool bFunction : {false, true})
	{
		AddInfo(bFunction ? TEXT("Checking function preview graph") : TEXT("Checking material preview graph"));
		FPreviewFixture F(bFunction); if (!TestFalse(TEXT("Open preview identity"), F.Identity.IsEmpty())) return false;
		const FString OriginalHash = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(F.Asset);
		int32 Updates = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == F.Preview) ++Updates; });
		const FString CustomId = F.Add(TEXT("Custom")), ParameterId = F.Add(TEXT("ScalarParameter"));
		if (!TestFalse(TEXT("Create Custom"), CustomId.IsEmpty()) || !TestFalse(TEXT("Create parameter"), ParameterId.IsEmpty())) { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle); return false; }
		auto P = F.Scope(); P->SetStringField(TEXT("nodeId"), CustomId); P->SetStringField(TEXT("code"), TEXT("return Strength * float3(0.2,0.4,0.6);")); P->SetStringField(TEXT("outputType"), TEXT("Float3"));
		P->SetArrayField(TEXT("inputs"), {MakeShared<FJsonValueObject>(Fields({{TEXT("name"),TEXT("Strength")}}))});
		TestTrue(TEXT("Configure newly created Custom"), F.Call(TEXT("content.material.custom.set"), P).bSuccess);
		auto Parameter = F.Scope(); Parameter->SetStringField(TEXT("nodeId"), ParameterId); Parameter->SetStringField(TEXT("name"), TEXT("Strength")); Parameter->SetNumberField(TEXT("defaultValue"), 0.75);
		TestTrue(TEXT("Configure newly created parameter"), F.Call(TEXT("content.material.parameter.set"), Parameter).bSuccess);
		TestTrue(TEXT("Connect parameter into Custom"), F.Call(TEXT("content.material.pin.connect"), F.Wire(ParameterId, TEXT("Output"), CustomId, TEXT("Strength"))).bSuccess);
		TestTrue(TEXT("Connect Custom to output"), F.Call(TEXT("content.material.pin.connect"), F.Wire(CustomId, TEXT("Output"), bFunction ? TEXT("expr:Result") : TEXT("root"), bFunction ? TEXT("index:0") : TEXT("Emissive Color"))).bSuccess);
		auto Move = F.Scope(); Move->SetStringField(TEXT("nodeId"), CustomId); Move->SetNumberField(TEXT("posX"), -240); Move->SetNumberField(TEXT("posY"), 80);
		TestTrue(TEXT("Move preview expression"), F.Call(TEXT("content.material.expression.move"), Move).bSuccess);
		TestFalse(TEXT("Reject a direct loop before editing"), F.Call(TEXT("content.material.pin.connect"), F.Wire(CustomId, TEXT("Output"), CustomId, TEXT("Strength"))).bSuccess);
		auto Disconnect = F.Scope(); Disconnect->SetStringField(TEXT("nodeId"), CustomId); Disconnect->SetStringField(TEXT("pinName"), TEXT("index:0")); Disconnect->SetStringField(TEXT("direction"), TEXT("input"));
		TestTrue(TEXT("Disconnect explicit input direction"), F.Call(TEXT("content.material.pin.disconnect"), Disconnect).bSuccess);
		auto AssetDisconnect = F.Scope(false); AssetDisconnect->SetStringField(TEXT("nodeId"), CustomId); AssetDisconnect->SetStringField(TEXT("pinName"), TEXT("index:0")); AssetDisconnect->SetStringField(TEXT("direction"), TEXT("input"));
		TestFalse(TEXT("Preview-only direction cannot silently edit an asset"), F.Call(TEXT("content.material.pin.disconnect"), AssetDisconnect).bSuccess);
		TestTrue(TEXT("Reconnect Custom input"), F.Call(TEXT("content.material.pin.connect"), F.Wire(ParameterId, TEXT("Output"), CustomId, TEXT("Strength"))).bSuccess);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
		TestEqual(TEXT("Create/configure/move/connect batch does not update base preview"), Updates, 0);
		TestEqual(TEXT("Original asset remains untouched"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(F.Asset), OriginalHash);
		TestFalse(TEXT("Original package is not saved or dirtied"), F.Asset->GetOutermost()->IsDirty());
		auto Index = Fields({{TEXT("assetPath"),F.Asset->GetPathName()},{TEXT("targetContext"),TEXT("editorPreview")},{TEXT("expectedPreviewId"),F.Identity}});
		const auto Snapshot = F.Call(TEXT("content.material.graph.index"), Index);
		if (!TestTrue(TEXT("GraphIR captures the edited working collection"), Snapshot.bSuccess)) { AddError(Snapshot.ErrorMessage); return false; }
		TestEqual(TEXT("Snapshot records preview identity"), Snapshot.Data->GetStringField(TEXT("previewId")), F.Identity);
		TestEqual(TEXT("Snapshot reports persistent asset path"), Snapshot.Data->GetStringField(TEXT("assetPath")), F.Asset->GetPathName());
		const int32 BeforeNodes = Snapshot.Data->GetIntegerField(TEXT("totalNodes"));
		// Native deletion must clear a selected-expression preview, even before the
		// graph's bIsPreviewExpression display flag has been updated by an editor tick.
		if (auto GraphEditor = SGraphEditor::FindGraphEditorForGraph(F.Preview->MaterialGraph)) { GraphEditor->ClearSelectionSet(); GraphEditor->SetNodeSelection(F.Find(CustomId)->GraphNode, true); }
		F.Editor->GetToolkitCommands()->ExecuteAction(FMaterialEditorCommands::Get().StartPreviewNode.ToSharedRef());
		auto Delete = F.Scope(); Delete->SetStringField(TEXT("nodeId"), CustomId);
		const FString BeforeDelete = F.Hash(); const auto Deleted = F.Call(TEXT("content.material.expression.delete"), Delete);
		if (!TestTrue(TEXT("Native preview deletion completes"), Deleted.bSuccess)) { AddError(Deleted.ErrorMessage); return false; }
		TestTrue(TEXT("Native deletion refresh is reported"), Deleted.Data->GetBoolField(TEXT("nativeEditorRefresh")));
		TestNull(TEXT("Deleted Custom leaves the working collection"), F.Find(CustomId));
		const auto NewSnapshot = F.Call(TEXT("content.material.graph.index"), Index);
		if (!TestTrue(TEXT("Capture after deletion"), NewSnapshot.bSuccess)) return false;
		TestEqual(TEXT("Live capture observes deletion"), NewSnapshot.Data->GetIntegerField(TEXT("totalNodes")), BeforeNodes - 1);
		auto DiffParams = MakeShared<FJsonObject>(); DiffParams->SetStringField(TEXT("beforeSnapshotId"), Snapshot.Data->GetStringField(TEXT("snapshotId"))); DiffParams->SetStringField(TEXT("afterSnapshotId"), NewSnapshot.Data->GetStringField(TEXT("snapshotId")));
		const auto Compared = F.Call(TEXT("content.material.graph.snapshots.diff"), DiffParams);
		if (!TestTrue(TEXT("Registered diff compares same preview session"), Compared.bSuccess)) return false;
		bool FoundRemoved = false;
		for (const auto& V : Compared.Data->GetArrayField(TEXT("changes")))
		{
			const auto Change = V->AsObject();
			if (Change->GetStringField(TEXT("entity")) == TEXT("node") && Change->GetStringField(TEXT("change")) == TEXT("removed")) FoundRemoved |= Change->GetStringField(TEXT("nodeId")) == CustomId;
		}
		TestTrue(TEXT("Preview diff identifies removed Custom node"), FoundRemoved);
		auto Page = Fields({{TEXT("snapshotId"),Snapshot.Data->GetStringField(TEXT("snapshotId"))}}); Page->SetNumberField(TEXT("limit"), 100);
		const auto Old = F.Call(TEXT("content.material.graph.nodes.list"), Page);
		if (!TestTrue(TEXT("Read captured snapshot"), Old.bSuccess)) return false;
		TestEqual(TEXT("Old snapshot remains immutable"), Old.Data->GetIntegerField(TEXT("totalNodes")), BeforeNodes);
		TestTrue(TEXT("Native undo restores deletion"), GEditor->UndoTransaction());
		TestNotNull(TEXT("Deleted Custom is restored"), F.Find(CustomId)); TestEqual(TEXT("Deletion undo restores graph structure"), F.Hash(), BeforeDelete);
		if (bFunction)
		{
			const FString InputId = F.Add(TEXT("FunctionInput")); auto Interface = F.Scope(); Interface->SetStringField(TEXT("nodeId"), InputId); Interface->SetStringField(TEXT("name"), TEXT("Weight")); Interface->SetStringField(TEXT("inputType"), TEXT("Scalar"));
			TestTrue(TEXT("New function interface can be configured in preview"), F.Call(TEXT("content.material.function.interface.set"), Interface).bSuccess);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewBatchTest, "UE_AI_integration.MaterialEditor.BatchAtomicity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewBatchTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Native preview batch test requires a rendering Editor.")); return true; }
	for (bool bFunction : {false, true})
	{
		AddInfo(bFunction ? TEXT("Checking function preview batch") : TEXT("Checking material preview batch"));
		FPreviewFixture F(bFunction); const FString Before = F.Hash();
		auto Configure = Fields({{TEXT("nodeId"),TEXT("$custom")},{TEXT("code"),TEXT("return float3(0.1,0.2,0.3);")},{TEXT("outputType"),TEXT("Float3")}}); Configure->SetArrayField(TEXT("inputs"), {});
		auto Connect = Fields({{TEXT("sourceNodeId"),TEXT("$custom")},{TEXT("sourcePinName"),TEXT("Output")},{TEXT("targetNodeId"),bFunction?TEXT("expr:Result"):TEXT("root")},{TEXT("targetPinName"),bFunction?TEXT("index:0"):TEXT("Emissive Color")}});
		auto Batch = F.Batch({Operation(TEXT("custom"),TEXT("content.material.expression.add"),Fields({{TEXT("expressionClass"),TEXT("Custom")}})), Operation(TEXT("configure"),TEXT("content.material.custom.set"),Configure), Operation(TEXT("connect"),TEXT("content.material.pin.connect"),Connect)}, true);
		Batch->SetStringField(TEXT("expectedStateHash"), Before); Batch->SetBoolField(TEXT("requireValidShader"), !bFunction);
		int32 Updates = 0; const auto Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == F.Preview) ++Updates; });
		const auto Completed = F.Call(TEXT("content.material.editor.batch"), Batch); FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
		if (!TestTrue(TEXT("Batch completes"), Completed.bSuccess && Completed.Data->GetBoolField(TEXT("success")))) { if (Completed.bSuccess) AddError(Completed.Data->GetStringField(TEXT("error"))); else AddError(Completed.ErrorMessage); return false; }
		TestEqual(TEXT("Batch compiles base preview only once"), Updates, 1);
		TestFalse(TEXT("Batch never saves"), Completed.Data->GetBoolField(TEXT("saved")));
		const FString After = F.Hash(), CustomId = Completed.Data->GetArrayField(TEXT("operations"))[0]->AsObject()->GetStringField(TEXT("nodeId"));
		TestTrue(TEXT("One undo rolls back the whole successful batch"), GEditor->UndoTransaction()); TestEqual(TEXT("Successful batch undo restores graph"), F.Hash(), Before);
		TestTrue(TEXT("One redo restores the whole successful batch"), GEditor->RedoTransaction()); TestEqual(TEXT("Batch redo restores graph"), F.Hash(), After);
		auto ChangedCode = Fields({{TEXT("nodeId"),CustomId},{TEXT("code"),TEXT("return float3(0.8,0.2,0.1);")}});
		auto BadMove = Fields({{TEXT("nodeId"),TEXT("expr:DoesNotExist")}}); BadMove->SetNumberField(TEXT("posX"), 0); BadMove->SetNumberField(TEXT("posY"), 0);
		const auto NoChangeFailure = F.Call(TEXT("content.material.editor.batch"), F.Batch({Operation(TEXT("missing"),TEXT("content.material.expression.move"),BadMove)}));
		if (!TestTrue(TEXT("Failure before editing is verified without undo"), NoChangeFailure.bSuccess && NoChangeFailure.Data->GetBoolField(TEXT("rollbackVerified")))) return false;
		TestFalse(TEXT("Failure before editing does not consume a previous transaction"), NoChangeFailure.Data->GetBoolField(TEXT("nativeUndoApplied")));
		TestEqual(TEXT("Failure before editing preserves the existing graph"), F.Hash(), After);
		const auto Failed = F.Call(TEXT("content.material.editor.batch"), F.Batch({Operation(TEXT("change"),TEXT("content.material.custom.set"),ChangedCode),Operation(TEXT("missing"),TEXT("content.material.expression.move"),BadMove)}));
		if (!TestTrue(TEXT("Runtime failure returns verified rollback"), Failed.bSuccess && Failed.Data->GetBoolField(TEXT("rollbackVerified")))) { AddError(Failed.ErrorMessage); return false; }
		TestEqual(TEXT("Runtime failure restores the working copy"), F.Hash(), After);
		auto Override = Fields({{TEXT("nodeId"),CustomId},{TEXT("code"),TEXT("return 1;")},{TEXT("material"),F.Asset->GetPathName()}});
		TestFalse(TEXT("All nested scopes rejected before any write"), F.Call(TEXT("content.material.editor.batch"), F.Batch({Operation(TEXT("change"),TEXT("content.material.custom.set"),ChangedCode),Operation(TEXT("override"),TEXT("content.material.custom.set"),Override)})).bSuccess);
		TestEqual(TEXT("Schema preflight is read-only"), F.Hash(), After);
		const auto DeletedFailure = F.Call(TEXT("content.material.editor.batch"), F.Batch({Operation(TEXT("delete"),TEXT("content.material.expression.delete"),Fields({{TEXT("nodeId"),CustomId}})),Operation(TEXT("missing"),TEXT("content.material.expression.move"),BadMove)}));
		if (!TestTrue(TEXT("Native deletion is restored after later batch failure"), DeletedFailure.bSuccess && DeletedFailure.Data->GetBoolField(TEXT("rollbackVerified")))) { AddError(DeletedFailure.ErrorMessage); return false; }
		TestEqual(TEXT("Deletion rollback restores graph"), F.Hash(), After);
		if (!bFunction)
		{
			auto BadCode = Fields({{TEXT("nodeId"),CustomId},{TEXT("code"),TEXT("return UEAI_BATCH_UNKNOWN_IDENTIFIER;")}});
			auto CompileFailure = F.Batch({Operation(TEXT("broken"),TEXT("content.material.custom.set"),BadCode)},true); CompileFailure->SetBoolField(TEXT("requireValidShader"),true);
			const auto Rejected = F.Call(TEXT("content.material.editor.batch"),CompileFailure);
			TestTrue(TEXT("Shader failure rolls back when validity is required"),Rejected.bSuccess && Rejected.Data->GetBoolField(TEXT("rollbackVerified"))); TestEqual(TEXT("Shader failure restores code and graph"),F.Hash(),After);
		}
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPreviewDeletionBatchTest, "UE_AI_integration.MaterialEditor.DeletionBatchRefresh", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPreviewDeletionBatchTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Native deletion test requires a rendering Editor.")); return true; }
	for (bool bFunction : {false, true}) for (bool bLive : {false, true})
	{
		AddInfo(FString::Printf(TEXT("Deletion batch: function=%d, LivePreview=%d"), bFunction, bLive));
		FPreviewFixture F(bFunction);
		auto Commands = F.Editor->GetToolkitCommands(); auto Toggle = FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef();
		auto IsLive = [&]() { return Commands->GetCheckState(Toggle) == ECheckBoxState::Checked; };
		if (IsLive() != bLive) Commands->ExecuteAction(Toggle);
		const FString OriginalHash = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(F.Asset);
		const FString DeadCustom = F.Add(TEXT("Custom")), DeadParameter = F.Add(TEXT("ScalarParameter")), Survivor = F.Add(TEXT("Custom"));
		if (DeadCustom.IsEmpty() || DeadParameter.IsEmpty() || Survivor.IsEmpty()) { AddError(TEXT("Could not create deletion fixtures.")); return false; }
		for (const auto& Id : {DeadCustom, Survivor})
		{
			auto P = F.Scope(); P->SetStringField(TEXT("nodeId"), Id); P->SetStringField(TEXT("code"), TEXT("return float3(0.2,0.3,0.4);")); P->SetStringField(TEXT("outputType"), TEXT("Float3")); P->SetArrayField(TEXT("inputs"), {});
			if (!TestTrue(TEXT("Configure deletion fixture Custom"), F.Call(TEXT("content.material.custom.set"), P).bSuccess)) return false;
		}
		if (!TestTrue(TEXT("Wire surviving Custom for actual shader validation"), F.Call(TEXT("content.material.pin.connect"), F.Wire(Survivor, TEXT("Output"), bFunction ? TEXT("expr:Result") : TEXT("root"), bFunction ? TEXT("index:0") : TEXT("Emissive Color"))).bSuccess)) return false;
		const FString Before = F.Hash();
		auto DeleteCustom = Operation(TEXT("deleteCustom"), TEXT("content.material.expression.delete"), Fields({{TEXT("nodeId"), DeadCustom}}));
		auto DeleteParameter = Operation(TEXT("deleteParameter"), TEXT("content.material.expression.delete"), Fields({{TEXT("nodeId"), DeadParameter}}));
		// Exercise native clearing of an actively selected expression preview.
		if (auto GraphEditor = SGraphEditor::FindGraphEditorForGraph(F.Preview->MaterialGraph)) { GraphEditor->ClearSelectionSet(); GraphEditor->SetNodeSelection(F.Find(DeadCustom)->GraphNode, true); }
		Commands->ExecuteAction(FMaterialEditorCommands::Get().StartPreviewNode.ToSharedRef());
		int32 Updates = 0;
		auto ExecuteCounted = [&](const TSharedRef<FJsonObject>& Batch)
		{
			Updates = 0;
			const auto Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == F.Preview) ++Updates; });
			auto Result = F.Call(TEXT("content.material.editor.batch"), Batch);
			FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle); return Result;
		};
		auto Batch = F.Batch({DeleteCustom, DeleteParameter}, true); Batch->SetBoolField(TEXT("requireValidShader"), !bFunction);
		const auto Deleted = ExecuteCounted(Batch);
		if (!TestTrue(TEXT("Consecutive deletion batch completes"), Deleted.bSuccess && Deleted.Data->GetBoolField(TEXT("success")))) { AddError(Deleted.ErrorMessage); return false; }
		TestEqual(TEXT("Two deletes and final validation update base preview once"), Updates, 1);
		TestEqual(TEXT("Consecutive deletes share one native call"), Deleted.Data->GetIntegerField(TEXT("nativeDeletionRefreshes")), 1);
		TestEqual(TEXT("Live Preview restores its original value"), IsLive(), bLive);
		TestNull(TEXT("Custom removed"), F.Find(DeadCustom)); TestNull(TEXT("Parameter removed"), F.Find(DeadParameter));
		TestEqual(TEXT("Both operation receipts retained"), Deleted.Data->GetArrayField(TEXT("operations")).Num(), 2);
		TestEqual(TEXT("Original source untouched"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(F.Asset), OriginalHash);
		TestFalse(TEXT("Original package stays clean"), F.Asset->GetOutermost()->IsDirty());
		const FString After = F.Hash();
		TestTrue(TEXT("Grouped deletion single Undo"), GEditor->UndoTransaction()); TestEqual(TEXT("Grouped undo restores graph"), F.Hash(), Before);
		TestTrue(TEXT("Grouped deletion single Redo"), GEditor->RedoTransaction()); TestEqual(TEXT("Grouped redo restores graph"), F.Hash(), After);
		TestTrue(TEXT("Restore fixture for next ordered case"), GEditor->UndoTransaction());
		auto Change = Operation(TEXT("change"), TEXT("content.material.custom.set"), Fields({{TEXT("nodeId"), Survivor},{TEXT("code"), TEXT("return float3(0.7,0.2,0.1);")}}));
		const auto Mixed = ExecuteCounted(F.Batch({DeleteCustom, Change, DeleteParameter}, true));
		if (!TestTrue(TEXT("Interleaved edits preserve operation order"), Mixed.bSuccess && Mixed.Data->GetBoolField(TEXT("success")))) return false;
		TestEqual(TEXT("Interleaved deletes remain distinct native groups"), Mixed.Data->GetIntegerField(TEXT("nativeDeletionRefreshes")), 2);
		TestEqual(TEXT("Interleaved deletion groups still compile base only once"), Updates, 1);
		TestEqual(TEXT("Later code edit retained"), CastChecked<UMaterialExpressionCustom>(F.Find(Survivor))->Code, FString(TEXT("return float3(0.7,0.2,0.1);")));
		TestTrue(TEXT("Undo mixed batch"), GEditor->UndoTransaction()); TestEqual(TEXT("Mixed undo restores graph"), F.Hash(), Before);
		const auto Duplicate = ExecuteCounted(F.Batch({DeleteCustom, Operation(TEXT("duplicate"), TEXT("content.material.expression.delete"), Fields({{TEXT("nodeId"), DeadCustom}}))}, true));
		TestTrue(TEXT("Duplicate deletion rejects group without edits"), Duplicate.bSuccess && Duplicate.Data->GetBoolField(TEXT("rollbackVerified")));
		TestEqual(TEXT("Invalid group does not refresh"), Updates, 0); TestEqual(TEXT("Invalid group preserves graph"), F.Hash(), Before);
		auto BadMove = Fields({{TEXT("nodeId"), TEXT("expr:Missing")}}); BadMove->SetNumberField(TEXT("posX"), 0); BadMove->SetNumberField(TEXT("posY"), 0);
		const auto Failed = ExecuteCounted(F.Batch({DeleteCustom, DeleteParameter, Operation(TEXT("missing"), TEXT("content.material.expression.move"), BadMove)}, true));
		TestTrue(TEXT("Failure after grouped deletes restores graph"), Failed.bSuccess && Failed.Data->GetBoolField(TEXT("rollbackVerified")));
		TestEqual(TEXT("Failed deletion batch restores original hash"), F.Hash(), Before); TestEqual(TEXT("Failure restores Live Preview"), IsLive(), bLive);
		if (!bFunction)
		{
			auto BadCode = Operation(TEXT("badCode"), TEXT("content.material.custom.set"), Fields({{TEXT("nodeId"), Survivor},{TEXT("code"), TEXT("return UEAI_DELETE_BATCH_UNKNOWN;")}}));
			auto BadBatch = F.Batch({DeleteCustom, DeleteParameter, BadCode}, true); BadBatch->SetBoolField(TEXT("requireValidShader"), true);
			const auto Rejected = ExecuteCounted(BadBatch);
			TestTrue(TEXT("Final compiler sees edits after deletion, and rolls back bad HLSL"), Rejected.bSuccess && Rejected.Data->GetBoolField(TEXT("rollbackVerified")));
			TestEqual(TEXT("Shader failure restores nodes and code"), F.Hash(), Before); TestEqual(TEXT("Shader failure restores Live Preview"), IsLive(), bLive);
		}
		const auto Deferred = ExecuteCounted(F.Batch({DeleteCustom, DeleteParameter}, false));
		TestTrue(TEXT("refresh=false still performs native grouped deletion"), Deferred.bSuccess && Deferred.Data->GetBoolField(TEXT("success")));
		TestEqual(TEXT("refresh=false leaves native Live Preview behavior intact"), Updates, bLive ? 1 : 0);
		TestEqual(TEXT("refresh=false keeps original Live Preview"), IsLive(), bLive);
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialNamedReroutePreviewTest,
	"UE_AI_integration.MaterialEditor.NamedReroutePreview", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialNamedReroutePreviewTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddWarning(TEXT("Named reroute preview requires a rendering Editor.")); return true; }
	for (bool bFunction : {false, true})
	{
		FPreviewFixture F(bFunction);
		const FString OriginalHash = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(F.Asset);
		const FString DeclId = F.Add(TEXT("NamedRerouteDeclaration")), UsageId = F.Add(TEXT("NamedRerouteUsage"));
		auto* Decl = Cast<UMaterialExpressionNamedRerouteDeclaration>(F.Find(DeclId));
		auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(F.Find(UsageId));
		if (!TestTrue(TEXT("Reroute nodes created in working collection"), Decl && Usage)) return false;
		Usage->Declaration = Decl; Usage->DeclarationGuid = Decl->VariableGuid;
		auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("assetPath"), F.Asset->GetPathName()); P->SetStringField(TEXT("targetContext"), TEXT("editorPreview")); P->SetStringField(TEXT("expectedPreviewId"), F.Identity); P->SetBoolField(TEXT("includeNamedReroutes"), true);
		const auto Snapshot = F.Call(TEXT("content.material.graph.index"), P);
		if (!TestTrue(TEXT("Registered query captures preview reference"), Snapshot.bSuccess)) { AddError(Snapshot.ErrorMessage); return false; }
		TestEqual(TEXT("Preview contains one implicit edge"), Snapshot.Data->GetIntegerField(TEXT("namedRerouteEdges")), 1);
		TestEqual(TEXT("Preview reference stays local"), Snapshot.Data->GetIntegerField(TEXT("unresolvedNamedReroutes")), 0);
		TestEqual(TEXT("Original remains unchanged"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(F.Asset), OriginalHash);
		TestFalse(TEXT("No package save or dirty from preview capture"), F.Asset->GetOutermost()->IsDirty());
		auto Release = MakeShared<FJsonObject>(); Release->SetStringField(TEXT("snapshotId"), Snapshot.Data->GetStringField(TEXT("snapshotId"))); F.Call(TEXT("content.material.graph.snapshot.release"), Release);
	}
	return true;
}

#endif
