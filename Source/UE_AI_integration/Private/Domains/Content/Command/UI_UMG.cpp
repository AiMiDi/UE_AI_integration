// UMG Widget Blueprint tools for UE_AI_integration
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Domains/Content/Command/WidgetEventBindingSupport.h"
#include "Workflow/UEWorkflowRuntime.h"

#include "Dom/JsonValue.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/VerticalBox.h"
#include "Components/HorizontalBox.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/Slider.h"
#include "Components/ProgressBar.h"
#include "Components/EditableTextBox.h"
#include "Components/ScrollBox.h"
#include "Components/Overlay.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/GridPanel.h"
#include "Components/UniformGridPanel.h"
#include "Components/WidgetSwitcher.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"

// ─────────────────────────────────────────────────────────────
// Helper: Resolve a widget class name to UClass
// ─────────────────────────────────────────────────────────────
static UClass* ResolveWidgetClass(const FString& ClassName)
{
	// Common short names mapping
	static TMap<FString, FString> ShortNames = {
		{TEXT("CanvasPanel"),		TEXT("UCanvasPanel")},
		{TEXT("Button"),			TEXT("UButton")},
		{TEXT("TextBlock"),			TEXT("UTextBlock")},
		{TEXT("Image"),				TEXT("UImage")},
		{TEXT("VerticalBox"),		TEXT("UVerticalBox")},
		{TEXT("HorizontalBox"),		TEXT("UHorizontalBox")},
		{TEXT("Border"),			TEXT("UBorder")},
		{TEXT("CheckBox"),			TEXT("UCheckBox")},
		{TEXT("Slider"),			TEXT("USlider")},
		{TEXT("ProgressBar"),		TEXT("UProgressBar")},
		{TEXT("EditableTextBox"),	TEXT("UEditableTextBox")},
		{TEXT("ScrollBox"),			TEXT("UScrollBox")},
		{TEXT("Overlay"),			TEXT("UOverlay")},
		{TEXT("SizeBox"),			TEXT("USizeBox")},
		{TEXT("Spacer"),			TEXT("USpacer")},
		{TEXT("GridPanel"),			TEXT("UGridPanel")},
		{TEXT("UniformGridPanel"),	TEXT("UUniformGridPanel")},
		{TEXT("WidgetSwitcher"),	TEXT("UWidgetSwitcher")},
	};

	FString Lookup = ClassName;
	if (ShortNames.Contains(ClassName))
	{
		Lookup = ShortNames[ClassName];
	}

	if (!Lookup.StartsWith(TEXT("U")))
	{
		Lookup = TEXT("U") + Lookup;
	}

	UClass* Found = FindFirstObject<UClass>(*Lookup, EFindFirstObjectOptions::ExactClass);
	if (!Found)
	{
		Found = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::ExactClass);
	}
	return Found;
}

static UWidget* FindWidgetByName(UWidgetTree* WidgetTree, const FString& WidgetName)
{
	if (!WidgetTree || WidgetName.IsEmpty())
	{
		return nullptr;
	}

	UWidget* FoundWidget = nullptr;
	WidgetTree->ForEachWidget([&](UWidget* Widget)
	{
		if (!FoundWidget && Widget && Widget->GetName() == WidgetName)
		{
			FoundWidget = Widget;
		}
	});
	return FoundWidget;
}

static UWidgetBlueprint* LoadWidgetBlueprint(const FString& AssetPath)
{
	if (UWidgetBlueprint* Direct =
		LoadObject<UWidgetBlueprint>(nullptr, *AssetPath))
	{
		return Direct;
	}

	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	if (PackageName.IsEmpty() || AssetName.IsEmpty())
	{
		return nullptr;
	}
	return LoadObject<UWidgetBlueprint>(
		nullptr,
		*(PackageName + TEXT(".") + AssetName));
}

static TSharedPtr<FJsonObject> MakeWidgetRef(
	const FString& WidgetBlueprintPath,
	const UWidget* Widget)
{
	TSharedPtr<FJsonObject> WidgetRef = MakeShared<FJsonObject>();
	WidgetRef->SetStringField(TEXT("kind"), TEXT("widget"));
	WidgetRef->SetStringField(TEXT("widgetBlueprint"), WidgetBlueprintPath);
	WidgetRef->SetStringField(
		TEXT("name"),
		Widget ? Widget->GetName() : FString());
	return WidgetRef;
}

static TArray<TSharedPtr<FJsonValue>> Vector2ToJson(const FVector2D& Value)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(Value.X));
	Values.Add(MakeShared<FJsonValueNumber>(Value.Y));
	return Values;
}

