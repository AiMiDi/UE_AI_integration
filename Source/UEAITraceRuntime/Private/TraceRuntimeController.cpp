#include "TraceRuntimeController.h"

#if WITH_UEAI_TRACE_RUNTIME

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Net/Core/Trace/NetTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Containers/Ticker.h"
#include "Trace/Trace.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <cstdlib>
#include <sys/stat.h>
#endif

namespace UEAI::TraceRuntime
{
namespace
{
constexpr int64 MaximumDescriptorBytes = 64 * 1024;
constexpr double HeartbeatIntervalSeconds = 1.0;
constexpr double TraceConnectionTimeoutSeconds = 5.0;
constexpr double TraceFinalizeTimeoutSeconds = 10.0;
constexpr int32 MaximumReplayedNetDrivers = 8;
constexpr int32 MaximumReplayedNetConnections = 128;

void EmitManagedEngineMarker()
{
	const FString ManagedEngineVersion = FString::Printf(
		TEXT("%d.%d"),
		FEngineVersion::Current().GetMajor(),
		FEngineVersion::Current().GetMinor());
	TRACE_BOOKMARK(
		TEXT("UEAI_TRACE_ENGINE_VERSION=%s"), *ManagedEngineVersion);
}

bool WaitForTraceWriterClose()
{
	const double Deadline =
		FPlatformTime::Seconds() + TraceFinalizeTimeoutSeconds;
	while (UE::Trace::IsTracing()
		&& FPlatformTime::Seconds() < Deadline)
	{
		// Update closes a pending trace in no-worker-thread builds and is a
		// no-op while the regular Trace writer thread owns the pump.
		UE::Trace::Update();
		FPlatformProcess::SleepNoStats(0.01f);
	}
	return !UE::Trace::IsTracing();
}

const TSet<FString>& AllowedChannels()
{
	static const TSet<FString> Values = {
		TEXT("default"), TEXT("cpu"), TEXT("gpu"), TEXT("frame"),
		TEXT("bookmark"), TEXT("log"), TEXT("counter"), TEXT("memory"),
		TEXT("memalloc"), TEXT("loadtime"), TEXT("file"), TEXT("net"),
		TEXT("task"), TEXT("contextswitch"), TEXT("module"),
		TEXT("callstack"), TEXT("screenshot"), TEXT("region")};
	return Values;
}

bool IsBoundedToken(
	const FString& Value,
	const int32 MinimumLength,
	const int32 MaximumLength)
{
	if (Value.Len() < MinimumLength || Value.Len() > MaximumLength)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!(FChar::IsAlnum(Character)
			|| Character == TEXT('-')
			|| Character == TEXT(':')))
		{
			return false;
		}
	}
	return true;
}

FString CanonicalDirectory(FString Path)
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

bool IsOwnedJobFilePath(
	const FString& Path,
	const FString& FinalJobDirectory,
	const bool bAllowMissing)
{
	FString FullPath = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(FullPath);
	if (!FPaths::IsSamePath(FPaths::GetPath(FullPath), FinalJobDirectory))
	{
		return false;
	}
	if (!IFileManager::Get().FileExists(*FullPath))
	{
		return bAllowMissing;
	}
	FString FinalPath;
	return ResolveExistingFinalPath(FullPath, false, FinalPath)
		&& FPaths::IsSamePath(FPaths::GetPath(FinalPath), FinalJobDirectory);
}

bool HasStartupMemoryTraceArgument()
{
	FString TraceChannels;
	if (!FParse::Value(
			FCommandLine::Get(), TEXT("-trace="), TraceChannels))
	{
		return false;
	}
	TArray<FString> Values;
	TraceChannels.ParseIntoArray(Values, TEXT(","), true);
	return Values.ContainsByPredicate(
		[](const FString& Value)
		{
			return Value.Equals(TEXT("memory"), ESearchCase::IgnoreCase)
				|| Value.Equals(TEXT("memalloc"), ESearchCase::IgnoreCase);
		});
}
}

FController::FController() = default;

