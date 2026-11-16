#include "PCGGraphSettings.h"

#if WITH_UEAI_PCG_GRAPH
#include "Algo/Find.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Metadata/PCGAttributePropertySelector.h"
#include "Metadata/PCGMetadataTypesConstantStruct.h"
#include "PCGSettings.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UEAIIntegrationTools::PCGGraphSettings
{
namespace
{
using FValue = TSharedPtr<FJsonValue>;
using FValues = TArray<FValue>;

constexpr int32 MaxChanges = 16;
constexpr int32 MaxOptions = 32;
constexpr int32 MaxStringLength = 1024;
constexpr int32 MaxNameLength = 256;
constexpr int32 MaxPathLength = 128;
constexpr int32 MaxPathDepth = 8;
constexpr int32 MaxReadFields = 512;
constexpr int32 MaxSettingsChars = 64 * 1024;
constexpr int32 MaxValueEntries = 4096;
constexpr EPropertyFlags NonPersistentFlags = CPF_Transient | CPF_Deprecated | CPF_SkipSerialization;

struct FValueBudget
{
	int32 RemainingEntries = MaxValueEntries;
	int32 RemainingChars = MaxSettingsChars;

	bool Visit(int32 Depth)
	{
		return Depth <= MaxPathDepth && --RemainingEntries >= 0;
	}

	bool AddText(const FString& Text)
	{
		RemainingChars -= Text.Len();
		return RemainingChars >= 0;
	}
};

struct FConstantField
{
	const TCHAR* Type;
	const TCHAR* ValueField;
};

constexpr FConstantField ConstantFields[] =
{
	{TEXT("Boolean"), TEXT("BoolValue")},
	{TEXT("Integer32"), TEXT("Int32Value")},
	{TEXT("Double"), TEXT("DoubleValue")},
	{TEXT("String"), TEXT("StringValue")},
	{TEXT("Name"), TEXT("NameValue")}
};

struct FField
{
	TArray<FProperty*, TInlineAllocator<MaxPathDepth>> Properties;

	FProperty* Leaf() const { return Properties.Last(); }

	void* Address(const UPCGSettings& Settings, int32 Count) const
	{
		void* Value = Properties[0]->ContainerPtrToValuePtr<void>(const_cast<UPCGSettings*>(&Settings));
		for (int32 Index = 1; Index < Count; ++Index)
		{
			Value = Properties[Index]->ContainerPtrToValuePtr<void>(Value);
		}
		return Value;
	}

	void* Address(const UPCGSettings& Settings) const
	{
		return Address(Settings, Properties.Num());
	}

	void BuildChain(FEditPropertyChain& Chain) const
	{
		for (FProperty* Property : Properties)
		{
			Chain.AddTail(Property);
		}
		Chain.SetActivePropertyNode(Leaf());
		Chain.SetActiveMemberPropertyNode(Properties[0]);
	}

	bool IsConstantMember() const
	{
		return Properties.Num() > 1 && CastFieldChecked<FStructProperty>(Properties[Properties.Num() - 2])->Struct
			== FPCGMetadataTypesConstantStruct::StaticStruct();
	}
};

bool IsInstanceProperty(const FProperty& Property)
{
	return Property.ArrayDim == 1 && Property.HasAnyPropertyFlags(CPF_Edit)
		&& !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance | NonPersistentFlags);
}

struct FPendingValue
{
	explicit FPendingValue(FField InField)
		: Field(InField), Storage(Field.Leaf()->AllocateAndInitializeValue())
	{
	}

	~FPendingValue()
	{
		Field.Leaf()->DestroyAndFreeValue(Storage);
	}

	FField Field;
	void* Storage;
};

bool ReadText(const FValue& Value, FString& Text, int32 MaxLength)
{
	return Value && Value->Type == EJson::String && Value->TryGetString(Text) && Text.Len() <= MaxLength
		&& FCString::Strlen(*Text) == Text.Len();
}

const UScriptStruct* SelectorStruct(const FProperty& Property)
{
	const FStructProperty* Struct = CastField<FStructProperty>(&Property);
	if (Struct && (Struct->Struct == FPCGAttributePropertyInputSelector::StaticStruct()
		|| Struct->Struct == FPCGAttributePropertyOutputSelector::StaticStruct()
		|| Struct->Struct == FPCGAttributePropertyOutputNoSourceSelector::StaticStruct()))
	{
		return Struct->Struct;
	}
	return nullptr;
}

const UEnum* GetEnum(const FProperty& Property, const FNumericProperty*& Number)
{
	if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
	{
		Number = Enum->GetUnderlyingProperty();
		return Enum->GetEnum();
	}
	if (const FByteProperty* Byte = CastField<FByteProperty>(&Property))
	{
		Number = Byte;
		return Byte->Enum;
	}
	return nullptr;
}

bool IsSupportedScalar(const FProperty& Property)
{
	const FNumericProperty* Number = nullptr;
	if (CastField<FBoolProperty>(&Property) || CastField<FIntProperty>(&Property)
		|| CastField<FDoubleProperty>(&Property) || CastField<FStrProperty>(&Property)
		|| CastField<FNameProperty>(&Property) || GetEnum(Property, Number) || SelectorStruct(Property))
	{
		return true;
	}
	return false;
}

bool VisitStructMembers(const UScriptStruct& Struct, TFunctionRef<bool(FProperty&)> Visit)
{
	for (TFieldIterator<FProperty> It(&Struct); It; ++It)
	{
		FProperty& Member = **It;
		if (Member.HasAnyPropertyFlags(NonPersistentFlags))
		{
			continue;
		}
		if (!IsInstanceProperty(Member) || !Visit(Member))
		{
			return false;
		}
	}
	return true;
}

bool IsSupportedWholeValue(const FProperty& Property, int32 Depth, FValueBudget& Budget)
{
	if (!Budget.Visit(Depth))
	{
		return false;
	}
	if (IsSupportedScalar(Property))
	{
		return true;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		return CastField<FStrProperty>(Array->Inner)
			|| (CastField<FStructProperty>(Array->Inner) && IsSupportedWholeValue(*Array->Inner, Depth + 1, Budget));
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		const UScriptStruct::ICppStructOps* Ops = Struct->Struct->GetCppStructOps();
		if (Ops && (Ops->HasSerializer() || Ops->HasStructuredSerializer()))
		{
			return false;
		}
		return VisitStructMembers(*Struct->Struct, [Depth, &Budget](FProperty& Member)
		{
			return IsSupportedWholeValue(Member, Depth + 1, Budget);
		});
	}
	return false;
}

bool IsSupportedLeaf(const FProperty& Property, int32 Depth)
{
	if (IsSupportedScalar(Property))
	{
		return true;
	}
	FValueBudget Budget;
	return CastField<FArrayProperty>(&Property) && IsSupportedWholeValue(Property, Depth, Budget);
}

const UScriptStruct* TraversableStruct(const FField& Field)
{
	const FStructProperty* Struct = CastField<FStructProperty>(Field.Leaf());
	if (!Struct || SelectorStruct(*Struct) || Field.IsConstantMember())
	{
		return nullptr;
	}
	return Struct->Struct;
}

bool Resolve(const UClass& Class, const FString& Path, FField& Field)
{
	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("."), false);
	if (Path.Len() > MaxPathLength || Segments.IsEmpty() || Segments.Num() > MaxPathDepth)
	{
		return false;
	}
	const UStruct* Container = &Class;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		const FString& Name = Segments[Index];
		FProperty* Property = FindFProperty<FProperty>(Container, FName(*Name, FNAME_Find));
		if (!Property || !Property->GetName().Equals(Name, ESearchCase::CaseSensitive) || !IsInstanceProperty(*Property))
		{
			return false;
		}
		Field.Properties.Add(Property);
		if (Index == Segments.Num() - 1)
		{
			return IsSupportedLeaf(*Property, Field.Properties.Num());
		}
		Container = TraversableStruct(Field);
		if (!Container)
		{
			return false;
		}
	}
	return false;
}

