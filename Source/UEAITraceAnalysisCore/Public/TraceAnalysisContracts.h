#pragma once

#include "CoreTypes.h"
#include "Templates/SharedPointer.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"

namespace UEAI::Trace
{
inline constexpr int32 ProtocolVersion = 1;
inline constexpr int32 DefaultPageLimit = 100;
inline constexpr int32 MaximumPageLimit = 1000;
inline constexpr int32 MaximumFilterLength = 512;

enum class ETraceValueType : uint8
{
	Null,
	Boolean,
	Integer,
	Number,
	String
};

/** A JSON-compatible scalar without a dependency on the Json module. */
struct UEAITRACEANALYSISCORE_API FTraceValue
{
	ETraceValueType Type = ETraceValueType::Null;
	bool BooleanValue = false;
	int64 IntegerValue = 0;
	double NumberValue = 0.0;
	FString StringValue;

	static FTraceValue Null();
	static FTraceValue Boolean(bool Value);
	static FTraceValue Integer(int64 Value);
	static FTraceValue Number(double Value);
	static FTraceValue String(FString Value);
};

struct UEAITRACEANALYSISCORE_API FTraceRow
{
	TMap<FString, FTraceValue> Fields;

	FTraceRow& Add(const TCHAR* Name, FTraceValue Value);
};

struct UEAITRACEANALYSISCORE_API FTraceProviderDescriptor
{
	FString Id;
	FString DisplayName;
	FString InsightsPanel;
	TArray<FString> ProviderNames;
	TArray<FString> RequiredChannels;
	TArray<FString> Operations;
	bool bQueryImplemented = false;
};

struct UEAITRACEANALYSISCORE_API FTraceProviderStatus
{
	FTraceProviderDescriptor Descriptor;
	bool bRecorded = false;
	TArray<FString> RecordedProviderNames;
	TArray<FString> MissingProviderNames;
	/** Enabled/disabled state is the final ChannelProvider snapshot, not history. */
	FString ChannelStatus = TEXT("unknown");
	TArray<FString> RecordedChannels;
	TArray<FString> MissingChannels;
	FString UnavailableReason;
};

struct UEAITRACEANALYSISCORE_API FTraceTimeRange
{
	bool bHasStart = false;
	bool bHasEnd = false;
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
};

struct UEAITRACEANALYSISCORE_API FTracePageRequest
{
	uint64 Cursor = 0;
	int32 Limit = DefaultPageLimit;
};

struct UEAITRACEANALYSISCORE_API FTraceQueryRequest
{
	FString Provider;
	FString Operation;
	FString Filter;
	FTraceTimeRange TimeRange;
	FTracePageRequest Page;
	TMap<FString, FString> Options;
};

struct UEAITRACEANALYSISCORE_API FTraceDiagnostic
{
	FString Severity;
	FString Code;
	FString Message;
};

struct UEAITRACEANALYSISCORE_API FTraceQueryResult
{
	FString Schema = TEXT("ue.trace-query-result.v1");
	FString Provider;
	FString Operation;
	double IntervalStartSeconds = 0.0;
	double IntervalEndSeconds = 0.0;
	uint64 TotalRows = 0;
	bool bTruncated = false;
	bool bHasNextCursor = false;
	uint64 NextCursor = 0;
	TArray<FString> Columns;
	TArray<FTraceRow> Rows;
	TArray<FTraceDiagnostic> Diagnostics;
};

struct UEAITRACEANALYSISCORE_API FTraceWorkerHandshake
{
	FString Schema = TEXT("ue.trace-worker-handshake.v1");
	int32 Protocol = ProtocolVersion;
	FString WorkerVersion;
	FString EngineVersion;
	FString ContractDigest;
	FString ProviderSchemaDigest;
	FString Transport = TEXT("stdio-one-shot");
	int32 MaximumResidentSessions = 1;
	TArray<FTraceProviderDescriptor> Providers;
	FString UnrealInsightsPath;
	bool bUnrealInsightsAvailable = false;
};

/** Canonical recording channels shared by Editor and Development capture. */
UEAITRACEANALYSISCORE_API TArray<FString> GetTracePresetChannels(
	const FString& Preset);

/** Bounded semantic queries used by post-stop summary/full analysis. */
UEAITRACEANALYSISCORE_API TArray<FString> GetTracePostStopOperations(
	const FString& Provider,
	bool bFull);
}
