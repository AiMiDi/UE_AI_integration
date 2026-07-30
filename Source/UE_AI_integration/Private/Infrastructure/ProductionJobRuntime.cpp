#include "Infrastructure/ProductionJobRuntime.h"

#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProperties.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Sha256.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "RenderTimer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "UnrealEngine.h"

namespace UEAIIntegration::Infrastructure
{
namespace ProductionJobRuntimePrivate
{
	constexpr int32 MaxCapturedLogChars = 1024 * 1024;
	constexpr int32 MaxArtifactChunkBytes = 1024 * 1024;
	constexpr int64 MaxStandaloneChildReceiptBytes = 256 * 1024;
	constexpr int64 MaxSynchronousArtifactHashBytes =
		64ll * 1024ll * 1024ll;
	constexpr int32 MaxPackageScanFiles = 4096;
	constexpr int32 MaxPackageArtifacts = 32;
	constexpr int32 MaxTraceFrames = 10000;
	constexpr int32 MaxTraceTimers = 200;
	constexpr int32 MaxTraceCounters = 64;
	constexpr int32 MaxTraceCounterValues = 10000;
	constexpr double MaxStartupTimeoutSeconds = 3600.0;
	constexpr double MaxExecutionTimeoutSeconds = 43200.0;
	constexpr double MaxShutdownTimeoutSeconds = 600.0;
	constexpr double MaxHardTimeoutSeconds = 86400.0;
	constexpr int32 MaxValidatedJsonDepth = 64;

	FString BuildStandaloneCVarCommands()
	{
		static const TCHAR* Names[] = {
			TEXT("r.VSync"),
			TEXT("t.MaxFPS"),
			TEXT("r.ScreenPercentage"),
			TEXT("r.DynamicRes.OperationMode"),
			TEXT("sg.ResolutionQuality"),
			TEXT("sg.ViewDistanceQuality"),
			TEXT("sg.AntiAliasingQuality"),
			TEXT("sg.ShadowQuality"),
			TEXT("sg.GlobalIlluminationQuality"),
			TEXT("sg.ReflectionQuality"),
			TEXT("sg.PostProcessQuality"),
			TEXT("sg.TextureQuality"),
			TEXT("sg.EffectsQuality"),
			TEXT("sg.FoliageQuality"),
			TEXT("sg.ShadingQuality")
		};
		FString Commands;
		for (const TCHAR* Name : Names)
		{
			const IConsoleVariable* Variable =
				IConsoleManager::Get().FindConsoleVariable(Name);
			if (!Variable)
			{
				continue;
			}
			const FString Value = Variable->GetString();
			if (Value.IsEmpty()
				|| Value.Contains(TEXT(";"))
				|| Value.Contains(TEXT("\"")))
			{
				continue;
			}
			Commands += FString::Printf(
				TEXT("%s %s;"),
				Name,
				*Value);
		}
		return Commands;
	}

	bool IsStartupPhase(const FString& Phase)
	{
		return Phase == TEXT("launching")
			|| Phase == TEXT("loading")
			|| Phase == TEXT("discovering");
	}

	bool IsExecutionPhase(const FString& Phase)
	{
		return Phase == TEXT("running")
			|| Phase == TEXT("reporting");
	}

	bool IsEditorExecutable(const FString& Executable)
	{
		const FString Name =
			FPaths::GetCleanFilename(Executable).ToLower();
		return Name.Contains(TEXT("unrealeditor"))
			|| Name.Contains(TEXT("ue4editor"));
	}

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

	bool HasOnlyFiniteJsonNumbers(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path,
		const int32 Depth,
		FString& OutInvalidPath)
	{
		if (!Value.IsValid())
		{
			OutInvalidPath = Path;
			return false;
		}
		if (Depth > MaxValidatedJsonDepth)
		{
			OutInvalidPath = Path + TEXT(" (depth limit)");
			return false;
		}
		if (Value->Type == EJson::Number)
		{
			if (!FMath::IsFinite(Value->AsNumber()))
			{
				OutInvalidPath = Path;
				return false;
			}
			return true;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (!HasOnlyFiniteJsonNumbers(
						Values[Index],
						FString::Printf(TEXT("%s[%d]"), *Path, Index),
						Depth + 1,
						OutInvalidPath))
				{
					return false;
				}
			}
			return true;
		}
		if (Value->Type == EJson::Object)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
				Value->AsObject()->Values)
			{
				if (!HasOnlyFiniteJsonNumbers(
						Pair.Value,
						Path + TEXT(".") + Pair.Key,
						Depth + 1,
						OutInvalidPath))
				{
					return false;
				}
			}
		}
		return true;
	}

	bool HasOnlyFiniteJsonNumbers(
		const TSharedPtr<FJsonObject>& Object,
		FString& OutInvalidPath)
	{
		if (!Object.IsValid())
		{
			OutInvalidPath = TEXT("$");
			return false;
		}
		return HasOnlyFiniteJsonNumbers(
			MakeShared<FJsonValueObject>(Object),
			TEXT("$"),
			0,
			OutInvalidPath);
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

	TArray<FString> ParseCsvRow(const FString& Line)
	{
		TArray<FString> Values;
		FString Current;
		bool bQuoted = false;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const TCHAR Character = Line[Index];
			if (Character == TEXT('"'))
			{
				if (bQuoted
					&& Index + 1 < Line.Len()
					&& Line[Index + 1] == TEXT('"'))
				{
					Current.AppendChar(TEXT('"'));
					++Index;
				}
				else
				{
					bQuoted = !bQuoted;
				}
			}
			else if (Character == TEXT(',') && !bQuoted)
			{
				Values.Add(Current.TrimStartAndEnd());
				Current.Reset();
			}
			else
			{
				Current.AppendChar(Character);
			}
		}
		Values.Add(Current.TrimStartAndEnd());
		return Values;
	}

	int32 FindCsvColumn(
		const TArray<FString>& Header,
		const TArray<FString>& Candidates)
	{
		int32 Match = INDEX_NONE;
		for (int32 Index = 0; Index < Header.Num(); ++Index)
		{
			FString Normalized = Header[Index].ToLower();
			Normalized.ReplaceInline(TEXT(" "), TEXT(""));
			Normalized.ReplaceInline(TEXT("_"), TEXT(""));
			Normalized.ReplaceInline(TEXT("(ms)"), TEXT(""));
			for (const FString& Candidate : Candidates)
			{
				if (Normalized == Candidate
					|| Normalized.EndsWith(
						FString(TEXT("/")) + Candidate))
				{
					if (Match != INDEX_NONE)
					{
						// Ambiguous columns must be rejected rather than
						// silently selecting Gameplay/GPU-memory counters.
						return INDEX_NONE;
					}
					Match = Index;
				}
			}
		}
		return Match;
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

	FString GetEditorStandaloneExecutable()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::EngineDir(),
#if PLATFORM_WINDOWS
				TEXT("Binaries/Win64/UnrealEditor.exe")
#elif PLATFORM_MAC
				TEXT("Binaries/Mac/UnrealEditor")
#else
				TEXT("Binaries/Linux/UnrealEditor")
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

	double ReadBoundedNumber(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const double DefaultValue,
		const double Minimum,
		const double Maximum)
	{
		double Value = DefaultValue;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		if (!FMath::IsFinite(Value))
		{
			Value = DefaultValue;
		}
		return FMath::Clamp(Value, Minimum, Maximum);
	}

	TSharedPtr<FJsonObject> BuildTraceProviderSummary(
		const TraceServices::IAnalysisSession& Session,
		const TSharedPtr<FJsonObject>& Params)
	{
		// UE 5.3 guards every session/provider read, including the seemingly
		// trivial duration accessor. Acquire the session read scope before the
		// first read and keep it alive through all provider aggregation.
		TraceServices::FAnalysisSessionReadScope ReadScope(Session);
		const double RawDuration = Session.GetDurationSeconds();
		const bool bDurationSanitized =
			!FMath::IsFinite(RawDuration) || RawDuration < 0.0;
		const double Duration =
			bDurationSanitized ? 0.0 : RawDuration;
		const double RequestedEnd = ReadBoundedNumber(
			Params,
			TEXT("endTimeSeconds"),
			Duration,
			0.0,
			Duration);
		const double MaxDuration = ReadBoundedNumber(
			Params,
			TEXT("maxDurationSeconds"),
			60.0,
			0.1,
			600.0);
		const double RequestedStart = ReadBoundedNumber(
			Params,
			TEXT("startTimeSeconds"),
			FMath::Max(0.0, RequestedEnd - MaxDuration),
			0.0,
			RequestedEnd);
		const double IntervalStart = RequestedStart;
		const double IntervalEnd = FMath::Min(
			RequestedEnd,
			IntervalStart + MaxDuration);
		const int32 MaxFrames = FMath::Clamp(
			static_cast<int32>(ReadBoundedNumber(
				Params,
				TEXT("maxFrames"),
				2000.0,
				1.0,
				MaxTraceFrames)),
			1,
			MaxTraceFrames);
		const int32 MaxTimers = FMath::Clamp(
			static_cast<int32>(ReadBoundedNumber(
				Params,
				TEXT("maxTimers"),
				50.0,
				1.0,
				MaxTraceTimers)),
			1,
			MaxTraceTimers);
		const int32 MaxCounters = FMath::Clamp(
			static_cast<int32>(ReadBoundedNumber(
				Params,
				TEXT("maxCounters"),
				32.0,
				1.0,
				MaxTraceCounters)),
			1,
			MaxTraceCounters);
		const int32 MaxCounterValues = FMath::Clamp(
			static_cast<int32>(ReadBoundedNumber(
				Params,
				TEXT("maxCounterValues"),
				2000.0,
				1.0,
				MaxTraceCounterValues)),
			1,
			MaxTraceCounterValues);

		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("schema"), TEXT("ue.trace-analysis.v1"));
		Summary->SetNumberField(TEXT("durationSeconds"), Duration);
		Summary->SetBoolField(
			TEXT("durationSanitized"),
			bDurationSanitized);
		Summary->SetNumberField(TEXT("intervalStartSeconds"), IntervalStart);
		Summary->SetNumberField(TEXT("intervalEndSeconds"), IntervalEnd);
		Summary->SetBoolField(
			TEXT("intervalClamped"),
			RequestedEnd - RequestedStart > MaxDuration);

		TSharedPtr<FJsonObject> Frames = MakeShared<FJsonObject>();
		const TraceServices::IFrameProvider* FrameProvider =
			Session.ReadProvider<TraceServices::IFrameProvider>(
				TraceServices::GetFrameProviderName());
		auto AddFrameSummary =
			[&Frames, FrameProvider, IntervalStart, IntervalEnd, MaxFrames](
				const TCHAR* Name,
				const ETraceFrameType FrameType)
			{
				TArray<double> Samples;
				uint64 Total = 0;
				uint64 ValidTotal = 0;
				uint64 InvalidTotal = 0;
				if (FrameProvider)
				{
					FrameProvider->EnumerateFrames(
						FrameType,
						IntervalStart,
						IntervalEnd,
						[&Samples, &Total, &ValidTotal, &InvalidTotal, MaxFrames](
							const TraceServices::FFrame& Frame)
						{
							++Total;
							const double DurationMs =
								(Frame.EndTime - Frame.StartTime) * 1000.0;
							if (!FMath::IsFinite(DurationMs)
								|| DurationMs < 0.0)
							{
								++InvalidTotal;
								return;
							}
							++ValidTotal;
							if (Samples.Num() < MaxFrames)
							{
								Samples.Add(DurationMs);
							}
						});
				}
				TSharedPtr<FJsonObject> FrameSummary =
					FProductionJobRuntime::SummarizeMetric(
						Samples,
						16.6667);
				FrameSummary->SetNumberField(
					TEXT("totalFrameCount"),
					static_cast<double>(Total));
				FrameSummary->SetNumberField(
					TEXT("validFrameCount"),
					static_cast<double>(ValidTotal));
				FrameSummary->SetNumberField(
					TEXT("invalidFrameCount"),
					static_cast<double>(InvalidTotal));
				FrameSummary->SetBoolField(
					TEXT("truncated"),
					ValidTotal > static_cast<uint64>(Samples.Num()));
				Frames->SetObjectField(Name, FrameSummary);
			};
		AddFrameSummary(TEXT("game"), TraceFrameType_Game);
		AddFrameSummary(TEXT("rendering"), TraceFrameType_Rendering);
		Summary->SetObjectField(TEXT("frames"), Frames);

		struct FTimerRow
		{
			FString Name;
			FString File;
			uint32 Id = 0;
			uint32 Line = 0;
			bool bGpu = false;
			uint64 InstanceCount = 0;
			double TotalInclusiveMs = 0.0;
			double AverageInclusiveMs = 0.0;
			double MaxInclusiveMs = 0.0;
			double TotalExclusiveMs = 0.0;
		};
		TArray<FTimerRow> TimerRows;
		int32 InvalidTimerCount = 0;
		const TraceServices::ITimingProfilerProvider* TimingProvider =
			Session.ReadProvider<TraceServices::ITimingProfilerProvider>(
				TraceServices::GetTimingProfilerProviderName());
		if (TimingProvider && IntervalEnd > IntervalStart)
		{
			TraceServices::FCreateAggreationParams AggregationParams;
			AggregationParams.IntervalStart = IntervalStart;
			AggregationParams.IntervalEnd = IntervalEnd;
			AggregationParams.CpuThreadFilter =
				[](const uint32 ThreadId)
				{
					return true;
				};
			AggregationParams.IncludeGpu = true;
			TUniquePtr<
				TraceServices::ITable<
					TraceServices::FTimingProfilerAggregatedStats>>
				Table(TimingProvider->CreateAggregation(AggregationParams));
			if (Table.IsValid())
			{
				TUniquePtr<
					TraceServices::ITableReader<
						TraceServices::FTimingProfilerAggregatedStats>>
					Reader(Table->CreateReader());
				for (; Reader.IsValid() && Reader->IsValid(); Reader->NextRow())
				{
					const TraceServices::FTimingProfilerAggregatedStats* Row =
						Reader->GetCurrentRow();
					if (!Row || !Row->Timer)
					{
						continue;
					}
					const double TotalInclusiveMs =
						Row->TotalInclusiveTime * 1000.0;
					const double AverageInclusiveMs =
						Row->AverageInclusiveTime * 1000.0;
					const double MaxInclusiveMs =
						Row->MaxInclusiveTime * 1000.0;
					const double TotalExclusiveMs =
						Row->TotalExclusiveTime * 1000.0;
					if (!FMath::IsFinite(TotalInclusiveMs)
						|| !FMath::IsFinite(AverageInclusiveMs)
						|| !FMath::IsFinite(MaxInclusiveMs)
						|| !FMath::IsFinite(TotalExclusiveMs)
						|| TotalInclusiveMs < 0.0
						|| AverageInclusiveMs < 0.0
						|| MaxInclusiveMs < 0.0
						|| TotalExclusiveMs < 0.0)
					{
						++InvalidTimerCount;
						continue;
					}
					FTimerRow& Output = TimerRows.AddDefaulted_GetRef();
					Output.Name = Row->Timer->Name
						? Row->Timer->Name
						: TEXT("<unnamed>");
					Output.File = Row->Timer->File
						? Row->Timer->File
						: FString();
					Output.Id = Row->Timer->Id;
					Output.Line = Row->Timer->Line;
					Output.bGpu = Row->Timer->IsGpuTimer != 0;
					Output.InstanceCount = Row->InstanceCount;
					Output.TotalInclusiveMs = TotalInclusiveMs;
					Output.AverageInclusiveMs = AverageInclusiveMs;
					Output.MaxInclusiveMs = MaxInclusiveMs;
					Output.TotalExclusiveMs = TotalExclusiveMs;
				}
			}
		}
		TimerRows.Sort(
			[](const FTimerRow& Left, const FTimerRow& Right)
			{
				if (Left.TotalInclusiveMs != Right.TotalInclusiveMs)
				{
					return Left.TotalInclusiveMs > Right.TotalInclusiveMs;
				}
				return Left.Name < Right.Name;
			});
		TArray<TSharedPtr<FJsonValue>> TimerValues;
		const int32 TimerCount = FMath::Min(MaxTimers, TimerRows.Num());
		TimerValues.Reserve(TimerCount);
		for (int32 Index = 0; Index < TimerCount; ++Index)
		{
			const FTimerRow& Row = TimerRows[Index];
			TSharedPtr<FJsonObject> Timer = MakeShared<FJsonObject>();
			Timer->SetStringField(TEXT("name"), Row.Name);
			Timer->SetNumberField(TEXT("timerId"), Row.Id);
			Timer->SetBoolField(TEXT("gpu"), Row.bGpu);
			Timer->SetNumberField(
				TEXT("instanceCount"),
				static_cast<double>(Row.InstanceCount));
			Timer->SetNumberField(
				TEXT("totalInclusiveMs"),
				Row.TotalInclusiveMs);
			Timer->SetNumberField(
				TEXT("averageInclusiveMs"),
				Row.AverageInclusiveMs);
			Timer->SetNumberField(
				TEXT("maxInclusiveMs"),
				Row.MaxInclusiveMs);
			Timer->SetNumberField(
				TEXT("totalExclusiveMs"),
				Row.TotalExclusiveMs);
			if (!Row.File.IsEmpty())
			{
				Timer->SetStringField(TEXT("file"), Row.File);
				Timer->SetNumberField(TEXT("line"), Row.Line);
			}
			TimerValues.Add(MakeShared<FJsonValueObject>(Timer));
		}
		Summary->SetArrayField(TEXT("timers"), TimerValues);
		Summary->SetNumberField(TEXT("timerTotal"), TimerRows.Num());
		Summary->SetNumberField(
			TEXT("invalidTimerCount"),
			InvalidTimerCount);
		Summary->SetBoolField(
			TEXT("timersTruncated"),
			TimerRows.Num() > TimerCount);

		struct FTraceThreadGroup
		{
			FString OutputName;
			TArray<uint32> ThreadIds;
			TArray<FString> ThreadNames;
			int32 MatchingThreadCount = 0;
			bool bIncludeGpu = false;
		};
		TArray<FTraceThreadGroup> TraceThreadGroups;
		{
			FTraceThreadGroup& GameGroup =
				TraceThreadGroups.AddDefaulted_GetRef();
			GameGroup.OutputName = TEXT("game");
			FTraceThreadGroup& RenderGroup =
				TraceThreadGroups.AddDefaulted_GetRef();
			RenderGroup.OutputName = TEXT("render");
			FTraceThreadGroup& RhiGroup =
				TraceThreadGroups.AddDefaulted_GetRef();
			RhiGroup.OutputName = TEXT("rhi");
			FTraceThreadGroup& GpuGroup =
				TraceThreadGroups.AddDefaulted_GetRef();
			GpuGroup.OutputName = TEXT("gpu");
			GpuGroup.bIncludeGpu = true;
		}
		const TraceServices::IThreadProvider* ThreadProvider =
			Session.ReadProvider<TraceServices::IThreadProvider>(
				TraceServices::GetThreadProviderName());
		if (ThreadProvider)
		{
			ThreadProvider->EnumerateThreads(
				[&TraceThreadGroups](
					const TraceServices::FThreadInfo& Thread)
				{
					const FString ThreadName =
						Thread.Name ? Thread.Name : TEXT("<unnamed>");
					const FString NormalizedName = ThreadName.ToLower();
					int32 GroupIndex = INDEX_NONE;
					if (NormalizedName.Contains(TEXT("gamethread"))
						|| NormalizedName.Contains(TEXT("game thread")))
					{
						GroupIndex = 0;
					}
					else if (NormalizedName.Contains(TEXT("renderthread"))
						|| NormalizedName.Contains(TEXT("render thread")))
					{
						GroupIndex = 1;
					}
					else if (NormalizedName.Contains(TEXT("rhithread"))
						|| NormalizedName.Contains(TEXT("rhi thread")))
					{
						GroupIndex = 2;
					}
					if (GroupIndex == INDEX_NONE)
					{
						return;
					}
					FTraceThreadGroup& Group =
						TraceThreadGroups[GroupIndex];
					++Group.MatchingThreadCount;
					if (Group.ThreadIds.Num() < 64)
					{
						Group.ThreadIds.Add(Thread.Id);
						Group.ThreadNames.Add(ThreadName);
					}
				});
		}
		TSharedPtr<FJsonObject> ThreadAggregates =
			MakeShared<FJsonObject>();
		for (const FTraceThreadGroup& Group : TraceThreadGroups)
		{
			TSharedPtr<FJsonObject> GroupSummary =
				MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> ThreadIdValues;
			TArray<TSharedPtr<FJsonValue>> ThreadNameValues;
			for (int32 ThreadIndex = 0;
				ThreadIndex < Group.ThreadIds.Num();
				++ThreadIndex)
			{
				ThreadIdValues.Add(
					MakeShared<FJsonValueNumber>(
						Group.ThreadIds[ThreadIndex]));
				ThreadNameValues.Add(
					MakeShared<FJsonValueString>(
						Group.ThreadNames[ThreadIndex]));
			}
			GroupSummary->SetArrayField(
				TEXT("threadIds"),
				ThreadIdValues);
			GroupSummary->SetArrayField(
				TEXT("threadNames"),
				ThreadNameValues);
			GroupSummary->SetNumberField(
				TEXT("matchingThreadCount"),
				Group.MatchingThreadCount);
			GroupSummary->SetBoolField(
				TEXT("threadsTruncated"),
				Group.MatchingThreadCount > Group.ThreadIds.Num());
			if (!TimingProvider
				|| (!Group.bIncludeGpu && Group.ThreadIds.IsEmpty()))
			{
				GroupSummary->SetBoolField(TEXT("available"), false);
				ThreadAggregates->SetObjectField(
					Group.OutputName,
					GroupSummary);
				continue;
			}

			TraceServices::FCreateAggreationParams GroupParams;
			GroupParams.IntervalStart = IntervalStart;
			GroupParams.IntervalEnd = IntervalEnd;
			const TArray<uint32> IncludedThreadIds = Group.ThreadIds;
			GroupParams.CpuThreadFilter =
				[IncludedThreadIds](const uint32 ThreadId)
				{
					return IncludedThreadIds.Contains(ThreadId);
				};
			GroupParams.IncludeGpu = Group.bIncludeGpu;
			TUniquePtr<
				TraceServices::ITable<
					TraceServices::FTimingProfilerAggregatedStats>>
				GroupTable(
					TimingProvider->CreateAggregation(GroupParams));
			double TotalExclusiveMs = 0.0;
			uint64 InstanceCount = 0;
			int32 AggregatedTimerCount = 0;
			int32 InvalidAggregatedTimerCount = 0;
			if (GroupTable.IsValid())
			{
				TUniquePtr<
					TraceServices::ITableReader<
						TraceServices::FTimingProfilerAggregatedStats>>
					GroupReader(GroupTable->CreateReader());
				for (;
					GroupReader.IsValid() && GroupReader->IsValid();
					GroupReader->NextRow())
				{
					const TraceServices::FTimingProfilerAggregatedStats* Row =
						GroupReader->GetCurrentRow();
					if (!Row || !Row->Timer)
					{
						continue;
					}
					const double RowExclusiveMs =
						Row->TotalExclusiveTime * 1000.0;
					const double NewTotalExclusiveMs =
						TotalExclusiveMs + RowExclusiveMs;
					if (!FMath::IsFinite(RowExclusiveMs)
						|| RowExclusiveMs < 0.0
						|| !FMath::IsFinite(NewTotalExclusiveMs))
					{
						++InvalidAggregatedTimerCount;
						continue;
					}
					++AggregatedTimerCount;
					InstanceCount += Row->InstanceCount;
					TotalExclusiveMs = NewTotalExclusiveMs;
				}
			}
			GroupSummary->SetBoolField(
				TEXT("available"),
				AggregatedTimerCount > 0);
			GroupSummary->SetNumberField(
				TEXT("timerCount"),
				AggregatedTimerCount);
			GroupSummary->SetNumberField(
				TEXT("invalidTimerCount"),
				InvalidAggregatedTimerCount);
			GroupSummary->SetNumberField(
				TEXT("instanceCount"),
				static_cast<double>(InstanceCount));
			GroupSummary->SetNumberField(
				TEXT("totalExclusiveMs"),
				TotalExclusiveMs);
			ThreadAggregates->SetObjectField(
				Group.OutputName,
				GroupSummary);
		}
		Summary->SetObjectField(
			TEXT("threadAggregates"),
			ThreadAggregates);

		TSet<FString> RequestedCounters;
		for (const FString& CounterName :
			ReadStringArray(Params, TEXT("counterNames")))
		{
			RequestedCounters.Add(CounterName);
		}
		TArray<TSharedPtr<FJsonValue>> CounterValues;
		int32 MatchingCounterTotal = 0;
		const TraceServices::ICounterProvider* CounterProvider =
			Session.ReadProvider<TraceServices::ICounterProvider>(
				TraceServices::GetCounterProviderName());
		if (CounterProvider)
		{
			CounterProvider->EnumerateCounters(
				[&](
					const uint32 CounterId,
					const TraceServices::ICounter& Counter)
				{
					const FString Name =
						Counter.GetName() ? Counter.GetName() : TEXT("<unnamed>");
					if (!RequestedCounters.IsEmpty()
						&& !RequestedCounters.Contains(Name))
					{
						return;
					}
					++MatchingCounterTotal;
					if (CounterValues.Num() >= MaxCounters)
					{
						return;
					}

					TArray<double> Values;
					int32 ObservedValueCount = 0;
					int32 ValidValueCount = 0;
					int32 InvalidValueCount = 0;
					if (Counter.IsFloatingPoint())
					{
						Counter.EnumerateFloatValues(
							IntervalStart,
							IntervalEnd,
							false,
							[&](const double Time, const double Value)
							{
								++ObservedValueCount;
								if (!FMath::IsFinite(Value))
								{
									++InvalidValueCount;
									return;
								}
								++ValidValueCount;
								if (Values.Num() < MaxCounterValues)
								{
									Values.Add(Value);
								}
							});
					}
					else
					{
						Counter.EnumerateValues(
							IntervalStart,
							IntervalEnd,
							false,
							[&](const double Time, const int64 Value)
							{
								++ObservedValueCount;
								++ValidValueCount;
								if (Values.Num() < MaxCounterValues)
								{
									Values.Add(static_cast<double>(Value));
								}
							});
					}
					TSharedPtr<FJsonObject> ValueSummary =
						FProductionJobRuntime::SummarizeMetric(
							Values,
							TNumericLimits<double>::Max());
					ValueSummary->SetStringField(
						TEXT("unit"),
						Counter.GetDisplayHint()
								== TraceServices::CounterDisplayHint_Memory
							? TEXT("bytes")
							: TEXT("value"));
					ValueSummary->RemoveField(TEXT("budgetMs"));
					ValueSummary->RemoveField(TEXT("overBudgetFrames"));

					TSharedPtr<FJsonObject> CounterObject =
						MakeShared<FJsonObject>();
					CounterObject->SetNumberField(TEXT("counterId"), CounterId);
					CounterObject->SetStringField(TEXT("name"), Name);
					CounterObject->SetStringField(
						TEXT("group"),
						Counter.GetGroup() ? Counter.GetGroup() : TEXT(""));
					CounterObject->SetNumberField(
						TEXT("observedValueCount"),
						ObservedValueCount);
					CounterObject->SetNumberField(
						TEXT("validValueCount"),
						ValidValueCount);
					CounterObject->SetNumberField(
						TEXT("invalidValueCount"),
						InvalidValueCount);
					CounterObject->SetBoolField(
						TEXT("valuesTruncated"),
						ValidValueCount > Values.Num());
					CounterObject->SetObjectField(
						TEXT("summary"),
						ValueSummary);
					CounterValues.Add(
						MakeShared<FJsonValueObject>(CounterObject));
				});
		}
		Summary->SetArrayField(TEXT("counters"), CounterValues);
		Summary->SetNumberField(
			TEXT("counterTotal"),
			MatchingCounterTotal);
		Summary->SetBoolField(
			TEXT("countersTruncated"),
			MatchingCounterTotal > CounterValues.Num());
		return Summary;
	}
}

