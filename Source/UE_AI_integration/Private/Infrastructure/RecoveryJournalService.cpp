#include "Infrastructure/RecoveryJournalService.h"

#include "HAL/FileManager.h"
#include "Infrastructure/Sha256.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
constexpr int64 MaxRecoveryBytes = 20LL * 1024LL * 1024LL * 1024LL;
constexpr int32 DefaultRetentionDays = 7;

FString StringField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Name)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Name, Value);
	}
	return Value;
}

TArray<FString> RecordPaths()
{
	TArray<FString> Paths;
	IFileManager::Get().FindFilesRecursive(
		Paths,
		*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEWorkflow")),
		TEXT("recovery.manifest.json"),
		true,
		false);
	TArray<FString> RecoveryPaths;
	IFileManager::Get().FindFilesRecursive(
		RecoveryPaths,
		*FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UE_AI_integration/Recovery")),
		TEXT("*.json"),
		true,
		false);
	Paths.Append(RecoveryPaths);
	Paths.Sort(
		[](const FString& Left, const FString& Right)
		{
			return IFileManager::Get().GetTimeStamp(*Left)
				> IFileManager::Get().GetTimeStamp(*Right);
		});
	return Paths;
}

int64 DirectoryBytes(const FString& Directory)
{
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(
		Files,
		*Directory,
		TEXT("*"),
		true,
		false);
	int64 Total = 0;
	for (const FString& File : Files)
	{
		Total += FMath::Max<int64>(0, IFileManager::Get().FileSize(*File));
	}
	return Total;
}

int64 RecordBytes(const FString& Path)
{
	return DirectoryBytes(FPaths::GetPath(Path));
}

bool HashFile(const FString& Path, FString& OutHash)
{
	TArray<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path)
		&& TrySha256Hex(Bytes, OutHash);
}

bool ValidateRecordFiles(
	const TSharedPtr<FJsonObject>& Record,
	FString& OutError)
{
	OutError.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (!Record.IsValid()
		|| !Record->TryGetArrayField(TEXT("assets"), Assets)
		|| !Assets)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Assets)
	{
		const TSharedPtr<FJsonObject> Asset =
			Value.IsValid() && Value->Type == EJson::Object
			? Value->AsObject()
			: nullptr;
		if (!Asset.IsValid())
		{
			OutError = TEXT("Recovery assets contains a non-object entry.");
			return false;
		}
		for (const TPair<const TCHAR*, const TCHAR*>& Fields : {
				TPair<const TCHAR*, const TCHAR*>(TEXT("snapshotFilename"), TEXT("snapshotSha256")),
				TPair<const TCHAR*, const TCHAR*>(TEXT("checkpointFilename"), TEXT("checkpointSha256"))})
		{
			const FString Filename = StringField(Asset, Fields.Key);
			const FString ExpectedHash = StringField(Asset, Fields.Value);
			if (Filename.IsEmpty() && ExpectedHash.IsEmpty())
			{
				continue;
			}
			FString ActualHash;
			if (Filename.IsEmpty() || ExpectedHash.IsEmpty()
				|| !IFileManager::Get().FileExists(*Filename)
				|| !HashFile(Filename, ActualHash)
				|| ActualHash != ExpectedHash)
			{
				OutError = FString::Printf(
					TEXT("Recovery artifact '%s' is missing or failed checksum validation."),
					*Filename);
				return false;
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if (Asset->TryGetArrayField(TEXT("files"), Files) && Files)
		{
			for (const TSharedPtr<FJsonValue>& FileValue : *Files)
			{
				const TSharedPtr<FJsonObject> File =
					FileValue.IsValid() && FileValue->Type == EJson::Object
					? FileValue->AsObject()
					: nullptr;
				bool bBeforeExists = false;
				if (!File.IsValid())
				{
					OutError = TEXT("Recovery file entry is not an object.");
					return false;
				}
				File->TryGetBoolField(TEXT("beforeExists"), bBeforeExists);
				if (!bBeforeExists)
				{
					continue;
				}
				const FString Snapshot = StringField(File, TEXT("snapshotFilename"));
				const FString Expected = StringField(File, TEXT("snapshotSha256"));
				FString Actual;
				if (Snapshot.IsEmpty() || Expected.IsEmpty()
					|| !IFileManager::Get().FileExists(*Snapshot)
					|| !HashFile(Snapshot, Actual)
					|| (Expected.StartsWith(TEXT("sha256:"))
						? TEXT("sha256:") + Actual != Expected
						: Actual != Expected))
				{
					OutError = FString::Printf(
						TEXT("Recovery artifact '%s' is missing or corrupt."),
						*Snapshot);
					return false;
				}
			}
		}
	}
	return true;
}

bool IsTerminal(const FString& Status)
{
	return Status == TEXT("completed")
		|| Status == TEXT("rolledBack")
		|| Status == TEXT("failed");
}

bool IsCleanupSafe(const TSharedPtr<FJsonObject>& Record)
{
	if (!IsTerminal(StringField(Record, TEXT("status"))))
	{
		return false;
	}
	bool bRollbackAvailable = false;
	bool bRollbackVerified = false;
	Record->TryGetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	Record->TryGetBoolField(TEXT("rollbackVerified"), bRollbackVerified);
	return !bRollbackAvailable || bRollbackVerified
		|| StringField(Record, TEXT("status")) == TEXT("rolledBack");
}
}

