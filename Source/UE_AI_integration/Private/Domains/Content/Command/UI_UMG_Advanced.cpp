// Advanced UMG authoring, binding inspection, animation, and designer tools.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Domains/Content/Command/WidgetEventBindingSupport.h"
#include "Workflow/UEWorkflowExecutionContext.h"

#include "Dom/JsonValue.h"
#include "Animation/MovieScene2DTransformSection.h"
#include "Animation/MovieScene2DTransformTrack.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/GridSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/NamedSlotInterface.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/ComponentDelegateBinding.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Sections/MovieSceneBoolSection.h"
#include "Sections/MovieSceneByteSection.h"
#include "Sections/MovieSceneColorSection.h"
#include "Sections/MovieSceneFloatSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Slate/WidgetTransform.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Tracks/MovieSceneByteTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

namespace
{
UWidgetBlueprint* LoadWidgetBlueprintAdvanced(const FString& AssetPath)
{
	if (UWidgetBlueprint* Direct =
		LoadObject<UWidgetBlueprint>(nullptr, *AssetPath))
	{
		return Direct;
	}
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	const FString AssetName = FPackageName::GetShortName(PackageName);
	return PackageName.IsEmpty() || AssetName.IsEmpty()
		? nullptr
		: LoadObject<UWidgetBlueprint>(
			nullptr,
			*(PackageName + TEXT(".") + AssetName));
}

UWidget* FindWidgetAdvanced(
	UWidgetBlueprint* WidgetBlueprint,
	const FString& WidgetName)
{
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		return nullptr;
	}
	UWidget* Found = nullptr;
	WidgetBlueprint->WidgetTree->ForEachWidget(
		[&Found, &WidgetName](UWidget* Widget)
		{
			if (!Found && Widget && Widget->GetName() == WidgetName)
			{
				Found = Widget;
			}
		});
	return Found;
}

TSharedPtr<FJsonObject> MakeMutationState(
	const bool bChanged,
	const bool bCompiled,
	const bool bSaved,
	const bool bVerified)
{
	TSharedPtr<FJsonObject> Mutation = MakeShared<FJsonObject>();
	Mutation->SetBoolField(TEXT("changed"), bChanged);
	Mutation->SetBoolField(TEXT("compiled"), bCompiled);
	Mutation->SetBoolField(TEXT("saved"), bSaved);
	Mutation->SetField(
		TEXT("reloaded"),
		MakeShared<FJsonValueNull>());
	Mutation->SetBoolField(TEXT("verified"), bVerified);
	Mutation->SetArrayField(
		TEXT("warnings"),
		TArray<TSharedPtr<FJsonValue>>());
	Mutation->SetArrayField(
		TEXT("errors"),
		TArray<TSharedPtr<FJsonValue>>());
	return Mutation;
}

FMCPToolResult FinishWidgetMutation(
	UWidgetBlueprint* WidgetBlueprint,
	const TSharedPtr<FJsonObject>& Params,
	const bool bChanged,
	const bool bStructural,
	const TSharedPtr<FJsonObject>& Result)
{
	if (!WidgetBlueprint || !Result.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("Widget Blueprint mutation has no target or result."));
	}

	const bool bDeferred =
		UEAIIntegration::Workflow::ShouldDeferCompile(Params);
	if (bChanged)
	{
		UEAIIntegration::Workflow::MarkBlueprintChanged(
			WidgetBlueprint,
			Params,
			bStructural);
		WidgetBlueprint->MarkPackageDirty();
	}

	if (!bChanged || bDeferred)
	{
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("deferredCompile"), bDeferred);
		Result->SetObjectField(
			TEXT("mutation"),
			MakeMutationState(bChanged, false, false, !bChanged));
		return FMCPToolResult::Ok(Result);
	}

	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	if (WidgetBlueprint->Status == BS_Error)
	{
		return FMCPToolResult::Error(
			FString::Printf(
				TEXT("Widget Blueprint '%s' failed to compile."),
				*WidgetBlueprint->GetPathName()),
			TEXT("asset_compile_failed"),
			500);
	}

	UPackage* Package = WidgetBlueprint->GetOutermost();
	const FString FilePath = FPackageName::LongPackageNameToFilename(
		Package->GetName(),
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const bool bSaved = UPackage::SavePackage(
		Package,
		WidgetBlueprint,
		*FilePath,
		SaveArgs);
	if (!bSaved)
	{
		return FMCPToolResult::Error(
			FString::Printf(
				TEXT("Widget Blueprint '%s' compiled but could not be saved."),
				*WidgetBlueprint->GetPathName()),
			TEXT("asset_save_failed"),
			500);
	}

	Result->SetBoolField(TEXT("saved"), true);
	Result->SetBoolField(TEXT("deferredCompile"), false);
	Result->SetStringField(TEXT("saved_path"), FilePath);
	Result->SetObjectField(
		TEXT("mutation"),
		MakeMutationState(bChanged, true, true, true));
	return FMCPToolResult::Ok(Result);
}

bool TryReadNumberArrayAdvanced(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Field,
	const int32 ExpectedCount,
	TArray<double>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid()
		|| !Object->TryGetArrayField(Field, Values)
		|| !Values
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

FMargin ReadMargin(
	const TSharedPtr<FJsonObject>& Object,
	const FString& Field,
	const FMargin& DefaultValue)
{
	TArray<double> Values;
	return TryReadNumberArrayAdvanced(Object, Field, 4, Values)
		? FMargin(Values[0], Values[1], Values[2], Values[3])
		: DefaultValue;
}

EHorizontalAlignment ParseHorizontalAlignment(
	const FString& Value,
	const EHorizontalAlignment DefaultValue)
{
	if (Value.Equals(TEXT("Left"), ESearchCase::IgnoreCase))
	{
		return HAlign_Left;
	}
	if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
	{
		return HAlign_Center;
	}
	if (Value.Equals(TEXT("Right"), ESearchCase::IgnoreCase))
	{
		return HAlign_Right;
	}
	if (Value.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
	{
		return HAlign_Fill;
	}
	return DefaultValue;
}

EVerticalAlignment ParseVerticalAlignment(
	const FString& Value,
	const EVerticalAlignment DefaultValue)
{
	if (Value.Equals(TEXT("Top"), ESearchCase::IgnoreCase))
	{
		return VAlign_Top;
	}
	if (Value.Equals(TEXT("Center"), ESearchCase::IgnoreCase))
	{
		return VAlign_Center;
	}
	if (Value.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase))
	{
		return VAlign_Bottom;
	}
	if (Value.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))
	{
		return VAlign_Fill;
	}
	return DefaultValue;
}

bool HasGeneratedComponentBinding(
	const UWidgetBlueprint* WidgetBlueprint,
	const UK2Node_ComponentBoundEvent* EventNode)
{
	const UBlueprintGeneratedClass* GeneratedClass =
		WidgetBlueprint
		? Cast<UBlueprintGeneratedClass>(WidgetBlueprint->GeneratedClass)
		: nullptr;
	if (!GeneratedClass || !EventNode)
	{
		return false;
	}
	for (const UDynamicBlueprintBinding* DynamicBinding :
		GeneratedClass->DynamicBindingObjects)
	{
		const UComponentDelegateBinding* ComponentBinding =
			Cast<UComponentDelegateBinding>(DynamicBinding);
		if (ComponentBinding
			&& ComponentBinding->ComponentDelegateBindings.ContainsByPredicate(
				[EventNode](
					const FBlueprintComponentDelegateBinding& Binding)
				{
					return Binding.ComponentPropertyName
							== EventNode->ComponentPropertyName
						&& Binding.DelegatePropertyName
							== EventNode->DelegatePropertyName
						&& Binding.FunctionNameToBind
							== EventNode->CustomFunctionName;
				}))
		{
			return true;
		}
	}
	return false;
}

FName GetConnectedHandlerFunction(
	const UK2Node_ComponentBoundEvent* EventNode)
{
	FName OwnedFunction;
	if (UEAIIntegration::WidgetEventBindings::TryGetOwnedHandlerFunction(
			EventNode,
			OwnedFunction))
	{
		return OwnedFunction;
	}
	if (!EventNode)
	{
		return NAME_None;
	}
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	const UEdGraphPin* ExecPin =
		Schema->FindExecutionPin(*EventNode, EGPD_Output);
	if (!ExecPin)
	{
		return NAME_None;
	}
	for (const UEdGraphPin* Linked : ExecPin->LinkedTo)
	{
		const UK2Node_CallFunction* CallNode = Linked
			? Cast<UK2Node_CallFunction>(Linked->GetOwningNode())
			: nullptr;
		if (CallNode)
		{
			return CallNode->FunctionReference.GetMemberName();
		}
	}
	return NAME_None;
}

bool MatchesOptionalFilter(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	const FString& Value)
{
	FString Filter;
	return !Params->TryGetStringField(Field, Filter)
		|| Filter.IsEmpty()
		|| Value.Equals(Filter, ESearchCase::IgnoreCase);
}