FController::~FController()
{
	Stop();
}

bool FController::StartFromCommandLine()
{
	FString DescriptorPath;
	if (!FParse::Value(
			FCommandLine::Get(), TEXT("UEAITraceJob="), DescriptorPath))
	{
		return false;
	}
	FString ErrorCode;
	FString ErrorMessage;
	if (!LoadAndValidateDescriptor(
			DescriptorPath, ErrorCode, ErrorMessage))
	{
		return false;
	}
	Phase = EPhase::Loading;
	LaunchSeconds = FPlatformTime::Seconds();
	LastHeartbeatSeconds = LaunchSeconds;
	EngineLoopInitCompleteHandle =
		FCoreDelegates::OnFEngineLoopInitComplete.AddRaw(
			this,
			&FController::HandleEngineLoopInitComplete);
	if (!WriteReceiptAtomic(
			TEXT("loading"), TEXT("running"), FString(), FString(), false,
			ReceiptPath)
		|| !WriteReceiptAtomic(
			TEXT("loading"), TEXT("running"), FString(), FString(), false,
			HeartbeatPath))
	{
		Phase = EPhase::Complete;
		return false;
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FController::Tick));
	return true;
}

void FController::Stop()
{
	RemoveTraceConnectionDelegate();
	if (EngineLoopInitCompleteHandle.IsValid())
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(
			EngineLoopInitCompleteHandle);
		EngineLoopInitCompleteHandle.Reset();
	}
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (Phase != EPhase::Complete && Phase != EPhase::Inactive)
	{
		Finish(
			TEXT("failed"),
			TEXT("trace_runtime_interrupted"),
			TEXT("The Development process exited before managed Trace completion."),
			true);
	}
}

