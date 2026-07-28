#include "Workflow/UEWorkflowRuntime.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ActorComponent.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Compatibility/UEVersionCompat.h"
#include "Infrastructure/Sha256.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/ITransaction.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "WidgetBlueprint.h"

#ifndef UE_AI_INTEGRATION_VERSION
#define UE_AI_INTEGRATION_VERSION "unknown"
#endif

namespace UEAIIntegration::Workflow
{
namespace
{
constexpr TCHAR WorkflowContextName[] = TEXT("UEWorkflow");
constexpr int32 MaxPreparedPlanCacheEntries = 64;
constexpr double PreparedPlanCacheTtlSeconds = 10.0 * 60.0;

FString CurrentPluginVersion()
{
	return UTF8_TO_TCHAR(UE_AI_INTEGRATION_VERSION);
}

FString FromUtf8(const std::string& Value)
{
	return UTF8_TO_TCHAR(Value.c_str());
}

FString ToAssetObjectPath(const FString& AssetPath)
{
	if (AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	return PackageName.IsEmpty() || AssetName.IsEmpty()
		? AssetPath
		: PackageName + TEXT(".") + AssetName;
}

UObject* LoadAssetWithoutLogging(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return nullptr;
	}
	const FString ObjectPath = ToAssetObjectPath(AssetPath);
	if (UObject* Loaded = FindObject<UObject>(nullptr, *ObjectPath))
	{
		if (IsValid(Loaded) && Loaded->IsAsset())
		{
			return Loaded;
		}
	}
	const FAssetData AssetData =
		IAssetRegistry::GetChecked().GetAssetByObjectPath(
			FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid())
	{
		return nullptr;
	}
	if (UObject* Loaded = AssetData.FastGetAsset(false))
	{
		return IsValid(Loaded) && Loaded->IsAsset() ? Loaded : nullptr;
	}
	if (!FPackageName::DoesPackageExist(AssetData.PackageName.ToString()))
	{
		return nullptr;
	}
	UObject* Asset = AssetData.GetAsset();
	return IsValid(Asset) && Asset->IsAsset() ? Asset : nullptr;
}

bool AssetExistsWithoutLogging(const FString& AssetPath)
{
	if (AssetPath.IsEmpty())
	{
		return false;
	}
	const FString ObjectPath = ToAssetObjectPath(AssetPath);
	if (UObject* Loaded = FindObject<UObject>(nullptr, *ObjectPath))
	{
		if (IsValid(Loaded) && Loaded->IsAsset())
		{
			return true;
		}
	}
	const FAssetData AssetData =
		IAssetRegistry::GetChecked().GetAssetByObjectPath(
			FSoftObjectPath(ObjectPath));
	if (!AssetData.IsValid())
	{
		return false;
	}
	if (UObject* Loaded = AssetData.FastGetAsset(false))
	{
		return IsValid(Loaded) && Loaded->IsAsset();
	}
	return FPackageName::DoesPackageExist(
		AssetData.PackageName.ToString());
}

std::string ToUtf8(const FString& Value)
{
	return TCHAR_TO_UTF8(*Value);
}

TArray<FString> DecodeJsonPointer(const FString& Pointer, FString& OutError)
{
	TArray<FString> Segments;
	if (Pointer.IsEmpty())
	{
		return Segments;
	}
	if (!Pointer.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("JSON Pointer '%s' must be empty or start with '/'."),
			*Pointer);
		return {};
	}

	Pointer.Mid(1).ParseIntoArray(Segments, TEXT("/"), false);
	for (FString& Segment : Segments)
	{
		FString Decoded;
		for (int32 Index = 0; Index < Segment.Len(); ++Index)
		{
			if (Segment[Index] != TCHAR('~'))
			{
				Decoded.AppendChar(Segment[Index]);
				continue;
			}
			if (Index + 1 >= Segment.Len())
			{
				OutError = FString::Printf(
					TEXT("JSON Pointer '%s' has an incomplete '~' escape."),
					*Pointer);
				return {};
			}
			const TCHAR Escaped = Segment[++Index];
			if (Escaped == TCHAR('0'))
			{
				Decoded.AppendChar(TCHAR('~'));
			}
			else if (Escaped == TCHAR('1'))
			{
				Decoded.AppendChar(TCHAR('/'));
			}
			else
			{
				OutError = FString::Printf(
					TEXT("JSON Pointer '%s' has invalid escape '~%c'."),
					*Pointer,
					Escaped);
				return {};
			}
		}
		Segment = MoveTemp(Decoded);
	}
	return Segments;
}

bool TryParseArrayIndex(const FString& Segment, int32& OutIndex)
{
	if (Segment.IsEmpty())
	{
		return false;
	}
	for (const TCHAR Character : Segment)
	{
		if (!FChar::IsDigit(Character))
		{
			return false;
		}
	}
	OutIndex = FCString::Atoi(*Segment);
	return OutIndex >= 0;
}

bool ResolveJsonPointer(
	const TSharedPtr<FJsonObject>& Root,
	const FString& Pointer,
	TSharedPtr<FJsonValue>& OutValue,
	FString& OutError)
{
	if (!Root.IsValid())
	{
		OutError = TEXT("Cannot resolve a JSON Pointer against a null object.");
		return false;
	}

	if (Pointer.IsEmpty())
	{
		OutValue = MakeShared<FJsonValueObject>(Root);
		return true;
	}

	TArray<FString> Segments = DecodeJsonPointer(Pointer, OutError);
	if (!OutError.IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(Root);
	for (const FString& Segment : Segments)
	{
		if (!Current.IsValid())
		{
			OutError = FString::Printf(
				TEXT("JSON Pointer '%s' traversed a null value."),
				*Pointer);
			return false;
		}

		if (Current->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Current->AsObject();
			const TSharedPtr<FJsonValue>* Field = Object.IsValid()
				? Object->Values.Find(Segment)
				: nullptr;
			if (!Field || !Field->IsValid())
			{
				OutError = FString::Printf(
					TEXT("JSON Pointer '%s' could not find object field '%s'."),
					*Pointer,
					*Segment);
				return false;
			}
			Current = *Field;
			continue;
		}

		if (Current->Type == EJson::Array)
		{
			int32 ArrayIndex = INDEX_NONE;
			const TArray<TSharedPtr<FJsonValue>>& Array = Current->AsArray();
			if (!TryParseArrayIndex(Segment, ArrayIndex)
				|| !Array.IsValidIndex(ArrayIndex))
			{
				OutError = FString::Printf(
					TEXT("JSON Pointer '%s' has invalid array index '%s'."),
					*Pointer,
					*Segment);
				return false;
			}
			Current = Array[ArrayIndex];
			continue;
		}

		OutError = FString::Printf(
			TEXT("JSON Pointer '%s' traversed a non-container value at '%s'."),
			*Pointer,
			*Segment);
		return false;
	}

	OutValue = Current;
	return true;
}

bool SetJsonPointer(
	const TSharedPtr<FJsonObject>& Params,
	const FString& OriginalPointer,
	const TSharedPtr<FJsonValue>& Value,
	FString& OutError)
{
	FString Pointer = OriginalPointer;
	if (Pointer == TEXT("/params"))
	{
		OutError = TEXT("A binding cannot replace the complete params object.");
		return false;
	}
	if (Pointer.StartsWith(TEXT("/params/")))
	{
		Pointer.RightChopInline(7, false);
	}
	if (!Pointer.StartsWith(TEXT("/")))
	{
		OutError = FString::Printf(
			TEXT("Binding destination '%s' must target params with a JSON Pointer."),
			*OriginalPointer);
		return false;
	}

	TArray<FString> Segments = DecodeJsonPointer(Pointer, OutError);
	if (!OutError.IsEmpty() || Segments.IsEmpty())
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("A binding destination cannot replace the complete params object.");
		}
		return false;
	}

	TSharedPtr<FJsonObject> CurrentObject = Params;
	for (int32 Index = 0; Index < Segments.Num() - 1; ++Index)
	{
		const FString& Segment = Segments[Index];
		const TSharedPtr<FJsonValue>* Existing = CurrentObject->Values.Find(Segment);
		if (!Existing)
		{
			TSharedPtr<FJsonObject> Child = MakeShared<FJsonObject>();
			CurrentObject->SetObjectField(Segment, Child);
			CurrentObject = MoveTemp(Child);
			continue;
		}
		if (!Existing->IsValid() || (*Existing)->Type != EJson::Object)
		{
			OutError = FString::Printf(
				TEXT("Binding destination '%s' crosses non-object field '%s'."),
				*OriginalPointer,
				*Segment);
			return false;
		}
		CurrentObject = (*Existing)->AsObject();
	}

	CurrentObject->SetField(Segments.Last(), Value);
	return true;
}

void SplitAssetPath(
	const FString& AssetPath,
	FString& OutPackagePath,
	FString& OutAssetName)
{
	FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	if (PackageName.IsEmpty())
	{
		PackageName = AssetPath;
	}
	OutAssetName = FPackageName::GetShortName(PackageName);
	OutPackagePath = FPackageName::GetLongPackagePath(PackageName);
}

bool SetScopedString(
	const TSharedPtr<FJsonObject>& Params,
	const FString& Field,
	const FString& ScopeValue,
	FString& OutError)
{
	FString Existing;
	if (Params->TryGetStringField(Field, Existing))
	{
		if (Existing != ScopeValue)
		{
			OutError = FString::Printf(
				TEXT("Operation field '%s' targets '%s', outside workflow scope '%s'."),
				*Field,
				*Existing,
				*ScopeValue);
			return false;
		}
		return true;
	}
	Params->SetStringField(Field, ScopeValue);
	return true;
}

TSharedPtr<FJsonObject> MakeOperationRecord(
	const FString& Id,
	const FString& Type,
	const FString& Status,
	const TSharedPtr<FJsonObject>& Data = nullptr)
{
	TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
	Record->SetStringField(TEXT("id"), Id);
	Record->SetStringField(TEXT("type"), Type);
	Record->SetStringField(TEXT("status"), Status);
	if (Data.IsValid())
	{
		Record->SetObjectField(TEXT("data"), Data);
	}
	return Record;
}

void CollectWidgetLayouts(const TSharedPtr<FJsonObject>& Node,
                          TArray<TSharedPtr<FJsonValue>>& OutLayouts)
{
	if (!Node.IsValid())
	{
		return;
	}

	const TSharedPtr<FJsonObject>* Layout = nullptr;
	FString SlotClass;
	const bool bHasLayout =
	    Node->TryGetObjectField(TEXT("layout"), Layout) && Layout && Layout->IsValid();
	const bool bHasSlotClass =
	    Node->TryGetStringField(TEXT("slotClass"), SlotClass) && !SlotClass.IsEmpty();
	if (bHasLayout || bHasSlotClass)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		FString Name;
		FString ClassName;
		if (Node->TryGetStringField(TEXT("name"), Name))
		{
			Entry->SetStringField(TEXT("name"), Name);
		}
		if (Node->TryGetStringField(TEXT("class"), ClassName))
		{
			Entry->SetStringField(TEXT("class"), ClassName);
		}
		if (bHasSlotClass)
		{
			Entry->SetStringField(TEXT("slotClass"), SlotClass);
		}
		const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
		if (Node->TryGetObjectField(TEXT("widgetRef"), WidgetRef) && WidgetRef &&
		    WidgetRef->IsValid())
		{
			Entry->SetObjectField(TEXT("widgetRef"), *WidgetRef);
		}
		if (bHasLayout)
		{
			Entry->SetObjectField(TEXT("layout"), *Layout);
		}
		OutLayouts.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (Node->TryGetArrayField(TEXT("children"), Children) && Children)
	{
		for (const TSharedPtr<FJsonValue>& Child : *Children)
		{
			if (Child.IsValid() && Child->Type == EJson::Object)
			{
				CollectWidgetLayouts(Child->AsObject(), OutLayouts);
			}
		}
	}
}

TSharedPtr<FJsonValue> ProjectReadBackValue(const FString& Key,
                                            const TSharedPtr<FJsonObject>& Output)
{
	if (!Output.IsValid())
	{
		return MakeShared<FJsonValueNull>();
	}

	if (Key == TEXT("widgetTree"))
	{
		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		FString WidgetBlueprint;
		double Count = 0.0;
		if (Output->TryGetStringField(TEXT("widgetBp"), WidgetBlueprint))
		{
			Tree->SetStringField(TEXT("widgetBp"), WidgetBlueprint);
		}
		if (Output->TryGetNumberField(TEXT("count"), Count))
		{
			Tree->SetNumberField(TEXT("count"), Count);
		}
		const TSharedPtr<FJsonObject>* Root = nullptr;
		if (Output->TryGetObjectField(TEXT("root"), Root) && Root && Root->IsValid())
		{
			Tree->SetObjectField(TEXT("root"), *Root);
		}
		return MakeShared<FJsonValueObject>(Tree);
	}

	if (Key == TEXT("bindings"))
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		return Output->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings
		           ? MakeShared<FJsonValueArray>(*Bindings)
		           : MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>());
	}

	if (Key == TEXT("layout"))
	{
		TArray<TSharedPtr<FJsonValue>> Layouts;
		const TSharedPtr<FJsonObject>* Root = nullptr;
		if (Output->TryGetObjectField(TEXT("root"), Root) && Root && Root->IsValid())
		{
			CollectWidgetLayouts(*Root, Layouts);
		}
		return MakeShared<FJsonValueArray>(Layouts);
	}

	return MakeShared<FJsonValueObject>(Output);
}

bool IsConfirmWriteRisk(const TSharedPtr<FJsonObject>& Plan)
{
	const TSharedPtr<FJsonObject>* Risk = nullptr;
	if (!Plan.IsValid()
		|| !Plan->TryGetObjectField(TEXT("risk"), Risk)
		|| !Risk
		|| !Risk->IsValid())
	{
		return false;
	}
	FString Overall;
	return (*Risk)->TryGetStringField(TEXT("overall"), Overall)
		&& Overall == TEXT("confirmWrite");
}

bool IsTopTransaction(const FString& TransactionId)
{
	if (TransactionId.IsEmpty() || !GEditor || !GEditor->Trans)
	{
		return false;
	}
	FGuid Expected;
	if (!FGuid::Parse(TransactionId, Expected))
	{
		return false;
	}
	const FTransactionContext Context = GEditor->Trans->GetUndoContext(false);
	return Context.IsValid()
		&& Context.TransactionId == Expected
		&& Context.Context == WorkflowContextName;
}

bool IsCanonicalRunId(const FString& RunId)
{
	FGuid Parsed;
	return FGuid::Parse(RunId, Parsed)
		&& RunId == Parsed.ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString EscapeJsonPointerSegment(const FString& Segment)
{
	FString Escaped = Segment.Replace(TEXT("~"), TEXT("~0"));
	return Escaped.Replace(TEXT("/"), TEXT("~1"));
}

FString StableNodeId(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("<null>");
	}
	return Node->NodeGuid.IsValid()
		? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
		: Node->GetName();
}

FString StablePinId(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return TEXT("<null>");
	}
	return Pin->PinId.IsValid()
		? Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower)
		: Pin->PinName.ToString();
}

TSharedPtr<FJsonObject> CaptureGraphStructure(UEdGraph* Graph)
{
	TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
	if (!Graph)
	{
		return GraphJson;
	}

	GraphJson->SetStringField(TEXT("id"), Graph->GetName());
	GraphJson->SetStringField(TEXT("name"), Graph->GetName());
	GraphJson->SetStringField(TEXT("class"), Graph->GetClass()->GetName());
	GraphJson->SetStringField(
		TEXT("guid"),
		Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	TArray<UEdGraphNode*> Nodes = Graph->Nodes;
	Nodes.Remove(nullptr);
	Nodes.Sort(
		[](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return StableNodeId(&Left) < StableNodeId(&Right);
		});

	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> ConnectionValues;
	for (UEdGraphNode* Node : Nodes)
	{
		TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("id"), StableNodeId(Node));
		NodeJson->SetStringField(TEXT("name"), Node->GetName());
		NodeJson->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeJson->SetStringField(
			TEXT("title"),
			Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		NodeJson->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeJson->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeJson->SetStringField(TEXT("comment"), Node->NodeComment);

		TArray<UEdGraphPin*> Pins = Node->Pins;
		Pins.Remove(nullptr);
		Pins.Sort(
			[](const UEdGraphPin& Left, const UEdGraphPin& Right)
			{
				return StablePinId(&Left) < StablePinId(&Right);
			});
		TArray<TSharedPtr<FJsonValue>> PinValues;
		for (UEdGraphPin* Pin : Pins)
		{
			TSharedPtr<FJsonObject> PinJson = MakeShared<FJsonObject>();
			PinJson->SetStringField(TEXT("id"), StablePinId(Pin));
			PinJson->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinJson->SetStringField(
				TEXT("direction"),
				Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinJson->SetStringField(TEXT("default"), Pin->DefaultValue);
			PinValues.Add(MakeShared<FJsonValueObject>(PinJson));

			if (Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* LinkedNode = LinkedPin
					? LinkedPin->GetOwningNodeUnchecked()
					: nullptr;
				if (!LinkedPin || !LinkedNode)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Connection = MakeShared<FJsonObject>();
				const FString ConnectionId = FString::Printf(
					TEXT("%s/%s->%s/%s"),
					*StableNodeId(Node),
					*StablePinId(Pin),
					*StableNodeId(LinkedNode),
					*StablePinId(LinkedPin));
				Connection->SetStringField(TEXT("id"), ConnectionId);
				Connection->SetStringField(
					TEXT("fromNode"),
					StableNodeId(Node));
				Connection->SetStringField(
					TEXT("fromPin"),
					Pin->PinName.ToString());
				Connection->SetStringField(
					TEXT("toNode"),
					StableNodeId(LinkedNode));
				Connection->SetStringField(
					TEXT("toPin"),
					LinkedPin->PinName.ToString());
				ConnectionValues.Add(
					MakeShared<FJsonValueObject>(Connection));
			}
		}
		NodeJson->SetArrayField(TEXT("pins"), PinValues);
		NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	ConnectionValues.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			FString LeftId;
			FString RightId;
			if (Left.IsValid() && Left->Type == EJson::Object)
			{
				Left->AsObject()->TryGetStringField(TEXT("id"), LeftId);
			}
			if (Right.IsValid() && Right->Type == EJson::Object)
			{
				Right->AsObject()->TryGetStringField(TEXT("id"), RightId);
			}
			return LeftId < RightId;
		});
	GraphJson->SetArrayField(TEXT("nodes"), NodeValues);
	GraphJson->SetArrayField(TEXT("connections"), ConnectionValues);
	return GraphJson;
}

void FlattenStructureValue(
	const TSharedPtr<FJsonValue>& Value,
	const FString& Path,
	TMap<FString, FString>& OutValues)
{
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		OutValues.Add(Path, TEXT("null"));
		return;
	}

	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid() || Object->Values.IsEmpty())
		{
			OutValues.Add(Path, TEXT("{}"));
			return;
		}
		TArray<FString> Keys;
		Object->Values.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			FlattenStructureValue(
				Object->Values[Key],
				Path + TEXT("/") + EscapeJsonPointerSegment(Key),
				OutValues);
		}
		return;
	}

	if (Value->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
		if (Array.IsEmpty())
		{
			OutValues.Add(Path, TEXT("[]"));
			return;
		}
		for (int32 Index = 0; Index < Array.Num(); ++Index)
		{
			FString Segment = FString::FromInt(Index);
			if (Array[Index].IsValid()
				&& Array[Index]->Type == EJson::Object)
			{
				FString Id;
				if (Array[Index]->AsObject()->TryGetStringField(TEXT("id"), Id)
					&& !Id.IsEmpty())
				{
					Segment = Id;
				}
			}
			FlattenStructureValue(
				Array[Index],
				Path + TEXT("/") + EscapeJsonPointerSegment(Segment),
				OutValues);
		}
		return;
	}

	if (Value->Type == EJson::String)
	{
		OutValues.Add(Path, TEXT("string:") + Value->AsString());
	}
	else if (Value->Type == EJson::Boolean)
	{
		OutValues.Add(
			Path,
			Value->AsBool() ? TEXT("bool:true") : TEXT("bool:false"));
	}
	else if (Value->Type == EJson::Number)
	{
		OutValues.Add(
			Path,
			TEXT("number:") + FString::SanitizeFloat(Value->AsNumber()));
	}
	else
	{
		OutValues.Add(Path, TEXT("<unsupported>"));
	}
}

int32 GetObjectOuterDepth(const UObject* Object)
{
	int32 Depth = 0;
	for (const UObject* Outer = Object ? Object->GetOuter() : nullptr;
		Outer;
		Outer = Outer->GetOuter())
	{
		++Depth;
	}
	return Depth;
}

void AddOwnedObject(
	UObject* Object,
	TSet<UObject*>& Seen,
	TArray<UObject*>& OutObjects)
{
	if (!Object || Seen.Contains(Object))
	{
		return;
	}
	Seen.Add(Object);
	OutObjects.Add(Object);
}

TArray<UObject*> GatherDomainOwnedObjects(UObject* Asset)
{
	TArray<UObject*> Objects;
	TSet<UObject*> Seen;
	AddOwnedObject(Asset, Seen, Objects);
	if (!Asset)
	{
		return Objects;
	}

	ForEachObjectWithOuter(
		Asset,
		[&Seen, &Objects](UObject* Object)
		{
			AddOwnedObject(Object, Seen, Objects);
		},
		true);

	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			AddOwnedObject(Graph, Seen, Objects);
			if (!Graph)
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				AddOwnedObject(Node, Seen, Objects);
			}
		}
		if (Blueprint->SimpleConstructionScript)
		{
			AddOwnedObject(
				Blueprint->SimpleConstructionScript,
				Seen,
				Objects);
			for (USCS_Node* Node :
				Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				AddOwnedObject(Node, Seen, Objects);
				AddOwnedObject(
					Node ? Node->ComponentTemplate : nullptr,
					Seen,
					Objects);
			}
		}
	}

	if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset))
	{
		UClass* GeneratedClass = WidgetBlueprint->GeneratedClass.Get();
		AddOwnedObject(
			GeneratedClass ? GeneratedClass->GetDefaultObject() : nullptr,
			Seen,
			Objects);
		AddOwnedObject(WidgetBlueprint->WidgetTree, Seen, Objects);
		if (WidgetBlueprint->WidgetTree)
		{
			TArray<UWidget*> Widgets;
			WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
			Widgets.AddUnique(WidgetBlueprint->WidgetTree->RootWidget);
			for (UWidget* Widget : Widgets)
			{
				AddOwnedObject(Widget, Seen, Objects);
				AddOwnedObject(
					Widget ? Widget->Slot.Get() : nullptr,
					Seen,
					Objects);
			}
		}
	}

	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		AddOwnedObject(Material->MaterialGraph.Get(), Seen, Objects);
		if (Material->MaterialGraph)
		{
			for (UEdGraphNode* Node : Material->MaterialGraph.Get()->Nodes)
			{
				AddOwnedObject(Node, Seen, Objects);
			}
		}
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			AddOwnedObject(Expression, Seen, Objects);
		}
	}

	Objects.Sort(
		[](const UObject& Left, const UObject& Right)
		{
			const int32 LeftDepth = GetObjectOuterDepth(&Left);
			const int32 RightDepth = GetObjectOuterDepth(&Right);
			return LeftDepth == RightDepth
				? Left.GetPathName() < Right.GetPathName()
				: LeftDepth < RightDepth;
		});
	return Objects;
}

TArray<FWorkflowObjectMemorySnapshot> CaptureMemorySnapshots(UObject* Asset)
{
	TArray<FWorkflowObjectMemorySnapshot> Snapshots;
	for (UObject* Object : GatherDomainOwnedObjects(Asset))
	{
		if (!IsValid(Object))
		{
			continue;
		}
		FWorkflowObjectMemorySnapshot Snapshot(Object);
		Snapshot.OuterDepth = GetObjectOuterDepth(Object);
		FObjectWriter Writer(
			Object,
			Snapshot.Bytes,
			false,
			false,
			false,
			PPF_DuplicateVerbatim);
		Snapshots.Add(MoveTemp(Snapshot));
	}
	return Snapshots;
}

FString ComputeAssetMemorySha256(UObject* Asset)
{
	if (!Asset)
	{
		return FString();
	}

	const TArray<FWorkflowObjectMemorySnapshot> Snapshots =
		CaptureMemorySnapshots(Asset);
	if (Snapshots.IsEmpty())
	{
		return FString();
	}

	TArray<uint8> DigestInput;
	for (const FWorkflowObjectMemorySnapshot& Snapshot : Snapshots)
	{
		const UObject* Object = Snapshot.Object.Get();
		if (!Object)
		{
			return FString();
		}
		const FTCHARToUTF8 PathUtf8(*Object->GetPathName());
		const uint64 PathBytes =
			static_cast<uint64>(PathUtf8.Length());
		const uint64 PayloadBytes =
			static_cast<uint64>(Snapshot.Bytes.Num());
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			DigestInput.Add(
				static_cast<uint8>(
					(PathBytes >> (ByteIndex * 8)) & 0xff));
		}
		DigestInput.Append(
			reinterpret_cast<const uint8*>(PathUtf8.Get()),
			PathUtf8.Length());
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			DigestInput.Add(
				static_cast<uint8>(
					(PayloadBytes >> (ByteIndex * 8)) & 0xff));
		}
		DigestInput.Append(Snapshot.Bytes);
	}

	FString Hex;
	return UEAIIntegration::Infrastructure::TrySha256Hex(
			DigestInput,
			Hex)
		? TEXT("sha256:") + Hex
		: FString();
}

void RebuildDomainAfterRollback(UObject* Asset)
{
	if (!Asset)
	{
		return;
	}

	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		Blueprint->PostEditUndo();
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph)
			{
				Graph->NotifyGraphChanged();
			}
		}
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return;
	}

	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		Material->PostEditUndo();
		if (Material->MaterialGraph)
		{
			Material->MaterialGraph.Get()->NotifyGraphChanged();
		}
		return;
	}

	Asset->PostEditUndo();
}

bool RestoreMemorySnapshots(
	UObject* Asset,
	TArray<FWorkflowObjectMemorySnapshot>& Snapshots)
{
	if (!Asset || Snapshots.IsEmpty())
	{
		return false;
	}

	TSet<UObject*> OriginalObjects;
	for (const FWorkflowObjectMemorySnapshot& Snapshot : Snapshots)
	{
		if (UObject* Object = Snapshot.Object.Get())
		{
			OriginalObjects.Add(Object);
		}
	}
	const TArray<UObject*> ObjectsBeforeRestore =
		GatherDomainOwnedObjects(Asset);

	bool bRestored = true;
	for (int32 Index = Snapshots.Num() - 1; Index >= 0; --Index)
	{
		if (UObject* Object = Snapshots[Index].Object.Get())
		{
			if (IsValid(Object))
			{
				Object->PreEditUndo();
			}
		}
	}
	for (int32 Index = Snapshots.Num() - 1; Index >= 0; --Index)
	{
		UObject* Object = Snapshots[Index].Object.Get();
		if (!IsValid(Object))
		{
			bRestored = false;
			continue;
		}
		FObjectReader Reader(Snapshots[Index].Bytes);
		Reader.SetPortFlags(PPF_DuplicateVerbatim);
		Object->Serialize(Reader);
		if (Reader.IsError())
		{
			bRestored = false;
			continue;
		}
	}
	for (int32 Index = Snapshots.Num() - 1; Index >= 0; --Index)
	{
		if (UObject* Object = Snapshots[Index].Object.Get())
		{
			if (Object != Asset && IsValid(Object))
			{
				Object->PostEditUndo();
			}
		}
	}
	for (UObject* Object : ObjectsBeforeRestore)
	{
		if (Object
			&& Object != Asset
			&& !OriginalObjects.Contains(Object)
			&& IsValid(Object))
		{
			Object->MarkAsGarbage();
		}
	}
	RebuildDomainAfterRollback(Asset);
	return bRestored;
}

FString PackageFilenameForAsset(const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	return PackageName.IsEmpty()
		? FString()
		: FPackageName::LongPackageNameToFilename(
			PackageName,
			FPackageName::GetAssetPackageExtension());
}

FString WorkflowAssetSnapshotKey(
	const FString& RunId,
	const FString& ScopeId)
{
	return RunId + TEXT("|") + ScopeId;
}

