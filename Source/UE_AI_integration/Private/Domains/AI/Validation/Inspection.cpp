#include "Domains/AI/AIInspection.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "BehaviorTree/BehaviorTreeTypes.h"
#include "UObject/UnrealType.h"

namespace UEAIInspectionValidationPrivate
{
using namespace UEAIIntegration::AIInspection;

FMCPToolResult AIValidationAssetNotFound(const FString& Path)
{
	return FMCPToolResult::Error(
		FString::Printf(TEXT("AI asset '%s' was not found."), *Path),
		TEXT("ai_asset_not_found"),
		404);
}

TSharedRef<FJsonObject> MakeDiff(
	const FString& Kind,
	const FString& LeftPath,
	const TSharedRef<FJsonObject>& Left,
	const FString& RightPath,
	const TSharedRef<FJsonObject>& Right)
{
	const FString LeftHash = Left->GetStringField(TEXT("snapshotHash"));
	const FString RightHash = Right->GetStringField(TEXT("snapshotHash"));
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.snapshot-diff.v1"));
	Result->SetStringField(TEXT("kind"), Kind);
	Result->SetStringField(TEXT("left"), LeftPath);
	Result->SetStringField(TEXT("right"), RightPath);
	Result->SetStringField(TEXT("leftHash"), LeftHash);
	Result->SetStringField(TEXT("rightHash"), RightHash);
	Result->SetBoolField(TEXT("identical"), LeftHash == RightHash);
	TArray<TSharedPtr<FJsonValue>> ChangedFields;
	TArray<FString> Keys;
	Left->Values.GetKeys(Keys);
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Right->Values)
	{
		Keys.AddUnique(Pair.Key);
	}
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		if (Key == TEXT("snapshotHash"))
		{
			continue;
		}
		const TSharedPtr<FJsonValue> LeftValue = Left->Values.FindRef(Key);
		const TSharedPtr<FJsonValue> RightValue = Right->Values.FindRef(Key);
		TSharedRef<FJsonObject> LeftWrapper = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> RightWrapper = MakeShared<FJsonObject>();
		LeftWrapper->SetField(
			TEXT("value"),
			LeftValue.IsValid() ? LeftValue : MakeShared<FJsonValueNull>());
		RightWrapper->SetField(
			TEXT("value"),
			RightValue.IsValid() ? RightValue : MakeShared<FJsonValueNull>());
		if (UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(LeftWrapper))
			!= UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
				MakeShared<FJsonValueObject>(RightWrapper)))
		{
			ChangedFields.Add(MakeShared<FJsonValueString>(Key));
		}
	}
	Result->SetArrayField(TEXT("changedFields"), ChangedFields);
	Result->SetNumberField(TEXT("changedFieldCount"), ChangedFields.Num());
	return Result;
}

void ValidateBlackboardSelectors(
	UObject* Node,
	const UBlackboardData* Blackboard,
	const FString& TreePath,
	TArray<TSharedPtr<FJsonValue>>& Findings)
{
	if (!Node || !Blackboard)
	{
		return;
	}
	for (TFieldIterator<FStructProperty> It(Node->GetClass()); It; ++It)
	{
		FStructProperty* Property = *It;
		if (!Property
			|| Property->Struct != FBlackboardKeySelector::StaticStruct())
		{
			continue;
		}
		const FBlackboardKeySelector* Selector =
			Property->ContainerPtrToValuePtr<FBlackboardKeySelector>(Node);
		if (!Selector || Selector->SelectedKeyName.IsNone())
		{
			continue;
		}
		const FBlackboard::FKey KeyId =
			Blackboard->GetKeyID(Selector->SelectedKeyName);
		if (KeyId == FBlackboard::InvalidKey)
		{
			AddFinding(
				Findings,
				TEXT("ai.behavior_tree.missing_blackboard_key"),
				TEXT("error"),
				FString::Printf(
					TEXT("Node references missing Blackboard key '%s'."),
					*Selector->SelectedKeyName.ToString()),
				TreePath,
				TEXT("BehaviorTree"),
				FString::Printf(
					TEXT("%s.%s"),
					*Node->GetPathName(),
					*Property->GetName()));
			continue;
		}
		const TSubclassOf<UBlackboardKeyType> ActualType =
			Blackboard->GetKeyType(KeyId);
		if (Selector->SelectedKeyType
			&& ActualType
			&& !ActualType->IsChildOf(Selector->SelectedKeyType))
		{
			AddFinding(
				Findings,
				TEXT("ai.behavior_tree.blackboard_keyType_mismatch"),
				TEXT("error"),
				FString::Printf(
					TEXT("Blackboard key '%s' type is incompatible with node selector '%s'."),
					*Selector->SelectedKeyName.ToString(),
					*Property->GetName()),
				TreePath,
				TEXT("BehaviorTree"),
				Node->GetPathName());
		}
	}
}