bool FController::LoadAndValidateDescriptor(
	const FString& DescriptorPath,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutErrorCode.Reset();
	OutErrorMessage.Reset();
	const FString Root = CanonicalDirectory(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("UE_AI_integration/TraceLaunch")));
	const FString FullDescriptor =
		FPaths::ConvertRelativePathToFull(DescriptorPath);
	FString FinalRoot;
	FString FinalDescriptor;
	if (!FPaths::IsUnderDirectory(FullDescriptor, Root)
		|| !ResolveExistingFinalPath(Root, true, FinalRoot)
		|| !FPaths::IsSamePath(FinalRoot, Root)
		|| !ResolveExistingFinalPath(
			FullDescriptor, false, FinalDescriptor)
		|| !IsWithinDirectory(FinalDescriptor, FinalRoot)
		|| IFileManager::Get().FileSize(*FinalDescriptor) <= 0
		|| IFileManager::Get().FileSize(*FinalDescriptor) > MaximumDescriptorBytes)
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT(
			"The Trace job descriptor must be a bounded file under the project TraceLaunch root.");
		return false;
	}
	FString Serialized;
	if (!FFileHelper::LoadFileToString(Serialized, *FinalDescriptor))
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_unavailable");
		OutErrorMessage = TEXT("The Trace job descriptor could not be read.");
		return false;
	}
	TSharedPtr<FJsonObject> Descriptor;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
	if (!FJsonSerializer::Deserialize(Reader, Descriptor) || !Descriptor.IsValid())
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT("The Trace job descriptor is not valid JSON.");
		return false;
	}
	const TSet<FString> AllowedFields = {
		TEXT("schema"), TEXT("jobId"), TEXT("requestId"),
		TEXT("launchPlanDigest"), TEXT("launchNonce"), TEXT("channels"),
		TEXT("maxDurationSeconds"), TEXT("maxFileSizeMiB"),
		TEXT("startupTimeoutSeconds"), TEXT("stopNonce"), TEXT("exitOnStop")};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Descriptor->Values)
	{
		if (!AllowedFields.Contains(Pair.Key))
		{
			OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
			OutErrorMessage = FString::Printf(
				TEXT("Unsupported Trace descriptor field '%s'."), *Pair.Key);
			return false;
		}
	}
	FString DescriptorSchema;
	if (!Descriptor->TryGetStringField(TEXT("schema"), DescriptorSchema)
		|| !Descriptor->TryGetStringField(TEXT("jobId"), JobId)
		|| !JobId.StartsWith(TEXT("trace-launch-local-"))
		|| !IsBoundedToken(JobId, 20, 128)
		|| DescriptorSchema != TEXT("ue.trace-runtime-job.v1"))
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT("The Trace descriptor schema or jobId is invalid.");
		return false;
	}
	Descriptor->TryGetStringField(TEXT("requestId"), RequestId);
	Descriptor->TryGetStringField(TEXT("launchPlanDigest"), LaunchPlanDigest);
	if (!Descriptor->TryGetStringField(TEXT("launchNonce"), LaunchNonce)
		|| !IsBoundedToken(LaunchNonce, 32, 128))
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT("The Trace descriptor requires a bounded launch nonce.");
		return false;
	}
	if (!Descriptor->TryGetStringField(TEXT("stopNonce"), StopNonce)
		|| !IsBoundedToken(StopNonce, 32, 128))
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT("The Trace descriptor requires a bounded stop nonce.");
		return false;
	}
	JobDirectory = CanonicalDirectory(FPaths::GetPath(FinalDescriptor));
	FString FinalJobDirectory;
	if (!ResolveExistingFinalPath(JobDirectory, true, FinalJobDirectory)
		|| !IsWithinDirectory(FinalJobDirectory, FinalRoot)
		|| !FPaths::IsSamePath(FPaths::GetPath(FinalJobDirectory), FinalRoot)
		|| !FPaths::IsSamePath(
			FPaths::GetPath(FinalDescriptor), FinalJobDirectory)
		|| FPaths::GetCleanFilename(FinalJobDirectory) != JobId)
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT(
			"The Trace descriptor final path must remain in its owned project TraceLaunch job directory.");
		return false;
	}
	JobDirectory = MoveTemp(FinalJobDirectory);

	const TArray<TSharedPtr<FJsonValue>>* ChannelValues = nullptr;
	TArray<FString> Channels;
	if (!Descriptor->TryGetArrayField(TEXT("channels"), ChannelValues)
		|| !ChannelValues || ChannelValues->IsEmpty() || ChannelValues->Num() > 32)
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT("The Trace descriptor requires 1-32 channels.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *ChannelValues)
	{
		FString Channel;
		if (!Value.IsValid()
			|| !Value->TryGetString(Channel)
			|| !AllowedChannels().Contains(Channel)
			|| Channels.Contains(Channel))
		{
			OutErrorCode = TEXT("trace_runtime_channel_invalid");
			OutErrorMessage = TEXT("The Trace descriptor contains an unsupported channel.");
			return false;
		}
		Channels.Add(Channel);
	}
	if ((Channels.Contains(TEXT("memory"))
			|| Channels.Contains(TEXT("memalloc")))
		&& !HasStartupMemoryTraceArgument())
	{
		// UE installs its allocation-tracing FMalloc wrapper before normal
		// module startup by inspecting -trace=memory/memalloc. Enabling the
		// channel later cannot retrofit the allocator safely or produce a
		// truthful Memory provider, so fail instead of silently degrading.
		OutErrorCode = TEXT("trace_runtime_startup_channel_required");
		OutErrorMessage = TEXT(
			"Memory allocation tracing requires the approved -trace=memory process startup argument.");
		return false;
	}
	ChannelList = FString::Join(Channels, TEXT(","));
	bNetTraceRequested = Channels.Contains(TEXT("net"));
	double Number = 120.0;
	Descriptor->TryGetNumberField(TEXT("maxDurationSeconds"), Number);
	MaxDurationSeconds = FMath::Clamp(Number, 1.0, 3600.0);
	Number = 180.0;
	Descriptor->TryGetNumberField(TEXT("startupTimeoutSeconds"), Number);
	StartupTimeoutSeconds = FMath::Clamp(Number, 30.0, 1800.0);
	Number = 4096.0;
	Descriptor->TryGetNumberField(TEXT("maxFileSizeMiB"), Number);
	Number = FMath::Clamp(Number, 1.0, 32768.0);
	MaxFileSizeBytes = static_cast<int64>(Number * 1024.0 * 1024.0);
	Descriptor->TryGetBoolField(TEXT("exitOnStop"), bExitOnStop);

	TracePath = FPaths::Combine(JobDirectory, TEXT("trace.utrace"));
	ReceiptPath = FPaths::Combine(JobDirectory, TEXT("receipt.json"));
	HeartbeatPath = FPaths::Combine(JobDirectory, TEXT("heartbeat.json"));
	StopRequestPath = FPaths::Combine(JobDirectory, TEXT("stop.json"));
	if (!IsOwnedJobFilePath(TracePath, JobDirectory, true)
		|| !IsOwnedJobFilePath(ReceiptPath, JobDirectory, true)
		|| !IsOwnedJobFilePath(HeartbeatPath, JobDirectory, true)
		|| !IsOwnedJobFilePath(StopRequestPath, JobDirectory, true))
	{
		OutErrorCode = TEXT("trace_runtime_descriptor_invalid");
		OutErrorMessage = TEXT(
			"Trace runtime output paths must remain owned files in the validated job directory.");
		return false;
	}
	RegionName = TEXT("UEAI.Trace.") + JobId;
	return true;
}

