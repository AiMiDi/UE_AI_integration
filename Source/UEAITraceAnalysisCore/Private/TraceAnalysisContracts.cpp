#include "TraceAnalysisContracts.h"
#include "Templates/UnrealTemplate.h"

namespace UEAI::Trace
{
FTraceValue FTraceValue::Null()
{
	return FTraceValue();
}

FTraceValue FTraceValue::Boolean(const bool Value)
{
	FTraceValue Result;
	Result.Type = ETraceValueType::Boolean;
	Result.BooleanValue = Value;
	return Result;
}

FTraceValue FTraceValue::Integer(const int64 Value)
{
	FTraceValue Result;
	Result.Type = ETraceValueType::Integer;
	Result.IntegerValue = Value;
	return Result;
}

FTraceValue FTraceValue::Number(const double Value)
{
	FTraceValue Result;
	Result.Type = ETraceValueType::Number;
	Result.NumberValue = Value;
	return Result;
}

FTraceValue FTraceValue::String(FString Value)
{
	FTraceValue Result;
	Result.Type = ETraceValueType::String;
	Result.StringValue = MoveTemp(Value);
	return Result;
}

FTraceRow& FTraceRow::Add(const TCHAR* Name, FTraceValue Value)
{
	Fields.Add(Name, MoveTemp(Value));
	return *this;
}

TArray<FString> GetTracePresetChannels(const FString& Preset)
{
	static const TMap<FString, TArray<FString>> Presets = {
		{TEXT("standard"), {TEXT("default"), TEXT("cpu"), TEXT("frame"),
			TEXT("bookmark"), TEXT("log"), TEXT("counter"), TEXT("region")}},
		{TEXT("cpu"), {TEXT("default"), TEXT("cpu"), TEXT("frame"),
			TEXT("bookmark"), TEXT("log"), TEXT("counter"), TEXT("region"),
			TEXT("task"), TEXT("contextswitch"), TEXT("module"),
			TEXT("callstack")}},
		{TEXT("gpu"), {TEXT("default"), TEXT("cpu"), TEXT("gpu"),
			TEXT("frame"), TEXT("bookmark"), TEXT("log"), TEXT("counter"),
			TEXT("region")}},
		{TEXT("memory"), {TEXT("default"), TEXT("cpu"), TEXT("frame"),
			TEXT("bookmark"), TEXT("log"), TEXT("memory"), TEXT("memalloc"),
			TEXT("module"), TEXT("callstack"), TEXT("region")}},
		{TEXT("loading"), {TEXT("default"), TEXT("cpu"), TEXT("frame"),
			TEXT("bookmark"), TEXT("log"), TEXT("loadtime"), TEXT("file"),
			TEXT("region")}},
		{TEXT("network"), {TEXT("default"), TEXT("cpu"), TEXT("frame"),
			TEXT("bookmark"), TEXT("log"), TEXT("net"), TEXT("region")}},
		{TEXT("fullInsights"), {TEXT("default"), TEXT("cpu"), TEXT("gpu"),
			TEXT("frame"), TEXT("bookmark"), TEXT("log"), TEXT("counter"),
			TEXT("memory"), TEXT("memalloc"), TEXT("loadtime"), TEXT("file"),
			TEXT("net"), TEXT("task"), TEXT("contextswitch"), TEXT("module"),
			TEXT("callstack"), TEXT("screenshot"), TEXT("region")}}
	};
	const TArray<FString>* Channels = Presets.Find(Preset);
	return Channels ? *Channels : TArray<FString>();
}

TArray<FString> GetTracePostStopOperations(
	const FString& Provider,
	const bool bFull)
{
	static const TMap<FString, TArray<FString>> SummaryOperations = {
		{TEXT("timing"), {TEXT("timers")}},
		{TEXT("counter"), {TEXT("list")}},
		{TEXT("memory"), {TEXT("tags")}},
		{TEXT("loading"), {TEXT("packages")}},
		{TEXT("network"), {TEXT("connections")}},
		{TEXT("tasks"), {TEXT("tasks")}},
		{TEXT("contextSwitches"), {TEXT("cores")}},
		{TEXT("io"), {TEXT("aggregate")}},
		{TEXT("log"), {TEXT("categories")}},
		{TEXT("bookmark"), {TEXT("list")}},
		{TEXT("region"), {TEXT("list")}},
		{TEXT("screenshot"), {TEXT("list")}},
		{TEXT("threads"), {TEXT("list")}}
	};
	static const TMap<FString, TArray<FString>> FullOperations = {
		{TEXT("timing"), {TEXT("timers"), TEXT("frames"), TEXT("events")}},
		{TEXT("counter"), {TEXT("list"), TEXT("aggregate")}},
		{TEXT("memory"), {TEXT("tags"), TEXT("allocations")}},
		{TEXT("loading"), {TEXT("packages"), TEXT("requests")}},
		{TEXT("network"), {TEXT("connections"), TEXT("stats")}},
		{TEXT("tasks"), {TEXT("tasks"), TEXT("criticalPath")}},
		{TEXT("contextSwitches"), {TEXT("cores"), TEXT("intervals")}},
		{TEXT("io"), {TEXT("aggregate"), TEXT("events")}},
		{TEXT("log"), {TEXT("categories"), TEXT("messages")}},
		{TEXT("bookmark"), {TEXT("list")}},
		{TEXT("region"), {TEXT("list"), TEXT("ranges")}},
		{TEXT("screenshot"), {TEXT("list")}},
		{TEXT("threads"), {TEXT("list")}}
	};
	const TArray<FString>* Operations =
		(bFull ? FullOperations : SummaryOperations).Find(Provider);
	return Operations ? *Operations : TArray<FString>();
}
}
