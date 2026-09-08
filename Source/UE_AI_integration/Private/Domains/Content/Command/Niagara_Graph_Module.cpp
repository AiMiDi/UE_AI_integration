// Plan-gated Niagara stack module insertion.
//
// This command is intentionally separate from the collision policy handlers:
// a System can contain a CollisionQuery module without exposing an Async GPU
// Trace data interface, so replacing the DF path sometimes requires adding a
// real module to the owning stack.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/DomainChangePlan.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

//++[SilverPalace] Begin add by wuziye 2026/09/04
#include "Algo/Reverse.h"
#include "EdGraph/EdGraphPin.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "NiagaraEmitterFactoryNew.h"
#endif
//--[SilverPalace] End add by wuziye

namespace UEAINiagaraModulePrivate
{
using UEAIIntegration::Infrastructure::TryDigestJson;
using UEAIIntegration::Infrastructure::ValidateChangeApproval;

constexpr int32 MaxTargetIndex = 256;

struct FModuleRequest
{
	FString SystemPath;
	FString EmitterSelector;
	FString GraphSelector;
	FString OutputNodePath;
	FString ModuleScriptPath;
	FString InsertBeforeSelector;
	FString SuggestedName;
	int32 TargetIndex = INDEX_NONE;
	bool bHasTargetIndex = false;
};

struct FModuleTarget
{
	UNiagaraGraph* Graph = nullptr;
	UNiagaraScript* OwningScript = nullptr;
	UNiagaraNodeOutput* OutputNode = nullptr;
	FString EmitterName;
	FString EmitterPath;
	FString GraphPath;
	FString OutputPath;
	FString ScriptUsage;
	bool bEditable = false;
	int32 ExistingModuleCount = 0;
	int32 ResolvedTargetIndex = INDEX_NONE;
	FString ResolvedInsertBefore;
	TArray<FString> OrderedNodePaths;
};

struct FModulePlanData
{
	FModuleRequest Request;
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UNiagaraScript> ModuleScript;
	FModuleTarget Target;
	FString SystemObjectPath;
	FString ModuleObjectPath;
	FString GraphChangeId;
	bool bBlocked = false;
	TArray<FString> Risks;
	TArray<FString> Warnings;
};

struct FModuleReceipt
{
	FString ReceiptId;
	FString RequestId;
	FString PlanDigest;
	FString SystemPath;
	FString GraphPath;
	FString OutputPath;
	FString ModuleScriptPath;
	FString NodePath;
	FString GraphChangeIdAfter;
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UNiagaraGraph> Graph;
	TWeakObjectPtr<UNiagaraNodeOutput> OutputNode;
	TWeakObjectPtr<UNiagaraNodeFunctionCall> Node;
	TWeakObjectPtr<UNiagaraNode> PreviousNode;
	TWeakObjectPtr<UNiagaraNode> NextNode;
	FName PreviousPinName;
	FName NextPinName;
	bool bChanged = false;
	bool bCompiled = false;
	FString CompileStatus = TEXT("notRequired");
	bool bRolledBack = false;
};

TMap<FString, FModuleReceipt>& Receipts()
{
	static TMap<FString, FModuleReceipt> Values;
	return Values;
}

TMap<FString, FString>& RequestReceiptIds()
{
	static TMap<FString, FString> Values;
	return Values;
}

FMCPToolResult ErrorResult(
	const FString& Message,
	const FString& Code,
	const int32 HttpStatus = 422)
{
	return FMCPToolResult::Error(Message, Code, HttpStatus);
}

FString NormalizeObjectPath(const FString& RequestedPath)
{
	FString Path = RequestedPath.TrimStartAndEnd();
	if (Path.IsEmpty() || Path.Contains(TEXT("..")))
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

bool MatchesSelector(
	const FString& Selector,
	const FString& Name,
	const FString& ObjectPath)
{
	if (Selector.IsEmpty())
	{
		return true;
	}
	return Selector.Equals(Name, ESearchCase::IgnoreCase)
		|| Selector.Equals(ObjectPath, ESearchCase::IgnoreCase)
		|| ObjectPath.EndsWith(
			FString::Printf(TEXT(".%s"), *Selector),
			ESearchCase::IgnoreCase);
}

FString UsageName(const ENiagaraScriptUsage Usage)
{
	switch (Usage)
	{
	case ENiagaraScriptUsage::ParticleSpawnScript:
		return TEXT("particleSpawn");
	case ENiagaraScriptUsage::ParticleUpdateScript:
		return TEXT("particleUpdate");
	case ENiagaraScriptUsage::EmitterSpawnScript:
		return TEXT("emitterSpawn");
	case ENiagaraScriptUsage::EmitterUpdateScript:
		return TEXT("emitterUpdate");
	case ENiagaraScriptUsage::ParticleEventScript:
		return TEXT("particleEvent");
	case ENiagaraScriptUsage::ParticleSimulationStageScript:
		return TEXT("particleSimulationStage");
	case ENiagaraScriptUsage::ParticleGPUComputeScript:
		return TEXT("particleGpuCompute");
	default:
		return TEXT("other");
	}
}

int32 UsagePriority(const ENiagaraScriptUsage Usage)
{
	switch (Usage)
	{
	case ENiagaraScriptUsage::ParticleUpdateScript:
		return 0;
	case ENiagaraScriptUsage::ParticleSpawnScript:
		return 1;
	case ENiagaraScriptUsage::EmitterUpdateScript:
		return 2;
	case ENiagaraScriptUsage::EmitterSpawnScript:
		return 3;
	default:
		return 10;
	}
}

FString NodeScriptPath(const UNiagaraNodeFunctionCall* Node)
{
	if (!Node)
	{
		return FString();
	}
	if (Node->FunctionScript)
	{
		return Node->FunctionScript->GetPathName();
	}
	return Node->FunctionScriptAssetObjectPath.ToString();
}

bool ScriptObjectPathsEqual(
	const FString& LeftPath,
	const FString& RightPath)
{
	const FString NormalizedLeftPath = NormalizeObjectPath(LeftPath);
	const FString NormalizedRightPath = NormalizeObjectPath(RightPath);
	return !NormalizedLeftPath.IsEmpty()
		&& !NormalizedRightPath.IsEmpty()
		&& NormalizedLeftPath.Equals(
			NormalizedRightPath,
			ESearchCase::IgnoreCase);
}

bool ModuleScriptIdentityMatches(
	const UNiagaraNodeFunctionCall* Node,
	const FString& RequestedModulePath)
{
	if (!Node)
	{
		return false;
	}
	if (ScriptObjectPathsEqual(
		Node->FunctionScriptAssetObjectPath.ToString(),
		RequestedModulePath))
	{
		return true;
	}
	return ScriptObjectPathsEqual(
		NodeScriptPath(Node),
		RequestedModulePath);
}

UEdGraphPin* FindMapPin(UNiagaraNode* Node, const EEdGraphPinDirection Direction)
{
	if (!Node)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Node->IsParameterMapPin(Pin))
		{
			return Pin;
		}
	}
	return nullptr;
}

