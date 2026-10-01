#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Tools/MCPToolBase.h"
class UMaterialExpression;

namespace UEAIIntegration::MaterialQuery
{
	struct FNode
	{
		FString Id;
		FString ClassName;
		FString SearchText;
		FString DataHash;
		TSharedPtr<FJsonObject> Data;
		TArray<int32> Incoming;
		TArray<int32> Outgoing;
	};

	struct FEdge
	{
		int32 Source = INDEX_NONE;
		int32 Target = INDEX_NONE;
		int32 OutputIndex = 0;
		int32 InputIndex = 0;
		FString InputName;
		bool bNamedRerouteReference = false;
		int32 Mask = 0, MaskR = 0, MaskG = 0, MaskB = 0, MaskA = 0;
		FString Key;
	};

	// Capture-time asset classification is derived from the concrete UObject cast
	// and retained separately from the display class name.  Keeping these as two
	// fields prevents typed handle semantics from depending on a string prefix.
	enum class ESnapshotAssetKind : uint8
	{
		Unknown,
		Material,
		MaterialFunction
	};

	// Created completely before publication; cache clients only receive const data.
	// No UObject references: paging never loads assets or touches a live graph.
	struct FSnapshot
	{
		FString Id;
		FString AssetPath;
		FString AssetClass;
		ESnapshotAssetKind AssetKind = ESnapshotAssetKind::Unknown;
		FString CapturedAt;
		FString ProjectionHash;
		FString PreviewId;
		// Function-level metadata is part of the immutable projection. Keeping it
		// separate from node data lets callers inspect function settings without
		// manufacturing a synthetic graph node, while the capture hash still
		// covers every field that can invalidate a write boundary.
		TSharedPtr<FJsonObject> FunctionMetadata;
		double CapturedSeconds = 0;
		double CaptureMilliseconds = 0;
		uint64 ApproximateBytes = 0;
		bool bIncludeNamedReroutes = false;
		int32 NamedRerouteEdges = 0;
		int32 UnresolvedNamedReroutes = 0;
		TArray<FNode> Nodes;
		TArray<FEdge> Edges;
		TArray<int32> EdgesByKey;
		TMap<FString, int32> ById;
		TMap<FString, TArray<int32>> ByClass;
	};

	// Immutable proof produced by graph.boundary.get. It contains only snapshot
	// identities and bounded node/edge IDs; writers must still recapture live state.
	struct FBoundaryProof
	{
		FString BoundaryId;
		FString SnapshotId;
		FString AssetPath;
		FString PreviewId;
		FString ProjectionHash;
		double CreatedSeconds = 0;
		bool bRequiresSharedNodeConfirmation = false;
		TArray<FString> SelectedNodeIds;
		TArray<FString> WritableNodeIds;
		TArray<FString> ExternalDependencyNodeIds;
		TArray<FString> ExternallyConsumedNodeIds;
		TArray<FString> BoundaryEdgeKeys;
	};

	struct FBoundaryWriteValidation
	{
		TSharedPtr<const FSnapshot> SourceSnapshot;
		TSharedPtr<const FSnapshot> FreshSnapshot;
		TSharedPtr<const FBoundaryProof> Boundary;
		// Set only after ValidateBoundaryWrite has checked the caller's explicit
		// confirmation.  Writers expose this in read-back so a successful result
		// cannot be mistaken for an unconfirmed shared-node edit.
		bool bSharedNodeImpactConfirmed = false;
	};

	// Exposed internally for deterministic contract tests as well as the tool adapter.
	FMCPToolResult Capture(
		UObject* Asset,
		const FString& OriginalPath = FString(),
		const FString& PreviewId = FString(),
		const TArray<UMaterialExpression*>* WorkingExpressions = nullptr,
		bool bIncludeNamedReroutes = false,
		TSharedPtr<const FSnapshot>* OutSnapshot = nullptr,
		bool bPublish = true);
	FMCPToolResult ListNodes(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult Subgraph(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult Boundary(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult ValidateBoundaryWrite(
		UObject* Asset,
		const TSharedPtr<FJsonObject>& Params,
		FBoundaryWriteValidation& OutValidation);
	// Internal Workflow factory. The caller must first verify its approved exact
	// asset scope. It supplies a fresh whole-asset proof without changing public
	// traversal limits, replacing explicit proofs or granting shared confirmation.
	// Release OutSnapshotId after the single operation finishes.
	FMCPToolResult PrepareAssetScopeWriteBoundary(
		UObject* Asset,
		const TSharedPtr<FJsonObject>& Params,
		FString& OutSnapshotId);
	FMCPToolResult ListDefinitions(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult ResolveNodeSource(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult Diff(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult ExecutePlan(const TSharedPtr<FJsonObject>& Params);
	FMCPToolResult Release(const FString& SnapshotId);
}
