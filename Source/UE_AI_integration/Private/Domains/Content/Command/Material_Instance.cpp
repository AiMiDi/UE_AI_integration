#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/Sha256.h"
#include "Tools/MCPToolRegistry.h"
#include "Materials/MaterialLayersFunctions.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Engine/Texture.h"
#include "EditorSupportDelegates.h"
#include "Editor.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GCObjectScopeGuard.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstance.h"

namespace
{
constexpr int32 MaxParameters = 4096;
const EMaterialParameterType Types[] = { EMaterialParameterType::Scalar, EMaterialParameterType::Vector, EMaterialParameterType::Texture, EMaterialParameterType::StaticSwitch };
FString TypeName(EMaterialParameterType Type)
{
	switch (Type) { case EMaterialParameterType::Scalar: return TEXT("scalar"); case EMaterialParameterType::Vector: return TEXT("vector"); case EMaterialParameterType::Texture: return TEXT("texture"); default: return TEXT("switch"); }
}
FString AssociationName(EMaterialParameterAssociation A) { return A == GlobalParameter ? TEXT("global") : A == LayerParameter ? TEXT("layer") : TEXT("blend"); }
FString Key(EMaterialParameterType Type, const FMaterialParameterInfo& Info)
{
	return TypeName(Type) + FString::Printf(TEXT("/%d/%d/"), int32(Info.Association), Info.Index) + Info.Name.ToString().ToLower();
}
TSharedPtr<FJsonValue> JsonValue(const FMaterialParameterValue& V)
{
	switch (V.Type)
	{
	case EMaterialParameterType::Scalar: return MakeShared<FJsonValueNumber>(V.AsScalar());
	case EMaterialParameterType::StaticSwitch: return MakeShared<FJsonValueBoolean>(V.AsStaticSwitch());
	case EMaterialParameterType::Texture: return V.Texture ? MakeShared<FJsonValueString>(V.Texture->GetPathName()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	case EMaterialParameterType::Vector:
	{
		const auto C = V.AsLinearColor(); auto O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("r"), C.R); O->SetNumberField(TEXT("g"), C.G); O->SetNumberField(TEXT("b"), C.B); O->SetNumberField(TEXT("a"), C.A);
		return MakeShared<FJsonValueObject>(O);
	}
	default: return MakeShared<FJsonValueNull>();
	}
}
struct FEntry
{
	FMaterialParameterInfo Info;
	EMaterialParameterType Type = EMaterialParameterType::None;
	FMaterialParameterMetadata Effective, Inherited;
	FMaterialParameterValue Local;
	FGuid LocalGuid, DeclaredGuid;
	bool bDeclared = false, bInherited = false, bOverride = false;
};
struct FState
{
	TMap<FString, FEntry> Entries;
	TArray<TSharedPtr<FJsonValue>> Rows;
	FString Hash, Error;
};
FMCPToolResult Invalid(const FString& Message, const TCHAR* Code = TEXT("material_instance_invalid_request"), int32 Status = 400)
{
	return FMCPToolResult::Error(Message, Code, Status);
}
FState ReadState(UMaterialInstanceConstant* MI)
{
	FState S; TSet<const UMaterialInterface*> Seen; TArray<TSharedPtr<FJsonValue>> Chain;
	for (UMaterialInterface* Asset = MI; Asset; )
	{
		if (Seen.Contains(Asset) || Seen.Num() >= 64) { S.Error = TEXT("Parent chain is cyclic or exceeds 64 assets."); return S; }
		Seen.Add(Asset); auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("path"), Asset->GetPathName());
		if (const auto* Constant = Cast<UMaterialInstanceConstant>(Asset)) O->SetStringField(TEXT("revision"), Constant->ParameterStateId.ToString());
		else if (const auto* Material = Cast<UMaterial>(Asset)) O->SetStringField(TEXT("revision"), Material->StateId.ToString());
		Chain.Add(MakeShared<FJsonValueObject>(O));
		auto* Instance = Cast<UMaterialInstance>(Asset); Asset = Instance ? Instance->Parent.Get() : nullptr;
	}
	if (!MI->Parent) { S.Error = TEXT("The instance has no parent material."); return S; }
	for (auto Type : Types)
	{
		TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters; MI->GetAllParametersOfType(Type, Parameters);
		TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids; MI->GetAllParameterInfoOfType(Type, Infos, Guids);
		TMap<FMaterialParameterInfo, FGuid> Declarations; for (int32 I = 0; I < Infos.Num() && I < Guids.Num(); ++I) Declarations.Add(Infos[I], Guids[I]);
		if (S.Entries.Num() + Parameters.Num() > MaxParameters) { S.Error = TEXT("Parameter catalog exceeds 4096 entries."); return S; }
		for (const auto& Pair : Parameters)
		{
			FEntry E; E.Info = Pair.Key; E.Type = Type; E.Effective = Pair.Value;
			if (const auto* Guid = Declarations.Find(Pair.Key)) { E.bDeclared = true; E.DeclaredGuid = *Guid; }
			// Skip only this instance's overrides; UE remaps layer indices while
			// walking parents and also retains defaults of locally overridden layers.
			E.bInherited = MI->GetParameterValue(Type, Pair.Key, E.Inherited, EMaterialGetParameterValueFlags::CheckNonOverrides);
			S.Entries.Add(Key(Type, Pair.Key), MoveTemp(E));
		}
	}
	auto Local = [&](EMaterialParameterType Type, const FMaterialParameterInfo& Info, const FMaterialParameterValue& Value, const FGuid& Guid)
	{
		auto& E = S.Entries.FindOrAdd(Key(Type, Info)); E.Info = Info; E.Type = Type;
		if (E.bOverride) { S.Error = TEXT("Duplicate local parameter identity."); return; }
		E.bOverride = true; E.Local = Value; E.LocalGuid = Guid;
	};
	for (const auto& P : MI->ScalarParameterValues) Local(EMaterialParameterType::Scalar, P.ParameterInfo, FMaterialParameterValue(P.ParameterValue), P.ExpressionGUID);
	for (const auto& P : MI->VectorParameterValues) Local(EMaterialParameterType::Vector, P.ParameterInfo, FMaterialParameterValue(P.ParameterValue), P.ExpressionGUID);
	for (const auto& P : MI->TextureParameterValues) Local(EMaterialParameterType::Texture, P.ParameterInfo, FMaterialParameterValue(P.ParameterValue), P.ExpressionGUID);
	const FStaticParameterSet LocalStatic = MI->GetStaticParameters();
	for (const auto& P : LocalStatic.StaticSwitchParameters) if (P.bOverride) Local(EMaterialParameterType::StaticSwitch, P.ParameterInfo, FMaterialParameterValue(P.Value), P.ExpressionGUID);
	if (!S.Error.IsEmpty()) return S;
	if (S.Entries.Num() > MaxParameters) { S.Error = TEXT("Parameter catalog including orphan overrides exceeds 4096 entries."); return S; }
	TArray<FString> Keys; S.Entries.GetKeys(Keys); Keys.Sort();
	for (const auto& K : Keys)
	{
		const auto& E = S.Entries.FindChecked(K); auto O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("name"), E.Info.Name.ToString()); O->SetStringField(TEXT("type"), TypeName(E.Type));
		O->SetStringField(TEXT("association"), AssociationName(E.Info.Association)); O->SetNumberField(TEXT("index"), E.Info.Index);
		O->SetBoolField(TEXT("declared"), E.bDeclared); O->SetBoolField(TEXT("overridden"), E.bOverride);
		O->SetStringField(TEXT("expressionGuid"), E.DeclaredGuid.ToString()); O->SetStringField(TEXT("overrideGuid"), E.LocalGuid.ToString());
		O->SetField(TEXT("effectiveValue"), E.bDeclared ? JsonValue(E.Effective.Value) : MakeShared<FJsonValueNull>());
		O->SetField(TEXT("inheritedValue"), E.bInherited ? JsonValue(E.Inherited.Value) : MakeShared<FJsonValueNull>());
		O->SetField(TEXT("localValue"), E.bOverride ? JsonValue(E.Local) : MakeShared<FJsonValueNull>());
		S.Rows.Add(MakeShared<FJsonValueObject>(O));
	}
	FMaterialLayersFunctions Layers; TArray<TSharedPtr<FJsonValue>> LayerRows;
	if (MI->GetMaterialLayers(Layers))
	{
		if (Layers.Layers.Num() > 256 || Layers.Blends.Num() > 256) { S.Error = TEXT("Layer layout exceeds 256 layers/blends."); return S; }
		for (int32 I = 0; I < Layers.Layers.Num(); ++I)
		{
			auto L = MakeShared<FJsonObject>(); L->SetStringField(TEXT("layer"), GetPathNameSafe(Layers.Layers[I]));
			L->SetStringField(TEXT("guid"), Layers.EditorOnly.LayerGuids.IsValidIndex(I) ? Layers.EditorOnly.LayerGuids[I].ToString() : TEXT("missing"));
			L->SetBoolField(TEXT("enabled"), Layers.EditorOnly.LayerStates.IsValidIndex(I) && Layers.EditorOnly.LayerStates[I]);
			LayerRows.Add(MakeShared<FJsonValueObject>(L));
		}
		for (UMaterialFunctionInterface* Blend : Layers.Blends) LayerRows.Add(MakeShared<FJsonValueString>(GetPathNameSafe(Blend)));
	}
	auto Full = MakeShared<FJsonObject>(); Full->SetArrayField(TEXT("parents"), Chain); Full->SetArrayField(TEXT("layers"), LayerRows); Full->SetArrayField(TEXT("parameters"), S.Rows);
	FString Canonical; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Canonical); FJsonSerializer::Serialize(Full, Writer);
	if (Canonical.Len() > 2 * 1024 * 1024) { S.Error = TEXT("Parameter state exceeds 2 Mi characters."); return S; }
	FTCHARToUTF8 Utf8(*Canonical); if (!UEAIIntegration::Infrastructure::TrySha256Hex(Utf8.Get(), Utf8.Length(), S.Hash)) S.Error = TEXT("Hash unavailable.");
	return S;
}
UMaterialInstanceConstant* Resolve(const TSharedPtr<FJsonObject>& P, FString& Error)
{
	FString Path; P->TryGetStringField(TEXT("instance"), Path);
	if (!Path.StartsWith(TEXT("/"))) { Error = TEXT("instance must be an exact asset path."); return nullptr; }
	return MCPMaterialInfrastructure::LoadMaterialInstanceByName(Path, Error);
}
struct FOverrides
{
	TArray<FScalarParameterValue> Scalars;
	TArray<FVectorParameterValue> Vectors;
	TArray<FTextureParameterValue> Textures;
	FStaticParameterSet Static;
	explicit FOverrides(UMaterialInstanceConstant* MI) : Scalars(MI->ScalarParameterValues), Vectors(MI->VectorParameterValues), Textures(MI->TextureParameterValues), Static(MI->GetStaticParameters()) {}
	void Assign(UMaterialInstanceConstant* MI, bool bStaticChanged) const
	{
		MI->ScalarParameterValues = Scalars; MI->VectorParameterValues = Vectors; MI->TextureParameterValues = Textures;
		if (bStaticChanged) MI->UpdateStaticPermutation(Static);
	}
};
template<class T, class V> void SetOverride(TArray<T>& Array, const FMaterialParameterInfo& Info, const V& Value, FGuid Guid, bool bClear)
{
	if (bClear) { Array.RemoveAll([&](const auto& P) { return P.ParameterInfo == Info; }); return; }
	auto* P = Array.FindByPredicate([&](const auto& P) { return P.ParameterInfo == Info; });
	if (!P) { P = &Array.AddDefaulted_GetRef(); P->ParameterInfo = Info; }
	P->ParameterValue = Value; P->ExpressionGUID = Guid;
}
struct FChange { FString Id; bool bClear; FMaterialParameterValue Value; FGuid Guid; };
FMCPToolResult CompleteResponse(UMaterialInstanceConstant* MI, const TSharedRef<FJsonObject>& Result, bool bSave)
{
	if (!bSave) return FMCPToolResult::Ok(Result);
	const bool bSaved = MCPMaterialInfrastructure::SaveMaterialPackage(MI);
	Result->SetBoolField(TEXT("saved"), bSaved); Result->SetStringField(TEXT("status"), bSaved ? TEXT("saved") : TEXT("appliedSaveFailed"));
	if (!bSaved) { auto Failure = Invalid(TEXT("Overrides applied but package save failed; in-memory edits retained."), TEXT("material_instance_save_failed"), 500); Failure.Data = Result; return Failure; }
	const auto SavedState = ReadState(MI);
	if (SavedState.Error.IsEmpty()) Result->SetStringField(TEXT("stateHash"), SavedState.Hash);
	else { Result->RemoveField(TEXT("stateHash")); Result->SetStringField(TEXT("stateReadbackError"), SavedState.Error); }
	return FMCPToolResult::Ok(Result);
}
FMCPToolResult ExecuteBatch(const TSharedPtr<FJsonObject>& P, bool bLegacy = false)
{
	FString Error; auto* MI = Resolve(P, Error); if (!MI) return Invalid(Error);
	FGCObjectScopeGuard Guard(MI);
	bool bDryRun = false, bSave = false; P->TryGetBoolField(TEXT("dryRun"), bDryRun); P->TryGetBoolField(TEXT("save"), bSave);
	FString Filename;
	if (bSave && (MI->HasAnyFlags(RF_Transient) || !MI->HasAnyFlags(RF_Standalone) || MI->GetOutermost() == GetTransientPackage() || !FPackageName::TryConvertLongPackageNameToFilename(MI->GetOutermost()->GetName(), Filename, FPackageName::GetAssetPackageExtension()))) return Invalid(TEXT("Saving requires a standalone, non-transient asset in a mounted package; use dirty-only editing for temporary instances."));
	FState Before = ReadState(MI); if (!Before.Error.IsEmpty()) return Invalid(Before.Error);
	FString Expected; P->TryGetStringField(TEXT("expectedStateHash"), Expected);
	if (!bLegacy && Expected != Before.Hash) return Invalid(TEXT("Instance or inherited parameter state changed. Read parameters again."), TEXT("material_instance_state_conflict"), 409);
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!P->TryGetArrayField(TEXT("operations"), Operations) || Operations->Num() < 1 || Operations->Num() > 128) return Invalid(TEXT("operations must contain 1..128 edits."));
	FOverrides Old(MI), Planned(MI); TSet<FString> Seen; TArray<FChange> Changes;
	for (const auto& V : *Operations)
	{
		const auto O = V->Type == EJson::Object ? V->AsObject() : nullptr; if (!O) return Invalid(TEXT("Every operation must be an object."));
		FString Op, Name, TypeString, Association = TEXT("global"); O->TryGetStringField(TEXT("op"), Op); O->TryGetStringField(TEXT("name"), Name); O->TryGetStringField(TEXT("type"), TypeString); O->TryGetStringField(TEXT("association"), Association);
		EMaterialParameterType Type = EMaterialParameterType::None; for (auto T : Types) if (TypeName(T) == TypeString) Type = T;
		if (Type == EMaterialParameterType::None || Name.IsEmpty() || Name.Len() > 256 || (Op != TEXT("set") && Op != TEXT("clear"))) return Invalid(TEXT("Expected set/clear, a supported type, and a nonempty name up to 256 characters."));
		EMaterialParameterAssociation A; if (Association == TEXT("global")) A = GlobalParameter; else if (Association == TEXT("layer")) A = LayerParameter; else if (Association == TEXT("blend")) A = BlendParameter; else return Invalid(TEXT("association must be global/layer/blend."));
		double Index = INDEX_NONE; O->TryGetNumberField(TEXT("index"), Index);
		if (!FMath::IsFinite(Index) || Index != FMath::FloorToDouble(Index) || (A == GlobalParameter ? Index != INDEX_NONE : Index < 0 || Index > 255)) return Invalid(TEXT("Global parameters require index -1; layer/blend parameters require an explicit index 0..255."));
		FMaterialParameterInfo Info(FName(*Name), A, int32(Index)); const FString Id = Key(Type, Info);
		if (Seen.Contains(Id)) return Invalid(TEXT("Duplicate parameter edits in one batch are ambiguous.")); Seen.Add(Id);
		const auto* E = Before.Entries.Find(Id); if (!E) return Invalid(TEXT("Unknown parameter identity: ") + Id);
		const bool bClear = Op == TEXT("clear");
		if (bClear && O->HasField(TEXT("value"))) return Invalid(TEXT("clear must omit value."));
		if (!bClear && !E->bDeclared) return Invalid(TEXT("Orphan overrides can be cleared but cannot be set."));
		FMaterialParameterValue Value;
		if (!bClear)
		{
			const auto* Input = O->Values.Find(TEXT("value")); if (!Input) return Invalid(TEXT("set requires value."));
			if (Type == EMaterialParameterType::Scalar)
			{
				double N; if (!(*Input)->TryGetNumber(N) || !FMath::IsFinite(N) || FMath::Abs(N) > TNumericLimits<float>::Max()) return Invalid(TEXT("Scalar must be a finite float."));
				if (E->Effective.bUsedAsAtlasPosition) return Invalid(TEXT("Atlas-position scalar sets require the curve/atlas contract; clear remains available."));
				Value = FMaterialParameterValue(float(N));
			}
			else if (Type == EMaterialParameterType::Vector)
			{
				if ((*Input)->Type != EJson::Object) return Invalid(TEXT("Vector must be an object with numeric r/g/b/a components."));
				const auto C = (*Input)->AsObject(); double Components[] = {0,0,0,1}; const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
				for (const auto& Pair : C->Values) if (Pair.Key != TEXT("r") && Pair.Key != TEXT("g") && Pair.Key != TEXT("b") && Pair.Key != TEXT("a")) return Invalid(TEXT("Unknown vector component."));
				for (int32 I = 0; I < 4; ++I) if (C->HasField(Names[I]) && (!C->TryGetNumberField(Names[I], Components[I]) || !FMath::IsFinite(Components[I]) || FMath::Abs(Components[I]) > TNumericLimits<float>::Max())) return Invalid(TEXT("Vector components must be finite floats."));
				Value = FMaterialParameterValue(FLinearColor(Components[0], Components[1], Components[2], Components[3]));
			}
			else if (Type == EMaterialParameterType::Texture)
			{
				FString Path; if (!(*Input)->TryGetString(Path) || !Path.StartsWith(TEXT("/")) || Path.Len() > 1024) return Invalid(TEXT("Texture requires an exact asset path; use clear to inherit."));
				auto* Texture = LoadObject<UTexture>(nullptr, *Path, nullptr, LOAD_NoWarn); if (!Texture) return Invalid(TEXT("Texture not found: ") + Path);
				if (E->bInherited && E->Inherited.Value.Texture && Texture->GetMaterialType() != E->Inherited.Value.Texture->GetMaterialType()) return Invalid(TEXT("Texture kind differs from the inherited parameter texture."));
				Value = FMaterialParameterValue(Texture);
			}
			else { bool B; if (!(*Input)->TryGetBool(B)) return Invalid(TEXT("Static switch requires a boolean.")); Value = FMaterialParameterValue(B); }
		}
		if (bClear ? !E->bOverride : E->bOverride && E->Local == Value && E->LocalGuid == E->DeclaredGuid) continue;
		const FGuid Guid = E->DeclaredGuid;
		if (Type == EMaterialParameterType::Scalar) SetOverride(Planned.Scalars, Info, bClear ? 0.f : Value.AsScalar(), Guid, bClear);
		else if (Type == EMaterialParameterType::Vector) SetOverride(Planned.Vectors, Info, bClear ? FLinearColor::Black : Value.AsLinearColor(), Guid, bClear);
		else if (Type == EMaterialParameterType::Texture) SetOverride(Planned.Textures, Info, bClear ? nullptr : Value.Texture, Guid, bClear);
		else { Planned.Static.StaticSwitchParameters.RemoveAll([&](const auto& S) { return S.ParameterInfo == Info; }); if (!bClear) Planned.Static.StaticSwitchParameters.Add(FStaticSwitchParameter(Info, Value.AsStaticSwitch(), true, Guid)); }
		Changes.Add({Id, bClear, Value, Guid});
	}
	const auto Current = ReadState(MI); if (!Current.Error.IsEmpty() || Current.Hash != Before.Hash) return Invalid(TEXT("State changed while resolving operation dependencies."), TEXT("material_instance_state_conflict"), 409);
	auto Result = MakeShared<FJsonObject>(); Result->SetStringField(TEXT("instance"), MI->GetName()); Result->SetStringField(TEXT("instancePath"), MI->GetPathName());
	Result->SetStringField(TEXT("beforeStateHash"), Before.Hash); Result->SetNumberField(TEXT("changedCount"), Changes.Num()); Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetBoolField(TEXT("saved"), false); Result->SetBoolField(TEXT("shaderValidationPerformed"), false); Result->SetNumberField(TEXT("finalizeCount"), 0);
	if (bDryRun || Changes.IsEmpty()) { Result->SetStringField(TEXT("stateHash"), Before.Hash); Result->SetStringField(TEXT("status"), bDryRun ? TEXT("validatedPlan") : TEXT("noChange")); return CompleteResponse(MI, Result, bSave && !bDryRun); }
	const bool bDirty = MI->GetOutermost()->IsDirty(); const FGuid OldId = MI->ParameterStateId;
	const bool bStaticChanged = Changes.ContainsByPredicate([&](const auto& C) { return Before.Entries.FindChecked(C.Id).Type == EMaterialParameterType::StaticSwitch; });
	FScopedTransaction Transaction(NSLOCTEXT("UEAI", "InstanceParameterBatch", "Edit Material Instance Parameters"));
	MI->Modify(); MI->GetEditorOnlyData()->Modify(); MI->PreEditChange(nullptr); Planned.Assign(MI, bStaticChanged); MI->PostEditChange();
	const FState After = ReadState(MI); bool bVerified = After.Error.IsEmpty(); TArray<TSharedPtr<FJsonValue>> Mismatches;
	for (const auto& C : Changes)
	{
		const auto* E = After.Entries.Find(C.Id);
		const bool bMatches = C.bClear ? !E || (!E->bOverride && (!E->bDeclared || !E->bInherited || E->Effective.Value == E->Inherited.Value))
			: E && E->bOverride && E->Local == C.Value && E->LocalGuid == C.Guid && E->Effective.Value == C.Value;
		bVerified &= bMatches;
		if (!bMatches)
		{
			auto D = MakeShared<FJsonObject>(); D->SetStringField(TEXT("parameter"), C.Id); D->SetBoolField(TEXT("clear"), C.bClear); D->SetField(TEXT("expected"), JsonValue(C.Value)); D->SetStringField(TEXT("expectedGuid"), C.Guid.ToString());
			if (E) { D->SetField(TEXT("local"), JsonValue(E->Local)); D->SetField(TEXT("effective"), JsonValue(E->Effective.Value)); D->SetStringField(TEXT("actualGuid"), E->LocalGuid.ToString()); D->SetBoolField(TEXT("overridden"), E->bOverride); }
			Mismatches.Add(MakeShared<FJsonValueObject>(D));
		}
	}
	if (!bVerified)
	{
		Old.Assign(MI, bStaticChanged); MI->PostEditChange(); MI->ParameterStateId = OldId; MI->GetOutermost()->SetDirtyFlag(bDirty); Transaction.Cancel();
		const auto Restored = ReadState(MI); auto Failure = Invalid(TEXT("Parameter readback failed; instance overrides restored."), TEXT("material_instance_readback_failed"), 409);
		Result->SetBoolField(TEXT("restoreVerified"), Restored.Error.IsEmpty() && Restored.Hash == Before.Hash); Result->SetArrayField(TEXT("mismatches"), Mismatches); Result->SetStringField(TEXT("readbackError"), After.Error); Failure.Data = Result; return Failure;
	}
	MI->MarkPackageDirty(); FEditorDelegates::RefreshEditor.Broadcast(); FEditorSupportDelegates::RedrawAllViewports.Broadcast();
	Result->SetNumberField(TEXT("finalizeCount"), 1); Result->SetStringField(TEXT("stateHash"), After.Hash); Result->SetBoolField(TEXT("readbackVerified"), true);
	Result->SetStringField(TEXT("status"), TEXT("appliedDirty"));
	return CompleteResponse(MI, Result, bSave);
}
class FTool_GetMaterialInstanceParameters : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.instance.parameters.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& P) override
	{
		FString Error; auto* MI = Resolve(P, Error); if (!MI) return Invalid(Error); const auto S = ReadState(MI); if (!S.Error.IsEmpty()) return Invalid(S.Error);
		int32 Offset = 0, Limit = 100; P->TryGetNumberField(TEXT("offset"), Offset); P->TryGetNumberField(TEXT("limit"), Limit); if (Offset < 0 || Limit < 1 || Limit > 500) return Invalid(TEXT("offset >= 0 and limit 1..500 required."));
		FString Expected; if (P->TryGetStringField(TEXT("expectedStateHash"), Expected) && Expected != S.Hash) return Invalid(TEXT("State changed between pages."), TEXT("material_instance_state_conflict"), 409);
		auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("instancePath"), MI->GetPathName()); R->SetStringField(TEXT("parentPath"), MI->Parent->GetPathName()); R->SetStringField(TEXT("stateHash"), S.Hash);
		TArray<TSharedPtr<FJsonValue>> Page; for (int32 I = Offset; I < S.Rows.Num() && Page.Num() < Limit; ++I) Page.Add(S.Rows[I]);
		R->SetArrayField(TEXT("parameters"), Page); R->SetNumberField(TEXT("total"), S.Rows.Num()); R->SetBoolField(TEXT("hasMore"), Offset < S.Rows.Num() - Page.Num());
		R->SetNumberField(TEXT("nextOffset"), FMath::Min(Offset, S.Rows.Num()) + Page.Num()); R->SetBoolField(TEXT("compileTriggered"), false); R->SetBoolField(TEXT("saved"), false); return FMCPToolResult::Ok(R);
	}
};
class FTool_BatchMaterialInstanceParameters : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.instance.parameters.batch"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& P) override { return ExecuteBatch(P); }
};
class FTool_SetMaterialInstanceParent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.instance.set_parent"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& P) override
	{
		FString Error; auto* MI = Resolve(P, Error); if (!MI) return Invalid(Error);
		FGCObjectScopeGuard Guard(MI);
		FString ParentPath; P->TryGetStringField(TEXT("parent"), ParentPath);
		if (!ParentPath.StartsWith(TEXT("/"))) return Invalid(TEXT("parent must be an exact asset path."));
		UMaterialInterface* NewParent = MCPMaterialInfrastructure::LoadMaterialInterfaceByName(ParentPath, Error);
		if (!NewParent) return Invalid(Error);
		FGCObjectScopeGuard ParentGuard(NewParent);
		// Reject a parent that is this instance or one of its descendants: assigning
		// it would close a cycle through the instance's own parent chain. UE's own
		// SetParentInternal only warns on this case, so reject it up front instead.
		TSet<UMaterialInterface*> Seen;
		for (UMaterialInterface* Cur = NewParent; Cur; )
		{
			if (Cur == MI) return Invalid(TEXT("New parent is this instance or one of its descendants; reparenting would create a cycle."), TEXT("material_instance_parent_cycle"), 409);
			if (Seen.Contains(Cur) || Seen.Num() >= 64) break;
			Seen.Add(Cur);
			auto* Inst = Cast<UMaterialInstance>(Cur);
			Cur = Inst ? Inst->Parent.Get() : nullptr;
		}
		// Snapshot local overrides before reparenting. UE keeps orphan overrides in
		// place, so retained vs dropped is decided against the new parent's declared set.
		struct FOverrideSnapshot { FMaterialParameterInfo Info; EMaterialParameterType Type; FMaterialParameterValue Value; };
		TArray<FOverrideSnapshot> Overrides;
		for (const auto& V : MI->ScalarParameterValues) Overrides.Add({V.ParameterInfo, EMaterialParameterType::Scalar, FMaterialParameterValue(V.ParameterValue)});
		for (const auto& V : MI->VectorParameterValues) Overrides.Add({V.ParameterInfo, EMaterialParameterType::Vector, FMaterialParameterValue(V.ParameterValue)});
		for (const auto& V : MI->TextureParameterValues) Overrides.Add({V.ParameterInfo, EMaterialParameterType::Texture, FMaterialParameterValue(V.ParameterValue)});
		const FStaticParameterSet LocalStatic = MI->GetStaticParameters();
		for (const auto& V : LocalStatic.StaticSwitchParameters) if (V.bOverride) Overrides.Add({V.ParameterInfo, EMaterialParameterType::StaticSwitch, FMaterialParameterValue(V.Value)});
		const FString OldParentPath = MI->Parent ? MI->Parent->GetPathName() : TEXT("");
		{
			FScopedTransaction Transaction(NSLOCTEXT("UEAI", "InstanceSetParent", "Set Material Instance Parent"));
			MI->Modify(); MI->GetEditorOnlyData()->Modify(); MI->PreEditChange(nullptr);
			// RecacheShader=false avoids a forced synchronous shader pass; PostEditChange
			// propagates the new parent into cached data without blocking on compilation.
			MI->SetParentEditorOnly(NewParent, false);
			MI->PostEditChange();
			MI->MarkPackageDirty();
		}
		auto IsDeclared = [&](EMaterialParameterType Type, const FMaterialParameterInfo& Info)
		{
			TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
			MI->GetAllParameterInfoOfType(Type, Infos, Guids);
			return Infos.Contains(Info);
		};
		TArray<TSharedPtr<FJsonValue>> Retained, Dropped;
		for (const auto& O : Overrides)
		{
			const bool bRetained = IsDeclared(O.Type, O.Info);
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), O.Info.Name.ToString());
			Row->SetStringField(TEXT("type"), TypeName(O.Type));
			Row->SetStringField(TEXT("association"), AssociationName(O.Info.Association));
			Row->SetNumberField(TEXT("index"), O.Info.Index);
			Row->SetField(TEXT("value"), JsonValue(O.Value));
			(bRetained ? Retained : Dropped).Add(MakeShared<FJsonValueObject>(Row));
		}
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("instance"), MI->GetName());
		Result->SetStringField(TEXT("instancePath"), MI->GetPathName());
		Result->SetStringField(TEXT("oldParentPath"), OldParentPath);
		Result->SetStringField(TEXT("newParentPath"), NewParent->GetPathName());
		Result->SetArrayField(TEXT("retainedParameters"), Retained);
		Result->SetArrayField(TEXT("droppedParameters"), Dropped);
		Result->SetNumberField(TEXT("retainedCount"), Retained.Num());
		Result->SetNumberField(TEXT("droppedCount"), Dropped.Num());
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compileTriggered"), false);
		return FMCPToolResult::Ok(Result);
	}
};

