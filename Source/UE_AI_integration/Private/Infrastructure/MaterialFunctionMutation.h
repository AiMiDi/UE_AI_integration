#pragma once

#include "Tools/MCPToolBase.h"
#include "Materials/MaterialFunction.h"
#include "Infrastructure/MaterialGraphIdentity.h"

namespace MCPMaterialInfrastructure
{
inline UMaterialExpression* FindFunctionExpression(UMaterialFunction* Function, const FString& NodeId)
{
	for (UMaterialExpression* Expression : Function->GetExpressions())
	{
		if (Expression && (ExpressionNodeId(Expression) == NodeId || MatchesMaterialNode(Expression->GraphNode, NodeId))) return Expression;
	}
	return nullptr;
}

FMCPToolResult MutateMaterialFunction(const FString& Capability, const TSharedPtr<FJsonObject>& Params);
}
