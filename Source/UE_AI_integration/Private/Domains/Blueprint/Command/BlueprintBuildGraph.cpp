#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.h"
#include "UObject/MetaData.h"
#include "Workflow/UEWorkflowRuntime.h"

namespace
{
	constexpr int32 MaxBuildNodes = 128;
	constexpr int32 MaxBuildConnections = 256;
	constexpr int32 MaxBuildComments = 64;
	constexpr int32 MaxBuildVariables = 64;
	constexpr int32 MaxBuildComponents = 64;

	bool IsSafeBuildToken(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 128)
		{
			return false;
		}
		for (const TCHAR Character : Value)
		{
			if (!FChar::IsAlnum(Character)
				&& Character != TEXT('_')
				&& Character != TEXT('-')
				&& Character != TEXT('.'))
			{
				return false;
			}
		}
		return true;
	}

	bool IsSupportedBuildNodeType(const FString& NodeType)
	{
		static const TSet<FString> Supported = {
			TEXT("CallFunction"), TEXT("VariableGet"), TEXT("VariableSet"),
			TEXT("BreakStruct"), TEXT("MakeStruct"), TEXT("Branch"),
			TEXT("Sequence"), TEXT("CustomEvent"), TEXT("OverrideEvent"),
			TEXT("ComponentBoundEvent"), TEXT("ActorBoundEvent"),
			TEXT("AssignDelegate"), TEXT("AddDelegate"),
			TEXT("RemoveDelegate"), TEXT("ClearDelegate"),
			TEXT("CallDelegate"), TEXT("CreateDelegate"),
			TEXT("InputAction"), TEXT("EnhancedInputAction"),
			TEXT("AsyncAction"), TEXT("DynamicCast"), TEXT("Comment"),
			TEXT("Reroute")
		};
		return Supported.Contains(NodeType);
	}

	FString BuildString(
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

	FString StringifyBuildJson(const TSharedPtr<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (Object.IsValid())
		{
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		}
		return Text;
	}

	TSharedPtr<FJsonObject> ParseBuildJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
			       ? Object
			       : nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> CollectBuildNodes(
		const TSharedPtr<FJsonObject>& Definition)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (Definition.IsValid()
			&& Definition->TryGetArrayField(TEXT("nodes"), Nodes)
			&& Nodes)
		{
			Result.Append(*Nodes);
		}
		const TArray<TSharedPtr<FJsonValue>>* Comments = nullptr;
		if (Definition.IsValid()
			&& Definition->TryGetArrayField(TEXT("comments"), Comments)
			&& Comments)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Comments)
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					Result.Add(Value);
					continue;
				}
				const TSharedPtr<FJsonObject> Comment = Value->AsObject();
				TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
				Node->SetStringField(TEXT("ref"), BuildString(Comment, TEXT("ref")));
				Node->SetStringField(TEXT("nodeType"), TEXT("Comment"));
				Node->SetStringField(TEXT("comment"), BuildString(Comment, TEXT("text")));
				double Number = 0.0;
				if (Comment->TryGetNumberField(TEXT("x"), Number))
				{
					Node->SetNumberField(TEXT("posX"), Number);
				}
				if (Comment->TryGetNumberField(TEXT("y"), Number))
				{
					Node->SetNumberField(TEXT("posY"), Number);
				}
				Result.Add(MakeShared<FJsonValueObject>(Node));
			}
		}
		return Result;
	}

	TSharedPtr<FJsonObject> FindDefinitionNode(
		const TSharedPtr<FJsonObject>& Definition,
		const FString& Ref)
	{
		for (const TSharedPtr<FJsonValue>& Value : CollectBuildNodes(Definition))
		{
			if (Value.IsValid() && Value->Type == EJson::Object
				&& BuildString(Value->AsObject(), TEXT("ref")) == Ref)
			{
				return Value->AsObject();
			}
		}
		return nullptr;
	}

	FString MetadataKey(const FString& BuildId, const FString& Graph)
	{
		return FString::Printf(TEXT("UEAI.BuildGraph.%s.%s"), *BuildId, *Graph);
	}

	UBlueprint* LoadBuildBlueprint(const FString& Path, FString& OutError)
	{
		return MCPHelpers::LoadBlueprintByName(Path, OutError);
	}

	bool ManagedNodeExists(UBlueprint* Blueprint, const FString& GraphName, const FString& NodeGuid)
	{
		FGuid ParsedGuid;
		if (!Blueprint || !FGuid::Parse(NodeGuid, ParsedGuid))
		{
			return false;
		}
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph->Nodes.ContainsByPredicate(
					[&ParsedGuid](const UEdGraphNode* Node)
					{
						return Node && Node->NodeGuid == ParsedGuid;
					});
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph->Nodes.ContainsByPredicate(
					[&ParsedGuid](const UEdGraphNode* Node)
					{
						return Node && Node->NodeGuid == ParsedGuid;
					});
			}
		}
		return false;
	}

	bool ValidateBuildDefinition(
		const TSharedPtr<FJsonObject>& Definition,
		TArray<FString>& OutErrors)
	{
		if (!Definition.IsValid()
			|| BuildString(Definition, TEXT("schema")) != TEXT("ue.blueprint-buildgraph.v1"))
		{
			OutErrors.Add(TEXT("schema must be ue.blueprint-buildgraph.v1."));
			return false;
		}
		for (const TCHAR* Field : {TEXT("buildId"), TEXT("blueprint"), TEXT("graph"), TEXT("mode")})
		{
			if (BuildString(Definition, Field).IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("%s is required."), Field));
			}
		}
		if (!IsSafeBuildToken(BuildString(Definition, TEXT("buildId"))))
		{
			OutErrors.Add(TEXT("buildId must contain only letters, numbers, '.', '_' or '-'."));
		}
		const FString Mode = BuildString(Definition, TEXT("mode"));
		if (Mode != TEXT("merge") && Mode != TEXT("replaceManaged"))
		{
			OutErrors.Add(TEXT("mode must be merge or replaceManaged."));
		}
		const TArray<TSharedPtr<FJsonValue>> Nodes = CollectBuildNodes(Definition);
		const TArray<TSharedPtr<FJsonValue>>* VariablesForMinimum = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ComponentsForMinimum = nullptr;
		const bool bHasVariables = Definition->TryGetArrayField(TEXT("variables"), VariablesForMinimum)
			&& VariablesForMinimum && !VariablesForMinimum->IsEmpty();
		const bool bHasComponents = Definition->TryGetArrayField(TEXT("components"), ComponentsForMinimum)
			&& ComponentsForMinimum && !ComponentsForMinimum->IsEmpty();
		if ((Nodes.IsEmpty() && !bHasVariables && !bHasComponents) || Nodes.Num() > MaxBuildNodes)
		{
			OutErrors.Add(TEXT(
				"nodes/comments or variables/components must contain at least one entry and nodes must contain at most 128 entries."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
		if (Definition->TryGetArrayField(TEXT("variables"), Variables) && Variables)
		{
			if (Variables->Num() > MaxBuildVariables)
			{
				OutErrors.Add(TEXT("variables exceeds 64 entries."));
			}
			TSet<FString> VariableNames;
			for (int32 Index = 0; Index < Variables->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Variable = (*Variables)[Index].IsValid()
				                                         && (*Variables)[Index]->Type == EJson::Object
					                                         ? (*Variables)[Index]->AsObject()
					                                         : nullptr;
				const FString Name = BuildString(Variable, TEXT("variableName"));
				const FString Type = BuildString(Variable, TEXT("variableType"));
				if (!Variable.IsValid() || !IsSafeBuildToken(Name) || Type.IsEmpty()
					|| VariableNames.Contains(Name))
				{
					OutErrors.Add(
						FString::Printf(TEXT("variables[%d] requires a unique variableName and variableType."), Index));
				}
				VariableNames.Add(Name);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (Definition->TryGetArrayField(TEXT("components"), Components) && Components)
		{
			if (Components->Num() > MaxBuildComponents)
			{
				OutErrors.Add(TEXT("components exceeds 64 entries."));
			}
			TSet<FString> ComponentNames;
			for (int32 Index = 0; Index < Components->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Component = (*Components)[Index].IsValid()
				                                          && (*Components)[Index]->Type == EJson::Object
					                                          ? (*Components)[Index]->AsObject()
					                                          : nullptr;
				const FString Name = BuildString(Component, TEXT("name"));
				const FString Class = BuildString(Component, TEXT("componentClass"));
				if (!Component.IsValid() || !IsSafeBuildToken(Name) || Class.IsEmpty()
					|| ComponentNames.Contains(Name))
				{
					OutErrors.Add(
						FString::Printf(TEXT("components[%d] requires a unique name and componentClass."), Index));
				}
				ComponentNames.Add(Name);
			}
		}
		TSet<FString> Refs;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			const TSharedPtr<FJsonObject> Node = Nodes[Index].IsValid()
			                                     && Nodes[Index]->Type == EJson::Object
				                                     ? Nodes[Index]->AsObject()
				                                     : nullptr;
			const FString Ref = BuildString(Node, TEXT("ref"));
			const FString NodeType = BuildString(Node, TEXT("nodeType"));
			if (!Node.IsValid() || !IsSafeBuildToken(Ref) || Refs.Contains(Ref)
				|| !IsSupportedBuildNodeType(NodeType))
			{
				OutErrors.Add(FString::Printf(
					TEXT("nodes/comments[%d] has an invalid, duplicate, or unsupported ref/nodeType."), Index));
				continue;
			}
			Refs.Add(Ref);
		}
		const TArray<TSharedPtr<FJsonValue>>* Comments = nullptr;
		if (Definition->TryGetArrayField(TEXT("comments"), Comments) && Comments)
		{
			if (Comments->Num() > MaxBuildComments)
			{
				OutErrors.Add(TEXT("comments exceeds 64 entries."));
			}
			for (int32 Index = 0; Index < Comments->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Comment = (*Comments)[Index].IsValid()
				                                        && (*Comments)[Index]->Type == EJson::Object
					                                        ? (*Comments)[Index]->AsObject()
					                                        : nullptr;
				double Width = 0.0;
				double Height = 0.0;
				if (!Comment.IsValid() || BuildString(Comment, TEXT("text")).IsEmpty()
					|| !Comment->TryGetNumberField(TEXT("width"), Width)
					|| !Comment->TryGetNumberField(TEXT("height"), Height)
					|| Width < 1.0 || Height < 1.0)
				{
					OutErrors.Add(
						FString::Printf(TEXT("comments[%d] requires text and positive width/height."), Index));
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (Definition->TryGetArrayField(TEXT("groups"), Groups) && Groups)
		{
			if (Groups->Num() > 32)
			{
				OutErrors.Add(TEXT("groups exceeds 32 entries."));
			}
			TSet<FString> GroupedRefs;
			TSet<FString> GroupIds;
			for (int32 Index = 0; Index < Groups->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Group = (*Groups)[Index].IsValid()
				                                      && (*Groups)[Index]->Type == EJson::Object
					                                      ? (*Groups)[Index]->AsObject()
					                                      : nullptr;
				const TArray<TSharedPtr<FJsonValue>>* GroupRefs = nullptr;
				const FString GroupId = BuildString(Group, TEXT("id"));
				if (!Group.IsValid() || !IsSafeBuildToken(GroupId)
					|| GroupIds.Contains(GroupId)
					|| !Group->TryGetArrayField(TEXT("refs"), GroupRefs)
					|| !GroupRefs || GroupRefs->IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("groups[%d] is invalid."), Index));
					continue;
				}
				GroupIds.Add(GroupId);
				for (const TSharedPtr<FJsonValue>& RefValue : *GroupRefs)
				{
					const FString Ref = RefValue.IsValid() && RefValue->Type == EJson::String
						                    ? RefValue->AsString()
						                    : FString();
					if (!Refs.Contains(Ref) || GroupedRefs.Contains(Ref))
					{
						OutErrors.Add(FString::Printf(
							TEXT("groups[%d] contains an unknown or multiply grouped ref '%s'."), Index, *Ref));
					}
					GroupedRefs.Add(Ref);
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
		if (Definition->TryGetArrayField(TEXT("connections"), Connections) && Connections)
		{
			if (Connections->Num() > MaxBuildConnections)
			{
				OutErrors.Add(TEXT("connections exceeds 256 entries."));
			}
			for (int32 Index = 0; Index < Connections->Num(); ++Index)
			{
				const TSharedPtr<FJsonObject> Connection = (*Connections)[Index].IsValid()
				                                           && (*Connections)[Index]->Type == EJson::Object
					                                           ? (*Connections)[Index]->AsObject()
					                                           : nullptr;
				const FString SourceRef = BuildString(Connection, TEXT("sourceRef"));
				const FString TargetRef = BuildString(Connection, TEXT("targetRef"));
				const TSharedPtr<FJsonObject> SourceNode =
					FindDefinitionNode(Definition, SourceRef);
				const TSharedPtr<FJsonObject> TargetNode =
					FindDefinitionNode(Definition, TargetRef);
				if (!Connection.IsValid()
					|| !Refs.Contains(SourceRef)
					|| !Refs.Contains(TargetRef)
					|| !SourceNode.IsValid() || !TargetNode.IsValid()
					|| BuildString(SourceNode, TEXT("nodeType")) == TEXT("Comment")
					|| BuildString(TargetNode, TEXT("nodeType")) == TEXT("Comment")
					|| BuildString(Connection, TEXT("sourcePin")).IsEmpty()
					|| BuildString(Connection, TEXT("targetPin")).IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("connections[%d] is invalid."), Index));
				}
			}
		}
		return OutErrors.IsEmpty();
	}

	TSharedPtr<FJsonObject> LoadManagedDefinition(UBlueprint* Blueprint, const FString& BuildId, const FString& Graph)
	{
		if (!Blueprint || !Blueprint->GetOutermost())
		{
			return nullptr;
		}
		const FString Text = Blueprint->GetOutermost()->GetMetaData()->GetValue(
			Blueprint,
			*MetadataKey(BuildId, Graph));
		return Text.IsEmpty() ? nullptr : ParseBuildJson(Text);
	}

	class FBuildGraphValidateTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.graph.build.validate"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const TSharedPtr<FJsonObject>* Definition = nullptr;
			TArray<FString> Errors;
			if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("definition"), Definition)
				|| !Definition || !Definition->IsValid())
			{
				Errors.Add(TEXT("definition must be an object."));
			}
			else
			{
				ValidateBuildDefinition(*Definition, Errors);
			}
			TArray<TSharedPtr<FJsonValue>> Diagnostics;
			for (const FString& Error : Errors)
			{
				Diagnostics.Add(MakeShared<FJsonValueString>(Error));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("valid"), Errors.IsEmpty());
			Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FBuildGraphDefinitionGetTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.graph.build.definition.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Error;
			UBlueprint* Blueprint = LoadBuildBlueprint(BuildString(Params, TEXT("blueprint")), Error);
			if (!Blueprint)
			{
				return FMCPToolResult::Error(Error, TEXT("asset_not_found"), 404);
			}
			const FString BuildId = BuildString(Params, TEXT("buildId"));
			const FString Graph = BuildString(Params, TEXT("graph"));
			const TSharedPtr<FJsonObject> Definition = LoadManagedDefinition(Blueprint, BuildId, Graph);
			if (!Definition.IsValid())
			{
				return FMCPToolResult::Error(
					TEXT("Managed BuildGraph definition was not found."), TEXT("job_not_found"), 404);
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("definition"), Definition);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FBuildGraphMetadataSetTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.graph.build.metadata.set"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Error;
			UBlueprint* Blueprint = LoadBuildBlueprint(BuildString(Params, TEXT("blueprint")), Error);
			const TSharedPtr<FJsonObject>* Definition = nullptr;
			if (!Blueprint || !Params->TryGetObjectField(TEXT("definition"), Definition)
				|| !Definition || !Definition->IsValid())
			{
				return FMCPToolResult::Error(
					Blueprint ? TEXT("definition is required.") : Error,
					TEXT("invalid_params"),
					422);
			}
			const FString BuildId = BuildString(Params, TEXT("buildId"));
			const FString Graph = BuildString(Params, TEXT("graph"));
			const FString Ref = BuildString(Params, TEXT("ref"));
			const FString NodeId = BuildString(Params, TEXT("nodeId"));
			FGuid NodeGuid;
			if (!IsSafeBuildToken(Ref) || !FGuid::Parse(NodeId, NodeGuid)
				|| !ManagedNodeExists(Blueprint, Graph, NodeId))
			{
				return FMCPToolResult::Error(
					TEXT("ref and a currently resolvable nodeId are required."),
					TEXT("buildgraph_managed_node_conflict"),
					409);
			}

			bool bResetManagedRefs = false;
			Params->TryGetBoolField(TEXT("resetManagedRefs"), bResetManagedRefs);
			TSharedPtr<FJsonObject> ManagedRefs = MakeShared<FJsonObject>();
			if (!bResetManagedRefs)
			{
				const TSharedPtr<FJsonObject> Existing =
					LoadManagedDefinition(Blueprint, BuildId, Graph);
				const TSharedPtr<FJsonObject>* ExistingRefs = nullptr;
				if (Existing.IsValid()
					&& Existing->TryGetObjectField(TEXT("managedRefs"), ExistingRefs)
					&& ExistingRefs && ExistingRefs->IsValid())
				{
					ManagedRefs->Values = (*ExistingRefs)->Values;
				}
			}
			const TSharedPtr<FJsonObject>* SeedRefs = nullptr;
			if (Params->TryGetObjectField(TEXT("seedManagedRefs"), SeedRefs)
				&& SeedRefs && SeedRefs->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SeedRefs)->Values)
				{
					if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
					{
						ManagedRefs->SetStringField(Pair.Key, Pair.Value->AsString());
					}
				}
			}
			ManagedRefs->SetStringField(Ref, NodeId);
			TSharedPtr<FJsonObject> Stored = MakeShared<FJsonObject>();
			Stored->Values = (*Definition)->Values;
			Stored->SetObjectField(TEXT("managedRefs"), ManagedRefs);
			Blueprint->Modify();
			Blueprint->GetOutermost()->GetMetaData()->SetValue(
				Blueprint,
				*MetadataKey(BuildId, Graph),
				*StringifyBuildJson(Stored));
			Blueprint->MarkPackageDirty();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("managedRefCount"), ManagedRefs->Values.Num());
			Result->SetStringField(TEXT("ref"), Ref);
			Result->SetStringField(TEXT("nodeId"), NodeId);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FBuildGraphPlanTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.graph.build.plan"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const TSharedPtr<FJsonObject>* DefinitionPtr = nullptr;
			if (!Params.IsValid() || !Params->TryGetObjectField(TEXT("definition"), DefinitionPtr)
				|| !DefinitionPtr || !DefinitionPtr->IsValid())
			{
				return FMCPToolResult::Error(TEXT("definition must be an object."), TEXT("invalid_params"), 422);
			}
			const TSharedPtr<FJsonObject> Definition = *DefinitionPtr;
			TArray<FString> Errors;
			if (!ValidateBuildDefinition(Definition, Errors))
			{
				return FMCPToolResult::Error(Errors[0], TEXT("workflow_plan_failed"), 422);
			}
			const FString BlueprintPath = BuildString(Definition, TEXT("blueprint"));
			const FString Graph = BuildString(Definition, TEXT("graph"));
			const FString BuildId = BuildString(Definition, TEXT("buildId"));
			FString LoadError;
			UBlueprint* Blueprint = LoadBuildBlueprint(BlueprintPath, LoadError);
			if (!Blueprint)
			{
				return FMCPToolResult::Error(LoadError, TEXT("asset_not_found"), 404);
			}

			TSharedRef<FJsonObject> Workflow = MakeShared<FJsonObject>();
			Workflow->SetStringField(TEXT("dsl"), TEXT("ue.workflow"));
			Workflow->SetStringField(TEXT("dslVersion"), TEXT("2.0"));
			Workflow->SetStringField(TEXT("workflowKind"), TEXT("assetEdit"));
			Workflow->SetStringField(TEXT("workflowId"), FString::Printf(TEXT("buildgraph-%s"), *BuildId));
			TSharedRef<FJsonObject> Scopes = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Scope = MakeShared<FJsonObject>();
			Scope->SetStringField(TEXT("kind"), TEXT("blueprint"));
			Scope->SetStringField(TEXT("asset"), BlueprintPath);
			TSharedRef<FJsonObject> Verify = MakeShared<FJsonObject>();
			Verify->SetBoolField(TEXT("compile"), true);
			Verify->SetArrayField(
				TEXT("readBack"),
				{MakeShared<FJsonValueString>(TEXT("graphs"))});
			Scope->SetObjectField(TEXT("verify"), Verify);
			Scopes->SetObjectField(TEXT("primary"), Scope);
			Workflow->SetObjectField(TEXT("scopes"), Scopes);
			Workflow->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));

			TArray<TSharedPtr<FJsonValue>> Operations;
			TSharedRef<FJsonObject> ManagedRefs = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject> Existing = LoadManagedDefinition(Blueprint, BuildId, Graph);
			const TSharedPtr<FJsonObject>* ExistingRefs = nullptr;
			if (Existing.IsValid())
			{
				Existing->TryGetObjectField(TEXT("managedRefs"), ExistingRefs);
			}
			// Build-from-spec owns variables and SCS components as well as graph
			// topology. Keep these operations in the same Workflow so a failure in
			// any phase rolls back the complete authored change.
			const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
			if (Definition->TryGetArrayField(TEXT("variables"), Variables) && Variables)
			{
				for (int32 Index = 0; Index < Variables->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Variable = (*Variables)[Index]->AsObject();
					TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
					Operation->SetStringField(TEXT("id"), FString::Printf(TEXT("variable-%d"), Index));
					Operation->SetStringField(TEXT("scope"), TEXT("primary"));
					Operation->SetStringField(TEXT("type"), TEXT("blueprint.variable.add"));
					TSharedRef<FJsonObject> OpParams = MakeShared<FJsonObject>();
					for (const TCHAR* Field : {
						     TEXT("variableName"), TEXT("variableType"), TEXT("category"), TEXT("defaultValue")
					     })
					{
						if (Variable->HasField(Field))
						{
							OpParams->SetField(Field, Variable->TryGetField(Field));
						}
					}
					if (Variable->HasField(TEXT("isArray")))
					{
						OpParams->SetField(TEXT("isArray"), Variable->TryGetField(TEXT("isArray")));
					}
					Operation->SetObjectField(TEXT("params"), OpParams);
					Operations.Add(MakeShared<FJsonValueObject>(Operation));
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (Definition->TryGetArrayField(TEXT("components"), Components) && Components)
			{
				for (int32 Index = 0; Index < Components->Num(); ++Index)
				{
					const TSharedPtr<FJsonObject> Component = (*Components)[Index]->AsObject();
					TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
					Operation->SetStringField(TEXT("id"), FString::Printf(TEXT("component-%d"), Index));
					Operation->SetStringField(TEXT("scope"), TEXT("primary"));
					Operation->SetStringField(TEXT("type"), TEXT("blueprint.component.add"));
					TSharedRef<FJsonObject> OpParams = MakeShared<FJsonObject>();
					for (const TCHAR* Field : {TEXT("componentClass"), TEXT("name"), TEXT("parentComponent")})
					{
						if (Component->HasField(Field))
						{
							OpParams->SetField(Field, Component->TryGetField(Field));
						}
					}
					Operation->SetObjectField(TEXT("params"), OpParams);
					Operations.Add(MakeShared<FJsonValueObject>(Operation));
				}
			}
			const TArray<TSharedPtr<FJsonValue>> Nodes = CollectBuildNodes(Definition);
			TMap<FString, FString> NodeOperationIds;
			TSet<FString> WantedRefs;
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const FString Ref = BuildString(Nodes[NodeIndex]->AsObject(), TEXT("ref"));
				WantedRefs.Add(Ref);
				NodeOperationIds.Add(Ref, FString::Printf(TEXT("node-%d"), NodeIndex));
			}
			if (BuildString(Definition, TEXT("mode")) == TEXT("replaceManaged")
				&& ExistingRefs && ExistingRefs->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*ExistingRefs)->Values)
				{
					if (WantedRefs.Contains(Pair.Key) || !Pair.Value.IsValid()
						|| Pair.Value->Type != EJson::String)
					{
						continue;
					}
					TSharedRef<FJsonObject> Delete = MakeShared<FJsonObject>();
					Delete->SetStringField(
						TEXT("id"),
						FString::Printf(TEXT("delete-%d"), Operations.Num()));
					Delete->SetStringField(TEXT("scope"), TEXT("primary"));
					Delete->SetStringField(TEXT("type"), TEXT("blueprint.node.delete"));
					TSharedRef<FJsonObject> DeleteParams = MakeShared<FJsonObject>();
					DeleteParams->SetStringField(TEXT("nodeId"), Pair.Value->AsString());
					Delete->SetObjectField(TEXT("params"), DeleteParams);
					Operations.Add(MakeShared<FJsonValueObject>(Delete));
				}
			}
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const TSharedPtr<FJsonValue>& Value = Nodes[NodeIndex];
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				const FString Ref = BuildString(Node, TEXT("ref"));
				FString ExistingGuid;
				if (ExistingRefs && ExistingRefs->IsValid())
				{
					(*ExistingRefs)->TryGetStringField(Ref, ExistingGuid);
				}
				if (!ExistingGuid.IsEmpty())
				{
					if (!ManagedNodeExists(Blueprint, Graph, ExistingGuid))
					{
						return FMCPToolResult::Error(
							FString::Printf(TEXT("Managed ref '%s' no longer resolves to its recorded node."), *Ref),
							TEXT("buildgraph_managed_node_conflict"),
							409);
					}
					const TSharedPtr<FJsonObject> PriorNode =
						FindDefinitionNode(Existing, Ref);
					if (PriorNode.IsValid()
						&& BuildString(PriorNode, TEXT("nodeType"))
						!= BuildString(Node, TEXT("nodeType")))
					{
						return FMCPToolResult::Error(
							FString::Printf(
								TEXT(
									"Managed ref '%s' changed nodeType; replaceManaged never overwrites a managed node in place."),
								*Ref),
							TEXT("buildgraph_managed_node_conflict"),
							409);
					}
					ManagedRefs->SetStringField(Ref, ExistingGuid);
					double PosX = 0.0;
					double PosY = 0.0;
					if (Node->TryGetNumberField(TEXT("posX"), PosX)
						&& Node->TryGetNumberField(TEXT("posY"), PosY))
					{
						TSharedRef<FJsonObject> Move = MakeShared<FJsonObject>();
						Move->SetStringField(
							TEXT("id"),
							FString::Printf(TEXT("move-%d"), NodeIndex));
						Move->SetStringField(TEXT("scope"), TEXT("primary"));
						Move->SetStringField(TEXT("type"), TEXT("blueprint.node.move"));
						TSharedRef<FJsonObject> MoveParams = MakeShared<FJsonObject>();
						MoveParams->SetStringField(TEXT("nodeId"), ExistingGuid);
						MoveParams->SetNumberField(TEXT("posX"), PosX);
						MoveParams->SetNumberField(TEXT("posY"), PosY);
						Move->SetObjectField(TEXT("params"), MoveParams);
						Operations.Add(MakeShared<FJsonValueObject>(Move));
					}
					continue;
				}
				TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
				Operation->SetStringField(TEXT("id"), NodeOperationIds[Ref]);
				Operation->SetStringField(TEXT("scope"), TEXT("primary"));
				Operation->SetStringField(TEXT("type"), TEXT("blueprint.node.add"));
				TSharedRef<FJsonObject> NodeParams = MakeShared<FJsonObject>();
				NodeParams->Values = Node->Values;
				NodeParams->Values.Remove(TEXT("ref"));
				NodeParams->Values.Remove(TEXT("pinDefaults"));
				NodeParams->SetStringField(TEXT("graph"), Graph);
				Operation->SetObjectField(TEXT("params"), NodeParams);
				Operations.Add(MakeShared<FJsonValueObject>(Operation));
			}

			const TArray<TSharedPtr<FJsonValue>>* Comments = nullptr;
			if (Definition->TryGetArrayField(TEXT("comments"), Comments) && Comments)
			{
				for (int32 CommentIndex = 0; CommentIndex < Comments->Num(); ++CommentIndex)
				{
					const TSharedPtr<FJsonValue>& Value = (*Comments)[CommentIndex];
					const TSharedPtr<FJsonObject> Comment = Value->AsObject();
					const FString Ref = BuildString(Comment, TEXT("ref"));
					TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
					Bounds->SetStringField(
						TEXT("id"),
						FString::Printf(TEXT("comment-bounds-%d"), CommentIndex));
					Bounds->SetStringField(TEXT("scope"), TEXT("primary"));
					Bounds->SetStringField(TEXT("type"), TEXT("blueprint.comment.bounds.set"));
					TSharedRef<FJsonObject> BoundsParams = MakeShared<FJsonObject>();
					BoundsParams->SetStringField(TEXT("graph"), Graph);
					for (const TCHAR* Field : {TEXT("x"), TEXT("y"), TEXT("width"), TEXT("height")})
					{
						double Number = 0.0;
						Comment->TryGetNumberField(Field, Number);
						BoundsParams->SetNumberField(Field, Number);
					}
					FString ExistingGuid;
					if (ManagedRefs->TryGetStringField(Ref, ExistingGuid))
					{
						BoundsParams->SetStringField(TEXT("commentNodeId"), ExistingGuid);
					}
					else
					{
						TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
						Binding->SetStringField(TEXT("from"), NodeOperationIds[Ref]);
						Binding->SetStringField(TEXT("path"), TEXT("/nodeId"));
						TSharedRef<FJsonObject> Bindings = MakeShared<FJsonObject>();
						Bindings->SetObjectField(TEXT("/params/commentNodeId"), Binding);
						Bounds->SetObjectField(TEXT("bindings"), Bindings);
					}
					Bounds->SetObjectField(TEXT("params"), BoundsParams);
					Operations.Add(MakeShared<FJsonValueObject>(Bounds));
				}
			}
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const TSharedPtr<FJsonValue>& Value = Nodes[NodeIndex];
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				const FString Ref = BuildString(Node, TEXT("ref"));
				const TSharedPtr<FJsonObject>* Defaults = nullptr;
				if (!Node->TryGetObjectField(TEXT("pinDefaults"), Defaults)
					|| !Defaults || !Defaults->IsValid())
				{
					continue;
				}
				int32 DefaultIndex = 0;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Defaults)->Values)
				{
					if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String)
					{
						continue;
					}
					TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
					Operation->SetStringField(
						TEXT("id"),
						FString::Printf(TEXT("default-%d-%d"), NodeIndex, DefaultIndex++));
					Operation->SetStringField(TEXT("scope"), TEXT("primary"));
					Operation->SetStringField(TEXT("type"), TEXT("blueprint.pin.default.set"));
					TSharedRef<FJsonObject> OpParams = MakeShared<FJsonObject>();
					OpParams->SetStringField(TEXT("pinName"), Pair.Key);
					OpParams->SetStringField(TEXT("value"), Pair.Value->AsString());
					FString ExistingGuid;
					if (ManagedRefs->TryGetStringField(Ref, ExistingGuid))
					{
						OpParams->SetStringField(TEXT("nodeId"), ExistingGuid);
					}
					else
					{
						TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
						Binding->SetStringField(TEXT("from"), NodeOperationIds[Ref]);
						Binding->SetStringField(TEXT("path"), TEXT("/nodeId"));
						TSharedRef<FJsonObject> Bindings = MakeShared<FJsonObject>();
						Bindings->SetObjectField(TEXT("/params/nodeId"), Binding);
						Operation->SetObjectField(TEXT("bindings"), Bindings);
					}
					Operation->SetObjectField(TEXT("params"), OpParams);
					Operations.Add(MakeShared<FJsonValueObject>(Operation));
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			if (Definition->TryGetArrayField(TEXT("connections"), Connections) && Connections)
			{
				int32 ConnectionIndex = 0;
				for (const TSharedPtr<FJsonValue>& Value : *Connections)
				{
					const TSharedPtr<FJsonObject> Connection = Value->AsObject();
					const FString SourceRef = BuildString(Connection, TEXT("sourceRef"));
					const FString TargetRef = BuildString(Connection, TEXT("targetRef"));
					TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
					Operation->SetStringField(TEXT("id"), FString::Printf(TEXT("connect-%d"), ConnectionIndex++));
					Operation->SetStringField(TEXT("scope"), TEXT("primary"));
					Operation->SetStringField(TEXT("type"), TEXT("blueprint.pin.connect"));
					TSharedRef<FJsonObject> OpParams = MakeShared<FJsonObject>();
					OpParams->SetStringField(TEXT("sourcePinName"), BuildString(Connection, TEXT("sourcePin")));
					OpParams->SetStringField(TEXT("targetPinName"), BuildString(Connection, TEXT("targetPin")));
					TSharedRef<FJsonObject> Bindings = MakeShared<FJsonObject>();
					auto BindRef = [&](const FString& Ref, const TCHAR* Destination)
					{
						FString ExistingGuid;
						if (ManagedRefs->TryGetStringField(Ref, ExistingGuid))
						{
							OpParams->SetStringField(Destination, ExistingGuid);
						}
						else
						{
							TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
							Binding->SetStringField(TEXT("from"), NodeOperationIds[Ref]);
							Binding->SetStringField(TEXT("path"), TEXT("/nodeId"));
							Bindings->SetObjectField(FString(TEXT("/params/")) + Destination, Binding);
						}
					};
					BindRef(SourceRef, TEXT("sourceNodeId"));
					BindRef(TargetRef, TEXT("targetNodeId"));
					Operation->SetObjectField(TEXT("params"), OpParams);
					if (!Bindings->Values.IsEmpty())
					{
						Operation->SetObjectField(TEXT("bindings"), Bindings);
					}
					Operations.Add(MakeShared<FJsonValueObject>(Operation));
				}
			}

			TSharedRef<FJsonObject> SeedManagedRefs = MakeShared<FJsonObject>();
			if (BuildString(Definition, TEXT("mode")) == TEXT("merge")
				&& ExistingRefs && ExistingRefs->IsValid())
			{
				SeedManagedRefs->Values = (*ExistingRefs)->Values;
			}
			for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
			{
				const FString Ref = BuildString(Nodes[NodeIndex]->AsObject(), TEXT("ref"));
				TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
				Metadata->SetStringField(
					TEXT("id"),
					FString::Printf(TEXT("buildgraph-metadata-%d"), NodeIndex));
				Metadata->SetStringField(TEXT("scope"), TEXT("primary"));
				Metadata->SetStringField(TEXT("type"), TEXT("blueprint.graph.build.metadata.set"));
				TSharedRef<FJsonObject> MetadataParams = MakeShared<FJsonObject>();
				MetadataParams->SetStringField(TEXT("buildId"), BuildId);
				MetadataParams->SetStringField(TEXT("graph"), Graph);
				MetadataParams->SetStringField(TEXT("ref"), Ref);
				MetadataParams->SetObjectField(TEXT("definition"), Definition);
				if (NodeIndex == 0)
				{
					MetadataParams->SetBoolField(TEXT("resetManagedRefs"), true);
					MetadataParams->SetObjectField(TEXT("seedManagedRefs"), SeedManagedRefs);
				}
				else
				{
					Metadata->SetArrayField(
						TEXT("dependsOn"),
						{
							MakeShared<FJsonValueString>(FString::Printf(
								TEXT("buildgraph-metadata-%d"),
								NodeIndex - 1))
						});
				}
				FString ExistingGuid;
				if (ManagedRefs->TryGetStringField(Ref, ExistingGuid))
				{
					MetadataParams->SetStringField(TEXT("nodeId"), ExistingGuid);
				}
				else
				{
					TSharedRef<FJsonObject> Binding = MakeShared<FJsonObject>();
					Binding->SetStringField(TEXT("from"), NodeOperationIds[Ref]);
					Binding->SetStringField(TEXT("path"), TEXT("/nodeId"));
					TSharedRef<FJsonObject> Bindings = MakeShared<FJsonObject>();
					Bindings->SetObjectField(TEXT("/params/nodeId"), Binding);
					Metadata->SetObjectField(TEXT("bindings"), Bindings);
				}
				Metadata->SetObjectField(TEXT("params"), MetadataParams);
				Operations.Add(MakeShared<FJsonValueObject>(Metadata));
			}
			Workflow->SetArrayField(TEXT("operations"), Operations);
			UUEAIIntegrationSubsystem* Subsystem = GEditor
				                                       ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
				                                       : nullptr;
			FUEAIIntegrationServer* Server = Subsystem ? Subsystem->GetServer() : nullptr;
			const FMCPResult Plan = Server
				                        ? Server->PlanWorkflowDefinition(Workflow)
				                        : FMCPResult::Fail(
					                        TEXT("workflow_runtime_unavailable"),
					                        TEXT("Workflow runtime is unavailable."), 503);
			if (!Plan.bOk)
			{
				FString Message = Plan.Error.Message;
				if (Plan.Error.Details.IsValid())
				{
					Message += TEXT(" Core diagnostics: ")
						+ StringifyBuildJson(Plan.Error.Details);
				}
				return FMCPToolResult::Error(
					Message,
					Plan.Error.Code,
					Plan.Error.HttpStatus);
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("workflow"), Workflow);
			Result->SetObjectField(TEXT("plan"), Plan.Data);
			Result->SetStringField(TEXT("planDigest"), BuildString(Plan.Data, TEXT("planDigest")));
			Result->SetStringField(
				TEXT("graphHash"),
				UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint));
			Result->SetObjectField(TEXT("managedRefs"), ManagedRefs);
			return FMCPToolResult::Ok(Result);
		}
	};

	/**
	 * Execute the declarative BuildGraph through the Editor-bound Workflow
	 * runtime.  Planning is intentionally repeated here so the approved digest
	 * is prepared against the same current Blueprint state that will execute;
	 * the runtime then owns the transaction, journal, compile/read-back, and
	 * rollback boundary.
	 */
	class FBuildGraphExecuteTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("blueprint.graph.build.execute");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const TSharedPtr<FJsonObject>* Definition = nullptr;
			if (!Params.IsValid()
				|| !Params->TryGetObjectField(TEXT("definition"), Definition)
				|| !Definition
				|| !Definition->IsValid())
			{
				return FMCPToolResult::Error(
					TEXT("definition must be an object."),
					TEXT("invalid_params"),
					422);
			}

			TSharedRef<FJsonObject> PlanParams = MakeShared<FJsonObject>();
			PlanParams->SetObjectField(TEXT("definition"), *Definition);
			FBuildGraphPlanTool Planner;
			const FMCPToolResult Planned = Planner.Execute(PlanParams);
			if (!Planned.bSuccess || !Planned.Data.IsValid())
			{
				return Planned;
			}

			const TSharedPtr<FJsonObject>* Workflow = nullptr;
			if (!Planned.Data->TryGetObjectField(TEXT("workflow"), Workflow)
				|| !Workflow
				|| !Workflow->IsValid())
			{
				return FMCPToolResult::Error(
					TEXT("BuildGraph planner returned no Workflow definition."),
					TEXT("workflow_plan_failed"),
					500);
			}
			const FString PlanDigest = BuildString(
				Planned.Data,
				TEXT("planDigest"));
			if (PlanDigest.IsEmpty())
			{
				return FMCPToolResult::Error(
					TEXT("BuildGraph planner returned no Editor-bound plan digest."),
					TEXT("workflow_plan_failed"),
					500);
			}

			UUEAIIntegrationSubsystem* Subsystem = GEditor
				                                       ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
				                                       : nullptr;
			FUEAIIntegrationServer* Server = Subsystem ? Subsystem->GetServer() : nullptr;
			if (!Server)
			{
				return FMCPToolResult::Error(
					TEXT("Workflow runtime is unavailable."),
					TEXT("workflow_runtime_unavailable"),
					503);
			}

			bool bSaveOnSuccess = true;
			if (Params->HasField(TEXT("saveOnSuccess"))
				&& !Params->TryGetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess))
			{
				return FMCPToolResult::Error(
					TEXT("saveOnSuccess must be a boolean."),
					TEXT("invalid_params"),
					422);
			}
			bool bConfirmWrite = true;
			if (Params->HasField(TEXT("confirmWrite"))
				&& !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite))
			{
				return FMCPToolResult::Error(
					TEXT("confirmWrite must be a boolean."),
					TEXT("invalid_params"),
					422);
			}
			FString RequestId;
			if (Params->HasField(TEXT("requestId"))
				&& (!Params->TryGetStringField(TEXT("requestId"), RequestId)
					|| RequestId.IsEmpty()))
			{
				return FMCPToolResult::Error(
					TEXT("requestId must be a non-empty string when provided."),
					TEXT("invalid_params"),
					422);
			}

			const FMCPResult Executed = Server->ExecuteWorkflowDefinition(
				*Workflow,
				PlanDigest,
				bSaveOnSuccess,
				bConfirmWrite,
				RequestId);
			if (!Executed.bOk)
			{
				return FMCPToolResult::Error(
					Executed.Error.Message,
					Executed.Error.Code,
					Executed.Error.HttpStatus);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint-buildgraph.v1"));
			Result->SetStringField(TEXT("planDigest"), PlanDigest);
			Result->SetObjectField(TEXT("workflow"), *Workflow);
			Result->SetObjectField(TEXT("execution"), Executed.Data);
			Result->SetBoolField(TEXT("saved"), bSaveOnSuccess);
			return FMCPToolResult::Ok(Result);
		}
	};

	/**
	 * Monolith-compatible one-shot authoring surface. The compact spec is
	 * normalized into the managed BuildGraph contract so it receives the same
	 * approval, transaction, compile, read-back, and rollback guarantees.
	 */
	class FBuildBlueprintFromSpecTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("blueprint.build.from_spec");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			return NormalizeAndDispatch(Params, false);
		}

		// Named templates share the spec normalizer, but planning never enters
		// the write path or manufactures approval for the generated Workflow.
		FMCPToolResult Plan(const TSharedPtr<FJsonObject>& Params)
		{
			return NormalizeAndDispatch(Params, true);
		}

	private:
		FMCPToolResult NormalizeAndDispatch(
			const TSharedPtr<FJsonObject>& Params,
			const bool bPlanOnly)
		{
			if (!Params.IsValid())
			{
				return FMCPToolResult::Error(TEXT("parameters are required."), TEXT("invalid_params"), 422);
			}
			FString BlueprintPath;
			if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintPath) || BlueprintPath.IsEmpty())
			{
				return FMCPToolResult::Error(TEXT("blueprint is required."), TEXT("invalid_params"), 422);
			}
			TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
			Definition->SetStringField(TEXT("schema"), TEXT("ue.blueprint-buildgraph.v1"));
			Definition->SetStringField(TEXT("blueprint"), BlueprintPath);
			Definition->SetStringField(TEXT("graph"), BuildString(Params, TEXT("graph")));
			if (BuildString(Definition, TEXT("graph")).IsEmpty())
			{
				Definition->SetStringField(TEXT("graph"), TEXT("EventGraph"));
			}
			FString BuildId = BuildString(Params, TEXT("buildId"));
			Definition->SetStringField(TEXT("buildId"), BuildId.IsEmpty() ? TEXT("monolith-spec") : BuildId);
			FString Mode = BuildString(Params, TEXT("mode"));
			Definition->SetStringField(TEXT("mode"), Mode.IsEmpty() ? TEXT("merge") : Mode);

			const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
			if (Params->TryGetArrayField(TEXT("variables"), Variables) && Variables)
			{
				TArray<TSharedPtr<FJsonValue>> Converted;
				for (const TSharedPtr<FJsonValue>& Value : *Variables)
				{
					const TSharedPtr<FJsonObject> Source = Value.IsValid() && Value->Type == EJson::Object
						                                       ? Value->AsObject()
						                                       : nullptr;
					if (!Source.IsValid()) { continue; }
					TSharedRef<FJsonObject> Variable = MakeShared<FJsonObject>();
					Variable->SetStringField(TEXT("variableName"), BuildString(Source, TEXT("variableName")).IsEmpty()
						                                               ? BuildString(Source, TEXT("name"))
						                                               : BuildString(Source, TEXT("variableName")));
					Variable->SetStringField(TEXT("variableType"), BuildString(Source, TEXT("variableType")).IsEmpty()
						                                               ? BuildString(Source, TEXT("type"))
						                                               : BuildString(Source, TEXT("variableType")));
					for (const TCHAR* Field : {TEXT("category"), TEXT("defaultValue"), TEXT("isArray")})
					{
						const TCHAR* Alias = FCString::Strcmp(Field, TEXT("defaultValue")) == 0
							                     ? TEXT("default_value")
							                     : Field;
						if (Source->HasField(Field)) Variable->SetField(Field, Source->TryGetField(Field));
						else if (Source->HasField(Alias)) Variable->SetField(Field, Source->TryGetField(Alias));
					}
					Converted.Add(MakeShared<FJsonValueObject>(Variable));
				}
				Definition->SetArrayField(TEXT("variables"), Converted);
			}
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (Params->TryGetArrayField(TEXT("components"), Components) && Components)
			{
				TArray<TSharedPtr<FJsonValue>> Converted;
				for (const TSharedPtr<FJsonValue>& Value : *Components)
				{
					const TSharedPtr<FJsonObject> Source = Value.IsValid() && Value->Type == EJson::Object
						                                       ? Value->AsObject()
						                                       : nullptr;
					if (!Source.IsValid()) { continue; }
					TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
					Component->SetStringField(TEXT("name"), BuildString(Source, TEXT("name")));
					Component->SetStringField(
						TEXT("componentClass"), BuildString(Source, TEXT("componentClass")).IsEmpty()
							                        ? BuildString(Source, TEXT("class"))
							                        : BuildString(Source, TEXT("componentClass")));
					if (Source->HasField(TEXT("parent")))
						Component->SetField(
							TEXT("parentComponent"), Source->TryGetField(TEXT("parent")));
					if (Source->HasField(TEXT("parentComponent")))
						Component->SetField(
							TEXT("parentComponent"), Source->TryGetField(TEXT("parentComponent")));
					Converted.Add(MakeShared<FJsonValueObject>(Component));
				}
				Definition->SetArrayField(TEXT("components"), Converted);
			}

			const TArray<TSharedPtr<FJsonValue>>* SpecNodes = nullptr;
			TArray<TSharedPtr<FJsonValue>> Nodes;
			if (Params->TryGetArrayField(TEXT("nodes"), SpecNodes) && SpecNodes)
			{
				for (const TSharedPtr<FJsonValue>& Value : *SpecNodes)
				{
					const TSharedPtr<FJsonObject> Source = Value.IsValid() && Value->Type == EJson::Object
						                                       ? Value->AsObject()
						                                       : nullptr;
					if (!Source.IsValid()) { continue; }
					TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
					Node->SetStringField(TEXT("ref"), BuildString(Source, TEXT("ref")).IsEmpty()
						                                  ? BuildString(Source, TEXT("id"))
						                                  : BuildString(Source, TEXT("ref")));
					Node->SetStringField(TEXT("nodeType"), BuildString(Source, TEXT("nodeType")).IsEmpty()
						                                       ? BuildString(Source, TEXT("type"))
						                                       : BuildString(Source, TEXT("nodeType")));
					for (const TCHAR* Field : {
						     TEXT("functionName"), TEXT("className"), TEXT("variableName"), TEXT("eventName"),
						     TEXT("typeName"), TEXT("castTarget"), TEXT("comment")
					     })
					{
						if (Source->HasField(Field)) Node->SetField(Field, Source->TryGetField(Field));
					}
					const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
					if (Source->TryGetArrayField(TEXT("position"), Position) && Position && Position->Num() >= 2)
					{
						Node->SetNumberField(TEXT("posX"), (*Position)[0]->AsNumber());
						Node->SetNumberField(TEXT("posY"), (*Position)[1]->AsNumber());
					}
					if (Source->HasField(TEXT("pin_defaults")))
						Node->SetField(
							TEXT("pinDefaults"), Source->TryGetField(TEXT("pin_defaults")));
					if (Source->HasField(TEXT("pinDefaults")))
						Node->SetField(
							TEXT("pinDefaults"), Source->TryGetField(TEXT("pinDefaults")));
					Nodes.Add(MakeShared<FJsonValueObject>(Node));
				}
			}
			Definition->SetArrayField(TEXT("nodes"), Nodes);

			const TArray<TSharedPtr<FJsonValue>>* SpecConnections = nullptr;
			if (Params->TryGetArrayField(TEXT("connections"), SpecConnections) && SpecConnections)
			{
				TArray<TSharedPtr<FJsonValue>> Connections;
				for (const TSharedPtr<FJsonValue>& Value : *SpecConnections)
				{
					const TSharedPtr<FJsonObject> Source = Value.IsValid() && Value->Type == EJson::Object
						                                       ? Value->AsObject()
						                                       : nullptr;
					if (!Source.IsValid()) { continue; }
					TSharedRef<FJsonObject> Connection = MakeShared<FJsonObject>();
					Connection->SetStringField(TEXT("sourceRef"), BuildString(Source, TEXT("sourceRef")).IsEmpty()
						                                              ? BuildString(Source, TEXT("source"))
						                                              : BuildString(Source, TEXT("sourceRef")));
					Connection->SetStringField(TEXT("targetRef"), BuildString(Source, TEXT("targetRef")).IsEmpty()
						                                              ? BuildString(Source, TEXT("target"))
						                                              : BuildString(Source, TEXT("targetRef")));
					Connection->SetStringField(TEXT("sourcePin"), BuildString(Source, TEXT("sourcePin")).IsEmpty()
						                                              ? BuildString(Source, TEXT("source_pin"))
						                                              : BuildString(Source, TEXT("sourcePin")));
					Connection->SetStringField(TEXT("targetPin"), BuildString(Source, TEXT("targetPin")).IsEmpty()
						                                              ? BuildString(Source, TEXT("target_pin"))
						                                              : BuildString(Source, TEXT("targetPin")));
					Connections.Add(MakeShared<FJsonValueObject>(Connection));
				}
				Definition->SetArrayField(TEXT("connections"), Connections);
			}
			TSharedRef<FJsonObject> ExecuteParams = MakeShared<FJsonObject>();
			ExecuteParams->SetObjectField(TEXT("definition"), Definition);
			if (bPlanOnly)
			{
				FBuildGraphPlanTool Planner;
				FMCPToolResult Result = Planner.Execute(ExecuteParams);
				if (Result.Data.IsValid())
				{
					Result.Data->SetObjectField(TEXT("normalizedDefinition"), Definition);
				}
				return Result;
			}
			bool bConfirmWrite = true;
			Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite);
			ExecuteParams->SetBoolField(TEXT("confirmWrite"), bConfirmWrite);
			bool bSaveOnSuccess = true;
			Params->TryGetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess);
			ExecuteParams->SetBoolField(TEXT("saveOnSuccess"), bSaveOnSuccess);
			if (Params->HasField(TEXT("requestId")))
				ExecuteParams->SetField(
					TEXT("requestId"), Params->TryGetField(TEXT("requestId")));
			FBuildGraphExecuteTool Executor;
			FMCPToolResult Result = Executor.Execute(ExecuteParams);
			if (Result.Data.IsValid()) Result.Data->SetObjectField(TEXT("normalizedDefinition"), Definition);
			return Result;
		}
	};

	struct FBlueprintBehaviorTemplate
	{
		FString Name;
		FString Description;
		TArray<FString> Limitations;
	};

	const TArray<FBlueprintBehaviorTemplate>& BlueprintBehaviorTemplates()
	{
		static const TArray<FBlueprintBehaviorTemplate> Templates = {
			{
				TEXT("health_system"),
				TEXT("Create no-argument TakeDamage/Heal events that read DamageAmount/HealAmount members and clamp Health into [0, MaxHealth]."),
				{
					TEXT("Events have no arguments. Set the authored DamageAmount/HealAmount members before calling them; negative runtime amounts are treated as zero."),
					TEXT("No replication, death event, damage-source attribution or gameplay integration is generated. Runtime acceptance is separate from planning.")
				}
			},
			{
				TEXT("timer_loop"),
				TEXT("Create StartTimer/StopTimer events, a named guarded callback and LoopCount. Repeated starts reset one looping timer; StopTimer and EndPlay clear it."),
				{
					TEXT("The timer starts only when StartTimer is invoked. delay must be at least 0.001 seconds; callback has no arguments and LoopCount is cumulative across restarts."),
					TEXT("No gameplay callback beyond the observable counter is generated; the timer is limited to at most one callback per frame.")
				}
			},
			{
				TEXT("interactable_actor"),
				TEXT("Create InteractionSphere and an Interact event that increments InteractionCount only while bIsInteractable is true."),
				{
					TEXT("Interaction is invoked explicitly through the no-argument Interact event; overlap, input binding and network replication are not generated."),
					TEXT("Component properties retain engine defaults; requested property edits use the separate local-SCS contract.")
				}
			}
		};
		return Templates;
	}

	const FBlueprintBehaviorTemplate* FindBlueprintBehaviorTemplate(const FString& Name)
	{
		return BlueprintBehaviorTemplates().FindByPredicate(
			[&Name](const FBlueprintBehaviorTemplate& Template)
			{
				return Template.Name == Name;
			});
	}

	TSharedRef<FJsonObject> BlueprintTemplateParameterSchema(const FString& Name)
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		auto AddNumber = [&Properties](const TCHAR* Field, const double Default, const double Maximum, const double Minimum = 0.0)
		{
			TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
			Property->SetStringField(TEXT("type"), TEXT("number"));
			Property->SetNumberField(TEXT("minimum"), Minimum);
			Property->SetNumberField(TEXT("maximum"), Maximum);
			Property->SetNumberField(TEXT("default"), Default);
			Properties->SetObjectField(Field, Property);
		};
		if (Name == TEXT("health_system"))
		{
			AddNumber(TEXT("maxHealth"), 100.0, 1.0e9);
			AddNumber(TEXT("damageAmount"), 10.0, 1.0e9);
			AddNumber(TEXT("healAmount"), 10.0, 1.0e9);
		}
		else if (Name == TEXT("timer_loop"))
		{
			AddNumber(TEXT("delay"), 1.0, 1.0e6, 0.001);
			TSharedRef<FJsonObject> EventName = MakeShared<FJsonObject>();
			EventName->SetStringField(TEXT("type"), TEXT("string"));
			EventName->SetStringField(TEXT("pattern"), TEXT("^[A-Za-z_][A-Za-z0-9_]{0,63}$"));
			EventName->SetStringField(TEXT("default"), TEXT("TimerLoop"));
			Properties->SetObjectField(TEXT("eventName"), EventName);
		}
		else if (Name == TEXT("interactable_actor"))
		{
			auto Enabled = MakeShared<FJsonObject>();
			Enabled->SetStringField(TEXT("type"), TEXT("boolean"));
			Enabled->SetBoolField(TEXT("default"), true);
			Properties->SetObjectField(TEXT("initiallyInteractable"), Enabled);
		}
		Schema->SetObjectField(TEXT("properties"), Properties);
		return Schema;
	}

	TSharedRef<FJsonObject> BlueprintTemplateDescriptor(const FBlueprintBehaviorTemplate& Template)
	{
		TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("name"), Template.Name);
		Descriptor->SetStringField(TEXT("version"), TEXT("2.0"));
		Descriptor->SetStringField(TEXT("scope"), TEXT("minimal_runtime_behavior"));
		Descriptor->SetStringField(TEXT("description"), Template.Description);
		Descriptor->SetObjectField(TEXT("parameterSchema"), BlueprintTemplateParameterSchema(Template.Name));
		TArray<TSharedPtr<FJsonValue>> Limitations;
		for (const FString& Limitation : Template.Limitations)
		{
			Limitations.Add(MakeShared<FJsonValueString>(Limitation));
		}
		Descriptor->SetArrayField(TEXT("limitations"), Limitations);
		return Descriptor;
	}

	bool ResolveBlueprintTemplateParameters(
		const FBlueprintBehaviorTemplate& Template,
		const TSharedPtr<FJsonObject>& Input,
		const TSharedRef<FJsonObject>& Resolved,
		FString& OutError)
	{
		const TSharedPtr<FJsonObject> Properties =
			BlueprintTemplateParameterSchema(Template.Name)->GetObjectField(TEXT("properties"));
		if (Input.IsValid())
		{
			for (const auto& Pair : Input->Values)
			{
				if (!Properties->HasField(Pair.Key))
				{
					OutError = FString::Printf(
						TEXT("Unknown parameter '%s' for template '%s'. Read blueprint.template.list for its parameterSchema."),
						*Pair.Key, *Template.Name);
					return false;
				}
			}
		}
		for (const auto& Pair : Properties->Values)
		{
			const TSharedPtr<FJsonObject> Property = Pair.Value->AsObject();
			const TSharedPtr<FJsonValue> Value = Input.IsValid() && Input->HasField(Pair.Key)
				? Input->TryGetField(Pair.Key)
				: Property->TryGetField(TEXT("default"));
			if (Property->GetStringField(TEXT("type")) == TEXT("number"))
			{
				const double Number = Value.IsValid() && Value->Type == EJson::Number
					? Value->AsNumber()
					: -1.0;
				if (!FMath::IsFinite(Number) || Number < Property->GetNumberField(TEXT("minimum"))
					|| Number > Property->GetNumberField(TEXT("maximum")))
				{
					OutError = FString::Printf(TEXT("parameters.%s must be a finite number in the declared range."), *Pair.Key);
					return false;
				}
				Resolved->SetNumberField(Pair.Key, Number);
			}
			else if (Property->GetStringField(TEXT("type")) == TEXT("boolean"))
			{
				if (!Value.IsValid() || Value->Type != EJson::Boolean)
				{
					OutError = FString::Printf(TEXT("parameters.%s must be a boolean."), *Pair.Key);
					return false;
				}
				Resolved->SetBoolField(Pair.Key, Value->AsBool());
			}
			else
			{
				const FString Name = Value.IsValid() && Value->Type == EJson::String
					? Value->AsString()
					: FString();
				// Match the ASCII identifier schema exactly; generic BuildGraph
				// tokens also permit punctuation that custom event names forbid.
				bool bIdentifier = !Name.IsEmpty() && Name.Len() <= 64;
				for (int32 Index = 0; Index < Name.Len(); ++Index)
				{
					const TCHAR Character = Name[Index];
					bIdentifier &= (Character >= TEXT('A') && Character <= TEXT('Z'))
						|| (Character >= TEXT('a') && Character <= TEXT('z'))
						|| Character == TEXT('_')
						|| (Index > 0 && Character >= TEXT('0') && Character <= TEXT('9'));
				}
				if (!bIdentifier)
				{
					OutError = TEXT("parameters.eventName must be an ASCII identifier of 1 to 64 characters.");
					return false;
				}
				if (Name.Equals(TEXT("StartTimer"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("StopTimer"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("ReceiveEndPlay"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("LoopCount"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("bTimerRunning"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("bTimerEnded"), ESearchCase::IgnoreCase)
					|| Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
				{
					OutError = TEXT("parameters.eventName conflicts with a generated timer declaration or the reserved None name.");
					return false;
				}
				Resolved->SetStringField(Pair.Key, Name);
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> MakeBlueprintTemplateNode(
		const TCHAR* Ref, const TCHAR* NodeType, const int32 X, const int32 Y)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("ref"), Ref);
		Node->SetStringField(TEXT("nodeType"), NodeType);
		Node->SetArrayField(TEXT("position"), {
			MakeShared<FJsonValueNumber>(X), MakeShared<FJsonValueNumber>(Y)});
		return Node;
	}

	TSharedRef<FJsonObject> MakeBlueprintTemplateSpec(
		const FBlueprintBehaviorTemplate& Template,
		const TSharedPtr<FJsonObject>& Input,
		const TSharedRef<FJsonObject>& Parameters)
	{
		TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("blueprint"), BuildString(Input, TEXT("blueprint")));
		Spec->SetStringField(TEXT("graph"), Input->HasField(TEXT("graph"))
			? BuildString(Input, TEXT("graph")) : TEXT("EventGraph"));
		Spec->SetStringField(TEXT("buildId"), Input->HasField(TEXT("buildId"))
			? BuildString(Input, TEXT("buildId")) : TEXT("template-") + Template.Name);
		Spec->SetStringField(TEXT("mode"), TEXT("merge"));
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Variables;
		TArray<TSharedPtr<FJsonValue>> Connections;
		auto AddVariable = [&Variables](const TCHAR* Name, const TCHAR* Type, const FString& Default, const TCHAR* Category)
		{
			TSharedRef<FJsonObject> Variable = MakeShared<FJsonObject>();
			Variable->SetStringField(TEXT("name"), Name);
			Variable->SetStringField(TEXT("type"), Type);
			Variable->SetStringField(TEXT("defaultValue"), Default);
			Variable->SetStringField(TEXT("category"), Category);
			Variables.Add(MakeShared<FJsonValueObject>(Variable));
		};
		auto AddVariableNode = [&Nodes](const TCHAR* Ref, const TCHAR* Type, const TCHAR* Variable, const int32 X, const int32 Y)
		{
			TSharedRef<FJsonObject> Node = MakeBlueprintTemplateNode(Ref, Type, X, Y);
			Node->SetStringField(TEXT("variableName"), Variable);
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
		};
		auto AddEvent = [&Nodes](const TCHAR* Ref, const FString& Name, int32 Y)
		{
			auto Node = MakeBlueprintTemplateNode(Ref, TEXT("CustomEvent"), 0, Y);
			Node->SetStringField(TEXT("eventName"), Name);
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
		};
		auto AddCall = [&Nodes](const TCHAR* Ref, const TCHAR* Function, const TCHAR* Class, int32 X, int32 Y,
			const TSharedPtr<FJsonObject>& Defaults = nullptr)
		{
			auto Node = MakeBlueprintTemplateNode(Ref, TEXT("CallFunction"), X, Y);
			Node->SetStringField(TEXT("functionName"), Function);
			Node->SetStringField(TEXT("className"), Class);
			if (Defaults) Node->SetObjectField(TEXT("pinDefaults"), Defaults);
			Nodes.Add(MakeShared<FJsonValueObject>(Node));
		};
		auto Link = [&Connections](const TCHAR* Source, const TCHAR* SourcePin, const TCHAR* Target, const TCHAR* TargetPin)
		{
			auto Connection = MakeShared<FJsonObject>();
			Connection->SetStringField(TEXT("sourceRef"), Source);
			Connection->SetStringField(TEXT("sourcePin"), SourcePin);
			Connection->SetStringField(TEXT("targetRef"), Target);
			Connection->SetStringField(TEXT("targetPin"), TargetPin);
			Connections.Add(MakeShared<FJsonValueObject>(Connection));
		};
		const TCHAR* MathLibrary = TEXT("/Script/Engine.KismetMathLibrary");
		const TCHAR* SystemLibrary = TEXT("/Script/Engine.KismetSystemLibrary");
		if (Template.Name == TEXT("health_system"))
		{
			const FString Health = FString::SanitizeFloat(Parameters->GetNumberField(TEXT("maxHealth")));
			AddVariable(TEXT("MaxHealth"), TEXT("float"), Health, TEXT("Health"));
			AddVariable(TEXT("Health"), TEXT("float"), Health, TEXT("Health"));
			AddVariable(TEXT("DamageAmount"), TEXT("float"), FString::SanitizeFloat(Parameters->GetNumberField(TEXT("damageAmount"))), TEXT("Health"));
			AddVariable(TEXT("HealAmount"), TEXT("float"), FString::SanitizeFloat(Parameters->GetNumberField(TEXT("healAmount"))), TEXT("Health"));
			for (int32 Operation = 0; Operation < 2; ++Operation)
			{
				const FString Prefix = Operation == 0 ? TEXT("damage") : TEXT("heal");
				const TCHAR* Event = Operation == 0 ? TEXT("TakeDamage") : TEXT("Heal");
				const TCHAR* Amount = Operation == 0 ? TEXT("DamageAmount") : TEXT("HealAmount");
				const int32 Y = Operation * 500;
				AddEvent(Event, Event, Y);
				AddVariableNode(*(Prefix + TEXT("-health")), TEXT("VariableGet"), TEXT("Health"), 200, Y + 100);
				AddVariableNode(*(Prefix + TEXT("-amount")), TEXT("VariableGet"), Amount, 0, Y + 200);
				AddVariableNode(*(Prefix + TEXT("-maximum")), TEXT("VariableGet"), TEXT("MaxHealth"), 400, Y + 300);
				auto ZeroB = MakeShared<FJsonObject>(); ZeroB->SetStringField(TEXT("B"), TEXT("0"));
				AddCall(*(Prefix + TEXT("-positive-amount")), TEXT("FMax"), MathLibrary, 200, Y + 200, ZeroB);
				AddCall(*(Prefix + TEXT("-positive-maximum")), TEXT("FMax"), MathLibrary, 600, Y + 300, ZeroB);
				AddCall(*(Prefix + TEXT("-arithmetic")), Operation == 0 ? TEXT("Subtract_DoubleDouble") : TEXT("Add_DoubleDouble"), MathLibrary, 400, Y + 100);
				auto Minimum = MakeShared<FJsonObject>(); Minimum->SetStringField(TEXT("Min"), TEXT("0"));
				AddCall(*(Prefix + TEXT("-clamp")), TEXT("FClamp"), MathLibrary, 800, Y + 100, Minimum);
				AddVariableNode(*(Prefix + TEXT("-set")), TEXT("VariableSet"), TEXT("Health"), 1100, Y);
				Link(Event, TEXT("then"), *(Prefix + TEXT("-set")), TEXT("execute"));
				Link(*(Prefix + TEXT("-amount")), Amount, *(Prefix + TEXT("-positive-amount")), TEXT("A"));
				Link(*(Prefix + TEXT("-health")), TEXT("Health"), *(Prefix + TEXT("-arithmetic")), TEXT("A"));
				Link(*(Prefix + TEXT("-positive-amount")), TEXT("ReturnValue"), *(Prefix + TEXT("-arithmetic")), TEXT("B"));
				Link(*(Prefix + TEXT("-maximum")), TEXT("MaxHealth"), *(Prefix + TEXT("-positive-maximum")), TEXT("A"));
				Link(*(Prefix + TEXT("-positive-maximum")), TEXT("ReturnValue"), *(Prefix + TEXT("-clamp")), TEXT("Max"));
				Link(*(Prefix + TEXT("-arithmetic")), TEXT("ReturnValue"), *(Prefix + TEXT("-clamp")), TEXT("Value"));
				Link(*(Prefix + TEXT("-clamp")), TEXT("ReturnValue"), *(Prefix + TEXT("-set")), TEXT("Health"));
			}
		}
		else if (Template.Name == TEXT("timer_loop"))
		{
			AddVariable(TEXT("LoopCount"), TEXT("int"), TEXT("0"), TEXT("Timer"));
			AddVariable(TEXT("bTimerRunning"), TEXT("bool"), TEXT("false"), TEXT("Timer"));
			AddVariable(TEXT("bTimerEnded"), TEXT("bool"), TEXT("false"), TEXT("Timer"));
			AddEvent(TEXT("start"), TEXT("StartTimer"), 0);
			AddEvent(TEXT("stop"), TEXT("StopTimer"), 400);
			AddEvent(TEXT("callback"), Parameters->GetStringField(TEXT("eventName")), 800);
			auto StartDefaults = MakeShared<FJsonObject>();
			StartDefaults->SetStringField(TEXT("FunctionName"), Parameters->GetStringField(TEXT("eventName")));
			StartDefaults->SetStringField(TEXT("Time"), FString::SanitizeFloat(Parameters->GetNumberField(TEXT("delay"))));
			StartDefaults->SetStringField(TEXT("bLooping"), TEXT("true"));
			StartDefaults->SetStringField(TEXT("bMaxOncePerFrame"), TEXT("true"));
			AddVariableNode(TEXT("ended-get"), TEXT("VariableGet"), TEXT("bTimerEnded"), 0, 200);
			AddCall(TEXT("not-ended"), TEXT("Not_PreBool"), MathLibrary, 200, 200);
			Nodes.Add(MakeShared<FJsonValueObject>(MakeBlueprintTemplateNode(TEXT("start-guard"), TEXT("Branch"), 300, 0)));
			AddCall(TEXT("set-timer"), TEXT("K2_SetTimer"), SystemLibrary, 550, 0, StartDefaults);
			AddVariableNode(TEXT("running-start"), TEXT("VariableSet"), TEXT("bTimerRunning"), 900, 0);
			auto Started = MakeShared<FJsonObject>();
			Started->SetStringField(TEXT("bTimerRunning"), TEXT("true"));
			Nodes.Last()->AsObject()->SetObjectField(TEXT("pinDefaults"), Started);
			Link(TEXT("start"), TEXT("then"), TEXT("start-guard"), TEXT("execute"));
			Link(TEXT("ended-get"), TEXT("bTimerEnded"), TEXT("not-ended"), TEXT("A"));
			Link(TEXT("not-ended"), TEXT("ReturnValue"), TEXT("start-guard"), TEXT("Condition"));
			Link(TEXT("start-guard"), TEXT("then"), TEXT("set-timer"), TEXT("execute"));
			Link(TEXT("set-timer"), TEXT("then"), TEXT("running-start"), TEXT("execute"));
			for (int32 Lifecycle = 0; Lifecycle < 2; ++Lifecycle)
			{
				const TCHAR* Entry = Lifecycle == 0 ? TEXT("stop") : TEXT("end-play");
				const TCHAR* Clear = Lifecycle == 0 ? TEXT("clear-stop") : TEXT("clear-end");
				const TCHAR* Running = Lifecycle == 0 ? TEXT("running-stop") : TEXT("running-end");
				const int32 Y = Lifecycle == 0 ? 400 : 1200;
				if (Lifecycle == 1)
				{
					auto EndPlay = MakeBlueprintTemplateNode(Entry, TEXT("OverrideEvent"), 0, Y);
					EndPlay->SetStringField(TEXT("functionName"), TEXT("ReceiveEndPlay"));
					EndPlay->SetStringField(TEXT("className"), TEXT("/Script/Engine.Actor"));
					Nodes.Add(MakeShared<FJsonValueObject>(EndPlay));
				}
				auto ClearDefaults = MakeShared<FJsonObject>();
				ClearDefaults->SetStringField(TEXT("FunctionName"), Parameters->GetStringField(TEXT("eventName")));
				AddCall(Clear, TEXT("K2_ClearTimer"), SystemLibrary, 300, Y, ClearDefaults);
				AddVariableNode(Running, TEXT("VariableSet"), TEXT("bTimerRunning"), 650, Y);
				auto Stopped = MakeShared<FJsonObject>(); Stopped->SetStringField(TEXT("bTimerRunning"), TEXT("false"));
				Nodes.Last()->AsObject()->SetObjectField(TEXT("pinDefaults"), Stopped);
				Link(Entry, TEXT("then"), Clear, TEXT("execute"));
				Link(Clear, TEXT("then"), Running, TEXT("execute"));
				if (Lifecycle == 1)
				{
					AddVariableNode(TEXT("ended-set"), TEXT("VariableSet"), TEXT("bTimerEnded"), 900, Y);
					auto Ended = MakeShared<FJsonObject>();
					Ended->SetStringField(TEXT("bTimerEnded"), TEXT("true"));
					Nodes.Last()->AsObject()->SetObjectField(TEXT("pinDefaults"), Ended);
					Link(Running, TEXT("then"), TEXT("ended-set"), TEXT("execute"));
				}
			}
			AddVariableNode(TEXT("running-get"), TEXT("VariableGet"), TEXT("bTimerRunning"), 0, 1000);
			Nodes.Add(MakeShared<FJsonValueObject>(MakeBlueprintTemplateNode(TEXT("running-guard"), TEXT("Branch"), 300, 800)));
			AddVariableNode(TEXT("count-get"), TEXT("VariableGet"), TEXT("LoopCount"), 300, 1000);
			AddCall(TEXT("increment"), TEXT("Add_IntInt"), MathLibrary, 550, 1000);
			AddVariableNode(TEXT("count-set"), TEXT("VariableSet"), TEXT("LoopCount"), 800, 800);
			Link(TEXT("callback"), TEXT("then"), TEXT("running-guard"), TEXT("execute"));
			Link(TEXT("running-get"), TEXT("bTimerRunning"), TEXT("running-guard"), TEXT("Condition"));
			Link(TEXT("running-guard"), TEXT("then"), TEXT("count-set"), TEXT("execute"));
			Link(TEXT("count-get"), TEXT("LoopCount"), TEXT("increment"), TEXT("A"));
			Link(TEXT("increment"), TEXT("ReturnValue"), TEXT("count-set"), TEXT("LoopCount"));
		}
		else
		{
			AddVariable(TEXT("bIsInteractable"), TEXT("bool"), Parameters->GetBoolField(TEXT("initiallyInteractable")) ? TEXT("true") : TEXT("false"), TEXT("Interaction"));
			AddVariable(TEXT("InteractionCount"), TEXT("int"), TEXT("0"), TEXT("Interaction"));
			TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
			Component->SetStringField(TEXT("name"), TEXT("InteractionSphere"));
			Component->SetStringField(TEXT("componentClass"), TEXT("/Script/Engine.SphereComponent"));
			Spec->SetArrayField(TEXT("components"), {MakeShared<FJsonValueObject>(Component)});
			AddEvent(TEXT("interact"), TEXT("Interact"), 0);
			AddVariableNode(TEXT("interactable-get"), TEXT("VariableGet"), TEXT("bIsInteractable"), 200, 150);
			Nodes.Add(MakeShared<FJsonValueObject>(
				MakeBlueprintTemplateNode(TEXT("branch"), TEXT("Branch"), 400, 0)));
			AddVariableNode(TEXT("interaction-count-get"), TEXT("VariableGet"), TEXT("InteractionCount"), 400, 250);
			AddCall(TEXT("interaction-increment"), TEXT("Add_IntInt"), MathLibrary, 600, 250);
			AddVariableNode(TEXT("interaction-count-set"), TEXT("VariableSet"), TEXT("InteractionCount"), 850, 0);
			Link(TEXT("interact"), TEXT("then"), TEXT("branch"), TEXT("execute"));
			Link(TEXT("interactable-get"), TEXT("bIsInteractable"), TEXT("branch"), TEXT("Condition"));
			Link(TEXT("branch"), TEXT("then"), TEXT("interaction-count-set"), TEXT("execute"));
			Link(TEXT("interaction-count-get"), TEXT("InteractionCount"), TEXT("interaction-increment"), TEXT("A"));
			Link(TEXT("interaction-increment"), TEXT("ReturnValue"), TEXT("interaction-count-set"), TEXT("InteractionCount"));
		}
		Spec->SetArrayField(TEXT("variables"), Variables);
		Spec->SetArrayField(TEXT("nodes"), Nodes);
		Spec->SetArrayField(TEXT("connections"), Connections);
		return Spec;
	}

	class FBlueprintTemplateListTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.template.list"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid() || !Params->Values.IsEmpty())
			{
				return FMCPToolResult::Error(TEXT("blueprint.template.list requires an empty parameter object."), TEXT("invalid_params"), 422);
			}
			TArray<TSharedPtr<FJsonValue>> Templates;
			for (const FBlueprintBehaviorTemplate& Template : BlueprintBehaviorTemplates())
			{
				Templates.Add(MakeShared<FJsonValueObject>(BlueprintTemplateDescriptor(Template)));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint-template-catalog.v1"));
			Result->SetArrayField(TEXT("templates"), Templates);
			Result->SetNumberField(TEXT("count"), Templates.Num());
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FBlueprintTemplateApplyTool final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.template.apply"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid())
			{
				return FMCPToolResult::Error(TEXT("parameters are required."), TEXT("invalid_params"), 422);
			}
			const TSet<FString> Allowed = {TEXT("templateName"), TEXT("blueprint"), TEXT("graph"), TEXT("buildId"), TEXT("parameters")};
			for (const auto& Pair : Params->Values)
			{
				if (!Allowed.Contains(Pair.Key))
				{
					return FMCPToolResult::Error(TEXT("Unknown template application field: ") + Pair.Key, TEXT("invalid_params"), 422);
				}
			}
			const FString TemplateName = BuildString(Params, TEXT("templateName"));
			if (TemplateName.IsEmpty())
			{
				return FMCPToolResult::Error(TEXT("templateName must be a non-empty string."), TEXT("invalid_params"), 422);
			}
			const FBlueprintBehaviorTemplate* Template = FindBlueprintBehaviorTemplate(TemplateName);
			if (!Template)
			{
				return FMCPToolResult::Error(TEXT("Unknown template. Discover names through blueprint.template.list."), TEXT("template_not_found"), 404);
			}
			const FString Blueprint = BuildString(Params, TEXT("blueprint"));
			if (!Blueprint.StartsWith(TEXT("/")) || Blueprint.Len() < 2 || Blueprint.Len() > 1024)
			{
				return FMCPToolResult::Error(TEXT("blueprint must be an explicit asset path of at most 1024 characters."), TEXT("invalid_params"), 422);
			}
			if (Params->HasField(TEXT("graph"))
				&& (BuildString(Params, TEXT("graph")).IsEmpty() || BuildString(Params, TEXT("graph")).Len() > 128))
			{
				return FMCPToolResult::Error(TEXT("graph must be a non-empty string of at most 128 characters."), TEXT("invalid_params"), 422);
			}
			if (Params->HasField(TEXT("buildId")) && !IsSafeBuildToken(BuildString(Params, TEXT("buildId"))))
			{
				return FMCPToolResult::Error(TEXT("buildId must be a safe BuildGraph token of 1 to 128 characters."), TEXT("invalid_params"), 422);
			}
			const TSharedPtr<FJsonObject>* InputParameters = nullptr;
			if (Params->HasField(TEXT("parameters"))
				&& (!Params->TryGetObjectField(TEXT("parameters"), InputParameters) || !InputParameters || !InputParameters->IsValid()))
			{
				return FMCPToolResult::Error(TEXT("parameters must be an object."), TEXT("invalid_params"), 422);
			}
			TSharedRef<FJsonObject> Resolved = MakeShared<FJsonObject>();
			FString Error;
			if (!ResolveBlueprintTemplateParameters(*Template, InputParameters ? *InputParameters : nullptr, Resolved, Error))
			{
				return FMCPToolResult::Error(Error, TEXT("invalid_params"), 422);
			}
			UBlueprint* Target = LoadBuildBlueprint(Blueprint, Error);
			if (!Target) return FMCPToolResult::Error(Error, TEXT("asset_not_found"), 404);
			const FString GraphName = Params->HasField(TEXT("graph")) ? BuildString(Params, TEXT("graph")) : TEXT("EventGraph");
			if (!Target->ParentClass || !Target->ParentClass->IsChildOf(AActor::StaticClass())
				|| !Target->UbergraphPages.ContainsByPredicate([&GraphName](const UEdGraph* Graph)
					{ return Graph && Graph->GetName() == GraphName; }))
			{
				return FMCPToolResult::Error(TEXT("Behavior templates require an Actor Blueprint and an existing event graph."), TEXT("invalid_params"), 422);
			}
			if (TemplateName == TEXT("timer_loop")
				&& Target->ParentClass->FindFunctionByName(FName(*Resolved->GetStringField(TEXT("eventName")))))
			{
				return FMCPToolResult::Error(TEXT("The timer callback name conflicts with an inherited function."), TEXT("invalid_params"), 422);
			}
			const TSharedRef<FJsonObject> Spec = MakeBlueprintTemplateSpec(*Template, Params, Resolved);
			FBuildBlueprintFromSpecTool SpecBuilder;
			FMCPToolResult Result = SpecBuilder.Plan(Spec);
			if (Result.bSuccess && Result.Data.IsValid())
			{
				Result.Data->SetStringField(TEXT("schema"), TEXT("ue.blueprint-template-application-plan.v1"));
				Result.Data->SetObjectField(TEXT("template"), BlueprintTemplateDescriptor(*Template));
				Result.Data->SetObjectField(TEXT("parameters"), Resolved);
				Result.Data->SetObjectField(TEXT("spec"), Spec);
				Result.Data->SetBoolField(TEXT("applied"), false);
				Result.Data->SetBoolField(TEXT("executionRequired"), true);
				Result.Data->SetBoolField(TEXT("runtimeVerified"), false);
				Result.Data->SetStringField(TEXT("nextAction"), TEXT("Review the returned Workflow, then execute it through ue-workflow-cli with approvePlanDigest and confirmWrite. Retain runId for read-back or rollback."));
			}
			return Result;
		}
	};
}

namespace UEAIIntegrationTools
{
	void RegisterBlueprintBuildGraphTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FBuildGraphValidateTool>());
		Registry.Register(MakeShared<FBuildGraphPlanTool>());
		Registry.Register(MakeShared<FBuildGraphExecuteTool>());
		Registry.Register(MakeShared<FBuildBlueprintFromSpecTool>());
		Registry.Register(MakeShared<FBlueprintTemplateListTool>());
		Registry.Register(MakeShared<FBlueprintTemplateApplyTool>());
		Registry.Register(MakeShared<FBuildGraphDefinitionGetTool>());
		Registry.Register(MakeShared<FBuildGraphMetadataSetTool>());
	}
}
