// Component Tools — list, add, remove components in Blueprint SCS
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/BlueprintMutationGuard.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/Sha256.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr int32 MaxComponentPropertiesPerPage = 200;
	constexpr int32 MaxComponentPropertyValueChars = 4096;
	constexpr int32 MaxBlueprintPathChars = 1024;
	constexpr int32 MaxComponentNameChars = 256;
	constexpr int32 MaxComponentNodeIdChars = 64;
	constexpr int32 MaxComponentPropertyNameChars = 256;
	constexpr int32 MaxComponentPropertyFilterChars = 256;
	constexpr int32 MaxSerializedComponentPropertyValueChars = 65536;
	constexpr int32 Sha256StateHashChars = 71;
	constexpr int32 MaxComponentStateHashProperties = 2048;
	constexpr int32 MaxComponentStateHashValueChars = 65536;
	constexpr int32 MaxComponentStateHashTotalValueChars = 1048576;
	constexpr int32 MaxComponentReceiptValueChars = 4096;
	constexpr int32 MaxComponentNodeSlots = 5000;

	struct FComponentStateHashCoverage
	{
		int32 PropertyTotal = 0;
		int32 PropertyHashed = 0;
		int64 ValueCharsHashed = 0;
		bool bComplete = false;
		FString UnavailableReason;
	};

	bool IsBoundedComponentString(
		const FString& Value,
		const int32 Maximum,
		const bool bAllowEmpty = false)
	{
		return (bAllowEmpty || !Value.IsEmpty()) && Value.Len() <= Maximum;
	}

	bool IsValidSha256StateHash(const FString& Value)
	{
		if (Value.Len() != Sha256StateHashChars
			|| !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Value[Index]))
			{
				return false;
			}
		}
		return true;
	}

	bool ReadComponentPageInteger(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		const int32 DefaultValue,
		const int32 Minimum,
		const int32 Maximum,
		int32& OutValue)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Field))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(Field, Number)
			|| !FMath::IsFinite(Number)
			|| Number != FMath::FloorToDouble(Number)
			|| Number < Minimum || Number > Maximum)
		{
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	USCS_Node* FindLocalComponentNode(
		UBlueprint* Blueprint,
		const FString& ComponentName,
		const FString& ComponentNodeId,
		FString& OutError,
		FString& OutErrorCode)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			OutError = TEXT("This Blueprint has no Simple Construction Script.");
			OutErrorCode = TEXT("component_scs_unavailable");
			return nullptr;
		}
		const TArray<USCS_Node*>& AllNodes =
			Blueprint->SimpleConstructionScript->GetAllNodes();
		if (AllNodes.Num() > MaxComponentNodeSlots)
		{
			OutError = FString::Printf(
				TEXT("Blueprint SCS contains %d node slots, exceeding the component "
					"selector safety limit of %d."),
				AllNodes.Num(),
				MaxComponentNodeSlots);
			OutErrorCode = TEXT("component_scan_limit_exceeded");
			return nullptr;
		}

		FGuid RequestedGuid;
		const bool bUseGuid = !ComponentNodeId.IsEmpty();
		if (bUseGuid
			&& (!FGuid::Parse(ComponentNodeId, RequestedGuid)
				|| !RequestedGuid.IsValid()))
		{
			OutError = TEXT("componentNodeId must be a nonzero SCS VariableGuid.");
			OutErrorCode = TEXT("invalid_params");
			return nullptr;
		}

		USCS_Node* Match = nullptr;
		for (USCS_Node* Node : AllNodes)
		{
			if (!Node)
			{
				continue;
			}
			const bool bGuidMatches = !bUseGuid || Node->VariableGuid == RequestedGuid;
			const bool bNameMatches = ComponentName.IsEmpty()
				|| Node->GetVariableName().ToString().Equals(
					ComponentName,
					ESearchCase::IgnoreCase);
			if (!bGuidMatches || !bNameMatches)
			{
				continue;
			}
			if (Match)
			{
				OutError = TEXT("The component selector is ambiguous.");
				OutErrorCode = TEXT("component_ambiguous");
				return nullptr;
			}
			Match = Node;
		}
		if (!Match)
		{
			OutError = TEXT(
				"No local SCS component matched the supplied name/node identity. "
				"Inherited native or parent-Blueprint components are read-only here.");
			OutErrorCode = TEXT("component_not_found");
		}
		return Match;
	}

	UActorComponent* ResolveLocalComponentTemplate(
		UBlueprint* Blueprint,
		USCS_Node* Node)
	{
		if (!Blueprint || !Node)
		{
			return nullptr;
		}
		if (UBlueprintGeneratedClass* GeneratedClass =
			Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
		{
			if (UActorComponent* Actual =
				Node->GetActualComponentTemplate(GeneratedClass))
			{
				return Actual;
			}
		}
		return Node->ComponentTemplate;
	}

	bool IsReadableComponentProperty(const FProperty* Property)
	{
		return Property
			&& Property->GetOwnerClass() != UObject::StaticClass()
			&& Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
			&& !Property->HasAnyPropertyFlags(
				CPF_Transient | CPF_DuplicateTransient
				| CPF_NonPIEDuplicateTransient | CPF_Deprecated);
	}

	bool IsWritableComponentProperty(const FProperty* Property)
	{
		return IsReadableComponentProperty(Property)
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_EditConst);
	}

	bool ExportComponentPropertyValue(
		UActorComponent* ComponentTemplate,
		const FName PropertyName,
		FString& OutValue)
	{
		if (!ComponentTemplate)
		{
			return false;
		}
		FProperty* Property = ComponentTemplate->GetClass()->FindPropertyByName(
			PropertyName);
		if (!Property)
		{
			return false;
		}
		void* Address = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
		OutValue.Reset();
		Property->ExportText_Direct(
			OutValue,
			Address,
			Address,
			ComponentTemplate,
			PPF_None);
		return true;
	}

	class FScopedComponentPropertyValue
	{
	public:
		explicit FScopedComponentPropertyValue(FProperty* InProperty)
			: Property(InProperty)
		{
			if (Property)
			{
				Value = FMemory::Malloc(
					Property->GetSize(),
					Property->GetMinAlignment());
				Property->InitializeValue(Value);
			}
		}

		~FScopedComponentPropertyValue()
		{
			if (Property && Value)
			{
				Property->DestroyValue(Value);
				FMemory::Free(Value);
			}
		}

		FScopedComponentPropertyValue(
			const FScopedComponentPropertyValue&) = delete;
		FScopedComponentPropertyValue& operator=(
			const FScopedComponentPropertyValue&) = delete;

		void* Get() const
		{
			return Value;
		}

	private:
		FProperty* Property = nullptr;
		void* Value = nullptr;
	};

	bool NormalizeComponentPropertyValue(
		UActorComponent* ComponentTemplate,
		FProperty* Property,
		const FString& SerializedValue,
		FString& OutNormalizedValue)
	{
		if (!ComponentTemplate || !Property)
		{
			return false;
		}
		FScopedComponentPropertyValue ScratchValue(Property);
		if (!ScratchValue.Get())
		{
			return false;
		}
		const TCHAR* ImportEnd = Property->ImportText_Direct(
			*SerializedValue,
			ScratchValue.Get(),
			ComponentTemplate,
			PPF_None);
		if (!ImportEnd || !FString(ImportEnd).TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		OutNormalizedValue.Reset();
		Property->ExportText_Direct(
			OutNormalizedValue,
			ScratchValue.Get(),
			ScratchValue.Get(),
			ComponentTemplate,
			PPF_None);
		return true;
	}

	bool ImportComponentPropertyValue(
		UActorComponent* ComponentTemplate,
		const FName PropertyName,
		const FString& SerializedValue,
		FString& OutNormalizedValue)
	{
		if (!ComponentTemplate)
		{
			return false;
		}
		FProperty* Property = ComponentTemplate->GetClass()->FindPropertyByName(
			PropertyName);
		if (!Property)
		{
			return false;
		}
		FScopedComponentPropertyValue ScratchValue(Property);
		if (!ScratchValue.Get())
		{
			return false;
		}
		const TCHAR* ImportEnd = Property->ImportText_Direct(
			*SerializedValue,
			ScratchValue.Get(),
			ComponentTemplate,
			PPF_None);
		if (!ImportEnd || !FString(ImportEnd).TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		void* Address = Property->ContainerPtrToValuePtr<void>(ComponentTemplate);
		ComponentTemplate->SetFlags(RF_Transactional);
		ComponentTemplate->Modify();
		FEditPropertyChain PropertyChain;
		PropertyChain.AddHead(Property);
		PropertyChain.SetActivePropertyNode(Property);
		static_cast<UObject*>(ComponentTemplate)->PreEditChange(PropertyChain);
		Property->CopyCompleteValue(Address, ScratchValue.Get());
		FPropertyChangedEvent PropertyEvent(
			Property,
			EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChainEvent(PropertyChain, PropertyEvent);
		static_cast<UObject*>(ComponentTemplate)->PostEditChangeChainProperty(
			ChainEvent);
		if (!ExportComponentPropertyValue(
			ComponentTemplate,
			PropertyName,
			OutNormalizedValue))
		{
			return false;
		}
		FString ExpectedNormalizedValue;
		Property->ExportText_Direct(
			ExpectedNormalizedValue,
			ScratchValue.Get(),
			ScratchValue.Get(),
			ComponentTemplate,
			PPF_None);
		return OutNormalizedValue == ExpectedNormalizedValue;
	}

	bool TryHashComponentText(const FString& Value, FString& OutHash)
	{
		const FTCHARToUTF8 Utf8(*Value);
		FString Hash;
		if (!UEAIIntegration::Infrastructure::TrySha256Hex(
			Utf8.Get(),
			static_cast<uint64>(Utf8.Length()),
			Hash))
		{
			return false;
		}
		OutHash = TEXT("sha256:") + Hash;
		return true;
	}

	TSharedRef<FJsonObject> SerializeComponentHashCoverage(
		const FComponentStateHashCoverage& Coverage)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("complete"), Coverage.bComplete);
		Result->SetNumberField(TEXT("propertyTotal"), Coverage.PropertyTotal);
		Result->SetNumberField(TEXT("propertyHashed"), Coverage.PropertyHashed);
		Result->SetNumberField(
			TEXT("valueCharsHashed"),
			static_cast<double>(Coverage.ValueCharsHashed));
		Result->SetNumberField(
			TEXT("maxProperties"), MaxComponentStateHashProperties);
		Result->SetNumberField(
			TEXT("maxValueCharsPerProperty"),
			MaxComponentStateHashValueChars);
		Result->SetNumberField(
			TEXT("maxTotalValueChars"),
			MaxComponentStateHashTotalValueChars);
		if (!Coverage.UnavailableReason.IsEmpty())
		{
			Result->SetStringField(
				TEXT("unavailableReason"), Coverage.UnavailableReason);
		}
		return Result;
	}

	FString ComputeComponentStateHash(
		UBlueprint* Blueprint,
		USCS_Node* Node,
		UActorComponent* ComponentTemplate,
		FComponentStateHashCoverage& OutCoverage)
	{
		OutCoverage = FComponentStateHashCoverage();
		if (!Blueprint || !Blueprint->SimpleConstructionScript
			|| !Node || !ComponentTemplate)
		{
			OutCoverage.UnavailableReason = TEXT("component_identity_unavailable");
			return FString();
		}
		USCS_Node* Parent =
			Blueprint->SimpleConstructionScript->FindParentNode(Node);
		const FString Identity = FString::Printf(
			TEXT("blueprint=%s\nnode=%s\nname=%s\nclass=%s\ntemplate=%s\n")
			TEXT("parent=%s\nparentNode=%s\nparentOwner=%s\nparentNative=%d\n")
			TEXT("socket=%s\n"),
			*Blueprint->GetPathName(),
			*Node->VariableGuid.ToString(),
			*Node->GetVariableName().ToString(),
			*ComponentTemplate->GetClass()->GetPathName(),
			*ComponentTemplate->GetPathName(),
			*Node->ParentComponentOrVariableName.ToString(),
			Parent ? *Parent->VariableGuid.ToString() : TEXT(""),
			*Node->ParentComponentOwnerClassName.ToString(),
			Node->bIsParentComponentNative ? 1 : 0,
			*Node->AttachToName.ToString());
		FString IdentityHash;
		if (Identity.Len() > 16384
			|| !TryHashComponentText(Identity, IdentityHash))
		{
			OutCoverage.UnavailableReason = TEXT("component_identity_too_large");
			return FString();
		}
		TArray<FProperty*> Properties;
		for (TFieldIterator<FProperty> It(
			     ComponentTemplate->GetClass(),
			     EFieldIteratorFlags::IncludeSuper,
			     EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
		{
			if (IsReadableComponentProperty(*It))
			{
				++OutCoverage.PropertyTotal;
				if (OutCoverage.PropertyTotal
					> MaxComponentStateHashProperties)
				{
					OutCoverage.UnavailableReason =
						TEXT("property_count_limit_exceeded");
					return FString();
				}
				Properties.Add(*It);
			}
		}
		Properties.Sort([](const FProperty& Left, const FProperty& Right)
		{
			return Left.GetName() < Right.GetName();
		});
		FString State = IdentityHash + TEXT("\n");
		for (const FProperty* Property : Properties)
		{
			FString Value;
			if (!ExportComponentPropertyValue(
				ComponentTemplate,
				Property->GetFName(),
				Value))
			{
				OutCoverage.UnavailableReason = TEXT("property_export_failed");
				return FString();
			}
			if (Value.Len() > MaxComponentStateHashValueChars)
			{
				OutCoverage.UnavailableReason =
					TEXT("property_value_limit_exceeded");
				return FString();
			}
			OutCoverage.ValueCharsHashed += Value.Len();
			if (OutCoverage.ValueCharsHashed
				> MaxComponentStateHashTotalValueChars)
			{
				OutCoverage.UnavailableReason =
					TEXT("total_property_value_limit_exceeded");
				return FString();
			}
			FString DescriptorHash;
			FString ValueHash;
			if (!TryHashComponentText(
					Property->GetName() + TEXT("|") + Property->GetCPPType(),
					DescriptorHash)
				|| !TryHashComponentText(Value, ValueHash))
			{
				OutCoverage.UnavailableReason = TEXT("property_hash_failed");
				return FString();
			}
			State += DescriptorHash + TEXT("|") + ValueHash + TEXT("\n");
			++OutCoverage.PropertyHashed;
		}
		FString StateHash;
		if (!TryHashComponentText(State, StateHash))
		{
			OutCoverage.UnavailableReason = TEXT("state_hash_failed");
			return FString();
		}
		OutCoverage.bComplete = true;
		return StateHash;
	}

	void SetBoundedComponentValueReceipt(
		const TSharedRef<FJsonObject>& Result,
		const TCHAR* Prefix,
		const FString& Value)
	{
		FString Digest;
		TryHashComponentText(Value, Digest);
		FString BoundedValue = Value;
		const bool bTruncated =
			BoundedValue.Len() > MaxComponentReceiptValueChars;
		if (bTruncated)
		{
			BoundedValue.LeftInline(MaxComponentReceiptValueChars, false);
		}
		const FString PrefixString(Prefix);
		Result->SetStringField(PrefixString + TEXT("Value"), BoundedValue);
		Result->SetNumberField(PrefixString + TEXT("ValueLength"), Value.Len());
		Result->SetBoolField(PrefixString + TEXT("ValueTruncated"), bTruncated);
		if (!Digest.IsEmpty())
		{
			Result->SetStringField(PrefixString + TEXT("ValueHash"), Digest);
		}
	}

	TSharedRef<FJsonObject> SerializeComponentTransform(
		const USceneComponent* SceneComponent)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!SceneComponent)
		{
			return Result;
		}
		const FVector Location = SceneComponent->GetRelativeLocation();
		const FRotator Rotation = SceneComponent->GetRelativeRotation();
		const FVector Scale = SceneComponent->GetRelativeScale3D();
		TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
		LocationJson->SetNumberField(TEXT("x"), Location.X);
		LocationJson->SetNumberField(TEXT("y"), Location.Y);
		LocationJson->SetNumberField(TEXT("z"), Location.Z);
		TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
		RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
		TSharedRef<FJsonObject> ScaleJson = MakeShared<FJsonObject>();
		ScaleJson->SetNumberField(TEXT("x"), Scale.X);
		ScaleJson->SetNumberField(TEXT("y"), Scale.Y);
		ScaleJson->SetNumberField(TEXT("z"), Scale.Z);
		Result->SetObjectField(TEXT("location"), LocationJson);
		Result->SetObjectField(TEXT("rotation"), RotationJson);
		Result->SetObjectField(TEXT("scale3D"), ScaleJson);
		Result->SetStringField(
			TEXT("mobility"),
			StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(
				SceneComponent->Mobility));
		return Result;
	}
}