bool IsSwitchOptions(const FProperty& Property)
{
	return Property.GetFName() == TEXT("StringOptions")
		&& Property.GetOwnerStruct()->GetPathName() == TEXT("/Script/PCG.PCGSwitchSettings");
}

bool SetValue(const FProperty& Property, void* Address, const FValue& Value, int32 Depth, FValueBudget& Budget)
{
	if (!Value || !Budget.Visit(Depth))
	{
		return false;
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
	{
		bool bValue = false;
		if (Value->Type != EJson::Boolean || !Value->TryGetBool(bValue))
		{
			return false;
		}
		Bool->SetPropertyValue(Address, bValue);
		return true;
	}

	if (const FNumericProperty* Number = nullptr; const UEnum* Enum = GetEnum(Property, Number))
	{
		FString Name;
		if (!ReadText(Value, Name, MaxNameLength))
		{
			return false;
		}
		const int32 Index = Enum->GetIndexByNameString(Name);
		if (Index == INDEX_NONE || !Enum->GetNameStringByIndex(Index).Equals(Name, ESearchCase::CaseSensitive)
			|| Name.EndsWith(TEXT("_MAX")) || Enum->HasMetaData(TEXT("Hidden"), Index))
		{
			return false;
		}
		Number->SetIntPropertyValue(Address, Enum->GetValueByIndex(Index));
		return true;
	}

	if (const FIntProperty* Int = CastField<FIntProperty>(&Property))
	{
		double Number = 0;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number)
			|| Number < MIN_int32 || Number > MAX_int32 || FMath::TruncToDouble(Number) != Number)
		{
			return false;
		}
		Int->SetPropertyValue(Address, static_cast<int32>(Number));
		return true;
	}
	if (const FDoubleProperty* Double = CastField<FDoubleProperty>(&Property))
	{
		double Number = 0;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
		{
			return false;
		}
		Double->SetPropertyValue(Address, Number);
		return true;
	}
	if (const FStrProperty* String = CastField<FStrProperty>(&Property))
	{
		FString& Text = *String->GetPropertyValuePtr(Address);
		return ReadText(Value, Text, MaxStringLength) && Budget.AddText(Text);
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
	{
		FString Text;
		if (!ReadText(Value, Text, MaxNameLength) || !Budget.AddText(Text))
		{
			return false;
		}
		Name->SetPropertyValue(Address, FName(Text));
		return true;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		const FStrProperty* Inner = CastField<FStrProperty>(Array->Inner);
		const FValues* Options = nullptr;
		if (!Value->TryGetArray(Options) || Options->Num() > MaxOptions)
		{
			return false;
		}
		FScriptArrayHelper Items(Array, Address);
		Items.EmptyValues(Options->Num());
		if (!Inner)
		{
			if (!CastField<FStructProperty>(Array->Inner))
			{
				return false;
			}
			for (const FValue& Option : *Options)
			{
				if (!SetValue(*Array->Inner, Items.GetRawPtr(Items.AddValue()), Option, Depth + 1, Budget))
				{
					return false;
				}
			}
			return true;
		}
		const bool bSwitchOptions = IsSwitchOptions(Property);
		TSet<FName> Labels{FName(TEXT("Default"))};
		for (const FValue& Option : *Options)
		{
			FString Text;
			if (!ReadText(Option, Text, MaxNameLength) || !Budget.Visit(Depth) || !Budget.AddText(Text))
			{
				return false;
			}
			if (bSwitchOptions)
			{
				const FName Label(Text);
				if (Label.IsNone() || Labels.Contains(Label))
				{
					return false;
				}
				Labels.Add(Label);
			}
			Inner->SetPropertyValue(Items.GetRawPtr(Items.AddValue()), Text);
		}
		return true;
	}

	if (const UScriptStruct* Struct = SelectorStruct(Property))
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Selection;
		FString Name;
		if (!Value->TryGetObject(Object) || (*Object)->Values.Num() != 2
			|| !ReadText((*Object)->TryGetField(TEXT("selection")), Selection, MaxNameLength)
			|| !Selection.Equals(TEXT("attribute"), ESearchCase::CaseSensitive)
			|| !ReadText((*Object)->TryGetField(TEXT("name")), Name, MaxNameLength) || Name.IsEmpty())
		{
			return false;
		}
		if (Name.StartsWith(TEXT("@")))
		{
			const bool bInput = Struct == FPCGAttributePropertyInputSelector::StaticStruct();
			const bool bOutput = Struct == FPCGAttributePropertyOutputSelector::StaticStruct();
			if (!(bInput && (Name == TEXT("@Last") || Name == TEXT("@LastCreated")))
				&& !(bOutput && Name == TEXT("@Source")))
			{
				return false;
			}
		}
		FPCGAttributePropertySelector* Selector = nullptr;
		if (Struct == FPCGAttributePropertyInputSelector::StaticStruct())
		{
			Selector = static_cast<FPCGAttributePropertyInputSelector*>(Address);
		}
		else if (Struct == FPCGAttributePropertyOutputSelector::StaticStruct())
		{
			Selector = static_cast<FPCGAttributePropertyOutputSelector*>(Address);
		}
		else
		{
			Selector = static_cast<FPCGAttributePropertyOutputNoSourceSelector*>(Address);
		}
		Selector->SetAttributeName(FName(Name));
		return !Selector->GetAttributeName().IsNone() && Selector->IsValid() && Budget.AddText(Name);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		if (!Value->TryGetObject(Object))
		{
			return false;
		}
		int32 Count = 0;
		const bool bComplete = VisitStructMembers(*Struct->Struct, [&](FProperty& Member)
		{
			++Count;
			const FString Name = Member.GetName();
			const auto* Pair = Algo::FindByPredicate((*Object)->Values, [&Name](const auto& Entry)
			{
				return Entry.Key.Equals(Name, ESearchCase::CaseSensitive);
			});
			return Pair && Budget.AddText(Name)
				&& SetValue(Member, Member.ContainerPtrToValuePtr<void>(Address), Pair->Value, Depth + 1, Budget);
		});
		return bComplete && Count == (*Object)->Values.Num();
	}
	return false;
}

