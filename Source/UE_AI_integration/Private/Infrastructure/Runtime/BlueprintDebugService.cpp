#include "Infrastructure/Runtime/BlueprintDebugService.h"

#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"

#include "CoreGlobals.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "HttpServerModule.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/WatchedPin.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/UObjectIterator.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
constexpr int32 DefaultTraceLimit = 100;
constexpr int32 MaxTraceLimit = 500;
constexpr int32 MaxRetainedTraceEvents = 4096;
constexpr int32 MaxCommandHistory = 128;

enum class EDebugCommandType : uint8
{
	BreakpointSet,
	BreakpointRemove,
	WatchSet,
	WatchRemove,
	Control,
};

struct FDebugCommand
{
	EDebugCommandType Type = EDebugCommandType::Control;
	FString CommandId;
	FString SessionId;
	uint64 Generation = 0;
	FString DebugSessionId;
	FString Blueprint;
	FString NodeGuid;
	FString PinGuid;
	TArray<FName> PropertyPath;
	FString Action;
	bool bEnabled = true;
	FString Signature;
};

struct FDebugCommandRecord
{
	FString CommandId;
	FString Type;
	FString Status;
	FString ErrorCode;
	FString ErrorMessage;
};

struct FTraceRecord
{
	uint64 Cursor = 0;
	FString DedupeKey;
	double ObservationTime = 0.0;
	FString ContextPath;
	FString FunctionPath;
	int32 Offset = INDEX_NONE;
	FString Blueprint;
	FString Graph;
	FString NodeGuid;
	FString NodeId;
};

struct FBreakpointRecord
{
	FString Blueprint;
	FString Graph;
	FString NodeGuid;
	FString NodeId;
	bool bEnabled = false;
	bool bValid = false;
};

struct FWatchRecord
{
	FString Blueprint;
	FString Graph;
	FString NodeGuid;
	FString NodeId;
	FString PinGuid;
	FString PinName;
	TArray<FName> PropertyPath;
	FString Value;
	FString ValueStatus;
};

struct FDebugSnapshot
{
	FString SessionId;
	uint64 Generation = 0;
	FString DebugSessionId;
	bool bActive = false;
	bool bPaused = false;
	FString DebugWorld;
	FString CurrentBlueprint;
	FString CurrentGraph;
	FString CurrentNodeGuid;
	FString CurrentNodeId;
	uint64 LatestTraceCursor = 0;
	TArray<FTraceRecord> Trace;
	TMap<FString, TArray<FBreakpointRecord>> Breakpoints;
	TMap<FString, TArray<FWatchRecord>> Watches;
	TArray<FDebugCommandRecord> Commands;
};

FString BlueprintDebugBlueprintPath(const UBlueprint* Blueprint)
{
	return Blueprint ? Blueprint->GetOutermost()->GetName() : FString();
}

FString BlueprintDebugCanonicalBlueprintPath(FString Path)
{
	Path.TrimStartAndEndInline();
	return Path.StartsWith(TEXT("/"))
		? FPackageName::ObjectPathToPackageName(Path)
		: Path;
}

FString BlueprintDebugGraphName(const UEdGraphNode* Node)
{
	return Node && Node->GetGraph() ? Node->GetGraph()->GetName() : FString();
}

FString BlueprintDebugNodeGuid(const UEdGraphNode* Node)
{
	return Node
		? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)
		: FString();
}

FString BlueprintDebugNodeStableId(const UEdGraphNode* Node)
{
	const UBlueprint* Blueprint =
		Node ? FBlueprintEditorUtils::FindBlueprintForNode(Node) : nullptr;
	return Node
		? MakeStableId(
			TEXT("bpnode"),
			{
				BlueprintDebugBlueprintPath(Blueprint),
				BlueprintDebugGraphName(Node),
				BlueprintDebugNodeGuid(Node)
			})
		: FString();
}

FString BlueprintDebugCursorString(uint64 Cursor)
{
	return LexToString(Cursor);
}

bool ParseBlueprintDebugCursor(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	uint64 DefaultValue,
	uint64& OutValue)
{
	OutValue = DefaultValue;
	FString Text;
	if (!Params.IsValid() || !Params->TryGetStringField(Field, Text))
	{
		return true;
	}
	return !Text.IsEmpty() && LexTryParseString(OutValue, *Text);
}

bool ReadBlueprintDebugSessionFields(
	const TSharedPtr<FJsonObject>& Params,
	FString& OutSessionId,
	uint64& OutGeneration,
	FString* OutDebugSessionId,
	FString& OutError)
{
	double Generation = 0.0;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("sessionId"), OutSessionId)
		|| OutSessionId.IsEmpty()
		|| !Params->TryGetNumberField(TEXT("generation"), Generation)
		|| Generation < 0.0)
	{
		OutError = TEXT("sessionId and generation are required.");
		return false;
	}
	OutGeneration = static_cast<uint64>(Generation);
	if (OutDebugSessionId)
	{
		if (!Params->TryGetStringField(TEXT("debugSessionId"), *OutDebugSessionId)
			|| OutDebugSessionId->IsEmpty())
		{
			OutError = TEXT("debugSessionId is required.");
			return false;
		}
	}
	return true;
}

TArray<FName> ReadBlueprintDebugPropertyPath(
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<FName> Result;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Params.IsValid()
		&& Params->TryGetArrayField(TEXT("propertyPath"), Values)
		&& Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				Result.Add(FName(*Value->AsString()));
			}
		}
	}
	return Result;
}

FString BlueprintDebugPropertyPathString(const TArray<FName>& Path)
{
	TArray<FString> Parts;
	Parts.Reserve(Path.Num());
	for (const FName& Part : Path)
	{
		Parts.Add(Part.ToString());
	}
	return FString::Join(Parts, TEXT("."));
}

FString BlueprintDebugCommandTypeString(EDebugCommandType Type)
{
	switch (Type)
	{
	case EDebugCommandType::BreakpointSet:
		return TEXT("breakpointSet");
	case EDebugCommandType::BreakpointRemove:
		return TEXT("breakpointRemove");
	case EDebugCommandType::WatchSet:
		return TEXT("watchSet");
	case EDebugCommandType::WatchRemove:
		return TEXT("watchRemove");
	case EDebugCommandType::Control:
	default:
		return TEXT("control");
	}
}

FString BlueprintDebugWatchStatusString(
	FKismetDebugUtilities::EWatchTextResult Status)
{
	switch (Status)
	{
	case FKismetDebugUtilities::EWTR_Valid:
		return TEXT("valid");
	case FKismetDebugUtilities::EWTR_NotInScope:
		return TEXT("notInScope");
	case FKismetDebugUtilities::EWTR_NoDebugObject:
		return TEXT("noDebugObject");
	case FKismetDebugUtilities::EWTR_NoProperty:
	default:
		return TEXT("noProperty");
	}
}