class FTool_BehaviorTreeValidate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.behavior_tree.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBehaviorTree* Tree = Cast<UBehaviorTree>(LoadAsset(Path));
		if (!Tree)
		{
			return AIValidationAssetNotFound(Path);
		}
		TArray<TSharedPtr<FJsonValue>> Findings;
		if (!Tree->RootNode)
		{
			AddFinding(
				Findings,
				TEXT("ai.behavior_tree.missing_root"),
				TEXT("error"),
				TEXT("Behavior Tree has no root node."),
				Tree->GetPathName());
		}
		if (!Tree->BlackboardAsset)
		{
			AddFinding(
				Findings,
				TEXT("ai.behavior_tree.missing_blackboard"),
				TEXT("warning"),
				TEXT("Behavior Tree has no Blackboard asset."),
				Tree->GetPathName());
		}

		if (Tree->RootNode)
		{
			TQueue<UBTCompositeNode*> Queue;
			TSet<const UBTCompositeNode*> Visited;
			Queue.Enqueue(Tree->RootNode);
			while (!Queue.IsEmpty())
			{
				UBTCompositeNode* Composite = nullptr;
				Queue.Dequeue(Composite);
				if (!Composite)
				{
					continue;
				}
				if (Visited.Contains(Composite))
				{
					AddFinding(
						Findings,
						TEXT("ai.behavior_tree.composite_cycle"),
						TEXT("error"),
						TEXT("Composite graph contains a cycle."),
						Tree->GetPathName(),
						TEXT("BehaviorTree"),
						Composite->GetPathName());
					continue;
				}
				Visited.Add(Composite);
				ValidateBlackboardSelectors(
					Composite,
					Tree->BlackboardAsset,
					Tree->GetPathName(),
					Findings);
				for (UBTService* Service : Composite->Services)
				{
					if (!Service)
					{
						AddFinding(
							Findings,
							TEXT("ai.behavior_tree.missing_serviceClass"),
							TEXT("error"),
							TEXT("Composite contains a null Service reference."),
							Tree->GetPathName(),
							TEXT("BehaviorTree"),
							Composite->GetPathName());
					}
					else
					{
						ValidateBlackboardSelectors(
							Service,
							Tree->BlackboardAsset,
							Tree->GetPathName(),
							Findings);
					}
				}
				for (int32 ChildIndex = 0;
					ChildIndex < Composite->Children.Num();
					++ChildIndex)
				{
					const FBTCompositeChild& Child =
						Composite->Children[ChildIndex];
					const FString Location = FString::Printf(
						TEXT("%s#child/%d"),
						*Composite->GetPathName(),
						ChildIndex);
					if (!Child.ChildComposite && !Child.ChildTask)
					{
						AddFinding(
							Findings,
							TEXT("ai.behavior_tree.empty_child"),
							TEXT("error"),
							TEXT("Composite child has neither a Composite nor Task node."),
							Tree->GetPathName(),
							TEXT("BehaviorTree"),
							Location);
					}
					if (Child.ChildComposite)
					{
						Queue.Enqueue(Child.ChildComposite);
					}
					if (Child.ChildTask)
					{
						ValidateBlackboardSelectors(
							Child.ChildTask,
							Tree->BlackboardAsset,
							Tree->GetPathName(),
							Findings);
					}
					for (UBTDecorator* Decorator : Child.Decorators)
					{
						if (!Decorator)
						{
							AddFinding(
								Findings,
								TEXT("ai.behavior_tree.missing_decoratorClass"),
								TEXT("error"),
								TEXT("Child contains a null Decorator reference."),
								Tree->GetPathName(),
								TEXT("BehaviorTree"),
								Location);
						}
						else
						{
							ValidateBlackboardSelectors(
								Decorator,
								Tree->BlackboardAsset,
								Tree->GetPathName(),
								Findings);
						}
					}
				}
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), Tree->GetPathName());
		Result->SetBoolField(TEXT("valid"), Findings.IsEmpty());
		Result->SetNumberField(TEXT("findingCount"), Findings.Num());
		Result->SetArrayField(TEXT("findings"), Findings);
		Result->SetObjectField(TEXT("snapshot"), DescribeBehaviorTree(Tree));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_BehaviorTreeDiff final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.behavior_tree.diff"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString LeftPath;
		FString RightPath;
		Params->TryGetStringField(TEXT("left"), LeftPath);
		Params->TryGetStringField(TEXT("right"), RightPath);
		UBehaviorTree* Left = Cast<UBehaviorTree>(LoadAsset(LeftPath));
		UBehaviorTree* Right = Cast<UBehaviorTree>(LoadAsset(RightPath));
		if (!Left)
		{
			return AIValidationAssetNotFound(LeftPath);
		}
		if (!Right)
		{
			return AIValidationAssetNotFound(RightPath);
		}
		return FMCPToolResult::Ok(MakeDiff(
			TEXT("behaviorTree"),
			LeftPath,
			DescribeBehaviorTree(Left),
			RightPath,
			DescribeBehaviorTree(Right)));
	}
};

