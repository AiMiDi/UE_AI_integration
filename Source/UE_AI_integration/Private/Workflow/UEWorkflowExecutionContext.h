#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UEAIIntegration::Workflow
{
/**
 * Internal execution metadata injected only after public manifest validation.
 *
 * Domain handlers must preserve their legacy behavior when this object is absent.
 * The reserved field is intentionally not part of any public capability schema.
 */
inline TSharedPtr<FJsonObject> GetExecutionContext(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid()
		|| !Params->HasTypedField<EJson::Object>(TEXT("__ueWorkflow")))
	{
		return nullptr;
	}
	return Params->GetObjectField(TEXT("__ueWorkflow"));
}

inline bool ShouldDeferCompile(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject> Context = GetExecutionContext(Params);
	bool bDeferCompile = false;
	return Context.IsValid()
		&& Context->TryGetBoolField(TEXT("deferCompile"), bDeferCompile)
		&& bDeferCompile;
}

inline bool ShouldSaveOnSuccess(const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject> Context = GetExecutionContext(Params);
	bool bSaveOnSuccess = false;
	return Context.IsValid()
		&& Context->TryGetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess)
		&& bSaveOnSuccess;
}

/**
 * True only for parameters that have passed public schema validation and were
 * injected by FWorkflowRuntime after an Editor-prepared plan (including its
 * asset preconditions) was approved.
 */
inline bool IsApprovedWorkflowExecution(
	const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject> Context = GetExecutionContext(Params);
	bool bApprovedPlan = false;
	return Context.IsValid()
		&& Context->TryGetBoolField(
			TEXT("approvedPlan"),
			bApprovedPlan)
		&& bApprovedPlan;
}

inline bool ShouldSaveImmediately(const TSharedPtr<FJsonObject>& Params)
{
	return !ShouldDeferCompile(Params);
}

#if WITH_DEV_AUTOMATION_TESTS
inline int32& MaterialCompileFinalizerCountForTests()
{
	static int32 Count = 0;
	return Count;
}

inline void ResetMaterialCompileFinalizerCountForTests()
{
	MaterialCompileFinalizerCountForTests() = 0;
}

inline void NotifyMaterialCompileFinalizerForTests()
{
	++MaterialCompileFinalizerCountForTests();
}

inline int32 GetMaterialCompileFinalizerCountForTests()
{
	return MaterialCompileFinalizerCountForTests();
}
#endif

inline void MarkBlueprintChanged(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	bool bStructurallyModified = true)
{
	if (!Blueprint)
	{
		return;
	}
	if (ShouldDeferCompile(Params))
	{
		// MarkBlueprintAsStructurallyModified synchronously regenerates the
		// skeleton in UE 5.3. Workflow steps defer that work to the finalizer.
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		Blueprint->MarkPackageDirty();
		return;
	}
	if (bStructurallyModified)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	}
}

inline bool AddBlueprintMemberVariable(
	UBlueprint* Blueprint,
	const FName& VariableName,
	const FEdGraphPinType& VariableType,
	const FString& DefaultValue,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!ShouldDeferCompile(Params))
	{
		return FBlueprintEditorUtils::AddMemberVariable(
			Blueprint,
			VariableName,
			VariableType,
			DefaultValue);
	}
	if (!Blueprint || VariableName.IsNone())
	{
		return false;
	}

	TSet<FName> CurrentVariables;
	FBlueprintEditorUtils::GetClassVariableList(
		Blueprint,
		CurrentVariables);
	if (CurrentVariables.Contains(VariableName))
	{
		return false;
	}

	Blueprint->Modify();
	FBPVariableDescription NewVariable;
	NewVariable.VarName = VariableName;
	NewVariable.VarGuid = FGuid::NewGuid();
	NewVariable.FriendlyName = FName::NameToDisplayString(
		VariableName.ToString(),
		VariableType.PinCategory == UEdGraphSchema_K2::PC_Boolean);
	NewVariable.VarType = VariableType;
	NewVariable.PropertyFlags |=
		CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance;
	if (VariableType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
	{
		NewVariable.PropertyFlags |=
			CPF_BlueprintAssignable | CPF_BlueprintCallable;
	}
	else if (NewVariable.VarType.PinCategory == UEdGraphSchema_K2::PC_Object
		|| NewVariable.VarType.PinCategory
			== UEdGraphSchema_K2::PC_Interface)
	{
		// FBlueprintEditorUtils::PostSetupObjectPinType is protected in UE 5.3.
		// Mirror its small normalization step so deferred workflow authoring can
		// create the variable without calling AddMemberVariable (which compiles
		// the skeleton immediately).
		if (NewVariable.VarType.PinSubCategory
			== UEdGraphSchema_K2::PSC_Self)
		{
			NewVariable.VarType.PinSubCategory = NAME_None;
			NewVariable.VarType.PinSubCategoryObject =
				*Blueprint->GeneratedClass;
		}
		else if (!NewVariable.VarType.PinSubCategoryObject.IsValid())
		{
			NewVariable.VarType.PinSubCategory = NAME_None;
			NewVariable.VarType.PinSubCategoryObject =
				UObject::StaticClass();
		}

		const UClass* ObjectClass = Cast<UClass>(
			NewVariable.VarType.PinSubCategoryObject.Get());
		if (!ObjectClass)
		{
			return false;
		}
		if (ObjectClass->IsChildOf(AActor::StaticClass()))
		{
			NewVariable.PropertyFlags |= CPF_DisableEditOnTemplate;
		}
	}
	NewVariable.ReplicationCondition = COND_None;
	NewVariable.Category = UEdGraphSchema_K2::VR_DefaultCategory;
	NewVariable.DefaultValue = DefaultValue;
	NewVariable.VarType.bIsConst = false;
	NewVariable.VarType.bIsWeakPointer = false;
	NewVariable.VarType.bIsReference = false;
	if (VariableType.PinCategory == UEdGraphSchema_K2::PC_String
		|| VariableType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		NewVariable.SetMetaData(TEXT("MultiLine"), TEXT("true"));
	}

	Blueprint->NewVariables.Add(MoveTemp(NewVariable));
	MarkBlueprintChanged(Blueprint, Params);
	return true;
}
}
