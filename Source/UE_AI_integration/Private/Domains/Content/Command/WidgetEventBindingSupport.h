#pragma once

#include "CoreMinimal.h"

class UK2Node_ComponentBoundEvent;
class UWidgetBlueprint;

namespace UEAIIntegration::WidgetEventBindings
{
struct FUpsertResult
{
	bool bChanged = false;
	bool bCreatedEventNode = false;
	bool bCreatedCallNode = false;
	bool bPendingCompile = false;
	bool bVerified = false;
	int32 RemovedDuplicateNodes = 0;
	FGuid EventNodeGuid;
	FGuid CallNodeGuid;
};

/**
 * Creates or repairs a graph-backed UMG multicast event binding.
 *
 * UMG's FDelegateEditorBinding path is for single-cast property bindings and
 * does not register multicast events such as UButton::OnClicked. Multicast
 * events must be represented by UK2Node_ComponentBoundEvent and the generated
 * UComponentDelegateBinding.
 */
bool Upsert(
	UWidgetBlueprint* WidgetBlueprint,
	const FString& WidgetName,
	const FString& EventName,
	const FString& FunctionName,
	bool bAllowDeferredCompile,
	FUpsertResult& OutResult,
	FString& OutError,
	FString& OutErrorCode);

/**
 * Resolves call nodes that were staged while a Workflow deferred Blueprint
 * compilation. Call this immediately before the Workflow compile finalizer.
 */
bool FinalizeDeferred(
	UWidgetBlueprint* WidgetBlueprint,
	int32& OutRepairedCount,
	FString& OutError);

/**
 * Verifies that every UE_AI_integration-owned event node has a compiled
 * handler function and a matching generated component delegate binding.
 * Call this after the Workflow compile finalizer.
 */
bool ValidateCompiled(
	const UWidgetBlueprint* WidgetBlueprint,
	int32& OutVerifiedCount,
	TArray<FString>& OutErrors);

bool TryGetOwnedHandlerFunction(
	const UK2Node_ComponentBoundEvent* EventNode,
	FName& OutFunctionName);
}