// Read-only inventory of authored material instances by parent. Recursive
// mode walks GetParent() upward through intermediate instances to the base
// UMaterial; non-recursive mode matches only the immediate parent object.
bool MaterialInstanceListMatchesParent(UMaterialInstance* Instance, UMaterialInterface* Target, bool bRecursive)
{
	UMaterialInterface* Parent = Instance->Parent.Get();
	if (!Parent)
	{
		return false;
	}
	if (!bRecursive)
	{
		return Parent == Target;
	}
	TSet<const UMaterialInterface*> Seen;
	for (UMaterialInterface* Cur = Parent; Cur; )
	{
		if (Seen.Contains(Cur) || Seen.Num() >= 64)
		{
			break;
		}
		Seen.Add(Cur);
		if (Cur == Target)
		{
			return true;
		}
		UMaterialInstance* Inst = Cast<UMaterialInstance>(Cur);
		Cur = Inst ? Inst->Parent.Get() : nullptr;
	}
	return false;
}

class FTool_ListMaterialInstances : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.instance.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& P) override
	{
		if (!P.IsValid())
		{
			return Invalid(TEXT("parent must be an exact Material or MaterialInstance object/package path."));
		}
		FString ParentPath;
		if (!P->TryGetStringField(TEXT("parent"), ParentPath) || ParentPath.IsEmpty() || !ParentPath.StartsWith(TEXT("/")))
		{
			return Invalid(TEXT("parent must be an exact Material or MaterialInstance object/package path."));
		}
		bool bRecursive = false;
		if (P->HasField(TEXT("recursive")))
		{
			const TSharedPtr<FJsonValue> Field = P->TryGetField(TEXT("recursive"));
			if (!Field.IsValid() || Field->Type != EJson::Boolean)
			{
				return Invalid(TEXT("recursive must be a boolean."));
			}
			bRecursive = Field->AsBool();
		}
		int32 MaxResults = 256;
		if (P->HasField(TEXT("maxResults")))
		{
			double Number = 0;
			if (!P->TryGetNumberField(TEXT("maxResults"), Number) || !FMath::IsFinite(Number)
				|| Number != FMath::FloorToDouble(Number) || Number < 1 || Number > 4096)
			{
				return Invalid(TEXT("maxResults must be an integer in [1, 4096]."));
			}
			MaxResults = static_cast<int32>(Number);
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ParentPath);
		const FString ObjectPath = ParentPath.Contains(TEXT(".")) ? ParentPath : PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		if (!Parent)
		{
			return Invalid(FString::Printf(TEXT("Material parent '%s' was not found."), *ParentPath), TEXT("material_parent_not_found"), 404);
		}
		if (!Cast<UMaterial>(Parent) && !Cast<UMaterialInstance>(Parent))
		{
			return Invalid(TEXT("parent must resolve to a Material or MaterialInstance."), TEXT("material_parent_not_found"), 404);
		}
		FGCObjectScopeGuard ParentGuard(Parent);
		const FString ResolvedParentPath = Parent->GetPathName();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UMaterialInstance::StaticClass()->GetClassPathName(), Assets, true);
		Assets.Sort([](const FAssetData& Left, const FAssetData& Right) { return Left.GetObjectPathString() < Right.GetObjectPathString(); });
		TArray<TSharedPtr<FJsonValue>> Instances;
		int32 TotalFound = 0;
		for (const FAssetData& Asset : Assets)
		{
			UMaterialInstance* Instance = Cast<UMaterialInstance>(Asset.GetAsset());
			if (!Instance || !MaterialInstanceListMatchesParent(Instance, Parent, bRecursive))
			{
				continue;
			}
			++TotalFound;
			if (Instances.Num() >= MaxResults)
			{
				continue;
			}
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), Instance->GetPathName());
			Row->SetStringField(TEXT("class"), Instance->GetClass()->GetName());
			Row->SetStringField(TEXT("parentPath"), Instance->Parent ? Instance->Parent->GetPathName() : TEXT(""));
			Instances.Add(MakeShared<FJsonValueObject>(Row));
		}
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.material.instance-list.v1"));
		Result->SetStringField(TEXT("parent"), ResolvedParentPath);
		Result->SetBoolField(TEXT("recursive"), bRecursive);
		Result->SetNumberField(TEXT("totalFound"), TotalFound);
		Result->SetNumberField(TEXT("returned"), Instances.Num());
		Result->SetBoolField(TEXT("truncated"), TotalFound > MaxResults);
		Result->SetNumberField(TEXT("maxResults"), MaxResults);
		Result->SetArrayField(TEXT("instances"), Instances);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compiled"), false);
		Result->SetStringField(TEXT("scope"), TEXT("authored asset inventory only; runtime material instance dynamics and PIE overrides are not inspected"));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_ClearMaterialInstanceParameter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.instance.parameter.clear"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& P) override
	{
		FString Parameter;
		if (!P->TryGetStringField(TEXT("parameter"), Parameter) || Parameter.IsEmpty() || Parameter.Len() > 256) return Invalid(TEXT("parameter must be a nonempty name up to 256 characters."));
		FString InstancePath;
		if (!P->TryGetStringField(TEXT("instance"), InstancePath) || !InstancePath.StartsWith(TEXT("/"))) return Invalid(TEXT("instance must be an exact asset path."));
		FString LoadError; auto* MI = MCPMaterialInfrastructure::LoadMaterialInstanceByName(InstancePath, LoadError);
		if (!MI) return Invalid(LoadError, TEXT("instance_not_found"), 404);
		FGCObjectScopeGuard Guard(MI);
		// Writes require a non-transient /Game/ instance. Runtime/PIE overrides are
		// out of scope, so refuse transient and non-/Game/ assets up front.
		if (MI->HasAnyFlags(RF_Transient) || MI->GetOutermost() == GetTransientPackage() || !MI->GetPathName().StartsWith(TEXT("/Game/")))
			return Invalid(TEXT("Clearing parameter overrides requires a non-transient /Game/ Material Instance."), TEXT("material_instance_read_only"), 409);
		const auto NameMatches = [&](const FMaterialParameterInfo& Info) { return Info.Name.ToString().Equals(Parameter, ESearchCase::IgnoreCase); };
		auto CountNamed = [&](const auto& Array) { int32 N = 0; for (const auto& V : Array) if (NameMatches(V.ParameterInfo)) ++N; return N; };
		const int32 Matches = CountNamed(MI->ScalarParameterValues) + CountNamed(MI->VectorParameterValues) + CountNamed(MI->DoubleVectorParameterValues) + CountNamed(MI->TextureParameterValues) + CountNamed(MI->FontParameterValues) + CountNamed(MI->RuntimeVirtualTextureParameterValues);
		if (Matches == 0) return Invalid(TEXT("Parameter has no local override to clear: ") + Parameter, TEXT("parameter_not_found"), 404);
		FScopedTransaction Transaction(NSLOCTEXT("UEAI", "InstanceParameterClear", "Clear Material Instance Parameter Override"));
		MI->Modify(); MI->PreEditChange(nullptr);
		auto RemoveNamed = [&](auto& Array) { return Array.RemoveAll([&](const auto& V) { return NameMatches(V.ParameterInfo); }); };
		const int32 RemovedCount = RemoveNamed(MI->ScalarParameterValues) + RemoveNamed(MI->VectorParameterValues) + RemoveNamed(MI->DoubleVectorParameterValues) + RemoveNamed(MI->TextureParameterValues) + RemoveNamed(MI->FontParameterValues) + RemoveNamed(MI->RuntimeVirtualTextureParameterValues);
		MI->PostEditChange(); MI->MarkPackageDirty();
		FEditorDelegates::RefreshEditor.Broadcast(); FEditorSupportDelegates::RedrawAllViewports.Broadcast();
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.material.instance-parameter-clear.v1"));
		Result->SetStringField(TEXT("instance"), MI->GetName());
		Result->SetStringField(TEXT("instancePath"), MI->GetPathName());
		Result->SetStringField(TEXT("parameter"), Parameter);
		Result->SetNumberField(TEXT("removedCount"), RemovedCount);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("dirty"), MI->GetOutermost()->IsDirty());
		Result->SetStringField(TEXT("scope"), TEXT("authored override removed; runtime/PIE overrides unverified"));
		return FMCPToolResult::Ok(Result);
	}
};
}
namespace UEAIIntegrationTools
{
void RegisterMaterialInstanceTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_GetMaterialInstanceParameters>()); Registry.Register(MakeShared<FTool_BatchMaterialInstanceParameters>()); Registry.Register(MakeShared<FTool_SetMaterialInstanceParent>()); Registry.Register(MakeShared<FTool_ListMaterialInstances>()); Registry.Register(MakeShared<FTool_ClearMaterialInstanceParameter>());
}
FMCPToolResult SetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params)
{
	FString Error, Name; Params->TryGetStringField(TEXT("parameterName"), Name);
	FString Instance; Params->TryGetStringField(TEXT("instance"), Instance);
	auto* MI = MCPMaterialInfrastructure::LoadMaterialInstanceByName(Instance, Error); if (!MI) return Invalid(Error);
	const auto* Value = Params->Values.Find(TEXT("value")); if (!Value || ((*Value)->Type != EJson::Number && (*Value)->Type != EJson::Object)) return Invalid(TEXT("value must be a scalar number or vector object."));
	auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("instance"), MI->GetPathName());
	for (const TCHAR* Field : {TEXT("save"), TEXT("dryRun")}) if (const auto* V = Params->Values.Find(Field)) P->SetField(Field, *V);
	auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("op"), TEXT("set")); O->SetStringField(TEXT("name"), Name); O->SetStringField(TEXT("type"), (*Value)->Type == EJson::Number ? TEXT("scalar") : TEXT("vector")); O->SetField(TEXT("value"), *Value);
	P->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(O)}); auto Result = ExecuteBatch(P, true);
	if (Result.Data) { Result.Data->SetBoolField(TEXT("success"), Result.bSuccess); Result.Data->SetStringField(TEXT("parameterName"), Name); Result.Data->SetStringField(TEXT("parameterType"), (*Value)->Type == EJson::Number ? TEXT("Scalar") : TEXT("Vector")); }
	return Result;
}
}
