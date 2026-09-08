// Plan-gated Niagara Async GPU Trace data-interface configuration.
//
// This surface is deliberately independent from Niagara_Graph.cpp. It edits
// only AsyncGpuTrace data-interface properties and keeps a session receipt so
// a failed compile or an explicit rollback cannot overwrite a later edit.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/DomainChangePlan.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "NiagaraDataInterfaceAsyncGpuTrace.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"

#include "Editor.h"
#include "EdGraph/EdGraphNode.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "SceneManagement.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UEAINiagaraAsyncTracePrivate
{
using UEAIIntegration::Infrastructure::TryDigestJson;
using UEAIIntegration::Infrastructure::ValidateChangeApproval;

constexpr int32 MaxTracesPerParticleLimit = 64;
constexpr int32 MaxRetracesLimit = 32;

struct FAsyncTraceGraphTarget
{
	UNiagaraGraph* Graph = nullptr;
	FString ScopeKind;
	FString ScopeName;
	FString EmitterPath;
	FString ScriptUsage;
	int32 ReferenceDepth = 0;
	bool bOwnedBySystem = false;
};

struct FAsyncTraceSelector
{
	FString InputNodePath;
	FString DataInterfacePath;
	FString InputName;
	FGuid InputNodeGuid;
	bool bHasInputNodeGuid = false;

	bool IsEmpty() const
	{
		return InputNodePath.IsEmpty()
			&& DataInterfacePath.IsEmpty()
			&& InputName.IsEmpty()
			&& !bHasInputNodeGuid;
	}
};

struct FAsyncTraceRequest
{
	bool bSetTraceProvider = false;
	ENDICollisionQuery_AsyncGpuTraceProvider::Type TraceProvider =
		ENDICollisionQuery_AsyncGpuTraceProvider::Default;
	FString TraceProviderName;
	bool bSetMaxTracesPerParticle = false;
	int32 MaxTracesPerParticle = 0;
	bool bSetMaxRetraces = false;
	int32 MaxRetraces = 0;
	bool bAllowReadOnlyTargets = false;
	FString Persistence = TEXT("dirtyOnly");
	FAsyncTraceSelector Selector;
};

struct FAsyncTraceTargetState
{
	TWeakObjectPtr<UNiagaraGraph> Graph;
	TWeakObjectPtr<UNiagaraNodeInput> InputNode;
	TWeakObjectPtr<UNiagaraDataInterfaceAsyncGpuTrace> DataInterface;
	FString GraphPath;
	FString InputNodePath;
	FString DataInterfacePath;
	FString InputName;
	FString BeforeTraceProvider;
	FString AfterTraceProvider;
	int32 BeforeMaxTracesPerParticle = 0;
	int32 AfterMaxTracesPerParticle = 0;
	int32 BeforeMaxRetraces = 0;
	int32 AfterMaxRetraces = 0;
	bool bEditable = false;
	bool bWouldChange = false;
	bool bProviderChanged = false;
	bool bMaxTracesChanged = false;
	bool bMaxRetracesChanged = false;
};

struct FAsyncTraceGraphState
{
	TWeakObjectPtr<UNiagaraGraph> Graph;
	FString GraphPath;
	FString ChangeId;
	FString AfterChangeId;
	FString ScopeKind;
	FString ScopeName;
	int32 ReferenceDepth = 0;
	int32 TargetCount = 0;
	bool bEditable = false;
};

struct FAsyncTracePlanData
{
	TWeakObjectPtr<UNiagaraSystem> System;
	FString SystemPath;
	FString EmitterSelector;
	FString GraphSelector;
	FAsyncTraceRequest Request;
	bool bBlocked = false;
	bool bChangesState = false;
	int32 EditableTargetCount = 0;
	int32 ReadOnlyTargetCount = 0;
	int32 ReadOnlyChangingTargetCount = 0;
	TArray<FString> Risks;
	TArray<FString> Warnings;
	TArray<FAsyncTraceGraphState> Graphs;
	TArray<FAsyncTraceTargetState> Targets;
};

struct FAsyncTraceCompileSummary
{
	FString AggregateStatus = TEXT("unknown");
	bool bCompiled = false;
	bool bHasError = false;
	TArray<TPair<FString, FString>> Scripts;
};

struct FAsyncTraceReceipt
{
	FString ReceiptId;
	FString RequestId;
	FString PlanDigest;
	FString SystemPath;
	TWeakObjectPtr<UNiagaraSystem> System;
	TArray<FAsyncTraceGraphState> Graphs;
	TArray<FAsyncTraceTargetState> Targets;
	bool bChanged = false;
	bool bCompiled = false;
	FString CompileStatus = TEXT("notRequired");
	bool bRolledBack = false;
};

TMap<FString, FAsyncTraceReceipt>& Receipts()
{
	static TMap<FString, FAsyncTraceReceipt> Values;
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

FString NormalizeSystemObjectPath(const FString& RequestedPath)
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

bool LoadSystem(
	const FString& RequestedPath,
	UNiagaraSystem*& OutSystem,
	FString& OutObjectPath,
	FString& OutErrorCode,
	FString& OutError)
{
	OutSystem = nullptr;
	OutObjectPath = NormalizeSystemObjectPath(RequestedPath);
	if (OutObjectPath.IsEmpty())
	{
		OutErrorCode = TEXT("invalid_system_path");
		OutError = TEXT("system must be a non-empty Niagara System object or package path.");
		return false;
	}
	OutSystem = LoadObject<UNiagaraSystem>(nullptr, *OutObjectPath);
	if (!OutSystem)
	{
		OutErrorCode = TEXT("system_not_found");
		OutError = FString::Printf(
			TEXT("Niagara System '%s' was not found."),
			*RequestedPath);
		return false;
	}
	return true;
}

bool IsProjectSystem(const UNiagaraSystem* System)
{
	return System
		&& System->GetOutermost()
		&& System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));
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

void AddGraphAndReferences(
	UNiagaraGraph* RootGraph,
	const FString& ScopeKind,
	const FString& ScopeName,
	const FString& EmitterPath,
	const FString& ScriptUsage,
	UPackage* SystemPackage,
	TArray<FAsyncTraceGraphTarget>& OutGraphs)
{
	if (!RootGraph)
	{
		return;
	}
	TSet<UNiagaraGraph*> Visited;
	TFunction<void(UNiagaraGraph*, int32)> Visit =
		[&](UNiagaraGraph* Graph, const int32 Depth)
	{
		if (!Graph || Visited.Contains(Graph))
		{
			return;
		}
		Visited.Add(Graph);

		FAsyncTraceGraphTarget Target;
		Target.Graph = Graph;
		Target.ScopeKind = ScopeKind;
		Target.ScopeName = ScopeName;
		Target.EmitterPath = EmitterPath;
		Target.ScriptUsage = ScriptUsage;
		Target.ReferenceDepth = Depth;
		Target.bOwnedBySystem = SystemPackage != nullptr
			&& Graph->GetOutermost() == SystemPackage;

		const int32 ExistingIndex = OutGraphs.IndexOfByPredicate(
			[Graph](const FAsyncTraceGraphTarget& Existing)
			{
				return Existing.Graph == Graph;
			});
		if (ExistingIndex == INDEX_NONE)
		{
			OutGraphs.Add(MoveTemp(Target));
		}
		else if (Depth < OutGraphs[ExistingIndex].ReferenceDepth)
		{
			OutGraphs[ExistingIndex] = MoveTemp(Target);
		}

		for (UEdGraphNode* RawNode : Graph->Nodes)
		{
			if (UNiagaraNodeFunctionCall* FunctionCall =
				Cast<UNiagaraNodeFunctionCall>(RawNode))
			{
				Visit(FunctionCall->GetCalledGraph(), Depth + 1);
			}
		}
	};
	Visit(RootGraph, 0);
}

bool CollectGraphs(
	UNiagaraSystem* System,
	const FString& EmitterSelector,
	const FString& GraphSelector,
	TArray<FAsyncTraceGraphTarget>& OutGraphs,
	FString& OutErrorCode,
	FString& OutError)
{
	OutGraphs.Reset();
	if (!System)
	{
		OutErrorCode = TEXT("system_not_found");
		OutError = TEXT("The Niagara System is unavailable.");
		return false;
	}

	if (EmitterSelector.IsEmpty())
	{
		const auto AddSystemScript =
			[&OutGraphs, System](UNiagaraScript* Script, const TCHAR* Usage)
		{
			if (UNiagaraScriptSource* Source = Script
				? Cast<UNiagaraScriptSource>(Script->GetLatestSource())
				: nullptr)
			{
				AddGraphAndReferences(
					Source->NodeGraph,
					TEXT("system"),
					TEXT("system"),
					FString(),
					Usage,
					System->GetOutermost(),
					OutGraphs);
			}
		};
		AddSystemScript(System->GetSystemSpawnScript(), TEXT("systemSpawn"));
		AddSystemScript(System->GetSystemUpdateScript(), TEXT("systemUpdate"));
	}

	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FString EmitterName = Handle.GetName().ToString();
		const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
		const UNiagaraEmitter* Emitter = Instance.Emitter;
		const FString EmitterPath = Emitter ? Emitter->GetPathName() : FString();
		if (!MatchesSelector(EmitterSelector, EmitterName, EmitterPath))
		{
			continue;
		}
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		UNiagaraScriptSource* Source = EmitterData
			? Cast<UNiagaraScriptSource>(EmitterData->GraphSource)
			: nullptr;
		if (Source && Source->NodeGraph)
		{
			AddGraphAndReferences(
				Source->NodeGraph,
				TEXT("emitter"),
				EmitterName,
				EmitterPath,
				TEXT("emitter"),
				System->GetOutermost(),
				OutGraphs);
		}
	}

	if (!GraphSelector.IsEmpty())
	{
		OutGraphs.RemoveAll(
			[&GraphSelector](const FAsyncTraceGraphTarget& Target)
			{
				return !MatchesSelector(
					GraphSelector,
					Target.Graph ? Target.Graph->GetName() : FString(),
					Target.Graph ? Target.Graph->GetPathName() : FString());
			});
	}
	if (OutGraphs.IsEmpty())
	{
		OutErrorCode = TEXT("graph_not_found");
		OutError = TEXT("The Niagara System has no loaded editor graph matching the requested scope.");
		return false;
	}
	return true;
}

