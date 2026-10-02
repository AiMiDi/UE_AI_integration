// Blueprint Mutation Tools — modify nodes, pins, connections, assets
#include "Tools/MCPToolBase.h"
#include "Infrastructure/DeferredGraphMutation.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/BlueprintMutationGuard.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "K2Node.h"
#include "K2Node_ActorBoundEvent.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_InputAction.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Select.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "GameFramework/Actor.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Editor.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/DefaultValueHelper.h"
#include "Modules/ModuleManager.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"

namespace
{
UEdGraphPin* FindUniquePinByIdentity(
	UEdGraphNode* Node,
	const FGuid* PinId,
	const FName PinName,
	bool& bOutAmbiguous)
{
	bOutAmbiguous = false;
	UEdGraphPin* Match = nullptr;
	if (!Node)
	{
		return nullptr;
	}
	for (UEdGraphPin* Candidate : Node->Pins)
	{
		if (!Candidate || (PinId ? Candidate->PinId != *PinId : Candidate->PinName != PinName))
		{
			continue;
		}
		if (Match)
		{
			bOutAmbiguous = true;
			return nullptr;
		}
		Match = Candidate;
	}
	return Match;
}

// Deferred workflow variable additions are kept in UBlueprint::NewVariables
// until the batch compile.  The native variable nodes resolve their value pin
// from the generated/skeleton class, so AllocateDefaultPins() cannot create
// that pin while the class is intentionally stale.  Materialize just the
// value pins described by the deferred variable entry; normal native/member
// variables continue to use the engine implementation unchanged.
bool MaterializeDeferredVariablePins(
	UBlueprint* Blueprint,
	UK2Node_Variable* VariableNode,
	const bool bIsSetNode)
{
	if (!Blueprint || !VariableNode)
	{
		return false;
	}

	const FName VariableName = VariableNode->GetVarName();
	const FBPVariableDescription* Variable = Blueprint->NewVariables.FindByPredicate(
		[VariableName](const FBPVariableDescription& Candidate)
		{
			return Candidate.VarName == VariableName;
		});
	if (!Variable)
	{
		return false;
	}

	auto HasPin = [VariableNode](
		const EEdGraphPinDirection Direction,
		const FName PinName)
	{
		for (const UEdGraphPin* Pin : VariableNode->Pins)
		{
			if (Pin && Pin->Direction == Direction && Pin->PinName == PinName)
			{
				return true;
			}
		}
		return false;
	};

	bool bMaterialized = false;
	if (bIsSetNode)
	{
		if (!HasPin(EGPD_Input, VariableName))
		{
			VariableNode->CreatePin(EGPD_Input, Variable->VarType, VariableName);
			bMaterialized = true;
		}

		const FName OutputPinName(TEXT("Output_Get"));
		if (!HasPin(EGPD_Output, OutputPinName))
		{
			VariableNode->CreatePin(EGPD_Output, Variable->VarType, OutputPinName);
			bMaterialized = true;
		}
	}
	else if (!HasPin(EGPD_Output, VariableName))
	{
		VariableNode->CreatePin(EGPD_Output, Variable->VarType, VariableName);
		bMaterialized = true;
	}

	if (bMaterialized)
	{
		// Match UK2Node_Variable's normal setup so self-context and explicit
		// member access behave the same after the skeleton catches up.
		VariableNode->CreatePinForSelf();
	}
	return bMaterialized;
}

// AssetTools intentionally omits a redirector when all known referencers can
// be fixed up in the same mount point.  Blueprint consumers still need the old
// object path to remain valid across an editor restart, so materialize the
// redirector through the public ObjectTools rename helper when AssetTools did
// not leave one.  The temporary round trip preserves AssetTools' generated
// class/reference fixups and lets UObject::Rename assign the destination safely.
bool EnsureBlueprintRenameRedirector(
	UBlueprint* Blueprint,
	const FString& SourcePackageName,
	const FString& SourceAssetName,
	const FString& DestinationPackageName,
	const FString& DestinationAssetName,
	IAssetRegistry& AssetRegistry,
	FString& OutError)
{
	OutError.Reset();
	if (!Blueprint)
	{
		OutError = TEXT("The renamed Blueprint is invalid.");
		return false;
	}

	const FString SourceObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*SourcePackageName,
		*SourceAssetName);
	const FAssetData ExistingRedirector =
		AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourceObjectPath));
	if (ExistingRedirector.IsValid() && ExistingRedirector.IsRedirector())
	{
		return true;
	}
	// AssetTools registers the redirector with the in-memory object hash before
	// AssetRegistry observes the rename.  Prefer that authoritative UObject
	// state over a second rename round-trip; the latter can collide with the
	// generated-class redirector that AssetTools already created.
	if (UObjectRedirector* ExistingObjectRedirector =
		FindObject<UObjectRedirector>(nullptr, *SourceObjectPath))
	{
		FAssetRegistryModule::AssetCreated(ExistingObjectRedirector);
		ExistingObjectRedirector->GetOutermost()->MarkPackageDirty();
		return true;
	}

	ObjectTools::FPackageGroupName SourceName;
	SourceName.PackageName = SourcePackageName;
	SourceName.ObjectName = SourceAssetName;
	ObjectTools::FPackageGroupName DestinationName;
	DestinationName.PackageName = DestinationPackageName;
	DestinationName.ObjectName = DestinationAssetName;
	TSet<UPackage*> PackagesUserRefusedToFullyLoad;
	FText RenameError;
	if (!ObjectTools::RenameSingleObject(
			Blueprint,
			SourceName,
			PackagesUserRefusedToFullyLoad,
			RenameError,
			nullptr,
			false))
	{
		OutError = RenameError.IsEmpty()
			? TEXT("Could not move the renamed Blueprint through its original path.")
			: RenameError.ToString();
		return false;
	}

	RenameError = FText::GetEmpty();
	if (!ObjectTools::RenameSingleObject(
			Blueprint,
			DestinationName,
			PackagesUserRefusedToFullyLoad,
			RenameError,
			nullptr,
			true))
	{
		OutError = RenameError.IsEmpty()
			? TEXT("Could not restore the renamed Blueprint after creating its redirector.")
			: RenameError.ToString();
		return false;
	}

	UObjectRedirector* Redirector = FindObject<UObjectRedirector>(
		nullptr,
		*SourceObjectPath);
	if (!Redirector)
	{
		OutError = TEXT("The original Blueprint path did not produce a redirector.");
		return false;
	}
	FAssetRegistryModule::AssetCreated(Redirector);
	Redirector->GetOutermost()->MarkPackageDirty();

	const FString RedirectorFilename = FPackageName::LongPackageNameToFilename(
		SourcePackageName,
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(
			Redirector->GetOutermost(),
			Redirector,
			*RedirectorFilename,
			SaveArgs))
	{
		OutError = FString::Printf(
			TEXT("Could not persist Blueprint redirector package '%s'."),
			*RedirectorFilename);
		return false;
	}

	// Save the destination again because the round trip dirties the package
	// after AssetTools has completed its original rename transaction.
	if (!MCPHelpers::SaveBlueprintPackage(Blueprint))
	{
		OutError = TEXT("Could not persist the renamed Blueprint after redirector creation.");
		return false;
	}
	AssetRegistry.ScanPathsSynchronous(
		{FPackageName::GetLongPackagePath(SourcePackageName)},
		true);
	const FAssetData PersistedRedirector = AssetRegistry.GetAssetByObjectPath(
		FSoftObjectPath(SourceObjectPath));
	if (!PersistedRedirector.IsValid() || !PersistedRedirector.IsRedirector())
	{
		OutError = TEXT("The persisted original Blueprint path is not an Asset Registry redirector.");
		return false;
	}
	return true;
}

bool ReadRequiredNumber(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	double& OutValue,
	FString& OutError)
{
	if (!Object.IsValid() || !Object->TryGetNumberField(Field, OutValue)
		|| !FMath::IsFinite(OutValue))
	{
		OutError = FString::Printf(
			TEXT("typedValue.%s must be a finite number."),
			Field);
		return false;
	}
	return true;
}

bool ReadVectorValue(
	const TSharedPtr<FJsonObject>& Object,
	FVector& OutValue,
	FString& OutError)
{
	double X = 0.0;
	double Y = 0.0;
	double Z = 0.0;
	if (!ReadRequiredNumber(Object, TEXT("x"), X, OutError)
		|| !ReadRequiredNumber(Object, TEXT("y"), Y, OutError)
		|| !ReadRequiredNumber(Object, TEXT("z"), Z, OutError))
	{
		return false;
	}
	OutValue = FVector(X, Y, Z);
	return true;
}

bool ReadRotatorValue(
	const TSharedPtr<FJsonObject>& Object,
	FRotator& OutValue,
	FString& OutError)
{
	double Pitch = 0.0;
	double Yaw = 0.0;
	double Roll = 0.0;
	if (!ReadRequiredNumber(Object, TEXT("pitch"), Pitch, OutError)
		|| !ReadRequiredNumber(Object, TEXT("yaw"), Yaw, OutError)
		|| !ReadRequiredNumber(Object, TEXT("roll"), Roll, OutError))
	{
		return false;
	}
	OutValue = FRotator(Pitch, Yaw, Roll);
	return true;
}

bool ExportStructValue(
	UScriptStruct* Struct,
	const void* Memory,
	FString& OutSerialized)
{
	OutSerialized.Reset();
	if (!Struct || !Memory)
	{
		return false;
	}
	Struct->ExportText(
		OutSerialized,
		Memory,
		nullptr,
		nullptr,
		PPF_None,
		nullptr);
	return !OutSerialized.IsEmpty();
}

bool NormalizeStructText(
	UScriptStruct* Struct,
	const FString& Serialized,
	FString& OutNormalized)
{
	if (!Struct || Serialized.IsEmpty())
	{
		return false;
	}
	FStructOnScope Imported(Struct);
	const TCHAR* End = Struct->ImportText(
		*Serialized,
		Imported.GetStructMemory(),
		nullptr,
		PPF_None,
		nullptr,
		Struct->GetName());
	return End != nullptr
		&& ExportStructValue(
			Struct,
			Imported.GetStructMemory(),
			OutNormalized);
}

bool SerializePinStructValue(
	UScriptStruct* Struct,
	const void* Memory,
	FString& OutSerialized)
{
	if (Struct == TBaseStructure<FVector>::Get())
	{
		return ExportStructValue(Struct, Memory, OutSerialized);
	}
	if (Struct == TBaseStructure<FRotator>::Get())
	{
		const FRotator& Rotator = *reinterpret_cast<const FRotator*>(Memory);
		// K2 uses FDefaultValueHelper's vector grammar for Rotator defaults.
		// Export the equivalent vector through UScriptStruct so the bridge does
		// not construct the accepted keyed/comma representation itself.
		const FVector Components(
			Rotator.Pitch,
			Rotator.Yaw,
			Rotator.Roll);
		return ExportStructValue(
			TBaseStructure<FVector>::Get(),
			&Components,
			OutSerialized);
	}
	if (Struct == TBaseStructure<FTransform>::Get())
	{
		OutSerialized =
			reinterpret_cast<const FTransform*>(Memory)->ToString();
		return true;
	}
	if (Struct == TBaseStructure<FLinearColor>::Get())
	{
		OutSerialized =
			reinterpret_cast<const FLinearColor*>(Memory)->ToString();
		return true;
	}
	return false;
}

bool NormalizePinStructText(
	UScriptStruct* Struct,
	const FString& Serialized,
	FString& OutNormalized)
{
	if (Struct == TBaseStructure<FVector>::Get())
	{
		FVector Value;
		return FDefaultValueHelper::ParseVector(Serialized, Value)
			&& SerializePinStructValue(Struct, &Value, OutNormalized);
	}
	if (Struct == TBaseStructure<FRotator>::Get())
	{
		FRotator Value;
		return FDefaultValueHelper::ParseRotator(Serialized, Value)
			&& SerializePinStructValue(Struct, &Value, OutNormalized);
	}
	if (Struct == TBaseStructure<FTransform>::Get())
	{
		FTransform Value;
		return Value.InitFromString(Serialized)
			&& SerializePinStructValue(Struct, &Value, OutNormalized);
	}
	if (Struct == TBaseStructure<FLinearColor>::Get())
	{
		FLinearColor Value;
		return Value.InitFromString(Serialized)
			&& SerializePinStructValue(Struct, &Value, OutNormalized);
	}
	return false;
}

TSharedPtr<FJsonObject> MakePinTypeResult(const UEdGraphPin& Pin)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("category"),
		Pin.PinType.PinCategory.ToString());
	Result->SetStringField(
		TEXT("subCategory"),
		Pin.PinType.PinSubCategory.ToString());
	if (const UObject* TypeObject = Pin.PinType.PinSubCategoryObject.Get())
	{
		Result->SetStringField(TEXT("typeObject"), TypeObject->GetPathName());
	}
	Result->SetBoolField(TEXT("byRef"), Pin.PinType.bIsReference);
	return Result;
}

bool BuildTypedPinDefault(
	const UEdGraphPin& Pin,
	const TSharedPtr<FJsonObject>& TypedValue,
	FString& OutSerialized,
	TSharedPtr<FJsonObject>& OutNormalizedValue,
	FString& OutError)
{
	FString Type;
	const TSharedPtr<FJsonObject>* Value = nullptr;
	if (!TypedValue.IsValid()
		|| !TypedValue->TryGetStringField(TEXT("type"), Type)
		|| !TypedValue->TryGetObjectField(TEXT("value"), Value)
		|| !Value || !Value->IsValid())
	{
		OutError = TEXT("typedValue requires {type, value}.");
		return false;
	}
	UScriptStruct* Struct = Cast<UScriptStruct>(
		Pin.PinType.PinSubCategoryObject.Get());
	if (Pin.PinType.PinCategory != UEdGraphSchema_K2::PC_Struct || !Struct)
	{
		OutError = TEXT("typedValue requires a concrete struct input pin.");
		return false;
	}

	FStructOnScope NativeValue(Struct);
	OutNormalizedValue = MakeShared<FJsonObject>();
	OutNormalizedValue->SetStringField(TEXT("type"), Type);
	TSharedPtr<FJsonObject> Normalized = MakeShared<FJsonObject>();
	if (Type == TEXT("Vector") && Struct == TBaseStructure<FVector>::Get())
	{
		FVector ValueData;
		if (!ReadVectorValue(*Value, ValueData, OutError))
		{
			return false;
		}
		*reinterpret_cast<FVector*>(NativeValue.GetStructMemory()) = ValueData;
		Normalized->SetNumberField(TEXT("x"), ValueData.X);
		Normalized->SetNumberField(TEXT("y"), ValueData.Y);
		Normalized->SetNumberField(TEXT("z"), ValueData.Z);
	}
	else if (Type == TEXT("Rotator")
		&& Struct == TBaseStructure<FRotator>::Get())
	{
		FRotator ValueData;
		if (!ReadRotatorValue(*Value, ValueData, OutError))
		{
			return false;
		}
		*reinterpret_cast<FRotator*>(NativeValue.GetStructMemory()) = ValueData;
		Normalized->SetNumberField(TEXT("pitch"), ValueData.Pitch);
		Normalized->SetNumberField(TEXT("yaw"), ValueData.Yaw);
		Normalized->SetNumberField(TEXT("roll"), ValueData.Roll);
	}
	else if (Type == TEXT("Transform")
		&& Struct == TBaseStructure<FTransform>::Get())
	{
		const TSharedPtr<FJsonObject>* Translation = nullptr;
		const TSharedPtr<FJsonObject>* Rotation = nullptr;
		const TSharedPtr<FJsonObject>* Scale = nullptr;
		if (!(*Value)->TryGetObjectField(TEXT("translation"), Translation)
			|| !Translation || !Translation->IsValid()
			|| !(*Value)->TryGetObjectField(TEXT("rotation"), Rotation)
			|| !Rotation || !Rotation->IsValid()
			|| !(*Value)->TryGetObjectField(TEXT("scale3D"), Scale)
			|| !Scale || !Scale->IsValid())
		{
			OutError = TEXT("Transform requires translation, rotation, and scale3D objects.");
			return false;
		}
		FVector TranslationValue;
		FRotator RotationValue;
		FVector ScaleValue;
		if (!ReadVectorValue(*Translation, TranslationValue, OutError)
			|| !ReadRotatorValue(*Rotation, RotationValue, OutError)
			|| !ReadVectorValue(*Scale, ScaleValue, OutError))
		{
			return false;
		}
		*reinterpret_cast<FTransform*>(NativeValue.GetStructMemory()) =
			FTransform(RotationValue, TranslationValue, ScaleValue);
		Normalized = *Value;
	}
	else if (Type == TEXT("LinearColor")
		&& Struct == TBaseStructure<FLinearColor>::Get())
	{
		double R = 0.0;
		double G = 0.0;
		double B = 0.0;
		double A = 0.0;
		if (!ReadRequiredNumber(*Value, TEXT("r"), R, OutError)
			|| !ReadRequiredNumber(*Value, TEXT("g"), G, OutError)
			|| !ReadRequiredNumber(*Value, TEXT("b"), B, OutError)
			|| !ReadRequiredNumber(*Value, TEXT("a"), A, OutError))
		{
			return false;
		}
		const FLinearColor ValueData(R, G, B, A);
		*reinterpret_cast<FLinearColor*>(NativeValue.GetStructMemory()) = ValueData;
		Normalized->SetNumberField(TEXT("r"), ValueData.R);
		Normalized->SetNumberField(TEXT("g"), ValueData.G);
		Normalized->SetNumberField(TEXT("b"), ValueData.B);
		Normalized->SetNumberField(TEXT("a"), ValueData.A);
	}
	else
	{
		OutError = FString::Printf(
			TEXT("typedValue type '%s' does not match pin struct '%s'."),
			*Type,
			*Struct->GetPathName());
		return false;
	}

	OutNormalizedValue->SetObjectField(TEXT("value"), Normalized);
	FString Exported;
	FString RoundTripExported;
	if (!ExportStructValue(Struct, NativeValue.GetStructMemory(), Exported)
		|| !NormalizeStructText(Struct, Exported, RoundTripExported)
		|| !SerializePinStructValue(
			Struct,
			NativeValue.GetStructMemory(),
			OutSerialized))
	{
		OutError = TEXT("UE struct import/export could not serialize typedValue.");
		return false;
	}
	return true;
}