bool TryHashFile(const FString& Filename, FString& OutDigest)
{
	OutDigest.Reset();
	TArray<uint8> Bytes;
	if (Filename.IsEmpty()
		|| !FFileHelper::LoadFileToArray(Bytes, *Filename))
	{
		return false;
	}
	FString Hex;
	if (!UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, Hex))
	{
		return false;
	}
	OutDigest = TEXT("sha256:") + Hex;
	return true;
}

TSharedPtr<FJsonObject> CaptureAssetPrecondition(
	const FString& ScopeId,
	const FString& Kind,
	const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("scopeId"), ScopeId);
	Result->SetStringField(TEXT("asset"), AssetPath);
	Result->SetStringField(TEXT("kind"), Kind);
	const bool bExists = AssetExistsWithoutLogging(AssetPath);
	Result->SetBoolField(TEXT("exists"), bExists);
	UObject* Asset = bExists
		? LoadAssetWithoutLogging(AssetPath)
		: nullptr;
	Result->SetBoolField(
		TEXT("dirty"),
		Asset && Asset->GetOutermost()->IsDirty());

	if (Asset)
	{
		Result->SetStringField(
			TEXT("structureHash"),
			FWorkflowRuntime::ComputeAssetStructureHash(Asset));
		const FString MemoryDigest = ComputeAssetMemorySha256(Asset);
		if (!MemoryDigest.IsEmpty())
		{
			Result->SetStringField(
				TEXT("memorySha256"),
				MemoryDigest);
		}
		else
		{
			Result->SetField(
				TEXT("memorySha256"),
				MakeShared<FJsonValueNull>());
		}
		FGuid PackageGuid;
		if (UEAIIntegration::Compatibility::TryGetPackagePersistentGuid(
				Asset->GetOutermost(),
				PackageGuid))
		{
			Result->SetStringField(
				TEXT("packageGuid"),
				PackageGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower));
		}
		else
		{
			Result->SetField(
				TEXT("packageGuid"),
				MakeShared<FJsonValueNull>());
		}
	}
	else
	{
		Result->SetField(
			TEXT("structureHash"),
			MakeShared<FJsonValueNull>());
		Result->SetField(
			TEXT("memorySha256"),
			MakeShared<FJsonValueNull>());
		Result->SetField(
			TEXT("packageGuid"),
			MakeShared<FJsonValueNull>());
	}

	const FString PackageFilename =
		PackageFilenameForAsset(AssetPath);
	FString PackageDigest;
	if (!PackageFilename.IsEmpty() &&
		IFileManager::Get().FileExists(*PackageFilename) &&
		TryHashFile(PackageFilename, PackageDigest))
	{
		Result->SetStringField(
			TEXT("packageSha256"),
			PackageDigest);
	}
	else
	{
		Result->SetField(
			TEXT("packageSha256"),
			MakeShared<FJsonValueNull>());
	}

	const UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (Blueprint && Blueprint->GeneratedClass)
	{
		TSharedPtr<FJsonObject> GeneratedClass =
			MakeShared<FJsonObject>();
		GeneratedClass->SetStringField(
			TEXT("path"),
			Blueprint->GeneratedClass->GetPathName());
		GeneratedClass->SetNumberField(
			TEXT("objectGenerationToken"),
			Blueprint->GeneratedClass->GetUniqueID());
		GeneratedClass->SetNumberField(
			TEXT("blueprintSystemVersion"),
			Blueprint->BlueprintSystemVersion);
		Result->SetObjectField(
			TEXT("generatedClass"),
			GeneratedClass);
	}
	else
	{
		Result->SetField(
			TEXT("generatedClass"),
			MakeShared<FJsonValueNull>());
	}
	return Result;
}

TSharedPtr<FJsonObject> FindAssetRecord(
	TArray<TSharedPtr<FJsonValue>>& Assets,
	const FString& ScopeId)
{
	for (const TSharedPtr<FJsonValue>& Value : Assets)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Asset = Value->AsObject();
		FString Candidate;
		if (Asset->TryGetStringField(TEXT("scopeId"), Candidate)
			&& Candidate == ScopeId)
		{
			return Asset;
		}
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindAssetRecord(
	const TArray<TSharedPtr<FJsonValue>>& Assets,
	const FString& ScopeId)
{
	for (const TSharedPtr<FJsonValue>& Value : Assets)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Asset = Value->AsObject();
		FString Candidate;
		if (Asset->TryGetStringField(TEXT("scopeId"), Candidate)
			&& Candidate == ScopeId)
		{
			return Asset;
		}
	}
	return nullptr;
}

bool DeleteWorkflowAsset(const FString& AssetPath)
{
	UObject* Asset = LoadAssetWithoutLogging(AssetPath);
	UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
	if (Asset)
	{
		const TArray<UObject*> Objects = {Asset};
		ObjectTools::DeleteObjectsUnchecked(Objects);
	}
	const FString Filename = PackageFilenameForAsset(AssetPath);
	const bool bFileRemoved =
		Filename.IsEmpty()
		|| !IFileManager::Get().FileExists(*Filename)
		|| IFileManager::Get().Delete(*Filename, false, true, true);
	if (Package)
	{
		IAssetRegistry::GetChecked().PackageDeleted(Package);
	}
	return bFileRemoved && !AssetExistsWithoutLogging(AssetPath);
}

bool RestoreWorkflowAssetFile(
	const TSharedPtr<FJsonObject>& AssetRecord,
	FString& OutError)
{
	const FString AssetPath =
		AssetRecord.IsValid()
		? AssetRecord->GetStringField(TEXT("asset"))
		: FString();
	bool bExistedBefore = false;
	if (AssetRecord.IsValid())
	{
		AssetRecord->TryGetBoolField(
			TEXT("existedBefore"),
			bExistedBefore);
	}
	if (!bExistedBefore)
	{
		if (DeleteWorkflowAsset(AssetPath))
		{
			return true;
		}
		OutError = FString::Printf(
			TEXT("Could not remove newly-created workflow asset '%s'."),
			*AssetPath);
		return false;
	}

	const FString SnapshotFilename =
		AssetRecord->GetStringField(TEXT("snapshotFilename"));
	const FString PackageFilename =
		AssetRecord->GetStringField(TEXT("packageFilename"));
	if (SnapshotFilename.IsEmpty()
		|| PackageFilename.IsEmpty()
		|| !IFileManager::Get().FileExists(*SnapshotFilename))
	{
		OutError = FString::Printf(
			TEXT("Durable snapshot is unavailable for '%s'."),
			*AssetPath);
		return false;
	}
	FString SnapshotDigest;
	const FString ExpectedSnapshotDigest =
		AssetRecord->GetStringField(TEXT("snapshotSha256"));
	if (ExpectedSnapshotDigest.IsEmpty()
		|| !TryHashFile(SnapshotFilename, SnapshotDigest)
		|| SnapshotDigest != ExpectedSnapshotDigest)
	{
		OutError = FString::Printf(
			TEXT("Durable snapshot integrity check failed for '%s'."),
			*AssetPath);
		return false;
	}
	if (IFileManager::Get().Copy(
			*PackageFilename,
			*SnapshotFilename,
			true,
			true) != COPY_OK)
	{
		OutError = FString::Printf(
			TEXT("Could not restore staged package snapshot for '%s'."),
			*AssetPath);
		return false;
	}

	if (UObject* LoadedAsset = LoadAssetWithoutLogging(AssetPath))
	{
		TArray<UPackage*> Packages = {LoadedAsset->GetOutermost()};
		FText ReloadError;
		if (!UPackageTools::ReloadPackages(
				Packages,
				ReloadError,
				EReloadPackagesInteractionMode::AssumePositive))
		{
			OutError = FString::Printf(
				TEXT("Restored package '%s' could not be reloaded: %s"),
				*AssetPath,
				*ReloadError.ToString());
			return false;
		}
	}
	return true;
}
}

FWorkflowRuntime::FWorkflowRuntime(FMCPToolRegistry& InRegistry)
	: Registry(InRegistry)
	, ServerInstanceId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower))
	, JournalDirectory(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEWorkflow")))
{
	IFileManager::Get().MakeDirectory(*JournalDirectory, true);

	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	if (!Plugin.IsValid())
	{
		CoreLoadResult.ok = false;
		CoreLoadResult.exit_code = 4;
		ue::workflow::Diagnostic Diagnostic;
		Diagnostic.code = "plugin_not_found";
		Diagnostic.phase = "load";
		Diagnostic.message =
			"IPluginManager could not resolve the UE_AI_integration plugin.";
		CoreLoadResult.diagnostics.push_back(MoveTemp(Diagnostic));
		return;
	}

	const FString PluginBase = Plugin->GetBaseDir();
	ue::workflow::LoadOptions Options;
	Options.contract_root = std::filesystem::path(ToUtf8(
		FPaths::Combine(PluginBase, TEXT("Workflow"), TEXT("Contracts"))));
	Options.capability_roots.push_back(std::filesystem::path(ToUtf8(
		FPaths::Combine(PluginBase, TEXT("Resources"), TEXT("Capabilities")))));
	CoreLoadResult = CoreEngine.Load(Options);
	if (CoreLoadResult.ok)
	{
		ContractSetDigest = FromUtf8(CoreEngine.ContractSetDigest());
	}
}

FWorkflowRuntime::~FWorkflowRuntime()
{
	PreparedPlanCache.Empty();
}

FMCPResult FWorkflowRuntime::MakeHandshake() const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("apiVersion"), TEXT("v1"));
	Data->SetStringField(TEXT("dsl"), TEXT("ue.workflow"));
	Data->SetStringField(TEXT("dslVersion"), FromUtf8(
		std::string(ue::workflow::DslVersion())));
	Data->SetArrayField(
		TEXT("dslVersions"),
		{
			MakeShared<FJsonValueString>(TEXT("1.0")),
			MakeShared<FJsonValueString>(TEXT("2.0")),
		});
	Data->SetStringField(TEXT("plannerVersion"), FromUtf8(
		std::string(ue::workflow::PlannerVersion())));
	Data->SetStringField(TEXT("serverInstanceId"), ServerInstanceId);
	Data->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Data->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Data->SetStringField(TEXT("contractSetDigest"), ContractSetDigest);
	Data->SetStringField(
		TEXT("status"),
		CoreLoadResult.ok && Registry.IsReady() ? TEXT("ready") : TEXT("degraded"));
	Data->SetNumberField(
		TEXT("composableOperationCount"),
		static_cast<double>(
			CoreLoadResult.ok ? CoreEngine.ComposableOperationCount() : 0));

	TArray<TSharedPtr<FJsonValue>> Actions;
	for (const TCHAR* Action : {
		TEXT("validate"),
		TEXT("plan"),
		TEXT("execute"),
		TEXT("resume"),
		TEXT("status"),
		TEXT("rollback")})
	{
		Actions.Add(MakeShared<FJsonValueString>(Action));
	}
	Data->SetArrayField(TEXT("actions"), Actions);

	TSharedPtr<FJsonObject> Features = MakeShared<FJsonObject>();
	Features->SetBoolField(TEXT("singleAssetScope"), true);
	Features->SetBoolField(TEXT("multiAssetScopes"), true);
	Features->SetNumberField(TEXT("maxScopes"), 16);
	Features->SetNumberField(TEXT("maxOperations"), 256);
	Features->SetBoolField(TEXT("transaction"), true);
	Features->SetStringField(
		TEXT("rollback"),
		TEXT("durableAssetSetSnapshot"));
	Features->SetBoolField(TEXT("dirtyOnly"), true);
	Features->SetBoolField(TEXT("saveOnSuccess"), true);
	Features->SetStringField(
		TEXT("resume"),
		TEXT("durableSegmentBoundary"));
	Features->SetStringField(
		TEXT("resumeV2"),
		TEXT("durableSegmentBoundary"));
	Data->SetObjectField(TEXT("features"), Features);

	if (!CoreLoadResult.ok)
	{
		Data->SetObjectField(
			TEXT("core"),
			MakeCoreDiagnostics(CoreLoadResult));
	}
	return FMCPResult::Ok(Data);
}

bool FWorkflowRuntime::ParseResponseOptions(const TSharedPtr<FJsonObject>& Request,
                                            const FString& Action, FResponseOptions& OutOptions,
                                            FMCPResult& OutFailure)
{
	OutOptions = FResponseOptions();
	OutOptions.DetailLevel = Action == TEXT("validate") || Action == TEXT("plan")
	                             ? EDetailLevel::Standard
	                             : EDetailLevel::Summary;

	const bool bHasDetails = Request->HasField(TEXT("details"));
	const bool bHasDetailLevel = Request->HasField(TEXT("detailLevel"));
	if (bHasDetails && bHasDetailLevel)
	{
		OutFailure = FMCPResult::Fail(
		    TEXT("invalid_workflow_request"),
		    TEXT("Fields 'details' and 'detailLevel' cannot be used together."), 422);
		return false;
	}

	if (bHasDetails)
	{
		bool bDetails = false;
		if (!Request->TryGetBoolField(TEXT("details"), bDetails))
		{
			OutFailure = FMCPResult::Fail(TEXT("invalid_workflow_request"),
			                              TEXT("Field 'details' must be a boolean."), 422);
			return false;
		}
		OutOptions.DetailLevel = bDetails ? EDetailLevel::Full : EDetailLevel::Summary;
		OutOptions.bUsedDeprecatedDetails = true;
	}
	else if (bHasDetailLevel)
	{
		FString DetailLevel;
		if (!Request->TryGetStringField(TEXT("detailLevel"), DetailLevel))
		{
			OutFailure = FMCPResult::Fail(TEXT("invalid_workflow_request"),
			                              TEXT("Field 'detailLevel' must be a string."), 422);
			return false;
		}
		if (DetailLevel == TEXT("summary"))
		{
			OutOptions.DetailLevel = EDetailLevel::Summary;
		}
		else if (DetailLevel == TEXT("standard"))
		{
			OutOptions.DetailLevel = EDetailLevel::Standard;
		}
		else if (DetailLevel == TEXT("full"))
		{
			OutOptions.DetailLevel = EDetailLevel::Full;
		}
		else
		{
			OutFailure = FMCPResult::Fail(
			    TEXT("invalid_workflow_request"),
			    TEXT("Field 'detailLevel' must be summary, standard, or full."), 422);
			return false;
		}
	}

	if (!Request->HasField(TEXT("sections")))
	{
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (!Request->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
	{
		OutFailure = FMCPResult::Fail(TEXT("invalid_workflow_request"),
		                              TEXT("Field 'sections' must be an array of strings."), 422);
		return false;
	}

	static const TSet<FString> SupportedSections = {
	    TEXT("operations"), TEXT("finalizers"), TEXT("readBack"),    TEXT("assetDiff"),
	    TEXT("structures"), TEXT("rollback"),   TEXT("diagnostics"),
	};
	for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
	{
		if (!SectionValue.IsValid() || SectionValue->Type != EJson::String)
		{
			OutFailure = FMCPResult::Fail(TEXT("invalid_workflow_request"),
			                              TEXT("Every 'sections' entry must be a string."), 422);
			return false;
		}
		const FString Section = SectionValue->AsString();
		if (!SupportedSections.Contains(Section))
		{
			OutFailure = FMCPResult::Fail(
			    TEXT("invalid_workflow_request"),
			    FString::Printf(TEXT("Unsupported workflow response section '%s'."), *Section),
			    422);
			return false;
		}
		OutOptions.Sections.Add(Section);
	}
	return true;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::ProjectPlanningResponse(
    const TSharedPtr<FJsonObject>& Response, const FResponseOptions& Options)
{
	if (!Response.IsValid())
	{
		return MakeShared<FJsonObject>();
	}

	if (Options.DetailLevel != EDetailLevel::Summary)
	{
		TSharedPtr<FJsonObject> Projected = CloneObject(Response);
		Projected->SetStringField(TEXT("detailLevel"), Options.DetailLevel == EDetailLevel::Full
		                                                   ? TEXT("full")
		                                                   : TEXT("standard"));
		if (Options.bUsedDeprecatedDetails)
		{
			Projected->SetArrayField(
			    TEXT("deprecations"),
			    {
			        MakeShared<FJsonValueString>(
			            TEXT("'details' is deprecated; use 'detailLevel' instead.")),
			    });
		}
		return Projected;
	}

	TSharedPtr<FJsonObject> Projected = MakeShared<FJsonObject>();
	static const TSet<FString> SummaryFields = {
	    TEXT("schema"),     TEXT("plannerVersion"), TEXT("contractSetDigest"),
	    TEXT("planDigest"), TEXT("contractSet"),    TEXT("validationScope"),
	    TEXT("corePlanDigest"), TEXT("executionReady"), TEXT("preconditions"),
	    TEXT("risk"),       TEXT("approval"),       TEXT("valid"),
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Response->Values)
	{
		if (SummaryFields.Contains(Field.Key))
		{
			Projected->SetField(Field.Key, Field.Value);
		}
	}
	Projected->SetStringField(TEXT("detailLevel"), TEXT("summary"));

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	for (const FString& ArrayField :
	     {TEXT("assetSet"), TEXT("initializers"), TEXT("operations"), TEXT("finalizers"),
	      TEXT("diagnostics")})
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Response->TryGetArrayField(ArrayField, Values) && Values)
		{
			Summary->SetNumberField(ArrayField + TEXT("Count"), Values->Num());
		}
	}
	Projected->SetObjectField(TEXT("summary"), Summary);
	const TArray<TSharedPtr<FJsonValue>>* PlannedAssets = nullptr;
	if (Response->TryGetArrayField(TEXT("assetSet"), PlannedAssets)
		&& PlannedAssets)
	{
		Projected->SetArrayField(TEXT("assetSet"), *PlannedAssets);
	}

	TSharedPtr<FJsonObject> Sections = MakeShared<FJsonObject>();
	for (const FString& SectionName : {TEXT("operations"), TEXT("finalizers"), TEXT("diagnostics")})
	{
		if (!Options.Sections.Contains(SectionName))
		{
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Response->TryGetArrayField(SectionName, Values) && Values)
		{
			Sections->SetArrayField(SectionName, *Values);
		}
	}
	if (!Sections->Values.IsEmpty())
	{
		Projected->SetObjectField(TEXT("sections"), Sections);
	}
	if (Options.bUsedDeprecatedDetails)
	{
		Projected->SetArrayField(TEXT("deprecations"),
		                         {
		                             MakeShared<FJsonValueString>(TEXT(
		                                 "'details' is deprecated; use 'detailLevel' instead.")),
		                         });
	}
	return Projected;
}

FMCPResult FWorkflowRuntime::HandleRequest(
	const TSharedPtr<FJsonObject>& Request)
{
	if (!Request.IsValid())
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Workflow request must be a JSON object."),
			422);
	}

	static const TSet<FString> AllowedFields = {
	    TEXT("action"),  TEXT("workflow"),      TEXT("approvePlanDigest"),
	    TEXT("runId"),   TEXT("saveOnSuccess"), TEXT("confirmWrite"),
	    TEXT("details"), TEXT("detailLevel"),   TEXT("sections"),
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Request->Values)
	{
		if (!AllowedFields.Contains(Field.Key))
		{
			return FMCPResult::Fail(
				TEXT("invalid_params"),
				FString::Printf(
					TEXT("Unknown workflow request field '%s'."),
					*Field.Key),
				422);
		}
	}

	FString Action;
	if (!Request->TryGetStringField(TEXT("action"), Action)
		|| Action.IsEmpty())
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Field 'action' must be a non-empty string."),
			422);
	}
	static const TSet<FString> SupportedActions = {
		TEXT("validate"),
		TEXT("plan"),
		TEXT("execute"),
		TEXT("resume"),
		TEXT("status"),
		TEXT("rollback"),
	};
	if (!SupportedActions.Contains(Action))
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			FString::Printf(
				TEXT("Unsupported workflow action '%s'."),
				*Action),
			422);
	}

	TSet<FString> ActionFields = {
	    TEXT("action"),
	    TEXT("details"),
	    TEXT("detailLevel"),
	    TEXT("sections"),
	};
	if (Action == TEXT("validate") || Action == TEXT("plan"))
	{
		ActionFields.Add(TEXT("workflow"));
	}
	else if (Action == TEXT("execute"))
	{
		ActionFields.Add(TEXT("workflow"));
		ActionFields.Add(TEXT("approvePlanDigest"));
		ActionFields.Add(TEXT("saveOnSuccess"));
		ActionFields.Add(TEXT("confirmWrite"));
	}
	else if (Action == TEXT("status") || Action == TEXT("resume"))
	{
		ActionFields.Add(TEXT("runId"));
	}
	else if (Action == TEXT("rollback"))
	{
		ActionFields.Add(TEXT("runId"));
		ActionFields.Add(TEXT("approvePlanDigest"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Request->Values)
	{
		if (!ActionFields.Contains(Field.Key))
		{
			return FMCPResult::Fail(
				TEXT("invalid_params"),
				FString::Printf(
					TEXT("Field '%s' is not valid for workflow action '%s'."),
					*Field.Key,
					*Action),
				422);
		}
	}
	for (const TCHAR* BoolField :
		{TEXT("saveOnSuccess"), TEXT("confirmWrite"), TEXT("details")})
	{
		if (Request->HasField(BoolField)
			&& !Request->HasTypedField<EJson::Boolean>(BoolField))
		{
			return FMCPResult::Fail(
				TEXT("invalid_params"),
				FString::Printf(
					TEXT("Field '%s' must be a boolean."),
					BoolField),
				422);
		}
	}

	FResponseOptions ResponseOptions;
	FMCPResult ResponseOptionsFailure;
	if (!ParseResponseOptions(Request, Action, ResponseOptions, ResponseOptionsFailure))
	{
		return ResponseOptionsFailure;
	}

	if (!CoreLoadResult.ok && Action != TEXT("status"))
	{
		return FMCPResult::Fail(
			TEXT("workflow_contract_unavailable"),
			TEXT("UE Workflow contracts failed to load."),
			503,
			MakeCoreDiagnostics(CoreLoadResult));
	}

	if (Action == TEXT("validate") || Action == TEXT("plan"))
	{
		const TSharedPtr<FJsonObject>* Workflow = nullptr;
		if (!Request->TryGetObjectField(TEXT("workflow"), Workflow)
			|| !Workflow
			|| !Workflow->IsValid())
		{
			return FMCPResult::Fail(
				TEXT("invalid_params"),
				TEXT("Action requires object field 'workflow'."),
				422);
		}
		return Action == TEXT("validate") ? ValidateWorkflow(*Workflow, ResponseOptions)
		                                  : PlanWorkflow(*Workflow, ResponseOptions);
	}

	if (Action == TEXT("execute"))
	{
		return ExecuteWorkflow(Request, ResponseOptions);
	}

	FString RunId;
	if (!Request->TryGetStringField(TEXT("runId"), RunId)
		|| RunId.IsEmpty())
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Action requires non-empty string field 'runId'."),
			422);
	}
	if (!IsCanonicalRunId(RunId))
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Field 'runId' must be a canonical lowercase GUID."),
			422);
	}

	if (Action == TEXT("status"))
	{
		return GetStatus(RunId, ResponseOptions);
	}
	if (Action == TEXT("resume"))
	{
		return ResumeRun(RunId, ResponseOptions);
	}
	if (Action == TEXT("rollback"))
	{
		return Rollback(Request, ResponseOptions);
	}

	return FMCPResult::Fail(
		TEXT("invalid_params"),
		FString::Printf(TEXT("Unsupported workflow action '%s'."), *Action),
		422);
}

FMCPResult FWorkflowRuntime::ValidateWorkflow(const TSharedPtr<FJsonObject>& Workflow,
                                              const FResponseOptions& Options) const
{
	const ue::workflow::Result CoreResult =
		CoreEngine.ValidateJson(ToUtf8(JsonStringify(Workflow)));
	if (!CoreResult.ok)
	{
		return FMCPResult::Fail(
			TEXT("workflow_validation_failed"),
			TEXT("Workflow failed validation."),
			422,
			MakeCoreDiagnostics(CoreResult));
	}

	TSharedPtr<FJsonObject> Data;
	if (!ParseJsonObject(FromUtf8(CoreResult.json), Data))
	{
		return FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("UEWorkflowCore returned invalid validation JSON."),
			500);
	}
	return FMCPResult::Ok(ProjectPlanningResponse(Data, Options));
}

FMCPResult FWorkflowRuntime::PlanWorkflow(
	const TSharedPtr<FJsonObject>& Workflow,
	const FResponseOptions& Options)
{
	TSharedPtr<FJsonObject> Plan;
	FMCPResult Failure;
	if (!TryPlan(Workflow, Plan, Failure))
	{
		return Failure;
	}
	TSharedPtr<FJsonObject> PreparedPlan;
	if (!PreparePlanWithAssetPreconditions(
			Plan,
			PreparedPlan,
			Failure))
	{
		return Failure;
	}
	CachePreparedPlan(PreparedPlan);
	return FMCPResult::Ok(
		ProjectPlanningResponse(PreparedPlan, Options));
}

bool FWorkflowRuntime::TryPlan(
	const TSharedPtr<FJsonObject>& Workflow,
	TSharedPtr<FJsonObject>& OutPlan,
	FMCPResult& OutFailure) const
{
	const ue::workflow::Result CoreResult =
		CoreEngine.PlanJson(ToUtf8(JsonStringify(Workflow)));
	if (!CoreResult.ok)
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_plan_failed"),
			TEXT("Workflow could not be planned."),
			422,
			MakeCoreDiagnostics(CoreResult));
		return false;
	}
	if (!ParseJsonObject(FromUtf8(CoreResult.json), OutPlan))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("UEWorkflowCore returned invalid plan JSON."),
			500);
		return false;
	}
	return true;
}