// ============================================================
// list_components
// ============================================================
class FTool_ListComponents : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.component.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
			|| !IsBoundedComponentString(
				BlueprintName,
				MaxBlueprintPathChars))
		{
			return FMCPToolResult::Error(
				TEXT("blueprint must contain between 1 and 1024 characters."),
				TEXT("invalid_params"),
				422);
		}
		int32 Limit = 50;
		int32 Offset = 0;
		int32 MaxScannedNodes = 200;
		int32 MaxChildrenPerComponent = 64;
		int32 MaxChildEntries = 1024;
		if (!ReadComponentPageInteger(
				Params, TEXT("limit"), 50, 1, 200, Limit)
			|| !ReadComponentPageInteger(
				Params, TEXT("offset"), 0, 0, MAX_int32, Offset)
			|| !ReadComponentPageInteger(
				Params,
				TEXT("maxScannedNodes"),
				200,
				1,
				MaxComponentNodeSlots,
				MaxScannedNodes)
			|| !ReadComponentPageInteger(
				Params,
				TEXT("maxChildrenPerComponent"),
				64,
				0,
				256,
				MaxChildrenPerComponent)
			|| !ReadComponentPageInteger(
				Params,
				TEXT("maxChildEntries"),
				1024,
				0,
				4096,
				MaxChildEntries))
		{
			return FMCPToolResult::Error(
				TEXT("Invalid component list pagination or scan/output limit."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Blueprint '%s' has no SCS (not an Actor Blueprint)"), *BlueprintName));

		const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
		const int32 NodeArrayCount = AllNodes.Num();
		if (NodeArrayCount > MaxScannedNodes)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Blueprint SCS contains %d node slots, exceeding the "
						"maxScannedNodes safety limit of %d. No component entries "
						"were scanned or returned; raise the limit, up to 5000, "
						"or inspect a narrower Blueprint."),
					NodeArrayCount,
					MaxScannedNodes),
				TEXT("component_scan_limit_exceeded"),
				413);
		}

		TArray<USCS_Node*> SortedNodes;
		SortedNodes.Reserve(NodeArrayCount);
		for (USCS_Node* Node : AllNodes)
		{
			if (Node)
			{
				SortedNodes.Add(Node);
			}
		}
		SortedNodes.Sort([](const USCS_Node& Left, const USCS_Node& Right)
		{
			const FString LeftKey = Left.GetVariableName().ToString()
				+ TEXT("|") + Left.VariableGuid.ToString();
			const FString RightKey = Right.GetVariableName().ToString()
				+ TEXT("|") + Right.VariableGuid.ToString();
			return LeftKey < RightKey;
		});
		const int32 Total = SortedNodes.Num();
		const int32 PageStart = FMath::Min(Offset, Total);
		const int32 PageEnd = PageStart
			+ FMath::Min(Limit, Total - PageStart);
		TArray<TSharedPtr<FJsonValue>> ComponentsArr;
		int32 ChildEntriesWritten = 0;
		bool bAnyChildrenTruncated = false;
		for (int32 NodeIndex = PageStart;
		     NodeIndex < PageEnd;
		     ++NodeIndex)
		{
			USCS_Node* Node = SortedNodes[NodeIndex];
			TSharedRef<FJsonObject> CompObj = MakeShared<FJsonObject>();
			CompObj->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
			CompObj->SetStringField(TEXT("componentNodeId"), Node->VariableGuid.ToString());
			CompObj->SetStringField(
				TEXT("componentClass"),
				Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT("None"));
			CompObj->SetStringField(
				TEXT("componentClassPath"),
				Node->ComponentClass ? Node->ComponentClass->GetPathName() : TEXT("None"));
			CompObj->SetStringField(TEXT("origin"), TEXT("localScs"));
			if (UActorComponent* Template =
				ResolveLocalComponentTemplate(BP, Node))
			{
				CompObj->SetStringField(
					TEXT("componentTemplatePath"), Template->GetPathName());
			}

			const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
			USCS_Node* Parent = SCS->FindParentNode(Node);
			if (Parent)
			{
				CompObj->SetStringField(
					TEXT("parentComponent"),
					Parent->GetVariableName().ToString());
				CompObj->SetStringField(
					TEXT("parentComponentNodeId"),
					Parent->VariableGuid.ToString());
				CompObj->SetStringField(TEXT("parentKind"), TEXT("localScs"));
			}
			else if (!Node->ParentComponentOrVariableName.IsNone())
			{
				CompObj->SetStringField(
					TEXT("parentComponent"),
					Node->ParentComponentOrVariableName.ToString());
				CompObj->SetStringField(
					TEXT("parentOwnerClass"),
					Node->ParentComponentOwnerClassName.ToString());
				CompObj->SetStringField(
					TEXT("parentKind"),
					Node->bIsParentComponentNative
						? TEXT("native")
						: TEXT("inheritedScs"));
			}
			else
			{
				CompObj->SetStringField(TEXT("parentKind"), TEXT("none"));
			}
			CompObj->SetStringField(
				TEXT("attachSocket"), Node->AttachToName.ToString());
			CompObj->SetBoolField(TEXT("isRoot"), RootNodes.Contains(Node));
			CompObj->SetBoolField(
				TEXT("isSceneComponent"),
				Node->ComponentClass
				&& Node->ComponentClass->IsChildOf(
					USceneComponent::StaticClass()));
			CompObj->SetNumberField(TEXT("childCount"), Node->GetChildNodes().Num());
			TArray<const USCS_Node*> SortedChildren;
			for (const USCS_Node* Child : Node->GetChildNodes())
			{
				if (Child)
				{
					SortedChildren.Add(Child);
				}
			}
			SortedChildren.Sort([](
				const USCS_Node& Left,
				const USCS_Node& Right)
				{
					const FString LeftKey = Left.GetVariableName().ToString()
						+ TEXT("|") + Left.VariableGuid.ToString();
					const FString RightKey = Right.GetVariableName().ToString()
						+ TEXT("|") + Right.VariableGuid.ToString();
					return LeftKey < RightKey;
				});
			const int32 RemainingChildBudget = FMath::Max(
				0,
				MaxChildEntries - ChildEntriesWritten);
			const int32 ChildOutputCount = FMath::Min3(
				SortedChildren.Num(),
				MaxChildrenPerComponent,
				RemainingChildBudget);
			TArray<TSharedPtr<FJsonValue>> Children;
			for (int32 ChildIndex = 0;
			     ChildIndex < ChildOutputCount;
			     ++ChildIndex)
			{
				const USCS_Node* Child = SortedChildren[ChildIndex];
				TSharedRef<FJsonObject> ChildIdentity = MakeShared<FJsonObject>();
				ChildIdentity->SetStringField(
					TEXT("name"), Child->GetVariableName().ToString());
				ChildIdentity->SetStringField(
					TEXT("componentNodeId"), Child->VariableGuid.ToString());
				Children.Add(MakeShared<FJsonValueObject>(ChildIdentity));
			}
			ChildEntriesWritten += ChildOutputCount;
			const bool bChildrenTruncated =
				ChildOutputCount < SortedChildren.Num();
			bAnyChildrenTruncated |= bChildrenTruncated;
			CompObj->SetBoolField(
				TEXT("childrenTruncated"),
				bChildrenTruncated);
			CompObj->SetArrayField(TEXT("children"), Children);
			ComponentsArr.Add(MakeShared<FJsonValueObject>(CompObj));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetNumberField(TEXT("count"), ComponentsArr.Num());
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(TEXT("nodeArrayCount"), NodeArrayCount);
		Result->SetNumberField(TEXT("scannedNodeCount"), NodeArrayCount);
		Result->SetNumberField(
			TEXT("pageNodeCount"), PageEnd - PageStart);
		Result->SetNumberField(TEXT("maxScannedNodes"), MaxScannedNodes);
		Result->SetNumberField(
			TEXT("maxChildrenPerComponent"), MaxChildrenPerComponent);
		Result->SetNumberField(TEXT("maxChildEntries"), MaxChildEntries);
		Result->SetNumberField(TEXT("childEntriesWritten"), ChildEntriesWritten);
		Result->SetBoolField(TEXT("scanExhausted"), false);
		Result->SetBoolField(
			TEXT("childrenTruncated"), bAnyChildrenTruncated);
		Result->SetBoolField(TEXT("partial"), bAnyChildrenTruncated);
		Result->SetBoolField(TEXT("hasMore"), PageEnd < Total);
		if (PageEnd < Total)
		{
			Result->SetNumberField(TEXT("nextOffset"), PageEnd);
		}
		Result->SetBoolField(TEXT("includesInheritedComponents"), false);
		Result->SetArrayField(TEXT("components"), ComponentsArr);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// get_component
// ============================================================
class FTool_GetComponent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.component.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName;
		FString ComponentName;
		FString ComponentNodeId;
		FString ExactProperty;
		FString PropertyFilter;
		const bool bParametersTyped = Params.IsValid()
			&& Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
			&& (!Params->HasField(TEXT("componentName"))
				|| Params->TryGetStringField(
					TEXT("componentName"), ComponentName))
			&& (!Params->HasField(TEXT("componentNodeId"))
				|| Params->TryGetStringField(
					TEXT("componentNodeId"), ComponentNodeId))
			&& (!Params->HasField(TEXT("property"))
				|| Params->TryGetStringField(TEXT("property"), ExactProperty))
			&& (!Params->HasField(TEXT("filter"))
				|| Params->TryGetStringField(TEXT("filter"), PropertyFilter));
		if (!bParametersTyped || BlueprintName.IsEmpty()
			|| (ComponentName.IsEmpty() && ComponentNodeId.IsEmpty()))
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and componentName or componentNodeId are required."),
				TEXT("invalid_params"),
				422);
		}
		if (!IsBoundedComponentString(BlueprintName, MaxBlueprintPathChars)
			|| (!ComponentName.IsEmpty()
				&& !IsBoundedComponentString(
					ComponentName,
					MaxComponentNameChars))
			|| (!ComponentNodeId.IsEmpty()
				&& !IsBoundedComponentString(
					ComponentNodeId,
					MaxComponentNodeIdChars))
			|| !IsBoundedComponentString(
				ExactProperty,
				MaxComponentPropertyNameChars,
				true)
			|| !IsBoundedComponentString(
				PropertyFilter,
				MaxComponentPropertyFilterChars,
				true))
		{
			return FMCPToolResult::Error(
				TEXT("One or more component query strings exceed their supported length."),
				TEXT("invalid_params"),
				422);
		}

		int32 Limit = 50;
		int32 Offset = 0;
		if (!ReadComponentPageInteger(
				Params,
				TEXT("limit"),
				50,
				1,
				MaxComponentPropertiesPerPage,
				Limit)
			|| !ReadComponentPageInteger(
				Params,
				TEXT("offset"),
				0,
				0,
				MAX_int32,
				Offset))
		{
			return FMCPToolResult::Error(
				TEXT("limit must be an integer in [1, 200] and offset must be a non-negative integer."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP =
			MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("asset_not_found"),
				404);
		}
		FString ComponentError;
		FString ComponentErrorCode;
		USCS_Node* Node = FindLocalComponentNode(
			BP,
			ComponentName,
			ComponentNodeId,
			ComponentError,
			ComponentErrorCode);
		if (!Node)
		{
			return FMCPToolResult::Error(
				ComponentError,
				ComponentErrorCode,
				ComponentErrorCode == TEXT("component_not_found") ? 404 : 422);
		}

		UActorComponent* ComponentTemplate =
			ResolveLocalComponentTemplate(BP, Node);
		if (!ComponentTemplate)
		{
			return FMCPToolResult::Error(
				TEXT("The local SCS node has no resolvable component template."),
				TEXT("component_template_unavailable"),
				422);
		}
		FComponentStateHashCoverage StateHashCoverage;
		const FString StateHash = ComputeComponentStateHash(
			BP,
			Node,
			ComponentTemplate,
			StateHashCoverage);

		struct FPropertyRecord
		{
			FProperty* Property = nullptr;
			FString Name;
			FString Category;
		};
		TArray<FPropertyRecord> PropertyRecords;
		for (TFieldIterator<FProperty> It(
			     ComponentTemplate->GetClass(),
			     EFieldIteratorFlags::IncludeSuper,
			     EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsReadableComponentProperty(Property))
			{
				continue;
			}
			const FString PropertyName = Property->GetName();
			const FString Category = Property->GetMetaData(TEXT("Category"));
			if (!ExactProperty.IsEmpty()
				&& !PropertyName.Equals(
					ExactProperty,
					ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (!PropertyFilter.IsEmpty()
				&& !PropertyName.Contains(
					PropertyFilter,
					ESearchCase::IgnoreCase)
				&& !Category.Contains(
					PropertyFilter,
					ESearchCase::IgnoreCase))
			{
				continue;
			}
			PropertyRecords.Add({Property, PropertyName, Category});
		}
		PropertyRecords.Sort([](
			const FPropertyRecord& Left,
			const FPropertyRecord& Right)
			{
				return Left.Name < Right.Name;
			});
		if (!ExactProperty.IsEmpty() && PropertyRecords.IsEmpty())
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Editable property '%s' was not found on component '%s'."),
					*ExactProperty,
					*Node->GetVariableName().ToString()),
				TEXT("property_not_found"),
				404);
		}

		const int32 Total = PropertyRecords.Num();
		const int32 PageStart = FMath::Min(Offset, Total);
		const int32 PageEnd =
			PageStart + FMath::Min(Limit, Total - PageStart);
		TArray<TSharedPtr<FJsonValue>> PropertyValues;
		PropertyValues.Reserve(PageEnd - PageStart);
		for (int32 Index = PageStart; Index < PageEnd; ++Index)
		{
			const FPropertyRecord& Record = PropertyRecords[Index];
			FString SerializedValue;
			const bool bExported = ExportComponentPropertyValue(
				ComponentTemplate,
				Record.Property->GetFName(),
				SerializedValue);
			const bool bTruncated =
				SerializedValue.Len() > MaxComponentPropertyValueChars;
			if (bTruncated)
			{
				SerializedValue.LeftInline(
					MaxComponentPropertyValueChars,
					false);
			}
			TSharedRef<FJsonObject> PropertyJson = MakeShared<FJsonObject>();
			PropertyJson->SetStringField(TEXT("name"), Record.Name);
			PropertyJson->SetStringField(
				TEXT("cppType"), Record.Property->GetCPPType());
			PropertyJson->SetStringField(TEXT("category"), Record.Category);
			PropertyJson->SetStringField(
				TEXT("ownerClass"),
				Record.Property->GetOwnerClass()
					? Record.Property->GetOwnerClass()->GetPathName()
					: TEXT("None"));
			const bool bWritable =
				IsWritableComponentProperty(Record.Property);
			PropertyJson->SetBoolField(TEXT("editable"), bWritable);
			PropertyJson->SetBoolField(
				TEXT("readOnly"),
				!bWritable);
			PropertyJson->SetBoolField(TEXT("valueAvailable"), bExported);
			PropertyJson->SetBoolField(TEXT("valueTruncated"), bTruncated);
			if (bExported)
			{
				PropertyJson->SetStringField(TEXT("value"), SerializedValue);
			}
			PropertyValues.Add(MakeShared<FJsonValueObject>(PropertyJson));
		}

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		USCS_Node* Parent = SCS->FindParentNode(Node);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(
			TEXT("componentName"), Node->GetVariableName().ToString());
		Result->SetStringField(
			TEXT("componentNodeId"), Node->VariableGuid.ToString());
		Result->SetStringField(TEXT("origin"), TEXT("localScs"));
		Result->SetStringField(
			TEXT("componentClass"), ComponentTemplate->GetClass()->GetName());
		Result->SetStringField(
			TEXT("componentClassPath"), ComponentTemplate->GetClass()->GetPathName());
		Result->SetStringField(
			TEXT("componentTemplatePath"), ComponentTemplate->GetPathName());
		Result->SetBoolField(
			TEXT("stateHashAvailable"), !StateHash.IsEmpty());
		Result->SetObjectField(
			TEXT("stateHashCoverage"),
			SerializeComponentHashCoverage(StateHashCoverage));
		if (!StateHash.IsEmpty())
		{
			Result->SetStringField(TEXT("stateHash"), StateHash);
		}
		TSharedRef<FJsonObject> TemplateIdentity = MakeShared<FJsonObject>();
		TemplateIdentity->SetStringField(
			TEXT("objectPath"), ComponentTemplate->GetPathName());
		TemplateIdentity->SetStringField(
			TEXT("classPath"), ComponentTemplate->GetClass()->GetPathName());
		TemplateIdentity->SetStringField(
			TEXT("componentNodeId"), Node->VariableGuid.ToString());
		if (!StateHash.IsEmpty())
		{
			TemplateIdentity->SetStringField(TEXT("stateHash"), StateHash);
		}
		Result->SetObjectField(TEXT("templateIdentity"), TemplateIdentity);
		Result->SetStringField(
			TEXT("componentTemplateOuter"),
			ComponentTemplate->GetOuter()
				? ComponentTemplate->GetOuter()->GetPathName()
				: TEXT("None"));
		Result->SetBoolField(TEXT("isRoot"), SCS->GetRootNodes().Contains(Node));
		Result->SetBoolField(
			TEXT("isSceneComponent"),
			ComponentTemplate->IsA<USceneComponent>());
		Result->SetStringField(TEXT("attachSocket"), Node->AttachToName.ToString());
		if (Parent)
		{
			Result->SetStringField(
				TEXT("parentComponent"),
				Parent->GetVariableName().ToString());
			Result->SetStringField(
				TEXT("parentComponentNodeId"),
				Parent->VariableGuid.ToString());
			Result->SetStringField(TEXT("parentKind"), TEXT("localScs"));
		}
		else if (!Node->ParentComponentOrVariableName.IsNone())
		{
			Result->SetStringField(
				TEXT("parentComponent"),
				Node->ParentComponentOrVariableName.ToString());
			Result->SetStringField(
				TEXT("parentOwnerClass"),
				Node->ParentComponentOwnerClassName.ToString());
			Result->SetStringField(
				TEXT("parentKind"),
				Node->bIsParentComponentNative
					? TEXT("native")
					: TEXT("inheritedScs"));
		}
		else
		{
			Result->SetStringField(TEXT("parentKind"), TEXT("none"));
		}
		if (const USceneComponent* SceneComponent =
			Cast<USceneComponent>(ComponentTemplate))
		{
			Result->SetObjectField(
				TEXT("relativeTransform"),
				SerializeComponentTransform(SceneComponent));
		}
		Result->SetNumberField(TEXT("propertyCount"), PropertyValues.Num());
		Result->SetNumberField(TEXT("propertyTotal"), Total);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetBoolField(TEXT("hasMore"), PageEnd < Total);
		Result->SetArrayField(TEXT("properties"), PropertyValues);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_component_property
