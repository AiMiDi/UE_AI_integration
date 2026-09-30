#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
template <typename T>
T* AddBoundaryExpression(UMaterialFunction* Function)
{
	T* Expression = NewObject<T>(Function);
	Function->GetExpressionCollection().AddExpression(Expression);
	return Expression;
}

TSharedRef<FJsonObject> BoundaryParams(
	const FString& SnapshotId,
	const FString& NodeId,
	const FString& Direction)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("snapshotId"), SnapshotId);
	Params->SetArrayField(
		TEXT("nodeIds"),
		{MakeShared<FJsonValueString>(NodeId)});
	Params->SetStringField(TEXT("direction"), Direction);
	Params->SetNumberField(TEXT("depth"), 8);
	Params->SetNumberField(TEXT("maxNodes"), 16);
	Params->SetNumberField(TEXT("maxEdges"), 16);
	return Params;
}

const TSharedPtr<FJsonObject> FindObjectByStringField(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const FString& Field,
	const FString& Expected)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const TSharedPtr<FJsonObject> Object =
			Value.IsValid() ? Value->AsObject() : nullptr;
		if (Object.IsValid()
			&& Object->HasTypedField<EJson::String>(Field)
			&& Object->GetStringField(Field) == Expected)
		{
			return Object;
		}
	}
	return nullptr;
}

bool StringArrayContains(
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const FString& Expected)
{
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		if (Value.IsValid()
			&& Value->Type == EJson::String
			&& Value->AsString() == Expected)
		{
			return true;
		}
	}
	return false;
}

