#pragma once

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegration::AIInspection
{
inline UObject* LoadAsset(const FString& Path)
{
	return StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
}

inline void AddFinding(
	TArray<TSharedPtr<FJsonValue>>& Findings,
	const FString& RuleId,
	const FString& Severity,
	const FString& Message,
	const FString& AssetPath,
	const FString& GraphName = FString(),
	const FString& NodeGuid = FString())
{
	const FString ContractSeverity =
		Severity == TEXT("error")
			? TEXT("high")
			: (Severity == TEXT("warning") ? TEXT("medium") : Severity);
	Findings.Add(MakeShared<FJsonValueObject>(
		Infrastructure::MakeFinding(
			RuleId,
			ContractSeverity,
			0.95,
			AssetPath,
			GraphName,
			NodeGuid,
			Message)));
}

inline TSharedRef<FJsonObject> DescribeBlackboard(UBlackboardData* Blackboard)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.blackboard.snapshot.v1"));
	Result->SetStringField(TEXT("name"), Blackboard->GetName());
	Result->SetStringField(TEXT("path"), Blackboard->GetPathName());
	Result->SetStringField(
		TEXT("parent"),
		Blackboard->Parent ? Blackboard->Parent->GetPathName() : TEXT(""));
	TArray<TSharedPtr<FJsonValue>> Keys;
	for (const FBlackboardEntry& Entry : Blackboard->Keys)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Entry.EntryName.ToString());
		Item->SetStringField(
			TEXT("type"),
			Entry.KeyType ? Entry.KeyType->GetClass()->GetPathName() : TEXT(""));
		Keys.Add(MakeShared<FJsonValueObject>(Item));
	}
	Keys.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsObject()->GetStringField(TEXT("name"))
				< B->AsObject()->GetStringField(TEXT("name"));
		});
	Result->SetArrayField(TEXT("keys"), Keys);
	Result->SetNumberField(TEXT("keyCount"), Keys.Num());
	const FString SnapshotHash = Infrastructure::DigestJson(Result);
	if (!SnapshotHash.IsEmpty())
	{
		Result->SetStringField(TEXT("snapshotHash"), SnapshotHash);
	}
	return Result;
}

inline TSharedRef<FJsonObject> DescribeBehaviorTree(UBehaviorTree* Tree)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.behavior-tree.snapshot.v1"));
	Result->SetStringField(TEXT("name"), Tree->GetName());
	Result->SetStringField(TEXT("path"), Tree->GetPathName());
	Result->SetStringField(
		TEXT("blackboard"),
		Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : TEXT(""));
	Result->SetBoolField(TEXT("hasRoot"), Tree->RootNode != nullptr);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	if (Tree->RootNode)
	{
		TQueue<TPair<UBTCompositeNode*, FString>> Queue;
		Queue.Enqueue(
			TPair<UBTCompositeNode*, FString>(Tree->RootNode, TEXT("0")));
		while (!Queue.IsEmpty())
		{
			TPair<UBTCompositeNode*, FString> Current;
			Queue.Dequeue(Current);
			UBTCompositeNode* Composite = Current.Key;
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("index"), Current.Value);
			Item->SetStringField(TEXT("kind"), TEXT("composite"));
			Item->SetStringField(TEXT("class"), Composite->GetClass()->GetPathName());
			Item->SetNumberField(TEXT("serviceCount"), Composite->Services.Num());
			Item->SetNumberField(TEXT("childCount"), Composite->Children.Num());
			Nodes.Add(MakeShared<FJsonValueObject>(Item));

			for (int32 ChildIndex = 0; ChildIndex < Composite->Children.Num(); ++ChildIndex)
			{
				const FBTCompositeChild& Child = Composite->Children[ChildIndex];
				const FString Index = FString::Printf(
					TEXT("%s.%d"),
					*Current.Value,
					ChildIndex);
				TSharedRef<FJsonObject> ChildItem = MakeShared<FJsonObject>();
				ChildItem->SetStringField(TEXT("index"), Index);
				ChildItem->SetNumberField(
					TEXT("decoratorCount"),
					Child.Decorators.Num());
				if (Child.ChildComposite)
				{
					ChildItem->SetStringField(TEXT("kind"), TEXT("compositeEdge"));
					ChildItem->SetStringField(
						TEXT("class"),
						Child.ChildComposite->GetClass()->GetPathName());
					Queue.Enqueue(
						TPair<UBTCompositeNode*, FString>(
							Child.ChildComposite,
							Index));
				}
				else
				{
					ChildItem->SetStringField(TEXT("kind"), TEXT("task"));
					ChildItem->SetStringField(
						TEXT("class"),
						Child.ChildTask
							? Child.ChildTask->GetClass()->GetPathName()
							: TEXT(""));
				}
				Nodes.Add(MakeShared<FJsonValueObject>(ChildItem));
			}
		}
	}
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetNumberField(TEXT("nodeRecordCount"), Nodes.Num());
	const FString SnapshotHash = Infrastructure::DigestJson(Result);
	if (!SnapshotHash.IsEmpty())
	{
		Result->SetStringField(TEXT("snapshotHash"), SnapshotHash);
	}
	return Result;
}

inline void CollectReferencedClasses(
	UBTCompositeNode* Root,
	TSet<FString>& OutClasses)
{
	if (!Root)
	{
		return;
	}
	TQueue<UBTCompositeNode*> Queue;
	TSet<const UBTCompositeNode*> Visited;
	Queue.Enqueue(Root);
	while (!Queue.IsEmpty())
	{
		UBTCompositeNode* Composite = nullptr;
		Queue.Dequeue(Composite);
		if (!Composite || Visited.Contains(Composite))
		{
			continue;
		}
		Visited.Add(Composite);
		OutClasses.Add(Composite->GetClass()->GetPathName());
		for (UBTService* Service : Composite->Services)
		{
			if (Service)
			{
				OutClasses.Add(Service->GetClass()->GetPathName());
			}
		}
		for (const FBTCompositeChild& Child : Composite->Children)
		{
			if (Child.ChildComposite)
			{
				Queue.Enqueue(Child.ChildComposite);
			}
			if (Child.ChildTask)
			{
				OutClasses.Add(Child.ChildTask->GetClass()->GetPathName());
			}
			for (UBTDecorator* Decorator : Child.Decorators)
			{
				if (Decorator)
				{
					OutClasses.Add(Decorator->GetClass()->GetPathName());
				}
			}
		}
	}
}
}
