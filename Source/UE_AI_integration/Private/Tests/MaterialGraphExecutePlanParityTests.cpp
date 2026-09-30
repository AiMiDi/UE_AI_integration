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
		if (!Fixture.Source || !Fixture.Target)
		{
			return Fixture;
		}

		Fixture.Source->R = 0.25f;
		Fixture.Target->A.Connect(0, Fixture.Source);
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
		|| !TestNotNull(TEXT("Execute-plan target expression exists"), Fixture.Target))
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

#endif
