#include "Modules/ModuleManager.h"

#if WITH_UEAI_TRACE_RUNTIME
#include "TraceRuntimeController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"
#endif

class FUEAITraceRuntimeModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if WITH_UEAI_TRACE_RUNTIME
		Controller = MakeUnique<UEAI::TraceRuntime::FController>();
		FString ManagedDescriptor;
		const bool bManagedLaunch = FParse::Value(
			FCommandLine::Get(), TEXT("UEAITraceJob="), ManagedDescriptor);
		if (!Controller->StartFromCommandLine() && bManagedLaunch)
		{
			// A constrained AI-owned launch must not silently continue without
			// its trace controller if the descriptor or receipt store is invalid.
			FPlatformMisc::RequestExit(false);
		}
#endif
	}

	virtual void ShutdownModule() override
	{
#if WITH_UEAI_TRACE_RUNTIME
		if (Controller)
		{
			Controller->Stop();
			Controller.Reset();
		}
#endif
	}

private:
#if WITH_UEAI_TRACE_RUNTIME
	TUniquePtr<UEAI::TraceRuntime::FController> Controller;
#endif
};

IMPLEMENT_MODULE(FUEAITraceRuntimeModule, UEAITraceRuntime)