TSharedRef<FJsonObject> BlueprintDebugTraceJson(const FTraceRecord& Record)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(
		TEXT("cursor"),
		BlueprintDebugCursorString(Record.Cursor));
	Json->SetNumberField(TEXT("observationTime"), Record.ObservationTime);
	Json->SetStringField(TEXT("contextPath"), Record.ContextPath);
	Json->SetStringField(TEXT("functionPath"), Record.FunctionPath);
	Json->SetNumberField(TEXT("offset"), Record.Offset);
	if (!Record.Blueprint.IsEmpty())
	{
		Json->SetStringField(TEXT("blueprint"), Record.Blueprint);
		Json->SetStringField(TEXT("graph"), Record.Graph);
		Json->SetStringField(TEXT("nodeGuid"), Record.NodeGuid);
		Json->SetStringField(TEXT("nodeId"), Record.NodeId);
	}
	return Json;
}

TSharedRef<FJsonObject> BlueprintDebugBreakpointJson(
	const FBreakpointRecord& Record)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("blueprint"), Record.Blueprint);
	Json->SetStringField(TEXT("graph"), Record.Graph);
	Json->SetStringField(TEXT("nodeGuid"), Record.NodeGuid);
	Json->SetStringField(TEXT("nodeId"), Record.NodeId);
	Json->SetBoolField(TEXT("enabled"), Record.bEnabled);
	Json->SetBoolField(TEXT("valid"), Record.bValid);
	return Json;
}

TSharedRef<FJsonObject> BlueprintDebugWatchJson(
	const FWatchRecord& Record,
	bool bIncludeValue)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("blueprint"), Record.Blueprint);
	Json->SetStringField(TEXT("graph"), Record.Graph);
	Json->SetStringField(TEXT("nodeGuid"), Record.NodeGuid);
	Json->SetStringField(TEXT("nodeId"), Record.NodeId);
	Json->SetStringField(TEXT("pinGuid"), Record.PinGuid);
	Json->SetStringField(TEXT("pinName"), Record.PinName);
	TArray<TSharedPtr<FJsonValue>> Path;
	for (const FName& Part : Record.PropertyPath)
	{
		Path.Add(MakeShared<FJsonValueString>(Part.ToString()));
	}
	Json->SetArrayField(TEXT("propertyPath"), Path);
	if (bIncludeValue)
	{
		Json->SetStringField(TEXT("value"), Record.Value);
		Json->SetStringField(TEXT("valueStatus"), Record.ValueStatus);
	}
	return Json;
}

UEdGraphPin* FindBlueprintDebugPinByGuid(
	UEdGraphNode* Node,
	const FString& PinGuid)
{
	FGuid Parsed;
	if (!Node || !FGuid::Parse(PinGuid, Parsed))
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinId == Parsed)
		{
			return Pin;
		}
	}
	return nullptr;
}

void BlueprintDebugPumpTickerObject(
	FTSTickerObjectBase& TickerObject,
	float DeltaTime)
{
	// FHttpServerModule::Tick is not exported in UE 5.3, but its public
	// FTSTickerObjectBase override is reachable through virtual dispatch.
	// Keeping the static type at the exported Core base also prevents a direct
	// link against the non-exported concrete symbol.
	TickerObject.Tick(DeltaTime);
}
}

FBlueprintDebugResult FBlueprintDebugResult::Ok(
	const TSharedPtr<FJsonObject>& InData)
{
	FBlueprintDebugResult Result;
	Result.Data = InData.IsValid() ? InData : MakeShared<FJsonObject>();
	return Result;
}

FBlueprintDebugResult FBlueprintDebugResult::Error(
	const FString& InCode,
	const FString& InMessage,
	int32 InHttpStatus)
{
	FBlueprintDebugResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = InCode;
	Result.ErrorMessage = InMessage;
	Result.HttpStatus = InHttpStatus;
	return Result;
}

class FBlueprintDebugService::FImpl
{
public:
	explicit FImpl(FPIESessionController& InPIEController)
		: PIEController(InPIEController)
	{
		EnsureSlatePreTick();
	}

	~FImpl()
	{
		if (SlatePreTickHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnPreTick().Remove(SlatePreTickHandle);
		}
	}

	void Tick()
	{
		if (!IsInGameThread())
		{
			return;
		}
		EnsureSlatePreTick();
		RefreshSessionIdentity();
		if (ConsumeCommands())
		{
			FScopeLock Lock(&SnapshotMutex);
			Published.bPaused = false;
			return;
		}
		if (!bSessionActive)
		{
			ClearInactiveDebuggerState();
			PublishSnapshot();
			return;
		}
		CaptureDebuggerState();
		PublishSnapshot();
	}

	void EnsureSlatePreTick()
	{
		if (!SlatePreTickHandle.IsValid() && FSlateApplication::IsInitialized())
		{
			SlatePreTickHandle = FSlateApplication::Get().OnPreTick().AddRaw(
				this,
				&FImpl::HandleSlatePreTick);
		}
	}

	void HandleSlatePreTick(float DeltaTime)
	{
		if (GIntraFrameDebuggingGameThread)
		{
			// Publish the pause latch before pumping HttpServer. On the very
			// first Slate pre-tick after a breakpoint, the normal subsystem
			// tick has not had an opportunity to publish this state yet. If the
			// listener were pumped first, that first request would enter the
			// normal Game Thread queue and could not complete until execution
			// resumed.
			RefreshSessionIdentity();
			if (!bSessionActive)
			{
				ClearInactiveDebuggerState();
			}
			CaptureDebuggerState();
			PublishSnapshot();

			if (!bPumpingPausedHttpServer && FHttpServerModule::IsAvailable())
			{
				TGuardValue<bool> ReentryGuard(
					bPumpingPausedHttpServer,
					true);
				BlueprintDebugPumpTickerObject(
					static_cast<FTSTickerObjectBase&>(
						FHttpServerModule::Get()),
					DeltaTime);
			}

			// The pump can enqueue a continue/step command. Consume it before
			// leaving this pre-tick so Kismet can exit its nested Slate loop.
			// Snapshot queries and rejected non-debug requests never touch the
			// ordinary request queue while the published pause latch is true.
			if (ConsumeCommands())
			{
				FScopeLock Lock(&SnapshotMutex);
				Published.bPaused = false;
			}
			return;
		}
		Tick();
	}

	FDebugSnapshot CopySnapshot() const
	{
		FScopeLock Lock(&SnapshotMutex);
		return Published;
	}

	bool IsPausedTransportActive() const
	{
		FScopeLock Lock(&SnapshotMutex);
		return Published.bPaused;
	}

