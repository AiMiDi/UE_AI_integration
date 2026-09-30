#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegrationTools
{
	void RegisterMaterialGraphQueryTools(FMCPToolRegistry& Registry);
	void RegisterMaterialMutationTools(FMCPToolRegistry& Registry);
}

namespace
{
	struct FMaterialFunctionContractFixture
	{
		FString PackageName;
		UPackage* Package = nullptr;
		UMaterialFunction* Function = nullptr;
		UMaterialExpressionScalarParameter* Parameter = nullptr;

		FString AssetPath() const
		{
			return Function ? Function->GetPathName() : FString();
		}
	};

	FMaterialFunctionContractFixture CreateFixture(const TCHAR* Name)
	{
		FMaterialFunctionContractFixture Fixture;
		Fixture.PackageName = TEXT("/Game/Automation/UEAI_MaterialMetadata_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Fixture.Package = CreatePackage(*Fixture.PackageName);
		if (!Fixture.Package)
		{
			return Fixture;
		}

		Fixture.Function = NewObject<UMaterialFunction>(
			Fixture.Package,
			Name,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!Fixture.Function)
		{
			return Fixture;
		}
		FAssetRegistryModule::AssetCreated(Fixture.Function);

		Fixture.Parameter = NewObject<UMaterialExpressionScalarParameter>(
			Fixture.Function,
			TEXT("ContractParameter"),
			RF_Transactional);
		if (!Fixture.Parameter)
		{
			return Fixture;
		}
		Fixture.Parameter->Function = Fixture.Function;
		Fixture.Parameter->ParameterName = TEXT("ContractValue");
		Fixture.Parameter->DefaultValue = 0.25f;
		Fixture.Parameter->Group = TEXT("Base");
		Fixture.Parameter->SortPriority = 3;
		Fixture.Function->GetExpressionCollection().AddExpression(Fixture.Parameter);

		// ExecutePlan requires the same editor graph projection that a live
		// Material Editor would attach to a function asset. Construct only the
		// transient graph used by this contract; no package is saved.
		Fixture.Function->MaterialGraph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(
				Fixture.Function,
				NAME_None,
				UMaterialGraph::StaticClass(),
				UMaterialGraphSchema::StaticClass()));
		Fixture.Function->MaterialGraph->MaterialFunction = Fixture.Function;
		Fixture.Function->MaterialGraph->RebuildGraph();
		Fixture.Package->SetDirtyFlag(false);
		return Fixture;
	}

	void CleanupFixture(const FMaterialFunctionContractFixture& Fixture)
	{
		if (!Fixture.Function)
		{
			return;
		}
		FAssetRegistryModule::AssetDeleted(Fixture.Function);
		Fixture.Function->ClearFlags(RF_Public | RF_Standalone);
		Fixture.Function->MarkAsGarbage();
		if (Fixture.Package)
		{
			Fixture.Package->SetDirtyFlag(false);
		}
	}

