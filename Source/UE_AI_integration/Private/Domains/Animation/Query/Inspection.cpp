#include "Domains/Animation/AnimationInspection.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace UEAIAnimationInspectionQueryPrivate
{
using namespace UEAIIntegration::AnimationInspection;

FMCPToolResult AssetNotFound(const FString& Path)
{
	return FMCPToolResult::Error(
		FString::Printf(TEXT("Animation asset '%s' was not found."), *Path),
		TEXT("animation_asset_not_found"),
		404);
}

class FTool_AnimationAssetList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.asset.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Type = TEXT("all");
		FString PackagePath;
		double LimitValue = 100.0;
		Params->TryGetStringField(TEXT("type"), Type);
		Params->TryGetStringField(TEXT("packagePath"), PackagePath);
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
		const int32 Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);

		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		if (Type == TEXT("animBlueprint"))
		{
			Registry.GetAssetsByClass(
				UAnimBlueprint::StaticClass()->GetClassPathName(),
				Assets,
				true);
		}
		else if (Type == TEXT("blendSpace"))
		{
			Registry.GetAssetsByClass(
				UBlendSpace::StaticClass()->GetClassPathName(),
				Assets,
				true);
		}
		else if (Type == TEXT("animationAsset"))
		{
			Registry.GetAssetsByClass(
				UAnimationAsset::StaticClass()->GetClassPathName(),
				Assets,
				true);
		}
		else if (Type == TEXT("all"))
		{
			Registry.GetAssetsByClass(
				UAnimationAsset::StaticClass()->GetClassPathName(),
				Assets,
				true);
			TArray<FAssetData> Blueprints;
			Registry.GetAssetsByClass(
				UAnimBlueprint::StaticClass()->GetClassPathName(),
				Blueprints,
				true);
			Assets.Append(Blueprints);
		}
		else
		{
			return FMCPToolResult::Error(
				TEXT("type must be all, animBlueprint, blendSpace, or animationAsset."),
				TEXT("invalid_request"),
				400);
		}

		Assets.Sort(
			[](const FAssetData& A, const FAssetData& B)
			{
				return A.PackageName.LexicalLess(B.PackageName);
			});
		TArray<TSharedPtr<FJsonValue>> Items;
		int32 MatchingCount = 0;
		for (const FAssetData& Asset : Assets)
		{
			if (!PackagePath.IsEmpty()
				&& !Asset.PackagePath.ToString().StartsWith(PackagePath))
			{
				continue;
			}
			++MatchingCount;
			if (Items.Num() >= Limit)
			{
				continue;
			}
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			Item->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			Item->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("assets"), Items);
		Result->SetNumberField(TEXT("total"), MatchingCount);
		Result->SetNumberField(TEXT("returned"), Items.Num());
		Result->SetBoolField(TEXT("truncated"), MatchingCount > Items.Num());
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_AnimationAssetGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.asset.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("asset"), Path) || Path.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("asset is required."),
				TEXT("invalid_request"),
				400);
		}
		UObject* Asset = LoadAsset(Path);
		if (UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Asset))
		{
			return FMCPToolResult::Ok(DescribeAnimBlueprint(Blueprint));
		}
		if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset))
		{
			return FMCPToolResult::Ok(DescribeBlendSpace(BlendSpace));
		}
		if (UAnimationAsset* AnimationAsset = Cast<UAnimationAsset>(Asset))
		{
			return FMCPToolResult::Ok(DescribeAnimationAsset(AnimationAsset));
		}
		return AssetNotFound(Path);
	}
};

class FTool_AnimationBlueprintGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.blueprint.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(LoadAsset(Path));
		return Blueprint
			? FMCPToolResult::Ok(DescribeAnimBlueprint(Blueprint))
			: AssetNotFound(Path);
	}
};

class FTool_AnimationStateMachineGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.state_machine.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		FString StateMachineName;
		Params->TryGetStringField(TEXT("asset"), Path);
		Params->TryGetStringField(TEXT("stateMachine"), StateMachineName);
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(LoadAsset(Path));
		if (!Blueprint)
		{
			return AssetNotFound(Path);
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			UAnimationStateMachineGraph* StateMachine =
				Cast<UAnimationStateMachineGraph>(Graph);
			if (StateMachine
				&& (StateMachineName.IsEmpty()
					|| StateMachine->GetName().Equals(
						StateMachineName,
						ESearchCase::IgnoreCase)))
			{
				return FMCPToolResult::Ok(DescribeStateMachine(StateMachine));
			}
		}
		return FMCPToolResult::Error(
			FString::Printf(
				TEXT("State machine '%s' was not found in '%s'."),
				*StateMachineName,
				*Path),
			TEXT("state_machine_not_found"),
			404);
	}
};

class FTool_AnimationBlendSpaceGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("animation.blend_space.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBlendSpace* BlendSpace = Cast<UBlendSpace>(LoadAsset(Path));
		return BlendSpace
			? FMCPToolResult::Ok(DescribeBlendSpace(BlendSpace))
			: AssetNotFound(Path);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterAnimationInspectionQueryTools(FMCPToolRegistry& Registry)
{
	using namespace UEAIAnimationInspectionQueryPrivate;
	Registry.Register(MakeShared<FTool_AnimationAssetList>());
	Registry.Register(MakeShared<FTool_AnimationAssetGet>());
	Registry.Register(MakeShared<FTool_AnimationBlueprintGet>());
	Registry.Register(MakeShared<FTool_AnimationStateMachineGet>());
	Registry.Register(MakeShared<FTool_AnimationBlendSpaceGet>());
}
}