	FBlueprintDebugResult ValidateSnapshotSession(
		const FDebugSnapshot& Snapshot,
		const TSharedPtr<FJsonObject>& Params,
		bool bRequireDebugSession,
		bool bRequireActive) const
	{
		FString RequestedSessionId;
		FString RequestedDebugSessionId;
		FString Error;
		uint64 RequestedGeneration = 0;
		if (!ReadBlueprintDebugSessionFields(
				Params,
				RequestedSessionId,
				RequestedGeneration,
				bRequireDebugSession ? &RequestedDebugSessionId : nullptr,
				Error))
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"), Error, 422);
		}
		if (Snapshot.SessionId != RequestedSessionId
			|| Snapshot.Generation != RequestedGeneration)
		{
			return FBlueprintDebugResult::Error(
				TEXT("stale_session_handle"),
				TEXT("The Blueprint debug request belongs to another PIE generation."),
				410);
		}
		if (bRequireDebugSession
			&& Snapshot.DebugSessionId != RequestedDebugSessionId)
		{
			return FBlueprintDebugResult::Error(
				TEXT("debug_session_not_found"),
				TEXT("The debugSessionId is unknown or belongs to an older PIE session."),
				410);
		}
		if (bRequireActive && !Snapshot.bActive)
		{
			return FBlueprintDebugResult::Error(
				TEXT("pie_not_running"),
				TEXT("PIE must be running for Blueprint debugging."),
				409);
		}
		return FBlueprintDebugResult::Ok(MakeShared<FJsonObject>());
	}

	FBlueprintDebugResult GetSession(const TSharedPtr<FJsonObject>& Params)
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		const FBlueprintDebugResult Validation =
			ValidateSnapshotSession(Snapshot, Params, false, false);
		if (!Validation.bSuccess)
		{
			return Validation;
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), TEXT("ue.blueprint-debug-session.v1"));
		Data->SetStringField(TEXT("sessionId"), Snapshot.SessionId);
		Data->SetNumberField(TEXT("generation"), static_cast<double>(Snapshot.Generation));
		Data->SetStringField(TEXT("debugSessionId"), Snapshot.DebugSessionId);
		Data->SetStringField(
			TEXT("state"),
			Snapshot.bPaused
				? TEXT("paused")
				: (Snapshot.bActive ? TEXT("running") : TEXT("stopped")));
		Data->SetBoolField(TEXT("active"), Snapshot.bActive);
		Data->SetBoolField(TEXT("paused"), Snapshot.bPaused);
		Data->SetStringField(TEXT("debugWorld"), Snapshot.DebugWorld);
		Data->SetStringField(
			TEXT("latestTraceCursor"),
			BlueprintDebugCursorString(Snapshot.LatestTraceCursor));

		TSharedRef<FJsonObject> Current = MakeShared<FJsonObject>();
		if (!Snapshot.CurrentNodeGuid.IsEmpty())
		{
			Current->SetStringField(TEXT("blueprint"), Snapshot.CurrentBlueprint);
			Current->SetStringField(TEXT("graph"), Snapshot.CurrentGraph);
			Current->SetStringField(TEXT("nodeGuid"), Snapshot.CurrentNodeGuid);
			Current->SetStringField(TEXT("nodeId"), Snapshot.CurrentNodeId);
		}
		Data->SetObjectField(TEXT("currentInstruction"), Current);

		TArray<TSharedPtr<FJsonValue>> Commands;
		for (const FDebugCommandRecord& Record : Snapshot.Commands)
		{
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("commandId"), Record.CommandId);
			Json->SetStringField(TEXT("type"), Record.Type);
			Json->SetStringField(TEXT("status"), Record.Status);
			if (!Record.ErrorCode.IsEmpty())
			{
				Json->SetStringField(TEXT("errorCode"), Record.ErrorCode);
				Json->SetStringField(TEXT("errorMessage"), Record.ErrorMessage);
			}
			Commands.Add(MakeShared<FJsonValueObject>(Json));
		}
		Data->SetArrayField(TEXT("recentCommands"), Commands);
		return FBlueprintDebugResult::Ok(Data);
	}

	FBlueprintDebugResult GetTrace(const TSharedPtr<FJsonObject>& Params)
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		const FBlueprintDebugResult Validation =
			ValidateSnapshotSession(Snapshot, Params, true, false);
		if (!Validation.bSuccess)
		{
			return Validation;
		}

		uint64 Cursor = 0;
		if (!ParseBlueprintDebugCursor(Params, TEXT("cursor"), 0, Cursor))
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"),
				TEXT("cursor must be an unsigned integer encoded as a string."),
				422);
		}
		double LimitNumber = DefaultTraceLimit;
		Params->TryGetNumberField(TEXT("limit"), LimitNumber);
		const int32 Limit = FMath::Clamp(
			static_cast<int32>(LimitNumber),
			1,
			MaxTraceLimit);

		TArray<TSharedPtr<FJsonValue>> Events;
		uint64 NextCursor = Cursor;
		int32 MatchingTotal = 0;
		for (const FTraceRecord& Record : Snapshot.Trace)
		{
			if (Record.Cursor <= Cursor)
			{
				continue;
			}
			++MatchingTotal;
			if (Events.Num() < Limit)
			{
				Events.Add(
					MakeShared<FJsonValueObject>(
						BlueprintDebugTraceJson(Record)));
				NextCursor = Record.Cursor;
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), TEXT("ue.blueprint-debug-trace.v1"));
		Data->SetStringField(TEXT("debugSessionId"), Snapshot.DebugSessionId);
		Data->SetArrayField(TEXT("events"), Events);
		Data->SetNumberField(TEXT("total"), MatchingTotal);
		Data->SetStringField(
			TEXT("cursor"),
			BlueprintDebugCursorString(Cursor));
		Data->SetStringField(
			TEXT("nextCursor"),
			BlueprintDebugCursorString(NextCursor));
		Data->SetBoolField(TEXT("hasMore"), MatchingTotal > Events.Num());
		return FBlueprintDebugResult::Ok(Data);
	}

	FBlueprintDebugResult ListBreakpoints(const TSharedPtr<FJsonObject>& Params)
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		const FBlueprintDebugResult Validation =
			ValidateSnapshotSession(Snapshot, Params, true, true);
		if (!Validation.bSuccess)
		{
			return Validation;
		}
		FString Blueprint;
		if (!Params->TryGetStringField(TEXT("blueprint"), Blueprint)
			|| Blueprint.IsEmpty())
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"), TEXT("blueprint is required."), 422);
		}
		Blueprint = BlueprintDebugCanonicalBlueprintPath(MoveTemp(Blueprint));
		const TArray<FBreakpointRecord>* Records = Snapshot.Breakpoints.Find(Blueprint);
		TArray<TSharedPtr<FJsonValue>> Values;
		if (Records)
		{
			for (const FBreakpointRecord& Record : *Records)
			{
				Values.Add(
					MakeShared<FJsonValueObject>(
						BlueprintDebugBreakpointJson(Record)));
			}
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), TEXT("ue.blueprint-breakpoints.v1"));
		Data->SetStringField(TEXT("blueprint"), Blueprint);
		Data->SetArrayField(TEXT("breakpoints"), Values);
		Data->SetNumberField(TEXT("total"), Values.Num());
		return FBlueprintDebugResult::Ok(Data);
	}

	FBlueprintDebugResult ListWatches(const TSharedPtr<FJsonObject>& Params)
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		const FBlueprintDebugResult Validation =
			ValidateSnapshotSession(Snapshot, Params, true, true);
		if (!Validation.bSuccess)
		{
			return Validation;
		}
		FString Blueprint;
		if (!Params->TryGetStringField(TEXT("blueprint"), Blueprint)
			|| Blueprint.IsEmpty())
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"), TEXT("blueprint is required."), 422);
		}
		Blueprint = BlueprintDebugCanonicalBlueprintPath(MoveTemp(Blueprint));
		const TArray<FWatchRecord>* Records = Snapshot.Watches.Find(Blueprint);
		TArray<TSharedPtr<FJsonValue>> Values;
		if (Records)
		{
			for (const FWatchRecord& Record : *Records)
			{
				Values.Add(
					MakeShared<FJsonValueObject>(
						BlueprintDebugWatchJson(Record, false)));
			}
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), TEXT("ue.blueprint-watches.v1"));
		Data->SetStringField(TEXT("blueprint"), Blueprint);
		Data->SetArrayField(TEXT("watches"), Values);
		Data->SetNumberField(TEXT("total"), Values.Num());
		return FBlueprintDebugResult::Ok(Data);
	}

	FBlueprintDebugResult GetWatchValue(const TSharedPtr<FJsonObject>& Params)
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		const FBlueprintDebugResult Validation =
			ValidateSnapshotSession(Snapshot, Params, true, true);
		if (!Validation.bSuccess)
		{
			return Validation;
		}
		FString Blueprint;
		FString Node;
		FString Pin;
		if (!Params->TryGetStringField(TEXT("blueprint"), Blueprint)
			|| !Params->TryGetStringField(TEXT("nodeGuid"), Node)
			|| !Params->TryGetStringField(TEXT("pinGuid"), Pin))
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"),
				TEXT("blueprint, nodeGuid, and pinGuid are required."),
				422);
		}
		Blueprint = BlueprintDebugCanonicalBlueprintPath(MoveTemp(Blueprint));
		const TArray<FName> RequestedPath =
			ReadBlueprintDebugPropertyPath(Params);
		const TArray<FWatchRecord>* Records = Snapshot.Watches.Find(Blueprint);
		if (Records)
		{
			for (const FWatchRecord& Record : *Records)
			{
				if (Record.NodeGuid == Node
					&& Record.PinGuid == Pin
					&& Record.PropertyPath == RequestedPath)
				{
					TSharedRef<FJsonObject> Data =
						BlueprintDebugWatchJson(Record, true);
					Data->SetStringField(TEXT("schema"), TEXT("ue.blueprint-watch-value.v1"));
					return FBlueprintDebugResult::Ok(Data);
				}
			}
		}
		return FBlueprintDebugResult::Error(
			TEXT("watch_not_found"),
			TEXT("The requested Blueprint pin watch is not registered."),
			404);
	}

	FBlueprintDebugResult Enqueue(
		FDebugCommand Command,
		const TSharedPtr<FJsonObject>& Params,
		bool bRequirePaused)
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		const FBlueprintDebugResult Validation =
			ValidateSnapshotSession(Snapshot, Params, true, true);
		if (!Validation.bSuccess)
		{
			return Validation;
		}
		if (bRequirePaused && !Snapshot.bPaused)
		{
			return FBlueprintDebugResult::Error(
				TEXT("debug_not_paused"),
				TEXT("Blueprint execution is not paused at an intra-frame breakpoint."),
				409);
		}

		Command.SessionId = Snapshot.SessionId;
		Command.Generation = Snapshot.Generation;
		Command.DebugSessionId = Snapshot.DebugSessionId;
		if (Command.CommandId.IsEmpty())
		{
			Command.CommandId =
				FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		}
		Command.Signature = FString::Printf(
			TEXT("%d|%s|%s|%s|%s|%s|%d"),
			static_cast<int32>(Command.Type),
			*Command.Blueprint,
			*Command.NodeGuid,
			*Command.PinGuid,
			*BlueprintDebugPropertyPathString(Command.PropertyPath),
			*Command.Action,
			Command.bEnabled ? 1 : 0);

		{
			FScopeLock Lock(&SnapshotMutex);
			if (const FString* ExistingSignature =
					AcceptedCommandSignatures.Find(Command.CommandId))
			{
				if (*ExistingSignature != Command.Signature)
				{
					return FBlueprintDebugResult::Error(
						TEXT("idempotency_conflict"),
						TEXT("The commandId has already been used for another debug command."),
						409);
				}
			}
			else
			{
				AcceptedCommandSignatures.Add(Command.CommandId, Command.Signature);
				FDebugCommandRecord Record;
				Record.CommandId = Command.CommandId;
				Record.Type = BlueprintDebugCommandTypeString(Command.Type);
				Record.Status = TEXT("queued");
				CommandRecords.Add(Record);
				if (CommandRecords.Num() > MaxCommandHistory)
				{
					CommandRecords.RemoveAt(
						0,
						CommandRecords.Num() - MaxCommandHistory,
						false);
				}
				Published.Commands = CommandRecords;
				CommandQueue.Enqueue(Command);
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("commandId"), Command.CommandId);
		Data->SetBoolField(TEXT("accepted"), true);
		Data->SetBoolField(TEXT("queued"), true);
		return FBlueprintDebugResult::Ok(Data);
	}

	FBlueprintDebugResult SetBreakpoint(
		const TSharedPtr<FJsonObject>& Params,
		bool bRemove,
		const FString& RequestId = FString())
	{
		FDebugCommand Command;
		Command.Type = bRemove
			? EDebugCommandType::BreakpointRemove
			: EDebugCommandType::BreakpointSet;
		Command.CommandId = RequestId;
		if (!Params->TryGetStringField(TEXT("blueprint"), Command.Blueprint)
			|| !Params->TryGetStringField(TEXT("nodeGuid"), Command.NodeGuid))
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"),
				TEXT("blueprint and nodeGuid are required."),
				422);
		}
		Command.Blueprint =
			BlueprintDebugCanonicalBlueprintPath(MoveTemp(Command.Blueprint));
		Params->TryGetBoolField(TEXT("enabled"), Command.bEnabled);
		return Enqueue(MoveTemp(Command), Params, false);
	}

	FBlueprintDebugResult SetWatch(
		const TSharedPtr<FJsonObject>& Params,
		bool bRemove,
		const FString& RequestId = FString())
	{
		FDebugCommand Command;
		Command.Type =
			bRemove ? EDebugCommandType::WatchRemove : EDebugCommandType::WatchSet;
		Command.CommandId = RequestId;
		if (!Params->TryGetStringField(TEXT("blueprint"), Command.Blueprint)
			|| !Params->TryGetStringField(TEXT("nodeGuid"), Command.NodeGuid)
			|| !Params->TryGetStringField(TEXT("pinGuid"), Command.PinGuid))
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"),
				TEXT("blueprint, nodeGuid, and pinGuid are required."),
				422);
		}
		Command.Blueprint =
			BlueprintDebugCanonicalBlueprintPath(MoveTemp(Command.Blueprint));
		Command.PropertyPath = ReadBlueprintDebugPropertyPath(Params);
		return Enqueue(MoveTemp(Command), Params, false);
	}

	FBlueprintDebugResult Control(
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestId)
	{
		FDebugCommand Command;
		Command.Type = EDebugCommandType::Control;
		Command.CommandId = RequestId;
		if (!Params->TryGetStringField(TEXT("action"), Command.Action)
			|| (Command.Action != TEXT("continue")
				&& Command.Action != TEXT("stepInto")
				&& Command.Action != TEXT("stepOver")
				&& Command.Action != TEXT("stepOut")
				&& Command.Action != TEXT("abort")))
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"),
				TEXT("action must be continue, stepInto, stepOver, stepOut, or abort."),
				422);
		}
		return Enqueue(MoveTemp(Command), Params, true);
	}

	bool TryHandlePausedRequest(
		const FString& Capability,
		const TSharedPtr<FJsonObject>& Params,
		const FString& RequestId,
		FBlueprintDebugResult& OutResult)
	{
		if (!IsPausedTransportActive())
		{
			return false;
		}
		if (Capability == TEXT("blueprint.debug.session.get"))
		{
			OutResult = GetSession(Params);
		}
		else if (Capability == TEXT("blueprint.debug.trace.get"))
		{
			OutResult = GetTrace(Params);
		}
		else if (Capability == TEXT("blueprint.debug.breakpoint.list"))
		{
			OutResult = ListBreakpoints(Params);
		}
		else if (Capability == TEXT("blueprint.debug.breakpoint.set"))
		{
			OutResult = SetBreakpoint(Params, false, RequestId);
		}
		else if (Capability == TEXT("blueprint.debug.breakpoint.remove"))
		{
			OutResult = SetBreakpoint(Params, true, RequestId);
		}
		else if (Capability == TEXT("blueprint.debug.watch.list"))
		{
			OutResult = ListWatches(Params);
		}
		else if (Capability == TEXT("blueprint.debug.watch.set"))
		{
			OutResult = SetWatch(Params, false, RequestId);
		}
		else if (Capability == TEXT("blueprint.debug.watch.remove"))
		{
			OutResult = SetWatch(Params, true, RequestId);
		}
		else if (Capability == TEXT("blueprint.debug.watch.value.get"))
		{
			OutResult = GetWatchValue(Params);
		}
		else if (Capability == TEXT("blueprint.debug.control"))
		{
			OutResult = Control(Params, RequestId);
		}
		else
		{
			return false;
		}
		return true;
	}

	FBlueprintDebugResult CollectObservedNodeIds(
		const FString& RequestedDebugSessionId,
		const FString& CursorStart,
		const FString& CursorEnd,
		TSet<FString>& OutNodeIds) const
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		if (Snapshot.DebugSessionId != RequestedDebugSessionId)
		{
			return FBlueprintDebugResult::Error(
				TEXT("debug_session_not_found"),
				TEXT("The debugSessionId is unknown or belongs to an older PIE session."),
				410);
		}
		uint64 Start = 0;
		uint64 End = MAX_uint64;
		if ((!CursorStart.IsEmpty() && !LexTryParseString(Start, *CursorStart))
			|| (!CursorEnd.IsEmpty() && !LexTryParseString(End, *CursorEnd))
			|| Start > End)
		{
			return FBlueprintDebugResult::Error(
				TEXT("invalid_params"),
				TEXT("traceRange cursors must be ordered unsigned integer strings."),
				422);
		}
		for (const FTraceRecord& Record : Snapshot.Trace)
		{
			if (Record.Cursor >= Start
				&& Record.Cursor <= End
				&& !Record.NodeId.IsEmpty())
			{
				OutNodeIds.Add(Record.NodeId);
			}
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("observedNodeCount"), OutNodeIds.Num());
		return FBlueprintDebugResult::Ok(Data);
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetSessionForTesting(
		const FString& InSessionId,
		uint64 InGeneration,
		const FString& InDebugSessionId,
		bool bInActive,
		bool bInPaused)
	{
		FScopeLock Lock(&SnapshotMutex);
		Published.SessionId = InSessionId;
		Published.Generation = InGeneration;
		Published.DebugSessionId = InDebugSessionId;
		Published.bActive = bInActive;
		Published.bPaused = bInPaused;
	}

	void AddTraceForTesting(uint64 Cursor, const FString& NodeId)
	{
		FTraceRecord Record;
		Record.Cursor = Cursor;
		Record.DedupeKey = FString::Printf(TEXT("test-%llu"), Cursor);
		Record.NodeId = NodeId;
		SeenTraceKeys.Add(Record.DedupeKey);
		TraceRecords.Add(MoveTemp(Record));
		TrimTraceHistory();
		FScopeLock Lock(&SnapshotMutex);
		Published.Trace = TraceRecords;
		Published.LatestTraceCursor = TraceRecords.IsEmpty()
			? 0
			: TraceRecords.Last().Cursor;
	}

	int32 GetTraceDedupeKeyCountForTesting() const
	{
		return SeenTraceKeys.Num();
	}

	int32 GetBlueprintScanCountForTesting() const
	{
		return BlueprintScanCountForTesting;
	}

	void GetPublishedSessionForTesting(
		FString& OutSessionId,
		uint64& OutGeneration,
		FString& OutDebugSessionId,
		bool& bOutActive,
		bool& bOutPaused,
		FString& OutCurrentNodeGuid) const
	{
		const FDebugSnapshot Snapshot = CopySnapshot();
		OutSessionId = Snapshot.SessionId;
		OutGeneration = Snapshot.Generation;
		OutDebugSessionId = Snapshot.DebugSessionId;
		bOutActive = Snapshot.bActive;
		bOutPaused = Snapshot.bPaused;
		OutCurrentNodeGuid = Snapshot.CurrentNodeGuid;
	}