FMCPToolResult RollbackDirectMutation(
	UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard& Guard,
	const FString& Message,
	const FString& Code)
{
	FString RollbackError;
	const bool bRollbackVerified = Guard.Rollback(RollbackError);
	return FMCPToolResult::Error(
		bRollbackVerified
			? Message + TEXT(" The mutation was rolled back and verified.")
			: Message + TEXT(" ") + RollbackError,
		bRollbackVerified ? Code : TEXT("rollback_failed"),
		500);
}

bool FinalizeDirectMutation(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard& Guard,
	bool& OutSaved,
	bool& OutCompiled,
	FMCPToolResult& OutFailure)
{
	OutSaved = false;
	OutCompiled = false;
	if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
	{
		Guard.Commit();
		return true;
	}
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint,
		EBlueprintCompileOptions::SkipSave);
	OutCompiled = Blueprint->Status != BS_Error;
	if (!OutCompiled)
	{
		OutFailure = RollbackDirectMutation(
			Guard,
			TEXT("The Blueprint mutation introduced compile errors."),
			TEXT("asset_compile_failed"));
		return false;
	}
	UEAIIntegration::Infrastructure::FBlueprintPersistenceError SaveError;
	OutSaved = UEAIIntegration::Infrastructure::SaveBlueprintPackage(
		Blueprint,
		nullptr,
		SaveError);
	if (!OutSaved)
	{
		OutFailure = RollbackDirectMutation(
			Guard,
			SaveError.Message.IsEmpty()
				? TEXT("The Blueprint mutation could not be saved.")
				: SaveError.Message,
			SaveError.Code.IsEmpty()
				? TEXT("asset_save_failed")
				: SaveError.Code);
		return false;
	}
	Guard.Commit();
	return true;
}
}

// ============================================================
// replace_function_calls
// ============================================================
class FTool_ReplaceFunctionCalls : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.function.calls.replace");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString OldClassName = Params->GetStringField(TEXT("oldClass"));
		FString NewClassName = Params->GetStringField(TEXT("newClass"));
		if (BlueprintName.IsEmpty() || OldClassName.IsEmpty() || NewClassName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, oldClass, newClass"));

		bool bDryRun = Params->HasField(TEXT("dryRun")) && Params->GetBoolField(TEXT("dryRun"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UClass* NewClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == NewClassName) { NewClass = *It; break; }
		}
		if (!NewClass)
			return FMCPToolResult::Error(FString::Printf(TEXT("Could not find class '%s'"), *NewClassName));

		TArray<UK2Node_CallFunction*> AllCallNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_CallFunction>(BP, AllCallNodes);

		int32 ReplacedCount = 0;
		for (UK2Node_CallFunction* CallNode : AllCallNodes)
		{
			UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass();
			if (!ParentClass) continue;
			FString ParentName = ParentClass->GetName();
			bool bMatch = ParentName == OldClassName || ParentName == OldClassName + TEXT("_C");
			if (!bMatch) continue;

			FName FuncName = CallNode->FunctionReference.GetMemberName();
			UFunction* NewFunc = NewClass->FindFunctionByName(FuncName);
			if (!NewFunc) continue;

			if (!bDryRun)
				CallNode->SetFromFunction(NewFunc);
			ReplacedCount++;
		}

		if (!bDryRun && ReplacedCount > 0)
			MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetNumberField(TEXT("replacedCount"), ReplacedCount);
		Result->SetBoolField(TEXT("dryRun"), bDryRun);
		if (!bDryRun) Result->SetBoolField(TEXT("saved"), ReplacedCount > 0);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// delete_asset
// ============================================================
class FTool_DeleteAsset : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.delete");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath = Params->GetStringField(TEXT("assetPath"));
		if (AssetPath.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'assetPath'"));

		bool bForce = Params->HasField(TEXT("force")) && Params->GetBoolField(TEXT("force"));

		FString PackageFilename = FPackageName::LongPackageNameToFilename(AssetPath, FPackageName::GetAssetPackageExtension());
		PackageFilename = FPaths::ConvertRelativePathToFull(PackageFilename);

		if (!IFileManager::Get().FileExists(*PackageFilename))
			return FMCPToolResult::Error(FString::Printf(TEXT("Asset file not found: %s"), *PackageFilename));

		IAssetRegistry& Registry = *IAssetRegistry::Get();
		TArray<FName> Referencers;
		Registry.GetReferencers(FName(*AssetPath), Referencers);
		Referencers.RemoveAll([&AssetPath](const FName& Ref) { return Ref.ToString() == AssetPath; });

		if (Referencers.Num() > 0 && !bForce)
			return FMCPToolResult::Error(FString::Printf(TEXT("Asset has %d referencers. Use force=true to override."), Referencers.Num()));

		bool bDeleted = IFileManager::Get().Delete(*PackageFilename, false, true);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), bDeleted);
		Result->SetStringField(TEXT("assetPath"), AssetPath);
		Result->SetBoolField(TEXT("forced"), bForce);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// connect_pins