bool BuildStackOrder(
	UNiagaraNodeOutput* Output,
	TArray<UNiagaraNode*>& OutOrdered,
	FString& OutError)
{
	OutOrdered.Reset();
	OutError.Reset();
	if (!Output)
	{
		OutError = TEXT("The Niagara stack output node is unavailable.");
		return false;
	}

	TArray<UNiagaraNode*> ReverseOrder;
	TSet<UNiagaraNode*> Seen;
	UNiagaraNode* Current = Output;
	while (Current)
	{
		if (Seen.Contains(Current))
		{
			OutError = TEXT("The Niagara stack parameter-map chain contains a cycle.");
			return false;
		}
		Seen.Add(Current);
		ReverseOrder.Add(Current);

		UEdGraphPin* InputMap = FindMapPin(Current, EGPD_Input);
		if (!InputMap || InputMap->LinkedTo.Num() == 0)
		{
			break;
		}
		if (InputMap->LinkedTo.Num() != 1 || !InputMap->LinkedTo[0])
		{
			OutError = TEXT("The Niagara stack output has an ambiguous parameter-map link.");
			return false;
		}
		Current = Cast<UNiagaraNode>(
			InputMap->LinkedTo[0]->GetOwningNodeUnchecked());
	}

	if (ReverseOrder.Num() < 2 || !ReverseOrder.Last()->IsA<UNiagaraNodeInput>())
	{
		OutError = TEXT("The Niagara stack output is not connected to a Niagara input node.");
		return false;
	}
	Algo::Reverse(ReverseOrder);
	OutOrdered = MoveTemp(ReverseOrder);
	return true;
}

UNiagaraNodeOutput* FindOutputForScript(
	UNiagaraGraph* Graph,
	const UNiagaraScript* Script)
{
	if (!Graph || !Script)
	{
		return nullptr;
	}
	TArray<UNiagaraNodeOutput*> Outputs;
	Graph->GetNodesOfClass(Outputs);
	for (UNiagaraNodeOutput* Output : Outputs)
	{
		if (Output
			&& Output->GetUsage() == Script->GetUsage()
			&& Output->GetUsageId() == Script->GetUsageId())
		{
			return Output;
		}
	}
	return nullptr;
}

bool ParseRequest(
	const TSharedPtr<FJsonObject>& Params,
	FModuleRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutError)
{
	OutRequest = FModuleRequest();
	if (!Params.IsValid())
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("A Niagara module insertion request is required.");
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
		|| OutRequest.EmitterSelector.TrimStartAndEnd().IsEmpty())
	{
		OutErrorCode = TEXT("emitter_required");
		OutError = TEXT("An explicit emitter selector is required; system-wide module insertion is not supported.");
		return false;
	}
	if (!Params->TryGetStringField(TEXT("moduleScript"), OutRequest.ModuleScriptPath)
		|| OutRequest.ModuleScriptPath.TrimStartAndEnd().IsEmpty())
	{
		OutErrorCode = TEXT("module_script_required");
		OutError = TEXT("moduleScript must be a Niagara Module script object path.");
		return false;
	}
	Params->TryGetStringField(TEXT("graph"), OutRequest.GraphSelector);
	if (Params->HasField(TEXT("outputNodePath"))
		&& (!Params->TryGetStringField(TEXT("outputNodePath"), OutRequest.OutputNodePath)
			|| OutRequest.OutputNodePath.TrimStartAndEnd().IsEmpty()))
	{
		OutErrorCode = TEXT("invalid_output_node_path");
		OutError = TEXT("outputNodePath must be a non-empty canonical output node path.");
		return false;
	}
	OutRequest.OutputNodePath = OutRequest.OutputNodePath.TrimStartAndEnd();
	Params->TryGetStringField(TEXT("insertBeforeNodePath"), OutRequest.InsertBeforeSelector);
	Params->TryGetStringField(TEXT("suggestedName"), OutRequest.SuggestedName);
	OutRequest.EmitterSelector = OutRequest.EmitterSelector.TrimStartAndEnd();
	OutRequest.GraphSelector = OutRequest.GraphSelector.TrimStartAndEnd();
	OutRequest.ModuleScriptPath = OutRequest.ModuleScriptPath.TrimStartAndEnd();
	OutRequest.InsertBeforeSelector = OutRequest.InsertBeforeSelector.TrimStartAndEnd();
	OutRequest.SuggestedName = OutRequest.SuggestedName.TrimStartAndEnd();

	const bool bHasInsertBefore = !OutRequest.InsertBeforeSelector.IsEmpty();
	const bool bHasIndex = Params->HasField(TEXT("targetIndex"));
	if (bHasInsertBefore && bHasIndex)
	{
		OutErrorCode = TEXT("ambiguous_insertion");
		OutError = TEXT("Specify only one of insertBeforeNodePath and targetIndex.");
		return false;
	}
	if (bHasIndex)
	{
		double Number = 0.0;
		if (!Params->TryGetNumberField(TEXT("targetIndex"), Number)
			|| !FMath::IsFinite(Number)
			|| FMath::TruncToInt(Number) != Number
			|| Number < 0.0
			|| Number > MaxTargetIndex)
		{
			OutErrorCode = TEXT("invalid_target_index");
			OutError = FString::Printf(
				TEXT("targetIndex must be an integer in [0,%d]."),
				MaxTargetIndex);
			return false;
		}
		OutRequest.TargetIndex = FMath::TruncToInt(Number);
		OutRequest.bHasTargetIndex = true;
	}
	return true;
}

