#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/Runtime/BlueprintDebugService.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/Async.h"
#include "Common/TcpSocketBuilder.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Factories/BlueprintFactory.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/WatchedPin.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "PlayInEditorDataTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"

namespace
{
using UEAIIntegration::Infrastructure::FBlueprintDebugResult;
using UEAIIntegration::Infrastructure::FBlueprintDebugService;

struct FBlueprintDebugHttpResponse
{
	int32 StatusCode = 0;
	TSharedPtr<FJsonObject> Json;
	FString Error;
};

struct FBlueprintDebugPIEWorkerResult
{
	bool bFirstPauseSeen = false;
	bool bPausedMutationRejected = false;
	bool bSessionRead = false;
	bool bTraceRead = false;
	bool bWatchRead = false;
	bool bStepAccepted = false;
	bool bSecondPauseSeen = false;
	bool bContinueAccepted = false;
	FString Error;
};

struct FBlueprintDebugPIEState
{
	FBlueprintDebugService* Service = nullptr;
	FUEAIIntegrationServer* Server = nullptr;
	TWeakObjectPtr<UBlueprint> Blueprint;
	TWeakObjectPtr<AActor> PlacedActor;
	FString AssetPath;
	FString FirstNodeGuid;
	FString SecondNodeGuid;
	FString WatchNodeGuid;
	FString WatchPinGuid;
	bool bEditorWorldWasDirty = false;
	TAtomic<bool> bWorkerDone{false};
	FCriticalSection ResultMutex;
	FBlueprintDebugPIEWorkerResult WorkerResult;
	double DeadlineSeconds = 0.0;
	bool bStopRequested = false;
};

using FBlueprintDebugPIEStateRef =
	TSharedRef<FBlueprintDebugPIEState, ESPMode::ThreadSafe>;

FString BlueprintDebugPIESerializeJson(
	const TSharedPtr<FJsonObject>& Object)
{
	FString Result;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Result);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Result;
}

int32 BlueprintDebugPIEFindHeaderEnd(const TArray<uint8>& Bytes)
{
	for (int32 Index = 0; Index + 3 < Bytes.Num(); ++Index)
	{
		if (Bytes[Index] == '\r'
			&& Bytes[Index + 1] == '\n'
			&& Bytes[Index + 2] == '\r'
			&& Bytes[Index + 3] == '\n')
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FString BlueprintDebugPIEUtf8String(
	const uint8* Data,
	int32 Length)
{
	if (!Data || Length <= 0)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converter(
		reinterpret_cast<const ANSICHAR*>(Data),
		Length);
	return FString(Converter.Length(), Converter.Get());
}

bool BlueprintDebugPIEParseHttpResponse(
	const TArray<uint8>& Bytes,
	FBlueprintDebugHttpResponse& OutResponse)
{
	const int32 HeaderEnd = BlueprintDebugPIEFindHeaderEnd(Bytes);
	if (HeaderEnd == INDEX_NONE)
	{
		OutResponse.Error = TEXT("Loopback HTTP response has no header terminator.");
		return false;
	}
	const FString Header =
		BlueprintDebugPIEUtf8String(Bytes.GetData(), HeaderEnd);
	TArray<FString> HeaderLines;
	Header.ParseIntoArrayLines(HeaderLines, false);
	if (HeaderLines.IsEmpty())
	{
		OutResponse.Error = TEXT("Loopback HTTP response has no status line.");
		return false;
	}
	TArray<FString> StatusParts;
	HeaderLines[0].ParseIntoArrayWS(StatusParts);
	if (StatusParts.Num() < 2
		|| !LexTryParseString(OutResponse.StatusCode, *StatusParts[1]))
	{
		OutResponse.Error = TEXT("Loopback HTTP response has an invalid status line.");
		return false;
	}

	const int32 BodyOffset = HeaderEnd + 4;
	const FString Body = BlueprintDebugPIEUtf8String(
		Bytes.GetData() + BodyOffset,
		Bytes.Num() - BodyOffset);
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, OutResponse.Json)
		|| !OutResponse.Json.IsValid())
	{
		OutResponse.Error = TEXT("Loopback HTTP response body is not JSON.");
		return false;
	}
	return true;
}

