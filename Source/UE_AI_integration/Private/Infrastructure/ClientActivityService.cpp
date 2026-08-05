#include "Infrastructure/ClientActivityService.h"
#include "Infrastructure/Sha256.h"

#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
FClientActivityService* GActiveClientActivityService = nullptr;
// Workflow full-detail results can legitimately exceed the compact-response
// target. Parsing is transient and metadata-only; the response body is never
// retained by the activity service.
constexpr int32 MaxActivityEnvelopeBytes = 4 * 1024 * 1024;

FString NewOpaqueId(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("%s-%s"),
		Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
}

FString NowUtc()
{
	return FDateTime::UtcNow().ToIso8601();
}

FString ReadString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

int64 ReadCount(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	double Value = 0.0;
	return Object.IsValid() && Object->TryGetNumberField(Field, Value)
		? FMath::Max<int64>(0, static_cast<int64>(Value))
		: 0;
}

bool IsTerminalRunStatus(const FString& Status)
{
	return Status == TEXT("completed")
		|| Status == TEXT("failed")
		|| Status == TEXT("blocked")
		|| Status == TEXT("rolledBack");
}

FString LeaseOverrideDigest(
	const FString& ServerInstanceId,
	const FString& Type,
	const FString& LeaseId,
	const FString& CurrentOwner,
	const FString& RequestedOwner)
{
	const FString Source = TEXT("ue.lease-override.v2|")
		+ ServerInstanceId + TEXT("|") + Type + TEXT("|") + LeaseId
		+ TEXT("|") + CurrentOwner + TEXT("|") + RequestedOwner;
	const FTCHARToUTF8 Utf8(*Source);
	FString Hex;
	TrySha256Hex(Utf8.Get(), Utf8.Length(), Hex);
	return TEXT("sha256:") + Hex;
}

}

void FClientActivityService::SetServerInstanceId(
	const FString& InServerInstanceId)
{
	FScopeLock Lock(&Mutex);
	ServerInstanceId = InServerInstanceId;
}

void FClientActivityService::SetActiveService(FClientActivityService* Service)
{
	GActiveClientActivityService = Service;
}

FClientActivityService* FClientActivityService::GetActiveService()
{
	return GActiveClientActivityService;
}

bool FClientActivityService::IsBoundedToken(
	const FString& Value,
	const int32 MaxLength,
	const bool bAllowEmpty)
{
	if ((!bAllowEmpty && Value.IsEmpty()) || Value.Len() > MaxLength)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (Character < 0x20 || Character == 0x7f)
		{
			return false;
		}
	}
	return true;
}

double FClientActivityService::Percentile(
	TArray<double> Values,
	const double Quantile)
{
	if (Values.IsEmpty())
	{
		return 0.0;
	}
	Values.Sort();
	const int32 Index = FMath::Clamp(
		FMath::CeilToInt(Quantile * Values.Num()) - 1,
		0,
		Values.Num() - 1);
	return Values[Index];
}

bool FClientActivityService::RegisterClient(
	const FClientRegistration& Registration,
	FString& OutSessionId,
	FString& OutError)
{
	OutSessionId.Reset();
	OutError.Reset();
	if ((Registration.ClientKind != TEXT("mcp")
			&& Registration.ClientKind != TEXT("cli"))
		|| !IsBoundedToken(Registration.Name, 128)
		|| !IsBoundedToken(Registration.Version, 64, true)
		|| !IsBoundedToken(Registration.Transport, 32)
		|| !IsBoundedToken(Registration.InstanceId, 128)
		|| !IsBoundedToken(Registration.InvocationId, 128, true)
		|| !IsBoundedToken(Registration.Command, 256, true))
	{
		OutError =
			TEXT("Client registration contains an unsupported kind or invalid field.");
		return false;
	}
	if (Registration.ClientKind == TEXT("cli")
		&& Registration.InvocationId.IsEmpty())
	{
		OutError = TEXT("CLI registrations require invocationId.");
		return false;
	}

	const double NowSeconds = FPlatformTime::Seconds();
	const FString Timestamp = NowUtc();
	ExpireSessions(NowSeconds);
	FScopeLock Lock(&Mutex);

	if (const FString* ExistingId =
		SessionByInstanceId.Find(Registration.InstanceId))
	{
		if (FSession* Existing = Sessions.Find(*ExistingId))
		{
			const bool bSameIdentity =
				Existing->Registration.ClientKind == Registration.ClientKind
				&& Existing->Registration.Name == Registration.Name
				&& Existing->Registration.Version == Registration.Version
				&& Existing->Registration.Transport == Registration.Transport
				&& Existing->Registration.Pid == Registration.Pid
				&& Existing->Registration.InvocationId
					== Registration.InvocationId
				&& Existing->Registration.Command == Registration.Command;
			if (!bSameIdentity)
			{
				OutError =
					TEXT("instanceId is already registered to another client.");
				return false;
			}
			Existing->LastActivitySeconds = NowSeconds;
			Existing->LastActivityAtUtc = Timestamp;
			OutSessionId = Existing->SessionId;
			return true;
		}
		SessionByInstanceId.Remove(Registration.InstanceId);
	}

	if (Sessions.Num() >= MaxSessions)
	{
		OutError = TEXT("The client session registry is full.");
		return false;
	}

	FSession Session;
	Session.SessionId = NewOpaqueId(TEXT("session"));
	Session.Registration = Registration;
	Session.RegisteredAtUtc = Timestamp;
	Session.LastActivityAtUtc = Timestamp;
	Session.RegisteredSeconds = NowSeconds;
	Session.LastActivitySeconds = NowSeconds;
	OutSessionId = Session.SessionId;
	SessionByInstanceId.Add(Registration.InstanceId, Session.SessionId);
	Sessions.Add(Session.SessionId, Session);

	if (Registration.ClientKind == TEXT("cli"))
	{
		FCallerContext Caller;
		Caller.ClientKind = Registration.ClientKind;
		Caller.Name = Registration.Name;
		Caller.Version = Registration.Version;
		Caller.Transport = Registration.Transport;
		Caller.Command = Registration.Command;
		Caller.InstanceId = Registration.InstanceId;
		Caller.InvocationId = Registration.InvocationId;
		Caller.SessionId = Session.SessionId;
		Caller.Pid = Registration.Pid;
		RecordCliInvocationLocked(Caller);
	}
	return true;
}

