#include "Infrastructure/Runtime/SlateRuntimeInputService.h"

#include "Components/Widget.h"
#include "IAutomationDriver.h"
#include "IAutomationDriverModule.h"
#include "IDriverElement.h"
#include "IDriverSequence.h"
#include "LocateBy.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "Layout/WidgetPath.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
FRuntimeServiceResult InputError(const FString& Message)
{
	return FRuntimeServiceResult::Error(
		TEXT("input_dispatch_failed"),
		Message,
		422);
}

TSharedPtr<FJsonObject> MakeInputResult(
	const FString& Action,
	bool bHandled,
	const FString& Backend = TEXT("slate"))
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action"), Action);
	Data->SetBoolField(TEXT("dispatched"), true);
	Data->SetBoolField(TEXT("handled"), bHandled);
	Data->SetStringField(TEXT("backend"), Backend);
	return Data;
}

FKey MouseButtonFromString(const FString& Button)
{
	if (Button.Equals(TEXT("right"), ESearchCase::IgnoreCase))
	{
		return EKeys::RightMouseButton;
	}
	if (Button.Equals(TEXT("middle"), ESearchCase::IgnoreCase))
	{
		return EKeys::MiddleMouseButton;
	}
	return EKeys::LeftMouseButton;
}

EMouseButtons::Type DriverMouseButtonFromString(const FString& Button)
{
	if (Button.Equals(TEXT("right"), ESearchCase::IgnoreCase))
	{
		return EMouseButtons::Right;
	}
	if (Button.Equals(TEXT("middle"), ESearchCase::IgnoreCase))
	{
		return EMouseButtons::Middle;
	}
	return EMouseButtons::Left;
}

EMouseLockMode MouseLockFromString(const FString& LockMouse)
{
	if (LockMouse.Equals(TEXT("lockAlways"), ESearchCase::IgnoreCase))
	{
		return EMouseLockMode::LockAlways;
	}
	if (LockMouse.Equals(TEXT("lockOnCapture"), ESearchCase::IgnoreCase))
	{
		return EMouseLockMode::LockOnCapture;
	}
	return EMouseLockMode::DoNotLock;
}

TOptional<FVector2D> GetWidgetCenter(UWidget* Widget)
{
	if (!Widget || !Widget->GetCachedWidget().IsValid())
	{
		return {};
	}

	const FGeometry& Geometry = Widget->GetCachedGeometry();
	return FVector2D(Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f));
}

IAutomationDriverModule* LoadAutomationDriverModule()
{
	return FModuleManager::LoadModulePtr<IAutomationDriverModule>(
		TEXT("AutomationDriver"));
}

TSharedRef<IElementLocator, ESPMode::ThreadSafe> MakeWidgetLocator(UWidget* Widget)
{
	const TWeakPtr<SWidget> WeakWidget = Widget ? Widget->GetCachedWidget() : nullptr;
	return By::WidgetLambda(
		[WeakWidget](TArray<TSharedRef<SWidget>>& OutWidgets)
		{
			if (const TSharedPtr<SWidget> PinnedWidget = WeakWidget.Pin())
			{
				OutWidgets.Add(PinnedWidget.ToSharedRef());
			}
		});
}
}