bool BlueprintDebugPIESendRawHttp(
	int32 Port,
	const TSharedPtr<FJsonObject>& Envelope,
	FBlueprintDebugHttpResponse& OutResponse)
{
	FSocket* Socket = FTcpSocketBuilder(TEXT("UE_AI Blueprint Debug PIE HTTP"))
		.AsBlocking()
		.WithReceiveBufferSize(64 * 1024)
		.WithSendBufferSize(64 * 1024);
	if (!Socket)
	{
		OutResponse.Error = TEXT("Could not create loopback TCP socket.");
		return false;
	}
	ISocketSubsystem* SocketSubsystem =
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	const auto DestroySocket = [&]()
	{
		Socket->Close();
		SocketSubsystem->DestroySocket(Socket);
	};

	const TSharedRef<FInternetAddr> Address =
		FIPv4Endpoint(FIPv4Address::InternalLoopback, Port).ToInternetAddr();
	if (!Socket->Connect(*Address))
	{
		OutResponse.Error = TEXT("Could not connect to the plugin loopback HTTP server.");
		DestroySocket();
		return false;
	}

	const FString Body = BlueprintDebugPIESerializeJson(Envelope);
	const FTCHARToUTF8 BodyUtf8(*Body);
	const FString Header = FString::Printf(
		TEXT(
			"POST /api/execute HTTP/1.1\r\n"
			"Host: 127.0.0.1:%d\r\n"
			"Content-Type: application/json\r\n"
			"Accept: application/json\r\n"
			"Connection: close\r\n"
			"Content-Length: %d\r\n\r\n"),
		Port,
		BodyUtf8.Length());
	const FTCHARToUTF8 HeaderUtf8(*Header);
	TArray<uint8> RequestBytes;
	RequestBytes.Append(
		reinterpret_cast<const uint8*>(HeaderUtf8.Get()),
		HeaderUtf8.Length());
	RequestBytes.Append(
		reinterpret_cast<const uint8*>(BodyUtf8.Get()),
		BodyUtf8.Length());

	int32 SentTotal = 0;
	while (SentTotal < RequestBytes.Num())
	{
		int32 SentNow = 0;
		if (!Socket->Send(
				RequestBytes.GetData() + SentTotal,
				RequestBytes.Num() - SentTotal,
				SentNow)
			|| SentNow <= 0)
		{
			OutResponse.Error = TEXT("Could not send loopback HTTP request.");
			DestroySocket();
			return false;
		}
		SentTotal += SentNow;
	}

	TArray<uint8> ResponseBytes;
	int32 ExpectedBytes = INDEX_NONE;
	const double Deadline = FPlatformTime::Seconds() + 3.0;
	while (FPlatformTime::Seconds() < Deadline)
	{
		if (!Socket->Wait(
				ESocketWaitConditions::WaitForRead,
				FTimespan::FromMilliseconds(100)))
		{
			continue;
		}
		uint32 PendingSize = 0;
		if (!Socket->HasPendingData(PendingSize) || PendingSize == 0)
		{
			continue;
		}
		const int32 ReadSize =
			FMath::Min<int32>(static_cast<int32>(PendingSize), 16 * 1024);
		const int32 Offset = ResponseBytes.AddUninitialized(ReadSize);
		int32 ReadNow = 0;
		if (!Socket->Recv(
				ResponseBytes.GetData() + Offset,
				ReadSize,
				ReadNow))
		{
			ResponseBytes.SetNum(Offset, false);
			break;
		}
		ResponseBytes.SetNum(Offset + ReadNow, false);

		if (ExpectedBytes == INDEX_NONE)
		{
			const int32 HeaderEnd =
				BlueprintDebugPIEFindHeaderEnd(ResponseBytes);
			if (HeaderEnd != INDEX_NONE)
			{
				const FString ResponseHeader =
					BlueprintDebugPIEUtf8String(
						ResponseBytes.GetData(),
						HeaderEnd);
				TArray<FString> Lines;
				ResponseHeader.ParseIntoArrayLines(Lines, false);
				for (const FString& Line : Lines)
				{
					if (Line.StartsWith(
							TEXT("Content-Length:"),
							ESearchCase::IgnoreCase))
					{
						FString LengthText =
							Line.Mid(15).TrimStartAndEnd();
						int32 ContentLength = 0;
						if (LexTryParseString(ContentLength, *LengthText))
						{
							ExpectedBytes = HeaderEnd + 4 + ContentLength;
						}
						break;
					}
				}
			}
		}
		if (ExpectedBytes != INDEX_NONE
			&& ResponseBytes.Num() >= ExpectedBytes)
		{
			ResponseBytes.SetNum(ExpectedBytes, false);
			break;
		}
	}
	DestroySocket();
	if (ResponseBytes.IsEmpty())
	{
		OutResponse.Error = TEXT("Loopback HTTP request timed out.");
		return false;
	}
	return BlueprintDebugPIEParseHttpResponse(ResponseBytes, OutResponse);
}

