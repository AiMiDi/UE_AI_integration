// Niagara compile requests, diagnostics, and generated GPU source.
// Compilation evidence does not prove execution by a runtime component or GPU frame.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraShared.h"
#include "NiagaraSystem.h"
#include "Misc/PackageName.h"
#include "ShaderCompiler.h"
#include "UObject/Package.h"

namespace UEAINiagaraCompile
{
namespace
{
constexpr int32 MaxPathCharacters = 2048;
constexpr int32 MaxEmitters = 128;
constexpr int32 MaxScripts = 256;
constexpr int32 MaxDiagnosticRows = 256;
constexpr int32 MaxDiagnosticCharacters = 256 * 1024;
constexpr int32 DefaultMaxSourceCharacters = 1024 * 1024;
constexpr int32 MaxSourceCharacters = 4 * 1024 * 1024;

FMCPToolResult Error(const FString& Message, const TCHAR* Code = TEXT("invalid_niagara_compile_request"), int32 Status = 422)
{
	return FMCPToolResult::Error(Message, Code, Status);
}

bool ReadBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, bool DefaultValue, bool& OutValue)
{
	OutValue = DefaultValue;
	if (!Params.IsValid() || !Params->HasField(Name))
	{
		return true;
	}
	// TryGetBoolField coerces strings/numbers via FString::ToBool, so check the
	// JSON type explicitly; a non-boolean field is an invalid request.
	const TSharedPtr<FJsonValue> Field = Params->TryGetField(Name);
	if (!Field.IsValid() || Field->Type != EJson::Boolean)
	{
		return false;
	}
	Field->TryGetBool(OutValue);
	return true;
}

bool ReadSourceLimit(const TSharedPtr<FJsonObject>& Params, int32& OutValue)
{
	OutValue = DefaultMaxSourceCharacters;
	if (!Params.IsValid() || !Params->HasField(TEXT("maxCharacters")))
	{
		return true;
	}
	double Number = 0.0;
	if (!Params->TryGetNumberField(TEXT("maxCharacters"), Number) || !FMath::IsFinite(Number)
		|| Number < 1.0 || Number > MaxSourceCharacters || Number != FMath::FloorToDouble(Number))
	{
		return false;
	}
	OutValue = static_cast<int32>(Number);
	return true;
}

FMCPToolResult LoadSystem(const TSharedPtr<FJsonObject>& Params, UNiagaraSystem*& OutSystem)
{
	OutSystem = nullptr;
	FString ObjectPath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), ObjectPath)
		|| ObjectPath.IsEmpty() || ObjectPath.Len() > MaxPathCharacters)
	{
		return Error(TEXT("system must be an exact Niagara System package or object path of at most 2048 characters."));
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		return Error(TEXT("system must be a valid long package or object path."));
	}
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
	}
	OutSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
	if (!OutSystem)
	{
		return Error(FString::Printf(TEXT("Niagara System '%s' was not found."), *ObjectPath), TEXT("system_not_found"), 404);
	}
	if (OutSystem->GetEmitterHandles().Num() > MaxEmitters)
	{
		return Error(TEXT("The system exceeds the 128-emitter inspection limit."), TEXT("niagara_inspection_limit"), 413);
	}
	return FMCPToolResult::Ok(nullptr);
}

FString CompileStatusName(ENiagaraScriptCompileStatus Status)
{
	switch (Status)
	{
	case ENiagaraScriptCompileStatus::NCS_Dirty: return TEXT("dirty");
	case ENiagaraScriptCompileStatus::NCS_Error: return TEXT("error");
	case ENiagaraScriptCompileStatus::NCS_UpToDate: return TEXT("upToDate");
	case ENiagaraScriptCompileStatus::NCS_BeingCreated: return TEXT("beingCreated");
	case ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings: return TEXT("upToDateWithWarnings");
	case ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings: return TEXT("computeUpToDateWithWarnings");
	default: return TEXT("unknown");
	}
}

