// Project-wide, read-only Niagara collision inventory.
//
// The graph audit capability intentionally takes one Niagara System. This
// companion surface uses the Asset Registry to discover project systems and
// reuses the same bounded graph-level operation inventory for every asset.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/EngineeringContractUtils.h"

#include "Niagara_Graph_Audit.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraphNode.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace UEAINiagaraProjectCollisionAudit
{
using UEAINiagaraGraphAuditExtensions::FNiagaraCollisionOperationInventory;

constexpr int32 DefaultSystemLimit = 128;
constexpr int32 MaxSystemLimit = 512;
constexpr int32 DefaultGraphLimit = 64;
constexpr int32 MaxGraphLimit = 512;
constexpr int32 DefaultDetailLimit = 256;
constexpr int32 MaxDetailLimit = 4096;

struct FGraphTargetContext
{
	FString TargetKind;
	FString Context;
	FString ScriptUsage;
};

struct FGraphTarget
{
	UNiagaraGraph* Graph = nullptr;
	UNiagaraGraph* RootGraph = nullptr;
	FString ScopeKind;
	FString ScopeName;
	FString EmitterPath;
	FString ScriptUsage;
	FString TargetKind;
	FString Context;
	TArray<FGraphTargetContext> TargetContexts;
	FString ReferencedFromNode;
	int32 ReferenceDepth = 0;
	bool bOwnedBySystem = false;
};

void SetStringArray(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> JsonValues;
	JsonValues.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		JsonValues.Add(MakeShared<FJsonValueString>(Value));
	}
	Object->SetArrayField(Field, JsonValues);
}

bool ReadInteger(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	const int32 DefaultValue,
	const int32 MinValue,
	const int32 MaxValue,
	int32& OutValue,
	FString& OutError)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(Field))
	{
		return true;
	}

	double Number = 0.0;
	if (!Params->TryGetNumberField(Field, Number)
		|| !FMath::IsFinite(Number)
		|| FMath::TruncToDouble(Number) != Number
		|| Number < static_cast<double>(MinValue)
		|| Number > static_cast<double>(MaxValue))
	{
		OutError = FString::Printf(
			TEXT("%s must be an integer in [%d, %d]."),
			Field,
			MinValue,
			MaxValue);
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

FString NormalizePackagePath(const FString& RequestedPath)
{
	FString Path = RequestedPath.TrimStartAndEnd();
	while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
	{
		Path.LeftChopInline(1, false);
	}
	return Path;
}

bool IsValidProjectPackagePath(const FString& Path)
{
	return !Path.IsEmpty()
		&& Path.StartsWith(TEXT("/Game"), ESearchCase::IgnoreCase)
		&& (Path.Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
			|| Path.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase))
		&& !Path.Contains(TEXT(".."));
}

bool MatchesPackagePath(const FAssetData& Asset, const FString& PackagePath)
{
	const FString AssetPackage = Asset.PackageName.ToString();
	return PackagePath.Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
		? AssetPackage.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase)
		: AssetPackage.Equals(PackagePath, ESearchCase::IgnoreCase)
			|| AssetPackage.StartsWith(
				PackagePath + TEXT("/"),
				ESearchCase::IgnoreCase);
}

bool MatchesSelector(
	const FString& Selector,
	const FString& Name,
	const FString& ObjectPath)
{
	if (Selector.IsEmpty())
	{
		return true;
	}
	return Selector.Equals(Name, ESearchCase::IgnoreCase)
		|| Selector.Equals(ObjectPath, ESearchCase::IgnoreCase)
		|| ObjectPath.EndsWith(
			FString::Printf(TEXT(".%s"), *Selector),
			ESearchCase::IgnoreCase);
}

