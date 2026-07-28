#include "Infrastructure/ProductionJobRuntime.h"

#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Infrastructure/Sha256.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "RenderTimer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealEngine.h"

namespace UEAIIntegration::Infrastructure
{
namespace ProductionJobRuntimePrivate
{
	constexpr int32 MaxCapturedLogChars = 1024 * 1024;
	constexpr int32 MaxArtifactChunkBytes = 1024 * 1024;
	constexpr int64 MaxSynchronousArtifactHashBytes =
		64ll * 1024ll * 1024ll;
	constexpr int32 MaxPackageScanFiles = 4096;
	constexpr int32 MaxPackageArtifacts = 32;

	FString PackageOutputRoot()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UEAIIntegration/Packages")));
	}

	struct FBoundedPackageVisitor final
		: IPlatformFile::FDirectoryVisitor
	{
		bool Visit(
			const TCHAR* FilenameOrDirectory,
			const bool bIsDirectory) override
		{
			if (bIsDirectory)
			{
				return true;
			}
			if (FileCount >= MaxPackageScanFiles)
			{
				bScanTruncated = true;
				return false;
			}

			const FString Filename(FilenameOrDirectory);
			++FileCount;
			const int64 Size = IFileManager::Get().FileSize(*Filename);
			if (Size > 0)
			{
				TotalBytes += Size;
			}
			const FString Extension =
				FPaths::GetExtension(Filename).ToLower();
			if (Extension == TEXT("exe")
				|| Extension == TEXT("pak")
				|| Extension == TEXT("utoc")
				|| Extension == TEXT("ucas")
				|| Extension == TEXT("target")
				|| Extension == TEXT("manifest"))
			{
				++EligibleArtifactCount;
				if (CandidateFiles.Num() < MaxPackageArtifacts)
				{
					CandidateFiles.Add(Filename);
				}
			}
			return true;
		}

		int32 FileCount = 0;
		int32 EligibleArtifactCount = 0;
		int64 TotalBytes = 0;
		bool bScanTruncated = false;
		TArray<FString> CandidateFiles;
	};

	TSharedPtr<FJsonObject> CopyObject(const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		if (Source.IsValid())
		{
			Copy->Values = Source->Values;
		}
		return Copy;
	}

	void WriteCanonicalJsonValue(
		const TSharedPtr<FJsonValue>& Value,
		FString& Out)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			Out += TEXT("null");
			return;
		}
		switch (Value->Type)
		{
		case EJson::String:
		{
			FString Escaped;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Escaped);
			Writer->WriteValue(Value->AsString());
			Writer->Close();
			Out += Escaped;
			break;
		}
		case EJson::Number:
			Out += FString::Printf(TEXT("%.17g"), Value->AsNumber());
			break;
		case EJson::Boolean:
			Out += Value->AsBool() ? TEXT("true") : TEXT("false");
			break;
		case EJson::Array:
		{
			Out += TEXT("[");
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				WriteCanonicalJsonValue(Values[Index], Out);
			}
			Out += TEXT("]");
			break;
		}
		case EJson::Object:
		{
			Out += TEXT("{");
			TArray<FString> Keys;
			Value->AsObject()->Values.GetKeys(Keys);
			Keys.Sort();
			for (int32 Index = 0; Index < Keys.Num(); ++Index)
			{
				if (Index > 0)
				{
					Out += TEXT(",");
				}
				WriteCanonicalJsonValue(
					MakeShared<FJsonValueString>(Keys[Index]),
					Out);
				Out += TEXT(":");
				WriteCanonicalJsonValue(
					Value->AsObject()->Values.FindRef(Keys[Index]),
					Out);
			}
			Out += TEXT("}");
			break;
		}
		default:
			Out += TEXT("null");
			break;
		}
	}

	FString ComputeFileSha256Stream(const FString& Path)
	{
		TArray<uint8> Bytes;
		FString Hash;
		return FFileHelper::LoadFileToArray(Bytes, *Path)
			&& TrySha256Hex(Bytes, Hash)
				? Hash
				: FString();
	}

	FString XmlEscape(FString Value)
	{
		Value.ReplaceInline(TEXT("&"), TEXT("&amp;"));
		Value.ReplaceInline(TEXT("<"), TEXT("&lt;"));
		Value.ReplaceInline(TEXT(">"), TEXT("&gt;"));
		Value.ReplaceInline(TEXT("\""), TEXT("&quot;"));
		Value.ReplaceInline(TEXT("'"), TEXT("&apos;"));
		return Value;
	}

	bool IsSafeLegacyArgumentString(const FString& Value)
	{
		return Value.Len() <= 4096
			&& !Value.Contains(TEXT("\r"))
			&& !Value.Contains(TEXT("\n"))
			&& !Value.Contains(TEXT("\""))
			&& !Value.Contains(TEXT("&"))
			&& !Value.Contains(TEXT("|"))
			&& !Value.Contains(TEXT("<"))
			&& !Value.Contains(TEXT(">"))
			&& !Value.Contains(TEXT(";"))
			&& !Value.Contains(TEXT("%"))
			&& !Value.Contains(TEXT("^"));
	}

	FString MakeUatExecutable(FString& InOutArguments)
	{
		const FString UatPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::EngineDir(),
				TEXT("Build/BatchFiles/RunUAT")
#if PLATFORM_WINDOWS
				TEXT(".bat")
#else
				TEXT(".sh")
#endif
			));
#if PLATFORM_WINDOWS
		FString CommandInterpreter =
			FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
		if (CommandInterpreter.IsEmpty())
		{
			CommandInterpreter = TEXT("cmd.exe");
		}
		InOutArguments = FString::Printf(
			TEXT("/d /s /c \"\"%s\" %s\""),
			*UatPath,
			*InOutArguments);
		return CommandInterpreter;
#else
		return UatPath;
#endif
	}

	FString GetEditorCommandletExecutable()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::EngineDir(),
#if PLATFORM_WINDOWS
				TEXT("Binaries/Win64/UnrealEditor-Cmd.exe")
#elif PLATFORM_MAC
				TEXT("Binaries/Mac/UnrealEditor-Cmd")
#else
				TEXT("Binaries/Linux/UnrealEditor-Cmd")
#endif
			));
	}

	FString GetJobIdParam(const TSharedPtr<FJsonObject>& Params)
	{
		FString Id;
		if (Params.IsValid())
		{
			if (!Params->TryGetStringField(TEXT("jobId"), Id))
			{
				if (!Params->TryGetStringField(TEXT("runId"), Id))
				{
					Params->TryGetStringField(TEXT("traceId"), Id);
				}
			}
		}
		return Id;
	}

	bool IsSafeRelativeGitPath(
		const FString& Input,
		FString& OutRelative)
	{
		if (Input.IsEmpty()
			|| Input.Len() > 512
			|| Input.Contains(TEXT("\""))
			|| Input.Contains(TEXT("\r"))
			|| Input.Contains(TEXT("\n")))
		{
			return false;
		}
		FString Relative = Input;
		FPaths::NormalizeFilename(Relative);
		if (FPaths::IsRelative(Relative)
			&& !Relative.StartsWith(TEXT("../"))
			&& Relative != TEXT(".."))
		{
			OutRelative = Relative;
			return true;
		}
		const FString Root =
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString Full =
			FPaths::ConvertRelativePathToFull(Relative);
		if (!FProductionJobRuntime::IsPathWithin(Full, Root))
		{
			return false;
		}
		OutRelative = Full;
		FPaths::MakePathRelativeTo(OutRelative, *Root);
		FPaths::NormalizeFilename(OutRelative);
		return true;
	}

	TArray<FString> ReadStringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		if (!Object.IsValid()
			|| !Object->HasTypedField<EJson::Array>(Field))
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value :
			Object->GetArrayField(Field))
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				Result.Add(Value->AsString());
			}
		}
		return Result;
	}

	FMCPToolResult MakeJobStartFailure(const FString& Error)
	{
		if (Error.StartsWith(TEXT("idempotency conflict")))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("idempotency_conflict"),
				409);
		}
		if (Error.Contains(TEXT("resource lock")))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("job_conflict"),
				409);
		}
		return FMCPToolResult::Error(
			Error,
			TEXT("job_start_failed"),
			503);
	}
}

using namespace ProductionJobRuntimePrivate;

FProductionJobRuntime::FProductionJobRuntime()
{
	IFileManager::Get().MakeDirectory(*JobsRoot(), true);
	LoadJournals();
}

FProductionJobRuntime::~FProductionJobRuntime()
{
	if (!ActiveTraceJobId.IsEmpty() && FTraceAuxiliary::IsConnected())
	{
		FTraceAuxiliary::Stop();
	}
	for (TPair<FString, TSharedPtr<FJob>>& Pair : Jobs)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		FJob& Job = *Pair.Value;
		if (Job.ProcessHandle.IsValid())
		{
			if (FPlatformProcess::IsProcRunning(Job.ProcessHandle))
			{
				FPlatformProcess::TerminateProc(Job.ProcessHandle, true);
			}
			if (!IsTerminalStatus(Job.Status))
			{
				Job.Status = TEXT("interrupted");
				Job.Phase = TEXT("complete");
				Job.ErrorCode = TEXT("editor_shutdown");
				Job.Message =
					TEXT("The Editor shut down before this job completed.");
				Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
				Job.Progress = 1.0;
				SaveJournal(Job);
			}
			FPlatformProcess::CloseProc(Job.ProcessHandle);
			Job.ProcessHandle.Reset();
		}
		if (Job.ReadPipe || Job.WritePipe)
		{
			FPlatformProcess::ClosePipe(Job.ReadPipe, Job.WritePipe);
			Job.ReadPipe = nullptr;
			Job.WritePipe = nullptr;
		}
	}
}

void FProductionJobRuntime::Tick(float DeltaTime)
{
	for (TPair<FString, TSharedPtr<FJob>>& Pair : Jobs)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Status != TEXT("running"))
		{
			continue;
		}
		if (Pair.Value->Kind == TEXT("performance"))
		{
			TickPerformanceJob(*Pair.Value);
		}
		else if (Pair.Value->Kind != TEXT("trace"))
		{
			TickProcessJob(*Pair.Value);
		}
	}
}

FMCPToolResult FProductionJobRuntime::Execute(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params)
{
	if (CapabilityId == TEXT("production.job.status")) return GetJobStatus(Params);
	if (CapabilityId == TEXT("production.job.cancel")) return CancelJob(Params);
	if (CapabilityId == TEXT("production.job.result.get")) return GetJobResult(Params);
	if (CapabilityId == TEXT("production.job.log.get")) return GetJobLog(Params);
	if (CapabilityId == TEXT("production.job.artifact.get")) return GetJobArtifact(Params);
	if (CapabilityId == TEXT("production.trace.start")) return StartTrace(Params);
	if (CapabilityId == TEXT("production.trace.status")) return GetTraceStatus(Params);
	if (CapabilityId == TEXT("production.trace.stop")) return StopTrace(Params);
	if (CapabilityId == TEXT("production.trace.analyze")) return AnalyzeTrace(Params);
	if (CapabilityId == TEXT("production.performance.run")) return StartPerformanceRun(Params);
	if (CapabilityId == TEXT("production.performance.result.get")) return GetPerformanceResult(Params);
	if (CapabilityId == TEXT("production.performance.compare")) return ComparePerformanceRuns(Params);
	if (CapabilityId == TEXT("production.test.list")) return ListTests(Params);
	if (CapabilityId == TEXT("production.test.run")) return StartTestRun(Params);
	if (CapabilityId == TEXT("production.test.result.get")) return GetTestResult(Params);
	if (CapabilityId == TEXT("production.project.cook")) return StartCook(Params);
	if (CapabilityId == TEXT("production.project.package")) return StartPackage(Params);
	if (CapabilityId == TEXT("production.commandlet.run")) return StartCommandlet(Params);
	if (CapabilityId == TEXT("production.source_control.repository.get")) return GetSourceControlRepository(Params);
	if (CapabilityId == TEXT("production.source_control.status")) return GetSourceControlStatus(Params);
	if (CapabilityId == TEXT("production.source_control.diff")) return GetSourceControlDiff(Params);
	if (CapabilityId == TEXT("production.source_control.change.plan")) return PlanSourceControlChange(Params);
	if (CapabilityId == TEXT("production.source_control.change.execute")) return ExecuteSourceControlChange(Params);
	if (CapabilityId == TEXT("production.ddc.status")) return GetDdcStatus(Params);
	if (CapabilityId == TEXT("production.ddc.job.start")) return StartDdcJob(Params);
	if (CapabilityId == TEXT("production.buildgraph.validate")) return ValidateBuildGraph(Params);
	if (CapabilityId == TEXT("production.buildgraph.run")) return StartBuildGraph(Params);
	if (CapabilityId == TEXT("production.horde.context.get")) return GetHordeContext(Params);
	return FMCPToolResult::Error(
		FString::Printf(TEXT("Unsupported production operation '%s'."), *CapabilityId),
		TEXT("capability_not_found"),
		404);
}

FString FProductionJobRuntime::ComputeChangePlanDigest(
	const TSharedPtr<FJsonObject>& Request)
{
	FString Canonical;
	WriteCanonicalJsonValue(
		MakeShared<FJsonValueObject>(
			Request.IsValid() ? Request : MakeShared<FJsonObject>()),
		Canonical);
	const FTCHARToUTF8 Utf8(*Canonical);
	FString Hash;
	if (!TrySha256Hex(Utf8.Get(), Utf8.Length(), Hash))
	{
		return FString();
	}
	return TEXT("sha256:") + Hash;
}

bool FProductionJobRuntime::IsPathWithin(
	const FString& Candidate,
	const FString& AllowedRoot)
{
	FString FullCandidate =
		FPaths::ConvertRelativePathToFull(Candidate);
	FString FullRoot =
		FPaths::ConvertRelativePathToFull(AllowedRoot);
	FPaths::NormalizeDirectoryName(FullCandidate);
	FPaths::NormalizeDirectoryName(FullRoot);
#if PLATFORM_WINDOWS
	FullCandidate = FullCandidate.ToLower();
	FullRoot = FullRoot.ToLower();
#endif
	return FullCandidate == FullRoot
		|| FullCandidate.StartsWith(FullRoot + TEXT("/"))
		|| FullCandidate.StartsWith(FullRoot + TEXT("\\"));
}