bool LoadSystemAndModule(
	const FModuleRequest& Request,
	UNiagaraSystem*& OutSystem,
	UNiagaraScript*& OutModule,
	FString& OutSystemPath,
	FString& OutModulePath,
	FString& OutErrorCode,
	FString& OutError)
{
	OutSystemPath = NormalizeObjectPath(Request.SystemPath);
	OutModulePath = NormalizeObjectPath(Request.ModuleScriptPath);
	if (OutSystemPath.IsEmpty() || OutModulePath.IsEmpty())
	{
		OutErrorCode = TEXT("invalid_object_path");
		OutError = TEXT("system and moduleScript must be valid object or package paths.");
		return false;
	}
	OutSystem = LoadObject<UNiagaraSystem>(nullptr, *OutSystemPath);
	if (!OutSystem)
	{
		OutErrorCode = TEXT("system_not_found");
		OutError = FString::Printf(TEXT("Niagara System '%s' was not found."), *Request.SystemPath);
		return false;
	}
	OutModule = LoadObject<UNiagaraScript>(nullptr, *OutModulePath);
	if (!OutModule)
	{
		OutErrorCode = TEXT("module_script_not_found");
		OutError = FString::Printf(TEXT("Niagara Module script '%s' was not found."), *Request.ModuleScriptPath);
		return false;
	}
	if (OutModule->GetUsage() != ENiagaraScriptUsage::Module)
	{
		OutErrorCode = TEXT("invalid_module_script");
		OutError = TEXT("moduleScript must reference a Niagara script whose usage is Module.");
		return false;
	}
	if (!OutModule->GetLatestSource())
	{
		OutErrorCode = TEXT("invalid_module_script");
		OutError = TEXT("The selected Niagara Module script has no loaded source graph.");
		return false;
	}
	return true;
}

bool IsSystemPackageEditable(const UNiagaraSystem* System)
{
	return System
		&& System->GetOutermost()
		&& System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));
}

bool FindTarget(
	UNiagaraSystem* System,
	const FModuleRequest& Request,
	FModuleTarget& OutTarget,
	FString& OutErrorCode,
	FString& OutError)
{
	TArray<FModuleTarget> Candidates;
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FString EmitterName = Handle.GetName().ToString();
		const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		const UNiagaraEmitter* Emitter = Instance.Emitter;
		const FString EmitterPath = Emitter ? Emitter->GetPathName() : FString();
		if (!MatchesSelector(Request.EmitterSelector, EmitterName, EmitterPath))
		{
			continue;
		}
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data)
		{
			continue;
		}

		TArray<UNiagaraScript*> Scripts;
		Data->GetScripts(Scripts, false, false);
		for (UNiagaraScript* Script : Scripts)
		{
			if (!Script || Script->GetUsage() == ENiagaraScriptUsage::Module)
			{
				continue;
			}
			UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
			if (!Source || !Source->NodeGraph)
			{
				continue;
			}
			UNiagaraNodeOutput* Output = FindOutputForScript(Source->NodeGraph, Script);
			if (!Output)
			{
				continue;
			}
			if (!Request.OutputNodePath.IsEmpty()
				&& !Output->GetPathName().Equals(Request.OutputNodePath, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (!Request.GraphSelector.IsEmpty()
				&& !MatchesSelector(
					Request.GraphSelector,
					Source->NodeGraph->GetName(),
					Source->NodeGraph->GetPathName()))
			{
				continue;
			}
			if (Candidates.ContainsByPredicate(
				[&Source, Output](const FModuleTarget& Existing)
				{
					return Existing.Graph == Source->NodeGraph
						&& Existing.OutputNode == Output;
				}))
			{
				continue;
			}

			FModuleTarget Candidate;
			Candidate.Graph = Source->NodeGraph;
			Candidate.OwningScript = Script;
			Candidate.OutputNode = Output;
			Candidate.EmitterName = EmitterName;
			Candidate.EmitterPath = EmitterPath;
			Candidate.GraphPath = Source->NodeGraph->GetPathName();
			Candidate.OutputPath = Output->GetPathName();
			Candidate.ScriptUsage = UsageName(Script->GetUsage());
			Candidate.bEditable = IsSystemPackageEditable(System)
				&& Source->NodeGraph->GetOutermost() == System->GetOutermost()
				&& Output->GetOutermost() == System->GetOutermost();
			Candidates.Add(MoveTemp(Candidate));
		}
	}

	if (Candidates.IsEmpty())
	{
		OutErrorCode = TEXT("emitter_or_graph_not_found");
		OutError = FString::Printf(
			TEXT("No editable Niagara stack output was found for emitter '%s' and graph '%s'."),
			*Request.EmitterSelector,
			*Request.GraphSelector);
		return false;
	}
	if (Candidates.Num() > 1)
	{
		Candidates.Sort(
			[](const FModuleTarget& A, const FModuleTarget& B)
			{
				if (A.GraphPath != B.GraphPath)
				{
					return A.GraphPath < B.GraphPath;
				}
				return A.ScriptUsage < B.ScriptUsage;
			});
		TArray<FString> CandidateDescriptions;
		CandidateDescriptions.Reserve(Candidates.Num());
		for (const FModuleTarget& Candidate : Candidates)
		{
			CandidateDescriptions.Add(
				FString::Printf(
					TEXT("%s (%s)"),
					*Candidate.OutputPath,
					*Candidate.ScriptUsage));
		}
		OutErrorCode = TEXT("ambiguous_graph");
		OutError = FString::Printf(
			TEXT("Emitter '%s' matched multiple Niagara stack outputs; specify outputNodePath (an explicit graph selector alone cannot distinguish outputs in the same graph). Candidates: %s"),
			*Request.EmitterSelector,
			*FString::Join(CandidateDescriptions, TEXT(", ")));
		return false;
	}
	Candidates.Sort(
		[](const FModuleTarget& A, const FModuleTarget& B)
		{
			const int32 APriority = A.OwningScript ? UsagePriority(A.OwningScript->GetUsage()) : 100;
			const int32 BPriority = B.OwningScript ? UsagePriority(B.OwningScript->GetUsage()) : 100;
			if (APriority != BPriority)
			{
				return APriority < BPriority;
			}
			return A.GraphPath < B.GraphPath;
		});
	OutTarget = Candidates[0];
	return true;
}

