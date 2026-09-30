#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataAsset.h"
#include "HAL/FileManager.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegrationTools
{
	void RegisterBlueprintDataAssetTools(FMCPToolRegistry& Registry);
	void RegisterBlueprintBatchSpawnTools(FMCPToolRegistry& Registry);
	void RegisterMaterialGraphQueryTools(FMCPToolRegistry& Registry);
}

namespace
{
	TSharedRef<FJsonObject> MakeDataAssetParams(
		const FString& SavePath,
		const FString& ClassName)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("savePath"), SavePath);
		Params->SetStringField(TEXT("className"), ClassName);
		return Params;
	}

	TSharedRef<FJsonObject> MakeBatchSpawnParams(
		const FString& BlueprintPath,
		const double Count)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), BlueprintPath);
		Params->SetNumberField(TEXT("count"), Count);
		return Params;
	}

	void CleanupTransientAsset(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->MarkAsGarbage();
		if (UPackage* Package = Asset->GetOutermost())
		{
			Package->SetDirtyFlag(false);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWave5iDataAssetCreateContractTest,
	"UE_AI_integration.Blueprint.DataAsset.CreateContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWave5iDataAssetCreateContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintDataAssetTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* Tool = Registry.FindTool(TEXT("blueprint.data_asset.create"));
	if (!TestNotNull(TEXT("DataAsset create tool is registered"), Tool))
	{
		return false;
	}

	const FMCPToolResult Missing = Tool->Execute(MakeShared<FJsonObject>());
	TestFalse(TEXT("DataAsset create rejects missing parameters"), Missing.bSuccess);
	TestEqual(TEXT("Missing parameters use invalid_params"), Missing.ErrorCode, FString(TEXT("invalid_params")));

	const FMCPToolResult BadPath = Tool->Execute(
		MakeDataAssetParams(TEXT("/Engine/Automation/NotGame"), TEXT("UDataAsset")));
	TestFalse(TEXT("DataAsset create rejects non-Game packages"), BadPath.bSuccess);
	TestEqual(TEXT("Non-Game package uses invalid_package_path"), BadPath.ErrorCode,
	          FString(TEXT("invalid_package_path")));

	const FMCPToolResult MissingClass = Tool->Execute(
		MakeDataAssetParams(
			TEXT("/Game/Automation/UEAI_DataAsset_MissingClass"),
			TEXT("UEAI_NoSuchClass")));
	TestFalse(TEXT("DataAsset create rejects unknown classes"), MissingClass.bSuccess);
	TestEqual(TEXT("Unknown class uses class_not_found"), MissingClass.ErrorCode, FString(TEXT("class_not_found")));

	const FString PackageName =
		TEXT("/Game/Automation/UEAI_DataAsset_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	const FMCPToolResult Created = Tool->Execute(
		MakeDataAssetParams(PackageName, UDataAsset::StaticClass()->GetPathName()));
	// Diagnostic-only context is retained in the Automation report when the
	// product contract fails before a result payload is returned.
	if (!Created.bSuccess)
	{
		AddInfo(FString::Printf(
			TEXT("DataAsset create diagnostic: errorCode='%s', httpStatus=%d, errorMessage='%s', hasData=%s"),
			*Created.ErrorCode,
			Created.HttpStatus,
			*Created.ErrorMessage,
			Created.Data.IsValid() ? TEXT("true") : TEXT("false")));
	}
	if (!TestTrue(TEXT("DataAsset create succeeds for a native UObject class"), Created.bSuccess)
		|| !TestNotNull(TEXT("DataAsset create returns data"), Created.Data.Get()))
	{
		return false;
	}

	TestEqual(TEXT("Created package path is echoed"), Created.Data->GetStringField(TEXT("asset_path")), PackageName);
	TestEqual(TEXT("Created object path is echoed"), Created.Data->GetStringField(TEXT("object_path")), ObjectPath);
	TestEqual(
		TEXT("Created class path is the requested native class"),
		Created.Data->GetStringField(TEXT("class_path")),
		UDataAsset::StaticClass()->GetPathName());
	TestTrue(TEXT("Created asset is registered"), Created.Data->GetBoolField(TEXT("asset_registry_registered")));
	TestTrue(
		TEXT("Skip-save false leaves the package clean after save"),
		!Created.Data->GetBoolField(TEXT("package_dirty")));
	TestTrue(TEXT("Create reports that save was attempted"), Created.Data->GetBoolField(TEXT("save_attempted")));
	TestTrue(TEXT("Create reports persisted asset"), Created.Data->GetBoolField(TEXT("saved")));

	// The default path is saved, so explicitly remove the isolated fixture and
	// its package file before the test returns.
	UObject* CreatedAsset = FindObject<UObject>(nullptr, *ObjectPath);
	CleanupTransientAsset(CreatedAsset);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		PackageName,
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().Delete(*PackageFilename, false, true, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWave5iBatchSpawnValidationContractTest,
	"UE_AI_integration.Blueprint.BatchSpawn.ValidationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWave5iBatchSpawnValidationContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintBatchSpawnTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* Tool = Registry.FindTool(TEXT("blueprint.actor.batch_spawn"));
	if (!TestNotNull(TEXT("Blueprint batch-spawn tool is registered"), Tool))
	{
		return false;
	}

	const FMCPToolResult Missing = Tool->Execute(nullptr);
	TestFalse(TEXT("Batch spawn rejects a null request"), Missing.bSuccess);
	TestEqual(TEXT("Null request uses invalid_params"), Missing.ErrorCode, FString(TEXT("invalid_params")));

	const FString UnknownBlueprint = TEXT("/Game/Automation/UEAI_MissingBlueprint_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FMCPToolResult ZeroCount = Tool->Execute(MakeBatchSpawnParams(UnknownBlueprint, 0.0));
	TestFalse(TEXT("Batch spawn rejects a zero count before asset lookup"), ZeroCount.bSuccess);
	TestEqual(TEXT("Zero count uses invalid_params"), ZeroCount.ErrorCode, FString(TEXT("invalid_params")));

	const FMCPToolResult FractionalCount = Tool->Execute(MakeBatchSpawnParams(UnknownBlueprint, 1.5));
	TestFalse(TEXT("Batch spawn rejects a fractional count"), FractionalCount.bSuccess);
	TestEqual(TEXT("Fractional count uses invalid_params"), FractionalCount.ErrorCode, FString(TEXT("invalid_params")));

	const FMCPToolResult MissingBlueprint = Tool->Execute(MakeBatchSpawnParams(UnknownBlueprint, 1.0));
	TestFalse(TEXT("Batch spawn rejects an unknown Blueprint"), MissingBlueprint.bSuccess);
	TestEqual(
		TEXT("Unknown Blueprint uses blueprint_not_found"),
		MissingBlueprint.ErrorCode,
		FString(TEXT("blueprint_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWave5iMaterialNodeSourceResolveContractTest,
	"UE_AI_integration.MaterialGraphQuery.NodeSourceResolveContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWave5iMaterialNodeSourceResolveContractTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialQuery;

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(Registry);
	Registry.EndDomainRegistration();
	if (!TestNotNull(
		TEXT("Material node source resolver is registered"),
		Registry.FindTool(TEXT("content.material.graph.node.source.resolve"))))
	{
		return false;
	}

	const FString PackageName =
		TEXT("/Game/Automation/UEAI_SourceResolve_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*PackageName);
	UMaterialFunction* Function = Package
		                              ? NewObject<UMaterialFunction>(
			                              Package,
			                              TEXT("SourceResolveFunction"),
			                              RF_Public | RF_Standalone | RF_Transactional)
		                              : nullptr;
	UMaterialExpressionConstant* Constant = Function
		                                        ? NewObject<UMaterialExpressionConstant>(
			                                        Function,
			                                        TEXT("AuthoredConstant"),
			                                        RF_Transactional)
		                                        : nullptr;
	if (!TestNotNull(TEXT("Material function fixture exists"), Function)
		|| !TestNotNull(TEXT("Material expression fixture exists"), Constant))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Function);
	Function->GetExpressionCollection().AddExpression(Constant);
	ON_SCOPE_EXIT
	{
		CleanupTransientAsset(Function);
	};

	const FMCPToolResult Captured = Capture(Function);
	if (!TestTrue(TEXT("Graph snapshot captures the authored function"), Captured.bSuccess)
		|| !TestNotNull(TEXT("Graph snapshot returns data"), Captured.Data.Get()))
	{
		return false;
	}
	const FString SnapshotId = Captured.Data->GetStringField(TEXT("snapshotId"));
	const FString NodeId = MCPMaterialInfrastructure::ExpressionNodeId(Constant);
	TSharedRef<FJsonObject> ResolveParams = MakeShared<FJsonObject>();
	ResolveParams->SetStringField(TEXT("snapshotId"), SnapshotId);
	ResolveParams->SetStringField(TEXT("nodeId"), NodeId);

	const FMCPToolResult Resolved = Registry.ExecuteTool(
		TEXT("content.material.graph.node.source.resolve"),
		ResolveParams);
	if (!TestTrue(TEXT("Authored node source resolves from its snapshot"), Resolved.bSuccess)
		|| !TestNotNull(TEXT("Node source response has data"), Resolved.Data.Get()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Source = Resolved.Data->GetObjectField(TEXT("source"));
	TestEqual(TEXT("Resolved source object name"), Source->GetStringField(TEXT("ueObjectName")), Constant->GetName());
	TestEqual(TEXT("Resolved source class"), Source->GetStringField(TEXT("ueClass")), Constant->GetClass()->GetName());
	TestEqual(TEXT("Resolver is diagnostic-only"), Source->GetStringField(TEXT("sourceMappingState")),
	          FString(TEXT("unavailable")));
	TestFalse(TEXT("Resolver never authorizes writes"), Resolved.Data->GetBoolField(TEXT("writeAuthorized")));
	TestEqual(TEXT("Resolver echoes snapshot identity"), Resolved.Data->GetStringField(TEXT("snapshotId")), SnapshotId);

	TSharedRef<FJsonObject> UnknownNode = MakeShared<FJsonObject>();
	UnknownNode->SetStringField(TEXT("snapshotId"), SnapshotId);
	UnknownNode->SetStringField(TEXT("nodeId"), TEXT("expr:does-not-exist"));
	const FMCPToolResult UnknownNodeResult = Registry.ExecuteTool(
		TEXT("content.material.graph.node.source.resolve"),
		UnknownNode);
	TestFalse(TEXT("Unknown snapshot node is rejected"), UnknownNodeResult.bSuccess);
	TestEqual(TEXT("Unknown node uses graph_node_not_found"), UnknownNodeResult.ErrorCode,
	          FString(TEXT("graph_node_not_found")));

	TestTrue(TEXT("Snapshot release succeeds"), Release(SnapshotId).bSuccess);
	const FMCPToolResult Released = Registry.ExecuteTool(
		TEXT("content.material.graph.node.source.resolve"),
		ResolveParams);
	TestFalse(TEXT("Released snapshot cannot resolve a source"), Released.bSuccess);
	TestEqual(TEXT("Released snapshot uses graph_snapshot_unavailable"), Released.ErrorCode,
	          FString(TEXT("graph_snapshot_unavailable")));
	return true;
}

#endif
