#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"

namespace UEAISceneEngineeringCommandPrivate
{
using UEAIIntegration::Infrastructure::TryDigestJson;
using UEAIIntegration::Infrastructure::ValidateChangeApproval;

struct FWorldChangeRecord
{
	FString RunId;
	FString RequestId;
	FString RequestDigest;
	FString PlanDigest;
	FString Action;
	FString WorldPath;
	FString DataLayer;
	bool bBeforeStreamingEnabled = false;
	bool bAfterStreamingEnabled = false;
	EDataLayerRuntimeState BeforeDataLayerState = EDataLayerRuntimeState::Unloaded;
	EDataLayerRuntimeState AfterDataLayerState = EDataLayerRuntimeState::Unloaded;
	bool bRolledBack = false;
};

struct FRenderChangeRecord
{
	FString RunId;
	FString RequestId;
	FString RequestDigest;
	FString PlanDigest;
	TMap<FString, FString> BeforeValues;
	TMap<FString, FString> AfterValues;
	bool bRolledBack = false;
};

struct FPCGOperationRecord
{
	FString RequestDigest;
	TSharedPtr<FJsonObject> Result;
};

TMap<FString, FWorldChangeRecord>& WorldRuns()
{
	static TMap<FString, FWorldChangeRecord> Runs;
	return Runs;
}

TMap<FString, FString>& WorldRequestRuns()
{
	static TMap<FString, FString> Requests;
	return Requests;
}

TMap<FString, FRenderChangeRecord>& RenderRuns()
{
	static TMap<FString, FRenderChangeRecord> Runs;
	return Runs;
}

TMap<FString, FString>& RenderRequestRuns()
{
	static TMap<FString, FString> Requests;
	return Requests;
}

TMap<FString, FPCGOperationRecord>& PCGRequestRuns()
{
	static TMap<FString, FPCGOperationRecord> Requests;
	return Requests;
}

bool TryExecutionRequestDigest(
	const TSharedPtr<FJsonObject>& Params,
	FString& OutDigest)
{
	if (!Params.IsValid())
	{
		return false;
	}
	TSharedPtr<FJsonObject> Intent =
		MakeShared<FJsonObject>(*Params);
	Intent->RemoveField(TEXT("requestId"));
	Intent->RemoveField(TEXT("approvePlanDigest"));
	Intent->RemoveField(TEXT("confirmWrite"));
	return TryDigestJson(Intent, OutDigest);
}

UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString DataLayerStateName(const EDataLayerRuntimeState State)
{
	switch (State)
	{
	case EDataLayerRuntimeState::Unloaded: return TEXT("unloaded");
	case EDataLayerRuntimeState::Loaded: return TEXT("loaded");
	case EDataLayerRuntimeState::Activated: return TEXT("activated");
	default: return TEXT("unknown");
	}
}

bool ParseDataLayerState(
	const FString& Value,
	EDataLayerRuntimeState& OutState)
{
	if (Value.Equals(TEXT("unloaded"), ESearchCase::IgnoreCase))
	{
		OutState = EDataLayerRuntimeState::Unloaded;
		return true;
	}
	if (Value.Equals(TEXT("loaded"), ESearchCase::IgnoreCase))
	{
		OutState = EDataLayerRuntimeState::Loaded;
		return true;
	}
	if (Value.Equals(TEXT("activated"), ESearchCase::IgnoreCase))
	{
		OutState = EDataLayerRuntimeState::Activated;
		return true;
	}
	return false;
}

const UDataLayerInstance* FindDataLayer(
	const UDataLayerManager* Manager,
	const FString& Identifier)
{
	if (!Manager)
	{
		return nullptr;
	}
	const UDataLayerInstance* Match = nullptr;
	Manager->ForEachDataLayerInstance(
		[&](UDataLayerInstance* Instance)
		{
			if (Instance
				&& (Instance->GetDataLayerShortName().Equals(
						Identifier,
						ESearchCase::IgnoreCase)
					|| Instance->GetDataLayerFullName().Equals(
						Identifier,
						ESearchCase::IgnoreCase)
					|| Instance->GetDataLayerFName().ToString().Equals(
						Identifier,
						ESearchCase::IgnoreCase)
					|| Instance->GetPathName().Equals(
						Identifier,
						ESearchCase::IgnoreCase)))
			{
				Match = Instance;
				return false;
			}
			return true;
		});
	return Match;
}

FMCPToolResult PlanError(
	const FString& Message,
	const FString& Code = TEXT("invalid_change"))
{
	return FMCPToolResult::Error(Message, Code, 400);
}

FMCPToolResult BuildWorldPlan(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutPlan)
{
	if (!Params.IsValid())
	{
		return PlanError(TEXT("A change request is required."));
	}
	FString Action;
	if (!Params->TryGetStringField(TEXT("action"), Action))
	{
		return PlanError(TEXT("action is required."));
	}

	UWorld* World = GetEditorWorld();
	UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
	if (!WorldPartition)
	{
		return FMCPToolResult::Error(
			TEXT("The current world is not partitioned."),
			TEXT("world_partition_unavailable"),
			409);
	}

	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("scene.world_partition"));
	Plan->SetStringField(TEXT("action"), Action);
	Plan->SetStringField(TEXT("world"), World->GetPathName());
	Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
	Plan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	Plan->SetStringField(
		TEXT("rollbackBoundary"),
		TEXT("sameEditorInstance"));

