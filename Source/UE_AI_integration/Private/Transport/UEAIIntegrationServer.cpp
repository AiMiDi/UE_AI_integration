#include "UEAIIntegrationServer.h"

#include "Core/MCPExecutor.h"
#include "Dom/JsonValue.h"
#include "HttpServerModule.h"
#include "Interfaces/IPluginManager.h"
#include "Infrastructure/ClientActivityService.h"
#include "Infrastructure/OptionalFeatureAvailability.h"
#include "Infrastructure/Runtime/BlueprintDebugService.h"
#include "HAL/PlatformProperties.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tools/MCPToolRegistry.h"
#include "Transport/MCPHttpContract.h"
#include "UEWorkflowCore/WorkflowCore.h"
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

TSharedPtr<FJsonObject> DecorateCapabilityAvailability(
	const TSharedPtr<FJsonObject>& Descriptor)
{
	TSharedPtr<FJsonObject> Decorated = MakeShared<FJsonObject>();
	if (Descriptor.IsValid())
	{
		Decorated->Values = Descriptor->Values;
	}

	TArray<TSharedPtr<FJsonValue>> Reasons;
	for (const FString& Reason :
		UEAIIntegration::Infrastructure::GetCapabilityUnavailableReasons(
			Descriptor))
	{
		Reasons.Add(MakeShared<FJsonValueString>(Reason));
	}

	Decorated->SetBoolField(TEXT("available"), Reasons.IsEmpty());
	Decorated->SetArrayField(TEXT("availabilityReasons"), Reasons);
	return Decorated;
}

TSharedPtr<FJsonObject> MakeCapabilitySummary(const TSharedPtr<FJsonObject>& Descriptor)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	static const TArray<FString> SummaryFields = {
	    TEXT("id"),          TEXT("domain"), TEXT("kind"),
	    TEXT("description"), TEXT("traits"), TEXT("output"), TEXT("requires"),
	    TEXT("available"), TEXT("availabilityReasons"), TEXT("match"),
	};
	for (const FString& FieldName : SummaryFields)
	{
		if (const TSharedPtr<FJsonValue>* Value = Descriptor->Values.Find(FieldName))
		{
			Summary->SetField(FieldName, *Value);
		}
	}
	const TSharedPtr<FJsonObject>* Dsl = nullptr;
	FString Risk;
	if (Descriptor->TryGetObjectField(TEXT("dsl"), Dsl)
		&& Dsl
		&& Dsl->IsValid()
		&& (*Dsl)->TryGetStringField(TEXT("risk"), Risk))
	{
		Summary->SetStringField(TEXT("risk"), Risk);
	}
	return Summary;
}

std::string ToUtf8String(const FString& Value)
{
	const FTCHARToUTF8 Converted(*Value);
	return std::string(Converted.Get(), Converted.Length());
}

ue::workflow::CapabilitySearchDocument MakeCapabilitySearchDocument(
	const TSharedPtr<FJsonObject>& Descriptor)
{
	ue::workflow::CapabilitySearchDocument Document;
	if (!Descriptor.IsValid())
	{
		return Document;
	}
	FString Value;
	if (Descriptor->TryGetStringField(TEXT("id"), Value))
	{
		Document.id = ToUtf8String(Value);
	}
	if (Descriptor->TryGetStringField(TEXT("description"), Value))
	{
		Document.description = ToUtf8String(Value);
	}

	const TSharedPtr<FJsonObject>* Search = nullptr;
	if (!Descriptor->TryGetObjectField(TEXT("search"), Search)
		|| !Search
		|| !Search->IsValid())
	{
		return Document;
	}
	if ((*Search)->TryGetStringField(TEXT("title"), Value))
	{
		Document.title = ToUtf8String(Value);
	}
	const auto ReadArray =
		[Search](const TCHAR* FieldName)
	{
		std::vector<std::string> Values;
		if (!(*Search)->HasTypedField<EJson::Array>(FieldName))
		{
			return Values;
		}
		for (const TSharedPtr<FJsonValue>& Item :
			(*Search)->GetArrayField(FieldName))
		{
			if (Item.IsValid() && Item->Type == EJson::String)
			{
				Values.push_back(ToUtf8String(Item->AsString()));
			}
		}
		return Values;
	};
	Document.keywords = ReadArray(TEXT("keywords"));
	Document.aliases = ReadArray(TEXT("aliases"));
	return Document;
}