class FTool_ListWidgetBindings final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.binding.list");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		if (!WidgetBlueprint)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint was not found."),
				TEXT("target_not_found"),
				404);
		}

		TArray<TSharedPtr<FJsonValue>> Bindings;
		for (const FDelegateEditorBinding& Binding :
			WidgetBlueprint->Bindings)
		{
			if (!MatchesOptionalFilter(
					Params,
					TEXT("widgetName"),
					Binding.ObjectName)
				|| !MatchesOptionalFilter(
					Params,
					TEXT("eventName"),
					Binding.PropertyName.ToString())
				|| !MatchesOptionalFilter(
					Params,
					TEXT("functionName"),
					Binding.FunctionName.ToString()))
			{
				continue;
			}

			UWidget* Widget = FindWidgetAdvanced(
				WidgetBlueprint,
				Binding.ObjectName);
			const UFunction* Function =
				WidgetBlueprint->GeneratedClass
				? WidgetBlueprint->GeneratedClass->FindFunctionByName(
					Binding.FunctionName)
				: nullptr;
			const FDelegateProperty* DelegateProperty = Widget
				? FindFProperty<FDelegateProperty>(
					Widget->GetClass(),
					Binding.PropertyName)
				: nullptr;
			const bool bSignatureMatches = Function
				&& DelegateProperty
				&& DelegateProperty->SignatureFunction
				&& Function->IsSignatureCompatibleWith(
					DelegateProperty->SignatureFunction,
					UFunction::GetDefaultIgnoredSignatureCompatibilityFlags()
						| CPF_ReturnParm);

			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("representation"), TEXT("propertyBinding"));
			Json->SetStringField(TEXT("widget"), Binding.ObjectName);
			Json->SetStringField(
				TEXT("event"),
				Binding.PropertyName.ToString());
			Json->SetStringField(
				TEXT("function"),
				Binding.FunctionName.ToString());
			Json->SetStringField(
				TEXT("kind"),
				Binding.Kind == EBindingKind::Function
					? TEXT("Function")
					: TEXT("Property"));
			Json->SetBoolField(TEXT("functionExists"), Function != nullptr);
			Json->SetBoolField(
				TEXT("signatureMatches"),
				bSignatureMatches);
			Json->SetBoolField(TEXT("generatedBinding"), false);
			Bindings.Add(MakeShared<FJsonValueObject>(Json));
		}

		TArray<UK2Node_ComponentBoundEvent*> EventNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(
			WidgetBlueprint,
			EventNodes);
		for (const UK2Node_ComponentBoundEvent* EventNode : EventNodes)
		{
			if (!EventNode)
			{
				continue;
			}
			const FName HandlerFunction =
				GetConnectedHandlerFunction(EventNode);
			if (!MatchesOptionalFilter(
					Params,
					TEXT("widgetName"),
					EventNode->ComponentPropertyName.ToString())
				|| !MatchesOptionalFilter(
					Params,
					TEXT("eventName"),
					EventNode->DelegatePropertyName.ToString())
				|| !MatchesOptionalFilter(
					Params,
					TEXT("functionName"),
					HandlerFunction.ToString()))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(
				TEXT("representation"),
				TEXT("componentBoundEvent"));
			Json->SetStringField(
				TEXT("widget"),
				EventNode->ComponentPropertyName.ToString());
			Json->SetStringField(
				TEXT("event"),
				EventNode->DelegatePropertyName.ToString());
			Json->SetStringField(
				TEXT("function"),
				HandlerFunction.ToString());
			Json->SetStringField(TEXT("kind"), TEXT("Function"));
			Json->SetBoolField(
				TEXT("functionExists"),
				WidgetBlueprint->GeneratedClass
					&& WidgetBlueprint->GeneratedClass->FindFunctionByName(
						HandlerFunction));
			Json->SetBoolField(TEXT("signatureMatches"), true);
			Json->SetBoolField(
				TEXT("generatedBinding"),
				HasGeneratedComponentBinding(
					WidgetBlueprint,
					EventNode));
			Json->SetStringField(
				TEXT("eventNodeId"),
				EventNode->NodeGuid.ToString(EGuidFormats::Digits));
			Bindings.Add(MakeShared<FJsonValueObject>(Json));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widgetBp"), WidgetBlueprintPath);
		Result->SetNumberField(TEXT("count"), Bindings.Num());
		Result->SetArrayField(TEXT("bindings"), Bindings);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_UnbindWidgetEvent final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.event.unbind");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString WidgetName =
			Params->GetStringField(TEXT("widgetName"));
		const FString EventName =
			Params->GetStringField(TEXT("eventName"));
		FString FunctionName;
		Params->TryGetStringField(TEXT("functionName"), FunctionName);

		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		if (!WidgetBlueprint)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint was not found."),
				TEXT("target_not_found"),
				404);
		}
		WidgetBlueprint->Modify();

		const int32 RemovedEditorBindings =
			WidgetBlueprint->Bindings.RemoveAll(
				[&](const FDelegateEditorBinding& Binding)
				{
					return Binding.ObjectName == WidgetName
						&& Binding.PropertyName == FName(*EventName)
						&& (FunctionName.IsEmpty()
							|| Binding.FunctionName
								== FName(*FunctionName));
				});

		int32 RemovedEventNodes = 0;
		TArray<UK2Node_ComponentBoundEvent*> EventNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(
			WidgetBlueprint,
			EventNodes);
		for (UK2Node_ComponentBoundEvent* EventNode : EventNodes)
		{
			if (!EventNode
				|| EventNode->ComponentPropertyName != FName(*WidgetName)
				|| EventNode->DelegatePropertyName != FName(*EventName))
			{
				continue;
			}
			const FName HandlerFunction =
				GetConnectedHandlerFunction(EventNode);
			if (!FunctionName.IsEmpty()
				&& HandlerFunction != FName(*FunctionName))
			{
				continue;
			}
			if (UEdGraph* Graph = EventNode->GetGraph())
			{
				Graph->Modify();
				EventNode->Modify();
				EventNode->BreakAllNodeLinks();
				Graph->RemoveNode(EventNode);
				++RemovedEventNodes;
			}
		}

		const int32 RemovedCount =
			RemovedEditorBindings + RemovedEventNodes;
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget"), WidgetName);
		Result->SetStringField(TEXT("event"), EventName);
		if (!FunctionName.IsEmpty())
		{
			Result->SetStringField(TEXT("function"), FunctionName);
		}
		Result->SetNumberField(TEXT("removed"), RemovedCount);
		Result->SetNumberField(
			TEXT("removed_property_bindings"),
			RemovedEditorBindings);
		Result->SetNumberField(
			TEXT("removed_event_nodes"),
			RemovedEventNodes);
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			RemovedCount > 0,
			true,
			Result);
	}
};

