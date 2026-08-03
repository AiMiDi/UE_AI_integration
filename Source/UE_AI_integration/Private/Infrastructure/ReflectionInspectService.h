#pragma once

#include "CoreMinimal.h"
#include "Tools/MCPToolBase.h"

namespace UEAIIntegration::Infrastructure
{
class FReflectionInspectService
{
public:
	bool Handles(const FString& CapabilityId) const;
	FMCPToolResult Execute(
		const FString& CapabilityId,
		const TSharedPtr<FJsonObject>& Params) const;

private:
	FMCPToolResult SearchTypes(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetType(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult GetMember(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult DescribeObject(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult CreateSnapshot(const TSharedPtr<FJsonObject>& Params) const;
	FMCPToolResult InspectPython(const TSharedPtr<FJsonObject>& Params) const;
};
}