bool FWorkflowRuntime::PreparePlanWithAssetPreconditions(
	const TSharedPtr<FJsonObject>& CorePlan,
	TSharedPtr<FJsonObject>& OutPlan,
	FMCPResult& OutFailure) const
{
	const FString CorePlanDigest =
		GetStringField(CorePlan, TEXT("planDigest"));
	const FString ContractDigest =
		GetStringField(CorePlan, TEXT("contractSetDigest"));
	const TSharedPtr<FJsonObject>* NormalizedWorkflow = nullptr;
	if (!CorePlan.IsValid() || CorePlanDigest.IsEmpty() ||
		ContractDigest.IsEmpty() ||
		!CorePlan->TryGetObjectField(
			TEXT("normalizedWorkflow"),
			NormalizedWorkflow)
		|| !NormalizedWorkflow || !NormalizedWorkflow->IsValid())
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT(
				"Core plan cannot be bound to Editor asset "
				"preconditions."),
			500);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Assets;
	const FString PlanSchema =
		GetStringField(CorePlan, TEXT("schema"));
	if (PlanSchema == TEXT("ue.workflow-plan.v2"))
	{
		const TArray<TSharedPtr<FJsonValue>>* PlannedAssets = nullptr;
		if (!CorePlan->TryGetArrayField(
				TEXT("assetSet"),
				PlannedAssets)
			|| !PlannedAssets)
		{
			OutFailure = FMCPResult::Fail(
				TEXT("workflow_core_invalid_response"),
				TEXT("Workflow v2 plan has no assetSet."),
				500);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& AssetValue :
			 *PlannedAssets)
		{
			const TSharedPtr<FJsonObject> Asset =
				AssetValue.IsValid() &&
					AssetValue->Type == EJson::Object
				? AssetValue->AsObject()
				: nullptr;
			if (!Asset.IsValid())
			{
				continue;
			}
			Assets.Add(MakeShared<FJsonValueObject>(
				CaptureAssetPrecondition(
					GetStringField(Asset, TEXT("scopeId")),
					GetStringField(Asset, TEXT("kind")),
					GetStringField(Asset, TEXT("asset")))));
		}
	}
	else
	{
		const TSharedPtr<FJsonObject>* Scope = nullptr;
		if (!(*NormalizedWorkflow)->TryGetObjectField(
				TEXT("scope"),
				Scope)
			|| !Scope || !Scope->IsValid())
		{
			OutFailure = FMCPResult::Fail(
				TEXT("workflow_core_invalid_response"),
				TEXT("Workflow v1 plan has no scope."),
				500);
			return false;
		}
		Assets.Add(MakeShared<FJsonValueObject>(
			CaptureAssetPrecondition(
				TEXT("primary"),
				GetStringField(*Scope, TEXT("kind")),
				GetStringField(*Scope, TEXT("asset")))));
	}
	Assets.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			const TSharedPtr<FJsonObject> LeftObject =
				Left->AsObject();
			const TSharedPtr<FJsonObject> RightObject =
				Right->AsObject();
			const FString LeftAsset =
				GetStringField(LeftObject, TEXT("asset"));
			const FString RightAsset =
				GetStringField(RightObject, TEXT("asset"));
			if (LeftAsset != RightAsset)
			{
				return LeftAsset < RightAsset;
			}
			return GetStringField(LeftObject, TEXT("scopeId")) <
				GetStringField(RightObject, TEXT("scopeId"));
		});

	for (const TSharedPtr<FJsonValue>& AssetValue : Assets)
	{
		const TSharedPtr<FJsonObject> Asset =
			AssetValue.IsValid() && AssetValue->Type == EJson::Object
			? AssetValue->AsObject()
			: nullptr;
		bool bDirty = false;
		if (Asset.IsValid()
			&& Asset->TryGetBoolField(TEXT("dirty"), bDirty)
			&& bDirty)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(
				TEXT("scopeId"),
				GetStringField(Asset, TEXT("scopeId")));
			Details->SetStringField(
				TEXT("asset"),
				GetStringField(Asset, TEXT("asset")));
			Details->SetStringField(
				TEXT("guidance"),
				TEXT(
					"Save or revert the asset before preparing an "
					"executable workflow plan."));
			OutFailure = FMCPResult::Fail(
				TEXT("asset_dirty"),
				TEXT(
					"Editor-prepared plans require a clean existing "
					"asset baseline."),
				409,
				Details);
			return false;
		}
	}

	TSharedPtr<FJsonObject> AssetsDigestInput =
		MakeShared<FJsonObject>();
	AssetsDigestInput->SetArrayField(TEXT("assets"), Assets);
	const FString AssetsDigest =
		UEAIIntegration::Infrastructure::DigestJson(
			AssetsDigestInput);
	if (AssetsDigest.IsEmpty())
	{
		OutFailure = FMCPResult::Fail(
			TEXT("asset_precondition_unavailable"),
			TEXT("Editor could not digest asset preconditions."),
			500);
		return false;
	}

	TSharedPtr<FJsonObject> Preconditions =
		MakeShared<FJsonObject>();
	Preconditions->SetStringField(
		TEXT("schema"),
		TEXT("ue.workflow-asset-preconditions.v1"));
	Preconditions->SetBoolField(TEXT("prepared"), true);
	Preconditions->SetArrayField(TEXT("assets"), Assets);
	Preconditions->SetStringField(
		TEXT("digest"),
		TEXT("sha256:") + AssetsDigest);

	TSharedPtr<FJsonObject> BoundDigestInput =
		MakeShared<FJsonObject>();
	BoundDigestInput->SetStringField(
		TEXT("corePlanDigest"),
		CorePlanDigest);
	BoundDigestInput->SetStringField(
		TEXT("contractSetDigest"),
		ContractDigest);
	BoundDigestInput->SetStringField(
		TEXT("preconditionsDigest"),
		TEXT("sha256:") + AssetsDigest);
	const FString BoundDigest =
		UEAIIntegration::Infrastructure::DigestJson(
			BoundDigestInput);
	if (BoundDigest.IsEmpty())
	{
		OutFailure = FMCPResult::Fail(
			TEXT("asset_precondition_unavailable"),
			TEXT("Editor could not bind the workflow plan digest."),
			500);
		return false;
	}

	OutPlan = CloneObject(CorePlan);
	OutPlan->SetStringField(
		TEXT("corePlanDigest"),
		CorePlanDigest);
	OutPlan->SetBoolField(TEXT("executionReady"), true);
	OutPlan->SetObjectField(
		TEXT("preconditions"),
		Preconditions);
	OutPlan->SetStringField(
		TEXT("planDigest"),
		TEXT("sha256:") + BoundDigest);
	const TSharedPtr<FJsonObject>* CoreApproval = nullptr;
	TSharedPtr<FJsonObject> Approval =
		OutPlan->TryGetObjectField(
				TEXT("approval"),
				CoreApproval)
			&& CoreApproval && CoreApproval->IsValid()
		? CloneObject(*CoreApproval)
		: MakeShared<FJsonObject>();
	Approval->SetStringField(
		TEXT("planDigest"),
		TEXT("sha256:") + BoundDigest);
	OutPlan->SetObjectField(TEXT("approval"), Approval);
	return true;
}

bool FWorkflowRuntime::VerifyPlanAssetPreconditions(
	const TSharedPtr<FJsonObject>& PreparedPlan,
	FMCPResult& OutFailure) const
{
	const TSharedPtr<FJsonObject>* Preconditions = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ExpectedAssets = nullptr;
	bool bPrepared = false;
	if (!PreparedPlan.IsValid() ||
		!PreparedPlan->TryGetObjectField(
			TEXT("preconditions"),
			Preconditions)
		|| !Preconditions || !Preconditions->IsValid() ||
		GetStringField(*Preconditions, TEXT("schema")) !=
			TEXT("ue.workflow-asset-preconditions.v1") ||
		!(*Preconditions)->TryGetBoolField(
			TEXT("prepared"),
			bPrepared)
		|| !bPrepared ||
		!(*Preconditions)->TryGetArrayField(
			TEXT("assets"),
			ExpectedAssets)
		|| !ExpectedAssets)
	{
		OutFailure = FMCPResult::Fail(
			TEXT("asset_precondition_required"),
			TEXT(
				"Execute requires an Editor-prepared plan; run "
				"'ue-workflow plan --connect' first."),
			409);
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> CurrentAssets;
	for (const TSharedPtr<FJsonValue>& AssetValue : *ExpectedAssets)
	{
		const TSharedPtr<FJsonObject> Expected =
			AssetValue.IsValid() &&
				AssetValue->Type == EJson::Object
			? AssetValue->AsObject()
			: nullptr;
		if (!Expected.IsValid())
		{
			continue;
		}
		CurrentAssets.Add(MakeShared<FJsonValueObject>(
			CaptureAssetPrecondition(
				GetStringField(Expected, TEXT("scopeId")),
				GetStringField(Expected, TEXT("kind")),
				GetStringField(Expected, TEXT("asset")))));
	}
	TSharedPtr<FJsonObject> DigestInput = MakeShared<FJsonObject>();
	DigestInput->SetArrayField(TEXT("assets"), CurrentAssets);
	const FString CurrentDigest =
		UEAIIntegration::Infrastructure::DigestJson(DigestInput);
	const FString ExpectedDigest =
		GetStringField(*Preconditions, TEXT("digest"));
	if (!CurrentDigest.IsEmpty() &&
		ExpectedDigest == TEXT("sha256:") + CurrentDigest)
	{
		return true;
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(
		TEXT("expectedDigest"),
		ExpectedDigest);
	Details->SetStringField(
		TEXT("actualDigest"),
		CurrentDigest.IsEmpty()
			? FString()
			: TEXT("sha256:") + CurrentDigest);
	for (int32 Index = 0;
		 Index < ExpectedAssets->Num() &&
		 Index < CurrentAssets.Num();
		 ++Index)
	{
		const TSharedPtr<FJsonObject> Expected =
			(*ExpectedAssets)[Index]->AsObject();
		const TSharedPtr<FJsonObject> Actual =
			CurrentAssets[Index]->AsObject();
		if (UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(Expected)) !=
			UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(Actual)))
		{
			Details->SetStringField(
				TEXT("scopeId"),
				GetStringField(Expected, TEXT("scopeId")));
			Details->SetStringField(
				TEXT("asset"),
				GetStringField(Expected, TEXT("asset")));
			Details->SetObjectField(TEXT("expected"), Expected);
			Details->SetObjectField(TEXT("actual"), Actual);
			break;
		}
	}
	OutFailure = FMCPResult::Fail(
		TEXT("asset_precondition_failed"),
		TEXT(
			"Workflow asset state changed after the approved Editor "
			"plan was prepared."),
		409,
		Details);
	return false;
}

void FWorkflowRuntime::PrunePreparedPlanCache()
{
	const double Now = FPlatformTime::Seconds();
	for (auto It = PreparedPlanCache.CreateIterator(); It; ++It)
	{
		if (!It.Value().Plan.IsValid() ||
			Now - It.Value().PreparedAtSeconds >
				PreparedPlanCacheTtlSeconds)
		{
			It.RemoveCurrent();
		}
	}
	while (PreparedPlanCache.Num() >=
		MaxPreparedPlanCacheEntries)
	{
		FString OldestDigest;
		double OldestTime = TNumericLimits<double>::Max();
		for (const TPair<FString, FPreparedPlanCacheEntry>& Pair :
			 PreparedPlanCache)
		{
			if (Pair.Value.PreparedAtSeconds < OldestTime)
			{
				OldestDigest = Pair.Key;
				OldestTime = Pair.Value.PreparedAtSeconds;
			}
		}
		if (OldestDigest.IsEmpty())
		{
			break;
		}
		PreparedPlanCache.Remove(OldestDigest);
	}
}

void FWorkflowRuntime::CachePreparedPlan(
	const TSharedPtr<FJsonObject>& PreparedPlan)
{
	if (!PreparedPlan.IsValid())
	{
		return;
	}
	const FString Digest =
		GetStringField(PreparedPlan, TEXT("planDigest"));
	if (Digest.IsEmpty())
	{
		return;
	}
	PrunePreparedPlanCache();
	FPreparedPlanCacheEntry Entry;
	Entry.Plan = CloneObject(PreparedPlan);
	Entry.PreparedAtSeconds = FPlatformTime::Seconds();
	PreparedPlanCache.Add(Digest, MoveTemp(Entry));
}

bool FWorkflowRuntime::FindPreparedPlan(
	const FString& BoundPlanDigest,
	TSharedPtr<FJsonObject>& OutPlan)
{
	OutPlan.Reset();
	PrunePreparedPlanCache();
	const FPreparedPlanCacheEntry* Entry =
		PreparedPlanCache.Find(BoundPlanDigest);
	if (!Entry || !Entry->Plan.IsValid())
	{
		return false;
	}
	OutPlan = CloneObject(Entry->Plan);
	return OutPlan.IsValid();
}

bool FWorkflowRuntime::AdaptV1PlanToV2(
	const TSharedPtr<FJsonObject>& Plan,
	TSharedPtr<FJsonObject>& OutPlan,
	FString& OutError) const
{
	const TSharedPtr<FJsonObject>* NormalizedWorkflow = nullptr;
	const TSharedPtr<FJsonObject>* Scope = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PlannedOperations = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Initializers = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Finalizers = nullptr;
	if (!Plan.IsValid()
		|| GetStringField(Plan, TEXT("schema")) !=
			TEXT("ue.workflow-plan.v1")
		|| !Plan->TryGetObjectField(
			TEXT("normalizedWorkflow"),
			NormalizedWorkflow)
		|| !NormalizedWorkflow || !NormalizedWorkflow->IsValid()
		|| !(*NormalizedWorkflow)->TryGetObjectField(
			TEXT("scope"),
			Scope)
		|| !Scope || !Scope->IsValid()
		|| !Plan->TryGetArrayField(
			TEXT("operations"),
			PlannedOperations)
		|| !PlannedOperations
		|| !Plan->TryGetArrayField(
			TEXT("initializers"),
			Initializers)
		|| !Initializers
		|| !Plan->TryGetArrayField(
			TEXT("finalizers"),
			Finalizers)
		|| !Finalizers)
	{
		OutError =
			TEXT("Workflow v1 plan is incomplete and cannot be adapted.");
		return false;
	}

	const FString ScopeId = TEXT("primary");
	TSharedPtr<FJsonObject> AdaptedScope = CloneObject(*Scope);
	const TSharedPtr<FJsonObject>* Verify = nullptr;
	if ((*NormalizedWorkflow)->TryGetObjectField(
			TEXT("verify"),
			Verify)
		&& Verify && Verify->IsValid())
	{
		AdaptedScope->SetObjectField(
			TEXT("verify"),
			CloneObject(*Verify));
	}
	TSharedPtr<FJsonObject> Scopes = MakeShared<FJsonObject>();
	Scopes->SetObjectField(ScopeId, AdaptedScope);

	auto AddScopeToRecords =
		[&](const TArray<TSharedPtr<FJsonValue>>& Source,
			TArray<TSharedPtr<FJsonValue>>& Destination) -> bool
	{
		Destination.Reserve(Source.Num());
		for (const TSharedPtr<FJsonValue>& Value : Source)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				OutError =
					TEXT("Workflow v1 plan contains a non-object record.");
				return false;
			}
			TSharedPtr<FJsonObject> Record =
				CloneObject(Value->AsObject());
			Record->SetStringField(TEXT("scope"), ScopeId);
			Destination.Add(MakeShared<FJsonValueObject>(Record));
		}
		return true;
	};

	TArray<TSharedPtr<FJsonValue>> AdaptedOperations;
	TArray<TSharedPtr<FJsonValue>> AdaptedInitializers;
	TArray<TSharedPtr<FJsonValue>> AdaptedFinalizers;
	if (!AddScopeToRecords(*PlannedOperations, AdaptedOperations)
		|| !AddScopeToRecords(*Initializers, AdaptedInitializers)
		|| !AddScopeToRecords(*Finalizers, AdaptedFinalizers))
	{
		return false;
	}

	TSharedPtr<FJsonObject> InternalWorkflow =
		CloneObject(*NormalizedWorkflow);
	InternalWorkflow->SetStringField(TEXT("dslVersion"), TEXT("2.0"));
	InternalWorkflow->Values.Remove(TEXT("scope"));
	InternalWorkflow->Values.Remove(TEXT("verify"));
	InternalWorkflow->SetObjectField(TEXT("scopes"), Scopes);
	const TArray<TSharedPtr<FJsonValue>>* AuthoredOperations = nullptr;
	TArray<TSharedPtr<FJsonValue>> AdaptedAuthoredOperations;
	if ((*NormalizedWorkflow)->TryGetArrayField(
			TEXT("operations"),
			AuthoredOperations)
		&& AuthoredOperations
		&& !AddScopeToRecords(
			*AuthoredOperations,
			AdaptedAuthoredOperations))
	{
		return false;
	}
	InternalWorkflow->SetArrayField(
		TEXT("operations"),
		AdaptedAuthoredOperations);

	bool bCreateIfMissing = false;
	AdaptedScope->TryGetBoolField(
		TEXT("createIfMissing"),
		bCreateIfMissing);
	TSharedPtr<FJsonObject> AssetRecord = MakeShared<FJsonObject>();
	AssetRecord->SetStringField(TEXT("scopeId"), ScopeId);
	AssetRecord->SetStringField(
		TEXT("kind"),
		GetStringField(AdaptedScope, TEXT("kind")));
	AssetRecord->SetStringField(
		TEXT("asset"),
		GetStringField(AdaptedScope, TEXT("asset")));
	AssetRecord->SetBoolField(
		TEXT("createIfMissing"),
		bCreateIfMissing);

	OutPlan = CloneObject(Plan);
	OutPlan->SetStringField(TEXT("schema"), TEXT("ue.workflow-plan.v2"));
	OutPlan->SetStringField(TEXT("sourceDslVersion"), TEXT("1.0"));
	OutPlan->SetObjectField(
		TEXT("originalNormalizedWorkflow"),
		CloneObject(*NormalizedWorkflow));
	OutPlan->SetObjectField(
		TEXT("normalizedWorkflow"),
		InternalWorkflow);
	OutPlan->SetArrayField(
		TEXT("assetSet"),
		{MakeShared<FJsonValueObject>(AssetRecord)});
	OutPlan->SetArrayField(TEXT("initializers"), AdaptedInitializers);
	OutPlan->SetArrayField(TEXT("operations"), AdaptedOperations);
	OutPlan->SetArrayField(TEXT("finalizers"), AdaptedFinalizers);
	return true;
}