class FTool_BlackboardValidate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.blackboard.validate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		UBlackboardData* Blackboard =
			Cast<UBlackboardData>(LoadAsset(Path));
		if (!Blackboard)
		{
			return AIValidationAssetNotFound(Path);
		}
		TArray<TSharedPtr<FJsonValue>> Findings;
		TSet<FName> Names;
		for (const FBlackboardEntry& Entry : Blackboard->Keys)
		{
			if (Entry.EntryName.IsNone())
			{
				AddFinding(
					Findings,
					TEXT("ai.blackboard.empty_keyName"),
					TEXT("error"),
					TEXT("Blackboard contains an unnamed key."),
					Blackboard->GetPathName());
			}
			if (Names.Contains(Entry.EntryName))
			{
				AddFinding(
					Findings,
					TEXT("ai.blackboard.duplicate_key"),
					TEXT("error"),
					FString::Printf(
						TEXT("Blackboard key '%s' is duplicated."),
						*Entry.EntryName.ToString()),
					Blackboard->GetPathName());
			}
			Names.Add(Entry.EntryName);
			if (!Entry.KeyType)
			{
				AddFinding(
					Findings,
					TEXT("ai.blackboard.missing_keyType"),
					TEXT("error"),
					FString::Printf(
						TEXT("Blackboard key '%s' has no type."),
						*Entry.EntryName.ToString()),
					Blackboard->GetPathName());
			}
		}
		TSet<const UBlackboardData*> Parents;
		const UBlackboardData* Parent = Blackboard->Parent;
		while (Parent)
		{
			if (Parent == Blackboard || Parents.Contains(Parent))
			{
				AddFinding(
					Findings,
					TEXT("ai.blackboard.parent_cycle"),
					TEXT("error"),
					TEXT("Blackboard parent chain contains a cycle."),
					Blackboard->GetPathName());
				break;
			}
			Parents.Add(Parent);
			Parent = Parent->Parent;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("asset"), Blackboard->GetPathName());
		Result->SetBoolField(TEXT("valid"), Findings.IsEmpty());
		Result->SetNumberField(TEXT("findingCount"), Findings.Num());
		Result->SetArrayField(TEXT("findings"), Findings);
		Result->SetObjectField(TEXT("snapshot"), DescribeBlackboard(Blackboard));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_BlackboardDiff final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("ai.blackboard.diff"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString LeftPath;
		FString RightPath;
		Params->TryGetStringField(TEXT("left"), LeftPath);
		Params->TryGetStringField(TEXT("right"), RightPath);
		UBlackboardData* Left = Cast<UBlackboardData>(LoadAsset(LeftPath));
		UBlackboardData* Right = Cast<UBlackboardData>(LoadAsset(RightPath));
		if (!Left)
		{
			return AIValidationAssetNotFound(LeftPath);
		}
		if (!Right)
		{
			return AIValidationAssetNotFound(RightPath);
		}
		return FMCPToolResult::Ok(MakeDiff(
			TEXT("blackboard"),
			LeftPath,
			DescribeBlackboard(Left),
			RightPath,
			DescribeBlackboard(Right)));
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterAIInspectionValidationTools(FMCPToolRegistry& Registry)
{
	using namespace UEAIInspectionValidationPrivate;
	Registry.Register(MakeShared<FTool_BehaviorTreeValidate>());
	Registry.Register(MakeShared<FTool_BehaviorTreeDiff>());
	Registry.Register(MakeShared<FTool_BlackboardValidate>());
	Registry.Register(MakeShared<FTool_BlackboardDiff>());
}
}