void FController::HandleEngineLoopInitComplete()
{
	bEngineLoopInitComplete = true;
	if (EngineLoopInitCompleteHandle.IsValid())
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(
			EngineLoopInitCompleteHandle);
		EngineLoopInitCompleteHandle.Reset();
	}
}

bool FController::HasStartedGameWorld() const
{
	if (!GEngine)
	{
		return false;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game
			&& Context.World()
			&& Context.World()->HasBegunPlay())
		{
			return true;
		}
	}
	return false;
}

bool FController::BeginRecording()
{
	if (!IsOwnedJobFilePath(TracePath, JobDirectory, true))
	{
		Finish(
			TEXT("failed"),
			TEXT("trace_runtime_output_path_invalid"),
			TEXT("The managed Trace output path escaped its validated job directory."),
			true);
		return false;
	}
	FTraceAuxiliary::FOptions Options;
	Options.bTruncateFile = true;
	Options.bExcludeTail = true;
	bTraceConnected.store(false, std::memory_order_release);
	TraceConnectionHandle = FTraceAuxiliary::OnConnection.AddRaw(
		this,
		&FController::HandleTraceConnected);
	if (!FTraceAuxiliary::Start(
			FTraceAuxiliary::EConnectionType::File,
			*TracePath,
			*ChannelList,
			&Options))
	{
		RemoveTraceConnectionDelegate();
		Finish(
			TEXT("failed"),
			TEXT("trace_runtime_start_failed"),
			TEXT("FTraceAuxiliary could not start the managed file Trace."));
		return false;
	}
	bTraceStarted = true;
	TraceFinalizationStatus = TEXT("pending");
	TraceConnectionStartedSeconds = FPlatformTime::Seconds();
	Phase = EPhase::Connecting;
	return true;
}

