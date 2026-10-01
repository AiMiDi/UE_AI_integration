#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/Sha256.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterMaterialMutationTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FMaterialRestoreFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UMaterial* Material = nullptr;
	UMaterialExpressionConstant* SourceA = nullptr;
	UMaterialExpressionConstant* SourceB = nullptr;
	UMaterialExpressionAdd* Sum = nullptr;
	UMaterialExpressionAdd* SideConsumer = nullptr;
};

template <typename T>
T* AddRestoreExpression(UMaterial* Material, const FName Name)
{
	T* Expression = NewObject<T>(
		Material, Name, RF_Transactional);
	Material->GetExpressionCollection().AddExpression(Expression);
	return Expression;
}

FMaterialRestoreFixture CreateMaterialRestoreFixture(const FString& Prefix)
{
	FMaterialRestoreFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/") + Prefix + TEXT("_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(Fixture.PackageName);
	Fixture.Package = CreatePackage(*Fixture.PackageName);
	Fixture.Material = Fixture.Package
		? NewObject<UMaterial>(
			Fixture.Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!Fixture.Material)
	{
		return Fixture;
	}
	FAssetRegistryModule::AssetCreated(Fixture.Material);
	Fixture.SourceA = AddRestoreExpression<UMaterialExpressionConstant>(
		Fixture.Material, TEXT("SourceA"));
	Fixture.SourceB = AddRestoreExpression<UMaterialExpressionConstant>(
		Fixture.Material, TEXT("SourceB"));
	Fixture.Sum = AddRestoreExpression<UMaterialExpressionAdd>(
		Fixture.Material, TEXT("Sum"));
	Fixture.SideConsumer = AddRestoreExpression<UMaterialExpressionAdd>(
		Fixture.Material, TEXT("SideConsumer"));
	Fixture.SourceA->R = 0.25f;
	Fixture.SourceB->R = 0.75f;
	Fixture.Sum->A.Connect(0, Fixture.SourceA);
	Fixture.Sum->B.Connect(0, Fixture.SourceB);
	Fixture.Material->GetExpressionInputForProperty(
		MP_EmissiveColor)->Connect(0, Fixture.Sum);
	MCPMaterialInfrastructure::EnsureMaterialGraph(Fixture.Material);
	if (Fixture.Material->MaterialGraph)
	{
		Fixture.Material->MaterialGraph->RebuildGraph();
	}
	Fixture.Material->PostEditChange();
	Fixture.Package->MarkPackageDirty();
	return Fixture;
}

bool SaveMaterialRestoreFixture(
	FMaterialRestoreFixture& Fixture,
	FString& OutError)
{
	if (!Fixture.Material || !Fixture.Material->MaterialGraph)
	{
		OutError = TEXT("Material fixture or graph is unavailable.");
		return false;
	}
	if (!UEditorAssetLibrary::SaveLoadedAsset(Fixture.Material, false))
	{
		OutError = TEXT("Material fixture could not be saved.");
		return false;
	}
	return true;
}

bool DeleteMaterialRestoreFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

bool HashMaterialRestorePackage(
	const FString& PackageName,
	FString& OutHash)
{
	TArray<uint8> Bytes;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		PackageName, FPackageName::GetAssetPackageExtension());
	return FFileHelper::LoadFileToArray(Bytes, *Filename)
		&& UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, OutHash);
}

TSharedRef<FJsonObject> MaterialSnapshotParams(const FString& Material)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("material"), Material);
	return Params;
}

TSharedRef<FJsonObject> MaterialDiffParams(
	const FString& Material,
	const FString& SnapshotId)
{
	auto Params = MaterialSnapshotParams(Material);
	Params->SetStringField(TEXT("snapshotId"), SnapshotId);
	return Params;
}

TSharedRef<FJsonObject> MaterialRestoreParams(
	const FString& Material,
	const FString& SnapshotId,
	const FString& ExpectedCurrentDigest,
	const bool bDryRun,
	const TOptional<bool>& Save = TOptional<bool>(),
	const TOptional<FString>& RestoreMode = TOptional<FString>(),
	const bool bConfirmFullGraphRestore = false)
{
	auto Params = MaterialDiffParams(Material, SnapshotId);
	Params->SetStringField(
		TEXT("expectedCurrentDigest"), ExpectedCurrentDigest);
	Params->SetBoolField(TEXT("dryRun"), bDryRun);
	if (Save.IsSet())
	{
		Params->SetBoolField(TEXT("save"), Save.GetValue());
	}
	if (RestoreMode.IsSet())
	{
		Params->SetStringField(TEXT("restoreMode"), RestoreMode.GetValue());
	}
	if (bConfirmFullGraphRestore)
	{
		Params->SetBoolField(TEXT("confirmFullGraphRestore"), true);
	}
	return Params;
}

