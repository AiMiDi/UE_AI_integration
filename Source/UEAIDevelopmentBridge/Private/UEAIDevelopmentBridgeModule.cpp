#include "Modules/ModuleManager.h"

#if WITH_UEAI_DEVELOPMENT_BRIDGE

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Atomic.h"

#include <openssl/sha.h>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <Windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_UNIX
#include <cerrno>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace UEAI::DevelopmentBridge
{
namespace
{
constexpr uint32 MaxMessageBytes = 64 * 1024;
constexpr double PairingLifetimeSeconds = 60.0;
constexpr double SessionLifetimeSeconds = 300.0;

FString OpaqueToken()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits)
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString Sha256(const FString& Value)
{
	const FTCHARToUTF8 Utf8(*Value);
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	::SHA256(
		reinterpret_cast<const unsigned char*>(Utf8.Get()),
		static_cast<size_t>(Utf8.Length()),
		Digest);
	return TEXT("sha256:")
		+ BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
}

FString Serialize(const TSharedPtr<FJsonObject>& Object)
{
	FString Result;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Result;
}

TSharedPtr<FJsonObject> Error(const FString& Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), false);
	TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("code"), Code);
	Value->SetStringField(TEXT("message"), Message);
	Root->SetObjectField(TEXT("error"), Value);
	return Root;
}

TSharedPtr<FJsonObject> Success(const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetObjectField(TEXT("data"), Data);
	return Root;
}
}

class FServer final : public FRunnable
{
public:
	~FServer() override
	{
		StopServer();
	}

