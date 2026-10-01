#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Workflow/UEWorkflowRuntime.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialEditorModule.h"
#include "MaterialEditorActions.h"
#include "IMaterialEditor.h"
#include "Framework/Commands/UICommandList.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "UObject/GCObjectScopeGuard.h"
#include "RHI.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialEditorPreviewTest, "UE_AI_integration.MaterialEditor.PreviewEditing", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FMaterialEditorPreviewTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Native Material Editor requires a rendering session; preview integration not exercised under NullRHI.")); return true; }
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	if (!Registry) return false;
	for (bool bFunction : {false, true})
	{
		auto* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/EditorPreview_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UObject* Asset = bFunction ? static_cast<UObject*>(NewObject<UMaterialFunction>(Package, TEXT("Function"), RF_Transactional))
			: NewObject<UMaterial>(Package, TEXT("Material"), RF_Transactional);
		// Native Apply replaces the original in place; TStrongObjectPtr's refcount
		// forbids that replacement. Use GC references like the native toolkit does.
		FGCObjectScopeGuard AssetGuard(Asset);
		auto* Custom = NewObject<UMaterialExpressionCustom>(Asset, TEXT("Custom"), RF_Transactional);
		Custom->Code = TEXT("return float3(0.2,0.4,0.6);"); Custom->Inputs.Reset(); Custom->OutputType = CMOT_Float3;
		auto* Scalar = NewObject<UMaterialExpressionScalarParameter>(Asset, TEXT("Strength"), RF_Transactional);
		Scalar->ParameterName = TEXT("Strength"); Scalar->DefaultValue = 1.0f;
		if (auto* Function = Cast<UMaterialFunction>(Asset))
		{
			auto* Output = NewObject<UMaterialExpressionFunctionOutput>(Function, NAME_None, RF_Transactional);
			Output->OutputName = TEXT("Result"); Output->Id = FGuid::NewGuid(); Output->A.Connect(0, Custom); Output->Function = Function;
			Function->GetExpressionCollection().AddExpression(Custom); Function->GetExpressionCollection().AddExpression(Scalar); Function->GetExpressionCollection().AddExpression(Output);
			Custom->Function = Function; Scalar->Function = Function;
			auto* FunctionCall = NewObject<UMaterialExpressionMaterialFunctionCall>(Function, TEXT("NestedCall"), RF_Transactional);
			FunctionCall->Function = Function; Function->GetExpressionCollection().AddExpression(FunctionCall);
		}
		else
		{
			auto* Material = CastChecked<UMaterial>(Asset); Material->SetShadingModel(MSM_Unlit);
			Material->GetExpressionCollection().AddExpression(Custom); Material->GetExpressionCollection().AddExpression(Scalar);
			Custom->Material = Material; Scalar->Material = Material; Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Custom);
		}
		auto Target = [&]() { auto P = MakeShared<FJsonObject>(); P->SetStringField(bFunction ? TEXT("materialFunction") : TEXT("material"), Asset->GetPathName()); return P; };
		auto Call = [&](const TCHAR* Id, const TSharedPtr<FJsonObject>& P) { return Registry->FindTool(Id)->Execute(P); };
		auto Closed = Call(TEXT("content.material.editor.context.get"), Target());
		if (!TestTrue(TEXT("Closed context is readable"), Closed.bSuccess)) return false;
		TestFalse(TEXT("Context query does not open an editor"), Closed.Data->GetBoolField(TEXT("editorOpen")));
		auto& Module = IMaterialEditorModule::Get();
		TSharedRef<IMaterialEditor> Editor = bFunction ? Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, CastChecked<UMaterialFunction>(Asset))
			: Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, CastChecked<UMaterial>(Asset));
		ON_SCOPE_EXIT { Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted); };
		auto* Preview = CastChecked<UMaterial>(Editor->GetMaterialInterface());
		auto Context = Call(TEXT("content.material.editor.context.get"), Target());
		if (!TestTrue(TEXT("Opened context is readable"), Context.bSuccess && Context.Data->GetBoolField(TEXT("editorOpen")))) return false;
		const FString PreviewIdentity = Context.Data->GetStringField(TEXT("previewId"));
		auto PreviewParams = [&](UMaterialExpression* Node) { auto P = Target(); P->SetStringField(TEXT("targetContext"), TEXT("editorPreview")); P->SetStringField(TEXT("expectedPreviewId"), PreviewIdentity); if (Node) P->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Node)); return P; };
		auto* PreviewCustom = FindObject<UMaterialExpressionCustom>(bFunction ? static_cast<UObject*>(Preview->MaterialGraph->MaterialFunction) : Preview, TEXT("Custom"));
		// Function previews can reparent expressions; select by preserved identity from the actual collection.
		for (UMaterialExpression* E : Preview->GetExpressions()) if (E && E->GetName() == TEXT("Custom")) PreviewCustom = Cast<UMaterialExpressionCustom>(E);
		if (!TestNotNull(TEXT("Custom belongs to the working copy"), PreviewCustom) || !TestTrue(TEXT("Working copy is separate"), PreviewCustom != Custom)) return false;
		Package->SetDirtyFlag(false);
		const FString OriginalHash = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Asset);
		auto P = PreviewParams(PreviewCustom); P->SetStringField(TEXT("code"), TEXT("return float3(0.3,0.5,0.7);"));
		P->SetStringField(TEXT("expectedPreviewId"), TEXT("preview:stale"));
		TestFalse(TEXT("Reopened/stale preview identity fails before writing"), Call(TEXT("content.material.custom.set"), P).bSuccess);
		P->SetStringField(TEXT("expectedPreviewId"), PreviewIdentity);
		auto Workflow = MakeShared<FJsonObject>(); Workflow->SetBoolField(TEXT("deferCompile"), true); P->SetObjectField(TEXT("__ueWorkflow"), Workflow);
		TestFalse(TEXT("Asset Workflow cannot escape its checkpoint into a preview"), Call(TEXT("content.material.custom.set"), P).bSuccess); P->RemoveField(TEXT("__ueWorkflow"));
		int32 PreviewUpdates = 0, OriginalUpdates = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == Preview) ++PreviewUpdates; if (Object == Asset) ++OriginalUpdates; });
		ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle); };
		for (int32 I = 0; I < 4; ++I)
		{
			P->SetStringField(TEXT("code"), FString::Printf(TEXT("return float3(0.3,0.5,%f);"), 0.6 + 0.01 * I));
			auto Edited = Call(TEXT("content.material.custom.set"), P);
			if (!TestTrue(TEXT("Preview Custom edit succeeds"), Edited.bSuccess)) { AddError(Edited.ErrorMessage); return false; }
			TestFalse(TEXT("Preview edit does not save"), Edited.Data->GetBoolField(TEXT("saved")));
		}
		for (UMaterialExpression* E : Preview->GetExpressions()) if (E && E->GetName() == TEXT("Strength"))
		{
			auto Parameter = PreviewParams(E); Parameter->SetNumberField(TEXT("defaultValue"), 2.5);
			TestTrue(TEXT("Parameter edit targets the working copy"), Call(TEXT("content.material.parameter.set"), Parameter).bSuccess);
		}
		TestEqual(TEXT("Continuous edits do not compile the preview"), PreviewUpdates, 0);
		TestEqual(TEXT("Continuous edits do not update the original asset"), OriginalUpdates, 0);
		TestEqual(TEXT("Authored source is unchanged"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Asset), OriginalHash);
		TestFalse(TEXT("Authored package stays clean before Apply"), Package->IsDirty());
		GEditor->UndoTransaction();
		for (UMaterialExpression* E : Preview->GetExpressions()) if (auto* Value = Cast<UMaterialExpressionScalarParameter>(E)) TestEqual(TEXT("Native undo restores the preview parameter"), Value->DefaultValue, 1.0f);
		GEditor->RedoTransaction();
		for (UMaterialExpression* E : Preview->GetExpressions()) if (auto* Value = Cast<UMaterialExpressionScalarParameter>(E)) TestEqual(TEXT("Native redo restores the tool edit"), Value->DefaultValue, 2.5f);
		if (bFunction) for (UMaterialExpression* E : Preview->GetExpressions()) if (E && E->GetName() == TEXT("NestedCall"))
		{
			auto Recursive = PreviewParams(E); Recursive->SetStringField(TEXT("function"), Asset->GetPathName());
			TestFalse(TEXT("Function preview cannot create a cycle on Apply"), Call(TEXT("content.material.function.call.set"), Recursive).bSuccess);
		}
		auto Refresh = Target(); Refresh->SetStringField(TEXT("expectedPreviewId"), PreviewIdentity);
		// Exercise both native Live Preview enabled and disabled, preserving the toggle.
		const auto Toggle = FMaterialEditorCommands::Get().ToggleLivePreview.ToSharedRef();
		if (bFunction && Editor->GetToolkitCommands()->GetCheckState(Toggle) == ECheckBoxState::Checked) Editor->GetToolkitCommands()->ExecuteAction(Toggle);
		const auto ToggleBefore = Editor->GetToolkitCommands()->GetCheckState(Toggle);
		PreviewUpdates = 0;
		auto Refreshed = Call(TEXT("content.material.editor.refresh"), Refresh);
		if (!TestTrue(TEXT("One explicit native refresh succeeds"), Refreshed.bSuccess)) { AddError(Refreshed.ErrorMessage); return false; }
		TestEqual(TEXT("Base preview compiled once"), PreviewUpdates, 1);
		TestEqual(TEXT("Refresh preserves Live Preview setting"), Editor->GetToolkitCommands()->GetCheckState(Toggle), ToggleBefore);
		TestEqual(TEXT("Refresh does not apply"), UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Asset), OriginalHash);
		TestFalse(TEXT("Refresh does not save"), Refreshed.Data->GetBoolField(TEXT("saved")));
		if (bFunction) TestTrue(TEXT("Function base preview cannot prove all outputs"), Refreshed.Data->HasTypedField<EJson::Null>(TEXT("valid")));
		else
		{
			TestTrue(TEXT("Material preview HLSL compiles"), Refreshed.Data->GetBoolField(TEXT("valid")));
			P->SetStringField(TEXT("code"), TEXT("return UEAI_PREVIEW_UNKNOWN_IDENTIFIER;")); TestTrue(TEXT("Broken code can be staged"), Call(TEXT("content.material.custom.set"), P).bSuccess);
			auto Failed = Call(TEXT("content.material.editor.refresh"), Refresh); TestFalse(TEXT("Compiler identifies broken preview HLSL"), Failed.Data->GetBoolField(TEXT("valid")));
			P->SetStringField(TEXT("code"), TEXT("return float3(0.1,0.2,0.3);")); TestTrue(TEXT("Correction can be staged"), Call(TEXT("content.material.custom.set"), P).bSuccess);
			auto Stale = Call(TEXT("content.material.diagnostics.get"), PreviewParams(nullptr)); TestEqual(TEXT("Old shader verdict becomes stale immediately"), Stale.Data->GetStringField(TEXT("compileState")), FString(TEXT("stale")));
			auto Fixed = Call(TEXT("content.material.editor.refresh"), Refresh); TestTrue(TEXT("Corrected preview compiles"), Fixed.Data->GetBoolField(TEXT("valid")));
			const auto Apply = FMaterialEditorCommands::Get().Apply.ToSharedRef();
			TestTrue(TEXT("Native Apply becomes available"), Editor->GetToolkitCommands()->CanExecuteAction(Apply));
			Editor->GetToolkitCommands()->ExecuteAction(Apply);
			TestFalse(TEXT("Native Apply accepts the tool edits"), Editor->GetToolkitCommands()->CanExecuteAction(Apply));
			auto Applied = Call(TEXT("content.material.custom.get"), [&]() { auto R = Target(); R->SetStringField(TEXT("nodeId"), TEXT("expr:Custom")); return R; }());
			TestEqual(TEXT("Native Apply transfers corrected code to the original"), Applied.Data->GetStringField(TEXT("code")), FString(TEXT("return float3(0.1,0.2,0.3);")));
		}
	}
	return true;
}

#endif