FMCPResult FWorkflowRuntime::ExecuteWorkflow(const TSharedPtr<FJsonObject>& Request,
                                             const FResponseOptions& Options)
{
	const TSharedPtr<FJsonObject>* Workflow = nullptr;
	if (!Request->TryGetObjectField(TEXT("workflow"), Workflow)
		|| !Workflow
		|| !Workflow->IsValid())
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Execute requires object field 'workflow'."),
			422);
	}

	TSharedPtr<FJsonObject> CorePlan;
	FMCPResult PlanFailure;
	if (!TryPlan(*Workflow, CorePlan, PlanFailure))
	{
		return PlanFailure;
	}
	const FString CorePlanDigest =
		GetStringField(CorePlan, TEXT("planDigest"));
	FString ApprovedDigest;
	if (!Request->TryGetStringField(TEXT("approvePlanDigest"), ApprovedDigest)
		|| ApprovedDigest.IsEmpty())
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(
			TEXT("corePlanDigest"),
			CorePlanDigest);
		Details->SetBoolField(TEXT("executionReady"), false);
		Details->SetStringField(
			TEXT("guidance"),
			TEXT("Run 'ue-workflow plan --connect' and approve its planDigest."));
		return FMCPResult::Fail(
			TEXT("plan_approval_required"),
			TEXT(
				"Execute requires exact 'approvePlanDigest' from an "
				"Editor-prepared plan."),
			409,
			Details);
	}
	if (ApprovedDigest == CorePlanDigest)
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("approvedPlanDigest"), ApprovedDigest);
		Details->SetStringField(
			TEXT("corePlanDigest"),
			CorePlanDigest);
		Details->SetStringField(
			TEXT("guidance"),
			TEXT("Run 'ue-workflow plan --connect' before executing."));
		Details->SetBoolField(TEXT("executionReady"), false);
		return FMCPResult::Fail(
			TEXT("asset_precondition_required"),
			TEXT(
				"The approved digest came from an offline Core plan "
				"and is not bound to current Editor assets."),
			409,
			Details);
	}

	TSharedPtr<FJsonObject> Plan;
	if (!FindPreparedPlan(ApprovedDigest, Plan))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(
			TEXT("approvedPlanDigest"),
			ApprovedDigest);
		Details->SetStringField(
			TEXT("corePlanDigest"),
			CorePlanDigest);
		Details->SetBoolField(TEXT("executionReady"), false);
		Details->SetStringField(
			TEXT("guidance"),
			TEXT(
				"The prepared plan expired or belongs to another Editor "
				"session; run 'ue-workflow plan --connect' again."));
		return FMCPResult::Fail(
			TEXT("asset_precondition_required"),
			TEXT(
				"Execute requires a cached Editor-prepared plan for the "
				"approved digest."),
			409,
			Details);
	}

	const FString PreparedCoreDigest =
		GetStringField(Plan, TEXT("corePlanDigest"));
	const FString PreparedContractDigest =
		GetStringField(Plan, TEXT("contractSetDigest"));
	if (PreparedCoreDigest != CorePlanDigest ||
		PreparedContractDigest !=
			GetStringField(CorePlan, TEXT("contractSetDigest")))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(
			TEXT("approvedPlanDigest"),
			ApprovedDigest);
		Details->SetStringField(
			TEXT("expectedCorePlanDigest"),
			PreparedCoreDigest);
		Details->SetStringField(
			TEXT("currentCorePlanDigest"),
			CorePlanDigest);
		Details->SetStringField(
			TEXT("expectedContractSetDigest"),
			PreparedContractDigest);
		Details->SetStringField(
			TEXT("currentContractSetDigest"),
			GetStringField(
				CorePlan,
				TEXT("contractSetDigest")));
		return FMCPResult::Fail(
			TEXT("plan_digest_mismatch"),
			TEXT(
				"The approved prepared plan belongs to a different "
				"workflow or contract set."),
			409,
			Details);
	}

	bool bConfirmWrite = false;
	Request->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite);
	if (IsConfirmWriteRisk(Plan) && !bConfirmWrite)
	{
		return FMCPResult::Fail(
			TEXT("confirm_write_required"),
			TEXT("This workflow plan contains confirmWrite operations."),
			403);
	}

	if (!Registry.IsReady())
	{
		return FMCPResult::Fail(
			TEXT("service_degraded"),
			TEXT("Capability bindings failed validation."),
			503,
			MakeDiagnostics(Registry.GetValidationErrors()));
	}
	if (GEditor && GEditor->IsTransactionActive())
	{
		return FMCPResult::Fail(
			TEXT("editor_busy"),
			TEXT("Cannot start a workflow while another Editor transaction is active."),
			409);
	}
	if (!VerifyPlanAssetPreconditions(Plan, PlanFailure))
	{
		return PlanFailure;
	}
	if (GetStringField(Plan, TEXT("schema")) ==
		TEXT("ue.workflow-plan.v2"))
	{
		return ExecuteWorkflowV2(Request, Plan, Options);
	}
	if (GetStringField(Plan, TEXT("schema")) ==
		TEXT("ue.workflow-plan.v1"))
	{
		TSharedPtr<FJsonObject> AdaptedPlan;
		FString AdaptError;
		if (!AdaptV1PlanToV2(Plan, AdaptedPlan, AdaptError))
		{
			return FMCPResult::Fail(
				TEXT("workflow_core_invalid_response"),
				AdaptError,
				500);
		}
		return ExecuteWorkflowV2(Request, AdaptedPlan, Options);
	}

	const TSharedPtr<FJsonObject>* NormalizedWorkflow = nullptr;
	const TSharedPtr<FJsonObject>* Scope = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Plan->TryGetObjectField(TEXT("normalizedWorkflow"), NormalizedWorkflow)
		|| !NormalizedWorkflow
		|| !(*NormalizedWorkflow)->TryGetObjectField(TEXT("scope"), Scope)
		|| !Scope
		|| !Plan->TryGetArrayField(TEXT("operations"), Operations)
		|| !Operations)
	{
		return FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("Workflow plan is missing normalized scope or planned operations."),
			500);
	}

	bool bSaveOnSuccess = false;
	Request->TryGetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess);

	FRunRecord Record;
	Record.RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Record.ServerInstanceId = ServerInstanceId;
	Record.WorkflowId =
		GetStringField(*NormalizedWorkflow, TEXT("workflowId"));
	Record.ScopeAsset = GetStringField(*Scope, TEXT("asset"));
	Record.PlanDigest =
		GetStringField(Plan, TEXT("planDigest"));
	Record.CorePlanDigest =
		GetStringField(Plan, TEXT("corePlanDigest"));
	Record.ContractSetDigest =
		GetStringField(Plan, TEXT("contractSetDigest"));
	Record.Status = TEXT("running");
	Record.RollbackStatus = TEXT("notRequested");
	Record.bSaveOnSuccess = bSaveOnSuccess;
	Runs.Add(Record.RunId, Record);

	FString JournalError;
	if (!SaveRun(Record, JournalError))
	{
		Runs.Remove(Record.RunId);
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Could not create workflow run journal."),
			500,
			MakeDiagnostics({JournalError}));
	}

	Record.bScopeExistedBefore =
		!Record.ScopeAsset.IsEmpty()
		&& AssetExistsWithoutLogging(Record.ScopeAsset);
	UObject* PrimaryAsset = Record.bScopeExistedBefore
		? LoadAssetWithoutLogging(Record.ScopeAsset)
		: nullptr;
	Record.bPackageDirtyBefore =
		PrimaryAsset
		&& PrimaryAsset->GetOutermost()->IsDirty();
	TArray<FWorkflowObjectMemorySnapshot> MemorySnapshots;
	if (Record.bScopeExistedBefore && PrimaryAsset)
	{
		MemorySnapshots = CaptureMemorySnapshots(PrimaryAsset);
		Record.bMemorySnapshotCaptured = !MemorySnapshots.IsEmpty();
	}
	Record.StructureBefore = CaptureAssetStructure(PrimaryAsset);
	Record.StructureHashBefore = ComputeAssetStructureHash(PrimaryAsset);
	const FText TransactionTitle = FText::FromString(
		FString::Printf(TEXT("UE Workflow %s"), *Record.RunId));

	TMap<FString, TSharedPtr<FJsonObject>> PriorOutputs;
	FMCPResult ExecutionFailure;
	bool bExecutionOk = true;

	{
		const FScopedTransaction Transaction(
			WorkflowContextName,
			TransactionTitle,
			PrimaryAsset);
		if (PrimaryAsset)
		{
			for (UObject* Object : GatherDomainOwnedObjects(PrimaryAsset))
			{
				if (Object)
				{
					Object->SetFlags(RF_Transactional);
					Object->Modify();
				}
			}
		}

		bool bCreateIfMissing = false;
		(*Scope)->TryGetBoolField(TEXT("createIfMissing"), bCreateIfMissing);
		const TArray<TSharedPtr<FJsonValue>>* Initializers = nullptr;
		if (!Plan->TryGetArrayField(TEXT("initializers"), Initializers)
			|| !Initializers)
		{
			ExecutionFailure = FMCPResult::Fail(
				TEXT("workflow_core_invalid_response"),
				TEXT("Workflow plan is missing initializers array."),
				500);
			bExecutionOk = false;
		}
		bool bHasExplicitCreate = false;
		for (const TSharedPtr<FJsonValue>& OperationValue : *Operations)
		{
			if (!OperationValue.IsValid()
				|| OperationValue->Type != EJson::Object)
			{
				continue;
			}
			if (GetStringField(
				OperationValue->AsObject(),
				TEXT("kind")) == TEXT("scopeInitializer"))
			{
				bHasExplicitCreate = true;
				break;
			}
		}

		if (bExecutionOk
			&& !PrimaryAsset
			&& bCreateIfMissing
			&& !bHasExplicitCreate)
		{
			if (!Initializers || Initializers->IsEmpty())
			{
				ExecutionFailure = FMCPResult::Fail(
					TEXT("workflow_core_invalid_response"),
					TEXT("createIfMissing workflow has no planned initializer."),
					500);
				bExecutionOk = false;
			}
			for (const TSharedPtr<FJsonValue>& InitializerValue : *Initializers)
			{
				if (!bExecutionOk)
				{
					break;
				}
				if (!InitializerValue.IsValid()
					|| InitializerValue->Type != EJson::Object)
				{
					ExecutionFailure = FMCPResult::Fail(
						TEXT("workflow_core_invalid_response"),
						TEXT("Workflow initializer is not an object."),
						500);
					bExecutionOk = false;
					break;
				}
				const TSharedPtr<FJsonObject> PlannedInitializer =
					InitializerValue->AsObject();
				const FString InitializerId =
					GetStringField(PlannedInitializer, TEXT("id"));
				const FString CreateCapability =
					GetStringField(PlannedInitializer, TEXT("operationType"));
				const TSharedPtr<FJsonObject>* PlannedParams = nullptr;
				TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
				if (PlannedInitializer->TryGetObjectField(
					TEXT("params"),
					PlannedParams)
					&& PlannedParams
					&& PlannedParams->IsValid())
				{
					CreateParams = CloneObject(*PlannedParams);
				}
				TSharedPtr<FJsonObject> Initializer = MakeShared<FJsonObject>();
				Initializer->SetStringField(TEXT("id"), InitializerId);
				Initializer->SetStringField(TEXT("type"), CreateCapability);
				Initializer->SetObjectField(TEXT("params"), CreateParams);
				TSharedPtr<FJsonObject> Output;
				if (!ExecuteOperation(
					Initializer,
					*Scope,
					PriorOutputs,
					bSaveOnSuccess,
					true,
					Output,
					ExecutionFailure))
				{
					Record.Operations.Add(MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							InitializerId,
							CreateCapability,
							TEXT("failed"),
							ExecutionFailure.Error.Details)));
					bExecutionOk = false;
				}
				else
				{
					PriorOutputs.Add(InitializerId, Output);
					Record.Operations.Add(MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							InitializerId,
							CreateCapability,
							TEXT("succeeded"),
							Output)));
					PrimaryAsset = LoadAssetWithoutLogging(
						Record.ScopeAsset);
					if (PrimaryAsset)
					{
						PrimaryAsset->Modify();
					}
				}
			}
		}
		else if (bExecutionOk
			&& PrimaryAsset
			&& Initializers
			&& !Initializers->IsEmpty())
		{
			for (const TSharedPtr<FJsonValue>& InitializerValue : *Initializers)
			{
				if (!InitializerValue.IsValid()
					|| InitializerValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Initializer =
					InitializerValue->AsObject();
				Record.Operations.Add(MakeShared<FJsonValueObject>(
					MakeOperationRecord(
						GetStringField(Initializer, TEXT("id")),
						GetStringField(Initializer, TEXT("operationType")),
						TEXT("skipped"),
						MakeShared<FJsonObject>())));
			}
		}

		for (const TSharedPtr<FJsonValue>& OperationValue : *Operations)
		{
			if (!bExecutionOk)
			{
				break;
			}
			if (!OperationValue.IsValid()
				|| OperationValue->Type != EJson::Object)
			{
				ExecutionFailure = FMCPResult::Fail(
					TEXT("workflow_core_invalid_response"),
					TEXT("Normalized operation is not an object."),
					500);
				bExecutionOk = false;
				break;
			}

			const TSharedPtr<FJsonObject> Operation = OperationValue->AsObject();
			const FString OperationId = GetStringField(Operation, TEXT("id"));
			const FString OperationType = GetStringField(Operation, TEXT("type"));
			TSharedPtr<FJsonObject> Output;
			if (!ExecuteOperation(
				Operation,
				*Scope,
				PriorOutputs,
				bSaveOnSuccess,
				true,
				Output,
				ExecutionFailure))
			{
				Record.Operations.Add(MakeShared<FJsonValueObject>(
					MakeOperationRecord(
						OperationId,
						OperationType,
						TEXT("failed"),
						ExecutionFailure.Error.Details)));
				bExecutionOk = false;
				break;
			}

			PriorOutputs.Add(OperationId, Output);
			Record.Operations.Add(MakeShared<FJsonValueObject>(
				MakeOperationRecord(
					OperationId,
					OperationType,
					TEXT("succeeded"),
					Output)));

			if (!PrimaryAsset && !Record.ScopeAsset.IsEmpty())
			{
				PrimaryAsset = LoadAssetWithoutLogging(Record.ScopeAsset);
				if (PrimaryAsset)
				{
					PrimaryAsset->Modify();
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Finalizers = nullptr;
		if (bExecutionOk
			&& Plan->TryGetArrayField(TEXT("finalizers"), Finalizers)
			&& Finalizers)
		{
			for (const TSharedPtr<FJsonValue>& FinalizerValue : *Finalizers)
			{
				if (!FinalizerValue.IsValid()
					|| FinalizerValue->Type != EJson::Object)
				{
					ExecutionFailure = FMCPResult::Fail(
						TEXT("workflow_core_invalid_response"),
						TEXT("Workflow finalizer is not an object."),
						500);
					bExecutionOk = false;
					break;
				}

				const TSharedPtr<FJsonObject> Finalizer = FinalizerValue->AsObject();
				const FString FinalizerId = GetStringField(Finalizer, TEXT("id"));
				const FString FinalizerType =
					GetStringField(Finalizer, TEXT("operationType"));
				TSharedPtr<FJsonObject> Output;
				if (!ExecuteFinalizer(
					Finalizer,
					*Scope,
					*Operations,
					PriorOutputs,
					Output,
					ExecutionFailure))
				{
					Record.Finalizers.Add(MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							FinalizerId,
							FinalizerType,
							TEXT("failed"),
							ExecutionFailure.Error.Details)));
					bExecutionOk = false;
					break;
				}
				Record.Finalizers.Add(MakeShared<FJsonValueObject>(
					MakeOperationRecord(
						FinalizerId,
						FinalizerType,
						TEXT("succeeded"),
						Output)));
				const FString FinalizerKind =
					GetStringField(Finalizer, TEXT("kind"));
				if (FinalizerKind == TEXT("readBack"))
				{
					if (!Record.ReadBack.IsValid())
					{
						Record.ReadBack = MakeShared<FJsonObject>();
					}
					TArray<FString> ReadBackKeys;
					const TArray<TSharedPtr<FJsonValue>>* GroupedKeys = nullptr;
					if (Finalizer->TryGetArrayField(TEXT("readBackKeys"), GroupedKeys) &&
					    GroupedKeys)
					{
						for (const TSharedPtr<FJsonValue>& KeyValue : *GroupedKeys)
						{
							if (KeyValue.IsValid() && KeyValue->Type == EJson::String)
							{
								ReadBackKeys.Add(KeyValue->AsString());
							}
						}
					}
					if (ReadBackKeys.IsEmpty())
					{
						const FString ReadBackKey = GetStringField(Finalizer, TEXT("readBackKey"));
						if (!ReadBackKey.IsEmpty())
						{
							ReadBackKeys.Add(ReadBackKey);
						}
					}

					for (const FString& ReadBackKey : ReadBackKeys)
					{
						if (ReadBackKey == TEXT("graphs"))
						{
							TSharedPtr<FJsonObject> GraphResults;
							const TSharedPtr<FJsonObject>* Existing = nullptr;
							if (Record.ReadBack->TryGetObjectField(ReadBackKey, Existing) &&
							    Existing && Existing->IsValid())
							{
								GraphResults = *Existing;
							}
							else
							{
								GraphResults = MakeShared<FJsonObject>();
								Record.ReadBack->SetObjectField(ReadBackKey, GraphResults);
							}
							FString GraphName;
							const TSharedPtr<FJsonObject>* FinalizerParams = nullptr;
							if (Finalizer->TryGetObjectField(TEXT("params"), FinalizerParams) &&
							    FinalizerParams && FinalizerParams->IsValid())
							{
								(*FinalizerParams)->TryGetStringField(TEXT("graph"), GraphName);
							}
							if (GraphName.IsEmpty())
							{
								GraphName = GetStringField(Finalizer, TEXT("id"));
							}
							GraphResults->SetObjectField(GraphName, Output);
						}
						else
						{
							Record.ReadBack->SetField(ReadBackKey,
							                          ProjectReadBackValue(ReadBackKey, Output));
						}
					}
				}
				else if (FinalizerKind == TEXT("diff"))
				{
					Record.AssetDiff = Output;
				}
			}
		}
	}

	PrimaryAsset = PrimaryAsset
		? PrimaryAsset
		: LoadAssetWithoutLogging(Record.ScopeAsset);
	Record.StructureAfter = CaptureAssetStructure(PrimaryAsset);
	Record.StructureHashAfter = ComputeAssetStructureHash(PrimaryAsset);
	Record.AssetDiff = MakeStructuralDiff(
		Record.StructureBefore,
		Record.StructureAfter);
	Record.AssetDiff->SetStringField(TEXT("scopeAsset"), Record.ScopeAsset);
	Record.AssetDiff->SetStringField(
		TEXT("structureHashBefore"),
		Record.StructureHashBefore);
	Record.AssetDiff->SetStringField(
		TEXT("structureHashAfter"),
		Record.StructureHashAfter);

	const FTransactionContext UndoContext =
		GEditor && GEditor->Trans
			? GEditor->Trans->GetUndoContext(false)
			: FTransactionContext();
	if (UndoContext.IsValid()
		&& UndoContext.Context == WorkflowContextName
		&& UndoContext.Title.EqualTo(TransactionTitle))
	{
		Record.TransactionId = UndoContext.TransactionId.ToString(
			EGuidFormats::DigitsWithHyphensLower);
	}
	Record.bRollbackAvailable =
		!bSaveOnSuccess
		&& (IsTopTransaction(Record.TransactionId)
			|| Record.bMemorySnapshotCaptured
			|| !Record.bScopeExistedBefore);

	auto AttemptExecutionRollback = [&]() -> bool
	{
		const bool bUndoApplied =
			IsTopTransaction(Record.TransactionId)
			&& GEditor->UndoTransaction(false);
		if (bUndoApplied && Record.bScopeExistedBefore)
		{
			UObject* RestoredAsset =
				LoadAssetWithoutLogging(Record.ScopeAsset);
			RebuildDomainAfterRollback(RestoredAsset);
			if (RestoredAsset)
			{
				RestoredAsset->GetOutermost()->SetDirtyFlag(
					Record.bPackageDirtyBefore);
			}
		}

		bool bCleanupVerified = VerifyOrCleanRollback(Record);
		if (Record.bScopeExistedBefore
			&& Record.bMemorySnapshotCaptured
			&& (!bUndoApplied || !bCleanupVerified))
		{
			Record.bMemorySnapshotRestoreAttempted = true;
			UObject* RestoreAsset =
				LoadAssetWithoutLogging(Record.ScopeAsset);
			const bool bRestoreCompleted =
				RestoreMemorySnapshots(RestoreAsset, MemorySnapshots);
			if (RestoreAsset)
			{
				RestoreAsset->GetOutermost()->SetDirtyFlag(
					Record.bPackageDirtyBefore);
			}
			bCleanupVerified = VerifyOrCleanRollback(Record);
			Record.bMemorySnapshotRestored =
				bRestoreCompleted && bCleanupVerified;

			TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
			Diagnostic->SetStringField(
				TEXT("kind"),
				TEXT("executionMemorySnapshotRestore"));
			Diagnostic->SetBoolField(TEXT("attempted"), true);
			Diagnostic->SetBoolField(
				TEXT("deserializeCompleted"),
				bRestoreCompleted);
			Diagnostic->SetBoolField(
				TEXT("structureVerified"),
				bCleanupVerified);
			Record.Diagnostics.Add(
				MakeShared<FJsonValueObject>(Diagnostic));
		}

		Record.bRollbackVerified =
			bCleanupVerified
			&& (bUndoApplied
				|| Record.bMemorySnapshotRestored
				|| !Record.bScopeExistedBefore);
		Record.RollbackStatus = Record.bRollbackVerified
			? TEXT("rolledBack")
			: TEXT("manualReview");
		Record.bRollbackAvailable = false;
		return bUndoApplied;
	};

	if (!bExecutionOk)
	{
		AttemptExecutionRollback();
		Record.Status = Record.bRollbackVerified
			? TEXT("failed")
			: TEXT("blocked");
		if (ExecutionFailure.Error.Details.IsValid())
		{
			Record.Diagnostics.Add(MakeShared<FJsonValueObject>(
				ExecutionFailure.Error.Details));
		}
		JournalError.Reset();
		const bool bJournalSaved = SaveRun(Record, JournalError);
		Runs.Add(Record.RunId, Record);

		TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
		Details->SetStringField(
			TEXT("causeCode"),
			ExecutionFailure.Error.Code);
		Details->SetStringField(
			TEXT("causeMessage"),
			ExecutionFailure.Error.Message);
		if (!bJournalSaved)
		{
			Details->SetStringField(TEXT("journalError"), JournalError);
			return FMCPResult::Fail(
				TEXT("journal_write_failed"),
				TEXT("Workflow failed and its terminal journal could not be written."),
				500,
				Details);
		}
		return FMCPResult::Fail(
			TEXT("workflow_execution_failed"),
			TEXT("Workflow execution failed."),
			ExecutionFailure.Error.HttpStatus >= 400
				? ExecutionFailure.Error.HttpStatus
				: 500,
			Details);
	}

	if (bSaveOnSuccess)
	{
		PrimaryAsset = PrimaryAsset
			? PrimaryAsset
			: LoadAssetWithoutLogging(Record.ScopeAsset);
		if (!PrimaryAsset
			|| !UEditorAssetLibrary::SaveLoadedAsset(PrimaryAsset, true))
		{
			AttemptExecutionRollback();
			Record.Status = Record.bRollbackVerified
				? TEXT("failed")
				: TEXT("blocked");
			Record.Diagnostics.Add(MakeShared<FJsonValueString>(
				TEXT("Final save failed after workflow verification.")));
			JournalError.Reset();
			const bool bJournalSaved = SaveRun(Record, JournalError);
			Runs.Add(Record.RunId, Record);
			if (!bJournalSaved)
			{
				TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
				Details->SetStringField(TEXT("journalError"), JournalError);
				return FMCPResult::Fail(
					TEXT("journal_write_failed"),
					TEXT("Final save and terminal journal write both failed."),
					500,
					Details);
			}
			return FMCPResult::Fail(TEXT("workflow_save_failed"),
			                        TEXT("Workflow verified, but the final asset save failed."),
			                        500, Record.ToResultJson(Options));
		}
		Record.RollbackStatus = TEXT("notAvailableAfterSave");
		Record.bRollbackAvailable = false;
		RollbackMemorySnapshots.Remove(Record.RunId);
	}
	else if (Record.bScopeExistedBefore
		&& Record.bMemorySnapshotCaptured
		&& !MemorySnapshots.IsEmpty())
	{
		RollbackMemorySnapshots.Add(
			Record.RunId,
			MoveTemp(MemorySnapshots));
		Record.bRollbackAvailable = true;
	}
	else if (!Record.bScopeExistedBefore)
	{
		// New unsaved assets can be removed safely in this Editor instance as
		// long as their post-state still matches the completed workflow.
		Record.bRollbackAvailable = true;
	}

	if (PrimaryAsset && PrimaryAsset->GetOutermost()->IsDirty())
	{
		Record.DirtyPackages.Add(MakeShared<FJsonValueString>(
			PrimaryAsset->GetOutermost()->GetName()));
	}
	Record.Status = TEXT("completed");
	JournalError.Reset();
	if (!SaveRun(Record, JournalError))
	{
		Runs.Add(Record.RunId, Record);
		TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
		Details->SetStringField(TEXT("journalError"), JournalError);
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Workflow completed, but its terminal journal could not be written."),
			500,
			Details);
	}
	Runs.Add(Record.RunId, Record);
	return FMCPResult::Ok(Record.ToResultJson(Options));
}

FMCPResult FWorkflowRuntime::ExecuteWorkflowV2(
	const TSharedPtr<FJsonObject>& Request,
	const TSharedPtr<FJsonObject>& Plan,
	const FResponseOptions& Options)
{
	const TSharedPtr<FJsonObject>* NormalizedWorkflow = nullptr;
	const TSharedPtr<FJsonObject>* Scopes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* AssetSet = nullptr;
	if (!Plan->TryGetObjectField(
			TEXT("normalizedWorkflow"),
			NormalizedWorkflow)
		|| !NormalizedWorkflow
		|| !(*NormalizedWorkflow)->TryGetObjectField(
			TEXT("scopes"),
			Scopes)
		|| !Scopes
		|| !Plan->TryGetArrayField(TEXT("assetSet"), AssetSet)
		|| !AssetSet
		|| AssetSet->IsEmpty())
	{
		return FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("Workflow v2 plan is missing named scopes or assetSet."),
			500);
	}

	bool bSaveOnSuccess = false;
	Request->TryGetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess);

	FRunRecord Record;
	Record.RunId =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Record.ServerInstanceId = ServerInstanceId;
	Record.WorkflowId =
		GetStringField(*NormalizedWorkflow, TEXT("workflowId"));
	Record.DslVersion =
		GetStringField(Plan, TEXT("sourceDslVersion"));
	if (Record.DslVersion.IsEmpty())
	{
		Record.DslVersion = TEXT("2.0");
	}
	Record.PluginVersion = CurrentPluginVersion();
	Record.PlanDigest = GetStringField(Plan, TEXT("planDigest"));
	Record.CorePlanDigest =
		GetStringField(Plan, TEXT("corePlanDigest"));
	Record.ContractSetDigest =
		GetStringField(Plan, TEXT("contractSetDigest"));
	Record.Status = TEXT("running");
	Record.RollbackStatus = TEXT("notRequested");
	Record.CurrentPhase = TEXT("staging");
	Record.bSaveOnSuccess = bSaveOnSuccess;
	Record.bDurableResume = true;
	const TSharedPtr<FJsonObject>* OriginalNormalizedWorkflow = nullptr;
	Record.NormalizedWorkflow =
		Plan->TryGetObjectField(
				TEXT("originalNormalizedWorkflow"),
				OriginalNormalizedWorkflow)
			&& OriginalNormalizedWorkflow
			&& OriginalNormalizedWorkflow->IsValid()
		? CloneObject(*OriginalNormalizedWorkflow)
		: CloneObject(*NormalizedWorkflow);
	Record.StoredPlan = CloneObject(Plan);
	Record.OperationOutputs = MakeShared<FJsonObject>();
	Record.StructureBefore = MakeShared<FJsonObject>();
	Record.StructureAfter = MakeShared<FJsonObject>();
	Record.StructureAfterRollback = MakeShared<FJsonObject>();
	Record.ReadBack = MakeShared<FJsonObject>();
	Record.AssetDiff = MakeShared<FJsonObject>();

	const FString SnapshotDirectory =
		FPaths::Combine(GetRunDirectory(Record.RunId), TEXT("Snapshots"));
	if (!IFileManager::Get().MakeDirectory(*SnapshotDirectory, true))
	{
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Could not create the Workflow v2 snapshot directory."),
			500);
	}

	for (const TSharedPtr<FJsonValue>& AssetValue : *AssetSet)
	{
		if (!AssetValue.IsValid() ||
			AssetValue->Type != EJson::Object)
		{
			return FMCPResult::Fail(
				TEXT("workflow_core_invalid_response"),
				TEXT("Workflow v2 assetSet contains a non-object entry."),
				500);
		}
		const TSharedPtr<FJsonObject> PlannedAsset =
			AssetValue->AsObject();
		const FString ScopeId =
			GetStringField(PlannedAsset, TEXT("scopeId"));
		const FString AssetPath =
			GetStringField(PlannedAsset, TEXT("asset"));
		const TSharedPtr<FJsonObject>* Scope = nullptr;
		if (ScopeId.IsEmpty() || AssetPath.IsEmpty()
			|| !(*Scopes)->TryGetObjectField(ScopeId, Scope)
			|| !Scope || !Scope->IsValid())
		{
			return FMCPResult::Fail(
				TEXT("workflow_core_invalid_response"),
				TEXT("Workflow v2 assetSet does not match normalized scopes."),
				500);
		}

		const bool bExistedBefore =
			AssetExistsWithoutLogging(AssetPath);
		UObject* Asset = bExistedBefore
			? LoadAssetWithoutLogging(AssetPath)
			: nullptr;
		const bool bDirtyBefore =
			Asset && Asset->GetOutermost()->IsDirty();
		if (bSaveOnSuccess && bDirtyBefore)
		{
			TSharedPtr<FJsonObject> Details =
				MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("scopeId"), ScopeId);
			Details->SetStringField(TEXT("asset"), AssetPath);
			return FMCPResult::Fail(
				TEXT("asset_dirty"),
				TEXT(
					"Workflow v2 saveOnSuccess refuses to overwrite a "
					"scope that was already dirty."),
				409,
				Details);
		}

		TSharedPtr<FJsonObject> AssetRecord =
			CloneObject(PlannedAsset);
		AssetRecord->SetBoolField(
			TEXT("existedBefore"),
			bExistedBefore);
		AssetRecord->SetBoolField(
			TEXT("packageDirtyBefore"),
			bDirtyBefore);
		const FString HashBefore =
			ComputeAssetStructureHash(Asset);
		AssetRecord->SetStringField(
			TEXT("structureHashBefore"),
			HashBefore);
		AssetRecord->SetStringField(
			TEXT("currentHash"),
			HashBefore);
		const FString MemoryHashBefore =
			ComputeAssetMemorySha256(Asset);
		if (!MemoryHashBefore.IsEmpty())
		{
			AssetRecord->SetStringField(
				TEXT("memorySha256Before"),
				MemoryHashBefore);
			AssetRecord->SetStringField(
				TEXT("currentMemorySha256"),
				MemoryHashBefore);
		}
		const FString PackageFilename =
			PackageFilenameForAsset(AssetPath);
		AssetRecord->SetStringField(
			TEXT("packageFilename"),
			PackageFilename);

		if (bExistedBefore)
		{
			if (!Asset || PackageFilename.IsEmpty()
				|| !IFileManager::Get().FileExists(*PackageFilename))
			{
				TSharedPtr<FJsonObject> Details =
					MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("scopeId"), ScopeId);
				Details->SetStringField(TEXT("asset"), AssetPath);
				return FMCPResult::Fail(
					TEXT("snapshot_unavailable"),
					TEXT(
						"Existing Workflow v2 scopes require a package "
						"file for durable recovery."),
					409,
					Details);
			}
			const FString SnapshotFilename = FPaths::Combine(
				SnapshotDirectory,
				ScopeId + FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().Copy(
					*SnapshotFilename,
					*PackageFilename,
					true,
					true) != COPY_OK)
			{
				return FMCPResult::Fail(
					TEXT("snapshot_write_failed"),
					FString::Printf(
						TEXT("Could not stage package snapshot for '%s'."),
						*AssetPath),
					500);
			}
			FString SnapshotDigest;
			if (!TryHashFile(SnapshotFilename, SnapshotDigest))
			{
				return FMCPResult::Fail(
					TEXT("snapshot_write_failed"),
					FString::Printf(
						TEXT("Could not hash package snapshot for '%s'."),
						*AssetPath),
					500);
			}
			AssetRecord->SetStringField(
				TEXT("snapshotFilename"),
				SnapshotFilename);
			AssetRecord->SetStringField(
				TEXT("snapshotSha256"),
				SnapshotDigest);
			FString PackageDigest;
			if (!TryHashFile(PackageFilename, PackageDigest))
			{
				return FMCPResult::Fail(
					TEXT("snapshot_unavailable"),
					FString::Printf(
						TEXT("Could not hash package for '%s'."),
						*AssetPath),
					500);
			}
			AssetRecord->SetStringField(
				TEXT("packageSha256Before"),
				PackageDigest);
			AssetRecord->SetStringField(
				TEXT("currentPackageSha256"),
				PackageDigest);
			TArray<FWorkflowObjectMemorySnapshot> MemorySnapshots =
				CaptureMemorySnapshots(Asset);
			if (!MemorySnapshots.IsEmpty())
			{
				Record.bMemorySnapshotCaptured = true;
				MultiAssetRollbackMemorySnapshots.Add(
					WorkflowAssetSnapshotKey(
						Record.RunId,
						ScopeId),
					MoveTemp(MemorySnapshots));
			}
		}
		Record.Assets.Add(
			MakeShared<FJsonValueObject>(AssetRecord));
		Record.StructureBefore->SetObjectField(
			ScopeId,
			CaptureAssetStructure(Asset));
	}
	if (Record.DslVersion == TEXT("1.0") &&
		Record.Assets.Num() == 1 &&
		Record.Assets[0].IsValid() &&
		Record.Assets[0]->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Asset =
			Record.Assets[0]->AsObject();
		Record.ScopeAsset =
			GetStringField(Asset, TEXT("asset"));
		Record.StructureHashBefore =
			GetStringField(Asset, TEXT("structureHashBefore"));
		Asset->TryGetBoolField(
			TEXT("existedBefore"),
			Record.bScopeExistedBefore);
		Asset->TryGetBoolField(
			TEXT("packageDirtyBefore"),
			Record.bPackageDirtyBefore);
	}

	FMCPResult PreconditionFailure;
	if (!VerifyPlanAssetPreconditions(Plan, PreconditionFailure))
	{
		for (const TSharedPtr<FJsonValue>& AssetValue :
			 Record.Assets)
		{
			if (AssetValue.IsValid() &&
				AssetValue->Type == EJson::Object)
			{
				MultiAssetRollbackMemorySnapshots.Remove(
					WorkflowAssetSnapshotKey(
						Record.RunId,
						GetStringField(
							AssetValue->AsObject(),
							TEXT("scopeId"))));
			}
		}
		const FString RunDirectory = GetRunDirectory(Record.RunId);
		IFileManager::Get().DeleteDirectory(
			*RunDirectory,
			false,
			true);
		return PreconditionFailure;
	}

	FString JournalError;
	if (!SaveRun(Record, JournalError))
	{
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Could not create Workflow v2 run journal."),
			500,
			MakeDiagnostics({JournalError}));
	}
	Runs.Add(Record.RunId, Record);
	return ContinueWorkflowV2(
		Record,
		Plan,
		Options,
		false);
}

