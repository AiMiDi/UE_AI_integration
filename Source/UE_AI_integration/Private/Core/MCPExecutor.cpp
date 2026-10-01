#include "Core/MCPExecutor.h"

#include "Dom/JsonValue.h"
#include "Infrastructure/ClientActivityService.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/OptionalFeatureAvailability.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
	constexpr int32 MaxIdempotencyRecords = 256;

	bool AllowsLegacyRequestIdOmission(const FString& Capability)
	{
		return Capability == TEXT("production.project.cook")
			|| Capability == TEXT("production.project.package")
			|| Capability == TEXT("production.commandlet.run");
	}

	bool HasWriteEffect(const TSharedPtr<FJsonObject>& Descriptor)
	{
		if (!Descriptor.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Effects = nullptr;
		if (!Descriptor->TryGetObjectField(TEXT("effects"), Effects)
			|| !Effects || !Effects->IsValid())
		{
			return false;
		}
		for (const TCHAR* Field : {
			TEXT("asset"), TEXT("world"), TEXT("editorSession"), TEXT("external") })
		{
			FString Value;
			if ((*Effects)->TryGetStringField(Field, Value)
				&& Value == TEXT("write"))
			{
				return true;
			}
		}
		return false;
	}

	FString ProtectedLeaseType(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Descriptor)
	{
		if (!HasWriteEffect(Descriptor))
		{
			return FString();
		}
		if (Capability.StartsWith(TEXT("scene.pie.")))
		{
			return TEXT("pie");
		}
		if (Capability.Contains(TEXT("compile"), ESearchCase::IgnoreCase))
		{
			return TEXT("compile");
		}
		if (Capability.StartsWith(TEXT("production.performance.")))
		{
			return TEXT("performance");
		}
		if (Capability.Contains(TEXT("editor.restart"), ESearchCase::IgnoreCase)
			|| Capability.Contains(TEXT("editorRestart"), ESearchCase::IgnoreCase))
		{
			return TEXT("editorRestart");
		}
		return FString();
	}

}

FMCPExecutor::FMCPExecutor(FMCPToolRegistry& InRegistry)
	: Registry(InRegistry)
{
}

FMCPResult FMCPExecutor::Execute(const FMCPExecutionContext& Context)
{
	FMCPResult LeaseFailure;
	if (!CheckProtectedLease(Context, LeaseFailure))
	{
		AnnotateVerification(Context, LeaseFailure, false);
		return LeaseFailure;
	}
	if (Context.RequestId.IsEmpty())
	{
		return ExecuteUncached(Context);
	}

	const FDateTime NowUtc = FDateTime::UtcNow();
	PruneIdempotencyCache(NowUtc);
	const FString PayloadKey = MakePayloadKey(Context);

	if (FIdempotencyRecord* Existing = IdempotencyCache.Find(Context.RequestId))
	{
		Existing->LastAccessUtc = NowUtc;
		if (Existing->PayloadKey != PayloadKey)
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("requestId"), Context.RequestId);
			FMCPResult Conflict = FMCPResult::Fail(
				TEXT("idempotency_conflict"),
				TEXT("The requestId has already been used with a different payload."),
				409,
				Details);
			AnnotateVerification(Context, Conflict, false);
			return Conflict;
		}
		return Existing->Result;
	}

	const FMCPResult Result = ExecuteUncached(Context);
	StoreIdempotencyResult(Context.RequestId, PayloadKey, Result, NowUtc);
	return Result;
}