TSharedPtr<FJsonObject> FProductionJobRuntime::SummarizeMetric(
	const TArray<double>& Samples,
	double BudgetMs)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("sampleCount"), Samples.Num());
	Summary->SetStringField(TEXT("unit"), TEXT("ms"));
	if (Samples.IsEmpty())
	{
		Summary->SetBoolField(TEXT("available"), false);
		return Summary;
	}

	TArray<double> Sorted = Samples;
	Sorted.Sort();
	double Sum = 0.0;
	int32 OverBudget = 0;
	for (const double Value : Sorted)
	{
		Sum += Value;
		if (Value > BudgetMs)
		{
			++OverBudget;
		}
	}
	auto Percentile = [&Sorted](const double Fraction)
	{
		const double Position = Fraction * (Sorted.Num() - 1);
		const int32 Lower = FMath::FloorToInt(Position);
		const int32 Upper = FMath::CeilToInt(Position);
		if (Lower == Upper)
		{
			return Sorted[Lower];
		}
		return FMath::Lerp(
			Sorted[Lower],
			Sorted[Upper],
			Position - Lower);
	};
	Summary->SetBoolField(TEXT("available"), true);
	Summary->SetNumberField(TEXT("min"), Sorted[0]);
	Summary->SetNumberField(TEXT("max"), Sorted.Last());
	Summary->SetNumberField(TEXT("mean"), Sum / Sorted.Num());
	Summary->SetNumberField(TEXT("p50"), Percentile(0.50));
	Summary->SetNumberField(TEXT("p95"), Percentile(0.95));
	Summary->SetNumberField(TEXT("p99"), Percentile(0.99));
	Summary->SetNumberField(TEXT("budgetMs"), BudgetMs);
	Summary->SetNumberField(TEXT("overBudgetFrames"), OverBudget);
	return Summary;
}

FMCPToolResult FProductionJobRuntime::GetJobStatus(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(JobId);
	if (!Job || !Job->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Job '%s' was not found."), *JobId),
			TEXT("job_not_found"),
			404);
	}
	return FMCPToolResult::Ok(MakeJobSummary(**Job, false));
}

FMCPToolResult FProductionJobRuntime::CancelJob(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString JobId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* JobPtr = Jobs.Find(JobId);
	if (!JobPtr || !JobPtr->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Job '%s' was not found."), *JobId),
			TEXT("job_not_found"),
			404);
	}
	FJob& Job = **JobPtr;
	if (IsTerminalStatus(Job.Status))
	{
		return FMCPToolResult::Error(
			TEXT("A terminal job cannot be cancelled."),
			TEXT("job_not_cancellable"),
			409);
	}

	if (Job.Kind == TEXT("trace"))
	{
		FTraceAuxiliary::Stop();
		if (ActiveTraceJobId == Job.Id)
		{
			ActiveTraceJobId.Reset();
		}
	}
	else if (Job.bOwnsTrace && !Job.TraceJobId.IsEmpty())
	{
		TSharedRef<FJsonObject> StopParams = MakeShared<FJsonObject>();
		StopParams->SetStringField(TEXT("traceId"), Job.TraceJobId);
		StopTrace(StopParams);
	}
	if (Job.ProcessHandle.IsValid())
	{
		FPlatformProcess::TerminateProc(Job.ProcessHandle, true);
	}
	FinishJob(Job, TEXT("cancelled"), TEXT("cancelled"), TEXT("Cancellation requested."));
	return FMCPToolResult::Ok(MakeJobSummary(Job, false));
}

FMCPToolResult FProductionJobRuntime::GetJobResult(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(JobId);
	if (!Job || !Job->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Job '%s' was not found."), *JobId),
			TEXT("job_not_found"),
			404);
	}
	return FMCPToolResult::Ok(MakeJobSummary(**Job, true));
}

FMCPToolResult FProductionJobRuntime::GetJobLog(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(JobId);
	if (!Job || !Job->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Job '%s' was not found."), *JobId),
			TEXT("job_not_found"),
			404);
	}
	const int64 RequestedCursor = static_cast<int64>(
		GetNumberFieldOr(Params, TEXT("cursor"), (*Job)->LogBaseCursor));
	const int32 MaxChars = FMath::Clamp(
		static_cast<int32>(GetNumberFieldOr(Params, TEXT("maxChars"), 16384)),
		1,
		65536);
	const int64 EffectiveCursor =
		FMath::Clamp<int64>(
			RequestedCursor,
			(*Job)->LogBaseCursor,
			(*Job)->LogTotalChars);
	const int32 LocalOffset =
		static_cast<int32>(EffectiveCursor - (*Job)->LogBaseCursor);
	const FString Chunk = (*Job)->Output.Mid(LocalOffset, MaxChars);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("jobId"), JobId);
	Data->SetNumberField(TEXT("cursor"), EffectiveCursor);
	Data->SetNumberField(TEXT("nextCursor"), EffectiveCursor + Chunk.Len());
	Data->SetNumberField(TEXT("retainedFromCursor"), (*Job)->LogBaseCursor);
	Data->SetStringField(TEXT("text"), Chunk);
	Data->SetBoolField(
		TEXT("eof"),
		EffectiveCursor + Chunk.Len() >= (*Job)->LogTotalChars);
	Data->SetBoolField(
		TEXT("truncated"),
		RequestedCursor < (*Job)->LogBaseCursor);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::GetJobArtifact(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = GetJobIdParam(Params);
	const FString ArtifactId =
		GetStringFieldOr(Params, TEXT("artifactId"));
	const TSharedPtr<FJob>* Job = Jobs.Find(JobId);
	if (!Job || !Job->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Job '%s' was not found."), *JobId),
			TEXT("job_not_found"),
			404);
	}
	const FArtifact* Artifact = (*Job)->Artifacts.FindByPredicate(
		[&ArtifactId](const FArtifact& Candidate)
		{
			return Candidate.Id == ArtifactId;
		});
	if (!Artifact)
	{
		return FMCPToolResult::Error(
			FString::Printf(
				TEXT("Artifact '%s' does not belong to job '%s'."),
				*ArtifactId,
				*JobId),
			TEXT("artifact_not_found"),
			404);
	}

	const auto MatchesRegisteredFile = [Artifact]()
	{
		const int64 CurrentSize =
			IFileManager::Get().FileSize(*Artifact->Path);
		const FDateTime CurrentModifiedAt =
			IFileManager::Get().GetTimeStamp(*Artifact->Path);
		return CurrentSize == Artifact->Size
			&& (Artifact->RegisteredModifiedAtUtc.GetTicks() == 0
				|| CurrentModifiedAt
					== Artifact->RegisteredModifiedAtUtc);
	};
	if (!MatchesRegisteredFile())
	{
		return FMCPToolResult::Error(
			TEXT(
				"The artifact changed after it was registered for this job."),
			TEXT("artifact_changed"),
			409);
	}
	TUniquePtr<IFileHandle> File(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Artifact->Path));
	if (!File.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("The artifact file is no longer available."),
			TEXT("artifact_unavailable"),
			410);
	}
	const int64 Size = File->Size();
	if (Size != Artifact->Size)
	{
		return FMCPToolResult::Error(
			TEXT(
				"The artifact size no longer matches its job receipt."),
			TEXT("artifact_changed"),
			409);
	}
	const int64 Offset = static_cast<int64>(
		GetNumberFieldOr(Params, TEXT("offset"), 0.0));
	if (Offset < 0 || Offset > Size)
	{
		return FMCPToolResult::Error(
			TEXT("offset is outside the registered artifact."),
			TEXT("artifact_offset_invalid"),
			416);
	}
	const int32 MaxBytes = FMath::Clamp(
		static_cast<int32>(
			GetNumberFieldOr(
				Params,
				TEXT("maxBytes"),
				MaxArtifactChunkBytes)),
		1,
		MaxArtifactChunkBytes);
	const int32 BytesToRead =
		static_cast<int32>(FMath::Min<int64>(MaxBytes, Size - Offset));
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(BytesToRead);
	if (!File->Seek(Offset)
		|| (BytesToRead > 0
			&& !File->Read(Bytes.GetData(), BytesToRead)))
	{
		return FMCPToolResult::Error(
			TEXT("The artifact chunk could not be read."),
			TEXT("artifact_unavailable"),
			500);
	}
	if (!MatchesRegisteredFile())
	{
		return FMCPToolResult::Error(
			TEXT(
				"The artifact changed while its chunk was being read."),
			TEXT("artifact_changed"),
			409);
	}

	TSharedPtr<FJsonObject> Data = MakeArtifactSummary(*Artifact);
	Data->SetStringField(TEXT("jobId"), JobId);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("nextOffset"), Offset + BytesToRead);
	Data->SetBoolField(TEXT("eof"), Offset + BytesToRead >= Size);
	Data->SetStringField(TEXT("contentBase64"), FBase64::Encode(Bytes));
	return FMCPToolResult::Ok(Data);
}

TSharedPtr<FProductionJobRuntime::FJob>
FProductionJobRuntime::CreateJob(
	const FString& Kind,
	const TSharedPtr<FJsonObject>& Input)
{
	TSharedPtr<FJob> Job = MakeShared<FJob>();
	Job->Id = NewOpaqueId(TEXT("job"));
	Job->Kind = Kind;
	Job->Status = TEXT("queued");
	Job->Phase = TEXT("queued");
	Job->CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job->Input = CopyObject(Input);
	Job->RequestId = GetStringFieldOr(Input, TEXT("requestId"));
	Job->InputDigest = ComputeChangePlanDigest(Job->Input);
	Job->Result = MakeShared<FJsonObject>();
	Jobs.Add(Job->Id, Job);
	SaveJournal(*Job);
	return Job;
}

TSharedPtr<FProductionJobRuntime::FJob>
FProductionJobRuntime::FindIdempotentJob(
	const FString& Kind,
	const TSharedPtr<FJsonObject>& Input,
	bool& bOutConflict) const
{
	bOutConflict = false;
	const FString RequestId =
		GetStringFieldOr(Input, TEXT("requestId"));
	if (RequestId.IsEmpty())
	{
		return nullptr;
	}
	const FString InputDigest = ComputeChangePlanDigest(Input);
	for (const TPair<FString, TSharedPtr<FJob>>& Pair : Jobs)
	{
		if (!Pair.Value.IsValid()
			|| Pair.Value->RequestId != RequestId)
		{
			continue;
		}
		if (Pair.Value->Kind == Kind
			&& Pair.Value->InputDigest == InputDigest)
		{
			return Pair.Value;
		}
		bOutConflict = true;
		return nullptr;
	}
	return nullptr;
}

TSharedPtr<FProductionJobRuntime::FJob>
FProductionJobRuntime::StartProcessJob(
	const FString& Kind,
	const FString& Executable,
	const FString& Arguments,
	const FString& WorkingDirectory,
	double TimeoutSeconds,
	const FString& PostProcess,
	const TSharedPtr<FJsonObject>& Input,
	FString& OutError)
{
	OutError.Reset();
	bool bIdempotencyConflict = false;
	if (TSharedPtr<FJob> Existing =
		FindIdempotentJob(Kind, Input, bIdempotencyConflict))
	{
		return Existing;
	}
	if (bIdempotencyConflict)
	{
		OutError =
			TEXT("idempotency conflict: requestId is already bound to a different durable job request.");
		return nullptr;
	}
	if (!ActiveHeavyJobId.IsEmpty())
	{
		const TSharedPtr<FJob>* Active = Jobs.Find(ActiveHeavyJobId);
		if (Active && Active->IsValid() && (*Active)->Status == TEXT("running"))
		{
			OutError = FString::Printf(
				TEXT("Job '%s' currently holds the production resource lock."),
				*ActiveHeavyJobId);
			return nullptr;
		}
		ActiveHeavyJobId.Reset();
	}
	if (Executable.IsEmpty()
		|| (!Executable.Equals(TEXT("git.exe"), ESearchCase::IgnoreCase)
			&& !IFileManager::Get().FileExists(*Executable)))
	{
		OutError = FString::Printf(
			TEXT("Required executable '%s' is unavailable."),
			*Executable);
		return nullptr;
	}

	TSharedPtr<FJob> Job = CreateJob(Kind, Input);
	Job->Executable = Executable;
	Job->Arguments = Arguments;
	Job->WorkingDirectory = WorkingDirectory;
	Job->PostProcess = PostProcess;
	Job->Status = TEXT("running");
	Job->Phase = TEXT("launching");
	Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job->StartedAtSeconds = FPlatformTime::Seconds();
	Job->TimeoutAtSeconds =
		TimeoutSeconds > 0.0
			? Job->StartedAtSeconds + TimeoutSeconds
			: 0.0;
	if (!FPlatformProcess::CreatePipe(Job->ReadPipe, Job->WritePipe))
	{
		OutError = TEXT("Failed to create the job output pipe.");
		Job->Status = TEXT("failed");
		Job->Phase = TEXT("complete");
		Job->ErrorCode = TEXT("job_start_failed");
		Job->Message = OutError;
		Job->Progress = 1.0;
		Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		SaveJournal(*Job);
		return nullptr;
	}

	uint32 ProcessId = 0;
#if PLATFORM_WINDOWS
	Job->ProcessHandle = FPlatformProcess::CreateProc(
		*Executable,
		*Arguments,
		false,
		true,
		true,
		&ProcessId,
		0,
		WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory,
		Job->WritePipe,
		nullptr,
		Job->WritePipe);
#else
	Job->ProcessHandle = FPlatformProcess::CreateProc(
		*Executable,
		*Arguments,
		false,
		true,
		true,
		&ProcessId,
		0,
		WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory,
		Job->WritePipe,
		nullptr);
#endif
	if (!Job->ProcessHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(Job->ReadPipe, Job->WritePipe);
		Job->ReadPipe = nullptr;
		Job->WritePipe = nullptr;
		OutError = FString::Printf(
			TEXT("Failed to launch %s job."),
			*Kind);
		Job->Status = TEXT("failed");
		Job->Phase = TEXT("complete");
		Job->ErrorCode = TEXT("job_start_failed");
		Job->Message = OutError;
		Job->Progress = 1.0;
		Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		SaveJournal(*Job);
		return nullptr;
	}
	Job->ProcessId = ProcessId;
	Job->Phase = TEXT("running");
	ActiveHeavyJobId = Job->Id;
	SaveJournal(*Job);
	return Job;
}