bool ReadCurrentRestoreDigest(
	FAutomationTestBase& Test,
	FMCPToolRegistry& Registry,
	const FString& Material,
	const FString& SnapshotId,
	FString& OutDigest)
{
	const FMCPToolResult Diff = Registry.ExecuteTool(
		TEXT("content.material.graph.diff"),
		MaterialDiffParams(Material, SnapshotId));
	if (!Test.TestTrue(TEXT("Material graph diff succeeds"), Diff.bSuccess)
		|| !Test.TestNotNull(TEXT("Material graph diff data"), Diff.Data.Get()))
	{
		if (!Diff.ErrorMessage.IsEmpty())
		{
			Test.AddError(Diff.ErrorMessage);
		}
		return false;
	}
	OutDigest = Diff.Data->GetStringField(TEXT("currentStateDigest"));
	return !OutDigest.IsEmpty();
}

const FPinConnectionRecord* FindRestoreConnection(
	const FGraphSnapshotData& Data,
	const FString& SourceNodeGuid,
	const FString& TargetNodeGuid,
	const FString& TargetPinName)
{
	for (const FPinConnectionRecord& Connection : Data.Connections)
	{
		if (Connection.SourceNodeGuid == SourceNodeGuid
			&& Connection.TargetNodeGuid == TargetNodeGuid
			&& Connection.TargetPinName == TargetPinName)
		{
			return &Connection;
		}
	}
	return nullptr;
}

