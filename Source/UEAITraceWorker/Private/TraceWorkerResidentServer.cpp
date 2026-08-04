#include "TraceWorkerResidentServer.h"
#include "TraceWorkerCommandLine.h"

#include "TraceWorkerProtocol.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceServices/ITraceServicesModule.h"

#include <atomic>
#include <cstdio>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include <sddl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace UEAI::TraceWorker
{
namespace
{
constexpr uint32 MaximumFrameBytes = 4u * 1024u * 1024u;
FCriticalSection ProtocolExecutionMutex;

void WriteError(const FString& Message)
{
	const FTCHARToUTF8 Utf8(*Message);
	std::fwrite(Utf8.Get(), 1, Utf8.Length(), stderr);
	std::fwrite("\n", 1, 1, stderr);
	std::fflush(stderr);
}

TSharedPtr<FJsonObject> MakeProtocolError(
	const FString& Code,
	const FString& Message)
{
	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("schema"), TEXT("ue.trace-worker-response.v1"));
	Response->SetBoolField(TEXT("ok"), false);
	Response->SetObjectField(TEXT("error"), Error);
	return Response;
}

TArray<uint8> HandlePayload(
	const TArray<uint8>& Payload,
	const FString& CommandLine)
{
	TSharedPtr<FJsonObject> Response;
	if (Payload.IsEmpty() || Payload.Num() > MaximumFrameBytes)
	{
		Response = MakeProtocolError(
			TEXT("trace_worker_frame_invalid"),
			TEXT("The resident request frame is empty or exceeds four MiB."));
	}
	else
	{
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Payload.GetData()), Payload.Num());
		const FString Input(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Request;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Input);
		if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
		{
			Response = MakeProtocolError(
				TEXT("trace_worker_request_invalid"),
				TEXT("The resident request is not a valid UTF-8 JSON object."));
		}
		else
		{
			// TraceServices supports independent analysis sessions, but loading UE
			// modules and driving our per-request adapter concurrently from arbitrary
			// connection threads is not a supported contract. The resident server
			// still accepts two transport connections; semantic work is serialized
			// after TraceServices has been initialized on the Program main thread.
			FScopeLock ProtocolLock(&ProtocolExecutionMutex);
			Response = FProtocol::HandleRequest(Request, CommandLine);
		}
	}
	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
			&Serialized);
	FJsonSerializer::Serialize(Response.ToSharedRef(), Writer);
	if (FTCHARToUTF8(*Serialized).Length()
		> static_cast<int32>(MaximumFrameBytes))
	{
		Response = MakeProtocolError(
			TEXT("trace_worker_response_too_large"),
			TEXT("The bounded resident response exceeds four MiB; reduce the query limit or export an artifact."));
		Serialized.Reset();
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			ErrorWriter =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
					&Serialized);
		FJsonSerializer::Serialize(Response.ToSharedRef(), ErrorWriter);
	}
	const FTCHARToUTF8 Utf8(*Serialized);
	TArray<uint8> Bytes;
	Bytes.Append(
		reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

void EncodeLength(const uint32 Length, uint8 (&Bytes)[4])
{
	Bytes[0] = static_cast<uint8>(Length & 0xffu);
	Bytes[1] = static_cast<uint8>((Length >> 8u) & 0xffu);
	Bytes[2] = static_cast<uint8>((Length >> 16u) & 0xffu);
	Bytes[3] = static_cast<uint8>((Length >> 24u) & 0xffu);
}

uint32 DecodeLength(const uint8 (&Bytes)[4])
{
	return static_cast<uint32>(Bytes[0])
		| (static_cast<uint32>(Bytes[1]) << 8u)
		| (static_cast<uint32>(Bytes[2]) << 16u)
		| (static_cast<uint32>(Bytes[3]) << 24u);
}

void ReapCompleted(TArray<TFuture<void>>& Tasks)
{
	for (int32 Index = Tasks.Num() - 1; Index >= 0; --Index)
	{
		if (Tasks[Index].IsReady())
		{
			Tasks[Index].Get();
			Tasks.RemoveAtSwap(Index, 1, false);
		}
	}
}

#if PLATFORM_WINDOWS
bool WaitForPipeRetry(const double DeadlineSeconds)
{
	if (FPlatformTime::Seconds() >= DeadlineSeconds)
	{
		return false;
	}
	FPlatformProcess::SleepNoStats(0.005f);
	return FPlatformTime::Seconds() < DeadlineSeconds;
}

bool ReadExact(
	const HANDLE Pipe,
	uint8* Destination,
	const uint32 Size,
	const double DeadlineSeconds)
{
	uint32 Offset = 0;
	while (Offset < Size)
	{
		DWORD Read = 0;
		if (ReadFile(Pipe, Destination + Offset, Size - Offset, &Read, nullptr))
		{
			if (Read > 0)
			{
				Offset += Read;
				continue;
			}
			if (!WaitForPipeRetry(DeadlineSeconds))
			{
				return false;
			}
			continue;
		}
		const DWORD Error = GetLastError();
		if (Error != ERROR_NO_DATA
			&& Error != ERROR_PIPE_LISTENING
			&& Error != ERROR_PIPE_BUSY)
		{
			return false;
		}
		if (!WaitForPipeRetry(DeadlineSeconds))
		{
			return false;
		}
	}
	return true;
}

bool WriteExact(
	const HANDLE Pipe,
	const uint8* Source,
	const uint32 Size,
	const double DeadlineSeconds)
{
	uint32 Offset = 0;
	while (Offset < Size)
	{
		DWORD Written = 0;
		if (WriteFile(Pipe, Source + Offset, Size - Offset, &Written, nullptr))
		{
			if (Written > 0)
			{
				Offset += Written;
				continue;
			}
			if (!WaitForPipeRetry(DeadlineSeconds))
			{
				return false;
			}
			continue;
		}
		const DWORD Error = GetLastError();
		if (Error != ERROR_NO_DATA
			&& Error != ERROR_PIPE_LISTENING
			&& Error != ERROR_PIPE_BUSY)
		{
			return false;
		}
		if (!WaitForPipeRetry(DeadlineSeconds))
		{
			return false;
		}
	}
	return true;
}

bool FlushWithDeadline(
	const HANDLE Pipe,
	const double DeadlineSeconds)
{
	HANDLE ConnectionThread = nullptr;
	if (!DuplicateHandle(
			GetCurrentProcess(),
			GetCurrentThread(),
			GetCurrentProcess(),
			&ConnectionThread,
			0,
			false,
			DUPLICATE_SAME_ACCESS))
	{
		return false;
	}
	std::atomic<bool> bCompleted{false};
	TFuture<void> Watchdog = Async(
		EAsyncExecution::Thread,
		[Pipe, ConnectionThread, DeadlineSeconds, &bCompleted]
		{
			while (!bCompleted.load()
				&& FPlatformTime::Seconds() < DeadlineSeconds)
			{
				FPlatformProcess::SleepNoStats(0.005f);
			}
			if (!bCompleted.load())
			{
				CancelSynchronousIo(ConnectionThread);
				CancelIoEx(Pipe, nullptr);
				DisconnectNamedPipe(Pipe);
			}
		});
	const bool bFlushed = FlushFileBuffers(Pipe) != 0;
	bCompleted.store(true);
	Watchdog.Get();
	CloseHandle(ConnectionThread);
	return bFlushed;
}

void ProcessConnection(
	const HANDLE Pipe,
	const FString CommandLine,
	const double IoTimeoutSeconds,
	std::atomic<int32>& ActiveConnections)
{
	const double ReadDeadline = FPlatformTime::Seconds() + IoTimeoutSeconds;
	uint8 LengthBytes[4] = {};
	if (ReadExact(
			Pipe,
			LengthBytes,
			UE_ARRAY_COUNT(LengthBytes),
			ReadDeadline))
	{
		const uint32 Length = DecodeLength(LengthBytes);
		if (Length > 0 && Length <= MaximumFrameBytes)
		{
			TArray<uint8> Request;
			Request.SetNumUninitialized(static_cast<int32>(Length));
			if (ReadExact(
					Pipe, Request.GetData(), Length, ReadDeadline))
			{
				const TArray<uint8> Response = HandlePayload(Request, CommandLine);
				const double WriteDeadline =
					FPlatformTime::Seconds() + IoTimeoutSeconds;
				uint8 ResponseLength[4] = {};
				EncodeLength(Response.Num(), ResponseLength);
				if (WriteExact(
						Pipe,
						ResponseLength,
						UE_ARRAY_COUNT(ResponseLength),
						WriteDeadline))
				{
					if (WriteExact(
						Pipe,
						Response.GetData(),
						Response.Num(),
						WriteDeadline))
					{
						FlushWithDeadline(Pipe, WriteDeadline);
					}
				}
			}
		}
	}
	DisconnectNamedPipe(Pipe);
	CloseHandle(Pipe);
	--ActiveConnections;
}

bool MakeCurrentUserSecurity(
	SECURITY_ATTRIBUTES& OutAttributes,
	PSECURITY_DESCRIPTOR& OutDescriptor,
	FString& OutError)
{
	OutDescriptor = nullptr;
	HANDLE Token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
	{
		OutError = TEXT("OpenProcessToken failed for the resident Named Pipe.");
		return false;
	}
	DWORD Size = 0;
	GetTokenInformation(Token, TokenUser, nullptr, 0, &Size);
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(Size);
	if (Size == 0
		|| !GetTokenInformation(Token, TokenUser, Buffer.GetData(), Size, &Size))
	{
		CloseHandle(Token);
		OutError = TEXT("The current user SID could not be read.");
		return false;
	}
	CloseHandle(Token);
	LPWSTR SidString = nullptr;
	const TOKEN_USER* User = reinterpret_cast<const TOKEN_USER*>(Buffer.GetData());
	if (!ConvertSidToStringSidW(User->User.Sid, &SidString))
	{
		OutError = TEXT("The current user SID could not be encoded.");
		return false;
	}
	const FString Sddl = FString::Printf(TEXT("D:P(A;;GA;;;%s)"), SidString);
	LocalFree(SidString);
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			*Sddl, SDDL_REVISION_1, &OutDescriptor, nullptr))
	{
		OutError = TEXT("The current-user Named Pipe ACL could not be created.");
		return false;
	}
	OutAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
	OutAttributes.lpSecurityDescriptor = OutDescriptor;
	OutAttributes.bInheritHandle = false;
	return true;
}

