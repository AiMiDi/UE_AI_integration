#include "Domains/Content/Command/WidgetEventBindingSupport.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/ComponentDelegateBinding.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "WidgetBlueprint.h"

namespace UEAIIntegration::WidgetEventBindings
{
namespace
{
constexpr TCHAR BindingMarkerPrefix[] =
	TEXT("UE_AI_integration:event-binding:");

UWidget* FindWidget(UWidgetBlueprint* WidgetBlueprint, const FString& Name)
{
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || Name.IsEmpty())
	{
		return nullptr;
	}

	UWidget* FoundWidget = nullptr;
	WidgetBlueprint->WidgetTree->ForEachWidget(
		[&FoundWidget, &Name](UWidget* Widget)
		{
			if (!FoundWidget && Widget && Widget->GetName() == Name)
			{
				FoundWidget = Widget;
			}
		});
	return FoundWidget;
}

bool IsFunctionDeclared(
	const UWidgetBlueprint* WidgetBlueprint,
	const FName FunctionName)
{
	if (!WidgetBlueprint || FunctionName.IsNone())
	{
		return false;
	}
	if ((WidgetBlueprint->GeneratedClass
			&& WidgetBlueprint->GeneratedClass->FindFunctionByName(FunctionName))
		|| (WidgetBlueprint->SkeletonGeneratedClass
			&& WidgetBlueprint->SkeletonGeneratedClass->FindFunctionByName(
				FunctionName)))
	{
		return true;
	}

	for (const UEdGraph* FunctionGraph : WidgetBlueprint->FunctionGraphs)
	{
		if (FunctionGraph && FunctionGraph->GetFName() == FunctionName)
		{
			return true;
		}
	}

	TArray<UK2Node_CustomEvent*> CustomEvents;
	FBlueprintEditorUtils::GetAllNodesOfClass(
		const_cast<UWidgetBlueprint*>(WidgetBlueprint),
		CustomEvents);
	return CustomEvents.ContainsByPredicate(
		[FunctionName](const UK2Node_CustomEvent* Event)
		{
			return Event && Event->CustomFunctionName == FunctionName;
		});
}

UFunction* FindCompiledFunction(
	UWidgetBlueprint* WidgetBlueprint,
	const FName FunctionName)
{
	if (!WidgetBlueprint || FunctionName.IsNone())
	{
		return nullptr;
	}
	if (WidgetBlueprint->GeneratedClass)
	{
		if (UFunction* Function =
			WidgetBlueprint->GeneratedClass->FindFunctionByName(FunctionName))
		{
			return Function;
		}
	}
	return WidgetBlueprint->SkeletonGeneratedClass
		? WidgetBlueprint->SkeletonGeneratedClass->FindFunctionByName(
			FunctionName)
		: nullptr;
}

FObjectProperty* FindWidgetVariableProperty(
	UWidgetBlueprint* WidgetBlueprint,
	const FName WidgetName)
{
	UClass* BlueprintClass = WidgetBlueprint
		? WidgetBlueprint->SkeletonGeneratedClass
		: nullptr;
	if (!BlueprintClass && WidgetBlueprint)
	{
		BlueprintClass = WidgetBlueprint->GeneratedClass;
	}
	return BlueprintClass
		? FindFProperty<FObjectProperty>(BlueprintClass, WidgetName)
		: nullptr;
}

UK2Node_ComponentBoundEvent* FindEventNode(
	UWidgetBlueprint* WidgetBlueprint,
	const FName EventName,
	const FName WidgetName)
{
	return WidgetBlueprint
		? const_cast<UK2Node_ComponentBoundEvent*>(
			FKismetEditorUtilities::FindBoundEventForComponent(
				WidgetBlueprint,
				EventName,
				WidgetName))
		: nullptr;
}

UK2Node_CallFunction* FindLinkedCall(
	const UK2Node_ComponentBoundEvent* EventNode,
	const FName FunctionName)
{
	if (!EventNode)
	{
		return nullptr;
	}
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	const UEdGraphPin* EventExecPin =
		K2Schema->FindExecutionPin(*EventNode, EGPD_Output);
	if (!EventExecPin)
	{
		return nullptr;
	}

	for (const UEdGraphPin* LinkedPin : EventExecPin->LinkedTo)
	{
		UK2Node_CallFunction* CallNode = LinkedPin
			? Cast<UK2Node_CallFunction>(LinkedPin->GetOwningNode())
			: nullptr;
		if (CallNode
			&& CallNode->FunctionReference.GetMemberName() == FunctionName)
		{
			return CallNode;
		}
	}
	return nullptr;
}

bool CreateAndConnectCall(
	UWidgetBlueprint* WidgetBlueprint,
	UK2Node_ComponentBoundEvent* EventNode,
	UFunction* Function,
	UK2Node_CallFunction*& OutCallNode,
	FString& OutError)
{
	OutCallNode = nullptr;
	if (!WidgetBlueprint || !EventNode || !Function)
	{
		OutError = TEXT("Cannot create an event call node without a compiled function.");
		return false;
	}

	UEdGraph* EventGraph = EventNode->GetGraph();
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	UEdGraphPin* EventExecPin =
		K2Schema->FindExecutionPin(*EventNode, EGPD_Output);
	if (!EventGraph || !EventExecPin)
	{
		OutError = TEXT("The component-bound event has no execution output.");
		return false;
	}

	UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(
		EventGraph,
		NAME_None,
		RF_Transactional);
	CallNode->SetFromFunction(Function);
	CallNode->NodePosX = EventNode->NodePosX + 320;
	CallNode->NodePosY = EventNode->NodePosY;
	EventGraph->AddNode(CallNode, false, false);
	CallNode->CreateNewGuid();
	CallNode->PostPlacedNewNode();
	CallNode->AllocateDefaultPins();

	UEdGraphPin* CallExecPin =
		K2Schema->FindExecutionPin(*CallNode, EGPD_Input);
	if (!CallExecPin
		|| !K2Schema->TryCreateConnection(EventExecPin, CallExecPin))
	{
		EventGraph->RemoveNode(CallNode);
		OutError = TEXT("Failed to connect the widget event to its handler function.");
		return false;
	}

	for (UEdGraphPin* EventPin : EventNode->Pins)
	{
		if (!EventPin
			|| EventPin == EventExecPin
			|| EventPin->Direction != EGPD_Output)
		{
			continue;
		}
		if (UEdGraphPin* FunctionPin =
			CallNode->FindPin(EventPin->PinName, EGPD_Input))
		{
			K2Schema->TryCreateConnection(EventPin, FunctionPin);
		}
	}

	OutCallNode = CallNode;
	return true;
}

void SetOwnedHandlerFunction(
	UK2Node_ComponentBoundEvent* EventNode,
	const FName FunctionName)
{
	if (!EventNode)
	{
		return;
	}
	EventNode->Modify();
	EventNode->NodeComment =
		BindingMarkerPrefix + FunctionName.ToString();
	EventNode->bCommentBubbleVisible = false;
}

bool HasGeneratedBinding(
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
		if (!ComponentBinding)
		{
			continue;
		}
		if (ComponentBinding->ComponentDelegateBindings.ContainsByPredicate(
			[EventNode](const FBlueprintComponentDelegateBinding& Binding)
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
}

bool TryGetOwnedHandlerFunction(
	const UK2Node_ComponentBoundEvent* EventNode,
	FName& OutFunctionName)
{
	OutFunctionName = NAME_None;
	if (!EventNode
		|| !EventNode->NodeComment.StartsWith(BindingMarkerPrefix))
	{
		return false;
	}
	const FString FunctionName =
		EventNode->NodeComment.RightChop(FCString::Strlen(BindingMarkerPrefix));
	if (FunctionName.IsEmpty())
	{
		return false;
	}
	OutFunctionName = FName(*FunctionName);
	return true;
}

bool Upsert(
	UWidgetBlueprint* WidgetBlueprint,
	const FString& WidgetName,
	const FString& EventName,
	const FString& FunctionName,
	const bool bAllowDeferredCompile,
	FUpsertResult& OutResult,
	FString& OutError,
	FString& OutErrorCode)
{
	OutResult = FUpsertResult();
	OutError.Reset();
	OutErrorCode.Reset();

	UWidget* TargetWidget = FindWidget(WidgetBlueprint, WidgetName);
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree || !TargetWidget)
	{
		OutError = FString::Printf(
			TEXT("Widget '%s' was not found in the Widget Blueprint."),
			*WidgetName);
		OutErrorCode = TEXT("target_not_found");
		return false;
	}

	FMulticastDelegateProperty* DelegateProperty =
		FindFProperty<FMulticastDelegateProperty>(
			TargetWidget->GetClass(),
			FName(*EventName));
	if (!DelegateProperty)
	{
		OutError = FString::Printf(
			TEXT("Event '%s' was not found on widget '%s' (class: %s)."),
			*EventName,
			*WidgetName,
			*TargetWidget->GetClass()->GetName());
		OutErrorCode = TEXT("target_not_found");
		return false;
	}

	const FName HandlerFunctionName(*FunctionName);
	if (!IsFunctionDeclared(WidgetBlueprint, HandlerFunctionName))
	{
		OutError = FString::Printf(
			TEXT("Function '%s' is not declared by Widget Blueprint '%s'."),
			*FunctionName,
			*WidgetBlueprint->GetPathName());
		OutErrorCode = TEXT("function_not_found");
		return false;
	}

	UFunction* HandlerFunction =
		FindCompiledFunction(WidgetBlueprint, HandlerFunctionName);
	if (!HandlerFunction && !bAllowDeferredCompile)
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		if (WidgetBlueprint->Status == BS_Error)
		{
			OutError = FString::Printf(
				TEXT("Widget Blueprint '%s' failed to compile before binding."),
				*WidgetBlueprint->GetPathName());
			OutErrorCode = TEXT("asset_compile_failed");
			return false;
		}
		HandlerFunction =
			FindCompiledFunction(WidgetBlueprint, HandlerFunctionName);
	}
	if (!HandlerFunction && !bAllowDeferredCompile)
	{
		OutError = FString::Printf(
			TEXT("Function '%s' was not found after compiling Widget Blueprint '%s'."),
			*FunctionName,
			*WidgetBlueprint->GetPathName());
		OutErrorCode = TEXT("function_not_found");
		return false;
	}

	FObjectProperty* WidgetProperty = FindWidgetVariableProperty(
		WidgetBlueprint,
		FName(*WidgetName));
	if (!WidgetProperty
		&& bAllowDeferredCompile
		&& TargetWidget->bIsVariable)
	{
		// Widget-tree edits intentionally defer a full compile in Workflow
		// mode. A component-bound event still needs the skeleton variable and
		// handler signature in order to allocate valid pins, so refresh only
		// the skeleton here and leave the single full compile to the finalizer.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
			WidgetBlueprint);
		WidgetProperty = FindWidgetVariableProperty(
			WidgetBlueprint,
			FName(*WidgetName));
		HandlerFunction =
			FindCompiledFunction(WidgetBlueprint, HandlerFunctionName);
	}
	if (!WidgetProperty && !bAllowDeferredCompile)
	{
		OutError = FString::Printf(
			TEXT("Widget '%s' is not exposed as a variable. Enable Is Variable before binding '%s'."),
			*WidgetName,
			*EventName);
		OutErrorCode = TEXT("invalid_target");
		return false;
	}
	if (!WidgetProperty)
	{
		OutError = FString::Printf(
			TEXT("Widget '%s' is not exposed as a Blueprint variable."),
			*WidgetName);
		OutErrorCode = TEXT("invalid_target");
		return false;
	}

	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WidgetBlueprint);
	if (!EventGraph)
	{
		OutError = TEXT("The Widget Blueprint has no EventGraph.");
		OutErrorCode = TEXT("invalid_target");
		return false;
	}

	WidgetBlueprint->Modify();
	TargetWidget->Modify();
	EventGraph->Modify();

	UK2Node_ComponentBoundEvent* EventNode = FindEventNode(
		WidgetBlueprint,
		FName(*EventName),
		FName(*WidgetName));
	if (!EventNode)
	{
		const FVector2D Position = EventGraph->GetGoodPlaceForNewNode();
		EventNode =
			FEdGraphSchemaAction_K2NewNode::SpawnNode<
				UK2Node_ComponentBoundEvent>(
				EventGraph,
				Position,
				EK2NewNodeFlags::None,
				[WidgetProperty, DelegateProperty](
					UK2Node_ComponentBoundEvent* NewNode)
				{
					NewNode->InitializeComponentBoundEventParams(
						WidgetProperty,
						DelegateProperty);
				});
		if (!EventNode)
		{
			OutError = TEXT("Failed to create a component-bound event node.");
			OutErrorCode = TEXT("execution_failed");
			return false;
		}
		OutResult.bChanged = true;
		OutResult.bCreatedEventNode = true;
	}

	SetOwnedHandlerFunction(EventNode, HandlerFunctionName);
	OutResult.EventNodeGuid = EventNode->NodeGuid;

	if (UK2Node_CallFunction* ExistingCall =
		FindLinkedCall(EventNode, HandlerFunctionName))
	{
		OutResult.CallNodeGuid = ExistingCall->NodeGuid;
		OutResult.bPendingCompile = !HandlerFunction;
		OutResult.bVerified = HandlerFunction != nullptr;
		return true;
	}

	if (!HandlerFunction)
	{
		OutResult.bPendingCompile = true;
		OutResult.bChanged = true;
		return true;
	}

	UK2Node_CallFunction* CallNode = nullptr;
	if (!CreateAndConnectCall(
			WidgetBlueprint,
			EventNode,
			HandlerFunction,
			CallNode,
			OutError))
	{
		OutErrorCode = TEXT("execution_failed");
		return false;
	}
	OutResult.bChanged = true;
	OutResult.bCreatedCallNode = true;
	OutResult.bVerified = true;
	OutResult.CallNodeGuid = CallNode->NodeGuid;
	return true;
}

