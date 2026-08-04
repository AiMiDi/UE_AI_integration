#include "TraceWorkerStore.h"
#include "TraceWorkerCommandLine.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_SSL
#include <openssl/sha.h>
#endif

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <cstdlib>
#include <sys/stat.h>
#endif

namespace UEAI::TraceWorker
{
namespace
{
constexpr int64 CopyBufferSize = 1024 * 1024;
constexpr int64 MaximumRecordBytes = 1024 * 1024;
constexpr int64 BytesPerMiB = 1024ll * 1024ll;
constexpr int64 DefaultMaximumTraceMiB = 64ll * 1024ll;
constexpr int64 HardMaximumTraceMiB = 1024ll * 1024ll;
constexpr uint64 MinimumFreeSpaceReserveBytes = 512ull * 1024ull * 1024ull;

bool IsSafeId(const FString& Value, const TCHAR* Prefix)
{
	if (Value.IsEmpty() || !Value.StartsWith(Prefix) || Value.Len() > 128)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!(FChar::IsAlnum(Character) || Character == TEXT('-')))
		{
			return false;
		}
	}
	return true;
}

FString NormalizeDirectory(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeDirectoryName(Path);
	return Path;
}

bool IsWithinDirectory(const FString& Path, const FString& Root)
{
	return FPaths::IsSamePath(Path, Root)
		|| FPaths::IsUnderDirectory(Path, Root);
}

