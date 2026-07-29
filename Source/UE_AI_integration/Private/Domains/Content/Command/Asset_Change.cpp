// Plan-gated generic asset changes. These commands are intentionally not
// admitted to UE Workflow DSL: they may span packages or invoke import tools.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/EngineeringContractUtils.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EditorReimportHandler.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace
{
using UEAIIntegration::Infrastructure::DigestJson;
using UEAIIntegration::Infrastructure::MakeStableId;
using UEAIIntegration::Infrastructure::SetBoundedArray;

constexpr int32 MaxAssetActions = 100;
constexpr int32 MaxReceipts = 32;

struct FReverseAction
{
	enum class EKind
	{
		DeleteCreated,
		RenameBack,
		RestoreDeleted,
	};

	EKind Kind = EKind::DeleteCreated;
	FString Source;
	FString Destination;
	UObject* Backup = nullptr;
};

struct FAssetChangeReceipt
{
	FString ReceiptId;
	FString PlanDigest;
	FString RequestId;
	FDateTime CreatedAt;
	bool bRollbackAvailable = true;
	bool bRolledBack = false;
	TArray<FReverseAction> ReverseActions;
	TArray<FString> RollbackBlockers;
};

TMap<FString, FAssetChangeReceipt>& GetReceipts()
{
	static TMap<FString, FAssetChangeReceipt> Receipts;
	return Receipts;
}

void ReleaseReceiptBackups(FAssetChangeReceipt& Receipt)
{
	for (FReverseAction& Reverse : Receipt.ReverseActions)
	{
		if (Reverse.Backup && Reverse.Backup->IsRooted())
		{
			Reverse.Backup->RemoveFromRoot();
		}
		Reverse.Backup = nullptr;
	}
}

void EvictOldReceipts()
{
	TMap<FString, FAssetChangeReceipt>& Receipts = GetReceipts();
	while (Receipts.Num() >= MaxReceipts)
	{
		FString OldestId;
		FDateTime Oldest = FDateTime::MaxValue();
		for (const TPair<FString, FAssetChangeReceipt>& Pair : Receipts)
		{
			if (Pair.Value.CreatedAt < Oldest)
			{
				Oldest = Pair.Value.CreatedAt;
				OldestId = Pair.Key;
			}
		}
		if (FAssetChangeReceipt* OldestReceipt = Receipts.Find(OldestId))
		{
			ReleaseReceiptBackups(*OldestReceipt);
		}
		Receipts.Remove(OldestId);
	}
}

UEditorAssetSubsystem* GetAssetSubsystem()
{
	return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
}

bool IsGamePackagePath(const FString& Path)
{
	return Path.StartsWith(TEXT("/Game/"))
		&& !Path.Contains(TEXT(".."))
		&& FPackageName::IsValidLongPackageName(Path);
}

bool IsUnderPackagePath(const FString& Package, const FString& Root)
{
	return Package.Equals(Root, ESearchCase::IgnoreCase)
		|| Package.StartsWith(
			Root.EndsWith(TEXT("/")) ? Root : Root + TEXT("/"),
			ESearchCase::IgnoreCase);
}

bool ResolveAssetData(
	IAssetRegistry& Registry,
	const FString& Path,
	FAssetData& OutData)
{
	if (Path.Contains(TEXT(".")))
	{
		OutData = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
		if (OutData.IsValid())
		{
			return true;
		}
	}
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(FName(*Path), Assets);
	if (!Assets.IsEmpty())
	{
		OutData = Assets[0];
		return true;
	}
	return false;
}

FString ObjectPathForPackage(const FString& PackagePath)
{
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	return FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
}

TSharedRef<FJsonObject> AssetState(
	IAssetRegistry& Registry,
	const FString& Path)
{
	TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("requestedPath"), Path);
	FAssetData Data;
	if (!ResolveAssetData(Registry, Path, Data))
	{
		State->SetBoolField(TEXT("exists"), false);
		return State;
	}
	State->SetBoolField(TEXT("exists"), true);
	State->SetStringField(TEXT("packageName"), Data.PackageName.ToString());
	State->SetStringField(TEXT("objectPath"), Data.GetObjectPathString());
	State->SetStringField(TEXT("classPath"), Data.AssetClassPath.ToString());
	State->SetBoolField(TEXT("redirector"), Data.IsRedirector());
	FString Filename;
	if (FPackageName::DoesPackageExist(Data.PackageName.ToString(), &Filename))
	{
		State->SetNumberField(
			TEXT("diskSizeBytes"),
			static_cast<double>(IFileManager::Get().FileSize(*Filename)));
		State->SetStringField(
			TEXT("fileTimestamp"),
			IFileManager::Get().GetTimeStamp(*Filename).ToIso8601());
	}
	return State;
}

bool ReadRequiredString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	FString& Out,
	FString& OutError)
{
	if (!Object.IsValid()
		|| !Object->TryGetStringField(Field, Out)
		|| Out.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Action requires non-empty '%s'."), Field);
		return false;
	}
	return true;
}

