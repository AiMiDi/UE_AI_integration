#include "Infrastructure/ProductionRuntimeController.h"
#include "Infrastructure/Sha256.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/ProductionJobRuntime.h"
#include "Interfaces/IPluginManager.h"
#include "JsonUtils/JsonPointer.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace UEAIIntegration::Infrastructure
{
namespace
{
	const TSet<FString> AllowedScenarioActions = {
		TEXT("pie.start"),
		TEXT("pie.stop"),
		TEXT("pie.restart"),
		TEXT("pie.pause"),
		TEXT("pie.resume"),
		TEXT("wait"),
		TEXT("object.find"),
		TEXT("object.get"),
		TEXT("object.set"),
		TEXT("object.call"),
		TEXT("widget.tree"),
		TEXT("widget.state"),
		TEXT("delegate.bind"),
		TEXT("delegate.unbind"),
		TEXT("delegate.list"),
		TEXT("delegate.is_bound"),
		TEXT("delegate.broadcast"),
		TEXT("widget.click"),
		TEXT("widget.drag"),
		TEXT("widget.focus"),
		TEXT("widget.hitTest"),
		TEXT("input.key"),
		TEXT("input.mode"),
		TEXT("viewport.capture"),
		TEXT("log.capture"),
		TEXT("frameTime.sample"),
		TEXT("metrics.begin"),
		TEXT("metrics.end"),
	};

	const TSet<FString> AllowedAssertionTypes = {
		TEXT("equals"),
		TEXT("exists"),
		TEXT("widgetState"),
		TEXT("delegateBound"),
		TEXT("hitTestContains"),
		TEXT("logNotContains"),
		TEXT("imageDiff"),
	};

	FString NewOpaqueId(const TCHAR* Prefix)
	{
		return FString::Printf(
			TEXT("%s-%s"),
			Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	TSharedPtr<FJsonObject> CopyControllerJsonObject(
		const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		if (Source.IsValid())
		{
			Copy->Values = Source->Values;
		}
		return Copy;
	}

	bool ResolveScenarioValue(
		const TSharedPtr<FJsonValue>& Source,
		const TMap<FString, TSharedPtr<FJsonObject>>& StepResults,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		if (!Source.IsValid())
		{
			OutValue = MakeShared<FJsonValueNull>();
			return true;
		}

		if (Source->Type == EJson::String)
		{
			const FString Binding = Source->AsString();
			if (!Binding.StartsWith(TEXT("${")) || !Binding.EndsWith(TEXT("}")))
			{
				OutValue = Source;
				return true;
			}

			const FString Expression = Binding.Mid(2, Binding.Len() - 3);
			FString StepId;
			FString PointerPath;
			if (!Expression.Split(TEXT("#"), &StepId, &PointerPath)
				|| StepId.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Invalid scenario binding '%s'. Expected ${stepId#/json/pointer}."),
					*Binding);
				return false;
			}
			const TSharedPtr<FJsonObject>* StepResult = StepResults.Find(StepId);
			if (!StepResult || !StepResult->IsValid())
			{
				OutError = FString::Printf(
					TEXT("Scenario binding '%s' references an unavailable step."),
					*Binding);
				return false;
			}

			TSharedPtr<FJsonValue> Resolved;
			const UE::Json::FJsonPointer Pointer(
				PointerPath.IsEmpty()
					? TEXT("#")
					: FString(TEXT("#")) + PointerPath);
			if (!Pointer.TryGet(*StepResult, Resolved) || !Resolved.IsValid())
			{
				OutError = FString::Printf(
					TEXT("Scenario binding '%s' could not be resolved."),
					*Binding);
				return false;
			}
			OutValue = FJsonValue::Duplicate(Resolved);
			return true;
		}

		if (Source->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> ResolvedItems;
			for (const TSharedPtr<FJsonValue>& Item : Source->AsArray())
			{
				TSharedPtr<FJsonValue> ResolvedItem;
				if (!ResolveScenarioValue(
						Item,
						StepResults,
						ResolvedItem,
						OutError))
				{
					return false;
				}
				ResolvedItems.Add(ResolvedItem);
			}
			OutValue = MakeShared<FJsonValueArray>(ResolvedItems);
			return true;
		}

		if (Source->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> ResolvedObject = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
				Source->AsObject()->Values)
			{
				TSharedPtr<FJsonValue> ResolvedField;
				if (!ResolveScenarioValue(
						Pair.Value,
						StepResults,
						ResolvedField,
						OutError))
				{
					return false;
				}
				ResolvedObject->SetField(Pair.Key, ResolvedField);
			}
			OutValue = MakeShared<FJsonValueObject>(ResolvedObject);
			return true;
		}

		OutValue = Source;
		return true;
	}

	TSharedPtr<FJsonValue> ResolveActualValue(
		const TSharedPtr<FJsonObject>& Assertion,
		const TSharedPtr<FJsonObject>& StepResult)
	{
		const TSharedPtr<FJsonValue>* Actual = Assertion->Values.Find(TEXT("actual"));
		if (!Actual)
		{
			return MakeShared<FJsonValueObject>(StepResult);
		}

		if ((*Actual)->Type != EJson::String)
		{
			return *Actual;
		}

		const FString Path = (*Actual)->AsString();
		if (Path.StartsWith(TEXT("#/")) || Path.StartsWith(TEXT("/")))
		{
			const FString PointerPath =
				Path.StartsWith(TEXT("#")) ? Path : FString(TEXT("#")) + Path;
			TSharedPtr<FJsonValue> Resolved;
			const UE::Json::FJsonPointer Pointer(PointerPath);
			return Pointer.TryGet(StepResult, Resolved) ? Resolved : nullptr;
		}
		const FString Prefix(TEXT("$result."));
		if (!Path.StartsWith(Prefix))
		{
			return *Actual;
		}

		TSharedPtr<FJsonValue> Current = MakeShared<FJsonValueObject>(StepResult);
		TArray<FString> Segments;
		Path.RightChop(Prefix.Len()).ParseIntoArray(Segments, TEXT("."), true);
		for (const FString& Segment : Segments)
		{
			if (!Current.IsValid() || Current->Type != EJson::Object)
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject> Object = Current->AsObject();
			const TSharedPtr<FJsonValue>* Next = Object->Values.Find(Segment);
			if (!Next)
			{
				return nullptr;
			}
			Current = *Next;
		}
		return Current;
	}

	FString JsonValueToCompactString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		FString Output;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Output;
	}

	TSharedPtr<FJsonObject> MakeErrorObject(
		const FString& Code,
		const FString& Message)
	{
		TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		return Error;
	}

	FString GetStringFieldOr(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& DefaultValue)
	{
		FString Value;
		return Object.IsValid() && Object->TryGetStringField(Field, Value)
			? Value
			: DefaultValue;
	}
}

FProductionRuntimeController::FProductionRuntimeController(
	FMCPToolRegistry& InRegistry,
	FPIESessionController& InPIEController)
	: Registry(InRegistry)
	, PIEController(InPIEController)
	, InitializedAtUtc(FDateTime::UtcNow())
	, JobRuntime(
		MakeUnique<FProductionJobRuntime>(
			[this](const TSharedPtr<FJsonObject>& Params)
			{
				return StartScenario(Params);
			},
			[this](const TSharedPtr<FJsonObject>& Params)
			{
				return GetScenarioStatus(Params);
			},
			[this](const TSharedPtr<FJsonObject>& Params)
			{
				return GetScenarioResult(Params);
			},
			[this](const TSharedPtr<FJsonObject>& Params)
			{
				return CancelScenario(Params);
			}))
{
	IFileManager::Get().MakeDirectory(*ScenarioDirectory(), true);
}

FProductionRuntimeController::~FProductionRuntimeController()
{
#if WITH_LIVE_CODING
	if (LiveCodingPatchHandle.IsValid())
	{
		if (ILiveCodingModule* LiveCoding =
			FModuleManager::GetModulePtr<ILiveCodingModule>(
				LIVE_CODING_MODULE_NAME))
		{
			LiveCoding->GetOnPatchCompleteDelegate().Remove(
				LiveCodingPatchHandle);
		}
		LiveCodingPatchHandle.Reset();
	}
#endif
	for (TPair<FString, TSharedPtr<FBuildJob>>& Pair : BuildJobs)
	{
		if (!Pair.Value.IsValid())
		{
			continue;
		}
		if (Pair.Value->ProcessHandle.IsValid())
		{
			// Closing our handle does not terminate an externally running UBT process.
			FPlatformProcess::CloseProc(Pair.Value->ProcessHandle);
			Pair.Value->ProcessHandle.Reset();
		}
		if (Pair.Value->ReadPipe || Pair.Value->WritePipe)
		{
			FPlatformProcess::ClosePipe(
				Pair.Value->ReadPipe,
				Pair.Value->WritePipe);
			Pair.Value->ReadPipe = nullptr;
			Pair.Value->WritePipe = nullptr;
		}
	}
}

void FProductionRuntimeController::Tick(float DeltaTime)
{
	TickScenario();
	TickBuild();
	if (JobRuntime.IsValid())
	{
		JobRuntime->Tick(DeltaTime);
	}
}

FMCPToolResult FProductionRuntimeController::ExecuteProductionJobOperation(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params)
{
	if (!JobRuntime.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("The production job runtime is unavailable."),
			TEXT("job_runtime_unavailable"),
			503);
	}
	return JobRuntime->Execute(CapabilityId, Params);
}

