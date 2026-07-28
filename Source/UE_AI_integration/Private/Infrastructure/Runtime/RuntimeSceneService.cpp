#include "Infrastructure/Runtime/RuntimeSceneService.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericWindow.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Infrastructure/Runtime/SlateRuntimeInputService.h"
#include "Infrastructure/Sha256.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Layout/WidgetPath.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Slate/SceneViewport.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
constexpr int32 DefaultFindLimit = 100;
constexpr int32 MaxFindLimit = 500;
constexpr int32 DefaultWidgetDepth = 32;
constexpr int32 MaxWidgetDepth = 64;
constexpr int32 MaxPointerSequenceActions = 256;
constexpr int32 MaxPointerSequencePoints = 1024;
constexpr int32 MaxRetainedAsyncJobs = 64;
constexpr int32 MaxLogWaitBytes = 256 * 1024;

FRuntimeServiceResult InvalidParams(const FString& Message)
{
	return FRuntimeServiceResult::Error(TEXT("invalid_params"), Message, 422);
}

FRuntimeServiceResult RuntimeObjectNotFound(const FString& Message)
{
	return FRuntimeServiceResult::Error(TEXT("runtime_object_not_found"), Message, 404);
}

FString WorldTypeToString(EWorldType::Type WorldType)
{
	switch (WorldType)
	{
	case EWorldType::Game:
		return TEXT("Game");
	case EWorldType::Editor:
		return TEXT("Editor");
	case EWorldType::PIE:
		return TEXT("PIE");
	case EWorldType::EditorPreview:
		return TEXT("Preview");
	case EWorldType::GamePreview:
		return TEXT("GamePreview");
	case EWorldType::GameRPC:
		return TEXT("GameRPC");
	case EWorldType::Inactive:
		return TEXT("Inactive");
	default:
		return TEXT("None");
	}
}

bool IsMutableWorld(const UWorld* World)
{
	return World &&
		(World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

bool IsQueryableWorld(const UWorld* World)
{
	return World &&
		(World->WorldType == EWorldType::PIE ||
			World->WorldType == EWorldType::Game ||
			World->WorldType == EWorldType::Editor ||
			World->WorldType == EWorldType::EditorPreview ||
			World->WorldType == EWorldType::GamePreview);
}

UWorld* GetObjectWorld(UObject* Object)
{
	if (!Object)
	{
		return nullptr;
	}
	if (UWorld* World = Cast<UWorld>(Object))
	{
		return World;
	}
	return Object->GetWorld();
}

TSharedPtr<FJsonObject> VectorToJson(const FVector2D& Vector)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Vector.X);
	Result->SetNumberField(TEXT("y"), Vector.Y);
	return Result;
}

bool ReadVector2D(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	TOptional<FVector2D>& OutValue)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Params->TryGetObjectField(FieldName, Object) || !Object || !Object->IsValid())
	{
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	if (!(*Object)->TryGetNumberField(TEXT("x"), X) ||
		!(*Object)->TryGetNumberField(TEXT("y"), Y))
	{
		return false;
	}
	OutValue = FVector2D(X, Y);
	return true;
}

FString VisibilityToString(ESlateVisibility Visibility)
{
	switch (Visibility)
	{
	case ESlateVisibility::Visible:
		return TEXT("visible");
	case ESlateVisibility::Collapsed:
		return TEXT("collapsed");
	case ESlateVisibility::Hidden:
		return TEXT("hidden");
	case ESlateVisibility::HitTestInvisible:
		return TEXT("hitTestInvisible");
	case ESlateVisibility::SelfHitTestInvisible:
		return TEXT("selfHitTestInvisible");
	default:
		return TEXT("unknown");
	}
}

bool IsCallableFunction(const UFunction* Function)
{
	if (!Function)
	{
		return false;
	}
	const bool bCallable =
		Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent);
	const bool bNetworked = Function->HasAnyFunctionFlags(FUNC_Net);
	const bool bLatent = Function->HasMetaData(TEXT("Latent"));
	return bCallable && !bNetworked && !bLatent;
}

bool IsInputParameter(const FProperty* Property)
{
	return Property->HasAnyPropertyFlags(CPF_Parm) &&
		!Property->HasAnyPropertyFlags(CPF_ReturnParm) &&
		!(Property->HasAnyPropertyFlags(CPF_OutParm) &&
			!Property->HasAnyPropertyFlags(CPF_ReferenceParm));
}

bool IsOutputParameter(const FProperty* Property)
{
	return Property->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm);
}

FString GetWorldId(UWorld* World)
{
	if (!GEngine || !World)
	{
		return FString();
	}
	if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
	{
		return Context->ContextHandle.ToString();
	}
	return FString();
}

bool ReadRequiredSession(
	const TSharedPtr<FJsonObject>& Params,
	FString& OutSessionId,
	uint64& OutGeneration)
{
	double GenerationNumber = 0.0;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("sessionId"), OutSessionId)
		|| OutSessionId.IsEmpty()
		|| !Params->TryGetNumberField(TEXT("generation"), GenerationNumber)
		|| GenerationNumber < 1.0
		|| !FMath::IsNearlyEqual(
			GenerationNumber,
			static_cast<double>(static_cast<uint64>(GenerationNumber))))
	{
		return false;
	}
	OutGeneration = static_cast<uint64>(GenerationNumber);
	return true;
}

bool ResolvePIEViewport(
	TSharedPtr<SViewport>& OutViewportWidget,
	TSharedPtr<SWindow>& OutWindow,
	FSceneViewport*& OutSceneViewport,
	FString& OutError)
{
	OutViewportWidget.Reset();
	OutWindow.Reset();
	OutSceneViewport = nullptr;
	if (!GEditor || !GEditor->IsPlayingSessionInEditor())
	{
		OutError = TEXT("PIE is not running.");
		return false;
	}

	FViewport* Viewport = GEditor->GetPIEViewport();
	if (!Viewport)
	{
		OutError = TEXT("The active PIE session has no PIE viewport.");
		return false;
	}
	FViewportClient* Client = Viewport->GetClient();
	UWorld* ViewportWorld = Client ? Client->GetWorld() : nullptr;
	if (!ViewportWorld || ViewportWorld->WorldType != EWorldType::PIE)
	{
		OutError = TEXT("The resolved viewport is not bound to a PIE world.");
		return false;
	}

	OutSceneViewport = static_cast<FSceneViewport*>(Viewport);
	OutViewportWidget = OutSceneViewport->GetViewportWidget().Pin();
	if (!OutViewportWidget.IsValid())
	{
		OutError = TEXT("The PIE FSceneViewport has no live SViewport.");
		OutSceneViewport = nullptr;
		return false;
	}
	if (!FSlateApplication::IsInitialized())
	{
		OutError = TEXT("Slate application is not initialized.");
		OutSceneViewport = nullptr;
		OutViewportWidget.Reset();
		return false;
	}

	OutWindow = FSlateApplication::Get().FindWidgetWindow(
		OutViewportWidget.ToSharedRef());
	if (!OutWindow.IsValid() || !OutWindow->GetNativeWindow().IsValid())
	{
		OutError = TEXT("The PIE SViewport is not attached to a native Slate window.");
		OutSceneViewport = nullptr;
		OutViewportWidget.Reset();
		OutWindow.Reset();
		return false;
	}
	if (OutWindow->GetNativeWindow()->GetOSWindowHandle() == nullptr)
	{
		OutError =
			TEXT("The PIE Slate window has no native OS window handle.");
		OutSceneViewport = nullptr;
		OutViewportWidget.Reset();
		OutWindow.Reset();
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> BuildHitTestEvidence(
	const FVector2D& Position,
	const TSharedPtr<SWidget>& TargetWidget)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("resolvedPosition"), VectorToJson(Position));
	if (!FSlateApplication::IsInitialized())
	{
		Data->SetBoolField(TEXT("pathValid"), false);
		Data->SetBoolField(TEXT("containsTarget"), false);
		Data->SetArrayField(TEXT("hitPath"), {});
		Data->SetStringField(TEXT("hitWidget"), FString());
		return Data;
	}

	FSlateApplication& Slate = FSlateApplication::Get();
	const FWidgetPath Path = Slate.LocateWindowUnderMouse(
		Position,
		Slate.GetInteractiveTopLevelWindows(),
		true);
	TArray<TSharedPtr<FJsonValue>> PathEntries;
	int32 TargetIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
	{
		const TSharedRef<SWidget> Widget = Path.Widgets[Index].Widget;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetStringField(TEXT("type"), Widget->GetTypeAsString());
		const bool bTarget = TargetWidget.IsValid() && Widget == TargetWidget;
		Entry->SetBoolField(TEXT("target"), bTarget);
		if (bTarget)
		{
			TargetIndex = Index;
		}
		PathEntries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	Data->SetBoolField(TEXT("pathValid"), Path.IsValid());
	Data->SetBoolField(TEXT("containsTarget"), TargetIndex != INDEX_NONE);
	Data->SetNumberField(TEXT("targetIndex"), TargetIndex);
	Data->SetArrayField(TEXT("hitPath"), PathEntries);
	Data->SetStringField(
		TEXT("hitWidget"),
		Path.Widgets.Num() > 0
			? Path.Widgets.Last().Widget->GetTypeAsString()
			: FString());
	const TSharedPtr<SWidget> Focused = Slate.GetKeyboardFocusedWidget();
	Data->SetStringField(
		TEXT("focus"),
		Focused.IsValid() ? Focused->GetTypeAsString() : FString());
	Data->SetStringField(
		TEXT("captureOwner"),
		Slate.GetMouseCaptureWindow()
			? FString::Printf(TEXT("%p"), Slate.GetMouseCaptureWindow())
			: FString());
	return Data;
}

FString FindActiveEditorLogPath()
{
	const FString LogDirectory =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
	const TArray<FString> Preferred = {
		FPaths::Combine(
			LogDirectory,
			FString(FApp::GetProjectName()) + TEXT(".log")),
		FPaths::Combine(LogDirectory, TEXT("UnrealEditor.log"))
	};
	for (const FString& Candidate : Preferred)
	{
		if (IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}
	}

	TArray<FString> LogFiles;
	IFileManager::Get().FindFiles(
		LogFiles,
		*FPaths::Combine(LogDirectory, TEXT("*.log")),
		true,
		false);
	FString NewestPath;
	FDateTime NewestTimestamp = FDateTime::MinValue();
	for (const FString& Name : LogFiles)
	{
		const FString Candidate = FPaths::Combine(LogDirectory, Name);
		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Candidate);
		if (NewestPath.IsEmpty() || Timestamp > NewestTimestamp)
		{
			NewestPath = Candidate;
			NewestTimestamp = Timestamp;
		}
	}
	return NewestPath;
}

bool ReadLogSince(
	const FString& Path,
	int64& InOutCursor,
	FString& OutContent,
	bool& OutRotated)
{
	OutContent.Reset();
	OutRotated = false;
	if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
	{
		return false;
	}
	const int64 FileSize = IFileManager::Get().FileSize(*Path);
	if (FileSize < 0)
	{
		return false;
	}
	if (FileSize < InOutCursor)
	{
		InOutCursor = 0;
		OutRotated = true;
	}
	const int64 Available = FileSize - InOutCursor;
	if (Available <= 0)
	{
		return true;
	}
	const int64 ReadStart = Available > MaxLogWaitBytes
		? FileSize - MaxLogWaitBytes
		: InOutCursor;
	const int32 BytesToRead = static_cast<int32>(
		FMath::Min<int64>(MaxLogWaitBytes, FileSize - ReadStart));
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(BytesToRead + 1);
	TUniquePtr<IFileHandle> File(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!File
		|| !File->Seek(ReadStart)
		|| (BytesToRead > 0 && !File->Read(Bytes.GetData(), BytesToRead)))
	{
		return false;
	}
	Bytes[BytesToRead] = 0;
	const FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		BytesToRead);
	OutContent = FString(Converted.Length(), Converted.Get());
	InOutCursor = FileSize;
	return true;
}

void ResizeRuntimePixels(
	const TArray<FColor>& Source,
	int32 SourceWidth,
	int32 SourceHeight,
	TArray<FColor>& Destination,
	int32 DestinationWidth,
	int32 DestinationHeight)
{
	Destination.SetNumUninitialized(DestinationWidth * DestinationHeight);
	const double ScaleX =
		static_cast<double>(SourceWidth) / DestinationWidth;
	const double ScaleY =
		static_cast<double>(SourceHeight) / DestinationHeight;
	for (int32 Y = 0; Y < DestinationHeight; ++Y)
	{
		for (int32 X = 0; X < DestinationWidth; ++X)
		{
			const int32 SourceX = FMath::Clamp(
				static_cast<int32>(X * ScaleX),
				0,
				SourceWidth - 1);
			const int32 SourceY = FMath::Clamp(
				static_cast<int32>(Y * ScaleY),
				0,
				SourceHeight - 1);
			Destination[Y * DestinationWidth + X] =
				Source[SourceY * SourceWidth + SourceX];
		}
	}
}