void FProductionJobRuntime::TickProcessJob(FJob& Job)
{
	if (Job.ReadPipe)
	{
		const FString Chunk = FPlatformProcess::ReadPipe(Job.ReadPipe);
		if (!Chunk.IsEmpty())
		{
			Job.Output += Chunk;
			Job.LogTotalChars += Chunk.Len();
			if (Job.Output.Len() > MaxCapturedLogChars)
			{
				const int32 Removed = Job.Output.Len() - MaxCapturedLogChars;
				Job.Output.RightInline(MaxCapturedLogChars, false);
				Job.LogBaseCursor += Removed;
			}
			const FString JobLogFile =
				FPaths::Combine(JobDirectory(Job.Id), TEXT("job.log"));
			FFileHelper::SaveStringToFile(
				Chunk,
				*JobLogFile,
				FFileHelper::EEncodingOptions::AutoDetect,
				&IFileManager::Get(),
				FILEWRITE_Append);
		}
	}

	if (Job.TimeoutAtSeconds > 0.0
		&& FPlatformTime::Seconds() >= Job.TimeoutAtSeconds)
	{
		if (Job.ProcessHandle.IsValid())
		{
			FPlatformProcess::TerminateProc(Job.ProcessHandle, true);
		}
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("job_timeout"),
			TEXT("The job exceeded its configured timeout."));
		return;
	}
	if (!Job.ProcessHandle.IsValid())
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("process_lost"),
			TEXT("The process handle was lost."));
		return;
	}
	if (FPlatformProcess::IsProcRunning(Job.ProcessHandle))
	{
		const double TimeoutSpan =
			Job.TimeoutAtSeconds > Job.StartedAtSeconds
				? Job.TimeoutAtSeconds - Job.StartedAtSeconds
				: 0.0;
		if (TimeoutSpan > 0.0)
		{
			Job.Progress = FMath::Clamp(
				(FPlatformTime::Seconds() - Job.StartedAtSeconds)
					/ TimeoutSpan,
				0.0,
				0.95);
		}
		return;
	}

	int32 ReturnCode = INDEX_NONE;
	FPlatformProcess::GetProcReturnCode(Job.ProcessHandle, &ReturnCode);
	Job.ReturnCode = ReturnCode;
	FinishJob(
		Job,
		ReturnCode == 0 ? TEXT("succeeded") : TEXT("failed"),
		ReturnCode == 0 ? FString() : TEXT("process_failed"),
		ReturnCode == 0
			? FString()
			: FString::Printf(
				TEXT("The process exited with code %d."),
				ReturnCode));
}

void FProductionJobRuntime::FinishJob(
	FJob& Job,
	const FString& Status,
	const FString& ErrorCode,
	const FString& Message)
{
	if (Job.ProcessHandle.IsValid())
	{
		if (Job.ReadPipe)
		{
			const FString FinalChunk = FPlatformProcess::ReadPipe(Job.ReadPipe);
			if (!FinalChunk.IsEmpty())
			{
				Job.Output += FinalChunk;
				Job.LogTotalChars += FinalChunk.Len();
				FFileHelper::SaveStringToFile(
					FinalChunk,
					*FPaths::Combine(JobDirectory(Job.Id), TEXT("job.log")),
					FFileHelper::EEncodingOptions::AutoDetect,
					&IFileManager::Get(),
					FILEWRITE_Append);
			}
		}
		FPlatformProcess::CloseProc(Job.ProcessHandle);
		Job.ProcessHandle.Reset();
	}
	if (Job.ReadPipe || Job.WritePipe)
	{
		FPlatformProcess::ClosePipe(Job.ReadPipe, Job.WritePipe);
		Job.ReadPipe = nullptr;
		Job.WritePipe = nullptr;
	}
	if (Job.Output.Len() > MaxCapturedLogChars)
	{
		const int32 Removed = Job.Output.Len() - MaxCapturedLogChars;
		Job.Output.RightInline(MaxCapturedLogChars, false);
		Job.LogBaseCursor += Removed;
	}
	Job.Status = Status;
	Job.Phase = TEXT("complete");
	Job.ErrorCode = ErrorCode;
	Job.Message = Message;
	Job.Progress = 1.0;
	Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
	if (ActiveHeavyJobId == Job.Id)
	{
		ActiveHeavyJobId.Reset();
	}
	PostProcessJob(Job);
	SaveJournal(Job);
}

FMCPToolResult FProductionJobRuntime::StartTrace(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bIdempotencyConflict = false;
	if (TSharedPtr<FJob> Existing =
		FindIdempotentJob(TEXT("trace"), Params, bIdempotencyConflict))
	{
		TSharedPtr<FJsonObject> Replay = MakeJobSummary(*Existing, false);
		Replay->SetStringField(TEXT("traceId"), Existing->Id);
		Replay->SetBoolField(TEXT("idempotentReplay"), true);
		return FMCPToolResult::Ok(Replay);
	}
	if (bIdempotencyConflict)
	{
		return FMCPToolResult::Error(
			TEXT("requestId is already bound to a different durable job request."),
			TEXT("idempotency_conflict"),
			409);
	}
	if (!ActiveTraceJobId.IsEmpty() || FTraceAuxiliary::IsConnected())
	{
		return FMCPToolResult::Error(
			TEXT("A trace recording is already active."),
			TEXT("trace_busy"),
			409);
	}
	TSharedPtr<FJob> Job = CreateJob(TEXT("trace"), Params);
	const FString TracePath =
		FPaths::Combine(JobDirectory(Job->Id), TEXT("capture.utrace"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TracePath), true);

	TArray<FString> Channels = ReadStringArray(Params, TEXT("channels"));
	if (Channels.IsEmpty())
	{
		Channels = {
			TEXT("default"),
			TEXT("frame"),
			TEXT("cpu"),
			TEXT("bookmark"),
			TEXT("log")
		};
	}
	for (const FString& Channel : Channels)
	{
		if (!IsSafeToken(Channel, 64))
		{
			Jobs.Remove(Job->Id);
			return FMCPToolResult::Error(
				TEXT("Trace channels must be bounded identifier tokens."),
				TEXT("invalid_params"),
				422);
		}
	}
	const FString ChannelList = FString::Join(Channels, TEXT(","));
	FTraceAuxiliary::FOptions Options;
	Options.bTruncateFile = true;
	Options.bExcludeTail = GetBoolFieldOr(
		Params,
		TEXT("excludeTail"),
		true);
	if (!FTraceAuxiliary::Start(
			FTraceAuxiliary::EConnectionType::File,
			*TracePath,
			*ChannelList,
			&Options))
	{
		FinishJob(
			*Job,
			TEXT("failed"),
			TEXT("trace_unavailable"),
			TEXT("Unreal Trace rejected the file recording request."));
		return FMCPToolResult::Error(
			Job->Message,
			Job->ErrorCode,
			503);
	}
	Job->Status = TEXT("running");
	Job->Phase = TEXT("recording");
	Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job->StartedAtSeconds = FPlatformTime::Seconds();
	Job->Result->SetStringField(TEXT("destination"), TracePath);
	Job->Result->SetStringField(TEXT("channels"), ChannelList);
	ActiveTraceJobId = Job->Id;
	SaveJournal(*Job);
	TSharedPtr<FJsonObject> Result = MakeJobSummary(*Job, false);
	Result->SetStringField(TEXT("traceId"), Job->Id);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FProductionJobRuntime::GetTraceStatus(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString TraceId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(TraceId);
	if (!Job || !Job->IsValid() || (*Job)->Kind != TEXT("trace"))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Trace '%s' was not found."), *TraceId),
			TEXT("trace_not_found"),
			404);
	}
	TSharedPtr<FJsonObject> Data = MakeJobSummary(**Job, false);
	Data->SetStringField(TEXT("traceId"), TraceId);
	Data->SetBoolField(
		TEXT("connected"),
		TraceId == ActiveTraceJobId && FTraceAuxiliary::IsConnected());
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::StopTrace(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString RequestedId = GetJobIdParam(Params);
	const FString TraceId =
		RequestedId.IsEmpty() ? ActiveTraceJobId : RequestedId;
	const TSharedPtr<FJob>* JobPtr = Jobs.Find(TraceId);
	if (!JobPtr || !JobPtr->IsValid() || (*JobPtr)->Kind != TEXT("trace"))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Trace '%s' was not found."), *TraceId),
			TEXT("trace_not_found"),
			404);
	}
	FJob& Job = **JobPtr;
	if (Job.Status != TEXT("running"))
	{
		return FMCPToolResult::Ok(MakeJobSummary(Job, true));
	}
	if (TraceId != ActiveTraceJobId || !FTraceAuxiliary::Stop())
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("trace_stop_failed"),
			TEXT("The active trace could not be stopped."));
		ActiveTraceJobId.Reset();
		return FMCPToolResult::Error(Job.Message, Job.ErrorCode, 500);
	}
	ActiveTraceJobId.Reset();
	const FString TracePath =
		GetStringFieldOr(Job.Result, TEXT("destination"));
	AddArtifact(
		Job,
		TracePath,
		TEXT("capture.utrace"),
		TEXT("application/x-unreal-trace"));
	FinishJob(Job, TEXT("succeeded"));
	TSharedPtr<FJsonObject> Result = MakeJobSummary(Job, true);
	Result->SetStringField(TEXT("traceId"), TraceId);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FProductionJobRuntime::AnalyzeTrace(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString TraceId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Trace = Jobs.Find(TraceId);
	if (!Trace || !Trace->IsValid() || (*Trace)->Kind != TEXT("trace"))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Trace '%s' was not found."), *TraceId),
			TEXT("trace_not_found"),
			404);
	}
	if ((*Trace)->Status != TEXT("succeeded"))
	{
		return FMCPToolResult::Error(
			TEXT("Only a completed trace can be analyzed."),
			TEXT("trace_not_ready"),
			409);
	}
	TSharedPtr<FJob> Analysis =
		CreateJob(TEXT("traceAnalysis"), Params);
	Analysis->Status = TEXT("succeeded");
	Analysis->Phase = TEXT("complete");
	Analysis->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Analysis->CompletedAtUtc = Analysis->StartedAtUtc;
	Analysis->Progress = 1.0;
	Analysis->Result->SetStringField(TEXT("traceId"), TraceId);
	Analysis->Result->SetStringField(
		TEXT("analysisLevel"),
		TEXT("artifactSummary"));
	Analysis->Result->SetBoolField(
		TEXT("traceServicesAvailable"),
		false);
	Analysis->Result->SetStringField(
		TEXT("availabilityReason"),
		TEXT("TraceServices provider parsing is not linked in this build; the durable .utrace artifact remains available for Unreal Insights."));
	if (!(*Trace)->Artifacts.IsEmpty())
	{
		Analysis->Result->SetObjectField(
			TEXT("traceArtifact"),
			MakeArtifactSummary((*Trace)->Artifacts[0]));
	}
	SaveJournal(*Analysis);
	return FMCPToolResult::Ok(MakeJobSummary(*Analysis, true));
}

FMCPToolResult FProductionJobRuntime::StartPerformanceRun(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bIdempotencyConflict = false;
	if (TSharedPtr<FJob> Existing =
		FindIdempotentJob(
			TEXT("performance"),
			Params,
			bIdempotencyConflict))
	{
		TSharedPtr<FJsonObject> Replay = MakeJobSummary(*Existing, false);
		Replay->SetStringField(TEXT("runId"), Existing->Id);
		Replay->SetBoolField(TEXT("idempotentReplay"), true);
		return FMCPToolResult::Ok(Replay);
	}
	if (bIdempotencyConflict)
	{
		return FMCPToolResult::Error(
			TEXT("requestId is already bound to a different durable job request."),
			TEXT("idempotency_conflict"),
			409);
	}
	if (!ActiveHeavyJobId.IsEmpty())
	{
		const TSharedPtr<FJob>* Active = Jobs.Find(ActiveHeavyJobId);
		if (Active && Active->IsValid() && (*Active)->Status == TEXT("running"))
		{
			return FMCPToolResult::Error(
				TEXT("Another production job holds the runtime resource lock."),
				TEXT("job_conflict"),
				409);
		}
		ActiveHeavyJobId.Reset();
	}

	const double WarmupSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("warmupSeconds"), 2.0),
		0.0,
		300.0);
	const double SampleSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("sampleSeconds"), 10.0),
		0.1,
		3600.0);
	TSharedPtr<FJob> Job = CreateJob(TEXT("performance"), Params);
	Job->Status = TEXT("running");
	Job->Phase =
		WarmupSeconds > 0.0 ? TEXT("warmup") : TEXT("sampling");
	Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job->StartedAtSeconds = FPlatformTime::Seconds();
	Job->WarmupUntilSeconds = Job->StartedAtSeconds + WarmupSeconds;
	Job->SamplingUntilSeconds =
		Job->WarmupUntilSeconds + SampleSeconds;
	Job->TimeoutAtSeconds = Job->SamplingUntilSeconds + 30.0;
	Job->BudgetMs = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("budgetMs"), 16.6667),
		0.01,
		10000.0);
	Job->Result->SetObjectField(TEXT("context"), MakeRuntimeContext());

	if (GetBoolFieldOr(Params, TEXT("captureTrace"), false))
	{
		TSharedPtr<FJsonObject> TraceParams = MakeShared<FJsonObject>();
		const FMCPToolResult TraceResult = StartTrace(TraceParams);
		if (TraceResult.bSuccess)
		{
			Job->bOwnsTrace = true;
			Job->TraceJobId =
				TraceResult.Data->GetStringField(TEXT("traceId"));
		}
		else
		{
			Job->Result->SetStringField(
				TEXT("traceAvailability"),
				TraceResult.ErrorMessage);
		}
	}
	ActiveHeavyJobId = Job->Id;
	SaveJournal(*Job);
	TSharedPtr<FJsonObject> Result = MakeJobSummary(*Job, false);
	Result->SetStringField(TEXT("runId"), Job->Id);
	return FMCPToolResult::Ok(Result);
}

