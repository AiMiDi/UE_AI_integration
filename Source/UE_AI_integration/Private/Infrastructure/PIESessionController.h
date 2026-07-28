#pragma once

#include "CoreMinimal.h"

namespace UEAIIntegration::Infrastructure
{
class FRuntimeSceneService;

struct FPIEControlResult
{
	bool bSuccess = true;
	bool bRequested = false;
	FString State;
	FString SessionId;
	uint64 Generation = 0;
	bool bPaused = false;
	FString ErrorCode;
	FString ErrorMessage;
	int32 HttpStatus = 200;
};

/**
 * Owns deferred Play In Editor lifecycle transitions.
 *
 * Restart is intentionally completed across editor ticks: Unreal queues both
 * PIE startup and shutdown, so requesting both in one call can race the old
 * play world.
 */
class FPIESessionController
{
public:
	FPIESessionController();
	~FPIESessionController();

	FPIEControlResult Start();
	FPIEControlResult Stop();
	FPIEControlResult Restart();
	FPIEControlResult Status() const;
	FPIEControlResult Pause();
	FPIEControlResult Resume();

	void Tick();

	FRuntimeSceneService& GetRuntimeService();
	const FRuntimeSceneService& GetRuntimeService() const;

private:
	void HandleBeginPIE(bool bIsSimulating);
	void HandlePostPIEStarted(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);
	void HandlePausePIE(bool bIsSimulating);
	void HandleResumePIE(bool bIsSimulating);
	void PopulateSession(FPIEControlResult& Result) const;
	FPIEControlResult MakeUnavailableResult() const;
	FString GetState() const;
	void RequestStart();

	TUniquePtr<FRuntimeSceneService> RuntimeService;
	FDelegateHandle BeginPIEHandle;
	FDelegateHandle PostPIEStartedHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle PausePIEHandle;
	FDelegateHandle ResumePIEHandle;
	bool bRestartPending = false;
	bool bPaused = false;
};
}