bool FMCPExecutor::BeginExecuteAsync(
	const FMCPExecutionContext& Context,
	TFunction<void(FMCPResult&&)> Completion,
	FMCPResult& OutImmediate)
{
	check(IsInGameThread());
	if (!CheckProtectedLease(Context, OutImmediate))
	{
		AnnotateVerification(Context, OutImmediate, false);
		return false;
	}
	FMCPToolBase* Tool = Registry.FindTool(Context.Capability);
	if (!Tool || !Tool->SupportsAsyncExecution())
	{
		OutImmediate = Execute(Context);
		return false;
	}

	const FDateTime NowUtc = FDateTime::UtcNow();
	PruneIdempotencyCache(NowUtc);
	const FString PayloadKey = MakePayloadKey(Context);
	if (!Context.RequestId.IsEmpty())
	{
		if (FIdempotencyRecord* Existing =
				IdempotencyCache.Find(Context.RequestId))
		{
			Existing->LastAccessUtc = NowUtc;
			if (Existing->PayloadKey != PayloadKey)
			{
				TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
				Details->SetStringField(TEXT("requestId"), Context.RequestId);
				OutImmediate = FMCPResult::Fail(
					TEXT("idempotency_conflict"),
					TEXT("The requestId has already been used with a different payload."),
					409,
					Details);
				AnnotateVerification(Context, OutImmediate, false);
			}
			else
			{
				OutImmediate = Existing->Result;
			}
			return false;
		}
		if (const FInFlightRecord* Existing =
				InFlightRequests.Find(Context.RequestId))
		{
			TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("requestId"), Context.RequestId);
			OutImmediate = Existing->PayloadKey == PayloadKey
				? FMCPResult::Fail(
					TEXT("async_request_in_progress"),
					TEXT("The requestId is already executing asynchronously."),
					409,
					Details)
				: FMCPResult::Fail(
					TEXT("idempotency_conflict"),
					TEXT("The requestId is already executing with a different payload."),
					409,
					Details);
			AnnotateVerification(Context, OutImmediate, false);
			return false;
		}
	}

	TSharedPtr<FJsonObject> EffectiveParams;
	if (!PrepareExecution(Context, EffectiveParams, OutImmediate))
	{
		AnnotateVerification(Context, OutImmediate, false);
		return false;
	}
	if (!Context.RequestId.IsEmpty())
	{
		InFlightRequests.Add(
			Context.RequestId,
			FInFlightRecord{PayloadKey, Context.Capability});
	}

	const FString RequestId = Context.RequestId;
	const FMCPExecutionContext ResultContext = Context;
	const TSharedRef<bool, ESPMode::ThreadSafe> bCompleted =
		MakeShared<bool, ESPMode::ThreadSafe>(false);
	const bool bStarted = Registry.BeginExecuteToolAsync(
		Context.Capability,
		EffectiveParams,
		[this,
		 RequestId,
		 PayloadKey,
		 ResultContext,
		 bCompleted,
		 Completion = MoveTemp(Completion)](FMCPToolResult&& ToolResult) mutable
		{
			check(IsInGameThread());
			if (*bCompleted)
			{
				ensureMsgf(false, TEXT("Async MCP tool completed more than once."));
				return;
			}
			*bCompleted = true;
			FMCPResult Result = ConvertToolResult(MoveTemp(ToolResult));
			AnnotateVerification(ResultContext, Result, true);
			if (!RequestId.IsEmpty())
			{
				InFlightRequests.Remove(RequestId);
				StoreIdempotencyResult(
					RequestId,
					PayloadKey,
					Result,
					FDateTime::UtcNow());
			}
			Completion(MoveTemp(Result));
		});
	if (bStarted)
	{
		return true;
	}

	if (!Context.RequestId.IsEmpty())
	{
		InFlightRequests.Remove(Context.RequestId);
	}
	OutImmediate = FMCPResult::Fail(
		TEXT("async_start_failed"),
		TEXT("The asynchronous capability did not accept the request."),
		500);
	AnnotateVerification(Context, OutImmediate, false);
	return false;
}

void FMCPExecutor::CancelAsyncOperations(const FString& Reason)
{
	check(IsInGameThread());
	Registry.CancelAsyncTools(Reason);
	ensureMsgf(
		InFlightRequests.IsEmpty(),
		TEXT("An async MCP tool did not complete during cancellation."));
}