// ============================================================
class FTool_ConnectPins : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.pin.connect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString SourceNodeId = Params->GetStringField(TEXT("sourceNodeId"));
		FString SourcePinName = Params->GetStringField(TEXT("sourcePinName"));
		FString TargetNodeId = Params->GetStringField(TEXT("targetNodeId"));
		FString TargetPinName = Params->GetStringField(TEXT("targetPinName"));

		if (BlueprintName.IsEmpty() || SourceNodeId.IsEmpty() || SourcePinName.IsEmpty() || TargetNodeId.IsEmpty() || TargetPinName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraph* SourceGraph = nullptr;
		UEdGraphNode* SourceNode = MCPHelpers::FindNodeByGuid(BP, SourceNodeId, &SourceGraph);
		if (!SourceNode) return FMCPToolResult::Error(FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId));

		UEdGraphNode* TargetNode = MCPHelpers::FindNodeByGuid(BP, TargetNodeId);
		if (!TargetNode) return FMCPToolResult::Error(FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId));

		UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName));
		if (!SourcePin) return FMCPToolResult::Error(FString::Printf(TEXT("Source pin '%s' not found"), *SourcePinName));

		UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
		if (!TargetPin) return FMCPToolResult::Error(FString::Printf(TEXT("Target pin '%s' not found"), *TargetPinName));

		const UEdGraphSchema* Schema = SourceGraph ? SourceGraph->GetSchema() : nullptr;
		if (!Schema) return FMCPToolResult::Error(TEXT("Graph schema not found"));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(),
				Guard.GetErrorCode(),
				422);
		}
		Guard.MarkMutationStarted();

		SourceNode->Modify();
		TargetNode->Modify();
		bool bConnected = UEAIIntegration::Infrastructure::TryCreateConnection(
			Schema, SourcePin, TargetPin,
			UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		if (!bConnected)
			return RollbackDirectMutation(
				Guard,
				TEXT("Cannot connect pins because the graph schema rejected the connection."),
				TEXT("pin_connection_rejected"));

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		bool bSaved = false;
		bool bCompiled = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP, Params, Guard, bSaved, bCompiled, Failure))
		{
			return Failure;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// disconnect_pin
// ============================================================
class FTool_DisconnectPin : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.pin.disconnect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString NodeId = Params->GetStringField(TEXT("nodeId"));
		FString PinName = Params->GetStringField(TEXT("pinName"));
		if (BlueprintName.IsEmpty() || NodeId.IsEmpty() || PinName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields"));

		FString TargetNodeId = Params->GetStringField(TEXT("targetNodeId"));
		FString TargetPinName = Params->GetStringField(TEXT("targetPinName"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId);
		if (!Node) return FMCPToolResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

		UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
		if (!Pin) return FMCPToolResult::Error(FString::Printf(TEXT("Pin '%s' not found"), *PinName));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(),
				Guard.GetErrorCode(),
				422);
		}

		int32 DisconnectedCount = 0;
		Guard.MarkMutationStarted();
		if (!TargetNodeId.IsEmpty() && !TargetPinName.IsEmpty())
		{
			UEdGraphNode* TargetNode = MCPHelpers::FindNodeByGuid(BP, TargetNodeId);
			if (!TargetNode) return FMCPToolResult::Error(TEXT("Target node not found"));
			UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
			if (!TargetPin) return FMCPToolResult::Error(TEXT("Target pin not found"));
			if (Pin->LinkedTo.Contains(TargetPin)) { Pin->BreakLinkTo(TargetPin); DisconnectedCount = 1; }
		}
		else
		{
			DisconnectedCount = Pin->LinkedTo.Num();
			if (DisconnectedCount > 0) Pin->BreakAllPinLinks(true);
		}

		bool bSaved = false;
		bool bCompiled = false;
		if (DisconnectedCount > 0)
		{
			UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
			FMCPToolResult Failure;
			if (!FinalizeDirectMutation(
				BP, Params, Guard, bSaved, bCompiled, Failure))
			{
				return Failure;
			}
		}
		else
		{
			Guard.Commit();
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("disconnectedCount"), DisconnectedCount);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// add_node
// ============================================================
namespace
{
UClass* FindNodeOwnerClass(
	const FString& ClassName,
	UBlueprint* Blueprint)
{
	if (!ClassName.IsEmpty())
	{
		if (UClass* Loaded = LoadObject<UClass>(
				nullptr,
				*ClassName))
		{
			return Loaded;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName().Equals(
					ClassName,
					ESearchCase::IgnoreCase)
				|| It->GetPathName().Equals(
					ClassName,
					ESearchCase::IgnoreCase)
				|| It->GetName().Equals(
					ClassName + TEXT("_C"),
					ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}
	return Blueprint
		? (Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass
			: Blueprint->GeneratedClass)
		: nullptr;
}

UFunction* FindNodeFunction(
	const FString& FunctionName,
	const FString& ClassName,
	UBlueprint* Blueprint)
{
	if (FunctionName.IsEmpty())
	{
		return nullptr;
	}
	if (UClass* OwnerClass =
		FindNodeOwnerClass(ClassName, Blueprint))
	{
		if (UFunction* Function = OwnerClass->FindFunctionByName(
				FName(*FunctionName)))
		{
			return Function;
		}
	}
	if (ClassName.IsEmpty() && Blueprint)
	{
		for (UClass* Parent = Blueprint->ParentClass;
			Parent;
			Parent = Parent->GetSuperClass())
		{
			if (UFunction* Function =
				Parent->FindFunctionByName(
					FName(*FunctionName)))
			{
				return Function;
			}
		}
	}
	return nullptr;
}

template <typename NodeType, typename InitializerType>
NodeType* SpawnConfiguredNode(
	UEdGraph* Graph,
	const int32 PosX,
	const int32 PosY,
	InitializerType&& Initializer)
{
	return FEdGraphSchemaAction_K2NewNode::SpawnNode<NodeType>(
		Graph,
		FVector2D(PosX, PosY),
		EK2NewNodeFlags::None,
		Forward<InitializerType>(Initializer));
}

FMulticastDelegateProperty* FindMulticastDelegateProperty(
	UBlueprint* Blueprint,
	const FString& ClassName,
	const FString& DelegateName,
	UClass*& OutOwnerClass)
{
	OutOwnerClass = FindNodeOwnerClass(ClassName, Blueprint);
	return OutOwnerClass && !DelegateName.IsEmpty()
		? FindFProperty<FMulticastDelegateProperty>(
			OutOwnerClass,
			FName(*DelegateName))
		: nullptr;
}

template <typename DelegateNodeType>
DelegateNodeType* SpawnDelegateNode(
	UEdGraph* Graph,
	UBlueprint* Blueprint,
	const FString& ClassName,
	const FString& DelegateName,
	const int32 PosX,
	const int32 PosY)
{
	UClass* OwnerClass = nullptr;
	FMulticastDelegateProperty* DelegateProperty =
		FindMulticastDelegateProperty(
			Blueprint,
			ClassName,
			DelegateName,
			OwnerClass);
	if (!DelegateProperty)
	{
		return nullptr;
	}
	const bool bSelfContext =
		OwnerClass == Blueprint->SkeletonGeneratedClass
		|| OwnerClass == Blueprint->GeneratedClass;
	return SpawnConfiguredNode<DelegateNodeType>(
		Graph,
		PosX,
		PosY,
		[DelegateProperty, bSelfContext, OwnerClass](
			DelegateNodeType* Node)
		{
			Node->SetFromProperty(
				DelegateProperty,
				bSelfContext,
				OwnerClass);
		});
}

bool SetReflectedObjectProperty(
	UObject* Object,
	const FName PropertyName,
	UObject* Value)
{
	FObjectPropertyBase* Property =
		Object
		? FindFProperty<FObjectPropertyBase>(
			Object->GetClass(),
			PropertyName)
		: nullptr;
	if (!Property
		|| (Value
			&& !Value->IsA(Property->PropertyClass)))
	{
		return false;
	}
	Property->SetObjectPropertyValue_InContainer(
		Object,
		Value);
	return true;
}

bool SetReflectedNameProperty(
	UObject* Object,
	const FName PropertyName,
	const FName Value)
{
	FNameProperty* Property =
		Object
		? FindFProperty<FNameProperty>(
			Object->GetClass(),
			PropertyName)
		: nullptr;
	if (!Property)
	{
		return false;
	}
	Property->SetPropertyValue_InContainer(Object, Value);
	return true;
}

bool HasUnresolvedWildcardPins(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return true;
	}
	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin
			&& Pin->PinType.PinCategory
				== UEdGraphSchema_K2::PC_Wildcard)
		{
			return true;
		}
	}
	return false;
}

void RollbackAddedGraphNodes(
	UEdGraph* Graph,
	const TSet<UEdGraphNode*>& NodesBefore,
	UPackage* Package,
	const bool bPackageWasDirty)
{
	if (Graph)
	{
		const TArray<TObjectPtr<UEdGraphNode>> CurrentNodes =
			Graph->Nodes;
		for (UEdGraphNode* Node : CurrentNodes)
		{
			if (Node && !NodesBefore.Contains(Node))
			{
				Node->DestroyNode();
			}
		}
	}
	if (Package)
	{
		Package->SetDirtyFlag(bPackageWasDirty);
	}
}

class FScopedBlueprintPropertyValue
{
public:
	explicit FScopedBlueprintPropertyValue(FProperty* InProperty)
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

	~FScopedBlueprintPropertyValue()
	{
		if (Property && Value)
		{
			Property->DestroyValue(Value);
			FMemory::Free(Value);
		}
	}

	FScopedBlueprintPropertyValue(
		const FScopedBlueprintPropertyValue&) = delete;
	FScopedBlueprintPropertyValue& operator=(
		const FScopedBlueprintPropertyValue&) = delete;

	void* Get() const
	{
		return Value;
	}

private:
	FProperty* Property = nullptr;
	void* Value = nullptr;
};

bool ImportBlueprintPropertyText(
	UObject* Object,
	FProperty* Property,
	const FString& SerializedValue,
	const bool bApply,
	FString& OutNormalizedValue)
{
	if (!Object || !Property)
	{
		return false;
	}
	FScopedBlueprintPropertyValue ScratchValue(Property);
	if (!ScratchValue.Get())
	{
		return false;
	}
	const TCHAR* ImportEnd = Property->ImportText_Direct(
		*SerializedValue,
		ScratchValue.Get(),
		Object,
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
		Object,
		PPF_None);
	if (!bApply)
	{
		return true;
	}

	void* Address = Property->ContainerPtrToValuePtr<void>(Object);
	Object->SetFlags(RF_Transactional);
	Object->Modify();
	FEditPropertyChain PropertyChain;
	PropertyChain.AddHead(Property);
	PropertyChain.SetActivePropertyNode(Property);
	Object->PreEditChange(PropertyChain);
	Property->CopyCompleteValue(Address, ScratchValue.Get());
	FPropertyChangedEvent PropertyEvent(
		Property,
		EPropertyChangeType::ValueSet);
	FPropertyChangedChainEvent ChainEvent(PropertyChain, PropertyEvent);
	Object->PostEditChangeChainProperty(ChainEvent);

	FString AppliedValue;
	Property->ExportText_Direct(
		AppliedValue,
		Address,
		Address,
		Object,
		PPF_None);
	return AppliedValue == OutNormalizedValue;
}

// Creates exactly one node in TargetGraph from Params (nodeType plus any
// type-specific fields).  Returns Ok with OutNode set on success, or an Error
// describing the failure.  A node added before a late validation failure is
// left in the graph so the caller can roll it back atomically; the caller owns
// transaction, compile, and rollback.
FMCPToolResult TryCreateNodeInGraph(
	UBlueprint* BP,
	UEdGraph* TargetGraph,
	const TSharedPtr<FJsonObject>& Params,
	const int32 PosX,
	const int32 PosY,
	const bool bDeferred,
	UEdGraphNode*& OutNode)
{
	OutNode = nullptr;
	const FString NodeType = Params->GetStringField(TEXT("nodeType"));
	UEdGraphNode* NewNode = nullptr;

	if (NodeType == TEXT("CallFunction"))
	{
		FString FunctionName = Params->GetStringField(TEXT("functionName"));
		FString ClassName = Params->GetStringField(TEXT("className"));
		if (FunctionName.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'functionName'"));

		UFunction* TargetFunc =
			FindNodeFunction(
				FunctionName,
				ClassName,
				BP);
		if (!TargetFunc)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UFunction* F = It->FindFunctionByName(FName(*FunctionName));
				if (F) { TargetFunc = F; break; }
			}
		}
		if (!TargetFunc) return FMCPToolResult::Error(FString::Printf(TEXT("Function '%s' not found"), *FunctionName));
		bool bRequireLatent = false;
		Params->TryGetBoolField(
			TEXT("latent"),
			bRequireLatent);
		if (bRequireLatent
			&& !TargetFunc->HasMetaData(
				TEXT("Latent")))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Function '%s' is not latent."),
					*FunctionName),
				TEXT("signature_mismatch"),
				422);
		}

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(
			TargetGraph,
			NAME_None,
			RF_Transactional);
		CallNode->Modify();
		CallNode->SetFromFunction(TargetFunc);
		CallNode->NodePosX = PosX; CallNode->NodePosY = PosY;
		TargetGraph->AddNode(CallNode, false, false);
		CallNode->AllocateDefaultPins();
		NewNode = CallNode;
	}
	else if (NodeType == TEXT("VariableGet") || NodeType == TEXT("VariableSet"))
	{
		FString VariableName = Params->GetStringField(TEXT("variableName"));
		if (VariableName.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'variableName'"));

		if (NodeType == TEXT("VariableGet"))
		{
			UK2Node_VariableGet* N = NewObject<UK2Node_VariableGet>(
				TargetGraph,
				NAME_None,
				RF_Transactional);
			N->Modify();
			N->VariableReference.SetSelfMember(FName(*VariableName));
			N->NodePosX = PosX; N->NodePosY = PosY;
			TargetGraph->AddNode(N, false, false);
			N->AllocateDefaultPins();
			MaterializeDeferredVariablePins(BP, N, false);
			NewNode = N;
		}
		else
		{
			UK2Node_VariableSet* N = NewObject<UK2Node_VariableSet>(
				TargetGraph,
				NAME_None,
				RF_Transactional);
			N->Modify();
			N->VariableReference.SetSelfMember(FName(*VariableName));
			N->NodePosX = PosX; N->NodePosY = PosY;
			TargetGraph->AddNode(N, false, false);
			N->AllocateDefaultPins();
			MaterializeDeferredVariablePins(BP, N, true);
			NewNode = N;
		}
	}
	else if (NodeType == TEXT("BreakStruct") || NodeType == TEXT("MakeStruct"))
	{
		FString TypeNameStr = Params->GetStringField(TEXT("typeName"));
		if (TypeNameStr.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'typeName'"));
		FString SearchName = TypeNameStr.StartsWith(TEXT("F")) ? TypeNameStr.Mid(1) : TypeNameStr;
		UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*SearchName);
		if (!FoundStruct) FoundStruct = FindFirstObject<UScriptStruct>(*TypeNameStr);
		if (!FoundStruct) return FMCPToolResult::Error(FString::Printf(TEXT("Struct '%s' not found"), *TypeNameStr));

		if (NodeType == TEXT("BreakStruct"))
		{
			UK2Node_BreakStruct* N = NewObject<UK2Node_BreakStruct>(
				TargetGraph,
				NAME_None,
				RF_Transactional);
			N->Modify();
			N->StructType = FoundStruct; N->NodePosX = PosX; N->NodePosY = PosY;
			TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
		}
		else
		{
			UK2Node_MakeStruct* N = NewObject<UK2Node_MakeStruct>(
				TargetGraph,
				NAME_None,
				RF_Transactional);
			N->Modify();
			N->StructType = FoundStruct; N->NodePosX = PosX; N->NodePosY = PosY;
			TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
		}
	}
	else if (NodeType == TEXT("Branch"))
	{
		UK2Node_IfThenElse* N = NewObject<UK2Node_IfThenElse>(
			TargetGraph,
			NAME_None,
			RF_Transactional);
		N->Modify();
		N->NodePosX = PosX; N->NodePosY = PosY;
		TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
	}
	else if (NodeType == TEXT("Sequence"))
	{
		UK2Node_ExecutionSequence* N =
			NewObject<UK2Node_ExecutionSequence>(
				TargetGraph,
				NAME_None,
				RF_Transactional);
		N->Modify();
		N->NodePosX = PosX; N->NodePosY = PosY;
		TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
	}
	else if (NodeType == TEXT("CustomEvent"))
	{
		FString EventName = Params->GetStringField(TEXT("eventName"));
		if (EventName.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'eventName'"));
		UK2Node_CustomEvent* N = NewObject<UK2Node_CustomEvent>(
			TargetGraph,
			NAME_None,
			RF_Transactional);
		N->Modify();
		N->CustomFunctionName = FName(*EventName);
		N->NodePosX = PosX; N->NodePosY = PosY;
		TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
	}
	else if (NodeType == TEXT("OverrideEvent"))
	{
		FString FunctionName;
		FString ClassName;
		Params->TryGetStringField(
			TEXT("functionName"),
			FunctionName);
		Params->TryGetStringField(
			TEXT("className"),
			ClassName);
		UFunction* Function =
			FindNodeFunction(
				FunctionName,
				ClassName,
				BP);
		if (!Function
			|| !Function->HasAnyFunctionFlags(
				FUNC_BlueprintEvent)
			|| Function->HasAnyFunctionFlags(FUNC_Final))
		{
			return FMCPToolResult::Error(
				TEXT("OverrideEvent requires a non-final Blueprint event function."),
				TEXT("signature_mismatch"),
				422);
		}
		UClass* FunctionOwner =
			Function->GetOuterUClass();
		NewNode = SpawnConfiguredNode<UK2Node_Event>(
			TargetGraph,
			PosX,
			PosY,
			[Function, FunctionOwner](UK2Node_Event* Node)
			{
				Node->bOverrideFunction = true;
				Node->EventReference.SetExternalMember(
					Function->GetFName(),
					FunctionOwner);
			});
	}
	else if (NodeType == TEXT("ComponentBoundEvent"))
	{
		FString ComponentName;
		FString DelegateName;
		Params->TryGetStringField(
			TEXT("componentName"),
			ComponentName);
		Params->TryGetStringField(
			TEXT("delegateName"),
			DelegateName);
		UClass* BlueprintClass =
			BP->SkeletonGeneratedClass
				? BP->SkeletonGeneratedClass
				: BP->GeneratedClass;
		FObjectProperty* ComponentProperty =
			BlueprintClass
			? FindFProperty<FObjectProperty>(
				BlueprintClass,
				FName(*ComponentName))
			: nullptr;
		FMulticastDelegateProperty* DelegateProperty =
			ComponentProperty
			&& ComponentProperty->PropertyClass
			? FindFProperty<FMulticastDelegateProperty>(
				ComponentProperty->PropertyClass,
				FName(*DelegateName))
			: nullptr;
		if (!ComponentProperty || !DelegateProperty)
		{
			return FMCPToolResult::Error(
				TEXT("ComponentBoundEvent could not resolve componentName and delegateName."),
				TEXT("signature_mismatch"),
				422);
		}
		NewNode =
			SpawnConfiguredNode<UK2Node_ComponentBoundEvent>(
				TargetGraph,
				PosX,
				PosY,
				[ComponentProperty, DelegateProperty](
					UK2Node_ComponentBoundEvent* Node)
				{
					Node->InitializeComponentBoundEventParams(
						ComponentProperty,
						DelegateProperty);
				});
	}
	else if (NodeType == TEXT("ActorBoundEvent"))
	{
		FString ActorPath;
		FString DelegateName;
		Params->TryGetStringField(
			TEXT("actorPath"),
			ActorPath);
		Params->TryGetStringField(
			TEXT("delegateName"),
			DelegateName);
		AActor* Actor = FindObject<AActor>(
			nullptr,
			*ActorPath);
		FMulticastDelegateProperty* DelegateProperty =
			Actor
			? FindFProperty<FMulticastDelegateProperty>(
				Actor->GetClass(),
				FName(*DelegateName))
			: nullptr;
		if (!Actor || !DelegateProperty)
		{
			return FMCPToolResult::Error(
				TEXT("ActorBoundEvent could not resolve actorPath and delegateName."),
				TEXT("signature_mismatch"),
				422);
		}
		NewNode =
			SpawnConfiguredNode<UK2Node_ActorBoundEvent>(
				TargetGraph,
				PosX,
				PosY,
				[Actor, DelegateProperty](
					UK2Node_ActorBoundEvent* Node)
				{
					Node->InitializeActorBoundEventParams(
						Actor,
						DelegateProperty);
				});
	}
	else if (NodeType == TEXT("AssignDelegate")
		|| NodeType == TEXT("AddDelegate")
		|| NodeType == TEXT("RemoveDelegate")
		|| NodeType == TEXT("ClearDelegate")
		|| NodeType == TEXT("CallDelegate"))
	{
		FString ClassName;
		FString DelegateName;
		Params->TryGetStringField(
			TEXT("className"),
			ClassName);
		Params->TryGetStringField(
			TEXT("delegateName"),
			DelegateName);
		if (NodeType == TEXT("AssignDelegate"))
		{
			NewNode = SpawnDelegateNode<UK2Node_AssignDelegate>(
				TargetGraph,
				BP,
				ClassName,
				DelegateName,
				PosX,
				PosY);
		}
		else if (NodeType == TEXT("AddDelegate"))
		{
			NewNode = SpawnDelegateNode<UK2Node_AddDelegate>(
				TargetGraph,
				BP,
				ClassName,
				DelegateName,
				PosX,
				PosY);
		}
		else if (NodeType == TEXT("RemoveDelegate"))
		{
			NewNode = SpawnDelegateNode<UK2Node_RemoveDelegate>(
				TargetGraph,
				BP,
				ClassName,
				DelegateName,
				PosX,
				PosY);
		}
		else if (NodeType == TEXT("ClearDelegate"))
		{
			NewNode = SpawnDelegateNode<UK2Node_ClearDelegate>(
				TargetGraph,
				BP,
				ClassName,
				DelegateName,
				PosX,
				PosY);
		}
		else
		{
			NewNode = SpawnDelegateNode<UK2Node_CallDelegate>(
				TargetGraph,
				BP,
				ClassName,
				DelegateName,
				PosX,
				PosY);
		}
		if (!NewNode)
		{
			return FMCPToolResult::Error(
				TEXT("Delegate node could not resolve delegateName on className/self."),
				TEXT("signature_mismatch"),
				422);
		}
	}
	else if (NodeType == TEXT("CreateDelegate"))
	{
		FString FunctionName;
		Params->TryGetStringField(
			TEXT("functionName"),
			FunctionName);
		if (FunctionName.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("CreateDelegate requires functionName."),
				TEXT("invalid_params"),
				422);
		}
		NewNode = SpawnConfiguredNode<UK2Node_CreateDelegate>(
			TargetGraph,
			PosX,
			PosY,
			[FunctionName](UK2Node_CreateDelegate* Node)
			{
				Node->SetFunction(FName(*FunctionName));
			});
	}
	else if (NodeType == TEXT("InputAction"))
	{
		FString InputActionName;
		if (!Params->TryGetStringField(
				TEXT("inputActionName"),
				InputActionName))
		{
			Params->TryGetStringField(
				TEXT("eventName"),
				InputActionName);
		}
		if (InputActionName.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("InputAction requires inputActionName."),
				TEXT("invalid_params"),
				422);
		}
		bool bConsumeInput = true;
		bool bExecuteWhenPaused = false;
		bool bOverrideParentBinding = true;
		Params->TryGetBoolField(
			TEXT("consumeInput"),
			bConsumeInput);
		Params->TryGetBoolField(
			TEXT("executeWhenPaused"),
			bExecuteWhenPaused);
		Params->TryGetBoolField(
			TEXT("overrideParentBinding"),
			bOverrideParentBinding);
		NewNode = SpawnConfiguredNode<UK2Node_InputAction>(
			TargetGraph,
			PosX,
			PosY,
			[InputActionName,
				bConsumeInput,
				bExecuteWhenPaused,
				bOverrideParentBinding](
				UK2Node_InputAction* Node)
			{
				Node->InputActionName =
					FName(*InputActionName);
				Node->bConsumeInput = bConsumeInput;
				Node->bExecuteWhenPaused =
					bExecuteWhenPaused;
				Node->bOverrideParentBinding =
					bOverrideParentBinding;
			});
	}
	else if (NodeType == TEXT("EnhancedInputAction"))
	{
		FString InputActionPath;
		Params->TryGetStringField(
			TEXT("inputAction"),
			InputActionPath);
		if (InputActionPath.IsEmpty())
		{
			Params->TryGetStringField(
				TEXT("inputActionPath"),
				InputActionPath);
		}
		UObject* InputAction = LoadObject<UObject>(
			nullptr,
			*InputActionPath);
		FModuleManager::Get().LoadModulePtr<IModuleInterface>(
			TEXT("InputBlueprintNodes"));
		UClass* EnhancedNodeClass = FindObject<UClass>(
			nullptr,
			TEXT("/Script/InputBlueprintNodes.K2Node_EnhancedInputAction"));
		if (!InputAction || !EnhancedNodeClass)
		{
			return FMCPToolResult::Error(
				TEXT("EnhancedInputAction requires a valid inputAction asset and InputBlueprintNodes module."),
				TEXT("target_not_found"),
				404);
		}
		NewNode = FEdGraphSchemaAction_K2NewNode::CreateNode(
			TargetGraph,
			TArrayView<UEdGraphPin*>(),
			FVector2D(PosX, PosY),
			[EnhancedNodeClass](
				UEdGraph* InParentGraph) -> UK2Node*
			{
				return NewObject<UK2Node>(
					InParentGraph,
					EnhancedNodeClass);
			},
			[InputAction](UK2Node* Node)
			{
				SetReflectedObjectProperty(
					Node,
					TEXT("InputAction"),
					InputAction);
			},
			EK2NewNodeFlags::None);
	}
	else if (NodeType == TEXT("AsyncAction"))
	{
		FString FactoryFunctionName;
		FString FactoryClassName;
		if (!Params->TryGetStringField(
				TEXT("factoryFunctionName"),
				FactoryFunctionName))
		{
			Params->TryGetStringField(
				TEXT("functionName"),
				FactoryFunctionName);
		}
		if (!Params->TryGetStringField(
				TEXT("factoryClassName"),
				FactoryClassName))
		{
			Params->TryGetStringField(
				TEXT("className"),
				FactoryClassName);
		}
		UFunction* FactoryFunction =
			FindNodeFunction(
				FactoryFunctionName,
				FactoryClassName,
				BP);
		FObjectPropertyBase* ReturnProperty =
			FactoryFunction
			? CastField<FObjectPropertyBase>(
				FactoryFunction->GetReturnProperty())
			: nullptr;
		UClass* ProxyClass =
			ReturnProperty
				? ReturnProperty->PropertyClass
				: nullptr;
		if (!FactoryFunction
			|| !FactoryFunction->HasAllFunctionFlags(
				FUNC_Static | FUNC_BlueprintCallable)
			|| !ProxyClass
			|| !ProxyClass->IsChildOf(
				UBlueprintAsyncActionBase::StaticClass()))
		{
			return FMCPToolResult::Error(
				TEXT("AsyncAction requires a static BlueprintCallable factory returning UBlueprintAsyncActionBase."),
				TEXT("signature_mismatch"),
				422);
		}
		NewNode = SpawnConfiguredNode<UK2Node_AsyncAction>(
			TargetGraph,
			PosX,
			PosY,
			[FactoryFunction, ProxyClass](
				UK2Node_AsyncAction* Node)
			{
				SetReflectedNameProperty(
					Node,
					TEXT("ProxyFactoryFunctionName"),
					FactoryFunction->GetFName());
				SetReflectedObjectProperty(
					Node,
					TEXT("ProxyFactoryClass"),
					FactoryFunction->GetOuterUClass());
				SetReflectedObjectProperty(
					Node,
					TEXT("ProxyClass"),
					ProxyClass);
				SetReflectedNameProperty(
					Node,
					TEXT("ProxyActivateFunctionName"),
					GET_FUNCTION_NAME_CHECKED(
						UBlueprintAsyncActionBase,
						Activate));
			});
	}
	else if (NodeType == TEXT("DynamicCast"))
	{
		FString CastTarget = Params->GetStringField(TEXT("castTarget"));
		if (CastTarget.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'castTarget'"));
		UClass* TargetClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == CastTarget || It->GetName() == CastTarget + TEXT("_C"))
			{ TargetClass = *It; break; }
		}
		if (!TargetClass) return FMCPToolResult::Error(FString::Printf(TEXT("Class '%s' not found"), *CastTarget));
		UK2Node_DynamicCast* N = NewObject<UK2Node_DynamicCast>(
			TargetGraph,
			NAME_None,
			RF_Transactional);
		N->Modify();
		N->TargetType = TargetClass; N->NodePosX = PosX; N->NodePosY = PosY;
		TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
	}
	else if (NodeType == TEXT("Comment"))
	{
		FString CommentText = Params->GetStringField(TEXT("comment"));
		if (CommentText.IsEmpty()) CommentText = TEXT("Comment");
		UEdGraphNode_Comment* N =
			NewObject<UEdGraphNode_Comment>(
				TargetGraph,
				NAME_None,
				RF_Transactional);
		N->Modify();
		N->NodeComment = CommentText; N->NodePosX = PosX; N->NodePosY = PosY;
		N->NodeWidth = 400; N->NodeHeight = 200;
		TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
	}
	else if (NodeType == TEXT("Reroute"))
	{
		UK2Node_Knot* N = NewObject<UK2Node_Knot>(
			TargetGraph,
			NAME_None,
			RF_Transactional);
		N->Modify();
		N->NodePosX = PosX; N->NodePosY = PosY;
		TargetGraph->AddNode(N, false, false); N->AllocateDefaultPins(); NewNode = N;
	}
	else
	{
		return FMCPToolResult::Error(FString::Printf(TEXT("Unsupported nodeType '%s'"), *NodeType));
	}

	if (!NewNode)
	{
		return FMCPToolResult::Error(
			TEXT("Failed to create node."),
			TEXT("execution_failed"),
			500);
	}
	if (!NewNode->NodeGuid.IsValid())
	{
		NewNode->CreateNewGuid();
	}

	NewNode->ReconstructNode();
	if (!bDeferred
		&& NodeType != TEXT("Reroute")
		&& HasUnresolvedWildcardPins(NewNode))
	{
		return FMCPToolResult::Error(
			TEXT("Node contains unresolved wildcard pins."),
			TEXT("signature_mismatch"),
			422);
	}
	if (!bDeferred)
	{
		if (UK2Node_CreateDelegate* CreateDelegate =
			Cast<UK2Node_CreateDelegate>(NewNode))
		{
			FCompilerResultsLog DelegateValidationLog;
			DelegateValidationLog.bSilentMode = true;
			CreateDelegate->ValidationAfterFunctionsAreCreated(
				DelegateValidationLog,
				false);
			if (DelegateValidationLog.NumErrors > 0)
			{
				FString DelegateValidationError =
					TEXT("CreateDelegate signature validation failed.");
				for (const TSharedRef<FTokenizedMessage>& Message :
					DelegateValidationLog.Messages)
				{
					if (Message->GetSeverity()
						== EMessageSeverity::Error)
					{
						DelegateValidationError =
							Message->ToText().ToString();
						break;
					}
				}
				return FMCPToolResult::Error(
					DelegateValidationError,
					TEXT("signature_mismatch"),
					422);
			}
		}
	}

	OutNode = NewNode;
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}
} // namespace