bool ResolveInsertion(
	const FModuleRequest& Request,
	FModuleTarget& Target,
	FString& OutErrorCode,
	FString& OutError)
{
	const FString RequestedModulePath = NormalizeObjectPath(Request.ModuleScriptPath);
	TArray<UNiagaraNode*> OrderedNodes;
	if (!BuildStackOrder(Target.OutputNode, OrderedNodes, OutError))
	{
		OutErrorCode = TEXT("invalid_stack");
		return false;
	}
	Target.OrderedNodePaths.Reset();
	for (UNiagaraNode* Node : OrderedNodes)
	{
		if (Node)
		{
			Target.OrderedNodePaths.Add(Node->GetPathName());
		}
	}
	Target.ExistingModuleCount = 0;
	for (UNiagaraNode* Node : OrderedNodes)
	{
		if (Cast<UNiagaraNodeFunctionCall>(Node))
		{
			++Target.ExistingModuleCount;
		}
	}

	for (UNiagaraNode* Node : OrderedNodes)
	{
		if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
		{
			if (ModuleScriptIdentityMatches(FunctionCall, RequestedModulePath))
			{
				OutErrorCode = TEXT("module_already_present");
				OutError = FString::Printf(
					TEXT("Module script '%s' is already present in graph '%s'."),
					*Request.ModuleScriptPath,
					*Target.GraphPath);
				return false;
			}
		}
	}

	if (Request.bHasTargetIndex)
	{
		if (Request.TargetIndex > Target.ExistingModuleCount)
		{
			OutErrorCode = TEXT("target_index_out_of_range");
			OutError = FString::Printf(
				TEXT("targetIndex %d exceeds the %d existing stack modules."),
				Request.TargetIndex,
				Target.ExistingModuleCount);
			return false;
		}
		Target.ResolvedTargetIndex = Request.TargetIndex;
		return true;
	}
	if (!Request.InsertBeforeSelector.IsEmpty())
	{
		int32 FunctionIndex = 0;
		for (UNiagaraNode* Node : OrderedNodes)
		{
			UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node);
			if (FunctionCall
				&& MatchesSelector(
					Request.InsertBeforeSelector,
					FunctionCall->GetName(),
					FunctionCall->GetPathName()))
			{
				Target.ResolvedTargetIndex = FunctionIndex;
				Target.ResolvedInsertBefore = FunctionCall->GetPathName();
				return true;
			}
			if (FunctionCall)
			{
				++FunctionIndex;
			}
		}
		OutErrorCode = TEXT("insert_before_node_not_found");
		OutError = FString::Printf(
			TEXT("No stack module matched insertBeforeNodePath '%s'."),
			*Request.InsertBeforeSelector);
		return false;
	}
	Target.ResolvedTargetIndex = INDEX_NONE;
	return true;
}

