#include "Infrastructure/PIESessionController.h"

#include "Editor.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "IAssetViewport.h"
#include "Kismet2/DebuggerCommands.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
FPIEControlResult MakeSuccess(bool bRequested, const FString& State)
{
	FPIEControlResult Result;
	Result.bRequested = bRequested;
	Result.State = State;
	return Result;
}
}

FPIESessionController::FPIESessionController()
	: RuntimeService(MakeUnique<FRuntimeSceneService>())
{
	BeginPIEHandle = FEditorDelegates::BeginPIE.AddRaw(
		this,
		&FPIESessionController::HandleBeginPIE);
	PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddRaw(
		this,
		&FPIESessionController::HandlePostPIEStarted);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(
		this,
		&FPIESessionController::HandleEndPIE);
	PausePIEHandle = FEditorDelegates::PausePIE.AddRaw(
		this,
		&FPIESessionController::HandlePausePIE);
	ResumePIEHandle = FEditorDelegates::ResumePIE.AddRaw(
		this,
		&FPIESessionController::HandleResumePIE);

	// Plugin reloads can occur while PIE is already running.
	if (GEditor && GEditor->IsPlayingSessionInEditor())
	{
		RuntimeService->PrepareNextSession();
		RuntimeService->BeginSession();
		bPaused = FPlayWorldCommandCallbacks::HasPlayWorldAndPaused();
		RuntimeService->SetPaused(bPaused);
	}
}

FPIESessionController::~FPIESessionController()
{
	FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	FEditorDelegates::PausePIE.Remove(PausePIEHandle);
	FEditorDelegates::ResumePIE.Remove(ResumePIEHandle);
}

FPIEControlResult FPIESessionController::Start()
{
	if (!GEditor)
	{
		return MakeUnavailableResult();
	}

	// An explicit start supersedes a pending restart without trying to start
	// while the current play world is still shutting down.
	bRestartPending = false;
	if (GEditor->IsPlaySessionInProgress() || GEditor->ShouldEndPlayMap())
	{
		FPIEControlResult Result = MakeSuccess(false, GetState());
		PopulateSession(Result);
		return Result;
	}

	RequestStart();
	FPIEControlResult Result = MakeSuccess(true, GetState());
	PopulateSession(Result);
	return Result;
}

FPIEControlResult FPIESessionController::Stop()
{
	if (!GEditor)
	{
		return MakeUnavailableResult();
	}

	bRestartPending = false;

	if (GEditor->IsPlaySessionRequestQueued() && !GEditor->IsPlayingSessionInEditor())
	{
		GEditor->CancelRequestPlaySession();
		RuntimeService->CancelPreparedSession();
		FPIEControlResult Result = MakeSuccess(true, GetState());
		PopulateSession(Result);
		return Result;
	}

	if (!GEditor->IsPlayingSessionInEditor())
	{
		FPIEControlResult Result = MakeSuccess(false, GetState());
		PopulateSession(Result);
		return Result;
	}

	if (GEditor->ShouldEndPlayMap())
	{
		FPIEControlResult Result = MakeSuccess(false, GetState());
		PopulateSession(Result);
		return Result;
	}

	GEditor->RequestEndPlayMap();
	FPIEControlResult Result = MakeSuccess(true, GetState());
	PopulateSession(Result);
	return Result;
}

FPIEControlResult FPIESessionController::Restart()
{
	if (!GEditor)
	{
		return MakeUnavailableResult();
	}

	if (bRestartPending)
	{
		FPIEControlResult Result = MakeSuccess(false, GetState());
		PopulateSession(Result);
		return Result;
	}

	if (GEditor->IsPlaySessionRequestQueued() && !GEditor->IsPlayingSessionInEditor())
	{
		GEditor->CancelRequestPlaySession();
		RequestStart();
		FPIEControlResult Result = MakeSuccess(true, GetState());
		PopulateSession(Result);
		return Result;
	}

	if (!GEditor->IsPlayingSessionInEditor())
	{
		RequestStart();
		FPIEControlResult Result = MakeSuccess(true, GetState());
		PopulateSession(Result);
		return Result;
	}

	bRestartPending = true;
	RuntimeService->PrepareNextSession();
	if (!GEditor->ShouldEndPlayMap())
	{
		GEditor->RequestEndPlayMap();
	}
	FPIEControlResult Result = MakeSuccess(true, GetState());
	PopulateSession(Result);
	return Result;
}

FPIEControlResult FPIESessionController::Status() const
{
	if (!GEditor)
	{
		return MakeUnavailableResult();
	}

	FPIEControlResult Result = MakeSuccess(false, GetState());
	PopulateSession(Result);
	return Result;
}

