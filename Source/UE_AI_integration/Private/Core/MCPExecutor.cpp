#include "Core/MCPExecutor.h"

#include "Dom/JsonValue.h"
#include "Infrastructure/EngineeringContractUtils.h"
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

}

FMCPExecutor::FMCPExecutor(FMCPToolRegistry& InRegistry)
	: Registry(InRegistry)
{
}

FMCPResult FMCPExecutor::Execute(const FMCPExecutionContext& Context)
{
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
			return FMCPResult::Fail(
				TEXT("idempotency_conflict"),
				TEXT("The requestId has already been used with a different payload."),
				409,
				Details);
		}
		return Existing->Result;
	}

	const FMCPResult Result = ExecuteUncached(Context);
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

	FIdempotencyRecord& Record = IdempotencyCache.Add(Context.RequestId);
	Record.PayloadKey = PayloadKey;
	Record.Result = Result;
	Record.LastAccessUtc = NowUtc;
	return Result;
}

FMCPResult FMCPExecutor::ExecuteUncached(const FMCPExecutionContext& Context) const
{
	if (!Registry.IsReady())
	{
		return FMCPResult::Fail(
			TEXT("service_degraded"),
			TEXT("Capability bindings failed validation."),
			503,
			MakeValidationDetails(Registry.GetValidationErrors()));
	}

	if (!Registry.FindTool(Context.Capability))
	{
		return FMCPResult::Fail(
			TEXT("capability_not_found"),
			FString::Printf(TEXT("Capability '%s' was not found."), *Context.Capability),
			404);
	}

	TSharedPtr<FJsonObject> EffectiveParams =
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
			return FMCPResult::Fail(
				TEXT("request_id_required"),
				TEXT(
					"This capability requires requestId in the /api/execute envelope."),
				422);
		}
		FString LegacyParamRequestId;
		if (Context.RequestId.IsEmpty()
			&& EffectiveParams->TryGetStringField(
				TEXT("requestId"),
				LegacyParamRequestId)
			&& !LegacyParamRequestId.IsEmpty())
		{
			return FMCPResult::Fail(
				TEXT("request_id_required"),
				TEXT(
					"requestId must be supplied in the /api/execute envelope."),
				422);
		}
		if (!Context.RequestId.IsEmpty())
		{
			FString ParamRequestId;
			if (EffectiveParams->TryGetStringField(TEXT("requestId"), ParamRequestId)
				&& ParamRequestId != Context.RequestId)
			{
				return FMCPResult::Fail(
					TEXT("request_id_mismatch"),
					TEXT(
						"params.requestId must match the /api/execute envelope requestId."),
					409);
			}
			EffectiveParams->SetStringField(TEXT("requestId"), Context.RequestId);
		}
	}

	TArray<FString> ParamErrors;
	if (!Registry.ValidateParams(
			Context.Capability, EffectiveParams, ParamErrors))
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Capability parameters failed manifest schema validation."),
			422,
			MakeValidationDetails(ParamErrors));
	}

	const FMCPToolResult ToolResult =
		Registry.ExecuteTool(Context.Capability, EffectiveParams);
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
