#include "TraceWorkerLaunch.h"

#include "TraceWorkerStore.h"
#include "TraceWorkerCommandLine.h"
#include "TraceAnalysisContracts.h"
#include "TraceAnalysisService.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

#if WITH_SSL
#include <openssl/sha.h>
#endif

#ifndef UEAI_TRACE_CONTRACT_DIGEST
#define UEAI_TRACE_CONTRACT_DIGEST "unbound"
#endif

#ifndef UEAI_TRACE_LAUNCH_PROFILES_DIGEST
#define UEAI_TRACE_LAUNCH_PROFILES_DIGEST "unbound"
#endif

#ifndef UEAI_TRACE_SOURCE_ROOT
#define UEAI_TRACE_SOURCE_ROOT ""
#endif

namespace UEAI::TraceWorker
{
namespace
{
constexpr int64 MaximumProfileBytes = 1024 * 1024;
constexpr int64 MaximumRecordBytes = 1024 * 1024;
constexpr int64 HashBufferBytes = 1024 * 1024;

int32 LaunchPhaseRank(const FString& Phase)
{
	if (Phase == TEXT("launching"))
	{
		return 0;
	}
	if (Phase == TEXT("loading"))
	{
		return 1;
	}
	if (Phase == TEXT("recording"))
	{
		return 2;
	}
	if (Phase == TEXT("finalizing"))
	{
		return 3;
	}
	if (Phase == TEXT("analyzing"))
	{
		return 4;
	}
	if (Phase == TEXT("completed"))
	{
		return 5;
	}
	return INDEX_NONE;
}

FCriticalSection OwnedProcessHandlesMutex;
TMap<FString, FProcHandle> OwnedProcessHandles;

void RetainOwnedProcessHandle(
	const FString& JobId,
	const FProcHandle& Process)
{
	FScopeLock Lock(&OwnedProcessHandlesMutex);
	OwnedProcessHandles.Add(JobId, Process);
}

void ForgetOwnedProcessHandle(const FString& JobId)
{
	FScopeLock Lock(&OwnedProcessHandlesMutex);
	if (FProcHandle* Process = OwnedProcessHandles.Find(JobId))
	{
		FPlatformProcess::CloseProc(*Process);
		OwnedProcessHandles.Remove(JobId);
	}
}

bool DuplicateOwnedProcessHandle(
	const FString& JobId,
	FProcHandle& OutProcess,
	bool& bOutMustClose)
{
	OutProcess = FProcHandle();
	bOutMustClose = false;
	FScopeLock Lock(&OwnedProcessHandlesMutex);
	const FProcHandle* Owned = OwnedProcessHandles.Find(JobId);
	if (!Owned || !Owned->IsValid())
	{
		return false;
	}
#if PLATFORM_WINDOWS
	HANDLE Duplicate = nullptr;
	if (!::DuplicateHandle(
			::GetCurrentProcess(),
			Owned->Get(),
			::GetCurrentProcess(),
			&Duplicate,
			0,
			false,
			DUPLICATE_SAME_ACCESS))
	{
		return false;
	}
	OutProcess = FProcHandle(Duplicate);
	bOutMustClose = true;
#else
	// Non-Windows durable PID attachment is intentionally unsupported, but a
	// resident Worker can safely observe its original CreateProc handle.
	OutProcess = *Owned;
#endif
	return true;
}

FString ReadProcessCreationIdentity(FProcHandle& Process)
{
#if PLATFORM_WINDOWS
	FILETIME CreationTime = {};
	FILETIME ExitTime = {};
	FILETIME KernelTime = {};
	FILETIME UserTime = {};
	if (!::GetProcessTimes(
			Process.Get(),
			&CreationTime,
			&ExitTime,
			&KernelTime,
			&UserTime))
	{
		return FString();
	}
	ULARGE_INTEGER Value = {};
	Value.LowPart = CreationTime.dwLowDateTime;
	Value.HighPart = CreationTime.dwHighDateTime;
	return LexToString(Value.QuadPart);
#else
	// Other platforms retain the original handle while the resident Worker is
	// alive. A durable force-stop is deliberately disabled until the platform
	// adapter can prove an equivalent process start identity.
	return FString();
#endif
}

FString CanonicalFilePath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString JsonString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Default = FString())
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Field, Value)
		? Value
		: Default;
}

double JsonNumber(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const double Default)
{
	double Value = Default;
	return Object.IsValid()
		&& Object->TryGetNumberField(Field, Value)
		&& FMath::IsFinite(Value)
		? Value
		: Default;
}

bool IsSafeId(const FString& Value, const TCHAR* Prefix)
{
	const bool bRequiresPrefix = Prefix != nullptr && Prefix[0] != TEXT('\0');
	if (Value.IsEmpty()
		|| (bRequiresPrefix && !Value.StartsWith(Prefix))
		|| Value.Len() > 128)
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

bool IsSafeCvarValue(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > 128)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!(FChar::IsAlnum(Character)
			|| Character == TEXT('.')
			|| Character == TEXT('-')
			|| Character == TEXT('_')
			|| Character == TEXT('/')))
		{
			return false;
		}
	}
	return true;
}

bool IsSafeGameMap(const FString& Value)
{
	if (!Value.StartsWith(TEXT("/Game/")) || Value.Len() > 512)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!(FChar::IsAlnum(Character)
			|| Character == TEXT('/')
			|| Character == TEXT('_')))
		{
			return false;
		}
	}
	return true;
}

bool IsSafeLaunchPath(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > 2048 || Value.Contains(TEXT("\"")))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (Character < 0x20)
		{
			return false;
		}
	}
	return true;
}

TArray<FString> PresetChannels(const FString& Preset)
{
	return UEAI::Trace::GetTracePresetChannels(Preset);
}

TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TSharedPtr<FJsonObject> ProviderStatusToJson(
	const UEAI::Trace::FTraceProviderStatus& Status)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Status.Descriptor.Id);
	Object->SetStringField(TEXT("displayName"), Status.Descriptor.DisplayName);
	Object->SetStringField(TEXT("insightsPanel"), Status.Descriptor.InsightsPanel);
	Object->SetArrayField(
		TEXT("operations"), StringsToJson(Status.Descriptor.Operations));
	Object->SetArrayField(
		TEXT("requiredChannels"),
		StringsToJson(Status.Descriptor.RequiredChannels));
	Object->SetBoolField(TEXT("recorded"), Status.bRecorded);
	Object->SetArrayField(
		TEXT("recordedProviderNames"),
		StringsToJson(Status.RecordedProviderNames));
	Object->SetArrayField(
		TEXT("missingProviderNames"),
		StringsToJson(Status.MissingProviderNames));
	Object->SetStringField(TEXT("channelStatus"), Status.ChannelStatus);
	Object->SetArrayField(
		TEXT("recordedChannels"), StringsToJson(Status.RecordedChannels));
	Object->SetArrayField(
		TEXT("missingChannels"), StringsToJson(Status.MissingChannels));
	Object->SetStringField(TEXT("unavailableReason"), Status.UnavailableReason);
	return Object;
}

TSharedPtr<FJsonValue> TraceValueToJson(
	const UEAI::Trace::FTraceValue& Value)
{
	using UEAI::Trace::ETraceValueType;
	switch (Value.Type)
	{
	case ETraceValueType::Boolean:
		return MakeShared<FJsonValueBoolean>(Value.BooleanValue);
	case ETraceValueType::Integer:
		return MakeShared<FJsonValueNumber>(
			static_cast<double>(Value.IntegerValue));
	case ETraceValueType::Number:
		if (FMath::IsFinite(Value.NumberValue))
		{
			return MakeShared<FJsonValueNumber>(Value.NumberValue);
		}
		return MakeShared<FJsonValueNull>();
	case ETraceValueType::String:
		return MakeShared<FJsonValueString>(Value.StringValue);
	default:
		return MakeShared<FJsonValueNull>();
	}
}

TSharedPtr<FJsonObject> QueryResultToJson(
	const UEAI::Trace::FTraceQueryResult& Result)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema"), Result.Schema);
	Object->SetStringField(TEXT("provider"), Result.Provider);
	Object->SetStringField(TEXT("operation"), Result.Operation);
	Object->SetNumberField(
		TEXT("intervalStartSeconds"), Result.IntervalStartSeconds);
	Object->SetNumberField(
		TEXT("intervalEndSeconds"), Result.IntervalEndSeconds);
	Object->SetNumberField(TEXT("total"), static_cast<double>(Result.TotalRows));
	Object->SetBoolField(TEXT("truncated"), Result.bTruncated);
	if (Result.bHasNextCursor)
	{
		Object->SetStringField(TEXT("nextCursor"), LexToString(Result.NextCursor));
	}
	else
	{
		Object->SetField(TEXT("nextCursor"), MakeShared<FJsonValueNull>());
	}
	Object->SetArrayField(TEXT("columns"), StringsToJson(Result.Columns));
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const UEAI::Trace::FTraceRow& Row : Result.Rows)
	{
		TSharedPtr<FJsonObject> RowObject = MakeShared<FJsonObject>();
		for (const FString& Column : Result.Columns)
		{
			const UEAI::Trace::FTraceValue* Value = Row.Fields.Find(Column);
			RowObject->SetField(
				Column,
				Value ? TraceValueToJson(*Value)
					: MakeShared<FJsonValueNull>());
		}
		Rows.Add(MakeShared<FJsonValueObject>(RowObject));
	}
	Object->SetArrayField(TEXT("rows"), Rows);
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const UEAI::Trace::FTraceDiagnostic& Diagnostic : Result.Diagnostics)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("severity"), Diagnostic.Severity);
		Value->SetStringField(TEXT("code"), Diagnostic.Code);
		Value->SetStringField(TEXT("message"), Diagnostic.Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Value));
	}
	Object->SetArrayField(TEXT("diagnostics"), Diagnostics);
	return Object;
}

TArray<FString> PostStopOperations(
	const FString& Provider,
	const bool bFull)
{
	return UEAI::Trace::GetTracePostStopOperations(Provider, bFull);
}

FString PlatformDirectory()
{
#if PLATFORM_WINDOWS
	return TEXT("Win64");
#elif PLATFORM_MAC
	return TEXT("Mac");
#else
	return TEXT("Linux");
#endif
}

FString DevelopmentExecutableName(const FString& ProjectName)
{
#if PLATFORM_WINDOWS
	return ProjectName + TEXT(".exe");
#else
	return ProjectName;
#endif
}

FString DebugGameExecutableName(const FString& ProjectName)
{
#if PLATFORM_WINDOWS
	return FString::Printf(TEXT("%s-Win64-DebugGame.exe"), *ProjectName);
#elif PLATFORM_MAC
	return FString::Printf(TEXT("%s-Mac-DebugGame"), *ProjectName);
#else
	return FString::Printf(TEXT("%s-Linux-DebugGame"), *ProjectName);
#endif
}

FString QuoteArgument(const FString& Value)
{
	// All callers pass validated package names or Windows paths (which cannot
	// contain quotes). Backslashes are path separators, not JSON escapes; do
	// not double them in a CreateProc command-line string.
	return TEXT("\"") + Value + TEXT("\"");
}

bool ReadBoundedJson(
	const FString& Path,
	const int64 MaximumBytes,
	TSharedPtr<FJsonObject>& OutObject)
{
	OutObject.Reset();
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size <= 0 || Size > MaximumBytes)
	{
		return false;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject)
		&& OutObject.IsValid();
}

bool ReadBoundedBytes(
	const FString& Path,
	const int64 MaximumBytes,
	TArray<uint8>& OutBytes)
{
	OutBytes.Reset();
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Reader(PlatformFile.OpenRead(*Path));
	if (!Reader.IsValid())
	{
		return false;
	}
	const int64 Size = Reader->Size();
	if (Size <= 0 || Size > MaximumBytes || Size > MAX_int32)
	{
		return false;
	}
	OutBytes.SetNumUninitialized(static_cast<int32>(Size));
	if (!Reader->Read(OutBytes.GetData(), Size))
	{
		OutBytes.Reset();
		return false;
	}
	return true;
}

bool HashBytesSha256(
	const TArray<uint8>& Bytes,
	FString& OutSha256)
{
	OutSha256.Reset();
#if !WITH_SSL
	return false;
#else
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	if (!::SHA256(Bytes.GetData(), Bytes.Num(), Digest))
	{
		return false;
	}
	OutSha256 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	return true;
#endif
}

bool ParseUtf8JsonBytes(
	const TArray<uint8>& Bytes,
	TSharedPtr<FJsonObject>& OutObject)
{
	OutObject.Reset();
	if (Bytes.IsEmpty())
	{
		return false;
	}
	const FUTF8ToTCHAR Converter(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		Bytes.Num());
	const FString Text(Converter.Length(), Converter.Get());
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, OutObject)
		&& OutObject.IsValid();
}

double SecondsSinceUtc(const FString& Timestamp)
{
	FDateTime Parsed;
	if (Timestamp.IsEmpty()
		|| !FDateTime::ParseIso8601(*Timestamp, Parsed))
	{
		return TNumericLimits<double>::Max();
	}
	return FMath::Max(
		0.0,
		(FDateTime::UtcNow() - Parsed).GetTotalSeconds());
}
}

FTraceWorkerLaunch::FTraceWorkerLaunch(
	const FString& InCommandLine,
	const FTraceStore& InTraceStore)
	: CommandLine(InCommandLine)
	, StoreRoot(InTraceStore.GetRoot())
	, TraceStore(InTraceStore)
{
}

bool FTraceWorkerLaunch::Handles(const FString& Capability) const
{
	return Capability == TEXT("production.trace.target.list")
		|| Capability == TEXT("production.trace.channel.list")
		|| Capability == TEXT("production.trace.launch.plan")
		|| Capability == TEXT("production.trace.start")
		|| Capability == TEXT("production.trace.status")
		|| Capability == TEXT("production.trace.stop")
		|| Capability == TEXT("production.job.status")
		|| Capability == TEXT("production.job.result.get")
		|| Capability == TEXT("production.job.log.get")
		|| Capability == TEXT("production.job.artifact.get")
		|| Capability == TEXT("production.job.cancel");
}

bool FTraceWorkerLaunch::Execute(
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Params,
	const FString& RequestId,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutData.Reset();
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	if (Capability.StartsWith(TEXT("production.job.")))
	{
		return RouteGenericJob(
			Capability, Params, OutData, OutErrorCode, OutErrorMessage);
	}
	if (Capability == TEXT("production.trace.target.list"))
	{
		return TargetList(OutData, OutErrorCode, OutErrorMessage);
	}
	if (Capability == TEXT("production.trace.channel.list"))
	{
		return ChannelList(Params, OutData, OutErrorCode, OutErrorMessage);
	}
	if (Capability == TEXT("production.trace.launch.plan"))
	{
		return LaunchPlan(Params, OutData, OutErrorCode, OutErrorMessage);
	}
	if (Capability == TEXT("production.trace.start"))
	{
		return Start(Params, RequestId, OutData, OutErrorCode, OutErrorMessage);
	}
	if (Capability == TEXT("production.trace.status"))
	{
		return Status(Params, OutData, OutErrorCode, OutErrorMessage);
	}
	return Stop(Params, OutData, OutErrorCode, OutErrorMessage);
}