FMCPResult FWorkflowRuntime::ContinueWorkflowV2(
	FRunRecord& Record,
	const TSharedPtr<FJsonObject>& Plan,
	const FResponseOptions& Options,
	const bool bRestartFromBaseline)
{
	const TSharedPtr<FJsonObject>* NormalizedWorkflow = nullptr;
	const TSharedPtr<FJsonObject>* Scopes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Initializers = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Finalizers = nullptr;
	if (!Plan.IsValid()
		|| !Plan->TryGetObjectField(
			TEXT("normalizedWorkflow"),
			NormalizedWorkflow)
		|| !NormalizedWorkflow
		|| !(*NormalizedWorkflow)->TryGetObjectField(
			TEXT("scopes"),
			Scopes)
		|| !Scopes
		|| !Plan->TryGetArrayField(
			TEXT("initializers"),
			Initializers)
		|| !Initializers
		|| !Plan->TryGetArrayField(TEXT("operations"), Operations)
		|| !Operations
		|| !Plan->TryGetArrayField(TEXT("finalizers"), Finalizers)
		|| !Finalizers)
	{
		return FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("Workflow v2 plan is incomplete."),
			500);
	}

	if (bRestartFromBaseline)
	{
		Record.NextInitializerIndex = 0;
		Record.NextOperationIndex = 0;
		Record.NextFinalizerIndex = 0;
		Record.CurrentPhase = TEXT("initializers");
		Record.Operations.Reset();
		Record.Finalizers.Reset();
		Record.Diagnostics.Reset();
		Record.DirtyPackages.Reset();
		Record.OperationOutputs = MakeShared<FJsonObject>();
		Record.ReadBack = MakeShared<FJsonObject>();
		Record.AssetDiff = MakeShared<FJsonObject>();
	}
	if (!Record.OperationOutputs.IsValid())
	{
		Record.OperationOutputs = MakeShared<FJsonObject>();
	}

	TMap<FString, TSharedPtr<FJsonObject>> PriorOutputs;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
		Record.OperationOutputs->Values)
	{
		if (Pair.Value.IsValid() &&
			Pair.Value->Type == EJson::Object)
		{
			PriorOutputs.Add(Pair.Key, Pair.Value->AsObject());
		}
	}

	const FText TransactionTitle = FText::FromString(
		FString::Printf(TEXT("UE Workflow v2 %s"), *Record.RunId));
	FMCPResult ExecutionFailure;
	bool bExecutionOk = true;
	auto UpdateCheckpointHash = [&](const FString& ScopeId)
	{
		const TSharedPtr<FJsonObject> AssetRecord =
			FindAssetRecord(Record.Assets, ScopeId);
		if (!AssetRecord.IsValid())
		{
			return;
		}
		const FString AssetPath =
			GetStringField(AssetRecord, TEXT("asset"));
		UObject* Asset = LoadAssetWithoutLogging(AssetPath);
		AssetRecord->SetStringField(
			TEXT("currentHash"),
			ComputeAssetStructureHash(Asset));
		const FString MemoryDigest =
			ComputeAssetMemorySha256(Asset);
		if (!MemoryDigest.IsEmpty())
		{
			AssetRecord->SetStringField(
				TEXT("currentMemorySha256"),
				MemoryDigest);
		}
		else
		{
			AssetRecord->Values.Remove(
				TEXT("currentMemorySha256"));
		}
		const FString PackageFilename =
			GetStringField(
				AssetRecord,
				TEXT("packageFilename"));
		FString PackageDigest;
		if (!PackageFilename.IsEmpty() &&
			IFileManager::Get().FileExists(*PackageFilename) &&
			TryHashFile(PackageFilename, PackageDigest))
		{
			AssetRecord->SetStringField(
				TEXT("currentPackageSha256"),
				PackageDigest);
		}
	};
	auto SaveCheckpoint = [&]() -> bool
	{
		FString Error;
		if (SaveRun(Record, Error))
		{
			Runs.Add(Record.RunId, Record);
			return true;
		}
		ExecutionFailure = FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Workflow v2 checkpoint could not be persisted."),
			500,
			MakeDiagnostics({Error}));
		return false;
	};

	{
		const FScopedTransaction Transaction(
			WorkflowContextName,
			TransactionTitle,
			nullptr);
		for (const TSharedPtr<FJsonValue>& AssetValue : Record.Assets)
		{
			if (!AssetValue.IsValid() ||
				AssetValue->Type != EJson::Object)
			{
				continue;
			}
			UObject* Asset = LoadAssetWithoutLogging(
				GetStringField(
					AssetValue->AsObject(),
					TEXT("asset")));
			for (UObject* Object : GatherDomainOwnedObjects(Asset))
			{
				if (Object)
				{
					Object->SetFlags(RF_Transactional);
					Object->Modify();
				}
			}
		}

		Record.CurrentPhase = TEXT("initializers");
		for (int32 Index = Record.NextInitializerIndex;
			 bExecutionOk && Index < Initializers->Num();
			 ++Index)
		{
			const TSharedPtr<FJsonObject> Planned =
				(*Initializers)[Index].IsValid() &&
					(*Initializers)[Index]->Type == EJson::Object
				? (*Initializers)[Index]->AsObject()
				: nullptr;
			const FString ScopeId =
				GetStringField(Planned, TEXT("scope"));
			const TSharedPtr<FJsonObject>* Scope = nullptr;
			if (!Planned.IsValid()
				|| !(*Scopes)->TryGetObjectField(ScopeId, Scope)
				|| !Scope || !Scope->IsValid())
			{
				ExecutionFailure = FMCPResult::Fail(
					TEXT("workflow_core_invalid_response"),
					TEXT("Workflow v2 initializer has an invalid scope."),
					500);
				bExecutionOk = false;
				break;
			}

			const FString InitializerId =
				GetStringField(Planned, TEXT("id"));
			const FString CapabilityId =
				GetStringField(Planned, TEXT("operationType"));
			const FString AssetPath =
				GetStringField(*Scope, TEXT("asset"));
			if (AssetExistsWithoutLogging(AssetPath))
			{
				Record.Operations.Add(
					MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							InitializerId,
							CapabilityId,
							TEXT("skipped"),
							MakeShared<FJsonObject>())));
			}
			else
			{
				TSharedPtr<FJsonObject> Synthetic =
					MakeShared<FJsonObject>();
				Synthetic->SetStringField(
					TEXT("id"),
					InitializerId);
				Synthetic->SetStringField(
					TEXT("type"),
					CapabilityId);
				const TSharedPtr<FJsonObject>* Params = nullptr;
				Synthetic->SetObjectField(
					TEXT("params"),
					Planned->TryGetObjectField(TEXT("params"), Params)
						&& Params && Params->IsValid()
						? CloneObject(*Params)
						: MakeShared<FJsonObject>());
				TSharedPtr<FJsonObject> Output;
				if (!ExecuteOperation(
						Synthetic,
						*Scope,
						PriorOutputs,
						Record.bSaveOnSuccess,
						true,
						Output,
						ExecutionFailure))
				{
					Record.Operations.Add(
						MakeShared<FJsonValueObject>(
							MakeOperationRecord(
								InitializerId,
								CapabilityId,
								TEXT("failed"),
								ExecutionFailure.Error.Details)));
					bExecutionOk = false;
					break;
				}
				PriorOutputs.Add(InitializerId, Output);
				Record.OperationOutputs->SetObjectField(
					InitializerId,
					Output);
				Record.Operations.Add(
					MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							InitializerId,
							CapabilityId,
							TEXT("succeeded"),
							Output)));
			}
			Record.NextInitializerIndex = Index + 1;
			UpdateCheckpointHash(ScopeId);
			bExecutionOk = SaveCheckpoint();
		}

		Record.CurrentPhase = TEXT("operations");
		for (int32 Index = Record.NextOperationIndex;
			 bExecutionOk && Index < Operations->Num();
			 ++Index)
		{
			const TSharedPtr<FJsonObject> Operation =
				(*Operations)[Index].IsValid() &&
					(*Operations)[Index]->Type == EJson::Object
				? (*Operations)[Index]->AsObject()
				: nullptr;
			const FString ScopeId =
				GetStringField(Operation, TEXT("scope"));
			const TSharedPtr<FJsonObject>* Scope = nullptr;
			if (!Operation.IsValid()
				|| !(*Scopes)->TryGetObjectField(ScopeId, Scope)
				|| !Scope || !Scope->IsValid())
			{
				ExecutionFailure = FMCPResult::Fail(
					TEXT("workflow_core_invalid_response"),
					TEXT("Workflow v2 operation has an invalid scope."),
					500);
				bExecutionOk = false;
				break;
			}
			const FString OperationId =
				GetStringField(Operation, TEXT("id"));
			const FString CapabilityId =
				GetStringField(Operation, TEXT("type"));
			TSharedPtr<FJsonObject> Output;
			if (!ExecuteOperation(
					Operation,
					*Scope,
					PriorOutputs,
					Record.bSaveOnSuccess,
					true,
					Output,
					ExecutionFailure))
			{
				Record.Operations.Add(
					MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							OperationId,
							CapabilityId,
							TEXT("failed"),
							ExecutionFailure.Error.Details)));
				bExecutionOk = false;
				break;
			}
			PriorOutputs.Add(OperationId, Output);
			Record.OperationOutputs->SetObjectField(
				OperationId,
				Output);
			Record.Operations.Add(
				MakeShared<FJsonValueObject>(
					MakeOperationRecord(
						OperationId,
						CapabilityId,
						TEXT("succeeded"),
						Output)));
			Record.NextOperationIndex = Index + 1;
			UpdateCheckpointHash(ScopeId);
			bExecutionOk = SaveCheckpoint();
		}

		Record.CurrentPhase = TEXT("finalizers");
		for (int32 Index = Record.NextFinalizerIndex;
			 bExecutionOk && Index < Finalizers->Num();
			 ++Index)
		{
			const TSharedPtr<FJsonObject> Finalizer =
				(*Finalizers)[Index].IsValid() &&
					(*Finalizers)[Index]->Type == EJson::Object
				? (*Finalizers)[Index]->AsObject()
				: nullptr;
			const FString ScopeId =
				GetStringField(Finalizer, TEXT("scope"));
			const TSharedPtr<FJsonObject>* Scope = nullptr;
			if (!Finalizer.IsValid()
				|| !(*Scopes)->TryGetObjectField(ScopeId, Scope)
				|| !Scope || !Scope->IsValid())
			{
				ExecutionFailure = FMCPResult::Fail(
					TEXT("workflow_core_invalid_response"),
					TEXT("Workflow v2 finalizer has an invalid scope."),
					500);
				bExecutionOk = false;
				break;
			}
			const FString FinalizerId =
				GetStringField(Finalizer, TEXT("id"));
			const FString CapabilityId =
				GetStringField(Finalizer, TEXT("operationType"));
			TSharedPtr<FJsonObject> Output;
			if (!ExecuteFinalizer(
					Finalizer,
					*Scope,
					*Operations,
					PriorOutputs,
					Output,
					ExecutionFailure))
			{
				Record.Finalizers.Add(
					MakeShared<FJsonValueObject>(
						MakeOperationRecord(
							FinalizerId,
							CapabilityId,
							TEXT("failed"),
							ExecutionFailure.Error.Details)));
				bExecutionOk = false;
				break;
			}
			Record.Finalizers.Add(
				MakeShared<FJsonValueObject>(
					MakeOperationRecord(
						FinalizerId,
						CapabilityId,
						TEXT("succeeded"),
						Output)));
			const FString Kind =
				GetStringField(Finalizer, TEXT("kind"));
			if (Kind == TEXT("readBack"))
			{
				TSharedPtr<FJsonObject> ScopeReadBack;
				const TSharedPtr<FJsonObject>* Existing = nullptr;
				if (Record.ReadBack->TryGetObjectField(
						ScopeId,
						Existing)
					&& Existing && Existing->IsValid())
				{
					ScopeReadBack = *Existing;
				}
				else
				{
					ScopeReadBack = MakeShared<FJsonObject>();
					Record.ReadBack->SetObjectField(
						ScopeId,
						ScopeReadBack);
				}
				TArray<FString> Keys;
				const TArray<TSharedPtr<FJsonValue>>* Grouped = nullptr;
				if (Finalizer->TryGetArrayField(
						TEXT("readBackKeys"),
						Grouped)
					&& Grouped)
				{
					for (const TSharedPtr<FJsonValue>& Value : *Grouped)
					{
						if (Value.IsValid() &&
							Value->Type == EJson::String)
						{
							Keys.Add(Value->AsString());
						}
					}
				}
				if (Keys.IsEmpty())
				{
					Keys.Add(
						GetStringField(
							Finalizer,
							TEXT("readBackKey")));
				}
				for (const FString& Key : Keys)
				{
					if (!Key.IsEmpty())
					{
						if (Key == TEXT("graphs"))
						{
							TSharedPtr<FJsonObject> GraphResults;
							const TSharedPtr<FJsonObject>* ExistingGraphs =
								nullptr;
							if (ScopeReadBack->TryGetObjectField(
									Key,
									ExistingGraphs)
								&& ExistingGraphs
								&& ExistingGraphs->IsValid())
							{
								GraphResults = *ExistingGraphs;
							}
							else
							{
								GraphResults =
									MakeShared<FJsonObject>();
								ScopeReadBack->SetObjectField(
									Key,
									GraphResults);
							}
							FString GraphName;
							const TSharedPtr<FJsonObject>* FinalizerParams =
								nullptr;
							if (Finalizer->TryGetObjectField(
									TEXT("params"),
									FinalizerParams)
								&& FinalizerParams
								&& FinalizerParams->IsValid())
							{
								(*FinalizerParams)->TryGetStringField(
									TEXT("graph"),
									GraphName);
							}
							if (GraphName.IsEmpty())
							{
								GraphName = FinalizerId;
							}
							GraphResults->SetObjectField(
								GraphName,
								Output);
						}
						else
						{
							ScopeReadBack->SetField(
								Key,
								ProjectReadBackValue(Key, Output));
						}
					}
				}
			}
			Record.NextFinalizerIndex = Index + 1;
			UpdateCheckpointHash(ScopeId);
			bExecutionOk = SaveCheckpoint();
		}
	}

	const FTransactionContext UndoContext =
		GEditor && GEditor->Trans
		? GEditor->Trans->GetUndoContext(false)
		: FTransactionContext();
	if (UndoContext.IsValid()
		&& UndoContext.Context == WorkflowContextName
		&& UndoContext.Title.EqualTo(TransactionTitle))
	{
		Record.TransactionId = UndoContext.TransactionId.ToString(
			EGuidFormats::DigitsWithHyphensLower);
	}

	for (const TSharedPtr<FJsonValue>& AssetValue : Record.Assets)
	{
		if (!AssetValue.IsValid() ||
			AssetValue->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> AssetRecord =
			AssetValue->AsObject();
		const FString ScopeId =
			GetStringField(AssetRecord, TEXT("scopeId"));
		UObject* Asset = LoadAssetWithoutLogging(
			GetStringField(AssetRecord, TEXT("asset")));
		const TSharedPtr<FJsonObject> StructureAfter =
			CaptureAssetStructure(Asset);
		const FString HashAfter =
			ComputeAssetStructureHash(Asset);
		Record.StructureAfter->SetObjectField(
			ScopeId,
			StructureAfter);
		AssetRecord->SetStringField(
			TEXT("structureHashAfter"),
			HashAfter);
		AssetRecord->SetStringField(
			TEXT("currentHash"),
			HashAfter);
		const FString MemoryHashAfter =
			ComputeAssetMemorySha256(Asset);
		if (!MemoryHashAfter.IsEmpty())
		{
			AssetRecord->SetStringField(
				TEXT("currentMemorySha256"),
				MemoryHashAfter);
		}
		const TSharedPtr<FJsonObject>* StructureBefore = nullptr;
		TSharedPtr<FJsonObject> Diff =
			MakeStructuralDiff(
				Record.StructureBefore->TryGetObjectField(
						ScopeId,
						StructureBefore)
					&& StructureBefore && StructureBefore->IsValid()
					? *StructureBefore
					: MakeShared<FJsonObject>(),
				StructureAfter);
		Diff->SetStringField(TEXT("scopeId"), ScopeId);
		Diff->SetStringField(
			TEXT("asset"),
			GetStringField(AssetRecord, TEXT("asset")));
		Diff->SetStringField(
			TEXT("structureHashBefore"),
			GetStringField(
				AssetRecord,
				TEXT("structureHashBefore")));
		Diff->SetStringField(
			TEXT("structureHashAfter"),
			HashAfter);
		Record.AssetDiff->SetObjectField(ScopeId, Diff);
	}

	if (!bExecutionOk)
	{
		Record.Status = TEXT("failed");
		Record.Diagnostics.Add(
			MakeShared<FJsonValueObject>(
				ExecutionFailure.Error.Details.IsValid()
				? ExecutionFailure.Error.Details
				: MakeShared<FJsonObject>()));
		const FMCPResult RollbackResult =
			RollbackV2(Record, Options, true);
		TSharedPtr<FJsonObject> Details =
			Record.ToResultJson(Options);
		Details->SetStringField(
			TEXT("causeCode"),
			ExecutionFailure.Error.Code);
		Details->SetStringField(
			TEXT("causeMessage"),
			ExecutionFailure.Error.Message);
		if (!RollbackResult.bOk)
		{
			Details->SetObjectField(
				TEXT("rollbackFailure"),
				RollbackResult.Error.Details.IsValid()
				? RollbackResult.Error.Details
				: MakeShared<FJsonObject>());
		}
		return FMCPResult::Fail(
			TEXT("workflow_execution_failed"),
			TEXT("Workflow v2 execution failed."),
			ExecutionFailure.Error.HttpStatus >= 400
				? ExecutionFailure.Error.HttpStatus
				: 500,
			Details);
	}

	if (Record.bSaveOnSuccess)
	{
		Record.CurrentPhase = TEXT("save");
		for (const TSharedPtr<FJsonValue>& AssetValue :
			 Record.Assets)
		{
			const TSharedPtr<FJsonObject> AssetRecord =
				AssetValue.IsValid() &&
					AssetValue->Type == EJson::Object
				? AssetValue->AsObject()
				: nullptr;
			UObject* Asset = LoadAssetWithoutLogging(
				GetStringField(AssetRecord, TEXT("asset")));
			if (!Asset ||
				!UEditorAssetLibrary::SaveLoadedAsset(Asset, true))
			{
				ExecutionFailure = FMCPResult::Fail(
					TEXT("workflow_save_failed"),
					FString::Printf(
						TEXT("Could not save Workflow v2 scope '%s'."),
						*GetStringField(
							AssetRecord,
							TEXT("scopeId"))),
					500);
				Record.Status = TEXT("failed");
				RollbackV2(Record, Options, true);
				return FMCPResult::Fail(
					TEXT("workflow_save_failed"),
					TEXT(
						"Workflow v2 verified, but its staged asset "
						"set could not be saved."),
					500,
					Record.ToResultJson(Options));
			}
			UpdateCheckpointHash(
				GetStringField(AssetRecord, TEXT("scopeId")));
		}
	}

	for (const TSharedPtr<FJsonValue>& AssetValue : Record.Assets)
	{
		const TSharedPtr<FJsonObject> AssetRecord =
			AssetValue.IsValid() &&
				AssetValue->Type == EJson::Object
			? AssetValue->AsObject()
			: nullptr;
		UObject* Asset = LoadAssetWithoutLogging(
			GetStringField(AssetRecord, TEXT("asset")));
		if (Asset && Asset->GetOutermost()->IsDirty())
		{
			Record.DirtyPackages.Add(
				MakeShared<FJsonValueString>(
					Asset->GetOutermost()->GetName()));
		}
		const FString Hash =
			ComputeAssetStructureHash(Asset);
		AssetRecord->SetStringField(
			TEXT("structureHashAfter"),
			Hash);
		AssetRecord->SetStringField(TEXT("currentHash"), Hash);
		const FString MemoryHash =
			ComputeAssetMemorySha256(Asset);
		if (!MemoryHash.IsEmpty())
		{
			AssetRecord->SetStringField(
				TEXT("currentMemorySha256"),
				MemoryHash);
		}
	}
	if (Record.DslVersion == TEXT("1.0") &&
		Record.Assets.Num() == 1 &&
		Record.Assets[0].IsValid() &&
		Record.Assets[0]->Type == EJson::Object)
	{
		Record.StructureHashAfter =
			GetStringField(
				Record.Assets[0]->AsObject(),
				TEXT("structureHashAfter"));
	}
	Record.Status = TEXT("completed");
	Record.CurrentPhase = TEXT("completed");
	Record.bRollbackAvailable = true;
	FString JournalError;
	if (!SaveRun(Record, JournalError))
	{
		Runs.Add(Record.RunId, Record);
		TSharedPtr<FJsonObject> Details =
			Record.ToResultJson(Options);
		Details->SetStringField(
			TEXT("journalError"),
			JournalError);
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT(
				"Workflow v2 completed, but its terminal journal "
				"could not be written."),
			500,
			Details);
	}
	Runs.Add(Record.RunId, Record);
	return FMCPResult::Ok(Record.ToResultJson(Options));
}

bool FWorkflowRuntime::ExecuteOperation(
	const TSharedPtr<FJsonObject>& Operation,
	const TSharedPtr<FJsonObject>& Scope,
	const TMap<FString, TSharedPtr<FJsonObject>>& PriorOutputs,
	bool bSaveOnSuccess,
	bool bDeferCompile,
	TSharedPtr<FJsonObject>& OutResult,
	FMCPResult& OutFailure) const
{
	const TSharedPtr<FJsonObject> MutableOperation = CloneObject(Operation);
	const FString OperationId = GetStringField(MutableOperation, TEXT("id"));
	const FString CapabilityId = GetStringField(MutableOperation, TEXT("type"));
	if (OperationId.IsEmpty() || CapabilityId.IsEmpty())
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("Normalized workflow operation is missing id or type."),
			500);
		return false;
	}
	if (!Registry.FindTool(CapabilityId))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("capability_not_found"),
			FString::Printf(
				TEXT("Workflow capability '%s' is not registered."),
				*CapabilityId),
			404);
		return false;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* OperationParams = nullptr;
	if (MutableOperation->TryGetObjectField(TEXT("params"), OperationParams)
		&& OperationParams
		&& OperationParams->IsValid())
	{
		Params = CloneObject(*OperationParams);
	}
	if (Params->HasField(TEXT("__ueWorkflow")))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Reserved field '__ueWorkflow' cannot be authored."),
			422);
		return false;
	}
	FString Error;
	if (!InjectScopeParams(CapabilityId, Scope, Params, Error))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_scope_mismatch"),
			Error,
			422);
		return false;
	}

	TSet<FString> ScopeParameters;
	const TArray<TSharedPtr<FJsonValue>>* PlannedScopeParameters = nullptr;
	if (MutableOperation->TryGetArrayField(
		TEXT("scopeParameters"),
		PlannedScopeParameters)
		&& PlannedScopeParameters)
	{
		for (const TSharedPtr<FJsonValue>& Value : *PlannedScopeParameters)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				ScopeParameters.Add(Value->AsString());
			}
		}
	}
	const TSharedPtr<FJsonObject>* Bindings = nullptr;
	if (MutableOperation->TryGetObjectField(TEXT("bindings"), Bindings)
		&& Bindings
		&& Bindings->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Binding :
			(*Bindings)->Values)
		{
			FString PointerError;
			TArray<FString> Segments =
				DecodeJsonPointer(Binding.Key, PointerError);
			if (!PointerError.IsEmpty())
			{
				continue;
			}
			if (!Segments.IsEmpty() && Segments[0] == TEXT("params"))
			{
				Segments.RemoveAt(0);
			}
			const FString Destination =
				Segments.Num() == 1 ? Segments[0] : FString();
			if (ScopeParameters.Contains(Destination)
				|| Destination == TEXT("materialFunction"))
			{
				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(
					TEXT("bindingDestination"),
					Binding.Key);
				Details->SetStringField(
					TEXT("protectedParameter"),
					Destination);
				OutFailure = FMCPResult::Fail(
					TEXT("workflow_scope_mismatch"),
					TEXT("A binding cannot target a scope-bound or forbidden parameter."),
					422,
					Details);
				return false;
			}
		}
	}

	if (!ResolveBindings(MutableOperation, PriorOutputs, Params, Error))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_binding_failed"),
			Error,
			422);
		return false;
	}
	if (!InjectScopeParams(CapabilityId, Scope, Params, Error)
		|| (CapabilityId.StartsWith(TEXT("content.material."))
			&& Params->HasField(TEXT("materialFunction"))))
	{
		if (Error.IsEmpty())
		{
			Error =
				TEXT("UE Workflow v1 material scope cannot target 'materialFunction'.");
		}
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_scope_mismatch"),
			Error,
			422);
		return false;
	}

	TArray<FString> ParamErrors;
	if (!Registry.ValidateParams(CapabilityId, Params, ParamErrors))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("invalid_params"),
			FString::Printf(
				TEXT("Workflow operation '%s' failed capability schema validation."),
				*OperationId),
			422,
			MakeDiagnostics(ParamErrors));
		return false;
	}

	TSharedPtr<FJsonObject> InternalParams = CloneObject(Params);
	TSharedPtr<FJsonObject> ExecutionContext = MakeShared<FJsonObject>();
	ExecutionContext->SetBoolField(TEXT("deferCompile"), bDeferCompile);
	ExecutionContext->SetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess);
	InternalParams->SetObjectField(TEXT("__ueWorkflow"), ExecutionContext);

	const FMCPToolResult ToolResult =
		Registry.ExecuteTool(CapabilityId, InternalParams);
	if (!ToolResult.bSuccess)
	{
		OutFailure = FMCPResult::Fail(
			ToolResult.ErrorCode.IsEmpty()
				? TEXT("execution_failed")
				: ToolResult.ErrorCode,
			ToolResult.ErrorMessage.IsEmpty()
				? TEXT("Capability execution failed.")
				: ToolResult.ErrorMessage,
			ToolResult.HttpStatus >= 400 ? ToolResult.HttpStatus : 500,
			ToolResult.Data);
		return false;
	}

	OutResult = ToolResult.Data.IsValid()
		? ToolResult.Data
		: MakeShared<FJsonObject>();
	bool bSemanticSuccess = true;
	if (OutResult->TryGetBoolField(TEXT("success"), bSemanticSuccess)
		&& !bSemanticSuccess)
	{
		OutFailure = FMCPResult::Fail(
			TEXT("execution_failed"),
			FString::Printf(
				TEXT("Capability '%s' reported success=false."),
				*CapabilityId),
			500,
			OutResult);
		return false;
	}
	// Keep execution-phase metadata explicit even for legacy handlers that do
	// not expose it themselves.
	OutResult->SetBoolField(TEXT("saved"), false);
	OutResult->SetBoolField(TEXT("deferredCompile"), bDeferCompile);

	if (CapabilityId == TEXT("blueprint.asset.create")
		|| CapabilityId == TEXT("content.widget.blueprint.create")
		|| CapabilityId == TEXT("content.material.create"))
	{
		FString CreatedPath;
		if (!OutResult->TryGetStringField(TEXT("assetPath"), CreatedPath))
		{
			OutResult->TryGetStringField(TEXT("path"), CreatedPath);
		}
		const FString ScopeAsset = GetStringField(Scope, TEXT("asset"));
		const FString CreatedPackage =
			FPackageName::ObjectPathToPackageName(CreatedPath);
		const FString ScopePackage =
			FPackageName::ObjectPathToPackageName(ScopeAsset);
		if (CreatedPath.IsEmpty() || CreatedPackage != ScopePackage)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("createdPath"), CreatedPath);
			Details->SetStringField(TEXT("scopeAsset"), ScopeAsset);
			OutFailure = FMCPResult::Fail(
				TEXT("workflow_scope_mismatch"),
				TEXT("Create operation did not create the exact scoped asset."),
				500,
				Details);
			return false;
		}
	}
	return true;
}

bool FWorkflowRuntime::ExecuteFinalizer(
	const TSharedPtr<FJsonObject>& Finalizer,
	const TSharedPtr<FJsonObject>& Scope,
	const TArray<TSharedPtr<FJsonValue>>& AuthoredOperations,
	const TMap<FString, TSharedPtr<FJsonObject>>& PriorOutputs,
	TSharedPtr<FJsonObject>& OutResult,
	FMCPResult& OutFailure) const
{
	const FString Kind = GetStringField(Finalizer, TEXT("kind"));
	const FString CapabilityId =
		GetStringField(Finalizer, TEXT("operationType"));
	if (Kind == TEXT("diff") && CapabilityId.IsEmpty())
	{
		OutResult = MakeShared<FJsonObject>();
		OutResult->SetStringField(TEXT("scopeAsset"), GetStringField(Scope, TEXT("asset")));
		OutResult->SetNumberField(
			TEXT("appliedOperationCount"),
			static_cast<double>(PriorOutputs.Num()));
		return true;
	}
	if (CapabilityId.IsEmpty())
	{
		OutFailure = FMCPResult::Fail(
			TEXT("workflow_core_invalid_response"),
			TEXT("Executable finalizer is missing operationType."),
			500);
		return false;
	}

	TSharedPtr<FJsonObject> SyntheticOperation = MakeShared<FJsonObject>();
	SyntheticOperation->SetStringField(
		TEXT("id"),
		GetStringField(Finalizer, TEXT("id")));
	SyntheticOperation->SetStringField(TEXT("type"), CapabilityId);
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* PlannedParams = nullptr;
	if (Finalizer->TryGetObjectField(TEXT("params"), PlannedParams)
		&& PlannedParams
		&& PlannedParams->IsValid())
	{
		Params = CloneObject(*PlannedParams);
	}

	// Graph read-back requires a graph name. Derive the most recent authored
	// graph target only for older plans that did not carry finalizer.params.
	if (CapabilityId == TEXT("blueprint.graph.get")
		&& !Params->HasField(TEXT("graph")))
	{
		for (int32 Index = AuthoredOperations.Num() - 1; Index >= 0; --Index)
		{
			const TSharedPtr<FJsonObject> Authored =
				AuthoredOperations[Index].IsValid()
				&& AuthoredOperations[Index]->Type == EJson::Object
					? AuthoredOperations[Index]->AsObject()
					: nullptr;
			const TSharedPtr<FJsonObject>* AuthoredParams = nullptr;
			if (!Authored.IsValid()
				|| !Authored->TryGetObjectField(TEXT("params"), AuthoredParams)
				|| !AuthoredParams
				|| !AuthoredParams->IsValid())
			{
				continue;
			}
			FString Graph;
			if ((*AuthoredParams)->TryGetStringField(TEXT("graph"), Graph)
				|| (*AuthoredParams)->TryGetStringField(TEXT("graphName"), Graph))
			{
				Params->SetStringField(TEXT("graph"), Graph);
				break;
			}
		}
	}
	SyntheticOperation->SetObjectField(TEXT("params"), Params);
	if (!ExecuteOperation(
		SyntheticOperation,
		Scope,
		PriorOutputs,
		false,
		false,
		OutResult,
		OutFailure))
	{
		return false;
	}

	if (Kind == TEXT("compile"))
	{
		bool bValid = true;
		const bool bHasValid = OutResult->TryGetBoolField(TEXT("valid"), bValid)
			|| OutResult->TryGetBoolField(TEXT("isValid"), bValid);
		if (bHasValid && !bValid)
		{
			OutFailure = FMCPResult::Fail(
				TEXT("workflow_finalizer_failed"),
				FString::Printf(
					TEXT("Compile finalizer '%s' reported an invalid asset."),
					*CapabilityId),
				422,
				OutResult);
			return false;
		}
	}
	return true;
}

bool FWorkflowRuntime::InjectScopeParams(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Scope,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutError) const
{
	const FString Asset = GetStringField(Scope, TEXT("asset"));
	if (Asset.IsEmpty())
	{
		OutError = TEXT("Workflow scope.asset must be non-empty.");
		return false;
	}

	FString PackagePath;
	FString AssetName;
	SplitAssetPath(Asset, PackagePath, AssetName);

	if (CapabilityId == TEXT("blueprint.asset.create"))
	{
		return SetScopedString(Params, TEXT("blueprintName"), AssetName, OutError)
			&& SetScopedString(Params, TEXT("packagePath"), PackagePath, OutError);
	}
	if (CapabilityId == TEXT("content.widget.blueprint.create"))
	{
		return SetScopedString(Params, TEXT("name"), AssetName, OutError)
			&& SetScopedString(
				Params,
				TEXT("packagePath"),
				PackagePath,
				OutError);
	}
	if (CapabilityId == TEXT("content.material.create"))
	{
		return SetScopedString(Params, TEXT("name"), AssetName, OutError)
			&& SetScopedString(Params, TEXT("packagePath"), PackagePath, OutError);
	}

	if (CapabilityId.StartsWith(TEXT("blueprint.")))
	{
		const FString Field =
			CapabilityId == TEXT("blueprint.asset.get")
			|| CapabilityId == TEXT("blueprint.graph.get")
				? TEXT("name")
				: TEXT("blueprint");
		return SetScopedString(Params, Field, Asset, OutError);
	}
	if (CapabilityId.StartsWith(TEXT("content.widget.")))
	{
		return SetScopedString(Params, TEXT("widgetBp"), Asset, OutError);
	}
	if (CapabilityId.StartsWith(TEXT("content.material.")))
	{
		const FString Field =
			CapabilityId == TEXT("content.material.graph.get")
				? TEXT("name")
				: TEXT("material");
		return SetScopedString(Params, Field, Asset, OutError);
	}

	OutError = FString::Printf(
		TEXT("Capability '%s' has no supported single-asset scope mapping."),
		*CapabilityId);
	return false;
}

