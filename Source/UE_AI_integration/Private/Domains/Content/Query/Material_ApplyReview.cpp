#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/Sha256.h"
#include "Tools/MCPToolRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "IMaterialEditor.h"
#include "MaterialEditorActions.h"
#include "Framework/Commands/UICommandList.h"
#include "EdGraphNode_Comment.h"
#include "UObject/UnrealType.h"
#include "HAL/PlatformTime.h"

namespace
{
using namespace UEAIIntegration::MaterialEditing;
FString ReviewHash(const FString& Text)
{
	FTCHARToUTF8 Utf8(*Text); FString Hash;
	UEAIIntegration::Infrastructure::TrySha256Hex(Utf8.Get(), Utf8.Length(), Hash); return Hash;
}
FString Part(const FString& Text) { return FString::FromInt(Text.Len()) + TEXT(":") + Text; }

// Typed references are normalized; literal code/text is never path-rewritten.
struct FApplyProjection
{
	TMap<const UObject*, FString> Local;
	TMap<FString, FString> Fields;
	FString Error, Hash;
	int32 Values = 0, TextChars = 0;
	FString Value(FProperty* Property, const void* Data, int32 Depth)
	{
		if (!Error.IsEmpty()) return {};
		if (++Values > 100000 || Depth > 16) { Error = TEXT("property_traversal_budget"); return {}; }
		if (auto* Soft = CastField<FSoftObjectProperty>(Property))
		{
			// Resolving to UObject loses the identity of an unloaded soft reference.
			const FString Path = Soft->GetPropertyValue(Data).ToSoftObjectPath().ToString();
			if (Path.Len() > 65536 || (TextChars += Path.Len()) > 2 * 1024 * 1024) { Error = TEXT("property_text_budget"); return {}; }
			return TEXT("soft:") + Part(Property->GetCPPType()) + Part(Path);
		}
		if (auto* Object = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* Ref = Object->GetObjectPropertyValue(Data);
			if (!Ref) return TEXT("null");
			if (const auto* Id = Local.Find(Ref)) return TEXT("local:") + *Id;
			return TEXT("external:") + Ref->GetPathName();
		}
		TArray<FString> Parts;
		if (auto* Struct = CastField<FStructProperty>(Property))
		{
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				for (int32 I = 0; I < It->ArrayDim; ++I)
					Parts.Add(Part(It->GetName() + TEXT("[") + FString::FromInt(I) + TEXT("]")) + Part(Value(*It, It->ContainerPtrToValuePtr<void>(Data, I), Depth + 1)));
			}
			Parts.Sort(); return TEXT("struct:") + Part(Struct->Struct->GetPathName()) + FString::Join(Parts, TEXT(""));
		}
		if (auto* Array = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(Array, Data);
			if (Helper.Num() > 20000) { Error = TEXT("container_budget"); return {}; }
			for (int32 I = 0; I < Helper.Num() && Error.IsEmpty(); ++I) Parts.Add(Part(Value(Array->Inner, Helper.GetRawPtr(I), Depth + 1)));
			return TEXT("array:") + FString::Join(Parts, TEXT(""));
		}
		if (auto* Map = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(Map, Data);
			if (Helper.GetMaxIndex() > 20000) { Error = TEXT("container_budget"); return {}; }
			for (int32 I = 0; I < Helper.GetMaxIndex() && Error.IsEmpty(); ++I) if (Helper.IsValidIndex(I))
				Parts.Add(Part(Value(Map->KeyProp, Helper.GetKeyPtr(I), Depth + 1)) + Part(Value(Map->ValueProp, Helper.GetValuePtr(I), Depth + 1)));
			Parts.Sort(); return TEXT("map:") + FString::Join(Parts, TEXT(""));
		}
		if (auto* Set = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(Set, Data);
			if (Helper.GetMaxIndex() > 20000) { Error = TEXT("container_budget"); return {}; }
			for (int32 I = 0; I < Helper.GetMaxIndex() && Error.IsEmpty(); ++I) if (Helper.IsValidIndex(I)) Parts.Add(Part(Value(Set->ElementProp, Helper.GetElementPtr(I), Depth + 1)));
			Parts.Sort(); return TEXT("set:") + FString::Join(Parts, TEXT(""));
		}
		if (auto* String = CastField<FStrProperty>(Property); String && String->GetPropertyValue(Data).Len() > 65536) { Error = TEXT("property_text_budget"); return {}; }
		FString Text; Property->ExportTextItem_Direct(Text, Data, nullptr, nullptr, PPF_None);
		if (Text.Len() > 65536 || (TextChars += Text.Len()) > 2 * 1024 * 1024) { Error = TEXT("property_text_budget"); return {}; }
		return TEXT("value:") + Part(Property->GetCPPType()) + Part(Text);
	}
	void Properties(UObject* Object, const FString& Prefix, bool bMaterialInputs = false)
	{
		if (!Object) return;
		for (TFieldIterator<FProperty> It(Object->GetClass()); It && Error.IsEmpty(); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
			auto* Struct = CastField<FStructProperty>(*It);
			const bool bInput = bMaterialInputs && Struct && Struct->Struct->GetName().EndsWith(TEXT("Input"));
			if (!It->HasAnyPropertyFlags(CPF_Edit) && !bInput) continue;
			for (int32 I = 0; I < It->ArrayDim; ++I)
				Fields.Add(Prefix + TEXT("/") + It->GetName() + TEXT("[") + FString::FromInt(I) + TEXT("]"), Value(*It, It->ContainerPtrToValuePtr<void>(Object, I), 0));
		}
	}
	void Capture(const FTarget& Target)
	{
		UObject* Owner = Target.Function ? static_cast<UObject*>(Target.Function) : Target.Material;
		Local.Add(Owner, TEXT("asset"));
		TArray<UMaterialExpression*> Nodes = Target.Expressions;
		const auto Comments = Target.Editor || !Target.Function ? Target.Material->GetEditorComments() : Target.Function->GetEditorComments();
		for (UMaterialExpressionComment* Comment : Comments) if (Comment) Nodes.Add(Comment);
		if (Nodes.Num() > 20000) { Error = TEXT("expression_budget"); return; }
		TSet<FString> Ids;
		for (auto* E : Nodes)
		{
			if (!E || E->IsA<UMaterialExpressionComposite>() || E->IsA<UMaterialExpressionPinBase>()) { Error = TEXT("unsupported_composite_or_missing_expression"); return; }
			const FString Id = MCPMaterialInfrastructure::ExpressionNodeId(E);
			if (Ids.Contains(Id)) { Error = TEXT("duplicate_expression_identity"); return; }
			Ids.Add(Id); Local.Add(E, Id);
		}
		Properties(Owner, TEXT("properties"));
		Properties(Target.Function ? static_cast<UObject*>(Target.Function->GetEditorOnlyData()) : Target.Material->GetEditorOnlyData(), TEXT("editorData"), !Target.Function);
		for (auto* E : Nodes)
		{
			const FString Prefix = TEXT("nodes/") + Local.FindChecked(E);
			Fields.Add(Prefix + TEXT("/class"), E->GetClass()->GetPathName());
			const UEdGraphNode* GraphNode = Target.Editor ? E->GraphNode.Get() : nullptr;
			Fields.Add(Prefix + TEXT("/position"), FString::Printf(TEXT("%d,%d"), GraphNode ? GraphNode->NodePosX : E->MaterialExpressionEditorX, GraphNode ? GraphNode->NodePosY : E->MaterialExpressionEditorY));
			Properties(E, Prefix + TEXT("/properties"));
			if (const auto* Comment = Cast<UMaterialExpressionComment>(E))
			{
				const auto* GraphComment = Cast<UEdGraphNode_Comment>(GraphNode);
				Fields.Add(Prefix + TEXT("/size"), FString::Printf(TEXT("%d,%d"), GraphComment ? GraphComment->NodeWidth : Comment->SizeX, GraphComment ? GraphComment->NodeHeight : Comment->SizeY));
				if (GraphComment) Fields.Add(Prefix + TEXT("/properties/Text[0]"), Value(FindFProperty<FStrProperty>(Comment->GetClass(), TEXT("Text")), &GraphComment->NodeComment, 0));
			}
			if (!Error.IsEmpty()) return;
			int32 I = 0;
			for (const auto* Input : E->GetInputsView())
			{
				if (Input && Input->Expression)
				{
					const auto* Ref = Local.Find(Input->Expression);
					if (!Ref) { Error = TEXT("input_outside_expression_collection"); return; }
					Fields.Add(Prefix + TEXT("/inputs/") + FString::FromInt(I), *Ref + FString::Printf(TEXT(":%d:%d:%d:%d:%d:%d"), Input->OutputIndex, Input->Mask, Input->MaskR, Input->MaskG, Input->MaskB, Input->MaskA));
				}
				++I;
			}
			if (!Error.IsEmpty()) return;
		}
		TArray<FString> Keys; Fields.GetKeys(Keys); Keys.Sort(); FString Canonical;
		for (const auto& Key : Keys)
		{
			Canonical += Part(Key) + Part(Fields.FindChecked(Key));
			if (Canonical.Len() > 4 * 1024 * 1024) { Error = TEXT("projection_text_budget"); return; }
		}
		Hash = ReviewHash(Canonical); if (Hash.IsEmpty()) Error = TEXT("hash_unavailable");
	}
};