	if (Action == TEXT("setStreamingEnabled"))
	{
		bool bEnabled = false;
		if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			return PlanError(
				TEXT("enabled is required for setStreamingEnabled."));
		}
		Plan->SetBoolField(
			TEXT("beforeStreamingEnabled"),
			WorldPartition->IsStreamingEnabled());
		Plan->SetBoolField(TEXT("afterStreamingEnabled"), bEnabled);
		Plan->SetBoolField(
			TEXT("changesState"),
			WorldPartition->IsStreamingEnabled() != bEnabled);
	}
	else if (Action == TEXT("setDataLayerRuntimeState"))
	{
		FString DataLayer;
		FString StateText;
		if (!Params->TryGetStringField(TEXT("dataLayer"), DataLayer)
			|| !Params->TryGetStringField(TEXT("state"), StateText))
		{
			return PlanError(
				TEXT("dataLayer and state are required for setDataLayerRuntimeState."));
		}
		EDataLayerRuntimeState State;
		if (!ParseDataLayerState(StateText, State))
		{
			return PlanError(
				TEXT("state must be unloaded, loaded, or activated."));
		}
		UDataLayerManager* Manager = World->GetDataLayerManager();
		const UDataLayerInstance* Instance = FindDataLayer(Manager, DataLayer);
		if (!Instance)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Data Layer '%s' was not found."), *DataLayer),
				TEXT("data_layer_not_found"),
				404);
		}
		if (!Instance->IsRuntime())
		{
			return FMCPToolResult::Error(
				TEXT("Only runtime Data Layers have a session runtime state."),
				TEXT("data_layer_not_runtime"),
				409);
		}
		const EDataLayerRuntimeState Before =
			Manager->GetDataLayerInstanceRuntimeState(Instance);
		Plan->SetStringField(TEXT("dataLayer"), Instance->GetDataLayerFullName());
		Plan->SetStringField(TEXT("dataLayerPath"), Instance->GetPathName());
		Plan->SetStringField(TEXT("beforeState"), DataLayerStateName(Before));
		Plan->SetStringField(TEXT("afterState"), DataLayerStateName(State));
		Plan->SetBoolField(TEXT("changesState"), Before != State);
		Plan->SetStringField(TEXT("persistence"), TEXT("sessionOnly"));
	}
	else
	{
		return PlanError(
			TEXT("action must be setStreamingEnabled or setDataLayerRuntimeState."));
	}

	FString Digest;
	if (!TryDigestJson(Plan, Digest))
	{
		return FMCPToolResult::Error(
			TEXT("Unable to compute the plan digest."),
			TEXT("digest_unavailable"),
			500);
	}
	Plan->SetStringField(TEXT("planDigest"), Digest);
	OutPlan = Plan;
	return FMCPToolResult::Ok(Plan);
}

TMap<FString, FString> AllowedRenderSettings()
{
	return {
		{TEXT("lumen"), TEXT("r.Lumen.DiffuseIndirect.Allow")},
		{TEXT("virtualShadowMaps"), TEXT("r.Shadow.Virtual.Enable")},
		{TEXT("hardwareRayTracing"), TEXT("r.RayTracing")},
		{TEXT("tsrUpsampling"), TEXT("r.TemporalAA.Upsampling")}
	};
}

