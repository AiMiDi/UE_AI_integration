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
	FRuntimeServiceResult Focus(UWidget* Widget, uint32 UserIndex);
	FRuntimeServiceResult Pointer(
		const FString& Action,
		UWidget* TargetWidget,
		const TOptional<FVector2D>& Position,
		const TOptional<FVector2D>& EndPosition,
		const FString& Button);
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

private:
	bool MoveCursor(const FVector2D& Position);
	bool SendMouseButton(const FVector2D& Position, const FKey& Button, bool bPressed);
	FVector2D LastCursorPosition = FVector2D::ZeroVector;
	TSet<FKey> PressedMouseButtons;
};
}