void AddGraphAndReferences(
	UNiagaraGraph* Graph,
	UNiagaraGraph* RootGraph,
	const FString& ScopeKind,
	const FString& ScopeName,
	const FString& EmitterPath,
	const FString& ScriptUsage,
	const FString& TargetKind,
	const FString& Context,
	UPackage* SystemPackage,
	TSet<UNiagaraGraph*>& Visited,
	TArray<FGraphTarget>& OutGraphs,
	const int32 ReferenceDepth = 0,
	const FString& ReferencedFromNode = FString())
{
	if (!Graph)
	{
		return;
	}

	const int32 ExistingIndex = OutGraphs.IndexOfByPredicate(
		[Graph](const FGraphTarget& Existing)
		{
			return Existing.Graph == Graph;
		});
	if (Visited.Contains(Graph))
	{
		if (ExistingIndex != INDEX_NONE)
		{
			FGraphTarget& Existing = OutGraphs[ExistingIndex];
			const bool bKnownContext = Existing.TargetContexts.ContainsByPredicate(
				[&](const FGraphTargetContext& ExistingContext)
				{
					return ExistingContext.TargetKind == TargetKind
						&& ExistingContext.Context == Context
						&& ExistingContext.ScriptUsage == ScriptUsage;
				});
			if (!bKnownContext)
			{
				FGraphTargetContext NewContext;
				NewContext.TargetKind = TargetKind;
				NewContext.Context = Context;
				NewContext.ScriptUsage = ScriptUsage;
				Existing.TargetContexts.Add(MoveTemp(NewContext));
			}
			else
			{
				return;
			}
		}
		else
		{
			return;
		}
	}
	else
	{
		Visited.Add(Graph);

		FGraphTarget Target;
		Target.Graph = Graph;
		Target.RootGraph = RootGraph;
		Target.ScopeKind = ScopeKind;
		Target.ScopeName = ScopeName;
		Target.EmitterPath = EmitterPath;
		Target.ScriptUsage = ScriptUsage;
		Target.TargetKind = TargetKind;
		Target.Context = Context;
		FGraphTargetContext InitialContext;
		InitialContext.TargetKind = TargetKind;
		InitialContext.Context = Context;
		InitialContext.ScriptUsage = ScriptUsage;
		Target.TargetContexts.Add(MoveTemp(InitialContext));
		Target.ReferencedFromNode = ReferencedFromNode;
		Target.ReferenceDepth = ReferenceDepth;
		Target.bOwnedBySystem = SystemPackage != nullptr
			&& Graph->GetOutermost() == SystemPackage;
		OutGraphs.Add(Target);
	}

	for (UEdGraphNode* RawNode : Graph->Nodes)
	{
		const UNiagaraNodeFunctionCall* FunctionCall =
			Cast<UNiagaraNodeFunctionCall>(RawNode);
		if (!FunctionCall)
		{
			continue;
		}
		AddGraphAndReferences(
			FunctionCall->GetCalledGraph(),
			RootGraph,
			ScopeKind,
			ScopeName,
			EmitterPath,
			ScriptUsage,
			TargetKind,
			Context,
			SystemPackage,
			Visited,
			OutGraphs,
			ReferenceDepth + 1,
			FunctionCall->GetPathName());
	}
}