FMCPResult FMCPExecutor::CancelAsyncOperation(
	const FString& RequestId,
	const FString& Reason)
{
	check(IsInGameThread());
	if (RequestId.IsEmpty())
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("requestId is required."),
			422);
	}
	const FInFlightRecord* InFlight = InFlightRequests.Find(RequestId);
	if (!InFlight)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("requestId"), RequestId);
		Data->SetBoolField(TEXT("cancelPending"), false);
		Data->SetStringField(TEXT("state"), TEXT("notInFlight"));
		return FMCPResult::Ok(Data);
	}
	// Cancellation may synchronously complete the tool, which removes this
	// request from InFlightRequests. Copy all acknowledgement data before
	// dispatching the cancellation instead of retaining a map-value pointer.
	const FString Capability = InFlight->Capability;
	const bool bAccepted = Registry.CancelAsyncTool(
		Capability,
		Reason.IsEmpty()
			? TEXT("The MCP client cancelled the request.")
			: Reason);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("requestId"), RequestId);
	Data->SetStringField(TEXT("capability"), Capability);
	Data->SetBoolField(TEXT("cancelPending"), bAccepted);
	Data->SetStringField(
		TEXT("state"),
		bAccepted ? TEXT("cancellationRequested") : TEXT("notCancellable"));
	return FMCPResult::Ok(Data);
}

FMCPResult FMCPExecutor::ExecuteUncached(const FMCPExecutionContext& Context) const
{
	TSharedPtr<FJsonObject> EffectiveParams;
	FMCPResult Failure;
	if (!PrepareExecution(Context, EffectiveParams, Failure))
	{
		AnnotateVerification(Context, Failure, false);
		return Failure;
	}

	FMCPResult Result = ConvertToolResult(
		Registry.ExecuteTool(Context.Capability, EffectiveParams));
	AnnotateVerification(Context, Result, true);
	return Result;
}

void FMCPExecutor::AnnotateVerification(
	const FMCPExecutionContext& Context,
	FMCPResult& Result,
	bool bExecuted) const
{
	const TSharedPtr<FJsonObject>* Descriptor =
		Registry.FindCapabilityDescriptor(Context.Capability);
	// These exact actions have handlers in FWorkflowRuntime::HandleRequest.
	// The workflow prefix alone cannot prove that an unknown action exists.
	static const TSet<FString> WorkflowCapabilities = {
		TEXT("workflow.validate"),
		TEXT("workflow.plan"),
		TEXT("workflow.execute"),
		TEXT("workflow.resume"),
		TEXT("workflow.status"),
		TEXT("workflow.rollback"),
	};
	const bool bWorkflowAction = WorkflowCapabilities.Contains(Context.Capability);
	const bool bLocalDeclared = bWorkflowAction || (Descriptor && Descriptor->IsValid());
	const bool bHandlerRegistered = bWorkflowAction
		? true
		: Registry.FindTool(Context.Capability) != nullptr;
	const TArray<FString> AvailabilityReasons =
		(!bWorkflowAction && bLocalDeclared)
			? UEAIIntegration::Infrastructure::GetCapabilityUnavailableReasons(
				*Descriptor)
			: TArray<FString>();
	const bool bLiveAvailable = bWorkflowAction
		? Registry.IsReady()
		: bLocalDeclared
		&& bHandlerRegistered
		&& Registry.IsReady()
		&& AvailabilityReasons.IsEmpty();
	const bool bHandlerExecuted = bExecuted && bHandlerRegistered;

	TSharedPtr<FJsonObject> Target;
	if (Result.bOk)
	{
		if (!Result.Data.IsValid())
		{
			Result.Data = MakeShared<FJsonObject>();
		}
		Target = Result.Data;
	}
	else
	{
		if (!Result.Error.Details.IsValid())
		{
			Result.Error.Details = MakeShared<FJsonObject>();
		}
		Target = Result.Error.Details;
	}

	// Preserve handler-owned values, including explicit false values used by
	// read-only queries. Missing values remain JSON null rather than being
	// inferred as a successful readback or runtime check.
	bool bReadbackVerified = false;
	const bool bHasReadbackVerified = Target->TryGetBoolField(
		TEXT("readbackVerified"),
		bReadbackVerified);
	bool bRuntimeVerified = false;
	const bool bHasRuntimeVerified = Target->TryGetBoolField(
		TEXT("runtimeVerified"),
		bRuntimeVerified);

	Target->SetBoolField(TEXT("localDeclared"), bLocalDeclared);
	Target->SetBoolField(TEXT("handlerRegistered"), bHandlerRegistered);
	Target->SetBoolField(TEXT("liveAvailable"), bLiveAvailable);
	Target->SetBoolField(TEXT("executed"), bHandlerExecuted);
	if (bHasReadbackVerified)
	{
		Target->SetBoolField(TEXT("readbackVerified"), bReadbackVerified);
	}
	else
	{
		Target->SetField(TEXT("readbackVerified"), MakeShared<FJsonValueNull>());
	}
	if (bHasRuntimeVerified)
	{
		Target->SetBoolField(TEXT("runtimeVerified"), bRuntimeVerified);
	}
	else
	{
		Target->SetField(TEXT("runtimeVerified"), MakeShared<FJsonValueNull>());
	}

	TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(TEXT("schema"), TEXT("ue.capability-verification.v1"));
	State->SetStringField(TEXT("capability"), Context.Capability);
	State->SetBoolField(TEXT("localDeclared"), bLocalDeclared);
	State->SetBoolField(TEXT("handlerRegistered"), bHandlerRegistered);
	State->SetBoolField(TEXT("liveAvailable"), bLiveAvailable);
	State->SetBoolField(TEXT("executed"), bHandlerExecuted);
	if (bHasReadbackVerified)
	{
		State->SetBoolField(TEXT("readbackVerified"), bReadbackVerified);
	}
	else
	{
		State->SetField(TEXT("readbackVerified"), MakeShared<FJsonValueNull>());
	}
	if (bHasRuntimeVerified)
	{
		State->SetBoolField(TEXT("runtimeVerified"), bRuntimeVerified);
	}
	else
	{
		State->SetField(TEXT("runtimeVerified"), MakeShared<FJsonValueNull>());
	}
	// Some domain payloads already use a string field named "verification";
	// keep that field intact and expose the cross-capability state separately.
	Target->SetObjectField(TEXT("verificationState"), State);
}

