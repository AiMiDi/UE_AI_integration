#if WITH_DEV_AUTOMATION_TESTS

#include "EditorAssetLibrary.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
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
struct FBlueprintGraphCopyNodesFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UBlueprint* Blueprint = nullptr;
	UEdGraph* EventGraph = nullptr;
	UEdGraph* SecondGraph = nullptr;

	bool Build(const FString& Tag, const bool bWithSecondGraph)
	{
		PackageName = TEXT("/Game/Automation/UEAI_GraphCopyNodes_") + Tag
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
				FName(TEXT("UEAI.BlueprintGraphCopyNodesContract")))
			: nullptr;
		EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
			? Blueprint->UbergraphPages[0]
			: nullptr;
		if (bWithSecondGraph && Blueprint)
		{
			SecondGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint,
				FName(TEXT("GraphCopyNodesTarget")),
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			if (SecondGraph)
			{
				Blueprint->UbergraphPages.Add(SecondGraph);
			}
		}
		return Blueprint != nullptr && EventGraph != nullptr
			&& (!bWithSecondGraph || SecondGraph != nullptr);
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

TSharedRef<FJsonObject> BlueprintGraphCopyNodesMakeParams(
	const FString& Blueprint,
	const FString& SourceGraph,
	const FString& TargetGraph)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("blueprint"), Blueprint);
	Params->SetStringField(TEXT("sourceGraph"), SourceGraph);
	Params->SetStringField(TEXT("targetGraph"), TargetGraph);
	return Params;
}

int32 BlueprintGraphCopyNodesNonNullNodeCount(UEdGraph* Graph)
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