	TSharedRef<FJsonObject> DiffParams(
		const FString& BeforeSnapshotId,
		const FString& AfterSnapshotId)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("beforeSnapshotId"), BeforeSnapshotId);
		Params->SetStringField(TEXT("afterSnapshotId"), AfterSnapshotId);
		Params->SetNumberField(TEXT("limit"), 200);
		return Params;
	}

	bool ContainsString(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		const FString& Expected)
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			if (Value.IsValid() && Value->Type == EJson::String && Value->AsString() == Expected)
			{
				return true;
			}
		}
		return false;
	}

	bool HasResolutionState(
		const TSharedPtr<FJsonObject>& Data,
		const FString& Expected)
	{
		if (!Data.IsValid())
		{
			return false;
		}
		for (const TCHAR* Field : {TEXT("resolutionState"), TEXT("sourceState"), TEXT("state")})
		{
			FString State;
			if (Data->TryGetStringField(Field, State) && State == Expected)
			{
				return true;
			}
		}
		return false;
	}

	TSharedRef<FJsonObject> MetadataOperation(const FString& Description)
	{
		TSharedRef<FJsonObject> Operation = MakeShared<FJsonObject>();
		Operation->SetStringField(TEXT("op"), TEXT("set_function_metadata"));
		Operation->SetStringField(TEXT("description"), Description);
		return Operation;
	}

	TSharedRef<FJsonObject> ExecutePlanParams(
		const FMaterialFunctionContractFixture& Fixture,
		const FString& SnapshotId,
		const FString& ProjectionHash,
		const FString& ExpectedAfterHash)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), Fixture.AssetPath());
		Params->SetStringField(TEXT("snapshotId"), SnapshotId);
		Params->SetStringField(TEXT("expectedProjectionHash"), ProjectionHash);
		Params->SetStringField(TEXT("expectedAfterProjectionHash"), ExpectedAfterHash);
		Params->SetArrayField(
			TEXT("operations"),
			{MakeShared<FJsonValueObject>(MetadataOperation(TEXT("changed by red contract")))});
		return Params;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialMetadataProjectionRedContractTest,
	"UE_AI_integration.MaterialGraphQuery.RedContract.FunctionMetadataProjectionAndDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialMetadataProjectionRedContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;

	FMaterialFunctionContractFixture Fixture = CreateFixture(TEXT("MetadataProjectionFunction"));
	TArray<FString> SnapshotIds;
	ON_SCOPE_EXIT
	{
		for (const FString& SnapshotId : SnapshotIds)
		{
			Release(SnapshotId);
		}
		CleanupFixture(Fixture);
	};

	if (!TestNotNull(TEXT("Function fixture exists"), Fixture.Function)
		|| !TestNotNull(TEXT("Parameter fixture exists"), Fixture.Parameter))
	{
		return false;
	}

	const FMCPToolResult Baseline = Capture(Fixture.Function);
	if (!TestTrue(TEXT("Baseline function capture succeeds"), Baseline.bSuccess)
		|| !TestNotNull(TEXT("Baseline capture data exists"), Baseline.Data.Get()))
	{
		return false;
	}
	const FString BaselineId = Baseline.Data->GetStringField(TEXT("snapshotId"));
	const FString BaselineHash = Baseline.Data->GetStringField(TEXT("projectionHash"));
	SnapshotIds.Add(BaselineId);

	// RED contract: function-level metadata is mutable projection state and a
	// metadata-only revision must have a first-class diff record. A hash change
	// with an empty changes array is insufficient for a caller to inspect or
	// authorize the metadata edit.
	Fixture.Function->Description = TEXT("changed description");
	Fixture.Function->UserExposedCaption = TEXT("Changed Caption");
	Fixture.Function->bExposeToLibrary = true;
	Fixture.Function->LibraryCategoriesText = {FText::FromString(TEXT("Contract"))};
	const FMCPToolResult MetadataRevision = Capture(Fixture.Function);
	if (!TestTrue(TEXT("Metadata revision capture succeeds"), MetadataRevision.bSuccess)
		|| !TestNotNull(TEXT("Metadata revision data exists"), MetadataRevision.Data.Get()))
	{
		return false;
	}
	const FString MetadataId = MetadataRevision.Data->GetStringField(TEXT("snapshotId"));
	SnapshotIds.Add(MetadataId);
	TestNotEqual(
		TEXT("Mutable function metadata changes projection hash"),
		MetadataRevision.Data->GetStringField(TEXT("projectionHash")),
		BaselineHash);
	const TSharedPtr<FJsonObject> Metadata = MetadataRevision.Data->GetObjectField(TEXT("functionMetadata"));
	if (TestNotNull(TEXT("Capture exposes function metadata"), Metadata.Get()))
	{
		TestEqual(TEXT("Description is projected"), Metadata->GetStringField(TEXT("description")), FString(TEXT("changed description")));
		TestEqual(TEXT("Caption is projected"), Metadata->GetStringField(TEXT("userExposedCaption")), FString(TEXT("Changed Caption")));
		TestTrue(TEXT("Expose-to-library is projected"), Metadata->GetBoolField(TEXT("exposeToLibrary")));
	}

	const FMCPToolResult MetadataDiff = Diff(DiffParams(BaselineId, MetadataId));
	if (TestTrue(TEXT("Metadata-only diff succeeds"), MetadataDiff.bSuccess)
		&& TestNotNull(TEXT("Metadata-only diff data exists"), MetadataDiff.Data.Get()))
	{
		TestFalse(TEXT("Metadata-only diff is not projection-equal"), MetadataDiff.Data->GetBoolField(TEXT("projectionEqual")));
		const TArray<TSharedPtr<FJsonValue>>& Changes = MetadataDiff.Data->GetArrayField(TEXT("changes"));
		TestEqual(TEXT("Metadata-only diff emits one first-class change"), Changes.Num(), 1);
		if (Changes.Num() == 1 && Changes[0].IsValid())
		{
			const TSharedPtr<FJsonObject> Change = Changes[0]->AsObject();
			TestEqual(TEXT("Metadata diff entity is functionMetadata"), Change->GetStringField(TEXT("entity")), FString(TEXT("functionMetadata")));
			TestEqual(TEXT("Metadata diff is modified"), Change->GetStringField(TEXT("change")), FString(TEXT("modified")));
			TestTrue(TEXT("Description appears in metadata changed fields"), ContainsString(Change->GetArrayField(TEXT("changedFields")), TEXT("description")));
			TestTrue(TEXT("Caption appears in metadata changed fields"), ContainsString(Change->GetArrayField(TEXT("changedFields")), TEXT("userExposedCaption")));
		}
	}

	// Parameter edits remain node-local, but must still participate in the
	// same projection hash and expose their changed fields to Diff.
	Fixture.Function->Description = TEXT("baseline description");
	Fixture.Function->UserExposedCaption = TEXT("Baseline Caption");
	Fixture.Function->bExposeToLibrary = false;
	Fixture.Function->LibraryCategoriesText.Reset();
	const FMCPToolResult ParameterBaseline = Capture(Fixture.Function);
	if (!TestTrue(TEXT("Parameter baseline capture succeeds"), ParameterBaseline.bSuccess)
		|| !TestNotNull(TEXT("Parameter baseline data exists"), ParameterBaseline.Data.Get()))
	{
		return false;
	}
	const FString ParameterBaselineId = ParameterBaseline.Data->GetStringField(TEXT("snapshotId"));
	const FString ParameterBaselineHash = ParameterBaseline.Data->GetStringField(TEXT("projectionHash"));
	SnapshotIds.Add(ParameterBaselineId);
	Fixture.Parameter->Group = TEXT("Changed");
	Fixture.Parameter->SortPriority = 9;
	Fixture.Parameter->DefaultValue = 0.75f;
	const FMCPToolResult ParameterRevision = Capture(Fixture.Function);
	if (!TestTrue(TEXT("Parameter revision capture succeeds"), ParameterRevision.bSuccess)
		|| !TestNotNull(TEXT("Parameter revision data exists"), ParameterRevision.Data.Get()))
	{
		return false;
	}
	const FString ParameterId = ParameterRevision.Data->GetStringField(TEXT("snapshotId"));
	SnapshotIds.Add(ParameterId);
	TestNotEqual(
		TEXT("Mutable parameter fields change projection hash"),
		ParameterRevision.Data->GetStringField(TEXT("projectionHash")),
		ParameterBaselineHash);
	const FMCPToolResult ParameterDiff = Diff(DiffParams(ParameterBaselineId, ParameterId));
	if (TestTrue(TEXT("Parameter diff succeeds"), ParameterDiff.bSuccess)
		&& TestNotNull(TEXT("Parameter diff data exists"), ParameterDiff.Data.Get()))
	{
		const FString ParameterIdText = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Parameter);
		bool bFoundParameterChange = false;
		for (const TSharedPtr<FJsonValue>& Value : ParameterDiff.Data->GetArrayField(TEXT("changes")))
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Change = Value->AsObject();
			if (Change->GetStringField(TEXT("entity")) == TEXT("node")
				&& Change->GetStringField(TEXT("nodeId")) == ParameterIdText)
			{
				bFoundParameterChange = true;
				TestTrue(TEXT("Parameter group is diffed"), ContainsString(Change->GetArrayField(TEXT("changedFields")), TEXT("group")));
				TestTrue(TEXT("Parameter sort priority is diffed"), ContainsString(Change->GetArrayField(TEXT("changedFields")), TEXT("sortPriority")));
				TestTrue(TEXT("Parameter value is diffed"), ContainsString(Change->GetArrayField(TEXT("changedFields")), TEXT("value")));
			}
		}
		TestTrue(TEXT("Parameter revision emits a node change"), bFoundParameterChange);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialNodeSourceStateRedContractTest,
	"UE_AI_integration.MaterialGraphQuery.RedContract.NodeSourceLiveAndStaleState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialNodeSourceStateRedContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(Registry);
	Registry.EndDomainRegistration();
	FMCPToolBase* Resolver = Registry.FindTool(TEXT("content.material.graph.node.source.resolve"));
	if (!TestNotNull(TEXT("Node source resolver is registered"), Resolver))
	{
		return false;
	}

	FMaterialFunctionContractFixture Fixture = CreateFixture(TEXT("NodeSourceStateFunction"));
	TArray<FString> SnapshotIds;
	ON_SCOPE_EXIT
	{
		for (const FString& SnapshotId : SnapshotIds)
		{
			Release(SnapshotId);
		}
		CleanupFixture(Fixture);
	};
	if (!TestNotNull(TEXT("Source-state function fixture exists"), Fixture.Function)
		|| !TestNotNull(TEXT("Source-state expression fixture exists"), Fixture.Parameter))
	{
		return false;
	}

	const FMCPToolResult Captured = Capture(Fixture.Function);
	if (!TestTrue(TEXT("Source-state snapshot succeeds"), Captured.bSuccess)
		|| !TestNotNull(TEXT("Source-state snapshot data exists"), Captured.Data.Get()))
	{
		return false;
	}
	const FString SnapshotId = Captured.Data->GetStringField(TEXT("snapshotId"));
	SnapshotIds.Add(SnapshotId);
	const FString NodeId = MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Parameter);
	TSharedRef<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("snapshotId"), SnapshotId);
	ResolveParams->SetStringField(TEXT("nodeId"), NodeId);

	const FMCPToolResult Live = Resolver->Execute(ResolveParams);
	if (TestTrue(TEXT("Current node resolves"), Live.bSuccess)
		&& TestNotNull(TEXT("Live resolution data exists"), Live.Data.Get()))
	{
		TestTrue(TEXT("Live resolution performs a live check"), Live.Data->GetBoolField(TEXT("liveStateChecked")));
		// RED contract: a successful resolution must carry an explicit state so a
		// caller can distinguish a live object from an unverified identity.
		TestTrue(TEXT("Live resolution publishes an explicit live state"), HasResolutionState(Live.Data, TEXT("live")));
	}

	Fixture.Function->GetExpressionCollection().RemoveExpression(Fixture.Parameter);
	const FMCPToolResult Stale = Resolver->Execute(ResolveParams);
	TestFalse(TEXT("Removed snapshot node is rejected as stale"), Stale.bSuccess);
	TestEqual(TEXT("Removed snapshot node uses graph_node_stale"), Stale.ErrorCode, FString(TEXT("graph_node_stale")));
	// RED contract: stale failures retain structured state and identity context;
	// a bare error string cannot be safely routed by an automated caller.
	TestTrue(TEXT("Stale resolution publishes an explicit stale state"), HasResolutionState(Stale.Data, TEXT("stale")));
	if (Stale.Data.IsValid())
	{
		TestEqual(TEXT("Stale response echoes snapshot identity"), Stale.Data->GetStringField(TEXT("snapshotId")), SnapshotId);
		TestEqual(TEXT("Stale response echoes node identity"), Stale.Data->GetStringField(TEXT("nodeId")), NodeId);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialMetadataRollbackIdentityRedContractTest,
	"UE_AI_integration.MaterialGraphQuery.RedContract.MetadataPostEditRollbackIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialMetadataRollbackIdentityRedContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(Registry);
	UEAIIntegrationTools::RegisterMaterialMutationTools(Registry);
	Registry.EndDomainRegistration();
	FMCPToolBase* ExecutePlan = Registry.FindTool(TEXT("content.material.graph.execute_plan"));
	if (!TestNotNull(TEXT("Material graph execute_plan is registered"), ExecutePlan))
	{
		return false;
	}

	FMaterialFunctionContractFixture Fixture = CreateFixture(TEXT("MetadataRollbackFunction"));
	TArray<FString> SnapshotIds;
	ON_SCOPE_EXIT
	{
		for (const FString& SnapshotId : SnapshotIds)
		{
			Release(SnapshotId);
		}
		CleanupFixture(Fixture);
	};
	if (!TestNotNull(TEXT("Rollback function fixture exists"), Fixture.Function)
		|| !TestNotNull(TEXT("Rollback expression fixture exists"), Fixture.Parameter))
	{
		return false;
	}

	const FString FunctionPath = Fixture.AssetPath();
	UMaterialFunction* OriginalFunction = Fixture.Function;
	UMaterialExpressionScalarParameter* OriginalParameter = Fixture.Parameter;
	const FString OriginalNodeId = MCPMaterialInfrastructure::ExpressionNodeId(OriginalParameter);
	const FString OriginalDescription = Fixture.Function->Description;
	const bool bOriginalExposeToLibrary = Fixture.Function->bExposeToLibrary;
	const TArray<FText> OriginalCategories = Fixture.Function->LibraryCategoriesText;
	Fixture.Package->SetDirtyFlag(false);

	const FMCPToolResult Baseline = Capture(Fixture.Function);
	if (!TestTrue(TEXT("Rollback baseline capture succeeds"), Baseline.bSuccess)
		|| !TestNotNull(TEXT("Rollback baseline data exists"), Baseline.Data.Get()))
	{
		return false;
	}
	const FString SnapshotId = Baseline.Data->GetStringField(TEXT("snapshotId"));
	const FString ProjectionHash = Baseline.Data->GetStringField(TEXT("projectionHash"));
	SnapshotIds.Add(SnapshotId);

	const FMCPToolResult Rollback = ExecutePlan->Execute(
		ExecutePlanParams(Fixture, SnapshotId, ProjectionHash, ProjectionHash));
	TestFalse(TEXT("Expected-after mismatch enters rollback path"), Rollback.bSuccess);
	TestTrue(TEXT("Rollback reports a plan failure code"), Rollback.ErrorCode == TEXT("material_graph_plan_failed") || Rollback.ErrorCode == TEXT("material_graph_rollback_failed"));
	TestTrue(TEXT("Function UObject identity survives PostEdit rollback"), Fixture.Function == OriginalFunction);
	TestTrue(TEXT("Parameter UObject identity survives PostEdit rollback"), Fixture.Parameter == OriginalParameter);
	TestEqual(TEXT("Function description is restored"), Fixture.Function->Description, OriginalDescription);
	TestEqual(TEXT("Function expose-to-library is restored"), Fixture.Function->bExposeToLibrary, bOriginalExposeToLibrary);
	TestEqual(TEXT("Function category count is restored"), Fixture.Function->LibraryCategoriesText.Num(), OriginalCategories.Num());
	for (int32 Index = 0; Index < OriginalCategories.Num() && Index < Fixture.Function->LibraryCategoriesText.Num(); ++Index)
	{
		TestTrue(TEXT("Function category identity is restored"), Fixture.Function->LibraryCategoriesText[Index].EqualTo(OriginalCategories[Index]));
	}
	TestFalse(TEXT("Rollback restores the original package dirty state"), Fixture.Package->IsDirty());

	const FMCPToolResult Restored = Capture(Fixture.Function);
	if (TestTrue(TEXT("Restored function can be captured"), Restored.bSuccess)
		&& TestNotNull(TEXT("Restored capture data exists"), Restored.Data.Get()))
	{
		const FString RestoredId = Restored.Data->GetStringField(TEXT("snapshotId"));
		SnapshotIds.Add(RestoredId);
		TestEqual(TEXT("Rollback restores the exact projection hash"), Restored.Data->GetStringField(TEXT("projectionHash")), ProjectionHash);
		TestEqual(TEXT("Rollback preserves the expression node identity"), MCPMaterialInfrastructure::ExpressionNodeId(Fixture.Parameter), OriginalNodeId);
	}

	TSharedRef<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("snapshotId"), SnapshotId);
	ResolveParams->SetStringField(TEXT("nodeId"), OriginalNodeId);
	const FMCPToolResult Resolved = Registry.FindTool(TEXT("content.material.graph.node.source.resolve"))->Execute(ResolveParams);
	TestTrue(TEXT("Original snapshot still resolves after rollback"), Resolved.bSuccess);
	if (Resolved.bSuccess && Resolved.Data.IsValid())
	{
		const TSharedPtr<FJsonObject> Source = Resolved.Data->GetObjectField(TEXT("source"));
		TestEqual(TEXT("Rollback source points to the original object"), Source->GetStringField(TEXT("ueObjectName")), OriginalParameter->GetName());
	}
	return true;
}

#endif
