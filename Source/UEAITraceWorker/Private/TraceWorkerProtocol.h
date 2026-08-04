#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

namespace UEAI::TraceWorker
{
class FProtocol
{
public:
	static TSharedPtr<FJsonObject> HandleRequest(
		const TSharedPtr<FJsonObject>& Request,
		const FString& CommandLine);
	/** Release cached TraceServices sessions before Core tears down event pools. */
	static void ShutdownAnalysisCache();

private:
	static TSharedPtr<FJsonObject> HandleHandshake(
		const FString& RequestId,
		const FString& CommandLine);
	static TSharedPtr<FJsonObject> HandleExecute(
		const FString& RequestId,
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Params,
		const FString& CommandLine);
};
}