bool FMCPExecutor::CheckProtectedLease(
	const FMCPExecutionContext& Context,
	FMCPResult& OutFailure) const
{
	const TSharedPtr<FJsonObject>* Descriptor =
		Registry.FindCapabilityDescriptor(Context.Capability);
	const FString LeaseType = ProtectedLeaseType(
		Context.Capability,
		Descriptor ? *Descriptor : nullptr);
	if (LeaseType.IsEmpty())
	{
		return true;
	}
	UEAIIntegration::Infrastructure::FClientActivityService* Activity =
		UEAIIntegration::Infrastructure::FClientActivityService::GetActiveService();
	if (!Activity)
	{
		return true;
	}
	FString OwnerSessionId;
	if (Activity->CanAccessLease(
		LeaseType,
		Context.CallerSessionId,
		OwnerSessionId))
	{
		return true;
	}
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("leaseType"), LeaseType);
	Details->SetStringField(TEXT("ownerSessionId"), OwnerSessionId);
	Details->SetStringField(TEXT("callerSessionId"), Context.CallerSessionId);
	OutFailure = FMCPResult::Fail(
		TEXT("lease_conflict"),
		TEXT("The protected operation is leased by another live session."),
		409,
		Details);
	return false;
}

bool FMCPExecutor::PrepareExecution(
	const FMCPExecutionContext& Context,
	TSharedPtr<FJsonObject>& OutEffectiveParams,
	FMCPResult& OutFailure) const
{
	const TSharedPtr<FJsonObject>* Descriptor =
		Registry.FindCapabilityDescriptor(Context.Capability);
	if (!Descriptor || !Descriptor->IsValid())
	{
		if (const TSharedPtr<FJsonObject>* Tombstone =
			Registry.FindCapabilityTombstone(Context.Capability))
		{
			const FString Replacement = (*Tombstone)->GetStringField(TEXT("replacement"));
			OutFailure = FMCPResult::Fail(
				TEXT("capability_removed"),
				FString::Printf(
					TEXT("Capability '%s' was removed; use '%s'."),
					*Context.Capability,
					*Replacement),
				410,
				*Tombstone);
			return false;
		}
		OutFailure = FMCPResult::Fail(
			TEXT("capability_not_found"),
			FString::Printf(TEXT("Capability '%s' was not found."), *Context.Capability),
			404);
		return false;
	}
	if (!Registry.FindTool(Context.Capability))
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		Details->SetStringField(TEXT("capability"), Context.Capability);
		OutFailure = FMCPResult::Fail(
			TEXT("capability_not_loaded"),
			FString::Printf(
				TEXT("Capability '%s' exists in the catalog but has no loaded Editor handler."),
				*Context.Capability),
			503,
			Details);
		return false;
	}
	if (!Registry.IsReady())
	{
		OutFailure = FMCPResult::Fail(
			TEXT("service_degraded"),
			TEXT("Capability bindings failed validation."),
			503,
			MakeValidationDetails(Registry.GetValidationErrors()));
		return false;
	}

	const TArray<FString> AvailabilityReasons =
		Descriptor && Descriptor->IsValid()
			? UEAIIntegration::Infrastructure::GetCapabilityUnavailableReasons(
				*Descriptor)
			: TArray<FString>();
	if (!AvailabilityReasons.IsEmpty())
	{
		TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ReasonValues;
		for (const FString& Reason : AvailabilityReasons)
		{
			ReasonValues.Add(MakeShared<FJsonValueString>(Reason));
		}
		Details->SetStringField(TEXT("capability"), Context.Capability);
		Details->SetArrayField(TEXT("availabilityReasons"), ReasonValues);
		OutFailure = FMCPResult::Fail(
			TEXT("capability_unavailable"),
			TEXT("The capability is not available in this plugin build or project."),
			409,
			Details);
		return false;
	}

	OutEffectiveParams =
		Context.Params.IsValid()
			? MakeShared<FJsonObject>(*Context.Params)
			: MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* InputSchema =
		Registry.FindInputSchema(Context.Capability);
	const TSharedPtr<FJsonObject>* Properties = nullptr;
	const bool bAcceptsRequestId =
		InputSchema
		&& InputSchema->IsValid()
		&& (*InputSchema)->TryGetObjectField(TEXT("properties"), Properties)
		&& Properties
		&& Properties->IsValid()
		&& (*Properties)->HasField(TEXT("requestId"));
	if (bAcceptsRequestId)
	{
		const bool bAllowsLegacyOmission =
			AllowsLegacyRequestIdOmission(Context.Capability);
		if (Context.RequestId.IsEmpty() && !bAllowsLegacyOmission)
		{
			OutFailure = FMCPResult::Fail(
				TEXT("request_id_required"),
				TEXT(
					"This capability requires requestId in the /api/execute envelope."),
				422);
			return false;
		}
		FString LegacyParamRequestId;
		if (Context.RequestId.IsEmpty()
			&& OutEffectiveParams->TryGetStringField(
				TEXT("requestId"),
				LegacyParamRequestId)
			&& !LegacyParamRequestId.IsEmpty())
		{
			OutFailure = FMCPResult::Fail(
				TEXT("request_id_required"),
				TEXT(
					"requestId must be supplied in the /api/execute envelope."),
				422);
			return false;
		}
		if (!Context.RequestId.IsEmpty())
		{
			FString ParamRequestId;
			if (OutEffectiveParams->TryGetStringField(TEXT("requestId"), ParamRequestId)
				&& ParamRequestId != Context.RequestId)
			{
				OutFailure = FMCPResult::Fail(
					TEXT("request_id_mismatch"),
					TEXT(
						"params.requestId must match the /api/execute envelope requestId."),
					409);
				return false;
			}
			OutEffectiveParams->SetStringField(TEXT("requestId"), Context.RequestId);
		}
	}

	TArray<FString> ParamErrors;
	if (!Registry.ValidateParams(
			Context.Capability, OutEffectiveParams, ParamErrors))
	{
		OutFailure = FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Capability parameters failed manifest schema validation."),
			422,
			MakeValidationDetails(ParamErrors));
		return false;
	}
	if (Context.Capability == TEXT("production.lease.acquire")
		|| Context.Capability == TEXT("production.lease.release"))
	{
		if (Context.CallerSessionId.IsEmpty())
		{
			OutFailure = FMCPResult::Fail(
				TEXT("client_session_required"),
				TEXT("Lease operations require a registered caller session."),
				401);
			return false;
		}
		OutEffectiveParams->SetStringField(
			TEXT("__callerSessionId"),
			Context.CallerSessionId);
	}
	return true;
}