FMCPToolResult FProductionRuntimeController::ValidateScenario(
	const TSharedPtr<FJsonObject>& Params) const
{
	TArray<FString> Errors;
	TSharedPtr<FJsonObject> Scenario;
	if (!Params.IsValid()
		|| !Params->HasTypedField<EJson::Object>(TEXT("scenario")))
	{
		Errors.Add(TEXT("scenario must be an object."));
	}
	else
	{
		Scenario = Params->GetObjectField(TEXT("scenario"));
		ValidateScenarioObject(Scenario, Errors);
	}

	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("valid"), Errors.IsEmpty());
	Data->SetArrayField(TEXT("errors"), ErrorValues);
	if (Scenario.IsValid())
	{
		Data->SetNumberField(
			TEXT("stepCount"),
			Scenario->GetArrayField(TEXT("steps")).Num());
	}
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionRuntimeController::StartScenario(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!ActiveScenarioId.IsEmpty())
	{
		const TSharedPtr<FScenarioRun>* Active = ScenarioRuns.Find(ActiveScenarioId);
		if (Active && Active->IsValid() && (*Active)->Status == TEXT("running"))
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Scenario '%s' is already running."),
					*ActiveScenarioId),
				TEXT("scenario_busy"),
				409);
		}
		ActiveScenarioId.Reset();
	}

	if (!Params.IsValid()
		|| !Params->HasTypedField<EJson::Object>(TEXT("scenario")))
	{
		return FMCPToolResult::Error(
			TEXT("scenario must be an object."),
			TEXT("invalid_params"),
			422);
	}

	const TSharedPtr<FJsonObject> Scenario = Params->GetObjectField(TEXT("scenario"));
	TArray<FString> Errors;
	ValidateScenarioObject(Scenario, Errors);
	if (!Errors.IsEmpty())
	{
		return FMCPToolResult::Error(
			FString::Join(Errors, TEXT(" ")),
			TEXT("invalid_params"),
			422);
	}

	TSharedPtr<FScenarioRun> Run = MakeShared<FScenarioRun>();
	Run->Id = NewOpaqueId(TEXT("scenario"));
	Run->Name = GetStringFieldOr(Scenario, TEXT("name"), Run->Id);
	Run->Status = TEXT("running");
	Run->Scenario = Scenario;
	Run->StartedAtSeconds = FPlatformTime::Seconds();
	const double TimeoutMs =
		Scenario->HasField(TEXT("timeoutMs"))
			? Scenario->GetNumberField(TEXT("timeoutMs"))
			: 300000.0;
	Run->DeadlineSeconds = Run->StartedAtSeconds + TimeoutMs / 1000.0;
	Run->LogPath = FindEditorLogPath();
	if (!Run->LogPath.IsEmpty())
	{
		Run->LogStartOffset =
			FMath::Max<int64>(
				0,
				IFileManager::Get().FileSize(*Run->LogPath));
	}

	if (Scenario->HasTypedField<EJson::Object>(TEXT("cleanup")))
	{
		const TSharedPtr<FJsonObject> Cleanup =
			Scenario->GetObjectField(TEXT("cleanup"));
		Cleanup->TryGetBoolField(TEXT("stopPie"), Run->bCleanupStopPIE);
	}

	while (ScenarioRuns.Num() >= 16)
	{
		FString OldestCompleted;
		double OldestStart = TNumericLimits<double>::Max();
		for (const TPair<FString, TSharedPtr<FScenarioRun>>& Pair : ScenarioRuns)
		{
			if (Pair.Value.IsValid()
				&& Pair.Value->Status != TEXT("running")
				&& Pair.Value->StartedAtSeconds < OldestStart)
			{
				OldestCompleted = Pair.Key;
				OldestStart = Pair.Value->StartedAtSeconds;
			}
		}
		if (OldestCompleted.IsEmpty())
		{
			break;
		}
		ScenarioRuns.Remove(OldestCompleted);
	}

	ActiveScenarioId = Run->Id;
	ScenarioRuns.Add(Run->Id, Run);
	return FMCPToolResult::Ok(MakeScenarioSummary(*Run));
}

FMCPToolResult FProductionRuntimeController::GetScenarioStatus(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = GetStringFieldOr(Params, TEXT("runId"), FString());
	const TSharedPtr<FScenarioRun>* Run = ScenarioRuns.Find(RunId);
	if (!Run || !Run->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Scenario run '%s' was not found."), *RunId),
			TEXT("runtime_object_not_found"),
			404);
	}
	return FMCPToolResult::Ok(MakeScenarioSummary(**Run));
}

FMCPToolResult FProductionRuntimeController::CancelScenario(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString RunId = GetStringFieldOr(Params, TEXT("runId"), FString());
	const TSharedPtr<FScenarioRun>* Run = ScenarioRuns.Find(RunId);
	if (!Run || !Run->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Scenario run '%s' was not found."), *RunId),
			TEXT("runtime_object_not_found"),
			404);
	}
	if ((*Run)->Status == TEXT("running"))
	{
		(*Run)->bCancelRequested = true;
	}
	return FMCPToolResult::Ok(MakeScenarioSummary(**Run));
}

FMCPToolResult FProductionRuntimeController::GetScenarioResult(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = GetStringFieldOr(Params, TEXT("runId"), FString());
	const TSharedPtr<FScenarioRun>* Run = ScenarioRuns.Find(RunId);
	if (!Run || !Run->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Scenario run '%s' was not found."), *RunId),
			TEXT("runtime_object_not_found"),
			404);
	}

	TSharedPtr<FJsonObject> Data = MakeScenarioSummary(**Run);
	Data->SetArrayField(TEXT("steps"), (*Run)->StepReceipts);
	TArray<TSharedPtr<FJsonValue>> ArtifactValues;
	for (const TPair<FString, FScenarioArtifact>& Pair : (*Run)->Artifacts)
	{
		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("artifactId"), Pair.Key);
		Artifact->SetStringField(TEXT("mime_type"), Pair.Value.MimeType);
		Artifact->SetStringField(TEXT("path"), Pair.Value.Path);
		ArtifactValues.Add(MakeShared<FJsonValueObject>(Artifact));
	}
	Data->SetArrayField(TEXT("artifacts"), ArtifactValues);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionRuntimeController::GetScenarioArtifact(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = GetStringFieldOr(Params, TEXT("runId"), FString());
	const FString ArtifactId =
		GetStringFieldOr(Params, TEXT("artifactId"), FString());
	const TSharedPtr<FScenarioRun>* Run = ScenarioRuns.Find(RunId);
	if (!Run || !Run->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Scenario run '%s' was not found."), *RunId),
			TEXT("runtime_object_not_found"),
			404);
	}
	const FScenarioArtifact* Artifact = (*Run)->Artifacts.Find(ArtifactId);
	if (!Artifact)
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Artifact '%s' was not found."), *ArtifactId),
			TEXT("runtime_object_not_found"),
			404);
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Artifact->Path))
	{
		return FMCPToolResult::Error(
			TEXT("Scenario artifact could not be read."),
			TEXT("verification_failed"),
			500);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("image_base64"), FBase64::Encode(Bytes));
	Data->SetStringField(TEXT("mime_type"), Artifact->MimeType);
	Data->SetObjectField(
		TEXT("metadata"),
		Artifact->Metadata.IsValid()
			? Artifact->Metadata
			: MakeShared<FJsonObject>());
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionRuntimeController::GetLoadedModule(
	const TSharedPtr<FJsonObject>& Params) const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("plugin"), TEXT("UE_AI_integration"));
	Data->SetStringField(TEXT("module"), TEXT("UE_AI_integration"));

	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	if (!Plugin.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("UE_AI_integration plugin descriptor was not found."),
			TEXT("build_unavailable"),
			503);
	}
	Data->SetStringField(TEXT("pluginVersion"), Plugin->GetDescriptor().VersionName);
	Data->SetStringField(TEXT("pluginBaseDir"), Plugin->GetBaseDir());

	FModuleManager& Modules = FModuleManager::Get();
	const FName ModuleName(TEXT("UE_AI_integration"));
	const bool bLoaded = Modules.IsModuleLoaded(ModuleName);
	Data->SetBoolField(TEXT("loaded"), bLoaded);
	const FString ModulePath = Modules.GetModuleFilename(ModuleName);
	Data->SetStringField(TEXT("modulePath"), ModulePath);

	const FString PdbPath = FPaths::ChangeExtension(ModulePath, TEXT("pdb"));
	Data->SetObjectField(TEXT("dll"), MakeFileProvenance(ModulePath));
	Data->SetObjectField(TEXT("pdb"), MakeFileProvenance(PdbPath));
	Data->SetStringField(TEXT("editorStartedAtUtc"), GetEditorStartTimeUtc());
	Data->SetStringField(TEXT("controllerInitializedAtUtc"), InitializedAtUtc.ToIso8601());

	TArray<FString> CandidateDlls;
	IFileManager::Get().FindFilesRecursive(
		CandidateDlls,
		*Plugin->GetBaseDir(),
		TEXT("*UE_AI_integration*.dll"),
		true,
		false);
	FString LatestPath;
	FDateTime LatestTimestamp;
	bool bFoundLatest = false;
	for (const FString& Candidate : CandidateDlls)
	{
		const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Candidate);
		if (!bFoundLatest || Timestamp > LatestTimestamp)
		{
			bFoundLatest = true;
			LatestTimestamp = Timestamp;
			LatestPath = Candidate;
		}
	}
	Data->SetStringField(TEXT("latestBuildArtifactPath"), LatestPath);
	const FString LoadedHash = ComputeFileSha256(ModulePath);
	const FString LatestHash = ComputeFileSha256(LatestPath);
	Data->SetStringField(TEXT("latestBuildArtifactSha256"), LatestHash);
	Data->SetBoolField(
		TEXT("matchesLatestBuildArtifact"),
		!LoadedHash.IsEmpty() && LoadedHash == LatestHash);

	TSharedPtr<FJsonObject> LiveCoding = MakeShared<FJsonObject>();
