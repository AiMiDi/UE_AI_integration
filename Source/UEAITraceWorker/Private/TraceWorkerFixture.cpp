#include "TraceWorkerFixture.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tasks/Task.h"
#include "Trace/Trace.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogUEAITraceFixture, Log, All);
TRACE_DECLARE_INT_COUNTER(
	UEAITraceFixtureCounter,
	TEXT("UEAI Trace Fixture Counter"));

namespace UEAI::TraceWorker
{
namespace
{
bool PublishReceipt(
	const FString& Destination,
	const TSharedPtr<FJsonObject>& Receipt,
	FString& OutError)
{
	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Receipt.ToSharedRef(), Writer))
	{
		OutError = TEXT("The diagnostic Trace receipt could not be serialized.");
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
		OutError = TEXT("The diagnostic Trace receipt could not be published atomically.");
		return false;
	}
	return true;
}

int64 WaitForDiagnosticTraceFileStart(const FString& TracePath)
{
	// Do not explicitly stop the writer in this one-shot fixture. UE 5.3's
	// Trace writer has close inertia and a second AppExit shutdown can race an
	// explicit Stop, producing a real ERROR_INVALID_HANDLE write failure. The
	// Program's single normal AppPreExit/AppExit path owns finalization. Keep the
	// process alive only until the file header/data is observable so the receipt
	// can be published before returning to main.
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	int64 TraceSize = IFileManager::Get().FileSize(*TracePath);
	while (TraceSize <= 0 && FPlatformTime::Seconds() < Deadline)
	{
		FPlatformProcess::SleepNoStats(0.01f);
		TraceSize = IFileManager::Get().FileSize(*TracePath);
	}
	return TraceSize;
}

}