FMCPToolResult BuildRenderPlan(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutPlan)
{
	FString Scope = TEXT("session");
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("scope"), Scope);
	}
	if (Scope != TEXT("session"))
	{
		return PlanError(
			TEXT("v1 render settings only support scope='session'; level/project writes are not yet admitted."),
			TEXT("unsupported_scope"));
	}
	const TSharedPtr<FJsonObject>* Settings = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetObjectField(TEXT("settings"), Settings)
		|| !Settings
		|| !Settings->IsValid()
		|| (*Settings)->Values.IsEmpty())
	{
		return PlanError(TEXT("A non-empty settings object is required."));
	}

	const TMap<FString, FString> Allowed = AllowedRenderSettings();
	TArray<FString> Keys;
	(*Settings)->Values.GetKeys(Keys);
	Keys.Sort();
	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
	for (const FString& Key : Keys)
	{
		const FString* CVarName = Allowed.Find(Key);
		if (!CVarName)
		{
			return PlanError(
				FString::Printf(TEXT("Render setting '%s' is not admitted."), *Key),
				TEXT("setting_not_admitted"));
		}
		IConsoleVariable* CVar =
			IConsoleManager::Get().FindConsoleVariable(**CVarName);
		if (!CVar)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Render setting '%s' is unavailable in this Editor."),
					*Key),
				TEXT("setting_unavailable"),
				409);
		}
		bool bDesired = false;
		if (!(*Settings)->TryGetBoolField(Key, bDesired))
		{
			return PlanError(
				FString::Printf(TEXT("Render setting '%s' must be boolean."), *Key));
		}
		Before->SetStringField(Key, CVar->GetString());
		After->SetNumberField(Key, bDesired ? 1 : 0);
	}

	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("scene.render"));
	Plan->SetStringField(TEXT("scope"), TEXT("session"));
	Plan->SetStringField(TEXT("persistence"), TEXT("sessionOnly"));
	Plan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	Plan->SetObjectField(TEXT("before"), Before);
	Plan->SetObjectField(TEXT("after"), After);
	Plan->SetBoolField(TEXT("restartRequired"), false);

	FString Digest;
	if (!TryDigestJson(Plan, Digest))
	{
		return FMCPToolResult::Error(
			TEXT("Unable to compute the plan digest."),
			TEXT("digest_unavailable"),
			500);
	}
	Plan->SetStringField(TEXT("planDigest"), Digest);
	OutPlan = Plan;
	return FMCPToolResult::Ok(Plan);
}

class FTool_WorldChangePlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.change.plan"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Plan;
		return BuildWorldPlan(Params, Plan);
	}
};