FString AddSnapshotClone(
	const FString& SourceSnapshotId,
	const FString& Suffix,
	TFunctionRef<void(FGraphSnapshotData&)> Mutate)
{
	TMap<FString, FGraphSnapshot>& Snapshots =
		MCPHelpers::GetMaterialSnapshots();
	const FGraphSnapshot* Source = Snapshots.Find(SourceSnapshotId);
	if (!Source)
	{
		return FString();
	}
	FGraphSnapshot Clone = *Source;
	Clone.SnapshotId = SourceSnapshotId + TEXT("-") + Suffix;
	FGraphSnapshotData* Data = Clone.Graphs.Find(TEXT("MaterialGraph"));
	if (!Data)
	{
		return FString();
	}
	Mutate(*Data);
	Snapshots.Add(Clone.SnapshotId, MoveTemp(Clone));
	return SourceSnapshotId + TEXT("-") + Suffix;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialGraphRestoreContractTest,
	"UE_AI_integration.MaterialGraphMutation.RestoreContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphRestoreContractTest::RunTest(const FString&)
{
	FMaterialRestoreFixture Primary =
		CreateMaterialRestoreFixture(TEXT("UEAI_MaterialRestore"));
	FMaterialRestoreFixture Other =
		CreateMaterialRestoreFixture(TEXT("UEAI_MaterialRestoreOther"));
	TArray<FString> SnapshotIds;
	ON_SCOPE_EXIT
	{
		for (const FString& SnapshotId : SnapshotIds)
		{
			MCPHelpers::GetMaterialSnapshots().Remove(SnapshotId);
		}
		TestTrue(TEXT("Primary restore fixture and package are deleted"),
			DeleteMaterialRestoreFixture(Primary.PackageName));
		TestTrue(TEXT("Other restore fixture and package are deleted"),
			DeleteMaterialRestoreFixture(Other.PackageName));
	};
	if (!TestNotNull(TEXT("Primary material fixture"), Primary.Material)
		|| !TestNotNull(TEXT("Primary material graph"),
			Primary.Material ? Primary.Material->MaterialGraph.Get() : nullptr)
		|| !TestNotNull(TEXT("Other material fixture"), Other.Material))
	{
		return false;
	}
	FString SaveError;
	if (!TestTrue(TEXT("Primary material baseline saves"),
		SaveMaterialRestoreFixture(Primary, SaveError))
		|| !TestTrue(TEXT("Other material baseline saves"),
			SaveMaterialRestoreFixture(Other, SaveError)))
	{
		AddError(SaveError);
		return false;
	}
	FString BaselineFileHash;
	if (!TestTrue(TEXT("Primary package baseline hash is available"),
		HashMaterialRestorePackage(Primary.PackageName, BaselineFileHash)))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialMutationTools(Registry);
	Registry.EndDomainRegistration();
	const FMCPToolResult Snapshot = Registry.ExecuteTool(
		TEXT("content.material.graph.snapshot"),
		MaterialSnapshotParams(Primary.PackageName));
	if (!TestTrue(TEXT("Restore baseline snapshot succeeds"), Snapshot.bSuccess)
		|| !TestNotNull(TEXT("Restore baseline snapshot data"),
			Snapshot.Data.Get()))
	{
		return false;
	}
	const FString SnapshotId =
		Snapshot.Data->GetStringField(TEXT("snapshotId"));
	const FString SnapshotDigest =
		Snapshot.Data->GetStringField(TEXT("stateDigest"));
	SnapshotIds.Add(SnapshotId);
	TestEqual(TEXT("Snapshot declares additive-only restore semantics"),
		Snapshot.Data->GetStringField(TEXT("restoreSemantics")),
		FString(TEXT("missingConnectionsOnly")));
	TestFalse(TEXT("Snapshot does not claim full graph restoration"),
		Snapshot.Data->GetBoolField(TEXT("fullGraphRestore")));

	FGraphSnapshot* StoredSnapshot =
		MCPHelpers::GetMaterialSnapshots().Find(SnapshotId);
	FGraphSnapshotData* StoredData = StoredSnapshot
		? StoredSnapshot->Graphs.Find(TEXT("MaterialGraph"))
		: nullptr;
	if (!TestNotNull(TEXT("Stored restore snapshot"), StoredSnapshot)
		|| !TestNotNull(TEXT("Stored restore graph projection"), StoredData)
		|| !TestNotNull(TEXT("Source A graph node"),
			Primary.SourceA->GraphNode.Get())
		|| !TestNotNull(TEXT("Source B graph node"),
			Primary.SourceB->GraphNode.Get())
		|| !TestNotNull(TEXT("Sum graph node"), Primary.Sum->GraphNode.Get()))
	{
		return false;
	}
	const FString SourceAId = MCPMaterialInfrastructure::ExpressionNodeId(Primary.SourceA);
	const FString SourceBId = MCPMaterialInfrastructure::ExpressionNodeId(Primary.SourceB);
	const FString SumId = MCPMaterialInfrastructure::ExpressionNodeId(Primary.Sum);
	const FPinConnectionRecord* SourceAToSumAPtr = FindRestoreConnection(
		*StoredData, SourceAId, SumId, TEXT("A"));
	const FPinConnectionRecord* SourceBToSumBPtr = FindRestoreConnection(
		*StoredData, SourceBId, SumId, TEXT("B"));
	if (!TestNotNull(TEXT("Snapshot contains Source A to Sum A"),
		SourceAToSumAPtr)
		|| !TestNotNull(TEXT("Snapshot contains Source B to Sum B"),
			SourceBToSumBPtr))
	{
		return false;
	}
	// Snapshot clones are inserted into the same TMap and may relocate stored
	// values.  Keep record copies rather than pointers into a map-owned array.
	const FPinConnectionRecord SourceAToSumA = *SourceAToSumAPtr;
	const FPinConnectionRecord SourceBToSumB = *SourceBToSumBPtr;

	// Establish a current graph that is missing one snapshot edge but contains
	// a new unrelated edge.  Restore must add the former without deleting the
	// latter, and save=false must leave the baseline package bytes unchanged.
	Primary.Sum->A.Expression = nullptr;
	Primary.SideConsumer->A.Connect(0, Primary.SourceA);
	Primary.Material->MaterialGraph->RebuildGraph();
	Primary.Package->SetDirtyFlag(false);
	FString CurrentDigest;
	if (!ReadCurrentRestoreDigest(
		*this, Registry, Primary.PackageName, SnapshotId, CurrentDigest))
	{
		return false;
	}
	TestNotEqual(TEXT("Current graph differs from the snapshot"),
		CurrentDigest, SnapshotDigest);

	const bool bDirtyBeforeCrossAsset = Other.Package->IsDirty();
	const FMCPToolResult CrossAsset = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Other.PackageName, SnapshotId, CurrentDigest, false));
	TestFalse(TEXT("Snapshot cannot be restored into another material"),
		CrossAsset.bSuccess);
	TestEqual(TEXT("Cross-asset restore has a stable conflict code"),
		CrossAsset.ErrorCode, FString(TEXT("snapshot_asset_mismatch")));
	TestEqual(TEXT("Cross-asset rejection preserves dirty state"),
		Other.Package->IsDirty(), bDirtyBeforeCrossAsset);

	const bool bDirtyBeforeStale = Primary.Package->IsDirty();
	const FMCPToolResult Stale = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName, SnapshotId, SnapshotDigest, false));
	TestFalse(TEXT("Stale current-state digest is rejected"), Stale.bSuccess);
	TestEqual(TEXT("Stale digest has a stable conflict code"),
		Stale.ErrorCode, FString(TEXT("material_graph_state_conflict")));
	FString DigestAfterStale;
	TestTrue(TEXT("Stale rejection leaves graph digest unchanged"),
		ReadCurrentRestoreDigest(
			*this,
			Registry,
			Primary.PackageName,
			SnapshotId,
			DigestAfterStale)
		&& DigestAfterStale == CurrentDigest);
	TestEqual(TEXT("Stale rejection preserves dirty state"),
		Primary.Package->IsDirty(), bDirtyBeforeStale);

	const FMCPToolResult DryRun = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName, SnapshotId, CurrentDigest, true, false));
	if (TestTrue(TEXT("Restore dry-run succeeds"), DryRun.bSuccess)
		&& DryRun.Data)
	{
		TestTrue(TEXT("Dry-run is identified"),
			DryRun.Data->GetBoolField(TEXT("dryRun")));
		TestEqual(TEXT("Dry-run finds one missing connection"),
			DryRun.Data->GetIntegerField(TEXT("missingConnectionCount")), 1);
		TestEqual(TEXT("Dry-run performs zero graph writes"),
			DryRun.Data->GetIntegerField(TEXT("appliedConnectionCount")), 0);
		TestFalse(TEXT("Dry-run does not claim a postcondition check"),
			DryRun.Data->GetBoolField(TEXT("postconditionChecked")));
	}
	FString DigestAfterDryRun;
	TestTrue(TEXT("Dry-run leaves graph digest unchanged"),
		ReadCurrentRestoreDigest(
			*this,
			Registry,
			Primary.PackageName,
			SnapshotId,
			DigestAfterDryRun)
		&& DigestAfterDryRun == CurrentDigest);

	const FMCPToolResult SaveRejected = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName, SnapshotId, CurrentDigest, false, true));
	TestFalse(TEXT("Direct restore cannot opt into persistence"),
		SaveRejected.bSuccess);
	TestEqual(TEXT("Unsupported persistence has a stable conflict code"),
		SaveRejected.ErrorCode, FString(TEXT("restore_save_not_supported")));
	FString DigestAfterSaveReject;
	TestTrue(TEXT("Rejected save request performs zero graph writes"),
		ReadCurrentRestoreDigest(
			*this,
			Registry,
			Primary.PackageName,
			SnapshotId,
			DigestAfterSaveReject)
		&& DigestAfterSaveReject == CurrentDigest);

	const FString MissingSnapshotId = AddSnapshotClone(
		SnapshotId,
		TEXT("missing-node"),
		[SourceAToSumA](FGraphSnapshotData& Data)
		{
			FPinConnectionRecord Missing = SourceAToSumA;
			Missing.SourceNodeGuid = FGuid::NewGuid().ToString();
			Data.Connections.Add(MoveTemp(Missing));
		});
	SnapshotIds.Add(MissingSnapshotId);
	if (!TestFalse(TEXT("Missing-node snapshot clone is created"),
		MissingSnapshotId.IsEmpty()))
	{
		return false;
	}
	const FMCPToolResult MissingNode = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName,
			MissingSnapshotId,
			CurrentDigest,
			false));
	TestFalse(TEXT("Missing-node preflight rejects the entire plan"),
		MissingNode.bSuccess);
	TestEqual(TEXT("Missing-node preflight has a stable error code"),
		MissingNode.ErrorCode, FString(TEXT("restore_preflight_failed")));
	FString DigestAfterMissingNode;
	TestTrue(TEXT("Failed preflight performs zero graph writes"),
		ReadCurrentRestoreDigest(
			*this,
			Registry,
			Primary.PackageName,
			SnapshotId,
			DigestAfterMissingNode)
		&& DigestAfterMissingNode == CurrentDigest);

	// An occupied single-input target is not an additive change.  The existing
	// user connection must remain intact after the preflight rejection.
	Primary.Sum->A.Connect(0, Primary.SourceB);
	Primary.Material->MaterialGraph->RebuildGraph();
	Primary.Package->SetDirtyFlag(false);
	FString OccupiedDigest;
	if (!ReadCurrentRestoreDigest(
		*this, Registry, Primary.PackageName, SnapshotId, OccupiedDigest))
	{
		return false;
	}
	const FMCPToolResult Occupied = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName, SnapshotId, OccupiedDigest, false));
	TestFalse(TEXT("Restore refuses to break an occupied target"),
		Occupied.bSuccess);
	TestEqual(TEXT("Occupied target has a stable conflict code"),
		Occupied.ErrorCode,
		FString(TEXT("restore_would_break_existing_connection")));
	TestTrue(TEXT("Occupied target connection is preserved"),
		Primary.Sum->A.Expression == Primary.SourceB);
	Primary.Sum->A.Expression = nullptr;
	Primary.Material->MaterialGraph->RebuildGraph();
	Primary.Package->SetDirtyFlag(false);
	if (!ReadCurrentRestoreDigest(
		*this, Registry, Primary.PackageName, SnapshotId, CurrentDigest))
	{
		return false;
	}

	// Add a second snapshot edge targeting Sum.A.  Both pass preflight while the
	// input is empty, but they cannot coexist.  The apply/postcondition failure
	// must remove every connection owned by this attempt and restore dirty state.
	const FString RollbackSnapshotId = AddSnapshotClone(
		SnapshotId,
		TEXT("rollback"),
		[SourceAToSumA, SourceBToSumB](FGraphSnapshotData& Data)
		{
			FPinConnectionRecord Competing = SourceAToSumA;
			Competing.SourceNodeGuid = SourceBToSumB.SourceNodeGuid;
			Competing.SourcePinName = SourceBToSumB.SourcePinName;
			Data.Connections.Add(MoveTemp(Competing));
		});
	SnapshotIds.Add(RollbackSnapshotId);
	if (!TestFalse(TEXT("Rollback snapshot clone is created"),
		RollbackSnapshotId.IsEmpty()))
	{
		return false;
	}
	const FMCPToolResult RolledBack = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName,
			RollbackSnapshotId,
			CurrentDigest,
			false));
	TestFalse(TEXT("Competing restore plan fails"), RolledBack.bSuccess);
	TestTrue(TEXT("Competing restore failure is not a rollback failure"),
		RolledBack.ErrorCode == TEXT("restore_apply_failed")
		|| RolledBack.ErrorCode == TEXT("restore_postcondition_failed"));
	TestTrue(TEXT("Failure reports semantic rollback verification"),
		RolledBack.ErrorMessage.Contains(TEXT("semanticRollbackVerified=true")));
	TestTrue(TEXT("Failure reports dirty restoration"),
		RolledBack.ErrorMessage.Contains(TEXT("dirtyRestored=true")));
	FString DigestAfterRollback;
	TestTrue(TEXT("Failed apply restores the exact pre-attempt digest"),
		ReadCurrentRestoreDigest(
			*this,
			Registry,
			Primary.PackageName,
			SnapshotId,
			DigestAfterRollback)
		&& DigestAfterRollback == CurrentDigest);
	TestFalse(TEXT("Failed apply restores the clean dirty state"),
		Primary.Package->IsDirty());
	TestNull(TEXT("Failed apply leaves the snapshot edge absent"),
		Primary.Sum->A.Expression);
	TestTrue(TEXT("Failed apply preserves the unrelated current edge"),
		Primary.SideConsumer->A.Expression == Primary.SourceA);

	const FMCPToolResult Applied = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName, SnapshotId, CurrentDigest, false, false));
	if (!TestTrue(TEXT("Additive-only restore succeeds"), Applied.bSuccess)
		|| !TestNotNull(TEXT("Additive restore data"), Applied.Data.Get()))
	{
		if (!Applied.ErrorMessage.IsEmpty())
		{
			AddError(Applied.ErrorMessage);
		}
		return false;
	}
	TestEqual(TEXT("Exactly one missing connection is applied"),
		Applied.Data->GetIntegerField(TEXT("appliedConnectionCount")), 1);
	TestTrue(TEXT("Restore postcondition is verified"),
		Applied.Data->GetBoolField(TEXT("postconditionVerified")));
	TestFalse(TEXT("Restore remains explicitly partial graph semantics"),
		Applied.Data->GetBoolField(TEXT("fullGraphRestore")));
	TestEqual(TEXT("Restore remains dirty-only"),
		Applied.Data->GetStringField(TEXT("persistence")),
		FString(TEXT("dirtyOnly")));
	TestFalse(TEXT("Restore does not claim disk persistence"),
		Applied.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Restore does not claim compile verification"),
		Applied.Data->GetBoolField(TEXT("compileVerified")));
	TestTrue(TEXT("Snapshot connection is restored in the authored model"),
		Primary.Sum->A.Expression == Primary.SourceA);
	TestTrue(TEXT("Current extra connection survives additive restore"),
		Primary.SideConsumer->A.Expression == Primary.SourceA);
	TestTrue(TEXT("Successful dirty-only restore marks the package dirty"),
		Primary.Package->IsDirty());
	FString AfterFileHash;
	TestTrue(TEXT("Dirty-only restore preserves package bytes"),
		HashMaterialRestorePackage(Primary.PackageName, AfterFileHash)
		&& AfterFileHash == BaselineFileHash);

	const FMCPToolResult AfterDiff = Registry.ExecuteTool(
		TEXT("content.material.graph.diff"),
		MaterialDiffParams(Primary.PackageName, SnapshotId));
	if (TestTrue(TEXT("Post-restore diff succeeds"), AfterDiff.bSuccess)
		&& AfterDiff.Data)
	{
		const TSharedPtr<FJsonObject> Summary =
			AfterDiff.Data->GetObjectField(TEXT("summary"));
		TestEqual(TEXT("No snapshot connection remains severed"),
			Summary->GetIntegerField(TEXT("severedConnections")), 0);
		TestEqual(TEXT("The unrelated current edge remains new"),
			Summary->GetIntegerField(TEXT("newConnections")), 1);
		TestEqual(TEXT("Applied digest matches the verified diff"),
			Applied.Data->GetStringField(TEXT("afterStateDigest")),
			AfterDiff.Data->GetStringField(TEXT("currentStateDigest")));
	}

	const FString AppliedDigest =
		Applied.Data->GetStringField(TEXT("afterStateDigest"));
	const FMCPToolResult Replay = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName, SnapshotId, AppliedDigest, false));
	if (TestTrue(TEXT("Restore replay is idempotent"), Replay.bSuccess)
		&& Replay.Data)
	{
		TestEqual(TEXT("Restore replay performs zero writes"),
			Replay.Data->GetIntegerField(TEXT("appliedConnectionCount")), 0);
		TestTrue(TEXT("Restore replay verifies its postcondition"),
			Replay.Data->GetBoolField(TEXT("postconditionVerified")));
		TestFalse(TEXT("Restore replay does not request compilation"),
			Replay.Data->GetBoolField(TEXT("compileRequested")));
	}

	// Explicit full-graph mode is the opt-in escape from additive semantics. It
	// must remove the unrelated side edge, preserve every snapshot edge, and
	// publish the stronger restore boundary in its read-back result.
	const FString FullGraphDigest = AfterDiff.Data
		? AfterDiff.Data->GetStringField(TEXT("currentStateDigest"))
		: Applied.Data->GetStringField(TEXT("afterStateDigest"));
	const FMCPToolResult FullGraph = Registry.ExecuteTool(
		TEXT("content.material.graph.restore"),
		MaterialRestoreParams(
			Primary.PackageName,
			SnapshotId,
			FullGraphDigest,
			false,
			TOptional<bool>(),
			TOptional<FString>(FString(TEXT("fullGraph"))),
			true));
	if (TestTrue(TEXT("Explicit full-graph restore succeeds"), FullGraph.bSuccess)
		&& FullGraph.Data)
	{
		TestTrue(TEXT("Full-graph result identifies full semantics"),
			FullGraph.Data->GetBoolField(TEXT("fullGraphRestore")));
		TestEqual(TEXT("Full-graph result reports its restore mode"),
			FullGraph.Data->GetStringField(TEXT("restoreSemantics")),
			FString(TEXT("fullGraph")));
		TestTrue(TEXT("Full-graph postcondition is verified"),
			FullGraph.Data->GetBoolField(TEXT("postconditionVerified")));
		TestTrue(TEXT("Full-graph removes the unrelated current edge"),
			Primary.SideConsumer->A.Expression == nullptr);
		TestTrue(TEXT("Full-graph preserves the snapshot edge"),
			Primary.Sum->A.Expression == Primary.SourceA);
	}
	return true;
}

#endif