FValue ReadValue(const FProperty& Property, const void* Address, int32 Depth, FValueBudget& Budget)
{
	if (!Budget.Visit(Depth))
	{
		return nullptr;
	}
	if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
	{
		return MakeShared<FJsonValueBoolean>(Bool->GetPropertyValue(Address));
	}
	if (const FNumericProperty* Number = nullptr; const UEnum* Enum = GetEnum(Property, Number))
	{
		return MakeShared<FJsonValueString>(Enum->GetNameStringByValue(Number->GetSignedIntPropertyValue(Address)));
	}
	if (const FIntProperty* Int = CastField<FIntProperty>(&Property))
	{
		return MakeShared<FJsonValueNumber>(Int->GetPropertyValue(Address));
	}
	if (const FDoubleProperty* Double = CastField<FDoubleProperty>(&Property))
	{
		const double Value = Double->GetPropertyValue(Address);
		return FMath::IsFinite(Value) ? FValue(MakeShared<FJsonValueNumber>(Value)) : FValue(MakeShared<FJsonValueNull>());
	}
	if (const FStrProperty* String = CastField<FStrProperty>(&Property))
	{
		const FString& Value = *String->GetPropertyValuePtr(Address);
		return Budget.AddText(Value) ? FValue(MakeShared<FJsonValueString>(Value)) : nullptr;
	}
	if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
	{
		FString Text = Name->GetPropertyValue(Address).ToString();
		return Budget.AddText(Text) ? FValue(MakeShared<FJsonValueString>(MoveTemp(Text))) : nullptr;
	}
	if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
	{
		FScriptArrayHelper Items(Array, Address);
		if (Items.Num() > MaxReadFields)
		{
			return nullptr;
		}
		const int32 ElementDepth = Depth + (CastField<FStructProperty>(Array->Inner) ? 1 : 0);
		FValues Values;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			FValue Value = ReadValue(*Array->Inner, Items.GetRawPtr(Index), ElementDepth, Budget);
			if (!Value)
			{
				return nullptr;
			}
			Values.Add(MoveTemp(Value));
		}
		return MakeShared<FJsonValueArray>(MoveTemp(Values));
	}
	if (const UScriptStruct* Struct = SelectorStruct(Property))
	{
		const FPCGAttributePropertySelector* Selector = nullptr;
		if (Struct == FPCGAttributePropertyInputSelector::StaticStruct())
		{
			Selector = static_cast<const FPCGAttributePropertyInputSelector*>(Address);
		}
		else if (Struct == FPCGAttributePropertyOutputSelector::StaticStruct())
		{
			Selector = static_cast<const FPCGAttributePropertyOutputSelector*>(Address);
		}
		else
		{
			Selector = static_cast<const FPCGAttributePropertyOutputNoSourceSelector*>(Address);
		}
		const TCHAR* Kind = TEXT("attribute");
		switch (Selector->GetSelection())
		{
		case EPCGAttributePropertySelection::PointProperty:
			Kind = TEXT("pointProperty");
			break;
		case EPCGAttributePropertySelection::ExtraProperty:
			Kind = TEXT("extraProperty");
			break;
		default:
			break;
		}
		const auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("selection"), Kind);
		FString SelectorName = Selector->GetName().ToString();
		if (!Budget.AddText(SelectorName))
		{
			return nullptr;
		}
		Json->SetStringField(TEXT("name"), MoveTemp(SelectorName));
		if (!Selector->GetExtraNames().IsEmpty())
		{
			if (Selector->GetExtraNames().Num() > MaxReadFields)
			{
				return nullptr;
			}
			FValues Extra;
			for (const FString& Name : Selector->GetExtraNames())
			{
				if (!Budget.Visit(Depth) || !Budget.AddText(Name))
				{
					return nullptr;
				}
				Extra.Add(MakeShared<FJsonValueString>(Name));
			}
			Json->SetArrayField(TEXT("extraNames"), MoveTemp(Extra));
		}
		return MakeShared<FJsonValueObject>(Json);
	}
	if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
	{
		const auto Object = MakeShared<FJsonObject>();
		const bool bComplete = VisitStructMembers(*Struct->Struct, [&](FProperty& Member)
		{
			const FString Name = Member.GetName();
			FValue Value = ReadValue(Member, Member.ContainerPtrToValuePtr<void>(Address), Depth + 1, Budget);
			if (!Value || !Budget.AddText(Name))
			{
				return false;
			}
			Object->SetField(Name, MoveTemp(Value));
			return true;
		});
		if (bComplete)
		{
			return MakeShared<FJsonValueObject>(Object);
		}
	}
	return nullptr;
}

