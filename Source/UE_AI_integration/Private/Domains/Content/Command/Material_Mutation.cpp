#include "Infrastructure/MaterialEditingTarget.h"
// Material Mutation Tools — create, modify, connect, snapshot/diff/restore materials
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialFunctionMutation.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/MaterialFunctionDependencies.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "MaterialEditingLibrary.h"
#include "Infrastructure/DeferredGraphMutation.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/Sha256.h"
#include "Workflow/UEWorkflowExecutionContext.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionComposite.h"
#include "Materials/MaterialExpressionPinBase.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "MaterialEditorUtilities.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "RHIFeatureLevel.h"
#include "ScopedTransaction.h"

// SEH wrapper for material expression creation
#if PLATFORM_WINDOWS
extern int32 TryAddMaterialExpressionSEH(
	UObject* Owner, UClass* ExprClass, UMaterial* Material, UMaterialFunction* MatFunc,
	int32 PosX, int32 PosY, UMaterialExpression** OutExpr);
#endif

using namespace MCPMaterialInfrastructure;

static void BeginMaterialMutation(
	UObject* Asset,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Asset)
	{
		return;
	}
	Asset->Modify();
	if (!UEAIIntegration::Workflow::ShouldDeferCompile(Params))
	{
		Asset->PreEditChange(nullptr);
	}
}

static void FinalizeMaterialMutation(
	UObject* Asset,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Asset)
	{
		return;
	}
	UEAIIntegration::MaterialEditing::NotifyMaterialSourceEdited(Asset);
	if (UEAIIntegration::Workflow::ShouldDeferCompile(Params))
	{
		Asset->MarkPackageDirty();
	}
	else
	{
		Asset->PostEditChange();
		if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
			UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
	}
}

static bool SaveMaterialForExecution(
	UObject* Asset,
	const TSharedPtr<FJsonObject>& Params)
{
	return UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
		&& SaveMaterialPackage(Asset);
}

static void CancelMaterialMutation(
	UObject* Asset,
	const TSharedPtr<FJsonObject>& Params)
{
	if (Asset
		&& !UEAIIntegration::Workflow::ShouldDeferCompile(Params))
	{
		Asset->PostEditChange();
	}
}

// Authored material graph writes must be fenced by an immutable graph boundary.
// Preview writes are routed before reaching these helpers and retain their
// editor-context contract.  Keeping this check in the mutation adapter closes
// the older direct entry points that otherwise bypass graph.execute_plan.
static FMCPToolResult RequireMaterialMutationBoundary(
	UObject* Asset,
	const TSharedPtr<FJsonObject>& Params,
	UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& OutValidation)
{
	const TSharedPtr<FJsonObject>* WorkflowContext = nullptr;
	bool bApprovedPlan = false;
	if (Params->TryGetObjectField(TEXT("__ueWorkflow"), WorkflowContext)
		&& WorkflowContext && (*WorkflowContext).IsValid())
	{
		(*WorkflowContext)->TryGetBoolField(TEXT("approvedPlan"), bApprovedPlan);
	}
	if (!Params->HasField(TEXT("boundaryId")))
	{
		if (bApprovedPlan)
		{
			return FMCPToolResult::Ok(MakeShared<FJsonObject>());
		}
		return FMCPToolResult::Error(
			TEXT("Authored material graph writes require boundaryId, snapshotId and expectedProjectionHash."),
			TEXT("material_boundary_required_for_mutation"),
			409);
	}
	return UEAIIntegration::MaterialQuery::ValidateBoundaryWrite(Asset, Params, OutValidation);
}

static FString MaterialMutationNodeId(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return FString();
	}
	if (Node->IsA<UMaterialGraphNode_Root>())
	{
		return TEXT("root");
	}
	if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
	{
		if (MaterialNode->MaterialExpression)
		{
			return MCPMaterialInfrastructure::ExpressionNodeId(MaterialNode->MaterialExpression);
		}
	}
	return Node->NodeGuid.ToString();
}

// A material expression can be consumed by more than one authored input.  A
// value edit on such a node has fan-out semantics even though it changes only
// one UObject.  Keep this check independent of the editor graph projection so
// headless MaterialFunctions receive the same shared-node protection.
static void CollectMaterialExpressionConsumers(
	UMaterial* Material,
	UMaterialFunction* Function,
	UMaterialExpression* Expression,
	TArray<FString>& OutConsumerIds)
{
	OutConsumerIds.Reset();
	if (!Expression)
	{
		return;
	}
	TSet<FString> UniqueConsumers;
	auto AddConsumer = [&UniqueConsumers](const FString& Id)
	{
		if (!Id.IsEmpty())
		{
			UniqueConsumers.Add(Id);
		}
	};

	// GetExpressions() is a raw-pointer array on some UE versions and a
	// TArrayView<const TObjectPtr<...>> on others.  Keep this helper generic so
	// the protection contract follows the engine's container without coupling
	// the authoring path to one representation.
	auto InspectConsumers = [&Expression, &AddConsumer](const auto& Expressions)
	{
		for (const auto& ConsumerValue : Expressions)
		{
			UMaterialExpression* Consumer = ConsumerValue;
			if (!Consumer || Consumer == Expression)
			{
				continue;
			}
			for (const FExpressionInput* Input : Consumer->GetInputsView())
			{
				if (Input && Input->Expression == Expression)
				{
					AddConsumer(MCPMaterialInfrastructure::ExpressionNodeId(Consumer));
					break;
				}
			}
		}
	};
	if (Material)
	{
		InspectConsumers(Material->GetExpressions());
	}
	else if (Function)
	{
		InspectConsumers(Function->GetExpressions());
	}
	if (Material)
	{
		for (int32 InputIndex = 0; InputIndex < MP_MAX; ++InputIndex)
		{
			const FExpressionInput* Input = Material->GetExpressionInputForProperty(
				static_cast<EMaterialProperty>(InputIndex));
			if (Input && Input->Expression == Expression)
			{
				AddConsumer(TEXT("root"));
			}
		}
	}
	OutConsumerIds = UniqueConsumers.Array();
	OutConsumerIds.Sort();
}

static void AddMaterialBoundaryResultFields(
	const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
	const TArray<FString>& ExternalConsumerIds,
	TSharedRef<FJsonObject>& Result,
	const bool bImplicitWorkflowConfirmation = false)
{
	const bool bHasBoundary = Validation.Boundary.IsValid();
	const bool bShared = bHasBoundary
		? Validation.Boundary->bRequiresSharedNodeConfirmation
		: ExternalConsumerIds.Num() > 1;
	Result->SetBoolField(TEXT("writeBoundaryVerified"), bHasBoundary);
	Result->SetBoolField(TEXT("sharedNodeImpactDetected"), bShared);
	Result->SetBoolField(
		TEXT("sharedNodeImpactConfirmed"),
		(bHasBoundary && Validation.bSharedNodeImpactConfirmed)
			|| (bShared && bImplicitWorkflowConfirmation));
	TArray<TSharedPtr<FJsonValue>> Consumers;
	const TArray<FString>* Source = &ExternalConsumerIds;
	if (bHasBoundary)
	{
		Source = &Validation.Boundary->ExternallyConsumedNodeIds;
	}
	for (const FString& Id : *Source)
	{
		Consumers.Add(MakeShared<FJsonValueString>(Id));
	}
	Result->SetArrayField(TEXT("externallyConsumedNodeIds"), Consumers);
	if (bHasBoundary)
	{
		Result->SetStringField(TEXT("boundaryId"), Validation.Boundary->BoundaryId);
		Result->SetStringField(TEXT("snapshotId"), Validation.SourceSnapshot->Id);
		Result->SetStringField(TEXT("sourceProjectionHash"), Validation.SourceSnapshot->ProjectionHash);
		Result->SetStringField(TEXT("freshProjectionHash"), Validation.FreshSnapshot->ProjectionHash);
	}
}

static FMCPToolResult RequireMaterialBoundaryNode(
	const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
	const UEdGraphNode* Node,
	const TCHAR* Role,
	bool bRequireWritable)
{
	if (!Validation.Boundary.IsValid())
	{
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}
	const FString NodeId = MaterialMutationNodeId(Node);
	if (NodeId.IsEmpty())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("The %s graph node has no stable material identity."), Role),
			TEXT("material_boundary_node_identity_missing"),
			409);
	}
	if (!Validation.Boundary->SelectedNodeIds.Contains(NodeId))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("The %s node '%s' is outside the selected material boundary."), Role, *NodeId),
			TEXT("material_boundary_node_outside_selection"),
			409);
	}
	if (bRequireWritable && !Validation.Boundary->WritableNodeIds.Contains(NodeId))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("The %s node '%s' is not writable in the selected material boundary."), Role, *NodeId),
			TEXT("material_boundary_node_not_writable"),
			409);
	}
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

struct FMaterialGraphLinkSnapshot
{
	UEdGraphPin* A = nullptr;
	UEdGraphPin* B = nullptr;
};

static void CaptureMaterialGraphLinks(
	const UEdGraph* Graph,
	TArray<FMaterialGraphLinkSnapshot>& OutLinks)
{
	OutLinks.Reset();
	if (!Graph)
	{
		return;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (!Linked || reinterpret_cast<UPTRINT>(Pin) >= reinterpret_cast<UPTRINT>(Linked))
				{
					continue;
				}
				FMaterialGraphLinkSnapshot& Link = OutLinks.AddDefaulted_GetRef();
				Link.A = Pin;
				Link.B = Linked;
			}
		}
	}
}

static bool RestoreMaterialGraphLinks(
	UEdGraph* Graph,
	const TArray<FMaterialGraphLinkSnapshot>& Links)
{
	if (!Graph)
	{
		return false;
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				Pin->BreakAllPinLinks();
			}
		}
	}
	for (const FMaterialGraphLinkSnapshot& Link : Links)
	{
		if (!Link.A || !Link.B || Link.A->LinkedTo.Contains(Link.B))
		{
			continue;
		}
		Link.A->MakeLinkTo(Link.B);
	}
	return true;
}

struct FMaterialExpressionDeleteSnapshot
{
	UMaterialExpression* Expression = nullptr;
	UMaterialGraphNode* GraphNode = nullptr;
	TArray<UMaterialExpression*> ExpressionOrder;
	TMap<UMaterialExpression*, TArray<FExpressionInput>> Inputs;
	TArray<FExpressionInput> MaterialInputs;
	TArray<FMaterialGraphLinkSnapshot> GraphLinks;
};

static FMaterialExpressionDeleteSnapshot CaptureMaterialExpressionDeleteSnapshot(
	UMaterial* Material,
	UMaterialFunction* Function,
	UMaterialExpression* Expression,
	UMaterialGraphNode* GraphNode,
	UEdGraph* Graph)
{
	FMaterialExpressionDeleteSnapshot Snapshot;
	Snapshot.Expression = Expression;
	Snapshot.GraphNode = GraphNode;
	const auto Expressions = Material ? Material->GetExpressions() : Function->GetExpressions();
	Snapshot.ExpressionOrder.Reserve(Expressions.Num());
	for (UMaterialExpression* Current : Expressions)
	{
		if (!Current)
		{
			continue;
		}
		Snapshot.ExpressionOrder.Add(Current);
		TArray<FExpressionInput>& SavedInputs = Snapshot.Inputs.Add(Current);
		for (FExpressionInput* Input : Current->GetInputsView())
		{
			SavedInputs.Add(Input ? *Input : FExpressionInput());
		}
	}
	if (Material)
	{
		Snapshot.MaterialInputs.Reserve(MP_MAX);
		for (int32 InputIndex = 0; InputIndex < MP_MAX; ++InputIndex)
		{
			const FExpressionInput* Input = Material->GetExpressionInputForProperty(
				static_cast<EMaterialProperty>(InputIndex));
			Snapshot.MaterialInputs.Add(Input ? *Input : FExpressionInput());
		}
	}
	CaptureMaterialGraphLinks(Graph, Snapshot.GraphLinks);
	return Snapshot;
}

static bool RestoreMaterialExpressionDeleteSnapshot(
	UMaterial* Material,
	UMaterialFunction* Function,
	UEdGraph* Graph,
	const FMaterialExpressionDeleteSnapshot& Snapshot,
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Asset = Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function);
	if (!Asset || !Snapshot.Expression)
	{
		return false;
	}
	Asset->Modify();
	if (!Snapshot.ExpressionOrder.Contains(Snapshot.Expression))
	{
		return false;
	}
	if (!Snapshot.ExpressionOrder.Contains(Snapshot.Expression)
		|| (Material ? !Material->GetExpressions().Contains(Snapshot.Expression)
		             : !Function->GetExpressions().Contains(Snapshot.Expression)))
	{
		if (Material)
		{
			Material->GetExpressionCollection().AddExpression(Snapshot.Expression);
		}
		else
		{
			Function->GetExpressionCollection().AddExpression(Snapshot.Expression);
		}
	}
	if (Graph && Snapshot.GraphNode && !Graph->Nodes.Contains(Snapshot.GraphNode))
	{
		Graph->AddNode(Snapshot.GraphNode, false);
	}
	for (const TPair<UMaterialExpression*, TArray<FExpressionInput>>& Pair : Snapshot.Inputs)
	{
		if (!Pair.Key)
		{
			continue;
		}
		TArrayView<FExpressionInput*> Inputs = Pair.Key->GetInputsView();
		for (int32 Index = 0; Index < Inputs.Num(); ++Index)
		{
			if (Inputs[Index] && Pair.Value.IsValidIndex(Index))
			{
				*Inputs[Index] = Pair.Value[Index];
			}
		}
	}
	if (Material)
	{
		for (int32 InputIndex = 0; InputIndex < MP_MAX; ++InputIndex)
		{
			FExpressionInput* Input = Material->GetExpressionInputForProperty(
				static_cast<EMaterialProperty>(InputIndex));
			if (Input && Snapshot.MaterialInputs.IsValidIndex(InputIndex))
			{
				*Input = Snapshot.MaterialInputs[InputIndex];
			}
		}
	}
	if (Graph)
	{
		RestoreMaterialGraphLinks(Graph, Snapshot.GraphLinks);
		if (!UEAIIntegration::Workflow::ShouldDeferCompile(Params))
		{
			CastChecked<UMaterialGraph>(Graph)->LinkMaterialExpressionsFromGraph();
			Graph->NotifyGraphChanged();
		}
	}
	return true;
}