UNiagaraDataInterfaceAsyncGpuTrace* FindAsyncTraceDataInterface(
	UNiagaraNodeInput* InputNode)
{
	if (!InputNode)
	{
		return nullptr;
	}
	FObjectProperty* DataInterfaceProperty = FindFProperty<FObjectProperty>(
		InputNode->GetClass(),
		TEXT("DataInterface"));
	if (!DataInterfaceProperty)
	{
		return nullptr;
	}
	return Cast<UNiagaraDataInterfaceAsyncGpuTrace>(
		DataInterfaceProperty->GetObjectPropertyValue_InContainer(InputNode));
}

FString ProviderName(
	const ENDICollisionQuery_AsyncGpuTraceProvider::Type Provider)
{
	switch (Provider)
	{
	case ENDICollisionQuery_AsyncGpuTraceProvider::Default:
		return TEXT("Default");
	case ENDICollisionQuery_AsyncGpuTraceProvider::HWRT:
		return TEXT("HWRT");
	case ENDICollisionQuery_AsyncGpuTraceProvider::GSDF:
		return TEXT("GSDF");
	case ENDICollisionQuery_AsyncGpuTraceProvider::None:
		return TEXT("None");
	default:
		return TEXT("Unknown");
	}
}

bool ParseProvider(
	const FString& Value,
	ENDICollisionQuery_AsyncGpuTraceProvider::Type& OutProvider)
{
	if (Value.Equals(TEXT("Default"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("ProjectDefault"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::Default;
		return true;
	}
	if (Value.Equals(TEXT("HWRT"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("HardwareRayTracing"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::HWRT;
		return true;
	}
	if (Value.Equals(TEXT("GSDF"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("DistanceField"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::GSDF;
		return true;
	}
	if (Value.Equals(TEXT("None"), ESearchCase::IgnoreCase)
		|| Value.Equals(TEXT("Disabled"), ESearchCase::IgnoreCase))
	{
		OutProvider = ENDICollisionQuery_AsyncGpuTraceProvider::None;
		return true;
	}
	return false;
}

bool TryReadOptionalInt(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	const int32 MinValue,
	const int32 MaxValue,
	bool& bOutSet,
	int32& OutValue,
	FString& OutError)
{
	bOutSet = Params->HasField(Field);
	if (!bOutSet)
	{
		return true;
	}
	double Number = 0.0;
	if (!Params->TryGetNumberField(Field, Number)
		|| !FMath::IsFinite(Number)
		|| Number < MinValue
		|| Number > MaxValue
		|| FMath::TruncToInt(Number) != Number)
	{
		OutError = FString::Printf(
			TEXT("%s must be an integer in [%d,%d]."),
			Field,
			MinValue,
			MaxValue);
		return false;
	}
	OutValue = FMath::TruncToInt(Number);
	return true;
}

bool ParseRequest(
	const TSharedPtr<FJsonObject>& Params,
	FAsyncTraceRequest& OutRequest,
	FString& OutErrorCode,
	FString& OutError)
{
	if (!Params.IsValid())
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("An AsyncGpuTrace configuration request is required.");
		return false;
	}
	OutRequest = FAsyncTraceRequest();
	if (Params->HasField(TEXT("traceProvider")))
	{
		FString Provider;
		if (!Params->TryGetStringField(TEXT("traceProvider"), Provider)
			|| !ParseProvider(Provider, OutRequest.TraceProvider))
		{
			OutErrorCode = TEXT("invalid_trace_provider");
			OutError = TEXT("traceProvider must be Default, HWRT, GSDF, or None.");
			return false;
		}
		OutRequest.bSetTraceProvider = true;
		OutRequest.TraceProviderName = ProviderName(OutRequest.TraceProvider);
	}
	if (!TryReadOptionalInt(
		Params,
		TEXT("maxTracesPerParticle"),
		0,
		MaxTracesPerParticleLimit,
		OutRequest.bSetMaxTracesPerParticle,
		OutRequest.MaxTracesPerParticle,
		OutError))
	{
		OutErrorCode = TEXT("invalid_max_traces_per_particle");
		return false;
	}
	if (!TryReadOptionalInt(
		Params,
		TEXT("maxRetraces"),
		0,
		MaxRetracesLimit,
		OutRequest.bSetMaxRetraces,
		OutRequest.MaxRetraces,
		OutError))
	{
		OutErrorCode = TEXT("invalid_max_retraces");
		return false;
	}
	if (!OutRequest.bSetTraceProvider
		&& !OutRequest.bSetMaxTracesPerParticle
		&& !OutRequest.bSetMaxRetraces)
	{
		OutErrorCode = TEXT("no_changes_requested");
		OutError = TEXT("At least one of traceProvider, maxTracesPerParticle, or maxRetraces is required.");
		return false;
	}

	Params->TryGetBoolField(
		TEXT("allowReadOnlyTargets"),
		OutRequest.bAllowReadOnlyTargets);
	if (Params->HasField(TEXT("persistence"))
		&& (!Params->TryGetStringField(TEXT("persistence"), OutRequest.Persistence)
			|| OutRequest.Persistence != TEXT("dirtyOnly")))
	{
		OutErrorCode = TEXT("invalid_persistence");
		OutError = TEXT("Only persistence='dirtyOnly' is supported for Niagara AsyncGpuTrace edits.");
		return false;
	}

	Params->TryGetStringField(TEXT("inputNodePath"), OutRequest.Selector.InputNodePath);
	Params->TryGetStringField(TEXT("dataInterfacePath"), OutRequest.Selector.DataInterfacePath);
	Params->TryGetStringField(TEXT("inputName"), OutRequest.Selector.InputName);
	OutRequest.Selector.InputNodePath =
		OutRequest.Selector.InputNodePath.TrimStartAndEnd();
	OutRequest.Selector.DataInterfacePath =
		OutRequest.Selector.DataInterfacePath.TrimStartAndEnd();
	OutRequest.Selector.InputName =
		OutRequest.Selector.InputName.TrimStartAndEnd();
	FString InputNodeGuidText;
	if (Params->TryGetStringField(TEXT("inputNodeGuid"), InputNodeGuidText)
		&& !InputNodeGuidText.TrimStartAndEnd().IsEmpty())
	{
		if (!FGuid::Parse(InputNodeGuidText.TrimStartAndEnd(), OutRequest.Selector.InputNodeGuid))
		{
			OutErrorCode = TEXT("invalid_input_node_guid");
			OutError = TEXT("inputNodeGuid must be a valid GUID.");
			return false;
		}
		OutRequest.Selector.bHasInputNodeGuid = true;
	}
	return true;
}

bool MatchesAsyncTraceSelector(
	const FAsyncTraceSelector& Selector,
	const UNiagaraNodeInput* InputNode,
	const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface)
{
	if (!InputNode || !DataInterface)
	{
		return false;
	}
	if (!Selector.InputNodePath.IsEmpty()
		&& !MatchesSelector(
			Selector.InputNodePath,
			InputNode->GetName(),
			InputNode->GetPathName()))
	{
		return false;
	}
	if (Selector.bHasInputNodeGuid
		&& InputNode->NodeGuid != Selector.InputNodeGuid)
	{
		return false;
	}
	if (!Selector.DataInterfacePath.IsEmpty()
		&& !MatchesSelector(
			Selector.DataInterfacePath,
			DataInterface->GetName(),
			DataInterface->GetPathName()))
	{
		return false;
	}
	return Selector.InputName.IsEmpty()
		|| Selector.InputName.Equals(
			InputNode->Input.GetName().ToString(),
			ESearchCase::IgnoreCase);
}

bool IsEditableTarget(
	const FAsyncTraceGraphTarget& GraphTarget,
	const UNiagaraSystem* System,
	const UNiagaraNodeInput* InputNode,
	const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface)
{
	const UPackage* SystemPackage = System ? System->GetOutermost() : nullptr;
	return GraphTarget.Graph
		&& SystemPackage
		&& GraphTarget.bOwnedBySystem
		&& GraphTarget.Graph->GetOutermost() == SystemPackage
		&& InputNode
		&& InputNode->GetOutermost() == SystemPackage
		&& DataInterface
		&& DataInterface->GetOutermost() == SystemPackage
		&& SystemPackage->GetName().StartsWith(TEXT("/Game/"));
}

bool IsAsyncProviderAvailable(
	const ENDICollisionQuery_AsyncGpuTraceProvider::Type Provider)
{
	const IConsoleManager& ConsoleManager = IConsoleManager::Get();
	if (Provider == ENDICollisionQuery_AsyncGpuTraceProvider::HWRT)
	{
		const IConsoleVariable* Enable = ConsoleManager.FindConsoleVariable(
			TEXT("fx.Niagara.AsyncGpuTrace.HWRayTraceEnabled"));
		return GRHISupportsRayTracing
			&& GIsRHIInitialized
			&& IsRayTracingEnabled()
			&& Enable
			&& Enable->GetBool();
	}
	if (Provider == ENDICollisionQuery_AsyncGpuTraceProvider::GSDF)
	{
		const IConsoleVariable* Enable = ConsoleManager.FindConsoleVariable(
			TEXT("fx.Niagara.AsyncGpuTrace.GlobalSdfEnabled"));
		return DoesProjectSupportDistanceFields()
			&& Enable
			&& Enable->GetBool();
	}
	return Provider == ENDICollisionQuery_AsyncGpuTraceProvider::None;
}

bool IsDefaultProviderFallbackRisk()
{
	const UNiagaraSettings* Settings = GetDefault<UNiagaraSettings>();
	if (!Settings)
	{
		return true;
	}
	const TArray<TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>>& Order =
		Settings->NDICollisionQuery_AsyncGpuTraceProviderOrder;
	const int32 HardwareIndex = Order.IndexOfByPredicate(
		[](const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>& Provider)
		{
			return Provider == ENDICollisionQuery_AsyncGpuTraceProvider::HWRT;
		});
	const int32 DistanceIndex = Order.IndexOfByPredicate(
		[](const TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>& Provider)
		{
			return Provider == ENDICollisionQuery_AsyncGpuTraceProvider::GSDF;
		});
	return DistanceIndex != INDEX_NONE
		&& (HardwareIndex == INDEX_NONE
			|| DistanceIndex < HardwareIndex
			|| !IsAsyncProviderAvailable(
				ENDICollisionQuery_AsyncGpuTraceProvider::HWRT));
}

TSharedRef<FJsonObject> SerializeRuntimeState()
{
	const IConsoleManager& ConsoleManager = IConsoleManager::Get();
	const IConsoleVariable* RayTracing =
		ConsoleManager.FindConsoleVariable(TEXT("r.RayTracing"));
	const IConsoleVariable* RayTracingEnable =
		ConsoleManager.FindConsoleVariable(TEXT("r.RayTracing.Enable"));
	const IConsoleVariable* AsyncHwrt =
		ConsoleManager.FindConsoleVariable(
			TEXT("fx.Niagara.AsyncGpuTrace.HWRayTraceEnabled"));
	const IConsoleVariable* AsyncGsdf =
		ConsoleManager.FindConsoleVariable(
			TEXT("fx.Niagara.AsyncGpuTrace.GlobalSdfEnabled"));
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("rRayTracing"), RayTracing ? RayTracing->GetInt() : -1);
	Result->SetNumberField(
		TEXT("rRayTracingEnable"),
		RayTracingEnable ? RayTracingEnable->GetInt() : -1);
	Result->SetNumberField(
		TEXT("fxNiagaraAsyncGpuTraceHWRayTraceEnabled"),
		AsyncHwrt ? AsyncHwrt->GetInt() : -1);
	Result->SetNumberField(
		TEXT("fxNiagaraAsyncGpuTraceGlobalSdfEnabled"),
		AsyncGsdf ? AsyncGsdf->GetInt() : -1);
	Result->SetBoolField(TEXT("hardwareSupported"), GRHISupportsRayTracing);
	// IsRayTracingEnabled() asserts before RenderCore initializes the RHI.
	// Async-trace planning is allowed to run during editor startup, so report
	// a conservative disabled state until the RHI is ready.
	const bool bRayTracingEnabled = GIsRHIInitialized && IsRayTracingEnabled();
	Result->SetBoolField(TEXT("rayTracingEnabled"), bRayTracingEnabled);
	Result->SetBoolField(
		TEXT("hwrtAvailable"),
		IsAsyncProviderAvailable(
			ENDICollisionQuery_AsyncGpuTraceProvider::HWRT));
	Result->SetBoolField(
		TEXT("gsdfAvailable"),
		IsAsyncProviderAvailable(
			ENDICollisionQuery_AsyncGpuTraceProvider::GSDF));
	Result->SetBoolField(
		TEXT("defaultProviderFallbackRisk"),
		IsDefaultProviderFallbackRisk());
	return Result;
}

void AddRisk(FAsyncTracePlanData& Data, const FString& Risk)
{
	if (!Data.Risks.Contains(Risk))
	{
		Data.Risks.Add(Risk);
	}
	Data.bBlocked = true;
}

void AddWarning(FAsyncTracePlanData& Data, const FString& Warning)
{
	if (!Data.Warnings.Contains(Warning))
	{
		Data.Warnings.Add(Warning);
	}
}

void SortStates(FAsyncTracePlanData& Data)
{
	Data.Graphs.Sort(
		[](const FAsyncTraceGraphState& Left, const FAsyncTraceGraphState& Right)
		{
			return Left.GraphPath < Right.GraphPath;
		});
	Data.Targets.Sort(
		[](const FAsyncTraceTargetState& Left, const FAsyncTraceTargetState& Right)
		{
			if (Left.GraphPath != Right.GraphPath)
			{
				return Left.GraphPath < Right.GraphPath;
			}
			return Left.DataInterfacePath < Right.DataInterfacePath;
		});
}

void CaptureTargetDesiredState(
	FAsyncTraceTargetState& State,
	const FAsyncTraceRequest& Request)
{
	State.AfterTraceProvider = State.BeforeTraceProvider;
	State.AfterMaxTracesPerParticle = State.BeforeMaxTracesPerParticle;
	State.AfterMaxRetraces = State.BeforeMaxRetraces;
	if (Request.bSetTraceProvider)
	{
		State.AfterTraceProvider = Request.TraceProviderName;
	}
	if (Request.bSetMaxTracesPerParticle)
	{
		State.AfterMaxTracesPerParticle = Request.MaxTracesPerParticle;
	}
	if (Request.bSetMaxRetraces)
	{
		State.AfterMaxRetraces = Request.MaxRetraces;
	}
	State.bProviderChanged =
		State.BeforeTraceProvider != State.AfterTraceProvider;
	State.bMaxTracesChanged =
		State.BeforeMaxTracesPerParticle != State.AfterMaxTracesPerParticle;
	State.bMaxRetracesChanged =
		State.BeforeMaxRetraces != State.AfterMaxRetraces;
	State.bWouldChange = State.bEditable
		&& (State.bProviderChanged
			|| State.bMaxTracesChanged
			|| State.bMaxRetracesChanged);
}

bool BuildPlanData(
	const TSharedPtr<FJsonObject>& Params,
	FAsyncTracePlanData& OutData,
	FString& OutErrorCode,
	FString& OutError)
{
	OutData = FAsyncTracePlanData();
	FString RequestedSystem;
	if (!Params.IsValid()
		|| !Params->TryGetStringField(TEXT("system"), RequestedSystem)
		|| RequestedSystem.TrimStartAndEnd().IsEmpty())
	{
		OutErrorCode = TEXT("invalid_system_path");
		OutError = TEXT("system is required.");
		return false;
	}
	UNiagaraSystem* System = nullptr;
	if (!LoadSystem(
		RequestedSystem,
		System,
		OutData.SystemPath,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	if (!IsProjectSystem(System))
	{
		OutErrorCode = TEXT("asset_scope_forbidden");
		OutError = TEXT("Niagara AsyncGpuTrace writes are restricted to project assets under /Game.");
		return false;
	}
	OutData.System = System;
	Params->TryGetStringField(TEXT("emitter"), OutData.EmitterSelector);
	Params->TryGetStringField(TEXT("graph"), OutData.GraphSelector);
	OutData.EmitterSelector = OutData.EmitterSelector.TrimStartAndEnd();
	OutData.GraphSelector = OutData.GraphSelector.TrimStartAndEnd();
	if (!ParseRequest(Params, OutData.Request, OutErrorCode, OutError))
	{
		return false;
	}

	TArray<FAsyncTraceGraphTarget> GraphTargets;
	if (!CollectGraphs(
		System,
		OutData.EmitterSelector,
		OutData.GraphSelector,
		GraphTargets,
		OutErrorCode,
		OutError))
	{
		return false;
	}
	GraphTargets.Sort(
		[](const FAsyncTraceGraphTarget& Left, const FAsyncTraceGraphTarget& Right)
		{
			return (Left.Graph ? Left.Graph->GetPathName() : FString())
				< (Right.Graph ? Right.Graph->GetPathName() : FString());
		});

	for (const FAsyncTraceGraphTarget& GraphTarget : GraphTargets)
	{
		if (!GraphTarget.Graph)
		{
			continue;
		}
		FAsyncTraceGraphState GraphState;
		GraphState.Graph = GraphTarget.Graph;
		GraphState.GraphPath = GraphTarget.Graph->GetPathName();
		GraphState.ChangeId = GraphTarget.Graph->GetChangeID().ToString(
			EGuidFormats::DigitsWithHyphensLower);
		GraphState.ScopeKind = GraphTarget.ScopeKind;
		GraphState.ScopeName = GraphTarget.ScopeName;
		GraphState.ReferenceDepth = GraphTarget.ReferenceDepth;
		GraphState.bEditable = GraphTarget.bOwnedBySystem
			&& GraphTarget.Graph->GetOutermost() == System->GetOutermost()
			&& System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));

		TSet<UNiagaraDataInterfaceAsyncGpuTrace*> SeenDataInterfaces;
		for (UEdGraphNode* RawNode : GraphTarget.Graph->Nodes)
		{
			UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(RawNode);
			UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
				FindAsyncTraceDataInterface(InputNode);
			if (!InputNode
				|| !DataInterface
				|| !MatchesAsyncTraceSelector(
					OutData.Request.Selector,
					InputNode,
					DataInterface)
				|| SeenDataInterfaces.Contains(DataInterface))
			{
				continue;
			}
			SeenDataInterfaces.Add(DataInterface);
			FAsyncTraceTargetState State;
			State.Graph = GraphTarget.Graph;
			State.InputNode = InputNode;
			State.DataInterface = DataInterface;
			State.GraphPath = GraphState.GraphPath;
			State.InputNodePath = InputNode->GetPathName();
			State.DataInterfacePath = DataInterface->GetPathName();
			State.InputName = InputNode->Input.GetName().ToString();
			State.BeforeTraceProvider = ProviderName(DataInterface->TraceProvider);
			State.BeforeMaxTracesPerParticle = DataInterface->MaxTracesPerParticle;
			State.BeforeMaxRetraces = DataInterface->MaxRetraces;
			State.bEditable = IsEditableTarget(
				GraphTarget,
				System,
				InputNode,
				DataInterface);
			CaptureTargetDesiredState(State, OutData.Request);
			++GraphState.TargetCount;
			OutData.Targets.Add(MoveTemp(State));
		}
		if (GraphState.TargetCount > 0)
		{
			OutData.Graphs.Add(MoveTemp(GraphState));
		}
	}

	if (OutData.Targets.IsEmpty())
	{
		OutErrorCode = TEXT("async_trace_not_found");
		OutError = TEXT("No AsyncGpuTrace data interface matched the requested Niagara System scope and selector.");
		return false;
	}
	for (const FAsyncTraceTargetState& State : OutData.Targets)
	{
		if (State.bEditable)
		{
			++OutData.EditableTargetCount;
		}
		else
		{
			++OutData.ReadOnlyTargetCount;
		}
		if (State.bWouldChange)
		{
			OutData.bChangesState = true;
		}
		else if (!State.bEditable
			&& (State.bProviderChanged
				|| State.bMaxTracesChanged
				|| State.bMaxRetracesChanged))
		{
			++OutData.ReadOnlyChangingTargetCount;
		}
	}
	if (OutData.ReadOnlyChangingTargetCount > 0
		&& !OutData.Request.bAllowReadOnlyTargets)
	{
		AddRisk(
			OutData,
			TEXT("One or more requested AsyncGpuTrace targets are outside the selected System package and are read-only"));
	}
	if (OutData.ReadOnlyTargetCount > 0
		&& OutData.Request.bAllowReadOnlyTargets)
	{
		AddWarning(
			OutData,
			TEXT("Read-only/shared AsyncGpuTrace targets are reported but will be skipped"));
	}
	if (OutData.Targets.Num() > 1 && OutData.Request.Selector.IsEmpty())
	{
		AddWarning(
			OutData,
			TEXT("No input selector was supplied; the request covers every matching AsyncGpuTrace data interface in scope"));
	}
	if (OutData.Request.bSetTraceProvider
		&& OutData.Request.TraceProvider
			== ENDICollisionQuery_AsyncGpuTraceProvider::HWRT
		&& !IsAsyncProviderAvailable(
			ENDICollisionQuery_AsyncGpuTraceProvider::HWRT))
	{
		AddWarning(
			OutData,
			TEXT("HWRT is currently unavailable; the configured provider will not produce hits until ray tracing and Niagara HWRT support are enabled"));
	}
	if (OutData.Request.bSetTraceProvider
		&& OutData.Request.TraceProvider
			== ENDICollisionQuery_AsyncGpuTraceProvider::Default
		&& IsDefaultProviderFallbackRisk())
	{
		AddWarning(
			OutData,
			TEXT("Project Default AsyncGpuTrace may resolve to GSDF before HWRT or when HWRT is unavailable"));
	}
	if (OutData.Request.bSetTraceProvider
		&& OutData.Request.TraceProvider
			== ENDICollisionQuery_AsyncGpuTraceProvider::HWRT)
	{
		AddWarning(
			OutData,
			TEXT("The plan does not wait for a level RT Scene; verify ray-tracing collision readiness after level load before enabling leaf motion"));
	}
	if (OutData.Request.bSetMaxTracesPerParticle
		&& OutData.Request.MaxTracesPerParticle >= 32)
	{
		AddWarning(
			OutData,
			TEXT("A high MaxTracesPerParticle value increases per-particle trace allocation and frame cost"));
	}
	if (OutData.Request.bSetMaxRetraces
		&& OutData.Request.MaxRetraces >= 16)
	{
		AddWarning(
			OutData,
			TEXT("A high MaxRetraces value increases collision work and can amplify one-frame async trace latency"));
	}
	SortStates(OutData);
	return true;
}

TSharedRef<FJsonObject> SerializeTarget(
	const FAsyncTraceTargetState& State,
	const bool bAfterState = false)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graph"), State.GraphPath);
	Result->SetStringField(TEXT("inputNode"), State.InputNodePath);
	Result->SetStringField(TEXT("dataInterface"), State.DataInterfacePath);
	Result->SetStringField(TEXT("inputName"), State.InputName);
	Result->SetBoolField(TEXT("editable"), State.bEditable);
	Result->SetBoolField(TEXT("wouldChange"), State.bWouldChange);
	// Keep the explicit before/after fields for clients that consume the
	// historical result shape, and expose one unambiguous state value for each
	// side of the change plan.
	Result->SetStringField(TEXT("beforeTraceProvider"), State.BeforeTraceProvider);
	Result->SetStringField(TEXT("afterTraceProvider"), State.AfterTraceProvider);
	Result->SetNumberField(
		TEXT("beforeMaxTracesPerParticle"),
		State.BeforeMaxTracesPerParticle);
	Result->SetNumberField(
		TEXT("afterMaxTracesPerParticle"),
		State.AfterMaxTracesPerParticle);
	Result->SetNumberField(TEXT("beforeMaxRetraces"), State.BeforeMaxRetraces);
	Result->SetNumberField(TEXT("afterMaxRetraces"), State.AfterMaxRetraces);
	Result->SetBoolField(TEXT("providerChanged"), State.bProviderChanged);
	Result->SetBoolField(TEXT("maxTracesChanged"), State.bMaxTracesChanged);
	Result->SetBoolField(TEXT("maxRetracesChanged"), State.bMaxRetracesChanged);
	Result->SetStringField(
		TEXT("traceProvider"),
		bAfterState ? State.AfterTraceProvider : State.BeforeTraceProvider);
	Result->SetNumberField(
		TEXT("maxTracesPerParticle"),
		bAfterState
			? State.AfterMaxTracesPerParticle
			: State.BeforeMaxTracesPerParticle);
	Result->SetNumberField(
		TEXT("maxRetraces"),
		bAfterState ? State.AfterMaxRetraces : State.BeforeMaxRetraces);
	Result->SetBoolField(TEXT("afterState"), bAfterState);
	return Result;
}

TSharedRef<FJsonObject> SerializeRequest(
	const FAsyncTracePlanData& Data)
{
	const FAsyncTraceRequest& Request = Data.Request;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("system"), Data.SystemPath);
	if (!Data.EmitterSelector.IsEmpty())
	{
		Result->SetStringField(TEXT("emitter"), Data.EmitterSelector);
	}
	if (!Data.GraphSelector.IsEmpty())
	{
		Result->SetStringField(TEXT("graph"), Data.GraphSelector);
	}
	if (Request.bSetTraceProvider)
	{
		Result->SetStringField(TEXT("traceProvider"), Request.TraceProviderName);
	}
	if (Request.bSetMaxTracesPerParticle)
	{
		Result->SetNumberField(
			TEXT("maxTracesPerParticle"),
			Request.MaxTracesPerParticle);
	}
	if (Request.bSetMaxRetraces)
	{
		Result->SetNumberField(TEXT("maxRetraces"), Request.MaxRetraces);
	}
	if (!Request.Selector.InputNodePath.IsEmpty())
	{
		Result->SetStringField(TEXT("inputNodePath"), Request.Selector.InputNodePath);
	}
	if (Request.Selector.bHasInputNodeGuid)
	{
		Result->SetStringField(
			TEXT("inputNodeGuid"),
			Request.Selector.InputNodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower));
	}
	if (!Request.Selector.DataInterfacePath.IsEmpty())
	{
		Result->SetStringField(
			TEXT("dataInterfacePath"),
			Request.Selector.DataInterfacePath);
	}
	if (!Request.Selector.InputName.IsEmpty())
	{
		Result->SetStringField(TEXT("inputName"), Request.Selector.InputName);
	}
	Result->SetBoolField(
		TEXT("allowReadOnlyTargets"),
		Request.bAllowReadOnlyTargets);
	Result->SetStringField(TEXT("persistence"), Request.Persistence);
	return Result;
}

TSharedRef<FJsonObject> BuildPlanJson(const FAsyncTracePlanData& Data)
{
	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("content.niagara.graph"));
	Plan->SetStringField(TEXT("planKind"), TEXT("niagaraAsyncTraceConfigure"));
	Plan->SetStringField(TEXT("action"), TEXT("configureAsyncGpuTrace"));
	Plan->SetStringField(TEXT("scope"), Data.SystemPath);
	Plan->SetStringField(TEXT("system"), Data.SystemPath);
	Plan->SetBoolField(TEXT("blocked"), Data.bBlocked);
	Plan->SetStringField(
		TEXT("risk"),
		Data.bBlocked ? TEXT("blocked") : TEXT("confirmWrite"));
	Plan->SetStringField(TEXT("persistence"), Data.Request.Persistence);
	Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	Plan->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	Plan->SetBoolField(TEXT("changesState"), Data.bChangesState);
	Plan->SetBoolField(TEXT("confirmWriteRequired"), true);
	Plan->SetNumberField(TEXT("targetCount"), Data.Targets.Num());
	Plan->SetNumberField(TEXT("editableTargetCount"), Data.EditableTargetCount);
	Plan->SetNumberField(TEXT("readOnlyTargetCount"), Data.ReadOnlyTargetCount);
	Plan->SetNumberField(
		TEXT("readOnlyChangingTargetCount"),
		Data.ReadOnlyChangingTargetCount);
	Plan->SetObjectField(TEXT("rayTracing"), SerializeRuntimeState());
	Plan->SetArrayField(
		TEXT("risks"),
		TArray<TSharedPtr<FJsonValue>>());
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
	Plan->SetObjectField(TEXT("request"), SerializeRequest(Data));

	TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
	Preconditions->SetStringField(TEXT("system"), Data.SystemPath);
	Preconditions->SetBoolField(
		TEXT("packageDirty"),
		Data.System.IsValid()
			&& Data.System->GetOutermost()->IsDirty());
	TArray<TSharedPtr<FJsonValue>> GraphValues;
	for (const FAsyncTraceGraphState& State : Data.Graphs)
	{
		TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
		Graph->SetStringField(TEXT("path"), State.GraphPath);
		Graph->SetStringField(TEXT("changeId"), State.ChangeId);
		Graph->SetBoolField(TEXT("editable"), State.bEditable);
		Graph->SetNumberField(TEXT("targetCount"), State.TargetCount);
		GraphValues.Add(MakeShared<FJsonValueObject>(Graph));
	}
	Preconditions->SetArrayField(TEXT("graphs"), GraphValues);
	Plan->SetObjectField(TEXT("preconditions"), Preconditions);

	TArray<TSharedPtr<FJsonValue>> BeforeValues;
	TArray<TSharedPtr<FJsonValue>> AfterValues;
	for (const FAsyncTraceTargetState& State : Data.Targets)
	{
		BeforeValues.Add(MakeShared<FJsonValueObject>(SerializeTarget(State, false)));
		AfterValues.Add(MakeShared<FJsonValueObject>(SerializeTarget(State, true)));
	}
	TSharedRef<FJsonObject> Before = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> After = MakeShared<FJsonObject>();
	Before->SetArrayField(TEXT("targets"), BeforeValues);
	After->SetArrayField(TEXT("targets"), AfterValues);
	Plan->SetObjectField(TEXT("before"), Before);
	Plan->SetObjectField(TEXT("after"), After);
	return Plan;
}

FString CompileStatusName(const ENiagaraScriptCompileStatus Status)
{
	switch (Status)
	{
	case ENiagaraScriptCompileStatus::NCS_Unknown:
		return TEXT("unknown");
	case ENiagaraScriptCompileStatus::NCS_Dirty:
		return TEXT("dirty");
	case ENiagaraScriptCompileStatus::NCS_Error:
		return TEXT("error");
	case ENiagaraScriptCompileStatus::NCS_UpToDate:
		return TEXT("upToDate");
	case ENiagaraScriptCompileStatus::NCS_BeingCreated:
		return TEXT("beingCreated");
	case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		return TEXT("upToDateWithWarnings");
	case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
		return TEXT("computeUpToDateWithWarnings");
	default:
		return TEXT("unknown");
	}
}

FAsyncTraceCompileSummary RequestAndSummarizeCompile(UNiagaraSystem* System)
{
	FAsyncTraceCompileSummary Summary;
	if (!System)
	{
		return Summary;
	}
	System->RequestCompile(false);
	System->WaitForCompilationComplete(true, false);

	TArray<UNiagaraScript*> Scripts;
	TSet<UNiagaraScript*> SeenScripts;
	const auto AddScript = [&Scripts, &SeenScripts](UNiagaraScript* Script)
	{
		if (Script && !SeenScripts.Contains(Script))
		{
			SeenScripts.Add(Script);
			Scripts.Add(Script);
		}
	};
	AddScript(System->GetSystemSpawnScript());
	AddScript(System->GetSystemUpdateScript());
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			EmitterData->ForEachScript(
				[&AddScript](UNiagaraScript* Script)
				{
					AddScript(Script);
				});
		}
	}

	bool bAnyKnown = false;
	bool bAnyDirty = false;
	bool bAnyBeingCreated = false;
	bool bAnyWarning = false;
	bool bAnyUpToDate = false;
	for (UNiagaraScript* Script : Scripts)
	{
		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		Summary.Scripts.Emplace(Script->GetPathName(), CompileStatusName(Status));
		switch (Status)
		{
		case ENiagaraScriptCompileStatus::NCS_Error:
			Summary.bHasError = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_Dirty:
			bAnyDirty = true;
			bAnyKnown = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_BeingCreated:
			bAnyBeingCreated = true;
			bAnyKnown = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings:
		case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings:
			bAnyWarning = true;
			bAnyKnown = true;
			bAnyUpToDate = true;
			break;
		case ENiagaraScriptCompileStatus::NCS_UpToDate:
			bAnyUpToDate = true;
			bAnyKnown = true;
			break;
		default:
			break;
		}
	}
	if (Summary.bHasError)
	{
		Summary.AggregateStatus = TEXT("error");
	}
	else if (bAnyBeingCreated)
	{
		Summary.AggregateStatus = TEXT("beingCreated");
	}
	else if (bAnyDirty)
	{
		Summary.AggregateStatus = TEXT("dirty");
	}
	else if (bAnyWarning)
	{
		Summary.AggregateStatus = TEXT("upToDateWithWarnings");
	}
	else if (bAnyUpToDate)
	{
		Summary.AggregateStatus = TEXT("upToDate");
	}
	Summary.bCompiled = !Summary.bHasError
		&& !bAnyDirty
		&& !bAnyBeingCreated
		&& bAnyKnown;
	return Summary;
}

bool ReadBackTarget(const FAsyncTraceTargetState& State)
{
	const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
		State.DataInterface.Get();
	if (!DataInterface)
	{
		return !State.bEditable;
	}
	if (State.bEditable)
	{
		return ProviderName(DataInterface->TraceProvider) == State.AfterTraceProvider
			&& DataInterface->MaxTracesPerParticle == State.AfterMaxTracesPerParticle
			&& DataInterface->MaxRetraces == State.AfterMaxRetraces;
	}
	return true;
}

bool ApplyTarget(
	FAsyncTraceTargetState& State,
	bool& bOutChanged)
{
	bOutChanged = false;
	if (!State.bEditable)
	{
		return true;
	}
	UNiagaraGraph* Graph = State.Graph.Get();
	UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
		State.DataInterface.Get();
	if (!Graph || !DataInterface)
	{
		return false;
	}
	const bool bNeedsChange =
		ProviderName(DataInterface->TraceProvider) != State.AfterTraceProvider
		|| DataInterface->MaxTracesPerParticle != State.AfterMaxTracesPerParticle
		|| DataInterface->MaxRetraces != State.AfterMaxRetraces;
	if (!bNeedsChange)
	{
		return true;
	}
	DataInterface->Modify();
	ENDICollisionQuery_AsyncGpuTraceProvider::Type DesiredProvider;
	if (!ParseProvider(State.AfterTraceProvider, DesiredProvider))
	{
		return false;
	}
	DataInterface->TraceProvider = DesiredProvider;
	DataInterface->MaxTracesPerParticle = State.AfterMaxTracesPerParticle;
	DataInterface->MaxRetraces = State.AfterMaxRetraces;
	DataInterface->MarkRenderDataDirty();
	Graph->NotifyGraphChanged();
	bOutChanged = true;
	return ReadBackTarget(State);
}

bool RestoreTarget(
	FAsyncTraceTargetState& State,
	const bool bRestoreBefore,
	bool& bOutChanged)
{
	bOutChanged = false;
	if (!State.bEditable)
	{
		return true;
	}
	UNiagaraGraph* Graph = State.Graph.Get();
	UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
		State.DataInterface.Get();
	if (!Graph || !DataInterface)
	{
		return false;
	}
	const FString& ProviderValue = bRestoreBefore
		? State.BeforeTraceProvider
		: State.AfterTraceProvider;
	const int32 MaxTraces = bRestoreBefore
		? State.BeforeMaxTracesPerParticle
		: State.AfterMaxTracesPerParticle;
	const int32 MaxRetraces = bRestoreBefore
		? State.BeforeMaxRetraces
		: State.AfterMaxRetraces;
	const bool bNeedsChange =
		ProviderName(DataInterface->TraceProvider) != ProviderValue
		|| DataInterface->MaxTracesPerParticle != MaxTraces
		|| DataInterface->MaxRetraces != MaxRetraces;
	if (!bNeedsChange)
	{
		return true;
	}
	ENDICollisionQuery_AsyncGpuTraceProvider::Type DesiredProvider;
	if (!ParseProvider(ProviderValue, DesiredProvider))
	{
		return false;
	}
	DataInterface->Modify();
	DataInterface->TraceProvider = DesiredProvider;
	DataInterface->MaxTracesPerParticle = MaxTraces;
	DataInterface->MaxRetraces = MaxRetraces;
	DataInterface->MarkRenderDataDirty();
	Graph->NotifyGraphChanged();
	bOutChanged = true;
	return ProviderName(DataInterface->TraceProvider) == ProviderValue
		&& DataInterface->MaxTracesPerParticle == MaxTraces
		&& DataInterface->MaxRetraces == MaxRetraces;
}

bool CheckGraphPreconditions(
	const FAsyncTracePlanData& Data,
	FString& OutErrorCode,
	FString& OutError)
{
	UNiagaraSystem* System = Data.System.Get();
	if (!System)
	{
		OutErrorCode = TEXT("target_unavailable");
		OutError = TEXT("The Niagara System is no longer loaded.");
		return false;
	}
	for (const FAsyncTraceGraphState& State : Data.Graphs)
	{
		UNiagaraGraph* Graph = State.Graph.Get();
		if (!Graph
			|| Graph->GetChangeID().ToString(
				EGuidFormats::DigitsWithHyphensLower) != State.ChangeId)
		{
			OutErrorCode = TEXT("plan_digest_mismatch");
			OutError = TEXT("A Niagara graph changed after the plan was created; re-plan before applying.");
			return false;
		}
		if (State.bEditable
			&& (Graph->GetOutermost() != System->GetOutermost()
				|| !System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))))
		{
			OutErrorCode = TEXT("graph_scope_forbidden");
			OutError = TEXT("The AsyncGpuTrace target is no longer owned by the selected System package.");
			return false;
		}
	}
	for (const FAsyncTraceTargetState& State : Data.Targets)
	{
		if (!State.bEditable)
		{
			continue;
		}
		if (!IsValid(State.InputNode.Get())
			|| !IsValid(State.DataInterface.Get())
			|| State.InputNode->GetPathName() != State.InputNodePath
			|| State.DataInterface->GetPathName() != State.DataInterfacePath
			|| State.DataInterface->GetOutermost() != Data.System->GetOutermost())
		{
			OutErrorCode = TEXT("target_changed");
			OutError = TEXT("An AsyncGpuTrace input or data interface changed after the plan was created.");
			return false;
		}
	}
	return true;
}