bool FClientActivityService::Heartbeat(
	const FString& SessionId,
	FString& OutError)
{
	OutError.Reset();
	ExpireSessions();
	FScopeLock Lock(&Mutex);
	FSession* Session = Sessions.Find(SessionId);
	if (!Session)
	{
		OutError = TEXT("Unknown or expired client session.");
		return false;
	}
	Session->LastActivitySeconds = FPlatformTime::Seconds();
	Session->LastActivityAtUtc = NowUtc();
	for (TPair<FString, FLease>& Pair : Leases)
	{
		if (Pair.Value.OwnerSessionId == SessionId)
		{
			Pair.Value.ExpiresSeconds = Session->LastActivitySeconds + 30.0;
			Pair.Value.ExpiresAtUtc = FDateTime::FromUnixTimestamp(
				FDateTime::UtcNow().ToUnixTimestamp() + 30).ToIso8601();
		}
	}
	return true;
}

bool FClientActivityService::UnregisterClient(
	const FString& SessionId,
	FString& OutError)
{
	OutError.Reset();
	const double FinishedSeconds = FPlatformTime::Seconds();
	const FString FinishedAtUtc = NowUtc();
	FScopeLock Lock(&Mutex);
	const FSession* Session = Sessions.Find(SessionId);
	if (!Session)
	{
		OutError = TEXT("Unknown or expired client session.");
		return false;
	}
	if (Session->ActiveRequestCount > 0)
	{
		OutError = TEXT("Client session has active requests.");
		return false;
	}
	if (Session->Registration.ClientKind == TEXT("cli"))
	{
		const FCliInvocation* Invocation = CliInvocations.FindByPredicate(
			[Session](const FCliInvocation& Candidate)
			{
				return Candidate.InvocationId
					== Session->Registration.InvocationId;
			});
		FinishInvocationLocked(
			Session->Registration.InvocationId,
			Invocation && Invocation->bHadFailure
				? TEXT("failed")
				: TEXT("completed"),
			FinishedSeconds,
			FinishedAtUtc);
	}
	SessionByInstanceId.Remove(Session->Registration.InstanceId);
	for (auto It = Leases.CreateIterator(); It; ++It)
	{
		if (It.Value().OwnerSessionId == SessionId) It.RemoveCurrent();
	}
	Sessions.Remove(SessionId);
	return true;
}

void FClientActivityService::DisconnectAllSessions()
{
	const double FinishedSeconds = FPlatformTime::Seconds();
	const FString FinishedAtUtc = NowUtc();
	FScopeLock Lock(&Mutex);
	for (const TPair<FString, FSession>& Pair : Sessions)
	{
		if (Pair.Value.Registration.ClientKind == TEXT("cli"))
		{
			FinishInvocationLocked(
				Pair.Value.Registration.InvocationId,
				TEXT("disconnected"),
				FinishedSeconds,
				FinishedAtUtc);
		}
	}
	Sessions.Reset();
	SessionByInstanceId.Reset();
	Leases.Reset();
}

bool FClientActivityService::BeginRequest(
	FCallerContext& Caller,
	FString& OutError)
{
	OutError.Reset();
	if (Caller.SessionId.IsEmpty())
	{
		if ((Caller.ClientKind != TEXT("legacy")
				&& Caller.ClientKind != TEXT("mcp")
				&& Caller.ClientKind != TEXT("cli"))
			|| !IsBoundedToken(Caller.ClientKind, 32)
			|| !IsBoundedToken(Caller.Name, 128)
			|| !IsBoundedToken(Caller.Version, 64, true)
			|| !IsBoundedToken(Caller.Transport, 32)
			|| !IsBoundedToken(Caller.Command, 256, true)
			|| !IsBoundedToken(Caller.InstanceId, 128, true)
			|| !IsBoundedToken(Caller.InvocationId, 128, true))
		{
			OutError = TEXT("Caller metadata contains an invalid header value.");
			return false;
		}
		if (Caller.ClientKind == TEXT("cli")
			&& Caller.InvocationId.IsEmpty())
		{
			OutError = TEXT("CLI caller metadata requires invocationId.");
			return false;
		}
		FScopeLock Lock(&Mutex);
		RecordCliInvocationLocked(Caller);
		if (Caller.ClientKind == TEXT("cli"))
		{
			if (FCliInvocation* Invocation =
				CliInvocations.FindByPredicate(
					[&Caller](const FCliInvocation& Candidate)
					{
						return Candidate.InvocationId
							== Caller.InvocationId;
					}))
			{
				++Invocation->ActiveRequestCount;
			}
		}
		return true;
	}
	if (!IsBoundedToken(Caller.SessionId, 128))
	{
		OutError = TEXT("The client session id is invalid.");
		return false;
	}

	ExpireSessions();
	FScopeLock Lock(&Mutex);
	FSession* Session = Sessions.Find(Caller.SessionId);
	if (!Session)
	{
		OutError = TEXT("The client session is unknown or expired.");
		return false;
	}
	Caller.ClientKind = Session->Registration.ClientKind;
	Caller.Name = Session->Registration.Name;
	Caller.Version = Session->Registration.Version;
	Caller.Transport = Session->Registration.Transport;
	Caller.Command = Session->Registration.Command;
	Caller.InstanceId = Session->Registration.InstanceId;
	Caller.InvocationId = Session->Registration.InvocationId;
	Caller.Pid = Session->Registration.Pid;
	Session->LastActivitySeconds = FPlatformTime::Seconds();
	Session->LastActivityAtUtc = NowUtc();
	++Session->ActiveRequestCount;
	return true;
}

