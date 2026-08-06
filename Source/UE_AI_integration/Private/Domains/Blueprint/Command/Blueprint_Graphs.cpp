// Blueprint Graph Tools — create/delete/rename graphs, create blueprint, reparent
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"

// ============================================================
// create_blueprint
// ============================================================
class FTool_CreateBlueprint : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprintName"));
		FString PackagePath = Params->GetStringField(TEXT("packagePath"));
		FString ParentClassName = Params->GetStringField(TEXT("parentClass"));
		FString BlueprintTypeStr;
		Params->TryGetStringField(TEXT("blueprintType"), BlueprintTypeStr);

		if (BlueprintName.IsEmpty() || PackagePath.IsEmpty() || ParentClassName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprintName, packagePath, parentClass"));

		if (!PackagePath.StartsWith(TEXT("/Game")))
			return FMCPToolResult::Error(TEXT("packagePath must start with '/Game'"));

		// Check if asset already exists
		FString FullAssetPath = PackagePath / BlueprintName;
		FString LoadErr;
		if (MCPHelpers::LoadBlueprintByName(BlueprintName, LoadErr) || MCPHelpers::LoadBlueprintByName(FullAssetPath, LoadErr))
			return FMCPToolResult::Error(FString::Printf(TEXT("Blueprint '%s' already exists."), *BlueprintName));

		// Resolve parent class
		UClass* ParentClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == ParentClassName)
			{
				ParentClass = *It;
				break;
			}
		}
		if (!ParentClass)
		{
			UBlueprint* ParentBP = MCPHelpers::LoadBlueprintByName(ParentClassName, LoadErr);
			if (ParentBP && ParentBP->GeneratedClass)
				ParentClass = ParentBP->GeneratedClass;
		}
		if (!ParentClass)
			return FMCPToolResult::Error(FString::Printf(TEXT("Could not find parent class '%s'."), *ParentClassName));

		// Map blueprintType
		EBlueprintType BlueprintType = BPTYPE_Normal;
		if (!BlueprintTypeStr.IsEmpty())
		{
			if (BlueprintTypeStr == TEXT("Interface")) BlueprintType = BPTYPE_Interface;
			else if (BlueprintTypeStr == TEXT("FunctionLibrary")) BlueprintType = BPTYPE_FunctionLibrary;
			else if (BlueprintTypeStr == TEXT("MacroLibrary")) BlueprintType = BPTYPE_MacroLibrary;
			else if (BlueprintTypeStr != TEXT("Normal"))
				return FMCPToolResult::Error(FString::Printf(TEXT("Invalid blueprintType '%s'. Valid: Normal, Interface, FunctionLibrary, MacroLibrary"), *BlueprintTypeStr));
		}

		if (BlueprintType == BPTYPE_Interface && !ParentClass->IsChildOf(UInterface::StaticClass()))
			ParentClass = UInterface::StaticClass();

		FString FullPackagePath = PackagePath / BlueprintName;

		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(FullPackagePath))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *FullPackagePath));
		}

		UPackage* Package = CreatePackage(*FullPackagePath);
		if (!Package)
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package at '%s'"), *FullPackagePath));

		UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*BlueprintName), BlueprintType,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (!NewBP)
			return FMCPToolResult::Error(TEXT("FKismetEditorUtilities::CreateBlueprint returned null"));

		const bool bDeferredWorkflow =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
		if (!bDeferredWorkflow)
		{
			FKismetEditorUtilities::CompileBlueprint(NewBP);
		}
		else
		{
			NewBP->MarkPackageDirty();
		}
		const bool bSaved = !bDeferredWorkflow
			&& MCPHelpers::SaveBlueprintPackage(NewBP);

		TArray<TSharedPtr<FJsonValue>> GraphNames;
		for (UEdGraph* Graph : NewBP->UbergraphPages)
			GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
		for (UEdGraph* Graph : NewBP->FunctionGraphs)
			GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprintName"), BlueprintName);
		Result->SetStringField(TEXT("packagePath"), PackagePath);
		Result->SetStringField(TEXT("assetPath"), FullAssetPath);
		Result->SetStringField(TEXT("parentClass"), ParentClass->GetName());
		Result->SetStringField(TEXT("blueprintType"), BlueprintTypeStr.IsEmpty() ? TEXT("Normal") : BlueprintTypeStr);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetArrayField(TEXT("graphs"), GraphNames);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// create_graph
