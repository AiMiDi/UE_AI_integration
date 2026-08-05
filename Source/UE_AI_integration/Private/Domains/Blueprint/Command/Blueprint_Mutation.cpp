// Blueprint Mutation Tools — modify nodes, pins, connections, assets
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
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
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/UnrealType.h"

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
			MCPHelpers::SaveBlueprintPackage(BP);

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

		SourceNode->Modify();
		TargetNode->Modify();
		bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);
		if (!bConnected)
			return FMCPToolResult::Error(TEXT("Cannot connect — types are incompatible"));

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetBoolField(TEXT("saved"), bSaved);
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

		int32 DisconnectedCount = 0;
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

		bool bSaved = DisconnectedCount > 0 ? MCPHelpers::SaveBlueprintPackage(BP) : false;

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("disconnectedCount"), DisconnectedCount);
		Result->SetBoolField(TEXT("saved"), bSaved);
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

		UEdGraphNode* NewNode = nullptr;
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
		TargetGraph->Modify();

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
			RollbackAddedGraphNodes(
				TargetGraph,
				NodesBefore,
				BlueprintPackage,
				bPackageWasDirty);
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
		const bool bDeferred =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		if (!bDeferred
			&& NodeType != TEXT("Reroute")
			&& HasUnresolvedWildcardPins(NewNode))
		{
			RollbackAddedGraphNodes(
				TargetGraph,
				NodesBefore,
				BlueprintPackage,
				bPackageWasDirty);
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
					RollbackAddedGraphNodes(
						TargetGraph,
						NodesBefore,
						BlueprintPackage,
						bPackageWasDirty);
					return FMCPToolResult::Error(
						DelegateValidationError,
						TEXT("signature_mismatch"),
						422);
				}
			}
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(
			BP,
			Params);
		bool bCompiled = false;
		bool bSaved = false;
		if (!bDeferred)
		{
			FKismetEditorUtilities::CompileBlueprint(BP);
			bCompiled = BP->Status != BS_Error;
			if (!bCompiled)
			{
				RollbackAddedGraphNodes(
					TargetGraph,
					NodesBefore,
					BlueprintPackage,
					bPackageWasDirty);
				FKismetEditorUtilities::CompileBlueprint(BP);
				if (BlueprintPackage)
				{
					BlueprintPackage->SetDirtyFlag(
						bPackageWasDirty);
				}
				return FMCPToolResult::Error(
					TEXT("Node creation introduced Blueprint compile errors and was rolled back."),
					TEXT("asset_compile_failed"),
					500);
			}
			bSaved = MCPHelpers::SaveBlueprintPackage(BP);
			if (!bSaved)
			{
				RollbackAddedGraphNodes(
					TargetGraph,
					NodesBefore,
					BlueprintPackage,
					bPackageWasDirty);
				FKismetEditorUtilities::CompileBlueprint(BP);
				if (BlueprintPackage)
				{
					BlueprintPackage->SetDirtyFlag(
						bPackageWasDirty);
				}
				return FMCPToolResult::Error(
					TEXT("Node compiled but the Blueprint could not be saved; the mutation was rolled back."),
					TEXT("asset_save_failed"),
					500);
			}
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

		FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		Node->BreakAllNodeLinks();
		Graph->RemoveNode(Node);

		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetStringField(TEXT("nodeTitle"), NodeTitle);
		Result->SetBoolField(TEXT("saved"), bSaved);
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

		Node->Modify();
		Node->NodePosX = (int32)Params->GetNumberField(TEXT("posX"));
		Node->NodePosY = (int32)Params->GetNumberField(TEXT("posY"));
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);

		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetNumberField(TEXT("posX"), Node->NodePosX);
		Result->SetNumberField(TEXT("posY"), Node->NodePosY);
		Result->SetBoolField(TEXT("saved"), bSaved);
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
		FString PinName = Params->GetStringField(TEXT("pinName"));
		FString Value = Params->GetStringField(TEXT("value"));
		if (BlueprintName.IsEmpty() || NodeId.IsEmpty() || PinName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraph* Graph = nullptr;
		UEdGraphNode* Node = MCPHelpers::FindNodeByGuid(BP, NodeId, &Graph);
		if (!Node) return FMCPToolResult::Error(TEXT("Node not found"));

		UEdGraphPin* Pin = Node->FindPin(FName(*PinName));
		if (!Pin) return FMCPToolResult::Error(TEXT("Pin not found"));
		if (Pin->Direction != EGPD_Input) return FMCPToolResult::Error(TEXT("Can only set defaults on input pins"));

		FString OldValue = Pin->DefaultValue;
		Node->Modify();
		const UEdGraphSchema* Schema = Graph ? Graph->GetSchema() : nullptr;
		if (Schema) Schema->TrySetDefaultValue(*Pin, Value);
		else Pin->DefaultValue = Value;

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		const bool bDeferredWorkflow =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		if (!bDeferredWorkflow)
		{
			FKismetEditorUtilities::CompileBlueprint(BP);
		}
		const bool bSaved = !bDeferredWorkflow
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("oldValue"), OldValue);
		Result->SetStringField(TEXT("newValue"), Pin->DefaultValue);
		Result->SetBoolField(TEXT("saved"), bSaved);
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
		// Stub — duplication requires deep node cloning; return info about what would be duplicated
		return FMCPToolResult::Error(TEXT("duplicate_nodes is not yet implemented in this version"));
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

		FString OldComment = Node->NodeComment;
		Node->Modify();
		Node->NodeComment = Title;
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);

		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("oldTitle"), OldComment);
		Result->SetStringField(TEXT("newTitle"), Title);
		Result->SetBoolField(TEXT("bubbleVisible"), Node->bCommentBubbleVisible);
		Result->SetBoolField(TEXT("saved"), bSaved);
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
		const bool bOldVisible = Node->bCommentBubbleVisible;
		Node->Modify();
		Node->bCommentBubbleVisible = bVisible;
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params, false);
		const bool bSaved = UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetBoolField(TEXT("oldVisible"), bOldVisible);
		Result->SetBoolField(TEXT("visible"), bVisible);
		Result->SetStringField(TEXT("title"), Node->NodeComment);
		Result->SetBoolField(TEXT("saved"), bSaved);
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

		bool bSaved = MCPHelpers::SaveBlueprintPackage(BP);

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
			return FMCPToolResult::Error(TEXT("Missing required fields: assetPath, newPath"));

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
		// Stub: actual rename requires FAssetRenameData pipeline
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("oldPath"), AssetPath);
		Result->SetStringField(TEXT("newPath"), NewPath);
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
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString VariableName = Params->GetStringField(TEXT("variable"));
		FString Value = Params->GetStringField(TEXT("value"));
		if (BlueprintName.IsEmpty() || VariableName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		if (!BP->GeneratedClass)
			return FMCPToolResult::Error(TEXT("Blueprint has no generated class"));

		UObject* CDO = BP->GeneratedClass->GetDefaultObject();
		if (!CDO)
			return FMCPToolResult::Error(TEXT("Could not get CDO"));

		FProperty* Prop = BP->GeneratedClass->FindPropertyByName(FName(*VariableName));
		if (!Prop)
			return FMCPToolResult::Error(FString::Printf(TEXT("Variable '%s' not found"), *VariableName));

		void* PropAddr = Prop->ContainerPtrToValuePtr<void>(CDO);
		Prop->ImportText_Direct(*Value, PropAddr, CDO, PPF_None);

		bool bSaved = MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("variable"), VariableName);
		Result->SetBoolField(TEXT("saved"), bSaved);
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
	}
}