TSharedRef<FJsonObject> BlueprintDebugPIEMakeSessionParams(
	const FString& SessionId,
	uint64 Generation,
	const FString& DebugSessionId = FString())
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("sessionId"), SessionId);
	Params->SetNumberField(
		TEXT("generation"),
		static_cast<double>(Generation));
	if (!DebugSessionId.IsEmpty())
	{
		Params->SetStringField(TEXT("debugSessionId"), DebugSessionId);
	}
	return Params;
}

bool BlueprintDebugPIEExecuteHttp(
	int32 Port,
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Params,
	const FString& RequestId,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError)
{
	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("capability"), Capability);
	Envelope->SetObjectField(TEXT("params"), Params);
	if (!RequestId.IsEmpty())
	{
		Envelope->SetStringField(TEXT("requestId"), RequestId);
	}
	FBlueprintDebugHttpResponse Response;
	if (!BlueprintDebugPIESendRawHttp(Port, Envelope, Response))
	{
		OutError = Response.Error;
		return false;
	}
	if (Response.StatusCode != 200
		|| !Response.Json->GetBoolField(TEXT("ok")))
	{
		OutError = FString::Printf(
			TEXT("Capability %s returned HTTP %d: %s"),
			*Capability,
			Response.StatusCode,
			*BlueprintDebugPIESerializeJson(Response.Json));
		return false;
	}
	OutData = Response.Json->GetObjectField(TEXT("data"));
	return OutData.IsValid();
}

bool BlueprintDebugPIEExpectPausedRouteLocked(
	int32 Port,
	FString& OutError)
{
	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(
		TEXT("capability"),
		TEXT("scene.actor.spawn"));
	Envelope->SetObjectField(
		TEXT("params"),
		MakeShared<FJsonObject>());
	FBlueprintDebugHttpResponse Response;
	if (!BlueprintDebugPIESendRawHttp(Port, Envelope, Response))
	{
		OutError = Response.Error;
		return false;
	}
	if (Response.StatusCode != 423
		|| Response.Json->GetBoolField(TEXT("ok")))
	{
		OutError = FString::Printf(
			TEXT("Paused mutating route returned HTTP %d instead of 423."),
			Response.StatusCode);
		return false;
	}
	const TSharedPtr<FJsonObject> Error =
		Response.Json->GetObjectField(TEXT("error"));
	if (!Error.IsValid()
		|| Error->GetStringField(TEXT("code"))
			!= TEXT("debug_session_paused"))
	{
		OutError =
			TEXT("Paused mutating route returned the wrong error contract.");
		return false;
	}
	return true;
}

