#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationSubsystem.h"
#include "WidgetBlueprint.h"

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

UWidgetBlueprint* LoadWidgetFixture(const FString& AssetPath)
{
	const FString PackageName =
		FPackageName::ObjectPathToPackageName(AssetPath);
	const FString ObjectPath = FString::Printf(
		TEXT("%s.%s"),
		*PackageName,
		*FPackageName::GetShortName(PackageName));
	return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath);
}

int32 CountHandlerGraphs(
	const UWidgetBlueprint* WidgetBlueprint,
	const FName HandlerName)
{
	int32 Count = 0;
	if (WidgetBlueprint)
	{
		for (const UEdGraph* Graph :
			WidgetBlueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == HandlerName)
			{
				++Count;
			}
		}
	}
	return Count;
}

int32 CountMatchingEventNodes(
	UWidgetBlueprint* WidgetBlueprint,
	const FName WidgetName,
	const FName EventName)
{
	if (!WidgetBlueprint)
	{
		return 0;
	}
	TArray<UK2Node_ComponentBoundEvent*> Nodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(
		WidgetBlueprint,
		Nodes);
	int32 Count = 0;
	for (const UK2Node_ComponentBoundEvent* Node : Nodes)
	{
		if (Node
			&& Node->ComponentPropertyName == WidgetName
			&& Node->DelegatePropertyName == EventName)
		{
			++Count;
		}
	}
	return Count;
}