using namespace ProductionJobRuntimePrivate;

FProductionJobRuntime::FProductionJobRuntime(
	FScenarioOperation InStartScenario,
	FScenarioOperation InGetScenarioStatus,
	FScenarioOperation InGetScenarioResult,
	FScenarioOperation InCancelScenario)
	: StartScenario(MoveTemp(InStartScenario))
	, GetScenarioStatus(MoveTemp(InGetScenarioStatus))
	, GetScenarioResult(MoveTemp(InGetScenarioResult))
	, CancelScenario(MoveTemp(InCancelScenario))
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
				TerminateProcessTree(Job);
			}
			if (!IsTerminalStatus(Job.Status))
			{
				Job.Status = TEXT("interrupted");
				Job.ErrorCode = TEXT("editor_shutdown");
				Job.Message =
					TEXT("The Editor shut down before this job completed.");
				Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
				Job.Progress = 1.0;
				TransitionProcessPhase(Job, TEXT("complete"));
				PostProcessJob(Job);
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
		if (Job.TraceAnalysisSession.IsValid()
			&& !Job.TraceAnalysisSession->IsAnalysisComplete())
		{
			Job.TraceAnalysisSession->Stop(true);
			Job.TraceAnalysisSession.Reset();
		}
		if (!IsTerminalStatus(Job.Status))
		{
			Job.Status = TEXT("interrupted");
			Job.ErrorCode = TEXT("editor_shutdown");
			Job.Message =
				TEXT("The Editor shut down before this job completed.");
			Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
			Job.Progress = 1.0;
			TransitionProcessPhase(Job, TEXT("complete"));
			SaveJournal(Job);
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
		else if (Pair.Value->Kind == TEXT("traceAnalysis"))
		{
			TickTraceAnalysisJob(*Pair.Value);
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
	const FString Digest =
		DigestJson(Request.IsValid() ? Request : MakeShared<FJsonObject>());
	return Digest.IsEmpty() ? FString() : TEXT("sha256:") + Digest;
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
	const bool bBudgetSanitized = !FMath::IsFinite(BudgetMs);
	const double SafeBudgetMs = bBudgetSanitized
		? TNumericLimits<double>::Max()
		: BudgetMs;
	TArray<double> Sorted;
	Sorted.Reserve(Samples.Num());
	int32 InvalidSampleCount = 0;
	for (const double Value : Samples)
	{
		if (FMath::IsFinite(Value))
		{
			Sorted.Add(Value);
		}
		else
		{
			++InvalidSampleCount;
		}
	}
	Summary->SetNumberField(TEXT("sampleCount"), Sorted.Num());
	Summary->SetNumberField(TEXT("inputSampleCount"), Samples.Num());
	Summary->SetNumberField(
		TEXT("invalidSampleCount"),
		InvalidSampleCount);
	Summary->SetBoolField(
		TEXT("budgetSanitized"),
		bBudgetSanitized);
	Summary->SetStringField(TEXT("unit"), TEXT("ms"));
	if (Sorted.IsEmpty())
	{
		Summary->SetBoolField(TEXT("available"), false);
		return Summary;
	}

	Sorted.Sort();
	int32 OverBudget = 0;
	double Scale = 0.0;
	for (const double Value : Sorted)
	{
		Scale = FMath::Max(Scale, FMath::Abs(Value));
		if (Value > SafeBudgetMs)
		{
			++OverBudget;
		}
	}
	double NormalizedSum = 0.0;
	if (Scale > 0.0)
	{
		for (const double Value : Sorted)
		{
			NormalizedSum += Value / Scale;
		}
	}
	const double NormalizedMean = Scale > 0.0
		? FMath::Clamp(
			NormalizedSum / static_cast<double>(Sorted.Num()),
			-1.0,
			1.0)
		: 0.0;
	const double Mean = NormalizedMean * Scale;
	auto Percentile = [&Sorted](const double Fraction)
	{
		const double Position = Fraction * (Sorted.Num() - 1);
		const int32 Lower = FMath::FloorToInt(Position);
		const int32 Upper = FMath::CeilToInt(Position);
		if (Lower == Upper)
		{
			return Sorted[Lower];
		}
		return FMath::LerpStable(
			Sorted[Lower],
			Sorted[Upper],
			Position - Lower);
	};
	Summary->SetBoolField(TEXT("available"), true);
	Summary->SetNumberField(TEXT("min"), Sorted[0]);
	Summary->SetNumberField(TEXT("max"), Sorted.Last());
	Summary->SetNumberField(TEXT("mean"), Mean);
	Summary->SetNumberField(TEXT("p50"), Percentile(0.50));
	Summary->SetNumberField(TEXT("p95"), Percentile(0.95));
	Summary->SetNumberField(TEXT("p99"), Percentile(0.99));
	Summary->SetNumberField(TEXT("budgetMs"), SafeBudgetMs);
	Summary->SetNumberField(TEXT("overBudgetFrames"), OverBudget);
	return Summary;
}

bool FProductionJobRuntime::IsPerformanceSampleValid(const double Value)
{
	return FMath::IsFinite(Value) && Value >= 0.0;
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
	if (Job.Kind == TEXT("performance")
		&& Job.PerformanceMode == TEXT("scenario")
		&& !Job.ScenarioRunId.IsEmpty()
		&& CancelScenario)
	{
		TSharedPtr<FJsonObject> CancelParams = MakeShared<FJsonObject>();
		CancelParams->SetStringField(TEXT("runId"), Job.ScenarioRunId);
		CancelScenario(CancelParams);
	}
	if (Job.bOwnsTrace && !Job.TraceJobId.IsEmpty())
	{
		TSharedRef<FJsonObject> StopParams = MakeShared<FJsonObject>();
		StopParams->SetStringField(TEXT("traceId"), Job.TraceJobId);
		StopTrace(StopParams);
	}
	if (Job.TraceAnalysisSession.IsValid()
		&& !Job.TraceAnalysisSession->IsAnalysisComplete())
	{
		Job.TraceAnalysisSession->Stop(true);
	}
	if (Job.ProcessHandle.IsValid())
	{
		TerminateProcessTree(Job);
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
	Job->CreatedAtSeconds = FPlatformTime::Seconds();
	Job->PhaseStartedAtSeconds = Job->CreatedAtSeconds;
	Job->PhaseHistory.Add(TEXT("queued"));
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
	FProcessLaunchSpec Spec;
	Spec.Kind = Kind;
	Spec.Executable = Executable;
	Spec.Arguments = Arguments;
	Spec.WorkingDirectory = WorkingDirectory;
	Spec.PostProcess = PostProcess;
	Spec.DefaultExecutionTimeoutSeconds = TimeoutSeconds;
	Spec.bEditorProcess = IsEditorExecutable(Executable);
	Spec.bAutomationProcess =
		Spec.bEditorProcess && PostProcess == TEXT("test");
	return StartProcessJob(Spec, Input, OutError);
}

TSharedPtr<FProductionJobRuntime::FJob>
FProductionJobRuntime::StartProcessJob(
	const FProcessLaunchSpec& Spec,
	const TSharedPtr<FJsonObject>& Input,
	FString& OutError)
{
	OutError.Reset();
	bool bIdempotencyConflict = false;
	if (TSharedPtr<FJob> Existing =
		FindIdempotentJob(Spec.Kind, Input, bIdempotencyConflict))
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
	if (Spec.Executable.IsEmpty()
		|| (!Spec.Executable.Equals(
				TEXT("git.exe"),
				ESearchCase::IgnoreCase)
			&& !IFileManager::Get().FileExists(*Spec.Executable)))
	{
		OutError = FString::Printf(
			TEXT("Required executable '%s' is unavailable."),
			*Spec.Executable);
		return nullptr;
	}

	TSharedPtr<FJob> Job = CreateJob(Spec.Kind, Input);
	Job->Executable = Spec.Executable;
	Job->WorkingDirectory = Spec.WorkingDirectory;
	Job->PostProcess = Spec.PostProcess;
	Job->bEditorProcess =
		Spec.bEditorProcess || IsEditorExecutable(Spec.Executable);
	Job->bAutomationProcess =
		Spec.bAutomationProcess
		|| (Job->bEditorProcess && Spec.PostProcess == TEXT("test"));
	Job->HeadlessProfile =
		GetStringFieldOr(
			Input,
			TEXT("headlessProfile"),
			Job->bAutomationProcess
				? FString(TEXT("minimal"))
				: FString());
	const FString Directory = JobDirectory(Job->Id);
	IFileManager::Get().MakeDirectory(*Directory, true);
	Job->EditorLogPath =
		Job->bEditorProcess
			? FPaths::Combine(Directory, TEXT("editor.log"))
			: FString();
	Job->ReportDirectory =
		Spec.PostProcess == TEXT("test")
			? FPaths::Combine(Directory, TEXT("report"))
			: FString();
	if (!Job->ReportDirectory.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(
			*Job->ReportDirectory,
			true);
	}
	Job->Arguments =
		Spec.BuildArguments
			? Spec.BuildArguments(
				Job->Id,
				Directory,
				Job->EditorLogPath,
				Job->ReportDirectory)
			: Spec.Arguments;
	if (Job->Arguments.IsEmpty())
	{
		OutError = TEXT("The constrained job argument builder failed.");
		Job->Status = TEXT("failed");
		Job->ErrorCode = TEXT("job_start_failed");
		Job->Message = OutError;
		Job->Progress = 1.0;
		Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		TransitionProcessPhase(*Job, TEXT("complete"));
		SaveJournal(*Job);
		return nullptr;
	}
	if (Job->bEditorProcess)
	{
		if (!Job->Arguments.Contains(
				TEXT("-stdout"),
				ESearchCase::IgnoreCase))
		{
			Job->Arguments += TEXT(" -stdout");
		}
		if (!Job->Arguments.Contains(
				TEXT("-FullStdOutLogOutput"),
				ESearchCase::IgnoreCase))
		{
			Job->Arguments += TEXT(" -FullStdOutLogOutput");
		}
		if (!Job->Arguments.Contains(
				TEXT("-NoSound"),
				ESearchCase::IgnoreCase))
		{
			Job->Arguments += TEXT(" -NoSound");
		}
		if (!Job->Arguments.Contains(
				TEXT("-abslog="),
				ESearchCase::IgnoreCase))
		{
			Job->Arguments += TEXT(" -abslog=")
				+ QuoteArgument(Job->EditorLogPath);
		}
	}

	const TSharedPtr<FJsonObject> TimeoutPolicy =
		ResolveProcessTimeoutPolicy(
			Input,
			Spec.DefaultExecutionTimeoutSeconds);
	Job->StartupTimeoutSeconds =
		TimeoutPolicy->GetNumberField(TEXT("startupTimeoutSeconds"));
	Job->ExecutionTimeoutSeconds =
		TimeoutPolicy->GetNumberField(TEXT("executionTimeoutSeconds"));
	Job->ShutdownTimeoutSeconds =
		TimeoutPolicy->GetNumberField(TEXT("shutdownTimeoutSeconds"));
	Job->HardTimeoutSeconds =
		TimeoutPolicy->GetNumberField(TEXT("hardTimeoutSeconds"));
	Job->Status = TEXT("running");
	Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job->StartedAtSeconds = FPlatformTime::Seconds();
	Job->StartupDeadlineSeconds =
		Job->StartedAtSeconds + Job->StartupTimeoutSeconds;
	Job->HardDeadlineSeconds =
		Job->StartedAtSeconds + Job->HardTimeoutSeconds;
	Job->TimeoutAtSeconds = Job->HardDeadlineSeconds;
	TransitionProcessPhase(
		*Job,
		TEXT("launching"),
		Job->StartedAtSeconds);
	if (!FPlatformProcess::CreatePipe(Job->ReadPipe, Job->WritePipe))
	{
		OutError = TEXT("Failed to create the job output pipe.");
		Job->Status = TEXT("failed");
		Job->ErrorCode = TEXT("job_start_failed");
		Job->Message = OutError;
		Job->Progress = 1.0;
		Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		TransitionProcessPhase(*Job, TEXT("complete"));
		SaveJournal(*Job);
		return nullptr;
	}

	uint32 ProcessId = 0;
#if PLATFORM_WINDOWS
	Job->ProcessHandle = FPlatformProcess::CreateProc(
		*Spec.Executable,
		*Job->Arguments,
		false,
		Spec.bLaunchHidden,
		Spec.bLaunchReallyHidden,
		&ProcessId,
		0,
		Spec.WorkingDirectory.IsEmpty()
			? nullptr
			: *Spec.WorkingDirectory,
		Job->WritePipe,
		nullptr,
		Job->WritePipe);
#else
	Job->ProcessHandle = FPlatformProcess::CreateProc(
		*Spec.Executable,
		*Job->Arguments,
		false,
		Spec.bLaunchHidden,
		Spec.bLaunchReallyHidden,
		&ProcessId,
		0,
		Spec.WorkingDirectory.IsEmpty()
			? nullptr
			: *Spec.WorkingDirectory,
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
			*Spec.Kind);
		Job->Status = TEXT("failed");
		Job->ErrorCode = TEXT("job_start_failed");
		Job->Message = OutError;
		Job->Progress = 1.0;
		Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		TransitionProcessPhase(*Job, TEXT("complete"));
		SaveJournal(*Job);
		return nullptr;
	}
	Job->ProcessId = ProcessId;
	TransitionProcessPhase(
		*Job,
		Job->bAutomationProcess ? TEXT("loading") : TEXT("running"));
	ActiveHeavyJobId = Job->Id;
	SaveJournal(*Job);
	return Job;
}

void FProductionJobRuntime::TransitionProcessPhase(
	FJob& Job,
	const FString& NewPhase,
	double NowSeconds)
{
	if (NewPhase.IsEmpty() || Job.Phase == NewPhase)
	{
		return;
	}
	if (NowSeconds <= 0.0)
	{
		NowSeconds = FPlatformTime::Seconds();
	}
	if (!Job.Phase.IsEmpty() && Job.PhaseStartedAtSeconds > 0.0)
	{
		Job.PhaseDurationsSeconds.FindOrAdd(Job.Phase) +=
			FMath::Max(0.0, NowSeconds - Job.PhaseStartedAtSeconds);
	}
	Job.Phase = NewPhase;
	Job.PhaseStartedAtSeconds = NowSeconds;
	if (Job.PhaseHistory.IsEmpty()
		|| Job.PhaseHistory.Last() != NewPhase)
	{
		Job.PhaseHistory.Add(NewPhase);
	}
	if (NewPhase == TEXT("running")
		&& Job.ExecutionDeadlineSeconds <= 0.0)
	{
		Job.ExecutionDeadlineSeconds =
			NowSeconds + Job.ExecutionTimeoutSeconds;
	}
	else if (NewPhase == TEXT("exiting")
		&& Job.ShutdownDeadlineSeconds <= 0.0)
	{
		Job.ShutdownDeadlineSeconds =
			NowSeconds + Job.ShutdownTimeoutSeconds;
	}
}

void FProductionJobRuntime::UpdateProcessPhaseFromLog(
	FJob& Job,
	const FString& LogChunk)
{
	if (!Job.bAutomationProcess || LogChunk.IsEmpty())
	{
		return;
	}
	const FString Target =
		InferAutomationPhase(Job.Phase, LogChunk);
	static const TArray<FString> OrderedPhases = {
		TEXT("launching"),
		TEXT("loading"),
		TEXT("discovering"),
		TEXT("running"),
		TEXT("reporting"),
		TEXT("exiting"),
		TEXT("complete")
	};
	const int32 CurrentIndex = OrderedPhases.IndexOfByKey(Job.Phase);
	const int32 TargetIndex = OrderedPhases.IndexOfByKey(Target);
	if (CurrentIndex == INDEX_NONE
		|| TargetIndex == INDEX_NONE
		|| TargetIndex <= CurrentIndex)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	for (int32 Index = CurrentIndex + 1;
		Index <= TargetIndex;
		++Index)
	{
		TransitionProcessPhase(Job, OrderedPhases[Index], Now);
	}
	SaveJournal(Job);
}

void FProductionJobRuntime::TerminateProcessTree(FJob& Job)
{
	if (!Job.ProcessHandle.IsValid())
	{
		return;
	}
	Job.bTerminationRequested = true;
	if (FPlatformProcess::IsProcRunning(Job.ProcessHandle))
	{
		FPlatformProcess::TerminateProc(Job.ProcessHandle, true);
		FPlatformProcess::WaitForProc(Job.ProcessHandle);
	}
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
			UpdateProcessPhaseFromLog(Job, Chunk);
		}
	}

	const double Now = FPlatformTime::Seconds();
	if (Job.HardDeadlineSeconds > 0.0
		&& Now >= Job.HardDeadlineSeconds)
	{
		TerminateProcessTree(Job);
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("job_hard_timeout"),
			TEXT("The job exceeded its total hard timeout."));
		return;
	}
	if (IsStartupPhase(Job.Phase)
		&& Job.StartupDeadlineSeconds > 0.0
		&& Now >= Job.StartupDeadlineSeconds)
	{
		TerminateProcessTree(Job);
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("job_startup_timeout"),
			TEXT("The job did not reach its running phase before startupTimeoutSeconds."));
		return;
	}
	if (IsExecutionPhase(Job.Phase)
		&& Job.ExecutionDeadlineSeconds > 0.0
		&& Now >= Job.ExecutionDeadlineSeconds)
	{
		TerminateProcessTree(Job);
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("job_execution_timeout"),
			TEXT("The job exceeded executionTimeoutSeconds."));
		return;
	}
	if (Job.Phase == TEXT("exiting")
		&& Job.ShutdownDeadlineSeconds > 0.0
		&& Now >= Job.ShutdownDeadlineSeconds)
	{
		TerminateProcessTree(Job);
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("job_shutdown_timeout"),
			TEXT("The job did not exit before shutdownTimeoutSeconds."));
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
		if (IsStartupPhase(Job.Phase))
		{
			Job.Progress = FMath::Clamp(
				((Now - Job.StartedAtSeconds)
					/ Job.StartupTimeoutSeconds) * 0.2,
				0.0,
				0.2);
		}
		else if (IsExecutionPhase(Job.Phase)
			&& Job.ExecutionDeadlineSeconds > 0.0)
		{
			const double ExecutionStarted =
				Job.ExecutionDeadlineSeconds
					- Job.ExecutionTimeoutSeconds;
			Job.Progress = FMath::Clamp(
				0.2
					+ ((Now - ExecutionStarted)
						/ Job.ExecutionTimeoutSeconds) * 0.7,
				0.2,
				0.9);
		}
		else if (Job.Phase == TEXT("exiting"))
		{
			Job.Progress = 0.95;
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
	if (Job.Kind == TEXT("performance")
		&& Status != TEXT("succeeded"))
	{
		if (Job.PerformanceMode == TEXT("scenario")
			&& !Job.ScenarioRunId.IsEmpty()
			&& CancelScenario)
		{
			TSharedPtr<FJsonObject> CancelParams = MakeShared<FJsonObject>();
			CancelParams->SetStringField(TEXT("runId"), Job.ScenarioRunId);
			CancelScenario(CancelParams);
		}
		if (Job.bOwnsTrace && !Job.TraceJobId.IsEmpty())
		{
			TSharedPtr<FJsonObject> StopParams = MakeShared<FJsonObject>();
			StopParams->SetStringField(TEXT("traceId"), Job.TraceJobId);
			StopTrace(StopParams);
		}
	}
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
	Job.ErrorCode = ErrorCode;
	Job.Message = Message;
	Job.Progress = 1.0;
	Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
	if (ActiveHeavyJobId == Job.Id)
	{
		ActiveHeavyJobId.Reset();
	}
	if (Job.Phase != TEXT("reporting")
		&& Job.Phase != TEXT("exiting")
		&& Job.Phase != TEXT("complete"))
	{
		TransitionProcessPhase(Job, TEXT("reporting"));
	}
	PostProcessJob(Job);
	if (Job.Phase != TEXT("exiting")
		&& Job.Phase != TEXT("complete"))
	{
		TransitionProcessPhase(Job, TEXT("exiting"));
	}
	TransitionProcessPhase(Job, TEXT("complete"));
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
			TEXT("gpu"),
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
	if (!AddArtifact(
			Job,
			TracePath,
			TEXT("capture.utrace"),
			TEXT("application/x-unreal-trace")))
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("trace_artifact_unavailable"),
			TEXT("The trace stopped, but its .utrace artifact was not created."));
		return FMCPToolResult::Error(
			Job.Message,
			Job.ErrorCode,
			500);
	}
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
	if (!Trace
		|| !Trace->IsValid()
		|| ((*Trace)->Kind != TEXT("trace")
			&& (*Trace)->Kind != TEXT("performanceStandalone")))
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
	const FArtifact* TraceArtifact =
		(*Trace)->Artifacts.FindByPredicate(
			[](const FArtifact& Candidate)
			{
				return Candidate.MimeType
						== TEXT("application/x-unreal-trace")
					&& Candidate.Kind == TEXT("trace")
					&& Candidate.Name.EndsWith(
						TEXT(".utrace"),
						ESearchCase::IgnoreCase);
			});
	if (!TraceArtifact)
	{
		return FMCPToolResult::Error(
			TEXT("The completed trace has no registered .utrace artifact."),
			TEXT("trace_artifact_unavailable"),
			410);
	}
	const FString TracePath = TraceArtifact->Path;
	if (!IFileManager::Get().FileExists(*TracePath))
	{
		return FMCPToolResult::Error(
			TEXT("The registered .utrace artifact is no longer available."),
			TEXT("trace_artifact_unavailable"),
			410);
	}

	ITraceServicesModule* TraceServicesModule =
		FModuleManager::LoadModulePtr<ITraceServicesModule>(
			TEXT("TraceServices"));
	if (!TraceServicesModule)
	{
		return FMCPToolResult::Error(
			TEXT("TraceServices could not be loaded by this Editor build."),
			TEXT("trace_analysis_unavailable"),
			503);
	}
	const TSharedPtr<TraceServices::IAnalysisService> AnalysisService =
		TraceServicesModule->GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("TraceServices did not provide an analysis service."),
			TEXT("trace_analysis_unavailable"),
			503);
	}

	TSharedPtr<FJob> Analysis =
		CreateJob(TEXT("traceAnalysis"), Params);
	Analysis->Status = TEXT("running");
	Analysis->Phase = TEXT("analyzing");
	Analysis->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Analysis->StartedAtSeconds = FPlatformTime::Seconds();
	Analysis->TimeoutAtSeconds =
		Analysis->StartedAtSeconds
		+ ReadBoundedNumber(
			Params,
			TEXT("timeoutSeconds"),
			120.0,
			1.0,
			600.0);
	Analysis->Progress = 0.0;
	Analysis->Result->SetStringField(TEXT("traceId"), TraceId);
	Analysis->Result->SetStringField(
		TEXT("traceSourceKind"),
		(*Trace)->Kind);
	Analysis->Result->SetStringField(TEXT("analysisLevel"), TEXT("providers"));
	Analysis->Result->SetBoolField(TEXT("traceServicesAvailable"), true);
	Analysis->Result->SetObjectField(
		TEXT("traceArtifact"),
		MakeArtifactSummary(*TraceArtifact));
	Analysis->TraceAnalysisSession =
		AnalysisService->StartAnalysis(*TracePath);
	if (!Analysis->TraceAnalysisSession.IsValid())
	{
		FinishJob(
			*Analysis,
			TEXT("failed"),
			TEXT("trace_analysis_unavailable"),
			TEXT("TraceServices rejected the .utrace artifact."));
		return FMCPToolResult::Error(
			Analysis->Message,
			Analysis->ErrorCode,
			500);
	}
	ActiveHeavyJobId = Analysis->Id;
	SaveJournal(*Analysis);
	return FMCPToolResult::Ok(MakeJobSummary(*Analysis, false));
}