bool FController::ActivateRecording()
{
	RemoveTraceConnectionDelegate();
	StartedSeconds = FPlatformTime::Seconds();
	LastHeartbeatSeconds = StartedSeconds;
	StartedAtUtc = FDateTime::UtcNow().ToIso8601();
	if (bNetTraceRequested)
	{
		// -NetTrace is parsed during UEngine initialization, before this
		// managed file connection exists. Its one-time Version event would be
		// lost, leaving real packet events unparseable by NetProfilerProvider.
		// Re-arm Net Trace only after OnConnection so the init event and all
		// subsequent loopback traffic belong to the same .utrace.
		PreviousNetTraceVerbosity = FNetTrace::GetTraceVerbosity();
		const uint32 RequestedVerbosity = FMath::Max<uint32>(
			1U,
			PreviousNetTraceVerbosity);
		FNetTrace::SetTraceVerbosity(0U);
		FNetTrace::SetTraceVerbosity(RequestedVerbosity);
		bNetTraceActivated = FNetTrace::IsEnabled();
		if (!bNetTraceActivated)
		{
			Finish(
				TEXT("failed"),
				TEXT("trace_runtime_net_trace_unavailable"),
				TEXT("Net Trace could not be armed after the managed Trace connection."),
				true);
			return false;
		}
		PublishNetworkMetadata(true);
	}
	EmitManagedEngineMarker();
	TRACE_BOOKMARK(TEXT("UEAI_TRACE_BEGIN job=%s"), *JobId);
	TRACE_BEGIN_REGION(*RegionName);
	Phase = EPhase::Recording;
	if (!WriteReceiptAtomic(
			TEXT("recording"), TEXT("running"), FString(), FString(), false,
			ReceiptPath))
	{
		Finish(
			TEXT("failed"),
			TEXT("trace_runtime_receipt_write_failed"),
			TEXT("The Development runtime could not publish its recording receipt."),
			true);
		return false;
	}
	return true;
}

void FController::PublishNetworkMetadata(const bool bReplayMissingHistory)
{
#if UE_NET_TRACE_ENABLED
	if (!GEngine || !FNetTrace::IsEnabled())
	{
		return;
	}
	TSet<UNetDriver*> SeenDrivers;
	int32 PublishedDriverCount = 0;
	int32 PublishedConnectionCount = 0;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (!World
			|| (Context.WorldType != EWorldType::Game
				&& Context.WorldType != EWorldType::GamePreview
				&& Context.WorldType != EWorldType::PIE))
		{
			continue;
		}
		for (const FNamedNetDriver& NamedDriver : Context.ActiveNetDrivers)
		{
			UNetDriver* Driver = NamedDriver.NetDriver.Get();
			if (!Driver || SeenDrivers.Contains(Driver))
			{
				continue;
			}
			if (PublishedDriverCount >= MaximumReplayedNetDrivers)
			{
				if (bReplayMissingHistory)
				{
					bNetworkReplayTruncated = true;
				}
				continue;
			}
			SeenDrivers.Add(Driver);
			++PublishedDriverCount;
			const uint32 GameInstanceId = Driver->GetNetTraceId();
			if (GameInstanceId == NetTraceInvalidGameInstanceId)
			{
				continue;
			}
			if (bReplayMissingHistory)
			{
				bNetworkSessionPredatesTrace = true;
				++ReplayedNetDriverCount;
			}
			const FString InstanceName = FString::Printf(
				TEXT("%s (%s)"),
				*GetNameSafe(World->GetGameInstance()),
				LexToString(World->WorldType.GetValue()));
			FNetTrace::TraceInstanceUpdated(
				GameInstanceId,
				Driver->IsServer(),
				*InstanceName);
			auto PublishConnection =
				[this,
					GameInstanceId,
					bReplayMissingHistory,
					&PublishedConnectionCount](UNetConnection* Connection)
				{
					if (!Connection)
					{
						return;
					}
					if (PublishedConnectionCount
						>= MaximumReplayedNetConnections)
					{
						if (bReplayMissingHistory)
						{
							bNetworkReplayTruncated = true;
						}
						return;
					}
					++PublishedConnectionCount;
					const uint32 ConnectionId = Connection->GetConnectionId();
					if (bReplayMissingHistory)
					{
						++ReplayedNetConnectionCount;
						FNetTrace::TraceConnectionCreated(
							GameInstanceId,
							ConnectionId);
						FNetTrace::TraceConnectionStateUpdated(
							GameInstanceId,
							ConnectionId,
							static_cast<uint8>(Connection->GetConnectionState()));
					}
					const FString Address =
						Connection->LowLevelGetRemoteAddress(true);
					const FString OwningActor =
						GetNameSafe(Connection->OwningActor.Get());
					FNetTrace::TraceConnectionUpdated(
						GameInstanceId,
						ConnectionId,
						*Address,
						*OwningActor);
				};
			PublishConnection(Driver->ServerConnection.Get());
			for (const TObjectPtr<UNetConnection>& Connection
				: Driver->ClientConnections)
			{
				PublishConnection(Connection.Get());
			}
		}
	}