class FTool_AddNode : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.node.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString GraphName = Params->GetStringField(TEXT("graph"));
		FString NodeType = Params->GetStringField(TEXT("nodeType"));
		if (BlueprintName.IsEmpty() || GraphName.IsEmpty() || NodeType.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, graph, nodeType"));

		int32 PosX = Params->HasField(TEXT("posX")) ? (int32)Params->GetNumberField(TEXT("posX")) : 0;
		int32 PosY = Params->HasField(TEXT("posY")) ? (int32)Params->GetNumberField(TEXT("posY")) : 0;

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		FString DecodedGraphName = MCPHelpers::UrlDecode(GraphName);
		UEdGraph* TargetGraph = nullptr;
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->GetName().Equals(DecodedGraphName, ESearchCase::IgnoreCase))
			{ TargetGraph = Graph; break; }
		}
		if (!TargetGraph) return FMCPToolResult::Error(FString::Printf(TEXT("Graph '%s' not found"), *DecodedGraphName));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}

		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* ExistingNode : TargetGraph->Nodes)
		{
			if (ExistingNode)
			{
				NodesBefore.Add(ExistingNode);
			}
		}
		UPackage* BlueprintPackage = BP->GetOutermost();
		const bool bPackageWasDirty =
			BlueprintPackage && BlueprintPackage->IsDirty();
		Guard.MarkMutationStarted();
		TargetGraph->Modify();

		const bool bDeferred =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		UEdGraphNode* NewNode = nullptr;
		const FMCPToolResult CreateResult = TryCreateNodeInGraph(
			BP,
			TargetGraph,
			Params,
			PosX,
			PosY,
			bDeferred,
			NewNode);
		if (!CreateResult.bSuccess)
		{
			RollbackAddedGraphNodes(
				TargetGraph,
				NodesBefore,
				BlueprintPackage,
				bPackageWasDirty);
			return CreateResult;
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(
			BP,
			Params);
		bool bCompiled = false;
		bool bSaved = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP,
			Params,
			Guard,
			bSaved,
			bCompiled,
			Failure))
		{
			return Failure;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("graph"), DecodedGraphName);
		Result->SetStringField(TEXT("nodeType"), NodeType);
		Result->SetStringField(TEXT("nodeId"), NewNode->NodeGuid.ToString());
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), bDeferred);
		Result->SetBoolField(TEXT("verified"), bDeferred || bCompiled);
		TSharedPtr<FJsonObject> NodeState = MCPHelpers::SerializeNode(NewNode);
		if (NodeState.IsValid()) Result->SetObjectField(TEXT("node"), NodeState);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// delete_node
// ============================================================
class FTool_DeleteNode : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.node.delete");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString NodeId = Params->GetStringField(TEXT("nodeId"));
		if (BlueprintName.IsEmpty() || NodeId.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing required fields"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId, &Graph);
		if (!Node) return FMCPToolResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

		if (Cast<UK2Node_FunctionEntry>(Node))
			return FMCPToolResult::Error(TEXT("Cannot delete FunctionEntry node"));
		if (Cast<UK2Node_Event>(Node))
			return FMCPToolResult::Error(TEXT("Cannot delete event entry node"));
		if (Cast<UK2Node_CustomEvent>(Node))
			return FMCPToolResult::Error(TEXT("Cannot delete CustomEvent entry node"));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(),
				Guard.GetErrorCode(),
				422);
		}
		Guard.MarkMutationStarted();

		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		Node->Modify();
		Graph->Modify();
		Node->BreakAllNodeLinks();
		Graph->RemoveNode(Node);
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		bool bSaved = false;
		bool bCompiled = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP, Params, Guard, bSaved, bCompiled, Failure))
		{
			return Failure;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// move_node
// ============================================================
class FTool_MoveNode : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.node.move");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString NodeId = Params->GetStringField(TEXT("nodeId"));
		if (BlueprintName.IsEmpty() || NodeId.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing required fields"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId);
		if (!Node) return FMCPToolResult::Error(TEXT("Node not found"));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(),
				Guard.GetErrorCode(),
				422);
		}
		Guard.MarkMutationStarted();

		Node->Modify();
		Node->NodePosX = (int32)Params->GetNumberField(TEXT("posX"));
		Node->NodePosY = (int32)Params->GetNumberField(TEXT("posY"));
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);

		bool bSaved = false;
		bool bCompiled = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP, Params, Guard, bSaved, bCompiled, Failure))
		{
			return Failure;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("posX"), Node->NodePosX);
		Result->SetNumberField(TEXT("posY"), Node->NodePosY);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_pin_default