bool NormalizeLocalSourceFile(
	const FString& Input,
	FString& OutPath,
	FString& OutError)
{
	OutPath.Reset();
	if (Input.IsEmpty()
		|| FPaths::IsRelative(Input)
		|| Input.Contains(TEXT("://"))
		|| Input.StartsWith(TEXT("\\\\"))
		|| Input.StartsWith(TEXT("//")))
	{
		OutError =
			TEXT("Import sources must be absolute local files; relative paths, URLs and network shares are not allowed.");
		return false;
	}
#if PLATFORM_WINDOWS
	if (Input.Len() < 3
		|| !FChar::IsAlpha(Input[0])
		|| Input[1] != TEXT(':')
		|| (Input[2] != TEXT('/') && Input[2] != TEXT('\\')))
	{
		OutError =
			TEXT("Import sources must use an absolute local drive path.");
		return false;
	}
#endif

	OutPath =
		IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Input);
	FPaths::NormalizeFilename(OutPath);
	if (!FPaths::CollapseRelativeDirectories(OutPath)
		|| OutPath.Contains(TEXT(".."))
		|| OutPath.StartsWith(TEXT("//")))
	{
		OutError =
			TEXT("Import source path could not be normalized safely.");
		OutPath.Reset();
		return false;
	}
	if (!IFileManager::Get().FileExists(*OutPath)
		|| IFileManager::Get().DirectoryExists(*OutPath))
	{
		OutError = FString::Printf(
			TEXT("Import source file '%s' does not exist."),
			*OutPath);
		OutPath.Reset();
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> SourceFileState(const FString& Path)
{
	TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("path"), Path);
	State->SetNumberField(
		TEXT("sizeBytes"),
		static_cast<double>(IFileManager::Get().FileSize(*Path)));
	State->SetStringField(
		TEXT("timestamp"),
		IFileManager::Get().GetTimeStamp(*Path).ToIso8601());
	State->SetStringField(TEXT("stateHash"), DigestJson(State));
	return State;
}