void FClientActivityService::EndRequest(const FCallerContext& Caller)
{
	if (Caller.SessionId.IsEmpty())
	{
		if (Caller.ClientKind != TEXT("cli")
			|| Caller.InvocationId.IsEmpty())
		{
			return;
		}
		const double FinishedSeconds = FPlatformTime::Seconds();
		const FString FinishedAtUtc = NowUtc();
		FScopeLock Lock(&Mutex);
		if (FCliInvocation* Invocation =
			CliInvocations.FindByPredicate(
				[&Caller](const FCliInvocation& Candidate)
				{
					return Candidate.InvocationId
						== Caller.InvocationId;
				}))
		{
			Invocation->ActiveRequestCount =
				FMath::Max(0, Invocation->ActiveRequestCount - 1);
			if (Invocation->ActiveRequestCount == 0)
			{
				FinishInvocationLocked(
					Caller.InvocationId,
					Invocation->bHadFailure
						? TEXT("failed")
						: TEXT("completed"),
					FinishedSeconds,
					FinishedAtUtc);
			}
		}
		return;
	}
	FScopeLock Lock(&Mutex);
	if (FSession* Session = Sessions.Find(Caller.SessionId))
	{
		Session->ActiveRequestCount =
			FMath::Max(0, Session->ActiveRequestCount - 1);
		Session->LastActivitySeconds = FPlatformTime::Seconds();
		Session->LastActivityAtUtc = NowUtc();
	}
}

void FClientActivityService::ExpireSessions(const double CurrentSeconds)
{
	const double NowSeconds =
		CurrentSeconds >= 0.0 ? CurrentSeconds : FPlatformTime::Seconds();
	const double Cutoff = NowSeconds - SessionTimeoutSeconds;
	const FString FinishedAtUtc = NowUtc();
	FScopeLock Lock(&Mutex);
	TArray<FString> ExpiredIds;
	for (const TPair<FString, FSession>& Pair : Sessions)
	{
		if (Pair.Value.ActiveRequestCount == 0
			&& Pair.Value.LastActivitySeconds < Cutoff)
		{
			ExpiredIds.Add(Pair.Key);
		}
	}
	for (const FString& SessionId : ExpiredIds)
	{
		if (const FSession* Session = Sessions.Find(SessionId))
		{
			if (Session->Registration.ClientKind == TEXT("cli"))
			{
				FinishInvocationLocked(
					Session->Registration.InvocationId,
					TEXT("expired"),
					NowSeconds,
					FinishedAtUtc);
			}
			SessionByInstanceId.Remove(Session->Registration.InstanceId);
		}
		Sessions.Remove(SessionId);
		for (auto It = Leases.CreateIterator(); It; ++It)
		{
			if (It.Value().OwnerSessionId == SessionId) It.RemoveCurrent();
		}
	}
	for (auto It = Leases.CreateIterator(); It; ++It)
	{
		if (It.Value().ExpiresSeconds <= NowSeconds) It.RemoveCurrent();
	}
}

FString FClientActivityService::BeginActivity(
	const FCallerContext& Caller,
	const FString& Kind)
{
	FActivity Activity;
	Activity.EventId = NewOpaqueId(TEXT("event"));
	Activity.Caller = Caller;
	Activity.Kind = Kind.Left(32);
	Activity.QueuedAtUtc = NowUtc();
	Activity.QueuedSeconds = FPlatformTime::Seconds();

	FScopeLock Lock(&Mutex);
	Activities.Add(Activity);
	PruneActivitiesLocked();
	if (FSession* Session = Sessions.Find(Caller.SessionId))
	{
		Session->LastActivityAtUtc = Activity.QueuedAtUtc;
		Session->LastActivitySeconds = Activity.QueuedSeconds;
		++Session->CallCount;
	}
	RecordCliInvocationLocked(Caller);
	return Activity.EventId;
}

void FClientActivityService::MarkActivityStarted(const FString& EventId)
{
	FScopeLock Lock(&Mutex);
	if (FActivity* Activity = FindActivityLocked(EventId))
	{
		Activity->StartedAtUtc = NowUtc();
		Activity->StartedSeconds = FPlatformTime::Seconds();
		Activity->Status = TEXT("running");
	}
}

void FClientActivityService::UpdateCapabilityActivity(
	const FString& EventId,
	const FString& Capability,
	const FString& RequestId,
	const FString& Risk)
{
	FScopeLock Lock(&Mutex);
	if (FActivity* Activity = FindActivityLocked(EventId))
	{
		Activity->Capability = Capability.Left(256);
		Activity->RequestId = RequestId.Left(200);
		Activity->Risk = Risk.Left(64);
	}
}

void FClientActivityService::UpdateWorkflowActivity(
	const FString& EventId,
	const FString& WorkflowAction,
	const FString& RequestId,
	const FString& Risk)
{
	FScopeLock Lock(&Mutex);
	if (FActivity* Activity = FindActivityLocked(EventId))
	{
		Activity->WorkflowAction = WorkflowAction.Left(64);
		Activity->RequestId = RequestId.Left(200);
		Activity->Risk = Risk.Left(64);
	}
}

void FClientActivityService::MarkActivityRejected(const FString& EventId)
{
	FScopeLock Lock(&Mutex);
	if (FActivity* Activity = FindActivityLocked(EventId))
	{
		Activity->Status = TEXT("rejected");
	}
}

