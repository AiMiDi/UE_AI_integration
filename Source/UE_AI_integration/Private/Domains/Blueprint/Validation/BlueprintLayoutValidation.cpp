// Blueprint Graph geometry and Comment layout validation.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Domains/Blueprint/BlueprintGraphEditorSupport.h"
#include "EdGraphNode_Comment.h"

namespace
{
using namespace UEAIIntegration::BlueprintGraph;

constexpr int32 MaxValidatedNodes = 256;
constexpr int32 MaxDiagnostics = 200;
constexpr double MaxGeometryTolerance = 256.0;

TSharedRef<FJsonObject> MakeDiagnostic(
	const FString& Rule,
	const FString& Severity,
	const FString& Message,
	const TArray<FGuid>& NodeIds)
{
	TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
	Diagnostic->SetStringField(TEXT("rule"), Rule);
	Diagnostic->SetStringField(TEXT("severity"), Severity);
	Diagnostic->SetStringField(TEXT("message"), Message);
	TArray<TSharedPtr<FJsonValue>> Ids;
	for (const FGuid& NodeId : NodeIds)
	{
		Ids.Add(MakeShared<FJsonValueString>(NodeId.ToString()));
	}
	Diagnostic->SetArrayField(TEXT("nodeIds"), Ids);
	return Diagnostic;
}

TSharedRef<FJsonObject> BuildStoredGeometryFingerprint(
	const TArray<UEdGraphNode*>& Nodes,
	const TMap<FGuid, FNodeBounds>& BoundsByNode)
{
	TArray<TSharedPtr<FJsonValue>> BoundsValues;
	for (const UEdGraphNode* Node : Nodes)
	{
		const FNodeBounds* Bounds =
			Node ? BoundsByNode.Find(Node->NodeGuid) : nullptr;
		if (!Bounds)
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = BoundsToJson(*Bounds);
		Entry->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
		BoundsValues.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
	Evidence->SetStringField(
		TEXT("schema"),
		TEXT("ue.blueprint-stored-graph-bounds.v1"));
	Evidence->SetStringField(TEXT("coordinateSpace"), TEXT("graph"));
	Evidence->SetArrayField(TEXT("nodeBounds"), BoundsValues);
	FString BoundsDigest;
	const bool bHasDigest =
		UEAIIntegration::Infrastructure::TryDigestJson(Evidence, BoundsDigest);

	TSharedRef<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(
		TEXT("schema"),
		TEXT("ue.blueprint-editor-geometry-fingerprint.v1"));
	Fingerprint->SetStringField(TEXT("coordinateSpace"), TEXT("graph"));
	Fingerprint->SetStringField(TEXT("geometrySource"), TEXT("stored"));
	Fingerprint->SetBoolField(TEXT("exact"), false);
	Fingerprint->SetStringField(
		TEXT("boundsHash"),
		bHasDigest ? TEXT("sha256:") + BoundsDigest : FString());
	Fingerprint->SetNumberField(TEXT("passCount"), 0);
	return Fingerprint;
}

void AddGeometryEvidence(
	const TSharedRef<FJsonObject>& Diagnostic,
	const UEdGraphNode* LeftNode,
	const UEdGraphNode* RightNode,
	const FNodeBounds& Left,
	const FNodeBounds& Right,
	const double MeasuredGap,
	const double RequiredGap,
	const double Tolerance,
	const FString& MeasurementAxis,
	const TSharedPtr<FJsonObject>& GeometryFingerprint)
{
	Diagnostic->SetStringField(TEXT("leftNodeId"), LeftNode->NodeGuid.ToString());
	Diagnostic->SetStringField(
		TEXT("rightNodeId"),
		RightNode->NodeGuid.ToString());
	Diagnostic->SetObjectField(TEXT("leftBounds"), BoundsToJson(Left));
	Diagnostic->SetObjectField(TEXT("rightBounds"), BoundsToJson(Right));
	Diagnostic->SetNumberField(TEXT("measuredGap"), MeasuredGap);
	Diagnostic->SetNumberField(TEXT("requiredGap"), RequiredGap);
	Diagnostic->SetNumberField(TEXT("tolerance"), Tolerance);
	Diagnostic->SetStringField(TEXT("measurementAxis"), MeasurementAxis);
	Diagnostic->SetNumberField(
		TEXT("overlapWidth"),
		FMath::Max(
			0.0,
			FMath::Min(Left.Right(), Right.Right())
				- FMath::Max(Left.X, Right.X)));
	Diagnostic->SetNumberField(
		TEXT("overlapHeight"),
		FMath::Max(
			0.0,
			FMath::Min(Left.Bottom(), Right.Bottom())
				- FMath::Max(Left.Y, Right.Y)));
	Diagnostic->SetObjectField(
		TEXT("geometryFingerprint"),
		GeometryFingerprint);
}

double MinimumOverlapDepth(
	const FNodeBounds& Left,
	const FNodeBounds& Right)
{
	const double OverlapWidth = FMath::Max(
		0.0,
		FMath::Min(Left.Right(), Right.Right())
			- FMath::Max(Left.X, Right.X));
	const double OverlapHeight = FMath::Max(
		0.0,
		FMath::Min(Left.Bottom(), Right.Bottom())
			- FMath::Max(Left.Y, Right.Y));
	return FMath::Min(OverlapWidth, OverlapHeight);
}

class FTool_ValidateBlueprintLayout final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.layout.validate");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		FString GeometryMode = TEXT("auto");
		Params->TryGetStringField(TEXT("geometryMode"), GeometryMode);
		if (GeometryMode != TEXT("auto")
			&& GeometryMode != TEXT("stored")
			&& GeometryMode != TEXT("editor"))
		{
			return InvalidRequest(
				TEXT("geometryMode must be auto, stored, or editor."));
		}

		double MinHorizontalGap = 32.0;
		double MinVerticalGap = 24.0;
		double CommentPadding = 32.0;
		double Tolerance = 1.0;
		bool bStrictWarnings = false;
		Params->TryGetNumberField(
			TEXT("minHorizontalGap"),
			MinHorizontalGap);
		Params->TryGetNumberField(
			TEXT("minVerticalGap"),
			MinVerticalGap);
		Params->TryGetNumberField(
			TEXT("commentPadding"),
			CommentPadding);
		Params->TryGetNumberField(TEXT("tolerance"), Tolerance);
		Params->TryGetBoolField(TEXT("strictWarnings"), bStrictWarnings);
		if (!FMath::IsFinite(MinHorizontalGap)
			|| !FMath::IsFinite(MinVerticalGap)
			|| !FMath::IsFinite(CommentPadding)
			|| !FMath::IsFinite(Tolerance)
			|| MinHorizontalGap < 0.0
			|| MinVerticalGap < 0.0
			|| CommentPadding < 0.0
			|| MinHorizontalGap > 4096.0
			|| MinVerticalGap > 4096.0
			|| CommentPadding > 4096.0
			|| Tolerance < 0.0
			|| Tolerance > MaxGeometryTolerance)
		{
			return InvalidRequest(
				TEXT("Layout gap and padding values must be finite numbers "
					"between 0 and 4096; tolerance must be between 0 and "
					"256 Graph Units."));
		}

		FContext Context;
		FMCPToolResult ContextResult =
			ResolveBlueprintGraph(Params, Context);
		if (!ContextResult.bSuccess)
		{
			return ContextResult;
		}

		bool bUseEditorGeometry = false;
		if (GeometryMode == TEXT("editor"))
		{
			ContextResult = ResolveGraphEditor(Context, true);
			if (!ContextResult.bSuccess)
			{
				return EditorUnavailable(
					TEXT("Exact Graph Editor geometry is unavailable."),
					TEXT("graph_geometry_unavailable"));
			}
			bUseEditorGeometry = true;
		}
		else if (GeometryMode == TEXT("auto"))
		{
			ContextResult = ResolveGraphEditor(Context, false);
			bUseEditorGeometry = ContextResult.bSuccess;
		}

		TSet<FGuid> RequestedIds;
		const TArray<TSharedPtr<FJsonValue>>* NodeIdValues = nullptr;
		if (Params->TryGetArrayField(TEXT("nodeIds"), NodeIdValues)
			&& NodeIdValues)
		{
			if (NodeIdValues->Num() > MaxValidatedNodes)
			{
				return InvalidRequest(
					TEXT("nodeIds is limited to 256 entries."));
			}
			for (const TSharedPtr<FJsonValue>& Value : *NodeIdValues)
			{
				FString NodeId;
				FGuid Guid;
				FString Error;
				if (!Value.IsValid()
					|| !Value->TryGetString(NodeId)
					|| !TryParseNodeGuid(NodeId, Guid, Error))
				{
					return InvalidRequest(
						Error.IsEmpty()
							? TEXT("Every nodeIds entry must be a Node GUID.")
							: Error);
				}
				if (!FindNode(Context.Graph, Guid))
				{
					return NotFound(
						FString::Printf(
							TEXT("Node '%s' was not found in graph '%s'."),
							*NodeId,
							*Context.GraphName));
				}
				RequestedIds.Add(Guid);
			}
		}

		TArray<UEdGraphNode*> Nodes;
		TMap<FGuid, FNodeBounds> BoundsByNode;
		for (UEdGraphNode* Node : Context.Graph->Nodes)
		{
			if (!Node
				|| (!RequestedIds.IsEmpty()
					&& !RequestedIds.Contains(Node->NodeGuid)))
			{
				continue;
			}
			if (Nodes.Num() >= MaxValidatedNodes)
			{
				return InvalidRequest(
					TEXT("Layout validation is limited to 256 nodes; "
						"provide nodeIds to narrow the scope."));
			}
			Nodes.Add(Node);
		}
		Nodes.Sort(
			[](const UEdGraphNode& Left, const UEdGraphNode& Right)
			{
				return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
			});

		TSharedPtr<FJsonObject> GeometryFingerprint;
		int32 ExactCount = 0;
		if (bUseEditorGeometry && !Nodes.IsEmpty())
		{
			FEditorGeometryFingerprint SettledFingerprint;
			FString GeometryError;
			if (TryCaptureSettledEditorGeometry(
					Context,
					Nodes,
					BoundsByNode,
					SettledFingerprint,
					GeometryError))
			{
				ExactCount = Nodes.Num();
				GeometryFingerprint =
					GeometryFingerprintToJson(SettledFingerprint);
				GeometryFingerprint->SetStringField(
					TEXT("geometrySource"),
					TEXT("editor"));
			}
			else if (GeometryMode == TEXT("editor"))
			{
				return EditorUnavailable(
					GeometryError,
					TEXT("graph_geometry_unavailable"));
			}
			else
			{
				bUseEditorGeometry = false;
			}
		}
		if (Nodes.IsEmpty())
		{
			bUseEditorGeometry = false;
		}
		if (!bUseEditorGeometry)
		{
			BoundsByNode.Reset();
			ExactCount = 0;
			for (UEdGraphNode* Node : Nodes)
			{
				FNodeBounds Bounds;
				TryGetNodeBounds(Context, Node, false, Bounds);
				BoundsByNode.Add(Node->NodeGuid, Bounds);
				ExactCount += Bounds.bExact ? 1 : 0;
			}
			GeometryFingerprint =
				BuildStoredGeometryFingerprint(Nodes, BoundsByNode);
		}

		const bool bGeometryComplete = ExactCount == Nodes.Num();

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		int32 TotalDiagnostics = 0;
		int32 ErrorCount = 0;
		int32 WarningCount = 0;
		auto AddDiagnostic =
			[&Diagnostics,
				&TotalDiagnostics,
				&ErrorCount,
				&WarningCount](
				const TSharedRef<FJsonObject>& Diagnostic)
			{
				++TotalDiagnostics;
				const FString Severity =
					Diagnostic->GetStringField(TEXT("severity"));
				ErrorCount += Severity == TEXT("error") ? 1 : 0;
				WarningCount += Severity == TEXT("warning") ? 1 : 0;
				if (Diagnostics.Num() < MaxDiagnostics)
				{
					Diagnostics.Add(
						MakeShared<FJsonValueObject>(Diagnostic));
				}
			};

		if (!bGeometryComplete)
		{
			TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
					TEXT("geometry_incomplete"),
					TEXT("warning"),
					TEXT("Some node dimensions are stored-only; overlap and "
						"spacing checks skip those nodes."),
					{});
			Diagnostic->SetNumberField(TEXT("tolerance"), Tolerance);
			Diagnostic->SetObjectField(
				TEXT("geometryFingerprint"),
				GeometryFingerprint);
			AddDiagnostic(Diagnostic);
		}

