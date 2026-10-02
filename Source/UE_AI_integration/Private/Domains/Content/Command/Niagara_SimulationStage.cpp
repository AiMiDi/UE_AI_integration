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
#include "NiagaraSimulationStageBase.h"
#include "NiagaraParameterBinding.h"
#include "NiagaraScriptBase.h"
#include "NiagaraSystem.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UEAINiagaraSimulationStagePrivate
{
	using UEAIIntegration::Infrastructure::TryDigestJson;
	using UEAIIntegration::Infrastructure::ValidateChangeApproval;

	constexpr int32 MaxPathCharacters = 2048;
	constexpr int32 MaxEmitterSelectorCharacters = 512;
	constexpr int32 MaxStageNameCharacters = 128;
	constexpr int32 MaxStageIndex = 128;
	const TCHAR* GenericStageClassPath = TEXT("/Script/Niagara.NiagaraSimulationStageGeneric");

#if WITH_DEV_AUTOMATION_TESTS
	static bool bFailNextUpdateAfterPropertiesForTests = false;
	void SetUpdatePropertyFailureForTests(bool bEnabled)
	{
		check(IsInGameThread());
		bFailNextUpdateAfterPropertiesForTests = bEnabled;
	}
#endif

	struct FStageRequest
	{
		FString SystemPath;
		FString EmitterSelector;
		FGuid UsageId;
		FString StageClassPath = GenericStageClassPath;
		FString StageName;
		bool bEnabled = true;
		int32 TargetIndex = INDEX_NONE;
		TSharedPtr<FJsonObject> Properties;
		TOptional<int32> NumIterations;
		FString NumIterationsBinding;
		bool bHasNumIterationsBinding = false;
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

	struct FGraphSnapshot;

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
		TSharedPtr<FGraphSnapshot> BeforeGraphSnapshot;
		FString GraphAfterDigest;
		FString EmitterChangeIdAfter;
		FGuid EmitterVersion;
		FGuid UsageId;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraEmitter> Emitter;
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TWeakObjectPtr<UNiagaraSimulationStageBase> Stage;
		FString Operation = TEXT("add");
		FName BeforeStageName;
		bool bBeforeEnabled = true;
		int32 BeforeIndex = INDEX_NONE;
		TSharedPtr<FJsonObject> BeforeProperties;
		FNiagaraParameterBindingWithValue BeforeNumIterations;
		bool bHasBeforeNumIterations = false;
		bool bHasBeforeState = false;
		bool bChanged = false;
		bool bCompiled = false;
		FString CompileStatus = TEXT("notRequired");
		bool bRolledBack = false;
		bool bFullGraphRestored = false;
	};

	struct FGraphSnapshot
	{
		struct FNodeState
		{
			TStrongObjectPtr<UEdGraphNode> Node;
			TArray<UEdGraphPin*> Pins;
			FGuid NiagaraChangeId;
			bool bHasNiagaraChangeId = false;
		};
		struct FPinState
		{
			UEdGraphPin* Pin = nullptr;
			TArray<UEdGraphPin*> Links;
			FString DefaultValue;
			TStrongObjectPtr<UObject> DefaultObject;
			FText DefaultTextValue;
		};
		TWeakObjectPtr<UNiagaraGraph> Graph;
		TStrongObjectPtr<UNiagaraSimulationStageBase> RetainedStage;
		TArray<FNodeState> Nodes;
		TArray<FPinState> Pins;
		FString Export;
		FString Digest;
		int32 NodeCount = 0;
		bool bCaptured = false;
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
		TSet<UEdGraphPin*> Pins;
		OutSnapshot.Graph = Graph;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node) || Node->GetGraph() != Graph || Nodes.Contains(Node))
			{
				return false;
			}
			Nodes.Add(Node);
			FGraphSnapshot::FNodeState& NodeState = OutSnapshot.Nodes.AddDefaulted_GetRef();
			NodeState.Node.Reset(Node);
			NodeState.Pins = Node->Pins;
			if (const UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node))
			{
				NodeState.NiagaraChangeId = NiagaraNode->GetChangeId();
				NodeState.bHasNiagaraChangeId = true;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->GetOwningNode() != Node || Pins.Contains(Pin))
				{
					return false;
				}
				Pins.Add(Pin);
				FGraphSnapshot::FPinState& PinState = OutSnapshot.Pins.AddDefaulted_GetRef();
				PinState.Pin = Pin;
				PinState.Links = Pin->LinkedTo;
				PinState.DefaultValue = Pin->DefaultValue;
				PinState.DefaultObject.Reset(Pin->DefaultObject.Get());
				PinState.DefaultTextValue = Pin->DefaultTextValue;
			}
		}
		for (const FGraphSnapshot::FPinState& State : OutSnapshot.Pins)
		{
			for (UEdGraphPin* LinkedPin : State.Links)
			{
				if (!Pins.Contains(LinkedPin) || !LinkedPin->LinkedTo.Contains(State.Pin))
				{
					return false;
				}
			}
		}
		FEdGraphUtilities::ExportNodesToText(Nodes, OutSnapshot.Export);
		if (!Nodes.IsEmpty() && OutSnapshot.Export.IsEmpty())
		{
			return false;
		}
		OutSnapshot.Digest = GraphSnapshotDigest(OutSnapshot.Export);
		OutSnapshot.NodeCount = Nodes.Num();
		OutSnapshot.bCaptured = true;
		return true;
	}

	bool SnapshotPinsSurvive(UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot)
	{
		if (!Graph || !Snapshot.bCaptured || Snapshot.Graph.Get() != Graph
			|| Snapshot.Nodes.Num() != Snapshot.NodeCount)
		{
			return false;
		}
		// Strong UObject references retain removed nodes, but not reconstructed
		// UEdGraphPins. Check membership before dereferencing any saved pin.
		for (const FGraphSnapshot::FNodeState& State : Snapshot.Nodes)
		{
			if (!IsValid(State.Node.Get()) || State.Node->GetGraph() != Graph || State.Node->Pins != State.Pins)
			{
				return false;
			}
		}
		return true;
	}

	// A removal snapshot intentionally retains the removed nodes after they have
	// been taken out of Graph->Nodes. Their UObject outer and pin arrays remain
	// valid, so they can be reattached during rollback. Keep this check separate
	// from SnapshotPinsSurvive, which is the stricter post-restore membership
	// check used by GraphSnapshotMatches.
	bool SnapshotNodesRetained(const UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot)
	{
		if (!Graph || !Snapshot.bCaptured || Snapshot.Graph.Get() != Graph
			|| Snapshot.Nodes.Num() != Snapshot.NodeCount)
		{
			return false;
		}
		for (const FGraphSnapshot::FNodeState& State : Snapshot.Nodes)
		{
			if (!IsValid(State.Node.Get()) || State.Node->GetOuter() != Graph
				|| State.Node->Pins != State.Pins)
			{
				return false;
			}
		}
		return true;
	}

	bool GraphSnapshotMatches(UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot)
	{
		if (!SnapshotPinsSurvive(Graph, Snapshot) || Graph->Nodes.Num() != Snapshot.NodeCount)
		{
			return false;
		}
		for (int32 Index = 0; Index < Snapshot.Nodes.Num(); ++Index)
		{
			if (Graph->Nodes[Index] != Snapshot.Nodes[Index].Node.Get())
			{
				return false;
			}
		}
		FGraphSnapshot ReadBack;
		return CaptureGraphSnapshot(Graph, ReadBack) && ReadBack.Digest == Snapshot.Digest
			&& ReadBack.NodeCount == Snapshot.NodeCount;
	}

	void ApplyRetainedGraphSnapshot(UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot)
	{
		Graph->Modify();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			Node->Modify();
			Node->BreakAllNodeLinks();
		}
		Graph->Nodes.Reset(Snapshot.NodeCount);
		for (const FGraphSnapshot::FNodeState& State : Snapshot.Nodes)
		{
			UEdGraphNode* Node = State.Node.Get();
			Node->Modify();
			Node->BreakAllNodeLinks();
			// AddNode keeps the graph membership notification and the graph's
			// ownership invariant together. Directly appending to Nodes can leave a
			// retained node invisible to graph listeners during rollback.
			Graph->AddNode(Node, false, false);
		}
		for (const FGraphSnapshot::FPinState& State : Snapshot.Pins)
		{
			State.Pin->DefaultValue = State.DefaultValue;
			State.Pin->DefaultObject = State.DefaultObject.Get();
			State.Pin->DefaultTextValue = State.DefaultTextValue;
			for (UEdGraphPin* Link : State.Links)
			{
				State.Pin->MakeLinkTo(Link);
			}
		}
		// Reconnecting a Niagara pin notifies the node and assigns a fresh
		// synchronization id.  That id is authored graph state (and is part of
		// the serialized node), so restore it after all links have been rebuilt.
		for (const FGraphSnapshot::FNodeState& State : Snapshot.Nodes)
		{
			if (State.bHasNiagaraChangeId)
			{
				if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(State.Node.Get()))
				{
					NiagaraNode->ForceChangeId(State.NiagaraChangeId, false);
				}
			}
		}
		UEAIIntegration::NiagaraEditing::NotifyRestoredGraph(Graph);
	}

	bool RestoreGraphSnapshot(UNiagaraGraph* Graph, const FGraphSnapshot& Snapshot, FString* OutFailure = nullptr)
	{
		auto Fail = [OutFailure](const TCHAR* Reason)
		{
			if (OutFailure)
			{
				*OutFailure = Reason;
			}
			return false;
		};
		// A snapshot can describe either a graph that is already attached (the
		// update failure path) or nodes detached by RemoveNode (the remove
		// rollback path). Prefer the stricter attached check, then accept only the
		// retained UObject/pin identity required for detached restoration.
		const bool bSnapshotAlreadyAttached = SnapshotPinsSurvive(Graph, Snapshot);
		if (!bSnapshotAlreadyAttached && !SnapshotNodesRetained(Graph, Snapshot))
		{
			return Fail(TEXT("snapshot_nodes_not_retained"));
		}
		FGraphSnapshot CurrentGraph;
		if (!CaptureGraphSnapshot(Graph, CurrentGraph))
		{
			return Fail(TEXT("current_graph_snapshot_failed"));
		}
		// Removal only changes membership and links. An unexpected new node is
		// graph drift, not permission to replace the emitter's other graphs.
		for (const FGraphSnapshot::FNodeState& State : CurrentGraph.Nodes)
		{
			if (!Snapshot.Nodes.ContainsByPredicate([&State](const FGraphSnapshot::FNodeState& Before)
				{ return Before.Node.Get() == State.Node.Get(); }))
			{
				return Fail(TEXT("current_graph_contains_unexpected_node"));
			}
		}
		ApplyRetainedGraphSnapshot(Graph, Snapshot);
		if (GraphSnapshotMatches(Graph, Snapshot))
		{
			return true;
		}
		// A node property or listener may prevent full restoration. Put the
		// current graph back, return failure, and retain the transaction for Undo.
		if (SnapshotPinsSurvive(Graph, CurrentGraph))
		{
			ApplyRetainedGraphSnapshot(Graph, CurrentGraph);
		}
		return Fail(TEXT("restored_graph_verification_failed"));
	}

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

	const TSet<FName>& EditableGenericStageProperties()
	{
		static const TSet<FName> Names = {
			TEXT("IterationSource"),
			TEXT("ExecuteBehavior"),
			TEXT("bDisablePartialParticleUpdate"),
			TEXT("bParticleIterationStateEnabled"),
			TEXT("ParticleIterationStateBinding"),
			TEXT("ParticleIterationStateRange"),
			TEXT("bGpuDispatchForceLinear"),
			TEXT("bOverrideGpuDispatchNumThreads"),
			TEXT("DirectDispatchType"),
			TEXT("DirectDispatchElementType"),
			TEXT("OverrideGpuDispatchNumThreadsX"),
			TEXT("OverrideGpuDispatchNumThreadsY"),
			TEXT("OverrideGpuDispatchNumThreadsZ"),
			TEXT("ElementCountX"),
			TEXT("ElementCountY"),
			TEXT("ElementCountZ"),
			TEXT("DataInterface")
		};
		return Names;
	}

	FName ResolveGenericStagePropertyName(const FString& JsonName)
	{
		static const TMap<FString, FName> Names = {
			{TEXT("iterationSource"), TEXT("IterationSource")},
			{TEXT("executeBehavior"), TEXT("ExecuteBehavior")},
			{TEXT("disablePartialParticleUpdate"), TEXT("bDisablePartialParticleUpdate")},
			{TEXT("particleIterationStateEnabled"), TEXT("bParticleIterationStateEnabled")},
			{TEXT("particleIterationStateBinding"), TEXT("ParticleIterationStateBinding")},
			{TEXT("particleIterationStateRange"), TEXT("ParticleIterationStateRange")},
			{TEXT("gpuDispatchForceLinear"), TEXT("bGpuDispatchForceLinear")},
			{TEXT("overrideGpuDispatchNumThreads"), TEXT("bOverrideGpuDispatchNumThreads")},
			{TEXT("directDispatchType"), TEXT("DirectDispatchType")},
			{TEXT("directDispatchElementType"), TEXT("DirectDispatchElementType")},
			{TEXT("overrideGpuDispatchNumThreadsX"), TEXT("OverrideGpuDispatchNumThreadsX")},
			{TEXT("overrideGpuDispatchNumThreadsY"), TEXT("OverrideGpuDispatchNumThreadsY")},
			{TEXT("overrideGpuDispatchNumThreadsZ"), TEXT("OverrideGpuDispatchNumThreadsZ")},
			{TEXT("elementCountX"), TEXT("ElementCountX")},
			{TEXT("elementCountY"), TEXT("ElementCountY")},
			{TEXT("elementCountZ"), TEXT("ElementCountZ")},
			{TEXT("dataInterface"), TEXT("DataInterface")}
		};
		if (const FName* Name = Names.Find(JsonName)) return *Name;
		return NAME_None;
	}

	FString GenericStagePropertyJsonKey(const FName& PropertyName)
	{
		for (const TPair<FString, FName>& Pair : {
			TPair<FString, FName>(TEXT("iterationSource"), TEXT("IterationSource")),
			TPair<FString, FName>(TEXT("executeBehavior"), TEXT("ExecuteBehavior")),
			TPair<FString, FName>(TEXT("disablePartialParticleUpdate"), TEXT("bDisablePartialParticleUpdate")),
			TPair<FString, FName>(TEXT("particleIterationStateEnabled"), TEXT("bParticleIterationStateEnabled")),
			TPair<FString, FName>(TEXT("particleIterationStateBinding"), TEXT("ParticleIterationStateBinding")),
			TPair<FString, FName>(TEXT("particleIterationStateRange"), TEXT("ParticleIterationStateRange")),
			TPair<FString, FName>(TEXT("gpuDispatchForceLinear"), TEXT("bGpuDispatchForceLinear")),
			TPair<FString, FName>(TEXT("overrideGpuDispatchNumThreads"), TEXT("bOverrideGpuDispatchNumThreads")),
			TPair<FString, FName>(TEXT("directDispatchType"), TEXT("DirectDispatchType")),
			TPair<FString, FName>(TEXT("directDispatchElementType"), TEXT("DirectDispatchElementType")),
			TPair<FString, FName>(TEXT("overrideGpuDispatchNumThreadsX"), TEXT("OverrideGpuDispatchNumThreadsX")),
			TPair<FString, FName>(TEXT("overrideGpuDispatchNumThreadsY"), TEXT("OverrideGpuDispatchNumThreadsY")),
			TPair<FString, FName>(TEXT("overrideGpuDispatchNumThreadsZ"), TEXT("OverrideGpuDispatchNumThreadsZ")),
			TPair<FString, FName>(TEXT("elementCountX"), TEXT("ElementCountX")),
			TPair<FString, FName>(TEXT("elementCountY"), TEXT("ElementCountY")),
			TPair<FString, FName>(TEXT("elementCountZ"), TEXT("ElementCountZ")),
			TPair<FString, FName>(TEXT("dataInterface"), TEXT("DataInterface"))})
		{
			if (Pair.Value == PropertyName) return Pair.Key;
		}
		return PropertyName.ToString();
	}

	bool CaptureGenericStageProperties(
		UNiagaraSimulationStageBase* Stage,
		TSharedPtr<FJsonObject>& OutProperties)
	{
		OutProperties.Reset();
		UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
		if (!Generic)
		{
			return true;
		}
		OutProperties = MakeShared<FJsonObject>();
		for (const FName& Name : EditableGenericStageProperties())
		{
			FProperty* Property = Generic->GetClass()->FindPropertyByName(Name);
			if (!Property)
			{
				return false;
			}
			void* Value = Property->ContainerPtrToValuePtr<void>(Generic);
			TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(Property, Value);
			if (!JsonValue.IsValid())
			{
				return false;
			}
			OutProperties->SetField(GenericStagePropertyJsonKey(Name), JsonValue);
		}
		return true;
	}

	bool GenericStagePropertiesMatch(
		UNiagaraSimulationStageBase* Stage,
		const TSharedPtr<FJsonObject>& Expected)
	{
		if (!Expected.IsValid())
		{
			return true;
		}
		TSharedPtr<FJsonObject> Actual;
		if (!CaptureGenericStageProperties(Stage, Actual) || !Actual.IsValid())
		{
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Expected->Values)
		{
			const TSharedPtr<FJsonValue>* ActualValue = Actual->Values.Find(Entry.Key);
			if (!ActualValue || !Entry.Value.IsValid() || !(*ActualValue).IsValid())
			{
				return false;
			}
			TSharedRef<FJsonObject> ExpectedObject = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> ActualObject = MakeShared<FJsonObject>();
			ExpectedObject->SetField(TEXT("value"), Entry.Value);
			ActualObject->SetField(TEXT("value"), *ActualValue);
			FString ExpectedDigest;
			FString ActualDigest;
			if (!TryDigestJson(ExpectedObject, ExpectedDigest) || !TryDigestJson(ActualObject, ActualDigest)
				|| ExpectedDigest != ActualDigest)
			{
				return false;
			}
		}
		return true;
	}

	bool ApplyGenericStageProperties(
		UNiagaraSimulationStageBase* Stage,
		const TSharedPtr<FJsonObject>& Properties,
		FString& OutError)
	{
		if (!Properties.IsValid())
		{
			return true;
		}
		UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
		if (!Generic)
		{
			OutError = TEXT("properties are only supported for NiagaraSimulationStageGeneric.");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Properties->Values)
		{
			const FName PropertyName = ResolveGenericStagePropertyName(Entry.Key);
			if (!EditableGenericStageProperties().Contains(PropertyName))
			{
				OutError = FString::Printf(TEXT("Simulation-stage property '%s' is not editable."), *Entry.Key);
				return false;
			}
			FProperty* Property = Generic->GetClass()->FindPropertyByName(PropertyName);
			if (!Property || !Entry.Value.IsValid())
			{
				OutError = FString::Printf(TEXT("Simulation-stage property '%s' is invalid."), *Entry.Key);
				return false;
			}
			void* Value = Property->ContainerPtrToValuePtr<void>(Generic);
			if (!FJsonObjectConverter::JsonValueToUProperty(Entry.Value, Property, Value, 0, 0))
			{
				OutError = FString::Printf(TEXT("Simulation-stage property '%s' has an invalid value."), *Entry.Key);
				return false;
			}
		}
		return true;
	}

	bool ApplyStageRequestProperties(
		UNiagaraSimulationStageBase* Stage,
		const FStageRequest& Request,
		FString& OutError)
	{
		if (!ApplyGenericStageProperties(Stage, Request.Properties, OutError))
		{
			return false;
		}
		UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
		if (!Generic)
		{
			if (Request.NumIterations.IsSet() || Request.bHasNumIterationsBinding)
			{
				OutError = TEXT("numIterations and numIterationsBinding require NiagaraSimulationStageGeneric.");
				return false;
			}
			return true;
		}
		if (Request.NumIterations.IsSet() || Request.bHasNumIterationsBinding)
		{
			int32 DefaultIterations = 1;
			if (Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
			{
				DefaultIterations = Generic->NumIterations.GetDefaultValue<int32>();
			}
			if (Request.NumIterations.IsSet())
			{
				DefaultIterations = Request.NumIterations.GetValue();
			}
			const FNiagaraTypeDefinition IntType = FNiagaraTypeDefinition::GetIntDef();
			const FName BindingName = Request.bHasNumIterationsBinding
				? FName(*Request.NumIterationsBinding)
				: Generic->NumIterations.ResolvedParameter.GetName();
			if (BindingName.IsNone())
			{
				Generic->NumIterations.SetDefaultParameter(IntType, DefaultIterations);
			}
			else
			{
				Generic->NumIterations.SetDefaultParameter(BindingName, IntType, DefaultIterations);
			}
		}
		return true;
	}

	bool RestoreCapturedStageProperties(
		UNiagaraSimulationStageBase* Stage,
		const TSharedPtr<FJsonObject>& Properties,
		const FNiagaraParameterBindingWithValue* BeforeNumIterations,
		bool bHasBeforeNumIterations,
		FString& OutError)
	{
		if (!ApplyGenericStageProperties(Stage, Properties, OutError))
		{
			return false;
		}
		if (bHasBeforeNumIterations)
		{
			UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
			if (!Generic || !BeforeNumIterations)
			{
				OutError = TEXT("The captured NumIterations state is incompatible with the current simulation stage class.");
				return false;
			}
			Generic->NumIterations = *BeforeNumIterations;
		}
		return true;
	}

	bool ParseGenericStageProperties(
		const TSharedPtr<FJsonObject>& Params,
		FStageRequest& Request,
		FString& OutCode,
		FString& OutError)
	{
		if (Params->HasField(TEXT("properties")))
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Params->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->IsValid())
			{
				OutCode = TEXT("properties_invalid");
				OutError = TEXT("properties must be an object of supported Generic Simulation Stage fields.");
				return false;
			}
			Request.Properties = *Properties;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Request.Properties->Values)
			{
				if (!EditableGenericStageProperties().Contains(ResolveGenericStagePropertyName(Entry.Key)))
				{
					OutCode = TEXT("property_unsupported");
					OutError = FString::Printf(TEXT("Simulation-stage property '%s' is not editable."), *Entry.Key);
					return false;
				}
			}
		}
		if (Params->HasField(TEXT("numIterations")))
		{
			double Number = 0.0;
			if (!Params->TryGetNumberField(TEXT("numIterations"), Number)
				|| !FMath::IsFinite(Number) || Number < 0.0 || Number > 65535.0
				|| Number != FMath::FloorToDouble(Number))
			{
				OutCode = TEXT("num_iterations_invalid");
				OutError = TEXT("numIterations must be an integer from 0 to 65535.");
				return false;
			}
			Request.NumIterations = static_cast<int32>(Number);
		}
		if (Params->HasField(TEXT("numIterationsBinding")))
		{
			if (!Params->TryGetStringField(TEXT("numIterationsBinding"), Request.NumIterationsBinding)
				|| Request.NumIterationsBinding.Len() > MaxPathCharacters)
			{
				OutCode = TEXT("num_iterations_binding_invalid");
				OutError = TEXT("numIterationsBinding must be a bounded parameter name.");
				return false;
			}
			Request.bHasNumIterationsBinding = true;
		}
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
		if (!ParseGenericStageProperties(Params, OutRequest, OutCode, OutError))
		{
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
		// The generic stage class is part of the loaded Niagara module, while a
		// HostProject may not have a reflected object path for it in its asset
		// registry.  Prefer the native class identity for the default and retain
		// path loading for explicitly requested custom stage classes.
		if (ClassPath.IsEmpty() || ClassPath == GenericStageClassPath)
		{
			return UNiagaraSimulationStageGeneric::StaticClass();
		}
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
			UEdGraphPin* ExistingInput = FindParameterMapPin(Existing, EGPD_Input);
			if (ExistingInput && ExistingInput->LinkedTo.Num() == 1)
			{
				return Existing;
			}
			Existing->Modify();
			Existing->DestroyNode();
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
		OutputInput->BreakAllPinLinks();
		OutputInput->MakeLinkTo(InputOutput);
		Graph->NotifyGraphChanged();
		return Output;
	}

	bool HasStageOutput(UNiagaraGraph* Graph, const FGuid& UsageId)
	{
		return Graph && Graph->FindEquivalentOutputNode(ENiagaraScriptUsage::ParticleSimulationStageScript, UsageId) != nullptr;
	}

	void CollectUpstreamNodes(UEdGraphNode* Node, UNiagaraGraph* Graph, TSet<UEdGraphNode*>& OutNodes)
	{
		if (!Node || !Graph->Nodes.Contains(Node) || OutNodes.Contains(Node))
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
				CollectUpstreamNodes(LinkedPin ? LinkedPin->GetOwningNode() : nullptr, Graph, OutNodes);
			}
		}
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
		TSet<UEdGraphNode*> Candidates;
		CollectUpstreamNodes(Output, Graph, Candidates);
		TSet<UEdGraphNode*> ProtectedNodes;
		for (UEdGraphNode* Node : Candidates)
		{
			if (Node == Output)
			{
				continue;
			}
			bool bShared = Cast<UNiagaraNodeOutput>(Node) != nullptr;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					bShared |= LinkedPin && !Candidates.Contains(LinkedPin->GetOwningNode());
				}
			}
			if (bShared)
			{
				// Preserve shared consumers even when their branch is not connected
				// to another output, and preserve all dependencies of that branch.
				CollectUpstreamNodes(Node, Graph, ProtectedNodes);
			}
		}
		const TArray<UEdGraphNode*> OriginalNodes = Graph->Nodes;
		for (UEdGraphNode* Node : OriginalNodes)
		{
			if (Candidates.Contains(Node) && !ProtectedNodes.Contains(Node))
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

	int32 FindStageIndex(FVersionedNiagaraEmitterData* Data, const UNiagaraSimulationStageBase* Stage)
	{
		if (!Data || !Stage)
		{
			return INDEX_NONE;
		}
		const TArray<UNiagaraSimulationStageBase*>& Stages = Data->GetSimulationStages();
		for (int32 Index = 0; Index < Stages.Num(); ++Index)
		{
			if (Stages[Index] == Stage)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	bool ReadBackMatches(const FStageTarget& Target, const FStageRequest& Request)
	{
		UNiagaraSimulationStageBase* Stage = FindStage(Target.Data, Request.UsageId);
		if (!Stage || !Stage->Script || Stage->Script->GetUsage() != ENiagaraScriptUsage::ParticleSimulationStageScript)
		{
			return false;
		}
		if (Stage->SimulationStageName != FName(*Request.StageName)
			|| Stage->bEnabled != Request.bEnabled
			|| Stage->GetClass()->GetPathName() != Request.StageClassPath
			|| (Request.TargetIndex != INDEX_NONE && FindStageIndex(Target.Data, Stage) != Request.TargetIndex)
			|| !HasStageOutput(Target.Graph, Request.UsageId))
		{
			return false;
		}
		const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
		if (Request.NumIterations.IsSet()
			&& (!Generic || Generic->NumIterations.GetDefaultValueArray().Num() != sizeof(int32)
				|| Generic->NumIterations.GetDefaultValue<int32>() != Request.NumIterations.GetValue()))
		{
			return false;
		}
		if (Request.bHasNumIterationsBinding
			&& (!Generic || Generic->NumIterations.ResolvedParameter.GetName() != FName(*Request.NumIterationsBinding)))
		{
			return false;
		}
		if (!GenericStagePropertiesMatch(const_cast<UNiagaraSimulationStageBase*>(Stage), Request.Properties))
		{
			return false;
		}
		return true;
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
		if (Request.Properties.IsValid()) Plan->SetObjectField(TEXT("properties"), Request.Properties);
		if (Request.NumIterations.IsSet()) Plan->SetNumberField(TEXT("numIterations"), Request.NumIterations.GetValue());
		if (Request.bHasNumIterationsBinding) Plan->SetStringField(TEXT("numIterationsBinding"), Request.NumIterationsBinding);
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
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-simulation-stage.v1"));
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
		Result->SetStringField(TEXT("action"), Receipt.Operation);
		Result->SetStringField(TEXT("usageId"), Receipt.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetBoolField(TEXT("changed"), Receipt.bChanged);
		Result->SetBoolField(TEXT("verified"), true);
		Result->SetBoolField(TEXT("saved"), false);
		Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
		Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
		Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
		Result->SetBoolField(TEXT("fullGraphRestored"), Receipt.bFullGraphRestored);
		Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
		if (Receipt.BeforeProperties.IsValid())
		{
			Result->SetObjectField(TEXT("beforeProperties"), Receipt.BeforeProperties);
		}
		if (Receipt.bHasBeforeNumIterations
			&& Receipt.BeforeNumIterations.GetDefaultValueArray().Num() == sizeof(int32))
		{
			Result->SetNumberField(TEXT("beforeNumIterations"), Receipt.BeforeNumIterations.GetDefaultValue<int32>());
			Result->SetStringField(TEXT("beforeNumIterationsBinding"), Receipt.BeforeNumIterations.ResolvedParameter.GetName().ToString());
		}
		if (const UNiagaraSimulationStageBase* Stage = Receipt.Stage.Get())
		{
			TSharedPtr<FJsonObject> CurrentProperties;
			if (CaptureGenericStageProperties(const_cast<UNiagaraSimulationStageBase*>(Stage), CurrentProperties)
				&& CurrentProperties.IsValid())
			{
				Result->SetObjectField(TEXT("properties"), CurrentProperties);
			}
			if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
			{
				if (Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
				{
					Result->SetNumberField(TEXT("numIterations"), Generic->NumIterations.GetDefaultValue<int32>());
				}
				Result->SetStringField(TEXT("numIterationsBinding"), Generic->NumIterations.ResolvedParameter.GetName().ToString());
			}
		}
		Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Result->SetStringField(
			TEXT("scope"), TEXT("authored simulation stage; GPU execution and runtime behavior unverified"));
		return Result;
	}

	bool BuildExistingPlanData(
		const TSharedPtr<FJsonObject>& Params,
		FStagePlanData& OutData,
		FString& OutCode,
		FString& OutError)
	{
		OutData = FStagePlanData();
		if (!ParseRequest(Params, OutData.Request, OutCode, OutError)
			|| !ResolveTarget(OutData.Request, OutData.Target, OutCode, OutError))
		{
			return false;
		}
		UNiagaraSimulationStageBase* Existing = FindStage(OutData.Target.Data, OutData.Request.UsageId);
		if (!Existing || !Existing->Script || !HasStageOutput(OutData.Target.Graph, OutData.Request.UsageId))
		{
			OutCode = TEXT("stage_not_found");
			OutError = TEXT("The requested simulation stage does not exist with a matching graph output.");
			return false;
		}
		if (Params->HasField(TEXT("stageClass"))
			&& OutData.Request.StageClassPath != Existing->GetClass()->GetPathName())
		{
			OutCode = TEXT("stage_class_immutable");
			OutError = TEXT("stageClass identifies the existing UObject class and cannot be changed in place.");
			return false;
		}
		if (!Params->HasField(TEXT("stageClass"))) OutData.Request.StageClassPath = Existing->GetClass()->GetPathName();
		if (!Params->HasField(TEXT("name"))) OutData.Request.StageName = Existing->SimulationStageName.ToString();
		if (!Params->HasField(TEXT("enabled"))) OutData.Request.bEnabled = Existing->bEnabled;
		if (!Params->HasField(TEXT("index"))) OutData.Request.TargetIndex = FindStageIndex(OutData.Target.Data, Existing);
		const UPackage* Package = OutData.Target.System->GetOutermost();
		OutData.bBlocked = !Package || !Package->GetName().StartsWith(TEXT("/Game/"))
			|| OutData.Target.Emitter->GetOutermost() != Package;
		if (OutData.bBlocked) OutData.Risks.Add(TEXT("The selected System or emitter is not an owned non-transient /Game/ asset."));
		OutData.Warnings.Add(TEXT("The operation changes authored Simulation Stage configuration and does not prove GPU execution or runtime particle behavior."));
		return true;
	}

	TSharedRef<FJsonObject> BuildExistingPlanJson(const FStagePlanData& Data, const TCHAR* Action)
	{
		TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
		Plan->SetStringField(TEXT("planKind"), FString::Printf(TEXT("niagaraSimulationStage%s"), Action));
		Plan->SetStringField(TEXT("action"), FString::Printf(TEXT("%sSimulationStage"), Action));
		TSharedPtr<FJsonObject> Preconditions = Plan->GetObjectField(TEXT("preconditions"));
		if (Preconditions.IsValid()) Preconditions->SetBoolField(TEXT("stagePresent"), true);
		TSharedPtr<FJsonObject> Before = Plan->GetObjectField(TEXT("before"));
		TSharedPtr<FJsonObject> After = Plan->GetObjectField(TEXT("after"));
		if (Before.IsValid()) Before->SetBoolField(TEXT("stagePresent"), true);
		if (After.IsValid()) After->SetBoolField(TEXT("stagePresent"), !FString(Action).Equals(TEXT("Remove")));
		if (UNiagaraSimulationStageBase* Existing = FindStage(Data.Target.Data, Data.Request.UsageId))
		{
			if (Before.IsValid())
			{
				Before->SetStringField(TEXT("stageClass"), Existing->GetClass()->GetPathName());
				Before->SetStringField(TEXT("name"), Existing->SimulationStageName.ToString());
				Before->SetBoolField(TEXT("enabled"), Existing->bEnabled);
				Before->SetNumberField(TEXT("index"), FindStageIndex(Data.Target.Data, Existing));
				TSharedPtr<FJsonObject> ExistingProperties;
				if (CaptureGenericStageProperties(Existing, ExistingProperties) && ExistingProperties.IsValid())
				{
					Before->SetObjectField(TEXT("properties"), ExistingProperties);
				}
				if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Existing))
				{
					if (Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
					{
						Before->SetNumberField(TEXT("numIterations"), Generic->NumIterations.GetDefaultValue<int32>());
					}
					Before->SetStringField(TEXT("numIterationsBinding"), Generic->NumIterations.ResolvedParameter.GetName().ToString());
				}
			}
			if (After.IsValid() && !FString(Action).Equals(TEXT("Remove")))
			{
				After->SetStringField(TEXT("stageClass"), Data.Request.StageClassPath);
				After->SetStringField(TEXT("name"), Data.Request.StageName);
				After->SetBoolField(TEXT("enabled"), Data.Request.bEnabled);
				After->SetNumberField(TEXT("index"), Data.Request.TargetIndex);
				if (Data.Request.Properties.IsValid())
				{
					After->SetObjectField(TEXT("properties"), Data.Request.Properties);
				}
				if (Data.Request.NumIterations.IsSet())
				{
					After->SetNumberField(TEXT("numIterations"), Data.Request.NumIterations.GetValue());
				}
				if (Data.Request.bHasNumIterationsBinding)
				{
					After->SetStringField(TEXT("numIterationsBinding"), Data.Request.NumIterationsBinding);
				}
			}
		}
		return Plan;
	}

	class FTool_NiagaraSimulationStageList final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.list"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStageRequest Request; FString Code, Error;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), Request.SystemPath) || Request.SystemPath.TrimStartAndEnd().IsEmpty()) return ErrorResult(TEXT("system is required."), TEXT("system_required"));
			if (!Params->TryGetStringField(TEXT("emitter"), Request.EmitterSelector) || Request.EmitterSelector.TrimStartAndEnd().IsEmpty()) return ErrorResult(TEXT("emitter is required."), TEXT("emitter_required"));
			int32 Offset = 0, Limit = 32;
			if (Params->HasField(TEXT("offset"))) { double N = 0; if (!Params->TryGetNumberField(TEXT("offset"), N) || N < 0 || N > 65536 || FMath::TruncToInt(N) != N) return ErrorResult(TEXT("offset must be an integer from 0 to 65536."), TEXT("page_invalid")); Offset = FMath::TruncToInt(N); }
			if (Params->HasField(TEXT("limit"))) { double N = 0; if (!Params->TryGetNumberField(TEXT("limit"), N) || N < 1 || N > 128 || FMath::TruncToInt(N) != N) return ErrorResult(TEXT("limit must be an integer from 1 to 128."), TEXT("page_invalid")); Limit = FMath::TruncToInt(N); }
			Request.SystemPath = Request.SystemPath.TrimStartAndEnd(); Request.EmitterSelector = Request.EmitterSelector.TrimStartAndEnd();
			FStageTarget Target;
			if (!ResolveTarget(Request, Target, Code, Error)) return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			const TArray<UNiagaraSimulationStageBase*>& Stages = Target.Data->GetSimulationStages();
			const int32 End = FMath::Min(Stages.Num(), Offset + Limit);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 Index = Offset; Index < End; ++Index)
			{
				UNiagaraSimulationStageBase* Stage = Stages[Index]; if (!Stage || !Stage->Script) continue;
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("usageId"), Stage->Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphensLower));
				Row->SetStringField(TEXT("name"), Stage->SimulationStageName.ToString());
				Row->SetBoolField(TEXT("enabled"), Stage->bEnabled);
				Row->SetStringField(TEXT("stageClass"), Stage->GetClass()->GetPathName());
				Row->SetStringField(TEXT("script"), Stage->Script->GetPathName());
				Row->SetBoolField(TEXT("graphPresent"), HasStageOutput(Target.Graph, Stage->Script->GetUsageId()));
				Row->SetNumberField(TEXT("index"), Index);
				if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
				{
					if (Generic->NumIterations.GetDefaultValueArray().Num() == sizeof(int32))
					{
						Row->SetNumberField(TEXT("numIterations"), Generic->NumIterations.GetDefaultValue<int32>());
					}
					Row->SetStringField(TEXT("numIterationsBinding"), Generic->NumIterations.ResolvedParameter.GetName().ToString());
					TSharedPtr<FJsonObject> Properties;
					if (CaptureGenericStageProperties(Stage, Properties) && Properties.IsValid())
					{
						Row->SetObjectField(TEXT("properties"), Properties);
					}
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.simulation-stages.v1"));
			Result->SetStringField(TEXT("system"), Target.System->GetPathName()); Result->SetStringField(TEXT("emitter"), Target.EmitterName); Result->SetStringField(TEXT("emitterPath"), Target.EmitterPath); Result->SetStringField(TEXT("graph"), Target.GraphPath);
			Result->SetNumberField(TEXT("total"), Stages.Num()); Result->SetNumberField(TEXT("offset"), Offset); Result->SetNumberField(TEXT("limit"), Limit); Result->SetBoolField(TEXT("hasMore"), End < Stages.Num()); Result->SetNumberField(TEXT("nextOffset"), End); Result->SetArrayField(TEXT("simulationStages"), Rows);
			return FMCPToolResult::Ok(Result);
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

	class FTool_NiagaraSimulationStageUpdatePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.update.plan"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStagePlanData Data;
			FString Code;
			FString Error;
			if (!BuildExistingPlanData(Params, Data, Code, Error))
			{
				return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Update"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(TEXT("Unable to compute the simulation-stage update plan digest."), TEXT("digest_unavailable"), 500);
			}
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraSimulationStageUpdateApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.update.apply"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			FString Code;
			FString Error;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			}
			if (const FString* ExistingId = UpdateRequestReceiptIds().Find(RequestId))
			{
				if (FStageReceipt* Existing = Receipts().Find(*ExistingId))
				{
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Error))
					{
						return ErrorResult(Error, Code, 409);
					}
					return FMCPToolResult::Ok(MakeResult(*Existing, true));
				}
				return ErrorResult(TEXT("requestId is associated with an unavailable receipt."), TEXT("request_id_conflict"), 409);
			}
			FStagePlanData Data;
		if (!BuildExistingPlanData(Params, Data, Code, Error))
			{
				return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Update"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(TEXT("Unable to compute the simulation-stage update plan digest."), TEXT("digest_unavailable"), 500);
			}
			if (!ValidateChangeApproval(Params, Digest, Code, Error))
			{
				return ErrorResult(Error, Code, 409);
			}
			if (Data.bBlocked)
			{
				return ErrorResult(TEXT("The simulation-stage target is read-only or not owned by the System package."), TEXT("plan_blocked"), 409);
			}
			if (Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.Target.EmitterChangeId)
			{
				return ErrorResult(TEXT("The Niagara emitter changed after the plan was created; re-plan before applying."), TEXT("plan_digest_mismatch"), 409);
			}
			UNiagaraSimulationStageBase* Stage = FindStage(Data.Target.Data, Data.Request.UsageId);
			if (!Stage || !Stage->Script)
			{
				return ErrorResult(TEXT("The simulation stage disappeared before apply."), TEXT("stage_not_found"), 404);
			}
			FStageReceipt Receipt;
			Receipt.BeforeStageName = Stage->SimulationStageName;
			Receipt.bBeforeEnabled = Stage->bEnabled;
			Receipt.BeforeIndex = FindStageIndex(Data.Target.Data, Stage);
			if (!CaptureGenericStageProperties(Stage, Receipt.BeforeProperties))
			{
				return ErrorResult(TEXT("The simulation-stage properties could not be snapshotted."), TEXT("snapshot_failed"), 500);
			}
			if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
			{
				Receipt.BeforeNumIterations = Generic->NumIterations;
				Receipt.bHasBeforeNumIterations = true;
			}
			Receipt.bHasBeforeState = true;
			TMap<UPackage*, bool> PackageDirtyBefore;
			for (UObject* Object : {static_cast<UObject*>(Data.Target.System), static_cast<UObject*>(Data.Target.Emitter),
				static_cast<UObject*>(Data.Target.Graph), static_cast<UObject*>(Stage), static_cast<UObject*>(Stage->Script)})
			{
				if (Object) PackageDirtyBefore.FindOrAdd(Object->GetOutermost(), Object->GetOutermost()->IsDirty());
			}
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Update Niagara Simulation Stage")));
			Data.Target.System->Modify();
			Data.Target.Emitter->Modify();
			auto RestoreBeforeState = [&](bool bRecompile, FString& RestoreError)
			{
				Stage->SimulationStageName = Receipt.BeforeStageName;
				Stage->bEnabled = Receipt.bBeforeEnabled;
				if (!RestoreCapturedStageProperties(Stage, Receipt.BeforeProperties,
					&Receipt.BeforeNumIterations, Receipt.bHasBeforeNumIterations, RestoreError)) return false;
				if (Receipt.BeforeIndex != INDEX_NONE)
				{
					Data.Target.Emitter->MoveSimulationStageToIndex(Stage, Receipt.BeforeIndex, Data.Target.EmitterVersion);
				}
				const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
				const bool bCompiled = !bRecompile || CompileStage(Data.Target.System, Stage->Script).bCompiled;
				const bool bRestored = bCompiled && FindStage(Data.Target.Data, Data.Request.UsageId) == Stage
					&& Stage->SimulationStageName == Receipt.BeforeStageName && Stage->bEnabled == Receipt.bBeforeEnabled
					&& FindStageIndex(Data.Target.Data, Stage) == Receipt.BeforeIndex
					&& (!Receipt.bHasBeforeNumIterations || (Generic && Generic->NumIterations == Receipt.BeforeNumIterations))
					&& GenericStagePropertiesMatch(Stage, Receipt.BeforeProperties);
				if (!bRestored)
				{
					RestoreError = TEXT("The original simulation-stage properties, order or compilation were not restored completely.");
					return false;
				}
				Transaction.Cancel();
				for (const auto& PackageState : PackageDirtyBefore) PackageState.Key->SetDirtyFlag(PackageState.Value);
				return true;
			};
			auto FailAndRestore = [&](const FString& Message, const TCHAR* Code, int32 Status, bool bRecompile)
			{
				FString RestoreError;
				const bool bRestored = RestoreBeforeState(bRecompile, RestoreError);
				const FString FailureMessage = bRestored
					? Message + TEXT(" The original authored state was restored and verified.")
					: Message + TEXT(" Restoration was not verified; the transaction was retained for Editor Undo. ") + RestoreError;
				FMCPToolResult Failure = ErrorResult(FailureMessage,
					bRestored ? Code : TEXT("restore_failed"), bRestored ? Status : 500);
				Failure.Data = MakeShared<FJsonObject>();
				Failure.Data->SetBoolField(TEXT("restorationVerified"), bRestored);
				return Failure;
			};
			Stage->Modify();
			Stage->SimulationStageName = FName(*Data.Request.StageName);
			Stage->bEnabled = Data.Request.bEnabled;
			bool bPropertiesApplied = ApplyStageRequestProperties(Stage, Data.Request, Error);