TSharedPtr<FJsonObject> MakeCapabilitySearchMatch(
	const ue::workflow::CapabilitySearchMatch& Match)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("score"), Match.score);
	TArray<TSharedPtr<FJsonValue>> MatchedFields;
	for (const std::string& Field : Match.matched_fields)
	{
		MatchedFields.Add(MakeShared<FJsonValueString>(
			UTF8_TO_TCHAR(Field.c_str())));
	}
	TArray<TSharedPtr<FJsonValue>> MatchedTokens;
	for (const std::string& Token : Match.matched_tokens)
	{
		MatchedTokens.Add(MakeShared<FJsonValueString>(
			UTF8_TO_TCHAR(Token.c_str())));
	}
	Result->SetArrayField(TEXT("matchedFields"), MatchedFields);
	Result->SetArrayField(TEXT("matchedTokens"), MatchedTokens);
	return Result;
}

bool IsBlueprintDebugCapability(const FString& Capability)
{
	static const TSet<FString> Capabilities = {
		TEXT("blueprint.debug.session.get"),
		TEXT("blueprint.debug.trace.get"),
		TEXT("blueprint.debug.breakpoint.list"),
		TEXT("blueprint.debug.breakpoint.set"),
		TEXT("blueprint.debug.breakpoint.remove"),
		TEXT("blueprint.debug.watch.list"),
		TEXT("blueprint.debug.watch.set"),
		TEXT("blueprint.debug.watch.remove"),
		TEXT("blueprint.debug.watch.value.get"),
		TEXT("blueprint.debug.control"),
	};
	return Capabilities.Contains(Capability);
}

FString RequestBodyToString(const FHttpServerRequest& Request)
{
	if (Request.Body.IsEmpty())
	{
		return FString();
	}
	const FUTF8ToTCHAR Converter(
		reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()),
		Request.Body.Num());
	return FString(Converter.Length(), Converter.Get());
}

FString FindHeaderValue(
	const FHttpServerRequest& Request,
	const FString& HeaderName)
{
	for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
	{
		if (Pair.Key.Equals(HeaderName, ESearchCase::IgnoreCase)
			&& !Pair.Value.IsEmpty())
		{
			return Pair.Value[0].TrimStartAndEnd();
		}
	}
	return FString();
}

uint32 ParseProcessId(const FString& Value)
{
	if (Value.IsEmpty() || !Value.IsNumeric())
	{
		return 0;
	}
	const uint64 Parsed = FCString::Strtoui64(*Value, nullptr, 10);
	return Parsed <= MAX_uint32 ? static_cast<uint32>(Parsed) : 0;
}

UEAIIntegration::Infrastructure::FCallerContext ParseCallerContext(
	const FHttpServerRequest& Request)
{
	using UEAIIntegration::Infrastructure::FCallerContext;
	FCallerContext Caller;
	const FString CallerType =
		FindHeaderValue(Request, TEXT("X-UEAI-Caller-Type"));
	if (!CallerType.IsEmpty())
	{
		Caller.ClientKind = CallerType;
	}
	const FString CallerName =
		FindHeaderValue(Request, TEXT("X-UEAI-Caller"));
	if (!CallerName.IsEmpty())
	{
		Caller.Name = CallerName;
	}
	Caller.Version =
		FindHeaderValue(Request, TEXT("X-UEAI-Caller-Version"));
	Caller.InstanceId =
		FindHeaderValue(Request, TEXT("X-UEAI-Instance-Id"));
	Caller.InvocationId =
		FindHeaderValue(Request, TEXT("X-UEAI-Invocation-Id"));
	Caller.SessionId =
		FindHeaderValue(Request, TEXT("X-UEAI-Session-Id"));
	const FString Transport =
		FindHeaderValue(Request, TEXT("X-UEAI-Transport"));
	if (!Transport.IsEmpty())
	{
		Caller.Transport = Transport;
	}
	Caller.Command = FindHeaderValue(Request, TEXT("X-UEAI-Command"));
	Caller.Pid = ParseProcessId(
		FindHeaderValue(Request, TEXT("X-UEAI-Process-Id")));
	return Caller;
}

FString FindCapabilityRisk(
	const FMCPToolRegistry& Registry,
	const FString& Capability)
{
	for (const TSharedPtr<FJsonObject>& Descriptor :
		Registry.GetCapabilityDescriptors())
	{
		FString Id;
		if (!Descriptor.IsValid()
			|| !Descriptor->TryGetStringField(TEXT("id"), Id)
			|| Id != Capability)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Dsl = nullptr;
		FString Risk;
		if (Descriptor->TryGetObjectField(TEXT("dsl"), Dsl)
			&& Dsl && Dsl->IsValid())
		{
			(*Dsl)->TryGetStringField(TEXT("risk"), Risk);
		}
		return Risk;
	}
	return FString();
}