void FProductionJobRuntime::TickTraceAnalysisJob(FJob& Job)
{
	if (!Job.TraceAnalysisSession.IsValid())
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("trace_analysis_unavailable"),
			TEXT("The TraceServices analysis session was lost."));
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (Job.TimeoutAtSeconds > 0.0 && Now >= Job.TimeoutAtSeconds)
	{
		Job.TraceAnalysisSession->Stop(true);
		Job.TraceAnalysisSession.Reset();
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("trace_analysis_timeout"),
			TEXT("TraceServices analysis exceeded its bounded timeout."));
		return;
	}
	if (!Job.TraceAnalysisSession->IsAnalysisComplete())
	{
		const double Span =
			FMath::Max(Job.TimeoutAtSeconds - Job.StartedAtSeconds, 0.001);
		Job.Progress = FMath::Clamp(
			(Now - Job.StartedAtSeconds) / Span,
			0.0,
			0.95);
		return;
	}

	Job.Result->SetObjectField(
		TEXT("analysis"),
		BuildTraceProviderSummary(
			*Job.TraceAnalysisSession,
			Job.Input));
	Job.TraceAnalysisSession.Reset();
	if (!WriteTraceAnalysisReport(Job))
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("artifact_write_failed"),
			TEXT("Trace analysis completed, but its bounded report artifact could not be written."));
		return;
	}
	FinishJob(Job, TEXT("succeeded"));
}