static TArray<TSharedPtr<FJsonValue>> AnchorsToJson(const FAnchors& Anchors)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(Anchors.Minimum.X));
	Values.Add(MakeShared<FJsonValueNumber>(Anchors.Minimum.Y));
	Values.Add(MakeShared<FJsonValueNumber>(Anchors.Maximum.X));
	Values.Add(MakeShared<FJsonValueNumber>(Anchors.Maximum.Y));
	return Values;
}

static TArray<TSharedPtr<FJsonValue>> MarginToJson(const FMargin& Margin)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Add(MakeShared<FJsonValueNumber>(Margin.Left));
	Values.Add(MakeShared<FJsonValueNumber>(Margin.Top));
	Values.Add(MakeShared<FJsonValueNumber>(Margin.Right));
	Values.Add(MakeShared<FJsonValueNumber>(Margin.Bottom));
	return Values;
}

static bool TryReadNumberArray(
	const TSharedPtr<FJsonObject>& Params,
	const FString& Field,
	int32 ExpectedCount,
	TArray<double>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(Field, Values) || !Values
		|| Values->Num() != ExpectedCount)
	{
		return false;
	}

	OutValues.Reset(ExpectedCount);
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			OutValues.Reset();
			return false;
		}
		OutValues.Add(Value->AsNumber());
	}
	return true;
}

static bool IsDeferredWorkflowExecution(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid()
		|| !Params->HasTypedField<EJson::Object>(TEXT("__ueWorkflow")))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> WorkflowContext =
		Params->GetObjectField(TEXT("__ueWorkflow"));
	bool bDeferCompile = false;
	return WorkflowContext.IsValid()
		&& WorkflowContext->TryGetBoolField(
			TEXT("deferCompile"),
			bDeferCompile)
		&& bDeferCompile;
}

static void MarkWidgetBlueprintEdited(
	UWidgetBlueprint* WidgetBlueprint,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!WidgetBlueprint)
	{
		return;
	}

	// MarkBlueprintAsStructurallyModified regenerates the skeleton immediately
	// in UE 5.3. Workflow execution defers that work to the single compile
	// finalizer; legacy one-shot operations retain their historical behavior.
	if (IsDeferredWorkflowExecution(Params))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
			WidgetBlueprint);
	}
}

static void AddExecutionMetadata(
	const TSharedPtr<FJsonObject>& Result,
	const TSharedPtr<FJsonObject>& Params,
	bool bSaved = false)
{
	if (!Result.IsValid())
	{
		return;
	}
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(
		TEXT("deferredCompile"),
		IsDeferredWorkflowExecution(Params));
}

static TSharedPtr<FJsonObject> SerializeWidgetTreeNode(
	const FString& WidgetBlueprintPath,
	UWidget* Widget)
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	if (!Widget)
	{
		return Node;
	}

	Node->SetStringField(TEXT("name"), Widget->GetName());
	Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	Node->SetObjectField(
		TEXT("widgetRef"),
		MakeWidgetRef(WidgetBlueprintPath, Widget));

	if (Widget->Slot)
	{
		Node->SetStringField(TEXT("slotClass"), Widget->Slot->GetClass()->GetName());
		if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
			Layout->SetArrayField(
				TEXT("anchors"),
				AnchorsToJson(CanvasSlot->GetAnchors()));
			Layout->SetArrayField(
				TEXT("offsets"),
				MarginToJson(CanvasSlot->GetOffsets()));
			Layout->SetArrayField(
				TEXT("alignment"),
				Vector2ToJson(CanvasSlot->GetAlignment()));
			Layout->SetBoolField(TEXT("autoSize"), CanvasSlot->GetAutoSize());
			Layout->SetNumberField(TEXT("zOrder"), CanvasSlot->GetZOrder());
			Node->SetObjectField(TEXT("layout"), Layout);
		}
	}

	if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
	{
		TArray<TSharedPtr<FJsonValue>> Children;
		for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
		{
			Children.Add(MakeShared<FJsonValueObject>(
				SerializeWidgetTreeNode(
					WidgetBlueprintPath,
					Panel->GetChildAt(Index))));
		}
		Node->SetArrayField(TEXT("children"), Children);
	}

	return Node;
}