bool IsUpToDate(ENiagaraScriptCompileStatus Status)
{
	return Status == ENiagaraScriptCompileStatus::NCS_UpToDate
		|| Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
		|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
}

struct FScriptEntry
{
	UNiagaraScript* Script = nullptr;
	FString Owner;
	FGuid EmitterId;
	bool bEnabled = true;
};

FMCPToolResult CollectScripts(UNiagaraSystem* System, TArray<FScriptEntry>& OutScripts)
{
	TSet<UNiagaraScript*> Seen;
	auto Add = [&](UNiagaraScript* Script, const FString& Owner, const FGuid& EmitterId, bool bEnabled)
	{
		if (Script && !Seen.Contains(Script))
		{
			Seen.Add(Script);
			FScriptEntry& Entry = OutScripts.AddDefaulted_GetRef();
			Entry.Script = Script;
			Entry.Owner = Owner;
			Entry.EmitterId = EmitterId;
			Entry.bEnabled = bEnabled;
		}
	};
	Add(System->GetSystemSpawnScript(), TEXT("System"), FGuid(), true);
	Add(System->GetSystemUpdateScript(), TEXT("System"), FGuid(), true);
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData())
		{
			TArray<UNiagaraScript*> Scripts;
			// Emitter spawn/update and simulation-stage graphs are inlined into the
			// system or GPU script; they are not independent compilation results.
			Data->GetScripts(Scripts, true, true);
			for (UNiagaraScript* Script : Scripts)
			{
				Add(Script, Handle.GetName().ToString(), Handle.GetId(), Handle.GetIsEnabled());
				if (OutScripts.Num() > MaxScripts)
				{
					return Error(TEXT("The system exceeds the 256-script inspection limit."), TEXT("niagara_inspection_limit"), 413);
				}
			}
		}
	}
	return FMCPToolResult::Ok(nullptr);
}

// One shared budget bounds compiler events, renderer feedback, and long messages.
struct FDiagnosticBudget
{
	int32 RemainingRows = MaxDiagnosticRows;
	int32 RemainingCharacters = MaxDiagnosticCharacters;
	bool bTruncated = false;

	FString Text(const FString& Value, int32 Limit)
	{
		const int32 Count = FMath::Min3(Value.Len(), Limit, RemainingCharacters);
		RemainingCharacters -= Count;
		bTruncated |= Count < Value.Len();
		return Value.Left(Count);
	}

	bool TakeRow()
	{
		if (RemainingRows <= 0 || RemainingCharacters <= 0)
		{
			bTruncated = true;
			return false;
		}
		--RemainingRows;
		return true;
	}
};

struct FCompileSummary
{
	FString Status = TEXT("unknown");
	bool bCompiled = false;
	bool bHasError = false;
	bool bStale = false;
	bool bOutstanding = false;
	int32 RequiredScripts = 0;
};