#if WITH_LIVE_CODING
	if (ILiveCodingModule* Module =
			FModuleManager::GetModulePtr<ILiveCodingModule>(
				LIVE_CODING_MODULE_NAME))
	{
		LiveCoding->SetBoolField(TEXT("available"), true);
		LiveCoding->SetBoolField(TEXT("started"), Module->HasStarted());
		LiveCoding->SetBoolField(
			TEXT("enabledForSession"),
			Module->IsEnabledForSession());
		LiveCoding->SetBoolField(TEXT("compiling"), Module->IsCompiling());
	}
	else
	{
		LiveCoding->SetBoolField(TEXT("available"), false);
	}
#else
	LiveCoding->SetBoolField(TEXT("available"), false);
#endif
	LiveCoding->SetStringField(TEXT("lastPatchResult"), LastLiveCodingPatchResult);
	Data->SetObjectField(TEXT("liveCoding"), LiveCoding);

	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FProductionRuntimeController::StartBuild(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bConfirmBuild = false;
	if (!Params.IsValid()
		|| !Params->TryGetBoolField(TEXT("confirmBuild"), bConfirmBuild)
		|| !bConfirmBuild)
	{
		return FMCPToolResult::Error(
			TEXT("confirmBuild:true is required."),
			TEXT("invalid_params"),
			422);
	}

	if (!ActiveBuildJobId.IsEmpty())
	{
		const TSharedPtr<FBuildJob>* Active = BuildJobs.Find(ActiveBuildJobId);
		if (Active && Active->IsValid() && (*Active)->Status == TEXT("running"))
		{
			return FMCPToolResult::Error(
				TEXT("A build job is already active."),
				TEXT("build_unavailable"),
				409);
		}
		ActiveBuildJobId.Reset();
	}

	const FString Mode = GetStringFieldOr(Params, TEXT("mode"), FString());
	if (Mode != TEXT("liveCoding") && Mode != TEXT("ubt"))
	{
		return FMCPToolResult::Error(
			TEXT("mode must be 'liveCoding' or 'ubt'."),
			TEXT("invalid_params"),
			422);
	}

	TSharedPtr<FBuildJob> Job = MakeShared<FBuildJob>();
	Job->Id = NewOpaqueId(TEXT("build"));
	Job->Mode = Mode;
	Job->Status = TEXT("running");
	Job->StartedAtUtc = FDateTime::UtcNow().ToIso8601();

	if (Mode == TEXT("liveCoding"))
	{
#if WITH_LIVE_CODING
		ILiveCodingModule* LiveCoding =
			FModuleManager::LoadModulePtr<ILiveCodingModule>(
				LIVE_CODING_MODULE_NAME);
		if (!LiveCoding || !LiveCoding->CanEnableForSession())
		{
			return FMCPToolResult::Error(
				TEXT("Live Coding is unavailable for this Editor session."),
				TEXT("build_unavailable"),
				503);
		}
		if (!LiveCoding->IsEnabledForSession())
		{
			LiveCoding->EnableForSession(true);
		}
		if (!LiveCoding->IsEnabledForSession())
		{
			return FMCPToolResult::Error(
				LiveCoding->GetEnableErrorText().ToString(),
				TEXT("build_unavailable"),
				503);
		}

		if (LiveCodingPatchHandle.IsValid())
		{
			LiveCoding->GetOnPatchCompleteDelegate().Remove(
				LiveCodingPatchHandle);
			LiveCodingPatchHandle.Reset();
		}
		LiveCodingPatchHandle =
			LiveCoding->GetOnPatchCompleteDelegate().AddLambda(
				[Job]()
				{
					Job->bLiveCodingPatchCompleted = true;
				});

		ELiveCodingCompileResult CompileResult =
			ELiveCodingCompileResult::NotStarted;
		if (!LiveCoding->Compile(
				ELiveCodingCompileFlags::None,
				&CompileResult))
		{
			LiveCoding->GetOnPatchCompleteDelegate().Remove(
				LiveCodingPatchHandle);
			LiveCodingPatchHandle.Reset();
			return FMCPToolResult::Error(
				TEXT("Live Coding rejected the compile request."),
				TEXT("build_unavailable"),
				503);
		}
		if (CompileResult == ELiveCodingCompileResult::Success
			|| CompileResult == ELiveCodingCompileResult::NoChanges)
		{
			LiveCoding->GetOnPatchCompleteDelegate().Remove(
				LiveCodingPatchHandle);
			LiveCodingPatchHandle.Reset();
			Job->Status = TEXT("succeeded");
			Job->CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
			LastLiveCodingPatchResult =
				CompileResult == ELiveCodingCompileResult::Success
					? TEXT("success")
					: TEXT("no_changes");
		}
		else if (CompileResult != ELiveCodingCompileResult::InProgress)
		{
			LiveCoding->GetOnPatchCompleteDelegate().Remove(
				LiveCodingPatchHandle);
			LiveCodingPatchHandle.Reset();
			return FMCPToolResult::Error(
				TEXT("Live Coding could not start a compile."),
				TEXT("build_unavailable"),
				503);
		}
#else
		return FMCPToolResult::Error(
			TEXT("Live Coding is not available on this platform."),
			TEXT("build_unavailable"),
			503);
#endif
	}
	else
	{
		const FString UbtPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::EngineDir(),
				TEXT("Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.exe")));
		const FString ProjectFile =
			FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		if (!IFileManager::Get().FileExists(*UbtPath)
			|| ProjectFile.IsEmpty()
			|| !IFileManager::Get().FileExists(*ProjectFile))
		{
			return FMCPToolResult::Error(
				TEXT("Current Engine UBT or current .uproject is unavailable."),
				TEXT("build_unavailable"),
				503);
		}

		const FString Target =
			FString::Printf(TEXT("%sEditor"), FApp::GetProjectName());
		const FString ModuleName = TEXT("UE_AI_integration");
		const TSharedPtr<IPlugin> Plugin =
			IPluginManager::Get().FindPlugin(ModuleName);
		if (!Plugin.IsValid())
		{
			return FMCPToolResult::Error(
				TEXT("UE_AI_integration plugin descriptor was not found."),
				TEXT("build_unavailable"),
				503);
		}
		TArray<FString> ExistingDlls;
		IFileManager::Get().FindFilesRecursive(
			ExistingDlls,
			*FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries")),
			TEXT("*UE_AI_integration*.dll"),
			true,
			false);
		for (const FString& ExistingDll : ExistingDlls)
		{
			const FString FullPath =
				FPaths::ConvertRelativePathToFull(ExistingDll);
			Job->ExistingDllTimestamps.Add(
				FullPath,
				IFileManager::Get().GetTimeStamp(*FullPath));
		}

		const FString Arguments = FString::Printf(
			TEXT("%s Win64 Development -Project=\"%s\" -Module=%s -ForceHotReload -NoEngineChanges -FailIfGeneratedCodeChanges -WaitMutex -IgnoreJunk"),
			*Target,
			*ProjectFile,
			*ModuleName);
		if (!FPlatformProcess::CreatePipe(Job->ReadPipe, Job->WritePipe))
		{
			return FMCPToolResult::Error(
				TEXT("Failed to create the UBT output pipe."),
				TEXT("build_unavailable"),
				503);
		}
		Job->LogPath = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UE_AI_integration/Builds"),
			Job->Id + TEXT(".log"));
		Job->LoadedPdbPath = FPaths::ChangeExtension(
			FModuleManager::Get().GetModuleFilename(FName(*ModuleName)),
			TEXT("pdb"));
		if (IFileManager::Get().FileExists(*Job->LoadedPdbPath))
		{
			Job->LoadedPdbBackupPath = FPaths::Combine(
				FPaths::GetPath(Job->LogPath),
				Job->Id + TEXT(".loaded.pdb"));
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(Job->LoadedPdbBackupPath),
				true);
			if (IFileManager::Get().Copy(
					*Job->LoadedPdbBackupPath,
					*Job->LoadedPdbPath,
					true,
					true)
				!= COPY_OK)
			{
				FPlatformProcess::ClosePipe(Job->ReadPipe, Job->WritePipe);
				Job->ReadPipe = nullptr;
				Job->WritePipe = nullptr;
				return FMCPToolResult::Error(
					TEXT("Failed to preserve the loaded module PDB before UBT."),
					TEXT("build_unavailable"),
					503);
			}
		}
		uint32 ProcessId = 0;
		FProcHandle Handle;