struct FApplyReviewReceipt
{
	FString Id, AssetPath, PreviewId, AssetBefore, ExpectedContent;
	bool bFunction = false;
	double Expires = 0;
};
TArray<FApplyReviewReceipt> ApplyReceipts;
void ExpireApplyReceipts() { const double Now = FPlatformTime::Seconds(); ApplyReceipts.RemoveAll([&](const auto& R) { return R.Expires <= Now; }); }
FMCPToolResult ReviewError(const FString& Message, const TCHAR* Code = TEXT("material_apply_review_unavailable")) { return FMCPToolResult::Error(Message, Code, 409); }

class FTool_PrepareMaterialApply : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.apply.prepare"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		auto P = MakeShared<FJsonObject>(); P->Values = Params->Values; P->SetStringField(TEXT("targetContext"), TEXT("editorPreview"));
		FTarget Preview; FString Error; if (!Resolve(P, Preview, Error, true)) return ReviewError(Error);
		FTarget Asset; auto AssetParams = MakeShared<FJsonObject>(); AssetParams->SetStringField(Preview.Function ? TEXT("materialFunction") : TEXT("material"), Preview.OriginalAsset->GetPathName());
		if (!Resolve(AssetParams, Asset, Error)) return ReviewError(Error);
		FApplyProjection Before, Expected; Before.Capture(Asset); Expected.Capture(Preview);
		if (!Before.Error.IsEmpty() || !Expected.Error.IsEmpty()) return ReviewError(TEXT("Cannot establish a complete supported projection: ") + Before.Error + TEXT(" ") + Expected.Error);
		int32 Limit = 50; Params->TryGetNumberField(TEXT("limit"), Limit); if (Limit < 1 || Limit > 100) return ReviewError(TEXT("limit must be 1..100."));
		TSet<FString> KeySet; for (const auto& Pair : Before.Fields) KeySet.Add(Pair.Key); for (const auto& Pair : Expected.Fields) KeySet.Add(Pair.Key);
		TArray<FString> Keys = KeySet.Array(); Keys.Sort();
		int32 Count = 0; TArray<TSharedPtr<FJsonValue>> Changes;
		for (const auto& Key : Keys)
		{
			const auto* A = Before.Fields.Find(Key); const auto* B = Expected.Fields.Find(Key);
			if (A && B && *A == *B) continue;
			++Count; if (Changes.Num() >= Limit) continue;
			auto Change = MakeShared<FJsonObject>(); Change->SetStringField(TEXT("path"), Key);
			Change->SetStringField(TEXT("kind"), !A ? TEXT("added") : !B ? TEXT("removed") : TEXT("changed"));
			Change->SetStringField(TEXT("before"), A ? A->Left(256) : TEXT("")); Change->SetStringField(TEXT("after"), B ? B->Left(256) : TEXT(""));
			Change->SetBoolField(TEXT("valueTruncated"), (A && A->Len() > 256) || (B && B->Len() > 256)); Changes.Add(MakeShared<FJsonValueObject>(Change));
		}
		ExpireApplyReceipts(); if (ApplyReceipts.Num() >= 64) ApplyReceipts.RemoveAt(0);
		FApplyReviewReceipt Receipt{FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens), Asset.OriginalAsset->GetPathName(), PreviewId(Preview), Before.Hash, Expected.Hash, Preview.Function != nullptr, FPlatformTime::Seconds() + 300};
		ApplyReceipts.Add(Receipt);
		auto Result = MakeShared<FJsonObject>(); DescribeTarget(Preview, Result);
		Result->SetStringField(TEXT("receiptId"), Receipt.Id); Result->SetNumberField(TEXT("expiresInSeconds"), 300);
		Result->SetStringField(TEXT("assetContentHash"), Before.Hash); Result->SetStringField(TEXT("expectedContentHash"), Expected.Hash);
		Result->SetStringField(TEXT("coverage"), TEXT("editableProperties; typedReferences; expressionsAndComments; inputConnections; materialRootInputsAndConstants"));
		Result->SetStringField(TEXT("status"), Count ? TEXT("reviewReady") : TEXT("noContentChange"));
		Result->SetBoolField(TEXT("hasUnappliedChanges"), Preview.Editor->GetToolkitCommands()->CanExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef()));
		Result->SetNumberField(TEXT("changeCount"), Count); Result->SetBoolField(TEXT("truncated"), Count > Changes.Num()); Result->SetArrayField(TEXT("changes"), Changes);
		Result->SetBoolField(TEXT("compileTriggered"), false); Result->SetBoolField(TEXT("applyTriggered"), false); Result->SetBoolField(TEXT("saved"), false);
		Result->SetStringField(TEXT("nextAction"), TEXT("Review changes, validate the same preview, use native Apply, then apply.verify with this receipt. This read-only receipt does not authorize or trigger Apply and does not prove shader validity."));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_VerifyMaterialApply : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.editor.apply.verify"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		ExpireApplyReceipts(); FString Id; Params->TryGetStringField(TEXT("receiptId"), Id);
		const auto* Receipt = ApplyReceipts.FindByPredicate([&](const auto& R) { return R.Id == Id; });
		if (!Receipt) return ReviewError(TEXT("Receipt expired, evicted, unknown, or from another Editor process. Prepare a new review."), TEXT("material_apply_receipt_unavailable"));
		// A receipt query must not reload an unloaded asset and trigger PostLoad work.
		FTarget Asset; FString Error; Asset.OriginalAsset = FindObject<UObject>(nullptr, *Receipt->AssetPath);
		Asset.Asset = Asset.OriginalAsset; Asset.Material = Cast<UMaterial>(Asset.Asset); Asset.Function = Cast<UMaterialFunction>(Asset.Asset);
		if ((Receipt->bFunction && !Asset.Function) || (!Receipt->bFunction && !Asset.Material)) return ReviewError(TEXT("Reviewed asset is no longer loaded with the same kind. Open it and prepare a new review."));
		if (Asset.Function) { for (UMaterialExpression* E : Asset.Function->GetExpressions()) Asset.Expressions.Add(E); }
		else { for (UMaterialExpression* E : Asset.Material->GetExpressions()) Asset.Expressions.Add(E); }
		FApplyProjection Actual; Actual.Capture(Asset); if (!Actual.Error.IsEmpty()) return ReviewError(Actual.Error);
		auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("receiptId"), Id); Result->SetStringField(TEXT("assetPath"), Receipt->AssetPath);
		const bool bMatches = Actual.Hash == Receipt->ExpectedContent;
		Result->SetStringField(TEXT("status"), bMatches ? TEXT("contentMatchesPreparedPreview") : TEXT("contentMismatch"));
		Result->SetBoolField(TEXT("assetMatchesPreparedPreview"), bMatches); Result->SetBoolField(TEXT("assetChangedSincePrepare"), Actual.Hash != Receipt->AssetBefore);
		Result->SetStringField(TEXT("actualContentHash"), Actual.Hash); Result->SetStringField(TEXT("expectedContentHash"), Receipt->ExpectedContent);
		Result->SetBoolField(TEXT("packageDirty"), Asset.OriginalAsset->GetOutermost()->IsDirty());
		FTarget Preview; Preview.OriginalAsset = Asset.OriginalAsset;
		const bool bEditorOpen = ResolvePreview(Preview, Error); Result->SetBoolField(TEXT("editorOpen"), bEditorOpen);
		if (bEditorOpen)
		{
			FApplyProjection Current; Current.Capture(Preview);
			Result->SetBoolField(TEXT("samePreviewSession"), PreviewId(Preview) == Receipt->PreviewId);
			Result->SetBoolField(TEXT("previewStillMatchesPreparedContent"), Current.Error.IsEmpty() && Current.Hash == Receipt->ExpectedContent);
			Result->SetBoolField(TEXT("hasUnappliedChanges"), Preview.Editor->GetToolkitCommands()->CanExecuteAction(FMaterialEditorCommands::Get().Apply.ToSharedRef()));
		}
		Result->SetBoolField(TEXT("nativeApplyExecutionProven"), false); Result->SetBoolField(TEXT("diskPersistenceVerified"), false);
		Result->SetBoolField(TEXT("compileTriggered"), false); Result->SetBoolField(TEXT("applyTriggered"), false); Result->SetBoolField(TEXT("saved"), false);
		return FMCPToolResult::Ok(Result);
	}
};
}
namespace UEAIIntegrationTools
{
void RegisterMaterialApplyReviewTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_PrepareMaterialApply>());
	Registry.Register(MakeShared<FTool_VerifyMaterialApply>());
}
}