void FProductionJobRuntime::TickPerformanceJob(FJob& Job)
{
	const double Now = FPlatformTime::Seconds();
	if (Now < Job.WarmupUntilSeconds)
	{
		Job.Phase = TEXT("warmup");
		const double Span =
			FMath::Max(Job.WarmupUntilSeconds - Job.StartedAtSeconds, 0.001);
		Job.Progress = 0.2
			* FMath::Clamp((Now - Job.StartedAtSeconds) / Span, 0.0, 1.0);
		return;
	}
	Job.Phase = TEXT("sampling");
	if (Now < Job.SamplingUntilSeconds)
	{
		const double DeltaMs = FApp::GetDeltaTime() * 1000.0;
		if (DeltaMs >= 0.0)
		{
			Job.MetricSamples.FindOrAdd(TEXT("frameMs")).Add(DeltaMs);
		}
		Job.MetricSamples.FindOrAdd(TEXT("gameMs")).Add(
			FPlatformTime::ToMilliseconds64(GGameThreadTime));
		Job.MetricSamples.FindOrAdd(TEXT("renderMs")).Add(
			FPlatformTime::ToMilliseconds64(GRenderThreadTime));
		Job.MetricSamples.FindOrAdd(TEXT("rhiMs")).Add(
			FPlatformTime::ToMilliseconds64(GRHIThreadTime));
		if (GDynamicRHI)
		{
			Job.MetricSamples.FindOrAdd(TEXT("gpuMs")).Add(
				FPlatformTime::ToMilliseconds64(
					RHIGetGPUFrameCycles()));
		}
		const double Span =
			FMath::Max(Job.SamplingUntilSeconds - Job.WarmupUntilSeconds, 0.001);
		Job.Progress = 0.2 + 0.75
			* FMath::Clamp((Now - Job.WarmupUntilSeconds) / Span, 0.0, 1.0);
		return;
	}

	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	static const TArray<FString> MetricNames = {
		TEXT("frameMs"),
		TEXT("gameMs"),
		TEXT("renderMs"),
		TEXT("rhiMs"),
		TEXT("gpuMs")
	};
	for (const FString& MetricName : MetricNames)
	{
		const TArray<double>* Samples = Job.MetricSamples.Find(MetricName);
		Metrics->SetObjectField(
			MetricName,
			SummarizeMetric(
				Samples ? *Samples : TArray<double>(),
				Job.BudgetMs));
	}
	Job.Result->SetObjectField(TEXT("metrics"), Metrics);
	Job.Result->SetNumberField(
		TEXT("sampleCount"),
		Job.MetricSamples.FindRef(TEXT("frameMs")).Num());
	Job.Result->SetNumberField(TEXT("budgetMs"), Job.BudgetMs);
	if (Job.bOwnsTrace && !Job.TraceJobId.IsEmpty())
	{
		TSharedPtr<FJsonObject> StopParams = MakeShared<FJsonObject>();
		StopParams->SetStringField(TEXT("traceId"), Job.TraceJobId);
		const FMCPToolResult TraceStop = StopTrace(StopParams);
		Job.Result->SetStringField(TEXT("traceId"), Job.TraceJobId);
		Job.Result->SetBoolField(TEXT("traceCompleted"), TraceStop.bSuccess);
	}
	WritePerformanceReport(Job);
	FinishJob(Job, TEXT("succeeded"));
}