int32 RunPlatformServer(
	const FString& Endpoint,
	const FString& CommandLine,
	const double IdleSeconds,
	const int32 MaximumSessions,
	const double IoTimeoutSeconds)
{
	if (!Endpoint.StartsWith(TEXT("\\\\.\\pipe\\"))
		|| Endpoint.Len() <= 9 || Endpoint.Len() > 240)
	{
		WriteError(TEXT(
			"Win64 --endpoint must be a bounded full Named Pipe path under \\\\.\\pipe\\."));
		return 2;
	}
	SECURITY_ATTRIBUTES Security = {};
	PSECURITY_DESCRIPTOR Descriptor = nullptr;
	FString SecurityError;
	if (!MakeCurrentUserSecurity(Security, Descriptor, SecurityError))
	{
		WriteError(SecurityError);
		return 2;
	}
	std::atomic<int32> ActiveConnections{0};
	TArray<TFuture<void>> Tasks;
	double LastActivity = FPlatformTime::Seconds();
	int32 ExitCode = 0;
	while (!IsEngineExitRequested())
	{
		ReapCompleted(Tasks);
		if (ActiveConnections.load() >= MaximumSessions)
		{
			FPlatformProcess::SleepNoStats(0.01f);
			continue;
		}
		if (ActiveConnections.load() == 0
			&& FPlatformTime::Seconds() - LastActivity >= IdleSeconds)
		{
			break;
		}
		HANDLE Pipe = CreateNamedPipeW(
			*Endpoint,
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
			MaximumSessions,
			64 * 1024,
			64 * 1024,
			0,
			&Security);
		if (Pipe == INVALID_HANDLE_VALUE)
		{
			WriteError(TEXT("The current-user Named Pipe could not be created."));
			ExitCode = 2;
			break;
		}
		bool bConnected = false;
		while (!IsEngineExitRequested())
		{
			if (ConnectNamedPipe(Pipe, nullptr))
			{
				bConnected = true;
				break;
			}
			const DWORD Error = GetLastError();
			if (Error == ERROR_PIPE_CONNECTED)
			{
				bConnected = true;
				break;
			}
			if (Error != ERROR_PIPE_LISTENING && Error != ERROR_NO_DATA)
			{
				break;
			}
			if (ActiveConnections.load() == 0
				&& FPlatformTime::Seconds() - LastActivity >= IdleSeconds)
			{
				break;
			}
			FPlatformProcess::SleepNoStats(0.025f);
		}
		if (!bConnected)
		{
			CloseHandle(Pipe);
			continue;
		}
		LastActivity = FPlatformTime::Seconds();
		++ActiveConnections;
		Tasks.Add(Async(
			EAsyncExecution::Thread,
			[Pipe, CommandLine, IoTimeoutSeconds, &ActiveConnections]
			{
				ProcessConnection(
					Pipe,
					CommandLine,
					IoTimeoutSeconds,
					ActiveConnections);
			}));
	}
	LocalFree(Descriptor);
	for (TFuture<void>& Task : Tasks)
	{
		Task.Get();
	}
	return ExitCode;
}
#else
bool WaitForSocket(
	const int Socket,
	const bool bWrite,
	const double DeadlineSeconds)
{
	while (FPlatformTime::Seconds() < DeadlineSeconds)
	{
		const double Remaining = DeadlineSeconds - FPlatformTime::Seconds();
		timeval Timeout = {};
		Timeout.tv_sec = static_cast<long>(Remaining);
		Timeout.tv_usec = static_cast<long>(
			(Remaining - static_cast<double>(Timeout.tv_sec)) * 1000000.0);
		fd_set Set;
		FD_ZERO(&Set);
		FD_SET(Socket, &Set);
		const int Ready = select(
			Socket + 1,
			bWrite ? nullptr : &Set,
			bWrite ? &Set : nullptr,
			nullptr,
			&Timeout);
		if (Ready > 0)
		{
			return true;
		}
		if (Ready < 0 && errno == EINTR)
		{
			continue;
		}
		return false;
	}
	return false;
}