bool ReadFields(const UPCGSettings& Settings, const UStruct& Container, const FString& Prefix,
	FField& Field, FJsonObject& Result)
{
	for (TFieldIterator<FProperty> It(&Container); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsInstanceProperty(*Property))
		{
			continue;
		}
		Field.Properties.Add(Property);
		const FString Path = Prefix + Property->GetName();
		if (IsSupportedLeaf(*Property, Field.Properties.Num()))
		{
			if (Field.Properties.Num() > MaxPathDepth || Path.Len() > MaxPathLength || Result.Values.Num() >= MaxReadFields)
			{
				return false;
			}
			FValueBudget Budget;
			FValue Value = ReadValue(*Property, Field.Address(Settings), Field.Properties.Num(), Budget);
			if (!Value)
			{
				return false;
			}
			Result.SetField(Path, MoveTemp(Value));
		}
		else if (const UScriptStruct* Struct = TraversableStruct(Field))
		{
			if (Field.Properties.Num() > MaxPathDepth || !ReadFields(Settings, *Struct, Path + TEXT("."), Field, Result))
			{
				return false;
			}
		}
		Field.Properties.Pop();
	}
	return true;
}

bool CanEditChain(const UPCGSettings& Settings, const FField& Field)
{
	FEditPropertyChain Chain;
	Field.BuildChain(Chain);
	return static_cast<const UObject&>(Settings).CanEditChange(Chain);
}

