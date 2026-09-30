#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "IMaterialEditor.h"
#include "Toolkits/ToolkitManager.h"
#include "MaterialEditingLibrary.h"

namespace UEAIIntegration::MaterialEditing
{
using namespace MCPMaterialInfrastructure;
UMaterialGraph* FTarget::Graph() const { return Material ? Material->MaterialGraph.Get() : nullptr; }
UMaterialExpression* FTarget::Find(const FString& Id) const
{
    for (auto* E : Expressions)
        if (E && (ExpressionNodeId(E) == Id || MatchesMaterialNode(E->GraphNode, Id))) return E;
    return nullptr;
}

FString PreviewId(const FTarget& Target)
{
	struct FIdentity { TWeakObjectPtr<UMaterial> Preview; FGuid Id; };
	static TArray<FIdentity> Identities;
	Identities.RemoveAll([](const FIdentity& Item) { return !Item.Preview.IsValid(); });
	if (auto* Existing = Identities.FindByPredicate([&](const FIdentity& Item) { return Item.Preview == Target.Material; })) return Existing->Id.ToString(EGuidFormats::DigitsWithHyphens);
	if (Identities.Num() >= 64) Identities.RemoveAt(0);
	const FGuid Id = FGuid::NewGuid(); Identities.Add({Target.Material, Id});
	return Id.ToString(EGuidFormats::DigitsWithHyphens);
}

bool ResolvePreview(FTarget& Out, FString& Error)
{
	auto Toolkit = FToolkitManager::Get().FindEditorForAsset(Out.OriginalAsset);
	if (!Toolkit || Toolkit->GetToolkitFName() != TEXT("MaterialEditor"))
	{
		Error = TEXT("The scoped asset has no open Material Editor. Open it explicitly before editing its preview."); return false;
	}
	Out.Editor = StaticCastSharedPtr<IMaterialEditor>(Toolkit);
	Out.Material = Cast<UMaterial>(Out.Editor->GetMaterialInterface());
	if (!Out.Material || !Out.Material->MaterialGraph || Out.Material == Out.OriginalAsset)
	{
		Error = TEXT("The Material Editor has no independent preview graph."); return false;
	}
	Out.Function = Out.Material->MaterialGraph->MaterialFunction;
	if (Out.OriginalAsset->IsA<UMaterialFunction>() != (Out.Function != nullptr))
	{
		Error = TEXT("The editor preview does not match the scoped asset kind."); return false;
	}
	Out.Asset = Out.Function ? static_cast<UObject*>(Out.Function) : Out.Material;
	Out.Expressions.Reset();
	// Native function editors keep the authoritative working collection on the preview material.
	for (UMaterialExpression* Expression : Out.Material->GetExpressions()) Out.Expressions.Add(Expression);
	return true;
}

bool Resolve(const TSharedPtr<FJsonObject>& Params, FTarget& Out, FString& Error, bool bMutating)
{
	FString Material, Function;
	Params->TryGetStringField(TEXT("material"), Material);
	Params->TryGetStringField(TEXT("materialFunction"), Function);
	if (Material.IsEmpty() == Function.IsEmpty()) { Error = TEXT("Specify exactly one material or materialFunction target."); return false; }
	if (!Material.IsEmpty())
	{
		Out.Material = LoadMaterialByName(Material, Error); Out.Asset = Out.Material;
		if (Out.Material) for (UMaterialExpression* E : Out.Material->GetExpressions()) Out.Expressions.Add(E);
	}
	else
	{
		Out.Function = LoadMaterialFunctionByName(Function, Error); Out.Asset = Out.Function;
		if (Out.Function) for (UMaterialExpression* E : Out.Function->GetExpressions()) Out.Expressions.Add(E);
	}
	if (!Out.Asset) return false;
	Out.OriginalAsset = Out.Asset;
	FString Context = TEXT("asset"); Params->TryGetStringField(TEXT("targetContext"), Context);
	if (Context != TEXT("asset") && Context != TEXT("editorPreview")) { Error = TEXT("Unknown targetContext."); return false; }
	if (Out.Asset->GetOutermost() == GetTransientPackage()
		&& (Context == TEXT("editorPreview") || (bMutating && !Workflow::GetExecutionContext(Params))))
	{
		Error = TEXT("Scope a persistent asset, not a transient preview object path. Transient source tests require trusted execution metadata."); return false;
	}
	if (Context == TEXT("editorPreview"))
	{
		if (Workflow::GetExecutionContext(Params)) { Error = TEXT("Asset Workflow checkpoints do not cover editor previews. Use interactive preview edits or targetContext=asset."); return false; }
		if (!ResolvePreview(Out, Error)) return false;
		FString Expected;
		if ((bMutating || Params->HasField(TEXT("expectedPreviewId")))
			&& (!Params->TryGetStringField(TEXT("expectedPreviewId"), Expected) || Expected != PreviewId(Out)))
		{
			Error = TEXT("Preview identity changed or is missing. Read editor.context.get before editing the open preview."); return false;
		}
	}
	else if (Params->HasField(TEXT("expectedPreviewId"))) { Error = TEXT("expectedPreviewId requires targetContext=editorPreview."); return false; }
	return true;
}

void BeginEdit(FTarget& Target)
{
	if (Target.Editor)
	{
		Target.Transaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("UEAI", "EditMaterialPreview", "Edit material preview with UE AI"));
		// Function editors create this material before setting RF_Transactional on
		// its owner. The expression collection lives in a separate editor-only object.
		Target.Material->GetEditorOnlyData()->SetFlags(RF_Transactional);
		Target.Material->Modify();
		if (Target.Function)
		{
			Target.Function->GetEditorOnlyData()->SetFlags(RF_Transactional);
			Target.Function->GetEditorOnlyData()->Modify();
		}
	}
	Target.Asset->Modify();
}

void DescribeTarget(const FTarget& Target, const TSharedRef<FJsonObject>& Result)
{
	Result->SetStringField(TEXT("targetContext"), Target.Editor ? TEXT("editorPreview") : TEXT("asset"));
	Result->SetStringField(TEXT("assetPath"), Target.OriginalAsset->GetPathName());
	if (Target.Editor) Result->SetStringField(TEXT("previewId"), PreviewId(Target));
}

void FinishEdit(const FTarget& Target, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Result)
{
	DescribeTarget(Target, Result);
	if (Target.Editor)
	{
		Target.Editor->MarkMaterialDirty();
		Result->SetBoolField(TEXT("compileDeferred"), true);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("applied"), false);
		return;
	}
	const bool bDeferred = Workflow::ShouldDeferCompile(Params);
	NotifyMaterialSourceEdited(Target.Asset);
	Target.Asset->MarkPackageDirty();
	if (!bDeferred)
	{
		Target.Asset->PostEditChange();
		if (Target.Function) UMaterialEditingLibrary::UpdateMaterialFunction(Target.Function, nullptr);
		if (Expression->GraphNode) Expression->GraphNode->GetGraph()->NotifyGraphChanged();
	}
	Result->SetBoolField(TEXT("compileDeferred"), bDeferred);
	Result->SetBoolField(TEXT("saved"), Workflow::ShouldSaveImmediately(Params) && SaveMaterialPackage(Target.Asset));
}

}
