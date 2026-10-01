#include "Infrastructure/MaterialSharedWriteProtection.h"
#include "Infrastructure/MaterialEditingTarget.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "EdGraph/EdGraphPin.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionNamedReroute.h"

namespace UEAIIntegration::MaterialEditing
{
using namespace MCPMaterialInfrastructure;

// Read both authored inputs and graph pins: a deferred editor edit may not yet
// have copied its links into FExpressionInput. Transparent reroutes must not
// hide multiple consumers of the value being changed.
void InspectMaterialExpressionConsumers(
	const FTarget& Target,
	UMaterialExpression* Expression,
	FMaterialSharedWriteProof& Proof)
{
	Proof = FMaterialSharedWriteProof();
	TSet<UMaterialExpression*> Visited;
	TArray<UMaterialExpression*> Pending{Expression};
	// Authored inputs and graph pins are two representations of the same edge.
	// Merge them by consumer/input identity so synchronized links count once,
	// while mixed authored and deferred links to different inputs count separately.
	TSet<FString> ConsumerConnections;
	TSet<FString> ConsumerIds;
	auto ExpressionInputKey = [](UMaterialExpression* Consumer, int32 InputIndex)
	{
		return FString::Printf(TEXT("%s:input:%d"), *ExpressionNodeId(Consumer), InputIndex);
	};
	auto RootInputKey = [](EMaterialProperty Property)
	{
		return FString::Printf(TEXT("root:property:%d"), static_cast<int32>(Property));
	};
	auto UnmappedGraphInputKey = [](const UEdGraphPin* Pin)
	{
		// An unrecognized native pin cannot prove equivalence to an authored edge.
		// Preserve it as a separate connection rather than undercounting fan-out.
		return FString::Printf(TEXT("graph:pin:%s"), *Pin->PinId.ToString(EGuidFormats::Digits));
	};
	auto VisitConsumer = [&](UMaterialExpression* Consumer)
	{
		if (Consumer->IsA<UMaterialExpressionRerouteBase>())
		{
			Pending.Add(Consumer);
			Proof.bTraversedNamedReroute |= Consumer->IsA<UMaterialExpressionNamedRerouteBase>();
			return true;
		}
		ConsumerIds.Add(ExpressionNodeId(Consumer));
		return false;
	};
	while (!Pending.IsEmpty())
	{
		UMaterialExpression* Source = Pending.Pop(EAllowShrinking::No);
		if (!Source || Visited.Contains(Source))
		{
			continue;
		}
		Visited.Add(Source);
		for (UMaterialExpression* Consumer : Target.Expressions)
		{
			if (!Consumer || Consumer == Source)
			{
				continue;
			}
			const TArrayView<FExpressionInput*> Inputs = Consumer->GetInputsView();
			for (int32 InputIndex = 0; InputIndex < Inputs.Num(); ++InputIndex)
			{
				const FExpressionInput* Input = Inputs[InputIndex];
				if (Input && Input->Expression == Source && !VisitConsumer(Consumer))
				{
					ConsumerConnections.Add(ExpressionInputKey(Consumer, InputIndex));
				}
			}
			const auto* Declaration = Cast<UMaterialExpressionNamedRerouteDeclaration>(Source);
			const auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Consumer);
			if (Declaration && Usage
				&& (Usage->Declaration == Declaration
					|| (!Usage->Declaration && Declaration->VariableGuid.IsValid()
						&& Usage->DeclarationGuid == Declaration->VariableGuid)))
			{
				Pending.Add(Consumer);
				Proof.bTraversedNamedReroute = true;
			}
		}
		if (Target.Material)
		{
			for (int32 Index = 0; Index < MP_MAX; ++Index)
			{
				const FExpressionInput* Input = Target.Material->GetExpressionInputForProperty(
					static_cast<EMaterialProperty>(Index));
				if (Input && Input->Expression == Source)
				{
					ConsumerConnections.Add(RootInputKey(static_cast<EMaterialProperty>(Index)));
					ConsumerIds.Add(TEXT("root"));
				}
			}
		}
		if (const UEdGraphNode* SourceNode = Source->GraphNode.Get())
		{
			for (const UEdGraphPin* Pin : SourceNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory == UMaterialGraphSchema::PC_Exec)
				{
					continue;
				}
				for (const UEdGraphPin* ConsumerPin : Pin->LinkedTo)
				{
					if (!ConsumerPin || ConsumerPin->Direction != EGPD_Input
						|| ConsumerPin->PinType.PinCategory == UMaterialGraphSchema::PC_Exec)
					{
						continue;
					}
					const UEdGraphNode* ConsumerNode = ConsumerPin->GetOwningNode();
					if (!ConsumerNode || ConsumerNode->GetGraph() != SourceNode->GetGraph())
					{
						continue;
					}
					if (ConsumerNode->IsA<UMaterialGraphNode_Root>())
					{
						ConsumerIds.Add(TEXT("root"));
						const auto* Graph = Cast<UMaterialGraph>(ConsumerNode->GetGraph());
						ConsumerConnections.Add(Graph && Graph->MaterialInputs.IsValidIndex(ConsumerPin->SourceIndex)
							? RootInputKey(Graph->MaterialInputs[ConsumerPin->SourceIndex].GetProperty())
							: UnmappedGraphInputKey(ConsumerPin));
					}
					else if (const auto* MaterialNode = Cast<UMaterialGraphNode>(ConsumerNode))
					{
						if (MaterialNode->MaterialExpression && !VisitConsumer(MaterialNode->MaterialExpression))
						{
							const TArrayView<FExpressionInput*> Inputs = MaterialNode->MaterialExpression->GetInputsView();
							ConsumerConnections.Add(Inputs.IsValidIndex(ConsumerPin->SourceIndex)
								? ExpressionInputKey(MaterialNode->MaterialExpression, ConsumerPin->SourceIndex)
								: UnmappedGraphInputKey(ConsumerPin));
						}
					}
				}
			}
		}
	}
	Proof.ConsumerNodeIds = ConsumerIds.Array();
	Proof.ConsumerNodeIds.Sort();
	Proof.ConsumerConnectionCount = ConsumerConnections.Num();
	Proof.bShared = Proof.ConsumerConnectionCount > 1 || Proof.ConsumerNodeIds.Num() > 1;
}