struct FRuntimePointerStep
{
	FString Action;
	FString Button = TEXT("left");
	TArray<FVector2D> Points;
	bool bUseTargetCenter = false;
	float WheelDelta = 0.0f;
	int32 DurationMs = 0;
	int32 NextPointIndex = 0;
	double StartSeconds = -1.0;
	double EndSeconds = -1.0;
	TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
};

struct FRuntimePointerSequence
{
	FString Id;
	FString SessionId;
	uint64 Generation = 0;
	FString CoordinateSpace = TEXT("screenAbsolute");
	bool bRequireTargetHit = false;
	TWeakObjectPtr<UWidget> TargetWidget;
	TArray<FRuntimePointerStep> Steps;
	int32 CurrentStepIndex = 0;
	FString Status = TEXT("running");
	FString ErrorCode;
	FString ErrorMessage;
	double AcceptedSeconds = 0.0;
	double CompletedSeconds = 0.0;
};

struct FRuntimeWaitJob
{
	FString Id;
	FString SessionId;
	uint64 Generation = 0;
	FString PredicateType;
	TSharedPtr<FJsonObject> Predicate;
	FString Status = TEXT("running");
	FString ErrorCode;
	FString ErrorMessage;
	double AcceptedSeconds = 0.0;
	double DeadlineSeconds = 0.0;
	double NextPollSeconds = 0.0;
	double CompletedSeconds = 0.0;
	double PollIntervalSeconds = 0.05;
	FString LogPath;
	int64 LogCursor = 0;
	FString LogWindow;
	bool bLogReadSucceeded = false;
	bool bLogReadFailed = false;
	TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
};
}

FRuntimeServiceResult FRuntimeServiceResult::Ok(const TSharedPtr<FJsonObject>& InData)
{
	FRuntimeServiceResult Result;
	Result.Data = InData;
	return Result;
}

FRuntimeServiceResult FRuntimeServiceResult::Error(
	const FString& InCode,
	const FString& InMessage,
	int32 InHttpStatus)
{
	FRuntimeServiceResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = InCode;
	Result.ErrorMessage = InMessage;
	Result.HttpStatus = InHttpStatus;
	return Result;
}

class FRuntimeSceneService::FImpl
{
public:
	void TerminateAsyncForSessionEnd()
	{
		Input.ReleaseInputState();
		ActivePointerSequenceId.Reset();
		for (TPair<FString, FRuntimePointerSequence>& Pair : PointerSequences)
		{
			FRuntimePointerSequence& Sequence = Pair.Value;
			if (Sequence.Status == TEXT("running"))
			{
				Sequence.Status = TEXT("cancelled");
				Sequence.ErrorCode = TEXT("stale_session_handle");
				Sequence.ErrorMessage =
					TEXT("PIE ended before the pointer sequence completed.");
				Sequence.CompletedSeconds = FPlatformTime::Seconds();
			}
		}
		for (TPair<FString, FRuntimeWaitJob>& Pair : WaitJobs)
		{
			FRuntimeWaitJob& Job = Pair.Value;
			if (Job.Status == TEXT("running")
				&& Job.PredicateType != TEXT("generationChanged"))
			{
				Job.Status = TEXT("failed");
				Job.ErrorCode = TEXT("stale_session_handle");
				Job.ErrorMessage =
					TEXT("PIE ended before the wait predicate completed.");
				Job.CompletedSeconds = FPlatformTime::Seconds();
			}
		}
	}

	void PrepareNextSession()
	{
		if (bSessionPrepared)
		{
			return;
		}

		TerminateAsyncForSessionEnd();
		++Generation;
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Objects.Reset();
		bSessionActive = false;
		bSessionPrepared = true;
		bPaused = false;
	}

	void BeginSession()
	{
		if (bSessionActive)
		{
			return;
		}
		if (!bSessionPrepared)
		{
			PrepareNextSession();
		}
		bSessionPrepared = false;
		bSessionActive = true;
		bPaused = false;
		Objects.Reset();
	}

	void CancelPreparedSession()
	{
		if (bSessionPrepared)
		{
			TerminateAsyncForSessionEnd();
			Objects.Reset();
			bSessionPrepared = false;
			bSessionActive = false;
			bPaused = false;
		}
	}

	void EndSession()
	{
		TerminateAsyncForSessionEnd();
		Objects.Reset();
		bSessionActive = false;
		bPaused = false;
	}

	FRuntimeServiceResult Resolve(
		const TSharedPtr<FJsonObject>& ObjectRef,
		UObject*& OutObject) const
	{
		OutObject = nullptr;
		if (!ObjectRef.IsValid())
		{
			return InvalidParams(TEXT("Missing objectRef."));
		}

		FString RequestedSession;
		FString ObjectId;
		double RequestedGeneration = 0.0;
		if (!ObjectRef->TryGetStringField(TEXT("sessionId"), RequestedSession) ||
			!ObjectRef->TryGetNumberField(TEXT("generation"), RequestedGeneration) ||
			!ObjectRef->TryGetStringField(TEXT("objectId"), ObjectId))
		{
			return InvalidParams(
				TEXT("objectRef requires sessionId, generation, and objectId."));
		}

		if (RequestedSession != SessionId ||
			static_cast<uint64>(RequestedGeneration) != Generation)
		{
			return FRuntimeServiceResult::Error(
				TEXT("stale_session_handle"),
				TEXT("The objectRef belongs to a different PIE generation."),
				410);
		}
		if (!bSessionActive)
		{
			return FRuntimeServiceResult::Error(
				TEXT("pie_not_running"),
				TEXT("The referenced PIE session is not running."),
				409);
		}

		FGuid Id;
		if (!FGuid::Parse(ObjectId, Id))
		{
			return InvalidParams(TEXT("objectRef.objectId is not a valid GUID."));
		}
		const TWeakObjectPtr<UObject>* WeakObject = Objects.Find(Id);
		if (!WeakObject || !WeakObject->IsValid())
		{
			return RuntimeObjectNotFound(
				TEXT("The runtime object no longer exists in this PIE session."));
		}

		OutObject = WeakObject->Get();
		return FRuntimeServiceResult::Ok(MakeShared<FJsonObject>());
	}

	FRuntimeServiceResult MakeRef(
		UObject* Object,
		TSharedPtr<FJsonObject>& OutObjectRef)
	{
		OutObjectRef.Reset();
		if (!bSessionActive)
		{
			return FRuntimeServiceResult::Error(
				TEXT("pie_not_running"),
				TEXT("PIE must be running before runtime handles can be created."),
				409);
		}
		if (!IsValid(Object))
		{
			return RuntimeObjectNotFound(TEXT("Cannot create a handle for an invalid object."));
		}
		if (!IsQueryableWorld(GetObjectWorld(Object)))
		{
			return RuntimeObjectNotFound(
				TEXT("Object does not belong to a queryable Editor or runtime world."));
		}

		FGuid ObjectId;
		for (const TPair<FGuid, TWeakObjectPtr<UObject>>& Pair : Objects)
		{
			if (Pair.Value.Get() == Object)
			{
				ObjectId = Pair.Key;
				break;
			}
		}
		if (!ObjectId.IsValid())
		{
			ObjectId = FGuid::NewGuid();
			Objects.Add(ObjectId, Object);
		}

		OutObjectRef = MakeShared<FJsonObject>();
		OutObjectRef->SetStringField(TEXT("sessionId"), SessionId);
		OutObjectRef->SetNumberField(TEXT("generation"), static_cast<double>(Generation));
		OutObjectRef->SetStringField(
			TEXT("objectId"),
			ObjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
		return FRuntimeServiceResult::Ok(OutObjectRef);
	}

	TSharedPtr<FJsonObject> DescribeObject(UObject* Object)
	{
		TSharedPtr<FJsonObject> ObjectRef;
		if (!MakeRef(Object, ObjectRef).bSuccess)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Description = MakeShared<FJsonObject>();
		Description->SetObjectField(TEXT("objectRef"), ObjectRef);
		Description->SetStringField(TEXT("name"), Object->GetName());
		Description->SetStringField(TEXT("path"), Object->GetPathName());
		Description->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		if (UWorld* World = GetObjectWorld(Object))
		{
			Description->SetStringField(TEXT("worldId"), GetWorldId(World));
			Description->SetStringField(TEXT("worldType"), WorldTypeToString(World->WorldType));
			Description->SetBoolField(TEXT("mutable"), IsMutableWorld(World));
		}
		return Description;
	}

	UWorld* FindWorld(const FString& WorldId) const
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (WorldId.IsEmpty() || Context.ContextHandle.ToString() == WorldId)
			{
				if (UWorld* World = Context.World())
				{
					if (IsQueryableWorld(World))
					{
						return World;
					}
				}
			}
		}
		return nullptr;
	}

	bool IsMutationAllowed(UObject* Object, FRuntimeServiceResult& OutError) const
	{
		if (!IsMutableWorld(GetObjectWorld(Object)))
		{
			OutError = FRuntimeServiceResult::Error(
				TEXT("runtime_world_read_only"),
				TEXT("Mutations are allowed only for PIE or Game world objects."),
				403);
			return false;
		}
		return true;
	}

	FRuntimeServiceResult ResolveParamObject(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		UObject*& OutObject) const
	{
		const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
		if (!Params->TryGetObjectField(FieldName, ObjectRef) ||
			!ObjectRef ||
			!ObjectRef->IsValid())
		{
			return InvalidParams(
				FString::Printf(TEXT("Missing '%s' objectRef."), FieldName));
		}
		return Resolve(*ObjectRef, OutObject);
	}

	FRuntimeServiceResult ResolveDelegate(
		const TSharedPtr<FJsonObject>& Params,
		UObject*& OutOwner,
		FMulticastDelegateProperty*& OutProperty,
		void*& OutPropertyValue,
		const FMulticastScriptDelegate*& OutDelegate) const
	{
		OutOwner = nullptr;
		OutProperty = nullptr;
		OutPropertyValue = nullptr;
		OutDelegate = nullptr;

		FRuntimeServiceResult OwnerResult =
			ResolveParamObject(Params, TEXT("objectRef"), OutOwner);
		if (!OwnerResult.bSuccess)
		{
			return OwnerResult;
		}

		FString DelegateName;
		if (!Params->TryGetStringField(TEXT("delegate"), DelegateName) ||
			DelegateName.IsEmpty())
		{
			return InvalidParams(TEXT("Missing 'delegate'."));
		}
		OutProperty = FindFProperty<FMulticastDelegateProperty>(
			OutOwner->GetClass(),
			FName(*DelegateName));
		if (!OutProperty)
		{
			return RuntimeObjectNotFound(
				FString::Printf(TEXT("Dynamic multicast delegate '%s' was not found."), *DelegateName));
		}
		OutPropertyValue = OutProperty->ContainerPtrToValuePtr<void>(OutOwner);
		OutDelegate = OutProperty->GetMulticastDelegate(OutPropertyValue);
		if (!OutDelegate)
		{
			return RuntimeObjectNotFound(
				FString::Printf(TEXT("Delegate '%s' has no runtime value."), *DelegateName));
		}
		return FRuntimeServiceResult::Ok(MakeShared<FJsonObject>());
	}

	FSlateRuntimeInputService Input;
	TMap<FGuid, TWeakObjectPtr<UObject>> Objects;
	TMap<FString, FRuntimePointerSequence> PointerSequences;
	TMap<FString, FRuntimeWaitJob> WaitJobs;
	FString ActivePointerSequenceId;
	FString SessionId;
	uint64 Generation = 0;
	bool bSessionPrepared = false;
	bool bSessionActive = false;
	bool bPaused = false;
};