FCompileSummary Summarize(UNiagaraSystem* System, const TArray<FScriptEntry>& Scripts)
{
	FCompileSummary Summary;
	Summary.bOutstanding = System->HasOutstandingCompilationRequests(true);
	bool bUnknown = !System->GetSystemSpawnScript() || !System->GetSystemUpdateScript();
	bool bDirty = false;
	bool bWarning = false;
	bool bAllValid = true;
	for (const FScriptEntry& Entry : Scripts)
	{
		if (!Entry.bEnabled)
		{
			continue;
		}
		++Summary.RequiredScripts;
		const UNiagaraScript* Script = Entry.Script;
		const ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
		if (Status == ENiagaraScriptCompileStatus::NCS_Unknown)
		{
			// No compiled VM data exists yet; the detailed VM accessors below can
			// range-check an empty array on an uncompiled script.
			bUnknown = true;
			continue;
		}
		const bool bGpu = Script->GetUsage() == ENiagaraScriptUsage::ParticleGPUComputeScript;
		Summary.bHasError |= Status == ENiagaraScriptCompileStatus::NCS_Error;
		Summary.bOutstanding |= Status == ENiagaraScriptCompileStatus::NCS_BeingCreated || Script->IsScriptCompilationPending(bGpu);
		bUnknown |= Status == ENiagaraScriptCompileStatus::NCS_Unknown;
		bDirty |= Status == ENiagaraScriptCompileStatus::NCS_Dirty || !Script->AreScriptAndSourceSynchronized();
		bWarning |= Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
		const FNiagaraVMExecutableData& VMData = Script->GetVMExecutableData();
		const bool bGeneratedData = VMData.IsValid() || (bGpu && (!VMData.LastHlslTranslationGPU.IsEmpty() || !VMData.LastHlslTranslation.IsEmpty() || !VMData.LastAssemblyTranslation.IsEmpty()));
		bAllValid &= IsUpToDate(Status) && bGeneratedData && Script->DidScriptCompilationSucceed(bGpu);
	}
	Summary.bStale = Summary.bOutstanding || bDirty || bUnknown || Summary.RequiredScripts == 0;
	Summary.bCompiled = !Summary.bStale && !Summary.bHasError && bAllValid;
	if (Summary.bHasError) Summary.Status = TEXT("error");
	else if (Summary.bOutstanding) Summary.Status = TEXT("beingCreated");
	else if (bDirty) Summary.Status = TEXT("dirty");
	else if (bUnknown || Summary.RequiredScripts == 0) Summary.Status = TEXT("unknown");
	else if (!bAllValid) Summary.Status = TEXT("unavailable");
	else Summary.Status = bWarning ? TEXT("upToDateWithWarnings") : TEXT("upToDate");
	return Summary;
}

TSharedRef<FJsonObject> ScriptJson(const FScriptEntry& Entry, FDiagnosticBudget& Budget)
{
	const UNiagaraScript* Script = Entry.Script;
	// Uncompiled scripts have no VM executable data; do not touch the engine's
	// VM accessors (they assume a compiled script and can range-check an empty
	// array). Report a stable unknown row instead.
	if (Script->GetLastCompileStatus() == ENiagaraScriptCompileStatus::NCS_Unknown)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("path"), Budget.Text(Script->GetPathName(), MaxPathCharacters));
		Json->SetStringField(TEXT("owner"), Budget.Text(Entry.Owner, MaxPathCharacters));
		Json->SetStringField(TEXT("emitterId"), Entry.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Json->SetBoolField(TEXT("enabled"), Entry.bEnabled);
		Json->SetStringField(TEXT("usage"), StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script->GetUsage())));
		Json->SetStringField(TEXT("status"), TEXT("unknown"));
		Json->SetBoolField(TEXT("valid"), false);
		Json->SetBoolField(TEXT("sourceSynchronized"), false);
		Json->SetBoolField(TEXT("compilationPending"), false);
		Json->SetBoolField(TEXT("compilationSucceeded"), false);
		Json->SetNumberField(TEXT("opCount"), 0);
		Json->SetNumberField(TEXT("tempRegisters"), 0);
		Json->SetStringField(TEXT("errorMessage"), TEXT(""));
		Json->SetArrayField(TEXT("events"), {});
		Json->SetNumberField(TEXT("eventCount"), 0);
		Json->SetBoolField(TEXT("eventsTruncated"), false);
		return Json;
	}
	const FNiagaraVMExecutableData& VMData = Script->GetVMExecutableData();
	const bool bGpu = Script->GetUsage() == ENiagaraScriptUsage::ParticleGPUComputeScript;
	auto Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("path"), Budget.Text(Script->GetPathName(), MaxPathCharacters));
	Json->SetStringField(TEXT("owner"), Budget.Text(Entry.Owner, MaxPathCharacters));
	Json->SetStringField(TEXT("emitterId"), Entry.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
	Json->SetBoolField(TEXT("enabled"), Entry.bEnabled);
	Json->SetStringField(TEXT("usage"), StaticEnum<ENiagaraScriptUsage>()->GetNameStringByValue(static_cast<int64>(Script->GetUsage())));
	Json->SetStringField(TEXT("status"), CompileStatusName(Script->GetLastCompileStatus()));
	Json->SetBoolField(TEXT("valid"), VMData.IsValid());
	Json->SetBoolField(TEXT("sourceSynchronized"), Script->AreScriptAndSourceSynchronized());
	Json->SetBoolField(TEXT("compilationPending"), Script->IsScriptCompilationPending(bGpu));
	Json->SetBoolField(TEXT("compilationSucceeded"), Script->DidScriptCompilationSucceed(bGpu));
	Json->SetNumberField(TEXT("opCount"), VMData.LastOpCount);
	Json->SetNumberField(TEXT("tempRegisters"), VMData.NumTempRegisters);
	Json->SetStringField(TEXT("errorMessage"), Budget.Text(VMData.ErrorMsg, 4096));
	TArray<TSharedPtr<FJsonValue>> Events;
	for (const FNiagaraCompileEvent& Event : VMData.LastCompileEvents)
	{
		if (Events.Num() >= 128 || !Budget.TakeRow())
		{
			Budget.bTruncated = true;
			break;
		}
		auto EventJson = MakeShared<FJsonObject>();
		EventJson->SetStringField(TEXT("message"), Budget.Text(Event.Message, 4096));
		EventJson->SetStringField(TEXT("shortDescription"), Budget.Text(Event.ShortDescription, 1024));
		EventJson->SetStringField(TEXT("severity"), Event.Severity == FNiagaraCompileEventSeverity::Error ? TEXT("error")
			: Event.Severity == FNiagaraCompileEventSeverity::Warning ? TEXT("warning") : TEXT("info"));
		Events.Add(MakeShared<FJsonValueObject>(EventJson));
	}
	Json->SetArrayField(TEXT("events"), Events);
	Json->SetNumberField(TEXT("eventCount"), VMData.LastCompileEvents.Num());
	Json->SetBoolField(TEXT("eventsTruncated"), Events.Num() < VMData.LastCompileEvents.Num());
	return Json;
}