FMCPToolResult FProductionJobRuntime::StartPerformanceRun(
	const TSharedPtr<FJsonObject>& Params)
{
	if (GetStringFieldOr(
			Params,
			TEXT("executionTarget"),
			TEXT("pie"))
		== TEXT("standalone"))
	{
		return StartStandalonePerformanceRun(Params);
	}
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

	const FString Mode =
		GetStringFieldOr(Params, TEXT("mode"), TEXT("window"));
	if (Mode != TEXT("window") && Mode != TEXT("scenario"))
	{
		return FMCPToolResult::Error(
			TEXT("mode must be either 'window' or 'scenario'."),
			TEXT("invalid_params"),
			422);
	}
	if (Mode == TEXT("scenario")
		&& (!Params.IsValid()
			|| !Params->HasTypedField<EJson::Object>(TEXT("scenario"))))
	{
		return FMCPToolResult::Error(
			TEXT("scenario mode requires a scenario object."),
			TEXT("invalid_params"),
			422);
	}
	if (Mode == TEXT("scenario"))
	{
		const TSharedPtr<FJsonObject> Scenario =
			Params->GetObjectField(TEXT("scenario"));
		int32 BeginIndex = INDEX_NONE;
		int32 EndIndex = INDEX_NONE;
		int32 BeginCount = 0;
		int32 EndCount = 0;
		if (Scenario->HasTypedField<EJson::Array>(TEXT("steps")))
		{
			const TArray<TSharedPtr<FJsonValue>>& Steps =
				Scenario->GetArrayField(TEXT("steps"));
			for (int32 Index = 0; Index < Steps.Num(); ++Index)
			{
				if (!Steps[Index].IsValid()
					|| Steps[Index]->Type != EJson::Object)
				{
					continue;
				}
				const FString Action =
					GetStringFieldOr(
						Steps[Index]->AsObject(),
						TEXT("action"));
				if (Action == TEXT("metrics.begin"))
				{
					++BeginCount;
					BeginIndex = Index;
				}
				else if (Action == TEXT("metrics.end"))
				{
					++EndCount;
					EndIndex = Index;
				}
			}
		}
		if (BeginCount != 1
			|| EndCount != 1
			|| BeginIndex >= EndIndex)
		{
			return FMCPToolResult::Error(
				TEXT("scenario mode requires exactly one metrics.begin followed by one metrics.end step."),
				TEXT("invalid_params"),
				422);
		}
	}
	if (Mode == TEXT("scenario")
		&& (!StartScenario
			|| !GetScenarioStatus
			|| !GetScenarioResult
			|| !CancelScenario))
	{
		return FMCPToolResult::Error(
			TEXT("Scenario orchestration is unavailable in this runtime."),
			TEXT("scenario_unavailable"),
			503);
	}

	const double WarmupSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("warmupSeconds"), 2.0),
		0.0,
		300.0);
	const double SampleSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("sampleSeconds"), 10.0),
		0.1,
		3600.0);
	const int32 RepeatCount = FMath::Clamp(
		static_cast<int32>(
			GetNumberFieldOr(Params, TEXT("repeatCount"), 1.0)),
		1,
		20);
	TSharedPtr<FJob> Job = CreateJob(TEXT("performance"), Params);
	Job->Status = TEXT("running");
	Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	Job->StartedAtSeconds = FPlatformTime::Seconds();
	Job->PerformanceMode = Mode;
	Job->RepeatCount = RepeatCount;
	Job->WarmupSeconds = WarmupSeconds;
	Job->SampleSeconds = SampleSeconds;
	Job->BudgetMs = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("budgetMs"), 16.6667),
		0.01,
		10000.0);
	Job->Result->SetObjectField(TEXT("context"), MakeRuntimeContext());
	Job->Result->SetStringField(TEXT("mode"), Mode);
	Job->Result->SetNumberField(TEXT("repeatCount"), RepeatCount);
	Job->Result->SetNumberField(TEXT("warmupSeconds"), WarmupSeconds);
	Job->Result->SetNumberField(TEXT("sampleSeconds"), SampleSeconds);
	Job->Result->SetNumberField(TEXT("budgetMs"), Job->BudgetMs);

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
			FinishJob(
				*Job,
				TEXT("failed"),
				TraceResult.ErrorCode.IsEmpty()
					? TEXT("trace_capture_failed")
					: TraceResult.ErrorCode,
				TraceResult.ErrorMessage.IsEmpty()
					? TEXT("Automatic trace capture could not be started.")
					: TraceResult.ErrorMessage);
			return FMCPToolResult::Error(
				Job->Message,
				Job->ErrorCode,
				TraceResult.HttpStatus);
		}
	}
	if (Mode == TEXT("window"))
	{
		BeginWindowIteration(*Job, Job->StartedAtSeconds);
		Job->TimeoutAtSeconds =
			Job->StartedAtSeconds
			+ RepeatCount * (WarmupSeconds + SampleSeconds)
			+ 30.0;
	}
	else
	{
		const TSharedPtr<FJsonObject> Scenario =
			Params->GetObjectField(TEXT("scenario"));
		const double ScenarioTimeoutSeconds =
			Scenario->HasField(TEXT("timeoutMs"))
				? FMath::Clamp(
					Scenario->GetNumberField(TEXT("timeoutMs")) / 1000.0,
					0.001,
					3600.0)
				: 300.0;
		Job->TimeoutAtSeconds =
			Job->StartedAtSeconds
			+ FMath::Min(
				86400.0,
				RepeatCount * ScenarioTimeoutSeconds + 60.0);
		FString ScenarioError;
		if (!StartScenarioIteration(*Job, ScenarioError))
		{
			if (Job->bOwnsTrace && !Job->TraceJobId.IsEmpty())
			{
				TSharedPtr<FJsonObject> StopParams = MakeShared<FJsonObject>();
				StopParams->SetStringField(TEXT("traceId"), Job->TraceJobId);
				StopTrace(StopParams);
			}
			FinishJob(
				*Job,
				TEXT("failed"),
				TEXT("scenario_start_failed"),
				ScenarioError);
			return FMCPToolResult::Error(
				ScenarioError,
				TEXT("scenario_start_failed"),
				422);
		}
	}
	ActiveHeavyJobId = Job->Id;
	SaveJournal(*Job);
	TSharedPtr<FJsonObject> Result = MakeJobSummary(*Job, false);
	Result->SetStringField(TEXT("runId"), Job->Id);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FProductionJobRuntime::StartStandalonePerformanceRun(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bIdempotencyConflict = false;
	if (TSharedPtr<FJob> Existing =
		FindIdempotentJob(
			TEXT("performanceStandalone"),
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
			TEXT(
				"requestId is already bound to a different durable "
				"standalone performance request."),
			TEXT("idempotency_conflict"),
			409);
	}
	if (!ActiveHeavyJobId.IsEmpty())
	{
		const TSharedPtr<FJob>* Active = Jobs.Find(ActiveHeavyJobId);
		if (Active && Active->IsValid()
			&& (*Active)->Status == TEXT("running"))
		{
			return FMCPToolResult::Error(
				TEXT(
					"Another production job holds the runtime resource "
					"lock."),
				TEXT("job_conflict"),
				409);
		}
		ActiveHeavyJobId.Reset();
	}
	if (!Params.IsValid()
		|| !Params->HasTypedField<EJson::Object>(TEXT("standardProfile")))
	{
		return FMCPToolResult::Error(
			TEXT(
				"Standalone performance runs require a fixed "
				"standardProfile."),
			TEXT("invalid_params"),
			422);
	}
	const TSharedPtr<FJsonObject> Standard =
		Params->GetObjectField(TEXT("standardProfile"));
	if (GetStringFieldOr(Params, TEXT("mode"), TEXT("scenario"))
			!= TEXT("scenario"))
	{
		return FMCPToolResult::Error(
			TEXT(
				"Standalone performance requires mode='scenario' and a "
				"fixed standardProfile."),
			TEXT("invalid_params"),
			422);
	}
	if (Standard->HasTypedField<EJson::Array>(TEXT("inputSteps"))
		&& !Standard->GetArrayField(TEXT("inputSteps")).IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT(
				"Standalone inputSteps are not yet supported; use a "
				"deterministic map/camera profile without input steps."),
			TEXT("invalid_params"),
			422);
	}
	const FString Map = GetStringFieldOr(Standard, TEXT("map"));
	if (!Map.StartsWith(TEXT("/Game/")))
	{
		return FMCPToolResult::Error(
			TEXT("standardProfile.map must be a /Game package path."),
			TEXT("invalid_params"),
			422);
	}
	FString MapFilename;
	if (!FPackageName::DoesPackageExist(Map, &MapFilename))
	{
		return FMCPToolResult::Error(
			TEXT("The fixed standalone map package does not exist."),
			TEXT("asset_not_found"),
			404);
	}

	const FString Executable = GetEditorStandaloneExecutable();
	if (!IFileManager::Get().FileExists(*Executable))
	{
		return FMCPToolResult::Error(
			TEXT("The Unreal Editor standalone executable is unavailable."),
			TEXT("build_unavailable"),
			503);
	}
	const double WarmupSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("warmupSeconds"), 5.0),
		0.0,
		300.0);
	const double SampleSeconds = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("sampleSeconds"), 10.0),
		0.1,
		3600.0);
	const int32 RepeatCount = FMath::Clamp(
		static_cast<int32>(
			GetNumberFieldOr(Params, TEXT("repeatCount"), 5.0)),
		1,
		20);
	const double BudgetMs = FMath::Clamp(
		GetNumberFieldOr(Params, TEXT("budgetMs"), 16.6667),
		0.01,
		10000.0);
	const bool bCaptureTrace =
		GetBoolFieldOr(Params, TEXT("captureTrace"), true);
	const FString CVarCommands = BuildStandaloneCVarCommands();
	const int32 ResolutionX = FMath::Max(1, GSystemResolution.ResX);
	const int32 ResolutionY = FMath::Max(1, GSystemResolution.ResY);
	FString RhiArgument;
	if (GDynamicRHI)
	{
		const FString RhiName = FString(GDynamicRHI->GetName());
		if (RhiName.Contains(TEXT("D3D12"), ESearchCase::IgnoreCase))
		{
			RhiArgument = TEXT("-dx12");
		}
		else if (RhiName.Contains(TEXT("D3D11"), ESearchCase::IgnoreCase))
		{
			RhiArgument = TEXT("-dx11");
		}
	}

	if (!Standard->HasTypedField<EJson::Object>(TEXT("camera")))
	{
		return FMCPToolResult::Error(
			TEXT(
				"Standalone performance requires a named camera with "
				"declared location and rotation."),
			TEXT("invalid_params"),
			422);
	}
	const TSharedPtr<FJsonObject> Camera =
		Standard->GetObjectField(TEXT("camera"));
	const FString CameraName = GetStringFieldOr(Camera, TEXT("name"));
	const FString GameInstanceMode =
		GetStringFieldOr(Standard, TEXT("gameInstanceMode"), TEXT("project"));
	if (GameInstanceMode != TEXT("project")
		&& GameInstanceMode != TEXT("minimal"))
	{
		return FMCPToolResult::Error(
			TEXT(
				"standardProfile.gameInstanceMode must be 'project' or "
				"'minimal'."),
			TEXT("invalid_params"),
			422);
	}
	if (CameraName.IsEmpty()
		|| CameraName.Len() > 128
		|| !IsSafeLegacyArgumentString(CameraName)
		|| !Camera->HasTypedField<EJson::Array>(TEXT("location"))
		|| !Camera->HasTypedField<EJson::Array>(TEXT("rotation"))
		|| Camera->GetArrayField(TEXT("location")).Num() != 3
		|| Camera->GetArrayField(TEXT("rotation")).Num() != 3)
	{
		return FMCPToolResult::Error(
			TEXT(
				"standardProfile.camera must provide a bounded name and "
				"three-number location/rotation arrays."),
			TEXT("invalid_params"),
			422);
	}
	const TArray<TSharedPtr<FJsonValue>>& CameraLocation =
		Camera->GetArrayField(TEXT("location"));
	const TArray<TSharedPtr<FJsonValue>>& CameraRotation =
		Camera->GetArrayField(TEXT("rotation"));
	const TArray<TSharedPtr<FJsonValue>> CameraValues = {
		CameraLocation[0],
		CameraLocation[1],
		CameraLocation[2],
		CameraRotation[0],
		CameraRotation[1],
		CameraRotation[2]
	};
	for (const TSharedPtr<FJsonValue>& Value : CameraValues)
	{
		if (!Value.IsValid()
			|| Value->Type != EJson::Number
			|| !FMath::IsFinite(Value->AsNumber()))
		{
			return FMCPToolResult::Error(
				TEXT(
					"standardProfile.camera transform values must be "
					"finite numbers."),
				TEXT("invalid_params"),
				422);
		}
	}

	TSharedPtr<FJsonObject> Input = CopyObject(Params);
	Input->SetStringField(TEXT("executionTarget"), TEXT("standalone"));
	Input->SetStringField(TEXT("headlessProfile"), TEXT("rendering"));
	Input->SetNumberField(TEXT("warmupSeconds"), WarmupSeconds);
	Input->SetNumberField(TEXT("sampleSeconds"), SampleSeconds);
	Input->SetNumberField(TEXT("repeatCount"), RepeatCount);
	Input->SetNumberField(TEXT("budgetMs"), BudgetMs);
	Input->SetBoolField(TEXT("captureTrace"), bCaptureTrace);
	TSharedPtr<FJsonObject> LaunchConfiguration = MakeShared<FJsonObject>();
	LaunchConfiguration->SetNumberField(TEXT("resolutionX"), ResolutionX);
	LaunchConfiguration->SetNumberField(TEXT("resolutionY"), ResolutionY);
	LaunchConfiguration->SetStringField(TEXT("rhiArgument"), RhiArgument);
	LaunchConfiguration->SetStringField(
		TEXT("cvarCommands"),
		CVarCommands);
	LaunchConfiguration->SetStringField(
		TEXT("windowMode"),
		TEXT("visibleWindow"));
	LaunchConfiguration->SetStringField(
		TEXT("cameraName"),
		CameraName);
	LaunchConfiguration->SetStringField(
		TEXT("gameInstanceMode"),
		GameInstanceMode);
	Input->SetObjectField(
		TEXT("standaloneLaunchConfiguration"),
		LaunchConfiguration);

	const FString ProjectFile =
		FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	FProcessLaunchSpec Spec;
	Spec.Kind = TEXT("performanceStandalone");
	Spec.Executable = Executable;
	Spec.WorkingDirectory = FPaths::ProjectDir();
	Spec.PostProcess = TEXT("performanceStandalone");
	Spec.DefaultExecutionTimeoutSeconds =
		FMath::Clamp(
			RepeatCount * (WarmupSeconds + SampleSeconds) + 300.0,
			60.0,
			21600.0);
	const double ChildStartupTimeoutSeconds =
		ResolveProcessTimeoutPolicy(
			Input,
			Spec.DefaultExecutionTimeoutSeconds)
			->GetNumberField(TEXT("startupTimeoutSeconds"));
	LaunchConfiguration->SetNumberField(
		TEXT("startupTimeoutSeconds"),
		ChildStartupTimeoutSeconds);
	Spec.bEditorProcess = true;
	Spec.bLaunchHidden = false;
	Spec.bLaunchReallyHidden = false;
	Spec.BuildArguments =
		[ProjectFile,
			Map,
			CameraName,
			CameraLocation,
			CameraRotation,
			CVarCommands,
			ResolutionX,
			ResolutionY,
			RhiArgument,
			WarmupSeconds,
			SampleSeconds,
			RepeatCount,
			ChildStartupTimeoutSeconds,
			GameInstanceMode,
			bCaptureTrace](
			const FString& JobId,
			const FString& JobDirectory,
			const FString&,
			const FString&)
		{
			FString Arguments = FString::Printf(
				TEXT(
					"%s %s -game -unattended -nop4 -nosplash "
					"-NoSound -windowed -ForceRes -ResX=%d -ResY=%d %s "
					"-csvCompression=0 "
					"-DisablePlugins=OpenXR,OpenXREyeTracker,"
					"OpenXRHandTracking,OpenXRMsftHandInteraction "
					"-UEAIPerformanceChild "
					"-UEAIPerfJob=%s -UEAIPerfJobDirectory=%s "
					"-UEAIPerfCameraName=%s "
					"-UEAIPerfGameInstanceMode=%s "
					"-UEAIPerfCameraX=%.17g -UEAIPerfCameraY=%.17g "
					"-UEAIPerfCameraZ=%.17g "
					"-UEAIPerfCameraPitch=%.17g "
					"-UEAIPerfCameraYaw=%.17g "
					"-UEAIPerfCameraRoll=%.17g "
					"-UEAIPerfWarmupSeconds=%.17g "
					"-UEAIPerfSampleSeconds=%.17g "
					"-UEAIPerfRepeatCount=%d"),
				*QuoteArgument(ProjectFile),
				*QuoteArgument(Map),
				ResolutionX,
				ResolutionY,
				*RhiArgument,
				*QuoteArgument(JobId),
				*QuoteArgument(JobDirectory),
				*QuoteArgument(CameraName),
				*QuoteArgument(GameInstanceMode),
				CameraLocation[0]->AsNumber(),
				CameraLocation[1]->AsNumber(),
				CameraLocation[2]->AsNumber(),
				CameraRotation[0]->AsNumber(),
				CameraRotation[1]->AsNumber(),
				CameraRotation[2]->AsNumber(),
				WarmupSeconds,
				SampleSeconds,
				RepeatCount);
			Arguments += TEXT(" ")
				+ BuildStandaloneStartupTimeoutArgument(
					ChildStartupTimeoutSeconds);
			if (GameInstanceMode == TEXT("minimal"))
			{
				Arguments +=
					TEXT(
						" -ini:Engine:[/Script/EngineSettings."
						"GameMapsSettings]:GameInstanceClass="
						"/Script/Engine.GameInstance");
			}
			if (!CVarCommands.IsEmpty())
			{
				Arguments +=
					TEXT(" -ExecCmds=") + QuoteArgument(CVarCommands);
			}
			if (bCaptureTrace)
			{
				Arguments += TEXT(" -UEAIPerfCaptureTrace");
			}
			return Arguments;
		};
	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(Spec, Input, Error);
	if (!Job.IsValid())
	{
		return MakeJobStartFailure(Error);
	}
	Job->Result->SetStringField(
		TEXT("executionTarget"),
		TEXT("standalone"));
	Job->Result->SetStringField(TEXT("mode"), TEXT("scenario"));
	Job->Result->SetNumberField(TEXT("repeatCount"), RepeatCount);
	Job->Result->SetNumberField(TEXT("warmupSeconds"), WarmupSeconds);
	Job->Result->SetNumberField(TEXT("sampleSeconds"), SampleSeconds);
	Job->Result->SetNumberField(
		TEXT("budgetMs"),
		BudgetMs);
	Job->Result->SetStringField(TEXT("map"), Map);
	Job->Result->SetStringField(
		TEXT("samplingBasis"),
		TEXT("standaloneChildWallClockCsvWindows"));
	Job->Result->SetStringField(
		TEXT("renderLaunchPolicy"),
		TEXT("visibleWindow"));
	Job->Result->SetStringField(
		TEXT("gameInstanceMode"),
		GameInstanceMode);
	Job->Result->SetBoolField(
		TEXT("traceRequested"),
		bCaptureTrace);
	SaveJournal(*Job);
	TSharedPtr<FJsonObject> Result = MakeJobSummary(*Job, false);
	Result->SetStringField(TEXT("runId"), Job->Id);
	return FMCPToolResult::Ok(Result);
}

void FProductionJobRuntime::BeginWindowIteration(
	FJob& Job,
	const double Now)
{
	Job.MetricSamples.Reset();
	Job.InvalidMetricSampleCounts.Reset();
	Job.PendingIterationResult.Reset();
	Job.WarmupUntilSeconds = Now + Job.WarmupSeconds;
	Job.SamplingUntilSeconds =
		Job.WarmupUntilSeconds + Job.SampleSeconds;
	Job.Phase =
		Job.WarmupSeconds > 0.0 ? TEXT("warmup") : TEXT("sampling");
}

void FProductionJobRuntime::SamplePerformanceFrame(FJob& Job)
{
	auto AddSample =
		[&Job](const FString& MetricName, const double Value)
		{
			if (IsPerformanceSampleValid(Value))
			{
				Job.MetricSamples.FindOrAdd(MetricName).Add(Value);
			}
			else
			{
				++Job.InvalidMetricSampleCounts.FindOrAdd(MetricName);
			}
		};
	AddSample(
		TEXT("frameMs"),
		FApp::GetDeltaTime() * 1000.0);
	AddSample(
		TEXT("gameMs"),
		FPlatformTime::ToMilliseconds64(GGameThreadTime));
	AddSample(
		TEXT("renderMs"),
		FPlatformTime::ToMilliseconds64(GRenderThreadTime));
	AddSample(
		TEXT("rhiMs"),
		FPlatformTime::ToMilliseconds64(GRHIThreadTime));
	if (GDynamicRHI)
	{
		AddSample(
			TEXT("gpuMs"),
			FPlatformTime::ToMilliseconds64(RHIGetGPUFrameCycles()));
	}
}