static FMCPToolResult RollbackMaterialExpressionDelete(
	UMaterial* Material,
	UMaterialFunction* Function,
	UEdGraph* Graph,
	const FMaterialExpressionDeleteSnapshot& Snapshot,
	const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
	const TSharedPtr<FJsonObject>& Params,
	FMCPToolResult Failure)
{
	const bool bRestored = RestoreMaterialExpressionDeleteSnapshot(
		Material,
		Function,
		Graph,
		Snapshot,
		Params);
	UObject* Asset = Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(Function);
	CancelMaterialMutation(Asset, Params);
	bool bVerified = bRestored;
	if (bVerified && Validation.SourceSnapshot.IsValid())
	{
		TSharedPtr<const UEAIIntegration::MaterialQuery::FSnapshot> Restored;
		const FMCPToolResult CaptureResult = UEAIIntegration::MaterialQuery::Capture(
			Asset,
			Asset->GetPathName(),
			FString(),
			nullptr,
			Validation.SourceSnapshot->bIncludeNamedReroutes,
			&Restored,
			false);
		bVerified = CaptureResult.bSuccess && Restored.IsValid()
			&& Restored->ProjectionHash == Validation.SourceSnapshot->ProjectionHash;
	}
	if (!Failure.Data.IsValid())
	{
		Failure.Data = MakeShared<FJsonObject>();
	}
	Failure.Data->SetStringField(TEXT("attempt_status"), TEXT("rolled_back"));
	Failure.Data->SetStringField(TEXT("restore_status"), bVerified ? TEXT("restored") : TEXT("restore_failed"));
	Failure.Data->SetBoolField(TEXT("rollbackVerified"), bVerified);
	return Failure;
}

static FMCPToolResult VerifyMaterialMutationPostcondition(
	UObject* Asset,
	const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
	const TSharedPtr<FJsonObject>& Params,
	FString& OutExpected,
	FString& OutActual)
{
	OutExpected.Reset();
	OutActual.Reset();
	if (!Params->TryGetStringField(TEXT("expectedAfterProjectionHash"), OutExpected)
		|| OutExpected.IsEmpty())
	{
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}

	TSharedPtr<const UEAIIntegration::MaterialQuery::FSnapshot> Fresh;
	const bool bIncludeNamedReroutes = Validation.SourceSnapshot.IsValid()
		? Validation.SourceSnapshot->bIncludeNamedReroutes
		: false;
	const FMCPToolResult CaptureResult = UEAIIntegration::MaterialQuery::Capture(
		Asset,
		Asset->GetPathName(),
		FString(),
		nullptr,
		bIncludeNamedReroutes,
		&Fresh,
		false);
	if (!CaptureResult.bSuccess || !Fresh)
	{
		return FMCPToolResult::Error(
			TEXT("The material graph changed, but the postcondition projection could not be captured."),
			TEXT("material_boundary_postcondition_capture_failed"),
			500);
	}
	OutActual = Fresh->ProjectionHash;
	if (OutActual != OutExpected)
	{
		return FMCPToolResult::Error(
			TEXT("The authored material mutation did not produce the expected projection."),
			TEXT("material_boundary_postcondition_failed"),
			409);
	}
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

static FMCPToolResult RollbackMaterialMutation(
	UObject* Asset,
	UEdGraph* Graph,
	const TArray<FMaterialGraphLinkSnapshot>& Links,
	const UEAIIntegration::MaterialQuery::FBoundaryWriteValidation& Validation,
	const TSharedPtr<FJsonObject>& Params,
	FMCPToolResult Failure)
{
	const bool bRestoredGraph = RestoreMaterialGraphLinks(Graph, Links);
	if (Graph && !UEAIIntegration::Workflow::ShouldDeferCompile(Params))
	{
		CastChecked<UMaterialGraph>(Graph)->LinkMaterialExpressionsFromGraph();
		Graph->NotifyGraphChanged();
	}
	CancelMaterialMutation(Asset, Params);

	bool bRestoredProjection = bRestoredGraph;
	if (bRestoredProjection && Validation.SourceSnapshot.IsValid())
	{
		TSharedPtr<const UEAIIntegration::MaterialQuery::FSnapshot> Restored;
		const FMCPToolResult RestoredCapture = UEAIIntegration::MaterialQuery::Capture(
			Asset,
			Asset->GetPathName(),
			FString(),
			nullptr,
			Validation.SourceSnapshot->bIncludeNamedReroutes,
			&Restored,
			false);
		bRestoredProjection = RestoredCapture.bSuccess && Restored.IsValid()
			&& Restored->ProjectionHash == Validation.SourceSnapshot->ProjectionHash;
	}
	if (!Failure.Data.IsValid())
	{
		Failure.Data = MakeShared<FJsonObject>();
	}
	Failure.Data->SetStringField(TEXT("attempt_status"), TEXT("rolled_back"));
	Failure.Data->SetStringField(TEXT("restore_status"), bRestoredProjection ? TEXT("restored") : TEXT("restore_failed"));
	Failure.Data->SetBoolField(TEXT("rollbackVerified"), bRestoredProjection);
	return Failure;
}

// ============================================================
// create_material
// ============================================================
class FTool_CreateMaterial : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		FString PackagePath = Params->GetStringField(TEXT("packagePath"));
		if (Name.IsEmpty() || PackagePath.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: name, packagePath"));
		if (!PackagePath.StartsWith(TEXT("/Game")))
			return FMCPToolResult::Error(TEXT("packagePath must start with '/Game'"));

		const FString FullObjectPath = PackagePath / Name;
		const FString FullPackageName = FPackageName::ObjectPathToPackageName(FullObjectPath);
		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(FullPackageName))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *FullObjectPath));
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		UObject* NewAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterial::StaticClass(), Factory);
		if (!NewAsset) return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create Material '%s'"), *Name));

		UMaterial* Material = Cast<UMaterial>(NewAsset);
		if (!Material) return FMCPToolResult::Error(TEXT("Created asset is not a UMaterial"));

		FString DomainStr, BlendModeStr;
		Params->TryGetStringField(TEXT("domain"), DomainStr);
		Params->TryGetStringField(TEXT("blendMode"), BlendModeStr);
		bool bTwoSided = false;
		bool bHasTwoSided = Params->TryGetBoolField(TEXT("twoSided"), bTwoSided);

		BeginMaterialMutation(Material, Params);

		if (!DomainStr.IsEmpty())
		{
			if (DomainStr == TEXT("Surface")) Material->MaterialDomain = MD_Surface;
			else if (DomainStr == TEXT("DeferredDecal")) Material->MaterialDomain = MD_DeferredDecal;
			else if (DomainStr == TEXT("LightFunction")) Material->MaterialDomain = MD_LightFunction;
			else if (DomainStr == TEXT("Volume")) Material->MaterialDomain = MD_Volume;
			else if (DomainStr == TEXT("PostProcess")) Material->MaterialDomain = MD_PostProcess;
			else if (DomainStr == TEXT("UI")) Material->MaterialDomain = MD_UI;
		}
		if (!BlendModeStr.IsEmpty())
		{
			if (BlendModeStr == TEXT("Opaque")) Material->BlendMode = BLEND_Opaque;
			else if (BlendModeStr == TEXT("Masked")) Material->BlendMode = BLEND_Masked;
			else if (BlendModeStr == TEXT("Translucent")) Material->BlendMode = BLEND_Translucent;
			else if (BlendModeStr == TEXT("Additive")) Material->BlendMode = BLEND_Additive;
			else if (BlendModeStr == TEXT("Modulate")) Material->BlendMode = BLEND_Modulate;
		}
		if (bHasTwoSided) Material->TwoSided = bTwoSided ? 1 : 0;

		FinalizeMaterialMutation(Material, Params);
		const bool bSaved = SaveMaterialForExecution(Material, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("path"), Material->GetPathName());
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_material_property
// ============================================================
class FTool_SetMaterialProperty : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.property.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString Property = Params->GetStringField(TEXT("property"));
		if (MaterialName.IsEmpty() || Property.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required fields: material, property"));
		if (!Params->HasField(TEXT("value")))
			return FMCPToolResult::Error(TEXT("Missing required field: value"));

		bool bDryRun = false;
		Params->TryGetBoolField(TEXT("dryRun"), bDryRun);

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
		if (!Material) return FMCPToolResult::Error(LoadError);

		FString OldValue, NewValue;

		if (Property == TEXT("domain"))
		{
			FString ValueStr = Params->GetStringField(TEXT("value"));
			OldValue = StaticEnum<EMaterialDomain>()->GetNameStringByValue((int64)Material->MaterialDomain);
			EMaterialDomain NewDomain = Material->MaterialDomain;
			if (ValueStr == TEXT("Surface")) NewDomain = MD_Surface;
			else if (ValueStr == TEXT("DeferredDecal")) NewDomain = MD_DeferredDecal;
			else if (ValueStr == TEXT("LightFunction")) NewDomain = MD_LightFunction;
			else if (ValueStr == TEXT("Volume")) NewDomain = MD_Volume;
			else if (ValueStr == TEXT("PostProcess")) NewDomain = MD_PostProcess;
			else if (ValueStr == TEXT("UI")) NewDomain = MD_UI;
			else return FMCPToolResult::Error(FString::Printf(TEXT("Invalid domain '%s'"), *ValueStr));
			NewValue = ValueStr;
			if (!bDryRun) { BeginMaterialMutation(Material, Params); Material->MaterialDomain = NewDomain; FinalizeMaterialMutation(Material, Params); }
		}
		else if (Property == TEXT("blendMode"))
		{
			FString ValueStr = Params->GetStringField(TEXT("value"));
			OldValue = StaticEnum<EBlendMode>()->GetNameStringByValue((int64)Material->BlendMode);
			EBlendMode NewBlend = Material->BlendMode;
			if (ValueStr == TEXT("Opaque")) NewBlend = BLEND_Opaque;
			else if (ValueStr == TEXT("Masked")) NewBlend = BLEND_Masked;
			else if (ValueStr == TEXT("Translucent")) NewBlend = BLEND_Translucent;
			else if (ValueStr == TEXT("Additive")) NewBlend = BLEND_Additive;
			else if (ValueStr == TEXT("Modulate")) NewBlend = BLEND_Modulate;
			else return FMCPToolResult::Error(FString::Printf(TEXT("Invalid blendMode '%s'"), *ValueStr));
			NewValue = ValueStr;
			if (!bDryRun) { BeginMaterialMutation(Material, Params); Material->BlendMode = NewBlend; FinalizeMaterialMutation(Material, Params); }
		}
		else if (Property == TEXT("twoSided"))
		{
			const FString ValueString = Params->GetStringField(TEXT("value"));
			bool bValue = false;
			if (ValueString.Equals(TEXT("true"), ESearchCase::IgnoreCase))
			{
				bValue = true;
			}
			else if (!ValueString.Equals(TEXT("false"), ESearchCase::IgnoreCase))
			{
				return FMCPToolResult::Error(
					TEXT("twoSided value must be 'true' or 'false'"));
			}
			OldValue = Material->TwoSided ? TEXT("true") : TEXT("false");
			NewValue = bValue ? TEXT("true") : TEXT("false");
			if (!bDryRun) { BeginMaterialMutation(Material, Params); Material->TwoSided = bValue ? 1 : 0; FinalizeMaterialMutation(Material, Params); }
		}
		else if (Property == TEXT("shadingModel"))
		{
			FString ValueStr = Params->GetStringField(TEXT("value"));
			EMaterialShadingModel NewModel = MSM_DefaultLit;
			if (ValueStr == TEXT("Unlit")) NewModel = MSM_Unlit;
			else if (ValueStr == TEXT("DefaultLit")) NewModel = MSM_DefaultLit;
			else if (ValueStr == TEXT("Subsurface")) NewModel = MSM_Subsurface;
			else if (ValueStr == TEXT("ClearCoat")) NewModel = MSM_ClearCoat;
			else if (ValueStr == TEXT("SubsurfaceProfile")) NewModel = MSM_SubsurfaceProfile;
			else if (ValueStr == TEXT("TwoSidedFoliage")) NewModel = MSM_TwoSidedFoliage;
			else if (ValueStr == TEXT("Hair")) NewModel = MSM_Hair;
			else if (ValueStr == TEXT("Cloth")) NewModel = MSM_Cloth;
			else if (ValueStr == TEXT("Eye")) NewModel = MSM_Eye;
			else return FMCPToolResult::Error(FString::Printf(TEXT("Invalid shadingModel '%s'"), *ValueStr));
			OldValue = TEXT("(current)");
			NewValue = ValueStr;
			if (!bDryRun) { BeginMaterialMutation(Material, Params); Material->SetShadingModel(NewModel); FinalizeMaterialMutation(Material, Params); }
		}
		else if (Property == TEXT("opacityMaskClipValue"))
		{
			double Val = 0.0;
			const FString ValueString = Params->GetStringField(TEXT("value"));
			if (!LexTryParseString(Val, *ValueString))
			{
				return FMCPToolResult::Error(
					TEXT("opacityMaskClipValue must be a numeric string"));
			}
			OldValue = FString::Printf(TEXT("%f"), Material->OpacityMaskClipValue);
			NewValue = FString::Printf(TEXT("%f"), Val);
			if (!bDryRun) { BeginMaterialMutation(Material, Params); Material->OpacityMaskClipValue = (float)Val; FinalizeMaterialMutation(Material, Params); }
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown property '%s'. Valid: domain, blendMode, twoSided, shadingModel, opacityMaskClipValue"), *Property));
		}

		bool bSaved = false;
		if (!bDryRun) bSaved = SaveMaterialForExecution(Material, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("property"), Property);
		Result->SetStringField(TEXT("oldValue"), OldValue);
		Result->SetStringField(TEXT("newValue"), NewValue);
		Result->SetBoolField(TEXT("dryRun"), bDryRun);
		if (!bDryRun) Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// add_material_expression
// ============================================================
class FTool_AddMaterialExpression : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.expression.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
        if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
            return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);

		if (Params->HasField(TEXT("materialFunction")))
			return MutateMaterialFunction(GetCapabilityId(), Params);
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString MaterialFunctionName;
		Params->TryGetStringField(TEXT("materialFunction"), MaterialFunctionName);
		FString ExpressionClassName = Params->GetStringField(TEXT("expressionClass"));

		if (MaterialName.IsEmpty() && MaterialFunctionName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: 'material' or 'materialFunction'"));
		if (ExpressionClassName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing required field: expressionClass"));

		int32 PosX = 0, PosY = 0;
		double D = 0;
		if (Params->TryGetNumberField(TEXT("posX"), D)) PosX = (int32)D;
		if (Params->TryGetNumberField(TEXT("posY"), D)) PosY = (int32)D;

		// Resolve expression class
		static TMap<FString, FString> Aliases = {{TEXT("Lerp"), TEXT("LinearInterpolate")}};
		FString LookupName = ExpressionClassName;
		if (const FString* Alias = Aliases.Find(ExpressionClassName)) LookupName = *Alias;

		UClass* ExprClass = nullptr;
		FString FullClassName = FString::Printf(TEXT("MaterialExpression%s"), *LookupName);
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->GetName() == FullClassName && It->IsChildOf(UMaterialExpression::StaticClass()))
			{
				ExprClass = *It;
				break;
			}
		}
		if (!ExprClass) return FMCPToolResult::Error(FString::Printf(TEXT("Unknown expression class '%s'"), *ExpressionClassName));
		if (ExprClass->HasAnyClassFlags(CLASS_Abstract))
			return FMCPToolResult::Error(FString::Printf(TEXT("Expression class '%s' is abstract"), *ExpressionClassName));

		UMaterial* Material = nullptr;
		UMaterialFunction* MatFunc = nullptr;
		UObject* Owner = nullptr;
		FString AssetDisplayName;

		if (!MaterialFunctionName.IsEmpty())
		{
			FString LoadError;
			MatFunc = LoadMaterialFunctionByName(MaterialFunctionName, LoadError);
			if (!MatFunc) return FMCPToolResult::Error(LoadError);
			Owner = MatFunc;
			AssetDisplayName = MatFunc->GetName();
		}
		else
		{
			FString LoadError;
			Material = LoadMaterialByName(MaterialName, LoadError);
			if (!Material) return FMCPToolResult::Error(LoadError);
			Owner = Material;
			AssetDisplayName = Material->GetName();
		}

		if (Material) EnsureMaterialGraph(Material);

		UMaterialExpression* NewExpr = nullptr;
		UMaterialGraphNode* NewGraphNode = nullptr;
		const bool bDeferredWorkflow =
			UEAIIntegration::Workflow::ShouldDeferCompile(Params);
