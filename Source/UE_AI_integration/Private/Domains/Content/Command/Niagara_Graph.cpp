// Plan-gated Niagara graph inspection and function-call node mutations.
//
// This surface intentionally stays narrow: Niagara graph edits are only
// admitted for function-call nodes, and every write is bound to the graph's
// current change id before it is applied.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/EngineeringContractUtils.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

//++[UEAI] Begin implementation
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraDataInterfaceAsyncGpuTrace.h"
#include "NiagaraDataInterfaceRigidMeshCollisionQuery.h"
#include "NiagaraDataInterfacePhysicsAsset.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraSystem.h"

#include "Editor.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "Niagara_Graph_Audit.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace UEAINiagaraGraphPrivate
{
using UEAIIntegration::Infrastructure::DigestJson;
using UEAIIntegration::Infrastructure::SetBoundedArray;
using UEAIIntegration::Infrastructure::TryDigestJson;
using UEAIIntegration::Infrastructure::ValidateChangeApproval;

constexpr int32 DefaultNodeLimit = 2048;
constexpr int32 MaxNodeLimit = 4096;
constexpr int32 MaxPinLimitPerNode = 256;
//--[UEAI] End implementation

struct FGraphTarget
{
	UNiagaraGraph* Graph = nullptr;
	UNiagaraGraph* RootGraph = nullptr;
	FString ScopeKind;
	FString ScopeName;
	FString EmitterPath;
	FString ScriptUsage;
	FString ReferencedFromNodePath;
	int32 ReferenceDepth = 0;
	bool bOwnedBySystem = false;
};

struct FNodeTarget
{
	FGraphTarget Graph;
	UNiagaraNode* Node = nullptr;
	UNiagaraNodeFunctionCall* FunctionCall = nullptr;
};

struct FPinTarget
{
	FNodeTarget Node;
	UEdGraphPin* Pin = nullptr;
};

struct FPinMutationReceipt
{
	FString ReceiptId;
	FString RequestId;
	FString PlanDigest;
	FString SystemPath;
	FString GraphPath;
	FString NodePath;
	FString PinName;
	FString BeforeValue;
	FString AfterValue;
	FString GraphChangeIdAfter;
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UNiagaraGraph> Graph;
	TWeakObjectPtr<UNiagaraNode> Node;
	bool bCompiled = false;
	FString CompileStatus = TEXT("unknown");
	bool bRolledBack = false;
};

struct FMutationReceipt
{
	FString ReceiptId;
	FString RequestId;
	FString PlanDigest;
	FString SystemPath;
	FString GraphPath;
	FString NodePath;
	FString GraphChangeIdAfter;
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UNiagaraGraph> Graph;
	TWeakObjectPtr<UNiagaraNodeFunctionCall> Node;
	FString MutationScope;
	struct FNodeState
	{
		TWeakObjectPtr<UNiagaraNode> Node;
		FString NodePath;
		ENodeEnabledState BeforeState = ENodeEnabledState::Enabled;
		bool bBeforeUserSet = false;
		ENodeEnabledState AfterState = ENodeEnabledState::Enabled;
		bool bAfterUserSet = false;
	};
	TArray<FNodeState> AffectedNodes;
	ENodeEnabledState BeforeState = ENodeEnabledState::Enabled;
	bool bBeforeUserSet = false;
	ENodeEnabledState AfterState = ENodeEnabledState::Enabled;
	bool bCompiled = false;
	FString CompileStatus = TEXT("unknown");
	bool bRolledBack = false;
};

// Collision policy edits may cover several graphs in one Niagara System, so
// they use a separate receipt rather than widening the existing single-graph
// node mutation contract.
struct FCollisionPolicyNodeState
{
	TWeakObjectPtr<UNiagaraGraph> Graph;
	TWeakObjectPtr<UNiagaraNodeFunctionCall> Node;
	FString GraphPath;
	FString NodePath;
	TArray<FString> Classes;
	bool bOrdinaryCollisionQuery = false;
	bool bDistanceField = false;
	bool bAsyncGpuTrace = false;
	bool bEditable = false;
	ENodeEnabledState BeforeState = ENodeEnabledState::Enabled;
	bool bBeforeUserSet = false;
	ENodeEnabledState AfterState = ENodeEnabledState::Enabled;
	bool bAfterUserSet = false;
};

// Async GPU Trace is configured on a data-interface object owned by a
// Niagara input node, rather than on the function-call node that invokes it.
// Keep that state separate so a policy receipt can deduplicate a DI referenced
// by more than one function call while still retaining its owning input node.
struct FCollisionPolicyDataInterfaceState
{
	TWeakObjectPtr<UNiagaraGraph> Graph;
	TWeakObjectPtr<UNiagaraNodeInput> InputNode;
	TWeakObjectPtr<UNiagaraDataInterfaceAsyncGpuTrace> DataInterface;
	FString GraphPath;
	FString InputNodePath;
	FString DataInterfacePath;
	FString InputName;
	FString BeforeProvider;
	FString AfterProvider;
	bool bEditable = false;
	bool bProviderChanged = false;
};

struct FCollisionPolicyGraphState
{
	TWeakObjectPtr<UNiagaraGraph> Graph;
	FString GraphPath;
	FString ChangeId;
	FString AfterChangeId;
	FString ScopeKind;
	FString ScopeName;
	int32 ReferenceDepth = 0;
	bool bEditable = false;
	int32 ManagedNodeCount = 0;
	int32 EditableManagedNodeCount = 0;
	int32 OrdinaryCollisionQueryCount = 0;
	int32 EditableOrdinaryCollisionQueryCount = 0;
};

struct FCollisionPolicyPlanData
{
	TWeakObjectPtr<UNiagaraSystem> System;
	FString SystemPath;
	FString EmitterSelector;
	FString GraphSelector;
	FString Policy;
	bool bAllowMissingAsyncTrace = false;
	bool bAllowDefaultProviderFallback = false;
	bool bAllowUnmanagedOrdinaryCollisionQuery = false;
	bool bAllowUnmanagedDistanceField = false;
	bool bIncludeDisabled = true;
	bool bBlocked = false;
	bool bHasOrdinaryCollisionQuery = false;
	bool bHasEditableOrdinaryCollisionQuery = false;
	bool bHasAsyncGpuTrace = false;
	bool bHasEditableAsyncGpuTrace = false;
	bool bHasEditableAsyncProvider = false;
	bool bDefaultProviderFallbackRisk = false;
	bool bHasDistanceField = false;
	bool bHasEditableDistanceField = false;
	bool bRayTracingEnabled = false;
	bool bRayTracingSupported = false;
	int32 OrdinaryCollisionQueryCount = 0;
	int32 EditableOrdinaryCollisionQueryCount = 0;
	int32 UnmanagedEnabledOrdinaryCollisionQueryCount = 0;
	int32 UnmanagedEnabledDistanceFieldCount = 0;
	TArray<FString> Risks;
	TArray<FString> Warnings;
	TArray<FCollisionPolicyGraphState> Graphs;
	TArray<FCollisionPolicyNodeState> Nodes;
	TArray<FCollisionPolicyDataInterfaceState> DataInterfaces;
};

struct FCollisionPolicyReceipt
{
	FString ReceiptId;
	FString RequestId;
	FString PlanDigest;
	FString SystemPath;
	FString Policy;
	TWeakObjectPtr<UNiagaraSystem> System;
	TArray<FCollisionPolicyGraphState> Graphs;
	TArray<FCollisionPolicyNodeState> Nodes;
	TArray<FCollisionPolicyDataInterfaceState> DataInterfaces;
	int32 OrdinaryCollisionQueryCount = 0;
	int32 EditableOrdinaryCollisionQueryCount = 0;
	int32 UnmanagedEnabledOrdinaryCollisionQueryCount = 0;
	int32 UnmanagedEnabledDistanceFieldCount = 0;
	bool bCompiled = false;
	FString CompileStatus = TEXT("unknown");
	bool bRolledBack = false;
};

TMap<FString, FMutationReceipt>& MutationReceipts()
{
	static TMap<FString, FMutationReceipt> Receipts;
	return Receipts;
}

TMap<FString, FString>& RequestReceiptIds()
{
	static TMap<FString, FString> RequestToReceipt;
	return RequestToReceipt;
}

TMap<FString, FPinMutationReceipt>& PinMutationReceipts()
{
	static TMap<FString, FPinMutationReceipt> Receipts;
	return Receipts;
}

TMap<FString, FString>& PinRequestReceiptIds()
{
	static TMap<FString, FString> RequestToReceipt;
	return RequestToReceipt;
}

TMap<FString, FCollisionPolicyReceipt>& CollisionPolicyReceipts()
{
	static TMap<FString, FCollisionPolicyReceipt> Receipts;
	return Receipts;
}

TMap<FString, FString>& CollisionPolicyRequestReceiptIds()
{
	static TMap<FString, FString> RequestToReceipt;
	return RequestToReceipt;
}

FString EnabledStateName(const ENodeEnabledState State)
{
	switch (State)
	{
	case ENodeEnabledState::Enabled:
		return TEXT("enabled");
	case ENodeEnabledState::Disabled:
		return TEXT("disabled");
	case ENodeEnabledState::DevelopmentOnly:
		return TEXT("developmentOnly");
	default:
		return TEXT("unknown");
	}
}

FString NormalizeSystemObjectPath(const FString& RequestedPath)
{
	FString Path = RequestedPath.TrimStartAndEnd();
	if (Path.IsEmpty() || Path.Contains(TEXT("..")))
	{
		return FString();
	}
	if (!Path.Contains(TEXT(".")))
	{
		const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
		if (!AssetName.IsEmpty())
		{
			Path += TEXT(".");
			Path += AssetName;
		}
	}
	return Path;
}

bool IsProjectSystem(const UNiagaraSystem* System)
{
	return System
		&& System->GetOutermost()
		&& System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));
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
	if (Selector.Equals(Name, ESearchCase::IgnoreCase)
		|| Selector.Equals(ObjectPath, ESearchCase::IgnoreCase))
	{
		return true;
	}
	return ObjectPath.EndsWith(
		FString::Printf(TEXT(".%s"), *Selector),
		ESearchCase::IgnoreCase);
}

// UNiagaraGraph::GetAllReferencedGraphs is intentionally not exported by
// NiagaraEditor in UE 5.4.1. Keep the traversal here so the optional Niagara
// build does not acquire a cross-module linker dependency on a private symbol.
void AddGraphAndReferences(
	UNiagaraGraph* RootGraph,
	const FString& ScopeKind,
	const FString& ScopeName,
	const FString& EmitterPath,
	const FString& ScriptUsage,
	UPackage* SystemPackage,
	TArray<FGraphTarget>& OutGraphs)
{
	if (!RootGraph)
	{
		return;
	}

	TSet<UNiagaraGraph*> Visited;
	TFunction<void(UNiagaraGraph*, int32, const FString&)> VisitGraph =
		[&](UNiagaraGraph* Graph, const int32 Depth, const FString& ReferencedFrom)
	{
		if (!Graph || Visited.Contains(Graph))
		{
			return;
		}
		Visited.Add(Graph);

		FGraphTarget Target;
		Target.Graph = Graph;
		Target.RootGraph = RootGraph;
		Target.ScopeKind = ScopeKind;
		Target.ScopeName = ScopeName;
		Target.EmitterPath = EmitterPath;
		Target.ScriptUsage = ScriptUsage;
		Target.ReferencedFromNodePath = ReferencedFrom;
		Target.ReferenceDepth = Depth;
		Target.bOwnedBySystem = SystemPackage != nullptr
			&& Graph->GetOutermost() == SystemPackage;

		const int32 ExistingIndex = OutGraphs.IndexOfByPredicate(
			[Graph](const FGraphTarget& Existing)
			{
				return Existing.Graph == Graph;
			});
		if (ExistingIndex == INDEX_NONE)
		{
			OutGraphs.Add(MoveTemp(Target));
		}
		else if (Depth < OutGraphs[ExistingIndex].ReferenceDepth)
		{
			// Prefer the shortest path, which is the only path that is safe for
			// a direct graph edit.
			OutGraphs[ExistingIndex] = MoveTemp(Target);
		}

		for (UEdGraphNode* RawNode : Graph->Nodes)
		{
			UNiagaraNodeFunctionCall* FunctionCall =
				Cast<UNiagaraNodeFunctionCall>(RawNode);
			if (!FunctionCall)
			{
				continue;
			}
			UNiagaraGraph* CalledGraph = FunctionCall->GetCalledGraph();
			if (CalledGraph)
			{
				VisitGraph(CalledGraph, Depth + 1, FunctionCall->GetPathName());
			}
		}
	};

	VisitGraph(RootGraph, 0, FString());
}

bool LoadSystem(
	const FString& RequestedPath,
	UNiagaraSystem*& OutSystem,
	FString& OutObjectPath,
	FString& OutErrorCode,
	FString& OutError)
{
	OutSystem = nullptr;
	OutObjectPath = NormalizeSystemObjectPath(RequestedPath);
	if (OutObjectPath.IsEmpty())
	{
		OutErrorCode = TEXT("invalid_system_path");
		OutError = TEXT("system must be a non-empty Niagara System object or package path.");
		return false;
	}

	OutSystem = LoadObject<UNiagaraSystem>(nullptr, *OutObjectPath);
	if (!OutSystem)
	{
		OutErrorCode = TEXT("system_not_found");
		OutError = FString::Printf(
			TEXT("Niagara System '%s' was not found."),
			*RequestedPath);
		return false;
	}
	return true;
}

bool CollectGraphs(
	UNiagaraSystem* System,
	const FString& EmitterSelector,
	const FString& GraphSelector,
	TArray<FGraphTarget>& OutGraphs,
	FString& OutErrorCode,
	FString& OutError)
{
	OutGraphs.Reset();
	if (!System)
	{
		OutErrorCode = TEXT("system_not_found");
		OutError = TEXT("The Niagara System is unavailable.");
		return false;
	}

	// An explicit emitter selector scopes the operation to emitter graphs. With
	// no emitter selector, include the system spawn/update graphs as well.
	if (EmitterSelector.IsEmpty())
	{
		const auto AddSystemScript =
			[&OutGraphs, System](UNiagaraScript* Script, const TCHAR* Usage)
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
					TEXT("system"),
					TEXT("system"),
					FString(),
					Usage,
					System->GetOutermost(),
					OutGraphs);
			}
		};
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
		if (!Source || !Source->NodeGraph)
		{
			continue;
		}
		AddGraphAndReferences(
			Source->NodeGraph,
			TEXT("emitter"),
			EmitterName,
			EmitterPath,
			TEXT("emitter"),
			System->GetOutermost(),
			OutGraphs);
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

	if (OutGraphs.IsEmpty())
	{
		OutErrorCode = GraphSelector.IsEmpty()
			? TEXT("graph_not_found")
			: TEXT("graph_not_found");
		OutError = GraphSelector.IsEmpty()
			? TEXT("The Niagara System has no loaded editor graph matching the requested scope.")
			: FString::Printf(
				TEXT("Niagara graph '%s' was not found in the requested system scope."),
				*GraphSelector);
		return false;
	}
	return true;
}

bool ContainsAnyToken(const FString& Value, const TArray<FString>& Tokens)
{
	for (const FString& Token : Tokens)
	{
		if (Value.Contains(Token, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

//++[UEAI] Begin implementation
// Match a module/function identity without treating a longer operation name
// (for example RayTraceDistanceField_GPU) as the generic RayTrace helper.
bool HasExactOperationPathOrName(
	const FString& FunctionName,
	const FString& SignatureName,
	const FString& FunctionScript,
	const TCHAR* Token)
{
	if (!Token)
	{
		return false;
	}
	const FString DottedToken = FString::Printf(TEXT("%s.%s"), Token, Token);
	if (FunctionName.Equals(Token, ESearchCase::IgnoreCase)
		|| SignatureName.Equals(Token, ESearchCase::IgnoreCase)
		|| FunctionName.Equals(DottedToken, ESearchCase::IgnoreCase)
		|| SignatureName.Equals(DottedToken, ESearchCase::IgnoreCase))
	{
		return true;
	}
	const FString PathSuffix = FString::Printf(TEXT("/%s"), Token);
	const FString ObjectSuffix = FString::Printf(TEXT(".%s"), Token);
	return FunctionScript.EndsWith(PathSuffix, ESearchCase::IgnoreCase)
		|| FunctionScript.EndsWith(ObjectSuffix, ESearchCase::IgnoreCase);
}
//--[UEAI] End implementation

//++[UEAI] Begin implementation
// Collision module names share a "Collision" prefix (CollisionRest,
// CollisionLinearImpulse, and CollisionQueryAndResponse). Match the module
// identity itself, including its dotted asset name, instead of searching for
// a prefix in the complete function metadata string.
bool HasExactCollisionModuleIdentity(
	const FString& FunctionName,
	const FString& SignatureName,
	const FString& FunctionScript,
	const TCHAR* ModuleName,
	const TCHAR* DottedModuleName,
	const TCHAR* AssetPathSuffix)
{
	if (!ModuleName || !DottedModuleName || !AssetPathSuffix)
	{
		return false;
	}
	const FString PackagePathSuffix = FString::Printf(
		TEXT("/Collision/%s"),
		ModuleName);
	return HasExactOperationPathOrName(
		FunctionName,
		SignatureName,
		FunctionScript,
		ModuleName)
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			DottedModuleName)
		|| FunctionScript.EndsWith(
			AssetPathSuffix,
			ESearchCase::IgnoreCase)
		|| FunctionScript.EndsWith(
			PackagePathSuffix,
			ESearchCase::IgnoreCase);
}
//--[UEAI] End implementation

FString GetPinPath(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return FString();
	}
	const UEdGraphNode* OwningNode = Pin->GetOwningNodeUnchecked();
	const FString PinId = Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower);
	return OwningNode
		? FString::Printf(TEXT("%s:%s"), *OwningNode->GetPathName(), *PinId)
		: PinId;
}

UEdGraphPin* FindParameterMapPin(UNiagaraNode* Node, const EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}
	TArray<UEdGraphPin*> Pins;
	if (Direction == EGPD_Input)
	{
		Node->GetInputPins(Pins);
	}
	else
	{
		Node->GetOutputPins(Pins);
	}
	for (UEdGraphPin* Pin : Pins)
	{
		if (Pin && Node->IsParameterMapPin(Pin))
		{
			return Pin;
		}
	}
	return nullptr;
}