	bool Start()
	{
		if (Thread)
		{
			return true;
		}
		PairingToken = OpaqueToken();
		PairingExpiresSeconds = FPlatformTime::Seconds() + PairingLifetimeSeconds;
		StartedAtUtc = FDateTime::UtcNow().ToIso8601();
		BuildId = FApp::GetBuildVersion();
		if (BuildId.IsEmpty())
		{
			BuildId = TEXT("unknown");
		}
		ProjectDigest = Sha256(FString(FApp::GetProjectName()));
		const uint32 Pid = FPlatformProcess::GetCurrentProcessId();
#if PLATFORM_WINDOWS
		Endpoint = FString::Printf(
			TEXT("\\\\.\\pipe\\ueai-development-%u-%s"),
			Pid,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
#else
		Endpoint = FPaths::Combine(
			FPlatformProcess::UserTempDir(),
			FString::Printf(
				TEXT("ueai-development-%u-%s.sock"),
				Pid,
				*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
#endif
		if (!CreateEndpoint())
		{
			return false;
		}
		if (!WriteDiscoveryRecord())
		{
			DestroyEndpoint();
			return false;
		}
		bStopRequested.Store(false);
		Thread = FRunnableThread::Create(
			this,
			TEXT("UEAI Development Bridge"),
			0,
			TPri_BelowNormal);
		if (!Thread)
		{
			DestroyEndpoint();
			IFileManager::Get().Delete(*DiscoveryPath, false, true);
			return false;
		}
		return true;
	}

	void StopServer()
	{
		bStopRequested.Store(true);
		DestroyEndpoint();
		if (Thread)
		{
			Thread->WaitForCompletion();
			delete Thread;
			Thread = nullptr;
		}
		if (!DiscoveryPath.IsEmpty())
		{
			IFileManager::Get().Delete(*DiscoveryPath, false, true);
		}
		SessionToken.Reset();
	}

	uint32 Run() override
	{
		while (!bStopRequested.Load())
		{
			ServeOne();
		}
		return 0;
	}

	void Stop() override
	{
		bStopRequested.Store(true);
		DestroyEndpoint();
	}

private:
	FString AttachDigest(const FString& Scope) const
	{
		return Sha256(
			TEXT("ue.development-attach.v1|")
			+ FString::FromInt(FPlatformProcess::GetCurrentProcessId())
			+ TEXT("|") + StartedAtUtc
			+ TEXT("|") + BuildId
			+ TEXT("|") + ProjectDigest
			+ TEXT("|") + Scope);
	}

	bool WriteDiscoveryRecord()
	{
		const FString Directory = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("UEAI"), TEXT("DevelopmentBridge"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		DiscoveryPath = FPaths::Combine(
			Directory,
			FString::Printf(
				TEXT("%u.json"),
				FPlatformProcess::GetCurrentProcessId()));
		TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("schema"), TEXT("ue.development-bridge.v1"));
		Record->SetNumberField(
			TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
		Record->SetStringField(TEXT("processStartTime"), StartedAtUtc);
		Record->SetStringField(TEXT("buildId"), BuildId);
		Record->SetStringField(TEXT("projectDigest"), ProjectDigest);
		Record->SetStringField(TEXT("endpoint"), Endpoint);
		Record->SetStringField(TEXT("transport"),
#if PLATFORM_WINDOWS
			TEXT("namedPipe"));
#else
			TEXT("unixSocket"));
#endif
		Record->SetStringField(TEXT("pairingToken"), PairingToken);
		Record->SetStringField(
			TEXT("pairingExpiresAtUtc"),
			(FDateTime::UtcNow() + FTimespan::FromSeconds(PairingLifetimeSeconds))
				.ToIso8601());
		Record->SetStringField(TEXT("observePlanDigest"), AttachDigest(TEXT("observe")));
		Record->SetStringField(TEXT("controlPlanDigest"), AttachDigest(TEXT("control")));
		Record->SetBoolField(TEXT("shippingExcluded"), true);
		Record->SetBoolField(TEXT("httpEnabled"), false);
		const bool bSaved = FFileHelper::SaveStringToFile(
			Serialize(Record),
			*DiscoveryPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
#if PLATFORM_UNIX
		if (bSaved)
		{
			const FTCHARToUTF8 PathUtf8(*DiscoveryPath);
			::chmod(PathUtf8.Get(), S_IRUSR | S_IWUSR);
		}
#endif
		return bSaved;
	}

	TSharedPtr<FJsonObject> Handle(const FString& RequestText)
	{
		TSharedPtr<FJsonObject> Request;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestText);
		if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
		{
			return Error(TEXT("invalid_json"), TEXT("Request must be a bounded JSON object."));
		}
		FString Action;
		if (!Request->TryGetStringField(TEXT("action"), Action))
		{
			return Error(TEXT("action_required"), TEXT("action is required."));
		}
		if (Action == TEXT("attach"))
		{
			return Attach(Request);
		}
		FString ProvidedSession;
		Request->TryGetStringField(TEXT("sessionToken"), ProvidedSession);
		if (SessionToken.IsEmpty()
			|| ProvidedSession != SessionToken
			|| FPlatformTime::Seconds() > SessionExpiresSeconds)
		{
			return Error(TEXT("session_expired"), TEXT("Attach session is absent or expired."));
		}
		SessionExpiresSeconds = FPlatformTime::Seconds() + SessionLifetimeSeconds;
		if (Action == TEXT("status"))
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("scope"), SessionScope);
			Data->SetStringField(TEXT("buildId"), BuildId);
			Data->SetStringField(TEXT("projectDigest"), ProjectDigest);
			Data->SetArrayField(TEXT("capabilities"), {
				MakeShared<FJsonValueString>(TEXT("development.runtime.status")),
				MakeShared<FJsonValueString>(TEXT("development.runtime.detach")) });
			Data->SetBoolField(TEXT("mayTerminateTarget"), false);
			return Success(Data);
		}
		if (Action == TEXT("detach"))
		{
			SessionToken.Reset();
			SessionScope.Reset();
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("detached"), true);
			Data->SetBoolField(TEXT("targetTerminated"), false);
			return Success(Data);
		}
		return Error(TEXT("capability_not_runtime_safe"), TEXT("Only status and detach are runtime-safe in this bridge."));
	}

	TSharedPtr<FJsonObject> Attach(const TSharedPtr<FJsonObject>& Request)
	{
		FString Token;
		FString Scope;
		FString ApprovedDigest;
		bool bConfirmed = false;
		Request->TryGetStringField(TEXT("pairingToken"), Token);
		Request->TryGetStringField(TEXT("scope"), Scope);
		Request->TryGetStringField(TEXT("approvePlanDigest"), ApprovedDigest);
		Request->TryGetBoolField(TEXT("confirmAttach"), bConfirmed);
		if (PairingToken.IsEmpty()
			|| Token != PairingToken
			|| FPlatformTime::Seconds() > PairingExpiresSeconds)
		{
			return Error(TEXT("pairing_token_invalid"), TEXT("Pairing token is absent, expired, or already used."));
		}
		if (Scope != TEXT("observe") && Scope != TEXT("control"))
		{
			return Error(TEXT("scope_invalid"), TEXT("scope must be observe or control."));
		}
		const FString Expected = AttachDigest(Scope);
		if (!bConfirmed || ApprovedDigest != Expected)
		{
			TSharedPtr<FJsonObject> Root = Error(
				TEXT("attach_approval_required"),
				TEXT("Attach requires approvePlanDigest and confirmAttach."));
			Root->GetObjectField(TEXT("error"))->SetStringField(TEXT("planDigest"), Expected);
			return Root;
		}
		PairingToken.Reset();
		SessionToken = OpaqueToken();
		SessionScope = Scope;
		SessionExpiresSeconds = FPlatformTime::Seconds() + SessionLifetimeSeconds;
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("sessionToken"), SessionToken);
		Data->SetStringField(TEXT("scope"), Scope);
		Data->SetNumberField(TEXT("ttlSeconds"), SessionLifetimeSeconds);
		Data->SetBoolField(TEXT("targetOwnedByAI"), false);
		Data->SetBoolField(TEXT("mayTerminateTarget"), false);
		return Success(Data);
	}

	bool CreateEndpoint();
	void DestroyEndpoint();
	void ServeOne();

	FRunnableThread* Thread = nullptr;
	TAtomic<bool> bStopRequested{false};
	FString Endpoint;
	FString DiscoveryPath;
	FString PairingToken;
	FString SessionToken;
	FString SessionScope;
	FString StartedAtUtc;
	FString BuildId;
	FString ProjectDigest;
	double PairingExpiresSeconds = 0.0;
	double SessionExpiresSeconds = 0.0;