#if PLATFORM_WINDOWS
		if (!bDeferredWorkflow)
		{
			const int32 CreateResult = TryAddMaterialExpressionSEH(
				Owner,
				ExprClass,
				Material,
				MatFunc,
				PosX,
				PosY,
				&NewExpr);
			if (CreateResult != 0 || !NewExpr)
			{
				return FMCPToolResult::Error(FString::Printf(
					TEXT("Expression class '%s' cannot be instantiated"),
					*ExpressionClassName));
			}
		}
		else
#endif
		{
			BeginMaterialMutation(Owner, Params);
			NewExpr = NewObject<UMaterialExpression>(
				Owner,
				ExprClass,
				NAME_None,
				RF_Transactional);
			if (!NewExpr)
			{
				return FMCPToolResult::Error(
					TEXT("Failed to create material expression"));
			}
			NewExpr->Modify();
			NewExpr->MaterialExpressionEditorX = PosX;
			NewExpr->MaterialExpressionEditorY = PosY;
			if (Material)
			{
				Material->GetExpressionCollection().AddExpression(NewExpr);
				if (Material->MaterialGraph)
				{
					Material->MaterialGraph->Modify();
					NewGraphNode =
						Material->MaterialGraph->AddExpression(NewExpr, false);
					if (NewGraphNode)
					{
						NewGraphNode->SetFlags(RF_Transactional);
						NewGraphNode->Modify();
					}
				}
				FinalizeMaterialMutation(Material, Params);
			}
			else if (MatFunc)
			{
				MatFunc->GetExpressionCollection().AddExpression(NewExpr);
				FinalizeMaterialMutation(MatFunc, Params);
			}
		}

		// Native expression creation without an open Material Editor may only
		// update the authored collection. Keep the existing graph usable for the
		// next tool call without rebuilding every node in the material.
		if (Material && Material->MaterialGraph && NewExpr && !NewExpr->GraphNode)
		{
			Material->MaterialGraph->Modify();
			NewGraphNode = Material->MaterialGraph->AddExpression(NewExpr, false);
			if (NewGraphNode) NewGraphNode->SetFlags(RF_Transactional);
		}
		const bool bSaved = SaveMaterialForExecution(
			Material ? static_cast<UObject*>(Material) : static_cast<UObject*>(MatFunc),
			Params);

		FString NodeGuid = MCPMaterialInfrastructure::ExpressionNodeId(NewExpr);
		if (NewGraphNode)
		{
			NodeGuid = NewGraphNode->NodeGuid.ToString();
		}
		else if (Material && Material->MaterialGraph)
		{
			for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
			{
				UMaterialGraphNode* MatNode = Cast<UMaterialGraphNode>(Node);
				if (MatNode && MatNode->MaterialExpression == NewExpr)
				{
					NodeGuid = Node->NodeGuid.ToString();
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), AssetDisplayName);
		Result->SetStringField(TEXT("expressionClass"), ExpressionClassName);
		Result->SetStringField(TEXT("nodeId"), NodeGuid);
		Result->SetNumberField(TEXT("posX"), PosX);
		Result->SetNumberField(TEXT("posY"), PosY);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// delete_material_expression
// ============================================================
class FTool_DeleteMaterialExpression : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.expression.delete");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
        if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
            return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);

		if (Params->HasField(TEXT("materialFunction")))
			return MutateMaterialFunction(GetCapabilityId(), Params);
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString MaterialFunctionName;
		Params->TryGetStringField(TEXT("materialFunction"), MaterialFunctionName);
		FString NodeId = Params->GetStringField(TEXT("nodeId"));

		if (MaterialName.IsEmpty() && MaterialFunctionName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: 'material' or 'materialFunction'"));
		if (NodeId.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing: nodeId"));

		UMaterial* Material = nullptr;
		UMaterialFunction* MatFunc = nullptr;
		FString AssetDisplayName;

		if (!MaterialFunctionName.IsEmpty())
		{
			FString E; MatFunc = LoadMaterialFunctionByName(MaterialFunctionName, E);
			if (!MatFunc) return FMCPToolResult::Error(E);
			AssetDisplayName = MatFunc->GetName();
		}
		else
		{
			FString E; Material = LoadMaterialByName(MaterialName, E);
			if (!Material) return FMCPToolResult::Error(E);
			AssetDisplayName = Material->GetName();
		}

		if (Material) EnsureMaterialGraph(Material);
		UEdGraph* Graph = Material ? (UEdGraph*)Material->MaterialGraph : (MatFunc ? MatFunc->MaterialGraph : nullptr);
		if (!Graph) return FMCPToolResult::Error(FString::Printf(TEXT("'%s' has no material graph"), *AssetDisplayName));
		UObject* Asset = Material ? (UObject*)Material : (UObject*)MatFunc;
		UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
		FMCPToolResult BoundaryResult = RequireMaterialMutationBoundary(Asset, Params, BoundaryValidation);
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}

		UMaterialGraphNode* TargetMatNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (MatchesMaterialNode(Node, NodeId))
			{
				TargetMatNode = Cast<UMaterialGraphNode>(Node);
				break;
			}
		}
		if (!TargetMatNode) return FMCPToolResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));
		if (!TargetMatNode->MaterialExpression) return FMCPToolResult::Error(TEXT("Node has no material expression"));
		if (!TargetMatNode->CanUserDeleteNode()
			|| TargetMatNode->MaterialExpression->IsA<UMaterialExpressionComposite>()
			|| TargetMatNode->MaterialExpression->IsA<UMaterialExpressionPinBase>())
		{
			return FMCPToolResult::Error(TEXT("This node requires material subgraph deletion and cannot be deleted as a single expression."));
		}
		BoundaryResult = RequireMaterialBoundaryNode(
			BoundaryValidation,
			TargetMatNode,
			TEXT("deleted"),
			true);
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}

		FString DeletedNodeTitle = TargetMatNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		FString DeletedExprClass = TargetMatNode->MaterialExpression->GetClass()->GetName();

		UMaterialExpression* ExprToRemove = TargetMatNode->MaterialExpression;
		TArray<FString> DeletedNodeConsumerIds;
		CollectMaterialExpressionConsumers(
			Material,
			MatFunc,
			ExprToRemove,
			DeletedNodeConsumerIds);
		const FMaterialExpressionDeleteSnapshot DeleteSnapshot = CaptureMaterialExpressionDeleteSnapshot(
			Material,
			MatFunc,
			ExprToRemove,
			TargetMatNode,
			Graph);
		BeginMaterialMutation(Asset, Params);
		Graph->Modify();
		TargetMatNode->Modify();
		ExprToRemove->Modify();
		TargetMatNode->BreakAllNodeLinks();

		// Clear durable references as well as graph pins. A checkpoint or save
		// must never retain a removed expression through another node/root input.
		const auto Expressions = Material ? Material->GetExpressions() : MatFunc->GetExpressions();
		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression || Expression == ExprToRemove) continue;
			for (FExpressionInput* Input : Expression->GetInputsView())
			{
				if (Input && Input->Expression == ExprToRemove)
				{
					Expression->Modify();
					Input->Expression = nullptr;
				}
			}
		}
		if (Material)
		{
			for (int32 InputIndex = 0; InputIndex < MP_MAX; ++InputIndex)
			{
				FExpressionInput* Input = Material->GetExpressionInputForProperty(
					static_cast<EMaterialProperty>(InputIndex));
				if (Input && Input->Expression == ExprToRemove) Input->Expression = nullptr;
			}
			Material->RemoveExpressionParameter(ExprToRemove);
			Material->GetExpressionCollection().RemoveExpression(ExprToRemove);
		}
		else
		{
			MatFunc->GetExpressionCollection().RemoveExpression(ExprToRemove);
		}
		Graph->RemoveNode(TargetMatNode, false);
		if (!UEAIIntegration::Workflow::ShouldDeferCompile(Params))
		{
			CastChecked<UMaterialGraph>(Graph)->LinkMaterialExpressionsFromGraph();
		}
		// Workflow snapshots keep removed objects valid for same-session undo
		// and memory restoration; normal GC can reclaim them after that boundary.
		FinalizeMaterialMutation(Asset, Params);
		FString ExpectedAfterProjectionHash;
		FString ActualProjectionHash;
		const FMCPToolResult Postcondition = VerifyMaterialMutationPostcondition(
			Asset,
			BoundaryValidation,
			Params,
			ExpectedAfterProjectionHash,
			ActualProjectionHash);
		if (!Postcondition.bSuccess)
		{
			return RollbackMaterialExpressionDelete(
				Material,
				MatFunc,
				Graph,
				DeleteSnapshot,
				BoundaryValidation,
				Params,
				Postcondition);
		}
		if (!UEAIIntegration::Workflow::ShouldDeferCompile(Params))
		{
			ExprToRemove->MarkAsGarbage();
		}

		const bool bSaved = SaveMaterialForExecution(Asset, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), AssetDisplayName);
		Result->SetStringField(TEXT("deletedNode"), NodeId);
		Result->SetStringField(TEXT("deletedNodeTitle"), DeletedNodeTitle);
		Result->SetStringField(TEXT("deletedExpressionClass"), DeletedExprClass);
		Result->SetBoolField(TEXT("saved"), bSaved);
		AddMaterialBoundaryResultFields(
			BoundaryValidation,
			DeletedNodeConsumerIds,
			Result);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// connect_material_pins
