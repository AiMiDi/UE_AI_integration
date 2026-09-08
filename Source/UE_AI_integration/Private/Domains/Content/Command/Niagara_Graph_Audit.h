#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UNiagaraGraph;
class UNiagaraDataInterface;
class UNiagaraNodeFunctionCall;

namespace UEAINiagaraGraphAuditExtensions
{
/**
 * Bounded operation-location inventory collected while auditing one or more
 * Niagara graphs. Counts are exact; Locations is intentionally capped by the
 * caller's requested result limit so a large graph cannot create an unbounded
 * JSON response.
 */
struct FNiagaraCollisionOperationInventoryEntry
{
	int32 Count = 0;
	TArray<FString> Locations;
	/** Collision data-interface/provider kinds observed for this operation. */
	TArray<FString> InterfaceKinds;
};

using FNiagaraCollisionOperationInventory =
	TMap<FString, FNiagaraCollisionOperationInventoryEntry>;

/**
 * Adds a detailed, read-only collision inventory to an existing audit graph
 * object. The caller owns the graph selection and legacy audit fields; this
 * extension only appends new fields and never mutates Niagara objects.
 */
void AppendDetailedCollisionAudit(
	UNiagaraGraph* Graph,
	bool bIncludeDisabled,
	int32 Limit,
	const TSharedRef<FJsonObject>& GraphObject,
	FNiagaraCollisionOperationInventory* AggregateInventory = nullptr,
	const FString& OperationSelector = FString());

/** Return the most specific known operation name for a function-call node. */
void GetCollisionOperationNames(
	const UNiagaraNodeFunctionCall* FunctionCall,
	TArray<FString>& OutOperations);

/**
 * Compatibility overload that returns provider kinds alongside operation
 * names without requiring a second classification pass.
 */
void GetCollisionOperationNames(
	const UNiagaraNodeFunctionCall* FunctionCall,
	TArray<FString>& OutOperations,
	TArray<FString>& OutInterfaceKinds);

/**
 * Return operation names plus the provider kinds that can be established from
 * the function-call signature/path. Shared PhysicsAsset/RigidMesh operation
 * names remain unclassified here when the asset does not retain its owner;
 * the detailed audit resolves those through the owning data-interface object.
 */
void GetCollisionOperationDetails(
	const UNiagaraNodeFunctionCall* FunctionCall,
	TArray<FString>& OutOperations,
	TArray<FString>& OutInterfaceKinds);

/**
 * Return true when a function-call node exposes the requested exact
 * collision operation. Matching is case-insensitive and trims the selector;
 * OutCanonicalOperation receives the operation name emitted in the audit
 * inventory when a match is found.
 */
bool MatchCollisionOperationSelector(
	const UNiagaraNodeFunctionCall* FunctionCall,
	const FString& OperationSelector,
	FString* OutCanonicalOperation = nullptr);

/** Return the concrete collision data-interface kind for a loaded object. */
FString GetCollisionDataInterfaceKind(const UNiagaraDataInterface* DataInterface);

/** True for every collision data interface covered by the detailed audit. */
bool IsSupportedCollisionDataInterface(const UNiagaraDataInterface* DataInterface);

/** Serialize an operation inventory with exact counts and bounded locations. */
void SetCollisionOperationInventory(
	const TSharedRef<FJsonObject>& Object,
	const FNiagaraCollisionOperationInventory& Inventory,
	int32 Limit);
}
