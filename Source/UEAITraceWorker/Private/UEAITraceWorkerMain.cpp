// UE 5.3 does not forward project crypto registration definitions into a
// plugin-hosted Program launch module. This worker never mounts pak files, so
// provide the same empty fallbacks used by the engine's low-level test host.
#ifndef IMPLEMENT_ENCRYPTION_KEY_REGISTRATION
#define IMPLEMENT_ENCRYPTION_KEY_REGISTRATION()
#endif
#ifndef IMPLEMENT_SIGNING_KEY_REGISTRATION
#define IMPLEMENT_SIGNING_KEY_REGISTRATION()
#endif

#include "RequiredProgramMainCPPInclude.h"
#include "TraceWorkerProtocol.h"
#include "TraceWorkerResidentServer.h"
#include "TraceWorkerFixture.h"
#include "TraceWorkerCommandLine.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <cstdio>
#include <string>

#if PLATFORM_WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

IMPLEMENT_APPLICATION(UEAITraceWorker, "UEAITraceWorker");

namespace
{
constexpr SIZE_T MaximumRequestBytes = 4 * 1024 * 1024;

class FScopedPreInitStdoutSilencer
{
public:
	FScopedPreInitStdoutSilencer()
	{
		std::fflush(stdout);
#if PLATFORM_WINDOWS
		SavedDescriptor = ::_dup(::_fileno(stdout));
		FILE* NullStream = nullptr;
		if (SavedDescriptor >= 0
			&& ::freopen_s(&NullStream, "NUL", "w", stdout) != 0)
		{
			::_close(SavedDescriptor);
			SavedDescriptor = -1;
		}
#else
		SavedDescriptor = ::dup(::fileno(stdout));
		if (SavedDescriptor >= 0 && !::freopen("/dev/null", "w", stdout))
		{
			::close(SavedDescriptor);
			SavedDescriptor = -1;
		}
#endif
	}

	~FScopedPreInitStdoutSilencer()
	{
		Restore();
	}

	void Restore()
	{
		if (SavedDescriptor < 0)
		{
			return;
		}
		std::fflush(stdout);
#if PLATFORM_WINDOWS
		::_dup2(SavedDescriptor, ::_fileno(stdout));
		::_close(SavedDescriptor);
#else
		::dup2(SavedDescriptor, ::fileno(stdout));
		::close(SavedDescriptor);
#endif
		SavedDescriptor = -1;
	}

private:
	int SavedDescriptor = -1;
};

class FScopedStdoutToStderr
{
public:
	FScopedStdoutToStderr()
	{
		std::fflush(stdout);
		std::fflush(stderr);
#if PLATFORM_WINDOWS
		SavedDescriptor = ::_dup(::_fileno(stdout));
		if (SavedDescriptor >= 0
			&& ::_dup2(::_fileno(stderr), ::_fileno(stdout)) != 0)
		{
			::_close(SavedDescriptor);
			SavedDescriptor = -1;
		}
#else
		SavedDescriptor = ::dup(::fileno(stdout));
		if (SavedDescriptor >= 0
			&& ::dup2(::fileno(stderr), ::fileno(stdout)) < 0)
		{
			::close(SavedDescriptor);
			SavedDescriptor = -1;
		}
#endif
	}

	~FScopedStdoutToStderr()
	{
		Restore();
	}

	void Restore()
	{
		if (SavedDescriptor < 0)
		{
			return;
		}
		std::fflush(stdout);
#if PLATFORM_WINDOWS
		::_dup2(SavedDescriptor, ::_fileno(stdout));
		::_close(SavedDescriptor);
#else
		::dup2(SavedDescriptor, ::fileno(stdout));
		::close(SavedDescriptor);
#endif
		SavedDescriptor = -1;
	}

private:
	int SavedDescriptor = -1;
};

bool ReadStandardInput(FString& OutText)
{
	std::string Bytes;
	char Buffer[4096];
	while (!std::feof(stdin))
	{
		const SIZE_T Read = std::fread(Buffer, 1, sizeof(Buffer), stdin);
		if (Read == 0)
		{
			break;
		}
		if (Bytes.size() + Read > MaximumRequestBytes)
		{
			return false;
		}
		Bytes.append(Buffer, Read);
	}
	if (Bytes.empty())
	{
		return false;
	}
	const FUTF8ToTCHAR Converter(Bytes.data(), static_cast<int32>(Bytes.size()));
	OutText = FString(Converter.Length(), Converter.Get());
	if (!OutText.IsEmpty() && OutText[0] == 0xfeff)
	{
		OutText.RemoveAt(0);
	}
	return true;
}

void WriteStandardOutput(const TSharedPtr<FJsonObject>& Object)
{
	FString Serialized;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
			&Serialized);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	const FTCHARToUTF8 Utf8(*Serialized);
	std::fwrite(Utf8.Get(), 1, Utf8.Length(), stdout);
	std::fwrite("\n", 1, 1, stdout);
	std::fflush(stdout);
}