// ─────────────────────────────────────────────────────────────
// create_widget_blueprint
// ─────────────────────────────────────────────────────────────
class FTool_CreateWidgetBlueprint : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.blueprint.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Parameter 'name' is required."));
		}

		UClass* ParentClass = UUserWidget::StaticClass();
		if (Params->HasField(TEXT("parent_class")))
		{
			FString ParentClassName = Params->GetStringField(TEXT("parent_class"));
			UClass* CustomParent = ResolveWidgetClass(ParentClassName);
			if (CustomParent && CustomParent->IsChildOf(UUserWidget::StaticClass()))
			{
				ParentClass = CustomParent;
			}
		}

		FString PackageDirectory = TEXT("/Game/UI");
		Params->TryGetStringField(TEXT("package_path"), PackageDirectory);
		PackageDirectory.RemoveFromEnd(TEXT("/"));
		if (!PackageDirectory.StartsWith(TEXT("/Game")))
		{
			return FMCPToolResult::Error(
				TEXT("Parameter 'package_path' must start with '/Game'."),
				TEXT("invalid_params"),
				422);
		}
		FString PackagePath =
			FString::Printf(TEXT("%s/%s"), *PackageDirectory, *Name);
		FString PackageName = FPackageName::ObjectPathToPackageName(PackagePath);

		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *PackagePath));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package at '%s'."), *PackageName));
		}

		UWidgetBlueprint* WidgetBP = CastChecked<UWidgetBlueprint>(
			FKismetEditorUtilities::CreateBlueprint(
				ParentClass,
				Package,
				FName(*Name),
				BPTYPE_Normal,
				UWidgetBlueprint::StaticClass(),
				UWidgetBlueprintGeneratedClass::StaticClass()
			)
		);

		if (!WidgetBP)
		{
			return FMCPToolResult::Error(TEXT("Failed to create Widget Blueprint."));
		}

		// Ensure a CanvasPanel root exists
		if (WidgetBP->WidgetTree)
		{
			UCanvasPanel* RootCanvas = WidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
			WidgetBP->WidgetTree->RootWidget = RootCanvas;
		}

		MarkWidgetBlueprintEdited(WidgetBP, Params);

		FAssetRegistryModule::AssetCreated(WidgetBP);
		WidgetBP->MarkPackageDirty();

		bool bSaved = false;
		if (!IsDeferredWorkflowExecution(Params))
		{
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			const FString FilePath = FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetAssetPackageExtension());
			bSaved =
				UPackage::SavePackage(Package, WidgetBP, *FilePath, SaveArgs);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), PackagePath);
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
		AddExecutionMetadata(Result, Params, bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_widget_child
// ─────────────────────────────────────────────────────────────
class FTool_AddWidgetChild : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.child.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString WidgetBPPath = Params->GetStringField(TEXT("widget_bp"));
		FString ChildClassName;
		if (!Params->TryGetStringField(TEXT("child_class"), ChildClassName))
		{
			Params->TryGetStringField(TEXT("class"), ChildClassName);
		}
		FString ChildName;
		if (!Params->TryGetStringField(TEXT("child_name"), ChildName))
		{
			Params->TryGetStringField(TEXT("name"), ChildName);
		}
		FString ParentName;
		if (!Params->TryGetStringField(TEXT("parent_name"), ParentName))
		{
			Params->TryGetStringField(TEXT("parent"), ParentName);
		}
		if (ChildClassName.IsEmpty() || ChildName.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Parameters 'child_class'/'class' and 'child_name'/'name' are required."),
				TEXT("invalid_params"),
				422);
		}

		UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Widget Blueprint not found at '%s'."), *WidgetBPPath));
		}

		UClass* ChildClass = ResolveWidgetClass(ChildClassName);
		if (!ChildClass || !ChildClass->IsChildOf(UWidget::StaticClass()))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Widget class '%s' not found."), *ChildClassName));
		}

		UWidgetTree* WidgetTree = WidgetBP->WidgetTree;
		if (!WidgetTree)
		{
			return FMCPToolResult::Error(TEXT("Widget Blueprint has no WidgetTree."));
		}
		if (FindWidgetByName(WidgetTree, ChildName))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget '%s' already exists in the Widget Blueprint."),
					*ChildName),
				TEXT("duplicate_widget_name"),
				409);
		}

		// Find parent
		UPanelWidget* ParentWidget = nullptr;
		if (ParentName.IsEmpty())
		{
			ParentWidget = Cast<UPanelWidget>(WidgetTree->RootWidget);
		}
		else
		{
			WidgetTree->ForEachWidget([&](UWidget* Widget)
			{
				if (Widget->GetName() == ParentName && !ParentWidget)
				{
					ParentWidget = Cast<UPanelWidget>(Widget);
				}
			});
		}

		if (!ParentWidget)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Parent widget '%s' not found or is not a panel."), *ParentName));
		}
		if (!ParentWidget->CanAddMoreChildren())
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Parent widget '%s' cannot accept another child."),
					*ParentWidget->GetName()),
				TEXT("parent_capacity_exceeded"),
				409);
		}

		WidgetBP->Modify();
		WidgetTree->Modify();
		ParentWidget->Modify();

		// Create child widget
		UWidget* NewChild = WidgetTree->ConstructWidget<UWidget>(ChildClass, FName(*ChildName));
		if (!NewChild)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to construct widget of class '%s'."), *ChildClassName));
		}
		NewChild->SetFlags(RF_Transactional);
		NewChild->Modify();

		UPanelSlot* Slot = ParentWidget->AddChild(NewChild);
		if (!Slot)
		{
			NewChild->MarkAsGarbage();
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Parent widget '%s' rejected child '%s'."),
					*ParentWidget->GetName(),
					*ChildName),
				TEXT("child_attach_failed"),
				409);
		}
		Slot->SetFlags(RF_Transactional);
		Slot->Modify();

		MarkWidgetBlueprintEdited(WidgetBP, Params);
		WidgetBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_bp"), WidgetBPPath);
		Result->SetStringField(TEXT("child_name"), ChildName);
		Result->SetStringField(TEXT("child_class"), ChildClassName);
		Result->SetStringField(TEXT("parent"), ParentWidget->GetName());
		Result->SetBoolField(TEXT("has_slot"), true);
		Result->SetObjectField(
			TEXT("widgetRef"),
			MakeWidgetRef(WidgetBPPath, NewChild));
		AddExecutionMetadata(Result, Params);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// set_widget_property
