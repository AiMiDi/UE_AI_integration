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

#include "Infrastructure/NiagaraGraphNotifications.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EdGraphSchema_Niagara.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UEAINiagaraEventHandlerPrivate
{
	using UEAIIntegration::Infrastructure::TryDigestJson;
	using UEAIIntegration::Infrastructure::ValidateChangeApproval;

	constexpr int32 MaxPathCharacters = 2048;
	constexpr int32 MaxEmitterSelectorCharacters = 512;
	constexpr int32 MaxEventNameCharacters = 128;
	constexpr uint32 MaxSpawnCount = 1000000;
	constexpr int32 MaxPageOffset = 65536;
	constexpr int32 MaxPageSize = 128;

#if WITH_DEV_AUTOMATION_TESTS
	// Verdict injection is internal to native tests, after real authoring and
	// compilation. It never changes the public request or approval contract.
	static bool bFailNextUpdateCompileForTests = false;
	static bool bFailNextUpdateReadbackForTests = false;
	void SetUpdateFailureForTests(bool bCompileFailure, bool bReadbackFailure)
	{
		check(IsInGameThread());
		bFailNextUpdateCompileForTests = bCompileFailure;
		bFailNextUpdateReadbackForTests = bReadbackFailure;
	}
#endif

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

	struct FGraphSnapshot;

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
		FNiagaraEventScriptProperties BeforeProperties;
		TSharedPtr<FGraphSnapshot> BeforeGraphSnapshot;
		TStrongObjectPtr<UNiagaraScript> BeforeScript;
		FString GraphAfterDigest;
		bool bHasBeforeProperties = false;
		FString Operation = TEXT("add");
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
	};

	struct FGraphPinSnapshot
	{
		UEdGraphPin* Pin = nullptr;
		TArray<UEdGraphPin*> LinkedTo;
	};

	struct FGraphNodeSnapshot
	{
		TStrongObjectPtr<UEdGraphNode> Node;
		TArray<FGraphPinSnapshot> Pins;
	};

	struct FGraphSnapshot
	{
		FString Export;
		FString Digest;
		int32 NodeCount = 0;
		bool bCaptured = false;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TArray<FGraphNodeSnapshot> Nodes;
	};

	FString GraphSnapshotDigest(const FString& Export)
	{
		FTCHARToUTF8 Bytes(*Export);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	bool CaptureGraphSnapshot(UNiagaraGraph* Graph, FGraphSnapshot& OutSnapshot)
	{
		OutSnapshot = FGraphSnapshot();
		if (!Graph)
		{
			return false;
		}
		TSet<UObject*> Nodes;
		OutSnapshot.Graph = Graph;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node) || Node->GetGraph() != Graph || Nodes.Contains(Node))
			{
				return false;
			}
			Nodes.Add(Node);
			FGraphNodeSnapshot& NodeSnapshot = OutSnapshot.Nodes.AddDefaulted_GetRef();
			NodeSnapshot.Node.Reset(Node);
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					return false;
				}
				NodeSnapshot.Pins.Add({Pin, Pin->LinkedTo});
			}
		}
		FEdGraphUtilities::ExportNodesToText(Nodes, OutSnapshot.Export);
		OutSnapshot.Digest = GraphSnapshotDigest(OutSnapshot.Export);
		OutSnapshot.NodeCount = Nodes.Num();
		OutSnapshot.bCaptured = true;
		return true;
	}

	bool CanRestoreGraphSnapshot(UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot)
	{
		if (!Graph || !Snapshot.bCaptured || Snapshot.Graph.Get() != Graph
			|| Snapshot.Nodes.Num() != Snapshot.NodeCount)
		{
			return false;
		}
		TSet<UEdGraphNode*> OriginalNodes;
		TSet<UEdGraphPin*> OriginalPins;
		for (const FGraphNodeSnapshot& NodeSnapshot : Snapshot.Nodes)
		{
			UEdGraphNode* Node = NodeSnapshot.Node.Get();
			if (!IsValid(Node) || Node->GetGraph() != Graph || OriginalNodes.Contains(Node)
				|| Node->Pins.Num() != NodeSnapshot.Pins.Num())
			{
				return false;
			}
			OriginalNodes.Add(Node);
			for (int32 Index = 0; Index < NodeSnapshot.Pins.Num(); ++Index)
			{
				UEdGraphPin* Pin = NodeSnapshot.Pins[Index].Pin;
				// Strong node references do not protect pins from reconstruction.
				// Check membership before ever dereferencing a recorded pin.
				if (!Pin || Node->Pins[Index] != Pin || OriginalPins.Contains(Pin))
				{
					return false;
				}
				OriginalPins.Add(Pin);
			}
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!OriginalNodes.Contains(Node))
			{
				return false;
			}
		}
		for (const FGraphNodeSnapshot& NodeSnapshot : Snapshot.Nodes)
		{
			for (const FGraphPinSnapshot& PinSnapshot : NodeSnapshot.Pins)
			{
				for (UEdGraphPin* LinkedPin : PinSnapshot.LinkedTo)
				{
					if (!OriginalPins.Contains(LinkedPin))
					{
						return false;
					}
				}
				for (UEdGraphPin* LinkedPin : PinSnapshot.Pin->LinkedTo)
				{
					if (!OriginalPins.Contains(LinkedPin)
						|| !LinkedPin->LinkedTo.Contains(PinSnapshot.Pin))
					{
						return false;
					}
				}
			}
		}
		return true;
	}

	bool RestoreGraphSnapshot(UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot)
	{
		if (!CanRestoreGraphSnapshot(Graph, Snapshot))
		{
			return false;
		}
		// Removal receipts live in this editor session. Restore the retained
		// original nodes and connections directly: Niagara output nodes cannot
		// reliably round-trip through the generic clipboard text importer.
		Graph->Modify();
		Graph->Nodes.Reset();
		for (const FGraphNodeSnapshot& NodeSnapshot : Snapshot.Nodes)
		{
			UEdGraphNode* Node = NodeSnapshot.Node.Get();
			Node->Modify();
			Graph->Nodes.Add(Node);
			for (const FGraphPinSnapshot& PinSnapshot : NodeSnapshot.Pins)
			{
				PinSnapshot.Pin->Modify();
				PinSnapshot.Pin->LinkedTo = PinSnapshot.LinkedTo;
			}
		}
		UEAIIntegration::NiagaraEditing::NotifyRestoredGraph(Graph);
		if (!CanRestoreGraphSnapshot(Graph, Snapshot) || Graph->Nodes.Num() != Snapshot.NodeCount)
		{
			return false;
		}
		for (const FGraphNodeSnapshot& NodeSnapshot : Snapshot.Nodes)
		{
			for (const FGraphPinSnapshot& PinSnapshot : NodeSnapshot.Pins)
			{
				if (PinSnapshot.Pin->LinkedTo != PinSnapshot.LinkedTo)
				{
					return false;
				}
			}
		}
		FGraphSnapshot Restored;
		return CaptureGraphSnapshot(Graph, Restored) && Restored.Digest == Snapshot.Digest;
	}

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

	bool ReadPageField(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Name,
		const int32 DefaultValue,
		const int32 MinimumValue,
		const int32 MaximumValue,
		int32& OutValue)
	{
		OutValue = DefaultValue;
		if (!Params->HasField(Name))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(Name, Number)
			|| !FMath::IsFinite(Number)
			|| Number < static_cast<double>(MinimumValue)
			|| Number > static_cast<double>(MaximumValue)
			|| Number != FMath::FloorToDouble(Number))
		{
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
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

	bool ValidateRequestOptions(
		const FEventHandlerRequest& Request,
		FString& OutErrorCode,
		FString& OutError)
	{
		if (Request.MinSpawnNumber > Request.SpawnNumber)
		{
			OutErrorCode = TEXT("spawn_range_invalid");
			OutError = TEXT("minSpawnNumber cannot exceed spawnNumber.");
			return false;
		}
		if (Request.ExecutionMode != EScriptExecutionMode::SpawnedParticles
			&& (Request.SpawnNumber != 0 || Request.MinSpawnNumber != 0 || Request.bRandomSpawnNumber))
		{
			OutErrorCode = TEXT("spawn_options_invalid");
			OutError = TEXT("spawnNumber, minSpawnNumber, and randomSpawnNumber apply only to SpawnedParticles.");
			return false;
		}
		return true;
	}

	bool ParseRequest(
		const TSharedPtr<FJsonObject>& Params,
		FEventHandlerRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutError,
		const bool bValidateCompleteOptions = true)
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
				|| (!SourceEmitterIdString.IsEmpty()
					&& (!FGuid::Parse(SourceEmitterIdString, OutRequest.SourceEmitterId)
						|| !OutRequest.SourceEmitterId.IsValid())))
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
		return !bValidateCompleteOptions || ValidateRequestOptions(OutRequest, OutErrorCode, OutError);
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

	UNiagaraNodeOutput* CreateEventGraphOutput(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		if (!Graph)
		{
			return nullptr;
		}
		if (UNiagaraNodeOutput* Existing = Graph->FindEquivalentOutputNode(
			ENiagaraScriptUsage::ParticleEventScript, UsageId))
		{
			UEdGraphPin* ExistingInput = FindParameterMapPin(Existing, EGPD_Input);
			if (ExistingInput && ExistingInput->LinkedTo.Num() == 1)
			{
				return Existing;
			}
			Existing->Modify();
			Existing->DestroyNode();
		}

		Graph->Modify();
		FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
		UNiagaraNodeOutput* Output = OutputCreator.CreateNode();
		Output->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
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

		OutputInput->BreakAllPinLinks();
		OutputInput->MakeLinkTo(InputOutput);
		Graph->NotifyGraphChanged();
		return Output;
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
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin)
						{
							LinkedPin->GetOwningNode()->Modify();
						}
					}
					// Avoid reconstruction callbacks removing orphaned pins which
					// the session receipt must retain for an exact graph restore.
					Pin->BreakAllPinLinks(false);
				}
				Graph->RemoveNode(Node, false);
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

	void SetHandlerProperties(FNiagaraEventScriptProperties& Properties, const FEventHandlerRequest& Request)
	{
		Properties.ExecutionMode = Request.ExecutionMode;
		Properties.SpawnNumber = Request.SpawnNumber;
		Properties.MinSpawnNumber = Request.MinSpawnNumber;
		Properties.MaxEventsPerFrame = Request.MaxEventsPerFrame;
		Properties.bRandomSpawnNumber = Request.bRandomSpawnNumber;
		Properties.UpdateAttributeInitialValues = Request.bUpdateAttributeInitialValues;
		Properties.SourceEmitterID = Request.SourceEmitterId;
		Properties.SourceEventName = FName(*Request.SourceEventName);
	}

	FEventHandlerRequest RequestFromProperties(
		const FString& SystemPath,
		const FString& EmitterName,
		const FGuid& UsageId,
		const FNiagaraEventScriptProperties& Properties)
	{
		FEventHandlerRequest Request;
		Request.SystemPath = SystemPath;
		Request.EmitterSelector = EmitterName;
		Request.UsageId = UsageId;
		Request.SourceEmitterId = Properties.SourceEmitterID;
		Request.SourceEventName = Properties.SourceEventName.ToString();
		Request.ExecutionMode = Properties.ExecutionMode;
		Request.SpawnNumber = Properties.SpawnNumber;
		Request.MinSpawnNumber = Properties.MinSpawnNumber;
		Request.MaxEventsPerFrame = Properties.MaxEventsPerFrame;
		Request.bRandomSpawnNumber = Properties.bRandomSpawnNumber;
		Request.bUpdateAttributeInitialValues = Properties.UpdateAttributeInitialValues;
		return Request;
	}

	FCompileSummary CompileSystem(UNiagaraSystem* System)
	{
		FCompileSummary Summary;
		if (!System)
		{
			return Summary;
		}
		System->RequestCompile(false);
		System->WaitForCompilationComplete(true, false);
		Summary.Status = System->HasOutstandingCompilationRequests(false) ? TEXT("dirty") : TEXT("upToDate");
		Summary.bCompiled = !System->HasOutstandingCompilationRequests(false);
		return Summary;
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
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-event-handler.v1"));
		Result->SetStringField(TEXT("status"), TEXT("succeeded"));
		Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
		Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Result->SetStringField(TEXT("system"), Receipt.SystemPath);
		Result->SetStringField(TEXT("emitter"), Receipt.EmitterName);
		Result->SetStringField(TEXT("emitterPath"), Receipt.EmitterPath);
		Result->SetStringField(TEXT("graph"), Receipt.GraphPath);
		Result->SetStringField(TEXT("script"), Receipt.ScriptPath);
		Result->SetStringField(TEXT("action"), Receipt.Operation);
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

	class FTool_NiagaraEventHandlerList final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.list"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid())
			{
				return ErrorResult(TEXT("A Niagara event-handler list request is required."), TEXT("invalid_request"));
			}
			FEventHandlerRequest Request;
			if (!Params->TryGetStringField(TEXT("system"), Request.SystemPath)
				|| Request.SystemPath.TrimStartAndEnd().IsEmpty())
			{
				return ErrorResult(TEXT("system is required."), TEXT("system_required"));
			}
			if (!Params->TryGetStringField(TEXT("emitter"), Request.EmitterSelector)
				|| Request.EmitterSelector.TrimStartAndEnd().IsEmpty()
				|| Request.EmitterSelector.Len() > MaxEmitterSelectorCharacters)
			{
				return ErrorResult(
					TEXT("emitter must be a bounded handle ID or display name."), TEXT("emitter_required"));
			}
			int32 Offset = 0;
			int32 Limit = 32;
			if (!ReadPageField(Params, TEXT("offset"), 0, 0, MaxPageOffset, Offset)
				|| !ReadPageField(Params, TEXT("limit"), 32, 1, MaxPageSize, Limit))
			{
				return ErrorResult(
					TEXT("offset must be an integer from 0 to 65536 and limit must be an integer from 1 to 128."),
					TEXT("page_invalid"));
			}

			Request.SystemPath = Request.SystemPath.TrimStartAndEnd();
			Request.EmitterSelector = Request.EmitterSelector.TrimStartAndEnd();
			FEventHandlerTarget Target;
			FString ErrorCode;
			FString Error;
			if (!ResolveTarget(Request, Target, ErrorCode, Error))
			{
				return ErrorResult(Error, ErrorCode, ErrorCode.Contains(TEXT("not_found")) ? 404 : 422);
			}

			const TArray<FNiagaraEventScriptProperties>& Handlers = Target.Data->EventHandlerScriptProps;
			const int32 End = FMath::Min(Handlers.Num(), Offset + Limit);
			TArray<TSharedPtr<FJsonValue>> Rows;
			Rows.Reserve(FMath::Max(0, End - Offset));
			for (int32 Index = Offset; Index < End; ++Index)
			{
				const FNiagaraEventScriptProperties& Handler = Handlers[Index];
				auto Row = MakeShared<FJsonObject>();
				const UNiagaraScript* Script = Handler.Script;
				const FGuid UsageId = Script ? Script->GetUsageId() : FGuid();
				Row->SetStringField(TEXT("usageId"), UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
				Row->SetStringField(TEXT("script"), Script ? Script->GetPathName() : FString());
				Row->SetStringField(TEXT("executionMode"), ExecutionModeName(Handler.ExecutionMode));
				Row->SetNumberField(TEXT("spawnNumber"), Handler.SpawnNumber);
				Row->SetNumberField(TEXT("minSpawnNumber"), Handler.MinSpawnNumber);
				Row->SetNumberField(TEXT("maxEventsPerFrame"), Handler.MaxEventsPerFrame);
				Row->SetBoolField(TEXT("randomSpawnNumber"), Handler.bRandomSpawnNumber);
				Row->SetBoolField(TEXT("updateAttributeInitialValues"), Handler.UpdateAttributeInitialValues);
				Row->SetStringField(
					TEXT("sourceEmitterId"),
					Handler.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
				Row->SetStringField(TEXT("sourceEventName"), Handler.SourceEventName.ToString());
				Row->SetBoolField(TEXT("graphPresent"), UsageId.IsValid() && HasEventGraph(Target.Graph, UsageId));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}

			auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.event-handlers.v1"));
			Result->SetStringField(TEXT("system"), Target.System->GetPathName());
			Result->SetStringField(TEXT("emitter"), Target.EmitterName);
			Result->SetStringField(TEXT("emitterPath"), Target.EmitterPath);
			Result->SetStringField(TEXT("graph"), Target.GraphPath);
			Result->SetNumberField(TEXT("total"), Handlers.Num());
			Result->SetNumberField(TEXT("offset"), Offset);
			Result->SetNumberField(TEXT("limit"), Limit);
			Result->SetBoolField(TEXT("hasMore"), End < Handlers.Num());
			Result->SetNumberField(TEXT("nextOffset"), End);
			Result->SetArrayField(TEXT("eventHandlers"), Rows);
			return FMCPToolResult::Ok(Result);
		}
	};

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
			SetHandlerProperties(Properties, Data.Request);
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
			// UNiagaraEmitter::AddEventHandler only records the authored script
			// properties. The editor also creates the matching output node; do
			// that explicitly so read-back and rollback observe the same graph
			// shape as a native Niagara edit.
			UNiagaraNodeOutput* EventOutput = CreateEventGraphOutput(Data.Target.Graph, Data.Request.UsageId);
			if (!Properties.Script || Properties.Script->GetUsage() != ENiagaraScriptUsage::ParticleEventScript)
			{
				return ErrorResult(
					TEXT("The Niagara editor did not create the event-handler script."),
					TEXT("event_handler_create_failed"), 500);
			}
			if (!EventOutput || !HasEventGraph(Data.Target.Graph, Data.Request.UsageId))
			{
				return ErrorResult(
					TEXT("The Niagara editor did not create the event-handler graph output."),
					TEXT("event_handler_graph_create_failed"), 500);
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
			Receipt.Operation = TEXT("add");

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

	TMap<FString, FString>& UpdateRequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	TMap<FString, FString>& RemoveRequestReceiptIds()
	{
		static TMap<FString, FString> Values;
		return Values;
	}

	bool BuildExistingPlanData(
		const TSharedPtr<FJsonObject>& Params,
		FEventHandlerPlanData& OutData,
		FString& OutErrorCode,
		FString& OutError)
	{
		OutData = FEventHandlerPlanData();
		if (!ParseRequest(Params, OutData.Request, OutErrorCode, OutError, false)
			|| !ResolveTarget(OutData.Request, OutData.Target, OutErrorCode, OutError))
		{
			return false;
		}
		FNiagaraEventScriptProperties* Existing = FindHandler(OutData.Target.Data, OutData.Request.UsageId);
		if (!Existing || !Existing->Script || !HasEventGraph(OutData.Target.Graph, OutData.Request.UsageId))
		{
			OutErrorCode = TEXT("handler_not_found");
			OutError = TEXT("The requested event handler does not exist with a matching graph output.");
			return false;
		}
		// Update plans are patches. Keep every option the caller omitted at its
		// authored value; this also makes remove plans independent of irrelevant
		// option defaults.
		if (!Params->HasField(TEXT("sourceEmitterId"))) OutData.Request.SourceEmitterId = Existing->SourceEmitterID;
		if (!Params->HasField(TEXT("sourceEventName"))) OutData.Request.SourceEventName = Existing->SourceEventName.ToString();
		if (!Params->HasField(TEXT("executionMode"))) OutData.Request.ExecutionMode = Existing->ExecutionMode;
		if (!Params->HasField(TEXT("spawnNumber"))) OutData.Request.SpawnNumber = Existing->SpawnNumber;
		if (!Params->HasField(TEXT("minSpawnNumber"))) OutData.Request.MinSpawnNumber = Existing->MinSpawnNumber;
		if (!Params->HasField(TEXT("maxEventsPerFrame"))) OutData.Request.MaxEventsPerFrame = Existing->MaxEventsPerFrame;
		if (!Params->HasField(TEXT("randomSpawnNumber"))) OutData.Request.bRandomSpawnNumber = Existing->bRandomSpawnNumber;
		if (!Params->HasField(TEXT("updateAttributeInitialValues"))) OutData.Request.bUpdateAttributeInitialValues = Existing->UpdateAttributeInitialValues;
		if (!ValidateRequestOptions(OutData.Request, OutErrorCode, OutError))
		{
			return false;
		}
		const UPackage* Package = OutData.Target.System->GetOutermost();
		OutData.bBlocked = !Package || !Package->GetName().StartsWith(TEXT("/Game/"))
			|| OutData.Target.Emitter->GetOutermost() != Package;
		if (OutData.bBlocked)
		{
			OutData.Risks.Add(TEXT("The selected System or emitter is not an owned non-transient /Game/ asset."));
		}
		OutData.Warnings.Add(TEXT("The operation changes authored event-handler properties and does not prove runtime event delivery."));
		return true;
	}

	TSharedRef<FJsonObject> BuildExistingPlanJson(const FEventHandlerPlanData& Data, const TCHAR* Action)
	{
		TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
		Plan->SetStringField(TEXT("planKind"), FString::Printf(TEXT("niagaraEventHandler%s"), Action));
		Plan->SetStringField(TEXT("action"), FString::Printf(TEXT("%sEventHandler"), Action));
		TSharedPtr<FJsonObject> Preconditions = Plan->GetObjectField(TEXT("preconditions"));
		if (Preconditions.IsValid())
		{
			Preconditions->SetBoolField(TEXT("handlerPresent"), true);
		}
		TSharedPtr<FJsonObject> Before = Plan->GetObjectField(TEXT("before"));
		TSharedPtr<FJsonObject> After = Plan->GetObjectField(TEXT("after"));
		if (Before.IsValid()) Before->SetBoolField(TEXT("handlerPresent"), true);
		if (After.IsValid()) After->SetBoolField(TEXT("handlerPresent"), !FString(Action).Equals(TEXT("Remove")));
		if (FNiagaraEventScriptProperties* Existing = FindHandler(Data.Target.Data, Data.Request.UsageId))
		{
			if (Before.IsValid())
			{
				Before->SetStringField(TEXT("sourceEmitterId"), Existing->SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
				Before->SetStringField(TEXT("sourceEventName"), Existing->SourceEventName.ToString());
				Before->SetStringField(TEXT("executionMode"), ExecutionModeName(Existing->ExecutionMode));
				Before->SetNumberField(TEXT("spawnNumber"), Existing->SpawnNumber);
				Before->SetNumberField(TEXT("minSpawnNumber"), Existing->MinSpawnNumber);
				Before->SetNumberField(TEXT("maxEventsPerFrame"), Existing->MaxEventsPerFrame);
				Before->SetBoolField(TEXT("randomSpawnNumber"), Existing->bRandomSpawnNumber);
				Before->SetBoolField(TEXT("updateAttributeInitialValues"), Existing->UpdateAttributeInitialValues);
			}
			if (After.IsValid() && !FString(Action).Equals(TEXT("Remove")))
			{
				After->SetStringField(TEXT("sourceEmitterId"), Data.Request.SourceEmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
				After->SetStringField(TEXT("sourceEventName"), Data.Request.SourceEventName);
				After->SetStringField(TEXT("executionMode"), ExecutionModeName(Data.Request.ExecutionMode));
				After->SetNumberField(TEXT("spawnNumber"), Data.Request.SpawnNumber);
				After->SetNumberField(TEXT("minSpawnNumber"), Data.Request.MinSpawnNumber);
				After->SetNumberField(TEXT("maxEventsPerFrame"), Data.Request.MaxEventsPerFrame);
				After->SetBoolField(TEXT("randomSpawnNumber"), Data.Request.bRandomSpawnNumber);
				After->SetBoolField(TEXT("updateAttributeInitialValues"), Data.Request.bUpdateAttributeInitialValues);
			}
		}
		return Plan;
	}

	class FTool_NiagaraEventHandlerUpdatePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.update.plan"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FEventHandlerPlanData Data; FString Code, Error;
			if (!BuildExistingPlanData(Params, Data, Code, Error)) return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Update"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest)) return ErrorResult(TEXT("Unable to compute the event-handler update plan digest."), TEXT("digest_unavailable"), 500);
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraEventHandlerUpdateApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.update.apply"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId, Code, Error;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty()) return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			if (const FString* ExistingId = UpdateRequestReceiptIds().Find(RequestId))
			{
				if (FEventHandlerReceipt* Existing = Receipts().Find(*ExistingId))
				{
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Error)) return ErrorResult(Error, Code, 409);
					return FMCPToolResult::Ok(MakeResult(*Existing, true));
				}
				return ErrorResult(TEXT("requestId is associated with an unavailable receipt."), TEXT("request_id_conflict"), 409);
			}
			FEventHandlerPlanData Data;
			if (!BuildExistingPlanData(Params, Data, Code, Error)) return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Update"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest)) return ErrorResult(TEXT("Unable to compute the event-handler update plan digest."), TEXT("digest_unavailable"), 500);
			if (!ValidateChangeApproval(Params, Digest, Code, Error)) return ErrorResult(Error, Code, 409);
			if (Data.bBlocked) return ErrorResult(TEXT("The event-handler target is read-only or not owned by the System package."), TEXT("plan_blocked"), 409);
			if (Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.Target.EmitterChangeId) return ErrorResult(TEXT("The Niagara emitter changed after the plan was created; re-plan before applying."), TEXT("plan_digest_mismatch"), 409);
			FNiagaraEventScriptProperties* Handler = FindHandler(Data.Target.Data, Data.Request.UsageId);
			if (!Handler || !Handler->Script) return ErrorResult(TEXT("The event handler disappeared before apply."), TEXT("handler_not_found"), 404);
			TMap<UPackage*, bool> PackageDirtyBefore;
			for (UObject* Object : {static_cast<UObject*>(Data.Target.System), static_cast<UObject*>(Data.Target.Emitter),
				static_cast<UObject*>(Data.Target.Graph), static_cast<UObject*>(Handler->Script)})
			{
				if (Object) PackageDirtyBefore.FindOrAdd(Object->GetOutermost(), Object->GetOutermost()->IsDirty());
			}
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Update Niagara Event Handler")));
			Data.Target.System->Modify(); Data.Target.Emitter->Modify();
			FEventHandlerReceipt Receipt;
			Receipt.BeforeProperties = *Handler;
			Receipt.bHasBeforeProperties = true;
			SetHandlerProperties(*Handler, Data.Request);
			FCompileSummary Compile = CompileHandler(Data.Target.System, Handler->Script);
			bool bReadBack = ReadBackMatches(Data.Target, Data.Request);
#if WITH_DEV_AUTOMATION_TESTS
			if (bFailNextUpdateCompileForTests)
			{
				Compile.bCompiled = false;
				Compile.Status = TEXT("injectedUpdateCompileFailure");
			}
			if (bFailNextUpdateReadbackForTests) bReadBack = false;
			bFailNextUpdateCompileForTests = false;
			bFailNextUpdateReadbackForTests = false;
#endif
			if (!bReadBack || !Compile.bCompiled)
			{
				*Handler = Receipt.BeforeProperties;
				const FCompileSummary RestoreCompile = CompileHandler(Data.Target.System, Receipt.BeforeProperties.Script);
				const FEventHandlerRequest BeforeRequest = RequestFromProperties(Data.Target.System->GetPathName(),
					Data.Target.EmitterName, Data.Request.UsageId, Receipt.BeforeProperties);
				const FNiagaraEventScriptProperties* RestoredHandler = FindHandler(Data.Target.Data, Data.Request.UsageId);
				const bool bRestored = RestoreCompile.bCompiled && RestoredHandler
					&& RestoredHandler->Script == Receipt.BeforeProperties.Script
					&& ReadBackMatches(Data.Target, BeforeRequest);
				if (bRestored)
				{
					Transaction.Cancel();
					for (const auto& PackageState : PackageDirtyBefore) PackageState.Key->SetDirtyFlag(PackageState.Value);
				}
				FMCPToolResult Failure = ErrorResult(FString::Printf(
					TEXT("Event-handler update read-back or compilation failed (status=%s); %s"), *Compile.Status,
					bRestored ? TEXT("the original properties and compilation were restored and verified.")
						: TEXT("restoration was not verified; the transaction was retained for Editor Undo.")),
					bRestored ? (!bReadBack ? TEXT("verification_failed") : TEXT("compile_failed")) : TEXT("restore_verification_failed"), 500);
				Failure.Data = MakeShared<FJsonObject>();
				Failure.Data->SetBoolField(TEXT("restorationVerified"), bRestored);
				Failure.Data->SetBoolField(TEXT("restorationCompiled"), RestoreCompile.bCompiled);
				return Failure;
			}
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId; Receipt.PlanDigest = Digest; Receipt.SystemPath = Data.Target.System->GetPathName();
			Receipt.EmitterName = Data.Target.EmitterName; Receipt.EmitterPath = Data.Target.EmitterPath; Receipt.GraphPath = Data.Target.GraphPath;
			Receipt.ScriptPath = Handler->Script->GetPathName(); Receipt.EmitterVersion = Data.Target.EmitterVersion; Receipt.UsageId = Data.Request.UsageId;
			Receipt.System = Data.Target.System; Receipt.Emitter = Data.Target.Emitter; Receipt.Graph = Data.Target.Graph;
			Receipt.EmitterChangeIdAfter = Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.Operation = TEXT("update"); Receipt.bChanged = true; Receipt.bCompiled = Compile.bCompiled; Receipt.CompileStatus = Compile.Status;
			Data.Target.System->MarkPackageDirty(); Receipts().Add(Receipt.ReceiptId, Receipt); UpdateRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeResult(Receipt, false));
		}
	};

	class FTool_NiagaraEventHandlerUpdateRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.update.rollback"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId, RequestId; bool bConfirm = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId) || !Params->TryGetStringField(TEXT("requestId"), RequestId) || !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirm) || ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirm) return ErrorResult(TEXT("rollbackId, requestId and confirmWrite=true are required."), TEXT("write_confirmation_required"));
			FEventHandlerReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt || Receipt->Operation != TEXT("update") || !Receipt->bHasBeforeProperties) return ErrorResult(TEXT("The event-handler update receipt is unknown."), TEXT("receipt_not_found"), 404);
			if (Receipt->bRolledBack) return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			if (Receipt->RequestId != RequestId) return ErrorResult(TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			UNiagaraSystem* System = Receipt->System.Get(); UNiagaraEmitter* Emitter = Receipt->Emitter.Get();
			if (!System || !Emitter || Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->EmitterChangeIdAfter) return ErrorResult(TEXT("The emitter changed after update; rollback was refused."), TEXT("rollback_conflict"), 409);
			FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(Receipt->EmitterVersion); FNiagaraEventScriptProperties* Handler = FindHandler(Data, Receipt->UsageId);
			if (!Handler || !Handler->Script || Handler->Script->GetPathName() != Receipt->ScriptPath) return ErrorResult(TEXT("The receipt-owned event handler is no longer present."), TEXT("rollback_conflict"), 409);
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Event Handler Update")));
			System->Modify(); Emitter->Modify(); *Handler = Receipt->BeforeProperties;
			const FCompileSummary Compile = CompileHandler(System, Handler->Script);
			const FEventHandlerTarget Target{System, Emitter, Data, nullptr, Receipt->Graph.Get(), Receipt->EmitterVersion, Receipt->EmitterName, Receipt->EmitterPath, Receipt->GraphPath, Receipt->EmitterChangeIdAfter};
			const FEventHandlerRequest Expected = RequestFromProperties(Receipt->SystemPath, Receipt->EmitterName, Receipt->UsageId, Receipt->BeforeProperties);
			if (!ReadBackMatches(Target, Expected) || !Compile.bCompiled) return ErrorResult(TEXT("Event-handler update rollback verification failed."), TEXT("rollback_verification_failed"), 500);
			System->MarkPackageDirty(); Receipt->bRolledBack = true; Receipt->bCompiled = Compile.bCompiled; Receipt->CompileStatus = Compile.Status;
			return FMCPToolResult::Ok(MakeResult(*Receipt, false));
		}
	};

	class FTool_NiagaraEventHandlerRemovePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.remove.plan"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FEventHandlerPlanData Data; FString Code, Error;
			if (!BuildExistingPlanData(Params, Data, Code, Error)) return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Remove"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest)) return ErrorResult(TEXT("Unable to compute the event-handler removal plan digest."), TEXT("digest_unavailable"), 500);
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraEventHandlerRemoveApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.remove.apply"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId, Code, Error;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty()) return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			if (const FString* ExistingId = RemoveRequestReceiptIds().Find(RequestId))
			{
				if (FEventHandlerReceipt* Existing = Receipts().Find(*ExistingId))
				{
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Error)) return ErrorResult(Error, Code, 409);
					return FMCPToolResult::Ok(MakeResult(*Existing, true));
				}
				return ErrorResult(TEXT("requestId is associated with an unavailable receipt."), TEXT("request_id_conflict"), 409);
			}
			FEventHandlerPlanData Data;
			if (!BuildExistingPlanData(Params, Data, Code, Error)) return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Remove"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest)) return ErrorResult(TEXT("Unable to compute the event-handler removal plan digest."), TEXT("digest_unavailable"), 500);
			if (!ValidateChangeApproval(Params, Digest, Code, Error)) return ErrorResult(Error, Code, 409);
			if (Data.bBlocked) return ErrorResult(TEXT("The event-handler target is read-only or not owned by the System package."), TEXT("plan_blocked"), 409);
			if (Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.Target.EmitterChangeId) return ErrorResult(TEXT("The Niagara emitter changed after the plan was created; re-plan before applying."), TEXT("plan_digest_mismatch"), 409);
			FNiagaraEventScriptProperties* Handler = FindHandler(Data.Target.Data, Data.Request.UsageId);
			if (!Handler || !Handler->Script) return ErrorResult(TEXT("The event handler disappeared before apply."), TEXT("handler_not_found"), 404);
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Remove Niagara Event Handler")));
			Data.Target.System->Modify(); Data.Target.Emitter->Modify(); Data.Target.Graph->Modify();
			FGraphSnapshot BeforeGraph;
			if (!CaptureGraphSnapshot(Data.Target.Graph, BeforeGraph)
				|| !CanRestoreGraphSnapshot(Data.Target.Graph, BeforeGraph))
			{
				return ErrorResult(TEXT("The event-handler graph could not be snapshotted safely."), TEXT("graph_snapshot_failed"), 500);
			}
			FEventHandlerReceipt Receipt;
			Receipt.BeforeProperties = *Handler; Receipt.bHasBeforeProperties = true;
			Receipt.BeforeScript.Reset(Handler->Script);
			const FString ScriptPath = Handler->Script->GetPathName();
			const bool bGraphRemoved = RemoveEventGraph(Data.Target.Graph, Data.Request.UsageId);
			Data.Target.Emitter->RemoveEventHandlerByUsageId(Data.Request.UsageId, Data.Target.EmitterVersion);
			const FCompileSummary Compile = CompileSystem(Data.Target.System);
			const bool bReadBack = bGraphRemoved && FindHandler(Data.Target.Data, Data.Request.UsageId) == nullptr;
			if (!bReadBack || !Compile.bCompiled)
			{
				const bool bGraphRestored = RestoreGraphSnapshot(Data.Target.Graph, BeforeGraph);
				if (!bGraphRestored || !HasEventGraph(Data.Target.Graph, Data.Request.UsageId))
				{
					return ErrorResult(TEXT("Event-handler removal failed and the original graph could not be restored; the transaction was retained for Editor Undo."), TEXT("restore_verification_failed"), 500);
				}
				if (!FindHandler(Data.Target.Data, Data.Request.UsageId))
				{
					Data.Target.Emitter->AddEventHandler(Receipt.BeforeProperties, Data.Target.EmitterVersion);
				}
				Data.Target.Data = Data.Target.Emitter->GetEmitterData(Data.Target.EmitterVersion);
				const FNiagaraEventScriptProperties* Restored = FindHandler(Data.Target.Data, Data.Request.UsageId);
				const FCompileSummary RestoreCompile = CompileHandler(Data.Target.System, Restored ? Restored->Script : nullptr);
				if (!bGraphRestored || !HasEventGraph(Data.Target.Graph, Data.Request.UsageId) || !Restored || !RestoreCompile.bCompiled)
				{
					return ErrorResult(FString::Printf(TEXT("Event-handler removal read-back or compilation failed and restoration was not verified (status=%s)."), *Compile.Status), TEXT("restore_verification_failed"), 500);
				}
				Transaction.Cancel();
				return ErrorResult(FString::Printf(TEXT("Event-handler removal read-back or compilation failed (status=%s); the graph was restored."), *Compile.Status), !bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"), 500);
			}
			FGraphSnapshot AfterGraph;
			if (!CaptureGraphSnapshot(Data.Target.Graph, AfterGraph))
			{
				const bool bGraphRestored = RestoreGraphSnapshot(Data.Target.Graph, BeforeGraph);
				if (!bGraphRestored || !HasEventGraph(Data.Target.Graph, Data.Request.UsageId))
				{
					return ErrorResult(TEXT("The removal result could not be snapshotted and the original graph could not be restored; the transaction was retained for Editor Undo."), TEXT("restore_verification_failed"), 500);
				}
				Data.Target.Emitter->AddEventHandler(Receipt.BeforeProperties, Data.Target.EmitterVersion);
				Data.Target.Data = Data.Target.Emitter->GetEmitterData(Data.Target.EmitterVersion);
				const FNiagaraEventScriptProperties* Restored = FindHandler(Data.Target.Data, Data.Request.UsageId);
				const FCompileSummary RestoreCompile = CompileHandler(Data.Target.System, Restored ? Restored->Script : nullptr);
				if (!bGraphRestored || !HasEventGraph(Data.Target.Graph, Data.Request.UsageId) || !Restored || !RestoreCompile.bCompiled)
					return ErrorResult(TEXT("The event-handler removal result could not be snapshotted safely and restoration was not verified."), TEXT("restore_verification_failed"), 500);
				Transaction.Cancel();
				return ErrorResult(TEXT("The event-handler removal result could not be snapshotted safely; the graph was restored."), TEXT("graph_snapshot_failed"), 500);
			}
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower); Receipt.RequestId = RequestId; Receipt.PlanDigest = Digest;
			Receipt.SystemPath = Data.Target.System->GetPathName(); Receipt.EmitterName = Data.Target.EmitterName; Receipt.EmitterPath = Data.Target.EmitterPath; Receipt.GraphPath = Data.Target.GraphPath;
			Receipt.ScriptPath = ScriptPath; Receipt.EmitterVersion = Data.Target.EmitterVersion; Receipt.UsageId = Data.Request.UsageId;
			Receipt.System = Data.Target.System; Receipt.Emitter = Data.Target.Emitter; Receipt.Graph = Data.Target.Graph; Receipt.EmitterChangeIdAfter = Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.BeforeGraphSnapshot = MakeShared<FGraphSnapshot>(MoveTemp(BeforeGraph));
			Receipt.GraphAfterDigest = AfterGraph.Digest;
			Receipt.Operation = TEXT("remove"); Receipt.bChanged = true; Receipt.bCompiled = Compile.bCompiled; Receipt.CompileStatus = Compile.Status;
			Data.Target.System->MarkPackageDirty(); Receipts().Add(Receipt.ReceiptId, Receipt); RemoveRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeResult(Receipt, false));
		}
	};

	class FTool_NiagaraEventHandlerRemoveRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.event_handler.remove.rollback"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId, RequestId; bool bConfirm = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId) || !Params->TryGetStringField(TEXT("requestId"), RequestId) || !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirm) || ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirm) return ErrorResult(TEXT("rollbackId, requestId and confirmWrite=true are required."), TEXT("write_confirmation_required"));
			FEventHandlerReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt || Receipt->Operation != TEXT("remove") || !Receipt->bHasBeforeProperties) return ErrorResult(TEXT("The event-handler removal receipt is unknown."), TEXT("receipt_not_found"), 404);
			if (Receipt->bRolledBack) return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			if (Receipt->RequestId != RequestId) return ErrorResult(TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			UNiagaraSystem* System = Receipt->System.Get(); UNiagaraEmitter* Emitter = Receipt->Emitter.Get(); UNiagaraGraph* Graph = Receipt->Graph.Get();
			if (!System || !Emitter || !Graph || Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->EmitterChangeIdAfter) return ErrorResult(TEXT("The emitter changed after removal; rollback was refused."), TEXT("rollback_conflict"), 409);
			FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			if (FindHandler(Data, Receipt->UsageId) || HasEventGraph(Graph, Receipt->UsageId)) return ErrorResult(TEXT("The removed event handler identity is already in use."), TEXT("rollback_conflict"), 409);
			if (!Receipt->BeforeGraphSnapshot.IsValid()) return ErrorResult(TEXT("The event-handler removal receipt has no graph snapshot."), TEXT("rollback_conflict"), 409);
			FGraphSnapshot CurrentGraph;
			if (!CaptureGraphSnapshot(Graph, CurrentGraph) || CurrentGraph.Digest != Receipt->GraphAfterDigest)
			{
				return ErrorResult(TEXT("The event-handler graph changed after removal; rollback was refused."), TEXT("rollback_conflict"), 409);
			}
			const FGraphSnapshot& BeforeGraph = *Receipt->BeforeGraphSnapshot;
			if (!CanRestoreGraphSnapshot(Graph, BeforeGraph)
				|| Receipt->BeforeScript.Get() != Receipt->BeforeProperties.Script)
			{
				return ErrorResult(TEXT("The retained event-handler nodes, pins, or script changed; rollback left the current graph and handler list unchanged."), TEXT("rollback_snapshot_invalid"), 409);
			}
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Event Handler Removal")));
			System->Modify(); Emitter->Modify(); Graph->Modify();
			const bool bGraphCreated = RestoreGraphSnapshot(Graph, BeforeGraph);
			if (!bGraphCreated || !HasEventGraph(Graph, Receipt->UsageId))
			{
				return ErrorResult(TEXT("The original event-handler graph was not restored completely; the transaction was retained for Editor Undo."), TEXT("rollback_verification_failed"), 500);
			}
			Emitter->AddEventHandler(Receipt->BeforeProperties, Receipt->EmitterVersion);
			Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			const FNiagaraEventScriptProperties* Restored = FindHandler(Data, Receipt->UsageId);
			const FCompileSummary Compile = CompileHandler(System, Restored ? Restored->Script : nullptr);
			if (!bGraphCreated || !HasEventGraph(Graph, Receipt->UsageId) || !Restored || !Compile.bCompiled)
			{
				return ErrorResult(FString::Printf(TEXT("Event-handler removal rollback verification failed (graphRestored=%s graphPresent=%s handlerPresent=%s compiled=%s exportChars=%d nodes=%d)."),
					bGraphCreated ? TEXT("true") : TEXT("false"),
					HasEventGraph(Graph, Receipt->UsageId) ? TEXT("true") : TEXT("false"),
					Restored ? TEXT("true") : TEXT("false"),
					Compile.bCompiled ? TEXT("true") : TEXT("false"), BeforeGraph.Export.Len(), BeforeGraph.NodeCount), TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty(); Receipt->bRolledBack = true; Receipt->bCompiled = Compile.bCompiled; Receipt->CompileStatus = Compile.Status;
			Receipt->BeforeGraphSnapshot.Reset();
			Receipt->BeforeScript.Reset();
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
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerList>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerAddPlan>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerAddApply>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerAddRollback>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerUpdatePlan>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerUpdateApply>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerUpdateRollback>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerRemovePlan>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerRemoveApply>());
		Registry.Register(MakeShared<FTool_NiagaraEventHandlerRemoveRollback>());
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
			TEXT("content.niagara.event_handler.list")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.add.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.add.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.add.rollback")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.update.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.update.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.update.rollback")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.remove.plan")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.remove.apply")));
		Registry.Register(MakeShared<FUnavailableNiagaraEventHandler>(
			TEXT("content.niagara.event_handler.remove.rollback")));
#endif
	}
}