void ClassifyNode(
	const UNiagaraNode* Node,
	TArray<FString>& OutClasses,
	bool& bOutOrdinaryCollisionQuery,
	bool& bOutDistanceField,
	bool& bOutAsyncGpuTrace)
{
	bOutOrdinaryCollisionQuery = false;
	bOutDistanceField = false;
	bOutAsyncGpuTrace = false;
	OutClasses.Reset();
	const UNiagaraNodeFunctionCall* FunctionCall =
		Cast<UNiagaraNodeFunctionCall>(Node);
	if (!FunctionCall)
	{
		return;
	}

	const FString FunctionName = FunctionCall->GetFunctionName();
	const FString SignatureName = FunctionCall->Signature.Name.ToString();
	const FString FunctionScript = FunctionCall->FunctionScript
		? FunctionCall->FunctionScript->GetPathName()
		: FunctionCall->FunctionScriptAssetObjectPath.ToString();
	const FString SearchText = FString::Join(
		TArray<FString>{FunctionName, SignatureName, FunctionScript},
		TEXT(" "));

	// Keep this list limited to collision providers and collision-oriented
	// queries. A generic "DistanceField" substring also occurs in unrelated
	// data interfaces such as WindField, and must not be disabled by the
	// hardware-ray-tracing collision policy.
	const bool bNamedDistanceFieldCollision = ContainsAnyToken(
		SearchText,
		{
			TEXT("NiagaraDistanceFieldCollisions"),
			TEXT("QueryMeshDistanceFieldGPU"),
			TEXT("QueryDistanceField"),
			TEXT("GetElementPointMeshDistanceFieldNoNormal"),
			TEXT("GetClosestPointMeshDistanceField"),
			TEXT("GetClosestPointMeshDistanceFieldAccurate"),
			TEXT("GetClosestPointMeshDistanceFieldNoNormal"),
			TEXT("GetMaxEncodedDistanceMeshDistanceField"),
			TEXT("Fn_SphereTraceDistanceField"),
			TEXT("AvoidDistanceFieldSurfaces_GPU"),
			TEXT("MoveToNearestDistanceFieldSurface_GPU"),
			TEXT("FindNearestDistanceFieldSurface_GPU"),
			TEXT("RayTraceDistanceField_GPU"),
			TEXT("ReadDistanceField_GPU"),
			TEXT("SphereTraceDistanceField_GPU"),
			TEXT("SphereCast_GlobalDistanceField"),
			TEXT("CalculateTheGlobalDistanceFieldSurfaceNormal_GPU"),
			TEXT("CalculateGlobalDistanceFieldSurfaceNormal"),
			TEXT("CalculateGlobalDistanceFieldIsoSurfaceNormal"),
			TEXT("FindNearestDistanceFieldIsoSurface"),
			TEXT("FindDistanceFieldVolumeTextureGradient"),
			TEXT("CalculateA_VolumeTexturesDistanceFieldGradient")
		});
	// Some nested calls in the shipped module graphs retain the unsuffixed
	// function identity even though their FunctionScript points at the _GPU
	// asset. Match those identities exactly so unrelated helper names do not
	// become policy-managed merely because they contain the same words.
	const bool bExactDistanceFieldHelper =
		HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("FindNearestDistanceFieldSurface"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CalculateTheGlobalDistanceFieldSurfaceNormal"));
	const bool bRigidMeshInterface = ContainsAnyToken(
		SearchText,
		{
			TEXT("UNiagaraDataInterfaceRigidMeshCollisionQuery"),
			TEXT("NiagaraDataInterfaceRigidMeshCollisionQuery"),
			TEXT("RigidMeshCollisionQuery")
		});
	const bool bRigidMeshOperation = ContainsAnyToken(
		SearchText,
		{
			TEXT("FindActors"),
			TEXT("GetBoxElementsStartIndex"),
			TEXT("GetSphereElementsStartIndex"),
			TEXT("GetCapsuleElementsStartIndex"),
			TEXT("GetSphereRadius"),
			TEXT("GetCapsuleSize"),
			TEXT("GetBoxSize"),
			TEXT("IsWorldPositionInsideCombinedBounds"),
			TEXT("GetClosestPointSimple"),
			TEXT("GetElementPointMeshDistanceFieldNoNormal"),
			TEXT("GetClosestPointMeshDistanceField"),
			TEXT("GetClosestPointMeshDistanceFieldAccurate"),
			TEXT("GetClosestPointMeshDistanceFieldNoNormal"),
			TEXT("GetMaxEncodedDistanceMeshDistanceField")
		});
	const bool bRigidMeshSharedOperation = bRigidMeshInterface
		&& ContainsAnyToken(
			SearchText,
			{
				// These names are shared with PhysicsAsset and are only
				// attributed to RigidMesh when its marker is present.
				TEXT("GetNumBoxes"),
				TEXT("GetNumSpheres"),
				TEXT("GetNumCapsules"),
				TEXT("GetNumElements"),
				TEXT("GetClosestElement"),
				TEXT("GetElementPoint"),
				TEXT("GetElementDistance"),
				TEXT("GetClosestPoint"),
				TEXT("GetClosestDistance")
			});
	const bool bRigidMeshCollisionQuery = bRigidMeshInterface
		|| bRigidMeshOperation
		|| bRigidMeshSharedOperation;
	const bool bPhysicsAssetInterface = ContainsAnyToken(
		SearchText,
		{
			TEXT("UNiagaraDataInterfacePhysicsAsset"),
			TEXT("NiagaraDataInterfacePhysicsAsset"),
			TEXT("PhysicsAsset")
		});
	const bool bPhysicsAssetOperation = ContainsAnyToken(
		SearchText,
		{
			TEXT("GetRestDistance"),
			TEXT("GetTexturePoint"),
			TEXT("GetProjectionPoint")
		});
	const bool bPhysicsAssetSharedOperation = bPhysicsAssetInterface
		&& ContainsAnyToken(
			SearchText,
			{
				// Shared operation names are attributed to PhysicsAsset only
				// when the signature/path retains its provider marker.
				TEXT("GetNumBoxes"),
				TEXT("GetNumSpheres"),
				TEXT("GetNumCapsules"),
				TEXT("GetClosestElement"),
				TEXT("GetElementPoint"),
				TEXT("GetElementDistance"),
				TEXT("GetClosestPoint"),
				TEXT("GetClosestDistance")
			});
	const bool bPhysicsAsset = bPhysicsAssetInterface
		|| bPhysicsAssetOperation
		|| bPhysicsAssetSharedOperation;
	// Analytical collision and helper modules consume collision state but do not
	// provide the legacy world CollisionQuery path. Keep them visible to graph
	// inspection while preventing hardware-ray-tracing policy from disabling
	// them as ordinary providers.
	const bool bAnalyticalCollision =
		HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("AnalyticalCollisionQuery"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("PlaneSphereCollisionDetection"));
	const bool bCollisionSupport =
		HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("RandomizeCollisionNormals"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("SetupRigidBodyDI"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("RayTrace"))
		// Collision helper/event modules consume collision state but do not
		// provide the ordinary world CollisionQuery provider.
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("AddRotationalVelocity"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CalculateLinePlaneInt"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("DebugCollisionEvents"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("FindTangentialVelocityOnSphere"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("InitialRotationalVelocity"))
		//++[UEAI] Begin implementation
		// Neighbor update modules participate in the collision solve, but they
		// are support operations rather than world-query providers.
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CalculateNeighbors"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("SampleNeighbors"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("NeighborBehaviours"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("NeighborBehaviors"))
		//--[UEAI] End implementation
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("InitializeNeighborGrid"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("PBD_IntraParticleCollision"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("PopulateNeighborGrid"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("GenerateCollisionEvent"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("ReceiveCollisionEvent"));
	const bool bCollisionPathDistanceField =
		SearchText.Contains(TEXT("DistanceField"), ESearchCase::IgnoreCase)
		&& SearchText.Contains(TEXT("Collision"), ESearchCase::IgnoreCase);
	bOutDistanceField = bNamedDistanceFieldCollision
		|| bExactDistanceFieldHelper
		|| bCollisionPathDistanceField;
	bOutAsyncGpuTrace = ContainsAnyToken(
		SearchText,
		{
			TEXT("AsyncGpuTrace"),
			TEXT("AsyncGPUTrace"),
			// Status probes are generated by the AsyncGpuTrace data interface and
			// must follow the same provider policy as its trace calls.
			TEXT("IsAsyncGpuTraceReadyGpu"),
			TEXT("IsHardwareRayTracingEnabledGpu"),
			TEXT("IsHardwareRayTracingAvailableGpu"),
			TEXT("AsyncRayTrace"),
			TEXT("IssueAsyncRayTrace"),
			TEXT("CreateAsyncRayTrace"),
			TEXT("ReserveAsyncRayTrace"),
			// Generated UE 5.4 function-call wrappers delegate to the async
			// trace reserve VM operation under this display name.
			TEXT("ReserveRayTraceIndex"),
			TEXT("ReadAsyncRayTrace"),
			// UE 5.4 keeps these deprecated signatures with a Gpu suffix.
			TEXT("IssueAsyncRayTraceGpu"),
			TEXT("CreateAsyncRayTraceGpu"),
			TEXT("ReserveAsyncRayTraceGpu"),
			TEXT("ReadAsyncRayTraceGpu")
		});
	const bool bDepthBufferCollisionQuery = ContainsAnyToken(
		SearchText,
		{
			TEXT("QuerySceneDepthGPU"),
			TEXT("QueryScenePartialDepthGPU"),
			TEXT("QueryCustomDepthGPU"),
			TEXT("SceneDepthTest"),
			TEXT("PlaceParticlesOnDepthBuffer_GPU")
		});
	// Collision modules can be represented by a function name, a signature name,
	// or an object path. Keep all of the shipped aliases here so graph.inspect,
	// collision.audit, and collision.policy see the same node classification.
	// Match only the two ordinary world-query module identities. The old
	// substring check for "CollisionQuery" also matched the response module,
	// while "/Collision/Collision" matched CollisionRest and
	// CollisionLinearImpulse through their shared path prefix.
	const bool bCollisionModule = HasExactCollisionModuleIdentity(
		FunctionName,
		SignatureName,
		FunctionScript,
		TEXT("Collision"),
		TEXT("Collision.Collision"),
		TEXT("/Collision/Collision.Collision"));
	const bool bCollisionQueryModule = HasExactCollisionModuleIdentity(
		FunctionName,
		SignatureName,
		FunctionScript,
		TEXT("CollisionQuery"),
		TEXT("CollisionQuery.CollisionQuery"),
		TEXT("/Collision/CollisionQuery.CollisionQuery"));
	const bool bCollisionQueryAndResponseModule =
		// This composite module performs a CPU query and a response solve; keep
		// it policy-managed while matching its name exactly.
		HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CollisionQueryAndResponse"));
	const bool bExplicitCpuCollisionQuery =
		bCollisionModule
		|| bCollisionQueryModule
		|| bCollisionQueryAndResponseModule
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("PerformCollisionQuerySyncCPU"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("PerformCollisionQueryAsyncCPU"));
	const bool bCollisionResponseOnly =
		HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("AlignParticlesWithCollisionPlane"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CollisionResponse"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CollisionRest"))
		|| HasExactOperationPathOrName(
			FunctionName,
			SignatureName,
			FunctionScript,
			TEXT("CollisionLinearImpulse"));
	const bool bCollisionResponse =
		bCollisionModule
		|| bCollisionQueryAndResponseModule
		|| bCollisionResponseOnly;
	bOutOrdinaryCollisionQuery =
		!bOutDistanceField
		&& !bOutAsyncGpuTrace
		&& !bDepthBufferCollisionQuery
		&& !bRigidMeshCollisionQuery
		&& !bPhysicsAsset
		&& !bAnalyticalCollision
		&& !bCollisionSupport
		&& !bCollisionResponseOnly
		&& bExplicitCpuCollisionQuery;

	if (bOutOrdinaryCollisionQuery)
	{
		OutClasses.Add(TEXT("ordinaryCollisionQuery"));
	}
	if (bOutDistanceField)
	{
		OutClasses.Add(TEXT("distanceField"));
	}
	if (bOutAsyncGpuTrace)
	{
		OutClasses.Add(TEXT("asyncGpuTrace"));
	}
	if (bDepthBufferCollisionQuery)
	{
		OutClasses.Add(TEXT("depthBufferCollisionQuery"));
	}
	if (bCollisionResponse)
	{
		OutClasses.Add(TEXT("collisionResponse"));
	}
	if (bRigidMeshCollisionQuery)
	{
		OutClasses.Add(TEXT("rigidMeshCollisionQuery"));
	}
	if (bPhysicsAsset)
	{
		OutClasses.Add(TEXT("physicsAsset"));
	}
	if (bAnalyticalCollision)
	{
		OutClasses.Add(TEXT("analyticalCollision"));
	}
	if (bCollisionSupport)
	{
		OutClasses.Add(TEXT("collisionSupport"));
	}
}

TSharedRef<FJsonObject> SerializePin(const UNiagaraNode* Node, const UEdGraphPin* Pin)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Node || !Pin)
	{
		return Result;
	}
	Result->SetStringField(TEXT("path"), GetPinPath(Pin));
	Result->SetStringField(
		TEXT("pinId"),
		Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Result->SetStringField(TEXT("name"), Pin->PinName.ToString());
	Result->SetStringField(
		TEXT("direction"),
		Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	Result->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
	Result->SetStringField(TEXT("subtype"), Pin->PinType.PinSubCategory.ToString());
	if (Pin->PinType.PinSubCategoryObject.IsValid())
	{
		Result->SetStringField(
			TEXT("subtypeObject"),
			Pin->PinType.PinSubCategoryObject->GetPathName());
	}
	Result->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
	Result->SetStringField(
		TEXT("autogeneratedDefaultValue"),
		Pin->AutogeneratedDefaultValue);
	Result->SetStringField(
		TEXT("persistentGuid"),
		Pin->PersistentGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	Result->SetBoolField(TEXT("linked"), Pin->LinkedTo.Num() > 0);
	Result->SetNumberField(TEXT("linkCount"), Pin->LinkedTo.Num());
	Result->SetBoolField(TEXT("parameterMap"), Node->IsParameterMapPin(Pin));
	Result->SetBoolField(TEXT("readOnly"), Pin->bDefaultValueIsReadOnly);
	Result->SetBoolField(TEXT("defaultIgnored"), Pin->bDefaultValueIsIgnored);
	TArray<TSharedPtr<FJsonValue>> LinkedPins;
	for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
	{
		if (LinkedPin)
		{
			LinkedPins.Add(MakeShared<FJsonValueString>(GetPinPath(LinkedPin)));
		}
	}
	Result->SetArrayField(TEXT("linkedPins"), LinkedPins);
	return Result;
}

TSharedRef<FJsonObject> SerializeNode(const UNiagaraNode* Node)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Node)
	{
		return Result;
	}
	Result->SetStringField(TEXT("path"), Node->GetPathName());
	Result->SetStringField(
		TEXT("nodeGuid"),
		Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	Result->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
	Result->SetNumberField(TEXT("nodePosX"), Node->NodePosX);
	Result->SetNumberField(TEXT("nodePosY"), Node->NodePosY);
	Result->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());
	Result->SetStringField(
		TEXT("desiredEnabledState"),
		EnabledStateName(Node->GetDesiredEnabledState()));
	Result->SetBoolField(
		TEXT("userSetEnabledState"),
		Node->HasUserSetTheEnabledState());

	TArray<TSharedPtr<FJsonValue>> PinValues;
	const int32 PinCount = FMath::Min(Node->Pins.Num(), MaxPinLimitPerNode);
	for (int32 PinIndex = 0; PinIndex < PinCount; ++PinIndex)
	{
		if (const UEdGraphPin* Pin = Node->Pins[PinIndex])
		{
			PinValues.Add(MakeShared<FJsonValueObject>(SerializePin(Node, Pin)));
		}
	}
	Result->SetArrayField(TEXT("pins"), PinValues);
	Result->SetNumberField(TEXT("pinTotal"), Node->Pins.Num());
	Result->SetBoolField(TEXT("pinsTruncated"), Node->Pins.Num() > PinCount);

	if (const UNiagaraNodeFunctionCall* FunctionCall =
		Cast<UNiagaraNodeFunctionCall>(Node))
	{
		Result->SetBoolField(TEXT("isFunctionCall"), true);
		Result->SetStringField(TEXT("functionName"), FunctionCall->GetFunctionName());
		Result->SetStringField(
			TEXT("signatureName"),
			FunctionCall->Signature.Name.ToString());
		Result->SetStringField(
			TEXT("signatureOwner"),
			FunctionCall->Signature.OwnerName.ToString());
		const FString FunctionScript = FunctionCall->FunctionScript
			? FunctionCall->FunctionScript->GetPathName()
			: FunctionCall->FunctionScriptAssetObjectPath.ToString();
		Result->SetStringField(TEXT("functionScript"), FunctionScript);
		TArray<FString> Classes;
		bool bOrdinaryCollisionQuery = false;
		bool bDistanceField = false;
		bool bAsyncGpuTrace = false;
		ClassifyNode(
			Node,
			Classes,
			bOrdinaryCollisionQuery,
			bDistanceField,
			bAsyncGpuTrace);
		TArray<TSharedPtr<FJsonValue>> ClassValues;
		for (const FString& Class : Classes)
		{
			ClassValues.Add(MakeShared<FJsonValueString>(Class));
		}
		Result->SetArrayField(TEXT("classifications"), ClassValues);
		TArray<FString> Operations;
		TArray<FString> OperationInterfaceKinds;
		UEAINiagaraGraphAuditExtensions::GetCollisionOperationNames(
			FunctionCall,
			Operations,
			OperationInterfaceKinds);
		TArray<TSharedPtr<FJsonValue>> OperationValues;
		OperationValues.Reserve(Operations.Num());
		for (const FString& Operation : Operations)
		{
			OperationValues.Add(MakeShared<FJsonValueString>(Operation));
		}
		Result->SetArrayField(TEXT("operations"), OperationValues);
		TArray<TSharedPtr<FJsonValue>> OperationKindValues;
		for (const FString& InterfaceKind : OperationInterfaceKinds)
		{
			OperationKindValues.Add(MakeShared<FJsonValueString>(InterfaceKind));
		}
		Result->SetArrayField(TEXT("operationInterfaceKinds"), OperationKindValues);
		Result->SetBoolField(TEXT("ordinaryCollisionQuery"), bOrdinaryCollisionQuery);
		Result->SetBoolField(TEXT("distanceFieldUsage"), bDistanceField);
		Result->SetBoolField(TEXT("asyncGpuTraceUsage"), bAsyncGpuTrace);
		Result->SetBoolField(
			TEXT("rigidMeshCollisionQueryUsage"),
			Classes.Contains(TEXT("rigidMeshCollisionQuery")));
		Result->SetBoolField(
			TEXT("physicsAssetUsage"),
			Classes.Contains(TEXT("physicsAsset")));
		if (const UNiagaraGraph* CalledGraph = FunctionCall->GetCalledGraph())
		{
			Result->SetStringField(TEXT("calledGraph"), CalledGraph->GetPathName());
		}
	}
	else
	{
		Result->SetBoolField(TEXT("isFunctionCall"), false);
	}
	return Result;
}

bool ResolveNode(
	const TSharedPtr<FJsonObject>& Params,
	UNiagaraSystem* System,
	FNodeTarget& OutTarget,
	FString& OutErrorCode,
	FString& OutError,
	const bool bRequireFunctionCall = true)
{
	FString EmitterSelector;
	FString GraphSelector;
	FString NodePath;
	FString NodeGuidText;
	Params->TryGetStringField(TEXT("emitter"), EmitterSelector);
	Params->TryGetStringField(TEXT("graph"), GraphSelector);
	Params->TryGetStringField(TEXT("nodePath"), NodePath);
	Params->TryGetStringField(TEXT("nodeGuid"), NodeGuidText);
	EmitterSelector = EmitterSelector.TrimStartAndEnd();
	GraphSelector = GraphSelector.TrimStartAndEnd();
	NodePath = NodePath.TrimStartAndEnd();
	NodeGuidText = NodeGuidText.TrimStartAndEnd();

	const bool bHasNodePath = !NodePath.IsEmpty();
	const bool bHasNodeGuid = !NodeGuidText.IsEmpty();
	if (bHasNodePath == bHasNodeGuid)
	{
		OutErrorCode = TEXT("invalid_node_selector");
		OutError = TEXT("Provide exactly one of nodePath or nodeGuid.");
		return false;
	}

	FGuid RequestedGuid;
	if (bHasNodeGuid
		&& !FGuid::Parse(NodeGuidText, RequestedGuid))
	{
		OutErrorCode = TEXT("invalid_node_guid");
		OutError = TEXT("nodeGuid must be a valid GUID.");
		return false;
	}

	TArray<FGraphTarget> Graphs;
	if (!CollectGraphs(
		System,
		EmitterSelector,
		GraphSelector,
		Graphs,
		OutErrorCode,
		OutError))
	{
		return false;
	}

	TArray<FNodeTarget> Matches;
	for (const FGraphTarget& GraphTarget : Graphs)
	{
		if (!GraphTarget.Graph)
		{
			continue;
		}
		for (UEdGraphNode* RawNode : GraphTarget.Graph->Nodes)
		{
			UNiagaraNode* Node = Cast<UNiagaraNode>(RawNode);
			if (!Node)
			{
				continue;
			}
			const bool bMatches = bHasNodeGuid
				? Node->NodeGuid == RequestedGuid
				: MatchesSelector(
					NodePath,
					Node->GetName(),
					Node->GetPathName());
			if (bMatches)
			{
				FNodeTarget Match;
				Match.Graph = GraphTarget;
				Match.Node = Node;
				Match.FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node);
				Matches.Add(MoveTemp(Match));
			}
		}
	}

	if (Matches.IsEmpty())
	{
		OutErrorCode = TEXT("node_not_found");
		OutError = bHasNodeGuid
			? FString::Printf(
				TEXT("Niagara node GUID '%s' was not found."),
				*NodeGuidText)
			: FString::Printf(
				TEXT("Niagara node '%s' was not found."),
				*NodePath);
		return false;
	}
	if (Matches.Num() != 1)
	{
		OutErrorCode = TEXT("node_ambiguous");
		OutError = TEXT("The node selector matched more than one Niagara node; provide a canonical nodePath and graph scope.");
		return false;
	}
	if (bRequireFunctionCall && !Matches[0].FunctionCall)
	{
		OutErrorCode = TEXT("unsupported_node_type");
		OutError = TEXT("Only Niagara function-call nodes support enabled-state mutation.");
		return false;
	}
	OutTarget = Matches[0];
	return true;
}

bool ResolvePin(
	const TSharedPtr<FJsonObject>& Params,
	UNiagaraSystem* System,
	FPinTarget& OutTarget,
	FString& OutErrorCode,
	FString& OutError)
{
	FString PinName;
	if (!Params->TryGetStringField(TEXT("pinName"), PinName)
		|| PinName.TrimStartAndEnd().IsEmpty())
	{
		OutErrorCode = TEXT("invalid_pin_selector");
		OutError = TEXT("pinName is required.");
		return false;
	}
	PinName = PinName.TrimStartAndEnd();

	FNodeTarget NodeTarget;
	if (!ResolveNode(
		Params,
		System,
		NodeTarget,
		OutErrorCode,
		OutError,
		false))
	{
		return false;
	}

	TArray<UEdGraphPin*> Matches;
	for (UEdGraphPin* Pin : NodeTarget.Node->Pins)
	{
		if (Pin
			&& Pin->Direction == EGPD_Input
			&& (Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)
				|| GetPinPath(Pin).Equals(PinName, ESearchCase::IgnoreCase)))
		{
			Matches.Add(Pin);
		}
	}
	if (Matches.IsEmpty())
	{
		OutErrorCode = TEXT("pin_not_found");
		OutError = FString::Printf(
			TEXT("Input pin '%s' was not found on Niagara node '%s'."),
			*PinName,
			*NodeTarget.Node->GetPathName());
		return false;
	}
	if (Matches.Num() != 1)
	{
		OutErrorCode = TEXT("pin_ambiguous");
		OutError = TEXT("The pin selector matched more than one Niagara input pin.");
		return false;
	}

	UEdGraphPin* Pin = Matches[0];
	if (NodeTarget.Node->IsParameterMapPin(Pin))
	{
		OutErrorCode = TEXT("unsupported_pin_type");
		OutError = TEXT("Parameter-map pins do not have editable default values.");
		return false;
	}
	if (Pin->LinkedTo.Num() > 0)
	{
		OutErrorCode = TEXT("pin_linked");
		OutError = TEXT("Linked Niagara input pins must be disconnected before changing their default value.");
		return false;
	}
	if (Pin->bDefaultValueIsReadOnly || Pin->bDefaultValueIsIgnored)
	{
		OutErrorCode = TEXT("pin_read_only");
		OutError = TEXT("The selected Niagara input pin does not accept a default value.");
		return false;
	}

	OutTarget.Node = NodeTarget;
	OutTarget.Pin = Pin;
	return true;
}

// SetModuleIsEnabled is exported, but its GetStackNodeGroups helper is not.
// Mirror the helper's graph walk locally so malformed or nested function graphs
// are rejected before the exported function reaches its checkf-based traversal.
bool IsTopLevelStackModule(UNiagaraGraph* Graph, UNiagaraNodeFunctionCall* Target)
{
	if (!Graph || !Target || Target->GetNiagaraGraph() != Graph)
	{
		return false;
	}

	// Find the output node reachable from the target. This follows the same
	// all-output-pin search used by Niagara's private stack utility.
	UNiagaraNodeOutput* OutputNode = nullptr;
	TArray<UNiagaraNode*> NodesToCheck;
	TSet<UNiagaraNode*> NodesSeen;
	NodesToCheck.Add(Target);
	NodesSeen.Add(Target);
	while (NodesToCheck.Num() > 0 && !OutputNode)
	{
		UNiagaraNode* NodeToCheck = NodesToCheck[0];
		NodesToCheck.RemoveAt(0);
		if (UNiagaraNodeOutput* Candidate = Cast<UNiagaraNodeOutput>(NodeToCheck))
		{
			OutputNode = Candidate;
			break;
		}
		TArray<UEdGraphPin*> OutputPins;
		NodeToCheck->GetOutputPins(OutputPins);
		for (UEdGraphPin* OutputPin : OutputPins)
		{
			if (!OutputPin)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : OutputPin->LinkedTo)
			{
				UNiagaraNode* LinkedNode = LinkedPin
					? Cast<UNiagaraNode>(LinkedPin->GetOwningNodeUnchecked())
					: nullptr;
				if (LinkedNode
					&& LinkedNode->GetNiagaraGraph() == Graph
					&& !NodesSeen.Contains(LinkedNode))
				{
					NodesSeen.Add(LinkedNode);
					NodesToCheck.Add(LinkedNode);
				}
			}
		}
	}
	if (!OutputNode)
	{
		return false;
	}

	// Build the ordered parameter-map chain from the output back to its input.
	// The first and last groups in Niagara's stack representation are input and
	// output; a target module must be an interior element of this chain.
	TArray<UNiagaraNodeFunctionCall*> OrderedModules;
	TSet<UNiagaraNode*> ChainSeen;
	UNiagaraNode* PreviousNode = OutputNode;
	bool bReachedInput = false;
	while (PreviousNode && !ChainSeen.Contains(PreviousNode))
	{
		ChainSeen.Add(PreviousNode);
		if (PreviousNode->IsA<UNiagaraNodeInput>())
		{
			bReachedInput = true;
			break;
		}
		UEdGraphPin* ParameterMapInput = FindParameterMapPin(PreviousNode, EGPD_Input);
		if (!ParameterMapInput || ParameterMapInput->LinkedTo.Num() != 1)
		{
			break;
		}
		UNiagaraNode* CurrentNode = Cast<UNiagaraNode>(
			ParameterMapInput->LinkedTo[0]->GetOwningNodeUnchecked());
		if (!CurrentNode || CurrentNode->GetNiagaraGraph() != Graph)
		{
			break;
		}
		if (UNiagaraNodeFunctionCall* ModuleNode = Cast<UNiagaraNodeFunctionCall>(CurrentNode))
		{
			OrderedModules.Insert(ModuleNode, 0);
		}
		PreviousNode = CurrentNode;
	}

	return bReachedInput && OrderedModules.Contains(Target);
}

TSharedRef<FJsonObject> SerializeGraphScope(const FGraphTarget& Target)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!Target.Graph)
	{
		return Result;
	}
	Result->SetStringField(TEXT("path"), Target.Graph->GetPathName());
	Result->SetStringField(TEXT("name"), Target.Graph->GetName());
	Result->SetStringField(TEXT("scopeKind"), Target.ScopeKind);
	Result->SetStringField(TEXT("scopeName"), Target.ScopeName);
	Result->SetStringField(TEXT("emitterPath"), Target.EmitterPath);
	Result->SetStringField(TEXT("scriptUsage"), Target.ScriptUsage);
	Result->SetStringField(
		TEXT("rootGraph"),
		Target.RootGraph ? Target.RootGraph->GetPathName() : FString());
	Result->SetNumberField(TEXT("referenceDepth"), Target.ReferenceDepth);
	Result->SetStringField(TEXT("referencedFromNode"), Target.ReferencedFromNodePath);
	Result->SetStringField(
		TEXT("package"),
		Target.Graph->GetOutermost() ? Target.Graph->GetOutermost()->GetName() : FString());
	Result->SetBoolField(TEXT("ownedBySystem"), Target.bOwnedBySystem);
	const UPackage* GraphPackage = Target.Graph->GetOutermost();
	Result->SetBoolField(
		TEXT("editable"),
		Target.bOwnedBySystem
			&& GraphPackage != nullptr
			&& GraphPackage->GetName().StartsWith(TEXT("/Game/")));
	Result->SetStringField(
		TEXT("changeId"),
		Target.Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower));
	return Result;
}