FAsyncTraceReceipt MakeReceipt(
	const FAsyncTracePlanData& Data,
	const FString& RequestId,
	const FString& PlanDigest,
	const FAsyncTraceCompileSummary& CompileSummary,
	const bool bChanged)
{
	FAsyncTraceReceipt Receipt;
	Receipt.ReceiptId = FGuid::NewGuid().ToString(
		EGuidFormats::DigitsWithHyphensLower);
	Receipt.RequestId = RequestId;
	Receipt.PlanDigest = PlanDigest;
	Receipt.SystemPath = Data.SystemPath;
	Receipt.System = Data.System;
	Receipt.Graphs = Data.Graphs;
	Receipt.Targets = Data.Targets;
	Receipt.bChanged = bChanged;
	Receipt.bCompiled = CompileSummary.bCompiled;
	Receipt.CompileStatus = bChanged
		? CompileSummary.AggregateStatus
		: TEXT("notRequired");
	for (FAsyncTraceGraphState& State : Receipt.Graphs)
	{
		State.AfterChangeId = State.Graph.IsValid()
			? State.Graph->GetChangeID().ToString(
				EGuidFormats::DigitsWithHyphensLower)
			: FString();
	}
	return Receipt;
}

TSharedRef<FJsonObject> MakeResult(
	const FAsyncTraceReceipt& Receipt,
	const bool bReplay)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("schema"),
		TEXT("ue.niagara-async-trace-config.v1"));
	Result->SetStringField(TEXT("status"), TEXT("succeeded"));
	Result->SetStringField(TEXT("receiptId"), Receipt.ReceiptId);
	Result->SetStringField(TEXT("requestId"), Receipt.RequestId);
	Result->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
	Result->SetStringField(TEXT("system"), Receipt.SystemPath);
	Result->SetNumberField(TEXT("targetCount"), Receipt.Targets.Num());
	Result->SetBoolField(TEXT("changed"), Receipt.bChanged);
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("compiled"), Receipt.bCompiled);
	Result->SetStringField(TEXT("compileStatus"), Receipt.CompileStatus);
	Result->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
	Result->SetBoolField(TEXT("idempotentReplay"), bReplay);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	TArray<TSharedPtr<FJsonValue>> Targets;
	for (const FAsyncTraceTargetState& State : Receipt.Targets)
	{
		Targets.Add(MakeShared<FJsonValueObject>(
			SerializeTarget(State, !Receipt.bRolledBack)));
	}
	Result->SetArrayField(TEXT("targets"), Targets);
	return Result;
}

class FTool_AsyncTraceConfigurePlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.async_trace.configure.plan");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FAsyncTracePlanData Data;
		FString ErrorCode;
		FString Error;
		if (!BuildPlanData(Params, Data, ErrorCode, Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					|| ErrorCode == TEXT("async_trace_not_found")
					? 404
					: 422);
		}
		TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
		FString Digest;
		if (!TryDigestJson(Plan, Digest))
		{
			return ErrorResult(
				TEXT("Unable to compute the AsyncGpuTrace configuration plan digest."),
				TEXT("digest_unavailable"),
				500);
		}
		Plan->SetStringField(TEXT("planDigest"), Digest);
		return FMCPToolResult::Ok(Plan);
	}
};

class FTool_AsyncTraceConfigureApply final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.async_trace.configure.apply");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString RequestId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
			|| RequestId.IsEmpty())
		{
			return ErrorResult(
				TEXT("A non-empty requestId is required for AsyncGpuTrace writes."),
				TEXT("request_id_required"),
				422);
		}
		if (const FString* ExistingReceiptId = RequestReceiptIds().Find(RequestId))
		{
			if (FAsyncTraceReceipt* Existing = Receipts().Find(*ExistingReceiptId))
			{
				FString ErrorCode;
				FString Error;
				if (!ValidateChangeApproval(
					Params,
					Existing->PlanDigest,
					ErrorCode,
					Error))
				{
					return ErrorResult(Error, ErrorCode, 409);
				}
				return FMCPToolResult::Ok(MakeResult(*Existing, true));
			}
			return ErrorResult(
				TEXT("requestId is associated with an unavailable AsyncGpuTrace receipt."),
				TEXT("request_id_conflict"),
				409);
		}

		FAsyncTracePlanData Data;
		FString ErrorCode;
		FString Error;
		if (!BuildPlanData(Params, Data, ErrorCode, Error))
		{
			return ErrorResult(
				Error,
				ErrorCode,
				ErrorCode == TEXT("system_not_found")
					|| ErrorCode == TEXT("graph_not_found")
					|| ErrorCode == TEXT("async_trace_not_found")
					? 404
					: 422);
		}
		TSharedRef<FJsonObject> Plan = BuildPlanJson(Data);
		FString PlanDigest;
		if (!TryDigestJson(Plan, PlanDigest))
		{
			return ErrorResult(
				TEXT("Unable to compute the AsyncGpuTrace configuration plan digest."),
				TEXT("digest_unavailable"),
				500);
		}
		if (!ValidateChangeApproval(Params, PlanDigest, ErrorCode, Error))
		{
			return ErrorResult(Error, ErrorCode, 409);
		}
		if (Data.bBlocked)
		{
			return ErrorResult(
				TEXT("The AsyncGpuTrace configuration plan is blocked by unresolved safety risks."),
				TEXT("plan_blocked"),
				409);
		}
		if (!CheckGraphPreconditions(Data, ErrorCode, Error))
		{
			return ErrorResult(Error, ErrorCode, 409);
		}

		UNiagaraSystem* System = Data.System.Get();
		if (!System)
		{
			return ErrorResult(
				TEXT("The Niagara System is no longer loaded."),
				TEXT("target_unavailable"),
				409);
		}
		FAsyncTraceCompileSummary CompileSummary;
		bool bAnyChanged = false;
		if (Data.bChangesState)
		{
			FScopedTransaction Transaction(
				FText::FromString(TEXT("UE AI Configure Niagara AsyncGpuTrace")));
			System->Modify();
			TSet<UNiagaraGraph*> ModifiedGraphs;
			bool bApplied = true;
			for (FAsyncTraceTargetState& State : Data.Targets)
			{
				if (!State.bEditable)
				{
					continue;
				}
				UNiagaraGraph* Graph = State.Graph.Get();
				if (!Graph)
				{
					bApplied = false;
					break;
				}
				if (!ModifiedGraphs.Contains(Graph))
				{
					Graph->Modify();
					ModifiedGraphs.Add(Graph);
				}
				bool bChanged = false;
				bApplied &= ApplyTarget(State, bChanged);
				bAnyChanged |= bChanged;
				if (!bApplied)
				{
					break;
				}
			}
			if (bAnyChanged)
			{
				System->MarkPackageDirty();
				CompileSummary = RequestAndSummarizeCompile(System);
			}
			bool bVerified = bApplied;
			for (const FAsyncTraceTargetState& State : Data.Targets)
			{
				bVerified &= ReadBackTarget(State);
			}
			bVerified &= !bAnyChanged || CompileSummary.bCompiled;
			if (!bVerified)
			{
				bool bRecoveryVerified = true;
				for (FAsyncTraceTargetState& State : Data.Targets)
				{
					bool bChanged = false;
					bRecoveryVerified &= RestoreTarget(State, true, bChanged);
				}
				if (bAnyChanged)
				{
					bRecoveryVerified &= RequestAndSummarizeCompile(System).bCompiled;
				}
				for (const FAsyncTraceTargetState& State : Data.Targets)
				{
					const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface = State.DataInterface.Get();
					if (State.bEditable)
					{
						bRecoveryVerified &= DataInterface
							&& ProviderName(DataInterface->TraceProvider) == State.BeforeTraceProvider
							&& DataInterface->MaxTracesPerParticle == State.BeforeMaxTracesPerParticle
							&& DataInterface->MaxRetraces == State.BeforeMaxRetraces;
					}
				}
				if (!bRecoveryVerified)
				{
					return ErrorResult(
						TEXT("AsyncGpuTrace edit failed and restoration could not be verified; the transaction was retained for Editor Undo."),
						TEXT("restore_verification_failed"), 500);
				}
				Transaction.Cancel();
				return ErrorResult(
					TEXT("AsyncGpuTrace read-back or compilation failed; the change was restored."),
					TEXT("verification_failed"),
					500);
			}
		}

		FAsyncTraceReceipt Receipt = MakeReceipt(
			Data,
			RequestId,
			PlanDigest,
			CompileSummary,
			bAnyChanged);
		Receipts().Add(Receipt.ReceiptId, Receipt);
		RequestReceiptIds().Add(RequestId, Receipt.ReceiptId);
		return FMCPToolResult::Ok(MakeResult(Receipt, false));
	}
};