bool FTraceWorkerLaunch::RouteGenericJob(
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	const FString JobId = JsonString(Params, TEXT("jobId"));
	if (JobId.StartsWith(TEXT("trace-launch-local-")))
	{
		TSharedPtr<FJsonObject> Routed = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
		{
			Routed->SetField(Pair.Key, Pair.Value);
		}
		Routed->SetStringField(TEXT("traceId"), JobId);
		if (Capability == TEXT("production.job.cancel"))
		{
			return Stop(Routed, OutData, OutErrorCode, OutErrorMessage);
		}
		if (Capability == TEXT("production.job.log.get"))
		{
			return ReadJobLog(Routed, OutData, OutErrorCode, OutErrorMessage);
		}
		if (Capability == TEXT("production.job.artifact.get"))
		{
			return ReadJobArtifact(Routed, OutData, OutErrorCode, OutErrorMessage);
		}
		if (!Status(Routed, OutData, OutErrorCode, OutErrorMessage))
		{
			return false;
		}
		OutData->SetStringField(TEXT("jobSchema"), TEXT("ue.job.v1"));
		OutData->SetBoolField(
			TEXT("terminal"), JsonString(OutData, TEXT("status")) != TEXT("running"));
		return true;
	}
	if (JobId.StartsWith(TEXT("trace-analysis-local-")))
	{
		TSharedPtr<FJsonObject> Record;
		if (!TraceStore.ResolveAnalysisJob(
				JobId, Record, OutErrorCode, OutErrorMessage))
		{
			return false;
		}
		if (Capability == TEXT("production.job.log.get"))
		{
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("schema"), TEXT("ue.job-log.v1"));
			OutData->SetStringField(TEXT("jobId"), JobId);
			OutData->SetStringField(TEXT("content"), FString());
			OutData->SetStringField(TEXT("nextCursor"), TEXT("0"));
			OutData->SetBoolField(TEXT("eof"), true);
			OutData->SetStringField(
				TEXT("note"),
				TEXT("Synchronous offline analysis has no process log."));
			return true;
		}
		if (Capability == TEXT("production.job.artifact.get"))
		{
			const FString ArtifactId = JsonString(Params, TEXT("artifactId"));
			const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
			if (!Record->TryGetArrayField(TEXT("artifacts"), Artifacts)
				|| !Artifacts)
			{
				OutErrorCode = TEXT("artifact_not_found");
				OutErrorMessage = TEXT("The analysis job has no registered artifacts.");
				return false;
			}
			TSharedPtr<FJsonObject> Selected;
			for (const TSharedPtr<FJsonValue>& Value : *Artifacts)
			{
				const TSharedPtr<FJsonObject> Candidate =
					Value.IsValid() && Value->Type == EJson::Object
					? Value->AsObject()
					: nullptr;
				if (Candidate.IsValid()
					&& JsonString(Candidate, TEXT("artifactId")) == ArtifactId)
				{
					Selected = Candidate;
					break;
				}
			}
			const FString ArtifactPath = JsonString(Selected, TEXT("path"));
			FString FullArtifactPath;
			const FString OwningDirectory = ArtifactId == TEXT("result")
				? FPaths::Combine(StoreRoot, TEXT("analyses"), JobId)
				: ArtifactId == TEXT("export")
					? FPaths::Combine(
						StoreRoot,
						TEXT("exports"),
						JsonString(Record, TEXT("traceId")))
					: FString();
			if (!Selected.IsValid()
				|| ArtifactPath.IsEmpty()
				|| OwningDirectory.IsEmpty()
				|| !FTraceStore::ResolveOwnedFileForRead(
					ArtifactPath,
					OwningDirectory,
					StoreRoot,
					FullArtifactPath))
			{
				OutErrorCode = TEXT("artifact_not_found");
				OutErrorMessage = TEXT("The requested analysis artifact is unavailable.");
				return false;
			}
			const int64 Offset = static_cast<int64>(
				JsonNumber(Params, TEXT("offset"), 0.0));
			const int32 MaxBytes = FMath::Clamp(
				static_cast<int32>(JsonNumber(
					Params, TEXT("maxBytes"), 1024.0 * 1024.0)),
				1,
				1024 * 1024);
			TUniquePtr<IFileHandle> File(
				FPlatformFileManager::Get().GetPlatformFile().OpenRead(
					*FullArtifactPath));
			if (!File.IsValid() || Offset < 0 || Offset > File->Size())
			{
				OutErrorCode = TEXT("artifact_offset_invalid");
				OutErrorMessage = TEXT("The requested analysis artifact offset is invalid.");
				return false;
			}
			const int32 Count = static_cast<int32>(
				FMath::Min<int64>(MaxBytes, File->Size() - Offset));
			TArray<uint8> Bytes;
			Bytes.SetNumUninitialized(Count);
			if (!File->Seek(Offset)
				|| (Count > 0 && !File->Read(Bytes.GetData(), Count)))
			{
				OutErrorCode = TEXT("artifact_unavailable");
				OutErrorMessage = TEXT("The analysis artifact chunk could not be read.");
				return false;
			}
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("schema"), TEXT("ue.job-artifact.v1"));
			OutData->SetStringField(TEXT("jobId"), JobId);
			OutData->SetStringField(TEXT("artifactId"), ArtifactId);
			OutData->SetStringField(
				TEXT("kind"), JsonString(Selected, TEXT("kind")));
			OutData->SetStringField(TEXT("path"), FullArtifactPath);
			OutData->SetNumberField(TEXT("sizeBytes"), File->Size());
			OutData->SetNumberField(TEXT("offset"), Offset);
			OutData->SetNumberField(TEXT("nextOffset"), Offset + Count);
			OutData->SetBoolField(TEXT("eof"), Offset + Count >= File->Size());
			OutData->SetStringField(TEXT("contentBase64"), FBase64::Encode(Bytes));
			return true;
		}

		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue.job.v1"));
		OutData->SetStringField(TEXT("jobId"), JobId);
		OutData->SetStringField(TEXT("analysisId"), JobId);
		OutData->SetStringField(TEXT("kind"), TEXT("traceAnalysis"));
		OutData->SetStringField(TEXT("status"), TEXT("completed"));
		OutData->SetStringField(TEXT("phase"), TEXT("completed"));
		OutData->SetBoolField(TEXT("terminal"), true);
		OutData->SetBoolField(TEXT("accepted"), false);
		OutData->SetBoolField(TEXT("canceled"), false);
		OutData->SetStringField(
			TEXT("traceId"), JsonString(Record, TEXT("traceId")));
		OutData->SetStringField(
			TEXT("capability"), JsonString(Record, TEXT("capability")));
		const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
		if (Record->TryGetArrayField(TEXT("artifacts"), Artifacts) && Artifacts)
		{
			OutData->SetArrayField(TEXT("artifacts"), *Artifacts);
		}
		if (Capability == TEXT("production.job.result.get"))
		{
			const TSharedPtr<FJsonObject>* Result = nullptr;
			if (Record->TryGetObjectField(TEXT("result"), Result)
				&& Result && Result->IsValid())
			{
				OutData->SetObjectField(TEXT("result"), *Result);
			}
		}
		return true;
	}

	if (!JobId.StartsWith(TEXT("trace-local-")))
	{
		OutErrorCode = TEXT("job_not_found");
		OutErrorMessage = TEXT("The local Trace Worker does not own this job ID.");
		return false;
	}
	FTraceRecord Record;
	if (!TraceStore.Resolve(JobId, Record, OutErrorCode, OutErrorMessage))
	{
		return false;
	}
	if (Capability == TEXT("production.job.log.get"))
	{
		OutErrorCode = TEXT("artifact_not_found");
		OutErrorMessage = TEXT("Imported Trace records do not have a process log.");
		return false;
	}
	if (Capability == TEXT("production.job.artifact.get"))
	{
		const FString ArtifactId = JsonString(Params, TEXT("artifactId"));
		if (ArtifactId != TEXT("trace") && ArtifactId != TEXT("utrace"))
		{
			OutErrorCode = TEXT("artifact_not_found");
			OutErrorMessage = TEXT("Only the registered 'trace' artifact belongs to this ID.");
			return false;
		}
		const int64 Offset = static_cast<int64>(JsonNumber(Params, TEXT("offset"), 0.0));
		const int32 MaxBytes = FMath::Clamp(
			static_cast<int32>(JsonNumber(Params, TEXT("maxBytes"), 1024.0 * 1024.0)),
			1,
			1024 * 1024);
		TUniquePtr<IFileHandle> File(
			FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Record.TracePath));
		if (!File.IsValid() || Offset < 0 || Offset > File->Size())
		{
			OutErrorCode = TEXT("artifact_offset_invalid");
			OutErrorMessage = TEXT("The requested Trace artifact offset is invalid.");
			return false;
		}
		const int32 Count = static_cast<int32>(
			FMath::Min<int64>(MaxBytes, File->Size() - Offset));
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Count);
		if (!File->Seek(Offset)
			|| (Count > 0 && !File->Read(Bytes.GetData(), Count)))
		{
			OutErrorCode = TEXT("artifact_unavailable");
			OutErrorMessage = TEXT("The Trace artifact chunk could not be read.");
			return false;
		}
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("jobId"), JobId);
		OutData->SetStringField(TEXT("artifactId"), TEXT("trace"));
		OutData->SetStringField(TEXT("kind"), TEXT("utrace"));
		OutData->SetStringField(TEXT("path"), Record.TracePath);
		OutData->SetNumberField(TEXT("sizeBytes"), Record.SizeBytes);
		OutData->SetNumberField(TEXT("offset"), Offset);
		OutData->SetNumberField(TEXT("nextOffset"), Offset + Count);
		OutData->SetBoolField(TEXT("eof"), Offset + Count >= Record.SizeBytes);
		OutData->SetStringField(TEXT("contentBase64"), FBase64::Encode(Bytes));
		return true;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue.job.v1"));
	OutData->SetStringField(TEXT("jobId"), JobId);
	OutData->SetStringField(TEXT("kind"), TEXT("traceImport"));
	OutData->SetStringField(TEXT("status"), TEXT("completed"));
	OutData->SetStringField(TEXT("phase"), TEXT("completed"));
	OutData->SetBoolField(TEXT("terminal"), true);
	OutData->SetBoolField(
		TEXT("accepted"), Capability != TEXT("production.job.cancel"));
	OutData->SetStringField(TEXT("traceId"), Record.TraceId);
	OutData->SetStringField(TEXT("sha256"), Record.Sha256);
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("artifactId"), TEXT("trace"));
	Artifact->SetStringField(TEXT("kind"), TEXT("utrace"));
	Artifact->SetStringField(TEXT("path"), Record.TracePath);
	Artifact->SetNumberField(TEXT("sizeBytes"), Record.SizeBytes);
	Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));
	OutData->SetArrayField(TEXT("artifacts"), Artifacts);
	return true;
}

FString FTraceWorkerLaunch::ResolveProfilesPath() const
{
	const FString BaseDirectory =
		FPaths::ConvertRelativePathToFull(FPlatformProcess::BaseDir());
	const FString AdjacentProfile =
		FPaths::Combine(BaseDirectory, TEXT("launch-profiles.json"));
	FString FinalAdjacentProfile;
	if (FTraceStore::ResolveOwnedFileForRead(
		AdjacentProfile,
		BaseDirectory,
		BaseDirectory,
		FinalAdjacentProfile))
	{
		return FinalAdjacentProfile;
	}
	// An installed bundle must use its adjacent, descriptor-owned profile. If
	// that file exists but could not be proven to remain in the bundle, do not
	// silently fall back to a source checkout.
	if (IFileManager::Get().FileExists(*AdjacentProfile))
	{
		return FString();
	}

	// Source fallback is intentionally limited to a Worker executable whose
	// own final path is inside the build-time source root. A copied/installed
	// Worker can therefore never consult a mutable source checkout when its
	// adjacent bundle resource is missing.
	const FString SourceRoot = TEXT(UEAI_TRACE_SOURCE_ROOT);
	if (SourceRoot.IsEmpty())
	{
		return FString();
	}
	const FString FullSourceRoot = FPaths::ConvertRelativePathToFull(SourceRoot);
	const FString ExecutablePath = FPlatformProcess::ExecutablePath();
	FString FinalSourceExecutable;
	if (!FTraceStore::ResolveOwnedFileForRead(
			ExecutablePath,
			BaseDirectory,
			FullSourceRoot,
			FinalSourceExecutable))
	{
		return FString();
	}
	const FString SourceProfileDirectory = FPaths::Combine(
		FullSourceRoot, TEXT("Resources/Trace"));
	const FString SourceProfile = FPaths::Combine(
		SourceProfileDirectory, TEXT("launch-profiles.json"));
	FString FinalSourceProfile;
	return FTraceStore::ResolveOwnedFileForRead(
			SourceProfile,
			SourceProfileDirectory,
			FullSourceRoot,
			FinalSourceProfile)
		? FinalSourceProfile
		: FString();
}

bool FTraceWorkerLaunch::LoadProfiles(
	TArray<FLaunchProfile>& OutProfiles,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	OutProfiles.Reset();
	const FString Path = ResolveProfilesPath();
	TArray<uint8> ProfileBytes;
	FString ProfilesSha256;
	const FString ExpectedProfilesSha256 =
		TEXT(UEAI_TRACE_LAUNCH_PROFILES_DIGEST);
	if (Path.IsEmpty()
		|| ExpectedProfilesSha256 == TEXT("unbound")
		|| !ReadBoundedBytes(Path, MaximumProfileBytes, ProfileBytes)
		|| !HashBytesSha256(ProfileBytes, ProfilesSha256)
		|| FString(TEXT("sha256:")) + ProfilesSha256
			!= ExpectedProfilesSha256)
	{
		OutErrorCode = TEXT("trace_launch_profiles_digest_mismatch");
		OutErrorMessage = TEXT(
			"The resolved Trace launch profile does not match the Worker build contract.");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!ParseUtf8JsonBytes(ProfileBytes, Root))
	{
		OutErrorCode = TEXT("trace_launch_profiles_unavailable");
		OutErrorMessage = TEXT(
			"The version-controlled Resources/Trace/launch-profiles.json could not be loaded.");
		return false;
	}
	if (JsonString(Root, TEXT("schema")) != TEXT("ue.trace-launch-profiles.v1"))
	{
		OutErrorCode = TEXT("trace_launch_profiles_invalid");
		OutErrorMessage = TEXT("The Trace launch profile schema is unsupported.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Root->TryGetArrayField(TEXT("profiles"), Values)
		|| !Values || Values->IsEmpty() || Values->Num() > 64)
	{
		OutErrorCode = TEXT("trace_launch_profiles_invalid");
		OutErrorMessage = TEXT("The Trace launch profile list is empty or unbounded.");
		return false;
	}
	TSet<FString> Ids;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid()
			? Value->AsObject() : nullptr;
		FLaunchProfile Profile;
		Profile.Id = JsonString(Object, TEXT("id"));
		Profile.ExecutableKind = JsonString(Object, TEXT("executableKind"));
		if (!IsSafeId(Profile.Id, TEXT(""))
			|| Ids.Contains(Profile.Id)
			|| Profile.ExecutableKind != TEXT("projectDevelopment"))
		{
			OutErrorCode = TEXT("trace_launch_profiles_invalid");
			OutErrorMessage = TEXT("A Trace launch profile identity is invalid.");
			return false;
		}
		Ids.Add(Profile.Id);
		auto ReadStrings = [&Object](const TCHAR* Field, TArray<FString>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Object->TryGetArrayField(Field, Array) || !Array)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Item : *Array)
			{
				FString String;
				if (!Item.IsValid() || !Item->TryGetString(String)
					|| String.IsEmpty() || String.Len() > 512)
				{
					return false;
				}
				Out.Add(String);
			}
			return !Out.IsEmpty();
		};
		TArray<FString> Cvars;
		if (!ReadStrings(TEXT("configurations"), Profile.Configurations)
			|| !ReadStrings(TEXT("allowedMaps"), Profile.AllowedMaps)
			|| !ReadStrings(TEXT("fixedArguments"), Profile.FixedArguments)
			|| !ReadStrings(TEXT("allowedCvars"), Cvars))
		{
			OutErrorCode = TEXT("trace_launch_profiles_invalid");
			OutErrorMessage = TEXT("A Trace launch profile array is invalid.");
			return false;
		}
		Profile.AllowedCvars.Append(Cvars);
		Profile.MaxDurationSeconds = FMath::Clamp(
			JsonNumber(Object, TEXT("maxDurationSeconds"), 3600.0), 1.0, 3600.0);
		Profile.MaxFileSizeMiB = static_cast<int64>(FMath::Clamp(
			JsonNumber(Object, TEXT("maxFileSizeMiB"), 16384.0), 1.0, 16384.0));
		Profile.StartupTimeoutSeconds = FMath::Clamp(
			JsonNumber(Object, TEXT("startupTimeoutSeconds"), 180.0),
			30.0,
			1800.0);
		Profile.ShutdownTimeoutSeconds = FMath::Clamp(
			JsonNumber(Object, TEXT("shutdownTimeoutSeconds"), 60.0), 1.0, 600.0);
		Object->TryGetBoolField(
			TEXT("allowForcedTermination"), Profile.bAllowForcedTermination);
		OutProfiles.Add(MoveTemp(Profile));
	}
	return true;
}

