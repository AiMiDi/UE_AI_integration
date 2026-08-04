#include "TraceAnalysisService.h"

#include "Algo/Reverse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Common/ProviderLock.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/Containers/Tables.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/ModuleService.h"
#include "TraceServices/Model/AllocationsProvider.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Bookmarks.h"
#include "TraceServices/Model/Callstack.h"
#include "TraceServices/Model/Channel.h"
#include "TraceServices/Model/ContextSwitches.h"
#include "TraceServices/Model/Counters.h"
#include "TraceServices/Model/Diagnostics.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/LoadTimeProfiler.h"
#include "TraceServices/Model/Log.h"
#include "TraceServices/Model/Memory.h"
#include "TraceServices/Model/Modules.h"
#include "TraceServices/Model/NetProfiler.h"
#include "TraceServices/Model/Regions.h"
#include "TraceServices/Model/Screenshot.h"
#include "TraceServices/Model/StackSamples.h"
#include "TraceServices/Model/TasksProfiler.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace UEAI::Trace
{
namespace
{
constexpr double DefaultQueryWindowSeconds = 60.0;
constexpr double MaximumQueryWindowSeconds = 600.0;
constexpr int32 MaximumAggregationRows = 250000;
constexpr double MaximumAllocationQuerySeconds = 5.0;
constexpr int32 MaximumScreenshotPayloadBytes = 1024 * 1024;
const FString ManagedEngineMarkerPrefix =
	TEXT("UEAI_TRACE_ENGINE_VERSION=");

void EnableSemanticAnalysisModules(ITraceServicesModule& Module)
{
	const TSharedPtr<TraceServices::IModuleService> ModuleService =
		Module.GetModuleService();
	if (!ModuleService.IsValid())
	{
		return;
	}

	// TraceServices deliberately disables these analyzers by default in Editor
	// targets. UEAITraceWorker is a Program target, so relying on Engine defaults
	// would make the same Core and .utrace expose different providers in the two
	// processes. Enable the semantic-query modules before every new session.
	static const FName RequiredModules[] = {
		TEXT("TraceModule_LoadTimeProfiler"),
		TEXT("TraceModule_Memory"),
		TEXT("TraceModule_Tasks")
	};
	for (const FName& RequiredModule : RequiredModules)
	{
		ModuleService->SetModuleEnabled(RequiredModule, true);
	}
}

bool ParseMajorMinor(
	const FString& Value,
	int32& OutMajor,
	int32& OutMinor)
{
	OutMajor = 0;
	OutMinor = 0;
	for (int32 Start = 0; Start < Value.Len(); ++Start)
	{
		if (!FChar::IsDigit(Value[Start]))
		{
			continue;
		}
		int32 Dot = Start;
		while (Dot < Value.Len() && FChar::IsDigit(Value[Dot]))
		{
			++Dot;
		}
		if (Dot >= Value.Len() || Value[Dot] != TEXT('.'))
		{
			continue;
		}
		int32 End = Dot + 1;
		while (End < Value.Len() && FChar::IsDigit(Value[End]))
		{
			++End;
		}
		if (End == Dot + 1
			|| !LexTryParseString(OutMajor, *Value.Mid(Start, Dot - Start))
			|| !LexTryParseString(OutMinor, *Value.Mid(Dot + 1, End - Dot - 1)))
		{
			continue;
		}
		if (OutMajor >= 4 && OutMajor <= 9 && OutMinor >= 0 && OutMinor < 100)
		{
			return true;
		}
	}
	return false;
}

bool ParseExactMajorMinor(
	const FString& Value,
	int32& OutMajor,
	int32& OutMinor)
{
	OutMajor = 0;
	OutMinor = 0;
	FString MajorText;
	FString MinorText;
	if (!Value.Split(TEXT("."), &MajorText, &MinorText)
		|| MajorText.IsEmpty() || MinorText.IsEmpty()
		|| MinorText.Contains(TEXT(".")))
	{
		return false;
	}
	for (const TCHAR Character : MajorText + MinorText)
	{
		if (!FChar::IsDigit(Character))
		{
			return false;
		}
	}
	return LexTryParseString(OutMajor, *MajorText)
		&& LexTryParseString(OutMinor, *MinorText)
		&& OutMajor >= 4 && OutMajor <= 9
		&& OutMinor >= 0 && OutMinor < 100;
}

struct FManagedEngineMarker
{
	bool bFound = false;
	bool bInvalid = false;
	bool bConflicting = false;
	int32 Major = 0;
	int32 Minor = 0;
	FString Text;
};

FManagedEngineMarker ReadManagedEngineMarker(
	const TraceServices::IAnalysisSession& Session,
	const double DurationSeconds)
{
	FManagedEngineMarker Result;
	const TraceServices::IBookmarkProvider* Provider =
		Session.ReadProvider<TraceServices::IBookmarkProvider>(
			TraceServices::GetBookmarkProviderName());
	if (!Provider)
	{
		return Result;
	}
	Provider->EnumerateBookmarks(
		0.0,
		FMath::Max(DurationSeconds, 0.0) + UE_DOUBLE_SMALL_NUMBER,
		[&Result](const TraceServices::FBookmark& Bookmark)
		{
			const FString Text = Bookmark.Text ? Bookmark.Text : TEXT("");
			if (!Text.StartsWith(
					ManagedEngineMarkerPrefix,
					ESearchCase::CaseSensitive))
			{
				return;
			}
			int32 Major = 0;
			int32 Minor = 0;
			if (!ParseExactMajorMinor(
					Text.Mid(ManagedEngineMarkerPrefix.Len()), Major, Minor))
			{
				Result.bInvalid = true;
				return;
			}
			if (Result.bFound
				&& (Result.Major != Major || Result.Minor != Minor))
			{
				Result.bConflicting = true;
				return;
			}
			Result.bFound = true;
			Result.Major = Major;
			Result.Minor = Minor;
			Result.Text = Text;
		});
	return Result;
}

struct FResolvedRange
{
	double Start = 0.0;
	double End = 0.0;
	bool bClamped = false;
};

FTraceProviderDescriptor MakeDescriptor(
	const TCHAR* Id,
	const TCHAR* DisplayName,
	const TCHAR* InsightsPanel,
	TArray<FString> ProviderNames,
	TArray<FString> RequiredChannels,
	TArray<FString> Operations,
	const bool bQueryImplemented)
{
	FTraceProviderDescriptor Result;
	Result.Id = Id;
	Result.DisplayName = DisplayName;
	Result.InsightsPanel = InsightsPanel;
	Result.ProviderNames = MoveTemp(ProviderNames);
	Result.RequiredChannels = MoveTemp(RequiredChannels);
	Result.Operations = MoveTemp(Operations);
	Result.bQueryImplemented = bQueryImplemented;
	return Result;
}

FResolvedRange ResolveRange(
	const FTraceTimeRange& Requested,
	const double Duration)
{
	FResolvedRange Result;
	const double SafeDuration =
		FMath::IsFinite(Duration) && Duration > 0.0 ? Duration : 0.0;
	Result.End = Requested.bHasEnd
		? FMath::Clamp(Requested.EndSeconds, 0.0, SafeDuration)
		: SafeDuration;
	Result.Start = Requested.bHasStart
		? FMath::Clamp(Requested.StartSeconds, 0.0, Result.End)
		: FMath::Max(0.0, Result.End - DefaultQueryWindowSeconds);
	if (Result.End - Result.Start > MaximumQueryWindowSeconds)
	{
		Result.Start = Result.End - MaximumQueryWindowSeconds;
		Result.bClamped = true;
	}
	return Result;
}

bool MatchesFilter(const FString& Value, const FString& Filter)
{
	return Filter.IsEmpty()
		|| Value.Contains(Filter, ESearchCase::IgnoreCase);
}

int32 ClampLimit(const int32 Requested)
{
	return FMath::Clamp(Requested, 1, MaximumPageLimit);
}

void AddRangeDiagnostic(
	const FResolvedRange& Range,
	FTraceQueryResult& Result)
{
	if (!Range.bClamped)
	{
		return;
	}
	FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Severity = TEXT("warning");
	Diagnostic.Code = TEXT("trace_query_interval_clamped");
	Diagnostic.Message = FString::Printf(
		TEXT("The query interval was bounded to the last %.0f seconds."),
		MaximumQueryWindowSeconds);
}

bool IsOnRequestedPage(
	const uint64 MatchingIndex,
	const FTracePageRequest& Page,
	const int32 Limit)
{
	return MatchingIndex >= Page.Cursor
		&& MatchingIndex - Page.Cursor < static_cast<uint64>(Limit);
}

void FinishPage(
	const uint64 MatchingRows,
	const FTracePageRequest& Page,
	const int32 Limit,
	FTraceQueryResult& Result)
{
	Result.TotalRows = MatchingRows;
	const uint64 ReturnedRows = static_cast<uint64>(Result.Rows.Num());
	const uint64 PageEnd = Page.Cursor
		> TNumericLimits<uint64>::Max() - ReturnedRows
		? TNumericLimits<uint64>::Max()
		: Page.Cursor + ReturnedRows;
	Result.bHasNextCursor = PageEnd < MatchingRows;
	Result.NextCursor = Result.bHasNextCursor ? PageEnd : 0;
	Result.bTruncated = Result.bHasNextCursor
		|| Page.Cursor > 0
		|| MatchingRows > static_cast<uint64>(Limit);
}

FString Option(const FTraceQueryRequest& Request, const TCHAR* Name)
{
	if (const FString* Value = Request.Options.Find(Name))
	{
		return *Value;
	}
	return FString();
}

bool OptionBool(
	const FTraceQueryRequest& Request,
	const TCHAR* Name,
	const bool DefaultValue)
{
	const FString Value = Option(Request, Name);
	if (Value.IsEmpty())
	{
		return DefaultValue;
	}
	return Value.Equals(TEXT("true"), ESearchCase::IgnoreCase)
		|| Value == TEXT("1");
}

bool OptionUInt32(
	const FTraceQueryRequest& Request,
	const TCHAR* Name,
	uint32& OutValue)
{
	const FString Value = Option(Request, Name);
	return !Value.IsEmpty() && LexTryParseString(OutValue, *Value);
}

bool OptionUInt64(
	const FTraceQueryRequest& Request,
	const TCHAR* Name,
	uint64& OutValue)
{
	const FString Value = Option(Request, Name);
	return !Value.IsEmpty() && LexTryParseString(OutValue, *Value);
}

/**
 * The JSON-free Core receives array options serialized by its adapters. Accept
 * a JSON string array as well as comma/semicolon-delimited text so the Editor
 * and Worker can share one bounded contract without depending on Json here.
 */
TArray<FString> ParseStringListOption(
	const FTraceQueryRequest& Request,
	const TCHAR* Name,
	const int32 MaximumItems = 64)
{
	FString Value = Option(Request, Name).TrimStartAndEnd();
	TArray<FString> Result;
	if (Value.IsEmpty())
	{
		return Result;
	}

	if (Value.StartsWith(TEXT("[")) && Value.EndsWith(TEXT("]")))
	{
		Value = Value.Mid(1, Value.Len() - 2);
		FString Current;
		bool bInString = false;
		bool bEscape = false;
		for (const TCHAR Character : Value)
		{
			if (bEscape)
			{
				Current.AppendChar(Character);
				bEscape = false;
				continue;
			}
			if (bInString && Character == TEXT('\\'))
			{
				bEscape = true;
				continue;
			}
			if (Character == TEXT('"'))
			{
				bInString = !bInString;
				continue;
			}
			if (!bInString && Character == TEXT(','))
			{
				const FString Item = Current.TrimStartAndEnd();
				if (!Item.IsEmpty() && Result.Num() < MaximumItems)
				{
					Result.Add(Item);
				}
				Current.Reset();
				continue;
			}
			Current.AppendChar(Character);
		}
		const FString Item = Current.TrimStartAndEnd();
		if (!Item.IsEmpty() && Result.Num() < MaximumItems)
		{
			Result.Add(Item);
		}
	}
	else
	{
		Value.ReplaceInline(TEXT(";"), TEXT(","));
		Value.ParseIntoArray(Result, TEXT(","), true);
		for (FString& Item : Result)
		{
			Item.TrimStartAndEndInline();
		}
		if (Result.Num() > MaximumItems)
		{
			Result.SetNum(MaximumItems);
		}
	}
	Result.RemoveAll([](const FString& Item)
	{
		return Item.IsEmpty() || Item.Len() > 256;
	});
	return Result;
}

bool MatchesAnyRequestedName(
	const FString& Name,
	const TArray<FString>& RequestedNames)
{
	if (RequestedNames.IsEmpty())
	{
		return true;
	}
	return RequestedNames.ContainsByPredicate(
		[&Name](const FString& Requested)
		{
			return Name.Equals(Requested, ESearchCase::IgnoreCase);
		});
}

void AddCollectionBoundDiagnostic(FTraceQueryResult& Result)
{
	FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
	Diagnostic.Severity = TEXT("warning");
	Diagnostic.Code = TEXT("trace_query_collection_bounded");
	Diagnostic.Message = FString::Printf(
		TEXT("The semantic adapter returned a bounded prefix of at most %d matching records; totalRows is a lower bound and the collection is incomplete."),
		MaximumAggregationRows);
	Result.bTruncated = true;
}

bool QueryFrames(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::IFrameProvider* Provider =
		Session.ReadProvider<TraceServices::IFrameProvider>(
			TraceServices::GetFrameProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain frame events.");
		return false;
	}

	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	Result.Columns = {
		TEXT("frameType"), TEXT("index"), TEXT("startSeconds"),
		TEXT("endSeconds"), TEXT("durationMs")};
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	uint64 MatchingRows = 0;
	auto Enumerate = [&](const ETraceFrameType FrameType, const TCHAR* Name)
	{
		Provider->EnumerateFrames(
			FrameType,
			Range.Start,
			Range.End,
			[&](const TraceServices::FFrame& Frame)
			{
				if (IsOnRequestedPage(MatchingRows, Request.Page, Limit))
				{
					FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
					Row.Add(TEXT("frameType"), FTraceValue::String(Name))
						.Add(TEXT("index"), FTraceValue::Integer(
							static_cast<int64>(Frame.Index)))
						.Add(TEXT("startSeconds"), FTraceValue::Number(Frame.StartTime))
						.Add(TEXT("endSeconds"), FTraceValue::Number(Frame.EndTime))
						.Add(TEXT("durationMs"), FTraceValue::Number(
							(Frame.EndTime - Frame.StartTime) * 1000.0));
				}
				++MatchingRows;
			});
	};
	Enumerate(TraceFrameType_Game, TEXT("game"));
	Enumerate(TraceFrameType_Rendering, TEXT("rendering"));
	FinishPage(MatchingRows, Request.Page, Limit, Result);
	return true;
}

struct FTimingTimerInfo
{
	uint32 Id = 0;
	FString Name;
	FString File;
	uint32 Line = 0;
	bool bGpu = false;
};

TMap<uint32, FTimingTimerInfo> ReadTimingTimers(
	const TraceServices::ITimingProfilerProvider& Provider)
{
	TMap<uint32, FTimingTimerInfo> Timers;
	Provider.ReadTimers([&Timers](
		const TraceServices::ITimingProfilerTimerReader& Reader)
	{
		for (uint32 Index = 0; Index < Reader.GetTimerCount(); ++Index)
		{
			const TraceServices::FTimingProfilerTimer* Timer =
				Reader.GetTimer(Index);
			if (!Timer)
			{
				continue;
			}
			FTimingTimerInfo Info;
			Info.Id = Timer->Id;
			Info.Name = Timer->Name ? Timer->Name : TEXT("<unnamed>");
			Info.File = Timer->File ? Timer->File : TEXT("");
			Info.Line = Timer->Line;
			Info.bGpu = Timer->IsGpuTimer != 0;
			Timers.Add(Info.Id, MoveTemp(Info));
		}
	});
	return Timers;
}

bool ResolveTimingTimerId(
	const FTraceQueryRequest& Request,
	const TMap<uint32, FTimingTimerInfo>& Timers,
	uint32& OutTimerId,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	if (OptionUInt32(Request, TEXT("timerId"), OutTimerId))
	{
		if (Timers.Contains(OutTimerId))
		{
			return true;
		}
		ErrorCode = TEXT("trace_timer_not_found");
		ErrorMessage = FString::Printf(
			TEXT("Timing timer %u is not present in this trace."), OutTimerId);
		return false;
	}
	if (!Option(Request, TEXT("timerId")).IsEmpty())
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT("timerId must be an unsigned 32-bit timing timer id.");
		return false;
	}
	if (Request.Filter.IsEmpty())
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT(
			"Timing callers/callees require timerId or a filter that uniquely identifies one timer.");
		return false;
	}
	TArray<uint32> Matches;
	for (const TPair<uint32, FTimingTimerInfo>& Pair : Timers)
	{
		if (MatchesFilter(Pair.Value.Name, Request.Filter))
		{
			Matches.Add(Pair.Key);
		}
	}
	Matches.Sort();
	if (Matches.Num() != 1)
	{
		ErrorCode = Matches.IsEmpty()
			? TEXT("trace_timer_not_found")
			: TEXT("trace_timer_ambiguous");
		ErrorMessage = FString::Printf(
			TEXT("Timing filter '%s' matched %d timers; callers/callees require exactly one."),
			*Request.Filter, Matches.Num());
		return false;
	}
	OutTimerId = Matches[0];
	return true;
}