// ============================================================
class FTool_ConnectMaterialPins : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.pin.connect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
        if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
            return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);

		if (Params->HasField(TEXT("materialFunction")))
			return MutateMaterialFunction(GetCapabilityId(), Params);
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString MaterialFunctionName;
		Params->TryGetStringField(TEXT("materialFunction"), MaterialFunctionName);
		FString SourceNodeId = Params->GetStringField(TEXT("sourceNodeId"));
		FString SourcePinName = Params->GetStringField(TEXT("sourcePinName"));
		FString TargetNodeId = Params->GetStringField(TEXT("targetNodeId"));
		FString TargetPinName = Params->GetStringField(TEXT("targetPinName"));

		if (MaterialName.IsEmpty() && MaterialFunctionName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: 'material' or 'materialFunction'"));
		if (SourceNodeId.IsEmpty() || SourcePinName.IsEmpty() || TargetNodeId.IsEmpty() || TargetPinName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: sourceNodeId, sourcePinName, targetNodeId, targetPinName"));

		UMaterial* Material = nullptr;
		UMaterialFunction* MatFunc = nullptr;
		FString AssetDisplayName;

		if (!MaterialFunctionName.IsEmpty())
		{
			FString E; MatFunc = LoadMaterialFunctionByName(MaterialFunctionName, E);
			if (!MatFunc) return FMCPToolResult::Error(E);
			AssetDisplayName = MatFunc->GetName();
		}
		else
		{
			FString E; Material = LoadMaterialByName(MaterialName, E);
			if (!Material) return FMCPToolResult::Error(E);
			AssetDisplayName = Material->GetName();
		}

		if (Material) EnsureMaterialGraph(Material);
		UEdGraph* Graph = Material ? (UEdGraph*)Material->MaterialGraph : (MatFunc ? MatFunc->MaterialGraph : nullptr);
		if (!Graph) return FMCPToolResult::Error(FString::Printf(TEXT("'%s' has no material graph"), *AssetDisplayName));

		UEdGraphNode* SourceNode = nullptr;
		UEdGraphNode* TargetNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (MatchesMaterialNode(Node, SourceNodeId)) SourceNode = Node;
			if (MatchesMaterialNode(Node, TargetNodeId)) TargetNode = Node;
			if (SourceNode && TargetNode) break;
		}
		if (!SourceNode) return FMCPToolResult::Error(FString::Printf(TEXT("Source node '%s' not found"), *SourceNodeId));
		if (!TargetNode) return FMCPToolResult::Error(FString::Printf(TEXT("Target node '%s' not found"), *TargetNodeId));

		UObject* Asset = Material ? (UObject*)Material : (UObject*)MatFunc;
		UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
		FMCPToolResult BoundaryResult = RequireMaterialMutationBoundary(Asset, Params, BoundaryValidation);
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}
		BoundaryResult = RequireMaterialBoundaryNode(
			BoundaryValidation,
			SourceNode,
			TEXT("source"),
			true);
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}
		BoundaryResult = RequireMaterialBoundaryNode(
			BoundaryValidation,
			TargetNode,
			TEXT("target"),
			!TargetNode->IsA<UMaterialGraphNode_Root>());
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}

		UEdGraphPin* SourcePin = SourceNode->FindPin(FName(*SourcePinName));
		if (!SourcePin) return FMCPToolResult::Error(FString::Printf(TEXT("Source pin '%s' not found"), *SourcePinName));
		UEdGraphPin* TargetPin = TargetNode->FindPin(FName(*TargetPinName));
		if (!TargetPin) return FMCPToolResult::Error(FString::Printf(TEXT("Target pin '%s' not found"), *TargetPinName));

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema) return FMCPToolResult::Error(TEXT("Material graph schema not found"));

		TArray<FMaterialGraphLinkSnapshot> LinkSnapshot;
		CaptureMaterialGraphLinks(Graph, LinkSnapshot);
		BeginMaterialMutation(Asset, Params);
		Graph->Modify();
		SourceNode->Modify();
		TargetNode->Modify();
		bool bConnected = UEAIIntegration::Infrastructure::TryCreateConnection(
			Schema, SourcePin, TargetPin,
			UEAIIntegration::Workflow::ShouldDeferCompile(Params));

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), bConnected);
		Result->SetBoolField(TEXT("connected"), bConnected);
		Result->SetStringField(TEXT("material"), AssetDisplayName);

		if (bConnected)
		{
			FinalizeMaterialMutation(Asset, Params);
			FString ExpectedAfterProjectionHash;
			FString ActualProjectionHash;
			const FMCPToolResult Postcondition = VerifyMaterialMutationPostcondition(
				Asset,
				BoundaryValidation,
				Params,
				ExpectedAfterProjectionHash,
				ActualProjectionHash);
			if (!Postcondition.bSuccess)
			{
				return RollbackMaterialMutation(
					Asset,
					Graph,
					LinkSnapshot,
					BoundaryValidation,
					Params,
					Postcondition);
			}
			const bool bSaved = SaveMaterialForExecution(Asset, Params);
			Result->SetBoolField(TEXT("saved"), bSaved);
			AddMaterialBoundaryResultFields(
				BoundaryValidation,
				TArray<FString>(),
				Result);
		}
		else
		{
			RestoreMaterialGraphLinks(Graph, LinkSnapshot);
			CancelMaterialMutation(Asset, Params);
			Result->SetStringField(TEXT("error"), TEXT("Cannot connect — types may be incompatible"));
		}
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// disconnect_material_pin
// ============================================================
class FTool_DisconnectMaterialPin : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.pin.disconnect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
        if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
            return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);
		if (Params->HasField(TEXT("direction")))
			return FMCPToolResult::Error(TEXT("direction requires targetContext=editorPreview."), TEXT("invalid_preview_edit"), 400);

		if (Params->HasField(TEXT("materialFunction")))
			return MutateMaterialFunction(GetCapabilityId(), Params);
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString MaterialFunctionName;
		Params->TryGetStringField(TEXT("materialFunction"), MaterialFunctionName);
		FString NodeId = Params->GetStringField(TEXT("nodeId"));
		FString PinName = Params->GetStringField(TEXT("pinName"));

		if (MaterialName.IsEmpty() && MaterialFunctionName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: 'material' or 'materialFunction'"));
		if (NodeId.IsEmpty() || PinName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: nodeId, pinName"));

		UMaterial* Material = nullptr;
		UMaterialFunction* MatFunc = nullptr;
		FString AssetDisplayName;
		if (!MaterialFunctionName.IsEmpty())
		{
			FString E; MatFunc = LoadMaterialFunctionByName(MaterialFunctionName, E);
			if (!MatFunc) return FMCPToolResult::Error(E);
			AssetDisplayName = MatFunc->GetName();
		}
		else
		{
			FString E; Material = LoadMaterialByName(MaterialName, E);
			if (!Material) return FMCPToolResult::Error(E);
			AssetDisplayName = Material->GetName();
		}

		if (Material) EnsureMaterialGraph(Material);
		UEdGraph* Graph = Material ? (UEdGraph*)Material->MaterialGraph : (MatFunc ? MatFunc->MaterialGraph : nullptr);
		if (!Graph) return FMCPToolResult::Error(FString::Printf(TEXT("'%s' has no material graph"), *AssetDisplayName));

		UEdGraphNode* TargetNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (MatchesMaterialNode(Node, NodeId)) { TargetNode = Node; break; }
		}
		if (!TargetNode) return FMCPToolResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

		UObject* Asset = Material ? (UObject*)Material : (UObject*)MatFunc;
		UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
		FMCPToolResult BoundaryResult = RequireMaterialMutationBoundary(Asset, Params, BoundaryValidation);
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}
		BoundaryResult = RequireMaterialBoundaryNode(
			BoundaryValidation,
			TargetNode,
			TEXT("target"),
			!TargetNode->IsA<UMaterialGraphNode_Root>());
		if (!BoundaryResult.bSuccess)
		{
			return BoundaryResult;
		}

		UEdGraphPin* Pin = TargetNode->FindPin(FName(*PinName));
		if (!Pin) return FMCPToolResult::Error(FString::Printf(TEXT("Pin '%s' not found on node '%s'"), *PinName, *NodeId));

		TArray<FMaterialGraphLinkSnapshot> LinkSnapshot;
		CaptureMaterialGraphLinks(Graph, LinkSnapshot);
		BeginMaterialMutation(Asset, Params);
		Graph->Modify();
		TargetNode->Modify();
		if (UMaterialGraphNode* MaterialNode =
			Cast<UMaterialGraphNode>(TargetNode))
		{
			if (MaterialNode->MaterialExpression)
			{
				MaterialNode->MaterialExpression->Modify();
			}
		}
		const int32 BrokenCount = Pin->LinkedTo.Num();
		Pin->BreakAllPinLinks();
		if (!UEAIIntegration::Workflow::ShouldDeferCompile(Params))
		{
			CastChecked<UMaterialGraph>(Graph)->LinkMaterialExpressionsFromGraph();
			Graph->NotifyGraphChanged();
		}
		FinalizeMaterialMutation(Asset, Params);
		FString ExpectedAfterProjectionHash;
		FString ActualProjectionHash;
		const FMCPToolResult Postcondition = VerifyMaterialMutationPostcondition(
			Asset,
			BoundaryValidation,
			Params,
			ExpectedAfterProjectionHash,
			ActualProjectionHash);
		if (!Postcondition.bSuccess)
		{
			return RollbackMaterialMutation(
				Asset,
				Graph,
				LinkSnapshot,
				BoundaryValidation,
				Params,
				Postcondition);
		}
		const bool bSaved = SaveMaterialForExecution(Asset, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), AssetDisplayName);
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetStringField(TEXT("pinName"), PinName);
		Result->SetNumberField(TEXT("brokenLinkCount"), BrokenCount);
		Result->SetBoolField(TEXT("saved"), bSaved);
		AddMaterialBoundaryResultFields(
			BoundaryValidation,
			TArray<FString>(),
			Result);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_expression_value