#if WITH_DEV_AUTOMATION_TESTS
			if (bFailNextUpdateAfterPropertiesForTests)
			{
				bFailNextUpdateAfterPropertiesForTests = false;
				bPropertiesApplied = false;
				Error = TEXT("Injected property failure after applying the complete requested stage state.");
			}
#endif
			if (!bPropertiesApplied)
			{
				return FailAndRestore(Error, TEXT("property_invalid"), 422, false);
			}
			if (Data.Request.TargetIndex != INDEX_NONE)
			{
				Data.Target.Emitter->MoveSimulationStageToIndex(Stage, Data.Request.TargetIndex, Data.Target.EmitterVersion);
			}
			const FCompileSummary Compile = CompileStage(Data.Target.System, Stage->Script);
			const bool bReadBack = ReadBackMatches(Data.Target, Data.Request);
			if (!bReadBack || !Compile.bCompiled)
			{
				return FailAndRestore(FString::Printf(TEXT("Simulation-stage update read-back or compilation failed (status=%s)."),
					*Compile.Status), !bReadBack ? TEXT("verification_failed") : TEXT("compile_failed"), 500, true);
			}
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = Digest;
			Receipt.SystemPath = Data.Target.System->GetPathName();
			Receipt.EmitterName = Data.Target.EmitterName;
			Receipt.EmitterPath = Data.Target.EmitterPath;
			Receipt.GraphPath = Data.Target.GraphPath;
			Receipt.StagePath = Stage->GetPathName();
			Receipt.StageClassPath = Stage->GetClass()->GetPathName();
			Receipt.EmitterVersion = Data.Target.EmitterVersion;
			Receipt.UsageId = Data.Request.UsageId;
			Receipt.System = Data.Target.System;
			Receipt.Emitter = Data.Target.Emitter;
			Receipt.Graph = Data.Target.Graph;
			Receipt.Stage = Stage;
			Receipt.EmitterChangeIdAfter = Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.Operation = TEXT("update");
			Receipt.bChanged = true;
			Receipt.bCompiled = Compile.bCompiled;
			Receipt.CompileStatus = Compile.Status;
			Data.Target.System->MarkPackageDirty();
			Receipts().Add(Receipt.ReceiptId, Receipt);
			UpdateRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeResult(Receipt, false));
		}
	};

	class FTool_NiagaraSimulationStageUpdateRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.update.rollback"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId;
			FString RequestId;
			bool bConfirm = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId)
				|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
				|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirm)
				|| ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirm)
			{
				return ErrorResult(TEXT("rollbackId, requestId and confirmWrite=true are required."), TEXT("write_confirmation_required"));
			}
			FStageReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt || Receipt->Operation != TEXT("update") || !Receipt->bHasBeforeState)
			{
				return ErrorResult(TEXT("The simulation-stage update receipt is unknown."), TEXT("receipt_not_found"), 404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraEmitter* Emitter = Receipt->Emitter.Get();
			UNiagaraSimulationStageBase* Stage = Receipt->Stage.Get();
			if (!System || !Emitter || !Stage
				|| Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->EmitterChangeIdAfter)
			{
				return ErrorResult(TEXT("The emitter changed after update; rollback was refused."), TEXT("rollback_conflict"), 409);
			}
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Simulation Stage Update")));
			System->Modify();
			Emitter->Modify();
			Stage->Modify();
			Stage->SimulationStageName = Receipt->BeforeStageName;
			Stage->bEnabled = Receipt->bBeforeEnabled;
		FString RestoreError;
		if (!RestoreCapturedStageProperties(
			Stage,
			Receipt->BeforeProperties,
			&Receipt->BeforeNumIterations,
			Receipt->bHasBeforeNumIterations,
			RestoreError))
		{
			return ErrorResult(RestoreError, TEXT("rollback_restore_failed"), 500);
		}
			if (Receipt->BeforeIndex != INDEX_NONE)
			{
				Emitter->MoveSimulationStageToIndex(Stage, Receipt->BeforeIndex, Receipt->EmitterVersion);
			}
			const FCompileSummary Compile = CompileStage(System, Stage->Script);
			FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage);
			const bool bReadBack = FindStage(Data, Receipt->UsageId) == Stage
				&& Stage->SimulationStageName == Receipt->BeforeStageName
				&& Stage->bEnabled == Receipt->bBeforeEnabled
				&& FindStageIndex(Data, Stage) == Receipt->BeforeIndex
				&& (!Receipt->bHasBeforeNumIterations
					|| (Generic && Generic->NumIterations == Receipt->BeforeNumIterations))
				&& GenericStagePropertiesMatch(Stage, Receipt->BeforeProperties);
			if (!bReadBack || !Compile.bCompiled)
			{
				return ErrorResult(TEXT("Simulation-stage update rollback verification failed."), TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bCompiled = Compile.bCompiled;
			Receipt->CompileStatus = Compile.Status;
			return FMCPToolResult::Ok(MakeResult(*Receipt, false));
		}
	};

	class FTool_NiagaraSimulationStageRemovePlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.remove.plan"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FStagePlanData Data;
			FString Code;
			FString Error;
			if (!BuildExistingPlanData(Params, Data, Code, Error))
			{
				return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Remove"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(TEXT("Unable to compute the simulation-stage removal plan digest."), TEXT("digest_unavailable"), 500);
			}
			Plan->SetStringField(TEXT("planDigest"), Digest);
			return FMCPToolResult::Ok(Plan);
		}
	};

	class FTool_NiagaraSimulationStageRemoveApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.remove.apply"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			FString Code;
			FString Error;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty())
			{
				return ErrorResult(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			}
			if (const FString* ExistingId = RemoveRequestReceiptIds().Find(RequestId))
			{
				if (FStageReceipt* Existing = Receipts().Find(*ExistingId))
				{
					if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Error))
					{
						return ErrorResult(Error, Code, 409);
					}
					return FMCPToolResult::Ok(MakeResult(*Existing, true));
				}
				return ErrorResult(TEXT("requestId is associated with an unavailable receipt."), TEXT("request_id_conflict"), 409);
			}
			FStagePlanData Data;
		if (!BuildExistingPlanData(Params, Data, Code, Error))
			{
				return ErrorResult(Error, Code, Code.Contains(TEXT("not_found")) ? 404 : 422);
			}
			TSharedRef<FJsonObject> Plan = BuildExistingPlanJson(Data, TEXT("Remove"));
			FString Digest;
			if (!TryDigestJson(Plan, Digest))
			{
				return ErrorResult(TEXT("Unable to compute the simulation-stage removal plan digest."), TEXT("digest_unavailable"), 500);
			}
			if (!ValidateChangeApproval(Params, Digest, Code, Error))
			{
				return ErrorResult(Error, Code, 409);
			}
			if (Data.bBlocked)
			{
				return ErrorResult(TEXT("The simulation-stage target is read-only or not owned by the System package."), TEXT("plan_blocked"), 409);
			}
			if (Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Data.Target.EmitterChangeId)
			{
				return ErrorResult(TEXT("The Niagara emitter changed after the plan was created; re-plan before applying."), TEXT("plan_digest_mismatch"), 409);
			}
			UNiagaraSimulationStageBase* Stage = FindStage(Data.Target.Data, Data.Request.UsageId);
			if (!Stage || !Stage->Script)
			{
				return ErrorResult(TEXT("The simulation stage disappeared before apply."), TEXT("stage_not_found"), 404);
			}
			FGraphSnapshot BeforeGraph;
			if (!CaptureGraphSnapshot(Data.Target.Graph, BeforeGraph))
			{
				return ErrorResult(TEXT("The simulation-stage graph could not be snapshotted safely."), TEXT("graph_snapshot_failed"), 500);
			}
			BeforeGraph.RetainedStage.Reset(Stage);
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Remove Niagara Simulation Stage")));
			Data.Target.System->Modify();
			Data.Target.Emitter->Modify();
			Data.Target.Graph->Modify();
			FStageReceipt Receipt;
			Receipt.BeforeStageName = Stage->SimulationStageName;
			Receipt.bBeforeEnabled = Stage->bEnabled;
			Receipt.BeforeIndex = FindStageIndex(Data.Target.Data, Stage);
			Receipt.bHasBeforeState = true;
			const auto RestoreBeforeRemoval = [&]()
			{
				if (!RestoreGraphSnapshot(Data.Target.Graph, BeforeGraph))
				{
					return false;
				}
				Data.Target.Data = Data.Target.Emitter->GetEmitterData(Data.Target.EmitterVersion);
				if (!FindStage(Data.Target.Data, Data.Request.UsageId))
				{
					Data.Target.Emitter->AddSimulationStage(Stage, Data.Target.EmitterVersion);
				}
				if (Receipt.BeforeIndex != INDEX_NONE)
				{
					Data.Target.Emitter->MoveSimulationStageToIndex(Stage, Receipt.BeforeIndex, Data.Target.EmitterVersion);
				}
				Data.Target.Data = Data.Target.Emitter->GetEmitterData(Data.Target.EmitterVersion);
				const FCompileSummary RestoreCompile = CompileStage(Data.Target.System, Stage->Script);
				return FindStage(Data.Target.Data, Data.Request.UsageId) == Stage
					&& FindStageIndex(Data.Target.Data, Stage) == Receipt.BeforeIndex
					&& HasStageOutput(Data.Target.Graph, Data.Request.UsageId)
					&& RestoreCompile.bCompiled && GraphSnapshotMatches(Data.Target.Graph, BeforeGraph);
			};
			const bool bGraphRemoved = RemoveStageOutput(Data.Target.Graph, Data.Request.UsageId);
			Data.Target.Emitter->RemoveSimulationStage(Stage, Data.Target.EmitterVersion);
			const FCompileSummary Compile = CompileSystem(Data.Target.System);
			const bool bReadBack = bGraphRemoved && FindStage(Data.Target.Data, Data.Request.UsageId) == nullptr;
			if (!bReadBack || !Compile.bCompiled)
			{
				if (!RestoreBeforeRemoval())
					return ErrorResult(TEXT("Simulation-stage removal failed and full restoration was not verified; the transaction was retained for Editor Undo."), TEXT("restore_verification_failed"), 500);
				Transaction.Cancel();
				return ErrorResult(TEXT("Simulation-stage removal read-back or compilation failed; the graph was restored."), TEXT("verification_failed"), 500);
			}
			FGraphSnapshot AfterGraph;
			if (!CaptureGraphSnapshot(Data.Target.Graph, AfterGraph))
			{
				if (!RestoreBeforeRemoval())
					return ErrorResult(TEXT("The simulation-stage removal result could not be snapshotted and full restoration was not verified; the transaction was retained for Editor Undo."), TEXT("restore_verification_failed"), 500);
				Transaction.Cancel();
				return ErrorResult(TEXT("The simulation-stage removal result could not be snapshotted safely; the graph was restored."), TEXT("graph_snapshot_failed"), 500);
			}
			Receipt.ReceiptId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.PlanDigest = Digest;
			Receipt.SystemPath = Data.Target.System->GetPathName();
			Receipt.EmitterName = Data.Target.EmitterName;
			Receipt.EmitterPath = Data.Target.EmitterPath;
			Receipt.GraphPath = Data.Target.GraphPath;
			Receipt.StagePath = Stage->GetPathName();
			Receipt.StageClassPath = Stage->GetClass()->GetPathName();
			Receipt.EmitterVersion = Data.Target.EmitterVersion;
			Receipt.UsageId = Data.Request.UsageId;
			Receipt.System = Data.Target.System;
			Receipt.Emitter = Data.Target.Emitter;
			Receipt.Graph = Data.Target.Graph;
			Receipt.Stage = Stage;
			Receipt.BeforeGraphSnapshot = MakeShared<FGraphSnapshot>(MoveTemp(BeforeGraph));
			Receipt.GraphAfterDigest = AfterGraph.Digest;
			Receipt.EmitterChangeIdAfter = Data.Target.Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.Operation = TEXT("remove");
			Receipt.bChanged = true;
			Receipt.bCompiled = Compile.bCompiled;
			Receipt.CompileStatus = Compile.Status;
			Data.Target.System->MarkPackageDirty();
			Receipts().Add(Receipt.ReceiptId, Receipt);
			RemoveRequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
			return FMCPToolResult::Ok(MakeResult(Receipt, false));
		}
	};

	class FTool_NiagaraSimulationStageRemoveRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.simulation_stage.remove.rollback"); }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString ReceiptId;
			FString RequestId;
			bool bConfirm = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), ReceiptId)
				|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
				|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirm)
				|| ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirm)
			{
				return ErrorResult(TEXT("rollbackId, requestId and confirmWrite=true are required."), TEXT("write_confirmation_required"));
			}
			FStageReceipt* Receipt = Receipts().Find(ReceiptId);
			if (!Receipt || Receipt->Operation != TEXT("remove") || !Receipt->bHasBeforeState || !Receipt->BeforeGraphSnapshot.IsValid())
			{
				return ErrorResult(TEXT("The simulation-stage removal receipt is unknown."), TEXT("receipt_not_found"), 404);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(MakeResult(*Receipt, true));
			}
			if (Receipt->RequestId != RequestId)
			{
				return ErrorResult(TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			}
			UNiagaraSystem* System = Receipt->System.Get();
			UNiagaraEmitter* Emitter = Receipt->Emitter.Get();
			UNiagaraGraph* Graph = Receipt->Graph.Get();
			UNiagaraSimulationStageBase* Stage = Receipt->Stage.Get();
			if (!System || !Emitter || !Graph || !Stage
				|| Emitter->GetChangeId().ToString(EGuidFormats::DigitsWithHyphensLower) != Receipt->EmitterChangeIdAfter)
			{
				return ErrorResult(TEXT("The emitter changed after removal; rollback was refused."), TEXT("rollback_conflict"), 409);
			}
			FGraphSnapshot CurrentGraph;
			if (!CaptureGraphSnapshot(Graph, CurrentGraph) || CurrentGraph.Digest != Receipt->GraphAfterDigest)
			{
				return ErrorResult(TEXT("The simulation-stage graph changed after removal; rollback was refused."), TEXT("rollback_conflict"), 409);
			}
			FVersionedNiagaraEmitterData* Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			if (!Data)
			{
				return ErrorResult(TEXT("The simulation-stage emitter version no longer exists."), TEXT("rollback_conflict"), 409);
			}
			if (FindStage(Data, Receipt->UsageId) || HasStageOutput(Graph, Receipt->UsageId))
			{
				return ErrorResult(TEXT("The removed simulation-stage identity is already in use."), TEXT("rollback_conflict"), 409);
			}
			const FGraphSnapshot& BeforeGraph = *Receipt->BeforeGraphSnapshot;
			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Simulation Stage Removal")));
			System->Modify();
			Emitter->Modify();
			Graph->Modify();
			FString RestoreError;
			// RestoreGraphSnapshot(Graph, BeforeGraph) is the single rollback
			// boundary; the optional diagnostic is populated by the same call.
			if (!RestoreGraphSnapshot(Graph, BeforeGraph, &RestoreError))
			{
				return ErrorResult(
					FString::Printf(TEXT("Full simulation-stage graph restoration was refused or failed (%s); the transaction was retained for Editor Undo."), *RestoreError),
					TEXT("rollback_verification_failed"), 500);
			}
			Emitter->AddSimulationStage(Stage, Receipt->EmitterVersion);
			Data = Emitter->GetEmitterData(Receipt->EmitterVersion);
			if (Receipt->BeforeIndex != INDEX_NONE)
			{
				Emitter->MoveSimulationStageToIndex(Stage, Receipt->BeforeIndex, Receipt->EmitterVersion);
			}
			const FCompileSummary Compile = CompileStage(System, Stage->Script);
			const bool bGraphRestored = GraphSnapshotMatches(Graph, BeforeGraph);
			const bool bReadBack = FindStage(Data, Receipt->UsageId) == Stage && bGraphRestored
				&& FindStageIndex(Data, Stage) == Receipt->BeforeIndex
				&& HasStageOutput(Graph, Receipt->UsageId);
			if (!bReadBack || !Compile.bCompiled)
			{
				Emitter->RemoveSimulationStage(Stage, Receipt->EmitterVersion);
				if (SnapshotPinsSurvive(Graph, CurrentGraph))
				{
					ApplyRetainedGraphSnapshot(Graph, CurrentGraph);
				}
				return ErrorResult(FString::Printf(TEXT("Simulation-stage removal rollback verification failed; the transaction was retained for Editor Undo (stageIdentity=%s graphRestored=%s graphPresent=%s compiled=%s exportChars=%d nodes=%d)."),
					FindStage(Data, Receipt->UsageId) == Stage ? TEXT("true") : TEXT("false"),
					bGraphRestored ? TEXT("true") : TEXT("false"),
					HasStageOutput(Graph, Receipt->UsageId) ? TEXT("true") : TEXT("false"),
					Compile.bCompiled ? TEXT("true") : TEXT("false"), BeforeGraph.Export.Len(), BeforeGraph.NodeCount), TEXT("rollback_verification_failed"), 500);
			}
			System->MarkPackageDirty();
			Receipt->bRolledBack = true;
			Receipt->bFullGraphRestored = true;
			Receipt->bCompiled = Compile.bCompiled;
			Receipt->CompileStatus = Compile.Status;
			return FMCPToolResult::Ok(MakeResult(*Receipt, false));
		}
	};

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
			if (!ApplyStageRequestProperties(Stage, Data.Request, Error))
			{
				Transaction.Cancel();
				return ErrorResult(Error, TEXT("property_invalid"), 422);
			}
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
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageList>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageAddPlan>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageAddApply>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageAddRollback>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageUpdatePlan>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageUpdateApply>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageUpdateRollback>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageRemovePlan>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageRemoveApply>());
		Registry.Register(MakeShared<FTool_NiagaraSimulationStageRemoveRollback>());
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
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.list")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.update.plan")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.update.apply")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.update.rollback")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.remove.plan")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.remove.apply")));
		Registry.Register(
			MakeShared<FUnavailableNiagaraSimulationStage>(TEXT("content.niagara.simulation_stage.remove.rollback")));
#endif
	}
}