bool DeserializeRequestObject(
	const FHttpServerRequest& Request,
	TSharedPtr<FJsonObject>& OutObject)
{
	const FString Body = RequestBodyToString(Request);
	if (Body.IsEmpty())
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Body);
	return FJsonSerializer::Deserialize(Reader, OutObject)
		&& OutObject.IsValid();
}
} // namespace

FUEAIIntegrationServer::FUEAIIntegrationServer(
	FMCPToolRegistry& InRegistry,
	FMCPExecutor& InExecutor,
	UEAIIntegration::Infrastructure::FClientActivityService&
		InClientActivityService,
	UEAIIntegration::Infrastructure::FBlueprintDebugService*
		InBlueprintDebugService)
	: Registry(InRegistry)
	, Executor(InExecutor)
	, ClientActivityService(InClientActivityService)
	, BlueprintDebugService(InBlueprintDebugService)
	, WorkflowRuntime(
		MakeUnique<UEAIIntegration::Workflow::FWorkflowRuntime>(InRegistry))
{
}

FUEAIIntegrationServer::~FUEAIIntegrationServer()
{
	Stop();
}

FMCPResult FUEAIIntegrationServer::PlanWorkflowDefinition(
	const TSharedPtr<FJsonObject>& Workflow) const
{
	if (!WorkflowRuntime.IsValid() || !Workflow.IsValid())
	{
		return FMCPResult::Fail(
			TEXT("workflow_runtime_unavailable"),
			TEXT("The Editor Workflow planner is unavailable."),
			503);
	}
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("action"), TEXT("plan"));
	Request->SetStringField(TEXT("detailLevel"), TEXT("standard"));
	Request->SetObjectField(TEXT("workflow"), Workflow);
	return WorkflowRuntime->HandleRequest(Request);
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
			return HandleCallerObserved(
				Request,
				OnComplete,
				[this, &Request](const FHttpResultCallback& ObservedComplete)
				{
					return HandleHealth(Request, ObservedComplete);
				});
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
			return HandleCallerObserved(
				Request,
				OnComplete,
				[this, &Request](const FHttpResultCallback& ObservedComplete)
				{
					return HandleCapabilities(Request, ObservedComplete);
				});
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

	const FHttpRouteHandle RegisterClientRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/v1/clients/register")),
		EHttpServerRequestVerbs::VERB_POST,
		[this](
			const FHttpServerRequest& Request,
			const FHttpResultCallback& OnComplete)
		{
			return HandleClientRegister(Request, OnComplete);
		});
	if (!RegisterClientRoute.IsValid())
	{
		UE_LOG(
			LogUEAIIntegrationServer,
			Error,
			TEXT("Failed to bind /api/v1/clients/register."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(RegisterClientRoute);

	const FHttpRouteHandle HeartbeatClientRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/v1/clients/heartbeat")),
		EHttpServerRequestVerbs::VERB_POST,
		[this](
			const FHttpServerRequest& Request,
			const FHttpResultCallback& OnComplete)
		{
			return HandleClientHeartbeat(Request, OnComplete);
		});
	if (!HeartbeatClientRoute.IsValid())
	{
		UE_LOG(
			LogUEAIIntegrationServer,
			Error,
			TEXT("Failed to bind /api/v1/clients/heartbeat."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(HeartbeatClientRoute);

	const FHttpRouteHandle UnregisterClientRoute = Router->BindRoute(
		FHttpPath(TEXT("/api/v1/clients/unregister")),
		EHttpServerRequestVerbs::VERB_POST,
		[this](
			const FHttpServerRequest& Request,
			const FHttpResultCallback& OnComplete)
		{
			return HandleClientUnregister(Request, OnComplete);
		});
	if (!UnregisterClientRoute.IsValid())
	{
		UE_LOG(
			LogUEAIIntegrationServer,
			Error,
			TEXT("Failed to bind /api/v1/clients/unregister."));
		UnbindRoutes();
		Router.Reset();
		return false;
	}
	RouteHandles.Add(UnregisterClientRoute);

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
			if (!Pending->ActivityId.IsEmpty())
			{
				ClientActivityService.MarkActivityRejected(
					Pending->ActivityId);
			}
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
	if (!Pending->ActivityId.IsEmpty())
	{
		ClientActivityService.MarkActivityStarted(Pending->ActivityId);
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
		FString Action;
		RequestObject->TryGetStringField(TEXT("action"), Action);
		ClientActivityService.UpdateWorkflowActivity(
			Pending->ActivityId,
			Action,
			FString(),
			TEXT("workflow"));
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
	ClientActivityService.UpdateCapabilityActivity(
		Pending->ActivityId,
		CapabilityId,
		FString(),
		FindCapabilityRisk(Registry, CapabilityId));

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
	ClientActivityService.UpdateCapabilityActivity(
		Pending->ActivityId,
		CapabilityId,
		RequestId,
		FindCapabilityRisk(Registry, CapabilityId));

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
	int32 AvailableCapabilityCount = 0;
	for (const TSharedPtr<FJsonObject>& Descriptor :
		Registry.GetCapabilityDescriptors())
	{
		if (UEAIIntegration::Infrastructure::GetCapabilityUnavailableReasons(
				Descriptor).IsEmpty())
		{
			++AvailableCapabilityCount;
		}
	}
	Data->SetNumberField(
		TEXT("availableCapabilityCount"),
		AvailableCapabilityCount);
	TSharedPtr<FJsonObject> OptionalFeatures = MakeShared<FJsonObject>();
	for (const TCHAR* Feature : {TEXT("Niagara"), TEXT("Water"), TEXT("PCG")})
	{
		OptionalFeatures->SetBoolField(
			Feature,
			UEAIIntegration::Infrastructure::IsOptionalFeatureCompiled(Feature));
	}
	Data->SetObjectField(TEXT("optionalFeatures"), OptionalFeatures);
	Data->SetNumberField(
		TEXT("onlineMcpCount"),
		ClientActivityService.GetOnlineMcpCount());
	Data->SetNumberField(
		TEXT("activeCliCount"),
		ClientActivityService.GetRunningCliCount());

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
	    TEXT("risk"),     TEXT("availableOnly"),
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

	FString RiskFilter;
	if (const FString* RiskValue = Request.QueryParams.Find(TEXT("risk")))
	{
		RiskFilter = *RiskValue;
		if (RiskFilter != TEXT("readOnly")
			&& RiskFilter != TEXT("safeWrite")
			&& RiskFilter != TEXT("confirmWrite")
			&& RiskFilter != TEXT("notOpen"))
		{
			SendError(
				OnComplete,
				422,
				TEXT("invalid_params"),
				TEXT(
					"Query parameter 'risk' must be readOnly, safeWrite, confirmWrite, or notOpen."));
			return true;
		}
	}

	TOptional<bool> ReadOnlyFilter;
	TOptional<bool> DestructiveFilter;
	TOptional<bool> ExpensiveFilter;
	TOptional<bool> AvailableOnlyFilter;
	FString ParseError;
	if (!TryParseQueryBool(Request.QueryParams, TEXT("readOnly"), ReadOnlyFilter, ParseError) ||
	    !TryParseQueryBool(Request.QueryParams, TEXT("destructive"), DestructiveFilter,
	                       ParseError) ||
	    !TryParseQueryBool(Request.QueryParams, TEXT("expensive"), ExpensiveFilter, ParseError) ||
	    !TryParseQueryBool(
		    Request.QueryParams, TEXT("availableOnly"), AvailableOnlyFilter, ParseError))
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

	struct FRankedCapability
	{
		TSharedPtr<FJsonObject> Descriptor;
		TOptional<ue::workflow::CapabilitySearchMatch> Match;
	};
	TArray<FRankedCapability> FilteredDescriptors;
	FilteredDescriptors.Reserve(Registry.GetCapabilityDescriptors().Num());
	for (const TSharedPtr<FJsonObject>& Descriptor : Registry.GetCapabilityDescriptors())
	{
		if (!Descriptor.IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject> DecoratedDescriptor =
			DecorateCapabilityAvailability(Descriptor);
		const FString Id = DecoratedDescriptor->GetStringField(TEXT("id"));
		const FString Domain = DecoratedDescriptor->GetStringField(TEXT("domain"));
		const FString Kind = DecoratedDescriptor->GetStringField(TEXT("kind"));

		FString DescriptorRisk;
		const TSharedPtr<FJsonObject>* Dsl = nullptr;
		if (DecoratedDescriptor->TryGetObjectField(TEXT("dsl"), Dsl)
			&& Dsl
			&& Dsl->IsValid())
		{
			(*Dsl)->TryGetStringField(TEXT("risk"), DescriptorRisk);
		}

		bool bAvailable = true;
		DecoratedDescriptor->TryGetBoolField(TEXT("available"), bAvailable);
		if ((!ExactOperation.IsEmpty() && Id != ExactOperation) ||
		    (!DomainFilter.IsEmpty() && Domain != DomainFilter) ||
		    (!KindFilter.IsEmpty() && Kind != KindFilter) ||
		    (!RiskFilter.IsEmpty() && DescriptorRisk != RiskFilter) ||
		    (AvailableOnlyFilter.Get(false) && !bAvailable) ||
		    !DescriptorMatchesTrait(
			    DecoratedDescriptor, TEXT("readOnly"), ReadOnlyFilter) ||
		    !DescriptorMatchesTrait(
			    DecoratedDescriptor, TEXT("destructive"), DestructiveFilter) ||
		    !DescriptorMatchesTrait(
			    DecoratedDescriptor, TEXT("expensive"), ExpensiveFilter))
		{
			continue;
		}
		if (!OutputKindFilter.IsEmpty())
		{
			const TSharedPtr<FJsonObject>* Output = nullptr;
			if (!DecoratedDescriptor->TryGetObjectField(TEXT("output"), Output) || !Output ||
			    !Output->IsValid() || (*Output)->GetStringField(TEXT("kind")) != OutputKindFilter)
			{
				continue;
			}
		}
		TOptional<ue::workflow::CapabilitySearchMatch> SearchMatch;
		if (!SearchQuery.IsEmpty())
		{
			const std::optional<ue::workflow::CapabilitySearchMatch> Match =
				ue::workflow::MatchCapabilitySearch(
					ToUtf8String(SearchQuery),
					MakeCapabilitySearchDocument(DecoratedDescriptor));
			if (!Match)
			{
				continue;
			}
			SearchMatch = *Match;
			DecoratedDescriptor->SetObjectField(
				TEXT("match"),
				MakeCapabilitySearchMatch(*Match));
		}
		FilteredDescriptors.Add({
			DecoratedDescriptor,
			MoveTemp(SearchMatch),
		});
	}
	FilteredDescriptors.Sort(
	    [&SearchQuery](
		    const FRankedCapability& Left,
		    const FRankedCapability& Right)
	    {
		    if (!SearchQuery.IsEmpty()
			    && Left.Match.GetValue().score
				    != Right.Match.GetValue().score)
		    {
			    return Left.Match.GetValue().score
				    > Right.Match.GetValue().score;
		    }
		    return Left.Descriptor->GetStringField(TEXT("id"))
			    < Right.Descriptor->GetStringField(TEXT("id"));
	    });

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
		const TSharedPtr<FJsonObject> Descriptor =
			FilteredDescriptors[Index].Descriptor;
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
	// A Kismet breakpoint blocks the normal subsystem Tick inside Slate's
	// intra-frame debugging loop. Only Blueprint debug operations are allowed
	// to bypass that queue, and the service guarantees this path never touches
	// UObject state: it reads a copied snapshot or enqueues a value-only command
	// consumed by Slate pre-tick.
	if (BlueprintDebugService
		&& BlueprintDebugService->IsPausedTransportActive())
	{
		UEAIIntegration::Infrastructure::FCallerContext Caller =
			ParseCallerContext(Request);
		FString CallerError;
		if (!ClientActivityService.BeginRequest(
				Caller,
				CallerError))
		{
			SendError(
				OnComplete,
				401,
				TEXT("client_session_expired"),
				CallerError);
			return true;
		}
		const FString ActivityId = ClientActivityService.BeginActivity(
			Caller,
			TEXT("capability"));
		ClientActivityService.MarkActivityStarted(ActivityId);
		const FHttpResultCallback Original = OnComplete;
		const FHttpResultCallback EffectiveOnComplete =
			[this, ActivityId, Caller, Original](
				TUniquePtr<FHttpServerResponse>&& Response) mutable
			{
				if (Response.IsValid())
				{
					ClientActivityService.CompleteActivityFromHttp(
						ActivityId,
						static_cast<int32>(Response->Code),
						Response->Body);
				}
				ClientActivityService.EndRequest(Caller);
				Original(MoveTemp(Response));
			};
		FString Body;
		if (Request.Body.Num() > 0)
		{
			const FUTF8ToTCHAR Converter(
				reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()),
				Request.Body.Num());
			Body = FString(Converter.Length(), Converter.Get());
		}
		TSharedPtr<FJsonObject> RequestObject;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, RequestObject)
			|| !RequestObject.IsValid())
		{
			SendError(
				EffectiveOnComplete,
				400,
				TEXT("invalid_json"),
				TEXT("Request body contains invalid JSON."));
			return true;
		}

		FString Capability;
		if (RequestObject->TryGetStringField(TEXT("capability"), Capability)
			&& IsBlueprintDebugCapability(Capability))
		{
			ClientActivityService.UpdateCapabilityActivity(
				ActivityId,
				Capability,
				FString(),
				FindCapabilityRisk(Registry, Capability));
			if (!Registry.IsReady())
			{
				SendError(
					EffectiveOnComplete,
					503,
					TEXT("service_degraded"),
					TEXT("Capability bindings failed validation."),
					MakeValidationDetails(Registry.GetValidationErrors()));
				return true;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
				RequestObject->Values)
			{
				if (Field.Key != TEXT("capability")
					&& Field.Key != TEXT("params")
					&& Field.Key != TEXT("requestId"))
				{
					SendError(
						EffectiveOnComplete,
						422,
						TEXT("invalid_params"),
						FString::Printf(
							TEXT("Unknown request field '%s'."),
							*Field.Key));
					return true;
				}
			}

			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			if (RequestObject->HasField(TEXT("params")))
			{
				if (!RequestObject->HasTypedField<EJson::Object>(TEXT("params")))
				{
					SendError(
						EffectiveOnComplete,
						422,
						TEXT("invalid_params"),
						TEXT("Field 'params' must be a JSON object."));
					return true;
				}
				Params = RequestObject->GetObjectField(TEXT("params"));
			}
			TArray<FString> ValidationErrors;
			if (!Registry.ValidateParams(Capability, Params, ValidationErrors))
			{
				SendError(
					EffectiveOnComplete,
					422,
					TEXT("invalid_params"),
					TEXT("Capability parameters failed manifest schema validation."),
					MakeValidationDetails(ValidationErrors));
				return true;
			}

			FString RequestId;
			if (RequestObject->HasField(TEXT("requestId"))
				&& (!RequestObject->TryGetStringField(TEXT("requestId"), RequestId)
					|| RequestId.IsEmpty()
					|| RequestId.Len() > 200))
			{
				SendError(
					EffectiveOnComplete,
					422,
					TEXT("invalid_params"),
					TEXT(
						"Field 'requestId' must be a non-empty string of at most 200 characters."));
				return true;
			}
			ClientActivityService.UpdateCapabilityActivity(
				ActivityId,
				Capability,
				RequestId,
				FindCapabilityRisk(Registry, Capability));

			UEAIIntegration::Infrastructure::FBlueprintDebugResult Result;
			if (BlueprintDebugService->TryHandlePausedRequest(
					Capability,
					Params,
					RequestId,
					Result))
			{
				if (Result.bSuccess)
				{
					SendSuccess(EffectiveOnComplete, Result.Data);
				}
				else
				{
					SendError(
						EffectiveOnComplete,
						Result.HttpStatus,
						Result.ErrorCode,
						Result.ErrorMessage);
				}
				return true;
			}
		}
		SendError(
			EffectiveOnComplete,
			423,
			TEXT("debug_session_paused"),
			TEXT(
				"Only Blueprint debug snapshot queries and POD control commands "
				"are available while Kismet is paused."));
		return true;
	}
	return QueueRequest(
		EUEAIIntegrationRequestKind::LegacyExecute,
		Request,
		OnComplete);
}

bool FUEAIIntegrationServer::HandleWorkflowHandshake(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	if (BlueprintDebugService
		&& BlueprintDebugService->IsPausedTransportActive())
	{
		return HandleCallerObserved(
			Request,
			OnComplete,
			[this](const FHttpResultCallback& ObservedComplete)
			{
				SendError(
					ObservedComplete,
					423,
					TEXT("debug_session_paused"),
					TEXT(
						"Workflow routes are unavailable while Kismet is paused."));
				return true;
			});
	}
	return QueueRequest(
		EUEAIIntegrationRequestKind::WorkflowHandshake,
		Request,
		OnComplete);
}

bool FUEAIIntegrationServer::HandleWorkflow(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	if (BlueprintDebugService
		&& BlueprintDebugService->IsPausedTransportActive())
	{
		UEAIIntegration::Infrastructure::FCallerContext Caller =
			ParseCallerContext(Request);
		FString CallerError;
		if (!ClientActivityService.BeginRequest(Caller, CallerError))
		{
			SendError(
				OnComplete,
				401,
				TEXT("client_session_expired"),
				CallerError);
			return true;
		}
		const FString ActivityId = ClientActivityService.BeginActivity(
			Caller,
			TEXT("workflow"));
		ClientActivityService.MarkActivityStarted(ActivityId);
		TSharedPtr<FJsonObject> RequestObject;
		const FString Body = RequestBodyToString(Request);
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Body);
		if (FJsonSerializer::Deserialize(Reader, RequestObject)
			&& RequestObject.IsValid())
		{
			FString Action;
			FString RequestId;
			RequestObject->TryGetStringField(TEXT("action"), Action);
			RequestObject->TryGetStringField(TEXT("requestId"), RequestId);
			ClientActivityService.UpdateWorkflowActivity(
				ActivityId,
				Action,
				RequestId,
				TEXT("interactive"));
		}
		const FHttpResultCallback Original = OnComplete;
		const FHttpResultCallback ObservedComplete =
			[this, ActivityId, Caller, Original](
				TUniquePtr<FHttpServerResponse>&& Response) mutable
			{
				if (Response.IsValid())
				{
					ClientActivityService.CompleteActivityFromHttp(
						ActivityId,
						static_cast<int32>(Response->Code),
						Response->Body);
				}
				ClientActivityService.EndRequest(Caller);
				Original(MoveTemp(Response));
			};
		SendError(
			ObservedComplete,
			423,
			TEXT("debug_session_paused"),
			TEXT("Workflow routes are unavailable while Kismet is paused."));
		return true;
	}
	return QueueRequest(
		EUEAIIntegrationRequestKind::WorkflowAction,
		Request,
		OnComplete);
}

bool FUEAIIntegrationServer::HandleClientRegister(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Object;
	if (!DeserializeRequestObject(Request, Object))
	{
		SendError(
			OnComplete,
			400,
			TEXT("invalid_json"),
			TEXT("Client registration body must be a JSON object."));
		return true;
	}
	static const TSet<FString> AllowedFields = {
		TEXT("clientKind"),
		TEXT("name"),
		TEXT("version"),
		TEXT("transport"),
		TEXT("pid"),
		TEXT("instanceId"),
		TEXT("invocationId"),
		TEXT("command"),
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		if (!AllowedFields.Contains(Field.Key))
		{
			SendError(
				OnComplete,
				422,
				TEXT("invalid_client_registration"),
				FString::Printf(
					TEXT("Unknown client registration field '%s'."),
					*Field.Key));
			return true;
		}
	}

	using UEAIIntegration::Infrastructure::FClientRegistration;
	FClientRegistration Registration;
	if (!Object->TryGetStringField(
			TEXT("clientKind"),
			Registration.ClientKind)
		|| !Object->TryGetStringField(TEXT("name"), Registration.Name)
		|| !Object->TryGetStringField(
			TEXT("instanceId"),
			Registration.InstanceId))
	{
		SendError(
			OnComplete,
			422,
			TEXT("invalid_client_registration"),
			TEXT("clientKind, name, and instanceId are required strings."));
		return true;
	}
	Object->TryGetStringField(TEXT("version"), Registration.Version);
	if (!Object->TryGetStringField(TEXT("transport"), Registration.Transport))
	{
		Registration.Transport = TEXT("http");
	}
	Object->TryGetStringField(
		TEXT("invocationId"),
		Registration.InvocationId);
	Object->TryGetStringField(TEXT("command"), Registration.Command);
	double ProcessId = 0.0;
	if (Object->HasField(TEXT("pid"))
		&& (!Object->TryGetNumberField(TEXT("pid"), ProcessId)
			|| ProcessId < 0.0
			|| ProcessId > static_cast<double>(MAX_uint32)
			|| FMath::FloorToDouble(ProcessId) != ProcessId))
	{
		SendError(
			OnComplete,
			422,
			TEXT("invalid_client_registration"),
			TEXT("pid must be an unsigned 32-bit integer."));
		return true;
	}
	Registration.Pid = static_cast<uint32>(ProcessId);

	FString SessionId;
	FString Error;
	if (!ClientActivityService.RegisterClient(
			Registration,
			SessionId,
			Error))
	{
		SendError(
			OnComplete,
			422,
			TEXT("invalid_client_registration"),
			Error);
		return true;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sessionId"), SessionId);
	Data->SetNumberField(
		TEXT("heartbeatIntervalMs"),
		UEAIIntegration::Infrastructure::FClientActivityService::
			HeartbeatIntervalMs);
	Data->SetNumberField(
		TEXT("expiresAfterMs"),
		UEAIIntegration::Infrastructure::FClientActivityService::
			ExpiresAfterMs);
	SendSuccess(OnComplete, Data);
	return true;
}

bool FUEAIIntegrationServer::HandleClientHeartbeat(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	const FString SessionId =
		FindHeaderValue(Request, TEXT("X-UEAI-Session-Id"));
	if (SessionId.IsEmpty())
	{
		SendError(
			OnComplete,
			422,
			TEXT("client_session_required"),
			TEXT("X-UEAI-Session-Id is required."));
		return true;
	}
	FString Error;
	if (!ClientActivityService.Heartbeat(SessionId, Error))
	{
		SendError(
			OnComplete,
			404,
			TEXT("client_session_not_found"),
			Error);
		return true;
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sessionId"), SessionId);
	Data->SetStringField(TEXT("status"), TEXT("online"));
	SendSuccess(OnComplete, Data);
	return true;
}

bool FUEAIIntegrationServer::HandleClientUnregister(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	const FString SessionId =
		FindHeaderValue(Request, TEXT("X-UEAI-Session-Id"));
	if (SessionId.IsEmpty())
	{
		SendError(
			OnComplete,
			422,
			TEXT("client_session_required"),
			TEXT("X-UEAI-Session-Id is required."));
		return true;
	}
	FString Error;
	if (!ClientActivityService.UnregisterClient(SessionId, Error))
	{
		const bool bBusy =
			Error == TEXT("Client session has active requests.");
		SendError(
			OnComplete,
			bBusy ? 409 : 404,
			bBusy
				? TEXT("client_session_busy")
				: TEXT("client_session_not_found"),
			Error);
		return true;
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sessionId"), SessionId);
	Data->SetStringField(TEXT("status"), TEXT("offline"));
	SendSuccess(OnComplete, Data);
	return true;
}

bool FUEAIIntegrationServer::HandleCallerObserved(
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete,
	TFunctionRef<bool(const FHttpResultCallback&)> Handler)
{
	TSharedPtr<UEAIIntegration::Infrastructure::FCallerContext> Caller =
		MakeShared<UEAIIntegration::Infrastructure::FCallerContext>(
			ParseCallerContext(Request));
	FString CallerError;
	if (!ClientActivityService.BeginRequest(*Caller, CallerError))
	{
		SendError(
			OnComplete,
			401,
			TEXT("client_session_expired"),
			CallerError);
		return true;
	}

	const FHttpResultCallback Original = OnComplete;
	const FHttpResultCallback ObservedComplete =
		[this, Caller, Original](
			TUniquePtr<FHttpServerResponse>&& Response) mutable
		{
			ClientActivityService.EndRequest(*Caller);
			Original(MoveTemp(Response));
		};
	const bool bHandled = Handler(ObservedComplete);
	if (!bHandled)
	{
		ClientActivityService.EndRequest(*Caller);
	}
	return bHandled;
}

bool FUEAIIntegrationServer::QueueRequest(
	EUEAIIntegrationRequestKind Kind,
	const FHttpServerRequest& Request,
	const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FPendingMCPExecuteRequest> Pending =
		MakeShared<FPendingMCPExecuteRequest>();
	Pending->Kind = Kind;
	Pending->Caller =
		MakeShared<UEAIIntegration::Infrastructure::FCallerContext>(
			ParseCallerContext(Request));
	FString CallerError;
	if (!ClientActivityService.BeginRequest(
			*Pending->Caller,
			CallerError))
	{
		SendError(
			OnComplete,
			401,
			TEXT("client_session_expired"),
			CallerError);
		return true;
	}

	if (!Request.Body.IsEmpty())
	{
		Pending->Body = RequestBodyToString(Request);
	}

	if (Kind == EUEAIIntegrationRequestKind::LegacyExecute
		|| Kind == EUEAIIntegrationRequestKind::WorkflowAction)
	{
		Pending->ActivityId = ClientActivityService.BeginActivity(
			*Pending->Caller,
			Kind == EUEAIIntegrationRequestKind::LegacyExecute
				? TEXT("capability")
				: TEXT("workflow"));
		const FString ActivityId = Pending->ActivityId;
		const TSharedPtr<
			UEAIIntegration::Infrastructure::FCallerContext> Caller =
				Pending->Caller;
		const FHttpResultCallback Original = OnComplete;
		Pending->OnComplete =
			[this, ActivityId, Caller, Original](
				TUniquePtr<FHttpServerResponse>&& Response) mutable
			{
				if (Response.IsValid())
				{
					ClientActivityService.CompleteActivityFromHttp(
						ActivityId,
						static_cast<int32>(Response->Code),
						Response->Body);
				}
				ClientActivityService.EndRequest(*Caller);
				Original(MoveTemp(Response));
			};
	}
	else
	{
		const TSharedPtr<
			UEAIIntegration::Infrastructure::FCallerContext> Caller =
				Pending->Caller;
		Pending->OnComplete =
			[this, Caller, OnComplete](
				TUniquePtr<FHttpServerResponse>&& Response) mutable
			{
				ClientActivityService.EndRequest(*Caller);
				OnComplete(MoveTemp(Response));
			};
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