FMCPToolResult FProductionJobRuntime::GetPerformanceResult(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(RunId);
	if (!Job || !Job->IsValid() || (*Job)->Kind != TEXT("performance"))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Performance run '%s' was not found."), *RunId),
			TEXT("performance_run_not_found"),
			404);
	}
	TSharedPtr<FJsonObject> Data = MakeJobSummary(**Job, true);
	Data->SetStringField(TEXT("runId"), RunId);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::ComparePerformanceRuns(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString BaselineId =
		GetStringFieldOr(Params, TEXT("baselineRunId"));
	const FString CandidateId =
		GetStringFieldOr(Params, TEXT("candidateRunId"));
	const TSharedPtr<FJob>* Baseline = Jobs.Find(BaselineId);
	const TSharedPtr<FJob>* Candidate = Jobs.Find(CandidateId);
	if (!Baseline || !Baseline->IsValid()
		|| !Candidate || !Candidate->IsValid()
		|| (*Baseline)->Kind != TEXT("performance")
		|| (*Candidate)->Kind != TEXT("performance"))
	{
		return FMCPToolResult::Error(
			TEXT("Both performance run identifiers must exist."),
			TEXT("performance_run_not_found"),
			404);
	}
	if ((*Baseline)->Status != TEXT("succeeded")
		|| (*Candidate)->Status != TEXT("succeeded"))
	{
		return FMCPToolResult::Error(
			TEXT("Both performance runs must have succeeded."),
			TEXT("performance_run_not_ready"),
			409);
	}

	const TSharedPtr<FJsonObject> BaselineContext =
		(*Baseline)->Result->GetObjectField(TEXT("context"));
	const TSharedPtr<FJsonObject> CandidateContext =
		(*Candidate)->Result->GetObjectField(TEXT("context"));
	static const TArray<FString> ContextFields = {
		TEXT("project"),
		TEXT("map"),
		TEXT("rhi"),
		TEXT("gpu"),
		TEXT("resolution"),
		TEXT("configuration")
	};
	TArray<TSharedPtr<FJsonValue>> Mismatches;
	for (const FString& Field : ContextFields)
	{
		const FString Before = GetStringFieldOr(BaselineContext, *Field);
		const FString After = GetStringFieldOr(CandidateContext, *Field);
		if (Before != After)
		{
			TSharedPtr<FJsonObject> Mismatch = MakeShared<FJsonObject>();
			Mismatch->SetStringField(TEXT("field"), Field);
			Mismatch->SetStringField(TEXT("baseline"), Before);
			Mismatch->SetStringField(TEXT("candidate"), After);
			Mismatches.Add(MakeShared<FJsonValueObject>(Mismatch));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("baselineRunId"), BaselineId);
	Data->SetStringField(TEXT("candidateRunId"), CandidateId);
	Data->SetArrayField(TEXT("contextMismatches"), Mismatches);
	if (!Mismatches.IsEmpty())
	{
		Data->SetStringField(TEXT("verdict"), TEXT("inconclusive"));
		Data->SetStringField(
			TEXT("reason"),
			TEXT("Performance run contexts differ."));
		return FMCPToolResult::Ok(Data);
	}

	const FString Metric =
		GetStringFieldOr(Params, TEXT("metric"), TEXT("frameMs"));
	const double MaxRegressionPercent =
		GetNumberFieldOr(Params, TEXT("maxRegressionPercent"), 5.0);
	const double AbsoluteBudgetMs =
		GetNumberFieldOr(Params, TEXT("absoluteBudgetMs"), 0.0);
	const TSharedPtr<FJsonObject> BaselineMetric =
		(*Baseline)->Result->GetObjectField(TEXT("metrics"))
			->GetObjectField(Metric);
	const TSharedPtr<FJsonObject> CandidateMetric =
		(*Candidate)->Result->GetObjectField(TEXT("metrics"))
			->GetObjectField(Metric);
	if (!GetBoolFieldOr(BaselineMetric, TEXT("available"), false)
		|| !GetBoolFieldOr(CandidateMetric, TEXT("available"), false))
	{
		Data->SetStringField(TEXT("verdict"), TEXT("inconclusive"));
		Data->SetStringField(
			TEXT("reason"),
			TEXT("The requested metric is unavailable."));
		return FMCPToolResult::Ok(Data);
	}
	const double Before =
		GetNumberFieldOr(BaselineMetric, TEXT("p95"), 0.0);
	const double After =
		GetNumberFieldOr(CandidateMetric, TEXT("p95"), 0.0);
	const double RegressionPercent =
		Before > SMALL_NUMBER ? ((After - Before) / Before) * 100.0 : 0.0;
	const bool bRegression =
		RegressionPercent > MaxRegressionPercent
		|| (AbsoluteBudgetMs > 0.0 && After > AbsoluteBudgetMs);
	Data->SetStringField(TEXT("metric"), Metric);
	Data->SetNumberField(TEXT("baselineP95"), Before);
	Data->SetNumberField(TEXT("candidateP95"), After);
	Data->SetNumberField(TEXT("regressionPercent"), RegressionPercent);
	Data->SetNumberField(
		TEXT("maxRegressionPercent"),
		MaxRegressionPercent);
	if (AbsoluteBudgetMs > 0.0)
	{
		Data->SetNumberField(TEXT("absoluteBudgetMs"), AbsoluteBudgetMs);
	}
	Data->SetStringField(
		TEXT("verdict"),
		bRegression ? TEXT("fail") : TEXT("pass"));
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::ListTests(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString Runner =
		GetStringFieldOr(Params, TEXT("runner"), TEXT("automation"));
	const FString Filter =
		GetStringFieldOr(Params, TEXT("filter"));
	const int32 Offset = FMath::Max(
		0,
		static_cast<int32>(GetNumberFieldOr(Params, TEXT("offset"), 0.0)));
	const int32 Limit = FMath::Clamp(
		static_cast<int32>(GetNumberFieldOr(Params, TEXT("limit"), 50.0)),
		1,
		500);
	TArray<TSharedPtr<FJsonValue>> Items;
	int32 Total = 0;

	if (Runner == TEXT("automation") || Runner == TEXT("functional"))
	{
		TArray<FAutomationTestInfo> Tests;
		FAutomationTestFramework::Get().GetValidTestNames(Tests);
		Tests.Sort(
			[](const FAutomationTestInfo& Left, const FAutomationTestInfo& Right)
			{
				return Left.GetFullTestPath() < Right.GetFullTestPath();
			});
		for (const FAutomationTestInfo& Test : Tests)
		{
			const FString FullPath = Test.GetFullTestPath();
			const bool bFunctional =
				FullPath.Contains(TEXT("Functional"), ESearchCase::IgnoreCase);
			if ((Runner == TEXT("functional") && !bFunctional)
				|| (!Filter.IsEmpty()
					&& !FullPath.Contains(Filter, ESearchCase::IgnoreCase)))
			{
				continue;
			}
			if (Total >= Offset && Items.Num() < Limit)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("runner"), Runner);
				Item->SetStringField(TEXT("name"), Test.GetTestName());
				Item->SetStringField(TEXT("fullPath"), FullPath);
				Item->SetStringField(TEXT("displayName"), Test.GetDisplayName());
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			++Total;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("runner"), Runner);
	Data->SetNumberField(TEXT("total"), Total);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetBoolField(TEXT("truncated"), Offset + Items.Num() < Total);
	Data->SetArrayField(TEXT("tests"), Items);
	if (Runner == TEXT("gauntlet"))
	{
		Data->SetStringField(
			TEXT("availability"),
			TEXT("Gauntlet tests are project-defined and require an explicit test name."));
	}
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::StartTestRun(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString Runner =
		GetStringFieldOr(Params, TEXT("runner"), TEXT("automation"));
	const FString Test =
		GetStringFieldOr(Params, TEXT("test"));
	if (Test.IsEmpty()
		|| !IsSafeLegacyArgumentString(Test))
	{
		return FMCPToolResult::Error(
			TEXT("test must be a bounded test path without command separators."),
			TEXT("invalid_params"),
			422);
	}
	const double TimeoutSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 1800.0),
		1.0,
		21600.0);
	FString Executable;
	FString Arguments;
	const FString ProjectFile =
		FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());

	if (Runner == TEXT("gauntlet"))
	{
		Arguments = FString::Printf(
			TEXT("RunUnreal -project=%s -test=%s -platform=%s -configuration=%s -build=editor -unattended"),
			*QuoteArgument(ProjectFile),
			*QuoteArgument(Test),
			*GetStringFieldOr(Params, TEXT("platform"), TEXT("Win64")),
			*GetStringFieldOr(Params, TEXT("config"), TEXT("Development")));
		Executable = MakeUatExecutable(Arguments);
	}
	else
	{
		Executable = GetEditorCommandletExecutable();
		if (!IFileManager::Get().FileExists(*Executable))
		{
			return FMCPToolResult::Error(
				TEXT("UnrealEditor-Cmd is unavailable."),
				TEXT("test_unavailable"),
				503);
		}
		const FString ReportDirectory =
			FPaths::Combine(
				JobsRoot(),
				NewOpaqueId(TEXT("test-report")));
		IFileManager::Get().MakeDirectory(*ReportDirectory, true);
		Arguments = FString::Printf(
			TEXT("%s -unattended -nop4 -nosplash -NullRHI -ExecCmds=%s -TestExit=%s -ReportExportPath=%s -log"),
			*QuoteArgument(ProjectFile),
			*QuoteArgument(
				FString::Printf(
					TEXT("Automation RunTests %s;Quit"),
					*Test)),
			*QuoteArgument(TEXT("Automation Test Queue Empty")),
			*QuoteArgument(ReportDirectory));

		FString Error;
		TSharedPtr<FJob> Job = StartProcessJob(
			TEXT("test"),
			Executable,
			Arguments,
			FPaths::ProjectDir(),
			TimeoutSeconds,
			TEXT("test"),
			Params,
			Error);
		if (!Job.IsValid())
		{
			return MakeJobStartFailure(Error);
		}
		// The actual job id differs from the reservation; rewrite the report path
		// before launch is not possible, so retain it as an explicit input field.
		if (!Job->Input->HasTypedField<EJson::String>(
				TEXT("reportDirectory")))
		{
			Job->Input->SetStringField(
				TEXT("reportDirectory"),
				ReportDirectory);
			SaveJournal(*Job);
		}
		return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
	}

	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("test"),
		Executable,
		Arguments,
		FPaths::ProjectDir(),
		TimeoutSeconds,
		TEXT("test"),
		Params,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::GetTestResult(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(RunId);
	if (!Job || !Job->IsValid() || (*Job)->Kind != TEXT("test"))
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Test run '%s' was not found."), *RunId),
			TEXT("test_run_not_found"),
			404);
	}
	TSharedPtr<FJsonObject> Data = MakeJobSummary(**Job, true);
	Data->SetStringField(TEXT("runId"), RunId);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::StartCook(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString Platform =
		GetStringFieldOr(Params, TEXT("platform"));
	const FString Config =
		GetStringFieldOr(Params, TEXT("config"), TEXT("Development"));
	const FString CookMode =
		GetStringFieldOr(Params, TEXT("cookMode"), TEXT("byTheBook"));
	FString UatArguments = FString::Printf(
		TEXT("BuildCookRun -project=%s -targetplatform=%s -clientconfig=%s -cook -NoP4 -UTF8Output"),
		*QuoteArgument(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())),
		*Platform,
		*Config);
	if (CookMode == TEXT("iterate"))
	{
		UatArguments += TEXT(" -iterate");
	}
	const TArray<FString> Maps = ReadStringArray(Params, TEXT("maps"));
	if (Maps.IsEmpty())
	{
		UatArguments += TEXT(" -allmaps");
	}
	else
	{
		for (const FString& Map : Maps)
		{
			if (!Map.StartsWith(TEXT("/Game/"))
				|| !IsSafeLegacyArgumentString(Map))
			{
				return FMCPToolResult::Error(
					TEXT("maps must contain bounded /Game asset paths."),
					TEXT("invalid_params"),
					422);
			}
		}
		UatArguments += TEXT(" -map=")
			+ QuoteArgument(FString::Join(Maps, TEXT("+")));
	}
	FString Executable = MakeUatExecutable(UatArguments);
	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("cook"),
		Executable,
		UatArguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 7200.0),
		TEXT("cook"),
		Params,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::StartPackage(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString Platform =
		GetStringFieldOr(Params, TEXT("platform"));
	const FString Config =
		GetStringFieldOr(Params, TEXT("config"), TEXT("Shipping"));
	const FString OutputInput =
		GetStringFieldOr(Params, TEXT("output_dir"));
	if (OutputInput.IsEmpty()
		|| !IsSafeLegacyArgumentString(OutputInput))
	{
		return FMCPToolResult::Error(
			TEXT("output_dir is required and may not contain command separators."),
			TEXT("invalid_params"),
			422);
	}
	const FString AllowedOutputRoot = PackageOutputRoot();
	const FString OutputDirectory =
		FPaths::ConvertRelativePathToFull(
			FPaths::IsRelative(OutputInput)
				? FPaths::Combine(AllowedOutputRoot, OutputInput)
				: OutputInput);
	if (!IsPathWithin(OutputDirectory, AllowedOutputRoot))
	{
		return FMCPToolResult::Error(
			TEXT(
				"output_dir must resolve inside "
				"Saved/UEAIIntegration/Packages."),
			TEXT("output_path_not_permitted"),
			403);
	}
	IFileManager::Get().MakeDirectory(*AllowedOutputRoot, true);
	FString UatArguments = FString::Printf(
		TEXT("BuildCookRun -project=%s -targetplatform=%s -clientconfig=%s -build -cook -stage -package -archive -archivedirectory=%s -allmaps -NoP4 -UTF8Output"),
		*QuoteArgument(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())),
		*Platform,
		*Config,
		*QuoteArgument(OutputDirectory));
	if (GetBoolFieldOr(Params, TEXT("pak"), true))
	{
		UatArguments += TEXT(" -pak");
	}
	if (GetBoolFieldOr(Params, TEXT("ioStore"), true))
	{
		UatArguments += TEXT(" -iostore");
	}
	FString Executable = MakeUatExecutable(UatArguments);
	TSharedPtr<FJsonObject> Input = CopyObject(Params);
	Input->SetStringField(TEXT("resolvedOutputDirectory"), OutputDirectory);
	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("package"),
		Executable,
		UatArguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 14400.0),
		TEXT("package"),
		Input,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::StartCommandlet(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bConfirmWrite = false;
	const bool bHasConfirmWrite =
		Params.IsValid()
		&& Params->TryGetBoolField(
			TEXT("confirmWrite"),
			bConfirmWrite);
	const bool bLegacyCompatibility =
		!bHasConfirmWrite
		&& GetStringFieldOr(Params, TEXT("requestId")).IsEmpty();
	if ((!bHasConfirmWrite || !bConfirmWrite)
		&& !bLegacyCompatibility)
	{
		return FMCPToolResult::Error(
			TEXT("confirmWrite:true is required."),
			TEXT("confirmation_required"),
			422);
	}
	const FString Commandlet =
		GetStringFieldOr(Params, TEXT("commandlet_name"));
	if (!IsSafeToken(Commandlet, 128))
	{
		return FMCPToolResult::Error(
			TEXT("commandlet_name must be an identifier token."),
			TEXT("invalid_params"),
			422);
	}
	static const TSet<FString> AllowedCommandlets = {
		TEXT("FixupRedirects"),
		TEXT("ResavePackages"),
		TEXT("WorldPartitionBuilderCommandlet")
	};
	if (!AllowedCommandlets.Contains(Commandlet))
	{
		return FMCPToolResult::Error(
			TEXT(
				"The requested commandlet is not in the constrained "
				"allowlist."),
			TEXT("operation_not_permitted"),
			403);
	}
	FString Arguments = FString::Printf(
		TEXT("%s -run=%s -NoP4 -UTF8Output -unattended"),
		*QuoteArgument(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())),
		*Commandlet);
	if (Params->HasTypedField<EJson::Array>(TEXT("arguments")))
	{
		for (const FString& Argument : ReadStringArray(Params, TEXT("arguments")))
		{
			if (!IsSafeLegacyArgumentString(Argument))
			{
				return FMCPToolResult::Error(
					TEXT("A commandlet argument contains a command separator."),
					TEXT("invalid_params"),
					422);
			}
			Arguments += TEXT(" ") + QuoteArgument(Argument);
		}
	}
	else
	{
		const FString LegacyArgs =
			GetStringFieldOr(Params, TEXT("args"));
		if (!IsSafeLegacyArgumentString(LegacyArgs))
		{
			return FMCPToolResult::Error(
				TEXT("Legacy args contains a command separator; use structured arguments."),
				TEXT("invalid_params"),
				422);
		}
		if (!LegacyArgs.IsEmpty())
		{
			Arguments += TEXT(" ") + LegacyArgs;
		}
	}
	FString Error;
	TSharedPtr<FJsonObject> Input = CopyObject(Params);
	if (bLegacyCompatibility)
	{
		Input->SetBoolField(TEXT("legacyCompatibility"), true);
	}
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("commandlet"),
		GetEditorCommandletExecutable(),
		Arguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 3600.0),
		TEXT("commandlet"),
		Input,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::GetSourceControlRepository(
	const TSharedPtr<FJsonObject>& Params) const
{
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = INDEX_NONE;
	if (!RunGitSync(
			TEXT("rev-parse --show-toplevel"),
			StdOut,
			StdErr,
			ReturnCode)
		|| ReturnCode != 0)
	{
		return FMCPToolResult::Error(
			StdErr.IsEmpty()
				? TEXT("The current project is not in a Git repository.")
				: StdErr,
			TEXT("source_control_unavailable"),
			503);
	}
	const FString Root = StdOut.TrimStartAndEnd();
	FString Branch;
	FString BranchError;
	int32 BranchCode = INDEX_NONE;
	RunGitSync(
		TEXT("branch --show-current"),
		Branch,
		BranchError,
		BranchCode);
	FString Head;
	FString HeadError;
	int32 HeadCode = INDEX_NONE;
	RunGitSync(
		TEXT("rev-parse HEAD"),
		Head,
		HeadError,
		HeadCode);
	FString Remote;
	FString RemoteError;
	int32 RemoteCode = INDEX_NONE;
	RunGitSync(
		TEXT("remote"),
		Remote,
		RemoteError,
		RemoteCode);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("provider"), TEXT("git"));
	Data->SetStringField(TEXT("root"), Root);
	Data->SetStringField(TEXT("branch"), Branch.TrimStartAndEnd());
	Data->SetStringField(TEXT("head"), Head.TrimStartAndEnd());
	TArray<TSharedPtr<FJsonValue>> Remotes;
	TArray<FString> RemoteLines;
	Remote.ParseIntoArrayLines(RemoteLines, true);
	for (const FString& Value : RemoteLines)
	{
		Remotes.Add(MakeShared<FJsonValueString>(Value));
	}
	Data->SetArrayField(TEXT("remotes"), Remotes);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::GetSourceControlStatus(
	const TSharedPtr<FJsonObject>& Params) const
{
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = INDEX_NONE;
	if (!RunGitSync(
			TEXT("status --porcelain=v2 --branch"),
			StdOut,
			StdErr,
			ReturnCode)
		|| ReturnCode != 0)
	{
		return FMCPToolResult::Error(
			StdErr,
			TEXT("source_control_unavailable"),
			503);
	}
	const int32 Limit = FMath::Clamp(
		static_cast<int32>(GetNumberFieldOr(Params, TEXT("limit"), 200.0)),
		1,
		1000);
	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines, true);
	TArray<TSharedPtr<FJsonValue>> Entries;
	FString Branch;
	int32 ChangeCount = 0;
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("# branch.head ")))
		{
			Branch = Line.RightChop(14);
			continue;
		}
		if (Line.StartsWith(TEXT("#")))
		{
			continue;
		}
		++ChangeCount;
		if (Entries.Num() < Limit)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("porcelain"), Line);
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("provider"), TEXT("git"));
	Data->SetStringField(TEXT("branch"), Branch);
	Data->SetNumberField(TEXT("changeCount"), ChangeCount);
	Data->SetBoolField(TEXT("clean"), ChangeCount == 0);
	Data->SetBoolField(TEXT("truncated"), Entries.Num() < ChangeCount);
	Data->SetArrayField(TEXT("changes"), Entries);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::GetSourceControlDiff(
	const TSharedPtr<FJsonObject>& Params) const
{
	const bool bStaged = GetBoolFieldOr(Params, TEXT("staged"), false);
	const int32 ContextLines = FMath::Clamp(
		static_cast<int32>(
			GetNumberFieldOr(Params, TEXT("contextLines"), 3.0)),
		0,
		20);
	FString Arguments = FString::Printf(
		TEXT("diff --no-ext-diff --unified=%d"),
		ContextLines);
	if (bStaged)
	{
		Arguments += TEXT(" --cached");
	}
	const TArray<FString> Files = ReadStringArray(Params, TEXT("files"));
	if (!Files.IsEmpty())
	{
		Arguments += TEXT(" --");
		for (const FString& File : Files)
		{
			FString Relative;
			if (!IsSafeRelativeGitPath(File, Relative))
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Unsafe Git path '%s'."), *File),
					TEXT("invalid_params"),
					422);
			}
			Arguments += TEXT(" ") + QuoteArgument(Relative);
		}
	}
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = INDEX_NONE;
	if (!RunGitSync(Arguments, StdOut, StdErr, ReturnCode)
		|| ReturnCode != 0)
	{
		return FMCPToolResult::Error(
			StdErr,
			TEXT("source_control_failed"),
			500);
	}
	const int32 MaxChars = FMath::Clamp(
		static_cast<int32>(
			GetNumberFieldOr(Params, TEXT("maxChars"), 65536.0)),
		1,
		262144);
	const bool bTruncated = StdOut.Len() > MaxChars;
	if (bTruncated)
	{
		StdOut.LeftInline(MaxChars, false);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("provider"), TEXT("git"));
	Data->SetStringField(TEXT("diff"), StdOut);
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::PlanSourceControlChange(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString Action = GetStringFieldOr(Params, TEXT("action"));
	static const TSet<FString> AllowedActions = {
		TEXT("stage"),
		TEXT("unstage"),
		TEXT("restore"),
		TEXT("createBranch"),
		TEXT("switch"),
		TEXT("commit"),
		TEXT("fetch"),
		TEXT("pull"),
		TEXT("push")
	};
	if (!AllowedActions.Contains(Action))
	{
		return FMCPToolResult::Error(
			TEXT("Unsupported source control action."),
			TEXT("invalid_params"),
			422);
	}
	TSharedPtr<FJsonObject> Normalized = CopyObject(Params);
	for (const FString& File : ReadStringArray(Params, TEXT("files")))
	{
		FString Relative;
		if (!IsSafeRelativeGitPath(File, Relative))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Unsafe Git path '%s'."), *File),
				TEXT("invalid_params"),
				422);
		}
	}
	const FString Digest = ComputeChangePlanDigest(Normalized);
	if (Digest.IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("The source control plan digest could not be generated."),
			TEXT("planning_failed"),
			500);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("normalizedRequest"), Normalized);
	Data->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Data->SetStringField(TEXT("domain"), TEXT("sourceControl"));
	Data->SetStringField(TEXT("action"), Action);
	Data->SetStringField(
		TEXT("persistence"),
		Action == TEXT("push") ? TEXT("external") : TEXT("saveOnSuccess"));
	Data->SetStringField(
		TEXT("rollbackBoundary"),
		TEXT("Git operations are not covered by an Unreal Transaction; recovery uses subsequent explicit Git operations."));
	Data->SetBoolField(TEXT("changesState"), true);
	Data->SetStringField(TEXT("planDigest"), Digest);
	Data->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	Data->SetBoolField(
		TEXT("externalSideEffect"),
		Action == TEXT("pull") || Action == TEXT("push"));
	Data->SetArrayField(
		TEXT("affectedFiles"),
		Params->HasTypedField<EJson::Array>(TEXT("files"))
			? Params->GetArrayField(TEXT("files"))
			: TArray<TSharedPtr<FJsonValue>>());
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::ExecuteSourceControlChange(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GetBoolFieldOr(Params, TEXT("confirmWrite"), false)
		|| !Params->HasTypedField<EJson::Object>(TEXT("request")))
	{
		return FMCPToolResult::Error(
			TEXT("request and confirmWrite:true are required."),
			TEXT("confirmation_required"),
			422);
	}
	const TSharedPtr<FJsonObject> Request =
		Params->GetObjectField(TEXT("request"));
	const FString Approved =
		GetStringFieldOr(Params, TEXT("approvePlanDigest"));
	const FString Actual = ComputeChangePlanDigest(Request);
	if (Approved.IsEmpty() || Approved != Actual)
	{
		return FMCPToolResult::Error(
			TEXT("approvePlanDigest does not match the normalized request."),
			TEXT("plan_digest_mismatch"),
			409);
	}
	const FString Action = GetStringFieldOr(Request, TEXT("action"));
	if ((Action == TEXT("pull") || Action == TEXT("push"))
		&& !GetBoolFieldOr(Params, TEXT("confirmExternal"), false))
	{
		return FMCPToolResult::Error(
			TEXT("confirmExternal:true is required for pull and push."),
			TEXT("confirmation_required"),
			422);
	}

	FString GitArguments;
	const TArray<FString> Files = ReadStringArray(Request, TEXT("files"));
	auto AppendFiles = [&Files, &GitArguments]() -> bool
	{
		for (const FString& File : Files)
		{
			FString Relative;
			if (!IsSafeRelativeGitPath(File, Relative))
			{
				return false;
			}
			GitArguments += TEXT(" ") + FProductionJobRuntime::QuoteArgument(Relative);
		}
		return true;
	};
	if (Action == TEXT("stage"))
	{
		GitArguments = TEXT("add --");
		if (Files.IsEmpty() || !AppendFiles())
		{
			return FMCPToolResult::Error(
				TEXT("stage requires safe explicit files."),
				TEXT("invalid_params"),
				422);
		}
	}
	else if (Action == TEXT("unstage"))
	{
		GitArguments = TEXT("restore --staged --");
		if (Files.IsEmpty() || !AppendFiles())
		{
			return FMCPToolResult::Error(
				TEXT("unstage requires safe explicit files."),
				TEXT("invalid_params"),
				422);
		}
	}
	else if (Action == TEXT("restore"))
	{
		GitArguments = TEXT("restore --");
		if (Files.IsEmpty() || !AppendFiles())
		{
			return FMCPToolResult::Error(
				TEXT("restore requires safe explicit files."),
				TEXT("invalid_params"),
				422);
		}
	}
	else if (Action == TEXT("createBranch"))
	{
		const FString Branch = GetStringFieldOr(Request, TEXT("branch"));
		if (!IsSafeToken(Branch, 128))
		{
			return FMCPToolResult::Error(TEXT("Invalid branch."), TEXT("invalid_params"), 422);
		}
		GitArguments = TEXT("switch -c ") + QuoteArgument(Branch);
	}
	else if (Action == TEXT("switch"))
	{
		const FString Branch = GetStringFieldOr(Request, TEXT("branch"));
		if (!IsSafeToken(Branch, 128))
		{
			return FMCPToolResult::Error(TEXT("Invalid branch."), TEXT("invalid_params"), 422);
		}
		GitArguments = TEXT("switch ") + QuoteArgument(Branch);
	}
	else if (Action == TEXT("commit"))
	{
		const FString Message = GetStringFieldOr(Request, TEXT("message"));
		if (Message.IsEmpty()
			|| Message.Len() > 1000
			|| Message.Contains(TEXT("\""))
			|| Message.Contains(TEXT("\r"))
			|| Message.Contains(TEXT("\n")))
		{
			return FMCPToolResult::Error(TEXT("Invalid commit message."), TEXT("invalid_params"), 422);
		}
		GitArguments = TEXT("commit -m ") + QuoteArgument(Message);
	}
	else if (Action == TEXT("fetch"))
	{
		const FString Remote =
			GetStringFieldOr(Request, TEXT("remote"), TEXT("origin"));
		if (!IsSafeToken(Remote, 128))
		{
			return FMCPToolResult::Error(TEXT("Invalid remote."), TEXT("invalid_params"), 422);
		}
		GitArguments = TEXT("fetch ") + QuoteArgument(Remote);
	}
	else if (Action == TEXT("pull"))
	{
		const FString Remote =
			GetStringFieldOr(Request, TEXT("remote"), TEXT("origin"));
		if (!IsSafeToken(Remote, 128))
		{
			return FMCPToolResult::Error(TEXT("Invalid remote."), TEXT("invalid_params"), 422);
		}
		GitArguments = TEXT("pull --ff-only ") + QuoteArgument(Remote);
		const FString Branch = GetStringFieldOr(Request, TEXT("branch"));
		if (!Branch.IsEmpty())
		{
			if (!IsSafeToken(Branch, 128))
			{
				return FMCPToolResult::Error(TEXT("Invalid branch."), TEXT("invalid_params"), 422);
			}
			GitArguments += TEXT(" ") + QuoteArgument(Branch);
		}
	}
	else if (Action == TEXT("push"))
	{
		const FString Remote =
			GetStringFieldOr(Request, TEXT("remote"), TEXT("origin"));
		const FString Branch = GetStringFieldOr(Request, TEXT("branch"));
		if (!IsSafeToken(Remote, 128) || !IsSafeToken(Branch, 128))
		{
			return FMCPToolResult::Error(TEXT("Push requires a safe remote and branch."), TEXT("invalid_params"), 422);
		}
		GitArguments =
			TEXT("push ")
			+ QuoteArgument(Remote)
			+ TEXT(" ")
			+ QuoteArgument(Branch);
	}
	else
	{
		return FMCPToolResult::Error(
			TEXT("Unsupported source control action."),
			TEXT("invalid_params"),
			422);
	}

	FString Error;
	TSharedPtr<FJsonObject> JobInput = CopyObject(Request);
	const FString RequestId = GetStringFieldOr(Params, TEXT("requestId"));
	if (!RequestId.IsEmpty())
	{
		JobInput->SetStringField(TEXT("requestId"), RequestId);
	}
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("sourceControl"),
		TEXT("git.exe"),
		TEXT("-C ")
			+ QuoteArgument(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()))
			+ TEXT(" ")
			+ GitArguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 600.0),
		TEXT("sourceControl"),
		JobInput,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	Job->Result->SetStringField(TEXT("planDigest"), Actual);
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::GetDdcStatus(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString ProjectLocal =
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("DerivedDataCache")));
	const int64 Size = DirectorySize(ProjectLocal);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("projectLocalPath"), ProjectLocal);
	Data->SetBoolField(
		TEXT("projectLocalExists"),
		IFileManager::Get().DirectoryExists(*ProjectLocal));
	Data->SetNumberField(TEXT("projectLocalBytes"), static_cast<double>(Size));
	Data->SetBoolField(TEXT("sharedDeleteSupported"), false);
	Data->SetStringField(
		TEXT("policy"),
		TEXT("Only the project-local Saved/DerivedDataCache directory can be cleaned."));
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::StartDdcJob(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString Action = GetStringFieldOr(Params, TEXT("action"));
	if (!GetBoolFieldOr(Params, TEXT("confirmWrite"), false))
	{
		return FMCPToolResult::Error(
			TEXT("confirmWrite:true is required."),
			TEXT("confirmation_required"),
			422);
	}
	if (Action == TEXT("cleanProjectLocal"))
	{
		if (!GetBoolFieldOr(Params, TEXT("confirmDestructive"), false))
		{
			return FMCPToolResult::Error(
				TEXT("confirmDestructive:true is required to clean project-local DDC."),
				TEXT("confirmation_required"),
				422);
		}
		const FString ProjectLocal =
			FPaths::ConvertRelativePathToFull(
				FPaths::Combine(
					FPaths::ProjectSavedDir(),
					TEXT("DerivedDataCache")));
		const FString SavedRoot =
			FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		if (!IsPathWithin(ProjectLocal, SavedRoot)
			|| FPaths::IsSamePath(ProjectLocal, SavedRoot))
		{
			return FMCPToolResult::Error(
				TEXT("Resolved DDC path failed the project-local safety boundary."),
				TEXT("operation_not_permitted"),
				403);
		}
		bool bIdempotencyConflict = false;
		if (TSharedPtr<FJob> Existing =
			FindIdempotentJob(TEXT("ddc"), Params, bIdempotencyConflict))
		{
			TSharedPtr<FJsonObject> Replay = MakeJobSummary(*Existing, true);
			Replay->SetBoolField(TEXT("idempotentReplay"), true);
			return FMCPToolResult::Ok(Replay);
		}
		if (bIdempotencyConflict)
		{
			return FMCPToolResult::Error(
				TEXT("requestId is already bound to a different durable job request."),
				TEXT("idempotency_conflict"),
				409);
		}
		TSharedPtr<FJob> Job = CreateJob(TEXT("ddc"), Params);
		Job->Status = TEXT("running");
		Job->Phase = TEXT("cleaning");
		Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
		const int64 BeforeBytes = DirectorySize(ProjectLocal);
		const bool bDeleted =
			!IFileManager::Get().DirectoryExists(*ProjectLocal)
			|| IFileManager::Get().DeleteDirectory(
				*ProjectLocal,
				false,
				true);
		Job->Result->SetStringField(TEXT("action"), Action);
		Job->Result->SetStringField(TEXT("path"), ProjectLocal);
		Job->Result->SetNumberField(
			TEXT("deletedBytes"),
			static_cast<double>(BeforeBytes));
		FinishJob(
			*Job,
			bDeleted ? TEXT("succeeded") : TEXT("failed"),
			bDeleted ? FString() : TEXT("ddc_clean_failed"),
			bDeleted
				? FString()
				: TEXT("Project-local DDC could not be deleted."));
		return FMCPToolResult::Ok(MakeJobSummary(*Job, true));
	}
	if (Action != TEXT("fill") && Action != TEXT("verify"))
	{
		return FMCPToolResult::Error(
			TEXT("action must be fill, verify, or cleanProjectLocal."),
			TEXT("invalid_params"),
			422);
	}

	FString Arguments = FString::Printf(
		TEXT("%s -run=DerivedDataCache -%s -unattended -nop4 -UTF8Output"),
		*QuoteArgument(FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath())),
		Action == TEXT("fill") ? TEXT("fill") : TEXT("verify"));
	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("ddc"),
		GetEditorCommandletExecutable(),
		Arguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 7200.0),
		TEXT("ddc"),
		Params,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::ValidateBuildGraph(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString ScriptInput =
		GetStringFieldOr(Params, TEXT("script"));
	const FString Target =
		GetStringFieldOr(Params, TEXT("target"));
	FString Script = FPaths::ConvertRelativePathToFull(
		FPaths::IsRelative(ScriptInput)
			? FPaths::Combine(FPaths::ProjectDir(), ScriptInput)
			: ScriptInput);
	const FString EngineBuild =
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::EngineDir(), TEXT("Build")));
	const FString ProjectBuild =
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Build")));
	TArray<TSharedPtr<FJsonValue>> Errors;
	if (!IFileManager::Get().FileExists(*Script))
	{
		Errors.Add(MakeShared<FJsonValueString>(
			TEXT("script does not exist.")));
	}
	if (!IsPathWithin(Script, EngineBuild)
		&& !IsPathWithin(Script, ProjectBuild))
	{
		Errors.Add(MakeShared<FJsonValueString>(
			TEXT("script must be under Engine/Build or Project/Build.")));
	}
	if (!Script.EndsWith(TEXT(".xml"), ESearchCase::IgnoreCase))
	{
		Errors.Add(MakeShared<FJsonValueString>(
			TEXT("script must be a BuildGraph XML file.")));
	}
	if (Target.IsEmpty() || !IsSafeLegacyArgumentString(Target))
	{
		Errors.Add(MakeShared<FJsonValueString>(
			TEXT("target must be a bounded identifier token.")));
	}
	if (Params->HasTypedField<EJson::Object>(TEXT("properties")))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
			Params->GetObjectField(TEXT("properties"))->Values)
		{
			if (!IsSafeToken(Pair.Key, 128)
				|| !Pair.Value.IsValid()
				|| (Pair.Value->Type != EJson::String
					&& Pair.Value->Type != EJson::Number
					&& Pair.Value->Type != EJson::Boolean)
				|| !IsSafeLegacyArgumentString(Pair.Value->AsString()))
			{
				Errors.Add(MakeShared<FJsonValueString>(
					FString::Printf(
						TEXT("BuildGraph property '%s' is not a safe scalar."),
						*Pair.Key)));
			}
		}
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("valid"), Errors.IsEmpty());
	Data->SetStringField(TEXT("resolvedScript"), Script);
	Data->SetStringField(TEXT("target"), Target);
	Data->SetArrayField(TEXT("errors"), Errors);
	TSharedPtr<FJsonObject> Horde = MakeShared<FJsonObject>();
	Horde->SetBoolField(TEXT("submissionSupported"), false);
	Horde->SetBoolField(TEXT("exportSupported"), false);
	Horde->SetStringField(
		TEXT("reason"),
		TEXT("This version validates and runs BuildGraph locally; it does not submit Horde jobs."));
	Data->SetObjectField(TEXT("horde"), Horde);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionJobRuntime::StartBuildGraph(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!GetBoolFieldOr(Params, TEXT("confirmBuild"), false))
	{
		return FMCPToolResult::Error(
			TEXT("confirmBuild:true is required."),
			TEXT("confirmation_required"),
			422);
	}
	const FMCPToolResult Validation = ValidateBuildGraph(Params);
	if (!Validation.bSuccess
		|| !Validation.Data->GetBoolField(TEXT("valid")))
	{
		return FMCPToolResult::Error(
			TEXT("BuildGraph request failed validation."),
			TEXT("invalid_params"),
			422);
	}
	const FString Script =
		Validation.Data->GetStringField(TEXT("resolvedScript"));
	const FString Target =
		Validation.Data->GetStringField(TEXT("target"));
	FString UatArguments = FString::Printf(
		TEXT("BuildGraph -Script=%s -Target=%s"),
		*QuoteArgument(Script),
		*QuoteArgument(Target));
	if (Params->HasTypedField<EJson::Object>(TEXT("properties")))
	{
		TArray<FString> Keys;
		const TSharedPtr<FJsonObject> Properties =
			Params->GetObjectField(TEXT("properties"));
		Properties->Values.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			const TSharedPtr<FJsonValue> Value =
				Properties->Values.FindRef(Key);
			FString TextValue;
			if (Value->Type == EJson::String)
			{
				TextValue = Value->AsString();
			}
			else if (Value->Type == EJson::Boolean)
			{
				TextValue = Value->AsBool() ? TEXT("true") : TEXT("false");
			}
			else
			{
				TextValue = FString::Printf(TEXT("%.17g"), Value->AsNumber());
			}
			UatArguments += FString::Printf(
				TEXT(" -set:%s=%s"),
				*Key,
				*QuoteArgument(TextValue));
		}
	}
	FString Executable = MakeUatExecutable(UatArguments);
	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("buildGraph"),
		Executable,
		UatArguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 14400.0),
		TEXT("buildGraph"),
		Params,
		Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
}

