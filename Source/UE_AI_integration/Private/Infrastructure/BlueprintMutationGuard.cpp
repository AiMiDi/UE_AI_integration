#include "Infrastructure/BlueprintMutationGuard.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Infrastructure/Sha256.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
int32 ObjectOuterDepth(const UObject* Object)
{
	int32 Depth = 0;
	for (const UObject* Outer = Object; Outer; Outer = Outer->GetOuter())
	{
		++Depth;
	}
	return Depth;
}

TArray<UObject*> GatherBlueprintGraphObjects(UBlueprint* Blueprint)
{
	TArray<UObject*> Objects;
	if (!Blueprint)
	{
		return Objects;
	}
	Objects.Add(Blueprint);
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (!Graph)
		{
			continue;
		}
		Objects.AddUnique(Graph);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Objects.AddUnique(Node);
			}
		}
	}
	Objects.Sort([](const UObject& Left, const UObject& Right)
	{
		const int32 LeftDepth = ObjectOuterDepth(&Left);
		const int32 RightDepth = ObjectOuterDepth(&Right);
		return LeftDepth == RightDepth
			? Left.GetPathName() < Right.GetPathName()
			: LeftDepth < RightDepth;
	});
	return Objects;
}

FString GraphStateDigest(UBlueprint* Blueprint)
{
	if (!Blueprint)
	{
		return FString();
	}
	TArray<UEdGraph*> Graphs;
	Blueprint->GetAllGraphs(Graphs);
	Graphs.RemoveAll([](const UEdGraph* Graph) { return Graph == nullptr; });
	Graphs.Sort([](const UEdGraph& Left, const UEdGraph& Right)
	{
		return Left.GetPathName() < Right.GetPathName();
	});
	FString State;
	for (UEdGraph* Graph : Graphs)
	{
		State += TEXT("G|") + Graph->GetPathName() + TEXT("\n");
		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(Node);
			}
		}
		Nodes.Sort([](const UEdGraphNode& Left, const UEdGraphNode& Right)
		{
			return Left.NodeGuid.ToString() < Right.NodeGuid.ToString();
		});
		for (UEdGraphNode* Node : Nodes)
		{
			State += FString::Printf(
				TEXT("N|%s|%s|%d|%d|%d|%d|%s\n"),
				*Node->NodeGuid.ToString(),
				*Node->GetClass()->GetPathName(),
				Node->NodePosX,
				Node->NodePosY,
				Node->NodeWidth,
				Node->NodeHeight,
				*Node->NodeComment);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				State += FString::Printf(
					TEXT("P|%s|%d|%s|%s\n"),
					*Pin->PinName.ToString(),
					static_cast<int32>(Pin->Direction),
					*Pin->PinType.PinCategory.ToString(),
					*Pin->DefaultValue);
				TArray<FString> Links;
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						Links.Add(
							Linked->GetOwningNode()->NodeGuid.ToString()
							+ TEXT("|") + Linked->PinName.ToString());
					}
				}
				Links.Sort();
				for (const FString& Link : Links)
				{
					State += TEXT("L|") + Link + TEXT("\n");
				}
			}
		}
	}
	FTCHARToUTF8 Utf8(*State);
	FString Digest;
	return TrySha256Hex(Utf8.Get(), Utf8.Length(), Digest)
		? TEXT("sha256:") + Digest
		: FString();
}
}

FBlueprintSingleRequestMutationGuard::FBlueprintSingleRequestMutationGuard(
	UBlueprint* InBlueprint)
	: Blueprint(InBlueprint)
{
	if (!InBlueprint)
	{
		ErrorCode = TEXT("target_not_found");
		ErrorMessage = TEXT("Blueprint mutation target is invalid.");
		return;
	}
	FBlueprintPersistenceTarget Target;
	FBlueprintPersistenceError PersistenceError;
	if (!ResolveBlueprintPersistenceTarget(
		InBlueprint,
		Target,
		PersistenceError))
	{
		ErrorCode = PersistenceError.Code;
		ErrorMessage = PersistenceError.Message;
		return;
	}
	PackageFilename = Target.Filename;
	bPackageWasDirty = Target.Package && Target.Package->IsDirty();
	GraphDigestBefore = GraphStateDigest(InBlueprint);
	if (GraphDigestBefore.IsEmpty())
	{
		ErrorCode = TEXT("snapshot_unavailable");
		ErrorMessage = TEXT("Could not fingerprint the Blueprint graph before mutation.");
		return;
	}
	for (UObject* Object : GatherBlueprintGraphObjects(InBlueprint))
	{
		FObjectSnapshot Snapshot(Object);
		Snapshot.OuterDepth = ObjectOuterDepth(Object);
		FObjectWriter Writer(
			Object,
			Snapshot.Bytes,
			false,
			false,
			false,
			PPF_DuplicateVerbatim);
		ObjectSnapshots.Add(MoveTemp(Snapshot));
	}
	if (ObjectSnapshots.IsEmpty())
	{
		ErrorCode = TEXT("snapshot_unavailable");
		ErrorMessage = TEXT("Could not snapshot the Blueprint before mutation.");
		return;
	}
	bDiskFileExisted = IFileManager::Get().FileExists(*PackageFilename);
	if (bDiskFileExisted)
	{
		const FString BaselineRoot = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UEAIIntegration"),
			TEXT("MutationGuards"));
		if (!IFileManager::Get().MakeDirectory(*BaselineRoot, true))
		{
			ErrorCode = TEXT("snapshot_unavailable");
			ErrorMessage = TEXT("Could not create the direct-mutation snapshot directory.");
			return;
		}
		DiskBaselineFilename = FPaths::Combine(
			BaselineRoot,
			FGuid::NewGuid().ToString(EGuidFormats::Digits)
				+ FPaths::GetExtension(PackageFilename, true));
		if (IFileManager::Get().Copy(
			*DiskBaselineFilename,
			*PackageFilename,
			true,
			true) != COPY_OK)
		{
			ErrorCode = TEXT("snapshot_unavailable");
			ErrorMessage = TEXT("Could not capture the package baseline before mutation.");
			CleanupDiskBaseline();
			return;
		}
	}
	bValid = true;
}