// ─────────────────────────────────────────────────────────────
class FTool_SetWidgetProperty : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.property.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString WidgetBPPath = Params->GetStringField(TEXT("widget_bp"));
		FString WidgetName = Params->GetStringField(TEXT("widget_name"));
		FString Property = Params->GetStringField(TEXT("property"));
		FString Value = Params->GetStringField(TEXT("value"));

		UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Widget Blueprint not found at '%s'."), *WidgetBPPath));
		}

		// Find the widget by name
		UWidget* TargetWidget = nullptr;
		WidgetBP->WidgetTree->ForEachWidget([&](UWidget* Widget)
		{
			if (Widget->GetName() == WidgetName && !TargetWidget)
			{
				TargetWidget = Widget;
			}
		});

		if (!TargetWidget)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Widget '%s' not found in blueprint."), *WidgetName));
		}

		WidgetBP->Modify();
		TargetWidget->Modify();

		// Handle common properties with dedicated setters
		if (Property == TEXT("Text") || Property == TEXT("text"))
		{
			if (UTextBlock* TextBlock = Cast<UTextBlock>(TargetWidget))
			{
				TextBlock->SetText(FText::FromString(Value));

				MarkWidgetBlueprintEdited(WidgetBP, Params);
				WidgetBP->MarkPackageDirty();

				TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("widget"), WidgetName);
				Result->SetStringField(TEXT("property"), Property);
				Result->SetStringField(TEXT("value"), Value);
				AddExecutionMetadata(Result, Params);
				return FMCPToolResult::Ok(Result);
			}
		}

		if (Property == TEXT("Visibility") || Property == TEXT("visibility"))
		{
			ESlateVisibility Vis = ESlateVisibility::Visible;
			FString LowerVal = Value.ToLower();
			if (LowerVal == TEXT("collapsed")) Vis = ESlateVisibility::Collapsed;
			else if (LowerVal == TEXT("hidden")) Vis = ESlateVisibility::Hidden;
			else if (LowerVal == TEXT("hitTestInvisible") || LowerVal == TEXT("hittestinvisible")) Vis = ESlateVisibility::HitTestInvisible;
			else if (LowerVal == TEXT("selfHitTestInvisible") || LowerVal == TEXT("selfhittestinvisible")) Vis = ESlateVisibility::SelfHitTestInvisible;

			TargetWidget->SetVisibility(Vis);

			MarkWidgetBlueprintEdited(WidgetBP, Params);
			WidgetBP->MarkPackageDirty();

			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("widget"), WidgetName);
			Result->SetStringField(TEXT("property"), Property);
			Result->SetStringField(TEXT("value"), Value);
			AddExecutionMetadata(Result, Params);
			return FMCPToolResult::Ok(Result);
		}

		// Generic property setter via reflection
		FProperty* Prop = TargetWidget->GetClass()->FindPropertyByName(FName(*Property));
		if (!Prop)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Property '%s' not found on widget class '%s'."), *Property, *TargetWidget->GetClass()->GetName()));
		}

		void* PropAddr = Prop->ContainerPtrToValuePtr<void>(TargetWidget);
		if (!Prop->ImportText_Direct(*Value, PropAddr, TargetWidget, PPF_None))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to set property '%s' to value '%s'."), *Property, *Value));
		}

		MarkWidgetBlueprintEdited(WidgetBP, Params);
		WidgetBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget"), WidgetName);
		Result->SetStringField(TEXT("property"), Property);
		Result->SetStringField(TEXT("value"), Value);
		AddExecutionMetadata(Result, Params);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// set_widget_slot_layout