bool NormalizeAction(
	const TSharedPtr<FJsonObject>& Input,
	const int32 Index,
	IAssetRegistry& Registry,
	TSharedPtr<FJsonObject>& OutAction,
	TSharedPtr<FJsonObject>& OutPrecondition,
	bool& bOutRollbackAvailable,
	FString& OutRollbackBlocker,
	FString& OutError)
{
	FString Action;
	if (!ReadRequiredString(Input, TEXT("action"), Action, OutError))
	{
		return false;
	}
	static const TSet<FString> Allowed = {
		TEXT("import"),
		TEXT("reimport"),
		TEXT("copy"),
		TEXT("move"),
		TEXT("rename"),
		TEXT("delete"),
		TEXT("fixRedirectors"),
	};
	if (!Allowed.Contains(Action))
	{
		OutError = FString::Printf(TEXT("Unsupported asset action '%s'."), *Action);
		return false;
	}

	OutAction = MakeShared<FJsonObject>();
	OutAction->SetStringField(TEXT("id"), FString::Printf(TEXT("action-%03d"), Index + 1));
	OutAction->SetStringField(TEXT("action"), Action);
	OutPrecondition = MakeShared<FJsonObject>();
	OutPrecondition->SetStringField(TEXT("id"), FString::Printf(TEXT("action-%03d"), Index + 1));

	FString Source;
	FString Destination;
	FString SourceFile;
	if (Action == TEXT("import"))
	{
		if (!ReadRequiredString(Input, TEXT("sourceFile"), SourceFile, OutError)
			|| !ReadRequiredString(Input, TEXT("destination"), Destination, OutError))
		{
			return false;
		}
		FString NormalizedSourceFile;
		if (!NormalizeLocalSourceFile(
			SourceFile,
			NormalizedSourceFile,
			OutError))
		{
			return false;
		}
		SourceFile = MoveTemp(NormalizedSourceFile);
		if (!IsGamePackagePath(Destination))
		{
			OutError = FString::Printf(
				TEXT("Import destination '%s' must be a valid /Game package path."),
				*Destination);
			return false;
		}
		OutAction->SetStringField(TEXT("sourceFile"), SourceFile);
		OutAction->SetStringField(TEXT("destination"), Destination);
		OutPrecondition->SetObjectField(
			TEXT("sourceFile"),
			SourceFileState(SourceFile));
		OutPrecondition->SetObjectField(
			TEXT("destination"),
			AssetState(Registry, Destination));
		const TSharedRef<FJsonObject> DestinationState =
			AssetState(Registry, Destination);
		if (DestinationState->GetBoolField(TEXT("exists")))
		{
			OutError = FString::Printf(
				TEXT("Import destination '%s' already exists; overwrite is not supported."),
				*Destination);
			return false;
		}
	}
	else if (Action == TEXT("fixRedirectors"))
	{
		if (!Input->TryGetStringField(TEXT("source"), Source) || Source.IsEmpty())
		{
			Source = TEXT("/Game");
		}
		if (Source != TEXT("/Game")
			&& !IsGamePackagePath(Source))
		{
			OutError = TEXT("fixRedirectors source must be /Game or a valid /Game path.");
			return false;
		}
		OutAction->SetStringField(TEXT("source"), Source);
		OutPrecondition->SetStringField(TEXT("source"), Source);
		TArray<FAssetData> RedirectorAssets;
		Registry.GetAssetsByClass(
			UObjectRedirector::StaticClass()->GetClassPathName(),
			RedirectorAssets,
			true);
		TArray<FString> RedirectorPaths;
		for (const FAssetData& Redirector : RedirectorAssets)
		{
			if (IsUnderPackagePath(Redirector.PackageName.ToString(), Source))
			{
				RedirectorPaths.Add(Redirector.GetObjectPathString());
			}
		}
		RedirectorPaths.Sort();
		if (RedirectorPaths.Num() > MaxAssetActions)
		{
			OutError = FString::Printf(
				TEXT("fixRedirectors matched %d assets; narrow source so no more than %d are changed."),
				RedirectorPaths.Num(),
				MaxAssetActions);
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> RedirectorValues;
		for (const FString& RedirectorPath : RedirectorPaths)
		{
			RedirectorValues.Add(MakeShared<FJsonValueString>(RedirectorPath));
		}
		OutPrecondition->SetArrayField(TEXT("redirectors"), RedirectorValues);
		bOutRollbackAvailable = false;
		OutRollbackBlocker = TEXT("fixRedirectors cannot be reversed reliably.");
	}
	else
	{
		if (!ReadRequiredString(Input, TEXT("source"), Source, OutError))
		{
			return false;
		}
		FAssetData SourceData;
		if (!ResolveAssetData(Registry, Source, SourceData))
		{
			OutError = FString::Printf(TEXT("Source asset '%s' does not exist."), *Source);
			return false;
		}
		Source = SourceData.PackageName.ToString();
		if (Action != TEXT("copy")
			&& !Source.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(
				TEXT("Action '%s' may only modify project assets under /Game."),
				*Action);
			return false;
		}
		OutAction->SetStringField(TEXT("source"), Source);
		OutAction->SetStringField(
			TEXT("sourceObjectPath"),
			SourceData.GetObjectPathString());
		OutPrecondition->SetObjectField(TEXT("source"), AssetState(Registry, Source));

		if (Action == TEXT("copy")
			|| Action == TEXT("move")
			|| Action == TEXT("rename"))
		{
			if (!ReadRequiredString(Input, TEXT("destination"), Destination, OutError))
			{
				return false;
			}
			if (!IsGamePackagePath(Destination))
			{
				OutError = FString::Printf(
					TEXT("Destination '%s' must be a valid /Game package path."),
					*Destination);
				return false;
			}
			const TSharedRef<FJsonObject> DestinationState =
				AssetState(Registry, Destination);
			if (DestinationState->GetBoolField(TEXT("exists")))
			{
				OutError = FString::Printf(
					TEXT("Destination asset '%s' already exists."),
					*Destination);
				return false;
			}
			OutAction->SetStringField(TEXT("destination"), Destination);
			OutPrecondition->SetObjectField(
				TEXT("destination"),
				AssetState(Registry, Destination));
		}
		if (Action == TEXT("reimport"))
		{
			UObject* SourceObject = SourceData.GetAsset();
			TArray<FString> SourceFiles;
			if (!SourceObject
				|| !FReimportManager::Instance()->CanReimport(
					SourceObject,
					&SourceFiles)
				|| SourceFiles.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Asset '%s' does not expose a reimport source."),
					*Source);
				return false;
			}
			SourceFiles.Sort();
			TArray<TSharedPtr<FJsonValue>> SourceFileStates;
			for (const FString& ReimportSource : SourceFiles)
			{
				FString FullPath;
				if (!NormalizeLocalSourceFile(
					ReimportSource,
					FullPath,
					OutError))
				{
					OutError = FString::Printf(
						TEXT("Reimport source for '%s' is unsafe: %s"),
						*Source,
						*OutError);
					return false;
				}
				SourceFileStates.Add(
					MakeShared<FJsonValueObject>(SourceFileState(FullPath)));
			}
			OutPrecondition->SetArrayField(TEXT("reimportSources"), SourceFileStates);
			bOutRollbackAvailable = false;
			OutRollbackBlocker = TEXT("reimport cannot be reversed reliably.");
		}
		else if (Action == TEXT("delete"))
		{
			TArray<FName> Referencers;
			Registry.GetReferencers(
				SourceData.PackageName,
				Referencers,
				UE::AssetRegistry::EDependencyCategory::Package);
			Referencers.Sort(FNameLexicalLess());
			TArray<TSharedPtr<FJsonValue>> ReferencerValues;
			for (const FName& Referencer : Referencers)
			{
				if (ReferencerValues.Num() < MaxAssetActions)
				{
					ReferencerValues.Add(
						MakeShared<FJsonValueString>(Referencer.ToString()));
				}
			}
			OutPrecondition->SetArrayField(TEXT("referencers"), ReferencerValues);
			OutPrecondition->SetNumberField(
				TEXT("referencerCount"),
				Referencers.Num());
			if (!Referencers.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Delete source '%s' has %d package referencer(s)."),
					*Source,
					Referencers.Num());
				return false;
			}
		}
	}
	return true;
}