// ============================================================
class FTool_SetExpressionValue : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.expression.value.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
        if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
            return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);

		FString MaterialName;
		Params->TryGetStringField(TEXT("material"), MaterialName);
		FString MaterialFunctionName;
		Params->TryGetStringField(TEXT("materialFunction"), MaterialFunctionName);
		if (!MaterialName.IsEmpty() && !MaterialFunctionName.IsEmpty()) return FMCPToolResult::Error(TEXT("Specify only one material target."));
		FString NodeId = Params->GetStringField(TEXT("nodeId"));

		if (MaterialName.IsEmpty() && MaterialFunctionName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: 'material' or 'materialFunction'"));
		if (NodeId.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing: nodeId"));
		if (!Params->HasField(TEXT("value"))) return FMCPToolResult::Error(TEXT("Missing: value"));

		UMaterial* Material = nullptr;
		UMaterialFunction* MatFunc = nullptr;
		FString AssetDisplayName;
		if (!MaterialFunctionName.IsEmpty())
		{
			FString E; MatFunc = LoadMaterialFunctionByName(MaterialFunctionName, E);
			if (!MatFunc) return FMCPToolResult::Error(E);
			AssetDisplayName = MatFunc->GetName();
		}
		else
		{
			FString E; Material = LoadMaterialByName(MaterialName, E);
			if (!Material) return FMCPToolResult::Error(E);
			AssetDisplayName = Material->GetName();
		}

		if (Material) EnsureMaterialGraph(Material);
		UEdGraph* Graph = Material ? (UEdGraph*)Material->MaterialGraph : (MatFunc ? MatFunc->MaterialGraph : nullptr);
		if (!Graph && !MatFunc) return FMCPToolResult::Error(FString::Printf(TEXT("'%s' has no material graph"), *AssetDisplayName));

		UMaterialGraphNode* TargetMatNode = nullptr;
		if (Graph) for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (MatchesMaterialNode(Node, NodeId)) { TargetMatNode = Cast<UMaterialGraphNode>(Node); break; }
		}
		if (!TargetMatNode && !MatFunc) return FMCPToolResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

		UMaterialExpression* Expr = MatFunc ? FindFunctionExpression(MatFunc, NodeId) : TargetMatNode->MaterialExpression.Get();
		if (!Expr) return FMCPToolResult::Error(TEXT("Node has no material expression"));

		UObject* Asset = Material ? (UObject*)Material : (UObject*)MatFunc;
		TArray<FString> ConsumerIds;
		CollectMaterialExpressionConsumers(Material, MatFunc, Expr, ConsumerIds);
		const bool bSharedNode = ConsumerIds.Num() > 1;
		const bool bApprovedWorkflow =
			UEAIIntegration::Workflow::IsApprovedWorkflowExecution(Params);
		UEAIIntegration::MaterialQuery::FBoundaryWriteValidation BoundaryValidation;
		if (Params->HasField(TEXT("boundaryId"))
			|| (bSharedNode && !bApprovedWorkflow))
		{
			FMCPToolResult BoundaryResult = RequireMaterialMutationBoundary(
				Asset,
				Params,
				BoundaryValidation);
			if (!BoundaryResult.bSuccess)
			{
				return BoundaryResult;
			}
			if (BoundaryValidation.Boundary.IsValid())
			{
				if (!TargetMatNode)
				{
					return FMCPToolResult::Error(
						TEXT("The edited expression has no stable material graph node identity for boundary validation."),
						TEXT("material_boundary_node_identity_missing"),
						409);
				}
				BoundaryResult = RequireMaterialBoundaryNode(
					BoundaryValidation,
					TargetMatNode,
					TEXT("edited"),
					true);
				if (!BoundaryResult.bSuccess)
				{
					return BoundaryResult;
				}
			}
		}
		BeginMaterialMutation(Asset, Params);
		if (Graph) Graph->Modify();
		if (TargetMatNode) TargetMatNode->Modify();
		Expr->Modify();

		FString ExprType, NewValueStr;

		if (UMaterialExpressionConstant* CE = Cast<UMaterialExpressionConstant>(Expr))
		{
			ExprType = TEXT("Constant");
			double V = Params->GetNumberField(TEXT("value"));
			CE->R = (float)V;
			NewValueStr = FString::Printf(TEXT("%f"), V);
		}
		else if (UMaterialExpressionConstant3Vector* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
		{
			ExprType = TEXT("Constant3Vector");
			const TSharedPtr<FJsonObject>* VO = nullptr;
			if (!Params->TryGetObjectField(TEXT("value"), VO) || !VO)
			{
				CancelMaterialMutation(Asset, Params);
				return FMCPToolResult::Error(TEXT("Constant3Vector requires value as {r, g, b}"));
			}
			double R = 0, G = 0, B = 0;
			(*VO)->TryGetNumberField(TEXT("r"), R); (*VO)->TryGetNumberField(TEXT("g"), G); (*VO)->TryGetNumberField(TEXT("b"), B);
			C3->Constant = FLinearColor((float)R, (float)G, (float)B);
			NewValueStr = FString::Printf(TEXT("(%f, %f, %f)"), R, G, B);
		}
		else if (UMaterialExpressionConstant4Vector* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
		{
			ExprType = TEXT("Constant4Vector");
			const TSharedPtr<FJsonObject>* VO = nullptr;
			if (!Params->TryGetObjectField(TEXT("value"), VO) || !VO)
			{
				CancelMaterialMutation(Asset, Params);
				return FMCPToolResult::Error(TEXT("Constant4Vector requires value as {r, g, b, a}"));
			}
			double R = 0, G = 0, B = 0, A = 1;
			(*VO)->TryGetNumberField(TEXT("r"), R); (*VO)->TryGetNumberField(TEXT("g"), G);
			(*VO)->TryGetNumberField(TEXT("b"), B); (*VO)->TryGetNumberField(TEXT("a"), A);
			C4->Constant = FLinearColor((float)R, (float)G, (float)B, (float)A);
			NewValueStr = FString::Printf(TEXT("(%f, %f, %f, %f)"), R, G, B, A);
		}
		else if (UMaterialExpressionScalarParameter* SP = Cast<UMaterialExpressionScalarParameter>(Expr))
		{
			ExprType = TEXT("ScalarParameter");
			double V = Params->GetNumberField(TEXT("value"));
			SP->DefaultValue = (float)V;
			NewValueStr = FString::Printf(TEXT("%f"), V);
			FString ParamName;
			if (Params->TryGetStringField(TEXT("parameterName"), ParamName) && !ParamName.IsEmpty())
				SP->ParameterName = FName(*ParamName);
		}
		else if (UMaterialExpressionVectorParameter* VP = Cast<UMaterialExpressionVectorParameter>(Expr))
		{
			ExprType = TEXT("VectorParameter");
			const TSharedPtr<FJsonObject>* VO = nullptr;
			if (!Params->TryGetObjectField(TEXT("value"), VO) || !VO)
			{
				CancelMaterialMutation(Asset, Params);
				return FMCPToolResult::Error(TEXT("VectorParameter requires value as {r, g, b, a}"));
			}
			double R = 0, G = 0, B = 0, A = 1;
			(*VO)->TryGetNumberField(TEXT("r"), R); (*VO)->TryGetNumberField(TEXT("g"), G);
			(*VO)->TryGetNumberField(TEXT("b"), B); (*VO)->TryGetNumberField(TEXT("a"), A);
			VP->DefaultValue = FLinearColor((float)R, (float)G, (float)B, (float)A);
			NewValueStr = FString::Printf(TEXT("(%f, %f, %f, %f)"), R, G, B, A);
			FString ParamName;
			if (Params->TryGetStringField(TEXT("parameterName"), ParamName) && !ParamName.IsEmpty())
				VP->ParameterName = FName(*ParamName);
		}
		else if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expr))
		{
			ExprType = TEXT("Custom");
			FString Code = Params->GetStringField(TEXT("value"));
			Custom->Code = Code;
			NewValueStr = FString::Printf(TEXT("Code: %d chars"), Code.Len());
		}
		else if (UMaterialExpressionComponentMask* CM = Cast<UMaterialExpressionComponentMask>(Expr))
		{
			ExprType = TEXT("ComponentMask");
			const TSharedPtr<FJsonObject>* VO = nullptr;
			if (!Params->TryGetObjectField(TEXT("value"), VO) || !VO)
			{
				CancelMaterialMutation(Asset, Params);
				return FMCPToolResult::Error(TEXT("ComponentMask requires value as {r, g, b, a} (booleans)"));
			}
			bool bR = false, bG = false, bB = false, bA = false;
			(*VO)->TryGetBoolField(TEXT("r"), bR); (*VO)->TryGetBoolField(TEXT("g"), bG);
			(*VO)->TryGetBoolField(TEXT("b"), bB); (*VO)->TryGetBoolField(TEXT("a"), bA);
			CM->R = bR ? 1 : 0; CM->G = bG ? 1 : 0; CM->B = bB ? 1 : 0; CM->A = bA ? 1 : 0;
			NewValueStr = FString::Printf(TEXT("(R=%s, G=%s, B=%s, A=%s)"),
				bR ? TEXT("true") : TEXT("false"), bG ? TEXT("true") : TEXT("false"),
				bB ? TEXT("true") : TEXT("false"), bA ? TEXT("true") : TEXT("false"));
		}
		else
		{
			CancelMaterialMutation(Asset, Params);
			return FMCPToolResult::Error(FString::Printf(TEXT("Expression type '%s' does not support direct value setting"), *Expr->GetClass()->GetName()));
		}

		FinalizeMaterialMutation(Asset, Params);
		const bool bSaved = SaveMaterialForExecution(Asset, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), AssetDisplayName);
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetStringField(TEXT("expressionType"), ExprType);
		Result->SetStringField(TEXT("newValue"), NewValueStr);
		Result->SetBoolField(TEXT("saved"), bSaved);
		AddMaterialBoundaryResultFields(
			BoundaryValidation,
			ConsumerIds,
			Result,
			bApprovedWorkflow);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// move_material_expression
// ============================================================
class FTool_MoveMaterialExpression : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.expression.move");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
        if (UEAIIntegration::MaterialEditing::RoutesToPreview(Params))
            return UEAIIntegration::MaterialEditing::MutatePreviewGraph(GetCapabilityId(), Params);

		if (Params->HasField(TEXT("materialFunction")))
			return MutateMaterialFunction(GetCapabilityId(), Params);
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString MaterialFunctionName;
		Params->TryGetStringField(TEXT("materialFunction"), MaterialFunctionName);
		FString NodeId = Params->GetStringField(TEXT("nodeId"));

		if (MaterialName.IsEmpty() && MaterialFunctionName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: 'material' or 'materialFunction'"));
		if (NodeId.IsEmpty() || !Params->HasField(TEXT("posX")) || !Params->HasField(TEXT("posY")))
			return FMCPToolResult::Error(TEXT("Missing: nodeId, posX, posY"));

		int32 PosX = (int32)Params->GetNumberField(TEXT("posX"));
		int32 PosY = (int32)Params->GetNumberField(TEXT("posY"));

		UMaterial* Material = nullptr;
		UMaterialFunction* MatFunc = nullptr;
		FString AssetDisplayName;
		if (!MaterialFunctionName.IsEmpty())
		{
			FString E; MatFunc = LoadMaterialFunctionByName(MaterialFunctionName, E);
			if (!MatFunc) return FMCPToolResult::Error(E);
			AssetDisplayName = MatFunc->GetName();
		}
		else
		{
			FString E; Material = LoadMaterialByName(MaterialName, E);
			if (!Material) return FMCPToolResult::Error(E);
			AssetDisplayName = Material->GetName();
		}

		if (Material) EnsureMaterialGraph(Material);
		UEdGraph* Graph = Material ? (UEdGraph*)Material->MaterialGraph : (MatFunc ? MatFunc->MaterialGraph : nullptr);
		if (!Graph) return FMCPToolResult::Error(FString::Printf(TEXT("'%s' has no material graph"), *AssetDisplayName));

		UMaterialGraphNode* TargetMatNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (MatchesMaterialNode(Node, NodeId)) { TargetMatNode = Cast<UMaterialGraphNode>(Node); break; }
		}
		if (!TargetMatNode) return FMCPToolResult::Error(FString::Printf(TEXT("Node '%s' not found"), *NodeId));

		UObject* Asset = Material ? (UObject*)Material : (UObject*)MatFunc;
		BeginMaterialMutation(Asset, Params);
		Graph->Modify();
		TargetMatNode->Modify();
		TargetMatNode->NodePosX = PosX;
		TargetMatNode->NodePosY = PosY;
		if (TargetMatNode->MaterialExpression)
		{
			TargetMatNode->MaterialExpression->Modify();
			TargetMatNode->MaterialExpression->MaterialExpressionEditorX = PosX;
			TargetMatNode->MaterialExpression->MaterialExpressionEditorY = PosY;
		}

		FinalizeMaterialMutation(Asset, Params);
		const bool bSaved = SaveMaterialForExecution(Asset, Params);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("material"), AssetDisplayName);
		Result->SetStringField(TEXT("nodeId"), NodeId);
		Result->SetNumberField(TEXT("posX"), PosX);
		Result->SetNumberField(TEXT("posY"), PosY);
		Result->SetBoolField(TEXT("saved"), bSaved);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// create_material_instance
// ============================================================
class FTool_CreateMaterialInstance : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.instance.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		FString PackagePath = Params->GetStringField(TEXT("packagePath"));
		FString ParentName = Params->GetStringField(TEXT("parent"));
		if (Name.IsEmpty() || PackagePath.IsEmpty() || ParentName.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: name, packagePath, parent"));

		FString LoadError;
		UMaterialInterface* Parent = LoadMaterialInterfaceByName(ParentName, LoadError);
		if (!Parent) return FMCPToolResult::Error(LoadError);
		if (!PackagePath.StartsWith(TEXT("/Game/"))
			|| !FPackageName::IsValidLongPackageName(PackagePath)
			|| Name.Contains(TEXT("/")) || Name.Contains(TEXT("."))
			|| !FPackageName::IsValidLongPackageName(PackagePath / Name))
		{
			return FMCPToolResult::Error(TEXT("Use a valid asset name and /Game/ package directory."), TEXT("invalid_request"), 400);
		}

		FString FullPath = PackagePath / Name;
		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(FullPath))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *FullPath));
		}
		UPackage* Package = CreatePackage(*FullPath);
		if (!Package) return FMCPToolResult::Error(TEXT("Failed to create package"));

		UMaterialInstanceConstant* MI = NewObject<UMaterialInstanceConstant>(Package, FName(*Name), RF_Public | RF_Standalone);
		if (!MI) return FMCPToolResult::Error(TEXT("Failed to create MaterialInstanceConstant"));

		MI->SetParentEditorOnly(Parent);
		if (MI->Parent != Parent)
		{
			return FMCPToolResult::Error(TEXT("Material instance parent was not applied."), TEXT("parent_assignment_failed"), 500);
		}
		MI->MarkPackageDirty();
		bool bSaved = SaveMaterialPackage(MI);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("name"), Name);
		Result->SetStringField(TEXT("path"), MI->GetPathName());
		Result->SetStringField(TEXT("parent"), Parent->GetPathName());
		Result->SetBoolField(TEXT("saved"), bSaved);
		Result->SetBoolField(TEXT("parentVerified"), MI->Parent == Parent);
		if (!bSaved)
		{
			FMCPToolResult Failure = FMCPToolResult::Error(
				TEXT("Material instance was created and parent verified, but package save failed."),
				TEXT("material_instance_save_failed"), 500);
			Failure.Data = Result;
			return Failure;
		}
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// set_material_instance_parameter
// ============================================================
namespace UEAIIntegrationTools { FMCPToolResult SetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Params); }
class FTool_SetMaterialInstanceParameter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.material.instance.parameter.set"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return UEAIIntegrationTools::SetMaterialInstanceParameter(Params);
	}
};