#endif
}

void FController::HandleTraceConnected()
{
	// FTraceAuxiliary may invoke this on its writer thread. Never touch UE
	// objects, delegates, files, or controller state other than the atomic flag.
	bTraceConnected.store(true, std::memory_order_release);
}

void FController::RemoveTraceConnectionDelegate()
{
	if (TraceConnectionHandle.IsValid())
	{
		FTraceAuxiliary::OnConnection.Remove(TraceConnectionHandle);
		TraceConnectionHandle.Reset();
	}
}

bool FController::ReadStopRequest(FString& OutError) const
{
	OutError.Reset();
	if (!IFileManager::Get().FileExists(*StopRequestPath))
	{
		return false;
	}
	if (!IsOwnedJobFilePath(StopRequestPath, JobDirectory, false))
	{
		OutError = TEXT(
			"The Trace stop request escaped its validated job directory.");
		return false;
	}
	FString Serialized;
	TSharedPtr<FJsonObject> Request;
	FString Schema;
	FString StopJobId;
	FString Nonce;
	if (!FFileHelper::LoadFileToString(Serialized, *StopRequestPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Serialized), Request)
		|| !Request.IsValid()
		|| !Request->TryGetStringField(TEXT("schema"), Schema)
		|| !Request->TryGetStringField(TEXT("jobId"), StopJobId)
		|| !Request->TryGetStringField(TEXT("nonce"), Nonce)
		|| Schema != TEXT("ue.trace-runtime-stop.v1")
		|| StopJobId != JobId
		|| Nonce != StopNonce)
	{
		OutError = TEXT("The Trace stop request failed its schema, jobId, or nonce check.");
		return false;
	}
	return true;
}

bool FController::Tick(const float)
{
	const double Now = FPlatformTime::Seconds();
	if (Phase == EPhase::Loading || Phase == EPhase::Connecting)
	{
		if (Now - LastHeartbeatSeconds >= HeartbeatIntervalSeconds)
		{
			if (!WriteReceiptAtomic(
					TEXT("loading"), TEXT("running"), FString(), FString(), false,
					HeartbeatPath))
			{
				Finish(
					TEXT("failed"),
					TEXT("trace_runtime_heartbeat_write_failed"),
					TEXT("The Development runtime could not publish its loading heartbeat."),
					true);
				return false;
			}
			LastHeartbeatSeconds = Now;
		}
		// A Game World can begin play while FEngineLoop::Init is still running.
		// Starting Trace from that initialization-time CoreTicker callback can
		// race RenderThread startup. Only begin from a later ticker after the
		// explicit engine-loop completion signal and World BeginPlay.
		if (Phase == EPhase::Loading
			&& bEngineLoopInitComplete
			&& HasStartedGameWorld())
		{
			return BeginRecording();
		}
		if (Phase == EPhase::Connecting)
		{
			// This is required by no-writer-thread builds and is harmless when the
			// regular writer thread owns the connection pump.
			UE::Trace::Update();
			if (bTraceConnected.load(std::memory_order_acquire))
			{
				return ActivateRecording();
			}
			if (Now - TraceConnectionStartedSeconds
				>= TraceConnectionTimeoutSeconds)
			{
				RemoveTraceConnectionDelegate();
				Finish(
					TEXT("failed"),
					TEXT("trace_runtime_connection_timeout"),
					TEXT("The managed Trace file connection was not established before the bounded timeout."),
					true);
				return false;
			}
		}
		if (Now - LaunchSeconds >= StartupTimeoutSeconds)
		{
			Finish(
				TEXT("failed"),
				TEXT("trace_runtime_startup_timeout"),
				TEXT("Engine initialization and Game World BeginPlay did not both complete before startupTimeoutSeconds."),
				true);
			return false;
		}
		return true;
	}
	if (Phase != EPhase::Recording)
	{
		return false;
	}
	if (Now - LastHeartbeatSeconds >= HeartbeatIntervalSeconds)
	{
		// UE 5.3 can create a connection before its address and owning actor are
		// available. Re-publish only the bounded metadata update after recording
		// starts; native packet/content events remain the source of traffic data.
		// This is not history replay and therefore does not make the result partial.
		if (bNetTraceActivated)
		{
			PublishNetworkMetadata(false);
		}
		if (!WriteReceiptAtomic(
				TEXT("recording"), TEXT("running"), FString(), FString(), false,
				HeartbeatPath))
		{
			Finish(
				TEXT("failed"),
				TEXT("trace_runtime_heartbeat_write_failed"),
				TEXT("The Development runtime could not publish its recording heartbeat."),
				true);
			return false;
		}
		LastHeartbeatSeconds = Now;
	}
	FString StopError;
	if (ReadStopRequest(StopError))
	{
		Finish(TEXT("succeeded"));
		return false;
	}
	if (!StopError.IsEmpty())
	{
		Finish(TEXT("failed"), TEXT("trace_stop_request_invalid"), StopError, true);
		return false;
	}
	if (Now - StartedSeconds >= MaxDurationSeconds)
	{
		Finish(TEXT("succeeded"));
		return false;
	}
	const int64 TraceSize = IFileManager::Get().FileSize(*TracePath);
	if (TraceSize > MaxFileSizeBytes)
	{
		Finish(
			TEXT("failed"),
			TEXT("trace_file_size_exceeded"),
			TEXT("The managed Trace exceeded maxFileSizeMiB."),
			true);
		return false;
	}
	return true;
}