void FClientActivityService::CompleteActivityFromHttp(
	const FString& EventId,
	const int32 HttpStatus,
	const TArray<uint8>& ResponseBody)
{
	FString ErrorCode;
	FString RunId;
	FString JobId;
	FString RemoteStatus;
	TSharedPtr<FJsonObject> Data;
	if (!ResponseBody.IsEmpty()
		&& ResponseBody.Num() <= MaxActivityEnvelopeBytes)
	{
		const FUTF8ToTCHAR Converter(
			reinterpret_cast<const ANSICHAR*>(ResponseBody.GetData()),
			ResponseBody.Num());
		const FString Body(Converter.Length(), Converter.Get());
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Body);
		if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
		{
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(TEXT("error"), Error)
				&& Error && Error->IsValid())
			{
				(*Error)->TryGetStringField(TEXT("code"), ErrorCode);
				const TSharedPtr<FJsonObject>* Details = nullptr;
				if ((*Error)->TryGetObjectField(TEXT("details"), Details)
					&& Details && Details->IsValid())
				{
					Data = *Details;
				}
			}
			const TSharedPtr<FJsonObject>* DataPtr = nullptr;
			if (Root->TryGetObjectField(TEXT("data"), DataPtr)
				&& DataPtr && DataPtr->IsValid())
			{
				Data = *DataPtr;
			}
			if (Data.IsValid())
			{
				RunId = ReadString(Data, TEXT("runId"));
				JobId = ReadString(Data, TEXT("jobId"));
				RemoteStatus = ReadString(Data, TEXT("status"));
				if (RunId.IsEmpty())
				{
					const TSharedPtr<FJsonObject>* Receipt = nullptr;
					if (Data->TryGetObjectField(TEXT("receipt"), Receipt)
						&& Receipt && Receipt->IsValid())
					{
						RunId = ReadString(*Receipt, TEXT("runId"));
						if (RemoteStatus.IsEmpty())
						{
							RemoteStatus =
								ReadString(*Receipt, TEXT("status"));
						}
					}
				}
			}
		}
	}
	if (RemoteStatus.IsEmpty()
		&& !RunId.IsEmpty()
		&& HttpStatus >= 400)
	{
		RemoteStatus = TEXT("failed");
	}

	const double FinishedSeconds = FPlatformTime::Seconds();
	const FString FinishedAtUtc = NowUtc();
	FScopeLock Lock(&Mutex);
	if (FActivity* Activity = FindActivityLocked(EventId))
	{
		Activity->HttpStatus = HttpStatus;
		Activity->ErrorCode = ErrorCode;
		Activity->RunId = RunId;
		Activity->JobId = JobId;
		Activity->FinishedAtUtc = FinishedAtUtc;
		const double StartSeconds = Activity->StartedSeconds > 0.0
			? Activity->StartedSeconds
			: Activity->QueuedSeconds;
		Activity->DurationMs =
			FMath::Max(0.0, (FinishedSeconds - StartSeconds) * 1000.0);
		if (Activity->Status == TEXT("rejected"))
		{
			// Preserve the explicit service-disable outcome.
		}
		else if (RemoteStatus == TEXT("blocked"))
		{
			Activity->Status = TEXT("blocked");
		}
		else if (HttpStatus >= 200 && HttpStatus < 300)
		{
			Activity->Status =
				!JobId.IsEmpty()
					&& (RemoteStatus == TEXT("running")
						|| RemoteStatus == TEXT("queued"))
				? TEXT("submitted")
				: TEXT("succeeded");
		}
		else
		{
			Activity->Status = TEXT("failed");
		}
		UpdateStatisticsLocked(*Activity, Data, RemoteStatus);
		if (Activity->Caller.ClientKind == TEXT("cli")
			&& !Activity->Caller.InvocationId.IsEmpty())
		{
			if (FCliInvocation* Invocation =
				CliInvocations.FindByPredicate(
					[Activity](const FCliInvocation& Candidate)
					{
						return Candidate.InvocationId
							== Activity->Caller.InvocationId;
					}))
			{
				Invocation->bHadFailure =
					Invocation->bHadFailure
					|| Activity->Status == TEXT("failed")
					|| Activity->Status == TEXT("blocked")
					|| Activity->Status == TEXT("rejected");
			}
		}
		PruneActivitiesLocked();
	}
}

void FClientActivityService::RecordCliInvocationLocked(
	const FCallerContext& Caller)
{
	if (Caller.ClientKind != TEXT("cli")
		|| Caller.InvocationId.IsEmpty())
	{
		return;
	}
	FCliInvocation* Existing = CliInvocations.FindByPredicate(
		[&Caller](const FCliInvocation& Invocation)
		{
			return Invocation.InvocationId == Caller.InvocationId;
		});
	if (Existing)
	{
		if (!Caller.SessionId.IsEmpty())
		{
			Existing->SessionId = Caller.SessionId;
		}
		if (Existing->Status != TEXT("running"))
		{
			Existing->Status = TEXT("running");
			Existing->FinishedAtUtc.Reset();
		}
		if (!Caller.Command.IsEmpty())
		{
			Existing->Command = Caller.Command;
		}
		return;
	}
	++Statistics.CliInvocations;
	FCliInvocation Invocation;
	Invocation.InvocationId = Caller.InvocationId;
	Invocation.SessionId = Caller.SessionId;
	Invocation.Name = Caller.Name;
	Invocation.Version = Caller.Version;
	Invocation.Command = Caller.Command;
	Invocation.Pid = Caller.Pid;
	Invocation.StartedAtUtc = NowUtc();
	Invocation.StartedSeconds = FPlatformTime::Seconds();
	CliInvocations.Add(MoveTemp(Invocation));
	PruneCliInvocationsLocked();
}

void FClientActivityService::FinishInvocationLocked(
	const FString& InvocationId,
	const FString& Status,
	const double FinishedSeconds,
	const FString& FinishedAtUtc)
{
	if (FCliInvocation* Invocation = CliInvocations.FindByPredicate(
			[&InvocationId](const FCliInvocation& Candidate)
			{
				return Candidate.InvocationId == InvocationId;
			}))
	{
		Invocation->Status = Status;
		Invocation->FinishedAtUtc = FinishedAtUtc;
		Invocation->DurationMs = FMath::Max(
			0.0,
			(FinishedSeconds - Invocation->StartedSeconds) * 1000.0);
	}
	PruneCliInvocationsLocked();
}