#endif

private:
	void ClearInactiveDebuggerState()
	{
		bPaused = false;
		DebugWorld.Reset();
		CurrentBlueprint.Reset();
		CurrentGraph.Reset();
		CurrentNodeGuid.Reset();
		CurrentNodeId.Reset();
		TraceRecords.Reset();
		SeenTraceKeys.Reset();
		BreakpointRecords.Reset();
		WatchRecords.Reset();
	}

	void RefreshSessionIdentity()
	{
		const FRuntimeSceneService& Runtime = PIEController.GetRuntimeService();
		const FString NewSessionId = Runtime.GetSessionId();
		const uint64 NewGeneration = Runtime.GetGeneration();
		const bool bNewActive = Runtime.IsSessionActive();
		if (NewSessionId != SessionId || NewGeneration != Generation)
		{
			SessionId = NewSessionId;
			Generation = NewGeneration;
			DebugSessionId = NewSessionId.IsEmpty()
				? FString()
				: FString::Printf(
					TEXT("bpdebug-%s"),
					*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
			TraceRecords.Reset();
			SeenTraceKeys.Reset();
			NextTraceCursor = 1;
			{
				FScopeLock Lock(&SnapshotMutex);
				AcceptedCommandSignatures.Reset();
				CommandRecords.Reset();
			}
			bBlueprintSettingsDirty = true;
			SessionTraceStartSeconds = FPlatformTime::Seconds();
			if (GIntraFrameDebuggingGameThread)
			{
				// Kismet records the breakpoint sample before entering its
				// nested Slate loop. The runtime service can first observe a
				// freshly-started PIE session from inside that loop, so using
				// only the observation time here would discard the real frame
				// that caused the pause. Anchor the new session to that newest
				// sample while keeping older PIE trace entries excluded.
				const TSimpleRingBuffer<FKismetTraceSample>& Stack =
					FKismetDebugUtilities::GetTraceStack();
				if (Stack.Num() > 0)
				{
					SessionTraceStartSeconds = FMath::Min(
						SessionTraceStartSeconds,
						Stack(0).ObservationTime);
				}
			}
		}
		bSessionActive = bNewActive;
	}

	bool ConsumeCommands()
	{
		FDebugCommand Command;
		while (CommandQueue.Dequeue(Command))
		{
			FString ErrorCode;
			FString ErrorMessage;
			bool bApplied = false;
			if (Command.SessionId != SessionId
				|| Command.Generation != Generation
				|| Command.DebugSessionId != DebugSessionId)
			{
				ErrorCode = TEXT("stale_session_handle");
				ErrorMessage = TEXT("The queued command belongs to an older PIE session.");
			}
			else if (!bSessionActive)
			{
				ErrorCode = TEXT("pie_not_running");
				ErrorMessage = TEXT("PIE ended before the debug command was consumed.");
			}
			else
			{
				switch (Command.Type)
				{
				case EDebugCommandType::BreakpointSet:
				case EDebugCommandType::BreakpointRemove:
					bApplied = ApplyBreakpoint(Command, ErrorCode, ErrorMessage);
					break;
				case EDebugCommandType::WatchSet:
				case EDebugCommandType::WatchRemove:
					bApplied = ApplyWatch(Command, ErrorCode, ErrorMessage);
					break;
				case EDebugCommandType::Control:
					bApplied = ApplyControl(Command, ErrorCode, ErrorMessage);
					break;
				}
			}
			UpdateCommandRecord(Command.CommandId, bApplied, ErrorCode, ErrorMessage);
			if (bApplied && Command.Type != EDebugCommandType::Control)
			{
				bBlueprintSettingsDirty = true;
			}

			// A continue/step command asks Slate to leave the nested debugging
			// loop. Do not touch any further UObject state in this pre-tick.
			if (Command.Type == EDebugCommandType::Control && bApplied)
			{
				return true;
			}
		}
		return false;
	}

	bool ApplyBreakpoint(
		const FDebugCommand& Command,
		FString& OutErrorCode,
		FString& OutErrorMessage)
	{
		FString LoadError;
		UBlueprint* Blueprint =
			MCPHelpers::LoadBlueprintByName(Command.Blueprint, LoadError);
		UEdGraphNode* Node =
			MCPHelpers::FindNodeByGuid(Blueprint, Command.NodeGuid);
		if (!Blueprint || !Node)
		{
			OutErrorCode = Blueprint
				? TEXT("debug_node_not_found")
				: TEXT("blueprint_not_found");
			OutErrorMessage = Blueprint
				? TEXT("The breakpoint nodeGuid was not found in the Blueprint.")
				: LoadError;
			return false;
		}

		if (Command.Type == EDebugCommandType::BreakpointRemove)
		{
			FKismetDebugUtilities::RemoveBreakpointFromNode(Node, Blueprint);
			return true;
		}

		FBlueprintBreakpoint* Existing =
			FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint);
		if (Existing)
		{
			FKismetDebugUtilities::SetBreakpointEnabled(
				*Existing,
				Command.bEnabled);
			return true;
		}

		FKismetDebugUtilities::CreateBreakpoint(
			Blueprint,
			Node,
			Command.bEnabled);
		FBlueprintBreakpoint* Created =
			FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint);
		if (!Created || !FKismetDebugUtilities::IsBreakpointValid(*Created))
		{
			FKismetDebugUtilities::RemoveBreakpointFromNode(Node, Blueprint);
			OutErrorCode = TEXT("invalid_breakpoint");
			OutErrorMessage =
				TEXT("The node does not compile to a valid Blueprint breakpoint site.");
			return false;
		}
		return true;
	}

	bool ApplyWatch(
		const FDebugCommand& Command,
		FString& OutErrorCode,
		FString& OutErrorMessage)
	{
		FString LoadError;
		UBlueprint* Blueprint =
			MCPHelpers::LoadBlueprintByName(Command.Blueprint, LoadError);
		UEdGraphNode* Node =
			MCPHelpers::FindNodeByGuid(Blueprint, Command.NodeGuid);
		UEdGraphPin* Pin =
			FindBlueprintDebugPinByGuid(Node, Command.PinGuid);
		if (!Blueprint || !Node || !Pin)
		{
			OutErrorCode = !Blueprint
				? TEXT("blueprint_not_found")
				: (!Node ? TEXT("debug_node_not_found") : TEXT("debug_pin_not_found"));
			OutErrorMessage = !Blueprint
				? LoadError
				: TEXT("The requested Blueprint node or pin could not be resolved.");
			return false;
		}

		if (Command.Type == EDebugCommandType::WatchRemove)
		{
			if (!FKismetDebugUtilities::RemovePinWatch(
					Blueprint,
					Pin,
					Command.PropertyPath))
			{
				OutErrorCode = TEXT("watch_not_found");
				OutErrorMessage = TEXT("The requested pin watch is not registered.");
				return false;
			}
			return true;
		}

		if (!FKismetDebugUtilities::CanWatchPin(
				Blueprint,
				Pin,
				Command.PropertyPath))
		{
			OutErrorCode = TEXT("watch_not_supported");
			OutErrorMessage =
				TEXT("The selected pin or property path cannot be watched.");
			return false;
		}
		if (!FKismetDebugUtilities::IsPinBeingWatched(
				Blueprint,
				Pin,
				Command.PropertyPath))
		{
			FKismetDebugUtilities::AddPinWatch(
				Blueprint,
				FBlueprintWatchedPin(Pin, TArray<FName>(Command.PropertyPath)));
		}
		return true;
	}

	bool ApplyControl(
		const FDebugCommand& Command,
		FString& OutErrorCode,
		FString& OutErrorMessage)
	{
		if (!GIntraFrameDebuggingGameThread
			|| !FSlateApplication::IsInitialized())
		{
			OutErrorCode = TEXT("debug_not_paused");
			OutErrorMessage =
				TEXT("Blueprint execution is not paused at an intra-frame breakpoint.");
			return false;
		}

		if (Command.Action == TEXT("stepInto"))
		{
			FKismetDebugUtilities::RequestSingleStepIn();
		}
		else if (Command.Action == TEXT("stepOver"))
		{
			FKismetDebugUtilities::RequestStepOver();
		}
		else if (Command.Action == TEXT("stepOut"))
		{
			FKismetDebugUtilities::RequestStepOut();
		}
		else if (Command.Action == TEXT("abort"))
		{
			FKismetDebugUtilities::RequestAbortingExecution();
		}

		if (GUnrealEd)
		{
			GUnrealEd->SetPIEWorldsPaused(false);
		}
		const bool bSingleStep =
			Command.Action == TEXT("stepInto")
			|| Command.Action == TEXT("stepOver")
			|| Command.Action == TEXT("stepOut");
		FSlateApplication::Get().LeaveDebuggingMode(bSingleStep);
		return true;
	}

	void UpdateCommandRecord(
		const FString& CommandId,
		bool bSuccess,
		const FString& ErrorCode,
		const FString& ErrorMessage)
	{
		FScopeLock Lock(&SnapshotMutex);
		for (FDebugCommandRecord& Record : CommandRecords)
		{
			if (Record.CommandId == CommandId)
			{
				Record.Status = bSuccess ? TEXT("applied") : TEXT("failed");
				Record.ErrorCode = ErrorCode;
				Record.ErrorMessage = ErrorMessage;
				break;
			}
		}
		Published.Commands = CommandRecords;
	}

	void CaptureDebuggerState()
	{
		bPaused = GIntraFrameDebuggingGameThread;
		DebugWorld.Reset();
		if (UWorld* World = FKismetDebugUtilities::GetCurrentDebuggingWorld())
		{
			DebugWorld = World->GetPathName();
		}

		UEdGraphNode* Current = FKismetDebugUtilities::GetCurrentInstruction();
		CurrentBlueprint.Reset();
		CurrentGraph.Reset();
		CurrentNodeGuid.Reset();
		CurrentNodeId.Reset();
		if (Current)
		{
			const UBlueprint* Blueprint =
				FBlueprintEditorUtils::FindBlueprintForNode(Current);
			CurrentBlueprint = BlueprintDebugBlueprintPath(Blueprint);
			CurrentGraph = BlueprintDebugGraphName(Current);
			CurrentNodeGuid = BlueprintDebugNodeGuid(Current);
			CurrentNodeId = BlueprintDebugNodeStableId(Current);
		}

		CaptureTrace();
		const double Now = FPlatformTime::Seconds();
		if (bPaused
			|| bBlueprintSettingsDirty
			|| Now - LastBlueprintSettingsCaptureSeconds >= 0.5)
		{
			CaptureBlueprintSettings();
			bBlueprintSettingsDirty = false;
			LastBlueprintSettingsCaptureSeconds = Now;
		}
	}

	void CaptureTrace()
	{
		if (!bSessionActive)
		{
			return;
		}
		const TSimpleRingBuffer<FKismetTraceSample>& Stack =
			FKismetDebugUtilities::GetTraceStack();
		for (int32 Index = Stack.Num() - 1; Index >= 0; --Index)
		{
			const FKismetTraceSample& Sample = Stack(Index);
			UObject* Context = Sample.Context.Get();
			UFunction* Function = Sample.Function.Get();
			if (!Context || !Function
				|| Sample.ObservationTime + KINDA_SMALL_NUMBER
					< SessionTraceStartSeconds)
			{
				continue;
			}
			const FString Key = FString::Printf(
				TEXT("%.9f|%s|%s|%d"),
				Sample.ObservationTime,
				*Context->GetPathName(),
				*Function->GetPathName(),
				Sample.Offset);
			if (SeenTraceKeys.Contains(Key))
			{
				continue;
			}
			SeenTraceKeys.Add(Key);

			FTraceRecord Record;
			Record.Cursor = NextTraceCursor++;
			Record.DedupeKey = Key;
			Record.ObservationTime = Sample.ObservationTime;
			Record.ContextPath = Context->GetPathName();
			Record.FunctionPath = Function->GetPathName();
			Record.Offset = Sample.Offset;
			if (UEdGraphNode* Node =
					FKismetDebugUtilities::FindSourceNodeForCodeLocation(
						Context,
						Function,
						Sample.Offset,
						true))
			{
				const UBlueprint* Blueprint =
					FBlueprintEditorUtils::FindBlueprintForNode(Node);
				Record.Blueprint = BlueprintDebugBlueprintPath(Blueprint);
				Record.Graph = BlueprintDebugGraphName(Node);
				Record.NodeGuid = BlueprintDebugNodeGuid(Node);
				Record.NodeId = BlueprintDebugNodeStableId(Node);
			}
			TraceRecords.Add(MoveTemp(Record));
		}
		TrimTraceHistory();
	}

	void TrimTraceHistory()
	{
		if (TraceRecords.Num() > MaxRetainedTraceEvents)
		{
			const int32 RemoveCount =
				TraceRecords.Num() - MaxRetainedTraceEvents;
			for (int32 Index = 0; Index < RemoveCount; ++Index)
			{
				SeenTraceKeys.Remove(TraceRecords[Index].DedupeKey);
			}
			TraceRecords.RemoveAt(
				0,
				RemoveCount,
				false);
		}
	}

	void CaptureBlueprintSettings()
	{
#if WITH_DEV_AUTOMATION_TESTS
		++BlueprintScanCountForTesting;
#endif
		BreakpointRecords.Reset();
		WatchRecords.Reset();
		for (TObjectIterator<UBlueprint> It; It; ++It)
		{
			UBlueprint* Blueprint = *It;
			if (!IsValid(Blueprint)
				|| Blueprint->HasAnyFlags(RF_ClassDefaultObject))
			{
				continue;
			}
			const FString Path = BlueprintDebugBlueprintPath(Blueprint);
			FKismetDebugUtilities::ForeachBreakpoint(
				Blueprint,
				[&](FBlueprintBreakpoint& Breakpoint)
				{
					UEdGraphNode* Node = Breakpoint.GetLocation();
					if (!Node)
					{
						return;
					}
					FBreakpointRecord Record;
					Record.Blueprint = Path;
					Record.Graph = BlueprintDebugGraphName(Node);
					Record.NodeGuid = BlueprintDebugNodeGuid(Node);
					Record.NodeId = BlueprintDebugNodeStableId(Node);
					Record.bEnabled = Breakpoint.IsEnabledByUser();
					Record.bValid =
						FKismetDebugUtilities::IsBreakpointValid(Breakpoint);
					BreakpointRecords.FindOrAdd(Path).Add(MoveTemp(Record));
				});

			UObject* DebugObject = Blueprint->GetObjectBeingDebugged();
			FKismetDebugUtilities::ForeachPinPropertyWatch(
				Blueprint,
				[&](FBlueprintWatchedPin& WatchedPin)
				{
					UEdGraphPin* Pin = WatchedPin.Get();
					UEdGraphNode* Node = Pin ? Pin->GetOwningNode() : nullptr;
					if (!Pin || !Node)
					{
						return;
					}
					FWatchRecord Record;
					Record.Blueprint = Path;
					Record.Graph = BlueprintDebugGraphName(Node);
					Record.NodeGuid = BlueprintDebugNodeGuid(Node);
					Record.NodeId = BlueprintDebugNodeStableId(Node);
					Record.PinGuid =
						Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower);
					Record.PinName = Pin->PinName.ToString();
					Record.PropertyPath = WatchedPin.GetPathToProperty();
					const FKismetDebugUtilities::EWatchTextResult Status =
						FKismetDebugUtilities::GetWatchText(
							Record.Value,
							Blueprint,
							DebugObject,
							Pin);
					Record.ValueStatus =
						BlueprintDebugWatchStatusString(Status);
					WatchRecords.FindOrAdd(Path).Add(MoveTemp(Record));
				});
		}

		for (TPair<FString, TArray<FBreakpointRecord>>& Pair : BreakpointRecords)
		{
			Pair.Value.Sort(
				[](const FBreakpointRecord& Left, const FBreakpointRecord& Right)
				{
					return Left.NodeGuid < Right.NodeGuid;
				});
		}
		for (TPair<FString, TArray<FWatchRecord>>& Pair : WatchRecords)
		{
			Pair.Value.Sort(
				[](const FWatchRecord& Left, const FWatchRecord& Right)
				{
					if (Left.NodeGuid != Right.NodeGuid)
					{
						return Left.NodeGuid < Right.NodeGuid;
					}
					if (Left.PinGuid != Right.PinGuid)
					{
						return Left.PinGuid < Right.PinGuid;
					}
					return BlueprintDebugPropertyPathString(Left.PropertyPath)
						< BlueprintDebugPropertyPathString(Right.PropertyPath);
				});
		}
	}

	void PublishSnapshot()
	{
		FDebugSnapshot Next;
		Next.SessionId = SessionId;
		Next.Generation = Generation;
		Next.DebugSessionId = DebugSessionId;
		Next.bActive = bSessionActive;
		Next.bPaused = bPaused;
		Next.DebugWorld = DebugWorld;
		Next.CurrentBlueprint = CurrentBlueprint;
		Next.CurrentGraph = CurrentGraph;
		Next.CurrentNodeGuid = CurrentNodeGuid;
		Next.CurrentNodeId = CurrentNodeId;
		Next.LatestTraceCursor =
			TraceRecords.IsEmpty() ? 0 : TraceRecords.Last().Cursor;
		Next.Trace = TraceRecords;
		Next.Breakpoints = BreakpointRecords;
		Next.Watches = WatchRecords;
		{
			FScopeLock Lock(&SnapshotMutex);
			Next.Commands = CommandRecords;
			Published = MoveTemp(Next);
		}
	}

	FPIESessionController& PIEController;
	FDelegateHandle SlatePreTickHandle;
	TQueue<FDebugCommand, EQueueMode::Mpsc> CommandQueue;

	mutable FCriticalSection SnapshotMutex;
	FDebugSnapshot Published;
	TMap<FString, FString> AcceptedCommandSignatures;
	TArray<FDebugCommandRecord> CommandRecords;

	FString SessionId;
	uint64 Generation = 0;
	FString DebugSessionId;
	bool bSessionActive = false;
	bool bPaused = false;
	double SessionTraceStartSeconds = 0.0;
	uint64 NextTraceCursor = 1;
	TSet<FString> SeenTraceKeys;
	TArray<FTraceRecord> TraceRecords;
	TMap<FString, TArray<FBreakpointRecord>> BreakpointRecords;
	TMap<FString, TArray<FWatchRecord>> WatchRecords;
	FString DebugWorld;
	FString CurrentBlueprint;
	FString CurrentGraph;
	FString CurrentNodeGuid;
	FString CurrentNodeId;
	bool bBlueprintSettingsDirty = true;
	double LastBlueprintSettingsCaptureSeconds = 0.0;
	bool bPumpingPausedHttpServer = false;
