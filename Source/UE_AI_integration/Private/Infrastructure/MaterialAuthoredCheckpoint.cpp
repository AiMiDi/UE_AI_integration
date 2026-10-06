#include "Infrastructure/MaterialAuthoredCheckpoint.h"

#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/MaterialSharedWriteProtection.h"
#include "Infrastructure/Sha256.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "ScopedTransaction.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace UEAIIntegration::MaterialCheckpoint
{
namespace
{
struct FCheckpoint
{
	TStrongObjectPtr<UMaterial> Image;
	// Outer ownership alone does not keep unreferenced authored descendants alive.
	TArray<TStrongObjectPtr<UObject>> OwnedObjects;
	FString AssetPath;
	FString Digest;
	double CapturedAt = 0.0;
};

TMap<FString, TSharedPtr<FCheckpoint>>& Checkpoints()
{
	static TMap<FString, TSharedPtr<FCheckpoint>> Values;
	return Values;
}

bool IsAuthoredProperty(const FProperty* Property)
{
	// Native Undo refreshes these derived shader/lightmass cache identities via
	// PostEditChange. They describe compiled state, not the authored graph; do
	// not make an otherwise exact source restore depend on cache regeneration.
	const UClass* OwnerClass = Property ? Property->GetOwnerClass() : nullptr;
	if (OwnerClass && OwnerClass->IsChildOf(UMaterialInterface::StaticClass()))
	{
		const FName Name = Property->GetFName();
		if (Name == TEXT("StateId") || Name == TEXT("ReferencedTextureGuids") || Name == TEXT("LightingGuid"))
		{
			return false;
		}
	}
	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		if (ObjectProperty->PropertyClass
			&& (ObjectProperty->PropertyClass->IsChildOf(UEdGraph::StaticClass())
				|| ObjectProperty->PropertyClass->IsChildOf(UEdGraphNode::StaticClass())))
		{
			return false;
		}
	}
	return Property && !Property->HasAnyPropertyFlags(
		CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient);
}

void CopyAuthoredProperty(FProperty* Property, UObject* Destination, const UObject* Source)
{
	if (!Property || !Destination || !Source)
	{
		return;
	}
	// Native bools such as UMaterial::TwoSided may be packed bitfields. Read and
	// write each logical bool through its accessor to preserve neighbouring bits.
	if (const FBoolProperty* BoolProperty = CastField<const FBoolProperty>(Property))
	{
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			BoolProperty->SetPropertyValue_InContainer(
				Destination,
				BoolProperty->GetPropertyValue_InContainer(Source, Index),
				Index);
		}
		return;
	}
	Property->CopyCompleteValue_InContainer(Destination, Source);
}

void GatherAuthoredObjects(UMaterial* Material, TArray<UObject*>& Objects)
{
	Objects.Reset();
	if (!Material)
	{
		return;
	}
	Objects.Add(Material);
	if (UObject* EditorData = Material->GetEditorOnlyData())
	{
		Objects.Add(EditorData);
	}
	auto AddExpression = [&Objects](UMaterialExpression* Expression)
	{
		if (!IsValid(Expression))
		{
			return;
		}
		Objects.AddUnique(Expression);
		TArray<UObject*> Children;
		GetObjectsWithOuter(Expression, Children, true);
		for (UObject* Child : Children)
		{
			if (IsValid(Child) && !Child->IsA<UEdGraph>() && !Child->IsA<UEdGraphNode>())
			{
				Objects.AddUnique(Child);
			}
		}
	};
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		AddExpression(Expression);
	}
	for (UMaterialExpressionComment* Comment : Material->GetEditorComments())
	{
		AddExpression(Comment);
	}
	Objects.Sort([](const UObject& Left, const UObject& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
}

FString CanonicalAuthoredObjectPath(UObject* Object, UMaterial* Material, const FString& AssetPath)
{
	const FString SourcePrefix = Material->GetPathName();
	FString CanonicalObjectName = AssetPath;
	int32 ObjectNameSeparator = INDEX_NONE;
	if (AssetPath.FindLastChar(TEXT('.'), ObjectNameSeparator))
	{
		CanonicalObjectName = AssetPath.Mid(ObjectNameSeparator + 1);
	}
	const FString ImageObjectName = Material->GetName();
	const FString ImageObjectReferencePrefix = TEXT(":") + ImageObjectName;
	const FString CanonicalObjectReferencePrefix = TEXT(":") + CanonicalObjectName;
	FString Canonical = Object->GetPathName().Replace(*SourcePrefix, *AssetPath);
	Canonical.ReplaceInline(*ImageObjectReferencePrefix, *CanonicalObjectReferencePrefix);
	return Canonical;
}

FString AuthoredObjectRecord(UObject* Object, UMaterial* Material, const FString& AssetPath)
{
	const FString SourcePrefix = Material->GetPathName();
	FString CanonicalObjectName = AssetPath;
	int32 ObjectNameSeparator = INDEX_NONE;
	if (AssetPath.FindLastChar(TEXT('.'), ObjectNameSeparator))
	{
		CanonicalObjectName = AssetPath.Mid(ObjectNameSeparator + 1);
	}
	const FString ImageObjectName = Material->GetName();
	const FString ImageObjectReferencePrefix = TEXT(":") + ImageObjectName;
	const FString CanonicalObjectReferencePrefix = TEXT(":") + CanonicalObjectName;
	FString Canonical = CanonicalAuthoredObjectPath(Object, Material, AssetPath);
	Canonical += TEXT("|") + Object->GetClass()->GetPathName() + TEXT("|");
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsAuthoredProperty(Property))
		{
			continue;
		}
		FString Value;
		for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
		{
			// A null delta forces ExportText_Direct to emit the authored value.
			// Passing Object as both Data and Delta makes UE treat the property as
			// identical and leaves Value empty, which would remove property edits
			// from the checkpoint digest entirely.
			Property->ExportText_InContainer(Index, Value, Object, nullptr, Object, PPF_None);
			Value += TEXT(";");
		}
		Value.ReplaceInline(*SourcePrefix, *AssetPath);
		// ExportText_InContainer can encode a subobject reference with the
		// duplicated root object's short name after a colon. The package path
		// replacement above cannot see that suffix, so canonicalize the root
		// object prefix independently of the transient duplicate's name.
		Value.ReplaceInline(*ImageObjectReferencePrefix, *CanonicalObjectReferencePrefix);
		Canonical += FString::Printf(TEXT("%d:%s=%d:%s|"),
			Property->GetName().Len(), *Property->GetName(), Value.Len(), *Value);
	}
	return Canonical;
}

