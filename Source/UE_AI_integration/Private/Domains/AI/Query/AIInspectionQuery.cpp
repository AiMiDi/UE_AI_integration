#include "Domains/AI/AIInspection.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

namespace UEAIInspectionQueryPrivate
{
using namespace UEAIIntegration::AIInspection;

FMCPToolResult AIAssetNotFound(const FString& Path)
{
	return FMCPToolResult::Error(
		FString::Printf(TEXT("AI asset '%s' was not found."), *Path),
		TEXT("ai_asset_not_found"),
		404);
}

class FTool_AIAssetList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.asset.list"); }
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
		if (Type == TEXT("behaviorTree") || Type == TEXT("all"))
		{
			Registry.GetAssetsByClass(
				UBehaviorTree::StaticClass()->GetClassPathName(),
				Assets,
				true);
		}
		if (Type == TEXT("blackboard") || Type == TEXT("all"))
		{
			TArray<FAssetData> Blackboards;
			Registry.GetAssetsByClass(
				UBlackboardData::StaticClass()->GetClassPathName(),
				Blackboards,
				true);
			Assets.Append(Blackboards);
		}
		if (Type != TEXT("all")
			&& Type != TEXT("behaviorTree")
			&& Type != TEXT("blackboard"))
		{
			return FMCPToolResult::Error(
				TEXT("type must be all, behaviorTree, or blackboard."),
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

class FTool_BehaviorTreeGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.behavior_tree.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBehaviorTree* Tree = Cast<UBehaviorTree>(LoadAsset(Path));
		return Tree
			? FMCPToolResult::Ok(DescribeBehaviorTree(Tree))
			: AIAssetNotFound(Path);
	}
};

class FTool_BehaviorTreeReferences final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.behavior_tree.references"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBehaviorTree* Tree = Cast<UBehaviorTree>(LoadAsset(Path));
		if (!Tree)
		{
			return AIAssetNotFound(Path);
		}
		TSet<FString> ReferencedClasses;
		CollectReferencedClasses(Tree->RootNode, ReferencedClasses);
		TArray<FString> SortedClasses = ReferencedClasses.Array();
		SortedClasses.Sort();
		TArray<TSharedPtr<FJsonValue>> Classes;
		for (const FString& ClassPath : SortedClasses)
		{
			Classes.Add(MakeShared<FJsonValueString>(ClassPath));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), Tree->GetPathName());
		Result->SetStringField(
			TEXT("blackboard"),
			Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : TEXT(""));
		Result->SetArrayField(TEXT("nodeClasses"), Classes);
		Result->SetNumberField(TEXT("nodeClassCount"), Classes.Num());
		Result->SetStringField(
			TEXT("evidenceBoundary"),
			TEXT("Dynamic Blueprint class resolution and runtime-selected assets are not statically resolved."));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_BlackboardGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.blackboard.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBlackboardData* Blackboard = Cast<UBlackboardData>(LoadAsset(Path));
		return Blackboard
			? FMCPToolResult::Ok(DescribeBlackboard(Blackboard))
			: AIAssetNotFound(Path);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterAIInspectionQueryTools(FMCPToolRegistry& Registry)
{
	using namespace UEAIInspectionQueryPrivate;
	Registry.Register(MakeShared<FTool_AIAssetList>());
	Registry.Register(MakeShared<FTool_BehaviorTreeGet>());
	Registry.Register(MakeShared<FTool_BehaviorTreeReferences>());
	Registry.Register(MakeShared<FTool_BlackboardGet>());
}
}