class FTool_RenameWidgetChild final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.child.rename");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString ChildName =
			Params->GetStringField(TEXT("childName"));
		const FString NewName =
			Params->GetStringField(TEXT("newName"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidget* Widget = FindWidgetAdvanced(WidgetBlueprint, ChildName);
		if (!WidgetBlueprint || !Widget)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint or child widget was not found."),
				TEXT("target_not_found"),
				404);
		}
		if (NewName.IsEmpty()
			|| FName(*NewName).IsNone()
			|| FindWidgetAdvanced(WidgetBlueprint, NewName))
		{
			return FMCPToolResult::Error(
				TEXT("newName is empty, invalid, or already in use."),
				TEXT("invalid_params"),
				422);
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		Widget->Modify();
		const FName OldName = Widget->GetFName();
		if (!Widget->Rename(
				*NewName,
				WidgetBlueprint->WidgetTree,
				REN_DontCreateRedirectors | REN_ForceNoResetLoaders))
		{
			return FMCPToolResult::Error(
				TEXT("The widget object could not be renamed."));
		}
		Widget->SetDisplayLabel(NewName);
		FBlueprintEditorUtils::ReplaceVariableReferences(
			WidgetBlueprint,
			OldName,
			FName(*NewName));
		for (FDelegateEditorBinding& Binding : WidgetBlueprint->Bindings)
		{
			if (Binding.ObjectName == ChildName)
			{
				Binding.ObjectName = NewName;
			}
		}
		TArray<UK2Node_ComponentBoundEvent*> EventNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass(
			WidgetBlueprint,
			EventNodes);
		for (UK2Node_ComponentBoundEvent* EventNode : EventNodes)
		{
			if (EventNode
				&& EventNode->ComponentPropertyName == OldName)
			{
				EventNode->Modify();
				EventNode->ComponentPropertyName = FName(*NewName);
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("old_name"), ChildName);
		Result->SetStringField(TEXT("newName"), NewName);
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_CopyWidgetChild final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.child.copy");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString ChildName =
			Params->GetStringField(TEXT("childName"));
		const FString NewName =
			Params->GetStringField(TEXT("newName"));
		FString ParentName;
		Params->TryGetStringField(TEXT("parentName"), ParentName);

		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidget* SourceWidget =
			FindWidgetAdvanced(WidgetBlueprint, ChildName);
		if (!WidgetBlueprint || !SourceWidget)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint or source widget was not found."),
				TEXT("target_not_found"),
				404);
		}
		if (NewName.IsEmpty()
			|| FindWidgetAdvanced(WidgetBlueprint, NewName))
		{
			return FMCPToolResult::Error(
				TEXT("newName is empty or already in use."),
				TEXT("invalid_params"),
				422);
		}

		int32 SourceIndex = INDEX_NONE;
		UPanelWidget* SourceParent =
			UWidgetTree::FindWidgetParent(SourceWidget, SourceIndex);
		UPanelWidget* DestinationParent = ParentName.IsEmpty()
			? SourceParent
			: Cast<UPanelWidget>(
				FindWidgetAdvanced(WidgetBlueprint, ParentName));
		if (!DestinationParent)
		{
			return FMCPToolResult::Error(
				TEXT("Destination parent was not found or is not a panel."),
				TEXT("invalid_target"),
				422);
		}
		if (!DestinationParent->CanAddMoreChildren())
		{
			return FMCPToolResult::Error(
				TEXT("Destination parent cannot accept another child."),
				TEXT("parent_capacity_exceeded"),
				409);
		}

		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		DestinationParent->Modify();
		FString ExportedText;
		TArray<UWidget*> WidgetsToExport;
		WidgetsToExport.Add(SourceWidget);
		FWidgetBlueprintEditorUtils::ExportWidgetsToText(
			WidgetsToExport,
			ExportedText);
		TSet<UWidget*> ImportedWidgets;
		TMap<FName, UWidgetSlotPair*> ImportedSlotData;
		FWidgetBlueprintEditorUtils::ImportWidgetsFromText(
			WidgetBlueprint,
			ExportedText,
			ImportedWidgets,
			ImportedSlotData);
		if (ImportedWidgets.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Widget import produced no copied widgets."));
		}

		UWidget* CopiedRoot = nullptr;
		for (UWidget* Imported : ImportedWidgets)
		{
			int32 ParentIndex = INDEX_NONE;
			UPanelWidget* ImportedParent =
				UWidgetTree::FindWidgetParent(Imported, ParentIndex);
			if (!ImportedParent || !ImportedWidgets.Contains(ImportedParent))
			{
				CopiedRoot = Imported;
				break;
			}
		}
		if (!CopiedRoot)
		{
			return FMCPToolResult::Error(
				TEXT("Copied widget hierarchy has no root."));
		}

		CopiedRoot->Modify();
		if (!CopiedRoot->Rename(
				*NewName,
				WidgetBlueprint->WidgetTree,
				REN_DontCreateRedirectors | REN_ForceNoResetLoaders))
		{
			return FMCPToolResult::Error(
				TEXT("Copied widget could not be assigned newName."));
		}
		CopiedRoot->SetDisplayLabel(NewName);
		UPanelSlot* NewSlot = DestinationParent->AddChild(CopiedRoot);
		if (!NewSlot)
		{
			return FMCPToolResult::Error(
				TEXT("Destination parent rejected the copied widget."));
		}

		double RequestedIndex = -1.0;
		if (Params->TryGetNumberField(TEXT("newIndex"), RequestedIndex))
		{
			const int32 NewIndex = FMath::Clamp(
				static_cast<int32>(RequestedIndex),
				0,
				DestinationParent->GetChildrenCount() - 1);
			DestinationParent->ShiftChild(NewIndex, CopiedRoot);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("source"), ChildName);
		Result->SetStringField(TEXT("copy"), NewName);
		Result->SetStringField(
			TEXT("parent"),
			DestinationParent->GetName());
		Result->SetNumberField(
			TEXT("copied_widget_count"),
			ImportedWidgets.Num());
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_ReparentWidgetChild final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.child.reparent");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString ChildName =
			Params->GetStringField(TEXT("childName"));
		const FString NewParentName =
			Params->GetStringField(TEXT("newParentName"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidget* Child = FindWidgetAdvanced(WidgetBlueprint, ChildName);
		UPanelWidget* NewParent = Cast<UPanelWidget>(
			FindWidgetAdvanced(WidgetBlueprint, NewParentName));
		if (!WidgetBlueprint || !Child || !NewParent)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint, child, or destination panel was not found."),
				TEXT("target_not_found"),
				404);
		}
		if (Child == NewParent)
		{
			return FMCPToolResult::Error(
				TEXT("A widget cannot be parented to itself."),
				TEXT("invalid_target"),
				422);
		}
		TArray<UWidget*> Descendants;
		UWidgetTree::GetChildWidgets(Child, Descendants);
		if (Descendants.Contains(NewParent))
		{
			return FMCPToolResult::Error(
				TEXT("Reparenting would create a widget-tree cycle."),
				TEXT("invalid_target"),
				422);
		}

		int32 OldIndex = INDEX_NONE;
		UPanelWidget* OldParent =
			UWidgetTree::FindWidgetParent(Child, OldIndex);
		if (!OldParent)
		{
			return FMCPToolResult::Error(
				TEXT("The root widget cannot be reparented; use root.set."),
				TEXT("invalid_target"),
				422);
		}
		if (OldParent == NewParent)
		{
			double RequestedIndex = OldIndex;
			Params->TryGetNumberField(TEXT("newIndex"), RequestedIndex);
			const int32 NewIndex = FMath::Clamp(
				static_cast<int32>(RequestedIndex),
				0,
				NewParent->GetChildrenCount() - 1);
			if (NewIndex != OldIndex)
			{
				NewParent->Modify();
				NewParent->ShiftChild(NewIndex, Child);
			}
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("child"), ChildName);
			Result->SetStringField(TEXT("parent"), NewParentName);
			Result->SetNumberField(TEXT("old_index"), OldIndex);
			Result->SetNumberField(TEXT("newIndex"), NewIndex);
			return FinishWidgetMutation(
				WidgetBlueprint,
				Params,
				NewIndex != OldIndex,
				true,
				Result);
		}
		if (!NewParent->CanAddMoreChildren())
		{
			return FMCPToolResult::Error(
				TEXT("Destination parent cannot accept another child."),
				TEXT("parent_capacity_exceeded"),
				409);
		}

		WidgetBlueprint->Modify();
		OldParent->Modify();
		NewParent->Modify();
		Child->Modify();
		if (!OldParent->RemoveChild(Child)
			|| !NewParent->AddChild(Child))
		{
			return FMCPToolResult::Error(
				TEXT("Failed to move the widget between panels."));
		}
		double RequestedIndex = -1.0;
		if (Params->TryGetNumberField(TEXT("newIndex"), RequestedIndex))
		{
			NewParent->ShiftChild(
				FMath::Clamp(
					static_cast<int32>(RequestedIndex),
					0,
					NewParent->GetChildrenCount() - 1),
				Child);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("child"), ChildName);
		Result->SetStringField(TEXT("old_parent"), OldParent->GetName());
		Result->SetStringField(TEXT("new_parent"), NewParentName);
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_SetNamedSlotContent final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.named_slot.set");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString HostName =
			Params->GetStringField(TEXT("hostName"));
		const FString SlotName =
			Params->GetStringField(TEXT("slotName"));
		FString ContentName;
		Params->TryGetStringField(TEXT("contentName"), ContentName);

		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidget* Host = FindWidgetAdvanced(WidgetBlueprint, HostName);
		INamedSlotInterface* NamedSlotHost =
			Host ? Cast<INamedSlotInterface>(Host) : nullptr;
		if (!WidgetBlueprint || !Host || !NamedSlotHost)
		{
			return FMCPToolResult::Error(
				TEXT("Named-slot host was not found."),
				TEXT("target_not_found"),
				404);
		}
		TArray<FName> AvailableSlots;
		NamedSlotHost->GetSlotNames(AvailableSlots);
		if (!AvailableSlots.Contains(FName(*SlotName)))
		{
			return FMCPToolResult::Error(
				TEXT("The requested named slot does not exist on the host."),
				TEXT("invalid_target"),
				422);
		}

		UWidget* Content = ContentName.IsEmpty()
			? nullptr
			: FindWidgetAdvanced(WidgetBlueprint, ContentName);
		if (!ContentName.IsEmpty() && !Content)
		{
			return FMCPToolResult::Error(
				TEXT("Named-slot content widget was not found."),
				TEXT("target_not_found"),
				404);
		}
		if (Content == Host)
		{
			return FMCPToolResult::Error(
				TEXT("A named-slot host cannot contain itself."),
				TEXT("invalid_target"),
				422);
		}

		UWidget* PreviousContent =
			NamedSlotHost->GetContentForSlot(FName(*SlotName));
		if (PreviousContent == Content)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("host"), HostName);
			Result->SetStringField(TEXT("slot"), SlotName);
			Result->SetStringField(TEXT("content"), ContentName);
			return FinishWidgetMutation(
				WidgetBlueprint,
				Params,
				false,
				true,
				Result);
		}

		WidgetBlueprint->Modify();
		Host->Modify();
		if (Content)
		{
			int32 ParentIndex = INDEX_NONE;
			if (UPanelWidget* Parent =
				UWidgetTree::FindWidgetParent(Content, ParentIndex))
			{
				Parent->Modify();
				Parent->RemoveChild(Content);
			}
			Content->Modify();
		}
		NamedSlotHost->SetContentForSlot(FName(*SlotName), Content);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("host"), HostName);
		Result->SetStringField(TEXT("slot"), SlotName);
		Result->SetStringField(TEXT("content"), ContentName);
		Result->SetStringField(
			TEXT("previous_content"),
			PreviousContent ? PreviousContent->GetName() : FString());
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_SetWidgetRoot final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.root.set");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString WidgetName =
			Params->GetStringField(TEXT("widgetName"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidget* NewRoot =
			FindWidgetAdvanced(WidgetBlueprint, WidgetName);
		if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !NewRoot)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint or root candidate was not found."),
				TEXT("target_not_found"),
				404);
		}
		UWidget* OldRoot = WidgetBlueprint->WidgetTree->RootWidget;
		if (OldRoot == NewRoot)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("root"), WidgetName);
			return FinishWidgetMutation(
				WidgetBlueprint,
				Params,
				false,
				true,
				Result);
		}

		int32 ParentIndex = INDEX_NONE;
		if (UPanelWidget* Parent =
			UWidgetTree::FindWidgetParent(NewRoot, ParentIndex))
		{
			Parent->Modify();
			Parent->RemoveChild(NewRoot);
		}
		WidgetBlueprint->Modify();
		WidgetBlueprint->WidgetTree->Modify();
		NewRoot->Modify();
		WidgetBlueprint->WidgetTree->RootWidget = NewRoot;

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("root"), WidgetName);
		Result->SetStringField(
			TEXT("previous_root"),
			OldRoot ? OldRoot->GetName() : FString());
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_SetWidgetSlotProperties final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.slot.properties.set");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString WidgetName =
			Params->GetStringField(TEXT("widgetName"));
		const FString SlotType =
			Params->GetStringField(TEXT("slotType"));
		const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("properties"), PropertiesPtr)
			|| !PropertiesPtr
			|| !PropertiesPtr->IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("properties must be an object."),
				TEXT("invalid_params"),
				422);
		}
		const TSharedPtr<FJsonObject> Properties = *PropertiesPtr;

		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidget* Widget = FindWidgetAdvanced(
			WidgetBlueprint,
			WidgetName);
		if (!WidgetBlueprint || !Widget || !Widget->Slot)
		{
			return FMCPToolResult::Error(
				TEXT("Widget or panel slot was not found."),
				TEXT("target_not_found"),
				404);
		}

		WidgetBlueprint->Modify();
		Widget->Modify();
		Widget->Slot->Modify();
		bool bApplied = false;
		TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();

		auto ReadHorizontal =
			[&Properties](
				const EHorizontalAlignment DefaultValue)
			{
				FString Value;
				return Properties->TryGetStringField(
						TEXT("horizontalAlignment"),
						Value)
					? ParseHorizontalAlignment(Value, DefaultValue)
					: DefaultValue;
			};
		auto ReadVertical =
			[&Properties](
				const EVerticalAlignment DefaultValue)
			{
				FString Value;
				return Properties->TryGetStringField(
						TEXT("verticalAlignment"),
						Value)
					? ParseVerticalAlignment(Value, DefaultValue)
					: DefaultValue;
			};

		if (SlotType == TEXT("Canvas"))
		{
			UCanvasPanelSlot* Slot = Cast<UCanvasPanelSlot>(Widget->Slot);
			if (!Slot)
			{
				return FMCPToolResult::Error(
					TEXT("slotType Canvas does not match the widget's actual slot."),
					TEXT("unsupported_slot_type"),
					422);
			}
			TArray<double> Values;
			if (Properties->HasField(TEXT("anchors"))
				&& !TryReadNumberArrayAdvanced(
					Properties,
					TEXT("anchors"),
					4,
					Values))
			{
				return FMCPToolResult::Error(
					TEXT("anchors must contain four numbers."),
					TEXT("invalid_params"),
					422);
			}
			if (Values.Num() == 4)
			{
				Slot->SetAnchors(
					FAnchors(
						Values[0],
						Values[1],
						Values[2],
						Values[3]));
				bApplied = true;
			}
			Values.Reset();
			if (Properties->HasField(TEXT("offsets"))
				&& !TryReadNumberArrayAdvanced(
					Properties,
					TEXT("offsets"),
					4,
					Values))
			{
				return FMCPToolResult::Error(
					TEXT("offsets must contain four numbers."),
					TEXT("invalid_params"),
					422);
			}
			if (Values.Num() == 4)
			{
				Slot->SetOffsets(
					FMargin(
						Values[0],
						Values[1],
						Values[2],
						Values[3]));
				bApplied = true;
			}
			Values.Reset();
			if (Properties->HasField(TEXT("alignment"))
				&& !TryReadNumberArrayAdvanced(
					Properties,
					TEXT("alignment"),
					2,
					Values))
			{
				return FMCPToolResult::Error(
					TEXT("alignment must contain two numbers."),
					TEXT("invalid_params"),
					422);
			}
			if (Values.Num() == 2)
			{
				Slot->SetAlignment(FVector2D(Values[0], Values[1]));
				bApplied = true;
			}
			bool bAutoSize = false;
			if (Properties->TryGetBoolField(TEXT("autoSize"), bAutoSize))
			{
				Slot->SetAutoSize(bAutoSize);
				bApplied = true;
			}
			double ZOrder = 0.0;
			if (Properties->TryGetNumberField(TEXT("zOrder"), ZOrder))
			{
				Slot->SetZOrder(static_cast<int32>(ZOrder));
				bApplied = true;
			}
			Applied->SetStringField(TEXT("slotType"), TEXT("Canvas"));
			Applied->SetBoolField(TEXT("autoSize"), Slot->GetAutoSize());
			Applied->SetNumberField(TEXT("zOrder"), Slot->GetZOrder());
		}
		else if (SlotType == TEXT("HorizontalBox")
			|| SlotType == TEXT("VerticalBox"))
		{
			UHorizontalBoxSlot* HorizontalSlot =
				Cast<UHorizontalBoxSlot>(Widget->Slot);
			UVerticalBoxSlot* VerticalSlot =
				Cast<UVerticalBoxSlot>(Widget->Slot);
			if ((SlotType == TEXT("HorizontalBox") && !HorizontalSlot)
				|| (SlotType == TEXT("VerticalBox") && !VerticalSlot))
			{
				return FMCPToolResult::Error(
					TEXT("slotType does not match the widget's actual box slot."),
					TEXT("unsupported_slot_type"),
					422);
			}

			FMargin Padding = HorizontalSlot
				? HorizontalSlot->GetPadding()
				: VerticalSlot->GetPadding();
			if (Properties->HasField(TEXT("padding")))
			{
				TArray<double> PaddingValues;
				if (!TryReadNumberArrayAdvanced(
						Properties,
						TEXT("padding"),
						4,
						PaddingValues))
				{
					return FMCPToolResult::Error(
						TEXT("padding must contain four numbers."),
						TEXT("invalid_params"),
						422);
				}
				Padding = FMargin(
					PaddingValues[0],
					PaddingValues[1],
					PaddingValues[2],
					PaddingValues[3]);
				bApplied = true;
			}
			FSlateChildSize Size = HorizontalSlot
				? HorizontalSlot->GetSize()
				: VerticalSlot->GetSize();
			FString SizeRule;
			if (Properties->TryGetStringField(TEXT("sizeRule"), SizeRule))
			{
				Size.SizeRule =
					SizeRule.Equals(TEXT("Fill"), ESearchCase::IgnoreCase)
					? ESlateSizeRule::Fill
					: ESlateSizeRule::Automatic;
				bApplied = true;
			}
			double Fill = Size.Value;
			if (Properties->TryGetNumberField(TEXT("fill"), Fill))
			{
				Size.Value = Fill;
				bApplied = true;
			}
			const EHorizontalAlignment HorizontalAlignment =
				ReadHorizontal(
					HorizontalSlot
						? HorizontalSlot->GetHorizontalAlignment()
						: VerticalSlot->GetHorizontalAlignment());
			const EVerticalAlignment VerticalAlignment =
				ReadVertical(
					HorizontalSlot
						? HorizontalSlot->GetVerticalAlignment()
						: VerticalSlot->GetVerticalAlignment());
			bApplied = bApplied
				|| Properties->HasField(TEXT("horizontalAlignment"))
				|| Properties->HasField(TEXT("verticalAlignment"));
			if (HorizontalSlot)
			{
				HorizontalSlot->SetPadding(Padding);
				HorizontalSlot->SetSize(Size);
				HorizontalSlot->SetHorizontalAlignment(HorizontalAlignment);
				HorizontalSlot->SetVerticalAlignment(VerticalAlignment);
			}
			else
			{
				VerticalSlot->SetPadding(Padding);
				VerticalSlot->SetSize(Size);
				VerticalSlot->SetHorizontalAlignment(HorizontalAlignment);
				VerticalSlot->SetVerticalAlignment(VerticalAlignment);
			}
			Applied->SetStringField(TEXT("slotType"), SlotType);
			Applied->SetStringField(
				TEXT("sizeRule"),
				Size.SizeRule == ESlateSizeRule::Fill
					? TEXT("Fill")
					: TEXT("Auto"));
			Applied->SetNumberField(TEXT("fill"), Size.Value);
		}
		else if (SlotType == TEXT("Grid"))
		{
			UGridSlot* Slot = Cast<UGridSlot>(Widget->Slot);
			if (!Slot)
			{
				return FMCPToolResult::Error(
					TEXT("slotType Grid does not match the widget's actual slot."),
					TEXT("unsupported_slot_type"),
					422);
			}
			if (Properties->HasField(TEXT("padding")))
			{
				TArray<double> Values;
				if (!TryReadNumberArrayAdvanced(
						Properties,
						TEXT("padding"),
						4,
						Values))
				{
					return FMCPToolResult::Error(
						TEXT("padding must contain four numbers."),
						TEXT("invalid_params"),
						422);
				}
				Slot->SetPadding(
					FMargin(Values[0], Values[1], Values[2], Values[3]));
				bApplied = true;
			}
			double Number = 0.0;
			if (Properties->TryGetNumberField(TEXT("row"), Number))
			{
				Slot->SetRow(static_cast<int32>(Number));
				bApplied = true;
			}
			if (Properties->TryGetNumberField(TEXT("column"), Number))
			{
				Slot->SetColumn(static_cast<int32>(Number));
				bApplied = true;
			}
			if (Properties->TryGetNumberField(TEXT("rowSpan"), Number))
			{
				Slot->SetRowSpan(static_cast<int32>(Number));
				bApplied = true;
			}
			if (Properties->TryGetNumberField(TEXT("columnSpan"), Number))
			{
				Slot->SetColumnSpan(static_cast<int32>(Number));
				bApplied = true;
			}
			if (Properties->TryGetNumberField(TEXT("layer"), Number))
			{
				Slot->SetLayer(static_cast<int32>(Number));
				bApplied = true;
			}
			TArray<double> Nudge;
			if (Properties->HasField(TEXT("nudge"))
				&& !TryReadNumberArrayAdvanced(
					Properties,
					TEXT("nudge"),
					2,
					Nudge))
			{
				return FMCPToolResult::Error(
					TEXT("nudge must contain two numbers."),
					TEXT("invalid_params"),
					422);
			}
			if (Nudge.Num() == 2)
			{
				Slot->SetNudge(FVector2D(Nudge[0], Nudge[1]));
				bApplied = true;
			}
			if (Properties->HasField(TEXT("horizontalAlignment")))
			{
				Slot->SetHorizontalAlignment(
					ReadHorizontal(Slot->GetHorizontalAlignment()));
				bApplied = true;
			}
			if (Properties->HasField(TEXT("verticalAlignment")))
			{
				Slot->SetVerticalAlignment(
					ReadVertical(Slot->GetVerticalAlignment()));
				bApplied = true;
			}
			Applied->SetStringField(TEXT("slotType"), TEXT("Grid"));
			Applied->SetNumberField(TEXT("row"), Slot->GetRow());
			Applied->SetNumberField(TEXT("column"), Slot->GetColumn());
			Applied->SetNumberField(TEXT("rowSpan"), Slot->GetRowSpan());
			Applied->SetNumberField(
				TEXT("columnSpan"),
				Slot->GetColumnSpan());
			Applied->SetNumberField(TEXT("layer"), Slot->GetLayer());
		}
		else if (SlotType == TEXT("UniformGrid"))
		{
			UUniformGridSlot* Slot =
				Cast<UUniformGridSlot>(Widget->Slot);
			if (!Slot)
			{
				return FMCPToolResult::Error(
					TEXT("slotType UniformGrid does not match the widget's actual slot."),
					TEXT("unsupported_slot_type"),
					422);
			}
			double Number = 0.0;
			if (Properties->TryGetNumberField(TEXT("row"), Number))
			{
				Slot->SetRow(static_cast<int32>(Number));
				bApplied = true;
			}
			if (Properties->TryGetNumberField(TEXT("column"), Number))
			{
				Slot->SetColumn(static_cast<int32>(Number));
				bApplied = true;
			}
			if (Properties->HasField(TEXT("horizontalAlignment")))
			{
				Slot->SetHorizontalAlignment(
					ReadHorizontal(Slot->GetHorizontalAlignment()));
				bApplied = true;
			}
			if (Properties->HasField(TEXT("verticalAlignment")))
			{
				Slot->SetVerticalAlignment(
					ReadVertical(Slot->GetVerticalAlignment()));
				bApplied = true;
			}
			Applied->SetStringField(TEXT("slotType"), TEXT("UniformGrid"));
			Applied->SetNumberField(TEXT("row"), Slot->GetRow());
			Applied->SetNumberField(TEXT("column"), Slot->GetColumn());
		}
		else if (SlotType == TEXT("Overlay"))
		{
			UOverlaySlot* Slot = Cast<UOverlaySlot>(Widget->Slot);
			if (!Slot)
			{
				return FMCPToolResult::Error(
					TEXT("slotType Overlay does not match the widget's actual slot."),
					TEXT("unsupported_slot_type"),
					422);
			}
			if (Properties->HasField(TEXT("padding")))
			{
				TArray<double> Values;
				if (!TryReadNumberArrayAdvanced(
						Properties,
						TEXT("padding"),
						4,
						Values))
				{
					return FMCPToolResult::Error(
						TEXT("padding must contain four numbers."),
						TEXT("invalid_params"),
						422);
				}
				Slot->SetPadding(
					FMargin(Values[0], Values[1], Values[2], Values[3]));
				bApplied = true;
			}
			if (Properties->HasField(TEXT("horizontalAlignment")))
			{
				Slot->SetHorizontalAlignment(
					ReadHorizontal(Slot->GetHorizontalAlignment()));
				bApplied = true;
			}
			if (Properties->HasField(TEXT("verticalAlignment")))
			{
				Slot->SetVerticalAlignment(
					ReadVertical(Slot->GetVerticalAlignment()));
				bApplied = true;
			}
			Applied->SetStringField(TEXT("slotType"), TEXT("Overlay"));
		}
		else
		{
			return FMCPToolResult::Error(
				TEXT("Unsupported slotType."),
				TEXT("unsupported_slot_type"),
				422);
		}

		if (!bApplied)
		{
			return FMCPToolResult::Error(
				TEXT("properties did not contain a supported field."),
				TEXT("invalid_params"),
				422);
		}
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widget"), WidgetName);
		Result->SetObjectField(TEXT("slot"), Applied);
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			false,
			Result);
	}
};