void AddRendererDiagnostics(UNiagaraSystem* System, FDiagnosticBudget& Budget, const TSharedRef<FJsonObject>& Json)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	bool bStopped = false;
#if WITH_EDITOR
	int32 RendererCount = 0;
	auto Add = [&](const FNiagaraEmitterHandle& Handle, const FString& Source, const TCHAR* Severity, const FString& Message)
	{
		if (!Budget.TakeRow()) return;
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("emitter"), Budget.Text(Handle.GetName().ToString(), MaxPathCharacters));
		Row->SetStringField(TEXT("emitterId"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Row->SetStringField(TEXT("source"), Budget.Text(Source, MaxPathCharacters));
		Row->SetStringField(TEXT("severity"), Severity);
		Row->SetStringField(TEXT("message"), Budget.Text(Message, 4096));
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	};
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data || !Handle.GetIsEnabled()) continue;
		for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			if (!Renderer || !Renderer->GetIsEnabled()) continue;
			if (++RendererCount > MaxDiagnosticRows || Budget.RemainingRows <= 0)
			{
				Budget.bTruncated = bStopped = true;
				break;
			}
			const FString Source = Renderer->GetPathName();
			if (!Renderer->IsSimTargetSupported(Data->SimTarget))
			{
				Add(Handle, Source, TEXT("error"), TEXT("Renderer is incompatible with the emitter simulation target."));
			}
			TArray<FNiagaraRendererFeedback> Errors, Warnings, Infos;
			Renderer->GetRendererFeedback(Handle.GetInstance(), Errors, Warnings, Infos);
			for (const FNiagaraRendererFeedback& Feedback : Errors) Add(Handle, Source, TEXT("error"), Feedback.GetDescriptionText().ToString());
			for (const FNiagaraRendererFeedback& Feedback : Warnings) Add(Handle, Source, TEXT("warning"), Feedback.GetDescriptionText().ToString());
			for (const FNiagaraRendererFeedback& Feedback : Infos) Add(Handle, Source, TEXT("info"), Feedback.GetDescriptionText().ToString());
		}
		if (bStopped) break;
		if (Data->SimTarget == ENiagaraSimTarget::GPUComputeSim
			&& Data->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic && !System->bFixedBounds)
		{
			Add(Handle, TEXT("EmitterProperties"), TEXT("warning"), TEXT("GPU emitter uses dynamic bounds without system fixed bounds."));
		}
	}