namespace
{
constexpr const TCHAR* MaterialRestoreProjection =
	TEXT("legacyMaterialGraph.connectionTopology.v1");

void AppendMaterialGraphDigestField(FString& Canonical, const FString& Value)
{
	Canonical += FString::Printf(TEXT("%d:"), Value.Len());
	Canonical += Value;
	Canonical += TEXT("|");
}

FString MakeMaterialConnectionKey(const FPinConnectionRecord& Connection)
{
	FString Key;
	Key.Reserve(
		Connection.SourceNodeGuid.Len()
		+ Connection.SourcePinName.Len()
		+ Connection.TargetNodeGuid.Len()
		+ Connection.TargetPinName.Len()
		+ 64);
	AppendMaterialGraphDigestField(Key, Connection.SourceNodeGuid);
	AppendMaterialGraphDigestField(Key, Connection.SourcePinName);
	AppendMaterialGraphDigestField(Key, Connection.TargetNodeGuid);
	AppendMaterialGraphDigestField(Key, Connection.TargetPinName);
	return Key;
}

bool ComputeMaterialConnectionStateDigest(
	const FString& AssetPath,
	const FGraphSnapshotData& Data,
	FString& OutDigest)
{
	TArray<FString> NodeRecords;
	NodeRecords.Reserve(Data.Nodes.Num());
	for (const FNodeRecord& Node : Data.Nodes)
	{
		FString Record;
		AppendMaterialGraphDigestField(Record, Node.NodeGuid);
		AppendMaterialGraphDigestField(Record, Node.NodeClass);
		AppendMaterialGraphDigestField(Record, Node.NodeTitle);
		AppendMaterialGraphDigestField(Record, Node.StructType);
		NodeRecords.Add(MoveTemp(Record));
	}
	NodeRecords.Sort();

	TArray<FString> ConnectionRecords;
	ConnectionRecords.Reserve(Data.Connections.Num());
	for (const FPinConnectionRecord& Connection : Data.Connections)
	{
		ConnectionRecords.Add(MakeMaterialConnectionKey(Connection));
	}
	ConnectionRecords.Sort();

	FString Canonical(TEXT("ue.material.connection-state/1|"));
	AppendMaterialGraphDigestField(Canonical, AssetPath);
	AppendMaterialGraphDigestField(Canonical, MaterialRestoreProjection);
	Canonical += FString::Printf(
		TEXT("nodes:%d|connections:%d|"),
		NodeRecords.Num(),
		ConnectionRecords.Num());
	for (const FString& Record : NodeRecords)
	{
		AppendMaterialGraphDigestField(Canonical, Record);
	}
	for (const FString& Record : ConnectionRecords)
	{
		AppendMaterialGraphDigestField(Canonical, Record);
	}

	FTCHARToUTF8 Utf8(*Canonical);
	FString Hex;
	if (!UEAIIntegration::Infrastructure::TrySha256Hex(
			Utf8.Get(),
			static_cast<uint64>(Utf8.Length()),
			Hex))
	{
		OutDigest.Reset();
		return false;
	}
	OutDigest = TEXT("sha256:") + Hex;
	return true;
}

bool IsMaterialConnectionStateDigest(const FString& Digest)
{
	if (!Digest.StartsWith(TEXT("sha256:")) || Digest.Len() != 71)
	{
		return false;
	}
	for (int32 Index = 7; Index < Digest.Len(); ++Index)
	{
		if (!FChar::IsHexDigit(Digest[Index]))
		{
			return false;
		}
	}
	return true;
}

UEdGraphPin* FindUniqueMaterialRestorePin(
	UEdGraphNode* Node,
	const FString& PinName,
	const EEdGraphPinDirection ExpectedDirection)
{
	if (!Node)
	{
		return nullptr;
	}
	UEdGraphPin* Match = nullptr;
	int32 MatchCount = 0;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString() == PinName)
		{
			++MatchCount;
			Match = Pin;
		}
	}
	return MatchCount == 1
		&& Match
		&& Match->Direction == ExpectedDirection
		&& Match->GetOwningNode() == Node
		? Match
		: nullptr;
}

struct FMaterialRestoreConnection
{
	FString Key;
	UEdGraphNode* SourceNode = nullptr;
	UEdGraphPin* SourcePin = nullptr;
	UEdGraphNode* TargetNode = nullptr;
	UEdGraphPin* TargetPin = nullptr;
};

struct FMaterialRestoreRollbackStatus
{
	bool bSemanticRollbackVerified = false;
	bool bDirtyRestored = false;

	bool IsVerified() const
	{
		return bSemanticRollbackVerified && bDirtyRestored;
	}
};

bool ContainsAllSnapshotConnections(
	const FGraphSnapshotData& SnapshotData,
	const FGraphSnapshotData& CurrentData)
{
	TSet<FString> CurrentConnections;
	CurrentConnections.Reserve(CurrentData.Connections.Num());
	for (const FPinConnectionRecord& Connection : CurrentData.Connections)
	{
		CurrentConnections.Add(MakeMaterialConnectionKey(Connection));
	}
	for (const FPinConnectionRecord& Connection : SnapshotData.Connections)
	{
		if (!CurrentConnections.Contains(MakeMaterialConnectionKey(Connection)))
		{
			return false;
		}
	}
	return true;
}

bool HasSameMaterialConnectionTopology(
	const FGraphSnapshotData& Left,
	const FGraphSnapshotData& Right)
{
	TSet<FString> LeftConnections;
	TSet<FString> RightConnections;
	LeftConnections.Reserve(Left.Connections.Num());
	RightConnections.Reserve(Right.Connections.Num());
	for (const FPinConnectionRecord& Connection : Left.Connections)
	{
		LeftConnections.Add(MakeMaterialConnectionKey(Connection));
	}
	for (const FPinConnectionRecord& Connection : Right.Connections)
	{
		RightConnections.Add(MakeMaterialConnectionKey(Connection));
	}
	return LeftConnections.Num() == RightConnections.Num()
		&& LeftConnections.Includes(RightConnections);
}

FString SummarizeMaterialConnections(const FGraphSnapshotData& Data)
{
	TArray<FString> Keys;
	Keys.Reserve(Data.Connections.Num());
	for (const FPinConnectionRecord& Connection : Data.Connections)
	{
		Keys.Add(MakeMaterialConnectionKey(Connection));
	}
	Keys.Sort();
	return FString::Join(Keys, TEXT(";"));
}
}

// ============================================================
// snapshot_material_graph
// ============================================================
class FTool_SnapshotMaterialGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.graph.snapshot");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName = Params->GetStringField(TEXT("material"));
		if (MaterialName.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing: material"));

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
		if (!Material) return FMCPToolResult::Error(LoadError);

		EnsureMaterialGraph(Material);
		if (!Material->MaterialGraph) return FMCPToolResult::Error(TEXT("Material has no graph"));

		FGraphSnapshot Snapshot;
		Snapshot.SnapshotId = MCPHelpers::GenerateSnapshotId(MaterialName);
		Snapshot.BlueprintName = Material->GetName();
		Snapshot.BlueprintPath = Material->GetPathName();
		Snapshot.CreatedAt = FDateTime::Now();

		FGraphSnapshotData GraphData = MCPHelpers::CaptureGraphSnapshot(
			Material->MaterialGraph,
			true);
		int32 NodeCount = GraphData.Nodes.Num();
		int32 ConnectionCount = GraphData.Connections.Num();
		FString StateDigest;
		if (!ComputeMaterialConnectionStateDigest(
				Snapshot.BlueprintPath,
				GraphData,
				StateDigest))
		{
			return FMCPToolResult::Error(
				TEXT("Could not hash the material graph snapshot."),
				TEXT("state_digest_unavailable"),
				500);
		}
		Snapshot.Graphs.Add(TEXT("MaterialGraph"), MoveTemp(GraphData));

		MCPHelpers::GetMaterialSnapshots().Add(Snapshot.SnapshotId, Snapshot);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("snapshotId"), Snapshot.SnapshotId);
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("assetPath"), Snapshot.BlueprintPath);
		Result->SetStringField(TEXT("stateDigest"), StateDigest);
		Result->SetStringField(
			TEXT("projectionIdentity"), MaterialRestoreProjection);
		Result->SetStringField(
			TEXT("restoreSemantics"), TEXT("missingConnectionsOnly"));
		Result->SetBoolField(TEXT("fullGraphRestore"), false);
		Result->SetNumberField(TEXT("nodeCount"), NodeCount);
		Result->SetNumberField(TEXT("connectionCount"), ConnectionCount);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// diff_material_graph
// ============================================================
class FTool_DiffMaterialGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.graph.diff");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName = Params->GetStringField(TEXT("material"));
		FString SnapshotId = Params->GetStringField(TEXT("snapshotId"));
		if (MaterialName.IsEmpty() || SnapshotId.IsEmpty())
			return FMCPToolResult::Error(TEXT("Missing: material, snapshotId"));

		FGraphSnapshot* SnapshotPtr = MCPHelpers::GetMaterialSnapshots().Find(SnapshotId);
		if (!SnapshotPtr) return FMCPToolResult::Error(FString::Printf(TEXT("Snapshot '%s' not found"), *SnapshotId));

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
		if (!Material) return FMCPToolResult::Error(LoadError);
		if (SnapshotPtr->BlueprintPath != Material->GetPathName())
		{
			return FMCPToolResult::Error(
				TEXT("The snapshot belongs to a different material asset."),
				TEXT("snapshot_asset_mismatch"),
				409);
		}

		EnsureMaterialGraph(Material);
		if (!Material->MaterialGraph) return FMCPToolResult::Error(TEXT("Material has no graph"));

		FGraphSnapshotData CurrentData = MCPHelpers::CaptureGraphSnapshot(
			Material->MaterialGraph,
			true);
		const FGraphSnapshotData* SnapData = SnapshotPtr->Graphs.Find(TEXT("MaterialGraph"));
		if (!SnapData) return FMCPToolResult::Error(TEXT("Snapshot has no MaterialGraph"));

		TSet<FString> SnapConnSet, CurConnSet;
		for (const FPinConnectionRecord& C : SnapData->Connections) SnapConnSet.Add(MakeMaterialConnectionKey(C));
		for (const FPinConnectionRecord& C : CurrentData.Connections) CurConnSet.Add(MakeMaterialConnectionKey(C));

		TMap<FString, const FNodeRecord*> CurNodeLookup;
		for (const FNodeRecord& NR : CurrentData.Nodes) CurNodeLookup.Add(NR.NodeGuid, &NR);

		TArray<TSharedPtr<FJsonValue>> SeveredArr, NewConnsArr, MissingNodesArr;

		for (const FPinConnectionRecord& C : SnapData->Connections)
		{
			if (!CurConnSet.Contains(MakeMaterialConnectionKey(C)))
			{
				TSharedRef<FJsonObject> SJ = MakeShared<FJsonObject>();
				SJ->SetStringField(TEXT("sourceNodeGuid"), C.SourceNodeGuid);
				SJ->SetStringField(TEXT("sourcePinName"), C.SourcePinName);
				SJ->SetStringField(TEXT("targetNodeGuid"), C.TargetNodeGuid);
				SJ->SetStringField(TEXT("targetPinName"), C.TargetPinName);
				SeveredArr.Add(MakeShared<FJsonValueObject>(SJ));
			}
		}
		for (const FPinConnectionRecord& C : CurrentData.Connections)
		{
			if (!SnapConnSet.Contains(MakeMaterialConnectionKey(C)))
			{
				TSharedRef<FJsonObject> NJ = MakeShared<FJsonObject>();
				NJ->SetStringField(TEXT("sourceNodeGuid"), C.SourceNodeGuid);
				NJ->SetStringField(TEXT("sourcePinName"), C.SourcePinName);
				NJ->SetStringField(TEXT("targetNodeGuid"), C.TargetNodeGuid);
				NJ->SetStringField(TEXT("targetPinName"), C.TargetPinName);
				NewConnsArr.Add(MakeShared<FJsonValueObject>(NJ));
			}
		}
		for (const FNodeRecord& SN : SnapData->Nodes)
		{
			if (!CurNodeLookup.Contains(SN.NodeGuid))
			{
				TSharedRef<FJsonObject> MJ = MakeShared<FJsonObject>();
				MJ->SetStringField(TEXT("nodeGuid"), SN.NodeGuid);
				MJ->SetStringField(TEXT("nodeClass"), SN.NodeClass);
				MJ->SetStringField(TEXT("nodeTitle"), SN.NodeTitle);
				MissingNodesArr.Add(MakeShared<FJsonValueObject>(MJ));
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("assetPath"), Material->GetPathName());
		Result->SetStringField(TEXT("snapshotId"), SnapshotId);
		FString SnapshotStateDigest;
		FString CurrentStateDigest;
		if (!ComputeMaterialConnectionStateDigest(
				Material->GetPathName(),
				*SnapData,
				SnapshotStateDigest)
			|| !ComputeMaterialConnectionStateDigest(
					Material->GetPathName(),
					CurrentData,
					CurrentStateDigest))
		{
			return FMCPToolResult::Error(
				TEXT("Could not hash the material graph comparison."),
				TEXT("state_digest_unavailable"),
				500);
		}
		Result->SetStringField(TEXT("snapshotStateDigest"), SnapshotStateDigest);
		Result->SetStringField(TEXT("currentStateDigest"), CurrentStateDigest);
		Result->SetStringField(
			TEXT("projectionIdentity"), MaterialRestoreProjection);
		Result->SetStringField(
			TEXT("restoreSemantics"), TEXT("missingConnectionsOnly"));
		Result->SetBoolField(TEXT("fullGraphRestore"), false);
		Result->SetArrayField(TEXT("severedConnections"), SeveredArr);
		Result->SetArrayField(TEXT("newConnections"), NewConnsArr);
		Result->SetArrayField(TEXT("missingNodes"), MissingNodesArr);

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("severedConnections"), SeveredArr.Num());
		Summary->SetNumberField(TEXT("newConnections"), NewConnsArr.Num());
		Summary->SetNumberField(TEXT("missingNodes"), MissingNodesArr.Num());
		Result->SetObjectField(TEXT("summary"), Summary);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// restore_material_graph