UWidgetAnimation* FindWidgetAnimation(
	UWidgetBlueprint* WidgetBlueprint,
	const FString& AnimationName)
{
	if (!WidgetBlueprint)
	{
		return nullptr;
	}
	for (UWidgetAnimation* Animation : WidgetBlueprint->Animations)
	{
		if (Animation
			&& (Animation->GetName().Equals(
					AnimationName,
					ESearchCase::IgnoreCase)
				|| Animation->GetDisplayLabel().Equals(
					AnimationName,
					ESearchCase::IgnoreCase)))
		{
			return Animation;
		}
	}
	return nullptr;
}

FGuid FindOrAddAnimationBinding(
	UWidgetAnimation* Animation,
	const UWidget* Widget,
	const UWidget* RootWidget)
{
	if (!Animation || !Animation->GetMovieScene() || !Widget)
	{
		return FGuid();
	}
	for (const FWidgetAnimationBinding& Binding :
		Animation->AnimationBindings)
	{
		if (Binding.WidgetName == Widget->GetFName()
			&& Binding.bIsRootWidget == (Widget == RootWidget))
		{
			return Binding.AnimationGuid;
		}
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	const FGuid Guid = MovieScene->AddPossessable(
		Widget->GetName(),
		Widget->GetClass());
	FWidgetAnimationBinding Binding;
	Binding.WidgetName = Widget->GetFName();
	Binding.AnimationGuid = Guid;
	Binding.bIsRootWidget = Widget == RootWidget;
	Animation->AnimationBindings.Add(Binding);
	return Guid;
}

void RemoveExistingPropertyTracks(
	UMovieScene* MovieScene,
	const FGuid& BindingGuid,
	const FName PropertyName)
{
	FMovieSceneBinding* Binding =
		MovieScene ? MovieScene->FindBinding(BindingGuid) : nullptr;
	if (!Binding)
	{
		return;
	}
	TArray<UMovieSceneTrack*> TracksToRemove;
	for (UMovieSceneTrack* Track : Binding->GetTracks())
	{
		const UMovieScenePropertyTrack* PropertyTrack =
			Cast<UMovieScenePropertyTrack>(Track);
		if (PropertyTrack
			&& PropertyTrack->GetPropertyName() == PropertyName)
		{
			TracksToRemove.Add(Track);
		}
	}
	for (UMovieSceneTrack* Track : TracksToRemove)
	{
		if (Track)
		{
			MovieScene->RemoveTrack(*Track);
		}
	}
}

bool ReadAnimationKeyObject(
	const TSharedPtr<FJsonValue>& KeyValue,
	double& OutTime,
	TSharedPtr<FJsonValue>& OutValue)
{
	if (!KeyValue.IsValid() || KeyValue->Type != EJson::Object)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Key = KeyValue->AsObject();
	if (!Key.IsValid()
		|| !Key->TryGetNumberField(TEXT("time"), OutTime))
	{
		return false;
	}
	OutValue = Key->TryGetField(TEXT("value"));
	return OutValue.IsValid();
}

bool IsFiniteNumberValue(
	const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid()
		&& Value->Type == EJson::Number
		&& FMath::IsFinite(Value->AsNumber());
}

bool IsFiniteNumberArrayValue(
	const TSharedPtr<FJsonValue>& Value,
	const int32 ExpectedCount)
{
	if (!Value.IsValid() || Value->Type != EJson::Array)
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
	return Values.Num() == ExpectedCount
		&& !Values.ContainsByPredicate(
			[](const TSharedPtr<FJsonValue>& Item)
			{
				return !IsFiniteNumberValue(Item);
			});
}

bool IsWidgetAnimationPropertyCompatible(
	const FProperty* Property,
	const FString& ValueType)
{
	if (!Property)
	{
		return false;
	}
	if (ValueType == TEXT("float"))
	{
		return CastField<const FFloatProperty>(Property) != nullptr;
	}
	if (ValueType == TEXT("bool"))
	{
		return CastField<const FBoolProperty>(Property) != nullptr;
	}
	if (ValueType == TEXT("enum"))
	{
		if (CastField<const FEnumProperty>(Property))
		{
			return true;
		}
		const FByteProperty* ByteProperty =
			CastField<const FByteProperty>(Property);
		return ByteProperty && ByteProperty->Enum;
	}

	const FStructProperty* StructProperty =
		CastField<const FStructProperty>(Property);
	if (!StructProperty)
	{
		return false;
	}
	if (ValueType == TEXT("vector2"))
	{
		return StructProperty->Struct ==
			TBaseStructure<FVector2D>::Get();
	}
	if (ValueType == TEXT("color"))
	{
		return StructProperty->Struct ==
				TBaseStructure<FLinearColor>::Get()
			|| StructProperty->Struct ==
				TBaseStructure<FColor>::Get();
	}
	if (ValueType == TEXT("transform2d"))
	{
		return StructProperty->Struct ==
			FWidgetTransform::StaticStruct();
	}
	return false;
}

bool ValidateWidgetAnimationKeys(
	const FString& ValueType,
	const TArray<TSharedPtr<FJsonValue>>& Keys,
	FString& OutError)
{
	for (const TSharedPtr<FJsonValue>& KeyValue : Keys)
	{
		double Time = 0.0;
		TSharedPtr<FJsonValue> Value;
		if (!ReadAnimationKeyObject(KeyValue, Time, Value)
			|| Time < 0.0
			|| !FMath::IsFinite(Time))
		{
			OutError =
				TEXT("Every animation key requires a finite, non-negative time and a value.");
			return false;
		}

		if (ValueType == TEXT("float"))
		{
			if (!IsFiniteNumberValue(Value))
			{
				OutError =
					TEXT("Float animation values must be finite numbers.");
				return false;
			}
		}
		else if (ValueType == TEXT("bool"))
		{
			if (!Value.IsValid() || Value->Type != EJson::Boolean)
			{
				OutError =
					TEXT("Bool animation values must be booleans.");
				return false;
			}
		}
		else if (ValueType == TEXT("enum"))
		{
			if (!IsFiniteNumberValue(Value))
			{
				OutError =
					TEXT("Enum animation values must be integer bytes.");
				return false;
			}
			const double Number = Value->AsNumber();
			const int32 Rounded = FMath::RoundToInt(Number);
			if (Number < 0.0
				|| Number > 255.0
				|| !FMath::IsNearlyEqual(
					Number,
					static_cast<double>(Rounded)))
			{
				OutError =
					TEXT("Enum animation values must be integer bytes in [0,255].");
				return false;
			}
		}
		else if (ValueType == TEXT("vector2"))
		{
			if (!IsFiniteNumberArrayValue(Value, 2))
			{
				OutError =
					TEXT("Vector2D animation values must contain two finite numbers.");
				return false;
			}
		}
		else if (ValueType == TEXT("color"))
		{
			if (!IsFiniteNumberArrayValue(Value, 4))
			{
				OutError =
					TEXT("Color animation values must contain four finite numbers.");
				return false;
			}
		}
		else if (ValueType == TEXT("transform2d"))
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				OutError =
					TEXT("2D transform animation values must be objects.");
				return false;
			}
			const TSharedPtr<FJsonObject> Transform = Value->AsObject();
			const TSharedPtr<FJsonValue> Translation =
				Transform->TryGetField(TEXT("translation"));
			const TSharedPtr<FJsonValue> Scale =
				Transform->TryGetField(TEXT("scale"));
			const TSharedPtr<FJsonValue> Shear =
				Transform->TryGetField(TEXT("shear"));
			const TSharedPtr<FJsonValue> Angle =
				Transform->TryGetField(TEXT("angle"));
			if (!IsFiniteNumberArrayValue(Translation, 2)
				|| !IsFiniteNumberArrayValue(Scale, 2)
				|| !IsFiniteNumberArrayValue(Shear, 2)
				|| !IsFiniteNumberValue(Angle))
			{
				OutError =
					TEXT("2D transform values require finite translation[2], scale[2], shear[2], and angle.");
				return false;
			}
		}
		else
		{
			OutError = TEXT("Unsupported animation valueType.");
			return false;
		}
	}
	return true;
}