bool FWorkflowRuntime::ResolveBindings(
	const TSharedPtr<FJsonObject>& Operation,
	const TMap<FString, TSharedPtr<FJsonObject>>& PriorOutputs,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutError) const
{
	const TSharedPtr<FJsonObject>* Bindings = nullptr;
	if (!Operation->TryGetObjectField(TEXT("bindings"), Bindings))
	{
		return true;
	}
	if (!Bindings || !Bindings->IsValid())
	{
		OutError = TEXT("Operation bindings must be an object.");
		return false;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Binding :
		(*Bindings)->Values)
	{
		if (!Binding.Value.IsValid()
			|| Binding.Value->Type != EJson::Object)
		{
			OutError = FString::Printf(
				TEXT("Binding '%s' must be an object."),
				*Binding.Key);
			return false;
		}

		const TSharedPtr<FJsonObject> Source = Binding.Value->AsObject();
		const FString From = GetStringField(Source, TEXT("from"));
		const FString Path = GetStringField(Source, TEXT("path"));
		const TSharedPtr<FJsonObject>* SourceOutput = PriorOutputs.Find(From);
		if (From.IsEmpty() || !SourceOutput || !SourceOutput->IsValid())
		{
			OutError = FString::Printf(
				TEXT("Binding '%s' references unavailable operation '%s'."),
				*Binding.Key,
				*From);
			return false;
		}

		TSharedPtr<FJsonValue> Resolved;
		if (!ResolveJsonPointer(*SourceOutput, Path, Resolved, OutError))
		{
			OutError = FString::Printf(
				TEXT("Binding '%s' could not resolve source '%s%s': %s"),
				*Binding.Key,
				*From,
				*Path,
				*OutError);
			return false;
		}
		if (!SetJsonPointer(
			Params,
			Binding.Key,
			Resolved,
			OutError))
		{
			return false;
		}
	}
	return true;
}

FMCPResult FWorkflowRuntime::GetStatus(const FString& RunId, const FResponseOptions& Options)
{
	FRunRecord Record;
	if (!LoadRun(RunId, Record))
	{
		return FMCPResult::Fail(
			TEXT("workflow_run_not_found"),
			FString::Printf(TEXT("Workflow run '%s' was not found."), *RunId),
			404);
	}
	if (Record.bDurableResume && !Record.Assets.IsEmpty())
	{
		return FMCPResult::Ok(Record.ToResultJson(Options));
	}
	bool bRecordChanged = false;
	if (Record.ServerInstanceId != ServerInstanceId
		&& (Record.Status == TEXT("pending")
			|| Record.Status == TEXT("running")))
	{
		Record.Status = TEXT("blocked");
		Record.RollbackStatus = TEXT("manualReview");
		Record.bRollbackAvailable = false;
		Record.bRollbackVerified = false;
		bRecordChanged = true;
	}

	const bool bUndoAvailable =
		Record.bRollbackAvailable
		&& IsTopTransaction(Record.TransactionId);
	bool bFallbackAvailable = false;
	if (Record.bRollbackAvailable
		&& Record.ServerInstanceId == ServerInstanceId)
	{
		UObject* CurrentAsset =
			LoadAssetWithoutLogging(Record.ScopeAsset);
		const bool bPostStateUnchanged =
			!Record.StructureHashAfter.IsEmpty()
			&& ComputeAssetStructureHash(CurrentAsset)
				== Record.StructureHashAfter;
		bFallbackAvailable =
			bPostStateUnchanged
			&& (!Record.bScopeExistedBefore
				|| RollbackMemorySnapshots.Contains(Record.RunId));
	}
	if (Record.bRollbackAvailable
		&& !bUndoAvailable
		&& !bFallbackAvailable)
	{
		Record.bRollbackAvailable = false;
		RollbackMemorySnapshots.Remove(Record.RunId);
		if (Record.RollbackStatus == TEXT("notRequested"))
		{
			Record.RollbackStatus = TEXT("postStateChanged");
		}
		bRecordChanged = true;
	}
	if (bRecordChanged)
	{
		FString JournalError;
		if (!SaveRun(Record, JournalError))
		{
			TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
			Details->SetStringField(TEXT("journalError"), JournalError);
			return FMCPResult::Fail(
				TEXT("journal_write_failed"),
				TEXT("Workflow status changed, but the journal could not be updated."),
				500,
				Details);
		}
		Runs.Add(Record.RunId, Record);
	}
	return FMCPResult::Ok(Record.ToResultJson(Options));
}

FMCPResult FWorkflowRuntime::ResumeRun(const FString& RunId, const FResponseOptions& Options)
{
	const FRunRecord* Existing = Runs.Find(RunId);
	FRunRecord DurableRecord;
	if (Existing)
	{
		DurableRecord = *Existing;
	}
	else
	{
		LoadRun(RunId, DurableRecord);
	}
	if (DurableRecord.bDurableResume &&
		!DurableRecord.Assets.IsEmpty())
	{
		return ResumeRunV2(DurableRecord, Options);
	}
	if (!Existing)
	{
		FRunRecord JournalRecord;
		if (LoadRun(RunId, JournalRecord)
			&& JournalRecord.ServerInstanceId != ServerInstanceId)
		{
			TSharedPtr<FJsonObject> Details = JournalRecord.ToResultJson(Options);
			Details->SetStringField(
				TEXT("resumeMode"),
				TEXT("rejectedDifferentInstance"));
			Details->SetBoolField(TEXT("reattached"), false);
			Details->SetBoolField(TEXT("resumedExecution"), false);
			return FMCPResult::Fail(
				TEXT("workflow_instance_changed"),
				TEXT("Run belongs to a previous Editor instance and cannot be resumed."),
				409,
				Details);
		}
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("runId"), RunId);
		Details->SetStringField(
			TEXT("resumeMode"),
			TEXT("rejectedUnknownRun"));
		Details->SetBoolField(TEXT("reattached"), false);
		Details->SetBoolField(TEXT("resumedExecution"), false);
		return FMCPResult::Fail(
			TEXT("workflow_run_not_recorded"),
			FString::Printf(
				TEXT("Workflow run '%s' is not recorded by this Editor instance."),
				*RunId),
			404,
			Details);
	}

	if (Existing->ServerInstanceId != ServerInstanceId)
	{
		TSharedPtr<FJsonObject> Details = Existing->ToResultJson(Options);
		Details->SetStringField(
			TEXT("resumeMode"),
			TEXT("rejectedDifferentInstance"));
		Details->SetBoolField(TEXT("reattached"), false);
		Details->SetBoolField(TEXT("resumedExecution"), false);
		return FMCPResult::Fail(
			TEXT("workflow_instance_changed"),
			TEXT("Run belongs to a previous Editor instance and cannot be resumed."),
			409,
			Details);
	}

	static const TSet<FString> TerminalStatuses = {
		TEXT("completed"),
		TEXT("failed"),
		TEXT("blocked"),
		TEXT("rolledBack"),
	};
	if (!TerminalStatuses.Contains(Existing->Status))
	{
		TSharedPtr<FJsonObject> Details = Existing->ToResultJson(Options);
		Details->SetStringField(
			TEXT("resumeMode"),
			TEXT("rejectedUnsafeContinuation"));
		Details->SetBoolField(TEXT("reattached"), false);
		Details->SetBoolField(TEXT("resumedExecution"), false);
		return FMCPResult::Fail(
			TEXT("workflow_resume_not_safe"),
			TEXT(
				"Synchronous UE Workflow v1 cannot continue a non-terminal "
				"run; inspect status and resolve it explicitly."),
			409,
			Details);
	}

	TSharedPtr<FJsonObject> Result = Existing->ToResultJson(Options);
	Result->SetStringField(
		TEXT("resumeMode"),
		TEXT("terminalReattach"));
	Result->SetBoolField(TEXT("reattached"), true);
	Result->SetBoolField(TEXT("resumedExecution"), false);
	return FMCPResult::Ok(Result);
}

FMCPResult FWorkflowRuntime::ResumeRunV2(
	FRunRecord& Record,
	const FResponseOptions& Options)
{
	if (Record.RunId.IsEmpty() ||
		!Record.NormalizedWorkflow.IsValid())
	{
		return FMCPResult::Fail(
			TEXT("workflow_run_not_found"),
			TEXT("Workflow v2 journal is missing durable run data."),
			404);
	}
	if (Record.PluginVersion.IsEmpty() ||
		Record.PluginVersion != CurrentPluginVersion())
	{
		TSharedPtr<FJsonObject> Details =
			Record.ToResultJson(Options);
		Details->SetStringField(
			TEXT("journalPluginVersion"),
			Record.PluginVersion);
		Details->SetStringField(
			TEXT("currentPluginVersion"),
			CurrentPluginVersion());
		return FMCPResult::Fail(
			TEXT("resume_conflict"),
			TEXT(
				"Workflow journal plugin version differs from the "
				"currently loaded plugin."),
			409,
			Details);
	}

	static const TSet<FString> TerminalStatuses = {
		TEXT("completed"),
		TEXT("failed"),
		TEXT("blocked"),
		TEXT("rolledBack"),
	};
	const bool bTerminal =
		TerminalStatuses.Contains(Record.Status);
	if (!bTerminal && Record.Status != TEXT("running") &&
		Record.Status != TEXT("pending"))
	{
		return FMCPResult::Fail(
			TEXT("workflow_resume_not_safe"),
			TEXT("Workflow v2 journal is not at a resumable boundary."),
			409,
			Record.ToResultJson(Options));
	}

	TSharedPtr<FJsonObject> CurrentPlan;
	FMCPResult PlanFailure;
	if (!TryPlan(
			Record.NormalizedWorkflow,
			CurrentPlan,
			PlanFailure)
		|| GetStringField(CurrentPlan, TEXT("planDigest")) !=
			Record.CorePlanDigest
		|| GetStringField(
			CurrentPlan,
			TEXT("contractSetDigest")) !=
			Record.ContractSetDigest)
	{
		TSharedPtr<FJsonObject> Details =
			Record.ToResultJson(Options);
		Details->SetStringField(
			TEXT("expectedPlanDigest"),
			Record.CorePlanDigest);
		if (CurrentPlan.IsValid())
		{
			Details->SetStringField(
				TEXT("currentPlanDigest"),
				GetStringField(
					CurrentPlan,
					TEXT("planDigest")));
		}
		return FMCPResult::Fail(
			TEXT("resume_conflict"),
			TEXT(
				"Workflow v2 contract or plan changed since the run "
				"journal was written."),
			409,
			Details);
	}

	TSharedPtr<FJsonObject> ExecutionPlan = CurrentPlan;
	if (Record.DslVersion == TEXT("1.0"))
	{
		FString AdaptError;
		if (!AdaptV1PlanToV2(
				CurrentPlan,
				ExecutionPlan,
				AdaptError))
		{
			TSharedPtr<FJsonObject> Details =
				Record.ToResultJson(Options);
			Details->SetStringField(TEXT("message"), AdaptError);
			return FMCPResult::Fail(
				TEXT("resume_conflict"),
				TEXT(
					"Workflow v1 plan can no longer be adapted to the "
					"durable execution model."),
				409,
				Details);
		}
	}

	const bool bDifferentInstance =
		Record.ServerInstanceId != ServerInstanceId;
	const bool bRestartFromBaseline =
		bDifferentInstance && !bTerminal;
	for (const TSharedPtr<FJsonValue>& AssetValue : Record.Assets)
	{
		const TSharedPtr<FJsonObject> AssetRecord =
			AssetValue.IsValid() &&
				AssetValue->Type == EJson::Object
			? AssetValue->AsObject()
			: nullptr;
		const FString ScopeId =
			GetStringField(AssetRecord, TEXT("scopeId"));
		const FString AssetPath =
			GetStringField(AssetRecord, TEXT("asset"));
		const FString ExpectedHash = GetStringField(
			AssetRecord,
			bRestartFromBaseline
				? TEXT("structureHashBefore")
				: TEXT("currentHash"));
		UObject* CurrentAsset =
			LoadAssetWithoutLogging(AssetPath);
		if (!bTerminal
			&& bDifferentInstance
			&& CurrentAsset
			&& CurrentAsset->GetOutermost()->IsDirty())
		{
			TSharedPtr<FJsonObject> Details =
				Record.ToResultJson(Options);
			Details->SetStringField(TEXT("scopeId"), ScopeId);
			Details->SetStringField(TEXT("asset"), AssetPath);
			Details->SetStringField(
				TEXT("guidance"),
				TEXT(
					"Save or revert the external edit before resuming "
					"from the staged baseline."));
			return FMCPResult::Fail(
				TEXT("resume_conflict"),
				TEXT(
					"Workflow resume refused a dirty asset loaded by "
					"the new Editor instance."),
				409,
				Details);
		}
		if (!bTerminal && !bDifferentInstance)
		{
			const FString ExpectedMemoryHash =
				GetStringField(
					AssetRecord,
					TEXT("currentMemorySha256"));
			const FString CurrentMemoryHash =
				ComputeAssetMemorySha256(CurrentAsset);
			if (ExpectedMemoryHash.IsEmpty()
				|| CurrentMemoryHash != ExpectedMemoryHash)
			{
				TSharedPtr<FJsonObject> Details =
					Record.ToResultJson(Options);
				Details->SetStringField(TEXT("scopeId"), ScopeId);
				Details->SetStringField(TEXT("asset"), AssetPath);
				Details->SetStringField(
					TEXT("expectedMemorySha256"),
					ExpectedMemoryHash);
				Details->SetStringField(
					TEXT("currentMemorySha256"),
					CurrentMemoryHash);
				return FMCPResult::Fail(
					TEXT("resume_conflict"),
					TEXT(
						"Workflow resume detected an in-memory asset "
						"edit outside the recorded segment boundary."),
					409,
					Details);
			}
		}
		const FString CurrentHash =
			ComputeAssetStructureHash(CurrentAsset);
		if (ExpectedHash.IsEmpty() ||
			CurrentHash != ExpectedHash)
		{
			TSharedPtr<FJsonObject> Details =
				Record.ToResultJson(Options);
			Details->SetStringField(TEXT("scopeId"), ScopeId);
			Details->SetStringField(TEXT("asset"), AssetPath);
			Details->SetStringField(
				TEXT("expectedHash"),
				ExpectedHash);
			Details->SetStringField(
				TEXT("currentHash"),
				CurrentHash);
			return FMCPResult::Fail(
				TEXT("resume_conflict"),
				TEXT(
					"A Workflow v2 scope changed outside the recorded "
					"execution boundary."),
				409,
				Details);
		}

		const FString PackageFilename =
			GetStringField(AssetRecord, TEXT("packageFilename"));
		const FString ExpectedPackageHash = GetStringField(
			AssetRecord,
			bRestartFromBaseline
				? TEXT("packageSha256Before")
				: TEXT("currentPackageSha256"));
		FString CurrentPackageHash;
		const bool bPackageExists =
			!PackageFilename.IsEmpty() &&
			IFileManager::Get().FileExists(*PackageFilename);
		if (bPackageExists &&
			!TryHashFile(PackageFilename, CurrentPackageHash))
		{
			CurrentPackageHash = TEXT("<unreadable>");
		}
		if ((ExpectedPackageHash.IsEmpty() && bPackageExists)
			|| (!ExpectedPackageHash.IsEmpty() &&
				CurrentPackageHash != ExpectedPackageHash))
		{
			TSharedPtr<FJsonObject> Details =
				Record.ToResultJson(Options);
			Details->SetStringField(TEXT("scopeId"), ScopeId);
			Details->SetStringField(TEXT("asset"), AssetPath);
			Details->SetStringField(
				TEXT("expectedPackageSha256"),
				ExpectedPackageHash);
			Details->SetStringField(
				TEXT("currentPackageSha256"),
				CurrentPackageHash);
			return FMCPResult::Fail(
				TEXT("resume_conflict"),
				TEXT(
					"A Workflow scope package changed outside the "
					"recorded execution boundary."),
				409,
				Details);
		}
	}

	if (bTerminal)
	{
		TSharedPtr<FJsonObject> Result =
			Record.ToResultJson(Options);
		Result->SetStringField(
			TEXT("resumeMode"),
			TEXT("terminalReattach"));
		Result->SetBoolField(TEXT("reattached"), true);
		Result->SetBoolField(
			TEXT("resumedExecution"),
			false);
		return FMCPResult::Ok(Result);
	}

	Record.ServerInstanceId = ServerInstanceId;
	Record.Status = TEXT("running");
	Runs.Add(Record.RunId, Record);
	FMCPResult Result = ContinueWorkflowV2(
		Record,
		ExecutionPlan,
		Options,
		bRestartFromBaseline);
	if (Result.bOk && Result.Data.IsValid())
	{
		Result.Data->SetStringField(
			TEXT("resumeMode"),
			bRestartFromBaseline
				? TEXT("restartFromStagedBaseline")
				: TEXT("continueFromCheckpoint"));
		Result.Data->SetBoolField(TEXT("reattached"), true);
		Result.Data->SetBoolField(
			TEXT("resumedExecution"),
			true);
	}
	return Result;
}

FMCPResult FWorkflowRuntime::Rollback(const TSharedPtr<FJsonObject>& Request,
                                      const FResponseOptions& Options)
{
	const FString RunId = GetStringField(Request, TEXT("runId"));
	FRunRecord Record;
	if (!LoadRun(RunId, Record))
	{
		return FMCPResult::Fail(
			TEXT("workflow_run_not_found"),
			FString::Printf(TEXT("Workflow run '%s' was not found."), *RunId),
			404);
	}
	if (Record.bDurableResume && !Record.Assets.IsEmpty())
	{
		FString ApprovedDigest;
		if (!Request->TryGetStringField(
				TEXT("approvePlanDigest"),
				ApprovedDigest)
			|| ApprovedDigest != Record.PlanDigest)
		{
			TSharedPtr<FJsonObject> Details =
				Record.ToResultJson(Options);
			Details->SetStringField(
				TEXT("requiredPlanDigest"),
				Record.PlanDigest);
			return FMCPResult::Fail(
				TEXT("plan_digest_mismatch"),
				TEXT(
					"Rollback requires the exact approved plan digest "
					"for the run."),
				409,
				Details);
		}
		return RollbackV2(Record, Options, false);
	}
	if (Record.ServerInstanceId != ServerInstanceId)
	{
		return FMCPResult::Fail(
		    TEXT("workflow_instance_changed"),
		    TEXT("Rollback is only available in the Editor instance that executed the run."), 409,
		    Record.ToResultJson(Options));
	}

	FString ApprovedDigest;
	if (!Request->TryGetStringField(TEXT("approvePlanDigest"), ApprovedDigest)
		|| ApprovedDigest != Record.PlanDigest)
	{
		TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
		Details->SetStringField(TEXT("requiredPlanDigest"), Record.PlanDigest);
		return FMCPResult::Fail(
			TEXT("plan_digest_mismatch"),
			TEXT("Rollback requires the exact approved plan digest for the run."),
			409,
			Details);
	}
	TArray<FWorkflowObjectMemorySnapshot>* MemorySnapshots =
		RollbackMemorySnapshots.Find(Record.RunId);
	UObject* CurrentAsset =
		LoadAssetWithoutLogging(Record.ScopeAsset);
	const bool bPostStateUnchanged =
		!Record.StructureHashAfter.IsEmpty()
		&& ComputeAssetStructureHash(CurrentAsset)
			== Record.StructureHashAfter;
	const bool bCanUndo =
		IsTopTransaction(Record.TransactionId);
	const bool bCanMemoryRestore =
		bPostStateUnchanged
		&& Record.bScopeExistedBefore
		&& MemorySnapshots
		&& !MemorySnapshots->IsEmpty();
	const bool bCanDeleteNew =
		bPostStateUnchanged
		&& !Record.bScopeExistedBefore;
	if (!Record.bRollbackAvailable
		|| (!bCanUndo && !bCanMemoryRestore && !bCanDeleteNew))
	{
		Record.bRollbackAvailable = false;
		Record.RollbackStatus = TEXT("manualReview");
		RollbackMemorySnapshots.Remove(Record.RunId);
		FString JournalError;
		if (!SaveRun(Record, JournalError))
		{
			TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
			Details->SetStringField(TEXT("journalError"), JournalError);
			return FMCPResult::Fail(
				TEXT("journal_write_failed"),
				TEXT("Rollback availability changed, but the journal could not be updated."),
				500,
				Details);
		}
		Runs.Add(Record.RunId, Record);
		return FMCPResult::Fail(
		    TEXT("workflow_rollback_not_available"),
		    TEXT("Workflow rollback is no longer safe because neither the Undo transaction nor an "
		         "unchanged in-memory post-state is available."),
		    409, Record.ToResultJson(Options));
	}

	bool bUndoApplied = false;
	if (bCanUndo && GEditor)
	{
		bUndoApplied = GEditor->UndoTransaction(false);
	}
	if (bUndoApplied && Record.bScopeExistedBefore)
	{
		UObject* RestoredAsset =
			LoadAssetWithoutLogging(Record.ScopeAsset);
		RebuildDomainAfterRollback(RestoredAsset);
		if (RestoredAsset)
		{
			RestoredAsset->GetOutermost()->SetDirtyFlag(
				Record.bPackageDirtyBefore);
		}
	}
	else if (bCanMemoryRestore)
	{
		Record.bMemorySnapshotRestoreAttempted = true;
		UObject* RestoreAsset =
			LoadAssetWithoutLogging(Record.ScopeAsset);
		const bool bRestoreCompleted =
			RestoreMemorySnapshots(RestoreAsset, *MemorySnapshots);
		if (RestoreAsset)
		{
			RestoreAsset->GetOutermost()->SetDirtyFlag(
				Record.bPackageDirtyBefore);
		}
		Record.bMemorySnapshotRestored = bRestoreCompleted;

		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(
			TEXT("kind"),
			TEXT("explicitMemorySnapshotRestore"));
		Diagnostic->SetBoolField(TEXT("attempted"), true);
		Diagnostic->SetBoolField(
			TEXT("deserializeCompleted"),
			bRestoreCompleted);
		Record.Diagnostics.Add(
			MakeShared<FJsonValueObject>(Diagnostic));
	}

	const bool bCleanupVerified = VerifyOrCleanRollback(Record);
	Record.bMemorySnapshotRestored =
		Record.bMemorySnapshotRestored && bCleanupVerified;
	Record.bRollbackVerified =
		bCleanupVerified
		&& (bUndoApplied
			|| Record.bMemorySnapshotRestored
			|| !Record.bScopeExistedBefore);
	Record.Status = Record.bRollbackVerified
		? TEXT("rolledBack")
		: TEXT("blocked");
	Record.RollbackStatus = Record.bRollbackVerified
		? TEXT("rolledBack")
		: TEXT("manualReview");
	Record.bRollbackAvailable = false;
	RollbackMemorySnapshots.Remove(Record.RunId);
	FString JournalError;
	if (!SaveRun(Record, JournalError))
	{
		Runs.Add(Record.RunId, Record);
		TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
		Details->SetStringField(TEXT("journalError"), JournalError);
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Rollback completed, but the terminal journal could not be written."),
			500,
			Details);
	}
	Runs.Add(Record.RunId, Record);
	if (!Record.bRollbackVerified)
	{
		return FMCPResult::Fail(
		    TEXT("workflow_rollback_verification_failed"),
		    TEXT("Workflow rollback could not restore and verify the pre-run scope structure."),
		    500, Record.ToResultJson(Options));
	}
	return FMCPResult::Ok(Record.ToResultJson(Options));
}