bool FTraceWorkerLaunch::TargetList(
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	TArray<FLaunchProfile> Profiles;
	const bool bProfilesAvailable =
		LoadProfiles(Profiles, OutErrorCode, OutErrorMessage);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue.trace-target-list.v1"));
	TArray<TSharedPtr<FJsonValue>> Targets;
	auto AddTarget = [&Targets](
		const FString& Kind, const bool bAvailable, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("kind"), Kind);
		Target->SetStringField(TEXT("backend"), TEXT("localTrace"));
		Target->SetBoolField(TEXT("available"), bAvailable);
		Target->SetStringField(TEXT("reason"), Reason);
		Targets.Add(MakeShared<FJsonValueObject>(Target));
	};
	AddTarget(TEXT("editor"), false,
		TEXT("Editor recording requires the Editor backend."));
	AddTarget(TEXT("pie"), false,
		TEXT("PIE recording requires the Editor backend."));
	AddTarget(TEXT("development"), bProfilesAvailable,
		bProfilesAvailable ? FString()
			: (!OutErrorMessage.IsEmpty()
				? OutErrorMessage
				: TEXT("No valid version-controlled launch profiles are installed.")));
	OutData->SetArrayField(TEXT("targets"), Targets);
	TArray<TSharedPtr<FJsonValue>> ProfileValues;
	for (const FLaunchProfile& Profile : Profiles)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("id"), Profile.Id);
		Value->SetStringField(TEXT("executableKind"), Profile.ExecutableKind);
		Value->SetNumberField(
			TEXT("maxDurationSeconds"), Profile.MaxDurationSeconds);
		Value->SetNumberField(
			TEXT("maxFileSizeMiB"), static_cast<double>(Profile.MaxFileSizeMiB));
		Value->SetNumberField(
			TEXT("startupTimeoutSeconds"), Profile.StartupTimeoutSeconds);
		ProfileValues.Add(MakeShared<FJsonValueObject>(Value));
	}
	OutData->SetArrayField(TEXT("launchProfiles"), ProfileValues);
	OutData->SetStringField(TEXT("launchProfilesPath"), ResolveProfilesPath());
	if (!bProfilesAvailable)
	{
		// Listing remains successful and reports the unavailable reason.
		OutErrorCode.Reset();
		OutErrorMessage.Reset();
	}
	return true;
}

bool FTraceWorkerLaunch::ChannelList(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	const FString TargetKind = JsonString(
		Params, TEXT("targetKind"), TEXT("development"));
	if (TargetKind != TEXT("editor")
		&& TargetKind != TEXT("pie")
		&& TargetKind != TEXT("development"))
	{
		OutErrorCode = TEXT("trace_target_invalid");
		OutErrorMessage = TEXT("targetKind must be editor, pie, or development.");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue.trace-channel-list.v1"));
	OutData->SetStringField(TEXT("targetKind"), TargetKind);
	OutData->SetStringField(TEXT("backend"), TEXT("localTrace"));
	OutData->SetBoolField(TEXT("available"), TargetKind == TEXT("development"));
	OutData->SetStringField(
		TEXT("reason"), TargetKind == TEXT("development")
			? FString() : TEXT("Editor and PIE channels require the Editor backend."));
	TArray<TSharedPtr<FJsonValue>> Presets;
	for (const FString& Name : {
		TEXT("standard"), TEXT("cpu"), TEXT("gpu"), TEXT("memory"),
		TEXT("loading"), TEXT("network"), TEXT("fullInsights")})
	{
		TSharedPtr<FJsonObject> Preset = MakeShared<FJsonObject>();
		const TArray<FString> Channels = PresetChannels(Name);
		const bool bRequiresStartupMemoryTrace =
			Channels.Contains(TEXT("memory"))
			|| Channels.Contains(TEXT("memalloc"));
		TArray<FString> StartupArguments;
		if (bRequiresStartupMemoryTrace)
		{
			StartupArguments.Add(TEXT("-trace=memory"));
		}
		if (Channels.Contains(TEXT("net")))
		{
			StartupArguments.Add(TEXT("-NetTrace=1"));
		}
		Preset->SetStringField(TEXT("id"), Name);
		Preset->SetArrayField(TEXT("channels"), StringsToJson(Channels));
		Preset->SetBoolField(TEXT("available"), TargetKind == TEXT("development"));
		Preset->SetBoolField(
			TEXT("requiresProcessStartup"), bRequiresStartupMemoryTrace);
		Preset->SetArrayField(
			TEXT("startupArguments"),
			StringsToJson(StartupArguments));
		Presets.Add(MakeShared<FJsonValueObject>(Preset));
	}
	OutData->SetArrayField(TEXT("presets"), Presets);
	return true;
}

bool FTraceWorkerLaunch::ResolveProjectExecutable(
	const FString& ProjectPath,
	const FLaunchProfile& Profile,
	FString& OutExecutable,
	FString& OutConfiguration) const
{
	const FString ProjectDirectory = FPaths::GetPath(ProjectPath);
	const FString ProjectName = FPaths::GetBaseFilename(ProjectPath);
	for (const FString& Configuration : Profile.Configurations)
	{
		FString Name;
		if (Configuration == TEXT("Development"))
		{
			Name = DevelopmentExecutableName(ProjectName);
		}
		else if (Configuration == TEXT("DebugGame"))
		{
			Name = DebugGameExecutableName(ProjectName);
		}
		else
		{
			continue;
		}
		const FString Candidate = FPaths::Combine(
			ProjectDirectory, TEXT("Binaries"), PlatformDirectory(), Name);
		if (IFileManager::Get().FileExists(*Candidate))
		{
			OutExecutable = FPaths::ConvertRelativePathToFull(Candidate);
			OutConfiguration = Configuration;
			return true;
		}
	}
	return false;
}

bool FTraceWorkerLaunch::HashFile(
	const FString& Path,
	FString& OutSha256,
	FString& OutError) const
{
	OutSha256.Reset();
	OutError.Reset();
#if !WITH_SSL
	OutError = TEXT("The Trace Worker was built without SHA-256 support.");
	return false;
#else
	IPlatformFile& PlatformFile =
		FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> Reader(PlatformFile.OpenRead(*Path));
	if (!Reader.IsValid() || Reader->Size() <= 0)
	{
		OutError = TEXT("A Launch Plan input could not be opened for hashing.");
		return false;
	}
	SHA256_CTX Context;
	if (::SHA256_Init(&Context) != 1)
	{
		OutError = TEXT("SHA-256 initialization failed.");
		return false;
	}
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(static_cast<int32>(HashBufferBytes));
	int64 Remaining = Reader->Size();
	while (Remaining > 0)
	{
		const int64 Count = FMath::Min<int64>(Remaining, Buffer.Num());
		if (!Reader->Read(Buffer.GetData(), Count)
			|| ::SHA256_Update(&Context, Buffer.GetData(), Count) != 1)
		{
			OutError = TEXT("A Launch Plan input could not be hashed completely.");
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
	return true;
#endif
}

bool FTraceWorkerLaunch::HashText(
	const FString& Text,
	FString& OutSha256) const
{
#if !WITH_SSL
	OutSha256.Reset();
	return false;
#else
	const FTCHARToUTF8 Utf8(*Text);
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	if (!::SHA256(
			reinterpret_cast<const uint8*>(Utf8.Get()),
			Utf8.Length(),
			Digest))
	{
		OutSha256.Reset();
		return false;
	}
	OutSha256 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	return true;
#endif
}

FString FTraceWorkerLaunch::CanonicalPlan(const FLaunchPlan& Plan) const
{
	TArray<FString> CvarNames;
	Plan.Cvars.GetKeys(CvarNames);
	CvarNames.Sort();
	TArray<FString> Fields = {
		TEXT("schema=ue.trace-launch-plan.v1"),
		TEXT("contract=") TEXT(UEAI_TRACE_CONTRACT_DIGEST),
		TEXT("engine=") + FEngineVersion::Current().ToString(),
		TEXT("profile=") + Plan.ProfileId,
		TEXT("project=") + Plan.ProjectPath,
		TEXT("projectSha256=") + Plan.ProjectSha256,
		TEXT("map=") + Plan.Map,
		TEXT("preset=") + Plan.Preset,
		TEXT("executable=") + Plan.ExecutablePath,
		TEXT("executableSha256=") + Plan.ExecutableSha256,
		TEXT("configuration=") + Plan.Configuration,
		TEXT("postStop=") + Plan.PostStop,
		TEXT("channels=") + FString::Join(Plan.Channels, TEXT(",")),
		TEXT("fixedArguments=") + FString::Join(Plan.FixedArguments, TEXT("\n")),
		FString::Printf(TEXT("maxDuration=%.6f"), Plan.MaxDurationSeconds),
		FString::Printf(TEXT("maxFileSize=%lld"), Plan.MaxFileSizeMiB),
		FString::Printf(TEXT("startupTimeout=%.6f"), Plan.StartupTimeoutSeconds),
		FString::Printf(TEXT("shutdownTimeout=%.6f"), Plan.ShutdownTimeoutSeconds),
		FString::Printf(TEXT("allowForce=%d"), Plan.bAllowForcedTermination ? 1 : 0)};
	for (const FString& Name : CvarNames)
	{
		Fields.Add(TEXT("cvar:") + Name + TEXT("=") + Plan.Cvars[Name]);
	}
	FString Canonical;
	for (const FString& Field : Fields)
	{
		Canonical += FString::Printf(TEXT("%d:%s\n"), Field.Len(), *Field);
	}
	return Canonical;
}

FString FTraceWorkerLaunch::PlanPath(const FString& Digest) const
{
	return FPaths::Combine(
		StoreRoot, TEXT("launch-plans"), Digest.RightChop(7) + TEXT(".json"));
}

FString FTraceWorkerLaunch::JobPath(const FString& JobId) const
{
	return FPaths::Combine(StoreRoot, TEXT("launch-jobs"), JobId + TEXT(".json"));
}

FString FTraceWorkerLaunch::RequestPath(const FString& RequestId) const
{
	FString Digest;
	if (!HashText(RequestId, Digest))
	{
		return FString();
	}
	return FPaths::Combine(
		StoreRoot, TEXT("launch-requests"), Digest + TEXT(".json"));
}

TSharedPtr<FJsonObject> FTraceWorkerLaunch::EffectiveConfig(
	const FLaunchPlan& Plan) const
{
	TSharedPtr<FJsonObject> Effective = MakeShared<FJsonObject>();
	Effective->SetStringField(TEXT("launchProfileId"), Plan.ProfileId);
	Effective->SetStringField(TEXT("project"), Plan.ProjectPath);
	Effective->SetStringField(TEXT("projectSha256"), Plan.ProjectSha256);
	Effective->SetStringField(TEXT("map"), Plan.Map);
	Effective->SetStringField(TEXT("preset"), Plan.Preset);
	Effective->SetStringField(TEXT("postStop"), Plan.PostStop);
	Effective->SetArrayField(TEXT("channels"), StringsToJson(Plan.Channels));
	TSharedPtr<FJsonObject> Cvars = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Plan.Cvars)
	{
		Cvars->SetStringField(Pair.Key, Pair.Value);
	}
	Effective->SetObjectField(TEXT("cvars"), Cvars);
	Effective->SetNumberField(
		TEXT("maxDurationSeconds"), Plan.MaxDurationSeconds);
	Effective->SetNumberField(
		TEXT("maxFileSizeMiB"), static_cast<double>(Plan.MaxFileSizeMiB));
	Effective->SetBoolField(TEXT("excludeTail"), true);
	Effective->SetStringField(TEXT("executable"), Plan.ExecutablePath);
	Effective->SetStringField(
		TEXT("executableSha256"), Plan.ExecutableSha256);
	Effective->SetArrayField(
		TEXT("fixedArguments"), StringsToJson(Plan.FixedArguments));
	return Effective;
}

bool FTraceWorkerLaunch::ValidateStartMatchesPlan(
	const TSharedPtr<FJsonObject>& Params,
	const FLaunchPlan& Plan,
	FString& OutError) const
{
	OutError.Reset();
	auto Mismatch = [&OutError](const TCHAR* Field)
	{
		OutError = FString::Printf(
			TEXT("Development start field '%s' does not match the approved Launch Plan."),
			Field);
		return false;
	};
	if (Params->HasField(TEXT("preset"))
		&& JsonString(Params, TEXT("preset")) != Plan.Preset)
	{
		return Mismatch(TEXT("preset"));
	}
	if (Params->HasField(TEXT("postStop"))
		&& JsonString(Params, TEXT("postStop")) != Plan.PostStop)
	{
		return Mismatch(TEXT("postStop"));
	}
	if (Params->HasField(TEXT("project"))
		&& !FPaths::IsSamePath(
			CanonicalFilePath(JsonString(Params, TEXT("project"))),
			CanonicalFilePath(Plan.ProjectPath)))
	{
		return Mismatch(TEXT("project"));
	}
	if (Params->HasField(TEXT("maxDurationSeconds"))
		&& !FMath::IsNearlyEqual(
			JsonNumber(Params, TEXT("maxDurationSeconds"), -1.0),
			Plan.MaxDurationSeconds,
			0.000001))
	{
		return Mismatch(TEXT("maxDurationSeconds"));
	}
	if (Params->HasField(TEXT("maxFileSizeMiB"))
		&& static_cast<int64>(JsonNumber(
			Params, TEXT("maxFileSizeMiB"), -1.0)) != Plan.MaxFileSizeMiB)
	{
		return Mismatch(TEXT("maxFileSizeMiB"));
	}
	bool bExcludeTail = true;
	if (Params->TryGetBoolField(TEXT("excludeTail"), bExcludeTail)
		&& !bExcludeTail)
	{
		return Mismatch(TEXT("excludeTail"));
	}
	const TArray<TSharedPtr<FJsonValue>>* RequestedChannels = nullptr;
	if (Params->TryGetArrayField(TEXT("channels"), RequestedChannels))
	{
		if (!RequestedChannels
			|| RequestedChannels->Num() != Plan.Channels.Num())
		{
			return Mismatch(TEXT("channels"));
		}
		for (int32 Index = 0; Index < RequestedChannels->Num(); ++Index)
		{
			if (!(*RequestedChannels)[Index].IsValid()
				|| (*RequestedChannels)[Index]->Type != EJson::String
				|| (*RequestedChannels)[Index]->AsString() != Plan.Channels[Index])
			{
				return Mismatch(TEXT("channels"));
			}
		}
	}
	const TSharedPtr<FJsonObject>* RequestedCvars = nullptr;
	if (Params->TryGetObjectField(TEXT("cvars"), RequestedCvars))
	{
		if (!RequestedCvars || !RequestedCvars->IsValid()
			|| (*RequestedCvars)->Values.Num() != Plan.Cvars.Num())
		{
			return Mismatch(TEXT("cvars"));
		}
		for (const TPair<FString, FString>& Pair : Plan.Cvars)
		{
			const TSharedPtr<FJsonValue>* Requested =
				(*RequestedCvars)->Values.Find(Pair.Key);
			FString RequestedValue;
			if (!Requested || !Requested->IsValid())
			{
				return Mismatch(TEXT("cvars"));
			}
			if ((*Requested)->Type == EJson::Boolean)
			{
				RequestedValue = (*Requested)->AsBool() ? TEXT("1") : TEXT("0");
			}
			else if ((*Requested)->Type == EJson::Number)
			{
				RequestedValue = LexToString((*Requested)->AsNumber());
			}
			else if ((*Requested)->Type == EJson::String)
			{
				RequestedValue = (*Requested)->AsString();
			}
			if (RequestedValue != Pair.Value)
			{
				return Mismatch(TEXT("cvars"));
			}
		}
	}
	return true;
}

bool FTraceWorkerLaunch::ClaimRequest(
	const FString& RequestId,
	const FString& PlanDigest,
	const FString& JobId,
	bool& bOutCreated,
	FString& OutExistingPlanDigest,
	FString& OutExistingJobId,
	FString& OutError) const
{
	bOutCreated = false;
	OutExistingPlanDigest.Reset();
	OutExistingJobId.Reset();
	OutError.Reset();
	const FString Path = RequestPath(RequestId);
	if (Path.IsEmpty()
		|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
	{
		OutError = TEXT("The persistent request-id registry is unavailable.");
		return false;
	}
	auto ReadExisting = [&]()
	{
		TSharedPtr<FJsonObject> Existing;
		if (!ReadBoundedJson(Path, MaximumRecordBytes, Existing)
			|| JsonString(Existing, TEXT("schema"))
				!= TEXT("ue.trace-launch-request.v1")
			|| JsonString(Existing, TEXT("requestId")) != RequestId)
		{
			OutError = TEXT("The persistent request-id record is corrupt or incomplete.");
			return false;
		}
		OutExistingPlanDigest = JsonString(Existing, TEXT("planDigest"));
		OutExistingJobId = JsonString(Existing, TEXT("jobId"));
		return !OutExistingPlanDigest.IsEmpty() && !OutExistingJobId.IsEmpty();
	};
	if (IFileManager::Get().FileExists(*Path))
	{
		return ReadExisting();
	}
	TSharedPtr<FJsonObject> Claim = MakeShared<FJsonObject>();
	Claim->SetStringField(TEXT("schema"), TEXT("ue.trace-launch-request.v1"));
	Claim->SetStringField(TEXT("requestId"), RequestId);
	Claim->SetStringField(TEXT("planDigest"), PlanDigest);
	Claim->SetStringField(TEXT("jobId"), JobId);
	Claim->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Claim.ToSharedRef(), Writer))
	{
		OutError = TEXT("The persistent request-id record could not be serialized.");
		return false;
	}
	TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileWriter(
		*Path, FILEWRITE_NoReplaceExisting));
	if (!Archive)
	{
		// Another Worker may have won the no-replace race.
		return ReadExisting();
	}
	const FTCHARToUTF8 Utf8(*Text);
	Archive->Serialize(
		const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
	Archive->Close();
	if (Archive->IsError())
	{
		OutError = TEXT("The persistent request-id record could not be written completely.");
		return false;
	}
	bOutCreated = true;
	OutExistingPlanDigest = PlanDigest;
	OutExistingJobId = JobId;
	return true;
}

bool FTraceWorkerLaunch::PublishJson(
	const FString& Path,
	const TSharedPtr<FJsonObject>& Object,
	FString& OutError) const
{
	OutError.Reset();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		OutError = TEXT("A Trace launch record could not be serialized.");
		return false;
	}
	const FString Temporary = Path + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(
			Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*Path, *Temporary, true, true, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("A Trace launch record could not be published atomically.");
		return false;
	}
	return true;
}