int32 GenerateDiagnosticTraceFixture(
	const FString& RequestedTracePath,
	const FString& EngineMarkerOverride,
	FString& OutReceiptPath,
	FString& OutError)
{
	OutReceiptPath.Reset();
	OutError.Reset();
	const FString TracePath =
		FPaths::ConvertRelativePathToFull(RequestedTracePath);
	if (!TracePath.EndsWith(TEXT(".utrace"), ESearchCase::IgnoreCase)
		|| !IFileManager::Get().MakeDirectory(*FPaths::GetPath(TracePath), true))
	{
		OutError = TEXT("--generate-fixture requires a writable .utrace path.");
		return 2;
	}
	FString MarkerVersion = EngineMarkerOverride;
	if (MarkerVersion.IsEmpty())
	{
		MarkerVersion = FString::Printf(
			TEXT("%d.%d"),
			FEngineVersion::Current().GetMajor(),
			FEngineVersion::Current().GetMinor());
	}
	const bool bEmitEngineMarker = MarkerVersion != TEXT("none");
	if (bEmitEngineMarker)
	{
		FString Major;
		FString Minor;
		if (!MarkerVersion.Split(TEXT("."), &Major, &Minor)
			|| Major.IsEmpty() || Minor.IsEmpty()
			|| Minor.Contains(TEXT(".")))
		{
			OutError = TEXT("--fixtureEngineMarker must be major.minor or none.");
			return 2;
		}
		for (const TCHAR Character : Major + Minor)
		{
			if (!FChar::IsDigit(Character))
			{
				OutError = TEXT("--fixtureEngineMarker must be major.minor or none.");
				return 2;
			}
		}
	}
	FTraceAuxiliary::FOptions Options;
	Options.bTruncateFile = true;
	Options.bExcludeTail = true;
	const FString Channels =
		TEXT("cpu,counter,bookmark,log,task,memory,memalloc,file,region");
	std::atomic<bool> bTraceConnected(false);
	const FDelegateHandle ConnectionHandle =
		FTraceAuxiliary::OnConnection.AddLambda(
			[&bTraceConnected]
			{
				bTraceConnected.store(true, std::memory_order_release);
			});
	if (!FTraceAuxiliary::Start(
			FTraceAuxiliary::EConnectionType::File,
			*TracePath,
			*Channels,
			&Options))
	{
		FTraceAuxiliary::OnConnection.Remove(ConnectionHandle);
		OutError = TEXT("FTraceAuxiliary could not start the diagnostic Trace.");
		return 1;
	}
	const double ConnectionDeadline = FPlatformTime::Seconds() + 5.0;
	while (!bTraceConnected.load(std::memory_order_acquire)
		&& FPlatformTime::Seconds() < ConnectionDeadline)
	{
		// The no-Engine Program can run without a Trace writer thread. Update
		// pumps the pending connection in that configuration and is otherwise a
		// no-op while the real writer thread owns the pump.
		UE::Trace::Update();
		FPlatformProcess::SleepNoStats(0.01f);
	}
	FTraceAuxiliary::OnConnection.Remove(ConnectionHandle);
	if (!bTraceConnected.load(std::memory_order_acquire))
	{
		OutError = TEXT("Timed out waiting for the diagnostic Trace file connection.");
		return 1;
	}

	if (bEmitEngineMarker)
	{
		TRACE_BOOKMARK(
			TEXT("UEAI_TRACE_ENGINE_VERSION=%s"), *MarkerVersion);
	}
	TRACE_BEGIN_REGION(TEXT("UEAI.Trace.DiagnosticFixture"));
	TRACE_BOOKMARK(TEXT("UEAI_TRACE_FIXTURE_BEGIN"));
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UEAITraceDiagnosticFixtureCpu);
		TRACE_COUNTER_SET(UEAITraceFixtureCounter, 1);
		UE_LOG(
			LogUEAITraceFixture,
			Display,
			TEXT("UEAI Trace diagnostic fixture log event"));

		UE::Tasks::FTask Task = UE::Tasks::Launch(
			TEXT("UEAITraceDiagnosticFixtureTask"),
			[]
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(UEAITraceDiagnosticFixtureTask);
				FPlatformProcess::SleepNoStats(0.002f);
			});
		Task.Wait();

		void* Allocation = FMemory::Malloc(64 * 1024, 16);
		FMemory::Memset(Allocation, 0x5a, 64 * 1024);
		FMemory::Free(Allocation);

		const FString IoPath = TracePath + TEXT(".io-fixture.tmp");
		const FString IoPayload = TEXT("UEAI Trace diagnostic File IO fixture");
		FString Loaded;
		FFileHelper::SaveStringToFile(
			IoPayload,
			*IoPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FFileHelper::LoadFileToString(Loaded, *IoPath);
		IFileManager::Get().Delete(*IoPath, false, true);
		TRACE_COUNTER_SET(UEAITraceFixtureCounter, 2);
	}
	TRACE_BOOKMARK(TEXT("UEAI_TRACE_FIXTURE_END"));
	TRACE_END_REGION(TEXT("UEAI.Trace.DiagnosticFixture"));
	const int64 TraceSize = WaitForDiagnosticTraceFileStart(TracePath);
	TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(
		TEXT("schema"), TEXT("ue.trace-diagnostic-fixture.v1"));
	Receipt->SetBoolField(TEXT("testOnly"), true);
	Receipt->SetStringField(TEXT("tracePath"), TracePath);
	Receipt->SetNumberField(TEXT("traceSizeBytes"), TraceSize);
	Receipt->SetStringField(
		TEXT("finalizationOwner"), TEXT("programAppExit"));
	Receipt->SetBoolField(TEXT("finalizedByProcessShutdown"), true);
	Receipt->SetStringField(
		TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Receipt->SetStringField(
		TEXT("managedEngineMarker"),
		bEmitEngineMarker
			? TEXT("UEAI_TRACE_ENGINE_VERSION=") + MarkerVersion
			: FString());
	Receipt->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
	TArray<TSharedPtr<FJsonValue>> EmittedProviders;
	for (const TCHAR* Provider : {
		TEXT("timing"), TEXT("counter"), TEXT("bookmark"), TEXT("region"),
		TEXT("log"), TEXT("tasks"), TEXT("memory"), TEXT("io")})
	{
		EmittedProviders.Add(MakeShared<FJsonValueString>(Provider));
	}
	Receipt->SetArrayField(TEXT("emittedProviders"), EmittedProviders);
	TArray<TSharedPtr<FJsonValue>> InstrumentationDependent;
	for (const TCHAR* Provider : {TEXT("tasks"), TEXT("memory"), TEXT("io")})
	{
		InstrumentationDependent.Add(MakeShared<FJsonValueString>(Provider));
	}
	Receipt->SetArrayField(
		TEXT("instrumentationDependentProviders"),
		InstrumentationDependent);
	Receipt->SetStringField(
		TEXT("boundary"),
		TEXT("Task, allocation, and file operations were executed, but provider presence must be verified by provider.list for this no-Engine Program build."));
	OutReceiptPath = TracePath + TEXT(".receipt.json");
	if (TraceSize <= 0 || !PublishReceipt(OutReceiptPath, Receipt, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The diagnostic Trace file did not start writing.");
		}
		return 1;
	}
	return 0;
}
}