#if WITH_DEV_AUTOMATION_TESTS
	int32 BlueprintScanCountForTesting = 0;
#endif
};

FBlueprintDebugService::FBlueprintDebugService(
	FPIESessionController& InPIEController)
	: Impl(MakeUnique<FImpl>(InPIEController))
{
}

FBlueprintDebugService::~FBlueprintDebugService() = default;

void FBlueprintDebugService::Tick()
{
	Impl->Tick();
}

FBlueprintDebugResult FBlueprintDebugService::GetSession(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->GetSession(Params);
}

FBlueprintDebugResult FBlueprintDebugService::GetTrace(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->GetTrace(Params);
}

FBlueprintDebugResult FBlueprintDebugService::ListBreakpoints(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->ListBreakpoints(Params);
}

FBlueprintDebugResult FBlueprintDebugService::SetBreakpoint(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->SetBreakpoint(Params, false);
}

FBlueprintDebugResult FBlueprintDebugService::RemoveBreakpoint(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->SetBreakpoint(Params, true);
}

FBlueprintDebugResult FBlueprintDebugService::ListWatches(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->ListWatches(Params);
}

FBlueprintDebugResult FBlueprintDebugService::SetWatch(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->SetWatch(Params, false);
}

FBlueprintDebugResult FBlueprintDebugService::RemoveWatch(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->SetWatch(Params, true);
}