// Forward declarations for provider-state serialization helpers defined with
// the collision-policy implementation below.
TArray<FString> GetAsyncTraceProviderOrder();
bool IsDefaultAsyncProviderFallbackRisk();
void SetJsonStringArray(
	const TSharedRef<FJsonObject>& Object,
	const FString& Field,
	const TArray<FString>& Values);

TSharedRef<FJsonObject> SerializeRayTracingState()
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	const IConsoleManager& ConsoleManager = IConsoleManager::Get();
	// IsRayTracingEnabled() asserts until RenderCore has initialized the RHI.
	// Graph inspection can be called during editor startup or an RHI restart,
	// so expose a conservative false value until that lifecycle point.
	const bool bRayTracingEnabled = GIsRHIInitialized && IsRayTracingEnabled();
	const IConsoleVariable* RayTracing =
		ConsoleManager.FindConsoleVariable(TEXT("r.RayTracing"));
	const IConsoleVariable* RayTracingEnable =
		ConsoleManager.FindConsoleVariable(TEXT("r.RayTracing.Enable"));
	const IConsoleVariable* RayTracingEditor =
		ConsoleManager.FindConsoleVariable(TEXT("r.RayTracing.EnableInEditor"));
	const IConsoleVariable* MeshDistanceFields =
		ConsoleManager.FindConsoleVariable(TEXT("r.GenerateMeshDistanceFields"));
	const IConsoleVariable* AsyncHwrt =
		ConsoleManager.FindConsoleVariable(TEXT("fx.Niagara.AsyncGpuTrace.HWRayTraceEnabled"));
	const IConsoleVariable* AsyncGsdf =
		ConsoleManager.FindConsoleVariable(TEXT("fx.Niagara.AsyncGpuTrace.GlobalSdfEnabled"));
	const int32 RayTracingValue = RayTracing ? RayTracing->GetInt() : -1;
	const int32 RayTracingEnableValue = RayTracingEnable ? RayTracingEnable->GetInt() : -1;
	const int32 RayTracingEditorValue = RayTracingEditor ? RayTracingEditor->GetInt() : -1;
	const int32 MeshDistanceFieldsValue = MeshDistanceFields ? MeshDistanceFields->GetInt() : -1;
	Result->SetNumberField(TEXT("rRayTracing"), RayTracingValue);
	Result->SetNumberField(TEXT("rRayTracingEnable"), RayTracingEnableValue);
	Result->SetNumberField(TEXT("rRayTracingEnableInEditor"), RayTracingEditorValue);
	Result->SetNumberField(TEXT("rGenerateMeshDistanceFields"), MeshDistanceFieldsValue);
	Result->SetNumberField(
		TEXT("fxNiagaraAsyncGpuTraceHWRayTraceEnabled"),
		AsyncHwrt ? AsyncHwrt->GetInt() : -1);
	Result->SetNumberField(
		TEXT("fxNiagaraAsyncGpuTraceGlobalSdfEnabled"),
		AsyncGsdf ? AsyncGsdf->GetInt() : -1);
	Result->SetBoolField(
		TEXT("hardwareSupported"),
		GRHISupportsRayTracing);
	Result->SetBoolField(
		TEXT("enabled"),
		bRayTracingEnabled);
	Result->SetBoolField(
		TEXT("dfEnabled"),
		MeshDistanceFieldsValue > 0);
	Result->SetBoolField(
		TEXT("hwrtPreferred"),
		bRayTracingEnabled && GRHISupportsRayTracing);
	SetJsonStringArray(
		Result,
		TEXT("asyncGpuTraceProviderOrder"),
		GetAsyncTraceProviderOrder());
	Result->SetBoolField(
		TEXT("defaultProviderFallbackRisk"),
		IsDefaultAsyncProviderFallbackRisk());
	return Result;
}