FMCPToolResult FProductionJobRuntime::GetHordeContext(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString Server =
		FPlatformMisc::GetEnvironmentVariable(TEXT("UE_HORDE_SERVER"));
	const FString Project =
		FPlatformMisc::GetEnvironmentVariable(TEXT("UE_HORDE_PROJECT"));
	const FString AgentType =
		FPlatformMisc::GetEnvironmentVariable(TEXT("UE_HORDE_AGENT_TYPE"));
	const FString HordeSource =
		FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::EngineDir(),
				TEXT("Source/Programs/Horde")));
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(
		TEXT("configured"),
		!Server.IsEmpty() && !Project.IsEmpty());
	Data->SetStringField(TEXT("server"), Server);
	Data->SetStringField(TEXT("project"), Project);
	Data->SetStringField(TEXT("agentType"), AgentType);
	Data->SetBoolField(
		TEXT("engineHordeSourceAvailable"),
		IFileManager::Get().DirectoryExists(*HordeSource));
	Data->SetBoolField(TEXT("credentialsExposed"), false);
	Data->SetBoolField(TEXT("submissionSupported"), false);
	Data->SetStringField(
		TEXT("reason"),
		TEXT("Horde context discovery is read-only; job submission is not implemented."));
	return FMCPToolResult::Ok(Data);
}