void FClientActivityService::UpdateStatisticsLocked(
	const FActivity& Activity,
	const TSharedPtr<FJsonObject>& Data,
	const FString& RemoteStatus)
{
	const bool bSuccess =
		Activity.HttpStatus >= 200 && Activity.HttpStatus < 300
		&& Activity.Status != TEXT("rejected");
	if (Activity.Kind == TEXT("capability"))
	{
		++Statistics.CapabilityCalls;
		if (bSuccess)
		{
			++Statistics.CapabilitySucceeded;
		}
		else
		{
			++Statistics.CapabilityFailed;
		}
		AddDurationSample(
			Statistics.CapabilityDurationsMs,
			Activity.DurationMs);
		return;
	}
	if (Activity.Kind != TEXT("workflow"))
	{
		return;
	}

	++Statistics.WorkflowApiCalls;
	if (Activity.WorkflowAction == TEXT("rollback"))
	{
		++Statistics.Rollbacks;
	}
	if (Activity.RunId.IsEmpty())
	{
		return;
	}

	const bool bNewRun = !Statistics.Runs.Contains(Activity.RunId);
	if (bNewRun && Activity.WorkflowAction != TEXT("execute"))
	{
		// status/resume may observe a run created before this Editor session.
		// They are API activity, but must not manufacture a session-local DSL
		// run or terminal counters without a matching execute.
		return;
	}
	if (Activity.WorkflowAction == TEXT("execute") && bNewRun)
	{
		++Statistics.DslRuns;
		AddDurationSample(
			Statistics.DslRunDurationsMs,
			Activity.DurationMs);
	}
	if (!bNewRun && Activity.WorkflowAction == TEXT("status"))
	{
		UpdateRunStatisticsLocked(Activity.RunId, RemoteStatus, Data);
		return;
	}
	if (Activity.WorkflowAction == TEXT("execute")
		|| Activity.WorkflowAction == TEXT("resume")
		|| Activity.WorkflowAction == TEXT("status"))
	{
		UpdateRunStatisticsLocked(Activity.RunId, RemoteStatus, Data);
	}
}

void FClientActivityService::UpdateRunStatisticsLocked(
	const FString& RunId,
	const FString& Status,
	const TSharedPtr<FJsonObject>& Data)
{
	FRunStatistics& Run = Statistics.Runs.FindOrAdd(RunId);
	Run.LastTouchedOrdinal = ++RunTouchOrdinal;
	auto DecrementStatus = [this](const FString& OldStatus)
	{
		if (OldStatus == TEXT("completed"))
		{
			Statistics.DslCompleted =
				FMath::Max<int64>(0, Statistics.DslCompleted - 1);
		}
		else if (OldStatus == TEXT("failed"))
		{
			Statistics.DslFailed =
				FMath::Max<int64>(0, Statistics.DslFailed - 1);
		}
		else if (OldStatus == TEXT("blocked"))
		{
			Statistics.DslBlocked =
				FMath::Max<int64>(0, Statistics.DslBlocked - 1);
		}
	};
	if (Run.Status != Status && IsTerminalRunStatus(Run.Status))
	{
		DecrementStatus(Run.Status);
	}
	if (Run.Status != Status)
	{
		if (Status == TEXT("completed"))
		{
			++Statistics.DslCompleted;
		}
		else if (Status == TEXT("failed"))
		{
			++Statistics.DslFailed;
		}
		else if (Status == TEXT("blocked"))
		{
			++Statistics.DslBlocked;
		}
		Run.Status = Status;
	}

	const TSharedPtr<FJsonObject>* Summary = nullptr;
	const TSharedPtr<FJsonObject>* Operations = nullptr;
	if (Data.IsValid()
		&& Data->TryGetObjectField(TEXT("summary"), Summary)
		&& Summary && Summary->IsValid()
		&& (*Summary)->TryGetObjectField(TEXT("operations"), Operations)
		&& Operations && Operations->IsValid())
	{
		const int64 NewTotal = ReadCount(*Operations, TEXT("total"));
		const int64 NewSucceeded = ReadCount(*Operations, TEXT("succeeded"));
		Statistics.OperationTotal += NewTotal - Run.OperationTotal;
		Statistics.OperationSucceeded +=
			NewSucceeded - Run.OperationSucceeded;
		Run.OperationTotal = NewTotal;
		Run.OperationSucceeded = NewSucceeded;
	}
	PruneRunStatisticsLocked();
}

TSharedPtr<FJsonObject> FClientActivityService::SessionToJson(
	const FSession& Session,
	const double NowSeconds)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("sessionId"), Session.SessionId);
	Object->SetStringField(TEXT("clientKind"), Session.Registration.ClientKind);
	Object->SetStringField(TEXT("name"), Session.Registration.Name);
	Object->SetStringField(TEXT("version"), Session.Registration.Version);
	Object->SetStringField(TEXT("transport"), Session.Registration.Transport);
	Object->SetStringField(TEXT("command"), Session.Registration.Command);
	Object->SetStringField(TEXT("instanceId"), Session.Registration.InstanceId);
	Object->SetStringField(
		TEXT("invocationId"),
		Session.Registration.InvocationId);
	Object->SetNumberField(TEXT("pid"), Session.Registration.Pid);
	Object->SetStringField(TEXT("registeredAtUtc"), Session.RegisteredAtUtc);
	Object->SetStringField(
		TEXT("lastActivityAtUtc"),
		Session.LastActivityAtUtc);
	Object->SetNumberField(
		TEXT("onlineForSeconds"),
		FMath::Max(0.0, NowSeconds - Session.RegisteredSeconds));
	Object->SetNumberField(
		TEXT("callCount"),
		static_cast<double>(Session.CallCount));
	Object->SetNumberField(
		TEXT("activeRequestCount"),
		Session.ActiveRequestCount);
	return Object;
}

