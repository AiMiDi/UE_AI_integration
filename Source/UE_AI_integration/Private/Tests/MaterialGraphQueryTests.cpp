#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Engine/Texture2D.h"
#include <limits>
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
TSharedPtr<FJsonObject> QueryParams(const FString& SnapshotId)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("snapshotId"), SnapshotId);
	return Params;
}

template <typename T, typename OwnerType>
T* AddQueryExpression(OwnerType* Owner)
{
	T* Expression = NewObject<T>(Owner);
	Owner->GetExpressionCollection().AddExpression(Expression);
	return Expression;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphQueryPagingTest,
	"UE_AI_integration.MaterialGraphQuery.PagingAndConsistency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphQueryPagingTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::MaterialQuery;
	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
	for (int32 Index = 0; Index < 5000; ++Index) AddQueryExpression<UMaterialExpressionConstant>(Function.Get())->R = Index;
	Function->GetOutermost()->SetDirtyFlag(false);
	const FMCPToolResult CaptureResult = Capture(Function.Get());
	if (!TestTrue(TEXT("Capture 5000 nodes without an editor graph"), CaptureResult.bSuccess)) return false;
	TestNull(TEXT("No function editor graph constructed"), Function->MaterialGraph);
	TestFalse(TEXT("Capture did not dirty package"), Function->GetOutermost()->IsDirty());
	TestEqual(TEXT("Count"), CaptureResult.Data->GetIntegerField(TEXT("totalNodes")), 5000);
	const FString Id = CaptureResult.Data->GetStringField(TEXT("snapshotId"));
	TestTrue(TEXT("Capture exposes typed asset reference"), CaptureResult.Data->HasField(TEXT("assetRef")));
	if (CaptureResult.Data->HasField(TEXT("assetRef")))
	{
		const auto AssetRef = CaptureResult.Data->GetObjectField(TEXT("assetRef"));
		TestEqual(TEXT("Asset reference kind"), AssetRef->GetStringField(TEXT("kind")), FString(TEXT("materialFunction")));
		TestEqual(TEXT("Asset reference snapshot binding"), AssetRef->GetStringField(TEXT("snapshotId")), Id);
		TestEqual(TEXT("Asset reference projection binding"), AssetRef->GetStringField(TEXT("projectionHash")), CaptureResult.Data->GetStringField(TEXT("projectionHash")));
		TestFalse(TEXT("Typed asset reference never has an empty projection hash"), AssetRef->GetStringField(TEXT("projectionHash")).IsEmpty());
	}
	auto Params = QueryParams(Id);
	Params->SetNumberField(TEXT("limit"), 137);
	TSet<FString> Seen;
	FString FirstCursor;
	int32 Pages = 0;
	const double Start = FPlatformTime::Seconds();
	while (Pages++ < 100)
	{
		const auto Page = ListNodes(Params);
		if (!TestTrue(TEXT("Page succeeds"), Page.bSuccess)) return false;
		for (const auto& Node : Page.Data->GetArrayField(TEXT("nodes")))
		{
			const auto NodeObject = Node->AsObject();
			if (Pages == 1)
			{
				TestTrue(TEXT("Node page exposes typed node reference"), NodeObject->HasField(TEXT("nodeRef")));
				if (NodeObject->HasField(TEXT("nodeRef")))
				{
					const auto Ref = NodeObject->GetObjectField(TEXT("nodeRef"));
					TestEqual(TEXT("Node reference ID matches legacy field"), Ref->GetStringField(TEXT("id")), NodeObject->GetStringField(TEXT("nodeId")));
					TestEqual(TEXT("Node reference snapshot binding"), Ref->GetStringField(TEXT("snapshotId")), Id);
					TestFalse(TEXT("Typed node reference never has an empty projection hash"), Ref->GetStringField(TEXT("projectionHash")).IsEmpty());
				}
			}
			const FString NodeId = Node->AsObject()->GetStringField(TEXT("nodeId"));
			TestFalse(TEXT("No duplicate across pages"), Seen.Contains(NodeId));
			Seen.Add(NodeId);
		}
		if (!Page.Data->GetBoolField(TEXT("hasMore"))) break;
		const FString Cursor = Page.Data->GetStringField(TEXT("nextCursor"));
		TestTrue(TEXT("Node page exposes bound continuation"), Page.Data->HasField(TEXT("continuation")));
		if (Page.Data->HasField(TEXT("continuation")))
		{
			const auto Continuation = Page.Data->GetObjectField(TEXT("continuation"));
			TestEqual(TEXT("Continuation snapshot binding"), Continuation->GetStringField(TEXT("snapshotId")), Id);
			TestEqual(TEXT("Continuation projection binding"), Continuation->GetStringField(TEXT("projectionHash")), CaptureResult.Data->GetStringField(TEXT("projectionHash")));
			TestEqual(TEXT("Continuation preserves legacy cursor"), Continuation->GetStringField(TEXT("nextCursor")), Cursor);
		}
		if (FirstCursor.IsEmpty()) FirstCursor = Cursor;
		Params->SetStringField(TEXT("cursor"), Cursor);
	}
	TestEqual(TEXT("Pagination covers every node"), Seen.Num(), 5000);
	AddInfo(FString::Printf(TEXT("Synthetic 5000-node function: capture %.3f ms; %d pages %.3f ms (in-process, excludes transport)."),
		CaptureResult.Data->GetNumberField(TEXT("captureMilliseconds")), Pages, (FPlatformTime::Seconds() - Start) * 1000));
	Params = QueryParams(Id);
	Params->SetStringField(TEXT("cursor"), FirstCursor);
	Params->SetStringField(TEXT("search"), TEXT("changed-filter"));
	TestFalse(TEXT("Cursor rejects different filter"), ListNodes(Params).bSuccess);
	Params = QueryParams(Id);
	Params->SetStringField(TEXT("search"), TEXT("no-node-can-match-this"));
	const auto Empty = ListNodes(Params);
	TestTrue(TEXT("Empty scan page has continuation"), Empty.Data->GetBoolField(TEXT("hasMore")));
	TestEqual(TEXT("Search scan is bounded"), Empty.Data->GetIntegerField(TEXT("scannedCount")), 4096);
	TestEqual(TEXT("Search has no matches"), Empty.Data->GetArrayField(TEXT("nodes")).Num(), 0);
	Params = QueryParams(Id);
	Params->SetStringField(TEXT("className"), TEXT("NotAnExpressionClass"));
	TestEqual(TEXT("Absent exact class has zero candidates"), ListNodes(Params).Data->GetIntegerField(TEXT("candidateCount")), 0);

	AddQueryExpression<UMaterialExpressionConstant>(Function.Get())->R = 42;
	const auto NewCapture = Capture(Function.Get());
	TestNotEqual(TEXT("Recapture changes projection hash"), NewCapture.Data->GetStringField(TEXT("projectionHash")), CaptureResult.Data->GetStringField(TEXT("projectionHash")));
	const auto OldPage = ListNodes(QueryParams(Id));
	TestEqual(TEXT("Old snapshot stays immutable after asset edit"), OldPage.Data->GetIntegerField(TEXT("totalNodes")), 5000);
	OldPage.Data->GetArrayField(TEXT("nodes"))[0]->AsObject()->SetStringField(TEXT("nodeId"), TEXT("mutated-consumer-copy"));
	TestNotEqual(TEXT("Consumer cannot mutate cached node"), ListNodes(QueryParams(Id)).Data->GetArrayField(TEXT("nodes"))[0]->AsObject()->GetStringField(TEXT("nodeId")), FString(TEXT("mutated-consumer-copy")));
	Release(Id);
	TestFalse(TEXT("Released snapshot is unavailable"), ListNodes(QueryParams(Id)).bSuccess);
	TestFalse(TEXT("Release is idempotent"), Release(Id).Data->GetBoolField(TEXT("released")));
	Release(NewCapture.Data->GetStringField(TEXT("snapshotId")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphQueryTraversalTest,
	"UE_AI_integration.MaterialGraphQuery.TraversalBudgets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphQueryTraversalTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::MaterialQuery;
	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
	auto* A = AddQueryExpression<UMaterialExpressionAdd>(Function.Get());
	auto* B = AddQueryExpression<UMaterialExpressionAdd>(Function.Get());
	auto* C = AddQueryExpression<UMaterialExpressionAdd>(Function.Get());
	A->A.Connect(0, B); B->A.Connect(0, C); C->A.Connect(0, A);
	for (int32 Index = 0; Index < 1024; ++Index) AddQueryExpression<UMaterialExpressionAdd>(Function.Get())->A.Connect(0, A);
	const auto Captured = Capture(Function.Get());
	if (!TestTrue(TEXT("Capture cyclic topology without compiling"), Captured.bSuccess)) return false;
	const FString Id = Captured.Data->GetStringField(TEXT("snapshotId"));
	auto Params = QueryParams(Id);
	Params->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(MCPMaterialInfrastructure::ExpressionNodeId(A))});
	Params->SetNumberField(TEXT("depth"), 32);
	const auto Cycle = Subgraph(Params);
	TestTrue(TEXT("Cycle traversal terminates"), Cycle.bSuccess);
	TestEqual(TEXT("Cycle visited once per node"), Cycle.Data->GetArrayField(TEXT("nodes")).Num(), 3);
	TestFalse(TEXT("Cycle upstream traversal complete"), Cycle.Data->GetBoolField(TEXT("truncated")));
	if (Cycle.Data->GetArrayField(TEXT("edges")).Num() > 0)
	{
		const auto Edge = Cycle.Data->GetArrayField(TEXT("edges"))[0]->AsObject();
		TestTrue(TEXT("Subgraph edge exposes typed source reference"), Edge->HasField(TEXT("sourceRef")));
		TestTrue(TEXT("Subgraph edge exposes typed target reference"), Edge->HasField(TEXT("targetRef")));
		if (Edge->HasField(TEXT("sourceRef")))
		{
			const auto SourceRef = Edge->GetObjectField(TEXT("sourceRef"));
			TestEqual(TEXT("Source reference ID matches legacy field"), SourceRef->GetStringField(TEXT("id")), Edge->GetStringField(TEXT("sourceNodeId")));
			TestEqual(TEXT("Source reference snapshot binding"), SourceRef->GetStringField(TEXT("snapshotId")), Id);
			TestFalse(TEXT("Typed source reference never has an empty projection hash"), SourceRef->GetStringField(TEXT("projectionHash")).IsEmpty());
		}
		if (Edge->HasField(TEXT("targetRef")))
		{
			const auto TargetRef = Edge->GetObjectField(TEXT("targetRef"));
			TestEqual(TEXT("Target reference ID matches legacy field"), TargetRef->GetStringField(TEXT("id")), Edge->GetStringField(TEXT("targetNodeId")));
			TestEqual(TEXT("Target reference snapshot binding"), TargetRef->GetStringField(TEXT("snapshotId")), Id);
			TestFalse(TEXT("Typed target reference never has an empty projection hash"), TargetRef->GetStringField(TEXT("projectionHash")).IsEmpty());
		}
	}
	TStrongObjectPtr<UMaterialFunction> BoundaryFunction(NewObject<UMaterialFunction>());
	auto* BoundarySource = AddQueryExpression<UMaterialExpressionConstant>(BoundaryFunction.Get());
	auto* BoundaryTarget = AddQueryExpression<UMaterialExpressionAdd>(BoundaryFunction.Get());
	BoundaryTarget->A.Connect(0, BoundarySource);
	const auto BoundaryCapture = Capture(BoundaryFunction.Get());
	if (TestTrue(TEXT("Boundary fixture captures"), BoundaryCapture.bSuccess))
	{
		auto BoundaryParams = QueryParams(BoundaryCapture.Data->GetStringField(TEXT("snapshotId")));
		BoundaryParams->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(MCPMaterialInfrastructure::ExpressionNodeId(BoundaryTarget))});
		BoundaryParams->SetNumberField(TEXT("depth"), 1);
		BoundaryParams->SetNumberField(TEXT("maxNodes"), 10);
		BoundaryParams->SetNumberField(TEXT("maxEdges"), 10);
		const auto BoundaryResult = Boundary(BoundaryParams);
		if (TestTrue(TEXT("Boundary exposes typed writable node references"), BoundaryResult.bSuccess))
		{
			TestTrue(TEXT("Boundary has writable node references"), BoundaryResult.Data->HasField(TEXT("writableNodeRefs")));
			if (BoundaryResult.Data->HasField(TEXT("writableNodeRefs")))
			{
				const auto Refs = BoundaryResult.Data->GetArrayField(TEXT("writableNodeRefs"));
				TestEqual(TEXT("Writable reference count matches legacy IDs"), Refs.Num(), BoundaryResult.Data->GetArrayField(TEXT("writableNodeIds")).Num());
				if (Refs.Num() > 0)
				{
					const auto Ref = Refs[0]->AsObject();
					TestTrue(TEXT("Writable reference has typed kind"), Ref->GetStringField(TEXT("kind")) == TEXT("materialNode"));
					TestEqual(TEXT("Writable reference snapshot binding"), Ref->GetStringField(TEXT("snapshotId")), BoundaryCapture.Data->GetStringField(TEXT("snapshotId")));
					TestFalse(TEXT("Writable reference never has an empty projection hash"), Ref->GetStringField(TEXT("projectionHash")).IsEmpty());
				}
			}
		}
		Release(BoundaryCapture.Data->GetStringField(TEXT("snapshotId")));
	}
	Params->SetStringField(TEXT("direction"), TEXT("downstream"));
	Params->SetNumberField(TEXT("maxNodes"), 5);
	Params->SetNumberField(TEXT("maxEdges"), 7);
	const auto Hub = Subgraph(Params);
	TestTrue(TEXT("Hub query succeeds"), Hub.bSuccess);
	TestTrue(TEXT("Hub scan bounded"), Hub.Data->GetIntegerField(TEXT("inspectedAdjacencies")) <= 256);
	TestTrue(TEXT("Hub node cap"), Hub.Data->GetArrayField(TEXT("nodes")).Num() <= 5);
	TestTrue(TEXT("Hub combined edge cap"), Hub.Data->GetArrayField(TEXT("edges")).Num() + Hub.Data->GetArrayField(TEXT("boundaryEdges")).Num() <= 7);
	TestTrue(TEXT("Hub reports incomplete edge coverage"), !Hub.Data->GetBoolField(TEXT("edgeCoverageComplete")));
	TestTrue(TEXT("Hub reports truncation"), Hub.Data->GetBoolField(TEXT("truncated")));
	Params->SetNumberField(TEXT("depth"), 0);
	TestEqual(TEXT("Depth zero returns seeds only"), Subgraph(Params).Data->GetArrayField(TEXT("nodes")).Num(), 1);
	Params->SetNumberField(TEXT("maxNodes"), 1.5);
	TestFalse(TEXT("Fractional limit rejected"), Subgraph(Params).bSuccess);
	Release(Id);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphQueryIdentityTest,
	"UE_AI_integration.MaterialGraphQuery.IdentityAndEviction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphQueryIdentityTest::RunTest(const FString& Parameters)
{
	using namespace UEAIIntegration::MaterialQuery;
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>());
	auto* Constant = AddQueryExpression<UMaterialExpressionConstant>(Material.Get());
	Material->GetExpressionInputForProperty(MP_Roughness)->Connect(0, Constant);
	Material->GetOutermost()->SetDirtyFlag(false);
	const auto Before = Capture(Material.Get());
	if (!TestTrue(TEXT("Capture material root inputs"), Before.bSuccess)) return false;
	TestEqual(TEXT("Material asset reference kind"), Before.Data->GetObjectField(TEXT("assetRef"))->GetStringField(TEXT("kind")), FString(TEXT("material")));
	TestNull(TEXT("Read model needs no material graph"), Material->MaterialGraph.Get());
	TestEqual(TEXT("Root edge included"), Before.Data->GetIntegerField(TEXT("totalEdges")), 1);
	MCPMaterialInfrastructure::EnsureMaterialGraph(Material.Get());
	const FString StableId = MCPMaterialInfrastructure::ExpressionNodeId(Constant);
	TestTrue(TEXT("Mutation resolver accepts stable ID"), MCPMaterialInfrastructure::MatchesMaterialNode(Constant->GraphNode, StableId));
	Material->MaterialGraph->RebuildGraph();
	Material->GetOutermost()->SetDirtyFlag(false);
	const auto After = Capture(Material.Get());
	TestEqual(TEXT("Editor graph reconstruction preserves projection hash"), After.Data->GetStringField(TEXT("projectionHash")), Before.Data->GetStringField(TEXT("projectionHash")));
	TestFalse(TEXT("Read and editor projection preserve clean state"), Material->GetOutermost()->IsDirty());
	TArray<FString> Ids;
	for (int32 Index = 0; Index < 9; ++Index) Ids.Add(Capture(Material.Get()).Data->GetStringField(TEXT("snapshotId")));
	TestFalse(TEXT("Capacity evicts oldest snapshot"), ListNodes(QueryParams(Ids[0])).bSuccess);
	TestTrue(TEXT("Newest snapshot available"), ListNodes(QueryParams(Ids.Last())).bSuccess);
	auto WrongCursor = QueryParams(Ids.Last());
	WrongCursor->SetNumberField(TEXT("limit"), 1);
	const FString Cursor = ListNodes(WrongCursor).Data->GetStringField(TEXT("nextCursor"));
	WrongCursor->SetStringField(TEXT("snapshotId"), Ids[1]);
	WrongCursor->SetStringField(TEXT("cursor"), Cursor);
	TestFalse(TEXT("Cursor cannot cross snapshots"), ListNodes(WrongCursor).bSuccess);
	for (const auto& Id : Ids) Release(Id);
	Release(Before.Data->GetStringField(TEXT("snapshotId")));
	Release(After.Data->GetStringField(TEXT("snapshotId")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphNamedRerouteTest,
	"UE_AI_integration.MaterialGraphQuery.NamedRerouteDependencies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphNamedRerouteTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	using namespace MCPMaterialInfrastructure;
	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
	auto* Value = AddQueryExpression<UMaterialExpressionConstant>(Function.Get());
	auto* Declaration = AddQueryExpression<UMaterialExpressionNamedRerouteDeclaration>(Function.Get());
	Declaration->Name = TEXT("DetailWeight"); Declaration->Input.Connect(0, Value);
	auto* Usage = AddQueryExpression<UMaterialExpressionNamedRerouteUsage>(Function.Get());
	Usage->Declaration = Declaration; Usage->DeclarationGuid = FGuid::NewGuid(); // Compiler follows the pointer even when copied GUID differs.
	auto* Consumer = AddQueryExpression<UMaterialExpressionAdd>(Function.Get()); Consumer->A.Connect(0, Usage);
	const bool WasDirty = Function->GetOutermost()->IsDirty();
	auto Plain = Capture(Function.Get());
	auto Semantic = Capture(Function.Get(), FString(), FString(), nullptr, true);
	if (!TestTrue(TEXT("Both topology modes capture"), Plain.bSuccess && Semantic.bSuccess)) return false;
	const FString PlainId = Plain.Data->GetStringField(TEXT("snapshotId")), Id = Semantic.Data->GetStringField(TEXT("snapshotId"));
	TestEqual(TEXT("One implicit dependency"), Semantic.Data->GetIntegerField(TEXT("namedRerouteEdges")), 1);
	TestEqual(TEXT("No unresolved local references"), Semantic.Data->GetIntegerField(TEXT("unresolvedNamedReroutes")), 0);
	TestNotEqual(TEXT("Topology mode participates in hash"), Plain.Data->GetStringField(TEXT("projectionHash")), Semantic.Data->GetStringField(TEXT("projectionHash")));
	auto Q = QueryParams(PlainId); Q->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(ExpressionNodeId(Consumer))}); Q->SetNumberField(TEXT("depth"), 8);
	TestEqual(TEXT("Default projection retains explicit-only behavior"), Subgraph(Q).Data->GetArrayField(TEXT("nodes")).Num(), 2);
	Q->SetStringField(TEXT("snapshotId"), Id);
	const auto Upstream = Subgraph(Q);
	TestEqual(TEXT("Semantic upstream reaches original value through declaration"), Upstream.Data->GetArrayField(TEXT("nodes")).Num(), 4);
	TestFalse(TEXT("Complete local walk is not truncated"), Upstream.Data->GetBoolField(TEXT("truncated")));
	int32 Implicit = 0;
	for (const auto& E : Upstream.Data->GetArrayField(TEXT("edges")))
	{
		const auto Edge = E->AsObject();
		if (Edge->GetStringField(TEXT("kind")) != TEXT("namedRerouteReference")) continue;
		++Implicit;
		TestEqual(TEXT("Reference source is declaration"), Edge->GetStringField(TEXT("sourceNodeId")), ExpressionNodeId(Declaration));
		TestEqual(TEXT("Reference target is usage"), Edge->GetStringField(TEXT("targetNodeId")), ExpressionNodeId(Usage));
		TestEqual(TEXT("Reference has no editable input index"), Edge->GetIntegerField(TEXT("inputIndex")), INDEX_NONE);
		TestFalse(TEXT("Reference cannot be edited as a wire"), Edge->GetBoolField(TEXT("editableConnection")));
	}
	TestEqual(TEXT("Reference edge emitted once"), Implicit, 1);
	Q->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(ExpressionNodeId(Value))}); Q->SetStringField(TEXT("direction"), TEXT("downstream"));
	TestEqual(TEXT("Downstream impact crosses named reroute"), Subgraph(Q).Data->GetArrayField(TEXT("nodes")).Num(), 4);
	Q->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(ExpressionNodeId(Usage))}); Q->SetStringField(TEXT("direction"), TEXT("upstream")); Q->SetNumberField(TEXT("depth"), 0);
	const auto Boundary = Subgraph(Q);
	TestTrue(TEXT("Implicit edge respects depth budget"), Boundary.Data->GetBoolField(TEXT("truncated")));
	TestEqual(TEXT("Implicit boundary remains typed"), Boundary.Data->GetArrayField(TEXT("boundaryEdges"))[0]->AsObject()->GetStringField(TEXT("kind")), FString(TEXT("namedRerouteReference")));
	auto Search = QueryParams(Id); Search->SetStringField(TEXT("search"), TEXT("DetailWeight"));
	TestEqual(TEXT("Reroute name finds declaration and usage"), ListNodes(Search).Data->GetArrayField(TEXT("nodes")).Num(), 2);

	// Do not repair missing references from GUIDs or collapse foreign same-name objects into local IDs.
	Usage->Declaration = nullptr; Usage->DeclarationGuid = Declaration->VariableGuid;
	auto Missing = Capture(Function.Get(), FString(), FString(), nullptr, true);
	if (!TestTrue(TEXT("Broken reference remains queryable for diagnosis"), Missing.bSuccess)) return false;
	TestEqual(TEXT("Missing reference explicitly incomplete"), Missing.Data->GetStringField(TEXT("namedRerouteCoverage")), FString(TEXT("incomplete")));
	TestEqual(TEXT("No invented GUID fallback edge"), Missing.Data->GetIntegerField(TEXT("namedRerouteEdges")), 0);
	TestEqual(TEXT("Missing reference counted"), Missing.Data->GetIntegerField(TEXT("unresolvedNamedReroutes")), 1);
	auto Compare = MakeShared<FJsonObject>(); Compare->SetStringField(TEXT("beforeSnapshotId"), Id); Compare->SetStringField(TEXT("afterSnapshotId"), Missing.Data->GetStringField(TEXT("snapshotId")));
	const auto ReferenceDiff = Diff(Compare); bool RemovedReference = false;
	if (!TestTrue(TEXT("Named reference snapshots compare"), ReferenceDiff.bSuccess)) return false;
	for (const auto& V : ReferenceDiff.Data->GetArrayField(TEXT("changes")))
	{
		const auto Change = V->AsObject();
		if (Change->GetStringField(TEXT("entity")) == TEXT("edge") && Change->GetStringField(TEXT("change")) == TEXT("removed"))
			RemovedReference |= Change->GetObjectField(TEXT("edge"))->GetStringField(TEXT("kind")) == TEXT("namedRerouteReference");
	}
	TestTrue(TEXT("Diff preserves implicit reference edge type"), RemovedReference);
	TStrongObjectPtr<UMaterialFunction> Other(NewObject<UMaterialFunction>());
	auto* Foreign = NewObject<UMaterialExpressionNamedRerouteDeclaration>(Other.Get(), Declaration->GetFName());
	Other->GetExpressionCollection().AddExpression(Foreign); Usage->Declaration = Foreign;
	auto External = Capture(Function.Get(), FString(), FString(), nullptr, true);
	TestEqual(TEXT("Foreign declaration cannot alias local same-name node"), External.Data->GetIntegerField(TEXT("namedRerouteEdges")), 0);
	Search = QueryParams(External.Data->GetStringField(TEXT("snapshotId"))); Search->SetStringField(TEXT("className"), TEXT("MaterialExpressionNamedRerouteUsage"));
	TestEqual(TEXT("External reference is labelled"), ListNodes(Search).Data->GetArrayField(TEXT("nodes"))[0]->AsObject()->GetStringField(TEXT("referenceStatus")), FString(TEXT("outsideSnapshot")));
	Q->SetStringField(TEXT("snapshotId"), Id); Q->SetNumberField(TEXT("depth"), 8);
	TestEqual(TEXT("Old snapshot is detached from later reference changes"), Subgraph(Q).Data->GetArrayField(TEXT("nodes")).Num(), 3);
	for (const auto& S : {PlainId, Id, Missing.Data->GetStringField(TEXT("snapshotId")), External.Data->GetStringField(TEXT("snapshotId"))}) Release(S);

	Usage->Declaration = Declaration; Declaration->Input.Connect(0, Usage);
	for (int32 I = 0; I < 1024; ++I) AddQueryExpression<UMaterialExpressionNamedRerouteUsage>(Function.Get())->Declaration = Declaration;
	auto Cycle = Capture(Function.Get(), FString(), FString(), nullptr, true);
	if (!TestTrue(TEXT("Named reference cycle capture terminates"), Cycle.bSuccess)) return false;
	Q = QueryParams(Cycle.Data->GetStringField(TEXT("snapshotId"))); Q->SetArrayField(TEXT("nodeIds"), {MakeShared<FJsonValueString>(ExpressionNodeId(Declaration))}); Q->SetNumberField(TEXT("depth"), 32);
	TestEqual(TEXT("Named reference cycle visits each node once"), Subgraph(Q).Data->GetArrayField(TEXT("nodes")).Num(), 2);
	Q->SetStringField(TEXT("direction"), TEXT("downstream")); Q->SetNumberField(TEXT("maxNodes"), 5); Q->SetNumberField(TEXT("maxEdges"), 7);
	const auto Hub = Subgraph(Q);
	TestTrue(TEXT("Implicit fan-out uses adjacency scan budget"), Hub.Data->GetIntegerField(TEXT("inspectedAdjacencies")) <= 256);
	TestTrue(TEXT("Implicit fan-out reports truncation"), Hub.Data->GetBoolField(TEXT("truncated")));
	TestTrue(TEXT("Implicit fan-out respects combined edge budget"), Hub.Data->GetArrayField(TEXT("edges")).Num() + Hub.Data->GetArrayField(TEXT("boundaryEdges")).Num() <= 7);
	Release(Cycle.Data->GetStringField(TEXT("snapshotId")));
	TestEqual(TEXT("Query never changes package dirty state"), Function->GetOutermost()->IsDirty(), WasDirty);
	TestNull(TEXT("Query never constructs an editor graph"), Function->MaterialGraph);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphDiffTest,
	"UE_AI_integration.MaterialGraphQuery.SnapshotDiff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialGraphDiffTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	using namespace MCPMaterialInfrastructure;
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>());
	auto* A = AddQueryExpression<UMaterialExpressionConstant>(M.Get());
	auto* B = AddQueryExpression<UMaterialExpressionConstant>(M.Get());
	auto* Root = M->GetExpressionInputForProperty(MP_EmissiveColor); Root->Connect(0, A);
	const auto Before = Capture(M.Get());
	M->GetExpressionCollection().RemoveExpression(A); B->R = 0.3f; B->MaterialExpressionEditorX = 123;
	auto* C = AddQueryExpression<UMaterialExpressionConstant>(M.Get()); Root->Connect(0, B);
	const auto After = Capture(M.Get());
	if (!TestTrue(TEXT("Capture both graph revisions"), Before.bSuccess && After.bSuccess)) return false;
	auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("beforeSnapshotId"), Before.Data->GetStringField(TEXT("snapshotId"))); P->SetStringField(TEXT("afterSnapshotId"), After.Data->GetStringField(TEXT("snapshotId"))); P->SetNumberField(TEXT("limit"), 1);
	int32 Added = 0, Removed = 0, Modified = 0, Edges = 0; FString FirstCursor;
	for (int32 I = 0; I < 20; ++I)
	{
		const auto Page = Diff(P); if (!TestTrue(TEXT("Diff page succeeds"), Page.bSuccess)) return false;
		TestTrue(TEXT("Per-page result limit"), Page.Data->GetArrayField(TEXT("changes")).Num() <= 1);
		if (Page.Data->GetBoolField(TEXT("hasMore")) && I == 0)
		{
			TestTrue(TEXT("Diff exposes bound continuation"), Page.Data->HasField(TEXT("continuation")));
			if (Page.Data->HasField(TEXT("continuation")))
			{
				const auto Continuation = Page.Data->GetObjectField(TEXT("continuation"));
				TestEqual(TEXT("Diff continuation before snapshot"), Continuation->GetStringField(TEXT("beforeSnapshotId")), Before.Data->GetStringField(TEXT("snapshotId")));
				TestEqual(TEXT("Diff continuation after snapshot"), Continuation->GetStringField(TEXT("afterSnapshotId")), After.Data->GetStringField(TEXT("snapshotId")));
				TestEqual(TEXT("Diff continuation preserves legacy cursor"), Continuation->GetStringField(TEXT("nextCursor")), Page.Data->GetStringField(TEXT("nextCursor")));
				TestTrue(TEXT("Diff continuation preserves merge phase"), Continuation->HasField(TEXT("phase")));
				TestTrue(TEXT("Diff continuation preserves left and right indexes"), Continuation->HasField(TEXT("left")) && Continuation->HasField(TEXT("right")));
				if (Continuation->HasField(TEXT("left")) && Continuation->HasField(TEXT("right")))
				{
					TestTrue(TEXT("Diff continuation left index is non-negative"), Continuation->GetIntegerField(TEXT("left")) >= 0);
					TestTrue(TEXT("Diff continuation right index is non-negative"), Continuation->GetIntegerField(TEXT("right")) >= 0);
				}
			}
		}
		for (const auto& V : Page.Data->GetArrayField(TEXT("changes")))
		{
			const auto Change = V->AsObject(); const FString Kind = Change->GetStringField(TEXT("change"));
			if (Change->GetStringField(TEXT("entity")) == TEXT("edge")) { ++Edges; continue; }
			const FString Id = Change->GetStringField(TEXT("nodeId"));
			if (Kind == TEXT("added")) { ++Added; TestEqual(TEXT("Added node identity"), Id, ExpressionNodeId(C)); }
			if (Kind == TEXT("removed")) { ++Removed; TestEqual(TEXT("Removed node identity"), Id, ExpressionNodeId(A)); }
			if (Kind == TEXT("modified"))
			{
				++Modified; TestEqual(TEXT("Modified node identity"), Id, ExpressionNodeId(B));
				TestTrue(TEXT("Diff node change exposes typed node reference"), Change->HasField(TEXT("nodeRef")));
				if (Change->HasField(TEXT("nodeRef")))
				{
					const auto Ref = Change->GetObjectField(TEXT("nodeRef"));
					TestEqual(TEXT("Diff node reference ID matches legacy field"), Ref->GetStringField(TEXT("id")), Id);
					TestEqual(TEXT("Diff node reference binds after snapshot"), Ref->GetStringField(TEXT("snapshotId")), After.Data->GetStringField(TEXT("snapshotId")));
					TestFalse(TEXT("Diff node reference never has an empty projection hash"), Ref->GetStringField(TEXT("projectionHash")).IsEmpty());
				}
				TSet<FString> Fields; for (const auto& Field : Change->GetArrayField(TEXT("changedFields"))) Fields.Add(Field->AsString());
				TestTrue(TEXT("Value and position changes identified"), Fields.Contains(TEXT("value")) && Fields.Contains(TEXT("x")));
			}
		}
		if (!Page.Data->GetBoolField(TEXT("hasMore"))) break;
		P->SetStringField(TEXT("cursor"), Page.Data->GetStringField(TEXT("nextCursor"))); if (FirstCursor.IsEmpty()) FirstCursor = P->GetStringField(TEXT("cursor"));
	}
	TestEqual(TEXT("One added node"), Added, 1); TestEqual(TEXT("One removed node"), Removed, 1); TestEqual(TEXT("One modified node"), Modified, 1); TestEqual(TEXT("Rewire is a removed and added edge"), Edges, 2);
	P->SetStringField(TEXT("cursor"), FirstCursor); P->SetStringField(TEXT("afterSnapshotId"), Before.Data->GetStringField(TEXT("snapshotId")));
	TestFalse(TEXT("Cursor cannot cross snapshot pairs"), Diff(P).bSuccess);
	P->RemoveField(TEXT("cursor")); TestTrue(TEXT("Identical projection is explicit"), Diff(P).Data->GetBoolField(TEXT("projectionEqual")));
	Root->Mask = 1; Root->MaskR = 0; Root->MaskG = 1;
	const auto Masked = Capture(M.Get()); P->SetStringField(TEXT("beforeSnapshotId"), After.Data->GetStringField(TEXT("snapshotId"))); P->SetStringField(TEXT("afterSnapshotId"), Masked.Data->GetStringField(TEXT("snapshotId"))); P->SetNumberField(TEXT("limit"), 50);
	const auto MaskDiff = Diff(P);
	TestFalse(TEXT("Channel-only rewire changes projection"), MaskDiff.Data->GetBoolField(TEXT("projectionEqual")));
	TestEqual(TEXT("Mask-only change emits two edge records"), MaskDiff.Data->GetArrayField(TEXT("changes")).Num(), 2);
	for (const auto& V : MaskDiff.Data->GetArrayField(TEXT("changes"))) TestEqual(TEXT("Mask change does not invent node property edits"), V->AsObject()->GetStringField(TEXT("entity")), FString(TEXT("edge")));
	const auto Semantic = Capture(M.Get(), FString(), FString(), nullptr, true); P->SetStringField(TEXT("afterSnapshotId"), Semantic.Data->GetStringField(TEXT("snapshotId")));
	TestFalse(TEXT("Reject different topology projections"), Diff(P).bSuccess);
	const auto Preview = Capture(M.Get(), M->GetPathName(), TEXT("preview-other-session")); P->SetStringField(TEXT("afterSnapshotId"), Preview.Data->GetStringField(TEXT("snapshotId")));
	TestFalse(TEXT("Reject asset/preview context mixing"), Diff(P).bSuccess);
	Release(After.Data->GetStringField(TEXT("snapshotId"))); TestFalse(TEXT("Released baseline fails explicitly"), Diff(P).bSuccess);
	for (const auto& S : {Before, Masked, Semantic, Preview}) Release(S.Data->GetStringField(TEXT("snapshotId")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphSparseDiffTest,
	"UE_AI_integration.MaterialGraphQuery.SparseDiffBudget", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialGraphSparseDiffTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	TStrongObjectPtr<UMaterialFunction> F(NewObject<UMaterialFunction>());
	TArray<UMaterialExpressionConstant*> Nodes;
	for (int32 I = 0; I < 5000; ++I) Nodes.Add(AddQueryExpression<UMaterialExpressionConstant>(F.Get()));
	Nodes.Sort([](const auto& A, const auto& B) { return MCPMaterialInfrastructure::ExpressionNodeId(&A).Compare(MCPMaterialInfrastructure::ExpressionNodeId(&B), ESearchCase::CaseSensitive) < 0; });
	const auto Before = Capture(F.Get()); Nodes.Last()->R = 0.625f; const auto After = Capture(F.Get());
	if (!TestTrue(TEXT("Sparse diff captures fit cache"), Before.bSuccess && After.bSuccess)) return false;
	auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("beforeSnapshotId"), Before.Data->GetStringField(TEXT("snapshotId"))); P->SetStringField(TEXT("afterSnapshotId"), After.Data->GetStringField(TEXT("snapshotId")));
	const auto First = Diff(P); if (!TestTrue(TEXT("Sparse first page succeeds"), First.bSuccess)) return false;
	TestEqual(TEXT("First page scans bounded prefix only"), First.Data->GetIntegerField(TEXT("scannedCount")), 4096);
	TestEqual(TEXT("Sparse prefix can return zero changes"), First.Data->GetArrayField(TEXT("changes")).Num(), 0);
	TestTrue(TEXT("Empty page preserves continuation"), First.Data->GetBoolField(TEXT("hasMore")));
	P->SetStringField(TEXT("cursor"), First.Data->GetStringField(TEXT("nextCursor")));
	const auto Last = Diff(P); TestEqual(TEXT("Continuation does not rescan prefix"), Last.Data->GetIntegerField(TEXT("scannedCount")), 904);
	TestEqual(TEXT("Sparse final change returned once"), Last.Data->GetArrayField(TEXT("changes")).Num(), 1); TestFalse(TEXT("Diff completes"), Last.Data->GetBoolField(TEXT("hasMore")));
	Nodes.Last()->R = 0.875f;
	TestEqual(TEXT("Diff remains detached from live edits"), Diff(P).Data->GetArrayField(TEXT("changes"))[0]->AsObject()->GetStringField(TEXT("afterHash")), Last.Data->GetArrayField(TEXT("changes"))[0]->AsObject()->GetStringField(TEXT("afterHash")));
	TestNull(TEXT("Diff needs no live editor graph"), F->MaterialGraph);
	Release(Before.Data->GetStringField(TEXT("snapshotId"))); Release(After.Data->GetStringField(TEXT("snapshotId")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialGraphEditablePropertiesTest,
	"UE_AI_integration.MaterialGraphQuery.EditableProperties", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialGraphEditablePropertiesTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	using namespace MCPMaterialInfrastructure;
	TStrongObjectPtr<UMaterialFunction> F(NewObject<UMaterialFunction>());
	auto* Scalar = AddQueryExpression<UMaterialExpressionScalarParameter>(F.Get());
	auto* Vector = AddQueryExpression<UMaterialExpressionVectorParameter>(F.Get());
	auto* Switch = AddQueryExpression<UMaterialExpressionStaticSwitchParameter>(F.Get());
	auto* Texture = AddQueryExpression<UMaterialExpressionTextureSampleParameter2D>(F.Get());
	auto* Custom = AddQueryExpression<UMaterialExpressionCustom>(F.Get()); Custom->Code = TEXT("return 0.25;");
	auto* Input = AddQueryExpression<UMaterialExpressionFunctionInput>(F.Get());
	Custom->AdditionalDefines.AddDefaulted(); Custom->AdditionalDefines.Last().DefineName = TEXT("GAIN"); Custom->AdditionalDefines.Last().DefineValue = TEXT("0.5");
	Custom->IncludeFilePaths.Add(TEXT("/Project/TestA.ush"));
	Custom->AdditionalOutputs.AddDefaulted(); Custom->AdditionalOutputs.Last().OutputName = TEXT("Aux");
	Scalar->Desc = FString::ChrN(300, TCHAR('a'));
	const bool WasDirty = F->GetOutermost()->IsDirty();
	auto CheckEdit = [&](UMaterialExpression* Expression, const TCHAR* ExpectedField, TFunctionRef<void()> Edit)
	{
		const auto Before = Capture(F.Get());
		if (!TestTrue(TEXT("Baseline captures"), Before.bSuccess)) return;
		Edit();
		const auto After = Capture(F.Get());
		if (!TestTrue(TEXT("Edited state captures"), After.bSuccess)) { Release(Before.Data->GetStringField(TEXT("snapshotId"))); return; }
		auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("beforeSnapshotId"), Before.Data->GetStringField(TEXT("snapshotId"))); P->SetStringField(TEXT("afterSnapshotId"), After.Data->GetStringField(TEXT("snapshotId")));
		const auto Delta = Diff(P);
		if (TestTrue(TEXT("Property diff succeeds"), Delta.bSuccess) && TestEqual(FString::Printf(TEXT("Exactly one expression modified for %s"), ExpectedField), Delta.Data->GetArrayField(TEXT("changes")).Num(), 1))
		{
			const auto Change = Delta.Data->GetArrayField(TEXT("changes"))[0]->AsObject();
			TestEqual(TEXT("Changed expression identity"), Change->GetStringField(TEXT("nodeId")), ExpressionNodeId(Expression));
			bool Found = false; for (const auto& V : Change->GetArrayField(TEXT("changedFields"))) Found |= V->AsString() == ExpectedField;
			TestTrue(FString::Printf(TEXT("Changed field %s captured"), ExpectedField), Found);
		}
		Release(Before.Data->GetStringField(TEXT("snapshotId"))); Release(After.Data->GetStringField(TEXT("snapshotId")));
	};
	CheckEdit(Scalar, TEXT("sliderMax"), [&] { Scalar->SliderMax += 10; });
	CheckEdit(Scalar, TEXT("group"), [&] { Scalar->Group = TEXT("Detail"); });
	CheckEdit(Scalar, TEXT("descriptionHash"), [&] { Scalar->Desc[299] = TCHAR('b'); });
	CheckEdit(Vector, TEXT("value"), [&] { Vector->DefaultValue = FLinearColor(0.125f, 0.25f, 0.5f, 0.75f); });
	CheckEdit(Switch, TEXT("value"), [&] { Switch->DefaultValue = !Switch->DefaultValue; });
	CheckEdit(Texture, TEXT("value"), [&] { Texture->Texture = NewObject<UTexture2D>(); });
	CheckEdit(Texture, TEXT("samplerType"), [&] { Texture->SamplerType = SAMPLERTYPE_LinearColor; });
	CheckEdit(Custom, TEXT("customOutputType"), [&] { Custom->OutputType = CMOT_Float4; });
	CheckEdit(Custom, TEXT("customConfigurationHash"), [&] { Custom->AdditionalDefines[0].DefineValue = TEXT("0.75"); });
	CheckEdit(Custom, TEXT("customConfigurationHash"), [&] { Custom->IncludeFilePaths[0] = TEXT("/Project/TestB.ush"); });
	CheckEdit(Custom, TEXT("customConfigurationHash"), [&] { Custom->AdditionalOutputs[0].OutputType = CMOT_Float2; });
	CheckEdit(Input, TEXT("previewValue"), [&] { Input->PreviewValue.X += 0.625f; });
	const auto Stable = Capture(F.Get());
	if (!TestTrue(TEXT("Capture for compact response"), Stable.bSuccess)) return false;
	auto Q = QueryParams(Stable.Data->GetStringField(TEXT("snapshotId"))); Q->SetStringField(TEXT("className"), TEXT("MaterialExpressionCustom"));
	const auto Page = ListNodes(Q);
	if (TestTrue(TEXT("Custom query succeeds"), Page.bSuccess) && TestEqual(TEXT("One Custom result"), Page.Data->GetArrayField(TEXT("nodes")).Num(), 1))
	{
		const auto Node = Page.Data->GetArrayField(TEXT("nodes"))[0]->AsObject();
		TestTrue(TEXT("Code and configuration have compact hashes"), Node->HasField(TEXT("codeHash")) && Node->HasField(TEXT("customConfigurationHash")));
		TestFalse(TEXT("No raw HLSL in graph response"), Node->HasField(TEXT("code")));
	}
	Scalar->DefaultValue = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("Non-finite value cannot produce misleading equality"), Capture(F.Get()).bSuccess); Scalar->DefaultValue = 0;
	Custom->Code = FString::ChrN(1024 * 1024 + 1, TCHAR('x'));
	TestFalse(TEXT("Oversize source fails capture rather than clipping"), Capture(F.Get()).bSuccess); Custom->Code = TEXT("return 0.25;");
	TestTrue(TEXT("Rejected captures do not destroy existing snapshot"), ListNodes(Q).bSuccess);
	Release(Stable.Data->GetStringField(TEXT("snapshotId")));
	TestEqual(TEXT("Query preserves dirty state"), F->GetOutermost()->IsDirty(), WasDirty);
	TestNull(TEXT("Property query creates no graph"), F->MaterialGraph);
	return true;
}

#endif