bool BuildPlanData(
	const TSharedPtr<FJsonObject>& Params,
	FModulePlanData& OutData,
	FString& OutErrorCode,
	FString& OutError)
{
	FModuleRequest Request;
	if (!ParseRequest(Params, Request, OutErrorCode, OutError))
	{
		return false;
	}
	UNiagaraSystem* System = nullptr;
	UNiagaraScript* ModuleScript = nullptr;
	if (!LoadSystemAndModule(
		Request,
		System,
		ModuleScript,
		OutData.SystemObjectPath,
		OutData.ModuleObjectPath,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	OutData = FModulePlanData();
	OutData.Request = Request;
	OutData.System = System;
	OutData.ModuleScript = ModuleScript;
	OutData.SystemObjectPath = NormalizeObjectPath(Request.SystemPath);
	OutData.ModuleObjectPath = NormalizeObjectPath(Request.ModuleScriptPath);
	if (!FindTarget(System, Request, OutData.Target, OutErrorCode, OutError))
	{
		return false;
	}
	if (!ResolveInsertion(Request, OutData.Target, OutErrorCode, OutError))
	{
		return false;
	}
	OutData.GraphChangeId = OutData.Target.Graph
		? OutData.Target.Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower)
		: FString();
	OutData.bBlocked = !OutData.Target.bEditable;
	if (!IsSystemPackageEditable(System))
	{
		OutData.bBlocked = true;
		OutData.Risks.Add(TEXT("The selected Niagara System is outside /Game/ and is read-only for this command."));
	}
	if (!OutData.Target.bEditable)
	{
		OutData.Risks.Add(TEXT("The emitter graph or output node is shared/external; shared graph mutation is forbidden."));
	}
	if (OutData.Target.ResolvedTargetIndex == INDEX_NONE)
	{
		OutData.Warnings.Add(TEXT("No insertion index was requested; Niagara will append the module at the end of the selected stack."));
	}
	if (OutData.Target.ExistingModuleCount == 0)
	{
		OutData.Warnings.Add(TEXT("The selected stack has no existing function-call modules; verify the output chain before applying."));
	}
	OutData.Warnings.Add(TEXT("This edit changes the Niagara graph only; runtime ray-tracing scene readiness during level load still requires a separate runtime check."));
	return true;
}

TSharedRef<FJsonObject> BuildPlanJson(const FModulePlanData& Data)
{
	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
	Plan->SetStringField(TEXT("planKind"), TEXT("niagaraModuleAdd"));
	Plan->SetStringField(TEXT("action"), TEXT("addModule"));
	Plan->SetStringField(TEXT("scope"), Data.SystemObjectPath);
	Plan->SetStringField(TEXT("status"), TEXT("planned"));
	Plan->SetStringField(TEXT("system"), Data.SystemObjectPath);
	Plan->SetStringField(TEXT("emitter"), Data.Target.EmitterName);
	Plan->SetStringField(TEXT("emitterPath"), Data.Target.EmitterPath);
	Plan->SetStringField(TEXT("graph"), Data.Target.GraphPath);
	Plan->SetStringField(TEXT("outputNodePath"), Data.Target.OutputPath);
	Plan->SetStringField(TEXT("owningScript"), Data.Target.OwningScript ? Data.Target.OwningScript->GetPathName() : FString());
	Plan->SetStringField(TEXT("scriptUsage"), Data.Target.ScriptUsage);
	Plan->SetStringField(TEXT("moduleScript"), Data.ModuleObjectPath);
	Plan->SetStringField(TEXT("insertBeforeNodePath"), Data.Target.ResolvedInsertBefore);
	Plan->SetNumberField(TEXT("targetIndex"), Data.Target.ResolvedTargetIndex);
	Plan->SetNumberField(TEXT("existingModuleCount"), Data.Target.ExistingModuleCount);
	Plan->SetBoolField(TEXT("editable"), Data.Target.bEditable);
	Plan->SetBoolField(TEXT("blocked"), Data.bBlocked);
	Plan->SetBoolField(TEXT("changesState"), !Data.bBlocked);
	Plan->SetStringField(
		TEXT("risk"),
		Data.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
	Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
	Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));

	TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
	Preconditions->SetStringField(TEXT("graphChangeId"), Data.GraphChangeId);
	Preconditions->SetStringField(TEXT("graph"), Data.Target.GraphPath);
	Preconditions->SetStringField(TEXT("outputNodePath"), Data.Target.OutputPath);
	Plan->SetObjectField(TEXT("preconditions"), Preconditions);

	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	Before->SetStringField(TEXT("moduleScript"), Data.ModuleObjectPath);
	Before->SetNumberField(TEXT("targetIndex"), Data.Target.ResolvedTargetIndex);
	Before->SetBoolField(TEXT("modulePresent"), false);
	TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
	After->SetStringField(TEXT("moduleScript"), Data.ModuleObjectPath);
	After->SetNumberField(TEXT("targetIndex"), Data.Target.ResolvedTargetIndex);
	After->SetBoolField(TEXT("modulePresent"), true);
	Plan->SetObjectField(TEXT("before"), Before);
	Plan->SetObjectField(TEXT("after"), After);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (const FString& Path : Data.Target.OrderedNodePaths)
	{
		Nodes.Add(MakeShared<FJsonValueString>(Path));
	}
	Plan->SetArrayField(TEXT("orderedNodePaths"), Nodes);
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

FString CompileStatusName(const ENiagaraScriptCompileStatus Status)
{
	switch (Status)
	{
	case ENiagaraScriptCompileStatus::NCS_Error:
		return TEXT("error");
	case ENiagaraScriptCompileStatus::NCS_Dirty:
		return TEXT("dirty");
	case ENiagaraScriptCompileStatus::NCS_BeingCreated:
		return TEXT("beingCreated");
	case ENiagaraScriptCompileStatus::NCS_UpToDate:
		return TEXT("upToDate");
	case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		return TEXT("upToDateWithWarnings");
	case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
		return TEXT("computeUpToDateWithWarnings");
	default:
		return TEXT("unknown");
	}
}

struct FCompileSummary
{
	bool bCompiled = false;
	bool bHasError = false;
	FString Status = TEXT("unknown");
};

FCompileSummary CompileSystem(UNiagaraSystem* System)
{
	FCompileSummary Summary;
	if (!System)
	{
		return Summary;
	}
	System->RequestCompile(false);
	System->WaitForCompilationComplete(true, false);

	TArray<UNiagaraScript*> Scripts;
	TSet<UNiagaraScript*> Seen;
	auto AddScript = [&Scripts, &Seen](UNiagaraScript* Script)
	{
		if (Script && !Seen.Contains(Script))
		{
			Seen.Add(Script);
			Scripts.Add(Script);
		}
	};
	AddScript(System->GetSystemSpawnScript());
	AddScript(System->GetSystemUpdateScript());
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			TArray<UNiagaraScript*> EmitterScripts;
			Data->GetScripts(EmitterScripts, false, false);
			for (UNiagaraScript* Script : EmitterScripts)
			{
				AddScript(Script);
			}
		}
	}

	bool bAnyKnown = false;
	bool bAnyPending = false;
	bool bAnyWarning = false;
	for (UNiagaraScript* Script : Scripts)
	{
		const ENiagaraScriptCompileStatus ScriptStatus = Script->GetLastCompileStatus();
		if (ScriptStatus == ENiagaraScriptCompileStatus::NCS_Error)
		{
			Summary.bHasError = true;
		}
		else if (ScriptStatus == ENiagaraScriptCompileStatus::NCS_Dirty
			|| ScriptStatus == ENiagaraScriptCompileStatus::NCS_BeingCreated)
		{
			bAnyPending = true;
			bAnyKnown = true;
		}
		else if (ScriptStatus == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| ScriptStatus == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings)
		{
			bAnyWarning = true;
			bAnyKnown = true;
		}
		else if (ScriptStatus == ENiagaraScriptCompileStatus::NCS_UpToDate)
		{
			bAnyKnown = true;
		}
	}
	Summary.Status = Summary.bHasError
		? TEXT("error")
		: bAnyPending
			? TEXT("pending")
			: bAnyWarning
			? TEXT("upToDateWithWarnings")
			: bAnyKnown
			? TEXT("upToDate")
			: TEXT("unknown");
	Summary.bCompiled = !Summary.bHasError && !bAnyPending && bAnyKnown;
	return Summary;
}