bool GetSystemAndSelectors(
	const TSharedPtr<FJsonObject>& Params,
	UNiagaraSystem*& OutSystem,
	FString& OutSystemPath,
	FString& OutEmitter,
	FString& OutGraph,
	FString& OutErrorCode,
	FString& OutError)
{
	if (!Params.IsValid())
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("A Niagara graph request is required.");
		return false;
	}
	FString RequestedSystem;
	if (!Params->TryGetStringField(TEXT("system"), RequestedSystem)
		|| RequestedSystem.TrimStartAndEnd().IsEmpty())
	{
		OutErrorCode = TEXT("invalid_system_path");
		OutError = TEXT("system is required.");
		return false;
	}
	if (!LoadSystem(
		RequestedSystem,
		OutSystem,
		OutSystemPath,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	Params->TryGetStringField(TEXT("emitter"), OutEmitter);
	Params->TryGetStringField(TEXT("graph"), OutGraph);
	OutEmitter = OutEmitter.TrimStartAndEnd();
	OutGraph = OutGraph.TrimStartAndEnd();
	return true;
}

FMCPToolResult ErrorResult(
	const FString& Message,
	const FString& Code,
	const int32 Status = 422)
{
	return FMCPToolResult::Error(Message, Code, Status);
}

class FTool_NiagaraGraphInspect final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.inspect"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FString SystemPath;
		FString EmitterSelector;
		FString GraphSelector;
		FString ErrorCode;
		FString Error;
		if (!GetSystemAndSelectors(
			Params,
			System,
			SystemPath,
			EmitterSelector,
			GraphSelector,
			ErrorCode,
			Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found") ? 404 : 422);
		}

		bool bIncludeDisabled = true;
		Params->TryGetBoolField(TEXT("includeDisabled"), bIncludeDisabled);
		int32 Limit = DefaultNodeLimit;
		if (Params->HasField(TEXT("limit")))
		{
			double Number = 0.0;
			if (!Params->TryGetNumberField(TEXT("limit"), Number)
				|| FMath::TruncToInt(Number) != Number)
			{
				return ErrorResult(
					TEXT("limit must be an integer."),
					TEXT("invalid_limit"));
			}
			Limit = FMath::Clamp(FMath::TruncToInt(Number), 1, MaxNodeLimit);
		}
		TArray<FGraphTarget> Graphs;
		if (!CollectGraphs(
			System,
			EmitterSelector,
			GraphSelector,
			Graphs,
			ErrorCode,
			Error))
		{
			return ErrorResult(Error, ErrorCode, 404);
		}

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		GraphValues.Reserve(Graphs.Num());
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		int32 TotalNodes = 0;
		for (const FGraphTarget& Graph : Graphs)
		{
			if (!Graph.Graph)
			{
				continue;
			}
			TSharedRef<FJsonObject> GraphObject = SerializeGraphScope(Graph);
			TArray<TSharedPtr<FJsonValue>> GraphNodes;
			int32 GraphNodeTotal = 0;
			for (UEdGraphNode* RawNode : Graph.Graph->Nodes)
			{
				const UNiagaraNode* Node = Cast<UNiagaraNode>(RawNode);
				if (!Node || (!bIncludeDisabled && !Node->IsNodeEnabled()))
				{
					continue;
				}
				++GraphNodeTotal;
				++TotalNodes;
				TSharedRef<FJsonObject> NodeObject = SerializeNode(Node);
				NodeObject->SetStringField(TEXT("graph"), Graph.Graph->GetPathName());
				if (NodeValues.Num() < Limit)
				{
					TSharedPtr<FJsonValue> NodeValue = MakeShared<FJsonValueObject>(NodeObject);
					NodeValues.Add(NodeValue);
					GraphNodes.Add(NodeValue);
				}
			}
			GraphObject->SetNumberField(TEXT("nodeTotal"),
				GraphNodeTotal);
			GraphObject->SetNumberField(TEXT("nodeReturned"), GraphNodes.Num());
			GraphObject->SetBoolField(
				TEXT("nodesTruncated"),
				GraphNodes.Num() < GraphNodeTotal);
			GraphObject->SetArrayField(TEXT("nodes"), GraphNodes);
			GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-graph.v1"));
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(TEXT("package"), System->GetOutermost()->GetName());
		Result->SetBoolField(
			TEXT("packageDirty"),
			System->GetOutermost()->IsDirty());
		Result->SetBoolField(TEXT("includeDisabled"), bIncludeDisabled);
		Result->SetNumberField(TEXT("graphCount"), GraphValues.Num());
		Result->SetObjectField(TEXT("rayTracing"), SerializeRayTracingState());
		Result->SetArrayField(TEXT("graphs"), GraphValues);
		SetBoundedArray(Result, TEXT("nodes"), NodeValues, TotalNodes, Limit);
		return FMCPToolResult::Ok(Result);
	}
};

// Forward declaration for policy apply/rollback helpers. The implementation
// remains below the existing compile utility, while the small value type is
// defined here so the policy tool classes can hold it by value.
struct FCompileSummary
{
	FString AggregateStatus = TEXT("unknown");
	bool bCompiled = false;
	bool bHasError = false;
	TArray<TPair<FString, FString>> Scripts;
};

FCompileSummary RequestAndSummarizeCompile(UNiagaraSystem* System);

FString CollisionPolicyProviderName(
	const ENDICollisionQuery_AsyncGpuTraceProvider::Type Provider)
{
	switch (Provider)
	{
	case ENDICollisionQuery_AsyncGpuTraceProvider::Default:
		return TEXT("Default");
	case ENDICollisionQuery_AsyncGpuTraceProvider::HWRT:
		return TEXT("HWRT");
	case ENDICollisionQuery_AsyncGpuTraceProvider::GSDF:
		return TEXT("GSDF");
	case ENDICollisionQuery_AsyncGpuTraceProvider::None:
		return TEXT("None");
	default:
		return TEXT("Unknown");
	}
}

bool ParseCollisionPolicyProvider(
	const FString& Value,
	ENDICollisionQuery_AsyncGpuTraceProvider::Type& OutProvider)
{
	if (Value.Equals(TEXT("Default"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("ProjectDefault"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::Default;
		return true;
	}
	if (Value.Equals(TEXT("HWRT"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("HardwareRayTracing"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::HWRT;
		return true;
	}
	if (Value.Equals(TEXT("GSDF"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("DistanceField"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::GSDF;
		return true;
	}
	if (Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("Disabled"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::None;
		return true;
	}
	return false;
}

bool IsCollisionPolicyName(const FString& Policy)
{
	return Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase)
		|| Policy.Equals(TEXT("distanceField"), ESearchCase::IgnoreCase)
		|| Policy.Equals(TEXT("hybrid"), ESearchCase::IgnoreCase);
}

FString CollisionPolicyOrdinaryAction(
	const FString& Policy,
	const int32 UnmanagedEnabledCount)
{
	if (!Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase))
	{
		return TEXT("leaveUnchanged");
	}
	return UnmanagedEnabledCount > 0
		? TEXT("disableEditablePreserveUnmanaged")
		: TEXT("disable");
}

bool IsEditableCollisionPolicyGraph(
	const FGraphTarget& Target,
	const UNiagaraSystem* System)
{
	return Target.Graph
		&& System
		&& Target.bOwnedBySystem
		&& Target.Graph->GetOutermost() == System->GetOutermost()
		&& Target.Graph->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));
}

bool IsCollisionPolicyNodeManaged(
	const FCollisionPolicyNodeState& State,
	const FString& Policy,
	bool& bOutDesiredEnabled)
{
	bOutDesiredEnabled = true;
	if (State.bDistanceField)
	{
		bOutDesiredEnabled = !Policy.Equals(
			TEXT("hardwareRayTracing"),
			ESearchCase::IgnoreCase);
		return true;
	}
	if (State.bAsyncGpuTrace)
	{
		bOutDesiredEnabled = !Policy.Equals(
			TEXT("distanceField"),
			ESearchCase::IgnoreCase);
		return true;
	}
	// Ordinary CollisionQuery modules are the CPU/legacy collision path. A
	// hardwareRayTracing policy must disable them so an AsyncGpuTrace HWRT path
	// is the sole collision provider. Distance-field and hybrid policies retain
	// the ordinary path for compatibility and do not mutate these nodes.
	if (State.bOrdinaryCollisionQuery)
	{
		if (!Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		bOutDesiredEnabled = false;
		return true;
	}
	return false;
}

bool CollisionPolicyNodeWouldChange(
	const FCollisionPolicyNodeState& State,
	const FString& Policy)
{
	if (!State.bEditable)
	{
		return false;
	}

	bool bDesiredEnabled = true;
	if (!IsCollisionPolicyNodeManaged(State, Policy, bDesiredEnabled))
	{
		return false;
	}

	const ENodeEnabledState DesiredState = bDesiredEnabled
		? ENodeEnabledState::Enabled
		: ENodeEnabledState::Disabled;
	// ApplyCollisionPolicyNodeState also makes an implicit state explicit when
	// the desired value already matches but the node has no user-set state.
	return State.BeforeState != DesiredState || !State.bBeforeUserSet;
}

bool CollisionPolicyDataInterfaceWouldChange(
	const FCollisionPolicyDataInterfaceState& State)
{
	return State.bEditable && State.BeforeProvider != State.AfterProvider;
}

bool CollisionPolicyPlanWouldChangeState(const FCollisionPolicyPlanData& Data)
{
	for (const FCollisionPolicyNodeState& State : Data.Nodes)
	{
		if (CollisionPolicyNodeWouldChange(State, Data.Policy))
		{
			return true;
		}
	}
	for (const FCollisionPolicyDataInterfaceState& State : Data.DataInterfaces)
	{
		if (CollisionPolicyDataInterfaceWouldChange(State))
		{
			return true;
		}
	}
	return false;
}

TArray<FString> GetAsyncTraceProviderOrder()
{
	TArray<FString> Result;
	const UNiagaraSettings* Settings = GetDefault<UNiagaraSettings>();
	if (!Settings)
	{
		return Result;
	}
	for (const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type> Provider
		: Settings->NDICollisionQuery_AsyncGpuTraceProviderOrder)
	{
		Result.Add(CollisionPolicyProviderName(Provider));
	}
	return Result;
}

bool IsAsyncTraceProviderAvailable(
	const ENDICollisionQuery_AsyncGpuTraceProvider::Type Provider)
{
	const IConsoleManager& ConsoleManager = IConsoleManager::Get();
	if (Provider == ENDICollisionQuery_AsyncGpuTraceProvider::HWRT)
	{
		const IConsoleVariable* Enable = ConsoleManager.FindConsoleVariable(
			TEXT("fx.Niagara.AsyncGpuTrace.HWRayTraceEnabled"));
		return GRHISupportsRayTracing
			&& GIsRHIInitialized
			&& IsRayTracingEnabled()
			&& Enable
			&& Enable->GetBool();
	}
	if (Provider == ENDICollisionQuery_AsyncGpuTraceProvider::GSDF)
	{
		const IConsoleVariable* Enable = ConsoleManager.FindConsoleVariable(
			TEXT("fx.Niagara.AsyncGpuTrace.GlobalSdfEnabled"));
		return DoesProjectSupportDistanceFields()
			&& Enable
			&& Enable->GetBool();
	}
	return Provider == ENDICollisionQuery_AsyncGpuTraceProvider::None;
}

bool IsDefaultAsyncProviderFallbackRisk()
{
	const UNiagaraSettings* Settings = GetDefault<UNiagaraSettings>();
	if (!Settings)
	{
		return true;
	}
	const TArray<TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>>& Order =
		Settings->NDICollisionQuery_AsyncGpuTraceProviderOrder;
	const int32 HardwareIndex = Order.IndexOfByPredicate(
		[](const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>& Provider)
		{
			return Provider == ENDICollisionQuery_AsyncGpuTraceProvider::HWRT;
		});
	const int32 DistanceIndex = Order.IndexOfByPredicate(
		[](const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>& Provider)
		{
			return Provider == ENDICollisionQuery_AsyncGpuTraceProvider::GSDF;
		});
	// Default resolves to the first supported provider in the configured order.
	// Therefore GSDF is a possible fallback whenever it has higher priority than
	// HWRT, HWRT is absent, or HWRT is currently unavailable.
	return DistanceIndex != INDEX_NONE
		&& (HardwareIndex == INDEX_NONE
			|| DistanceIndex < HardwareIndex
			|| !IsAsyncTraceProviderAvailable(
				ENDICollisionQuery_AsyncGpuTraceProvider::HWRT));
}

// UNiagaraNodeInput's typed DataInterface accessors are not exported by the
// UE 5.4 NiagaraEditor module. Read its private UPROPERTY through reflection
// so the optional plugin build does not introduce an LNK2019 dependency.
UNiagaraDataInterfaceAsyncGpuTrace* FindAsyncTraceDataInterface(
	UNiagaraNodeInput* InputNode)
{
	if (!InputNode)
	{
		return nullptr;
	}
	FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(
		InputNode->GetClass(),
		TEXT("DataInterface"));
	if (!DataInterfaceProperty)
	{
		return nullptr;
	}
	UObject* Value = DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode);
	return Cast<UNiagaraDataInterfaceAsyncGpuTrace>(Value);
}

void SetJsonStringArray(
	const TSharedRef<FJsonObject>& Object,
	const FString& Field,
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

TSharedRef<FJsonObject> SerializeCollisionPolicyDataInterface(
	const FCollisionPolicyDataInterfaceState& State,
	const bool bAfterState)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), State.GraphPath);
	Result->SetStringField(TEXT("inputNode"), State.InputNodePath);
	Result->SetStringField(TEXT("dataInterface"), State.DataInterfacePath);
	Result->SetStringField(TEXT("inputName"), State.InputName);
	Result->SetStringField(TEXT("beforeProvider"), State.BeforeProvider);
	Result->SetStringField(TEXT("afterProvider"), State.AfterProvider);
	Result->SetStringField(
		TEXT("provider"),
		bAfterState ? State.AfterProvider : State.BeforeProvider);
	Result->SetBoolField(TEXT("editable"), State.bEditable);
	Result->SetBoolField(TEXT("providerChanged"), State.bProviderChanged);
	Result->SetBoolField(TEXT("wouldChange"), CollisionPolicyDataInterfaceWouldChange(State));
	return Result;
}

TSharedRef<FJsonObject> SerializeCollisionPolicyNode(
	const FCollisionPolicyNodeState& State,
	const FString& Policy,
	const bool bAfterState)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), State.GraphPath);
	Result->SetStringField(TEXT("node"), State.NodePath);
	Result->SetBoolField(TEXT("ordinaryCollisionQuery"), State.bOrdinaryCollisionQuery);
	Result->SetBoolField(TEXT("distanceField"), State.bDistanceField);
	Result->SetBoolField(TEXT("asyncGpuTrace"), State.bAsyncGpuTrace);
	Result->SetBoolField(TEXT("editable"), State.bEditable);
	Result->SetBoolField(TEXT("wouldChange"), CollisionPolicyNodeWouldChange(State, Policy));
	const bool bBeforeEnabled = State.BeforeState != ENodeEnabledState::Disabled;
	Result->SetBoolField(TEXT("beforeEnabled"), bBeforeEnabled);
	bool bDesiredEnabled = true;
	const bool bManaged = IsCollisionPolicyNodeManaged(State, Policy, bDesiredEnabled);
	const bool bEffectiveManaged = bManaged && State.bEditable;
	const bool bAfterEnabled = bEffectiveManaged ? bDesiredEnabled : bBeforeEnabled;
	const ENodeEnabledState AfterState = bEffectiveManaged
		? (bDesiredEnabled ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled)
		: State.BeforeState;
	const bool bAfterUserSet = bEffectiveManaged ? true : State.bBeforeUserSet;
	Result->SetBoolField(TEXT("managed"), bManaged);
	Result->SetBoolField(TEXT("willMutate"), bEffectiveManaged && CollisionPolicyNodeWouldChange(State, Policy));
	Result->SetBoolField(TEXT("afterEnabled"), bAfterEnabled);
	Result->SetStringField(TEXT("beforeDesiredState"), EnabledStateName(State.BeforeState));
	Result->SetStringField(TEXT("afterDesiredState"), EnabledStateName(AfterState));
	Result->SetBoolField(TEXT("enabled"), bAfterState ? bAfterEnabled : bBeforeEnabled);
	Result->SetStringField(
		TEXT("desiredState"),
		EnabledStateName(bAfterState ? AfterState : State.BeforeState));
	Result->SetBoolField(
		TEXT("userSet"),
		bAfterState ? bAfterUserSet : State.bBeforeUserSet);
	return Result;
}

void AddCollisionPolicyRisk(
	FCollisionPolicyPlanData& Data,
	const FString& Risk)
{
	if (!Data.Risks.Contains(Risk))
	{
		Data.Risks.Add(Risk);
	}
	Data.bBlocked = true;
}

void AddCollisionPolicyWarning(
	FCollisionPolicyPlanData& Data,
	const FString& Warning)
{
	if (!Data.Warnings.Contains(Warning))
	{
		Data.Warnings.Add(Warning);
	}
}

void SortCollisionPolicyStates(FCollisionPolicyPlanData& Data)
{
	Data.Graphs.Sort(
		[](const FCollisionPolicyGraphState& Left, const FCollisionPolicyGraphState& Right)
		{
			return Left.GraphPath < Right.GraphPath;
		});
	Data.Nodes.Sort(
		[](const FCollisionPolicyNodeState& Left, const FCollisionPolicyNodeState& Right)
		{
			if (Left.GraphPath != Right.GraphPath)
			{
				return Left.GraphPath < Right.GraphPath;
			}
			return Left.NodePath < Right.NodePath;
		});
	Data.DataInterfaces.Sort(
		[](const FCollisionPolicyDataInterfaceState& Left, const FCollisionPolicyDataInterfaceState& Right)
		{
			if (Left.GraphPath != Right.GraphPath)
			{
				return Left.GraphPath < Right.GraphPath;
			}
			return Left.DataInterfacePath < Right.DataInterfacePath;
		});
}

bool BuildCollisionPolicyPlanData(
	const TSharedPtr<FJsonObject>& Params,
	FCollisionPolicyPlanData& OutData,
	FString& OutErrorCode,
	FString& OutError)
{
	OutData = FCollisionPolicyPlanData();
	UNiagaraSystem* System = nullptr;
	if (!GetSystemAndSelectors(
		Params,
		System,
		OutData.SystemPath,
		OutData.EmitterSelector,
		OutData.GraphSelector,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	OutData.System = System;
	if (!IsProjectSystem(System))
	{
		OutErrorCode = TEXT("asset_scope_forbidden");
		OutError = TEXT("Niagara collision policy writes are restricted to project assets under /Game.");
		return false;
	}

	if (!Params->TryGetStringField(TEXT("policy"), OutData.Policy)
		|| !IsCollisionPolicyName(OutData.Policy))
	{
		OutErrorCode = TEXT("invalid_policy");
		OutError = TEXT("policy must be one of hardwareRayTracing, distanceField, or hybrid.");
		return false;
	}
	Params->TryGetBoolField(
		TEXT("allowMissingAsyncTrace"),
		OutData.bAllowMissingAsyncTrace);
	Params->TryGetBoolField(
		TEXT("allowDefaultProviderFallback"),
		OutData.bAllowDefaultProviderFallback);
	Params->TryGetBoolField(
		TEXT("allowUnmanagedOrdinaryCollisionQuery"),
		OutData.bAllowUnmanagedOrdinaryCollisionQuery);
	Params->TryGetBoolField(
		TEXT("allowUnmanagedDistanceField"),
		OutData.bAllowUnmanagedDistanceField);
	Params->TryGetBoolField(TEXT("includeDisabled"), OutData.bIncludeDisabled);
	FString Persistence = TEXT("dirtyOnly");
	if (Params->HasField(TEXT("persistence"))
		&& (!Params->TryGetStringField(TEXT("persistence"), Persistence)
			|| Persistence != TEXT("dirtyOnly")))
	{
		OutErrorCode = TEXT("invalid_persistence");
		OutError = TEXT("Only persistence='dirtyOnly' is supported for Niagara collision policy edits.");
		return false;
	}

	TArray<FGraphTarget> Graphs;
	if (!CollectGraphs(
		System,
		OutData.EmitterSelector,
		OutData.GraphSelector,
		Graphs,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	Graphs.Sort(
		[](const FGraphTarget& Left, const FGraphTarget& Right)
		{
			return (Left.Graph ? Left.Graph->GetPathName() : FString())
				< (Right.Graph ? Right.Graph->GetPathName() : FString());
		});

		for (const FGraphTarget& Target : Graphs)
		{
			if (!Target.Graph)
			{
				continue;
			}
			FCollisionPolicyGraphState GraphState;
			GraphState.Graph = Target.Graph;
			GraphState.GraphPath = Target.Graph->GetPathName();
			GraphState.ChangeId = Target.Graph->GetChangeID().ToString(
				EGuidFormats::DigitsWithHyphensLower);
			GraphState.ScopeKind = Target.ScopeKind;
			GraphState.ScopeName = Target.ScopeName;
			GraphState.ReferenceDepth = Target.ReferenceDepth;
			GraphState.bEditable = IsEditableCollisionPolicyGraph(Target, System);
			bool bGraphHasAsync = false;
			for (UEdGraphNode* RawNode : Target.Graph->Nodes)
			{
				UNiagaraNodeFunctionCall* FunctionCall =
					Cast<UNiagaraNodeFunctionCall>(RawNode);
				if (!FunctionCall
					|| (!OutData.bIncludeDisabled && !FunctionCall->IsNodeEnabled()))
				{
					continue;
				}
				TArray<FString> Classes;
				bool bOrdinary = false;
				bool bDistanceField = false;
				bool bAsync = false;
				ClassifyNode(
					FunctionCall,
					Classes,
					bOrdinary,
					bDistanceField,
					bAsync);
				if (!bOrdinary && !bDistanceField && !bAsync)
				{
					continue;
				}
				bGraphHasAsync |= bAsync;
				const bool bManagedByPolicy = bDistanceField
					|| bAsync
					|| (bOrdinary
						&& OutData.Policy.Equals(
							TEXT("hardwareRayTracing"),
							ESearchCase::IgnoreCase));
				GraphState.ManagedNodeCount += bManagedByPolicy ? 1 : 0;
				if (bOrdinary)
				{
					++GraphState.OrdinaryCollisionQueryCount;
				}
				if (GraphState.bEditable && bManagedByPolicy)
				{
					++GraphState.EditableManagedNodeCount;
				}
				if (GraphState.bEditable && bOrdinary)
				{
					++GraphState.EditableOrdinaryCollisionQueryCount;
				}
				FCollisionPolicyNodeState NodeState;
				NodeState.Graph = Target.Graph;
				NodeState.Node = FunctionCall;
				NodeState.GraphPath = GraphState.GraphPath;
				NodeState.NodePath = FunctionCall->GetPathName();
				NodeState.Classes = Classes;
				NodeState.bOrdinaryCollisionQuery = bOrdinary;
				NodeState.bDistanceField = bDistanceField;
				NodeState.bAsyncGpuTrace = bAsync;
				NodeState.bEditable = GraphState.bEditable;
				NodeState.BeforeState = FunctionCall->GetDesiredEnabledState();
				NodeState.bBeforeUserSet = FunctionCall->HasUserSetTheEnabledState();
				NodeState.AfterState = NodeState.BeforeState;
				NodeState.bAfterUserSet = NodeState.bBeforeUserSet;
				OutData.bHasOrdinaryCollisionQuery |= bOrdinary;
				OutData.bHasDistanceField |= bDistanceField;
				OutData.bHasAsyncGpuTrace |= bAsync;
				OutData.bHasEditableOrdinaryCollisionQuery |=
					bOrdinary && GraphState.bEditable;
				OutData.OrdinaryCollisionQueryCount += bOrdinary ? 1 : 0;
				OutData.EditableOrdinaryCollisionQueryCount +=
					bOrdinary && GraphState.bEditable ? 1 : 0;
				OutData.UnmanagedEnabledOrdinaryCollisionQueryCount +=
					bOrdinary
					&& !GraphState.bEditable
					&& FunctionCall->GetDesiredEnabledState() != ENodeEnabledState::Disabled
						? 1
						: 0;
				OutData.UnmanagedEnabledDistanceFieldCount +=
					bDistanceField
					&& !GraphState.bEditable
					&& FunctionCall->GetDesiredEnabledState() != ENodeEnabledState::Disabled
						? 1
						: 0;
				OutData.bHasEditableDistanceField |= bDistanceField && GraphState.bEditable;
				OutData.bHasEditableAsyncGpuTrace |= bAsync && GraphState.bEditable;
				OutData.Nodes.Add(MoveTemp(NodeState));
			}

			// AsyncGpuTrace's provider lives on a DI input node. Only inspect DIs
			// in graphs that actually contain an Async trace call, and deduplicate
			// by graph/object path so shared pins cannot be changed twice.
			if (bGraphHasAsync)
			{
				for (UEdGraphNode* RawNode : Target.Graph->Nodes)
				{
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(RawNode);
					if (!InputNode)
					{
						continue;
					}
					UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
						FindAsyncTraceDataInterface(InputNode);
					if (!DataInterface)
					{
						continue;
					}
					const FString DataInterfacePath = DataInterface->GetPathName();
					const bool bAlreadyCaptured = OutData.DataInterfaces.ContainsByPredicate(
						[&Target, &DataInterfacePath](const FCollisionPolicyDataInterfaceState& Existing)
						{
							return Existing.GraphPath == (Target.Graph ? Target.Graph->GetPathName() : FString())
								&& Existing.DataInterfacePath == DataInterfacePath;
						});
					if (bAlreadyCaptured)
					{
						continue;
					}
					FCollisionPolicyDataInterfaceState DIState;
					DIState.Graph = Target.Graph;
					DIState.InputNode = InputNode;
					DIState.DataInterface = DataInterface;
					DIState.GraphPath = GraphState.GraphPath;
					DIState.InputNodePath = InputNode->GetPathName();
					DIState.DataInterfacePath = DataInterfacePath;
					DIState.InputName = InputNode->Input.GetName().ToString();
					DIState.BeforeProvider = CollisionPolicyProviderName(DataInterface->TraceProvider);
					DIState.AfterProvider = DIState.BeforeProvider;
					DIState.bEditable = GraphState.bEditable;
					if (DIState.BeforeProvider == TEXT("Default")
						&& (!OutData.Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase)
							|| !DIState.bEditable))
					{
						OutData.bDefaultProviderFallbackRisk = true;
					}
					if (OutData.Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase)
						&& DIState.bEditable)
					{
						DIState.AfterProvider = CollisionPolicyProviderName(
							ENDICollisionQuery_AsyncGpuTraceProvider::HWRT);
					}
					DIState.bProviderChanged =
						DIState.BeforeProvider != DIState.AfterProvider;
					OutData.bHasEditableAsyncProvider |= DIState.bEditable;
					OutData.DataInterfaces.Add(MoveTemp(DIState));
				}
			}
			OutData.Graphs.Add(MoveTemp(GraphState));
		}

	SortCollisionPolicyStates(OutData);

	// Explicit provider selection is required for RT-only semantics. A Default
	// DI in a read-only/shared graph cannot be made RT-only by toggling nodes.
	if (OutData.Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase))
	{
		if (OutData.UnmanagedEnabledOrdinaryCollisionQueryCount > 0
			&& !OutData.bAllowUnmanagedOrdinaryCollisionQuery)
		{
			AddCollisionPolicyRisk(
				OutData,
				FString::Printf(
					TEXT("hardwareRayTracing cannot disable %d enabled ordinary CollisionQuery node(s) in read-only/shared graphs"),
					OutData.UnmanagedEnabledOrdinaryCollisionQueryCount));
		}
		else if (OutData.UnmanagedEnabledOrdinaryCollisionQueryCount > 0)
		{
			AddCollisionPolicyWarning(
				OutData,
				FString::Printf(
					TEXT("%d enabled ordinary CollisionQuery node(s) are read-only/shared and remain enabled because allowUnmanagedOrdinaryCollisionQuery=true"),
					OutData.UnmanagedEnabledOrdinaryCollisionQueryCount));
		}
		if (OutData.UnmanagedEnabledDistanceFieldCount > 0
			&& !OutData.bAllowUnmanagedDistanceField)
		{
			AddCollisionPolicyRisk(
				OutData,
				FString::Printf(
					TEXT("hardwareRayTracing cannot disable %d enabled Distance Field node(s) in read-only/shared graphs"),
					OutData.UnmanagedEnabledDistanceFieldCount));
		}
		else if (OutData.UnmanagedEnabledDistanceFieldCount > 0)
		{
			AddCollisionPolicyWarning(
				OutData,
				FString::Printf(
					TEXT("%d enabled Distance Field node(s) are read-only/shared and remain enabled because allowUnmanagedDistanceField=true"),
					OutData.UnmanagedEnabledDistanceFieldCount));
		}
		if (!OutData.bHasAsyncGpuTrace && !OutData.bAllowMissingAsyncTrace)
		{
			AddCollisionPolicyRisk(
				OutData,
				TEXT("hardwareRayTracing requires at least one AsyncGpuTrace node; none was found"));
		}
		else if (OutData.bHasAsyncGpuTrace && !OutData.bHasEditableAsyncGpuTrace
			&& !OutData.bAllowMissingAsyncTrace)
		{
			AddCollisionPolicyRisk(
				OutData,
				TEXT("all AsyncGpuTrace nodes are in read-only/shared graphs"));
		}
		if (OutData.bHasAsyncGpuTrace && !OutData.bHasEditableAsyncProvider
			&& !OutData.bAllowDefaultProviderFallback)
		{
			AddCollisionPolicyRisk(
				OutData,
				TEXT("AsyncGpuTrace provider is not editable; Project Default may resolve to GSDF instead of HWRT"));
		}
		if (IsDefaultAsyncProviderFallbackRisk())
		{
			AddCollisionPolicyWarning(
				OutData,
				TEXT("Default AsyncGpuTrace provider order may resolve to GSDF before HWRT or when HWRT is unavailable"));
		}
		if (!IsAsyncTraceProviderAvailable(
			ENDICollisionQuery_AsyncGpuTraceProvider::HWRT))
		{
			AddCollisionPolicyWarning(
				OutData,
				TEXT("HWRT is currently unavailable; explicit HWRT AsyncGpuTrace providers will not produce hits until ray tracing is enabled"));
		}
	}
	if (OutData.Policy.Equals(TEXT("distanceField"), ESearchCase::IgnoreCase)
		&& !OutData.bHasDistanceField)
	{
		AddCollisionPolicyWarning(
			OutData,
			TEXT("distanceField policy found no Distance Field nodes"));
	}
	if (OutData.bDefaultProviderFallbackRisk)
	{
		AddCollisionPolicyWarning(
			OutData,
			TEXT("One or more enabled AsyncGpuTrace inputs retain Project Default; they may resolve to GSDF instead of HWRT when the configured priority or availability selects it"));
	}
	if (OutData.OrdinaryCollisionQueryCount > 0)
	{
		if (OutData.Policy.Equals(TEXT("hardwareRayTracing"), ESearchCase::IgnoreCase))
		{
			if (OutData.EditableOrdinaryCollisionQueryCount > 0)
			{
				AddCollisionPolicyWarning(
					OutData,
					TEXT("hardwareRayTracing disables editable ordinary CollisionQuery modules; their rollback state is retained in the receipt"));
			}
		}
		else
		{
			AddCollisionPolicyWarning(
				OutData,
				TEXT("distanceField and hybrid policies leave ordinary CollisionQuery modules unchanged"));
		}
	}
	if (!CollisionPolicyPlanWouldChangeState(OutData))
	{
		AddCollisionPolicyWarning(
			OutData,
			TEXT("No editable collision policy target requires a state change"));
	}

	return true;
}

TSharedRef<FJsonObject> BuildCollisionPolicyPlanJson(
	const FCollisionPolicyPlanData& Data)
{
	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
	Plan->SetStringField(TEXT("planKind"), TEXT("niagaraCollisionPolicy"));
	Plan->SetStringField(TEXT("action"), TEXT("setCollisionProviderPolicy"));
	Plan->SetStringField(TEXT("scope"), Data.SystemPath);
	Plan->SetStringField(TEXT("system"), Data.SystemPath);
	Plan->SetStringField(TEXT("policy"), Data.Policy);
	Plan->SetBoolField(TEXT("includeDisabled"), Data.bIncludeDisabled);
	Plan->SetBoolField(TEXT("allowMissingAsyncTrace"), Data.bAllowMissingAsyncTrace);
	Plan->SetBoolField(TEXT("allowDefaultProviderFallback"), Data.bAllowDefaultProviderFallback);
	Plan->SetBoolField(
		TEXT("allowUnmanagedOrdinaryCollisionQuery"),
		Data.bAllowUnmanagedOrdinaryCollisionQuery);
	Plan->SetBoolField(
		TEXT("allowUnmanagedDistanceField"),
		Data.bAllowUnmanagedDistanceField);
	Plan->SetBoolField(TEXT("hasOrdinaryCollisionQuery"), Data.bHasOrdinaryCollisionQuery);
	Plan->SetBoolField(
		TEXT("hasEditableOrdinaryCollisionQuery"),
		Data.bHasEditableOrdinaryCollisionQuery);
	Plan->SetStringField(
		TEXT("ordinaryCollisionQueryAction"),
		CollisionPolicyOrdinaryAction(
			Data.Policy,
			Data.UnmanagedEnabledOrdinaryCollisionQueryCount));
	Plan->SetNumberField(
		TEXT("ordinaryCollisionQueryCount"),
		Data.OrdinaryCollisionQueryCount);
	Plan->SetNumberField(
		TEXT("editableOrdinaryCollisionQueryCount"),
		Data.EditableOrdinaryCollisionQueryCount);
	Plan->SetNumberField(
		TEXT("unmanagedEnabledOrdinaryCollisionQueryCount"),
		Data.UnmanagedEnabledOrdinaryCollisionQueryCount);
	Plan->SetNumberField(
		TEXT("unmanagedEnabledDistanceFieldCount"),
		Data.UnmanagedEnabledDistanceFieldCount);
	Plan->SetBoolField(TEXT("defaultProviderFallbackRisk"), Data.bDefaultProviderFallbackRisk);
	Plan->SetBoolField(TEXT("blocked"), Data.bBlocked);
	Plan->SetStringField(TEXT("risk"), Data.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
	Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	Plan->SetBoolField(TEXT("changesState"), CollisionPolicyPlanWouldChangeState(Data));
	Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
	Plan->SetObjectField(TEXT("rayTracing"), SerializeRayTracingState());
	SetJsonStringArray(Plan, TEXT("risks"), Data.Risks);
	SetJsonStringArray(Plan, TEXT("warnings"), Data.Warnings);
	SetJsonStringArray(Plan, TEXT("providerOrder"), GetAsyncTraceProviderOrder());

	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("system"), Data.SystemPath);
	if (!Data.EmitterSelector.IsEmpty())
	{
		Request->SetStringField(TEXT("emitter"), Data.EmitterSelector);
	}
	if (!Data.GraphSelector.IsEmpty())
	{
		Request->SetStringField(TEXT("graph"), Data.GraphSelector);
	}
	Request->SetStringField(TEXT("policy"), Data.Policy);
	Request->SetBoolField(TEXT("includeDisabled"), Data.bIncludeDisabled);
	Request->SetBoolField(TEXT("allowMissingAsyncTrace"), Data.bAllowMissingAsyncTrace);
	Request->SetBoolField(TEXT("allowDefaultProviderFallback"), Data.bAllowDefaultProviderFallback);
	Request->SetBoolField(
		TEXT("allowUnmanagedOrdinaryCollisionQuery"),
		Data.bAllowUnmanagedOrdinaryCollisionQuery);
	Request->SetBoolField(
		TEXT("allowUnmanagedDistanceField"),
		Data.bAllowUnmanagedDistanceField);
	Request->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
	Plan->SetObjectField(TEXT("request"), Request);

	TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
	Preconditions->SetStringField(TEXT("system"), Data.SystemPath);
	Preconditions->SetBoolField(
		TEXT("packageDirty"),
		Data.System.IsValid() && Data.System->GetOutermost()->IsDirty());
	TArray<TSharedPtr<FJsonValue>> GraphValues;
	for (const FCollisionPolicyGraphState& Graph : Data.Graphs)
	{
		TSharedRef<FJsonObject> GraphObject = MakeShared<FJsonObject>();
		GraphObject->SetStringField(TEXT("path"), Graph.GraphPath);
		GraphObject->SetStringField(TEXT("changeId"), Graph.ChangeId);
		GraphObject->SetBoolField(TEXT("editable"), Graph.bEditable);
		GraphObject->SetNumberField(TEXT("managedNodeCount"), Graph.ManagedNodeCount);
		GraphObject->SetNumberField(TEXT("editableManagedNodeCount"), Graph.EditableManagedNodeCount);
		GraphObject->SetNumberField(
			TEXT("ordinaryCollisionQueryCount"),
			Graph.OrdinaryCollisionQueryCount);
		GraphObject->SetNumberField(
			TEXT("editableOrdinaryCollisionQueryCount"),
			Graph.EditableOrdinaryCollisionQueryCount);
		GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
	}
	Preconditions->SetArrayField(TEXT("graphs"), GraphValues);
	Plan->SetObjectField(TEXT("preconditions"), Preconditions);

	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NodeValues;
	TArray<TSharedPtr<FJsonValue>> DIValues;
	TArray<TSharedPtr<FJsonValue>> AfterNodeValues;
	TArray<TSharedPtr<FJsonValue>> AfterDIValues;
	NodeValues.Reserve(Data.Nodes.Num());
	AfterNodeValues.Reserve(Data.Nodes.Num());
	DIValues.Reserve(Data.DataInterfaces.Num());
	AfterDIValues.Reserve(Data.DataInterfaces.Num());
	for (const FCollisionPolicyNodeState& Node : Data.Nodes)
	{
		NodeValues.Add(MakeShared<FJsonValueObject>(
			SerializeCollisionPolicyNode(Node, Data.Policy, false)));
		AfterNodeValues.Add(MakeShared<FJsonValueObject>(
			SerializeCollisionPolicyNode(Node, Data.Policy, true)));
	}
	for (const FCollisionPolicyDataInterfaceState& DI : Data.DataInterfaces)
	{
		DIValues.Add(MakeShared<FJsonValueObject>(
			SerializeCollisionPolicyDataInterface(DI, false)));
		AfterDIValues.Add(MakeShared<FJsonValueObject>(
			SerializeCollisionPolicyDataInterface(DI, true)));
	}
	Before->SetArrayField(TEXT("nodes"), NodeValues);
	Before->SetArrayField(TEXT("dataInterfaces"), DIValues);
	// Keep before and after records separate so the approval digest exposes the
	// exact state transition, including ordinary CollisionQuery disablement.
	After->SetArrayField(TEXT("nodes"), AfterNodeValues);
	After->SetArrayField(TEXT("dataInterfaces"), AfterDIValues);
	Plan->SetObjectField(TEXT("before"), Before);
	Plan->SetObjectField(TEXT("after"), After);
	return Plan;
}

bool ApplyCollisionPolicyNodeState(
	UNiagaraGraph* Graph,
	FCollisionPolicyNodeState& State,
	const FString& Policy,
	bool& bOutChanged)
{
	bOutChanged = false;
	UNiagaraNodeFunctionCall* Node = State.Node.Get();
	if (!Graph || !Node || !State.bEditable)
	{
		return !State.bEditable;
	}
	bool bDesiredEnabled = true;
	if (!IsCollisionPolicyNodeManaged(State, Policy, bDesiredEnabled))
	{
		return true;
	}
	const ENodeEnabledState DesiredState = bDesiredEnabled
		? ENodeEnabledState::Enabled
		: ENodeEnabledState::Disabled;
	if (Node->GetDesiredEnabledState() != DesiredState
		|| !Node->HasUserSetTheEnabledState())
	{
		Node->Modify();
		Node->SetEnabledState(DesiredState, true);
		Node->MarkNodeRequiresSynchronization(
			TEXT("UE AI Niagara collision policy changed"),
			true);
		bOutChanged = true;
	}
	State.AfterState = Node->GetDesiredEnabledState();
	State.bAfterUserSet = Node->HasUserSetTheEnabledState();
	return State.AfterState == DesiredState && State.bAfterUserSet;
}

bool ApplyCollisionPolicyDataInterfaceState(
	UNiagaraGraph* Graph,
	FCollisionPolicyDataInterfaceState& State,
	bool& bOutChanged)
{
	bOutChanged = false;
	UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = State.DataInterface.Get();
	if (!Graph || !DataInterface || !State.bEditable)
	{
		return !State.bEditable;
	}
	ENDICollisionQuery_AsyncGpuTraceProvider::Type DesiredProvider;
	if (!ParseCollisionPolicyProvider(State.AfterProvider, DesiredProvider))
	{
		return false;
	}
	const FString CurrentProvider = CollisionPolicyProviderName(DataInterface->TraceProvider);
	if (CurrentProvider != State.AfterProvider)
	{
		DataInterface->Modify();
		DataInterface->TraceProvider = DesiredProvider;
		// MarkRenderDataDirty is public inline API and schedules the proxy update;
		// graph notification is dispatched through the exported UEdGraph virtual.
		DataInterface->MarkRenderDataDirty();
		Graph->NotifyGraphChanged();
		bOutChanged = true;
	}
	return CollisionPolicyProviderName(DataInterface->TraceProvider) == State.AfterProvider;
}

bool RestoreCollisionPolicyStates(
	UNiagaraSystem* System,
	TArray<FCollisionPolicyGraphState>& GraphStates,
	TArray<FCollisionPolicyNodeState>& NodeStates,
	TArray<FCollisionPolicyDataInterfaceState>& DataInterfaces,
	const bool bRestoreBefore,
	FCompileSummary& OutCompileSummary)
{
	OutCompileSummary = FCompileSummary();
	OutCompileSummary.AggregateStatus = TEXT("not_required");
	OutCompileSummary.bCompiled = true;
	if (!System)
	{
		return false;
	}
	TMap<UNiagaraGraph*, bool> ChangedGraphs;
	for (FCollisionPolicyGraphState& GraphState : GraphStates)
	{
		UNiagaraGraph* Graph = GraphState.Graph.Get();
		if (Graph && GraphState.bEditable)
		{
			Graph->Modify();
			ChangedGraphs.FindOrAdd(Graph, false);
		}
	}
	bool bChanged = false;
	for (FCollisionPolicyNodeState& State : NodeStates)
	{
		UNiagaraGraph* Graph = State.Graph.Get();
		UNiagaraNodeFunctionCall* Node = State.Node.Get();
		if (!Graph || !Node || !State.bEditable)
		{
			continue;
		}
		const ENodeEnabledState DesiredState = bRestoreBefore
			? State.BeforeState
			: State.AfterState;
		const bool bDesiredUserSet = bRestoreBefore
			? State.bBeforeUserSet
			: State.bAfterUserSet;
		if (Node->GetDesiredEnabledState() != DesiredState
			|| Node->HasUserSetTheEnabledState() != bDesiredUserSet)
		{
			Node->Modify();
			Node->SetEnabledState(DesiredState, bDesiredUserSet);
			Node->MarkNodeRequiresSynchronization(
				bRestoreBefore
					? TEXT("UE AI Niagara collision policy restored")
					: TEXT("UE AI Niagara collision policy applied"),
				true);
			ChangedGraphs.FindOrAdd(Graph) = true;
			bChanged = true;
		}
	}
	for (FCollisionPolicyDataInterfaceState& State : DataInterfaces)
	{
		UNiagaraGraph* Graph = State.Graph.Get();
		UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = State.DataInterface.Get();
		if (!Graph || !DataInterface || !State.bEditable)
		{
			continue;
		}
		const FString& DesiredName = bRestoreBefore
			? State.BeforeProvider
			: State.AfterProvider;
		ENDICollisionQuery_AsyncGpuTraceProvider::Type DesiredProvider;
		if (!ParseCollisionPolicyProvider(DesiredName, DesiredProvider))
		{
			return false;
		}
		if (CollisionPolicyProviderName(DataInterface->TraceProvider) != DesiredName)
		{
			DataInterface->Modify();
			DataInterface->TraceProvider = DesiredProvider;
			DataInterface->MarkRenderDataDirty();
			Graph->NotifyGraphChanged();
			ChangedGraphs.FindOrAdd(Graph) = true;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		System->Modify();
		System->MarkPackageDirty();
		OutCompileSummary = RequestAndSummarizeCompile(System);
	}
	for (const FCollisionPolicyNodeState& State : NodeStates)
	{
		const UNiagaraNodeFunctionCall* Node = State.Node.Get();
		if (State.bEditable && (!State.Graph.IsValid() || !Node
			|| Node->GetDesiredEnabledState() != (bRestoreBefore ? State.BeforeState : State.AfterState)
			|| Node->HasUserSetTheEnabledState() != (bRestoreBefore ? State.bBeforeUserSet : State.bAfterUserSet)))
		{
			return false;
		}
	}
	for (const FCollisionPolicyDataInterfaceState& State : DataInterfaces)
	{
		const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = State.DataInterface.Get();
		if (State.bEditable && (!State.Graph.IsValid() || !DataInterface
			|| CollisionPolicyProviderName(DataInterface->TraceProvider)
				!= (bRestoreBefore ? State.BeforeProvider : State.AfterProvider)))
		{
			return false;
		}
	}
	return true;
}

TSharedRef<FJsonObject> MakeCollisionPolicyResult(
	const FCollisionPolicyReceipt& Receipt,
	const bool bReplay)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-collision-policy.v1"));
	Result->SetStringField(TEXT("status"), TEXT("succeeded"));
	Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
	Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
	Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
	Result->SetStringField(TEXT("system"), Receipt.SystemPath);
	Result->SetStringField(TEXT("policy"), Receipt.Policy);
	Result->SetNumberField(TEXT("affectedNodeCount"), Receipt.Nodes.Num());
	Result->SetNumberField(TEXT("affectedDataInterfaceCount"), Receipt.DataInterfaces.Num());
	const int32 OrdinaryCollisionQueryCount = Receipt.OrdinaryCollisionQueryCount;
	const int32 EditableOrdinaryCollisionQueryCount = Receipt.EditableOrdinaryCollisionQueryCount;
	int32 OrdinaryCollisionQueryChangedCount = 0;
	bool bChanged = false;
	for (const FCollisionPolicyNodeState& State : Receipt.Nodes)
	{
		if (State.bOrdinaryCollisionQuery)
		{
			OrdinaryCollisionQueryChangedCount +=
				(State.BeforeState != State.AfterState
					|| State.bBeforeUserSet != State.bAfterUserSet)
				? 1
				: 0;
		}
		bChanged |= State.BeforeState != State.AfterState
			|| State.bBeforeUserSet != State.bAfterUserSet;
	}
	for (const FCollisionPolicyDataInterfaceState& State : Receipt.DataInterfaces)
	{
		bChanged |= State.bProviderChanged;
	}
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetNumberField(
		TEXT("ordinaryCollisionQueryCount"),
		OrdinaryCollisionQueryCount);
	Result->SetNumberField(
		TEXT("editableOrdinaryCollisionQueryCount"),
		EditableOrdinaryCollisionQueryCount);
	Result->SetNumberField(
		TEXT("ordinaryCollisionQueryChangedCount"),
		OrdinaryCollisionQueryChangedCount);
	Result->SetNumberField(
		TEXT("unmanagedEnabledOrdinaryCollisionQueryCount"),
		Receipt.UnmanagedEnabledOrdinaryCollisionQueryCount);
	Result->SetNumberField(
		TEXT("unmanagedEnabledDistanceFieldCount"),
		Receipt.UnmanagedEnabledDistanceFieldCount);
	Result->SetStringField(
		TEXT("ordinaryCollisionQueryAction"),
		CollisionPolicyOrdinaryAction(
			Receipt.Policy,
			Receipt.UnmanagedEnabledOrdinaryCollisionQueryCount));
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
	Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
	Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
	Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	return Result;
}

class FTool_NiagaraCollisionPolicyPlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.collision.policy.plan");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FCollisionPolicyPlanData Data;
		FString ErrorCode;
		FString Error;
		if (!BuildCollisionPolicyPlanData(Params, Data, ErrorCode, Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					? 404
					: 422);
		}
		TSharedRef<FJsonObject> Plan = BuildCollisionPolicyPlanJson(Data);
		FString PlanDigest;
		if (!TryDigestJson(Plan, PlanDigest))
		{
			return ErrorResult(
				TEXT("Unable to compute the Niagara collision policy plan digest."),
				TEXT("digest_unavailable"),
				500);
		}
		Plan->SetStringField(TEXT("planDigest"), PlanDigest);
		return FMCPToolResult::Ok(Plan);
	}
};

class FTool_NiagaraCollisionPolicyApply final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.collision.policy.apply");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RequestId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty())
		{
			return ErrorResult(
				TEXT("A non-empty requestId is required for Niagara collision policy writes."),
				TEXT("request_id_required"),
				422);
		}
		if (const FString* ExistingReceiptId = CollisionPolicyRequestReceiptIds().Find(RequestId))
		{
			if (FCollisionPolicyReceipt* Existing = CollisionPolicyReceipts().Find(*ExistingReceiptId))
			{
				FString ErrorCode;
				FString Error;
				if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
				{
					return ErrorResult(Error, ErrorCode, 409);
				}
				return FMCPToolResult::Ok(MakeCollisionPolicyResult(*Existing, true));
			}
			return ErrorResult(
				TEXT("requestId is associated with an unavailable Niagara collision policy receipt."),
				TEXT("request_id_conflict"),
				409);
		}

		FCollisionPolicyPlanData Data;
		FString ErrorCode;
		FString Error;
		if (!BuildCollisionPolicyPlanData(Params, Data, ErrorCode, Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					? 404
					: 422);
		}
		TSharedRef<FJsonObject> Plan = BuildCollisionPolicyPlanJson(Data);
		FString PlanDigest;
		if (!TryDigestJson(Plan, PlanDigest))
		{
			return ErrorResult(
				TEXT("Unable to compute the Niagara collision policy plan digest."),
				TEXT("digest_unavailable"),
				500);
		}
		if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
		{
			return ErrorResult(Error, ErrorCode, 409);
		}
		if (Data.bBlocked)
		{
			return ErrorResult(
				TEXT("The Niagara collision policy plan is blocked by unresolved safety risks; resolve them or provide the explicit allow* override and re-plan."),
				TEXT("policy_blocked"),
				409);
		}

		UNiagaraSystem* System = Data.System.Get();
		if (!System)
		{
			return ErrorResult(
				TEXT("The Niagara System is no longer loaded."),
				TEXT("target_unavailable"),
				409);
		}
		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Apply Niagara Collision Policy")));
		System->Modify();
		bool bApplied = true;
		bool bAnyChanged = false;
		for (FCollisionPolicyGraphState& GraphState : Data.Graphs)
		{
			UNiagaraGraph* Graph = GraphState.Graph.Get();
			if (!Graph || !GraphState.bEditable)
			{
				continue;
			}
			Graph->Modify();
			for (FCollisionPolicyNodeState& NodeState : Data.Nodes)
			{
				if (NodeState.Graph.Get() != Graph)
				{
					continue;
				}
				bool bChanged = false;
				bApplied &= ApplyCollisionPolicyNodeState(
					Graph,
					NodeState,
					Data.Policy,
					bChanged);
				bAnyChanged |= bChanged;
			}
			for (FCollisionPolicyDataInterfaceState& DIState : Data.DataInterfaces)
			{
				if (DIState.Graph.Get() != Graph)
				{
					continue;
				}
				bool bChanged = false;
				bApplied &= ApplyCollisionPolicyDataInterfaceState(
					Graph,
					DIState,
					bChanged);
				bAnyChanged |= bChanged;
			}
		}
		if (bAnyChanged)
		{
			System->MarkPackageDirty();
		}
		const FCompileSummary CompileSummary = RequestAndSummarizeCompile(System);
		for (FCollisionPolicyNodeState& NodeState : Data.Nodes)
		{
			UNiagaraNodeFunctionCall* Node = NodeState.Node.Get();
			if (!Node || !NodeState.bEditable)
			{
				continue;
			}
			bool bDesiredEnabled = true;
			if (IsCollisionPolicyNodeManaged(NodeState, Data.Policy, bDesiredEnabled))
			{
				const ENodeEnabledState DesiredState = bDesiredEnabled
					? ENodeEnabledState::Enabled
					: ENodeEnabledState::Disabled;
				bApplied &= Node->GetDesiredEnabledState() == DesiredState
					&& Node->HasUserSetTheEnabledState();
			}
		}
		for (const FCollisionPolicyDataInterfaceState& DIState : Data.DataInterfaces)
		{
			const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = DIState.DataInterface.Get();
			if (DataInterface && DIState.bEditable)
			{
				bApplied &= CollisionPolicyProviderName(DataInterface->TraceProvider)
					== DIState.AfterProvider;
			}
		}
		if (!bApplied || !CompileSummary.bCompiled)
		{
			FCompileSummary RestoreSummary;
			const bool bRestored = RestoreCollisionPolicyStates(
				System,
				Data.Graphs,
				Data.Nodes,
				Data.DataInterfaces,
				true,
				RestoreSummary);
			if (!bRestored || !RestoreSummary.bCompiled)
			{
				return ErrorResult(
					TEXT("Niagara collision policy failed and restoration could not be verified; the transaction was retained for Editor Undo."),
					TEXT("restore_verification_failed"), 500);
			}
			Transaction.Cancel();
			return ErrorResult(
				!bApplied
					? TEXT("Niagara collision policy read-back failed; the change was restored.")
					: FString::Printf(
						TEXT("Niagara collision policy compilation failed (status=%s); the change was restored."),
						*CompileSummary.AggregateStatus),
				!bApplied ? TEXT("verification_failed") : TEXT("compile_failed"),
				500);
		}

		FCollisionPolicyReceipt Receipt;
		Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Receipt.RequestId = RequestId;
		Receipt.PlanDigest = PlanDigest;
		Receipt.SystemPath = System->GetPathName();
		Receipt.Policy = Data.Policy;
		Receipt.System = System;
		Receipt.OrdinaryCollisionQueryCount = Data.OrdinaryCollisionQueryCount;
		Receipt.EditableOrdinaryCollisionQueryCount = Data.EditableOrdinaryCollisionQueryCount;
		Receipt.UnmanagedEnabledOrdinaryCollisionQueryCount =
			Data.UnmanagedEnabledOrdinaryCollisionQueryCount;
		Receipt.UnmanagedEnabledDistanceFieldCount =
			Data.UnmanagedEnabledDistanceFieldCount;
		for (FCollisionPolicyNodeState& NodeState : Data.Nodes)
		{
			if (UNiagaraNodeFunctionCall* Node = NodeState.Node.Get())
			{
				NodeState.AfterState = Node->GetDesiredEnabledState();
				NodeState.bAfterUserSet = Node->HasUserSetTheEnabledState();
			}
		}
		for (FCollisionPolicyDataInterfaceState& DIState : Data.DataInterfaces)
		{
			if (const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = DIState.DataInterface.Get())
			{
				DIState.AfterProvider = CollisionPolicyProviderName(DataInterface->TraceProvider);
				DIState.bProviderChanged = DIState.BeforeProvider != DIState.AfterProvider;
			}
		}

		// A receipt is a rollback boundary for mutations made by this apply, not
		// an inventory of every collision node inspected by the plan. Excluding
		// unmanaged and unchanged states prevents unrelated later edits from
		// blocking a rollback, especially for distanceField/hybrid policies.
		TSet<UNiagaraGraph*> AffectedGraphs;
		for (const FCollisionPolicyNodeState& NodeState : Data.Nodes)
		{
			bool bDesiredEnabled = true;
			if (!NodeState.bEditable
				|| !IsCollisionPolicyNodeManaged(NodeState, Data.Policy, bDesiredEnabled)
				|| (NodeState.BeforeState == NodeState.AfterState
					&& NodeState.bBeforeUserSet == NodeState.bAfterUserSet))
			{
				continue;
			}
			Receipt.Nodes.Add(NodeState);
			if (UNiagaraGraph* Graph = NodeState.Graph.Get())
			{
				AffectedGraphs.Add(Graph);
			}
		}
		for (const FCollisionPolicyDataInterfaceState& DIState : Data.DataInterfaces)
		{
			if (!DIState.bEditable || !DIState.bProviderChanged)
			{
				continue;
			}
			Receipt.DataInterfaces.Add(DIState);
			if (UNiagaraGraph* Graph = DIState.Graph.Get())
			{
				AffectedGraphs.Add(Graph);
			}
		}
		for (const FCollisionPolicyGraphState& GraphState : Data.Graphs)
		{
			UNiagaraGraph* Graph = GraphState.Graph.Get();
			if (!Graph || !AffectedGraphs.Contains(Graph))
			{
				continue;
			}
			FCollisionPolicyGraphState ReceiptGraph = GraphState;
			ReceiptGraph.AfterChangeId = Graph->GetChangeID().ToString(
				EGuidFormats::DigitsWithHyphensLower);
			Receipt.Graphs.Add(MoveTemp(ReceiptGraph));
		}
		Receipt.bCompiled = CompileSummary.bCompiled;
		Receipt.CompileStatus = CompileSummary.AggregateStatus;
		CollisionPolicyReceipts().Add(Receipt.ReceiptId, Receipt);
		CollisionPolicyRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
		return FMCPToolResult::Ok(MakeCollisionPolicyResult(Receipt, false));
	}
};

class FTool_NiagaraCollisionPolicyRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.collision.policy.rollback");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ReceiptId;
		FString RequestId;
		bool bConfirmWrite = false;
		if (!Params.IsValid())
		{
			return ErrorResult(TEXT("rollbackId, requestId and confirmWrite=true are required."), TEXT("write_confirmation_required"), 422);
		}
		Params->TryGetStringField(TEXT("rollbackId"), ReceiptId);
		Params->TryGetStringField(TEXT("requestId"), RequestId);
		Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite);
		if (ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirmWrite)
		{
			return ErrorResult(
				TEXT("rollbackId, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				422);
		}
		FCollisionPolicyReceipt* Receipt = CollisionPolicyReceipts().Find(ReceiptId);
		if (!Receipt)
		{
			return ErrorResult(
				TEXT("The Niagara collision policy receipt is unknown in this Editor instance."),
				TEXT("receipt_not_found"),
				404);
		}
		if (Receipt->bRolledBack)
		{
			return FMCPToolResult::Ok(MakeCollisionPolicyResult(*Receipt, true));
		}
		if (Receipt->RequestId != RequestId)
		{
			return ErrorResult(
				TEXT("requestId does not match the collision policy receipt."),
				TEXT("request_id_mismatch"),
				409);
		}
		UNiagaraSystem* System = Receipt->System.Get();
		if (!System)
		{
			return ErrorResult(TEXT("The Niagara System is no longer loaded."), TEXT("target_unavailable"), 409);
		}
		for (const FCollisionPolicyGraphState& GraphState : Receipt->Graphs)
		{
			UNiagaraGraph* Graph = GraphState.Graph.Get();
			if (!Graph)
			{
				return ErrorResult(
					TEXT("The Niagara collision policy graph changed after apply; rollback was refused."),
					TEXT("rollback_conflict"),
					409);
			}
			if (!GraphState.bEditable)
			{
				continue;
			}
			if (Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower)
				!= GraphState.AfterChangeId)
			{
				return ErrorResult(
					TEXT("The Niagara collision policy graph changed after apply; rollback was refused."),
					TEXT("rollback_conflict"),
					409);
			}
		}
		for (const FCollisionPolicyNodeState& State : Receipt->Nodes)
		{
			if (!State.bEditable)
			{
				continue;
			}
			const UNiagaraNodeFunctionCall* Node = State.Node.Get();
			if (!Node
				|| Node->GetDesiredEnabledState() != State.AfterState
				|| Node->HasUserSetTheEnabledState() != State.bAfterUserSet)
			{
				return ErrorResult(
					TEXT("A Niagara collision policy node changed after apply; rollback was refused."),
					TEXT("rollback_conflict"),
					409);
			}
		}
		for (const FCollisionPolicyDataInterfaceState& State : Receipt->DataInterfaces)
		{
			if (!State.bEditable)
			{
				continue;
			}
			const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = State.DataInterface.Get();
			if (!DataInterface
				|| CollisionPolicyProviderName(DataInterface->TraceProvider) != State.AfterProvider)
			{
				return ErrorResult(
					TEXT("An AsyncGpuTrace provider changed after apply; rollback was refused."),
					TEXT("rollback_conflict"),
					409);
			}
		}

		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Rollback Niagara Collision Policy")));
		FCompileSummary CompileSummary;
		if (!RestoreCollisionPolicyStates(
			System,
			Receipt->Graphs,
			Receipt->Nodes,
			Receipt->DataInterfaces,
			true,
			CompileSummary))
		{
			return ErrorResult(
				TEXT("Niagara collision policy rollback could not restore all provider values; the transaction was retained for Editor Undo."),
				TEXT("rollback_verification_failed"),
				500);
		}
		bool bVerified = true;
		for (const FCollisionPolicyNodeState& State : Receipt->Nodes)
		{
			if (State.bEditable)
			{
				const UNiagaraNodeFunctionCall* Node = State.Node.Get();
				bVerified &= Node
					&& Node->GetDesiredEnabledState() == State.BeforeState
					&& Node->HasUserSetTheEnabledState() == State.bBeforeUserSet;
			}
		}
		for (const FCollisionPolicyDataInterfaceState& State : Receipt->DataInterfaces)
		{
			if (State.bEditable)
			{
				const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = State.DataInterface.Get();
				bVerified &= DataInterface
					&& CollisionPolicyProviderName(DataInterface->TraceProvider) == State.BeforeProvider;
			}
		}
		if (!bVerified || (Receipt->Nodes.Num() > 0 || Receipt->DataInterfaces.Num() > 0)
			&& !CompileSummary.bCompiled)
		{
			return ErrorResult(
				TEXT("Niagara collision policy rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
				TEXT("rollback_verification_failed"),
				500);
		}
		Receipt->bCompiled = CompileSummary.bCompiled;
		Receipt->CompileStatus = CompileSummary.AggregateStatus;
		Receipt->bRolledBack = true;
		return FMCPToolResult::Ok(MakeCollisionPolicyResult(*Receipt, false));
	}
};

