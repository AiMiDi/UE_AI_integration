#pragma once

#include "CoreMinimal.h"
#include "Infrastructure/MaterialGraphSnapshot.h"

class UMaterialExpression;

namespace UEAIIntegration::MaterialEditing
{
struct FTarget;

struct FMaterialSharedWriteProof
{
	MaterialQuery::FBoundaryWriteValidation Validation;
	TArray<FString> ConsumerNodeIds;
	int32 ConsumerConnectionCount = 0;
	bool bShared = false;
	bool bTraversedNamedReroute = false;
};

// Read-only fan-out detection includes authored inputs, deferred graph pins,
// transparent reroutes and implicit named-reroute references.
void InspectMaterialExpressionConsumers(
    const FTarget& Target,
    UMaterialExpression* Expression,
    FMaterialSharedWriteProof& Proof);
void InspectMaterialExpressionConsumers(
    UObject* Asset,
    UMaterialExpression* Expression,
    FMaterialSharedWriteProof& Proof);

// Ordinary unshared writes remain compatible. Shared writes require a fresh,
// matching authored boundary that selects the expression and explicit consent.
FMCPToolResult ValidateMaterialExpressionSharedWrite(
    const FTarget& Target,
    UMaterialExpression* Expression,
    const TSharedPtr<FJsonObject>& Params,
    FMaterialSharedWriteProof& Proof);
FMCPToolResult ValidateMaterialExpressionSharedWrite(
    UObject* Asset,
    UMaterialExpression* Expression,
    const TSharedPtr<FJsonObject>& Params,
    FMaterialSharedWriteProof& Proof);
void DescribeMaterialExpressionSharedWrite(
    const FMaterialSharedWriteProof& Proof,
    const TSharedRef<FJsonObject>& Result);
}