bool ReadExact(
	const int Socket,
	uint8* Destination,
	const uint32 Size,
	const double DeadlineSeconds)
{
	uint32 Offset = 0;
	while (Offset < Size)
	{
		if (!WaitForSocket(Socket, false, DeadlineSeconds))
		{
			return false;
		}
		const ssize_t Read = recv(Socket, Destination + Offset, Size - Offset, 0);
		if (Read > 0)
		{
			Offset += static_cast<uint32>(Read);
			continue;
		}
		if (Read == 0
			|| (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
		{
			return false;
		}
	}
	return true;
}

bool WriteExact(
	const int Socket,
	const uint8* Source,
	const uint32 Size,
	const double DeadlineSeconds)
{
	uint32 Offset = 0;
	while (Offset < Size)
	{
		if (!WaitForSocket(Socket, true, DeadlineSeconds))
		{
			return false;
		}
		const int SendFlags =
#if defined(MSG_NOSIGNAL)
			MSG_NOSIGNAL;
#else
			0;
#endif
		const ssize_t Written =
			send(Socket, Source + Offset, Size - Offset, SendFlags);
		if (Written > 0)
		{
			Offset += static_cast<uint32>(Written);
			continue;
		}
		if (Written == 0
			|| (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
		{
			return false;
		}
	}
	return true;
}

void ProcessConnection(
	const int Socket,
	const FString CommandLine,
	const double IoTimeoutSeconds,
	std::atomic<int32>& ActiveConnections)
{
	const double ReadDeadline = FPlatformTime::Seconds() + IoTimeoutSeconds;
	uint8 LengthBytes[4] = {};
	if (ReadExact(
			Socket,
			LengthBytes,
			UE_ARRAY_COUNT(LengthBytes),
			ReadDeadline))
	{
		const uint32 Length = DecodeLength(LengthBytes);
		if (Length > 0 && Length <= MaximumFrameBytes)
		{
			TArray<uint8> Request;
			Request.SetNumUninitialized(static_cast<int32>(Length));
			if (ReadExact(
					Socket, Request.GetData(), Length, ReadDeadline))
			{
				const TArray<uint8> Response = HandlePayload(Request, CommandLine);
				const double WriteDeadline =
					FPlatformTime::Seconds() + IoTimeoutSeconds;
				uint8 ResponseLength[4] = {};
				EncodeLength(Response.Num(), ResponseLength);
				if (WriteExact(
						Socket,
						ResponseLength,
						UE_ARRAY_COUNT(ResponseLength),
						WriteDeadline))
				{
					WriteExact(
						Socket,
						Response.GetData(),
						Response.Num(),
						WriteDeadline);
				}
			}
		}
	}
	close(Socket);
	--ActiveConnections;
}

int32 RunPlatformServer(
	const FString& Endpoint,
	const FString& CommandLine,
	const double IdleSeconds,
	const int32 MaximumSessions,
	const double IoTimeoutSeconds)
{
	const FTCHARToUTF8 EndpointUtf8(*Endpoint);
	sockaddr_un Address = {};
	if (!Endpoint.StartsWith(TEXT("/"))
		|| EndpointUtf8.Length() <= 1
		|| EndpointUtf8.Length() >= static_cast<int32>(sizeof(Address.sun_path)))
	{
		WriteError(TEXT("Unix --endpoint must be a bounded absolute socket path."));
		return 2;
	}
	struct stat Existing = {};
	if (lstat(EndpointUtf8.Get(), &Existing) == 0)
	{
		if (!S_ISSOCK(Existing.st_mode) || Existing.st_uid != geteuid())
		{
			WriteError(TEXT("The existing Unix endpoint is not a current-user socket."));
			return 2;
		}
		unlink(EndpointUtf8.Get());
	}
	const int Listener = socket(AF_UNIX, SOCK_STREAM, 0);
	if (Listener < 0)
	{
		WriteError(TEXT("The Unix domain socket could not be created."));
		return 2;
	}
	Address.sun_family = AF_UNIX;
	std::memcpy(
		Address.sun_path, EndpointUtf8.Get(), EndpointUtf8.Length() + 1);
	const mode_t OldMask = umask(0077);
	const int BindResult = bind(
		Listener, reinterpret_cast<sockaddr*>(&Address), sizeof(Address));
	umask(OldMask);
	if (BindResult != 0 || chmod(EndpointUtf8.Get(), 0600) != 0
		|| listen(Listener, MaximumSessions) != 0)
	{
		close(Listener);
		unlink(EndpointUtf8.Get());
		WriteError(TEXT("The current-user Unix domain socket could not be bound."));
		return 2;
	}
	std::atomic<int32> ActiveConnections{0};
	TArray<TFuture<void>> Tasks;
	double LastActivity = FPlatformTime::Seconds();
	while (!IsEngineExitRequested())
	{
		ReapCompleted(Tasks);
		if (ActiveConnections.load() == 0
			&& FPlatformTime::Seconds() - LastActivity >= IdleSeconds)
		{
			break;
		}
		if (ActiveConnections.load() >= MaximumSessions)
		{
			FPlatformProcess::SleepNoStats(0.01f);
			continue;
		}
		fd_set Set;
		FD_ZERO(&Set);
		FD_SET(Listener, &Set);
		timeval Timeout = {0, 100000};
		const int Ready = select(Listener + 1, &Set, nullptr, nullptr, &Timeout);
		if (Ready <= 0)
		{
			continue;
		}
		const int Connection = accept(Listener, nullptr, nullptr);
		if (Connection < 0)
		{
			continue;
		}
		const int ExistingFlags = fcntl(Connection, F_GETFL, 0);
		if (ExistingFlags < 0
			|| fcntl(Connection, F_SETFL, ExistingFlags | O_NONBLOCK) != 0)
		{
			close(Connection);
			continue;
		}
#if PLATFORM_MAC
		const int NoSignal = 1;
		setsockopt(
			Connection, SOL_SOCKET, SO_NOSIGPIPE, &NoSignal, sizeof(NoSignal));
#endif
		LastActivity = FPlatformTime::Seconds();
		++ActiveConnections;
		Tasks.Add(Async(
			EAsyncExecution::Thread,
			[Connection, CommandLine, IoTimeoutSeconds, &ActiveConnections]
			{
				ProcessConnection(
					Connection,
					CommandLine,
					IoTimeoutSeconds,
					ActiveConnections);
			}));
	}
	close(Listener);
	unlink(EndpointUtf8.Get());
	for (TFuture<void>& Task : Tasks)
	{
		Task.Get();
	}
	return 0;
}
#endif
}

int32 FResidentServer::Run(const FString& CommandLine)
{
	// FModuleManager rejects the first dynamic module load from a connection
	// worker thread. Preload and probe TraceServices while Run is still executing
	// on the Program main thread; subsequent serialized requests may then create
	// independent per-request analysis sessions safely.
	ITraceServicesModule* TraceServicesModule =
		FModuleManager::LoadModulePtr<ITraceServicesModule>(TEXT("TraceServices"));
	if (!TraceServicesModule
		|| !TraceServicesModule->GetAnalysisService().IsValid())
	{
		WriteError(TEXT(
			"TraceServices could not be initialized on the resident Program main thread."));
		return 2;
	}

	FString Endpoint;
	if (!ReadCommandLineValue(*CommandLine, TEXT("endpoint="), Endpoint)
		|| Endpoint.IsEmpty())
	{
		WriteError(TEXT("--serve requires --endpoint=<local IPC path>."));
		return 2;
	}
	double IdleSeconds = 600.0;
	ReadCommandLineValue(*CommandLine, TEXT("idleSeconds="), IdleSeconds);
	IdleSeconds = FMath::Clamp(IdleSeconds, 1.0, 3600.0);
	int32 MaximumSessions = 2;
	ReadCommandLineValue(*CommandLine, TEXT("maxSessions="), MaximumSessions);
	MaximumSessions = FMath::Clamp(MaximumSessions, 1, 2);
	double IoTimeoutSeconds = 15.0;
	ReadCommandLineValue(
		*CommandLine,
		TEXT("connectionIoTimeoutSeconds="),
		IoTimeoutSeconds);
	IoTimeoutSeconds = FMath::Clamp(IoTimeoutSeconds, 1.0, 120.0);
	return RunPlatformServer(
		Endpoint,
		CommandLine,
		IdleSeconds,
		MaximumSessions,
		IoTimeoutSeconds);
}
}
