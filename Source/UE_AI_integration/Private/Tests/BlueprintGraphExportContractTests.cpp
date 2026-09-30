#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintGraphExportFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
};

FBlueprintGraphExportFixture BlueprintGraphExportCreateFixture(
	const FString& Prefix)
{
	FBlueprintGraphExportFixture Fixture;
	Fixture.PackageName = TEXT("/Game/Automation/") + Prefix + TEXT("_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(Fixture.PackageName);
	Fixture.Package = CreatePackage(*Fixture.PackageName);
	Fixture.Blueprint = Fixture.Package
		? FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Fixture.Package,
			*AssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UEAI.BlueprintGraphExportContract")))
		: nullptr;
	Fixture.Graph = Fixture.Blueprint
		&& !Fixture.Blueprint->UbergraphPages.IsEmpty()
		? Fixture.Blueprint->UbergraphPages[0]
		: nullptr;
	return Fixture;
}

bool BlueprintGraphExportCleanupFixture(const FString& PackageName)
{
	// The fixture is never saved to disk, so an in-memory /Game/Automation
	// package simply has no asset to delete; the existing on-disk cleanup
	// contract still applies when a package does exist.
	return !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
}

TSharedRef<FJsonObject> BlueprintGraphExportMakeParams(
	const FString& Blueprint,
	const FString& Graph)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	if (!Graph.IsEmpty())
	{
		Params->SetStringField(TEXT("graph"), Graph);
	}
	return Params;
}

TSharedPtr<FJsonObject> BlueprintGraphExportFindNode(
	const TArray<TSharedPtr<FJsonValue>>& Nodes,
	const FString& NodeId)
{
	for (const TSharedPtr<FJsonValue>& Value : Nodes)
	{
		const TSharedPtr<FJsonObject> Object =
			Value.IsValid() ? Value->AsObject() : nullptr;
		if (Object.IsValid())
		{
			FString Id;
			if (Object->TryGetStringField(TEXT("id"), Id) && Id == NodeId)
			{
				return Object;
			}
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintGraphExportContractTest,
	"UE_AI_integration.Blueprint.GraphExportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintGraphExportContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(Registry);
	Registry.EndDomainRegistration();

	// (a) Unknown blueprint path -> blueprint_not_found (404).
	{
		const FString UnknownBlueprint = FString::Printf(
			TEXT("/Game/Automation/UEAI_GraphExportMissing_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.export"),
			BlueprintGraphExportMakeParams(UnknownBlueprint, FString()));
		TestFalse(TEXT("Unknown blueprint export fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown blueprint has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("blueprint_not_found")));
		TestEqual(TEXT("Unknown blueprint returns 404"), Result.HttpStatus, 404);
	}

	// (b) Unknown graph -> graph_not_found (404).
	{
		const FBlueprintGraphExportFixture Fixture =
			BlueprintGraphExportCreateFixture(TEXT("UEAI_GraphExportUnknownGraph"));
		ON_SCOPE_EXIT
		{
			BlueprintGraphExportCleanupFixture(Fixture.PackageName);
		};
		if (TestNotNull(TEXT("Unknown-graph Blueprint fixture"), Fixture.Blueprint))
		{
			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("blueprint.graph.export"),
				BlueprintGraphExportMakeParams(
					Fixture.Blueprint->GetPathName(),
					FString::Printf(
						TEXT("NoSuchGraph_%s"),
						*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
			TestFalse(TEXT("Unknown graph export fails"), Result.bSuccess);
			TestEqual(
				TEXT("Unknown graph has a stable error code"),
				Result.ErrorCode,
				FString(TEXT("graph_not_found")));
			TestEqual(TEXT("Unknown graph returns 404"), Result.HttpStatus, 404);
		}
	}

	// (c) Transient Blueprint export of its primary event graph.
	{
		const FBlueprintGraphExportFixture Fixture =
			BlueprintGraphExportCreateFixture(TEXT("UEAI_GraphExportTransient"));
		ON_SCOPE_EXIT
		{
			BlueprintGraphExportCleanupFixture(Fixture.PackageName);
		};
		if (!Fixture.Blueprint || !Fixture.Graph)
		{
			AddInfo(
				TEXT("Blueprint creation is unavailable in this context; skipping the transient export assertion."));
		}
		else
		{
			UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(
				Fixture.Graph, NAME_None, RF_Transactional);
			EventNode->CustomFunctionName = FName(TEXT("GraphExportProbe"));
			EventNode->NodeGuid = FGuid(0, 0, 0, 1);
			Fixture.Graph->AddNode(EventNode, false, false);
			EventNode->AllocateDefaultPins();

			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("blueprint.graph.export"),
				BlueprintGraphExportMakeParams(
					Fixture.Blueprint->GetPathName(), FString()));
			if (TestTrue(TEXT("Transient export succeeds"), Result.bSuccess)
				&& TestNotNull(TEXT("Transient export data"), Result.Data.Get()))
			{
				TestEqual(
					TEXT("Export schema is the graph-export schema"),
					Result.Data->GetStringField(TEXT("schema")),
					FString(TEXT("ue.blueprint.graph-export.v1")));
				TestTrue(
					TEXT("Export reports at least one node"),
					Result.Data->GetIntegerField(TEXT("nodeCount")) >= 1);
				const TArray<TSharedPtr<FJsonValue>>& Nodes =
					Result.Data->GetArrayField(TEXT("nodes"));
				const TSharedPtr<FJsonObject> EventRow =
					BlueprintGraphExportFindNode(
						Nodes, EventNode->NodeGuid.ToString());
				if (TestNotNull(TEXT("Probe event node is exported"), EventRow.Get()))
				{
					TestEqual(
						TEXT("Probe node keeps its stable GUID identity"),
						EventRow->GetStringField(TEXT("id")),
						EventNode->NodeGuid.ToString());
					TestEqual(
						TEXT("Probe node reports its class name"),
						EventRow->GetStringField(TEXT("class")),
						FString(TEXT("K2Node_CustomEvent")));
				}
				TestFalse(
					TEXT("Read-only export never saves"),
					Result.Data->GetBoolField(TEXT("saved")));
				TestFalse(
					TEXT("Read-only export never compiles"),
					Result.Data->GetBoolField(TEXT("compiled")));
			}
		}
	}

	return true;
}

#endif