// ============================================================
class FTool_SetPinDefault : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.pin.default.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString NodeId = Params->GetStringField(TEXT("nodeId"));
		FString PinName;
		FString RequestedPinId;
		Params->TryGetStringField(TEXT("pinName"), PinName);
		Params->TryGetStringField(TEXT("pinId"), RequestedPinId);
		if (BlueprintName.IsEmpty() || NodeId.IsEmpty()
			|| (PinName.IsEmpty() && RequestedPinId.IsEmpty()))
		{
			return FMCPToolResult::Error(
				TEXT("blueprint, nodeId, and either pinId or pinName are required."),
				TEXT("invalid_params"),
				422);
		}
		FGuid ParsedNodeId;
		FGuid ParsedPinId;
		const bool bUsePinId = Params->HasField(TEXT("pinId"));
		if (!FGuid::Parse(NodeId, ParsedNodeId) || !ParsedNodeId.IsValid()
			|| (bUsePinId && (!FGuid::Parse(RequestedPinId, ParsedPinId) || !ParsedPinId.IsValid())))
		{
			return FMCPToolResult::Error(
				TEXT("nodeId and optional pinId must be nonzero GUIDs."),
				TEXT("invalid_params"),
				422);
		}
		FString Value;
		const bool bHasValue =
			Params->TryGetStringField(TEXT("value"), Value);
		const TSharedPtr<FJsonObject>* TypedValueField = nullptr;
		const bool bHasTypedValue = Params->TryGetObjectField(
			TEXT("typedValue"),
			TypedValueField)
			&& TypedValueField && TypedValueField->IsValid();
		if (bHasValue == bHasTypedValue)
		{
			return FMCPToolResult::Error(
				TEXT("Exactly one of value or typedValue is required."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId, &Graph);
		if (!Node) return FMCPToolResult::Error(TEXT("Node not found"));

		bool bAmbiguousPin = false;
		UEdGraphPin* Pin = FindUniquePinByIdentity(
			Node, bUsePinId ? &ParsedPinId : nullptr, FName(*PinName), bAmbiguousPin);
		if (bAmbiguousPin)
		{
			return FMCPToolResult::Error(
				bUsePinId
					? TEXT("The node contains duplicate pin IDs. Reconstruct the node and query blueprint.graph.get again.")
					: TEXT("The node has multiple pins with this name. Use pinId from blueprint.graph.get."),
				TEXT("pin_ambiguous"),
				409);
		}
		if (!Pin)
		{
			return FMCPToolResult::Error(
				TEXT("Pin not found on the selected node. Query blueprint.graph.get for current pin identities."),
				TEXT("pin_not_found"),
				404);
		}
		if (!PinName.IsEmpty() && Pin->PinName != FName(*PinName))
		{
			return FMCPToolResult::Error(
				TEXT("pinId and pinName refer to different pins. Query blueprint.graph.get again."),
				TEXT("pin_identity_mismatch"),
				409);
		}
		PinName = Pin->PinName.ToString();
		const FGuid SelectedPinId = Pin->PinId;
		if (Pin->Direction != EGPD_Input)
			return FMCPToolResult::Error(
				TEXT("Can only set defaults on input pins."),
				TEXT("pin_default_unsupported"),
				422);
		if (!Pin->LinkedTo.IsEmpty())
			return FMCPToolResult::Error(
				TEXT("Linked pins cannot accept a default value."),
				TEXT("pin_linked"),
				409);
		if (Pin->PinType.bIsReference)
			return FMCPToolResult::Error(
				TEXT("By-reference pins cannot accept a persisted default value."),
				TEXT("pin_by_ref"),
				422);
		if (Pin->bDefaultValueIsReadOnly)
			return FMCPToolResult::Error(
				TEXT("The pin default is read-only."),
				TEXT("pin_default_read_only"),
				409);
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			return FMCPToolResult::Error(
				TEXT("Wildcard pins must be resolved before setting a default."),
				TEXT("pin_type_unresolved"),
				422);

		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		if (!Schema)
			return FMCPToolResult::Error(
				TEXT("The graph has no schema for default-value validation."),
				TEXT("pin_default_unsupported"),
				422);

		TSharedPtr<FJsonObject> NormalizedTypedValue;
		FString SerializedValue = Value;
		FString TypedError;
		if (bHasTypedValue
			&& !BuildTypedPinDefault(
				*Pin,
				*TypedValueField,
				SerializedValue,
				NormalizedTypedValue,
				TypedError))
		{
			return FMCPToolResult::Error(
				TypedError,
				TEXT("pin_type_mismatch"),
				422);
		}
		const FString ValidationError = Schema->IsPinDefaultValid(
			Pin,
			SerializedValue,
			nullptr,
			FText::GetEmpty());
		if (!ValidationError.IsEmpty())
		{
			return FMCPToolResult::Error(
				ValidationError,
				TEXT("pin_default_invalid"),
				422);
		}
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard
			SingleRequestGuard(BP);
		if (!SingleRequestGuard.IsValid())
		{
			return FMCPToolResult::Error(
				SingleRequestGuard.GetErrorMessage(),
				SingleRequestGuard.GetErrorCode(),
				422);
		}

		FString OldValue = Pin->DefaultValue;
		UObject* OldDefaultObject = Pin->DefaultObject;
		const FText OldDefaultText = Pin->DefaultTextValue;
		UPackage* Package = BP->GetOutermost();
		const bool bPackageWasDirty = Package && Package->IsDirty();
		auto Rollback = [&]()
		{
			UEdGraphNode* CurrentNode = MCPHelpers::FindNodeByGuid(
				BP,
				NodeId,
				nullptr);
			bool bAmbiguousCurrentPin = false;
			UEdGraphPin* CurrentPin = FindUniquePinByIdentity(
				CurrentNode, &SelectedPinId, NAME_None, bAmbiguousCurrentPin);
			if (CurrentNode && CurrentPin)
			{
				CurrentNode->Modify();
				CurrentPin->DefaultValue = OldValue;
				CurrentPin->DefaultObject = OldDefaultObject;
				CurrentPin->DefaultTextValue = OldDefaultText;
				CurrentNode->PinDefaultValueChanged(CurrentPin);
			}
			FKismetEditorUtilities::CompileBlueprint(
				BP,
				EBlueprintCompileOptions::SkipSave);
			if (Package)
			{
				Package->SetDirtyFlag(bPackageWasDirty);
			}
		};
		SingleRequestGuard.MarkMutationStarted();
		Node->Modify();
		Schema->TrySetDefaultValue(
			*Pin, SerializedValue,
			!UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		FString ImmediateNormalized;
		UScriptStruct* PinStruct = Cast<UScriptStruct>(
			Pin->PinType.PinSubCategoryObject.Get());
		const bool bImmediateMatches = bHasTypedValue
			? NormalizePinStructText(
				PinStruct,
				Pin->DefaultValue,
				ImmediateNormalized)
				&& ImmediateNormalized == SerializedValue
			: Schema->DoesDefaultValueMatch(*Pin, SerializedValue);
		if (!bImmediateMatches
			|| !Schema->IsCurrentPinDefaultValid(Pin).IsEmpty())
		{
			Rollback();
			return FMCPToolResult::Error(
				TEXT("The graph schema rejected or changed the requested pin default."),
				TEXT("pin_default_persistence_failed"),
				500);
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		const bool bDeferredWorkflow =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		if (!bDeferredWorkflow)
		{
			FKismetEditorUtilities::CompileBlueprint(
				BP,
				EBlueprintCompileOptions::SkipSave);
			if (BP->Status == BS_Error)
			{
				Rollback();
				return FMCPToolResult::Error(
					TEXT("Pin default caused Blueprint compile errors and was rolled back."),
					TEXT("asset_compile_failed"),
					500);
			}
		}
		UEdGraphNode* ReadBackNode = MCPHelpers::FindNodeByGuid(
			BP,
			NodeId,
			nullptr);
		bool bAmbiguousReadBackPin = false;
		UEdGraphPin* ReadBackPin = FindUniquePinByIdentity(
			ReadBackNode, &SelectedPinId, NAME_None, bAmbiguousReadBackPin);
		FString ReadBackNormalized;
		const bool bReadBackMatches = ReadBackPin && (bHasTypedValue
			? NormalizePinStructText(
				PinStruct,
				ReadBackPin->DefaultValue,
				ReadBackNormalized)
				&& ReadBackNormalized == SerializedValue
			: Schema->DoesDefaultValueMatch(*ReadBackPin, SerializedValue));
		if (!bReadBackMatches)
		{
			Rollback();
			return FMCPToolResult::Error(
				TEXT("Pin default did not survive reconstruction or compilation."),
				TEXT("pin_default_persistence_failed"),
				500);
		}
		const bool bSaved = !bDeferredWorkflow
			&& MCPHelpers::SaveBlueprintPackage(BP);
		if (!bDeferredWorkflow && !bSaved)
		{
			Rollback();
			return FMCPToolResult::Error(
				TEXT("Pin default could not be saved and was rolled back."),
				TEXT("asset_save_failed"),
				500);
		}
		SingleRequestGuard.Commit();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("oldValue"), OldValue);
		Result->SetObjectField(TEXT("pinType"), MakePinTypeResult(*ReadBackPin));
		if (NormalizedTypedValue.IsValid())
		{
			Result->SetObjectField(
				TEXT("normalizedValue"),
				NormalizedTypedValue);
		}
		else
		{
			Result->SetStringField(TEXT("normalizedValue"), ReadBackPin->DefaultValue);
		}
		Result->SetStringField(TEXT("serializedValue"), ReadBackPin->DefaultValue);
		TSharedPtr<FJsonObject> ReadBack = MakeShared<FJsonObject>();
		ReadBack->SetStringField(TEXT("nodeId"), NodeId);
		ReadBack->SetStringField(TEXT("pinName"), ReadBackPin->PinName.ToString());
		ReadBack->SetStringField(TEXT("pinId"), ReadBackPin->PinId.ToString());
		ReadBack->SetStringField(TEXT("serializedValue"), ReadBackPin->DefaultValue);
		Result->SetObjectField(TEXT("readBack"), ReadBack);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("deferredCompile"), bDeferredWorkflow);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// duplicate_nodes
// ============================================================
class FTool_DuplicateNodes : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.node.duplicate");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString BlueprintName =
			Params->GetStringField(TEXT("blueprint"));
		const TArray<TSharedPtr<FJsonValue>>* NodeIdValues = nullptr;
		if (BlueprintName.IsEmpty()
			|| !Params->TryGetArrayField(TEXT("nodeIds"), NodeIdValues)
			|| !NodeIdValues || NodeIdValues->IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and a non-empty nodeIds array are required."),
				TEXT("invalid_params"),
				422);
		}
		if (NodeIdValues->Num() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("nodeIds is limited to 256 nodes per request."),
				TEXT("request_too_large"),
				422);
		}

		auto ReadOffset = [&Params](
			const TCHAR* Field,
			int32& OutValue,
			FString& OutError) -> bool
		{
			OutValue = 0;
			if (!Params->HasField(Field))
			{
				return true;
			}
			double Number = 0.0;
			if (!Params->TryGetNumberField(Field, Number)
				|| !FMath::IsFinite(Number)
				|| Number != FMath::FloorToDouble(Number)
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				OutError = FString::Printf(
					TEXT("%s must be a finite 32-bit integer."),
					Field);
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		};

		int32 OffsetX = 0;
		int32 OffsetY = 0;
		FString OffsetError;
		if (!ReadOffset(TEXT("offsetX"), OffsetX, OffsetError)
			|| !ReadOffset(TEXT("offsetY"), OffsetY, OffsetError))
		{
			return FMCPToolResult::Error(
				OffsetError,
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

		UEdGraph* SourceGraph = nullptr;
		TSet<FGuid> RequestedIds;
		TMap<FGuid, UEdGraphNode*> SourceNodesById;
		TSet<UObject*> NodesToExport;
		for (int32 Index = 0; Index < NodeIdValues->Num(); ++Index)
		{
			FString NodeIdText;
			FGuid NodeId;
			if (!(*NodeIdValues)[Index].IsValid()
				|| !(*NodeIdValues)[Index]->TryGetString(NodeIdText)
				|| !FGuid::Parse(NodeIdText, NodeId)
				|| !NodeId.IsValid())
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("nodeIds[%d] must be a nonzero node GUID."),
						Index),
					TEXT("invalid_params"),
					422);
			}
			if (RequestedIds.Contains(NodeId))
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("nodeIds contains duplicate GUID '%s'."),
						*NodeId.ToString()),
					TEXT("invalid_params"),
					422);
			}

			UEdGraph* NodeGraph = nullptr;
			UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(
				BP,
				NodeIdText,
				&NodeGraph);
			if (!Node || !NodeGraph)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Node '%s' was not found."),
						*NodeIdText),
					TEXT("node_not_found"),
					404);
			}
			if (!Node->CanDuplicateNode())
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Node '%s' cannot be duplicated by its graph schema."),
						*NodeIdText),
					TEXT("node_not_duplicable"),
					422);
			}
			if (SourceGraph && NodeGraph != SourceGraph)
			{
				return FMCPToolResult::Error(
					TEXT("All nodes in one duplicate request must belong to the same graph."),
					TEXT("graph_scope_mismatch"),
					422);
			}

			SourceGraph = NodeGraph;
			RequestedIds.Add(NodeId);
			SourceNodesById.Add(NodeId, Node);
			NodesToExport.Add(Node);
		}

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard
			Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		Guard.MarkMutationStarted();
		SourceGraph->Modify();

		FString ExportedText;
		FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);
		if (ExportedText.IsEmpty())
		{
			return RollbackDirectMutation(
				Guard,
				TEXT("The selected nodes could not be serialized for duplication."),
				TEXT("node_duplicate_export_failed"));
		}

		TSet<UEdGraphNode*> ImportedNodes;
		FEdGraphUtilities::ImportNodesFromText(
			SourceGraph,
			ExportedText,
			ImportedNodes);
		if (ImportedNodes.Num() != SourceNodesById.Num())
		{
			return RollbackDirectMutation(
				Guard,
				FString::Printf(
					TEXT("Expected %d duplicated nodes but Unreal imported %d."),
					SourceNodesById.Num(),
					ImportedNodes.Num()),
				TEXT("node_duplicate_import_failed"));
		}

		struct FDuplicateRecord
		{
			FGuid SourceId;
			FGuid DuplicateId;
			UEdGraphNode* Duplicate = nullptr;
		};
		TArray<FDuplicateRecord> Records;
		Records.Reserve(ImportedNodes.Num());
		bool bStructurallyModified = false;
		for (UEdGraphNode* ImportedNode : ImportedNodes)
		{
			if (!ImportedNode
				|| !ImportedNode->NodeGuid.IsValid()
				|| !SourceNodesById.Contains(ImportedNode->NodeGuid))
			{
				return RollbackDirectMutation(
					Guard,
					TEXT("An imported node did not retain a source identity."),
					TEXT("node_duplicate_identity_failed"));
			}

			const FGuid SourceId = ImportedNode->NodeGuid;
			ImportedNode->Modify();
			ImportedNode->NodePosX += OffsetX;
			ImportedNode->NodePosY += OffsetY;
			ImportedNode->CreateNewGuid();
			if (!ImportedNode->NodeGuid.IsValid()
				|| RequestedIds.Contains(ImportedNode->NodeGuid))
			{
				return RollbackDirectMutation(
					Guard,
					TEXT("Unreal did not assign a unique GUID to a duplicated node."),
					TEXT("node_duplicate_identity_failed"));
			}

			if (const UK2Node* K2Node = Cast<UK2Node>(ImportedNode))
			{
				bStructurallyModified |=
					K2Node->NodeCausesStructuralBlueprintChange();
			}
			Records.Add({SourceId, ImportedNode->NodeGuid, ImportedNode});
		}

		Records.Sort([](const FDuplicateRecord& Left, const FDuplicateRecord& Right)
		{
			return Left.SourceId.ToString() < Right.SourceId.ToString();
		});
		UEAIIntegration::Workflow::MarkBlueprintChanged(
			BP,
			Params,
			bStructurallyModified);

		bool bSaved = false;
		bool bCompiled = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP, Params, Guard, bSaved, bCompiled, Failure))
		{
			return Failure;
		}

		TArray<TSharedPtr<FJsonValue>> MappingValues;
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		for (const FDuplicateRecord& Record : Records)
		{
			UEdGraphNode* ReadBack = MCPHelpers::FindNodeByGuid(
				BP,
				Record.DuplicateId.ToString());
			if (!ReadBack || ReadBack->GetGraph() != SourceGraph)
			{
				return FMCPToolResult::Error(
					TEXT("A duplicated node was not present during read-back."),
					TEXT("node_duplicate_readback_failed"),
					500);
			}

			TSharedRef<FJsonObject> Mapping = MakeShared<FJsonObject>();
			Mapping->SetStringField(
				TEXT("sourceNodeId"), Record.SourceId.ToString());
			Mapping->SetStringField(
				TEXT("duplicateNodeId"), Record.DuplicateId.ToString());
			MappingValues.Add(MakeShared<FJsonValueObject>(Mapping));
			if (TSharedPtr<FJsonObject> NodeState =
				MCPHelpers::SerializeNode(ReadBack))
			{
				NodeValues.Add(
					MakeShared<FJsonValueObject>(NodeState.ToSharedRef()));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("graph"), SourceGraph->GetName());
		Result->SetNumberField(TEXT("duplicatedCount"), Records.Num());
		Result->SetNumberField(TEXT("offsetX"), OffsetX);
		Result->SetNumberField(TEXT("offsetY"), OffsetY);
		Result->SetArrayField(TEXT("nodeIdMappings"), MappingValues);
		Result->SetArrayField(TEXT("nodes"), NodeValues);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(
			TEXT("deferredCompile"),
			UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_node_comment
// ============================================================
class FTool_SetCommentTitle : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.comment.title.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString NodeId = Params->GetStringField(TEXT("commentNodeId"));
		FString Title = Params->GetStringField(TEXT("title"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraphNode_Comment* Node = Cast<UEdGraphNode_Comment>(
			MCPHelpers::FindNodeByGuid(BP, NodeId));
		if (!Node) return FMCPToolResult::Error(TEXT("Comment node not found"));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		Guard.MarkMutationStarted();

		FString OldComment = Node->NodeComment;
		Node->Modify();
		Node->NodeComment = Title;
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);

		bool bSaved = false;
		bool bCompiled = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP, Params, Guard, bSaved, bCompiled, Failure))
		{
			return Failure;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("oldTitle"), OldComment);
		Result->SetStringField(TEXT("newTitle"), Title);
		Result->SetBoolField(TEXT("bubbleVisible"), Node->bCommentBubbleVisible);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_SetCommentBubble : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.comment.bubble.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		const FString NodeId = Params->GetStringField(TEXT("commentNodeId"));
		bool bVisible = false;
		if (!Params->TryGetBoolField(TEXT("visible"), bVisible))
		{
			return FMCPToolResult::Error(TEXT("visible is required"), TEXT("invalid_params"), 422);
		}
		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);
		UEdGraphNode_Comment* Node = Cast<UEdGraphNode_Comment>(
			MCPHelpers::FindNodeByGuid(BP, NodeId));
		if (!Node) return FMCPToolResult::Error(TEXT("Comment node not found"));
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		Guard.MarkMutationStarted();
		const bool bOldVisible = Node->bCommentBubbleVisible;
		Node->Modify();
		Node->bCommentBubbleVisible = bVisible;
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		bool bSaved = false;
		bool bCompiled = false;
		FMCPToolResult Failure;
		if (!FinalizeDirectMutation(
			BP, Params, Guard, bSaved, bCompiled, Failure))
		{
			return Failure;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("oldVisible"), bOldVisible);
		Result->SetBoolField(TEXT("visible"), bVisible);
		Result->SetStringField(TEXT("title"), Node->NodeComment);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// refresh_all_nodes
// ============================================================
class FTool_RefreshAllNodes : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.node.refresh_all");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		if (BlueprintName.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing 'blueprint'"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		FBlueprintEditorUtils::RefreshAllNodes(BP);

		// Remove orphaned pins
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		int32 OrphanedPinsRemoved = 0;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				for (int32 i = Node->Pins.Num() - 1; i >= 0; --i)
				{
					if (Node->Pins[i] && Node->Pins[i]->bOrphanedPin)
					{
						Node->Pins[i]->BreakAllPinLinks();
						Node->Pins.RemoveAt(i);
						OrphanedPinsRemoved++;
					}
				}
			}
		}

		bool bSaved = MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetNumberField(TEXT("orphanedPinsRemoved"), OrphanedPinsRemoved);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// rename_asset