class FTool_NiagaraCollisionAudit final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.collision.audit");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FString SystemPath;
		FString EmitterSelector;
		FString GraphSelector;
		FString ErrorCode;
		FString Error;
		if (!GetSystemAndSelectors(
			Params,
			System,
			SystemPath,
			EmitterSelector,
			GraphSelector,
			ErrorCode,
			Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found") ? 404 : 422);
		}

		bool bIncludeDisabled = true;
		Params->TryGetBoolField(TEXT("includeDisabled"), bIncludeDisabled);
		int32 Limit = DefaultNodeLimit;
		if (Params->HasField(TEXT("limit")))
		{
			double Number = 0.0;
			if (!Params->TryGetNumberField(TEXT("limit"), Number)
				|| FMath::TruncToInt(Number) != Number)
			{
				return ErrorResult(
					TEXT("limit must be an integer."),
					TEXT("invalid_limit"));
			}
			Limit = FMath::Clamp(FMath::TruncToInt(Number), 1, MaxNodeLimit);
		}
		//++[UEAI] Begin implementation
		FString OperationSelector;
		if (Params->HasField(TEXT("operation"))
			&& !Params->TryGetStringField(TEXT("operation"), OperationSelector))
		{
			return ErrorResult(
				TEXT("operation must be a string."),
				TEXT("invalid_operation_selector"));
		}
		OperationSelector = OperationSelector.TrimStartAndEnd();
		//--[UEAI] End implementation

		TArray<FGraphTarget> Graphs;
		if (!CollectGraphs(
			System,
			EmitterSelector,
			GraphSelector,
			Graphs,
			ErrorCode,
			Error))
		{
			return ErrorResult(Error, ErrorCode, 404);
		}

		TArray<TSharedPtr<FJsonValue>> GraphValues;
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		UEAINiagaraGraphAuditExtensions::FNiagaraCollisionOperationInventory OperationInventory;
		int32 TotalNodes = 0;
		int32 OrdinaryCollisionQueryCount = 0;
		int32 DistanceFieldCount = 0;
		int32 AsyncGpuTraceCount = 0;
		int32 RigidMeshCollisionQueryCount = 0;
		int32 PhysicsAssetCount = 0;
		for (const FGraphTarget& Graph : Graphs)
		{
			if (!Graph.Graph)
			{
				continue;
			}
			TSharedRef<FJsonObject> GraphObject = SerializeGraphScope(Graph);
			TArray<TSharedPtr<FJsonValue>> GraphNodes;
			int32 GraphNodeTotal = 0;
			int32 GraphOrdinaryCount = 0;
			int32 GraphDistanceFieldCount = 0;
			int32 GraphAsyncCount = 0;
			int32 GraphRigidMeshCount = 0;
			int32 GraphPhysicsAssetCount = 0;
			TArray<TSharedPtr<FJsonValue>> GraphAsyncDataInterfaces;
			for (UEdGraphNode* RawNode : Graph.Graph->Nodes)
			{
				const UNiagaraNode* Node = Cast<UNiagaraNode>(RawNode);
				if (!Node || (!bIncludeDisabled && !Node->IsNodeEnabled()))
				{
					continue;
				}
				// An operation selector is intentionally evaluated against the
				// canonical inventory names. This also admits inventory-only support
				// modules (for example CalculateNeighbors) into the filtered view.
				const UNiagaraNodeFunctionCall* FunctionCall =
					Cast<UNiagaraNodeFunctionCall>(Node);
				FString MatchedOperation;
				const bool bOperationMatches =
					OperationSelector.IsEmpty()
					|| UEAINiagaraGraphAuditExtensions::MatchCollisionOperationSelector(
						FunctionCall,
						OperationSelector,
						&MatchedOperation);
				if (!bOperationMatches)
				{
					continue;
				}
				TArray<FString> Classes;
				bool bOrdinaryCollisionQuery = false;
				bool bDistanceField = false;
				bool bAsyncGpuTrace = false;
				ClassifyNode(
					Node,
					Classes,
					bOrdinaryCollisionQuery,
					bDistanceField,
					bAsyncGpuTrace);
				const bool bRigidMeshCollisionQuery =
					Classes.Contains(TEXT("rigidMeshCollisionQuery"));
				const bool bPhysicsAsset = Classes.Contains(TEXT("physicsAsset"));
				if (!bOrdinaryCollisionQuery
					&& !bDistanceField
					&& !bAsyncGpuTrace
					&& !bRigidMeshCollisionQuery
					&& !bPhysicsAsset
					&& OperationSelector.IsEmpty())
				{
					continue;
				}

				++GraphNodeTotal;
				++TotalNodes;
				if (bOrdinaryCollisionQuery)
				{
					++GraphOrdinaryCount;
					++OrdinaryCollisionQueryCount;
				}
				if (bDistanceField)
				{
					++GraphDistanceFieldCount;
					++DistanceFieldCount;
				}
				if (bAsyncGpuTrace)
				{
					++GraphAsyncCount;
					++AsyncGpuTraceCount;
				}
				if (bRigidMeshCollisionQuery)
				{
					++GraphRigidMeshCount;
					++RigidMeshCollisionQueryCount;
				}
				if (bPhysicsAsset)
				{
					++GraphPhysicsAssetCount;
					++PhysicsAssetCount;
				}

				if (NodeValues.Num() < Limit)
				{
					TSharedRef<FJsonObject> NodeObject = SerializeNode(Node);
					NodeObject->SetStringField(TEXT("graph"), Graph.Graph->GetPathName());
					if (!OperationSelector.IsEmpty())
					{
						NodeObject->SetStringField(TEXT("matchedOperation"), MatchedOperation);
					}
					TSharedPtr<FJsonValue> NodeValue =
						MakeShared<FJsonValueObject>(NodeObject);
					NodeValues.Add(NodeValue);
					GraphNodes.Add(NodeValue);
				}
			}
			if (GraphAsyncCount > 0)
			{
				for (UEdGraphNode* RawNode : Graph.Graph->Nodes)
				{
					UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(RawNode);
					UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
						FindAsyncTraceDataInterface(InputNode);
					if (!InputNode || !DataInterface)
					{
						continue;
					}
					TSharedRef<FJsonObject> DIObject = MakeShared<FJsonObject>();
					DIObject->SetStringField(TEXT("inputNode"), InputNode->GetPathName());
					DIObject->SetStringField(TEXT("dataInterface"), DataInterface->GetPathName());
					DIObject->SetStringField(TEXT("inputName"), InputNode->Input.GetName().ToString());
					DIObject->SetStringField(
						TEXT("traceProvider"),
						CollisionPolicyProviderName(DataInterface->TraceProvider));
					DIObject->SetBoolField(
						TEXT("defaultProviderFallbackRisk"),
						DataInterface->TraceProvider
							== ENDICollisionQuery_AsyncGpuTraceProvider::Default
							&& IsDefaultAsyncProviderFallbackRisk());
					GraphAsyncDataInterfaces.Add(MakeShared<FJsonValueObject>(DIObject));
				}
			}
			GraphObject->SetNumberField(TEXT("collisionNodeTotal"), GraphNodeTotal);
			GraphObject->SetNumberField(TEXT("ordinaryCollisionQueryCount"), GraphOrdinaryCount);
			GraphObject->SetNumberField(TEXT("distanceFieldCount"), GraphDistanceFieldCount);
			GraphObject->SetNumberField(TEXT("asyncGpuTraceCount"), GraphAsyncCount);
			GraphObject->SetNumberField(TEXT("rigidMeshCollisionQueryCount"), GraphRigidMeshCount);
			GraphObject->SetNumberField(TEXT("physicsAssetCount"), GraphPhysicsAssetCount);
			GraphObject->SetArrayField(TEXT("asyncGpuTraceDataInterfaces"), GraphAsyncDataInterfaces);
			GraphObject->SetNumberField(TEXT("nodeReturned"), GraphNodes.Num());
			GraphObject->SetBoolField(
				TEXT("nodesTruncated"),
				GraphNodes.Num() < GraphNodeTotal);
			GraphObject->SetArrayField(TEXT("nodes"), GraphNodes);
			UEAINiagaraGraphAuditExtensions::AppendDetailedCollisionAudit(
				Graph.Graph,
				bIncludeDisabled,
				Limit,
				GraphObject,
				&OperationInventory,
				OperationSelector);
			GraphValues.Add(MakeShared<FJsonValueObject>(GraphObject));
		}

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(
			TEXT("ordinaryCollisionQueryCount"),
			OrdinaryCollisionQueryCount);
		Summary->SetNumberField(TEXT("distanceFieldCount"), DistanceFieldCount);
		Summary->SetNumberField(TEXT("asyncGpuTraceCount"), AsyncGpuTraceCount);
		Summary->SetNumberField(TEXT("rigidMeshCollisionQueryCount"), RigidMeshCollisionQueryCount);
		Summary->SetNumberField(TEXT("physicsAssetCount"), PhysicsAssetCount);
		Summary->SetNumberField(TEXT("classifiedNodeTotal"), TotalNodes);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-collision-audit.v1"));
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(TEXT("package"), System->GetOutermost()->GetName());
		Result->SetBoolField(
			TEXT("packageDirty"),
			System->GetOutermost()->IsDirty());
		Result->SetBoolField(TEXT("includeDisabled"), bIncludeDisabled);
		//++[UEAI] Begin implementation
		Result->SetStringField(TEXT("operationSelector"), OperationSelector);
		const bool bOperationSelectorMatched =
			OperationSelector.IsEmpty() || OperationInventory.Num() > 0;
		Result->SetBoolField(
			TEXT("operationSelectorMatched"),
			bOperationSelectorMatched);
		TArray<FString> MatchedOperationNames;
		OperationInventory.GetKeys(MatchedOperationNames);
		MatchedOperationNames.Sort();
		Result->SetStringField(
			TEXT("operationSelectorCanonical"),
			MatchedOperationNames.Num() == 1
				? MatchedOperationNames[0]
				: FString());
		if (!OperationSelector.IsEmpty() && !bOperationSelectorMatched)
		{
			SetJsonStringArray(
				Result,
				TEXT("warnings"),
				TArray<FString>{FString::Printf(
					TEXT("No collision operation matched operation selector '%s'."),
					*OperationSelector)});
		}
		//--[UEAI] End implementation
		Result->SetNumberField(TEXT("graphCount"), GraphValues.Num());
		Result->SetObjectField(TEXT("summary"), Summary);
		Result->SetObjectField(TEXT("rayTracing"), SerializeRayTracingState());
		Result->SetArrayField(TEXT("graphs"), GraphValues);
		SetBoundedArray(Result, TEXT("nodes"), NodeValues, TotalNodes, Limit);
		UEAINiagaraGraphAuditExtensions::SetCollisionOperationInventory(
			Result,
			OperationInventory,
			Limit);
		return FMCPToolResult::Ok(Result);
	}
};