UEdGraphNode_Comment* BlueprintGraphCopyNodesAddComment(
	UEdGraph* Graph,
	const FString& Text)
{
	if (!Graph)
	{
		return nullptr;
	}
	UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(
		Graph,
		NAME_None,
		RF_Transactional);
	Comment->NodeComment = Text;
	Comment->NodePosX = 0;
	Comment->NodePosY = 0;
	Graph->AddNode(Comment, false, false);
	Comment->AllocateDefaultPins();
	return Comment;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintGraphCopyNodesContractTest,
	"UE_AI_integration.Blueprint.GraphCopyNodesContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintGraphCopyNodesContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();

	// (a) Unknown blueprint -> blueprint_not_found (404).
	{
		const FString UnknownBlueprint = FString::Printf(
			TEXT("/Game/Automation/UEAI_GraphCopyNodesMissing_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.copy_nodes"),
			BlueprintGraphCopyNodesMakeParams(
				UnknownBlueprint,
				TEXT("EventGraph"),
				TEXT("EventGraph")));
		TestFalse(TEXT("Unknown blueprint copy fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown blueprint has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("blueprint_not_found")));
		TestEqual(TEXT("Unknown blueprint returns 404"), Result.HttpStatus, 404);
	}

	// (b) Unknown source graph -> graph_not_found (404).
	{
		FBlueprintGraphCopyNodesFixture Fixture;
		if (!Fixture.Build(TEXT("UnknownSource"), true))
		{
			AddError(TEXT("Could not create the unknown-source Blueprint fixture."));
			return false;
		}
		ON_SCOPE_EXIT
		{
			Fixture.Delete();
		};
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.copy_nodes"),
			BlueprintGraphCopyNodesMakeParams(
				Fixture.Blueprint->GetPathName(),
				FString::Printf(
					TEXT("NoSuchSourceGraph_%s"),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits)),
				Fixture.EventGraph->GetName()));
		TestFalse(TEXT("Unknown source graph copy fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown source graph has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("graph_not_found")));
		TestEqual(TEXT("Unknown source graph returns 404"), Result.HttpStatus, 404);
	}

	// (b2) Unknown target graph -> graph_not_found (404).
	{
		FBlueprintGraphCopyNodesFixture Fixture;
		if (!Fixture.Build(TEXT("UnknownTarget"), true))
		{
			AddError(TEXT("Could not create the unknown-target Blueprint fixture."));
			return false;
		}
		ON_SCOPE_EXIT
		{
			Fixture.Delete();
		};
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.copy_nodes"),
			BlueprintGraphCopyNodesMakeParams(
				Fixture.Blueprint->GetPathName(),
				Fixture.EventGraph->GetName(),
				FString::Printf(
					TEXT("NoSuchTargetGraph_%s"),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		TestFalse(TEXT("Unknown target graph copy fails"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown target graph has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("graph_not_found")));
		TestEqual(TEXT("Unknown target graph returns 404"), Result.HttpStatus, 404);
	}

	// (c) Self-copy -> graph_copy_self (422).
	{
		FBlueprintGraphCopyNodesFixture Fixture;
		if (!Fixture.Build(TEXT("SelfCopy"), false))
		{
			AddError(TEXT("Could not create the self-copy Blueprint fixture."));
			return false;
		}
		ON_SCOPE_EXIT
		{
			Fixture.Delete();
		};
		const FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("blueprint.graph.copy_nodes"),
			BlueprintGraphCopyNodesMakeParams(
				Fixture.Blueprint->GetPathName(),
				Fixture.EventGraph->GetName(),
				Fixture.EventGraph->GetName()));
		TestFalse(TEXT("Self-copy fails"), Result.bSuccess);
		TestEqual(
			TEXT("Self-copy has a stable error code"),
			Result.ErrorCode,
			FString(TEXT("graph_copy_self")));
		TestEqual(TEXT("Self-copy returns 422"), Result.HttpStatus, 422);
	}

	// (d) Two event graphs: copy the source graph's nodes into the target and
	// assert the target node count increases by the source's non-null count.
	{
		FBlueprintGraphCopyNodesFixture Fixture;
		if (!Fixture.Build(TEXT("CopyNodes"), true))
		{
			AddInfo(
				TEXT("Blueprint creation is unavailable in this context; skipping the copy assertion."));
		}
		else
		{
			ON_SCOPE_EXIT
			{
				Fixture.Delete();
			};

			BlueprintGraphCopyNodesAddComment(
				Fixture.EventGraph,
				TEXT("GraphCopyNodesSource"));
			BlueprintGraphCopyNodesAddComment(
				Fixture.SecondGraph,
				TEXT("GraphCopyNodesTarget"));

			const int32 SourceNodeCount =
				BlueprintGraphCopyNodesNonNullNodeCount(Fixture.SecondGraph);
			const int32 TargetNodeCountBefore =
				BlueprintGraphCopyNodesNonNullNodeCount(Fixture.EventGraph);

			const FMCPToolResult Result = Registry.ExecuteTool(
				TEXT("blueprint.graph.copy_nodes"),
				BlueprintGraphCopyNodesMakeParams(
					Fixture.Blueprint->GetPathName(),
					Fixture.SecondGraph->GetName(),
					Fixture.EventGraph->GetName()));
			if (TestTrue(TEXT("Copy nodes succeeds"), Result.bSuccess)
				&& TestNotNull(TEXT("Copy nodes data"), Result.Data.Get()))
			{
				TestEqual(
					TEXT("Copy schema is the graph-copy-nodes schema"),
					Result.Data->GetStringField(TEXT("schema")),
					FString(TEXT("ue.blueprint.graph-copy-nodes.v1")));
				TestEqual(
					TEXT("copiedNodeCount matches the source graph"),
					Result.Data->GetIntegerField(TEXT("copiedNodeCount")),
					SourceNodeCount);
				TestEqual(
					TEXT("targetNodeCount increased by the source node count"),
					Result.Data->GetIntegerField(TEXT("targetNodeCount")),
					TargetNodeCountBefore + SourceNodeCount);
				TestTrue(
					TEXT("Copy reports compiled state"),
					Result.Data->GetBoolField(TEXT("compiled")));
				TestFalse(
					TEXT("Copy never saves the package"),
					Result.Data->GetBoolField(TEXT("saved")));
				const int32 TargetNodeCountAfter =
					BlueprintGraphCopyNodesNonNullNodeCount(Fixture.EventGraph);
				TestEqual(
					TEXT("In-memory target graph gained the source nodes"),
					TargetNodeCountAfter,
					TargetNodeCountBefore + SourceNodeCount);
			}
		}
	}

	return true;
}

#endif