// ============================================================
class FTool_RenameAsset : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.rename");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath = Params->GetStringField(TEXT("assetPath"));
		FString NewPath = Params->GetStringField(TEXT("newPath"));
		if (AssetPath.IsEmpty() || NewPath.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Missing required fields: assetPath, newPath"),
				TEXT("invalid_params"),
				422);
		}
		if (NewPath.EndsWith(TEXT("/")) || NewPath.Contains(TEXT("..")))
		{
			return FMCPToolResult::Error(
				TEXT("newPath must identify one asset and may not contain '..'."),
				TEXT("invalid_asset_path"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(AssetPath, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("asset_not_found"),
				404);
		}
		const FString SourcePackageName = BP->GetOutermost()->GetName();
		const FString SourceObjectPath = BP->GetPathName();

		FString DestinationPackageName = NewPath;
		if (DestinationPackageName.Contains(TEXT(".")))
		{
			DestinationPackageName =
				FPackageName::ObjectPathToPackageName(DestinationPackageName);
		}
		if (!DestinationPackageName.StartsWith(TEXT("/")))
		{
			DestinationPackageName =
				FPackageName::GetLongPackagePath(SourcePackageName)
				+ TEXT("/") + DestinationPackageName;
		}
		if (!FPackageName::IsValidLongPackageName(DestinationPackageName))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("newPath '%s' is not a valid long package name."),
					*NewPath),
				TEXT("invalid_asset_path"),
				422);
		}

		const FString DestinationAssetName =
			FPackageName::GetLongPackageAssetName(DestinationPackageName);
		FText InvalidNameReason;
		if (DestinationAssetName.IsEmpty()
			|| !FName(*DestinationAssetName).IsValidObjectName(InvalidNameReason))
		{
			return FMCPToolResult::Error(
				InvalidNameReason.IsEmpty()
					? TEXT("newPath has an invalid asset name.")
					: InvalidNameReason.ToString(),
				TEXT("invalid_asset_path"),
				422);
		}

		const FString DestinationObjectPath = FString::Printf(
			TEXT("%s.%s"),
			*DestinationPackageName,
			*DestinationAssetName);
		if (SourcePackageName.Equals(
				DestinationPackageName,
				ESearchCase::CaseSensitive))
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetBoolField(TEXT("renamed"), false);
			Result->SetBoolField(TEXT("verified"), true);
			Result->SetStringField(TEXT("oldPath"), SourceObjectPath);
			Result->SetStringField(TEXT("newPath"), SourceObjectPath);
			Result->SetStringField(TEXT("packagePath"), SourcePackageName);
			return FMCPToolResult::Ok(Result);
		}

		IAssetRegistry& AssetRegistry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")).Get();
		if (FPackageName::DoesPackageExist(DestinationPackageName)
			|| AssetRegistry.GetAssetByObjectPath(
				FSoftObjectPath(DestinationObjectPath)).IsValid())
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("An asset already exists at '%s'."),
					*DestinationPackageName),
				TEXT("asset_already_exists"),
				409);
		}

		FAssetToolsModule& AssetToolsModule =
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(
				TEXT("AssetTools"));
		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(
			BP,
			FPackageName::GetLongPackagePath(DestinationPackageName),
			DestinationAssetName);
		const bool bRenameReportedSuccess =
			AssetToolsModule.Get().RenameAssets(RenameData);
		const FString ActualPackageName = BP->GetOutermost()->GetName();
		const FString ActualObjectPath = BP->GetPathName();
		const bool bVerified = bRenameReportedSuccess
			&& ActualPackageName.Equals(
				DestinationPackageName,
				ESearchCase::CaseSensitive)
			&& ActualObjectPath.Equals(
				DestinationObjectPath,
				ESearchCase::CaseSensitive);
		if (!bVerified)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Asset rename was not verified. Current path: '%s'."),
					*ActualObjectPath),
				bRenameReportedSuccess
					? TEXT("asset_rename_verification_failed")
					: TEXT("asset_rename_failed"),
				500);
		}

		const FAssetData SourcePathState =
			AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourceObjectPath));
		if (!SourcePathState.IsValid() || !SourcePathState.IsRedirector())
		{
			FString RedirectorError;
			if (!EnsureBlueprintRenameRedirector(
					BP,
					SourcePackageName,
					FPackageName::GetLongPackageAssetName(SourcePackageName),
					DestinationPackageName,
					DestinationAssetName,
					AssetRegistry,
					RedirectorError))
			{
				return FMCPToolResult::Error(
					RedirectorError,
					TEXT("asset_rename_redirector_failed"),
					500);
			}
		}
		const FAssetData PersistedSourcePathState =
			AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourceObjectPath));
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("renamed"), true);
		Result->SetBoolField(TEXT("verified"), true);
		Result->SetStringField(TEXT("oldPath"), SourceObjectPath);
		Result->SetStringField(TEXT("newPath"), ActualObjectPath);
		Result->SetStringField(TEXT("packagePath"), ActualPackageName);
		Result->SetBoolField(
			TEXT("redirectorCreated"),
			PersistedSourcePathState.IsValid()
				&& PersistedSourcePathState.IsRedirector());
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_blueprint_default
// ============================================================
class FTool_SetBlueprintDefault : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.default.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName;
		FString VariableName;
		FString Value;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(
				TEXT("blueprint"), BlueprintName)
			|| !Params->TryGetStringField(
				TEXT("variable"), VariableName)
			|| !Params->TryGetStringField(TEXT("value"), Value)
			|| BlueprintName.IsEmpty()
			|| VariableName.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("blueprint, variable, and value are required."),
				TEXT("invalid_params"),
				422);
		}
		if (BlueprintName.Len() > 1024
			|| VariableName.Len() > 256
			|| Value.Len() > 65536)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint may contain at most 1024 characters, variable 256, and value 65536."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("asset_not_found"),
				404);
		}

		if (!BP->GeneratedClass)
		{
			return FMCPToolResult::Error(
				TEXT("Blueprint has no generated class."),
				TEXT("generated_class_unavailable"),
				422);
		}

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO)
		{
			return FMCPToolResult::Error(
				TEXT("Could not resolve the Blueprint class default object."),
				TEXT("cdo_unavailable"),
				422);
		}

		FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*VariableName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(BP->GeneratedClass); It; ++It)
			{
				if (It->GetName().Equals(
						VariableName,
						ESearchCase::IgnoreCase))
				{
					Prop = *It;
					break;
				}
			}
		}
		if (!Prop)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Property '%s' was not found on '%s'."),
					*VariableName,
					*BP->GeneratedClass->GetName()),
				TEXT("property_not_found"),
				404);
		}
		if (Prop->HasAnyPropertyFlags(
			CPF_EditConst | CPF_Transient | CPF_DuplicateTransient
			| CPF_NonPIEDuplicateTransient))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Property '%s' is read-only or non-persistent."),
					*Prop->GetName()),
				TEXT("property_not_writable"),
				422);
		}

		const FName PropertyName = Prop->GetFName();
		auto ExportPropertyValue = [PropertyName](
			UObject* Object,
			FString& OutValue) -> bool
		{
			if (!Object)
			{
				return false;
			}
			FProperty* CurrentProperty =
				Object->GetClass()->FindPropertyByName(PropertyName);
			if (!CurrentProperty)
			{
				return false;
			}
			void* Address =
				CurrentProperty->ContainerPtrToValuePtr<void>(Object);
			OutValue.Reset();
			CurrentProperty->ExportText_Direct(
				OutValue,
				Address,
				Address,
				Object,
				PPF_None);
			return true;
		};
		auto ImportPropertyValue = [PropertyName](
			UObject* Object,
			const FString& SerializedValue,
			FString& OutNormalizedValue) -> bool
		{
			if (!Object)
			{
				return false;
			}
			FProperty* CurrentProperty =
				Object->GetClass()->FindPropertyByName(PropertyName);
			if (!CurrentProperty)
			{
				return false;
			}
			return ImportBlueprintPropertyText(
				Object,
				CurrentProperty,
				SerializedValue,
				true,
				OutNormalizedValue);
		};

		FString OldValue;
		if (!ExportPropertyValue(CDO, OldValue))
		{
			return FMCPToolResult::Error(
				TEXT("Could not serialize the current CDO property value."),
				TEXT("property_read_failed"),
				500);
		}
		FString ExpectedValue;
		if (!ImportBlueprintPropertyText(
			CDO,
			Prop,
			Value,
			false,
			ExpectedValue))
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
		Guard.MarkMutationStarted();
		BP->Modify();

		auto ResolveCurrentCDO = [BP]() -> UObject*
		{
			return BP && BP->GeneratedClass
				? BP->GeneratedClass->GetDefaultObject()
				: nullptr;
		};
		auto RestoreOldValue = [&]() -> bool
		{
			FString Ignored;
			return ImportPropertyValue(
				ResolveCurrentCDO(),
				OldValue,
				Ignored);
		};
		auto FailAndRollback = [&](
			const FString& Message,
			const FString& Code) -> FMCPToolResult
		{
			const bool bValueRestored = RestoreOldValue();
			FString RollbackError;
			const bool bGuardRestored = Guard.Rollback(RollbackError);
			if (!bValueRestored || !bGuardRestored)
			{
				return FMCPToolResult::Error(
					Message + TEXT(" The previous default could not be fully restored. ")
					+ RollbackError,
					TEXT("rollback_failed"),
					500);
			}
			return FMCPToolResult::Error(
				Message + TEXT(" The mutation was rolled back and verified."),
				Code,
				500);
		};

		FString AppliedValue;
		if (!ImportPropertyValue(CDO, Value, AppliedValue)
			|| AppliedValue != ExpectedValue)
		{
			return FailAndRollback(
				FString::Printf(
					TEXT("Property '%s' rejected the supplied Unreal text value."),
					*PropertyName.ToString()),
				TEXT("property_value_invalid"));
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
					TEXT("The CDO property change introduced Blueprint compile errors."),
					TEXT("asset_compile_failed"));
			}
		}

		FString ReadBackValue;
		if (!ExportPropertyValue(ResolveCurrentCDO(), ReadBackValue)
			|| ReadBackValue != ExpectedValue)
		{
			return FailAndRollback(
				TEXT("The CDO property value did not survive read-back."),
				TEXT("property_persistence_failed"));
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
						? TEXT("The CDO property change could not be saved.")
						: SaveError.Message,
					SaveError.Code.IsEmpty()
						? TEXT("asset_save_failed")
						: SaveError.Code);
			}
		}
		Guard.Commit();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("variable"), PropertyName.ToString());
		Result->SetStringField(TEXT("oldValue"), OldValue);
		Result->SetStringField(TEXT("newValue"), ReadBackValue);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), bDeferred);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Bulk graph authoring + pin promotion helpers
// ============================================================
namespace
{
UEdGraph* ResolveTargetGraph(UBlueprint* Blueprint, const FString& GraphName)
{
	if (!Blueprint)
	{
		return nullptr;
	}
	if (GraphName.IsEmpty())
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				return Graph;
			}
		}
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph)
			{
				return Graph;
			}
		}
		return nullptr;
	}
	const FString DecodedGraphName = MCPHelpers::UrlDecode(GraphName);
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph && Graph->GetName().Equals(DecodedGraphName, ESearchCase::IgnoreCase))
		{
			return Graph;
		}
	}
	return nullptr;
}

// Compiles, saves, and commits a guarded mutation while keeping the surrounding
// FScopedTransaction alive so a compile/save failure can still cancel it before
// the deep snapshot rollback runs.  Reuses the same persistence + rollback
// primitives as FinalizeDirectMutation.
FMCPToolResult FinalizeGuardedMutation(
	UBlueprint* Blueprint,
	const TSharedPtr<FJsonObject>& Params,
	UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard& Guard,
	FScopedTransaction& Transaction,
	bool& OutSaved,
	bool& OutCompiled)
{
	OutSaved = false;
	OutCompiled = false;
	if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
	{
		Guard.Commit();
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}
	FKismetEditorUtilities::CompileBlueprint(
		Blueprint,
		EBlueprintCompileOptions::SkipSave);
	OutCompiled = Blueprint->Status != BS_Error;
	if (!OutCompiled)
	{
		Transaction.Cancel();
		return RollbackDirectMutation(
			Guard,
			TEXT("The Blueprint mutation introduced compile errors."),
			TEXT("asset_compile_failed"));
	}
	UEAIIntegration::Infrastructure::FBlueprintPersistenceError SaveError;
	OutSaved = UEAIIntegration::Infrastructure::SaveBlueprintPackage(
		Blueprint,
		nullptr,
		SaveError);
	if (!OutSaved)
	{
		Transaction.Cancel();
		return RollbackDirectMutation(
			Guard,
			SaveError.Message.IsEmpty()
				? TEXT("The Blueprint mutation could not be saved.")
				: SaveError.Message,
			SaveError.Code.IsEmpty()
				? TEXT("asset_save_failed")
				: SaveError.Code);
	}
	Guard.Commit();
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

// Validates and applies one unlinked input-pin default.  Mirrors the validation
// and persistence primitives of blueprint.pin.default.set; the caller owns the
// transaction, compile, and read-back.
FMCPToolResult TrySetSinglePinDefault(
	UEdGraphNode* Node,
	UEdGraphPin* Pin,
	const UEdGraphSchema* Schema,
	const FString& Value,
	const bool bDeferred)
{
	if (!Node || !Pin || !Schema)
	{
		return FMCPToolResult::Error(
			TEXT("The pin default target could not be resolved."),
			TEXT("pin_not_found"),
			404);
	}
	if (Pin->Direction != EGPD_Input)
	{
		return FMCPToolResult::Error(
			TEXT("Can only set defaults on input pins."),
			TEXT("pin_default_unsupported"),
			422);
	}
	if (!Pin->LinkedTo.IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("Linked pins cannot accept a default value."),
			TEXT("pin_linked"),
			409);
	}
	if (Pin->PinType.bIsReference)
	{
		return FMCPToolResult::Error(
			TEXT("By-reference pins cannot accept a persisted default value."),
			TEXT("pin_by_ref"),
			422);
	}
	if (Pin->bDefaultValueIsReadOnly)
	{
		return FMCPToolResult::Error(
			TEXT("The pin default is read-only."),
			TEXT("pin_default_read_only"),
			409);
	}
	if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		return FMCPToolResult::Error(
			TEXT("Wildcard pins must be resolved before setting a default."),
			TEXT("pin_type_unresolved"),
			422);
	}
	const FString ValidationError = Schema->IsPinDefaultValid(
		Pin,
		Value,
		nullptr,
		FText::GetEmpty());
	if (!ValidationError.IsEmpty())
	{
		return FMCPToolResult::Error(
			ValidationError,
			TEXT("pin_default_invalid"),
			422);
	}
	Node->Modify();
	Schema->TrySetDefaultValue(*Pin, Value, !bDeferred);
	if (!Schema->DoesDefaultValueMatch(*Pin, Value)
		|| !Schema->IsCurrentPinDefaultValid(Pin).IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("The graph schema rejected or changed the requested pin default."),
			TEXT("pin_default_persistence_failed"),
			500);
	}
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}
} // namespace