bool FTraceWorkerLaunch::OpenVerifiedProcess(
	FLaunchJob& Job,
	FProcHandle& OutProcess,
	bool& bOutMustClose,
	FString& OutReason) const
{
	OutProcess = FProcHandle();
	bOutMustClose = false;
	OutReason.Reset();
	if (Job.ProcessId == 0)
	{
		OutReason = TEXT("missingProcessId");
		return false;
	}

	const bool bUsingOwnedHandle = DuplicateOwnedProcessHandle(
		Job.JobId, OutProcess, bOutMustClose);
	if (!bUsingOwnedHandle)
	{
		OutProcess = FPlatformProcess::OpenProcess(Job.ProcessId);
		bOutMustClose = OutProcess.IsValid();
	}
	if (!OutProcess.IsValid())
	{
		OutReason = TEXT("processUnavailable");
		return false;
	}
	if (bUsingOwnedHandle && !FPlatformProcess::IsProcRunning(OutProcess))
	{
		// A signaled original CreateProc handle is definitive exit evidence even
		// if the executable file was replaced after launch.
		Job.ProcessIdentityStatus = TEXT("verified");
		return true;
	}

	const FString ActualPath = CanonicalFilePath(
		FPlatformProcess::GetApplicationName(Job.ProcessId));
	const FString ExpectedPath = CanonicalFilePath(Job.ExecutablePath);
	const FString ActualCreationIdentity = ReadProcessCreationIdentity(OutProcess);
	FString ActualSha256;
	FString HashError;
	const bool bHashMatches = !ExpectedPath.IsEmpty()
		&& HashFile(ExpectedPath, ActualSha256, HashError)
		&& ActualSha256 == Job.ExecutableSha256;
	const bool bPathMatches = !ActualPath.IsEmpty()
		&& FPaths::IsSamePath(ActualPath, ExpectedPath);
	const bool bCreationMatches =
#if PLATFORM_WINDOWS
		!Job.ProcessCreationIdentity.IsEmpty()
		&& ActualCreationIdentity == Job.ProcessCreationIdentity;
#else
		Job.ProcessIdentityStatus == TEXT("residentOnly")
		&& Job.ProcessCreationIdentity.IsEmpty();
#endif
	if (!bPathMatches || !bHashMatches || !bCreationMatches)
	{
		Job.ProcessIdentityStatus = TEXT("mismatch");
		OutReason = FString::Printf(
			TEXT("path=%s hash=%s creation=%s"),
			bPathMatches ? TEXT("match") : TEXT("mismatch"),
			bHashMatches ? TEXT("match") : TEXT("mismatch"),
			bCreationMatches ? TEXT("match") : TEXT("mismatch"));
		return false;
	}
	Job.ProcessIdentityStatus = TEXT("verified");
	return true;
}

void FTraceWorkerLaunch::StopUntrackedProcess(
	const FLaunchJob& Job,
	FProcHandle& Process) const
{
	if (!Process.IsValid())
	{
		return;
	}
	TSharedPtr<FJsonObject> StopRequest = MakeShared<FJsonObject>();
	StopRequest->SetStringField(TEXT("schema"), TEXT("ue.trace-runtime-stop.v1"));
	StopRequest->SetStringField(TEXT("jobId"), Job.JobId);
	StopRequest->SetStringField(TEXT("nonce"), Job.StopNonce);
	FString IgnoredError;
	PublishJson(Job.StopPath, StopRequest, IgnoredError);

	const double Deadline = FPlatformTime::Seconds()
		+ FMath::Clamp(Job.ShutdownTimeoutSeconds, 1.0, 60.0);
	while (FPlatformProcess::IsProcRunning(Process)
		&& FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::Sleep(0.05f);
	}
	if (FPlatformProcess::IsProcRunning(Process))
	{
		// This launch was never durably accepted. The original CreateProc handle
		// is proof of ownership, so fail closed instead of orphaning a process
		// that can no longer be addressed safely by the Worker.
		FPlatformProcess::TerminateProc(Process, true);
		const double TerminationDeadline = FPlatformTime::Seconds() + 5.0;
		while (FPlatformProcess::IsProcRunning(Process)
			&& FPlatformTime::Seconds() < TerminationDeadline)
		{
			FPlatformProcess::Sleep(0.05f);
		}
	}
	FPlatformProcess::CloseProc(Process);
}

bool FTraceWorkerLaunch::SavePlan(
	const FLaunchPlan& Plan,
	FString& OutError) const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema"), TEXT("ue.trace-launch-plan-record.v1"));
	Object->SetStringField(TEXT("digest"), Plan.Digest);
	Object->SetStringField(TEXT("profileId"), Plan.ProfileId);
	Object->SetStringField(TEXT("projectPath"), Plan.ProjectPath);
	Object->SetStringField(TEXT("projectSha256"), Plan.ProjectSha256);
	Object->SetStringField(TEXT("map"), Plan.Map);
	Object->SetStringField(TEXT("preset"), Plan.Preset);
	Object->SetStringField(TEXT("executablePath"), Plan.ExecutablePath);
	Object->SetStringField(TEXT("executableSha256"), Plan.ExecutableSha256);
	Object->SetStringField(TEXT("configuration"), Plan.Configuration);
	Object->SetStringField(TEXT("postStop"), Plan.PostStop);
	Object->SetArrayField(TEXT("channels"), StringsToJson(Plan.Channels));
	Object->SetArrayField(TEXT("fixedArguments"), StringsToJson(Plan.FixedArguments));
	TSharedPtr<FJsonObject> Cvars = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Plan.Cvars)
	{
		Cvars->SetStringField(Pair.Key, Pair.Value);
	}
	Object->SetObjectField(TEXT("cvars"), Cvars);
	Object->SetNumberField(TEXT("maxDurationSeconds"), Plan.MaxDurationSeconds);
	Object->SetNumberField(
		TEXT("maxFileSizeMiB"), static_cast<double>(Plan.MaxFileSizeMiB));
	Object->SetNumberField(
		TEXT("startupTimeoutSeconds"), Plan.StartupTimeoutSeconds);
	Object->SetNumberField(
		TEXT("shutdownTimeoutSeconds"), Plan.ShutdownTimeoutSeconds);
	Object->SetBoolField(
		TEXT("allowForcedTermination"), Plan.bAllowForcedTermination);
	return PublishJson(PlanPath(Plan.Digest), Object, OutError);
}

bool FTraceWorkerLaunch::LoadPlan(
	const FString& Digest,
	FLaunchPlan& OutPlan,
	FString& OutError) const
{
	if (!Digest.StartsWith(TEXT("sha256:")) || Digest.Len() != 71)
	{
		OutError = TEXT("The Launch Plan digest is malformed.");
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	if (!ReadBoundedJson(PlanPath(Digest), MaximumRecordBytes, Object)
		|| JsonString(Object, TEXT("schema"))
			!= TEXT("ue.trace-launch-plan-record.v1"))
	{
		OutError = TEXT("The approved Launch Plan is not registered in this Worker store.");
		return false;
	}
	OutPlan.Digest = JsonString(Object, TEXT("digest"));
	OutPlan.ProfileId = JsonString(Object, TEXT("profileId"));
	OutPlan.ProjectPath = JsonString(Object, TEXT("projectPath"));
	OutPlan.ProjectSha256 = JsonString(Object, TEXT("projectSha256"));
	OutPlan.Map = JsonString(Object, TEXT("map"));
	OutPlan.Preset = JsonString(Object, TEXT("preset"));
	OutPlan.ExecutablePath = JsonString(Object, TEXT("executablePath"));
	OutPlan.ExecutableSha256 = JsonString(Object, TEXT("executableSha256"));
	OutPlan.Configuration = JsonString(Object, TEXT("configuration"));
	OutPlan.PostStop = JsonString(
		Object, TEXT("postStop"), TEXT("artifactOnly"));
	OutPlan.MaxDurationSeconds = JsonNumber(
		Object, TEXT("maxDurationSeconds"), 120.0);
	OutPlan.MaxFileSizeMiB = static_cast<int64>(JsonNumber(
		Object, TEXT("maxFileSizeMiB"), 4096.0));
	OutPlan.StartupTimeoutSeconds = JsonNumber(
		Object, TEXT("startupTimeoutSeconds"), 180.0);
	OutPlan.ShutdownTimeoutSeconds = JsonNumber(
		Object, TEXT("shutdownTimeoutSeconds"), 60.0);
	Object->TryGetBoolField(
		TEXT("allowForcedTermination"), OutPlan.bAllowForcedTermination);
	auto ReadStrings = [&Object](const TCHAR* Field, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(Field, Values) || !Values)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid() || !Value->TryGetString(Text))
			{
				return false;
			}
			Out.Add(Text);
		}
		return true;
	};
	const TSharedPtr<FJsonObject>* Cvars = nullptr;
	if (!ReadStrings(TEXT("channels"), OutPlan.Channels)
		|| !ReadStrings(TEXT("fixedArguments"), OutPlan.FixedArguments)
		|| !Object->TryGetObjectField(TEXT("cvars"), Cvars)
		|| !Cvars || !Cvars->IsValid())
	{
		OutError = TEXT("The approved Launch Plan record is corrupt.");
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Cvars)->Values)
	{
		FString Value;
		if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Value))
		{
			OutError = TEXT("The approved Launch Plan CVar record is corrupt.");
			return false;
		}
		OutPlan.Cvars.Add(Pair.Key, Value);
	}
	FString ActualDigest;
	if (!HashText(CanonicalPlan(OutPlan), ActualDigest)
		|| OutPlan.Digest != Digest
		|| Digest != TEXT("sha256:") + ActualDigest)
	{
		OutError = TEXT("The approved Launch Plan digest no longer matches its record.");
		return false;
	}
	return true;
}