class FTool_GetWidgetAnimation final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.animation.get");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		FString AnimationFilter;
		Params->TryGetStringField(
			TEXT("animationName"),
			AnimationFilter);
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		if (!WidgetBlueprint)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint was not found."),
				TEXT("target_not_found"),
				404);
		}

		TArray<TSharedPtr<FJsonValue>> Animations;
		for (const UWidgetAnimation* Animation :
			WidgetBlueprint->Animations)
		{
			if (!Animation
				|| (!AnimationFilter.IsEmpty()
					&& !Animation->GetName().Equals(
						AnimationFilter,
						ESearchCase::IgnoreCase)
					&& !Animation->GetDisplayLabel().Equals(
						AnimationFilter,
						ESearchCase::IgnoreCase)))
			{
				continue;
			}
			const UMovieScene* MovieScene =
				Animation->GetMovieScene();
			TSharedPtr<FJsonObject> AnimationJson =
				MakeShared<FJsonObject>();
			AnimationJson->SetStringField(
				TEXT("name"),
				Animation->GetName());
			AnimationJson->SetStringField(
				TEXT("displayName"),
				Animation->GetDisplayLabel());
			AnimationJson->SetNumberField(
				TEXT("startTime"),
				Animation->GetStartTime());
			AnimationJson->SetNumberField(
				TEXT("endTime"),
				Animation->GetEndTime());
			AnimationJson->SetNumberField(
				TEXT("displayRate"),
				MovieScene
					? MovieScene->GetDisplayRate().AsDecimal()
					: 0.0);

			TArray<TSharedPtr<FJsonValue>> Bindings;
			if (MovieScene)
			{
				for (const FMovieSceneBinding& Binding :
					MovieScene->GetBindings())
				{
					TSharedPtr<FJsonObject> BindingJson =
						MakeShared<FJsonObject>();
					BindingJson->SetStringField(
						TEXT("id"),
						Binding.GetObjectGuid().ToString(
							EGuidFormats::Digits));
					BindingJson->SetStringField(
						TEXT("name"),
						Binding.GetName());
					TArray<TSharedPtr<FJsonValue>> Tracks;
					for (const UMovieSceneTrack* Track :
						Binding.GetTracks())
					{
						if (!Track)
						{
							continue;
						}
						TSharedPtr<FJsonObject> TrackJson =
							MakeShared<FJsonObject>();
						TrackJson->SetStringField(
							TEXT("class"),
							Track->GetClass()->GetName());
						const UMovieScenePropertyTrack* PropertyTrack =
							Cast<UMovieScenePropertyTrack>(Track);
						TrackJson->SetStringField(
							TEXT("property"),
							PropertyTrack
								? PropertyTrack
									->GetPropertyName()
									.ToString()
								: FString());
						TrackJson->SetNumberField(
							TEXT("sectionCount"),
							Track->GetAllSections().Num());
						Tracks.Add(
							MakeShared<FJsonValueObject>(
								TrackJson));
					}
					BindingJson->SetArrayField(TEXT("tracks"), Tracks);
					Bindings.Add(
						MakeShared<FJsonValueObject>(BindingJson));
				}
			}
			AnimationJson->SetArrayField(TEXT("bindings"), Bindings);
			Animations.Add(
				MakeShared<FJsonValueObject>(AnimationJson));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("widgetBp"), WidgetBlueprintPath);
		Result->SetNumberField(TEXT("count"), Animations.Num());
		Result->SetArrayField(TEXT("animations"), Animations);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_CreateWidgetAnimation final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.animation.create");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString AnimationName =
			Params->GetStringField(TEXT("animationName"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		if (!WidgetBlueprint)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint was not found."),
				TEXT("target_not_found"),
				404);
		}
		if (AnimationName.IsEmpty()
			|| FindWidgetAnimation(WidgetBlueprint, AnimationName))
		{
			return FMCPToolResult::Error(
				TEXT("animationName is empty or already in use."),
				TEXT("invalid_params"),
				422);
		}

		double StartTime = 0.0;
		double EndTime = 5.0;
		double DisplayRate = 20.0;
		Params->TryGetNumberField(TEXT("startTime"), StartTime);
		Params->TryGetNumberField(TEXT("endTime"), EndTime);
		Params->TryGetNumberField(TEXT("displayRate"), DisplayRate);
		if (StartTime < 0.0
			|| EndTime <= StartTime
			|| DisplayRate <= 0.0)
		{
			return FMCPToolResult::Error(
				TEXT("Animation time range or display rate is invalid."),
				TEXT("invalid_params"),
				422);
		}

		WidgetBlueprint->Modify();
		UWidgetAnimation* Animation =
			NewObject<UWidgetAnimation>(
				WidgetBlueprint,
				FName(*AnimationName),
				RF_Transactional);
		Animation->SetDisplayLabel(AnimationName);
		Animation->MovieScene = NewObject<UMovieScene>(
			Animation,
			FName(*AnimationName),
			RF_Transactional);
		Animation->MovieScene->SetDisplayRate(
			FFrameRate(
				FMath::Max(1, FMath::RoundToInt(DisplayRate)),
				1));
		const FFrameRate TickResolution =
			Animation->MovieScene->GetTickResolution();
		const FFrameNumber StartFrame =
			(StartTime * TickResolution).RoundToFrame();
		const FFrameNumber EndFrame =
			(EndTime * TickResolution).RoundToFrame();
		Animation->MovieScene->SetPlaybackRange(
			TRange<FFrameNumber>(StartFrame, EndFrame + 1));
		Animation->MovieScene->GetEditorData().WorkStart = StartTime;
		Animation->MovieScene->GetEditorData().WorkEnd = EndTime;
		WidgetBlueprint->Animations.Add(Animation);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("animation"), AnimationName);
		Result->SetNumberField(TEXT("startTime"), StartTime);
		Result->SetNumberField(TEXT("endTime"), EndTime);
		Result->SetNumberField(TEXT("displayRate"), DisplayRate);
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_SetWidgetAnimationTrack final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.animation.track.set");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString AnimationName =
			Params->GetStringField(TEXT("animationName"));
		const FString WidgetName =
			Params->GetStringField(TEXT("widgetName"));
		const FString PropertyName =
			Params->GetStringField(TEXT("property"));
		const FString ValueType =
			Params->GetStringField(TEXT("valueType"));
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!Params->TryGetArrayField(TEXT("keys"), Keys)
			|| !Keys
			|| Keys->IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("keys must contain at least one animation key."),
				TEXT("invalid_params"),
				422);
		}

		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidgetAnimation* Animation =
			FindWidgetAnimation(WidgetBlueprint, AnimationName);
		UWidget* Widget =
			FindWidgetAdvanced(WidgetBlueprint, WidgetName);
		if (!WidgetBlueprint || !Animation || !Widget)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint, animation, or widget was not found."),
				TEXT("target_not_found"),
				404);
		}
		FProperty* Property =
			FindFProperty<FProperty>(Widget->GetClass(), FName(*PropertyName));
		if (!Property)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Property '%s' was not found on widget class '%s'."),
					*PropertyName,
					*Widget->GetClass()->GetName()),
				TEXT("target_not_found"),
				404);
		}
		if (!IsWidgetAnimationPropertyCompatible(
				Property,
				ValueType))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("valueType '%s' is incompatible with property '%s' of type '%s'."),
					*ValueType,
					*PropertyName,
					*Property->GetCPPType()),
				TEXT("unsupported_property_type"),
				422);
		}
		FString KeyValidationError;
		if (!ValidateWidgetAnimationKeys(
				ValueType,
				*Keys,
				KeyValidationError))
		{
			return FMCPToolResult::Error(
				KeyValidationError,
				TEXT("invalid_params"),
				422);
		}

		Animation->Modify();
		UMovieScene* MovieScene = Animation->GetMovieScene();
		MovieScene->Modify();
		const FGuid BindingGuid = FindOrAddAnimationBinding(
			Animation,
			Widget,
			WidgetBlueprint->WidgetTree
				? WidgetBlueprint->WidgetTree->RootWidget
				: nullptr);
		if (!BindingGuid.IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("Could not create the widget animation binding."));
		}
		RemoveExistingPropertyTracks(
			MovieScene,
			BindingGuid,
			FName(*PropertyName));

		const FFrameRate TickResolution =
			MovieScene->GetTickResolution();
		auto ReadFrame =
			[TickResolution](const double Time)
			{
				return (Time * TickResolution).RoundToFrame();
			};
		UMovieSceneTrack* CreatedTrack = nullptr;

		if (ValueType == TEXT("float"))
		{
			UMovieSceneFloatTrack* Track =
				MovieScene->AddTrack<UMovieSceneFloatTrack>(BindingGuid);
			UMovieSceneFloatSection* Section = Track
				? Cast<UMovieSceneFloatSection>(
					Track->CreateNewSection())
				: nullptr;
			if (!Track || !Section)
			{
				return FMCPToolResult::Error(
					TEXT("Could not create a float animation track."));
			}
			Track->SetPropertyNameAndPath(
				FName(*PropertyName),
				PropertyName);
			Track->AddSection(*Section);
			Section->SetRange(TRange<FFrameNumber>::All());
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				double Time = 0.0;
				TSharedPtr<FJsonValue> Value;
				if (!ReadAnimationKeyObject(KeyValue, Time, Value)
					|| Value->Type != EJson::Number)
				{
					return FMCPToolResult::Error(
						TEXT("Float keys require numeric time and value."),
						TEXT("invalid_params"),
						422);
				}
				Section->GetChannel().AddCubicKey(
					ReadFrame(Time),
					Value->AsNumber());
			}
			CreatedTrack = Track;
		}
		else if (ValueType == TEXT("bool"))
		{
			UMovieSceneBoolTrack* Track =
				MovieScene->AddTrack<UMovieSceneBoolTrack>(BindingGuid);
			UMovieSceneBoolSection* Section = Track
				? Cast<UMovieSceneBoolSection>(
					Track->CreateNewSection())
				: nullptr;
			if (!Track || !Section)
			{
				return FMCPToolResult::Error(
					TEXT("Could not create a bool animation track."));
			}
			Track->SetPropertyNameAndPath(
				FName(*PropertyName),
				PropertyName);
			Track->AddSection(*Section);
			Section->SetRange(TRange<FFrameNumber>::All());
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				double Time = 0.0;
				TSharedPtr<FJsonValue> Value;
				if (!ReadAnimationKeyObject(KeyValue, Time, Value)
					|| Value->Type != EJson::Boolean)
				{
					return FMCPToolResult::Error(
						TEXT("Bool keys require numeric time and boolean value."),
						TEXT("invalid_params"),
						422);
				}
				Section->GetChannel().GetData().AddKey(
					ReadFrame(Time),
					Value->AsBool());
			}
			CreatedTrack = Track;
		}
		else if (ValueType == TEXT("enum"))
		{
			UMovieSceneByteTrack* Track =
				MovieScene->AddTrack<UMovieSceneByteTrack>(BindingGuid);
			UMovieSceneByteSection* Section = Track
				? Cast<UMovieSceneByteSection>(
					Track->CreateNewSection())
				: nullptr;
			if (!Track || !Section)
			{
				return FMCPToolResult::Error(
					TEXT("Could not create an enum animation track."));
			}
			UEnum* Enum = nullptr;
			if (const FEnumProperty* EnumProperty =
				CastField<FEnumProperty>(Property))
			{
				Enum = EnumProperty->GetEnum();
			}
			else if (const FByteProperty* ByteProperty =
				CastField<FByteProperty>(Property))
			{
				Enum = ByteProperty->Enum;
			}
			Track->SetEnum(Enum);
			Track->SetPropertyNameAndPath(
				FName(*PropertyName),
				PropertyName);
			Track->AddSection(*Section);
			Section->SetRange(TRange<FFrameNumber>::All());
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				double Time = 0.0;
				TSharedPtr<FJsonValue> Value;
				if (!ReadAnimationKeyObject(KeyValue, Time, Value)
					|| Value->Type != EJson::Number)
				{
					return FMCPToolResult::Error(
						TEXT("Enum keys require numeric time and integer value."),
						TEXT("invalid_params"),
						422);
				}
				Section->ByteCurve.GetData().AddKey(
					ReadFrame(Time),
					static_cast<uint8>(Value->AsNumber()));
			}
			CreatedTrack = Track;
		}
		else if (ValueType == TEXT("vector2"))
		{
			UMovieSceneDoubleVectorTrack* Track =
				MovieScene->AddTrack<UMovieSceneDoubleVectorTrack>(
					BindingGuid);
			if (Track)
			{
				Track->SetNumChannelsUsed(2);
			}
			UMovieSceneDoubleVectorSection* Section = Track
				? Cast<UMovieSceneDoubleVectorSection>(
					Track->CreateNewSection())
				: nullptr;
			if (!Track || !Section)
			{
				return FMCPToolResult::Error(
					TEXT("Could not create a Vector2D animation track."));
			}
			Section->SetChannelsUsed(2);
			Track->SetPropertyNameAndPath(
				FName(*PropertyName),
				PropertyName);
			Track->AddSection(*Section);
			Section->SetRange(TRange<FFrameNumber>::All());
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				double Time = 0.0;
				TSharedPtr<FJsonValue> Value;
				if (!ReadAnimationKeyObject(KeyValue, Time, Value)
					|| Value->Type != EJson::Array)
				{
					return FMCPToolResult::Error(
						TEXT("Vector2D keys require numeric time and [x,y] value."),
						TEXT("invalid_params"),
						422);
				}
				const TArray<TSharedPtr<FJsonValue>>& Components =
					Value->AsArray();
				if (Components.Num() != 2
					|| Components[0]->Type != EJson::Number
					|| Components[1]->Type != EJson::Number)
				{
					return FMCPToolResult::Error(
						TEXT("Vector2D value must contain two numbers."),
						TEXT("invalid_params"),
						422);
				}
				const FFrameNumber Frame = ReadFrame(Time);
				const_cast<FMovieSceneDoubleChannel&>(
					Section->GetChannel(0))
					.AddCubicKey(Frame, Components[0]->AsNumber());
				const_cast<FMovieSceneDoubleChannel&>(
					Section->GetChannel(1))
					.AddCubicKey(Frame, Components[1]->AsNumber());
			}
			CreatedTrack = Track;
		}
		else if (ValueType == TEXT("color"))
		{
			UMovieSceneColorTrack* Track =
				MovieScene->AddTrack<UMovieSceneColorTrack>(BindingGuid);
			UMovieSceneColorSection* Section = Track
				? Cast<UMovieSceneColorSection>(
					Track->CreateNewSection())
				: nullptr;
			if (!Track || !Section)
			{
				return FMCPToolResult::Error(
					TEXT("Could not create a color animation track."));
			}
			Track->SetPropertyNameAndPath(
				FName(*PropertyName),
				PropertyName);
			Track->AddSection(*Section);
			Section->SetRange(TRange<FFrameNumber>::All());
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				double Time = 0.0;
				TSharedPtr<FJsonValue> Value;
				if (!ReadAnimationKeyObject(KeyValue, Time, Value)
					|| Value->Type != EJson::Array)
				{
					return FMCPToolResult::Error(
						TEXT("Color keys require numeric time and [r,g,b,a] value."),
						TEXT("invalid_params"),
						422);
				}
				const TArray<TSharedPtr<FJsonValue>>& Components =
					Value->AsArray();
				if (Components.Num() != 4
					|| Components.ContainsByPredicate(
						[](const TSharedPtr<FJsonValue>& Component)
						{
							return !Component.IsValid()
								|| Component->Type != EJson::Number;
						}))
				{
					return FMCPToolResult::Error(
						TEXT("Color value must contain four numbers."),
						TEXT("invalid_params"),
						422);
				}
				const FFrameNumber Frame = ReadFrame(Time);
				Section->GetRedChannel().AddCubicKey(
					Frame,
					Components[0]->AsNumber());
				Section->GetGreenChannel().AddCubicKey(
					Frame,
					Components[1]->AsNumber());
				Section->GetBlueChannel().AddCubicKey(
					Frame,
					Components[2]->AsNumber());
				Section->GetAlphaChannel().AddCubicKey(
					Frame,
					Components[3]->AsNumber());
			}
			CreatedTrack = Track;
		}
		else if (ValueType == TEXT("transform2d"))
		{
			UMovieScene2DTransformTrack* Track =
				MovieScene->AddTrack<UMovieScene2DTransformTrack>(
					BindingGuid);
			UMovieScene2DTransformSection* Section = Track
				? Cast<UMovieScene2DTransformSection>(
					Track->CreateNewSection())
				: nullptr;
			if (!Track || !Section)
			{
				return FMCPToolResult::Error(
					TEXT("Could not create a 2D transform animation track."));
			}
			Track->SetPropertyNameAndPath(
				FName(*PropertyName),
				PropertyName);
			Track->AddSection(*Section);
			Section->SetRange(TRange<FFrameNumber>::All());
			Section->SetMask(FMovieScene2DTransformMask(
				EMovieScene2DTransformChannel::AllTransform));
			for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
			{
				double Time = 0.0;
				TSharedPtr<FJsonValue> Value;
				if (!ReadAnimationKeyObject(KeyValue, Time, Value)
					|| Value->Type != EJson::Object)
				{
					return FMCPToolResult::Error(
						TEXT("2D transform keys require an object value."),
						TEXT("invalid_params"),
						422);
				}
				const TSharedPtr<FJsonObject> Transform = Value->AsObject();
				TArray<double> Translation;
				TArray<double> Scale;
				TArray<double> Shear;
				double Angle = 0.0;
				if (!TryReadNumberArrayAdvanced(
						Transform,
						TEXT("translation"),
						2,
						Translation)
					|| !TryReadNumberArrayAdvanced(
						Transform,
						TEXT("scale"),
						2,
						Scale)
					|| !TryReadNumberArrayAdvanced(
						Transform,
						TEXT("shear"),
						2,
						Shear)
					|| !Transform->TryGetNumberField(
						TEXT("angle"),
						Angle))
				{
					return FMCPToolResult::Error(
						TEXT("2D transform value requires translation[2], scale[2], shear[2], and angle."),
						TEXT("invalid_params"),
						422);
				}
				const FFrameNumber Frame = ReadFrame(Time);
				Section->Translation[0].AddCubicKey(
					Frame,
					Translation[0]);
				Section->Translation[1].AddCubicKey(
					Frame,
					Translation[1]);
				Section->Scale[0].AddCubicKey(Frame, Scale[0]);
				Section->Scale[1].AddCubicKey(Frame, Scale[1]);
				Section->Shear[0].AddCubicKey(Frame, Shear[0]);
				Section->Shear[1].AddCubicKey(Frame, Shear[1]);
				Section->Rotation.AddCubicKey(Frame, Angle);
			}
			CreatedTrack = Track;
		}
		else
		{
			return FMCPToolResult::Error(
				TEXT("Unsupported animation valueType."),
				TEXT("invalid_params"),
				422);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("animation"), AnimationName);
		Result->SetStringField(TEXT("widget"), WidgetName);
		Result->SetStringField(TEXT("property"), PropertyName);
		Result->SetStringField(TEXT("valueType"), ValueType);
		Result->SetNumberField(TEXT("keyCount"), Keys->Num());
		Result->SetStringField(
			TEXT("trackClass"),
			CreatedTrack ? CreatedTrack->GetClass()->GetName() : FString());
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			true,
			true,
			Result);
	}
};