int32 CountHandlerCallNodes(
	UWidgetBlueprint* WidgetBlueprint,
	const FName HandlerName)
{
	if (!WidgetBlueprint)
	{
		return 0;
	}
	TArray<UK2Node_CallFunction*> Nodes;
	FBlueprintEditorUtils::GetAllNodesOfClass(
		WidgetBlueprint,
		Nodes);
	int32 Count = 0;
	for (const UK2Node_CallFunction* Node : Nodes)
	{
		if (Node
			&& Node->FunctionReference.GetMemberName()
				== HandlerName)
		{
			++Count;
		}
	}
	return Count;
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
			{TEXT("packagePath"), PackagePath},
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
			{TEXT("widgetBp"), AssetPath},
			{TEXT("childClass"), TEXT("Button")},
			{TEXT("childName"), TEXT("TestButton")},
			{TEXT("parentName"), TEXT("RootCanvas")},
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
		{TEXT("widgetBp"), AssetPath},
		{TEXT("widgetName"), TEXT("TestButton")},
		{TEXT("eventName"), TEXT("OnClicked")},
		{TEXT("functionName"), TEXT("HandleClicked")},
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
			{TEXT("widgetBp"), AssetPath},
			{TEXT("widgetName"), TEXT("TestButton")},
			{TEXT("eventName"), TEXT("OnPressed")},
			{TEXT("functionName"), TEXT("FunctionThatDoesNotExist")},
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
			{TEXT("widgetBp"), AssetPath},
			{TEXT("widgetName"), TEXT("TestButton")},
			{TEXT("eventName"), TEXT("OnPressed")},
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidgetEnsureHandlerSuccessTest,
	"UE_AI_integration.UMG.EnsureHandlerCreatesAndIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetEnsureHandlerSuccessTest::RunTest(
	const FString& Parameters)
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
		AddError(
			TEXT("UE_AI_integration subsystem or registry is unavailable."));
		return false;
	}
	FMCPToolRegistry& Registry = *Subsystem->GetRegistry();

	const FString AssetName = FString::Printf(
		TEXT("WBP_EnsureHandler_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = TEXT("/Game/Automation");
	const FString AssetPath =
		PackagePath + TEXT("/") + AssetName;
	auto Cleanup = [&AssetPath]()
	{
		return CleanupWidgetAsset(AssetPath);
	};
	Cleanup();

	FMCPToolResult Create = Registry.ExecuteTool(
		TEXT("content.widget.blueprint.create"),
		Params({
			{TEXT("name"), AssetName},
			{TEXT("packagePath"), PackagePath},
		}));
	TestTrue(
		TEXT("Widget Blueprint fixture is created"),
		Create.bSuccess);
	if (!Create.bSuccess)
	{
		Cleanup();
		return false;
	}

	FMCPToolResult AddSlider = Registry.ExecuteTool(
		TEXT("content.widget.child.add"),
		Params({
			{TEXT("widgetBp"), AssetPath},
			{TEXT("childClass"), TEXT("Slider")},
			{TEXT("childName"), TEXT("ValueSlider")},
			{TEXT("parentName"), TEXT("RootCanvas")},
		}));
	TestTrue(TEXT("Slider is added"), AddSlider.bSuccess);
	if (!AddSlider.bSuccess)
	{
		Cleanup();
		return false;
	}

	const TSharedPtr<FJsonObject> EnsureParams = Params({
		{TEXT("widgetBp"), AssetPath},
		{TEXT("widget"), TEXT("ValueSlider")},
		{TEXT("event"), TEXT("OnValueChanged")},
		{TEXT("handlerFunctionName"), TEXT("HandleValueChanged")},
	});
	FMCPToolResult First = Registry.ExecuteTool(
		TEXT("content.widget.event.ensure_handler"),
		EnsureParams);
	TestTrue(
		TEXT("Handler graph and binding are ensured"),
		First.bSuccess);
	if (!First.bSuccess || !First.Data.IsValid())
	{
		Cleanup();
		return false;
	}

	TestTrue(
		TEXT("First ensure changes the asset"),
		First.Data->GetBoolField(TEXT("changed")));
	TestTrue(
		TEXT("First ensure creates missing structure"),
		First.Data->GetBoolField(TEXT("created")));
	TestFalse(
		TEXT("Fresh creation is not reported as repair"),
		First.Data->GetBoolField(TEXT("repaired")));
	TestTrue(
		TEXT("First ensure compiles"),
		First.Data->GetBoolField(TEXT("compiled")));
	TestTrue(
		TEXT("First ensure verifies"),
		First.Data->GetBoolField(TEXT("verified")));

	const TSharedPtr<FJsonObject> FirstSignature =
		First.Data->GetObjectField(TEXT("signature"));
	const TArray<TSharedPtr<FJsonValue>>& SignatureParameters =
		FirstSignature->GetArrayField(TEXT("parameters"));
	TestEqual(
		TEXT("OnValueChanged exposes one delegate parameter"),
		SignatureParameters.Num(),
		1);
	if (SignatureParameters.Num() == 1)
	{
		TestEqual(
			TEXT("Delegate parameter name is preserved"),
			SignatureParameters[0]->AsObject()->GetStringField(
				TEXT("name")),
			FString(TEXT("Value")));
	}

	const TSharedPtr<FJsonObject> FirstEvidence =
		First.Data->GetObjectField(TEXT("evidence"));
	TestTrue(
		TEXT("Generated component binding exists"),
		FirstEvidence->GetBoolField(TEXT("generatedBinding")));
	TestTrue(
		TEXT("At least one valid exec edge exists"),
		FirstEvidence->GetNumberField(TEXT("execEdgeCount")) >= 1.0);
	TestEqual(
		TEXT("Delegate parameter is connected to handler call"),
		static_cast<int32>(
			FirstEvidence->GetNumberField(TEXT("parameterEdgeCount"))),
		1);

	UWidgetBlueprint* WidgetBlueprint =
		LoadWidgetFixture(AssetPath);
	TestNotNull(TEXT("Fixture loads for structural checks"), WidgetBlueprint);
	const int32 FunctionGraphsAfterFirst = CountHandlerGraphs(
		WidgetBlueprint,
		TEXT("HandleValueChanged"));
	const int32 EventNodesAfterFirst = CountMatchingEventNodes(
		WidgetBlueprint,
		TEXT("ValueSlider"),
		TEXT("OnValueChanged"));
	const int32 CallNodesAfterFirst = CountHandlerCallNodes(
		WidgetBlueprint,
		TEXT("HandleValueChanged"));
	TestEqual(
		TEXT("Exactly one handler graph exists"),
		FunctionGraphsAfterFirst,
		1);
	TestEqual(
		TEXT("Exactly one component event exists"),
		EventNodesAfterFirst,
		1);
	TestEqual(
		TEXT("Exactly one handler call exists"),
		CallNodesAfterFirst,
		1);

	FMCPToolResult Replay = Registry.ExecuteTool(
		TEXT("content.widget.event.ensure_handler"),
		EnsureParams);
	TestTrue(TEXT("Repeated ensure succeeds"), Replay.bSuccess);
	if (Replay.bSuccess && Replay.Data.IsValid())
	{
		TestFalse(
			TEXT("Repeated ensure is unchanged"),
			Replay.Data->GetBoolField(TEXT("changed")));
		TestFalse(
			TEXT("Repeated ensure creates nothing"),
			Replay.Data->GetBoolField(TEXT("created")));
		TestFalse(
			TEXT("Repeated ensure repairs nothing"),
			Replay.Data->GetBoolField(TEXT("repaired")));
		TestTrue(
			TEXT("Repeated ensure remains verified"),
			Replay.Data->GetBoolField(TEXT("verified")));
		const TSharedPtr<FJsonObject> ReplayEvidence =
			Replay.Data->GetObjectField(TEXT("evidence"));
		TestEqual(
			TEXT("Event node identity is stable"),
			ReplayEvidence->GetStringField(TEXT("eventNodeId")),
			FirstEvidence->GetStringField(TEXT("eventNodeId")));
		TestEqual(
			TEXT("Call node identity is stable"),
			ReplayEvidence->GetStringField(TEXT("callNodeId")),
			FirstEvidence->GetStringField(TEXT("callNodeId")));
	}

	WidgetBlueprint = LoadWidgetFixture(AssetPath);
	TestEqual(
		TEXT("Replay does not add handler graphs"),
		CountHandlerGraphs(
			WidgetBlueprint,
			TEXT("HandleValueChanged")),
		FunctionGraphsAfterFirst);
	TestEqual(
		TEXT("Replay does not add component events"),
		CountMatchingEventNodes(
			WidgetBlueprint,
			TEXT("ValueSlider"),
			TEXT("OnValueChanged")),
		EventNodesAfterFirst);
	TestEqual(
		TEXT("Replay does not add handler calls"),
		CountHandlerCallNodes(
			WidgetBlueprint,
			TEXT("HandleValueChanged")),
		CallNodesAfterFirst);

	TestTrue(TEXT("Widget Blueprint fixture is removed"), Cleanup());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWidgetEnsureHandlerSignatureMismatchTest,
	"UE_AI_integration.UMG.EnsureHandlerSignatureMismatchHasNoResidualMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWidgetEnsureHandlerSignatureMismatchTest::RunTest(
	const FString& Parameters)
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
		AddError(
			TEXT("UE_AI_integration subsystem or registry is unavailable."));
		return false;
	}
	FMCPToolRegistry& Registry = *Subsystem->GetRegistry();

	const FString AssetName = FString::Printf(
		TEXT("WBP_EnsureMismatch_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = TEXT("/Game/Automation");
	const FString AssetPath =
		PackagePath + TEXT("/") + AssetName;
	auto Cleanup = [&AssetPath]()
	{
		return CleanupWidgetAsset(AssetPath);
	};
	Cleanup();

	FMCPToolResult Create = Registry.ExecuteTool(
		TEXT("content.widget.blueprint.create"),
		Params({
			{TEXT("name"), AssetName},
			{TEXT("packagePath"), PackagePath},
		}));
	FMCPToolResult AddSlider = Registry.ExecuteTool(
		TEXT("content.widget.child.add"),
		Params({
			{TEXT("widgetBp"), AssetPath},
			{TEXT("childClass"), TEXT("Slider")},
			{TEXT("childName"), TEXT("ValueSlider")},
			{TEXT("parentName"), TEXT("RootCanvas")},
		}));
	FMCPToolResult AddWrongHandler = Registry.ExecuteTool(
		TEXT("blueprint.graph.create"),
		Params({
			{TEXT("blueprint"), AssetPath},
			{TEXT("graphName"), TEXT("HandleValueChanged")},
			{TEXT("graphType"), TEXT("function")},
		}));
	TestTrue(TEXT("Widget Blueprint fixture is created"), Create.bSuccess);
	TestTrue(TEXT("Slider is added"), AddSlider.bSuccess);
	TestTrue(
		TEXT("Mismatched zero-parameter handler is created"),
		AddWrongHandler.bSuccess);
	if (!Create.bSuccess
		|| !AddSlider.bSuccess
		|| !AddWrongHandler.bSuccess)
	{
		Cleanup();
		return false;
	}

	UWidgetBlueprint* WidgetBlueprint =
		LoadWidgetFixture(AssetPath);
	TestNotNull(TEXT("Fixture loads before mismatch test"), WidgetBlueprint);
	const int32 FunctionGraphsBefore = CountHandlerGraphs(
		WidgetBlueprint,
		TEXT("HandleValueChanged"));
	const int32 EventNodesBefore = CountMatchingEventNodes(
		WidgetBlueprint,
		TEXT("ValueSlider"),
		TEXT("OnValueChanged"));
	const int32 CallNodesBefore = CountHandlerCallNodes(
		WidgetBlueprint,
		TEXT("HandleValueChanged"));
	const bool bDirtyBefore =
		WidgetBlueprint
		&& WidgetBlueprint->GetOutermost()->IsDirty();

	FMCPToolResult Ensure = Registry.ExecuteTool(
		TEXT("content.widget.event.ensure_handler"),
		Params({
			{TEXT("widgetBp"), AssetPath},
			{TEXT("widget"), TEXT("ValueSlider")},
			{TEXT("event"), TEXT("OnValueChanged")},
			{TEXT("handlerFunctionName"), TEXT("HandleValueChanged")},
		}));
	TestFalse(
		TEXT("Mismatched handler signature is rejected"),
		Ensure.bSuccess);
	TestEqual(
		TEXT("Mismatch uses stable error code"),
		Ensure.ErrorCode,
		FString(TEXT("signature_mismatch")));

	WidgetBlueprint = LoadWidgetFixture(AssetPath);
	TestEqual(
		TEXT("Rejected ensure leaves handler graph count unchanged"),
		CountHandlerGraphs(
			WidgetBlueprint,
			TEXT("HandleValueChanged")),
		FunctionGraphsBefore);
	TestEqual(
		TEXT("Rejected ensure leaves event node count unchanged"),
		CountMatchingEventNodes(
			WidgetBlueprint,
			TEXT("ValueSlider"),
			TEXT("OnValueChanged")),
		EventNodesBefore);
	TestEqual(
		TEXT("Rejected ensure leaves call node count unchanged"),
		CountHandlerCallNodes(
			WidgetBlueprint,
			TEXT("HandleValueChanged")),
		CallNodesBefore);
	TestEqual(
		TEXT("Rejected ensure restores package dirty state"),
		WidgetBlueprint
			&& WidgetBlueprint->GetOutermost()->IsDirty(),
		bDirtyBefore);

	FMCPToolResult List = Registry.ExecuteTool(
		TEXT("content.widget.binding.list"),
		Params({
			{TEXT("widgetBp"), AssetPath},
			{TEXT("widgetName"), TEXT("ValueSlider")},
			{TEXT("eventName"), TEXT("OnValueChanged")},
		}));
	TestTrue(TEXT("Binding list succeeds"), List.bSuccess);
	if (List.bSuccess && List.Data.IsValid())
	{
		TestEqual(
			TEXT("Rejected ensure leaves no binding"),
			static_cast<int32>(
				List.Data->GetNumberField(TEXT("count"))),
			0);
	}

	TestTrue(TEXT("Widget Blueprint fixture is removed"), Cleanup());
	return true;
}

#endif
