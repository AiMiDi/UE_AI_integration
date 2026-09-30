// Plan-gated authoring for one Niagara emitter simulation stage.
//
// The native Niagara editor helper that resets a graph output is not exported
// by the engine module in this branch. This handler therefore creates the
// minimal parameter-map input/output pair locally using public graph APIs.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSimulationStageBase.h"
#include "NiagaraSystem.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace UEAINiagaraSimulationStagePrivate
{
	using UEAIIntegration::Infrastructure::TryDigestJson;
	using UEAIIntegration::Infrastructure::ValidateChangeApproval;

	constexpr int32 MaxPathCharacters = 2048;
	constexpr int32 MaxEmitterSelectorCharacters = 512;
	constexpr int32 MaxStageNameCharacters = 128;
	constexpr int32 MaxStageIndex = 128;
	const TCHAR* GenericStageClassPath = TEXT("/Script/Niagara.NiagaraSimulationStageGeneric");

	struct FStageRequest
	{
		FString SystemPath;
		FString EmitterSelector;
		FGuid UsageId;
		FString StageClassPath = GenericStageClassPath;
		FString StageName;
		bool bEnabled = true;
		int32 TargetIndex = INDEX_NONE;
	};

	struct FStageTarget
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

	struct FStagePlanData
	{
		FStageRequest Request;
		FStageTarget Target;
		UClass* StageClass = nullptr;
		bool bBlocked = false;
		TArray<FString> Risks;
		TArray<FString> Warnings;
	};

	struct FStageReceipt
	{
		FString ReceiptId;
		FString RequestId;
		FString PlanDigest;
		FString SystemPath;
		FString EmitterName;
		FString EmitterPath;
		FString GraphPath;
		FString StagePath;
		FString StageClassPath;
		FString EmitterChangeIdAfter;
		FGuid EmitterVersion;
		FGuid UsageId;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraEmitter> Emitter;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TWeakObjectPtr<UNiagaraSimulationStageBase> Stage;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
	};

	TMap<FString, FStageReceipt>& Receipts()
	{
		static TMap<FString, FStageReceipt> Values;
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

	bool ReadTargetIndex(const TSharedPtr<FJsonObject>& Params, int32& OutIndex)
	{
		OutIndex = INDEX_NONE;
		if (!Params->HasField(TEXT("index")))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(TEXT("index"), Number)
			|| !FMath::IsFinite(Number)
			|| Number < 0.0
			|| Number > MaxStageIndex
			|| Number != FMath::FloorToDouble(Number))
		{
			return false;
		}
		OutIndex = static_cast<int32>(Number);
		return true;
	}

	bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FStageRequest& OutRequest, FString& OutCode,
	                  FString& OutError)
	{
		OutRequest = FStageRequest();
		OutCode.Reset();
		OutError.Reset();
		if (!Params.IsValid())
		{
			OutCode = TEXT("invalid_request");
			OutError = TEXT("A Niagara simulation-stage request is required.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("system"), OutRequest.SystemPath)
			|| OutRequest.SystemPath.TrimStartAndEnd().IsEmpty())
		{
			OutCode = TEXT("system_required");
			OutError = TEXT("system is required.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("emitter"), OutRequest.EmitterSelector)
			|| OutRequest.EmitterSelector.TrimStartAndEnd().IsEmpty()
			|| OutRequest.EmitterSelector.Len() > MaxEmitterSelectorCharacters)
		{
			OutCode = TEXT("emitter_required");
			OutError = TEXT("emitter must be a bounded handle ID or display name.");
			return false;
		}
		FString UsageIdString;
		if (!Params->TryGetStringField(TEXT("usageId"), UsageIdString)
			|| !FGuid::Parse(UsageIdString, OutRequest.UsageId)
			|| !OutRequest.UsageId.IsValid())
		{
			OutCode = TEXT("usage_id_required");
			OutError = TEXT("usageId must be a valid GUID and is the stable simulation-stage identity.");
			return false;
		}
		OutRequest.SystemPath = OutRequest.SystemPath.TrimStartAndEnd();
		OutRequest.EmitterSelector = OutRequest.EmitterSelector.TrimStartAndEnd();
		if (Params->HasField(TEXT("stageClass"))
			&& (!Params->TryGetStringField(TEXT("stageClass"), OutRequest.StageClassPath)
				|| OutRequest.StageClassPath.Len() > MaxPathCharacters
				|| OutRequest.StageClassPath.IsEmpty()))
		{
			OutCode = TEXT("stage_class_invalid");
			OutError = TEXT("stageClass must be a bounded Niagara simulation-stage class path.");
			return false;
		}
		if (Params->HasField(TEXT("name"))
			&& (!Params->TryGetStringField(TEXT("name"), OutRequest.StageName)
				|| OutRequest.StageName.Len() > MaxStageNameCharacters))
		{
			OutCode = TEXT("stage_name_invalid");
			OutError = FString::Printf(TEXT("name must be at most %d characters."), MaxStageNameCharacters);
			return false;
		}
		OutRequest.StageName = OutRequest.StageName.TrimStartAndEnd();
		if (OutRequest.StageName.IsEmpty())
		{
			OutRequest.StageName = FString::Printf(
				TEXT("SimulationStage_%s"), *OutRequest.UsageId.ToString(EGuidFormats::Digits));
		}
		if (Params->HasField(TEXT("enabled"))
			&& !Params->TryGetBoolField(TEXT("enabled"), OutRequest.bEnabled))
		{
			OutCode = TEXT("enabled_invalid");
			OutError = TEXT("enabled must be a boolean.");
			return false;
		}
		if (!ReadTargetIndex(Params, OutRequest.TargetIndex))
		{
			OutCode = TEXT("index_invalid");
			OutError = FString::Printf(TEXT("index must be an integer from 0 to %d."), MaxStageIndex);
			return false;
		}
		return true;
	}

	bool ResolveTarget(const FStageRequest& Request, FStageTarget& OutTarget, FString& OutCode, FString& OutError)
	{
		OutTarget = FStageTarget();
		const FString SystemPath = NormalizeObjectPath(Request.SystemPath);
		if (SystemPath.IsEmpty())
		{
			OutCode = TEXT("invalid_object_path");
			OutError = TEXT("system must be a valid object or package path.");
			return false;
		}
		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath, nullptr, LOAD_NoWarn);
		if (!System)
		{
			OutCode = TEXT("system_not_found");
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
					OutCode = TEXT("ambiguous_emitter");
					OutError = TEXT("emitter is ambiguous; use an exact handle GUID.");
					return false;
				}
				Match = &Handle;
			}
		}
		if (!Match)
		{
			OutCode = TEXT("emitter_not_found");
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
			OutCode = TEXT("emitter_graph_missing");
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

	UClass* ResolveStageClass(const FString& ClassPath)
	{
		UClass* Class = LoadObject<UClass>(nullptr, *ClassPath, nullptr, LOAD_NoWarn);
		return Class && Class->IsChildOf(UNiagaraSimulationStageBase::StaticClass()) && !Class->
		       HasAnyClassFlags(CLASS_Abstract)
			       ? Class
			       : nullptr;
	}

	UNiagaraSimulationStageBase* FindStage(FVersionedNiagaraEmitterData* Data, const FGuid& UsageId)
	{
		if (!Data)
		{
			return nullptr;
		}
		for (UNiagaraSimulationStageBase* Stage : Data->GetSimulationStages())
		{
			if (Stage && Stage->Script && Stage->Script->GetUsageId() == UsageId)
			{
				return Stage;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindParameterMapPin(UNiagaraNode* Node, EEdGraphPinDirection Direction)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->GetAllPins())
		{
			if (Pin && Pin->Direction == Direction
				&& UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef())
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UNiagaraNodeOutput* CreateStageOutput(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		if (!Graph)
		{
			return nullptr;
		}
		if (UNiagaraNodeOutput* Existing = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript,
		                                                         UsageId))
		{
			return Existing;
		}
		FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
		UNiagaraNodeOutput* Output = OutputCreator.CreateNode();
		Output->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
		Output->SetUsageId(UsageId);
		Output->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
		OutputCreator.Finalize();
		UEdGraphPin* OutputInput = FindParameterMapPin(Output, EGPD_Input);
		if (!OutputInput)
		{
			Graph->RemoveNode(Output);
			return nullptr;
		}
		FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
		UNiagaraNodeInput* Input = InputCreator.CreateNode();
		Input->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
		Input->Usage = ENiagaraInputNodeUsage::Parameter;
		InputCreator.Finalize();
		UEdGraphPin* InputOutput = FindParameterMapPin(Input, EGPD_Output);
		if (!InputOutput)
		{
			Graph->RemoveNode(Output);
			Graph->RemoveNode(Input);
			return nullptr;
		}
		OutputInput->MakeLinkTo(InputOutput);
		Graph->NotifyGraphChanged();
		return Output;
	}

	bool HasStageOutput(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		return Graph && Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId) != nullptr;
	}

	bool RemoveStageOutput(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		if (!Graph)
		{
			return false;
		}
		UNiagaraNodeOutput* Output = Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId);
		if (!Output)
		{
			return true;
		}
		TArray<UNiagaraNode*> Nodes;
		Nodes.Add(Output);
		for (UEdGraphPin* Pin : Output->GetAllPins())
		{
			if (Pin)
			{
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (UNiagaraNode* LinkedNode = LinkedPin ? Cast<UNiagaraNode>(LinkedPin->GetOwningNode()) : nullptr)
					{
						Nodes.AddUnique(LinkedNode);
					}
				}
			}
		}
		for (UNiagaraNode* Node : Nodes)
		{
			if (Node)
			{
				Node->Modify();
				Node->DestroyNode();
			}
		}
		Graph->NotifyGraphChanged();
		return Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId) == nullptr;
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

	FCompileSummary CompileStage(UNiagaraSystem* System, UNiagaraScript* Script)
	{
		FCompileSummary Summary;
		if (!System || !Script)
		{
			return Summary;
		}
		System->RequestCompile(false);
		System->WaitForCompilationComplete(true, false);
		Summary.Status = CompileStatusName(Script->GetLastCompileStatus());
		Summary.bCompiled = Summary.Status == TEXT("upToDate") || Summary.Status == TEXT("upToDateWithWarnings")
			|| Summary.Status == TEXT("computeUpToDateWithWarnings");
		return Summary;
	}

	bool ReadBackMatches(const FStageTarget& Target, const FStageRequest& Request)
	{
		UNiagaraSimulationStageBase* Stage = FindStage(Target.Data, Request.UsageId);
		return Stage && Stage->Script && Stage->Script->GetUsage() == ENiagaraScriptUsage::ParticleSimulationStageScript
			&& Stage->SimulationStageName == FName(*Request.StageName)
			&& Stage->bEnabled == Request.bEnabled
			&& Stage->GetClass()->GetPathName() == Request.StageClassPath
			&& HasStageOutput(Target.Graph, Request.UsageId);
	}

	TSharedRef<FJsonObject> BuildPlanJson(const FStagePlanData& Data)
	{
		const FStageRequest& Request = Data.Request;
		const FStageTarget& Target = Data.Target;
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.simulation_stage"));
		Plan->SetStringField(TEXT("planKind"), TEXT("niagaraSimulationStageAdd"));
		Plan->SetStringField(TEXT("action"), TEXT("addSimulationStage"));
		Plan->SetStringField(TEXT("scope"), Target.System->GetPathName());
		Plan->SetStringField(TEXT("status"), TEXT("planned"));
		Plan->SetStringField(TEXT("system"), Target.System->GetPathName());
		Plan->SetStringField(TEXT("emitter"), Target.EmitterName);
		Plan->SetStringField(TEXT("emitterPath"), Target.EmitterPath);
		Plan->SetStringField(TEXT("graph"), Target.GraphPath);
		Plan->SetStringField(TEXT("usageId"), Request.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Plan->SetStringField(TEXT("stageClass"), Request.StageClassPath);
		Plan->SetStringField(TEXT("name"), Request.StageName);
		Plan->SetBoolField(TEXT("enabled"), Request.bEnabled);
		Plan->SetNumberField(TEXT("index"), Request.TargetIndex == INDEX_NONE ? -1 : Request.TargetIndex);
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
		Preconditions->SetBoolField(TEXT("stagePresent"), false);
		Plan->SetObjectField(TEXT("preconditions"), Preconditions);
		TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetBoolField(TEXT("stagePresent"), false);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetBoolField(TEXT("stagePresent"), true);
		After->SetStringField(TEXT("usageId"), Request.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Plan->SetObjectField(TEXT("before"), Before);
		Plan->SetObjectField(TEXT("after"), After);
		TArray<TSharedPtr<FJsonValue>> Risks;
		for (const FString& Risk : Data.Risks) Risks.Add(MakeShared<FJsonValueString>(Risk));
		Plan->SetArrayField(TEXT("risks"), Risks);
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Data.Warnings) Warnings.Add(MakeShared<FJsonValueString>(Warning));
		Plan->SetArrayField(TEXT("warnings"), Warnings);
		return Plan;
	}

	bool BuildPlanData(const TSharedPtr<FJsonObject>& Params, FStagePlanData& OutData, FString& OutCode,
	                   FString& OutError)
	{
		OutData = FStagePlanData();
		if (!ParseRequest(Params, OutData.Request, OutCode, OutError)
			|| !ResolveTarget(OutData.Request, OutData.Target, OutCode, OutError))
		{
			return false;
		}
		OutData.StageClass = ResolveStageClass(OutData.Request.StageClassPath);
		if (!OutData.StageClass)
		{
			OutCode = TEXT("stage_class_not_found");
			OutError = TEXT("stageClass must resolve to a non-abstract Niagara simulation-stage class.");
			return false;
		}
		if (FindStage(OutData.Target.Data, OutData.Request.UsageId)
			|| HasStageOutput(OutData.Target.Graph, OutData.Request.UsageId))
		{
			OutCode = TEXT("usage_id_conflict");
			OutError = TEXT("usageId is already used by a simulation stage or graph output; choose a new stable GUID.");
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
			"This adds authored Simulation Stage configuration and does not prove GPU execution or runtime particle behavior."));
		return true;
	}

	TSharedRef<FJsonObject> MakeResult(const FStageReceipt& Receipt, bool bReplay)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-simulation-stage-add.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("emitter"), Receipt.EmitterName);
		Result->SetStringField(TEXT("emitterPath"), Receipt.EmitterPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("stage"), Receipt.StagePath);
		Result->SetStringField(TEXT("stageClass"), Receipt.StageClassPath);
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
			TEXT("scope"), TEXT("authored simulation stage; GPU execution and runtime behavior unverified"));
		return Result;
	}

	class FTool_NiagaraSimulationStageAddPlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.add.plan"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStagePlanData Data;
			FString Code, Error;
			if (!BuildPlanData(Params, Data, Code, Error))
				return ErrorResult(
					Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
				return ErrorResult(
					TEXT("Unable to compute the simulation-stage plan digest."), TEXT("digest_unavailable"), 500);
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraSimulationStageAddApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.add.apply"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
				return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			if (const FString* ExistingReceiptId = RequestReceiptIds().Find(RequestId))
			{
				if (FStageReceipt* Existing = Receipts().Find(*ExistingReceiptId))
				{
					FString Code, Error;
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Error))
						return ErrorResult(
							Error, Code, 409);
					return FMCPToolResult::Ok(MakeResult(*Existing, true));
				}
				return ErrorResult(
					TEXT("requestId is associated with an unavailable receipt."), TEXT("request_id_conflict"), 409);
			}
			FStagePlanData Data;
			FString Code, Error;
			if (!BuildPlanData(Params, Data, Code, Error))
				return ErrorResult(
					Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
			FString PlanDigest;
			if (!TryDigestJson(Plan, PlanDigest))
				return ErrorResult(
					TEXT("Unable to compute the simulation-stage plan digest."), TEXT("digest_unavailable"), 500);
			if (!ValidateChangeApproval(Params, PlanDigest, Code, Error)) return ErrorResult(Error, Code, 409);
			if (Data.bBlocked)
				return ErrorResult(
					TEXT("The simulation-stage target is read-only or not owned by the System package."),
					TEXT("plan_blocked"), 409);
			if (Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.Target.
				EmitterChangeId)
				return ErrorResult(
					TEXT("The Niagara emitter changed after the plan was created; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Add Niagara Simulation Stage")));
			Data.Target.System->Modify();
			Data.Target.Emitter->Modify();
			Data.Target.Graph->Modify();
			UNiagaraSimulationStageBase* Stage = NewObject<UNiagaraSimulationStageBase>(
				Data.Target.Emitter, Data.StageClass, NAME_None, RF_Transactional);
			Stage->SimulationStageName = FName(*Data.Request.StageName);
			Stage->bEnabled = Data.Request.bEnabled;
			Stage->OuterEmitterVersion = Data.Target.EmitterVersion;
			Stage->Script = NewObject<UNiagaraScript>(
				Stage, MakeUniqueObjectName(Stage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")),
				RF_Transactional);
			Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
			Stage->Script->SetUsageId(Data.Request.UsageId);
			Stage->Script->SetLatestSource(Data.Target.Source);
			Data.Target.Emitter->AddSimulationStage(Stage, Data.Target.EmitterVersion);
			if (Data.Request.TargetIndex != INDEX_NONE)
				Data.Target.Emitter->MoveSimulationStageToIndex(
					Stage, Data.Request.TargetIndex, Data.Target.EmitterVersion);
			UNiagaraNodeOutput* Output = CreateStageOutput(Data.Target.Graph, Data.Request.UsageId);
			FStageReceipt Receipt;
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = PlanDigest;
			Receipt.SystemPath = Data.Target.System->GetPathName();
			Receipt.EmitterName = Data.Target.EmitterName;
			Receipt.EmitterPath = Data.Target.EmitterPath;
			Receipt.GraphPath = Data.Target.GraphPath;
			Receipt.StagePath = Stage ? Stage->GetPathName() : FString();
			Receipt.StageClassPath = Data.Request.StageClassPath;
			Receipt.EmitterVersion = Data.Target.EmitterVersion;
			Receipt.UsageId = Data.Request.UsageId;
			Receipt.System = Data.Target.System;
			Receipt.Emitter = Data.Target.Emitter;
			Receipt.Graph = Data.Target.Graph;
			Receipt.Stage = Stage;
			FCompileSummary Compile = CompileStage(Data.Target.System, Stage ? Stage->Script : nullptr);
			const bool bReadBack = Output != nullptr && ReadBackMatches(Data.Target, Data.Request);
			if (!bReadBack || !Compile.bCompiled)
			{
				RemoveStageOutput(Data.Target.Graph, Data.Request.UsageId);
				Data.Target.Emitter->RemoveSimulationStage(Stage, Data.Target.EmitterVersion);
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT("Simulation-stage read-back or compilation failed (status=%s); the change was restored."),
						*Compile.Status), !bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"), 500);
			}
			(void)Data.Target.System->MarkPackageDirty();
			Receipt.EmitterChangeIdAfter = Data.Target.Emitter->GetChangeId().ToString(
				EGuidFormats::DigitsWithHyphensLower);
			Receipt.bChanged = true;
			Receipt.bCompiled = Compile.bCompiled;
			Receipt.CompileStatus = Compile.Status;
			Receipts().Add(Receipt.ReceiptId, Receipt);
			RequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeResult(Receipt, false));
		}
	};

	class FTool_NiagaraSimulationStageAddRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.add.rollback"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId, RequestId;
			bool bConfirmWrite = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId) || !Params->
				TryGetStringField(TEXT("requestId"), RequestId) || !Params->
				TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite) || ReceiptId.IsEmpty() || RequestId.IsEmpty() || !
				bConfirmWrite)
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"));
			FStageReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt)
				return ErrorResult(
					TEXT("The Niagara simulation-stage receipt is unknown in this Editor instance."),
					TEXT("receipt_not_found"), 404);
			if (Receipt->bRolledBack) return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			if (Receipt->RequestId != RequestId)
				return ErrorResult(
					TEXT("requestId does not match the simulation-stage receipt."), TEXT("request_id_mismatch"), 409);
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraEmitter* Emitter = Receipt->Emitter.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraSimulationStageBase* Stage = Receipt->Stage.Get();
			if (!System || !Emitter || !Graph || !Stage)
				return ErrorResult(
					TEXT("The Niagara simulation-stage target is no longer loaded."), TEXT("target_unavailable"), 409);
			if (Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->EmitterChangeIdAfter)
				return ErrorResult(
					TEXT("The emitter changed after apply; simulation-stage rollback was refused."),
					TEXT("rollback_conflict"), 409);
			FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			if (FindStage(Data, Receipt->UsageId) != Stage)
				return ErrorResult(
					TEXT("The applied simulation stage is no longer the exact receipt-owned stage."),
					TEXT("rollback_conflict"), 409);
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Simulation Stage")));
			System->Modify();
			Emitter->Modify();
			Graph->Modify();
			const bool bGraphRemoved = RemoveStageOutput(Graph, Receipt->UsageId);
			Emitter->RemoveSimulationStage(Stage, Receipt->EmitterVersion);
			System->RequestCompile(false);
			System->WaitForCompilationComplete(true, false);
			if (!bGraphRemoved || FindStage(Data, Receipt->UsageId) != nullptr || System->
				HasOutstandingCompilationRequests(false))
				return ErrorResult(
					TEXT(
						"Simulation-stage rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			(void)System->MarkPackageDirty();
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
	void RegisterNiagaraSimulationStageTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
		using namespace UEAINiagaraSimulationStagePrivate;
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageAddPlan>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageAddApply>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageAddRollback>());
#else
		class FUnavailableNiagaraSimulationStage final : public FMCPToolBase
		{
		public:
			explicit FUnavailableNiagaraSimulationStage(FString InCapabilityId) : CapabilityId(MoveTemp(InCapabilityId))
			{
			}

			FString GetCapabilityId() const override { return CapabilityId; }

			FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
			{
				return FMCPToolResult::Error(
					TEXT("Niagara simulation-stage authoring requires Niagara editor support in this build."),
					TEXT("feature_unavailable"), 503);
			}

		private:
			FString CapabilityId;
		};
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.add.plan")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.add.apply")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.add.rollback")));
#endif
	}
}