FMCPResult FWorkflowRuntime::RollbackV2(
	FRunRecord& Record,
	const FResponseOptions& Options,
	const bool bAutomatic)
{
	if (Record.Status == TEXT("rolledBack"))
	{
		return FMCPResult::Ok(Record.ToResultJson(Options));
	}
	if (!bAutomatic && !Record.bRollbackAvailable)
	{
		return FMCPResult::Fail(
			TEXT("workflow_rollback_not_available"),
			TEXT("Workflow v2 rollback is not available for this run."),
			409,
			Record.ToResultJson(Options));
	}
	if (!bAutomatic &&
		(Record.PluginVersion.IsEmpty() ||
			Record.PluginVersion != CurrentPluginVersion()))
	{
		TSharedPtr<FJsonObject> Details =
			Record.ToResultJson(Options);
		Details->SetStringField(
			TEXT("journalPluginVersion"),
			Record.PluginVersion);
		Details->SetStringField(
			TEXT("currentPluginVersion"),
			CurrentPluginVersion());
		return FMCPResult::Fail(
			TEXT("resume_conflict"),
			TEXT(
				"Workflow rollback refused a journal created by a "
				"different plugin version."),
			409,
			Details);
	}
	if (!Record.StructureAfterRollback.IsValid())
	{
		Record.StructureAfterRollback = MakeShared<FJsonObject>();
	}

	const bool bSameInstance =
		Record.ServerInstanceId == ServerInstanceId;
	if (!bAutomatic)
	{
		for (const TSharedPtr<FJsonValue>& AssetValue : Record.Assets)
		{
			const TSharedPtr<FJsonObject> AssetRecord =
				AssetValue.IsValid() && AssetValue->Type == EJson::Object
				? AssetValue->AsObject()
				: nullptr;
			const FString AssetPath = GetStringField(AssetRecord, TEXT("asset"));
			UObject* CurrentAsset =
				LoadAssetWithoutLogging(AssetPath);
			if (!bSameInstance
				&& CurrentAsset
				&& CurrentAsset->GetOutermost()->IsDirty())
			{
				TSharedPtr<FJsonObject> Details =
					Record.ToResultJson(Options);
				Details->SetStringField(
					TEXT("scopeId"),
					GetStringField(
						AssetRecord,
						TEXT("scopeId")));
				Details->SetStringField(
					TEXT("asset"),
					AssetPath);
				Details->SetStringField(
					TEXT("guidance"),
					TEXT(
						"Save or revert the external edit before "
						"rolling back from the staged baseline."));
				return FMCPResult::Fail(
					TEXT("resume_conflict"),
					TEXT(
						"Workflow v2 rollback refused a dirty asset "
						"loaded by a different Editor instance."),
					409,
					Details);
			}
			const FString CurrentHash =
				ComputeAssetStructureHash(CurrentAsset);
			const FString BeforeHash = GetStringField(
				AssetRecord,
				TEXT("structureHashBefore"));
			const FString ExpectedHash = GetStringField(
				AssetRecord,
				TEXT("currentHash"));
			if (bSameInstance)
			{
				const FString ExpectedMemoryHash =
					GetStringField(
						AssetRecord,
						TEXT("currentMemorySha256"));
				const FString BeforeMemoryHash =
					GetStringField(
						AssetRecord,
						TEXT("memorySha256Before"));
				const FString CurrentMemoryHash =
					ComputeAssetMemorySha256(CurrentAsset);
				bool bExistedBefore = false;
				AssetRecord->TryGetBoolField(
					TEXT("existedBefore"),
					bExistedBefore);
				const bool bMatchesRecordedCurrent =
					!ExpectedMemoryHash.IsEmpty()
					&& CurrentMemoryHash == ExpectedMemoryHash;
				const bool bMatchesRecordedBaseline =
					(!bExistedBefore && !CurrentAsset)
					|| (!BeforeMemoryHash.IsEmpty()
						&& CurrentMemoryHash == BeforeMemoryHash);
				if (!bMatchesRecordedCurrent
					&& !bMatchesRecordedBaseline)
				{
					TSharedPtr<FJsonObject> Details =
						Record.ToResultJson(Options);
					Details->SetStringField(
						TEXT("scopeId"),
						GetStringField(
							AssetRecord,
							TEXT("scopeId")));
					Details->SetStringField(
						TEXT("asset"),
						AssetPath);
					Details->SetStringField(
						TEXT("expectedMemorySha256"),
						ExpectedMemoryHash);
					Details->SetStringField(
						TEXT("currentMemorySha256"),
						CurrentMemoryHash);
					return FMCPResult::Fail(
						TEXT("resume_conflict"),
						TEXT(
							"Workflow v2 rollback detected an "
							"in-memory edit outside the recorded "
							"execution boundary."),
						409,
						Details);
				}
			}
			if (CurrentHash != BeforeHash
				&& (ExpectedHash.IsEmpty() || CurrentHash != ExpectedHash))
			{
				TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
				Details->SetStringField(
					TEXT("scopeId"),
					GetStringField(AssetRecord, TEXT("scopeId")));
				Details->SetStringField(TEXT("asset"), AssetPath);
				Details->SetStringField(TEXT("expectedHash"), ExpectedHash);
				Details->SetStringField(TEXT("currentHash"), CurrentHash);
				return FMCPResult::Fail(
					TEXT("resume_conflict"),
					TEXT(
						"Workflow v2 rollback refused to overwrite an "
						"externally modified scope."),
					409,
					Details);
			}

			const FString PackageFilename =
				GetStringField(
					AssetRecord,
					TEXT("packageFilename"));
			const FString ExpectedPackageHash =
				GetStringField(
					AssetRecord,
					TEXT("currentPackageSha256"));
			FString CurrentPackageHash;
			const bool bPackageExists =
				!PackageFilename.IsEmpty() &&
				IFileManager::Get().FileExists(*PackageFilename);
			if (bPackageExists &&
				!TryHashFile(PackageFilename, CurrentPackageHash))
			{
				CurrentPackageHash = TEXT("<unreadable>");
			}
			if ((ExpectedPackageHash.IsEmpty() && bPackageExists)
				|| (!ExpectedPackageHash.IsEmpty() &&
					CurrentPackageHash != ExpectedPackageHash))
			{
				TSharedPtr<FJsonObject> Details =
					Record.ToResultJson(Options);
				Details->SetStringField(
					TEXT("scopeId"),
					GetStringField(AssetRecord, TEXT("scopeId")));
				Details->SetStringField(TEXT("asset"), AssetPath);
				Details->SetStringField(
					TEXT("expectedPackageSha256"),
					ExpectedPackageHash);
				Details->SetStringField(
					TEXT("currentPackageSha256"),
					CurrentPackageHash);
				return FMCPResult::Fail(
					TEXT("resume_conflict"),
					TEXT(
						"Workflow rollback refused to overwrite an "
						"externally modified package."),
					409,
					Details);
			}
		}
	}

	const bool bUndoApplied =
		bSameInstance
		&& IsTopTransaction(Record.TransactionId)
		&& GEditor->UndoTransaction(false);
	bool bAllRestored = true;
	for (int32 Index = Record.Assets.Num() - 1; Index >= 0; --Index)
	{
		const TSharedPtr<FJsonObject> AssetRecord =
			Record.Assets[Index].IsValid()
				&& Record.Assets[Index]->Type == EJson::Object
			? Record.Assets[Index]->AsObject()
			: nullptr;
		if (!AssetRecord.IsValid())
		{
			bAllRestored = false;
			continue;
		}
		const FString ScopeId = GetStringField(AssetRecord, TEXT("scopeId"));
		const FString AssetPath = GetStringField(AssetRecord, TEXT("asset"));
		const FString BeforeHash = GetStringField(
			AssetRecord,
			TEXT("structureHashBefore"));
		UObject* Asset = LoadAssetWithoutLogging(AssetPath);
		FString CurrentHash = ComputeAssetStructureHash(Asset);
		bool bRestored = CurrentHash == BeforeHash;

		if (!bRestored && bUndoApplied)
		{
			RebuildDomainAfterRollback(Asset);
			CurrentHash = ComputeAssetStructureHash(
				LoadAssetWithoutLogging(AssetPath));
			bRestored = CurrentHash == BeforeHash;
		}

		const FString SnapshotKey =
			WorkflowAssetSnapshotKey(Record.RunId, ScopeId);
		if (!bRestored && bSameInstance)
		{
			if (TArray<FWorkflowObjectMemorySnapshot>* Snapshots =
					MultiAssetRollbackMemorySnapshots.Find(SnapshotKey))
			{
				Record.bMemorySnapshotRestoreAttempted = true;
				Asset = LoadAssetWithoutLogging(AssetPath);
				const bool bDeserialized =
					RestoreMemorySnapshots(Asset, *Snapshots);
				CurrentHash = ComputeAssetStructureHash(
					LoadAssetWithoutLogging(AssetPath));
				bRestored = bDeserialized && CurrentHash == BeforeHash;
				Record.bMemorySnapshotRestored =
					Record.bMemorySnapshotRestored || bRestored;
			}
		}

		if (!bRestored)
		{
			FString RestoreError;
			bRestored = RestoreWorkflowAssetFile(AssetRecord, RestoreError);
			if (!bRestored)
			{
				TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
				Diagnostic->SetStringField(TEXT("scopeId"), ScopeId);
				Diagnostic->SetStringField(TEXT("asset"), AssetPath);
				Diagnostic->SetStringField(TEXT("message"), RestoreError);
				Record.Diagnostics.Add(
					MakeShared<FJsonValueObject>(Diagnostic));
			}
		}

		Asset = LoadAssetWithoutLogging(AssetPath);
		CurrentHash = ComputeAssetStructureHash(Asset);
		bRestored = bRestored && CurrentHash == BeforeHash;
		bool bDirtyBefore = false;
		AssetRecord->TryGetBoolField(
			TEXT("packageDirtyBefore"),
			bDirtyBefore);
		if (bRestored && Asset)
		{
			Asset->GetOutermost()->SetDirtyFlag(bDirtyBefore);
		}
		AssetRecord->SetStringField(
			TEXT("structureHashAfterRollback"),
			CurrentHash);
		AssetRecord->SetStringField(TEXT("currentHash"), CurrentHash);
		const FString RestoredMemoryHash =
			ComputeAssetMemorySha256(Asset);
		if (!RestoredMemoryHash.IsEmpty())
		{
			AssetRecord->SetStringField(
				TEXT("currentMemorySha256"),
				RestoredMemoryHash);
		}
		else
		{
			AssetRecord->Values.Remove(
				TEXT("currentMemorySha256"));
		}
		const FString PackageFilename =
			GetStringField(AssetRecord, TEXT("packageFilename"));
		FString RestoredPackageHash;
		if (!PackageFilename.IsEmpty() &&
			IFileManager::Get().FileExists(*PackageFilename) &&
			TryHashFile(PackageFilename, RestoredPackageHash))
		{
			AssetRecord->SetStringField(
				TEXT("currentPackageSha256"),
				RestoredPackageHash);
		}
		else
		{
			AssetRecord->Values.Remove(
				TEXT("currentPackageSha256"));
		}
		Record.StructureAfterRollback->SetObjectField(
			ScopeId,
			CaptureAssetStructure(Asset));
		bAllRestored = bAllRestored && bRestored;
		MultiAssetRollbackMemorySnapshots.Remove(SnapshotKey);
	}
	if (Record.DslVersion == TEXT("1.0") &&
		Record.Assets.Num() == 1 &&
		Record.Assets[0].IsValid() &&
		Record.Assets[0]->Type == EJson::Object)
	{
		Record.StructureHashAfterRollback =
			GetStringField(
				Record.Assets[0]->AsObject(),
				TEXT("structureHashAfterRollback"));
	}

	Record.bRollbackVerified = bAllRestored;
	Record.bRollbackAvailable = false;
	Record.RollbackStatus =
		bAllRestored ? TEXT("rolledBack") : TEXT("manualReview");
	Record.Status = bAllRestored
		? (bAutomatic ? TEXT("failed") : TEXT("rolledBack"))
		: TEXT("blocked");
	Record.CurrentPhase =
		bAllRestored ? TEXT("rolledBack") : TEXT("blocked");
	FString JournalError;
	if (!SaveRun(Record, JournalError))
	{
		Runs.Add(Record.RunId, Record);
		TSharedPtr<FJsonObject> Details = Record.ToResultJson(Options);
		Details->SetStringField(TEXT("journalError"), JournalError);
		return FMCPResult::Fail(
			TEXT("journal_write_failed"),
			TEXT("Workflow v2 rollback journal could not be written."),
			500,
			Details);
	}
	Runs.Add(Record.RunId, Record);
	if (bAllRestored)
	{
		return FMCPResult::Ok(Record.ToResultJson(Options));
	}
	return FMCPResult::Fail(
		TEXT("workflow_rollback_verification_failed"),
		TEXT(
			"Workflow v2 could not restore and verify its entire asset set."),
		500,
		Record.ToResultJson(Options));
}

TSharedPtr<FJsonObject> FWorkflowRuntime::FRunRecord::ToJournalJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("ue.workflow-run.v1"));
	Json->SetStringField(TEXT("runId"), RunId);
	Json->SetStringField(TEXT("serverInstanceId"), ServerInstanceId);
	Json->SetStringField(TEXT("workflowId"), WorkflowId);
	Json->SetStringField(TEXT("dslVersion"), DslVersion);
	Json->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Json->SetStringField(TEXT("scopeAsset"), ScopeAsset);
	Json->SetStringField(TEXT("planDigest"), PlanDigest);
	Json->SetStringField(TEXT("corePlanDigest"), CorePlanDigest);
	Json->SetStringField(TEXT("contractSetDigest"), ContractSetDigest);
	Json->SetStringField(TEXT("transactionId"), TransactionId);
	Json->SetStringField(TEXT("status"), Status);
	Json->SetStringField(TEXT("rollbackStatus"), RollbackStatus);
	Json->SetStringField(TEXT("structureHashBefore"), StructureHashBefore);
	Json->SetStringField(TEXT("structureHashAfter"), StructureHashAfter);
	Json->SetStringField(
		TEXT("structureHashAfterRollback"),
		StructureHashAfterRollback);
	Json->SetStringField(TEXT("currentPhase"), CurrentPhase);
	Json->SetNumberField(
		TEXT("nextInitializerIndex"),
		NextInitializerIndex);
	Json->SetNumberField(
		TEXT("nextOperationIndex"),
		NextOperationIndex);
	Json->SetNumberField(
		TEXT("nextFinalizerIndex"),
		NextFinalizerIndex);
	Json->SetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess);
	Json->SetBoolField(TEXT("durableResume"), bDurableResume);
	Json->SetBoolField(TEXT("scopeExistedBefore"), bScopeExistedBefore);
	Json->SetBoolField(TEXT("packageDirtyBefore"), bPackageDirtyBefore);
	Json->SetBoolField(
		TEXT("memorySnapshotCaptured"),
		bMemorySnapshotCaptured);
	Json->SetBoolField(
		TEXT("memorySnapshotRestoreAttempted"),
		bMemorySnapshotRestoreAttempted);
	Json->SetBoolField(
		TEXT("memorySnapshotRestored"),
		bMemorySnapshotRestored);
	Json->SetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	Json->SetBoolField(TEXT("rollbackVerified"), bRollbackVerified);
	Json->SetArrayField(TEXT("operations"), Operations);
	Json->SetArrayField(TEXT("finalizers"), Finalizers);
	Json->SetArrayField(TEXT("dirtyPackages"), DirtyPackages);
	Json->SetObjectField(
		TEXT("readBack"),
		ReadBack.IsValid() ? ReadBack : MakeShared<FJsonObject>());
	Json->SetObjectField(
		TEXT("assetDiff"),
		AssetDiff.IsValid() ? AssetDiff : MakeShared<FJsonObject>());
	Json->SetObjectField(
		TEXT("structureBefore"),
		StructureBefore.IsValid()
			? StructureBefore
			: MakeShared<FJsonObject>());
	Json->SetObjectField(
		TEXT("structureAfter"),
		StructureAfter.IsValid()
			? StructureAfter
			: MakeShared<FJsonObject>());
	Json->SetObjectField(
		TEXT("structureAfterRollback"),
		StructureAfterRollback.IsValid()
			? StructureAfterRollback
			: MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetStringField(TEXT("status"), RollbackStatus);
	Rollback->SetBoolField(TEXT("available"), bRollbackAvailable);
	Rollback->SetBoolField(TEXT("verified"), bRollbackVerified);
	Rollback->SetBoolField(
		TEXT("executionMemorySnapshotCaptured"),
		bMemorySnapshotCaptured);
	Rollback->SetBoolField(
		TEXT("executionMemoryRestoreAttempted"),
		bMemorySnapshotRestoreAttempted);
	Rollback->SetBoolField(
		TEXT("executionMemoryRestored"),
		bMemorySnapshotRestored);
	Rollback->SetStringField(
		TEXT("structureHashAfter"),
		StructureHashAfterRollback);
	Json->SetObjectField(TEXT("rollback"), Rollback);
	Json->SetArrayField(TEXT("diagnostics"), Diagnostics);
	Json->SetArrayField(TEXT("assets"), Assets);
	Json->SetObjectField(
		TEXT("normalizedWorkflow"),
		NormalizedWorkflow.IsValid()
			? NormalizedWorkflow
			: MakeShared<FJsonObject>());
	Json->SetObjectField(
		TEXT("plan"),
		StoredPlan.IsValid()
			? StoredPlan
			: MakeShared<FJsonObject>());
	Json->SetObjectField(
		TEXT("operationOutputs"),
		OperationOutputs.IsValid()
			? OperationOutputs
			: MakeShared<FJsonObject>());
	return Json;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::FRunRecord::ToReceiptJson() const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("ue.workflow-run.v1"));
	Json->SetStringField(TEXT("runId"), RunId);
	Json->SetStringField(TEXT("serverInstanceId"), ServerInstanceId);
	Json->SetStringField(TEXT("workflowId"), WorkflowId);
	Json->SetStringField(TEXT("dslVersion"), DslVersion);
	Json->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Json->SetStringField(TEXT("scopeAsset"), ScopeAsset);
	Json->SetStringField(TEXT("planDigest"), PlanDigest);
	Json->SetStringField(TEXT("corePlanDigest"), CorePlanDigest);
	Json->SetStringField(TEXT("contractSetDigest"), ContractSetDigest);
	Json->SetStringField(TEXT("status"), Status);
	Json->SetStringField(TEXT("rollbackStatus"), RollbackStatus);
	Json->SetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	Json->SetBoolField(TEXT("rollbackVerified"), bRollbackVerified);
	Json->SetStringField(TEXT("structureHashBefore"), StructureHashBefore);
	Json->SetStringField(TEXT("structureHashAfter"), StructureHashAfter);
	Json->SetStringField(TEXT("structureHashAfterRollback"), StructureHashAfterRollback);
	Json->SetStringField(TEXT("currentPhase"), CurrentPhase);
	Json->SetNumberField(TEXT("assetCount"), Assets.Num());
	Json->SetNumberField(
		TEXT("nextInitializerIndex"),
		NextInitializerIndex);
	Json->SetNumberField(
		TEXT("nextOperationIndex"),
		NextOperationIndex);
	Json->SetNumberField(
		TEXT("nextFinalizerIndex"),
		NextFinalizerIndex);
	Json->SetBoolField(TEXT("durableResume"), bDurableResume);
	if (DslVersion == TEXT("2.0"))
	{
		TArray<TSharedPtr<FJsonValue>> AssetSet;
		for (const TSharedPtr<FJsonValue>& Value : Assets)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
			for (const TCHAR* Field : {
				TEXT("scopeId"),
				TEXT("kind"),
				TEXT("asset"),
				TEXT("structureHashBefore"),
				TEXT("structureHashAfter"),
				TEXT("structureHashAfterRollback"),
				TEXT("packageSha256Before"),
				TEXT("currentPackageSha256")})
			{
				FString Text;
				if (Value->AsObject()->TryGetStringField(Field, Text))
				{
					Summary->SetStringField(Field, Text);
				}
			}
			AssetSet.Add(MakeShared<FJsonValueObject>(Summary));
		}
		Json->SetArrayField(TEXT("assetSet"), AssetSet);
	}
	return Json;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::FRunRecord::ToResultJson(
    const FResponseOptions& Options) const
{
	TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("ue.workflow-result.v1"));
	Json->SetStringField(TEXT("runId"), RunId);
	Json->SetStringField(TEXT("workflowId"), WorkflowId);
	Json->SetStringField(TEXT("dslVersion"), DslVersion);
	Json->SetStringField(TEXT("scopeAsset"), ScopeAsset);
	Json->SetStringField(TEXT("status"), Status);
	Json->SetStringField(TEXT("planDigest"), PlanDigest);
	Json->SetStringField(TEXT("corePlanDigest"), CorePlanDigest);
	Json->SetStringField(TEXT("contractSetDigest"), ContractSetDigest);
	Json->SetStringField(TEXT("rollbackStatus"), RollbackStatus);
	Json->SetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	Json->SetBoolField(TEXT("rollbackVerified"), bRollbackVerified);
	Json->SetStringField(TEXT("currentPhase"), CurrentPhase);
	Json->SetNumberField(TEXT("assetCount"), Assets.Num());
	Json->SetStringField(TEXT("detailLevel"),
	                     Options.DetailLevel == EDetailLevel::Full       ? TEXT("full")
	                     : Options.DetailLevel == EDetailLevel::Standard ? TEXT("standard")
	                                                                     : TEXT("summary"));
	const bool bAssetSetExecution =
		bDurableResume && !Assets.IsEmpty();
	bool bChanged = false;
	if (AssetDiff.IsValid())
	{
		AssetDiff->TryGetBoolField(TEXT("changed"), bChanged);
		if (bAssetSetExecution)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
				AssetDiff->Values)
			{
				bool bAssetChanged = false;
				if (Pair.Value.IsValid()
					&& Pair.Value->Type == EJson::Object
					&& Pair.Value->AsObject()->TryGetBoolField(
						TEXT("changed"),
						bAssetChanged)
					&& bAssetChanged)
				{
					bChanged = true;
					break;
				}
			}
		}
	}
	bool bCompiled = bAssetSetExecution;
	int32 SuccessfulCompileCount = 0;
	for (const TSharedPtr<FJsonValue>& FinalizerValue : Finalizers)
	{
		if (!FinalizerValue.IsValid()
			|| FinalizerValue->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Finalizer = FinalizerValue->AsObject();
		FString Id;
		FString FinalizerStatus;
		Finalizer->TryGetStringField(TEXT("id"), Id);
		Finalizer->TryGetStringField(TEXT("status"), FinalizerStatus);
		if ((Id == TEXT("$finalizer.compile")
				|| Id.EndsWith(TEXT(".compile")))
			&& FinalizerStatus == TEXT("succeeded"))
		{
			++SuccessfulCompileCount;
			if (!bAssetSetExecution)
			{
				bCompiled = true;
				break;
			}
		}
	}
	if (bAssetSetExecution)
	{
		bCompiled =
			SuccessfulCompileCount == Assets.Num();
	}
	const bool bCompleted = Status == TEXT("completed");
	auto ProjectMutationDiagnostics = [this](const bool bWarnings)
	{
		TArray<TSharedPtr<FJsonValue>> Projected;
		for (const TSharedPtr<FJsonValue>& DiagnosticValue : Diagnostics)
		{
			if (!DiagnosticValue.IsValid())
			{
				continue;
			}
			if (DiagnosticValue->Type != EJson::Object)
			{
				if (!bWarnings)
				{
					Projected.Add(DiagnosticValue);
				}
				continue;
			}

			const TSharedPtr<FJsonObject> Diagnostic = DiagnosticValue->AsObject();
			FString Severity;
			Diagnostic->TryGetStringField(TEXT("severity"), Severity);
			const bool bIsWarning = Severity.Equals(TEXT("warning"), ESearchCase::IgnoreCase);
			if (bIsWarning != bWarnings)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Compact = MakeShared<FJsonObject>();
			for (const TCHAR* FieldName : {TEXT("severity"), TEXT("code"), TEXT("message")})
			{
				FString Value;
				if (Diagnostic->TryGetStringField(FieldName, Value) && !Value.IsEmpty())
				{
					Compact->SetStringField(FieldName, Value);
				}
			}
			Projected.Add(MakeShared<FJsonValueObject>(Compact));
		}
		return Projected;
	};
	TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
	Mutation->SetBoolField(TEXT("changed"), bChanged);
	Mutation->SetBoolField(TEXT("compiled"), bCompiled);
	if (bSaveOnSuccess)
	{
		Mutation->SetBoolField(TEXT("saved"), bCompleted);
	}
	else
	{
		Mutation->SetField(
			TEXT("saved"),
			MakeShared<FJsonValueNull>());
	}
	Mutation->SetField(
		TEXT("reloaded"),
		MakeShared<FJsonValueNull>());
	Mutation->SetBoolField(TEXT("verified"), bCompleted);
	Mutation->SetStringField(TEXT("beforeHash"), StructureHashBefore);
	Mutation->SetStringField(TEXT("afterHash"), StructureHashAfter);
	Mutation->SetArrayField(
		TEXT("warnings"),
		ProjectMutationDiagnostics(true));
	Mutation->SetArrayField(
		TEXT("errors"),
		bCompleted
			? TArray<TSharedPtr<FJsonValue>>()
			: ProjectMutationDiagnostics(false));
	Json->SetObjectField(TEXT("mutation"), Mutation);
	Json->SetObjectField(TEXT("receipt"), ToReceiptJson());
	auto MakeStatusCounts = [](const TArray<TSharedPtr<FJsonValue>>& Records)
	{
		TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
		Counts->SetNumberField(TEXT("total"), Records.Num());
		TMap<FString, int32> StatusCounts;
		for (const TSharedPtr<FJsonValue>& Value : Records)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			FString RecordStatus;
			if (Value->AsObject()->TryGetStringField(TEXT("status"), RecordStatus) &&
			    !RecordStatus.IsEmpty())
			{
				++StatusCounts.FindOrAdd(RecordStatus);
			}
		}
		TArray<FString> StatusNames;
		StatusCounts.GetKeys(StatusNames);
		StatusNames.Sort();
		for (const FString& StatusName : StatusNames)
		{
			Counts->SetNumberField(StatusName, StatusCounts.FindChecked(StatusName));
		}
		return Counts;
	};

	TSharedPtr<FJsonObject> DiffSummary = MakeShared<FJsonObject>();
	DiffSummary->SetBoolField(TEXT("changed"), bChanged);
	if (AssetDiff.IsValid())
	{
		const TSharedPtr<FJsonObject>* ExistingSummary = nullptr;
		if (AssetDiff->TryGetObjectField(TEXT("summary"), ExistingSummary) && ExistingSummary &&
		    ExistingSummary->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*ExistingSummary)->Values)
			{
				DiffSummary->SetField(Field.Key, Field.Value);
			}
		}
		if (bAssetSetExecution)
		{
			int32 Added = 0;
			int32 Removed = 0;
			int32 Changed = 0;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
				AssetDiff->Values)
			{
				if (!Pair.Value.IsValid()
					|| Pair.Value->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* AssetSummary = nullptr;
				if (!Pair.Value->AsObject()->TryGetObjectField(
						TEXT("summary"),
						AssetSummary)
					|| !AssetSummary || !AssetSummary->IsValid())
				{
					continue;
				}
				Added += static_cast<int32>(
					(*AssetSummary)->GetNumberField(TEXT("added")));
				Removed += static_cast<int32>(
					(*AssetSummary)->GetNumberField(TEXT("removed")));
				Changed += static_cast<int32>(
					(*AssetSummary)->GetNumberField(TEXT("changed")));
			}
			DiffSummary->SetNumberField(TEXT("added"), Added);
			DiffSummary->SetNumberField(TEXT("removed"), Removed);
			DiffSummary->SetNumberField(TEXT("changed"), Changed);
			DiffSummary->SetNumberField(
				TEXT("assetCount"),
				Assets.Num());
		}
	}
	DiffSummary->SetStringField(TEXT("beforeHash"), StructureHashBefore);
	DiffSummary->SetStringField(TEXT("afterHash"), StructureHashAfter);

	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetObjectField(TEXT("operations"), MakeStatusCounts(Operations));
	Summary->SetObjectField(TEXT("finalizers"), MakeStatusCounts(Finalizers));
	Summary->SetNumberField(TEXT("dirtyPackageCount"), DirtyPackages.Num());
	Summary->SetNumberField(TEXT("diagnosticCount"), Diagnostics.Num());
	Summary->SetObjectField(TEXT("diff"), DiffSummary);
	TSharedPtr<FJsonObject> RollbackSummary = MakeShared<FJsonObject>();
	RollbackSummary->SetStringField(TEXT("status"), RollbackStatus);
	RollbackSummary->SetBoolField(TEXT("available"), bRollbackAvailable);
	RollbackSummary->SetBoolField(TEXT("verified"), bRollbackVerified);
	Summary->SetObjectField(TEXT("rollback"), RollbackSummary);
	Json->SetObjectField(TEXT("summary"), Summary);

	static const TArray<FString> AvailableSectionNames = {
	    TEXT("operations"), TEXT("finalizers"), TEXT("readBack"),    TEXT("assetDiff"),
	    TEXT("structures"), TEXT("rollback"),   TEXT("diagnostics"),
	};
	TArray<TSharedPtr<FJsonValue>> AvailableSections;
	for (const FString& SectionName : AvailableSectionNames)
	{
		AvailableSections.Add(MakeShared<FJsonValueString>(SectionName));
	}
	Json->SetArrayField(TEXT("availableSections"), AvailableSections);
	TSharedPtr<FJsonObject> ResultRef = MakeShared<FJsonObject>();
	ResultRef->SetStringField(TEXT("runId"), RunId);
	ResultRef->SetStringField(TEXT("action"), TEXT("status"));
	ResultRef->SetArrayField(TEXT("availableSections"), AvailableSections);
	Json->SetObjectField(TEXT("resultRef"), ResultRef);

	auto StripRecordData = [](const TArray<TSharedPtr<FJsonValue>>& Records)
	{
		TArray<TSharedPtr<FJsonValue>> Projected;
		Projected.Reserve(Records.Num());
		for (const TSharedPtr<FJsonValue>& Value : Records)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Value->AsObject()->Values)
			{
				if (Field.Key != TEXT("data"))
				{
					Record->SetField(Field.Key, Field.Value);
				}
			}
			Projected.Add(MakeShared<FJsonValueObject>(Record));
		}
		return Projected;
	};

	auto ShouldInclude = [&Options](const FString& SectionName)
	{
		if (Options.DetailLevel == EDetailLevel::Full || Options.Sections.Contains(SectionName))
		{
			return true;
		}
		return Options.DetailLevel == EDetailLevel::Standard &&
		       (SectionName == TEXT("operations") || SectionName == TEXT("finalizers") ||
		        SectionName == TEXT("diagnostics"));
	};
	auto ProjectSingleScopeSection =
		[this, bAssetSetExecution](
			const TSharedPtr<FJsonObject>& Section)
			-> TSharedPtr<FJsonObject>
	{
		if (DslVersion != TEXT("1.0") ||
			!bAssetSetExecution ||
			!Section.IsValid())
		{
			return Section.IsValid()
				? Section
				: MakeShared<FJsonObject>();
		}
		const TSharedPtr<FJsonObject>* Primary = nullptr;
		return Section->TryGetObjectField(TEXT("primary"), Primary)
				&& Primary && Primary->IsValid()
			? *Primary
			: Section;
	};

	TSharedPtr<FJsonObject> Sections = MakeShared<FJsonObject>();
	if (ShouldInclude(TEXT("operations")))
	{
		Sections->SetArrayField(TEXT("operations"),
		                        Options.DetailLevel == EDetailLevel::Full ||
		                                Options.Sections.Contains(TEXT("operations"))
		                            ? Operations
		                            : StripRecordData(Operations));
	}
	if (ShouldInclude(TEXT("finalizers")))
	{
		// Read-back and diff payloads live in their dedicated sections.
		Sections->SetArrayField(TEXT("finalizers"), StripRecordData(Finalizers));
	}
	if (ShouldInclude(TEXT("readBack")))
	{
		Sections->SetObjectField(TEXT("readBack"),
		                         ProjectSingleScopeSection(ReadBack));
	}
	if (ShouldInclude(TEXT("assetDiff")))
	{
		Sections->SetObjectField(TEXT("assetDiff"),
		                         ProjectSingleScopeSection(AssetDiff));
	}
	if (ShouldInclude(TEXT("structures")))
	{
		TSharedPtr<FJsonObject> Structures = MakeShared<FJsonObject>();
		Structures->SetObjectField(
			TEXT("before"),
			ProjectSingleScopeSection(StructureBefore));
		Structures->SetObjectField(
			TEXT("after"),
			ProjectSingleScopeSection(StructureAfter));
		Structures->SetObjectField(
			TEXT("afterRollback"),
			ProjectSingleScopeSection(StructureAfterRollback));
		Sections->SetObjectField(TEXT("structures"), Structures);
	}
	if (ShouldInclude(TEXT("rollback")))
	{
		TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
		Rollback->SetStringField(TEXT("status"), RollbackStatus);
		Rollback->SetBoolField(TEXT("available"), bRollbackAvailable);
		Rollback->SetBoolField(TEXT("verified"), bRollbackVerified);
		Rollback->SetBoolField(TEXT("executionMemorySnapshotCaptured"), bMemorySnapshotCaptured);
		Rollback->SetBoolField(TEXT("executionMemoryRestoreAttempted"),
		                       bMemorySnapshotRestoreAttempted);
		Rollback->SetBoolField(TEXT("executionMemoryRestored"), bMemorySnapshotRestored);
		Rollback->SetStringField(TEXT("structureHashAfter"), StructureHashAfterRollback);
		Sections->SetObjectField(TEXT("rollback"), Rollback);
	}
	if (ShouldInclude(TEXT("diagnostics")))
	{
		Sections->SetArrayField(TEXT("diagnostics"), Diagnostics);
	}
	if (!Sections->Values.IsEmpty())
	{
		Json->SetObjectField(TEXT("sections"), Sections);
	}

	if (Options.bUsedDeprecatedDetails)
	{
		Json->SetArrayField(TEXT("deprecations"),
		                    {
		                        MakeShared<FJsonValueString>(
		                            TEXT("'details' is deprecated; use 'detailLevel' instead.")),
		                    });
	}
	return Json;
}

