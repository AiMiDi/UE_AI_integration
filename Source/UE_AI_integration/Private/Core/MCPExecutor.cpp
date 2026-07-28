#include "Core/MCPExecutor.h"

#include "Dom/JsonValue.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
	constexpr int32 MaxIdempotencyRecords = 256;

	FString CanonicalizeJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return TEXT("z");
		}

		switch (Value->Type)
		{
		case EJson::String:
		{
			const FString Text = Value->AsString();
			return FString::Printf(TEXT("s%d:%s"), Text.Len(), *Text);
		}
		case EJson::Number:
			return FString::Printf(TEXT("n%.17g"), Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("b1") : TEXT("b0");
		case EJson::Array:
		{
			FString Result(TEXT("a"));
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				const FString CanonicalItem = CanonicalizeJsonValue(Item);
				Result += FString::Printf(
					TEXT("%d:%s"),
					CanonicalItem.Len(),
					*CanonicalItem);
			}
			return Result;
		}
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				return TEXT("o");
			}

			TArray<FString> Keys;
			Object->Values.GetKeys(Keys);
			Keys.Sort();

			FString Result(TEXT("o"));
			for (const FString& Key : Keys)
			{
				const FString CanonicalItem =
					CanonicalizeJsonValue(Object->Values.FindRef(Key));
				Result += FString::Printf(
					TEXT("%d:%s%d:%s"),
					Key.Len(),
					*Key,
					CanonicalItem.Len(),
					*CanonicalItem);
			}
			return Result;
		}
		default:
			return TEXT("z");
		}
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

	TArray<FString> ParamErrors;
	if (!Registry.ValidateParams(Context.Capability, Context.Params, ParamErrors))
	{
		return FMCPResult::Fail(
			TEXT("invalid_params"),
			TEXT("Capability parameters failed manifest schema validation."),
			422,
			MakeValidationDetails(ParamErrors));
	}

	const FMCPToolResult ToolResult =
		Registry.ExecuteTool(Context.Capability, Context.Params);
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
	const TSharedPtr<FJsonObject> Params =
		Context.Params.IsValid() ? Context.Params : MakeShared<FJsonObject>();
	const FString CanonicalParams =
		CanonicalizeJsonValue(MakeShared<FJsonValueObject>(Params));
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
