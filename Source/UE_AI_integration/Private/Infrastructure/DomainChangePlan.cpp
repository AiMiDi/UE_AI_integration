#include "Infrastructure/DomainChangePlan.h"

#include "Infrastructure/EngineeringContractUtils.h"

namespace UEAIIntegration::Infrastructure
{
FString CanonicalizeJson(const TSharedPtr<FJsonObject>& Object)
{
	return CanonicalizeJsonValue(MakeShared<FJsonValueObject>(Object));
}

bool TryDigestJson(const TSharedPtr<FJsonObject>& Object, FString& OutDigest)
{
	OutDigest = DigestJson(Object);
	return !OutDigest.IsEmpty();
}

bool ValidateChangeApproval(
	const TSharedPtr<FJsonObject>& Params,
	const FString& CurrentDigest,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	OutErrorCode.Reset();
	OutErrorMessage.Reset();

	FString ApprovedDigest;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("approvePlanDigest"), ApprovedDigest)
		|| ApprovedDigest.IsEmpty())
	{
		OutErrorCode = TEXT("approval_required");
		OutErrorMessage = TEXT("approvePlanDigest is required.");
		return false;
	}
	if (ApprovedDigest != CurrentDigest)
	{
		OutErrorCode = TEXT("plan_digest_mismatch");
		OutErrorMessage = TEXT("approvePlanDigest does not match the current plan.");
		return false;
	}

	bool bConfirmWrite = false;
	if (!Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite) || !bConfirmWrite)
	{
		OutErrorCode = TEXT("write_confirmation_required");
		OutErrorMessage = TEXT("confirmWrite=true is required.");
		return false;
	}

	FString RequestId;
	if (!Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
	{
		OutErrorCode = TEXT("request_id_required");
		OutErrorMessage = TEXT("A non-empty requestId is required for idempotent writes.");
		return false;
	}
	return true;
}
}