namespace
{
TSharedPtr<FJsonObject> BuildPointerSequenceData(
	const FRuntimePointerSequence& Sequence)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("sequenceId"), Sequence.Id);
	Data->SetStringField(TEXT("sessionId"), Sequence.SessionId);
	Data->SetNumberField(
		TEXT("generation"),
		static_cast<double>(Sequence.Generation));
	Data->SetStringField(TEXT("status"), Sequence.Status);
	Data->SetBoolField(TEXT("accepted"), true);
	Data->SetNumberField(TEXT("currentStep"), Sequence.CurrentStepIndex);
	Data->SetNumberField(TEXT("stepCount"), Sequence.Steps.Num());
	Data->SetStringField(TEXT("coordinateSpace"), Sequence.CoordinateSpace);
	Data->SetBoolField(TEXT("requireTargetHit"), Sequence.bRequireTargetHit);

	TArray<TSharedPtr<FJsonValue>> Steps;
	for (int32 Index = 0; Index < Sequence.Steps.Num(); ++Index)
	{
		const FRuntimePointerStep& Step = Sequence.Steps[Index];
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetStringField(TEXT("action"), Step.Action);
		Entry->SetStringField(
			TEXT("status"),
			Index < Sequence.CurrentStepIndex
				? TEXT("completed")
				: Index == Sequence.CurrentStepIndex
					&& Sequence.Status == TEXT("running")
					? TEXT("running")
					: TEXT("pending"));
		if (Sequence.Status != TEXT("running")
			&& Index == Sequence.CurrentStepIndex)
		{
			Entry->SetStringField(TEXT("status"), Sequence.Status);
		}
		Entry->SetNumberField(TEXT("durationMs"), Step.DurationMs);
		Entry->SetNumberField(TEXT("pointCount"), Step.Points.Num());
		Entry->SetNumberField(TEXT("dispatchedPoints"), Step.NextPointIndex);
		if (Step.Evidence.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
				Step.Evidence->Values)
			{
				Entry->SetField(Field.Key, Field.Value);
			}
		}
		Steps.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Data->SetArrayField(TEXT("steps"), Steps);
	if (!Sequence.ErrorCode.IsEmpty())
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Sequence.ErrorCode);
		Error->SetStringField(TEXT("message"), Sequence.ErrorMessage);
		Data->SetObjectField(TEXT("error"), Error);
	}
	if (Sequence.CompletedSeconds > 0.0)
	{
		Data->SetNumberField(
			TEXT("elapsedMs"),
			(Sequence.CompletedSeconds - Sequence.AcceptedSeconds) * 1000.0);
	}
	return Data;
}

TSharedPtr<FJsonObject> BuildWaitData(const FRuntimeWaitJob& Job)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("waitId"), Job.Id);
	Data->SetStringField(TEXT("sessionId"), Job.SessionId);
	Data->SetNumberField(
		TEXT("generation"),
		static_cast<double>(Job.Generation));
	Data->SetStringField(TEXT("predicate"), Job.PredicateType);
	Data->SetStringField(TEXT("status"), Job.Status);
	Data->SetBoolField(TEXT("accepted"), true);
	if (Job.Evidence.IsValid())
	{
		Data->SetObjectField(TEXT("evidence"), Job.Evidence);
	}
	if (!Job.ErrorCode.IsEmpty())
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Job.ErrorCode);
		Error->SetStringField(TEXT("message"), Job.ErrorMessage);
		Data->SetObjectField(TEXT("error"), Error);
	}
	if (Job.CompletedSeconds > 0.0)
	{
		Data->SetNumberField(
			TEXT("elapsedMs"),
			(Job.CompletedSeconds - Job.AcceptedSeconds) * 1000.0);
	}
	return Data;
}
}

FRuntimeSceneService::FRuntimeSceneService()
	: Impl(MakeUnique<FImpl>())
{
}

FRuntimeSceneService::~FRuntimeSceneService()
{
	if (Impl)
	{
		Impl->TerminateAsyncForSessionEnd();
	}
}

void FRuntimeSceneService::PrepareNextSession()
{
	Impl->PrepareNextSession();
}

void FRuntimeSceneService::BeginSession()
{
	Impl->BeginSession();
}

void FRuntimeSceneService::CancelPreparedSession()
{
	Impl->CancelPreparedSession();
}

void FRuntimeSceneService::EndSession()
{
	Impl->EndSession();
}

void FRuntimeSceneService::SetPaused(bool bInPaused)
{
	Impl->bPaused = bInPaused;
}