bool BuildPlan(
	const TSharedPtr<FJsonObject>& Request,
	TSharedPtr<FJsonObject>& OutPlan,
	FString& OutError)
{
	if (!Request.IsValid())
	{
		OutError = TEXT("request must be an object.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
	if (!Request->TryGetArrayField(TEXT("actions"), Actions)
		|| !Actions
		|| Actions->IsEmpty()
		|| Actions->Num() > MaxAssetActions)
	{
		OutError = FString::Printf(
			TEXT("request.actions must contain between 1 and %d actions."),
			MaxAssetActions);
		return false;
	}

	FString Persistence = TEXT("dirtyOnly");
	Request->TryGetStringField(TEXT("persistence"), Persistence);
	if (Persistence != TEXT("dirtyOnly")
		&& Persistence != TEXT("saveOnSuccess"))
	{
		OutError = TEXT("persistence must be dirtyOnly or saveOnSuccess.");
		return false;
	}

	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<TSharedPtr<FJsonValue>> NormalizedActions;
	TArray<TSharedPtr<FJsonValue>> Preconditions;
	TArray<TSharedPtr<FJsonValue>> RollbackBlockers;
	bool bRollbackAvailable = true;
	for (int32 Index = 0; Index < Actions->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Actions)[Index];
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("request.actions[%d] must be an object."), Index);
			return false;
		}
		TSharedPtr<FJsonObject> Normalized;
		TSharedPtr<FJsonObject> Precondition;
		bool bActionRollbackAvailable = true;
		FString Blocker;
		if (!NormalizeAction(
				Value->AsObject(),
				Index,
				Registry,
				Normalized,
				Precondition,
				bActionRollbackAvailable,
				Blocker,
				OutError))
		{
			OutError = FString::Printf(TEXT("actions[%d]: %s"), Index, *OutError);
			return false;
		}
		NormalizedActions.Add(MakeShared<FJsonValueObject>(Normalized));
		Preconditions.Add(MakeShared<FJsonValueObject>(Precondition));
		if (!bActionRollbackAvailable)
		{
			bRollbackAvailable = false;
			RollbackBlockers.Add(MakeShared<FJsonValueString>(Blocker));
		}
	}
	if (!bRollbackAvailable && Actions->Num() != 1)
	{
		OutError =
			TEXT("reimport and fixRedirectors must be executed as a single-action plan because they cannot be rolled back reliably.");
		return false;
	}

	TSharedRef<FJsonObject> NormalizedRequest = MakeShared<FJsonObject>();
	NormalizedRequest->SetStringField(TEXT("schema"), TEXT("ue.asset-change-request.v1"));
	NormalizedRequest->SetStringField(TEXT("persistence"), Persistence);
	NormalizedRequest->SetArrayField(TEXT("actions"), NormalizedActions);

	TSharedRef<FJsonObject> DigestInput = MakeShared<FJsonObject>();
	DigestInput->SetStringField(TEXT("contract"), TEXT("ue.change-plan.v1"));
	DigestInput->SetStringField(TEXT("domain"), TEXT("content.asset"));
	DigestInput->SetStringField(TEXT("planKind"), TEXT("assetChange"));
	DigestInput->SetStringField(TEXT("persistence"), Persistence);
	DigestInput->SetObjectField(TEXT("request"), NormalizedRequest);
	DigestInput->SetArrayField(TEXT("preconditions"), Preconditions);
	const FString PlanDigest = DigestJson(DigestInput);
	if (PlanDigest.IsEmpty())
	{
		OutError = TEXT("Unable to calculate plan digest.");
		return false;
	}

	OutPlan = MakeShared<FJsonObject>();
	OutPlan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	OutPlan->SetStringField(TEXT("domain"), TEXT("content.asset"));
	OutPlan->SetStringField(TEXT("planKind"), TEXT("assetChange"));
	OutPlan->SetStringField(TEXT("action"), TEXT("assetChange"));
	OutPlan->SetStringField(TEXT("scope"), TEXT("projectContent"));
	OutPlan->SetStringField(TEXT("persistence"), Persistence);
	OutPlan->SetStringField(TEXT("planDigest"), PlanDigest);
	OutPlan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	OutPlan->SetBoolField(TEXT("changesState"), true);
	OutPlan->SetStringField(
		TEXT("rollbackBoundary"),
		TEXT("sameEditorInstance"));
	OutPlan->SetBoolField(TEXT("confirmWriteRequired"), true);
	OutPlan->SetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	OutPlan->SetObjectField(TEXT("request"), NormalizedRequest);
	OutPlan->SetArrayField(TEXT("preconditions"), Preconditions);
	OutPlan->SetArrayField(TEXT("rollbackBlockers"), RollbackBlockers);
	return true;
}

bool SaveChangedAssets(
	UEditorAssetSubsystem* AssetSubsystem,
	const TSet<FString>& ChangedObjectPaths,
	FString& OutError)
{
	for (const FString& ObjectPath : ChangedObjectPaths)
	{
		if (AssetSubsystem->DoesAssetExist(ObjectPath)
			&& !AssetSubsystem->SaveAsset(ObjectPath, true))
		{
			OutError = FString::Printf(TEXT("Failed to save asset '%s'."), *ObjectPath);
			return false;
		}
	}
	return true;
}