bool CanReplaceArray(const UPCGSettings& Settings, FField& Field, const FArrayProperty& Array,
	const void* Current, const void* Replacement);

bool CanReplaceStruct(const UPCGSettings& Settings, FField& Field, const FStructProperty& Struct,
	const void* Current, const void* Replacement)
{
	if (SelectorStruct(Struct))
	{
		return true;
	}
	return VisitStructMembers(*Struct.Struct, [&](FProperty& Member)
	{
		// The property editor excludes a container's synthetic Inner from the edit chain.
		Field.Properties.Add(&Member);
		bool bEditable = CanEditChain(Settings, Field);
		if (bEditable)
		{
			const void* Before = Member.ContainerPtrToValuePtr<void>(Current);
			const void* After = Member.ContainerPtrToValuePtr<void>(Replacement);
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Member))
			{
				bEditable = CanReplaceArray(Settings, Field, *Array, Before, After);
			}
			else if (const FStructProperty* Child = CastField<FStructProperty>(&Member))
			{
				bEditable = CanReplaceStruct(Settings, Field, *Child, Before, After);
			}
		}
		Field.Properties.Pop();
		return bEditable;
	});
}

bool CanReplaceArray(const UPCGSettings& Settings, FField& Field, const FArrayProperty& Array,
	const void* Current, const void* Replacement)
{
	FScriptArrayHelper Before(&Array, Current);
	FScriptArrayHelper After(&Array, Replacement);
	if (Array.HasAnyPropertyFlags(CPF_EditFixedSize) && Before.Num() != After.Num())
	{
		return false;
	}
	const FStructProperty* Inner = CastField<FStructProperty>(Array.Inner);
	if (!Inner || SelectorStruct(*Inner))
	{
		return true;
	}
	FStructOnScope DefaultElement;
	if (After.Num() > Before.Num())
	{
		DefaultElement.Initialize(Inner->Struct.Get());
	}
	for (int32 Index = 0; Index < After.Num(); ++Index)
	{
		const void* Existing = Index < Before.Num() ? Before.GetRawPtr(Index) : DefaultElement.GetStructMemory();
		if (!CanReplaceStruct(Settings, Field, *Inner, Existing, After.GetRawPtr(Index)))
		{
			return false;
		}
	}
	return true;
}

