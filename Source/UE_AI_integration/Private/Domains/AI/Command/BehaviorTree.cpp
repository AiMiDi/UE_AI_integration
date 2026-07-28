// AI Behavior Tree tools for UE_AI_integration
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Composites/BTComposite_Sequence.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

// ─────────────────────────────────────────────────────────────
// create_behavior_tree
// ─────────────────────────────────────────────────────────────
class FTool_CreateBehaviorTree : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Parameter 'name' is required."));
		}

		FString PackagePath = FString::Printf(TEXT("/Game/AI/BehaviorTrees/%s"), *Name);
		FString PackageName = FPackageName::ObjectPathToPackageName(PackagePath);

		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *PackagePath));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package at '%s'."), *PackageName));
		}

		UBehaviorTree* BT = NewObject<UBehaviorTree>(Package, *Name, RF_Public | RF_Standalone);
		if (!BT)
		{
			return FMCPToolResult::Error(TEXT("Failed to create UBehaviorTree."));
		}

		// Create default root node (Selector)
		UBTComposite_Selector* RootSelector = NewObject<UBTComposite_Selector>(BT);
		BT->RootNode = RootSelector;

		FAssetRegistryModule::AssetCreated(BT);
		BT->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FString FilePath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		UPackage::SavePackage(Package, BT, *FilePath, SaveArgs);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), PackagePath);
		Result->SetStringField(TEXT("name"), Name);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// create_blackboard
// ─────────────────────────────────────────────────────────────
class FTool_CreateBlackboard : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.blackboard.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Parameter 'name' is required."));
		}

		FString PackagePath = FString::Printf(TEXT("/Game/AI/Blackboards/%s"), *Name);
		FString PackageName = FPackageName::ObjectPathToPackageName(PackagePath);

		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *PackagePath));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package at '%s'."), *PackageName));
		}

		UBlackboardData* BB = NewObject<UBlackboardData>(Package, *Name, RF_Public | RF_Standalone);
		if (!BB)
		{
			return FMCPToolResult::Error(TEXT("Failed to create UBlackboardData."));
		}

		FAssetRegistryModule::AssetCreated(BB);
		BB->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FString FilePath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		UPackage::SavePackage(Package, BB, *FilePath, SaveArgs);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), PackagePath);
		Result->SetStringField(TEXT("name"), Name);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_blackboard_key
// ─────────────────────────────────────────────────────────────
class FTool_AddBlackboardKey : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.blackboard.key.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BBPath = Params->GetStringField(TEXT("blackboard"));
		FString KeyName = Params->GetStringField(TEXT("key_name"));
		FString KeyType = Params->GetStringField(TEXT("key_type")).ToLower();

		UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *BBPath);
		if (!BB)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Blackboard not found at '%s'."), *BBPath));
		}

		// Resolve key type class
		TSubclassOf<UBlackboardKeyType> KeyTypeClass = nullptr;
		if (KeyType == TEXT("bool"))			KeyTypeClass = UBlackboardKeyType_Bool::StaticClass();
		else if (KeyType == TEXT("int"))			KeyTypeClass = UBlackboardKeyType_Int::StaticClass();
		else if (KeyType == TEXT("float"))		KeyTypeClass = UBlackboardKeyType_Float::StaticClass();
		else if (KeyType == TEXT("string"))		KeyTypeClass = UBlackboardKeyType_String::StaticClass();
		else if (KeyType == TEXT("vector"))		KeyTypeClass = UBlackboardKeyType_Vector::StaticClass();
		else if (KeyType == TEXT("rotator"))		KeyTypeClass = UBlackboardKeyType_Rotator::StaticClass();
		else if (KeyType == TEXT("object"))		KeyTypeClass = UBlackboardKeyType_Object::StaticClass();
		else if (KeyType == TEXT("name"))		KeyTypeClass = UBlackboardKeyType_Name::StaticClass();
		else if (KeyType == TEXT("class"))		KeyTypeClass = UBlackboardKeyType_Class::StaticClass();
		else if (KeyType == TEXT("enum"))		KeyTypeClass = UBlackboardKeyType_Enum::StaticClass();
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown key type '%s'. Valid: bool, int, float, string, vector, rotator, object, name, class, enum."), *KeyType));
		}

		// Check if key already exists
		for (const FBlackboardEntry& Entry : BB->Keys)
		{
			if (Entry.EntryName == FName(*KeyName))
			{
				return FMCPToolResult::Error(FString::Printf(TEXT("Key '%s' already exists in blackboard."), *KeyName));
			}
		}

		FBlackboardEntry NewEntry;
		NewEntry.EntryName = FName(*KeyName);
		NewEntry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyTypeClass);
		BB->Keys.Add(NewEntry);

		BB->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("blackboard"), BBPath);
		Result->SetStringField(TEXT("key_name"), KeyName);
		Result->SetStringField(TEXT("key_type"), KeyType);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_bt_task