bool FTraceWorkerLaunch::LaunchPlan(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	TArray<FLaunchProfile> Profiles;
	if (!LoadProfiles(Profiles, OutErrorCode, OutErrorMessage))
	{
		return false;
	}
	const FString ProfileId = JsonString(Params, TEXT("launchProfileId"));
	const FLaunchProfile* Profile = Profiles.FindByPredicate(
		[&ProfileId](const FLaunchProfile& Candidate)
		{
			return Candidate.Id == ProfileId;
		});
	if (!Profile)
	{
		OutErrorCode = TEXT("trace_launch_profile_not_found");
		OutErrorMessage = TEXT("launchProfileId is not present in the installed profile file.");
		return false;
	}
	FLaunchPlan Plan;
	Plan.ProfileId = Profile->Id;
	Plan.ProjectPath = FPaths::ConvertRelativePathToFull(
		JsonString(Params, TEXT("project")));
	Plan.Map = JsonString(Params, TEXT("map"));
	Plan.Preset = JsonString(Params, TEXT("preset"), TEXT("standard"));
	Plan.PostStop = JsonString(
		Params, TEXT("postStop"), TEXT("artifactOnly"));
	Plan.Channels = PresetChannels(Plan.Preset);
	Plan.FixedArguments = Profile->FixedArguments;
	if (Plan.Channels.Contains(TEXT("memory"))
		|| Plan.Channels.Contains(TEXT("memalloc")))
	{
		// MemoryTrace_Create installs the allocation-tracing wrapper before
		// modules load and only when the process command line contains this
		// startup argument. It is therefore part of the approved, digested
		// launch configuration rather than a late runtime channel toggle.
		Plan.FixedArguments.AddUnique(TEXT("-trace=memory"));
	}
	if (Plan.Channels.Contains(TEXT("net")))
	{
		// The Net channel records transport events only after the runtime Net
		// trace verbosity is enabled. Keep this fixed switch in the approved
		// Launch Plan instead of hiding it in the fixture or child process.
		Plan.FixedArguments.AddUnique(TEXT("-NetTrace=1"));
	}
	Plan.MaxDurationSeconds = FMath::Clamp(
		JsonNumber(Params, TEXT("maxDurationSeconds"), 120.0),
		1.0,
		Profile->MaxDurationSeconds);
	Plan.MaxFileSizeMiB = static_cast<int64>(FMath::Clamp(
		JsonNumber(Params, TEXT("maxFileSizeMiB"), 4096.0),
		1.0,
		static_cast<double>(Profile->MaxFileSizeMiB)));
	Plan.StartupTimeoutSeconds = Profile->StartupTimeoutSeconds;
	Plan.ShutdownTimeoutSeconds = Profile->ShutdownTimeoutSeconds;
	Plan.bAllowForcedTermination = Profile->bAllowForcedTermination;
	if (!IsSafeLaunchPath(Plan.ProjectPath)
		|| !Plan.ProjectPath.EndsWith(TEXT(".uproject"), ESearchCase::IgnoreCase)
		|| !IFileManager::Get().FileExists(*Plan.ProjectPath)
		|| !IsSafeGameMap(Plan.Map)
		|| Plan.Channels.IsEmpty()
		|| (Plan.PostStop != TEXT("artifactOnly")
			&& Plan.PostStop != TEXT("analyzeSummary")
			&& Plan.PostStop != TEXT("analyzeFull")))
	{
		OutErrorCode = TEXT("trace_launch_plan_invalid");
		OutErrorMessage = TEXT("Project, map, or Trace preset is invalid.");
		return false;
	}
	bool bMapAllowed = false;
	for (const FString& Pattern : Profile->AllowedMaps)
	{
		if ((Pattern.EndsWith(TEXT("*"))
				&& Plan.Map.StartsWith(Pattern.LeftChop(1)))
			|| Pattern == Plan.Map)
		{
			bMapAllowed = true;
			break;
		}
	}
	if (!bMapAllowed)
	{
		OutErrorCode = TEXT("trace_launch_map_not_allowed");
		OutErrorMessage = TEXT("The requested map is outside the Launch Profile allowlist.");
		return false;
	}
	if (!ResolveProjectExecutable(
			Plan.ProjectPath, *Profile, Plan.ExecutablePath, Plan.Configuration))
	{
		OutErrorCode = TEXT("trace_launch_executable_unavailable");
		OutErrorMessage = TEXT(
			"No built Development or DebugGame project executable matches the Launch Profile.");
		return false;
	}
	const TSharedPtr<FJsonObject>* Cvars = nullptr;
	if (Params->TryGetObjectField(TEXT("cvars"), Cvars)
		&& Cvars && Cvars->IsValid())
	{
		if ((*Cvars)->Values.Num() > 64)
		{
			OutErrorCode = TEXT("trace_launch_cvar_invalid");
			OutErrorMessage = TEXT("A Launch Plan may set at most 64 CVars.");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Cvars)->Values)
		{
			FString Value;
			if (!Profile->AllowedCvars.Contains(Pair.Key) || !Pair.Value.IsValid())
			{
				OutErrorCode = TEXT("trace_launch_cvar_invalid");
				OutErrorMessage = TEXT("The Launch Plan contains a CVar outside the profile allowlist.");
				return false;
			}
			if (Pair.Value->Type == EJson::Boolean)
			{
				Value = Pair.Value->AsBool() ? TEXT("1") : TEXT("0");
			}
			else if (Pair.Value->Type == EJson::Number)
			{
				Value = LexToString(Pair.Value->AsNumber());
			}
			else if (Pair.Value->Type == EJson::String)
			{
				Value = Pair.Value->AsString();
			}
			if (!IsSafeCvarValue(Value))
			{
				OutErrorCode = TEXT("trace_launch_cvar_invalid");
				OutErrorMessage = TEXT("A Launch Plan CVar value contains unsafe characters.");
				return false;
			}
			Plan.Cvars.Add(Pair.Key, Value);
		}
	}
	if (!HashFile(Plan.ProjectPath, Plan.ProjectSha256, OutErrorMessage)
		|| !HashFile(Plan.ExecutablePath, Plan.ExecutableSha256, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_hash_failed");
		return false;
	}
	FString Digest;
	if (!HashText(CanonicalPlan(Plan), Digest))
	{
		OutErrorCode = TEXT("trace_launch_hash_failed");
		OutErrorMessage = TEXT("The Launch Plan digest could not be calculated.");
		return false;
	}
	Plan.Digest = TEXT("sha256:") + Digest;
	if (!SavePlan(Plan, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_plan_store_failed");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue.trace-launch-plan.v1"));
	OutData->SetStringField(TEXT("planDigest"), Plan.Digest);
	OutData->SetStringField(TEXT("approvePlanDigest"), Plan.Digest);
	OutData->SetBoolField(TEXT("executionReady"), true);
	OutData->SetStringField(TEXT("launchProfileId"), Plan.ProfileId);
	OutData->SetStringField(TEXT("project"), Plan.ProjectPath);
	OutData->SetStringField(TEXT("projectSha256"), Plan.ProjectSha256);
	OutData->SetStringField(TEXT("map"), Plan.Map);
	OutData->SetStringField(TEXT("preset"), Plan.Preset);
	OutData->SetStringField(TEXT("executable"), Plan.ExecutablePath);
	OutData->SetStringField(TEXT("executableSha256"), Plan.ExecutableSha256);
	OutData->SetStringField(TEXT("configuration"), Plan.Configuration);
	OutData->SetStringField(TEXT("postStop"), Plan.PostStop);
	OutData->SetArrayField(TEXT("channels"), StringsToJson(Plan.Channels));
	OutData->SetArrayField(TEXT("fixedArguments"), StringsToJson(Plan.FixedArguments));
	OutData->SetNumberField(TEXT("maxDurationSeconds"), Plan.MaxDurationSeconds);
	OutData->SetNumberField(
		TEXT("maxFileSizeMiB"), static_cast<double>(Plan.MaxFileSizeMiB));
	OutData->SetNumberField(
		TEXT("startupTimeoutSeconds"), Plan.StartupTimeoutSeconds);
	OutData->SetNumberField(
		TEXT("shutdownTimeoutSeconds"), Plan.ShutdownTimeoutSeconds);
	OutData->SetBoolField(
		TEXT("allowForcedTermination"), Plan.bAllowForcedTermination);
	OutData->SetObjectField(
		TEXT("effectiveApprovedConfig"), EffectiveConfig(Plan));
	return true;
}

bool FTraceWorkerLaunch::SaveJob(
	const FLaunchJob& Job,
	FString& OutError) const
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema"), TEXT("ue.trace-launch-job.v1"));
	Object->SetStringField(TEXT("jobId"), Job.JobId);
	Object->SetStringField(TEXT("planDigest"), Job.PlanDigest);
	Object->SetStringField(TEXT("requestId"), Job.RequestId);
	Object->SetStringField(TEXT("projectPath"), Job.ProjectPath);
	Object->SetStringField(TEXT("map"), Job.Map);
	Object->SetStringField(TEXT("jobDirectory"), Job.JobDirectory);
	Object->SetStringField(TEXT("descriptorPath"), Job.DescriptorPath);
	Object->SetStringField(TEXT("receiptPath"), Job.ReceiptPath);
	Object->SetStringField(TEXT("stopPath"), Job.StopPath);
	Object->SetStringField(TEXT("tracePath"), Job.TracePath);
	Object->SetStringField(TEXT("logPath"), Job.LogPath);
	Object->SetStringField(TEXT("stopNonce"), Job.StopNonce);
	Object->SetStringField(TEXT("launchedAtUtc"), Job.LaunchedAtUtc);
	Object->SetStringField(TEXT("stopRequestedAtUtc"), Job.StopRequestedAtUtc);
	Object->SetStringField(TEXT("artifactTraceId"), Job.ArtifactTraceId);
	Object->SetStringField(TEXT("postStop"), Job.PostStop);
	Object->SetStringField(
		TEXT("postStopAnalysisId"), Job.PostStopAnalysisId);
	Object->SetStringField(
		TEXT("postStopAnalysisStatus"), Job.PostStopAnalysisStatus);
	Object->SetStringField(TEXT("executablePath"), Job.ExecutablePath);
	Object->SetStringField(TEXT("executableSha256"), Job.ExecutableSha256);
	Object->SetStringField(TEXT("launchNonce"), Job.LaunchNonce);
	Object->SetStringField(
		TEXT("processCreationIdentity"), Job.ProcessCreationIdentity);
	Object->SetStringField(
		TEXT("processIdentityStatus"), Job.ProcessIdentityStatus);
	Object->SetStringField(TEXT("errorCode"), Job.ErrorCode);
	Object->SetStringField(TEXT("errorMessage"), Job.ErrorMessage);
	Object->SetStringField(TEXT("lastHeartbeatAtUtc"), Job.LastHeartbeatAtUtc);
	Object->SetStringField(TEXT("phase"), Job.Phase);
	Object->SetArrayField(TEXT("phaseHistory"), StringsToJson(Job.PhaseHistory));
	Object->SetStringField(TEXT("status"), Job.Status);
	Object->SetNumberField(TEXT("processId"), Job.ProcessId);
	Object->SetNumberField(
		TEXT("stopRequestedUnixSeconds"), Job.StopRequestedUnixSeconds);
	Object->SetNumberField(
		TEXT("startupTimeoutSeconds"), Job.StartupTimeoutSeconds);
	Object->SetNumberField(
		TEXT("shutdownTimeoutSeconds"), Job.ShutdownTimeoutSeconds);
	Object->SetBoolField(
		TEXT("allowForcedTermination"), Job.bAllowForcedTermination);
	Object->SetBoolField(
		TEXT("forcedTermination"), Job.bForcedTermination);
	Object->SetBoolField(TEXT("partial"), Job.bPartial);
	return PublishJson(JobPath(Job.JobId), Object, OutError);
}

void FTraceWorkerLaunch::TransitionPhase(
	FLaunchJob& Job,
	const FString& Phase)
{
	const int32 NewRank = LaunchPhaseRank(Phase);
	if (NewRank == INDEX_NONE)
	{
		return;
	}
	const int32 CurrentRank = Job.PhaseHistory.IsEmpty()
		? INDEX_NONE
		: LaunchPhaseRank(Job.PhaseHistory.Last());
	if (CurrentRank != INDEX_NONE && NewRank < CurrentRank)
	{
		// A stale runtime receipt must not move a durable launch job backwards.
		return;
	}
	Job.Phase = Phase;
	if (Job.PhaseHistory.IsEmpty()
		|| Job.PhaseHistory.Last() != Phase)
	{
		Job.PhaseHistory.Add(Phase);
	}
}

bool FTraceWorkerLaunch::LoadJob(
	const FString& JobId,
	FLaunchJob& OutJob,
	FString& OutError) const
{
	if (!IsSafeId(JobId, TEXT("trace-launch-local-")))
	{
		OutError = TEXT("The Development Trace ID is malformed.");
		return false;
	}
	TSharedPtr<FJsonObject> Object;
	if (!ReadBoundedJson(JobPath(JobId), MaximumRecordBytes, Object)
		|| JsonString(Object, TEXT("schema")) != TEXT("ue.trace-launch-job.v1"))
	{
		OutError = TEXT("The Development Trace ID is not registered in this Worker store.");
		return false;
	}
	OutJob.JobId = JsonString(Object, TEXT("jobId"));
	OutJob.PlanDigest = JsonString(Object, TEXT("planDigest"));
	OutJob.RequestId = JsonString(Object, TEXT("requestId"));
	OutJob.ProjectPath = JsonString(Object, TEXT("projectPath"));
	OutJob.Map = JsonString(Object, TEXT("map"));
	OutJob.JobDirectory = JsonString(Object, TEXT("jobDirectory"));
	OutJob.DescriptorPath = JsonString(Object, TEXT("descriptorPath"));
	OutJob.ReceiptPath = JsonString(Object, TEXT("receiptPath"));
	OutJob.StopPath = JsonString(Object, TEXT("stopPath"));
	OutJob.TracePath = JsonString(Object, TEXT("tracePath"));
	OutJob.LogPath = JsonString(Object, TEXT("logPath"));
	OutJob.StopNonce = JsonString(Object, TEXT("stopNonce"));
	OutJob.LaunchedAtUtc = JsonString(Object, TEXT("launchedAtUtc"));
	OutJob.StopRequestedAtUtc = JsonString(Object, TEXT("stopRequestedAtUtc"));
	OutJob.ArtifactTraceId = JsonString(Object, TEXT("artifactTraceId"));
	OutJob.PostStop = JsonString(
		Object, TEXT("postStop"), TEXT("artifactOnly"));
	OutJob.PostStopAnalysisId = JsonString(
		Object, TEXT("postStopAnalysisId"));
	OutJob.PostStopAnalysisStatus = JsonString(
		Object, TEXT("postStopAnalysisStatus"), TEXT("pending"));
	if (OutJob.PostStop == TEXT("artifactOnly"))
	{
		OutJob.PostStopAnalysisStatus = TEXT("notRequested");
	}
	if (OutJob.PostStopAnalysisStatus == TEXT("running"))
	{
		// Analysis records are published atomically. A persisted running state
		// means the previous Worker exited before publishing a result, so retry
		// the whole bounded analysis on the next status request.
		OutJob.PostStopAnalysisStatus = TEXT("pending");
	}
	OutJob.ExecutablePath = JsonString(Object, TEXT("executablePath"));
	OutJob.ExecutableSha256 = JsonString(Object, TEXT("executableSha256"));
	OutJob.LaunchNonce = JsonString(Object, TEXT("launchNonce"));
	OutJob.ProcessCreationIdentity = JsonString(
		Object, TEXT("processCreationIdentity"));
	OutJob.ProcessIdentityStatus = JsonString(
		Object, TEXT("processIdentityStatus"), TEXT("pending"));
	OutJob.ErrorCode = JsonString(Object, TEXT("errorCode"));
	OutJob.ErrorMessage = JsonString(Object, TEXT("errorMessage"));
	OutJob.LastHeartbeatAtUtc = JsonString(
		Object, TEXT("lastHeartbeatAtUtc"));
	OutJob.PhaseHistory.Reset();
	const TArray<TSharedPtr<FJsonValue>>* PhaseHistory = nullptr;
	int32 LastPhaseRank = INDEX_NONE;
	if (Object->TryGetArrayField(TEXT("phaseHistory"), PhaseHistory)
		&& PhaseHistory)
	{
		for (const TSharedPtr<FJsonValue>& Value : *PhaseHistory)
		{
			FString Phase;
			if (!Value.IsValid() || !Value->TryGetString(Phase))
			{
				continue;
			}
			const int32 PhaseRank = LaunchPhaseRank(Phase);
			if (PhaseRank != INDEX_NONE
				&& PhaseRank >= LastPhaseRank
				&& (OutJob.PhaseHistory.IsEmpty()
					|| OutJob.PhaseHistory.Last() != Phase))
			{
				OutJob.PhaseHistory.Add(Phase);
				LastPhaseRank = PhaseRank;
			}
		}
	}
	if (OutJob.PhaseHistory.IsEmpty())
	{
		TransitionPhase(OutJob, TEXT("launching"));
	}
	else
	{
		TransitionPhase(OutJob, OutJob.PhaseHistory.Last());
	}
	const FString StoredPhase = JsonString(Object, TEXT("phase"), OutJob.Phase);
	if (LaunchPhaseRank(StoredPhase) != INDEX_NONE)
	{
		TransitionPhase(OutJob, StoredPhase);
	}
	OutJob.Status = JsonString(Object, TEXT("status"));
	OutJob.ProcessId = static_cast<uint32>(
		JsonNumber(Object, TEXT("processId"), 0.0));
	OutJob.StopRequestedUnixSeconds = JsonNumber(
		Object, TEXT("stopRequestedUnixSeconds"), 0.0);
	OutJob.StartupTimeoutSeconds = JsonNumber(
		Object, TEXT("startupTimeoutSeconds"), 180.0);
	OutJob.ShutdownTimeoutSeconds = JsonNumber(
		Object, TEXT("shutdownTimeoutSeconds"), 60.0);
	Object->TryGetBoolField(
		TEXT("allowForcedTermination"), OutJob.bAllowForcedTermination);
	Object->TryGetBoolField(
		TEXT("forcedTermination"), OutJob.bForcedTermination);
	Object->TryGetBoolField(TEXT("partial"), OutJob.bPartial);
	FLaunchPlan Plan;
	FString PlanError;
	if (!LoadPlan(OutJob.PlanDigest, Plan, PlanError))
	{
		OutError = TEXT("The Development Trace job no longer matches a valid approved Launch Plan.");
		return false;
	}
	const FString CanonicalProject = CanonicalFilePath(Plan.ProjectPath);
	const FString ExpectedDirectory = CanonicalFilePath(FPaths::Combine(
		FPaths::GetPath(CanonicalProject),
		TEXT("Saved/UE_AI_integration/TraceLaunch"),
		JobId));
	const FString ExpectedDescriptor = FPaths::Combine(
		ExpectedDirectory, TEXT("job.json"));
	const FString ExpectedReceipt = FPaths::Combine(
		ExpectedDirectory, TEXT("receipt.json"));
	const FString ExpectedStop = FPaths::Combine(
		ExpectedDirectory, TEXT("stop.json"));
	const FString ExpectedTrace = FPaths::Combine(
		ExpectedDirectory, TEXT("trace.utrace"));
	const FString ExpectedLog = FPaths::Combine(
		ExpectedDirectory, TEXT("development.log"));
	const bool bRecordMatchesPlan = OutJob.JobId == JobId
		&& FPaths::IsSamePath(OutJob.ProjectPath, CanonicalProject)
		&& OutJob.Map == Plan.Map
		&& FPaths::IsSamePath(OutJob.ExecutablePath, Plan.ExecutablePath)
		&& OutJob.ExecutableSha256 == Plan.ExecutableSha256
		&& OutJob.PostStop == Plan.PostStop
		&& FPaths::IsSamePath(OutJob.JobDirectory, ExpectedDirectory)
		&& FPaths::IsSamePath(OutJob.DescriptorPath, ExpectedDescriptor)
		&& FPaths::IsSamePath(OutJob.ReceiptPath, ExpectedReceipt)
		&& FPaths::IsSamePath(OutJob.StopPath, ExpectedStop)
		&& FPaths::IsSamePath(OutJob.TracePath, ExpectedTrace)
		&& FPaths::IsSamePath(OutJob.LogPath, ExpectedLog)
		&& !OutJob.LaunchNonce.IsEmpty();
	if (!bRecordMatchesPlan)
	{
		OutError = TEXT("The Development Trace job record conflicts with its approved Launch Plan or derived artifact paths.");
		return false;
	}
	// Use only plan-derived canonical paths after validation.
	OutJob.ProjectPath = CanonicalProject;
	OutJob.JobDirectory = ExpectedDirectory;
	OutJob.DescriptorPath = ExpectedDescriptor;
	OutJob.ReceiptPath = ExpectedReceipt;
	OutJob.StopPath = ExpectedStop;
	OutJob.TracePath = ExpectedTrace;
	OutJob.LogPath = ExpectedLog;
	return true;
}