#if PLATFORM_WINDOWS
		Handle = FPlatformProcess::CreateProc(
			*UbtPath,
			*Arguments,
			false,
			true,
			true,
			&ProcessId,
			0,
			*FPaths::GetPath(ProjectFile),
			Job->WritePipe,
			nullptr,
			Job->WritePipe);
#else
		Handle = FPlatformProcess::CreateProc(
			*UbtPath,
			*Arguments,
			false,
			true,
			true,
			&ProcessId,
			0,
			*FPaths::GetPath(ProjectFile),
			Job->WritePipe,
			nullptr);
#endif
		if (!Handle.IsValid())
		{
			if (!Job->LoadedPdbBackupPath.IsEmpty())
			{
				IFileManager::Get().Delete(
					*Job->LoadedPdbBackupPath,
					false,
					true,
					true);
			}
			FPlatformProcess::ClosePipe(Job->ReadPipe, Job->WritePipe);
			Job->ReadPipe = nullptr;
			Job->WritePipe = nullptr;
			return FMCPToolResult::Error(
				TEXT("Failed to start the current project's Editor target build."),
				TEXT("build_unavailable"),
				503);
		}
		Job->Executable = UbtPath;
		Job->Arguments = Arguments;
		Job->ProcessId = ProcessId;
		Job->ProcessHandle = Handle;
	}

	BuildJobs.Add(Job->Id, Job);
	if (Job->Status == TEXT("running"))
	{
		ActiveBuildJobId = Job->Id;
	}
	return FMCPToolResult::Ok(MakeBuildSummary(*Job));
}

FMCPToolResult FProductionRuntimeController::GetBuildJob(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = GetStringFieldOr(Params, TEXT("jobId"), FString());
	const TSharedPtr<FBuildJob>* Job = BuildJobs.Find(JobId);
	if (!Job || !Job->IsValid())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Build job '%s' was not found."), *JobId),
			TEXT("runtime_object_not_found"),
			404);
	}
	return FMCPToolResult::Ok(MakeBuildSummary(**Job));
}

FMCPToolResult FProductionRuntimeController::ValidateScenarioObject(
	const TSharedPtr<FJsonObject>& Scenario,
	TArray<FString>& OutErrors) const
{
	if (!Scenario.IsValid()
		|| !Scenario->HasTypedField<EJson::Array>(TEXT("steps")))
	{
		OutErrors.Add(TEXT("scenario.steps must be a non-empty array."));
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}

	const TArray<TSharedPtr<FJsonValue>>& Steps =
		Scenario->GetArrayField(TEXT("steps"));
	if (Steps.IsEmpty())
	{
		OutErrors.Add(TEXT("scenario.steps must contain at least one step."));
	}
	if (Steps.Num() > 256)
	{
		OutErrors.Add(TEXT("scenario.steps may contain at most 256 steps."));
	}

	FString ScenarioName;
	if (!Scenario->TryGetStringField(TEXT("name"), ScenarioName)
		|| ScenarioName.IsEmpty()
		|| ScenarioName.Len() > 128)
	{
		OutErrors.Add(TEXT("scenario.name must contain between 1 and 128 characters."));
	}
	if (Scenario->HasField(TEXT("timeoutMs")))
	{
		const double TimeoutMs = Scenario->GetNumberField(TEXT("timeoutMs"));
		if (TimeoutMs < 1.0 || TimeoutMs > 3600000.0)
		{
			OutErrors.Add(TEXT("scenario.timeoutMs must be between 1 and 3600000."));
		}
	}

	TSet<FString> StepIds;
	int32 MetricsBeginCount = 0;
	int32 MetricsEndCount = 0;
	int32 MetricsBeginIndex = INDEX_NONE;
	int32 MetricsEndIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		if (!Steps[Index].IsValid() || Steps[Index]->Type != EJson::Object)
		{
			OutErrors.Add(
				FString::Printf(TEXT("steps[%d] must be an object."), Index));
			continue;
		}
		const TSharedPtr<FJsonObject> Step = Steps[Index]->AsObject();
		FString StepId;
		FString Action;
		if (!Step->TryGetStringField(TEXT("id"), StepId) || StepId.IsEmpty())
		{
			OutErrors.Add(
				FString::Printf(TEXT("steps[%d].id is required."), Index));
		}
		else if (StepIds.Contains(StepId))
		{
			OutErrors.Add(
				FString::Printf(TEXT("Duplicate step id '%s'."), *StepId));
		}
		else
		{
			StepIds.Add(StepId);
		}
		if (StepId.Len() > 128)
		{
			OutErrors.Add(
				FString::Printf(TEXT("steps[%d].id is too long."), Index));
		}
		if (!Step->TryGetStringField(TEXT("action"), Action)
			|| !AllowedScenarioActions.Contains(Action))
		{
			OutErrors.Add(
				FString::Printf(
					TEXT("steps[%d].action is not supported."),
					Index));
		}
		else if (Action == TEXT("metrics.begin"))
		{
			++MetricsBeginCount;
			MetricsBeginIndex = Index;
		}
		else if (Action == TEXT("metrics.end"))
		{
			++MetricsEndCount;
			MetricsEndIndex = Index;
		}

		if (Step->HasField(TEXT("assertions")))
		{
			if (!Step->HasTypedField<EJson::Array>(TEXT("assertions")))
			{
				OutErrors.Add(
					FString::Printf(
						TEXT("steps[%d].assertions must be an array."),
						Index));
				continue;
			}
			if (Step->GetArrayField(TEXT("assertions")).Num() > 64)
			{
				OutErrors.Add(
					FString::Printf(
						TEXT("steps[%d].assertions may contain at most 64 entries."),
						Index));
			}
			for (const TSharedPtr<FJsonValue>& AssertionValue :
				Step->GetArrayField(TEXT("assertions")))
			{
				if (!AssertionValue.IsValid()
					|| AssertionValue->Type != EJson::Object)
				{
					OutErrors.Add(
						FString::Printf(
							TEXT("steps[%d] contains a non-object assertion."),
							Index));
					continue;
				}
				FString Type;
				if (!AssertionValue->AsObject()->TryGetStringField(
						TEXT("type"),
						Type)
					|| !AllowedAssertionTypes.Contains(Type))
				{
					OutErrors.Add(
						FString::Printf(
							TEXT("steps[%d] contains an unsupported assertion."),
							Index));
				}
			}
		}
		if (Step->HasField(TEXT("timeoutMs")))
		{
			const double TimeoutMs = Step->GetNumberField(TEXT("timeoutMs"));
			if (TimeoutMs < 1.0 || TimeoutMs > 300000.0)
			{
				OutErrors.Add(
					FString::Printf(
						TEXT("steps[%d].timeoutMs is outside the supported range."),
						Index));
			}
		}
	}
	if (MetricsBeginCount != MetricsEndCount
		|| MetricsBeginCount > 1
		|| (MetricsBeginCount == 1
			&& MetricsBeginIndex >= MetricsEndIndex))
	{
		OutErrors.Add(
			TEXT("Scenario metrics markers must contain at most one metrics.begin followed by exactly one metrics.end."));
	}
	return FMCPToolResult::Ok(MakeShared<FJsonObject>());
}

