#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry);
}

namespace
{
struct FBlueprintGraphDuplicateFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* EventGraph = nullptr;

	bool Build(const FString& Tag)
	{
		PackageName = TEXT("/Game/Automation/UEAI_GraphDuplicate_") + Tag
			+ TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(PackageName);
		Package = CreatePackage(*PackageName);
		Blueprint = Package
			? FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				Package,
				*AssetName,
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass(),
				FName(TEXT("UEAI.BlueprintGraphDuplicateContract")))
			: nullptr;
		EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
			? Blueprint->UbergraphPages[0]
			: nullptr;
		return Blueprint != nullptr && EventGraph != nullptr;
	}

	void Delete()
	{
		if (!PackageName.IsEmpty()
			&& UEditorAssetLibrary::DoesAssetExist(PackageName))
		{
			UEditorAssetLibrary::DeleteAsset(PackageName);
		}
	}
};

TSharedRef<FJsonObject> BlueprintGraphDuplicateMakeParams(
	const FString& Blueprint,
	const FString& Graph,
	const FString& NewName = FString())
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	if (!Graph.IsEmpty())
	{
		Params->SetStringField(TEXT("graph"), Graph);
	}
	if (!NewName.IsEmpty())
	{
		Params->SetStringField(TEXT("newName"), NewName);
	}
	return Params;
}

int32 BlueprintGraphDuplicateNonNullNodeCount(UEdGraph* Graph)
{
	int32 Count = 0;
	if (Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				++Count;
			}
		}
	}
	return Count;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintGraphDuplicateContractTest,
	"UE_AI_integration.Blueprint.GraphDuplicateContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintGraphDuplicateContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();

	// (a) Unknown blueprint -> blueprint_not_found (404).
	{
		const FString UnknownBlueprint = FString::Printf(
			TEXT("/Game/Automation/UEAI_GraphDuplicateMissing_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.duplicate"),
			BlueprintGraphDuplicateMakeParams(UnknownBlueprint, FString()));
		TestFalse(TEXT("Unknown blueprint duplicate fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown blueprint has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("blueprint_not_found")));
		TestEqual(TEXT("Unknown blueprint returns 404"), Result.HttpStatus, 404);
	}

	// (b) Unknown graph -> graph_not_found (404).
	{
		FBlueprintGraphDuplicateFixture Fixture;
		if (!Fixture.Build(TEXT("UnknownGraph")))
		{
			AddError(TEXT("Could not create the unknown-graph Blueprint fixture."));
			return false;
		}
		ON_SCOPE_EXIT
		{
			Fixture.Delete();
		};
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.duplicate"),
			BlueprintGraphDuplicateMakeParams(
				Fixture.Blueprint->GetPathName(),
				FString::Printf(
					TEXT("NoSuchGraph_%s"),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		TestFalse(TEXT("Unknown graph duplicate fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown graph has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("graph_not_found")));
		TestEqual(TEXT("Unknown graph returns 404"), Result.HttpStatus, 404);
	}

	// (c) Transient /Game/ Blueprint: duplicate the primary event graph.
	{
		FBlueprintGraphDuplicateFixture Fixture;
		if (!Fixture.Build(TEXT("Transient")))
		{
			AddInfo(
				TEXT("Blueprint creation is unavailable in this context; skipping the transient duplicate assertion."));
		}
		else
		{
			ON_SCOPE_EXIT
			{
				Fixture.Delete();
			};

			// Seed the source graph with one self-contained comment node so the
			// node-count assertion is non-trivial and the clone is exercised.
			UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(
				Fixture.EventGraph, NAME_None, RF_Transactional);
			Comment->NodeComment = TEXT("GraphDuplicateProbe");
			Comment->NodePosX = 0;
			Comment->NodePosY = 0;
			Fixture.EventGraph->AddNode(Comment, false, false);
			Comment->AllocateDefaultPins();

			const int32 SourceNodeCount =
				BlueprintGraphDuplicateNonNullNodeCount(Fixture.EventGraph);

			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("blueprint.graph.duplicate"),
				BlueprintGraphDuplicateMakeParams(
					Fixture.Blueprint->GetPathName(), FString()));
			if (TestTrue(TEXT("Transient duplicate succeeds"), Result.bSuccess)
				&& TestNotNull(TEXT("Transient duplicate data"), Result.Data.Get()))
			{
				TestEqual(
					TEXT("Duplicate schema is the graph-duplicate schema"),
					Result.Data->GetStringField(TEXT("schema")),
					FString(TEXT("ue.blueprint.graph-duplicate.v1")));
				TestEqual(
					TEXT("Duplicate node count matches the source graph"),
					Result.Data->GetIntegerField(TEXT("nodeCount")),
					SourceNodeCount);
				const FString NewGraphPath =
					Result.Data->GetStringField(TEXT("newGraph"));
				TestTrue(
					TEXT("Duplicate reports a new graph object path"),
					!NewGraphPath.IsEmpty()
						&& NewGraphPath != Fixture.EventGraph->GetPathName());
				UEdGraph* ReadBackGraph =
					FindObject<UEdGraph>(nullptr, *NewGraphPath);
				TestNotNull(
					TEXT("Duplicated graph exists in memory"),
					ReadBackGraph);
				TestFalse(
					TEXT("Duplicate never saves the package"),
					Result.Data->GetBoolField(TEXT("saved")));
				TestTrue(
					TEXT("Duplicate reports compiled state"),
					Result.Data->GetBoolField(TEXT("compiled")));
			}
		}
	}

	return true;
}

#endif