		for (int32 LeftIndex = 0; LeftIndex < Nodes.Num(); ++LeftIndex)
		{
			UEdGraphNode* LeftNode = Nodes[LeftIndex];
			const FNodeBounds& Left = BoundsByNode.FindChecked(
				LeftNode->NodeGuid);
			for (int32 RightIndex = LeftIndex + 1;
				RightIndex < Nodes.Num();
				++RightIndex)
			{
				UEdGraphNode* RightNode = Nodes[RightIndex];
				const FNodeBounds& Right = BoundsByNode.FindChecked(
					RightNode->NodeGuid);
				const bool bLeftComment =
					Cast<UEdGraphNode_Comment>(LeftNode) != nullptr;
				const bool bRightComment =
					Cast<UEdGraphNode_Comment>(RightNode) != nullptr;

				if (bLeftComment || bRightComment)
				{
					if (bLeftComment && bRightComment
						&& Left.bExact
						&& Right.bExact
						&& Intersects(Left, Right)
						&& MinimumOverlapDepth(Left, Right) > Tolerance
						&& !Contains(Left, Right)
						&& !Contains(Right, Left))
					{
						const double OverlapDepth =
							MinimumOverlapDepth(Left, Right);
						TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
								TEXT("comment_overlap"),
								TEXT("warning"),
								TEXT("Comment boxes overlap without nesting."),
								{LeftNode->NodeGuid, RightNode->NodeGuid});
						AddGeometryEvidence(
							Diagnostic,
							LeftNode,
							RightNode,
							Left,
							Right,
							-OverlapDepth,
							0.0,
							Tolerance,
							TEXT("minimumOverlap"),
							GeometryFingerprint);
						AddDiagnostic(Diagnostic);
					}
					continue;
				}
				if (!Left.bExact || !Right.bExact)
				{
					continue;
				}

				const double OverlapDepth =
					MinimumOverlapDepth(Left, Right);
				if (Intersects(Left, Right)
					&& OverlapDepth > Tolerance)
				{
					TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
							TEXT("node_overlap"),
							TEXT("error"),
							TEXT("Graph nodes overlap."),
							{LeftNode->NodeGuid, RightNode->NodeGuid});
					AddGeometryEvidence(
						Diagnostic,
						LeftNode,
						RightNode,
						Left,
						Right,
						-OverlapDepth,
						0.0,
						Tolerance,
						TEXT("minimumOverlap"),
						GeometryFingerprint);
					AddDiagnostic(Diagnostic);
					continue;
				}