void FController::Finish(
	const FString& Status,
	const FString& ErrorCode,
	const FString& Message,
	const bool bPartial)
{
	if (Phase == EPhase::Complete || Phase == EPhase::Inactive)
	{
		return;
	}
	FString FinalStatus = Status;
	FString FinalErrorCode = ErrorCode;
	FString FinalMessage = Message;
	bool bFinalPartial = bPartial || bNetworkSessionPredatesTrace;
	if (bTraceStarted)
	{
		WriteReceiptAtomic(
			TEXT("finalizing"), TEXT("running"), ErrorCode, Message,
			bPartial || bNetworkSessionPredatesTrace,
			ReceiptPath);
		if (Phase == EPhase::Recording)
		{
			TRACE_END_REGION(*RegionName);
			TRACE_BOOKMARK(
				TEXT("UEAI_TRACE_END job=%s status=%s"), *JobId, *Status);
		}
		if (bNetTraceActivated)
		{
			FNetTrace::SetTraceVerbosity(0U);
			bNetTraceActivated = false;
		}
		const bool bStopAccepted = FTraceAuxiliary::Stop();
		const bool bWriterClosed = bStopAccepted && WaitForTraceWriterClose();
		if (bNetTraceRequested && PreviousNetTraceVerbosity > 0U)
		{
			FNetTrace::SetTraceVerbosity(PreviousNetTraceVerbosity);
		}
		TraceFinalizationStatus = bWriterClosed
			? TEXT("completed")
			: (bStopAccepted ? TEXT("timedOut") : TEXT("stopFailed"));
		if (!bWriterClosed)
		{
			FinalStatus = TEXT("failed");
			FinalErrorCode = bStopAccepted
				? TEXT("trace_runtime_finalize_timeout")
				: TEXT("trace_runtime_stop_failed");
			const FString FinalizeMessage = bStopAccepted
				? TEXT("The Trace writer did not flush and close before the bounded finalization deadline.")
				: TEXT("FTraceAuxiliary rejected the managed Trace stop request.");
			FinalMessage = Message.IsEmpty()
				? FinalizeMessage
				: Message + TEXT(" ") + FinalizeMessage;
			bFinalPartial = true;
		}
		bTraceStarted = false;
	}
	Phase = EPhase::Complete;
	WriteReceiptAtomic(
		TEXT("completed"), FinalStatus, FinalErrorCode, FinalMessage,
		bFinalPartial, ReceiptPath);
	if (bExitOnStop)
	{
		FPlatformMisc::RequestExit(false);
	}
}