class FTool_WorldChangeExecute final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.change.execute"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString RequestId = Params->GetStringField(TEXT("requestId"));
		FString RequestDigest;
		if (!TryExecutionRequestDigest(Params, RequestDigest))
		{
			return FMCPToolResult::Error(
				TEXT("Unable to compute the execution request digest."),
				TEXT("digest_unavailable"),
				500);
		}
		if (const FString* ExistingRunId = WorldRequestRuns().Find(RequestId))
		{
			const FWorldChangeRecord* Existing = WorldRuns().Find(*ExistingRunId);
			if (Existing
				&& Existing->RequestDigest != RequestDigest)
			{
				return FMCPToolResult::Error(
					TEXT("requestId was already used with a different world change request."),
					TEXT("request_id_conflict"),
					409);
			}
			FString ErrorCode;
			FString ErrorMessage;
			if (!Existing
				|| !ValidateChangeApproval(
					Params,
					Existing->PlanDigest,
					ErrorCode,
					ErrorMessage))
			{
				return FMCPToolResult::Error(
					Existing
						? ErrorMessage
						: TEXT("requestId was already used for an unavailable run."),
					Existing
						? ErrorCode
						: TEXT("request_id_conflict"),
					409);
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("runId"), Existing->RunId);
			Result->SetStringField(TEXT("planDigest"), Existing->PlanDigest);
			Result->SetStringField(TEXT("status"), TEXT("succeeded"));
			Result->SetBoolField(TEXT("idempotentReplay"), true);
			Result->SetBoolField(TEXT("rolledBack"), Existing->bRolledBack);
			return FMCPToolResult::Ok(Result);
		}

		TSharedPtr<FJsonObject> Plan;
		const FMCPToolResult PlanResult = BuildWorldPlan(Params, Plan);
		if (!PlanResult.bSuccess)
		{
			return PlanResult;
		}
		const FString Digest = Plan->GetStringField(TEXT("planDigest"));
		FString ErrorCode;
		FString ErrorMessage;
		if (!ValidateChangeApproval(
				Params,
				Digest,
				ErrorCode,
				ErrorMessage))
		{
			return FMCPToolResult::Error(ErrorMessage, ErrorCode, 409);
		}

		UWorld* World = GetEditorWorld();
		UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
		if (!WorldPartition)
		{
			return FMCPToolResult::Error(
				TEXT("The current world is no longer partitioned."),
				TEXT("world_partition_unavailable"),
				409);
		}

		FWorldChangeRecord Record;
		Record.RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Record.RequestId = RequestId;
		Record.RequestDigest = RequestDigest;
		Record.PlanDigest = Digest;
		Record.Action = Plan->GetStringField(TEXT("action"));
		Record.WorldPath = World->GetPathName();

		if (Record.Action == TEXT("setStreamingEnabled"))
		{
			Record.bBeforeStreamingEnabled =
				Plan->GetBoolField(TEXT("beforeStreamingEnabled"));
			Record.bAfterStreamingEnabled =
				Plan->GetBoolField(TEXT("afterStreamingEnabled"));
			WorldPartition->SetEnableStreaming(Record.bAfterStreamingEnabled);
		}
		else
		{
			Record.DataLayer = Plan->GetStringField(TEXT("dataLayerPath"));
			ParseDataLayerState(
				Plan->GetStringField(TEXT("beforeState")),
				Record.BeforeDataLayerState);
			ParseDataLayerState(
				Plan->GetStringField(TEXT("afterState")),
				Record.AfterDataLayerState);
			UDataLayerManager* Manager = World->GetDataLayerManager();
			const UDataLayerInstance* Instance =
				FindDataLayer(Manager, Record.DataLayer);
			if (!Instance
				|| !Manager->SetDataLayerInstanceRuntimeState(
					Instance,
					Record.AfterDataLayerState))
			{
				return FMCPToolResult::Error(
					TEXT("The Data Layer runtime state could not be changed."),
					TEXT("data_layer_change_failed"),
					409);
			}
		}

		WorldRuns().Add(Record.RunId, Record);
		WorldRequestRuns().Add(RequestId, Record.RunId);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("runId"), Record.RunId);
		Result->SetStringField(TEXT("planDigest"), Digest);
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetBoolField(TEXT("rolledBack"), false);
		Result->SetObjectField(TEXT("readBack"), Plan);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_WorldChangeRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.change.rollback"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RunId;
		bool bConfirmWrite = false;
		if (!Params->TryGetStringField(TEXT("runId"), RunId)
			|| RunId.IsEmpty()
			|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
			|| !bConfirmWrite)
		{
			return FMCPToolResult::Error(
				TEXT("runId and confirmWrite=true are required."),
				TEXT("rollback_confirmation_required"),
				400);
		}
		FWorldChangeRecord* Record = WorldRuns().Find(RunId);
		if (!Record)
		{
			return FMCPToolResult::Error(
				TEXT("The run is unknown in this Editor instance."),
				TEXT("run_not_found"),
				404);
		}
		if (Record->bRolledBack)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("runId"), RunId);
			Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
			Result->SetBoolField(TEXT("idempotentReplay"), true);
			return FMCPToolResult::Ok(Result);
		}
		UWorld* World = GetEditorWorld();
		if (!World || World->GetPathName() != Record->WorldPath
			|| !World->GetWorldPartition())
		{
			return FMCPToolResult::Error(
				TEXT("Rollback requires the original world in the same Editor instance."),
				TEXT("rollback_boundary_exceeded"),
				409);
		}
		if (Record->Action == TEXT("setStreamingEnabled"))
		{
			World->GetWorldPartition()->SetEnableStreaming(
				Record->bBeforeStreamingEnabled);
		}
		else
		{
			UDataLayerManager* Manager = World->GetDataLayerManager();
			const UDataLayerInstance* Instance =
				FindDataLayer(Manager, Record->DataLayer);
			if (!Instance
				|| !Manager->SetDataLayerInstanceRuntimeState(
					Instance,
					Record->BeforeDataLayerState))
			{
				return FMCPToolResult::Error(
					TEXT("The Data Layer can no longer be restored."),
					TEXT("rollback_failed"),
					409);
			}
		}
		Record->bRolledBack = true;
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("runId"), RunId);
		Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
		Result->SetBoolField(TEXT("idempotentReplay"), false);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_RenderSettingsPlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.render.settings.plan"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Plan;
		return BuildRenderPlan(Params, Plan);
	}
};