bool QueryTimingEvents(
	const TraceServices::IAnalysisSession& Session,
	const TraceServices::ITimingProfilerProvider& Provider,
	const TMap<uint32, FTimingTimerInfo>& Timers,
	const FTraceQueryRequest& Request,
	const FResolvedRange& Range,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	struct FEventRow
	{
		uint32 ThreadId = 0;
		FString ThreadName;
		bool bGpu = false;
		uint32 Depth = 0;
		uint32 TimerId = 0;
		FString TimerName;
		double Start = 0.0;
		double End = 0.0;
	};
	TArray<FEventRow> Events;
	bool bCollectionBounded = false;
	uint32 RequestedThread = 0;
	const bool bHasRequestedThread =
		OptionUInt32(Request, TEXT("threadId"), RequestedThread);
	if (!Option(Request, TEXT("threadId")).IsEmpty() && !bHasRequestedThread)
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT("threadId must be an unsigned 32-bit thread id.");
		return false;
	}
	uint32 RequestedTimer = 0;
	const FString TimerOption = Option(Request, TEXT("timerId"));
	const bool bHasRequestedTimer =
		OptionUInt32(Request, TEXT("timerId"), RequestedTimer);
	if (!TimerOption.IsEmpty() && !bHasRequestedTimer)
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT("timerId must be an unsigned 32-bit timing timer id.");
		return false;
	}
	if (bHasRequestedTimer && !Timers.Contains(RequestedTimer))
	{
		ErrorCode = TEXT("trace_timer_not_found");
		ErrorMessage = FString::Printf(
			TEXT("Timing timer %u is not present in this trace."), RequestedTimer);
		return false;
	}
	const bool bIncludeGpu = OptionBool(Request, TEXT("includeGpu"), true);

	auto ReadTimeline = [&](
		const uint32 TimelineIndex,
		const uint32 ThreadId,
		const FString& ThreadName,
		const bool bGpu)
	{
		if (bCollectionBounded
			|| (bHasRequestedThread && ThreadId != RequestedThread))
		{
			return;
		}
		Provider.ReadTimers(
			[&](const TraceServices::ITimingProfilerTimerReader& TimerReader)
			{
				Provider.ReadTimeline(
					TimelineIndex,
					[&](
						const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
					{
						Timeline.EnumerateEvents(
							Range.Start,
							Range.End,
							[&](
								const double Start,
								const double End,
								const uint32 Depth,
								const TraceServices::FTimingProfilerEvent& Event)
							{
								const TraceServices::FTimingProfilerTimer* Timer =
									TimerReader.GetTimer(Event.TimerIndex);
								const uint32 CanonicalTimerId = Timer
									? Timer->Id
									: Event.TimerIndex;
								const FString Name = Timer && Timer->Name
									? Timer->Name
									: FString::Printf(
										TEXT("<timer:%u>"), Event.TimerIndex);
								if ((bHasRequestedTimer
										&& CanonicalTimerId != RequestedTimer)
									|| !MatchesFilter(Name, Request.Filter))
								{
									return TraceServices::EEventEnumerate::Continue;
								}
								if (Events.Num() >= MaximumAggregationRows)
								{
									bCollectionBounded = true;
									return TraceServices::EEventEnumerate::Stop;
								}
								Events.Add({
									ThreadId,
									ThreadName,
									bGpu,
									Depth,
									CanonicalTimerId,
									Name,
									Start,
									End});
								return TraceServices::EEventEnumerate::Continue;
							});
					});
			});
	};

	const TraceServices::IThreadProvider* ThreadProvider =
		Session.ReadProvider<TraceServices::IThreadProvider>(
			TraceServices::GetThreadProviderName());
	if (ThreadProvider)
	{
		struct FThreadTimeline
		{
			uint32 ThreadId = 0;
			uint32 TimelineIndex = 0;
			FString Name;
		};
		TArray<FThreadTimeline> ThreadTimelines;
		ThreadProvider->EnumerateThreads(
			[&](const TraceServices::FThreadInfo& Thread)
			{
				uint32 TimelineIndex = 0;
				if (Provider.GetCpuThreadTimelineIndex(Thread.Id, TimelineIndex))
				{
					ThreadTimelines.Add({
						Thread.Id,
						TimelineIndex,
						Thread.Name ? Thread.Name : TEXT("<unnamed>")});
				}
			});
		ThreadTimelines.Sort([](
			const FThreadTimeline& Left,
			const FThreadTimeline& Right)
		{
			return Left.ThreadId < Right.ThreadId;
		});
		for (const FThreadTimeline& Thread : ThreadTimelines)
		{
			ReadTimeline(
				Thread.TimelineIndex, Thread.ThreadId, Thread.Name, false);
		}
	}
	if (!bHasRequestedThread && bIncludeGpu)
	{
		uint32 TimelineIndex = 0;
		if (Provider.GetGpuTimelineIndex(TimelineIndex))
		{
			ReadTimeline(TimelineIndex, MAX_uint32 - 1, TEXT("GPU1"), true);
		}
		if (Provider.GetGpu2TimelineIndex(TimelineIndex))
		{
			ReadTimeline(TimelineIndex, MAX_uint32, TEXT("GPU2"), true);
		}
	}

	Events.Sort([](const FEventRow& Left, const FEventRow& Right)
	{
		if (Left.Start != Right.Start)
		{
			return Left.Start < Right.Start;
		}
		if (Left.ThreadId != Right.ThreadId)
		{
			return Left.ThreadId < Right.ThreadId;
		}
		if (Left.Depth != Right.Depth)
		{
			return Left.Depth < Right.Depth;
		}
		return Left.TimerId < Right.TimerId;
	});
	Result.Columns = {
		TEXT("threadId"), TEXT("threadName"), TEXT("gpu"), TEXT("depth"),
		TEXT("timerId"), TEXT("name"), TEXT("startSeconds"),
		TEXT("endSeconds"), TEXT("durationMs")};
	const int32 Limit = ClampLimit(Request.Page.Limit);
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Events.Num()) && Result.Rows.Num() < Limit;
		++Index)
	{
		const FEventRow& Event = Events[static_cast<int32>(Index)];
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("threadId"), FTraceValue::String(LexToString(Event.ThreadId)))
			.Add(TEXT("threadName"), FTraceValue::String(Event.ThreadName))
			.Add(TEXT("gpu"), FTraceValue::Boolean(Event.bGpu))
			.Add(TEXT("depth"), FTraceValue::Integer(Event.Depth))
			.Add(TEXT("timerId"), FTraceValue::Integer(Event.TimerId))
			.Add(TEXT("name"), FTraceValue::String(Event.TimerName))
			.Add(TEXT("startSeconds"), FTraceValue::Number(Event.Start))
			.Add(TEXT("endSeconds"), FTraceValue::Number(Event.End))
			.Add(TEXT("durationMs"), FTraceValue::Number(
				FMath::Max(0.0, Event.End - Event.Start) * 1000.0));
	}
	FinishPage(Events.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

bool QueryTimingButterfly(
	const TraceServices::ITimingProfilerProvider& Provider,
	const TMap<uint32, FTimingTimerInfo>& Timers,
	const FTraceQueryRequest& Request,
	const FResolvedRange& Range,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	uint32 TimerId = 0;
	if (!ResolveTimingTimerId(
		Request, Timers, TimerId, ErrorCode, ErrorMessage))
	{
		return false;
	}
	uint32 RequestedThread = 0;
	const bool bHasRequestedThread =
		OptionUInt32(Request, TEXT("threadId"), RequestedThread);
	if (!Option(Request, TEXT("threadId")).IsEmpty() && !bHasRequestedThread)
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT("threadId must be an unsigned 32-bit thread id.");
		return false;
	}
	const bool bIncludeGpu = OptionBool(
		Request, TEXT("includeGpu"), !bHasRequestedThread);
	TUniquePtr<TraceServices::ITimingProfilerButterfly> Butterfly(
		Provider.CreateButterfly(
			Range.Start,
			Range.End,
			[bHasRequestedThread, RequestedThread](const uint32 ThreadId)
			{
				return !bHasRequestedThread || ThreadId == RequestedThread;
			},
			bIncludeGpu));
	if (!Butterfly.IsValid())
	{
		ErrorCode = TEXT("trace_query_failed");
		ErrorMessage = TEXT("TraceServices could not build the timing caller/callee tree.");
		return false;
	}
	const TraceServices::FTimingProfilerButterflyNode& Root =
		Request.Operation == TEXT("callers")
			? Butterfly->GenerateCallersTree(TimerId)
			: Butterfly->GenerateCalleesTree(TimerId);
	struct FPendingNode
	{
		const TraceServices::FTimingProfilerButterflyNode* Node = nullptr;
		uint32 Depth = 0;
		uint32 ParentTimerId = MAX_uint32;
	};
	struct FButterflyRow
	{
		uint32 Depth = 0;
		uint32 ParentTimerId = MAX_uint32;
		uint32 TimerId = MAX_uint32;
		FString Name;
		uint64 Count = 0;
		double InclusiveMs = 0.0;
		double ExclusiveMs = 0.0;
	};
	TArray<FPendingNode> Pending;
	Pending.Add({&Root, 0, MAX_uint32});
	TArray<FButterflyRow> Rows;
	bool bCollectionBounded = false;
	while (!Pending.IsEmpty())
	{
		const FPendingNode Current = Pending.Pop(false);
		if (!Current.Node)
		{
			continue;
		}
		if (Rows.Num() >= MaximumAggregationRows)
		{
			bCollectionBounded = true;
			break;
		}
		const uint32 CurrentTimerId = Current.Node->Timer
			? Current.Node->Timer->Id
			: MAX_uint32;
		const FString Name = Current.Node->Timer && Current.Node->Timer->Name
			? Current.Node->Timer->Name
			: TEXT("<root>");
		Rows.Add({
			Current.Depth,
			Current.ParentTimerId,
			CurrentTimerId,
			Name,
			Current.Node->Count,
			Current.Node->InclusiveTime * 1000.0,
			Current.Node->ExclusiveTime * 1000.0});

		TArray<const TraceServices::FTimingProfilerButterflyNode*> Children;
		for (const TraceServices::FTimingProfilerButterflyNode* Child
			: Current.Node->Children)
		{
			if (Child)
			{
				Children.Add(Child);
			}
		}
		Children.Sort([](
			const TraceServices::FTimingProfilerButterflyNode& Left,
			const TraceServices::FTimingProfilerButterflyNode& Right)
		{
			if (Left.InclusiveTime != Right.InclusiveTime)
			{
				return Left.InclusiveTime > Right.InclusiveTime;
			}
			const uint32 LeftId = Left.Timer ? Left.Timer->Id : MAX_uint32;
			const uint32 RightId = Right.Timer ? Right.Timer->Id : MAX_uint32;
			return LeftId < RightId;
		});
		for (int32 Index = Children.Num() - 1; Index >= 0; --Index)
		{
			Pending.Add({Children[Index], Current.Depth + 1, CurrentTimerId});
		}
	}
	Result.Columns = {
		TEXT("depth"), TEXT("parentTimerId"), TEXT("timerId"), TEXT("name"),
		TEXT("instanceCount"), TEXT("inclusiveMs"), TEXT("exclusiveMs")};
	const int32 Limit = ClampLimit(Request.Page.Limit);
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Rows.Num()) && Result.Rows.Num() < Limit;
		++Index)
	{
		const FButterflyRow& Item = Rows[static_cast<int32>(Index)];
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("depth"), FTraceValue::Integer(Item.Depth))
			.Add(TEXT("parentTimerId"), Item.ParentTimerId == MAX_uint32
				? FTraceValue::Null()
				: FTraceValue::Integer(Item.ParentTimerId))
			.Add(TEXT("timerId"), Item.TimerId == MAX_uint32
				? FTraceValue::Null()
				: FTraceValue::Integer(Item.TimerId))
			.Add(TEXT("name"), FTraceValue::String(Item.Name))
			.Add(TEXT("instanceCount"), FTraceValue::String(LexToString(Item.Count)))
			.Add(TEXT("inclusiveMs"), FTraceValue::Number(Item.InclusiveMs))
			.Add(TEXT("exclusiveMs"), FTraceValue::Number(Item.ExclusiveMs));
	}
	FinishPage(Rows.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

bool QueryCpuSampling(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const FResolvedRange& Range,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::IStackSamplesProvider* SamplesProvider =
		Session.ReadProvider<TraceServices::IStackSamplesProvider>(
			TraceServices::GetStackSamplesProviderName());
	const TraceServices::IThreadProvider* ThreadProvider =
		Session.ReadProvider<TraceServices::IThreadProvider>(
			TraceServices::GetThreadProviderName());
	if (!SamplesProvider || !ThreadProvider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain CPU stack samples and thread metadata.");
		return false;
	}
	TraceServices::IModuleProvider* ModuleProvider = const_cast<
		TraceServices::IModuleProvider*>(
			Session.ReadProvider<TraceServices::IModuleProvider>(
				TraceServices::GetModuleProviderName()));
	struct FSampleFrameRow
	{
		double Time = 0.0;
		uint32 ThreadId = 0;
		FString ThreadName;
		uint64 SampleOrdinal = 0;
		uint32 Depth = 0;
		uint64 Address = 0;
		FString Module;
		FString Symbol;
		FString File;
		uint32 Line = 0;
		FString ResolveStatus;
	};
	TArray<FSampleFrameRow> Frames;
	bool bCollectionBounded = false;
	bool bStackTruncated = false;
	bool bMalformedStackSample = false;
	uint32 RequestedThread = 0;
	const bool bHasRequestedThread =
		OptionUInt32(Request, TEXT("threadId"), RequestedThread);
	if (!Option(Request, TEXT("threadId")).IsEmpty() && !bHasRequestedThread)
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT("threadId must be an unsigned 32-bit thread id.");
		return false;
	}
	struct FSampleThread
	{
		uint32 Id = 0;
		FString Name;
	};
	TArray<FSampleThread> Threads;
	ThreadProvider->EnumerateThreads(
		[&](const TraceServices::FThreadInfo& Thread)
		{
			if (!bHasRequestedThread || Thread.Id == RequestedThread)
			{
				Threads.Add({
					Thread.Id,
					Thread.Name ? Thread.Name : TEXT("<unnamed>")});
			}
		});
	Threads.Sort([](const FSampleThread& Left, const FSampleThread& Right)
	{
		return Left.Id < Right.Id;
	});
	for (const FSampleThread& Thread : Threads)
	{
		if (bCollectionBounded)
		{
			break;
		}
		const TraceServices::TPagedArray<TraceServices::FStackSample>* Samples =
			SamplesProvider->GetStackSamples(Thread.Id);
		if (!Samples)
		{
			continue;
		}
		uint64 SampleOrdinal = 0;
		for (const TraceServices::FStackSample& Sample : *Samples)
		{
			const uint64 CurrentSampleOrdinal = SampleOrdinal++;
			if (Sample.Time > Range.End)
			{
				// StackSamples are appended in timestamp order per thread by the
				// PlatformEvents analyzer, so no later sample can enter this range.
				break;
			}
			if (Sample.Time < Range.Start)
			{
				continue;
			}
			if (Sample.Count > 0 && Sample.Addresses == nullptr)
			{
				bMalformedStackSample = true;
				continue;
			}
			const uint32 DepthCount = FMath::Min<uint32>(Sample.Count, 512);
			bStackTruncated |= Sample.Count > DepthCount;
			for (uint32 Depth = 0; Depth < DepthCount; ++Depth)
			{
				const uint64 Address = Sample.Addresses[Depth];
				const TraceServices::FResolvedSymbol* Resolved =
					ModuleProvider ? ModuleProvider->GetSymbol(Address) : nullptr;
				const TraceServices::ESymbolQueryResult ResolveResult = Resolved
					? Resolved->GetResult()
					: TraceServices::ESymbolQueryResult::NotFound;
				const bool bResolvedFieldsStable = Resolved
					&& ResolveResult != TraceServices::ESymbolQueryResult::Pending;
				const FString Module = bResolvedFieldsStable && Resolved->Module
					? Resolved->Module : TEXT("");
				const FString Symbol = bResolvedFieldsStable && Resolved->Name
					? Resolved->Name : TEXT("<unresolved>");
				const FString File = bResolvedFieldsStable && Resolved->File
					? Resolved->File : TEXT("");
				const uint32 Line = bResolvedFieldsStable
					? static_cast<uint32>(Resolved->Line)
					: uint32(0);
				const FString AddressText = FString::Printf(
					TEXT("0x%016llx"), Address);
				if (!MatchesFilter(
					Thread.Name + TEXT(" ") + Module + TEXT(" ") + Symbol
						+ TEXT(" ") + AddressText,
					Request.Filter))
				{
					continue;
				}
				if (Frames.Num() >= MaximumAggregationRows)
				{
					bCollectionBounded = true;
					break;
				}
				Frames.Add({
					Sample.Time,
					Thread.Id,
					Thread.Name,
					CurrentSampleOrdinal,
					Depth,
					Address,
					Module,
					Symbol,
					File,
					Line,
					Resolved
						? TraceServices::QueryResultToString(ResolveResult)
						: TEXT("Unknown")});
			}
			if (bCollectionBounded)
			{
				break;
			}
		}
	}
	Frames.Sort([](const FSampleFrameRow& Left, const FSampleFrameRow& Right)
	{
		if (Left.Time != Right.Time)
		{
			return Left.Time < Right.Time;
		}
		if (Left.ThreadId != Right.ThreadId)
		{
			return Left.ThreadId < Right.ThreadId;
		}
		if (Left.SampleOrdinal != Right.SampleOrdinal)
		{
			return Left.SampleOrdinal < Right.SampleOrdinal;
		}
		if (Left.Depth != Right.Depth)
		{
			return Left.Depth < Right.Depth;
		}
		return Left.Address < Right.Address;
	});
	Result.Columns = {
		TEXT("timeSeconds"), TEXT("threadId"), TEXT("threadName"),
		TEXT("sampleOrdinal"), TEXT("depth"),
		TEXT("address"), TEXT("module"), TEXT("symbol"), TEXT("file"),
		TEXT("line"), TEXT("resolveStatus")};
	const int32 Limit = ClampLimit(Request.Page.Limit);
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Frames.Num()) && Result.Rows.Num() < Limit;
		++Index)
	{
		const FSampleFrameRow& Frame = Frames[static_cast<int32>(Index)];
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("timeSeconds"), FTraceValue::Number(Frame.Time))
			.Add(TEXT("threadId"), FTraceValue::Integer(Frame.ThreadId))
			.Add(TEXT("threadName"), FTraceValue::String(Frame.ThreadName))
			.Add(TEXT("sampleOrdinal"), FTraceValue::String(
				LexToString(Frame.SampleOrdinal)))
			.Add(TEXT("depth"), FTraceValue::Integer(Frame.Depth))
			.Add(TEXT("address"), FTraceValue::String(FString::Printf(
				TEXT("0x%016llx"), Frame.Address)))
			.Add(TEXT("module"), FTraceValue::String(Frame.Module))
			.Add(TEXT("symbol"), FTraceValue::String(Frame.Symbol))
			.Add(TEXT("file"), FTraceValue::String(Frame.File))
			.Add(TEXT("line"), FTraceValue::Integer(Frame.Line))
			.Add(TEXT("resolveStatus"), FTraceValue::String(Frame.ResolveStatus));
	}
	FinishPage(Frames.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	if (bStackTruncated)
	{
		FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Code = TEXT("trace_cpu_sample_stack_truncated");
		Diagnostic.Message = TEXT(
			"At least one CPU sample exceeded the bounded depth of 512 frames; sampleOrdinal preserves the remaining stack grouping.");
	}
	if (bMalformedStackSample)
	{
		FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Code = TEXT("trace_cpu_sample_malformed");
		Diagnostic.Message = TEXT(
			"At least one CPU sample declared stack frames without an address payload and was skipped.");
	}
	return true;
}

bool QueryTiming(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	if (Request.Operation == TEXT("frames"))
	{
		return QueryFrames(
			Session, Request, Duration, Result, ErrorCode, ErrorMessage);
	}
	if (Request.Operation == TEXT("cpuSampling"))
	{
		const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
		Result.IntervalStartSeconds = Range.Start;
		Result.IntervalEndSeconds = Range.End;
		AddRangeDiagnostic(Range, Result);
		return QueryCpuSampling(
			Session, Request, Range, Result, ErrorCode, ErrorMessage);
	}
	const TraceServices::ITimingProfilerProvider* Provider =
		Session.ReadProvider<TraceServices::ITimingProfilerProvider>(
			TraceServices::GetTimingProfilerProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain timing events.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const TMap<uint32, FTimingTimerInfo> Timers = ReadTimingTimers(*Provider);
	if (Request.Operation == TEXT("events"))
	{
		return QueryTimingEvents(
			Session, *Provider, Timers, Request, Range, Result,
			ErrorCode, ErrorMessage);
	}
	if (Request.Operation == TEXT("callers")
		|| Request.Operation == TEXT("callees"))
	{
		return QueryTimingButterfly(
			*Provider, Timers, Request, Range, Result, ErrorCode, ErrorMessage);
	}
	if (Request.Operation != TEXT("timers"))
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT(
			"Timing queries support frames, threads, events, timers, callers, callees, and cpuSampling.");
		return false;
	}
	Result.Columns = {
		TEXT("timerId"), TEXT("name"), TEXT("file"), TEXT("line"),
		TEXT("gpu"), TEXT("instanceCount"), TEXT("totalInclusiveMs"),
		TEXT("averageInclusiveMs"), TEXT("maxInclusiveMs"),
		TEXT("totalExclusiveMs")};

	TraceServices::FCreateAggreationParams Params;
	Params.IntervalStart = Range.Start;
	Params.IntervalEnd = Range.End;
	Params.CpuThreadFilter = [](const uint32) { return true; };
	Params.IncludeGpu = OptionBool(Request, TEXT("includeGpu"), true);
	TUniquePtr<TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>>
		Table(Provider->CreateAggregation(Params));
	if (!Table.IsValid())
	{
		ErrorCode = TEXT("trace_query_failed");
		ErrorMessage = TEXT("TraceServices could not aggregate timing events.");
		return false;
	}

	struct FTimerRow
	{
		TraceServices::FTimingProfilerAggregatedStats Stats;
	};
	TArray<FTimerRow> Rows;
	TUniquePtr<TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>>
		Reader(Table->CreateReader());
	bool bCollectionBounded = false;
	for (int32 Count = 0;
		Reader.IsValid() && Reader->IsValid() && Count < MaximumAggregationRows;
		Reader->NextRow(), ++Count)
	{
		const TraceServices::FTimingProfilerAggregatedStats* Row =
			Reader->GetCurrentRow();
		if (!Row || !Row->Timer)
		{
			continue;
		}
		const FString Name = Row->Timer->Name
			? Row->Timer->Name
			: TEXT("<unnamed>");
		if (MatchesFilter(Name, Request.Filter))
		{
			Rows.Add({*Row});
		}
	}
	if (Reader.IsValid() && Reader->IsValid())
	{
		bCollectionBounded = true;
	}
	Rows.Sort([](const FTimerRow& Left, const FTimerRow& Right)
	{
		if (Left.Stats.TotalInclusiveTime != Right.Stats.TotalInclusiveTime)
		{
			return Left.Stats.TotalInclusiveTime > Right.Stats.TotalInclusiveTime;
		}
		return Left.Stats.Timer->Id < Right.Stats.Timer->Id;
	});

	const int32 Limit = ClampLimit(Request.Page.Limit);
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Rows.Num())
			&& Result.Rows.Num() < Limit;
		++Index)
	{
		const TraceServices::FTimingProfilerAggregatedStats& Stats =
			Rows[static_cast<int32>(Index)].Stats;
		const TraceServices::FTimingProfilerTimer& Timer = *Stats.Timer;
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("timerId"), FTraceValue::Integer(Timer.Id))
			.Add(TEXT("name"), FTraceValue::String(
				Timer.Name ? Timer.Name : TEXT("<unnamed>")))
			.Add(TEXT("file"), FTraceValue::String(
				Timer.File ? Timer.File : TEXT("")))
			.Add(TEXT("line"), FTraceValue::Integer(Timer.Line))
			.Add(TEXT("gpu"), FTraceValue::Boolean(Timer.IsGpuTimer != 0))
			.Add(TEXT("instanceCount"), FTraceValue::Integer(
				static_cast<int64>(Stats.InstanceCount)))
			.Add(TEXT("totalInclusiveMs"), FTraceValue::Number(
				Stats.TotalInclusiveTime * 1000.0))
			.Add(TEXT("averageInclusiveMs"), FTraceValue::Number(
				Stats.AverageInclusiveTime * 1000.0))
			.Add(TEXT("maxInclusiveMs"), FTraceValue::Number(
				Stats.MaxInclusiveTime * 1000.0))
			.Add(TEXT("totalExclusiveMs"), FTraceValue::Number(
				Stats.TotalExclusiveTime * 1000.0));
	}
	FinishPage(Rows.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

bool QueryCounters(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::ICounterProvider* Provider =
		Session.ReadProvider<TraceServices::ICounterProvider>(
			TraceServices::GetCounterProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain counters.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	TArray<FString> RequestedNames = ParseStringListOption(Request, TEXT("names"));

	struct FCounterDefinition
	{
		uint32 Id = 0;
		const TraceServices::ICounter* Counter = nullptr;
		FString Name;
	};
	TArray<FCounterDefinition> Definitions;
	Provider->EnumerateCounters(
		[&](const uint32 CounterId, const TraceServices::ICounter& Counter)
		{
			const FString Name = Counter.GetName()
				? Counter.GetName()
				: TEXT("<unnamed>");
			if (MatchesAnyRequestedName(Name, RequestedNames)
				&& MatchesFilter(Name, Request.Filter))
			{
				Definitions.Add({CounterId, &Counter, Name});
			}
		});
	Definitions.Sort([](const FCounterDefinition& Left, const FCounterDefinition& Right)
	{
		const int32 NameOrder = Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase);
		return NameOrder != 0 ? NameOrder < 0 : Left.Id < Right.Id;
	});

	if (Request.Operation == TEXT("list"))
	{
		Result.Columns = {
			TEXT("counterId"), TEXT("name"), TEXT("group"), TEXT("description"),
			TEXT("floatingPoint"), TEXT("resetEveryFrame"), TEXT("displayHint")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Definitions.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FCounterDefinition& Definition = Definitions[static_cast<int32>(Index)];
			const TraceServices::ICounter& Counter = *Definition.Counter;
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("counterId"), FTraceValue::Integer(Definition.Id))
				.Add(TEXT("name"), FTraceValue::String(Definition.Name))
				.Add(TEXT("group"), FTraceValue::String(
					Counter.GetGroup() ? Counter.GetGroup() : TEXT("")))
				.Add(TEXT("description"), FTraceValue::String(
					Counter.GetDescription() ? Counter.GetDescription() : TEXT("")))
				.Add(TEXT("floatingPoint"), FTraceValue::Boolean(
					Counter.IsFloatingPoint()))
				.Add(TEXT("resetEveryFrame"), FTraceValue::Boolean(
					Counter.IsResetEveryFrame()))
				.Add(TEXT("displayHint"), FTraceValue::String(
					Counter.GetDisplayHint() == TraceServices::CounterDisplayHint_Memory
						? TEXT("memory") : TEXT("none")));
		}
		FinishPage(Definitions.Num(), Request.Page, Limit, Result);
		return true;
	}

	struct FCounterValue
	{
		uint32 Id = 0;
		FString Name;
		double Time = 0.0;
		double Value = 0.0;
	};
	TArray<FCounterValue> Values;
	bool bCollectionBounded = false;
	for (const FCounterDefinition& Definition : Definitions)
	{
		auto Append = [&](const double Time, const double Value)
		{
			if (Values.Num() < MaximumAggregationRows)
			{
				Values.Add({Definition.Id, Definition.Name, Time, Value});
			}
			else
			{
				bCollectionBounded = true;
			}
		};
		if (Definition.Counter->IsFloatingPoint())
		{
			Definition.Counter->EnumerateFloatValues(
				Range.Start, Range.End, false,
				[&](const double Time, const double Value) { Append(Time, Value); });
		}
		else
		{
			Definition.Counter->EnumerateValues(
				Range.Start, Range.End, false,
				[&](const double Time, const int64 Value)
				{
					Append(Time, static_cast<double>(Value));
				});
		}
	}

	if (Request.Operation == TEXT("series"))
	{
		Values.Sort([](const FCounterValue& Left, const FCounterValue& Right)
		{
			if (Left.Time != Right.Time)
			{
				return Left.Time < Right.Time;
			}
			const int32 NameOrder = Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase);
			return NameOrder != 0 ? NameOrder < 0 : Left.Id < Right.Id;
		});
		Result.Columns = {
			TEXT("counterId"), TEXT("name"), TEXT("timeSeconds"), TEXT("value")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Values.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FCounterValue& Value = Values[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("counterId"), FTraceValue::Integer(Value.Id))
				.Add(TEXT("name"), FTraceValue::String(Value.Name))
				.Add(TEXT("timeSeconds"), FTraceValue::Number(Value.Time))
				.Add(TEXT("value"), FTraceValue::Number(Value.Value));
		}
		FinishPage(Values.Num(), Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	if (Request.Operation == TEXT("aggregate"))
	{
		struct FAggregate
		{
			uint32 Id = 0;
			FString Name;
			uint64 Count = 0;
			double Min = TNumericLimits<double>::Max();
			double Max = -TNumericLimits<double>::Max();
			double Sum = 0.0;
			double LastTime = 0.0;
			double LastValue = 0.0;
		};
		TMap<uint32, FAggregate> ById;
		for (const FCounterValue& Value : Values)
		{
			FAggregate& Aggregate = ById.FindOrAdd(Value.Id);
			Aggregate.Id = Value.Id;
			Aggregate.Name = Value.Name;
			++Aggregate.Count;
			Aggregate.Min = FMath::Min(Aggregate.Min, Value.Value);
			Aggregate.Max = FMath::Max(Aggregate.Max, Value.Value);
			Aggregate.Sum += Value.Value;
			if (Aggregate.Count == 1 || Value.Time >= Aggregate.LastTime)
			{
				Aggregate.LastTime = Value.Time;
				Aggregate.LastValue = Value.Value;
			}
		}
		TArray<FAggregate> Aggregates;
		ById.GenerateValueArray(Aggregates);
		Aggregates.Sort([](const FAggregate& Left, const FAggregate& Right)
		{
			const int32 NameOrder = Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase);
			return NameOrder != 0 ? NameOrder < 0 : Left.Id < Right.Id;
		});
		Result.Columns = {
			TEXT("counterId"), TEXT("name"), TEXT("sampleCount"), TEXT("min"),
			TEXT("max"), TEXT("average"), TEXT("lastTimeSeconds"), TEXT("lastValue")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Aggregates.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FAggregate& Aggregate = Aggregates[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("counterId"), FTraceValue::Integer(Aggregate.Id))
				.Add(TEXT("name"), FTraceValue::String(Aggregate.Name))
				.Add(TEXT("sampleCount"), FTraceValue::Integer(Aggregate.Count))
				.Add(TEXT("min"), FTraceValue::Number(Aggregate.Min))
				.Add(TEXT("max"), FTraceValue::Number(Aggregate.Max))
				.Add(TEXT("average"), FTraceValue::Number(
					Aggregate.Count > 0 ? Aggregate.Sum / Aggregate.Count : 0.0))
				.Add(TEXT("lastTimeSeconds"), FTraceValue::Number(Aggregate.LastTime))
				.Add(TEXT("lastValue"), FTraceValue::Number(Aggregate.LastValue));
		}
		FinishPage(Aggregates.Num(), Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	ErrorCode = TEXT("trace_query_unsupported");
	ErrorMessage = TEXT("Counter queries support 'list', 'series', and 'aggregate'.");
	return false;
}

bool QueryLogs(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::ILogProvider* Provider =
		Session.ReadProvider<TraceServices::ILogProvider>(
			TraceServices::GetLogProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain log messages.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	Result.Columns = {
		TEXT("index"), TEXT("timeSeconds"), TEXT("category"), TEXT("verbosity"),
		TEXT("message"), TEXT("file"), TEXT("line")};
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	if (Request.Operation == TEXT("categories"))
	{
		struct FCategoryRow
		{
			FString Name;
			uint64 MessageCount = 0;
			uint64 ErrorCount = 0;
			uint64 WarningCount = 0;
		};
		TMap<FString, FCategoryRow> ByCategory;
		Provider->EnumerateMessages(
			Range.Start,
			Range.End,
			[&](const TraceServices::FLogMessageInfo& Message)
			{
				const FString Category = Message.Category && Message.Category->Name
					? Message.Category->Name : TEXT("<unknown>");
				if (!MatchesFilter(Category, Request.Filter))
				{
					return;
				}
				FCategoryRow& Row = ByCategory.FindOrAdd(Category);
				Row.Name = Category;
				++Row.MessageCount;
				if (Message.Verbosity == ELogVerbosity::Error
					|| Message.Verbosity == ELogVerbosity::Fatal)
				{
					++Row.ErrorCount;
				}
				else if (Message.Verbosity == ELogVerbosity::Warning)
				{
					++Row.WarningCount;
				}
			});
		TArray<FCategoryRow> Categories;
		ByCategory.GenerateValueArray(Categories);
		Categories.Sort([](const FCategoryRow& Left, const FCategoryRow& Right)
		{
			return Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase) < 0;
		});
		Result.Columns = {
			TEXT("category"), TEXT("messageCount"), TEXT("warningCount"), TEXT("errorCount")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Categories.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FCategoryRow& Category = Categories[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("category"), FTraceValue::String(Category.Name))
				.Add(TEXT("messageCount"), FTraceValue::String(LexToString(Category.MessageCount)))
				.Add(TEXT("warningCount"), FTraceValue::String(LexToString(Category.WarningCount)))
				.Add(TEXT("errorCount"), FTraceValue::String(LexToString(Category.ErrorCount)));
		}
		FinishPage(Categories.Num(), Request.Page, Limit, Result);
		return true;
	}
	if (Request.Operation != TEXT("messages"))
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Log queries support categories and messages.");
		return false;
	}
	uint64 MatchingRows = 0;
	Provider->EnumerateMessages(
		Range.Start,
		Range.End,
		[&](const TraceServices::FLogMessageInfo& Message)
		{
			const FString Text = Message.Message ? Message.Message : TEXT("");
			const FString Category = Message.Category && Message.Category->Name
				? Message.Category->Name
				: TEXT("<unknown>");
			if (!MatchesFilter(Text + TEXT(" ") + Category, Request.Filter))
			{
				return;
			}
			if (IsOnRequestedPage(MatchingRows, Request.Page, Limit))
			{
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("index"), FTraceValue::Integer(
					static_cast<int64>(Message.Index)))
					.Add(TEXT("timeSeconds"), FTraceValue::Number(Message.Time))
					.Add(TEXT("category"), FTraceValue::String(Category))
					.Add(TEXT("verbosity"), FTraceValue::String(
						ToString(Message.Verbosity)))
					.Add(TEXT("message"), FTraceValue::String(Text))
					.Add(TEXT("file"), FTraceValue::String(
						Message.File ? Message.File : TEXT("")))
					.Add(TEXT("line"), FTraceValue::Integer(Message.Line));
			}
			++MatchingRows;
		});
	FinishPage(MatchingRows, Request.Page, Limit, Result);
	return true;
}

