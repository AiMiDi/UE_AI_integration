#pragma once

#include "CoreMinimal.h"

class APlayerController;
class SWidget;
class UWidget;

namespace UEAIIntegration::Infrastructure
{
struct FRuntimeServiceResult;

/**
 * Centralized runtime input adapter.
 *
 * Off-thread targeted widget actions use AutomationDriver with a locator
 * created from UWidget::GetCachedWidget(). Game-thread calls, absolute
 * coordinates, and unsupported driver actions use the centralized Slate event
 * path so a synchronous driver action can never wait on the thread advancing
 * that action.
 */
class FSlateRuntimeInputService
{
public:
	~FSlateRuntimeInputService();

	FRuntimeServiceResult Focus(UWidget* Widget, uint32 UserIndex);
	FRuntimeServiceResult Pointer(
		const FString& Action,
		UWidget* TargetWidget,
		const TOptional<FVector2D>& Position,
		const TOptional<FVector2D>& EndPosition,
		const FString& Button,
		float WheelDelta = 0.0f);
	FRuntimeServiceResult Key(
		const FString& Action,
		const FString& KeyName,
		const FString& Text,
		const TArray<FString>& KeyNames,
		UWidget* TargetWidget);
	FRuntimeServiceResult SetPlayerInputMode(
		APlayerController* PlayerController,
		const FString& Mode,
		UWidget* FocusWidget,
		const FString& LockMouse,
		const TOptional<bool>& ShowCursor);

	/**
	 * Release every button pressed through this service and clear Slate pointer
	 * capture. This is safe to call repeatedly and is required on PIE teardown
	 * and asynchronous-sequence failure paths.
	 */
	void ReleasePointerState();

	/**
	 * Release every key in reverse press order, then release pointer state.
	 * Repeated calls are idempotent. Session transitions and asynchronous
	 * cancellation must use this unified teardown entry point.
	 */
	void ReleaseInputState();

#if WITH_DEV_AUTOMATION_TESTS
	int32 GetPressedPointerButtonCountForTesting() const
	{
		return PressedMouseButtons.Num();
	}
	int32 GetPressedKeyCountForTesting() const
	{
		return PressedKeys.Num();
	}
#endif

private:
	FModifierKeysState BuildModifierKeysState() const;
	bool SendKey(const FKey& Key, bool bPressed);
	void TrackKeyPressed(const FKey& Key);
	void TrackKeyReleased(const FKey& Key);
	bool MoveCursor(const FVector2D& Position);
	bool SendMouseButton(const FVector2D& Position, const FKey& Button, bool bPressed);
	bool SendMouseDoubleClick(const FVector2D& Position, const FKey& Button);
	bool SendMouseWheel(const FVector2D& Position, float WheelDelta);
	FVector2D LastCursorPosition = FVector2D::ZeroVector;
	TSet<FKey> PressedMouseButtons;
	TSet<FKey> PressedKeys;
	TArray<FKey> PressedKeysInOrder;
};
}
