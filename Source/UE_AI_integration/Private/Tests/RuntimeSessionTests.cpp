#if WITH_DEV_AUTOMATION_TESTS

#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Runtime/SlateRuntimeInputService.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SWindow.h"

using UEAIIntegration::Infrastructure::FRuntimeSceneService;
using UEAIIntegration::Infrastructure::FRuntimeServiceResult;
using UEAIIntegration::Infrastructure::FPIESessionController;
using UEAIIntegration::Infrastructure::FSlateRuntimeInputService;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeSessionGenerationTest,
	"UE_AI_integration.Runtime.SessionGenerationInvalidatesHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeSessionGenerationTest::RunTest(const FString& Parameters)
{
	FRuntimeSceneService Service;
	TestFalse(TEXT("Session begins inactive"), Service.IsSessionActive());
	TestEqual(TEXT("Initial generation is zero"), Service.GetGeneration(), uint64(0));

	Service.PrepareNextSession();
	const FString FirstSessionId = Service.GetSessionId();
	const uint64 FirstGeneration = Service.GetGeneration();
	TestFalse(TEXT("Prepared session is not active"), Service.IsSessionActive());
	TestTrue(TEXT("Prepared session has an id"), !FirstSessionId.IsEmpty());
	TestEqual(TEXT("First generation is one"), FirstGeneration, uint64(1));

	Service.BeginSession();
	TestTrue(TEXT("Begin activates prepared session"), Service.IsSessionActive());
	TestEqual(TEXT("Begin reuses prepared id"), Service.GetSessionId(), FirstSessionId);
	TestEqual(TEXT("Begin does not increment twice"), Service.GetGeneration(), FirstGeneration);

	TSharedPtr<FJsonObject> OldRef = MakeShared<FJsonObject>();
	OldRef->SetStringField(TEXT("sessionId"), FirstSessionId);
	OldRef->SetNumberField(TEXT("generation"), static_cast<double>(FirstGeneration));
	OldRef->SetStringField(
		TEXT("objectId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));

	Service.EndSession();
	UObject* ResolvedObject = nullptr;
	const FRuntimeServiceResult StoppedResult =
		Service.ResolveObjectRef(OldRef, ResolvedObject);
	TestFalse(TEXT("Stopped sessions do not resolve handles"), StoppedResult.bSuccess);
	TestEqual(
		TEXT("Stopped matching session reports pie_not_running"),
		StoppedResult.ErrorCode,
		FString(TEXT("pie_not_running")));
	TestEqual(TEXT("Stopped session status"), StoppedResult.HttpStatus, 409);

	Service.PrepareNextSession();
	TestNotEqual(
		TEXT("A new session gets a new id"),
		Service.GetSessionId(),
		FirstSessionId);
	TestEqual(
		TEXT("A new session increments generation"),
		Service.GetGeneration(),
		FirstGeneration + 1);

	const FRuntimeServiceResult StaleResult =
		Service.ResolveObjectRef(OldRef, ResolvedObject);
	TestFalse(TEXT("Old generation is stale"), StaleResult.bSuccess);
	TestEqual(
		TEXT("Old generation has stable error code"),
		StaleResult.ErrorCode,
		FString(TEXT("stale_session_handle")));
	TestEqual(TEXT("Old generation uses HTTP 410"), StaleResult.HttpStatus, 410);

	Service.BeginSession();
	Service.BeginSession();
	TestEqual(
		TEXT("Repeated BeginSession is idempotent"),
		Service.GetGeneration(),
		FirstGeneration + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeWorldContextQueryTest,
	"UE_AI_integration.Runtime.WorldContextsAreQueryableWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeWorldContextQueryTest::RunTest(const FString& Parameters)
{
	FRuntimeSceneService Service;
	const FRuntimeServiceResult Result =
		Service.ListWorldContexts(MakeShared<FJsonObject>());
	TestTrue(TEXT("World context query succeeds without PIE"), Result.bSuccess);
	TestTrue(TEXT("World context query returns data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("World context query returns worlds"), Result.Data->HasField(TEXT("worlds")));
		TestFalse(
			TEXT("World context query reports inactive session"),
			Result.Data->GetBoolField(TEXT("sessionActive")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeInputGameThreadNoDeadlockTest,
	"UE_AI_integration.Runtime.Input.GameThreadDispatchDoesNotDeadlock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeInputGameThreadNoDeadlockTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Automation test executes on the game thread"), IsInGameThread());
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Slate must be initialized for the runtime input regression."));
		return false;
	}

	UButton* ButtonObject = NewObject<UButton>(GetTransientPackage());
	UEditableText* TextObject = NewObject<UEditableText>(GetTransientPackage());
	ButtonObject->AddToRoot();
	TextObject->AddToRoot();
	ON_SCOPE_EXIT
	{
		ButtonObject->RemoveFromRoot();
		TextObject->RemoveFromRoot();
	};

	int32 RawButtonClicks = 0;
	TSharedPtr<SButton> RawButton;
	TSharedPtr<SEditableText> RawText;
	const TSharedRef<SWindow> Window =
		SNew(SWindow)
		.Title(FText::FromString(TEXT("UE AI Runtime Input Regression")))
		.ScreenPosition(FVector2D(120.0, 120.0))
		.ClientSize(FVector2D(480.0, 240.0))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				ButtonObject->TakeWidget()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				TextObject->TakeWidget()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(RawButton, SButton)
				.OnClicked_Lambda(
					[&RawButtonClicks]()
					{
						++RawButtonClicks;
						return FReply::Handled();
					})
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(RawText, SEditableText)
			]
		];
	FSlateApplication::Get().AddWindow(Window, false);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
		FSlateApplication::Get().Tick();
	};
	FSlateApplication::Get().Tick();
	Window->SlatePrepass();

	FSlateRuntimeInputService Input;
	const double StartSeconds = FPlatformTime::Seconds();
	const FRuntimeServiceResult TargetClick =
		Input.Pointer(TEXT("click"), ButtonObject, {}, {}, TEXT("left"));
	const FRuntimeServiceResult TargetType =
		Input.Key(TEXT("type"), FString(), TEXT("abc"), {}, TextObject);
	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

	TestTrue(TEXT("Targeted click dispatch succeeds"), TargetClick.bSuccess);
	TestTrue(TEXT("Targeted type dispatch succeeds"), TargetType.bSuccess);
	TestTrue(TEXT("Game-thread input returns promptly"), ElapsedSeconds < 2.0);
	if (TargetClick.Data.IsValid())
	{
		TestEqual(
			TEXT("Targeted click uses the direct Slate backend"),
			TargetClick.Data->GetStringField(TEXT("backend")),
			FString(TEXT("slate")));
	}
	if (TargetType.Data.IsValid())
	{
		TestEqual(
			TEXT("Targeted type uses the direct Slate backend"),
			TargetType.Data->GetStringField(TEXT("backend")),
			FString(TEXT("slate")));
	}

	const FVector2D ButtonCenter = RawButton->GetCachedGeometry().GetAbsolutePosition()
		+ RawButton->GetCachedGeometry().GetAbsoluteSize() * 0.5f;
	const FRuntimeServiceResult RawClick =
		Input.Pointer(TEXT("click"), nullptr, ButtonCenter, {}, TEXT("left"));
	TestTrue(TEXT("Direct Slate button click dispatch succeeds"), RawClick.bSuccess);
	TestEqual(TEXT("Direct Slate click invokes the button"), RawButtonClicks, 1);

	FSlateApplication::Get().SetKeyboardFocus(
		RawText,
		EFocusCause::SetDirectly);
	const FRuntimeServiceResult RawType =
		Input.Key(TEXT("type"), FString(), TEXT("deadlock-free"), {}, nullptr);
	TestTrue(TEXT("Direct Slate text input dispatch succeeds"), RawType.bSuccess);
	TestEqual(
		TEXT("Direct Slate text input mutates the focused control"),
		RawText->GetText().ToString(),
		FString(TEXT("deadlock-free")));

	TSharedPtr<FJsonObject> PositionOnlyHitTest = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> HitPosition = MakeShared<FJsonObject>();
	HitPosition->SetNumberField(TEXT("x"), ButtonCenter.X);
	HitPosition->SetNumberField(TEXT("y"), ButtonCenter.Y);
	PositionOnlyHitTest->SetObjectField(TEXT("position"), HitPosition);
	FRuntimeSceneService RuntimeService;
	const FRuntimeServiceResult HitResult =
		RuntimeService.HitTestWidget(PositionOnlyHitTest);
	TestTrue(
		TEXT("Runtime hit testing accepts an explicit position without objectRef"),
		HitResult.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimePointerSequenceStressTest,
	"UE_AI_integration.Runtime.Input.PointerSequenceStressAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimePointerSequenceStressTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Pointer sequence test runs on Game Thread"), IsInGameThread());
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Slate must be initialized for pointer-sequence tests."));
		return false;
	}

	UWorld* RuntimeWorld = UWorld::CreateWorld(EWorldType::PIE, false);
	if (!RuntimeWorld)
	{
		AddError(TEXT("Failed to create an isolated PIE world."));
		return false;
	}
	ON_SCOPE_EXIT
	{
		RuntimeWorld->DestroyWorld(false);
	};

	UButton* TargetButton = NewObject<UButton>(RuntimeWorld);
	int32 RawButtonClicks = 0;
	TSharedPtr<SButton> RawButton;
	const TSharedRef<SWindow> Window =
		SNew(SWindow)
		.Title(FText::FromString(TEXT("UE AI Pointer Sequence Regression")))
		.ScreenPosition(FVector2D(180.0, 180.0))
		.ClientSize(FVector2D(420.0, 220.0))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				TargetButton->TakeWidget()
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(RawButton, SButton)
				.OnClicked_Lambda(
					[&RawButtonClicks]()
					{
						++RawButtonClicks;
						return FReply::Handled();
					})
			]
		];
	FSlateApplication::Get().AddWindow(Window, false);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
		FSlateApplication::Get().Tick();
	};
	FSlateApplication::Get().Tick();
	Window->SlatePrepass();

	FRuntimeSceneService Service;
	Service.PrepareNextSession();
	Service.BeginSession();
	ON_SCOPE_EXIT
	{
		Service.EndSession();
	};

	TSharedPtr<FJsonObject> TargetRef;
	const FRuntimeServiceResult TargetRefResult =
		Service.MakeObjectRef(TargetButton, TargetRef);
	TestTrue(TEXT("Target UWidget receives a runtime ref"), TargetRefResult.bSuccess);
	if (!TargetRefResult.bSuccess)
	{
		return false;
	}

	auto MakePoint = [](const FVector2D& Position)
	{
		TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
		Point->SetNumberField(TEXT("x"), Position.X);
		Point->SetNumberField(TEXT("y"), Position.Y);
		return Point;
	};
	auto MakeSequenceParams =
		[&Service](
			const TArray<TSharedPtr<FJsonValue>>& Actions)
		{
			TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
			Params->SetStringField(TEXT("sessionId"), Service.GetSessionId());
			Params->SetNumberField(
				TEXT("generation"),
				static_cast<double>(Service.GetGeneration()));
			Params->SetStringField(
				TEXT("coordinateSpace"),
				TEXT("screenAbsolute"));
			Params->SetArrayField(TEXT("actions"), Actions);
			return Params;
		};

	const FVector2D RawButtonCenter =
		RawButton->GetCachedGeometry().GetAbsolutePosition()
		+ RawButton->GetCachedGeometry().GetAbsoluteSize() * 0.5f;
	for (int32 SequenceIndex = 0; SequenceIndex < 1000; ++SequenceIndex)
	{
		TArray<TSharedPtr<FJsonValue>> Actions;
		if (SequenceIndex < 50)
		{
			for (const FString& ActionName :
				{FString(TEXT("down")), FString(TEXT("up"))})
			{
				TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
				Action->SetStringField(TEXT("action"), ActionName);
				Action->SetObjectField(
					TEXT("position"),
					MakePoint(RawButtonCenter));
				Actions.Add(MakeShared<FJsonValueObject>(Action));
			}
		}
		else
		{
			TSharedPtr<FJsonObject> Action = MakeShared<FJsonObject>();
			Action->SetStringField(TEXT("action"), TEXT("move"));
			Action->SetObjectField(
				TEXT("position"),
				MakePoint(RawButtonCenter));
			Actions.Add(MakeShared<FJsonValueObject>(Action));
		}

		const FRuntimeServiceResult StartResult =
			Service.StartPointerSequence(MakeSequenceParams(Actions));
		if (!StartResult.bSuccess)
		{
			AddError(
				FString::Printf(
					TEXT("Sequence %d failed to start: %s"),
					SequenceIndex,
					*StartResult.ErrorMessage));
			return false;
		}
		Service.Tick();
		if (Service.GetActivePointerSequenceCountForTesting() != 0)
		{
			AddError(
				FString::Printf(
					TEXT("Sequence %d remained active after a zero-duration Tick."),
					SequenceIndex));
			return false;
		}
	}
	TestEqual(
		TEXT("Fifty real Slate down/up sequences click the raw button"),
		RawButtonClicks,
		50);
	TestEqual(
		TEXT("Stress run leaves no service-owned pressed buttons"),
		Service.GetPressedPointerButtonCountForTesting(),
		0);
	TestTrue(
		TEXT("Stress run leaves no Slate pointer capture"),
		FSlateApplication::Get().GetMouseCaptureWindow() == nullptr);

	TArray<TSharedPtr<FJsonValue>> TimedActions;
	TSharedPtr<FJsonObject> TimedMove = MakeShared<FJsonObject>();
	TimedMove->SetStringField(TEXT("action"), TEXT("move"));
	TimedMove->SetNumberField(TEXT("durationMs"), 30.0);
	TArray<TSharedPtr<FJsonValue>> TimedPath;
	TimedPath.Add(
		MakeShared<FJsonValueObject>(
			MakePoint(RawButtonCenter - FVector2D(10.0, 0.0))));
	TimedPath.Add(
		MakeShared<FJsonValueObject>(
			MakePoint(RawButtonCenter + FVector2D(10.0, 0.0))));
	TimedMove->SetArrayField(TEXT("path"), TimedPath);
	TimedActions.Add(MakeShared<FJsonValueObject>(TimedMove));
	const FRuntimeServiceResult TimedStart =
		Service.StartPointerSequence(MakeSequenceParams(TimedActions));
	TestTrue(TEXT("Timed pointer sequence starts"), TimedStart.bSuccess);
	FString TimedSequenceId;
	if (TimedStart.Data.IsValid())
	{
		TimedStart.Data->TryGetStringField(
			TEXT("sequenceId"),
			TimedSequenceId);
	}
	Service.Tick();
	TestEqual(
		TEXT("Timed sequence remains active before duration elapses"),
		Service.GetActivePointerSequenceCountForTesting(),
		1);
	FPlatformProcess::Sleep(0.04f);
	Service.Tick();
	TestEqual(
		TEXT("Timed sequence completes after elapsed duration"),
		Service.GetActivePointerSequenceCountForTesting(),
		0);
	TSharedPtr<FJsonObject> TimedGetParams = MakeShared<FJsonObject>();
	TimedGetParams->SetStringField(TEXT("sequenceId"), TimedSequenceId);
	const FRuntimeServiceResult TimedGet =
		Service.GetPointerSequence(TimedGetParams);
	TestTrue(TEXT("Timed sequence result remains queryable"), TimedGet.bSuccess);
	if (TimedGet.Data.IsValid())
	{
		TestEqual(
			TEXT("Timed sequence reaches completed"),
			TimedGet.Data->GetStringField(TEXT("status")),
			FString(TEXT("completed")));
		TestTrue(
			TEXT("Timed sequence reports at least the requested duration"),
			TimedGet.Data->GetNumberField(TEXT("elapsedMs")) >= 30.0);
	}

	TArray<TSharedPtr<FJsonValue>> MissActions;
	TSharedPtr<FJsonObject> MissDown = MakeShared<FJsonObject>();
	MissDown->SetStringField(TEXT("action"), TEXT("down"));
	MissDown->SetObjectField(
		TEXT("position"),
		MakePoint(FVector2D(1.0, 1.0)));
	MissActions.Add(MakeShared<FJsonValueObject>(MissDown));
	TSharedPtr<FJsonObject> MissParams = MakeSequenceParams(MissActions);
	MissParams->SetObjectField(TEXT("target"), TargetRef);
	MissParams->SetBoolField(TEXT("requireTargetHit"), true);
	const FRuntimeServiceResult MissStart =
		Service.StartPointerSequence(MissParams);
	TestTrue(TEXT("Target-hit guarded sequence is accepted asynchronously"), MissStart.bSuccess);
	FString MissSequenceId;
	MissStart.Data->TryGetStringField(TEXT("sequenceId"), MissSequenceId);
	Service.Tick();
	TSharedPtr<FJsonObject> MissGetParams = MakeShared<FJsonObject>();
	MissGetParams->SetStringField(TEXT("sequenceId"), MissSequenceId);
	const FRuntimeServiceResult MissGet =
		Service.GetPointerSequence(MissGetParams);
	TestTrue(TEXT("Target-hit rejection remains queryable"), MissGet.bSuccess);
	if (MissGet.Data.IsValid())
	{
		TestEqual(
			TEXT("Target miss fails the sequence"),
			MissGet.Data->GetStringField(TEXT("status")),
			FString(TEXT("failed")));
		const TArray<TSharedPtr<FJsonValue>>& Steps =
			MissGet.Data->GetArrayField(TEXT("steps"));
		if (!Steps.IsEmpty())
		{
			TestEqual(
				TEXT("Target miss is rejected before any Down dispatch"),
				static_cast<int32>(
					Steps[0]->AsObject()->GetNumberField(
						TEXT("dispatchedPoints"))),
				0);
		}
	}
	TestEqual(
		TEXT("Target-hit rejection leaves no pressed buttons"),
		Service.GetPressedPointerButtonCountForTesting(),
		0);

	TArray<TSharedPtr<FJsonValue>> HeldActions;
	TSharedPtr<FJsonObject> HeldDown = MakeShared<FJsonObject>();
	HeldDown->SetStringField(TEXT("action"), TEXT("down"));
	HeldDown->SetNumberField(TEXT("durationMs"), 1000.0);
	HeldDown->SetObjectField(
		TEXT("position"),
		MakePoint(RawButtonCenter));
	HeldActions.Add(MakeShared<FJsonValueObject>(HeldDown));
	const FRuntimeServiceResult HeldStart =
		Service.StartPointerSequence(MakeSequenceParams(HeldActions));
	TestTrue(TEXT("Held-button sequence starts"), HeldStart.bSuccess);
	Service.Tick();
	TestEqual(
		TEXT("Held sequence owns one pressed button before PIE end"),
		Service.GetPressedPointerButtonCountForTesting(),
		1);
	Service.EndSession();
	TestEqual(
		TEXT("PIE end releases every service-owned button"),
		Service.GetPressedPointerButtonCountForTesting(),
		0);
	TestEqual(
		TEXT("PIE end clears the active sequence"),
		Service.GetActivePointerSequenceCountForTesting(),
		0);
	TestTrue(
		TEXT("PIE end clears Slate pointer capture"),
		FSlateApplication::Get().GetMouseCaptureWindow() == nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeWaitAndStrictCaptureTest,
	"UE_AI_integration.Runtime.WaitAndStrictViewportContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeWaitAndStrictCaptureTest::RunTest(const FString& Parameters)
{
	FRuntimeSceneService Service;
	Service.PrepareNextSession();
	Service.BeginSession();

	auto MakeSessionParams = [&Service]()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), Service.GetSessionId());
		Params->SetNumberField(
			TEXT("generation"),
			static_cast<double>(Service.GetGeneration()));
		return Params;
	};

	const FRuntimeServiceResult StrictCapture =
		Service.CapturePIEViewport(MakeSessionParams());
	TestFalse(
		TEXT("A synthetic service session cannot fall back to an Editor window"),
		StrictCapture.bSuccess);
	TestEqual(
		TEXT("Strict capture reports capture failure without a real PIE viewport"),
		StrictCapture.ErrorCode,
		FString(TEXT("viewport_capture_failed")));

	TSharedPtr<FJsonObject> GenerationWait = MakeSessionParams();
	TSharedPtr<FJsonObject> GenerationPredicate = MakeShared<FJsonObject>();
	GenerationPredicate->SetStringField(TEXT("type"), TEXT("generationChanged"));
	GenerationWait->SetObjectField(TEXT("predicate"), GenerationPredicate);
	GenerationWait->SetNumberField(TEXT("timeoutMs"), 1000.0);
	const FRuntimeServiceResult GenerationStart =
		Service.StartWait(GenerationWait);
	TestTrue(TEXT("generationChanged wait starts"), GenerationStart.bSuccess);
	FString GenerationWaitId;
	GenerationStart.Data->TryGetStringField(
		TEXT("waitId"),
		GenerationWaitId);

	Service.PrepareNextSession();
	Service.Tick();
	TSharedPtr<FJsonObject> GenerationGetParams = MakeShared<FJsonObject>();
	GenerationGetParams->SetStringField(TEXT("waitId"), GenerationWaitId);
	const FRuntimeServiceResult GenerationGet =
		Service.GetWait(GenerationGetParams);
	TestTrue(TEXT("generationChanged wait remains queryable"), GenerationGet.bSuccess);
	if (GenerationGet.Data.IsValid())
	{
		TestEqual(
			TEXT("generationChanged completes after the next prepared generation"),
			GenerationGet.Data->GetStringField(TEXT("status")),
			FString(TEXT("completed")));
	}

	Service.BeginSession();
	TSharedPtr<FJsonObject> TimeoutWait = MakeSessionParams();
	TSharedPtr<FJsonObject> MissingWidgetPredicate = MakeShared<FJsonObject>();
	MissingWidgetPredicate->SetStringField(TEXT("type"), TEXT("widgetExists"));
	MissingWidgetPredicate->SetStringField(
		TEXT("name"),
		TEXT("__UEAI_Runtime_Missing_Widget__"));
	TimeoutWait->SetObjectField(TEXT("predicate"), MissingWidgetPredicate);
	TimeoutWait->SetNumberField(TEXT("timeoutMs"), 1.0);
	TimeoutWait->SetNumberField(TEXT("pollIntervalMs"), 1.0);
	const double StartSeconds = FPlatformTime::Seconds();
	const FRuntimeServiceResult TimeoutStart = Service.StartWait(TimeoutWait);
	const double StartElapsed = FPlatformTime::Seconds() - StartSeconds;
	TestTrue(TEXT("widgetExists wait starts"), TimeoutStart.bSuccess);
	TestTrue(
		TEXT("wait.until never blocks the Game Thread for its timeout"),
		StartElapsed < 0.1);
	FString TimeoutWaitId;
	TimeoutStart.Data->TryGetStringField(TEXT("waitId"), TimeoutWaitId);
	FPlatformProcess::Sleep(0.005f);
	Service.Tick();
	TSharedPtr<FJsonObject> TimeoutGetParams = MakeShared<FJsonObject>();
	TimeoutGetParams->SetStringField(TEXT("waitId"), TimeoutWaitId);
	const FRuntimeServiceResult TimeoutGet =
		Service.GetWait(TimeoutGetParams);
	TestTrue(TEXT("Timed-out wait remains queryable"), TimeoutGet.bSuccess);
	if (TimeoutGet.Data.IsValid())
	{
		TestEqual(
			TEXT("Timed-out wait reaches failed"),
			TimeoutGet.Data->GetStringField(TEXT("status")),
			FString(TEXT("failed")));
		const TSharedPtr<FJsonObject> Error =
			TimeoutGet.Data->GetObjectField(TEXT("error"));
		TestEqual(
			TEXT("Timed-out wait has a stable error code"),
			Error->GetStringField(TEXT("code")),
			FString(TEXT("wait_timeout")));
	}

	TSharedPtr<FJsonObject> LogWait = MakeSessionParams();
	TSharedPtr<FJsonObject> LogPredicate = MakeShared<FJsonObject>();
	LogPredicate->SetStringField(TEXT("type"), TEXT("logNotContains"));
	LogPredicate->SetStringField(
		TEXT("text"),
		TEXT("__UEAI_Runtime_Forbidden_Log_Text__"));
	LogWait->SetObjectField(TEXT("predicate"), LogPredicate);
	LogWait->SetNumberField(TEXT("timeoutMs"), 1.0);
	LogWait->SetNumberField(TEXT("pollIntervalMs"), 1.0);
	const FRuntimeServiceResult LogStart = Service.StartWait(LogWait);
	TestTrue(TEXT("logNotContains wait starts with the active log"), LogStart.bSuccess);
	FString LogWaitId;
	if (LogStart.Data.IsValid())
	{
		LogStart.Data->TryGetStringField(TEXT("waitId"), LogWaitId);
	}
	TestTrue(
		TEXT("Test can replace the log with an unreadable path"),
		Service.SetWaitLogPathForTesting(
			LogWaitId,
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("__UEAI_Missing_Log__.log"))));
	FPlatformProcess::Sleep(0.005f);
	Service.Tick();
	TSharedPtr<FJsonObject> LogGetParams = MakeShared<FJsonObject>();
	LogGetParams->SetStringField(TEXT("waitId"), LogWaitId);
	const FRuntimeServiceResult LogGet = Service.GetWait(LogGetParams);
	TestTrue(TEXT("Unavailable-log wait remains queryable"), LogGet.bSuccess);
	if (LogGet.Data.IsValid())
	{
		TestEqual(
			TEXT("Unavailable-log wait fails instead of proving absence"),
			LogGet.Data->GetStringField(TEXT("status")),
			FString(TEXT("failed")));
		const TSharedPtr<FJsonObject> Error =
			LogGet.Data->GetObjectField(TEXT("error"));
		TestEqual(
			TEXT("Unavailable-log wait has a stable error code"),
			Error->GetStringField(TEXT("code")),
			FString(TEXT("log_unavailable")));
	}
	Service.EndSession();
	return true;
}

