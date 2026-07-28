#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Typed, manifest-authoritative public capability descriptor. */
struct FMCPCapabilityDescriptor
{
	FString Id;
	FString Domain;
	FString Kind;
	FString Description;
	TSharedPtr<FJsonObject> InputSchema;
	bool bReadOnly = false;
	bool bDestructive = false;
	bool bExpensive = false;
	FString OutputKind;
	TSharedPtr<FJsonObject> Json;
};

/** Transport-neutral request passed into the Core executor. */
struct FMCPExecutionContext
{
	FString Capability;
	TSharedPtr<FJsonObject> Params;
	/** Optional caller supplied key used to make command retries idempotent. */
	FString RequestId;
};

/** Transport-neutral execution error. */
struct FMCPError
{
	FString Code;
	FString Message;
	int32 HttpStatus = 500;
	TSharedPtr<FJsonObject> Details;
};

/** Core execution result converted to the HTTP envelope only at the transport boundary. */
struct FMCPResult
{
	bool bOk = false;
	TSharedPtr<FJsonObject> Data;
	FMCPError Error;

	static FMCPResult Ok(const TSharedPtr<FJsonObject>& InData)
	{
		FMCPResult Result;
		Result.bOk = true;
		Result.Data = InData.IsValid() ? InData : MakeShared<FJsonObject>();
		return Result;
	}

	static FMCPResult Fail(
		const FString& Code,
		const FString& Message,
		int32 HttpStatus,
		const TSharedPtr<FJsonObject>& Details = nullptr)
	{
		FMCPResult Result;
		Result.bOk = false;
		Result.Error.Code = Code;
		Result.Error.Message = Message;
		Result.Error.HttpStatus = HttpStatus;
		Result.Error.Details = Details;
		return Result;
	}
};