bool FWorkflowRuntime::FRunRecord::FromJson(
	const TSharedPtr<FJsonObject>& Json,
	FRunRecord& OutRecord)
{
	FString Schema;
	if (!Json.IsValid()
		|| !Json->TryGetStringField(TEXT("schema"), Schema)
		|| Schema != TEXT("ue.workflow-run.v1")
		|| !Json->TryGetStringField(TEXT("runId"), OutRecord.RunId)
		|| OutRecord.RunId.IsEmpty())
	{
		return false;
	}
	Json->TryGetStringField(TEXT("serverInstanceId"), OutRecord.ServerInstanceId);
	Json->TryGetStringField(TEXT("workflowId"), OutRecord.WorkflowId);
	Json->TryGetStringField(TEXT("dslVersion"), OutRecord.DslVersion);
	Json->TryGetStringField(
		TEXT("pluginVersion"),
		OutRecord.PluginVersion);
	Json->TryGetStringField(TEXT("scopeAsset"), OutRecord.ScopeAsset);
	Json->TryGetStringField(TEXT("planDigest"), OutRecord.PlanDigest);
	Json->TryGetStringField(
		TEXT("corePlanDigest"),
		OutRecord.CorePlanDigest);
	Json->TryGetStringField(
		TEXT("contractSetDigest"),
		OutRecord.ContractSetDigest);
	Json->TryGetStringField(TEXT("transactionId"), OutRecord.TransactionId);
	Json->TryGetStringField(TEXT("status"), OutRecord.Status);
	Json->TryGetStringField(TEXT("rollbackStatus"), OutRecord.RollbackStatus);
	Json->TryGetStringField(
		TEXT("structureHashBefore"),
		OutRecord.StructureHashBefore);
	Json->TryGetStringField(
		TEXT("structureHashAfter"),
		OutRecord.StructureHashAfter);
	Json->TryGetStringField(
		TEXT("structureHashAfterRollback"),
		OutRecord.StructureHashAfterRollback);
	Json->TryGetStringField(TEXT("currentPhase"), OutRecord.CurrentPhase);
	double NumericIndex = 0.0;
	if (Json->TryGetNumberField(
			TEXT("nextInitializerIndex"),
			NumericIndex))
	{
		OutRecord.NextInitializerIndex =
			static_cast<int32>(NumericIndex);
	}
	if (Json->TryGetNumberField(
			TEXT("nextOperationIndex"),
			NumericIndex))
	{
		OutRecord.NextOperationIndex =
			static_cast<int32>(NumericIndex);
	}
	if (Json->TryGetNumberField(
			TEXT("nextFinalizerIndex"),
			NumericIndex))
	{
		OutRecord.NextFinalizerIndex =
			static_cast<int32>(NumericIndex);
	}
	Json->TryGetBoolField(TEXT("saveOnSuccess"), OutRecord.bSaveOnSuccess);
	Json->TryGetBoolField(TEXT("durableResume"), OutRecord.bDurableResume);
	Json->TryGetBoolField(
		TEXT("scopeExistedBefore"),
		OutRecord.bScopeExistedBefore);
	Json->TryGetBoolField(
		TEXT("packageDirtyBefore"),
		OutRecord.bPackageDirtyBefore);
	Json->TryGetBoolField(
		TEXT("memorySnapshotCaptured"),
		OutRecord.bMemorySnapshotCaptured);
	Json->TryGetBoolField(
		TEXT("memorySnapshotRestoreAttempted"),
		OutRecord.bMemorySnapshotRestoreAttempted);
	Json->TryGetBoolField(
		TEXT("memorySnapshotRestored"),
		OutRecord.bMemorySnapshotRestored);
	Json->TryGetBoolField(TEXT("rollbackAvailable"), OutRecord.bRollbackAvailable);
	Json->TryGetBoolField(
		TEXT("rollbackVerified"),
		OutRecord.bRollbackVerified);

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Json->TryGetArrayField(TEXT("operations"), Values) && Values)
	{
		OutRecord.Operations = *Values;
	}
	if (Json->TryGetArrayField(TEXT("finalizers"), Values) && Values)
	{
		OutRecord.Finalizers = *Values;
	}
	if (Json->TryGetArrayField(TEXT("dirtyPackages"), Values) && Values)
	{
		OutRecord.DirtyPackages = *Values;
	}
	if (Json->TryGetArrayField(TEXT("diagnostics"), Values) && Values)
	{
		OutRecord.Diagnostics = *Values;
	}
	if (Json->TryGetArrayField(TEXT("assets"), Values) && Values)
	{
		OutRecord.Assets = *Values;
	}
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (Json->TryGetObjectField(TEXT("readBack"), Object)
		&& Object
		&& Object->IsValid())
	{
		OutRecord.ReadBack = *Object;
	}
	if (Json->TryGetObjectField(TEXT("assetDiff"), Object)
		&& Object
		&& Object->IsValid())
	{
		OutRecord.AssetDiff = *Object;
	}
	if (Json->TryGetObjectField(TEXT("structureBefore"), Object)
		&& Object
		&& Object->IsValid())
	{
		OutRecord.StructureBefore = *Object;
	}
	if (Json->TryGetObjectField(TEXT("structureAfter"), Object)
		&& Object
		&& Object->IsValid())
	{
		OutRecord.StructureAfter = *Object;
	}
	if (Json->TryGetObjectField(TEXT("structureAfterRollback"), Object)
		&& Object
		&& Object->IsValid())
	{
		OutRecord.StructureAfterRollback = *Object;
	}
	if (Json->TryGetObjectField(TEXT("normalizedWorkflow"), Object)
		&& Object && Object->IsValid())
	{
		OutRecord.NormalizedWorkflow = *Object;
	}
	if (Json->TryGetObjectField(TEXT("plan"), Object)
		&& Object && Object->IsValid())
	{
		OutRecord.StoredPlan = *Object;
	}
	if (Json->TryGetObjectField(TEXT("operationOutputs"), Object)
		&& Object && Object->IsValid())
	{
		OutRecord.OperationOutputs = *Object;
	}
	return true;
}

bool FWorkflowRuntime::LoadRun(
	const FString& RunId,
	FRunRecord& OutRecord) const
{
	if (const FRunRecord* Existing = Runs.Find(RunId))
	{
		OutRecord = *Existing;
		return true;
	}

	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *GetRunPath(RunId)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Json;
	return ParseJsonObject(JsonString, Json)
		&& FRunRecord::FromJson(Json, OutRecord);
}

bool FWorkflowRuntime::SaveRun(
	const FRunRecord& Record,
	FString& OutError) const
{
	if (Record.RunId.IsEmpty())
	{
		OutError = TEXT("Cannot save a workflow journal without runId.");
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*JournalDirectory, true))
	{
		OutError = FString::Printf(
			TEXT("Could not create journal directory '%s'."),
			*JournalDirectory);
		return false;
	}

	const FString FinalPath = GetRunPath(Record.RunId);
	const FString TemporaryPath = FinalPath + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(JsonStringify(Record.ToJournalJson()), *TemporaryPath,
	                                   FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(
			TEXT("Could not write journal '%s'."),
			*TemporaryPath);
		return false;
	}
	if (!IFileManager::Get().Move(
		*FinalPath,
		*TemporaryPath,
		true,
		true,
		false,
		true))
	{
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		OutError = FString::Printf(
			TEXT("Could not publish journal '%s'."),
			*FinalPath);
		return false;
	}
	return true;
}

FString FWorkflowRuntime::GetRunPath(const FString& RunId) const
{
	FGuid Parsed;
	if (!FGuid::Parse(RunId, Parsed))
	{
		return FPaths::Combine(JournalDirectory, TEXT("invalid-run-id.json"));
	}
	return FPaths::Combine(
		JournalDirectory,
		Parsed.ToString(EGuidFormats::DigitsWithHyphensLower) + TEXT(".json"));
}

FString FWorkflowRuntime::GetRunDirectory(const FString& RunId) const
{
	FGuid Parsed;
	if (!FGuid::Parse(RunId, Parsed))
	{
		return FPaths::Combine(
			JournalDirectory,
			TEXT("invalid-run-id"));
	}
	return FPaths::Combine(
		JournalDirectory,
		Parsed.ToString(EGuidFormats::DigitsWithHyphensLower));
}

FString FWorkflowRuntime::JsonStringify(
	const TSharedPtr<FJsonObject>& Json)
{
	FString Result;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Result);
	if (Json.IsValid())
	{
		FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	}
	return Result;
}

bool FWorkflowRuntime::ParseJsonObject(
	const FString& Json,
	TSharedPtr<FJsonObject>& OutObject)
{
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, OutObject)
		&& OutObject.IsValid();
}

TSharedPtr<FJsonObject> FWorkflowRuntime::CloneObject(
	const TSharedPtr<FJsonObject>& Object)
{
	TSharedPtr<FJsonObject> Clone;
	if (!Object.IsValid()
		|| !ParseJsonObject(JsonStringify(Object), Clone))
	{
		return MakeShared<FJsonObject>();
	}
	return Clone;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::MakeDiagnostics(
	const TArray<FString>& Errors)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Error : Errors)
	{
		Values.Add(MakeShared<FJsonValueString>(Error));
	}
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("diagnostics"), Values);
	return Details;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::MakeCoreDiagnostics(
	const ue::workflow::Result& Result)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const ue::workflow::Diagnostic& Diagnostic : Result.diagnostics)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(
			TEXT("severity"),
			UTF8_TO_TCHAR(ue::workflow::SeverityName(Diagnostic.severity)));
		Json->SetStringField(TEXT("phase"), FromUtf8(Diagnostic.phase));
		Json->SetStringField(TEXT("code"), FromUtf8(Diagnostic.code));
		Json->SetStringField(TEXT("path"), FromUtf8(Diagnostic.path));
		Json->SetStringField(TEXT("message"), FromUtf8(Diagnostic.message));
		if (!Diagnostic.operation_id.empty())
		{
			Json->SetStringField(
				TEXT("operationId"),
				FromUtf8(Diagnostic.operation_id));
		}
		if (!Diagnostic.next_action.empty())
		{
			Json->SetStringField(
				TEXT("nextAction"),
				FromUtf8(Diagnostic.next_action));
		}
		Values.Add(MakeShared<FJsonValueObject>(Json));
	}
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetNumberField(TEXT("exitCode"), Result.exit_code);
	Details->SetArrayField(TEXT("diagnostics"), Values);
	return Details;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::CaptureAssetStructure(UObject* Asset)
{
	TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(
		TEXT("snapshotKind"),
		TEXT("structuralVerification"));
	Snapshot->SetBoolField(TEXT("restoreAvailable"), false);
	Snapshot->SetBoolField(TEXT("exists"), Asset != nullptr);
	if (!Asset)
	{
		return Snapshot;
	}

	Snapshot->SetStringField(TEXT("assetPath"), Asset->GetPathName());
	Snapshot->SetStringField(TEXT("assetClass"), Asset->GetClass()->GetName());

	auto SortById = [](TArray<TSharedPtr<FJsonValue>>& Values)
	{
		Values.Sort(
			[](const TSharedPtr<FJsonValue>& Left,
				const TSharedPtr<FJsonValue>& Right)
			{
				FString LeftId;
				FString RightId;
				if (Left.IsValid() && Left->Type == EJson::Object)
				{
					Left->AsObject()->TryGetStringField(TEXT("id"), LeftId);
				}
				if (Right.IsValid() && Right->Type == EJson::Object)
				{
					Right->AsObject()->TryGetStringField(TEXT("id"), RightId);
				}
				return LeftId < RightId;
			});
	};

	if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		TSharedPtr<FJsonObject> BlueprintJson = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Variables;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			TSharedPtr<FJsonObject> VariableJson = MakeShared<FJsonObject>();
			VariableJson->SetStringField(
				TEXT("id"),
				Variable.VarName.ToString());
			VariableJson->SetStringField(
				TEXT("name"),
				Variable.VarName.ToString());
			VariableJson->SetStringField(
				TEXT("type"),
				UEdGraphSchema_K2::TypeToText(Variable.VarType).ToString());
			VariableJson->SetStringField(
				TEXT("category"),
				Variable.Category.ToString());
			VariableJson->SetStringField(
				TEXT("default"),
				Variable.DefaultValue);
			VariableJson->SetStringField(
				TEXT("propertyFlags"),
				FString::Printf(
					TEXT("0x%016llx"),
					static_cast<unsigned long long>(
						Variable.PropertyFlags)));
			Variables.Add(MakeShared<FJsonValueObject>(VariableJson));
		}
		SortById(Variables);
		BlueprintJson->SetArrayField(TEXT("variables"), Variables);

		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		TArray<TSharedPtr<FJsonValue>> GraphValues;
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph)
			{
				GraphValues.Add(MakeShared<FJsonValueObject>(
					CaptureGraphStructure(Graph)));
			}
		}
		SortById(GraphValues);
		BlueprintJson->SetArrayField(TEXT("graphs"), GraphValues);

		TArray<TSharedPtr<FJsonValue>> Components;
		if (Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node :
				Blueprint->SimpleConstructionScript->GetAllNodes())
			{
				if (!Node)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Component = MakeShared<FJsonObject>();
				Component->SetStringField(
					TEXT("id"),
					Node->GetVariableName().ToString());
				Component->SetStringField(
					TEXT("name"),
					Node->GetVariableName().ToString());
				Component->SetStringField(
					TEXT("class"),
					Node->ComponentTemplate
						? Node->ComponentTemplate->GetClass()->GetName()
						: FString());
				Component->SetStringField(
					TEXT("parent"),
					Node->ParentComponentOrVariableName.ToString());
				Components.Add(MakeShared<FJsonValueObject>(Component));
			}
		}
		SortById(Components);
		BlueprintJson->SetArrayField(TEXT("components"), Components);
		Snapshot->SetObjectField(TEXT("blueprint"), BlueprintJson);
	}

	if (UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Asset))
	{
		TSharedPtr<FJsonObject> WidgetJson = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Hierarchy;
		if (WidgetBlueprint->WidgetTree)
		{
			TArray<UWidget*> Widgets;
			WidgetBlueprint->WidgetTree->GetAllWidgets(Widgets);
			if (WidgetBlueprint->WidgetTree->RootWidget)
			{
				Widgets.AddUnique(WidgetBlueprint->WidgetTree->RootWidget);
			}
			for (UWidget* Widget : Widgets)
			{
				if (!Widget)
				{
					continue;
				}
				TSharedPtr<FJsonObject> WidgetNode = MakeShared<FJsonObject>();
				WidgetNode->SetStringField(TEXT("id"), Widget->GetName());
				WidgetNode->SetStringField(TEXT("name"), Widget->GetName());
				WidgetNode->SetStringField(
					TEXT("class"),
					Widget->GetClass()->GetName());
				UPanelWidget* Parent = Widget->GetParent();
				WidgetNode->SetStringField(
					TEXT("parent"),
					Parent ? Parent->GetName() : FString());
				WidgetNode->SetNumberField(
					TEXT("index"),
					Parent ? Parent->GetChildIndex(Widget) : INDEX_NONE);

				if (UCanvasPanelSlot* CanvasSlot =
					Cast<UCanvasPanelSlot>(Widget->Slot))
				{
					TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
					const FAnchors Anchors = CanvasSlot->GetAnchors();
					Layout->SetArrayField(
						TEXT("anchors"),
						{
							MakeShared<FJsonValueNumber>(Anchors.Minimum.X),
							MakeShared<FJsonValueNumber>(Anchors.Minimum.Y),
							MakeShared<FJsonValueNumber>(Anchors.Maximum.X),
							MakeShared<FJsonValueNumber>(Anchors.Maximum.Y),
						});
					const FVector2D Alignment = CanvasSlot->GetAlignment();
					Layout->SetArrayField(
						TEXT("alignment"),
						{
							MakeShared<FJsonValueNumber>(Alignment.X),
							MakeShared<FJsonValueNumber>(Alignment.Y),
						});
					const FMargin Offsets = CanvasSlot->GetOffsets();
					Layout->SetArrayField(
						TEXT("offsets"),
						{
							MakeShared<FJsonValueNumber>(Offsets.Left),
							MakeShared<FJsonValueNumber>(Offsets.Top),
							MakeShared<FJsonValueNumber>(Offsets.Right),
							MakeShared<FJsonValueNumber>(Offsets.Bottom),
						});
					Layout->SetBoolField(
						TEXT("autoSize"),
						CanvasSlot->GetAutoSize());
					Layout->SetNumberField(
						TEXT("zOrder"),
						CanvasSlot->GetZOrder());
					WidgetNode->SetObjectField(TEXT("layout"), Layout);
				}
				Hierarchy.Add(MakeShared<FJsonValueObject>(WidgetNode));
			}
		}
		SortById(Hierarchy);
		WidgetJson->SetArrayField(TEXT("hierarchy"), Hierarchy);

		TArray<TSharedPtr<FJsonValue>> Bindings;
		for (const FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
		{
			TSharedPtr<FJsonObject> BindingJson = MakeShared<FJsonObject>();
			const FString BindingId = FString::Printf(
				TEXT("%s/%s"),
				*Binding.ObjectName,
				*Binding.PropertyName.ToString());
			BindingJson->SetStringField(TEXT("id"), BindingId);
			BindingJson->SetStringField(
				TEXT("object"),
				Binding.ObjectName);
			BindingJson->SetStringField(
				TEXT("property"),
				Binding.PropertyName.ToString());
			BindingJson->SetStringField(
				TEXT("function"),
				Binding.FunctionName.ToString());
			BindingJson->SetNumberField(
				TEXT("kind"),
				static_cast<uint8>(Binding.Kind));
			Bindings.Add(MakeShared<FJsonValueObject>(BindingJson));
		}
		SortById(Bindings);
		WidgetJson->SetArrayField(TEXT("bindings"), Bindings);
		Snapshot->SetObjectField(TEXT("widget"), WidgetJson);
	}

	if (UMaterial* Material = Cast<UMaterial>(Asset))
	{
		TSharedPtr<FJsonObject> MaterialJson = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetNumberField(
			TEXT("domain"),
			static_cast<uint8>(Material->MaterialDomain));
		Properties->SetNumberField(
			TEXT("blendMode"),
			static_cast<uint8>(Material->BlendMode));
		Properties->SetBoolField(TEXT("twoSided"), Material->TwoSided != 0);
		Properties->SetNumberField(
			TEXT("opacityMaskClipValue"),
			Material->OpacityMaskClipValue);
		MaterialJson->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Expressions;
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			if (!Expression)
			{
				continue;
			}
			TSharedPtr<FJsonObject> ExpressionJson = MakeShared<FJsonObject>();
			const FString ExpressionId =
				Expression->MaterialExpressionGuid.IsValid()
					? Expression->MaterialExpressionGuid.ToString(
						EGuidFormats::DigitsWithHyphensLower)
					: Expression->GetName();
			ExpressionJson->SetStringField(TEXT("id"), ExpressionId);
			ExpressionJson->SetStringField(
				TEXT("name"),
				Expression->GetName());
			ExpressionJson->SetStringField(
				TEXT("class"),
				Expression->GetClass()->GetName());
			ExpressionJson->SetNumberField(
				TEXT("x"),
				Expression->MaterialExpressionEditorX);
			ExpressionJson->SetNumberField(
				TEXT("y"),
				Expression->MaterialExpressionEditorY);
			if (Expression->HasAParameterName())
			{
				ExpressionJson->SetStringField(
					TEXT("parameterName"),
					Expression->GetParameterName().ToString());
			}
			TArray<TSharedPtr<FJsonValue>> Connections;
			const TArrayView<FExpressionInput*> Inputs =
				Expression->GetInputsView();
			for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
			{
				const FExpressionInput* Input = Inputs[InputIndex];
				if (!Input || !Input->Expression)
				{
					continue;
				}
				const FString InputName =
					!Input->InputName.IsNone()
						? Input->InputName.ToString()
						: Expression->GetInputName(InputIndex).ToString();
				const FString SourceId =
					Input->Expression->MaterialExpressionGuid.IsValid()
						? Input->Expression->MaterialExpressionGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower)
						: Input->Expression->GetName();
				TSharedPtr<FJsonObject> Connection =
					MakeShared<FJsonObject>();
				Connection->SetStringField(
					TEXT("id"),
					FString::Printf(
						TEXT("%04d:%s"),
						InputIndex,
						*InputName));
				Connection->SetStringField(TEXT("input"), InputName);
				Connection->SetStringField(
					TEXT("sourceExpression"),
					SourceId);
				Connection->SetNumberField(
					TEXT("outputIndex"),
					Input->OutputIndex);
				Connections.Add(
					MakeShared<FJsonValueObject>(Connection));
			}
			SortById(Connections);
			ExpressionJson->SetArrayField(
				TEXT("connections"),
				Connections);
			Expressions.Add(MakeShared<FJsonValueObject>(ExpressionJson));
		}
		SortById(Expressions);
		MaterialJson->SetArrayField(TEXT("expressions"), Expressions);
		// MaterialGraph is a transient editor projection and may be created
		// lazily by an edit or by opening the material editor. Durable expression
		// connections are captured above; hashing transient graph nodes would
		// still cause false mismatches after an otherwise exact rollback.
		Snapshot->SetObjectField(TEXT("material"), MaterialJson);
	}

	return Snapshot;
}

TSharedPtr<FJsonObject> FWorkflowRuntime::MakeStructuralDiff(
	const TSharedPtr<FJsonObject>& Before,
	const TSharedPtr<FJsonObject>& After)
{
	TMap<FString, FString> BeforeValues;
	TMap<FString, FString> AfterValues;
	FlattenStructureValue(
		MakeShared<FJsonValueObject>(
			Before.IsValid() ? Before : MakeShared<FJsonObject>()),
		FString(),
		BeforeValues);
	FlattenStructureValue(
		MakeShared<FJsonValueObject>(
			After.IsValid() ? After : MakeShared<FJsonObject>()),
		FString(),
		AfterValues);

	TSet<FString> PathSet;
	for (const TPair<FString, FString>& Pair : BeforeValues)
	{
		PathSet.Add(Pair.Key);
	}
	for (const TPair<FString, FString>& Pair : AfterValues)
	{
		PathSet.Add(Pair.Key);
	}
	TArray<FString> Paths = PathSet.Array();
	Paths.Sort();

	TArray<TSharedPtr<FJsonValue>> Added;
	TArray<TSharedPtr<FJsonValue>> Removed;
	TArray<TSharedPtr<FJsonValue>> Changed;
	for (const FString& Path : Paths)
	{
		const FString* BeforeValue = BeforeValues.Find(Path);
		const FString* AfterValue = AfterValues.Find(Path);
		if (!BeforeValue && AfterValue)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), Path);
			Entry->SetStringField(TEXT("value"), *AfterValue);
			Added.Add(MakeShared<FJsonValueObject>(Entry));
		}
		else if (BeforeValue && !AfterValue)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), Path);
			Entry->SetStringField(TEXT("value"), *BeforeValue);
			Removed.Add(MakeShared<FJsonValueObject>(Entry));
		}
		else if (BeforeValue && AfterValue && *BeforeValue != *AfterValue)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("path"), Path);
			Entry->SetStringField(TEXT("before"), *BeforeValue);
			Entry->SetStringField(TEXT("after"), *AfterValue);
			Changed.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
	Diff->SetStringField(
		TEXT("snapshotKind"),
		TEXT("structuralVerification"));
	Diff->SetBoolField(TEXT("restoreAvailable"), false);
	Diff->SetBoolField(
		TEXT("changed"),
		!Added.IsEmpty() || !Removed.IsEmpty() || !Changed.IsEmpty());
	Diff->SetArrayField(TEXT("added"), Added);
	Diff->SetArrayField(TEXT("removed"), Removed);
	Diff->SetArrayField(TEXT("changedFields"), Changed);
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("added"), Added.Num());
	Summary->SetNumberField(TEXT("removed"), Removed.Num());
	Summary->SetNumberField(TEXT("changed"), Changed.Num());
	Diff->SetObjectField(TEXT("summary"), Summary);
	return Diff;
}

FString FWorkflowRuntime::ComputeAssetStructureHash(UObject* Asset)
{
	if (!Asset)
	{
		return TEXT("sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	}

	const FString Structure =
		JsonStringify(CaptureAssetStructure(Asset));
	const FTCHARToUTF8 Utf8(*Structure);
	FString Hash;
	if (!UEAIIntegration::Infrastructure::TrySha256Hex(
			Utf8.Get(),
			static_cast<uint64>(Utf8.Length()),
			Hash))
	{
		return FString();
	}
	return FString(TEXT("sha256:")) + Hash;
}

bool FWorkflowRuntime::VerifyOrCleanRollback(FRunRecord& Record)
{
	if (Record.ScopeAsset.IsEmpty())
	{
		return false;
	}

	UObject* CurrentAsset = LoadAssetWithoutLogging(Record.ScopeAsset);
	if (!Record.bScopeExistedBefore)
	{
		if (!CurrentAsset)
		{
			Record.StructureHashAfterRollback =
				ComputeAssetStructureHash(nullptr);
			Record.StructureAfterRollback =
				CaptureAssetStructure(nullptr);
			return !AssetExistsWithoutLogging(Record.ScopeAsset);
		}

		const FString PackageName =
			FPackageName::ObjectPathToPackageName(Record.ScopeAsset);
		if (FPackageName::DoesPackageExist(PackageName))
		{
			Record.StructureHashAfterRollback =
				ComputeAssetStructureHash(CurrentAsset);
			Record.StructureAfterRollback =
				CaptureAssetStructure(CurrentAsset);
			return false;
		}

		UPackage* NewPackage = CurrentAsset->GetOutermost();
		const TArray<UObject*> ObjectsToDelete = {CurrentAsset};
		ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
		if (NewPackage)
		{
			// The scope did not exist on disk before this run. Clear any
			// in-memory Asset Registry package data left behind by deleting the
			// newly-created object so absence is immediately observable.
			IAssetRegistry::GetChecked().PackageDeleted(NewPackage);
		}
		CurrentAsset = LoadAssetWithoutLogging(Record.ScopeAsset);
		Record.StructureHashAfterRollback =
			ComputeAssetStructureHash(CurrentAsset);
		Record.StructureAfterRollback =
			CaptureAssetStructure(CurrentAsset);
		return !CurrentAsset
			&& !AssetExistsWithoutLogging(Record.ScopeAsset);
	}

	Record.StructureHashAfterRollback =
		ComputeAssetStructureHash(CurrentAsset);
	Record.StructureAfterRollback =
		CaptureAssetStructure(CurrentAsset);
	const bool bMatches =
		!Record.StructureHashBefore.IsEmpty()
		&& Record.StructureHashBefore == Record.StructureHashAfterRollback;
	if (bMatches && CurrentAsset)
	{
		CurrentAsset->GetOutermost()->SetDirtyFlag(
			Record.bPackageDirtyBefore);
	}
	return bMatches;
}

FString FWorkflowRuntime::GetStringField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}
}