UEdGraphPin* FindPinByName(UNiagaraNode* Node, const EEdGraphPinDirection Direction, const FName PinName)
{
	if (!Node)
	{
		return nullptr;
	}
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == Direction && Pin->PinName == PinName)
		{
			return Pin;
		}
	}
	return nullptr;
}

void CaptureAdjacentNodes(
	UNiagaraNodeFunctionCall* Node,
	FModuleReceipt& Receipt)
{
	UEdGraphPin* InputMap = FindMapPin(Node, EGPD_Input);
	UEdGraphPin* OutputMap = FindMapPin(Node, EGPD_Output);
	if (InputMap && InputMap->LinkedTo.Num() > 0 && InputMap->LinkedTo[0])
	{
		Receipt.PreviousNode = Cast<UNiagaraNode>(InputMap->LinkedTo[0]->GetOwningNodeUnchecked());
		Receipt.PreviousPinName = InputMap->LinkedTo[0]->PinName;
	}
	if (OutputMap && OutputMap->LinkedTo.Num() > 0 && OutputMap->LinkedTo[0])
	{
		Receipt.NextNode = Cast<UNiagaraNode>(OutputMap->LinkedTo[0]->GetOwningNodeUnchecked());
		Receipt.NextPinName = OutputMap->LinkedTo[0]->PinName;
	}
}

bool AdjacentLinksMatch(const FModuleReceipt& Receipt)
{
	UNiagaraNodeFunctionCall* Node = Receipt.Node.Get();
	if (!Node)
	{
		return false;
	}
	UEdGraphPin* InputMap = FindMapPin(Node, EGPD_Input);
	UEdGraphPin* OutputMap = FindMapPin(Node, EGPD_Output);
	if (!InputMap || !OutputMap || InputMap->LinkedTo.Num() != 1 || OutputMap->LinkedTo.Num() != 1)
	{
		return false;
	}
	const UEdGraphPin* PreviousPin = InputMap->LinkedTo[0];
	const UEdGraphPin* NextPin = OutputMap->LinkedTo[0];
	return PreviousPin
		&& NextPin
		&& PreviousPin->GetOwningNodeUnchecked() == Receipt.PreviousNode.Get()
		&& NextPin->GetOwningNodeUnchecked() == Receipt.NextNode.Get()
		&& PreviousPin->PinName == Receipt.PreviousPinName
		&& NextPin->PinName == Receipt.NextPinName;
}

bool RemoveInsertedNode(FModuleReceipt& Receipt)
{
	UNiagaraGraph* Graph = Receipt.Graph.Get();
	UNiagaraNodeFunctionCall* Node = Receipt.Node.Get();
	if (!Graph || !Node || !AdjacentLinksMatch(Receipt))
	{
		return false;
	}
	UEdGraphPin* InputMap = FindMapPin(Node, EGPD_Input);
	UEdGraphPin* OutputMap = FindMapPin(Node, EGPD_Output);
	UEdGraphPin* PreviousPin = FindPinByName(Receipt.PreviousNode.Get(), EGPD_Output, Receipt.PreviousPinName);
	UEdGraphPin* NextPin = FindPinByName(Receipt.NextNode.Get(), EGPD_Input, Receipt.NextPinName);
	if (!InputMap || !OutputMap || !PreviousPin || !NextPin)
	{
		return false;
	}
	InputMap->BreakAllPinLinks();
	OutputMap->BreakAllPinLinks();
	PreviousPin->MakeLinkTo(NextPin);
	Node->Modify();
	Graph->RemoveNode(Node);
	Graph->NotifyGraphChanged();
	return !Graph->Nodes.Contains(Node);
}