bool BuildNodeEnabledPlan(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutPlan,
	FNodeTarget& OutTarget,
	FString& OutErrorCode,
	FString& OutError)
{
	UNiagaraSystem* System = nullptr;
	FString SystemPath;
	FString EmitterSelector;
	FString GraphSelector;
	if (!GetSystemAndSelectors(
		Params,
		System,
		SystemPath,
		EmitterSelector,
		GraphSelector,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	if (!IsProjectSystem(System))
	{
		OutErrorCode = TEXT("asset_scope_forbidden");
		OutError = TEXT("Niagara graph writes are restricted to project assets under /Game.");
		return false;
	}
	bool bEnabled = false;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		OutErrorCode = TEXT("invalid_enabled");
		OutError = TEXT("enabled is required and must be boolean.");
		return false;
	}

	FString MutationScope = TEXT("node");
	if (Params->HasField(TEXT("mutationScope")))
	{
		if (!Params->TryGetStringField(TEXT("mutationScope"), MutationScope)
			|| (MutationScope != TEXT("node") && MutationScope != TEXT("module")))
		{
			OutErrorCode = TEXT("invalid_mutation_scope");
			OutError = TEXT("mutationScope must be 'node' or 'module'.");
			return false;
		}
	}

	FString Persistence = TEXT("dirtyOnly");
	if (Params->HasField(TEXT("persistence")))
	{
		if (!Params->TryGetStringField(TEXT("persistence"), Persistence)
			|| Persistence != TEXT("dirtyOnly"))
		{
			OutErrorCode = TEXT("invalid_persistence");
			OutError = TEXT("Only persistence='dirtyOnly' is supported for Niagara graph edits.");
			return false;
		}
	}

	if (!ResolveNode(Params, System, OutTarget, OutErrorCode, OutError))
	{
		return false;
	}
	if (!OutTarget.Graph.bOwnedBySystem
		|| !OutTarget.Graph.Graph
		|| OutTarget.Graph.Graph->GetOutermost() != System->GetOutermost())
	{
		OutErrorCode = TEXT("graph_scope_forbidden");
		OutError = TEXT("Niagara graph writes may only target a graph owned by the selected System package; shared /Niagara/ graphs are read-only.");
		return false;
	}
	if (MutationScope == TEXT("module"))
	{
		if (OutTarget.Graph.ReferenceDepth != 0
			|| !IsTopLevelStackModule(OutTarget.Graph.Graph, OutTarget.FunctionCall))
		{
			OutErrorCode = TEXT("unsupported_module_scope");
			OutError = TEXT("module mutation requires a top-level function-call module on a valid Niagara parameter-map stack; use mutationScope='node' for nested function graphs.");
			return false;
		}
	}

	UNiagaraNodeFunctionCall* Node = OutTarget.FunctionCall;
	UNiagaraGraph* Graph = OutTarget.Graph.Graph;
	const ENodeEnabledState BeforeState = Node->GetDesiredEnabledState();
	const bool bBeforeUserSet = Node->HasUserSetTheEnabledState();
	const ENodeEnabledState AfterState = bEnabled
		? ENodeEnabledState::Enabled
		: ENodeEnabledState::Disabled;

	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	Before->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());
	Before->SetStringField(TEXT("desiredEnabledState"), EnabledStateName(BeforeState));
	Before->SetBoolField(TEXT("userSetEnabledState"), bBeforeUserSet);

	TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
	After->SetBoolField(TEXT("enabled"), bEnabled);
	After->SetStringField(TEXT("desiredEnabledState"), EnabledStateName(AfterState));
	After->SetBoolField(TEXT("userSetEnabledState"), true);

	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("system"), SystemPath);
	if (!EmitterSelector.IsEmpty())
	{
		Request->SetStringField(TEXT("emitter"), EmitterSelector);
	}
	if (!GraphSelector.IsEmpty())
	{
		Request->SetStringField(TEXT("graph"), GraphSelector);
	}
	if (Params->HasField(TEXT("nodePath")))
	{
		Request->SetStringField(TEXT("nodePath"), Params->GetStringField(TEXT("nodePath")));
	}
	else
	{
		Request->SetStringField(TEXT("nodeGuid"), Params->GetStringField(TEXT("nodeGuid")));
	}
	Request->SetBoolField(TEXT("enabled"), bEnabled);
	Request->SetStringField(TEXT("mutationScope"), MutationScope);
	Request->SetStringField(TEXT("persistence"), Persistence);

	TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
	Preconditions->SetStringField(TEXT("system"), System->GetPathName());
	Preconditions->SetStringField(TEXT("graph"), Graph->GetPathName());
	Preconditions->SetStringField(
		TEXT("graphChangeId"),
		Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower));
	Preconditions->SetStringField(TEXT("nodePath"), Node->GetPathName());
	Preconditions->SetStringField(
		TEXT("nodeGuid"),
		Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	Preconditions->SetBoolField(
		TEXT("packageDirty"),
		System->GetOutermost()->IsDirty());

	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
	Plan->SetStringField(TEXT("planKind"), TEXT("niagaraNodeEnabled"));
	Plan->SetStringField(TEXT("action"), TEXT("setFunctionCallEnabled"));
	Plan->SetStringField(TEXT("mutationScope"), MutationScope);
	Plan->SetStringField(TEXT("scope"), System->GetPathName());
	Plan->SetStringField(TEXT("persistence"), Persistence);
	Plan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	Plan->SetBoolField(TEXT("changesState"), BeforeState != AfterState);
	Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
	Plan->SetObjectField(TEXT("request"), Request);
	Plan->SetObjectField(TEXT("preconditions"), Preconditions);
	Plan->SetObjectField(TEXT("before"), Before);
	Plan->SetObjectField(TEXT("after"), After);
	Plan->SetStringField(TEXT("system"), System->GetPathName());
	Plan->SetStringField(TEXT("graph"), Graph->GetPathName());
	Plan->SetStringField(TEXT("nodePath"), Node->GetPathName());
	Plan->SetStringField(
		TEXT("nodeGuid"),
		Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	FString PlanDigest;
	if (!TryDigestJson(Plan, PlanDigest))
	{
		OutErrorCode = TEXT("digest_unavailable");
		OutError = TEXT("Unable to compute the Niagara graph change plan digest.");
		return false;
	}
	Plan->SetStringField(TEXT("planDigest"), PlanDigest);
	OutPlan = Plan;
	return true;
}

TSharedRef<FJsonObject> MakeMutationResult(
	const FMutationReceipt& Receipt,
	const bool bReplay)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-node-mutation.v1"));
	Result->SetStringField(TEXT("status"), TEXT("succeeded"));
	Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
	Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
	Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
	Result->SetStringField(TEXT("system"), Receipt.SystemPath);
	Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
	Result->SetStringField(TEXT("nodePath"), Receipt.NodePath);
	Result->SetStringField(TEXT("mutationScope"), Receipt.MutationScope);
	Result->SetNumberField(TEXT("affectedNodeCount"), Receipt.AffectedNodes.Num());
	bool bChanged = false;
	for (const FMutationReceipt::FNodeState& State : Receipt.AffectedNodes)
	{
		bChanged |= State.BeforeState != State.AfterState
			|| State.bBeforeUserSet != State.bAfterUserSet;
	}
	Result->SetBoolField(TEXT("changed"), bChanged);
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
	Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
	Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
	Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	return Result;
}

FString CompileStatusName(const ENiagaraScriptCompileStatus Status)
{
	switch (Status)
	{
	case ENiagaraScriptCompileStatus::NCS_Unknown:
		return TEXT("unknown");
	case ENiagaraScriptCompileStatus::NCS_Dirty:
		return TEXT("dirty");
	case ENiagaraScriptCompileStatus::NCS_Error:
		return TEXT("error");
	case ENiagaraScriptCompileStatus::NCS_UpToDate:
		return TEXT("upToDate");
	case ENiagaraScriptCompileStatus::NCS_BeingCreated:
		return TEXT("beingCreated");
	case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		return TEXT("upToDateWithWarnings");
	case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
		return TEXT("computeUpToDateWithWarnings");
	default:
		return TEXT("unknown");
	}
}

FCompileSummary RequestAndSummarizeCompile(UNiagaraSystem* System)
{
	FCompileSummary Summary;
	if (!System)
	{
		return Summary;
	}

	System->RequestCompile(false);
	System->WaitForCompilationComplete(true, false);

	TArray<UNiagaraScript*> Scripts;
	TSet<UNiagaraScript*> SeenScripts;
	const auto AddScript = [&Scripts, &SeenScripts](UNiagaraScript* Script)
	{
		if (Script && !SeenScripts.Contains(Script))
		{
			SeenScripts.Add(Script);
			Scripts.Add(Script);
		}
	};
	AddScript(System->GetSystemSpawnScript());
	AddScript(System->GetSystemUpdateScript());
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			EmitterData->ForEachScript(
				[&AddScript](UNiagaraScript* Script)
				{
					AddScript(Script);
				});
		}
	}

	bool bAnyKnown = false;
	bool bAnyDirty = false;
	bool bAnyBeingCreated = false;
	bool bAnyWarning = false;
	bool bAnyUpToDate = false;
	for (UNiagaraScript* Script : Scripts)
	{
		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		Summary.Scripts.Emplace(Script->GetPathName(), CompileStatusName(Status));
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Error:
			Summary.bHasError = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_Dirty:
			bAnyDirty = true;
			bAnyKnown = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:
			bAnyBeingCreated = true;
			bAnyKnown = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
			bAnyWarning = true;
			bAnyKnown = true;
			bAnyUpToDate = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			bAnyUpToDate = true;
			bAnyKnown = true;
			break;
		default:
			break;
		}
	}

	if (Summary.bHasError)
	{
		Summary.AggregateStatus = TEXT("error");
	}
	else if (bAnyBeingCreated)
	{
		Summary.AggregateStatus = TEXT("beingCreated");
	}
	else if (bAnyDirty)
	{
		Summary.AggregateStatus = TEXT("dirty");
	}
	else if (bAnyWarning)
	{
		Summary.AggregateStatus = TEXT("upToDateWithWarnings");
	}
	else if (bAnyUpToDate)
	{
		Summary.AggregateStatus = TEXT("upToDate");
	}
	else if (!bAnyKnown)
	{
		Summary.AggregateStatus = TEXT("unknown");
	}
	Summary.bCompiled = !Summary.bHasError
		&& !bAnyDirty
		&& !bAnyBeingCreated
		&& bAnyKnown;
	return Summary;
}

TSharedRef<FJsonObject> SerializeCompileSummary(const FCompileSummary& Summary)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), Summary.AggregateStatus);
	Result->SetBoolField(TEXT("compiled"), Summary.bCompiled);
	Result->SetBoolField(TEXT("hasError"), Summary.bHasError);
	TArray<TSharedPtr<FJsonValue>> Scripts;
	for (const TPair<FString, FString>& Entry : Summary.Scripts)
	{
		TSharedRef<FJsonObject> Script = MakeShared<FJsonObject>();
		Script->SetStringField(TEXT("path"), Entry.Key);
		Script->SetStringField(TEXT("status"), Entry.Value);
		Scripts.Add(MakeShared<FJsonValueObject>(Script));
	}
	Result->SetArrayField(TEXT("scripts"), Scripts);
	return Result;
}

TArray<FMutationReceipt::FNodeState> CaptureNodeStates(
	UNiagaraGraph* Graph,
	UNiagaraNode* OnlyNode = nullptr)
{
	TArray<FMutationReceipt::FNodeState> States;
	if (!Graph)
	{
		return States;
	}
	if (OnlyNode)
	{
		FMutationReceipt::FNodeState State;
		State.Node = OnlyNode;
		State.NodePath = OnlyNode->GetPathName();
		State.BeforeState = OnlyNode->GetDesiredEnabledState();
		State.bBeforeUserSet = OnlyNode->HasUserSetTheEnabledState();
		State.AfterState = State.BeforeState;
		State.bAfterUserSet = State.bBeforeUserSet;
		States.Add(MoveTemp(State));
		return States;
	}
	for (UEdGraphNode* RawNode : Graph->Nodes)
	{
		UNiagaraNode* Node = Cast<UNiagaraNode>(RawNode);
		if (!Node)
		{
			continue;
		}
		FMutationReceipt::FNodeState State;
		State.Node = Node;
		State.NodePath = Node->GetPathName();
		State.BeforeState = Node->GetDesiredEnabledState();
		State.bBeforeUserSet = Node->HasUserSetTheEnabledState();
		State.AfterState = State.BeforeState;
		State.bAfterUserSet = State.bBeforeUserSet;
		States.Add(MoveTemp(State));
	}
	return States;
}

void CaptureAfterStates(TArray<FMutationReceipt::FNodeState>& States)
{
	for (FMutationReceipt::FNodeState& State : States)
	{
		if (const UNiagaraNode* Node = State.Node.Get())
		{
			State.AfterState = Node->GetDesiredEnabledState();
			State.bAfterUserSet = Node->HasUserSetTheEnabledState();
		}
	}
}

TArray<FMutationReceipt::FNodeState> BuildAffectedStates(
	const TArray<FMutationReceipt::FNodeState>& States,
	const UNiagaraNode* TargetNode)
{
	TArray<FMutationReceipt::FNodeState> Affected;
	for (const FMutationReceipt::FNodeState& State : States)
	{
		const UNiagaraNode* Node = State.Node.Get();
		if (Node
			&& (Node == TargetNode
				|| State.BeforeState != State.AfterState
				|| State.bBeforeUserSet != State.bAfterUserSet))
		{
			Affected.Add(State);
		}
	}
	return Affected;
}

bool IsNodeStateEnabled(const UNiagaraNode* Node, const bool bEnabled)
{
	return Node
		&& Node->IsNodeEnabled() == bEnabled
		&& Node->GetDesiredEnabledState()
			== (bEnabled ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled);
}

FCompileSummary RestoreNodeStates(
	UNiagaraSystem* System,
	UNiagaraGraph* Graph,
	TArray<FMutationReceipt::FNodeState>& States)
{
	FCompileSummary Summary;
	if (!System || !Graph)
	{
		return Summary;
	}
	System->Modify();
	Graph->Modify();
	bool bChanged = false;
	bool bRaisedGraphRecompile = false;
	for (FMutationReceipt::FNodeState& State : States)
	{
		UNiagaraNode* Node = State.Node.Get();
		if (!Node)
		{
			continue;
		}
		const bool bNeedsChange = Node->GetDesiredEnabledState() != State.BeforeState
			|| Node->HasUserSetTheEnabledState() != State.bBeforeUserSet;
		Node->Modify();
		Node->SetEnabledState(State.BeforeState, State.bBeforeUserSet);
		if (bNeedsChange)
		{
			Node->MarkNodeRequiresSynchronization(
				TEXT("UE AI Niagara node state restored"),
				!bRaisedGraphRecompile);
			bRaisedGraphRecompile = true;
			bChanged = true;
		}
	}
	if (bChanged)
	{
		System->MarkPackageDirty();
		Summary = RequestAndSummarizeCompile(System);
	}
	return Summary;
}

TSharedRef<FJsonObject> MakePinMutationResult(
	const FPinMutationReceipt& Receipt,
	const bool bReplay)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-pin-mutation.v1"));
	Result->SetStringField(TEXT("status"), TEXT("succeeded"));
	Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
	Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
	Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
	Result->SetStringField(TEXT("system"), Receipt.SystemPath);
	Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
	Result->SetStringField(TEXT("node"), Receipt.NodePath);
	Result->SetStringField(TEXT("pinName"), Receipt.PinName);
	Result->SetStringField(TEXT("beforeValue"), Receipt.BeforeValue);
	Result->SetStringField(TEXT("afterValue"), Receipt.AfterValue);
	Result->SetBoolField(
		TEXT("changed"),
		Receipt.BeforeValue != Receipt.AfterValue);
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
	Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
	Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
	Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	return Result;
}

bool BuildPinDefaultPlan(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutPlan,
	FPinTarget& OutTarget,
	FString& OutErrorCode,
	FString& OutError)
{
	UNiagaraSystem* System = nullptr;
	FString SystemPath;
	FString EmitterSelector;
	FString GraphSelector;
	if (!GetSystemAndSelectors(
		Params,
		System,
		SystemPath,
		EmitterSelector,
		GraphSelector,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	if (!IsProjectSystem(System))
	{
		OutErrorCode = TEXT("asset_scope_forbidden");
		OutError = TEXT("Niagara graph writes are restricted to project assets under /Game.");
		return false;
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		OutErrorCode = TEXT("invalid_pin_value");
		OutError = TEXT("value is required and must be a string in Niagara pin format.");
		return false;
	}

	FString Persistence = TEXT("dirtyOnly");
	if (Params->HasField(TEXT("persistence")))
	{
		if (!Params->TryGetStringField(TEXT("persistence"), Persistence)
			|| Persistence != TEXT("dirtyOnly"))
		{
			OutErrorCode = TEXT("invalid_persistence");
			OutError = TEXT("Only persistence='dirtyOnly' is supported for Niagara graph edits.");
			return false;
		}
	}

	FNodeTarget NodeTarget;
	if (!ResolvePin(
		Params,
		System,
		OutTarget,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	NodeTarget = OutTarget.Node;
	UNiagaraGraph* Graph = NodeTarget.Graph.Graph;
	UNiagaraNode* Node = NodeTarget.Node;
	UEdGraphPin* Pin = OutTarget.Pin;
	if (!Graph || !Node || !Pin)
	{
		OutErrorCode = TEXT("target_unavailable");
		OutError = TEXT("The Niagara pin target is unavailable.");
		return false;
	}

	const FString BeforeValue = Pin->DefaultValue;
	const bool bChangesState = BeforeValue != Value;
	TSharedRef<FJsonObject> Before = SerializePin(Node, Pin);
	TSharedRef<FJsonObject> After = SerializePin(Node, Pin);
	After->SetStringField(TEXT("defaultValue"), Value);

	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("system"), SystemPath);
	if (!EmitterSelector.IsEmpty())
	{
		Request->SetStringField(TEXT("emitter"), EmitterSelector);
	}
	if (!GraphSelector.IsEmpty())
	{
		Request->SetStringField(TEXT("graph"), GraphSelector);
	}
	if (Params->HasField(TEXT("nodePath")))
	{
		Request->SetStringField(TEXT("nodePath"), Params->GetStringField(TEXT("nodePath")));
	}
	else
	{
		Request->SetStringField(TEXT("nodeGuid"), Params->GetStringField(TEXT("nodeGuid")));
	}
	Request->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
	Request->SetStringField(TEXT("value"), Value);
	Request->SetStringField(TEXT("persistence"), Persistence);

	TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
	Preconditions->SetStringField(TEXT("system"), System->GetPathName());
	Preconditions->SetStringField(TEXT("graph"), Graph->GetPathName());
	Preconditions->SetStringField(
		TEXT("graphChangeId"),
		Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower));
	Preconditions->SetStringField(TEXT("node"), Node->GetPathName());
	Preconditions->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
	Preconditions->SetStringField(TEXT("beforeValue"), BeforeValue);
	Preconditions->SetBoolField(
		TEXT("packageDirty"),
		System->GetOutermost() && System->GetOutermost()->IsDirty());

	TSharedRef<FJsonObject> DigestInput = MakeShared<FJsonObject>();
	DigestInput->SetStringField(TEXT("contract"), TEXT("ue.change-plan.v1"));
	DigestInput->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
	DigestInput->SetStringField(TEXT("planKind"), TEXT("niagaraPinDefault"));
	DigestInput->SetObjectField(TEXT("request"), Request);
	DigestInput->SetObjectField(TEXT("preconditions"), Preconditions);
	DigestInput->SetObjectField(TEXT("before"), Before);
	DigestInput->SetObjectField(TEXT("after"), After);
	const FString PlanDigest = DigestJson(DigestInput);
	if (PlanDigest.IsEmpty())
	{
		OutErrorCode = TEXT("digest_unavailable");
		OutError = TEXT("Unable to compute the Niagara pin change plan digest.");
		return false;
	}

	OutPlan = MakeShared<FJsonObject>();
	OutPlan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	OutPlan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
	OutPlan->SetStringField(TEXT("planKind"), TEXT("niagaraPinDefault"));
	OutPlan->SetStringField(TEXT("action"), TEXT("setPinDefault"));
	OutPlan->SetStringField(TEXT("scope"), System->GetPathName());
	OutPlan->SetStringField(TEXT("persistence"), Persistence);
	OutPlan->SetStringField(TEXT("planDigest"), PlanDigest);
	OutPlan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	OutPlan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	OutPlan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	OutPlan->SetBoolField(TEXT("changesState"), bChangesState);
	OutPlan->SetBoolField(TEXT("confirmWriteRequired"), true);
	OutPlan->SetObjectField(TEXT("request"), Request);
	OutPlan->SetObjectField(TEXT("preconditions"), Preconditions);
	OutPlan->SetObjectField(TEXT("before"), Before);
	OutPlan->SetObjectField(TEXT("after"), After);
	OutPlan->SetStringField(TEXT("system"), System->GetPathName());
	OutPlan->SetStringField(TEXT("graph"), Graph->GetPathName());
	OutPlan->SetStringField(TEXT("node"), Node->GetPathName());
	OutPlan->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
	return true;
}

bool ApplyPinDefaultValue(
	UNiagaraSystem* System,
	UNiagaraGraph* Graph,
	UNiagaraNode* Node,
	UEdGraphPin* Pin,
	const FString& Value)
{
	if (!System || !Graph || !Node || !Pin)
	{
		return false;
	}
	const UEdGraphSchema_Niagara* Schema = GetDefault<UEdGraphSchema_Niagara>();
	if (!Schema)
	{
		return false;
	}
	System->Modify();
	Graph->Modify();
	Node->Modify();
	Pin->Modify();
	Schema->TrySetDefaultValue(*Pin, Value, true);
	Node->MarkNodeRequiresSynchronization(
		TEXT("UE AI Niagara pin default value changed"),
		true);
	Graph->NotifyGraphChanged();
	System->MarkPackageDirty();
	return Pin->DefaultValue == Value;
}

class FTool_NiagaraPinDefaultPlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.pin.set_default.plan");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Plan;
		FPinTarget IgnoredTarget;
		FString ErrorCode;
		FString Error;
		if (!BuildPinDefaultPlan(
			Params,
			Plan,
			IgnoredTarget,
			ErrorCode,
			Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					|| ErrorCode == TEXT("node_not_found")
					|| ErrorCode == TEXT("pin_not_found")
					? 404
					: 422);
		}
		return FMCPToolResult::Ok(Plan);
	}
};