bool FinalizeDeferred(
	UWidgetBlueprint* WidgetBlueprint,
	int32& OutRepairedCount,
	FString& OutError)
{
	OutRepairedCount = 0;
	OutError.Reset();
	if (!WidgetBlueprint)
	{
		OutError = TEXT("Widget Blueprint is null.");
		return false;
	}

	TArray<UK2Node_ComponentBoundEvent*> EventNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(WidgetBlueprint, EventNodes);

	bool bNeedsSkeletonRefresh = false;
	for (const UK2Node_ComponentBoundEvent* EventNode : EventNodes)
	{
		FName HandlerFunctionName;
		if (TryGetOwnedHandlerFunction(EventNode, HandlerFunctionName)
			&& !FindCompiledFunction(WidgetBlueprint, HandlerFunctionName))
		{
			bNeedsSkeletonRefresh = true;
			break;
		}
	}
	if (bNeedsSkeletonRefresh)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
			WidgetBlueprint);
	}

	for (UK2Node_ComponentBoundEvent* EventNode : EventNodes)
	{
		FName HandlerFunctionName;
		if (!TryGetOwnedHandlerFunction(EventNode, HandlerFunctionName)
			|| FindLinkedCall(EventNode, HandlerFunctionName))
		{
			continue;
		}

		UFunction* Function =
			FindCompiledFunction(WidgetBlueprint, HandlerFunctionName);
		if (!Function)
		{
			OutError = FString::Printf(
				TEXT("Deferred widget event handler '%s' was not generated."),
				*HandlerFunctionName.ToString());
			return false;
		}
		if (!FindWidgetVariableProperty(
			WidgetBlueprint,
			EventNode->ComponentPropertyName))
		{
			OutError = FString::Printf(
				TEXT("Deferred widget '%s' was not generated as a variable."),
				*EventNode->ComponentPropertyName.ToString());
			return false;
		}

		UK2Node_CallFunction* CallNode = nullptr;
		if (!CreateAndConnectCall(
			WidgetBlueprint,
			EventNode,
			Function,
			CallNode,
			OutError))
		{
			return false;
		}
		++OutRepairedCount;
	}

	return true;
}