// ─────────────────────────────────────────────────────────────
class FTool_SetWidgetSlotLayout : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.slot.layout.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBPPath = Params->GetStringField(TEXT("widget_bp"));
		FString WidgetName;
		Params->TryGetStringField(TEXT("widget_name"), WidgetName);
		if (WidgetName.IsEmpty()
			&& Params->HasTypedField<EJson::Object>(TEXT("target")))
		{
			const TSharedPtr<FJsonObject> Target =
				Params->GetObjectField(TEXT("target"));
			FString TargetKind;
			FString TargetBlueprint;
			if (!Target.IsValid()
				|| !Target->TryGetStringField(TEXT("kind"), TargetKind)
				|| TargetKind != TEXT("widget")
				|| !Target->TryGetStringField(
					TEXT("widgetBlueprint"),
					TargetBlueprint)
				|| TargetBlueprint != WidgetBPPath
				|| !Target->TryGetStringField(TEXT("name"), WidgetName))
			{
				return FMCPToolResult::Error(
					TEXT("Parameter 'target' must be a widget reference for the scoped Widget Blueprint."),
					TEXT("invalid_reference"),
					422);
			}
		}
		if (WidgetName.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Parameter 'widget_name' or typed 'target' is required."),
				TEXT("invalid_params"),
				422);
		}

		UWidgetBlueprint* WidgetBP =
			LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget Blueprint not found at '%s'."),
					*WidgetBPPath),
				TEXT("target_not_found"),
				404);
		}

		UWidget* TargetWidget =
			FindWidgetByName(WidgetBP->WidgetTree, WidgetName);
		if (!TargetWidget)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget '%s' not found."),
					*WidgetName),
				TEXT("target_not_found"),
				404);
		}

		UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(TargetWidget->Slot);
		if (!CanvasSlot)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget '%s' is not in a CanvasPanel slot."),
					*WidgetName),
				TEXT("unsupported_slot_type"),
				422);
		}

		WidgetBP->Modify();
		TargetWidget->Modify();
		CanvasSlot->Modify();

		if (Params->HasField(TEXT("anchors")))
		{
			TArray<double> Anchors;
			if (!TryReadNumberArray(Params, TEXT("anchors"), 4, Anchors))
			{
				return FMCPToolResult::Error(
					TEXT("Parameter 'anchors' must contain four numbers."),
					TEXT("invalid_params"),
					422);
			}
			CanvasSlot->SetAnchors(
				FAnchors(
					Anchors[0],
					Anchors[1],
					Anchors[2],
					Anchors[3]));
		}

		if (Params->HasField(TEXT("offsets")))
		{
			TArray<double> Offsets;
			if (!TryReadNumberArray(Params, TEXT("offsets"), 4, Offsets))
			{
				return FMCPToolResult::Error(
					TEXT("Parameter 'offsets' must contain four numbers."),
					TEXT("invalid_params"),
					422);
			}
			CanvasSlot->SetOffsets(
				FMargin(
					Offsets[0],
					Offsets[1],
					Offsets[2],
					Offsets[3]));
		}

		if (Params->HasField(TEXT("alignment")))
		{
			TArray<double> Alignment;
			if (!TryReadNumberArray(Params, TEXT("alignment"), 2, Alignment))
			{
				return FMCPToolResult::Error(
					TEXT("Parameter 'alignment' must contain two numbers."),
					TEXT("invalid_params"),
					422);
			}
			CanvasSlot->SetAlignment(
				FVector2D(Alignment[0], Alignment[1]));
		}

		bool bAutoSize = false;
		if (Params->TryGetBoolField(TEXT("auto_size"), bAutoSize))
		{
			CanvasSlot->SetAutoSize(bAutoSize);
		}

		double ZOrder = 0.0;
		if (Params->TryGetNumberField(TEXT("z_order"), ZOrder))
		{
			CanvasSlot->SetZOrder(static_cast<int32>(ZOrder));
		}

		FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);
		WidgetBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
		Layout->SetArrayField(
			TEXT("anchors"),
			AnchorsToJson(CanvasSlot->GetAnchors()));
		Layout->SetArrayField(
			TEXT("offsets"),
			MarginToJson(CanvasSlot->GetOffsets()));
		Layout->SetArrayField(
			TEXT("alignment"),
			Vector2ToJson(CanvasSlot->GetAlignment()));
		Layout->SetBoolField(TEXT("autoSize"), CanvasSlot->GetAutoSize());
		Layout->SetNumberField(TEXT("zOrder"), CanvasSlot->GetZOrder());

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(
			TEXT("widgetRef"),
			MakeWidgetRef(WidgetBPPath, TargetWidget));
		Result->SetObjectField(TEXT("layout"), Layout);
		AddExecutionMetadata(Result, Params);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// remove_widget_child