void BlueprintDebugPIEQueueContinueFallback(
	FBlueprintDebugService& Service)
{
	FString SessionId;
	uint64 Generation = 0;
	FString DebugSessionId;
	bool bActive = false;
	bool bPaused = false;
	FString CurrentNodeGuid;
	Service.GetPublishedSessionForTesting(
		SessionId,
		Generation,
		DebugSessionId,
		bActive,
		bPaused,
		CurrentNodeGuid);
	if (!bActive || !bPaused)
	{
		return;
	}
	TSharedRef<FJsonObject> Params =
		BlueprintDebugPIEMakeSessionParams(
			SessionId,
			Generation,
			DebugSessionId);
	Params->SetStringField(TEXT("action"), TEXT("continue"));
	FBlueprintDebugResult Ignored;
	Service.TryHandlePausedRequest(
		TEXT("blueprint.debug.control"),
		Params,
		FString::Printf(
			TEXT("automation-fallback-%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits)),
		Ignored);
}

bool BlueprintDebugPIEWaitForPause(
	FBlueprintDebugService& Service,
	const FString& ExpectedNodeGuid,
	double Deadline,
	FString& OutSessionId,
	uint64& OutGeneration,
	FString& OutDebugSessionId)
{
	while (FPlatformTime::Seconds() < Deadline)
	{
		bool bActive = false;
		bool bPaused = false;
		FString CurrentNodeGuid;
		Service.GetPublishedSessionForTesting(
			OutSessionId,
			OutGeneration,
			OutDebugSessionId,
			bActive,
			bPaused,
			CurrentNodeGuid);
		if (bActive
			&& bPaused
			&& CurrentNodeGuid == ExpectedNodeGuid)
		{
			return true;
		}
		FPlatformProcess::Sleep(0.002f);
	}
	return false;
}

void BlueprintDebugPIERunWorker(
	const FBlueprintDebugPIEStateRef& State)
{
	FBlueprintDebugPIEWorkerResult Result;
	FBlueprintDebugService& Service = *State->Service;
	const int32 Port = State->Server->GetPort();
	const double FirstPauseDeadline =
		FPlatformTime::Seconds() + 15.0;
	FString SessionId;
	uint64 Generation = 0;
	FString DebugSessionId;
	if (!BlueprintDebugPIEWaitForPause(
			Service,
			State->FirstNodeGuid,
			FirstPauseDeadline,
			SessionId,
			Generation,
			DebugSessionId))
	{
		Result.Error = TEXT("Timed out waiting for the real Blueprint breakpoint.");
		BlueprintDebugPIEQueueContinueFallback(Service);
	}
	else
	{
		Result.bFirstPauseSeen = true;
		TSharedPtr<FJsonObject> Data;
		Result.bPausedMutationRejected =
			BlueprintDebugPIEExpectPausedRouteLocked(
				Port,
				Result.Error);
		TSharedRef<FJsonObject> SessionParams =
			BlueprintDebugPIEMakeSessionParams(SessionId, Generation);
		if (Result.Error.IsEmpty())
		{
			if (BlueprintDebugPIEExecuteHttp(
					Port,
					TEXT("blueprint.debug.session.get"),
					SessionParams,
					FString(),
					Data,
					Result.Error))
			{
				const TSharedPtr<FJsonObject> Current =
					Data->GetObjectField(TEXT("currentInstruction"));
				Result.bSessionRead =
					Data->GetBoolField(TEXT("paused"))
					&& Current.IsValid()
					&& Current->GetStringField(TEXT("nodeGuid"))
						== State->FirstNodeGuid;
				if (!Result.bSessionRead)
				{
					Result.Error =
						TEXT("Paused session did not expose the first source instruction.");
				}
			}
		}

		if (Result.Error.IsEmpty())
		{
			TSharedRef<FJsonObject> TraceParams =
				BlueprintDebugPIEMakeSessionParams(
					SessionId,
					Generation,
					DebugSessionId);
			TraceParams->SetStringField(TEXT("cursor"), TEXT("0"));
			TraceParams->SetNumberField(TEXT("limit"), 20);
			if (BlueprintDebugPIEExecuteHttp(
					Port,
					TEXT("blueprint.debug.trace.get"),
					TraceParams,
					FString(),
					Data,
					Result.Error))
			{
				Result.bTraceRead =
					!Data->GetArrayField(TEXT("events")).IsEmpty();
				if (!Result.bTraceRead)
				{
					Result.Error =
						TEXT("Paused trace endpoint returned no Kismet frames.");
				}
			}
		}

		if (Result.Error.IsEmpty())
		{
			TSharedRef<FJsonObject> WatchParams =
				BlueprintDebugPIEMakeSessionParams(
					SessionId,
					Generation,
					DebugSessionId);
			WatchParams->SetStringField(
				TEXT("blueprint"),
				State->AssetPath);
			WatchParams->SetStringField(
				TEXT("nodeGuid"),
				State->WatchNodeGuid);
			WatchParams->SetStringField(
				TEXT("pinGuid"),
				State->WatchPinGuid);
			if (BlueprintDebugPIEExecuteHttp(
					Port,
					TEXT("blueprint.debug.watch.value.get"),
					WatchParams,
					FString(),
					Data,
					Result.Error))
			{
				Result.bWatchRead =
					Data->GetStringField(TEXT("valueStatus"))
						== TEXT("valid")
					&& !Data->GetStringField(TEXT("value")).IsEmpty();
				if (!Result.bWatchRead)
				{
					Result.Error =
						TEXT("Paused watch endpoint did not expose a valid value.");
				}
			}
		}

		if (Result.Error.IsEmpty())
		{
			TSharedRef<FJsonObject> StepParams =
				BlueprintDebugPIEMakeSessionParams(
					SessionId,
					Generation,
					DebugSessionId);
			StepParams->SetStringField(TEXT("action"), TEXT("stepOver"));
			if (BlueprintDebugPIEExecuteHttp(
					Port,
					TEXT("blueprint.debug.control"),
					StepParams,
					TEXT("automation-step-over"),
					Data,
					Result.Error))
			{
				Result.bStepAccepted =
					Data->GetBoolField(TEXT("accepted"));
			}
		}

		if (!Result.Error.IsEmpty())
		{
			BlueprintDebugPIEQueueContinueFallback(Service);
		}
		else
		{
			const double SecondPauseDeadline =
				FPlatformTime::Seconds() + 8.0;
			if (!BlueprintDebugPIEWaitForPause(
					Service,
					State->SecondNodeGuid,
					SecondPauseDeadline,
					SessionId,
					Generation,
					DebugSessionId))
			{
				Result.Error =
					TEXT("stepOver did not pause at the next Blueprint instruction.");
				BlueprintDebugPIEQueueContinueFallback(Service);
			}
			else
			{
				Result.bSecondPauseSeen = true;
				TSharedRef<FJsonObject> ContinueParams =
					BlueprintDebugPIEMakeSessionParams(
						SessionId,
						Generation,
						DebugSessionId);
				ContinueParams->SetStringField(
					TEXT("action"),
					TEXT("continue"));
				if (BlueprintDebugPIEExecuteHttp(
						Port,
						TEXT("blueprint.debug.control"),
						ContinueParams,
						TEXT("automation-continue"),
						Data,
						Result.Error))
				{
					Result.bContinueAccepted =
						Data->GetBoolField(TEXT("accepted"));
				}
				if (!Result.bContinueAccepted)
				{
					BlueprintDebugPIEQueueContinueFallback(Service);
				}
			}
		}
	}

	{
		FScopeLock Lock(&State->ResultMutex);
		State->WorkerResult = MoveTemp(Result);
	}
	State->bWorkerDone.Store(true);
}

UEdGraphNode* BlueprintDebugPIEPerformNode(
	UK2Node* NodeTemplate,
	UEdGraph* Graph,
	const FVector2D& Position,
	UEdGraphPin* ConnectPin = nullptr)
{
	TSharedPtr<FEdGraphSchemaAction_K2NewNode> Action =
		MakeShared<FEdGraphSchemaAction_K2NewNode>(
			FText::GetEmpty(),
			FText::GetEmpty(),
			FText::GetEmpty(),
			0);
	Action->NodeTemplate = NodeTemplate;
	return Action->PerformAction(
		Graph,
		ConnectPin,
		Position,
		false);
}

UK2Node_CallFunction* BlueprintDebugPIEAddCall(
	UBlueprint* Blueprint,
	UEdGraph* Graph,
	UClass* FunctionOwner,
	const FName FunctionName,
	const FVector2D& Position,
	UEdGraphPin* ConnectPin = nullptr)
{
	UEdGraph* TempOuter = NewObject<UEdGraph>(Blueprint);
	TempOuter->SetFlags(RF_Transient);
	UK2Node_CallFunction* Template =
		NewObject<UK2Node_CallFunction>(TempOuter);
	UFunction* Function =
		FunctionOwner->FindFunctionByName(FunctionName);
	if (!Function)
	{
		return nullptr;
	}
	Template->FunctionReference.SetFromField<UFunction>(Function, false);
	return Cast<UK2Node_CallFunction>(
		BlueprintDebugPIEPerformNode(
			Template,
			Graph,
			Position,
			ConnectPin));
}

bool BlueprintDebugPIECreateFixture(
	FBlueprintDebugPIEState& State,
	FString& OutError)
{
	const FString AssetName = FString::Printf(
		TEXT("BP_BlueprintDebug_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackageName =
		FString::Printf(TEXT("/Game/Automation/%s"), *AssetName);
	State.AssetPath =
		FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
	UPackage* Package = CreatePackage(*PackageName);
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();
	UBlueprint* Blueprint = Cast<UBlueprint>(
		Factory->FactoryCreateNew(
			UBlueprint::StaticClass(),
			Package,
			FName(*AssetName),
			RF_Public | RF_Standalone,
			nullptr,
			GWarn));
	if (!Blueprint)
	{
		OutError = TEXT("Could not create Blueprint fixture.");
		return false;
	}
	State.Blueprint = Blueprint;
	FAssetRegistryModule::AssetCreated(Blueprint);

	UEdGraph* EventGraph =
		FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!EventGraph)
	{
		OutError = TEXT("Blueprint fixture has no EventGraph.");
		return false;
	}
	UEdGraph* EventOuter = NewObject<UEdGraph>(Blueprint);
	EventOuter->SetFlags(RF_Transient);
	UK2Node_Event* EventTemplate =
		NewObject<UK2Node_Event>(EventOuter);
	EventTemplate->EventReference.SetExternalMember(
		TEXT("ReceiveBeginPlay"),
		AActor::StaticClass());
	EventTemplate->bOverrideFunction = true;
	UK2Node_Event* BeginPlay = Cast<UK2Node_Event>(
		BlueprintDebugPIEPerformNode(
			EventTemplate,
			EventGraph,
			FVector2D(0.0, 0.0)));
	if (!BeginPlay)
	{
		OutError = TEXT("Could not create ReceiveBeginPlay event.");
		return false;
	}

	UEdGraphPin* BeginThen =
		BeginPlay->FindPin(UEdGraphSchema_K2::PN_Then);
	UK2Node_CallFunction* FirstPrint =
		BlueprintDebugPIEAddCall(
			Blueprint,
			EventGraph,
			UKismetSystemLibrary::StaticClass(),
			TEXT("PrintString"),
			FVector2D(500.0, 0.0),
			BeginThen);
	UK2Node_CallFunction* SecondPrint =
		FirstPrint
			? BlueprintDebugPIEAddCall(
				Blueprint,
				EventGraph,
				UKismetSystemLibrary::StaticClass(),
				TEXT("PrintString"),
				FVector2D(900.0, 0.0),
				FirstPrint->FindPin(UEdGraphSchema_K2::PN_Then))
			: nullptr;
	UK2Node_CallFunction* ToString =
		BlueprintDebugPIEAddCall(
			Blueprint,
			EventGraph,
			UKismetStringLibrary::StaticClass(),
			TEXT("Conv_IntToString"),
			FVector2D(200.0, 180.0));
	if (!FirstPrint || !SecondPrint || !ToString)
	{
		OutError = TEXT("Could not create the Blueprint debug call chain.");
		return false;
	}

	UEdGraphPin* IntegerInput = ToString->FindPin(TEXT("InInt"));
	UEdGraphPin* StringOutput = ToString->GetReturnValuePin();
	UEdGraphPin* FirstString = FirstPrint->FindPin(TEXT("InString"));
	UEdGraphPin* SecondString = SecondPrint->FindPin(TEXT("InString"));
	const UEdGraphSchema_K2* Schema =
		Cast<UEdGraphSchema_K2>(EventGraph->GetSchema());
	if (!IntegerInput
		|| !StringOutput
		|| !FirstString
		|| !SecondString
		|| !Schema)
	{
		OutError = TEXT("Blueprint debug fixture pins are unavailable.");
		return false;
	}
	IntegerInput->DefaultValue = TEXT("4242");
	SecondString->DefaultValue = TEXT("UE_AI debug step target");
	if (!Schema->TryCreateConnection(StringOutput, FirstString))
	{
		OutError = TEXT("Could not connect the watched value to PrintString.");
		return false;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
	{
		OutError = TEXT("Blueprint debug fixture did not compile.");
		return false;
	}
	State.FirstNodeGuid =
		FirstPrint->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	State.SecondNodeGuid =
		SecondPrint->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	State.WatchNodeGuid =
		ToString->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
	State.WatchPinGuid =
		StringOutput->PinId.ToString(EGuidFormats::DigitsWithHyphensLower);

	FKismetDebugUtilities::CreateBreakpoint(
		Blueprint,
		FirstPrint,
		true);
	FBlueprintBreakpoint* Breakpoint =
		FKismetDebugUtilities::FindBreakpointForNode(
			FirstPrint,
			Blueprint);
	if (!Breakpoint
		|| !FKismetDebugUtilities::IsBreakpointValid(*Breakpoint))
	{
		OutError = TEXT("Blueprint fixture breakpoint is not valid.");
		return false;
	}
	if (!FKismetDebugUtilities::CanWatchPin(
			Blueprint,
			StringOutput))
	{
		OutError = TEXT("Blueprint fixture output pin cannot be watched.");
		return false;
	}
	FKismetDebugUtilities::AddPinWatch(
		Blueprint,
		FBlueprintWatchedPin(
			StringOutput,
			TArray<FName>()));

	UWorld* EditorWorld =
		GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!EditorWorld)
	{
		OutError = TEXT("No Editor world is available for the PIE fixture.");
		return false;
	}
	UPackage* WorldPackage = EditorWorld->GetOutermost();
	State.bEditorWorldWasDirty =
		WorldPackage && WorldPackage->IsDirty();
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(
		EditorWorld->PersistentLevel,
		Blueprint->GeneratedClass,
		TEXT("UEAIBlueprintDebugActor"));
	State.PlacedActor = EditorWorld->SpawnActor<AActor>(
		Blueprint->GeneratedClass,
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParams);
	if (!State.PlacedActor.IsValid())
	{
		OutError = TEXT("Could not place the Blueprint fixture in the Editor world.");
		return false;
	}
	return true;
}

void BlueprintDebugPIECleanup(
	FBlueprintDebugPIEState& State)
{
	if (UBlueprint* Blueprint = State.Blueprint.Get())
	{
		FKismetDebugUtilities::ClearBreakpoints(Blueprint);
		FKismetDebugUtilities::ClearPinWatches(Blueprint);
	}
	if (AActor* Actor = State.PlacedActor.Get())
	{
		if (UWorld* World = Actor->GetWorld())
		{
			World->DestroyActor(Actor);
			if (!State.bEditorWorldWasDirty)
			{
				World->GetOutermost()->SetDirtyFlag(false);
			}
		}
	}
	if (UBlueprint* Blueprint = State.Blueprint.Get())
	{
		UPackage* OriginalPackage = Blueprint->GetOutermost();
		OriginalPackage->SetDirtyFlag(false);
		if (!FApp::CanEverRender())
		{
			// The commandlet GenericWindow implementation cannot close the
			// debugger-opened Blueprint toolkit: its layout path calls
			// GetRestoredDimensions and fatals. Keep this unsaved, uniquely
			// named fixture valid for the remainder of the short Automation
			// process so later Slate prepasses never observe an empty toolkit.
			// RF_Standalone keeps it alive; no package is written to disk.
			State.Blueprint.Reset();
			return;
		}

		// A breakpoint asks Kismet to focus the Blueprint. Close that toolkit
		// while the edited object is still valid; otherwise its next Slate
		// prepass observes an empty EditingObjects array after this transient
		// fixture is renamed and garbage-collected.
		if (GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem =
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(Blueprint);
			}
		}

		// This fixture was never saved. Remove it from the registry and asset
		// namespace without invoking ObjectTools deletion, whose window-layout
		// path is unavailable in UnrealEditor-Cmd -NullRHI.
		FAssetRegistryModule::AssetDeleted(Blueprint);
		Blueprint->ClearFlags(RF_Public | RF_Standalone);
		const FName TransientName = MakeUniqueObjectName(
			GetTransientPackage(),
			Blueprint->GetClass(),
			TEXT("UEAIBlueprintDebugFixture"));
		Blueprint->Rename(
			*TransientName.ToString(),
			GetTransientPackage(),
			REN_DontCreateRedirectors
				| REN_NonTransactional
				| REN_ForceNoResetLoaders
				| REN_DoNotDirty
				| REN_ForceGlobalUnique
				| REN_SkipGeneratedClasses);
		Blueprint->MarkAsGarbage();
		State.Blueprint.Reset();
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FWaitForBlueprintDebugPIEHttp,
	FBlueprintDebugPIEStateRef,
	State,
	FAutomationTestBase*,
	Test);

bool FWaitForBlueprintDebugPIEHttp::Update()
{
	if (!State->bWorkerDone.Load())
	{
		if (FPlatformTime::Seconds() >= State->DeadlineSeconds)
		{
			Test->AddError(
				TEXT("Blueprint debug HTTP worker exceeded its bounded deadline."));
			BlueprintDebugPIEQueueContinueFallback(*State->Service);
		}
		return false;
	}

	if (GEditor
		&& (GEditor->IsPlayingSessionInEditor()
			|| GEditor->IsPlaySessionRequestQueued()))
	{
		if (!State->bStopRequested)
		{
			State->bStopRequested = true;
			if (GEditor->IsPlaySessionRequestQueued()
				&& !GEditor->IsPlayingSessionInEditor())
			{
				GEditor->CancelRequestPlaySession();
			}
			else
			{
				GEditor->RequestEndPlayMap();
			}
		}
		return false;
	}

	FBlueprintDebugPIEWorkerResult Result;
	{
		FScopeLock Lock(&State->ResultMutex);
		Result = State->WorkerResult;
	}
	Test->TestTrue(
		TEXT("A real Blueprint breakpoint entered the paused Slate loop"),
		Result.bFirstPauseSeen);
	Test->TestTrue(
		TEXT("Paused adapter rejects non-debug UObject mutations"),
		Result.bPausedMutationRejected);
	Test->TestTrue(
		TEXT("Paused HTTP session query returned the current source node"),
		Result.bSessionRead);
	Test->TestTrue(
		TEXT("Paused HTTP trace query returned Kismet trace frames"),
		Result.bTraceRead);
	Test->TestTrue(
		TEXT("Paused HTTP watch query returned a real value"),
		Result.bWatchRead);
	Test->TestTrue(
		TEXT("Paused HTTP stepOver command was accepted"),
		Result.bStepAccepted);
	Test->TestTrue(
		TEXT("stepOver paused at the next Blueprint instruction"),
		Result.bSecondPauseSeen);
	Test->TestTrue(
		TEXT("Paused HTTP continue command was accepted"),
		Result.bContinueAccepted);
	if (!Result.Error.IsEmpty())
	{
		Test->AddError(Result.Error);
	}
	BlueprintDebugPIECleanup(*State);
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintDebugPIEHttpE2ETest,
	"UE_AI_integration.BlueprintDebug.RealPIEHttpStepWatchContinue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDebugPIEHttpE2ETest::RunTest(const FString& Parameters)
{
	UUEAIIntegrationSubsystem* Subsystem =
		GEditor
			? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
			: nullptr;
	TestNotNull(TEXT("UE integration subsystem is initialized"), Subsystem);
	if (!Subsystem
		|| !Subsystem->GetServer()
		|| !Subsystem->GetServer()->IsRunning()
		|| !Subsystem->GetBlueprintDebugServiceForTesting())
	{
		return false;
	}

	const FBlueprintDebugPIEStateRef State =
		MakeShared<FBlueprintDebugPIEState, ESPMode::ThreadSafe>();
	State->Service = Subsystem->GetBlueprintDebugServiceForTesting();
	State->Server = Subsystem->GetServer();
	FString SetupError;
	if (!BlueprintDebugPIECreateFixture(*State, SetupError))
	{
		BlueprintDebugPIECleanup(*State);
		AddError(SetupError);
		return false;
	}

	State->DeadlineSeconds = FPlatformTime::Seconds() + 30.0;
	(void)Async(
		EAsyncExecution::Thread,
		[State]()
		{
			BlueprintDebugPIERunWorker(State);
		});

	FRequestPlaySessionParams PlayParams;
	GEditor->RequestPlaySession(PlayParams);
	ADD_LATENT_AUTOMATION_COMMAND(
		FWaitForBlueprintDebugPIEHttp(State, this));
	return true;
}

#endif