FString AuthoredDigest(UMaterial* Material, const FString& AssetPath)
{
	TArray<UObject*> Objects;
	GatherAuthoredObjects(Material, Objects);
	FString Canonical = TEXT("ue.material.authoredGraph/1|") + AssetPath + TEXT("|");
	TArray<FString> Records;
	Records.Reserve(Objects.Num());
	for (UObject* Object : Objects)
	{
		Records.Add(AuthoredObjectRecord(Object, Material, AssetPath));
	}
	// Duplicate images can have different transient object names, so the raw
	// UObject path order is not a stable ordering for a cross-process digest.
	// Sort the already canonicalized records instead.
	Records.Sort();
	for (const FString& Record : Records)
	{
		Canonical += Record;
	}
	FTCHARToUTF8 Utf8(*Canonical);
	FString Hex;
	return UEAIIntegration::Infrastructure::TrySha256Hex(
		Utf8.Get(), static_cast<uint64>(Utf8.Length()), Hex)
		? TEXT("sha256:") + Hex : FString();
}

FString AuthoredDigestMismatch(UMaterial* Original, UMaterial* Image, const FString& AssetPath)
{
	if (!Original || !Image)
	{
		return TEXT("image_or_original_null");
	}
	TArray<UObject*> OriginalObjects;
	TArray<UObject*> ImageObjects;
	GatherAuthoredObjects(Original, OriginalObjects);
	GatherAuthoredObjects(Image, ImageObjects);
	const FString OriginalPrefix = Original->GetPathName();
	const FString ImagePrefix = Image->GetPathName();
	auto BuildRecords = [&AssetPath](UMaterial* Material, const FString& SourcePrefix,
		TArray<UObject*>& Objects, TMap<FString, FString>& Records)
	{
		for (UObject* Object : Objects)
		{
			const FString StablePath = CanonicalAuthoredObjectPath(Object, Material, AssetPath);
			Records.Add(StablePath, AuthoredObjectRecord(Object, Material, AssetPath));
		}
	};
	TMap<FString, FString> OriginalRecords;
	TMap<FString, FString> ImageRecords;
	BuildRecords(Original, OriginalPrefix, OriginalObjects, OriginalRecords);
	BuildRecords(Image, ImagePrefix, ImageObjects, ImageRecords);
	if (OriginalRecords.Num() != ImageRecords.Num())
	{
		FString Missing;
		int32 MissingCount = 0;
		for (const auto& Pair : OriginalRecords)
		{
			if (!ImageRecords.Contains(Pair.Key))
			{
				if (MissingCount++ < 3)
				{
					if (!Missing.IsEmpty()) Missing += TEXT(",");
					Missing += Pair.Key;
				}
			}
		}
		FString Extra;
		int32 ExtraCount = 0;
		for (const auto& Pair : ImageRecords)
		{
			if (!OriginalRecords.Contains(Pair.Key))
			{
				if (ExtraCount++ < 3)
				{
					if (!Extra.IsEmpty()) Extra += TEXT(",");
					Extra += Pair.Key;
				}
			}
		}
		return FString::Printf(TEXT("object_count:%d/%d;missing=%s%s;extra=%s%s"),
			OriginalRecords.Num(), ImageRecords.Num(), *Missing, MissingCount > 3 ? TEXT(",...") : TEXT(""),
			*Extra, ExtraCount > 3 ? TEXT(",...") : TEXT(""));
	}
	auto Fingerprint = [](const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		FString Hash;
		return UEAIIntegration::Infrastructure::TrySha256Hex(
			Utf8.Get(), static_cast<uint64>(Utf8.Length()), Hash) ? Hash : TEXT("hash_unavailable");
	};
	for (const auto& Pair : OriginalRecords)
	{
		const FString* ImageRecord = ImageRecords.Find(Pair.Key);
		if (!ImageRecord)
		{
			return FString::Printf(TEXT("missing_image_record:%s"), *Pair.Key);
		}
		if (*ImageRecord != Pair.Value)
		{
			FString PropertyDifference;
			UObject* OriginalObject = nullptr;
			UObject* ImageObject = nullptr;
			for (UObject* Object : OriginalObjects)
			{
				if (CanonicalAuthoredObjectPath(Object, Original, AssetPath) == Pair.Key)
				{
					OriginalObject = Object;
					break;
				}
			}
			for (UObject* Object : ImageObjects)
			{
				if (CanonicalAuthoredObjectPath(Object, Image, AssetPath) == Pair.Key)
				{
					ImageObject = Object;
					break;
				}
			}
			if (OriginalObject && ImageObject && OriginalObject->GetClass() == ImageObject->GetClass())
			{
				for (TFieldIterator<FProperty> It(OriginalObject->GetClass()); It; ++It)
				{
					FProperty* Property = *It;
					if (!IsAuthoredProperty(Property)) continue;
					FString OriginalValue;
					FString ImageValue;
					for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
					{
						Property->ExportText_InContainer(Index, OriginalValue, OriginalObject, nullptr, OriginalObject, PPF_None);
						Property->ExportText_InContainer(Index, ImageValue, ImageObject, nullptr, ImageObject, PPF_None);
						OriginalValue += TEXT(";");
						ImageValue += TEXT(";");
					}
					OriginalValue.ReplaceInline(*OriginalPrefix, *AssetPath);
					ImageValue.ReplaceInline(*ImagePrefix, *AssetPath);
					const FString OriginalObjectName = Original->GetName();
					const FString ImageObjectName = Image->GetName();
					FString CanonicalObjectName = AssetPath;
					int32 ObjectNameSeparator = INDEX_NONE;
					if (AssetPath.FindLastChar(TEXT('.'), ObjectNameSeparator))
					{
						CanonicalObjectName = AssetPath.Mid(ObjectNameSeparator + 1);
					}
					ImageValue.ReplaceInline(*(TEXT(":") + ImageObjectName), *(TEXT(":") + CanonicalObjectName));
					OriginalValue.ReplaceInline(*(TEXT(":") + OriginalObjectName), *(TEXT(":") + CanonicalObjectName));
					if (OriginalValue != ImageValue)
					{
						PropertyDifference = FString::Printf(TEXT(";property=%s;orig_value=%s;image_value=%s"),
							*Property->GetName(), *OriginalValue.Left(256), *ImageValue.Left(256));
						break;
					}
				}
			}
			return FString::Printf(TEXT("record_mismatch:%s;orig_len=%d;image_len=%d;orig_sha256=%s;image_sha256=%s%s"),
				*Pair.Key, Pair.Value.Len(), ImageRecord->Len(),
				*Fingerprint(Pair.Value), *Fingerprint(*ImageRecord), *PropertyDifference);
		}
	}
	return TEXT("records_equal_digest_mismatch");
}