bool ValidateCompiled(
	const UWidgetBlueprint* WidgetBlueprint,
	int32& OutVerifiedCount,
	TArray<FString>& OutErrors)
{
	OutVerifiedCount = 0;
	OutErrors.Reset();
	if (!WidgetBlueprint || !WidgetBlueprint->GeneratedClass)
	{
		OutErrors.Add(TEXT("Widget Blueprint has no generated class."));
		return false;
	}

	TArray<UK2Node_ComponentBoundEvent*> EventNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(
		const_cast<UWidgetBlueprint*>(WidgetBlueprint),
		EventNodes);
	for (const UK2Node_ComponentBoundEvent* EventNode : EventNodes)
	{
		FName HandlerFunctionName;
		if (!TryGetOwnedHandlerFunction(EventNode, HandlerFunctionName))
		{
			continue;
		}
		if (!WidgetBlueprint->GeneratedClass->FindFunctionByName(
			HandlerFunctionName))
		{
			OutErrors.Add(FString::Printf(
				TEXT("Generated handler function '%s' is missing."),
				*HandlerFunctionName.ToString()));
			continue;
		}
		if (!FindLinkedCall(EventNode, HandlerFunctionName))
		{
			OutErrors.Add(FString::Printf(
				TEXT("Event '%s.%s' is not connected to '%s'."),
				*EventNode->ComponentPropertyName.ToString(),
				*EventNode->DelegatePropertyName.ToString(),
				*HandlerFunctionName.ToString()));
			continue;
		}
		if (!HasGeneratedBinding(WidgetBlueprint, EventNode))
		{
			OutErrors.Add(FString::Printf(
				TEXT("Generated component delegate binding '%s.%s' is missing."),
				*EventNode->ComponentPropertyName.ToString(),
				*EventNode->DelegatePropertyName.ToString()));
			continue;
		}
		++OutVerifiedCount;
	}
	return OutErrors.IsEmpty();
}
}