// ─────────────────────────────────────────────────────────────
class FTool_RemoveWidgetChild : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.child.remove");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBPPath = Params->GetStringField(TEXT("widget_bp"));
		const FString ChildName = Params->GetStringField(TEXT("child_name"));

		UWidgetBlueprint* WidgetBP =
			LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget Blueprint not found at '%s'."),
					*WidgetBPPath),
				TEXT("target_not_found"),
				404);
		}

		UWidget* Child =
			FindWidgetByName(WidgetBP->WidgetTree, ChildName);
		if (!Child)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Widget '%s' not found."), *ChildName),
				TEXT("target_not_found"),
				404);
		}
		if (WidgetBP->WidgetTree->RootWidget == Child)
		{
			return FMCPToolResult::Error(
				TEXT("The root widget cannot be removed."),
				TEXT("invalid_target"),
				422);
		}

		int32 ChildIndex = INDEX_NONE;
		UPanelWidget* Parent =
			UWidgetTree::FindWidgetParent(Child, ChildIndex);
		const FString ParentName = Parent ? Parent->GetName() : FString();

		WidgetBP->Modify();
		WidgetBP->WidgetTree->Modify();
		Child->Modify();
		if (Parent)
		{
			Parent->Modify();
		}

		if (!WidgetBP->WidgetTree->RemoveWidget(Child))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Failed to remove widget '%s'."),
					*ChildName));
		}

		MarkWidgetBlueprintEdited(WidgetBP, Params);
		WidgetBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("child_name"), ChildName);
		Result->SetStringField(TEXT("parent"), ParentName);
		Result->SetNumberField(TEXT("former_index"), ChildIndex);
		Result->SetBoolField(TEXT("removed"), true);
		AddExecutionMetadata(Result, Params);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// reorder_widget_child
// ─────────────────────────────────────────────────────────────
class FTool_ReorderWidgetChild : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.child.reorder");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBPPath = Params->GetStringField(TEXT("widget_bp"));
		const FString ChildName = Params->GetStringField(TEXT("child_name"));
		const int32 NewIndex = Params->GetIntegerField(TEXT("new_index"));

		UWidgetBlueprint* WidgetBP =
			LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget Blueprint not found at '%s'."),
					*WidgetBPPath),
				TEXT("target_not_found"),
				404);
		}

		UWidget* Child =
			FindWidgetByName(WidgetBP->WidgetTree, ChildName);
		if (!Child)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Widget '%s' not found."), *ChildName),
				TEXT("target_not_found"),
				404);
		}

		int32 OldIndex = INDEX_NONE;
		UPanelWidget* Parent =
			UWidgetTree::FindWidgetParent(Child, OldIndex);
		if (!Parent)
		{
			return FMCPToolResult::Error(
				TEXT("The target widget has no panel parent."),
				TEXT("invalid_target"),
				422);
		}
		if (NewIndex < 0 || NewIndex >= Parent->GetChildrenCount())
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("new_index must be between 0 and %d."),
					Parent->GetChildrenCount() - 1),
				TEXT("invalid_params"),
				422);
		}

		WidgetBP->Modify();
		Parent->Modify();
		Child->Modify();
		Parent->ShiftChild(NewIndex, Child);

		MarkWidgetBlueprintEdited(WidgetBP, Params);
		WidgetBP->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetObjectField(
			TEXT("widgetRef"),
			MakeWidgetRef(WidgetBPPath, Child));
		Result->SetStringField(TEXT("parent"), Parent->GetName());
		Result->SetNumberField(TEXT("old_index"), OldIndex);
		Result->SetNumberField(TEXT("new_index"), NewIndex);
		AddExecutionMetadata(Result, Params);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// get_widget_hierarchy