class FTool_DeleteWidgetAnimation final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.animation.delete");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		const FString AnimationName =
			Params->GetStringField(TEXT("animationName"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UWidgetAnimation* Animation =
			FindWidgetAnimation(WidgetBlueprint, AnimationName);
		if (!WidgetBlueprint || !Animation)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint or animation was not found."),
				TEXT("target_not_found"),
				404);
		}

		WidgetBlueprint->Modify();
		Animation->Modify();
		const int32 Removed =
			WidgetBlueprint->Animations.Remove(Animation);
		Animation->Rename(
			nullptr,
			GetTransientPackage(),
			REN_DontCreateRedirectors | REN_ForceNoResetLoaders);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("animation"), AnimationName);
		Result->SetBoolField(TEXT("deleted"), Removed > 0);
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			Removed > 0,
			true,
			Result);
	}
};

FString DesignPreviewSizeModeToString(
	const EDesignPreviewSizeMode Mode)
{
	switch (Mode)
	{
	case EDesignPreviewSizeMode::FillScreen:
		return TEXT("FillScreen");
	case EDesignPreviewSizeMode::Custom:
		return TEXT("Custom");
	case EDesignPreviewSizeMode::CustomOnScreen:
		return TEXT("CustomOnScreen");
	case EDesignPreviewSizeMode::Desired:
		return TEXT("Desired");
	case EDesignPreviewSizeMode::DesiredOnScreen:
		return TEXT("DesiredOnScreen");
	default:
		return TEXT("FillScreen");
	}
}