FBlueprintSingleRequestMutationGuard::~FBlueprintSingleRequestMutationGuard()
{
	if (bValid && bMutationStarted && !bCommitted && !bRolledBack)
	{
		FString Ignored;
		Rollback(Ignored);
	}
	CleanupDiskBaseline();
}

void FBlueprintSingleRequestMutationGuard::Commit()
{
	bCommitted = true;
	CleanupDiskBaseline();
}

bool FBlueprintSingleRequestMutationGuard::Rollback(FString& OutError)
{
	if (!bValid || bRolledBack || !Blueprint.IsValid())
	{
		OutError = TEXT("The direct Blueprint mutation snapshot is unavailable.");
		return false;
	}
	UBlueprint* TargetBlueprint = Blueprint.Get();
	const TArray<UObject*> ObjectsAfter = GatherBlueprintGraphObjects(
		TargetBlueprint);
	TSet<UObject*> OriginalObjects;
	for (const FObjectSnapshot& Snapshot : ObjectSnapshots)
	{
		if (Snapshot.Object.IsValid())
		{
			OriginalObjects.Add(Snapshot.Object.Get());
		}
	}
	bool bRestored = true;
	for (int32 Index = ObjectSnapshots.Num() - 1; Index >= 0; --Index)
	{
		if (ObjectSnapshots[Index].Object.IsValid())
		{
			ObjectSnapshots[Index].Object->PreEditUndo();
		}
	}
	for (int32 Index = ObjectSnapshots.Num() - 1; Index >= 0; --Index)
	{
		UObject* Object = ObjectSnapshots[Index].Object.Get();
		if (!::IsValid(Object))
		{
			bRestored = false;
			continue;
		}
		FObjectReader Reader(ObjectSnapshots[Index].Bytes);
		Reader.SetPortFlags(PPF_DuplicateVerbatim);
		Object->Serialize(Reader);
		bRestored = bRestored && !Reader.IsError();
	}
	for (int32 Index = ObjectSnapshots.Num() - 1; Index >= 0; --Index)
	{
		if (UObject* Object = ObjectSnapshots[Index].Object.Get())
		{
			if (Object != TargetBlueprint && ::IsValid(Object))
			{
				Object->PostEditUndo();
			}
		}
	}
	for (UObject* Object : ObjectsAfter)
	{
		if (Object && Object != TargetBlueprint
			&& !OriginalObjects.Contains(Object) && ::IsValid(Object))
		{
			Object->MarkAsGarbage();
		}
	}
	TargetBlueprint->PostEditUndo();
	TArray<UEdGraph*> Graphs;
	TargetBlueprint->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph)
		{
			Graph->NotifyGraphChanged();
		}
	}
	FKismetEditorUtilities::CompileBlueprint(
		TargetBlueprint,
		EBlueprintCompileOptions::SkipSave);
	if (UPackage* Package = TargetBlueprint->GetOutermost())
	{
		Package->SetDirtyFlag(bPackageWasDirty);
	}
	if (bDiskFileExisted && !DiskBaselineFilename.IsEmpty())
	{
		const FString RestoreTemporary = PackageFilename
			+ TEXT(".ueai-restore-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const bool bCopied = IFileManager::Get().Copy(
			*RestoreTemporary,
			*DiskBaselineFilename,
			true,
			true) == COPY_OK;
		const bool bMoved = bCopied && IFileManager::Get().Move(
			*PackageFilename,
			*RestoreTemporary,
			true,
			true,
			false,
			true);
		if (!bMoved)
		{
			IFileManager::Get().Delete(*RestoreTemporary, false, true);
			bRestored = false;
		}
	}
	else if (!bDiskFileExisted && IFileManager::Get().FileExists(*PackageFilename))
	{
		bRestored = IFileManager::Get().Delete(
			*PackageFilename,
			false,
			true) && bRestored;
	}
	bRestored = bRestored
		&& GraphStateDigest(TargetBlueprint) == GraphDigestBefore
		&& (!TargetBlueprint->GetOutermost()
			|| TargetBlueprint->GetOutermost()->IsDirty() == bPackageWasDirty);
	bRolledBack = true;
	if (!bRestored)
	{
		OutError = TEXT("The direct Blueprint mutation could not be fully restored and verified.");
	}
	CleanupDiskBaseline();
	return bRestored;
}

void FBlueprintSingleRequestMutationGuard::CleanupDiskBaseline()
{
	if (!DiskBaselineFilename.IsEmpty())
	{
		IFileManager::Get().Delete(*DiskBaselineFilename, false, true);
		DiskBaselineFilename.Reset();
	}
}
}