// ─────────────────────────────────────────────────────────────
class FTool_GetWidgetHierarchy : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.hierarchy.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBPPath = Params->GetStringField(TEXT("widget_bp"));
		UWidgetBlueprint* WidgetBP =
			LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP || !WidgetBP->WidgetTree
			|| !WidgetBP->WidgetTree->RootWidget)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget Blueprint or root widget not found at '%s'."),
					*WidgetBPPath),
				TEXT("target_not_found"),
				404);
		}

		int32 WidgetCount = 0;
		WidgetBP->WidgetTree->ForEachWidget(
			[&WidgetCount](UWidget* Widget)
			{
				if (Widget)
				{
					++WidgetCount;
				}
			});

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget_bp"), WidgetBPPath);
		Result->SetNumberField(TEXT("count"), WidgetCount);
		TArray<TSharedPtr<FJsonValue>> Bindings;
		for (const FDelegateEditorBinding& Binding : WidgetBP->Bindings)
		{
			TSharedPtr<FJsonObject> BindingJson = MakeShared<FJsonObject>();
			BindingJson->SetStringField(TEXT("widget"), Binding.ObjectName);
			BindingJson->SetStringField(
				TEXT("event"),
				Binding.PropertyName.ToString());
			BindingJson->SetStringField(
				TEXT("function"),
				Binding.FunctionName.ToString());
			Bindings.Add(MakeShared<FJsonValueObject>(BindingJson));
		}
		Result->SetArrayField(TEXT("bindings"), Bindings);
		Result->SetObjectField(
			TEXT("root"),
			SerializeWidgetTreeNode(
				WidgetBPPath,
				WidgetBP->WidgetTree->RootWidget));
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// list_widget_blueprints
// ─────────────────────────────────────────────────────────────
class FTool_ListWidgetBlueprints : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.blueprint.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Filter = Params->HasField(TEXT("filter")) ? Params->GetStringField(TEXT("filter")) : TEXT("");

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByClass(UWidgetBlueprint::StaticClass()->GetClassPathName(), Assets, true);

		TArray<TSharedPtr<FJsonValue>> BPArray;
		for (const FAssetData& Asset : Assets)
		{
			FString AssetName = Asset.AssetName.ToString();
			if (!Filter.IsEmpty() && !AssetName.Contains(Filter))
			{
				continue;
			}

			TSharedPtr<FJsonObject> BPObj = MakeShared<FJsonObject>();
			BPObj->SetStringField(TEXT("name"), AssetName);
			BPObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			BPObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			BPArray.Add(MakeShared<FJsonValueObject>(BPObj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), BPArray.Num());
		Result->SetArrayField(TEXT("widget_blueprints"), BPArray);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// bind_widget_event
// ─────────────────────────────────────────────────────────────
class FTool_BindWidgetEvent : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.event.bind");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBPPath =
			Params->GetStringField(TEXT("widget_bp"));
		const FString WidgetName =
			Params->GetStringField(TEXT("widget_name"));
		const FString EventName =
			Params->GetStringField(TEXT("event_name"));
		const FString FunctionName =
			Params->GetStringField(TEXT("function_name"));

		UWidgetBlueprint* WidgetBP = LoadWidgetBlueprint(WidgetBPPath);
		if (!WidgetBP || !WidgetBP->WidgetTree)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Widget Blueprint not found at '%s'."),
					*WidgetBPPath),
				TEXT("target_not_found"),
				404);
		}
		const FString BeforeHash =
			UEAIIntegration::Workflow::FWorkflowRuntime::
				ComputeAssetStructureHash(WidgetBP);

		const bool bDeferred = IsDeferredWorkflowExecution(Params);
		UEAIIntegration::WidgetEventBindings::FUpsertResult UpsertResult;
		FString BindingError;
		FString BindingErrorCode;
		if (!UEAIIntegration::WidgetEventBindings::Upsert(
				WidgetBP,
				WidgetName,
				EventName,
				FunctionName,
				bDeferred,
				UpsertResult,
				BindingError,
				BindingErrorCode))
		{
			return FMCPToolResult::Error(
				BindingError,
				BindingErrorCode.IsEmpty()
					? TEXT("execution_failed")
					: BindingErrorCode,
				BindingErrorCode == TEXT("target_not_found")
					? 404
					: 422);
		}

		MarkWidgetBlueprintEdited(WidgetBP, Params);
		WidgetBP->MarkPackageDirty();

		bool bSaved = false;
		bool bVerified = false;
		FString SavedPath;
		if (!bDeferred)
		{
			FKismetEditorUtilities::CompileBlueprint(WidgetBP);
			if (WidgetBP->Status == BS_Error)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Widget Blueprint '%s' failed to compile after binding '%s.%s' to '%s'."),
						*WidgetBPPath,
						*WidgetName,
						*EventName,
						*FunctionName),
					TEXT("asset_compile_failed"),
					500);
			}

			int32 VerifiedCount = 0;
			TArray<FString> VerificationErrors;
			bVerified =
				UEAIIntegration::WidgetEventBindings::ValidateCompiled(
					WidgetBP,
					VerifiedCount,
					VerificationErrors);
			if (!bVerified)
			{
				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Errors;
				for (const FString& VerificationError : VerificationErrors)
				{
					Errors.Add(MakeShared<FJsonValueString>(VerificationError));
				}
				Details->SetArrayField(TEXT("errors"), Errors);
				FMCPToolResult Failure = FMCPToolResult::Error(
					TEXT("The widget event graph compiled but its generated delegate binding could not be verified."),
					TEXT("verification_failed"),
					500);
				Failure.Data = Details;
				return Failure;
			}

			UPackage* Package = WidgetBP->GetOutermost();
			SavedPath = FPackageName::LongPackageNameToFilename(
				Package->GetName(),
				FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			bSaved = UPackage::SavePackage(
				Package,
				WidgetBP,
				*SavedPath,
				SaveArgs);
			if (!bSaved)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Widget Blueprint '%s' compiled but could not be saved."),
						*WidgetBPPath),
					TEXT("asset_save_failed"),
					500);
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget"), WidgetName);
		Result->SetStringField(TEXT("event"), EventName);
		Result->SetStringField(TEXT("function"), FunctionName);
		Result->SetStringField(
			TEXT("status"),
			bDeferred ? TEXT("bound_pending_compile") : TEXT("bound"));
		Result->SetBoolField(
			TEXT("already_bound"),
			!UpsertResult.bChanged);
		Result->SetBoolField(
			TEXT("created_event_node"),
			UpsertResult.bCreatedEventNode);
		Result->SetBoolField(
			TEXT("created_call_node"),
			UpsertResult.bCreatedCallNode);
		Result->SetStringField(
			TEXT("event_node_id"),
			UpsertResult.EventNodeGuid.ToString(EGuidFormats::Digits));
		Result->SetStringField(
			TEXT("call_node_id"),
			UpsertResult.CallNodeGuid.ToString(EGuidFormats::Digits));
		Result->SetBoolField(TEXT("compiled"), !bDeferred);
		Result->SetBoolField(TEXT("verified"), bVerified);
		if (!SavedPath.IsEmpty())
		{
			Result->SetStringField(TEXT("saved_path"), SavedPath);
		}
		TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
		Mutation->SetBoolField(
			TEXT("changed"),
			UpsertResult.bChanged);
		Mutation->SetBoolField(TEXT("compiled"), !bDeferred);
		if (bDeferred)
		{
			Mutation->SetField(
				TEXT("saved"),
				MakeShared<FJsonValueNull>());
		}
		else
		{
			Mutation->SetBoolField(TEXT("saved"), bSaved);
		}
		Mutation->SetField(
			TEXT("reloaded"),
			MakeShared<FJsonValueNull>());
		Mutation->SetBoolField(TEXT("verified"), bVerified);
		Mutation->SetStringField(TEXT("beforeHash"), BeforeHash);
		Mutation->SetStringField(
			TEXT("afterHash"),
			UEAIIntegration::Workflow::FWorkflowRuntime::
				ComputeAssetStructureHash(WidgetBP));
		Mutation->SetArrayField(
			TEXT("warnings"),
			TArray<TSharedPtr<FJsonValue>>());
		Mutation->SetArrayField(
			TEXT("errors"),
			TArray<TSharedPtr<FJsonValue>>());
		Result->SetObjectField(TEXT("mutation"), Mutation);
		AddExecutionMetadata(Result, Params, bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────
namespace UEAIIntegrationTools
{
	void RegisterAdvancedUITools(FMCPToolRegistry& Registry);

	void RegisterUITools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_CreateWidgetBlueprint>());
		Registry.Register(MakeShared<FTool_AddWidgetChild>());
		Registry.Register(MakeShared<FTool_RemoveWidgetChild>());
		Registry.Register(MakeShared<FTool_ReorderWidgetChild>());
		Registry.Register(MakeShared<FTool_SetWidgetProperty>());
		Registry.Register(MakeShared<FTool_SetWidgetSlotLayout>());
		Registry.Register(MakeShared<FTool_GetWidgetHierarchy>());
		Registry.Register(MakeShared<FTool_ListWidgetBlueprints>());
		Registry.Register(MakeShared<FTool_BindWidgetEvent>());
		RegisterAdvancedUITools(Registry);
	}
}