FBlueprintDebugResult FBlueprintDebugService::GetWatchValue(
	const TSharedPtr<FJsonObject>& Params)
{
	return Impl->GetWatchValue(Params);
}

FBlueprintDebugResult FBlueprintDebugService::Control(
	const TSharedPtr<FJsonObject>& Params,
	const FString& RequestId)
{
	return Impl->Control(Params, RequestId);
}

bool FBlueprintDebugService::TryHandlePausedRequest(
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Params,
	const FString& RequestId,
	FBlueprintDebugResult& OutResult)
{
	return Impl->TryHandlePausedRequest(
		Capability,
		Params,
		RequestId,
		OutResult);
}

bool FBlueprintDebugService::IsPausedTransportActive() const
{
	return Impl->IsPausedTransportActive();
}

FBlueprintDebugResult FBlueprintDebugService::CollectObservedNodeIds(
	const FString& RequestedDebugSessionId,
	const FString& CursorStart,
	const FString& CursorEnd,
	TSet<FString>& OutNodeIds) const
{
	return Impl->CollectObservedNodeIds(
		RequestedDebugSessionId,
		CursorStart,
		CursorEnd,
		OutNodeIds);
}

#if WITH_DEV_AUTOMATION_TESTS
void FBlueprintDebugService::SetSessionForTesting(
	const FString& InSessionId,
	uint64 InGeneration,
	const FString& InDebugSessionId,
	bool bInActive,
	bool bInPaused)
{
	Impl->SetSessionForTesting(
		InSessionId,
		InGeneration,
		InDebugSessionId,
		bInActive,
		bInPaused);
}

void FBlueprintDebugService::AddTraceForTesting(
	uint64 Cursor,
	const FString& NodeId)
{
	Impl->AddTraceForTesting(Cursor, NodeId);
}

int32 FBlueprintDebugService::GetTraceDedupeKeyCountForTesting() const
{
	return Impl->GetTraceDedupeKeyCountForTesting();
}

int32 FBlueprintDebugService::GetBlueprintScanCountForTesting() const
{
	return Impl->GetBlueprintScanCountForTesting();
}

void FBlueprintDebugService::GetPublishedSessionForTesting(
	FString& OutSessionId,
	uint64& OutGeneration,
	FString& OutDebugSessionId,
	bool& bOutActive,
	bool& bOutPaused,
	FString& OutCurrentNodeGuid) const
{
	Impl->GetPublishedSessionForTesting(
		OutSessionId,
		OutGeneration,
		OutDebugSessionId,
		bOutActive,
		bOutPaused,
		OutCurrentNodeGuid);
}
#endif
}