				const bool bVerticalOverlap =
					Left.Y < Right.Bottom() && Left.Bottom() > Right.Y;
				const double HorizontalGap =
					Left.Right() <= Right.X
						? Right.X - Left.Right()
						: Left.X - Right.Right();
				if (bVerticalOverlap
					&& HorizontalGap >= 0.0
					&& HorizontalGap + Tolerance < MinHorizontalGap)
				{
					TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
							TEXT("horizontal_gap"),
							TEXT("warning"),
							TEXT("Horizontally adjacent nodes are too close."),
							{LeftNode->NodeGuid, RightNode->NodeGuid});
					AddGeometryEvidence(
						Diagnostic,
						LeftNode,
						RightNode,
						Left,
						Right,
						HorizontalGap,
						MinHorizontalGap,
						Tolerance,
						TEXT("horizontal"),
						GeometryFingerprint);
					AddDiagnostic(Diagnostic);
				}

				const bool bHorizontalOverlap =
					Left.X < Right.Right() && Left.Right() > Right.X;
				const double VerticalGap =
					Left.Bottom() <= Right.Y
						? Right.Y - Left.Bottom()
						: Left.Y - Right.Bottom();
				if (bHorizontalOverlap
					&& VerticalGap >= 0.0
					&& VerticalGap + Tolerance < MinVerticalGap)
				{
					TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
							TEXT("vertical_gap"),
							TEXT("warning"),
							TEXT("Vertically adjacent nodes are too close."),
							{LeftNode->NodeGuid, RightNode->NodeGuid});
					AddGeometryEvidence(
						Diagnostic,
						LeftNode,
						RightNode,
						Left,
						Right,
						VerticalGap,
						MinVerticalGap,
						Tolerance,
						TEXT("vertical"),
						GeometryFingerprint);
					AddDiagnostic(Diagnostic);
				}
			}
		}

		for (UEdGraphNode* Node : Nodes)
		{
			if (!Cast<UEdGraphNode_Comment>(Node))
			{
				continue;
			}
			const FNodeBounds& CommentBounds =
				BoundsByNode.FindChecked(Node->NodeGuid);
			if (!CommentBounds.bExact)
			{
				continue;
			}
			for (UEdGraphNode* Candidate : Nodes)
			{
				if (Candidate == Node
					|| Cast<UEdGraphNode_Comment>(Candidate))
				{
					continue;
				}
				const FNodeBounds& CandidateBounds =
					BoundsByNode.FindChecked(Candidate->NodeGuid);
				if (!CandidateBounds.bExact)
				{
					continue;
				}
				const double MeasuredPadding = FMath::Min(
					FMath::Min(
						CandidateBounds.X - CommentBounds.X,
						CandidateBounds.Y - CommentBounds.Y),
					FMath::Min(
						CommentBounds.Right() - CandidateBounds.Right(),
						CommentBounds.Bottom() - CandidateBounds.Bottom()));
				if (Contains(CommentBounds, CandidateBounds)
					&& MeasuredPadding + Tolerance < CommentPadding)
				{
					TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
							TEXT("comment_padding"),
							TEXT("warning"),
							TEXT("A contained node does not have the required "
								"Comment padding."),
							{Node->NodeGuid, Candidate->NodeGuid});
					AddGeometryEvidence(
						Diagnostic,
						Node,
						Candidate,
						CommentBounds,
						CandidateBounds,
						MeasuredPadding,
						CommentPadding,
						Tolerance,
						TEXT("minimumContainmentPadding"),
						GeometryFingerprint);
					AddDiagnostic(Diagnostic);
				}
				else if (Intersects(CommentBounds, CandidateBounds)
					&& MinimumOverlapDepth(
						CommentBounds,
						CandidateBounds) > Tolerance
					&& !Contains(CommentBounds, CandidateBounds))
				{
					const double OverlapDepth = MinimumOverlapDepth(
						CommentBounds,
						CandidateBounds);
					TSharedRef<FJsonObject> Diagnostic = MakeDiagnostic(
							TEXT("comment_partial_overlap"),
							TEXT("error"),
							TEXT("A Comment intersects a node without fully "
								"containing it."),
							{Node->NodeGuid, Candidate->NodeGuid});
					AddGeometryEvidence(
						Diagnostic,
						Node,
						Candidate,
						CommentBounds,
						CandidateBounds,
						-OverlapDepth,
						0.0,
						Tolerance,
						TEXT("minimumOverlap"),
						GeometryFingerprint);
					AddDiagnostic(Diagnostic);
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("blueprint"),
			Context.BlueprintInput);
		Result->SetStringField(TEXT("graph"), Context.GraphName);
		Result->SetStringField(
			TEXT("graphHash"),
			ComputeGraphHash(Context.Graph));
		Result->SetStringField(TEXT("geometryMode"), GeometryMode);
		Result->SetObjectField(
			TEXT("geometryFingerprint"),
			GeometryFingerprint);
		Result->SetNumberField(TEXT("tolerance"), Tolerance);
		Result->SetBoolField(TEXT("strictWarnings"), bStrictWarnings);
		Result->SetStringField(
			TEXT("geometryStatus"),
			Nodes.IsEmpty() && bUseEditorGeometry
				? TEXT("exact")
				: Nodes.Num() > 0 && ExactCount == Nodes.Num()
				? TEXT("exact")
				: ExactCount > 0
					? TEXT("partial")
					: TEXT("storedOnly"));
		Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
		TSharedRef<FJsonObject> GeometrySources = MakeShared<FJsonObject>();
		GeometrySources->SetNumberField(TEXT("editor"), ExactCount);
		GeometrySources->SetNumberField(
			TEXT("stored"),
			Nodes.Num() - ExactCount);
		Result->SetObjectField(TEXT("geometrySources"), GeometrySources);
		TSharedRef<FJsonObject> CheckScope = MakeShared<FJsonObject>();
		CheckScope->SetStringField(
			TEXT("kind"),
			RequestedIds.IsEmpty() ? TEXT("graph") : TEXT("nodes"));
		CheckScope->SetNumberField(
			TEXT("requestedNodeCount"),
			RequestedIds.Num());
		CheckScope->SetNumberField(TEXT("checkedNodeCount"), Nodes.Num());
		Result->SetObjectField(TEXT("checkScope"), CheckScope);
		Result->SetNumberField(
			TEXT("diagnosticCount"),
			TotalDiagnostics);
		Result->SetNumberField(TEXT("errorCount"), ErrorCount);
		Result->SetNumberField(TEXT("warningCount"), WarningCount);
		Result->SetNumberField(
			TEXT("returnedDiagnosticCount"),
			Diagnostics.Num());
		Result->SetBoolField(
			TEXT("truncated"),
			TotalDiagnostics > Diagnostics.Num());
		const bool bFailedByWarnings =
			bStrictWarnings && WarningCount > 0;
		TSharedRef<FJsonObject> ValidationSummary = MakeShared<FJsonObject>();
		ValidationSummary->SetBoolField(
			TEXT("failedByErrors"),
			ErrorCount > 0);
		ValidationSummary->SetBoolField(
			TEXT("failedByWarnings"),
			bFailedByWarnings);
		ValidationSummary->SetBoolField(
			TEXT("geometryComplete"),
			bGeometryComplete);
		Result->SetObjectField(TEXT("validationSummary"), ValidationSummary);
		if (ErrorCount > 0 || bFailedByWarnings)
		{
			Result->SetStringField(TEXT("conclusion"), TEXT("invalid"));
			Result->SetBoolField(TEXT("valid"), false);
		}
		else if (!bGeometryComplete)
		{
			Result->SetStringField(TEXT("conclusion"), TEXT("inconclusive"));
			Result->SetField(TEXT("valid"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Result->SetStringField(TEXT("conclusion"), TEXT("valid"));
			Result->SetBoolField(TEXT("valid"), true);
		}
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
		return FMCPToolResult::Ok(Result);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintLayoutValidationTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_ValidateBlueprintLayout>());
}
}