class FTool_NiagaraPinDefaultApply final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.pin.set_default.apply");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RequestId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty())
		{
			return ErrorResult(
				TEXT("A non-empty requestId is required for Niagara graph writes."),
				TEXT("request_id_required"),
				422);
		}

		if (const FString* ExistingReceiptId = PinRequestReceiptIds().Find(RequestId))
		{
			if (FPinMutationReceipt* Existing = PinMutationReceipts().Find(*ExistingReceiptId))
			{
				FString ErrorCode;
				FString Error;
				if (!ValidateChangeApproval(
					Params,
					Existing->PlanDigest,
					ErrorCode,
					Error))
				{
					return ErrorResult(Error, ErrorCode, 409);
				}
				return FMCPToolResult::Ok(MakePinMutationResult(*Existing, true));
			}
			return ErrorResult(
				TEXT("requestId is associated with an unavailable Niagara pin mutation."),
				TEXT("request_id_conflict"),
				409);
		}

		TSharedPtr<FJsonObject> Plan;
		FPinTarget Target;
		FString ErrorCode;
		FString Error;
		if (!BuildPinDefaultPlan(
			Params,
			Plan,
			Target,
			ErrorCode,
			Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					|| ErrorCode == TEXT("node_not_found")
					|| ErrorCode == TEXT("pin_not_found")
					? 404
					: 422);
		}
		if (!ValidateChangeApproval(
			Params,
			Plan->GetStringField(TEXT("planDigest")),
			ErrorCode,
			Error))
		{
			return ErrorResult(Error, ErrorCode, 409);
		}

		UNiagaraSystem* System = nullptr;
		FString LoadedSystemPath;
		if (!LoadSystem(
			Plan->GetStringField(TEXT("system")),
			System,
			LoadedSystemPath,
			ErrorCode,
			Error))
		{
			return ErrorResult(Error, ErrorCode, 404);
		}
		UNiagaraGraph* Graph = Target.Node.Graph.Graph;
		UNiagaraNode* Node = Target.Node.Node;
		UEdGraphPin* Pin = Target.Pin;
		if (!Graph || !Node || !Pin
			|| Graph->GetPathName() != Plan->GetStringField(TEXT("graph"))
			|| Node->GetPathName() != Plan->GetStringField(TEXT("node"))
			|| Pin->PinName.ToString() != Plan->GetStringField(TEXT("pinName")))
		{
			return ErrorResult(
				TEXT("The Niagara pin target changed while applying the plan."),
				TEXT("target_changed"),
				409);
		}

		const TSharedPtr<FJsonObject> Preconditions =
			Plan->GetObjectField(TEXT("preconditions"));
		const FString CurrentChangeId = Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		if (CurrentChangeId != Preconditions->GetStringField(TEXT("graphChangeId")))
		{
			return ErrorResult(
				TEXT("The Niagara graph changed after the plan was created; re-plan before applying."),
				TEXT("plan_digest_mismatch"),
				409);
		}
		const FString BeforeValue = Preconditions->GetStringField(TEXT("beforeValue"));
		if (Pin->DefaultValue != BeforeValue)
		{
			return ErrorResult(
				TEXT("The Niagara pin default changed after the plan was created; re-plan before applying."),
				TEXT("plan_digest_mismatch"),
				409);
		}
		const FString AfterValue = Plan->GetObjectField(TEXT("after"))->GetStringField(TEXT("defaultValue"));

		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Set Niagara Pin Default")));
		const bool bApplied = ApplyPinDefaultValue(
			System,
			Graph,
			Node,
			Pin,
			AfterValue);
		const FCompileSummary CompileSummary = RequestAndSummarizeCompile(System);
		const bool bVerified = bApplied
			&& Pin->DefaultValue == AfterValue
			&& CompileSummary.bCompiled;
		if (!bVerified)
		{
			const bool bRestored = ApplyPinDefaultValue(System, Graph, Node, Pin, BeforeValue);
			const FCompileSummary RestoreSummary = RequestAndSummarizeCompile(System);
			if (!bRestored || Pin->DefaultValue != BeforeValue || !RestoreSummary.bCompiled)
			{
				return ErrorResult(
					TEXT("Niagara pin edit failed and restoration could not be verified; the transaction was retained for Editor Undo."),
					TEXT("restore_verification_failed"), 500);
			}
			Transaction.Cancel();
			return ErrorResult(
				FString::Printf(
					TEXT("Niagara pin read-back or compilation failed (status=%s). The change was restored."),
					*CompileSummary.AggregateStatus),
				TEXT("compile_failed"),
				500);
		}

		FPinMutationReceipt Receipt;
		Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Receipt.RequestId = RequestId;
		Receipt.PlanDigest = Plan->GetStringField(TEXT("planDigest"));
		Receipt.SystemPath = System->GetPathName();
		Receipt.GraphPath = Graph->GetPathName();
		Receipt.NodePath = Node->GetPathName();
		Receipt.PinName = Pin->PinName.ToString();
		Receipt.BeforeValue = BeforeValue;
		Receipt.AfterValue = AfterValue;
		Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		Receipt.System = System;
		Receipt.Graph = Graph;
		Receipt.Node = Node;
		Receipt.bCompiled = CompileSummary.bCompiled;
		Receipt.CompileStatus = CompileSummary.AggregateStatus;
		PinMutationReceipts().Add(Receipt.ReceiptId, Receipt);
		PinRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
		return FMCPToolResult::Ok(MakePinMutationResult(Receipt, false));
	}
};

class FTool_NiagaraPinDefaultRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.pin.set_default.rollback");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ReceiptId;
		FString RequestId;
		bool bConfirmWrite = false;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId)
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
			|| ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirmWrite)
		{
			return ErrorResult(
				TEXT("rollbackId, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				422);
		}

		FPinMutationReceipt* Receipt = PinMutationReceipts().Find(ReceiptId);
		if (!Receipt)
		{
			return ErrorResult(
				TEXT("The Niagara pin mutation receipt is unknown in this Editor instance."),
				TEXT("receipt_not_found"),
				404);
		}
		if (Receipt->bRolledBack)
		{
			return FMCPToolResult::Ok(MakePinMutationResult(*Receipt, true));
		}
		if (Receipt->RequestId != RequestId)
		{
			return ErrorResult(
				TEXT("requestId does not match the pin mutation receipt."),
				TEXT("request_id_mismatch"),
				409);
		}

		UNiagaraSystem* System = Receipt->System.Get();
		UNiagaraGraph* Graph = Receipt->Graph.Get();
		UNiagaraNode* Node = Receipt->Node.Get();
		if (!System || !Graph || !Node)
		{
			return ErrorResult(
				TEXT("The Niagara pin mutation target is no longer loaded."),
				TEXT("target_unavailable"),
				409);
		}
		UEdGraphPin* Pin = nullptr;
		for (UEdGraphPin* Candidate : Node->Pins)
		{
			if (Candidate
				&& Candidate->Direction == EGPD_Input
				&& Candidate->PinName.ToString() == Receipt->PinName)
			{
				Pin = Candidate;
				break;
			}
		}
		const FString CurrentChangeId = Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		if (!Pin || CurrentChangeId != Receipt->GraphChangeIdAfter
			|| Pin->DefaultValue != Receipt->AfterValue)
		{
			return ErrorResult(
				TEXT("The Niagara graph or pin changed after apply; rollback was refused to avoid overwriting newer edits."),
				TEXT("rollback_conflict"),
				409);
		}

		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Rollback Niagara Pin Default")));
		const bool bApplied = ApplyPinDefaultValue(
			System,
			Graph,
			Node,
			Pin,
			Receipt->BeforeValue);
		const FCompileSummary CompileSummary = RequestAndSummarizeCompile(System);
		if (!bApplied || Pin->DefaultValue != Receipt->BeforeValue || !CompileSummary.bCompiled)
		{
			return ErrorResult(
			TEXT("Niagara pin rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
			TEXT("rollback_verification_failed"),
			500);
		}
		Receipt->bCompiled = CompileSummary.bCompiled;
		Receipt->CompileStatus = CompileSummary.AggregateStatus;
		Receipt->bRolledBack = true;
		return FMCPToolResult::Ok(MakePinMutationResult(*Receipt, false));
	}
};

class FTool_NiagaraNodeEnabledPlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.node.set_enabled.plan"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Plan;
		FNodeTarget IgnoredTarget;
		FString ErrorCode;
		FString Error;
		if (!BuildNodeEnabledPlan(
			Params,
			Plan,
			IgnoredTarget,
			ErrorCode,
			Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("node_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					? 404
					: 422);
		}
		return FMCPToolResult::Ok(Plan);
	}
};

class FTool_NiagaraNodeEnabledApply final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.node.set_enabled.apply"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RequestId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty())
		{
			return ErrorResult(
				TEXT("A non-empty requestId is required for Niagara graph writes."),
				TEXT("request_id_required"),
				422);
		}

		if (const FString* ExistingReceiptId = RequestReceiptIds().Find(RequestId))
		{
			if (FMutationReceipt* Existing = MutationReceipts().Find(*ExistingReceiptId))
			{
				FString ErrorCode;
				FString Error;
				if (!ValidateChangeApproval(
					Params,
					Existing->PlanDigest,
					ErrorCode,
					Error))
				{
					return ErrorResult(Error, ErrorCode, 409);
				}
				return FMCPToolResult::Ok(MakeMutationResult(*Existing, true));
			}
			return ErrorResult(
				TEXT("requestId is associated with an unavailable Niagara mutation."),
				TEXT("request_id_conflict"),
				409);
		}

		TSharedPtr<FJsonObject> Plan;
		FNodeTarget Target;
		FString ErrorCode;
		FString Error;
		if (!BuildNodeEnabledPlan(
			Params,
			Plan,
			Target,
			ErrorCode,
			Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("node_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					? 404
					: 422);
		}

		if (!ValidateChangeApproval(
			Params,
			Plan->GetStringField(TEXT("planDigest")),
			ErrorCode,
			Error))
		{
			return ErrorResult(Error, ErrorCode, 409);
		}

		UNiagaraSystem* System = nullptr;
		FString SystemPath;
		if (!LoadSystem(
			Plan->GetStringField(TEXT("system")),
			System,
			SystemPath,
			ErrorCode,
			Error))
		{
			return ErrorResult(Error, ErrorCode, 404);
		}

		UNiagaraGraph* Graph = Target.Graph.Graph;
		UNiagaraNodeFunctionCall* Node = Target.FunctionCall;
		if (!Graph || !Node
			|| Graph->GetPathName() != Plan->GetStringField(TEXT("graph"))
			|| Node->GetPathName() != Plan->GetStringField(TEXT("nodePath")))
		{
			return ErrorResult(
				TEXT("The Niagara graph target changed while applying the plan."),
				TEXT("target_changed"),
				409);
		}

		const TSharedPtr<FJsonObject> Preconditions =
			Plan->GetObjectField(TEXT("preconditions"));
		const FString CurrentChangeId = Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		if (CurrentChangeId != Preconditions->GetStringField(TEXT("graphChangeId")))
		{
			return ErrorResult(
				TEXT("The Niagara graph changed after the plan was created; re-plan before applying."),
				TEXT("plan_digest_mismatch"),
				409);
		}

		const TSharedPtr<FJsonObject> After = Plan->GetObjectField(TEXT("after"));
		const bool bEnabled = After->GetBoolField(TEXT("enabled"));
		const FString MutationScope = Plan->GetStringField(TEXT("mutationScope"));
		const bool bModuleMutation = MutationScope == TEXT("module");
		const ENodeEnabledState BeforeState = Node->GetDesiredEnabledState();
		const bool bBeforeUserSet = Node->HasUserSetTheEnabledState();
		TArray<FMutationReceipt::FNodeState> BeforeStates = CaptureNodeStates(
			Graph,
			bModuleMutation ? nullptr : static_cast<UNiagaraNode*>(Node));
		if (BeforeStates.IsEmpty())
		{
			return ErrorResult(
				TEXT("The Niagara graph did not contain a mutable node state snapshot."),
				TEXT("target_unavailable"),
				409);
		}

		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Set Niagara Function Node Enabled")));
		System->Modify();
		Graph->Modify();
		if (bModuleMutation)
		{
			// This is the same operation used by the Niagara stack editor. It
			// updates every node in the module group and raises the graph compile
			// notification inside NiagaraEditor.
			FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Node, bEnabled);
		}
		else
		{
			Node->Modify();
			Node->SetEnabledState(
				bEnabled ? ENodeEnabledState::Enabled : ENodeEnabledState::Disabled,
				true);
			Node->MarkNodeRequiresSynchronization(
				TEXT("UE AI Niagara function node enabled state changed"),
				true);
		}
		System->MarkPackageDirty();
		const FCompileSummary CompileSummary = RequestAndSummarizeCompile(System);
		CaptureAfterStates(BeforeStates);
		TArray<FMutationReceipt::FNodeState> AffectedStates = BuildAffectedStates(
			BeforeStates,
			Node);

		const ENodeEnabledState AfterState = Node->GetDesiredEnabledState();
		bool bVerified = IsNodeStateEnabled(Node, bEnabled);
		if (bVerified && bModuleMutation)
		{
			for (const FMutationReceipt::FNodeState& State : AffectedStates)
			{
				if (!IsNodeStateEnabled(State.Node.Get(), bEnabled))
				{
					bVerified = false;
					break;
				}
			}
		}
		if (!bVerified || !CompileSummary.bCompiled)
		{
			const FCompileSummary RestoreSummary = RestoreNodeStates(System, Graph, BeforeStates);
			bool bRestored = RestoreSummary.bCompiled;
			for (const FMutationReceipt::FNodeState& State : BeforeStates)
			{
				const UNiagaraNode* RestoredNode = State.Node.Get();
				bRestored &= RestoredNode
					&& RestoredNode->GetDesiredEnabledState() == State.BeforeState
					&& RestoredNode->HasUserSetTheEnabledState() == State.bBeforeUserSet;
			}
			if (!bRestored)
			{
				return ErrorResult(
					TEXT("Niagara node edit failed and restoration could not be verified; the transaction was retained for Editor Undo."),
					TEXT("restore_verification_failed"), 500);
			}
			Transaction.Cancel();
			const FString FailureReason = !bVerified
				? TEXT("Niagara node read-back did not match the approved enabled state.")
				: FString::Printf(
					TEXT("Niagara compilation did not complete successfully (status=%s)."),
					*CompileSummary.AggregateStatus);
			return ErrorResult(
				FailureReason + TEXT(" The change was restored."),
				!bVerified ? TEXT("verification_failed") : TEXT("compile_failed"),
				500);
		}

		FMutationReceipt Receipt;
		Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Receipt.RequestId = RequestId;
		Receipt.PlanDigest = Plan->GetStringField(TEXT("planDigest"));
		Receipt.SystemPath = System->GetPathName();
		Receipt.GraphPath = Graph->GetPathName();
		Receipt.NodePath = Node->GetPathName();
		Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		Receipt.System = System;
		Receipt.Graph = Graph;
		Receipt.Node = Node;
		Receipt.MutationScope = MutationScope;
		Receipt.AffectedNodes = MoveTemp(AffectedStates);
		Receipt.BeforeState = BeforeState;
		Receipt.bBeforeUserSet = bBeforeUserSet;
		Receipt.AfterState = AfterState;
		Receipt.bCompiled = CompileSummary.bCompiled;
		Receipt.CompileStatus = CompileSummary.AggregateStatus;
		MutationReceipts().Add(Receipt.ReceiptId, Receipt);
		RequestReceiptIds().Add(RequestId, Receipt.ReceiptId);

		return FMCPToolResult::Ok(MakeMutationResult(Receipt, false));
	}
};

class FTool_NiagaraNodeEnabledRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.node.set_enabled.rollback"); }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ReceiptId;
		FString RequestId;
		bool bConfirmWrite = false;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId)
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
			|| ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirmWrite)
		{
			return ErrorResult(
				TEXT("rollbackId, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				422);
		}

		FMutationReceipt* Receipt = MutationReceipts().Find(ReceiptId);
		if (!Receipt)
		{
			return ErrorResult(
				TEXT("The Niagara mutation receipt is unknown in this Editor instance."),
				TEXT("receipt_not_found"),
				404);
		}
		if (Receipt->bRolledBack)
		{
			return FMCPToolResult::Ok(MakeMutationResult(*Receipt, true));
		}
		if (Receipt->RequestId != RequestId)
		{
			return ErrorResult(
				TEXT("requestId does not match the mutation receipt."),
				TEXT("request_id_mismatch"),
				409);
		}

		UNiagaraSystem* System = Receipt->System.Get();
		UNiagaraGraph* Graph = Receipt->Graph.Get();
		UNiagaraNodeFunctionCall* Node = Receipt->Node.Get();
		if (!System || !Graph || !Node)
		{
			return ErrorResult(
				TEXT("The Niagara mutation target is no longer loaded."),
				TEXT("target_unavailable"),
				409);
		}
		const FString CurrentChangeId = Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		bool bRollbackConflict = CurrentChangeId != Receipt->GraphChangeIdAfter;
		bool bRollbackChangesState = false;
		for (const FMutationReceipt::FNodeState& State : Receipt->AffectedNodes)
		{
			const UNiagaraNode* AffectedNode = State.Node.Get();
			bRollbackConflict |= !AffectedNode
				|| AffectedNode->GetDesiredEnabledState() != State.AfterState
				|| AffectedNode->HasUserSetTheEnabledState() != State.bAfterUserSet;
			bRollbackChangesState |= State.BeforeState != State.AfterState
				|| State.bBeforeUserSet != State.bAfterUserSet;
		}
		if (bRollbackConflict)
		{
			return ErrorResult(
				TEXT("The Niagara graph or an affected module node changed after apply; rollback was refused to avoid overwriting newer edits."),
				TEXT("rollback_conflict"),
				409);
		}

		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Rollback Niagara Function Node Enabled")));
		const FCompileSummary CompileSummary = RestoreNodeStates(
			System,
			Graph,
			Receipt->AffectedNodes);
		bool bRollbackVerified = true;
		for (const FMutationReceipt::FNodeState& State : Receipt->AffectedNodes)
		{
			const UNiagaraNode* AffectedNode = State.Node.Get();
			if (!AffectedNode
				|| AffectedNode->GetDesiredEnabledState() != State.BeforeState
				|| AffectedNode->HasUserSetTheEnabledState() != State.bBeforeUserSet)
			{
				bRollbackVerified = false;
				break;
			}
		}
		if (!bRollbackVerified || (bRollbackChangesState && !CompileSummary.bCompiled))
		{
			const FString FailureReason = !bRollbackVerified
				? TEXT("Niagara node rollback read-back failed.")
				: FString::Printf(
					TEXT("Niagara node rollback compilation failed (status=%s)."),
					*CompileSummary.AggregateStatus);
			return ErrorResult(
				FailureReason + TEXT(" The transaction was retained for Editor Undo."),
				TEXT("rollback_verification_failed"),
				500);
		}
		if (bRollbackChangesState)
		{
			Receipt->bCompiled = CompileSummary.bCompiled;
			Receipt->CompileStatus = CompileSummary.AggregateStatus;
		}
		Receipt->bRolledBack = true;
		TSharedRef<FJsonObject> Result = MakeMutationResult(*Receipt, false);
		Result->SetBoolField(TEXT("changed"), bRollbackChangesState);
		Result->SetBoolField(TEXT("rolledBack"), true);
		return FMCPToolResult::Ok(Result);
	}
};

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraRecoveryTargetsTest,
	"UE_AI_integration.Niagara.RecoveryTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAINiagaraRecoveryTargetsTest::RunTest(const FString& Parameters)
{
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage());
	TArray<FCollisionPolicyGraphState> Graphs;
	TArray<FCollisionPolicyNodeState> Nodes;
	TArray<FCollisionPolicyDataInterfaceState> Interfaces;
	FCompileSummary Summary;
	TestTrue(TEXT("Empty recovery has no state to restore"), RestoreCollisionPolicyStates(System, Graphs, Nodes, Interfaces, true, Summary));
	FCollisionPolicyNodeState MissingNode;
	MissingNode.bEditable = true;
	Nodes.Add(MissingNode);
	TestFalse(TEXT("A lost editable node is not reported as restored"), RestoreCollisionPolicyStates(System, Graphs, Nodes, Interfaces, true, Summary));
	Nodes.Reset();
	FCollisionPolicyDataInterfaceState MissingInterface;
	MissingInterface.bEditable = true;
	Interfaces.Add(MissingInterface);
	TestFalse(TEXT("A lost editable DI is not reported as restored"), RestoreCollisionPolicyStates(System, Graphs, Nodes, Interfaces, true, Summary));
	return true;
}
#endif

} // namespace UEAINiagaraGraphPrivate

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphTools(FMCPToolRegistry& Registry)
{
	using namespace UEAINiagaraGraphPrivate;
	Registry.Register(MakeShared<FTool_NiagaraGraphInspect>());
	Registry.Register(MakeShared<FTool_NiagaraCollisionAudit>());
	Registry.Register(MakeShared<FTool_NiagaraCollisionPolicyPlan>());
	Registry.Register(MakeShared<FTool_NiagaraCollisionPolicyApply>());
	Registry.Register(MakeShared<FTool_NiagaraCollisionPolicyRollback>());
	Registry.Register(MakeShared<FTool_NiagaraNodeEnabledPlan>());
	Registry.Register(MakeShared<FTool_NiagaraNodeEnabledApply>());
	Registry.Register(MakeShared<FTool_NiagaraNodeEnabledRollback>());
	Registry.Register(MakeShared<FTool_NiagaraPinDefaultPlan>());
	Registry.Register(MakeShared<FTool_NiagaraPinDefaultApply>());
	Registry.Register(MakeShared<FTool_NiagaraPinDefaultRollback>());
}
}

#else

class FUnavailableNiagaraGraphTool final : public FMCPToolBase
{
public:
	explicit FUnavailableNiagaraGraphTool(FString InCapabilityId)
		: CapabilityId(MoveTemp(InCapabilityId))
	{
	}

	FString GetCapabilityId() const override { return CapabilityId; }

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

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.inspect")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.collision.audit")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.collision.policy.plan")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.collision.policy.apply")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.collision.policy.rollback")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.node.set_enabled.plan")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.node.set_enabled.apply")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.node.set_enabled.rollback")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.pin.set_default.plan")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.pin.set_default.apply")));
	Registry.Register(MakeShared<FUnavailableNiagaraGraphTool>(
		TEXT("content.niagara.graph.pin.set_default.rollback")));
}
}

#endif