#endif
	Json->SetArrayField(TEXT("rendererDiagnostics"), Rows);
	Json->SetBoolField(TEXT("rendererInspectionTruncated"), bStopped);
	Json->SetBoolField(TEXT("rendererFeedbackAvailable"), WITH_EDITOR != 0);
}

TSharedRef<FJsonObject> SummaryJson(UNiagaraSystem* System, const TArray<FScriptEntry>& Scripts, const FCompileSummary& Summary, bool bRendererDiagnostics)
{
	auto Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.compile-summary.v1"));
	Json->SetStringField(TEXT("system"), System->GetPathName());
	Json->SetStringField(TEXT("status"), Summary.Status);
	Json->SetBoolField(TEXT("compiled"), Summary.bCompiled);
	Json->SetBoolField(TEXT("hasError"), Summary.bHasError);
	Json->SetBoolField(TEXT("outstanding"), Summary.bOutstanding);
	Json->SetBoolField(TEXT("diagnosticsMayBeStale"), Summary.bStale);
	Json->SetNumberField(TEXT("scriptCount"), Scripts.Num());
	Json->SetNumberField(TEXT("requiredScriptCount"), Summary.RequiredScripts);
	FDiagnosticBudget Budget;
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FScriptEntry& Entry : Scripts)
	{
		if (!Budget.TakeRow())
		{
			break;
		}
		Rows.Add(MakeShared<FJsonValueObject>(ScriptJson(Entry, Budget)));
	}
	Json->SetArrayField(TEXT("scripts"), Rows);
	Json->SetBoolField(TEXT("scriptsTruncated"), Rows.Num() < Scripts.Num());
	if (bRendererDiagnostics) AddRendererDiagnostics(System, Budget, Json);
	Json->SetBoolField(TEXT("diagnosticsTruncated"), Budget.bTruncated);
	return Json;
}

FMCPToolResult CompileError(const TCHAR* Code, const FString& Message, const TSharedRef<FJsonObject>& Details)
{
	FMCPToolResult Result = Error(Message, Code, 409);
	Result.Data = Details;
	return Result;
}

class FCompileRequest final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.system.compile.request"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		bool bWait = true;
		bool bForce = false;
		if (!ReadBool(Params, TEXT("waitForCompletion"), true, bWait) || !ReadBool(Params, TEXT("force"), false, bForce))
		{
			return Error(TEXT("waitForCompletion and force must be booleans."));
		}
		UNiagaraSystem* System = nullptr;
		FMCPToolResult Result = LoadSystem(Params, System);
		if (!Result.bSuccess) return Result;
		TArray<FScriptEntry> Scripts;
		Result = CollectScripts(System, Scripts);
		if (!Result.bSuccess) return Result;
		if (!AllowShaderCompiling() || System->GetOutermost()->bIsCookedForEditor
			|| System->RootPackageHasAnyFlags(PKG_FilterEditorOnly) || (!GIsClient && GIsServer))
		{
			return Error(TEXT("The current editor or cooked package does not permit Niagara compilation."), TEXT("niagara_compile_unavailable"), 409);
		}
		// The bool reports whether a new compilation launched, not success. A
		// cache hit can finish immediately and valid up-to-date data is success.
		const bool bLaunched = System->RequestCompile(bForce);
		if (bWait)
		{
			// Drain VM work first. UE 5.4 snapshots pending GPU shaders on entry,
			// so only after VM work finishes can the GPU pass see new shader jobs.
			System->WaitForCompilationComplete(false, false);
			if (!System->HasOutstandingCompilationRequests(false))
			{
				System->WaitForCompilationComplete(true, false);
			}
		}
		const FCompileSummary Summary = Summarize(System, Scripts);
		auto Json = SummaryJson(System, Scripts, Summary, false);
		Json->SetBoolField(TEXT("compileRequested"), true);
		Json->SetBoolField(TEXT("compilationLaunched"), bLaunched);
		Json->SetBoolField(TEXT("waited"), bWait);
		Json->SetBoolField(TEXT("force"), bForce);
		Json->SetBoolField(TEXT("dirty"), System->GetOutermost()->IsDirty());
		if (bWait && Summary.bOutstanding)
		{
			return CompileError(TEXT("niagara_compile_pending"), TEXT("Compilation remains outstanding after the engine wait. Read system.diagnostics.get before retrying."), Json);
		}
		if (!Summary.bOutstanding && !Summary.bCompiled)
		{
			return CompileError(TEXT("niagara_compile_failed"), FString::Printf(TEXT("Niagara compilation has status '%s'. Read system.diagnostics.get for bounded per-script errors."), *Summary.Status), Json);
		}
		return FMCPToolResult::Ok(Json);
	}
};