void FProductionRuntimeController::TickScenario()
{
	if (ActiveScenarioId.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FScenarioRun>* RunPtr =
		ScenarioRuns.Find(ActiveScenarioId);
	if (!RunPtr || !RunPtr->IsValid())
	{
		ActiveScenarioId.Reset();
		return;
	}
	FScenarioRun& Run = **RunPtr;
	if (Run.Status != TEXT("running"))
	{
		ActiveScenarioId.Reset();
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Run.bCancelRequested)
	{
		FinishScenario(
			Run,
			TEXT("cancelled"),
			TEXT("scenario_cancelled"),
			TEXT("Scenario was cancelled."));
		return;
	}
	if (Now >= Run.DeadlineSeconds)
	{
		FinishScenario(
			Run,
			TEXT("failed"),
			TEXT("scenario_timeout"),
			TEXT("Scenario exceeded its timeout."));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>& Steps =
		Run.Scenario->GetArrayField(TEXT("steps"));
	if (Run.StepIndex >= Steps.Num())
	{
		FinishScenario(Run, TEXT("succeeded"));
		return;
	}

	const TSharedPtr<FJsonObject> Step = Steps[Run.StepIndex]->AsObject();
	if (Run.StepStartedAtSeconds <= 0.0)
	{
		Run.StepStartedAtSeconds = Now;
	}

	const FString Action =
		GetStringFieldOr(Step, TEXT("action"), FString());
	if (Action == TEXT("wait"))
	{
		if (Run.WaitUntilSeconds <= 0.0)
		{
			double DurationMs = 0.0;
			if (Step->HasTypedField<EJson::Object>(TEXT("params")))
			{
				const TSharedPtr<FJsonObject> StepParams =
					Step->GetObjectField(TEXT("params"));
				if (StepParams->HasField(TEXT("durationMs")))
				{
					DurationMs =
						StepParams->GetNumberField(TEXT("durationMs"));
				}
			}
			Run.WaitUntilSeconds = Now + FMath::Max(0.0, DurationMs) / 1000.0;
		}
		if (Now < Run.WaitUntilSeconds)
		{
			return;
		}

		TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
		Receipt->SetStringField(
			TEXT("id"),
			GetStringFieldOr(Step, TEXT("id"), FString()));
		Receipt->SetStringField(TEXT("action"), Action);
		Receipt->SetBoolField(TEXT("ok"), true);
		Receipt->SetNumberField(
			TEXT("elapsedMs"),
			(Now - Run.StepStartedAtSeconds) * 1000.0);
		Run.StepReceipts.Add(MakeShared<FJsonValueObject>(Receipt));
		++Run.StepIndex;
		Run.StepStartedAtSeconds = 0.0;
		Run.WaitUntilSeconds = 0.0;
		return;
	}

	FString ErrorCode;
	FString ErrorMessage;
	bool bShouldRetry = false;
	if (!ExecuteScenarioStep(
			Run,
			Step,
			ErrorCode,
			ErrorMessage,
			bShouldRetry))
	{
		const double StepTimeoutMs =
			Step->HasField(TEXT("timeoutMs"))
				? Step->GetNumberField(TEXT("timeoutMs"))
				: 10000.0;
		if (bShouldRetry
			&& (Now - Run.StepStartedAtSeconds) * 1000.0 < StepTimeoutMs)
		{
			return;
		}
		FinishScenario(
			Run,
			TEXT("failed"),
			ErrorCode.IsEmpty() ? TEXT("verification_failed") : ErrorCode,
			ErrorMessage);
	}
}

void FProductionRuntimeController::TickBuild()
{
	if (ActiveBuildJobId.IsEmpty())
	{
		return;
	}
	const TSharedPtr<FBuildJob>* JobPtr = BuildJobs.Find(ActiveBuildJobId);
	if (!JobPtr || !JobPtr->IsValid())
	{
		ActiveBuildJobId.Reset();
		return;
	}
	FBuildJob& Job = **JobPtr;
	if (Job.Status != TEXT("running"))
	{
		ActiveBuildJobId.Reset();
		return;
	}

	if (Job.Mode == TEXT("liveCoding"))
	{
#if WITH_LIVE_CODING
		ILiveCodingModule* LiveCoding =
			FModuleManager::GetModulePtr<ILiveCodingModule>(
				LIVE_CODING_MODULE_NAME);
		if (LiveCoding && LiveCoding->IsCompiling())
		{
			return;
		}
		Job.Status = Job.bLiveCodingPatchCompleted
			? TEXT("succeeded")
			: TEXT("indeterminate");
		if (!Job.bLiveCodingPatchCompleted)
		{
			Job.Message =
				TEXT("Live Coding finished without a patch event; UE 5.3 cannot distinguish no-changes from a failed asynchronous compile through the public API.");
		}
		Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		LastLiveCodingPatchResult = Job.bLiveCodingPatchCompleted
			? TEXT("success")
			: TEXT("no_patch_event");
		if (LiveCodingPatchHandle.IsValid())
		{
			if (LiveCoding)
			{
				LiveCoding->GetOnPatchCompleteDelegate().Remove(
					LiveCodingPatchHandle);
			}
			LiveCodingPatchHandle.Reset();
		}
#else
		Job.Status = TEXT("failed");
		Job.Message = TEXT("Live Coding became unavailable.");
		Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		LastLiveCodingPatchResult = TEXT("unavailable");
#endif
		ActiveBuildJobId.Reset();
		return;
	}

	if (Job.ReadPipe)
	{
		Job.Output += FPlatformProcess::ReadPipe(Job.ReadPipe);
		constexpr int32 MaxCapturedBuildOutput = 262144;
		if (Job.Output.Len() > MaxCapturedBuildOutput)
		{
			Job.Output.RightInline(MaxCapturedBuildOutput, false);
		}
	}

	if (!Job.ProcessHandle.IsValid())
	{
		Job.Status = TEXT("failed");
		Job.Message = TEXT("UBT process handle was lost.");
		Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
		if (!Job.LogPath.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(Job.LogPath),
				true);
			FFileHelper::SaveStringToFile(Job.Output, *Job.LogPath);
		}
		if (Job.ReadPipe || Job.WritePipe)
		{
			FPlatformProcess::ClosePipe(Job.ReadPipe, Job.WritePipe);
			Job.ReadPipe = nullptr;
			Job.WritePipe = nullptr;
		}
		ActiveBuildJobId.Reset();
		return;
	}
	if (FPlatformProcess::IsProcRunning(Job.ProcessHandle))
	{
		return;
	}

	int32 ReturnCode = INDEX_NONE;
	FPlatformProcess::GetProcReturnCode(Job.ProcessHandle, &ReturnCode);
	Job.ReturnCode = ReturnCode;
	Job.Status = ReturnCode == 0 ? TEXT("succeeded") : TEXT("failed");
	Job.CompletedAtUtc = FDateTime::UtcNow().ToIso8601();
	FPlatformProcess::CloseProc(Job.ProcessHandle);
	Job.ProcessHandle.Reset();
	if (!Job.LogPath.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Job.LogPath), true);
		FFileHelper::SaveStringToFile(Job.Output, *Job.LogPath);
	}
	if (Job.ReadPipe || Job.WritePipe)
	{
		FPlatformProcess::ClosePipe(Job.ReadPipe, Job.WritePipe);
		Job.ReadPipe = nullptr;
		Job.WritePipe = nullptr;
	}

	bool bPdbRestored = true;
	if (!Job.LoadedPdbBackupPath.IsEmpty()
		&& IFileManager::Get().FileExists(*Job.LoadedPdbBackupPath))
	{
		if (!IFileManager::Get().FileExists(*Job.LoadedPdbPath))
		{
			bPdbRestored =
				IFileManager::Get().Copy(
					*Job.LoadedPdbPath,
					*Job.LoadedPdbBackupPath,
					true,
					true)
				== COPY_OK;
		}
		if (bPdbRestored)
		{
			IFileManager::Get().Delete(
				*Job.LoadedPdbBackupPath,
				false,
				true,
				true);
		}
	}
	if (!bPdbRestored)
	{
		Job.Status = TEXT("failed");
		Job.Message =
			TEXT("UBT completed, but the loaded module PDB could not be restored.");
	}

	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	FString BuiltDllPath;
	FDateTime BuiltDllTimestamp;
	if (ReturnCode == 0 && Plugin.IsValid())
	{
		TArray<FString> CandidateDlls;
		IFileManager::Get().FindFilesRecursive(
			CandidateDlls,
			*FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries")),
			TEXT("*UE_AI_integration*.dll"),
			true,
			false);
		for (const FString& Candidate : CandidateDlls)
		{
			const FString FullPath =
				FPaths::ConvertRelativePathToFull(Candidate);
			const FDateTime Timestamp =
				IFileManager::Get().GetTimeStamp(*FullPath);
			const FDateTime* ExistingTimestamp =
				Job.ExistingDllTimestamps.Find(FullPath);
			if ((!ExistingTimestamp || Timestamp > *ExistingTimestamp)
				&& (BuiltDllPath.IsEmpty()
					|| Timestamp > BuiltDllTimestamp))
			{
				BuiltDllPath = FullPath;
				BuiltDllTimestamp = Timestamp;
			}
		}
	}
	if (!BuiltDllPath.IsEmpty())
	{
		Job.BuiltDll = MakeFileProvenance(BuiltDllPath);
		Job.BuiltPdb = MakeFileProvenance(
			FPaths::ChangeExtension(BuiltDllPath, TEXT("pdb")));
	}
	else if (ReturnCode == 0 && Job.Message.IsEmpty())
	{
		Job.Message =
			TEXT("Target is up to date; no new module artifact was produced.");
	}
	ActiveBuildJobId.Reset();
}