bool QueryBookmarks(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	if (!Request.Operation.IsEmpty()
		&& Request.Operation != TEXT("list"))
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Bookmark queries support list.");
		return false;
	}
	const TraceServices::IBookmarkProvider* Provider =
		Session.ReadProvider<TraceServices::IBookmarkProvider>(
			TraceServices::GetBookmarkProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain bookmarks.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	Result.Columns = {TEXT("timeSeconds"), TEXT("text")};
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	uint64 MatchingRows = 0;
	Provider->EnumerateBookmarks(
		Range.Start,
		Range.End,
		[&](const TraceServices::FBookmark& Bookmark)
		{
			const FString Text = Bookmark.Text ? Bookmark.Text : TEXT("");
			if (!MatchesFilter(Text, Request.Filter))
			{
				return;
			}
			if (IsOnRequestedPage(MatchingRows, Request.Page, Limit))
			{
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("timeSeconds"), FTraceValue::Number(Bookmark.Time))
					.Add(TEXT("text"), FTraceValue::String(Text));
			}
			++MatchingRows;
		});
	FinishPage(MatchingRows, Request.Page, Limit, Result);
	return true;
}

bool QueryRegions(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	if (!Request.Operation.IsEmpty()
		&& Request.Operation != TEXT("list")
		&& Request.Operation != TEXT("ranges"))
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Region queries support list and ranges.");
		return false;
	}
	const TraceServices::IRegionProvider* Provider =
		Session.ReadProvider<TraceServices::IRegionProvider>(
			TraceServices::GetRegionProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain regions.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	Result.Columns = {
		TEXT("name"), TEXT("depth"), TEXT("beginSeconds"), TEXT("endSeconds"),
		TEXT("durationMs"), TEXT("openEnded")};
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	uint64 MatchingRows = 0;
	TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);
	Provider->EnumerateRegions(
		Range.Start,
		Range.End,
		[&](const TraceServices::FTimeRegion& Region)
		{
			const FString Name = Region.Text ? Region.Text : TEXT("<unnamed>");
			if (!MatchesFilter(Name, Request.Filter))
			{
				return true;
			}
			if (IsOnRequestedPage(MatchingRows, Request.Page, Limit))
			{
				const bool bOpenEnded = !FMath::IsFinite(Region.EndTime);
				const double End = bOpenEnded ? Range.End : Region.EndTime;
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("name"), FTraceValue::String(Name))
					.Add(TEXT("depth"), FTraceValue::Integer(Region.Depth))
					.Add(TEXT("beginSeconds"), FTraceValue::Number(Region.BeginTime))
					.Add(TEXT("endSeconds"), FTraceValue::Number(End))
					.Add(TEXT("durationMs"), FTraceValue::Number(
						FMath::Max(0.0, End - Region.BeginTime) * 1000.0))
					.Add(TEXT("openEnded"), FTraceValue::Boolean(bOpenEnded));
			}
			++MatchingRows;
			return true;
		});
	FinishPage(MatchingRows, Request.Page, Limit, Result);
	return true;
}

bool QueryThreads(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::IThreadProvider* Provider =
		Session.ReadProvider<TraceServices::IThreadProvider>(
			TraceServices::GetThreadProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain thread metadata.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	Result.Columns = {
		TEXT("threadId"), TEXT("name"), TEXT("group"),
		TEXT("hasTimingTrack"), TEXT("activeInRange")};
	const int32 Limit = ClampLimit(Request.Page.Limit);
	const TraceServices::ITimingProfilerProvider* TimingProvider =
		Session.ReadProvider<TraceServices::ITimingProfilerProvider>(
			TraceServices::GetTimingProfilerProviderName());
	struct FThreadRow
	{
		uint32 Id = 0;
		FString Name;
		FString Group;
		bool bHasTimingTrack = false;
		bool bActiveInRange = false;
	};
	TArray<FThreadRow> Threads;
	bool bCollectionBounded = false;
	Provider->EnumerateThreads(
		[&](const TraceServices::FThreadInfo& Thread)
		{
			const FString Name = Thread.Name ? Thread.Name : TEXT("<unnamed>");
			if (!MatchesFilter(Name, Request.Filter))
			{
				return;
			}
			if (Threads.Num() >= MaximumAggregationRows)
			{
				bCollectionBounded = true;
				return;
			}
			uint32 TimelineIndex = 0;
			const bool bHasTimingTrack = TimingProvider
				&& TimingProvider->GetCpuThreadTimelineIndex(
					Thread.Id, TimelineIndex);
			bool bActiveInRange = false;
			if (bHasTimingTrack)
			{
				TimingProvider->ReadTimeline(
					TimelineIndex,
					[&](const TraceServices::ITimingProfilerProvider::Timeline& Timeline)
					{
						Timeline.EnumerateEvents(
							Range.Start,
							Range.End,
							[&](
								const double,
								const double,
								const uint32,
								const TraceServices::FTimingProfilerEvent&)
							{
								bActiveInRange = true;
								return TraceServices::EEventEnumerate::Stop;
							});
					});
			}
			Threads.Add({
				Thread.Id,
				Name,
				Thread.GroupName ? Thread.GroupName : TEXT(""),
				bHasTimingTrack,
				bActiveInRange});
		});
	Threads.Sort([](const FThreadRow& Left, const FThreadRow& Right)
	{
		if (Left.Id != Right.Id)
		{
			return Left.Id < Right.Id;
		}
		const int32 NameOrder = Left.Name.Compare(
			Right.Name, ESearchCase::IgnoreCase);
		return NameOrder != 0
			? NameOrder < 0
			: Left.Group.Compare(Right.Group, ESearchCase::IgnoreCase) < 0;
	});
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Threads.Num())
			&& Result.Rows.Num() < Limit;
		++Index)
	{
		const FThreadRow& Thread = Threads[static_cast<int32>(Index)];
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("threadId"), FTraceValue::Integer(Thread.Id))
			.Add(TEXT("name"), FTraceValue::String(Thread.Name))
			.Add(TEXT("group"), FTraceValue::String(Thread.Group))
			.Add(
				TEXT("hasTimingTrack"),
				FTraceValue::Boolean(Thread.bHasTimingTrack))
			.Add(
				TEXT("activeInRange"),
				FTraceValue::Boolean(Thread.bActiveInRange));
	}
	FinishPage(Threads.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

struct FBoundedAllocationRow
{
	double StartTime = 0.0;
	double EndTime = 0.0;
	uint64 Address = 0;
	uint64 Size = 0;
	uint32 Alignment = 0;
	uint32 ThreadId = 0;
	uint32 CallstackId = 0;
	uint32 FreeCallstackId = 0;
	TraceServices::TagIdType TagId = 0;
	FString Tag;
};

bool CollectAllocations(
	const TraceServices::IAllocationsProvider& Provider,
	const TraceServices::IAllocationsProvider::EQueryRule Rule,
	const FResolvedRange& Range,
	const FString& Filter,
	TArray<FBoundedAllocationRow>& OutRows,
	bool& bOutBounded,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	bOutBounded = false;
	// aAf answers "which allocations are live at A". For the public
	// liveAllocations operation, A is the end of the selected interval; AaB
	// continues to use the whole interval for allocation events.
	const double TimeA =
		Rule == TraceServices::IAllocationsProvider::EQueryRule::aAf
			? Range.End
			: Range.Start;
	TraceServices::IAllocationsProvider::FQueryParams Params{
		Rule, TimeA, Range.End, Range.End, Range.End};
	TraceServices::IAllocationsProvider::FQueryHandle Handle = 0;
	{
		TraceServices::FProviderReadScopeLock ProviderReadScope(Provider);
		Handle = Provider.StartQuery(Params);
	}
	if (Handle == 0)
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("The allocations provider rejected the requested query rule.");
		return false;
	}

	const double Deadline = FPlatformTime::Seconds() + MaximumAllocationQuerySeconds;
	for (;;)
	{
		const TraceServices::IAllocationsProvider::FQueryStatus Status =
			Provider.PollQuery(Handle);
		if (Status.Status == TraceServices::IAllocationsProvider::EQueryStatus::Done)
		{
			break;
		}
		if (Status.Status == TraceServices::IAllocationsProvider::EQueryStatus::Unknown)
		{
			Provider.CancelQuery(Handle);
			ErrorCode = TEXT("trace_query_failed");
			ErrorMessage = TEXT("The allocations provider returned an unknown query state.");
			return false;
		}
		if (Status.Status == TraceServices::IAllocationsProvider::EQueryStatus::Available)
		{
			TraceServices::FProviderReadScopeLock ProviderReadScope(Provider);
			TraceServices::IAllocationsProvider::FQueryResult Page = Status.NextResult();
			while (Page.IsValid())
			{
				for (uint32 Index = 0; Index < Page->Num(); ++Index)
				{
					const TraceServices::IAllocationsProvider::FAllocation* Allocation =
						Page->Get(Index);
					if (!Allocation)
					{
						continue;
					}
					const TCHAR* FullPath = Provider.GetTagFullPath(Allocation->GetTag());
					const FString Tag = FullPath ? FullPath : TEXT("<untagged>");
					if (!MatchesFilter(Tag, Filter))
					{
						continue;
					}
					if (OutRows.Num() >= MaximumAggregationRows)
					{
						bOutBounded = true;
						continue;
					}
					FBoundedAllocationRow& Row = OutRows.AddDefaulted_GetRef();
					Row.StartTime = Allocation->GetStartTime();
					Row.EndTime = Allocation->GetEndTime();
					Row.Address = Allocation->GetAddress();
					Row.Size = Allocation->GetSize();
					Row.Alignment = Allocation->GetAlignment();
					Row.ThreadId = Allocation->GetThreadId();
					Row.CallstackId = Allocation->GetCallstackId();
					Row.FreeCallstackId = Allocation->GetFreeCallstackId();
					Row.TagId = Allocation->GetTag();
					Row.Tag = Tag;
				}
				Page = Status.NextResult();
			}
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Provider.CancelQuery(Handle);
			ErrorCode = TEXT("trace_query_timeout");
			ErrorMessage = FString::Printf(
				TEXT("The allocation query exceeded its %.0f second bound."),
				MaximumAllocationQuerySeconds);
			return false;
		}
		FPlatformProcess::SleepNoStats(0.005f);
	}
	OutRows.Sort([](const FBoundedAllocationRow& Left, const FBoundedAllocationRow& Right)
	{
		if (Left.StartTime != Right.StartTime)
		{
			return Left.StartTime < Right.StartTime;
		}
		return Left.Address < Right.Address;
	});
	return true;
}