bool RestoreDeletedAsset(
	const FReverseAction& Reverse,
	FString& OutError)
{
	if (!Reverse.Backup || !IsValid(Reverse.Backup))
	{
		OutError = FString::Printf(
			TEXT("Deleted asset backup for '%s' is unavailable."),
			*Reverse.Destination);
		return false;
	}
	UPackage* Package = CreatePackage(*Reverse.Destination);
	if (!Package)
	{
		OutError = FString::Printf(
			TEXT("Unable to create package '%s' during rollback."),
			*Reverse.Destination);
		return false;
	}
	const FString AssetName =
		Reverse.Source.IsEmpty()
			? FPackageName::GetLongPackageAssetName(Reverse.Destination)
			: Reverse.Source;
	UObject* Restored = StaticDuplicateObject(
		Reverse.Backup,
		Package,
		FName(*AssetName));
	if (!Restored)
	{
		OutError = FString::Printf(
			TEXT("Unable to restore deleted asset '%s'."),
			*Reverse.Destination);
		return false;
	}
	FAssetRegistryModule::AssetCreated(Restored);
	Package->MarkPackageDirty();
	return true;
}

bool ApplyReverseActions(
	FAssetChangeReceipt& Receipt,
	FString& OutError)
{
	UEditorAssetSubsystem* AssetSubsystem = GetAssetSubsystem();
	if (!AssetSubsystem)
	{
		OutError = TEXT("EditorAssetSubsystem is unavailable.");
		return false;
	}

	for (int32 Index = Receipt.ReverseActions.Num() - 1; Index >= 0; --Index)
	{
		const FReverseAction& Reverse = Receipt.ReverseActions[Index];
		bool bSuccess = false;
		switch (Reverse.Kind)
		{
		case FReverseAction::EKind::DeleteCreated:
			bSuccess = !AssetSubsystem->DoesAssetExist(Reverse.Source)
				|| AssetSubsystem->DeleteAsset(Reverse.Source);
			break;
		case FReverseAction::EKind::RenameBack:
		{
			IAssetRegistry& Registry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"))
					.Get();
			FAssetData ExistingDestination;
			if (ResolveAssetData(
					Registry,
					Reverse.Destination,
					ExistingDestination))
			{
				if (!ExistingDestination.IsRedirector()
					|| !AssetSubsystem->DeleteAsset(
						ExistingDestination.GetObjectPathString()))
				{
					OutError = FString::Printf(
						TEXT("Rollback destination '%s' is occupied."),
						*Reverse.Destination);
					return false;
				}
			}
			bSuccess = AssetSubsystem->RenameAsset(
				Reverse.Source,
				Reverse.Destination);
			break;
		}
		case FReverseAction::EKind::RestoreDeleted:
			bSuccess = RestoreDeletedAsset(Reverse, OutError);
			break;
		}
		if (!bSuccess)
		{
			if (OutError.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Rollback action %d failed."),
					Index);
			}
			return false;
		}
	}
	return true;
}