namespace
{
class SRuntimeKeyRecorder : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRuntimeKeyRecorder)
	{
	}
	SLATE_END_ARGS()

	void Construct(const FArguments& Arguments)
	{
		ChildSlot
		[
			SNullWidget::NullWidget
		];
	}

	virtual bool SupportsKeyboardFocus() const override
	{
		return true;
	}

	virtual FReply OnKeyDown(
		const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override
	{
		const FKey Key = InKeyEvent.GetKey();
		HeldKeys.Add(Key);
		KeyDownOrder.Add(Key);
		if (Key == EKeys::W)
		{
			bWObservedShift = InKeyEvent.IsShiftDown();
		}
		return FReply::Handled();
	}

	virtual FReply OnKeyUp(
		const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override
	{
		const FKey Key = InKeyEvent.GetKey();
		HeldKeys.Remove(Key);
		KeyUpOrder.Add(Key);
		return FReply::Handled();
	}

	TSet<FKey> HeldKeys;
	TArray<FKey> KeyDownOrder;
	TArray<FKey> KeyUpOrder;
	bool bWObservedShift = false;
};

enum class ERuntimePIECaptureStage : uint8
{
	WaitingForPIE,
	WaitingForRenderDrain,
	WaitingForShutdown
};

struct FRuntimePIECaptureState
{
	TSharedPtr<FPIESessionController> Controller;
	ERuntimePIECaptureStage Stage = ERuntimePIECaptureStage::WaitingForPIE;
	double DeadlineSeconds = 0.0;
	bool bCaptureAttempted = false;
	bool bStopRequested = false;
	bool bExpectCaptureSuccess = true;
	int32 FramesUntilStop = 0;
};

using FRuntimePIECaptureStateRef =
	TSharedRef<FRuntimePIECaptureState, ESPMode::ThreadSafe>;

void StopRuntimePIECaptureSession(FRuntimePIECaptureState& State)
{
	if (State.bStopRequested)
	{
		return;
	}
	State.bStopRequested = true;
	if (State.Controller.IsValid())
	{
		State.Controller->Stop();
	}
	else if (GEditor)
	{
		if (GEditor->IsPlaySessionRequestQueued()
			&& !GEditor->IsPlayingSessionInEditor())
		{
			GEditor->CancelRequestPlaySession();
		}
		else if (GEditor->IsPlayingSessionInEditor())
		{
			GEditor->RequestEndPlayMap();
		}
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FWaitForRuntimePIECapture,
	FRuntimePIECaptureStateRef,
	State,
	FAutomationTestBase*,
	Test);

bool FWaitForRuntimePIECapture::Update()
{
	if (State->Controller.IsValid())
	{
		State->Controller->Tick();
	}

	const double Now = FPlatformTime::Seconds();
	if (State->Stage == ERuntimePIECaptureStage::WaitingForPIE)
	{
		if (Now >= State->DeadlineSeconds)
		{
			Test->AddError(
				TEXT("Timed out waiting for a real PIE Slate viewport."));
			StopRuntimePIECaptureSession(*State);
			State->Stage = ERuntimePIECaptureStage::WaitingForShutdown;
			State->DeadlineSeconds = Now + 10.0;
			return false;
		}

		if (!GEditor
			|| !GEditor->IsPlayingSessionInEditor()
			|| !State->Controller.IsValid())
		{
			return false;
		}

		FRuntimeSceneService& Service =
			State->Controller->GetRuntimeService();
		if (!Service.IsSessionActive() || !GEditor->GetPIEViewport())
		{
			return false;
		}

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("sessionId"), Service.GetSessionId());
		Params->SetNumberField(
			TEXT("generation"),
			static_cast<double>(Service.GetGeneration()));
		Params->SetNumberField(TEXT("width"), 160.0);
		Params->SetNumberField(TEXT("height"), 90.0);
		Params->SetNumberField(TEXT("quality"), 90.0);
		const FRuntimeServiceResult Capture =
			Service.CapturePIEViewport(Params);
		State->bCaptureAttempted = true;
		if (!State->bExpectCaptureSuccess)
		{
			Test->TestFalse(
				TEXT("NullRHI PIE capture is rejected instead of returning a false source"),
				Capture.bSuccess);
			Test->TestEqual(
				TEXT("NullRHI PIE capture uses the stable capture error"),
				Capture.ErrorCode,
				FString(TEXT("viewport_capture_failed")));
			Test->TestFalse(
				TEXT("Rejected NullRHI capture does not return image data"),
				Capture.Data.IsValid());
		}
		else
		{
			Test->TestTrue(
				TEXT("Capturing the specified live PIE session succeeds"),
				Capture.bSuccess);
			Test->TestTrue(
				TEXT("PIE capture returns structured data"),
				Capture.Data.IsValid());
		}

		if (State->bExpectCaptureSuccess
			&& Capture.bSuccess
			&& Capture.Data.IsValid())
		{
			const TSharedPtr<FJsonObject>& Data = Capture.Data;
			Test->TestEqual(
				TEXT("Capture provenance is the bound PIE Slate viewport"),
				Data->GetStringField(TEXT("captureSource")),
				FString(TEXT("pieSlateViewport")));
			Test->TestEqual(
				TEXT("Capture belongs to the requested session"),
				Data->GetStringField(TEXT("sessionId")),
				Service.GetSessionId());
			Test->TestEqual(
				TEXT("Capture belongs to the requested generation"),
				static_cast<uint64>(
					Data->GetNumberField(TEXT("generation"))),
				Service.GetGeneration());

			const FString WindowHandle =
				Data->GetStringField(TEXT("windowHandle"));
			Test->TestTrue(
				TEXT("Capture reports a non-null native window handle"),
				!WindowHandle.IsEmpty()
					&& WindowHandle != TEXT("0x0000000000000000")
					&& WindowHandle != TEXT("0000000000000000"));

			const TSharedPtr<FJsonObject> ViewportRect =
				Data->GetObjectField(TEXT("viewportRect"));
			Test->TestTrue(
				TEXT("PIE viewport rectangle has positive width"),
				ViewportRect->GetNumberField(TEXT("width")) > 0.0);
			Test->TestTrue(
				TEXT("PIE viewport rectangle has positive height"),
				ViewportRect->GetNumberField(TEXT("height")) > 0.0);

			const int32 SourceWidth = static_cast<int32>(
				Data->GetNumberField(TEXT("sourceWidth")));
			const int32 SourceHeight = static_cast<int32>(
				Data->GetNumberField(TEXT("sourceHeight")));
			Test->TestTrue(
				TEXT("Raw PIE capture width is positive"),
				SourceWidth > 0);
			Test->TestTrue(
				TEXT("Raw PIE capture height is positive"),
				SourceHeight > 0);

			TArray<uint8> DecodedImage;
			const FString EncodedImage =
				Data->GetStringField(TEXT("image_base64"));
			Test->TestTrue(
				TEXT("PIE image base64 decodes"),
				FBase64::Decode(EncodedImage, DecodedImage));
			Test->TestTrue(
				TEXT("Decoded PIE image is non-empty"),
				!DecodedImage.IsEmpty());

			const FString PixelSha256 =
				Data->GetStringField(TEXT("pixelSha256"));
			Test->TestTrue(
				TEXT("Raw pixel SHA-256 is populated"),
				PixelSha256.StartsWith(TEXT("sha256:"))
					&& PixelSha256.Len() == 71);
			const TSharedPtr<FJsonObject> RawCapture =
				Data->GetObjectField(TEXT("rawCapture"));
			Test->TestEqual(
				TEXT("Raw metadata SHA-256 matches the capture hash"),
				RawCapture->GetStringField(TEXT("pixelSha256")),
				PixelSha256);
			Test->TestEqual(
				TEXT("Raw metadata width matches the captured source"),
				static_cast<int32>(
					RawCapture->GetNumberField(TEXT("width"))),
				SourceWidth);
			Test->TestEqual(
				TEXT("Raw metadata height matches the captured source"),
				static_cast<int32>(
					RawCapture->GetNumberField(TEXT("height"))),
				SourceHeight);
			const int64 ExpectedPixelCount =
				static_cast<int64>(SourceWidth) * SourceHeight;
			Test->TestEqual(
				TEXT("Raw metadata pixel count matches dimensions"),
				static_cast<int64>(
					RawCapture->GetNumberField(TEXT("pixelCount"))),
				ExpectedPixelCount);
			Test->TestEqual(
				TEXT("Raw metadata byte count matches BGRA8 pixels"),
				static_cast<int64>(
					RawCapture->GetNumberField(TEXT("byteCount"))),
				ExpectedPixelCount * static_cast<int64>(sizeof(FColor)));
		}

		State->Stage = ERuntimePIECaptureStage::WaitingForRenderDrain;
		State->FramesUntilStop = 3;
		State->DeadlineSeconds = Now + 10.0;
		return false;
	}

	if (State->Stage == ERuntimePIECaptureStage::WaitingForRenderDrain)
	{
		if (Now >= State->DeadlineSeconds)
		{
			Test->AddError(
				TEXT("Timed out draining render work after the PIE capture."));
			StopRuntimePIECaptureSession(*State);
			State->Stage = ERuntimePIECaptureStage::WaitingForShutdown;
			State->DeadlineSeconds = Now + 10.0;
			return false;
		}
		if (--State->FramesUntilStop > 0)
		{
			return false;
		}

		FlushRenderingCommands();
		StopRuntimePIECaptureSession(*State);
		State->Stage = ERuntimePIECaptureStage::WaitingForShutdown;
		State->DeadlineSeconds = Now + 10.0;
		return false;
	}

	if (!GEditor
		|| (!GEditor->IsPlayingSessionInEditor()
			&& !GEditor->IsPlaySessionRequestQueued()))
	{
		Test->TestTrue(
			TEXT("Real PIE capture was attempted before shutdown"),
			State->bCaptureAttempted);
		State->Controller.Reset();
		return true;
	}
	if (Now >= State->DeadlineSeconds)
	{
		Test->AddError(
			TEXT("Timed out stopping PIE after the capture regression."));
		if (GEditor && GEditor->IsPlayingSessionInEditor())
		{
			GEditor->RequestEndPlayMap();
		}
		State->Controller.Reset();
		return true;
	}
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeKeyStateReleasedOnSessionEndTest,
	"UE_AI_integration.Runtime.Input.KeyStateReleasedOnSessionEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeKeyStateReleasedOnSessionEndTest::RunTest(
	const FString& Parameters)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Slate is required for the runtime key teardown regression."));
		return false;
	}

	TSharedPtr<SRuntimeKeyRecorder> Recorder;
	const TSharedRef<SWindow> Window =
		SNew(SWindow)
		.Title(FText::FromString(TEXT("UE AI Key Teardown Regression")))
		.ScreenPosition(FVector2D(320.0, 320.0))
		.ClientSize(FVector2D(360.0, 180.0))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		[
			SAssignNew(Recorder, SRuntimeKeyRecorder)
		];
	FSlateApplication::Get().AddWindow(Window, false);
	ON_SCOPE_EXIT
	{
		Window->RequestDestroyWindow();
		FSlateApplication::Get().Tick();
	};
	FSlateApplication::Get().Tick();
	Window->SlatePrepass();
	TestTrue(
		TEXT("Key recorder receives keyboard focus"),
		FSlateApplication::Get().SetKeyboardFocus(
			Recorder,
			EFocusCause::SetDirectly));

	FRuntimeSceneService Service;
	Service.PrepareNextSession();
	Service.BeginSession();
	auto PressKey = [&Service](const FString& KeyName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("action"), TEXT("press"));
		Params->SetStringField(TEXT("key"), KeyName);
		return Service.KeyInput(Params);
	};

	const FRuntimeServiceResult ShiftPress = PressKey(TEXT("LeftShift"));
	const FRuntimeServiceResult WPress = PressKey(TEXT("W"));
	TestTrue(TEXT("LeftShift press dispatches through Slate"), ShiftPress.bSuccess);
	TestTrue(TEXT("W press dispatches through Slate"), WPress.bSuccess);
	TestEqual(
		TEXT("Service tracks both independently pressed keys"),
		Service.GetPressedKeyCountForTesting(),
		2);
	TestTrue(
		TEXT("Recorder observes LeftShift held during W down"),
		Recorder->bWObservedShift);
	TestTrue(
		TEXT("Recorder holds LeftShift before session teardown"),
		Recorder->HeldKeys.Contains(EKeys::LeftShift));
	TestTrue(
		TEXT("Recorder holds W before session teardown"),
		Recorder->HeldKeys.Contains(EKeys::W));

	Service.EndSession();
	TestEqual(
		TEXT("Session teardown clears service-owned key state"),
		Service.GetPressedKeyCountForTesting(),
		0);
	TestEqual(
		TEXT("Session teardown leaves no key held by Slate target"),
		Recorder->HeldKeys.Num(),
		0);
	TestEqual(
		TEXT("Session teardown emits one key-up per pressed key"),
		Recorder->KeyUpOrder.Num(),
		2);
	if (Recorder->KeyUpOrder.Num() == 2)
	{
		TestEqual(
			TEXT("Keys release in reverse press order: W first"),
			Recorder->KeyUpOrder[0],
			EKeys::W);
		TestEqual(
			TEXT("Keys release in reverse press order: Shift last"),
			Recorder->KeyUpOrder[1],
			EKeys::LeftShift);
	}

	Service.EndSession();
	TestEqual(
		TEXT("Repeated teardown does not emit duplicate key-up events"),
		Recorder->KeyUpOrder.Num(),
		2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeRealPIEViewportCaptureTest,
	"UE_AI_integration.Runtime.Viewport.RealPIECapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeRealPIEViewportCaptureTest::RunTest(const FString& Parameters)
{
	if (!GEditor)
	{
		AddError(TEXT("GEditor is required for a real PIE capture."));
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("Slate is required for a real PIE capture."));
		return false;
	}
	if (GEditor->IsPlayingSessionInEditor()
		|| GEditor->IsPlaySessionRequestQueued())
	{
		AddError(TEXT("A PIE session was already active before the capture test."));
		return false;
	}
	if (!FApp::CanEverRender())
	{
		AddInfo(
			TEXT("Real PIE capture requires a rendered Automation run; NullRHI is covered by the separate rejection regression."));
		return true;
	}

	const FRuntimePIECaptureStateRef State =
		MakeShared<FRuntimePIECaptureState, ESPMode::ThreadSafe>();
	State->Controller = MakeShared<FPIESessionController>();
	State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0;
	const UEAIIntegration::Infrastructure::FPIEControlResult StartResult =
		State->Controller->Start();
	TestTrue(
		TEXT("PIE capture regression queues standard embedded PIE startup"),
		StartResult.bSuccess && StartResult.bRequested);
	if (!StartResult.bSuccess)
	{
		State->Controller.Reset();
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(
		FWaitForRuntimePIECapture(State, this));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeNullRHIPIEViewportCaptureTest,
	"UE_AI_integration.Runtime.Viewport.NullRHIRejectsCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeNullRHIPIEViewportCaptureTest::RunTest(const FString& Parameters)
{
	if (FApp::CanEverRender())
	{
		AddInfo(
			TEXT("NullRHI rejection is only applicable to non-rendering Automation runs."));
		return true;
	}
	if (!GEditor)
	{
		AddError(TEXT("GEditor is required for the NullRHI PIE capture regression."));
		return false;
	}
	if (GEditor->IsPlayingSessionInEditor()
		|| GEditor->IsPlaySessionRequestQueued())
	{
		AddError(TEXT("A PIE session was already active before the NullRHI capture test."));
		return false;
	}

	const FRuntimePIECaptureStateRef State =
		MakeShared<FRuntimePIECaptureState, ESPMode::ThreadSafe>();
	State->Controller = MakeShared<FPIESessionController>();
	State->bExpectCaptureSuccess = false;
	State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0;
	const UEAIIntegration::Infrastructure::FPIEControlResult StartResult =
		State->Controller->Start();
	TestTrue(
		TEXT("NullRHI capture regression queues embedded PIE startup"),
		StartResult.bSuccess && StartResult.bRequested);
	if (!StartResult.bSuccess)
	{
		State->Controller.Reset();
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(
		FWaitForRuntimePIECapture(State, this));
	return true;
}

#endif