bool FRecoveryJournalService::Handles(const FString& CapabilityId) const
{
	return CapabilityId == TEXT("production.recovery.list")
		|| CapabilityId == TEXT("production.recovery.get")
		|| CapabilityId == TEXT("production.recovery.cleanup");
}

FMCPToolResult FRecoveryJournalService::Execute(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params) const
{
	if (CapabilityId == TEXT("production.recovery.list"))
	{
		return List(Params);
	}
	if (CapabilityId == TEXT("production.recovery.get"))
	{
		return Get(Params);
	}
	if (CapabilityId == TEXT("production.recovery.cleanup"))
	{
		return Cleanup(Params);
	}
	return FMCPToolResult::Error(
		TEXT("Recovery capability is not supported."),
		TEXT("capability_not_found"),
		404);
}

bool FRecoveryJournalService::IsSafeId(const FString& Value)
{
	FGuid Guid;
	return FGuid::Parse(Value, Guid);
}

TSharedPtr<FJsonObject> FRecoveryJournalService::ReadRecord(
	const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)
		|| Text.Len() > 16 * 1024 * 1024)
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
		? Object
		: nullptr;
}

TSharedPtr<FJsonObject> FRecoveryJournalService::Summarize(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Record)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), StringField(Record, TEXT("schema")));
	FString Id = StringField(Record, TEXT("changeSetId"));
	if (Id.IsEmpty())
	{
		Id = StringField(Record, TEXT("runId"));
	}
	Result->SetStringField(TEXT("changeSetId"), Id);
	Result->SetStringField(TEXT("status"), StringField(Record, TEXT("status")));
	Result->SetStringField(
		TEXT("recoveryState"),
		StringField(Record, TEXT("recoveryState")));
	Result->SetStringField(
		TEXT("rollbackDurability"),
		!StringField(Record, TEXT("rollbackDurability")).IsEmpty()
			? StringField(Record, TEXT("rollbackDurability"))
			: StringField(Record, TEXT("durability")));
	bool bRollbackAvailable = false;
	Record->TryGetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	Result->SetBoolField(TEXT("rollbackAvailable"), bRollbackAvailable);
	Result->SetStringField(
		TEXT("updatedAtUtc"),
		StringField(Record, TEXT("updatedAtUtc")));
	Result->SetStringField(
		TEXT("recoveryExpiresAt"),
		StringField(Record, TEXT("recoveryExpiresAt")));
	Result->SetStringField(TEXT("path"), Path);
	Result->SetNumberField(
		TEXT("bytes"),
		static_cast<double>(RecordBytes(Path)));
	return Result;
}

