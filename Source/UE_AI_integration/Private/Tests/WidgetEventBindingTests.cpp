#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationSubsystem.h"

namespace
{
TSharedPtr<FJsonObject> Params(
	std::initializer_list<TPair<FString, FString>> Fields)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Field : Fields)
	{
		Result->SetStringField(Field.Key, Field.Value);
	}
	return Result;
}

bool CleanupWidgetAsset(const FString& AssetPath)
{
	if (!AssetPath.StartsWith(TEXT("/Game/Automation/")))
	{
		return false;
	}
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*PackageName,
		*FPackageName::GetShortName(PackageName));
	UObject* LoadedAsset = FindObject<UObject>(nullptr, *ObjectPath);
	FAssetData AssetData;
	if (IsValid(LoadedAsset) && LoadedAsset->IsAsset())
	{
		LoadedAsset->GetOutermost()->SetDirtyFlag(false);
		AssetData = FAssetData(LoadedAsset);
	}
	else
	{
		AssetData = FAssetRegistryModule::GetRegistry()
			.GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true);
	}
	if (AssetData.IsValid())
	{
		TArray<FAssetData> AssetsToDelete{AssetData};
		ObjectTools::DeleteAssets(AssetsToDelete, false);
	}

	LoadedAsset = FindObject<UObject>(nullptr, *ObjectPath);
	if (IsValid(LoadedAsset) && LoadedAsset->IsAsset())
	{
		LoadedAsset->GetOutermost()->SetDirtyFlag(false);
		TArray<UObject*> AssetsToDelete{LoadedAsset};
		ObjectTools::DeleteObjectsUnchecked(AssetsToDelete);
		LoadedAsset = FindObject<UObject>(nullptr, *ObjectPath);
	}
	return (!IsValid(LoadedAsset) || !LoadedAsset->IsAsset())
		&& !FAssetRegistryModule::GetRegistry()
			.GetAssetByObjectPath(FSoftObjectPath(ObjectPath), true)
			.IsValid()
		&& !FPackageName::DoesPackageExist(PackageName);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidgetEventBindingRegressionTest,
	"UE_AI_integration.UMG.ComponentEventBindingRegression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetEventBindingRegressionTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddError(TEXT("The Editor is unavailable."));
		return false;
	}
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>();
	if (!Subsystem || !Subsystem->GetRegistry())
	{
		AddError(TEXT("UE_AI_integration subsystem or registry is unavailable."));
		return false;
	}
	FMCPToolRegistry& Registry = *Subsystem->GetRegistry();

	const FString AssetName = FString::Printf(
		TEXT("WBP_EventBinding_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = TEXT("/Game/Automation");
	const FString AssetPath = PackagePath + TEXT("/") + AssetName;
	auto Cleanup = [&AssetPath]()
	{
		return CleanupWidgetAsset(AssetPath);
	};
	Cleanup();

	FMCPToolResult Create = Registry.ExecuteTool(
		TEXT("content.widget.blueprint.create"),
		Params({
			{TEXT("name"), AssetName},
			{TEXT("package_path"), PackagePath},
		}));
	TestTrue(TEXT("Widget Blueprint fixture is created"), Create.bSuccess);
	if (!Create.bSuccess)
	{
		Cleanup();
		return false;
	}

	FMCPToolResult AddButton = Registry.ExecuteTool(
		TEXT("content.widget.child.add"),
		Params({
			{TEXT("widget_bp"), AssetPath},
			{TEXT("child_class"), TEXT("Button")},
			{TEXT("child_name"), TEXT("TestButton")},
			{TEXT("parent_name"), TEXT("RootCanvas")},
		}));
	TestTrue(TEXT("Button is added"), AddButton.bSuccess);

	FMCPToolResult AddFunction = Registry.ExecuteTool(
		TEXT("blueprint.graph.create"),
		Params({
			{TEXT("blueprint"), AssetPath},
			{TEXT("graphName"), TEXT("HandleClicked")},
			{TEXT("graphType"), TEXT("function")},
		}));
	TestTrue(TEXT("Handler function graph is created"), AddFunction.bSuccess);

	const TSharedPtr<FJsonObject> BindParams = Params({
		{TEXT("widget_bp"), AssetPath},
		{TEXT("widget_name"), TEXT("TestButton")},
		{TEXT("event_name"), TEXT("OnClicked")},
		{TEXT("function_name"), TEXT("HandleClicked")},
	});
	FMCPToolResult FirstBind = Registry.ExecuteTool(
		TEXT("content.widget.event.bind"),
		BindParams);
	TestTrue(TEXT("OnClicked binding is created"), FirstBind.bSuccess);

	FMCPToolResult ReplayBind = Registry.ExecuteTool(
		TEXT("content.widget.event.bind"),
		BindParams);
	TestTrue(TEXT("Repeated binding succeeds"), ReplayBind.bSuccess);
	if (ReplayBind.bSuccess && ReplayBind.Data.IsValid())
	{
		TestTrue(
			TEXT("Repeated binding is idempotent"),
			ReplayBind.Data->GetBoolField(TEXT("already_bound")));
	}

	FMCPToolResult List = Registry.ExecuteTool(
		TEXT("content.widget.binding.list"),
		BindParams);
	TestTrue(TEXT("Binding list succeeds"), List.bSuccess);
	if (List.bSuccess && List.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Bindings =
			List.Data->GetArrayField(TEXT("bindings"));
		TestEqual(TEXT("Exactly one binding exists"), Bindings.Num(), 1);
		if (Bindings.Num() == 1)
		{
			const TSharedPtr<FJsonObject> Binding = Bindings[0]->AsObject();
			TestEqual(
				TEXT("Binding uses component-bound event representation"),
				Binding->GetStringField(TEXT("representation")),
				FString(TEXT("componentBoundEvent")));
			TestEqual(
				TEXT("Binding kind is Function"),
				Binding->GetStringField(TEXT("kind")),
				FString(TEXT("Function")));
			TestTrue(
				TEXT("Generated handler function exists"),
				Binding->GetBoolField(TEXT("functionExists")));
			TestTrue(
				TEXT("Handler signature matches"),
				Binding->GetBoolField(TEXT("signatureMatches")));
			TestTrue(
				TEXT("Generated component delegate binding exists"),
				Binding->GetBoolField(TEXT("generatedBinding")));
		}
	}

	FMCPToolResult MissingFunction = Registry.ExecuteTool(
		TEXT("content.widget.event.bind"),
		Params({
			{TEXT("widget_bp"), AssetPath},
			{TEXT("widget_name"), TEXT("TestButton")},
			{TEXT("event_name"), TEXT("OnPressed")},
			{TEXT("function_name"), TEXT("FunctionThatDoesNotExist")},
		}));
	TestFalse(
		TEXT("Missing handler function is rejected"),
		MissingFunction.bSuccess);
	TestEqual(
		TEXT("Missing handler uses stable error code"),
		MissingFunction.ErrorCode,
		FString(TEXT("function_not_found")));

	FMCPToolResult MissingList = Registry.ExecuteTool(
		TEXT("content.widget.binding.list"),
		Params({
			{TEXT("widget_bp"), AssetPath},
			{TEXT("widget_name"), TEXT("TestButton")},
			{TEXT("event_name"), TEXT("OnPressed")},
		}));
	if (MissingList.bSuccess && MissingList.Data.IsValid())
	{
		TestEqual(
			TEXT("Rejected handler does not write a binding"),
			static_cast<int32>(
				MissingList.Data->GetNumberField(TEXT("count"))),
			0);
	}

	TestTrue(TEXT("Widget Blueprint fixture is removed"), Cleanup());
	return true;
}

#endif
