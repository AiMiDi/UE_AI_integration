// Variable Tools — add, remove, change type, set metadata for Blueprint variables
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// ============================================================
// add_variable
// ============================================================
class FTool_AddVariable : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.variable.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString VariableName = Params->GetStringField(TEXT("variableName"));
		FString VariableType = Params->GetStringField(TEXT("variableType"));

		if (BlueprintName.IsEmpty() || VariableName.IsEmpty() || VariableType.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, variableName, variableType"));

		FString Category;
		Params->TryGetStringField(TEXT("category"), Category);
		bool bIsArray = false;
		Params->TryGetBoolField(TEXT("isArray"), bIsArray);
		FString DefaultValue;
		Params->TryGetStringField(TEXT("defaultValue"), DefaultValue);

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		FName VarFName(*VariableName);
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == VarFName)
				return FMCPToolResult::Error(FString::Printf(TEXT("Variable '%s' already exists"), *VariableName));
		}

		FEdGraphPinType PinType;
		FString TypeError;
		if (!MCPHelpers::ResolveTypeFromString(VariableType, PinType, TypeError))
			return FMCPToolResult::Error(TypeError);

		if (bIsArray)
			PinType.ContainerType = EPinContainerType::Array;

		const bool bSuccess =
			UEAIIntegration::Workflow::AddBlueprintMemberVariable(
				BP,
				VarFName,
				PinType,
				DefaultValue,
				Params);
		if (!bSuccess)
			return FMCPToolResult::Error(FString::Printf(TEXT("AddMemberVariable failed for '%s'"), *VariableName));

		if (!Category.IsEmpty())
		{
			if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				for (FBPVariableDescription& Variable : BP->NewVariables)
				{
					if (Variable.VarName == VarFName)
					{
						Variable.Category = FText::FromString(Category);
						break;
					}
				}
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(
					BP,
					VarFName,
					nullptr,
					FText::FromString(Category));
			}
		}

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("variableName"), VariableName);
		Result->SetStringField(TEXT("variableType"), VariableType);
		if (!Category.IsEmpty()) Result->SetStringField(TEXT("category"), Category);
		Result->SetBoolField(TEXT("isArray"), bIsArray);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// remove_variable
// ============================================================
class FTool_RemoveVariable : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.variable.remove");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString VariableName = Params->GetStringField(TEXT("variableName"));

		if (BlueprintName.IsEmpty() || VariableName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, variableName"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		FName VarFName(*VariableName);
		bool bVarFound = false;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName.ToString().Equals(VariableName, ESearchCase::IgnoreCase))
			{
				VarFName = Var.VarName;
				bVarFound = true;
				break;
			}
		}

		if (!bVarFound)
		{
			TArray<TSharedPtr<FJsonValue>> AvailVars;
			for (const FBPVariableDescription& Var : BP->NewVariables)
				AvailVars.Add(MakeShared<FJsonValueString>(Var.VarName.ToString()));

			TSharedRef<FJsonObject> ErrorResult = MakeShared<FJsonObject>();
			ErrorResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable '%s' not found"), *VariableName));
			ErrorResult->SetArrayField(TEXT("availableVariables"), AvailVars);
			return FMCPToolResult::Error(FString::Printf(TEXT("Variable '%s' not found in Blueprint '%s'"), *VariableName, *BlueprintName));
		}

		FBlueprintEditorUtils::RemoveMemberVariable(BP, VarFName);
		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("variableName"), VariableName);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// change_variable_type