// ─────────────────────────────────────────────────────────────
class FTool_AddBTTask : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.task.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TreePath = Params->GetStringField(TEXT("tree"));
		FString TaskClassName = Params->GetStringField(TEXT("task_class"));
		int32 ParentIndex = Params->HasField(TEXT("parent_index")) ? (int32)Params->GetNumberField(TEXT("parent_index")) : 0;

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *TreePath);
		if (!BT)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Behavior Tree not found at '%s'."), *TreePath));
		}

		// Ensure class name has correct prefix
		FString FullClassName = TaskClassName;
		if (!FullClassName.StartsWith(TEXT("U")))
		{
			FullClassName = TEXT("U") + FullClassName;
		}

		UClass* TaskClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::ExactClass);
		if (!TaskClass)
		{
			// Try without U prefix
			TaskClass = FindFirstObject<UClass>(*TaskClassName, EFindFirstObjectOptions::ExactClass);
		}
		if (!TaskClass || !TaskClass->IsChildOf(UBTTaskNode::StaticClass()))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("BTTask class '%s' not found or is not a UBTTaskNode."), *TaskClassName));
		}

		// Find parent composite node
		UBTCompositeNode* ParentNode = BT->RootNode;
		if (!ParentNode)
		{
			return FMCPToolResult::Error(TEXT("Behavior Tree has no root node."));
		}

		// Navigate to requested parent by index (breadth-first)
		if (ParentIndex > 0)
		{
			TArray<UBTCompositeNode*> Composites;
			TQueue<UBTCompositeNode*> Queue;
			Queue.Enqueue(ParentNode);
			while (!Queue.IsEmpty())
			{
				UBTCompositeNode* Current = nullptr;
				Queue.Dequeue(Current);
				Composites.Add(Current);
				for (int32 i = 0; i < Current->Children.Num(); ++i)
				{
					if (UBTCompositeNode* ChildComposite = Cast<UBTCompositeNode>(Current->Children[i].ChildComposite))
					{
						Queue.Enqueue(ChildComposite);
					}
				}
			}
			if (ParentIndex >= Composites.Num())
			{
				return FMCPToolResult::Error(FString::Printf(TEXT("Parent index %d out of range. Tree has %d composite nodes."), ParentIndex, Composites.Num()));
			}
			ParentNode = Composites[ParentIndex];
		}

		UBTTaskNode* NewTask = NewObject<UBTTaskNode>(BT, TaskClass);
		if (!NewTask)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create task of class '%s'."), *TaskClassName));
		}

		FBTCompositeChild NewChild;
		NewChild.ChildTask = NewTask;
		ParentNode->Children.Add(NewChild);

		BT->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tree"), TreePath);
		Result->SetStringField(TEXT("task_class"), TaskClassName);
		Result->SetNumberField(TEXT("child_index"), ParentNode->Children.Num() - 1);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_bt_decorator