FMCPToolResult ReadExactDefinition(const FString& DefinitionKey)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("assetKind"), TEXT("material"));
	Params->SetStringField(TEXT("definitionKey"), DefinitionKey);
	return UEAIIntegration::MaterialQuery::ListDefinitions(Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialGraphBoundaryContractTest,
	"UE_AI_integration.MaterialGraphQuery.BoundaryContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphBoundaryContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	using MCPMaterialInfrastructure::ExpressionNodeId;

	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
	auto* Upstream = AddBoundaryExpression<UMaterialExpressionConstant>(Function.Get());
	auto* Selected = AddBoundaryExpression<UMaterialExpressionAdd>(Function.Get());
	auto* SharedConsumer = AddBoundaryExpression<UMaterialExpressionAdd>(Function.Get());
	Upstream->R = 0.25f;
	Selected->A.Connect(0, Upstream);
	SharedConsumer->A.Connect(0, Selected);
	Function->GetOutermost()->SetDirtyFlag(false);

	const FMCPToolResult Captured = Capture(Function.Get());
	if (!TestTrue(TEXT("Boundary fixture captures"), Captured.bSuccess)
		|| !Captured.Data)
	{
		return false;
	}
	const FString SnapshotId = Captured.Data->GetStringField(TEXT("snapshotId"));
	FString ChangedSnapshotId;
	ON_SCOPE_EXIT
	{
		if (!ChangedSnapshotId.IsEmpty())
		{
			const FMCPToolResult Released = Release(ChangedSnapshotId);
			TestTrue(TEXT("Changed boundary snapshot is released"),
				Released.bSuccess && Released.Data
				&& Released.Data->GetBoolField(TEXT("released")));
		}
		const FMCPToolResult Released = Release(SnapshotId);
		TestTrue(TEXT("Boundary snapshot is released"),
			Released.bSuccess && Released.Data
			&& Released.Data->GetBoolField(TEXT("released")));
	};
	const FString SelectedId = ExpressionNodeId(Selected);
	const FMCPToolResult UpstreamBoundary = Boundary(
		BoundaryParams(SnapshotId, SelectedId, TEXT("upstream")));
	if (TestTrue(TEXT("Complete upstream boundary succeeds"), UpstreamBoundary.bSuccess)
		&& TestNotNull(TEXT("Boundary result data"), UpstreamBoundary.Data.Get()))
	{
		TestEqual(TEXT("Boundary keeps source snapshot identity"),
			UpstreamBoundary.Data->GetStringField(TEXT("sourceSnapshotId")), SnapshotId);
		TestFalse(TEXT("Read does not claim a live-state check"),
			UpstreamBoundary.Data->GetBoolField(TEXT("liveStateChecked")));
		TestTrue(TEXT("Writer must perform a live fingerprint check"),
			UpstreamBoundary.Data->GetBoolField(TEXT("requiresLiveFingerprintCheck")));
		TestTrue(TEXT("Opposite-direction shared consumer is reported"),
			UpstreamBoundary.Data->GetBoolField(TEXT("hasExternallyConsumedNodes")));
		TestTrue(TEXT("Shared consumer requires confirmation"),
			UpstreamBoundary.Data->GetBoolField(TEXT("requiresSharedNodeConfirmation")));
		TestFalse(TEXT("Upstream closure has no hidden dependency"),
			UpstreamBoundary.Data->GetBoolField(TEXT("hasExternalDependencies")));
		TestEqual(TEXT("One downstream crossing edge is returned"),
			UpstreamBoundary.Data->GetIntegerField(TEXT("boundaryEdgeCount")), 1);
		const TArray<TSharedPtr<FJsonValue>>& Edges =
			UpstreamBoundary.Data->GetArrayField(TEXT("boundaryEdges"));
		if (Edges.Num() == 1 && Edges[0].IsValid())
		{
			const TSharedPtr<FJsonObject> Edge = Edges[0]->AsObject();
			TestEqual(TEXT("Crossing edge starts inside"),
				Edge->GetStringField(TEXT("sourceNodeId")), SelectedId);
			TestEqual(TEXT("Crossing edge identifies external consumer"),
				Edge->GetStringField(TEXT("targetNodeId")), ExpressionNodeId(SharedConsumer));
		}
	}

	const FMCPToolResult Replay = Boundary(
		BoundaryParams(SnapshotId, SelectedId, TEXT("upstream")));
	if (TestTrue(TEXT("Identical boundary replay succeeds"), Replay.bSuccess)
		&& Replay.Data && UpstreamBoundary.Data)
	{
		TestEqual(TEXT("Boundary digest is deterministic"),
			Replay.Data->GetStringField(TEXT("boundaryDigest")),
			UpstreamBoundary.Data->GetStringField(TEXT("boundaryDigest")));
		TestEqual(TEXT("Boundary ID is deterministic"),
			Replay.Data->GetStringField(TEXT("boundaryId")),
			UpstreamBoundary.Data->GetStringField(TEXT("boundaryId")));
	}

	const FMCPToolResult DownstreamBoundary = Boundary(
		BoundaryParams(SnapshotId, SelectedId, TEXT("downstream")));
	if (TestTrue(TEXT("Complete downstream boundary succeeds"), DownstreamBoundary.bSuccess)
		&& DownstreamBoundary.Data)
	{
		TestTrue(TEXT("Opposite-direction dependency is reported"),
			DownstreamBoundary.Data->GetBoolField(TEXT("hasExternalDependencies")));
		TestFalse(TEXT("Downstream closure has no hidden consumer"),
			DownstreamBoundary.Data->GetBoolField(TEXT("hasExternallyConsumedNodes")));
	}

	auto Incomplete = BoundaryParams(SnapshotId, SelectedId, TEXT("both"));
	Incomplete->SetNumberField(TEXT("maxEdges"), 1);
	const FMCPToolResult Rejected = Boundary(Incomplete);
	TestFalse(TEXT("Incomplete edge coverage is rejected"), Rejected.bSuccess);
	TestEqual(TEXT("Incomplete boundary has stable conflict code"),
		Rejected.ErrorCode, FString(TEXT("material_boundary_incomplete")));

	Upstream->R = 0.75f;
	const FMCPToolResult Changed = Capture(Function.Get());
	if (TestTrue(TEXT("Changed projection captures"), Changed.bSuccess)
		&& Changed.Data)
	{
		ChangedSnapshotId = Changed.Data->GetStringField(TEXT("snapshotId"));
		if (UpstreamBoundary.Data)
		{
			const FMCPToolResult ChangedBoundary = Boundary(
				BoundaryParams(ChangedSnapshotId, SelectedId, TEXT("upstream")));
			if (TestTrue(TEXT("Changed boundary succeeds"), ChangedBoundary.bSuccess)
				&& ChangedBoundary.Data)
			{
				TestNotEqual(TEXT("Projection change produces a new boundary digest"),
					ChangedBoundary.Data->GetStringField(TEXT("boundaryDigest")),
					UpstreamBoundary.Data->GetStringField(TEXT("boundaryDigest")));
			}
		}
	}

	TestFalse(TEXT("Boundary queries preserve clean package state"),
		Function->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialGraphDefinitionCatalogContractTest,
	"UE_AI_integration.MaterialGraphQuery.DefinitionCatalogContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialGraphDefinitionCatalogContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;
	auto FirstParams = MakeShared<FJsonObject>();
	FirstParams->SetStringField(TEXT("assetKind"), TEXT("material"));
	FirstParams->SetNumberField(TEXT("limit"), 1);
	const FMCPToolResult First = ListDefinitions(FirstParams);
	if (!TestTrue(TEXT("First definition page succeeds"), First.bSuccess)
		|| !First.Data)
	{
		return false;
	}
	TestEqual(TEXT("Catalog schema is explicit"),
		First.Data->GetStringField(TEXT("schema")),
		FString(TEXT("ue.material.expression-definitions/1")));
	TestEqual(TEXT("Limit one returns one definition"),
		First.Data->GetIntegerField(TEXT("returnedCount")), 1);
	TestTrue(TEXT("Definition catalog is complete"),
		First.Data->GetBoolField(TEXT("catalogComplete")));
	TestFalse(TEXT("Definition class scan is not exhausted"),
		First.Data->GetBoolField(TEXT("scanExhausted")));
	TestFalse(TEXT("No generic reflected-property writer is advertised"),
		First.Data->GetBoolField(
			TEXT("genericReflectedPropertyWriterSupported")));
	const TSharedPtr<FJsonObject> Limits =
		First.Data->GetObjectField(TEXT("limits"));
	if (TestNotNull(TEXT("Definition limits are explicit"), Limits.Get()))
	{
		TestEqual(TEXT("Property contracts are bounded"),
			Limits->GetIntegerField(TEXT("maxPropertyContractsPerDefinition")),
			32);
		TestEqual(TEXT("Input templates are bounded"),
			Limits->GetIntegerField(TEXT("maxInputTemplatesPerDefinition")),
			64);
		TestEqual(TEXT("Output templates are bounded"),
			Limits->GetIntegerField(TEXT("maxOutputTemplatesPerDefinition")),
			64);
		TestEqual(TEXT("Enum values are bounded"),
			Limits->GetIntegerField(TEXT("maxEnumValuesPerProperty")),
			64);
		TestEqual(TEXT("Default property text is bounded"),
			Limits->GetIntegerField(TEXT("maxDefaultTextCharacters")),
			512);
		TestEqual(TEXT("Definition response bytes are bounded"),
			Limits->GetIntegerField(TEXT("responseBudgetBytes")),
			256 * 1024);
		TestEqual(TEXT("Loaded class scanning is bounded"),
			Limits->GetIntegerField(TEXT("maxClassesScanned")),
			50000);
		TestEqual(TEXT("Built contracts are bounded"),
			Limits->GetIntegerField(TEXT("maxContractsBuilt")),
			1024);
	}
	TestTrue(TEXT("Catalog exposes continuation"),
		First.Data->GetBoolField(TEXT("hasMore")));
	const TArray<TSharedPtr<FJsonValue>>& FirstRows =
		First.Data->GetArrayField(TEXT("definitions"));
	if (FirstRows.IsEmpty() || !FirstRows[0].IsValid())
	{
		AddError(TEXT("Definition page did not contain its promised row."));
		return false;
	}
	const TSharedPtr<FJsonObject> FirstRow =
		FirstRows[0]->AsObject();
	const FString FirstKey = FirstRow->GetStringField(TEXT("definitionKey"));
	const FString FirstClass = FirstRow->GetStringField(TEXT("className"));
	TestEqual(TEXT("Only ordinary creation is advertised"),
		FirstRow->GetStringField(TEXT("creationMode")),
		FString(TEXT("ordinaryExpression")));
	TestEqual(TEXT("Row keeps target asset kind"),
		FirstRow->GetStringField(TEXT("assetKind")), FString(TEXT("material")));

	const FString Cursor = First.Data->GetStringField(TEXT("nextCursor"));
	auto SecondParams = MakeShared<FJsonObject>();
	SecondParams->SetStringField(TEXT("assetKind"), TEXT("material"));
	SecondParams->SetNumberField(TEXT("limit"), 1);
	SecondParams->SetStringField(TEXT("cursor"), Cursor);
	const FMCPToolResult Second = ListDefinitions(SecondParams);
	if (TestTrue(TEXT("Second definition page succeeds"), Second.bSuccess)
		&& Second.Data)
	{
		const TArray<TSharedPtr<FJsonValue>>& SecondRows =
			Second.Data->GetArrayField(TEXT("definitions"));
		if (TestTrue(TEXT("Second page contains one row"), !SecondRows.IsEmpty()))
		{
			const FString SecondKey = SecondRows[0]
				->AsObject()->GetStringField(TEXT("definitionKey"));
			TestTrue(TEXT("Definitions have stable key ordering"),
				FirstKey.Compare(SecondKey, ESearchCase::CaseSensitive) < 0);
		}
	}

	auto ExactParams = MakeShared<FJsonObject>();
	ExactParams->SetStringField(TEXT("definitionKey"), FirstKey);
	const FMCPToolResult Exact = ListDefinitions(ExactParams);
	if (TestTrue(TEXT("Exact definition lookup succeeds"), Exact.bSuccess)
		&& Exact.Data)
	{
		TestEqual(TEXT("Exact lookup returns one definition"),
			Exact.Data->GetIntegerField(TEXT("total")), 1);
		const TArray<TSharedPtr<FJsonValue>>& ExactRows =
			Exact.Data->GetArrayField(TEXT("definitions"));
		if (TestTrue(TEXT("Exact lookup contains one row"), ExactRows.Num() == 1))
		{
			TestEqual(TEXT("Exact lookup preserves class identity"),
				ExactRows[0]->AsObject()->GetStringField(TEXT("className")),
				FirstClass);
		}
	}

	auto Disallowed = MakeShared<FJsonObject>();
	Disallowed->SetStringField(TEXT("definitionKey"), TEXT("Comment"));
	const FMCPToolResult DisallowedResult = ListDefinitions(Disallowed);
	if (TestTrue(TEXT("Disallowed lookup is queryable"), DisallowedResult.bSuccess)
		&& DisallowedResult.Data)
	{
		TestEqual(TEXT("Comment is excluded from ordinary creation"),
			DisallowedResult.Data->GetIntegerField(TEXT("total")), 0);
	}

	auto ChangedFilter = MakeShared<FJsonObject>();
	ChangedFilter->SetStringField(TEXT("assetKind"), TEXT("material"));
	ChangedFilter->SetStringField(TEXT("search"), TEXT("constant"));
	ChangedFilter->SetNumberField(TEXT("limit"), 1);
	ChangedFilter->SetStringField(TEXT("cursor"), Cursor);
	TestFalse(TEXT("Cursor is bound to its exact filter"),
		ListDefinitions(ChangedFilter).bSuccess);
	auto ChangedKind = MakeShared<FJsonObject>();
	ChangedKind->SetStringField(TEXT("assetKind"), TEXT("materialFunction"));
	ChangedKind->SetNumberField(TEXT("limit"), 1);
	ChangedKind->SetStringField(TEXT("cursor"), Cursor);
	TestFalse(TEXT("Cursor is bound to asset kind and catalog"),
		ListDefinitions(ChangedKind).bSuccess);
	auto Malformed = MakeShared<FJsonObject>();
	Malformed->SetStringField(TEXT("cursor"), TEXT("***not-base64***"));
	TestFalse(TEXT("Malformed cursor is rejected"), ListDefinitions(Malformed).bSuccess);
	auto Conflicting = MakeShared<FJsonObject>();
	Conflicting->SetStringField(TEXT("definitionKey"), FirstKey);
	Conflicting->SetStringField(TEXT("search"), FirstKey);
	TestFalse(TEXT("Exact and fuzzy filters are mutually exclusive"),
		ListDefinitions(Conflicting).bSuccess);
	auto Fractional = MakeShared<FJsonObject>();
	Fractional->SetNumberField(TEXT("limit"), 1.5);
	TestFalse(TEXT("Definition limit must be an integer"),
		ListDefinitions(Fractional).bSuccess);

	// Verify concrete core definitions instead of only checking catalog paging.
	// These assertions keep read-only reflection separate from the small,
	// class-specific direct-value writer surface.
	const FMCPToolResult Constant = ReadExactDefinition(TEXT("Constant"));
	if (TestTrue(TEXT("Constant definition lookup succeeds"), Constant.bSuccess)
		&& Constant.Data)
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			Constant.Data->GetArrayField(TEXT("definitions"));
		if (TestEqual(TEXT("Constant lookup returns one row"), Rows.Num(), 1))
		{
			const TSharedPtr<FJsonObject> Row = Rows[0]->AsObject();
			TestEqual(TEXT("Constant is a built-in definition"),
				Row->GetStringField(TEXT("definitionKind")),
				FString(TEXT("builtInExpression")));
			TestEqual(TEXT("Pin templates come from the class default object"),
				Row->GetStringField(TEXT("templateSource")),
				FString(TEXT("classDefaultObject")));
			TestFalse(TEXT("CDO templates do not promise instance completeness"),
				Row->GetBoolField(TEXT("templatesCompleteForInstances")));
			const TSharedPtr<FJsonObject> Writer =
				Row->GetObjectField(TEXT("directValueWriter"));
			if (TestNotNull(TEXT("Constant direct writer contract"), Writer.Get()))
			{
				TestTrue(TEXT("Constant direct value is supported"),
					Writer->GetBoolField(TEXT("supported")));
				TestFalse(TEXT("Constant writer is not generic reflection"),
					Writer->GetBoolField(TEXT("genericReflectedPropertyWriter")));
				TestEqual(TEXT("Constant direct value kind is numeric"),
					Writer->GetStringField(TEXT("valueKind")),
					FString(TEXT("number")));
			}
			const TSharedPtr<FJsonObject> R = FindObjectByStringField(
				Row->GetArrayField(TEXT("propertyContracts")),
				TEXT("propertyKey"),
				TEXT("R"));
			if (TestNotNull(TEXT("Constant R property contract"), R.Get()))
			{
				TestTrue(TEXT("Constant R has a class-specific writer"),
					R->GetBoolField(TEXT("writerSupported")));
				TestFalse(TEXT("Writable R is not marked read-only"),
					R->GetBoolField(TEXT("readOnly")));
				TestEqual(TEXT("Constant R writer role is primary value"),
					R->GetStringField(TEXT("writerRole")),
					FString(TEXT("primaryValue")));
				TestTrue(TEXT("Constant R accepts numeric values"),
					StringArrayContains(
						R->GetArrayField(TEXT("allowedValueKinds")),
						TEXT("number")));
				TestFalse(TEXT("Bounded Constant default is retained"),
					R->GetBoolField(TEXT("defaultValueOmitted")));
				TestTrue(TEXT("Constant default remains within its bound"),
					R->GetIntegerField(TEXT("defaultValueCharacters")) <= 512);
			}
		}
	}

	const FMCPToolResult Add = ReadExactDefinition(TEXT("Add"));
	if (TestTrue(TEXT("Add definition lookup succeeds"), Add.bSuccess)
		&& Add.Data)
	{
		const TSharedPtr<FJsonObject> Row =
			Add.Data->GetArrayField(TEXT("definitions"))[0]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>& Inputs =
			Row->GetArrayField(TEXT("inputTemplates"));
		TestEqual(TEXT("Add exposes both CDO input templates"), Inputs.Num(), 2);
		for (const TSharedPtr<FJsonValue>& Value : Inputs)
		{
			const TSharedPtr<FJsonObject> Input = Value->AsObject();
			TestFalse(TEXT("CDO input presence is not guaranteed on instances"),
				Input->GetBoolField(TEXT("instancePresenceGuaranteed")));
			TestTrue(TEXT("Present input supports the connection writer"),
				Input->GetBoolField(
					TEXT("connectionWriterSupportedWhenPresent")));
			TestEqual(TEXT("Input connection writer is explicit"),
				Input->GetStringField(TEXT("connectionWriterCapability")),
				FString(TEXT("content.material.pin.connect")));
		}
		const TSharedPtr<FJsonObject> ConstA = FindObjectByStringField(
			Row->GetArrayField(TEXT("propertyContracts")),
			TEXT("propertyKey"),
			TEXT("ConstA"));
		if (TestNotNull(TEXT("Add ConstA property contract"), ConstA.Get()))
		{
			TestTrue(TEXT("Unsupported reflected Add fields are read-only"),
				ConstA->GetBoolField(TEXT("readOnly")));
			TestFalse(TEXT("Unsupported reflected Add fields have no writer"),
				ConstA->GetBoolField(TEXT("writerSupported")));
		}
	}

	const FMCPToolResult Custom = ReadExactDefinition(TEXT("Custom"));
	if (TestTrue(TEXT("Custom definition lookup succeeds"), Custom.bSuccess)
		&& Custom.Data)
	{
		const TSharedPtr<FJsonObject> Row =
			Custom.Data->GetArrayField(TEXT("definitions"))[0]->AsObject();
		TestEqual(TEXT("Custom HLSL definition kind is explicit"),
			Row->GetStringField(TEXT("definitionKind")),
			FString(TEXT("customHlslNode")));
		TestTrue(TEXT("Custom pins require instance configuration"),
			Row->GetBoolField(TEXT("dynamicInstanceConfigurationRequired")));
		const TSharedPtr<FJsonObject> Writer =
			Row->GetObjectField(TEXT("directValueWriter"));
		TestTrue(TEXT("Custom Code has a direct writer"),
			Writer->GetBoolField(TEXT("supported")));
		TestEqual(TEXT("Custom direct value is string HLSL"),
			Writer->GetStringField(TEXT("valueKind")),
			FString(TEXT("string")));
	}
	return true;
}

#endif
