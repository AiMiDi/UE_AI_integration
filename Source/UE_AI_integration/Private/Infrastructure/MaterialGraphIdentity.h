#pragma once

#include "Materials/MaterialExpression.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"

namespace MCPMaterialInfrastructure
{
// Asset-local authored-object identity, independent of transient editor GUIDs.
inline FString ExpressionNodeId(const UMaterialExpression* Expression)
{
	return Expression ? TEXT("expr:") + Expression->GetName() : FString();
}

inline bool MatchesMaterialNode(const UEdGraphNode* Node, const FString& Id)
{
	if (!Node) return false;
	if (Node->NodeGuid.ToString() == Id) return true;
	if (Id == TEXT("root")) return Node->IsA<UMaterialGraphNode_Root>();
	const UMaterialGraphNode* ExpressionNode = Cast<UMaterialGraphNode>(Node);
	return ExpressionNode && ExpressionNode->MaterialExpression
		&& ExpressionNodeId(ExpressionNode->MaterialExpression) == Id;
}
}
