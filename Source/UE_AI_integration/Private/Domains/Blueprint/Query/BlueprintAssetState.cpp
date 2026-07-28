// Read-only Blueprint package lifecycle state.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"

#include "Engine/Blueprint.h"

namespace
{
FString BlueprintAssetStateStatusName(const EBlueprintStatus Status)
{
	switch (Status)
	{
	case BS_Dirty:
		return TEXT("Dirty");
	case BS_Error:
		return TEXT("Error");
	case BS_UpToDate:
		return TEXT("UpToDate");
	case BS_BeingCreated:
		return TEXT("BeingCreated");
	case BS_UpToDateWithWarnings:
		return TEXT("UpToDateWithWarnings");
	case BS_Unknown:
	default:
		return TEXT("Unknown");
	}
}

class FTool_GetBlueprintAssetDirtyState final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.asset.dirty.get");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		FString BlueprintInput;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(
				TEXT("blueprint"),
				BlueprintInput)
			|| BlueprintInput.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("Missing required field: blueprint"),
				TEXT("invalid_params"),
				422);
		}

		FString LoadError;
		UBlueprint* Blueprint =
			MCPHelpers::LoadBlueprintByName(
				BlueprintInput,
				LoadError);
		if (!Blueprint)
		{
			return FMCPToolResult::Error(
				LoadError,
				TEXT("target_not_found"),
				404);
		}
		UPackage* Package = Blueprint->GetOutermost();
		if (!Package)
		{
			return FMCPToolResult::Error(
				TEXT("Blueprint has no package."),
				TEXT("asset_package_missing"),
				500);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("blueprint"),
			Blueprint->GetPathName());
		Result->SetStringField(TEXT("package"), Package->GetName());
		Result->SetBoolField(TEXT("dirty"), Package->IsDirty());
		Result->SetStringField(
			TEXT("status"),
			BlueprintAssetStateStatusName(Blueprint->Status));
		return FMCPToolResult::Ok(Result);
	}
};
} // namespace

namespace UEAIIntegrationTools
{
void RegisterBlueprintAssetStateTools(FMCPToolRegistry& Registry)
{
	Registry.Register(
		MakeShared<FTool_GetBlueprintAssetDirtyState>());
}
} // namespace UEAIIntegrationTools
