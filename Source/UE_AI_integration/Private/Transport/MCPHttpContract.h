#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HttpServerResponse.h"

namespace UEAIIntegration
{
namespace Transport
{
	TSharedPtr<FJsonObject> MakeSuccessEnvelope(
		const TSharedPtr<FJsonObject>& Data);

	TSharedPtr<FJsonObject> MakeErrorEnvelope(
		const FString& Code,
		const FString& Message,
		const TSharedPtr<FJsonObject>& Details = nullptr);

	TUniquePtr<FHttpServerResponse> MakeJsonResponse(
		int32 StatusCode,
		const TSharedPtr<FJsonObject>& Envelope);
}
}