void FProductionJobRuntime::CompletePerformanceIteration(FJob& Job)
{
	if (Job.PendingIterationResult.IsValid())
	{
		return;
	}
	static const TArray<FString> MetricNames = {
		TEXT("frameMs"),
		TEXT("gameMs"),
		TEXT("renderMs"),
		TEXT("rhiMs"),
		TEXT("gpuMs")
	};
	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	for (const FString& MetricName : MetricNames)
	{
		const TArray<double>* Samples = Job.MetricSamples.Find(MetricName);
		TSharedPtr<FJsonObject> Summary =
			SummarizeMetric(
				Samples ? *Samples : TArray<double>(),
				Job.BudgetMs);
		const int32 InvalidCount =
			Job.InvalidMetricSampleCounts.FindRef(MetricName);
		Summary->SetNumberField(
			TEXT("inputSampleCount"),
			(Samples ? Samples->Num() : 0) + InvalidCount);
		Summary->SetNumberField(
			TEXT("invalidSampleCount"),
			InvalidCount);
		Metrics->SetObjectField(MetricName, Summary);
		if (Samples)
		{
			Job.AggregateMetricSamples.FindOrAdd(MetricName).Append(*Samples);
		}
		Job.AggregateInvalidMetricSampleCounts.FindOrAdd(MetricName) +=
			InvalidCount;
	}
	Job.PendingIterationResult = MakeShared<FJsonObject>();
	Job.PendingIterationResult->SetNumberField(
		TEXT("repeatIndex"),
		Job.RepeatIndex + 1);
	Job.PendingIterationResult->SetNumberField(
		TEXT("sampleCount"),
		Job.MetricSamples.FindRef(TEXT("frameMs")).Num());
	Job.PendingIterationResult->SetNumberField(
		TEXT("invalidFrameSampleCount"),
		Job.InvalidMetricSampleCounts.FindRef(TEXT("frameMs")));
	Job.PendingIterationResult->SetObjectField(TEXT("metrics"), Metrics);
	if (!Job.ScenarioRunId.IsEmpty())
	{
		Job.PendingIterationResult->SetStringField(
			TEXT("scenarioRunId"),
			Job.ScenarioRunId);
	}
}

bool FProductionJobRuntime::StartScenarioIteration(
	FJob& Job,
	FString& OutError)
{
	OutError.Reset();
	if (!StartScenario
		|| !Job.Input.IsValid()
		|| !Job.Input->HasTypedField<EJson::Object>(TEXT("scenario")))
	{
		OutError = TEXT("Scenario orchestration is unavailable.");
		return false;
	}
	TSharedPtr<FJsonObject> ScenarioParams = MakeShared<FJsonObject>();
	ScenarioParams->SetObjectField(
		TEXT("scenario"),
		CopyObject(Job.Input->GetObjectField(TEXT("scenario"))));
	const FMCPToolResult ScenarioStart = StartScenario(ScenarioParams);
	if (!ScenarioStart.bSuccess
		|| !ScenarioStart.Data.IsValid()
		|| !ScenarioStart.Data->TryGetStringField(
			TEXT("runId"),
			Job.ScenarioRunId))
	{
		OutError = ScenarioStart.ErrorMessage.IsEmpty()
			? TEXT("Scenario did not return a runId.")
			: ScenarioStart.ErrorMessage;
		Job.ScenarioRunId.Reset();
		return false;
	}
	Job.MetricSamples.Reset();
	Job.InvalidMetricSampleCounts.Reset();
	Job.PendingIterationResult.Reset();
	Job.bScenarioMetricsWasActive = false;
	Job.bScenarioMetricsObserved = false;
	Job.WarmupUntilSeconds = 0.0;
	Job.Phase = TEXT("scenario");
	return true;
}

void FProductionJobRuntime::FinishPerformanceRun(FJob& Job)
{
	static const TArray<FString> MetricNames = {
		TEXT("frameMs"),
		TEXT("gameMs"),
		TEXT("renderMs"),
		TEXT("rhiMs"),
		TEXT("gpuMs")
	};
	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	for (const FString& MetricName : MetricNames)
	{
		const TArray<double>& Samples =
			Job.AggregateMetricSamples.FindRef(MetricName);
		const int32 InvalidCount =
			Job.AggregateInvalidMetricSampleCounts.FindRef(MetricName);
		TSharedPtr<FJsonObject> Summary =
			SummarizeMetric(Samples, Job.BudgetMs);
		Summary->SetNumberField(
			TEXT("inputSampleCount"),
			Samples.Num() + InvalidCount);
		Summary->SetNumberField(
			TEXT("invalidSampleCount"),
			InvalidCount);
		Metrics->SetObjectField(MetricName, Summary);
	}
	Job.Result->SetObjectField(TEXT("metrics"), Metrics);
	Job.Result->SetArrayField(TEXT("repetitions"), Job.Repetitions);
	TArray<TSharedPtr<FJsonValue>> CapturedLogWindows;
	for (const TSharedPtr<FJsonValue>& RepetitionValue : Job.Repetitions)
	{
		if (!RepetitionValue.IsValid()
			|| RepetitionValue->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Repetition =
			RepetitionValue->AsObject();
		if (!Repetition->HasTypedField<EJson::Object>(TEXT("scenario")))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Scenario =
			Repetition->GetObjectField(TEXT("scenario"));
		if (!Scenario->HasTypedField<EJson::Object>(TEXT("logWindow")))
		{
			continue;
		}
		TSharedPtr<FJsonObject> LogWindow =
			CopyObject(Scenario->GetObjectField(TEXT("logWindow")));
		LogWindow->SetNumberField(
			TEXT("repeatIndex"),
			GetNumberFieldOr(Repetition, TEXT("repeatIndex"), 0.0));
		LogWindow->SetStringField(
			TEXT("scenarioRunId"),
			GetStringFieldOr(Scenario, TEXT("runId")));
		CapturedLogWindows.Add(MakeShared<FJsonValueObject>(LogWindow));
	}
	Job.Result->SetArrayField(TEXT("logWindows"), CapturedLogWindows);
	Job.Result->SetNumberField(
		TEXT("completedRepeatCount"),
		Job.Repetitions.Num());
	Job.Result->SetNumberField(
		TEXT("sampleCount"),
		Job.AggregateMetricSamples.FindRef(TEXT("frameMs")).Num());
	Job.Result->SetNumberField(
		TEXT("invalidFrameSampleCount"),
		Job.AggregateInvalidMetricSampleCounts.FindRef(TEXT("frameMs")));
	if (Job.bOwnsTrace && !Job.TraceJobId.IsEmpty())
	{
		TSharedPtr<FJsonObject> StopParams = MakeShared<FJsonObject>();
		StopParams->SetStringField(TEXT("traceId"), Job.TraceJobId);
		const FMCPToolResult TraceStop = StopTrace(StopParams);
		Job.Result->SetStringField(TEXT("traceId"), Job.TraceJobId);
		Job.Result->SetBoolField(TEXT("traceCompleted"), TraceStop.bSuccess);
		if (TraceStop.bSuccess
			&& TraceStop.Data.IsValid()
			&& TraceStop.Data->HasTypedField<EJson::Array>(TEXT("artifacts")))
		{
			Job.Result->SetArrayField(
				TEXT("traceArtifacts"),
				TraceStop.Data->GetArrayField(TEXT("artifacts")));
		}
		const bool bTraceArtifactAvailable =
			TraceStop.bSuccess
			&& TraceStop.Data.IsValid()
			&& TraceStop.Data->HasTypedField<EJson::Array>(
				TEXT("artifacts"))
			&& !TraceStop.Data->GetArrayField(TEXT("artifacts")).IsEmpty();
		if (!bTraceArtifactAvailable)
		{
			WritePerformanceReport(Job);
			FinishJob(
				Job,
				TEXT("failed"),
				TraceStop.ErrorCode.IsEmpty()
					? TEXT("trace_artifact_unavailable")
					: TraceStop.ErrorCode,
				TraceStop.ErrorMessage.IsEmpty()
					? TEXT("Automatic trace capture completed without a registered artifact.")
					: TraceStop.ErrorMessage);
			return;
		}
	}
	if (!WritePerformanceReport(Job))
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("artifact_write_failed"),
			TEXT("Performance sampling completed, but its report artifact could not be written."));
		return;
	}
	FinishJob(Job, TEXT("succeeded"));
}

void FProductionJobRuntime::TickPerformanceJob(FJob& Job)
{
	const double Now = FPlatformTime::Seconds();
	if (Job.TimeoutAtSeconds > 0.0 && Now >= Job.TimeoutAtSeconds)
	{
		if (Job.PerformanceMode == TEXT("scenario")
			&& !Job.ScenarioRunId.IsEmpty()
			&& CancelScenario)
		{
			TSharedPtr<FJsonObject> CancelParams = MakeShared<FJsonObject>();
			CancelParams->SetStringField(TEXT("runId"), Job.ScenarioRunId);
			CancelScenario(CancelParams);
		}
		if (Job.bOwnsTrace && !Job.TraceJobId.IsEmpty())
		{
			TSharedPtr<FJsonObject> StopParams = MakeShared<FJsonObject>();
			StopParams->SetStringField(TEXT("traceId"), Job.TraceJobId);
			StopTrace(StopParams);
		}
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("performance_timeout"),
			TEXT("The performance run exceeded its bounded timeout."));
		return;
	}

	if (Job.PerformanceMode == TEXT("scenario"))
	{
		TSharedPtr<FJsonObject> StatusParams = MakeShared<FJsonObject>();
		StatusParams->SetStringField(TEXT("runId"), Job.ScenarioRunId);
		const FMCPToolResult ScenarioStatus =
			GetScenarioStatus(StatusParams);
		if (!ScenarioStatus.bSuccess || !ScenarioStatus.Data.IsValid())
		{
			FinishJob(
				Job,
				TEXT("failed"),
				TEXT("scenario_status_failed"),
				ScenarioStatus.ErrorMessage.IsEmpty()
					? TEXT("Scenario status became unavailable.")
					: ScenarioStatus.ErrorMessage);
			return;
		}

		bool bMetricsActive = false;
		int32 MetricsBeginCount = 0;
		int32 MetricsEndCount = 0;
		if (ScenarioStatus.Data->HasTypedField<EJson::Object>(
				TEXT("metrics")))
		{
			const TSharedPtr<FJsonObject> MetricsState =
				ScenarioStatus.Data->GetObjectField(TEXT("metrics"));
			bMetricsActive =
				GetBoolFieldOr(MetricsState, TEXT("active"), false);
			MetricsBeginCount = static_cast<int32>(
				GetNumberFieldOr(
					MetricsState,
					TEXT("beginCount"),
					0.0));
			MetricsEndCount = static_cast<int32>(
				GetNumberFieldOr(
					MetricsState,
					TEXT("endCount"),
					0.0));
		}
		if (bMetricsActive && !Job.bScenarioMetricsWasActive)
		{
			Job.MetricSamples.Reset();
			Job.PendingIterationResult.Reset();
			Job.bScenarioMetricsObserved = true;
			Job.WarmupUntilSeconds = Now + Job.WarmupSeconds;
			Job.SamplingUntilSeconds =
				Job.WarmupUntilSeconds + Job.SampleSeconds;
			Job.Phase =
				Job.WarmupSeconds > 0.0
					? TEXT("scenarioWarmup")
					: TEXT("scenarioSampling");
		}
		if (bMetricsActive)
		{
			if (Now >= Job.WarmupUntilSeconds
				&& Now < Job.SamplingUntilSeconds)
			{
				Job.Phase = TEXT("scenarioSampling");
				SamplePerformanceFrame(Job);
			}
			else if (Now >= Job.SamplingUntilSeconds)
			{
				Job.Phase = TEXT("scenarioSampleComplete");
			}
		}
		else if (Job.bScenarioMetricsWasActive)
		{
			CompletePerformanceIteration(Job);
			Job.Phase = TEXT("scenarioFinalizing");
		}
		Job.bScenarioMetricsWasActive = bMetricsActive;

		const FString ScenarioState =
			GetStringFieldOr(
				ScenarioStatus.Data,
				TEXT("status"),
				TEXT("running"));
		const double ScenarioProgress =
			GetNumberFieldOr(
				ScenarioStatus.Data,
				TEXT("stepCount"),
				0.0) > 0.0
				? GetNumberFieldOr(
					ScenarioStatus.Data,
					TEXT("currentStep"),
					0.0)
					/ GetNumberFieldOr(
						ScenarioStatus.Data,
						TEXT("stepCount"),
						1.0)
				: 0.0;
		Job.Progress = FMath::Clamp(
			(Job.RepeatIndex + ScenarioProgress) / Job.RepeatCount,
			0.0,
			0.95);
		if (ScenarioState == TEXT("running"))
		{
			return;
		}
		if (ScenarioState != TEXT("succeeded"))
		{
			FinishJob(
				Job,
				TEXT("failed"),
				TEXT("scenario_failed"),
				TEXT("A measured scenario repetition did not succeed."));
			return;
		}
		if (!Job.bScenarioMetricsObserved
			|| MetricsBeginCount != 1
			|| MetricsEndCount != 1
			|| !Job.PendingIterationResult.IsValid()
			|| GetNumberFieldOr(
				Job.PendingIterationResult,
				TEXT("sampleCount"),
				0.0) < 1.0)
		{
			FinishJob(
				Job,
				TEXT("failed"),
				TEXT("performance_markers_invalid"),
				TEXT("Scenario mode requires exactly one completed metrics.begin/metrics.end window with at least one sampled frame."));
			return;
		}

		const FMCPToolResult ScenarioResult =
			GetScenarioResult(StatusParams);
		if (!ScenarioResult.bSuccess || !ScenarioResult.Data.IsValid())
		{
			FinishJob(
				Job,
				TEXT("failed"),
				TEXT("scenario_result_failed"),
				ScenarioResult.ErrorMessage.IsEmpty()
					? TEXT("Scenario result became unavailable.")
					: ScenarioResult.ErrorMessage);
			return;
		}
		TSharedPtr<FJsonObject> ScenarioEvidence =
			MakeShared<FJsonObject>();
		ScenarioEvidence->SetStringField(
			TEXT("runId"),
			Job.ScenarioRunId);
		ScenarioEvidence->SetStringField(
			TEXT("status"),
			ScenarioState);
		if (ScenarioResult.Data->HasTypedField<EJson::Object>(
				TEXT("logWindow")))
		{
			ScenarioEvidence->SetObjectField(
				TEXT("logWindow"),
				CopyObject(
					ScenarioResult.Data->GetObjectField(
						TEXT("logWindow"))));
		}
		if (ScenarioResult.Data->HasTypedField<EJson::Array>(
				TEXT("artifacts")))
		{
			ScenarioEvidence->SetArrayField(
				TEXT("artifacts"),
				ScenarioResult.Data->GetArrayField(TEXT("artifacts")));
		}
		Job.PendingIterationResult->SetObjectField(
			TEXT("scenario"),
			ScenarioEvidence);
		Job.Repetitions.Add(
			MakeShared<FJsonValueObject>(
				Job.PendingIterationResult));
		Job.PendingIterationResult.Reset();
		++Job.RepeatIndex;
		if (Job.RepeatIndex >= Job.RepeatCount)
		{
			FinishPerformanceRun(Job);
			return;
		}
		FString StartError;
		if (!StartScenarioIteration(Job, StartError))
		{
			FinishJob(
				Job,
				TEXT("failed"),
				TEXT("scenario_start_failed"),
				StartError);
		}
		return;
	}

	if (Now < Job.WarmupUntilSeconds)
	{
		Job.Phase = TEXT("warmup");
		const double IterationStart =
			Job.WarmupUntilSeconds - Job.WarmupSeconds;
		const double WarmupFraction =
			Job.WarmupSeconds > 0.0
				? FMath::Clamp(
					(Now - IterationStart) / Job.WarmupSeconds,
					0.0,
					1.0)
				: 1.0;
		Job.Progress = FMath::Clamp(
			(Job.RepeatIndex + 0.2 * WarmupFraction)
				/ Job.RepeatCount,
			0.0,
			0.95);
		return;
	}
	Job.Phase = TEXT("sampling");
	if (Now < Job.SamplingUntilSeconds)
	{
		SamplePerformanceFrame(Job);
		const double SamplingFraction =
			FMath::Clamp(
				(Now - Job.WarmupUntilSeconds)
					/ FMath::Max(Job.SampleSeconds, 0.001),
				0.0,
				1.0);
		Job.Progress = FMath::Clamp(
			(Job.RepeatIndex + 0.2 + 0.75 * SamplingFraction)
				/ Job.RepeatCount,
			0.0,
			0.95);
		return;
	}

	CompletePerformanceIteration(Job);
	if (GetNumberFieldOr(
			Job.PendingIterationResult,
			TEXT("sampleCount"),
			0.0) < 1.0)
	{
		FinishJob(
			Job,
			TEXT("failed"),
			TEXT("performance_no_samples"),
			TEXT("The sampling window completed without a frame sample."));
		return;
	}
	Job.Repetitions.Add(
		MakeShared<FJsonValueObject>(Job.PendingIterationResult));
	Job.PendingIterationResult.Reset();
	++Job.RepeatIndex;
	if (Job.RepeatIndex >= Job.RepeatCount)
	{
		FinishPerformanceRun(Job);
		return;
	}
	BeginWindowIteration(Job, Now);
}

FMCPToolResult FProductionJobRuntime::GetPerformanceResult(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = GetJobIdParam(Params);
	const TSharedPtr<FJob>* Job = Jobs.Find(RunId);
	if (!Job
		|| !Job->IsValid()
		|| ((*Job)->Kind != TEXT("performance")
			&& (*Job)->Kind != TEXT("performanceStandalone")))
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
		|| ((*Baseline)->Kind != TEXT("performance")
			&& (*Baseline)->Kind != TEXT("performanceStandalone"))
		|| ((*Candidate)->Kind != TEXT("performance")
			&& (*Candidate)->Kind != TEXT("performanceStandalone")))
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

	TSharedPtr<FJsonObject> Data = ComparePerformanceResults(
		(*Baseline)->Result,
		(*Candidate)->Result,
		Params);
	Data->SetStringField(TEXT("baselineRunId"), BaselineId);
	Data->SetStringField(TEXT("candidateRunId"), CandidateId);
	return FMCPToolResult::Ok(Data);
}