TSharedPtr<FJsonObject> FClientActivityService::InvocationToJson(
	const FCliInvocation& Invocation,
	const double NowSeconds)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("invocationId"), Invocation.InvocationId);
	Object->SetStringField(TEXT("sessionId"), Invocation.SessionId);
	Object->SetStringField(TEXT("name"), Invocation.Name);
	Object->SetStringField(TEXT("version"), Invocation.Version);
	Object->SetStringField(TEXT("command"), Invocation.Command);
	Object->SetNumberField(TEXT("pid"), Invocation.Pid);
	Object->SetStringField(TEXT("status"), Invocation.Status);
	Object->SetStringField(TEXT("startedAtUtc"), Invocation.StartedAtUtc);
	Object->SetStringField(TEXT("finishedAtUtc"), Invocation.FinishedAtUtc);
	Object->SetNumberField(
		TEXT("durationMs"),
		Invocation.Status == TEXT("running")
			? FMath::Max(
				0.0,
				(NowSeconds - Invocation.StartedSeconds) * 1000.0)
			: Invocation.DurationMs);
	return Object;
}

TSharedPtr<FJsonObject> FClientActivityService::ActivityToJson(
	const FActivity& Activity)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("eventId"), Activity.EventId);
	Object->SetStringField(TEXT("kind"), Activity.Kind);
	Object->SetStringField(TEXT("sessionId"), Activity.Caller.SessionId);
	Object->SetStringField(
		TEXT("invocationId"),
		Activity.Caller.InvocationId);
	Object->SetStringField(TEXT("clientKind"), Activity.Caller.ClientKind);
	Object->SetStringField(TEXT("callerName"), Activity.Caller.Name);
	Object->SetStringField(TEXT("capability"), Activity.Capability);
	Object->SetStringField(
		TEXT("workflowAction"),
		Activity.WorkflowAction);
	Object->SetStringField(TEXT("requestId"), Activity.RequestId);
	Object->SetStringField(TEXT("runId"), Activity.RunId);
	Object->SetStringField(TEXT("jobId"), Activity.JobId);
	Object->SetStringField(TEXT("queuedAtUtc"), Activity.QueuedAtUtc);
	Object->SetStringField(TEXT("startedAtUtc"), Activity.StartedAtUtc);
	Object->SetStringField(TEXT("finishedAtUtc"), Activity.FinishedAtUtc);
	Object->SetNumberField(TEXT("durationMs"), Activity.DurationMs);
	Object->SetStringField(TEXT("status"), Activity.Status);
	Object->SetNumberField(TEXT("httpStatus"), Activity.HttpStatus);
	Object->SetStringField(TEXT("errorCode"), Activity.ErrorCode);
	Object->SetStringField(TEXT("risk"), Activity.Risk);
	return Object;
}

TSharedPtr<FJsonObject> FClientActivityService::MakeSnapshot(
	const int32 RecentExecutionLimit,
	const int32 RecentCliLimit) const
{
	const_cast<FClientActivityService*>(this)->ExpireSessions();
	FScopeLock Lock(&Mutex);
	const double NowSeconds = FPlatformTime::Seconds();
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("ue.status-snapshot.v1"));

	TArray<TSharedPtr<FJsonValue>> McpClients;
	for (const TPair<FString, FSession>& Pair : Sessions)
	{
		if (Pair.Value.Registration.ClientKind == TEXT("mcp"))
		{
			McpClients.Add(MakeShared<FJsonValueObject>(
				SessionToJson(Pair.Value, NowSeconds)));
		}
	}
	McpClients.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("name"))
				< Right->AsObject()->GetStringField(TEXT("name"));
		});

	TArray<TSharedPtr<FJsonValue>> CliValues;
	const int32 SafeCliLimit = FMath::Clamp(RecentCliLimit, 0, 100);
	for (const FCliInvocation& Invocation : CliInvocations)
	{
		if (Invocation.Status == TEXT("running"))
		{
			CliValues.Add(MakeShared<FJsonValueObject>(
				InvocationToJson(Invocation, NowSeconds)));
		}
	}
	int32 RecentCount = 0;
	for (int32 Index = CliInvocations.Num() - 1;
		Index >= 0 && RecentCount < SafeCliLimit;
		--Index)
	{
		if (CliInvocations[Index].Status == TEXT("running"))
		{
			continue;
		}
		CliValues.Add(MakeShared<FJsonValueObject>(
			InvocationToJson(CliInvocations[Index], NowSeconds)));
		++RecentCount;
	}
	CliValues.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			const bool bLeftRunning =
				Left->AsObject()->GetStringField(TEXT("status"))
				== TEXT("running");
			const bool bRightRunning =
				Right->AsObject()->GetStringField(TEXT("status"))
				== TEXT("running");
			if (bLeftRunning != bRightRunning)
			{
				return bLeftRunning;
			}
			return Left->AsObject()->GetStringField(TEXT("startedAtUtc"))
				> Right->AsObject()->GetStringField(TEXT("startedAtUtc"));
		});

	TArray<TSharedPtr<FJsonValue>> ActivityValues;
	const int32 SafeExecutionLimit =
		FMath::Clamp(RecentExecutionLimit, 0, ActivityCapacity);
	for (int32 Index = Activities.Num() - 1;
		Index >= 0 && ActivityValues.Num() < SafeExecutionLimit;
		--Index)
	{
		ActivityValues.Add(MakeShared<FJsonValueObject>(
			ActivityToJson(Activities[Index])));
	}

	int32 RunningCli = 0;
	for (const FCliInvocation& Invocation : CliInvocations)
	{
		if (Invocation.Status == TEXT("running"))
		{
			++RunningCli;
		}
	}

	TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
	Stats->SetNumberField(
		TEXT("capabilityCalls"),
		static_cast<double>(Statistics.CapabilityCalls));
	Stats->SetNumberField(
		TEXT("capabilitySucceeded"),
		static_cast<double>(Statistics.CapabilitySucceeded));
	Stats->SetNumberField(
		TEXT("capabilityFailed"),
		static_cast<double>(Statistics.CapabilityFailed));
	Stats->SetNumberField(
		TEXT("workflowApiCalls"),
		static_cast<double>(Statistics.WorkflowApiCalls));
	Stats->SetNumberField(
		TEXT("dslRuns"),
		static_cast<double>(Statistics.DslRuns));
	Stats->SetNumberField(
		TEXT("dslCompleted"),
		static_cast<double>(Statistics.DslCompleted));
	Stats->SetNumberField(
		TEXT("dslFailed"),
		static_cast<double>(Statistics.DslFailed));
	Stats->SetNumberField(
		TEXT("dslBlocked"),
		static_cast<double>(Statistics.DslBlocked));
	Stats->SetNumberField(
		TEXT("rollbacks"),
		static_cast<double>(Statistics.Rollbacks));
	Stats->SetNumberField(
		TEXT("operationTotal"),
		static_cast<double>(Statistics.OperationTotal));
	Stats->SetNumberField(
		TEXT("operationSucceeded"),
		static_cast<double>(Statistics.OperationSucceeded));
	Stats->SetNumberField(
		TEXT("cliInvocations"),
		static_cast<double>(Statistics.CliInvocations));
	Stats->SetNumberField(
		TEXT("capabilityP50Ms"),
		Percentile(Statistics.CapabilityDurationsMs, 0.50));
	Stats->SetNumberField(
		TEXT("capabilityP95Ms"),
		Percentile(Statistics.CapabilityDurationsMs, 0.95));
	Stats->SetNumberField(
		TEXT("dslRunP50Ms"),
		Percentile(Statistics.DslRunDurationsMs, 0.50));
	Stats->SetNumberField(
		TEXT("dslRunP95Ms"),
		Percentile(Statistics.DslRunDurationsMs, 0.95));

	Root->SetNumberField(TEXT("onlineMcpCount"), McpClients.Num());
	Root->SetNumberField(TEXT("runningCliCount"), RunningCli);
	Root->SetArrayField(TEXT("mcpClients"), McpClients);
	Root->SetArrayField(TEXT("cliInvocations"), CliValues);
	Root->SetArrayField(TEXT("recentExecutions"), ActivityValues);
	Root->SetObjectField(TEXT("statistics"), Stats);
	Root->SetNumberField(TEXT("activityRingCount"), Activities.Num());
	Root->SetNumberField(TEXT("activityRingCapacity"), ActivityCapacity);
	return Root;
}

