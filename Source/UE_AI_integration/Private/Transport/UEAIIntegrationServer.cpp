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

namespace
{
bool TryParseQueryBool(const TMap<FString, FString>& QueryParams, const FString& Name,
                       TOptional<bool>& OutValue, FString& OutError)
{
	const FString* RawValue = QueryParams.Find(Name);
	if (!RawValue)
	{
		return true;
	}
	if (*RawValue == TEXT("true"))
	{
		OutValue = true;
		return true;
	}
	if (*RawValue == TEXT("false"))
	{
		OutValue = false;
		return true;
	}
	OutError = FString::Printf(TEXT("Query parameter '%s' must be true or false."), *Name);
	return false;
}

bool TryParseQueryInteger(const TMap<FString, FString>& QueryParams, const FString& Name,
                          int32 DefaultValue, int32 MinValue, int32 MaxValue, int32& OutValue,
                          FString& OutError)
{
	OutValue = DefaultValue;
	const FString* RawValue = QueryParams.Find(Name);
	if (!RawValue)
	{
		return true;
	}
	if (!RawValue->IsNumeric())
	{
		OutError = FString::Printf(TEXT("Query parameter '%s' must be an integer."), *Name);
		return false;
	}
	const int64 Parsed = FCString::Atoi64(**RawValue);
	if (Parsed < MinValue || Parsed > MaxValue)
	{
		OutError = FString::Printf(TEXT("Query parameter '%s' must be between %d and %d."), *Name,
		                           MinValue, MaxValue);
		return false;
	}
	OutValue = static_cast<int32>(Parsed);
	return true;
}

bool DescriptorMatchesTrait(const TSharedPtr<FJsonObject>& Descriptor, const FString& TraitName,
                            const TOptional<bool>& Expected)
{
	if (!Expected.IsSet())
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* Traits = nullptr;
	bool Actual = false;
	return Descriptor.IsValid() && Descriptor->TryGetObjectField(TEXT("traits"), Traits) &&
	       Traits && Traits->IsValid() && (*Traits)->TryGetBoolField(TraitName, Actual) &&
	       Actual == Expected.GetValue();
}

TSharedPtr<FJsonObject> MakeCapabilitySummary(const TSharedPtr<FJsonObject>& Descriptor)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	static const TArray<FString> SummaryFields = {
	    TEXT("id"),          TEXT("domain"), TEXT("kind"),
	    TEXT("description"), TEXT("traits"), TEXT("output"),
	};
	for (const FString& FieldName : SummaryFields)
	{
		if (const TSharedPtr<FJsonValue>* Value = Descriptor->Values.Find(FieldName))
		{
			Summary->SetField(FieldName, *Value);
		}
	}
	return Summary;
}
} // namespace

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
	static const TSet<FString> SupportedQueryParams = {
	    TEXT("query"),    TEXT("domain"),      TEXT("operation"), TEXT("kind"),
	    TEXT("readOnly"), TEXT("destructive"), TEXT("expensive"), TEXT("outputKind"),
	    TEXT("offset"),   TEXT("limit"),       TEXT("detail"),
	};
	for (const TPair<FString, FString>& QueryParam : Request.QueryParams)
	{
		if (!SupportedQueryParams.Contains(QueryParam.Key))
		{
			SendError(
			    OnComplete, 422, TEXT("invalid_params"),
			    FString::Printf(TEXT("Unknown capability query parameter '%s'."), *QueryParam.Key));
			return true;
		}
	}

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

	FString KindFilter;
	if (const FString* KindValue = Request.QueryParams.Find(TEXT("kind")))
	{
		KindFilter = *KindValue;
		if (KindFilter != TEXT("query") && KindFilter != TEXT("command") &&
		    KindFilter != TEXT("validation"))
		{
			SendError(OnComplete, 422, TEXT("invalid_params"),
			          TEXT("Query parameter 'kind' must be query, command, or validation."));
			return true;
		}
	}

	FString OutputKindFilter;
	if (const FString* OutputKindValue = Request.QueryParams.Find(TEXT("outputKind")))
	{
		OutputKindFilter = *OutputKindValue;
		if (OutputKindFilter != TEXT("json") && OutputKindFilter != TEXT("image"))
		{
			SendError(OnComplete, 422, TEXT("invalid_params"),
			          TEXT("Query parameter 'outputKind' must be json or image."));
			return true;
		}
	}

	TOptional<bool> ReadOnlyFilter;
	TOptional<bool> DestructiveFilter;
	TOptional<bool> ExpensiveFilter;
	FString ParseError;
	if (!TryParseQueryBool(Request.QueryParams, TEXT("readOnly"), ReadOnlyFilter, ParseError) ||
	    !TryParseQueryBool(Request.QueryParams, TEXT("destructive"), DestructiveFilter,
	                       ParseError) ||
	    !TryParseQueryBool(Request.QueryParams, TEXT("expensive"), ExpensiveFilter, ParseError))
	{
		SendError(OnComplete, 422, TEXT("invalid_params"), ParseError);
		return true;
	}

	int32 Offset = 0;
	int32 Limit = 25;
	if (!TryParseQueryInteger(Request.QueryParams, TEXT("offset"), 0, 0, MAX_int32, Offset,
	                          ParseError) ||
	    !TryParseQueryInteger(Request.QueryParams, TEXT("limit"), 25, 1, 100, Limit, ParseError))
	{
		SendError(OnComplete, 422, TEXT("invalid_params"), ParseError);
		return true;
	}

	FString Detail = TEXT("summary");
	if (const FString* DetailValue = Request.QueryParams.Find(TEXT("detail")))
	{
		Detail = *DetailValue;
		if (Detail != TEXT("summary") && Detail != TEXT("full"))
		{
			SendError(OnComplete, 422, TEXT("invalid_params"),
			          TEXT("Query parameter 'detail' must be summary or full."));
			return true;
		}
	}

	const FString SearchQuery = Request.QueryParams.FindRef(TEXT("query")).TrimStartAndEnd();
	const FString ExactOperation = Request.QueryParams.FindRef(TEXT("operation")).TrimStartAndEnd();
	if (Request.QueryParams.Contains(TEXT("operation")) && ExactOperation.IsEmpty())
	{
		SendError(OnComplete, 422, TEXT("invalid_params"),
		          TEXT("Query parameter 'operation' must be non-empty."));
		return true;
	}

	TArray<TSharedPtr<FJsonObject>> FilteredDescriptors;
	FilteredDescriptors.Reserve(Registry.GetCapabilityDescriptors().Num());
	for (const TSharedPtr<FJsonObject>& Descriptor : Registry.GetCapabilityDescriptors())
	{
		if (!Descriptor.IsValid())
		{
			continue;
		}
		const FString Id = Descriptor->GetStringField(TEXT("id"));
		const FString Domain = Descriptor->GetStringField(TEXT("domain"));
		const FString Kind = Descriptor->GetStringField(TEXT("kind"));
		const FString Description = Descriptor->GetStringField(TEXT("description"));
		if ((!ExactOperation.IsEmpty() && Id != ExactOperation) ||
		    (!DomainFilter.IsEmpty() && Domain != DomainFilter) ||
		    (!KindFilter.IsEmpty() && Kind != KindFilter) ||
		    (!SearchQuery.IsEmpty() && !Id.Contains(SearchQuery, ESearchCase::IgnoreCase) &&
		     !Description.Contains(SearchQuery, ESearchCase::IgnoreCase)) ||
		    !DescriptorMatchesTrait(Descriptor, TEXT("readOnly"), ReadOnlyFilter) ||
		    !DescriptorMatchesTrait(Descriptor, TEXT("destructive"), DestructiveFilter) ||
		    !DescriptorMatchesTrait(Descriptor, TEXT("expensive"), ExpensiveFilter))
		{
			continue;
		}
		if (!OutputKindFilter.IsEmpty())
		{
			const TSharedPtr<FJsonObject>* Output = nullptr;
			if (!Descriptor->TryGetObjectField(TEXT("output"), Output) || !Output ||
			    !Output->IsValid() || (*Output)->GetStringField(TEXT("kind")) != OutputKindFilter)
			{
				continue;
			}
		}
		FilteredDescriptors.Add(Descriptor);
	}
	FilteredDescriptors.Sort(
	    [](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	    { return Left->GetStringField(TEXT("id")) < Right->GetStringField(TEXT("id")); });

	if (!ExactOperation.IsEmpty() && FilteredDescriptors.IsEmpty())
	{
		SendError(OnComplete, 404, TEXT("capability_not_found"),
		          FString::Printf(TEXT("Unknown capability '%s'."), *ExactOperation));
		return true;
	}

	const int32 Total = FilteredDescriptors.Num();
	if (!ExactOperation.IsEmpty())
	{
		Offset = 0;
		Limit = 1;
		Detail = TEXT("full");
	}
	const int32 PageEnd = Offset >= Total ? Total : FMath::Min(Total, Offset + Limit);
	TArray<TSharedPtr<FJsonValue>> CapabilityValues;
	CapabilityValues.Reserve(FMath::Max(0, PageEnd - Offset));
	for (int32 Index = Offset; Index < PageEnd; ++Index)
	{
		const TSharedPtr<FJsonObject> Descriptor = FilteredDescriptors[Index];
		CapabilityValues.Add(MakeShared<FJsonValueObject>(
		    Detail == TEXT("full") ? Descriptor : MakeCapabilitySummary(Descriptor)));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("capabilities"), CapabilityValues);
	Data->SetNumberField(TEXT("total"), Total);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetBoolField(TEXT("hasMore"), PageEnd < Total);
	Data->SetStringField(TEXT("detail"), Detail);
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