TSharedPtr<FJsonObject> MakeParseError(
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
}

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope TaskScope(ETaskTag::EGameThread);
	ON_SCOPE_EXIT
	{
		// The one-shot protocol has already emitted its single JSON line when
		// this cleanup runs. TraceServices and module shutdown may log (for
		// example, LogAnalysisCache), so keep those messages off stdout as well.
		FScopedStdoutToStderr ShutdownLogRedirect;
		UEAI::TraceWorker::FProtocol::ShutdownAnalysisCache();
		RequestEngineExit(TEXT("UEAITraceWorker completed"));
		FEngineLoop::AppPreExit();
		FModuleManager::Get().UnloadModulesAtShutdown();
		FEngineLoop::AppExit();
	};

	// The stdio transport is a JSON protocol. Suppress engine log routing so a
	// caller never has to strip localization or platform warnings from stdout.
	FScopedPreInitStdoutSilencer PreInitStdout;
	const int32 InitResult = GEngineLoop.PreInit(
		ArgC,
		ArgV,
		// UE 5.3 still installs the default file output device for -NoLog.
		// -NoDefaultLog is the Core switch that prevents a relocated Worker
		// from writing Saved/Logs beside the packaged executable.
		TEXT("-Unattended -NoLog -NoDefaultLog -SaveToUserDir"));
	PreInitStdout.Restore();
	if (InitResult != 0)
	{
		return InitResult;
	}
	FModuleManager::Get().StartProcessingNewlyLoadedObjects();
	FString FixturePath;
	if (UEAI::TraceWorker::ReadCommandLineValue(
			FCommandLine::Get(), TEXT("generate-fixture="), FixturePath))
	{
		FString FixtureEngineMarker;
		UEAI::TraceWorker::ReadCommandLineValue(
			FCommandLine::Get(),
			TEXT("fixtureEngineMarker="),
			FixtureEngineMarker);
		FString ReceiptPath;
		FString Error;
		FScopedStdoutToStderr RequestLogRedirect;
		const int32 Result = UEAI::TraceWorker::GenerateDiagnosticTraceFixture(
			FixturePath, FixtureEngineMarker, ReceiptPath, Error);
		TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
		Output->SetStringField(
			TEXT("schema"), TEXT("ue.trace-diagnostic-fixture-result.v1"));
		Output->SetBoolField(TEXT("ok"), Result == 0);
		Output->SetStringField(TEXT("tracePath"), FixturePath);
		Output->SetStringField(TEXT("receiptPath"), ReceiptPath);
		Output->SetStringField(TEXT("error"), Error);
		RequestLogRedirect.Restore();
		WriteStandardOutput(Output);
		return Result;
	}
	if (UEAI::TraceWorker::HasCommandLineSwitch(
		FCommandLine::Get(), TEXT("serve")))
	{
		return UEAI::TraceWorker::FResidentServer::Run(FCommandLine::Get());
	}
	if (!UEAI::TraceWorker::HasCommandLineSwitch(
		FCommandLine::Get(), TEXT("stdio")))
	{
		WriteStandardOutput(MakeParseError(
			TEXT("trace_worker_transport_required"),
			TEXT("UEAITraceWorker requires either --stdio or --serve.")));
		return 2;
	}

	FString Input;
	if (!ReadStandardInput(Input))
	{
		WriteStandardOutput(MakeParseError(
			TEXT("trace_worker_request_invalid"),
			TEXT("The worker request is empty, too large, or not UTF-8.")));
		return 2;
	}
	TSharedPtr<FJsonObject> Request;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Input);
	if (!FJsonSerializer::Deserialize(Reader, Request) || !Request.IsValid())
	{
		WriteStandardOutput(MakeParseError(
			TEXT("trace_worker_request_invalid"),
			TEXT("The worker request is not a valid JSON object.")));
		return 2;
	}
	FScopedStdoutToStderr RequestLogRedirect;
	const TSharedPtr<FJsonObject> Response =
		UEAI::TraceWorker::FProtocol::HandleRequest(Request, FCommandLine::Get());
	RequestLogRedirect.Restore();
	WriteStandardOutput(Response);
	return Response->GetBoolField(TEXT("ok")) ? 0 : 1;
}