bool TryParseDesignPreviewSizeMode(
	const FString& Value,
	EDesignPreviewSizeMode& OutMode)
{
	if (Value.Equals(TEXT("FillScreen"), ESearchCase::IgnoreCase))
	{
		OutMode = EDesignPreviewSizeMode::FillScreen;
		return true;
	}
	if (Value.Equals(TEXT("Custom"), ESearchCase::IgnoreCase))
	{
		OutMode = EDesignPreviewSizeMode::Custom;
		return true;
	}
	if (Value.Equals(TEXT("CustomOnScreen"), ESearchCase::IgnoreCase))
	{
		OutMode = EDesignPreviewSizeMode::CustomOnScreen;
		return true;
	}
	if (Value.Equals(TEXT("Desired"), ESearchCase::IgnoreCase))
	{
		OutMode = EDesignPreviewSizeMode::Desired;
		return true;
	}
	if (Value.Equals(TEXT("DesiredOnScreen"), ESearchCase::IgnoreCase))
	{
		OutMode = EDesignPreviewSizeMode::DesiredOnScreen;
		return true;
	}
	return false;
}

UUserWidget* GetWidgetBlueprintDefaultWidget(
	const UWidgetBlueprint* WidgetBlueprint)
{
	return WidgetBlueprint && WidgetBlueprint->GeneratedClass
		? Cast<UUserWidget>(
			WidgetBlueprint->GeneratedClass->GetDefaultObject())
		: nullptr;
}