bool CanEditValue(const UPCGSettings& Settings, const FPendingValue& Item)
{
	FField Field = Item.Field;
	if (!CanEditChain(Settings, Field))
	{
		return false;
	}
	const FProperty* Leaf = Field.Leaf();
	if (const FArrayProperty* Array = CastField<FArrayProperty>(Leaf))
	{
		return CanReplaceArray(Settings, Field, *Array, Field.Address(Settings), Item.Storage);
	}
	if (Field.IsConstantMember() && Leaf->GetFName() == GET_MEMBER_NAME_CHECKED(FPCGMetadataTypesConstantStruct, Type))
	{
		const auto* Constant = static_cast<const FPCGMetadataTypesConstantStruct*>(Field.Address(Settings, Field.Properties.Num() - 1));
		return Constant->bAllowsTypeChange || Leaf->Identical(Field.Address(Settings), Item.Storage);
	}
	return true;
}

bool Stage(const UClass& Class, const UPCGSettings* Instance, const FValues& Changes,
	TArray<TUniquePtr<FPendingValue>>& Pending, FString& Message)
{
	if (Changes.IsEmpty() || Changes.Num() > MaxChanges)
	{
		Message = TEXT("A Settings edit requires between 1 and 16 changes.");
		return false;
	}
	TSet<FString> Paths;
	TMap<FString, int32> ConstantGroups;
	TMap<FString, FString> ConstantTypes;
	for (const FValue& Change : Changes)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString Path;
		if (!Change->TryGetObject(Entry) || (*Entry)->Values.Num() != 2
			|| !(*Entry)->TryGetStringField(TEXT("path"), Path) || Paths.Contains(Path))
		{
			Message = TEXT("Settings path is missing or duplicated.");
			return false;
		}
		const FValue Value = (*Entry)->TryGetField(TEXT("value"));
		FField Field;
		if (!Value || !Resolve(Class, Path, Field))
		{
			Message = TEXT("Settings path is unavailable, read-only, or uses an unsupported type: ") + Path;
			return false;
		}
		TUniquePtr<FPendingValue> Item = MakeUnique<FPendingValue>(Field);
		FValueBudget Budget;
		if (!SetValue(*Field.Leaf(), Item->Storage, Value, Field.Properties.Num(), Budget))
		{
			Message = TEXT("Invalid Settings value for ") + Path;
			return false;
		}
		if (Instance && !CanEditValue(*Instance, *Item))
		{
			Message = TEXT("The Settings instance does not allow this change: ") + Path;
			return false;
		}
		if (Field.IsConstantMember())
		{
			const FString Prefix = Path.LeftChop(Field.Leaf()->GetName().Len());
			++ConstantGroups.FindOrAdd(Prefix);
			if (Field.Leaf()->GetFName() == GET_MEMBER_NAME_CHECKED(FPCGMetadataTypesConstantStruct, Type))
			{
				Value->TryGetString(ConstantTypes.FindOrAdd(Prefix));
			}
		}
		Paths.Add(Path);
		Pending.Add(MoveTemp(Item));
	}

	for (const TPair<FString, int32>& Group : ConstantGroups)
	{
		const FString Type = ConstantTypes.FindRef(Group.Key);
		const FConstantField* Constant = Algo::FindByPredicate(ConstantFields, [&Type](const FConstantField& Item) { return Type == Item.Type; });
		if (!Constant || Group.Value != 2 || !Paths.Contains(Group.Key + Constant->ValueField))
		{
			Message = TEXT("Change Type and exactly its matching value field together in ") + Group.Key.LeftChop(1);
			return false;
		}
	}
	return true;
}

