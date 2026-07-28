#include "Transport/MCPHttpContract.h"

#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAIIntegration
{
namespace Transport
{
TSharedPtr<FJsonObject> MakeSuccessEnvelope(
	const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("ok"), true);
	Envelope->SetObjectField(
		TEXT("data"),
		Data.IsValid() ? Data : MakeShared<FJsonObject>());
	return Envelope;
}

TSharedPtr<FJsonObject> MakeErrorEnvelope(
	const FString& Code,
	const FString& Message,
	const TSharedPtr<FJsonObject>& Details)
{
	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	if (Details.IsValid())
	{
		Error->SetObjectField(TEXT("details"), Details);
	}

	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("ok"), false);
	Envelope->SetObjectField(TEXT("error"), Error);
	return Envelope;
}

TUniquePtr<FHttpServerResponse> MakeJsonResponse(
	int32 StatusCode,
	const TSharedPtr<FJsonObject>& Envelope)
{
	check(Envelope.IsValid());

	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);

	TUniquePtr<FHttpServerResponse> Response =
		FHttpServerResponse::Create(Json, TEXT("application/json; charset=utf-8"));
	Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);
	return Response;
}
}
}