void FRuntimeSceneService::Tick()
{
	const double Now = FPlatformTime::Seconds();

	if (!Impl->ActivePointerSequenceId.IsEmpty())
	{
		FRuntimePointerSequence* Sequence =
			Impl->PointerSequences.Find(Impl->ActivePointerSequenceId);
		if (!Sequence || Sequence->Status != TEXT("running"))
		{
			Impl->ActivePointerSequenceId.Reset();
		}
		else
		{
			auto FailSequence =
				[this, Sequence, Now](
					const FString& Code,
					const FString& Message,
					const FString& Status = TEXT("failed"))
				{
					Impl->Input.ReleaseInputState();
					Sequence->Status = Status;
					Sequence->ErrorCode = Code;
					Sequence->ErrorMessage = Message;
					Sequence->CompletedSeconds = Now;
					Impl->ActivePointerSequenceId.Reset();
				};

			if (!Impl->bSessionActive
				|| Sequence->SessionId != Impl->SessionId
				|| Sequence->Generation != Impl->Generation)
			{
				FailSequence(
					TEXT("stale_session_handle"),
					TEXT("The pointer sequence belongs to a stale PIE session."),
					TEXT("cancelled"));
			}
			else
			{
				int32 DispatchGuard = 0;
				while (Sequence->Status == TEXT("running")
					&& Sequence->CurrentStepIndex < Sequence->Steps.Num()
					&& ++DispatchGuard <= MaxPointerSequencePoints
						+ MaxPointerSequenceActions)
				{
					FRuntimePointerStep& Step =
						Sequence->Steps[Sequence->CurrentStepIndex];
					if (Step.Action == TEXT("cancel"))
					{
						Impl->Input.ReleaseInputState();
						Step.Evidence->SetBoolField(TEXT("handled"), true);
						Step.Evidence->SetBoolField(TEXT("released"), true);
						Sequence->Status = TEXT("cancelled");
						Sequence->CompletedSeconds = Now;
						Impl->ActivePointerSequenceId.Reset();
						break;
					}

					if (Step.StartSeconds < 0.0)
					{
						Step.StartSeconds = Now;
						Step.EndSeconds =
							Now + static_cast<double>(Step.DurationMs) / 1000.0;
					}

					const int32 PointCount = Step.Points.Num();
					bool bDispatchedPoint = false;
					while (Step.NextPointIndex < PointCount)
					{
						const double Fraction = PointCount <= 1
							? 0.0
							: static_cast<double>(Step.NextPointIndex)
								/ static_cast<double>(PointCount - 1);
						const double PointTime =
							Step.StartSeconds
							+ (Step.EndSeconds - Step.StartSeconds) * Fraction;
						if (Now + UE_DOUBLE_SMALL_NUMBER < PointTime)
						{
							break;
						}

						UWidget* TargetWidget = Sequence->TargetWidget.Get();
						TSharedPtr<SWidget> TargetSlateWidget =
							TargetWidget
								? TargetWidget->GetCachedWidget()
								: TSharedPtr<SWidget>();
						if (Sequence->TargetWidget.IsStale()
							|| (TargetWidget && !TargetSlateWidget.IsValid()))
						{
							FailSequence(
								TEXT("widget_not_interactable"),
								TEXT("The pointer sequence target widget is no longer interactable."));
							break;
						}

						const FVector2D RawPoint =
							Step.Points[Step.NextPointIndex];
						FVector2D ResolvedPosition = RawPoint;
						FString ResolveError;
						if (Step.bUseTargetCenter)
						{
							if (!TargetWidget || !TargetSlateWidget.IsValid())
							{
								ResolveError =
									TEXT("A constructed target widget is required for an implicit position.");
							}
							else
							{
								const FGeometry& Geometry =
									TargetWidget->GetCachedGeometry();
								ResolvedPosition = FVector2D(
									Geometry.LocalToAbsolute(
										Geometry.GetLocalSize() * 0.5f));
							}
						}
						else if (Sequence->CoordinateSpace == TEXT("widgetLocal")
							|| Sequence->CoordinateSpace == TEXT("widgetNormalized"))
						{
							if (!TargetWidget || !TargetSlateWidget.IsValid())
							{
								ResolveError =
									TEXT("widgetLocal and widgetNormalized require a target widget.");
							}
							else
							{
								const FGeometry& Geometry =
									TargetWidget->GetCachedGeometry();
								FVector2D Local = RawPoint;
								if (Sequence->CoordinateSpace
									== TEXT("widgetNormalized"))
								{
									if (RawPoint.X < 0.0 || RawPoint.X > 1.0
										|| RawPoint.Y < 0.0 || RawPoint.Y > 1.0)
									{
										ResolveError =
											TEXT("widgetNormalized coordinates must be between 0 and 1.");
									}
									Local *= FVector2D(Geometry.GetLocalSize());
								}
								if (ResolveError.IsEmpty())
								{
									ResolvedPosition = FVector2D(
										Geometry.LocalToAbsolute(Local));
								}
							}
						}
						else if (Sequence->CoordinateSpace == TEXT("window"))
						{
							TSharedPtr<SWindow> Window;
							if (TargetSlateWidget.IsValid())
							{
								Window = FSlateApplication::Get().FindWidgetWindow(
									TargetSlateWidget.ToSharedRef());
							}
							if (!Window.IsValid())
							{
								TSharedPtr<SViewport> PIEViewportWidget;
								FSceneViewport* SceneViewport = nullptr;
								ResolvePIEViewport(
									PIEViewportWidget,
									Window,
									SceneViewport,
									ResolveError);
							}
							if (Window.IsValid())
							{
								ResolvedPosition =
									FVector2D(Window->GetPositionInScreen())
									+ RawPoint;
							}
							else if (ResolveError.IsEmpty())
							{
								ResolveError =
									TEXT("The target is not attached to a Slate window.");
							}
						}
						else if (Sequence->CoordinateSpace
							== TEXT("viewportNormalized"))
						{
							if (RawPoint.X < 0.0 || RawPoint.X > 1.0
								|| RawPoint.Y < 0.0 || RawPoint.Y > 1.0)
							{
								ResolveError =
									TEXT("viewportNormalized coordinates must be between 0 and 1.");
							}
							else
							{
								TSharedPtr<SViewport> PIEViewportWidget;
								TSharedPtr<SWindow> PIEWindow;
								FSceneViewport* SceneViewport = nullptr;
								if (ResolvePIEViewport(
										PIEViewportWidget,
										PIEWindow,
										SceneViewport,
										ResolveError))
								{
									const FGeometry& Geometry =
										PIEViewportWidget->GetCachedGeometry();
									ResolvedPosition = FVector2D(
										Geometry.LocalToAbsolute(
											FVector2D(Geometry.GetLocalSize())
											* RawPoint));
								}
							}
						}
						else if (Sequence->CoordinateSpace
							!= TEXT("screenAbsolute"))
						{
							ResolveError = FString::Printf(
								TEXT("Unsupported coordinateSpace '%s'."),
								*Sequence->CoordinateSpace);
						}
						if (!ResolveError.IsEmpty())
						{
							FailSequence(
								TEXT("invalid_params"),
								ResolveError);
							break;
						}

						TSharedPtr<FJsonObject> HitEvidence =
							BuildHitTestEvidence(
								ResolvedPosition,
								TargetSlateWidget);
						if (Sequence->bRequireTargetHit
							&& !HitEvidence->GetBoolField(TEXT("containsTarget")))
						{
							FailSequence(
								TEXT("widget_not_interactable"),
								TEXT("The resolved pointer position does not hit the required target widget."));
							break;
						}

						const bool bFinalPoint =
							Step.NextPointIndex == PointCount - 1;
						const FString DispatchAction =
							bFinalPoint || Step.Action == TEXT("move")
								? Step.Action
								: TEXT("move");
						FRuntimeServiceResult DispatchResult =
							Impl->Input.Pointer(
								DispatchAction,
								nullptr,
								ResolvedPosition,
								{},
								Step.Button,
								Step.WheelDelta);
						if (!DispatchResult.bSuccess)
						{
							FailSequence(
								DispatchResult.ErrorCode,
								DispatchResult.ErrorMessage);
							break;
						}

						TSharedPtr<FJsonObject> PointEvidence =
							BuildHitTestEvidence(
								ResolvedPosition,
								TargetSlateWidget);
						bool bHandled = false;
						if (DispatchResult.Data.IsValid())
						{
							DispatchResult.Data->TryGetBoolField(
								TEXT("handled"),
								bHandled);
						}
						PointEvidence->SetBoolField(TEXT("handled"), bHandled);
						PointEvidence->SetNumberField(
							TEXT("pointIndex"),
							Step.NextPointIndex);
						PointEvidence->SetStringField(
							TEXT("dispatchedAction"),
							DispatchAction);

						const TArray<TSharedPtr<FJsonValue>>* ExistingPoints =
							nullptr;
						TArray<TSharedPtr<FJsonValue>> PointEvidenceArray;
						if (Step.Evidence->TryGetArrayField(
								TEXT("points"),
								ExistingPoints)
							&& ExistingPoints)
						{
							PointEvidenceArray = *ExistingPoints;
						}
						PointEvidenceArray.Add(
							MakeShared<FJsonValueObject>(PointEvidence));
						Step.Evidence->SetArrayField(
							TEXT("points"),
							PointEvidenceArray);
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
							PointEvidence->Values)
						{
							if (Field.Key != TEXT("pointIndex")
								&& Field.Key != TEXT("dispatchedAction"))
							{
								Step.Evidence->SetField(Field.Key, Field.Value);
							}
						}

						++Step.NextPointIndex;
						bDispatchedPoint = true;
					}

					if (Sequence->Status != TEXT("running"))
					{
						break;
					}
					if (Step.NextPointIndex >= PointCount
						&& Now + UE_DOUBLE_SMALL_NUMBER >= Step.EndSeconds)
					{
						Step.Evidence->SetNumberField(
							TEXT("elapsedMs"),
							(Now - Step.StartSeconds) * 1000.0);
						++Sequence->CurrentStepIndex;
						continue;
					}
					if (!bDispatchedPoint
						|| Now + UE_DOUBLE_SMALL_NUMBER < Step.EndSeconds)
					{
						break;
					}
				}

				if (Sequence->Status == TEXT("running")
					&& Sequence->CurrentStepIndex >= Sequence->Steps.Num())
				{
					Sequence->Status = TEXT("completed");
					Sequence->CompletedSeconds = Now;
					Impl->ActivePointerSequenceId.Reset();
				}
			}
		}
	}

	for (TPair<FString, FRuntimeWaitJob>& Pair : Impl->WaitJobs)
	{
		FRuntimeWaitJob& Job = Pair.Value;
		if (Job.Status != TEXT("running")
			|| (Job.PredicateType != TEXT("generationChanged")
				&& Now + UE_DOUBLE_SMALL_NUMBER < Job.NextPollSeconds))
		{
			continue;
		}
		Job.NextPollSeconds = Now + Job.PollIntervalSeconds;

		auto CompleteWait = [&Job, Now]()
		{
			Job.Status = TEXT("completed");
			Job.CompletedSeconds = Now;
			Job.Evidence->SetBoolField(TEXT("matched"), true);
		};
		auto FailWait =
			[&Job, Now](const FString& Code, const FString& Message)
			{
				Job.Status = TEXT("failed");
				Job.ErrorCode = Code;
				Job.ErrorMessage = Message;
				Job.CompletedSeconds = Now;
				Job.Evidence->SetBoolField(TEXT("matched"), false);
			};

		if (Job.PredicateType == TEXT("generationChanged"))
		{
			if (Job.SessionId != Impl->SessionId
				|| Job.Generation != Impl->Generation)
			{
				Job.Evidence->SetStringField(
					TEXT("currentSessionId"),
					Impl->SessionId);
				Job.Evidence->SetNumberField(
					TEXT("currentGeneration"),
					static_cast<double>(Impl->Generation));
				CompleteWait();
			}
		}
		else if (Job.SessionId != Impl->SessionId
			|| Job.Generation != Impl->Generation)
		{
			FailWait(
				TEXT("stale_session_handle"),
				TEXT("The wait belongs to a stale PIE session."));
		}
		else if (Job.PredicateType == TEXT("pieReady"))
		{
			TSharedPtr<SViewport> ViewportWidget;
			TSharedPtr<SWindow> Window;
			FSceneViewport* SceneViewport = nullptr;
			FString Error;
			if (Impl->bSessionActive
				&& ResolvePIEViewport(
					ViewportWidget,
					Window,
					SceneViewport,
					Error))
			{
				Job.Evidence->SetBoolField(TEXT("viewportReady"), true);
				CompleteWait();
			}
			else
			{
				Job.Evidence->SetBoolField(TEXT("viewportReady"), false);
			}
		}
		else if (Job.PredicateType == TEXT("widgetExists"))
		{
			bool bExists = false;
			const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
			if (Job.Predicate->TryGetObjectField(TEXT("objectRef"), ObjectRef)
				&& ObjectRef
				&& ObjectRef->IsValid())
			{
				UObject* Object = nullptr;
				const FRuntimeServiceResult ResolveResult =
					Impl->Resolve(*ObjectRef, Object);
				if (ResolveResult.bSuccess)
				{
					bExists = IsValid(Cast<UWidget>(Object));
				}
				else if (ResolveResult.ErrorCode == TEXT("stale_session_handle"))
				{
					FailWait(
						ResolveResult.ErrorCode,
						ResolveResult.ErrorMessage);
				}
			}
			else
			{
				FString Name;
				FString ClassName;
				FString Path;
				Job.Predicate->TryGetStringField(TEXT("name"), Name);
				Job.Predicate->TryGetStringField(TEXT("class"), ClassName);
				Job.Predicate->TryGetStringField(TEXT("path"), Path);
				for (TObjectIterator<UWidget> It; It; ++It)
				{
					UWidget* Widget = *It;
					if (!IsValid(Widget)
						|| !IsMutableWorld(GetObjectWorld(Widget)))
					{
						continue;
					}
					const bool bNameMatches =
						Name.IsEmpty()
						|| Widget->GetName().MatchesWildcard(
							Name,
							ESearchCase::IgnoreCase);
					const bool bClassMatches =
						ClassName.IsEmpty()
						|| Widget->GetClass()->GetPathName().MatchesWildcard(
							ClassName,
							ESearchCase::IgnoreCase)
						|| Widget->GetClass()->GetName().MatchesWildcard(
							ClassName,
							ESearchCase::IgnoreCase);
					const bool bPathMatches =
						Path.IsEmpty()
						|| Widget->GetPathName().MatchesWildcard(
							Path,
							ESearchCase::IgnoreCase);
					if (bNameMatches && bClassMatches && bPathMatches)
					{
						bExists = true;
						Job.Evidence->SetStringField(
							TEXT("widgetPath"),
							Widget->GetPathName());
						break;
					}
				}
			}
			if (Job.Status == TEXT("running") && bExists)
			{
				CompleteWait();
			}
		}
		else if (Job.PredicateType == TEXT("propertyEquals"))
		{
			const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
			FString PropertyName;
			const TSharedPtr<FJsonValue> Expected =
				Job.Predicate->TryGetField(TEXT("expected"));
			if (!Job.Predicate->TryGetObjectField(TEXT("objectRef"), ObjectRef)
				|| !ObjectRef
				|| !ObjectRef->IsValid()
				|| !Job.Predicate->TryGetStringField(
					TEXT("property"),
					PropertyName)
				|| !Expected.IsValid())
			{
				FailWait(
					TEXT("invalid_params"),
					TEXT("propertyEquals requires objectRef, property, and expected."));
			}
			else
			{
				UObject* Object = nullptr;
				const FRuntimeServiceResult ResolveResult =
					Impl->Resolve(*ObjectRef, Object);
				if (!ResolveResult.bSuccess)
				{
					if (ResolveResult.ErrorCode == TEXT("stale_session_handle"))
					{
						FailWait(
							ResolveResult.ErrorCode,
							ResolveResult.ErrorMessage);
					}
				}
				else if (FProperty* Property = FindFProperty<FProperty>(
						Object->GetClass(),
						FName(*PropertyName)))
				{
					const void* ValuePtr =
						Property->ContainerPtrToValuePtr<void>(Object);
					const TSharedPtr<FJsonValue> Actual =
						FJsonObjectConverter::UPropertyToJsonValue(
							Property,
							ValuePtr);
					if (Actual.IsValid())
					{
						Job.Evidence->SetField(TEXT("actual"), Actual);
						if (FJsonValue::CompareEqual(
								*Actual,
								*Expected))
						{
							CompleteWait();
						}
					}
				}
				else
				{
					FailWait(
						TEXT("runtime_object_not_found"),
						FString::Printf(
							TEXT("Property '%s' was not found."),
							*PropertyName));
				}
			}
		}
		else if (Job.PredicateType == TEXT("logContains")
			|| Job.PredicateType == TEXT("logNotContains"))
		{
			FString NewContent;
			bool bRotated = false;
			if (ReadLogSince(
					Job.LogPath,
					Job.LogCursor,
					NewContent,
					bRotated))
			{
				Job.bLogReadSucceeded = true;
				Job.LogWindow =
					(Job.LogWindow + NewContent).Right(MaxLogWaitBytes);
				FString Needle;
				Job.Predicate->TryGetStringField(TEXT("text"), Needle);
				const bool bContains =
					Job.LogWindow.Contains(
						Needle,
						ESearchCase::IgnoreCase);
				Job.Evidence->SetBoolField(TEXT("logRotated"), bRotated);
				Job.Evidence->SetNumberField(
					TEXT("cursor"),
					static_cast<double>(Job.LogCursor));
				Job.Evidence->SetBoolField(TEXT("contains"), bContains);
				if (Job.PredicateType == TEXT("logContains") && bContains)
				{
					CompleteWait();
				}
				else if (Job.PredicateType == TEXT("logNotContains")
					&& bContains)
				{
					FailWait(
						TEXT("condition_failed"),
						TEXT("The forbidden log text appeared during the wait window."));
				}
			}
			else
			{
				Job.bLogReadFailed = true;
				Job.Evidence->SetBoolField(TEXT("readAvailable"), false);
			}
		}

		if (Job.Status == TEXT("running")
			&& Now + UE_DOUBLE_SMALL_NUMBER >= Job.DeadlineSeconds)
		{
			const bool bLogPredicate =
				Job.PredicateType == TEXT("logContains")
				|| Job.PredicateType == TEXT("logNotContains");
			if (bLogPredicate
				&& (!Job.bLogReadSucceeded || Job.bLogReadFailed))
			{
				FailWait(
					TEXT("log_unavailable"),
					TEXT("The Editor log could not be read for the complete wait window."));
			}
			else if (Job.PredicateType == TEXT("logNotContains"))
			{
				CompleteWait();
			}
			else
			{
				FailWait(
					TEXT("wait_timeout"),
					TEXT("The runtime wait predicate did not match before timeout."));
			}
		}
	}
}

const FString& FRuntimeSceneService::GetSessionId() const
{
	return Impl->SessionId;
}

uint64 FRuntimeSceneService::GetGeneration() const
{
	return Impl->Generation;
}

bool FRuntimeSceneService::IsSessionActive() const
{
	return Impl->bSessionActive;
}

bool FRuntimeSceneService::IsPaused() const
{
	return Impl->bPaused;
}

FRuntimeServiceResult FRuntimeSceneService::ResolveObjectRef(
	const TSharedPtr<FJsonObject>& ObjectRef,
	UObject*& OutObject) const
{
	return Impl->Resolve(ObjectRef, OutObject);
}

FRuntimeServiceResult FRuntimeSceneService::MakeObjectRef(
	UObject* Object,
	TSharedPtr<FJsonObject>& OutObjectRef)
{
	return Impl->MakeRef(Object, OutObjectRef);
}