// ─────────────────────────────────────────────────────────────
class FTool_AddBTDecorator : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.decorator.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TreePath = Params->GetStringField(TEXT("tree"));
		FString DecClassName = Params->GetStringField(TEXT("decorator_class"));
		int32 NodeIndex = Params->HasField(TEXT("node_index")) ? (int32)Params->GetNumberField(TEXT("node_index")) : 0;

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *TreePath);
		if (!BT || !BT->RootNode)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Behavior Tree not found or has no root at '%s'."), *TreePath));
		}

		FString FullClassName = DecClassName.StartsWith(TEXT("U")) ? DecClassName : TEXT("U") + DecClassName;
		UClass* DecClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::ExactClass);
		if (!DecClass)
		{
			DecClass = FindFirstObject<UClass>(*DecClassName, EFindFirstObjectOptions::ExactClass);
		}
		if (!DecClass || !DecClass->IsChildOf(UBTDecorator::StaticClass()))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Decorator class '%s' not found or invalid."), *DecClassName));
		}

		// Find composite node by index (breadth-first)
		TArray<UBTCompositeNode*> Composites;
		TQueue<UBTCompositeNode*> Queue;
		Queue.Enqueue(BT->RootNode);
		while (!Queue.IsEmpty())
		{
			UBTCompositeNode* Current = nullptr;
			Queue.Dequeue(Current);
			Composites.Add(Current);
			for (int32 i = 0; i < Current->Children.Num(); ++i)
			{
				if (UBTCompositeNode* Child = Cast<UBTCompositeNode>(Current->Children[i].ChildComposite))
				{
					Queue.Enqueue(Child);
				}
			}
		}

		if (NodeIndex >= Composites.Num())
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Node index %d out of range (%d composites)."), NodeIndex, Composites.Num()));
		}

		UBTCompositeNode* TargetNode = Composites[NodeIndex];
		UBTDecorator* NewDec = NewObject<UBTDecorator>(BT, DecClass);
		if (!NewDec)
		{
			return FMCPToolResult::Error(TEXT("Failed to create decorator."));
		}

		// In UE 5.7, decorators live on FBTCompositeChild (parent-child connections),
		// not directly on UBTCompositeNode. Add to the first child's decorator list.
		if (TargetNode->Children.Num() > 0)
		{
			TargetNode->Children[0].Decorators.Add(NewDec);
		}
		else
		{
			return FMCPToolResult::Error(TEXT("Target composite node has no children to attach a decorator to."));
		}
		BT->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tree"), TreePath);
		Result->SetStringField(TEXT("decorator_class"), DecClassName);
		Result->SetNumberField(TEXT("node_index"), NodeIndex);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_bt_service
// ─────────────────────────────────────────────────────────────
class FTool_AddBTService : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.service.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TreePath = Params->GetStringField(TEXT("tree"));
		FString SvcClassName = Params->GetStringField(TEXT("service_class"));
		int32 NodeIndex = Params->HasField(TEXT("node_index")) ? (int32)Params->GetNumberField(TEXT("node_index")) : 0;

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *TreePath);
		if (!BT || !BT->RootNode)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Behavior Tree not found or has no root at '%s'."), *TreePath));
		}

		FString FullClassName = SvcClassName.StartsWith(TEXT("U")) ? SvcClassName : TEXT("U") + SvcClassName;
		UClass* SvcClass = FindFirstObject<UClass>(*FullClassName, EFindFirstObjectOptions::ExactClass);
		if (!SvcClass)
		{
			SvcClass = FindFirstObject<UClass>(*SvcClassName, EFindFirstObjectOptions::ExactClass);
		}
		if (!SvcClass || !SvcClass->IsChildOf(UBTService::StaticClass()))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Service class '%s' not found or invalid."), *SvcClassName));
		}

		TArray<UBTCompositeNode*> Composites;
		TQueue<UBTCompositeNode*> Queue;
		Queue.Enqueue(BT->RootNode);
		while (!Queue.IsEmpty())
		{
			UBTCompositeNode* Current = nullptr;
			Queue.Dequeue(Current);
			Composites.Add(Current);
			for (int32 i = 0; i < Current->Children.Num(); ++i)
			{
				if (UBTCompositeNode* Child = Cast<UBTCompositeNode>(Current->Children[i].ChildComposite))
				{
					Queue.Enqueue(Child);
				}
			}
		}

		if (NodeIndex >= Composites.Num())
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Node index %d out of range (%d composites)."), NodeIndex, Composites.Num()));
		}

		UBTCompositeNode* TargetNode = Composites[NodeIndex];
		UBTService* NewSvc = NewObject<UBTService>(BT, SvcClass);
		if (!NewSvc)
		{
			return FMCPToolResult::Error(TEXT("Failed to create service."));
		}

		TargetNode->Services.Add(NewSvc);
		BT->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tree"), TreePath);
		Result->SetStringField(TEXT("service_class"), SvcClassName);
		Result->SetNumberField(TEXT("node_index"), NodeIndex);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_bt_selector