class FTool_RenderSettingsExecute final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.render.settings.execute"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		const FString RequestId = Params->GetStringField(TEXT("requestId"));
		FString RequestDigest;
		if (!TryExecutionRequestDigest(Params, RequestDigest))
		{
			return FMCPToolResult::Error(
				TEXT("Unable to compute the execution request digest."),
				TEXT("digest_unavailable"),
				500);
		}
		if (const FString* ExistingRunId = RenderRequestRuns().Find(RequestId))
		{
			const FRenderChangeRecord* Existing = RenderRuns().Find(*ExistingRunId);
			if (Existing
				&& Existing->RequestDigest != RequestDigest)
			{
				return FMCPToolResult::Error(
					TEXT("requestId was already used with different render settings."),
					TEXT("request_id_conflict"),
					409);
			}
			FString ErrorCode;
			FString ErrorMessage;
			if (!Existing
				|| !ValidateChangeApproval(
					Params,
					Existing->PlanDigest,
					ErrorCode,
					ErrorMessage))
			{
				return FMCPToolResult::Error(
					Existing
						? ErrorMessage
						: TEXT("requestId was already used for an unavailable run."),
					Existing
						? ErrorCode
						: TEXT("request_id_conflict"),
					409);
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("runId"), Existing->RunId);
			Result->SetStringField(TEXT("status"), TEXT("succeeded"));
			Result->SetStringField(TEXT("planDigest"), Existing->PlanDigest);
			Result->SetBoolField(TEXT("idempotentReplay"), true);
			Result->SetBoolField(TEXT("rolledBack"), Existing->bRolledBack);
			return FMCPToolResult::Ok(Result);
		}

		TSharedPtr<FJsonObject> Plan;
		const FMCPToolResult PlanResult = BuildRenderPlan(Params, Plan);
		if (!PlanResult.bSuccess)
		{
			return PlanResult;
		}
		const FString Digest = Plan->GetStringField(TEXT("planDigest"));
		FString ErrorCode;
		FString ErrorMessage;
		if (!ValidateChangeApproval(
				Params,
				Digest,
				ErrorCode,
				ErrorMessage))
		{
			return FMCPToolResult::Error(ErrorMessage, ErrorCode, 409);
		}
		FRenderChangeRecord Record;
		Record.RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Record.RequestId = RequestId;
		Record.RequestDigest = RequestDigest;
		Record.PlanDigest = Digest;
		const TSharedPtr<FJsonObject> Before =
			Plan->GetObjectField(TEXT("before"));
		const TSharedPtr<FJsonObject> After =
			Plan->GetObjectField(TEXT("after"));
		const TMap<FString, FString> Allowed = AllowedRenderSettings();
		TArray<FString> Keys;
		After->Values.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			IConsoleVariable* CVar =
				IConsoleManager::Get().FindConsoleVariable(*Allowed.FindChecked(Key));
			const FString BeforeValue = Before->GetStringField(Key);
			const FString AfterValue =
				FString::FromInt(static_cast<int32>(After->GetNumberField(Key)));
			Record.BeforeValues.Add(Key, BeforeValue);
			Record.AfterValues.Add(Key, AfterValue);
			CVar->Set(*AfterValue, ECVF_SetByConsole);
			if (CVar->GetString() != AfterValue)
			{
				for (const TPair<FString, FString>& Applied : Record.BeforeValues)
				{
					IConsoleManager::Get()
						.FindConsoleVariable(*Allowed.FindChecked(Applied.Key))
						->Set(*Applied.Value, ECVF_SetByConsole);
				}
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Render setting '%s' rejected the requested value."),
						*Key),
					TEXT("setting_change_failed"),
					409);
			}
		}
		RenderRuns().Add(Record.RunId, Record);
		RenderRequestRuns().Add(RequestId, Record.RunId);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("runId"), Record.RunId);
		Result->SetStringField(TEXT("planDigest"), Digest);
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetObjectField(TEXT("readBack"), After);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_RenderSettingsRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.render.settings.rollback"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RunId;
		bool bConfirmWrite = false;
		if (!Params->TryGetStringField(TEXT("runId"), RunId)
			|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
			|| !bConfirmWrite)
		{
			return FMCPToolResult::Error(
				TEXT("runId and confirmWrite=true are required."),
				TEXT("rollback_confirmation_required"),
				400);
		}
		FRenderChangeRecord* Record = RenderRuns().Find(RunId);
		if (!Record)
		{
			return FMCPToolResult::Error(
				TEXT("The run is unknown in this Editor instance."),
				TEXT("run_not_found"),
				404);
		}
		if (!Record->bRolledBack)
		{
			const TMap<FString, FString> Allowed = AllowedRenderSettings();
			for (const TPair<FString, FString>& Pair : Record->BeforeValues)
			{
				if (IConsoleVariable* CVar =
					IConsoleManager::Get().FindConsoleVariable(
						*Allowed.FindChecked(Pair.Key)))
				{
					CVar->Set(*Pair.Value, ECVF_SetByConsole);
				}
			}
			Record->bRolledBack = true;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("runId"), RunId);
		Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
		return FMCPToolResult::Ok(Result);
	}
};