// ============================================================
class FTool_ChangeVariableType : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.variable.type.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString VariableName = Params->GetStringField(TEXT("variable"));
		FString NewTypeName = Params->GetStringField(TEXT("newType"));

		if (BlueprintName.IsEmpty() || VariableName.IsEmpty() || NewTypeName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, variable, newType"));

		FString TypeCategory;
		Params->TryGetStringField(TEXT("typeCategory"), TypeCategory);
		bool bDryRun = false;
		Params->TryGetBoolField(TEXT("dryRun"), bDryRun);

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		bool bVarFound = false;
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName.ToString() == VariableName) { bVarFound = true; break; }
		}
		if (!bVarFound)
			return FMCPToolResult::Error(FString::Printf(TEXT("Variable '%s' not found"), *VariableName));

		FEdGraphPinType NewPinType;
		FString ResolveInput = NewTypeName;
		if (TypeCategory == TEXT("object") || TypeCategory == TEXT("softobject") ||
			TypeCategory == TEXT("class") || TypeCategory == TEXT("softclass") ||
			TypeCategory == TEXT("interface"))
		{
			ResolveInput = TypeCategory + TEXT(":") + NewTypeName;
		}

		FString TypeError;
		if (!MCPHelpers::ResolveTypeFromString(ResolveInput, NewPinType, TypeError))
			return FMCPToolResult::Error(TypeError);

		// Analyze affected nodes
		TArray<TSharedPtr<FJsonValue>> AffectedNodes;
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				if (auto* VG = Cast<UK2Node_VariableGet>(Node))
				{
					if (VG->GetVarName().ToString() == VariableName)
					{
						TSharedRef<FJsonObject> AffNode = MakeShared<FJsonObject>();
						AffNode->SetStringField(TEXT("nodeId"), VG->NodeGuid.ToString());
						AffNode->SetStringField(TEXT("nodeType"), TEXT("VariableGet"));
						AffNode->SetStringField(TEXT("graph"), Graph->GetName());
						AffectedNodes.Add(MakeShared<FJsonValueObject>(AffNode));
					}
				}
				else if (auto* VS = Cast<UK2Node_VariableSet>(Node))
				{
					if (VS->GetVarName().ToString() == VariableName)
					{
						TSharedRef<FJsonObject> AffNode = MakeShared<FJsonObject>();
						AffNode->SetStringField(TEXT("nodeId"), VS->NodeGuid.ToString());
						AffNode->SetStringField(TEXT("nodeType"), TEXT("VariableSet"));
						AffNode->SetStringField(TEXT("graph"), Graph->GetName());
						AffectedNodes.Add(MakeShared<FJsonValueObject>(AffNode));
					}
				}
			}
		}

		if (bDryRun)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("dryRun"), true);
			Result->SetStringField(TEXT("blueprint"), BlueprintName);
			Result->SetStringField(TEXT("variable"), VariableName);
			Result->SetStringField(TEXT("newType"), NewTypeName);
			Result->SetNumberField(TEXT("affectedNodeCount"), AffectedNodes.Num());
			Result->SetArrayField(TEXT("affectedNodes"), AffectedNodes);
			return FMCPToolResult::Ok(Result);
		}

		for (FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == FName(*VariableName))
			{
				Var.VarType = NewPinType;
				break;
			}
		}

		bool bSaved = MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("variable"), VariableName);
		Result->SetStringField(TEXT("newType"), NewTypeName);
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetArrayField(TEXT("affectedNodes"), AffectedNodes);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_variable_metadata
// ============================================================
class FTool_SetVariableMetadata : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.variable.metadata.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintName = Params->GetStringField(TEXT("blueprint"));
		FString VariableName = Params->GetStringField(TEXT("variable"));

		if (BlueprintName.IsEmpty() || VariableName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: blueprint, variable"));

		FString LoadError;
		UBlueprint* BP = MCPHelpers::LoadBlueprintByName(BlueprintName, LoadError);
		if (!BP) return FMCPToolResult::Error(LoadError);

		FName VarFName(*VariableName);
		FBPVariableDescription* VarDesc = nullptr;
		for (FBPVariableDescription& Var : BP->NewVariables)
		{
			if (Var.VarName == VarFName) { VarDesc = &Var; break; }
		}
		if (!VarDesc)
			return FMCPToolResult::Error(FString::Printf(TEXT("Variable '%s' not found"), *VariableName));

		TArray<TSharedPtr<FJsonValue>> Changes;

		if (Params->HasField(TEXT("category")))
		{
			FString NewCategory = Params->GetStringField(TEXT("category"));
			if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				VarDesc->Category = FText::FromString(NewCategory);
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(
					BP,
					VarFName,
					nullptr,
					FText::FromString(NewCategory));
			}
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("field"), TEXT("category"));
			Change->SetStringField(TEXT("newValue"), NewCategory);
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Params->HasField(TEXT("tooltip")))
		{
			FString NewTooltip = Params->GetStringField(TEXT("tooltip"));
			if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				VarDesc->SetMetaData(TEXT("tooltip"), *NewTooltip);
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(
					BP,
					VarFName,
					nullptr,
					TEXT("tooltip"),
					NewTooltip);
			}
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("field"), TEXT("tooltip"));
			Change->SetStringField(TEXT("newValue"), NewTooltip);
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Params->HasField(TEXT("replication")))
		{
			FString ReplicationStr = Params->GetStringField(TEXT("replication"));
			if (ReplicationStr == TEXT("none"))
			{
				VarDesc->PropertyFlags &= ~CPF_Net;
				VarDesc->PropertyFlags &= ~CPF_RepNotify;
				VarDesc->RepNotifyFunc = NAME_None;
			}
			else if (ReplicationStr == TEXT("replicated"))
			{
				VarDesc->PropertyFlags |= CPF_Net;
				VarDesc->PropertyFlags &= ~CPF_RepNotify;
				VarDesc->RepNotifyFunc = NAME_None;
			}
			else if (ReplicationStr == TEXT("repNotify"))
			{
				VarDesc->PropertyFlags |= CPF_Net | CPF_RepNotify;
				VarDesc->RepNotifyFunc = FName(*FString::Printf(TEXT("OnRep_%s"), *VariableName));
			}
			else
				return FMCPToolResult::Error(FString::Printf(TEXT("Invalid replication '%s'. Valid: none, replicated, repNotify"), *ReplicationStr));

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("field"), TEXT("replication"));
			Change->SetStringField(TEXT("newValue"), ReplicationStr);
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Params->HasField(TEXT("exposeOnSpawn")))
		{
			bool bNew = Params->GetBoolField(TEXT("exposeOnSpawn"));
			if (bNew) VarDesc->PropertyFlags |= CPF_ExposeOnSpawn;
			else VarDesc->PropertyFlags &= ~CPF_ExposeOnSpawn;
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("field"), TEXT("exposeOnSpawn"));
			Change->SetStringField(TEXT("newValue"), bNew ? TEXT("true") : TEXT("false"));
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Params->HasField(TEXT("isPrivate")))
		{
			bool bNew = Params->GetBoolField(TEXT("isPrivate"));
			if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				VarDesc->SetMetaData(
					TEXT("BlueprintPrivate"),
					bNew ? TEXT("true") : TEXT("false"));
			}
			else
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(
					BP,
					VarFName,
					nullptr,
					TEXT("BlueprintPrivate"),
					bNew ? TEXT("true") : TEXT("false"));
			}
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("field"), TEXT("isPrivate"));
			Change->SetStringField(TEXT("newValue"), bNew ? TEXT("true") : TEXT("false"));
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Params->HasField(TEXT("editability")))
		{
			FString Editability = Params->GetStringField(TEXT("editability"));
			VarDesc->PropertyFlags &= ~(CPF_Edit | CPF_DisableEditOnInstance | CPF_DisableEditOnTemplate);
			if (Editability == TEXT("editAnywhere"))
				VarDesc->PropertyFlags |= CPF_Edit;
			else if (Editability == TEXT("editDefaultsOnly"))
				VarDesc->PropertyFlags |= CPF_Edit | CPF_DisableEditOnInstance;
			else if (Editability == TEXT("editInstanceOnly"))
				VarDesc->PropertyFlags |= CPF_Edit | CPF_DisableEditOnTemplate;
			else if (Editability != TEXT("none"))
				return FMCPToolResult::Error(FString::Printf(TEXT("Invalid editability '%s'"), *Editability));

			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("field"), TEXT("editability"));
			Change->SetStringField(TEXT("newValue"), Editability);
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Changes.Num() == 0)
			return FMCPToolResult::Error(TEXT("No metadata fields specified. Provide at least one of: category, tooltip, replication, exposeOnSpawn, isPrivate, editability"));

		UEAIIntegration::Workflow::MarkBlueprintChanged(BP, Params);
		const bool bSaved =
			UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::SaveBlueprintPackage(BP);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("blueprint"), BlueprintName);
		Result->SetStringField(TEXT("variable"), VariableName);
		Result->SetArrayField(TEXT("changes"), Changes);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterVariableTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_AddVariable>());
		Registry.Register(MakeShared<FTool_RemoveVariable>());
		Registry.Register(MakeShared<FTool_ChangeVariableType>());
		Registry.Register(MakeShared<FTool_SetVariableMetadata>());
	}
}