// ============================================================
class FTool_RestoreMaterialGraph : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.graph.restore");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName;
		FString SnapshotId;
		FString ExpectedCurrentDigest;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("material"), MaterialName)
			|| !Params->TryGetStringField(TEXT("snapshotId"), SnapshotId)
			|| !Params->TryGetStringField(
				TEXT("expectedCurrentDigest"), ExpectedCurrentDigest)
			|| MaterialName.IsEmpty()
			|| SnapshotId.IsEmpty()
			|| !IsMaterialConnectionStateDigest(ExpectedCurrentDigest))
		{
			return FMCPToolResult::Error(
				TEXT("material, snapshotId and a sha256 expectedCurrentDigest are required."),
				TEXT("invalid_restore_request"),
				422);
		}

		bool bDryRun = false;
		bool bSave = false;
		FString RestoreMode = TEXT("connectionsOnly");
		bool bConfirmFullGraphRestore = false;
		if (Params->HasField(TEXT("restoreMode"))
			&& (!Params->TryGetStringField(TEXT("restoreMode"), RestoreMode)
				|| (RestoreMode != TEXT("connectionsOnly") && RestoreMode != TEXT("fullGraph"))))
		{
			return FMCPToolResult::Error(
				TEXT("restoreMode must be 'connectionsOnly' or 'fullGraph'."),
				TEXT("invalid_restore_request"),
				422);
		}
		if (Params->HasField(TEXT("confirmFullGraphRestore"))
			&& !Params->TryGetBoolField(TEXT("confirmFullGraphRestore"), bConfirmFullGraphRestore))
		{
			return FMCPToolResult::Error(
				TEXT("confirmFullGraphRestore must be a boolean."),
				TEXT("invalid_restore_request"),
				422);
		}
		if ((Params->HasField(TEXT("dryRun"))
				&& !Params->TryGetBoolField(TEXT("dryRun"), bDryRun))
			|| (Params->HasField(TEXT("save"))
				&& !Params->TryGetBoolField(TEXT("save"), bSave)))
		{
			return FMCPToolResult::Error(
				TEXT("dryRun and save must be booleans."),
				TEXT("invalid_restore_request"),
				422);
		}
		if (bSave)
		{
			return FMCPToolResult::Error(
				TEXT("This limited restore is dirty-only. Persist through an approved Workflow with a durable checkpoint."),
				TEXT("restore_save_not_supported"),
				409);
		}

		FGraphSnapshot* SnapshotPtr =
			MCPHelpers::GetMaterialSnapshots().Find(SnapshotId);
		if (!SnapshotPtr)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Snapshot '%s' is unavailable in this Editor instance."),
					*SnapshotId),
				TEXT("snapshot_not_found"),
				410);
		}

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
		if (!Material)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("material_not_found"),
				404);
		}
		const FString AssetPath = Material->GetPathName();
		if (SnapshotPtr->BlueprintPath != AssetPath)
		{
			return FMCPToolResult::Error(
				TEXT("The snapshot belongs to a different material asset."),
				TEXT("snapshot_asset_mismatch"),
				409);
		}

		EnsureMaterialGraph(Material);
		UMaterialGraph* Graph = Material->MaterialGraph;
		if (!Graph)
		{
			return FMCPToolResult::Error(
				TEXT("Material has no graph."),
				TEXT("material_graph_unavailable"),
				409);
		}
		const FGraphSnapshotData* SnapshotData =
			SnapshotPtr->Graphs.Find(TEXT("MaterialGraph"));
		if (!SnapshotData)
		{
			return FMCPToolResult::Error(
				TEXT("Snapshot has no MaterialGraph projection."),
				TEXT("snapshot_projection_mismatch"),
				409);
		}

		const FGraphSnapshotData BeforeData =
			MCPHelpers::CaptureGraphSnapshot(Graph, true);
		FString BeforeDigest;
		FString SnapshotDigest;
		if (!ComputeMaterialConnectionStateDigest(
				AssetPath,
				BeforeData,
				BeforeDigest)
			|| !ComputeMaterialConnectionStateDigest(
					AssetPath,
					*SnapshotData,
					SnapshotDigest))
		{
			return FMCPToolResult::Error(
				TEXT("Could not hash the material graph restore state."),
				TEXT("state_digest_unavailable"),
				500);
		}
		if (ExpectedCurrentDigest != BeforeDigest)
		{
			return FMCPToolResult::Error(
				TEXT("The material graph changed after its current state was read."),
				TEXT("material_graph_state_conflict"),
				409);
		}
		const bool bFullGraphRestore = RestoreMode == TEXT("fullGraph");
		if (bFullGraphRestore && !bConfirmFullGraphRestore && !bDryRun)
		{
			return FMCPToolResult::Error(
				TEXT("fullGraph restore requires confirmFullGraphRestore=true because it may remove links absent from the snapshot."),
				TEXT("full_graph_restore_confirmation_required"),
				409);
		}
		if (bFullGraphRestore)
		{
			TSet<FString> SnapshotNodeIds;
			TSet<FString> CurrentNodeIds;
			for (const FNodeRecord& Node : SnapshotData->Nodes)
			{
				SnapshotNodeIds.Add(Node.NodeGuid);
			}
			for (const FNodeRecord& Node : BeforeData.Nodes)
			{
				CurrentNodeIds.Add(Node.NodeGuid);
			}
			bool bSameNodeSet = SnapshotNodeIds.Num() == CurrentNodeIds.Num();
			for (const FString& CurrentNodeId : CurrentNodeIds)
			{
				if (!SnapshotNodeIds.Contains(CurrentNodeId))
				{
					bSameNodeSet = false;
					break;
				}
			}
			if (!bSameNodeSet)
			{
				return FMCPToolResult::Error(
					TEXT("fullGraph restore requires the current graph to contain exactly the snapshot node set; node creation/removal is not inferred from legacy snapshots."),
					TEXT("full_graph_restore_node_set_mismatch"),
					409);
			}
		}

		TSet<FString> CurrentConnections;
		CurrentConnections.Reserve(BeforeData.Connections.Num());
		for (const FPinConnectionRecord& Connection : BeforeData.Connections)
		{
			CurrentConnections.Add(MakeMaterialConnectionKey(Connection));
		}

		TMap<FString, UEdGraphNode*> NodeLookup;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || Node->GetGraph() != Graph)
			{
				continue;
			}
			const FString NodeGuid = Node->NodeGuid.ToString();
			const FString StableNodeId = MaterialMutationNodeId(Node);
			if (NodeLookup.Contains(NodeGuid)
				|| (!StableNodeId.IsEmpty() && NodeLookup.Contains(StableNodeId)))
			{
				return FMCPToolResult::Error(
					TEXT("The live material graph has duplicate node GUIDs; no connection was changed."),
					TEXT("restore_preflight_failed"),
					409);
			}
			NodeLookup.Add(NodeGuid, Node);
			if (!StableNodeId.IsEmpty())
			{
				NodeLookup.Add(StableNodeId, Node);
			}
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (!Schema)
		{
			return FMCPToolResult::Error(
				TEXT("The material graph schema is unavailable; no connection was changed."),
				TEXT("restore_preflight_failed"),
				409);
		}

		TArray<FMaterialRestoreConnection> Plan;
		TSet<FString> PlannedKeys;
		for (const FPinConnectionRecord& Connection : SnapshotData->Connections)
		{
			const FString Key = MakeMaterialConnectionKey(Connection);
			if ((!bFullGraphRestore && CurrentConnections.Contains(Key))
				|| PlannedKeys.Contains(Key))
			{
				continue;
			}
			UEdGraphNode* const* SourceNodePtr =
				NodeLookup.Find(Connection.SourceNodeGuid);
			UEdGraphNode* const* TargetNodePtr =
				NodeLookup.Find(Connection.TargetNodeGuid);
			if (!SourceNodePtr || !TargetNodePtr
				|| !*SourceNodePtr || !*TargetNodePtr
				|| (*SourceNodePtr)->GetGraph() != Graph
				|| (*TargetNodePtr)->GetGraph() != Graph)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Snapshot connection '%s' references a missing or foreign node; no connection was changed."),
						*Key),
					TEXT("restore_preflight_failed"),
					409);
			}
			UEdGraphPin* SourcePin = FindUniqueMaterialRestorePin(
				*SourceNodePtr,
				Connection.SourcePinName,
				EGPD_Output);
			UEdGraphPin* TargetPin = FindUniqueMaterialRestorePin(
				*TargetNodePtr,
				Connection.TargetPinName,
				EGPD_Input);
			if (!SourcePin || !TargetPin
				|| SourcePin->GetOwningNode()->GetGraph() != Graph
				|| TargetPin->GetOwningNode()->GetGraph() != Graph)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Snapshot connection '%s' does not resolve to unique output/input pins in this graph; no connection was changed."),
						*Key),
					TEXT("restore_preflight_failed"),
					409);
			}
			const FPinConnectionResponse Response =
				Schema->CanCreateConnection(SourcePin, TargetPin);
			if (!bFullGraphRestore && Response.Response != CONNECT_RESPONSE_MAKE)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Snapshot connection '%s' is not a pure additive connection (%s); no connection was changed."),
						*Key,
						*Response.Message.ToString()),
					TEXT("restore_would_break_existing_connection"),
					409);
			}
			FMaterialRestoreConnection& Planned = Plan.AddDefaulted_GetRef();
			Planned.Key = Key;
			Planned.SourceNode = *SourceNodePtr;
			Planned.SourcePin = SourcePin;
			Planned.TargetNode = *TargetNodePtr;
			Planned.TargetPin = TargetPin;
			PlannedKeys.Add(Key);
		}
		Plan.Sort([](
			const FMaterialRestoreConnection& Left,
			const FMaterialRestoreConnection& Right)
		{
			return Left.Key < Right.Key;
		});

		FGraphSnapshotData ExpectedAfterData = BeforeData;
		if (bFullGraphRestore)
		{
			ExpectedAfterData.Connections = SnapshotData->Connections;
		}
		for (const FPinConnectionRecord& Connection : SnapshotData->Connections)
		{
			if (bFullGraphRestore)
			{
				continue;
			}
			const FString Key = MakeMaterialConnectionKey(Connection);
			if (!CurrentConnections.Contains(Key))
			{
				ExpectedAfterData.Connections.Add(Connection);
				CurrentConnections.Add(Key);
			}
		}
		FString ExpectedAfterDigest;
		if (!ComputeMaterialConnectionStateDigest(
				AssetPath,
				ExpectedAfterData,
				ExpectedAfterDigest))
		{
			return FMCPToolResult::Error(
				TEXT("Could not hash the expected post-restore state."),
				TEXT("state_digest_unavailable"),
				500);
		}

		auto MakeResult = [&]()
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("material"), Material->GetName());
			Result->SetStringField(TEXT("assetPath"), AssetPath);
			Result->SetStringField(TEXT("snapshotId"), SnapshotId);
			Result->SetStringField(TEXT("snapshotStateDigest"), SnapshotDigest);
			Result->SetStringField(TEXT("beforeStateDigest"), BeforeDigest);
			Result->SetStringField(
				TEXT("expectedAfterStateDigest"), ExpectedAfterDigest);
			Result->SetStringField(
				TEXT("projectionIdentity"), MaterialRestoreProjection);
			Result->SetStringField(
				TEXT("restoreSemantics"),
				bFullGraphRestore ? TEXT("fullGraph") : TEXT("missingConnectionsOnly"));
			Result->SetBoolField(TEXT("fullGraphRestore"), bFullGraphRestore);
			Result->SetBoolField(TEXT("preflightVerified"), true);
			Result->SetNumberField(
				TEXT("missingConnectionCount"), Plan.Num());
			Result->SetBoolField(TEXT("dryRun"), bDryRun);
			Result->SetBoolField(TEXT("saveRequested"), bSave);
			Result->SetBoolField(TEXT("saveSupported"), false);
			Result->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
			Result->SetBoolField(
				TEXT("compileDeferred"),
				UEAIIntegration::Workflow::ShouldDeferCompile(Params));
			Result->SetBoolField(TEXT("compileVerified"), false);
			return Result;
		};

		if (bDryRun)
		{
			TSharedRef<FJsonObject> Result = MakeResult();
			Result->SetNumberField(TEXT("appliedConnectionCount"), 0);
			Result->SetBoolField(TEXT("postconditionChecked"), false);
			Result->SetBoolField(TEXT("postconditionVerified"), false);
			Result->SetBoolField(TEXT("rollbackAttempted"), false);
			Result->SetBoolField(TEXT("rollbackVerified"), false);
			Result->SetBoolField(TEXT("compileRequested"), false);
			Result->SetBoolField(TEXT("saved"), false);
			return FMCPToolResult::Ok(Result);
		}

		const bool bFullGraphNoOp = bFullGraphRestore
			&& BeforeData.Connections.Num() == 0
			&& SnapshotData->Connections.Num() == 0;
		if (Plan.IsEmpty() && (!bFullGraphRestore || bFullGraphNoOp))
		{
			TSharedRef<FJsonObject> Result = MakeResult();
			Result->SetNumberField(TEXT("appliedConnectionCount"), 0);
			Result->SetStringField(TEXT("afterStateDigest"), BeforeDigest);
			Result->SetBoolField(TEXT("postconditionChecked"), true);
			Result->SetBoolField(TEXT("postconditionVerified"), true);
			Result->SetBoolField(TEXT("rollbackAttempted"), false);
			Result->SetBoolField(TEXT("rollbackVerified"), false);
			Result->SetBoolField(TEXT("compileRequested"), false);
			Result->SetBoolField(TEXT("saved"), false);
			return FMCPToolResult::Ok(Result);
		}

		const bool bDirtyBefore = Material->GetOutermost()->IsDirty();
		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Restore Material Graph Connections")));
		BeginMaterialMutation(Material, Params);
		Graph->Modify();
		TSet<UEdGraphNode*> ModifiedNodes;
		for (const FMaterialRestoreConnection& Connection : Plan)
		{
			UEdGraphNode* NodesToModify[] = {
				Connection.SourceNode,
				Connection.TargetNode};
			for (UEdGraphNode* Node : NodesToModify)
			{
				if (Node && !ModifiedNodes.Contains(Node))
				{
					Node->Modify();
					if (UMaterialGraphNode* MaterialNode =
						Cast<UMaterialGraphNode>(Node))
					{
						if (MaterialNode->MaterialExpression)
						{
							MaterialNode->MaterialExpression->Modify();
						}
					}
					ModifiedNodes.Add(Node);
				}
			}
		}

		TArray<int32> AppliedIndexes;
		AppliedIndexes.Reserve(Plan.Num());
		bool bMutationFinalized = false;
		auto ClearAllGraphLinks = [&]()
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						Pin->BreakAllPinLinks();
					}
				}
			}
		};
		auto LinkSnapshotConnections = [&](const FGraphSnapshotData& Data)
		{
			TMap<FString, UEdGraphNode*> NodesByGuid;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node)
				{
					NodesByGuid.Add(Node->NodeGuid.ToString(), Node);
					const FString StableNodeId = MaterialMutationNodeId(Node);
					if (!StableNodeId.IsEmpty())
					{
						NodesByGuid.Add(StableNodeId, Node);
					}
				}
			}
			for (const FPinConnectionRecord& Connection : Data.Connections)
			{
				UEdGraphNode* const* SourceNode = NodesByGuid.Find(Connection.SourceNodeGuid);
				UEdGraphNode* const* TargetNode = NodesByGuid.Find(Connection.TargetNodeGuid);
				if (!SourceNode || !TargetNode || !*SourceNode || !*TargetNode)
				{
					return false;
				}
				UEdGraphPin* SourcePin = nullptr;
				UEdGraphPin* TargetPin = nullptr;
				for (UEdGraphPin* Pin : (*SourceNode)->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinName.ToString() == Connection.SourcePinName)
					{
						if (SourcePin)
						{
							return false;
						}
						SourcePin = Pin;
					}
				}
				for (UEdGraphPin* Pin : (*TargetNode)->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input
						&& Pin->PinName.ToString() == Connection.TargetPinName)
					{
						if (TargetPin)
						{
							return false;
						}
						TargetPin = Pin;
					}
				}
				if (!SourcePin || !TargetPin)
				{
					return false;
				}
				SourcePin->MakeLinkTo(TargetPin);
				if (!SourcePin->LinkedTo.Contains(TargetPin)
					|| !TargetPin->LinkedTo.Contains(SourcePin))
				{
					return false;
				}
			}
			return true;
		};
		auto RollBackOwnedConnections = [&]()
		{
			FMaterialRestoreRollbackStatus Status;
			if (bMutationFinalized
				&& !UEAIIntegration::Workflow::ShouldDeferCompile(Params))
			{
				Material->PreEditChange(nullptr);
			}
			if (bFullGraphRestore)
			{
				ClearAllGraphLinks();
			}
			for (int32 Index = AppliedIndexes.Num() - 1; Index >= 0; --Index)
			{
				const FMaterialRestoreConnection& Connection =
					Plan[AppliedIndexes[Index]];
				if (Connection.SourcePin && Connection.TargetPin
					&& (Connection.SourcePin->LinkedTo.Contains(Connection.TargetPin)
						|| Connection.TargetPin->LinkedTo.Contains(Connection.SourcePin)))
				{
					Connection.SourcePin->BreakLinkTo(Connection.TargetPin);
					if (Connection.TargetPin->LinkedTo.Contains(Connection.SourcePin))
					{
						Connection.TargetPin->BreakLinkTo(Connection.SourcePin);
					}
				}
			}
			if (bFullGraphRestore && !LinkSnapshotConnections(BeforeData))
			{
				Status.bSemanticRollbackVerified = false;
			}
			Graph->LinkMaterialExpressionsFromGraph();
			FinalizeMaterialMutation(Material, Params);
			bMutationFinalized = true;
			const FGraphSnapshotData RestoredData =
				MCPHelpers::CaptureGraphSnapshot(Graph, true);
			FString RestoredDigest;
			Status.bSemanticRollbackVerified =
				ComputeMaterialConnectionStateDigest(
					AssetPath,
					RestoredData,
					RestoredDigest)
				&& RestoredDigest == BeforeDigest;
			if (Status.bSemanticRollbackVerified)
			{
				Material->GetOutermost()->SetDirtyFlag(bDirtyBefore);
			}
			Status.bDirtyRestored =
				Material->GetOutermost()->IsDirty() == bDirtyBefore;
			if (Status.IsVerified())
			{
				Transaction.Cancel();
			}
			return Status;
		};

		if (bFullGraphRestore)
		{
			ClearAllGraphLinks();
		}
		for (int32 Index = 0; Index < Plan.Num(); ++Index)
		{
			const FMaterialRestoreConnection& Connection = Plan[Index];
			const bool bCreated = Schema->TryCreateConnection(
				Connection.SourcePin,
				Connection.TargetPin);
			const bool bSourceLinked =
				Connection.SourcePin->LinkedTo.Contains(Connection.TargetPin);
			const bool bTargetLinked =
				Connection.TargetPin->LinkedTo.Contains(Connection.SourcePin);
			const bool bConnected = bSourceLinked && bTargetLinked;
			if (bSourceLinked || bTargetLinked)
			{
				AppliedIndexes.Add(Index);
			}
			if (!bCreated || !bConnected)
			{
				const FMaterialRestoreRollbackStatus Rollback =
					RollBackOwnedConnections();
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Connection restore failed after %d additions; semanticRollbackVerified=%s, dirtyRestored=%s, diskRollbackRequired=false."),
						AppliedIndexes.Num(),
						Rollback.bSemanticRollbackVerified ? TEXT("true") : TEXT("false"),
						Rollback.bDirtyRestored ? TEXT("true") : TEXT("false")),
					Rollback.IsVerified()
						? TEXT("restore_apply_failed")
						: TEXT("restore_rollback_failed"),
					500);
			}
		}

		Graph->LinkMaterialExpressionsFromGraph();
		FinalizeMaterialMutation(Material, Params);
		bMutationFinalized = true;
		const FGraphSnapshotData AfterData =
			MCPHelpers::CaptureGraphSnapshot(Graph, true);
		FString AfterDigest;
		const bool bPostconditionVerified =
			ComputeMaterialConnectionStateDigest(
				AssetPath,
				AfterData,
				AfterDigest)
			&& HasSameMaterialConnectionTopology(ExpectedAfterData, AfterData)
			&& ContainsAllSnapshotConnections(*SnapshotData, AfterData)
			&& (bFullGraphRestore || ContainsAllSnapshotConnections(BeforeData, AfterData));
		if (!bPostconditionVerified)
		{
			const FMaterialRestoreRollbackStatus Rollback =
				RollBackOwnedConnections();
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Material graph restore postcondition failed; expectedAfter=%s, actualAfter=%s, beforeConnections=%d, snapshotConnections=%d, actualConnections=%d, expectedConnections=%s, actualConnectionKeys=%s, semanticRollbackVerified=%s, dirtyRestored=%s, diskRollbackRequired=false."),
					*ExpectedAfterDigest,
					*AfterDigest,
					BeforeData.Connections.Num(),
					SnapshotData->Connections.Num(),
					AfterData.Connections.Num(),
					*SummarizeMaterialConnections(ExpectedAfterData),
					*SummarizeMaterialConnections(AfterData),
					Rollback.bSemanticRollbackVerified ? TEXT("true") : TEXT("false"),
					Rollback.bDirtyRestored ? TEXT("true") : TEXT("false")),
				Rollback.IsVerified()
					? TEXT("restore_postcondition_failed")
					: TEXT("restore_rollback_failed"),
				500);
		}

		TSharedRef<FJsonObject> Result = MakeResult();
		Result->SetNumberField(
			TEXT("appliedConnectionCount"), AppliedIndexes.Num());
		Result->SetStringField(TEXT("afterStateDigest"), AfterDigest);
		Result->SetBoolField(TEXT("postconditionChecked"), true);
		Result->SetBoolField(TEXT("postconditionVerified"), true);
		Result->SetBoolField(TEXT("rollbackAttempted"), false);
		Result->SetBoolField(TEXT("rollbackVerified"), false);
		Result->SetBoolField(
			TEXT("compileRequested"),
			!UEAIIntegration::Workflow::ShouldDeferCompile(Params));
		Result->SetBoolField(TEXT("saved"), false);
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// validate_material
// ============================================================
class FTool_ValidateMaterial : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.material.validate");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString MaterialName = Params->GetStringField(TEXT("material"));
		if (MaterialName.IsEmpty()) return FMCPToolResult::Error(TEXT("Missing: material"));

		FString LoadError;
		UMaterial* Material = LoadMaterialByName(MaterialName, LoadError);
		if (!Material) return FMCPToolResult::Error(LoadError);

