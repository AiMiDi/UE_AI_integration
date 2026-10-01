#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
	void RegisterMaterialGraphQueryTools(FMCPToolRegistry& Registry);
	void RegisterMaterialMutationTools(FMCPToolRegistry& Registry);
}

namespace
{
	struct FExecutePlanFixture
	{
		FString PackageName;
		UPackage* Package = nullptr;
		UMaterial* Material = nullptr;
		UMaterialExpressionConstant* Source = nullptr;
		UMaterialExpressionAdd* Target = nullptr;
		UMaterialExpressionAdd* SharedConsumer = nullptr;

		FString AssetPath() const
		{
			return Material ? Material->GetPathName() : FString();
		}
	};

	template <typename T>
	T* AddExpression(UMaterial* Material, const FName Name)
	{
		T* Expression = NewObject<T>(Material, Name, RF_Transactional);
		if (Expression)
		{
			Material->GetExpressionCollection().AddExpression(Expression);
		}
		return Expression;
	}

	FExecutePlanFixture CreateFixture()
	{
		FExecutePlanFixture Fixture;
		Fixture.PackageName = TEXT("/Game/Automation/UEAI_ExecutePlan_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Fixture.Package = CreatePackage(*Fixture.PackageName);
		if (!Fixture.Package)
		{
			return Fixture;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(Fixture.PackageName);
		Fixture.Material = NewObject<UMaterial>(
			Fixture.Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Fixture.Material)
		{
			return Fixture;
		}
		FAssetRegistryModule::AssetCreated(Fixture.Material);

		Fixture.Source = AddExpression<UMaterialExpressionConstant>(
			Fixture.Material, TEXT("ExecutePlanSource"));
		Fixture.Target = AddExpression<UMaterialExpressionAdd>(
			Fixture.Material, TEXT("ExecutePlanTarget"));
		Fixture.SharedConsumer = AddExpression<UMaterialExpressionAdd>(
			Fixture.Material, TEXT("ExecutePlanSharedConsumer"));
		if (!Fixture.Source || !Fixture.Target || !Fixture.SharedConsumer)
		{
			return Fixture;
		}

		Fixture.Source->R = 0.25f;
		Fixture.Target->A.Connect(0, Fixture.Source);
		// Source is consumed by both the selected target and an external node.
		// The upstream boundary therefore has a concrete shared-node impact.
		Fixture.SharedConsumer->A.Connect(0, Fixture.Source);
		Fixture.Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(
			0,
			Fixture.Target);
		MCPMaterialInfrastructure::EnsureMaterialGraph(Fixture.Material);
		if (Fixture.Material->MaterialGraph)
		{
			Fixture.Material->MaterialGraph->RebuildGraph();
		}
		Fixture.Material->PostEditChange();
		Fixture.Package->SetDirtyFlag(true);
		return Fixture;
	}

	void CleanupFixture(const FExecutePlanFixture& Fixture)
	{
		if (!Fixture.Material)
		{
			return;
		}
		FAssetRegistryModule::AssetDeleted(Fixture.Material);
		Fixture.Material->ClearFlags(RF_Public | RF_Standalone);
		Fixture.Material->MarkAsGarbage();
		if (Fixture.Package)
		{
			Fixture.Package->SetDirtyFlag(false);
		}
	}

	TSharedRef<FJsonObject> MakePlanParams(
		const FExecutePlanFixture& Fixture,
		const FString& SnapshotId,
		const FString& ProjectionHash,
		const TArray<TSharedPtr<FJsonValue>>& Operations)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), Fixture.AssetPath());
		Params->SetStringField(TEXT("snapshotId"), SnapshotId);
		Params->SetStringField(TEXT("expectedProjectionHash"), ProjectionHash);
		Params->SetArrayField(TEXT("operations"), Operations);
		return Params;
	}

	TSharedRef<FJsonValue> MoveOperation(
		const FString& NodeId,
		const int32 X,
		const int32 Y)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("op"), TEXT("move_node"));
		Operation->SetStringField(TEXT("node_id"), NodeId);
		TArray<TSharedPtr<FJsonValue>> Position;
		Position.Add(MakeShared<FJsonValueNumber>(X));
		Position.Add(MakeShared<FJsonValueNumber>(Y));
		Operation->SetArrayField(TEXT("position"), Position);
		return MakeShared<FJsonValueObject>(Operation);
	}

	TSharedRef<FJsonValue> DisconnectOperation(
		const FString& SourceNodeId,
		const FString& TargetNodeId,
		const FString& TargetPinName)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("op"), TEXT("disconnect"));
		Operation->SetStringField(TEXT("sourceNodeId"), SourceNodeId);
		Operation->SetStringField(TEXT("targetNodeId"), TargetNodeId);
		Operation->SetStringField(TEXT("targetPinName"), TargetPinName);
		return MakeShared<FJsonValueObject>(Operation);
	}

	TSharedRef<FJsonObject> BoundaryParams(
		const FString& SnapshotId,
		const TArray<FString>& NodeIds)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("snapshotId"), SnapshotId);
		TArray<TSharedPtr<FJsonValue>> NodeValues;
		for (const FString& NodeId : NodeIds)
		{
			NodeValues.Add(MakeShared<FJsonValueString>(NodeId));
		}
		Params->SetArrayField(TEXT("nodeIds"), NodeValues);
		Params->SetStringField(TEXT("direction"), TEXT("upstream"));
		Params->SetNumberField(TEXT("depth"), 8);
		Params->SetNumberField(TEXT("maxNodes"), 16);
		Params->SetNumberField(TEXT("maxEdges"), 16);
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialGraphExecutePlanParityTest,
	"UE_AI_integration.MaterialGraphExecutePlan.RegistrationStaleRollbackAndMove",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphExecutePlanParityTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(Registry);
	UEAIIntegrationTools::RegisterMaterialMutationTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* ExecutePlanTool = Registry.FindTool(
		TEXT("content.material.graph.execute_plan"));
	if (!TestNotNull(TEXT("GraphIR execute_plan is registered"), ExecutePlanTool))
	{
		return false;
	}

	FExecutePlanFixture Fixture = CreateFixture();
	TArray<FString> SnapshotIds;
	ON_SCOPE_EXIT
	{
		for (const FString& SnapshotId : SnapshotIds)
		{
			Release(SnapshotId);
		}
		CleanupFixture(Fixture);
	};

	if (!TestNotNull(TEXT("Execute-plan material fixture exists"), Fixture.Material)
		|| !TestNotNull(TEXT("Execute-plan source expression exists"), Fixture.Source)
		|| !TestNotNull(TEXT("Execute-plan target expression exists"), Fixture.Target)
		|| !TestNotNull(TEXT("Execute-plan shared consumer exists"), Fixture.SharedConsumer))
	{
		return false;
	}

	const int32 OriginalX = Fixture.Target->MaterialExpressionEditorX;
	const int32 OriginalY = Fixture.Target->MaterialExpressionEditorY;
	const FString TargetNodeId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Target);
	const FMCPToolResult Captured = Capture(Fixture.Material);
	if (!TestTrue(TEXT("Execute-plan baseline snapshot succeeds"), Captured.bSuccess)
		|| !TestNotNull(TEXT("Execute-plan baseline snapshot has data"), Captured.Data.Get()))
	{
		return false;
	}
	const FString SnapshotId = Captured.Data->GetStringField(TEXT("snapshotId"));
	const FString ProjectionHash = Captured.Data->GetStringField(TEXT("projectionHash"));
	SnapshotIds.Add(SnapshotId);
	TestTrue(TEXT("Baseline snapshot has an immutable identity"), !SnapshotId.IsEmpty());
	TestTrue(TEXT("Baseline snapshot has a projection hash"), !ProjectionHash.IsEmpty());

	// Link mutations are destructive even when the selected target happens to
	// have one consumer today.  They must carry a fresh boundary proof so the
	// writer can detect shared-node impact and require explicit confirmation.
	const FMCPToolResult UnboundedDisconnect = ExecutePlanTool->Execute(
		MakePlanParams(
			Fixture,
			SnapshotId,
			ProjectionHash,
			{DisconnectOperation(
				MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Source),
				MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Target),
				TEXT("A"))}));
	TestFalse(TEXT("Disconnect without a writable boundary is rejected"),
		UnboundedDisconnect.bSuccess);
	TestEqual(TEXT("Unbounded link mutation has a stable protection code"),
		UnboundedDisconnect.ErrorCode,
		FString(TEXT("material_boundary_required_for_link_mutation")));
	TestTrue(TEXT("Rejected link mutation preserves the authored edge"),
		Fixture.Target->A.Expression == Fixture.Source);

	// A live authored edit after capture must invalidate the old read boundary
	// before execute_plan can perform any write.
	Fixture.Target->MaterialExpressionEditorX = OriginalX + 17;
	Fixture.Target->MaterialExpressionEditorY = OriginalY + 19;
	const FMCPToolResult Stale = ExecutePlanTool->Execute(MakePlanParams(
		Fixture,
		SnapshotId,
		ProjectionHash,
		{MoveOperation(TargetNodeId, OriginalX + 100, OriginalY + 100)}));
	TestFalse(TEXT("execute_plan rejects a stale projection hash"), Stale.bSuccess);
	TestTrue(TEXT("Stale rejection publishes an error code"), !Stale.ErrorCode.IsEmpty());
	TestEqual(
		TEXT("Stale rejection performs no write"),
		Fixture.Target->MaterialExpressionEditorX,
		OriginalX + 17);
	TestEqual(
		TEXT("Stale rejection preserves the second coordinate"),
		Fixture.Target->MaterialExpressionEditorY,
		OriginalY + 19);

	// Restore the fixture to the captured state, then force a postcondition
	// mismatch. The implementation must undo the applied move and report the
	// attempt as rolled back.
	Fixture.Target->MaterialExpressionEditorX = OriginalX;
	Fixture.Target->MaterialExpressionEditorY = OriginalY;
	TSharedRef<FJsonObject> RollbackParams = MakePlanParams(
		Fixture,
		SnapshotId,
		ProjectionHash,
		{MoveOperation(TargetNodeId, OriginalX + 200, OriginalY + 200)});
	RollbackParams->SetStringField(TEXT("expectedAfterProjectionHash"), ProjectionHash);
	const FMCPToolResult Rollback = ExecutePlanTool->Execute(RollbackParams);
	TestFalse(TEXT("Postcondition mismatch rejects the plan"), Rollback.bSuccess);
	TestEqual(TEXT("Rollback restores the original X coordinate"),
	          Fixture.Target->MaterialExpressionEditorX,
	          OriginalX);
	TestEqual(TEXT("Rollback restores the original Y coordinate"),
	          Fixture.Target->MaterialExpressionEditorY,
	          OriginalY);
	if (Rollback.Data.IsValid())
	{
		TestEqual(TEXT("Rollback receipt reports rolled_back attempt"),
		          Rollback.Data->GetStringField(TEXT("attempt_status")),
		          FString(TEXT("rolled_back")));
		TestEqual(TEXT("Rollback receipt reports restored state"),
		          Rollback.Data->GetStringField(TEXT("restore_status")),
		          FString(TEXT("restored")));
	}

	const FMCPToolResult Applied = ExecutePlanTool->Execute(MakePlanParams(
		Fixture,
		SnapshotId,
		ProjectionHash,
		{MoveOperation(TargetNodeId, OriginalX + 300, OriginalY + 300)}));
	if (!TestTrue(TEXT("execute_plan commits a valid move_node"), Applied.bSuccess)
		|| !TestNotNull(TEXT("Committed plan returns a receipt"), Applied.Data.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Committed move updates X"), Fixture.Target->MaterialExpressionEditorX, OriginalX + 300);
	TestEqual(TEXT("Committed move updates Y"), Fixture.Target->MaterialExpressionEditorY, OriginalY + 300);
	TestEqual(TEXT("Committed receipt reports committed attempt"),
	          Applied.Data->GetStringField(TEXT("attempt_status")),
	          FString(TEXT("committed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialGraphSharedBoundaryWriterContractTest,
	"UE_AI_integration.MaterialGraphExecutePlan.SharedBoundaryWriterContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphSharedBoundaryWriterContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(Registry);
	UEAIIntegrationTools::RegisterMaterialMutationTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* ExecutePlanTool = Registry.FindTool(
		TEXT("content.material.graph.execute_plan"));
	FMCPToolBase* BoundaryTool = Registry.FindTool(
		TEXT("content.material.graph.boundary.get"));
	if (!TestNotNull(TEXT("Shared boundary execute_plan is registered"), ExecutePlanTool)
		|| !TestNotNull(TEXT("Shared boundary query is registered"), BoundaryTool))
	{
		return false;
	}

	FExecutePlanFixture Fixture = CreateFixture();
	TArray<FString> SnapshotIds;
	ON_SCOPE_EXIT
	{
		for (const FString& SnapshotId : SnapshotIds)
		{
			Release(SnapshotId);
		}
		CleanupFixture(Fixture);
	};

	if (!TestNotNull(TEXT("Shared boundary material fixture exists"), Fixture.Material)
		|| !TestNotNull(TEXT("Shared boundary source exists"), Fixture.Source)
		|| !TestNotNull(TEXT("Shared boundary target exists"), Fixture.Target)
		|| !TestNotNull(TEXT("Shared boundary consumer exists"), Fixture.SharedConsumer))
	{
		return false;
	}

	const FMCPToolResult Captured = Capture(Fixture.Material);
	if (!TestTrue(TEXT("Shared boundary baseline capture succeeds"), Captured.bSuccess)
		|| !TestNotNull(TEXT("Shared boundary baseline has data"), Captured.Data.Get()))
	{
		return false;
	}
	const FString SnapshotId = Captured.Data->GetStringField(TEXT("snapshotId"));
	const FString ProjectionHash = Captured.Data->GetStringField(TEXT("projectionHash"));
	SnapshotIds.Add(SnapshotId);
	const FString SourceNodeId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Source);
	const FString TargetNodeId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Target);

	const FMCPToolResult Boundary = BoundaryTool->Execute(
		BoundaryParams(SnapshotId, {TargetNodeId}));
	if (!TestTrue(TEXT("Shared-node boundary capture succeeds"), Boundary.bSuccess)
		|| !TestNotNull(TEXT("Shared-node boundary returns data"), Boundary.Data.Get()))
	{
		return false;
	}
	TestTrue(TEXT("Boundary identifies the external shared consumer"),
		Boundary.Data->GetBoolField(TEXT("hasExternallyConsumedNodes")));
	TestTrue(TEXT("Boundary requires explicit shared-node confirmation"),
		Boundary.Data->GetBoolField(TEXT("requiresSharedNodeConfirmation")));
	const FString BoundaryId = Boundary.Data->GetStringField(TEXT("boundaryId"));
	TestTrue(TEXT("Boundary returns a stable writer identity"), !BoundaryId.IsEmpty());

	TSharedRef<FJsonObject> SharedBoundaryParams = MakePlanParams(
		Fixture,
		SnapshotId,
		ProjectionHash,
		{DisconnectOperation(SourceNodeId, TargetNodeId, TEXT("A"))});
	SharedBoundaryParams->SetStringField(TEXT("boundaryId"), BoundaryId);
	const FMCPToolResult UnconfirmedDisconnect = ExecutePlanTool->Execute(
		SharedBoundaryParams);
	TestFalse(TEXT("Unconfirmed shared-node disconnect is rejected"),
		UnconfirmedDisconnect.bSuccess);
	TestEqual(TEXT("Shared-node confirmation has a stable protection code"),
		UnconfirmedDisconnect.ErrorCode,
		FString(TEXT("material_boundary_shared_node_confirmation_required")));
	TestTrue(TEXT("Unconfirmed disconnect preserves the selected edge"),
		Fixture.Target->A.Expression == Fixture.Source);
	TestTrue(TEXT("Unconfirmed disconnect preserves the external edge"),
		Fixture.SharedConsumer->A.Expression == Fixture.Source);

	const FMCPToolResult UnboundedDisconnect = ExecutePlanTool->Execute(
		MakePlanParams(
			Fixture,
			SnapshotId,
			ProjectionHash,
			{DisconnectOperation(SourceNodeId, TargetNodeId, TEXT("A"))}));
	TestFalse(TEXT("Disconnect without a boundary is rejected"),
		UnboundedDisconnect.bSuccess);
	TestEqual(TEXT("Unbounded link mutation has a stable protection code"),
		UnboundedDisconnect.ErrorCode,
		FString(TEXT("material_boundary_required_for_link_mutation")));

	SharedBoundaryParams->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
	// Force the postcondition to disagree with the committed projection. The
	// shared-node restore path must reconnect the selected edge while preserving
	// the independent external consumer edge.
	SharedBoundaryParams->SetStringField(TEXT("expectedAfterProjectionHash"), ProjectionHash);
	const FMCPToolResult RolledBackDisconnect = ExecutePlanTool->Execute(
		SharedBoundaryParams);
	TestFalse(TEXT("Shared-node postcondition mismatch is rejected"),
		RolledBackDisconnect.bSuccess);
	if (RolledBackDisconnect.Data.IsValid())
	{
		TestEqual(TEXT("Shared-node rollback reports rolled_back attempt"),
			RolledBackDisconnect.Data->GetStringField(TEXT("attempt_status")),
			FString(TEXT("rolled_back")));
		TestEqual(TEXT("Shared-node rollback reports restored state"),
			RolledBackDisconnect.Data->GetStringField(TEXT("restore_status")),
			FString(TEXT("restored")));
	}
	TestTrue(TEXT("Shared-node rollback restores the selected edge"),
		Fixture.Target->A.Expression == Fixture.Source);
	TestTrue(TEXT("Shared-node rollback preserves the external edge"),
		Fixture.SharedConsumer->A.Expression == Fixture.Source);

	SharedBoundaryParams->RemoveField(TEXT("expectedAfterProjectionHash"));
	const FMCPToolResult ConfirmedDisconnect = ExecutePlanTool->Execute(
		SharedBoundaryParams);
	if (!ConfirmedDisconnect.bSuccess)
	{
		AddError(FString::Printf(
			TEXT("Confirmed shared-node disconnect failed: code=%s message=%s"),
			*ConfirmedDisconnect.ErrorCode,
			*ConfirmedDisconnect.ErrorMessage));
		if (ConfirmedDisconnect.Data.IsValid())
		{
			FString Details;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Details);
			FJsonSerializer::Serialize(ConfirmedDisconnect.Data.ToSharedRef(), Writer);
			AddInfo(Details);
		}
	}
	if (!TestTrue(TEXT("Confirmed shared-node disconnect commits"), ConfirmedDisconnect.bSuccess)
		|| !TestNotNull(TEXT("Confirmed disconnect returns a receipt"), ConfirmedDisconnect.Data.Get()))
	{
		return false;
	}
	TestTrue(TEXT("Confirmed disconnect removes only the selected edge"),
		Fixture.Target->A.Expression == nullptr);
	TestTrue(TEXT("Confirmed disconnect preserves the external consumer edge"),
		Fixture.SharedConsumer->A.Expression == Fixture.Source);
	TestTrue(TEXT("Confirmed disconnect verifies its postcondition"),
		ConfirmedDisconnect.Data->GetBoolField(TEXT("postconditionVerified")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialGraphSharedAtomicBatchContractTest,
	"UE_AI_integration.MaterialGraphExecutePlan.SharedAtomicBatchContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphSharedAtomicBatchContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(Registry);
	Registry.EndDomainRegistration();
	FMCPToolBase* ExecutePlanTool = Registry.FindTool(TEXT("content.material.graph.execute_plan"));
	FMCPToolBase* BoundaryTool = Registry.FindTool(TEXT("content.material.graph.boundary.get"));
	if (!TestNotNull(TEXT("Atomic batch writer is registered"), ExecutePlanTool)
		|| !TestNotNull(TEXT("Atomic batch boundary query is registered"), BoundaryTool)) return false;

	FExecutePlanFixture Fixture = CreateFixture();
	FString SnapshotId;
	ON_SCOPE_EXIT
	{
		if (!SnapshotId.IsEmpty()) Release(SnapshotId);
		CleanupFixture(Fixture);
	};
	if (!TestNotNull(TEXT("Atomic batch fixture exists"), Fixture.Material)
		|| !Fixture.Material->MaterialGraph || !Fixture.Source || !Fixture.Target || !Fixture.SharedConsumer)
	{
		return false;
	}

	// Source has two consumer expressions. Target itself feeds two root inputs.
	// Select every consumer to prove that native sharing, rather than only an
	// external-boundary edge, still requires the caller's explicit confirmation.
	Fixture.Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Fixture.Target);
	Fixture.Material->MaterialGraph->RebuildGraph();
	Fixture.Material->PostEditChange();
	Fixture.Package->SetDirtyFlag(false);
	const FMCPToolResult Captured = Capture(Fixture.Material);
	if (!TestTrue(TEXT("Atomic batch source snapshot succeeds"), Captured.bSuccess)
		|| !Captured.Data.IsValid()) return false;
	SnapshotId = Captured.Data->GetStringField(TEXT("snapshotId"));
	const FString ProjectionHash = Captured.Data->GetStringField(TEXT("projectionHash"));
	const FString SourceId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Source);
	const FString TargetId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Target);
	const FString ConsumerId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.SharedConsumer);
	const FMCPToolResult Boundary = BoundaryTool->Execute(
		BoundaryParams(SnapshotId, {SourceId, TargetId, ConsumerId, TEXT("root")}));
	if (!TestTrue(TEXT("Whole-selection boundary succeeds"), Boundary.bSuccess)
		|| !Boundary.Data.IsValid()) return false;
	TestFalse(TEXT("Whole selection has no external consumers"),
		Boundary.Data->GetBoolField(TEXT("hasExternallyConsumedNodes")));

	const TArray<TSharedPtr<FJsonValue>> Operations{
		DisconnectOperation(SourceId, TargetId, TEXT("A")),
		DisconnectOperation(SourceId, ConsumerId, TEXT("A"))};
	TSharedRef<FJsonObject> Params = MakePlanParams(Fixture, SnapshotId, ProjectionHash, Operations);
	Params->SetStringField(TEXT("boundaryId"), Boundary.Data->GetStringField(TEXT("boundaryId")));
	TSharedRef<FJsonObject> WorkflowContext = MakeShared<FJsonObject>();
	WorkflowContext->SetBoolField(TEXT("approvedPlan"), true);
	Params->SetObjectField(TEXT("__ueWorkflow"), WorkflowContext);
	const FMCPToolResult Unconfirmed = ExecutePlanTool->Execute(Params);
	TestFalse(TEXT("Approval cannot bypass selection-internal shared confirmation"), Unconfirmed.bSuccess);
	TestEqual(TEXT("Native fan-out rejection uses the shared protection code"), Unconfirmed.ErrorCode,
		FString(TEXT("material_boundary_shared_node_confirmation_required")));
	TestTrue(TEXT("Preflight rejection preserves both edges"),
		Fixture.Target->A.Expression == Fixture.Source && Fixture.SharedConsumer->A.Expression == Fixture.Source);
	TestFalse(TEXT("Preflight rejection leaves the package clean"), Fixture.Package->IsDirty());

	Params->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
	Params->SetStringField(TEXT("expectedAfterProjectionHash"), ProjectionHash);
	const FMCPToolResult RolledBack = ExecutePlanTool->Execute(Params);
	TestFalse(TEXT("Both actual writes are rolled back on a wrong final hash"), RolledBack.bSuccess);
	if (!TestNotNull(TEXT("Atomic rollback publishes its receipt"), RolledBack.Data.Get())) return false;
	const TArray<TSharedPtr<FJsonValue>>& RollbackOperations = RolledBack.Data->GetArrayField(TEXT("operationResults"));
	TestEqual(TEXT("Rollback occurs after both operation attempts"), RollbackOperations.Num(), 2);
	for (const TSharedPtr<FJsonValue>& Value : RollbackOperations)
	{
		TestTrue(TEXT("Each pre-rollback disconnect actually succeeds"), Value->AsObject()->GetBoolField(TEXT("success")));
	}
	TestTrue(TEXT("Batch rollback is verified"), RolledBack.Data->GetBoolField(TEXT("rollbackVerified")));
	TestTrue(TEXT("Batch rollback restores both authored edges"),
		Fixture.Target->A.Expression == Fixture.Source && Fixture.SharedConsumer->A.Expression == Fixture.Source);
	TestFalse(TEXT("Verified batch rollback restores the clean package"), Fixture.Package->IsDirty());

	Params->RemoveField(TEXT("expectedAfterProjectionHash"));
	const FMCPToolResult Applied = ExecutePlanTool->Execute(Params);
	if (!TestTrue(TEXT("Confirmed two-step batch commits with one fresh source boundary"), Applied.bSuccess)
		|| !TestNotNull(TEXT("Confirmed atomic batch publishes a receipt"), Applied.Data.Get())) return false;
	TestNull(TEXT("First operation removes the target edge"), Fixture.Target->A.Expression);
	TestNull(TEXT("Second operation removes the other edge from the same source"), Fixture.SharedConsumer->A.Expression);
	TestTrue(TEXT("Both root consumers of the edited target remain intact"),
		Fixture.Material->GetExpressionInputForProperty(MP_BaseColor)->Expression == Fixture.Target
		&& Fixture.Material->GetExpressionInputForProperty(MP_EmissiveColor)->Expression == Fixture.Target);
	TestEqual(TEXT("Committed batch reports both operations"), Applied.Data->GetArrayField(TEXT("operationResults")).Num(), 2);
	TestTrue(TEXT("Committed batch verifies the shared boundary"), Applied.Data->GetBoolField(TEXT("writeBoundaryVerified")));
	TestTrue(TEXT("Committed batch reports native sharing"), Applied.Data->GetBoolField(TEXT("sharedNodeImpactDetected")));
	TestTrue(TEXT("Committed batch reports explicit shared confirmation"), Applied.Data->GetBoolField(TEXT("sharedNodeImpactConfirmed")));
	TestTrue(TEXT("Committed batch verifies its final projection"), Applied.Data->GetBoolField(TEXT("postconditionVerified")));
	TestFalse(TEXT("Atomic graph batch remains unsaved"), Applied.Data->GetBoolField(TEXT("saved")));
	return true;
}

#endif