FMCPResult FMCPExecutor::ConvertToolResult(FMCPToolResult&& ToolResult)
{
	if (!ToolResult.bSuccess)
	{
		return FMCPResult::Fail(
			ToolResult.ErrorCode.IsEmpty() ? TEXT("execution_failed") : ToolResult.ErrorCode,
			ToolResult.ErrorMessage.IsEmpty()
				? TEXT("Capability execution failed.")
				: ToolResult.ErrorMessage,
			ToolResult.HttpStatus >= 400 ? ToolResult.HttpStatus : 500,
			ToolResult.Data);
	}

	return FMCPResult::Ok(ToolResult.Data);
}

void FMCPExecutor::StoreIdempotencyResult(
	const FString& RequestId,
	const FString& PayloadKey,
	const FMCPResult& Result,
	const FDateTime& NowUtc)
{
	if (RequestId.IsEmpty())
	{
		return;
	}
	if (IdempotencyCache.Num() >= MaxIdempotencyRecords)
	{
		FString OldestRequestId;
		FDateTime OldestAccessUtc;
		bool bFoundOldest = false;
		for (const TPair<FString, FIdempotencyRecord>& Pair : IdempotencyCache)
		{
			if (!bFoundOldest || Pair.Value.LastAccessUtc < OldestAccessUtc)
			{
				bFoundOldest = true;
				OldestRequestId = Pair.Key;
				OldestAccessUtc = Pair.Value.LastAccessUtc;
			}
		}
		if (bFoundOldest)
		{
			IdempotencyCache.Remove(OldestRequestId);
		}
	}
	FIdempotencyRecord& Record = IdempotencyCache.Add(RequestId);
	Record.PayloadKey = PayloadKey;
	Record.Result = Result;
	Record.LastAccessUtc = NowUtc;
}