void FProductionJobRuntime::PostProcessJob(FJob& Job)
{
	const FString JobLogFile =
		FPaths::Combine(JobDirectory(Job.Id), TEXT("job.log"));
	if (!Job.Output.IsEmpty()
		&& !IFileManager::Get().FileExists(*JobLogFile))
	{
		FFileHelper::SaveStringToFile(Job.Output, *JobLogFile);
	}
	if (IFileManager::Get().FileExists(*JobLogFile))
	{
		AddArtifact(
			Job,
			JobLogFile,
			TEXT("job.log"),
			TEXT("text/plain"));
	}
	if (Job.PostProcess == TEXT("test"))
	{
		WriteTestReports(Job);
	}
	else if (Job.PostProcess == TEXT("cook")
		&& Job.Status == TEXT("succeeded"))
	{
		const FString Platform =
			GetStringFieldOr(Job.Input, TEXT("platform"));
		const FString RegistryPath =
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Cooked"),
				Platform,
				FApp::GetProjectName(),
				TEXT("Metadata/DevelopmentAssetRegistry.bin"));
		const FString LegacyRegistryPath =
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Cooked"),
				Platform,
				FApp::GetProjectName(),
				TEXT("AssetRegistry.bin"));
		const FString FoundPath =
			IFileManager::Get().FileExists(*RegistryPath)
				? RegistryPath
				: LegacyRegistryPath;
		const bool bRegistryFound =
			IFileManager::Get().FileExists(*FoundPath);
		Job.Result->SetBoolField(
			TEXT("cookedAssetRegistryFound"),
			bRegistryFound);
		if (bRegistryFound)
		{
			AddArtifact(
				Job,
				FoundPath,
				TEXT("CookedAssetRegistry.bin"),
				TEXT("application/octet-stream"));
		}
	}
	else if (Job.PostProcess == TEXT("package")
		&& Job.Status == TEXT("succeeded"))
	{
		const FString OutputDirectory =
			GetStringFieldOr(
				Job.Input,
				TEXT("resolvedOutputDirectory"));
		FBoundedPackageVisitor Visitor;
		if (IsPathWithin(OutputDirectory, PackageOutputRoot())
			&& IFileManager::Get().DirectoryExists(*OutputDirectory))
		{
			FPlatformFileManager::Get()
				.GetPlatformFile()
				.IterateDirectoryRecursively(
					*OutputDirectory,
					Visitor);
		}
		Visitor.CandidateFiles.Sort();
		for (const FString& Candidate : Visitor.CandidateFiles)
		{
			AddArtifact(
				Job,
				Candidate,
				FPaths::GetCleanFilename(Candidate),
				TEXT("application/octet-stream"));
		}
		Job.Result->SetStringField(
			TEXT("outputDirectory"),
			OutputDirectory);
		Job.Result->SetNumberField(
			TEXT("fileCount"),
			Visitor.FileCount);
		Job.Result->SetNumberField(
			TEXT("totalBytes"),
			static_cast<double>(Visitor.TotalBytes));
		Job.Result->SetBoolField(
			TEXT("scanTruncated"),
			Visitor.bScanTruncated);
		Job.Result->SetBoolField(
			TEXT("artifactsTruncated"),
			Visitor.bScanTruncated
				|| Visitor.EligibleArtifactCount
					> Visitor.CandidateFiles.Num());
	}
}

void FProductionJobRuntime::WriteTestReports(FJob& Job)
{
	const FString ReportDirectory =
		GetStringFieldOr(
			Job.Input,
			TEXT("reportDirectory"),
			FPaths::Combine(JobDirectory(Job.Id), TEXT("report")));
	const FString NativeIndex =
		FPaths::Combine(ReportDirectory, TEXT("index.json"));
	if (IFileManager::Get().FileExists(*NativeIndex))
	{
		AddArtifact(
			Job,
			NativeIndex,
			TEXT("automation-index.json"),
			TEXT("application/json"));
	}
	TArray<FString> ScreenshotFiles;
	if (IFileManager::Get().DirectoryExists(*ReportDirectory))
	{
		IFileManager::Get().FindFilesRecursive(
			ScreenshotFiles,
			*ReportDirectory,
			TEXT("*.png"),
			true,
			false);
		TArray<FString> JpegFiles;
		IFileManager::Get().FindFilesRecursive(
			JpegFiles,
			*ReportDirectory,
			TEXT("*.jpg"),
			true,
			false);
		ScreenshotFiles.Append(JpegFiles);
		ScreenshotFiles.Sort();
	}
	for (int32 Index = 0;
		Index < FMath::Min(ScreenshotFiles.Num(), 64);
		++Index)
	{
		AddArtifact(
			Job,
			ScreenshotFiles[Index],
			FPaths::GetCleanFilename(ScreenshotFiles[Index]),
			ScreenshotFiles[Index].EndsWith(TEXT(".png"))
				? TEXT("image/png")
				: TEXT("image/jpeg"));
	}

	const FString Runner =
		GetStringFieldOr(Job.Input, TEXT("runner"), TEXT("automation"));
	const FString TestName =
		GetStringFieldOr(Job.Input, TEXT("test"));
	int32 Passed = Job.Status == TEXT("succeeded") ? 1 : 0;
	int32 Failed = Job.Status == TEXT("succeeded") ? 0 : 1;
	int32 Skipped = 0;
	double Duration = 0.0;
	TArray<TSharedPtr<FJsonValue>> NativeTests;
	TSharedPtr<FJsonObject> NativeReport;
	if (IFileManager::Get().FileExists(*NativeIndex))
	{
		FString NativeJson;
		FFileHelper::LoadFileToString(NativeJson, *NativeIndex);
		const TSharedRef<TJsonReader<>> NativeReader =
			TJsonReaderFactory<>::Create(NativeJson);
		if (FJsonSerializer::Deserialize(NativeReader, NativeReport)
			&& NativeReport.IsValid())
		{
			Passed =
				static_cast<int32>(
					GetNumberFieldOr(
						NativeReport,
						TEXT("succeeded"),
						0.0))
				+ static_cast<int32>(
					GetNumberFieldOr(
						NativeReport,
						TEXT("succeededWithWarnings"),
						0.0));
			Failed = static_cast<int32>(
				GetNumberFieldOr(NativeReport, TEXT("failed"), 0.0));
			Skipped = static_cast<int32>(
				GetNumberFieldOr(NativeReport, TEXT("notRun"), 0.0));
			Duration =
				GetNumberFieldOr(
					NativeReport,
					TEXT("totalDuration"),
					0.0);
			if (NativeReport->HasTypedField<EJson::Array>(TEXT("tests")))
			{
				NativeTests = NativeReport->GetArrayField(TEXT("tests"));
			}
		}
	}
	const int32 Total = Passed + Failed + Skipped;
	const bool bSucceeded = Failed == 0
		&& Job.Status == TEXT("succeeded");
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetStringField(TEXT("schema"), TEXT("ue.test-report.v1"));
	Summary->SetStringField(TEXT("jobId"), Job.Id);
	Summary->SetStringField(TEXT("runner"), Runner);
	Summary->SetStringField(TEXT("test"), TestName);
	Summary->SetNumberField(TEXT("total"), Total);
	Summary->SetNumberField(TEXT("passed"), Passed);
	Summary->SetNumberField(TEXT("failed"), Failed);
	Summary->SetNumberField(TEXT("skipped"), Skipped);
	Summary->SetNumberField(TEXT("durationSeconds"), Duration);
	Summary->SetNumberField(
		TEXT("screenshotArtifactCount"),
		FMath::Min(ScreenshotFiles.Num(), 64));
	Summary->SetStringField(
		TEXT("status"),
		bSucceeded ? TEXT("passed") : TEXT("failed"));
	Job.Result->SetObjectField(TEXT("testReport"), Summary);

	FString SummaryJson;
	const TSharedRef<TJsonWriter<>> JsonWriter =
		TJsonWriterFactory<>::Create(&SummaryJson);
	FJsonSerializer::Serialize(Summary.ToSharedRef(), JsonWriter);
	const FString SummaryPath =
		FPaths::Combine(JobDirectory(Job.Id), TEXT("test-report.json"));
	FFileHelper::SaveStringToFile(SummaryJson, *SummaryPath);
	AddArtifact(
		Job,
		SummaryPath,
		TEXT("test-report.json"),
		TEXT("application/json"));

	FString TestCases;
	if (!NativeTests.IsEmpty())
	{
		for (const TSharedPtr<FJsonValue>& Value : NativeTests)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Test = Value->AsObject();
			const FString Name =
				GetStringFieldOr(
					Test,
					TEXT("fullTestPath"),
					GetStringFieldOr(Test, TEXT("testDisplayName")));
			const FString State =
				GetStringFieldOr(Test, TEXT("state"));
			FString Child;
			if (State.Equals(TEXT("Fail"), ESearchCase::IgnoreCase))
			{
				Child = TEXT("<failure message=\"Automation test failed.\"/>");
			}
			else if (State.Equals(TEXT("NotRun"), ESearchCase::IgnoreCase))
			{
				Child = TEXT("<skipped/>");
			}
			TestCases += FString::Printf(
				TEXT("  <testcase classname=\"%s\" name=\"%s\" time=\"%.6f\">%s</testcase>\n"),
				*XmlEscape(Runner),
				*XmlEscape(Name),
				GetNumberFieldOr(Test, TEXT("duration"), 0.0),
				*Child);
		}
	}
	else
	{
		const FString FailureNode =
			bSucceeded
				? FString()
				: FString::Printf(
					TEXT("<failure message=\"%s\"/>"),
					*XmlEscape(
						Job.Message.IsEmpty()
							? TEXT("Test process failed.")
							: Job.Message));
		TestCases = FString::Printf(
			TEXT("  <testcase classname=\"%s\" name=\"%s\">%s</testcase>\n"),
			*XmlEscape(Runner),
			*XmlEscape(TestName),
			*FailureNode);
	}
	const FString Junit = FString::Printf(
		TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
		TEXT("<testsuite name=\"%s\" tests=\"%d\" failures=\"%d\" skipped=\"%d\" time=\"%.6f\">\n")
		TEXT("%s")
		TEXT("</testsuite>\n"),
		*XmlEscape(Runner),
		Total,
		Failed,
		Skipped,
		Duration,
		*TestCases);
	const FString JunitPath =
		FPaths::Combine(JobDirectory(Job.Id), TEXT("junit.xml"));
	FFileHelper::SaveStringToFile(Junit, *JunitPath);
	AddArtifact(
		Job,
		JunitPath,
		TEXT("junit.xml"),
		TEXT("application/xml"));
}

void FProductionJobRuntime::WritePerformanceReport(FJob& Job)
{
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("ue.performance-run.v1"));
	Report->SetStringField(TEXT("runId"), Job.Id);
	Report->SetObjectField(
		TEXT("context"),
		Job.Result->GetObjectField(TEXT("context")));
	Report->SetObjectField(
		TEXT("metrics"),
		Job.Result->GetObjectField(TEXT("metrics")));
	Report->SetNumberField(
		TEXT("sampleCount"),
		GetNumberFieldOr(Job.Result, TEXT("sampleCount"), 0.0));
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Report.ToSharedRef(), Writer);
	const FString Path =
		FPaths::Combine(JobDirectory(Job.Id), TEXT("performance.json"));
	FFileHelper::SaveStringToFile(Json, *Path);
	AddArtifact(
		Job,
		Path,
		TEXT("performance.json"),
		TEXT("application/json"));
}

FProductionJobRuntime::FArtifact* FProductionJobRuntime::AddArtifact(
	FJob& Job,
	const FString& Path,
	const FString& Name,
	const FString& MimeType)
{
	const FString FullPath = FPaths::ConvertRelativePathToFull(Path);
	if (!IsPathWithin(FullPath, FPaths::ProjectSavedDir()))
	{
		return nullptr;
	}
	const int64 Size = IFileManager::Get().FileSize(*FullPath);
	if (Size < 0)
	{
		return nullptr;
	}
	if (FArtifact* Existing = Job.Artifacts.FindByPredicate(
			[&FullPath](const FArtifact& Candidate)
			{
				return FPaths::IsSamePath(Candidate.Path, FullPath);
			}))
	{
		return Existing;
	}
	FArtifact Artifact;
	Artifact.Id = NewOpaqueId(TEXT("artifact"));
	Artifact.Kind =
		MimeType == TEXT("application/x-unreal-trace")
			? TEXT("trace")
			: (MimeType.StartsWith(TEXT("image/"))
				? TEXT("image")
			: (MimeType == TEXT("application/json")
				|| MimeType == TEXT("application/xml")
				? TEXT("report")
				: (MimeType.StartsWith(TEXT("text/"))
					? TEXT("log")
					: TEXT("file"))));
	Artifact.Name = Name;
	Artifact.Path = FullPath;
	Artifact.MimeType = MimeType;
	Artifact.Size = Size;
	Artifact.RegisteredModifiedAtUtc =
		IFileManager::Get().GetTimeStamp(*FullPath);
	const bool bWithinPerFileBudget =
		Size <= MaxSynchronousArtifactHashBytes;
	const bool bWithinAggregateBudget =
		Size <= MaxSynchronousArtifactHashBytes
			- Job.SynchronousArtifactHashBytes;
	if (bWithinPerFileBudget && bWithinAggregateBudget)
	{
		Job.SynchronousArtifactHashBytes += Size;
		Artifact.Sha256 = ComputeFileSha256Stream(FullPath);
	}
	Artifact.bSha256Deferred =
		Size > 0 && Artifact.Sha256.IsEmpty();
	return &Job.Artifacts.Add_GetRef(MoveTemp(Artifact));
}

TSharedPtr<FJsonObject> FProductionJobRuntime::MakeJobSummary(
	const FJob& Job,
	bool bIncludeResult) const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.job.v1"));
	Data->SetStringField(TEXT("jobId"), Job.Id);
	Data->SetStringField(TEXT("kind"), Job.Kind);
	Data->SetStringField(TEXT("status"), Job.Status);
	TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
	Progress->SetNumberField(TEXT("fraction"), Job.Progress);
	Progress->SetStringField(TEXT("phase"), Job.Phase);
	if (!Job.Message.IsEmpty())
	{
		Progress->SetStringField(TEXT("message"), Job.Message);
	}
	Data->SetObjectField(TEXT("progress"), Progress);
	Data->SetStringField(TEXT("phase"), Job.Phase);
	Data->SetStringField(TEXT("createdAt"), Job.CreatedAtUtc);
	Data->SetStringField(TEXT("startedAt"), Job.StartedAtUtc);
	Data->SetStringField(TEXT("finishedAt"), Job.CompletedAtUtc);
	Data->SetStringField(TEXT("createdAtUtc"), Job.CreatedAtUtc);
	Data->SetStringField(TEXT("startedAtUtc"), Job.StartedAtUtc);
	Data->SetStringField(TEXT("completedAtUtc"), Job.CompletedAtUtc);
	if (Job.ProcessId != 0)
	{
		Data->SetNumberField(TEXT("processId"), Job.ProcessId);
	}
	if (Job.ReturnCode != INDEX_NONE)
	{
		Data->SetNumberField(TEXT("exitCode"), Job.ReturnCode);
		Data->SetNumberField(TEXT("returnCode"), Job.ReturnCode);
	}
	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	if (!Job.Message.IsEmpty())
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Job.ErrorCode);
		Error->SetStringField(TEXT("message"), Job.Message);
		Data->SetObjectField(TEXT("error"), Error);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Error));
	}
	Data->SetArrayField(TEXT("diagnostics"), Diagnostics);
	TArray<TSharedPtr<FJsonValue>> ArtifactValues;
	TArray<TSharedPtr<FJsonValue>> ArtifactRefs;
	for (const FArtifact& Artifact : Job.Artifacts)
	{
		ArtifactRefs.Add(MakeShared<FJsonValueString>(Artifact.Id));
		ArtifactValues.Add(
			MakeShared<FJsonValueObject>(
				MakeArtifactSummary(Artifact)));
	}
	Data->SetArrayField(TEXT("artifactRefs"), ArtifactRefs);
	Data->SetArrayField(TEXT("artifacts"), ArtifactValues);
	if (!Job.RequestId.IsEmpty())
	{
		Data->SetStringField(TEXT("requestId"), Job.RequestId);
	}
	if (bIncludeResult && Job.Result.IsValid())
	{
		Data->SetObjectField(TEXT("result"), CopyObject(Job.Result));
	}
	return Data;
}

