#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Editor.h"
#include "Engine/Blueprint.h"
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
		TEXT("Reroute")};
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
	if (Nodes.IsEmpty() || Nodes.Num() > MaxBuildNodes)
	{
		OutErrors.Add(TEXT("nodes and comments must contain 1..128 entries in total."));
		return false;
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
			OutErrors.Add(FString::Printf(TEXT("nodes/comments[%d] has an invalid, duplicate, or unsupported ref/nodeType."), Index));
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
				OutErrors.Add(FString::Printf(TEXT("comments[%d] requires text and positive width/height."), Index));
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
					OutErrors.Add(FString::Printf(TEXT("groups[%d] contains an unknown or multiply grouped ref '%s'."), Index, *Ref));
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
			return FMCPToolResult::Error(TEXT("Managed BuildGraph definition was not found."), TEXT("job_not_found"), 404);
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
							TEXT("Managed ref '%s' changed nodeType; replaceManaged never overwrites a managed node in place."),
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
					{MakeShared<FJsonValueString>(FString::Printf(
						TEXT("buildgraph-metadata-%d"),
						NodeIndex - 1))});
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
			: FMCPResult::Fail(TEXT("workflow_runtime_unavailable"), TEXT("Workflow runtime is unavailable."), 503);
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
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintBuildGraphTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FBuildGraphValidateTool>());
	Registry.Register(MakeShared<FBuildGraphPlanTool>());
	Registry.Register(MakeShared<FBuildGraphDefinitionGetTool>());
	Registry.Register(MakeShared<FBuildGraphMetadataSetTool>());
}
}
