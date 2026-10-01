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
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
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
#include "NiagaraConstants.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraParameterMapHistory.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"

#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
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
		if (ScriptObjectPathsEqual(
			NodeScriptPath(Node),
			RequestedModulePath))
		{
			return true;
		}

		// AddScriptModuleToStack may duplicate the authored module script into a
		// transient graph-owned object.  In that case both the asset-object path
		// and FunctionScript path refer to the duplicate, while the caller still
		// has the authored source path.  Preserve path identity as the first
		// choice above, then use the script asset leaf as a bounded fallback.  A
		// leaf match is deliberately resolved through ModuleNodeMatchesSelector,
		// which still rejects ambiguous selectors instead of silently picking one.
		const FString NormalizedRequested = NormalizeObjectPath(RequestedModulePath);
		if (NormalizedRequested.IsEmpty())
		{
			return false;
		}
		const FString RequestedAssetName = FPackageName::GetShortName(NormalizedRequested);
		if (RequestedAssetName.IsEmpty())
		{
			return false;
		}
		const FString FunctionName = Node->GetFunctionName();
		if (FunctionName.Equals(RequestedAssetName, ESearchCase::IgnoreCase))
		{
			return true;
		}
		return Node->FunctionScript
			&& Node->FunctionScript->GetName().Equals(RequestedAssetName, ESearchCase::IgnoreCase);
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
			const ENiagaraScriptUsage ScriptUsage = Script->GetUsage();
			const ENiagaraScriptUsage OutputUsage = Output ? Output->GetUsage() : ENiagaraScriptUsage::Function;
			const bool bParticleSpawnUsageMatch =
				(ScriptUsage == ENiagaraScriptUsage::ParticleSpawnScriptInterpolated
					&& OutputUsage == ENiagaraScriptUsage::ParticleSpawnScript)
				|| (ScriptUsage == ENiagaraScriptUsage::ParticleSpawnScript
					&& OutputUsage == ENiagaraScriptUsage::ParticleSpawnScriptInterpolated);
			if (Output
				&& (OutputUsage == ScriptUsage || bParticleSpawnUsageMatch)
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
				TEXT(
					"Emitter '%s' matched multiple Niagara stack outputs; specify outputNodePath (an explicit graph selector alone cannot distinguish outputs in the same graph). Candidates: %s"),
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
			OutData.Risks.Add(
				TEXT("The emitter graph or output node is shared/external; shared graph mutation is forbidden."));
		}
		if (OutData.Target.ResolvedTargetIndex == INDEX_NONE)
		{
			OutData.Warnings.Add(TEXT(
				"No insertion index was requested; Niagara will append the module at the end of the selected stack."));
		}
		if (OutData.Target.ExistingModuleCount == 0)
		{
			OutData.Warnings.Add(TEXT(
				"The selected stack has no existing function-call modules; verify the output chain before applying."));
		}
		OutData.Warnings.Add(TEXT(
			"This edit changes the Niagara graph only; runtime ray-tracing scene readiness during level load still requires a separate runtime check."));
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
		Plan->SetStringField(
			TEXT("owningScript"), Data.Target.OwningScript ? Data.Target.OwningScript->GetPathName() : FString());
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
				return ErrorResult(
					TEXT("Unable to compute the Niagara module insertion plan digest."), TEXT("digest_unavailable"),
					500);
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
				return ErrorResult(
					TEXT("A non-empty requestId is required for Niagara module writes."), TEXT("request_id_required"),
					422);
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
				return ErrorResult(
					TEXT("requestId is associated with an unavailable Niagara module receipt."),
					TEXT("request_id_conflict"), 409);
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
				return ErrorResult(
					TEXT("Unable to compute the Niagara module insertion plan digest."), TEXT("digest_unavailable"),
					500);
			}
			if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 409);
			}
			if (Data.bBlocked)
			{
				return ErrorResult(
					TEXT(
						"The Niagara module insertion plan is blocked because the selected graph is shared or read-only."),
					TEXT("plan_blocked"), 409);
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
				return ErrorResult(
					TEXT("The Niagara graph or output changed after the plan was created; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);
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
				return ErrorResult(
					TEXT("Niagara AddScriptModuleToStack returned no node."), TEXT("module_add_failed"), 500);
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
						TEXT(
							"Niagara module insertion failed and restoration could not be verified; the transaction was retained for Editor Undo."),
						TEXT("restore_verification_failed"), 500);
				}
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara module insertion read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
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
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"), 422);
			}
			FModuleReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt)
			{
				return ErrorResult(
					TEXT("The Niagara module receipt is unknown in this Editor instance."), TEXT("receipt_not_found"),
					404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(
					TEXT("requestId does not match the Niagara module receipt."), TEXT("request_id_mismatch"), 409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraNodeFunctionCall* Node = Receipt->Node.Get();
			if (!System || !Graph || !Node)
			{
				return ErrorResult(
					TEXT("The Niagara module target is no longer loaded."), TEXT("target_unavailable"), 409);
			}
			if (Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->GraphChangeIdAfter
				|| !ModuleScriptIdentityMatches(Node, Receipt->ModuleScriptPath)
				|| !AdjacentLinksMatch(*Receipt))
			{
				return ErrorResult(
					TEXT("The Niagara graph or inserted module changed after apply; rollback was refused."),
					TEXT("rollback_conflict"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Stack Module")));
			System->Modify();
			Graph->Modify();
			if (!RemoveInsertedNode(*Receipt))
			{
				Transaction.Cancel();
				return ErrorResult(
					TEXT("The inserted Niagara module could not be removed without overwriting newer links."),
					TEXT("rollback_conflict"), 409);
			}
			FCompileSummary CompileSummary = CompileSystem(System);
			const bool bRemoved = !Graph->Nodes.Contains(Node);
			if (!bRemoved || !CompileSummary.bCompiled)
			{
				return ErrorResult(
					TEXT(
						"Niagara module rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bCompiled = CompileSummary.bCompiled;
			Receipt->CompileStatus = CompileSummary.Status;
			return FMCPToolResult::Ok(MakeResult(*Receipt, false));
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module authoring: list / remove / move.
	//
	// These read/mutate the same parameter-map stack chain as module.add but
	// operate on an already-present function-call module. Like module.add, writes
	// are plan-gated (plan -> apply -> rollback), compile + read-back verified,
	// and never saved (persistence is dirtyOnly). Read queries never compile or
	// save. Only /Game/ authored systems are writable (IsSystemPackageEditable).
	//
	// The engine primitives that move/remove a stack module
	// (FNiagaraStackGraphUtilities::RemoveModuleFromStack / MoveModule /
	// GetStackNodeGroups / DisconnectStackNodeGroup / ConnectStackNodeGroup) are
	// declared without NIAGARAEDITOR_API in UE 5.4 and therefore are not exported
	// from the NiagaraEditor module; only AddScriptModuleToStack is exported. So
	// this file re-implements remove and move as manual parameter-map pin splices,
	// mirroring Monolith's semantics: the removed module (and any upstream
	// ParameterMapSet override node) is spliced out and relinked, and a moved
	// module carries its override node with it so data-input overrides survive the
	// reorder.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxStackModules = 256;

	struct FStackModuleRequest
	{
		FString SystemPath;
		FString EmitterSelector;
		FString GraphSelector;
		FString OutputNodePath;
		FString ModuleSelector;
		int32 TargetIndex = INDEX_NONE;
		bool bHasTargetIndex = false;
	};

	struct FStackModuleResolve
	{
		TWeakObjectPtr<UNiagaraSystem> System;
		FString SystemObjectPath;
		FModuleTarget Target;
		TArray<UNiagaraNode*> OrderedNodes;
		TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
		UNiagaraNodeFunctionCall* Selected = nullptr;
		int32 SelectedIndex = INDEX_NONE;
		FString GraphChangeId;
		bool bBlocked = false;
		TArray<FString> Risks;
		TArray<FString> Warnings;
	};

	struct FRemoveReceipt
	{
		FString ReceiptId;
		FString RequestId;
		FString PlanDigest;
		FString SystemPath;
		FString GraphPath;
		FString OutputPath;
		FString ModuleScriptPath;
		FString NodeName;
		FString NodePath;
		FString GraphChangeIdAfter;
		int32 OriginalIndex = INDEX_NONE;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TWeakObjectPtr<UNiagaraNodeOutput> OutputNode;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
	};

	struct FMoveReceipt
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
		int32 OriginalIndex = INDEX_NONE;
		int32 NewIndex = INDEX_NONE;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TWeakObjectPtr<UNiagaraNodeOutput> OutputNode;
		TWeakObjectPtr<UNiagaraNodeFunctionCall> Node;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
	};

	TMap<FString, FRemoveReceipt>& RemoveReceipts()
	{
		static TMap<FString, FRemoveReceipt> Values;
		return Values;
	}

	TMap<FString, FString>& RemoveRequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	TMap<FString, FMoveReceipt>& MoveReceipts()
	{
		static TMap<FString, FMoveReceipt> Values;
		return Values;
	}

	TMap<FString, FString>& MoveRequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	bool ParseStackRequest(
		const TSharedPtr<FJsonObject>& Params,
		FStackModuleRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutRequest = FStackModuleRequest();
		if (!Params.IsValid())
		{
			OutErrorCode = TEXT("invalid_request");
			OutError = TEXT("A Niagara stack module request is required.");
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
			OutError = TEXT("An explicit emitter selector is required.");
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
		Params->TryGetStringField(TEXT("moduleSelector"), OutRequest.ModuleSelector);
		OutRequest.EmitterSelector = OutRequest.EmitterSelector.TrimStartAndEnd();
		OutRequest.GraphSelector = OutRequest.GraphSelector.TrimStartAndEnd();
		OutRequest.OutputNodePath = OutRequest.OutputNodePath.TrimStartAndEnd();
		OutRequest.ModuleSelector = OutRequest.ModuleSelector.TrimStartAndEnd();

		if (Params->HasField(TEXT("targetIndex")))
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

	bool LoadStackSystem(
		const FStackModuleRequest& Request,
		UNiagaraSystem*& OutSystem,
		FString& OutSystemPath,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutSystemPath = NormalizeObjectPath(Request.SystemPath);
		if (OutSystemPath.IsEmpty())
		{
			OutErrorCode = TEXT("invalid_object_path");
			OutError = TEXT("system must be a valid object or package path.");
			return false;
		}
		OutSystem = LoadObject<UNiagaraSystem>(nullptr, *OutSystemPath);
		if (!OutSystem)
		{
			OutErrorCode = TEXT("system_not_found");
			OutError = FString::Printf(TEXT("Niagara System '%s' was not found."), *Request.SystemPath);
			return false;
		}
		return true;
	}

	bool ModuleNodeMatchesSelector(UNiagaraNodeFunctionCall* Node, const FString& Selector)
	{
		if (!Node || Selector.IsEmpty())
		{
			return false;
		}
		FGuid RequestedGuid;
		if (FGuid::Parse(Selector, RequestedGuid) && Node->NodeGuid == RequestedGuid)
		{
			return true;
		}
		return MatchesSelector(Selector, Node->GetName(), Node->GetPathName())
			|| MatchesSelector(Selector, Node->GetFunctionName(), Node->GetPathName())
			|| ModuleScriptIdentityMatches(Node, Selector);
	}

	bool ResolveStackModule(
		const FStackModuleRequest& Request,
		FStackModuleResolve& OutResolve,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutResolve = FStackModuleResolve();
		UNiagaraSystem* System = nullptr;
		if (!LoadStackSystem(Request, System, OutResolve.SystemObjectPath, OutErrorCode, OutError))
		{
			return false;
		}
		OutResolve.System = System;

		FModuleRequest TargetRequest;
		TargetRequest.EmitterSelector = Request.EmitterSelector;
		TargetRequest.GraphSelector = Request.GraphSelector;
		TargetRequest.OutputNodePath = Request.OutputNodePath;
		if (!FindTarget(System, TargetRequest, OutResolve.Target, OutErrorCode, OutError))
		{
			return false;
		}

		if (!BuildStackOrder(OutResolve.Target.OutputNode, OutResolve.OrderedNodes, OutError))
		{
			OutErrorCode = TEXT("invalid_stack");
			return false;
		}

		int32 ModuleIndex = 0;
		int32 MatchCount = 0;
		for (UNiagaraNode* Node : OutResolve.OrderedNodes)
		{
			if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
			{
				OutResolve.ModuleNodes.Add(FunctionCall);
				if (!Request.ModuleSelector.IsEmpty()
					&& ModuleNodeMatchesSelector(FunctionCall, Request.ModuleSelector))
				{
					++MatchCount;
					if (!OutResolve.Selected)
					{
						OutResolve.Selected = FunctionCall;
						OutResolve.SelectedIndex = ModuleIndex;
					}
				}
				++ModuleIndex;
			}
		}

		if (!Request.ModuleSelector.IsEmpty() && !OutResolve.Selected)
		{
			OutErrorCode = TEXT("module_not_found");
			OutError = FString::Printf(
				TEXT("No stack module matched selector '%s' in graph '%s'."),
				*Request.ModuleSelector,
				*OutResolve.Target.GraphPath);
			return false;
		}
		if (MatchCount > 1)
		{
			OutErrorCode = TEXT("ambiguous_module");
			OutError = FString::Printf(
				TEXT("moduleSelector '%s' matched %d stack modules; use a nodePath or a more specific name."),
				*Request.ModuleSelector,
				MatchCount);
			return false;
		}

		OutResolve.GraphChangeId = OutResolve.Target.Graph
			                           ? OutResolve.Target.Graph->GetChangeID().ToString(
				                           EGuidFormats::DigitsWithHyphensLower)
			                           : FString();
		OutResolve.bBlocked = !OutResolve.Target.bEditable || !IsSystemPackageEditable(System);
		if (!IsSystemPackageEditable(System))
		{
			OutResolve.Risks.Add(
				TEXT("The selected Niagara System is outside /Game/ and is read-only for this command."));
		}
		if (!OutResolve.Target.bEditable)
		{
			OutResolve.Risks.Add(
				TEXT("The emitter graph or output node is shared/external; shared graph mutation is forbidden."));
		}
		return true;
	}

	int32 FindModuleIndexInOrder(UNiagaraNodeOutput* Output, UNiagaraNodeFunctionCall* Node)
	{
		if (!Output || !Node)
		{
			return INDEX_NONE;
		}
		TArray<UNiagaraNode*> Ordered;
		FString Error;
		if (!BuildStackOrder(Output, Ordered, Error))
		{
			return INDEX_NONE;
		}
		int32 Index = 0;
		for (UNiagaraNode* OrderedNode : Ordered)
		{
			if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(OrderedNode))
			{
				if (FunctionCall == Node)
				{
					return Index;
				}
				++Index;
			}
		}
		return INDEX_NONE;
	}

	// A ParameterMapSet override node is structurally a UNiagaraNode that is not a
	// FunctionCall and has ParameterMap pins on both the input and output side
	// (the stack input connector only has an output pin).
	bool IsStackOverrideNode(UEdGraphNode* Node)
	{
		if (!Node || Cast<UNiagaraNodeFunctionCall>(Node))
		{
			return false;
		}
		UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node);
		return NiagaraNode
			&& FindMapPin(NiagaraNode, EGPD_Input) != nullptr
			&& FindMapPin(NiagaraNode, EGPD_Output) != nullptr;
	}

	// The node a stack chain link must connect TO when wiring a module's group
	// start: its upstream override node when present, else the module itself.
	UNiagaraNode* StackGroupStart(UNiagaraNode* EndNode)
	{
		if (!EndNode || !Cast<UNiagaraNodeFunctionCall>(EndNode))
		{
			return EndNode;
		}
		UEdGraphPin* MapIn = FindMapPin(EndNode, EGPD_Input);
		if (MapIn && MapIn->LinkedTo.Num() > 0)
		{
			UNiagaraNode* Upstream = Cast<UNiagaraNode>(MapIn->LinkedTo[0]->GetOwningNodeUnchecked());
			if (IsStackOverrideNode(Upstream))
			{
				return Upstream;
			}
		}
		return EndNode;
	}

	// Splice the function-call module out of the parameter-map chain, relinking the
	// real previous node to the next node, and removing any upstream
	// ParameterMapSet override node alongside it. Mirrors Monolith's
	// RemoveModuleFromStack semantics using only exported pin primitives.
	bool SpliceOutStackNode(UNiagaraGraph* Graph, UNiagaraNodeFunctionCall* Node)
	{
		if (!Graph || !Node)
		{
			return false;
		}
		UEdGraphPin* TargetMapIn = FindMapPin(Node, EGPD_Input);
		UEdGraphPin* TargetMapOut = FindMapPin(Node, EGPD_Output);
		if (!TargetMapIn || !TargetMapOut)
		{
			return false;
		}

		UEdGraphPin* UpstreamOutputPin = TargetMapIn->LinkedTo.Num() > 0 ? TargetMapIn->LinkedTo[0] : nullptr;
		UEdGraphPin* DownstreamInputPin = TargetMapOut->LinkedTo.Num() > 0 ? TargetMapOut->LinkedTo[0] : nullptr;

		UEdGraphNode* OverrideNode = nullptr;
		if (UpstreamOutputPin)
		{
			UEdGraphNode* UpstreamNode = UpstreamOutputPin->GetOwningNodeUnchecked();
			UNiagaraNode* UpstreamNiagara = Cast<UNiagaraNode>(UpstreamNode);
			if (UpstreamNiagara
				&& !Cast<UNiagaraNodeFunctionCall>(UpstreamNode)
				&& FindMapPin(UpstreamNiagara, EGPD_Input)
				&& FindMapPin(UpstreamNiagara, EGPD_Output))
			{
				OverrideNode = UpstreamNode;
				UEdGraphPin* OverrideMapIn = FindMapPin(UpstreamNiagara, EGPD_Input);
				UpstreamOutputPin = OverrideMapIn->LinkedTo.Num() > 0 ? OverrideMapIn->LinkedTo[0] : nullptr;
			}
		}

		TargetMapIn->BreakAllPinLinks();
		TargetMapOut->BreakAllPinLinks();
		if (UpstreamOutputPin && DownstreamInputPin)
		{
			DownstreamInputPin->MakeLinkTo(UpstreamOutputPin);
		}
		if (OverrideNode)
		{
			OverrideNode->Modify();
			OverrideNode->BreakAllNodeLinks();
			Graph->RemoveNode(OverrideNode);
		}
		Node->Modify();
		Node->BreakAllNodeLinks();
		Graph->RemoveNode(Node);
		Graph->NotifyGraphChanged();
		return !Graph->Nodes.Contains(Node);
	}

	// Move a function-call module to a new zero-based stack index by detaching its
	// group (module plus any upstream override node) and re-wiring it between the
	// new neighbors. The node object is preserved, so its pins (including static
	// switch and data-input override pins) survive the move.
	bool MoveStackModule(
		UNiagaraGraph* Graph,
		UNiagaraNodeOutput* Output,
		UNiagaraNodeFunctionCall* Node,
		int32 CurrentIndex,
		int32 TargetIndex,
		FString& OutError)
	{
		OutError.Reset();
		if (!Graph || !Output || !Node)
		{
			OutError = TEXT("The Niagara move target is unavailable.");
			return false;
		}
		if (CurrentIndex == TargetIndex)
		{
			return true;
		}

		UNiagaraNode* UnitStart = StackGroupStart(Node);
		if (!UnitStart)
		{
			OutError = TEXT("The module stack group start is unavailable.");
			return false;
		}
		UEdGraphPin* UnitStartMapIn = FindMapPin(UnitStart, EGPD_Input);
		UEdGraphPin* ModuleMapOut = FindMapPin(Node, EGPD_Output);
		if (!UnitStartMapIn || !ModuleMapOut
			|| UnitStartMapIn->LinkedTo.Num() != 1 || ModuleMapOut->LinkedTo.Num() != 1)
		{
			OutError = TEXT("The module stack link is ambiguous or missing; refusing to splice.");
			return false;
		}
		UNiagaraNode* UnitPrev = Cast<UNiagaraNode>(UnitStartMapIn->LinkedTo[0]->GetOwningNodeUnchecked());
		UNiagaraNode* UnitNext = Cast<UNiagaraNode>(ModuleMapOut->LinkedTo[0]->GetOwningNodeUnchecked());
		if (!UnitPrev || !UnitNext)
		{
			OutError = TEXT("The module stack neighbors are unavailable.");
			return false;
		}
		UNiagaraNode* UnitNextStart = StackGroupStart(UnitNext);
		UEdGraphPin* UnitPrevMapOut = FindMapPin(UnitPrev, EGPD_Output);
		UEdGraphPin* UnitNextStartMapIn = FindMapPin(UnitNextStart, EGPD_Input);
		if (!UnitPrevMapOut || !UnitNextStartMapIn)
		{
			OutError = TEXT("The module stack neighbor pins are unavailable.");
			return false;
		}

		// Detach the unit (override node + module), relinking previous -> next.
		Node->Modify();
		if (UnitStart != Node)
		{
			UnitStart->Modify();
		}
		UnitStartMapIn->BreakAllPinLinks();
		ModuleMapOut->BreakAllPinLinks();
		UnitPrevMapOut->BreakAllPinLinks();
		UnitPrevMapOut->MakeLinkTo(UnitNextStartMapIn);
		Graph->NotifyGraphChanged();

		// Rebuild the remaining module order (the moved module is out of the chain).
		TArray<UNiagaraNode*> Ordered;
		if (!BuildStackOrder(Output, Ordered, OutError))
		{
			return false;
		}
		TArray<UNiagaraNodeFunctionCall*> Remaining;
		for (UNiagaraNode* OrderedNode : Ordered)
		{
			if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(OrderedNode))
			{
				Remaining.Add(FunctionCall);
			}
		}
		const int32 RemainingCount = Remaining.Num();
		if (TargetIndex < 0 || TargetIndex > RemainingCount)
		{
			OutError = FString::Printf(
				TEXT("targetIndex %d is outside the reordered stack range."),
				TargetIndex);
			return false;
		}

		UNiagaraNode* NewPrev = TargetIndex == 0
			                        ? static_cast<UNiagaraNode*>(Ordered[0])
			                        : static_cast<UNiagaraNode*>(Remaining[TargetIndex - 1]);
		UNiagaraNode* NewNext = TargetIndex == RemainingCount
			                        ? static_cast<UNiagaraNode*>(Output)
			                        : static_cast<UNiagaraNode*>(Remaining[TargetIndex]);
		UNiagaraNode* NewNextStart = StackGroupStart(NewNext);
		UEdGraphPin* NewPrevMapOut = FindMapPin(NewPrev, EGPD_Output);
		UEdGraphPin* NewNextStartMapIn = FindMapPin(NewNextStart, EGPD_Input);
		if (!NewPrevMapOut || !NewNextStartMapIn)
		{
			OutError = TEXT("The reordered stack insertion pins are unavailable.");
			return false;
		}

		NewPrevMapOut->BreakAllPinLinks();
		NewPrevMapOut->MakeLinkTo(UnitStartMapIn);
		ModuleMapOut->MakeLinkTo(NewNextStartMapIn);
		Graph->NotifyGraphChanged();
		return true;
	}

	TSharedRef<FJsonObject> BuildRemovePlanJson(const FStackModuleResolve& Resolve)
	{
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
		Plan->SetStringField(TEXT("planKind"), TEXT("niagaraModuleRemove"));
		Plan->SetStringField(TEXT("action"), TEXT("removeModule"));
		Plan->SetStringField(TEXT("scope"), Resolve.SystemObjectPath);
		Plan->SetStringField(TEXT("status"), TEXT("planned"));
		Plan->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
		Plan->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
		Plan->SetStringField(TEXT("emitterPath"), Resolve.Target.EmitterPath);
		Plan->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
		Plan->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
		Plan->SetStringField(TEXT("scriptUsage"), Resolve.Target.ScriptUsage);
		Plan->SetStringField(TEXT("moduleScript"), NodeScriptPath(Resolve.Selected));
		Plan->SetStringField(TEXT("nodePath"), Resolve.Selected->GetPathName());
		Plan->SetStringField(TEXT("nodeName"), Resolve.Selected->GetName());
		Plan->SetStringField(TEXT("graphChangeId"), Resolve.GraphChangeId);
		Plan->SetNumberField(TEXT("nodeIndex"), Resolve.SelectedIndex);
		Plan->SetNumberField(TEXT("moduleCount"), Resolve.ModuleNodes.Num());
		Plan->SetBoolField(TEXT("editable"), Resolve.Target.bEditable);
		Plan->SetBoolField(TEXT("blocked"), Resolve.bBlocked);
		Plan->SetBoolField(TEXT("changesState"), !Resolve.bBlocked);
		Plan->SetStringField(TEXT("risk"), Resolve.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
		Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
		Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));

		TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
		Preconditions->SetStringField(TEXT("graphChangeId"), Resolve.GraphChangeId);
		Preconditions->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
		Preconditions->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
		Plan->SetObjectField(TEXT("preconditions"), Preconditions);

		TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetBoolField(TEXT("modulePresent"), true);
		Before->SetStringField(TEXT("moduleScript"), NodeScriptPath(Resolve.Selected));
		Before->SetStringField(TEXT("nodePath"), Resolve.Selected->GetPathName());
		Before->SetNumberField(TEXT("nodeIndex"), Resolve.SelectedIndex);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetBoolField(TEXT("modulePresent"), false);
		Plan->SetObjectField(TEXT("before"), Before);
		Plan->SetObjectField(TEXT("after"), After);

		TArray<TSharedPtr<FJsonValue>> Risks;
		for (const FString& Risk : Resolve.Risks)
		{
			Risks.Add(MakeShared<FJsonValueString>(Risk));
		}
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Resolve.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warning));
		}
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("Removal changes the Niagara graph only; it does not save or verify runtime scene state.")));
		Plan->SetArrayField(TEXT("risks"), Risks);
		Plan->SetArrayField(TEXT("warnings"), Warnings);
		return Plan;
	}

	TSharedRef<FJsonObject> BuildMovePlanJson(const FStackModuleResolve& Resolve, const int32 TargetIndex)
	{
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
		Plan->SetStringField(TEXT("planKind"), TEXT("niagaraModuleMove"));
		Plan->SetStringField(TEXT("action"), TEXT("moveModule"));
		Plan->SetStringField(TEXT("scope"), Resolve.SystemObjectPath);
		Plan->SetStringField(TEXT("status"), TEXT("planned"));
		Plan->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
		Plan->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
		Plan->SetStringField(TEXT("emitterPath"), Resolve.Target.EmitterPath);
		Plan->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
		Plan->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
		Plan->SetStringField(TEXT("scriptUsage"), Resolve.Target.ScriptUsage);
		Plan->SetStringField(TEXT("moduleScript"), NodeScriptPath(Resolve.Selected));
		Plan->SetStringField(TEXT("nodePath"), Resolve.Selected->GetPathName());
		Plan->SetStringField(TEXT("nodeName"), Resolve.Selected->GetName());
		Plan->SetStringField(TEXT("graphChangeId"), Resolve.GraphChangeId);
		Plan->SetNumberField(TEXT("beforeIndex"), Resolve.SelectedIndex);
		Plan->SetNumberField(TEXT("afterIndex"), TargetIndex);
		Plan->SetNumberField(TEXT("targetIndex"), TargetIndex);
		Plan->SetNumberField(TEXT("moduleCount"), Resolve.ModuleNodes.Num());
		Plan->SetBoolField(TEXT("editable"), Resolve.Target.bEditable);
		Plan->SetBoolField(TEXT("blocked"), Resolve.bBlocked);
		Plan->SetBoolField(TEXT("changesState"), !Resolve.bBlocked);
		Plan->SetStringField(TEXT("risk"), Resolve.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
		Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
		Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));

		TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
		Preconditions->SetStringField(TEXT("graphChangeId"), Resolve.GraphChangeId);
		Preconditions->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
		Preconditions->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
		Plan->SetObjectField(TEXT("preconditions"), Preconditions);

		TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetNumberField(TEXT("index"), Resolve.SelectedIndex);
		Before->SetStringField(TEXT("nodePath"), Resolve.Selected->GetPathName());
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetNumberField(TEXT("index"), TargetIndex);
		After->SetStringField(TEXT("nodePath"), Resolve.Selected->GetPathName());
		Plan->SetObjectField(TEXT("before"), Before);
		Plan->SetObjectField(TEXT("after"), After);

		TArray<TSharedPtr<FJsonValue>> Risks;
		for (const FString& Risk : Resolve.Risks)
		{
			Risks.Add(MakeShared<FJsonValueString>(Risk));
		}
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Resolve.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warning));
		}
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT(
				"Moving a module reorders execution and preserves the node and its override pins; overridden data inputs move with the module.")));
		Plan->SetArrayField(TEXT("risks"), Risks);
		Plan->SetArrayField(TEXT("warnings"), Warnings);
		return Plan;
	}

	TSharedRef<FJsonObject> MakeRemoveResult(const FRemoveReceipt& Receipt, const bool bReplay)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-module-remove.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("rollbackId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("outputNodePath"), Receipt.OutputPath);
		Result->SetStringField(TEXT("moduleScript"), Receipt.ModuleScriptPath);
		Result->SetStringField(TEXT("nodePath"), Receipt.NodePath);
		Result->SetStringField(TEXT("nodeName"), Receipt.NodeName);
		Result->SetNumberField(TEXT("removedIndex"), Receipt.OriginalIndex);
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

	TSharedRef<FJsonObject> MakeMoveResult(const FMoveReceipt& Receipt, const bool bReplay)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-module-move.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("rollbackId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("outputNodePath"), Receipt.OutputPath);
		Result->SetStringField(TEXT("moduleScript"), Receipt.ModuleScriptPath);
		Result->SetStringField(TEXT("nodePath"), Receipt.NodePath);
		Result->SetNumberField(TEXT("beforeIndex"), Receipt.OriginalIndex);
		Result->SetNumberField(TEXT("afterIndex"), Receipt.NewIndex);
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

	class FTool_NiagaraModuleList final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.list");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-module-list.v1"));
			Result->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
			Result->SetStringField(TEXT("emitterPath"), Resolve.Target.EmitterPath);
			Result->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
			Result->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
			Result->SetStringField(TEXT("scriptUsage"), Resolve.Target.ScriptUsage);
			Result->SetNumberField(TEXT("moduleCount"), Resolve.ModuleNodes.Num());
			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetNumberField(TEXT("moduleLimit"), MaxStackModules);

			TArray<TSharedPtr<FJsonValue>> Modules;
			const int32 Count = FMath::Min(Resolve.ModuleNodes.Num(), MaxStackModules);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				UNiagaraNodeFunctionCall* Node = Resolve.ModuleNodes[Index];
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("moduleScript"), NodeScriptPath(Node));
				Entry->SetStringField(TEXT("nodePath"), Node->GetPathName());
				Entry->SetStringField(TEXT("name"), Node->GetName());
				Entry->SetStringField(TEXT("functionName"), Node->GetFunctionName());
				Entry->SetBoolField(TEXT("enabled"), Node->IsNodeEnabled());
				Entry->SetNumberField(TEXT("index"), Index);
				Modules.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetArrayField(TEXT("modules"), Modules);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), false);
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module input read-back: content.niagara.graph.module.inputs.list.
	//
	// Read-only enumeration of a resolved stack module's authored inputs via the
	// exported FNiagaraStackGraphUtilities::GetStackFunctionInputs. The module is
	// resolved with ResolveStackModule (sharing its module_not_found /
	// ambiguous_module semantics), so resolution is not reimplemented here. The
	// query never compiles, never saves, and never verifies runtime/PIE state: the
	// input set reflects the authored graph only.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxStackInputs = 256;

	// These helpers are declared before the read command because the override
	// pin implementation lives with the write commands below.  Keeping the
	// serialization here makes the query and the later spec export report the
	// same authored source categories (default, literal, binding, or dynamic
	// input) instead of exposing only a pin's raw DefaultValue.
	UEdGraphPin* FindExistingOverridePin(UNiagaraNodeFunctionCall* Node, FName AliasedPinName);
	FNiagaraParameterHandle AliasedInputHandle(UNiagaraNodeFunctionCall* Node, FName FullName);
	void GetDynamicInputChildBindings(
		UNiagaraNodeFunctionCall* Parent,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		TArray<TPair<FString, UNiagaraNodeFunctionCall*>>& OutChildren);
	void GetDynamicInputChildren(
		UNiagaraNodeFunctionCall* Parent,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		TArray<UNiagaraNodeFunctionCall*>& OutChildren);
	bool FindDynamicInputInSubtree(
		UNiagaraNodeFunctionCall* Root,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		const FGuid& WantedGuid,
		UNiagaraNodeFunctionCall*& OutNode,
		TSet<UNiagaraNodeFunctionCall*>* OutSubtree = nullptr);
	void CollectDynamicInputSubtree(
		UNiagaraNodeFunctionCall* Root,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		TSet<UNiagaraNodeFunctionCall*>& OutNodes);

	// Niagara's exported linked-value helper is not available to external modules
	// in UE 5.4.  Reproduce its read-only structural check without including the
	// private NiagaraNodeParameterMapGet class header.  The output pin on the
	// ParameterMapGet node carries the full linked parameter name.
	UEdGraphPin* FindLinkedParameterPin(UEdGraphPin* OverridePin)
	{
		if (!OverridePin || OverridePin->LinkedTo.Num() != 1)
		{
			return nullptr;
		}

		UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
		if (!LinkedPin || LinkedPin->Direction != EGPD_Output)
		{
			return nullptr;
		}

		const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNodeUnchecked();
		if (!LinkedNode || LinkedNode->GetClass()->GetFName() != TEXT("NiagaraNodeParameterMapGet"))
		{
			return nullptr;
		}

		FNiagaraParameterHandle LinkedHandle(LinkedPin->PinName);
		return LinkedHandle.IsValid() && !LinkedHandle.GetNamespace().IsNone()
			       ? LinkedPin
			       : nullptr;
	}

	void AddAuthoredInputState(
		UNiagaraNodeFunctionCall* Node,
		const FNiagaraVariable& Input,
		const TSharedRef<FJsonObject>& Entry)
	{
		Entry->SetBoolField(TEXT("hasOverride"), false);
		Entry->SetBoolField(TEXT("isLinked"), false);
		Entry->SetBoolField(TEXT("isDefault"), true);

		if (!Node)
		{
			Entry->SetStringField(TEXT("source"), TEXT("default"));
			return;
		}

		const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Input.GetName());
		UEdGraphPin* OverridePin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
		if (!OverridePin)
		{
			Entry->SetStringField(TEXT("source"), TEXT("default"));
			return;
		}

		Entry->SetBoolField(TEXT("hasOverride"), true);
		Entry->SetStringField(TEXT("overridePin"), OverridePin->GetName());
		if (OverridePin->LinkedTo.Num() > 0)
		{
			Entry->SetBoolField(TEXT("isLinked"), true);
			Entry->SetBoolField(TEXT("isDefault"), false);
			Entry->SetNumberField(TEXT("linkedPinCount"), OverridePin->LinkedTo.Num());

			UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
			if (!LinkedPin)
			{
				Entry->SetStringField(TEXT("source"), TEXT("linked"));
				return;
			}

			if (UNiagaraNodeFunctionCall* DynamicNode = Cast<UNiagaraNodeFunctionCall>(
				LinkedPin->GetOwningNodeUnchecked()))
			{
				if (DynamicNode->FunctionScript
					&& DynamicNode->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput)
				{
					Entry->SetStringField(TEXT("source"), TEXT("dynamicInput"));
					Entry->SetStringField(TEXT("dynamicInputName"), DynamicNode->GetFunctionName());
					Entry->SetStringField(
						TEXT("dynamicInputGuid"),
						DynamicNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					Entry->SetStringField(TEXT("dynamicInputPath"), NodeScriptPath(DynamicNode));
					return;
				}
			}

			if (UEdGraphPin* LinkedParameterPin = FindLinkedParameterPin(OverridePin))
			{
				Entry->SetStringField(TEXT("source"), TEXT("parameterBinding"));
				Entry->SetStringField(TEXT("linkedParameter"), LinkedParameterPin->PinName.ToString());
				return;
			}

			if (const UNiagaraNodeInput* LinkedInput = Cast<UNiagaraNodeInput>(
				LinkedPin->GetOwningNodeUnchecked()))
			{
				Entry->SetStringField(TEXT("source"), TEXT("parameterBinding"));
				Entry->SetStringField(TEXT("linkedParameter"), LinkedInput->Input.GetName().ToString());
				return;
			}

			Entry->SetStringField(TEXT("source"), TEXT("linked"));
			Entry->SetStringField(TEXT("linkedPin"), LinkedPin->PinName.ToString());
			if (const UEdGraphNode* LinkedNode = LinkedPin->GetOwningNodeUnchecked())
			{
				Entry->SetStringField(TEXT("linkedNodeClass"), LinkedNode->GetClass()->GetName());
			}
			return;
		}

		Entry->SetStringField(TEXT("source"), TEXT("literal"));
		Entry->SetStringField(TEXT("value"), OverridePin->DefaultValue);
		Entry->SetBoolField(TEXT("isDefault"), OverridePin->DefaultValue.IsEmpty());
	}

	class FTool_NiagaraModuleInputsList final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.inputs.list");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to read a stack module's inputs."),
					TEXT("module_selector_required"), 422);
			}
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			UNiagaraNodeFunctionCall* Node = Resolve.Selected;
			if (!Node)
			{
				return ErrorResult(TEXT("The selected stack module is unavailable."), TEXT("module_unavailable"), 409);
			}

			// Resolve static-switch constants against the owning emitter when it can
			// be located, falling back to the system, mirroring the editor's stack
			// input enumeration. The usage is the parent script that owns the output
			// node, matching FNiagaraStackGraphUtilities' own call sites.
			const ENiagaraScriptUsage Usage = Resolve.Target.OwningScript
				                                  ? Resolve.Target.OwningScript->GetUsage()
				                                  : ENiagaraScriptUsage::Function;
			FCompileConstantResolver ConstantResolver(Resolve.System.Get(), Usage);
			if (UNiagaraSystem* System = Resolve.System.Get())
			{
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					if (Handle.GetName().ToString().Equals(Resolve.Target.EmitterName, ESearchCase::CaseSensitive))
					{
						ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
						break;
					}
				}
			}

			TArray<FNiagaraVariable> InputVariables;
			TSet<FNiagaraVariable> HiddenVariables;
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(
				*Node,
				InputVariables,
				HiddenVariables,
				ConstantResolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.module-inputs.v1"));
			Result->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
			Result->SetStringField(TEXT("emitterPath"), Resolve.Target.EmitterPath);
			Result->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
			Result->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
			Result->SetStringField(TEXT("scriptUsage"), Resolve.Target.ScriptUsage);
			Result->SetStringField(TEXT("moduleSelector"), Request.ModuleSelector);
			Result->SetStringField(TEXT("moduleScript"), NodeScriptPath(Node));
			Result->SetStringField(TEXT("nodePath"), Node->GetPathName());
			Result->SetNumberField(TEXT("inputCount"), InputVariables.Num());
			Result->SetNumberField(TEXT("hiddenCount"), HiddenVariables.Num());
			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetNumberField(TEXT("inputLimit"), MaxStackInputs);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), false);
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			Result->SetStringField(
				TEXT("scope"),
				TEXT("Authored-graph input readback only; does not compile, save, or verify runtime/PIE state."));

			TArray<TSharedPtr<FJsonValue>> Inputs;
			const int32 Count = FMath::Min(InputVariables.Num(), MaxStackInputs);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FNiagaraVariable& Input = InputVariables[Index];
				FString InputName = Input.GetName().ToString();
				InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), InputName);
				Entry->SetStringField(TEXT("type"), Input.GetType().GetName());
				Entry->SetBoolField(TEXT("hidden"), HiddenVariables.Contains(Input));
				Entry->SetBoolField(TEXT("dataInterface"), Input.IsDataInterface() || Input.IsUObject());
				AddAuthoredInputState(Node, Input, Entry);
				Inputs.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetArrayField(TEXT("inputs"), Inputs);
			Result->SetBoolField(TEXT("inputsTruncated"), InputVariables.Num() > MaxStackInputs);

			TArray<UEdGraphPin*> StaticSwitchPins;
			TSet<UEdGraphPin*> HiddenSwitchPins;
			FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
				*Node,
				StaticSwitchPins,
				HiddenSwitchPins,
				ConstantResolver);
			TArray<TSharedPtr<FJsonValue>> StaticSwitches;
			const int32 StaticSwitchCount = FMath::Min(StaticSwitchPins.Num(), MaxStackInputs);
			StaticSwitches.Reserve(StaticSwitchCount);
			for (int32 Index = 0; Index < StaticSwitchCount; ++Index)
			{
				UEdGraphPin* SwitchPin = StaticSwitchPins[Index];
				if (!SwitchPin)
				{
					continue;
				}
				const FNiagaraTypeDefinition SwitchType = UEdGraphSchema_Niagara::PinToTypeDefinition(SwitchPin);
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), SwitchPin->GetName());
				Entry->SetStringField(TEXT("type"), SwitchType.GetName());
				Entry->SetBoolField(TEXT("hidden"), HiddenSwitchPins.Contains(SwitchPin));
				Entry->SetBoolField(TEXT("staticSwitch"), true);
				Entry->SetStringField(TEXT("source"), TEXT("staticSwitch"));
				Entry->SetStringField(TEXT("value"), SwitchPin->DefaultValue);
				Entry->SetStringField(TEXT("defaultValue"), SwitchPin->AutogeneratedDefaultValue);
				const bool bMatchesDefault = SwitchPin->DoesDefaultValueMatchAutogenerated();
				Entry->SetBoolField(TEXT("hasOverride"), !bMatchesDefault);
				Entry->SetBoolField(TEXT("isLinked"), false);
				Entry->SetBoolField(TEXT("isDefault"), bMatchesDefault);
				StaticSwitches.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetNumberField(TEXT("staticSwitchCount"), StaticSwitchPins.Num());
			Result->SetBoolField(TEXT("staticSwitchesTruncated"), StaticSwitchPins.Num() > MaxStackInputs);
			Result->SetArrayField(TEXT("staticSwitches"), StaticSwitches);
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module dynamic-input read-back:
	// content.niagara.graph.module.dynamic_inputs.list.
	//
	// Niagara stores a module input's authored dynamic input on the override pin
	// owned by the stack's ParameterMapSet node. This query follows those links
	// and reports only function-call nodes whose script is a DynamicInput script.
	// It deliberately does not compile, save, or mutate the graph.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxStackDynamicInputs = 256;

	class FTool_NiagaraModuleDynamicInputsList final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.dynamic_inputs.list");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to list a stack module's dynamic inputs."),
					TEXT("module_selector_required"),
					422);
			}

			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			UNiagaraNodeFunctionCall* Node = Resolve.Selected;
			if (!Node)
			{
				return ErrorResult(TEXT("The selected stack module is unavailable."), TEXT("module_unavailable"), 409);
			}

			const ENiagaraScriptUsage Usage = Resolve.Target.OwningScript
				                                  ? Resolve.Target.OwningScript->GetUsage()
				                                  : ENiagaraScriptUsage::Function;
			FCompileConstantResolver ConstantResolver(Resolve.System.Get(), Usage);
			if (UNiagaraSystem* System = Resolve.System.Get())
			{
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					if (Handle.GetName().ToString().Equals(Resolve.Target.EmitterName, ESearchCase::CaseSensitive))
					{
						ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
						break;
					}
				}
			}

			TArray<FNiagaraVariable> InputVariables;
			TSet<FNiagaraVariable> HiddenVariables;
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(
				*Node,
				InputVariables,
				HiddenVariables,
				ConstantResolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

			TArray<TSharedPtr<FJsonValue>> DynamicInputs;
			int32 DynamicInputCount = 0;
			const int32 InputCount = FMath::Min(InputVariables.Num(), MaxStackDynamicInputs);
			for (int32 InputIndex = 0; InputIndex < InputCount; ++InputIndex)
			{
				const FNiagaraVariable& Input = InputVariables[InputIndex];
				const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Input.GetName());
				UEdGraphPin* OverridePin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
				if (!OverridePin || OverridePin->LinkedTo.Num() == 0)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}
					UNiagaraNodeFunctionCall* DynamicNode = Cast<UNiagaraNodeFunctionCall>(
						LinkedPin->GetOwningNodeUnchecked());
					if (!DynamicNode
						|| !DynamicNode->FunctionScript
						|| DynamicNode->FunctionScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
					{
						continue;
					}

					++DynamicInputCount;
					if (DynamicInputs.Num() >= MaxStackDynamicInputs)
					{
						continue;
					}

					FString InputName = Input.GetName().ToString();
					InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("inputName"), InputName);
					Entry->SetStringField(TEXT("inputType"), Input.GetType().GetName());
					Entry->SetStringField(TEXT("dynamicInputName"), DynamicNode->GetFunctionName());
					Entry->SetStringField(
						TEXT("dynamicInputGuid"),
						DynamicNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
					Entry->SetStringField(TEXT("dynamicInputPath"), NodeScriptPath(DynamicNode));
					Entry->SetStringField(TEXT("nodePath"), DynamicNode->GetPathName());
					DynamicInputs.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.module-dynamic-inputs.v1"));
			Result->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
			Result->SetStringField(TEXT("emitterPath"), Resolve.Target.EmitterPath);
			Result->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
			Result->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
			Result->SetStringField(TEXT("scriptUsage"), Resolve.Target.ScriptUsage);
			Result->SetStringField(TEXT("moduleSelector"), Request.ModuleSelector);
			Result->SetStringField(TEXT("moduleScript"), NodeScriptPath(Node));
			Result->SetStringField(TEXT("nodePath"), Node->GetPathName());
			Result->SetNumberField(TEXT("inputCount"), InputVariables.Num());
			Result->SetNumberField(TEXT("inputLimit"), MaxStackDynamicInputs);
			Result->SetBoolField(TEXT("inputsTruncated"), InputVariables.Num() > MaxStackDynamicInputs);
			Result->SetNumberField(TEXT("dynamicInputCount"), DynamicInputCount);
			Result->SetNumberField(TEXT("dynamicInputLimit"), MaxStackDynamicInputs);
			Result->SetBoolField(
				TEXT("dynamicInputsTruncated"),
				InputVariables.Num() > MaxStackDynamicInputs || DynamicInputCount > MaxStackDynamicInputs);
			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), false);
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			Result->SetStringField(
				TEXT("scope"),
				TEXT(
					"Authored-graph dynamic input readback only; does not compile, save, or verify runtime/PIE state."));
			Result->SetArrayField(TEXT("dynamicInputs"), DynamicInputs);
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module Dynamic Input tree read-back:
	// content.niagara.graph.module.dynamic_inputs.tree.
	//
	// A Dynamic Input can itself consume another Dynamic Input.  The flat list
	// above intentionally reports only the nodes attached to module override
	// pins; this query follows those nodes' authored input-pin links and returns
	// a bounded tree.  It is read-only and never compiles or saves the graph.
	// ---------------------------------------------------------------------------

	constexpr int32 DefaultDynamicInputTreeDepth = 8;
	constexpr int32 MaxDynamicInputTreeDepth = 32;
	constexpr int32 DefaultDynamicInputTreeNodes = 256;
	constexpr int32 MaxDynamicInputTreeNodes = 512;

	class FTool_NiagaraModuleDynamicInputsTree final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.dynamic_inputs.tree");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to read a stack module's Dynamic Input tree."),
					TEXT("module_selector_required"),
					422);
			}

			int32 MaxDepth = DefaultDynamicInputTreeDepth;
			if (Params->HasField(TEXT("maxDepth")))
			{
				double Number = 0.0;
				if (!Params->TryGetNumberField(TEXT("maxDepth"), Number)
					|| !FMath::IsFinite(Number)
					|| FMath::TruncToInt(Number) != Number
					|| Number < 0.0
					|| Number > MaxDynamicInputTreeDepth)
				{
					return ErrorResult(
						FString::Printf(TEXT("maxDepth must be an integer in [0,%d]."), MaxDynamicInputTreeDepth),
						TEXT("invalid_max_depth"),
						422);
				}
				MaxDepth = FMath::TruncToInt(Number);
			}

			int32 MaxNodes = DefaultDynamicInputTreeNodes;
			if (Params->HasField(TEXT("maxNodes")))
			{
				double Number = 0.0;
				if (!Params->TryGetNumberField(TEXT("maxNodes"), Number)
					|| !FMath::IsFinite(Number)
					|| FMath::TruncToInt(Number) != Number
					|| Number < 1.0
					|| Number > MaxDynamicInputTreeNodes)
				{
					return ErrorResult(
						FString::Printf(TEXT("maxNodes must be an integer in [1,%d]."), MaxDynamicInputTreeNodes),
						TEXT("invalid_max_nodes"),
						422);
				}
				MaxNodes = FMath::TruncToInt(Number);
			}

			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			UNiagaraNodeFunctionCall* Node = Resolve.Selected;
			if (!Node)
			{
				return ErrorResult(TEXT("The selected stack module is unavailable."), TEXT("module_unavailable"), 409);
			}

			const ENiagaraScriptUsage Usage = Resolve.Target.OwningScript
				                                  ? Resolve.Target.OwningScript->GetUsage()
				                                  : ENiagaraScriptUsage::Function;
			FCompileConstantResolver ConstantResolver(Resolve.System.Get(), Usage);
			if (UNiagaraSystem* System = Resolve.System.Get())
			{
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					if (Handle.GetName().ToString().Equals(Resolve.Target.EmitterName, ESearchCase::CaseSensitive))
					{
						ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
						break;
					}
				}
			}

			TArray<FNiagaraVariable> InputVariables;
			TSet<FNiagaraVariable> HiddenVariables;
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(
				*Node,
				InputVariables,
				HiddenVariables,
				ConstantResolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

			int32 NodeCount = 0;
			int32 RootCount = 0;
			bool bTreeTruncated = false;
			bool bCycleDetected = false;
			TSet<const UNiagaraNodeFunctionCall*> ActiveNodes;

			TFunction<TSharedRef<FJsonObject>(UNiagaraNodeFunctionCall*, const FString&, const FString&, int32)>
				BuildNode;
			BuildNode = [&](
				UNiagaraNodeFunctionCall* Current,
				const FString& InputName,
				const FString& InputType,
				int32 Depth) -> TSharedRef<FJsonObject>
				{
					TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("inputName"), InputName);
					Entry->SetStringField(TEXT("inputType"), InputType);
					Entry->SetStringField(TEXT("dynamicInputName"), Current ? Current->GetFunctionName() : FString());
					Entry->SetStringField(
						TEXT("dynamicInputGuid"),
						Current ? Current->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
					Entry->SetStringField(TEXT("dynamicInputPath"), NodeScriptPath(Current));
					Entry->SetStringField(TEXT("nodePath"), Current ? Current->GetPathName() : FString());
					Entry->SetNumberField(TEXT("depth"), Depth);

					if (!Current)
					{
						Entry->SetBoolField(TEXT("invalid"), true);
						return Entry;
					}
					if (ActiveNodes.Contains(Current))
					{
						bCycleDetected = true;
						Entry->SetBoolField(TEXT("cycle"), true);
						return Entry;
					}
					if (NodeCount >= MaxNodes)
					{
						bTreeTruncated = true;
						Entry->SetBoolField(TEXT("truncated"), true);
						return Entry;
					}

					++NodeCount;
					ActiveNodes.Add(Current);
					const ENiagaraScriptUsage CurrentUsage = Resolve.Target.OwningScript
						? Resolve.Target.OwningScript->GetUsage()
						: ENiagaraScriptUsage::Function;
					TArray<TPair<FString, UNiagaraNodeFunctionCall*>> ChildBindings;
					GetDynamicInputChildBindings(Current, Resolve.System.Get(), CurrentUsage, ChildBindings);
					TArray<TSharedPtr<FJsonValue>> Children;
					bool bChildrenTruncated = false;
					if (Depth < MaxDepth)
					{
						for (const TPair<FString, UNiagaraNodeFunctionCall*>& Binding : ChildBindings)
						{
							UNiagaraNodeFunctionCall* Child = Binding.Value;
							if (!Child)
							{
								continue;
							}
							if (NodeCount >= MaxNodes)
							{
								bChildrenTruncated = true;
								bTreeTruncated = true;
								continue;
							}
							Children.Add(MakeShared<FJsonValueObject>(
								BuildNode(Child, Binding.Key, FString(), Depth + 1)));
						}
					}
					else if (ChildBindings.Num() > 0)
					{
						bChildrenTruncated = true;
						bTreeTruncated = true;
					}
					ActiveNodes.Remove(Current);
					Entry->SetNumberField(TEXT("inputCount"), ChildBindings.Num());
					Entry->SetBoolField(TEXT("childrenTruncated"), bChildrenTruncated);
					Entry->SetArrayField(TEXT("children"), Children);
					return Entry;
				};

			TArray<TSharedPtr<FJsonValue>> DynamicInputs;
			int32 DynamicInputCount = 0;
			const int32 InputCount = FMath::Min(InputVariables.Num(), MaxNodes);
			for (int32 InputIndex = 0; InputIndex < InputCount; ++InputIndex)
			{
				const FNiagaraVariable& Input = InputVariables[InputIndex];
				const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Input.GetName());
				UEdGraphPin* OverridePin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
				if (!OverridePin)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
				{
					if (!LinkedPin || LinkedPin->Direction != EGPD_Output)
					{
						continue;
					}
					UNiagaraNodeFunctionCall* DynamicNode = Cast<UNiagaraNodeFunctionCall>(
						LinkedPin->GetOwningNodeUnchecked());
					if (!DynamicNode
						|| !DynamicNode->FunctionScript
						|| DynamicNode->FunctionScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
					{
						continue;
					}

					++DynamicInputCount;
					FString InputName = Input.GetName().ToString();
					InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
					const FString InputType = Input.GetType().GetName();
					if (NodeCount >= MaxNodes)
					{
						bTreeTruncated = true;
						continue;
					}
					DynamicInputs.Add(MakeShared<FJsonValueObject>(
						BuildNode(DynamicNode, InputName, InputType, 0)));
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.module-dynamic-input-tree.v1"));
			Result->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
			Result->SetStringField(TEXT("emitterPath"), Resolve.Target.EmitterPath);
			Result->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
			Result->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
			Result->SetStringField(TEXT("scriptUsage"), Resolve.Target.ScriptUsage);
			Result->SetStringField(TEXT("moduleSelector"), Request.ModuleSelector);
			Result->SetStringField(TEXT("moduleScript"), NodeScriptPath(Node));
			Result->SetStringField(TEXT("nodePath"), Node->GetPathName());
			Result->SetNumberField(TEXT("rootInputCount"), InputVariables.Num());
			Result->SetNumberField(TEXT("rootInputLimit"), MaxNodes);
			Result->SetBoolField(TEXT("rootInputsTruncated"), InputVariables.Num() > InputCount);
			Result->SetNumberField(TEXT("dynamicInputCount"), DynamicInputCount);
			Result->SetNumberField(TEXT("nodeCount"), NodeCount);
			Result->SetNumberField(TEXT("nodeLimit"), MaxNodes);
			Result->SetNumberField(TEXT("maxDepth"), MaxDepth);
			Result->SetBoolField(TEXT("treeTruncated"), bTreeTruncated);
			Result->SetBoolField(TEXT("cycleDetected"), bCycleDetected);
			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), false);
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			Result->SetStringField(
				TEXT("scope"),
				TEXT(
					"Authored-graph Dynamic Input tree readback only; does not compile, save, or verify runtime/PIE state."));
			Result->SetArrayField(TEXT("dynamicInputs"), DynamicInputs);
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Dynamic Input script introspection:
	// content.niagara.graph.dynamic_input.inputs.get.
	//
	// Monolith exposes the inputs of an unattached Dynamic Input script so a
	// caller can validate an add/set request before mutating a stack. Keep this
	// query independent from a System: Dynamic Input assets are reusable and may
	// not be attached to a graph yet.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxDynamicInputScriptInputs = 256;

	class FTool_NiagaraDynamicInputInputsGet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.dynamic_input.inputs.get");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ScriptSelector;
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("scriptPath"), ScriptSelector)
				|| ScriptSelector.TrimStartAndEnd().IsEmpty())
			{
				return ErrorResult(
					TEXT("scriptPath is required and must identify a Niagara Dynamic Input script."),
					TEXT("script_path_required"),
					422);
			}

			const FString ScriptPath = NormalizeObjectPath(ScriptSelector.TrimStartAndEnd());
			if (ScriptPath.IsEmpty())
			{
				return ErrorResult(
					TEXT("scriptPath must be a valid object or package path."),
					TEXT("invalid_object_path"),
					422);
			}

			UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *ScriptPath);
			if (!Script)
			{
				return ErrorResult(
					FString::Printf(TEXT("Niagara script '%s' was not found."), *ScriptSelector),
					TEXT("script_not_found"),
					404);
			}
			if (Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
			{
				return ErrorResult(
					FString::Printf(
						TEXT("Script '%s' has usage '%d'; a Dynamic Input script is required."),
						*Script->GetPathName(),
						static_cast<int32>(Script->GetUsage())),
					TEXT("invalid_dynamic_input_script"),
					422);
			}

			UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Script->GetLatestSource());
			if (!Source || !Source->NodeGraph)
			{
				return ErrorResult(
					FString::Printf(
						TEXT("Dynamic Input script '%s' has no loaded source graph."), *Script->GetPathName()),
					TEXT("script_graph_unavailable"),
					409);
			}

			TArray<UNiagaraNodeInput*> InputNodes;
			Source->NodeGraph->GetNodesOfClass(InputNodes);
			InputNodes.RemoveAll([](const UNiagaraNodeInput* Node)
			{
				return !Node || Node->Usage != ENiagaraInputNodeUsage::Parameter;
			});
			InputNodes.Sort([](const UNiagaraNodeInput& Left, const UNiagaraNodeInput& Right)
			{
				const FString LeftName = Left.Input.GetName().ToString();
				const FString RightName = Right.Input.GetName().ToString();
				if (LeftName != RightName)
				{
					return LeftName < RightName;
				}
				return Left.GetPathName() < Right.GetPathName();
			});

			TArray<TSharedPtr<FJsonValue>> Inputs;
			const int32 InputCount = FMath::Min(InputNodes.Num(), MaxDynamicInputScriptInputs);
			Inputs.Reserve(InputCount);
			for (int32 Index = 0; Index < InputCount; ++Index)
			{
				const UNiagaraNodeInput* InputNode = InputNodes[Index];
				if (!InputNode)
				{
					continue;
				}
				FString ShortName = InputNode->Input.GetName().ToString();
				ShortName.RemoveFromStart(PARAM_MAP_MODULE_STR);

				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), ShortName);
				Entry->SetStringField(TEXT("fullName"), InputNode->Input.GetName().ToString());
				Entry->SetStringField(TEXT("type"), InputNode->Input.GetType().GetName());
				Entry->SetBoolField(TEXT("dataInterface"), InputNode->Input.GetType().IsDataInterface());
				Entry->SetStringField(TEXT("nodePath"), InputNode->GetPathName());
				Inputs.Add(MakeShared<FJsonValueObject>(Entry));
			}

			FString OutputType;
			TArray<UNiagaraNodeOutput*> OutputNodes;
			Source->NodeGraph->GetNodesOfClass(OutputNodes);
			OutputNodes.Sort([](const UNiagaraNodeOutput& Left, const UNiagaraNodeOutput& Right)
			{
				return Left.GetPathName() < Right.GetPathName();
			});
			for (const UNiagaraNodeOutput* OutputNode : OutputNodes)
			{
				if (!OutputNode)
				{
					continue;
				}
				for (const UEdGraphPin* Pin : OutputNode->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input)
					{
						continue;
					}
					const FNiagaraTypeDefinition Type = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
					if (Type == FNiagaraTypeDefinition::GetParameterMapDef())
					{
						continue;
					}
					OutputType = Type.GetName();
					break;
				}
				if (!OutputType.IsEmpty())
				{
					break;
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.dynamic-input-inputs.v1"));
			Result->SetStringField(TEXT("scriptPath"), Script->GetPathName());
			Result->SetStringField(TEXT("scriptName"), Script->GetName());
			Result->SetStringField(TEXT("scriptUsage"), TEXT("dynamicInput"));
			Result->SetStringField(TEXT("outputType"), OutputType);
			Result->SetNumberField(TEXT("inputCount"), Inputs.Num());
			Result->SetNumberField(TEXT("inputLimit"), MaxDynamicInputScriptInputs);
			Result->SetBoolField(TEXT("inputsTruncated"), InputNodes.Num() > MaxDynamicInputScriptInputs);
			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), false);
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			Result->SetStringField(
				TEXT("scope"),
				TEXT(
					"Authored Dynamic Input script graph inspection only; does not compile, save, or mutate the asset."));
			Result->SetArrayField(TEXT("inputs"), Inputs);
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Mounted Dynamic Input value read-back:
	// content.niagara.graph.module.dynamic_input.value.get.
	// ---------------------------------------------------------------------------

	class FTool_NiagaraModuleDynamicInputValueGet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.dynamic_input.value.get");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest StackRequest;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, StackRequest, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (StackRequest.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to read a Dynamic Input value."),
					TEXT("module_selector_required"),
					422);
			}

			FString DynamicInputGuid;
			if (!Params->TryGetStringField(TEXT("dynamicInputGuid"), DynamicInputGuid)
				|| DynamicInputGuid.TrimStartAndEnd().IsEmpty())
			{
				return ErrorResult(
					TEXT("dynamicInputGuid is required and must identify a mounted Dynamic Input node."),
					TEXT("dynamic_input_guid_required"),
					422);
			}
			DynamicInputGuid = DynamicInputGuid.TrimStartAndEnd();

			FString InputSelector;
			if (!Params->TryGetStringField(TEXT("input"), InputSelector)
				|| InputSelector.TrimStartAndEnd().IsEmpty())
			{
				return ErrorResult(
					TEXT("input is required and must identify a Dynamic Input sub-input."),
					TEXT("input_required"),
					422);
			}
			InputSelector = InputSelector.TrimStartAndEnd();

			FStackModuleResolve Resolve;
			if (!ResolveStackModule(StackRequest, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			UNiagaraNodeFunctionCall* ModuleNode = Resolve.Selected;
			UNiagaraGraph* Graph = Resolve.Target.Graph;
			if (!ModuleNode || !Graph)
			{
				return ErrorResult(
					TEXT("The selected Niagara module graph is unavailable."),
					TEXT("module_unavailable"),
					409);
			}

			FGuid RequestedGuid;
			if (!FGuid::Parse(DynamicInputGuid, RequestedGuid) || !RequestedGuid.IsValid())
			{
				return ErrorResult(
					TEXT("dynamicInputGuid must be a valid GUID."), TEXT("dynamic_input_guid_invalid"), 422);
			}
			UNiagaraNodeFunctionCall* DynamicNode = nullptr;
			const ENiagaraScriptUsage RootUsage = Resolve.Target.OwningScript
				? Resolve.Target.OwningScript->GetUsage()
				: ENiagaraScriptUsage::Function;
			if (!FindDynamicInputInSubtree(
				ModuleNode, Resolve.System.Get(), RootUsage, RequestedGuid, DynamicNode))
			{
				DynamicNode = nullptr;
			}
			if (!DynamicNode)
			{
				return ErrorResult(
					FString::Printf(
						TEXT("Dynamic Input node '%s' was not found in the selected module graph."), *DynamicInputGuid),
					TEXT("dynamic_input_not_found"),
					404);
			}

			const ENiagaraScriptUsage Usage = Resolve.Target.OwningScript
				                                  ? Resolve.Target.OwningScript->GetUsage()
				                                  : ENiagaraScriptUsage::Function;
			FCompileConstantResolver ConstantResolver(Resolve.System.Get(), Usage);
			if (UNiagaraSystem* System = Resolve.System.Get())
			{
				for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
				{
					if (Handle.GetName().ToString().Equals(Resolve.Target.EmitterName, ESearchCase::CaseSensitive))
					{
						ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
						break;
					}
				}
			}

			TArray<FNiagaraVariable> InputVariables;
			TSet<FNiagaraVariable> HiddenVariables;
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(
				*DynamicNode,
				InputVariables,
				HiddenVariables,
				ConstantResolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

			const FNiagaraVariable* MatchedInput = nullptr;
			for (const FNiagaraVariable& Candidate : InputVariables)
			{
				FString ShortName = Candidate.GetName().ToString();
				ShortName.RemoveFromStart(PARAM_MAP_MODULE_STR);
				FString CompactShortName = ShortName;
				CompactShortName.ReplaceInline(TEXT(" "), TEXT(""));
				FString CompactSelector = InputSelector;
				CompactSelector.ReplaceInline(TEXT(" "), TEXT(""));
				if (ShortName.Equals(InputSelector, ESearchCase::IgnoreCase)
					|| Candidate.GetName().ToString().Equals(InputSelector, ESearchCase::IgnoreCase)
					|| CompactShortName.Equals(CompactSelector, ESearchCase::IgnoreCase))
				{
					MatchedInput = &Candidate;
					break;
				}
			}
			if (!MatchedInput)
			{
				TArray<FString> AvailableNames;
				for (const FNiagaraVariable& Candidate : InputVariables)
				{
					FString ShortName = Candidate.GetName().ToString();
					ShortName.RemoveFromStart(PARAM_MAP_MODULE_STR);
					AvailableNames.Add(ShortName);
				}
				return ErrorResult(
					FString::Printf(
						TEXT("Input '%s' was not found on Dynamic Input node '%s'. Available: %s"),
						*InputSelector,
						*DynamicInputGuid,
						*FString::Join(AvailableNames, TEXT(", "))),
					TEXT("input_not_found"),
					404);
			}

			const FNiagaraParameterHandle Aliased = AliasedInputHandle(DynamicNode, MatchedInput->GetName());
			UEdGraphPin* OverridePin = FindExistingOverridePin(DynamicNode, Aliased.GetParameterHandleString());
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FString ShortName = MatchedInput->GetName().ToString();
			ShortName.RemoveFromStart(PARAM_MAP_MODULE_STR);
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.dynamic-input-value.v1"));
			Result->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Target.EmitterName);
			Result->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
			Result->SetStringField(TEXT("outputNodePath"), Resolve.Target.OutputPath);
			Result->SetStringField(TEXT("moduleSelector"), StackRequest.ModuleSelector);
			Result->SetStringField(TEXT("moduleScript"), NodeScriptPath(ModuleNode));
			Result->SetStringField(
				TEXT("dynamicInputGuid"), DynamicNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			Result->SetStringField(TEXT("dynamicInputName"), DynamicNode->GetFunctionName());
			Result->SetStringField(TEXT("dynamicInputPath"), NodeScriptPath(DynamicNode));
			Result->SetStringField(TEXT("input"), ShortName);
			Result->SetStringField(TEXT("fullName"), MatchedInput->GetName().ToString());
			Result->SetStringField(TEXT("type"), MatchedInput->GetType().GetName());
			Result->SetBoolField(TEXT("dataInterface"), MatchedInput->GetType().IsDataInterface());
			Result->SetBoolField(TEXT("hasOverride"), OverridePin != nullptr);

			if (!OverridePin)
			{
				Result->SetStringField(TEXT("source"), TEXT("default"));
				Result->SetBoolField(TEXT("isDefault"), true);
			}
			else if (OverridePin->LinkedTo.Num() > 0 && OverridePin->LinkedTo[0])
			{
				UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
				Result->SetBoolField(TEXT("isLinked"), true);
				if (UNiagaraNodeFunctionCall* NestedNode = Cast<UNiagaraNodeFunctionCall>(
					LinkedPin->GetOwningNodeUnchecked()))
				{
					if (NestedNode->FunctionScript
						&& NestedNode->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput)
					{
						Result->SetStringField(TEXT("source"), TEXT("dynamicInput"));
						Result->SetStringField(TEXT("nestedDynamicInputName"), NestedNode->GetFunctionName());
						Result->SetStringField(
							TEXT("nestedDynamicInputGuid"),
							NestedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
						Result->SetStringField(TEXT("nestedDynamicInputPath"), NodeScriptPath(NestedNode));
					}
					else
					{
						Result->SetStringField(TEXT("source"), TEXT("linked"));
						Result->SetStringField(
							TEXT("linkedNodeClass"), LinkedPin->GetOwningNodeUnchecked()->GetClass()->GetName());
					}
				}
				else if (UNiagaraNodeInput* LinkedInput = Cast<UNiagaraNodeInput>(LinkedPin->GetOwningNodeUnchecked()))
				{
					Result->SetStringField(TEXT("source"), TEXT("parameterBinding"));
					Result->SetStringField(TEXT("linkedParameter"), LinkedInput->Input.GetName().ToString());
				}
			}
			else
			{
				Result->SetStringField(TEXT("source"), TEXT("literal"));
				Result->SetStringField(TEXT("value"), OverridePin->DefaultValue);
				Result->SetBoolField(TEXT("isDefault"), OverridePin->DefaultValue.IsEmpty());
			}

			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), false);
			Result->SetBoolField(TEXT("runtimeVerified"), false);
			Result->SetStringField(
				TEXT("scope"),
				TEXT("Authored Dynamic Input sub-input readback only; does not compile, save, or mutate the graph."));
			return FMCPToolResult::Ok(Result);
		}
	};

	// Remove a mounted Dynamic Input node by its stable graph GUID. This accepts
	// nodes at any depth in the selected module graph, so callers can edit the
	// complete tree returned by dynamic_inputs.tree. Only Dynamic Input function
	// calls are eligible; unrelated graph nodes are never removed.
	class FTool_NiagaraModuleDynamicInputRemove final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.dynamic_input.remove");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			FString GuidText;
			FGuid DynamicGuid;
			if (!Params->TryGetStringField(TEXT("dynamicInputGuid"), GuidText)
				|| !FGuid::Parse(GuidText, DynamicGuid) || !DynamicGuid.IsValid())
			{
				return ErrorResult(
					TEXT("dynamicInputGuid must be a valid mounted Dynamic Input node GUID."),
					TEXT("dynamic_input_guid_required"), 422);
			}
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			UNiagaraSystem* System = Resolve.System.Get();
			UNiagaraGraph* Graph = Resolve.Target.Graph;
			if (!System || !Graph || Resolve.bBlocked)
			{
				return ErrorResult(
					TEXT("The selected Niagara graph is unavailable or read-only."), TEXT("plan_blocked"), 409);
			}
			UNiagaraNodeFunctionCall* DynamicNode = nullptr;
			TArray<UNiagaraNodeFunctionCall*> FunctionNodes;
			Graph->GetNodesOfClass(FunctionNodes);
			for (UNiagaraNodeFunctionCall* Candidate : FunctionNodes)
			{
				if (Candidate && Candidate->NodeGuid == DynamicGuid && Candidate->FunctionScript
					&& Candidate->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput)
				{
					DynamicNode = Candidate;
					break;
				}
			}
			if (!DynamicNode)
			{
				return ErrorResult(
					FString::Printf(TEXT("Dynamic Input node '%s' was not found."), *GuidText),
					TEXT("dynamic_input_not_found"), 404);
			}
			const ENiagaraScriptUsage RootUsage = Resolve.Target.OwningScript
				? Resolve.Target.OwningScript->GetUsage()
				: ENiagaraScriptUsage::Function;
			UNiagaraNodeFunctionCall* ReachableNode = nullptr;
			if (!FindDynamicInputInSubtree(
				Resolve.Selected, System, RootUsage, DynamicGuid, ReachableNode)
				|| ReachableNode != DynamicNode)
			{
				return ErrorResult(
					TEXT("The requested Dynamic Input GUID is not reachable beneath the selected module."),
					TEXT("dynamic_input_not_found"), 404);
			}

			TSet<UNiagaraNodeFunctionCall*> Subtree;
			CollectDynamicInputSubtree(DynamicNode, System, RootUsage, Subtree);
			// A nested node may be shared by multiple authored override pins. Removing
			// it would silently change another input, so reject the operation unless
			// the only outside parent is the selected module's direct override.
			int32 OutsideParentCount = 0;
			TArray<UNiagaraNodeFunctionCall*> GraphFunctions;
			Graph->GetNodesOfClass(GraphFunctions);
			for (UNiagaraNodeFunctionCall* Candidate : GraphFunctions)
			{
				if (!Candidate)
				{
					continue;
				}
				TArray<TPair<FString, UNiagaraNodeFunctionCall*>> Bindings;
				GetDynamicInputChildBindings(Candidate, System, RootUsage, Bindings);
				for (const TPair<FString, UNiagaraNodeFunctionCall*>& Binding : Bindings)
				{
					if (Binding.Value == DynamicNode || Subtree.Contains(Binding.Value))
					{
						if (!Subtree.Contains(Candidate))
						{
							++OutsideParentCount;
						}
					}
				}
			}
			if (OutsideParentCount != 1)
			{
				return ErrorResult(
					TEXT("The Dynamic Input subtree is shared or has an unexpected parent; removal was refused."),
					TEXT("dynamic_input_shared"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Remove Niagara Dynamic Input")));
			System->Modify();
			Graph->Modify();
			TArray<TPair<UEdGraphPin*, UEdGraphPin*>> SavedLinks;
			for (UNiagaraNodeFunctionCall* Node : Subtree)
			{
				if (!Node)
				{
					continue;
				}
				Node->Modify();
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked)
						{
							SavedLinks.Add(TPair<UEdGraphPin*, UEdGraphPin*>(Pin, Linked));
						}
					}
				}
				Node->BreakAllNodeLinks();
				Graph->RemoveNode(Node);
			}
			Graph->NotifyGraphChanged();
			const FCompileSummary CompileSummary = CompileSystem(System);
			bool bReadBack = true;
		for (UNiagaraNodeFunctionCall* Node : Subtree)
		{
			bReadBack &= Node && !Graph->Nodes.Contains(Node);
		}
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				for (UNiagaraNodeFunctionCall* Node : Subtree)
				{
					if (Node && !Graph->Nodes.Contains(Node))
					{
						Graph->AddNode(Node, false, false);
					}
				}
				for (const TPair<UEdGraphPin*, UEdGraphPin*>& Link : SavedLinks)
				{
					if (Link.Key && Link.Value && Link.Key->GetOwningNodeUnchecked()
						&& Link.Value->GetOwningNodeUnchecked())
					{
						Link.Key->MakeLinkTo(Link.Value);
					}
				}
				Graph->NotifyGraphChanged();
				CompileSystem(System);
				return ErrorResult(
					FString::Printf(TEXT("Dynamic Input removal read-back or compilation failed (status=%s); the graph was restored."), *CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"), 500);
			}
			System->MarkPackageDirty();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.dynamic-input-remove.v1"));
			Result->SetStringField(TEXT("system"), Resolve.SystemObjectPath);
			Result->SetStringField(TEXT("graph"), Resolve.Target.GraphPath);
			Result->SetStringField(TEXT("moduleSelector"), Request.ModuleSelector);
			Result->SetStringField(TEXT("dynamicInputGuid"), GuidText);
			Result->SetBoolField(TEXT("changed"), true);
			Result->SetBoolField(TEXT("verified"), true);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), CompileSummary.bCompiled);
			Result->SetStringField(TEXT("compileStatus"), CompileSummary.Status);
			Result->SetStringField(TEXT("scope"), TEXT("authored Dynamic Input tree; runtime resolution unverified"));
			return FMCPToolResult::Ok(Result);
		}
	};

	// Search Dynamic Input script assets through Asset Registry metadata plus a
	// bounded scan of scripts already loaded in the current editor. The fallback
	// never loads arbitrary assets; callers can discover project assets without
	// forcing every script into memory, then opt into an explicit load through the
	// authoring/readback commands.
	class FTool_NiagaraDynamicInputSearch final : public FMCPToolBase
	{
	public:
		static constexpr int32 MaxLoadedObjectScan = 4096;

		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.dynamic_input.search");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Query;
			if (Params.IsValid() && Params->HasField(TEXT("query"))
				&& (!Params->TryGetStringField(TEXT("query"), Query) || Query.Len() > 512))
			{
				return ErrorResult(TEXT("query must be at most 512 characters."), TEXT("query_invalid"), 422);
			}
			int32 Limit = 64;
			if (Params.IsValid() && Params->HasField(TEXT("limit")))
			{
				double Number = 0.0;
				if (!Params->TryGetNumberField(TEXT("limit"), Number) || !FMath::IsFinite(Number)
					|| Number < 1.0 || Number > 256.0 || FMath::TruncToInt(Number) != Number)
				{
					return ErrorResult(TEXT("limit must be an integer from 1 to 256."), TEXT("limit_invalid"), 422);
				}
				Limit = FMath::TruncToInt(Number);
			}
			Query = Query.TrimStartAndEnd();
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			FARFilter Filter;
			Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			Filter.bRecursivePaths = true;
			// Usage is an AssetRegistrySearchable enum on UNiagaraScript.  Filter
			// on the authored usage instead of inferring Dynamic Input assets from
			// a conventional directory name; projects are free to store them in
			// any content folder.
			Filter.TagsAndValues.Add(
				GET_MEMBER_NAME_CHECKED(UNiagaraScript, Usage),
				TOptional<FString>(TEXT("DynamicInput")));
			TArray<FAssetData> Assets;
			AssetRegistry.GetAssets(Filter, Assets);

			// The Usage tag is not guaranteed to be present in an isolated/editor
			// fixture even when a valid Dynamic Input script is already loaded.  Keep
			// Asset Registry as the unloaded discovery path, then add a bounded
			// loaded-object fallback.  This does not load arbitrary assets and is
			// deduplicated by the stable object path.
			TMap<FString, FString> CandidateNames;
			TSet<FString> LoadedPaths;
			for (const FAssetData& Asset : Assets)
			{
				const FString Path = Asset.GetSoftObjectPath().ToString();
				const FString Name = Asset.AssetName.ToString();
				if (Path.IsEmpty()
					|| (!Query.IsEmpty()
						&& !Name.Contains(Query, ESearchCase::IgnoreCase)
						&& !Path.Contains(Query, ESearchCase::IgnoreCase)))
				{
					continue;
				}
				CandidateNames.FindOrAdd(Path, Name);
				if (FindObject<UNiagaraScript>(nullptr, *Path) != nullptr)
				{
					LoadedPaths.Add(Path);
				}
			}

			int32 LoadedObjectScanCount = 0;
			bool bLoadedObjectScanTruncated = false;
			for (TObjectIterator<UNiagaraScript> It; It; ++It)
			{
				if (++LoadedObjectScanCount > MaxLoadedObjectScan)
				{
					bLoadedObjectScanTruncated = true;
					break;
				}
				UNiagaraScript* Script = *It;
				if (!Script
					|| Script->HasAnyFlags(RF_ClassDefaultObject | RF_Transient)
					|| Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
				{
					continue;
				}
				const FString Path = Script->GetPathName();
				const FString Name = Script->GetName();
				if (Path.IsEmpty()
					|| (!Query.IsEmpty()
						&& !Name.Contains(Query, ESearchCase::IgnoreCase)
						&& !Path.Contains(Query, ESearchCase::IgnoreCase)))
				{
					continue;
				}
				CandidateNames.FindOrAdd(Path, Name);
				LoadedPaths.Add(Path);
			}

			TArray<FString> CandidatePaths;
			CandidateNames.GetKeys(CandidatePaths);
			CandidatePaths.Sort();
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 Index = 0; Index < CandidatePaths.Num() && Index < Limit; ++Index)
			{
				const FString& Path = CandidatePaths[Index];
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("name"), CandidateNames.FindChecked(Path));
				Row->SetStringField(TEXT("path"), Path);
				Row->SetStringField(TEXT("usage"), TEXT("dynamicInput"));
				Row->SetBoolField(TEXT("loaded"), LoadedPaths.Contains(Path));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.dynamic-input-search.v1"));
			Result->SetStringField(TEXT("query"), Query);
			Result->SetNumberField(TEXT("total"), CandidatePaths.Num());
			Result->SetNumberField(TEXT("limit"), Limit);
			Result->SetBoolField(TEXT("hasMore"), CandidatePaths.Num() > Limit);
			Result->SetBoolField(TEXT("loadedOnly"), false);
			Result->SetBoolField(TEXT("assetRegistry"), true);
			Result->SetBoolField(TEXT("loadedObjectFallback"), true);
			Result->SetNumberField(TEXT("loadedObjectScanLimit"), MaxLoadedObjectScan);
			Result->SetBoolField(TEXT("loadedObjectScanTruncated"), bLoadedObjectScanTruncated);
			Result->SetBoolField(TEXT("bounded"), true);
			Result->SetArrayField(TEXT("dynamicInputs"), Rows);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_NiagaraModuleRemovePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.remove.plan");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to plan a module removal."), TEXT("module_selector_required"),
					422);
			}
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildRemovePlanJson(Resolve);
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(
					TEXT("Unable to compute the Niagara module removal plan digest."), TEXT("digest_unavailable"), 500);
			}
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraModuleRemoveApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.remove.apply");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return ErrorResult(
					TEXT("A non-empty requestId is required for Niagara module removal."), TEXT("request_id_required"),
					422);
			}
			if (const FString* ExistingReceiptId = RemoveRequestReceiptIds().Find(RequestId))
			{
				if (FRemoveReceipt* Existing = RemoveReceipts().Find(*ExistingReceiptId))
				{
					FString ErrorCode;
					FString Error;
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
					{
						return ErrorResult(Error, ErrorCode, 409);
					}
					return FMCPToolResult::Ok(MakeRemoveResult(*Existing, true));
				}
				return ErrorResult(
					TEXT("requestId is associated with an unavailable Niagara removal receipt."),
					TEXT("request_id_conflict"), 409);
			}

			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to remove a module."), TEXT("module_selector_required"), 422);
			}
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildRemovePlanJson(Resolve);
			FString PlanDigest;
			if (!TryDigestJson(Plan, PlanDigest))
			{
				return ErrorResult(
					TEXT("Unable to compute the Niagara module removal plan digest."), TEXT("digest_unavailable"), 500);
			}
			if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 409);
			}
			if (Resolve.bBlocked)
			{
				return ErrorResult(
					TEXT(
						"The Niagara module removal plan is blocked because the selected graph is shared or read-only."),
					TEXT("plan_blocked"), 409);
			}

			UNiagaraSystem* System = Resolve.System.Get();
			UNiagaraGraph* Graph = Resolve.Target.Graph;
			UNiagaraNodeOutput* Output = Resolve.Target.OutputNode;
			UNiagaraNodeFunctionCall* Node = Resolve.Selected;
			if (!System || !Graph || !Output || !Node
				|| Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Resolve.GraphChangeId
				|| Graph->GetPathName() != Resolve.Target.GraphPath
				|| Output->GetPathName() != Resolve.Target.OutputPath)
			{
				return ErrorResult(
					TEXT("The Niagara graph or output changed after the plan was created; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);
			}

			const FString ModuleScriptPath = NodeScriptPath(Node);
			const FString NodeName = Node->GetName();
			const FString NodePath = Node->GetPathName();
			const int32 OriginalIndex = Resolve.SelectedIndex;

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Remove Niagara Stack Module")));
			System->Modify();
			Graph->Modify();
			if (!SpliceOutStackNode(Graph, Node))
			{
				Transaction.Cancel();
				return ErrorResult(
					TEXT("The Niagara module could not be removed from the stack."), TEXT("module_remove_failed"), 500);
			}

			const bool bReadBack = !Graph->Nodes.Contains(Node);
			FCompileSummary CompileSummary = CompileSystem(System);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *ModuleScriptPath);
				UNiagaraNodeFunctionCall* Restored = ModuleScript
					                                     ? FNiagaraStackGraphUtilities::AddScriptModuleToStack(
						                                     ModuleScript, *Output, OriginalIndex)
					                                     : nullptr;
				const FCompileSummary RestoreSummary = CompileSystem(System);
				if (!Restored || !Graph->Nodes.Contains(Restored) || !RestoreSummary.bCompiled)
				{
					return ErrorResult(
						TEXT(
							"Niagara module removal failed and restoration could not be verified; the transaction was retained for Editor Undo."),
						TEXT("restore_verification_failed"), 500);
				}
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara module removal read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}

			System->MarkPackageDirty();
			FRemoveReceipt Receipt;
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = PlanDigest;
			Receipt.SystemPath = Resolve.SystemObjectPath;
			Receipt.GraphPath = Resolve.Target.GraphPath;
			Receipt.OutputPath = Resolve.Target.OutputPath;
			Receipt.ModuleScriptPath = ModuleScriptPath;
			Receipt.NodeName = NodeName;
			Receipt.NodePath = NodePath;
			Receipt.OriginalIndex = OriginalIndex;
			Receipt.System = System;
			Receipt.Graph = Graph;
			Receipt.OutputNode = Output;
			Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.bChanged = true;
			Receipt.bCompiled = CompileSummary.bCompiled;
			Receipt.CompileStatus = CompileSummary.Status;
			RemoveReceipts().Add(Receipt.ReceiptId, Receipt);
			RemoveRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeRemoveResult(Receipt, false));
		}
	};

	class FTool_NiagaraModuleRemoveRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.remove.rollback");
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
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"), 422);
			}
			FRemoveReceipt* Receipt = RemoveReceipts().Find(ReceiptId);
			if (!Receipt)
			{
				return ErrorResult(
					TEXT("The Niagara module removal receipt is unknown in this Editor instance."),
					TEXT("receipt_not_found"), 404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeRemoveResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(
					TEXT("requestId does not match the Niagara module removal receipt."), TEXT("request_id_mismatch"),
					409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraNodeOutput* Output = Receipt->OutputNode.Get();
			if (!System || !Graph || !Output)
			{
				return ErrorResult(
					TEXT("The Niagara module target is no longer loaded."), TEXT("target_unavailable"), 409);
			}
			if (Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->GraphChangeIdAfter)
			{
				return ErrorResult(
					TEXT("The Niagara graph changed after removal; rollback was refused."), TEXT("rollback_conflict"),
					409);
			}

			UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr, *Receipt->ModuleScriptPath);
			if (!ModuleScript || ModuleScript->GetUsage() != ENiagaraScriptUsage::Module)
			{
				return ErrorResult(
					TEXT("The removed module script is no longer available for re-insertion."),
					TEXT("module_script_unavailable"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Stack Module Removal")));
			System->Modify();
			Graph->Modify();
			UNiagaraNodeFunctionCall* Restored = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
				ModuleScript, *Output, Receipt->OriginalIndex);
			if (!Restored)
			{
				Transaction.Cancel();
				return ErrorResult(
					TEXT("The removed Niagara module could not be re-inserted."), TEXT("rollback_failed"), 500);
			}
			FCompileSummary CompileSummary = CompileSystem(System);
			const bool bRestored = Graph->Nodes.Contains(Restored)
				&& ModuleScriptIdentityMatches(Restored, Receipt->ModuleScriptPath);
			if (!bRestored || !CompileSummary.bCompiled)
			{
				return ErrorResult(
					TEXT(
						"Niagara module removal rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bCompiled = CompileSummary.bCompiled;
			Receipt->CompileStatus = CompileSummary.Status;
			return FMCPToolResult::Ok(MakeRemoveResult(*Receipt, false));
		}
	};

	class FTool_NiagaraModuleMovePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.move.plan");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to plan a module move."), TEXT("module_selector_required"), 422);
			}
			// Resolve the module before validating targetIndex so an unknown selector
			// surfaces a semantic module_not_found/ambiguous_module error ahead of the
			// parameter-level targetIndex error.
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (!Request.bHasTargetIndex)
			{
				return ErrorResult(
					TEXT("targetIndex is required to plan a module move."), TEXT("target_index_required"), 422);
			}
			const int32 ModuleCount = Resolve.ModuleNodes.Num();
			if (Request.TargetIndex < 0 || Request.TargetIndex >= ModuleCount)
			{
				return ErrorResult(
					FString::Printf(
						TEXT("targetIndex %d is outside the stack module range [0,%d)."), Request.TargetIndex,
						ModuleCount),
					TEXT("target_index_out_of_range"), 422);
			}
			if (Request.TargetIndex == Resolve.SelectedIndex)
			{
				return ErrorResult(
					TEXT("The module is already at the requested target index."), TEXT("already_at_target_index"), 409);
			}
			TSharedRef<FJsonObject> Plan = BuildMovePlanJson(Resolve, Request.TargetIndex);
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(
					TEXT("Unable to compute the Niagara module move plan digest."), TEXT("digest_unavailable"), 500);
			}
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraModuleMoveApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.move.apply");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return ErrorResult(
					TEXT("A non-empty requestId is required for Niagara module moves."), TEXT("request_id_required"),
					422);
			}
			if (const FString* ExistingReceiptId = MoveRequestReceiptIds().Find(RequestId))
			{
				if (FMoveReceipt* Existing = MoveReceipts().Find(*ExistingReceiptId))
				{
					FString ErrorCode;
					FString Error;
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
					{
						return ErrorResult(Error, ErrorCode, 409);
					}
					return FMCPToolResult::Ok(MakeMoveResult(*Existing, true));
				}
				return ErrorResult(
					TEXT("requestId is associated with an unavailable Niagara move receipt."),
					TEXT("request_id_conflict"), 409);
			}

			FStackModuleRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseStackRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			if (Request.ModuleSelector.IsEmpty())
			{
				return ErrorResult(
					TEXT("moduleSelector is required to move a module."), TEXT("module_selector_required"), 422);
			}
			if (!Request.bHasTargetIndex)
			{
				return ErrorResult(
					TEXT("targetIndex is required to move a module."), TEXT("target_index_required"), 422);
			}
			FStackModuleResolve Resolve;
			if (!ResolveStackModule(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			const int32 ModuleCount = Resolve.ModuleNodes.Num();
			if (Request.TargetIndex < 0 || Request.TargetIndex >= ModuleCount)
			{
				return ErrorResult(
					FString::Printf(
						TEXT("targetIndex %d is outside the stack module range [0,%d)."), Request.TargetIndex,
						ModuleCount),
					TEXT("target_index_out_of_range"), 422);
			}
			if (Request.TargetIndex == Resolve.SelectedIndex)
			{
				return ErrorResult(
					TEXT("The module is already at the requested target index."), TEXT("already_at_target_index"), 409);
			}
			TSharedRef<FJsonObject> Plan = BuildMovePlanJson(Resolve, Request.TargetIndex);
			FString PlanDigest;
			if (!TryDigestJson(Plan, PlanDigest))
			{
				return ErrorResult(
					TEXT("Unable to compute the Niagara module move plan digest."), TEXT("digest_unavailable"), 500);
			}
			if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 409);
			}
			if (Resolve.bBlocked)
			{
				return ErrorResult(
					TEXT("The Niagara module move plan is blocked because the selected graph is shared or read-only."),
					TEXT("plan_blocked"), 409);
			}

			UNiagaraSystem* System = Resolve.System.Get();
			UNiagaraGraph* Graph = Resolve.Target.Graph;
			UNiagaraNodeOutput* Output = Resolve.Target.OutputNode;
			UNiagaraNodeFunctionCall* Node = Resolve.Selected;
			if (!System || !Graph || !Output || !Node
				|| Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Resolve.GraphChangeId
				|| Graph->GetPathName() != Resolve.Target.GraphPath
				|| Output->GetPathName() != Resolve.Target.OutputPath)
			{
				return ErrorResult(
					TEXT("The Niagara graph or output changed after the plan was created; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);
			}

			const int32 OriginalIndex = Resolve.SelectedIndex;
			const int32 TargetIndex = Request.TargetIndex;

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Move Niagara Stack Module")));
			System->Modify();
			Graph->Modify();
			if (!MoveStackModule(Graph, Output, Node, OriginalIndex, TargetIndex, Error))
			{
				Transaction.Cancel();
				return ErrorResult(FString::Printf(TEXT("The Niagara module move failed: %s"), *Error),
				                   TEXT("module_move_failed"), 500);
			}
			const int32 ReadBackIndex = FindModuleIndexInOrder(Output, Node);
			FCompileSummary CompileSummary = CompileSystem(System);
			if (ReadBackIndex != TargetIndex || !CompileSummary.bCompiled)
			{
				FString RestoreError;
				const bool bRestored = MoveStackModule(Graph, Output, Node, TargetIndex, OriginalIndex, RestoreError);
				const int32 RestoredIndex = bRestored ? FindModuleIndexInOrder(Output, Node) : INDEX_NONE;
				const FCompileSummary RestoreSummary = CompileSystem(System);
				if (!bRestored || RestoredIndex != OriginalIndex || !RestoreSummary.bCompiled)
				{
					return ErrorResult(
						TEXT(
							"Niagara module move failed and restoration could not be verified; the transaction was retained for Editor Undo."),
						TEXT("restore_verification_failed"), 500);
				}
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara module move read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					ReadBackIndex != TargetIndex ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}

			System->MarkPackageDirty();
			FMoveReceipt Receipt;
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = PlanDigest;
			Receipt.SystemPath = Resolve.SystemObjectPath;
			Receipt.GraphPath = Resolve.Target.GraphPath;
			Receipt.OutputPath = Resolve.Target.OutputPath;
			Receipt.ModuleScriptPath = NodeScriptPath(Node);
			Receipt.NodePath = Node->GetPathName();
			Receipt.OriginalIndex = OriginalIndex;
			Receipt.NewIndex = TargetIndex;
			Receipt.System = System;
			Receipt.Graph = Graph;
			Receipt.OutputNode = Output;
			Receipt.Node = Node;
			Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.bChanged = true;
			Receipt.bCompiled = CompileSummary.bCompiled;
			Receipt.CompileStatus = CompileSummary.Status;
			MoveReceipts().Add(Receipt.ReceiptId, Receipt);
			MoveRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeMoveResult(Receipt, false));
		}
	};

	class FTool_NiagaraModuleMoveRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.move.rollback");
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
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"), 422);
			}
			FMoveReceipt* Receipt = MoveReceipts().Find(ReceiptId);
			if (!Receipt)
			{
				return ErrorResult(
					TEXT("The Niagara module move receipt is unknown in this Editor instance."),
					TEXT("receipt_not_found"), 404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeMoveResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(
					TEXT("requestId does not match the Niagara module move receipt."), TEXT("request_id_mismatch"),
					409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraNodeOutput* Output = Receipt->OutputNode.Get();
			UNiagaraNodeFunctionCall* Node = Receipt->Node.Get();
			if (!System || !Graph || !Output || !Node)
			{
				return ErrorResult(
					TEXT("The Niagara module target is no longer loaded."), TEXT("target_unavailable"), 409);
			}
			if (Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->GraphChangeIdAfter)
			{
				return ErrorResult(
					TEXT("The Niagara graph changed after the move; rollback was refused."), TEXT("rollback_conflict"),
					409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Stack Module Move")));
			System->Modify();
			Graph->Modify();
			FString MoveError;
			if (!MoveStackModule(Graph, Output, Node, Receipt->NewIndex, Receipt->OriginalIndex, MoveError))
			{
				Transaction.Cancel();
				return ErrorResult(FString::Printf(TEXT("The Niagara module move rollback failed: %s"), *MoveError),
				                   TEXT("rollback_failed"), 500);
			}
			const int32 ReadBackIndex = FindModuleIndexInOrder(Output, Node);
			FCompileSummary CompileSummary = CompileSystem(System);
			if (ReadBackIndex != Receipt->OriginalIndex || !CompileSummary.bCompiled)
			{
				return ErrorResult(
					TEXT(
						"Niagara module move rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bCompiled = CompileSummary.bCompiled;
			Receipt->CompileStatus = CompileSummary.Status;
			return FMCPToolResult::Ok(MakeMoveResult(*Receipt, false));
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module input inline value:
	// content.niagara.graph.module.input.value.plan / .apply / .rollback.
	//
	// Plan-gated setting of an inline value on a resolved stack module input via
	// the exported override-pin API. The input is resolved with ResolveStackModule
	// plus FNiagaraStackGraphUtilities::GetStackFunctionInputs /
	// GetStackFunctionStaticSwitchPins (sharing module_not_found / ambiguous_module
	// semantics), and the value is written through
	// FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin with
	// the pin DefaultValue set directly (the owning override node is transacted).
	// Data-interface and static-switch inputs are rejected with
	// input_type_unsupported. Writes are compile + read-back verified and
	// restore-on-failure, mirroring the module add/remove/move receipt pattern.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxModuleInputNameCharacters = 512;

	struct FModuleInputRequest
	{
		FStackModuleRequest Stack;
		FString Input;
		// Optional mounted Dynamic Input GUID. When present, input resolution is
		// rooted at the selected stack module and follows only authored Dynamic
		// Input override links. A GUID found elsewhere in the graph is rejected.
		FString DynamicInputGuid;
		TSharedPtr<FJsonValue> Value;
	};

	struct FModuleInputResolve
	{
		FStackModuleResolve Module;
		FString Input;
		FString InputName;
		FName MatchedFullName;
		FNiagaraTypeDefinition InputType;
		FString TypeName;
		FName AliasedPinName;
		FString BeforeValue;
		bool bHadOverride = false;
		bool bHadOverrideNode = false;
	};

	struct FModuleInputValueReceipt
	{
		FString ReceiptId;
		FString RequestId;
		FString PlanDigest;
		FString SystemPath;
		FString GraphPath;
		FString OutputPath;
		FString ModuleScriptPath;
		FString NodePath;
		FString NodeName;
		FString InputName;
		FName MatchedFullName;
		FName AliasedPinName;
		FNiagaraTypeDefinition InputType;
		FString TypeName;
		FString BeforeValue;
		FString AfterValue;
		bool bHadOverrideBefore = false;
		bool bHadOverrideNodeBefore = false;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TWeakObjectPtr<UNiagaraNodeOutput> OutputNode;
		TWeakObjectPtr<UNiagaraNodeFunctionCall> Node;
		FString GraphChangeIdAfter;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
		bool bValueRestored = false;
	};

	TMap<FString, FModuleInputValueReceipt>& InputValueReceipts()
	{
		static TMap<FString, FModuleInputValueReceipt> Values;
		return Values;
	}

	TMap<FString, FString>& InputValueRequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	bool ParseOptionalDynamicInputGuid(
		const TSharedPtr<FJsonObject>& Params,
		FString& OutGuid,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutGuid.Reset();
		if (!Params.IsValid() || !Params->HasField(TEXT("dynamicInputGuid")))
		{
			return true;
		}
		FString RequestedGuid;
		if (!Params->TryGetStringField(TEXT("dynamicInputGuid"), RequestedGuid)
			|| RequestedGuid.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("dynamic_input_guid_invalid");
			OutError = TEXT("dynamicInputGuid must be a non-empty GUID when provided.");
			return false;
		}
		OutGuid = RequestedGuid.TrimStartAndEnd();
		FGuid Parsed;
		if (!FGuid::Parse(OutGuid, Parsed) || !Parsed.IsValid())
		{
			OutErrorCode = TEXT("dynamic_input_guid_invalid");
			OutError = TEXT("dynamicInputGuid must be a valid mounted Dynamic Input node GUID.");
			return false;
		}
		return true;
	}

	bool ParseInputValueRequest(
		const TSharedPtr<FJsonObject>& Params,
		FModuleInputRequest& Out,
		FString& OutErrorCode,
		FString& OutError)
	{
		Out = FModuleInputRequest();
		if (!ParseStackRequest(Params, Out.Stack, OutErrorCode, OutError))
		{
			return false;
		}
		if (Out.Stack.ModuleSelector.IsEmpty())
		{
			OutErrorCode = TEXT("module_selector_required");
			OutError = TEXT("moduleSelector is required to set a stack module input value.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("input"), Out.Input)
			|| Out.Input.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("input_required");
			OutError = TEXT("input must be a non-empty module input name.");
			return false;
		}
		Out.Input = Out.Input.TrimStartAndEnd();
		if (Out.Input.Len() > MaxModuleInputNameCharacters)
		{
			OutErrorCode = TEXT("input_too_long");
			OutError = FString::Printf(TEXT("input must be at most %d characters."), MaxModuleInputNameCharacters);
			return false;
		}
		if (!ParseOptionalDynamicInputGuid(Params, Out.DynamicInputGuid, OutErrorCode, OutError))
		{
			return false;
		}
		Out.Value = Params->TryGetField(TEXT("value"));
		if (!Out.Value.IsValid())
		{
			OutErrorCode = TEXT("value_required");
			OutError = TEXT("value is required and must be a JSON number, boolean, or object.");
			return false;
		}
		return true;
	}

	bool InputNameMatches(const FString& Selector, const FString& FullName)
	{
		if (Selector.IsEmpty())
		{
			return false;
		}
		if (FullName.Equals(Selector, ESearchCase::IgnoreCase))
		{
			return true;
		}
		FString Short = FullName;
		Short.RemoveFromStart(PARAM_MAP_MODULE_STR);
		return Short.Equals(Selector, ESearchCase::IgnoreCase);
	}

	// The ParameterMapSet override node immediately upstream of a function-call
	// module's parameter-map input, identified with the same structural test the
	// remove/move splice helpers use (a non-function-call Niagara node with
	// parameter-map pins on both sides).
	UNiagaraNode* FindStackOverrideNode(UNiagaraNodeFunctionCall* Node)
	{
		if (!Node)
		{
			return nullptr;
		}
		UEdGraphPin* MapIn = FindMapPin(Node, EGPD_Input);
		if (!MapIn || MapIn->LinkedTo.Num() != 1)
		{
			return nullptr;
		}
		UNiagaraNode* Upstream = Cast<UNiagaraNode>(MapIn->LinkedTo[0]->GetOwningNodeUnchecked());
		if (Upstream && IsStackOverrideNode(Upstream))
		{
			return Upstream;
		}
		return nullptr;
	}

	UEdGraphPin* FindExistingOverridePin(UNiagaraNodeFunctionCall* Node, const FName AliasedPinName)
	{
		UNiagaraNode* OverrideNode = FindStackOverrideNode(Node);
		if (!OverrideNode)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : OverrideNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && Pin->PinName == AliasedPinName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	FNiagaraParameterHandle AliasedInputHandle(UNiagaraNodeFunctionCall* Node, const FName FullName)
	{
		return FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
			FNiagaraParameterHandle(FullName), Node);
	}

	// Resolve authored Dynamic Input descendants from the selected stack module.
	// Niagara stores nested overrides on the ParameterMapSet override node that
	// precedes each function call; walking raw function-call pins misses those
	// authored links. This bounded traversal follows the same override pins used
	// by read/write commands and therefore cannot select an unrelated GUID from a
	// different module in the shared graph.
	void GetDynamicInputChildBindings(
		UNiagaraNodeFunctionCall* Parent,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		TArray<TPair<FString, UNiagaraNodeFunctionCall*>>& OutChildren)
	{
		OutChildren.Reset();
		if (!Parent)
		{
			return;
		}
		FCompileConstantResolver ConstantResolver(System, Usage);
		TArray<FNiagaraVariable> Inputs;
		TSet<FNiagaraVariable> Hidden;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			*Parent,
			Inputs,
			Hidden,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
		TSet<UNiagaraNodeFunctionCall*> Seen;
		for (const FNiagaraVariable& Input : Inputs)
		{
			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Parent, Input.GetName());
			UEdGraphPin* OverridePin = FindExistingOverridePin(Parent, Aliased.GetParameterHandleString());
			if (!OverridePin)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : OverridePin->LinkedTo)
			{
				UNiagaraNodeFunctionCall* Child = LinkedPin
					? Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNodeUnchecked())
					: nullptr;
				if (Child && Child->FunctionScript
					&& Child->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput
					&& !Seen.Contains(Child))
				{
					Seen.Add(Child);
					OutChildren.Add(TPair<FString, UNiagaraNodeFunctionCall*>(
						Input.GetName().ToString(), Child));
				}
			}
		}
	}

	void GetDynamicInputChildren(
		UNiagaraNodeFunctionCall* Parent,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		TArray<UNiagaraNodeFunctionCall*>& OutChildren)
	{
		OutChildren.Reset();
		TArray<TPair<FString, UNiagaraNodeFunctionCall*>> Bindings;
		GetDynamicInputChildBindings(Parent, System, Usage, Bindings);
		for (const TPair<FString, UNiagaraNodeFunctionCall*>& Binding : Bindings)
		{
			OutChildren.Add(Binding.Value);
		}
	}

	bool FindDynamicInputInSubtree(
		UNiagaraNodeFunctionCall* Root,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		const FGuid& WantedGuid,
		UNiagaraNodeFunctionCall*& OutNode,
		TSet<UNiagaraNodeFunctionCall*>* OutSubtree)
	{
		OutNode = nullptr;
		if (!Root)
		{
			return false;
		}
		TArray<UNiagaraNodeFunctionCall*> Pending;
		Pending.Add(Root);
		TSet<UNiagaraNodeFunctionCall*> Visited;
		while (Pending.Num() > 0)
		{
			UNiagaraNodeFunctionCall* Current = Pending.Pop(false);
			if (!Current || Visited.Contains(Current))
			{
				continue;
			}
			Visited.Add(Current);
			if (OutSubtree)
			{
				OutSubtree->Add(Current);
			}
			if (Current != Root && Current->NodeGuid == WantedGuid)
			{
				OutNode = Current;
				return true;
			}
			TArray<UNiagaraNodeFunctionCall*> Children;
			GetDynamicInputChildren(Current, System, Usage, Children);
			for (UNiagaraNodeFunctionCall* Child : Children)
			{
				Pending.Add(Child);
			}
		}
		return false;
	}

	void CollectDynamicInputSubtree(
		UNiagaraNodeFunctionCall* Root,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		TSet<UNiagaraNodeFunctionCall*>& OutNodes)
	{
		OutNodes.Reset();
		if (!Root)
		{
			return;
		}
		TArray<UNiagaraNodeFunctionCall*> Pending;
		Pending.Add(Root);
		while (Pending.Num() > 0)
		{
			UNiagaraNodeFunctionCall* Current = Pending.Pop(false);
			if (!Current || OutNodes.Contains(Current))
			{
				continue;
			}
			OutNodes.Add(Current);
			TArray<UNiagaraNodeFunctionCall*> Children;
			GetDynamicInputChildren(Current, System, Usage, Children);
			for (UNiagaraNodeFunctionCall* Child : Children)
			{
				Pending.Add(Child);
			}
		}
	}

	bool ResolveModuleInput(
		const FModuleInputRequest& Request,
		FModuleInputResolve& Out,
		FString& OutErrorCode,
		FString& OutError)
	{
		Out = FModuleInputResolve();
		if (!ResolveStackModule(Request.Stack, Out.Module, OutErrorCode, OutError))
		{
			return false;
		}
		UNiagaraNodeFunctionCall* Node = Out.Module.Selected;
		if (!Node)
		{
			OutErrorCode = TEXT("module_unavailable");
			OutError = TEXT("The selected stack module is unavailable.");
			return false;
		}
		if (!Request.DynamicInputGuid.IsEmpty())
		{
			FGuid WantedGuid;
			if (!FGuid::Parse(Request.DynamicInputGuid, WantedGuid) || !WantedGuid.IsValid())
			{
				OutErrorCode = TEXT("dynamic_input_guid_invalid");
				OutError = TEXT("dynamicInputGuid must be a valid mounted Dynamic Input node GUID.");
				return false;
			}
			const ENiagaraScriptUsage RootUsage = Out.Module.Target.OwningScript
				? Out.Module.Target.OwningScript->GetUsage()
				: ENiagaraScriptUsage::Function;
			UNiagaraNodeFunctionCall* NestedNode = nullptr;
			if (!FindDynamicInputInSubtree(
				Node, Out.Module.System.Get(), RootUsage, WantedGuid, NestedNode))
			{
				OutErrorCode = TEXT("dynamic_input_not_found");
				OutError = FString::Printf(
					TEXT("Dynamic Input node '%s' is not reachable beneath module '%s'."),
					*Request.DynamicInputGuid,
					*Node->GetName());
				return false;
			}
			// Keep the enclosing graph/output identity for plan and receipt checks,
			// while making all pin mutations target the reachable nested function.
			Out.Module.Selected = NestedNode;
			Node = NestedNode;
		}
		Out.Input = Request.Input;

		const ENiagaraScriptUsage Usage = Out.Module.Target.OwningScript
			                                  ? Out.Module.Target.OwningScript->GetUsage()
			                                  : ENiagaraScriptUsage::Function;
		FCompileConstantResolver ConstantResolver(Out.Module.System.Get(), Usage);
		if (UNiagaraSystem* System = Out.Module.System.Get())
		{
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetName().ToString().Equals(Out.Module.Target.EmitterName, ESearchCase::CaseSensitive))
				{
					ConstantResolver = FCompileConstantResolver(Handle.GetInstance(), Usage);
					break;
				}
			}
		}

		// Static-switch inputs are pins on the FunctionCall node, not parameter-map
		// entries, so they never appear in GetStackFunctionInputs. Enumerate them
		// first so a static-switch name reports input_type_unsupported rather than
		// input_not_found.
		TArray<UEdGraphPin*> StaticSwitchPins;
		TSet<UEdGraphPin*> HiddenSwitchPins;
		FNiagaraStackGraphUtilities::GetStackFunctionStaticSwitchPins(
			*Node, StaticSwitchPins, HiddenSwitchPins, ConstantResolver);
		for (UEdGraphPin* SwitchPin : StaticSwitchPins)
		{
			if (SwitchPin && InputNameMatches(Out.Input, SwitchPin->GetFName().ToString()))
			{
				OutErrorCode = TEXT("input_type_unsupported");
				OutError = FString::Printf(
					TEXT("Input '%s' is a static switch; static-switch inputs cannot be set as an inline value."),
					*Out.Input);
				return false;
			}
		}

		TArray<FNiagaraVariable> InputVariables;
		TSet<FNiagaraVariable> HiddenVariables;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			*Node,
			InputVariables,
			HiddenVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

		for (const FNiagaraVariable& Input : InputVariables)
		{
			const FString FullName = Input.GetName().ToString();
			if (!InputNameMatches(Out.Input, FullName))
			{
				continue;
			}
			if (Input.IsDataInterface() || Input.IsUObject())
			{
				OutErrorCode = TEXT("input_type_unsupported");
				OutError = FString::Printf(
					TEXT("Input '%s' is a data interface or object input and cannot be set as an inline value."),
					*Out.Input);
				return false;
			}
			Out.InputType = Input.GetType();
			Out.TypeName = Out.InputType.GetName();
			Out.MatchedFullName = Input.GetName();
			Out.InputName = FullName;
			Out.InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
			Out.AliasedPinName = AliasedInputHandle(Node, Out.MatchedFullName).GetParameterHandleString();
			Out.bHadOverrideNode = FindStackOverrideNode(Node) != nullptr;
			if (UEdGraphPin* Existing = FindExistingOverridePin(Node, Out.AliasedPinName))
			{
				Out.bHadOverride = true;
				Out.BeforeValue = Existing->DefaultValue;
			}
			return true;
		}

		TArray<FString> ValidNames;
		for (const FNiagaraVariable& Input : InputVariables)
		{
			FString Name = Input.GetName().ToString();
			Name.RemoveFromStart(PARAM_MAP_MODULE_STR);
			ValidNames.Add(Name);
		}
		for (UEdGraphPin* SwitchPin : StaticSwitchPins)
		{
			if (SwitchPin)
			{
				ValidNames.Add(SwitchPin->GetFName().ToString());
			}
		}
		OutErrorCode = TEXT("input_not_found");
		OutError = FString::Printf(
			TEXT("Input '%s' was not found on module '%s'. Valid inputs: [%s]"),
			*Out.Input,
			*Node->GetName(),
			*FString::Join(ValidNames, TEXT(", ")));
		return false;
	}

	bool IsInlineValueType(const FNiagaraTypeDefinition& Type)
	{
		return Type == FNiagaraTypeDefinition::GetFloatDef()
			|| Type == FNiagaraTypeDefinition::GetIntDef()
			|| Type == FNiagaraTypeDefinition::GetBoolDef()
			|| Type == FNiagaraTypeDefinition::GetVec2Def()
			|| Type == FNiagaraTypeDefinition::GetVec3Def()
			|| Type == FNiagaraTypeDefinition::GetPositionDef()
			|| Type == FNiagaraTypeDefinition::GetVec4Def()
			|| Type == FNiagaraTypeDefinition::GetQuatDef()
			|| Type == FNiagaraTypeDefinition::GetColorDef();
	}

	bool ReadFiniteComponent(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, float& OutValue)
	{
		double Number = 0.0;
		if (!Object.IsValid() || !Object->TryGetNumberField(Key, Number))
		{
			return false;
		}
		const float Value = static_cast<float>(Number);
		if (!FMath::IsFinite(Number) || !FMath::IsFinite(Value))
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	bool ValueToPinString(
		const FNiagaraTypeDefinition& Type,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutString)
	{
		OutString.Reset();
		if (!Value.IsValid())
		{
			return false;
		}
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			if (Value->Type != EJson::Number) return false;
			const double Number = Value->AsNumber();
			const float FloatValue = static_cast<float>(Number);
			if (!FMath::IsFinite(Number) || !FMath::IsFinite(FloatValue)) return false;
			OutString = FString::SanitizeFloat(FloatValue);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			if (Value->Type != EJson::Number) return false;
			const double Number = Value->AsNumber();
			if (!FMath::IsFinite(Number) || FMath::TruncToInt(Number) != Number
				|| Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutString = FString::FromInt(static_cast<int32>(Number));
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			if (Value->Type != EJson::Boolean) return false;
			OutString = Value->AsBool() ? TEXT("true") : TEXT("false");
			return true;
		}
		if (Value->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			if (Object->Values.Num() != 2) return false;
			float X = 0.0f, Y = 0.0f;
			if (!ReadFiniteComponent(Object, TEXT("x"), X) || !ReadFiniteComponent(Object, TEXT("y"), Y)) return false;
			OutString = FString::Printf(TEXT("%f,%f"), X, Y);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			if (Object->Values.Num() != 3) return false;
			float X = 0.0f, Y = 0.0f, Z = 0.0f;
			if (!ReadFiniteComponent(Object, TEXT("x"), X)
				|| !ReadFiniteComponent(Object, TEXT("y"), Y)
				|| !ReadFiniteComponent(Object, TEXT("z"), Z))
				return false;
			OutString = FString::Printf(TEXT("%f,%f,%f"), X, Y, Z);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			if (Object->Values.Num() != 4) return false;
			float X = 0.0f, Y = 0.0f, Z = 0.0f, W = 0.0f;
			if (!ReadFiniteComponent(Object, TEXT("x"), X)
				|| !ReadFiniteComponent(Object, TEXT("y"), Y)
				|| !ReadFiniteComponent(Object, TEXT("z"), Z)
				|| !ReadFiniteComponent(Object, TEXT("w"), W))
				return false;
			OutString = FString::Printf(TEXT("%f,%f,%f,%f"), X, Y, Z, W);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			if (Object->Values.Num() != 3 && Object->Values.Num() != 4) return false;
			float R = 0.0f, G = 0.0f, B = 0.0f, A = 1.0f;
			if (!ReadFiniteComponent(Object, TEXT("r"), R)
				|| !ReadFiniteComponent(Object, TEXT("g"), G)
				|| !ReadFiniteComponent(Object, TEXT("b"), B))
				return false;
			if (Object->HasField(TEXT("a")))
			{
				if (!ReadFiniteComponent(Object, TEXT("a"), A)) return false;
			}
			OutString = FString::Printf(TEXT("%f,%f,%f,%f"), R, G, B, A);
			return true;
		}
		return false;
	}

	TSharedRef<FJsonObject> BuildInputValuePlanJson(const FModuleInputResolve& Resolve, const FString& ValueString)
	{
		const FStackModuleResolve& Module = Resolve.Module;
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
		Plan->SetStringField(TEXT("planKind"), TEXT("niagaraModuleInputValue"));
		Plan->SetStringField(TEXT("action"), TEXT("setInputValue"));
		Plan->SetStringField(TEXT("scope"), Module.SystemObjectPath);
		Plan->SetStringField(TEXT("status"), TEXT("planned"));
		Plan->SetStringField(TEXT("system"), Module.SystemObjectPath);
		Plan->SetStringField(TEXT("emitter"), Module.Target.EmitterName);
		Plan->SetStringField(TEXT("emitterPath"), Module.Target.EmitterPath);
		Plan->SetStringField(TEXT("graph"), Module.Target.GraphPath);
		Plan->SetStringField(TEXT("outputNodePath"), Module.Target.OutputPath);
		Plan->SetStringField(TEXT("scriptUsage"), Module.Target.ScriptUsage);
		Plan->SetStringField(TEXT("moduleScript"), NodeScriptPath(Module.Selected));
		Plan->SetStringField(TEXT("nodePath"), Module.Selected->GetPathName());
		Plan->SetStringField(TEXT("nodeName"), Module.Selected->GetName());
		Plan->SetStringField(TEXT("input"), Resolve.InputName);
		Plan->SetStringField(TEXT("type"), Resolve.TypeName);
		Plan->SetStringField(TEXT("graphChangeId"), Module.GraphChangeId);
		Plan->SetBoolField(TEXT("editable"), Module.Target.bEditable);
		Plan->SetBoolField(TEXT("blocked"), Module.bBlocked);
		Plan->SetBoolField(TEXT("changesState"),
		                   !Module.bBlocked && (!Resolve.bHadOverride || Resolve.BeforeValue != ValueString));
		Plan->SetStringField(TEXT("risk"), Module.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
		Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
		Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));

		TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
		Preconditions->SetStringField(TEXT("graphChangeId"), Module.GraphChangeId);
		Preconditions->SetStringField(TEXT("graph"), Module.Target.GraphPath);
		Preconditions->SetStringField(TEXT("outputNodePath"), Module.Target.OutputPath);
		Plan->SetObjectField(TEXT("preconditions"), Preconditions);

		TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
		Before->SetBoolField(TEXT("overridePresent"), Resolve.bHadOverride);
		Before->SetStringField(TEXT("value"), Resolve.BeforeValue);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetBoolField(TEXT("overridePresent"), true);
		After->SetStringField(TEXT("value"), ValueString);
		Plan->SetObjectField(TEXT("before"), Before);
		Plan->SetObjectField(TEXT("after"), After);

		TArray<TSharedPtr<FJsonValue>> Risks;
		for (const FString& Risk : Module.Risks)
		{
			Risks.Add(MakeShared<FJsonValueString>(Risk));
		}
		Plan->SetArrayField(TEXT("risks"), Risks);
		TArray<TSharedPtr<FJsonValue>> Warnings;
		for (const FString& Warning : Module.Warnings)
		{
			Warnings.Add(MakeShared<FJsonValueString>(Warning));
		}
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT("The input value changes the Niagara graph only; it does not save or verify runtime scene state.")));
		Plan->SetArrayField(TEXT("warnings"), Warnings);
		return Plan;
	}

	TSharedRef<FJsonObject> MakeInputValueResult(const FModuleInputValueReceipt& Receipt, const bool bReplay)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-module-input-value.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("rollbackId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("outputNodePath"), Receipt.OutputPath);
		Result->SetStringField(TEXT("moduleScript"), Receipt.ModuleScriptPath);
		Result->SetStringField(TEXT("nodePath"), Receipt.NodePath);
		Result->SetStringField(TEXT("nodeName"), Receipt.NodeName);
		Result->SetStringField(TEXT("input"), Receipt.InputName);
		Result->SetStringField(TEXT("type"), Receipt.TypeName);
		Result->SetStringField(TEXT("beforeValue"), Receipt.BeforeValue);
		Result->SetStringField(TEXT("afterValue"), Receipt.AfterValue);
		Result->SetBoolField(TEXT("overridePresentBefore"), Receipt.bHadOverrideBefore);
		Result->SetBoolField(TEXT("overridePresent"), Receipt.bRolledBack ? Receipt.bHadOverrideBefore : true);
		Result->SetBoolField(TEXT("changed"), Receipt.bChanged);
		Result->SetBoolField(TEXT("verified"), true);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
		Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
		Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
		Result->SetBoolField(TEXT("valueRestored"), Receipt.bValueRestored);
		Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
		Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		return Result;
	}

	bool SpliceOutOverrideNode(
		UNiagaraGraph* Graph,
		UNiagaraNode* OverrideNode,
		UNiagaraNodeFunctionCall* ModuleNode)
	{
		if (!Graph || !OverrideNode || !ModuleNode)
		{
			return false;
		}
		UEdGraphPin* OverrideMapIn = FindMapPin(OverrideNode, EGPD_Input);
		UEdGraphPin* OverrideMapOut = FindMapPin(OverrideNode, EGPD_Output);
		UEdGraphPin* ModuleMapIn = FindMapPin(ModuleNode, EGPD_Input);
		if (!OverrideMapIn || !OverrideMapOut || !ModuleMapIn
			|| OverrideMapIn->LinkedTo.Num() != 1
			|| OverrideMapOut->LinkedTo.Num() != 1
			|| ModuleMapIn->LinkedTo.Num() != 1
			|| OverrideMapOut->LinkedTo[0] != ModuleMapIn)
		{
			return false;
		}
		UEdGraphPin* PrevMapOut = OverrideMapIn->LinkedTo[0];
		PrevMapOut->BreakAllPinLinks();
		ModuleMapIn->BreakAllPinLinks();
		OverrideNode->Modify();
		OverrideNode->BreakAllNodeLinks();
		Graph->RemoveNode(OverrideNode);
		PrevMapOut->MakeLinkTo(ModuleMapIn);
		Graph->NotifyGraphChanged();
		return !Graph->Nodes.Contains(OverrideNode);
	}

	// Restore an input override to the receipt's captured before-state. When an
	// override existed before, its DefaultValue is restored; otherwise the override
	// pin is removed (and an override node created by this write is spliced out).
	bool RestoreInputValue(
		UNiagaraGraph* Graph,
		UNiagaraNodeFunctionCall* Node,
		const FModuleInputValueReceipt& Receipt)
	{
		if (Receipt.bHadOverrideBefore)
		{
			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Receipt.MatchedFullName);
			UEdGraphPin& Pin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Receipt.InputType, FGuid(), FGuid());
			if (Pin.LinkedTo.Num() > 0)
			{
				Pin.BreakAllPinLinks();
			}
			Pin.GetOwningNodeUnchecked()->Modify();
			Pin.DefaultValue = Receipt.BeforeValue;
			Graph->NotifyGraphChanged();
			UEdGraphPin* ReadBack = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
			return ReadBack != nullptr && ReadBack->DefaultValue == Receipt.BeforeValue;
		}

		UEdGraphPin* Pin = FindExistingOverridePin(Node, Receipt.AliasedPinName);
		if (Pin)
		{
			UEdGraphNode* OverrideNode = Pin->GetOwningNodeUnchecked();
			OverrideNode->Modify();
			Pin->BreakAllPinLinks();
			OverrideNode->RemovePin(Pin);
			Graph->NotifyGraphChanged();
		}
		if (!Receipt.bHadOverrideNodeBefore)
		{
			if (UNiagaraNode* OverrideNode = FindStackOverrideNode(Node))
			{
				SpliceOutOverrideNode(Graph, OverrideNode, Node);
			}
		}
		return FindExistingOverridePin(Node, Receipt.AliasedPinName) == nullptr;
	}

	class FTool_NiagaraModuleInputValuePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.input.value.plan");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FModuleInputRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseInputValueRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			FModuleInputResolve Resolve;
			if (!ResolveModuleInput(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (!IsInlineValueType(Resolve.InputType))
			{
				return ErrorResult(
					FString::Printf(
						TEXT("Input '%s' has type '%s' which cannot hold an inline value."), *Resolve.InputName,
						*Resolve.TypeName),
					TEXT("input_type_unsupported"), 422);
			}
			FString ValueString;
			if (!ValueToPinString(Resolve.InputType, Request.Value, ValueString))
			{
				return ErrorResult(
					FString::Printf(
						TEXT("value does not match the input type '%s' or contains non-finite/out-of-range data."),
						*Resolve.TypeName),
					TEXT("value_invalid"), 422);
			}
			TSharedRef<FJsonObject> Plan = BuildInputValuePlanJson(Resolve, ValueString);
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(
					TEXT("Unable to compute the Niagara module input value plan digest."), TEXT("digest_unavailable"),
					500);
			}
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraModuleInputValueApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.input.value.apply");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return ErrorResult(
					TEXT("A non-empty requestId is required for Niagara module input value writes."),
					TEXT("request_id_required"), 422);
			}
			if (const FString* ExistingReceiptId = InputValueRequestReceiptIds().Find(RequestId))
			{
				if (FModuleInputValueReceipt* Existing = InputValueReceipts().Find(*ExistingReceiptId))
				{
					FString ErrorCode;
					FString Error;
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
					{
						return ErrorResult(Error, ErrorCode, 409);
					}
					return FMCPToolResult::Ok(MakeInputValueResult(*Existing, true));
				}
				return ErrorResult(
					TEXT("requestId is associated with an unavailable Niagara module input value receipt."),
					TEXT("request_id_conflict"), 409);
			}

			FModuleInputRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseInputValueRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			FModuleInputResolve Resolve;
			if (!ResolveModuleInput(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}
			if (!IsInlineValueType(Resolve.InputType))
			{
				return ErrorResult(
					FString::Printf(
						TEXT("Input '%s' has type '%s' which cannot hold an inline value."), *Resolve.InputName,
						*Resolve.TypeName),
					TEXT("input_type_unsupported"), 422);
			}
			FString ValueString;
			if (!ValueToPinString(Resolve.InputType, Request.Value, ValueString))
			{
				return ErrorResult(
					FString::Printf(
						TEXT("value does not match the input type '%s' or contains non-finite/out-of-range data."),
						*Resolve.TypeName),
					TEXT("value_invalid"), 422);
			}
			TSharedRef<FJsonObject> Plan = BuildInputValuePlanJson(Resolve, ValueString);
			FString PlanDigest;
			if (!TryDigestJson(Plan, PlanDigest))
			{
				return ErrorResult(
					TEXT("Unable to compute the Niagara module input value plan digest."), TEXT("digest_unavailable"),
					500);
			}
			if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 409);
			}
			if (Resolve.Module.bBlocked)
			{
				return ErrorResult(
					TEXT(
						"The Niagara module input value plan is blocked because the selected graph is shared or read-only."),
					TEXT("plan_blocked"), 409);
			}

			UNiagaraSystem* System = Resolve.Module.System.Get();
			UNiagaraGraph* Graph = Resolve.Module.Target.Graph;
			UNiagaraNodeOutput* Output = Resolve.Module.Target.OutputNode;
			UNiagaraNodeFunctionCall* Node = Resolve.Module.Selected;
			if (!System || !Graph || !Output || !Node
				|| Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Resolve.Module.GraphChangeId
				|| Graph->GetPathName() != Resolve.Module.Target.GraphPath
				|| Output->GetPathName() != Resolve.Module.Target.OutputPath)
			{
				return ErrorResult(
					TEXT("The Niagara graph or output changed after the plan was created; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);
			}

			// The receipt carries the before-state captured by ResolveModuleInput.
			FModuleInputValueReceipt Receipt;
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = PlanDigest;
			Receipt.SystemPath = Resolve.Module.SystemObjectPath;
			Receipt.GraphPath = Resolve.Module.Target.GraphPath;
			Receipt.OutputPath = Resolve.Module.Target.OutputPath;
			Receipt.ModuleScriptPath = NodeScriptPath(Node);
			Receipt.NodePath = Node->GetPathName();
			Receipt.NodeName = Node->GetName();
			Receipt.InputName = Resolve.InputName;
			Receipt.MatchedFullName = Resolve.MatchedFullName;
			Receipt.AliasedPinName = Resolve.AliasedPinName;
			Receipt.InputType = Resolve.InputType;
			Receipt.TypeName = Resolve.TypeName;
			Receipt.BeforeValue = Resolve.BeforeValue;
			Receipt.AfterValue = ValueString;
			Receipt.bHadOverrideBefore = Resolve.bHadOverride;
			Receipt.bHadOverrideNodeBefore = Resolve.bHadOverrideNode;
			Receipt.System = System;
			Receipt.Graph = Graph;
			Receipt.OutputNode = Output;
			Receipt.Node = Node;
			Receipt.bChanged = !Resolve.bHadOverride || Resolve.BeforeValue != ValueString;

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Set Niagara Stack Module Input Value")));
			System->Modify();
			Graph->Modify();
			Node->Modify();

			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Resolve.MatchedFullName);
			UEdGraphPin* OverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Resolve.InputType, FGuid(), FGuid());
			if (OverridePin->LinkedTo.Num() > 0)
			{
				OverridePin->BreakAllPinLinks();
			}
			OverridePin->GetOwningNodeUnchecked()->Modify();
			OverridePin->DefaultValue = ValueString;
			Graph->NotifyGraphChanged();

			UEdGraphPin* ReadBackPin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
			const bool bReadBack = ReadBackPin != nullptr && ReadBackPin->DefaultValue == ValueString;
			const FCompileSummary CompileSummary = CompileSystem(System);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				const bool bRestored = RestoreInputValue(Graph, Node, Receipt);
				const FCompileSummary RestoreSummary = CompileSystem(System);
				if (!bRestored || !RestoreSummary.bCompiled)
				{
					return ErrorResult(
						TEXT(
							"Niagara module input value write failed and restoration could not be verified; the transaction was retained for Editor Undo."),
						TEXT("restore_verification_failed"), 500);
				}
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara module input value read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}

			System->MarkPackageDirty();
			Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.bCompiled = CompileSummary.bCompiled;
			Receipt.CompileStatus = CompileSummary.Status;
			InputValueReceipts().Add(Receipt.ReceiptId, Receipt);
			InputValueRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeInputValueResult(Receipt, false));
		}
	};

	class FTool_NiagaraModuleInputValueRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.input.value.rollback");
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
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"), 422);
			}
			FModuleInputValueReceipt* Receipt = InputValueReceipts().Find(ReceiptId);
			if (!Receipt)
			{
				return ErrorResult(
					TEXT("The Niagara module input value receipt is unknown in this Editor instance."),
					TEXT("receipt_not_found"), 404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeInputValueResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(
					TEXT("requestId does not match the Niagara module input value receipt."),
					TEXT("request_id_mismatch"), 409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraNodeFunctionCall* Node = Receipt->Node.Get();
			if (!System || !Graph || !Node)
			{
				return ErrorResult(
					TEXT("The Niagara module input value target is no longer loaded."), TEXT("target_unavailable"),
					409);
			}
			if (Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->GraphChangeIdAfter)
			{
				return ErrorResult(
					TEXT("The Niagara graph changed after the input value was applied; rollback was refused."),
					TEXT("rollback_conflict"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Stack Module Input Value")));
			System->Modify();
			Graph->Modify();
			Node->Modify();
			const bool bRestored = RestoreInputValue(Graph, Node, *Receipt);
			const FCompileSummary CompileSummary = CompileSystem(System);
			if (!bRestored || !CompileSummary.bCompiled)
			{
				return ErrorResult(
					TEXT(
						"Niagara module input value rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bValueRestored = true;
			Receipt->bCompiled = CompileSummary.bCompiled;
			Receipt->CompileStatus = CompileSummary.Status;
			return FMCPToolResult::Ok(MakeInputValueResult(*Receipt, false));
		}
	};

	// ---------------------------------------------------------------------------
	// Emitter-wide module clear: content.niagara.emitter.modules.clear.
	//
	// A DIRECT command (not plan-gated, no receipt): it removes every
	// function-call module node from every stack of one emitter (particle
	// spawn/update, event handlers, simulation stages, ...) inside a single
	// FScopedTransaction. It is atomic and receipt-less: any read-back or
	// compile failure cancels the transaction and restores the authored graph.
	// An emitter that already has zero modules is an idempotent success
	// (removedCount 0) and succeeds before any write guard applies, because a
	// no-op clear changes nothing. An actual write still requires a non-transient
	// /Game/ System and an owned emitter (mirroring the renderer material guard).
	// ---------------------------------------------------------------------------

	struct FEmitterModulesClearRequest
	{
		FString SystemPath;
		FString EmitterSelector;
	};

	struct FEmitterModuleNode
	{
		UNiagaraGraph* Graph = nullptr;
		UNiagaraNodeFunctionCall* Node = nullptr;
	};

	bool ParseEmitterModulesClearRequest(
		const TSharedPtr<FJsonObject>& Params,
		FEmitterModulesClearRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutRequest = FEmitterModulesClearRequest();
		if (!Params.IsValid())
		{
			OutErrorCode = TEXT("invalid_request");
			OutError = TEXT("A Niagara emitter module clear request is required.");
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
			OutError = TEXT("emitter is required.");
			return false;
		}
		OutRequest.SystemPath = OutRequest.SystemPath.TrimStartAndEnd();
		OutRequest.EmitterSelector = OutRequest.EmitterSelector.TrimStartAndEnd();
		return true;
	}

	bool EmitterModulesClearSystemWritable(const UNiagaraSystem* System)
	{
		return System
			&& System->GetOutermost()
			&& System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))
			&& !System->HasAnyFlags(RF_Transient);
	}

	bool FindEmitterHandle(
		UNiagaraSystem* System,
		const FString& Selector,
		const FNiagaraEmitterHandle*& OutHandle,
		FString& OutErrorCode,
		FString& OutError)
	{
		FGuid Id;
		const bool bId = FGuid::Parse(Selector, Id);
		TArray<const FNiagaraEmitterHandle*> Matches;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const bool bMatch = bId
				                    ? Handle.GetId() == Id
				                    : Handle.GetName().ToString().Equals(Selector, ESearchCase::IgnoreCase);
			if (bMatch)
			{
				Matches.Add(&Handle);
			}
		}
		if (Matches.Num() == 0)
		{
			OutErrorCode = TEXT("emitter_not_found");
			OutError = FString::Printf(
				TEXT("Niagara emitter '%s' was not found in system '%s'."),
				*Selector,
				*System->GetPathName());
			return false;
		}
		if (Matches.Num() > 1)
		{
			OutErrorCode = TEXT("ambiguous_emitter");
			OutError = FString::Printf(
				TEXT("Niagara emitter '%s' is ambiguous; use its exact handle ID."),
				*Selector);
			return false;
		}
		OutHandle = Matches[0];
		return true;
	}

	// Enumerate every function-call module node across the emitter's script
	// graphs. GetScripts(false, false) covers particle spawn/update, emitter
	// spawn/update, event handlers and simulation stages; each script's output
	// node is resolved with FindOutputForScript and its stack walked with
	// BuildStackOrder, then deduplicated by node pointer (a node belongs to
	// exactly one output stack even when several scripts share one graph).
	bool CollectEmitterModuleNodes(
		const FNiagaraEmitterHandle& Handle,
		TArray<FEmitterModuleNode>& OutNodes,
		FString& OutError)
	{
		OutNodes.Reset();
		OutError.Reset();
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data)
		{
			OutError = TEXT("The emitter exposes no versioned emitter data.");
			return false;
		}
		TArray<UNiagaraScript*> Scripts;
		Data->GetScripts(Scripts, false, false);
		TSet<UNiagaraNodeFunctionCall*> Seen;
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
			TArray<UNiagaraNode*> Ordered;
			FString StackError;
			if (!BuildStackOrder(Output, Ordered, StackError))
			{
				OutError = FString::Printf(
					TEXT("Emitter stack '%s' is malformed: %s"),
					*Output->GetPathName(),
					*StackError);
				return false;
			}
			for (UNiagaraNode* Node : Ordered)
			{
				if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
				{
					if (!Seen.Contains(FunctionCall))
					{
						Seen.Add(FunctionCall);
						FEmitterModuleNode Entry;
						Entry.Graph = Source->NodeGraph;
						Entry.Node = FunctionCall;
						OutNodes.Add(Entry);
					}
				}
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> MakeEmitterModulesClearResult(
		const FString& SystemPath,
		const FString& EmitterName,
		const FString& EmitterId,
		const int32 RemovedCount)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.emitter-modules-clear.v1"));
		Result->SetStringField(TEXT("system"), SystemPath);
		Result->SetStringField(TEXT("emitter"), EmitterName);
		Result->SetStringField(TEXT("emitterId"), EmitterId);
		Result->SetNumberField(TEXT("removedCount"), RemovedCount);
		Result->SetNumberField(TEXT("remainingCount"), 0);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compiled"), true);
		Result->SetStringField(
			TEXT("scope"),
			TEXT("authored module stack cleared; runtime unverified"));
		return Result;
	}

	class FTool_NiagaraEmitterModulesClear final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.emitter.modules.clear");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FEmitterModulesClearRequest Request;
			FString ErrorCode;
			FString Error;
			if (!ParseEmitterModulesClearRequest(Params, Request, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}

			const FString SystemPath = NormalizeObjectPath(Request.SystemPath);
			if (SystemPath.IsEmpty())
			{
				return ErrorResult(
					TEXT("system must be a valid object or package path."), TEXT("invalid_object_path"), 422);
			}
			UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
			if (!System)
			{
				return ErrorResult(
					FString::Printf(TEXT("Niagara System '%s' was not found."), *Request.SystemPath),
					TEXT("system_not_found"), 404);
			}

			const FNiagaraEmitterHandle* Handle = nullptr;
			if (!FindEmitterHandle(System, Request.EmitterSelector, Handle, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 409);
			}
			const FString EmitterName = Handle->GetName().ToString();
			const FString EmitterId = Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower);

			TArray<FEmitterModuleNode> ModuleNodes;
			if (!CollectEmitterModuleNodes(*Handle, ModuleNodes, Error))
			{
				return ErrorResult(Error, TEXT("invalid_stack"), 422);
			}
			const int32 RemovedCount = ModuleNodes.Num();

			// Idempotent no-op: an already-empty emitter is a successful clear that
			// changes nothing, so it succeeds before any write guard applies.
			if (RemovedCount == 0)
			{
				return FMCPToolResult::Ok(MakeEmitterModulesClearResult(
					SystemPath, EmitterName, EmitterId, 0));
			}

			if (!EmitterModulesClearSystemWritable(System))
			{
				return ErrorResult(
					TEXT("Writes require a non-transient /Game/ Niagara System."),
					TEXT("system_read_only"), 409);
			}
			const UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
			if (!Emitter || Emitter->GetOutermost() != System->GetOutermost())
			{
				return ErrorResult(
					TEXT("Writes require an emitter owned by the System package."),
					TEXT("emitter_read_only"), 409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Clear Niagara Emitter Modules")));
			System->Modify();
			TSet<UNiagaraGraph*> TouchedGraphs;
			for (const FEmitterModuleNode& Module : ModuleNodes)
			{
				if (Module.Graph && !TouchedGraphs.Contains(Module.Graph))
				{
					TouchedGraphs.Add(Module.Graph);
					Module.Graph->Modify();
				}
			}
			for (const FEmitterModuleNode& Module : ModuleNodes)
			{
				if (!SpliceOutStackNode(Module.Graph, Module.Node))
				{
					Transaction.Cancel();
					return ErrorResult(
						TEXT("One of the emitter's stack modules could not be removed; the clear was rolled back."),
						TEXT("module_clear_failed"), 500);
				}
			}

			TArray<FEmitterModuleNode> ReadBackNodes;
			const bool bReadBack = CollectEmitterModuleNodes(*Handle, ReadBackNodes, Error)
				&& ReadBackNodes.Num() == 0;
			FCompileSummary CompileSummary = CompileSystem(System);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara emitter module clear read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}

			System->MarkPackageDirty();
			return FMCPToolResult::Ok(MakeEmitterModulesClearResult(
				SystemPath, EmitterName, EmitterId, RemovedCount));
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module input binding:
	// content.niagara.graph.module.input.binding.set.
	//
	// A DIRECT command (not plan-gated, no receipt): it links a resolved stack
	// module input's override pin to a linked parameter (a system/user parameter
	// or an attribute) through the exported
	// FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput. The
	// input is resolved with ResolveModuleInput (sharing input_not_found /
	// input_type_unsupported semantics), and the linked parameter is validated
	// against the known system/user parameters (user parameters plus engine
	// constants and common particle attributes). The write is atomic via a single
	// FScopedTransaction: any read-back or compile failure cancels the transaction
	// and restores the authored graph. Only /Game/ authored systems are writable.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxBindingParameterCharacters = 512;

	bool ParseInputBindingRequest(
		const TSharedPtr<FJsonObject>& Params,
		FModuleInputRequest& OutRequest,
		FString& OutParameter,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutRequest = FModuleInputRequest();
		OutParameter.Reset();
		if (!ParseStackRequest(Params, OutRequest.Stack, OutErrorCode, OutError))
		{
			return false;
		}
		if (OutRequest.Stack.ModuleSelector.IsEmpty())
		{
			OutErrorCode = TEXT("module_selector_required");
			OutError = TEXT("moduleSelector is required to bind a stack module input.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("input"), OutRequest.Input)
			|| OutRequest.Input.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("input_required");
			OutError = TEXT("input must be a non-empty module input name.");
			return false;
		}
		OutRequest.Input = OutRequest.Input.TrimStartAndEnd();
		if (OutRequest.Input.Len() > MaxModuleInputNameCharacters)
		{
			OutErrorCode = TEXT("input_too_long");
			OutError = FString::Printf(TEXT("input must be at most %d characters."), MaxModuleInputNameCharacters);
			return false;
		}
		if (!ParseOptionalDynamicInputGuid(
			Params, OutRequest.DynamicInputGuid, OutErrorCode, OutError))
		{
			return false;
		}
		if (!Params->TryGetStringField(TEXT("parameter"), OutParameter)
			|| OutParameter.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("parameter_required");
			OutError = TEXT("parameter must be a non-empty linked parameter name (e.g. User.X or an attribute).");
			return false;
		}
		OutParameter = OutParameter.TrimStartAndEnd();
		if (OutParameter.Len() > MaxBindingParameterCharacters)
		{
			OutErrorCode = TEXT("parameter_too_long");
			OutError = FString::Printf(TEXT("parameter must be at most %d characters."), MaxBindingParameterCharacters);
			return false;
		}
		return true;
	}

	// Build the set of parameters a module input may link to. The editor's own
	// GetParametersForContext (graph reference map + user parameters) is not
	// exported, so this mirrors it with exported APIs only: system user parameters
	// (User.*), engine/system constants (Engine.*, System.*, Emitter.*, ...) and
	// common particle attributes (Particles.*). OutOrdered preserves a
	// deterministic match order with user parameters first so a bare name resolves
	// to a user parameter before any namespaced constant.
	void BuildKnownBindingParameters(
		UNiagaraSystem* System,
		TSet<FNiagaraVariableBase>& OutKnown,
		TArray<FNiagaraVariableBase>& OutOrdered)
	{
		OutKnown.Reset();
		OutOrdered.Reset();
		if (!System)
		{
			return;
		}
		auto AddKnown = [&OutKnown, &OutOrdered](const FNiagaraVariableBase& Variable)
		{
			if (!OutKnown.Contains(Variable))
			{
				OutKnown.Add(Variable);
				OutOrdered.Add(Variable);
			}
		};
		TArray<FNiagaraVariable> UserParameters;
		System->GetExposedParameters().GetUserParameters(UserParameters);
		for (FNiagaraVariable Variable : UserParameters)
		{
			FNiagaraUserRedirectionParameterStore::MakeUserVariable(Variable);
			AddKnown(Variable);
		}
		for (const FNiagaraVariable& Constant : FNiagaraConstants::GetEngineConstants())
		{
			AddKnown(Constant);
		}
		for (const FNiagaraVariable& Attribute : FNiagaraConstants::GetCommonParticleAttributes())
		{
			AddKnown(Attribute);
		}
		for (const FNiagaraVariable& Constant : FNiagaraConstants::GetStaticSwitchConstants())
		{
			AddKnown(Constant);
		}
	}

	// Match the requested linked-parameter name against the known set, mirroring
	// the user-parameter lookup in Niagara_SystemParameters.cpp: an optional
	// "User." prefix is stripped and names compare case-insensitively. Attributes
	// and constants require their full namespaced name (e.g. "Particles.Position").
	bool ResolveLinkedParameter(
		const TArray<FNiagaraVariableBase>& Ordered,
		const FString& Selector,
		FString& OutFullName,
		FString& OutErrorCode,
		FString& OutError)
	{
		const FString Stripped = Selector.StartsWith(TEXT("User."), ESearchCase::IgnoreCase)
			                         ? Selector.RightChop(5)
			                         : Selector;
		for (const FNiagaraVariableBase& Candidate : Ordered)
		{
			const FString FullName = Candidate.GetName().ToString();
			const bool bFullMatch = FullName.Equals(Selector, ESearchCase::IgnoreCase);
			bool bBareMatch = false;
			if (FullName.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
			{
				FString Bare = FullName;
				Bare.RightChopInline(5, EAllowShrinking::No);
				bBareMatch = Bare.Equals(Stripped, ESearchCase::IgnoreCase);
			}
			if (bFullMatch || bBareMatch)
			{
				OutFullName = FullName;
				return true;
			}
		}
		OutErrorCode = TEXT("parameter_not_found");
		OutError = FString::Printf(
			TEXT("Linked parameter '%s' is not a known system or user parameter."),
			*Selector);
		return false;
	}

	// The linked-value read-back: the override pin must have exactly one link to an
	// output pin whose name is the linked parameter's full name. The engine's own
	// GetLinkedValueHandleForFunctionInput is not exported and additionally casts
	// to a Private node class, so the structural check is sufficient here.
	bool IsOverridePinLinkedToParameter(UEdGraphPin* OverridePin, const FName ParameterName)
	{
		if (!OverridePin || OverridePin->LinkedTo.Num() != 1)
		{
			return false;
		}
		UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
		if (!LinkedPin || LinkedPin->Direction != EGPD_Output)
		{
			return false;
		}
		return LinkedPin->PinName.ToString().Equals(ParameterName.ToString(), ESearchCase::IgnoreCase);
	}

	class FTool_NiagaraModuleInputBindingSet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.input.binding.set");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FModuleInputRequest Request;
			FString ParameterSelector;
			FString ErrorCode;
			FString Error;
			if (!ParseInputBindingRequest(Params, Request, ParameterSelector, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			FModuleInputResolve Resolve;
			if (!ResolveModuleInput(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}

			UNiagaraSystem* System = Resolve.Module.System.Get();
			UNiagaraGraph* Graph = Resolve.Module.Target.Graph;
			UNiagaraNodeFunctionCall* Node = Resolve.Module.Selected;
			if (!System || !Graph || !Node)
			{
				return ErrorResult(
					TEXT("The selected Niagara stack module is unavailable."), TEXT("module_unavailable"), 409);
			}
			if (Resolve.Module.bBlocked)
			{
				return ErrorResult(
					TEXT(
						"The Niagara module input binding is blocked because the selected graph is shared or read-only."),
					TEXT("plan_blocked"), 409);
			}

			TSet<FNiagaraVariableBase> Known;
			TArray<FNiagaraVariableBase> Ordered;
			BuildKnownBindingParameters(System, Known, Ordered);
			FString LinkedName;
			if (!ResolveLinkedParameter(Ordered, ParameterSelector, LinkedName, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 404);
			}

			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Resolve.MatchedFullName);

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Bind Niagara Stack Module Input")));
			System->Modify();
			Graph->Modify();
			Node->Modify();

			UEdGraphPin* OverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Resolve.InputType, FGuid(), FGuid());
			// A binding replaces any prior link or inline value on the override pin;
			// break existing links so the linked-parameter API can attach a fresh
			// parameter-map get node (it asserts the pin is unlinked).
			if (OverridePin->LinkedTo.Num() > 0)
			{
				OverridePin->BreakAllPinLinks();
			}
			const FNiagaraVariableBase LinkedParameter(Resolve.InputType, FName(*LinkedName));
			FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
				*OverridePin,
				LinkedParameter,
				Known,
				ENiagaraDefaultMode::FailIfPreviouslyNotSet,
				FGuid());
			Graph->NotifyGraphChanged();

			UEdGraphPin* ReadBackPin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
			const bool bReadBack = IsOverridePinLinkedToParameter(ReadBackPin, FName(*LinkedName));
			const FCompileSummary CompileSummary = CompileSystem(System);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara module input binding read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}

			System->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.module-input-binding.v1"));
			Result->SetStringField(TEXT("system"), Resolve.Module.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Module.Target.EmitterName);
			Result->SetStringField(TEXT("moduleSelector"), Request.Stack.ModuleSelector);
			Result->SetStringField(TEXT("input"), Resolve.InputName);
			Result->SetStringField(TEXT("parameter"), LinkedName);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), true);
			Result->SetStringField(TEXT("scope"), TEXT("authored binding; runtime resolution unverified"));
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Stack module input dynamic input:
	// content.niagara.graph.module.input.di.set.
	//
	// A DIRECT command (not plan-gated, no receipt): it sets a resolved stack
	// module input's override pin to a Dynamic Input script through the exported
	// FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput. The input is
	// resolved with ResolveModuleInput (sharing input_not_found /
	// input_type_unsupported semantics), and the dynamic-input script is loaded and
	// validated as usage == DynamicInput. The write is atomic via a single
	// FScopedTransaction: any read-back or compile failure cancels the transaction
	// and restores the authored graph. Only /Game/ authored systems are writable.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxDynamicInputPathCharacters = 512;

	bool ParseInputDIRequest(
		const TSharedPtr<FJsonObject>& Params,
		FModuleInputRequest& OutRequest,
		FString& OutDynamicInput,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutRequest = FModuleInputRequest();
		OutDynamicInput.Reset();
		if (!ParseStackRequest(Params, OutRequest.Stack, OutErrorCode, OutError))
		{
			return false;
		}
		if (OutRequest.Stack.ModuleSelector.IsEmpty())
		{
			OutErrorCode = TEXT("module_selector_required");
			OutError = TEXT("moduleSelector is required to set a stack module input dynamic input.");
			return false;
		}
		if (!Params->TryGetStringField(TEXT("input"), OutRequest.Input)
			|| OutRequest.Input.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("input_required");
			OutError = TEXT("input must be a non-empty module input name.");
			return false;
		}
		OutRequest.Input = OutRequest.Input.TrimStartAndEnd();
		if (OutRequest.Input.Len() > MaxModuleInputNameCharacters)
		{
			OutErrorCode = TEXT("input_too_long");
			OutError = FString::Printf(TEXT("input must be at most %d characters."), MaxModuleInputNameCharacters);
			return false;
		}
		if (!ParseOptionalDynamicInputGuid(
			Params, OutRequest.DynamicInputGuid, OutErrorCode, OutError))
		{
			return false;
		}
		if (!Params->TryGetStringField(TEXT("dynamicInput"), OutDynamicInput)
			|| OutDynamicInput.TrimStartAndEnd().IsEmpty())
		{
			OutErrorCode = TEXT("dynamic_input_required");
			OutError = TEXT("dynamicInput must be a Dynamic Input script object or package path.");
			return false;
		}
		OutDynamicInput = OutDynamicInput.TrimStartAndEnd();
		if (OutDynamicInput.Len() > MaxDynamicInputPathCharacters)
		{
			OutErrorCode = TEXT("dynamic_input_too_long");
			OutError = FString::Printf(
				TEXT("dynamicInput must be at most %d characters."), MaxDynamicInputPathCharacters);
			return false;
		}
		return true;
	}

	// The dynamic-input read-back: the override pin must have exactly one link to an
	// output pin owned by a UNiagaraNodeFunctionCall whose FunctionScript is the
	// authored dynamic-input script (or any DynamicInput script), matching the node
	// SetDynamicInputForFunctionInput attaches to the pin.
	bool IsOverridePinDynamicInput(UEdGraphPin* OverridePin, UNiagaraScript* DynamicInputScript)
	{
		if (!OverridePin || OverridePin->LinkedTo.Num() != 1)
		{
			return false;
		}
		UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
		if (!LinkedPin || LinkedPin->Direction != EGPD_Output)
		{
			return false;
		}
		UNiagaraNodeFunctionCall* DynamicInputNode = Cast<
			UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNodeUnchecked());
		if (!DynamicInputNode)
		{
			return false;
		}
		if (DynamicInputNode->FunctionScript == DynamicInputScript)
		{
			return true;
		}
		return DynamicInputNode->FunctionScript != nullptr
			&& DynamicInputNode->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput;
	}

	// Remove only the Dynamic Input node currently mounted on an override pin.
	// Literal values and parameter bindings are deliberately preserved so the
	// direct set command cannot destroy a non-DI authoring choice by accident.
	bool RemoveMountedDynamicInput(
		UNiagaraGraph* Graph,
		UNiagaraNodeFunctionCall* Module,
		UEdGraphPin* OverridePin)
	{
		if (!Graph || !Module || !OverridePin || OverridePin->LinkedTo.Num() != 1)
		{
			return false;
		}
		UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
		UNiagaraNodeFunctionCall* DynamicNode = LinkedPin
			? Cast<UNiagaraNodeFunctionCall>(LinkedPin->GetOwningNodeUnchecked())
			: nullptr;
		if (!DynamicNode || !DynamicNode->FunctionScript
			|| DynamicNode->FunctionScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			return false;
		}
		UEdGraphNode* OverrideNode = OverridePin->GetOwningNodeUnchecked();
		OverridePin->BreakAllPinLinks();
		DynamicNode->Modify();
		DynamicNode->BreakAllNodeLinks();
		Graph->RemoveNode(DynamicNode);
		if (OverrideNode && IsStackOverrideNode(OverrideNode))
		{
			bool bHasOtherInputs = false;
			for (UEdGraphPin* Pin : OverrideNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input
					&& Pin != FindMapPin(Cast<UNiagaraNode>(OverrideNode), EGPD_Input)
					&& Pin->PinName != TEXT("Add")
					&& (Pin->LinkedTo.Num() > 0 || !Pin->DefaultValue.IsEmpty()))
				{
					bHasOtherInputs = true;
					break;
				}
			}
			if (!bHasOtherInputs)
			{
				SpliceOutOverrideNode(Graph, Cast<UNiagaraNode>(OverrideNode), Module);
			}
		}
		return !Graph->Nodes.Contains(DynamicNode);
	}

	class FTool_NiagaraModuleInputDISet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.input.di.set");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FModuleInputRequest Request;
			FString DynamicInputSelector;
			FString ErrorCode;
			FString Error;
			if (!ParseInputDIRequest(Params, Request, DynamicInputSelector, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, 422);
			}
			FModuleInputResolve Resolve;
			if (!ResolveModuleInput(Request, Resolve, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}

			const FString DynamicInputPath = NormalizeObjectPath(DynamicInputSelector);
			UNiagaraScript* DynamicInputScript = DynamicInputPath.IsEmpty()
				                                     ? nullptr
				                                     : LoadObject<UNiagaraScript>(nullptr, *DynamicInputPath);
			if (!DynamicInputScript || DynamicInputScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
			{
				return ErrorResult(
					FString::Printf(
						TEXT("Dynamic Input script '%s' was not found or is not a Dynamic Input."),
						*DynamicInputSelector),
					TEXT("dynamic_input_not_found"), 404);
			}

			UNiagaraSystem* System = Resolve.Module.System.Get();
			UNiagaraGraph* Graph = Resolve.Module.Target.Graph;
			UNiagaraNodeFunctionCall* Node = Resolve.Module.Selected;
			if (!System || !Graph || !Node)
			{
				return ErrorResult(
					TEXT("The selected Niagara stack module is unavailable."), TEXT("module_unavailable"), 409);
			}
			if (Resolve.Module.bBlocked)
			{
				return ErrorResult(
					TEXT(
						"The Niagara module input dynamic input is blocked because the selected graph is shared or read-only."),
					TEXT("plan_blocked"), 409);
			}

			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Resolve.MatchedFullName);

			FScopedTransaction Transaction(
				FText::FromString(TEXT("UE AI Set Niagara Stack Module Input Dynamic Input")));
			System->Modify();
			Graph->Modify();
			Node->Modify();

			UEdGraphPin* OverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Resolve.InputType, FGuid(), FGuid());
			// A dynamic input replaces any prior link or inline value on the override
			// pin; break existing links and clear the default so the engine's
			// SetDynamicInputForFunctionInput (which asserts the pin is unlinked) can
			// attach a fresh dynamic-input node.
			if (OverridePin->LinkedTo.Num() > 0)
			{
				// A prior Dynamic Input node is owned by this override and must be
				// removed; otherwise repeated set operations leave unreachable graph
				// nodes that still participate in compilation and tree readback.
				if (!RemoveMountedDynamicInput(Graph, Node, OverridePin))
				{
					OverridePin->BreakAllPinLinks();
				}
			}
			// Removing the last mounted node may splice the override node out;
			// reacquire the pin before clearing its default and mounting the new DI.
			OverridePin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Resolve.InputType, FGuid(), FGuid());
			OverridePin->GetOwningNodeUnchecked()->Modify();
			OverridePin->DefaultValue = FString();
			UNiagaraNodeFunctionCall* DynamicInputNode = nullptr;
			FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(
				*OverridePin, DynamicInputScript, DynamicInputNode);
			Graph->NotifyGraphChanged();

			UEdGraphPin* ReadBackPin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
			const bool bReadBack = IsOverridePinDynamicInput(ReadBackPin, DynamicInputScript);
			const FCompileSummary CompileSummary = CompileSystem(System);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT(
							"Niagara module input dynamic input read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status),
					!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}

			System->MarkPackageDirty();

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.module-input-di.v1"));
			Result->SetStringField(TEXT("system"), Resolve.Module.SystemObjectPath);
			Result->SetStringField(TEXT("emitter"), Resolve.Module.Target.EmitterName);
			Result->SetStringField(TEXT("moduleSelector"), Request.Stack.ModuleSelector);
			Result->SetStringField(TEXT("input"), Resolve.InputName);
			Result->SetStringField(TEXT("dynamicInput"), DynamicInputSelector);
			Result->SetStringField(TEXT("dynamicInputPath"), DynamicInputScript->GetPathName());
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetBoolField(TEXT("compiled"), true);
			Result->SetStringField(TEXT("scope"), TEXT("authored dynamic input; runtime resolution unverified"));
			return FMCPToolResult::Ok(Result);
		}
	};

	// ---------------------------------------------------------------------------
	// Mounted Dynamic Input authoring:
	// content.niagara.graph.module.dynamic_input.add.{plan,apply,rollback}.
	//
	// Unlike the legacy input.di.set command, this chain is deliberately
	// additive: it refuses an input that already has an authored override.  That
	// makes the receipt rollback unambiguous and avoids silently destroying a
	// literal, binding, or previously mounted Dynamic Input.
	// ---------------------------------------------------------------------------

	struct FDynamicInputAddRequest
	{
		FModuleInputRequest Input;
		FString DynamicInput;
	};

	struct FDynamicInputAddResolve
	{
		FModuleInputResolve Input;
		FString DynamicInputPath;
		TWeakObjectPtr<UNiagaraScript> DynamicInputScript;
		bool bOverridePresent = false;
	};

	struct FDynamicInputAddReceipt
	{
		FString ReceiptId;
		FString RequestId;
		FString PlanDigest;
		FString SystemPath;
		FString GraphPath;
		FString OutputPath;
		FString ModuleScriptPath;
		FString NodePath;
		FString InputName;
		FString DynamicInputPath;
		FString GraphChangeIdAfter;
		FName MatchedFullName;
		FName AliasedPinName;
		FNiagaraTypeDefinition InputType;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TWeakObjectPtr<UNiagaraNodeFunctionCall> ModuleNode;
		TWeakObjectPtr<UNiagaraNodeFunctionCall> DynamicNode;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
	};

	TMap<FString, FDynamicInputAddReceipt>& DynamicInputAddReceipts()
	{
		static TMap<FString, FDynamicInputAddReceipt> Values;
		return Values;
	}

	TMap<FString, FString>& DynamicInputAddRequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	bool ParseDynamicInputAddRequest(
		const TSharedPtr<FJsonObject>& Params,
		FDynamicInputAddRequest& Out,
		FString& OutErrorCode,
		FString& OutError)
	{
		Out = FDynamicInputAddRequest();
		if (!ParseInputDIRequest(Params, Out.Input, Out.DynamicInput, OutErrorCode, OutError))
		{
			return false;
		}
		return true;
	}

	bool ResolveDynamicInputAdd(
		const FDynamicInputAddRequest& Request,
		FDynamicInputAddResolve& Out,
		FString& OutErrorCode,
		FString& OutError)
	{
		Out = FDynamicInputAddResolve();
		if (!ResolveModuleInput(Request.Input, Out.Input, OutErrorCode, OutError))
		{
			return false;
		}
		Out.DynamicInputPath = NormalizeObjectPath(Request.DynamicInput);
		UNiagaraScript* Script = Out.DynamicInputPath.IsEmpty()
			                         ? nullptr
			                         : LoadObject<UNiagaraScript>(nullptr, *Out.DynamicInputPath);
		if (!Script || Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
		{
			OutErrorCode = TEXT("dynamic_input_not_found");
			OutError = FString::Printf(
				TEXT("Dynamic Input script '%s' was not found or is not a Dynamic Input."),
				*Request.DynamicInput);
			return false;
		}
		Out.DynamicInputScript = Script;
		const FNiagaraParameterHandle Aliased = AliasedInputHandle(
			Out.Input.Module.Selected, Out.Input.MatchedFullName);
		UEdGraphPin* Existing = FindExistingOverridePin(
			Out.Input.Module.Selected, Aliased.GetParameterHandleString());
		Out.bOverridePresent = Existing && (Existing->LinkedTo.Num() > 0 || !Existing->DefaultValue.IsEmpty());
		if (Out.bOverridePresent)
		{
			OutErrorCode = TEXT("input_override_present");
			OutError = FString::Printf(
				TEXT("Input '%s' already has an authored override; use input.di.set or clear it first."),
				*Out.Input.InputName);
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> BuildDynamicInputAddPlan(
		const FDynamicInputAddResolve& Resolve)
	{
		TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
		const FModuleInputResolve& Input = Resolve.Input;
		Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
		Plan->SetStringField(TEXT("planKind"), TEXT("niagaraDynamicInputAdd"));
		Plan->SetStringField(TEXT("action"), TEXT("addDynamicInput"));
		Plan->SetStringField(TEXT("scope"), Input.Module.SystemObjectPath);
		Plan->SetStringField(TEXT("status"), TEXT("planned"));
		Plan->SetStringField(TEXT("system"), Input.Module.SystemObjectPath);
		Plan->SetStringField(TEXT("emitter"), Input.Module.Target.EmitterName);
		Plan->SetStringField(TEXT("graph"), Input.Module.Target.GraphPath);
		Plan->SetStringField(TEXT("outputNodePath"), Input.Module.Target.OutputPath);
		Plan->SetStringField(TEXT("moduleSelector"), Input.Module.Selected->GetName());
		Plan->SetStringField(TEXT("moduleScript"), NodeScriptPath(Input.Module.Selected));
		Plan->SetStringField(TEXT("input"), Input.InputName);
		Plan->SetStringField(TEXT("type"), Input.TypeName);
		Plan->SetStringField(TEXT("dynamicInput"), Resolve.DynamicInputPath);
		Plan->SetStringField(TEXT("graphChangeId"), Input.Module.GraphChangeId);
		Plan->SetBoolField(TEXT("blocked"), Input.Module.bBlocked);
		Plan->SetBoolField(TEXT("changesState"), !Input.Module.bBlocked);
		Plan->SetStringField(TEXT("risk"), Input.Module.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
		Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
		Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
		TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
		Preconditions->SetStringField(TEXT("graphChangeId"), Input.Module.GraphChangeId);
		Preconditions->SetBoolField(TEXT("inputOverridePresent"), false);
		Plan->SetObjectField(TEXT("preconditions"), Preconditions);
		TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetBoolField(TEXT("overridePresent"), true);
		After->SetStringField(TEXT("dynamicInput"), Resolve.DynamicInputPath);
		Plan->SetObjectField(TEXT("after"), After);
		TArray<TSharedPtr<FJsonValue>> Warnings;
		Warnings.Add(MakeShared<FJsonValueString>(
			TEXT(
				"The Dynamic Input is authored in the graph only; the package is not saved and runtime resolution is unverified.")));
		Plan->SetArrayField(TEXT("warnings"), Warnings);
		return Plan;
	}

	TSharedRef<FJsonObject> MakeDynamicInputAddResult(
		const FDynamicInputAddReceipt& Receipt,
		const bool bReplay)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.dynamic-input-add.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("rollbackId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("outputNodePath"), Receipt.OutputPath);
		Result->SetStringField(TEXT("moduleScript"), Receipt.ModuleScriptPath);
		Result->SetStringField(TEXT("nodePath"), Receipt.NodePath);
		Result->SetStringField(TEXT("input"), Receipt.InputName);
		Result->SetStringField(TEXT("dynamicInput"), Receipt.DynamicInputPath);
		Result->SetStringField(TEXT("dynamicInputNodeGuid"), Receipt.DynamicNode.IsValid()
			                                                     ? Receipt.DynamicNode->NodeGuid.ToString(
				                                                     EGuidFormats::DigitsWithHyphensLower)
			                                                     : FString());
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

	bool RemoveAddedDynamicInput(FDynamicInputAddReceipt& Receipt)
	{
		UNiagaraGraph* Graph = Receipt.Graph.Get();
		UNiagaraNodeFunctionCall* Module = Receipt.ModuleNode.Get();
		UNiagaraNodeFunctionCall* Dynamic = Receipt.DynamicNode.Get();
		if (!Graph || !Module || !Dynamic)
		{
			return false;
		}
		const FNiagaraParameterHandle Aliased = AliasedInputHandle(Module, Receipt.MatchedFullName);
		UEdGraphPin* OverridePin = FindExistingOverridePin(Module, Aliased.GetParameterHandleString());
		if (!OverridePin || OverridePin->LinkedTo.Num() != 1
			|| OverridePin->LinkedTo[0]->GetOwningNodeUnchecked() != Dynamic)
		{
			return false;
		}
		UEdGraphNode* OverrideNode = OverridePin->GetOwningNodeUnchecked();
		OverridePin->BreakAllPinLinks();
		Dynamic->Modify();
		Dynamic->BreakAllNodeLinks();
		Graph->RemoveNode(Dynamic);
		if (OverrideNode && IsStackOverrideNode(OverrideNode))
		{
			bool bHasOtherInputs = false;
			for (UEdGraphPin* Pin : OverrideNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin != FindMapPin(
						Cast<UNiagaraNode>(OverrideNode), EGPD_Input)
					&& Pin->PinName != TEXT("Add") && (Pin->LinkedTo.Num() > 0 || !Pin->DefaultValue.IsEmpty()))
				{
					bHasOtherInputs = true;
					break;
				}
			}
			if (!bHasOtherInputs)
			{
				SpliceOutOverrideNode(Graph, Cast<UNiagaraNode>(OverrideNode), Module);
			}
		}
		Graph->NotifyGraphChanged();
		return !Graph->Nodes.Contains(Dynamic);
	}

	class FTool_NiagaraDynamicInputAddPlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.graph.module.dynamic_input.add.plan"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FDynamicInputAddRequest Request;
			FString ErrorCode, Error;
			if (!ParseDynamicInputAddRequest(Params, Request, ErrorCode, Error))
				return ErrorResult(
					Error, ErrorCode, 422);
			FDynamicInputAddResolve Resolve;
			if (!ResolveDynamicInputAdd(Request, Resolve, ErrorCode, Error))
				return ErrorResult(
					Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			if (Resolve.Input.Module.bBlocked)
				return ErrorResult(
					TEXT("The selected Niagara graph is shared or read-only."), TEXT("plan_blocked"), 409);
			TSharedRef<FJsonObject> Plan = BuildDynamicInputAddPlan(Resolve);
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
				return ErrorResult(
					TEXT("Unable to compute the Dynamic Input plan digest."), TEXT("digest_unavailable"), 500);
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraDynamicInputAddApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.dynamic_input.add.apply");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
				return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"), 422);
			if (const FString* ExistingId = DynamicInputAddRequestReceiptIds().Find(RequestId))
			{
				if (FDynamicInputAddReceipt* Existing = DynamicInputAddReceipts().Find(*ExistingId))
				{
					FString ErrorCode, Error;
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, ErrorCode, Error))
						return ErrorResult(
							Error, ErrorCode, 409);
					return FMCPToolResult::Ok(MakeDynamicInputAddResult(*Existing, true));
				}
				return ErrorResult(
					TEXT("requestId is associated with an unavailable Dynamic Input receipt."),
					TEXT("request_id_conflict"), 409);
			}
			FDynamicInputAddRequest Request;
			FString ErrorCode, Error;
			if (!ParseDynamicInputAddRequest(Params, Request, ErrorCode, Error))
				return ErrorResult(
					Error, ErrorCode, 422);
			FDynamicInputAddResolve Resolve;
			if (!ResolveDynamicInputAdd(Request, Resolve, ErrorCode, Error))
				return ErrorResult(
					Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildDynamicInputAddPlan(Resolve);
			FString PlanDigest;
			if (!TryDigestJson(Plan, PlanDigest))
				return ErrorResult(
					TEXT("Unable to compute the Dynamic Input plan digest."), TEXT("digest_unavailable"), 500);
			if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
				return
					ErrorResult(Error, ErrorCode, 409);
			if (Resolve.Input.Module.bBlocked)
				return ErrorResult(
					TEXT("The selected Niagara graph is shared or read-only."), TEXT("plan_blocked"), 409);
			UNiagaraSystem* System = Resolve.Input.Module.System.Get();
			UNiagaraGraph* Graph = Resolve.Input.Module.Target.Graph;
			UNiagaraNodeFunctionCall* Module = Resolve.Input.Module.Selected;
			UNiagaraScript* Script = Resolve.DynamicInputScript.Get();
			if (!System || !Graph || !Module || !Script || Graph->GetChangeID().
			                                                      ToString(EGuidFormats::DigitsWithHyphensLower) !=
				Resolve.Input.Module.GraphChangeId)
				return ErrorResult(
					TEXT("The Niagara graph changed after the plan; re-plan before applying."),
					TEXT("plan_digest_mismatch"), 409);
			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Module, Resolve.Input.MatchedFullName);
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Add Niagara Dynamic Input")));
			System->Modify();
			Graph->Modify();
			Module->Modify();
			UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Module, Aliased, Resolve.Input.InputType, FGuid(), FGuid());
			if (OverridePin.LinkedTo.Num() > 0 || !OverridePin.DefaultValue.IsEmpty())
			{
				Transaction.Cancel();
				return ErrorResult(
					TEXT("The input acquired an override while the plan was pending; re-plan before applying."),
					TEXT("input_override_present"), 409);
			}
			UNiagaraNodeFunctionCall* DynamicNode = nullptr;
			FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(OverridePin, Script, DynamicNode);
			Graph->NotifyGraphChanged();
			UEdGraphPin* ReadBackPin = FindExistingOverridePin(Module, Aliased.GetParameterHandleString());
			const bool bReadBack = IsOverridePinDynamicInput(ReadBackPin, Script) && DynamicNode != nullptr;
			const FCompileSummary CompileSummary = CompileSystem(System);
			if (!bReadBack || !CompileSummary.bCompiled)
			{
				FDynamicInputAddReceipt Failed;
				Failed.Graph = Graph;
				Failed.ModuleNode = Module;
				Failed.DynamicNode = DynamicNode;
				Failed.MatchedFullName = Resolve.Input.MatchedFullName;
				RemoveAddedDynamicInput(Failed);
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(
						TEXT("Dynamic Input read-back or compilation failed (status=%s); the change was restored."),
						*CompileSummary.Status), !bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"),
					500);
			}
			FDynamicInputAddReceipt Receipt;
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = PlanDigest;
			Receipt.SystemPath = Resolve.Input.Module.SystemObjectPath;
			Receipt.GraphPath = Resolve.Input.Module.Target.GraphPath;
			Receipt.OutputPath = Resolve.Input.Module.Target.OutputPath;
			Receipt.ModuleScriptPath = NodeScriptPath(Module);
			Receipt.NodePath = Module->GetPathName();
			Receipt.InputName = Resolve.Input.InputName;
			Receipt.DynamicInputPath = Resolve.DynamicInputPath;
			Receipt.GraphChangeIdAfter = Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.MatchedFullName = Resolve.Input.MatchedFullName;
			Receipt.AliasedPinName = Aliased.GetParameterHandleString();
			Receipt.InputType = Resolve.Input.InputType;
			Receipt.System = System;
			Receipt.Graph = Graph;
			Receipt.ModuleNode = Module;
			Receipt.DynamicNode = DynamicNode;
			Receipt.bChanged = true;
			Receipt.bCompiled = CompileSummary.bCompiled;
			Receipt.CompileStatus = CompileSummary.Status;
			System->MarkPackageDirty();
			DynamicInputAddReceipts().Add(Receipt.ReceiptId, Receipt);
			DynamicInputAddRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeDynamicInputAddResult(Receipt, false));
		}
	};

	class FTool_NiagaraDynamicInputAddRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.graph.module.dynamic_input.add.rollback");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId, RequestId;
			bool bConfirm = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId) || !Params->
				TryGetStringField(TEXT("requestId"), RequestId) || !Params->
				TryGetBoolField(TEXT("confirmWrite"), bConfirm) || ReceiptId.IsEmpty() || RequestId.IsEmpty() || !
				bConfirm)
				return ErrorResult(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"),
					422);
			FDynamicInputAddReceipt* Receipt = DynamicInputAddReceipts().Find(ReceiptId);
			if (!Receipt)
				return ErrorResult(
					TEXT("The Dynamic Input receipt is unknown in this Editor instance."), TEXT("receipt_not_found"),
					404);
			if (Receipt->bRolledBack) return FMCPToolResult::Ok(MakeDynamicInputAddResult(*Receipt, true));
			if (Receipt->RequestId != RequestId)
				return ErrorResult(
					TEXT("requestId does not match the Dynamic Input receipt."), TEXT("request_id_mismatch"), 409);
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraSystem* System = Receipt->System.Get();
			if (!Graph || !System || Graph->GetChangeID().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->
				GraphChangeIdAfter)
				return ErrorResult(
					TEXT("The Niagara graph changed after apply; rollback was refused."), TEXT("rollback_conflict"),
					409);
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Dynamic Input")));
			System->Modify();
			Graph->Modify();
			if (!RemoveAddedDynamicInput(*Receipt))
			{
				Transaction.Cancel();
				return ErrorResult(
					TEXT("The Dynamic Input could not be removed without overwriting newer graph links."),
					TEXT("rollback_conflict"), 409);
			}
			const FCompileSummary CompileSummary = CompileSystem(System);
			if (Graph->Nodes.Contains(Receipt->DynamicNode.Get()) || !CompileSummary.bCompiled)
				return ErrorResult(
					TEXT(
						"Dynamic Input rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
					TEXT("rollback_verification_failed"), 500);
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bCompiled = CompileSummary.bCompiled;
			Receipt->CompileStatus = CompileSummary.Status;
			return FMCPToolResult::Ok(MakeDynamicInputAddResult(*Receipt, false));
		}
	};

	// ---------------------------------------------------------------------------
	// Authored-system spec export: content.niagara.system.spec.export.
	//
	// Read-only snapshot of a full authored system for round-trip/import: user
	// parameters (with default values), emitters (renderers + stack modules), and
	// each stack module's inputs with their value / binding / dynamic-input state.
	// It reuses BuildStackOrder, FindOutputForScript, UsageName, NodeScriptPath,
	// AliasedInputHandle, FindStackOverrideNode and FindExistingOverridePin, and
	// reads user parameters through the system's FNiagaraUserRedirectionParameterStore
	// (GetExposedParameters). The export never compiles and never saves; it is
	// bounded to 4096 emitters, stacks, modules and inputs with truncated flags.
	// ---------------------------------------------------------------------------

	constexpr int32 SpecExportMaxEmitters = 4096;
	constexpr int32 SpecExportMaxStacks = 4096;
	constexpr int32 SpecExportMaxModules = 4096;
	constexpr int32 SpecExportMaxInputs = 4096;
	constexpr int32 SpecExportMaxUserParameters = 4096;
	constexpr int32 SpecExportMaxDynamicInputNodes = 512;

	TSharedPtr<FJsonValue> SpecExportFiniteNumber(const double Value)
	{
		if (FMath::IsFinite(Value))
		{
			return MakeShared<FJsonValueNumber>(Value);
		}
		return MakeShared<FJsonValueNull>();
	}

	// Serialize a user-parameter default value for the primitive/vector types the
	// system parameter store can read back; bOutSupported is false for data
	// interfaces, objects and other non-inline types, in which case the caller
	// omits the "default" field.
	TSharedPtr<FJsonValue> SpecExportUserDefault(
		const FNiagaraVariable& Variable,
		const FNiagaraUserRedirectionParameterStore& Store,
		bool& bOutSupported)
	{
		bOutSupported = true;
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return SpecExportFiniteNumber(Store.GetParameterValue<float>(Variable));
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return MakeShared<FJsonValueNumber>(Store.GetParameterValue<int32>(Variable));
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			const FNiagaraBool Value = Store.GetParameterValue<FNiagaraBool>(Variable);
			if (Value.IsValid())
			{
				return MakeShared<FJsonValueBoolean>(Value.GetValue());
			}
			return MakeShared<FJsonValueNull>();
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			const FVector2f Value = Store.GetParameterValue<FVector2f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), SpecExportFiniteNumber(Value.X));
			Json->SetField(TEXT("y"), SpecExportFiniteNumber(Value.Y));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			const FVector3f Value = Store.GetParameterValue<FVector3f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), SpecExportFiniteNumber(Value.X));
			Json->SetField(TEXT("y"), SpecExportFiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), SpecExportFiniteNumber(Value.Z));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			const FVector4f Value = Store.GetParameterValue<FVector4f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), SpecExportFiniteNumber(Value.X));
			Json->SetField(TEXT("y"), SpecExportFiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), SpecExportFiniteNumber(Value.Z));
			Json->SetField(TEXT("w"), SpecExportFiniteNumber(Value.W));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			const FQuat4f Value = Store.GetParameterValue<FQuat4f>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), SpecExportFiniteNumber(Value.X));
			Json->SetField(TEXT("y"), SpecExportFiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), SpecExportFiniteNumber(Value.Z));
			Json->SetField(TEXT("w"), SpecExportFiniteNumber(Value.W));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const FLinearColor Value = Store.GetParameterValue<FLinearColor>(Variable);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("r"), SpecExportFiniteNumber(Value.R));
			Json->SetField(TEXT("g"), SpecExportFiniteNumber(Value.G));
			Json->SetField(TEXT("b"), SpecExportFiniteNumber(Value.B));
			Json->SetField(TEXT("a"), SpecExportFiniteNumber(Value.A));
			return MakeShared<FJsonValueObject>(Json);
		}
		bOutSupported = false;
		return MakeShared<FJsonValueNull>();
	}

	TSharedRef<FJsonObject> SpecExportBuildInput(
		UNiagaraNodeFunctionCall* Node,
		const FNiagaraVariable& Input,
		UNiagaraSystem* System,
		int32* SharedDynamicInputBudget = nullptr,
		TSet<UNiagaraNodeFunctionCall*>* SharedDynamicInputActive = nullptr);

	TSharedRef<FJsonObject> SpecExportBuildDynamicNode(
		UNiagaraNodeFunctionCall* Node,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		int32 Depth,
		int32& NodeBudget,
		TSet<UNiagaraNodeFunctionCall*>& Active);

	TSharedRef<FJsonObject> SpecExportBuildDynamicNode(
		UNiagaraNodeFunctionCall* Node,
		UNiagaraSystem* System,
		ENiagaraScriptUsage Usage,
		int32 Depth,
		int32& NodeBudget,
		TSet<UNiagaraNodeFunctionCall*>& Active)
	{
		auto Result = MakeShared<FJsonObject>();
		if (!Node || Active.Contains(Node) || Depth > 16 || NodeBudget-- <= 0)
		{
			Result->SetBoolField(TEXT("truncated"), true);
			return Result;
		}
		Active.Add(Node);
		const ENiagaraScriptUsage NodeUsage = Node->FunctionScript
			? Node->FunctionScript->GetUsage()
			: Usage;
		Result->SetStringField(TEXT("name"), Node->GetFunctionName());
		Result->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetStringField(TEXT("scriptPath"), NodeScriptPath(Node));
		const FCompileConstantResolver ConstantResolver(System, NodeUsage);
		TArray<FNiagaraVariable> InputVariables;
		TSet<FNiagaraVariable> HiddenVariables;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			*Node,
			InputVariables,
			HiddenVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (const FNiagaraVariable& Input : InputVariables)
		{
			Inputs.Add(MakeShared<FJsonValueObject>(SpecExportBuildInput(
				Node, Input, System, &NodeBudget, &Active)));
		}
		Result->SetArrayField(TEXT("inputs"), Inputs);
		TArray<TPair<FString, UNiagaraNodeFunctionCall*>> Bindings;
		GetDynamicInputChildBindings(Node, System, NodeUsage, Bindings);
		TArray<TSharedPtr<FJsonValue>> Children;
		for (const TPair<FString, UNiagaraNodeFunctionCall*>& Binding : Bindings)
		{
			auto Child = SpecExportBuildDynamicNode(
				Binding.Value, System, NodeUsage, Depth + 1, NodeBudget, Active);
			Child->SetStringField(TEXT("input"), Binding.Key);
			Children.Add(MakeShared<FJsonValueObject>(Child));
		}
		Result->SetArrayField(TEXT("children"), Children);
		Active.Remove(Node);
		return Result;
	}

	TSharedRef<FJsonObject> SpecExportBuildInput(
		UNiagaraNodeFunctionCall* Node,
		const FNiagaraVariable& Input,
		UNiagaraSystem* System,
		int32* SharedDynamicInputBudget,
		TSet<UNiagaraNodeFunctionCall*>* SharedDynamicInputActive)
	{
		auto Entry = MakeShared<FJsonObject>();
		FString InputName = Input.GetName().ToString();
		InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
		Entry->SetStringField(TEXT("name"), InputName);
		Entry->SetStringField(TEXT("type"), Input.GetType().GetName());

		if (Node)
		{
			// Mirror the write path's override-pin resolution: an input with an
			// override node holds its value/binding/dynamic-input state on the
			// aliased pin of that override node.
			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Input.GetName());
			if (UEdGraphPin* OverridePin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString()))
			{
				if (OverridePin->LinkedTo.Num() > 0 && OverridePin->LinkedTo[0])
				{
					UEdGraphPin* LinkedPin = OverridePin->LinkedTo[0];
					UNiagaraNodeFunctionCall* LinkedCall = Cast<UNiagaraNodeFunctionCall>(
						LinkedPin->GetOwningNodeUnchecked());
					if (LinkedCall
						&& LinkedCall->FunctionScript
						&& LinkedCall->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput)
					{
						Entry->SetStringField(TEXT("dynamicInput"), NodeScriptPath(LinkedCall));
						Entry->SetStringField(
							TEXT("dynamicInputGuid"),
							LinkedCall->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
						int32 LocalDynamicInputBudget = SpecExportMaxDynamicInputNodes;
						int32& DynamicInputBudget = SharedDynamicInputBudget
							? *SharedDynamicInputBudget
							: LocalDynamicInputBudget;
						TSet<UNiagaraNodeFunctionCall*> LocalActive;
						TSet<UNiagaraNodeFunctionCall*>& Active = SharedDynamicInputActive
							? *SharedDynamicInputActive
							: LocalActive;
						const ENiagaraScriptUsage Usage = Node->FunctionScript
							? Node->FunctionScript->GetUsage()
							: ENiagaraScriptUsage::Function;
						Entry->SetObjectField(
							TEXT("dynamicInputTree"),
							SpecExportBuildDynamicNode(
								LinkedCall, System, Usage, 0, DynamicInputBudget, Active));
					}
					else
					{
						// A linked parameter-map get pin exposes the full parameter
						// name (User.X, Particles.Position, ...) as its pin name.
						Entry->SetStringField(TEXT("binding"), LinkedPin->PinName.ToString());
					}
				}
				else if (!OverridePin->DefaultValue.IsEmpty())
				{
					Entry->SetStringField(TEXT("value"), OverridePin->DefaultValue);
				}
			}
		}
		return Entry;
	}

	TSharedRef<FJsonObject> SpecExportBuildModule(
		UNiagaraNodeFunctionCall* Node,
		const FCompileConstantResolver& ConstantResolver,
		bool& bOutInputsTruncated,
		UNiagaraSystem* System)
	{
		auto Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("scriptPath"), NodeScriptPath(Node));
		Entry->SetStringField(TEXT("nodePath"), Node ? Node->GetPathName() : FString());
		Entry->SetStringField(
			TEXT("nodeGuid"),
			Node ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());

		TArray<FNiagaraVariable> InputVariables;
		TSet<FNiagaraVariable> HiddenVariables;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			*Node,
			InputVariables,
			HiddenVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

		TArray<TSharedPtr<FJsonValue>> Inputs;
		const int32 Count = FMath::Min(InputVariables.Num(), SpecExportMaxInputs);
		Inputs.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Inputs.Add(MakeShared<FJsonValueObject>(SpecExportBuildInput(Node, InputVariables[Index], System)));
		}
		Entry->SetArrayField(TEXT("inputs"), Inputs);
		const bool bInputsTruncated = InputVariables.Num() > SpecExportMaxInputs;
		Entry->SetBoolField(TEXT("inputsTruncated"), bInputsTruncated);
		bOutInputsTruncated = bOutInputsTruncated || bInputsTruncated;
		return Entry;
	}

	TSharedRef<FJsonObject> SpecExportBuildStack(
		UNiagaraNodeOutput* Output,
		UNiagaraScript* Script,
		const FCompileConstantResolver& ConstantResolver,
		bool& bOutModulesTruncated,
		bool& bOutInputsTruncated,
		UNiagaraSystem* System)
	{
		auto Stack = MakeShared<FJsonObject>();
		Stack->SetStringField(TEXT("stackName"), UsageName(Script->GetUsage()));

		TArray<TSharedPtr<FJsonValue>> Modules;
		TArray<UNiagaraNode*> Ordered;
		FString StackError;
		if (BuildStackOrder(Output, Ordered, StackError))
		{
			int32 ModuleIndex = 0;
			for (UNiagaraNode* Node : Ordered)
			{
				if (UNiagaraNodeFunctionCall* FunctionCall = Cast<UNiagaraNodeFunctionCall>(Node))
				{
					if (ModuleIndex >= SpecExportMaxModules)
					{
						bOutModulesTruncated = true;
						break;
					}
					Modules.Add(MakeShared<FJsonValueObject>(SpecExportBuildModule(
						FunctionCall, ConstantResolver, bOutInputsTruncated, System)));
					++ModuleIndex;
				}
			}
		}
		Stack->SetArrayField(TEXT("modules"), Modules);
		return Stack;
	}

	FString SpecCanonicalRendererPropertyText(const FName PropertyName, FString Value)
	{
		// Niagara normalizes this binding's cached-value bookkeeping while
		// importing a renderer property.  It is derived editor metadata, not an
		// authored renderer setting, so omit it from the wire contract to keep
		// export -> import -> export stable.
		if (PropertyName == TEXT("RendererEnabledBinding"))
		{
			Value.ReplaceInline(TEXT(",bIsCachedParticleValue=True"), TEXT(""), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT(",bIsCachedParticleValue=False"), TEXT(""), ESearchCase::CaseSensitive);
		}
		return Value;
	}

	TSharedRef<FJsonObject> SpecExportBuildRenderer(UNiagaraRendererProperties* Renderer)
	{
		auto Result = MakeShared<FJsonObject>();
		if (!Renderer) return Result;
		Result->SetStringField(TEXT("rendererClass"), Renderer->GetClass()->GetPathName());
		Result->SetStringField(TEXT("rendererPath"), Renderer->GetPathName());
		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		int32 PropertyCount = 0;
		bool bTruncated = false;
		for (TFieldIterator<FProperty> It(Renderer->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property
				|| !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
				|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
			{
				continue;
			}
			if (PropertyCount >= SpecExportMaxInputs)
			{
				bTruncated = true;
				break;
			}
			FString Value;
			Property->ExportTextItem_Direct(
				Value,
				Property->ContainerPtrToValuePtr<void>(Renderer),
				nullptr,
				nullptr,
				PPF_None);
			Value = SpecCanonicalRendererPropertyText(Property->GetFName(), MoveTemp(Value));
			Properties->SetStringField(Property->GetName(), Value.Left(64 * 1024));
			++PropertyCount;
		}
		Result->SetObjectField(TEXT("properties"), Properties);
		Result->SetBoolField(TEXT("propertiesTruncated"), bTruncated);
		return Result;
	}

	TSharedRef<FJsonObject> SpecExportBuildEmitter(
		const FNiagaraEmitterHandle& Handle,
		bool& bOutStacksTruncated,
		bool& bOutModulesTruncated,
		bool& bOutInputsTruncated,
		UNiagaraSystem* System)
	{
		auto Emitter = MakeShared<FJsonObject>();
		Emitter->SetStringField(TEXT("name"), Handle.GetName().ToString());
		Emitter->SetStringField(TEXT("id"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Emitter->SetBoolField(TEXT("isEnabled"), Handle.GetIsEnabled());

		TArray<TSharedPtr<FJsonValue>> Renderers;
		TArray<TSharedPtr<FJsonValue>> Stacks;
		if (FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
			{
				if (Renderer)
				{
					Renderers.Add(MakeShared<FJsonValueObject>(SpecExportBuildRenderer(Renderer)));
				}
			}

			TArray<UNiagaraScript*> Scripts;
			Data->GetScripts(Scripts, false, false);
			int32 StackIndex = 0;
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
				if (StackIndex >= SpecExportMaxStacks)
				{
					bOutStacksTruncated = true;
					break;
				}
				const FCompileConstantResolver ConstantResolver(Handle.GetInstance(), Script->GetUsage());
				Stacks.Add(MakeShared<FJsonValueObject>(SpecExportBuildStack(
					Output, Script, ConstantResolver,
					bOutModulesTruncated, bOutInputsTruncated, System)));
				++StackIndex;
			}
		}
		Emitter->SetArrayField(TEXT("renderers"), Renderers);
		Emitter->SetArrayField(TEXT("stacks"), Stacks);
		return Emitter;
	}

	bool SpecContentDigest(const TSharedPtr<FJsonObject>& Spec, FString& OutDigest);

	class FTool_NiagaraSystemSpecExport final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("content.niagara.system.spec.export");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString SystemSelector;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), SystemSelector)
				|| SystemSelector.TrimStartAndEnd().IsEmpty())
			{
				return ErrorResult(TEXT("system is required."), TEXT("system_required"), 422);
			}
			const FString SystemPath = NormalizeObjectPath(SystemSelector.TrimStartAndEnd());
			if (SystemPath.IsEmpty())
			{
				return ErrorResult(
					TEXT("system must be a valid object or package path."), TEXT("invalid_object_path"), 422);
			}
			UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
			if (!System)
			{
				return ErrorResult(
					FString::Printf(TEXT("Niagara System '%s' was not found."), *SystemSelector),
					TEXT("system_not_found"), 404);
			}

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-spec.v1"));
			Json->SetNumberField(TEXT("specVersion"), 1);
			Json->SetStringField(TEXT("system"), System->GetName());
			Json->SetStringField(TEXT("systemPath"), System->GetPathName());
			Json->SetBoolField(TEXT("bounded"), true);

			// User parameters (authored defaults), sorted for determinism.
			const FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			TArray<FNiagaraVariable> UserParameters;
			UserParameters.Reserve(Store.ReadParameterVariables().Num());
			for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables())
			{
				UserParameters.Add(Variable);
			}
			UserParameters.Sort([](const FNiagaraVariable& Left, const FNiagaraVariable& Right)
			{
				const FString LeftName = Left.GetName().ToString();
				const FString RightName = Right.GetName().ToString();
				if (LeftName != RightName)
				{
					return LeftName < RightName;
				}
				return Left.GetType().GetName() < Right.GetType().GetName();
			});
			TArray<TSharedPtr<FJsonValue>> UserParameterRows;
			const int32 UserParameterCount = FMath::Min(UserParameters.Num(), SpecExportMaxUserParameters);
			UserParameterRows.Reserve(UserParameterCount);
			for (int32 Index = 0; Index < UserParameterCount; ++Index)
			{
				const FNiagaraVariable& Variable = UserParameters[Index];
				auto Parameter = MakeShared<FJsonObject>();
				Parameter->SetStringField(TEXT("name"), Variable.GetName().ToString());
				Parameter->SetStringField(TEXT("type"), Variable.GetType().GetName());
				bool bDefaultSupported = false;
				const TSharedPtr<FJsonValue> DefaultValue = SpecExportUserDefault(Variable, Store, bDefaultSupported);
				if (bDefaultSupported)
				{
					Parameter->SetField(TEXT("default"), DefaultValue);
				}
				UserParameterRows.Add(MakeShared<FJsonValueObject>(Parameter));
			}
			Json->SetArrayField(TEXT("userParameters"), UserParameterRows);
			Json->SetBoolField(TEXT("userParametersTruncated"), UserParameters.Num() > SpecExportMaxUserParameters);

			// Emitters (renderers + stacks + modules + inputs), all bounded.
			const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
			bool bStacksTruncated = false;
			bool bModulesTruncated = false;
			bool bInputsTruncated = false;
			TArray<TSharedPtr<FJsonValue>> Emitters;
			const int32 EmitterCount = FMath::Min(Handles.Num(), SpecExportMaxEmitters);
			Emitters.Reserve(EmitterCount);
			for (int32 Index = 0; Index < EmitterCount; ++Index)
			{
				Emitters.Add(MakeShared<FJsonValueObject>(SpecExportBuildEmitter(
					Handles[Index], bStacksTruncated, bModulesTruncated, bInputsTruncated, System)));
			}
			Json->SetArrayField(TEXT("emitters"), Emitters);
			Json->SetBoolField(TEXT("emittersTruncated"), Handles.Num() > SpecExportMaxEmitters);
			Json->SetBoolField(TEXT("stacksTruncated"), bStacksTruncated);
			Json->SetBoolField(TEXT("modulesTruncated"), bModulesTruncated);
			Json->SetBoolField(TEXT("inputsTruncated"), bInputsTruncated);

			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("compiled"), false);
			// The digest excludes itself and is stable across JSON key ordering.  It
			// gives import/round-trip callers an exact source snapshot identity while
			// keeping the export read-only.
			FString SpecDigest;
			if (!SpecContentDigest(Json, SpecDigest))
			{
				return ErrorResult(
					TEXT("Could not compute the Niagara System spec digest."), TEXT("digest_unavailable"), 500);
			}
			Json->SetStringField(TEXT("specDigest"), SpecDigest);
			return FMCPToolResult::Ok(Json);
		}
	};

	// A spec carries its own digest for transport, so digesting the object as
	// supplied would make a valid export fail round-trip (the digest would be
	// included on the second pass). Keep the transport field outside the
	// canonical content digest while retaining every authored field.
	bool SpecContentDigest(const TSharedPtr<FJsonObject>& Spec, FString& OutDigest)
	{
		if (!Spec.IsValid())
		{
			return false;
		}
		TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Spec->Values)
		{
			const bool bTransportField = Pair.Key.Equals(TEXT("specDigest"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("localDeclared"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("handlerRegistered"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("liveAvailable"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("executed"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("readbackVerified"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("runtimeVerified"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("verificationState"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("ok"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("errorCode"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("message"), ESearchCase::CaseSensitive)
				|| Pair.Key.Equals(TEXT("status"), ESearchCase::CaseSensitive);
			if (!bTransportField)
			{
				Content->SetField(Pair.Key, Pair.Value);
			}
		}
		return TryDigestJson(Content, OutDigest);
	}

	// Import is deliberately strict: it applies authored user-parameter defaults
	// to an existing System only when the emitter/renderer/stack structure in the
	// supplied spec still matches the live asset.  This prevents a stale export
	// from deleting or recreating graph objects behind the user's back while still
	// providing a useful export -> edit -> import -> export round-trip.
	bool SpecImportReadNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, float& Out)
	{
		double Number = 0.0;
		if (!Object.IsValid() || !Object->TryGetNumberField(Name, Number) || !FMath::IsFinite(Number)) return false;
		Out = static_cast<float>(Number);
		return FMath::IsFinite(Out);
	}

	bool SpecValidateEnvelope(const TSharedPtr<FJsonObject>& Spec, FString& OutError)
	{
		OutError.Reset();
		if (!Spec.IsValid())
		{
			OutError = TEXT("spec must be an object.");
			return false;
		}
		FString Schema;
		double Version = 0.0;
		if (!Spec->TryGetStringField(TEXT("schema"), Schema)
			|| Schema != TEXT("ue.niagara.system-spec.v1")
			|| !Spec->TryGetNumberField(TEXT("specVersion"), Version)
			|| !FMath::IsFinite(Version)
			|| FMath::TruncToInt(Version) != Version
			|| static_cast<int32>(Version) != 1)
		{
			OutError = TEXT("spec must be a ue.niagara.system-spec.v1 object with integer specVersion 1.");
			return false;
		}
		for (const TCHAR* TruncationField : {
			TEXT("userParametersTruncated"),
			TEXT("emittersTruncated"),
			TEXT("stacksTruncated"),
			TEXT("modulesTruncated"),
			TEXT("inputsTruncated")})
		{
			bool bTruncated = false;
			if (Spec->TryGetBoolField(TruncationField, bTruncated) && bTruncated)
			{
				OutError = FString::Printf(
					TEXT("spec field '%s' is truncated; export an untruncated snapshot before importing."),
					TruncationField);
				return false;
			}
		}
		return true;
	}

	FString SpecImportShortName(FString Name)
	{
		Name.TrimStartAndEndInline();
		if (Name.StartsWith(TEXT("User."), ESearchCase::IgnoreCase)) Name.RightChopInline(5);
		return Name;
	}

	bool SpecImportSetDefault(
		FNiagaraUserRedirectionParameterStore& Store,
		const FNiagaraVariable& Variable,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return false;
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (Type == FNiagaraTypeDefinition::GetFloatDef() && Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			return FMath::IsFinite(Number) && Store.SetParameterValue<float>(static_cast<float>(Number), Variable, false);
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef() && Value->Type == EJson::Number)
		{
			const double Number = Value->AsNumber();
			if (!FMath::IsFinite(Number) || Number != FMath::FloorToDouble(Number)
				|| Number < MIN_int32 || Number > MAX_int32)
			{
				return false;
			}
			return Store.SetParameterValue<int32>(static_cast<int32>(Number), Variable, false);
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef() && Value->Type == EJson::Boolean)
		{
			FNiagaraBool Bool;
			Bool.SetValue(Value->AsBool());
			return Store.SetParameterValue<FNiagaraBool>(Bool, Variable, false);
		}
		if (Value->Type != EJson::Object) return false;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid()) return false;
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			FVector2f Result;
			return SpecImportReadNumber(Object, TEXT("x"), Result.X)
				&& SpecImportReadNumber(Object, TEXT("y"), Result.Y)
				&& Store.SetParameterValue<FVector2f>(Result, Variable, false);
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			FVector3f Result;
			return SpecImportReadNumber(Object, TEXT("x"), Result.X)
				&& SpecImportReadNumber(Object, TEXT("y"), Result.Y)
				&& SpecImportReadNumber(Object, TEXT("z"), Result.Z)
				&& Store.SetParameterValue<FVector3f>(Result, Variable, false);
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			FVector4f Result;
			return SpecImportReadNumber(Object, TEXT("x"), Result.X)
				&& SpecImportReadNumber(Object, TEXT("y"), Result.Y)
				&& SpecImportReadNumber(Object, TEXT("z"), Result.Z)
				&& SpecImportReadNumber(Object, TEXT("w"), Result.W)
				&& Store.SetParameterValue<FVector4f>(Result, Variable, false);
		}
		if (Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			FQuat4f Result;
			return SpecImportReadNumber(Object, TEXT("x"), Result.X)
				&& SpecImportReadNumber(Object, TEXT("y"), Result.Y)
				&& SpecImportReadNumber(Object, TEXT("z"), Result.Z)
				&& SpecImportReadNumber(Object, TEXT("w"), Result.W)
				&& Store.SetParameterValue<FQuat4f>(Result, Variable, false);
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			FLinearColor Result = FLinearColor::White;
			return SpecImportReadNumber(Object, TEXT("r"), Result.R)
				&& SpecImportReadNumber(Object, TEXT("g"), Result.G)
				&& SpecImportReadNumber(Object, TEXT("b"), Result.B)
				&& (!Object->HasField(TEXT("a")) || SpecImportReadNumber(Object, TEXT("a"), Result.A))
				&& Store.SetParameterValue<FLinearColor>(Result, Variable, false);
		}
		return false;
	}

	bool SpecImportDefaultShapeValid(
		const FNiagaraTypeDefinition& Type,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) return false;
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			if (Value->Type != EJson::Number) return false;
			double Number = Value->AsNumber();
			float Converted = static_cast<float>(Number);
			return FMath::IsFinite(Number) && FMath::IsFinite(Converted);
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			if (Value->Type != EJson::Number) return false;
			const double Number = Value->AsNumber();
			return FMath::IsFinite(Number) && Number == FMath::FloorToDouble(Number)
				&& Number >= MIN_int32 && Number <= MAX_int32;
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef()) return Value->Type == EJson::Boolean;
		if (Value->Type != EJson::Object || !Value->AsObject().IsValid()) return false;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		auto HasFinite = [&Object](const TCHAR* Name)
		{
			float Value = 0.0f;
			return SpecImportReadNumber(Object, Name, Value);
		};
		if (Type == FNiagaraTypeDefinition::GetVec2Def()) return HasFinite(TEXT("x")) && HasFinite(TEXT("y"));
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
			return HasFinite(TEXT("x")) && HasFinite(TEXT("y")) && HasFinite(TEXT("z"));
		if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetQuatDef())
			return HasFinite(TEXT("x")) && HasFinite(TEXT("y")) && HasFinite(TEXT("z")) && HasFinite(TEXT("w"));
		if (Type == FNiagaraTypeDefinition::GetColorDef())
			return HasFinite(TEXT("r")) && HasFinite(TEXT("g")) && HasFinite(TEXT("b"))
				&& HasFinite(TEXT("a"));
		return false;
	}

	TSharedPtr<FJsonValue> SpecImportCloneValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		switch (Value->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> Source = Value->AsObject();
			if (!Source.IsValid()) return MakeShared<FJsonValueNull>();
			auto Copy = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
			{
				Copy->SetField(Pair.Key, SpecImportCloneValue(Pair.Value));
			}
			return MakeShared<FJsonValueObject>(Copy);
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>* Source = nullptr;
			if (!Value->TryGetArray(Source) || !Source) return MakeShared<FJsonValueNull>();
			TArray<TSharedPtr<FJsonValue>> Copy;
			Copy.Reserve(Source->Num());
			for (const TSharedPtr<FJsonValue>& Item : *Source)
			{
				Copy.Add(SpecImportCloneValue(Item));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Copy));
		}
		case EJson::String:
			return MakeShared<FJsonValueString>(Value->AsString());
		case EJson::Number:
			return MakeShared<FJsonValueNumber>(Value->AsNumber());
		case EJson::Boolean:
			return MakeShared<FJsonValueBoolean>(Value->AsBool());
		case EJson::Null:
		default:
			return MakeShared<FJsonValueNull>();
		}
	}

	TSharedPtr<FJsonObject> SpecImportCloneObject(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
			return nullptr;
		const TSharedPtr<FJsonValue> Clone = SpecImportCloneValue(MakeShared<FJsonValueObject>(Object));
		return Clone.IsValid() && Clone->Type == EJson::Object ? Clone->AsObject() : nullptr;
	}

	bool SpecImportStructureDigest(const TSharedPtr<FJsonObject>& Spec, FString& OutDigest)
	{
		if (!Spec.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
		if (!Spec->TryGetArrayField(TEXT("emitters"), Emitters) || !Emitters
			|| !Spec->TryGetArrayField(TEXT("userParameters"), Parameters) || !Parameters)
		{
			return false;
		}
		auto Structure = MakeShared<FJsonObject>();
		for (const TCHAR* Field : {TEXT("schema"), TEXT("specVersion"), TEXT("emittersTruncated"),
			TEXT("stacksTruncated"), TEXT("modulesTruncated"), TEXT("inputsTruncated"), TEXT("userParametersTruncated")})
		{
			if (Spec->HasField(Field)) Structure->SetField(Field, SpecImportCloneValue(Spec->TryGetField(Field)));
		}
		// User defaults are the one mutable portion applied by import.  Exclude
		// only those values from the structure digest; every emitter, stack,
		// module input, dynamic-input tree, and renderer field remains covered.
		TArray<TSharedPtr<FJsonValue>> ParameterRows;
		ParameterRows.Reserve(Parameters->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Parameters)
		{
			const TSharedPtr<FJsonObject> Parameter = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			if (!Parameter.IsValid()) return false;
			auto Row = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Parameter->Values)
			{
				if (!Pair.Key.Equals(TEXT("default"), ESearchCase::CaseSensitive))
					Row->SetField(Pair.Key, SpecImportCloneValue(Pair.Value));
			}
			ParameterRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Structure->SetArrayField(TEXT("userParameters"), MoveTemp(ParameterRows));
		TArray<TSharedPtr<FJsonValue>> EmitterRows;
		EmitterRows.Reserve(Emitters->Num());
		// Module input authoring and renderer properties are the mutable part of a
		// system spec.  Keep the object identity/type/stack topology in the stale
		// snapshot guard, while allowing an export -> edit -> import to change
		// those authored values.  The full content digest below still covers every
		// field and is therefore the read-back proof after a write.
		auto CloneStructureEmitter = [](const TSharedPtr<FJsonValue>& Value) -> TSharedPtr<FJsonValue>
		{
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{
				return MakeShared<FJsonValueNull>();
			}
			const TSharedPtr<FJsonObject> Source = Value->AsObject();
			auto Emitter = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
			{
				if (Pair.Key.Equals(TEXT("renderers"), ESearchCase::CaseSensitive))
				{
					const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(Renderers) || !Renderers)
						return MakeShared<FJsonValueNull>();
					TArray<TSharedPtr<FJsonValue>> Rows;
					for (const TSharedPtr<FJsonValue>& RendererValue : *Renderers)
					{
						const TSharedPtr<FJsonObject> Renderer = RendererValue.IsValid()
							&& RendererValue->Type == EJson::Object ? RendererValue->AsObject() : nullptr;
						if (!Renderer.IsValid()) return MakeShared<FJsonValueNull>();
						auto Row = MakeShared<FJsonObject>();
						for (const TPair<FString, TSharedPtr<FJsonValue>>& RendererPair : Renderer->Values)
						{
							if (!RendererPair.Key.Equals(TEXT("properties"), ESearchCase::CaseSensitive))
								Row->SetField(RendererPair.Key, SpecImportCloneValue(RendererPair.Value));
						}
						Rows.Add(MakeShared<FJsonValueObject>(Row));
					}
					Emitter->SetArrayField(Pair.Key, MoveTemp(Rows));
				}
				else if (Pair.Key.Equals(TEXT("stacks"), ESearchCase::CaseSensitive))
				{
					const TArray<TSharedPtr<FJsonValue>>* Stacks = nullptr;
					if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(Stacks) || !Stacks)
						return MakeShared<FJsonValueNull>();
					TArray<TSharedPtr<FJsonValue>> StackRows;
					for (const TSharedPtr<FJsonValue>& StackValue : *Stacks)
					{
						const TSharedPtr<FJsonObject> Stack = StackValue.IsValid()
							&& StackValue->Type == EJson::Object ? StackValue->AsObject() : nullptr;
						if (!Stack.IsValid()) return MakeShared<FJsonValueNull>();
						auto StackRow = MakeShared<FJsonObject>();
						for (const TPair<FString, TSharedPtr<FJsonValue>>& StackPair : Stack->Values)
						{
							if (!StackPair.Key.Equals(TEXT("modules"), ESearchCase::CaseSensitive))
							{
								StackRow->SetField(StackPair.Key, SpecImportCloneValue(StackPair.Value));
								continue;
							}
							const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
							if (!StackPair.Value.IsValid() || !StackPair.Value->TryGetArray(Modules) || !Modules)
								return MakeShared<FJsonValueNull>();
							TArray<TSharedPtr<FJsonValue>> ModuleRows;
							for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
							{
								const TSharedPtr<FJsonObject> Module = ModuleValue.IsValid()
									&& ModuleValue->Type == EJson::Object ? ModuleValue->AsObject() : nullptr;
								if (!Module.IsValid()) return MakeShared<FJsonValueNull>();
								auto ModuleRow = MakeShared<FJsonObject>();
								for (const TPair<FString, TSharedPtr<FJsonValue>>& ModulePair : Module->Values)
								{
									if (!ModulePair.Key.Equals(TEXT("inputs"), ESearchCase::CaseSensitive))
									{
										ModuleRow->SetField(ModulePair.Key, SpecImportCloneValue(ModulePair.Value));
										continue;
									}
									const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
								if (!ModulePair.Value.IsValid() || !ModulePair.Value->TryGetArray(Inputs) || !Inputs)
										return MakeShared<FJsonValueNull>();
									TArray<TSharedPtr<FJsonValue>> InputRows;
									for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
									{
										const TSharedPtr<FJsonObject> Input = InputValue.IsValid()
											&& InputValue->Type == EJson::Object ? InputValue->AsObject() : nullptr;
										if (!Input.IsValid()) return MakeShared<FJsonValueNull>();
										auto InputRow = MakeShared<FJsonObject>();
										for (const TPair<FString, TSharedPtr<FJsonValue>>& InputPair : Input->Values)
										{
											if (InputPair.Key.Equals(TEXT("value"), ESearchCase::CaseSensitive)
												|| InputPair.Key.Equals(TEXT("binding"), ESearchCase::CaseSensitive)
												|| InputPair.Key.Equals(TEXT("dynamicInput"), ESearchCase::CaseSensitive)
												|| InputPair.Key.Equals(TEXT("dynamicInputGuid"), ESearchCase::CaseSensitive)
								|| InputPair.Key.Equals(TEXT("dynamicInputTree"), ESearchCase::CaseSensitive))
												continue;
											InputRow->SetField(InputPair.Key, SpecImportCloneValue(InputPair.Value));
										}
										InputRows.Add(MakeShared<FJsonValueObject>(InputRow));
									}
									ModuleRow->SetArrayField(ModulePair.Key, MoveTemp(InputRows));
								}
								ModuleRows.Add(MakeShared<FJsonValueObject>(ModuleRow));
							}
							StackRow->SetArrayField(StackPair.Key, MoveTemp(ModuleRows));
						}
						StackRows.Add(MakeShared<FJsonValueObject>(StackRow));
					}
					Emitter->SetArrayField(Pair.Key, MoveTemp(StackRows));
				}
				else
				{
					Emitter->SetField(Pair.Key, SpecImportCloneValue(Pair.Value));
				}
			}
			return MakeShared<FJsonValueObject>(Emitter);
		};

		for (const TSharedPtr<FJsonValue>& Value : *Emitters)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid()) return false;
			const TSharedPtr<FJsonValue> Row = CloneStructureEmitter(Value);
			if (!Row.IsValid() || Row->Type == EJson::Null) return false;
			EmitterRows.Add(Row);
		}
		Structure->SetArrayField(TEXT("emitters"), MoveTemp(EmitterRows));
		return TryDigestJson(Structure, OutDigest);
	}

	class FTool_NiagaraSystemSpecRoundTrip final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.spec.round_trip"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid() || !Params->HasField(TEXT("spec")))
				return ErrorResult(TEXT("spec is required."), TEXT("spec_required"), 422);
			UNiagaraSystem* System = nullptr;
			FString Selector;
			if (!Params->TryGetStringField(TEXT("system"), Selector) || Selector.IsEmpty())
				return ErrorResult(TEXT("system is required."), TEXT("system_required"), 422);
			const FString SystemPath = NormalizeObjectPath(Selector.TrimStartAndEnd());
			System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
			if (!System) return ErrorResult(TEXT("The Niagara System was not found."), TEXT("system_not_found"), 404);
			const TSharedPtr<FJsonObject>* SpecField = nullptr;
			if (!Params->TryGetObjectField(TEXT("spec"), SpecField) || !SpecField || !SpecField->IsValid())
				return ErrorResult(TEXT("spec must be an object."), TEXT("spec_invalid"), 422);
			const TSharedPtr<FJsonObject> Spec = *SpecField;
			FString EnvelopeError;
			if (!SpecValidateEnvelope(Spec, EnvelopeError))
				return ErrorResult(EnvelopeError, TEXT("spec_invalid"), 422);
			FString CurrentDigest;
			auto ExportParams = MakeShared<FJsonObject>();
			ExportParams->SetStringField(TEXT("system"), SystemPath);
			FMCPToolResult ExportResult = FTool_NiagaraSystemSpecExport().Execute(ExportParams);
			if (!ExportResult.bSuccess || !ExportResult.Data || !SpecContentDigest(Spec, CurrentDigest))
				return ExportResult.bSuccess ? ErrorResult(TEXT("Could not digest the supplied spec."), TEXT("digest_unavailable"), 500) : ExportResult;
			FString CurrentStructure, SuppliedStructure;
			if (!SpecImportStructureDigest(Spec, SuppliedStructure) || !SpecImportStructureDigest(ExportResult.Data, CurrentStructure))
				return ErrorResult(TEXT("spec structure is incomplete."), TEXT("spec_invalid"), 422);
			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-spec-round-trip.v1"));
			Json->SetStringField(TEXT("system"), System->GetPathName());
			Json->SetStringField(TEXT("suppliedSpecDigest"), CurrentDigest);
			Json->SetStringField(TEXT("currentSpecDigest"), ExportResult.Data->GetStringField(TEXT("specDigest")));
			const bool bStructureMatch = SuppliedStructure == CurrentStructure;
			const bool bDigestMatch = CurrentDigest == ExportResult.Data->GetStringField(TEXT("specDigest"));
			const bool bRoundTripVerified = bStructureMatch && bDigestMatch;
			Json->SetBoolField(TEXT("structureMatch"), bStructureMatch);
			Json->SetBoolField(TEXT("roundTripVerified"), bRoundTripVerified);
			Json->SetBoolField(TEXT("readbackVerified"), bRoundTripVerified);
			Json->SetBoolField(TEXT("saved"), false);
			return FMCPToolResult::Ok(Json);
		}
	};

	struct FSpecModuleImportEdit
	{
		FModuleInputResolve Resolve;
		TSharedPtr<FJsonObject> Before;
		TSharedPtr<FJsonObject> DynamicInputTree;
		bool bHadOverrideNodeBefore = false;
		FString ValueString;
		TSharedPtr<FJsonValue> DesiredValue;
		FString BindingName;
		TWeakObjectPtr<UNiagaraScript> DynamicInputScript;
		FString Mode;
		bool bAttempted = false;
	};

	struct FSpecRendererImportEdit
	{
		TWeakObjectPtr<UNiagaraRendererProperties> Renderer;
		FString PropertyName;
		FString NewValue;
		FString Before;
		bool bAttempted = false;
	};

	bool SpecImportClearInputOverride(
		UNiagaraGraph* Graph,
		UNiagaraNodeFunctionCall* Node,
		const FNiagaraParameterHandle& Aliased)
	{
		if (!Graph || !Node)
			return false;
		UEdGraphPin* Pin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
		if (!Pin)
			return true;
		if (Pin->LinkedTo.Num() > 0)
		{
			// A valid authored override normally has one link, but malformed or
			// externally edited graphs can leave more than one.  Clear every link
			// deterministically and remove every mounted Dynamic Input node so the
			// resulting graph cannot retain an unreachable child or stale binding.
			TArray<UNiagaraNodeFunctionCall*> DynamicNodes;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UNiagaraNodeFunctionCall* DynamicNode = Cast<UNiagaraNodeFunctionCall>(
					LinkedPin ? LinkedPin->GetOwningNodeUnchecked() : nullptr);
				if (DynamicNode && DynamicNode->FunctionScript
					&& DynamicNode->FunctionScript->GetUsage() == ENiagaraScriptUsage::DynamicInput
					&& !DynamicNodes.Contains(DynamicNode))
				{
					DynamicNodes.Add(DynamicNode);
				}
			}
			for (UNiagaraNodeFunctionCall* DynamicNode : DynamicNodes)
			{
				for (UEdGraphPin* NodePin : DynamicNode->Pins)
				{
					if (!NodePin || NodePin->Direction != EGPD_Output)
						continue;
					for (UEdGraphPin* Consumer : NodePin->LinkedTo)
					{
						if (Consumer && Consumer != Pin)
						{
							// Do not destroy a Dynamic Input node shared with another
							// consumer while clearing one module input override.
							return false;
						}
					}
				}
			}
			Pin->BreakAllPinLinks();
			for (UNiagaraNodeFunctionCall* DynamicNode : DynamicNodes)
			{
				if (!DynamicNode)
					continue;
				DynamicNode->Modify();
				DynamicNode->BreakAllNodeLinks();
				Graph->RemoveNode(DynamicNode);
			}
		}
		Pin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
		if (Pin)
			Pin->DefaultValue.Reset();
		return true;
	}

	// System-spec exports use Niagara pin text for module input values.  The
	// interactive value capability uses typed JSON instead, so accept both
	// representations while rejecting empty/control-bearing text before any
	// graph mutation.  The Niagara pin schema remains the final parser at apply.
	bool SpecImportValueToPinString(
		const FNiagaraTypeDefinition& Type,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutString)
	{
		OutString.Reset();
		if (!Value.IsValid())
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			OutString = Value->AsString();
			return !OutString.IsEmpty()
				&& !OutString.Contains(TEXT("\r"))
				&& !OutString.Contains(TEXT("\n"));
		}
		return ValueToPinString(Type, Value, OutString);
	}

	bool SpecImportApplyDynamicInputTree(
		UNiagaraSystem* System,
		UNiagaraNodeFunctionCall* Node,
		const TSharedPtr<FJsonObject>& Tree,
		int32 Depth,
		int32& NodeBudget);

	bool SpecImportSetInputState(
		UNiagaraSystem* System,
		UNiagaraGraph* Graph,
		UNiagaraNodeFunctionCall* Node,
		const FNiagaraVariable& Input,
		const TSharedPtr<FJsonObject>& State,
		const bool bHadOverrideNodeBefore,
		int32& DynamicInputNodeBudget)
	{
		if (!System || !Graph || !Node || !State.IsValid())
			return false;
		const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Input.GetName());
		FString DynamicInputPath;
		FString Binding;
		const TSharedPtr<FJsonValue> Value = State->TryGetField(TEXT("value"));
		const bool bHasValue = State->HasField(TEXT("value")) && Value.IsValid();
		const bool bHasBinding = State->TryGetStringField(TEXT("binding"), Binding);
		const bool bHasDynamic = State->TryGetStringField(TEXT("dynamicInput"), DynamicInputPath);
		const int32 SourceCount = (bHasValue ? 1 : 0) + (bHasBinding ? 1 : 0) + (bHasDynamic ? 1 : 0);
		if (SourceCount > 1)
			return false;
		if (bHasDynamic)
		{
			UNiagaraScript* Script = LoadObject<UNiagaraScript>(nullptr, *NormalizeObjectPath(DynamicInputPath));
			if (!Script || Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
				return false;
			const TSharedPtr<FJsonObject>* Tree = nullptr;
			State->TryGetObjectField(TEXT("dynamicInputTree"), Tree);
			FString RequestedGuid;
			State->TryGetStringField(TEXT("dynamicInputGuid"), RequestedGuid);
			if (Tree && Tree->IsValid() && RequestedGuid.IsEmpty())
				(*Tree)->TryGetStringField(TEXT("guid"), RequestedGuid);
			FGuid RequestedNodeGuid;
			const bool bHasRequestedGuid = !RequestedGuid.IsEmpty();
			if (bHasRequestedGuid
				&& (!FGuid::Parse(RequestedGuid, RequestedNodeGuid) || !RequestedNodeGuid.IsValid()))
			{
				return false;
			}
			if (Tree && Tree->IsValid())
			{
				FString TreeScriptPath;
				if ((*Tree)->TryGetStringField(TEXT("scriptPath"), TreeScriptPath)
					&& !TreeScriptPath.IsEmpty()
					&& !ScriptObjectPathsEqual(TreeScriptPath, Script->GetPathName()))
				{
					return false;
				}
				FString TreeGuid;
				if ((*Tree)->TryGetStringField(TEXT("guid"), TreeGuid) && !TreeGuid.IsEmpty())
				{
					FGuid ParsedTreeGuid;
					if (!FGuid::Parse(TreeGuid, ParsedTreeGuid) || !ParsedTreeGuid.IsValid()
						|| (bHasRequestedGuid && ParsedTreeGuid != RequestedNodeGuid))
					{
						return false;
					}
				}
			}
			UEdGraphPin* ExistingPin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
			UNiagaraNodeFunctionCall* DynamicNode = ExistingPin && ExistingPin->LinkedTo.Num() > 0
				? Cast<UNiagaraNodeFunctionCall>(ExistingPin->LinkedTo[0]->GetOwningNodeUnchecked())
				: nullptr;
			const bool bSameScript = DynamicNode && DynamicNode->FunctionScript
				&& ScriptObjectPathsEqual(NodeScriptPath(DynamicNode), DynamicInputPath);
			const bool bSameGuid = !bHasRequestedGuid
				|| (DynamicNode && DynamicNode->NodeGuid == RequestedNodeGuid);
			if (bHasRequestedGuid)
			{
				for (UEdGraphNode* Candidate : Graph->Nodes)
				{
					if (Candidate && Candidate != DynamicNode && Candidate->NodeGuid == RequestedNodeGuid)
					{
						return false;
					}
				}
			}
			if (!bSameScript || !bSameGuid)
			{
				if (!SpecImportClearInputOverride(Graph, Node, Aliased))
					return false;
				ExistingPin = nullptr;
				DynamicNode = nullptr;
			}
			if (!DynamicNode)
			{
				UEdGraphPin* Pin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
					*Node, Aliased, Input.GetType(), FGuid(), FGuid());
				Pin->GetOwningNodeUnchecked()->Modify();
				Pin->DefaultValue.Reset();
				FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(*Pin, Script, DynamicNode);
			}
			if (!DynamicNode)
				return false;
			if (!bSameScript || !bSameGuid)
			{
				if (bHasRequestedGuid)
				{
					DynamicNode->NodeGuid = RequestedNodeGuid;
				}
			}
			if (!Tree || !Tree->IsValid())
				return true;
			return SpecImportApplyDynamicInputTree(
				System, DynamicNode, *Tree, 0, DynamicInputNodeBudget);
		}
		if (!SpecImportClearInputOverride(Graph, Node, Aliased))
			return false;
		if (bHasBinding)
		{
			TSet<FNiagaraVariableBase> Known;
			TArray<FNiagaraVariableBase> Ordered;
			BuildKnownBindingParameters(System, Known, Ordered);
			FString LinkedName, ErrorCode, Error;
			if (!ResolveLinkedParameter(Ordered, Binding, LinkedName, ErrorCode, Error))
				return false;
			UEdGraphPin* Pin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Input.GetType(), FGuid(), FGuid());
			const FNiagaraVariableBase LinkedParameter(Input.GetType(), FName(*LinkedName));
			FNiagaraStackGraphUtilities::SetLinkedParameterValueForFunctionInput(
				*Pin, LinkedParameter, Known, ENiagaraDefaultMode::FailIfPreviouslyNotSet, FGuid());
			return true;
		}
		if (bHasValue)
		{
			FString ValueString;
			if (!SpecImportValueToPinString(Input.GetType(), Value, ValueString))
				return false;
			UEdGraphPin* Pin = &FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
				*Node, Aliased, Input.GetType(), FGuid(), FGuid());
			Pin->GetOwningNodeUnchecked()->Modify();
			Pin->DefaultValue = ValueString;
			return true;
		}
		UEdGraphPin* Pin = FindExistingOverridePin(Node, Aliased.GetParameterHandleString());
		if (Pin)
		{
			UEdGraphNode* OverrideNode = Pin->GetOwningNodeUnchecked();
			OverrideNode->Modify();
			Pin->BreakAllPinLinks();
			OverrideNode->RemovePin(Pin);
		}
		if (!bHadOverrideNodeBefore)
		{
			if (UNiagaraNode* OverrideNode = FindStackOverrideNode(Node))
				SpliceOutOverrideNode(Graph, OverrideNode, Node);
		}
		return FindExistingOverridePin(Node, Aliased.GetParameterHandleString()) == nullptr;
	}

	bool SpecImportApplyDynamicInputTree(
		UNiagaraSystem* System,
		UNiagaraNodeFunctionCall* Node,
		const TSharedPtr<FJsonObject>& Tree,
		int32 Depth,
		int32& NodeBudget)
	{
		if (!System || !Node || !Tree.IsValid() || Depth > 16 || NodeBudget-- <= 0)
			return false;
		bool bTruncated = false;
		if (Tree->TryGetBoolField(TEXT("truncated"), bTruncated) && bTruncated)
			return false;
		FString TreeScriptPath;
		if (Tree->TryGetStringField(TEXT("scriptPath"), TreeScriptPath)
			&& !TreeScriptPath.IsEmpty()
			&& !ScriptObjectPathsEqual(TreeScriptPath, NodeScriptPath(Node)))
			return false;
		FString TreeGuid;
		if (Tree->TryGetStringField(TEXT("guid"), TreeGuid)
			&& !TreeGuid.IsEmpty())
		{
			FGuid ParsedTreeGuid;
			if (!FGuid::Parse(TreeGuid, ParsedTreeGuid) || !ParsedTreeGuid.IsValid()
				|| ParsedTreeGuid != Node->NodeGuid)
				return false;
		}
		FString TreeName;
		if (Tree->TryGetStringField(TEXT("name"), TreeName)
			&& !TreeName.IsEmpty()
			&& TreeName != Node->GetFunctionName())
			return false;
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (!Tree->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs)
		{
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			return !Tree->TryGetArrayField(TEXT("children"), Children) || !Children || Children->Num() == 0;
		}
		const ENiagaraScriptUsage Usage = Node->FunctionScript
			? Node->FunctionScript->GetUsage()
			: ENiagaraScriptUsage::DynamicInput;
		const FCompileConstantResolver ConstantResolver(System, Usage);
		TArray<FNiagaraVariable> InputVariables;
		TSet<FNiagaraVariable> HiddenVariables;
		FNiagaraStackGraphUtilities::GetStackFunctionInputs(
			*Node,
			InputVariables,
			HiddenVariables,
			ConstantResolver,
			FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
		TSet<FString> SeenInputs;
		for (const TSharedPtr<FJsonValue>& Value : *Inputs)
		{
			const TSharedPtr<FJsonObject> InputSpec = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject()
				: nullptr;
			if (!InputSpec.IsValid())
				return false;
			FString Name;
			if (!InputSpec->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
				return false;
			Name.RemoveFromStart(PARAM_MAP_MODULE_STR);
			if (SeenInputs.Contains(Name))
				return false;
			SeenInputs.Add(Name);
			FString TypeName;
			InputSpec->TryGetStringField(TEXT("type"), TypeName);
			const FNiagaraVariable* Match = nullptr;
			for (const FNiagaraVariable& Candidate : InputVariables)
			{
				FString CandidateName = Candidate.GetName().ToString();
				CandidateName.RemoveFromStart(PARAM_MAP_MODULE_STR);
				if (CandidateName.Equals(Name, ESearchCase::IgnoreCase)
					&& (TypeName.IsEmpty() || Candidate.GetType().GetName() == TypeName))
				{
					Match = &Candidate;
					break;
				}
			}
			if (!Match)
				return false;
			const FNiagaraParameterHandle Aliased = AliasedInputHandle(Node, Match->GetName());
			const bool bHadOverride = FindExistingOverridePin(Node, Aliased.GetParameterHandleString()) != nullptr;
			if (!SpecImportSetInputState(
				System,
				Cast<UNiagaraGraph>(Node->GetGraph()),
				Node,
				*Match,
				InputSpec,
				bHadOverride,
				NodeBudget))
				return false;
			const TSharedPtr<FJsonObject>* ChildTree = nullptr;
			if (InputSpec->TryGetObjectField(TEXT("dynamicInputTree"), ChildTree)
				&& ChildTree && ChildTree->IsValid())
			{
				// SpecImportSetInputState applies the child tree while mounting the
				// dynamic input.  Do not recurse a second time here: doing so clears
				// and reapplies nested overrides and can change graph identity.
			}
		}
		for (const FNiagaraVariable& Candidate : InputVariables)
		{
			FString CandidateName = Candidate.GetName().ToString();
			CandidateName.RemoveFromStart(PARAM_MAP_MODULE_STR);
			if (!SeenInputs.Contains(CandidateName))
				return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
		const bool bHasChildren = Tree->TryGetArrayField(TEXT("children"), Children) && Children;
		bool bExpectedChildren = false;
		for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
		{
			const TSharedPtr<FJsonObject> InputSpec = InputValue.IsValid() && InputValue->Type == EJson::Object
				? InputValue->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* ChildTree = nullptr;
			if (InputSpec.IsValid()
				&& InputSpec->TryGetObjectField(TEXT("dynamicInputTree"), ChildTree)
				&& ChildTree && ChildTree->IsValid())
			{
				bExpectedChildren = true;
				break;
			}
		}
		if (bExpectedChildren && !bHasChildren)
			return false;
		if (bHasChildren)
		{
			TSet<FString> ExpectedChildren;
			TMap<FString, TSharedPtr<FJsonObject>> ExpectedTrees;
			for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
			{
				const TSharedPtr<FJsonObject> InputSpec = InputValue.IsValid() && InputValue->Type == EJson::Object
					? InputValue->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* ChildTree = nullptr;
				if (InputSpec.IsValid() && InputSpec->TryGetObjectField(TEXT("dynamicInputTree"), ChildTree)
					&& ChildTree && ChildTree->IsValid())
				{
					FString InputName;
					InputSpec->TryGetStringField(TEXT("name"), InputName);
					// Spec exports use the short input name while the graph binding
					// helper reports the aliased Module.<name> form.  Normalize both
					// representations before comparing the child set.
					InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
					ExpectedChildren.Add(InputName);
					ExpectedTrees.Add(InputName, *ChildTree);
				}
			}
			TSet<FString> SeenChildren;
			for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
			{
				const TSharedPtr<FJsonObject> Child = ChildValue.IsValid() && ChildValue->Type == EJson::Object
					? ChildValue->AsObject() : nullptr;
				FString InputName;
				if (!Child.IsValid() || !Child->TryGetStringField(TEXT("input"), InputName))
					return false;
				InputName.RemoveFromStart(PARAM_MAP_MODULE_STR);
				if (!ExpectedChildren.Contains(InputName) || SeenChildren.Contains(InputName))
					return false;
				const TSharedPtr<FJsonObject>* ExpectedTree = ExpectedTrees.Find(InputName);
				if (!ExpectedTree || !ExpectedTree->IsValid())
					return false;
				FString ExpectedGuid;
				FString ChildGuid;
				(*ExpectedTree)->TryGetStringField(TEXT("guid"), ExpectedGuid);
				Child->TryGetStringField(TEXT("guid"), ChildGuid);
				if (!ExpectedGuid.IsEmpty())
				{
					FGuid ExpectedParsed;
					FGuid ChildParsed;
					if (!FGuid::Parse(ExpectedGuid, ExpectedParsed)
						|| !FGuid::Parse(ChildGuid, ChildParsed)
						|| ExpectedParsed != ChildParsed)
					{
						return false;
					}
				}
				FString ExpectedScriptPath;
				FString ChildScriptPath;
				(*ExpectedTree)->TryGetStringField(TEXT("scriptPath"), ExpectedScriptPath);
				Child->TryGetStringField(TEXT("scriptPath"), ChildScriptPath);
				if (!ExpectedScriptPath.IsEmpty()
					&& !ScriptObjectPathsEqual(ExpectedScriptPath, ChildScriptPath))
				{
					return false;
				}
				SeenChildren.Add(InputName);
			}
			if (SeenChildren.Num() != ExpectedChildren.Num())
				return false;
		}
		return true;
	}

	class FTool_NiagaraSystemSpecImport final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.spec.import"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid() || !Params->HasField(TEXT("spec")))
				return ErrorResult(TEXT("spec is required."), TEXT("spec_required"), 422);
			FString Selector;
			if (!Params->TryGetStringField(TEXT("system"), Selector) || Selector.IsEmpty())
				return ErrorResult(TEXT("system is required."), TEXT("system_required"), 422);
			const FString SystemPath = NormalizeObjectPath(Selector.TrimStartAndEnd());
			UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
			if (!System) return ErrorResult(TEXT("The Niagara System was not found."), TEXT("system_not_found"), 404);
			if (!System->GetOutermost()->GetName().StartsWith(TEXT("/Game/")) || System->HasAnyFlags(RF_Transient))
				return ErrorResult(TEXT("Spec import requires a non-transient /Game/ Niagara System."), TEXT("system_read_only"), 409);
			const TSharedPtr<FJsonObject>* SpecField = nullptr;
			if (!Params->TryGetObjectField(TEXT("spec"), SpecField) || !SpecField || !SpecField->IsValid())
				return ErrorResult(TEXT("spec must be an object."), TEXT("spec_invalid"), 422);
			const TSharedPtr<FJsonObject> Spec = *SpecField;
			FString EnvelopeError;
			if (!SpecValidateEnvelope(Spec, EnvelopeError))
				return ErrorResult(EnvelopeError, TEXT("spec_invalid"), 422);
			FString RequestId;
			if (!Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
				return ErrorResult(TEXT("requestId is required."), TEXT("request_id_required"), 422);
			bool ConfirmWrite = false;
			if (!Params->TryGetBoolField(TEXT("confirmWrite"), ConfirmWrite) || !ConfirmWrite)
				return ErrorResult(TEXT("confirmWrite must be true."), TEXT("confirm_write_required"), 409);
			auto ExportParams = MakeShared<FJsonObject>();
			ExportParams->SetStringField(TEXT("system"), SystemPath);
			FMCPToolResult ExportResult = FTool_NiagaraSystemSpecExport().Execute(ExportParams);
			if (!ExportResult.bSuccess || !ExportResult.Data) return ExportResult;
			FString SuppliedStructure, CurrentStructure;
			if (!SpecImportStructureDigest(Spec, SuppliedStructure) || !SpecImportStructureDigest(ExportResult.Data, CurrentStructure))
				return ErrorResult(TEXT("spec structure is incomplete."), TEXT("spec_invalid"), 422);
			if (SuppliedStructure != CurrentStructure)
				return ErrorResult(TEXT("The live System structure changed since the spec was exported; re-export before importing."), TEXT("spec_structure_mismatch"), 409);
			const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
			if (!Spec->TryGetArrayField(TEXT("userParameters"), Parameters) || !Parameters)
				return ErrorResult(TEXT("spec.userParameters must be an array."), TEXT("spec_invalid"), 422);
			struct FImportParameterSnapshot
			{
				FNiagaraVariable Variable;
				TSharedPtr<FJsonValue> Before;
				bool bRestorable = false;
			};
			FNiagaraUserRedirectionParameterStore& Store = System->GetExposedParameters();
			TArray<TPair<const FNiagaraVariableWithOffset*, TSharedPtr<FJsonValue>>> Pending;
			TArray<FImportParameterSnapshot> Snapshots;
			TSet<FString> SeenParameters;
			for (const TSharedPtr<FJsonValue>& Value : *Parameters)
			{
				const TSharedPtr<FJsonObject> Parameter = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				if (!Parameter.IsValid())
					return ErrorResult(TEXT("Each user parameter must be an object."), TEXT("spec_invalid"), 422);
				if (!Parameter->HasField(TEXT("default"))) continue;
				FString Name, TypeName;
				if (!Parameter->TryGetStringField(TEXT("name"), Name) || !Parameter->TryGetStringField(TEXT("type"), TypeName))
					return ErrorResult(TEXT("Each user parameter requires name and type."), TEXT("spec_invalid"), 422);
				const FString CanonicalName = SpecImportShortName(Name).ToLower();
				if (SeenParameters.Contains(CanonicalName))
					return ErrorResult(FString::Printf(TEXT("User parameter '%s' appears more than once."), *Name), TEXT("spec_invalid"), 422);
				SeenParameters.Add(CanonicalName);
				const FNiagaraVariableWithOffset* Match = nullptr;
				for (const FNiagaraVariableWithOffset& Candidate : Store.ReadParameterVariables())
				{
					if (SpecImportShortName(Candidate.GetName().ToString()).Equals(SpecImportShortName(Name), ESearchCase::IgnoreCase)
						&& Candidate.GetType().GetName() == TypeName)
					{
						Match = &Candidate;
						break;
					}
				}
				if (!Match) return ErrorResult(FString::Printf(TEXT("User parameter '%s' was not found or changed type."), *Name), TEXT("parameter_not_found"), 409);
				const TSharedPtr<FJsonValue> DefaultValue = Parameter->TryGetField(TEXT("default"));
				if (!SpecImportDefaultShapeValid(Match->GetType(), DefaultValue))
					return ErrorResult(FString::Printf(TEXT("User parameter '%s' has an unsupported or invalid default."), *Name), TEXT("parameter_default_invalid"), 422);
				bool bBeforeSupported = false;
				const TSharedPtr<FJsonValue> Before = SpecExportUserDefault(*Match, Store, bBeforeSupported);
				Snapshots.Add({FNiagaraVariable(*Match), Before, bBeforeSupported});
				Pending.Add(TPair<const FNiagaraVariableWithOffset*, TSharedPtr<FJsonValue>>(Match, DefaultValue));
			}

			TArray<FSpecModuleImportEdit> ModuleEdits;
			TArray<FSpecRendererImportEdit> RendererEdits;
			const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
			if (!Spec->TryGetArrayField(TEXT("emitters"), Emitters) || !Emitters)
				return ErrorResult(TEXT("spec.emitters must be an array."), TEXT("spec_invalid"), 422);
			for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
			{
				const TSharedPtr<FJsonObject> EmitterSpec = EmitterValue.IsValid()
					&& EmitterValue->Type == EJson::Object ? EmitterValue->AsObject() : nullptr;
				if (!EmitterSpec.IsValid())
					return ErrorResult(TEXT("Each emitter must be an object."), TEXT("spec_invalid"), 422);
				FString EmitterName;
			if (!EmitterSpec->TryGetStringField(TEXT("name"), EmitterName) || EmitterName.IsEmpty())
					return ErrorResult(TEXT("Each emitter requires a name."), TEXT("spec_invalid"), 422);
				FNiagaraEmitterHandle* Handle = nullptr;
				for (const FNiagaraEmitterHandle& Candidate : System->GetEmitterHandles())
				{
					if (Candidate.GetName().ToString().Equals(EmitterName, ESearchCase::CaseSensitive))
					{
						Handle = const_cast<FNiagaraEmitterHandle*>(&Candidate);
						break;
					}
				}
				if (!Handle || !Handle->GetEmitterData())
					return ErrorResult(FString::Printf(TEXT("Emitter '%s' was not found."), *EmitterName), TEXT("emitter_not_found"), 409);

				const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
				if (EmitterSpec->TryGetArrayField(TEXT("renderers"), Renderers) && Renderers)
				{
					const TArray<UNiagaraRendererProperties*>& LiveRenderers = Handle->GetEmitterData()->GetRenderers();
					if (Renderers->Num() != LiveRenderers.Num())
						return ErrorResult(FString::Printf(TEXT("Emitter '%s' renderer count changed."), *EmitterName), TEXT("spec_structure_mismatch"), 409);
					for (int32 RendererIndex = 0; RendererIndex < Renderers->Num(); ++RendererIndex)
					{
						const TSharedPtr<FJsonObject> RendererSpec = (*Renderers)[RendererIndex].IsValid()
							&& (*Renderers)[RendererIndex]->Type == EJson::Object ? (*Renderers)[RendererIndex]->AsObject() : nullptr;
						UNiagaraRendererProperties* Renderer = LiveRenderers.IsValidIndex(RendererIndex) ? LiveRenderers[RendererIndex] : nullptr;
						if (!RendererSpec.IsValid() || !Renderer)
							return ErrorResult(TEXT("Each renderer entry must match a live renderer."), TEXT("renderer_not_found"), 409);
						FString RendererClass;
						if (!RendererSpec->TryGetStringField(TEXT("rendererClass"), RendererClass)
							|| RendererClass != Renderer->GetClass()->GetPathName())
							return ErrorResult(TEXT("Renderer class changed since export."), TEXT("spec_structure_mismatch"), 409);
						FString RendererPath;
						if (RendererSpec->TryGetStringField(TEXT("rendererPath"), RendererPath)
							&& !RendererPath.IsEmpty()
							&& RendererPath != Renderer->GetPathName())
							return ErrorResult(TEXT("Renderer identity changed since export."), TEXT("spec_structure_mismatch"), 409);
						const TSharedPtr<FJsonObject>* Properties = nullptr;
						if (!RendererSpec->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->IsValid())
							continue;
						for (const TPair<FString, TSharedPtr<FJsonValue>>& PropertyPair : (*Properties)->Values)
						{
							FString ValueText;
							if (!PropertyPair.Value.IsValid() || !PropertyPair.Value->TryGetString(ValueText))
								return ErrorResult(TEXT("Renderer properties must contain exported text values."), TEXT("renderer_property_invalid"), 422);
							FProperty* Property = FindFProperty<FProperty>(Renderer->GetClass(), FName(*PropertyPair.Key));
							if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)
								|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient))
								return ErrorResult(FString::Printf(TEXT("Renderer property '%s' is not editable."), *PropertyPair.Key), TEXT("renderer_property_unsupported"), 422);
							ValueText = SpecCanonicalRendererPropertyText(Property->GetFName(), MoveTemp(ValueText));
							FString Before;
							Property->ExportTextItem_Direct(Before, Property->ContainerPtrToValuePtr<void>(Renderer), nullptr, nullptr, PPF_None);
							Before = SpecCanonicalRendererPropertyText(Property->GetFName(), MoveTemp(Before));
							if (Before == ValueText)
								continue;
							RendererEdits.Add({Renderer, PropertyPair.Key, ValueText, Before});
						}
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* Stacks = nullptr;
				if (!EmitterSpec->TryGetArrayField(TEXT("stacks"), Stacks) || !Stacks)
					continue;
				for (const TSharedPtr<FJsonValue>& StackValue : *Stacks)
				{
					const TSharedPtr<FJsonObject> StackSpec = StackValue.IsValid()
						&& StackValue->Type == EJson::Object ? StackValue->AsObject() : nullptr;
					if (!StackSpec.IsValid()) return ErrorResult(TEXT("Each stack must be an object."), TEXT("spec_invalid"), 422);
					FString StackName;
					if (!StackSpec->TryGetStringField(TEXT("stackName"), StackName) || StackName.IsEmpty())
						return ErrorResult(TEXT("Each stack requires stackName."), TEXT("spec_invalid"), 422);
					UNiagaraScript* StackScript = nullptr;
					UNiagaraNodeOutput* StackOutput = nullptr;
					TArray<UNiagaraScript*> Scripts;
					Handle->GetEmitterData()->GetScripts(Scripts, false, false);
					for (UNiagaraScript* Candidate : Scripts)
					{
						if (!Candidate || UsageName(Candidate->GetUsage()) != StackName) continue;
						UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Candidate->GetLatestSource());
						UNiagaraNodeOutput* CandidateOutput = Source && Source->NodeGraph ? FindOutputForScript(Source->NodeGraph, Candidate) : nullptr;
						if (CandidateOutput) { StackScript = Candidate; StackOutput = CandidateOutput; break; }
					}
					if (!StackScript || !StackOutput)
						return ErrorResult(FString::Printf(TEXT("Stack '%s' was not found."), *StackName), TEXT("stack_not_found"), 409);
					const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
					if (!StackSpec->TryGetArrayField(TEXT("modules"), Modules) || !Modules) continue;
					for (const TSharedPtr<FJsonValue>& ModuleValue : *Modules)
					{
						const TSharedPtr<FJsonObject> ModuleSpec = ModuleValue.IsValid()
							&& ModuleValue->Type == EJson::Object ? ModuleValue->AsObject() : nullptr;
						if (!ModuleSpec.IsValid()) return ErrorResult(TEXT("Each module must be an object."), TEXT("spec_invalid"), 422);
						FString ModuleScriptPath;
						if (!ModuleSpec->TryGetStringField(TEXT("scriptPath"), ModuleScriptPath) || ModuleScriptPath.IsEmpty())
							return ErrorResult(TEXT("Each module requires scriptPath."), TEXT("spec_invalid"), 422);
						FString ModuleNodePath;
						FString ModuleNodeGuid;
						ModuleSpec->TryGetStringField(TEXT("nodePath"), ModuleNodePath);
						ModuleSpec->TryGetStringField(TEXT("nodeGuid"), ModuleNodeGuid);
						FStackModuleRequest ModuleRequest;
						ModuleRequest.SystemPath = SystemPath;
						ModuleRequest.EmitterSelector = EmitterName;
						ModuleRequest.OutputNodePath = StackOutput->GetPathName();
						ModuleRequest.ModuleSelector = !ModuleNodeGuid.IsEmpty()
							? ModuleNodeGuid
							: (!ModuleNodePath.IsEmpty() ? ModuleNodePath : ModuleScriptPath);
						FModuleInputResolve ModuleResolve;
						const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
						if (!ModuleSpec->TryGetArrayField(TEXT("inputs"), Inputs) || !Inputs) continue;
						for (const TSharedPtr<FJsonValue>& InputValue : *Inputs)
						{
							const TSharedPtr<FJsonObject> InputSpec = InputValue.IsValid()
								&& InputValue->Type == EJson::Object ? InputValue->AsObject() : nullptr;
							if (!InputSpec.IsValid()) return ErrorResult(TEXT("Each module input must be an object."), TEXT("spec_invalid"), 422);
							FString InputName, TypeName;
							if (!InputSpec->TryGetStringField(TEXT("name"), InputName) || !InputSpec->TryGetStringField(TEXT("type"), TypeName))
								return ErrorResult(TEXT("Each module input requires name and type."), TEXT("spec_invalid"), 422);
							const bool bHasValue = InputSpec->HasField(TEXT("value"));
							FString BindingName, DynamicInputPath;
							const bool bHasBinding = InputSpec->TryGetStringField(TEXT("binding"), BindingName);
							const bool bHasDynamic = InputSpec->TryGetStringField(TEXT("dynamicInput"), DynamicInputPath);
							const int32 SourceCount = (bHasValue ? 1 : 0) + (bHasBinding ? 1 : 0) + (bHasDynamic ? 1 : 0);
					if (SourceCount > 1) return ErrorResult(TEXT("A module input may specify only one of value, binding, or dynamicInput; omitting all three means clear."), TEXT("input_state_invalid"), 422);
							FModuleInputRequest InputRequest;
							InputRequest.Stack = ModuleRequest;
							InputRequest.Input = InputName;
							FString InputErrorCode;
							FString InputError;
							if (!ResolveModuleInput(InputRequest, ModuleResolve, InputErrorCode, InputError))
								return ErrorResult(InputError, InputErrorCode.IsEmpty() ? TEXT("input_not_found") : InputErrorCode, 409);
							if (!ModuleNodeGuid.IsEmpty()
								&& (!ModuleResolve.Module.Selected
									|| ModuleResolve.Module.Selected->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) != ModuleNodeGuid))
								return ErrorResult(TEXT("Module node identity changed since export."), TEXT("spec_structure_mismatch"), 409);
							if (ModuleResolve.TypeName != TypeName)
								return ErrorResult(FString::Printf(TEXT("Input '%s' changed type since export."), *InputName), TEXT("input_type_changed"), 409);
							if (ModuleResolve.Module.bBlocked)
								return ErrorResult(
									TEXT("System spec import cannot edit a shared or read-only Niagara graph."),
									TEXT("plan_blocked"), 409);
							FSpecModuleImportEdit Edit;
							Edit.Resolve = ModuleResolve;
							Edit.Before = SpecExportBuildInput(ModuleResolve.Module.Selected, FNiagaraVariable(ModuleResolve.InputType, ModuleResolve.MatchedFullName), System);
							Edit.bHadOverrideNodeBefore = FindStackOverrideNode(ModuleResolve.Module.Selected) != nullptr;
					Edit.Mode = bHasValue
						? TEXT("value")
						: (bHasBinding ? TEXT("binding") : (bHasDynamic ? TEXT("dynamicInput") : TEXT("clear")));
							Edit.BindingName = BindingName;
							if (bHasValue && !SpecImportValueToPinString(ModuleResolve.InputType, InputSpec->TryGetField(TEXT("value")), Edit.ValueString))
								return ErrorResult(FString::Printf(TEXT("Input '%s' has an invalid inline value."), *InputName), TEXT("value_invalid"), 422);
			if (bHasValue)
				Edit.DesiredValue = SpecImportCloneValue(InputSpec->TryGetField(TEXT("value")));
							if (bHasBinding)
							{
								TSet<FNiagaraVariableBase> Known;
								TArray<FNiagaraVariableBase> Ordered;
								BuildKnownBindingParameters(System, Known, Ordered);
								FString LinkedName, ErrorCode, Error;
								if (!ResolveLinkedParameter(Ordered, BindingName, LinkedName, ErrorCode, Error)) return ErrorResult(Error, ErrorCode, 404);
								Edit.BindingName = LinkedName;
							}
							if (bHasDynamic)
							{
								UNiagaraScript* DynamicScript = LoadObject<UNiagaraScript>(nullptr, *NormalizeObjectPath(DynamicInputPath));
								if (!DynamicScript || DynamicScript->GetUsage() != ENiagaraScriptUsage::DynamicInput)
									return ErrorResult(TEXT("dynamicInput is not a Dynamic Input script."), TEXT("dynamic_input_not_found"), 404);
								Edit.DynamicInputScript = DynamicScript;
				const TSharedPtr<FJsonObject>* DynamicTree = nullptr;
				if (InputSpec->TryGetObjectField(TEXT("dynamicInputTree"), DynamicTree)
					&& DynamicTree && DynamicTree->IsValid())
				{
					Edit.DynamicInputTree = SpecImportCloneObject(*DynamicTree);
				}
						}
							ModuleEdits.Add(MoveTemp(Edit));
					}
				}
			}
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Import Niagara System Spec")));
			System->Modify();
			FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(Store);
			int32 Applied = 0;
			int32 AppliedModuleEdits = 0;
			int32 AppliedRendererEdits = 0;
			int32 DynamicInputNodeBudget = SpecExportMaxDynamicInputNodes;
			bool bApplyFailed = false;
			for (const TPair<const FNiagaraVariableWithOffset*, TSharedPtr<FJsonValue>>& Item : Pending)
			{
				if (!Item.Key || !SpecImportSetDefault(Store, *Item.Key, Item.Value))
				{
					bApplyFailed = true;
					break;
				}
				++Applied;
			}
			bool bGraphChanged = false;
			for (FSpecModuleImportEdit& Edit : ModuleEdits)
			{
				Edit.bAttempted = true;
				UNiagaraSystem* EditSystem = Edit.Resolve.Module.System.Get();
				UNiagaraGraph* EditGraph = Edit.Resolve.Module.Target.Graph;
				UNiagaraNodeFunctionCall* EditNode = Edit.Resolve.Module.Selected;
				if (!EditSystem || !EditGraph || !EditNode)
				{
					bApplyFailed = true;
					break;
				}
				auto Desired = MakeShared<FJsonObject>();
				if (Edit.Mode == TEXT("value"))
					Desired->SetField(TEXT("value"), Edit.DesiredValue);
				else if (Edit.Mode == TEXT("binding"))
					Desired->SetStringField(TEXT("binding"), Edit.BindingName);
				else if (UNiagaraScript* DynamicScript = Edit.DynamicInputScript.Get())
				{
					Desired->SetStringField(TEXT("dynamicInput"), DynamicScript->GetPathName());
					if (Edit.DynamicInputTree.IsValid())
						Desired->SetObjectField(TEXT("dynamicInputTree"), Edit.DynamicInputTree);
				}
				else if (Edit.Mode == TEXT("clear"))
				{
					// An exported input with no authored source is an explicit clear
					// state.  Passing an empty state lets SpecImportSetInputState remove
					// a stale live override while preserving the snapshot topology.
				}
				else
				{
					bApplyFailed = true;
					break;
				}
				const FNiagaraVariable InputVariable(Edit.Resolve.InputType, Edit.Resolve.MatchedFullName);
				if (!SpecImportSetInputState(
					EditSystem, EditGraph, EditNode, InputVariable, Desired,
					Edit.bHadOverrideNodeBefore,
					DynamicInputNodeBudget))
				{
					bApplyFailed = true;
					break;
				}
				EditGraph->NotifyGraphChanged();
				bGraphChanged = true;
				++AppliedModuleEdits;
			}
			for (int32 EditIndex = 0; !bApplyFailed && EditIndex < RendererEdits.Num(); ++EditIndex)
			{
				FSpecRendererImportEdit& Edit = RendererEdits[EditIndex];
				Edit.bAttempted = true;
				UNiagaraRendererProperties* Renderer = Edit.Renderer.Get();
				FProperty* Property = Renderer ? FindFProperty<FProperty>(Renderer->GetClass(), FName(*Edit.PropertyName)) : nullptr;
				if (!Renderer || !Property)
				{
					bApplyFailed = true;
					break;
				}
				Renderer->Modify();
				void* Address = Property->ContainerPtrToValuePtr<void>(Renderer);
				const TCHAR* ImportEnd = Property->ImportText_Direct(*Edit.NewValue, Address, Renderer, PPF_None);
				if (!ImportEnd)
				{
					bApplyFailed = true;
					break;
				}
				while (*ImportEnd && FChar::IsWhitespace(*ImportEnd)) ++ImportEnd;
				if (*ImportEnd != TEXT('\0')) bApplyFailed = true;
				if (!bApplyFailed)
					++AppliedRendererEdits;
				bGraphChanged = true;
			}
			auto RestoreImportChanges = [&]()
			{
				bool bRestored = true;
				int32 RestoreDynamicInputNodeBudget = SpecExportMaxDynamicInputNodes;
				for (int32 Index = ModuleEdits.Num() - 1; Index >= 0; --Index)
				{
					FSpecModuleImportEdit& Edit = ModuleEdits[Index];
					if (!Edit.bAttempted)
						continue;
					UNiagaraSystem* EditSystem = Edit.Resolve.Module.System.Get();
					UNiagaraGraph* EditGraph = Edit.Resolve.Module.Target.Graph;
					UNiagaraNodeFunctionCall* EditNode = Edit.Resolve.Module.Selected;
					if (!EditSystem || !EditGraph || !EditNode
						|| !SpecImportSetInputState(
							EditSystem, EditGraph, EditNode,
							FNiagaraVariable(Edit.Resolve.InputType, Edit.Resolve.MatchedFullName),
							Edit.Before,
							Edit.bHadOverrideNodeBefore,
							RestoreDynamicInputNodeBudget))
					{
						bRestored = false;
					}
				}
				for (int32 Index = RendererEdits.Num() - 1; Index >= 0; --Index)
				{
					FSpecRendererImportEdit& Edit = RendererEdits[Index];
					if (!Edit.bAttempted)
						continue;
					UNiagaraRendererProperties* Renderer = Edit.Renderer.Get();
					FProperty* Property = Renderer ? FindFProperty<FProperty>(Renderer->GetClass(), FName(*Edit.PropertyName)) : nullptr;
					if (!Renderer || !Property)
					{
						bRestored = false;
						continue;
					}
					void* Address = Property->ContainerPtrToValuePtr<void>(Renderer);
					const TCHAR* RestoreEnd = Property->ImportText_Direct(*Edit.Before, Address, Renderer, PPF_None);
					if (!RestoreEnd)
						bRestored = false;
					else
					{
						while (*RestoreEnd && FChar::IsWhitespace(*RestoreEnd)) ++RestoreEnd;
						if (*RestoreEnd != TEXT('\0')) bRestored = false;
					}
				}
				for (const FImportParameterSnapshot& Snapshot : Snapshots)
				{
					bRestored &= Snapshot.bRestorable
						&& SpecImportSetDefault(Store, Snapshot.Variable, Snapshot.Before);
				}
				return bRestored;
			};
			if (bApplyFailed)
			{
				bool bRestored = RestoreImportChanges();
				if (bRestored)
				{
					bRestored = CompileSystem(System).bCompiled;
				}
				Transaction.Cancel();
				return ErrorResult(
					TEXT("Spec import failed while applying a default; all previously changed defaults were restored."),
					bRestored ? TEXT("parameter_default_invalid") : TEXT("restore_verification_failed"),
					500);
			}

			FCompileSummary ImportCompileSummary;
			if (bGraphChanged)
			{
				ImportCompileSummary = CompileSystem(System);
				if (!ImportCompileSummary.bCompiled)
				{
					bool bRestored = RestoreImportChanges();
					if (bRestored)
						bRestored = CompileSystem(System).bCompiled;
					Transaction.Cancel();
					return ErrorResult(
						FString::Printf(TEXT("Spec import compilation failed (status=%s); authored changes were restored."), *ImportCompileSummary.Status),
						bRestored ? TEXT("compile_failed") : TEXT("restore_verification_failed"), 500);
				}
			}
			if (Applied > 0 || bGraphChanged) System->MarkPackageDirty();
			// Re-export the authored asset and compare the canonical digest. This is
			// the readback proof for both no-op and changed imports; a transport-level
			// success flag alone is insufficient.
			FMCPToolResult ReadbackExport = FTool_NiagaraSystemSpecExport().Execute(ExportParams);
			FString SuppliedDigest;
			FString ReadbackDigest;
			const bool bReadback = ReadbackExport.bSuccess && ReadbackExport.Data
				&& SpecContentDigest(Spec, SuppliedDigest)
				&& ReadbackExport.Data->TryGetStringField(TEXT("specDigest"), ReadbackDigest)
				&& SuppliedDigest == ReadbackDigest;
			if (!bReadback)
			{
				bool bRestored = RestoreImportChanges();
				if (bRestored)
					bRestored = CompileSystem(System).bCompiled;
				Transaction.Cancel();
				return ErrorResult(
					FString::Printf(TEXT("Spec import read-back digest did not match the supplied spec; authored changes were restored (supplied=%s readback=%s)."), *SuppliedDigest, *ReadbackDigest),
					bRestored ? TEXT("readback_verification_failed") : TEXT("restore_verification_failed"),
					500);
			}
			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-spec-import.v1"));
			Json->SetStringField(TEXT("system"), System->GetPathName());
			Json->SetStringField(TEXT("requestId"), RequestId);
			Json->SetNumberField(TEXT("appliedUserParameters"), Applied);
			Json->SetNumberField(TEXT("appliedModuleInputs"), AppliedModuleEdits);
			Json->SetNumberField(TEXT("appliedRendererProperties"), AppliedRendererEdits);
			Json->SetBoolField(TEXT("changed"), Applied > 0 || bGraphChanged);
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("readbackVerified"), bReadback);
			Json->SetBoolField(TEXT("roundTripVerified"), bReadback);
			Json->SetStringField(TEXT("importMode"), TEXT("userParameterDefaultsAndAuthoredSystemSpec"));
			Json->SetBoolField(TEXT("compiled"), bGraphChanged ? ImportCompileSummary.bCompiled : false);
			return FMCPToolResult::Ok(Json);
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
		TestFalse(TEXT("Graph path alone cannot select an output"), FindTarget(System, Request, Target, ErrorCode,
		                                                                       Error));
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
			if (TestTrue(
				TEXT("Canonical output resolves shared graph ambiguity"),
				FindTarget(System, Request, Target, ErrorCode, Error)))
			{
				TestTrue(TEXT("Requested output is selected"), Target.OutputNode == Output);
				++SelectedOutputs;
			}
		}
		TestTrue(TEXT("Emitter and particle spawn/update outputs were exercised"), SelectedOutputs >= 4);
		Request.OutputNodePath = Graph->GetPathName() + TEXT(".MissingOutput");
		TestFalse(
			TEXT("Unknown output does not fall back to another stage"),
			FindTarget(System, Request, Target, ErrorCode, Error));
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
		Registry.Register(MakeShared<FTool_NiagaraModuleList>());
		Registry.Register(MakeShared<FTool_NiagaraModuleInputsList>());
		Registry.Register(MakeShared<FTool_NiagaraModuleRemovePlan>());
		Registry.Register(MakeShared<FTool_NiagaraModuleRemoveApply>());
		Registry.Register(MakeShared<FTool_NiagaraModuleRemoveRollback>());
		Registry.Register(MakeShared<FTool_NiagaraModuleMovePlan>());
		Registry.Register(MakeShared<FTool_NiagaraModuleMoveApply>());
		Registry.Register(MakeShared<FTool_NiagaraModuleMoveRollback>());
		Registry.Register(MakeShared<FTool_NiagaraModuleInputValuePlan>());
		Registry.Register(MakeShared<FTool_NiagaraModuleInputValueApply>());
		Registry.Register(MakeShared<FTool_NiagaraModuleInputValueRollback>());
		Registry.Register(MakeShared<FTool_NiagaraModuleInputBindingSet>());
		Registry.Register(MakeShared<FTool_NiagaraModuleInputDISet>());
		Registry.Register(MakeShared<FTool_NiagaraEmitterModulesClear>());
		Registry.Register(MakeShared<FTool_NiagaraSystemSpecExport>());
		Registry.Register(MakeShared<FTool_NiagaraSystemSpecImport>());
		Registry.Register(MakeShared<FTool_NiagaraSystemSpecRoundTrip>());
	}

	void RegisterNiagaraDynamicInputTools(FMCPToolRegistry& Registry)
	{
		using namespace UEAINiagaraModulePrivate;
		Registry.Register(MakeShared<FTool_NiagaraModuleDynamicInputsList>());
		Registry.Register(MakeShared<FTool_NiagaraModuleDynamicInputsTree>());
		Registry.Register(MakeShared<FTool_NiagaraDynamicInputInputsGet>());
		Registry.Register(MakeShared<FTool_NiagaraModuleDynamicInputValueGet>());
		Registry.Register(MakeShared<FTool_NiagaraDynamicInputSearch>());
		Registry.Register(MakeShared<FTool_NiagaraModuleDynamicInputRemove>());
		Registry.Register(MakeShared<FTool_NiagaraDynamicInputAddPlan>());
		Registry.Register(MakeShared<FTool_NiagaraDynamicInputAddApply>());
		Registry.Register(MakeShared<FTool_NiagaraDynamicInputAddRollback>());
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
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.list")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.inputs.list")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.remove.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.remove.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.remove.rollback")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.move.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.move.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.move.rollback")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.input.value.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.input.value.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.input.value.rollback")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.input.binding.set")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.graph.module.input.di.set")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.emitter.modules.clear")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.system.spec.export")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.system.spec.import")));
		Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(
			TEXT("content.niagara.system.spec.round_trip")));
	}

	void RegisterNiagaraDynamicInputTools(FMCPToolRegistry& Registry)
	{
		for (const TCHAR* Id : {
			     TEXT("content.niagara.graph.module.dynamic_inputs.list"),
			     TEXT("content.niagara.graph.module.dynamic_inputs.tree"),
			     TEXT("content.niagara.graph.dynamic_input.inputs.get"),
			     TEXT("content.niagara.graph.module.dynamic_input.value.get"),
			     TEXT("content.niagara.graph.dynamic_input.search"),
			     TEXT("content.niagara.graph.module.dynamic_input.remove"),
			     TEXT("content.niagara.graph.module.dynamic_input.add.plan"),
			     TEXT("content.niagara.graph.module.dynamic_input.add.apply"),
			     TEXT("content.niagara.graph.module.dynamic_input.add.rollback")
		     })
		{
			Registry.Register(MakeShared<FUnavailableNiagaraModuleTool>(Id));
		}
	}
}

#endif