TSharedPtr<FJsonObject> FProductionJobRuntime::MakeArtifactSummary(
	const FArtifact& Artifact)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.artifact.v1"));
	Data->SetStringField(TEXT("artifactId"), Artifact.Id);
	Data->SetStringField(
		TEXT("kind"),
		Artifact.Kind.IsEmpty() ? TEXT("file") : Artifact.Kind);
	Data->SetStringField(TEXT("name"), Artifact.Name);
	Data->SetStringField(TEXT("mimeType"), Artifact.MimeType);
	Data->SetNumberField(
		TEXT("sizeBytes"),
		static_cast<double>(Artifact.Size));
	Data->SetNumberField(TEXT("size"), static_cast<double>(Artifact.Size));
	if (!Artifact.Sha256.IsEmpty())
	{
		Data->SetStringField(TEXT("sha256"), Artifact.Sha256);
	}
	else if (Artifact.bSha256Deferred)
	{
		Data->SetBoolField(TEXT("sha256Deferred"), true);
	}
	return Data;
}

TSharedPtr<FJsonObject> FProductionJobRuntime::MakeRuntimeContext()
{
	TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
	Context->SetStringField(TEXT("project"), FApp::GetProjectName());
	UWorld* World =
		GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	Context->SetStringField(
		TEXT("map"),
		World ? World->GetMapName() : FString());
	Context->SetStringField(
		TEXT("rhi"),
		GDynamicRHI ? FString(GDynamicRHI->GetName()) : TEXT("unavailable"));
	Context->SetStringField(TEXT("gpu"), FPlatformMisc::GetPrimaryGPUBrand());
	Context->SetStringField(
		TEXT("resolution"),
		FString::Printf(
			TEXT("%dx%d"),
			GSystemResolution.ResX,
			GSystemResolution.ResY));
	Context->SetStringField(
		TEXT("configuration"),
		LexToString(FApp::GetBuildConfiguration()));
	return Context;
}

void FProductionJobRuntime::LoadJournals()
{
	TArray<FString> Directories;
	IFileManager::Get().FindFiles(
		Directories,
		*(JobsRoot() / TEXT("*")),
		false,
		true);
	for (const FString& DirectoryName : Directories)
	{
		const FString JournalPath =
			FPaths::Combine(
				JobsRoot(),
				DirectoryName,
				TEXT("job.json"));
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *JournalPath))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root)
			|| !Root.IsValid())
		{
			continue;
		}
		TSharedPtr<FJob> Job = MakeShared<FJob>();
		Job->Id = GetStringFieldOr(Root, TEXT("jobId"));
		Job->Kind = GetStringFieldOr(Root, TEXT("kind"));
		Job->Status = GetStringFieldOr(Root, TEXT("status"));
		Job->Phase = GetStringFieldOr(Root, TEXT("phase"));
		Job->Message = GetStringFieldOr(Root, TEXT("message"));
		Job->ErrorCode = GetStringFieldOr(Root, TEXT("errorCode"));
		Job->CreatedAtUtc = GetStringFieldOr(Root, TEXT("createdAtUtc"));
		Job->StartedAtUtc = GetStringFieldOr(Root, TEXT("startedAtUtc"));
		Job->CompletedAtUtc = GetStringFieldOr(Root, TEXT("completedAtUtc"));
		Job->Progress = GetNumberFieldOr(Root, TEXT("progress"), 0.0);
		Job->ProcessId = static_cast<uint32>(
			GetNumberFieldOr(Root, TEXT("processId"), 0.0));
		Job->ReturnCode = static_cast<int32>(
			GetNumberFieldOr(Root, TEXT("returnCode"), INDEX_NONE));
		Job->LogBaseCursor = static_cast<int64>(
			GetNumberFieldOr(Root, TEXT("logBaseCursor"), 0.0));
		Job->LogTotalChars = static_cast<int64>(
			GetNumberFieldOr(Root, TEXT("logTotalChars"), 0.0));
		if (Root->HasTypedField<EJson::Object>(TEXT("input")))
		{
			Job->Input = Root->GetObjectField(TEXT("input"));
		}
		else
		{
			Job->Input = MakeShared<FJsonObject>();
		}
		Job->RequestId = GetStringFieldOr(
			Root,
			TEXT("requestId"),
			GetStringFieldOr(Job->Input, TEXT("requestId")));
		Job->InputDigest = GetStringFieldOr(
			Root,
			TEXT("inputDigest"));
		if (Job->InputDigest.IsEmpty())
		{
			Job->InputDigest = ComputeChangePlanDigest(Job->Input);
		}
		if (Root->HasTypedField<EJson::Object>(TEXT("result")))
		{
			Job->Result = Root->GetObjectField(TEXT("result"));
		}
		else
		{
			Job->Result = MakeShared<FJsonObject>();
		}
		if (Root->HasTypedField<EJson::Array>(TEXT("artifacts")))
		{
			for (const TSharedPtr<FJsonValue>& Value :
				Root->GetArrayField(TEXT("artifacts")))
			{
				if (!Value.IsValid() || Value->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Object = Value->AsObject();
				FArtifact Artifact;
				Artifact.Id = GetStringFieldOr(Object, TEXT("artifactId"));
				Artifact.Kind = GetStringFieldOr(
					Object,
					TEXT("kind"),
					TEXT("file"));
				Artifact.Name = GetStringFieldOr(Object, TEXT("name"));
				Artifact.Path = GetStringFieldOr(Object, TEXT("path"));
				Artifact.MimeType = GetStringFieldOr(Object, TEXT("mimeType"));
				Artifact.Sha256 = GetStringFieldOr(Object, TEXT("sha256"));
				Artifact.Size = static_cast<int64>(
					GetNumberFieldOr(
						Object,
						TEXT("sizeBytes"),
						GetNumberFieldOr(Object, TEXT("size"), 0.0)));
				Artifact.bSha256Deferred =
					GetBoolFieldOr(
						Object,
						TEXT("sha256Deferred"),
						Artifact.Sha256.IsEmpty()
							&& Artifact.Size
								> MaxSynchronousArtifactHashBytes);
				const FString RegisteredModifiedAt =
					GetStringFieldOr(
						Object,
						TEXT("registeredModifiedAtUtc"));
				if (!RegisteredModifiedAt.IsEmpty())
				{
					FDateTime::ParseIso8601(
						*RegisteredModifiedAt,
						Artifact.RegisteredModifiedAtUtc);
				}
				if (!Artifact.Id.IsEmpty()
					&& !Artifact.Path.IsEmpty()
					&& IsPathWithin(
						Artifact.Path,
						FPaths::ProjectSavedDir()))
				{
					Job->Artifacts.Add(MoveTemp(Artifact));
				}
			}
		}
		const FString JobLogFile =
			FPaths::Combine(
				JobsRoot(),
				DirectoryName,
				TEXT("job.log"));
		if (IFileManager::Get().FileExists(*JobLogFile))
		{
			FFileHelper::LoadFileToString(Job->Output, *JobLogFile);
			if (Job->Output.Len() > MaxCapturedLogChars)
			{
				Job->LogTotalChars = Job->Output.Len();
				Job->LogBaseCursor =
					Job->Output.Len() - MaxCapturedLogChars;
				Job->Output.RightInline(MaxCapturedLogChars, false);
			}
			else
			{
				Job->LogTotalChars = Job->Output.Len();
				Job->LogBaseCursor = 0;
			}
		}
		if (Job->Id.IsEmpty())
		{
			continue;
		}
		if (!IsTerminalStatus(Job->Status))
		{
			Job->Status = TEXT("interrupted");
			Job->Phase = TEXT("complete");
			Job->ErrorCode = TEXT("editor_restarted");
			Job->Message =
				TEXT("The Editor restarted before this job reached a terminal state.");
			Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
			Job->Progress = 1.0;
		}
		Jobs.Add(Job->Id, Job);
		SaveJournal(*Job);
	}
}

void FProductionJobRuntime::SaveJournal(const FJob& Job) const
{
	const FString Directory = JobDirectory(Job.Id);
	IFileManager::Get().MakeDirectory(*Directory, true);
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("ue.job-journal.v1"));
	Root->SetStringField(TEXT("jobId"), Job.Id);
	Root->SetStringField(TEXT("kind"), Job.Kind);
	Root->SetStringField(TEXT("status"), Job.Status);
	Root->SetStringField(TEXT("phase"), Job.Phase);
	Root->SetStringField(TEXT("message"), Job.Message);
	Root->SetStringField(TEXT("errorCode"), Job.ErrorCode);
	Root->SetStringField(TEXT("createdAtUtc"), Job.CreatedAtUtc);
	Root->SetStringField(TEXT("startedAtUtc"), Job.StartedAtUtc);
	Root->SetStringField(TEXT("completedAtUtc"), Job.CompletedAtUtc);
	Root->SetNumberField(TEXT("progress"), Job.Progress);
	Root->SetNumberField(TEXT("processId"), Job.ProcessId);
	Root->SetNumberField(TEXT("returnCode"), Job.ReturnCode);
	Root->SetNumberField(TEXT("logBaseCursor"), Job.LogBaseCursor);
	Root->SetNumberField(TEXT("logTotalChars"), Job.LogTotalChars);
	Root->SetStringField(TEXT("requestId"), Job.RequestId);
	Root->SetStringField(TEXT("inputDigest"), Job.InputDigest);
	Root->SetObjectField(
		TEXT("input"),
		Job.Input.IsValid() ? CopyObject(Job.Input) : MakeShared<FJsonObject>());
	Root->SetObjectField(
		TEXT("result"),
		Job.Result.IsValid() ? CopyObject(Job.Result) : MakeShared<FJsonObject>());
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	for (const FArtifact& Artifact : Job.Artifacts)
	{
		TSharedPtr<FJsonObject> Object = MakeArtifactSummary(Artifact);
		Object->SetStringField(TEXT("path"), Artifact.Path);
		if (Artifact.RegisteredModifiedAtUtc.GetTicks() != 0)
		{
			Object->SetStringField(
				TEXT("registeredModifiedAtUtc"),
				Artifact.RegisteredModifiedAtUtc.ToIso8601());
		}
		Artifacts.Add(MakeShared<FJsonValueObject>(Object));
	}
	Root->SetArrayField(TEXT("artifacts"), Artifacts);

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(
			Json,
			*FPaths::Combine(Directory, TEXT("job.json")));
	}
}

FString FProductionJobRuntime::JobDirectory(
	const FString& JobId) const
{
	return FPaths::Combine(JobsRoot(), JobId);
}

FString FProductionJobRuntime::JobsRoot()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UEAIIntegration/Jobs")));
}

FString FProductionJobRuntime::NewOpaqueId(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("%s-%s"),
		Prefix,
		*FGuid::NewGuid().ToString(
			EGuidFormats::DigitsWithHyphensLower));
}

FString FProductionJobRuntime::GetStringFieldOr(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& DefaultValue)
{
	FString Value;
	return Object.IsValid()
		&& Object->TryGetStringField(Field, Value)
			? Value
			: DefaultValue;
}

double FProductionJobRuntime::GetNumberFieldOr(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	double DefaultValue)
{
	double Value = DefaultValue;
	return Object.IsValid()
		&& Object->TryGetNumberField(Field, Value)
			? Value
			: DefaultValue;
}

bool FProductionJobRuntime::GetBoolFieldOr(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	bool DefaultValue)
{
	bool Value = DefaultValue;
	return Object.IsValid()
		&& Object->TryGetBoolField(Field, Value)
			? Value
			: DefaultValue;
}

bool FProductionJobRuntime::IsSafeToken(
	const FString& Value,
	int32 MaxLength)
{
	if (Value.IsEmpty()
		|| Value.Len() > MaxLength
		|| Value.StartsWith(TEXT("-")))
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-')
			&& Character != TEXT('.')
			&& Character != TEXT('/')
			&& Character != TEXT(':'))
		{
			return false;
		}
	}
	return true;
}

bool FProductionJobRuntime::IsTerminalStatus(
	const FString& Status)
{
	return Status == TEXT("succeeded")
		|| Status == TEXT("failed")
		|| Status == TEXT("cancelled")
		|| Status == TEXT("interrupted");
}

FString FProductionJobRuntime::QuoteArgument(
	const FString& Value)
{
	FString Escaped = Value;
	Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
	return TEXT("\"") + Escaped + TEXT("\"");
}

bool FProductionJobRuntime::RunGitSync(
	const FString& Arguments,
	FString& OutStdOut,
	FString& OutStdErr,
	int32& OutReturnCode)
{
	const FString Root =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString FullArguments =
		TEXT("-C ") + QuoteArgument(Root) + TEXT(" ") + Arguments;
	return FPlatformProcess::ExecProcess(
		TEXT("git.exe"),
		*FullArguments,
		&OutReturnCode,
		&OutStdOut,
		&OutStdErr,
		*Root);
}

int64 FProductionJobRuntime::DirectorySize(
	const FString& Directory)
{
	if (!IFileManager::Get().DirectoryExists(*Directory))
	{
		return 0;
	}
	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(
		Files,
		*Directory,
		TEXT("*"),
		true,
		false);
	int64 Size = 0;
	for (const FString& File : Files)
	{
		const int64 FileSize = IFileManager::Get().FileSize(*File);
		if (FileSize > 0)
		{
			Size += FileSize;
		}
	}
	return Size;
}
}
