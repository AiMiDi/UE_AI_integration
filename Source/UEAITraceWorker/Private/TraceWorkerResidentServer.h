#pragma once

#include "CoreMinimal.h"

namespace UEAI::TraceWorker
{
class FResidentServer
{
public:
	/** Run a bounded current-user local IPC server until its idle timeout. */
	static int32 Run(const FString& CommandLine);
};
}