bool FController::WriteReceiptAtomic(
	const FString& ReceiptPhase,
	const FString& Status,
	const FString& ErrorCode,
	const FString& Message,
	const bool bPartial,
	const FString& Destination) const
{
	if (!IsOwnedJobFilePath(Destination, JobDirectory, true))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("schema"), TEXT("ue.trace-runtime-receipt.v1"));
	Receipt->SetStringField(TEXT("traceJobId"), JobId);
	Receipt->SetStringField(TEXT("requestId"), RequestId);
	Receipt->SetStringField(TEXT("launchPlanDigest"), LaunchPlanDigest);
	Receipt->SetStringField(TEXT("launchNonce"), LaunchNonce);
	Receipt->SetStringField(TEXT("phase"), ReceiptPhase);
	Receipt->SetStringField(TEXT("status"), Status);
	Receipt->SetStringField(TEXT("errorCode"), ErrorCode);
	Receipt->SetStringField(TEXT("message"), Message);
	Receipt->SetStringField(TEXT("tracePath"), TracePath);
	Receipt->SetStringField(TEXT("channels"), ChannelList);
	Receipt->SetStringField(TEXT("startedAtUtc"), StartedAtUtc);
	Receipt->SetStringField(TEXT("updatedAtUtc"), FDateTime::UtcNow().ToIso8601());
	Receipt->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Receipt->SetStringField(TEXT("buildVersion"), FApp::GetBuildVersion());
	Receipt->SetNumberField(
		TEXT("elapsedSeconds"),
		StartedSeconds > 0.0
			? FMath::Max(0.0, FPlatformTime::Seconds() - StartedSeconds)
			: 0.0);
	Receipt->SetNumberField(
		TEXT("traceSizeBytes"),
		FMath::Max<int64>(0, IFileManager::Get().FileSize(*TracePath)));
	const bool bEffectivePartial =
		bPartial || bNetworkSessionPredatesTrace;
	Receipt->SetBoolField(TEXT("partial"), bEffectivePartial);
	Receipt->SetStringField(
		TEXT("networkCompleteness"),
		bNetworkSessionPredatesTrace
			? TEXT("partialPreexistingSession")
			: TEXT("complete"));
	Receipt->SetNumberField(
		TEXT("replayedNetDriverCount"),
		ReplayedNetDriverCount);
	Receipt->SetNumberField(
		TEXT("replayedNetConnectionCount"),
		ReplayedNetConnectionCount);
	Receipt->SetBoolField(
		TEXT("networkReplayTruncated"),
		bNetworkReplayTruncated);
	TArray<TSharedPtr<FJsonValue>> Warnings;
	if (bNetworkSessionPredatesTrace)
	{
		TSharedPtr<FJsonObject> Warning = MakeShared<FJsonObject>();
		Warning->SetStringField(
			TEXT("code"),
			TEXT("network_session_predates_trace"));
		Warning->SetStringField(
			TEXT("message"),
			TEXT("Basic network instance and connection metadata was replayed after Trace connected, but object/name history from before recording is incomplete."));
		Warnings.Add(MakeShared<FJsonValueObject>(Warning));
	}
	Receipt->SetArrayField(TEXT("warnings"), Warnings);
	Receipt->SetStringField(
		TEXT("traceFinalizationStatus"), TraceFinalizationStatus);

	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Receipt.ToSharedRef(), Writer))
	{
		return false;
	}
	const FString Temporary = Destination + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(
			Serialized,
			*Temporary,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(
			*Destination, *Temporary, true, true, false, true)
		|| !IsOwnedJobFilePath(Destination, JobDirectory, false))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		return false;
	}
	return true;
}
}

#endif