class FTool_AsyncTraceConfigureRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.graph.async_trace.configure.rollback");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ReceiptId;
		FString RequestId;
		bool bConfirmWrite = false;
		if (!Params.IsValid())
		{
			return ErrorResult(
				TEXT("rollbackId, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				422);
		}
		Params->TryGetStringField(TEXT("rollbackId"), ReceiptId);
		Params->TryGetStringField(TEXT("requestId"), RequestId);
		Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite);
		if (ReceiptId.IsEmpty() || RequestId.IsEmpty() || !bConfirmWrite)
		{
			return ErrorResult(
				TEXT("rollbackId, requestId and confirmWrite=true are required."),
				TEXT("write_confirmation_required"),
				422);
		}
		FAsyncTraceReceipt* Receipt = Receipts().Find(ReceiptId);
		if (!Receipt)
		{
			return ErrorResult(
				TEXT("The AsyncGpuTrace receipt is unknown in this Editor instance."),
				TEXT("receipt_not_found"),
				404);
		}
		if (Receipt->bRolledBack)
		{
			return FMCPToolResult::Ok(MakeResult(*Receipt, true));
		}
		if (Receipt->RequestId != RequestId)
		{
			return ErrorResult(
				TEXT("requestId does not match the AsyncGpuTrace receipt."),
				TEXT("request_id_mismatch"),
				409);
		}
		UNiagaraSystem* System = Receipt->System.Get();
		if (!System)
		{
			return ErrorResult(
				TEXT("The Niagara System is no longer loaded."),
				TEXT("target_unavailable"),
				409);
		}
		for (const FAsyncTraceGraphState& State : Receipt->Graphs)
		{
			if (!State.bEditable)
			{
				continue;
			}
			UNiagaraGraph* Graph = State.Graph.Get();
			if (!Graph
				|| Graph->GetChangeID().ToString(
					EGuidFormats::DigitsWithHyphensLower) != State.AfterChangeId)
			{
				return ErrorResult(
					TEXT("A Niagara graph changed after AsyncGpuTrace apply; rollback was refused."),
					TEXT("rollback_conflict"),
					409);
			}
		}
		for (const FAsyncTraceTargetState& State : Receipt->Targets)
		{
			if (!State.bEditable)
			{
				continue;
			}
			const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
				State.DataInterface.Get();
			if (!DataInterface
				|| ProviderName(DataInterface->TraceProvider)
					!= State.AfterTraceProvider
				|| DataInterface->MaxTracesPerParticle
					!= State.AfterMaxTracesPerParticle
				|| DataInterface->MaxRetraces != State.AfterMaxRetraces)
			{
				return ErrorResult(
					TEXT("An AsyncGpuTrace target changed after apply; rollback was refused."),
					TEXT("rollback_conflict"),
					409);
			}
		}
		if (!Receipt->bChanged)
		{
			Receipt->bRolledBack = true;
			return FMCPToolResult::Ok(MakeResult(*Receipt, false));
		}

		FScopedTransaction Transaction(
			FText::FromString(TEXT("UE AI Rollback Niagara AsyncGpuTrace")));
		System->Modify();
		TSet<UNiagaraGraph*> ModifiedGraphs;
		bool bRestored = true;
		bool bAnyChanged = false;
		for (FAsyncTraceTargetState& State : Receipt->Targets)
		{
			if (!State.bEditable)
			{
				continue;
			}
			UNiagaraGraph* Graph = State.Graph.Get();
			if (!Graph)
			{
				bRestored = false;
				break;
			}
			if (!ModifiedGraphs.Contains(Graph))
			{
				Graph->Modify();
				ModifiedGraphs.Add(Graph);
			}
			bool bChanged = false;
			bRestored &= RestoreTarget(State, true, bChanged);
			bAnyChanged |= bChanged;
			if (!bRestored)
			{
				break;
			}
		}
		FAsyncTraceCompileSummary CompileSummary;
		if (bAnyChanged)
		{
			System->MarkPackageDirty();
			CompileSummary = RequestAndSummarizeCompile(System);
		}
		for (const FAsyncTraceTargetState& State : Receipt->Targets)
		{
			if (!State.bEditable)
			{
				continue;
			}
			const UNiagaraDataInterfaceAsyncGpuTrace* DataInterface =
				State.DataInterface.Get();
			bRestored &= DataInterface
				&& ProviderName(DataInterface->TraceProvider)
					== State.BeforeTraceProvider
				&& DataInterface->MaxTracesPerParticle
					== State.BeforeMaxTracesPerParticle
				&& DataInterface->MaxRetraces == State.BeforeMaxRetraces;
		}
		bRestored &= !bAnyChanged || CompileSummary.bCompiled;
		if (!bRestored)
		{
			return ErrorResult(
				TEXT("AsyncGpuTrace rollback read-back or compilation failed; the transaction was retained for Editor Undo."),
				TEXT("rollback_verification_failed"),
				500);
		}
		Receipt->bCompiled = bAnyChanged
			? CompileSummary.bCompiled
			: Receipt->bCompiled;
		Receipt->CompileStatus = bAnyChanged
			? CompileSummary.AggregateStatus
			: Receipt->CompileStatus;
		Receipt->bRolledBack = true;
		return FMCPToolResult::Ok(MakeResult(*Receipt, false));
	}
};

} // namespace UEAINiagaraAsyncTracePrivate

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphAdvancedTools(FMCPToolRegistry& Registry)
{
	using namespace UEAINiagaraAsyncTracePrivate;
	Registry.Register(MakeShared<FTool_AsyncTraceConfigurePlan>());
	Registry.Register(MakeShared<FTool_AsyncTraceConfigureApply>());
	Registry.Register(MakeShared<FTool_AsyncTraceConfigureRollback>());
}
}

#else

class FUnavailableNiagaraAsyncTraceTool final : public FMCPToolBase
{
public:
	explicit FUnavailableNiagaraAsyncTraceTool(FString InCapabilityId)
		: CapabilityId(MoveTemp(InCapabilityId))
	{
	}

	FString GetCapabilityId() const override
	{
		return CapabilityId;
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(
			TEXT("Niagara AsyncGpuTrace support was not compiled into this plugin build."),
			TEXT("capability_unavailable"),
			409);
	}

private:
	FString CapabilityId;
};

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphAdvancedTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FUnavailableNiagaraAsyncTraceTool>(
		TEXT("content.niagara.graph.async_trace.configure.plan")));
	Registry.Register(MakeShared<FUnavailableNiagaraAsyncTraceTool>(
		TEXT("content.niagara.graph.async_trace.configure.apply")));
	Registry.Register(MakeShared<FUnavailableNiagaraAsyncTraceTool>(
		TEXT("content.niagara.graph.async_trace.configure.rollback")));
}
}

#endif
