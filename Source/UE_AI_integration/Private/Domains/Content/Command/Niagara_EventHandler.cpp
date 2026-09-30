// Plan-gated authoring for one Niagara emitter event handler.
//
// The handler owns a script usage id and a bounded set of event options. The
// graph output is created through Niagara's editor utility so the operation
// has the same authored shape as adding an Event Handler in the editor.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace UEAINiagaraEventHandlerPrivate
{
	using UEAIIntegration::Infrastructure::TryDigestJson;
	using UEAIIntegration::Infrastructure::ValidateChangeApproval;

	constexpr int32 MaxPathCharacters = 2048;
	constexpr int32 MaxEmitterSelectorCharacters = 512;
	constexpr int32 MaxEventNameCharacters = 128;
	constexpr uint32 MaxSpawnCount = 1000000;

	struct FEventHandlerRequest
	{
		FString SystemPath;
		FString EmitterSelector;
		FGuid UsageId;
		FGuid SourceEmitterId;
		FString SourceEventName;
		EScriptExecutionMode ExecutionMode = EScriptExecutionMode::EveryParticle;
		uint32 SpawnNumber = 0;
		uint32 MinSpawnNumber = 0;
		uint32 MaxEventsPerFrame = 0;
		bool bRandomSpawnNumber = false;
		bool bUpdateAttributeInitialValues = true;
	};

	struct FEventHandlerTarget
	{
		UNiagaraSystem* System = nullptr;
		UNiagaraEmitter* Emitter = nullptr;
		FVersionedNiagaraEmitterData* Data = nullptr;
		UNiagaraScriptSource* Source = nullptr;
		UNiagaraGraph* Graph = nullptr;
		FGuid EmitterVersion;
		FString EmitterName;
		FString EmitterPath;
		FString GraphPath;
		FString EmitterChangeId;
	};

	struct FEventHandlerPlanData
	{
		FEventHandlerRequest Request;
		FEventHandlerTarget Target;
		bool bBlocked = false;
		TArray<FString> Risks;
		TArray<FString> Warnings;
	};

	struct FEventHandlerReceipt
	{
		FString ReceiptId;
		FString RequestId;
		FString PlanDigest;
		FString SystemPath;
		FString EmitterName;
		FString EmitterPath;
		FString GraphPath;
		FString ScriptPath;
		FString EmitterChangeIdAfter;
		FGuid EmitterVersion;
		FGuid UsageId;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraEmitter> Emitter;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
	};

	TMap<FString, FEventHandlerReceipt>& Receipts()
	{
		static TMap<FString, FEventHandlerReceipt> Values;
		return Values;
	}

	TMap<FString, FString>& RequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	FMCPToolResult ErrorResult(const FString& Message, const FString& Code, int32 Status = 422)
	{
		return FMCPToolResult::Error(Message, Code, Status);
	}

	FString NormalizeObjectPath(const FString& RequestedPath)
	{
		FString Path = RequestedPath.TrimStartAndEnd();
		if (Path.IsEmpty() || Path.Len() > MaxPathCharacters || Path.Contains(TEXT("..")))
		{
			return FString();
		}
		if (!Path.Contains(TEXT(".")))
		{
			const FString AssetName = FPackageName::GetLongPackageAssetName(Path);
			if (!AssetName.IsEmpty())
			{
				Path += TEXT(".");
				Path += AssetName;
			}
		}
		return Path;
	}

	bool ReadBoundedUint(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Name,
		uint32 DefaultValue,
		uint32& OutValue)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Name))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(Name, Number)
			|| !FMath::IsFinite(Number)
			|| Number < 0.0
			|| Number > static_cast<double>(MaxSpawnCount)
			|| Number != FMath::FloorToDouble(Number))
		{
			return false;
		}
		OutValue = static_cast<uint32>(Number);
		return true;
	}

	bool ParseExecutionMode(const FString& Value, EScriptExecutionMode& OutMode)
	{
		if (Value.IsEmpty() || Value.Equals(TEXT("EveryParticle"), ESearchCase::IgnoreCase))
		{
			OutMode = EScriptExecutionMode::EveryParticle;
			return true;
		}
		if (Value.Equals(TEXT("SpawnedParticles"), ESearchCase::IgnoreCase))
		{
			OutMode = EScriptExecutionMode::SpawnedParticles;
			return true;
		}
		if (Value.Equals(TEXT("SingleParticle"), ESearchCase::IgnoreCase))
		{
			OutMode = EScriptExecutionMode::SingleParticle;
			return true;
		}
		return false;
	}

	FString ExecutionModeName(EScriptExecutionMode Mode)
	{
		return StaticEnum<EScriptExecutionMode>()->GetNameStringByValue(static_cast<int64>(Mode));
	}

	bool ParseRequest(
		const TSharedPtr<FJsonObject>& Params,
		FEventHandlerRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutRequest = FEventHandlerRequest();
		OutErrorCode.Reset();
		OutError.Reset();
		if (!Params.IsValid())
		{
			OutErrorCode = TEXT("invalid_request");
			OutError = TEXT("A Niagara event-handler request is required.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("system"), OutRequest.SystemPath)
			|| OutRequest.SystemPath.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("system_required");
			OutError = TEXT("system is required.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("emitter"), OutRequest.EmitterSelector)
			|| OutRequest.EmitterSelector.TrimStartAndEnd().IsEmpty()
			|| OutRequest.EmitterSelector.Len() > MaxEmitterSelectorCharacters)
		{
			OutErrorCode = TEXT("emitter_required");
			OutError = TEXT("emitter must be a bounded handle ID or display name.");
			return false;
		}
		FString UsageIdString;
		if (!Params->TryGetStringField(TEXT("usageId"), UsageIdString)
			|| !FGuid::Parse(UsageIdString, OutRequest.UsageId)
			|| !OutRequest.UsageId.IsValid())
		{
			OutErrorCode = TEXT("usage_id_required");
			OutError = TEXT("usageId must be a valid GUID and is the stable event-handler identity.");
			return false;
		}
		OutRequest.SystemPath = OutRequest.SystemPath.TrimStartAndEnd();
		OutRequest.EmitterSelector = OutRequest.EmitterSelector.TrimStartAndEnd();

		FString SourceEmitterIdString;
		if (Params->HasField(TEXT("sourceEmitterId")))
		{
			if (!Params->TryGetStringField(TEXT("sourceEmitterId"), SourceEmitterIdString)
				|| (!SourceEmitterIdString.IsEmpty() && !
					FGuid::Parse(SourceEmitterIdString, OutRequest.SourceEmitterId)))
			{
				OutErrorCode = TEXT("source_emitter_id_invalid");
				OutError = TEXT("sourceEmitterId must be an empty string or a valid emitter handle GUID.");
				return false;
			}
		}

		if (Params->HasField(TEXT("sourceEventName")))
		{
			if (!Params->TryGetStringField(TEXT("sourceEventName"), OutRequest.SourceEventName)
				|| OutRequest.SourceEventName.Len() > MaxEventNameCharacters)
			{
				OutErrorCode = TEXT("source_event_name_invalid");
				OutError = FString::Printf(
					TEXT("sourceEventName must be at most %d characters."), MaxEventNameCharacters);
				return false;
			}
			OutRequest.SourceEventName = OutRequest.SourceEventName.TrimStartAndEnd();
		}

		FString ExecutionModeString;
		if (Params->HasField(TEXT("executionMode")) && !Params->TryGetStringField(
			TEXT("executionMode"), ExecutionModeString))
		{
			OutErrorCode = TEXT("execution_mode_invalid");
			OutError = TEXT("executionMode must be EveryParticle, SpawnedParticles, or SingleParticle.");
			return false;
		}
		if (!ParseExecutionMode(ExecutionModeString, OutRequest.ExecutionMode))
		{
			OutErrorCode = TEXT("execution_mode_invalid");
			OutError = TEXT("executionMode must be EveryParticle, SpawnedParticles, or SingleParticle.");
			return false;
		}
		if (!ReadBoundedUint(Params, TEXT("spawnNumber"), 0, OutRequest.SpawnNumber)
			|| !ReadBoundedUint(Params, TEXT("minSpawnNumber"), 0, OutRequest.MinSpawnNumber)
			|| !ReadBoundedUint(Params, TEXT("maxEventsPerFrame"), 0, OutRequest.MaxEventsPerFrame))
		{
			OutErrorCode = TEXT("event_count_invalid");
			OutError = FString::Printf(
				TEXT("spawnNumber, minSpawnNumber and maxEventsPerFrame must be integers from 0 to %u."),
				MaxSpawnCount);
			return false;
		}
		if (OutRequest.MinSpawnNumber > OutRequest.SpawnNumber)
		{
			OutErrorCode = TEXT("spawn_range_invalid");
			OutError = TEXT("minSpawnNumber cannot exceed spawnNumber.");
			return false;
		}
		if (Params->HasField(TEXT("randomSpawnNumber"))
			&& !Params->TryGetBoolField(TEXT("randomSpawnNumber"), OutRequest.bRandomSpawnNumber))
		{
			OutErrorCode = TEXT("random_spawn_number_invalid");
			OutError = TEXT("randomSpawnNumber must be a boolean.");
			return false;
		}
		if (Params->HasField(TEXT("updateAttributeInitialValues"))
			&& !Params->TryGetBoolField(TEXT("updateAttributeInitialValues"), OutRequest.bUpdateAttributeInitialValues))
		{
			OutErrorCode = TEXT("initial_values_invalid");
			OutError = TEXT("updateAttributeInitialValues must be a boolean.");
			return false;
		}
		if (OutRequest.ExecutionMode != EScriptExecutionMode::SpawnedParticles
			&& (OutRequest.SpawnNumber != 0 || OutRequest.MinSpawnNumber != 0 || OutRequest.bRandomSpawnNumber))
		{
			OutErrorCode = TEXT("spawn_options_invalid");
			OutError = TEXT("spawnNumber, minSpawnNumber, and randomSpawnNumber apply only to SpawnedParticles.");
			return false;
		}
		return true;
	}

	bool ResolveTarget(
		const FEventHandlerRequest& Request,
		FEventHandlerTarget& OutTarget,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutTarget = FEventHandlerTarget();
		const FString SystemPath = NormalizeObjectPath(Request.SystemPath);
		if (SystemPath.IsEmpty())
		{
			OutErrorCode = TEXT("invalid_object_path");
			OutError = TEXT("system must be a valid object or package path.");
			return false;
		}
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath, nullptr, LOAD_NoWarn);
		if (!System)
		{
			OutErrorCode = TEXT("system_not_found");
			OutError = FString::Printf(TEXT("Niagara System '%s' was not found."), *Request.SystemPath);
			return false;
		}
		FGuid HandleId;
		const bool bSelectorIsId = FGuid::Parse(Request.EmitterSelector, HandleId);
		const FNiagaraEmitterHandle* Match = nullptr;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const bool bMatches = bSelectorIsId
				                      ? Handle.GetId() == HandleId
				                      : Handle.GetName().ToString().Equals(
					                      Request.EmitterSelector, ESearchCase::IgnoreCase);
			if (bMatches)
			{
				if (Match)
				{
					OutErrorCode = TEXT("ambiguous_emitter");
					OutError = TEXT("emitter is ambiguous; use an exact handle GUID.");
					return false;
				}
				Match = &Handle;
			}
		}
		if (!Match)
		{
			OutErrorCode = TEXT("emitter_not_found");
			OutError = FString::Printf(
				TEXT("Emitter '%s' was not found in '%s'."), *Request.EmitterSelector, *SystemPath);
			return false;
		}
		const FVersionedNiagaraEmitter Instance = Match->GetInstance();
		UNiagaraEmitter* Emitter = Instance.Emitter;
		FVersionedNiagaraEmitterData* Data = Match->GetEmitterData();
		UNiagaraScriptSource* Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
		if (!Emitter || !Data || !Source || !Source->NodeGraph)
		{
			OutErrorCode = TEXT("emitter_graph_missing");
			OutError = TEXT("The selected emitter has no editable Niagara graph source.");
			return false;
		}
		OutTarget.System = System;
		OutTarget.Emitter = Emitter;
		OutTarget.Data = Data;
		OutTarget.Source = Source;
		OutTarget.Graph = Source->NodeGraph;
		OutTarget.EmitterVersion = Instance.Version;
		OutTarget.EmitterName = Match->GetName().ToString();
		OutTarget.EmitterPath = Emitter->GetPathName();
		OutTarget.GraphPath = OutTarget.Graph->GetPathName();
		OutTarget.EmitterChangeId = Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower);
		return true;
	}

	FNiagaraEventScriptProperties* FindHandler(FVersionedNiagaraEmitterData* Data, const FGuid& UsageId)
	{
		return Data
			       ? Data->EventHandlerScriptProps.FindByPredicate(
				       [&UsageId](const FNiagaraEventScriptProperties& Entry)
				       {
					       return Entry.Script && Entry.Script->GetUsageId() == UsageId;
				       })
			       : nullptr;
	}

	bool HasEventGraph(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		if (!Graph)
		{
			return false;
		}
		return Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleEventScript, UsageId) != nullptr;
	}

	void CollectReachableNodes(UNiagaraNode* Node, TSet<UNiagaraNode*>& OutNodes)
	{
		if (!Node || OutNodes.Contains(Node))
		{
			return;
		}
		OutNodes.Add(Node);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				CollectReachableNodes(
					LinkedPin ? Cast<UNiagaraNode>(LinkedPin->GetOwningNode()) : nullptr,
					OutNodes);
			}
		}
	}

	bool RemoveEventGraph(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		if (!Graph)
		{
			return false;
		}
		UNiagaraNodeOutput* EventOutput = Graph->FindEquivalentOutputNode(
			ENiagaraScriptUsage::ParticleEventScript, UsageId);
		if (!EventOutput)
		{
			return false;
		}
		TSet<UNiagaraNode*> Nodes;
		CollectReachableNodes(EventOutput, Nodes);
		// A shared graph can feed multiple outputs. Preserve any node that is
		// still reachable from another output before destroying this handler.
		TSet<UNiagaraNode*> SharedNodes;
		for (UEdGraphNode* Candidate : Graph->Nodes)
		{
			UNiagaraNodeOutput* OtherOutput = Cast<UNiagaraNodeOutput>(Candidate);
			if (!OtherOutput || OtherOutput == EventOutput)
			{
				continue;
			}
			CollectReachableNodes(OtherOutput, SharedNodes);
		}
		for (UNiagaraNode* Node : Nodes)
		{
			if (Node && !SharedNodes.Contains(Node))
			{
				Node->Modify();
				Node->DestroyNode();
			}
		}
		Graph->NotifyGraphChanged();
		return Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleEventScript, UsageId) == nullptr;
	}

	bool ReadBackMatches(const FEventHandlerTarget& Target, const FEventHandlerRequest& Request)
	{
		FNiagaraEventScriptProperties* Handler = FindHandler(Target.Data, Request.UsageId);
		if (!Handler || !Handler->Script || Handler->Script->GetUsage() != ENiagaraScriptUsage::ParticleEventScript)
		{
			return false;
		}
		return Handler->ExecutionMode == Request.ExecutionMode
			&& Handler->SpawnNumber == Request.SpawnNumber
			&& Handler->MinSpawnNumber == Request.MinSpawnNumber
			&& Handler->MaxEventsPerFrame == Request.MaxEventsPerFrame
			&& Handler->bRandomSpawnNumber == Request.bRandomSpawnNumber
			&& Handler->UpdateAttributeInitialValues == Request.bUpdateAttributeInitialValues
			&& Handler->SourceEmitterID == Request.SourceEmitterId
			&& Handler->SourceEventName == FName(*Request.SourceEventName)
			&& HasEventGraph(Target.Graph, Request.UsageId);
	}

	FString CompileStatusName(ENiagaraScriptCompileStatus Status)
	{
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Error: return TEXT("error");
		case ENiagaraScriptCompileStatus::NCS_Dirty: return TEXT("dirty");
		case ENiagaraScriptCompileStatus::NCS_BeingCreated: return TEXT("beingCreated");
		case ENiagaraScriptCompileStatus::NCS_UpToDate: return TEXT("upToDate");
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings: return TEXT("upToDateWithWarnings");
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: return TEXT("computeUpToDateWithWarnings");
		default: return TEXT("unknown");
		}
	}

	struct FCompileSummary
	{
		bool bCompiled = false;
		FString Status = TEXT("unknown");
	};

	FCompileSummary CompileHandler(UNiagaraSystem* System, UNiagaraScript* HandlerScript)
	{
		FCompileSummary Summary;
		if (!System || !HandlerScript)
		{
			return Summary;
		}
		System->RequestCompile(false);
		System->WaitForCompilationComplete(true, false);
		const ENiagaraScriptCompileStatus Status = HandlerScript->GetLastCompileStatus();
		Summary.Status = CompileStatusName(Status);
		Summary.bCompiled = Status == ENiagaraScriptCompileStatus::NCS_UpToDate
			|| Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
		return Summary;
	}

	bool RestoreAddedHandler(FEventHandlerTarget& Target, const FGuid& UsageId)
	{
		if (!Target.Emitter || !Target.Data || !Target.Graph)
		{
			return false;
		}
		Target.Emitter->Modify();
		Target.Graph->Modify();
		const bool bGraphRemoved = RemoveEventGraph(Target.Graph, UsageId);
		Target.Emitter->RemoveEventHandlerByUsageId(UsageId, Target.EmitterVersion);
		return bGraphRemoved && FindHandler(Target.Data, UsageId) == nullptr;
	}

	TSharedRef<FJsonObject> BuildPlanJson(const FEventHandlerPlanData& Data)
	{
		const FEventHandlerRequest& Request = Data.Request;
		const FEventHandlerTarget& Target = Data.Target;
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.event_handler"));
		Plan->SetStringField(TEXT("planKind"), TEXT("niagaraEventHandlerAdd"));
		Plan->SetStringField(TEXT("action"), TEXT("addEventHandler"));
		Plan->SetStringField(TEXT("scope"), Target.System->GetPathName());
		Plan->SetStringField(TEXT("status"), TEXT("planned"));
		Plan->SetStringField(TEXT("system"), Target.System->GetPathName());
		Plan->SetStringField(TEXT("emitter"), Target.EmitterName);
		Plan->SetStringField(TEXT("emitterPath"), Target.EmitterPath);
		Plan->SetStringField(TEXT("graph"), Target.GraphPath);
		Plan->SetStringField(TEXT("usageId"), Request.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Plan->SetStringField(
			TEXT("sourceEmitterId"), Request.SourceEmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Plan->SetStringField(TEXT("sourceEventName"), Request.SourceEventName);
		Plan->SetStringField(TEXT("executionMode"), ExecutionModeName(Request.ExecutionMode));
		Plan->SetNumberField(TEXT("spawnNumber"), Request.SpawnNumber);
		Plan->SetNumberField(TEXT("minSpawnNumber"), Request.MinSpawnNumber);
		Plan->SetNumberField(TEXT("maxEventsPerFrame"), Request.MaxEventsPerFrame);
		Plan->SetBoolField(TEXT("randomSpawnNumber"), Request.bRandomSpawnNumber);
		Plan->SetBoolField(TEXT("updateAttributeInitialValues"), Request.bUpdateAttributeInitialValues);
		Plan->SetBoolField(TEXT("editable"), !Data.bBlocked);
		Plan->SetBoolField(TEXT("blocked"), Data.bBlocked);
		Plan->SetBoolField(TEXT("changesState"), !Data.bBlocked);
		Plan->SetStringField(TEXT("risk"), Data.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
		Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
		Plan->SetBoolField(TEXT("confirmWriteRequired"), true);

		TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
		Preconditions->SetStringField(TEXT("emitterChangeId"), Target.EmitterChangeId);
		Preconditions->SetStringField(TEXT("graph"), Target.GraphPath);
		Preconditions->SetStringField(TEXT("usageId"), Request.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Preconditions->SetBoolField(TEXT("handlerPresent"), false);
		Plan->SetObjectField(TEXT("preconditions"), Preconditions);

		TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetBoolField(TEXT("handlerPresent"), false);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetBoolField(TEXT("handlerPresent"), true);
		After->SetStringField(TEXT("scriptUsage"), TEXT("particleEvent"));
		After->SetStringField(TEXT("usageId"), Request.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Plan->SetObjectField(TEXT("before"), Before);
		Plan->SetObjectField(TEXT("after"), After);

		TArray<TSharedPtr<FJsonValue>> Risks;
		for (const FString& Risk : Data.Risks)
		{
			Risks.Add(MakeShared<FJsonValueString>(Risk));
		}
		Plan->SetArrayField(TEXT("risks"), Risks);
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Data.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warning));
		}
		Plan->SetArrayField(TEXT("warnings"), Warnings);
		return Plan;
	}

	bool BuildPlanData(
		const TSharedPtr<FJsonObject>& Params,
		FEventHandlerPlanData& OutData,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutData = FEventHandlerPlanData();
		if (!ParseRequest(Params, OutData.Request, OutErrorCode, OutError)
			|| !ResolveTarget(OutData.Request, OutData.Target, OutErrorCode, OutError))
		{
			return false;
		}
		if (FindHandler(OutData.Target.Data, OutData.Request.UsageId) != nullptr
			|| HasEventGraph(OutData.Target.Graph, OutData.Request.UsageId))
		{
			OutErrorCode = TEXT("usage_id_conflict");
			OutError = TEXT("usageId is already used by an event handler or graph output; choose a new stable GUID.");
			return false;
		}
		const UPackage* Package = OutData.Target.System->GetOutermost();
		OutData.bBlocked = !Package || !Package->GetName().StartsWith(TEXT("/Game/"))
			|| OutData.Target.Emitter->GetOutermost() != Package;
		if (OutData.bBlocked)
		{
			OutData.Risks.Add(TEXT("The selected System or emitter is not an owned non-transient /Game/ asset."));
		}
		OutData.Warnings.Add(TEXT(
			"This adds an authored Particle Event script and does not prove event delivery or runtime particle behavior."));
		OutData.Warnings.Add(
			TEXT("The requested usageId is the stable identity used for later readback and same-Editor rollback."));
		return true;
	}

	TSharedRef<FJsonObject> MakeResult(const FEventHandlerReceipt& Receipt, bool bReplay)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-event-handler-add.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("emitter"), Receipt.EmitterName);
		Result->SetStringField(TEXT("emitterPath"), Receipt.EmitterPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("script"), Receipt.ScriptPath);
		Result->SetStringField(TEXT("usageId"), Receipt.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetBoolField(TEXT("changed"), Receipt.bChanged);
		Result->SetBoolField(TEXT("verified"), true);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
		Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
		Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
		Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
		Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Result->SetStringField(
			TEXT("scope"), TEXT("authored event handler; event delivery and runtime execution unverified"));
		return Result;
	}

	class FTool_NiagaraEventHandlerAddPlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.add.plan"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FEventHandlerPlanData Data;
			FString ErrorCode;
			FString Error;
			if (!BuildPlanData(Params, Data, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(
					TEXT("Unable to compute the event-handler plan digest."), TEXT("digest_unavailable"), 500);
			}
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraEventHandlerAddApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.add.apply"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			}
			if (const FString* ExistingReceiptId = RequestReceiptIds().Find(RequestId))
			{
				if (FEventHandlerReceipt* Existing = Receipts().Find(*ExistingReceiptId))
				{
					FString ErrorCode;
					FString Error;
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
					{
						return ErrorResult(Error, ErrorCode, 409);
					}
					return FMCPToolResult::Ok(MakeResult(*Existing, true));
				}
				return ErrorResult(
					TEXT("requestId is associated with an unavailable receipt."), TEXT("request_id_conflict"), 409);
			}

			FEventHandlerPlanData Data;
			FString ErrorCode;
			FString Error;
			if (!BuildPlanData(Params, Data, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
			FString PlanDigest;
			if (!TryDigestJson(Plan, PlanDigest))
			{
				return ErrorResult(
					TEXT("Unable to compute the event-handler plan digest."), TEXT("digest_unavailable"), 500);
			}
			if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 409);
			}
			if (Data.bBlocked)
			{
				return ErrorResult(
					TEXT("The event-handler target is read-only or not owned by the System package."),
					TEXT("plan_blocked"), 409);
			}
			if (Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.Target.
				EmitterChangeId)
			{
				return ErrorResult(
					TEXT("The Niagara emitter changed after the plan was created; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Add Niagara Event Handler")));
			Data.Target.System->Modify();
			Data.Target.Emitter->Modify();
			Data.Target.Graph->Modify();
			FNiagaraEventScriptProperties Properties;
			Properties.ExecutionMode = Data.Request.ExecutionMode;
			Properties.SpawnNumber = Data.Request.SpawnNumber;
			Properties.MinSpawnNumber = Data.Request.MinSpawnNumber;
			Properties.MaxEventsPerFrame = Data.Request.MaxEventsPerFrame;
			Properties.bRandomSpawnNumber = Data.Request.bRandomSpawnNumber;
			Properties.UpdateAttributeInitialValues = Data.Request.bUpdateAttributeInitialValues;
			Properties.SourceEmitterID = Data.Request.SourceEmitterId;
			Properties.SourceEventName = FName(*Data.Request.SourceEventName);
			Properties.Script = NewObject<UNiagaraScript>(
				Data.Target.Emitter,
				MakeUniqueObjectName(Data.Target.Emitter, UNiagaraScript::StaticClass(), TEXT("EventScript")),
				RF_Transactional);
			if (!Properties.Script)
			{
				return ErrorResult(
					TEXT("The Niagara event-handler script could not be created."),
					TEXT("event_handler_create_failed"), 500);
			}
			Properties.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
			Properties.Script->SetUsageId(Data.Request.UsageId);
			Properties.Script->SetLatestSource(Data.Target.Source);
			Data.Target.Emitter->AddEventHandler(Properties, Data.Target.EmitterVersion);
			if (!Properties.Script || Properties.Script->GetUsage() != ENiagaraScriptUsage::ParticleEventScript)
			{
				return ErrorResult(
					TEXT("The Niagara editor did not create the event-handler script."),
					TEXT("event_handler_create_failed"), 500);
			}
			FEventHandlerReceipt Receipt;
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = PlanDigest;
			Receipt.SystemPath = Data.Target.System->GetPathName();
			Receipt.EmitterName = Data.Target.EmitterName;
			Receipt.EmitterPath = Data.Target.EmitterPath;
			Receipt.GraphPath = Data.Target.GraphPath;
			Receipt.EmitterVersion = Data.Target.EmitterVersion;
			Receipt.UsageId = Data.Request.UsageId;
			Receipt.System = Data.Target.System;
			Receipt.Emitter = Data.Target.Emitter;
			Receipt.Graph = Data.Target.Graph;
			Receipt.ScriptPath = Properties.Script ? Properties.Script->GetPathName() : FString();

			FNiagaraEventScriptProperties* ReadBackHandler = FindHandler(Data.Target.Data, Data.Request.UsageId);
			const bool bReadBack = HasEventGraph(Data.Target.Graph, Data.Request.UsageId)
				&& ReadBackHandler
				&& ReadBackHandler->Script == Properties.Script
				&& ReadBackMatches(Data.Target, Data.Request);
			FCompileSummary CompileSummary = CompileHandler(Data.Target.System, Properties.Script);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				const bool bRestored = RestoreAddedHandler(Data.Target, Data.Request.UsageId);
				const FCompileSummary RestoreCompile = bRestored
					                                       ? CompileHandler(Data.Target.System, nullptr)
					                                       : FCompileSummary();
				Transaction.Cancel();
				if (!bRestored || (Properties.Script && Data.Target.System->HasOutstandingCompilationRequests(false)))
				{
					return ErrorResult(
						TEXT(
							"Event-handler creation failed and restoration could not be verified; the Editor transaction was retained for Undo."),
						TEXT("restore_verification_failed"), 500);
				}
				return ErrorResult(
					FString::Printf(
						TEXT("Event-handler read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"), 500);
			}

			Data.Target.System->MarkPackageDirty();
			Receipt.EmitterChangeIdAfter = Data.Target.Emitter->GetChangeId().ToString(
				EGuidFormats::DigitsWithHyphensLower);
			Receipt.bChanged = true;
			Receipt.bCompiled = CompileSummary.bCompiled;
			Receipt.CompileStatus = CompileSummary.Status;
			Receipts().Add(Receipt.ReceiptId, Receipt);
			RequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeResult(Receipt, false));
		}
	};

	class FTool_NiagaraEventHandlerAddRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.add.rollback"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId;
			FString RequestId;
			bool bConfirmWrite = false;
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId)
				|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
				|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
				|| ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirmWrite)
			{
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"));
			}
			FEventHandlerReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt)
			{
				return ErrorResult(
					TEXT("The Niagara event-handler receipt is unknown in this Editor instance."),
					TEXT("receipt_not_found"), 404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(
					TEXT("requestId does not match the event-handler receipt."), TEXT("request_id_mismatch"), 409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraEmitter* Emitter = Receipt->Emitter.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			if (!System || !Emitter || !Graph)
			{
				return ErrorResult(
					TEXT("The Niagara event-handler target is no longer loaded."), TEXT("target_unavailable"), 409);
			}
			if (Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->EmitterChangeIdAfter)
			{
				return ErrorResult(
					TEXT("The emitter changed after apply; event-handler rollback was refused."),
					TEXT("rollback_conflict"), 409);
			}
			FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			FNiagaraEventScriptProperties* Handler = FindHandler(Data, Receipt->UsageId);
			if (!Handler || !Handler->Script || Handler->Script->GetPathName() != Receipt->ScriptPath)
			{
				return ErrorResult(
					TEXT("The applied event handler is no longer the exact receipt-owned handler."),
					TEXT("rollback_conflict"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Event Handler")));
			System->Modify();
			Emitter->Modify();
			Graph->Modify();
			FEventHandlerTarget Target;
			Target.System = System;
			Target.Emitter = Emitter;
			Target.Data = Data;
			Target.Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
			Target.Graph = Graph;
			Target.EmitterVersion = Receipt->EmitterVersion;
			Target.EmitterName = Receipt->EmitterName;
			Target.EmitterPath = Receipt->EmitterPath;
			Target.GraphPath = Receipt->GraphPath;
			const bool bRestored = RestoreAddedHandler(Target, Receipt->UsageId);
			System->RequestCompile(false);
			System->WaitForCompilationComplete(true, false);
			const bool bReadBack = bRestored && FindHandler(Data, Receipt->UsageId) == nullptr;
			if (!bReadBack || System->HasOutstandingCompilationRequests(false))
			{
				return ErrorResult(
					TEXT(
						"Event-handler rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bCompiled = true;
			Receipt->CompileStatus = TEXT("upToDate");
			return FMCPToolResult::Ok(MakeResult(*Receipt, false));
		}
	};
}
#endif

namespace UEAIIntegrationTools
{
	void RegisterNiagaraEventHandlerTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
		using namespace UEAINiagaraEventHandlerPrivate;
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerAddPlan>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerAddApply>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerAddRollback>());
#else
		class FUnavailableNiagaraEventHandler final : public FMCPToolBase
		{
		public:
			explicit FUnavailableNiagaraEventHandler(FString InCapabilityId)
				: CapabilityId(MoveTemp(InCapabilityId))
			{
			}

			FString GetCapabilityId() const override { return CapabilityId; }

			FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
			{
				return FMCPToolResult::Error(
					TEXT("Niagara event-handler authoring requires Niagara editor support in this build."),
					TEXT("feature_unavailable"), 503);
			}

		private:
			FString CapabilityId;
		};
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.add.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.add.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.add.rollback")));
#endif
	}
}