TSharedPtr<FJsonObject> FProductionJobRuntime::ComparePerformanceResults(
	const TSharedPtr<FJsonObject>& BaselineResult,
	const TSharedPtr<FJsonObject>& CandidateResult,
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.performance-comparison.v1"));
	if (!BaselineResult.IsValid()
		|| !CandidateResult.IsValid()
		|| !BaselineResult->HasTypedField<EJson::Object>(TEXT("context"))
		|| !CandidateResult->HasTypedField<EJson::Object>(TEXT("context"))
		|| !BaselineResult->HasTypedField<EJson::Object>(TEXT("metrics"))
		|| !CandidateResult->HasTypedField<EJson::Object>(TEXT("metrics")))
	{
		Data->SetStringField(TEXT("verdict"), TEXT("inconclusive"));
		Data->SetStringField(
			TEXT("reason"),
			TEXT("One or both performance results are incomplete."));
		return Data;
	}

	const TSharedPtr<FJsonObject> BaselineContext =
		BaselineResult->GetObjectField(TEXT("context"));
	const TSharedPtr<FJsonObject> CandidateContext =
		CandidateResult->GetObjectField(TEXT("context"));
	static const TArray<FString> ContextFields = {
		TEXT("project"),
		TEXT("map"),
		TEXT("rhi"),
		TEXT("gpu"),
		TEXT("resolution"),
		TEXT("configuration"),
		TEXT("engineVersion"),
		TEXT("platform")
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

	Data->SetArrayField(TEXT("contextMismatches"), Mismatches);
	if (!Mismatches.IsEmpty())
	{
		Data->SetStringField(TEXT("verdict"), TEXT("inconclusive"));
		Data->SetStringField(
			TEXT("reason"),
			TEXT("Performance run contexts differ."));
		return Data;
	}

	TArray<TSharedPtr<FJsonValue>> RequestedChecks;
	if (Params.IsValid()
		&& Params->HasTypedField<EJson::Array>(TEXT("checks")))
	{
		RequestedChecks = Params->GetArrayField(TEXT("checks"));
	}
	if (RequestedChecks.IsEmpty())
	{
		TSharedPtr<FJsonObject> LegacyCheck = MakeShared<FJsonObject>();
		LegacyCheck->SetStringField(
			TEXT("metric"),
			GetStringFieldOr(Params, TEXT("metric"), TEXT("frameMs")));
		LegacyCheck->SetStringField(
			TEXT("statistic"),
			GetStringFieldOr(Params, TEXT("statistic"), TEXT("p95")));
		LegacyCheck->SetNumberField(
			TEXT("maxRegressionPercent"),
			GetNumberFieldOr(
				Params,
				TEXT("maxRegressionPercent"),
				5.0));
		const double AbsoluteBudgetMs =
			GetNumberFieldOr(
				Params,
				TEXT("absoluteBudgetMs"),
				0.0);
		if (AbsoluteBudgetMs > 0.0)
		{
			LegacyCheck->SetNumberField(
				TEXT("absoluteBudgetMs"),
				AbsoluteBudgetMs);
		}
		RequestedChecks.Add(
			MakeShared<FJsonValueObject>(LegacyCheck));
	}

	const TSharedPtr<FJsonObject> BaselineMetrics =
		BaselineResult->GetObjectField(TEXT("metrics"));
	const TSharedPtr<FJsonObject> CandidateMetrics =
		CandidateResult->GetObjectField(TEXT("metrics"));
	TArray<TSharedPtr<FJsonValue>> CheckResults;
	bool bAnyRegression = false;
	bool bAnyInconclusive = false;
	const int32 CheckLimit = FMath::Min(32, RequestedChecks.Num());
	for (int32 CheckIndex = 0; CheckIndex < CheckLimit; ++CheckIndex)
	{
		const TSharedPtr<FJsonValue>& CheckValue =
			RequestedChecks[CheckIndex];
		if (!CheckValue.IsValid() || CheckValue->Type != EJson::Object)
		{
			bAnyInconclusive = true;
			continue;
		}
		const TSharedPtr<FJsonObject> Check = CheckValue->AsObject();
		const FString Metric =
			GetStringFieldOr(Check, TEXT("metric"), TEXT("frameMs"));
		const FString Statistic =
			GetStringFieldOr(Check, TEXT("statistic"), TEXT("p95"));
		const double MaxRegressionPercent =
			GetNumberFieldOr(
				Check,
				TEXT("maxRegressionPercent"),
				5.0);
		const double AbsoluteBudgetMs =
			GetNumberFieldOr(
				Check,
				TEXT("absoluteBudgetMs"),
				0.0);
		TSharedPtr<FJsonObject> CheckResult = MakeShared<FJsonObject>();
		CheckResult->SetStringField(TEXT("metric"), Metric);
		CheckResult->SetStringField(TEXT("statistic"), Statistic);
		const bool bThresholdsValid =
			FMath::IsFinite(MaxRegressionPercent)
			&& MaxRegressionPercent >= 0.0
			&& FMath::IsFinite(AbsoluteBudgetMs)
			&& AbsoluteBudgetMs >= 0.0;
		if (!bThresholdsValid)
		{
			CheckResult->SetStringField(
				TEXT("verdict"),
				TEXT("inconclusive"));
			CheckResult->SetStringField(
				TEXT("reason"),
				TEXT(
					"Performance thresholds must be finite, non-negative "
					"numbers."));
			CheckResults.Add(
				MakeShared<FJsonValueObject>(CheckResult));
			bAnyInconclusive = true;
			continue;
		}
		CheckResult->SetNumberField(
			TEXT("maxRegressionPercent"),
			MaxRegressionPercent);
		if (AbsoluteBudgetMs > 0.0)
		{
			CheckResult->SetNumberField(
				TEXT("absoluteBudgetMs"),
				AbsoluteBudgetMs);
		}

		const TSharedPtr<FJsonObject>* BaselineMetricPtr = nullptr;
		const TSharedPtr<FJsonObject>* CandidateMetricPtr = nullptr;
		const bool bMetricObjectsAvailable =
			BaselineMetrics->TryGetObjectField(
				Metric,
				BaselineMetricPtr)
			&& CandidateMetrics->TryGetObjectField(
				Metric,
				CandidateMetricPtr)
			&& BaselineMetricPtr
			&& CandidateMetricPtr;
		if (!bMetricObjectsAvailable
			|| !GetBoolFieldOr(
				*BaselineMetricPtr,
				TEXT("available"),
				false)
			|| !GetBoolFieldOr(
				*CandidateMetricPtr,
				TEXT("available"),
				false)
			|| !(*BaselineMetricPtr)->HasField(Statistic)
			|| !(*CandidateMetricPtr)->HasField(Statistic))
		{
			CheckResult->SetStringField(
				TEXT("verdict"),
				TEXT("inconclusive"));
			CheckResult->SetStringField(
				TEXT("reason"),
				TEXT("The requested metric or statistic is unavailable."));
			CheckResults.Add(
				MakeShared<FJsonValueObject>(CheckResult));
			bAnyInconclusive = true;
			continue;
		}
		const double Before =
			(*BaselineMetricPtr)->GetNumberField(Statistic);
		const double After =
			(*CandidateMetricPtr)->GetNumberField(Statistic);
		if (!FMath::IsFinite(Before)
			|| !FMath::IsFinite(After)
			|| Before < 0.0
			|| After < 0.0)
		{
			CheckResult->SetStringField(
				TEXT("verdict"),
				TEXT("inconclusive"));
			CheckResult->SetStringField(
				TEXT("reason"),
				TEXT(
					"Performance samples must be finite, non-negative "
					"numbers."));
			CheckResults.Add(
				MakeShared<FJsonValueObject>(CheckResult));
			bAnyInconclusive = true;
			continue;
		}
		double RegressionPercent = 0.0;
		bool bRegressionPercentSaturated = false;
		if (Before > SMALL_NUMBER)
		{
			const double Ratio = After / Before;
			const double MaxPercent = TNumericLimits<double>::Max();
			const double MaxRatioDelta = MaxPercent / 100.0;
			if (!FMath::IsFinite(Ratio)
				|| Ratio - 1.0 > MaxRatioDelta)
			{
				RegressionPercent = MaxPercent;
				bRegressionPercentSaturated = true;
			}
			else
			{
				RegressionPercent = (Ratio - 1.0) * 100.0;
			}
		}
		else if (After > Before)
		{
			RegressionPercent = 1000000000.0;
		}
		const bool bRegression =
			RegressionPercent > MaxRegressionPercent
			|| (AbsoluteBudgetMs > 0.0 && After > AbsoluteBudgetMs);
		CheckResult->SetNumberField(TEXT("baseline"), Before);
		CheckResult->SetNumberField(TEXT("candidate"), After);
		CheckResult->SetNumberField(
			TEXT("regressionPercent"),
			RegressionPercent);
		CheckResult->SetBoolField(
			TEXT("regressionPercentSaturated"),
			bRegressionPercentSaturated);
		CheckResult->SetStringField(
			TEXT("verdict"),
			bRegression ? TEXT("regression") : TEXT("pass"));
		CheckResults.Add(
			MakeShared<FJsonValueObject>(CheckResult));
		bAnyRegression |= bRegression;
	}
	Data->SetArrayField(TEXT("checks"), CheckResults);
	Data->SetNumberField(TEXT("checkTotal"), RequestedChecks.Num());
	Data->SetBoolField(
		TEXT("checksTruncated"),
		RequestedChecks.Num() > CheckLimit);
	Data->SetStringField(
		TEXT("verdict"),
		bAnyRegression
			? TEXT("regression")
			: (bAnyInconclusive ? TEXT("inconclusive") : TEXT("pass")));
	return Data;
}

TSharedPtr<FJsonObject> FProductionJobRuntime::ResolveProcessTimeoutPolicy(
	const TSharedPtr<FJsonObject>& Params,
	const double DefaultExecutionTimeoutSeconds)
{
	const double DefaultExecution = FMath::Clamp(
		DefaultExecutionTimeoutSeconds,
		1.0,
		MaxExecutionTimeoutSeconds);
	const double LegacyExecution = GetNumberFieldOr(
		Params,
		TEXT("timeoutSeconds"),
		DefaultExecution);
	const double Startup = FMath::Clamp(
		GetNumberFieldOr(
			Params,
			TEXT("startupTimeoutSeconds"),
			300.0),
		1.0,
		MaxStartupTimeoutSeconds);
	const double Execution = FMath::Clamp(
		GetNumberFieldOr(
			Params,
			TEXT("executionTimeoutSeconds"),
			LegacyExecution),
		1.0,
		MaxExecutionTimeoutSeconds);
	const double Shutdown = FMath::Clamp(
		GetNumberFieldOr(
			Params,
			TEXT("shutdownTimeoutSeconds"),
			60.0),
		1.0,
		MaxShutdownTimeoutSeconds);
	const double RequestedHard = GetNumberFieldOr(
		Params,
		TEXT("hardTimeoutSeconds"),
		Startup + Execution + Shutdown);
	const double Hard = FMath::Clamp(
		RequestedHard,
		1.0,
		MaxHardTimeoutSeconds);

	TSharedPtr<FJsonObject> Policy = MakeShared<FJsonObject>();
	Policy->SetNumberField(TEXT("startupTimeoutSeconds"), Startup);
	Policy->SetNumberField(TEXT("executionTimeoutSeconds"), Execution);
	Policy->SetNumberField(TEXT("shutdownTimeoutSeconds"), Shutdown);
	Policy->SetNumberField(TEXT("hardTimeoutSeconds"), Hard);
	Policy->SetNumberField(
		TEXT("hardLimitSeconds"),
		MaxHardTimeoutSeconds);
	Policy->SetBoolField(
		TEXT("hardTimeoutIsLimiting"),
		Hard < Startup + Execution + Shutdown);
	return Policy;
}

FString FProductionJobRuntime::BuildStandaloneStartupTimeoutArgument(
	const double StartupTimeoutSeconds)
{
	return FString::Printf(
		TEXT("-UEAIPerfStartupTimeoutSeconds=%.17g"),
		FMath::Clamp(
			StartupTimeoutSeconds,
			1.0,
			MaxStartupTimeoutSeconds));
}

bool FProductionJobRuntime::ValidateStandaloneChildReceipt(
	const TSharedPtr<FJsonObject>& Receipt,
	const FString& ExpectedJobId,
	const int32 ExpectedRepeatCount,
	FString& OutError)
{
	OutError.Reset();
	if (!Receipt.IsValid())
	{
		OutError =
			TEXT("The standalone child receipt is missing.");
		return false;
	}
	FString InvalidNumberPath;
	if (!HasOnlyFiniteJsonNumbers(Receipt, InvalidNumberPath))
	{
		OutError = FString::Printf(
			TEXT(
				"The standalone child receipt contains a non-finite number "
				"or exceeds the validation depth at '%s'."),
			*InvalidNumberPath);
		return false;
	}
	if (GetStringFieldOr(Receipt, TEXT("schema"))
		!= TEXT("ue.performance-standalone-child.v1"))
	{
		OutError =
			TEXT("The standalone child receipt schema is missing or invalid.");
		return false;
	}
	if (GetStringFieldOr(Receipt, TEXT("jobId")) != ExpectedJobId)
	{
		OutError =
			TEXT("The standalone child receipt belongs to another job.");
		return false;
	}
	const FString Status = GetStringFieldOr(Receipt, TEXT("status"));
	if (Status != TEXT("succeeded") && Status != TEXT("failed"))
	{
		OutError =
			TEXT("The standalone child receipt has no terminal status.");
		return false;
	}
	if (Status == TEXT("succeeded"))
	{
		const int32 CompletedRepeatCount = static_cast<int32>(
			GetNumberFieldOr(
				Receipt,
				TEXT("completedRepeatCount"),
				-1.0));
		if (CompletedRepeatCount != ExpectedRepeatCount
			|| !Receipt->HasTypedField<EJson::Array>(TEXT("csvFiles"))
			|| Receipt->GetArrayField(TEXT("csvFiles")).Num()
				!= ExpectedRepeatCount)
		{
			OutError =
				TEXT(
					"The standalone child receipt does not contain the "
					"declared number of completed CSV measurement windows.");
			return false;
		}
		if (!Receipt->HasTypedField<EJson::Object>(TEXT("camera"))
			|| !GetBoolFieldOr(
				Receipt->GetObjectField(TEXT("camera")),
				TEXT("lockedAndVerified"),
				false))
		{
			OutError =
				TEXT(
					"The standalone child receipt does not verify the "
					"declared camera lock.");
			return false;
		}
	}
	else
	{
		const FString ErrorCode =
			GetStringFieldOr(Receipt, TEXT("errorCode"));
		const FString Message = GetStringFieldOr(Receipt, TEXT("message"));
		if (ErrorCode.IsEmpty() || ErrorCode.Len() > 128
			|| Message.IsEmpty() || Message.Len() > 2048)
		{
			OutError =
				TEXT(
					"A failed standalone child receipt must include a "
					"bounded errorCode and message.");
			return false;
		}
	}
	return true;
}

bool FProductionJobRuntime::CanStandaloneChildReceiptOverrideTerminal(
	const int32 ReturnCode,
	const FString& ErrorCode)
{
	return ReturnCode != INDEX_NONE
		&& (ErrorCode.IsEmpty() || ErrorCode == TEXT("process_failed"));
}

FString FProductionJobRuntime::BuildHeadlessProfileArguments(
	const FString& Profile,
	FString& OutError)
{
	OutError.Reset();
	const FString Effective =
		Profile.IsEmpty() ? TEXT("minimal") : Profile;
	const FString Common = TEXT("-unattended -nop4 -nosplash -NoSound");
	const FString XrDisabled =
		TEXT(" -DisablePlugins=OpenXR,OpenXREyeTracker,")
		TEXT("OpenXRHandTracking,OpenXRMsftHandInteraction");
	if (Effective == TEXT("minimal"))
	{
		return Common
			+ TEXT(" -NullRHI -DisablePlugins=OpenXR,OpenXREyeTracker,")
			+ TEXT("OpenXRHandTracking,OpenXRMsftHandInteraction,")
			+ TEXT("PerforceSourceControl,PlasticSourceControl,")
			+ TEXT("SubversionSourceControl");
	}
	if (Effective == TEXT("project"))
	{
		return Common + XrDisabled + TEXT(" -NullRHI");
	}
	if (Effective == TEXT("rendering"))
	{
		return Common + XrDisabled;
	}
	OutError =
		TEXT("headlessProfile must be 'minimal', 'project', or 'rendering'.");
	return FString();
}

FString FProductionJobRuntime::InferAutomationPhase(
	const FString& CurrentPhase,
	const FString& LogChunk)
{
	FString Phase = CurrentPhase;
	if (LogChunk.Contains(
			TEXT("Ready to start automation"),
			ESearchCase::IgnoreCase)
		|| LogChunk.Contains(
			TEXT("FindWorkers"),
			ESearchCase::IgnoreCase)
		|| LogChunk.Contains(
			TEXT("Requesting test list"),
			ESearchCase::IgnoreCase))
	{
		if (Phase == TEXT("launching") || Phase == TEXT("loading"))
		{
			Phase = TEXT("discovering");
		}
	}
	if (LogChunk.Contains(
			TEXT("automation tests based on"),
			ESearchCase::IgnoreCase)
		|| LogChunk.Contains(
			TEXT("Test Started."),
			ESearchCase::IgnoreCase)
		|| LogChunk.Contains(
			TEXT("Sending RunTest"),
			ESearchCase::IgnoreCase))
	{
		if (IsStartupPhase(Phase))
		{
			Phase = TEXT("running");
		}
	}
	if (LogChunk.Contains(
			TEXT("Automation Test Queue Empty"),
			ESearchCase::IgnoreCase))
	{
		if (IsStartupPhase(Phase))
		{
			Phase = TEXT("running");
		}
		if (Phase == TEXT("running"))
		{
			Phase = TEXT("reporting");
		}
	}
	if (LogChunk.Contains(
			TEXT("Successfully wrote html results file"),
			ESearchCase::IgnoreCase))
	{
		// The test report is complete; any remaining process lifetime belongs
		// to Editor shutdown and is governed by shutdownTimeoutSeconds.
		Phase = TEXT("exiting");
	}
	if (LogChunk.Contains(
			TEXT("Engine exit requested"),
			ESearchCase::IgnoreCase)
		|| LogChunk.Contains(
			TEXT("Requesting Exit"),
			ESearchCase::IgnoreCase)
		|| LogChunk.Contains(
			TEXT("LogExit:"),
			ESearchCase::IgnoreCase))
	{
		Phase = TEXT("exiting");
	}
	return Phase;
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
		const FString HeadlessProfile =
			GetStringFieldOr(
				Params,
				TEXT("headlessProfile"),
				TEXT("minimal"));
		FString ProfileError;
		const FString HeadlessArguments =
			BuildHeadlessProfileArguments(
				HeadlessProfile,
				ProfileError);
		if (!ProfileError.IsEmpty())
		{
			return FMCPToolResult::Error(
				ProfileError,
				TEXT("invalid_params"),
				422);
		}
		Executable = GetEditorCommandletExecutable();
		if (!IFileManager::Get().FileExists(*Executable))
		{
			return FMCPToolResult::Error(
				TEXT("UnrealEditor-Cmd is unavailable."),
				TEXT("test_unavailable"),
				503);
		}

		FProcessLaunchSpec Spec;
		Spec.Kind = TEXT("test");
		Spec.Executable = Executable;
		Spec.WorkingDirectory = FPaths::ProjectDir();
		Spec.PostProcess = TEXT("test");
		Spec.DefaultExecutionTimeoutSeconds = 1800.0;
		Spec.bEditorProcess = true;
		Spec.bAutomationProcess = true;
		Spec.BuildArguments =
			[ProjectFile, Test, HeadlessArguments](
				const FString&,
				const FString&,
				const FString&,
				const FString& ReportDirectory)
			{
				return FString::Printf(
					TEXT("%s %s -ExecCmds=%s -TestExit=%s -ReportExportPath=%s -log"),
					*QuoteArgument(ProjectFile),
					*HeadlessArguments,
					*QuoteArgument(
						FString::Printf(
							TEXT("Automation RunTests %s;Quit"),
							*Test)),
					*QuoteArgument(
						TEXT("Automation Test Queue Empty")),
					*QuoteArgument(ReportDirectory));
			};
		FString Error;
		TSharedPtr<FJob> Job = StartProcessJob(
			Spec,
			Params,
			Error);
		if (!Job.IsValid())
		{
			return MakeJobStartFailure(Error);
		}
		return FMCPToolResult::Ok(MakeJobSummary(*Job, false));
	}

	FString Error;
	TSharedPtr<FJob> Job = StartProcessJob(
		TEXT("test"),
		Executable,
		Arguments,
		FPaths::ProjectDir(),
		GetNumberFieldOr(Params, TEXT("timeoutSeconds"), 1800.0),
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
		GetStringFieldOr(Params, TEXT("outputDir"));
	if (OutputInput.IsEmpty()
		|| !IsSafeLegacyArgumentString(OutputInput))
	{
		return FMCPToolResult::Error(
			TEXT("outputDir is required and may not contain command separators."),
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
				"outputDir must resolve inside "
				"Saved/UEAIIntegration/Packages."),
			TEXT("outputPath_not_permitted"),
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
		GetStringFieldOr(Params, TEXT("commandletName"));
	if (!IsSafeToken(Commandlet, 128))
	{
		return FMCPToolResult::Error(
			TEXT("commandletName must be an identifier token."),
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
	TSharedPtr<FJsonObject> JobInput = CopyObject(Params);
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
	if (!IFileManager::Get().FileExists(*JobLogFile))
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
	if (!Job.EditorLogPath.IsEmpty()
		&& IFileManager::Get().FileExists(*Job.EditorLogPath))
	{
		AddArtifact(
			Job,
			Job.EditorLogPath,
			TEXT("editor.log"),
			TEXT("text/plain"));
	}
	if (Job.PostProcess == TEXT("test"))
	{
		WriteTestReports(Job);
	}
	else if (Job.PostProcess == TEXT("performanceStandalone"))
	{
		// A bounded child receipt is authoritative only for a process that
		// reached its own terminal exit. Cancellation, timeout, Editor
		// shutdown, or another controller-owned terminal state must remain
		// authoritative even if a child happened to flush a receipt while
		// teardown was in progress.
		const bool bMayAdoptChildTerminal =
			CanStandaloneChildReceiptOverrideTerminal(
				Job.ReturnCode,
				Job.ErrorCode);
		const FString ReceiptPath = FPaths::Combine(
			JobDirectory(Job.Id),
			TEXT("standalone-child.json"));
		auto AddStandaloneWarning =
			[&Job](const FString& Code, const FString& Message)
			{
				TArray<TSharedPtr<FJsonValue>> Warnings;
				if (Job.Result->HasTypedField<EJson::Array>(TEXT("warnings")))
				{
					Warnings = Job.Result->GetArrayField(TEXT("warnings"));
				}
				TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
				Warning->SetStringField(TEXT("code"), Code);
				Warning->SetStringField(TEXT("message"), Message);
				Warnings.Add(MakeShared<FJsonValueObject>(Warning));
				Job.Result->SetArrayField(TEXT("warnings"), Warnings);
			};

		TSharedPtr<FJsonObject> ChildReceipt;
		FString ReceiptJson;
		const int64 ReceiptSize =
			IFileManager::Get().FileSize(*ReceiptPath);
		const bool bReceiptSizeValid =
			ReceiptSize >= 0
			&& ReceiptSize <= MaxStandaloneChildReceiptBytes;
		if (bReceiptSizeValid
			&& FFileHelper::LoadFileToString(ReceiptJson, *ReceiptPath))
		{
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(ReceiptJson);
			FJsonSerializer::Deserialize(Reader, ChildReceipt);
		}
		FString ReceiptValidationError;
		const int32 ExpectedRepeatCount = static_cast<int32>(
			GetNumberFieldOr(
				Job.Input,
				TEXT("repeatCount"),
				5.0));
		const bool bReceiptValid =
			ChildReceipt.IsValid()
			&& ValidateStandaloneChildReceipt(
				ChildReceipt,
				Job.Id,
				ExpectedRepeatCount,
				ReceiptValidationError);
		if (!bReceiptSizeValid)
		{
			ReceiptValidationError =
				ReceiptSize < 0
					? TEXT("The standalone child receipt is unavailable.")
					: TEXT(
						"The standalone child receipt exceeds the 256 KiB "
						"bounded evidence limit.");
		}
		else if (!ChildReceipt.IsValid())
		{
			ReceiptValidationError =
				TEXT("The standalone child receipt is invalid JSON.");
		}

		Job.Result->SetBoolField(
			TEXT("childReceiptVerified"),
			bReceiptValid);
		Job.Result->SetBoolField(
			TEXT("standaloneReceiptAuthoritative"),
			false);
		if (!bReceiptValid)
		{
			if (bMayAdoptChildTerminal)
			{
				Job.Status = TEXT("failed");
				Job.ErrorCode =
					ChildReceipt.IsValid()
						? TEXT("performance_child_receipt_invalid")
						: TEXT("performance_child_receipt_unavailable");
				Job.Message = ReceiptValidationError;
			}
			Job.Result->SetStringField(
				TEXT("standaloneReceiptValidationError"),
				ReceiptValidationError);
		}
		else
		{
			Job.Result->SetObjectField(
				TEXT("standaloneChildReceipt"),
				CopyObject(ChildReceipt));
			if (ChildReceipt->HasTypedField<EJson::Object>(
				TEXT("runtimeFingerprint")))
			{
				Job.Result->SetObjectField(
					TEXT("runtimeFingerprint"),
					CopyObject(
						ChildReceipt->GetObjectField(
							TEXT("runtimeFingerprint"))));
			}
			if (ChildReceipt->HasTypedField<EJson::Object>(TEXT("camera")))
			{
				Job.Result->SetObjectField(
					TEXT("camera"),
					CopyObject(
						ChildReceipt->GetObjectField(TEXT("camera"))));
			}
			const FString ChildStatus =
				GetStringFieldOr(ChildReceipt, TEXT("status"));
			if (bMayAdoptChildTerminal
				&& ChildStatus != TEXT("succeeded"))
			{
				Job.Status = TEXT("failed");
				Job.ErrorCode =
					GetStringFieldOr(
						ChildReceipt,
						TEXT("errorCode"));
				Job.Message =
					GetStringFieldOr(
						ChildReceipt,
						TEXT("message"));
			}
			else if (bMayAdoptChildTerminal)
			{
				// Promote the process into a provisional success so its CSV
				// and requested Trace evidence can be independently verified.
				// The authoritative marker is set only after those gates pass.
				Job.Status = TEXT("succeeded");
				Job.ErrorCode.Reset();
				Job.Message.Reset();
				Job.Result->SetNumberField(
					TEXT("childProcessExitCode"),
					Job.ReturnCode);
			}
			else
			{
				AddStandaloneWarning(
					TEXT(
						"standalone_receipt_ignored_after_control_terminal"),
					TEXT(
						"The child receipt was retained as evidence but did "
						"not override cancellation, timeout, or another "
						"controller-owned terminal state."));
			}
		}
		if (ReceiptSize >= 0)
		{
			AddArtifact(
				Job,
				ReceiptPath,
				TEXT("standalone-child.json"),
				TEXT("application/json"));
		}

		TArray<FString> CsvPaths;
		IFileManager::Get().FindFilesRecursive(
			CsvPaths,
			*JobDirectory(Job.Id),
			TEXT("standalone-repeat-*.csv"),
			true,
			false);
		CsvPaths.Sort();
		for (int32 Index = 0; Index < CsvPaths.Num(); ++Index)
		{
			AddArtifact(
				Job,
				CsvPaths[Index],
				FString::Printf(
					TEXT("standalone-repeat-%02d.csv"),
					Index + 1),
				TEXT("text/csv"));
		}
		const FString TracePath =
			FPaths::Combine(JobDirectory(Job.Id), TEXT("standalone.utrace"));
		bool bTraceArtifactReady = false;
		if (IFileManager::Get().FileSize(*TracePath) > 0)
		{
			if (FArtifact* TraceArtifact = AddArtifact(
				Job,
				TracePath,
				TEXT("standalone.utrace"),
				TEXT("application/x-unreal-trace")))
			{
				bTraceArtifactReady = true;
				Job.Result->SetStringField(
					TEXT("traceArtifactId"),
					TraceArtifact->Id);
				// A standalone run is itself the durable owner of this trace.
				// AnalyzeTrace validates the trace artifact before accepting
				// this source job id.
				Job.Result->SetStringField(TEXT("traceId"), Job.Id);
				Job.Result->SetBoolField(TEXT("traceCompleted"), true);
			}
		}
		const bool bVerifiedSuccessfulReceipt =
			bReceiptValid
			&& GetStringFieldOr(ChildReceipt, TEXT("status"))
				== TEXT("succeeded");
		bool bCsvEvidenceReady = false;
		if (Job.Status == TEXT("succeeded")
			&& bVerifiedSuccessfulReceipt)
		{
			bCsvEvidenceReady = ParseStandalonePerformanceCsv(Job);
			if (!bCsvEvidenceReady)
			{
				Job.Status = TEXT("failed");
				Job.ErrorCode = TEXT("performance_parse_failed");
				Job.Message =
					TEXT(
						"The standalone child receipt was verified, but its "
						"bounded CSV timing evidence could not be parsed.");
			}
		}
		const bool bTraceRequested =
			GetBoolFieldOr(Job.Input, TEXT("captureTrace"), true);
		const bool bReceiptReportsTrace =
			bVerifiedSuccessfulReceipt
			&& GetBoolFieldOr(
				ChildReceipt,
				TEXT("traceAvailable"),
				false);
		if (Job.Status == TEXT("succeeded")
			&& bTraceRequested
			&& (!bReceiptReportsTrace || !bTraceArtifactReady))
		{
			Job.Status = TEXT("failed");
			Job.ErrorCode = TEXT("performance_trace_unavailable");
			Job.Message =
				TEXT(
					"The standalone child completed its CSV windows, but "
					"the requested non-empty Trace artifact was not verified.");
		}
		if (Job.Status == TEXT("succeeded")
			&& bMayAdoptChildTerminal
			&& bVerifiedSuccessfulReceipt
			&& bCsvEvidenceReady
			&& (!bTraceRequested || bTraceArtifactReady))
		{
			Job.Result->SetBoolField(
				TEXT("standaloneReceiptAuthoritative"),
				true);
			if (Job.ReturnCode != 0)
			{
				AddStandaloneWarning(
					TEXT(
						"standalone_nonzero_exit_after_verified_receipt"),
					FString::Printf(
						TEXT(
							"The Editor process exited with code %d after "
							"the managed child flushed a verified receipt, "
							"CSV windows, and all requested Trace evidence."),
						Job.ReturnCode));
			}
		}
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
		Job.ReportDirectory.IsEmpty()
			? FPaths::Combine(JobDirectory(Job.Id), TEXT("report"))
			: Job.ReportDirectory;
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

bool FProductionJobRuntime::ParseStandalonePerformanceCsv(FJob& Job)
{
	struct FCsvSample
	{
		double Frame = 0.0;
		double Game = 0.0;
		double Render = 0.0;
		double Rhi = 0.0;
		double Gpu = 0.0;
	};
	struct FParsedCsv
	{
		FString Path;
		TArray<FCsvSample> Samples;
		TSharedPtr<FJsonObject> ColumnMapping;
	};
	const auto ReadNumber =
		[](const TArray<FString>& Row, const int32 Column, double& Out)
		{
			if (Column == INDEX_NONE || !Row.IsValidIndex(Column))
			{
				Out = 0.0;
				return false;
			}
			return FDefaultValueHelper::ParseDouble(Row[Column], Out)
				&& FMath::IsFinite(Out)
				&& Out >= 0.0;
		};
	const auto ParseOne =
		[&ReadNumber](const FString& CsvPath, FParsedCsv& Out)
		{
			FString Csv;
			if (!FFileHelper::LoadFileToString(Csv, *CsvPath))
			{
				return false;
			}
			TArray<FString> Lines;
			Csv.ParseIntoArrayLines(Lines, true);
			int32 HeaderIndex = INDEX_NONE;
			TArray<FString> Header;
			int32 FrameColumn = INDEX_NONE;
			int32 GameColumn = INDEX_NONE;
			int32 RenderColumn = INDEX_NONE;
			int32 RhiColumn = INDEX_NONE;
			int32 GpuColumn = INDEX_NONE;
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				TArray<FString> Candidate = ParseCsvRow(Lines[Index]);
				const int32 CandidateFrame = FindCsvColumn(
					Candidate,
					{TEXT("frametime")});
				if (CandidateFrame == INDEX_NONE)
				{
					continue;
				}
				const int32 CandidateGame = FindCsvColumn(
					Candidate,
					{TEXT("gamethreadtime"), TEXT("gamethread")});
				const int32 CandidateRender = FindCsvColumn(
					Candidate,
					{TEXT("renderthreadtime"), TEXT("renderthread")});
				const int32 CandidateRhi = FindCsvColumn(
					Candidate,
					{TEXT("rhithreadtime"), TEXT("rhithread")});
				const int32 CandidateGpu = FindCsvColumn(
					Candidate,
					{TEXT("gputime")});
				if (CandidateGame == INDEX_NONE
					&& CandidateRender == INDEX_NONE
					&& CandidateRhi == INDEX_NONE
					&& CandidateGpu == INDEX_NONE)
				{
					continue;
				}
				HeaderIndex = Index;
				Header = MoveTemp(Candidate);
				FrameColumn = CandidateFrame;
				GameColumn = CandidateGame;
				RenderColumn = CandidateRender;
				RhiColumn = CandidateRhi;
				GpuColumn = CandidateGpu;
				break;
			}
			if (HeaderIndex == INDEX_NONE || FrameColumn == INDEX_NONE)
			{
				return false;
			}
			for (int32 LineIndex = HeaderIndex + 1;
				LineIndex < Lines.Num();
				++LineIndex)
			{
				const TArray<FString> Row = ParseCsvRow(Lines[LineIndex]);
				FCsvSample Sample;
				if (!ReadNumber(Row, FrameColumn, Sample.Frame))
				{
					continue;
				}
				ReadNumber(Row, GameColumn, Sample.Game);
				ReadNumber(Row, RenderColumn, Sample.Render);
				ReadNumber(Row, RhiColumn, Sample.Rhi);
				ReadNumber(Row, GpuColumn, Sample.Gpu);
				Out.Samples.Add(Sample);
			}
			if (Out.Samples.IsEmpty())
			{
				return false;
			}
			Out.Path = CsvPath;
			Out.ColumnMapping = MakeShared<FJsonObject>();
			const auto SetColumn =
				[&Header, &Out](const TCHAR* Name, const int32 Column)
				{
					Out.ColumnMapping->SetStringField(
						Name,
						Column != INDEX_NONE && Header.IsValidIndex(Column)
							? Header[Column]
							: TEXT("unavailable"));
				};
			SetColumn(TEXT("frameMs"), FrameColumn);
			SetColumn(TEXT("gameMs"), GameColumn);
			SetColumn(TEXT("renderMs"), RenderColumn);
			SetColumn(TEXT("rhiMs"), RhiColumn);
			SetColumn(TEXT("gpuMs"), GpuColumn);
			return true;
		};

	const int32 RepeatCount = FMath::Clamp(
		static_cast<int32>(
			GetNumberFieldOr(Job.Input, TEXT("repeatCount"), 1.0)),
		1,
		20);
	TArray<FString> CsvPaths;
	IFileManager::Get().FindFilesRecursive(
		CsvPaths,
		*JobDirectory(Job.Id),
		TEXT("standalone-repeat-*.csv"),
		true,
		false);
	CsvPaths.Sort();
	if (CsvPaths.Num() != RepeatCount)
	{
		return false;
	}
	TArray<FParsedCsv> ParsedCsvs;
	ParsedCsvs.Reserve(CsvPaths.Num());
	for (const FString& CsvPath : CsvPaths)
	{
		FParsedCsv Parsed;
		if (!ParseOne(CsvPath, Parsed))
		{
			return false;
		}
		ParsedCsvs.Add(MoveTemp(Parsed));
	}
	const double BudgetMs = GetNumberFieldOr(
		Job.Result,
		TEXT("budgetMs"),
		16.6667);

	TMap<FString, TArray<double>> Aggregate;
	TArray<TSharedPtr<FJsonValue>> Repetitions;
	TArray<TSharedPtr<FJsonValue>> ColumnMappings;
	auto AppendSample =
		[&Aggregate](const FCsvSample& Sample)
		{
			Aggregate.FindOrAdd(TEXT("frameMs")).Add(Sample.Frame);
			if (Sample.Game > 0.0)
			{
				Aggregate.FindOrAdd(TEXT("gameMs")).Add(Sample.Game);
			}
			if (Sample.Render > 0.0)
			{
				Aggregate.FindOrAdd(TEXT("renderMs")).Add(Sample.Render);
			}
			if (Sample.Rhi > 0.0)
			{
				Aggregate.FindOrAdd(TEXT("rhiMs")).Add(Sample.Rhi);
			}
			if (Sample.Gpu > 0.0)
			{
				Aggregate.FindOrAdd(TEXT("gpuMs")).Add(Sample.Gpu);
			}
		};
	const TSharedPtr<FJsonObject> ChildReceipt =
		Job.Result->HasTypedField<EJson::Object>(
			TEXT("standaloneChildReceipt"))
			? Job.Result->GetObjectField(TEXT("standaloneChildReceipt"))
			: nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ChildRepetitions = nullptr;
	if (ChildReceipt.IsValid()
		&& ChildReceipt->HasTypedField<EJson::Array>(TEXT("repetitions")))
	{
		ChildRepetitions =
			&ChildReceipt->GetArrayField(TEXT("repetitions"));
	}
	int32 CapturedFrameCount = 0;
	for (int32 Repeat = 0; Repeat < ParsedCsvs.Num(); ++Repeat)
	{
		const FParsedCsv& Parsed = ParsedCsvs[Repeat];
		TMap<FString, TArray<double>> Iteration;
		for (const FCsvSample& Sample : Parsed.Samples)
		{
			AppendSample(Sample);
			Iteration.FindOrAdd(TEXT("frameMs")).Add(Sample.Frame);
			if (Sample.Game > 0.0)
			{
				Iteration.FindOrAdd(TEXT("gameMs")).Add(Sample.Game);
			}
			if (Sample.Render > 0.0)
			{
				Iteration.FindOrAdd(TEXT("renderMs")).Add(Sample.Render);
			}
			if (Sample.Rhi > 0.0)
			{
				Iteration.FindOrAdd(TEXT("rhiMs")).Add(Sample.Rhi);
			}
			if (Sample.Gpu > 0.0)
			{
				Iteration.FindOrAdd(TEXT("gpuMs")).Add(Sample.Gpu);
			}
		}
		if (Iteration.FindRef(TEXT("frameMs")).IsEmpty())
		{
			continue;
		}
		TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
		for (const TCHAR* Metric : {
			TEXT("frameMs"),
			TEXT("gameMs"),
			TEXT("renderMs"),
			TEXT("rhiMs"),
			TEXT("gpuMs")})
		{
			Metrics->SetObjectField(
				Metric,
				SummarizeMetric(Iteration.FindRef(Metric), BudgetMs));
		}
		TSharedPtr<FJsonObject> IterationResult =
			MakeShared<FJsonObject>();
		IterationResult->SetNumberField(TEXT("repeatIndex"), Repeat + 1);
		IterationResult->SetNumberField(
			TEXT("sampleCount"),
			Iteration.FindRef(TEXT("frameMs")).Num());
		if (ChildRepetitions
			&& ChildRepetitions->IsValidIndex(Repeat)
			&& (*ChildRepetitions)[Repeat].IsValid()
			&& (*ChildRepetitions)[Repeat]->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Timing =
				(*ChildRepetitions)[Repeat]->AsObject();
			IterationResult->SetNumberField(
				TEXT("requestedSampleSeconds"),
				GetNumberFieldOr(
					Timing,
					TEXT("requestedSampleSeconds"),
					0.0));
			IterationResult->SetNumberField(
				TEXT("actualSampleSeconds"),
				GetNumberFieldOr(
					Timing,
					TEXT("actualSampleSeconds"),
					0.0));
		}
		IterationResult->SetObjectField(TEXT("metrics"), Metrics);
		Repetitions.Add(
			MakeShared<FJsonValueObject>(IterationResult));
		TSharedPtr<FJsonObject> Mapping =
			CopyObject(Parsed.ColumnMapping);
		Mapping->SetNumberField(TEXT("repeatIndex"), Repeat + 1);
		Mapping->SetStringField(
			TEXT("file"),
			FPaths::GetCleanFilename(Parsed.Path));
		ColumnMappings.Add(MakeShared<FJsonValueObject>(Mapping));
		CapturedFrameCount += Parsed.Samples.Num();
	}
	if (Aggregate.FindRef(TEXT("frameMs")).IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	for (const TCHAR* Metric : {
		TEXT("frameMs"),
		TEXT("gameMs"),
		TEXT("renderMs"),
		TEXT("rhiMs"),
		TEXT("gpuMs")})
	{
		Metrics->SetObjectField(
			Metric,
			SummarizeMetric(Aggregate.FindRef(Metric), BudgetMs));
	}
	TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
	Context->SetStringField(
		TEXT("map"),
		GetStringFieldOr(Job.Result, TEXT("map")));
	Context->SetStringField(
		TEXT("executionTarget"),
		TEXT("standalone"));
	Context->SetStringField(
		TEXT("renderLaunchPolicy"),
		TEXT("visibleWindow"));
	if (Job.Result->HasTypedField<EJson::Object>(
		TEXT("runtimeFingerprint")))
	{
		Context->SetObjectField(
			TEXT("runtimeFingerprint"),
			CopyObject(
				Job.Result->GetObjectField(TEXT("runtimeFingerprint"))));
	}
	Job.Result->SetObjectField(TEXT("context"), Context);
	Job.Result->SetObjectField(TEXT("metrics"), Metrics);
	Job.Result->SetArrayField(TEXT("repetitions"), Repetitions);
	Job.Result->SetArrayField(TEXT("csvColumnMappings"), ColumnMappings);
	Job.Result->SetNumberField(
		TEXT("sampleCount"),
		Aggregate.FindRef(TEXT("frameMs")).Num());
	Job.Result->SetNumberField(
		TEXT("completedRepeatCount"),
		ParsedCsvs.Num());
	Job.Result->SetNumberField(
		TEXT("capturedFrameCount"),
		CapturedFrameCount);

	TSharedPtr<FJsonObject> LogWindow = MakeShared<FJsonObject>();
	FString Log;
	if (!Job.EditorLogPath.IsEmpty()
		&& FFileHelper::LoadFileToString(Log, *Job.EditorLogPath))
	{
		const bool bTruncated = Log.Len() > 262144;
		if (bTruncated)
		{
			Log.RightInline(262144, false);
		}
		TArray<FString> LogLines;
		Log.ParseIntoArrayLines(LogLines, false);
		LogWindow->SetBoolField(TEXT("available"), true);
		LogWindow->SetBoolField(TEXT("truncated"), bTruncated);
		LogWindow->SetNumberField(TEXT("lineCount"), LogLines.Num());
		LogWindow->SetStringField(TEXT("content"), Log);
		LogWindow->SetStringField(TEXT("path"), Job.EditorLogPath);
	}
	else
	{
		LogWindow->SetBoolField(TEXT("available"), false);
	}
	Job.Result->SetArrayField(
		TEXT("logWindows"),
		{MakeShared<FJsonValueObject>(LogWindow)});
	return WritePerformanceReport(Job);
}

bool FProductionJobRuntime::WritePerformanceReport(FJob& Job)
{
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("ue.performance-run.v2"));
	Report->SetStringField(TEXT("runId"), Job.Id);
	Report->SetStringField(
		TEXT("mode"),
		GetStringFieldOr(Job.Result, TEXT("mode"), TEXT("window")));
	Report->SetStringField(
		TEXT("executionTarget"),
		GetStringFieldOr(
			Job.Result,
			TEXT("executionTarget"),
			TEXT("pie")));
	if (Job.Result->HasField(TEXT("samplingBasis")))
	{
		Report->SetStringField(
			TEXT("samplingBasis"),
			GetStringFieldOr(Job.Result, TEXT("samplingBasis")));
	}
	Report->SetObjectField(
		TEXT("context"),
		Job.Result->GetObjectField(TEXT("context")));
	Report->SetObjectField(
		TEXT("metrics"),
		Job.Result->GetObjectField(TEXT("metrics")));
	Report->SetNumberField(
		TEXT("sampleCount"),
		GetNumberFieldOr(Job.Result, TEXT("sampleCount"), 0.0));
	Report->SetNumberField(
		TEXT("repeatCount"),
		GetNumberFieldOr(Job.Result, TEXT("repeatCount"), 1.0));
	Report->SetNumberField(
		TEXT("completedRepeatCount"),
		GetNumberFieldOr(
			Job.Result,
			TEXT("completedRepeatCount"),
			0.0));
	if (Job.Result->HasTypedField<EJson::Array>(TEXT("repetitions")))
	{
		Report->SetArrayField(
			TEXT("repetitions"),
			Job.Result->GetArrayField(TEXT("repetitions")));
	}
	if (Job.Result->HasTypedField<EJson::Array>(TEXT("logWindows")))
	{
		Report->SetArrayField(
			TEXT("logWindows"),
			Job.Result->GetArrayField(TEXT("logWindows")));
	}
	if (Job.Result->HasTypedField<EJson::Object>(
		TEXT("runtimeFingerprint")))
	{
		Report->SetObjectField(
			TEXT("runtimeFingerprint"),
			CopyObject(
				Job.Result->GetObjectField(TEXT("runtimeFingerprint"))));
	}
	if (Job.Result->HasTypedField<EJson::Object>(TEXT("camera")))
	{
		Report->SetObjectField(
			TEXT("camera"),
			CopyObject(Job.Result->GetObjectField(TEXT("camera"))));
	}
	if (Job.Result->HasTypedField<EJson::Array>(
		TEXT("csvColumnMappings")))
	{
		Report->SetArrayField(
			TEXT("csvColumnMappings"),
			Job.Result->GetArrayField(TEXT("csvColumnMappings")));
	}
	if (Job.Result->HasField(TEXT("traceId")))
	{
		Report->SetStringField(
			TEXT("traceId"),
			GetStringFieldOr(Job.Result, TEXT("traceId")));
		Report->SetBoolField(
			TEXT("traceCompleted"),
			GetBoolFieldOr(Job.Result, TEXT("traceCompleted"), false));
		if (Job.Result->HasTypedField<EJson::Array>(
				TEXT("traceArtifacts")))
		{
			Report->SetArrayField(
				TEXT("traceArtifacts"),
				Job.Result->GetArrayField(TEXT("traceArtifacts")));
		}
	}
	if (Job.Result->HasField(TEXT("traceArtifactId")))
	{
		Report->SetStringField(
			TEXT("traceArtifactId"),
			GetStringFieldOr(
				Job.Result,
				TEXT("traceArtifactId")));
	}
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report.ToSharedRef(), Writer))
	{
		return false;
	}
	const FString Path =
		FPaths::Combine(JobDirectory(Job.Id), TEXT("performance.json"));
	return FFileHelper::SaveStringToFile(Json, *Path)
		&& AddArtifact(
			Job,
			Path,
			TEXT("performance.json"),
			TEXT("application/json")) != nullptr;
}

bool FProductionJobRuntime::WriteTraceAnalysisReport(FJob& Job)
{
	if (!Job.Result.IsValid()
		|| !Job.Result->HasTypedField<EJson::Object>(TEXT("analysis")))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("schema"), TEXT("ue.trace-analysis.v1"));
	Report->SetStringField(TEXT("jobId"), Job.Id);
	Report->SetStringField(
		TEXT("traceId"),
		GetStringFieldOr(Job.Result, TEXT("traceId")));
	Report->SetObjectField(
		TEXT("analysis"),
		CopyObject(Job.Result->GetObjectField(TEXT("analysis"))));
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Report.ToSharedRef(), Writer))
	{
		return false;
	}
	const FString Path =
		FPaths::Combine(JobDirectory(Job.Id), TEXT("trace-analysis.json"));
	return FFileHelper::SaveStringToFile(Json, *Path)
		&& AddArtifact(
			Job,
			Path,
			TEXT("trace-analysis.json"),
			TEXT("application/json")) != nullptr;
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
	TSharedPtr<FJsonObject> PhaseDurations = MakeShared<FJsonObject>();
	static const TArray<FString> ProcessPhases = {
		TEXT("launching"),
		TEXT("loading"),
		TEXT("discovering"),
		TEXT("running"),
		TEXT("reporting"),
		TEXT("exiting"),
		TEXT("complete")
	};
	for (const FString& Phase : ProcessPhases)
	{
		double Duration =
			Job.PhaseDurationsSeconds.FindRef(Phase);
		if (Job.Phase == Phase
			&& !IsTerminalStatus(Job.Status)
			&& Job.PhaseStartedAtSeconds > 0.0)
		{
			Duration += FMath::Max(
				0.0,
				FPlatformTime::Seconds()
					- Job.PhaseStartedAtSeconds);
		}
		PhaseDurations->SetNumberField(Phase, Duration);
	}
	Data->SetObjectField(
		TEXT("phaseDurationsSeconds"),
		PhaseDurations);
	TArray<TSharedPtr<FJsonValue>> PhaseHistory;
	for (const FString& Phase : Job.PhaseHistory)
	{
		PhaseHistory.Add(MakeShared<FJsonValueString>(Phase));
	}
	Data->SetArrayField(TEXT("phaseHistory"), PhaseHistory);
	TSharedPtr<FJsonObject> Timeouts = MakeShared<FJsonObject>();
	Timeouts->SetNumberField(
		TEXT("startupTimeoutSeconds"),
		Job.StartupTimeoutSeconds);
	Timeouts->SetNumberField(
		TEXT("executionTimeoutSeconds"),
		Job.ExecutionTimeoutSeconds);
	Timeouts->SetNumberField(
		TEXT("shutdownTimeoutSeconds"),
		Job.ShutdownTimeoutSeconds);
	Timeouts->SetNumberField(
		TEXT("hardTimeoutSeconds"),
		Job.HardTimeoutSeconds);
	Timeouts->SetNumberField(
		TEXT("hardLimitSeconds"),
		MaxHardTimeoutSeconds);
	Data->SetObjectField(TEXT("timeouts"), Timeouts);
	if (!Job.HeadlessProfile.IsEmpty())
	{
		Data->SetStringField(
			TEXT("headlessProfile"),
			Job.HeadlessProfile);
	}
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
	Context->SetStringField(TEXT("cpu"), FPlatformMisc::GetCPUBrand());
	Context->SetStringField(
		TEXT("resolution"),
		FString::Printf(
			TEXT("%dx%d"),
			GSystemResolution.ResX,
			GSystemResolution.ResY));
	Context->SetStringField(
		TEXT("configuration"),
		LexToString(FApp::GetBuildConfiguration()));
	Context->SetStringField(
		TEXT("engineVersion"),
		FEngineVersion::Current().ToString());
	Context->SetStringField(
		TEXT("platform"),
		ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
	const FString Fingerprint = DigestJson(Context);
	if (!Fingerprint.IsEmpty())
	{
		Context->SetStringField(
			TEXT("fingerprint"),
			TEXT("sha256:") + Fingerprint);
	}
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
		Job->StartupTimeoutSeconds =
			GetNumberFieldOr(
				Root,
				TEXT("startupTimeoutSeconds"),
				300.0);
		Job->ExecutionTimeoutSeconds =
			GetNumberFieldOr(
				Root,
				TEXT("executionTimeoutSeconds"),
				1800.0);
		Job->ShutdownTimeoutSeconds =
			GetNumberFieldOr(
				Root,
				TEXT("shutdownTimeoutSeconds"),
				60.0);
		Job->HardTimeoutSeconds =
			GetNumberFieldOr(
				Root,
				TEXT("hardTimeoutSeconds"),
				FMath::Min(
					MaxHardTimeoutSeconds,
					Job->StartupTimeoutSeconds
						+ Job->ExecutionTimeoutSeconds
						+ Job->ShutdownTimeoutSeconds));
		Job->EditorLogPath =
			GetStringFieldOr(Root, TEXT("editorLogPath"));
		Job->ReportDirectory =
			GetStringFieldOr(Root, TEXT("reportDirectory"));
		Job->HeadlessProfile =
			GetStringFieldOr(Root, TEXT("headlessProfile"));
		Job->bEditorProcess =
			GetBoolFieldOr(Root, TEXT("editorProcess"), false);
		Job->bAutomationProcess =
			GetBoolFieldOr(
				Root,
				TEXT("automationProcess"),
				false);
		if (Root->HasTypedField<EJson::Object>(
				TEXT("phaseDurationsSeconds")))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
				Root->GetObjectField(
					TEXT("phaseDurationsSeconds"))->Values)
			{
				double Duration = 0.0;
				if (Pair.Value.IsValid()
					&& Pair.Value->TryGetNumber(Duration))
				{
					Job->PhaseDurationsSeconds.Add(
						Pair.Key,
						FMath::Max(0.0, Duration));
				}
			}
		}
		if (Root->HasTypedField<EJson::Array>(TEXT("phaseHistory")))
		{
			for (const TSharedPtr<FJsonValue>& Value :
				Root->GetArrayField(TEXT("phaseHistory")))
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Job->PhaseHistory.Add(Value->AsString());
				}
			}
		}
		if (Job->PhaseHistory.IsEmpty() && !Job->Phase.IsEmpty())
		{
			Job->PhaseHistory.Add(Job->Phase);
		}
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
		Job->PhaseStartedAtSeconds = FPlatformTime::Seconds();
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
	Root->SetNumberField(
		TEXT("startupTimeoutSeconds"),
		Job.StartupTimeoutSeconds);
	Root->SetNumberField(
		TEXT("executionTimeoutSeconds"),
		Job.ExecutionTimeoutSeconds);
	Root->SetNumberField(
		TEXT("shutdownTimeoutSeconds"),
		Job.ShutdownTimeoutSeconds);
	Root->SetNumberField(
		TEXT("hardTimeoutSeconds"),
		Job.HardTimeoutSeconds);
	Root->SetStringField(
		TEXT("editorLogPath"),
		Job.EditorLogPath);
	Root->SetStringField(
		TEXT("reportDirectory"),
		Job.ReportDirectory);
	Root->SetStringField(
		TEXT("headlessProfile"),
		Job.HeadlessProfile);
	Root->SetBoolField(
		TEXT("editorProcess"),
		Job.bEditorProcess);
	Root->SetBoolField(
		TEXT("automationProcess"),
		Job.bAutomationProcess);
	TSharedPtr<FJsonObject> PhaseDurations = MakeShared<FJsonObject>();
	for (const TPair<FString, double>& Pair :
		Job.PhaseDurationsSeconds)
	{
		PhaseDurations->SetNumberField(Pair.Key, Pair.Value);
	}
	Root->SetObjectField(
		TEXT("phaseDurationsSeconds"),
		PhaseDurations);
	TArray<TSharedPtr<FJsonValue>> PhaseHistory;
	for (const FString& Phase : Job.PhaseHistory)
	{
		PhaseHistory.Add(MakeShared<FJsonValueString>(Phase));
	}
	Root->SetArrayField(TEXT("phaseHistory"), PhaseHistory);
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