// ============================================================
class FTool_CreateGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString GraphName = Params->GetStringField(TEXT("graphName"));
		FString GraphType = Params->GetStringField(TEXT("graphType"));

		if (BlueprintName.IsEmpty() || GraphName.IsEmpty() || GraphType.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, graphName, graphType"));

		if (GraphType != TEXT("function") && GraphType != TEXT("macro") && GraphType != TEXT("customEvent"))
			return FMCPToolResult::Error(FString::Printf(TEXT("Invalid graphType '%s'. Valid: function, macro, customEvent"), *GraphType));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		// Check name uniqueness
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Existing : AllGraphs)
		{
			if (Existing && Existing->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				return FMCPToolResult::Error(FString::Printf(TEXT("A graph named '%s' already exists in Blueprint '%s'"), *GraphName, *BlueprintName));
		}

		// Check for existing custom events with same name
		if (GraphType == TEXT("customEvent"))
		{
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
					{
						if (CE->CustomFunctionName == FName(*GraphName))
							return FMCPToolResult::Error(FString::Printf(TEXT("A custom event named '%s' already exists"), *GraphName));
					}
				}
			}
		}

		FString CreatedNodeId;

		if (GraphType == TEXT("function"))
		{
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*GraphName),
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph) return FMCPToolResult::Error(TEXT("Failed to create function graph"));
			NewGraph->SetFlags(RF_Transactional);
			NewGraph->Modify();
			if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				FBlueprintEditorUtils::CreateFunctionGraph(
					BP,
					NewGraph,
					true,
					static_cast<UClass*>(nullptr));
				BP->FunctionGraphs.Add(NewGraph);
			}
			else
			{
				FBlueprintEditorUtils::AddFunctionGraph(
					BP,
					NewGraph,
					true,
					static_cast<UClass*>(nullptr));
			}
		}
		else if (GraphType == TEXT("macro"))
		{
			UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(BP, FName(*GraphName),
				UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!NewGraph) return FMCPToolResult::Error(TEXT("Failed to create macro graph"));
			NewGraph->SetFlags(RF_Transactional);
			NewGraph->Modify();
			if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				const UEdGraphSchema* Schema = NewGraph->GetSchema();
				const UEdGraphSchema_K2* K2Schema =
					Cast<const UEdGraphSchema_K2>(Schema);
				Schema->CreateDefaultNodesForGraph(*NewGraph);
				if (K2Schema)
				{
					K2Schema->CreateMacroGraphTerminators(
						*NewGraph,
						nullptr);
					K2Schema->AddExtraFunctionFlags(
						NewGraph,
						FUNC_BlueprintCallable | FUNC_BlueprintEvent);
					K2Schema->MarkFunctionEntryAsEditable(
						NewGraph,
						true);
				}
				if (BP->BlueprintType == BPTYPE_MacroLibrary)
				{
					NewGraph->SetFlags(RF_Public);
				}
				BP->MacroGraphs.Add(NewGraph);
			}
			else
			{
				FBlueprintEditorUtils::AddMacroGraph(
					BP,
					NewGraph,
					true,
					nullptr);
			}
		}
		else // customEvent
		{
			UEdGraph* EventGraph = BP->UbergraphPages.Num() > 0 ? BP->UbergraphPages[0] : nullptr;
			if (!EventGraph)
				return FMCPToolResult::Error(TEXT("Blueprint has no EventGraph to add a custom event to"));

			EventGraph->Modify();
			UK2Node_CustomEvent* NewEvent =
				NewObject<UK2Node_CustomEvent>(
					EventGraph,
					NAME_None,
					RF_Transactional);
			NewEvent->Modify();
			NewEvent->CustomFunctionName = FName(*GraphName);
			NewEvent->bIsEditable = true;
			EventGraph->AddNode(NewEvent, false, false);
			NewEvent->CreateNewGuid();
			NewEvent->PostPlacedNewNode();
			NewEvent->AllocateDefaultPins();
			CreatedNodeId = NewEvent->NodeGuid.ToString();
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("graphName"), GraphName);
		Result->SetStringField(TEXT("graphType"), GraphType);
		Result->SetBoolField(TEXT("saved"), bSaved);
		if (!CreatedNodeId.IsEmpty())
			Result->SetStringField(TEXT("nodeId"), CreatedNodeId);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// delete_graph