// ─────────────────────────────────────────────────────────────
class FTool_AddBTSelector : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.selector.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TreePath = Params->GetStringField(TEXT("tree"));
		int32 ParentIndex = Params->HasField(TEXT("parent_index")) ? (int32)Params->GetNumberField(TEXT("parent_index")) : 0;

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *TreePath);
		if (!BT || !BT->RootNode)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Behavior Tree not found or has no root at '%s'."), *TreePath));
		}

		TArray<UBTCompositeNode*> Composites;
		TQueue<UBTCompositeNode*> Queue;
		Queue.Enqueue(BT->RootNode);
		while (!Queue.IsEmpty())
		{
			UBTCompositeNode* Current = nullptr;
			Queue.Dequeue(Current);
			Composites.Add(Current);
			for (int32 i = 0; i < Current->Children.Num(); ++i)
			{
				if (UBTCompositeNode* Child = Cast<UBTCompositeNode>(Current->Children[i].ChildComposite))
				{
					Queue.Enqueue(Child);
				}
			}
		}

		if (ParentIndex >= Composites.Num())
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Parent index %d out of range (%d composites)."), ParentIndex, Composites.Num()));
		}

		UBTCompositeNode* ParentNode = Composites[ParentIndex];
		UBTComposite_Selector* NewSelector = NewObject<UBTComposite_Selector>(BT);

		FBTCompositeChild NewChild;
		NewChild.ChildComposite = NewSelector;
		ParentNode->Children.Add(NewChild);

		BT->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tree"), TreePath);
		Result->SetStringField(TEXT("type"), TEXT("Selector"));
		Result->SetNumberField(TEXT("parent_index"), ParentIndex);
		Result->SetNumberField(TEXT("child_index"), ParentNode->Children.Num() - 1);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_bt_sequence
// ─────────────────────────────────────────────────────────────
class FTool_AddBTSequence : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.sequence.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TreePath = Params->GetStringField(TEXT("tree"));
		int32 ParentIndex = Params->HasField(TEXT("parent_index")) ? (int32)Params->GetNumberField(TEXT("parent_index")) : 0;

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *TreePath);
		if (!BT || !BT->RootNode)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Behavior Tree not found or has no root at '%s'."), *TreePath));
		}

		TArray<UBTCompositeNode*> Composites;
		TQueue<UBTCompositeNode*> Queue;
		Queue.Enqueue(BT->RootNode);
		while (!Queue.IsEmpty())
		{
			UBTCompositeNode* Current = nullptr;
			Queue.Dequeue(Current);
			Composites.Add(Current);
			for (int32 i = 0; i < Current->Children.Num(); ++i)
			{
				if (UBTCompositeNode* Child = Cast<UBTCompositeNode>(Current->Children[i].ChildComposite))
				{
					Queue.Enqueue(Child);
				}
			}
		}

		if (ParentIndex >= Composites.Num())
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Parent index %d out of range (%d composites)."), ParentIndex, Composites.Num()));
		}

		UBTCompositeNode* ParentNode = Composites[ParentIndex];
		UBTComposite_Sequence* NewSeq = NewObject<UBTComposite_Sequence>(BT);

		FBTCompositeChild NewChild;
		NewChild.ChildComposite = NewSeq;
		ParentNode->Children.Add(NewChild);

		BT->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tree"), TreePath);
		Result->SetStringField(TEXT("type"), TEXT("Sequence"));
		Result->SetNumberField(TEXT("parent_index"), ParentIndex);
		Result->SetNumberField(TEXT("child_index"), ParentNode->Children.Num() - 1);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// link_blackboard_to_tree
// ─────────────────────────────────────────────────────────────
class FTool_LinkBlackboardToTree : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("ai.behavior_tree.blackboard.link");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString TreePath = Params->GetStringField(TEXT("tree"));
		FString BBPath = Params->GetStringField(TEXT("blackboard"));

		UBehaviorTree* BT = LoadObject<UBehaviorTree>(nullptr, *TreePath);
		if (!BT)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Behavior Tree not found at '%s'."), *TreePath));
		}

		UBlackboardData* BB = LoadObject<UBlackboardData>(nullptr, *BBPath);
		if (!BB)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Blackboard not found at '%s'."), *BBPath));
		}

		BT->BlackboardAsset = BB;
		BT->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("tree"), TreePath);
		Result->SetStringField(TEXT("blackboard"), BBPath);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────
namespace UEAIIntegrationTools
{
	void RegisterBehaviorTreeTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_CreateBehaviorTree>());
		Registry.Register(MakeShared<FTool_CreateBlackboard>());
		Registry.Register(MakeShared<FTool_AddBlackboardKey>());
		Registry.Register(MakeShared<FTool_AddBTTask>());
		Registry.Register(MakeShared<FTool_AddBTDecorator>());
		Registry.Register(MakeShared<FTool_AddBTService>());
		Registry.Register(MakeShared<FTool_AddBTSelector>());
		Registry.Register(MakeShared<FTool_AddBTSequence>());
		Registry.Register(MakeShared<FTool_LinkBlackboardToTree>());
	}
}