bool ApplyAction(
	const TSharedPtr<FJsonObject>& Action,
	FAssetChangeReceipt& Receipt,
	TSet<FString>& ChangedObjectPaths,
	TSharedPtr<FJsonObject>& OutChange,
	FString& OutError)
{
	const FString Type = Action->GetStringField(TEXT("action"));
	const FString Source =
		Action->HasField(TEXT("source"))
			? Action->GetStringField(TEXT("source"))
			: FString();
	const FString SourceObjectPath =
		Action->HasField(TEXT("sourceObjectPath"))
			? Action->GetStringField(TEXT("sourceObjectPath"))
			: ObjectPathForPackage(Source);
	const FString Destination =
		Action->HasField(TEXT("destination"))
			? Action->GetStringField(TEXT("destination"))
			: FString();
	UEditorAssetSubsystem* AssetSubsystem = GetAssetSubsystem();
	if (!AssetSubsystem)
	{
		OutError = TEXT("EditorAssetSubsystem is unavailable.");
		return false;
	}
	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	OutChange = MakeShared<FJsonObject>();
	OutChange->SetStringField(TEXT("action"), Type);
	OutChange->SetObjectField(TEXT("before"), AssetState(Registry, Source));

	if (Type == TEXT("copy"))
	{
		UObject* Created = AssetSubsystem->DuplicateAsset(
			SourceObjectPath,
			ObjectPathForPackage(Destination));
		if (!Created)
		{
			OutError = FString::Printf(
				TEXT("Failed to copy '%s' to '%s'."),
				*Source,
				*Destination);
			return false;
		}
		Receipt.ReverseActions.Add(
			{FReverseAction::EKind::DeleteCreated,
			 ObjectPathForPackage(Destination),
			 FString(),
			 nullptr});
		ChangedObjectPaths.Add(Created->GetPathName());
	}
	else if (Type == TEXT("move") || Type == TEXT("rename"))
	{
		if (!AssetSubsystem->RenameAsset(
				SourceObjectPath,
				ObjectPathForPackage(Destination)))
		{
			OutError = FString::Printf(
				TEXT("Failed to %s '%s' to '%s'."),
				*Type,
				*Source,
				*Destination);
			return false;
		}
		Receipt.ReverseActions.Add(
			{FReverseAction::EKind::RenameBack,
			 ObjectPathForPackage(Destination),
			 SourceObjectPath,
			 nullptr});
		ChangedObjectPaths.Add(SourceObjectPath);
		ChangedObjectPaths.Add(ObjectPathForPackage(Destination));
	}
	else if (Type == TEXT("delete"))
	{
		FAssetData Data;
		if (!ResolveAssetData(Registry, Source, Data))
		{
			OutError = FString::Printf(TEXT("Asset '%s' no longer exists."), *Source);
			return false;
		}
		UObject* Asset = Data.GetAsset();
		UObject* Backup = Asset
			? StaticDuplicateObject(
				Asset,
				GetTransientPackage(),
				MakeUniqueObjectName(
					GetTransientPackage(),
					Asset->GetClass(),
					TEXT("UEAIAssetBackup")))
			: nullptr;
		if (!Backup)
		{
			OutError = FString::Printf(
				TEXT("Unable to create rollback backup for '%s'."),
				*Source);
			return false;
		}
		Backup->AddToRoot();
		if (!AssetSubsystem->DeleteAsset(Data.GetObjectPathString()))
		{
			Backup->RemoveFromRoot();
			OutError = FString::Printf(TEXT("Failed to delete '%s'."), *Source);
			return false;
		}
		Receipt.ReverseActions.Add(
			{FReverseAction::EKind::RestoreDeleted,
			 Asset->GetName(),
			 Source,
			 Backup});
	}
	else if (Type == TEXT("import"))
	{
		const FString SourceFile = Action->GetStringField(TEXT("sourceFile"));
		UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
		ImportTask->Filename = SourceFile;
		ImportTask->DestinationPath =
			FPackageName::GetLongPackagePath(Destination);
		ImportTask->DestinationName =
			FPackageName::GetLongPackageAssetName(Destination);
		ImportTask->bAutomated = true;
		ImportTask->bReplaceExisting = false;
		ImportTask->bReplaceExistingSettings = false;
		ImportTask->bSave = false;
		ImportTask->bAsync = false;
		TArray<UAssetImportTask*> Tasks = {ImportTask};
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"))
			.Get()
			.ImportAssetTasks(Tasks);
		const TArray<UObject*>& Imported = ImportTask->GetObjects();
		if (Imported.IsEmpty())
		{
			OutError = FString::Printf(
				TEXT("No asset was imported from '%s'."),
				*SourceFile);
			return false;
		}
		for (UObject* Object : Imported)
		{
			if (!Object)
			{
				continue;
			}
			Receipt.ReverseActions.Add(
				{FReverseAction::EKind::DeleteCreated,
				 Object->GetPathName(),
				 FString(),
				 nullptr});
			ChangedObjectPaths.Add(Object->GetPathName());
		}
	}
	else if (Type == TEXT("reimport"))
	{
		FAssetData Data;
		UObject* Asset =
			ResolveAssetData(Registry, Source, Data) ? Data.GetAsset() : nullptr;
		if (!Asset
			|| !FReimportManager::Instance()->Reimport(
				Asset,
				false,
				false,
				FString(),
				nullptr,
				INDEX_NONE,
				false,
				true))
		{
			OutError = FString::Printf(TEXT("Failed to reimport '%s'."), *Source);
			return false;
		}
		ChangedObjectPaths.Add(Data.GetObjectPathString());
	}
	else if (Type == TEXT("fixRedirectors"))
	{
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(
			UObjectRedirector::StaticClass()->GetClassPathName(),
			Assets,
			true);
		TArray<UObjectRedirector*> Redirectors;
		for (const FAssetData& Data : Assets)
		{
			if (IsUnderPackagePath(Data.PackageName.ToString(), Source)
				&& Redirectors.Num() < MaxAssetActions)
			{
				if (UObjectRedirector* Redirector =
					Cast<UObjectRedirector>(Data.GetAsset()))
				{
					Redirectors.Add(Redirector);
				}
			}
		}
		FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"))
			.Get()
			.FixupReferencers(
				Redirectors,
				false,
				ERedirectFixupMode::DeleteFixedUpRedirectors);
		OutChange->SetNumberField(TEXT("redirectorCount"), Redirectors.Num());
	}
	else
	{
		OutError = FString::Printf(TEXT("Unsupported asset action '%s'."), *Type);
		return false;
	}
	if (Receipt.ReverseActions.Num() > MaxAssetActions)
	{
		OutError = FString::Printf(
			TEXT("Asset batch produced more than %d changed assets."),
			MaxAssetActions);
		return false;
	}

	const FString AfterPath = Destination.IsEmpty() ? Source : Destination;
	OutChange->SetObjectField(TEXT("after"), AssetState(Registry, AfterPath));
	OutChange->SetStringField(
		TEXT("changeId"),
		MakeStableId(
			TEXT("assetchange"),
			{Type, Source, Destination, DigestJson(OutChange)}));
	return true;
}