void FProductionRuntimeController::FinishScenario(
	FScenarioRun& Run,
	const FString& Status,
	const FString& ErrorCode,
	const FString& ErrorMessage)
{
	Run.Status = Status;
	Run.ErrorCode = ErrorCode;
	Run.ErrorMessage = ErrorMessage;
	if (Run.bCleanupStopPIE)
	{
		PIEController.Stop();
	}
	CaptureScenarioLogWindow(Run);
	WriteScenarioReceipt(Run);
	if (ActiveScenarioId == Run.Id)
	{
		ActiveScenarioId.Reset();
	}
}

bool FProductionRuntimeController::ExecuteScenarioStep(
	FScenarioRun& Run,
	const TSharedPtr<FJsonObject>& Step,
	FString& OutErrorCode,
	FString& OutErrorMessage,
	bool& bOutShouldRetry)
{
	const FString StepId = GetStringFieldOr(Step, TEXT("id"), FString());
	const FString Action = GetStringFieldOr(Step, TEXT("action"), FString());
	TSharedPtr<FJsonObject> ToolParams = MakeShared<FJsonObject>();
	if (Step->HasTypedField<EJson::Object>(TEXT("params")))
	{
		ToolParams =
			CopyControllerJsonObject(
				Step->GetObjectField(TEXT("params")));
	}
	TSharedPtr<FJsonValue> ResolvedParamsValue;
	if (!ResolveScenarioValue(
			MakeShared<FJsonValueObject>(ToolParams),
			Run.StepResults,
			ResolvedParamsValue,
			OutErrorMessage))
	{
		OutErrorCode = TEXT("invalid_params");
		return false;
	}
	ToolParams = ResolvedParamsValue->AsObject();

	TSharedPtr<FJsonObject> StepResult;
	if (Action == TEXT("metrics.begin"))
	{
		if (Run.bMetricsActive || Run.MetricsBeginCount > 0)
		{
			OutErrorCode = TEXT("performance_markers_invalid");
			OutErrorMessage =
				TEXT("metrics.begin may execute only once per scenario.");
			return false;
		}
		Run.bMetricsActive = true;
		Run.MetricsBeginCount = 1;
		Run.MetricsStartedAtSeconds = FPlatformTime::Seconds();
		StepResult = MakeShared<FJsonObject>();
		StepResult->SetBoolField(TEXT("active"), true);
		StepResult->SetNumberField(TEXT("beginCount"), 1);
	}
	else if (Action == TEXT("metrics.end"))
	{
		if (!Run.bMetricsActive
			|| Run.MetricsBeginCount != 1
			|| Run.MetricsEndCount > 0)
		{
			OutErrorCode = TEXT("performance_markers_invalid");
			OutErrorMessage =
				TEXT("metrics.end requires one active metrics.begin marker.");
			return false;
		}
		Run.bMetricsActive = false;
		Run.MetricsEndCount = 1;
		Run.MetricsEndedAtSeconds = FPlatformTime::Seconds();
		StepResult = MakeShared<FJsonObject>();
		StepResult->SetBoolField(TEXT("active"), false);
		StepResult->SetNumberField(TEXT("endCount"), 1);
		StepResult->SetNumberField(
			TEXT("durationMs"),
			FMath::Max(
				0.0,
				Run.MetricsEndedAtSeconds - Run.MetricsStartedAtSeconds)
				* 1000.0);
	}
	else if (Action == TEXT("log.capture"))
	{
		StepResult = BuildScenarioLogWindow(Run, ToolParams);
	}
	else if (Action == TEXT("frameTime.sample"))
	{
		StepResult = MakeShared<FJsonObject>();
		StepResult->SetNumberField(
			TEXT("frameTimeMs"),
			FApp::GetDeltaTime() * 1000.0);
		StepResult->SetNumberField(
			TEXT("fps"),
			FApp::GetDeltaTime() > SMALL_NUMBER
				? 1.0 / FApp::GetDeltaTime()
				: 0.0);
	}
	else
	{
		const FString Capability =
			MapScenarioActionToCapability(Action, ToolParams);
		if (Capability.IsEmpty() || !Registry.FindTool(Capability))
		{
			OutErrorCode = TEXT("capability_not_found");
			OutErrorMessage =
				FString::Printf(TEXT("Scenario action '%s' is unavailable."), *Action);
			return false;
		}

		TArray<FString> ParamErrors;
		if (!Registry.ValidateParams(Capability, ToolParams, ParamErrors))
		{
			OutErrorCode = TEXT("invalid_params");
			OutErrorMessage = FString::Join(ParamErrors, TEXT(" "));
			return false;
		}

		const FMCPToolResult ToolResult =
			Registry.ExecuteTool(Capability, ToolParams);
		if (!ToolResult.bSuccess)
		{
			OutErrorCode = ToolResult.ErrorCode;
			OutErrorMessage = ToolResult.ErrorMessage;
			bOutShouldRetry =
				OutErrorCode == TEXT("pie_not_running")
				|| OutErrorCode == TEXT("runtime_object_not_found")
				|| OutErrorCode == TEXT("widget_not_interactable");
			return false;
		}
		StepResult =
			ToolResult.Data.IsValid()
				? ToolResult.Data
				: MakeShared<FJsonObject>();
	}

	if (!EvaluateAssertions(Step, StepResult, OutErrorMessage))
	{
		OutErrorCode = TEXT("verification_failed");
		return false;
	}

	Run.StepResults.Add(StepId, StepResult);
	CaptureArtifact(Run, StepId, StepResult);

	TSharedPtr<FJsonObject> Receipt = MakeShared<FJsonObject>();
	Receipt->SetStringField(TEXT("id"), StepId);
	Receipt->SetStringField(TEXT("action"), Action);
	Receipt->SetBoolField(TEXT("ok"), true);
	Receipt->SetNumberField(
		TEXT("elapsedMs"),
		(FPlatformTime::Seconds() - Run.StepStartedAtSeconds) * 1000.0);
	TSharedPtr<FJsonObject> ReceiptData =
		CopyControllerJsonObject(StepResult);
	ReceiptData->RemoveField(TEXT("image_base64"));
	Receipt->SetObjectField(TEXT("data"), ReceiptData);
	Run.StepReceipts.Add(MakeShared<FJsonValueObject>(Receipt));

	++Run.StepIndex;
	Run.StepStartedAtSeconds = 0.0;
	Run.WaitUntilSeconds = 0.0;
	return true;
}