FString FMCPExecutor::MakePayloadKey(const FMCPExecutionContext& Context)
{
	TSharedPtr<FJsonObject> Params =
		Context.Params.IsValid()
			? MakeShared<FJsonObject>(*Context.Params)
			: MakeShared<FJsonObject>();
	FString ParamRequestId;
	if (Params->TryGetStringField(TEXT("requestId"), ParamRequestId)
		&& ParamRequestId == Context.RequestId)
	{
		// requestId belongs to the execution envelope. Accept a legacy duplicate
		// in params without treating an otherwise identical retry as a conflict.
		Params->RemoveField(TEXT("requestId"));
	}
	const FString CanonicalParams =
		UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
			MakeShared<FJsonValueObject>(Params));
	return FString::Printf(
		TEXT("%d:%s%s"),
		Context.Capability.Len(),
		*Context.Capability,
		*CanonicalParams);
}

TSharedPtr<FJsonObject> FMCPExecutor::MakeValidationDetails(
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

void FMCPExecutor::PruneIdempotencyCache(const FDateTime& NowUtc)
{
	const FDateTime ExpiryUtc = NowUtc - FTimespan::FromMinutes(15.0);
	for (auto It = IdempotencyCache.CreateIterator(); It; ++It)
	{
		if (It.Value().LastAccessUtc < ExpiryUtc)
		{
			It.RemoveCurrent();
		}
	}
}