bool FClientActivityService::AcquireLease(
	const FString& Type,
	const FString& SessionId,
	const bool bOverride,
	const FString& ApprovePlanDigest,
	TSharedPtr<FJsonObject>& OutLease,
	FString& OutError,
	int32& OutHttpStatus)
{
	OutLease.Reset();
	OutError.Reset();
	OutHttpStatus = 200;
	static const TSet<FString> Allowed = {
		TEXT("pie"), TEXT("compile"), TEXT("editorRestart"), TEXT("performance") };
	if (!Allowed.Contains(Type) || SessionId.IsEmpty())
	{
		OutError = TEXT("lease type or sessionId is invalid.");
		OutHttpStatus = 422;
		return false;
	}
	ExpireSessions();
	FScopeLock Lock(&Mutex);
	if (!Sessions.Contains(SessionId))
	{
		OutError = TEXT("lease owner session is unknown or expired.");
		OutHttpStatus = 404;
		return false;
	}
	if (const FLease* Existing = Leases.Find(Type))
	{
		if (Existing->OwnerSessionId != SessionId)
		{
			const FString Expected = LeaseOverrideDigest(
				ServerInstanceId,
				Type,
				Existing->LeaseId,
				Existing->OwnerSessionId,
				SessionId);
			if (!bOverride || ApprovePlanDigest != Expected)
			{
				OutLease = MakeShared<FJsonObject>();
				OutLease->SetStringField(TEXT("ownerSessionId"), Existing->OwnerSessionId);
				OutLease->SetStringField(TEXT("leaseId"), Existing->LeaseId);
				OutLease->SetStringField(TEXT("overridePlanDigest"), Expected);
				OutError = TEXT("lease is owned by another live session.");
				OutHttpStatus = 409;
				return false;
			}
			TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
			Audit->SetStringField(TEXT("type"), Type);
			Audit->SetStringField(TEXT("leaseId"), Existing->LeaseId);
			Audit->SetStringField(TEXT("previousOwner"), Existing->OwnerSessionId);
			Audit->SetStringField(TEXT("newOwner"), SessionId);
			Audit->SetStringField(TEXT("atUtc"), NowUtc());
			LeaseAudit.Add(Audit);
			if (LeaseAudit.Num() > 100) LeaseAudit.RemoveAt(0, LeaseAudit.Num() - 100);
		}
	}
	const double NowSeconds = FPlatformTime::Seconds();
	FLease Lease;
	const FLease* Previous = Leases.Find(Type);
	Lease.LeaseId = Previous && Previous->OwnerSessionId == SessionId
		? Previous->LeaseId
		: NewOpaqueId(TEXT("lease"));
	Lease.Type = Type;
	Lease.OwnerSessionId = SessionId;
	Lease.AcquiredAtUtc = NowUtc();
	Lease.ExpiresAtUtc = FDateTime::FromUnixTimestamp(
		FDateTime::UtcNow().ToUnixTimestamp() + 30).ToIso8601();
	Lease.ExpiresSeconds = NowSeconds + 30.0;
	Leases.Add(Type, Lease);
	OutLease = MakeShared<FJsonObject>();
	OutLease->SetStringField(TEXT("leaseId"), Lease.LeaseId);
	OutLease->SetStringField(TEXT("type"), Type);
	OutLease->SetStringField(TEXT("ownerSessionId"), SessionId);
	OutLease->SetNumberField(TEXT("ttlSeconds"), 30);
	OutLease->SetStringField(TEXT("expiresAtUtc"), Lease.ExpiresAtUtc);
	return true;
}