FMCPToolResult FRecoveryJournalService::List(
	const TSharedPtr<FJsonObject>& Params) const
{
	double OffsetNumber = 0.0;
	double LimitNumber = 25.0;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("offset"), OffsetNumber);
		Params->TryGetNumberField(TEXT("limit"), LimitNumber);
	}
	const int32 Offset = FMath::Max(0, static_cast<int32>(OffsetNumber));
	const int32 Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, 100);
	TArray<TSharedPtr<FJsonValue>> All;
	int64 StorageBytes = 0;
	for (const FString& Path : RecordPaths())
	{
		const TSharedPtr<FJsonObject> Record = ReadRecord(Path);
		const FString Schema = StringField(Record, TEXT("schema"));
		if (Schema == TEXT("ue.workflow-run.v1")
			|| Schema == TEXT("ue.recovery-journal.v1")
			|| Schema == TEXT("ue.landscape-recovery.v1"))
		{
			All.Add(MakeShared<FJsonValueObject>(Summarize(Path, Record)));
			StorageBytes += RecordBytes(Path);
		}
	}
	TArray<TSharedPtr<FJsonValue>> Page;
	for (int32 Index = Offset; Index < All.Num() && Page.Num() < Limit; ++Index)
	{
		Page.Add(All[Index]);
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("records"), Page);
	Data->SetNumberField(TEXT("total"), All.Num());
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetBoolField(TEXT("hasMore"), Offset + Page.Num() < All.Num());
	Data->SetNumberField(TEXT("storageLimitBytes"), static_cast<double>(MaxRecoveryBytes));
	Data->SetNumberField(TEXT("storageBytes"), static_cast<double>(StorageBytes));
	Data->SetBoolField(TEXT("overStorageLimit"), StorageBytes > MaxRecoveryBytes);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FRecoveryJournalService::Get(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString Id = StringField(Params, TEXT("changeSetId"));
	if (!IsSafeId(Id))
	{
		return FMCPToolResult::Error(
			TEXT("changeSetId must be a UUID."),
			TEXT("invalid_params"),
			422);
	}
	for (const FString& Path : RecordPaths())
	{
		const TSharedPtr<FJsonObject> Record = ReadRecord(Path);
		if (StringField(Record, TEXT("runId")) == Id
			|| StringField(Record, TEXT("changeSetId")) == Id)
		{
			FString ValidationError;
			if (!ValidateRecordFiles(Record, ValidationError))
			{
				TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("changeSetId"), Id);
				Details->SetStringField(TEXT("path"), Path);
				Details->SetStringField(TEXT("message"), ValidationError);
				FMCPToolResult Failure = FMCPToolResult::Error(
					TEXT("Recovery record artifacts failed verification."),
					TEXT("recovery_checkpoint_corrupt"),
					409);
				Failure.Data = Details;
				return Failure;
			}
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetObjectField(TEXT("summary"), Summarize(Path, Record));
			Data->SetObjectField(TEXT("record"), Record);
			return FMCPToolResult::Ok(Data);
		}
	}
	return FMCPToolResult::Error(
		TEXT("Recovery record was not found."),
		TEXT("job_not_found"),
		404);
}

FMCPToolResult FRecoveryJournalService::Cleanup(
	const TSharedPtr<FJsonObject>& Params) const
{
	bool bConfirmWrite = false;
	bool bDryRun = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite);
		Params->TryGetBoolField(TEXT("dryRun"), bDryRun);
	}
	if (!bDryRun && (!bConfirmWrite || StringField(Params, TEXT("requestId")).IsEmpty()))
	{
		return FMCPToolResult::Error(
			TEXT("Cleanup apply requires confirmWrite=true and requestId."),
			TEXT("confirmation_required"),
			409);
	}
	double RetentionNumber = DefaultRetentionDays;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("retentionDays"), RetentionNumber);
	}
	const FDateTime Cutoff = FDateTime::UtcNow()
		- FTimespan::FromDays(FMath::Clamp(RetentionNumber, 1.0, 3650.0));
	TArray<TSharedPtr<FJsonValue>> Candidates;
	int64 Reclaimed = 0;
	const TArray<FString> Paths = RecordPaths();
	int64 CurrentStorageBytes = 0;
	for (const FString& Path : Paths)
	{
		CurrentStorageBytes += RecordBytes(Path);
	}
	for (int32 Index = Paths.Num() - 1; Index >= 0; --Index)
	{
		const FString& Path = Paths[Index];
		const TSharedPtr<FJsonObject> Record = ReadRecord(Path);
		const bool bExpired = IFileManager::Get().GetTimeStamp(*Path) < Cutoff;
		const bool bOverLimit = CurrentStorageBytes > MaxRecoveryBytes;
		if (!Record.IsValid() || !IsCleanupSafe(Record)
			|| (!bExpired && !bOverLimit))
		{
			continue;
		}
		const int64 Bytes = RecordBytes(Path);
		Candidates.Add(MakeShared<FJsonValueObject>(Summarize(Path, Record)));
		Reclaimed += Bytes;
		CurrentStorageBytes = FMath::Max<int64>(0, CurrentStorageBytes - Bytes);
		if (!bDryRun)
		{
			const FString RecordDirectory = FPaths::GetPath(Path);
			const FString WorkflowRoot = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEWorkflow")));
			const FString RecoveryRoot = FPaths::ConvertRelativePathToFull(
				FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE_AI_integration/Recovery")));
			const FString FullDirectory = FPaths::ConvertRelativePathToFull(RecordDirectory);
			if ((FullDirectory.StartsWith(WorkflowRoot)
					|| FullDirectory.StartsWith(RecoveryRoot))
				&& FullDirectory != WorkflowRoot && FullDirectory != RecoveryRoot)
			{
				IFileManager::Get().DeleteDirectory(*FullDirectory, false, true);
			}
		}
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("dryRun"), bDryRun);
	Data->SetArrayField(TEXT("records"), Candidates);
	Data->SetNumberField(TEXT("recordCount"), Candidates.Num());
	Data->SetNumberField(TEXT("reclaimedBytes"), static_cast<double>(Reclaimed));
	Data->SetNumberField(
		TEXT("remainingStorageBytes"),
		static_cast<double>(CurrentStorageBytes));
	return FMCPToolResult::Ok(Data);
}
}
