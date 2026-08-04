#pragma once

#include "CoreMinimal.h"

namespace UEAI::TraceWorker
{
/** Test-only process diagnostic. It is intentionally not an MCP capability. */
int32 GenerateDiagnosticTraceFixture(
	const FString& TracePath,
	const FString& EngineMarkerOverride,
	FString& OutReceiptPath,
	FString& OutError);
}