// ============================================================
// promote_pin
// ============================================================
class FTool_PromotePin : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.pin.promote");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		const FString NodeId = Params->GetStringField(TEXT("nodeId"));
		FString PinName;
		FString RequestedPinId;
		Params->TryGetStringField(TEXT("pinName"), PinName);
		Params->TryGetStringField(TEXT("pinId"), RequestedPinId);
		if (BlueprintName.IsEmpty() || NodeId.IsEmpty()
			|| (PinName.IsEmpty() && RequestedPinId.IsEmpty()))
		{
			return FMCPToolResult::Error(
				TEXT("blueprint, nodeId, and either pinId or pinName are required."),
				TEXT("invalid_params"),
				422);
		}
		FGuid ParsedNodeId;
		FGuid ParsedPinId;
		const bool bUsePinId = Params->HasField(TEXT("pinId"));
		if (!FGuid::Parse(NodeId, ParsedNodeId) || !ParsedNodeId.IsValid()
			|| (bUsePinId && (!FGuid::Parse(RequestedPinId, ParsedPinId) || !ParsedPinId.IsValid())))
		{
			return FMCPToolResult::Error(
				TEXT("nodeId and optional pinId must be nonzero GUIDs."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);

		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId, &Graph);
		if (!Node) return FMCPToolResult::Error(TEXT("Node not found"), TEXT("node_not_found"), 404);
		if (!Graph) return FMCPToolResult::Error(TEXT("The node's graph could not be resolved."), TEXT("graph_not_found"), 404);

		bool bAmbiguousPin = false;
		UEdGraphPin* Pin = FindUniquePinByIdentity(
			Node, bUsePinId ? &ParsedPinId : nullptr, FName(*PinName), bAmbiguousPin);
		if (bAmbiguousPin)
		{
			return FMCPToolResult::Error(
				bUsePinId
					? TEXT("The node contains duplicate pin IDs. Reconstruct the node and query blueprint.graph.get again.")
					: TEXT("The node has multiple pins with this name. Use pinId from blueprint.graph.get."),
				TEXT("pin_ambiguous"),
				409);
		}
		if (!Pin)
		{
			return FMCPToolResult::Error(
				TEXT("Pin not found on the selected node. Query blueprint.graph.get for current pin identities."),
				TEXT("pin_not_found"),
				404);
		}
		if (!PinName.IsEmpty() && Pin->PinName != FName(*PinName))
		{
			return FMCPToolResult::Error(
				TEXT("pinId and pinName refer to different pins. Query blueprint.graph.get again."),
				TEXT("pin_identity_mismatch"),
				409);
		}
		PinName = Pin->PinName.ToString();

		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			return FMCPToolResult::Error(
				TEXT("Cannot promote execution (exec) pins to variables."),
				TEXT("pin_promote_unsupported"),
				422);
		if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			return FMCPToolResult::Error(
				TEXT("Cannot promote wildcard pins to variables — resolve the type first."),
				TEXT("pin_type_unresolved"),
				422);
		if (Pin->PinType.ContainerType != EPinContainerType::None)
			return FMCPToolResult::Error(
				TEXT("Container types (Array, Map, Set) are not supported by pin promotion."),
				TEXT("pin_container_unsupported"),
				422);

		FString VariableName;
		if (!Params->TryGetStringField(TEXT("variableName"), VariableName)
			|| VariableName.IsEmpty())
		{
			VariableName = PinName;
		}
		for (const FBPVariableDescription& Existing : BP->NewVariables)
		{
			if (Existing.VarName == FName(*VariableName))
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("A variable named '%s' already exists in this Blueprint."), *VariableName),
					TEXT("variable_exists"),
					409);
			}
		}

		const FGuid SelectedPinId = Pin->PinId;
		TArray<UEdGraphPin*> LinkedPins;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked) LinkedPins.Add(Linked);
		}

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "PromotePin", "Promote Pin To Variable"));
		Guard.MarkMutationStarted();
		BP->Modify();
		Graph->Modify();

		// Match the native promote semantics: strip const/ref/weak flags before
		// the member variable is created.
		FEdGraphPinType VariableType = Pin->PinType;
		VariableType.bIsConst = false;
		VariableType.bIsReference = false;
		VariableType.bIsWeakPointer = false;
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, FName(*VariableName), VariableType))
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				FString::Printf(TEXT("Failed to add variable '%s' — a variable with that name may already exist."), *VariableName),
				TEXT("variable_add_failed"));
		}

		// Adding a variable can regenerate the skeleton and reconstruct nodes,
		// so re-resolve the source pin before rewiring.
		UEdGraphNode* SourceNode = MCPHelpers::FindNodeByGuid(BP, NodeId, nullptr);
		bool bAmbiguousNow = false;
		UEdGraphPin* SourcePin = FindUniquePinByIdentity(
			SourceNode, &SelectedPinId, NAME_None, bAmbiguousNow);
		if (!SourcePin && !PinName.IsEmpty())
		{
			SourcePin = FindUniquePinByIdentity(
				SourceNode, nullptr, FName(*PinName), bAmbiguousNow);
		}
		if (!SourceNode || !SourcePin || bAmbiguousNow)
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("The promoted pin could not be re-resolved after adding the variable."),
				TEXT("pin_resolution_failed"));
		}

		const EEdGraphPinDirection PinDir = SourcePin->Direction;
		const int32 AccessorPosX = SourceNode->NodePosX + (PinDir == EGPD_Output ? 200 : -200);
		const int32 AccessorPosY = SourceNode->NodePosY;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		UEdGraphNode* AccessorNode = nullptr;
		UEdGraphPin* RewiredPin = nullptr;
		int32 ConnectionsMade = 0;
		if (PinDir == EGPD_Output)
		{
			UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(
				Graph, NAME_None, RF_Transactional);
			GetNode->Modify();
			GetNode->VariableReference.SetSelfMember(FName(*VariableName));
			GetNode->NodePosX = AccessorPosX; GetNode->NodePosY = AccessorPosY;
			Graph->AddNode(GetNode, false, false);
			GetNode->AllocateDefaultPins();
			MaterializeDeferredVariablePins(BP, GetNode, false);
			AccessorNode = GetNode;
		}
		else
		{
			UK2Node_VariableSet* SetNode = NewObject<UK2Node_VariableSet>(
				Graph, NAME_None, RF_Transactional);
			SetNode->Modify();
			SetNode->VariableReference.SetSelfMember(FName(*VariableName));
			SetNode->NodePosX = AccessorPosX; SetNode->NodePosY = AccessorPosY;
			Graph->AddNode(SetNode, false, false);
			SetNode->AllocateDefaultPins();
			MaterializeDeferredVariablePins(BP, SetNode, true);
			AccessorNode = SetNode;
		}
		if (!AccessorNode)
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("Failed to create the variable accessor node."),
				TEXT("execution_failed"));
		}
		if (!AccessorNode->NodeGuid.IsValid())
		{
			AccessorNode->CreateNewGuid();
		}
		for (UEdGraphPin* P : AccessorNode->Pins)
		{
			if (P && P->Direction == (PinDir == EGPD_Output ? EGPD_Output : EGPD_Input)
				&& P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				RewiredPin = P;
				break;
			}
		}

		SourcePin->BreakAllPinLinks(true);
		if (RewiredPin && Schema)
		{
			const bool bDeferred = UEAIIntegration::Workflow::ShouldDeferCompile(Params);
			for (UEdGraphPin* Other : LinkedPins)
			{
				const bool bMade = PinDir == EGPD_Output
					? UEAIIntegration::Infrastructure::TryCreateConnection(Schema, RewiredPin, Other, bDeferred)
					: UEAIIntegration::Infrastructure::TryCreateConnection(Schema, Other, RewiredPin, bDeferred);
				if (bMade)
				{
					ConnectionsMade++;
				}
			}
		}

		if (ConnectionsMade != LinkedPins.Num())
		{
			// A partial re-wire would silently drop a prior connection; roll
			// back instead of reporting success.
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("The pin promote did not re-wire every prior connection."),
				TEXT("promote_rewire_incomplete"));
		}

		const FGuid AccessorGuid = AccessorNode->NodeGuid;
		const bool bPromotedOutput = (PinDir == EGPD_Output);
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, true);

		bool bSaved = false;
		bool bCompiled = false;
		const FMCPToolResult Finalize = FinalizeGuardedMutation(
			BP, Params, Guard, Transaction, bSaved, bCompiled);
		if (!Finalize.bSuccess)
		{
			return Finalize;
		}

		UEdGraphNode* ReadBackAccessor = MCPHelpers::FindNodeByGuid(BP, AccessorGuid.ToString());
		UEdGraphPin* ReadBackRewiredPin = nullptr;
		if (ReadBackAccessor)
		{
			for (UEdGraphPin* P : ReadBackAccessor->Pins)
			{
				if (P && P->Direction == (bPromotedOutput ? EGPD_Output : EGPD_Input)
					&& P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					ReadBackRewiredPin = P;
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("variableName"), VariableName);
		Result->SetObjectField(TEXT("variableType"), MCPHelpers::SerializePinType(VariableType));
		Result->SetStringField(TEXT("accessorNodeId"), AccessorGuid.ToString());
		Result->SetStringField(TEXT("accessorKind"), bPromotedOutput ? TEXT("VariableGet") : TEXT("VariableSet"));
		if (ReadBackRewiredPin)
		{
			TSharedRef<FJsonObject> Rewired = MakeShared<FJsonObject>();
			Rewired->SetStringField(TEXT("pinId"), ReadBackRewiredPin->PinId.ToString());
			Rewired->SetStringField(TEXT("pinName"), ReadBackRewiredPin->PinName.ToString());
			Rewired->SetNumberField(TEXT("linkedCount"), ReadBackRewiredPin->LinkedTo.Num());
			Result->SetObjectField(TEXT("rewiredPin"), Rewired);
		}
		Result->SetNumberField(TEXT("connectionsMade"), ConnectionsMade);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// bulk_add_nodes
// ============================================================
class FTool_BulkAddNodes : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.node.bulk.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		const TArray<TSharedPtr<FJsonValue>>* NodesValues = nullptr;
		if (BlueprintName.IsEmpty()
			|| !Params->TryGetArrayField(TEXT("nodes"), NodesValues)
			|| !NodesValues || NodesValues->IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and a non-empty nodes array are required."),
				TEXT("invalid_params"),
				422);
		}
		if (NodesValues->Num() > 64)
		{
			return FMCPToolResult::Error(
				TEXT("nodes is limited to 64 nodes per request."),
				TEXT("request_too_large"),
				422);
		}

		FString GraphName;
		Params->TryGetStringField(TEXT("graph"), GraphName);

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);

		UEdGraph* TargetGraph = ResolveTargetGraph(BP, GraphName);
		if (!TargetGraph)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Graph '%s' not found"), GraphName.IsEmpty() ? TEXT("EventGraph") : *GraphName),
				TEXT("graph_not_found"),
				404);
		}

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "BulkAddNodes", "Bulk Add Blueprint Nodes"));
		Guard.MarkMutationStarted();
		TargetGraph->Modify();

		const bool bDeferred = UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		TArray<FGuid> AddedGuids;
		for (int32 Index = 0; Index < NodesValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*NodesValues)[Index];
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("nodes[%d] must be an object."), Index),
					TEXT("invalid_params"));
			}

			// Build sub-params for the shared single-node resolution.  The
			// canonical kind field is nodeType; 'class' is the bulk alias.
			TSharedRef<FJsonObject> SubParams = MakeShared<FJsonObject>();
			for (const auto& Pair : (*Entry)->Values)
			{
				SubParams->SetField(Pair.Key, Pair.Value);
			}
			FString NodeKind;
			SubParams->TryGetStringField(TEXT("nodeType"), NodeKind);
			if (NodeKind.IsEmpty())
			{
				SubParams->TryGetStringField(TEXT("class"), NodeKind);
			}
			if (NodeKind.IsEmpty())
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("nodes[%d] requires 'class' or 'nodeType'."), Index),
					TEXT("invalid_params"));
			}
			SubParams->SetStringField(TEXT("nodeType"), NodeKind);

			int32 PosX = 0;
			int32 PosY = 0;
			const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
			if (SubParams->TryGetArrayField(TEXT("position"), Position) && Position && Position->Num() >= 2
				&& (*Position)[0].IsValid() && (*Position)[1].IsValid()
				&& (*Position)[0]->Type == EJson::Number && (*Position)[1]->Type == EJson::Number)
			{
				PosX = static_cast<int32>((*Position)[0]->AsNumber());
				PosY = static_cast<int32>((*Position)[1]->AsNumber());
			}

			UEdGraphNode* NewNode = nullptr;
			const FMCPToolResult CreateResult = TryCreateNodeInGraph(
				BP, TargetGraph, SubParams, PosX, PosY, bDeferred, NewNode);
			if (!CreateResult.bSuccess)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					CreateResult.ErrorMessage,
					CreateResult.ErrorCode);
			}
			AddedGuids.Add(NewNode->NodeGuid);
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, true);
		bool bSaved = false;
		bool bCompiled = false;
		const FMCPToolResult Finalize = FinalizeGuardedMutation(
			BP, Params, Guard, Transaction, bSaved, bCompiled);
		if (!Finalize.bSuccess)
		{
			return Finalize;
		}

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		for (const FGuid& Guid : AddedGuids)
		{
			UEdGraphNode* ReadBack = MCPHelpers::FindNodeByGuid(BP, Guid.ToString());
			if (ReadBack && ReadBack->GetGraph() == TargetGraph)
			{
				if (TSharedPtr<FJsonObject> NodeState = MCPHelpers::SerializeNode(ReadBack))
				{
					NodeValues.Add(MakeShared<FJsonValueObject>(NodeState.ToSharedRef()));
				}
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetStringField(TEXT("graph"), TargetGraph->GetName());
		Result->SetNumberField(TEXT("addedCount"), AddedGuids.Num());
		Result->SetArrayField(TEXT("nodes"), NodeValues);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// bulk_connect_pins
// ============================================================
class FTool_BulkConnectPins : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.pin.bulk.connect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
		if (BlueprintName.IsEmpty()
			|| !Params->TryGetArrayField(TEXT("connections"), Connections)
			|| !Connections || Connections->IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and a non-empty connections array are required."),
				TEXT("invalid_params"),
				422);
		}
		if (Connections->Num() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("connections is limited to 256 pairs per request."),
				TEXT("request_too_large"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "BulkConnectPins", "Bulk Connect Blueprint Pins"));
		Guard.MarkMutationStarted();

		const bool bDeferred = UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		TArray<TSharedPtr<FJsonValue>> ConnectionRows;
		for (int32 Index = 0; Index < Connections->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Connections)[Index];
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d] must be an object."), Index),
					TEXT("invalid_params"));
			}
			const FString SourceNodeId = (*Entry)->GetStringField(TEXT("sourceNodeId"));
			const FString SourcePinName = (*Entry)->GetStringField(TEXT("sourcePinName"));
			const FString TargetNodeId = (*Entry)->GetStringField(TEXT("targetNodeId"));
			const FString TargetPinName = (*Entry)->GetStringField(TEXT("targetPinName"));
			if (SourceNodeId.IsEmpty() || SourcePinName.IsEmpty() || TargetNodeId.IsEmpty() || TargetPinName.IsEmpty())
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d] requires sourceNodeId, sourcePinName, targetNodeId, targetPinName."), Index),
					TEXT("invalid_params"));
			}

			UEdGraph* SourceGraph = nullptr;
			UEdGraphNode* SourceNode = MCPHelpers::FindNodeByGuid(BP, SourceNodeId, &SourceGraph);
			if (!SourceNode)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: source node '%s' not found."), Index, *SourceNodeId),
					TEXT("node_not_found"));
			}
			UEdGraphNode* TargetNode = MCPHelpers::FindNodeByGuid(BP, TargetNodeId);
			if (!TargetNode)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: target node '%s' not found."), Index, *TargetNodeId),
					TEXT("node_not_found"));
			}
			UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName));
			if (!SourcePin)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: source pin '%s' not found."), Index, *SourcePinName),
					TEXT("pin_not_found"));
			}
			UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
			if (!TargetPin)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: target pin '%s' not found."), Index, *TargetPinName),
					TEXT("pin_not_found"));
			}
			if (SourcePin == TargetPin)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: a pin cannot be connected to itself."), Index),
					TEXT("pin_self_connection"));
			}
			if (SourcePin->LinkedTo.Contains(TargetPin) || TargetPin->LinkedTo.Contains(SourcePin))
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: the pins are already connected."), Index),
					TEXT("pin_already_connected"));
			}
			const UEdGraphSchema* Schema = SourceGraph ? SourceGraph->GetSchema() : nullptr;
			if (!Schema)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: the graph schema is unavailable."), Index),
					TEXT("schema_unavailable"));
			}

			SourceNode->Modify();
			TargetNode->Modify();
			const bool bConnected = UEAIIntegration::Infrastructure::TryCreateConnection(
				Schema, SourcePin, TargetPin, bDeferred);
			if (!bConnected)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("connections[%d]: the schema rejected the connection (incompatible or would create a cycle)."), Index),
					TEXT("pin_connection_rejected"));
			}

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetStringField(TEXT("sourceNodeId"), SourceNodeId);
			Row->SetStringField(TEXT("sourcePinName"), SourcePinName);
			Row->SetStringField(TEXT("targetNodeId"), TargetNodeId);
			Row->SetStringField(TEXT("targetPinName"), TargetPinName);
			ConnectionRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		bool bSaved = false;
		bool bCompiled = false;
		const FMCPToolResult Finalize = FinalizeGuardedMutation(
			BP, Params, Guard, Transaction, bSaved, bCompiled);
		if (!Finalize.bSuccess)
		{
			return Finalize;
		}

		for (TSharedPtr<FJsonValue>& RowValue : ConnectionRows)
		{
			const TSharedPtr<FJsonObject> Row = RowValue->AsObject();
			if (!Row.IsValid()) continue;
			UEdGraphNode* SourceNode = MCPHelpers::FindNodeByGuid(
				BP, Row->GetStringField(TEXT("sourceNodeId")));
			UEdGraphNode* TargetNode = MCPHelpers::FindNodeByGuid(
				BP, Row->GetStringField(TEXT("targetNodeId")));
			UEdGraphPin* SourcePin = SourceNode
				? SourceNode->FindPin(FName(*Row->GetStringField(TEXT("sourcePinName"))))
				: nullptr;
			UEdGraphPin* TargetPin = TargetNode
				? TargetNode->FindPin(FName(*Row->GetStringField(TEXT("targetPinName"))))
				: nullptr;
			Row->SetBoolField(
				TEXT("verified"),
				SourcePin && TargetPin && SourcePin->LinkedTo.Contains(TargetPin));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetNumberField(TEXT("connectedCount"), ConnectionRows.Num());
		Result->SetArrayField(TEXT("connections"), ConnectionRows);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// bulk_set_pin_defaults
// ============================================================
class FTool_BulkSetPinDefaults : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.pin.default.bulk.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		const TArray<TSharedPtr<FJsonValue>>* DefaultsValues = nullptr;
		if (BlueprintName.IsEmpty()
			|| !Params->TryGetArrayField(TEXT("defaults"), DefaultsValues)
			|| !DefaultsValues || DefaultsValues->IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("blueprint and a non-empty defaults array are required."),
				TEXT("invalid_params"),
				422);
		}
		if (DefaultsValues->Num() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("defaults is limited to 256 entries per request."),
				TEXT("request_too_large"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
		}
		FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "BulkSetPinDefaults", "Bulk Set Pin Defaults"));
		Guard.MarkMutationStarted();

		const bool bDeferred = UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		TArray<TSharedPtr<FJsonValue>> EntryRows;
		for (int32 Index = 0; Index < DefaultsValues->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*DefaultsValues)[Index];
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("defaults[%d] must be an object."), Index),
					TEXT("invalid_params"));
			}
			const FString NodeId = (*Entry)->GetStringField(TEXT("nodeId"));
			const FString PinName = (*Entry)->GetStringField(TEXT("pinName"));
			const FString PinValue = (*Entry)->GetStringField(TEXT("value"));
			if (NodeId.IsEmpty() || PinName.IsEmpty())
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("defaults[%d] requires nodeId, pinName, and value."), Index),
					TEXT("invalid_params"));
			}

			UEdGraph* Graph = nullptr;
			UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId, &Graph);
			if (!Node)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("defaults[%d]: node '%s' not found."), Index, *NodeId),
					TEXT("node_not_found"));
			}
			bool bAmbiguous = false;
			UEdGraphPin* Pin = FindUniquePinByIdentity(
				Node, nullptr, FName(*PinName), bAmbiguous);
			if (bAmbiguous)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("defaults[%d]: pin '%s' is ambiguous."), Index, *PinName),
					TEXT("pin_ambiguous"));
			}
			if (!Pin)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("defaults[%d]: pin '%s' not found."), Index, *PinName),
					TEXT("pin_not_found"));
			}
			const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
			const FMCPToolResult SetResult = TrySetSinglePinDefault(
				Node, Pin, Schema, PinValue, bDeferred);
			if (!SetResult.bSuccess)
			{
				Transaction.Cancel();
				return RollbackDirectMutation(
					Guard,
					FString::Printf(TEXT("defaults[%d]: %s"), Index, *SetResult.ErrorMessage),
					SetResult.ErrorCode);
			}

			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetStringField(TEXT("nodeId"), NodeId);
			Row->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
			Row->SetStringField(TEXT("pinId"), Pin->PinId.ToString());
			Row->SetStringField(TEXT("value"), Pin->DefaultValue);
			EntryRows.Add(MakeShared<FJsonValueObject>(Row));
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		bool bSaved = false;
		bool bCompiled = false;
		const FMCPToolResult Finalize = FinalizeGuardedMutation(
			BP, Params, Guard, Transaction, bSaved, bCompiled);
		if (!Finalize.bSuccess)
		{
			return Finalize;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BP->GetPathName());
		Result->SetNumberField(TEXT("setCount"), EntryRows.Num());
		Result->SetArrayField(TEXT("defaults"), EntryRows);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("deferredCompile"), UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// duplicate_graph