bool ResolveExistingFinalPath(
	const FString& Input,
	const bool bExpectDirectory,
	FString& OutPath)
{
	OutPath.Reset();
	const FString FullPath = FPaths::ConvertRelativePathToFull(Input);
#if PLATFORM_WINDOWS
	const DWORD Flags = bExpectDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0;
	const HANDLE Handle = CreateFileW(
		*FullPath,
		FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_EXISTING,
		Flags,
		nullptr);
	if (Handle == INVALID_HANDLE_VALUE || GetFileType(Handle) != FILE_TYPE_DISK)
	{
		if (Handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(Handle);
		}
		return false;
	}
	BY_HANDLE_FILE_INFORMATION Information = {};
	if (!GetFileInformationByHandle(Handle, &Information)
		|| ((Information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			!= bExpectDirectory)
	{
		CloseHandle(Handle);
		return false;
	}
	const DWORD Required = GetFinalPathNameByHandleW(
		Handle,
		nullptr,
		0,
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (Required == 0)
	{
		CloseHandle(Handle);
		return false;
	}
	TArray<WCHAR> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(Required) + 1);
	const DWORD Written = GetFinalPathNameByHandleW(
		Handle,
		Buffer.GetData(),
		static_cast<DWORD>(Buffer.Num()),
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	CloseHandle(Handle);
	if (Written == 0 || Written >= static_cast<DWORD>(Buffer.Num()))
	{
		return false;
	}
	FString FinalPath(Buffer.GetData());
	if (FinalPath.StartsWith(TEXT("\\\\?\\UNC\\"), ESearchCase::IgnoreCase))
	{
		FinalPath = TEXT("\\\\") + FinalPath.Mid(8);
	}
	else if (FinalPath.StartsWith(TEXT("\\\\?\\"), ESearchCase::IgnoreCase))
	{
		FinalPath.RightChopInline(4, false);
	}
	OutPath = FPaths::ConvertRelativePathToFull(FinalPath);
#else
	const FTCHARToUTF8 Utf8(*FullPath);
	char* Resolved = ::realpath(Utf8.Get(), nullptr);
	if (!Resolved)
	{
		return false;
	}
	struct stat Information = {};
	const bool bValid = ::stat(Resolved, &Information) == 0
		&& (bExpectDirectory ? S_ISDIR(Information.st_mode)
			: S_ISREG(Information.st_mode));
	if (bValid)
	{
		OutPath = UTF8_TO_TCHAR(Resolved);
	}
	std::free(Resolved);
	if (!bValid)
	{
		return false;
	}
#endif
	if (bExpectDirectory)
	{
		FPaths::NormalizeDirectoryName(OutPath);
	}
	else
	{
		FPaths::NormalizeFilename(OutPath);
	}
	return !OutPath.IsEmpty();
}

bool IsLinkOrReparsePoint(const FString& Input)
{
	const FString FullPath = FPaths::ConvertRelativePathToFull(Input);
#if PLATFORM_WINDOWS
	const DWORD Attributes = GetFileAttributesW(*FullPath);
	return Attributes != INVALID_FILE_ATTRIBUTES
		&& (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
	const FTCHARToUTF8 Utf8(*FullPath);
	struct stat Information = {};
	return ::lstat(Utf8.Get(), &Information) == 0
		&& S_ISLNK(Information.st_mode);
#endif
}

int64 ReadMaximumTraceBytes(const FString& CommandLine)
{
	FString LimitText;
	if (!ReadCommandLineValue(
			*CommandLine, TEXT("maxImportMiB="), LimitText))
	{
		LimitText = FPlatformMisc::GetEnvironmentVariable(
			TEXT("UEAI_TRACE_MAX_IMPORT_MIB"));
	}
	int64 LimitMiB = DefaultMaximumTraceMiB;
	int64 ParsedMiB = 0;
	if (!LimitText.IsEmpty()
		&& LexTryParseString(ParsedMiB, *LimitText)
		&& ParsedMiB > 0)
	{
		LimitMiB = FMath::Clamp<int64>(
			ParsedMiB, 1, HardMaximumTraceMiB);
	}
	return LimitMiB * BytesPerMiB;
}
}

FTraceStore::FTraceStore(const FString& CommandLine)
{
	MaximumTraceBytes = ReadMaximumTraceBytes(CommandLine);
	FString RequestedRoot;
	ReadCommandLineValue(*CommandLine, TEXT("StoreRoot="), RequestedRoot);
	if (RequestedRoot.IsEmpty())
	{
		RequestedRoot =
			FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_TRACE_STORE"));
	}
	if (RequestedRoot.IsEmpty())
	{
		RequestedRoot = FPaths::Combine(
			FPlatformProcess::UserSettingsDir(),
			TEXT("UE_AI_integration/TraceWorker"));
	}
	Root = NormalizeDirectory(RequestedRoot);
	if (!IFileManager::Get().MakeDirectory(*Root, true))
	{
		Root.Reset();
	}
	else
	{
		FString FinalRoot;
		if (ResolveExistingFinalPath(Root, true, FinalRoot))
		{
			Root = MoveTemp(FinalRoot);
		}
		else
		{
			Root.Reset();
		}
	}

	FString Roots =
		FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_TRACE_ROOTS"));
	TArray<FString> Values;
	Roots.ParseIntoArray(Values, TEXT(";"), true);
	for (FString Value : Values)
	{
		Value.TrimStartAndEndInline();
		FString FinalRoot;
		if (!Value.IsEmpty()
			&& ResolveExistingFinalPath(Value, true, FinalRoot))
		{
			AllowedRoots.AddUnique(MoveTemp(FinalRoot));
		}
	}
	if (!Root.IsEmpty())
	{
		AllowedRoots.AddUnique(Root);
	}
}

bool FTraceStore::IsAvailable(FString& OutError) const
{
	OutError.Reset();
	FString CurrentRoot;
	if (Root.IsEmpty()
		|| !ResolveExistingFinalPath(Root, true, CurrentRoot)
		|| !FPaths::IsSamePath(CurrentRoot, Root))
	{
		OutError = TEXT(
			"The local Trace Worker store root is unavailable or its final path changed.");
		return false;
	}
	for (const TCHAR* Child : {
			TEXT("records"), TEXT("traces"), TEXT("exports"), TEXT("analyses")})
	{
		const FString Directory = FPaths::Combine(Root, Child);
		FString FinalDirectory;
		if (!IFileManager::Get().MakeDirectory(*Directory, true)
			|| !ResolveExistingFinalPath(Directory, true, FinalDirectory)
			|| !IsWithinDirectory(FinalDirectory, Root))
		{
			OutError = TEXT(
				"A Trace Worker store directory escapes its canonical store root.");
			return false;
		}
	}
	return true;
}

bool FTraceStore::IsSourceAllowed(const FString& SourcePath) const
{
	FString FinalPath;
	if (!ResolveExistingFinalPath(SourcePath, false, FinalPath))
	{
		return false;
	}
	for (const FString& AllowedRoot : AllowedRoots)
	{
		if (IsWithinDirectory(FinalPath, AllowedRoot))
		{
			return true;
		}
	}
	return false;
}

bool FTraceStore::IsStorePathAllowed(const FString& Path) const
{
	FString FinalPath;
	return ResolveExistingFinalPath(Path, false, FinalPath)
		&& IsWithinDirectory(FinalPath, Root);
}

bool FTraceStore::ResolveOwnedFileForRead(
	const FString& Path,
	const FString& OwningDirectory,
	const FString& TrustedRoot,
	FString& OutFinalPath)
{
	OutFinalPath.Reset();
	const FString FullOwner = NormalizeDirectory(OwningDirectory);
	const FString OwnerParent = FPaths::GetPath(FullOwner);
	const FString OwnerLeaf = FPaths::GetCleanFilename(FullOwner);
	FString FinalParent;
	FString FinalOwner;
	FString FinalTrustedRoot;
	FString FinalFile;
	if (OwnerParent.IsEmpty()
		|| OwnerLeaf.IsEmpty()
		|| IsLinkOrReparsePoint(FullOwner)
		|| !ResolveExistingFinalPath(OwnerParent, true, FinalParent)
		|| !ResolveExistingFinalPath(FullOwner, true, FinalOwner)
		|| !ResolveExistingFinalPath(TrustedRoot, true, FinalTrustedRoot)
		|| !FPaths::IsSamePath(
			FinalOwner,
			FPaths::Combine(FinalParent, OwnerLeaf))
		|| !IsWithinDirectory(FinalOwner, FinalTrustedRoot)
		|| !ResolveExistingFinalPath(Path, false, FinalFile)
		|| !IsWithinDirectory(FinalFile, FinalOwner))
	{
		return false;
	}
	OutFinalPath = MoveTemp(FinalFile);
	return true;
}

bool FTraceStore::HashFile(
	const FString& Path,
	FString& OutSha256,
	int64& OutSize,
	FString& OutError) const
{
	OutSha256.Reset();
	OutSize = 0;
	OutError.Reset();
#if !WITH_SSL
	OutError = TEXT("The worker was built without SHA-256 support.");
	return false;
#else
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Reader(PlatformFile.OpenRead(*Path));
	if (!Reader.IsValid())
	{
		OutError = TEXT("The trace file could not be opened for hashing.");
		return false;
	}
	const int64 Size = Reader->Size();
	if (Size <= 0 || Size > MaximumTraceBytes)
	{
		OutError = TEXT("The trace file is empty or exceeds the worker size bound.");
		return false;
	}
	SHA256_CTX Context;
	if (::SHA256_Init(&Context) != 1)
	{
		OutError = TEXT("SHA-256 initialization failed.");
		return false;
	}
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(CopyBufferSize);
	int64 Remaining = Size;
	while (Remaining > 0)
	{
		if (IsEngineExitRequested())
		{
			OutError = TEXT("The trace hash was cancelled during Worker shutdown.");
			return false;
		}
		const int64 Count = FMath::Min<int64>(Remaining, Buffer.Num());
		if (!Reader->Read(Buffer.GetData(), Count)
			|| ::SHA256_Update(&Context, Buffer.GetData(), Count) != 1)
		{
			OutError = TEXT("The trace file could not be hashed completely.");
			return false;
		}
		Remaining -= Count;
	}
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	if (::SHA256_Final(Digest, &Context) != 1)
	{
		OutError = TEXT("SHA-256 finalization failed.");
		return false;
	}
	OutSha256 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	OutSize = Size;
	return true;
#endif
}

bool FTraceStore::CopyAndHashFile(
	const FString& Source,
	const FString& Destination,
	FString& OutSha256,
	int64& OutSize,
	FString& OutError) const
{
	OutSha256.Reset();
	OutSize = 0;
	OutError.Reset();
#if !WITH_SSL
	OutError = TEXT("The worker was built without SHA-256 support.");
	return false;
#else
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Reader(PlatformFile.OpenRead(*Source));
	if (!Reader.IsValid())
	{
		OutError = TEXT("The trace file could not be opened for import.");
		return false;
	}
	const int64 Size = Reader->Size();
	if (Size <= 0 || Size > MaximumTraceBytes)
	{
		OutError = TEXT("The trace file is empty or exceeds the worker size bound.");
		return false;
	}
	const FString RequestedDirectory = FPaths::GetPath(Destination);
	FString FinalDirectory;
	if (!IFileManager::Get().MakeDirectory(*RequestedDirectory, true)
		|| !ResolveExistingFinalPath(RequestedDirectory, true, FinalDirectory)
		|| !IsWithinDirectory(FinalDirectory, Root))
	{
		OutError = TEXT("The trace copy destination escapes the canonical Worker store.");
		return false;
	}
	uint64 TotalBytes = 0;
	uint64 FreeBytes = 0;
	const uint64 RequiredBytes = static_cast<uint64>(Size)
		+ MinimumFreeSpaceReserveBytes;
	if (!FPlatformMisc::GetDiskTotalAndFreeSpace(
			FinalDirectory, TotalBytes, FreeBytes)
		|| FreeBytes < RequiredBytes)
	{
		OutError = TEXT(
			"The Worker store does not have enough verified free space for the trace copy and safety reserve.");
		return false;
	}
	const FString EffectiveDestination = FPaths::Combine(
		FinalDirectory, FPaths::GetCleanFilename(Destination));
	const FString Temporary = EffectiveDestination + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TUniquePtr<IFileHandle> Writer(PlatformFile.OpenWrite(*Temporary));
	if (!Writer.IsValid())
	{
		OutError = TEXT("The worker store could not create a temporary trace copy.");
		return false;
	}
	SHA256_CTX Context;
	if (::SHA256_Init(&Context) != 1)
	{
		Writer.Reset();
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("SHA-256 initialization failed.");
		return false;
	}
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(CopyBufferSize);
	int64 Remaining = Size;
	while (Remaining > 0)
	{
		if (IsEngineExitRequested())
		{
			Writer.Reset();
			IFileManager::Get().Delete(*Temporary, false, true);
			OutError = TEXT("The trace import was cancelled during Worker shutdown.");
			return false;
		}
		const int64 Count = FMath::Min<int64>(Remaining, Buffer.Num());
		if (!Reader->Read(Buffer.GetData(), Count)
			|| !Writer->Write(Buffer.GetData(), Count)
			|| ::SHA256_Update(&Context, Buffer.GetData(), Count) != 1)
		{
			Writer.Reset();
			IFileManager::Get().Delete(*Temporary, false, true);
			OutError = TEXT("The trace import did not complete.");
			return false;
		}
		Remaining -= Count;
	}
	Writer.Reset();
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	if (::SHA256_Final(Digest, &Context) != 1)
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("SHA-256 finalization failed.");
		return false;
	}
	if (!IFileManager::Get().Move(
			*EffectiveDestination, *Temporary, true, true, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("The imported trace could not be published atomically.");
		return false;
	}
	OutSha256 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	OutSize = Size;
	return true;
#endif
}

FString FTraceStore::RecordPath(const FString& TraceId) const
{
	return FPaths::Combine(Root, TEXT("records"), TraceId + TEXT(".json"));
}

FString FTraceStore::AnalysisRecordPath(const FString& AnalysisId) const
{
	return FPaths::Combine(
		Root, TEXT("analyses"), AnalysisId, TEXT("result.json"));
}

bool FTraceStore::WriteRecordAtomic(
	const FTraceRecord& Record,
	FString& OutError) const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema"), TEXT("ue.trace-worker-record.v1"));
	Object->SetStringField(TEXT("traceId"), Record.TraceId);
	Object->SetStringField(TEXT("tracePath"), Record.TracePath);
	Object->SetStringField(TEXT("sourcePath"), Record.SourcePath);
	Object->SetStringField(TEXT("sha256"), Record.Sha256);
	Object->SetStringField(TEXT("copyMode"), Record.CopyMode);
	Object->SetStringField(TEXT("importedAtUtc"), Record.ImportedAtUtc);
	Object->SetNumberField(TEXT("sizeBytes"), static_cast<double>(Record.SizeBytes));
	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		OutError = TEXT("The trace registry record could not be serialized.");
		return false;
	}
	const FString Destination = RecordPath(Record.TraceId);
	const FString Temporary = Destination + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(
			Serialized, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(
			*Destination, *Temporary, true, true, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("The trace registry record could not be published atomically.");
		return false;
	}
	return true;
}

bool FTraceStore::ReadRecord(
	const FString& TraceId,
	FTraceRecord& OutRecord,
	FString& OutError) const
{
	OutError.Reset();
	FString Serialized;
	const FString Path = RecordPath(TraceId);
	const int64 RecordSize = IFileManager::Get().FileSize(*Path);
	if (RecordSize <= 0
		|| RecordSize > MaximumRecordBytes
		|| !FFileHelper::LoadFileToString(Serialized, *Path))
	{
		OutError = TEXT(
			"The trace ID is not registered or its bounded Worker record is invalid.");
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
	FString Schema;
	if (!FJsonSerializer::Deserialize(Reader, Object)
		|| !Object.IsValid()
		|| !Object->TryGetStringField(TEXT("schema"), Schema)
		|| Schema != TEXT("ue.trace-worker-record.v1"))
	{
		OutError = TEXT("The trace registry record is corrupt.");
		return false;
	}
	double SizeBytes = 0.0;
	if (!Object->TryGetStringField(TEXT("traceId"), OutRecord.TraceId)
		|| !Object->TryGetStringField(TEXT("tracePath"), OutRecord.TracePath)
		|| !Object->TryGetStringField(TEXT("sourcePath"), OutRecord.SourcePath)
		|| !Object->TryGetStringField(TEXT("sha256"), OutRecord.Sha256)
		|| !Object->TryGetStringField(TEXT("copyMode"), OutRecord.CopyMode)
		|| !Object->TryGetStringField(TEXT("importedAtUtc"), OutRecord.ImportedAtUtc)
		|| !Object->TryGetNumberField(TEXT("sizeBytes"), SizeBytes)
		|| !FMath::IsFinite(SizeBytes)
		|| (OutRecord.CopyMode != TEXT("copy")
			&& OutRecord.CopyMode != TEXT("reference")))
	{
		OutError = TEXT("The trace registry record is corrupt.");
		return false;
	}
	OutRecord.SizeBytes = static_cast<int64>(SizeBytes);
	return OutRecord.TraceId == TraceId && OutRecord.SizeBytes > 0;
}

bool FTraceStore::Import(
	const FString& SourcePath,
	const FString& CopyMode,
	FTraceRecord& OutRecord,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	return ImportInternal(
		SourcePath,
		CopyMode,
		true,
		OutRecord,
		OutErrorCode,
		OutErrorMessage);
}

bool FTraceStore::ImportGeneratedTrace(
	const FString& SourcePath,
	FTraceRecord& OutRecord,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	return ImportInternal(
		SourcePath,
		TEXT("copy"),
		false,
		OutRecord,
		OutErrorCode,
		OutErrorMessage);
}

bool FTraceStore::ImportInternal(
	const FString& SourcePath,
	const FString& CopyMode,
	const bool bEnforceSourcePolicy,
	FTraceRecord& OutRecord,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	FString StoreError;
	if (!IsAvailable(StoreError))
	{
		OutErrorCode = TEXT("trace_store_unavailable");
		OutErrorMessage = StoreError;
		return false;
	}
	if (CopyMode != TEXT("copy") && CopyMode != TEXT("reference"))
	{
		OutErrorCode = TEXT("trace_import_invalid");
		OutErrorMessage = TEXT("copyMode must be 'copy' or 'reference'.");
		return false;
	}
	FString FullSource;
	if (!ResolveExistingFinalPath(SourcePath, false, FullSource)
		|| !FullSource.EndsWith(TEXT(".utrace"), ESearchCase::IgnoreCase)
		|| (CopyMode == TEXT("reference")
			&& bEnforceSourcePolicy
			&& !IsSourceAllowed(FullSource)))
	{
		OutErrorCode = TEXT("trace_path_not_allowed");
		OutErrorMessage = CopyMode == TEXT("reference")
			? TEXT("Referenced traces must resolve inside the canonical Worker store or an explicit UEAI_TRACE_ROOTS directory.")
			: TEXT("The trace import source is unavailable or is not a regular .utrace file.");
		return false;
	}

	FString Sha256;
	int64 Size = 0;
	if (!HashFile(FullSource, Sha256, Size, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_hash_failed");
		return false;
	}
	const FString TraceId = TEXT("trace-local-") + Sha256.Left(32);
	FTraceRecord Existing;
	FString ExistingCode;
	FString ExistingMessage;
	if (Resolve(TraceId, Existing, ExistingCode, ExistingMessage)
		&& Existing.Sha256 == Sha256
		&& Existing.SizeBytes == Size
		&& (CopyMode == TEXT("reference")
			|| Existing.CopyMode == TEXT("copy")))
	{
		OutRecord = MoveTemp(Existing);
		return true;
	}

	FTraceRecord Record;
	Record.TraceId = TraceId;
	Record.SourcePath = FullSource;
	Record.Sha256 = Sha256;
	Record.SizeBytes = Size;
	Record.CopyMode = CopyMode;
	Record.ImportedAtUtc = FDateTime::UtcNow().ToIso8601();
	if (CopyMode == TEXT("copy"))
	{
		Record.TracePath = FPaths::Combine(
			Root, TEXT("traces"), TraceId, TEXT("trace.utrace"));
		FString CopiedSha;
		int64 CopiedSize = 0;
		if (!CopyAndHashFile(
				FullSource,
				Record.TracePath,
				CopiedSha,
				CopiedSize,
				OutErrorMessage)
			|| CopiedSha != Sha256
			|| CopiedSize != Size)
		{
			OutErrorCode = TEXT("trace_import_failed");
			return false;
		}
		FString FinalDestination;
		if (!ResolveExistingFinalPath(
				Record.TracePath, false, FinalDestination)
			|| !IsWithinDirectory(FinalDestination, Root))
		{
			OutErrorCode = TEXT("trace_import_failed");
			OutErrorMessage = TEXT(
				"The imported trace did not remain inside the canonical Worker store.");
			return false;
		}
		Record.TracePath = MoveTemp(FinalDestination);
	}
	else
	{
		Record.TracePath = FullSource;
	}
	if (!WriteRecordAtomic(Record, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_registry_write_failed");
		return false;
	}
	OutRecord = MoveTemp(Record);
	return true;
}

bool FTraceStore::Resolve(
	const FString& TraceId,
	FTraceRecord& OutRecord,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	FString StoreError;
	if (!IsAvailable(StoreError))
	{
		OutErrorCode = TEXT("trace_store_unavailable");
		OutErrorMessage = StoreError;
		return false;
	}
	if (!IsSafeId(TraceId, TEXT("trace-local-")))
	{
		OutErrorCode = TEXT("trace_id_invalid");
		OutErrorMessage = TEXT("The trace ID is not a local Worker trace ID.");
		return false;
	}
	if (!ReadRecord(TraceId, OutRecord, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_not_found");
		return false;
	}
	FString FinalTracePath;
	if (!ResolveExistingFinalPath(
			OutRecord.TracePath, false, FinalTracePath))
	{
		OutErrorCode = TEXT("trace_not_found");
		OutErrorMessage = TEXT("The registered trace file no longer exists.");
		return false;
	}
	if ((OutRecord.CopyMode == TEXT("reference")
			&& !IsSourceAllowed(FinalTracePath))
		|| (OutRecord.CopyMode == TEXT("copy")
			&& !IsStorePathAllowed(FinalTracePath)))
	{
		OutErrorCode = TEXT("trace_path_not_allowed");
		OutErrorMessage = OutRecord.CopyMode == TEXT("reference")
			? TEXT("The referenced trace is no longer inside a canonical configured Trace root.")
			: TEXT("The copied trace no longer resolves inside the canonical Worker store.");
		return false;
	}
	OutRecord.TracePath = MoveTemp(FinalTracePath);
	FString CurrentSha;
	int64 CurrentSize = 0;
	if (!HashFile(
			OutRecord.TracePath, CurrentSha, CurrentSize, OutErrorMessage)
		|| CurrentSha != OutRecord.Sha256
		|| CurrentSize != OutRecord.SizeBytes)
	{
		OutErrorCode = TEXT("trace_registry_conflict");
		OutErrorMessage = TEXT("The registered trace no longer matches its SHA-256 record.");
		return false;
	}
	return true;
}

bool FTraceStore::PersistAnalysisJob(
	const FString& AnalysisId,
	const FString& TraceId,
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Result,
	FString& OutError) const
{
	OutError.Reset();
	if (!IsSafeId(AnalysisId, TEXT("trace-analysis-local-"))
		|| !IsSafeId(TraceId, TEXT("trace-local-"))
		|| Capability.IsEmpty()
		|| !Result.IsValid())
	{
		OutError = TEXT("The analysis job record contains invalid identifiers.");
		return false;
	}
	FString StoreError;
	if (!IsAvailable(StoreError))
	{
		OutError = StoreError;
		return false;
	}
	const FString Destination = AnalysisRecordPath(AnalysisId);
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true))
	{
		OutError = TEXT("The analysis job directory could not be created.");
		return false;
	}
	TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
	Record->SetStringField(TEXT("schema"), TEXT("ue.trace-analysis-job.v1"));
	Record->SetStringField(TEXT("jobId"), AnalysisId);
	Record->SetStringField(TEXT("analysisId"), AnalysisId);
	Record->SetStringField(TEXT("traceId"), TraceId);
	Record->SetStringField(TEXT("capability"), Capability);
	Record->SetStringField(TEXT("status"), TEXT("completed"));
	Record->SetStringField(TEXT("phase"), TEXT("completed"));
	Record->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
	Record->SetObjectField(TEXT("result"), Result);

	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TSharedPtr<FJsonObject> ResultArtifact = MakeShared<FJsonObject>();
	ResultArtifact->SetStringField(TEXT("artifactId"), TEXT("result"));
	ResultArtifact->SetStringField(TEXT("kind"), TEXT("traceAnalysisResult"));
	ResultArtifact->SetStringField(TEXT("path"), Destination);
	ResultArtifact->SetStringField(TEXT("mimeType"), TEXT("application/json"));
	Artifacts.Add(MakeShared<FJsonValueObject>(ResultArtifact));
	const TSharedPtr<FJsonObject>* ExportArtifact = nullptr;
	if (Result->TryGetObjectField(TEXT("artifact"), ExportArtifact)
		&& ExportArtifact && ExportArtifact->IsValid())
	{
		FString ExportPath;
		if ((*ExportArtifact)->TryGetStringField(TEXT("path"), ExportPath)
			&& !ExportPath.IsEmpty()
			&& FPaths::IsUnderDirectory(
				FPaths::ConvertRelativePathToFull(ExportPath), Root))
		{
			TSharedPtr<FJsonObject> ExportSummary = MakeShared<FJsonObject>();
			ExportSummary->SetStringField(TEXT("artifactId"), TEXT("export"));
			ExportSummary->SetStringField(TEXT("kind"), TEXT("traceExport"));
			ExportSummary->SetStringField(TEXT("path"), ExportPath);
			FString MimeType;
			if (!(*ExportArtifact)->TryGetStringField(TEXT("mimeType"), MimeType)
				|| MimeType.IsEmpty())
			{
				MimeType = TEXT("application/octet-stream");
			}
			ExportSummary->SetStringField(TEXT("mimeType"), MimeType);
			ExportSummary->SetNumberField(
				TEXT("sizeBytes"), IFileManager::Get().FileSize(*ExportPath));
			Artifacts.Add(MakeShared<FJsonValueObject>(ExportSummary));
		}
	}
	Record->SetArrayField(TEXT("artifacts"), Artifacts);

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Record.ToSharedRef(), Writer))
	{
		OutError = TEXT("The analysis job result could not be serialized.");
		return false;
	}
	const FString Temporary = Destination + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(
			Serialized,
			*Temporary,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(
			*Destination, *Temporary, true, true, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("The analysis job result could not be published atomically.");
		return false;
	}
	return true;
}

bool FTraceStore::ResolveAnalysisJob(
	const FString& AnalysisId,
	TSharedPtr<FJsonObject>& OutRecord,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	OutRecord.Reset();
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	if (!IsSafeId(AnalysisId, TEXT("trace-analysis-local-")))
	{
		OutErrorCode = TEXT("job_id_invalid");
		OutErrorMessage = TEXT("The analysis job ID is invalid.");
		return false;
	}
	FString Serialized;
	const FString Path = AnalysisRecordPath(AnalysisId);
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size <= 0 || Size > 4 * 1024 * 1024
		|| !FFileHelper::LoadFileToString(Serialized, *Path))
	{
		OutErrorCode = TEXT("job_not_found");
		OutErrorMessage = TEXT("The analysis job was not found or exceeds its size bound.");
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
	FString Schema;
	FString JobId;
	if (!FJsonSerializer::Deserialize(Reader, OutRecord)
		|| !OutRecord.IsValid()
		|| !OutRecord->TryGetStringField(TEXT("schema"), Schema)
		|| Schema != TEXT("ue.trace-analysis-job.v1")
		|| !OutRecord->TryGetStringField(TEXT("jobId"), JobId)
		|| JobId != AnalysisId)
	{
		OutRecord.Reset();
		OutErrorCode = TEXT("trace_analysis_record_corrupt");
		OutErrorMessage = TEXT("The analysis job record is corrupt.");
		return false;
	}
	return true;
}

FString FTraceStore::MakeAnalysisId() const
{
	return TEXT("trace-analysis-local-")
		+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString FTraceStore::MakeExportPath(
	const FString& TraceId,
	const FString& Provider,
	const FString& Format) const
{
	FString SafeProvider = Provider;
	for (TCHAR& Character : SafeProvider)
	{
		if (!(FChar::IsAlnum(Character) || Character == TEXT('-')))
		{
			Character = TEXT('_');
		}
	}
	return FPaths::Combine(
		Root,
		TEXT("exports"),
		TraceId,
		FString::Printf(
			TEXT("%s-%s.%s"),
			*SafeProvider,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits),
			*Format));
}
}