class FDiagnostics final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.system.diagnostics.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		if (Params.IsValid() && Params->HasField(TEXT("compileFirst")))
		{
			return Error(TEXT("This query does not compile. Call content.niagara.system.compile.request explicitly first."));
		}
		UNiagaraSystem* System = nullptr;
		FMCPToolResult Result = LoadSystem(Params, System);
		if (!Result.bSuccess) return Result;
		TArray<FScriptEntry> Scripts;
		Result = CollectScripts(System, Scripts);
		if (!Result.bSuccess) return Result;
		return FMCPToolResult::Ok(SummaryJson(System, Scripts, Summarize(System, Scripts), true));
	}
};

class FGpuHlsl final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.emitter.gpu_hlsl.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		if (Params.IsValid() && Params->HasField(TEXT("compileFirst")))
		{
			return Error(TEXT("This query does not compile. Call content.niagara.system.compile.request explicitly first."));
		}
		FString Selector;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("emitter"), Selector)
			|| Selector.IsEmpty() || Selector.Len() > MaxPathCharacters)
		{
			return Error(TEXT("emitter must be an exact emitter handle name, unique instance name, or GUID of at most 2048 characters."));
		}
		int32 MaxCharacters;
		if (!ReadSourceLimit(Params, MaxCharacters))
		{
			return Error(TEXT("maxCharacters must be an integer from 1 to 4194304."));
		}
		UNiagaraSystem* System = nullptr;
		FMCPToolResult Result = LoadSystem(Params, System);
		if (!Result.bSuccess) return Result;
		FGuid SelectorGuid;
		const bool bGuid = FGuid::Parse(Selector, SelectorGuid);
		const FNiagaraEmitterHandle* Selected = nullptr;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			const bool bMatches = bGuid ? Handle.GetId() == SelectorGuid
				: Selector.Equals(Handle.GetName().ToString(), ESearchCase::CaseSensitive)
					|| Selector.Equals(Handle.GetUniqueInstanceName(), ESearchCase::CaseSensitive);
			if (!bMatches) continue;
			if (Selected)
			{
				return Error(TEXT("The emitter selector matches multiple handles. Use the handle GUID."), TEXT("emitter_ambiguous"), 409);
			}
			Selected = &Handle;
		}
		if (!Selected) return Error(TEXT("The emitter handle was not found in this Niagara System."), TEXT("emitter_not_found"), 404);
		const FVersionedNiagaraEmitterData* Data = Selected->GetEmitterData();
		if (!Data) return Error(TEXT("The selected emitter has no versioned data."), TEXT("emitter_data_missing"), 409);
		if (Data->SimTarget != ENiagaraSimTarget::GPUComputeSim)
		{
			return Error(TEXT("The selected emitter does not use GPU simulation."), TEXT("emitter_not_gpu"), 409);
		}
		const UNiagaraScript* Script = Data->GetGPUComputeScript();
		if (!Script) return Error(TEXT("The selected emitter has no GPU compute script."), TEXT("gpu_script_not_found"), 404);
		const FNiagaraVMExecutableData& VMData = Script->GetVMExecutableData();
		const FString* Source = &VMData.LastHlslTranslationGPU;
		const TCHAR* Format = TEXT("gpu_hlsl");
		if (Source->IsEmpty()) { Source = &VMData.LastHlslTranslation; Format = TEXT("hlsl"); }
		if (Source->IsEmpty()) { Source = &VMData.LastAssemblyTranslation; Format = TEXT("assembly"); }
		if (Source->IsEmpty())
		{
			return Error(FString::Printf(TEXT("No generated source is cached (status=%s). Read diagnostics or explicitly request compilation."), *CompileStatusName(Script->GetLastCompileStatus())), TEXT("gpu_source_unavailable"), 409);
		}
		const bool bOutstanding = System->HasOutstandingCompilationRequests(true);
		const bool bSynchronized = Script->AreScriptAndSourceSynchronized();
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.gpu-source.v1"));
		Json->SetStringField(TEXT("system"), System->GetPathName());
		Json->SetStringField(TEXT("emitter"), Selected->GetName().ToString());
		Json->SetStringField(TEXT("emitterId"), Selected->GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Json->SetStringField(TEXT("uniqueInstanceName"), Selected->GetUniqueInstanceName());
		Json->SetStringField(TEXT("script"), Script->GetPathName());
		Json->SetStringField(TEXT("status"), CompileStatusName(Script->GetLastCompileStatus()));
		Json->SetBoolField(TEXT("valid"), VMData.IsValid());
		Json->SetBoolField(TEXT("sourceSynchronized"), bSynchronized);
		Json->SetBoolField(TEXT("sourceMayBeStale"), bOutstanding || !bSynchronized || !IsUpToDate(Script->GetLastCompileStatus()));
		Json->SetBoolField(TEXT("compilationSucceeded"), Script->DidScriptCompilationSucceed(true));
		Json->SetBoolField(TEXT("outstanding"), bOutstanding);
		Json->SetStringField(TEXT("format"), Format);
		Json->SetStringField(TEXT("source"), Source->Left(MaxCharacters));
		Json->SetNumberField(TEXT("sourceCharacters"), Source->Len());
		Json->SetNumberField(TEXT("returnedCharacters"), FMath::Min(Source->Len(), MaxCharacters));
		Json->SetBoolField(TEXT("truncated"), Source->Len() > MaxCharacters);
		return FMCPToolResult::Ok(Json);
	}
};
}
}
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraCompileTools(FMCPToolRegistry& Registry)
{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
	Registry.Register(MakeShared<UEAINiagaraCompile::FCompileRequest>());
	Registry.Register(MakeShared<UEAINiagaraCompile::FDiagnostics>());
	Registry.Register(MakeShared<UEAINiagaraCompile::FGpuHlsl>());
#else
	class FUnavailableNiagaraCompile final : public FMCPToolBase
	{
	public:
		explicit FUnavailableNiagaraCompile(const TCHAR* InId) : Id(InId) {}
		FString GetCapabilityId() const override { return Id; }
		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			return FMCPToolResult::Error(TEXT("Niagara compile diagnostics are unavailable in this build."), TEXT("capability_unavailable"), 409);
		}
	private:
		FString Id;
	};
	for (const TCHAR* Id : { TEXT("content.niagara.system.compile.request"), TEXT("content.niagara.system.diagnostics.get"), TEXT("content.niagara.emitter.gpu_hlsl.get") })
	{
		Registry.Register(MakeShared<FUnavailableNiagaraCompile>(Id));
	}
#endif
}
}