bool FTraceWorkerLaunch::Start(
	const TSharedPtr<FJsonObject>& Params,
	const FString& RequestId,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	if (RequestId.IsEmpty())
	{
		OutErrorCode = TEXT("trace_launch_request_id_required");
		OutErrorMessage = TEXT(
			"Development Trace start requires a durable requestId.");
		return false;
	}
	if (Params->HasField(TEXT("requestId"))
		&& JsonString(Params, TEXT("requestId")) != RequestId)
	{
		OutErrorCode = TEXT("trace_launch_request_id_mismatch");
		OutErrorMessage = TEXT(
			"The capability requestId must match the Worker request envelope requestId.");
		return false;
	}
	bool bConfirmLaunch = false;
	Params->TryGetBoolField(TEXT("confirmLaunch"), bConfirmLaunch);
	const FString Digest = JsonString(Params, TEXT("approvePlanDigest"));
	if (!bConfirmLaunch || Digest.IsEmpty())
	{
		OutErrorCode = TEXT("trace_launch_approval_required");
		OutErrorMessage = TEXT(
			"Development Trace start requires approvePlanDigest and confirmLaunch=true.");
		return false;
	}
	FLaunchPlan Plan;
	if (!LoadPlan(Digest, Plan, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_plan_mismatch");
		return false;
	}
	const TSharedPtr<FJsonObject>* Target = nullptr;
	if (!Params->TryGetObjectField(TEXT("target"), Target)
		|| !Target || !Target->IsValid()
		|| JsonString(*Target, TEXT("kind")) != TEXT("development")
		|| JsonString(*Target, TEXT("launchProfileId")) != Plan.ProfileId
		|| JsonString(*Target, TEXT("map")) != Plan.Map)
	{
		OutErrorCode = TEXT("trace_launch_plan_mismatch");
		OutErrorMessage = TEXT("The Development target does not match the approved Launch Plan.");
		return false;
	}
	if (!ValidateStartMatchesPlan(Params, Plan, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_plan_mismatch");
		return false;
	}
	FString ProjectSha;
	FString ExecutableSha;
	if (!HashFile(Plan.ProjectPath, ProjectSha, OutErrorMessage)
		|| !HashFile(Plan.ExecutablePath, ExecutableSha, OutErrorMessage)
		|| ProjectSha != Plan.ProjectSha256
		|| ExecutableSha != Plan.ExecutableSha256)
	{
		OutErrorCode = TEXT("trace_launch_plan_conflict");
		OutErrorMessage = TEXT("The project or executable changed after Launch Plan approval.");
		return false;
	}
	FLaunchJob Job;
	Job.JobId = TEXT("trace-launch-local-")
		+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Job.PlanDigest = Plan.Digest;
	Job.RequestId = RequestId;
	Job.ProjectPath = Plan.ProjectPath;
	Job.Map = Plan.Map;
	Job.PostStop = Plan.PostStop;
	Job.PostStopAnalysisStatus = Plan.PostStop == TEXT("artifactOnly")
		? TEXT("notRequested")
		: TEXT("pending");
	Job.JobDirectory = FPaths::Combine(
		FPaths::GetPath(Plan.ProjectPath),
		TEXT("Saved/UE_AI_integration/TraceLaunch"),
		Job.JobId);
	Job.DescriptorPath = FPaths::Combine(Job.JobDirectory, TEXT("job.json"));
	Job.ReceiptPath = FPaths::Combine(Job.JobDirectory, TEXT("receipt.json"));
	Job.StopPath = FPaths::Combine(Job.JobDirectory, TEXT("stop.json"));
	Job.TracePath = FPaths::Combine(Job.JobDirectory, TEXT("trace.utrace"));
	Job.LogPath = FPaths::Combine(Job.JobDirectory, TEXT("development.log"));
	Job.StopNonce = FGuid::NewGuid().ToString(EGuidFormats::Digits)
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Job.LaunchNonce = FGuid::NewGuid().ToString(EGuidFormats::Digits)
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Job.ExecutablePath = Plan.ExecutablePath;
	Job.ExecutableSha256 = Plan.ExecutableSha256;
	Job.LaunchedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job.StartupTimeoutSeconds = Plan.StartupTimeoutSeconds;
	Job.ShutdownTimeoutSeconds = Plan.ShutdownTimeoutSeconds;
	Job.bAllowForcedTermination = Plan.bAllowForcedTermination;
	bool bRequestCreated = false;
	FString ExistingPlanDigest;
	FString ExistingJobId;
	if (!ClaimRequest(
			RequestId,
			Plan.Digest,
			Job.JobId,
			bRequestCreated,
			ExistingPlanDigest,
			ExistingJobId,
			OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_request_registry_unavailable");
		return false;
	}
	if (!bRequestCreated)
	{
		if (ExistingPlanDigest != Plan.Digest)
		{
			OutErrorCode = TEXT("trace_launch_request_conflict");
			OutErrorMessage = TEXT(
				"requestId was already bound to a different canonical Launch Plan digest.");
			return false;
		}
		FLaunchJob ExistingJob;
		if (!LoadJob(ExistingJobId, ExistingJob, OutErrorMessage))
		{
			OutErrorCode = TEXT("trace_launch_idempotency_incomplete");
			OutErrorMessage = TEXT(
				"requestId is durably reserved, but its launch job record is incomplete; a duplicate process will not be started.");
			return false;
		}
		TSharedPtr<FJsonObject> Receipt;
		if (!RefreshJob(
				ExistingJob,
				Receipt,
				OutErrorCode,
				OutErrorMessage))
		{
			return false;
		}
		OutData = JobToResult(ExistingJob, Receipt);
		OutData->SetBoolField(TEXT("accepted"), true);
		OutData->SetBoolField(TEXT("idempotentReplay"), true);
		OutData->SetObjectField(
			TEXT("effectiveApprovedConfig"), EffectiveConfig(Plan));
		return true;
	}
	if (!IFileManager::Get().MakeDirectory(*Job.JobDirectory, true))
	{
		OutErrorCode = TEXT("trace_launch_storage_unavailable");
		OutErrorMessage = TEXT("The project TraceLaunch directory could not be created.");
		return false;
	}
	TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
	Descriptor->SetStringField(TEXT("schema"), TEXT("ue.trace-runtime-job.v1"));
	Descriptor->SetStringField(TEXT("jobId"), Job.JobId);
	Descriptor->SetStringField(TEXT("requestId"), RequestId);
	Descriptor->SetStringField(TEXT("launchPlanDigest"), Plan.Digest);
	Descriptor->SetStringField(TEXT("launchNonce"), Job.LaunchNonce);
	Descriptor->SetArrayField(TEXT("channels"), StringsToJson(Plan.Channels));
	Descriptor->SetNumberField(TEXT("maxDurationSeconds"), Plan.MaxDurationSeconds);
	Descriptor->SetNumberField(
		TEXT("startupTimeoutSeconds"), Plan.StartupTimeoutSeconds);
	Descriptor->SetNumberField(
		TEXT("maxFileSizeMiB"), static_cast<double>(Plan.MaxFileSizeMiB));
	Descriptor->SetStringField(TEXT("stopNonce"), Job.StopNonce);
	Descriptor->SetBoolField(TEXT("exitOnStop"), true);
	if (!PublishJson(Job.DescriptorPath, Descriptor, OutErrorMessage)
		|| !SaveJob(Job, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_storage_unavailable");
		return false;
	}

	TArray<FString> Arguments;
	Arguments.Add(QuoteArgument(Plan.ProjectPath));
	Arguments.Add(QuoteArgument(Plan.Map));
	Arguments.Append(Plan.FixedArguments);
	Arguments.Add(TEXT("-UEAITraceJob=") + QuoteArgument(Job.DescriptorPath));
	Arguments.Add(TEXT("-abslog=") + QuoteArgument(Job.LogPath));
	Arguments.Add(TEXT("-stdout"));
	Arguments.Add(TEXT("-FullStdOutLogOutput"));
	if (!Plan.Cvars.IsEmpty())
	{
		TArray<FString> Names;
		Plan.Cvars.GetKeys(Names);
		Names.Sort();
		TArray<FString> Commands;
		for (const FString& Name : Names)
		{
			Commands.Add(Name + TEXT(" ") + Plan.Cvars[Name]);
		}
		Arguments.Add(TEXT("-ExecCmds=")
			+ QuoteArgument(FString::Join(Commands, TEXT(","))));
	}
	const FString ArgumentString = FString::Join(Arguments, TEXT(" "));
	uint32 ProcessId = 0;
	FProcHandle Process = FPlatformProcess::CreateProc(
		*Plan.ExecutablePath,
		*ArgumentString,
		true,
		true,
		true,
		&ProcessId,
		0,
		*FPaths::GetPath(Plan.ProjectPath),
		nullptr,
		nullptr);
	if (!Process.IsValid())
	{
		Job.Status = TEXT("failed");
		TransitionPhase(Job, TEXT("completed"));
		SaveJob(Job, OutErrorMessage);
		OutErrorCode = TEXT("trace_launch_failed");
		OutErrorMessage = TEXT("The approved Development executable could not be launched.");
		return false;
	}
	Job.ProcessId = ProcessId;
	Job.ProcessCreationIdentity = ReadProcessCreationIdentity(Process);
	const FString LaunchedPath = CanonicalFilePath(
		FPlatformProcess::GetApplicationName(ProcessId));
	const bool bLaunchedPathMatches = !LaunchedPath.IsEmpty()
		&& FPaths::IsSamePath(
			LaunchedPath, CanonicalFilePath(Plan.ExecutablePath));
#if PLATFORM_WINDOWS
	const bool bCreationIdentityAvailable =
		!Job.ProcessCreationIdentity.IsEmpty();
#else
	const bool bCreationIdentityAvailable = true;
#endif
	if (!bLaunchedPathMatches || !bCreationIdentityAvailable)
	{
		Job.Status = TEXT("failed");
		TransitionPhase(Job, TEXT("completed"));
		Job.bPartial = true;
		Job.ErrorCode = TEXT("trace_launch_process_identity_failed");
		Job.ErrorMessage = TEXT(
			"The launched process could not be bound to a durable executable and creation identity.");
		StopUntrackedProcess(Job, Process);
		SaveJob(Job, OutErrorMessage);
		OutErrorCode = Job.ErrorCode;
		OutErrorMessage = Job.ErrorMessage;
		return false;
	}
#if PLATFORM_WINDOWS
	Job.ProcessIdentityStatus = TEXT("verified");
#else
	Job.ProcessIdentityStatus = TEXT("residentOnly");
#endif
	if (!SaveJob(Job, OutErrorMessage))
	{
		StopUntrackedProcess(Job, Process);
		OutErrorCode = TEXT("trace_launch_storage_unavailable");
		OutErrorMessage = TEXT(
			"The launched process was stopped because its durable identity record could not be published.");
		return false;
	}
	RetainOwnedProcessHandle(Job.JobId, Process);
	OutData = JobToResult(Job, nullptr);
	OutData->SetBoolField(TEXT("accepted"), true);
	OutData->SetBoolField(TEXT("idempotentReplay"), false);
	OutData->SetObjectField(
		TEXT("effectiveApprovedConfig"), EffectiveConfig(Plan));
	return true;
}

bool FTraceWorkerLaunch::RefreshJob(
	FLaunchJob& Job,
	TSharedPtr<FJsonObject>& OutReceipt,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutReceipt.Reset();
	ReadBoundedJson(Job.ReceiptPath, MaximumRecordBytes, OutReceipt);
	const auto MatchesRuntimeIdentity = [&Job](
		const TSharedPtr<FJsonObject>& Receipt)
	{
		return Receipt.IsValid()
			&& JsonString(Receipt, TEXT("schema"))
				== TEXT("ue.trace-runtime-receipt.v1")
			&& JsonString(Receipt, TEXT("traceJobId")) == Job.JobId
			&& JsonString(Receipt, TEXT("launchPlanDigest")) == Job.PlanDigest
			&& JsonString(Receipt, TEXT("launchNonce")) == Job.LaunchNonce;
	};
	const bool bReceiptMatches = MatchesRuntimeIdentity(OutReceipt);
	const bool bWorkerWasTerminal = Job.Status == TEXT("succeeded")
		|| Job.Status == TEXT("failed");
	const FString RuntimeStatus = bReceiptMatches
		? JsonString(OutReceipt, TEXT("status"), Job.Status)
		: FString();
	if (OutReceipt.IsValid() && !bReceiptMatches)
	{
		Job.ErrorCode = TEXT("trace_runtime_receipt_mismatch");
		Job.ErrorMessage = TEXT(
			"The runtime receipt does not match this launch identity.");
		Job.bPartial = true;
		OutReceipt.Reset();
	}
	else if (bReceiptMatches && !bWorkerWasTerminal)
	{
		const FString RuntimePhase = JsonString(
			OutReceipt, TEXT("phase"), Job.Phase);
		if (RuntimePhase == TEXT("loading"))
		{
			TransitionPhase(Job, TEXT("loading"));
		}
		else if (RuntimePhase == TEXT("recording"))
		{
			// The runtime receipt is atomically replaced, so a Worker status
			// poll can observe recording without ever seeing loading.
			TransitionPhase(Job, TEXT("loading"));
			TransitionPhase(Job, TEXT("recording"));
		}
		else if (RuntimePhase == TEXT("finalizing")
			|| RuntimePhase == TEXT("completed"))
		{
			// The managed runtime only enters finalizing after its recording
			// gate. Its receipt is atomically replaced, so persist both skipped
			// prerequisites before advancing even when polling first observes
			// finalizing or the terminal receipt.
			TransitionPhase(Job, TEXT("loading"));
			TransitionPhase(Job, TEXT("recording"));
			TransitionPhase(Job, TEXT("finalizing"));
		}
		Job.Status = RuntimeStatus;
		Job.ErrorCode = JsonString(OutReceipt, TEXT("errorCode"));
		Job.ErrorMessage = JsonString(OutReceipt, TEXT("message"));
		OutReceipt->TryGetBoolField(TEXT("partial"), Job.bPartial);
	}

	TSharedPtr<FJsonObject> Heartbeat;
	ReadBoundedJson(
		FPaths::Combine(Job.JobDirectory, TEXT("heartbeat.json")),
		MaximumRecordBytes,
		Heartbeat);
	const bool bHeartbeatMatches = MatchesRuntimeIdentity(Heartbeat);
	if (bHeartbeatMatches)
	{
		Job.LastHeartbeatAtUtc = JsonString(
			Heartbeat, TEXT("updatedAtUtc"));
	}

	FProcHandle Process;
	bool bMustCloseProcess = false;
	FString ProcessReason;
	const bool bProcessVerified = OpenVerifiedProcess(
		Job, Process, bMustCloseProcess, ProcessReason);
	bool bRunning = bProcessVerified && FPlatformProcess::IsProcRunning(Process);
	bool bConfirmedExited = bProcessVerified && !bRunning;
	bool bIdentityUnverified = false;
	const bool bRuntimeTerminal = RuntimeStatus == TEXT("succeeded")
		|| RuntimeStatus == TEXT("failed");
	if (!bProcessVerified && Job.ProcessIdentityStatus == TEXT("mismatch"))
	{
		if (bReceiptMatches && bRuntimeTerminal)
		{
			// A matching terminal receipt is bound to this job ID, approved
			// digest, and launch nonce. If the durable PID now identifies a
			// different process, the owned Development process has exited and
			// Windows has reused its PID. Treat that as exit evidence without
			// waiting on or terminating the unrelated process.
			Job.ProcessIdentityStatus = TEXT("verifiedExitedPidReused");
			bConfirmedExited = true;
		}
		else
		{
			TransitionPhase(Job, TEXT("completed"));
			Job.Status = TEXT("failed");
			Job.bPartial = true;
			Job.ErrorCode = TEXT("trace_launch_process_identity_mismatch");
			Job.ErrorMessage = TEXT(
				"The persisted PID now identifies a different process; it was not opened or terminated.");
		}
	}
	else if (!bProcessVerified)
	{
		const bool bPidStillPresent = Job.ProcessId != 0
			&& FPlatformProcess::IsApplicationRunning(Job.ProcessId);
		if (bPidStillPresent)
		{
			bIdentityUnverified = true;
			Job.Status = TEXT("running");
			Job.bPartial = true;
			Job.ErrorCode = TEXT("trace_launch_process_verification_unavailable");
			Job.ErrorMessage = TEXT(
				"The Development process still exists, but its durable identity could not be verified; ownership is retained and it will not be terminated or finalized.");
		}
		else
		{
			bConfirmedExited = Job.ProcessId != 0;
		}
	}

	if (bRuntimeTerminal && bRunning)
	{
		// The Runtime publishes its terminal receipt immediately before
		// RequestExit. Keep the job non-terminal until the owned process handle
		// is signaled so artifact import never races buffered trace writes.
		Job.Status = TEXT("running");
		TransitionPhase(Job, TEXT("finalizing"));
		if (Job.StopRequestedUnixSeconds <= 0.0)
		{
			Job.StopRequestedAtUtc = FDateTime::UtcNow().ToIso8601();
			Job.StopRequestedUnixSeconds = static_cast<double>(
				FDateTime::UtcNow().ToUnixTimestamp());
		}
	}
	else if (bConfirmedExited && Job.Status == TEXT("running"))
	{
		TransitionPhase(Job, TEXT("completed"));
		Job.Status = TEXT("failed");
		Job.bPartial = true;
		Job.ErrorCode = TEXT("trace_development_process_crashed");
		Job.ErrorMessage = TEXT(
			"The AI-owned Development process exited without a terminal runtime receipt.");
	}

	auto RequestManagedStop = [&Job, this]()
	{
		if (Job.StopRequestedUnixSeconds > 0.0)
		{
			return;
		}
		TSharedPtr<FJsonObject> StopRequest = MakeShared<FJsonObject>();
		StopRequest->SetStringField(
			TEXT("schema"), TEXT("ue.trace-runtime-stop.v1"));
		StopRequest->SetStringField(TEXT("jobId"), Job.JobId);
		StopRequest->SetStringField(TEXT("nonce"), Job.StopNonce);
		FString Ignored;
		if (PublishJson(Job.StopPath, StopRequest, Ignored))
		{
			Job.StopRequestedAtUtc = FDateTime::UtcNow().ToIso8601();
			Job.StopRequestedUnixSeconds = static_cast<double>(
				FDateTime::UtcNow().ToUnixTimestamp());
			TransitionPhase(Job, TEXT("finalizing"));
		}
	};

	if (bRunning && Job.Status == TEXT("running"))
	{
		const double SinceLaunch = SecondsSinceUtc(Job.LaunchedAtUtc);
		const bool bStartupPhase = Job.Phase == TEXT("launching")
			|| Job.Phase == TEXT("loading");
		const bool bRecordingPhase = Job.Phase == TEXT("recording");
		if (bStartupPhase && SinceLaunch > Job.StartupTimeoutSeconds)
		{
			Job.ErrorCode = TEXT("trace_runtime_startup_timeout");
			Job.ErrorMessage = TEXT(
				"The Development runtime did not enter recording before startupTimeoutSeconds.");
			Job.bPartial = true;
			RequestManagedStop();
		}
		else if (bRecordingPhase)
		{
			const FString RecordingReferenceUtc = bHeartbeatMatches
				? Job.LastHeartbeatAtUtc
				: JsonString(OutReceipt, TEXT("updatedAtUtc"));
			const double SinceRecordingSignal =
				SecondsSinceUtc(RecordingReferenceUtc);
			if (SinceRecordingSignal > 10.0)
			{
				Job.ErrorCode = TEXT("trace_runtime_heartbeat_stale");
				Job.ErrorMessage = TEXT(
					"The Development runtime recording heartbeat has been stale for more than 10 seconds.");
				Job.bPartial = true;
				RequestManagedStop();
			}
		}
	}

	if (bRunning
		&& Job.StopRequestedUnixSeconds > 0.0
		&& FDateTime::UtcNow().ToUnixTimestamp()
			- Job.StopRequestedUnixSeconds > Job.ShutdownTimeoutSeconds
		&& Job.bAllowForcedTermination
		&& bProcessVerified)
	{
		// Process is terminated only through the handle that passed path, hash,
		// creation-time, and launch-nonce-backed receipt validation above.
		FPlatformProcess::TerminateProc(Process, true);
		const double TerminationDeadline = FPlatformTime::Seconds() + 5.0;
		while (FPlatformProcess::IsProcRunning(Process)
			&& FPlatformTime::Seconds() < TerminationDeadline)
		{
			FPlatformProcess::Sleep(0.05f);
		}
		bRunning = FPlatformProcess::IsProcRunning(Process);
		Job.bForcedTermination = true;
		Job.bPartial = true;
		if (!bRunning)
		{
			bConfirmedExited = true;
			TransitionPhase(Job, TEXT("completed"));
			Job.Status = TEXT("failed");
			if (Job.ErrorCode.IsEmpty())
			{
				Job.ErrorCode = TEXT("trace_shutdown_timeout");
				Job.ErrorMessage = TEXT(
					"The AI-owned Development process exceeded shutdownTimeoutSeconds and was terminated.");
			}
		}
		else
		{
			TransitionPhase(Job, TEXT("finalizing"));
			Job.Status = TEXT("running");
			Job.ErrorCode = TEXT("trace_forced_termination_pending");
			Job.ErrorMessage = TEXT(
				"The owned process did not exit within five seconds after forced termination; ownership is retained.");
		}
	}
	else if (bRunning
		&& Job.StopRequestedUnixSeconds > 0.0
		&& FDateTime::UtcNow().ToUnixTimestamp()
			- Job.StopRequestedUnixSeconds > Job.ShutdownTimeoutSeconds
		&& !Job.bAllowForcedTermination)
	{
		TransitionPhase(Job, TEXT("finalizing"));
		Job.Status = TEXT("running");
		Job.bPartial = true;
		Job.ErrorCode = TEXT("trace_shutdown_timeout_manual_intervention_required");
		Job.ErrorMessage = TEXT(
			"The Development process exceeded shutdownTimeoutSeconds, but its approved profile forbids forced termination; ownership is retained.");
	}
	if (bMustCloseProcess && Process.IsValid())
	{
		FPlatformProcess::CloseProc(Process);
	}
	if (bConfirmedExited)
	{
		ForgetOwnedProcessHandle(Job.JobId);
	}
	if (bConfirmedExited
		&& (Job.Status == TEXT("succeeded") || Job.Status == TEXT("failed"))
		&& Job.ArtifactTraceId.IsEmpty()
		&& IFileManager::Get().FileSize(*Job.TracePath) > 0)
	{
		FTraceRecord Record;
		if (TraceStore.ImportGeneratedTrace(
				Job.TracePath, Record, OutErrorCode, OutErrorMessage))
		{
			Job.ArtifactTraceId = Record.TraceId;
		}
		else
		{
			Job.Status = TEXT("failed");
			Job.bPartial = true;
			Job.ErrorCode = OutErrorCode.IsEmpty()
				? TEXT("trace_artifact_import_failed")
				: OutErrorCode;
			Job.ErrorMessage = OutErrorMessage;
		}
	}
	else if (bConfirmedExited
		&& (Job.Status == TEXT("succeeded") || Job.Status == TEXT("failed"))
		&& Job.ArtifactTraceId.IsEmpty()
		&& IFileManager::Get().FileSize(*Job.TracePath) <= 0)
	{
		Job.Status = TEXT("failed");
		Job.bPartial = true;
		Job.ErrorCode = TEXT("trace_artifact_missing");
		Job.ErrorMessage = TEXT(
			"The Development process exited without a non-empty .utrace artifact.");
	}
	if (bConfirmedExited
		&& !Job.ArtifactTraceId.IsEmpty()
		&& Job.PostStop != TEXT("artifactOnly")
		&& Job.PostStopAnalysisStatus == TEXT("pending"))
	{
		TransitionPhase(Job, TEXT("analyzing"));
		Job.PostStopAnalysisStatus = TEXT("running");
		FString SavePhaseError;
		SaveJob(Job, SavePhaseError);
		FTraceRecord Record;
		if (!TraceStore.Resolve(
				Job.ArtifactTraceId, Record, OutErrorCode, OutErrorMessage))
		{
			Job.Status = TEXT("failed");
			Job.bPartial = true;
			Job.ErrorCode = OutErrorCode;
			Job.ErrorMessage = OutErrorMessage;
			Job.PostStopAnalysisStatus = TEXT("failed");
		}
		else
		{
			UEAI::Trace::FTraceAnalysisSession Session;
			if (!Session.Open(
					Record.TracePath,
					120.0,
					OutErrorCode,
					OutErrorMessage))
			{
				Job.Status = TEXT("failed");
				Job.bPartial = true;
				Job.ErrorCode = OutErrorCode;
				Job.ErrorMessage = OutErrorMessage;
				Job.PostStopAnalysisStatus = TEXT("failed");
			}
			else
			{
				const FString AnalysisId = TraceStore.MakeAnalysisId();
				TSharedPtr<FJsonObject> Analysis = MakeShared<FJsonObject>();
				Analysis->SetStringField(
					TEXT("schema"), TEXT("ue.trace-post-stop-analysis.v1"));
				Analysis->SetStringField(TEXT("analysisId"), AnalysisId);
				Analysis->SetStringField(TEXT("traceId"), Record.TraceId);
				Analysis->SetStringField(TEXT("mode"), Job.PostStop);
				Analysis->SetNumberField(
					TEXT("durationSeconds"), Session.GetDurationSeconds());
				Analysis->SetStringField(
					TEXT("recordedBuildVersion"),
					Session.GetRecordedBuildVersion());
				Analysis->SetStringField(
					TEXT("engineVersionStatus"),
					Session.GetEngineVersionStatus());
				Analysis->SetStringField(
					TEXT("managedEngineMarker"),
					Session.GetManagedEngineMarker());
				const TArray<UEAI::Trace::FTraceProviderStatus> ProviderStatuses =
					Session.GetProviderStatuses();
				TArray<TSharedPtr<FJsonValue>> Providers;
				for (const UEAI::Trace::FTraceProviderStatus& Status
					: ProviderStatuses)
				{
					Providers.Add(MakeShared<FJsonValueObject>(
						ProviderStatusToJson(Status)));
				}
				Analysis->SetArrayField(TEXT("providers"), Providers);
				TSharedPtr<FJsonObject> SemanticQueries =
					MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> QueryDiagnostics;
				const bool bFull = Job.PostStop == TEXT("analyzeFull");
				for (const UEAI::Trace::FTraceProviderStatus& Status
					: ProviderStatuses)
				{
					if (!Status.bRecorded
						|| !Status.Descriptor.bQueryImplemented)
					{
						continue;
					}
					TSharedPtr<FJsonObject> ProviderQueries =
						MakeShared<FJsonObject>();
					for (const FString& Operation : PostStopOperations(
						Status.Descriptor.Id, bFull))
					{
						UEAI::Trace::FTraceQueryRequest Query;
						Query.Provider = Status.Descriptor.Id;
						Query.Operation = Operation;
						Query.Page.Limit = bFull ? 100 : 25;
						UEAI::Trace::FTraceQueryResult QueryResult;
						FString QueryErrorCode;
						FString QueryErrorMessage;
						if (Session.Query(
								Query,
								QueryResult,
								QueryErrorCode,
								QueryErrorMessage))
						{
							ProviderQueries->SetObjectField(
								Operation,
								QueryResultToJson(QueryResult));
						}
						else
						{
							TSharedPtr<FJsonObject> Diagnostic =
								MakeShared<FJsonObject>();
							Diagnostic->SetStringField(
								TEXT("provider"), Status.Descriptor.Id);
							Diagnostic->SetStringField(
								TEXT("operation"), Operation);
							Diagnostic->SetStringField(
								TEXT("code"), QueryErrorCode);
							Diagnostic->SetStringField(
								TEXT("message"), QueryErrorMessage);
							QueryDiagnostics.Add(
								MakeShared<FJsonValueObject>(Diagnostic));
							Job.bPartial = true;
						}
					}
					if (!ProviderQueries->Values.IsEmpty())
					{
						SemanticQueries->SetObjectField(
							Status.Descriptor.Id, ProviderQueries);
					}
				}
				Analysis->SetObjectField(
					TEXT("semanticQueries"), SemanticQueries);
				Analysis->SetArrayField(
					TEXT("queryDiagnostics"), QueryDiagnostics);
				FString PersistError;
				if (!TraceStore.PersistAnalysisJob(
						AnalysisId,
						Record.TraceId,
						TEXT("production.trace.analyze"),
						Analysis,
						PersistError))
				{
					Job.Status = TEXT("failed");
					Job.bPartial = true;
					Job.ErrorCode = TEXT("trace_analysis_record_failed");
					Job.ErrorMessage = PersistError;
					Job.PostStopAnalysisStatus = TEXT("failed");
				}
				else
				{
					Job.PostStopAnalysisId = AnalysisId;
					Job.PostStopAnalysisStatus = TEXT("completed");
				}
			}
		}
		TransitionPhase(Job, TEXT("completed"));
		OutErrorCode.Reset();
		OutErrorMessage.Reset();
	}
	if (bConfirmedExited
		&& (Job.Status == TEXT("succeeded") || Job.Status == TEXT("failed"))
		&& (Job.PostStop == TEXT("artifactOnly")
			|| Job.PostStopAnalysisStatus == TEXT("completed")
			|| Job.PostStopAnalysisStatus == TEXT("failed")
			|| Job.ArtifactTraceId.IsEmpty()))
	{
		TransitionPhase(Job, TEXT("completed"));
	}
	if (bIdentityUnverified)
	{
		// Do not accidentally return a worker transport failure: the status
		// payload carries the durable verification diagnostic.
		OutErrorCode.Reset();
		OutErrorMessage.Reset();
	}
	if (!SaveJob(Job, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_storage_unavailable");
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> FTraceWorkerLaunch::JobToResult(
	const FLaunchJob& Job,
	const TSharedPtr<FJsonObject>& Receipt) const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.trace-launch-status.v1"));
	Data->SetStringField(TEXT("traceId"), Job.JobId);
	Data->SetStringField(TEXT("jobId"), Job.JobId);
	Data->SetStringField(TEXT("requestId"), Job.RequestId);
	Data->SetStringField(TEXT("planDigest"), Job.PlanDigest);
	Data->SetStringField(TEXT("phase"), Job.Phase);
	Data->SetArrayField(TEXT("phaseHistory"), StringsToJson(Job.PhaseHistory));
	Data->SetStringField(TEXT("status"), Job.Status);
	Data->SetBoolField(TEXT("partial"), Job.bPartial);
	Data->SetStringField(TEXT("errorCode"), Job.ErrorCode);
	Data->SetStringField(TEXT("message"), Job.ErrorMessage);
	Data->SetNumberField(TEXT("processId"), Job.ProcessId);
	Data->SetStringField(
		TEXT("processIdentityStatus"), Job.ProcessIdentityStatus);
	Data->SetStringField(
		TEXT("lastHeartbeatAtUtc"), Job.LastHeartbeatAtUtc);
	Data->SetStringField(TEXT("project"), Job.ProjectPath);
	Data->SetStringField(TEXT("map"), Job.Map);
	Data->SetStringField(TEXT("launchedAtUtc"), Job.LaunchedAtUtc);
	Data->SetStringField(TEXT("stopRequestedAtUtc"), Job.StopRequestedAtUtc);
	Data->SetBoolField(TEXT("aiOwnedProcess"), true);
	Data->SetBoolField(TEXT("forcedTermination"), Job.bForcedTermination);
	Data->SetStringField(TEXT("artifactTraceId"), Job.ArtifactTraceId);
	Data->SetStringField(TEXT("postStop"), Job.PostStop);
	Data->SetStringField(
		TEXT("postStopAnalysisId"), Job.PostStopAnalysisId);
	Data->SetStringField(
		TEXT("postStopAnalysisStatus"), Job.PostStopAnalysisStatus);
	TSharedPtr<FJsonObject> TraceArtifact = MakeShared<FJsonObject>();
	TraceArtifact->SetStringField(TEXT("kind"), TEXT("utrace"));
	TraceArtifact->SetStringField(TEXT("path"), Job.TracePath);
	TraceArtifact->SetNumberField(
		TEXT("sizeBytes"), FMath::Max<int64>(0, IFileManager::Get().FileSize(*Job.TracePath)));
	Data->SetObjectField(TEXT("traceArtifact"), TraceArtifact);
	TSharedPtr<FJsonObject> LogArtifact = MakeShared<FJsonObject>();
	LogArtifact->SetStringField(TEXT("kind"), TEXT("log"));
	LogArtifact->SetStringField(TEXT("path"), Job.LogPath);
	LogArtifact->SetNumberField(
		TEXT("sizeBytes"), FMath::Max<int64>(0, IFileManager::Get().FileSize(*Job.LogPath)));
	Data->SetObjectField(TEXT("logArtifact"), LogArtifact);
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TSharedPtr<FJsonObject> TraceSummary = MakeShared<FJsonObject>();
	TraceSummary->SetStringField(TEXT("artifactId"), TEXT("trace"));
	TraceSummary->SetStringField(TEXT("kind"), TEXT("utrace"));
	TraceSummary->SetStringField(TEXT("path"), Job.TracePath);
	TraceSummary->SetNumberField(
		TEXT("sizeBytes"), FMath::Max<int64>(0, IFileManager::Get().FileSize(*Job.TracePath)));
	Artifacts.Add(MakeShared<FJsonValueObject>(TraceSummary));
	TSharedPtr<FJsonObject> LogSummary = MakeShared<FJsonObject>();
	LogSummary->SetStringField(TEXT("artifactId"), TEXT("log"));
	LogSummary->SetStringField(TEXT("kind"), TEXT("log"));
	LogSummary->SetStringField(TEXT("path"), Job.LogPath);
	LogSummary->SetNumberField(
		TEXT("sizeBytes"), FMath::Max<int64>(0, IFileManager::Get().FileSize(*Job.LogPath)));
	Artifacts.Add(MakeShared<FJsonValueObject>(LogSummary));
	TSharedPtr<FJsonObject> ReceiptSummary = MakeShared<FJsonObject>();
	ReceiptSummary->SetStringField(TEXT("artifactId"), TEXT("receipt"));
	ReceiptSummary->SetStringField(TEXT("kind"), TEXT("json"));
	ReceiptSummary->SetStringField(TEXT("path"), Job.ReceiptPath);
	ReceiptSummary->SetNumberField(
		TEXT("sizeBytes"), FMath::Max<int64>(0, IFileManager::Get().FileSize(*Job.ReceiptPath)));
	Artifacts.Add(MakeShared<FJsonValueObject>(ReceiptSummary));
	Data->SetArrayField(TEXT("artifacts"), Artifacts);
	if (Receipt.IsValid())
	{
		Data->SetObjectField(TEXT("runtimeReceipt"), Receipt);
	}
	return Data;
}

bool FTraceWorkerLaunch::ReadJobLog(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	const FString JobId = JsonString(Params, TEXT("traceId"));
	FLaunchJob Job;
	if (!LoadJob(JobId, Job, OutErrorMessage))
	{
		OutErrorCode = TEXT("job_not_found");
		return false;
	}
	const int64 Cursor = static_cast<int64>(
		JsonNumber(Params, TEXT("cursor"), 0.0));
	const int32 MaxChars = FMath::Clamp(
		static_cast<int32>(JsonNumber(Params, TEXT("maxChars"), 16384.0)),
		1,
		65536);
	TUniquePtr<IFileHandle> File(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Job.LogPath));
	if (!File.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("jobId"), Job.JobId);
		OutData->SetNumberField(TEXT("cursor"), 0);
		OutData->SetNumberField(TEXT("nextCursor"), 0);
		OutData->SetNumberField(TEXT("retainedFromCursor"), 0);
		OutData->SetStringField(TEXT("text"), FString());
		OutData->SetBoolField(TEXT("eof"), true);
		OutData->SetBoolField(TEXT("truncated"), false);
		return true;
	}
	const int64 Size = File->Size();
	if (Cursor < 0 || Cursor > Size)
	{
		OutErrorCode = TEXT("job_log_cursor_invalid");
		OutErrorMessage = TEXT("cursor is outside the Development Trace log.");
		return false;
	}
	const int32 Count = static_cast<int32>(
		FMath::Min<int64>(MaxChars, Size - Cursor));
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Count);
	if (!File->Seek(Cursor)
		|| (Count > 0 && !File->Read(Bytes.GetData(), Count)))
	{
		OutErrorCode = TEXT("job_log_unavailable");
		OutErrorMessage = TEXT("The Development Trace log chunk could not be read.");
		return false;
	}
	FString Text;
	if (Count > 0)
	{
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Count);
		Text = FString(Converted.Length(), Converted.Get());
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("jobId"), Job.JobId);
	OutData->SetNumberField(TEXT("cursor"), Cursor);
	OutData->SetNumberField(TEXT("nextCursor"), Cursor + Count);
	OutData->SetNumberField(TEXT("retainedFromCursor"), 0);
	OutData->SetStringField(TEXT("text"), Text);
	OutData->SetBoolField(TEXT("eof"), Cursor + Count >= Size);
	OutData->SetBoolField(TEXT("truncated"), false);
	return true;
}

bool FTraceWorkerLaunch::ReadJobArtifact(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	const FString JobId = JsonString(Params, TEXT("traceId"));
	FLaunchJob Job;
	if (!LoadJob(JobId, Job, OutErrorMessage))
	{
		OutErrorCode = TEXT("job_not_found");
		return false;
	}
	const FString ArtifactId = JsonString(Params, TEXT("artifactId"));
	FString Path;
	FString Kind;
	if (ArtifactId == TEXT("trace") || ArtifactId == TEXT("utrace"))
	{
		Path = Job.TracePath;
		Kind = TEXT("utrace");
	}
	else if (ArtifactId == TEXT("log"))
	{
		Path = Job.LogPath;
		Kind = TEXT("log");
	}
	else if (ArtifactId == TEXT("receipt"))
	{
		Path = Job.ReceiptPath;
		Kind = TEXT("json");
	}
	else
	{
		OutErrorCode = TEXT("artifact_not_found");
		OutErrorMessage = TEXT("The artifact does not belong to this Development Trace job.");
		return false;
	}
	FString FinalArtifactPath;
	if (!FTraceStore::ResolveOwnedFileForRead(
		Path,
		Job.JobDirectory,
		FPaths::GetPath(Job.ProjectPath),
		FinalArtifactPath))
	{
		OutErrorCode = TEXT("artifact_not_found");
		OutErrorMessage = TEXT("The persisted artifact path failed its job ownership check.");
		return false;
	}
	TUniquePtr<IFileHandle> File(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FinalArtifactPath));
	if (!File.IsValid())
	{
		OutErrorCode = TEXT("artifact_unavailable");
		OutErrorMessage = TEXT("The Development Trace artifact is not available yet.");
		return false;
	}
	const int64 Size = File->Size();
	const int64 Offset = static_cast<int64>(
		JsonNumber(Params, TEXT("offset"), 0.0));
	const int32 MaxBytes = FMath::Clamp(
		static_cast<int32>(JsonNumber(Params, TEXT("maxBytes"), 1024.0 * 1024.0)),
		1,
		1024 * 1024);
	if (Offset < 0 || Offset > Size)
	{
		OutErrorCode = TEXT("artifact_offset_invalid");
		OutErrorMessage = TEXT("offset is outside the Development Trace artifact.");
		return false;
	}
	const int32 Count = static_cast<int32>(
		FMath::Min<int64>(MaxBytes, Size - Offset));
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Count);
	if (!File->Seek(Offset)
		|| (Count > 0 && !File->Read(Bytes.GetData(), Count)))
	{
		OutErrorCode = TEXT("artifact_unavailable");
		OutErrorMessage = TEXT("The Development Trace artifact chunk could not be read.");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("jobId"), Job.JobId);
	OutData->SetStringField(TEXT("artifactId"), ArtifactId);
	OutData->SetStringField(TEXT("kind"), Kind);
	OutData->SetStringField(TEXT("path"), FinalArtifactPath);
	OutData->SetNumberField(TEXT("sizeBytes"), Size);
	OutData->SetNumberField(TEXT("offset"), Offset);
	OutData->SetNumberField(TEXT("nextOffset"), Offset + Count);
	OutData->SetBoolField(TEXT("eof"), Offset + Count >= Size);
	OutData->SetStringField(TEXT("contentBase64"), FBase64::Encode(Bytes));
	return true;
}