bool ReadConfirmedJobRequest(
	const TSharedPtr<FJsonObject>& Params,
	FString& OutRequestId,
	FMCPToolResult& OutError)
{
	bool bConfirmWrite = false;
	if (!Params.IsValid()
		|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
		|| !bConfirmWrite)
	{
		OutError = FMCPToolResult::Error(
			TEXT("confirmWrite=true is required."),
			TEXT("write_confirmation_required"),
			422);
		return false;
	}
	if (!Params->TryGetStringField(TEXT("requestId"), OutRequestId)
		|| OutRequestId.IsEmpty())
	{
		OutError = FMCPToolResult::Error(
			TEXT("A requestId in the execution envelope is required."),
			TEXT("request_id_required"),
			422);
		return false;
	}
	return true;
}

bool MatchesActorFilter(const AActor* Actor, const FString& Filter)
{
	return Filter.IsEmpty()
		|| (Actor
			&& (Actor->GetName().Equals(Filter, ESearchCase::IgnoreCase)
				|| Actor->GetActorLabel().Equals(
					Filter,
					ESearchCase::IgnoreCase)
				|| Actor->GetPathName().Equals(
					Filter,
					ESearchCase::IgnoreCase)));
}

bool InvokePCGFunction(
	UActorComponent* Component,
	const FName FunctionName,
	const bool bFirstArgument,
	const bool bSecondArgument)
{
	UFunction* Function =
		Component ? Component->FindFunction(FunctionName) : nullptr;
	if (!Function)
	{
		return false;
	}

	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(Function->ParmsSize);
	if (FunctionName == TEXT("GenerateLocal"))
	{
		FBoolProperty* ForceProperty =
			FindFProperty<FBoolProperty>(Function, TEXT("bForce"));
		if (!ForceProperty)
		{
			return false;
		}
		ForceProperty->SetPropertyValue_InContainer(
			Buffer.GetData(),
			bFirstArgument);
	}
	else
	{
		FBoolProperty* RemoveComponentsProperty =
			FindFProperty<FBoolProperty>(
				Function,
				TEXT("bRemoveComponents"));
		FBoolProperty* SaveProperty =
			FindFProperty<FBoolProperty>(Function, TEXT("bSave"));
		if (!RemoveComponentsProperty || !SaveProperty)
		{
			return false;
		}
		RemoveComponentsProperty->SetPropertyValue_InContainer(
			Buffer.GetData(),
			bFirstArgument);
		SaveProperty->SetPropertyValue_InContainer(
			Buffer.GetData(),
			bSecondArgument);
	}
	Component->ProcessEvent(Function, Buffer.GetData());
	return true;
}

