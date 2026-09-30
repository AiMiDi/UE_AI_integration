#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionComment.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialEditorModule.h"
#include "MaterialEditorActions.h"
#include "IMaterialEditor.h"
#include "Framework/Commands/UICommandList.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UnrealType.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "RHI.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialApplyReviewTest, "UE_AI_integration.MaterialEditor.ApplyReviewAndReadback", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialApplyReviewTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Apply readback acceptance requires a rendering Editor.")); return true; }
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	auto Call = [&](const TCHAR* Id, const TSharedPtr<FJsonObject>& P) { return Registry->FindTool(Id)->Execute(P); };
	for (bool bFunction : {false, true})
	{
		auto* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/ApplyReview_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		UObject* Asset = bFunction ? static_cast<UObject*>(NewObject<UMaterialFunction>(Package, TEXT("Function"), RF_Transactional)) : NewObject<UMaterial>(Package, TEXT("Material"), RF_Transactional);
		FGCObjectScopeGuard Guard(Asset);
		auto* Custom = NewObject<UMaterialExpressionCustom>(Asset, TEXT("Code"), RF_Transactional); Custom->Inputs.Reset(); Custom->Code = TEXT("return float3(0.2,0.3,0.4);"); Custom->OutputType = CMOT_Float3;
		auto* Scalar = NewObject<UMaterialExpressionScalarParameter>(Asset, TEXT("Strength"), RF_Transactional); Scalar->ParameterName = TEXT("Strength"); Scalar->DefaultValue = 1.0f;
		FCustomInput Gain; Gain.InputName = TEXT("Gain"); Gain.Input.Connect(0, Scalar); Custom->Inputs.Add(Gain);
		auto* Comment = NewObject<UMaterialExpressionComment>(Asset, TEXT("Note"), RF_Transactional); Comment->Text = TEXT("Apply review fixture"); Comment->SizeX = 320; Comment->SizeY = 160;
		if (auto* F = Cast<UMaterialFunction>(Asset))
		{
			Custom->Function = F; Scalar->Function = F; F->GetExpressionCollection().AddExpression(Custom); F->GetExpressionCollection().AddExpression(Scalar);
			Comment->Function = F; F->GetExpressionCollection().AddComment(Comment);
			auto* Output = NewObject<UMaterialExpressionFunctionOutput>(F, TEXT("Output"), RF_Transactional); Output->Function = F; Output->OutputName = TEXT("Result"); Output->Id = FGuid::NewGuid(); Output->A.Connect(0, Custom); F->GetExpressionCollection().AddExpression(Output);
		}
		else
		{
			auto* M = CastChecked<UMaterial>(Asset); M->SetShadingModel(MSM_Unlit); Custom->Material = M; Scalar->Material = M;
			M->GetExpressionCollection().AddExpression(Custom); M->GetExpressionCollection().AddExpression(Scalar); M->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Custom);
			Comment->Material = M; M->GetExpressionCollection().AddComment(Comment);
		}
		auto Target = [&]() { auto P = MakeShared<FJsonObject>(); P->SetStringField(bFunction ? TEXT("materialFunction") : TEXT("material"), Asset->GetPathName()); return P; };
		auto& Module = IMaterialEditorModule::Get();
		auto Editor = bFunction ? Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, CastChecked<UMaterialFunction>(Asset)) : Module.CreateMaterialEditor(EToolkitMode::Standalone, nullptr, CastChecked<UMaterial>(Asset));
		ON_SCOPE_EXIT { Editor->CloseWindow(EAssetEditorCloseReason::AssetForceDeleted); };
		auto* Preview = CastChecked<UMaterial>(Editor->GetMaterialInterface());
		const auto Context = Call(TEXT("content.material.editor.context.get"), Target()); if (!TestTrue(TEXT("Read preview context"), Context.bSuccess)) return false;
		auto PrepareParams = Target(); PrepareParams->SetStringField(TEXT("expectedPreviewId"), Context.Data->GetStringField(TEXT("previewId"))); PrepareParams->SetNumberField(TEXT("limit"), 1);
		UMaterialExpressionCustom* PreviewCode = nullptr;
		for (UMaterialExpression* E : Preview->GetExpressions()) if (E && E->GetName() == TEXT("Code")) PreviewCode = Cast<UMaterialExpressionCustom>(E);
		if (!TestNotNull(TEXT("Working copy Custom exists"), PreviewCode)) return false;
		auto Set = Target(); Set->SetStringField(TEXT("targetContext"), TEXT("editorPreview")); Set->SetStringField(TEXT("expectedPreviewId"), Context.Data->GetStringField(TEXT("previewId"))); Set->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(PreviewCode));
		Set->SetStringField(TEXT("code"), TEXT("return float3(0.7,0.8,0.9); // literal path: ") + Preview->GetPathName());
		if (!TestTrue(TEXT("Edit preview code"), Call(TEXT("content.material.custom.set"), Set).bSuccess)) return false;
		PreviewCode->GraphNode->NodePosX += 128; PreviewCode->GraphNode->NodePosY += 64;
		for (UMaterialExpressionComment* Note : Preview->GetEditorComments()) if (Note && Note->GraphNode) Note->GraphNode->NodeComment = TEXT("Updated through graph UI");
		if (!bFunction) { Preview->TwoSided = true; Preview->GetEditorOnlyData()->Roughness.UseConstant = true; Preview->GetEditorOnlyData()->Roughness.Constant = 0.375f; }
		if (!bFunction)
		{
			// Exercise a reflected soft reference while both targets remain unloaded.
			auto* NaniteProperty = FindFProperty<FStructProperty>(Preview->GetClass(), TEXT("NaniteOverrideMaterial"));
			if (!TestNotNull(TEXT("Nanite override property exists"), NaniteProperty)) return false;
			auto* SoftProperty = FindFProperty<FSoftObjectProperty>(NaniteProperty->Struct, TEXT("OverrideMaterialRef"));
			if (!TestNotNull(TEXT("Soft reference fixture property exists"), SoftProperty)) return false;
			void* Value = SoftProperty->ContainerPtrToValuePtr<void>(NaniteProperty->ContainerPtrToValuePtr<void>(Preview));
			const auto Previous = SoftProperty->GetPropertyValue(Value);
			const FString PathA = Package->GetName() + TEXT("_MissingA.Material");
			const FString PathB = Package->GetName() + TEXT("_MissingB.Material");
			SoftProperty->SetPropertyValue(Value, FSoftObjectPtr(FSoftObjectPath(PathA)));
			const auto ReviewA = Call(TEXT("content.material.editor.apply.prepare"), PrepareParams);
			SoftProperty->SetPropertyValue(Value, FSoftObjectPtr(FSoftObjectPath(PathB)));
			const auto ReviewB = Call(TEXT("content.material.editor.apply.prepare"), PrepareParams);
			SoftProperty->SetPropertyValue(Value, Previous);
			TestTrue(TEXT("Different unloaded soft references produce different content receipts"), ReviewA.bSuccess && ReviewB.bSuccess && ReviewA.Data->GetStringField(TEXT("expectedContentHash")) != ReviewB.Data->GetStringField(TEXT("expectedContentHash")));
			TestNull(TEXT("First soft reference was not loaded by review"), FindObject<UObject>(nullptr, *PathA));
			TestNull(TEXT("Second soft reference was not loaded by review"), FindObject<UObject>(nullptr, *PathB));
		}
		Package->SetDirtyFlag(false);
		int32 Updates = 0; const auto Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == Asset || Object == Preview) ++Updates; });
		ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle); };
		const auto Prepared = Call(TEXT("content.material.editor.apply.prepare"), PrepareParams);
		if (!TestTrue(TEXT("Prepare content receipt"), Prepared.bSuccess)) { AddError(Prepared.ErrorMessage); return false; }
		TestEqual(TEXT("Review produces no native updates"), Updates, 0); TestFalse(TEXT("Review does not dirty original package"), Package->IsDirty());
		TestTrue(TEXT("Changed code/settings are reviewed"), Prepared.Data->GetNumberField(TEXT("changeCount")) >= 1);
		if (!bFunction) TestTrue(TEXT("Root constants/settings participate in review"), Prepared.Data->GetNumberField(TEXT("changeCount")) >= 3);
		auto VerifyParams = MakeShared<FJsonObject>(); VerifyParams->SetStringField(TEXT("receiptId"), Prepared.Data->GetStringField(TEXT("receiptId")));
		const auto Before = Call(TEXT("content.material.editor.apply.verify"), VerifyParams);
		if (!TestTrue(TEXT("Verify before Apply readable"), Before.bSuccess)) return false;
		TestFalse(TEXT("Unapplied preview does not match original"), Before.Data->GetBoolField(TEXT("assetMatchesPreparedPreview")));
		TestEqual(TEXT("Readback produces no native updates"), Updates, 0);
		auto RefreshParams = Target(); RefreshParams->SetStringField(TEXT("expectedPreviewId"), Context.Data->GetStringField(TEXT("previewId")));
		const auto Refreshed = Call(TEXT("content.material.editor.refresh"), RefreshParams); if (!TestTrue(TEXT("Refresh valid preview before native Apply"), Refreshed.bSuccess)) return false;
		const auto Apply = FMaterialEditorCommands::Get().Apply.ToSharedRef(); TestTrue(TEXT("Native Apply available"), Editor->GetToolkitCommands()->CanExecuteAction(Apply)); Editor->GetToolkitCommands()->ExecuteAction(Apply);
		const auto Applied = Call(TEXT("content.material.editor.apply.verify"), VerifyParams);
		if (!TestTrue(TEXT("Readback after native Apply"), Applied.bSuccess)) return false;
		TestTrue(TEXT("Applied content matches prepared preview for material/function"), Applied.Data->GetBoolField(TEXT("assetMatchesPreparedPreview")));
		TestFalse(TEXT("Content comparison does not invent execution history"), Applied.Data->GetBoolField(TEXT("nativeApplyExecutionProven")));
		TestFalse(TEXT("Content comparison does not claim disk persistence"), Applied.Data->GetBoolField(TEXT("diskPersistenceVerified")));
		if (!Applied.Data->GetBoolField(TEXT("assetMatchesPreparedPreview")))
		{
			PrepareParams->SetNumberField(TEXT("limit"), 100); const auto Difference = Call(TEXT("content.material.editor.apply.prepare"), PrepareParams);
			if (Difference.bSuccess) { FString Json; auto Writer = TJsonWriterFactory<>::Create(&Json); FJsonSerializer::Serialize(Difference.Data.ToSharedRef(), Writer); AddInfo(Json); }
		}
		UMaterialExpressionCustom* AppliedCode = nullptr;
		const auto OriginalExpressions = bFunction ? CastChecked<UMaterialFunction>(Asset)->GetExpressions() : CastChecked<UMaterial>(Asset)->GetExpressions();
		for (UMaterialExpression* E : OriginalExpressions) if (E && E->GetName() == TEXT("Code")) AppliedCode = Cast<UMaterialExpressionCustom>(E);
		if (!TestNotNull(TEXT("Applied asset has its Custom node"), AppliedCode)) return false;
		const FString AcceptedCode = AppliedCode->Code; AppliedCode->Code = TEXT("return 0.25;");
		const auto Drifted = Call(TEXT("content.material.editor.apply.verify"), VerifyParams);
		TestTrue(TEXT("Subsequent asset changes invalidate content equality"), Drifted.bSuccess && !Drifted.Data->GetBoolField(TEXT("assetMatchesPreparedPreview")));
		AppliedCode->Code = AcceptedCode;
		// Subsequent edits to the working copy do not change what was applied.
		Set->SetStringField(TEXT("code"), TEXT("return float3(0.1,0.1,0.1);")); TestTrue(TEXT("Further preview edit"), Call(TEXT("content.material.custom.set"), Set).bSuccess);
		const auto Later = Call(TEXT("content.material.editor.apply.verify"), VerifyParams);
		TestTrue(TEXT("Original still matches prior accepted content"), Later.bSuccess && Later.Data->GetBoolField(TEXT("assetMatchesPreparedPreview")));
		if (Later.bSuccess) TestFalse(TEXT("New preview edits are reported separately"), Later.Data->GetBoolField(TEXT("previewStillMatchesPreparedContent")));
		Set->SetStringField(TEXT("code"), FString::ChrN(65537, 'x')); PreviewCode->Code = Set->GetStringField(TEXT("code"));
		TestFalse(TEXT("Oversized projection cannot create a partial receipt"), Call(TEXT("content.material.editor.apply.prepare"), PrepareParams).bSuccess);
		PreviewCode->Code = TEXT("return 1;");
		const FString Identity = PrepareParams->GetStringField(TEXT("expectedPreviewId")); PrepareParams->SetStringField(TEXT("expectedPreviewId"), TEXT("stale"));
		TestFalse(TEXT("Review cannot silently switch preview sessions"), Call(TEXT("content.material.editor.apply.prepare"), PrepareParams).bSuccess); PrepareParams->SetStringField(TEXT("expectedPreviewId"), Identity);
		if (!bFunction)
		{
			for (int32 I = 0; I < 65; ++I) if (!TestTrue(TEXT("Bounded receipt cache remains usable"), Call(TEXT("content.material.editor.apply.prepare"), PrepareParams).bSuccess)) return false;
			TestFalse(TEXT("Evicted receipt cannot return a false verdict"), Call(TEXT("content.material.editor.apply.verify"), VerifyParams).bSuccess);
		}
		VerifyParams->SetStringField(TEXT("receiptId"), TEXT("unknown")); TestFalse(TEXT("Unknown receipt rejected"), Call(TEXT("content.material.editor.apply.verify"), VerifyParams).bSuccess);
	}
	return true;
}
#endif