#if WITH_DEV_AUTOMATION_TESTS
		UEAIIntegration::Workflow::NotifyMaterialCompileFinalizerForTests();
#endif
		const bool bWorkflowFinalizer =
			UEAIIntegration::Workflow::IsApprovedWorkflowExecution(Params);
		TArray<UMaterialFunctionInterface*> Dependencies;
		if (!UEAIIntegration::MaterialEditing::CollectFunctionDependencies(Material, Dependencies, LoadError))
			return FMCPToolResult::Error(LoadError, TEXT("invalid_function_dependencies"), 400);
		if (bWorkflowFinalizer && Material->MaterialGraph)
		{
			// Deferred connections only edit graph pins. Persist the expression
			// inputs before compilation/save, including when no editor is open.
			Material->MaterialGraph->LinkMaterialExpressionsFromGraph();
		}
		const bool bShaderCacheRefreshed = UEAIIntegration::MaterialEditing::PrepareMaterialSourceValidation(Material);
		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		if (bWorkflowFinalizer && Material->MaterialGraph)
		{
			Material->MaterialGraph->NotifyGraphChanged();
			FMaterialEditorUtilities::UpdateMaterialAfterGraphChange(Material->MaterialGraph);
		}

		bool bWait = bWorkflowFinalizer;
		if (!bWorkflowFinalizer) Params->TryGetBoolField(TEXT("waitForCompilation"), bWait);
		auto Result = UEAIIntegration::MaterialEditing::CompleteMaterialValidation(Material, bWait);
		Result->SetBoolField(TEXT("shaderFileCacheRefreshed"), bShaderCacheRefreshed);
		TArray<TSharedPtr<FJsonValue>> ErrorArray;
		for (const auto& Entry : Result->GetArrayField(TEXT("diagnostics"))) ErrorArray.Add(MakeShared<FJsonValueString>(Entry->AsObject()->GetStringField(TEXT("message"))));

		auto Expressions = Material->GetExpressions();
		int32 ConnectionCount = 0;
		if (Material->MaterialGraph)
		{
			for (UEdGraphNode* Node : Material->MaterialGraph->Nodes)
			{
				if (!Node) continue;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output) ConnectionCount += Pin->LinkedTo.Num();
				}
			}
		}

		// Custom HLSL must have an actual compiler verdict before a Workflow can
		// commit/save it. NullRHI or absent resources are not successful validation.
		if (bWorkflowFinalizer && !Result->GetBoolField(TEXT("shaderValidationPerformed")))
		{
			bool bHasCustom = false;
			for (UMaterialExpression* Expression : Expressions) if (Expression && Expression->IsA<UMaterialExpressionCustom>()) bHasCustom = true;
			for (auto* Function : Dependencies) for (UMaterialExpression* Expression : Function->GetExpressions())
				if (Expression && Expression->IsA<UMaterialExpressionCustom>()) bHasCustom = true;
			if (bHasCustom)
			{
				Result->SetBoolField(TEXT("valid"), false);
				ErrorArray.Add(MakeShared<FJsonValueString>(TEXT("Custom HLSL requires completed shader validation in a rendering Editor.")));
			}
		}
		Result->SetStringField(TEXT("material"), Material->GetName());
		Result->SetStringField(TEXT("materialPath"), Material->GetPathName());
		Result->SetNumberField(TEXT("expressionCount"), Expressions.Num());
		Result->SetNumberField(TEXT("connectionCount"), ConnectionCount);
		Result->SetArrayField(TEXT("errors"), ErrorArray);
		Result->SetNumberField(TEXT("errorCount"), ErrorArray.Num());
		return FMCPToolResult::Ok(Result);
	}
};

// ============================================================
// Registration
// ============================================================
namespace UEAIIntegrationTools
{
	void RegisterMaterialMutationTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_CreateMaterial>());
		Registry.Register(MakeShared<FTool_SetMaterialProperty>());
		Registry.Register(MakeShared<FTool_AddMaterialExpression>());
		Registry.Register(MakeShared<FTool_DeleteMaterialExpression>());
		Registry.Register(MakeShared<FTool_ConnectMaterialPins>());
		Registry.Register(MakeShared<FTool_DisconnectMaterialPin>());
		Registry.Register(MakeShared<FTool_SetExpressionValue>());
		Registry.Register(MakeShared<FTool_MoveMaterialExpression>());
		Registry.Register(MakeShared<FTool_CreateMaterialInstance>());
		Registry.Register(MakeShared<FTool_SetMaterialInstanceParameter>());
		Registry.Register(MakeShared<FTool_SnapshotMaterialGraph>());
		Registry.Register(MakeShared<FTool_DiffMaterialGraph>());
		Registry.Register(MakeShared<FTool_RestoreMaterialGraph>());
		Registry.Register(MakeShared<FTool_ValidateMaterial>());
	}
}