bool FTraceWorkerLaunch::Status(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	const FString JobId = JsonString(Params, TEXT("traceId"));
	FLaunchJob Job;
	if (!LoadJob(JobId, Job, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Receipt;
	if (!RefreshJob(Job, Receipt, OutErrorCode, OutErrorMessage))
	{
		return false;
	}
	OutData = JobToResult(Job, Receipt);
	return true;
}

bool FTraceWorkerLaunch::Stop(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	const FString JobId = JsonString(Params, TEXT("traceId"));
	FLaunchJob Job;
	if (!LoadJob(JobId, Job, OutErrorMessage))
	{
		OutErrorCode = TEXT("trace_launch_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Receipt;
	if (!RefreshJob(Job, Receipt, OutErrorCode, OutErrorMessage))
	{
		return false;
	}
	if (Job.Status == TEXT("running"))
	{
		TSharedPtr<FJsonObject> StopRequest = MakeShared<FJsonObject>();
		StopRequest->SetStringField(TEXT("schema"), TEXT("ue.trace-runtime-stop.v1"));
		StopRequest->SetStringField(TEXT("jobId"), Job.JobId);
		StopRequest->SetStringField(TEXT("nonce"), Job.StopNonce);
		if (!PublishJson(Job.StopPath, StopRequest, OutErrorMessage))
		{
			OutErrorCode = TEXT("trace_stop_failed");
			return false;
		}
		Job.StopRequestedAtUtc = FDateTime::UtcNow().ToIso8601();
		Job.StopRequestedUnixSeconds =
			static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp());
		TransitionPhase(Job, TEXT("finalizing"));
		if (!SaveJob(Job, OutErrorMessage))
		{
			OutErrorCode = TEXT("trace_launch_storage_unavailable");
			return false;
		}
	}
	OutData = JobToResult(Job, Receipt);
	OutData->SetBoolField(
		TEXT("accepted"), Job.Status == TEXT("running"));
	return true;
}
}