FRuntimeServiceResult FRuntimeSceneService::ListWorldContexts(
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Worlds;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || !IsQueryableWorld(World))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("worldId"), Context.ContextHandle.ToString());
			Entry->SetStringField(TEXT("type"), WorldTypeToString(Context.WorldType));
			Entry->SetStringField(TEXT("name"), World->GetName());
			Entry->SetStringField(TEXT("path"), World->GetPathName());
			Entry->SetBoolField(TEXT("mutable"), IsMutableWorld(World));
			Entry->SetBoolField(
				TEXT("currentSession"),
				Impl->bSessionActive && IsMutableWorld(World));
			Worlds.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("worlds"), Worlds);
	Data->SetNumberField(TEXT("count"), Worlds.Num());
	Data->SetStringField(TEXT("sessionId"), Impl->SessionId);
	Data->SetNumberField(TEXT("generation"), static_cast<double>(Impl->Generation));
	Data->SetBoolField(TEXT("sessionActive"), Impl->bSessionActive);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::FindObjects(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before runtime objects can be found."),
			409);
	}

	FString WorldId;
	FString ClassFilter;
	FString NameFilter;
	FString PathFilter;
	Params->TryGetStringField(TEXT("worldId"), WorldId);
	Params->TryGetStringField(TEXT("class"), ClassFilter);
	Params->TryGetStringField(TEXT("name"), NameFilter);
	Params->TryGetStringField(TEXT("path"), PathFilter);
	if (ClassFilter.IsEmpty() && NameFilter.IsEmpty() && PathFilter.IsEmpty())
	{
		return InvalidParams(TEXT("At least one of class, name, or path is required."));
	}

	int32 Limit = DefaultFindLimit;
	double LimitNumber = 0.0;
	if (Params->TryGetNumberField(TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, MaxFindLimit);
	}
	if (!WorldId.IsEmpty() && !Impl->FindWorld(WorldId))
	{
		return RuntimeObjectNotFound(
			FString::Printf(TEXT("World context '%s' was not found."), *WorldId));
	}

	TArray<TSharedPtr<FJsonValue>> Matches;
	for (TObjectIterator<UObject> It; It && Matches.Num() < Limit; ++It)
	{
		UObject* Object = *It;
		if (!IsValid(Object) ||
			Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) ||
			!IsQueryableWorld(GetObjectWorld(Object)))
		{
			continue;
		}
		if (!WorldId.IsEmpty() && GetWorldId(GetObjectWorld(Object)) != WorldId)
		{
			continue;
		}
		if (WorldId.IsEmpty() && !IsMutableWorld(GetObjectWorld(Object)))
		{
			continue;
		}

		const bool bClassMatches =
			ClassFilter.IsEmpty() ||
			Object->GetClass()->GetName().MatchesWildcard(ClassFilter, ESearchCase::IgnoreCase) ||
			Object->GetClass()->GetPathName().MatchesWildcard(ClassFilter, ESearchCase::IgnoreCase);
		const bool bNameMatches =
			NameFilter.IsEmpty() ||
			Object->GetName().MatchesWildcard(NameFilter, ESearchCase::IgnoreCase);
		const bool bPathMatches =
			PathFilter.IsEmpty() ||
			Object->GetPathName().MatchesWildcard(PathFilter, ESearchCase::IgnoreCase);
		if (!bClassMatches || !bNameMatches || !bPathMatches)
		{
			continue;
		}

		if (TSharedPtr<FJsonObject> Description = Impl->DescribeObject(Object))
		{
			Matches.Add(MakeShared<FJsonValueObject>(Description));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("objects"), Matches);
	Data->SetNumberField(TEXT("count"), Matches.Num());
	Data->SetBoolField(TEXT("truncated"), Matches.Num() == Limit);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::GetObject(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	TSet<FString> RequestedProperties;
	const TArray<TSharedPtr<FJsonValue>>* PropertyNames = nullptr;
	if (Params->TryGetArrayField(TEXT("properties"), PropertyNames))
	{
		for (const TSharedPtr<FJsonValue>& Value : *PropertyNames)
		{
			RequestedProperties.Add(Value->AsString());
		}
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		const FString PropertyName = Property->GetName();
		if (!RequestedProperties.IsEmpty() && !RequestedProperties.Contains(PropertyName))
		{
			continue;
		}
		if (RequestedProperties.IsEmpty() &&
			!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		TSharedPtr<FJsonValue> JsonValue =
			FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			Properties->SetField(PropertyName, JsonValue);
		}
	}

	TSharedPtr<FJsonObject> Data = Impl->DescribeObject(Object);
	if (!Data.IsValid())
	{
		return RuntimeObjectNotFound(TEXT("Failed to describe the runtime object."));
	}
	Data->SetObjectField(TEXT("properties"), Properties);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::SetObject(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Object, MutationError))
	{
		return MutationError;
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property"), PropertyName) ||
		PropertyName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'property'."));
	}
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
	if (!JsonValue.IsValid())
	{
		return InvalidParams(TEXT("Missing 'value'."));
	}

	FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*PropertyName));
	if (!Property)
	{
		return RuntimeObjectNotFound(
			FString::Printf(TEXT("Property '%s' was not found."), *PropertyName));
	}
	if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible) ||
		Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
	{
		return FRuntimeServiceResult::Error(
			TEXT("unsupported_property_type"),
			FString::Printf(TEXT("Property '%s' is not runtime writable."), *PropertyName),
			422);
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	FText FailureReason;
	if (!FJsonObjectConverter::JsonValueToUProperty(
			JsonValue,
			Property,
			ValuePtr,
			0,
			0,
			true,
			&FailureReason))
	{
		return FRuntimeServiceResult::Error(
			TEXT("unsupported_property_type"),
			FString::Printf(
				TEXT("Failed to set '%s': %s"),
				*PropertyName,
				*FailureReason.ToString()),
			422);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("property"), PropertyName);
	Data->SetBoolField(TEXT("changed"), true);
	const TSharedPtr<FJsonValue> ReadBack =
		FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr);
	if (ReadBack.IsValid())
	{
		Data->SetField(TEXT("value"), ReadBack);
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::CallObject(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Object, MutationError))
	{
		return MutationError;
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}
	UFunction* Function = Object->FindFunction(FName(*FunctionName));
	if (!Function)
	{
		return RuntimeObjectNotFound(
			FString::Printf(TEXT("Function '%s' was not found."), *FunctionName));
	}
	if (!IsCallableFunction(Function))
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			TEXT("Only non-latent, non-network BlueprintCallable/BlueprintEvent functions may be called."),
			422);
	}

	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	Params->TryGetObjectField(TEXT("args"), Arguments);
	FStructOnScope ParameterMemory(Function);
	void* ParameterBuffer = ParameterMemory.GetStructMemory();

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsInputParameter(Property))
		{
			continue;
		}
		const TSharedPtr<FJsonValue>* Argument =
			Arguments && Arguments->IsValid()
				? (*Arguments)->Values.Find(Property->GetName())
				: nullptr;
		if (!Argument || !Argument->IsValid())
		{
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ParameterBuffer);
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const TSharedPtr<FJsonObject>* ArgumentRef = nullptr;
			if ((*Argument)->Type != EJson::Object ||
				!(*Argument)->AsObject()->TryGetObjectField(
					TEXT("objectRef"),
					ArgumentRef))
			{
				return InvalidParams(
					FString::Printf(
						TEXT("Object parameter '%s' requires {objectRef:{...}}."),
						*Property->GetName()));
			}
			UObject* ArgumentObject = nullptr;
			FRuntimeServiceResult ArgumentResult = Impl->Resolve(*ArgumentRef, ArgumentObject);
			if (!ArgumentResult.bSuccess)
			{
				return ArgumentResult;
			}
			if (!ArgumentObject->IsA(ObjectProperty->PropertyClass))
			{
				return FRuntimeServiceResult::Error(
					TEXT("signature_mismatch"),
					FString::Printf(
						TEXT("Object parameter '%s' has incompatible class."),
						*Property->GetName()),
					422);
			}
			ObjectProperty->SetObjectPropertyValue(ValuePtr, ArgumentObject);
			continue;
		}

		FText FailureReason;
		if (!FJsonObjectConverter::JsonValueToUProperty(
				*Argument,
				Property,
				ValuePtr,
				0,
				0,
				true,
				&FailureReason))
		{
			return FRuntimeServiceResult::Error(
				TEXT("signature_mismatch"),
				FString::Printf(
					TEXT("Invalid parameter '%s': %s"),
					*Property->GetName(),
					*FailureReason.ToString()),
				422);
		}
	}

	Object->ProcessEvent(Function, ParameterBuffer);

	TSharedPtr<FJsonObject> Outputs = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsOutputParameter(Property))
		{
			continue;
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ParameterBuffer);
		TSharedPtr<FJsonValue> JsonValue =
			FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			Outputs->SetField(Property->GetName(), JsonValue);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetObjectField(TEXT("outputs"), Outputs);
	Data->SetBoolField(TEXT("called"), true);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::GetWidgetTree(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	UWidget* Widget = Cast<UWidget>(Object);
	UWidget* RootWidget = Widget;
	if (UUserWidget* UserWidget = Cast<UUserWidget>(Object))
	{
		RootWidget = UserWidget->WidgetTree ? UserWidget->WidgetTree->RootWidget : nullptr;
	}
	if (!RootWidget)
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("The object has no runtime widget tree."),
			422);
	}

	int32 MaxDepth = DefaultWidgetDepth;
	double MaxDepthNumber = 0.0;
	if (Params->TryGetNumberField(TEXT("maxDepth"), MaxDepthNumber))
	{
		MaxDepth = FMath::Clamp(
			static_cast<int32>(MaxDepthNumber),
			0,
			MaxWidgetDepth);
	}

	TFunction<TSharedPtr<FJsonObject>(UWidget*, int32)> BuildNode =
		[this, MaxDepth, &BuildNode](UWidget* Current, int32 Depth)
		{
			if (!Current)
			{
				return TSharedPtr<FJsonObject>();
			}

			TSharedPtr<FJsonObject> ObjectRef;
			if (!Impl->MakeRef(Current, ObjectRef).bSuccess)
			{
				return TSharedPtr<FJsonObject>();
			}

			TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetObjectField(TEXT("objectRef"), ObjectRef);
			Node->SetStringField(TEXT("name"), Current->GetName());
			Node->SetStringField(TEXT("class"), Current->GetClass()->GetPathName());
			Node->SetStringField(TEXT("visibility"), VisibilityToString(Current->GetVisibility()));
			Node->SetBoolField(TEXT("enabled"), Current->GetIsEnabled());
			Node->SetNumberField(TEXT("depth"), Depth);
			if (Current->Slot)
			{
				Node->SetStringField(TEXT("slotClass"), Current->Slot->GetClass()->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> Children;
			if (Depth < MaxDepth)
			{
				if (UPanelWidget* Panel = Cast<UPanelWidget>(Current))
				{
					for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
					{
						if (TSharedPtr<FJsonObject> Child =
								BuildNode(Panel->GetChildAt(Index), Depth + 1))
						{
							Children.Add(MakeShared<FJsonValueObject>(Child));
						}
					}
				}
			}
			Node->SetArrayField(TEXT("children"), Children);
			Node->SetBoolField(
				TEXT("truncated"),
				Depth >= MaxDepth &&
					Cast<UPanelWidget>(Current) &&
					Cast<UPanelWidget>(Current)->GetChildrenCount() > 0);
			return Node;
		};

	TSharedPtr<FJsonObject> Root = BuildNode(RootWidget, 0);
	if (!Root.IsValid())
	{
		return RuntimeObjectNotFound(TEXT("Failed to build the runtime widget tree."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("root"), Root);
	Data->SetNumberField(TEXT("maxDepth"), MaxDepth);
	if (Object != RootWidget)
	{
		TSharedPtr<FJsonObject> OwnerRef;
		if (Impl->MakeRef(Object, OwnerRef).bSuccess)
		{
			Data->SetObjectField(TEXT("ownerRef"), OwnerRef);
		}
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::GetWidgetState(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	UWidget* Widget = Cast<UWidget>(Object);
	if (!Widget)
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("objectRef does not resolve to a UWidget."),
			422);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Widget->GetName());
	Data->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
	Data->SetStringField(TEXT("visibility"), VisibilityToString(Widget->GetVisibility()));
	Data->SetBoolField(TEXT("enabled"), Widget->GetIsEnabled());
	Data->SetBoolField(TEXT("hovered"), Widget->IsHovered());
	Data->SetBoolField(TEXT("keyboardFocused"), Widget->HasKeyboardFocus());
	Data->SetObjectField(TEXT("desiredSize"), VectorToJson(Widget->GetDesiredSize()));

	const TSharedPtr<SWidget> CachedWidget = Widget->GetCachedWidget();
	Data->SetBoolField(TEXT("constructed"), CachedWidget.IsValid());
	if (CachedWidget.IsValid())
	{
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		Data->SetObjectField(
			TEXT("absolutePosition"),
			VectorToJson(FVector2D(Geometry.GetAbsolutePosition())));
		Data->SetObjectField(
			TEXT("absoluteSize"),
			VectorToJson(FVector2D(Geometry.GetAbsoluteSize())));
		Data->SetObjectField(
			TEXT("localSize"),
			VectorToJson(FVector2D(Geometry.GetLocalSize())));
		Data->SetStringField(TEXT("slateType"), CachedWidget->GetTypeAsString());
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::HitTestWidget(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FRuntimeServiceResult::Error(
			TEXT("input_dispatch_failed"),
			TEXT("Slate application is not initialized."),
			422);
	}

	UWidget* Widget = nullptr;
	TSharedPtr<SWidget> TargetSlateWidget;
	const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
	if (Params->TryGetObjectField(TEXT("objectRef"), ObjectRef)
		&& ObjectRef
		&& ObjectRef->IsValid())
	{
		UObject* Object = nullptr;
		FRuntimeServiceResult ResolveResult = Impl->Resolve(*ObjectRef, Object);
		if (!ResolveResult.bSuccess)
		{
			return ResolveResult;
		}
		Widget = Cast<UWidget>(Object);
		if (!Widget || !Widget->GetCachedWidget().IsValid())
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Widget has no cached Slate widget."),
				422);
		}
		TargetSlateWidget = Widget->GetCachedWidget();
	}

	TOptional<FVector2D> Position;
	if (!ReadVector2D(Params, TEXT("position"), Position))
	{
		if (!Widget)
		{
			return InvalidParams(
				TEXT("Hit testing requires either objectRef or an explicit position."));
		}
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		Position = FVector2D(
			Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f));
	}

	TSharedPtr<FJsonObject> Data =
		BuildHitTestEvidence(Position.GetValue(), TargetSlateWidget);
	Data->SetObjectField(TEXT("position"), VectorToJson(Position.GetValue()));
	const TArray<TSharedPtr<FJsonValue>>* HitPath = nullptr;
	if (Data->TryGetArrayField(TEXT("hitPath"), HitPath) && HitPath)
	{
		Data->SetArrayField(TEXT("path"), *HitPath);
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::FocusWidget(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Object, MutationError))
	{
		return MutationError;
	}

	double UserIndexNumber = 0.0;
	Params->TryGetNumberField(TEXT("userIndex"), UserIndexNumber);
	return Impl->Input.Focus(
		Cast<UWidget>(Object),
		static_cast<uint32>(FMath::Max(0.0, UserIndexNumber)));
}

FRuntimeServiceResult FRuntimeSceneService::ListDelegates(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	TArray<TSharedPtr<FJsonValue>> Delegates;
	for (TFieldIterator<FMulticastDelegateProperty> It(Object->GetClass()); It; ++It)
	{
		FMulticastDelegateProperty* Property = *It;
		void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
		const FMulticastScriptDelegate* Delegate =
			Property->GetMulticastDelegate(PropertyValue);
		if (!Delegate)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Property->GetName());
		Entry->SetStringField(
			TEXT("signature"),
			Property->SignatureFunction
				? Property->SignatureFunction->GetPathName()
				: FString());
		Entry->SetBoolField(TEXT("bound"), Delegate->IsBound());

		TArray<TSharedPtr<FJsonValue>> BoundObjects;
		for (UObject* BoundObject : Delegate->GetAllObjects())
		{
			TSharedPtr<FJsonObject> BoundRef;
			if (Impl->MakeRef(BoundObject, BoundRef).bSuccess)
			{
				BoundObjects.Add(MakeShared<FJsonValueObject>(BoundRef));
			}
		}
		Entry->SetNumberField(TEXT("boundObjectCount"), BoundObjects.Num());
		Entry->SetArrayField(TEXT("boundObjects"), BoundObjects);
		Delegates.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("delegates"), Delegates);
	Data->SetNumberField(TEXT("count"), Delegates.Num());
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::BindDelegate(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Owner, MutationError))
	{
		return MutationError;
	}

	UObject* FunctionTarget = nullptr;
	FRuntimeServiceResult TargetResult =
		Impl->ResolveParamObject(Params, TEXT("functionTargetRef"), FunctionTarget);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}
	if (GetObjectWorld(Owner) != GetObjectWorld(FunctionTarget))
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			TEXT("Delegate owner and function target must belong to the same world."),
			422);
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}
	UFunction* Function = FunctionTarget->FindFunction(FName(*FunctionName));
	if (!Function || !Property->SignatureFunction ||
		!Property->SignatureFunction->IsSignatureCompatibleWith(Function))
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			FString::Printf(
				TEXT("Function '%s' is missing or incompatible with delegate '%s'."),
				*FunctionName,
				*Property->GetName()),
			422);
	}

	const bool bAlreadyBound =
		Delegate->Contains(FunctionTarget, FName(*FunctionName));
	if (!bAlreadyBound)
	{
		FScriptDelegate ScriptDelegate;
		ScriptDelegate.BindUFunction(FunctionTarget, FName(*FunctionName));
		Property->AddDelegate(ScriptDelegate, Owner, PropertyValue);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetBoolField(TEXT("changed"), !bAlreadyBound);
	Data->SetBoolField(TEXT("bound"), true);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::UnbindDelegate(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Owner, MutationError))
	{
		return MutationError;
	}

	UObject* FunctionTarget = nullptr;
	FRuntimeServiceResult TargetResult =
		Impl->ResolveParamObject(Params, TEXT("functionTargetRef"), FunctionTarget);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}

	FScriptDelegate ScriptDelegate;
	ScriptDelegate.BindUFunction(FunctionTarget, FName(*FunctionName));
	const bool bWasBound = Delegate->Contains(FunctionTarget, FName(*FunctionName));
	if (bWasBound)
	{
		Property->RemoveDelegate(ScriptDelegate, Owner, PropertyValue);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetBoolField(TEXT("changed"), bWasBound);
	Data->SetBoolField(TEXT("bound"), false);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::IsDelegateBound(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}

	UObject* FunctionTarget = nullptr;
	FRuntimeServiceResult TargetResult =
		Impl->ResolveParamObject(Params, TEXT("functionTargetRef"), FunctionTarget);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetBoolField(
		TEXT("bound"),
		Delegate->Contains(FunctionTarget, FName(*FunctionName)));
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::BroadcastDelegate(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Owner, MutationError))
	{
		return MutationError;
	}
	if (!Property->SignatureFunction)
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			TEXT("Delegate has no signature function."),
			422);
	}

	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	Params->TryGetObjectField(TEXT("args"), Arguments);
	FStructOnScope ParameterMemory(Property->SignatureFunction);
	void* ParameterBuffer = ParameterMemory.GetStructMemory();
	for (TFieldIterator<FProperty> It(Property->SignatureFunction); It; ++It)
	{
		FProperty* Parameter = *It;
		if (!IsInputParameter(Parameter))
		{
			continue;
		}
		const TSharedPtr<FJsonValue>* JsonValue =
			Arguments && Arguments->IsValid()
				? (*Arguments)->Values.Find(Parameter->GetName())
				: nullptr;
		if (!JsonValue || !JsonValue->IsValid())
		{
			continue;
		}
		FText FailureReason;
		if (!FJsonObjectConverter::JsonValueToUProperty(
				*JsonValue,
				Parameter,
				Parameter->ContainerPtrToValuePtr<void>(ParameterBuffer),
				0,
				0,
				true,
				&FailureReason))
		{
			return FRuntimeServiceResult::Error(
				TEXT("signature_mismatch"),
				FString::Printf(
					TEXT("Invalid delegate argument '%s': %s"),
					*Parameter->GetName(),
					*FailureReason.ToString()),
				422);
		}
	}

	Delegate->ProcessMulticastDelegate<UObject>(ParameterBuffer);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetBoolField(TEXT("broadcast"), true);
	Data->SetNumberField(TEXT("boundObjectCount"), Delegate->GetAllObjects().Num());
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::PointerInput(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before input can be dispatched."),
			409);
	}

	FString Action;
	if (!Params->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'action'."));
	}
	FString Button = TEXT("left");
	Params->TryGetStringField(TEXT("button"), Button);

	UWidget* TargetWidget = nullptr;
	const TSharedPtr<FJsonObject>* TargetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("target"), TargetRef) &&
		TargetRef &&
		TargetRef->IsValid())
	{
		UObject* TargetObject = nullptr;
		FRuntimeServiceResult TargetResult = Impl->Resolve(*TargetRef, TargetObject);
		if (!TargetResult.bSuccess)
		{
			return TargetResult;
		}
		if (!Impl->IsMutationAllowed(TargetObject, TargetResult))
		{
			return TargetResult;
		}
		TargetWidget = Cast<UWidget>(TargetObject);
		if (!TargetWidget)
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Pointer target must resolve to a UWidget."),
				422);
		}
	}

	TOptional<FVector2D> Position;
	TOptional<FVector2D> EndPosition;
	ReadVector2D(Params, TEXT("position"), Position);
	ReadVector2D(Params, TEXT("endPosition"), EndPosition);
	return Impl->Input.Pointer(
		Action,
		TargetWidget,
		Position,
		EndPosition,
		Button);
}