bool FProductionRuntimeController::EvaluateAssertions(
	const TSharedPtr<FJsonObject>& Step,
	const TSharedPtr<FJsonObject>& StepResult,
	FString& OutErrorMessage) const
{
	if (!Step->HasTypedField<EJson::Array>(TEXT("assertions")))
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& AssertionValue :
		Step->GetArrayField(TEXT("assertions")))
	{
		const TSharedPtr<FJsonObject> Assertion = AssertionValue->AsObject();
		const FString Type =
			GetStringFieldOr(Assertion, TEXT("type"), FString());
		TSharedPtr<FJsonValue> Actual =
			ResolveActualValue(Assertion, StepResult);
		if (!Assertion->HasField(TEXT("actual"))
			&& Type == TEXT("delegateBound"))
		{
			Actual = StepResult->Values.FindRef(TEXT("bound"));
		}
		const TSharedPtr<FJsonValue>* ExpectedPtr =
			Assertion->Values.Find(TEXT("expected"));
		const TSharedPtr<FJsonValue> Expected =
			ExpectedPtr ? *ExpectedPtr : nullptr;

		bool bPassed = true;
		if (Type == TEXT("exists"))
		{
			bPassed = Actual.IsValid() && !Actual->IsNull();
		}
		else if (Type == TEXT("equals") || Type == TEXT("widgetState"))
		{
			bPassed =
				Actual.IsValid()
				&& Expected.IsValid()
				&& FJsonValue::CompareEqual(*Actual, *Expected);
		}
		else if (Type == TEXT("delegateBound"))
		{
			const bool bExpected =
				Expected.IsValid() && Expected->Type == EJson::Boolean
					? Expected->AsBool()
					: true;
			bPassed =
				Actual.IsValid()
				&& Actual->Type == EJson::Boolean
				&& Actual->AsBool() == bExpected;
		}
		else if (Type == TEXT("hitTestContains"))
		{
			bPassed =
				Actual.IsValid()
				&& Expected.IsValid()
				&& Expected->Type == EJson::String
				&& JsonValueToCompactString(Actual).Contains(Expected->AsString());
		}
		else if (Type == TEXT("logNotContains"))
		{
			bPassed =
				Actual.IsValid()
				&& Expected.IsValid()
				&& Expected->Type == EJson::String
				&& !JsonValueToCompactString(Actual).Contains(Expected->AsString());
		}
		else if (Type == TEXT("imageDiff"))
		{
			// Image comparisons are hash based at this layer; callers can supply
			// an expected SHA-256 in assertion.params.expectedSha256.
			FString ExpectedHash;
			if (Assertion->HasTypedField<EJson::Object>(TEXT("params")))
			{
				Assertion->GetObjectField(TEXT("params"))->TryGetStringField(
					TEXT("expectedSha256"),
					ExpectedHash);
			}
			FString Base64;
			bPassed =
				!ExpectedHash.IsEmpty()
				&& StepResult->TryGetStringField(TEXT("image_base64"), Base64);
			if (bPassed)
			{
				TArray<uint8> Bytes;
				bPassed =
					FBase64::Decode(Base64, Bytes)
					&& Bytes.Num() <= MAX_uint32;
				if (bPassed)
				{
					FString ActualHash;
					bPassed =
						UEAIIntegration::Infrastructure::TrySha256Hex(
							Bytes,
							ActualHash)
						&& ActualHash.Equals(
							ExpectedHash,
							ESearchCase::IgnoreCase);
				}
			}
		}

		if (!bPassed)
		{
			OutErrorMessage = FString::Printf(
				TEXT("Assertion '%s' failed."),
				*Type);
			return false;
		}
	}
	return true;
}

void FProductionRuntimeController::CaptureArtifact(
	FScenarioRun& Run,
	const FString& StepId,
	const TSharedPtr<FJsonObject>& StepResult)
{
	FString Base64;
	if (!StepResult.IsValid()
		|| !StepResult->TryGetStringField(TEXT("image_base64"), Base64))
	{
		return;
	}

	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64, Bytes))
	{
		return;
	}

	FString MimeType;
	if (!StepResult->TryGetStringField(TEXT("mime_type"), MimeType))
	{
		const FString Format =
			GetStringFieldOr(StepResult, TEXT("format"), TEXT("jpeg"));
		MimeType =
			Format.Equals(TEXT("png"), ESearchCase::IgnoreCase)
				? TEXT("image/png")
				: TEXT("image/jpeg");
	}
	const FString Extension =
		MimeType == TEXT("image/png") ? TEXT("png") : TEXT("jpg");
	const FString ArtifactId = NewOpaqueId(TEXT("artifact"));
	const FString Path = FPaths::Combine(
		ScenarioDirectory(),
		Run.Id,
		FString::Printf(TEXT("%s.%s"), *ArtifactId, *Extension));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
	{
		return;
	}

	FScenarioArtifact Artifact;
	Artifact.Id = ArtifactId;
	Artifact.Path = Path;
	Artifact.MimeType = MimeType;
	Artifact.Metadata = MakeShared<FJsonObject>();
	Artifact.Metadata->SetStringField(TEXT("runId"), Run.Id);
	Artifact.Metadata->SetStringField(TEXT("stepId"), StepId);
	Artifact.Metadata->SetStringField(TEXT("sha256"), ComputeFileSha256(Path));
	Run.Artifacts.Add(ArtifactId, Artifact);
	StepResult->SetStringField(TEXT("artifactId"), ArtifactId);
}

void FProductionRuntimeController::CaptureScenarioLogWindow(
	FScenarioRun& Run) const
{
	Run.LogWindow = BuildScenarioLogWindow(Run);
}

TSharedPtr<FJsonObject> FProductionRuntimeController::BuildScenarioLogWindow(
	const FScenarioRun& Run,
	const TSharedPtr<FJsonObject>& Params) const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), Run.LogPath);
	Data->SetNumberField(
		TEXT("startCursor"),
		static_cast<double>(Run.LogStartOffset));
	if (Run.LogPath.IsEmpty()
		|| !IFileManager::Get().FileExists(*Run.LogPath))
	{
		Data->SetBoolField(TEXT("available"), false);
		Data->SetStringField(
			TEXT("reason"),
			TEXT("The active Editor log file could not be resolved."));
		Data->SetStringField(TEXT("content"), FString());
		return Data;
	}

	const int64 FileSize = IFileManager::Get().FileSize(*Run.LogPath);
	if (FileSize < 0)
	{
		Data->SetBoolField(TEXT("available"), false);
		Data->SetStringField(
			TEXT("reason"),
			TEXT("The active Editor log file could not be measured."));
		Data->SetStringField(TEXT("content"), FString());
		return Data;
	}
	const bool bRotated = FileSize < Run.LogStartOffset;
	const int64 WindowStart = bRotated ? 0 : Run.LogStartOffset;
	const int32 MaxChars = FMath::Clamp(
		Params.IsValid() && Params->HasField(TEXT("maxChars"))
			? static_cast<int32>(
				Params->GetNumberField(TEXT("maxChars")))
			: 262144,
		1,
		1024 * 1024);
	const int64 AvailableBytes = FMath::Max<int64>(0, FileSize - WindowStart);
	const int64 ReadStart =
		AvailableBytes > MaxChars
			? FileSize - MaxChars
			: WindowStart;
	const int32 BytesToRead = static_cast<int32>(
		FMath::Min<int64>(MaxChars, FileSize - ReadStart));
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(BytesToRead + 1);
	bool bRead = false;
	TUniquePtr<IFileHandle> File(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(
			*Run.LogPath));
	if (File)
	{
		bRead =
			File->Seek(ReadStart)
			&& (BytesToRead == 0
				|| File->Read(Bytes.GetData(), BytesToRead));
	}
	if (!bRead)
	{
		Data->SetBoolField(TEXT("available"), false);
		Data->SetStringField(
			TEXT("reason"),
			TEXT("The scenario log window could not be read."));
		Data->SetStringField(TEXT("content"), FString());
		return Data;
	}
	Bytes[BytesToRead] = 0;
	const FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		BytesToRead);
	FString Content(Converted.Length(), Converted.Get());
	FString Filter;
	if (Params.IsValid())
	{
		Params->TryGetStringField(TEXT("filter"), Filter);
	}
	Filter.LeftInline(256, false);
	if (!Filter.IsEmpty())
	{
		TArray<FString> Lines;
		Content.ParseIntoArrayLines(Lines, false);
		TArray<FString> MatchingLines;
		for (const FString& Line : Lines)
		{
			if (Line.Contains(Filter, ESearchCase::IgnoreCase))
			{
				MatchingLines.Add(Line);
			}
		}
		Content = FString::Join(MatchingLines, TEXT("\n"));
		Data->SetStringField(TEXT("filter"), Filter);
		Data->SetNumberField(
			TEXT("matchingLineCount"),
			MatchingLines.Num());
	}

	TArray<FString> ResultLines;
	Content.ParseIntoArrayLines(ResultLines, false);
	Data->SetBoolField(TEXT("available"), true);
	Data->SetBoolField(TEXT("rotated"), bRotated);
	Data->SetBoolField(
		TEXT("truncated"),
		AvailableBytes > MaxChars);
	Data->SetNumberField(
		TEXT("retainedFromCursor"),
		static_cast<double>(ReadStart));
	Data->SetNumberField(
		TEXT("endCursor"),
		static_cast<double>(FileSize));
	Data->SetNumberField(TEXT("lineCount"), ResultLines.Num());
	Data->SetStringField(TEXT("content"), Content);
	return Data;
}

void FProductionRuntimeController::WriteScenarioReceipt(
	const FScenarioRun& Run) const
{
	TSharedPtr<FJsonObject> Receipt = MakeScenarioSummary(Run);
	Receipt->SetArrayField(TEXT("steps"), Run.StepReceipts);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	if (!FJsonSerializer::Serialize(Receipt.ToSharedRef(), Writer))
	{
		return;
	}
	const FString RunDirectory = FPaths::Combine(ScenarioDirectory(), Run.Id);
	IFileManager::Get().MakeDirectory(*RunDirectory, true);
	FFileHelper::SaveStringToFile(
		Json,
		*FPaths::Combine(RunDirectory, TEXT("receipt.json")));
}