void CollectSystemGraphs(
	UNiagaraSystem* System,
	const FString& EmitterSelector,
	const FString& GraphSelector,
	TArray<FGraphTarget>& OutGraphs)
{
	OutGraphs.Reset();
	if (!System)
	{
		return;
	}

	TSet<UNiagaraGraph*> Visited;
	const auto AddSystemScript =
		[&](UNiagaraScript* Script, const TCHAR* Usage)
	{
		if (!Script)
		{
			return;
		}
		UNiagaraScriptSource* Source =
			Cast<UNiagaraScriptSource>(Script->GetLatestSource());
		if (Source && Source->NodeGraph)
		{
			AddGraphAndReferences(
				Source->NodeGraph,
				Source->NodeGraph,
				TEXT("system"),
				TEXT("system"),
				FString(),
				Usage,
				TEXT("system"),
				Usage,
				System->GetOutermost(),
				Visited,
				OutGraphs);
		}
	};

	if (EmitterSelector.IsEmpty())
	{
		AddSystemScript(System->GetSystemSpawnScript(), TEXT("systemSpawn"));
		AddSystemScript(System->GetSystemUpdateScript(), TEXT("systemUpdate"));
	}

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		const UNiagaraEmitter* Emitter = Instance.Emitter;
		const FString EmitterName = Handle.GetName().ToString();
		const FString EmitterPath = Emitter ? Emitter->GetPathName() : FString();
		if (!MatchesSelector(EmitterSelector, EmitterName, EmitterPath))
		{
			continue;
		}

		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		UNiagaraScriptSource* Source = EmitterData
			? Cast<UNiagaraScriptSource>(EmitterData->GraphSource)
			: nullptr;
		if (Source && Source->NodeGraph)
		{
			AddGraphAndReferences(
				Source->NodeGraph,
				Source->NodeGraph,
				TEXT("emitter"),
				EmitterName,
				EmitterPath,
				TEXT("emitter"),
				TEXT("emitter"),
				EmitterName,
				System->GetOutermost(),
				Visited,
				OutGraphs);
		}
	}

	if (!GraphSelector.IsEmpty())
	{
		OutGraphs.RemoveAll(
			[&GraphSelector](const FGraphTarget& Target)
			{
				return !MatchesSelector(
					GraphSelector,
					Target.Graph ? Target.Graph->GetName() : FString(),
					Target.Graph ? Target.Graph->GetPathName() : FString());
			});
	}

	OutGraphs.Sort(
		[](const FGraphTarget& Left, const FGraphTarget& Right)
		{
			return Left.Graph && Right.Graph
				? Left.Graph->GetPathName() < Right.Graph->GetPathName()
				: Left.Graph != nullptr;
		});
}

void SetGraphContext(
	const TSharedRef<FJsonObject>& GraphObject,
	const FGraphTarget& Target,
	const FString& SystemPath)
{
	GraphObject->SetStringField(TEXT("system"), SystemPath);
	GraphObject->SetStringField(
		TEXT("path"),
		Target.Graph ? Target.Graph->GetPathName() : FString());
	GraphObject->SetStringField(
		TEXT("name"),
		Target.Graph ? Target.Graph->GetName() : FString());
	GraphObject->SetStringField(TEXT("scopeKind"), Target.ScopeKind);
	GraphObject->SetStringField(TEXT("scopeName"), Target.ScopeName);
	GraphObject->SetStringField(TEXT("emitterPath"), Target.EmitterPath);
	GraphObject->SetStringField(TEXT("scriptUsage"), Target.ScriptUsage);
	TArray<TSharedPtr<FJsonValue>> Contexts;
	for (const FGraphTargetContext& Context : Target.TargetContexts)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("targetKind"), Context.TargetKind);
		Value->SetStringField(TEXT("context"), Context.Context);
		Value->SetStringField(TEXT("scriptUsage"), Context.ScriptUsage);
		Contexts.Add(MakeShared<FJsonValueObject>(Value));
	}
	GraphObject->SetArrayField(TEXT("targetContexts"), Contexts);
	GraphObject->SetStringField(
		TEXT("rootGraph"),
		Target.RootGraph ? Target.RootGraph->GetPathName() : FString());
	GraphObject->SetNumberField(TEXT("referenceDepth"), Target.ReferenceDepth);
	GraphObject->SetStringField(TEXT("referencedFromNode"), Target.ReferencedFromNode);
	const UPackage* Package = Target.Graph ? Target.Graph->GetOutermost() : nullptr;
	GraphObject->SetStringField(
		TEXT("package"),
		Package ? Package->GetName() : FString());
	GraphObject->SetBoolField(TEXT("ownedBySystem"), Target.bOwnedBySystem);
	GraphObject->SetBoolField(
		TEXT("editable"),
		Target.bOwnedBySystem
			&& Package != nullptr
			&& Package->GetName().StartsWith(TEXT("/Game/")));
}

int32 ReadCount(const TSharedRef<FJsonObject>& Object, const TCHAR* Field)
{
	double Number = 0.0;
	return Object->TryGetNumberField(Field, Number)
		? FMath::Max(0, FMath::TruncToInt(Number))
		: 0;
}