FRuntimeServiceResult FRuntimeSceneService::StartPointerSequence(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedSessionId;
	uint64 RequestedGeneration = 0;
	if (!ReadRequiredSession(
			Params,
			RequestedSessionId,
			RequestedGeneration))
	{
		return InvalidParams(
			TEXT("sessionId and integer generation are required."));
	}
	if (RequestedSessionId != Impl->SessionId
		|| RequestedGeneration != Impl->Generation)
	{
		return FRuntimeServiceResult::Error(
			TEXT("stale_session_handle"),
			TEXT("The pointer sequence targets a stale PIE session."),
			410);
	}
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before a pointer sequence can start."),
			409);
	}
	if (!Impl->ActivePointerSequenceId.IsEmpty())
	{
		return FRuntimeServiceResult::Error(
			TEXT("pointer_sequence_busy"),
			TEXT("Another pointer sequence is still running."),
			409);
	}

	FRuntimePointerSequence Sequence;
	Sequence.Id =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Sequence.SessionId = RequestedSessionId;
	Sequence.Generation = RequestedGeneration;
	Sequence.AcceptedSeconds = FPlatformTime::Seconds();
	Params->TryGetStringField(
		TEXT("coordinateSpace"),
		Sequence.CoordinateSpace);
	if (Sequence.CoordinateSpace.IsEmpty())
	{
		Sequence.CoordinateSpace = TEXT("screenAbsolute");
	}
	static const TSet<FString> CoordinateSpaces = {
		TEXT("screenAbsolute"),
		TEXT("window"),
		TEXT("widgetLocal"),
		TEXT("widgetNormalized"),
		TEXT("viewportNormalized")
	};
	if (!CoordinateSpaces.Contains(Sequence.CoordinateSpace))
	{
		return InvalidParams(
			FString::Printf(
				TEXT("Unsupported coordinateSpace '%s'."),
				*Sequence.CoordinateSpace));
	}
	Params->TryGetBoolField(
		TEXT("requireTargetHit"),
		Sequence.bRequireTargetHit);

	const TSharedPtr<FJsonObject>* TargetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("target"), TargetRef)
		&& TargetRef
		&& TargetRef->IsValid())
	{
		UObject* TargetObject = nullptr;
		FRuntimeServiceResult TargetResult =
			Impl->Resolve(*TargetRef, TargetObject);
		if (!TargetResult.bSuccess)
		{
			return TargetResult;
		}
		if (!Impl->IsMutationAllowed(TargetObject, TargetResult))
		{
			return TargetResult;
		}
		UWidget* TargetWidget = Cast<UWidget>(TargetObject);
		if (!TargetWidget || !TargetWidget->GetCachedWidget().IsValid())
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Pointer sequence target must be a constructed runtime UWidget."),
				422);
		}
		Sequence.TargetWidget = TargetWidget;
	}
	if (Sequence.bRequireTargetHit && !Sequence.TargetWidget.IsValid())
	{
		return InvalidParams(
			TEXT("requireTargetHit requires a target widget."));
	}
	if ((Sequence.CoordinateSpace == TEXT("widgetLocal")
			|| Sequence.CoordinateSpace == TEXT("widgetNormalized"))
		&& !Sequence.TargetWidget.IsValid())
	{
		return InvalidParams(
			TEXT("widgetLocal and widgetNormalized require a target widget."));
	}

	const TArray<TSharedPtr<FJsonValue>>* Actions = nullptr;
	if (!Params->TryGetArrayField(TEXT("actions"), Actions)
		|| !Actions
		|| Actions->IsEmpty()
		|| Actions->Num() > MaxPointerSequenceActions)
	{
		return InvalidParams(
			FString::Printf(
				TEXT("actions must contain between 1 and %d entries."),
				MaxPointerSequenceActions));
	}

	int32 TotalPointCount = 0;
	static const TSet<FString> SupportedActions = {
		TEXT("down"),
		TEXT("move"),
		TEXT("up"),
		TEXT("doubleClick"),
		TEXT("wheel"),
		TEXT("cancel")
	};
	for (int32 ActionIndex = 0; ActionIndex < Actions->Num(); ++ActionIndex)
	{
		const TSharedPtr<FJsonObject> ActionObject =
			(*Actions)[ActionIndex]->AsObject();
		if (!ActionObject.IsValid())
		{
			return InvalidParams(
				FString::Printf(
					TEXT("actions[%d] must be an object."),
					ActionIndex));
		}

		FRuntimePointerStep Step;
		if (!ActionObject->TryGetStringField(TEXT("action"), Step.Action)
			|| !SupportedActions.Contains(Step.Action))
		{
			return InvalidParams(
				FString::Printf(
					TEXT("actions[%d].action is unsupported."),
					ActionIndex));
		}
		ActionObject->TryGetStringField(TEXT("button"), Step.Button);
		if (Step.Button.IsEmpty())
		{
			Step.Button = TEXT("left");
		}
		if (Step.Button != TEXT("left")
			&& Step.Button != TEXT("right")
			&& Step.Button != TEXT("middle"))
		{
			return InvalidParams(
				FString::Printf(
					TEXT("actions[%d].button is unsupported."),
					ActionIndex));
		}
		double WheelDelta = 0.0;
		ActionObject->TryGetNumberField(TEXT("wheelDelta"), WheelDelta);
		Step.WheelDelta = static_cast<float>(WheelDelta);
		if (Step.Action == TEXT("wheel")
			&& FMath::IsNearlyZero(Step.WheelDelta))
		{
			return InvalidParams(
				FString::Printf(
					TEXT("actions[%d].wheelDelta must be non-zero."),
					ActionIndex));
		}
		double DurationMs = 0.0;
		ActionObject->TryGetNumberField(TEXT("durationMs"), DurationMs);
		if (DurationMs < 0.0 || DurationMs > 60000.0)
		{
			return InvalidParams(
				FString::Printf(
					TEXT("actions[%d].durationMs must be between 0 and 60000."),
					ActionIndex));
		}
		Step.DurationMs = static_cast<int32>(DurationMs);

		const TArray<TSharedPtr<FJsonValue>>* Path = nullptr;
		if (ActionObject->TryGetArrayField(TEXT("path"), Path)
			&& Path)
		{
			if (Path->IsEmpty() || Path->Num() > MaxPointerSequencePoints)
			{
				return InvalidParams(
					FString::Printf(
						TEXT("actions[%d].path has an invalid point count."),
						ActionIndex));
			}
			for (int32 PointIndex = 0; PointIndex < Path->Num(); ++PointIndex)
			{
				const TSharedPtr<FJsonObject> Point = (*Path)[PointIndex]->AsObject();
				if (!Point.IsValid())
				{
					return InvalidParams(
						FString::Printf(
							TEXT("actions[%d].path[%d] must be an object."),
							ActionIndex,
							PointIndex));
				}
				double X = 0.0;
				double Y = 0.0;
				if (!Point->TryGetNumberField(TEXT("x"), X)
					|| !Point->TryGetNumberField(TEXT("y"), Y))
				{
					return InvalidParams(
						FString::Printf(
							TEXT("actions[%d].path[%d] requires x and y."),
							ActionIndex,
							PointIndex));
				}
				Step.Points.Add(FVector2D(X, Y));
			}
		}
		else
		{
			TOptional<FVector2D> Position;
			if (ReadVector2D(ActionObject, TEXT("position"), Position))
			{
				Step.Points.Add(Position.GetValue());
			}
		}

		if (Step.Action != TEXT("cancel") && Step.Points.IsEmpty())
		{
			if (!Sequence.TargetWidget.IsValid())
			{
				return InvalidParams(
					FString::Printf(
						TEXT("actions[%d] requires position/path or a target widget."),
						ActionIndex));
			}
			Step.Points.Add(FVector2D::ZeroVector);
			Step.bUseTargetCenter = true;
		}
		TotalPointCount += Step.Points.Num();
		if (TotalPointCount > MaxPointerSequencePoints)
		{
			return InvalidParams(
				FString::Printf(
					TEXT("A pointer sequence may contain at most %d total path points."),
					MaxPointerSequencePoints));
		}
		Sequence.Steps.Add(MoveTemp(Step));
	}

	while (Impl->PointerSequences.Num() >= MaxRetainedAsyncJobs)
	{
		FString OldestId;
		double OldestSeconds = TNumericLimits<double>::Max();
		for (const TPair<FString, FRuntimePointerSequence>& Pair :
			Impl->PointerSequences)
		{
			if (Pair.Value.Status != TEXT("running")
				&& Pair.Value.AcceptedSeconds < OldestSeconds)
			{
				OldestId = Pair.Key;
				OldestSeconds = Pair.Value.AcceptedSeconds;
			}
		}
		if (OldestId.IsEmpty())
		{
			return FRuntimeServiceResult::Error(
				TEXT("pointer_sequence_busy"),
				TEXT("The pointer sequence result store is full."),
				409);
		}
		Impl->PointerSequences.Remove(OldestId);
	}

	const FString SequenceId = Sequence.Id;
	Impl->PointerSequences.Add(SequenceId, MoveTemp(Sequence));
	Impl->ActivePointerSequenceId = SequenceId;
	return FRuntimeServiceResult::Ok(
		BuildPointerSequenceData(
			Impl->PointerSequences.FindChecked(SequenceId)));
}