FMCPToolResult ValidateMaterialExpressionSharedWrite(
	const FTarget& Target,
	UMaterialExpression* Expression,
	const TSharedPtr<FJsonObject>& Params,
	FMaterialSharedWriteProof& Proof)
{
	if (!Target.Asset || !Expression || !Target.Expressions.Contains(Expression))
	{
		return FMCPToolResult::Error(
			TEXT("The edited expression is not a member of the resolved material target."),
			TEXT("material_boundary_node_identity_missing"), 409);
	}
	InspectMaterialExpressionConsumers(Target, Expression, Proof);
	const bool bHasBoundaryFields = Params->HasField(TEXT("boundaryId"))
		|| Params->HasField(TEXT("snapshotId")) || Params->HasField(TEXT("expectedProjectionHash"))
		|| Params->HasField(TEXT("confirmSharedNodeImpact"));
	if (!Proof.bShared && !bHasBoundaryFields)
	{
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}
	if (Target.Editor)
	{
		return FMCPToolResult::Error(
			TEXT("Shared specialized preview writes cannot use an authored-asset boundary. Use an approved authored-asset edit or a preview batch with its own protection contract."),
			TEXT("material_boundary_preview_writer_unsupported"), 409);
	}
	if (!bHasBoundaryFields)
	{
		return FMCPToolResult::Error(
			TEXT("This expression has multiple consumers. Read a fresh graph boundary and explicitly confirm shared-node impact before editing it."),
			TEXT("material_boundary_required_for_mutation"), 409);
	}
	const FMCPToolResult Validated = MaterialQuery::ValidateBoundaryWrite(Target.Asset, Params, Proof.Validation);
	if (!Validated.bSuccess)
	{
		return Validated;
	}
	const FString NodeId = ExpressionNodeId(Expression);
	if (!Proof.Validation.Boundary->WritableNodeIds.Contains(NodeId)
		|| !Proof.Validation.FreshSnapshot->ById.Contains(NodeId))
	{
		return FMCPToolResult::Error(
			TEXT("The edited expression is outside the boundary's writable node selection."),
			TEXT("material_boundary_node_outside_selection"), 409);
	}
	if (Proof.bTraversedNamedReroute && !Proof.Validation.SourceSnapshot->bIncludeNamedReroutes)
	{
		return FMCPToolResult::Error(
			TEXT("The edited value reaches named reroutes. Capture the boundary with includeNamedReroutes=true."),
			TEXT("material_boundary_named_reroute_coverage_required"), 409);
	}
	bool bConfirmed = false;
	Params->TryGetBoolField(TEXT("confirmSharedNodeImpact"), bConfirmed);
	if (Proof.bShared && !bConfirmed)
	{
		return FMCPToolResult::Error(
			TEXT("The edited expression has multiple consumers. Explicit confirmSharedNodeImpact=true is required even when the boundary contains all consumers."),
			TEXT("material_boundary_shared_node_confirmation_required"), 409);
	}
	// Boundary external-impact detection can be false when every consumer is
	// selected. Keep the explicit confirmation for the native fan-out proof too.
	Proof.Validation.bSharedNodeImpactConfirmed |= Proof.bShared && bConfirmed;
	return Validated;
}