#if PLATFORM_WINDOWS
	HANDLE Pipe = INVALID_HANDLE_VALUE;
	PSECURITY_DESCRIPTOR SecurityDescriptor = nullptr;
#elif PLATFORM_UNIX
	int32 ListenSocket = -1;
#endif
};

#if PLATFORM_WINDOWS
bool FServer::CreateEndpoint()
{
	HANDLE Token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
	{
		return false;
	}
	DWORD Required = 0;
	GetTokenInformation(Token, TokenUser, nullptr, 0, &Required);
	TArray<uint8> TokenInfo;
	TokenInfo.SetNumUninitialized(Required);
	if (!GetTokenInformation(Token, TokenUser, TokenInfo.GetData(), Required, &Required))
	{
		CloseHandle(Token);
		return false;
	}
	CloseHandle(Token);
	TOKEN_USER* User = reinterpret_cast<TOKEN_USER*>(TokenInfo.GetData());
	const DWORD AclBytes = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE)
		+ GetLengthSid(User->User.Sid) - sizeof(DWORD);
	TArray<uint8> AclStorage;
	AclStorage.SetNumZeroed(AclBytes);
	PACL Acl = reinterpret_cast<PACL>(AclStorage.GetData());
	if (!InitializeAcl(Acl, AclBytes, ACL_REVISION)
		|| !AddAccessAllowedAce(Acl, ACL_REVISION, GENERIC_ALL, User->User.Sid))
	{
		return false;
	}
	SecurityDescriptor = static_cast<PSECURITY_DESCRIPTOR>(
		LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH));
	if (!SecurityDescriptor
		|| !InitializeSecurityDescriptor(SecurityDescriptor, SECURITY_DESCRIPTOR_REVISION)
		|| !SetSecurityDescriptorOwner(SecurityDescriptor, User->User.Sid, false)
		|| !SetSecurityDescriptorDacl(SecurityDescriptor, true, Acl, false))
	{
		if (SecurityDescriptor) LocalFree(SecurityDescriptor);
		SecurityDescriptor = nullptr;
		return false;
	}
	SECURITY_ATTRIBUTES Attributes{};
	Attributes.nLength = sizeof(Attributes);
	Attributes.lpSecurityDescriptor = SecurityDescriptor;
	Pipe = CreateNamedPipe(
		*Endpoint,
		PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		1,
		MaxMessageBytes,
		MaxMessageBytes,
		0,
		&Attributes);
	return Pipe != INVALID_HANDLE_VALUE;
}

void FServer::DestroyEndpoint()
{
	if (Pipe != INVALID_HANDLE_VALUE)
	{
		CancelIoEx(Pipe, nullptr);
		DisconnectNamedPipe(Pipe);
		CloseHandle(Pipe);
		Pipe = INVALID_HANDLE_VALUE;
	}
	if (SecurityDescriptor)
	{
		LocalFree(SecurityDescriptor);
		SecurityDescriptor = nullptr;
	}
}