FRuntimeServiceResult FRuntimeSceneService::GetPointerSequence(
	const TSharedPtr<FJsonObject>& Params)
{
	FString SequenceId;
	if (!Params->TryGetStringField(TEXT("sequenceId"), SequenceId)
		|| SequenceId.IsEmpty())
	{
		return InvalidParams(TEXT("sequenceId is required."));
	}
	const FRuntimePointerSequence* Sequence =
		Impl->PointerSequences.Find(SequenceId);
	if (!Sequence)
	{
		return RuntimeObjectNotFound(
			FString::Printf(
				TEXT("Pointer sequence '%s' was not found."),
				*SequenceId));
	}
	return FRuntimeServiceResult::Ok(BuildPointerSequenceData(*Sequence));
}

FRuntimeServiceResult FRuntimeSceneService::KeyInput(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before input can be dispatched."),
			409);
	}

	FString Action;
	FString KeyName;
	FString Text;
	Params->TryGetStringField(TEXT("action"), Action);
	Params->TryGetStringField(TEXT("key"), KeyName);
	Params->TryGetStringField(TEXT("text"), Text);
	if (Action.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'action'."));
	}

	TArray<FString> KeyNames;
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Params->TryGetArrayField(TEXT("keys"), Keys))
	{
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			KeyNames.Add(KeyValue->AsString());
		}
	}

	UWidget* TargetWidget = nullptr;
	const TSharedPtr<FJsonObject>* TargetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("target"), TargetRef) &&
		TargetRef &&
		TargetRef->IsValid())
	{
		UObject* TargetObject = nullptr;
		FRuntimeServiceResult TargetResult = Impl->Resolve(*TargetRef, TargetObject);
		if (!TargetResult.bSuccess)
		{
			return TargetResult;
		}
		if (!Impl->IsMutationAllowed(TargetObject, TargetResult))
		{
			return TargetResult;
		}
		TargetWidget = Cast<UWidget>(TargetObject);
		if (!TargetWidget)
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Key target must resolve to a UWidget."),
				422);
		}
	}

	return Impl->Input.Key(Action, KeyName, Text, KeyNames, TargetWidget);
}

FRuntimeServiceResult FRuntimeSceneService::SetInputMode(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before input mode can be changed."),
			409);
	}

	FString Mode;
	if (!Params->TryGetStringField(TEXT("mode"), Mode) || Mode.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'mode'."));
	}
	FString LockMouse = TEXT("doNotLock");
	Params->TryGetStringField(TEXT("lockMouse"), LockMouse);

	UWidget* FocusWidget = nullptr;
	UWorld* RuntimeWorld = nullptr;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("widget"), WidgetRef) &&
		WidgetRef &&
		WidgetRef->IsValid())
	{
		UObject* WidgetObject = nullptr;
		FRuntimeServiceResult WidgetResult = Impl->Resolve(*WidgetRef, WidgetObject);
		if (!WidgetResult.bSuccess)
		{
			return WidgetResult;
		}
		FocusWidget = Cast<UWidget>(WidgetObject);
		if (!FocusWidget)
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Input mode widget must resolve to a UWidget."),
				422);
		}
		RuntimeWorld = GetObjectWorld(FocusWidget);
	}
	if (!RuntimeWorld && GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (IsMutableWorld(Context.World()))
			{
				RuntimeWorld = Context.World();
				break;
			}
		}
	}
	if (!RuntimeWorld)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("No PIE/Game world is available."),
			409);
	}

	double PlayerIndexNumber = 0.0;
	Params->TryGetNumberField(TEXT("playerIndex"), PlayerIndexNumber);
	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(
		RuntimeWorld,
		FMath::Max(0, static_cast<int32>(PlayerIndexNumber)));
	TOptional<bool> ShowCursor;
	bool bShowCursor = false;
	if (Params->TryGetBoolField(TEXT("showCursor"), bShowCursor))
	{
		ShowCursor = bShowCursor;
	}
	return Impl->Input.SetPlayerInputMode(
		PlayerController,
		Mode,
		FocusWidget,
		LockMouse,
		ShowCursor);
}

FRuntimeServiceResult FRuntimeSceneService::CapturePIEViewport(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedSessionId;
	uint64 RequestedGeneration = 0;
	if (!ReadRequiredSession(
			Params,
			RequestedSessionId,
			RequestedGeneration))
	{
		return InvalidParams(
			TEXT("sessionId and integer generation are required."));
	}
	if (RequestedSessionId != Impl->SessionId
		|| RequestedGeneration != Impl->Generation)
	{
		return FRuntimeServiceResult::Error(
			TEXT("stale_session_handle"),
			TEXT("The viewport request targets a stale PIE session."),
			410);
	}
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before its viewport can be captured."),
			409);
	}

	TSharedPtr<SViewport> ViewportWidget;
	TSharedPtr<SWindow> Window;
	FSceneViewport* SceneViewport = nullptr;
	FString ResolveError;
	if (!ResolvePIEViewport(
			ViewportWidget,
			Window,
			SceneViewport,
			ResolveError))
	{
		return FRuntimeServiceResult::Error(
			TEXT("viewport_capture_failed"),
			ResolveError,
			422);
	}

	TArray<FColor> Pixels;
	FIntVector ScreenshotSize = FIntVector::ZeroValue;
	FString CaptureSource;
	bool bIncludesSlate = false;
	bool bForceSceneViewportReadbackForTesting = false;
#if WITH_DEV_AUTOMATION_TESTS
	Params->TryGetBoolField(
		TEXT("forceSceneViewportReadbackForTesting"),
		bForceSceneViewportReadbackForTesting);