FVector2D ReadPreviewSize(
	const TSharedPtr<FJsonObject>& Params,
	const UUserWidget* DefaultWidget)
{
	TArray<double> PreviewSize;
	if (TryReadNumberArrayAdvanced(
			Params,
			TEXT("previewSize"),
			2,
			PreviewSize)
		&& PreviewSize[0] > 0.0
		&& PreviewSize[1] > 0.0)
	{
		return FVector2D(PreviewSize[0], PreviewSize[1]);
	}
	if (DefaultWidget
		&& DefaultWidget->DesignTimeSize.X > 0.0
		&& DefaultWidget->DesignTimeSize.Y > 0.0)
	{
		return DefaultWidget->DesignTimeSize;
	}
	return FVector2D(1920.0, 1080.0);
}

TSharedPtr<FJsonObject> MakeDesignerSettingsResult(
	const UWidgetBlueprint* WidgetBlueprint,
	const TSharedPtr<FJsonObject>& Params)
{
	UUserWidget* DefaultWidget =
		GetWidgetBlueprintDefaultWidget(WidgetBlueprint);
	if (!DefaultWidget)
	{
		return nullptr;
	}

	const FVector2D PreviewSize =
		ReadPreviewSize(Params, DefaultWidget);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("widgetBlueprint"),
		WidgetBlueprint->GetPathName());
	Result->SetStringField(
		TEXT("designSizeMode"),
		DesignPreviewSizeModeToString(DefaultWidget->DesignSizeMode));
	Result->SetArrayField(
		TEXT("designTimeSize"),
		{
			MakeShared<FJsonValueNumber>(DefaultWidget->DesignTimeSize.X),
			MakeShared<FJsonValueNumber>(DefaultWidget->DesignTimeSize.Y)
		});
	Result->SetArrayField(
		TEXT("previewSize"),
		{
			MakeShared<FJsonValueNumber>(PreviewSize.X),
			MakeShared<FJsonValueNumber>(PreviewSize.Y)
		});
	Result->SetNumberField(
		TEXT("previewDpi"),
		FWidgetBlueprintEditorUtils::GetWidgetPreviewDPIScale(
			DefaultWidget,
			PreviewSize));
	return Result;
}

class FTool_GetWidgetDesignerSettings final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.designer.settings.get");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		if (!WidgetBlueprint)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint was not found."),
				TEXT("target_not_found"),
				404);
		}

		TSharedPtr<FJsonObject> Result =
			MakeDesignerSettingsResult(WidgetBlueprint, Params);
		if (!Result.IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint has no generated default widget. Compile it first."),
				TEXT("asset_compile_required"),
				422);
		}
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetWidgetDesignerSettings final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.widget.designer.settings.set");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		const FString WidgetBlueprintPath =
			Params->GetStringField(TEXT("widgetBp"));
		UWidgetBlueprint* WidgetBlueprint =
			LoadWidgetBlueprintAdvanced(WidgetBlueprintPath);
		UUserWidget* DefaultWidget =
			GetWidgetBlueprintDefaultWidget(WidgetBlueprint);
		if (!WidgetBlueprint || !DefaultWidget)
		{
			return FMCPToolResult::Error(
				TEXT("Widget Blueprint or its generated default widget was not found."),
				TEXT("target_not_found"),
				404);
		}

		const bool bHasMode =
			Params->HasField(TEXT("designSizeMode"));
		const bool bHasDesignTimeSize =
			Params->HasField(TEXT("designTimeSize"));
		const bool bHasPreviewSize =
			Params->HasField(TEXT("previewSize"));
		if (!bHasMode && !bHasDesignTimeSize && !bHasPreviewSize)
		{
			return FMCPToolResult::Error(
				TEXT("At least one designer setting or previewSize must be supplied."),
				TEXT("invalid_params"),
				422);
		}

		TOptional<EDesignPreviewSizeMode> ParsedMode;
		FString ModeValue;
		if (bHasMode
			&& Params->TryGetStringField(
				TEXT("designSizeMode"),
				ModeValue))
		{
			EDesignPreviewSizeMode NewMode;
			if (!TryParseDesignPreviewSizeMode(ModeValue, NewMode))
			{
				return FMCPToolResult::Error(
					TEXT("designSizeMode is invalid."),
					TEXT("invalid_params"),
					422);
			}
			ParsedMode = NewMode;
		}

		TArray<double> DesignTimeSize;
		TOptional<FVector2D> ParsedDesignTimeSize;
		if (bHasDesignTimeSize)
		{
			if (!TryReadNumberArrayAdvanced(
					Params,
					TEXT("designTimeSize"),
					2,
					DesignTimeSize)
				|| DesignTimeSize[0] <= 0.0
				|| DesignTimeSize[1] <= 0.0)
			{
				return FMCPToolResult::Error(
					TEXT("designTimeSize must contain two positive numbers."),
					TEXT("invalid_params"),
					422);
			}
			ParsedDesignTimeSize = FVector2D(
				DesignTimeSize[0],
				DesignTimeSize[1]);
		}

		if (bHasPreviewSize)
		{
			TArray<double> PreviewSize;
			if (!TryReadNumberArrayAdvanced(
					Params,
					TEXT("previewSize"),
					2,
					PreviewSize)
				|| PreviewSize[0] <= 0.0
				|| PreviewSize[1] <= 0.0)
			{
				return FMCPToolResult::Error(
					TEXT("previewSize must contain two positive numbers."),
					TEXT("invalid_params"),
					422);
			}
		}

		bool bChanged = false;
		if (ParsedMode.IsSet()
			&& DefaultWidget->DesignSizeMode != ParsedMode.GetValue())
		{
			DefaultWidget->Modify();
			DefaultWidget->DesignSizeMode = ParsedMode.GetValue();
			bChanged = true;
		}
		if (ParsedDesignTimeSize.IsSet()
			&& !DefaultWidget->DesignTimeSize.Equals(
				ParsedDesignTimeSize.GetValue()))
		{
			DefaultWidget->Modify();
			DefaultWidget->DesignTimeSize =
				ParsedDesignTimeSize.GetValue();
			bChanged = true;
		}

		if (bChanged)
		{
			WidgetBlueprint->Modify();
			FBlueprintEditorUtils::MarkBlueprintAsModified(
				WidgetBlueprint);
		}
		TSharedPtr<FJsonObject> Result =
			MakeDesignerSettingsResult(WidgetBlueprint, Params);
		if (!Result.IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("Could not read back designer settings."),
				TEXT("verification_failed"),
				500);
		}
		return FinishWidgetMutation(
			WidgetBlueprint,
			Params,
			bChanged,
			false,
			Result);
	}
};
} // namespace

namespace UEAIIntegrationTools
{
void RegisterAdvancedUITools(FMCPToolRegistry& Registry)
{
	Registry.Register(
		MakeShared<FTool_ListWidgetBindings>());
	Registry.Register(
		MakeShared<FTool_UnbindWidgetEvent>());
	Registry.Register(
		MakeShared<FTool_RenameWidgetChild>());
	Registry.Register(
		MakeShared<FTool_CopyWidgetChild>());
	Registry.Register(
		MakeShared<FTool_ReparentWidgetChild>());
	Registry.Register(
		MakeShared<FTool_SetNamedSlotContent>());
	Registry.Register(
		MakeShared<FTool_SetWidgetRoot>());
	Registry.Register(
		MakeShared<FTool_SetWidgetSlotProperties>());
	Registry.Register(
		MakeShared<FTool_GetWidgetAnimation>());
	Registry.Register(
		MakeShared<FTool_CreateWidgetAnimation>());
	Registry.Register(
		MakeShared<FTool_SetWidgetAnimationTrack>());
	Registry.Register(
		MakeShared<FTool_DeleteWidgetAnimation>());
	Registry.Register(
		MakeShared<FTool_GetWidgetDesignerSettings>());
	Registry.Register(
		MakeShared<FTool_SetWidgetDesignerSettings>());
}
} // namespace UEAIIntegrationTools