FMCPToolResult ExecutePCGOperation(
	const TSharedPtr<FJsonObject>& Params,
	const bool bGenerate)
{
	FString RequestId;
	FMCPToolResult ConfirmationError;
	if (!ReadConfirmedJobRequest(Params, RequestId, ConfirmationError))
	{
		return ConfirmationError;
	}

	TSharedPtr<FJsonObject> DigestInput =
		MakeShared<FJsonObject>(*Params);
	DigestInput->SetStringField(
		TEXT("operation"),
		bGenerate ? TEXT("generate") : TEXT("cleanup"));
	FString RequestDigest;
	if (!TryDigestJson(DigestInput, RequestDigest))
	{
		return FMCPToolResult::Error(
			TEXT("The PCG request digest could not be generated."),
			TEXT("digest_unavailable"),
			500);
	}
	if (const FPCGOperationRecord* Existing =
		PCGRequestRuns().Find(RequestId))
	{
		if (Existing->RequestDigest != RequestDigest
			|| !Existing->Result.IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("requestId was already used for a different PCG operation."),
				TEXT("request_id_conflict"),
				409);
		}
		TSharedRef<FJsonObject> Replay =
			MakeShared<FJsonObject>(*Existing->Result);
		Replay->SetBoolField(TEXT("idempotentReplay"), true);
		return FMCPToolResult::Ok(Replay);
	}

	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return FMCPToolResult::Error(
			TEXT("No editor world is loaded."),
			TEXT("world_unavailable"),
			409);
	}
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("PCG"))
		&& FModuleManager::Get().ModuleExists(TEXT("PCG")))
	{
		EModuleLoadResult FailureReason =
			EModuleLoadResult::Success;
		FModuleManager::Get().LoadModuleWithFailureReason(
			TEXT("PCG"),
			FailureReason);
	}
	UClass* ComponentClass =
		FindFirstObject<UClass>(
			TEXT("PCGComponent"),
			EFindFirstObjectOptions::None);
	if (!ComponentClass)
	{
		return FMCPToolResult::Error(
			TEXT("The optional PCG plugin/module is not loaded."),
			TEXT("pcg_unavailable"),
			503);
	}

	FString ActorFilter;
	Params->TryGetStringField(TEXT("actor"), ActorFilter);
	double MaxComponentsValue = 100.0;
	Params->TryGetNumberField(TEXT("maxComponents"), MaxComponentsValue);
	const int32 MaxComponents =
		FMath::Clamp(static_cast<int32>(MaxComponentsValue), 1, 500);

	TArray<UActorComponent*> Components;
	for (TObjectIterator<UActorComponent> It; It; ++It)
	{
		UActorComponent* Component = *It;
		if (!Component
			|| Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
			|| Component->GetWorld() != World
			|| !Component->IsA(ComponentClass)
			|| !MatchesActorFilter(Component->GetOwner(), ActorFilter))
		{
			continue;
		}
		Components.Add(Component);
	}
	Components.Sort(
		[](const UActorComponent& A, const UActorComponent& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
	if (Components.Num() > MaxComponents)
	{
		return FMCPToolResult::Error(
			FString::Printf(
				TEXT(
					"%d PCG components matched, exceeding maxComponents=%d; "
					"provide an exact actor filter or raise the bounded limit."),
				Components.Num(),
				MaxComponents),
			TEXT("pcg_component_limit_exceeded"),
			409);
	}

	bool bForce = true;
	bool bRemoveComponents = true;
	if (bGenerate)
	{
		Params->TryGetBoolField(TEXT("force"), bForce);
	}
	else
	{
		Params->TryGetBoolField(
			TEXT("removeComponents"),
			bRemoveComponents);
	}

	TArray<TSharedPtr<FJsonValue>> Scheduled;
	for (UActorComponent* Component : Components)
	{
		if (!InvokePCGFunction(
				Component,
				bGenerate ? TEXT("GenerateLocal") : TEXT("CleanupLocal"),
				bGenerate ? bForce : bRemoveComponents,
				false))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("PCG reflection contract is unavailable on '%s'."),
					*Component->GetPathName()),
				TEXT("pcg_reflection_contract_unavailable"),
				503);
		}
		Scheduled.Add(
			MakeShared<FJsonValueString>(Component->GetPathName()));
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("operation"),
		bGenerate ? TEXT("generate") : TEXT("cleanup"));
	Result->SetStringField(TEXT("executionMode"), TEXT("asyncEditorTask"));
	Result->SetBoolField(TEXT("workflowAdmitted"), false);
	Result->SetBoolField(TEXT("accepted"), true);
	Result->SetStringField(
		TEXT("status"),
		Scheduled.IsEmpty()
			? TEXT("noMatchingComponents")
			: TEXT("scheduled"));
	Result->SetStringField(TEXT("requestId"), RequestId);
	Result->SetStringField(TEXT("actorFilter"), ActorFilter);
	Result->SetNumberField(TEXT("scheduledCount"), Scheduled.Num());
	Result->SetArrayField(TEXT("scheduledComponents"), Scheduled);
	Result->SetStringField(
		TEXT("completionBoundary"),
		TEXT(
			"GenerateLocal/CleanupLocal schedule PCG work; this response does "
			"not claim graph completion. Re-inspect the component or use PCG "
			"runtime diagnostics for completion evidence."));
	FPCGOperationRecord Record;
	Record.RequestDigest = RequestDigest;
	Record.Result = MakeShared<FJsonObject>(*Result);
	PCGRequestRuns().Add(RequestId, MoveTemp(Record));
	return FMCPToolResult::Ok(Result);
}