#endif
	const bool bSlateCaptureSucceeded =
		!bForceSceneViewportReadbackForTesting
		&& FSlateApplication::Get().TakeScreenshot(
			ViewportWidget.ToSharedRef(),
			Pixels,
			ScreenshotSize)
		&& ScreenshotSize.X > 0
		&& ScreenshotSize.Y > 0
		&& Pixels.Num() == ScreenshotSize.X * ScreenshotSize.Y;
	if (bSlateCaptureSucceeded)
	{
		CaptureSource = TEXT("pieSlateViewport");
		bIncludesSlate = true;
	}
	else
	{
		Pixels.Reset();
		ScreenshotSize = FIntVector::ZeroValue;
		const FIntPoint SceneViewportSize = SceneViewport->GetSizeXY();
		if (SceneViewportSize.X <= 0
			|| SceneViewportSize.Y <= 0
			|| !SceneViewport->GetRenderTargetTexture().IsValid()
			|| !SceneViewport->ReadPixels(Pixels)
			|| Pixels.Num() != SceneViewportSize.X * SceneViewportSize.Y)
		{
			return FRuntimeServiceResult::Error(
				TEXT("viewport_capture_failed"),
				TEXT(
					"Capturing the bound PIE SViewport failed and its "
					"exact FSceneViewport render target could not be read; "
					"no desktop or other-window fallback was used."),
				422);
		}
		ScreenshotSize =
			FIntVector(SceneViewportSize.X, SceneViewportSize.Y, 0);
		CaptureSource = TEXT("pieSceneViewportReadPixels");
	}

	FString PixelSha256;
	if (!TrySha256Hex(
			Pixels.GetData(),
			static_cast<uint64>(Pixels.Num()) * sizeof(FColor),
			PixelSha256))
	{
		return FRuntimeServiceResult::Error(
			TEXT("viewport_capture_failed"),
			TEXT("Failed to hash the captured PIE pixels."),
			500);
	}

	double WidthNumber = ScreenshotSize.X;
	double HeightNumber = ScreenshotSize.Y;
	double QualityNumber = 90.0;
	Params->TryGetNumberField(TEXT("width"), WidthNumber);
	Params->TryGetNumberField(TEXT("height"), HeightNumber);
	Params->TryGetNumberField(TEXT("quality"), QualityNumber);
	const int32 OutputWidth =
		FMath::Clamp(static_cast<int32>(WidthNumber), 1, 4096);
	const int32 OutputHeight =
		FMath::Clamp(static_cast<int32>(HeightNumber), 1, 4096);
	const int32 Quality =
		FMath::Clamp(static_cast<int32>(QualityNumber), 1, 100);

	TArray<FColor> ResizedPixels;
	const TArray<FColor>* EncodePixels = &Pixels;
	if (OutputWidth != ScreenshotSize.X || OutputHeight != ScreenshotSize.Y)
	{
		ResizeRuntimePixels(
			Pixels,
			ScreenshotSize.X,
			ScreenshotSize.Y,
			ResizedPixels,
			OutputWidth,
			OutputHeight);
		EncodePixels = &ResizedPixels;
	}

	IImageWrapperModule& ImageWrapperModule =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(
			TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Wrapper =
		ImageWrapperModule.CreateImageWrapper(EImageFormat::JPEG);
	if (!Wrapper.IsValid()
		|| !Wrapper->SetRaw(
			EncodePixels->GetData(),
			EncodePixels->Num() * sizeof(FColor),
			OutputWidth,
			OutputHeight,
			ERGBFormat::BGRA,
			8))
	{
		return FRuntimeServiceResult::Error(
			TEXT("viewport_capture_failed"),
			TEXT("Failed to initialize JPEG encoding for the PIE capture."),
			500);
	}
	const TArray64<uint8> Compressed = Wrapper->GetCompressed(Quality);
	if (Compressed.IsEmpty())
	{
		return FRuntimeServiceResult::Error(
			TEXT("viewport_capture_failed"),
			TEXT("JPEG encoding for the PIE capture returned no bytes."),
			500);
	}

	const FGeometry& Geometry = ViewportWidget->GetCachedGeometry();
	const FVector2D ViewportPosition =
		FVector2D(Geometry.GetAbsolutePosition());
	const FVector2D ViewportSize =
		FVector2D(Geometry.GetAbsoluteSize());
	TSharedPtr<FJsonObject> ViewportRect = MakeShared<FJsonObject>();
	ViewportRect->SetNumberField(TEXT("x"), ViewportPosition.X);
	ViewportRect->SetNumberField(TEXT("y"), ViewportPosition.Y);
	ViewportRect->SetNumberField(TEXT("width"), ViewportSize.X);
	ViewportRect->SetNumberField(TEXT("height"), ViewportSize.Y);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(
		TEXT("image_base64"),
		FBase64::Encode(Compressed.GetData(), Compressed.Num()));
	Data->SetStringField(TEXT("mime_type"), TEXT("image/jpeg"));
	Data->SetStringField(TEXT("mimeType"), TEXT("image/jpeg"));
	Data->SetNumberField(TEXT("width"), OutputWidth);
	Data->SetNumberField(TEXT("height"), OutputHeight);
	Data->SetNumberField(TEXT("quality"), Quality);
	Data->SetStringField(TEXT("format"), TEXT("jpeg"));
	Data->SetStringField(TEXT("sessionId"), Impl->SessionId);
	Data->SetNumberField(
		TEXT("generation"),
		static_cast<double>(Impl->Generation));
	Data->SetStringField(TEXT("captureSource"), CaptureSource);
	Data->SetBoolField(TEXT("includesSlate"), bIncludesSlate);
	Data->SetStringField(TEXT("slateWidget"), ViewportWidget->GetTypeAsString());
	Data->SetStringField(TEXT("windowTitle"), Window->GetTitle().ToString());
	Data->SetStringField(
		TEXT("windowHandle"),
		FString::Printf(
			TEXT("%p"),
			Window->GetNativeWindow()->GetOSWindowHandle()));
	Data->SetObjectField(TEXT("viewportRect"), ViewportRect);
	Data->SetNumberField(TEXT("sourceWidth"), ScreenshotSize.X);
	Data->SetNumberField(TEXT("sourceHeight"), ScreenshotSize.Y);
	Data->SetStringField(
		TEXT("pixelSha256"),
		TEXT("sha256:") + PixelSha256);
	TSharedPtr<FJsonObject> RawCapture = MakeShared<FJsonObject>();
	RawCapture->SetStringField(TEXT("pixelFormat"), TEXT("BGRA8"));
	RawCapture->SetNumberField(TEXT("width"), ScreenshotSize.X);
	RawCapture->SetNumberField(TEXT("height"), ScreenshotSize.Y);
	RawCapture->SetNumberField(TEXT("pixelCount"), Pixels.Num());
	RawCapture->SetNumberField(
		TEXT("byteCount"),
		static_cast<double>(Pixels.Num()) * sizeof(FColor));
	RawCapture->SetStringField(
		TEXT("pixelSha256"),
		TEXT("sha256:") + PixelSha256);
	Data->SetObjectField(TEXT("rawCapture"), RawCapture);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::StartWait(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RequestedSessionId;
	uint64 RequestedGeneration = 0;
	if (!ReadRequiredSession(
			Params,
			RequestedSessionId,
			RequestedGeneration))
	{
		return InvalidParams(
			TEXT("sessionId and integer generation are required."));
	}
	if (RequestedSessionId != Impl->SessionId
		|| RequestedGeneration != Impl->Generation)
	{
		return FRuntimeServiceResult::Error(
			TEXT("stale_session_handle"),
			TEXT("The wait targets a stale PIE session."),
			410);
	}

	const TSharedPtr<FJsonObject>* PredicateObject = nullptr;
	if (!Params->TryGetObjectField(TEXT("predicate"), PredicateObject)
		|| !PredicateObject
		|| !PredicateObject->IsValid())
	{
		return InvalidParams(TEXT("predicate is required."));
	}

	FRuntimeWaitJob Job;
	Job.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Job.SessionId = RequestedSessionId;
	Job.Generation = RequestedGeneration;
	Job.Predicate = MakeShared<FJsonObject>(**PredicateObject);
	if (!Job.Predicate->TryGetStringField(
			TEXT("type"),
			Job.PredicateType))
	{
		return InvalidParams(TEXT("predicate.type is required."));
	}
	static const TSet<FString> PredicateTypes = {
		TEXT("pieReady"),
		TEXT("widgetExists"),
		TEXT("propertyEquals"),
		TEXT("generationChanged"),
		TEXT("logContains"),
		TEXT("logNotContains")
	};
	if (!PredicateTypes.Contains(Job.PredicateType))
	{
		return InvalidParams(
			FString::Printf(
				TEXT("Unsupported wait predicate '%s'."),
				*Job.PredicateType));
	}
	if (!Impl->bSessionActive
		&& Job.PredicateType != TEXT("pieReady")
		&& Job.PredicateType != TEXT("generationChanged"))
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("This wait predicate requires an active PIE session."),
			409);
	}

	if (Job.PredicateType == TEXT("widgetExists"))
	{
		const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
		FString Name;
		FString ClassName;
		FString Path;
		const bool bHasRef =
			Job.Predicate->TryGetObjectField(TEXT("objectRef"), ObjectRef)
			&& ObjectRef
			&& ObjectRef->IsValid();
		Job.Predicate->TryGetStringField(TEXT("name"), Name);
		Job.Predicate->TryGetStringField(TEXT("class"), ClassName);
		Job.Predicate->TryGetStringField(TEXT("path"), Path);
		if (!bHasRef
			&& Name.IsEmpty()
			&& ClassName.IsEmpty()
			&& Path.IsEmpty())
		{
			return InvalidParams(
				TEXT("widgetExists requires objectRef or a name/class/path selector."));
		}
	}
	else if (Job.PredicateType == TEXT("propertyEquals"))
	{
		const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
		FString PropertyName;
		if (!Job.Predicate->TryGetObjectField(TEXT("objectRef"), ObjectRef)
			|| !ObjectRef
			|| !ObjectRef->IsValid()
			|| !Job.Predicate->TryGetStringField(
				TEXT("property"),
				PropertyName)
			|| PropertyName.IsEmpty()
			|| !Job.Predicate->HasField(TEXT("expected")))
		{
			return InvalidParams(
				TEXT("propertyEquals requires objectRef, property, and expected."));
		}
	}
	else if (Job.PredicateType == TEXT("logContains")
		|| Job.PredicateType == TEXT("logNotContains"))
	{
		FString Text;
		if (!Job.Predicate->TryGetStringField(TEXT("text"), Text)
			|| Text.IsEmpty())
		{
			return InvalidParams(
				TEXT("Log predicates require non-empty text."));
		}
		Job.LogPath = FindActiveEditorLogPath();
		const int64 LogSize = Job.LogPath.IsEmpty()
			? -1
			: IFileManager::Get().FileSize(*Job.LogPath);
		if (LogSize < 0)
		{
			return FRuntimeServiceResult::Error(
				TEXT("log_unavailable"),
				TEXT("No readable Editor log is available for the requested predicate."),
				503);
		}
		Job.LogCursor = LogSize;
		Job.Evidence->SetStringField(TEXT("logPath"), Job.LogPath);
		Job.Evidence->SetNumberField(
			TEXT("startCursor"),
			static_cast<double>(Job.LogCursor));
	}

	double TimeoutMs = 5000.0;
	double PollIntervalMs = 50.0;
	Params->TryGetNumberField(TEXT("timeoutMs"), TimeoutMs);
	Params->TryGetNumberField(TEXT("pollIntervalMs"), PollIntervalMs);
	if (TimeoutMs < 1.0 || TimeoutMs > 120000.0)
	{
		return InvalidParams(
			TEXT("timeoutMs must be between 1 and 120000."));
	}
	if (PollIntervalMs < 1.0 || PollIntervalMs > 1000.0)
	{
		return InvalidParams(
			TEXT("pollIntervalMs must be between 1 and 1000."));
	}
	Job.AcceptedSeconds = FPlatformTime::Seconds();
	Job.DeadlineSeconds =
		Job.AcceptedSeconds + TimeoutMs / 1000.0;
	Job.NextPollSeconds = Job.AcceptedSeconds;
	Job.PollIntervalSeconds = PollIntervalMs / 1000.0;

	while (Impl->WaitJobs.Num() >= MaxRetainedAsyncJobs)
	{
		FString OldestId;
		double OldestSeconds = TNumericLimits<double>::Max();
		for (const TPair<FString, FRuntimeWaitJob>& Pair : Impl->WaitJobs)
		{
			if (Pair.Value.Status != TEXT("running")
				&& Pair.Value.AcceptedSeconds < OldestSeconds)
			{
				OldestId = Pair.Key;
				OldestSeconds = Pair.Value.AcceptedSeconds;
			}
		}
		if (OldestId.IsEmpty())
		{
			return FRuntimeServiceResult::Error(
				TEXT("wait_busy"),
				TEXT("The runtime wait result store is full."),
				409);
		}
		Impl->WaitJobs.Remove(OldestId);
	}

	const FString WaitId = Job.Id;
	Impl->WaitJobs.Add(WaitId, MoveTemp(Job));
	Tick();
	return FRuntimeServiceResult::Ok(
		BuildWaitData(Impl->WaitJobs.FindChecked(WaitId)));
}

FRuntimeServiceResult FRuntimeSceneService::GetWait(
	const TSharedPtr<FJsonObject>& Params)
{
	FString WaitId;
	if (!Params->TryGetStringField(TEXT("waitId"), WaitId)
		|| WaitId.IsEmpty())
	{
		return InvalidParams(TEXT("waitId is required."));
	}
	const FRuntimeWaitJob* Job = Impl->WaitJobs.Find(WaitId);
	if (!Job)
	{
		return RuntimeObjectNotFound(
			FString::Printf(
				TEXT("Runtime wait '%s' was not found."),
				*WaitId));
	}
	return FRuntimeServiceResult::Ok(BuildWaitData(*Job));
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FRuntimeSceneService::GetActivePointerSequenceCountForTesting() const
{
	return Impl->ActivePointerSequenceId.IsEmpty() ? 0 : 1;
}

int32 FRuntimeSceneService::GetPressedPointerButtonCountForTesting() const
{
	return Impl->Input.GetPressedPointerButtonCountForTesting();
}

int32 FRuntimeSceneService::GetPressedKeyCountForTesting() const
{
	return Impl->Input.GetPressedKeyCountForTesting();
}

bool FRuntimeSceneService::SetWaitLogPathForTesting(
	const FString& WaitId,
	const FString& InLogPath)
{
	FRuntimeWaitJob* Job = Impl->WaitJobs.Find(WaitId);
	if (!Job)
	{
		return false;
	}
	Job->LogPath = InLogPath;
	Job->LogCursor = 0;
	Job->bLogReadSucceeded = false;
	Job->bLogReadFailed = false;
	return true;
}
#endif
}
