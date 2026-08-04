#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace UEAI::TraceWorker
{
struct FTraceRecord
{
	FString TraceId;
	FString TracePath;
	FString SourcePath;
	FString Sha256;
	FString CopyMode;
	FString ImportedAtUtc;
	int64 SizeBytes = 0;
};

/** Disk-backed registry used by one-shot worker processes. */
class FTraceStore
{
public:
	explicit FTraceStore(const FString& CommandLine);

	bool IsAvailable(FString& OutError) const;
	bool Import(
		const FString& SourcePath,
		const FString& CopyMode,
		FTraceRecord& OutRecord,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	/** Import an output produced by a Worker-owned, persisted launch job. */
	bool ImportGeneratedTrace(
		const FString& SourcePath,
		FTraceRecord& OutRecord,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	bool Resolve(
		const FString& TraceId,
		FTraceRecord& OutRecord,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	bool PersistAnalysisJob(
		const FString& AnalysisId,
		const FString& TraceId,
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Result,
		FString& OutError) const;
	bool ResolveAnalysisJob(
		const FString& AnalysisId,
		TSharedPtr<FJsonObject>& OutRecord,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;

	FString MakeAnalysisId() const;
	FString MakeExportPath(
		const FString& TraceId,
		const FString& Provider,
		const FString& Format) const;
	/**
	 * Resolve an existing regular file and prove that its final path remains in
	 * an unreplaced owning directory below a separately trusted root. This
	 * rejects symlink/junction escape or replacement of either file or owner.
	 */
	static bool ResolveOwnedFileForRead(
		const FString& Path,
		const FString& OwningDirectory,
		const FString& TrustedRoot,
		FString& OutFinalPath);
	const FString& GetRoot() const { return Root; }

private:
	bool IsSourceAllowed(const FString& SourcePath) const;
	bool IsStorePathAllowed(const FString& Path) const;
	bool HashFile(
		const FString& Path,
		FString& OutSha256,
		int64& OutSize,
		FString& OutError) const;
	bool CopyAndHashFile(
		const FString& Source,
		const FString& Destination,
		FString& OutSha256,
		int64& OutSize,
		FString& OutError) const;
	bool WriteRecordAtomic(const FTraceRecord& Record, FString& OutError) const;
	bool ReadRecord(
		const FString& TraceId,
		FTraceRecord& OutRecord,
		FString& OutError) const;
	bool ImportInternal(
		const FString& SourcePath,
		const FString& CopyMode,
		bool bEnforceSourcePolicy,
		FTraceRecord& OutRecord,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;
	FString RecordPath(const FString& TraceId) const;
	FString AnalysisRecordPath(const FString& AnalysisId) const;

	FString Root;
	TArray<FString> AllowedRoots;
	int64 MaximumTraceBytes = 0;
};
}