TSharedPtr<FJsonObject> FProductionRuntimeController::MakeScenarioSummary(
	const FScenarioRun& Run) const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("runId"), Run.Id);
	Data->SetStringField(TEXT("name"), Run.Name);
	Data->SetStringField(TEXT("status"), Run.Status);
	Data->SetNumberField(TEXT("currentStep"), Run.StepIndex);
	Data->SetNumberField(
		TEXT("stepCount"),
		Run.Scenario.IsValid()
			? Run.Scenario->GetArrayField(TEXT("steps")).Num()
			: 0);
	TSharedPtr<FJsonObject> Metrics = MakeShared<FJsonObject>();
	Metrics->SetBoolField(TEXT("active"), Run.bMetricsActive);
	Metrics->SetNumberField(
		TEXT("beginCount"),
		Run.MetricsBeginCount);
	Metrics->SetNumberField(
		TEXT("endCount"),
		Run.MetricsEndCount);
	Metrics->SetBoolField(
		TEXT("completed"),
		Run.MetricsBeginCount == 1
			&& Run.MetricsEndCount == 1
			&& !Run.bMetricsActive);
	if (Run.MetricsStartedAtSeconds > 0.0)
	{
		Metrics->SetNumberField(
			TEXT("elapsedMs"),
			FMath::Max(
				0.0,
				(Run.bMetricsActive
					? FPlatformTime::Seconds()
					: Run.MetricsEndedAtSeconds)
					- Run.MetricsStartedAtSeconds)
				* 1000.0);
	}
	Data->SetObjectField(TEXT("metrics"), Metrics);
	if (Run.LogWindow.IsValid())
	{
		Data->SetObjectField(
			TEXT("logWindow"),
			CopyControllerJsonObject(Run.LogWindow));
	}
	if (!Run.ErrorCode.IsEmpty())
	{
		Data->SetObjectField(
			TEXT("error"),
			MakeErrorObject(Run.ErrorCode, Run.ErrorMessage));
	}
	return Data;
}

TSharedPtr<FJsonObject> FProductionRuntimeController::MakeBuildSummary(
	const FBuildJob& Job) const
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("jobId"), Job.Id);
	Data->SetStringField(TEXT("mode"), Job.Mode);
	Data->SetStringField(TEXT("status"), Job.Status);
	Data->SetStringField(TEXT("startedAtUtc"), Job.StartedAtUtc);
	Data->SetStringField(TEXT("completedAtUtc"), Job.CompletedAtUtc);
	Data->SetNumberField(TEXT("processId"), Job.ProcessId);
	if (Job.ReturnCode != INDEX_NONE)
	{
		Data->SetNumberField(TEXT("returnCode"), Job.ReturnCode);
	}
	if (!Job.Message.IsEmpty())
	{
		Data->SetStringField(TEXT("message"), Job.Message);
	}
	if (!Job.LogPath.IsEmpty())
	{
		Data->SetStringField(TEXT("logPath"), Job.LogPath);
	}
	if (!Job.Output.IsEmpty())
	{
		Data->SetStringField(TEXT("outputTail"), Job.Output.Right(16384));
	}
	if (Job.BuiltDll.IsValid())
	{
		Data->SetObjectField(TEXT("dll"), Job.BuiltDll);
	}
	if (Job.BuiltPdb.IsValid())
	{
		Data->SetObjectField(TEXT("pdb"), Job.BuiltPdb);
	}
	return Data;
}

FString FProductionRuntimeController::ScenarioDirectory()
{
	return FPaths::ConvertRelativePathToFull(
		FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UE_AI_integration/Scenarios")));
}

FString FProductionRuntimeController::FindEditorLogPath()
{
	const FString ProjectLogDirectory =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
	const TArray<FString> Preferred = {
		FPaths::Combine(
			ProjectLogDirectory,
			FString(FApp::GetProjectName()) + TEXT(".log")),
		FPaths::Combine(
			ProjectLogDirectory,
			TEXT("UnrealEditor.log"))
	};
	for (const FString& Candidate : Preferred)
	{
		if (IFileManager::Get().FileExists(*Candidate))
		{
			return Candidate;
		}
	}

	TArray<FString> LogFiles;
	IFileManager::Get().FindFiles(
		LogFiles,
		*FPaths::Combine(ProjectLogDirectory, TEXT("*.log")),
		true,
		false);
	FString NewestPath;
	FDateTime NewestTimestamp = FDateTime::MinValue();
	for (const FString& Name : LogFiles)
	{
		const FString Candidate =
			FPaths::Combine(ProjectLogDirectory, Name);
		const FDateTime Timestamp =
			IFileManager::Get().GetTimeStamp(*Candidate);
		if (NewestPath.IsEmpty() || Timestamp > NewestTimestamp)
		{
			NewestPath = Candidate;
			NewestTimestamp = Timestamp;
		}
	}
	return NewestPath;
}

FString FProductionRuntimeController::MapScenarioActionToCapability(
	const FString& Action,
	TSharedPtr<FJsonObject>& InOutParams)
{
	if (Action.StartsWith(TEXT("pie.")))
	{
		return FString::Printf(TEXT("scene.%s"), *Action);
	}
	if (Action == TEXT("object.find")
		|| Action == TEXT("object.get")
		|| Action == TEXT("object.set")
		|| Action == TEXT("object.call"))
	{
		return FString::Printf(TEXT("scene.runtime.%s"), *Action);
	}
	if (Action == TEXT("delegate.bind")
		|| Action == TEXT("delegate.unbind")
		|| Action == TEXT("delegate.list")
		|| Action == TEXT("delegate.is_bound")
		|| Action == TEXT("delegate.broadcast"))
	{
		return FString::Printf(TEXT("scene.runtime.%s"), *Action);
	}
	if (Action == TEXT("widget.click"))
	{
		InOutParams->SetStringField(TEXT("action"), TEXT("click"));
		return TEXT("scene.runtime.input.pointer");
	}
	if (Action == TEXT("widget.drag"))
	{
		InOutParams->SetStringField(TEXT("action"), TEXT("drag"));
		return TEXT("scene.runtime.input.pointer");
	}
	if (Action == TEXT("widget.focus"))
	{
		return TEXT("scene.runtime.widget.focus.set");
	}
	if (Action == TEXT("widget.hitTest"))
	{
		return TEXT("scene.runtime.widget.hit_test");
	}
	if (Action == TEXT("widget.tree"))
	{
		return TEXT("scene.runtime.widget.tree.get");
	}
	if (Action == TEXT("widget.state"))
	{
		return TEXT("scene.runtime.widget.state.get");
	}
	if (Action == TEXT("input.key"))
	{
		return TEXT("scene.runtime.input.key");
	}
	if (Action == TEXT("input.mode"))
	{
		return TEXT("scene.runtime.input.mode.set");
	}
	if (Action == TEXT("viewport.capture"))
	{
		return TEXT("scene.viewport.capture");
	}
	if (Action == TEXT("log.capture"))
	{
		return TEXT("scene.output_log.get");
	}
	return FString();
}

FString FProductionRuntimeController::ComputeFileSha256(const FString& Path)
{
	if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path))
	{
		return FString();
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path)
		|| Bytes.Num() > MAX_uint32)
	{
		return FString();
	}
	FString Hash;
	if (!UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, Hash))
	{
		return FString();
	}
	return Hash;
}

TSharedPtr<FJsonObject> FProductionRuntimeController::MakeFileProvenance(
	const FString& Path)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), Path);
	const bool bExists =
		!Path.IsEmpty() && IFileManager::Get().FileExists(*Path);
	Data->SetBoolField(TEXT("exists"), bExists);
	if (!bExists)
	{
		return Data;
	}
	Data->SetNumberField(
		TEXT("size"),
		static_cast<double>(IFileManager::Get().FileSize(*Path)));
	Data->SetStringField(
		TEXT("timestampUtc"),
		IFileManager::Get().GetTimeStamp(*Path).ToIso8601());
	Data->SetStringField(TEXT("sha256"), ComputeFileSha256(Path));
	return Data;
}

FString FProductionRuntimeController::GetEditorStartTimeUtc()
{
#if PLATFORM_WINDOWS
	FILETIME CreationTime;
	FILETIME ExitTime;
	FILETIME KernelTime;
	FILETIME UserTime;
	if (::GetProcessTimes(
			::GetCurrentProcess(),
			&CreationTime,
			&ExitTime,
			&KernelTime,
			&UserTime))
	{
		SYSTEMTIME SystemTime;
		if (::FileTimeToSystemTime(&CreationTime, &SystemTime))
		{
			return FDateTime(
				SystemTime.wYear,
				SystemTime.wMonth,
				SystemTime.wDay,
				SystemTime.wHour,
				SystemTime.wMinute,
				SystemTime.wSecond,
				SystemTime.wMilliseconds).ToIso8601();
		}
	}
#endif
	return FDateTime::UtcNow().ToIso8601();
}
}