void AddCount(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	int32& Total)
{
	Total += ReadCount(Object, Field);
}

// A path cursor is unaffected by deletions or insertions before the last asset.
int32 FindFirstSystemAfter(const TArray<FAssetData>& Assets, const FString& AfterSystem)
{
	int32 Index = 0;
	if (!AfterSystem.IsEmpty())
	{
		while (Index < Assets.Num() && Assets[Index].GetObjectPathString() <= AfterSystem)
		{
			++Index;
		}
	}
	return Index;
}

class FTool_NiagaraProjectCollisionAudit final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.collision.audit_project");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString PackagePath = TEXT("/Game");
		FString Filter;
		FString EmitterSelector;
		FString GraphSelector;
		FString OperationSelector;
		FString AfterSystem;
		if (Params.IsValid())
		{
			const auto ReadOptionalString =
				[&Params](const TCHAR* Field, FString& OutValue)
			{
				return !Params->HasField(Field)
					|| Params->TryGetStringField(Field, OutValue);
			};
			if (!ReadOptionalString(TEXT("packagePath"), PackagePath)
				|| !ReadOptionalString(TEXT("filter"), Filter)
				|| !ReadOptionalString(TEXT("emitter"), EmitterSelector)
				|| !ReadOptionalString(TEXT("graph"), GraphSelector)
				|| !ReadOptionalString(TEXT("operation"), OperationSelector)
				|| !ReadOptionalString(TEXT("afterSystem"), AfterSystem))
			{
				return FMCPToolResult::Error(
					TEXT("packagePath, filter, emitter, graph, operation and afterSystem must be strings when provided."),
					TEXT("invalid_string_parameter"),
					422);
			}
		}
		PackagePath = NormalizePackagePath(PackagePath);
		Filter = Filter.TrimStartAndEnd();
		EmitterSelector = EmitterSelector.TrimStartAndEnd();
		GraphSelector = GraphSelector.TrimStartAndEnd();
		OperationSelector = OperationSelector.TrimStartAndEnd();
		AfterSystem = AfterSystem.TrimStartAndEnd();
		if (!AfterSystem.IsEmpty()
			&& (!AfterSystem.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidObjectPath(AfterSystem)))
		{
			return FMCPToolResult::Error(TEXT("afterSystem must be a Niagara System object path under /Game."), TEXT("invalid_cursor"), 422);
		}
		if (!IsValidProjectPackagePath(PackagePath))
		{
			return FMCPToolResult::Error(
				TEXT("packagePath must be /Game or a child path under /Game."),
				TEXT("invalid_package_path"),
				422);
		}

		int32 SystemLimit = DefaultSystemLimit;
		int32 GraphLimit = DefaultGraphLimit;
		int32 DetailLimit = DefaultDetailLimit;
		FString IntegerError;
		if (!ReadInteger(
			Params,
			TEXT("limit"),
			DefaultSystemLimit,
			1,
			MaxSystemLimit,
			SystemLimit,
			IntegerError)
			|| !ReadInteger(
				Params,
				TEXT("graphLimit"),
				DefaultGraphLimit,
				1,
				MaxGraphLimit,
				GraphLimit,
				IntegerError)
			|| !ReadInteger(
				Params,
				TEXT("detailLimit"),
				DefaultDetailLimit,
				1,
				MaxDetailLimit,
				DetailLimit,
				IntegerError))
		{
			return FMCPToolResult::Error(
				IntegerError,
				TEXT("invalid_limit"),
				422);
		}

		bool bIncludeDisabled = true;
		bool bWaitForAssetRegistry = true;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(TEXT("includeDisabled"), bIncludeDisabled);
			Params->TryGetBoolField(
				TEXT("waitForAssetRegistry"),
				bWaitForAssetRegistry);
		}

		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		if (bWaitForAssetRegistry)
		{
			Registry.WaitForCompletion();
		}

		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(
			UNiagaraSystem::StaticClass()->GetClassPathName(),
			Assets,
			true);
		Assets.RemoveAll(
			[&](const FAssetData& Asset)
			{
				if (!MatchesPackagePath(Asset, PackagePath))
				{
					return true;
				}
				if (Filter.IsEmpty())
				{
					return false;
				}
				const FString AssetName = Asset.AssetName.ToString();
				const FString ObjectPath = Asset.GetObjectPathString();
				return !AssetName.Contains(Filter, ESearchCase::IgnoreCase)
					&& !ObjectPath.Contains(Filter, ESearchCase::IgnoreCase);
			});
		Assets.Sort(
			[](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.GetObjectPathString() < Right.GetObjectPathString();
			});

		TArray<TSharedPtr<FJsonValue>> SystemValues;
		const int32 StartIndex = FindFirstSystemAfter(Assets, AfterSystem);
		FString LastScannedSystem;
		TArray<TSharedPtr<FJsonValue>> FailureValues;
		FNiagaraCollisionOperationInventory OperationInventory;
		int32 ScannedSystemCount = 0;
		int32 LoadFailureCount = 0;
		int32 MatchedSystemCount = 0;
		int32 GraphCandidateCount = 0;
		int32 GraphMatchedCount = 0;
		int32 GraphReturnedCount = 0;
		int32 CollisionFunctionNodeCount = 0;
		int32 OrdinaryCollisionQueryCount = 0;
		int32 DistanceFieldCount = 0;
		int32 AsyncGpuTraceCount = 0;
		int32 RigidMeshCollisionQueryCount = 0;
		int32 PhysicsAssetCount = 0;

		for (int32 AssetIndex = StartIndex;
			AssetIndex < Assets.Num() && ScannedSystemCount < SystemLimit;
			++AssetIndex)
		{
			const FAssetData& Asset = Assets[AssetIndex];
			LastScannedSystem = Asset.GetObjectPathString();
			++ScannedSystemCount;
			UNiagaraSystem* System = Cast<UNiagaraSystem>(Asset.GetAsset());
			if (!System)
			{
				++LoadFailureCount;
				if (FailureValues.Num() < SystemLimit)
				{
					TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetStringField(
						TEXT("asset"),
						Asset.GetObjectPathString());
					Failure->SetStringField(
						TEXT("package"),
						Asset.PackageName.ToString());
					Failure->SetStringField(
						TEXT("reason"),
						TEXT("Niagara System could not be loaded."));
					FailureValues.Add(MakeShared<FJsonValueObject>(Failure));
				}
				continue;
			}

			TArray<FGraphTarget> Graphs;
			CollectSystemGraphs(System, EmitterSelector, GraphSelector, Graphs);
			TArray<TSharedPtr<FJsonValue>> GraphValues;
			int32 SystemGraphMatchedCount = 0;
			int32 SystemGraphReturnedCount = 0;
			int32 SystemCollisionNodeCount = 0;
			int32 SystemOrdinaryCount = 0;
			int32 SystemDistanceFieldCount = 0;
			int32 SystemAsyncCount = 0;
			int32 SystemRigidMeshCount = 0;
			int32 SystemPhysicsAssetCount = 0;
			for (const FGraphTarget& Target : Graphs)
			{
				if (!Target.Graph)
				{
					continue;
				}
				++GraphCandidateCount;
				TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
				SetGraphContext(
					GraphObject,
					Target,
					System->GetPathName());
				UEAINiagaraGraphAuditExtensions::AppendDetailedCollisionAudit(
					Target.Graph,
					bIncludeDisabled,
					DetailLimit,
					GraphObject,
					&OperationInventory,
					OperationSelector);
				const int32 GraphCollisionNodes = ReadCount(
					GraphObject,
					TEXT("collisionFunctionNodeCount"));
				const bool bGraphMatched = GraphCollisionNodes > 0
					|| OperationSelector.IsEmpty()
					&& (ReadCount(GraphObject, TEXT("collisionDataInterfaceCount")) > 0);
				if (!bGraphMatched)
				{
					continue;
				}
				++GraphMatchedCount;
				++SystemGraphMatchedCount;
				AddCount(GraphObject, TEXT("collisionFunctionNodeCount"), CollisionFunctionNodeCount);
				AddCount(GraphObject, TEXT("ordinaryCollisionQueryNodeCount"), OrdinaryCollisionQueryCount);
				AddCount(GraphObject, TEXT("distanceFieldNodeCount"), DistanceFieldCount);
				AddCount(GraphObject, TEXT("asyncGpuTraceNodeCount"), AsyncGpuTraceCount);
				AddCount(GraphObject, TEXT("rigidMeshCollisionQueryNodeCount"), RigidMeshCollisionQueryCount);
				AddCount(GraphObject, TEXT("physicsAssetNodeCount"), PhysicsAssetCount);
				AddCount(GraphObject, TEXT("collisionFunctionNodeCount"), SystemCollisionNodeCount);
				AddCount(GraphObject, TEXT("ordinaryCollisionQueryNodeCount"), SystemOrdinaryCount);
				AddCount(GraphObject, TEXT("distanceFieldNodeCount"), SystemDistanceFieldCount);
				AddCount(GraphObject, TEXT("asyncGpuTraceNodeCount"), SystemAsyncCount);
				AddCount(GraphObject, TEXT("rigidMeshCollisionQueryNodeCount"), SystemRigidMeshCount);
				AddCount(GraphObject, TEXT("physicsAssetNodeCount"), SystemPhysicsAssetCount);
				if (SystemGraphReturnedCount < GraphLimit)
				{
					GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
					++SystemGraphReturnedCount;
					++GraphReturnedCount;
				}
			}

			if (SystemGraphMatchedCount > 0)
			{
				++MatchedSystemCount;
			}
			TSharedRef<FJsonObject> SystemObject = MakeShared<FJsonObject>();
			SystemObject->SetStringField(TEXT("system"), System->GetPathName());
			SystemObject->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
			SystemObject->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			SystemObject->SetBoolField(
				TEXT("packageDirty"),
				System->GetOutermost() && System->GetOutermost()->IsDirty());
			SystemObject->SetBoolField(
				TEXT("matched"),
				SystemGraphMatchedCount > 0);
			SystemObject->SetNumberField(TEXT("graphCandidateCount"), Graphs.Num());
			SystemObject->SetNumberField(TEXT("graphMatchedCount"), SystemGraphMatchedCount);
			SystemObject->SetNumberField(TEXT("graphReturnedCount"), SystemGraphReturnedCount);
			SystemObject->SetBoolField(
				TEXT("graphsTruncated"),
				SystemGraphReturnedCount < SystemGraphMatchedCount);
			SystemObject->SetNumberField(TEXT("collisionFunctionNodeCount"), SystemCollisionNodeCount);
			SystemObject->SetNumberField(TEXT("ordinaryCollisionQueryCount"), SystemOrdinaryCount);
			SystemObject->SetNumberField(TEXT("distanceFieldCount"), SystemDistanceFieldCount);
			SystemObject->SetNumberField(TEXT("asyncGpuTraceCount"), SystemAsyncCount);
			SystemObject->SetNumberField(TEXT("rigidMeshCollisionQueryCount"), SystemRigidMeshCount);
			SystemObject->SetNumberField(TEXT("physicsAssetCount"), SystemPhysicsAssetCount);
			SystemObject->SetArrayField(TEXT("graphs"), GraphValues);
			SystemValues.Add(MakeShared<FJsonValueObject>(SystemObject));
		}

		TArray<FString> InventoryNames;
		OperationInventory.GetKeys(InventoryNames);
		InventoryNames.Sort();
		TArray<FString> Warnings;
		if (!bWaitForAssetRegistry && Registry.IsLoadingAssets())
		{
			Warnings.Add(TEXT("Asset Registry is still loading; candidateSystemCount may be incomplete."));
		}
		if (!OperationSelector.IsEmpty() && InventoryNames.Num() == 0)
		{
			Warnings.Add(FString::Printf(
				TEXT("No Niagara collision operation matched operation selector '%s'."),
				*OperationSelector));
		}

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("matchedSystemCount"), MatchedSystemCount);
		Summary->SetNumberField(TEXT("graphCandidateCount"), GraphCandidateCount);
		Summary->SetNumberField(TEXT("graphMatchedCount"), GraphMatchedCount);
		Summary->SetNumberField(TEXT("graphReturnedCount"), GraphReturnedCount);
		Summary->SetNumberField(TEXT("collisionFunctionNodeCount"), CollisionFunctionNodeCount);
		Summary->SetNumberField(TEXT("ordinaryCollisionQueryCount"), OrdinaryCollisionQueryCount);
		Summary->SetNumberField(TEXT("distanceFieldCount"), DistanceFieldCount);
		Summary->SetNumberField(TEXT("asyncGpuTraceCount"), AsyncGpuTraceCount);
		Summary->SetNumberField(TEXT("rigidMeshCollisionQueryCount"), RigidMeshCollisionQueryCount);
		Summary->SetNumberField(TEXT("physicsAssetCount"), PhysicsAssetCount);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-project-collision-audit.v1"));
		Result->SetStringField(TEXT("packagePath"), PackagePath);
		Result->SetStringField(TEXT("filter"), Filter);
		Result->SetStringField(TEXT("emitter"), EmitterSelector);
		Result->SetStringField(TEXT("graph"), GraphSelector);
		Result->SetStringField(TEXT("operationSelector"), OperationSelector);
		const bool bHasMore = StartIndex + ScannedSystemCount < Assets.Num();
		Result->SetStringField(TEXT("afterSystem"), AfterSystem);
		Result->SetBoolField(TEXT("hasMore"), bHasMore);
		Result->SetStringField(TEXT("nextAfterSystem"), bHasMore ? LastScannedSystem : FString());
		Result->SetStringField(TEXT("summaryScope"), TEXT("page"));
		Result->SetBoolField(TEXT("includeDisabled"), bIncludeDisabled);
		Result->SetBoolField(TEXT("waitForAssetRegistry"), bWaitForAssetRegistry);
		Result->SetBoolField(TEXT("assetRegistryComplete"), !Registry.IsLoadingAssets());
		Result->SetNumberField(TEXT("candidateSystemCount"), Assets.Num());
		Result->SetNumberField(TEXT("scannedSystemCount"), ScannedSystemCount);
		Result->SetNumberField(TEXT("returnedSystemCount"), SystemValues.Num());
		Result->SetBoolField(
			TEXT("systemsTruncated"),
			bHasMore);
		Result->SetNumberField(TEXT("loadFailureCount"), LoadFailureCount);
		Result->SetNumberField(TEXT("loadFailureReturned"), FailureValues.Num());
		Result->SetBoolField(
			TEXT("loadFailuresTruncated"),
			FailureValues.Num() < LoadFailureCount);
		Result->SetBoolField(
			TEXT("operationSelectorMatched"),
			OperationSelector.IsEmpty() || InventoryNames.Num() > 0);
		Result->SetStringField(
			TEXT("operationSelectorCanonical"),
			InventoryNames.Num() == 1 ? InventoryNames[0] : FString());
		Result->SetNumberField(TEXT("systemLimit"), SystemLimit);
		Result->SetNumberField(TEXT("graphLimit"), GraphLimit);
		Result->SetNumberField(TEXT("detailLimit"), DetailLimit);
		Result->SetObjectField(TEXT("summary"), Summary);
		Result->SetArrayField(TEXT("systems"), SystemValues);
		Result->SetArrayField(TEXT("loadFailures"), FailureValues);
		UEAINiagaraGraphAuditExtensions::SetCollisionOperationInventory(
			Result,
			OperationInventory,
			DetailLimit);
		SetStringArray(Result, TEXT("warnings"), Warnings);
		return FMCPToolResult::Ok(Result);
	}
};

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraAuditCursorTest,
	"UE_AI_integration.Niagara.AuditCursor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAINiagaraAuditCursorTest::RunTest(const FString& Parameters)
{
	TArray<FAssetData> Assets;
	for (const TCHAR* Name : {TEXT("A"), TEXT("B"), TEXT("C")})
	{
		FAssetData Asset;
		Asset.PackageName = FName(FString(TEXT("/Game/Effects/")) + Name);
		Asset.AssetName = FName(Name);
		Assets.Add(Asset);
	}
	TestEqual(TEXT("First page starts at zero"), FindFirstSystemAfter(Assets, FString()), 0);
	const FString Cursor = Assets[1].GetObjectPathString();
	TestEqual(TEXT("Resume excludes the last processed asset"), FindFirstSystemAfter(Assets, Cursor), 2);
	Assets.RemoveAt(1);
	TestEqual(TEXT("Deleting the cursor asset does not skip the next asset"), FindFirstSystemAfter(Assets, Cursor), 1);
	TestEqual(TEXT("End cursor yields an empty page"), FindFirstSystemAfter(Assets, Assets.Last().GetObjectPathString()), Assets.Num());
	TestEqual(TEXT("Empty registry yields an empty page"), FindFirstSystemAfter(TArray<FAssetData>(), Cursor), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraAuditContextsTest,
	"UE_AI_integration.Niagara.AuditContexts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAINiagaraAuditContextsTest::RunTest(const FString& Parameters)
{
	UNiagaraScript* Script = NewObject<UNiagaraScript>(GetTransientPackage());
	UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Script);
	UNiagaraGraph* SharedGraph = NewObject<UNiagaraGraph>(Source);
	Source->NodeGraph = SharedGraph;
	Script->SetLatestSource(Source);
	UNiagaraGraph* Root = NewObject<UNiagaraGraph>(GetTransientPackage());
	UNiagaraNodeFunctionCall* Call = NewObject<UNiagaraNodeFunctionCall>(Root);
	Call->FunctionScript = Script;
	Root->AddNode(Call, false, false);
	TSet<UNiagaraGraph*> Visited;
	TArray<FGraphTarget> Graphs;
	for (const TCHAR* Usage : {TEXT("systemSpawn"), TEXT("systemUpdate"), TEXT("systemUpdate")})
	{
		AddGraphAndReferences(Root, Root, TEXT("system"), TEXT("system"), FString(), Usage,
			TEXT("system"), Usage, nullptr, Visited, Graphs);
	}
	TestEqual(TEXT("Shared graphs remain deduplicated"), Graphs.Num(), 2);
	for (const FGraphTarget& Target : Graphs)
	{
		TestEqual(TEXT("New contexts propagate into referenced graphs once"), Target.TargetContexts.Num(), 2);
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		SetGraphContext(Json, Target, TEXT("/Game/Test.Test"));
		TestEqual(TEXT("Every collected context is returned"), Json->GetArrayField(TEXT("targetContexts")).Num(), 2);
	}
	return true;
}
#endif

} // namespace UEAINiagaraProjectCollisionAudit

#else

class FUnavailableNiagaraProjectCollisionAudit final : public FMCPToolBase
{
public:
	explicit FUnavailableNiagaraProjectCollisionAudit(FString InCapabilityId)
		: CapabilityId(MoveTemp(InCapabilityId))
	{
	}

	FString GetCapabilityId() const override
	{
		return CapabilityId;
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(
			TEXT("Niagara graph support was not compiled into this plugin build."),
			TEXT("capability_unavailable"),
			409);
	}

private:
	FString CapabilityId;
};

#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraCollisionProjectAuditTools(FMCPToolRegistry& Registry)
{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
	Registry.Register(MakeShared<UEAINiagaraProjectCollisionAudit::FTool_NiagaraProjectCollisionAudit>());
#else
	Registry.Register(MakeShared<FUnavailableNiagaraProjectCollisionAudit>(
		TEXT("content.niagara.collision.audit_project")));
#endif
}
}