class FTool_AssetChangePlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.change.plan");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const TSharedPtr<FJsonObject>* RequestPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("request"), RequestPtr)
			|| !RequestPtr || !RequestPtr->IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("request is required."),
				TEXT("invalid_request"),
				400);
		}
		TSharedPtr<FJsonObject> Plan;
		FString Error;
		if (!BuildPlan(*RequestPtr, Plan, Error))
		{
			return FMCPToolResult::Error(Error, TEXT("plan_invalid"), 400);
		}
		return FMCPToolResult::Ok(Plan);
	}
};

class FTool_AssetChangeExecute final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.change.execute");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const TSharedPtr<FJsonObject>* RequestPtr = nullptr;
		FString ApprovedDigest;
		FString RequestId;
		bool bConfirmWrite = false;
		if (!Params->TryGetObjectField(TEXT("request"), RequestPtr)
			|| !RequestPtr || !RequestPtr->IsValid()
			|| !Params->TryGetStringField(TEXT("approvePlanDigest"), ApprovedDigest)
			|| ApprovedDigest.IsEmpty()
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty()
			|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
			|| !bConfirmWrite)
		{
			return FMCPToolResult::Error(
				TEXT("request, approvePlanDigest, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				400);
		}

		TSharedPtr<FJsonObject> Plan;
		FString Error;
		if (!BuildPlan(*RequestPtr, Plan, Error))
		{
			return FMCPToolResult::Error(Error, TEXT("plan_invalid"), 400);
		}
		if (Plan->GetStringField(TEXT("planDigest")) != ApprovedDigest)
		{
			return FMCPToolResult::Error(
				TEXT("The approved plan digest does not match current asset state."),
				TEXT("plan_digest_mismatch"),
				409);
		}

		const TSharedPtr<FJsonObject> NormalizedRequest =
			Plan->GetObjectField(TEXT("request"));
		const FString Persistence =
			NormalizedRequest->GetStringField(TEXT("persistence"));
		FAssetChangeReceipt Receipt;
		Receipt.ReceiptId = FString::Printf(
			TEXT("asset-run-%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Receipt.PlanDigest = ApprovedDigest;
		Receipt.RequestId = RequestId;
		Receipt.CreatedAt = FDateTime::UtcNow();
		Receipt.bRollbackAvailable =
			Plan->GetBoolField(TEXT("rollbackAvailable"));
		for (const TSharedPtr<FJsonValue>& Blocker :
			Plan->GetArrayField(TEXT("rollbackBlockers")))
		{
			Receipt.RollbackBlockers.Add(Blocker->AsString());
		}

		TArray<TSharedPtr<FJsonValue>> Changes;
		TSet<FString> ChangedObjectPaths;
		for (const TSharedPtr<FJsonValue>& Value :
			NormalizedRequest->GetArrayField(TEXT("actions")))
		{
			TSharedPtr<FJsonObject> Change;
			if (!ApplyAction(
					Value->AsObject(),
					Receipt,
					ChangedObjectPaths,
					Change,
					Error))
			{
				FString RollbackError;
				const bool bRecovered =
					Receipt.bRollbackAvailable
					&& ApplyReverseActions(Receipt, RollbackError);
				FString RecoveryReceiptId;
				if (bRecovered)
				{
					ReleaseReceiptBackups(Receipt);
				}
				else
				{
					EvictOldReceipts();
					RecoveryReceiptId = Receipt.ReceiptId;
					GetReceipts().Add(RecoveryReceiptId, MoveTemp(Receipt));
				}
				return FMCPToolResult::Error(
					bRecovered
						? FString::Printf(TEXT("%s Changes were rolled back."), *Error)
						: FString::Printf(
							TEXT("%s Automatic rollback was unavailable or failed: %s Recovery receipt: %s"),
							*Error,
							*RollbackError,
							*RecoveryReceiptId),
					bRecovered
						? TEXT("asset_change_failed_rolled_back")
						: TEXT("asset_change_failed"),
					500);
			}
			Changes.Add(MakeShared<FJsonValueObject>(Change));
		}

		if (Persistence == TEXT("saveOnSuccess"))
		{
			UEditorAssetSubsystem* AssetSubsystem = GetAssetSubsystem();
			if (!AssetSubsystem
				|| !SaveChangedAssets(AssetSubsystem, ChangedObjectPaths, Error))
			{
				FString RollbackError;
				const bool bRecovered =
					Receipt.bRollbackAvailable
					&& ApplyReverseActions(Receipt, RollbackError);
				FString RecoveryReceiptId;
				if (bRecovered)
				{
					ReleaseReceiptBackups(Receipt);
				}
				else
				{
					EvictOldReceipts();
					RecoveryReceiptId = Receipt.ReceiptId;
					GetReceipts().Add(RecoveryReceiptId, MoveTemp(Receipt));
				}
				return FMCPToolResult::Error(
					bRecovered
						? FString::Printf(TEXT("%s Changes were rolled back."), *Error)
						: FString::Printf(
							TEXT("%s Recovery receipt: %s"),
							*Error,
							*RecoveryReceiptId),
					TEXT("asset_save_failed"),
					500);
			}
		}

		EvictOldReceipts();
		const FString ReceiptId = Receipt.ReceiptId;
		GetReceipts().Add(ReceiptId, MoveTemp(Receipt));

		TSharedRef<FJsonObject> Diff = MakeShared<FJsonObject>();
		Diff->SetStringField(TEXT("schema"), TEXT("ue.snapshot-diff.v1"));
		TArray<TSharedPtr<FJsonValue>> BeforeSnapshots;
		TArray<TSharedPtr<FJsonValue>> AfterSnapshots;
		int32 AddedCount = 0;
		int32 RemovedCount = 0;
		int32 ModifiedCount = 0;
		for (const TSharedPtr<FJsonValue>& ChangeValue : Changes)
		{
			const TSharedPtr<FJsonObject> Change = ChangeValue->AsObject();
			BeforeSnapshots.Add(
				MakeShared<FJsonValueObject>(
					Change->GetObjectField(TEXT("before"))));
			AfterSnapshots.Add(
				MakeShared<FJsonValueObject>(
					Change->GetObjectField(TEXT("after"))));
			const FString Action = Change->GetStringField(TEXT("action"));
			if (Action == TEXT("copy") || Action == TEXT("import"))
			{
				++AddedCount;
			}
			else if (Action == TEXT("delete"))
			{
				++RemovedCount;
			}
			else
			{
				++ModifiedCount;
			}
		}
		TSharedRef<FJsonObject> BeforeSnapshot = MakeShared<FJsonObject>();
		BeforeSnapshot->SetArrayField(TEXT("assets"), BeforeSnapshots);
		TSharedRef<FJsonObject> AfterSnapshot = MakeShared<FJsonObject>();
		AfterSnapshot->SetArrayField(TEXT("assets"), AfterSnapshots);
		Diff->SetStringField(TEXT("beforeHash"), DigestJson(BeforeSnapshot));
		Diff->SetStringField(TEXT("afterHash"), DigestJson(AfterSnapshot));
		Diff->SetNumberField(TEXT("addedCount"), AddedCount);
		Diff->SetNumberField(TEXT("removedCount"), RemovedCount);
		Diff->SetNumberField(TEXT("modifiedCount"), ModifiedCount);
		Diff->SetNumberField(TEXT("changeCount"), Changes.Num());
		Diff->SetArrayField(TEXT("changes"), Changes);
		Diff->SetStringField(TEXT("diffDigest"), DigestJson(Diff));

		const FAssetChangeReceipt& Stored = GetReceipts().FindChecked(ReceiptId);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.asset-change-receipt.v1"));
		Result->SetStringField(TEXT("receiptId"), ReceiptId);
		Result->SetStringField(TEXT("planDigest"), ApprovedDigest);
		Result->SetStringField(TEXT("requestId"), RequestId);
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("persistence"), Persistence);
		Result->SetBoolField(TEXT("rollbackAvailable"), Stored.bRollbackAvailable);
		TArray<TSharedPtr<FJsonValue>> Blockers;
		for (const FString& Blocker : Stored.RollbackBlockers)
		{
			Blockers.Add(MakeShared<FJsonValueString>(Blocker));
		}
		Result->SetArrayField(TEXT("rollbackBlockers"), Blockers);
		Result->SetObjectField(TEXT("diff"), Diff);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_AssetChangeRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.change.rollback");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ReceiptId;
		FString RequestId;
		bool bConfirmWrite = false;
		if (!Params->TryGetStringField(TEXT("receiptId"), ReceiptId)
			|| ReceiptId.IsEmpty()
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty()
			|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
			|| !bConfirmWrite)
		{
			return FMCPToolResult::Error(
				TEXT("receiptId, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				400);
		}
		FAssetChangeReceipt* Receipt = GetReceipts().Find(ReceiptId);
		if (!Receipt)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Receipt '%s' is unknown in this Editor instance."),
					*ReceiptId),
				TEXT("receipt_not_found"),
				404);
		}
		if (Receipt->bRolledBack)
		{
			TSharedRef<FJsonObject> Idempotent = MakeShared<FJsonObject>();
			Idempotent->SetStringField(TEXT("schema"), TEXT("ue.asset-change-rollback.v1"));
			Idempotent->SetStringField(TEXT("receiptId"), ReceiptId);
			Idempotent->SetStringField(TEXT("status"), TEXT("alreadyRolledBack"));
			return FMCPToolResult::Ok(Idempotent);
		}
		if (!Receipt->bRollbackAvailable)
		{
			return FMCPToolResult::Error(
				TEXT("This receipt contains operations that cannot be reversed reliably."),
				TEXT("rollback_unavailable"),
				409);
		}

		FString Error;
		if (!ApplyReverseActions(*Receipt, Error))
		{
			return FMCPToolResult::Error(Error, TEXT("rollback_failed"), 500);
		}
		Receipt->bRolledBack = true;
		ReleaseReceiptBackups(*Receipt);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.asset-change-rollback.v1"));
		Result->SetStringField(TEXT("receiptId"), ReceiptId);
		Result->SetStringField(TEXT("requestId"), RequestId);
		Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
		Result->SetNumberField(
			TEXT("revertedActionCount"),
			Receipt->ReverseActions.Num());
		return FMCPToolResult::Ok(Result);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterContentAssetChangeTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_AssetChangePlan>());
	Registry.Register(MakeShared<FTool_AssetChangeExecute>());
	Registry.Register(MakeShared<FTool_AssetChangeRollback>());
}
}