TSharedRef<FJsonObject> MakeResult(const FModuleReceipt& Receipt, const bool bReplay)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-module-add.v1"));
	Result->SetStringField(TEXT("status"), TEXT("succeeded"));
	Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
	Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
	Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
	Result->SetStringField(TEXT("system"), Receipt.SystemPath);
	Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
	Result->SetStringField(TEXT("outputNodePath"), Receipt.OutputPath);
	Result->SetStringField(TEXT("moduleScript"), Receipt.ModuleScriptPath);
	Result->SetStringField(TEXT("nodePath"), Receipt.NodePath);
	Result->SetBoolField(TEXT("changed"), Receipt.bChanged);
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
	Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
	Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
	Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	return Result;
}

class FTool_NiagaraModuleAddPlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.module.add.plan");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FModulePlanData Data;
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
			return ErrorResult(TEXT("Unable to compute the Niagara module insertion plan digest."), TEXT("digest_unavailable"), 500);
		}
		Plan->SetStringField(TEXT("planDigest"), Digest);
		return FMCPToolResult::Ok(Plan);
	}
};

class FTool_NiagaraModuleAddApply final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.module.add.apply");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RequestId;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
		{
			return ErrorResult(TEXT("A non-empty requestId is required for Niagara module writes."), TEXT("request_id_required"), 422);
		}
		if (const FString* ExistingReceiptId = RequestReceiptIds().Find(RequestId))
		{
			if (FModuleReceipt* Existing = Receipts().Find(*ExistingReceiptId))
			{
				FString ErrorCode;
				FString Error;
				if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
				{
					return ErrorResult(Error, ErrorCode, 409);
				}
				return FMCPToolResult::Ok(MakeResult(*Existing, true));
			}
			return ErrorResult(TEXT("requestId is associated with an unavailable Niagara module receipt."), TEXT("request_id_conflict"), 409);
		}

		FModulePlanData Data;
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
			return ErrorResult(TEXT("Unable to compute the Niagara module insertion plan digest."), TEXT("digest_unavailable"), 500);
		}
		if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
		{
			return ErrorResult(Error, ErrorCode, 409);
		}
		if (Data.bBlocked)
		{
			return ErrorResult(TEXT("The Niagara module insertion plan is blocked because the selected graph is shared or read-only."), TEXT("plan_blocked"), 409);
		}

		UNiagaraSystem* System = Data.System.Get();
		UNiagaraGraph* Graph = Data.Target.Graph;
		UNiagaraNodeOutput* Output = Data.Target.OutputNode;
		UNiagaraScript* ModuleScript = Data.ModuleScript.Get();
		if (!System || !Graph || !Output || !ModuleScript
			|| Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.GraphChangeId
			|| Graph->GetPathName() != Data.Target.GraphPath
			|| Output->GetPathName() != Data.Target.OutputPath)
		{
			return ErrorResult(TEXT("The Niagara graph or output changed after the plan was created; re-plan before applying."), TEXT("plan_digest_mismatch"), 409);
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Add Niagara Stack Module")));
		System->Modify();
		Graph->Modify();
		UNiagaraNodeFunctionCall* AddedNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
			ModuleScript,
			*Output,
			Data.Target.ResolvedTargetIndex,
			Data.Request.SuggestedName);
		if (!AddedNode)
		{
			Transaction.Cancel();
			return ErrorResult(TEXT("Niagara AddScriptModuleToStack returned no node."), TEXT("module_add_failed"), 500);
		}

		FModuleReceipt Receipt;
		Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Receipt.RequestId = RequestId;
		Receipt.PlanDigest = PlanDigest;
		Receipt.SystemPath = Data.SystemObjectPath;
		Receipt.GraphPath = Data.Target.GraphPath;
		Receipt.OutputPath = Data.Target.OutputPath;
		Receipt.ModuleScriptPath = Data.ModuleObjectPath;
		Receipt.NodePath = AddedNode->GetPathName();
		Receipt.System = System;
		Receipt.Graph = Graph;
		Receipt.OutputNode = Output;
		Receipt.Node = AddedNode;
		CaptureAdjacentNodes(AddedNode, Receipt);

		const bool bReadBack = Graph->Nodes.Contains(AddedNode)
			&& ModuleScriptIdentityMatches(AddedNode, Data.ModuleObjectPath)
			&& AdjacentLinksMatch(Receipt);
		FCompileSummary CompileSummary = CompileSystem(System);
		if (!bReadBack || !CompileSummary.bCompiled)
		{
			const bool bRemoved = RemoveInsertedNode(Receipt);
			const FCompileSummary RestoreSummary = CompileSystem(System);
			if (!bRemoved || Graph->Nodes.Contains(AddedNode) || !RestoreSummary.bCompiled)
			{
				return ErrorResult(
					TEXT("Niagara module insertion failed and restoration could not be verified; the transaction was retained for Editor Undo."),
					TEXT("restore_verification_failed"), 500);
			}
			Transaction.Cancel();
			return ErrorResult(
				FString::Printf(TEXT("Niagara module insertion read-back or compilation failed (status=%s); the change was restored."), *CompileSummary.Status),
				!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
				500);
		}
		System->MarkPackageDirty();
		Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower);
		Receipt.bChanged = true;
		Receipt.bCompiled = CompileSummary.bCompiled;
		Receipt.CompileStatus = CompileSummary.Status;
		Receipts().Add(Receipt.ReceiptId, Receipt);
		RequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
		return FMCPToolResult::Ok(MakeResult(Receipt, false));
	}
};