class FTool_HLODBuild final : public FMCPToolBase
{
public:
	explicit FTool_HLODBuild(FMCPToolRegistry& InRegistry)
		: Registry(InRegistry)
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.hlod.build"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RequestId;
		FMCPToolResult ConfirmationError;
		if (!ReadConfirmedJobRequest(
				Params,
				RequestId,
				ConfirmationError))
		{
			return ConfirmationError;
		}
		UWorld* World = GetEditorWorld();
		if (!World || !World->IsPartitionedWorld())
		{
			return FMCPToolResult::Error(
				TEXT("The current editor world is not partitioned."),
				TEXT("world_partition_unavailable"),
				409);
		}

		FString Mode;
		if (!Params->TryGetStringField(TEXT("mode"), Mode))
		{
			return FMCPToolResult::Error(
				TEXT("mode is required."),
				TEXT("invalid_params"),
				422);
		}
		FString ModeArgument;
		if (Mode == TEXT("setup"))
		{
			ModeArgument = TEXT("-SetupHLODs");
		}
		else if (Mode == TEXT("build"))
		{
			ModeArgument = TEXT("-BuildHLODs");
		}
		else if (Mode == TEXT("delete"))
		{
			ModeArgument = TEXT("-DeleteHLODs");
		}
		else if (Mode == TEXT("stats"))
		{
			ModeArgument = TEXT("-DumpStats");
		}
		else
		{
			return FMCPToolResult::Error(
				TEXT("mode must be setup, build, delete, or stats."),
				TEXT("invalid_params"),
				422);
		}

		const FString MapPackage = World->GetPackage()->GetName();
		if (!MapPackage.StartsWith(TEXT("/Game/")))
		{
			return FMCPToolResult::Error(
				TEXT("HLOD jobs require a saved /Game map."),
				TEXT("map_not_saved"),
				409);
		}
		TArray<TSharedPtr<FJsonValue>> Arguments = {
			MakeShared<FJsonValueString>(MapPackage),
			MakeShared<FJsonValueString>(
				TEXT("-Builder=WorldPartitionHLODsBuilder")),
			MakeShared<FJsonValueString>(ModeArgument)
		};
		if (Mode == TEXT("build"))
		{
			Arguments.Add(
				MakeShared<FJsonValueString>(
					TEXT("-AllowCommandletRendering")));
		}

		TSharedRef<FJsonObject> DelegateParams = MakeShared<FJsonObject>();
		DelegateParams->SetStringField(
			TEXT("commandletName"),
			TEXT("WorldPartitionBuilderCommandlet"));
		DelegateParams->SetArrayField(TEXT("arguments"), Arguments);
		DelegateParams->SetStringField(TEXT("requestId"), RequestId);
		DelegateParams->SetBoolField(TEXT("confirmWrite"), true);
		double TimeoutSeconds = 3600.0;
		Params->TryGetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);
		DelegateParams->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);

		FMCPToolResult Result = Registry.ExecuteTool(
			TEXT("production.commandlet.run"),
			DelegateParams);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			Result.Data->SetStringField(
				TEXT("delegatedCapability"),
				TEXT("production.commandlet.run"));
			Result.Data->SetStringField(TEXT("hlodMode"), Mode);
			Result.Data->SetStringField(TEXT("map"), MapPackage);
			Result.Data->SetBoolField(TEXT("workflowAdmitted"), false);
		}
		return Result;
	}

private:
	FMCPToolRegistry& Registry;
};

class FTool_PCGGenerate final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.pcg.generate"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ExecutePCGOperation(Params, true);
	}
};

class FTool_PCGCleanup final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.pcg.cleanup"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ExecutePCGOperation(Params, false);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneEngineeringCommandTools(FMCPToolRegistry& Registry)
{
	using namespace UEAISceneEngineeringCommandPrivate;
	Registry.Register(MakeShared<FTool_WorldChangePlan>());
	Registry.Register(MakeShared<FTool_WorldChangeExecute>());
	Registry.Register(MakeShared<FTool_WorldChangeRollback>());
	Registry.Register(MakeShared<FTool_RenderSettingsPlan>());
	Registry.Register(MakeShared<FTool_RenderSettingsExecute>());
	Registry.Register(MakeShared<FTool_RenderSettingsRollback>());
	Registry.Register(MakeShared<FTool_HLODBuild>(Registry));
	Registry.Register(MakeShared<FTool_PCGGenerate>());
	Registry.Register(MakeShared<FTool_PCGCleanup>());
}
}