FRuntimeServiceResult FSlateRuntimeInputService::Focus(UWidget* Widget, uint32 UserIndex)
{
	if (!FSlateApplication::IsInitialized())
	{
		return InputError(TEXT("Slate application is not initialized."));
	}
	if (!Widget)
	{
		return InputError(TEXT("Focus target is not a UWidget."));
	}

	const TSharedPtr<SWidget> CachedWidget = Widget->GetCachedWidget();
	if (!CachedWidget.IsValid())
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("Widget has no cached Slate widget. It may not be constructed or visible."),
			422);
	}

	const bool bFocused = FSlateApplication::Get().SetUserFocus(
		UserIndex,
		CachedWidget,
		EFocusCause::SetDirectly);
	if (!bFocused)
	{
		return InputError(TEXT("Slate rejected the focus request."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("focused"), true);
	Data->SetNumberField(TEXT("userIndex"), UserIndex);
	Data->SetStringField(TEXT("backend"), TEXT("slate"));
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FSlateRuntimeInputService::Pointer(
	const FString& Action,
	UWidget* TargetWidget,
	const TOptional<FVector2D>& Position,
	const TOptional<FVector2D>& EndPosition,
	const FString& Button)
{
	if (!FSlateApplication::IsInitialized())
	{
		return InputError(TEXT("Slate application is not initialized."));
	}

	TOptional<FVector2D> StartPosition = Position;
	if (!StartPosition.IsSet())
	{
		StartPosition = GetWidgetCenter(TargetWidget);
	}
	if (!StartPosition.IsSet())
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("Pointer input requires a screen position or a constructed target widget."),
			422);
	}

	const FKey EffectingButton = MouseButtonFromString(Button);
	bool bHandled = false;
	FString Backend = TEXT("slate");

	// A cached UWidget maps directly to an AutomationDriver locator. Keep
	// coordinate-only input in the Slate fallback because AutomationDriver
	// intentionally exposes element-relative rather than absolute positioning.
	if (TargetWidget && !Position.IsSet())
	{
		const TSharedPtr<SWidget> CachedWidget = TargetWidget->GetCachedWidget();
		if (CachedWidget.IsValid())
		{
			if (IAutomationDriverModule* DriverModule =
					LoadAutomationDriverModule())
			{
				const bool bWasEnabled = DriverModule->IsEnabled();
				if (!bWasEnabled)
				{
					DriverModule->Enable();
				}
				ON_SCOPE_EXIT
				{
					if (!bWasEnabled)
					{
						DriverModule->Disable();
					}
				};

				const TSharedRef<IAutomationDriver, ESPMode::ThreadSafe> Driver =
					DriverModule->CreateDriver();
				const TSharedRef<IElementLocator, ESPMode::ThreadSafe> Locator =
					MakeWidgetLocator(TargetWidget);
				const TSharedRef<IDriverElement, ESPMode::ThreadSafe> Element =
					Driver->FindElement(Locator);
				if (Element->Exists())
				{
					const EMouseButtons::Type DriverButton =
						DriverMouseButtonFromString(Button);
					if (Action == TEXT("move"))
					{
						bHandled = Element->Hover();
					}
					else if (Action == TEXT("down"))
					{
						bHandled = Element->Press(DriverButton);
					}
					else if (Action == TEXT("up"))
					{
						bHandled = Element->Release(DriverButton);
					}
					else if (Action == TEXT("click"))
					{
						bHandled = Element->Click(DriverButton);
					}
					else if (Action == TEXT("doubleClick"))
					{
						bHandled = Element->DoubleClick(DriverButton);
					}
					else if (Action == TEXT("drag") && EndPosition.IsSet())
					{
						const FVector2D Center = GetWidgetCenter(TargetWidget).Get(
							EndPosition.GetValue());
						const FVector2D Offset = EndPosition.GetValue() - Center;
						const TSharedRef<IDriverSequence, ESPMode::ThreadSafe> Sequence =
							Driver->CreateSequence();
						Sequence->Actions()
							.MoveToElement(Locator)
							.Press(DriverButton)
							.MoveByOffset(
								FMath::RoundToInt(Offset.X),
								FMath::RoundToInt(Offset.Y))
							.Release(DriverButton);
						bHandled = Sequence->Perform();
					}
					else if (Action != TEXT("drag"))
					{
						return InputError(
							FString::Printf(
								TEXT("Unsupported pointer action '%s'."),
								*Action));
					}

					Backend = TEXT("automation_driver");
					TSharedPtr<FJsonObject> DriverData =
						MakeInputResult(Action, bHandled, Backend);
					DriverData->SetNumberField(TEXT("x"), StartPosition->X);
					DriverData->SetNumberField(TEXT("y"), StartPosition->Y);
					if (EndPosition.IsSet())
					{
						DriverData->SetNumberField(TEXT("endX"), EndPosition->X);
						DriverData->SetNumberField(TEXT("endY"), EndPosition->Y);
					}
					return FRuntimeServiceResult::Ok(DriverData);
				}
			}
		}
	}

	if (Action == TEXT("move"))
	{
		bHandled = MoveCursor(StartPosition.GetValue());
	}
	else if (Action == TEXT("down"))
	{
		MoveCursor(StartPosition.GetValue());
		bHandled = SendMouseButton(StartPosition.GetValue(), EffectingButton, true);
	}
	else if (Action == TEXT("up"))
	{
		MoveCursor(StartPosition.GetValue());
		bHandled = SendMouseButton(StartPosition.GetValue(), EffectingButton, false);
	}
	else if (Action == TEXT("click") || Action == TEXT("doubleClick"))
	{
		MoveCursor(StartPosition.GetValue());
		const int32 Count = Action == TEXT("doubleClick") ? 2 : 1;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			bHandled |= SendMouseButton(StartPosition.GetValue(), EffectingButton, true);
			bHandled |= SendMouseButton(StartPosition.GetValue(), EffectingButton, false);
		}
	}
	else if (Action == TEXT("drag"))
	{
		if (!EndPosition.IsSet())
		{
			return InputError(TEXT("Drag requires endPosition."));
		}
		MoveCursor(StartPosition.GetValue());
		bHandled |= SendMouseButton(StartPosition.GetValue(), EffectingButton, true);
		bHandled |= MoveCursor(EndPosition.GetValue());
		bHandled |= SendMouseButton(EndPosition.GetValue(), EffectingButton, false);
	}
	else
	{
		return InputError(FString::Printf(TEXT("Unsupported pointer action '%s'."), *Action));
	}

	TSharedPtr<FJsonObject> Data = MakeInputResult(Action, bHandled, Backend);
	Data->SetNumberField(TEXT("x"), StartPosition->X);
	Data->SetNumberField(TEXT("y"), StartPosition->Y);
	if (EndPosition.IsSet())
	{
		Data->SetNumberField(TEXT("endX"), EndPosition->X);
		Data->SetNumberField(TEXT("endY"), EndPosition->Y);
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FSlateRuntimeInputService::Key(
	const FString& Action,
	const FString& KeyName,
	const FString& Text,
	const TArray<FString>& KeyNames,
	UWidget* TargetWidget)
{
	if (!FSlateApplication::IsInitialized())
	{
		return InputError(TEXT("Slate application is not initialized."));
	}

	if (TargetWidget)
	{
		FRuntimeServiceResult FocusResult = Focus(TargetWidget, 0);
		if (!FocusResult.bSuccess)
		{
			return FocusResult;
		}
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	bool bHandled = false;
	const FModifierKeysState NoModifiers;

	if (TargetWidget && TargetWidget->GetCachedWidget().IsValid())
	{
		if (IAutomationDriverModule* DriverModule =
				LoadAutomationDriverModule())
		{
			const bool bWasEnabled = DriverModule->IsEnabled();
			if (!bWasEnabled)
			{
				DriverModule->Enable();
			}
			ON_SCOPE_EXIT
			{
				if (!bWasEnabled)
				{
					DriverModule->Disable();
				}
			};

			const TSharedRef<IAutomationDriver, ESPMode::ThreadSafe> Driver =
				DriverModule->CreateDriver();
			const TSharedRef<IDriverElement, ESPMode::ThreadSafe> Element =
				Driver->FindElement(MakeWidgetLocator(TargetWidget));
			if (Element->Exists())
			{
				if (Action == TEXT("type"))
				{
					if (Text.IsEmpty())
					{
						return InputError(TEXT("Type requires non-empty text."));
					}
					bHandled = Element->Type(Text);
				}
				else if (Action == TEXT("press") || Action == TEXT("release"))
				{
					const FKey Key{FName(*KeyName)};
					if (!Key.IsValid())
					{
						return InputError(
							FString::Printf(TEXT("Unknown key '%s'."), *KeyName));
					}
					bHandled = Action == TEXT("press")
						? Element->Press(Key)
						: Element->Release(Key);
				}
				else if (Action == TEXT("chord"))
				{
					TArray<FKey> DriverKeys;
					for (const FString& Name : KeyNames)
					{
						const FKey Key{FName(*Name)};
						if (!Key.IsValid())
						{
							return InputError(
								FString::Printf(TEXT("Unknown key '%s'."), *Name));
						}
						DriverKeys.Add(Key);
					}
					if (DriverKeys.Num() == 2)
					{
						bHandled = Element->TypeChord(DriverKeys[0], DriverKeys[1]);
					}
					else if (DriverKeys.Num() == 3)
					{
						bHandled = Element->TypeChord(
							DriverKeys[0],
							DriverKeys[1],
							DriverKeys[2]);
					}
					else if (DriverKeys.Num() < 2)
					{
						return InputError(TEXT("Chord requires at least two keys."));
					}
					else
					{
						// Four-key chords are kept on the Slate fallback below.
						bHandled = false;
					}
				}
				else
				{
					return InputError(
						FString::Printf(TEXT("Unsupported key action '%s'."), *Action));
				}

				if (Action != TEXT("chord") || KeyNames.Num() <= 3)
				{
					return FRuntimeServiceResult::Ok(
						MakeInputResult(
							Action,
							bHandled,
							TEXT("automation_driver")));
				}
			}
		}
	}

	if (Action == TEXT("type"))
	{
		if (Text.IsEmpty())
		{
			return InputError(TEXT("Type requires non-empty text."));
		}
		for (TCHAR Character : Text)
		{
			bHandled |= Slate.ProcessKeyCharEvent(
				FCharacterEvent(Character, NoModifiers, 0, false));
		}
	}
	else if (Action == TEXT("chord"))
	{
		if (KeyNames.Num() < 2 || KeyNames.Num() > 4)
		{
			return InputError(TEXT("Chord requires between two and four keys."));
		}
		TArray<FKey> Keys;
		for (const FString& Name : KeyNames)
		{
			const FKey Key{FName(*Name)};
			if (!Key.IsValid())
			{
				return InputError(FString::Printf(TEXT("Unknown key '%s'."), *Name));
			}
			Keys.Add(Key);
		}
		for (const FKey& Key : Keys)
		{
			bHandled |= Slate.ProcessKeyDownEvent(FKeyEvent(Key, NoModifiers, 0, false, 0, 0));
		}
		for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
		{
			bHandled |= Slate.ProcessKeyUpEvent(FKeyEvent(Keys[Index], NoModifiers, 0, false, 0, 0));
		}
	}
	else
	{
		const FKey Key{FName(*KeyName)};
		if (!Key.IsValid())
		{
			return InputError(FString::Printf(TEXT("Unknown key '%s'."), *KeyName));
		}
		const FKeyEvent Event(Key, NoModifiers, 0, false, 0, 0);
		if (Action == TEXT("press"))
		{
			bHandled = Slate.ProcessKeyDownEvent(Event);
		}
		else if (Action == TEXT("release"))
		{
			bHandled = Slate.ProcessKeyUpEvent(Event);
		}
		else
		{
			return InputError(FString::Printf(TEXT("Unsupported key action '%s'."), *Action));
		}
	}

	return FRuntimeServiceResult::Ok(MakeInputResult(Action, bHandled));
}

FRuntimeServiceResult FSlateRuntimeInputService::SetPlayerInputMode(
	APlayerController* PlayerController,
	const FString& Mode,
	UWidget* FocusWidget,
	const FString& LockMouse,
	const TOptional<bool>& ShowCursor)
{
	if (!PlayerController)
	{
		return InputError(TEXT("Player controller was not found."));
	}

	const TSharedPtr<SWidget> SlateWidget =
		FocusWidget ? FocusWidget->GetCachedWidget() : TSharedPtr<SWidget>();
	const EMouseLockMode MouseLock = MouseLockFromString(LockMouse);

	if (Mode == TEXT("gameOnly"))
	{
		PlayerController->SetInputMode(FInputModeGameOnly());
	}
	else if (Mode == TEXT("uiOnly"))
	{
		FInputModeUIOnly InputMode;
		InputMode.SetLockMouseToViewportBehavior(MouseLock);
		if (SlateWidget.IsValid())
		{
			InputMode.SetWidgetToFocus(SlateWidget);
		}
		PlayerController->SetInputMode(InputMode);
	}
	else if (Mode == TEXT("gameAndUi"))
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetLockMouseToViewportBehavior(MouseLock);
		if (SlateWidget.IsValid())
		{
			InputMode.SetWidgetToFocus(SlateWidget);
		}
		PlayerController->SetInputMode(InputMode);
	}
	else
	{
		return InputError(FString::Printf(TEXT("Unsupported input mode '%s'."), *Mode));
	}

	if (ShowCursor.IsSet())
	{
		PlayerController->bShowMouseCursor = ShowCursor.GetValue();
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mode"), Mode);
	Data->SetStringField(TEXT("lockMouse"), LockMouse);
	Data->SetBoolField(TEXT("showCursor"), PlayerController->bShowMouseCursor);
	Data->SetStringField(TEXT("backend"), TEXT("player_controller"));
	return FRuntimeServiceResult::Ok(Data);
}

bool FSlateRuntimeInputService::MoveCursor(const FVector2D& Position)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	Slate.SetCursorPos(Position);
	const FPointerEvent Event(
		0,
		Position,
		LastCursorPosition,
		PressedMouseButtons,
		FKey(),
		0.0f,
		FModifierKeysState());
	LastCursorPosition = Position;
	return Slate.ProcessMouseMoveEvent(Event, true);
}

bool FSlateRuntimeInputService::SendMouseButton(
	const FVector2D& Position,
	const FKey& Button,
	bool bPressed)
{
	FSlateApplication& Slate = FSlateApplication::Get();
	if (bPressed)
	{
		PressedMouseButtons.Add(Button);
	}
	else
	{
		PressedMouseButtons.Remove(Button);
	}

	const FPointerEvent Event(
		0,
		Position,
		LastCursorPosition,
		PressedMouseButtons,
		Button,
		0.0f,
		FModifierKeysState());
	LastCursorPosition = Position;

	if (!bPressed)
	{
		return Slate.ProcessMouseButtonUpEvent(Event);
	}

	const FWidgetPath Path = Slate.LocateWindowUnderMouse(
		Position,
		Slate.GetInteractiveTopLevelWindows());
	if (!Path.IsValid())
	{
		return false;
	}
	return Slate.ProcessMouseButtonDownEvent(Path.GetWindow()->GetNativeWindow(), Event);
}
}