class FTool_NiagaraModuleAddRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.module.add.rollback");
	}

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
			return ErrorResult(TEXT("rollbackId, requestId and confirmWrite=true are required."), TEXT("write_confirmation_required"), 422);
		}
		FModuleReceipt* Receipt = Receipts().Find(ReceiptId);
		if (!Receipt)
		{
			return ErrorResult(TEXT("The Niagara module receipt is unknown in this Editor instance."), TEXT("receipt_not_found"), 404);
		}
		if (Receipt->bRolledBack)
		{
			return FMCPToolResult::Ok(MakeResult(*Receipt, true));
		}
		if (Receipt->RequestId != RequestId)
		{
			return ErrorResult(TEXT("requestId does not match the Niagara module receipt."), TEXT("request_id_mismatch"), 409);
		}
		UNiagaraSystem* System = Receipt->System.Get();
		UNiagaraGraph* Graph = Receipt->Graph.Get();
		UNiagaraNodeFunctionCall* Node = Receipt->Node.Get();
		if (!System || !Graph || !Node)
		{
			return ErrorResult(TEXT("The Niagara module target is no longer loaded."), TEXT("target_unavailable"), 409);
		}
		if (Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->GraphChangeIdAfter
			|| !ModuleScriptIdentityMatches(Node, Receipt->ModuleScriptPath)
			|| !AdjacentLinksMatch(*Receipt))
		{
			return ErrorResult(TEXT("The Niagara graph or inserted module changed after apply; rollback was refused."), TEXT("rollback_conflict"), 409);
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Stack Module")));
		System->Modify();
		Graph->Modify();
		if (!RemoveInsertedNode(*Receipt))
		{
			Transaction.Cancel();
			return ErrorResult(TEXT("The inserted Niagara module could not be removed without overwriting newer links."), TEXT("rollback_conflict"), 409);
		}
		FCompileSummary CompileSummary = CompileSystem(System);
		const bool bRemoved = !Graph->Nodes.Contains(Node);
		if (!bRemoved || !CompileSummary.bCompiled)
		{
			return ErrorResult(TEXT("Niagara module rollback read-back or compilation failed; the transaction was retained for Editor Undo."), TEXT("rollback_verification_failed"), 500);
		}
		System->MarkPackageDirty();
		Receipt->bRolledBack = true;
		Receipt->bCompiled = CompileSummary.bCompiled;
		Receipt->CompileStatus = CompileSummary.Status;
		return FMCPToolResult::Ok(MakeResult(*Receipt, false));
	}
};

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraOutputSelectionTest,
	"UE_AI_integration.Niagara.OutputSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAINiagaraOutputSelectionTest::RunTest(const FString& Parameters)
{
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(GetTransientPackage());
	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(System);
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	FNiagaraEmitterHandle Handle(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	System->AddEmitterHandleDirect(Handle);
	UNiagaraGraph* Graph = CastChecked<UNiagaraScriptSource>(Handle.GetEmitterData()->GraphSource)->NodeGraph;
	FModuleRequest Request;
	Request.EmitterSelector = Handle.GetName().ToString();
	Request.GraphSelector = Graph->GetPathName();
	FModuleTarget Target;
	FString ErrorCode, Error;
	TestFalse(TEXT("Graph path alone cannot select an output"), FindTarget(System, Request, Target, ErrorCode, Error));
	TestEqual(TEXT("Ambiguity is explicit"), ErrorCode, FString(TEXT("ambiguous_graph")));
	TArray<UNiagaraScript*> Scripts;
	Handle.GetEmitterData()->GetScripts(Scripts, false, false);
	int32 SelectedOutputs = 0;
	for (UNiagaraScript* Script : Scripts)
	{
		UNiagaraNodeOutput* Output = Script ? FindOutputForScript(Graph, Script) : nullptr;
		if (!Output)
		{
			continue;
		}
		Request.OutputNodePath = Output->GetPathName();
		if (TestTrue(TEXT("Canonical output resolves shared graph ambiguity"), FindTarget(System, Request, Target, ErrorCode, Error)))
		{
			TestTrue(TEXT("Requested output is selected"), Target.OutputNode == Output);
			++SelectedOutputs;
		}
	}
	TestTrue(TEXT("Emitter and particle spawn/update outputs were exercised"), SelectedOutputs >= 4);
	Request.OutputNodePath = Graph->GetPathName() + TEXT(".MissingOutput");
	TestFalse(TEXT("Unknown output does not fall back to another stage"), FindTarget(System, Request, Target, ErrorCode, Error));
	return true;
}
#endif

} // namespace UEAINiagaraModulePrivate

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry)
{
	using namespace UEAINiagaraModulePrivate;
	Registry.Register(MakeShared<FTool_NiagaraModuleAddPlan>());
	Registry.Register(MakeShared<FTool_NiagaraModuleAddApply>());
	Registry.Register(MakeShared<FTool_NiagaraModuleAddRollback>());
}
}

#else

class FUnavailableNiagaraModuleTool final : public FMCPToolBase
{
public:
	explicit FUnavailableNiagaraModuleTool(FString InCapabilityId)
		: CapabilityId(MoveTemp(InCapabilityId))
	{
	}

	FString GetCapabilityId() const override { return CapabilityId; }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(
			TEXT("Niagara module support was not compiled into this plugin build."),
			TEXT("capability_unavailable"),
			409);
	}

private:
	FString CapabilityId;
};

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
		TEXT("content.niagara.graph.module.add.plan")));
	Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
		TEXT("content.niagara.graph.module.add.apply")));
	Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
		TEXT("content.niagara.graph.module.add.rollback")));
}
}

#endif