UMaterial* DuplicateAuthoredImage(
	UMaterial* Material, const FName Prefix, TArray<TStrongObjectPtr<UObject>>& OutOwnedObjects)
{
	OutOwnedObjects.Reset();
	TMap<UObject*, UObject*> CreatedObjects;
	FObjectDuplicationParameters Duplicate(Material, GetTransientPackage());
	Duplicate.DestName = MakeUniqueObjectName(GetTransientPackage(), Material->GetClass(), Prefix);
	Duplicate.FlagMask = RF_AllFlags & ~(RF_Public | RF_Standalone);
	Duplicate.PortFlags |= PPF_DuplicateVerbatim;
	Duplicate.CreatedObjects = &CreatedObjects;
	UMaterial* Image = Cast<UMaterial>(StaticDuplicateObjectEx(Duplicate));
	if (!Image)
	{
		return nullptr;
	}
	// EditorOnlyData is an optional, owner-created subobject rather than a
	// default subobject. The duplication reader normally records the populated
	// source-data copy in CreatedObjects and PostDuplicate renames that same
	// object to the duplicate root's expected name. Keep that source-to-image
	// identity explicit so the root EditorOnlyData pointer is always remapped
	// back to the populated image object and is never duplicated a second time
	// by the authored-object fallback below.
	UObject* SourceEditorOnlyData = Material->GetEditorOnlyData();
	UObject* ImageEditorOnlyData = Image->GetEditorOnlyData();
	if (!SourceEditorOnlyData || !ImageEditorOnlyData)
	{
		return nullptr;
	}
	if (UObject* ExistingImageEditorOnlyData = CreatedObjects.FindRef(SourceEditorOnlyData))
	{
		if (ExistingImageEditorOnlyData != ImageEditorOnlyData)
		{
			return nullptr;
		}
	}
	else
	{
		// The normal duplication reader must have populated this mapping because
		// EditorOnlyData is a reflected UPROPERTY. Do not bind the auto-created
		// empty object as a fallback: its non-UPROPERTY cached state and owner
		// linkage are not a faithful checkpoint image.
		return nullptr;
	}
	// PostDuplicate may regenerate cache/parameter GUIDs even with verbatim port
	// flags. Copy the persisted fields from the original, then redirect only
	// internal references into the independent image.
	TArray<UObject*> Objects;
	GatherAuthoredObjects(Material, Objects);
	// Native duplication follows serialized references. Owned descendants may
	// have no reflected incoming reference, but still belong to this checkpoint.
	// Duplicate each missing subtree into its already resolved copied outer.
	for (UObject* Source : Objects)
	{
		if (CreatedObjects.Contains(Source)) continue;
		UObject* CopiedOuter = CreatedObjects.FindRef(Source->GetOuter());
		if (!CopiedOuter) return nullptr;
		FObjectDuplicationParameters OwnedDuplicate(Source, CopiedOuter);
		OwnedDuplicate.DestName = Source->GetFName();
		OwnedDuplicate.FlagMask = Duplicate.FlagMask;
		OwnedDuplicate.PortFlags |= PPF_DuplicateVerbatim;
		OwnedDuplicate.DuplicationSeed = CreatedObjects;
		OwnedDuplicate.CreatedObjects = &CreatedObjects;
		if (!StaticDuplicateObjectEx(OwnedDuplicate)) return nullptr;
	}
	for (UObject* Source : Objects)
	{
		UObject* Destination = CreatedObjects.FindRef(Source);
		if (!Destination)
		{
			return nullptr;
		}
		for (TFieldIterator<FProperty> It(Source->GetClass()); It; ++It)
		{
			if (IsAuthoredProperty(*It))
			{
				CopyAuthoredProperty(*It, Destination, Source);
			}
		}
	}
	for (UObject* Source : Objects)
	{
		FArchiveReplaceObjectRef<UObject> Remap(CreatedObjects.FindRef(Source), CreatedObjects,
			EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		OutOwnedObjects.Emplace(CreatedObjects.FindRef(Source));
	}
	return Image;
}

struct FObjectRestore
{
	UObject* Image = nullptr;
	UObject* Target = nullptr;
	UObject* Occupant = nullptr;
	FString RelativePath;
	bool bRename = false;
	enum class ECollectionMembership : uint8
	{
		None,
		Expression,
		Comment,
	};
	ECollectionMembership CollectionMembership = ECollectionMembership::None;
};

struct FOwnedSubtreeRemoval
{
	UObject* Object = nullptr;
	UObject* OriginalOuter = nullptr;
	FName OriginalName;
	FName RetiredName;
	TArray<UObject*> OwnedObjects;
	FObjectRestore::ECollectionMembership CollectionMembership = FObjectRestore::ECollectionMembership::None;
};

// No UObject mutation is permitted here. Resolve every identity, including the
// destination of renamed expressions, before an apply may recreate or copy data.
FMCPToolResult BuildRestorePlan(
	UMaterial* Material, UMaterial* Image, TArray<FObjectRestore>& Plan,
	TArray<FOwnedSubtreeRemoval>& RemovedSubtrees)
{
	if (!Material || !Image)
	{
		return FMCPToolResult::Error(
			TEXT("Material checkpoint restore requires both the live material and its image."),
			TEXT("restore_preflight_failed"), 409);
	}
	TArray<UObject*> SavedObjects;
	TArray<UObject*> CurrentObjects;
	GatherAuthoredObjects(Image, SavedObjects);
	GatherAuthoredObjects(Material, CurrentObjects);
	// UMaterialInterface::PostDuplicate creates a fresh optional data object and
	// names it from the duplicated material (for example,
	// UEAIAuthoredCheckpoint_0EditorOnlyData). That name is intentionally
	// different from the live asset's EditorOnlyData name, so it cannot be
	// resolved through StaticFindObjectFast. Bind the two objects by their
	// owner-owned semantic slot and preserve the live object identity. Copying
	// the root material later may temporarily copy the image pointer, but the
	// replacement archive maps it back to this exact target object.
	UMaterialEditorOnlyData* ImageEditorOnlyData = Image->GetEditorOnlyData();
	UMaterialEditorOnlyData* MaterialEditorOnlyData = Material->GetEditorOnlyData();
	if (!ImageEditorOnlyData || !MaterialEditorOnlyData)
	{
		return FMCPToolResult::Error(
			TEXT("Material checkpoint restore requires valid EditorOnlyData objects."),
			TEXT("restore_preflight_failed"), 409);
	}
	auto OuterDepth = [](const UObject* Object)
	{
		int32 Depth = 0;
		for (const UObject* Outer = Object->GetOuter(); Outer; Outer = Outer->GetOuter()) ++Depth;
		return Depth;
	};
	SavedObjects.Sort([&OuterDepth](const UObject& Left, const UObject& Right)
	{
		const int32 LeftDepth = OuterDepth(&Left);
		const int32 RightDepth = OuterDepth(&Right);
		return LeftDepth == RightDepth ? Left.GetPathName() < Right.GetPathName() : LeftDepth < RightDepth;
	});
	TMap<UObject*, UObject*> ResolvedObjects;
	ResolvedObjects.Add(Image, Material);
	TSet<UObject*> Claimed;
	for (UObject* Saved : SavedObjects)
	{
		FObjectRestore& Item = Plan.AddDefaulted_GetRef();
		Item.Image = Saved;
		Item.RelativePath = Saved->GetPathName(Image);
		if (Saved->GetOuter() == Image)
		{
			Item.CollectionMembership = Cast<UMaterialExpressionComment>(Saved)
				? FObjectRestore::ECollectionMembership::Comment
				: (Saved->IsA<UMaterialExpression>()
					? FObjectRestore::ECollectionMembership::Expression
					: FObjectRestore::ECollectionMembership::None);
		}
		if (Saved == Image)
		{
			Item.Target = Material;
			continue;
		}
		// UMaterialInterface owns exactly one editor-only data object and the
		// engine enforces its name from the live material name during PostLoad and
		// save. A transient checkpoint therefore has a different short name even
		// though it represents the same authored object; resolve it by ownership
		// identity instead of creating a second, unreferenced editor-data object.
		if (Saved == ImageEditorOnlyData)
		{
			Item.Target = MaterialEditorOnlyData;
			Claimed.Add(Item.Target);
			ResolvedObjects.Add(Saved, Item.Target);
			continue;
		}
		if (!ResolvedObjects.Contains(Saved->GetOuter()))
		{
			return FMCPToolResult::Error(TEXT("Checkpoint subobject has an unresolved authored outer."), TEXT("restore_preflight_failed"), 409);
		}
		UObject* ResolvedOuter = ResolvedObjects.FindRef(Saved->GetOuter());
		// If the outer itself is missing, its newly allocated identity cannot have
		// an existing child-name collision. Existing outers are checked at every
		// depth, including children of expressions that were renamed meanwhile.
		if (ResolvedOuter)
		{
			Item.Occupant = StaticFindObjectFast(UObject::StaticClass(), ResolvedOuter, Saved->GetFName());
			if (IsValid(Item.Occupant)) Item.Target = Item.Occupant;
		}
		if (!Item.Target)
		{
			if (const UMaterialExpression* SavedExpression = Cast<UMaterialExpression>(Saved))
			{
				for (UObject* Candidate : CurrentObjects)
				{
					UMaterialExpression* Expression = Cast<UMaterialExpression>(Candidate);
					if (Expression && Expression->GetClass() == Saved->GetClass()
						&& SavedExpression->MaterialExpressionGuid.IsValid()
						&& Expression->MaterialExpressionGuid == SavedExpression->MaterialExpressionGuid)
					{
						if (Item.Target)
						{
							return FMCPToolResult::Error(TEXT("Expression GUID is ambiguous."), TEXT("restore_preflight_failed"), 409);
						}
						Item.Target = Expression;
						Item.bRename = Expression->GetFName() != Saved->GetFName()
							|| Expression->GetOuter() != ResolvedOuter;
					}
				}
			}
		}
		if (Item.Target && !ResolvedOuter)
		{
			return FMCPToolResult::Error(TEXT("A live checkpoint child cannot be reparented into an outer that must first be recreated."), TEXT("restore_preflight_failed"), 409);
		}
		if (Item.Target && (Item.Target->GetClass() != Saved->GetClass() || Claimed.Contains(Item.Target)))
		{
			return FMCPToolResult::Error(TEXT("Checkpoint object identity conflicts with the current asset."), TEXT("restore_preflight_failed"), 409);
		}
		if (const UMaterialExpression* SavedExpression = Cast<UMaterialExpression>(Saved))
		{
			const UMaterialExpression* TargetExpression = Cast<UMaterialExpression>(Item.Target);
			if (TargetExpression && SavedExpression->MaterialExpressionGuid.IsValid()
				&& TargetExpression->MaterialExpressionGuid != SavedExpression->MaterialExpressionGuid)
			{
				return FMCPToolResult::Error(TEXT("A different expression occupies a checkpoint identity."), TEXT("restore_preflight_failed"), 409);
			}
		}
		if (Item.Target)
		{
			Claimed.Add(Item.Target);
		}
		if (Item.Occupant && !IsValid(Item.Occupant)
			&& !Item.Occupant->Rename(nullptr, GetTransientPackage(), REN_Test))
		{
			return FMCPToolResult::Error(TEXT("A deleted checkpoint name cannot be safely released."), TEXT("restore_preflight_failed"), 409);
		}
		if (Item.bRename && !Item.Target->Rename(*Saved->GetName(), ResolvedOuter, REN_Test))
		{
			return FMCPToolResult::Error(TEXT("A checkpoint node cannot recover its authored name."), TEXT("restore_preflight_failed"), 409);
		}
		ResolvedObjects.Add(Saved, Item.Target);
	}
	TSet<UObject*> RemovedRoots;
	for (UObject* Current : CurrentObjects)
	{
		if (Claimed.Contains(Current)) continue;
		if (Current->GetOuter() == Material
			&& (Current->IsA<UMaterialExpression>() || Current->IsA<UMaterialExpressionComment>()))
		{
			FOwnedSubtreeRemoval Removal;
			Removal.Object = Current;
			Removal.OriginalOuter = Current->GetOuter();
			Removal.OriginalName = Current->GetFName();
			Removal.RetiredName = MakeUniqueObjectName(GetTransientPackage(), Current->GetClass(), Current->GetFName());
			Removal.CollectionMembership = Cast<UMaterialExpressionComment>(Current)
				? FObjectRestore::ECollectionMembership::Comment
				: FObjectRestore::ECollectionMembership::Expression;
			for (UObject* OwnedObject : CurrentObjects)
			{
				if (OwnedObject == Current || (OwnedObject->IsIn(Current) && !Claimed.Contains(OwnedObject)))
				{
					Removal.OwnedObjects.Add(OwnedObject);
				}
			}
			if (!Current->Rename(*Removal.RetiredName.ToString(), GetTransientPackage(), REN_Test))
			{
				return FMCPToolResult::Error(TEXT("An extra material expression cannot be safely retired."),
					TEXT("restore_preflight_failed"), 409);
			}
			RemovedSubtrees.Add(MoveTemp(Removal));
			RemovedRoots.Add(Current);
			continue;
		}
		bool bRetainedExpressionAncestor = false;
		bool bAncestorRemoved = false;
		for (UObject* Outer = Current->GetOuter(); Outer && Outer != Material; Outer = Outer->GetOuter())
		{
			bAncestorRemoved |= RemovedRoots.Contains(Outer);
			bRetainedExpressionAncestor |= Outer->IsA<UMaterialExpression>() && Claimed.Contains(Outer);
		}
		// Entire expressions removed from ExpressionCollection are excluded by the
		// authored traversal already. Only retire extra children of retained nodes,
		// and move their descendants together to preserve the subtree's identity.
		if (!bRetainedExpressionAncestor || bAncestorRemoved) continue;
		FOwnedSubtreeRemoval Removal;
		Removal.Object = Current;
		Removal.OriginalOuter = Current->GetOuter();
		Removal.OriginalName = Current->GetFName();
		Removal.RetiredName = MakeUniqueObjectName(GetTransientPackage(), Current->GetClass(), Current->GetFName());
		for (UObject* OwnedObject : CurrentObjects)
		{
			if (OwnedObject == Current || (OwnedObject->IsIn(Current) && !Claimed.Contains(OwnedObject)))
			{
				Removal.OwnedObjects.Add(OwnedObject);
			}
		}
		if (!Current->IsIn(Material)
			|| !Current->Rename(*Removal.RetiredName.ToString(), GetTransientPackage(), REN_Test))
		{
			return FMCPToolResult::Error(TEXT("An extra authored subtree cannot be safely retired."),
				TEXT("restore_preflight_failed"), 409);
		}
		RemovedSubtrees.Add(Removal);
		RemovedRoots.Add(Current);
	}
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

bool RestoreRemovedSubtrees(UMaterial* Material, const TArray<FOwnedSubtreeRemoval>& RemovedSubtrees)
{
	if (Material && RemovedSubtrees.Num() > 0)
	{
		Material->SetFlags(RF_Transactional);
		Material->Modify();
		if (UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData())
		{
			EditorOnlyData->SetFlags(RF_Transactional);
			EditorOnlyData->Modify();
		}
	}
	for (const FOwnedSubtreeRemoval& Removal : RemovedSubtrees)
	{
		if (!IsValid(Removal.Object) || !IsValid(Removal.OriginalOuter)) return false;
		if (Removal.Object->GetOuter() != Removal.OriginalOuter
			|| Removal.Object->GetFName() != Removal.OriginalName)
		{
			if (Removal.Object->GetOuter() != GetTransientPackage()
				|| Removal.Object->GetFName() != Removal.RetiredName
				|| !Removal.Object->Rename(*Removal.OriginalName.ToString(), Removal.OriginalOuter, REN_Test)
				|| !Removal.Object->Rename(*Removal.OriginalName.ToString(), Removal.OriginalOuter,
					REN_DontCreateRedirectors | REN_DoNotDirty)) return false;
		}
		if (Material && Removal.CollectionMembership != FObjectRestore::ECollectionMembership::None)
		{
			FMaterialExpressionCollection& Collection = Material->GetExpressionCollection();
			if (Removal.CollectionMembership == FObjectRestore::ECollectionMembership::Comment)
			{
				Collection.AddComment(CastChecked<UMaterialExpressionComment>(Removal.Object));
			}
			else
			{
				Collection.AddExpression(CastChecked<UMaterialExpression>(Removal.Object));
			}
		}
	}
	return true;
}

bool ApplyRestorePlan(
	UMaterial* Material, UMaterial* Image, TArray<FObjectRestore>& Plan,
	const TArray<FOwnedSubtreeRemoval>& RemovedSubtrees)
{
	Material->SetFlags(RF_Transactional);
	Material->Modify();
	if (UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData())
	{
		EditorOnlyData->SetFlags(RF_Transactional);
		EditorOnlyData->Modify();
	}
	TMap<UObject*, UObject*> Replacements;
	Replacements.Add(Image, Material);
	for (FObjectRestore& Item : Plan)
	{
		if (Item.Image == Image)
		{
			continue;
		}
		UObject* Outer = Replacements.FindRef(Item.Image->GetOuter());
		if (!Outer)
		{
			return false;
		}
		if (IsValid(Item.Target) && Item.bRename)
		{
			Item.Target->SetFlags(RF_Transactional);
			Item.Target->Modify();
		}
		if (!IsValid(Item.Target))
		{
			if (Item.Occupant && !Item.Occupant->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
			{
				return false;
			}
			Item.Target = NewObject<UObject>(Outer, Item.Image->GetClass(), Item.Image->GetFName(), RF_Transactional);
			if (Item.CollectionMembership != FObjectRestore::ECollectionMembership::None
				&& Outer == Material)
			{
				FMaterialExpressionCollection& Collection = Material->GetExpressionCollection();
				if (Item.CollectionMembership == FObjectRestore::ECollectionMembership::Comment)
				{
					Collection.AddComment(CastChecked<UMaterialExpressionComment>(Item.Target));
				}
				else
				{
					Collection.AddExpression(CastChecked<UMaterialExpression>(Item.Target));
				}
			}
		}
		else if (Item.bRename && !Item.Target->Rename(*Item.Image->GetName(), Outer, REN_DontCreateRedirectors | REN_DoNotDirty))
		{
			return false;
		}
		if (!Item.Target)
		{
			return false;
		}
		Replacements.Add(Item.Image, Item.Target);
	}
	for (const FOwnedSubtreeRemoval& Removal : RemovedSubtrees)
	{
		if (!IsValid(Removal.Object) || Removal.Object->GetOuter() != Removal.OriginalOuter
			|| Removal.Object->GetFName() != Removal.OriginalName) return false;
		// GC does not follow Outer in reverse. Record every authored descendant,
		// including objects with no reflected incoming reference, so Undo/Redo
		// keeps the complete retired subtree alive after garbage collection.
		for (UObject* OwnedObject : Removal.OwnedObjects)
		{
			OwnedObject->SetFlags(RF_Transactional);
			OwnedObject->Modify();
		}
		if (Removal.CollectionMembership != FObjectRestore::ECollectionMembership::None)
		{
			FMaterialExpressionCollection& Collection = Material->GetExpressionCollection();
			Material->Modify();
			if (UMaterialEditorOnlyData* EditorOnlyData = Material->GetEditorOnlyData())
			{
				EditorOnlyData->Modify();
			}
			if (Removal.CollectionMembership == FObjectRestore::ECollectionMembership::Comment)
			{
				Collection.RemoveComment(CastChecked<UMaterialExpressionComment>(Removal.Object));
			}
			else
			{
				Collection.RemoveExpression(CastChecked<UMaterialExpression>(Removal.Object));
			}
		}
		if (!Removal.Object->Rename(*Removal.RetiredName.ToString(), GetTransientPackage(),
			REN_DontCreateRedirectors | REN_DoNotDirty)) return false;
	}
	for (const FObjectRestore& Item : Plan)
	{
		Item.Target->SetFlags(RF_Transactional);
	}
	for (const FObjectRestore& Item : Plan)
	{
		// Leave already identical objects out of Undo. In particular, an
		// unchanged nested texture must not run its own PostEditUndo/resource
		// rebuild merely because its parent material was restored.
		const bool bIsCheckpointRoot = Item.Image == Image || Item.Target == Material;
		if (!bIsCheckpointRoot
			&& AuthoredObjectRecord(Item.Target, Material, Material->GetPathName())
			== AuthoredObjectRecord(Item.Image, Image, Material->GetPathName()))
		{
			continue;
		}
		Item.Target->Modify();
		for (TFieldIterator<FProperty> It(Item.Image->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (IsAuthoredProperty(Property))
			{
				CopyAuthoredProperty(Property, Item.Target, Item.Image);
			}
		}
	}
	// Rewrite every reference between copied authored objects. References to
	// external texture/function assets remain the original external identities.
	for (const FObjectRestore& Item : Plan)
	{
		FArchiveReplaceObjectRef<UObject> Remap(Item.Target, Replacements,
			EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
	}
	if (Material->MaterialGraph)
	{
		Material->MaterialGraph->RebuildGraph();
		Material->MaterialGraph->NotifyGraphChanged();
	}
	// Reapply authored fields on the live root after graph rebuild and callbacks.
	// The final readback must reflect the checkpoint image, even if graph refresh
	// or a notification listener has changed root material settings.
	for (const FObjectRestore& Item : Plan)
	{
		if (Item.Image != Image || Item.Target != Material)
		{
			continue;
		}
		for (TFieldIterator<FProperty> It(Material->GetClass()); It; ++It)
		{
			if (IsAuthoredProperty(*It))
			{
				CopyAuthoredProperty(*It, Material, Image);
			}
		}
		FArchiveReplaceObjectRef<UObject> Remap(Material, Replacements,
			EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);
		break;
	}
	return true;
}
}

FMCPToolResult Capture(UMaterial* Material, const FString& SnapshotId, FString& OutDigest)
{
	OutDigest = AuthoredDigest(Material, Material->GetPathName());
	if (OutDigest.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("Cannot hash authored material properties."), TEXT("state_digest_unavailable"), 500);
	}
	auto Checkpoint = MakeShared<FCheckpoint>();
	Checkpoint->Image.Reset(DuplicateAuthoredImage(Material, TEXT("UEAIAuthoredCheckpoint"), Checkpoint->OwnedObjects));
	Checkpoint->AssetPath = Material->GetPathName();
	Checkpoint->Digest = OutDigest;
	Checkpoint->CapturedAt = FPlatformTime::Seconds();
	if (!Checkpoint->Image.IsValid())
	{
		return FMCPToolResult::Error(TEXT("The authored material checkpoint failed independent readback (image_null)."), TEXT("checkpoint_capture_failed"), 500);
	}
	const FString ImageDigest = AuthoredDigest(Checkpoint->Image.Get(), Checkpoint->AssetPath);
	if (ImageDigest != OutDigest)
	{
		const FString Detail = AuthoredDigestMismatch(Material, Checkpoint->Image.Get(), Checkpoint->AssetPath);
		return FMCPToolResult::Error(
			FString::Printf(TEXT("The authored material checkpoint failed independent readback (%s;orig_digest=%s;image_digest=%s)."),
				*Detail, *OutDigest, *ImageDigest),
			TEXT("checkpoint_capture_failed"), 500);
	}
	// Retain a bounded number of full images; stale callers receive an explicit
	// unavailable checkpoint error instead of silently falling back to topology.
	if (Checkpoints().Num() >= 32)
	{
		FString OldestId;
		double OldestTime = TNumericLimits<double>::Max();
		for (const auto& Pair : Checkpoints())
		{
			if (Pair.Value->CapturedAt < OldestTime)
			{
				OldestId = Pair.Key;
				OldestTime = Pair.Value->CapturedAt;
			}
		}
		Checkpoints().Remove(OldestId);
	}
	Checkpoints().Add(SnapshotId, Checkpoint);
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

bool Contains(const FString& SnapshotId)
{
	return Checkpoints().Contains(SnapshotId);
}

void Release(const FString& SnapshotId)
{
	Checkpoints().Remove(SnapshotId);
}

bool GetDigests(UMaterial* Material, const FString& SnapshotId, FString& OutSaved, FString& OutCurrent)
{
	const TSharedPtr<FCheckpoint> Checkpoint = Checkpoints().FindRef(SnapshotId);
	if (!Checkpoint || !Checkpoint->Image.IsValid() || Checkpoint->AssetPath != Material->GetPathName())
	{
		return false;
	}
	OutSaved = Checkpoint->Digest;
	OutCurrent = AuthoredDigest(Material, Checkpoint->AssetPath);
	return !OutCurrent.IsEmpty();
}

FMCPToolResult Restore(UMaterial* Material, const FString& SnapshotId, const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FCheckpoint> Checkpoint = Checkpoints().FindRef(SnapshotId);
	if (!Checkpoint || !Checkpoint->Image.IsValid())
	{
		return FMCPToolResult::Error(TEXT("The authored checkpoint is unavailable in this Editor process."), TEXT("authored_checkpoint_unavailable"), 410);
	}
	FString SavedDigest, CurrentDigest;
	if (!GetDigests(Material, SnapshotId, SavedDigest, CurrentDigest))
	{
		return FMCPToolResult::Error(TEXT("Checkpoint asset identity does not match."), TEXT("snapshot_asset_mismatch"), 409);
	}
	if (CurrentDigest != Params->GetStringField(TEXT("expectedCurrentDigest")))
	{
		return FMCPToolResult::Error(TEXT("Authored material state changed after it was read."), TEXT("material_graph_state_conflict"), 409);
	}
	bool bDryRun = false, bConfirmed = false, bSharedConfirmed = false;
	Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
	Params->TryGetBoolField(TEXT("confirmFullGraphRestore"), bConfirmed);
	Params->TryGetBoolField(TEXT("confirmSharedNodeImpact"), bSharedConfirmed);
	TArray<FObjectRestore> Plan;
	TArray<FOwnedSubtreeRemoval> RemovedSubtrees;
	const FMCPToolResult Preflight = BuildRestorePlan(Material, Checkpoint->Image.Get(), Plan, RemovedSubtrees);
	if (!Preflight.bSuccess)
	{
		return Preflight;
	}
	TArray<TSharedPtr<FJsonValue>> SharedNodes;
	TSet<FString> SharedNodeIds;
	for (UMaterial* InspectedMaterial : {Material, Checkpoint->Image.Get()})
	{
		// The authored image can contain nested UMaterialExpression objects owned
		// by a top-level node. They are included in the checkpoint and may have
		// consumers outside that owner's subtree, so shared-write confirmation must
		// inspect the complete authored object set rather than only the collection
		// roots returned by GetExpressions().
		TArray<UObject*> AuthoredObjects;
		GatherAuthoredObjects(InspectedMaterial, AuthoredObjects);
		for (UObject* AuthoredObject : AuthoredObjects)
		{
			UMaterialExpression* Source = Cast<UMaterialExpression>(AuthoredObject);
			if (!Source) continue;
			UEAIIntegration::MaterialEditing::FMaterialSharedWriteProof Proof;
			UEAIIntegration::MaterialEditing::InspectMaterialExpressionConsumers(InspectedMaterial, Source, Proof);
			if (Proof.bShared)
			{
				SharedNodeIds.Add(MCPMaterialInfrastructure::ExpressionNodeId(Source));
			}
		}
	}
	for (const FString& SharedId : SharedNodeIds)
	{
		SharedNodes.Add(MakeShared<FJsonValueString>(SharedId));
	}
	const bool bChanges = CurrentDigest != SavedDigest;
	if (!bDryRun && bChanges && !bConfirmed)
	{
		return FMCPToolResult::Error(TEXT("Authored restoration requires confirmFullGraphRestore=true."), TEXT("full_graph_restore_confirmation_required"), 409);
	}
	if (!bDryRun && bChanges && SharedNodes.Num() > 0 && !bSharedConfirmed)
	{
		return FMCPToolResult::Error(TEXT("Authored restoration affects shared expressions; confirmSharedNodeImpact=true is required."), TEXT("shared_node_impact_confirmation_required"), 409);
	}
	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("snapshotId"), SnapshotId);
	Result->SetStringField(TEXT("assetPath"), Checkpoint->AssetPath);
	Result->SetStringField(TEXT("projectionIdentity"), ProjectionIdentity);
	Result->SetStringField(TEXT("restoreSemantics"), TEXT("authoredGraph"));
	Result->SetBoolField(TEXT("fullGraphRestore"), true);
	Result->SetBoolField(TEXT("dryRun"), bDryRun);
	Result->SetBoolField(TEXT("sharedNodeImpactDetected"), SharedNodes.Num() > 0);
	Result->SetBoolField(TEXT("sharedNodeImpactConfirmed"), bSharedConfirmed);
	Result->SetArrayField(TEXT("sharedNodeIds"), SharedNodes);
	Result->SetStringField(TEXT("beforeStateDigest"), CurrentDigest);
	Result->SetStringField(TEXT("snapshotStateDigest"), SavedDigest);
	Result->SetStringField(TEXT("afterStateDigest"), CurrentDigest);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
	Result->SetBoolField(TEXT("compileRequested"), false);
	Result->SetBoolField(TEXT("compileVerified"), false);
	Result->SetBoolField(TEXT("preflightVerified"), true);
	Result->SetBoolField(TEXT("postconditionVerified"), !bChanges);
	Result->SetBoolField(TEXT("undoRetained"), false);
	Result->SetStringField(TEXT("attempt_status"), bDryRun ? TEXT("dry_run") : TEXT("not_attempted"));
	Result->SetStringField(TEXT("restore_status"), bDryRun ? TEXT("not_attempted") : TEXT("restored"));
	if (bDryRun || !bChanges)
	{
		return FMCPToolResult::Ok(Result);
	}
	TArray<TStrongObjectPtr<UObject>> BeforeOwnedObjects;
	TStrongObjectPtr<UMaterial> Before(DuplicateAuthoredImage(Material, TEXT("UEAIRestoreBefore"), BeforeOwnedObjects));
	if (!Before.IsValid() || AuthoredDigest(Before.Get(), Checkpoint->AssetPath) != CurrentDigest)
	{
		return FMCPToolResult::Error(TEXT("Cannot verify the before-image; no material was changed."), TEXT("restore_preflight_failed"), 409);
	}
	const bool bWasDirty = Material->GetOutermost()->IsDirty();
	FScopedTransaction Transaction(TEXT("UEAI.MaterialGraph.Restore"),
		NSLOCTEXT("UEAI", "RestoreAuthoredMaterialGraph", "Restore authored material graph"), Material);
	const bool bApplied = ApplyRestorePlan(Material, Checkpoint->Image.Get(), Plan, RemovedSubtrees);
	const FString AfterDigest = AuthoredDigest(Material, Checkpoint->AssetPath);
	FString ExpectedAfter = SavedDigest;
	Params->TryGetStringField(TEXT("expectedAfterDigest"), ExpectedAfter);
	if (!bApplied || AfterDigest != SavedDigest || AfterDigest != ExpectedAfter)
	{
		// Capture the mismatch while the failed restore state is still present.
		// Computing this only after rollback describes the rollback state instead
		// of the original postcondition failure and can hide the object/property
		// that made the restore diverge.
		const FString AttemptReadbackDetail = AuthoredDigestMismatch(
			Checkpoint->Image.Get(), Material, Checkpoint->AssetPath);
		TArray<FObjectRestore> RollbackPlan;
		TArray<FOwnedSubtreeRemoval> RollbackRemovedSubtrees;
		const bool bRollbackRemovedSubtrees = RestoreRemovedSubtrees(Material, RemovedSubtrees);
		bool bRollbackPlanBuilt = false;
		if (bRollbackRemovedSubtrees)
		{
			bRollbackPlanBuilt = BuildRestorePlan(
				Material, Before.Get(), RollbackPlan, RollbackRemovedSubtrees).bSuccess;
		}
		const bool bRollbackApplied = bRollbackPlanBuilt
			&& ApplyRestorePlan(Material, Before.Get(), RollbackPlan, RollbackRemovedSubtrees);
		const FString RollbackDigest = AuthoredDigest(Material, Checkpoint->AssetPath);
		const bool bRollbackDigestMatches = RollbackDigest == CurrentDigest;
		const bool bRollback = bRollbackRemovedSubtrees && bRollbackPlanBuilt
			&& bRollbackApplied && bRollbackDigestMatches;
		if (bRollback)
		{
			Material->GetOutermost()->SetDirtyFlag(bWasDirty);
			Transaction.Cancel();
		}
		const FString RollbackReadbackDetail = AuthoredDigestMismatch(
			Before.Get(), Material, Checkpoint->AssetPath);
		FMCPToolResult Failure = FMCPToolResult::Error(
			FString::Printf(TEXT("Authored material restore failed its readback (saved=%s;after=%s;attempt_detail=%s;rollback_detail=%s;rollback_removed=%s;rollback_plan=%s;rollback_apply=%s;rollback_digest=%s)."),
				*SavedDigest, *AfterDigest, *AttemptReadbackDetail, *RollbackReadbackDetail,
				bRollbackRemovedSubtrees ? TEXT("true") : TEXT("false"),
				bRollbackPlanBuilt ? TEXT("true") : TEXT("false"),
				bRollbackApplied ? TEXT("true") : TEXT("false"),
				bRollbackDigestMatches ? TEXT("true") : TEXT("false")),
			bRollback ? TEXT("restore_postcondition_failed") : TEXT("restore_rollback_failed"), 500);
		Failure.Data = Result;
		Result->SetStringField(TEXT("attempt_status"), bRollback ? TEXT("rolled_back") : TEXT("failed"));
		Result->SetStringField(TEXT("restore_status"), bRollback ? TEXT("restored") : TEXT("restore_failed"));
		Result->SetBoolField(TEXT("rollbackVerified"), bRollback);
		Result->SetBoolField(TEXT("rollbackSubtreesRestored"), bRollbackRemovedSubtrees);
		Result->SetBoolField(TEXT("rollbackPlanBuilt"), bRollbackPlanBuilt);
		Result->SetBoolField(TEXT("rollbackApplied"), bRollbackApplied);
		Result->SetBoolField(TEXT("rollbackDigestMatches"), bRollbackDigestMatches);
		Result->SetStringField(TEXT("rollbackDigest"), RollbackDigest);
		Result->SetBoolField(TEXT("undoRetained"), !bRollback && Transaction.IsOutstanding());
		Result->SetStringField(TEXT("afterStateDigest"), AuthoredDigest(Material, Checkpoint->AssetPath));
		if (!bRollback) UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Material);
		return Failure;
	}
	UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Material);
	Material->MarkPackageDirty();
	Result->SetStringField(TEXT("afterStateDigest"), AfterDigest);
	Result->SetBoolField(TEXT("postconditionVerified"), true);
	Result->SetBoolField(TEXT("undoRetained"), Transaction.IsOutstanding());
	Result->SetStringField(TEXT("attempt_status"), TEXT("applied"));
	return FMCPToolResult::Ok(Result);
}
}
