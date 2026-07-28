#include "UEAIIntegrationServer.h"

#include "Core/MCPExecutor.h"
#include "Dom/JsonValue.h"
#include "HttpServerModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tools/MCPToolRegistry.h"
#include "Transport/MCPHttpContract.h"
#include "Workflow/UEWorkflowRuntime.h"

#ifndef UE_AI_INTEGRATION_VERSION
#define UE_AI_INTEGRATION_VERSION "unknown"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUEAIIntegrationServer, Log, All);

FUEAIIntegrationServer::FUEAIIntegrationServer(
	FMCPToolRegistry& InRegistry,
	FMCPExecutor& InExecutor)
	: Registry(InRegistry)
	, Executor(InExecutor)
	, WorkflowRuntime(
		MakeUnique<UEAIIntegration::Workflow::FWorkflowRuntime>(InRegistry))
{
}

FUEAIIntegrationServer::~FUEAIIntegrationServer()
{
	Stop();
}

bool FUEAIIntegrationServer::Start(int32 Port)
{
	if (bIsRunning)
	{
		return true;
	}

	ListenPort = Port;
	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();
	HttpServerModule.StartAllListeners();
	Router = HttpServerModule.GetHttpRouter(ListenPort, true);
	if (!Router.IsValid())
	{
		UE_LOG(
			LogUEAIIntegrationServer,
			Error,
			TEXT("Failed to acquire HTTP router on port %d."),
			ListenPort);
		return false;
	}

	const FHttpRouteHandle HealthRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/health")),
		EHttpServerRequestVerbs::VERB_GET,
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleHealth(Request, OnComplete);
		});
	if (!HealthRoute.IsValid())
	{
		UE_LOG(LogUEAIIntegrationServer, Error, TEXT("Failed to bind /api/health."));
		Router.Reset();
		return false;
	}
	RouteHandles.Add(HealthRoute);

	const FHttpRouteHandle CapabilitiesRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/capabilities")),
		EHttpServerRequestVerbs::VERB_GET,
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleCapabilities(Request, OnComplete);
		});
	if (!CapabilitiesRoute.IsValid())
	{
		UE_LOG(LogUEAIIntegrationServer, Error, TEXT("Failed to bind /api/capabilities."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(CapabilitiesRoute);

	const FHttpRouteHandle ExecuteRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/execute")),
		EHttpServerRequestVerbs::VERB_POST,
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleExecute(Request, OnComplete);
		});
	if (!ExecuteRoute.IsValid())
	{
		UE_LOG(LogUEAIIntegrationServer, Error, TEXT("Failed to bind /api/execute."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(ExecuteRoute);

	const FHttpRouteHandle WorkflowHandshakeRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/v1/workflow/handshake")),
		EHttpServerRequestVerbs::VERB_GET,
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleWorkflowHandshake(Request, OnComplete);
		});
	if (!WorkflowHandshakeRoute.IsValid())
	{
		UE_LOG(
			LogUEAIIntegrationServer,
			Error,
			TEXT("Failed to bind /api/v1/workflow/handshake."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(WorkflowHandshakeRoute);

	const FHttpRouteHandle WorkflowRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/v1/workflow")),
		EHttpServerRequestVerbs::VERB_POST,
		[this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleWorkflow(Request, OnComplete);
		});
	if (!WorkflowRoute.IsValid())
	{
		UE_LOG(
			LogUEAIIntegrationServer,
			Error,
			TEXT("Failed to bind /api/v1/workflow."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(WorkflowRoute);

	bIsRunning = true;

	UE_LOG(
		LogUEAIIntegrationServer,
		Log,
		TEXT("HTTP API listening on port %d (%s)."),
		ListenPort,
		Registry.IsReady() ? TEXT("ready") : TEXT("degraded"));
	return true;
}

void FUEAIIntegrationServer::Stop()
{
	if (!bIsRunning && RouteHandles.IsEmpty())
	{
		return;
	}

	UnbindRoutes();
	Router.Reset();
	bIsRunning = false;

	TSharedPtr<FPendingMCPExecuteRequest> Pending;
	while (RequestQueue.Dequeue(Pending))
	{
		if (Pending.IsValid())
		{
			SendError(
				Pending->OnComplete,
				503,
				TEXT("service_unavailable"),
				TEXT("The UE_AI_integration server is stopping."));
		}
	}

	UE_LOG(LogUEAIIntegrationServer, Log, TEXT("HTTP routes unbound."));
}

void FUEAIIntegrationServer::Tick(float DeltaTime)
{
	ProcessOneRequest();
}

void FUEAIIntegrationServer::ProcessOneRequest()
{
	TSharedPtr<FPendingMCPExecuteRequest> Pending;
	if (!RequestQueue.Dequeue(Pending) || !Pending.IsValid())
	{
		return;
	}

	if (Pending->Kind == EUEAIIntegrationRequestKind::WorkflowHandshake)
	{
		const FMCPResult Result = WorkflowRuntime->MakeHandshake();
		if (Result.bOk)
		{
			SendSuccess(Pending->OnComplete, Result.Data);
		}
		else
		{
			SendError(
				Pending->OnComplete,
				Result.Error.HttpStatus,
				Result.Error.Code,
				Result.Error.Message,
				Result.Error.Details);
		}
		return;
	}

	TSharedPtr<FJsonObject> RequestObject;
	if (Pending->Body.IsEmpty())
	{
		SendError(
			Pending->OnComplete,
			400,
			TEXT("invalid_json"),
			TEXT("Request body must be a JSON object."));
		return;
	}

	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Pending->Body);
	if (!FJsonSerializer::Deserialize(Reader, RequestObject) || !RequestObject.IsValid())
	{
		SendError(
			Pending->OnComplete,
			400,
			TEXT("invalid_json"),
			TEXT("Request body contains invalid JSON."));
		return;
	}

	if (Pending->Kind == EUEAIIntegrationRequestKind::WorkflowAction)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (WorkflowDispatchObserverForTesting)
		{
			WorkflowDispatchObserverForTesting();
		}
#endif
		const FMCPResult Result = WorkflowRuntime->HandleRequest(RequestObject);
		if (!Result.bOk)
		{
			SendError(
				Pending->OnComplete,
				Result.Error.HttpStatus,
				Result.Error.Code,
				Result.Error.Message,
				Result.Error.Details);
			return;
		}
		SendSuccess(Pending->OnComplete, Result.Data);
		return;
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : RequestObject->Values)
	{
		if (Field.Key != TEXT("capability")
			&& Field.Key != TEXT("params")
			&& Field.Key != TEXT("requestId"))
		{
			SendError(
				Pending->OnComplete,
				422,
				TEXT("invalid_params"),
				FString::Printf(TEXT("Unknown request field '%s'."), *Field.Key));
			return;
		}
	}

	FString CapabilityId;
	if (!RequestObject->TryGetStringField(TEXT("capability"), CapabilityId)
		|| CapabilityId.IsEmpty())
	{
		SendError(
			Pending->OnComplete,
			422,
			TEXT("invalid_params"),
			TEXT("Field 'capability' must be a non-empty dotted capability id."));
		return;
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	if (RequestObject->HasField(TEXT("params")))
	{
		if (!RequestObject->HasTypedField<EJson::Object>(TEXT("params")))
		{
			SendError(
				Pending->OnComplete,
				422,
				TEXT("invalid_params"),
				TEXT("Field 'params' must be a JSON object."));
			return;
		}
		Params = RequestObject->GetObjectField(TEXT("params"));
	}

	FString RequestId;
	if (RequestObject->HasField(TEXT("requestId")))
	{
		if (!RequestObject->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty()
			|| RequestId.Len() > 200)
		{
			SendError(
				Pending->OnComplete,
				422,
				TEXT("invalid_params"),
				TEXT("Field 'requestId' must be a non-empty string of at most 200 characters."));
			return;
		}
	}

	FMCPExecutionContext Context;
	Context.Capability = CapabilityId;
	Context.Params = Params;
	Context.RequestId = RequestId;
	const FMCPResult Result = Executor.Execute(Context);
	if (!Result.bOk)
	{
		SendError(
			Pending->OnComplete,
			Result.Error.HttpStatus,
			Result.Error.Code,
			Result.Error.Message,
			Result.Error.Details);
		return;
	}

	SendSuccess(Pending->OnComplete, Result.Data);
}

bool FUEAIIntegrationServer::HandleHealth(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	const FString State = Registry.IsReady() ? TEXT("ready") : TEXT("degraded");
	Data->SetStringField(TEXT("state"), State);
	Data->SetStringField(TEXT("status"), State);
	Data->SetStringField(TEXT("plugin"), TEXT("UE_AI_integration"));

	FString PluginVersion = UTF8_TO_TCHAR(UE_AI_INTEGRATION_VERSION);
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	if (Plugin.IsValid() && !Plugin->GetDescriptor().VersionName.IsEmpty())
	{
		PluginVersion = Plugin->GetDescriptor().VersionName;
	}
	Data->SetStringField(TEXT("version"), PluginVersion);
	Data->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Data->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Data->SetStringField(TEXT("projectName"), FApp::GetProjectName());
	Data->SetStringField(TEXT("mode"), TEXT("editor"));
	Data->SetNumberField(TEXT("capabilityCount"), Registry.GetCapabilityCount());

	TSharedPtr<FJsonObject> DomainCounts = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Pair : Registry.GetDomainCounts())
	{
		DomainCounts->SetNumberField(Pair.Key, Pair.Value);
	}
	Data->SetObjectField(TEXT("domainCounts"), DomainCounts);

	TArray<TSharedPtr<FJsonValue>> ValidationErrors;
	for (const FString& Error : Registry.GetValidationErrors())
	{
		ValidationErrors.Add(MakeShared<FJsonValueString>(Error));
	}
	Data->SetArrayField(TEXT("validationErrors"), ValidationErrors);

	SendSuccess(OnComplete, Data);
	return true;
}

bool FUEAIIntegrationServer::HandleCapabilities(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	FString DomainFilter;
	if (const FString* DomainValue = Request.QueryParams.Find(TEXT("domain")))
	{
		DomainFilter = *DomainValue;
		if (DomainFilter.IsEmpty() || !Registry.GetDomainCounts().Contains(DomainFilter))
		{
			SendError(
				OnComplete,
				422,
				TEXT("invalid_params"),
				FString::Printf(TEXT("Unknown capability domain '%s'."), *DomainFilter));
			return true;
		}
	}

	TArray<TSharedPtr<FJsonValue>> CapabilityValues;
	CapabilityValues.Reserve(Registry.GetCapabilityDescriptors().Num());
	for (const TSharedPtr<FJsonObject>& Descriptor : Registry.GetCapabilityDescriptors())
	{
		if (!DomainFilter.IsEmpty()
			&& Descriptor->GetStringField(TEXT("domain")) != DomainFilter)
		{
			continue;
		}
		CapabilityValues.Add(MakeShared<FJsonValueObject>(Descriptor));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("capabilities"), CapabilityValues);
	SendSuccess(OnComplete, Data);
	return true;
}

bool FUEAIIntegrationServer::HandleExecute(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	return QueueRequest(
		EUEAIIntegrationRequestKind::LegacyExecute,
		Request,
		OnComplete);
}

bool FUEAIIntegrationServer::HandleWorkflowHandshake(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	return QueueRequest(
		EUEAIIntegrationRequestKind::WorkflowHandshake,
		Request,
		OnComplete);
}

bool FUEAIIntegrationServer::HandleWorkflow(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	return QueueRequest(
		EUEAIIntegrationRequestKind::WorkflowAction,
		Request,
		OnComplete);
}

bool FUEAIIntegrationServer::QueueRequest(
	EUEAIIntegrationRequestKind Kind,
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FPendingMCPExecuteRequest> Pending =
		MakeShared<FPendingMCPExecuteRequest>();
	Pending->Kind = Kind;
	Pending->OnComplete = OnComplete;

	if (Request.Body.Num() > 0)
	{
		const FUTF8ToTCHAR Converter(
			reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()),
			Request.Body.Num());
		Pending->Body = FString(Converter.Length(), Converter.Get());
	}

	RequestQueue.Enqueue(Pending);
	return true;
}

void FUEAIIntegrationServer::UnbindRoutes()
{
	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& RouteHandle : RouteHandles)
		{
			if (RouteHandle.IsValid())
			{
				Router->UnbindRoute(RouteHandle);
			}
		}
	}
	RouteHandles.Reset();
}

void FUEAIIntegrationServer::SendJsonResponse(
	const FHttpResultCallback& OnComplete,
	int32 StatusCode,
	const TSharedPtr<FJsonObject>& Object)
{
	OnComplete(UEAIIntegration::Transport::MakeJsonResponse(StatusCode, Object));
}

void FUEAIIntegrationServer::SendSuccess(
	const FHttpResultCallback& OnComplete,
	const TSharedPtr<FJsonObject>& Data,
	int32 StatusCode)
{
	SendJsonResponse(
		OnComplete,
		StatusCode,
		UEAIIntegration::Transport::MakeSuccessEnvelope(Data));
}

void FUEAIIntegrationServer::SendError(
	const FHttpResultCallback& OnComplete,
	int32 StatusCode,
	const FString& Code,
	const FString& Message,
	const TSharedPtr<FJsonObject>& Details)
{
	SendJsonResponse(
		OnComplete,
		StatusCode,
		UEAIIntegration::Transport::MakeErrorEnvelope(Code, Message, Details));
}

TSharedPtr<FJsonObject> FUEAIIntegrationServer::MakeValidationDetails(
	const TArray<FString>& Errors)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Errors.Num());
	for (const FString& Error : Errors)
	{
		Values.Add(MakeShared<FJsonValueString>(Error));
	}

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetArrayField(TEXT("validationErrors"), Values);
	return Details;
}