// ============================================================
namespace
{
UEdGraph* ResolveDuplicateSourceGraph(
	UBlueprint* Blueprint,
	const FString& GraphInput)
{
	// Reuse the shared name/default resolution first, then fall back to
	// matching the full object path for callers that pass a subobject path.
	UEdGraph* Graph = ResolveTargetGraph(Blueprint, GraphInput);
	if (Graph || GraphInput.IsEmpty())
	{
		return Graph;
	}
	const FString Decoded = MCPHelpers::UrlDecode(GraphInput);
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Candidate : AllGraphs)
	{
		if (Candidate
			&& (Candidate->GetPathName().Equals(
					Decoded, ESearchCase::IgnoreCase)
				|| Candidate->GetPathName().EndsWith(
					TEXT(":") + Decoded)
				|| Candidate->GetPathName().EndsWith(
					TEXT("/") + Decoded)))
		{
			return Candidate;
		}
	}
	return nullptr;
}

bool DuplicateGraphNameExists(
	UBlueprint* Blueprint,
	const FString& Name)
{
	if (!Blueprint || Name.IsEmpty())
	{
		return true;
	}
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (Graph
			&& Graph->GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

int32 DuplicateGraphNonNullNodeCount(UEdGraph* Graph)
{
	int32 Count = 0;
	if (Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				++Count;
			}
		}
	}
	return Count;
}
} // namespace

class FTool_DuplicateGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.duplicate");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("blueprint"), BlueprintInput)
			|| BlueprintInput.IsEmpty()
			|| BlueprintInput.Len() > 1024)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint must be a non-empty string of at most 1024 characters."),
				TEXT("invalid_params"),
				422);
		}
		FString GraphInput;
		if ((Params->HasField(TEXT("graph"))
				&& !Params->TryGetStringField(TEXT("graph"), GraphInput))
			|| GraphInput.Len() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("graph must be a string of at most 256 characters."),
				TEXT("invalid_params"),
				422);
		}
		FString NewName;
		if ((Params->HasField(TEXT("newName"))
				&& !Params->TryGetStringField(TEXT("newName"), NewName))
			|| NewName.Len() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("newName must be a string of at most 256 characters."),
				TEXT("invalid_params"),
				422);
		}
		NewName.TrimStartAndEndInline();

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(
			BlueprintInput,
			LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("blueprint_not_found"),
				404);
		}

		UEdGraph* SourceGraph = ResolveDuplicateSourceGraph(
			BP,
			GraphInput);
		if (!SourceGraph)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Graph '%s' was not found in Blueprint '%s'."),
					GraphInput.IsEmpty()
						? TEXT("(primary event graph)")
						: *MCPHelpers::UrlDecode(GraphInput),
					*BlueprintInput),
				TEXT("graph_not_found"),
				404);
		}

		// Writes require a non-transient /Game/ Blueprint; runtime/PIE
		// overrides are out of scope for structural duplication.
		if (BP->HasAnyFlags(RF_Transient)
			|| BP->GetOutermost() == GetTransientPackage()
			|| !BP->GetPathName().StartsWith(TEXT("/Game/")))
		{
			return FMCPToolResult::Error(
				TEXT("Writes require a non-transient /Game/ Blueprint."),
				TEXT("blueprint_read_only"),
				409);
		}

		if (NewName.IsEmpty())
		{
			NewName = SourceGraph->GetName() + TEXT("_Copy");
		}
		FText InvalidNameReason;
		if (!FName(*NewName).IsValidObjectName(InvalidNameReason))
		{
			return FMCPToolResult::Error(
				InvalidNameReason.IsEmpty()
					? FString::Printf(
						TEXT("newName '%s' is not a valid object name."),
						*NewName)
					: InvalidNameReason.ToString(),
				TEXT("invalid_params"),
				422);
		}
		if (DuplicateGraphNameExists(BP, NewName))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("A graph named '%s' already exists in this Blueprint."),
					*NewName),
				TEXT("graph_name_conflict"),
				409);
		}

		// Classify the source graph so the clone lands in the correct list.
		// 0 = event graph, 1 = function graph, 2 = macro graph.
		int32 GraphListKind = -1;
		if (BP->UbergraphPages.Contains(SourceGraph))
		{
			GraphListKind = 0;
		}
		else if (BP->FunctionGraphs.Contains(SourceGraph))
		{
			GraphListKind = 1;
		}
		else if (BP->MacroGraphs.Contains(SourceGraph))
		{
			GraphListKind = 2;
		}
		else
		{
			return FMCPToolResult::Error(
				TEXT("Only event graphs, function graphs, and macro graphs can be duplicated."),
				TEXT("graph_type_unsupported"),
				422);
		}

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard
			Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(),
				Guard.GetErrorCode(),
				422);
		}
		FScopedTransaction Transaction(NSLOCTEXT(
			"UEAIIntegration",
			"DuplicateGraph",
			"Duplicate Blueprint Graph"));
		Guard.MarkMutationStarted();
		BP->Modify();

		UEdGraph* NewGraph = FEdGraphUtilities::CloneGraph(SourceGraph, BP, nullptr, false);
		if (!NewGraph)
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("Failed to allocate the duplicated graph."),
				TEXT("graph_create_failed"));
		}
		if (!NewName.IsEmpty())
		{
			NewGraph->Rename(*NewName);
		}
		NewGraph->Schema = SourceGraph->Schema;
		// CloneGraph preserves node identities from DuplicateObject; assign
		// fresh GUIDs so the copy never collides with the source graph.
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			if (Node)
			{
				Node->CreateNewGuid();
			}
		}

		if (GraphListKind == 0)
		{
			BP->UbergraphPages.Add(NewGraph);
		}
		else if (GraphListKind == 1)
		{
			BP->FunctionGraphs.Add(NewGraph);
		}
		else
		{
			BP->MacroGraphs.Add(NewGraph);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		BP->MarkPackageDirty();

		FKismetEditorUtilities::CompileBlueprint(
			BP,
			EBlueprintCompileOptions::SkipSave);
		const bool bCompiled = BP->Status != BS_Error;
		if (!bCompiled)
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("The duplicated graph introduced Blueprint compile errors."),
				TEXT("asset_compile_failed"));
		}

		const int32 NodeCount = NewGraph->Nodes.Num();
		const bool bStillPresent =
			BP->UbergraphPages.Contains(NewGraph)
			|| BP->FunctionGraphs.Contains(NewGraph)
			|| BP->MacroGraphs.Contains(NewGraph);
		if (!bStillPresent
			|| NodeCount != DuplicateGraphNonNullNodeCount(SourceGraph))
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("The duplicated graph was not present during read-back."),
				TEXT("graph_duplicate_readback_failed"));
		}
		Guard.Commit();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(
			TEXT("schema"), TEXT("ue.blueprint.graph-duplicate.v1"));
		Result->SetStringField(TEXT("blueprint"), BP->GetName());
		Result->SetStringField(
			TEXT("blueprintPath"), BP->GetPathName());
		Result->SetStringField(
			TEXT("sourceGraph"), SourceGraph->GetName());
		Result->SetStringField(
			TEXT("newGraph"), NewGraph->GetPathName());
		Result->SetStringField(TEXT("newGraphName"), NewName);
		Result->SetNumberField(TEXT("nodeCount"), NodeCount);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetStringField(
			TEXT("scope"),
			TEXT("authored structural duplication; runtime unverified"));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// copy_nodes
// ============================================================
class FTool_CopyNodes : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.copy_nodes");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("blueprint"), BlueprintInput)
			|| BlueprintInput.IsEmpty()
			|| BlueprintInput.Len() > 1024)
		{
			return FMCPToolResult::Error(
				TEXT("blueprint must be a non-empty string of at most 1024 characters."),
				TEXT("invalid_params"),
				422);
		}
		FString SourceGraphInput;
		if (!Params->TryGetStringField(TEXT("sourceGraph"), SourceGraphInput)
			|| SourceGraphInput.IsEmpty()
			|| SourceGraphInput.Len() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("sourceGraph must be a non-empty string of at most 256 characters."),
				TEXT("invalid_params"),
				422);
		}
		FString TargetGraphInput;
		if (!Params->TryGetStringField(TEXT("targetGraph"), TargetGraphInput)
			|| TargetGraphInput.IsEmpty()
			|| TargetGraphInput.Len() > 256)
		{
			return FMCPToolResult::Error(
				TEXT("targetGraph must be a non-empty string of at most 256 characters."),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(
			BlueprintInput,
			LoadError);
		if (!BP)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("blueprint_not_found"),
				404);
		}

		UEdGraph* SourceGraph = ResolveDuplicateSourceGraph(
			BP,
			SourceGraphInput);
		if (!SourceGraph)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Source graph '%s' was not found in Blueprint '%s'."),
					*MCPHelpers::UrlDecode(SourceGraphInput),
					*BlueprintInput),
				TEXT("graph_not_found"),
				404);
		}
		UEdGraph* TargetGraph = ResolveDuplicateSourceGraph(
			BP,
			TargetGraphInput);
		if (!TargetGraph)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Target graph '%s' was not found in Blueprint '%s'."),
					*MCPHelpers::UrlDecode(TargetGraphInput),
					*BlueprintInput),
				TEXT("graph_not_found"),
				404);
		}
		if (SourceGraph == TargetGraph)
		{
			return FMCPToolResult::Error(
				TEXT("sourceGraph and targetGraph must refer to different graphs."),
				TEXT("graph_copy_self"),
				422);
		}

		// Writes require a non-transient /Game/ Blueprint; runtime/PIE
		// overrides are out of scope for structural node copying.
		if (BP->HasAnyFlags(RF_Transient)
			|| BP->GetOutermost() == GetTransientPackage()
			|| !BP->GetPathName().StartsWith(TEXT("/Game/")))
		{
			return FMCPToolResult::Error(
				TEXT("Writes require a non-transient /Game/ Blueprint."),
				TEXT("blueprint_read_only"),
				409);
		}

		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard
			Guard(BP);
		if (!Guard.IsValid())
		{
			return FMCPToolResult::Error(
				Guard.GetErrorMessage(),
				Guard.GetErrorCode(),
				422);
		}
		FScopedTransaction Transaction(NSLOCTEXT(
			"UEAIIntegration",
			"CopyGraphNodes",
			"Copy Blueprint Graph Nodes"));
		Guard.MarkMutationStarted();
		BP->Modify();
		TargetGraph->Modify();

		const int32 SourceNodeCount =
			DuplicateGraphNonNullNodeCount(SourceGraph);
		const int32 TargetNodeCountBefore =
			DuplicateGraphNonNullNodeCount(TargetGraph);

		FCompilerResultsLog MessageLog;
		MessageLog.bSilentMode = true;
		TArray<UEdGraphNode*> ClonedNodes;
		FEdGraphUtilities::CloneAndMergeGraphIn(
			TargetGraph,
			SourceGraph,
			MessageLog,
			true,
			false,
			&ClonedNodes);

		// CloneAndMergeGraphIn clones through the compile path (transient,
		// non-transactional), so each copied node must be re-identified and
		// re-flagged as durable graph content before it can be trusted.
		int32 CopiedNodeCount = 0;
		for (UEdGraphNode* Node : ClonedNodes)
		{
			if (!Node)
			{
				continue;
			}
			Node->ClearFlags(RF_Transient);
			Node->SetFlags(RF_Transactional);
			Node->CreateNewGuid();
			++CopiedNodeCount;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		BP->MarkPackageDirty();

		FKismetEditorUtilities::CompileBlueprint(
			BP,
			EBlueprintCompileOptions::SkipSave);
		const bool bCompiled = BP->Status != BS_Error;
		if (!bCompiled)
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("The copied nodes introduced Blueprint compile errors."),
				TEXT("asset_compile_failed"));
		}

		const int32 TargetNodeCountAfter =
			DuplicateGraphNonNullNodeCount(TargetGraph);
		if (CopiedNodeCount != SourceNodeCount
			|| TargetNodeCountAfter != TargetNodeCountBefore + SourceNodeCount)
		{
			Transaction.Cancel();
			return RollbackDirectMutation(
				Guard,
				TEXT("The target graph did not gain the source graph's nodes during read-back."),
				TEXT("graph_copy_readback_failed"));
		}
		Guard.Commit();

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(
			TEXT("schema"), TEXT("ue.blueprint.graph-copy-nodes.v1"));
		Result->SetStringField(TEXT("blueprint"), BP->GetName());
		Result->SetStringField(
			TEXT("blueprintPath"), BP->GetPathName());
		Result->SetStringField(
			TEXT("sourceGraph"), SourceGraph->GetName());
		Result->SetStringField(
			TEXT("targetGraph"), TargetGraph->GetName());
		Result->SetNumberField(
			TEXT("sourceNodeCount"), SourceNodeCount);
		Result->SetNumberField(
			TEXT("copiedNodeCount"), CopiedNodeCount);
		Result->SetNumberField(
			TEXT("targetNodeCount"), TargetNodeCountAfter);
		Result->SetBoolField(TEXT("compiled"), bCompiled);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetStringField(
			TEXT("scope"),
			TEXT("authored structural copy; runtime unverified"));
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_ReplaceFunctionCalls>());
		Registry.Register(MakeShared<FTool_DeleteAsset>());
		Registry.Register(MakeShared<FTool_ConnectPins>());
		Registry.Register(MakeShared<FTool_DisconnectPin>());
		Registry.Register(MakeShared<FTool_AddNode>());
		Registry.Register(MakeShared<FTool_DeleteNode>());
		Registry.Register(MakeShared<FTool_MoveNode>());
		Registry.Register(MakeShared<FTool_SetPinDefault>());
		Registry.Register(MakeShared<FTool_DuplicateNodes>());
		Registry.Register(MakeShared<FTool_SetCommentTitle>());
		Registry.Register(MakeShared<FTool_SetCommentBubble>());
		Registry.Register(MakeShared<FTool_RefreshAllNodes>());
		Registry.Register(MakeShared<FTool_RenameAsset>());
		Registry.Register(MakeShared<FTool_SetBlueprintDefault>());
		Registry.Register(MakeShared<FTool_PromotePin>());
		Registry.Register(MakeShared<FTool_BulkAddNodes>());
		Registry.Register(MakeShared<FTool_BulkConnectPins>());
		Registry.Register(MakeShared<FTool_BulkSetPinDefaults>());
		Registry.Register(MakeShared<FTool_DuplicateGraph>());
		Registry.Register(MakeShared<FTool_CopyNodes>());
	}
}