void NotifyChange(UObject& Settings, const FField& Field)
{
	FEditPropertyChain Chain;
	Field.BuildChain(Chain);
	FPropertyChangedEvent Event(Field.Leaf(), EPropertyChangeType::ValueSet);
	Event.SetActiveMemberProperty(Field.Properties[0]);
	FPropertyChangedChainEvent ChainEvent(Chain, Event);
	Settings.PostEditChangeChainProperty(ChainEvent);
}
}

bool Read(const UPCGSettings& Settings, TSharedPtr<FJsonObject>& Values, int32& RemainingChars)
{
	const auto Result = MakeShared<FJsonObject>();
	FField Field;
	if (!ReadFields(Settings, *Settings.GetClass(), FString(), Field, *Result))
	{
		return false;
	}
	if (!Result->Values.IsEmpty())
	{
		const int32 Size = UEAIIntegration::Infrastructure::CanonicalizeJson(Result).Len();
		if (Size > MaxSettingsChars || Size > RemainingChars)
		{
			return false;
		}
		RemainingChars -= Size;
		Values = Result;
	}
	return true;
}

bool Validate(const FString& ClassPath, const UPCGSettings* Instance, const FValues& Changes, FString& Message)
{
	const UClass* Class = Instance ? Instance->GetClass() : FindObject<UClass>(nullptr, *ClassPath);
	if (!Class || !Class->IsChildOf(UPCGSettings::StaticClass()) || !Class->GetPathName().Equals(ClassPath, ESearchCase::CaseSensitive))
	{
		Message = TEXT("The expected PCG Settings class is unavailable or does not match the instance.");
		return false;
	}
	TArray<TUniquePtr<FPendingValue>> Pending;
	return Stage(*Class, Instance, Changes, Pending, Message);
}

bool Apply(UPCGSettings& Settings, const FValues& Changes, TArray<FString>& EditedPaths, FString& Message)
{
	TArray<TUniquePtr<FPendingValue>> Pending;
	if (!Stage(*Settings.GetClass(), &Settings, Changes, Pending, Message))
	{
		return false;
	}

	UObject& Edited = Settings;
	Edited.SetFlags(RF_Transactional);
	Edited.Modify();
	for (const TUniquePtr<FPendingValue>& Item : Pending)
	{
		FEditPropertyChain Chain;
		Item->Field.BuildChain(Chain);
		Edited.PreEditChange(Chain);
	}
	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		const FPendingValue& Item = *Pending[Index];
		Item.Field.Leaf()->CopyCompleteValue(Item.Field.Address(Settings), Item.Storage);
		EditedPaths.AddUnique(Changes[Index]->AsObject()->GetStringField(TEXT("path")));
	}
	for (const TUniquePtr<FPendingValue>& Item : Pending)
	{
		NotifyChange(Edited, Item->Field);
	}
	for (const TUniquePtr<FPendingValue>& Item : Pending)
	{
		if (!Item->Field.Leaf()->Identical(Item->Field.Address(Settings), Item->Storage))
		{
			Message = TEXT("Native Settings notification did not preserve the requested value.");
			return false;
		}
	}
	return true;
}

bool RefreshAfterRestore(UPCGSettings& Settings, const TArray<FString>& EditedPaths)
{
	for (const FString& Path : EditedPaths)
	{
		FField Field;
		if (!Resolve(*Settings.GetClass(), Path, Field))
		{
			return false;
		}
		NotifyChange(Settings, Field);
	}
	if (EditedPaths.Contains(TEXT("SelectionMode")) && !EditedPaths.Contains(TEXT("StringOptions"))
		&& Settings.GetClass()->GetPathName() == TEXT("/Script/PCG.PCGSwitchSettings"))
	{
		// The Switch rebuilds its cached labels on the options notification.
		FField Options;
		if (!Resolve(*Settings.GetClass(), TEXT("StringOptions"), Options))
		{
			return false;
		}
		NotifyChange(Settings, Options);
	}
	return true;
}
}
#endif