FPIEControlResult FPIESessionController::Pause()
{
	if (!GEditor)
	{
		return MakeUnavailableResult();
	}
	if (!FPlayWorldCommandCallbacks::HasPlayWorld())
	{
		FPIEControlResult Result;
		Result.bSuccess = false;
		Result.State = GetState();
		Result.ErrorCode = TEXT("pie_not_running");
		Result.ErrorMessage = TEXT("PIE is not running.");
		Result.HttpStatus = 409;
		PopulateSession(Result);
		return Result;
	}

	const bool bWasPaused = FPlayWorldCommandCallbacks::HasPlayWorldAndPaused();
	if (!bWasPaused)
	{
		FPlayWorldCommandCallbacks::PausePlaySession_Clicked();
	}
	bPaused = FPlayWorldCommandCallbacks::HasPlayWorldAndPaused();
	RuntimeService->SetPaused(bPaused);

	FPIEControlResult Result = MakeSuccess(!bWasPaused, GetState());
	PopulateSession(Result);
	return Result;
}

FPIEControlResult FPIESessionController::Resume()
{
	if (!GEditor)
	{
		return MakeUnavailableResult();
	}
	if (!FPlayWorldCommandCallbacks::HasPlayWorld())
	{
		FPIEControlResult Result;
		Result.bSuccess = false;
		Result.State = GetState();
		Result.ErrorCode = TEXT("pie_not_running");
		Result.ErrorMessage = TEXT("PIE is not running.");
		Result.HttpStatus = 409;
		PopulateSession(Result);
		return Result;
	}

	const bool bWasPaused = FPlayWorldCommandCallbacks::HasPlayWorldAndPaused();
	if (bWasPaused)
	{
		FPlayWorldCommandCallbacks::ResumePlaySession_Clicked();
	}
	bPaused = FPlayWorldCommandCallbacks::HasPlayWorldAndPaused();
	RuntimeService->SetPaused(bPaused);

	FPIEControlResult Result = MakeSuccess(bWasPaused, GetState());
	PopulateSession(Result);
	return Result;
}

void FPIESessionController::Tick()
{
	RuntimeService->Tick();

	if (!bRestartPending || !GEditor)
	{
		return;
	}

	if (GEditor->IsPlayingSessionInEditor() ||
		GEditor->IsPlaySessionRequestQueued() ||
		GEditor->ShouldEndPlayMap())
	{
		return;
	}

	bRestartPending = false;
	RequestStart();
}

FRuntimeSceneService& FPIESessionController::GetRuntimeService()
{
	check(RuntimeService);
	return *RuntimeService;
}

const FRuntimeSceneService& FPIESessionController::GetRuntimeService() const
{
	check(RuntimeService);
	return *RuntimeService;
}

void FPIESessionController::HandleBeginPIE(bool bIsSimulating)
{
	RuntimeService->BeginSession();
	bPaused = false;
	RuntimeService->SetPaused(false);
}

void FPIESessionController::HandlePostPIEStarted(bool bIsSimulating)
{
	// BeginPIE normally activates the generation. This is intentionally
	// idempotent so plugin reload and engine-version event ordering are safe.
	RuntimeService->BeginSession();
}

void FPIESessionController::HandleEndPIE(bool bIsSimulating)
{
	RuntimeService->EndSession();
	bPaused = false;
}

void FPIESessionController::HandlePausePIE(bool bIsSimulating)
{
	bPaused = true;
	RuntimeService->SetPaused(true);
}

void FPIESessionController::HandleResumePIE(bool bIsSimulating)
{
	bPaused = false;
	RuntimeService->SetPaused(false);
}

void FPIESessionController::PopulateSession(FPIEControlResult& Result) const
{
	Result.SessionId = RuntimeService->GetSessionId();
	Result.Generation = RuntimeService->GetGeneration();
	Result.bPaused = bPaused || FPlayWorldCommandCallbacks::HasPlayWorldAndPaused();
}

FPIEControlResult FPIESessionController::MakeUnavailableResult() const
{
	FPIEControlResult Result;
	Result.bSuccess = false;
	Result.State = TEXT("unavailable");
	Result.ErrorCode = TEXT("editor_unavailable");
	Result.ErrorMessage = TEXT("Unreal Editor is not available.");
	Result.HttpStatus = 503;
	return Result;
}

FString FPIESessionController::GetState() const
{
	if (!GEditor)
	{
		return TEXT("unavailable");
	}
	if (bRestartPending)
	{
		return TEXT("restarting");
	}
	if (GEditor->ShouldEndPlayMap())
	{
		return TEXT("stopping");
	}
	if (GEditor->IsPlaySessionRequestQueued())
	{
		return TEXT("starting");
	}
	if (GEditor->IsPlayingSessionInEditor())
	{
		if (bPaused || FPlayWorldCommandCallbacks::HasPlayWorldAndPaused())
		{
			return TEXT("paused");
		}
		return TEXT("running");
	}
	return TEXT("stopped");
}

void FPIESessionController::RequestStart()
{
	RuntimeService->PrepareNextSession();

	FRequestPlaySessionParams Params;
	if (FLevelEditorModule* LevelEditor =
			FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
	{
		const TSharedPtr<IAssetViewport> ActiveViewport = LevelEditor->GetFirstActiveViewport();
		if (ActiveViewport.IsValid())
		{
			Params.DestinationSlateViewport = ActiveViewport;
		}
	}

	GEditor->RequestPlaySession(Params);
}
}