void FServer::ServeOne()
{
	if (Pipe == INVALID_HANDLE_VALUE)
	{
		return;
	}
	if (!ConnectNamedPipe(Pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
	{
		return;
	}
	DWORD Read = 0;
	uint32 Size = 0;
	if (ReadFile(Pipe, &Size, sizeof(Size), &Read, nullptr)
		&& Read == sizeof(Size) && Size > 0 && Size <= MaxMessageBytes)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size + 1);
		if (ReadFile(Pipe, Bytes.GetData(), Size, &Read, nullptr) && Read == Size)
		{
			Bytes[Size] = 0;
			const FString Response = Serialize(Handle(UTF8_TO_TCHAR(
				reinterpret_cast<const ANSICHAR*>(Bytes.GetData()))));
			const FTCHARToUTF8 Utf8(*Response);
			const uint32 ResponseSize = Utf8.Length();
			DWORD Written = 0;
			WriteFile(Pipe, &ResponseSize, sizeof(ResponseSize), &Written, nullptr);
			WriteFile(Pipe, Utf8.Get(), ResponseSize, &Written, nullptr);
			FlushFileBuffers(Pipe);
		}
	}
	DisconnectNamedPipe(Pipe);
}
#elif PLATFORM_UNIX
bool FServer::CreateEndpoint()
{
	ListenSocket = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (ListenSocket < 0)
	{
		return false;
	}
	sockaddr_un Address{};
	Address.sun_family = AF_UNIX;
	const FTCHARToUTF8 EndpointUtf8(*Endpoint);
	if (EndpointUtf8.Length() >= static_cast<int32>(sizeof(Address.sun_path)))
	{
		DestroyEndpoint();
		return false;
	}
	FCStringAnsi::Strncpy(Address.sun_path, EndpointUtf8.Get(), sizeof(Address.sun_path));
	::unlink(Address.sun_path);
	if (::bind(ListenSocket, reinterpret_cast<sockaddr*>(&Address), sizeof(Address)) != 0
		|| ::chmod(Address.sun_path, S_IRUSR | S_IWUSR) != 0
		|| ::listen(ListenSocket, 1) != 0)
	{
		DestroyEndpoint();
		return false;
	}
	return true;
}

void FServer::DestroyEndpoint()
{
	if (ListenSocket >= 0)
	{
		::shutdown(ListenSocket, SHUT_RDWR);
		::close(ListenSocket);
		ListenSocket = -1;
	}
	if (!Endpoint.IsEmpty())
	{
		const FTCHARToUTF8 EndpointUtf8(*Endpoint);
		::unlink(EndpointUtf8.Get());
	}
}

namespace
{
bool ReadAll(const int32 Socket, void* Data, uint32 Size)
{
	uint8* Cursor = static_cast<uint8*>(Data);
	while (Size > 0)
	{
		const ssize_t Count = ::read(Socket, Cursor, Size);
		if (Count <= 0) return false;
		Cursor += Count;
		Size -= static_cast<uint32>(Count);
	}
	return true;
}
}

void FServer::ServeOne()
{
	if (ListenSocket < 0) return;
	const int32 Client = ::accept(ListenSocket, nullptr, nullptr);
	if (Client < 0) return;
	uint32 Size = 0;
	if (ReadAll(Client, &Size, sizeof(Size)) && Size > 0 && Size <= MaxMessageBytes)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size + 1);
		if (ReadAll(Client, Bytes.GetData(), Size))
		{
			Bytes[Size] = 0;
			const FString Response = Serialize(Handle(UTF8_TO_TCHAR(
				reinterpret_cast<const ANSICHAR*>(Bytes.GetData()))));
			const FTCHARToUTF8 Utf8(*Response);
			const uint32 ResponseSize = Utf8.Length();
			::write(Client, &ResponseSize, sizeof(ResponseSize));
			::write(Client, Utf8.Get(), ResponseSize);
		}
	}
	::close(Client);
}
#else
bool FServer::CreateEndpoint() { return false; }
void FServer::DestroyEndpoint() {}
void FServer::ServeOne() {}
#endif
}

class FUEAIDevelopmentBridgeModule final : public IModuleInterface
{
public:
	void StartupModule() override
	{
		EnableCommand = MakeUnique<FAutoConsoleCommand>(
			TEXT("ueai.Bridge.Enable"),
			TEXT("Explicitly opt this Development/DebugGame process into the local UE AI bridge."),
			FConsoleCommandDelegate::CreateRaw(this, &FUEAIDevelopmentBridgeModule::Enable));
		if (FParse::Param(FCommandLine::Get(), TEXT("UEAIDevelopmentBridge")))
		{
			Enable();
		}
	}

	void ShutdownModule() override
	{
		EnableCommand.Reset();
		Server.Reset();
	}

private:
	void Enable()
	{
		if (!Server)
		{
			Server = MakeUnique<UEAI::DevelopmentBridge::FServer>();
			if (!Server->Start())
			{
				Server.Reset();
			}
		}
	}

	TUniquePtr<FAutoConsoleCommand> EnableCommand;
	TUniquePtr<UEAI::DevelopmentBridge::FServer> Server;
};

#else

class FUEAIDevelopmentBridgeModule final : public IModuleInterface
{
};

#endif

IMPLEMENT_MODULE(FUEAIDevelopmentBridgeModule, UEAIDevelopmentBridge)
