#if WITH_DEV_AUTOMATION_TESTS

#include "Components/Button.h"
#include "Components/EditableText.h"
#include "Framework/Application/SlateApplication.h"
#include "Infrastructure/Runtime/SlateRuntimeInputService.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"

using UEAIIntegration::Infrastructure::FRuntimeSceneService;
using UEAIIntegration::Infrastructure::FRuntimeServiceResult;
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
	return true;
}

#endif