// ============================================================
class FTool_SetComponentProperty : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.component.property.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName;
		FString ComponentName;
		FString ComponentNodeId;
		FString RequestedPropertyName;
		FString RequestedValue;
		const bool bParametersTyped = Params.IsValid()
			&& Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
			&& (!Params->HasField(TEXT("componentName"))
				|| Params->TryGetStringField(
					TEXT("componentName"), ComponentName))
			&& (!Params->HasField(TEXT("componentNodeId"))
				|| Params->TryGetStringField(
					TEXT("componentNodeId"), ComponentNodeId))
			&& Params->TryGetStringField(
				TEXT("property"), RequestedPropertyName)
			&& Params->TryGetStringField(TEXT("value"), RequestedValue);
		if (!bParametersTyped || BlueprintName.IsEmpty()
			|| (ComponentName.IsEmpty() && ComponentNodeId.IsEmpty())
			|| RequestedPropertyName.IsEmpty()
			|| !Params->HasField(TEXT("value")))
		{
			return FMCPToolResult::Error(
				TEXT("blueprint, componentName or componentNodeId, property, and value are required."),
				TEXT("invalid_params"),
				422);
		}
		if (!IsBoundedComponentString(BlueprintName, MaxBlueprintPathChars)
			|| (!ComponentName.IsEmpty()
				&& !IsBoundedComponentString(
					ComponentName,
					MaxComponentNameChars))
			|| (!ComponentNodeId.IsEmpty()
				&& !IsBoundedComponentString(
					ComponentNodeId,
					MaxComponentNodeIdChars))
			|| !IsBoundedComponentString(
				RequestedPropertyName,
				MaxComponentPropertyNameChars)
			|| !IsBoundedComponentString(
				RequestedValue,
				MaxSerializedComponentPropertyValueChars,
				true))
		{
			return FMCPToolResult::Error(
				TEXT("One or more component mutation strings exceed their supported length."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP =
			MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("asset_not_found"),
				404);
		}

		FString ComponentError;
		FString ComponentErrorCode;
		USCS_Node* Node = FindLocalComponentNode(
			BP,
			ComponentName,
			ComponentNodeId,
			ComponentError,
			ComponentErrorCode);
		if (!Node)
		{
			return FMCPToolResult::Error(
				ComponentError,
				ComponentErrorCode,
				ComponentErrorCode == TEXT("component_not_found") ? 404 : 422);
		}
		UActorComponent* ComponentTemplate =
			ResolveLocalComponentTemplate(BP, Node);
		if (!ComponentTemplate)
		{
			return FMCPToolResult::Error(
				TEXT("The local SCS node has no resolvable component template."),
				TEXT("component_template_unavailable"),
				422);
		}
		FComponentStateHashCoverage StateHashCoverageBefore;
		const FString StateHashBefore = ComputeComponentStateHash(
			BP,
			Node,
			ComponentTemplate,
			StateHashCoverageBefore);
		if (StateHashBefore.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Could not compute a complete component state hash: ")
				+ StateHashCoverageBefore.UnavailableReason,
				TEXT("state_hash_unavailable"),
				500);
		}
		FString ExpectedStateHash;
		if (Params->HasField(TEXT("expectedStateHash")))
		{
			if (!Params->TryGetStringField(
				TEXT("expectedStateHash"),
				ExpectedStateHash))
			{
				return FMCPToolResult::Error(
					TEXT("expectedStateHash must be a string."),
					TEXT("invalid_params"),
					422);
			}
			if (!IsValidSha256StateHash(ExpectedStateHash))
			{
				return FMCPToolResult::Error(
					TEXT("expectedStateHash must be a sha256: value with 64 hexadecimal digits."),
					TEXT("invalid_params"),
					422);
			}
			if (ExpectedStateHash != StateHashBefore)
			{
				return FMCPToolResult::Error(
					TEXT("The component changed since it was read."),
					TEXT("component_state_conflict"),
					409);
			}
		}

		FString ExpectedTemplatePath;
		if (Params->HasField(TEXT("expectedTemplatePath")))
		{
			if (!Params->TryGetStringField(
				TEXT("expectedTemplatePath"),
				ExpectedTemplatePath))
			{
				return FMCPToolResult::Error(
					TEXT("expectedTemplatePath must be a string."),
					TEXT("invalid_params"),
					422);
			}
			if (!IsBoundedComponentString(
				ExpectedTemplatePath,
				MaxBlueprintPathChars))
			{
				return FMCPToolResult::Error(
					TEXT("expectedTemplatePath must contain between 1 and 1024 characters."),
					TEXT("invalid_params"),
					422);
			}
			if (ExpectedTemplatePath != ComponentTemplate->GetPathName())
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("The component template changed. Expected '%s', current '%s'."),
						*ExpectedTemplatePath,
						*ComponentTemplate->GetPathName()),
					TEXT("stale_component"),
					409);
			}
		}

		if (Params->HasField(TEXT("expectedTemplateIdentity")))
		{
			const TSharedPtr<FJsonObject>* ExpectedIdentity = nullptr;
			if (!Params->TryGetObjectField(
				TEXT("expectedTemplateIdentity"), ExpectedIdentity)
				|| !ExpectedIdentity || !ExpectedIdentity->IsValid())
			{
				return FMCPToolResult::Error(
					TEXT("expectedTemplateIdentity must be an object."),
					TEXT("invalid_params"),
					422);
			}
			FString ExpectedObjectPath;
			FString ExpectedClassPath;
			FString ExpectedIdentityStateHash;
			if (!(*ExpectedIdentity)->TryGetStringField(
				TEXT("objectPath"), ExpectedObjectPath)
				|| !(*ExpectedIdentity)->TryGetStringField(
					TEXT("classPath"), ExpectedClassPath))
			{
				return FMCPToolResult::Error(
					TEXT("expectedTemplateIdentity requires objectPath and classPath."),
					TEXT("invalid_params"),
					422);
			}
			(*ExpectedIdentity)->TryGetStringField(
				TEXT("stateHash"), ExpectedIdentityStateHash);
			const bool bIdentityMatches = ExpectedObjectPath
				== ComponentTemplate->GetPathName()
				&& ExpectedClassPath == ComponentTemplate->GetClass()->GetPathName()
				&& (ExpectedIdentityStateHash.IsEmpty()
					|| ExpectedIdentityStateHash == StateHashBefore);
			if (!bIdentityMatches)
			{
				return FMCPToolResult::Error(
					TEXT("The component template identity changed. Re-read blueprint.component.get before writing."),
					TEXT("stale_component_template_identity"),
					409);
			}
		}

		FProperty* Property = ComponentTemplate->GetClass()->FindPropertyByName(
			FName(*RequestedPropertyName));
		if (!Property)
		{
			for (TFieldIterator<FProperty> It(
				     ComponentTemplate->GetClass(),
				     EFieldIteratorFlags::IncludeSuper,
				     EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
			{
				if (It->GetName().Equals(
					RequestedPropertyName,
					ESearchCase::IgnoreCase))
				{
					Property = *It;
					break;
				}
			}
		}
		if (!Property || !IsReadableComponentProperty(Property))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Editable property '%s' was not found on component '%s'."),
					*RequestedPropertyName,
					*Node->GetVariableName().ToString()),
				TEXT("property_not_found"),
				404);
		}
		if (!IsWritableComponentProperty(Property))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Property '%s' is read-only or non-persistent."),
					*Property->GetName()),
				TEXT("property_not_writable"),
				422);
		}

		const FName PropertyName = Property->GetFName();
		const FString StableNodeId = Node->VariableGuid.ToString();
		const FString StableComponentName =
			Node->GetVariableName().ToString();
		FString OldValue;
		if (!ExportComponentPropertyValue(
			ComponentTemplate,
			PropertyName,
			OldValue))
		{
			return FMCPToolResult::Error(
				TEXT("Could not serialize the current component property value."),
				TEXT("property_read_failed"),
				500);
		}
		FString ExpectedOldValue;
		if (Params->HasField(TEXT("expectedOldValue")))
		{
			if (!Params->TryGetStringField(
				TEXT("expectedOldValue"),
				ExpectedOldValue))
			{
				return FMCPToolResult::Error(
					TEXT("expectedOldValue must be a string."),
					TEXT("invalid_params"),
					422);
			}
			if (!IsBoundedComponentString(
				ExpectedOldValue,
				MaxSerializedComponentPropertyValueChars,
				true))
			{
				return FMCPToolResult::Error(
					TEXT("expectedOldValue may contain at most 65536 characters."),
					TEXT("invalid_params"),
					422);
			}
			if (ExpectedOldValue != OldValue)
			{
				FString ExpectedOldValueHash;
				FString CurrentOldValueHash;
				TryHashComponentText(
					ExpectedOldValue,
					ExpectedOldValueHash);
				TryHashComponentText(OldValue, CurrentOldValueHash);
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("The component property changed. Expected length/hash %d/%s, current %d/%s."),
						ExpectedOldValue.Len(),
						*ExpectedOldValueHash,
						OldValue.Len(),
						*CurrentOldValueHash),
					TEXT("stale_property"),
					409);
			}
		}

		FString ExpectedNormalizedValue;
		if (!NormalizeComponentPropertyValue(
			ComponentTemplate,
			Property,
			RequestedValue,
			ExpectedNormalizedValue))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Property '%s' rejected the supplied Unreal text value."),
					*PropertyName.ToString()),
				TEXT("property_value_invalid"),
				422);
		}

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard
			Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		TUniquePtr<FScopedTransaction> DirectTransaction;
		if (!UEAIIntegration::Workflow::GetExecutionContext(Params).IsValid())
		{
			DirectTransaction = MakeUnique<FScopedTransaction>(
				FText::FromString(
					TEXT("UE AI: Set Blueprint Component Property")));
		}
		Guard.MarkMutationStarted();
		BP->Modify();

		auto ResolveCurrentTemplate = [&]() -> UActorComponent*
		{
			FString Error;
			FString ErrorCode;
			USCS_Node* CurrentNode = FindLocalComponentNode(
				BP,
				FString(),
				StableNodeId,
				Error,
				ErrorCode);
			return ResolveLocalComponentTemplate(BP, CurrentNode);
		};
		auto RestoreOldValue = [&]() -> bool
		{
			UActorComponent* CurrentTemplate = ResolveCurrentTemplate();
			FString RestoredValue;
			return ImportComponentPropertyValue(
					CurrentTemplate,
					PropertyName,
					OldValue,
					RestoredValue)
				&& RestoredValue == OldValue;
		};
		auto FailAndRollback = [&](
			const FString& Message,
			const FString& Code,
			const int32 Status) -> FMCPToolResult
		{
			const bool bValueRestored = RestoreOldValue();
			if (DirectTransaction)
			{
				DirectTransaction->Cancel();
			}
			FString RollbackError;
			const bool bGuardRestored = Guard.Rollback(RollbackError);
			FString VerifiedValue;
			const bool bRestoreVerified = ExportComponentPropertyValue(
					ResolveCurrentTemplate(),
					PropertyName,
					VerifiedValue)
				&& VerifiedValue == OldValue;
			if (!bValueRestored || !bGuardRestored || !bRestoreVerified)
			{
				return FMCPToolResult::Error(
					Message
					+ TEXT(" The previous component property could not be fully restored. ")
					+ RollbackError,
					TEXT("rollback_failed"),
					500);
			}
			return FMCPToolResult::Error(
				Message + TEXT(" The mutation was rolled back and verified."),
				Code,
				Status);
		};

		FString AppliedValue;
		if (!ImportComponentPropertyValue(
				ComponentTemplate,
				PropertyName,
				RequestedValue,
				AppliedValue)
			|| AppliedValue != ExpectedNormalizedValue)
		{
			return FailAndRollback(
				TEXT("The component property did not accept the validated value."),
				TEXT("property_write_failed"),
				500);
		}
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);

		const bool bDeferred =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		bool bCompiled = false;
		bool bSaved = false;
		if (!bDeferred)
		{
			FKismetEditorUtilities::CompileBlueprint(
				BP,
				EBlueprintCompileOptions::SkipSave);
			bCompiled = BP->Status != BS_Error;
			if (!bCompiled)
			{
				return FailAndRollback(
					TEXT("The component property change introduced Blueprint compile errors."),
					TEXT("asset_compile_failed"),
					500);
			}
		}

		FString ReadBackValue;
		if (!ExportComponentPropertyValue(
				ResolveCurrentTemplate(),
				PropertyName,
				ReadBackValue)
			|| ReadBackValue != ExpectedNormalizedValue)
		{
			return FailAndRollback(
				TEXT("The component property value did not survive read-back."),
				TEXT("property_persistence_failed"),
				500);
		}
		USCS_Node* FinalNode = nullptr;
		UActorComponent* FinalTemplate = ResolveCurrentTemplate();
		FString FinalError;
		FString FinalErrorCode;
		FinalNode = FindLocalComponentNode(
			BP,
			FString(),
			StableNodeId,
			FinalError,
			FinalErrorCode);
		FComponentStateHashCoverage StateHashCoverageAfter;
		const FString StateHashAfter = ComputeComponentStateHash(
			BP,
			FinalNode,
			FinalTemplate,
			StateHashCoverageAfter);
		if (StateHashAfter.IsEmpty())
		{
			return FailAndRollback(
				TEXT("Could not compute the final component state hash."),
				TEXT("state_hash_unavailable"),
				500);
		}
		if (!bDeferred)
		{
			UEAIIntegration::Infrastructure::FBlueprintPersistenceError SaveError;
			bSaved = UEAIIntegration::Infrastructure::SaveBlueprintPackage(
				BP,
				nullptr,
				SaveError);
			if (!bSaved)
			{
				return FailAndRollback(
					SaveError.Message.IsEmpty()
						? TEXT("The component property change could not be saved.")
						: SaveError.Message,
					SaveError.Code.IsEmpty()
						? TEXT("asset_save_failed")
						: SaveError.Code,
					500);
			}
		}
		Guard.Commit();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(
			TEXT("componentName"), StableComponentName);
		Result->SetStringField(TEXT("componentNodeId"), StableNodeId);
		Result->SetStringField(
			TEXT("componentTemplatePath"),
			FinalTemplate->GetPathName());
		TSharedRef<FJsonObject> FinalTemplateIdentity = MakeShared<FJsonObject>();
		FinalTemplateIdentity->SetStringField(
			TEXT("objectPath"), FinalTemplate->GetPathName());
		FinalTemplateIdentity->SetStringField(
			TEXT("classPath"), FinalTemplate->GetClass()->GetPathName());
		FinalTemplateIdentity->SetStringField(
			TEXT("componentNodeId"), StableNodeId);
		FinalTemplateIdentity->SetStringField(
			TEXT("stateHash"), StateHashAfter);
		Result->SetObjectField(TEXT("templateIdentity"), FinalTemplateIdentity);
		Result->SetStringField(TEXT("property"), PropertyName.ToString());
		SetBoundedComponentValueReceipt(Result, TEXT("old"), OldValue);
		SetBoundedComponentValueReceipt(Result, TEXT("new"), ReadBackValue);
		Result->SetStringField(TEXT("stateHashBefore"), StateHashBefore);
		Result->SetStringField(TEXT("stateHash"), StateHashAfter);
		Result->SetObjectField(
			TEXT("stateHashCoverage"),
			SerializeComponentHashCoverage(StateHashCoverageAfter));
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), bDeferred);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// add_component
// ============================================================
class FTool_AddComponent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.component.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString ComponentClassName = Params->GetStringField(TEXT("componentClass"));
		FString ComponentName = Params->GetStringField(TEXT("name"));

		if (BlueprintName.IsEmpty() || ComponentClassName.IsEmpty() || ComponentName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, componentClass, name"));

		FString ParentComponentName;
		Params->TryGetStringField(TEXT("parentComponent"), ParentComponentName);

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
			return FMCPToolResult::Error(FString::Printf(TEXT("Blueprint '%s' has no SCS"), *BlueprintName));

		// Check for duplicate
		const TArray<USCS_Node*>& ExistingNodes = SCS->GetAllNodes();
		for (USCS_Node* Existing : ExistingNodes)
		{
			if (Existing && Existing->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
				return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' already exists"), *ComponentName));
		}

		// Resolve component class
		UClass* ComponentClass = nullptr;
		// Blueprint build-graph specs persist canonical object paths (for example
		// /Script/Engine.SphereComponent). Resolve those paths first; comparing the
		// short UObject name against the full path silently rejected valid specs.
		if (ComponentClassName.Contains(TEXT(".")))
		{
			ComponentClass = LoadObject<UClass>(nullptr, *ComponentClassName);
			if (ComponentClass
				&& !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
			{
				ComponentClass = nullptr;
			}
		}
		TArray<FString> NamesToTry;
		NamesToTry.Add(ComponentClassName);
		if (!ComponentClassName.StartsWith(TEXT("U")))
			NamesToTry.Add(FString::Printf(TEXT("U%s"), *ComponentClassName));
		else
			NamesToTry.Add(ComponentClassName.Mid(1));

		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (!It->IsChildOf(UActorComponent::StaticClass())) continue;
			FString ClassName = It->GetName();
			for (const FString& NameToTry : NamesToTry)
			{
				if (ClassName.Equals(NameToTry, ESearchCase::IgnoreCase))
				{
					ComponentClass = *It;
					break;
				}
			}
			if (ComponentClass) break;
		}

		if (!ComponentClass)
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Component class '%s' not found or not a UActorComponent subclass"), *ComponentClassName));

		// Find parent SCS node if specified
		USCS_Node* ParentSCSNode = nullptr;
		if (!ParentComponentName.IsEmpty())
		{
			for (USCS_Node* Node : ExistingNodes)
			{
				if (Node && Node->GetVariableName().ToString().Equals(ParentComponentName, ESearchCase::IgnoreCase))
				{
					ParentSCSNode = Node;
					break;
				}
			}
			if (!ParentSCSNode)
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Parent component '%s' not found"), *ParentComponentName));
		}

		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, FName(*ComponentName));
		if (!NewNode)
			return FMCPToolResult::Error(TEXT("Failed to create SCS node"));

		if (ParentSCSNode)
			ParentSCSNode->AddChildNode(NewNode);
		else
			SCS->AddNode(NewNode);

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("name"), NewNode->GetVariableName().ToString());
		Result->SetStringField(TEXT("componentClass"), ComponentClass->GetName());
		if (ParentSCSNode) Result->SetStringField(TEXT("parentComponent"), ParentSCSNode->GetVariableName().ToString());
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// remove_component
// ============================================================
class FTool_RemoveComponent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.component.remove");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString ComponentName = Params->GetStringField(TEXT("name"));

		if (BlueprintName.IsEmpty() || ComponentName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, name"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
			return FMCPToolResult::Error(FString::Printf(TEXT("Blueprint '%s' has no SCS"), *BlueprintName));

		USCS_Node* NodeToRemove = nullptr;
		const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
		for (USCS_Node* Node : AllNodes)
		{
			if (Node && Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				NodeToRemove = Node;
				break;
			}
		}

		if (!NodeToRemove)
			return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' not found"), *ComponentName));

		const TArray<USCS_Node*>& RootNodes = SCS->GetRootNodes();
		if (RootNodes.Contains(NodeToRemove) && NodeToRemove->GetChildNodes().Num() > 0)
			return FMCPToolResult::Error(FString::Printf(
				TEXT("Cannot remove root component '%s' with %d children. Remove children first."),
				*ComponentName, NodeToRemove->GetChildNodes().Num()));

		SCS->RemoveNodeAndPromoteChildren(NodeToRemove);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		bool bSaved = MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("name"), ComponentName);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// reparent_component
