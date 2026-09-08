// Detailed read-only inventory for Niagara collision graph audits.

#include "Niagara_Graph_Audit.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "NiagaraDataInterfaceAsyncGpuTrace.h"
#include "NiagaraDataInterfaceCollisionQuery.h"
#include "NiagaraDataInterfacePhysicsAsset.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScript.h"
#include "NiagaraSettings.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "SceneManagement.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include <initializer_list>

namespace UEAINiagaraGraphAuditExtensions
{
namespace
{
constexpr int32 MaxAuditLimit = 4096;

// RigidMeshCollisionQuery is not exported by Niagara in UE 5.4.1, so a
// direct StaticClass() reference would leave this plugin with an unresolved
// GetPrivateStaticClass symbol. Walk the loaded class hierarchy instead; this
// also keeps derived data-interface classes classified as their provider kind.
bool IsClassNamed(const UObject* Object, const FName& ClassName)
{
	for (const UClass* Class = Object ? Object->GetClass() : nullptr;
		Class;
		Class = Class->GetSuperClass())
	{
		if (Class->GetFName() == ClassName)
		{
			return true;
		}
	}
	return false;
}

struct FFunctionClassification
{
	TArray<FString> Classes;
	TArray<FString> Operations;
	TArray<FString> InterfaceKinds;
	bool bCollisionRelated = false;
	bool bOrdinaryCollisionQuery = false;
	bool bDistanceField = false;
	bool bAsyncGpuTrace = false;
	bool bDepthBufferQuery = false;
	bool bCollisionResponse = false;
	bool bAnalyticalCollision = false;
	bool bCollisionSupport = false;
	bool bRigidMeshCollisionQuery = false;
	bool bPhysicsAsset = false;
};

bool ContainsAnyToken(const FString& Value, std::initializer_list<const TCHAR*> Tokens)
{
	for (const TCHAR* Token : Tokens)
	{
		if (Token && Value.Contains(Token, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

//++[SilverPalace] Begin add by wuziye 2026/09/05
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
//--[SilverPalace] End add by wuziye

//++[SilverPalace] Begin add by wuziye 2026/09/05
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
//--[SilverPalace] End add by wuziye

void AddUniqueString(TArray<FString>& Values, const FString& Value)
{
	if (!Value.IsEmpty() && !Values.Contains(Value))
	{
		Values.Add(Value);
	}
}

void AddOperationInventoryEntry(
	FNiagaraCollisionOperationInventory& Inventory,
	const FString& Operation,
	const FString& Location,
	const TArray<FString>& InterfaceKinds,
	const int32 LocationLimit)
{
	if (Operation.IsEmpty())
	{
		return;
	}
	FNiagaraCollisionOperationInventoryEntry& Entry = Inventory.FindOrAdd(Operation);
	++Entry.Count;
	if (!Location.IsEmpty()
		&& Entry.Locations.Num() < LocationLimit
		&& !Entry.Locations.Contains(Location))
	{
		Entry.Locations.Add(Location);
	}
	for (const FString& InterfaceKind : InterfaceKinds)
	{
		AddUniqueString(Entry.InterfaceKinds, InterfaceKind);
	}
}

void AddBestOperationMatch(
	const FString& SearchText,
	const FString& FunctionName,
	const FString& SignatureName,
	TArray<FString>& OutOperations)
{
	// A function-call node normally carries the exact data-interface signature
	// in Signature.Name. Prefer that (or the display name) so a specific
	// operation such as ...Accurate is not also reported as its shorter base
	// name. The search-text fallback is longest-match-first for older assets
	// that do not retain either name.
	const TCHAR* const OperationTokens[] =
	{
		TEXT("PerformCollisionQuerySyncCPU"),
		TEXT("PerformCollisionQueryAsyncCPU"),
		// RigidMeshCollisionQuery exposes both primitive/physics queries and
		// mesh-distance-field queries. Keep every UE 5.4 signature explicit so
		// inventory results do not collapse to a generic CollisionQuery label.
		TEXT("GetClosestPointMeshDistanceFieldAccurate"),
		TEXT("GetClosestPointMeshDistanceFieldNoNormal"),
		TEXT("GetClosestPointMeshDistanceField"),
		TEXT("GetElementPointMeshDistanceFieldNoNormal"),
		TEXT("GetMaxEncodedDistanceMeshDistanceField"),
		TEXT("GetBoxElementsStartIndex"),
		TEXT("GetSphereElementsStartIndex"),
		TEXT("GetCapsuleElementsStartIndex"),
		TEXT("IsWorldPositionInsideCombinedBounds"),
		TEXT("GetClosestPointSimple"),
		// PhysicsAsset-only signatures. Shared names such as GetClosestPoint
		// are resolved from the owning DI when the graph retains that object.
		TEXT("GetProjectionPoint"),
		TEXT("GetTexturePoint"),
		TEXT("GetRestDistance"),
		TEXT("FindActors"),
		TEXT("GetNumBoxes"),
		TEXT("GetNumSpheres"),
		TEXT("GetNumCapsules"),
		TEXT("GetNumElements"),
		TEXT("GetSphereRadius"),
		TEXT("GetCapsuleSize"),
		TEXT("GetBoxSize"),
		TEXT("GetClosestElement"),
		TEXT("GetElementPoint"),
		TEXT("GetElementDistance"),
		TEXT("GetClosestPoint"),
		TEXT("GetClosestDistance"),
		TEXT("Collision.Collision"),
		TEXT("CollisionQuery.CollisionQuery"),
		TEXT("CollisionQueryAndResponse"),
		TEXT("CollisionRest"),
		TEXT("CollisionLinearImpulse"),
		// Analytical/support operations are inventory-only. They consume or
		// prepare collision state and are intentionally not policy providers.
		TEXT("PlaneSphereCollisionDetection"),
		TEXT("RandomizeCollisionNormals"),
		TEXT("AnalyticalCollisionQuery"),
		TEXT("SetupRigidBodyDI"),
		TEXT("RayTrace"),
		// Collision helper/event modules are inventory-only and are not
		// ordinary world-query providers.
		TEXT("AddRotationalVelocity"),
		TEXT("CalculateLinePlaneInt"),
		TEXT("DebugCollisionEvents"),
		TEXT("FindTangentialVelocityOnSphere"),
		TEXT("InitialRotationalVelocity"),
		//++[SilverPalace] Begin add by wuziye 2026/09/05
		TEXT("CalculateNeighbors"),
		TEXT("SampleNeighbors"),
		TEXT("NeighborBehaviours"),
		TEXT("NeighborBehaviors"),
		//--[SilverPalace] End add by wuziye
		TEXT("InitializeNeighborGrid"),
		TEXT("PBD_IntraParticleCollision"),
		TEXT("PopulateNeighborGrid"),
		TEXT("GenerateCollisionEvent"),
		TEXT("ReceiveCollisionEvent"),
		TEXT("QueryMeshDistanceFieldGPU"),
		TEXT("QueryDistanceField"),
		TEXT("Fn_SphereTraceDistanceField"),
		TEXT("AvoidDistanceFieldSurfaces_GPU"),
		TEXT("MoveToNearestDistanceFieldSurface_GPU"),
		TEXT("FindNearestDistanceFieldSurface_GPU"),
		TEXT("FindNearestDistanceFieldSurface"),
		TEXT("RayTraceDistanceField_GPU"),
		TEXT("ReadDistanceField_GPU"),
		TEXT("NiagaraDistanceFieldCollisions"),
		TEXT("SphereTraceDistanceField_GPU"),
		TEXT("SphereCast_GlobalDistanceField"),
		TEXT("CalculateTheGlobalDistanceFieldSurfaceNormal_GPU"),
		TEXT("CalculateTheGlobalDistanceFieldSurfaceNormal"),
		TEXT("CalculateGlobalDistanceFieldSurfaceNormal"),
		TEXT("CalculateGlobalDistanceFieldIsoSurfaceNormal"),
		TEXT("FindNearestDistanceFieldIsoSurface"),
		TEXT("FindDistanceFieldVolumeTextureGradient"),
		TEXT("CalculateA_VolumeTexturesDistanceFieldGradient"),
		TEXT("AlignParticlesWithCollisionPlane"),
		TEXT("QuerySceneDepthGPU"),
		TEXT("QueryScenePartialDepthGPU"),
		TEXT("QueryCustomDepthGPU"),
		TEXT("SceneDepthTest"),
		TEXT("PlaceParticlesOnDepthBuffer_GPU"),
		TEXT("CreateAsyncRayTrace"),
		TEXT("CreateAsyncRayTraceGpu"),
		TEXT("IssueAsyncRayTrace"),
		TEXT("IssueAsyncRayTraceGpu"),
		TEXT("ReserveAsyncRayTrace"),
		TEXT("ReserveAsyncRayTraceGpu"),
		TEXT("ReserveRayTraceIndex"),
		TEXT("ReadAsyncRayTrace"),
		TEXT("ReadAsyncRayTraceGpu"),
		TEXT("IsAsyncGpuTraceReadyGpu"),
		TEXT("IsHardwareRayTracingEnabledGpu"),
		TEXT("IsHardwareRayTracingAvailableGpu"),
		TEXT("AsyncGpuTrace")
	};

	for (const TCHAR* Token : OperationTokens)
	{
		if (SignatureName.Equals(Token, ESearchCase::IgnoreCase)
			|| FunctionName.Equals(Token, ESearchCase::IgnoreCase))
		{
			AddUniqueString(OutOperations, Token);
			return;
		}
	}

	const TCHAR* BestMatch = nullptr;
	int32 BestMatchLength = 0;
	for (const TCHAR* Token : OperationTokens)
	{
		if (!SearchText.Contains(Token, ESearchCase::IgnoreCase))
		{
			continue;
		}
		const int32 TokenLength = FCString::Strlen(Token);
		if (TokenLength > BestMatchLength)
		{
			BestMatch = Token;
			BestMatchLength = TokenLength;
		}
	}
	if (BestMatch)
	{
		AddUniqueString(OutOperations, BestMatch);
	}
}

FString FunctionSearchText(const UNiagaraNodeFunctionCall* FunctionCall)
{
	if (!FunctionCall)
	{
		return FString();
	}
	const FString FunctionScript = FunctionCall->FunctionScript
		? FunctionCall->FunctionScript->GetPathName()
		: FunctionCall->FunctionScriptAssetObjectPath.ToString();
	return FString::Join(
		TArray<FString>{
			FunctionCall->GetFunctionName(),
			FunctionCall->Signature.Name.ToString(),
			FunctionScript},
		TEXT(" "));
}

bool HasRigidMeshCollisionOperation(const FString& SearchText)
{
	return ContainsAnyToken(
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
}

bool HasPhysicsAssetOperation(const FString& SearchText)
{
	return ContainsAnyToken(
		SearchText,
		{
			TEXT("GetRestDistance"),
			TEXT("GetTexturePoint"),
			TEXT("GetProjectionPoint")
		});
}

void AddCollisionInterfaceKind(
	TArray<FString>& InterfaceKinds,
	const FString& Kind)
{
	AddUniqueString(InterfaceKinds, Kind);
}

void InferCollisionInterfaceKinds(
	const FString& SearchText,
	TArray<FString>& OutInterfaceKinds)
{
	OutInterfaceKinds.Reset();
	const bool bRigidMeshInterface = ContainsAnyToken(
		SearchText,
		{
			TEXT("UNiagaraDataInterfaceRigidMeshCollisionQuery"),
			TEXT("NiagaraDataInterfaceRigidMeshCollisionQuery"),
			TEXT("RigidMeshCollisionQuery")
		});
	const bool bPhysicsAssetInterface = ContainsAnyToken(
		SearchText,
		{
			TEXT("UNiagaraDataInterfacePhysicsAsset"),
			TEXT("NiagaraDataInterfacePhysicsAsset"),
			TEXT("PhysicsAsset")
		});
	if (bRigidMeshInterface || HasRigidMeshCollisionOperation(SearchText))
	{
		AddCollisionInterfaceKind(OutInterfaceKinds, TEXT("rigidMeshCollisionQuery"));
	}
	if (bPhysicsAssetInterface || HasPhysicsAssetOperation(SearchText))
	{
		AddCollisionInterfaceKind(OutInterfaceKinds, TEXT("physicsAsset"));
	}
}

void ClassifyFunctionNode(
	const UNiagaraNodeFunctionCall* FunctionCall,
	FFunctionClassification& OutClassification)
{
	OutClassification = FFunctionClassification();
	if (!FunctionCall)
	{
		return;
	}

	const FString SearchText = FunctionSearchText(FunctionCall);
	const FString FunctionScript = FunctionCall->FunctionScript
		? FunctionCall->FunctionScript->GetPathName()
		: FunctionCall->FunctionScriptAssetObjectPath.ToString();
	const FString FunctionName = FunctionCall->GetFunctionName();
	const FString SignatureName = FunctionCall->Signature.Name.ToString();
	InferCollisionInterfaceKinds(SearchText, OutClassification.InterfaceKinds);
	// Shared signatures are emitted by both RigidMeshCollisionQuery and
	// PhysicsAsset. Only attribute them to a provider when its owner marker is
	// present; otherwise a generic GetClosestPoint must remain unqualified.
	const bool bRigidMeshOwnerMarker = ContainsAnyToken(
		SearchText,
		{
			TEXT("UNiagaraDataInterfaceRigidMeshCollisionQuery"),
			TEXT("NiagaraDataInterfaceRigidMeshCollisionQuery"),
			TEXT("RigidMeshCollisionQuery")
		});
	const bool bPhysicsAssetOwnerMarker = ContainsAnyToken(
		SearchText,
		{
			TEXT("UNiagaraDataInterfacePhysicsAsset"),
			TEXT("NiagaraDataInterfacePhysicsAsset"),
			TEXT("PhysicsAsset")
		});
	const bool bRigidMeshSharedOperation = bRigidMeshOwnerMarker
		&& ContainsAnyToken(
			SearchText,
			{
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
	const bool bPhysicsAssetSharedOperation = bPhysicsAssetOwnerMarker
		&& ContainsAnyToken(
			SearchText,
			{
				TEXT("GetNumBoxes"),
				TEXT("GetNumSpheres"),
				TEXT("GetNumCapsules"),
				TEXT("GetClosestElement"),
				TEXT("GetElementPoint"),
				TEXT("GetElementDistance"),
				TEXT("GetClosestPoint"),
				TEXT("GetClosestDistance")
			});
	if (bRigidMeshSharedOperation)
	{
		AddCollisionInterfaceKind(
			OutClassification.InterfaceKinds,
			TEXT("rigidMeshCollisionQuery"));
	}
	if (bPhysicsAssetSharedOperation)
	{
		AddCollisionInterfaceKind(
			OutClassification.InterfaceKinds,
			TEXT("physicsAsset"));
	}
	OutClassification.bRigidMeshCollisionQuery =
		OutClassification.InterfaceKinds.Contains(TEXT("rigidMeshCollisionQuery"));
	OutClassification.bPhysicsAsset =
		OutClassification.InterfaceKinds.Contains(TEXT("physicsAsset"));
	// Only collision providers and collision-oriented queries belong to the
	// policy's Distance Field class. Do not use a generic DistanceField
	// substring: WindField exposes similarly named, non-collision functions.
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
	const bool bCollisionPathDistanceField =
		SearchText.Contains(TEXT("DistanceField"), ESearchCase::IgnoreCase)
		&& SearchText.Contains(TEXT("Collision"), ESearchCase::IgnoreCase);
	const bool bDistanceField = bNamedDistanceFieldCollision
		|| bExactDistanceFieldHelper
		|| bCollisionPathDistanceField;
	const bool bAsyncGpuTrace = ContainsAnyToken(
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
	const bool bDepthBufferQuery = ContainsAnyToken(
		SearchText,
		{
		TEXT("QuerySceneDepthGPU"),
		TEXT("QueryScenePartialDepthGPU"),
		TEXT("QueryCustomDepthGPU"),
		TEXT("SceneDepthTest"),
		TEXT("PlaceParticlesOnDepthBuffer_GPU")
		});
	// These operations are collision-related, but they only solve analytical
	// planes or prepare/consume collision state. They are not world-query
	// providers and must stay outside the collision policy mutation set.
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
		//++[SilverPalace] Begin add by wuziye 2026/09/05
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
		//--[SilverPalace] End add by wuziye
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
	const bool bExplicitCpuQuery =
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

	OutClassification.bDistanceField = bDistanceField;
	OutClassification.bAsyncGpuTrace = bAsyncGpuTrace;
	OutClassification.bDepthBufferQuery = bDepthBufferQuery;
	OutClassification.bOrdinaryCollisionQuery =
		bExplicitCpuQuery
		&& !bDistanceField
		&& !bAsyncGpuTrace
		&& !bDepthBufferQuery
		&& !OutClassification.bRigidMeshCollisionQuery
		&& !OutClassification.bPhysicsAsset
		&& !bAnalyticalCollision
		&& !bCollisionSupport
		&& !bCollisionResponseOnly;
	OutClassification.bCollisionResponse = bCollisionResponse;
	OutClassification.bAnalyticalCollision = bAnalyticalCollision;
	OutClassification.bCollisionSupport = bCollisionSupport;
	OutClassification.bCollisionRelated =
		OutClassification.bOrdinaryCollisionQuery
		|| bDistanceField
		|| bAsyncGpuTrace
		|| bDepthBufferQuery
		|| bCollisionResponse
		|| bAnalyticalCollision
		|| bCollisionSupport
		|| OutClassification.bRigidMeshCollisionQuery
		|| OutClassification.bPhysicsAsset;

	if (OutClassification.bOrdinaryCollisionQuery)
	{
		AddUniqueString(OutClassification.Classes, TEXT("ordinaryCollisionQuery"));
	}
	if (bDistanceField)
	{
		AddUniqueString(OutClassification.Classes, TEXT("distanceField"));
	}
	if (bAsyncGpuTrace)
	{
		AddUniqueString(OutClassification.Classes, TEXT("asyncGpuTrace"));
	}
	if (bDepthBufferQuery)
	{
		AddUniqueString(OutClassification.Classes, TEXT("depthBufferCollisionQuery"));
	}
	if (bCollisionResponse)
	{
		AddUniqueString(OutClassification.Classes, TEXT("collisionResponse"));
	}
	if (bAnalyticalCollision)
	{
		AddUniqueString(OutClassification.Classes, TEXT("analyticalCollision"));
	}
	if (bCollisionSupport)
	{
		AddUniqueString(OutClassification.Classes, TEXT("collisionSupport"));
	}
	if (OutClassification.bRigidMeshCollisionQuery)
	{
		AddUniqueString(OutClassification.Classes, TEXT("rigidMeshCollisionQuery"));
	}
	if (OutClassification.bPhysicsAsset)
	{
		AddUniqueString(OutClassification.Classes, TEXT("physicsAsset"));
	}

	// Keep one exact operation name in the result so callers can distinguish
	// MoveToNearestDistanceFieldSurface_GPU from a generic DistanceField node.
	AddBestOperationMatch(
		SearchText,
		FunctionCall->GetFunctionName(),
		FunctionCall->Signature.Name.ToString(),
		OutClassification.Operations);
	if (OutClassification.bOrdinaryCollisionQuery
		&& OutClassification.Operations.IsEmpty())
	{
		AddUniqueString(OutClassification.Operations, TEXT("CollisionQuery"));
	}
	if (bDistanceField && OutClassification.Operations.IsEmpty())
	{
		AddUniqueString(OutClassification.Operations, TEXT("DistanceField"));
	}
	if (bAsyncGpuTrace && OutClassification.Operations.IsEmpty())
	{
		AddUniqueString(OutClassification.Operations, TEXT("AsyncGpuTrace"));
	}
}

bool IsNestedUnder(const UObject* Object, const UObject* Ancestor)
{
	if (!Object || !Ancestor)
	{
		return false;
	}
	for (const UObject* Cursor = Object; Cursor; Cursor = Cursor->GetOuter())
	{
		if (Cursor == Ancestor)
		{
			return true;
		}
	}
	return false;
}

const UNiagaraNode* FindOwningNode(
	const UObject* Object,
	const UNiagaraGraph* Graph)
{
	if (!Object || !Graph)
	{
		return nullptr;
	}
	for (const UObject* Cursor = Object->GetOuter();
		Cursor && Cursor != Graph;
		Cursor = Cursor->GetOuter())
	{
		if (const UNiagaraNode* Node = Cast<UNiagaraNode>(Cursor))
		{
			return Node;
		}
	}
	return nullptr;
}

const UNiagaraNodeFunctionCall* AsFunctionCall(const UNiagaraNode* Node)
{
	return Cast<UNiagaraNodeFunctionCall>(Node);
}

const UNiagaraScript* FindOwningScript(const UObject* Object)
{
	return Object ? Object->GetTypedOuter<UNiagaraScript>() : nullptr;
}

FString EnabledStateName(const UNiagaraNode* Node)
{
	if (!Node)
	{
		return TEXT("unknown");
	}
	switch (Node->GetDesiredEnabledState())
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

FString ProviderName(const ENDICollisionQuery_AsyncGpuTraceProvider::Type Provider)
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

void SetPathArray(
	const TSharedRef<FJsonObject>& Object,
	const TCHAR* Field,
	const TArray<FString>& Values)
{
	SetStringArray(Object, Field, Values);
}

bool IsPackageEditable(const UNiagaraGraph* Graph)
{
	return Graph
		&& Graph->GetOutermost()
		&& Graph->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));
}

struct FGraphAuditContext
{
	int32 ReferenceDepth = 0;
	bool bEditable = false;
};

FGraphAuditContext ReadGraphAuditContext(
	const UNiagaraGraph* Graph,
	const TSharedRef<FJsonObject>& GraphObject)
{
	FGraphAuditContext Context;
	Context.bEditable = IsPackageEditable(Graph);
	if (GraphObject->HasField(TEXT("editable")))
	{
		GraphObject->TryGetBoolField(TEXT("editable"), Context.bEditable);
	}
	double ReferenceDepth = 0.0;
	if (GraphObject->TryGetNumberField(TEXT("referenceDepth"), ReferenceDepth))
	{
		Context.ReferenceDepth = FMath::Max(0, FMath::TruncToInt(ReferenceDepth));
	}
	return Context;
}

bool WouldChangeUnderHardwareRayTracing(
	const UNiagaraNodeFunctionCall* FunctionCall,
	const FFunctionClassification& Classification,
	const bool bEditable)
{
	if (!FunctionCall || !bEditable)
	{
		return false;
	}
	if (Classification.bDistanceField)
	{
		return FunctionCall->GetDesiredEnabledState() != ENodeEnabledState::Disabled
			|| !FunctionCall->HasUserSetTheEnabledState();
	}
	if (Classification.bAsyncGpuTrace)
	{
		return FunctionCall->GetDesiredEnabledState() != ENodeEnabledState::Enabled
			|| !FunctionCall->HasUserSetTheEnabledState();
	}
	if (Classification.bOrdinaryCollisionQuery)
	{
		// The hardware-ray-tracing policy disables the legacy CPU collision path
		// so an AsyncGpuTrace HWRT module is the active provider.
		return FunctionCall->GetDesiredEnabledState() != ENodeEnabledState::Disabled
			|| !FunctionCall->HasUserSetTheEnabledState();
	}
	return false;
}

bool IsProviderAvailable(const ENDICollisionQuery_AsyncGpuTraceProvider::Type Provider)
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

bool IsDefaultProviderFallbackRisk()
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
	return DistanceIndex != INDEX_NONE
		&& (HardwareIndex == INDEX_NONE
			|| DistanceIndex < HardwareIndex
			|| !IsProviderAvailable(ENDICollisionQuery_AsyncGpuTraceProvider::HWRT));
}

FString ResolveDefaultProvider()
{
	const UNiagaraSettings* Settings = GetDefault<UNiagaraSettings>();
	if (!Settings)
	{
		return TEXT("Unknown");
	}
	for (const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type> Provider
		: Settings->NDICollisionQuery_AsyncGpuTraceProviderOrder)
	{
		if (IsProviderAvailable(Provider))
		{
			return ProviderName(Provider);
		}
	}
	return TEXT("Unavailable");
}

void SetProviderState(
	const TSharedRef<FJsonObject>& Object,
	const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface)
{
	if (!DataInterface)
	{
		return;
	}
	const FString Provider = ProviderName(DataInterface->TraceProvider);
	Object->SetStringField(TEXT("traceProvider"), Provider);
	Object->SetStringField(TEXT("provider"), Provider);
	Object->SetNumberField(TEXT("maxTracesPerParticle"), DataInterface->MaxTracesPerParticle);
	Object->SetNumberField(TEXT("maxRetraces"), DataInterface->MaxRetraces);
	Object->SetBoolField(
		TEXT("defaultProviderFallbackRisk"),
		DataInterface->TraceProvider == ENDICollisionQuery_AsyncGpuTraceProvider::Default
			&& IsDefaultProviderFallbackRisk());

	TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
	Availability->SetBoolField(
		TEXT("HWRT"),
		IsProviderAvailable(ENDICollisionQuery_AsyncGpuTraceProvider::HWRT));
	Availability->SetBoolField(
		TEXT("GSDF"),
		IsProviderAvailable(ENDICollisionQuery_AsyncGpuTraceProvider::GSDF));
	Availability->SetBoolField(TEXT("None"), true);
	Object->SetObjectField(TEXT("providerAvailability"), Availability);
	Object->SetStringField(TEXT("resolvedDefaultProvider"), ResolveDefaultProvider());

	const UNiagaraSettings* Settings = GetDefault<UNiagaraSettings>();
	TArray<FString> ProviderOrder;
	if (Settings)
	{
		for (const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type> ProviderEntry
			: Settings->NDICollisionQuery_AsyncGpuTraceProviderOrder)
		{
			ProviderOrder.Add(ProviderName(ProviderEntry));
		}
	}
	SetStringArray(Object, TEXT("providerOrder"), ProviderOrder);
}

void AddConsumerNodes(
	const UNiagaraNode* OwnerNode,
	TSet<const UNiagaraNodeFunctionCall*>& OutConsumers)
{
	if (!OwnerNode)
	{
		return;
	}
	if (const UNiagaraNodeFunctionCall* FunctionCall = AsFunctionCall(OwnerNode))
	{
		OutConsumers.Add(FunctionCall);
	}
	for (UEdGraphPin* Pin : OwnerNode->Pins)
	{
		if (!Pin || Pin->Direction != EGPD_Output)
		{
			continue;
		}
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			const UNiagaraNodeFunctionCall* Consumer = LinkedPin
				? Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNodeUnchecked())
				: nullptr;
			if (Consumer)
			{
				OutConsumers.Add(Consumer);
			}
		}
	}
}

UNiagaraDataInterface* FindInputDataInterface(UNiagaraNodeInput* InputNode)
{
	if (!InputNode)
	{
		return nullptr;
	}
	// GetDataInterface is not exported by the UE 5.4 NiagaraEditor module in
	// some target configurations. Reflection keeps the audit read-only and
	// avoids introducing a link dependency on that non-exported accessor.
	FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(
		InputNode->GetClass(),
		TEXT("DataInterface"));
	return DataInterfaceProperty
		? Cast<UNiagaraDataInterface>(
			DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode))
		: nullptr;
}

TSharedRef<FJsonObject> SerializeFunctionNode(
	const UNiagaraNodeFunctionCall* FunctionCall,
	const UNiagaraGraph* Graph,
	const TSet<const UNiagaraNodeFunctionCall*>& DIConsumers,
	const FGraphAuditContext& Context)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	FFunctionClassification Classification;
	ClassifyFunctionNode(FunctionCall, Classification);
	const FString SearchText = FunctionSearchText(FunctionCall);
	Object->SetStringField(TEXT("graphPath"), Graph ? Graph->GetPathName() : FString());
	Object->SetStringField(TEXT("nodePath"), FunctionCall ? FunctionCall->GetPathName() : FString());
	Object->SetStringField(TEXT("path"), FunctionCall ? FunctionCall->GetPathName() : FString());
	Object->SetNumberField(TEXT("referenceDepth"), Context.ReferenceDepth);
	if (FunctionCall)
	{
		Object->SetStringField(
			TEXT("nodeGuid"),
			FunctionCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		Object->SetStringField(TEXT("functionName"), FunctionCall->GetFunctionName());
		Object->SetStringField(TEXT("signatureName"), FunctionCall->Signature.Name.ToString());
		Object->SetStringField(TEXT("signatureOwner"), FunctionCall->Signature.OwnerName.ToString());
		const FString FunctionScript = FunctionCall->FunctionScript
			? FunctionCall->FunctionScript->GetPathName()
			: FunctionCall->FunctionScriptAssetObjectPath.ToString();
		Object->SetStringField(TEXT("functionScript"), FunctionScript);
		Object->SetBoolField(TEXT("enabled"), FunctionCall->IsNodeEnabled());
		Object->SetStringField(TEXT("desiredEnabledState"), EnabledStateName(FunctionCall));
		Object->SetBoolField(TEXT("userSetEnabledState"), FunctionCall->HasUserSetTheEnabledState());
	}
	Object->SetStringField(TEXT("searchText"), SearchText);
	SetStringArray(Object, TEXT("classes"), Classification.Classes);
	SetStringArray(Object, TEXT("operations"), Classification.Operations);
	SetStringArray(Object, TEXT("operationInterfaceKinds"), Classification.InterfaceKinds);
	Object->SetBoolField(TEXT("ordinaryCollisionQuery"), Classification.bOrdinaryCollisionQuery);
	Object->SetBoolField(TEXT("distanceField"), Classification.bDistanceField);
	Object->SetBoolField(TEXT("asyncGpuTrace"), Classification.bAsyncGpuTrace);
	Object->SetBoolField(TEXT("rigidMeshCollisionQuery"), Classification.bRigidMeshCollisionQuery);
	Object->SetBoolField(TEXT("physicsAsset"), Classification.bPhysicsAsset);
	Object->SetBoolField(TEXT("depthBufferCollisionQuery"), Classification.bDepthBufferQuery);
	Object->SetBoolField(TEXT("collisionResponse"), Classification.bCollisionResponse);
	Object->SetBoolField(TEXT("analyticalCollision"), Classification.bAnalyticalCollision);
	Object->SetBoolField(TEXT("collisionSupport"), Classification.bCollisionSupport);
	Object->SetBoolField(TEXT("hasCollisionDataInterface"), DIConsumers.Contains(FunctionCall));
	Object->SetBoolField(TEXT("editable"), Context.bEditable);
	Object->SetStringField(TEXT("changePolicy"), TEXT("hardwareRayTracing"));
	Object->SetBoolField(
		TEXT("wouldChange"),
		WouldChangeUnderHardwareRayTracing(FunctionCall, Classification, Context.bEditable));
	return Object;
}

TSharedRef<FJsonObject> SerializeCollisionDataInterface(
	const UNiagaraDataInterface* DataInterface,
	const UNiagaraGraph* Graph,
	const UNiagaraNode* OwnerNode,
	const TSet<const UNiagaraNodeFunctionCall*>& Consumers,
	const FGraphAuditContext& Context,
	const bool bGraphOwned)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("graphPath"), Graph ? Graph->GetPathName() : FString());
	Object->SetNumberField(TEXT("referenceDepth"), Context.ReferenceDepth);
	const UNiagaraScript* OwningScript = FindOwningScript(DataInterface);
	Object->SetStringField(
		TEXT("scriptPath"),
		OwningScript ? OwningScript->GetPathName() : FString());
	Object->SetStringField(
		TEXT("dataInterfaceScope"),
		bGraphOwned ? TEXT("graph") : TEXT("script"));
	Object->SetStringField(
		TEXT("path"),
		DataInterface ? DataInterface->GetPathName() : FString());
	Object->SetStringField(
		TEXT("dataInterfacePath"),
		DataInterface ? DataInterface->GetPathName() : FString());
	Object->SetStringField(
		TEXT("nodePath"),
		OwnerNode ? OwnerNode->GetPathName() : FString());
	Object->SetStringField(
		TEXT("dataInterfaceClass"),
		DataInterface && DataInterface->GetClass()
			? DataInterface->GetClass()->GetPathName()
			: FString());
	Object->SetStringField(
		TEXT("ownerNodePath"),
		OwnerNode ? OwnerNode->GetPathName() : FString());
	Object->SetStringField(
		TEXT("ownerNodeClass"),
		OwnerNode && OwnerNode->GetClass()
			? OwnerNode->GetClass()->GetPathName()
			: FString());
	Object->SetStringField(TEXT("inputNodePath"), FString());
	Object->SetStringField(TEXT("inputName"), FString());
	const bool bDirectlyEditable = Context.bEditable && bGraphOwned && OwnerNode != nullptr;
	Object->SetBoolField(TEXT("editable"), bDirectlyEditable);
	Object->SetBoolField(TEXT("readOnly"), !bDirectlyEditable);
	Object->SetStringField(TEXT("changePolicy"), TEXT("hardwareRayTracing"));
	Object->SetBoolField(TEXT("wouldChange"), false);
	const FString DataInterfaceKind = GetCollisionDataInterfaceKind(DataInterface);
	Object->SetStringField(TEXT("kind"), DataInterfaceKind);
	Object->SetStringField(TEXT("dataInterfaceKind"), DataInterfaceKind);
	Object->SetBoolField(
		TEXT("rigidMeshCollisionQueryDataInterface"),
		DataInterfaceKind == TEXT("rigidMeshCollisionQuery"));
	Object->SetBoolField(
		TEXT("physicsAssetDataInterface"),
		DataInterfaceKind == TEXT("physicsAsset"));

	if (const UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(OwnerNode))
	{
		Object->SetStringField(TEXT("ownerKind"), TEXT("inputNode"));
		Object->SetStringField(TEXT("inputNodePath"), InputNode->GetPathName());
		Object->SetStringField(TEXT("inputName"), InputNode->Input.GetName().ToString());
	}
	else if (Cast<UNiagaraNodeFunctionCall>(OwnerNode))
	{
		Object->SetStringField(TEXT("ownerKind"), TEXT("functionCall"));
	}
	else
	{
		Object->SetStringField(TEXT("ownerKind"), TEXT("script"));
	}

	TArray<FString> ConsumerPaths;
	for (const UNiagaraNodeFunctionCall* Consumer : Consumers)
	{
		if (Consumer)
		{
			ConsumerPaths.Add(Consumer->GetPathName());
		}
	}
	ConsumerPaths.Sort();
	SetPathArray(Object, TEXT("consumerNodePaths"), ConsumerPaths);
	Object->SetNumberField(TEXT("consumerNodeCount"), ConsumerPaths.Num());
	bool bUsedByDistanceField = false;
	bool bUsedByOrdinaryCollisionQuery = false;
	bool bUsedByAsyncGpuTrace = false;
	bool bUsedByRigidMeshCollisionQuery = false;
	bool bUsedByPhysicsAsset = false;
	for (const UNiagaraNodeFunctionCall* Consumer : Consumers)
	{
		FFunctionClassification ConsumerClassification;
		ClassifyFunctionNode(Consumer, ConsumerClassification);
		bUsedByDistanceField |= ConsumerClassification.bDistanceField;
		bUsedByOrdinaryCollisionQuery |= ConsumerClassification.bOrdinaryCollisionQuery;
		bUsedByAsyncGpuTrace |= ConsumerClassification.bAsyncGpuTrace;
		bUsedByRigidMeshCollisionQuery |= ConsumerClassification.bRigidMeshCollisionQuery;
		bUsedByPhysicsAsset |= ConsumerClassification.bPhysicsAsset;
	}
	Object->SetBoolField(TEXT("usedByDistanceField"), bUsedByDistanceField);
	Object->SetBoolField(TEXT("usedByOrdinaryCollisionQuery"), bUsedByOrdinaryCollisionQuery);
	Object->SetBoolField(TEXT("usedByAsyncGpuTrace"), bUsedByAsyncGpuTrace);
	Object->SetBoolField(TEXT("usedByRigidMeshCollisionQuery"), bUsedByRigidMeshCollisionQuery);
	Object->SetBoolField(TEXT("usedByPhysicsAsset"), bUsedByPhysicsAsset);

	if (const UNiagaraDataInterfaceAsyncGpuTrace* AsyncTrace =
		Cast<UNiagaraDataInterfaceAsyncGpuTrace>(DataInterface))
	{
		Object->SetBoolField(TEXT("asyncGpuTraceDataInterface"), true);
		SetProviderState(Object, AsyncTrace);
		Object->SetBoolField(
			TEXT("wouldChange"),
			bDirectlyEditable
				&& AsyncTrace->TraceProvider != ENDICollisionQuery_AsyncGpuTraceProvider::HWRT);
		Object->SetBoolField(TEXT("requiresGlobalDistanceField"), AsyncTrace->RequiresGlobalDistanceField());
		Object->SetBoolField(TEXT("requiresRayTracingScene"), AsyncTrace->RequiresRayTracingScene());
	}
	else if (const UNiagaraDataInterfaceCollisionQuery* CollisionQuery =
		Cast<UNiagaraDataInterfaceCollisionQuery>(DataInterface))
	{
		Object->SetBoolField(TEXT("collisionQueryDataInterface"), true);
		Object->SetBoolField(TEXT("requiresGlobalDistanceField"), CollisionQuery->RequiresGlobalDistanceField());
		Object->SetBoolField(TEXT("requiresDepthBuffer"), CollisionQuery->RequiresDepthBuffer());
	}
	else if (DataInterfaceKind == TEXT("rigidMeshCollisionQuery"))
	{
		Object->SetBoolField(TEXT("requiresGlobalDistanceField"), bUsedByDistanceField);
	}
	else if (DataInterfaceKind == TEXT("physicsAsset"))
	{
		Object->SetBoolField(TEXT("requiresPhysicsAsset"), true);
	}
	return Object;
}

} // namespace

FString GetCollisionDataInterfaceKind(const UNiagaraDataInterface* DataInterface)
{
	if (!DataInterface)
	{
		return TEXT("unknown");
	}
	if (DataInterface->IsA(UNiagaraDataInterfaceAsyncGpuTrace::StaticClass()))
	{
		return TEXT("asyncGpuTrace");
	}
	if (DataInterface->IsA(UNiagaraDataInterfaceCollisionQuery::StaticClass()))
	{
		return TEXT("collisionQuery");
	}
	if (IsClassNamed(
		DataInterface,
		FName(TEXT("NiagaraDataInterfaceRigidMeshCollisionQuery"))))
	{
		return TEXT("rigidMeshCollisionQuery");
	}
	if (DataInterface->IsA(UNiagaraDataInterfacePhysicsAsset::StaticClass()))
	{
		return TEXT("physicsAsset");
	}
	return TEXT("unknown");
}

bool IsSupportedCollisionDataInterface(const UNiagaraDataInterface* DataInterface)
{
	const FString Kind = GetCollisionDataInterfaceKind(DataInterface);
	return Kind == TEXT("asyncGpuTrace")
		|| Kind == TEXT("collisionQuery")
		|| Kind == TEXT("rigidMeshCollisionQuery")
		|| Kind == TEXT("physicsAsset");
}

void AppendDetailedCollisionAudit(
	UNiagaraGraph* Graph,
	const bool bIncludeDisabled,
	const int32 Limit,
	const TSharedRef<FJsonObject>& GraphObject,
	FNiagaraCollisionOperationInventory* AggregateInventory,
	const FString& OperationSelector)
{
	if (!Graph)
	{
		return;
	}
	const int32 SafeLimit = FMath::Clamp(Limit, 1, MaxAuditLimit);
	const FString NormalizedOperationSelector = OperationSelector.TrimStartAndEnd();
	const FGraphAuditContext Context = ReadGraphAuditContext(Graph, GraphObject);
	FNiagaraCollisionOperationInventory GraphOperationInventory;

	TArray<UNiagaraDataInterface*> DataInterfaces;
	TSet<const UObject*> SeenDataInterfaces;
	const UNiagaraScript* GraphScript = FindOwningScript(Graph);
	auto TryAddDataInterface =
		[&](UNiagaraDataInterface* DataInterface)
	{
		const bool bGraphOwned = IsNestedUnder(DataInterface, Graph);
		const bool bScriptOwned = GraphScript != nullptr
			&& FindOwningScript(DataInterface) == GraphScript;
		if (!DataInterface
			|| SeenDataInterfaces.Contains(DataInterface)
			|| (!bGraphOwned && !bScriptOwned)
			|| !IsSupportedCollisionDataInterface(DataInterface))
		{
			return;
		}
		const UNiagaraNode* OwnerNode = FindOwningNode(DataInterface, Graph);
		if (OwnerNode && !bIncludeDisabled && !OwnerNode->IsNodeEnabled())
		{
			return;
		}
		SeenDataInterfaces.Add(DataInterface);
		DataInterfaces.Add(DataInterface);
	};
	// First inspect graph input nodes directly. This also covers transient DI
	// objects that are referenced by a loaded graph but are not discoverable by
	// a package-level object iterator on an older editor build.
	for (UEdGraphNode* RawNode : Graph->Nodes)
	{
		TryAddDataInterface(FindInputDataInterface(Cast<UNiagaraNodeInput>(RawNode)));
	}
	for (TObjectIterator<UNiagaraDataInterface> It; It; ++It)
	{
		TryAddDataInterface(*It);
	}
	DataInterfaces.Sort(
		[](const UNiagaraDataInterface& Left, const UNiagaraDataInterface& Right)
		{
			return Left.GetPathName() < Right.GetPathName();
		});

	TSet<const UNiagaraNodeFunctionCall*> DIConsumers;
	TMap<const UNiagaraNodeFunctionCall*, TArray<FString>> ConsumerInterfaceKinds;
	for (UNiagaraDataInterface* DataInterface : DataInterfaces)
	{
		if (const UNiagaraNode* OwnerNode = FindOwningNode(DataInterface, Graph))
		{
			TSet<const UNiagaraNodeFunctionCall*> InterfaceConsumers;
			AddConsumerNodes(OwnerNode, InterfaceConsumers);
			const FString InterfaceKind = GetCollisionDataInterfaceKind(DataInterface);
			for (const UNiagaraNodeFunctionCall* Consumer : InterfaceConsumers)
			{
				if (!NormalizedOperationSelector.IsEmpty()
					&& !MatchCollisionOperationSelector(
						Consumer,
						NormalizedOperationSelector))
				{
					continue;
				}
				DIConsumers.Add(Consumer);
				if (!InterfaceKind.IsEmpty() && InterfaceKind != TEXT("unknown"))
				{
					AddUniqueString(ConsumerInterfaceKinds.FindOrAdd(Consumer), InterfaceKind);
				}
			}
		}
	}

	TArray<const UNiagaraNodeFunctionCall*> CollisionNodes;
	for (UEdGraphNode* RawNode : Graph->Nodes)
	{
		const UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(RawNode);
		if (!FunctionCall)
		{
			continue;
		}
		FFunctionClassification Classification;
		ClassifyFunctionNode(FunctionCall, Classification);
		if (!NormalizedOperationSelector.IsEmpty()
			&& !MatchCollisionOperationSelector(
				FunctionCall,
				NormalizedOperationSelector))
		{
			continue;
		}
		if ((!bIncludeDisabled && !FunctionCall->IsNodeEnabled())
			|| (!Classification.bCollisionRelated && !DIConsumers.Contains(FunctionCall)))
		{
			continue;
		}
		CollisionNodes.Add(FunctionCall);
	}
	CollisionNodes.Sort(
		[](const UNiagaraNodeFunctionCall& Left, const UNiagaraNodeFunctionCall& Right)
		{
			return Left.GetPathName() < Right.GetPathName();
		});

	TArray<TSharedPtr<FJsonValue>> FunctionValues;
	FunctionValues.Reserve(FMath::Min(CollisionNodes.Num(), SafeLimit));
	int32 OrdinaryNodeCount = 0;
	int32 DistanceFieldNodeCount = 0;
	int32 AsyncNodeCount = 0;
	int32 DepthNodeCount = 0;
	int32 CollisionResponseNodeCount = 0;
	int32 AnalyticalCollisionNodeCount = 0;
	int32 CollisionSupportNodeCount = 0;
	int32 RigidMeshNodeCount = 0;
	int32 PhysicsAssetNodeCount = 0;
	for (const UNiagaraNodeFunctionCall* FunctionCall : CollisionNodes)
	{
		FFunctionClassification Classification;
		ClassifyFunctionNode(FunctionCall, Classification);
		FString MatchedOperation;
		if (!NormalizedOperationSelector.IsEmpty())
		{
			// The node list was filtered above; retain the canonical spelling so
			// aliases such as CollisionQuery map to the inventory key actually
			// emitted for this graph.
			MatchCollisionOperationSelector(
				FunctionCall,
				NormalizedOperationSelector,
				&MatchedOperation);
		}
		TArray<FString> InterfaceKinds = Classification.InterfaceKinds;
		if (const TArray<FString>* ConsumerKinds = ConsumerInterfaceKinds.Find(FunctionCall))
		{
			for (const FString& Kind : *ConsumerKinds)
			{
				AddUniqueString(InterfaceKinds, Kind);
			}
		}
		OrdinaryNodeCount += Classification.bOrdinaryCollisionQuery ? 1 : 0;
		DistanceFieldNodeCount += Classification.bDistanceField ? 1 : 0;
		AsyncNodeCount += Classification.bAsyncGpuTrace ? 1 : 0;
		DepthNodeCount += Classification.bDepthBufferQuery ? 1 : 0;
		CollisionResponseNodeCount += Classification.bCollisionResponse ? 1 : 0;
		AnalyticalCollisionNodeCount += Classification.bAnalyticalCollision ? 1 : 0;
		CollisionSupportNodeCount += Classification.bCollisionSupport ? 1 : 0;
		RigidMeshNodeCount += InterfaceKinds.Contains(TEXT("rigidMeshCollisionQuery")) ? 1 : 0;
		PhysicsAssetNodeCount += InterfaceKinds.Contains(TEXT("physicsAsset")) ? 1 : 0;
		for (const FString& Operation : Classification.Operations)
		{
			if (!NormalizedOperationSelector.IsEmpty()
				&& !Operation.Equals(MatchedOperation, ESearchCase::IgnoreCase))
			{
				continue;
			}
			AddOperationInventoryEntry(
				GraphOperationInventory,
				Operation,
				FunctionCall ? FunctionCall->GetPathName() : FString(),
				InterfaceKinds,
				SafeLimit);
			if (AggregateInventory)
			{
				AddOperationInventoryEntry(
					*AggregateInventory,
					Operation,
					FunctionCall ? FunctionCall->GetPathName() : FString(),
					InterfaceKinds,
					SafeLimit);
			}
		}
		if (FunctionValues.Num() < SafeLimit)
		{
			TSharedRef<FJsonObject> FunctionObject =
				SerializeFunctionNode(FunctionCall, Graph, DIConsumers, Context);
			SetStringArray(FunctionObject, TEXT("operationInterfaceKinds"), InterfaceKinds);
			if (!NormalizedOperationSelector.IsEmpty())
			{
				FunctionObject->SetStringField(TEXT("matchedOperation"), MatchedOperation);
			}
			FunctionValues.Add(MakeShared<FJsonValueObject>(FunctionObject));
		}
	}

	TArray<TSharedPtr<FJsonValue>> DataInterfaceValues;
	DataInterfaceValues.Reserve(FMath::Min(DataInterfaces.Num(), SafeLimit));
	int32 CollisionQueryDataInterfaceCount = 0;
	int32 AsyncDataInterfaceCount = 0;
	int32 RigidMeshDataInterfaceCount = 0;
	int32 PhysicsAssetDataInterfaceCount = 0;
	int32 GraphOwnedDataInterfaceCount = 0;
	int32 ScriptOwnedDataInterfaceCount = 0;
	for (UNiagaraDataInterface* DataInterface : DataInterfaces)
	{
		const UNiagaraNode* OwnerNode = FindOwningNode(DataInterface, Graph);
		TSet<const UNiagaraNodeFunctionCall*> AllConsumers;
		AddConsumerNodes(OwnerNode, AllConsumers);
		if (!NormalizedOperationSelector.IsEmpty())
		{
			bool bHasSelectedConsumer = false;
			for (const UNiagaraNodeFunctionCall* Consumer : AllConsumers)
			{
				if (DIConsumers.Contains(Consumer))
				{
					bHasSelectedConsumer = true;
					break;
				}
			}
			if (!bHasSelectedConsumer)
			{
				continue;
			}
		}
		const bool bGraphOwned = IsNestedUnder(DataInterface, Graph);
		if (bGraphOwned)
		{
			++GraphOwnedDataInterfaceCount;
		}
		else
		{
			++ScriptOwnedDataInterfaceCount;
		}
		TSet<const UNiagaraNodeFunctionCall*> Consumers;
		for (const UNiagaraNodeFunctionCall* Consumer : AllConsumers)
		{
			if (NormalizedOperationSelector.IsEmpty()
				|| DIConsumers.Contains(Consumer))
			{
				Consumers.Add(Consumer);
			}
		}
		if (DataInterface->IsA(UNiagaraDataInterfaceAsyncGpuTrace::StaticClass()))
		{
			++AsyncDataInterfaceCount;
		}
		if (DataInterface->IsA(UNiagaraDataInterfaceCollisionQuery::StaticClass()))
		{
			++CollisionQueryDataInterfaceCount;
		}
		if (IsClassNamed(
			DataInterface,
			FName(TEXT("NiagaraDataInterfaceRigidMeshCollisionQuery"))))
		{
			++RigidMeshDataInterfaceCount;
		}
		if (DataInterface->IsA(UNiagaraDataInterfacePhysicsAsset::StaticClass()))
		{
			++PhysicsAssetDataInterfaceCount;
		}
		if (DataInterfaceValues.Num() < SafeLimit)
		{
			const bool bDataInterfaceGraphOwned = IsNestedUnder(DataInterface, Graph);
			DataInterfaceValues.Add(MakeShared<FJsonValueObject>(
				SerializeCollisionDataInterface(
					DataInterface,
					Graph,
					OwnerNode,
					Consumers,
					Context,
					bDataInterfaceGraphOwned)));
		}
	}

	GraphObject->SetStringField(TEXT("detailedAuditSchema"), TEXT("ue.niagara-collision-audit-detail.v1"));
	GraphObject->SetStringField(TEXT("detailedChangePolicy"), TEXT("hardwareRayTracing"));
	GraphObject->SetStringField(TEXT("operationSelector"), NormalizedOperationSelector);
	GraphObject->SetBoolField(
		TEXT("operationSelectorMatched"),
		NormalizedOperationSelector.IsEmpty() || GraphOperationInventory.Num() > 0);
	TArray<FString> GraphOperationNames;
	GraphOperationInventory.GetKeys(GraphOperationNames);
	GraphOperationNames.Sort();
	GraphObject->SetStringField(
		TEXT("operationSelectorCanonical"),
		GraphOperationNames.Num() == 1 ? GraphOperationNames[0] : FString());
	GraphObject->SetNumberField(TEXT("collisionFunctionNodeCount"), CollisionNodes.Num());
	GraphObject->SetNumberField(TEXT("collisionFunctionNodeReturned"), FunctionValues.Num());
	GraphObject->SetBoolField(TEXT("collisionFunctionNodesTruncated"), FunctionValues.Num() < CollisionNodes.Num());
	GraphObject->SetNumberField(TEXT("ordinaryCollisionQueryNodeCount"), OrdinaryNodeCount);
	GraphObject->SetNumberField(TEXT("distanceFieldNodeCount"), DistanceFieldNodeCount);
	GraphObject->SetNumberField(TEXT("asyncGpuTraceNodeCount"), AsyncNodeCount);
	GraphObject->SetNumberField(TEXT("depthBufferCollisionQueryNodeCount"), DepthNodeCount);
	GraphObject->SetNumberField(TEXT("collisionResponseNodeCount"), CollisionResponseNodeCount);
	GraphObject->SetNumberField(TEXT("analyticalCollisionNodeCount"), AnalyticalCollisionNodeCount);
	GraphObject->SetNumberField(TEXT("collisionSupportNodeCount"), CollisionSupportNodeCount);
	GraphObject->SetNumberField(TEXT("rigidMeshCollisionQueryNodeCount"), RigidMeshNodeCount);
	GraphObject->SetNumberField(TEXT("physicsAssetNodeCount"), PhysicsAssetNodeCount);
	GraphObject->SetNumberField(TEXT("collisionDataInterfaceCount"), DataInterfaces.Num());
	GraphObject->SetNumberField(TEXT("collisionDataInterfaceReturned"), DataInterfaceValues.Num());
	GraphObject->SetBoolField(TEXT("collisionDataInterfacesTruncated"), DataInterfaceValues.Num() < DataInterfaces.Num());
	GraphObject->SetNumberField(TEXT("collisionQueryDataInterfaceCount"), CollisionQueryDataInterfaceCount);
	GraphObject->SetNumberField(TEXT("asyncGpuTraceDataInterfaceCount"), AsyncDataInterfaceCount);
	GraphObject->SetNumberField(TEXT("rigidMeshCollisionQueryDataInterfaceCount"), RigidMeshDataInterfaceCount);
	GraphObject->SetNumberField(TEXT("physicsAssetDataInterfaceCount"), PhysicsAssetDataInterfaceCount);
	GraphObject->SetNumberField(TEXT("graphOwnedDataInterfaceCount"), GraphOwnedDataInterfaceCount);
	GraphObject->SetNumberField(TEXT("scriptOwnedDataInterfaceCount"), ScriptOwnedDataInterfaceCount);
	GraphObject->SetArrayField(TEXT("collisionFunctionNodes"), FunctionValues);
	GraphObject->SetArrayField(TEXT("collisionDataInterfaces"), DataInterfaceValues);
	SetCollisionOperationInventory(GraphObject, GraphOperationInventory, SafeLimit);
}

void GetCollisionOperationNames(
	const UNiagaraNodeFunctionCall* FunctionCall,
	TArray<FString>& OutOperations)
{
	TArray<FString> InterfaceKinds;
	GetCollisionOperationNames(FunctionCall, OutOperations, InterfaceKinds);
}

void GetCollisionOperationNames(
	const UNiagaraNodeFunctionCall* FunctionCall,
	TArray<FString>& OutOperations,
	TArray<FString>& OutInterfaceKinds)
{
	OutOperations.Reset();
	OutInterfaceKinds.Reset();
	FFunctionClassification Classification;
	ClassifyFunctionNode(FunctionCall, Classification);
	OutOperations = MoveTemp(Classification.Operations);
	OutInterfaceKinds = MoveTemp(Classification.InterfaceKinds);
}

void GetCollisionOperationDetails(
	const UNiagaraNodeFunctionCall* FunctionCall,
	TArray<FString>& OutOperations,
	TArray<FString>& OutInterfaceKinds)
{
	GetCollisionOperationNames(FunctionCall, OutOperations, OutInterfaceKinds);
}

//++[SilverPalace] Begin add by wuziye 2026/09/05
bool MatchCollisionOperationSelector(
	const UNiagaraNodeFunctionCall* FunctionCall,
	const FString& OperationSelector,
	FString* OutCanonicalOperation)
{
	if (OutCanonicalOperation)
	{
		OutCanonicalOperation->Empty();
	}
	const FString NormalizedSelector = OperationSelector.TrimStartAndEnd();
	if (NormalizedSelector.IsEmpty())
	{
		return true;
	}
	TArray<FString> Operations;
	GetCollisionOperationNames(FunctionCall, Operations);
	for (const FString& Operation : Operations)
	{
		const bool bExactMatch =
			Operation.Equals(NormalizedSelector, ESearchCase::IgnoreCase);
		// Niagara 5.4 assets use both Collision.Collision and
		// CollisionQuery.CollisionQuery for the ordinary world-query module.
		// Keep the selector ergonomic while retaining the asset's canonical
		// spelling in the returned inventory.
		const bool bCollisionQueryAlias =
			NormalizedSelector.Equals(TEXT("CollisionQuery"), ESearchCase::IgnoreCase)
			&& (Operation.Equals(
					TEXT("CollisionQuery.CollisionQuery"),
					ESearchCase::IgnoreCase)
				|| Operation.Equals(
					TEXT("Collision.Collision"),
					ESearchCase::IgnoreCase));
		if (bExactMatch || bCollisionQueryAlias)
		{
			if (OutCanonicalOperation)
			{
				*OutCanonicalOperation = Operation;
			}
			return true;
		}
	}
	return false;
}
//--[SilverPalace] End add by wuziye

void SetCollisionOperationInventory(
	const TSharedRef<FJsonObject>& Object,
	const FNiagaraCollisionOperationInventory& Inventory,
	const int32 Limit)
{
	const int32 SafeLimit = FMath::Clamp(Limit, 1, MaxAuditLimit);
	TArray<FString> OperationNames;
	Inventory.GetKeys(OperationNames);
	OperationNames.Sort();

	TArray<TSharedPtr<FJsonValue>> InventoryValues;
	InventoryValues.Reserve(OperationNames.Num());
	bool bAnyLocationsTruncated = false;
	for (const FString& Operation : OperationNames)
	{
		const FNiagaraCollisionOperationInventoryEntry* Entry = Inventory.Find(Operation);
		if (!Entry)
		{
			continue;
		}
		TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetStringField(TEXT("operation"), Operation);
		EntryObject->SetNumberField(TEXT("count"), Entry->Count);
		TArray<FString> InterfaceKinds = Entry->InterfaceKinds;
		InterfaceKinds.Sort();
		SetStringArray(EntryObject, TEXT("interfaceKinds"), InterfaceKinds);
		TArray<FString> Locations = Entry->Locations;
		Locations.Sort();
		SetPathArray(EntryObject, TEXT("locations"), Locations);
		EntryObject->SetNumberField(TEXT("locationsReturned"), Locations.Num());
		const bool bLocationsTruncated = Entry->Count > Locations.Num();
		EntryObject->SetBoolField(TEXT("locationsTruncated"), bLocationsTruncated);
		bAnyLocationsTruncated |= bLocationsTruncated;
		InventoryValues.Add(MakeShared<FJsonValueObject>(EntryObject));
	}
	Object->SetArrayField(TEXT("operationInventory"), InventoryValues);
	Object->SetStringField(
		TEXT("operationInventorySchema"),
		TEXT("ue.niagara-operation-inventory.v2"));
	Object->SetNumberField(TEXT("operationInventoryCount"), InventoryValues.Num());
	Object->SetNumberField(TEXT("operationInventoryLocationLimit"), SafeLimit);
	Object->SetBoolField(TEXT("operationInventoryLocationsTruncated"), bAnyLocationsTruncated);
}

} // namespace UEAINiagaraGraphAuditExtensions

#endif