void DescribeMaterialExpressionSharedWrite(
	const FMaterialSharedWriteProof& Proof,
	const TSharedRef<FJsonObject>& Result)
{
	Result->SetBoolField(TEXT("writeBoundaryVerified"), Proof.Validation.Boundary.IsValid());
	const bool bSharedImpact = Proof.bShared
		|| (Proof.Validation.Boundary && Proof.Validation.Boundary->bRequiresSharedNodeConfirmation);
	Result->SetBoolField(TEXT("sharedNodeImpactDetected"), bSharedImpact);
	Result->SetBoolField(TEXT("sharedNodeImpactConfirmed"), bSharedImpact && Proof.Validation.bSharedNodeImpactConfirmed);
	Result->SetNumberField(TEXT("consumerConnectionCount"), Proof.ConsumerConnectionCount);
	TArray<TSharedPtr<FJsonValue>> Consumers;
	for (const FString& Id : Proof.ConsumerNodeIds)
	{
		Consumers.Add(MakeShared<FJsonValueString>(Id));
	}
	Result->SetArrayField(TEXT("consumerNodeIds"), Consumers);
	if (Proof.Validation.Boundary)
	{
		TArray<TSharedPtr<FJsonValue>> ExternalConsumers;
		for (const FString& Id : Proof.Validation.Boundary->ExternallyConsumedNodeIds)
		{
			ExternalConsumers.Add(MakeShared<FJsonValueString>(Id));
		}
		Result->SetArrayField(TEXT("externallyConsumedNodeIds"), ExternalConsumers);
		Result->SetStringField(TEXT("boundaryId"), Proof.Validation.Boundary->BoundaryId);
		Result->SetStringField(TEXT("snapshotId"), Proof.Validation.SourceSnapshot->Id);
		Result->SetStringField(TEXT("sourceProjectionHash"), Proof.Validation.SourceSnapshot->ProjectionHash);
		Result->SetStringField(TEXT("freshProjectionHash"), Proof.Validation.FreshSnapshot->ProjectionHash);
		Result->SetBoolField(TEXT("freshLiveProjectionVerified"), true);
	}
}


namespace
{
FTarget MakeMaterialSharedWriteTarget(UObject* Asset)
{
    FTarget Target;
    Target.Asset = Asset;
    Target.OriginalAsset = Asset;
    Target.Material = Cast<UMaterial>(Asset);
    Target.Function = Cast<UMaterialFunction>(Asset);
    if (Target.Material)
    {
        for (UMaterialExpression* Expression : Target.Material->GetExpressions())
        {
            Target.Expressions.Add(Expression);
        }
    }
    else if (Target.Function)
    {
        for (UMaterialExpression* Expression : Target.Function->GetExpressions())
        {
            Target.Expressions.Add(Expression);
        }
    }
    return Target;
}
}

void InspectMaterialExpressionConsumers(
    UObject* Asset,
    UMaterialExpression* Expression,
    FMaterialSharedWriteProof& Proof)
{
    const FTarget Target = MakeMaterialSharedWriteTarget(Asset);
    InspectMaterialExpressionConsumers(Target, Expression, Proof);
}

FMCPToolResult ValidateMaterialExpressionSharedWrite(
    UObject* Asset,
    UMaterialExpression* Expression,
    const TSharedPtr<FJsonObject>& Params,
    FMaterialSharedWriteProof& Proof)
{
    const FTarget Target = MakeMaterialSharedWriteTarget(Asset);
    return ValidateMaterialExpressionSharedWrite(Target, Expression, Params, Proof);
}

}