// ============================================================
class FTool_ReparentComponent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.component.reparent");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName;
		FString ComponentName;
		FString ComponentNodeId;
		FString NewParentName;
		FString NewParentNodeId;
		const bool bParametersTyped = Params.IsValid()
			&& Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
			&& (!Params->HasField(TEXT("componentName"))
				|| Params->TryGetStringField(TEXT("componentName"), ComponentName))
			&& (!Params->HasField(TEXT("componentNodeId"))
				|| Params->TryGetStringField(TEXT("componentNodeId"), ComponentNodeId))
			&& (!Params->HasField(TEXT("newParentName"))
				|| Params->TryGetStringField(TEXT("newParentName"), NewParentName))
			&& (!Params->HasField(TEXT("newParentNodeId"))
				|| Params->TryGetStringField(TEXT("newParentNodeId"), NewParentNodeId));
		if (!bParametersTyped || BlueprintName.IsEmpty()
			|| (ComponentName.IsEmpty() && ComponentNodeId.IsEmpty()))
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and componentName or componentNodeId are required."),
				TEXT("invalid_params"),
				422);
		}
		if (!IsBoundedComponentString(BlueprintName, MaxBlueprintPathChars)
			|| (!ComponentName.IsEmpty()
				&& !IsBoundedComponentString(ComponentName, MaxComponentNameChars))
			|| (!ComponentNodeId.IsEmpty()
				&& !IsBoundedComponentString(ComponentNodeId, MaxComponentNodeIdChars))
			|| (!NewParentName.IsEmpty()
				&& !IsBoundedComponentString(NewParentName, MaxComponentNameChars))
			|| (!NewParentNodeId.IsEmpty()
				&& !IsBoundedComponentString(NewParentNodeId, MaxComponentNodeIdChars)))
		{
			return FMCPToolResult::Error(
				TEXT("One or more component mutation strings exceed their supported length."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);
		}
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
		{
			return FMCPToolResult::Error(
				TEXT("This Blueprint has no Simple Construction Script."),
				TEXT("component_scs_unavailable"),
				422);
		}

		FString ComponentError;
		FString ComponentErrorCode;
		USCS_Node* Node = FindLocalComponentNode(
			BP, ComponentName, ComponentNodeId, ComponentError, ComponentErrorCode);
		if (!Node)
		{
			return FMCPToolResult::Error(
				ComponentError,
				ComponentErrorCode,
				ComponentErrorCode == TEXT("component_not_found") ? 404 : 422);
		}

		// An empty parent selector means "move to the SCS root".
		USCS_Node* NewParentNode = nullptr;
		if (!NewParentName.IsEmpty() || !NewParentNodeId.IsEmpty())
		{
			FString ParentError;
			FString ParentErrorCode;
			NewParentNode = FindLocalComponentNode(
				BP, NewParentName, NewParentNodeId, ParentError, ParentErrorCode);
			if (!NewParentNode)
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("New parent component: %s"), *ParentError),
					ParentErrorCode,
					ParentErrorCode == TEXT("component_not_found") ? 404 : 422);
			}
			if (NewParentNode == Node)
			{
				return FMCPToolResult::Error(
					TEXT("Cannot reparent a component to itself."),
					TEXT("invalid_params"),
					422);
			}

			// Reject cycles: the new parent must not be a descendant of the node
			// being moved. Iterative breadth-first walk avoids recursion depth on
			// deeply nested SCS trees.
			bool bWouldCreateCycle = false;
			TArray<USCS_Node*> Stack = Node->GetChildNodes();
			for (int32 Index = 0; Index < Stack.Num(); ++Index)
			{
				USCS_Node* Current = Stack[Index];
				if (Current == NewParentNode)
				{
					bWouldCreateCycle = true;
					break;
				}
				Stack.Append(Current->GetChildNodes());
			}
			if (bWouldCreateCycle)
			{
				return FMCPToolResult::Error(
					TEXT("Cannot reparent a component to one of its own descendants."),
					TEXT("invalid_params"),
					422);
			}
		}

		const FString StableNodeId = Node->VariableGuid.ToString();
		const FString StableComponentName = Node->GetVariableName().ToString();
		USCS_Node* OldParent = SCS->FindParentNode(Node);
		const bool bSameLocalParent =
			OldParent == NewParentNode
			|| (OldParent == nullptr && NewParentNode == nullptr);

		if (!bSameLocalParent)
		{
			// A reparent attaches to the new parent's default socket, not a socket
			// that only existed on the previous parent.
			Node->Modify();
			Node->AttachToName = NAME_None;

			if (NewParentNode)
			{
				if (OldParent)
				{
					OldParent->RemoveChildNode(Node);
				}
				else
				{
					// Root node becomes a local child. RemoveNode also clears any
					// native/inherited parent identity (ParentComponentOrVariableName,
					// ParentComponentOwnerClassName, bIsParentComponentNative).
					SCS->RemoveNode(Node);
				}
				NewParentNode->AddChildNode(Node);
			}
			else if (OldParent)
			{
				// Promote to root. An Actor Blueprint has exactly one scene root,
				// and compilation (USimpleConstructionScript::FixupSceneNodeHierarchy,
				// reached through FixupRootNodeParentReferences) re-nests any extra
				// scene-component root back under the scene root, silently undoing a
				// plain AddNode. So promoting a scene component means making it the
				// scene root and nesting the previous scene root underneath it — the
				// same normalization the SCS editor performs when a component is
				// dropped onto the root. Non-scene components become ordinary roots.
				OldParent->RemoveChildNode(Node);

				USCS_Node* OldSceneRootNode = nullptr;
				if (Node->ComponentClass
					&& Node->ComponentClass->IsChildOf(USceneComponent::StaticClass()))
				{
					// GetSceneRootComponentTemplate does not reliably populate its
					// optional OutSCSNode when the CDO already exposes a root, so
					// walk the root set directly to find the local scene root node.
					for (USCS_Node* RootNode : SCS->GetRootNodes())
					{
						if (RootNode
							&& RootNode != SCS->GetDefaultSceneRootNode()
							&& RootNode->ComponentClass
							&& RootNode->ComponentClass->IsChildOf(
								USceneComponent::StaticClass()))
						{
							OldSceneRootNode = RootNode;
							break;
						}
					}
				}

				if (OldSceneRootNode && OldSceneRootNode != Node)
				{
					// Keep the old root in AllNodes; remove it from the root set only,
					// then nest it under the newly promoted scene root.
					SCS->RemoveNode(OldSceneRootNode, /*bValidateSceneRootNodes=*/false);
					SCS->AddNode(Node);
					Node->AddChildNode(OldSceneRootNode);
				}
				else
				{
					SCS->AddNode(Node);
				}
			}

			UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		}

		const bool bChanged = !bSameLocalParent;
		const bool bSaved =
			bChanged
			&& UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		// Compilation can normalize the SCS hierarchy (for example, when a scene
		// component is promoted to root), so report the effective post-operation
		// parent/root state rather than the requested one.
		const bool bIsRoot = SCS->GetRootNodes().Contains(Node);
		USCS_Node* EffectiveParent = SCS->FindParentNode(Node);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("componentName"), StableComponentName);
		Result->SetStringField(TEXT("componentNodeId"), StableNodeId);
		Result->SetBoolField(TEXT("isRoot"), bIsRoot);
		Result->SetBoolField(TEXT("changed"), bChanged);
		if (EffectiveParent)
		{
			Result->SetStringField(
				TEXT("parentComponent"),
				EffectiveParent->GetVariableName().ToString());
			Result->SetStringField(
				TEXT("parentComponentNodeId"),
				EffectiveParent->VariableGuid.ToString());
		}
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// rename_component
// ============================================================
class FTool_RenameComponent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("blueprint.component.rename"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName, ComponentName, ComponentNodeId, NewName;
		const bool bTyped = Params.IsValid()
			&& Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
			&& (!Params->HasField(TEXT("componentName")) || Params->TryGetStringField(
				TEXT("componentName"), ComponentName))
			&& (!Params->HasField(TEXT("componentNodeId")) || Params->TryGetStringField(
				TEXT("componentNodeId"), ComponentNodeId))
			&& Params->TryGetStringField(TEXT("newName"), NewName);
		if (!bTyped || BlueprintName.IsEmpty()
			|| (ComponentName.IsEmpty() && ComponentNodeId.IsEmpty())
			|| NewName.IsEmpty() || NewName.Len() > MaxComponentNameChars)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint, componentName/componentNodeId and newName are required."),
				TEXT("invalid_params"), 422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);
		if (!BP->SimpleConstructionScript)
			return FMCPToolResult::Error(
				TEXT("This Blueprint has no Simple Construction Script."), TEXT("component_scs_unavailable"), 422);

		FString ResolveError, ResolveCode;
		USCS_Node* Node = FindLocalComponentNode(BP, ComponentName, ComponentNodeId, ResolveError, ResolveCode);
		if (!Node)
			return FMCPToolResult::Error(ResolveError, ResolveCode,
			                             ResolveCode == TEXT("component_not_found") ? 404 : 422);

		const TArray<USCS_Node*>& AllNodes = BP->SimpleConstructionScript->GetAllNodes();
		for (USCS_Node* Other : AllNodes)
		{
			if (Other && Other != Node
				&& Other->GetVariableName().ToString().Equals(NewName, ESearchCase::IgnoreCase))
			{
				return FMCPToolResult::Error(FString::Printf(TEXT("Component '%s' already exists"), *NewName),
				                             TEXT("already_exists"), 409);
			}
		}

		const FString OldName = Node->GetVariableName().ToString();
		Node->Modify();
		Node->SetVariableName(FName(*NewName));

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("oldName"), OldName);
		Result->SetStringField(TEXT("componentName"), NewName);
		Result->SetStringField(TEXT("componentNodeId"), Node->VariableGuid.ToString());
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// duplicate_component
// ============================================================
class FTool_DuplicateComponent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("blueprint.component.duplicate"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName, ComponentName, ComponentNodeId, NewName;
		const bool bTyped = Params.IsValid()
			&& Params->TryGetStringField(TEXT("blueprint"), BlueprintName)
			&& (!Params->HasField(TEXT("componentName")) || Params->TryGetStringField(
				TEXT("componentName"), ComponentName))
			&& (!Params->HasField(TEXT("componentNodeId")) || Params->TryGetStringField(
				TEXT("componentNodeId"), ComponentNodeId))
			&& (!Params->HasField(TEXT("newName")) || Params->TryGetStringField(TEXT("newName"), NewName));
		if (!bTyped || BlueprintName.IsEmpty()
			|| (ComponentName.IsEmpty() && ComponentNodeId.IsEmpty())
			|| NewName.Len() > MaxComponentNameChars)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and componentName/componentNodeId are required; newName is optional."),
				TEXT("invalid_params"), 422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);
		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (!SCS)
			return FMCPToolResult::Error(
				TEXT("This Blueprint has no Simple Construction Script."), TEXT("component_scs_unavailable"), 422);

		FString ResolveError, ResolveCode;
		USCS_Node* Node = FindLocalComponentNode(BP, ComponentName, ComponentNodeId, ResolveError, ResolveCode);
		if (!Node)
			return FMCPToolResult::Error(ResolveError, ResolveCode,
			                             ResolveCode == TEXT("component_not_found") ? 404 : 422);

		const TArray<USCS_Node*>& AllNodes = SCS->GetAllNodes();
		auto NameTaken = [&](const FString& Name)
		{
			for (USCS_Node* Other : AllNodes)
			{
				if (Other && Other->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase)) return true;
			}
			return false;
		};
		FString CopyName = NewName.IsEmpty() ? Node->GetVariableName().ToString() + TEXT("_Copy") : NewName;
		const FString Base = CopyName;
		int32 Suffix = 1;
		while (NameTaken(CopyName))
		{
			CopyName = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}

		USCS_Node* NewNode = SCS->CreateNode(Node->ComponentClass, FName(*CopyName));
		if (!NewNode)
			return FMCPToolResult::Error(TEXT("Failed to create SCS node"), TEXT("scs_create_failed"), 500);
		if (Node->ComponentTemplate && NewNode->ComponentTemplate)
		{
			UEngine::CopyPropertiesForUnrelatedObjects(Node->ComponentTemplate, NewNode->ComponentTemplate);
		}
		USCS_Node* Parent = SCS->FindParentNode(Node);
		if (Parent) Parent->AddChildNode(NewNode);
		else SCS->AddNode(NewNode);

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("sourceComponentName"), Node->GetVariableName().ToString());
		Result->SetStringField(TEXT("componentName"), CopyName);
		Result->SetStringField(TEXT("componentNodeId"), NewNode->VariableGuid.ToString());
		if (Parent) Result->SetStringField(TEXT("parentComponent"), Parent->GetVariableName().ToString());
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterComponentTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_ListComponents>());
		Registry.Register(MakeShared<FTool_GetComponent>());
		Registry.Register(MakeShared<FTool_SetComponentProperty>());
		Registry.Register(MakeShared<FTool_AddComponent>());
		Registry.Register(MakeShared<FTool_RemoveComponent>());
		Registry.Register(MakeShared<FTool_ReparentComponent>());
		Registry.Register(MakeShared<FTool_RenameComponent>());
		Registry.Register(MakeShared<FTool_DuplicateComponent>());
	}
}