bool QueryMemory(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);

	if (Request.Operation == TEXT("tags"))
	{
		struct FTagRow
		{
			FString Source;
			int64 Id = 0;
			int64 ParentId = 0;
			FString Name;
			FString FullPath;
			uint64 Trackers = 0;
		};
		TArray<FTagRow> Tags;
		const TraceServices::IMemoryProvider* MemoryProvider =
			Session.ReadProvider<TraceServices::IMemoryProvider>(
				TraceServices::GetMemoryProviderName());
		if (MemoryProvider && MemoryProvider->GetTagCount() > 0)
		{
			MemoryProvider->EnumerateTags([&](const TraceServices::FMemoryTagInfo& Tag)
			{
				if (MatchesFilter(Tag.Name, Request.Filter))
				{
					Tags.Add({TEXT("llm"), Tag.Id, Tag.ParentId, Tag.Name, Tag.Name, Tag.Trackers});
				}
			});
		}
		const TraceServices::IAllocationsProvider* AllocationsProvider =
			Session.ReadProvider<TraceServices::IAllocationsProvider>(
				TraceServices::GetAllocationsProviderName());
		if (AllocationsProvider)
		{
			TraceServices::FProviderReadScopeLock ProviderReadScope(*AllocationsProvider);
			if (AllocationsProvider->IsInitialized())
			{
				AllocationsProvider->EnumerateTags(
					[&](const TCHAR* Name, const TCHAR* FullPath,
						const TraceServices::TagIdType TagId,
						const TraceServices::TagIdType ParentId)
					{
						const FString SafeName = Name ? Name : TEXT("<unnamed>");
						const FString SafePath = FullPath ? FullPath : SafeName;
						if (MatchesFilter(SafeName + TEXT(" ") + SafePath, Request.Filter))
						{
							Tags.Add({TEXT("allocations"), TagId, ParentId,
								SafeName, SafePath, 0});
						}
					});
			}
		}
		if (!MemoryProvider && !AllocationsProvider)
		{
			ErrorCode = TEXT("trace_provider_unavailable");
			ErrorMessage = TEXT("The trace does not contain Memory or Allocations providers.");
			return false;
		}
		Tags.Sort([](const FTagRow& Left, const FTagRow& Right)
		{
			const int32 SourceOrder = Left.Source.Compare(Right.Source);
			if (SourceOrder != 0)
			{
				return SourceOrder < 0;
			}
			const int32 NameOrder = Left.FullPath.Compare(Right.FullPath, ESearchCase::IgnoreCase);
			return NameOrder != 0 ? NameOrder < 0 : Left.Id < Right.Id;
		});
		Result.Columns = {TEXT("source"), TEXT("tagId"), TEXT("parentTagId"),
			TEXT("name"), TEXT("fullPath"), TEXT("trackerMask")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Tags.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FTagRow& Tag = Tags[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("source"), FTraceValue::String(Tag.Source))
				.Add(TEXT("tagId"), FTraceValue::Integer(Tag.Id))
				.Add(TEXT("parentTagId"), FTraceValue::Integer(Tag.ParentId))
				.Add(TEXT("name"), FTraceValue::String(Tag.Name))
				.Add(TEXT("fullPath"), FTraceValue::String(Tag.FullPath))
				.Add(TEXT("trackerMask"), FTraceValue::String(
					FString::Printf(TEXT("0x%016llx"), Tag.Trackers)));
		}
		FinishPage(Tags.Num(), Request.Page, Limit, Result);
		return true;
	}

	if (Request.Operation == TEXT("modules"))
	{
		const TraceServices::IModuleProvider* Provider =
			Session.ReadProvider<TraceServices::IModuleProvider>(
				TraceServices::GetModuleProviderName());
		if (!Provider || Provider->GetNumModules() == 0)
		{
			ErrorCode = TEXT("trace_provider_unavailable");
			ErrorMessage = TEXT("The trace does not contain module records.");
			return false;
		}
		struct FModuleRow
		{
			FString Name;
			FString FullName;
			uint64 Base = 0;
			uint32 Size = 0;
			TraceServices::EModuleStatus Status = TraceServices::EModuleStatus::Discovered;
			FString StatusMessage;
		};
		TArray<FModuleRow> Modules;
		Provider->EnumerateModules(0, [&](const TraceServices::FModule& Module)
		{
			const FString Name = Module.Name ? Module.Name : TEXT("<unnamed>");
			const FString FullName = Module.FullName ? Module.FullName : Name;
			if (MatchesFilter(Name + TEXT(" ") + FullName, Request.Filter))
			{
				Modules.Add({Name, FullName, Module.Base, Module.Size,
					Module.Status.load(std::memory_order_acquire),
					Module.StatusMessage ? Module.StatusMessage : TEXT("")});
			}
		});
		Modules.Sort([](const FModuleRow& Left, const FModuleRow& Right)
		{
			const int32 NameOrder = Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase);
			return NameOrder != 0 ? NameOrder < 0 : Left.Base < Right.Base;
		});
		Result.Columns = {TEXT("name"), TEXT("fullName"), TEXT("base"), TEXT("sizeBytes"),
			TEXT("status"), TEXT("statusMessage")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Modules.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FModuleRow& Module = Modules[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("name"), FTraceValue::String(Module.Name))
				.Add(TEXT("fullName"), FTraceValue::String(Module.FullName))
				.Add(TEXT("base"), FTraceValue::String(
					FString::Printf(TEXT("0x%016llx"), Module.Base)))
				.Add(TEXT("sizeBytes"), FTraceValue::Integer(Module.Size))
				.Add(TEXT("status"), FTraceValue::String(
					TraceServices::ModuleStatusToString(Module.Status)))
				.Add(TEXT("statusMessage"), FTraceValue::String(Module.StatusMessage));
		}
		FinishPage(Modules.Num(), Request.Page, Limit, Result);
		return true;
	}

	if (Request.Operation != TEXT("allocations")
		&& Request.Operation != TEXT("liveAllocations")
		&& Request.Operation != TEXT("callstacks"))
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Memory queries support allocations, liveAllocations, tags, modules, and callstacks.");
		return false;
	}
	const TraceServices::IAllocationsProvider* Provider =
		Session.ReadProvider<TraceServices::IAllocationsProvider>(
			TraceServices::GetAllocationsProviderName());
	bool bAllocationsInitialized = false;
	if (Provider)
	{
		TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);
		bAllocationsInitialized = Provider->IsInitialized();
	}
	if (!bAllocationsInitialized)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain initialized allocation data.");
		return false;
	}
	TArray<FBoundedAllocationRow> Allocations;
	bool bCollectionBounded = false;
	const TraceServices::IAllocationsProvider::EQueryRule Rule =
		Request.Operation == TEXT("liveAllocations")
			? TraceServices::IAllocationsProvider::EQueryRule::aAf
			: TraceServices::IAllocationsProvider::EQueryRule::AaB;
	const FString AllocationFilter = Request.Operation == TEXT("callstacks")
		? FString() : Request.Filter;
	if (!CollectAllocations(
		*Provider, Rule, Range, AllocationFilter, Allocations,
		bCollectionBounded, ErrorCode, ErrorMessage))
	{
		return false;
	}

	if (Request.Operation == TEXT("callstacks"))
	{
		const TraceServices::ICallstacksProvider* Callstacks =
			Session.ReadProvider<TraceServices::ICallstacksProvider>(
				TraceServices::GetCallstacksProviderName());
		if (!Callstacks)
		{
			ErrorCode = TEXT("trace_provider_unavailable");
			ErrorMessage = TEXT("The trace does not contain callstack records.");
			return false;
		}
		TSet<uint32> IdSet;
		for (const FBoundedAllocationRow& Allocation : Allocations)
		{
			if (Allocation.CallstackId != 0)
			{
				IdSet.Add(Allocation.CallstackId);
			}
		}
		TArray<uint32> Ids = IdSet.Array();
		Ids.Sort();
		Result.Columns = {TEXT("callstackId"), TEXT("depth"), TEXT("address"),
			TEXT("module"), TEXT("symbol"), TEXT("file"), TEXT("line"), TEXT("resolveStatus")};
		TArray<FTraceRow> FrameRows;
		for (const uint32 Id : Ids)
		{
			const TraceServices::FCallstack* Callstack = Callstacks->GetCallstack(Id);
			if (!Callstack)
			{
				continue;
			}
			for (uint32 Depth = 0; Depth < Callstack->Num(); ++Depth)
			{
				const TraceServices::FStackFrame* Frame = Callstack->Frame(Depth);
				if (!Frame)
				{
					continue;
				}
				const TraceServices::FResolvedSymbol* Symbol = Frame->Symbol;
				const FString SymbolName = Symbol && Symbol->Name ? Symbol->Name : TEXT("<unresolved>");
				if (!Request.Filter.IsEmpty() && !MatchesFilter(SymbolName, Request.Filter))
				{
					continue;
				}
				FTraceRow& Row = FrameRows.AddDefaulted_GetRef();
				Row.Add(TEXT("callstackId"), FTraceValue::Integer(Id))
					.Add(TEXT("depth"), FTraceValue::Integer(Depth))
					.Add(TEXT("address"), FTraceValue::String(
						FString::Printf(TEXT("0x%016llx"), Frame->Addr)))
					.Add(TEXT("module"), FTraceValue::String(
						Symbol && Symbol->Module ? Symbol->Module : TEXT("")))
					.Add(TEXT("symbol"), FTraceValue::String(SymbolName))
					.Add(TEXT("file"), FTraceValue::String(
						Symbol && Symbol->File ? Symbol->File : TEXT("")))
					.Add(TEXT("line"), FTraceValue::Integer(Symbol ? Symbol->Line : 0))
					.Add(TEXT("resolveStatus"), FTraceValue::String(
						Symbol ? TraceServices::QueryResultToString(Symbol->GetResult()) : TEXT("Unknown")));
				if (FrameRows.Num() >= MaximumAggregationRows)
				{
					bCollectionBounded = true;
					break;
				}
			}
			if (bCollectionBounded)
			{
				break;
			}
		}
		const uint64 TotalRows = FrameRows.Num();
		for (uint64 Index = Request.Page.Cursor;
			Index < TotalRows && Result.Rows.Num() < Limit;
			++Index)
		{
			Result.Rows.Add(MoveTemp(FrameRows[static_cast<int32>(Index)]));
		}
		FinishPage(TotalRows, Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	Result.Columns = {TEXT("startSeconds"), TEXT("endSeconds"), TEXT("openEnded"),
		TEXT("address"), TEXT("sizeBytes"), TEXT("alignment"), TEXT("threadId"),
		TEXT("callstackId"), TEXT("freeCallstackId"), TEXT("tagId"), TEXT("tag")};
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Allocations.Num()) && Result.Rows.Num() < Limit;
		++Index)
	{
		const FBoundedAllocationRow& Allocation = Allocations[static_cast<int32>(Index)];
		const bool bOpenEnded = !FMath::IsFinite(Allocation.EndTime);
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("startSeconds"), FTraceValue::Number(Allocation.StartTime))
			.Add(TEXT("endSeconds"), FTraceValue::Number(
				bOpenEnded ? Range.End : Allocation.EndTime))
			.Add(TEXT("openEnded"), FTraceValue::Boolean(bOpenEnded))
			.Add(TEXT("address"), FTraceValue::String(
				FString::Printf(TEXT("0x%016llx"), Allocation.Address)))
			.Add(TEXT("sizeBytes"), FTraceValue::String(
				LexToString(Allocation.Size)))
			.Add(TEXT("alignment"), FTraceValue::Integer(Allocation.Alignment))
			.Add(TEXT("threadId"), FTraceValue::Integer(Allocation.ThreadId))
			.Add(TEXT("callstackId"), FTraceValue::Integer(Allocation.CallstackId))
			.Add(TEXT("freeCallstackId"), FTraceValue::Integer(Allocation.FreeCallstackId))
			.Add(TEXT("tagId"), FTraceValue::Integer(Allocation.TagId))
			.Add(TEXT("tag"), FTraceValue::String(Allocation.Tag));
	}
	FinishPage(Allocations.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

template <typename RowType>
TArray<RowType> ReadBoundedTable(
	TUniquePtr<TraceServices::ITable<RowType>> Table,
	bool& bOutBounded)
{
	bOutBounded = false;
	TArray<RowType> Rows;
	if (!Table.IsValid())
	{
		return Rows;
	}
	TUniquePtr<TraceServices::ITableReader<RowType>> Reader(Table->CreateReader());
	while (Reader.IsValid() && Reader->IsValid())
	{
		const RowType* Row = Reader->GetCurrentRow();
		if (Row)
		{
			if (Rows.Num() >= MaximumAggregationRows)
			{
				bOutBounded = true;
				break;
			}
			Rows.Add(*Row);
		}
		Reader->NextRow();
	}
	return Rows;
}

bool QueryLoading(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::ILoadTimeProfilerProvider* Provider =
		Session.ReadProvider<TraceServices::ILoadTimeProfilerProvider>(
			TraceServices::GetLoadTimeProfilerProviderName());
	if (!Provider || Provider->GetTimelineCount() == 0)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain Asset Loading events.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	bool bCollectionBounded = false;

	if (Request.Operation == TEXT("packages") || Request.Operation == TEXT("dependencies"))
	{
		TArray<TraceServices::FPackagesTableRow> Packages = ReadBoundedTable(
			TUniquePtr<TraceServices::ITable<TraceServices::FPackagesTableRow>>(
				Provider->CreatePackageDetailsTable(Range.Start, Range.End)),
			bCollectionBounded);
		if (Request.Operation == TEXT("packages"))
		{
			Packages.RemoveAll([&](const TraceServices::FPackagesTableRow& Row)
			{
				const FString Name = Row.PackageInfo && Row.PackageInfo->Name
					? Row.PackageInfo->Name : TEXT("<unknown>");
				return !MatchesFilter(Name, Request.Filter);
			});
			Packages.Sort([](const TraceServices::FPackagesTableRow& Left,
				const TraceServices::FPackagesTableRow& Right)
			{
				const double LeftTime = Left.MainThreadTime + Left.AsyncLoadingThreadTime;
				const double RightTime = Right.MainThreadTime + Right.AsyncLoadingThreadTime;
				if (LeftTime != RightTime)
				{
					return LeftTime > RightTime;
				}
				const FString LeftName = Left.PackageInfo && Left.PackageInfo->Name
					? Left.PackageInfo->Name : TEXT("");
				const FString RightName = Right.PackageInfo && Right.PackageInfo->Name
					? Right.PackageInfo->Name : TEXT("");
				return LeftName < RightName;
			});
			Result.Columns = {TEXT("packageId"), TEXT("name"), TEXT("serializedSizeBytes"),
				TEXT("headerSizeBytes"), TEXT("exportCount"), TEXT("mainThreadMs"),
				TEXT("asyncLoadingThreadMs"), TEXT("totalLoadMs")};
			for (uint64 Index = Request.Page.Cursor;
				Index < static_cast<uint64>(Packages.Num()) && Result.Rows.Num() < Limit;
				++Index)
			{
				const TraceServices::FPackagesTableRow& Package = Packages[static_cast<int32>(Index)];
				const TraceServices::FPackageInfo* Info = Package.PackageInfo;
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("packageId"), FTraceValue::Integer(Info ? Info->Id : -1))
					.Add(TEXT("name"), FTraceValue::String(
						Info && Info->Name ? Info->Name : TEXT("<unknown>")))
					.Add(TEXT("serializedSizeBytes"), FTraceValue::String(
						LexToString(Package.TotalSerializedSize)))
					.Add(TEXT("headerSizeBytes"), FTraceValue::String(
						LexToString(Package.SerializedHeaderSize)))
					.Add(TEXT("exportCount"), FTraceValue::String(
						LexToString(Package.SerializedExportsCount)))
					.Add(TEXT("mainThreadMs"), FTraceValue::Number(Package.MainThreadTime * 1000.0))
					.Add(TEXT("asyncLoadingThreadMs"), FTraceValue::Number(
						Package.AsyncLoadingThreadTime * 1000.0))
					.Add(TEXT("totalLoadMs"), FTraceValue::Number(
						(Package.MainThreadTime + Package.AsyncLoadingThreadTime) * 1000.0));
			}
			FinishPage(Packages.Num(), Request.Page, Limit, Result);
		}
		else
		{
			struct FDependencyRow
			{
				uint32 PackageId = 0;
				FString Package;
				uint32 ImportedPackageId = 0;
				FString ImportedPackage;
			};
			TArray<FDependencyRow> Dependencies;
			bool bDependenciesBounded = false;
			for (const TraceServices::FPackagesTableRow& Package : Packages)
			{
				if (!Package.PackageInfo)
				{
					continue;
				}
				const FString PackageName = Package.PackageInfo->Name
					? Package.PackageInfo->Name : TEXT("<unknown>");
				for (const TraceServices::FPackageInfo* Imported : Package.PackageInfo->ImportedPackages)
				{
					if (!Imported)
					{
						continue;
					}
					const FString ImportedName = Imported->Name ? Imported->Name : TEXT("<unknown>");
					if (MatchesFilter(PackageName + TEXT(" ") + ImportedName, Request.Filter))
					{
						if (Dependencies.Num() >= MaximumAggregationRows)
						{
							bDependenciesBounded = true;
							break;
						}
						Dependencies.Add({Package.PackageInfo->Id, PackageName,
							Imported->Id, ImportedName});
					}
				}
				if (bDependenciesBounded)
				{
					break;
				}
			}
			bCollectionBounded = bCollectionBounded || bDependenciesBounded;
			Dependencies.Sort([](const FDependencyRow& Left, const FDependencyRow& Right)
			{
				const int32 PackageOrder = Left.Package.Compare(Right.Package);
				return PackageOrder != 0 ? PackageOrder < 0
					: Left.ImportedPackage.Compare(Right.ImportedPackage) < 0;
			});
			Result.Columns = {TEXT("packageId"), TEXT("package"),
				TEXT("importedPackageId"), TEXT("importedPackage")};
			for (uint64 Index = Request.Page.Cursor;
				Index < static_cast<uint64>(Dependencies.Num()) && Result.Rows.Num() < Limit;
				++Index)
			{
				const FDependencyRow& Dependency = Dependencies[static_cast<int32>(Index)];
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("packageId"), FTraceValue::Integer(Dependency.PackageId))
					.Add(TEXT("package"), FTraceValue::String(Dependency.Package))
					.Add(TEXT("importedPackageId"), FTraceValue::Integer(Dependency.ImportedPackageId))
					.Add(TEXT("importedPackage"), FTraceValue::String(Dependency.ImportedPackage));
			}
			FinishPage(Dependencies.Num(), Request.Page, Limit, Result);
		}
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	if (Request.Operation == TEXT("objects") || Request.Operation == TEXT("exports"))
	{
		TArray<TraceServices::FExportsTableRow> Exports = ReadBoundedTable(
			TUniquePtr<TraceServices::ITable<TraceServices::FExportsTableRow>>(
				Provider->CreateExportDetailsTable(Range.Start, Range.End)),
			bCollectionBounded);
		Exports.RemoveAll([&](const TraceServices::FExportsTableRow& Row)
		{
			const TraceServices::FPackageExportInfo* Info = Row.ExportInfo;
			const FString ClassName = Info && Info->Class && Info->Class->Name
				? Info->Class->Name : TEXT("<unknown>");
			const FString PackageName = Info && Info->Package && Info->Package->Name
				? Info->Package->Name : TEXT("<unknown>");
			return !MatchesFilter(ClassName + TEXT(" ") + PackageName, Request.Filter);
		});
		Exports.Sort([](const TraceServices::FExportsTableRow& Left,
			const TraceServices::FExportsTableRow& Right)
		{
			const double LeftTime = Left.MainThreadTime + Left.AsyncLoadingThreadTime;
			const double RightTime = Right.MainThreadTime + Right.AsyncLoadingThreadTime;
			if (LeftTime != RightTime)
			{
				return LeftTime > RightTime;
			}
			const uint32 LeftId = Left.ExportInfo ? Left.ExportInfo->Id : 0;
			const uint32 RightId = Right.ExportInfo ? Right.ExportInfo->Id : 0;
			return LeftId < RightId;
		});
		Result.Columns = {TEXT("exportId"), TEXT("package"), TEXT("class"),
			TEXT("serializedSizeBytes"), TEXT("mainThreadMs"),
			TEXT("asyncLoadingThreadMs"), TEXT("eventType")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Exports.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const TraceServices::FExportsTableRow& Export = Exports[static_cast<int32>(Index)];
			const TraceServices::FPackageExportInfo* Info = Export.ExportInfo;
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("exportId"), FTraceValue::Integer(Info ? Info->Id : -1))
				.Add(TEXT("package"), FTraceValue::String(
					Info && Info->Package && Info->Package->Name
						? Info->Package->Name : TEXT("<unknown>")))
				.Add(TEXT("class"), FTraceValue::String(
					Info && Info->Class && Info->Class->Name
						? Info->Class->Name : TEXT("<unknown>")))
				.Add(TEXT("serializedSizeBytes"), FTraceValue::String(
					LexToString(Export.SerializedSize)))
				.Add(TEXT("mainThreadMs"), FTraceValue::Number(Export.MainThreadTime * 1000.0))
				.Add(TEXT("asyncLoadingThreadMs"), FTraceValue::Number(
					Export.AsyncLoadingThreadTime * 1000.0))
				.Add(TEXT("eventType"), FTraceValue::String(
					TraceServices::GetLoadTimeProfilerObjectEventTypeString(Export.EventType)));
		}
		FinishPage(Exports.Num(), Request.Page, Limit, Result);
		if (Request.Operation == TEXT("objects"))
		{
			FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Severity = TEXT("info");
			Diagnostic.Code = TEXT("trace_loading_object_model");
			Diagnostic.Message = TEXT(
				"UE 5.3 records loadable object activity through package export records; object rows therefore use stable export ids, package names, and classes.");
		}
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	if (Request.Operation == TEXT("requests"))
	{
		TArray<TraceServices::FRequestsTableRow> Requests = ReadBoundedTable(
			TUniquePtr<TraceServices::ITable<TraceServices::FRequestsTableRow>>(
				Provider->CreateRequestsTable(Range.Start, Range.End)),
			bCollectionBounded);
		Requests.RemoveAll([&](const TraceServices::FRequestsTableRow& Row)
		{
			return !MatchesFilter(Row.Name ? Row.Name : TEXT("<unnamed>"), Request.Filter);
		});
		Requests.Sort([](const TraceServices::FRequestsTableRow& Left,
			const TraceServices::FRequestsTableRow& Right)
		{
			if (Left.StartTime != Right.StartTime)
			{
				return Left.StartTime < Right.StartTime;
			}
			return Left.Id < Right.Id;
		});
		Result.Columns = {TEXT("requestId"), TEXT("name"), TEXT("startSeconds"),
			TEXT("durationMs"), TEXT("packageCount")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Requests.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const TraceServices::FRequestsTableRow& LoadRequest = Requests[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("requestId"), FTraceValue::String(LexToString(LoadRequest.Id)))
				.Add(TEXT("name"), FTraceValue::String(
					LoadRequest.Name ? LoadRequest.Name : TEXT("<unnamed>")))
				.Add(TEXT("startSeconds"), FTraceValue::Number(LoadRequest.StartTime))
				.Add(TEXT("durationMs"), FTraceValue::Number(LoadRequest.Duration * 1000.0))
				.Add(TEXT("packageCount"), FTraceValue::Integer(LoadRequest.Packages.Num()));
		}
		FinishPage(Requests.Num(), Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	ErrorCode = TEXT("trace_query_unsupported");
	ErrorMessage = TEXT("Loading queries support packages, objects, exports, requests, and dependencies.");
	return false;
}

struct FNetworkConnectionRow
{
	uint32 GameInstanceIndex = 0;
	uint32 GameInstanceId = 0;
	FString GameInstanceName;
	bool bServer = false;
	bool bIris = false;
	uint32 ConnectionIndex = 0;
	uint32 ConnectionId = 0;
	FString Name;
	FString Address;
	double Begin = 0.0;
	double End = 0.0;
	bool bIncoming = false;
	bool bOutgoing = false;
};

TArray<FNetworkConnectionRow> CollectNetworkConnections(
	const TraceServices::INetProfilerProvider& Provider,
	const FResolvedRange& Range,
	const FTraceQueryRequest& Request)
{
	uint32 RequestedConnectionId = 0;
	const bool bHasConnectionId = OptionUInt32(Request, TEXT("connectionId"), RequestedConnectionId);
	TArray<FNetworkConnectionRow> Connections;
	Provider.ReadGameInstances([&](const TraceServices::FNetProfilerGameInstance& Instance)
	{
		Provider.ReadConnections(
			Instance.GameInstanceIndex,
			[&](const TraceServices::FNetProfilerConnection& Connection)
			{
				const double End = FMath::IsFinite(Connection.LifeTime.End)
					? Connection.LifeTime.End : Range.End;
				const FString Name = Connection.Name ? Connection.Name : TEXT("<unnamed>");
				const FString Address = Connection.AddressString
					? Connection.AddressString : TEXT("");
				if ((bHasConnectionId && Connection.ConnectionId != RequestedConnectionId)
					|| End < Range.Start || Connection.LifeTime.Begin > Range.End
					|| !MatchesFilter(Name + TEXT(" ") + Address, Request.Filter))
				{
					return;
				}
				FNetworkConnectionRow& Row = Connections.AddDefaulted_GetRef();
				Row.GameInstanceIndex = Instance.GameInstanceIndex;
				Row.GameInstanceId = Instance.GameInstanceId;
				Row.GameInstanceName = Instance.InstanceName
					? Instance.InstanceName : TEXT("<unnamed>");
				Row.bServer = Instance.bIsServer;
				Row.bIris = Instance.bIsUsingIrisReplication;
				Row.ConnectionIndex = Connection.ConnectionIndex;
				Row.ConnectionId = Connection.ConnectionId;
				Row.Name = Name;
				Row.Address = Address;
				Row.Begin = Connection.LifeTime.Begin;
				Row.End = End;
				Row.bIncoming = Connection.bHasIncomingData != 0;
				Row.bOutgoing = Connection.bHasOutgoingData != 0;
			});
	});
	Connections.Sort([](const FNetworkConnectionRow& Left, const FNetworkConnectionRow& Right)
	{
		if (Left.GameInstanceIndex != Right.GameInstanceIndex)
		{
			return Left.GameInstanceIndex < Right.GameInstanceIndex;
		}
		return Left.ConnectionIndex < Right.ConnectionIndex;
	});
	return Connections;
}

bool ReadPacketAt(
	const TraceServices::INetProfilerProvider& Provider,
	const uint32 ConnectionIndex,
	const TraceServices::ENetProfilerConnectionMode Mode,
	const uint32 PacketIndex,
	TraceServices::FNetProfilerPacket& OutPacket)
{
	bool bFound = false;
	Provider.EnumeratePackets(
		ConnectionIndex, Mode, PacketIndex, PacketIndex,
		[&](const TraceServices::FNetProfilerPacket& Packet)
		{
			OutPacket = Packet;
			bFound = true;
		});
	return bFound;
}

uint32 LowerBoundPacketTime(
	const TraceServices::INetProfilerProvider& Provider,
	const uint32 ConnectionIndex,
	const TraceServices::ENetProfilerConnectionMode Mode,
	const uint32 Count,
	const double Time,
	const bool bStrictlyGreater)
{
	uint32 Low = 0;
	uint32 High = Count;
	while (Low < High)
	{
		const uint32 Mid = Low + (High - Low) / 2;
		TraceServices::FNetProfilerPacket Packet;
		if (!ReadPacketAt(Provider, ConnectionIndex, Mode, Mid, Packet))
		{
			return Count;
		}
		const bool bBefore = bStrictlyGreater
			? Packet.TimeStamp <= Time
			: Packet.TimeStamp < Time;
		if (bBefore)
		{
			Low = Mid + 1;
		}
		else
		{
			High = Mid;
		}
	}
	return Low;
}

bool DirectionAllowed(
	const FString& Direction,
	const TraceServices::ENetProfilerConnectionMode Mode)
{
	return Direction.IsEmpty()
		|| Direction.Equals(TEXT("both"), ESearchCase::IgnoreCase)
		|| (Mode == TraceServices::ENetProfilerConnectionMode::Incoming
			&& Direction.Equals(TEXT("incoming"), ESearchCase::IgnoreCase))
		|| (Mode == TraceServices::ENetProfilerConnectionMode::Outgoing
			&& Direction.Equals(TEXT("outgoing"), ESearchCase::IgnoreCase));
}

const TCHAR* NetworkDirectionName(const TraceServices::ENetProfilerConnectionMode Mode)
{
	return Mode == TraceServices::ENetProfilerConnectionMode::Incoming
		? TEXT("incoming") : TEXT("outgoing");
}

bool QueryNetwork(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::INetProfilerProvider* Provider =
		Session.ReadProvider<TraceServices::INetProfilerProvider>(
			TraceServices::GetNetProfilerProviderName());
	if (!Provider || Provider->GetNetTraceVersion() == 0)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain Network Insights data.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	const FString Direction = Option(Request, TEXT("direction"));
	const TArray<FNetworkConnectionRow> Connections =
		CollectNetworkConnections(*Provider, Range, Request);

	if (Request.Operation == TEXT("connections"))
	{
		TArray<const FNetworkConnectionRow*> MatchingConnections;
		for (const FNetworkConnectionRow& Connection : Connections)
		{
			const FString SearchText = FString::Printf(
				TEXT("%s %s %s %u %u"),
				*Connection.GameInstanceName,
				*Connection.Name,
				*Connection.Address,
				Connection.ConnectionId,
				Connection.ConnectionIndex);
			if (MatchesFilter(SearchText, Request.Filter))
			{
				MatchingConnections.Add(&Connection);
			}
		}
		Result.Columns = {TEXT("gameInstanceId"), TEXT("gameInstance"), TEXT("server"),
			TEXT("iris"), TEXT("connectionId"), TEXT("connectionIndex"), TEXT("name"),
			TEXT("address"), TEXT("beginSeconds"), TEXT("endSeconds"),
			TEXT("hasIncoming"), TEXT("hasOutgoing")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(MatchingConnections.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FNetworkConnectionRow& Connection =
				*MatchingConnections[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("gameInstanceId"), FTraceValue::Integer(Connection.GameInstanceId))
				.Add(TEXT("gameInstance"), FTraceValue::String(Connection.GameInstanceName))
				.Add(TEXT("server"), FTraceValue::Boolean(Connection.bServer))
				.Add(TEXT("iris"), FTraceValue::Boolean(Connection.bIris))
				.Add(TEXT("connectionId"), FTraceValue::Integer(Connection.ConnectionId))
				.Add(TEXT("connectionIndex"), FTraceValue::Integer(Connection.ConnectionIndex))
				.Add(TEXT("name"), FTraceValue::String(Connection.Name))
				.Add(TEXT("address"), FTraceValue::String(Connection.Address))
				.Add(TEXT("beginSeconds"), FTraceValue::Number(Connection.Begin))
				.Add(TEXT("endSeconds"), FTraceValue::Number(Connection.End))
				.Add(TEXT("hasIncoming"), FTraceValue::Boolean(Connection.bIncoming))
				.Add(TEXT("hasOutgoing"), FTraceValue::Boolean(Connection.bOutgoing));
		}
		FinishPage(MatchingConnections.Num(), Request.Page, Limit, Result);
		return true;
	}

	struct FPacketRow
	{
		FNetworkConnectionRow Connection;
		TraceServices::ENetProfilerConnectionMode Mode =
			TraceServices::ENetProfilerConnectionMode::Outgoing;
		uint32 PacketIndex = 0;
		TraceServices::FNetProfilerPacket Packet;
	};
	TArray<FPacketRow> Packets;
	bool bCollectionBounded = false;
	for (const FNetworkConnectionRow& Connection : Connections)
	{
		for (const TraceServices::ENetProfilerConnectionMode Mode : {
			TraceServices::ENetProfilerConnectionMode::Outgoing,
			TraceServices::ENetProfilerConnectionMode::Incoming})
		{
			if (!DirectionAllowed(Direction, Mode)
				|| (Mode == TraceServices::ENetProfilerConnectionMode::Incoming
					&& !Connection.bIncoming)
				|| (Mode == TraceServices::ENetProfilerConnectionMode::Outgoing
					&& !Connection.bOutgoing))
			{
				continue;
			}
			const uint32 PacketCount = Provider->GetPacketCount(Connection.ConnectionIndex, Mode);
			if (PacketCount == 0)
			{
				continue;
			}
			const uint32 First = LowerBoundPacketTime(
				*Provider, Connection.ConnectionIndex, Mode, PacketCount, Range.Start, false);
			const uint32 AfterLast = LowerBoundPacketTime(
				*Provider, Connection.ConnectionIndex, Mode, PacketCount, Range.End, true);
			if (First >= AfterLast || First >= PacketCount)
			{
				continue;
			}
			uint32 PacketIndex = First;
			Provider->EnumeratePackets(
				Connection.ConnectionIndex, Mode, First, FMath::Min(AfterLast, PacketCount) - 1,
				[&](const TraceServices::FNetProfilerPacket& Packet)
				{
					if (Packets.Num() < MaximumAggregationRows)
					{
						Packets.Add({Connection, Mode, PacketIndex, Packet});
					}
					else
					{
						bCollectionBounded = true;
					}
					++PacketIndex;
				});
		}
	}
	Packets.Sort([](const FPacketRow& Left, const FPacketRow& Right)
	{
		if (Left.Packet.TimeStamp != Right.Packet.TimeStamp)
		{
			return Left.Packet.TimeStamp < Right.Packet.TimeStamp;
		}
		if (Left.Connection.ConnectionIndex != Right.Connection.ConnectionIndex)
		{
			return Left.Connection.ConnectionIndex < Right.Connection.ConnectionIndex;
		}
		if (Left.Mode != Right.Mode)
		{
			return static_cast<uint8>(Left.Mode) < static_cast<uint8>(Right.Mode);
		}
		return Left.PacketIndex < Right.PacketIndex;
	});

	if (Request.Operation == TEXT("packets"))
	{
		Packets.RemoveAll([&Request](const FPacketRow& Packet)
		{
			const TCHAR* Delivery = Packet.Packet.DeliveryStatus
				== TraceServices::ENetProfilerDeliveryStatus::Delivered ? TEXT("delivered")
				: Packet.Packet.DeliveryStatus
					== TraceServices::ENetProfilerDeliveryStatus::Dropped ? TEXT("dropped")
					: TEXT("unknown");
			return !MatchesFilter(
				FString::Printf(
					TEXT("%s %s %s %s %s %u %u %u"),
					*Packet.Connection.GameInstanceName,
					*Packet.Connection.Name,
					*Packet.Connection.Address,
					NetworkDirectionName(Packet.Mode),
					Delivery,
					Packet.Connection.ConnectionId,
					Packet.PacketIndex,
					Packet.Packet.SequenceNumber),
				Request.Filter);
		});
		Result.Columns = {TEXT("timeSeconds"), TEXT("connectionId"), TEXT("direction"),
			TEXT("packetIndex"), TEXT("sequence"), TEXT("sizeBytes"), TEXT("contentBits"),
			TEXT("eventCount"), TEXT("delivery"), TEXT("connectionState")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Packets.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FPacketRow& Packet = Packets[static_cast<int32>(Index)];
			const TCHAR* Delivery = Packet.Packet.DeliveryStatus
				== TraceServices::ENetProfilerDeliveryStatus::Delivered ? TEXT("delivered")
				: Packet.Packet.DeliveryStatus
					== TraceServices::ENetProfilerDeliveryStatus::Dropped ? TEXT("dropped")
					: TEXT("unknown");
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("timeSeconds"), FTraceValue::Number(Packet.Packet.TimeStamp))
				.Add(TEXT("connectionId"), FTraceValue::Integer(Packet.Connection.ConnectionId))
				.Add(TEXT("direction"), FTraceValue::String(NetworkDirectionName(Packet.Mode)))
				.Add(TEXT("packetIndex"), FTraceValue::Integer(Packet.PacketIndex))
				.Add(TEXT("sequence"), FTraceValue::Integer(Packet.Packet.SequenceNumber))
				.Add(TEXT("sizeBytes"), FTraceValue::Integer(Packet.Packet.TotalPacketSizeInBytes))
				.Add(TEXT("contentBits"), FTraceValue::Integer(Packet.Packet.ContentSizeInBits))
				.Add(TEXT("eventCount"), FTraceValue::Integer(Packet.Packet.EventCount))
				.Add(TEXT("delivery"), FTraceValue::String(Delivery))
				.Add(TEXT("connectionState"), FTraceValue::String(
					TraceServices::LexToString(Packet.Packet.ConnectionState)));
		}
		FinishPage(Packets.Num(), Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	TMap<uint32, FString> EventTypeNames;
	Provider->ReadEventTypes(
		[&](const TraceServices::FNetProfilerEventType* EventTypes, const uint64 Count)
		{
			for (uint64 Index = 0; EventTypes && Index < Count; ++Index)
			{
				EventTypeNames.Add(
					EventTypes[Index].EventTypeIndex,
					EventTypes[Index].Name ? EventTypes[Index].Name : TEXT("<unnamed>"));
			}
		});
	if (Request.Operation == TEXT("contentEvents"))
	{
		struct FContentRow
		{
			double Time = 0.0;
			uint32 ConnectionId = 0;
			TraceServices::ENetProfilerConnectionMode Mode =
				TraceServices::ENetProfilerConnectionMode::Outgoing;
			uint32 PacketIndex = 0;
			uint32 Sequence = 0;
			FString Name;
			uint32 Level = 0;
			uint32 StartBit = 0;
			uint32 EndBit = 0;
			uint32 ObjectIndex = 0;
		};
		TArray<FContentRow> ContentRows;
		for (const FPacketRow& Packet : Packets)
		{
			if (Packet.Packet.EventCount == 0)
			{
				continue;
			}
			Provider->EnumeratePacketContentEventsByIndex(
				Packet.Connection.ConnectionIndex,
				Packet.Mode,
				Packet.Packet.StartEventIndex,
				Packet.Packet.StartEventIndex + Packet.Packet.EventCount - 1,
				[&](const TraceServices::FNetProfilerContentEvent& Event)
				{
					if (ContentRows.Num() >= MaximumAggregationRows)
					{
						bCollectionBounded = true;
						return;
					}
					const FString EventName =
						EventTypeNames.FindRef(Event.EventTypeIndex);
					if (!MatchesFilter(
						FString::Printf(
							TEXT("%s %s %u %u"),
							*EventName,
							NetworkDirectionName(Packet.Mode),
							Packet.Connection.ConnectionId,
							Packet.PacketIndex),
						Request.Filter))
					{
						return;
					}
					ContentRows.Add({Packet.Packet.TimeStamp,
						Packet.Connection.ConnectionId, Packet.Mode, Packet.PacketIndex,
						Packet.Packet.SequenceNumber,
						EventName,
						static_cast<uint32>(Event.Level), static_cast<uint32>(Event.StartPos),
						static_cast<uint32>(Event.EndPos), Event.ObjectInstanceIndex});
				});
		}
		ContentRows.Sort([](const FContentRow& Left, const FContentRow& Right)
		{
			if (Left.Time != Right.Time)
			{
				return Left.Time < Right.Time;
			}
			if (Left.PacketIndex != Right.PacketIndex)
			{
				return Left.PacketIndex < Right.PacketIndex;
			}
			return Left.StartBit < Right.StartBit;
		});
		Result.Columns = {TEXT("timeSeconds"), TEXT("connectionId"), TEXT("direction"),
			TEXT("packetIndex"), TEXT("sequence"), TEXT("name"), TEXT("level"),
			TEXT("startBit"), TEXT("endBit"), TEXT("bitSize"), TEXT("objectIndex")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(ContentRows.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FContentRow& Content = ContentRows[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("timeSeconds"), FTraceValue::Number(Content.Time))
				.Add(TEXT("connectionId"), FTraceValue::Integer(Content.ConnectionId))
				.Add(TEXT("direction"), FTraceValue::String(NetworkDirectionName(Content.Mode)))
				.Add(TEXT("packetIndex"), FTraceValue::Integer(Content.PacketIndex))
				.Add(TEXT("sequence"), FTraceValue::Integer(Content.Sequence))
				.Add(TEXT("name"), FTraceValue::String(
					Content.Name.IsEmpty() ? TEXT("<unknown>") : Content.Name))
				.Add(TEXT("level"), FTraceValue::Integer(Content.Level))
				.Add(TEXT("startBit"), FTraceValue::Integer(Content.StartBit))
				.Add(TEXT("endBit"), FTraceValue::Integer(Content.EndBit))
				.Add(TEXT("bitSize"), FTraceValue::Integer(Content.EndBit - Content.StartBit))
				.Add(TEXT("objectIndex"), FTraceValue::Integer(Content.ObjectIndex));
		}
		FinishPage(ContentRows.Num(), Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	if (Request.Operation == TEXT("stats"))
	{
		struct FStatsRow
		{
			uint32 ConnectionId = 0;
			TraceServices::ENetProfilerConnectionMode Mode =
				TraceServices::ENetProfilerConnectionMode::Outgoing;
			uint32 TypeIndex = 0;
			FString Name;
			uint32 Sum = 0;
			uint32 Min = 0;
			uint32 Max = 0;
			uint32 Average = 0;
			uint32 Count = 0;
		};
		TMap<uint32, FString> StatNames;
		Provider->ReadNetStatsCounterTypes(
			[&](const TraceServices::FNetProfilerStatsCounterType* Types, const uint64 Count)
			{
				for (uint64 Index = 0; Types && Index < Count; ++Index)
				{
					FString Name = TEXT("<unknown>");
					Provider->ReadName(Types[Index].NameIndex,
						[&](const TraceServices::FNetProfilerName& NetName)
						{
							Name = NetName.Name ? NetName.Name : TEXT("<unknown>");
						});
					StatNames.Add(Types[Index].StatsCounterTypeIndex, Name);
				}
			});
		TArray<FStatsRow> StatsRows;
		bool bStatsRowsBounded = false;
		for (const FNetworkConnectionRow& Connection : Connections)
		{
			for (const TraceServices::ENetProfilerConnectionMode Mode : {
				TraceServices::ENetProfilerConnectionMode::Outgoing,
				TraceServices::ENetProfilerConnectionMode::Incoming})
			{
				if (!DirectionAllowed(Direction, Mode))
				{
					continue;
				}
				TArray<const FPacketRow*> ConnectionPackets;
				for (const FPacketRow& Packet : Packets)
				{
					if (Packet.Connection.ConnectionIndex == Connection.ConnectionIndex
						&& Packet.Mode == Mode)
					{
						ConnectionPackets.Add(&Packet);
					}
				}
				if (ConnectionPackets.IsEmpty())
				{
					continue;
				}
				TUniquePtr<TraceServices::ITable<TraceServices::FNetProfilerAggregatedStatsCounterStats>>
					Table(Provider->CreateStatsCountersAggregation(
						Connection.ConnectionIndex, Mode,
						ConnectionPackets[0]->PacketIndex,
						ConnectionPackets.Last()->PacketIndex));
				bool bTableBounded = false;
				const TArray<TraceServices::FNetProfilerAggregatedStatsCounterStats> TableRows =
					ReadBoundedTable(MoveTemp(Table), bTableBounded);
				bCollectionBounded = bCollectionBounded || bTableBounded;
				for (const TraceServices::FNetProfilerAggregatedStatsCounterStats& Stats : TableRows)
				{
					const FString StatName =
						StatNames.FindRef(Stats.StatsCounterTypeIndex);
					if (!MatchesFilter(
						FString::Printf(
							TEXT("%s %s %u %u"),
							*StatName,
							NetworkDirectionName(Mode),
							Connection.ConnectionId,
							Stats.StatsCounterTypeIndex),
						Request.Filter))
					{
						continue;
					}
					if (StatsRows.Num() >= MaximumAggregationRows)
					{
						bStatsRowsBounded = true;
						break;
					}
					StatsRows.Add({Connection.ConnectionId, Mode,
						Stats.StatsCounterTypeIndex, StatName,
						Stats.Sum, Stats.Min, Stats.Max, Stats.Average, Stats.Count});
				}
				if (bStatsRowsBounded)
				{
					break;
				}
			}
			if (bStatsRowsBounded)
			{
				break;
			}
		}
		bCollectionBounded = bCollectionBounded || bStatsRowsBounded;
		StatsRows.Sort([](const FStatsRow& Left, const FStatsRow& Right)
		{
			if (Left.ConnectionId != Right.ConnectionId)
			{
				return Left.ConnectionId < Right.ConnectionId;
			}
			if (Left.Mode != Right.Mode)
			{
				return static_cast<uint8>(Left.Mode) < static_cast<uint8>(Right.Mode);
			}
			return Left.TypeIndex < Right.TypeIndex;
		});
		Result.Columns = {TEXT("connectionId"), TEXT("direction"), TEXT("statType"),
			TEXT("name"), TEXT("sum"), TEXT("min"), TEXT("max"), TEXT("average"), TEXT("count")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(StatsRows.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FStatsRow& Stats = StatsRows[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("connectionId"), FTraceValue::Integer(Stats.ConnectionId))
				.Add(TEXT("direction"), FTraceValue::String(NetworkDirectionName(Stats.Mode)))
				.Add(TEXT("statType"), FTraceValue::Integer(Stats.TypeIndex))
				.Add(TEXT("name"), FTraceValue::String(
					Stats.Name.IsEmpty() ? TEXT("<unknown>") : Stats.Name))
				.Add(TEXT("sum"), FTraceValue::Integer(Stats.Sum))
				.Add(TEXT("min"), FTraceValue::Integer(Stats.Min))
				.Add(TEXT("max"), FTraceValue::Integer(Stats.Max))
				.Add(TEXT("average"), FTraceValue::Integer(Stats.Average))
				.Add(TEXT("count"), FTraceValue::Integer(Stats.Count));
		}
		FinishPage(StatsRows.Num(), Request.Page, Limit, Result);
		if (bCollectionBounded)
		{
			AddCollectionBoundDiagnostic(Result);
		}
		return true;
	}

	ErrorCode = TEXT("trace_query_unsupported");
	ErrorMessage = TEXT("Network queries support connections, packets, contentEvents, and stats.");
	return false;
}

struct FTaskSemanticRow
{
	uint64 Id = 0;
	FString Name;
	bool bTracked = false;
	int32 ThreadToExecuteOn = 0;
	double Created = 0.0;
	double Launched = 0.0;
	double Scheduled = 0.0;
	double Started = 0.0;
	double Finished = 0.0;
	double Completed = 0.0;
	double Destroyed = 0.0;
	uint32 CreatedThreadId = 0;
	uint32 LaunchedThreadId = 0;
	uint32 ScheduledThreadId = 0;
	uint32 StartedThreadId = 0;
	uint32 CompletedThreadId = 0;
	uint64 TaskSize = 0;
	TArray<TraceServices::FTaskInfo::FRelationInfo> Prerequisites;
	TArray<TraceServices::FTaskInfo::FRelationInfo> Subsequents;
	TArray<TraceServices::FTaskInfo::FRelationInfo> ParentTasks;
	TArray<TraceServices::FTaskInfo::FRelationInfo> NestedTasks;
};

TArray<FTaskSemanticRow> CollectTasks(
	const TraceServices::ITasksProvider& Provider,
	const FResolvedRange& Range,
	const FTraceQueryRequest& Request,
	bool& bOutBounded)
{
	uint64 RequestedTaskId = 0;
	const bool bHasTaskId = OptionUInt64(Request, TEXT("taskId"), RequestedTaskId);
	bOutBounded = false;
	TArray<FTaskSemanticRow> Tasks;
	Provider.EnumerateTasks(
		Range.Start, Range.End, TraceServices::ETaskEnumerationOption::Alive,
		[&](const TraceServices::FTaskInfo& Task)
		{
			const FString Name = Task.DebugName ? Task.DebugName : TEXT("<unnamed>");
			if ((bHasTaskId && Task.Id != RequestedTaskId)
				|| !MatchesFilter(Name, Request.Filter))
			{
				return TraceServices::ETaskEnumerationResult::Continue;
			}
			if (Tasks.Num() >= MaximumAggregationRows)
			{
				bOutBounded = true;
				return TraceServices::ETaskEnumerationResult::Stop;
			}
			FTaskSemanticRow& Row = Tasks.AddDefaulted_GetRef();
			Row.Id = Task.Id;
			Row.Name = Name;
			Row.bTracked = Task.bTracked;
			Row.ThreadToExecuteOn = Task.ThreadToExecuteOn;
			Row.Created = Task.CreatedTimestamp;
			Row.Launched = Task.LaunchedTimestamp;
			Row.Scheduled = Task.ScheduledTimestamp;
			Row.Started = Task.StartedTimestamp;
			Row.Finished = Task.FinishedTimestamp;
			Row.Completed = Task.CompletedTimestamp;
			Row.Destroyed = Task.DestroyedTimestamp;
			Row.CreatedThreadId = Task.CreatedThreadId;
			Row.LaunchedThreadId = Task.LaunchedThreadId;
			Row.ScheduledThreadId = Task.ScheduledThreadId;
			Row.StartedThreadId = Task.StartedThreadId;
			Row.CompletedThreadId = Task.CompletedThreadId;
			Row.TaskSize = Task.TaskSize;
			Row.Prerequisites = Task.Prerequisites;
			Row.Subsequents = Task.Subsequents;
			Row.ParentTasks = Task.ParentTasks;
			Row.NestedTasks = Task.NestedTasks;
			return TraceServices::ETaskEnumerationResult::Continue;
		});
	Tasks.Sort([](const FTaskSemanticRow& Left, const FTaskSemanticRow& Right)
	{
		const double LeftOrder = Left.Launched > 0.0 ? Left.Launched : Left.Created;
		const double RightOrder = Right.Launched > 0.0 ? Right.Launched : Right.Created;
		return LeftOrder != RightOrder ? LeftOrder < RightOrder : Left.Id < Right.Id;
	});
	return Tasks;
}

double ValidTaskDeltaMs(const double Begin, const double End)
{
	return Begin > 0.0 && End >= Begin ? (End - Begin) * 1000.0 : 0.0;
}

bool QueryTasks(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::ITasksProvider* Provider =
		Session.ReadProvider<TraceServices::ITasksProvider>(
			TraceServices::GetTaskProviderName());
	if (!Provider || Provider->GetNumTasks() == 0)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain Task events.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	bool bCollectionBounded = false;
	const TArray<FTaskSemanticRow> Tasks =
		CollectTasks(*Provider, Range, Request, bCollectionBounded);

	if (Request.Operation == TEXT("tasks"))
	{
		Result.Columns = {TEXT("taskId"), TEXT("name"), TEXT("tracked"),
			TEXT("threadToExecuteOn"), TEXT("createdSeconds"), TEXT("launchedSeconds"),
			TEXT("scheduledSeconds"), TEXT("startedSeconds"), TEXT("finishedSeconds"),
			TEXT("completedSeconds"), TEXT("executionMs"), TEXT("queueWaitMs"),
			TEXT("startedThreadId"), TEXT("taskSizeBytes"), TEXT("prerequisiteCount"),
			TEXT("subsequentCount"), TEXT("parentCount"), TEXT("nestedCount")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Tasks.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FTaskSemanticRow& Task = Tasks[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("taskId"), FTraceValue::String(LexToString(Task.Id)))
				.Add(TEXT("name"), FTraceValue::String(Task.Name))
				.Add(TEXT("tracked"), FTraceValue::Boolean(Task.bTracked))
				.Add(TEXT("threadToExecuteOn"), FTraceValue::Integer(Task.ThreadToExecuteOn))
				.Add(TEXT("createdSeconds"), FTraceValue::Number(Task.Created))
				.Add(TEXT("launchedSeconds"), FTraceValue::Number(Task.Launched))
				.Add(TEXT("scheduledSeconds"), FTraceValue::Number(Task.Scheduled))
				.Add(TEXT("startedSeconds"), FTraceValue::Number(Task.Started))
				.Add(TEXT("finishedSeconds"), FTraceValue::Number(Task.Finished))
				.Add(TEXT("completedSeconds"), FTraceValue::Number(Task.Completed))
				.Add(TEXT("executionMs"), FTraceValue::Number(
					ValidTaskDeltaMs(Task.Started, Task.Finished)))
				.Add(TEXT("queueWaitMs"), FTraceValue::Number(
					ValidTaskDeltaMs(Task.Scheduled, Task.Started)))
				.Add(TEXT("startedThreadId"), FTraceValue::Integer(Task.StartedThreadId))
				.Add(TEXT("taskSizeBytes"), FTraceValue::String(LexToString(Task.TaskSize)))
				.Add(TEXT("prerequisiteCount"), FTraceValue::Integer(Task.Prerequisites.Num()))
				.Add(TEXT("subsequentCount"), FTraceValue::Integer(Task.Subsequents.Num()))
				.Add(TEXT("parentCount"), FTraceValue::Integer(Task.ParentTasks.Num()))
				.Add(TEXT("nestedCount"), FTraceValue::Integer(Task.NestedTasks.Num()));
		}
		FinishPage(Tasks.Num(), Request.Page, Limit, Result);
	}
	else if (Request.Operation == TEXT("relations"))
	{
		struct FRelationRow
		{
			uint64 TaskId = 0;
			FString Relation;
			uint64 RelativeId = 0;
			double Time = 0.0;
			uint32 ThreadId = 0;
		};
		TArray<FRelationRow> Relations;
		auto Append = [&](const FTaskSemanticRow& Task, const FString& Relation,
			const TArray<TraceServices::FTaskInfo::FRelationInfo>& Values)
		{
			for (const TraceServices::FTaskInfo::FRelationInfo& Value : Values)
			{
				if (Relations.Num() >= MaximumAggregationRows)
				{
					bCollectionBounded = true;
					return;
				}
				Relations.Add({Task.Id, Relation, Value.RelativeId, Value.Timestamp, Value.ThreadId});
			}
		};
		for (const FTaskSemanticRow& Task : Tasks)
		{
			Append(Task, TEXT("prerequisite"), Task.Prerequisites);
			Append(Task, TEXT("subsequent"), Task.Subsequents);
			Append(Task, TEXT("parent"), Task.ParentTasks);
			Append(Task, TEXT("nested"), Task.NestedTasks);
		}
		Relations.Sort([](const FRelationRow& Left, const FRelationRow& Right)
		{
			if (Left.Time != Right.Time)
			{
				return Left.Time < Right.Time;
			}
			if (Left.TaskId != Right.TaskId)
			{
				return Left.TaskId < Right.TaskId;
			}
			const int32 TypeOrder = Left.Relation.Compare(Right.Relation);
			return TypeOrder != 0 ? TypeOrder < 0 : Left.RelativeId < Right.RelativeId;
		});
		Result.Columns = {TEXT("taskId"), TEXT("relation"), TEXT("relativeTaskId"),
			TEXT("timeSeconds"), TEXT("threadId")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Relations.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FRelationRow& Relation = Relations[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("taskId"), FTraceValue::String(LexToString(Relation.TaskId)))
				.Add(TEXT("relation"), FTraceValue::String(Relation.Relation))
				.Add(TEXT("relativeTaskId"), FTraceValue::String(LexToString(Relation.RelativeId)))
				.Add(TEXT("timeSeconds"), FTraceValue::Number(Relation.Time))
				.Add(TEXT("threadId"), FTraceValue::Integer(Relation.ThreadId));
		}
		FinishPage(Relations.Num(), Request.Page, Limit, Result);
	}
	else if (Request.Operation == TEXT("waiting"))
	{
		struct FWaitRow
		{
			uint64 TaskId = 0;
			FString Name;
			FString WaitKind;
			double Begin = 0.0;
			double End = 0.0;
		};
		TArray<FWaitRow> Waits;
		for (const FTaskSemanticRow& Task : Tasks)
		{
			if (Task.Launched > 0.0 && Task.Scheduled > Task.Launched)
			{
				Waits.Add({Task.Id, Task.Name, TEXT("prerequisites"), Task.Launched, Task.Scheduled});
			}
			if (Task.Scheduled > 0.0 && Task.Started > Task.Scheduled)
			{
				Waits.Add({Task.Id, Task.Name, TEXT("queue"), Task.Scheduled, Task.Started});
			}
			if (Task.Finished > 0.0 && Task.Completed > Task.Finished)
			{
				Waits.Add({Task.Id, Task.Name, TEXT("nested"), Task.Finished, Task.Completed});
			}
		}
		Waits.Sort([](const FWaitRow& Left, const FWaitRow& Right)
		{
			return Left.Begin != Right.Begin ? Left.Begin < Right.Begin : Left.TaskId < Right.TaskId;
		});
		Result.Columns = {TEXT("taskId"), TEXT("name"), TEXT("waitKind"),
			TEXT("beginSeconds"), TEXT("endSeconds"), TEXT("durationMs")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Waits.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FWaitRow& Wait = Waits[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("taskId"), FTraceValue::String(LexToString(Wait.TaskId)))
				.Add(TEXT("name"), FTraceValue::String(Wait.Name))
				.Add(TEXT("waitKind"), FTraceValue::String(Wait.WaitKind))
				.Add(TEXT("beginSeconds"), FTraceValue::Number(Wait.Begin))
				.Add(TEXT("endSeconds"), FTraceValue::Number(Wait.End))
				.Add(TEXT("durationMs"), FTraceValue::Number((Wait.End - Wait.Begin) * 1000.0));
		}
		FinishPage(Waits.Num(), Request.Page, Limit, Result);
		FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Severity = TEXT("info");
		Diagnostic.Code = TEXT("trace_tasks_waiting_method");
		Diagnostic.Message = TEXT(
			"Wait intervals are derived from recorded task lifecycle transitions: launch-to-schedule, schedule-to-start, and finish-to-complete.");
	}
	else if (Request.Operation == TEXT("criticalPath"))
	{
		struct FCriticalState
		{
			double ScoreMs = 0.0;
			uint64 Previous = 0;
		};
		TMap<uint64, FCriticalState> StateById;
		TMap<uint64, const FTaskSemanticRow*> TaskById;
		for (const FTaskSemanticRow& Task : Tasks)
		{
			TaskById.Add(Task.Id, &Task);
			FCriticalState& State = StateById.Add(Task.Id);
			State.ScoreMs = ValidTaskDeltaMs(Task.Started, Task.Finished);
		}

		// Resolve the recorded prerequisite DAG independently of launch order. A
		// malformed cycle is ignored at its back edge and reported explicitly.
		struct FVisitFrame
		{
			uint64 TaskId = 0;
			int32 NextPrerequisite = 0;
		};
		TMap<uint64, uint8> VisitState;
		bool bCycleDetected = false;
		for (const FTaskSemanticRow& RootTask : Tasks)
		{
			if (VisitState.FindRef(RootTask.Id) == 2)
			{
				continue;
			}
			TArray<FVisitFrame> Stack;
			Stack.Add({RootTask.Id, 0});
			while (!Stack.IsEmpty())
			{
				FVisitFrame& Frame = Stack.Last();
				const FTaskSemanticRow* const* TaskPtr = TaskById.Find(Frame.TaskId);
				if (!TaskPtr || !*TaskPtr)
				{
					VisitState.Add(Frame.TaskId, 2);
					Stack.Pop(false);
					continue;
				}
				const FTaskSemanticRow& Task = **TaskPtr;
				uint8& CurrentVisitState = VisitState.FindOrAdd(Task.Id);
				if (CurrentVisitState == 0)
				{
					CurrentVisitState = 1;
				}
				if (Frame.NextPrerequisite < Task.Prerequisites.Num())
				{
					const uint64 PrerequisiteId =
						Task.Prerequisites[Frame.NextPrerequisite++].RelativeId;
					if (!TaskById.Contains(PrerequisiteId))
					{
						continue;
					}
					const uint8 PrerequisiteVisitState = VisitState.FindRef(PrerequisiteId);
					if (PrerequisiteVisitState == 0)
					{
						Stack.Add({PrerequisiteId, 0});
						continue;
					}
					if (PrerequisiteVisitState == 1)
					{
						bCycleDetected = true;
						continue;
					}
					const FCriticalState* PreviousState =
						StateById.Find(PrerequisiteId);
					FCriticalState* CurrentState = StateById.Find(Task.Id);
					if (!PreviousState || !CurrentState)
					{
						continue;
					}
					const double Candidate = PreviousState->ScoreMs
						+ ValidTaskDeltaMs(Task.Started, Task.Finished);
					if (Candidate > CurrentState->ScoreMs
						|| (FMath::IsNearlyEqual(Candidate, CurrentState->ScoreMs)
							&& (CurrentState->Previous == 0
								|| PrerequisiteId < CurrentState->Previous)))
					{
						CurrentState->ScoreMs = Candidate;
						CurrentState->Previous = PrerequisiteId;
					}
					continue;
				}
				CurrentVisitState = 2;
				Stack.Pop(false);
			}
		}
		uint64 Tail = 0;
		double BestScore = 0.0;
		for (const TPair<uint64, FCriticalState>& Pair : StateById)
		{
			if (Pair.Value.ScoreMs > BestScore
				|| (Pair.Value.ScoreMs == BestScore && (Tail == 0 || Pair.Key < Tail)))
			{
				Tail = Pair.Key;
				BestScore = Pair.Value.ScoreMs;
			}
		}
		TArray<uint64> Path;
		TSet<uint64> Visited;
		while (Tail != 0 && !Visited.Contains(Tail))
		{
			Visited.Add(Tail);
			Path.Add(Tail);
			const FCriticalState* State = StateById.Find(Tail);
			Tail = State ? State->Previous : 0;
		}
		Algo::Reverse(Path);
		Result.Columns = {TEXT("pathIndex"), TEXT("taskId"), TEXT("name"),
			TEXT("startedSeconds"), TEXT("finishedSeconds"), TEXT("executionMs"),
			TEXT("cumulativeMs"), TEXT("method")};
		double Cumulative = 0.0;
		TArray<FTraceRow> PathRows;
		for (int32 Index = 0; Index < Path.Num(); ++Index)
		{
			const FTaskSemanticRow* const* TaskPtr = TaskById.Find(Path[Index]);
			if (!TaskPtr || !*TaskPtr)
			{
				continue;
			}
			const FTaskSemanticRow& Task = **TaskPtr;
			const double ExecutionMs = ValidTaskDeltaMs(Task.Started, Task.Finished);
			Cumulative += ExecutionMs;
			FTraceRow& Row = PathRows.AddDefaulted_GetRef();
			Row.Add(TEXT("pathIndex"), FTraceValue::Integer(Index))
				.Add(TEXT("taskId"), FTraceValue::String(LexToString(Task.Id)))
				.Add(TEXT("name"), FTraceValue::String(Task.Name))
				.Add(TEXT("startedSeconds"), FTraceValue::Number(Task.Started))
				.Add(TEXT("finishedSeconds"), FTraceValue::Number(Task.Finished))
				.Add(TEXT("executionMs"), FTraceValue::Number(ExecutionMs))
				.Add(TEXT("cumulativeMs"), FTraceValue::Number(Cumulative))
				.Add(TEXT("method"), FTraceValue::String(
					TEXT("longest prerequisite chain by task execution time")));
		}
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(PathRows.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			Result.Rows.Add(MoveTemp(PathRows[static_cast<int32>(Index)]));
		}
		FinishPage(PathRows.Num(), Request.Page, Limit, Result);
		FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
		Diagnostic.Severity = TEXT("info");
		Diagnostic.Code = TEXT("trace_tasks_critical_path_method");
		Diagnostic.Message = TEXT(
			"Critical path is the deterministic longest recorded prerequisite chain weighted by task execution time; it does not infer unrecorded scheduler dependencies.");
		if (bCycleDetected)
		{
			FTraceDiagnostic& CycleDiagnostic =
				Result.Diagnostics.AddDefaulted_GetRef();
			CycleDiagnostic.Severity = TEXT("warning");
			CycleDiagnostic.Code = TEXT("trace_tasks_dependency_cycle");
			CycleDiagnostic.Message = TEXT(
				"The recorded task prerequisite graph contains a cycle; cyclic back edges were excluded from the critical-path calculation.");
		}
	}
	else
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Task queries support tasks, relations, waiting, and criticalPath.");
		return false;
	}
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

bool QueryContextSwitches(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::IContextSwitchesProvider* Provider =
		Session.ReadProvider<TraceServices::IContextSwitchesProvider>(
			TraceServices::GetContextSwitchesProviderName());
	if (!Provider || !Provider->HasData())
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain context-switch data.");
		return false;
	}
	const TraceServices::IThreadProvider* ThreadProvider =
		Session.ReadProvider<TraceServices::IThreadProvider>(
			TraceServices::GetThreadProviderName());
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	uint32 RequestedCore = 0;
	uint32 RequestedThread = 0;
	const bool bHasCore = OptionUInt32(Request, TEXT("core"), RequestedCore);
	const bool bHasThread = OptionUInt32(Request, TEXT("threadId"), RequestedThread);

	struct FIntervalRow
	{
		uint32 Core = 0;
		uint32 SystemThreadId = 0;
		uint32 ThreadId = 0;
		FString ThreadName;
		double Start = 0.0;
		double End = 0.0;
	};
	TArray<FIntervalRow> Intervals;
	bool bCollectionBounded = false;
	auto AddCoreEvents = [&](const uint32 Core)
	{
		Provider->EnumerateCpuCoreEvents(
			Core, Range.Start, Range.End,
			[&](const TraceServices::FCpuCoreEvent& Event)
			{
				uint32 ThreadId = 0;
				Provider->GetThreadId(Event.SystemThreadId, ThreadId);
				if (bHasThread && ThreadId != RequestedThread)
				{
					return TraceServices::EContextSwitchEnumerationResult::Continue;
				}
				if (Intervals.Num() >= MaximumAggregationRows)
				{
					bCollectionBounded = true;
					return TraceServices::EContextSwitchEnumerationResult::Stop;
				}
				const TCHAR* Name = ThreadProvider ? ThreadProvider->GetThreadName(ThreadId) : nullptr;
				const FString ThreadName = Name ? Name : TEXT("<unknown>");
				if (!MatchesFilter(
					FString::Printf(
						TEXT("%s %u %u %u"),
						*ThreadName,
						ThreadId,
						Event.SystemThreadId,
						Core),
					Request.Filter))
				{
					return TraceServices::EContextSwitchEnumerationResult::Continue;
				}
				Intervals.Add({Core, Event.SystemThreadId, ThreadId,
					ThreadName, Event.Start, Event.End});
				return TraceServices::EContextSwitchEnumerationResult::Continue;
			});
	};
	if (bHasCore)
	{
		AddCoreEvents(RequestedCore);
	}
	else
	{
		Provider->EnumerateCpuCores([&](const TraceServices::FCpuCoreInfo& Core)
		{
			if (!bCollectionBounded)
			{
				AddCoreEvents(Core.CoreNumber);
			}
		});
	}
	Intervals.Sort([](const FIntervalRow& Left, const FIntervalRow& Right)
	{
		if (Left.Start != Right.Start)
		{
			return Left.Start < Right.Start;
		}
		return Left.Core != Right.Core ? Left.Core < Right.Core
			: Left.SystemThreadId < Right.SystemThreadId;
	});

	if (Request.Operation == TEXT("intervals") || Request.Operation == TEXT("threads"))
	{
		if (Request.Operation == TEXT("threads"))
		{
			struct FThreadAggregate
			{
				uint32 ThreadId = 0;
				uint32 SystemThreadId = 0;
				FString Name;
				uint64 IntervalCount = 0;
				double RunningSeconds = 0.0;
				uint32 CoreCount = 0;
				TSet<uint32> Cores;
			};
			TMap<uint32, FThreadAggregate> BySystemThread;
			for (const FIntervalRow& Interval : Intervals)
			{
				FThreadAggregate& Aggregate = BySystemThread.FindOrAdd(Interval.SystemThreadId);
				Aggregate.ThreadId = Interval.ThreadId;
				Aggregate.SystemThreadId = Interval.SystemThreadId;
				Aggregate.Name = Interval.ThreadName;
				++Aggregate.IntervalCount;
				Aggregate.RunningSeconds += FMath::Max(0.0, Interval.End - Interval.Start);
				Aggregate.Cores.Add(Interval.Core);
			}
			TArray<FThreadAggregate> Threads;
			BySystemThread.GenerateValueArray(Threads);
			Threads.Sort([](const FThreadAggregate& Left, const FThreadAggregate& Right)
			{
				return Left.RunningSeconds != Right.RunningSeconds
					? Left.RunningSeconds > Right.RunningSeconds
					: Left.SystemThreadId < Right.SystemThreadId;
			});
			Result.Columns = {TEXT("threadId"), TEXT("systemThreadId"), TEXT("name"),
				TEXT("intervalCount"), TEXT("runningMs"), TEXT("coreCount")};
			for (uint64 Index = Request.Page.Cursor;
				Index < static_cast<uint64>(Threads.Num()) && Result.Rows.Num() < Limit;
				++Index)
			{
				const FThreadAggregate& Thread = Threads[static_cast<int32>(Index)];
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("threadId"), FTraceValue::Integer(Thread.ThreadId))
					.Add(TEXT("systemThreadId"), FTraceValue::Integer(Thread.SystemThreadId))
					.Add(TEXT("name"), FTraceValue::String(Thread.Name))
					.Add(TEXT("intervalCount"), FTraceValue::Integer(Thread.IntervalCount))
					.Add(TEXT("runningMs"), FTraceValue::Number(Thread.RunningSeconds * 1000.0))
					.Add(TEXT("coreCount"), FTraceValue::Integer(Thread.Cores.Num()));
			}
			FinishPage(Threads.Num(), Request.Page, Limit, Result);
		}
		else
		{
			Result.Columns = {TEXT("core"), TEXT("systemThreadId"), TEXT("threadId"),
				TEXT("threadName"), TEXT("startSeconds"), TEXT("endSeconds"), TEXT("durationMs")};
			for (uint64 Index = Request.Page.Cursor;
				Index < static_cast<uint64>(Intervals.Num()) && Result.Rows.Num() < Limit;
				++Index)
			{
				const FIntervalRow& Interval = Intervals[static_cast<int32>(Index)];
				FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
				Row.Add(TEXT("core"), FTraceValue::Integer(Interval.Core))
					.Add(TEXT("systemThreadId"), FTraceValue::Integer(Interval.SystemThreadId))
					.Add(TEXT("threadId"), FTraceValue::Integer(Interval.ThreadId))
					.Add(TEXT("threadName"), FTraceValue::String(Interval.ThreadName))
					.Add(TEXT("startSeconds"), FTraceValue::Number(Interval.Start))
					.Add(TEXT("endSeconds"), FTraceValue::Number(Interval.End))
					.Add(TEXT("durationMs"), FTraceValue::Number(
						FMath::Max(0.0, Interval.End - Interval.Start) * 1000.0));
			}
			FinishPage(Intervals.Num(), Request.Page, Limit, Result);
		}
	}
	else if (Request.Operation == TEXT("cores"))
	{
		struct FCoreAggregate
		{
			uint32 Core = 0;
			uint64 IntervalCount = 0;
			double RunningSeconds = 0.0;
			TSet<uint32> Threads;
		};
		TMap<uint32, FCoreAggregate> ByCore;
		for (const FIntervalRow& Interval : Intervals)
		{
			FCoreAggregate& Aggregate = ByCore.FindOrAdd(Interval.Core);
			Aggregate.Core = Interval.Core;
			++Aggregate.IntervalCount;
			Aggregate.RunningSeconds += FMath::Max(0.0, Interval.End - Interval.Start);
			Aggregate.Threads.Add(Interval.SystemThreadId);
		}
		TArray<FCoreAggregate> Cores;
		ByCore.GenerateValueArray(Cores);
		Cores.Sort([](const FCoreAggregate& Left, const FCoreAggregate& Right)
		{
			return Left.Core < Right.Core;
		});
		Result.Columns = {TEXT("core"), TEXT("intervalCount"), TEXT("runningMs"),
			TEXT("utilization"), TEXT("threadCount")};
		const double Window = FMath::Max(0.0, Range.End - Range.Start);
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Cores.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FCoreAggregate& Core = Cores[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("core"), FTraceValue::Integer(Core.Core))
				.Add(TEXT("intervalCount"), FTraceValue::Integer(Core.IntervalCount))
				.Add(TEXT("runningMs"), FTraceValue::Number(Core.RunningSeconds * 1000.0))
				.Add(TEXT("utilization"), FTraceValue::Number(
					Window > 0.0 ? FMath::Clamp(Core.RunningSeconds / Window, 0.0, 1.0) : 0.0))
				.Add(TEXT("threadCount"), FTraceValue::Integer(Core.Threads.Num()));
		}
		FinishPage(Cores.Num(), Request.Page, Limit, Result);
	}
	else
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Context-switch queries support cores, threads, and intervals.");
		return false;
	}
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

bool QueryFileIo(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::IFileActivityProvider* Provider =
		Session.ReadProvider<TraceServices::IFileActivityProvider>(
			TraceServices::GetFileActivityProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain File Activity data.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	struct FIoRow
	{
		uint32 FileId = 0;
		FString Path;
		double Start = 0.0;
		double End = 0.0;
		uint64 Offset = 0;
		uint64 Size = 0;
		uint64 ActualSize = 0;
		uint32 ThreadId = 0;
		TraceServices::EFileActivityType Type = TraceServices::FileActivityType_Invalid;
		bool bFailed = false;
	};
	TArray<FIoRow> Events;
	bool bCollectionBounded = false;
	Provider->EnumerateFileActivity(
		[&](const TraceServices::FFileInfo& File,
			const TraceServices::IFileActivityProvider::Timeline& Timeline)
		{
			const FString Path = File.Path ? File.Path : TEXT("<unknown>");
			if (!MatchesFilter(Path, Request.Filter))
			{
				return true;
			}
			Timeline.EnumerateEvents(
				Range.Start, Range.End,
				[&](const double Start, const double End, const uint32,
					TraceServices::FFileActivity* const& Activity)
				{
					if (Events.Num() >= MaximumAggregationRows)
					{
						bCollectionBounded = true;
						return TraceServices::EEventEnumerate::Stop;
					}
					if (Activity)
					{
						Events.Add({File.Id, Path, Start, End, Activity->Offset,
							Activity->Size, Activity->ActualSize, Activity->ThreadId,
							Activity->ActivityType, Activity->Failed});
					}
					return TraceServices::EEventEnumerate::Continue;
				});
			return !bCollectionBounded;
		});
	Events.Sort([](const FIoRow& Left, const FIoRow& Right)
	{
		if (Left.Start != Right.Start)
		{
			return Left.Start < Right.Start;
		}
		const int32 PathOrder = Left.Path.Compare(Right.Path, ESearchCase::IgnoreCase);
		return PathOrder != 0 ? PathOrder < 0 : Left.Type < Right.Type;
	});
	if (Request.Operation == TEXT("events"))
	{
		Result.Columns = {TEXT("fileId"), TEXT("path"), TEXT("activity"),
			TEXT("startSeconds"), TEXT("endSeconds"), TEXT("durationMs"), TEXT("offset"),
			TEXT("requestedBytes"), TEXT("actualBytes"), TEXT("threadId"), TEXT("failed")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Events.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FIoRow& Event = Events[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("fileId"), FTraceValue::Integer(Event.FileId))
				.Add(TEXT("path"), FTraceValue::String(Event.Path))
				.Add(TEXT("activity"), FTraceValue::String(
					TraceServices::GetFileActivityTypeString(Event.Type)))
				.Add(TEXT("startSeconds"), FTraceValue::Number(Event.Start))
				.Add(TEXT("endSeconds"), FTraceValue::Number(Event.End))
				.Add(TEXT("durationMs"), FTraceValue::Number(
					FMath::Max(0.0, Event.End - Event.Start) * 1000.0))
				.Add(TEXT("offset"), FTraceValue::String(LexToString(Event.Offset)))
				.Add(TEXT("requestedBytes"), FTraceValue::String(LexToString(Event.Size)))
				.Add(TEXT("actualBytes"), FTraceValue::String(LexToString(Event.ActualSize)))
				.Add(TEXT("threadId"), FTraceValue::Integer(Event.ThreadId))
				.Add(TEXT("failed"), FTraceValue::Boolean(Event.bFailed));
		}
		FinishPage(Events.Num(), Request.Page, Limit, Result);
	}
	else if (Request.Operation == TEXT("files") || Request.Operation == TEXT("aggregate"))
	{
		struct FFileAggregate
		{
			uint32 FileId = 0;
			FString Path;
			uint64 EventCount = 0;
			uint64 ReadBytes = 0;
			uint64 WriteBytes = 0;
			uint64 FailedCount = 0;
			double DurationSeconds = 0.0;
		};
		TMap<uint32, FFileAggregate> ByFile;
		for (const FIoRow& Event : Events)
		{
			FFileAggregate& File = ByFile.FindOrAdd(Event.FileId);
			File.FileId = Event.FileId;
			File.Path = Event.Path;
			++File.EventCount;
			File.DurationSeconds += FMath::Max(0.0, Event.End - Event.Start);
			File.FailedCount += Event.bFailed ? 1 : 0;
			if (Event.Type == TraceServices::FileActivityType_Read)
			{
				File.ReadBytes += Event.ActualSize;
			}
			else if (Event.Type == TraceServices::FileActivityType_Write)
			{
				File.WriteBytes += Event.ActualSize;
			}
		}
		TArray<FFileAggregate> Files;
		ByFile.GenerateValueArray(Files);
		Files.Sort([&](const FFileAggregate& Left, const FFileAggregate& Right)
		{
			if (Request.Operation == TEXT("aggregate"))
			{
				const uint64 LeftBytes = Left.ReadBytes + Left.WriteBytes;
				const uint64 RightBytes = Right.ReadBytes + Right.WriteBytes;
				if (LeftBytes != RightBytes)
				{
					return LeftBytes > RightBytes;
				}
			}
			return Left.Path.Compare(Right.Path, ESearchCase::IgnoreCase) < 0;
		});
		Result.Columns = {TEXT("fileId"), TEXT("path"), TEXT("eventCount"),
			TEXT("readBytes"), TEXT("writeBytes"), TEXT("failedCount"), TEXT("totalDurationMs")};
		for (uint64 Index = Request.Page.Cursor;
			Index < static_cast<uint64>(Files.Num()) && Result.Rows.Num() < Limit;
			++Index)
		{
			const FFileAggregate& File = Files[static_cast<int32>(Index)];
			FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
			Row.Add(TEXT("fileId"), FTraceValue::Integer(File.FileId))
				.Add(TEXT("path"), FTraceValue::String(File.Path))
				.Add(TEXT("eventCount"), FTraceValue::String(LexToString(File.EventCount)))
				.Add(TEXT("readBytes"), FTraceValue::String(LexToString(File.ReadBytes)))
				.Add(TEXT("writeBytes"), FTraceValue::String(LexToString(File.WriteBytes)))
				.Add(TEXT("failedCount"), FTraceValue::String(LexToString(File.FailedCount)))
				.Add(TEXT("totalDurationMs"), FTraceValue::Number(File.DurationSeconds * 1000.0));
		}
		FinishPage(Files.Num(), Request.Page, Limit, Result);
	}
	else
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("File IO queries support files, events, and aggregate.");
		return false;
	}
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

FString ScreenshotFormat(const TraceServices::FScreenshot& Screenshot)
{
	const TArray<uint8>& Data = Screenshot.Data;
	if (Data.Num() >= 8
		&& Data[0] == 0x89 && Data[1] == 0x50 && Data[2] == 0x4e && Data[3] == 0x47)
	{
		return TEXT("png");
	}
	if (Data.Num() >= 3 && Data[0] == 0xff && Data[1] == 0xd8 && Data[2] == 0xff)
	{
		return TEXT("jpeg");
	}
	return TEXT("unknown");
}

bool QueryScreenshots(
	const TraceServices::IAnalysisSession& Session,
	const FTraceQueryRequest& Request,
	const double Duration,
	FTraceQueryResult& Result,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const TraceServices::IScreenshotProvider* Provider =
		Session.ReadProvider<TraceServices::IScreenshotProvider>(
			TraceServices::GetScreenshotProviderName());
	const TraceServices::ILogProvider* LogProvider =
		Session.ReadProvider<TraceServices::ILogProvider>(
			TraceServices::GetLogProviderName());
	if (!Provider || !LogProvider)
	{
		ErrorCode = TEXT("trace_provider_unavailable");
		ErrorMessage = TEXT("The trace does not contain Screenshot and Log providers.");
		return false;
	}
	const FResolvedRange Range = ResolveRange(Request.TimeRange, Duration);
	Result.IntervalStartSeconds = Range.Start;
	Result.IntervalEndSeconds = Range.End;
	AddRangeDiagnostic(Range, Result);
	const int32 Limit = ClampLimit(Request.Page.Limit);
	const bool bGet = Request.Operation == TEXT("get");
	if (!bGet && Request.Operation != TEXT("list"))
	{
		ErrorCode = TEXT("trace_query_unsupported");
		ErrorMessage = TEXT("Screenshot queries support list and get.");
		return false;
	}
	uint32 RequestedId = 0;
	if (bGet && !OptionUInt32(Request, TEXT("screenshotId"), RequestedId))
	{
		ErrorCode = TEXT("trace_query_invalid");
		ErrorMessage = TEXT("screenshotId is required for screenshot get.");
		return false;
	}
	TArray<TSharedPtr<const TraceServices::FScreenshot>> Screenshots;
	TSharedPtr<const TraceServices::FScreenshot> RequestedScreenshot;
	TSet<uint32> SeenIds;
	bool bCollectionBounded = false;
	LogProvider->EnumerateMessages(
		Range.Start, Range.End,
		[&](const TraceServices::FLogMessageInfo& Message)
		{
			const FString Category = Message.Category && Message.Category->Name
				? Message.Category->Name : TEXT("");
			if (!Category.Equals(TEXT("Screenshot"), ESearchCase::IgnoreCase))
			{
				return;
			}
			if (Message.Line < 0)
			{
				return;
			}
			const uint32 Id = static_cast<uint32>(Message.Line);
			const TSharedPtr<const TraceServices::FScreenshot> Screenshot =
				Provider->GetScreenshot(Id);
			if (!Screenshot.IsValid()
				|| !MatchesFilter(Screenshot->Name, Request.Filter))
			{
				return;
			}
			if (bGet)
			{
				if (Id == RequestedId)
				{
					RequestedScreenshot = Screenshot;
				}
				return;
			}
			if (SeenIds.Contains(Id))
			{
				return;
			}
			if (Screenshots.Num() >= MaximumAggregationRows)
			{
				bCollectionBounded = true;
				return;
			}
			SeenIds.Add(Id);
			Screenshots.Add(Screenshot);
		});
	Screenshots.Sort([](const TSharedPtr<const TraceServices::FScreenshot>& Left,
		const TSharedPtr<const TraceServices::FScreenshot>& Right)
	{
		return Left->Timestamp != Right->Timestamp
			? Left->Timestamp < Right->Timestamp : Left->Id < Right->Id;
	});

	if (bGet)
	{
		const TSharedPtr<const TraceServices::FScreenshot> Screenshot =
			RequestedScreenshot;
		if (!Screenshot.IsValid())
		{
			ErrorCode = TEXT("trace_screenshot_unavailable");
			ErrorMessage = TEXT(
				"The requested screenshot id is not present in the requested time range and filter.");
			return false;
		}
		Result.Columns = {TEXT("screenshotId"), TEXT("name"), TEXT("timeSeconds"),
			TEXT("width"), TEXT("height"), TEXT("sizeBytes"), TEXT("chunkCount"),
			TEXT("complete"), TEXT("format"), TEXT("dataBase64")};
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		const bool bComplete = Screenshot->Data.Num() == static_cast<int32>(Screenshot->Size);
		Row.Add(TEXT("screenshotId"), FTraceValue::String(LexToString(Screenshot->Id)))
			.Add(TEXT("name"), FTraceValue::String(Screenshot->Name))
			.Add(TEXT("timeSeconds"), FTraceValue::Number(Screenshot->Timestamp))
			.Add(TEXT("width"), FTraceValue::Integer(Screenshot->Width))
			.Add(TEXT("height"), FTraceValue::Integer(Screenshot->Height))
			.Add(TEXT("sizeBytes"), FTraceValue::Integer(Screenshot->Size))
			.Add(TEXT("chunkCount"), FTraceValue::Integer(Screenshot->ChunkNum))
			.Add(TEXT("complete"), FTraceValue::Boolean(bComplete))
			.Add(TEXT("format"), FTraceValue::String(ScreenshotFormat(*Screenshot)));
		if (bComplete && Screenshot->Data.Num() <= MaximumScreenshotPayloadBytes)
		{
			Row.Add(TEXT("dataBase64"), FTraceValue::String(
				FBase64::Encode(Screenshot->Data)));
		}
		else
		{
			Row.Add(TEXT("dataBase64"), FTraceValue::Null());
			FTraceDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Severity = TEXT("warning");
			Diagnostic.Code = bComplete
				? TEXT("trace_screenshot_payload_bounded")
				: TEXT("trace_screenshot_incomplete");
			Diagnostic.Message = bComplete
				? TEXT("The embedded screenshot exceeds the 1 MiB semantic response bound; export it as an artifact instead.")
				: TEXT("The embedded screenshot payload is incomplete.");
		}
		FinishPage(1, FTracePageRequest(), 1, Result);
		return true;
	}
	Result.Columns = {TEXT("screenshotId"), TEXT("name"), TEXT("timeSeconds"),
		TEXT("width"), TEXT("height"), TEXT("sizeBytes"), TEXT("chunkCount"),
		TEXT("complete"), TEXT("format")};
	for (uint64 Index = Request.Page.Cursor;
		Index < static_cast<uint64>(Screenshots.Num()) && Result.Rows.Num() < Limit;
		++Index)
	{
		const TraceServices::FScreenshot& Screenshot = *Screenshots[static_cast<int32>(Index)];
		FTraceRow& Row = Result.Rows.AddDefaulted_GetRef();
		Row.Add(TEXT("screenshotId"), FTraceValue::String(LexToString(Screenshot.Id)))
			.Add(TEXT("name"), FTraceValue::String(Screenshot.Name))
			.Add(TEXT("timeSeconds"), FTraceValue::Number(Screenshot.Timestamp))
			.Add(TEXT("width"), FTraceValue::Integer(Screenshot.Width))
			.Add(TEXT("height"), FTraceValue::Integer(Screenshot.Height))
			.Add(TEXT("sizeBytes"), FTraceValue::Integer(Screenshot.Size))
			.Add(TEXT("chunkCount"), FTraceValue::Integer(Screenshot.ChunkNum))
			.Add(TEXT("complete"), FTraceValue::Boolean(
				Screenshot.Data.Num() == static_cast<int32>(Screenshot.Size)))
			.Add(TEXT("format"), FTraceValue::String(ScreenshotFormat(Screenshot)));
	}
	FinishPage(Screenshots.Num(), Request.Page, Limit, Result);
	if (bCollectionBounded)
	{
		AddCollectionBoundDiagnostic(Result);
	}
	return true;
}

bool ApplyNamedRegionRange(
	const TraceServices::IAnalysisSession& Session,
	const double Duration,
	FTraceQueryRequest& InOutRequest,
	FTraceDiagnostic& OutDiagnostic,
	FString& ErrorCode,
	FString& ErrorMessage)
{
	const FString RegionName = Option(InOutRequest, TEXT("regionName"));
	if (RegionName.IsEmpty()
		|| InOutRequest.TimeRange.bHasStart
		|| InOutRequest.TimeRange.bHasEnd)
	{
		return true;
	}
	const TraceServices::IRegionProvider* Provider =
		Session.ReadProvider<TraceServices::IRegionProvider>(
			TraceServices::GetRegionProviderName());
	if (!Provider)
	{
		ErrorCode = TEXT("trace_region_unavailable");
		ErrorMessage = FString::Printf(
			TEXT("Region '%s' was requested, but this trace has no Region provider."),
			*RegionName);
		return false;
	}
	bool bFound = false;
	double SelectedStart = 0.0;
	double SelectedEnd = 0.0;
	TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);
	Provider->EnumerateRegions(
		0.0,
		Duration,
		[&](const TraceServices::FTimeRegion& Region)
		{
			const FString Name = Region.Text ? Region.Text : TEXT("");
			if (Name.Equals(RegionName, ESearchCase::CaseSensitive)
				&& (!bFound || Region.BeginTime >= SelectedStart))
			{
				bFound = true;
				SelectedStart = FMath::Clamp(Region.BeginTime, 0.0, Duration);
				SelectedEnd = FMath::IsFinite(Region.EndTime)
					? FMath::Clamp(Region.EndTime, SelectedStart, Duration)
					: Duration;
			}
			return true;
		});
	if (!bFound)
	{
		ErrorCode = TEXT("trace_region_unavailable");
		ErrorMessage = FString::Printf(
			TEXT("The exact managed region '%s' is not present in this trace."),
			*RegionName);
		return false;
	}
	InOutRequest.TimeRange.bHasStart = true;
	InOutRequest.TimeRange.bHasEnd = true;
	InOutRequest.TimeRange.StartSeconds = SelectedStart;
	InOutRequest.TimeRange.EndSeconds = SelectedEnd;
	OutDiagnostic.Severity = TEXT("info");
	OutDiagnostic.Code = TEXT("trace_region_range_applied");
	OutDiagnostic.Message = FString::Printf(
		TEXT("The query interval was resolved from exact region '%s' [%.6f, %.6f]."),
		*RegionName, SelectedStart, SelectedEnd);
	return true;
}
}

FTraceAnalysisSession::FTraceAnalysisSession() = default;

FTraceAnalysisSession::~FTraceAnalysisSession()
{
	Close();
}

bool FTraceAnalysisSession::Open(
	const FString& TracePath,
	const double TimeoutSeconds,
	FString& OutErrorCode,
	FString& OutErrorMessage,
	const bool bAllowUnknownEngineVersionForTestFixture)
{
	Close();
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	const FString FullPath = FPaths::ConvertRelativePathToFull(TracePath);
	if (!FullPath.EndsWith(TEXT(".utrace"), ESearchCase::IgnoreCase)
		|| IFileManager::Get().FileSize(*FullPath) <= 0)
	{
		OutErrorCode = TEXT("trace_artifact_unavailable");
		OutErrorMessage = TEXT("The trace path is not a readable non-empty .utrace file.");
		return false;
	}

	ITraceServicesModule* Module =
		FModuleManager::LoadModulePtr<ITraceServicesModule>(TEXT("TraceServices"));
	if (!Module)
	{
		OutErrorCode = TEXT("trace_analysis_unavailable");
		OutErrorMessage = TEXT("TraceServices could not be loaded by this process.");
		return false;
	}
	EnableSemanticAnalysisModules(*Module);
	const TSharedPtr<TraceServices::IAnalysisService> AnalysisService =
		Module->GetAnalysisService();
	if (!AnalysisService.IsValid())
	{
		OutErrorCode = TEXT("trace_analysis_unavailable");
		OutErrorMessage = TEXT("TraceServices did not provide an analysis service.");
		return false;
	}

	Session = AnalysisService->StartAnalysis(*FullPath);
	if (!Session.IsValid())
	{
		OutErrorCode = TEXT("trace_analysis_rejected");
		OutErrorMessage = TEXT("TraceServices rejected the trace artifact.");
		return false;
	}
	const double Deadline = FPlatformTime::Seconds()
		+ FMath::Clamp(TimeoutSeconds, 1.0, 600.0);
	while (!Session->IsAnalysisComplete())
	{
		if (FPlatformTime::Seconds() >= Deadline)
		{
			Session->Stop(true);
			Session.Reset();
			OutErrorCode = TEXT("trace_analysis_timeout");
			OutErrorMessage = TEXT("TraceServices analysis exceeded its bounded timeout.");
			return false;
		}
		FPlatformProcess::SleepNoStats(0.01f);
	}
	return InitializeCompletedSession(
		FullPath,
		bAllowUnknownEngineVersionForTestFixture,
		OutErrorCode,
		OutErrorMessage);
}

bool FTraceAnalysisSession::AttachCompletedSession(
	const FString& TracePath,
	TSharedPtr<const TraceServices::IAnalysisSession> CompletedSession,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	Close();
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	if (!CompletedSession.IsValid()
		|| !CompletedSession->IsAnalysisComplete())
	{
		OutErrorCode = TEXT("trace_analysis_not_complete");
		OutErrorMessage = TEXT(
			"Only a completed TraceServices analysis session can be attached.");
		return false;
	}
	Session = MoveTemp(CompletedSession);
	return InitializeCompletedSession(
		FPaths::ConvertRelativePathToFull(TracePath),
		false,
		OutErrorCode,
		OutErrorMessage);
}

bool FTraceAnalysisSession::InitializeCompletedSession(
	const FString& TracePath,
	const bool bAllowUnknownEngineVersionForTestFixture,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	if (!Session.IsValid() || !Session->IsAnalysisComplete())
	{
		OutErrorCode = TEXT("trace_analysis_not_complete");
		OutErrorMessage = TEXT(
			"TraceServices did not provide a completed analysis session.");
		return false;
	}

	FManagedEngineMarker ManagedMarker;
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session);
		const double RawDuration = Session->GetDurationSeconds();
		DurationSeconds = FMath::IsFinite(RawDuration) && RawDuration >= 0.0
			? RawDuration
			: 0.0;
		const TraceServices::IDiagnosticsProvider* Diagnostics =
			Session->ReadProvider<TraceServices::IDiagnosticsProvider>(
				TraceServices::GetDiagnosticsProviderName());
		if (Diagnostics && Diagnostics->IsSessionInfoAvailable())
		{
			RecordedBuildVersion = Diagnostics->GetSessionInfo().BuildVersion;
		}
		if (RecordedBuildVersion.IsEmpty())
		{
			Session->EnumerateMetadata(
				[this](const TraceServices::FTraceSessionMetadata& Metadata)
				{
					if (Metadata.Name == FName(TEXT("BuildVersion"))
						&& Metadata.Type
							== TraceServices::FTraceSessionMetadata::EType::String)
					{
						RecordedBuildVersion = Metadata.StringValue;
					}
				});
		}
		ManagedMarker = ReadManagedEngineMarker(*Session, DurationSeconds);
	}
	ManagedEngineMarker = ManagedMarker.Text;
	const int32 CurrentMajor = FEngineVersion::Current().GetMajor();
	const int32 CurrentMinor = FEngineVersion::Current().GetMinor();
	if (ManagedMarker.bInvalid || ManagedMarker.bConflicting)
	{
		EngineVersionStatus = TEXT("unknown");
		Session.Reset();
		DurationSeconds = 0.0;
		OutErrorCode = TEXT("trace_engine_version_unknown");
		OutErrorMessage = ManagedMarker.bConflicting
			? TEXT("The trace contains conflicting UEAI managed Engine markers.")
			: TEXT("The trace contains a malformed UEAI managed Engine marker.");
		return false;
	}
	int32 RecordedMajor = 0;
	int32 RecordedMinor = 0;
	const bool bRecordedVersionKnown =
		ParseMajorMinor(RecordedBuildVersion, RecordedMajor, RecordedMinor);
	if (bRecordedVersionKnown
		&& (RecordedMajor != CurrentMajor || RecordedMinor != CurrentMinor))
	{
		EngineVersionStatus = TEXT("mismatch");
		Session.Reset();
		DurationSeconds = 0.0;
		OutErrorCode = TEXT("trace_engine_version_mismatch");
		OutErrorMessage = FString::Printf(
			TEXT("Trace BuildVersion '%s' does not match worker Engine %d.%d."),
			*RecordedBuildVersion,
			CurrentMajor,
			CurrentMinor);
		return false;
	}
	if (ManagedMarker.bFound
		&& (ManagedMarker.Major != CurrentMajor
			|| ManagedMarker.Minor != CurrentMinor))
	{
		EngineVersionStatus = TEXT("mismatch");
		Session.Reset();
		DurationSeconds = 0.0;
		OutErrorCode = TEXT("trace_engine_version_mismatch");
		OutErrorMessage = FString::Printf(
			TEXT("Managed Trace marker '%s' does not match worker Engine %d.%d."),
			*ManagedMarker.Text,
			CurrentMajor,
			CurrentMinor);
		return false;
	}
	if (!bRecordedVersionKnown)
	{
		if (ManagedMarker.bFound)
		{
			EngineVersionStatus = TEXT("matchedManagedMarker");
		}
		else if (!bAllowUnknownEngineVersionForTestFixture)
		{
			EngineVersionStatus = TEXT("unknown");
			Session.Reset();
			DurationSeconds = 0.0;
			OutErrorCode = TEXT("trace_engine_version_unknown");
			OutErrorMessage = FString::Printf(
				TEXT("Trace BuildVersion '%s' cannot be matched safely to worker Engine %d.%d."),
				*RecordedBuildVersion,
				CurrentMajor,
				CurrentMinor);
			return false;
		}
		else
		{
			EngineVersionStatus = TEXT("unknownTestFixture");
		}
	}
	else
	{
		EngineVersionStatus = TEXT("matched");
	}
	OpenTracePath = TracePath;
	return true;
}

void FTraceAnalysisSession::Close()
{
	if (Session.IsValid() && !Session->IsAnalysisComplete())
	{
		Session->Stop(true);
	}
	Session.Reset();
	OpenTracePath.Reset();
	DurationSeconds = 0.0;
	RecordedBuildVersion.Reset();
	EngineVersionStatus = TEXT("unknown");
	ManagedEngineMarker.Reset();
}

bool FTraceAnalysisSession::IsOpen() const
{
	return Session.IsValid() && Session->IsAnalysisComplete();
}

const FString& FTraceAnalysisSession::GetTracePath() const
{
	return OpenTracePath;
}

double FTraceAnalysisSession::GetDurationSeconds() const
{
	return DurationSeconds;
}

const FString& FTraceAnalysisSession::GetRecordedBuildVersion() const
{
	return RecordedBuildVersion;
}

const FString& FTraceAnalysisSession::GetManagedEngineMarker() const
{
	return ManagedEngineMarker;
}

const FString& FTraceAnalysisSession::GetEngineVersionStatus() const
{
	return EngineVersionStatus;
}

TArray<FTraceProviderDescriptor> FTraceAnalysisSession::GetProviderDescriptors()
{
	return {
		MakeDescriptor(TEXT("timing"), TEXT("Timing"), TEXT("Timing Insights"),
			{TraceServices::GetTimingProfilerProviderName().ToString(),
			 TraceServices::GetFrameProviderName().ToString(),
			 TraceServices::GetThreadProviderName().ToString(),
			 TraceServices::GetStackSamplesProviderName().ToString(),
			 TraceServices::GetModuleProviderName().ToString()},
			{TEXT("cpu"), TEXT("gpu"), TEXT("frame"),
			 TEXT("stacksampling"), TEXT("module")},
			{TEXT("frames"), TEXT("threads"), TEXT("events"), TEXT("timers"),
			 TEXT("callers"), TEXT("callees"), TEXT("cpuSampling")}, true),
		MakeDescriptor(TEXT("counter"), TEXT("Counters"), TEXT("Timing Insights / Counters"),
			{TraceServices::GetCounterProviderName().ToString()},
			{TEXT("counter")}, {TEXT("list"), TEXT("series"), TEXT("aggregate")}, true),
		MakeDescriptor(TEXT("memory"), TEXT("Memory"), TEXT("Memory Insights"),
			{TraceServices::GetAllocationsProviderName().ToString(),
			 TraceServices::GetMemoryProviderName().ToString(),
			 TraceServices::GetCallstacksProviderName().ToString(),
			 TraceServices::GetModuleProviderName().ToString()},
			{TEXT("memory"), TEXT("memalloc"), TEXT("callstack"), TEXT("module")},
			{TEXT("allocations"), TEXT("liveAllocations"), TEXT("tags"),
			 TEXT("modules"), TEXT("callstacks")}, true),
		MakeDescriptor(TEXT("loading"), TEXT("Asset Loading"), TEXT("Loading Insights"),
			{TraceServices::GetLoadTimeProfilerProviderName().ToString()},
			{TEXT("loadtime")},
			{TEXT("packages"), TEXT("objects"), TEXT("exports"),
			 TEXT("requests"), TEXT("dependencies")}, true),
		MakeDescriptor(TEXT("network"), TEXT("Network"), TEXT("Network Insights"),
			{TraceServices::GetNetProfilerProviderName().ToString()},
			{TEXT("net")}, {TEXT("connections"), TEXT("packets"),
			 TEXT("contentEvents"), TEXT("stats")}, true),
		MakeDescriptor(TEXT("tasks"), TEXT("Tasks"), TEXT("Task Graph Insights"),
			{TraceServices::GetTaskProviderName().ToString()},
			{TEXT("task")}, {TEXT("tasks"), TEXT("relations"),
			 TEXT("waiting"), TEXT("criticalPath")}, true),
		MakeDescriptor(TEXT("contextSwitches"), TEXT("Context Switches"),
			TEXT("Timing Insights / Context Switches"),
			{TraceServices::GetContextSwitchesProviderName().ToString()},
			{TEXT("contextswitch")}, {TEXT("cores"), TEXT("threads"), TEXT("intervals")}, true),
		MakeDescriptor(TEXT("io"), TEXT("File IO"), TEXT("Loading Insights / File Activity"),
			{TraceServices::GetFileActivityProviderName().ToString()},
			{TEXT("file")}, {TEXT("files"), TEXT("events"), TEXT("aggregate")}, true),
		MakeDescriptor(TEXT("log"), TEXT("Log"), TEXT("Log View"),
			{TraceServices::GetLogProviderName().ToString()},
			{TEXT("log")}, {TEXT("categories"), TEXT("messages")}, true),
		MakeDescriptor(TEXT("bookmark"), TEXT("Bookmarks"), TEXT("Timing Insights"),
			{TraceServices::GetBookmarkProviderName().ToString()},
			{TEXT("bookmark")}, {TEXT("list")}, true),
		MakeDescriptor(TEXT("region"), TEXT("Regions"), TEXT("Timing Insights"),
			{TraceServices::GetRegionProviderName().ToString()},
			{TEXT("region")}, {TEXT("list"), TEXT("ranges")}, true),
		MakeDescriptor(TEXT("screenshot"), TEXT("Trace Screenshots"), TEXT("Timing Insights"),
			{TraceServices::GetScreenshotProviderName().ToString()},
			{TEXT("screenshot")}, {TEXT("list"), TEXT("get")}, true),
		MakeDescriptor(TEXT("threads"), TEXT("Threads"), TEXT("Timing Insights"),
			{TraceServices::GetThreadProviderName().ToString()},
			{TEXT("cpu")}, {TEXT("list")}, true),
		MakeDescriptor(TEXT("channels"), TEXT("Channels"), TEXT("Session Info"),
			{TraceServices::GetChannelProviderName().ToString()},
			{TEXT("default")}, {TEXT("list")}, false),
		MakeDescriptor(TEXT("modules"), TEXT("Modules"), TEXT("Memory Insights"),
			{TraceServices::GetModuleProviderName().ToString(),
			 TraceServices::GetStackSamplesProviderName().ToString()},
			{TEXT("module"), TEXT("callstack")}, {TEXT("list")}, false)
	};
}

TArray<FTraceProviderStatus> FTraceAnalysisSession::GetProviderStatuses() const
{
	TArray<FTraceProviderStatus> Result;
	if (!IsOpen())
	{
		return Result;
	}
	TraceServices::FAnalysisSessionReadScope ReadScope(*Session);
	const TraceServices::IChannelProvider* ChannelProvider =
		Session->ReadProvider<TraceServices::IChannelProvider>(
			TraceServices::GetChannelProviderName());
	for (FTraceProviderDescriptor Descriptor : GetProviderDescriptors())
	{
		FTraceProviderStatus& Status = Result.AddDefaulted_GetRef();
		Status.Descriptor = MoveTemp(Descriptor);
		for (const FString& ProviderName : Status.Descriptor.ProviderNames)
		{
			if (Session->ReadProvider<TraceServices::IProvider>(FName(*ProviderName)))
			{
				Status.RecordedProviderNames.Add(ProviderName);
			}
			else
			{
				Status.MissingProviderNames.Add(ProviderName);
			}
		}
		Status.bRecorded = !Status.RecordedProviderNames.IsEmpty();
		if (Status.Descriptor.Id == TEXT("timing"))
		{
			bool bHasTimingEvents = false;
			const TraceServices::ITimingProfilerProvider* Timing =
				Session->ReadProvider<TraceServices::ITimingProfilerProvider>(
					TraceServices::GetTimingProfilerProviderName());
			if (Timing)
			{
				Timing->ReadTimers(
					[&bHasTimingEvents](
						const TraceServices::ITimingProfilerTimerReader& Reader)
					{
						bHasTimingEvents = Reader.GetTimerCount() > 0;
					});
			}
			const TraceServices::IFrameProvider* Frames =
				Session->ReadProvider<TraceServices::IFrameProvider>(
					TraceServices::GetFrameProviderName());
			const bool bHasFrames = Frames
				&& (Frames->GetFrameCount(TraceFrameType_Game) > 0
					|| Frames->GetFrameCount(TraceFrameType_Rendering) > 0);
			const TraceServices::IStackSamplesProvider* Samples =
				Session->ReadProvider<TraceServices::IStackSamplesProvider>(
					TraceServices::GetStackSamplesProviderName());
			const TraceServices::IThreadProvider* Threads =
				Session->ReadProvider<TraceServices::IThreadProvider>(
					TraceServices::GetThreadProviderName());
			bool bHasStackSamples = false;
			bool bHasThreads = false;
			if (Samples && Threads)
			{
				Threads->EnumerateThreads(
					[&](const TraceServices::FThreadInfo& Thread)
					{
						bHasThreads = true;
						bHasStackSamples |= Samples->GetStackSamples(Thread.Id) != nullptr;
					});
			}
			else if (Threads)
			{
				Threads->EnumerateThreads(
					[&bHasThreads](const TraceServices::FThreadInfo&)
					{
						bHasThreads = true;
					});
			}
			Status.bRecorded = bHasTimingEvents || bHasFrames
				|| bHasStackSamples || bHasThreads;
		}
		else if (Status.Descriptor.Id == TEXT("memory"))
		{
			const TraceServices::IAllocationsProvider* Allocations =
				Session->ReadProvider<TraceServices::IAllocationsProvider>(
					TraceServices::GetAllocationsProviderName());
			const TraceServices::IMemoryProvider* Memory =
				Session->ReadProvider<TraceServices::IMemoryProvider>(
					TraceServices::GetMemoryProviderName());
			const TraceServices::IModuleProvider* Modules =
				Session->ReadProvider<TraceServices::IModuleProvider>(
					TraceServices::GetModuleProviderName());
			bool bHasAllocations = false;
			if (Allocations)
			{
				TraceServices::FProviderReadScopeLock ProviderReadScope(*Allocations);
				bHasAllocations = Allocations->IsInitialized();
			}
			Status.bRecorded = bHasAllocations
				|| (Memory && (Memory->GetTagCount() > 0 || Memory->GetTrackerCount() > 0))
				|| (Modules && Modules->GetNumModules() > 0);
		}
		else if (Status.Descriptor.Id == TEXT("loading"))
		{
			const TraceServices::ILoadTimeProfilerProvider* Provider =
				Session->ReadProvider<TraceServices::ILoadTimeProfilerProvider>(
					TraceServices::GetLoadTimeProfilerProviderName());
			Status.bRecorded = Provider && Provider->GetTimelineCount() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("network"))
		{
			const TraceServices::INetProfilerProvider* Provider =
				Session->ReadProvider<TraceServices::INetProfilerProvider>(
					TraceServices::GetNetProfilerProviderName());
			Status.bRecorded = Provider && Provider->GetNetTraceVersion() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("tasks"))
		{
			const TraceServices::ITasksProvider* Provider =
				Session->ReadProvider<TraceServices::ITasksProvider>(
					TraceServices::GetTaskProviderName());
			Status.bRecorded = Provider && Provider->GetNumTasks() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("contextSwitches"))
		{
			const TraceServices::IContextSwitchesProvider* Provider =
				Session->ReadProvider<TraceServices::IContextSwitchesProvider>(
					TraceServices::GetContextSwitchesProviderName());
			Status.bRecorded = Provider && Provider->HasData();
		}
		else if (Status.Descriptor.Id == TEXT("counter"))
		{
			const TraceServices::ICounterProvider* Provider =
				Session->ReadProvider<TraceServices::ICounterProvider>(
					TraceServices::GetCounterProviderName());
			Status.bRecorded = Provider && Provider->GetCounterCount() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("io"))
		{
			const TraceServices::IFileActivityProvider* Provider =
				Session->ReadProvider<TraceServices::IFileActivityProvider>(
					TraceServices::GetFileActivityProviderName());
			Status.bRecorded = Provider
				&& Provider->GetFileActivityTable().GetRowCount() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("log"))
		{
			const TraceServices::ILogProvider* Provider =
				Session->ReadProvider<TraceServices::ILogProvider>(
					TraceServices::GetLogProviderName());
			Status.bRecorded = Provider && Provider->GetMessageCount() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("bookmark"))
		{
			const TraceServices::IBookmarkProvider* Provider =
				Session->ReadProvider<TraceServices::IBookmarkProvider>(
					TraceServices::GetBookmarkProviderName());
			Status.bRecorded = Provider && Provider->GetBookmarkCount() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("region"))
		{
			const TraceServices::IRegionProvider* Provider =
				Session->ReadProvider<TraceServices::IRegionProvider>(
					TraceServices::GetRegionProviderName());
			if (Provider)
			{
				TraceServices::FProviderReadScopeLock ProviderReadScope(*Provider);
				Status.bRecorded = Provider->GetRegionCount() > 0;
			}
			else
			{
				Status.bRecorded = false;
			}
		}
		else if (Status.Descriptor.Id == TEXT("screenshot"))
		{
			const TraceServices::IScreenshotProvider* ScreenshotProvider =
				Session->ReadProvider<TraceServices::IScreenshotProvider>(
					TraceServices::GetScreenshotProviderName());
			const TraceServices::ILogProvider* LogProvider =
				Session->ReadProvider<TraceServices::ILogProvider>(
					TraceServices::GetLogProviderName());
			bool bScreenshotRecorded = false;
			if (ScreenshotProvider && LogProvider)
			{
				// FMiscTraceAnalyzer registers the Screenshot category even when no
				// ScreenshotHeader event was recorded. Inspect the entire timeline:
				// a bounded tail probe produces false negatives for old captures and
				// category presence alone produces false positives. The callback does
				// not retain rows, so memory remains bounded independently of log size.
				LogProvider->EnumerateMessages(
					0.0,
					DurationSeconds,
					[&](const TraceServices::FLogMessageInfo& Message)
					{
						if (bScreenshotRecorded
							|| Message.Line < 0
							|| !Message.Category
							|| !Message.Category->Name
							|| FCString::Stricmp(
								Message.Category->Name,
								TEXT("Screenshot")) != 0)
						{
							return;
						}
						bScreenshotRecorded = ScreenshotProvider->GetScreenshot(
							static_cast<uint32>(Message.Line)).IsValid();
					});
			}
			Status.bRecorded = ScreenshotProvider
				&& bScreenshotRecorded;
		}
		else if (Status.Descriptor.Id == TEXT("threads"))
		{
			const TraceServices::IThreadProvider* Provider =
				Session->ReadProvider<TraceServices::IThreadProvider>(
					TraceServices::GetThreadProviderName());
			Status.bRecorded = Provider && Provider->GetModCount() > 0;
		}
		else if (Status.Descriptor.Id == TEXT("channels"))
		{
			Status.bRecorded = ChannelProvider
				&& !ChannelProvider->GetChannels().IsEmpty();
		}
		else if (Status.Descriptor.Id == TEXT("modules"))
		{
			const TraceServices::IModuleProvider* Provider =
				Session->ReadProvider<TraceServices::IModuleProvider>(
					TraceServices::GetModuleProviderName());
			Status.bRecorded = Provider && Provider->GetNumModules() > 0;
		}

		if (ChannelProvider)
		{
			Status.ChannelStatus = TEXT("finalSnapshot");
			for (const FString& RequiredChannel : Status.Descriptor.RequiredChannels)
			{
				const TraceServices::FChannelEntry* Entry =
					ChannelProvider->GetChannels().FindByPredicate(
						[&RequiredChannel](const TraceServices::FChannelEntry& Candidate)
						{
							return Candidate.Name.Equals(
								RequiredChannel, ESearchCase::IgnoreCase);
						});
				if (Entry && Entry->bIsEnabled)
				{
					Status.RecordedChannels.Add(Entry->Name);
				}
				else
				{
					Status.MissingChannels.Add(
						Entry ? Entry->Name : RequiredChannel);
				}
			}
		}
		if (!Status.bRecorded)
		{
			Status.UnavailableReason = TEXT("No matching provider was recorded in this trace.");
		}
		else if (!Status.Descriptor.bQueryImplemented)
		{
			Status.UnavailableReason =
				TEXT("Provider data is recorded, but its semantic query adapter is not enabled yet.");
		}
		else if (!ChannelProvider)
		{
			Status.ChannelStatus = TEXT("unknown");
			Status.UnavailableReason =
				TEXT("Provider data is queryable, but the trace has no ChannelProvider; required-channel history is unknown.");
		}
		else if (!Status.MissingChannels.IsEmpty())
		{
			Status.UnavailableReason =
				TEXT("Provider data is queryable. Required channels shown as missing reflect only the final channel snapshot and may have been enabled earlier.");
		}
	}
	return Result;
}

bool FTraceAnalysisSession::Query(
	const FTraceQueryRequest& Request,
	FTraceQueryResult& OutResult,
	FString& OutErrorCode,
	FString& OutErrorMessage) const
{
	OutResult = FTraceQueryResult();
	OutResult.Provider = Request.Provider;
	OutResult.Operation = Request.Operation;
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	if (!IsOpen())
	{
		OutErrorCode = TEXT("trace_session_unavailable");
		OutErrorMessage = TEXT("No completed trace analysis session is open.");
		return false;
	}
	if (Request.Filter.Len() > MaximumFilterLength)
	{
		OutErrorCode = TEXT("trace_query_invalid");
		OutErrorMessage = TEXT("The trace query filter exceeds the bounded length.");
		return false;
	}
	const TArray<FTraceProviderStatus> ProviderStatuses = GetProviderStatuses();
	const FTraceProviderStatus* RequestedProvider =
		ProviderStatuses.FindByPredicate(
			[&Request](const FTraceProviderStatus& Status)
			{
				return Status.Descriptor.Id == Request.Provider;
			});
	if (!RequestedProvider || !RequestedProvider->Descriptor.bQueryImplemented)
	{
		OutErrorCode = TEXT("trace_query_unsupported");
		OutErrorMessage = FString::Printf(
			TEXT("Provider '%s' is known but has no enabled semantic adapter."),
			*Request.Provider);
		return false;
	}
	if (!RequestedProvider->bRecorded)
	{
		OutErrorCode = TEXT("trace_provider_unavailable");
		OutErrorMessage = RequestedProvider->UnavailableReason.IsEmpty()
			? FString::Printf(
				TEXT("The trace contains no queryable '%s' provider data."),
				*Request.Provider)
			: RequestedProvider->UnavailableReason;
		return false;
	}

	TraceServices::FAnalysisSessionReadScope ReadScope(*Session);
	FTraceQueryRequest EffectiveRequest = Request;
	FTraceDiagnostic RegionDiagnostic;
	if (!ApplyNamedRegionRange(
		*Session, DurationSeconds, EffectiveRequest, RegionDiagnostic,
		OutErrorCode, OutErrorMessage))
	{
		return false;
	}

	bool bSucceeded = false;
	if (EffectiveRequest.Provider == TEXT("timing")
		&& EffectiveRequest.Operation == TEXT("threads"))
	{
		bSucceeded = QueryThreads(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("timing"))
	{
		bSucceeded = QueryTiming(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("counter"))
	{
		bSucceeded = QueryCounters(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("memory"))
	{
		bSucceeded = QueryMemory(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("loading"))
	{
		bSucceeded = QueryLoading(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("network"))
	{
		bSucceeded = QueryNetwork(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("tasks"))
	{
		bSucceeded = QueryTasks(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("contextSwitches"))
	{
		bSucceeded = QueryContextSwitches(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("io"))
	{
		bSucceeded = QueryFileIo(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("log"))
	{
		bSucceeded = QueryLogs(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("bookmark"))
	{
		bSucceeded = QueryBookmarks(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("region"))
	{
		bSucceeded = QueryRegions(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("screenshot"))
	{
		bSucceeded = QueryScreenshots(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else if (EffectiveRequest.Provider == TEXT("threads"))
	{
		bSucceeded = QueryThreads(
			*Session, EffectiveRequest, DurationSeconds, OutResult,
			OutErrorCode, OutErrorMessage);
	}
	else
	{
		OutErrorCode = TEXT("trace_query_unsupported");
		OutErrorMessage = FString::Printf(
			TEXT("Provider '%s' is known but has no enabled semantic adapter."),
			*EffectiveRequest.Provider);
		return false;
	}

	if (bSucceeded && !RegionDiagnostic.Code.IsEmpty())
	{
		OutResult.Diagnostics.Add(MoveTemp(RegionDiagnostic));
	}
	return bSucceeded;
}
}