bool FClientActivityService::ReleaseLease(
	const FString& Type,
	const FString& SessionId,
	FString& OutError)
{
	FScopeLock Lock(&Mutex);
	const FLease* Existing = Leases.Find(Type);
	if (!Existing || Existing->OwnerSessionId != SessionId)
	{
		OutError = TEXT("lease is absent or owned by another session.");
		return false;
	}
	Leases.Remove(Type);
	return true;
}

bool FClientActivityService::CanAccessLease(
	const FString& Type,
	const FString& SessionId,
	FString& OutOwnerSessionId)
{
	OutOwnerSessionId.Reset();
	ExpireSessions();
	FScopeLock Lock(&Mutex);
	const FLease* Existing = Leases.Find(Type);
	if (!Existing || Existing->OwnerSessionId == SessionId)
	{
		return true;
	}
	OutOwnerSessionId = Existing->OwnerSessionId;
	return false;
}

TSharedPtr<FJsonObject> FClientActivityService::MakeLeaseSnapshot() const
{
	const_cast<FClientActivityService*>(this)->ExpireSessions();
	FScopeLock Lock(&Mutex);
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.session-leases.v1"));
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TPair<FString, FLease>& Pair : Leases)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("type"), Pair.Value.Type);
		Value->SetStringField(TEXT("leaseId"), Pair.Value.LeaseId);
		Value->SetStringField(TEXT("ownerSessionId"), Pair.Value.OwnerSessionId);
		Value->SetStringField(TEXT("expiresAtUtc"), Pair.Value.ExpiresAtUtc);
		Values.Add(MakeShared<FJsonValueObject>(Value));
	}
	Result->SetArrayField(TEXT("leases"), Values);
	TArray<TSharedPtr<FJsonValue>> AuditValues;
	for (const TSharedPtr<FJsonObject>& Entry : LeaseAudit)
	{
		AuditValues.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Result->SetArrayField(TEXT("overrideAudit"), AuditValues);
	return Result;
}

int32 FClientActivityService::GetOnlineMcpCount() const
{
	const_cast<FClientActivityService*>(this)->ExpireSessions();
	FScopeLock Lock(&Mutex);
	int32 Count = 0;
	for (const TPair<FString, FSession>& Pair : Sessions)
	{
		if (Pair.Value.Registration.ClientKind == TEXT("mcp"))
		{
			++Count;
		}
	}
	return Count;
}

int32 FClientActivityService::GetRunningCliCount() const
{
	const_cast<FClientActivityService*>(this)->ExpireSessions();
	FScopeLock Lock(&Mutex);
	int32 Count = 0;
	for (const FCliInvocation& Invocation : CliInvocations)
	{
		if (Invocation.Status == TEXT("running"))
		{
			++Count;
		}
	}
	return Count;
}

FString FClientActivityService::GetLastExecutionResult() const
{
	const_cast<FClientActivityService*>(this)->ExpireSessions();
	FScopeLock Lock(&Mutex);
	for (int32 Index = Activities.Num() - 1; Index >= 0; --Index)
	{
		const FActivity& Activity = Activities[Index];
		if (!Activity.FinishedAtUtc.IsEmpty())
		{
			const FString Subject = Activity.Kind == TEXT("capability")
				? Activity.Capability
				: Activity.WorkflowAction;
			return Subject.IsEmpty()
				? Activity.Status
				: FString::Printf(
					TEXT("%s: %s"),
					*Subject,
					*Activity.Status);
		}
	}
	return TEXT("None");
}

void FClientActivityService::PruneActivitiesLocked()
{
	while (Activities.Num() > ActivityCapacity)
	{
		const int32 RemovableIndex = Activities.IndexOfByPredicate(
			[](const FActivity& Activity)
			{
				return !Activity.FinishedAtUtc.IsEmpty();
			});
		if (RemovableIndex == INDEX_NONE)
		{
			// Never lose an in-flight request merely to satisfy the history
			// bound. Completion immediately brings the ring back to capacity.
			break;
		}
		Activities.RemoveAt(RemovableIndex, 1, false);
	}
}

void FClientActivityService::PruneCliInvocationsLocked()
{
	int32 TerminalCount = 0;
	for (const FCliInvocation& Invocation : CliInvocations)
	{
		if (Invocation.Status != TEXT("running"))
		{
			++TerminalCount;
		}
	}
	while (TerminalCount > MaxRecentCliInvocations)
	{
		const int32 RemovableIndex = CliInvocations.IndexOfByPredicate(
			[](const FCliInvocation& Invocation)
			{
				return Invocation.Status != TEXT("running");
			});
		if (RemovableIndex == INDEX_NONE)
		{
			break;
		}
		CliInvocations.RemoveAt(RemovableIndex, 1, false);
		--TerminalCount;
	}
}

void FClientActivityService::PruneRunStatisticsLocked()
{
	while (Statistics.Runs.Num() > MaxTrackedRuns)
	{
		FString OldestTerminalRunId;
		uint64 OldestOrdinal = MAX_uint64;
		for (const TPair<FString, FRunStatistics>& Pair : Statistics.Runs)
		{
			if (IsTerminalRunStatus(Pair.Value.Status)
				&& Pair.Value.LastTouchedOrdinal < OldestOrdinal)
			{
				OldestTerminalRunId = Pair.Key;
				OldestOrdinal = Pair.Value.LastTouchedOrdinal;
			}
		}
		if (OldestTerminalRunId.IsEmpty())
		{
			break;
		}
		Statistics.Runs.Remove(OldestTerminalRunId);
	}
}

void FClientActivityService::AddDurationSample(
	TArray<double>& Samples,
	const double DurationMs)
{
	Samples.Add(FMath::Max(0.0, DurationMs));
	if (Samples.Num() > MaxDurationSamples)
	{
		Samples.RemoveAt(
			0,
			Samples.Num() - MaxDurationSamples,
			false);
	}
}

FClientActivityService::FActivity*
FClientActivityService::FindActivityLocked(const FString& EventId)
{
	return Activities.FindByPredicate(
		[&EventId](const FActivity& Activity)
		{
			return Activity.EventId == EventId;
		});
}
}