// ============================================================
class FTool_DeleteGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.delete");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString GraphName = Params->GetStringField(TEXT("graphName"));

		if (BlueprintName.IsEmpty() || GraphName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, graphName"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		UEdGraph* TargetGraph = nullptr;
		FString GraphType;

		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = Graph;
				GraphType = TEXT("function");
				break;
			}
		}
		if (!TargetGraph)
		{
			for (UEdGraph* Graph : BP->MacroGraphs)
			{
				if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					TargetGraph = Graph;
					GraphType = TEXT("macro");
					break;
				}
			}
		}

		if (!TargetGraph)
		{
			for (UEdGraph* Graph : BP->UbergraphPages)
			{
				if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
					return FMCPToolResult::Error(FString::Printf(TEXT("Cannot delete UbergraphPage '%s'."), *GraphName));
			}
			return FMCPToolResult::Error(FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintName));
		}

		int32 NodeCount = TargetGraph->Nodes.Num();
		FBlueprintEditorUtils::RemoveGraph(BP, TargetGraph, EGraphRemoveFlags::Default);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		bool bSaved = MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("graphName"), GraphName);
		Result->SetStringField(TEXT("graphType"), GraphType);
		Result->SetNumberField(TEXT("nodeCount"), NodeCount);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// rename_graph
// ============================================================
class FTool_RenameGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.rename");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString GraphName = Params->GetStringField(TEXT("graphName"));
		FString NewName = Params->GetStringField(TEXT("newName"));

		if (BlueprintName.IsEmpty() || GraphName.IsEmpty() || NewName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, graphName, newName"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				return FMCPToolResult::Error(FString::Printf(TEXT("Cannot rename UbergraphPage '%s'."), *GraphName));
		}

		UEdGraph* TargetGraph = nullptr;
		FString GraphType;

		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
			{
				TargetGraph = Graph;
				GraphType = TEXT("function");
				break;
			}
		}
		if (!TargetGraph)
		{
			for (UEdGraph* Graph : BP->MacroGraphs)
			{
				if (Graph && Graph->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					TargetGraph = Graph;
					GraphType = TEXT("macro");
					break;
				}
			}
		}

		if (!TargetGraph)
			return FMCPToolResult::Error(FString::Printf(TEXT("Graph '%s' not found in Blueprint '%s'"), *GraphName, *BlueprintName));

		// Check for name collision
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Existing : AllGraphs)
		{
			if (Existing && Existing != TargetGraph && Existing->GetName().Equals(NewName, ESearchCase::IgnoreCase))
				return FMCPToolResult::Error(FString::Printf(TEXT("A graph named '%s' already exists"), *NewName));
		}

		FBlueprintEditorUtils::RenameGraph(TargetGraph, NewName);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		bool bSaved = MCPHelpers::CompileAndSaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("oldName"), GraphName);
		Result->SetStringField(TEXT("newName"), TargetGraph->GetName());
		Result->SetStringField(TEXT("graphType"), GraphType);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// reparent_blueprint
// ============================================================
class FTool_ReparentBlueprint : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.reparent");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString NewParentName = Params->GetStringField(TEXT("newParentClass"));

		if (BlueprintName.IsEmpty() || NewParentName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, newParentClass"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		FString OldParentName = BP->ParentClass ? BP->ParentClass->GetName() : TEXT("None");

		// Find new parent class
		UClass* NewParentClass = nullptr;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == NewParentName)
			{
				NewParentClass = *It;
				break;
			}
		}
		if (!NewParentClass)
		{
			FString ParentLoadError;
			UBlueprint* ParentBP = MCPHelpers::LoadBlueprintByName(NewParentName, ParentLoadError);
			if (ParentBP && ParentBP->GeneratedClass)
				NewParentClass = ParentBP->GeneratedClass;
		}
		if (!NewParentClass)
			return FMCPToolResult::Error(FString::Printf(TEXT("Could not find class '%s'."), *NewParentName));

		BP->ParentClass = NewParentClass;
		FBlueprintEditorUtils::RefreshAllNodes(BP);
		FKismetEditorUtilities::CompileBlueprint(BP);
		bool bSaved = MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("oldParentClass"), OldParentName);
		Result->SetStringField(TEXT("newParentClass"), NewParentClass->GetName());
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterBlueprintGraphTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_CreateBlueprint>());
		Registry.Register(MakeShared<FTool_CreateGraph>());
		Registry.Register(MakeShared<FTool_DeleteGraph>());
		Registry.Register(MakeShared<FTool_RenameGraph>());
		Registry.Register(MakeShared<FTool_ReparentBlueprint>());
	}
}
