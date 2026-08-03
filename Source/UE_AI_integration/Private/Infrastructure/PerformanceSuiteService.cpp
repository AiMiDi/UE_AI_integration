#include "Infrastructure/PerformanceSuiteService.h"

#include "Algo/AllOf.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Sha256.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
TSharedPtr<FJsonObject> CopySuiteObject(const TSharedPtr<FJsonObject>& Source)
{
	TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
	if (Source.IsValid())
	{
		Copy->Values = Source->Values;
	}
	return Copy;
}

bool IsSuiteTerminal(const FString& Status)
{
	return Status == TEXT("succeeded")
		|| Status == TEXT("completed")
		|| Status == TEXT("failed")
		|| Status == TEXT("cancelled")
		|| Status == TEXT("timedOut")
		|| Status == TEXT("timeout");
}

bool ReadSuiteJsonFile(const FString& Path, TSharedPtr<FJsonObject>& Out)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
}

bool WriteSuiteJsonAtomic(const FString& Path, const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FString Temporary = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(
			Text,
			*Temporary,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	return IFileManager::Get().Move(
		*Path,
		*Temporary,
		true,
		true,
		false,
		true);
}

TArray<FString> DefinitionRoots()
{
	TArray<FString> Roots;
	if (const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration")))
	{
		Roots.Add(FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Resources/PerformanceSuites")));
	}
	Roots.Add(FPaths::Combine(
		FPaths::ProjectConfigDir(),
		TEXT("UE_AI_integration/PerformanceSuites")));
	return Roots;
}

bool HashSuiteFile(const FString& Path, FString& OutDigest)
{
	TArray<uint8> Bytes;
	return FFileHelper::LoadFileToArray(Bytes, *Path)
		&& TrySha256Hex(Bytes, OutDigest);
}

bool ResolveSuiteMap(
	const TSharedPtr<FJsonObject>& Scenario,
	FString& OutFilename,
	FString& OutDigest,
	FString& OutError)
{
	FString Map;
	FString Expected;
	FString ScenarioId;
	if (Scenario.IsValid())
	{
		Scenario->TryGetStringField(TEXT("map"), Map);
		Scenario->TryGetStringField(TEXT("mapHash"), Expected);
		Scenario->TryGetStringField(TEXT("id"), ScenarioId);
	}
	if (!FPackageName::DoesPackageExist(Map, &OutFilename))
	{
		OutError = FString::Printf(
			TEXT("Scenario map '%s' does not exist in this project."),
			*Map);
		return false;
	}
	if (!HashSuiteFile(OutFilename, OutDigest))
	{
		OutError = FString::Printf(
			TEXT("Scenario map '%s' could not be hashed."),
			*Map);
		return false;
	}
	if (Expected.StartsWith(TEXT("sha256:"))
		&& Expected != TEXT("sha256:") + OutDigest)
	{
		OutError = FString::Printf(
			TEXT("Scenario map '%s' does not match its declared SHA-256."),
			*Map);
		return false;
	}
	if (!Expected.StartsWith(TEXT("sha256:"))
		&& !Expected.StartsWith(TEXT("fixture:")))
	{
		OutError = FString::Printf(
			TEXT("Scenario '%s' has an unsupported mapHash contract."),
			*ScenarioId);
		return false;
	}
	return true;
}

void ValidateSuiteInputStep(
	const FString& ScenarioId,
	const TSharedPtr<FJsonObject>& Step,
	TArray<FString>& OutErrors)
{
	FString StepId;
	FString Action;
	Step->TryGetStringField(TEXT("id"), StepId);
	Step->TryGetStringField(TEXT("action"), Action);
	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (StepId.IsEmpty() || Action.IsEmpty()
		|| !Step->TryGetObjectField(TEXT("params"), Params)
		|| !Params || !Params->IsValid())
	{
		OutErrors.Add(FString::Printf(
			TEXT("Scenario '%s' has an input step without id, action or params."),
			*ScenarioId));
		return;
	}

	bool bValid = true;
	if (Action == TEXT("object.find"))
	{
		bValid = (*Params)->HasField(TEXT("class"))
			|| (*Params)->HasField(TEXT("name"))
			|| (*Params)->HasField(TEXT("path"));
	}
	else if (Action == TEXT("widget.focus"))
	{
		bValid = (*Params)->HasField(TEXT("objectRef"));
	}
	else if (Action == TEXT("widget.click"))
	{
		bValid = (*Params)->HasField(TEXT("target"))
			|| (*Params)->HasField(TEXT("position"));
	}
	else if (Action == TEXT("widget.drag"))
	{
		bValid = ((*Params)->HasField(TEXT("target"))
				|| (*Params)->HasField(TEXT("position")))
			&& (*Params)->HasField(TEXT("endPosition"));
	}
	else if (Action == TEXT("input.key"))
	{
		FString KeyAction;
		(*Params)->TryGetStringField(TEXT("action"), KeyAction);
		bValid = (KeyAction == TEXT("press")
				|| KeyAction == TEXT("release"))
			? (*Params)->HasField(TEXT("key"))
			: KeyAction == TEXT("type")
				? (*Params)->HasField(TEXT("text"))
				: KeyAction == TEXT("chord")
					&& (*Params)->HasField(TEXT("keys"));
	}
	if (!bValid)
	{
		OutErrors.Add(FString::Printf(
			TEXT("Scenario '%s' input step '%s' does not satisfy the '%s' parameter contract."),
			*ScenarioId,
			*StepId,
			*Action));
	}
}
}

FPerformanceSuiteService::FPerformanceSuiteService(FOperation InOperation)
	: Operation(MoveTemp(InOperation))
{
}

FPerformanceSuiteService::~FPerformanceSuiteService()
{
	if (FSuiteRun* Active = Runs.Find(ActiveRunId))
	{
		RestoreMeasurementCVars(*Active);
		FString RestoreError;
		RestoreOriginalMap(*Active, RestoreError);
	}
}

FString FPerformanceSuiteService::StringField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Default)
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Field, Value)
		? Value
		: Default;
}

FString FPerformanceSuiteService::NewId(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("%s-%s"),
		Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
}

bool FPerformanceSuiteService::Handles(const FString& CapabilityId) const
{
	return CapabilityId.StartsWith(TEXT("production.performance.suite."))
		|| CapabilityId == TEXT("production.performance.baseline.promote");
}

FMCPToolResult FPerformanceSuiteService::Execute(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params)
{
	if (CapabilityId == TEXT("production.performance.suite.list"))
	{
		return ListSuites();
	}
	if (CapabilityId == TEXT("production.performance.suite.validate"))
	{
		return ValidateSuites(Params);
	}
	if (CapabilityId == TEXT("production.performance.suite.run"))
	{
		return StartSuite(Params);
	}
	if (CapabilityId == TEXT("production.performance.suite.result.get"))
	{
		return GetSuiteResult(Params);
	}
	if (CapabilityId == TEXT("production.performance.baseline.promote"))
	{
		return PromoteBaseline(Params);
	}
	return FMCPToolResult::Error(
		TEXT("Unknown performance suite operation."),
		TEXT("unknown_capability"),
		404);
}

bool FPerformanceSuiteService::ValidateDefinition(
	const TSharedPtr<FJsonObject>& Definition,
	TArray<FString>& OutErrors)
{
	OutErrors.Reset();
	if (!Definition.IsValid()
		|| StringField(Definition, TEXT("schema"))
			!= TEXT("ue.performance-suite.v1"))
	{
		OutErrors.Add(TEXT("schema must be ue.performance-suite.v1."));
		return false;
	}
	if (StringField(Definition, TEXT("suiteId")).IsEmpty())
	{
		OutErrors.Add(TEXT("suiteId is required."));
	}
	if (!Definition->HasTypedField<EJson::Array>(TEXT("scenarios")))
	{
		OutErrors.Add(TEXT("scenarios must be an array."));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Scenarios =
		Definition->GetArrayField(TEXT("scenarios"));
	if (Scenarios.IsEmpty() || Scenarios.Num() > 32)
	{
		OutErrors.Add(TEXT("scenarios must contain between 1 and 32 entries."));
	}
	TSet<FString> Ids;
	for (const TSharedPtr<FJsonValue>& Value : Scenarios)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutErrors.Add(TEXT("Every scenario must be an object."));
			continue;
		}
		const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
		const FString Id = StringField(Scenario, TEXT("id"));
		if (Id.IsEmpty() || Ids.Contains(Id))
		{
			OutErrors.Add(TEXT("Scenario ids must be non-empty and unique."));
		}
		Ids.Add(Id);
		if (StringField(Scenario, TEXT("map")).IsEmpty()
			|| StringField(Scenario, TEXT("mapHash")).IsEmpty())
		{
			OutErrors.Add(FString::Printf(
				TEXT("Scenario '%s' requires explicit map and mapHash."),
				*Id));
		}
		if (!Scenario->HasTypedField<EJson::Object>(TEXT("request")))
		{
			OutErrors.Add(FString::Printf(
				TEXT("Scenario '%s' requires a performance run request."),
				*Id));
			continue;
		}
		const TSharedPtr<FJsonObject> Request =
			Scenario->GetObjectField(TEXT("request"));
		double RepeatsNumber = 0.0;
		double Warmup = 0.0;
		double Sample = 0.0;
		Request->TryGetNumberField(TEXT("repeatCount"), RepeatsNumber);
		Request->TryGetNumberField(TEXT("warmupSeconds"), Warmup);
		Request->TryGetNumberField(TEXT("sampleSeconds"), Sample);
		const int32 Repeats = static_cast<int32>(RepeatsNumber);
		if (Repeats < 5 || Warmup < 5.0 || Sample < 10.0)
		{
			OutErrors.Add(FString::Printf(
				TEXT("Scenario '%s' must use repeatCount>=5, warmup>=5s and sample>=10s."),
				*Id));
		}
		const TSharedPtr<FJsonObject>* Standard = nullptr;
		if (StringField(Request, TEXT("profile")) != TEXT("standardScenario")
			|| !Request->TryGetObjectField(TEXT("standardProfile"), Standard)
			|| !Standard || !Standard->IsValid())
		{
			OutErrors.Add(FString::Printf(
				TEXT("Scenario '%s' requires a standardScenario profile."),
				*Id));
			continue;
		}
		if (StringField(*Standard, TEXT("map"))
			!= StringField(Scenario, TEXT("map"))
			|| !(*Standard)->HasTypedField<EJson::Object>(TEXT("camera"))
			|| !(*Standard)->HasTypedField<EJson::Array>(TEXT("inputSteps"))
			|| !(*Standard)->HasTypedField<EJson::Object>(TEXT("resolution"))
			|| !(*Standard)->HasTypedField<EJson::Object>(TEXT("scalability"))
			|| !(*Standard)->HasTypedField<EJson::Object>(TEXT("cvars")))
		{
			OutErrors.Add(FString::Printf(
				TEXT("Scenario '%s' must declare matching map, camera, inputSteps, resolution, scalability and cvars."),
				*Id));
		}
		else
		{
			for (const TSharedPtr<FJsonValue>& InputValue :
				(*Standard)->GetArrayField(TEXT("inputSteps")))
			{
				if (!InputValue.IsValid()
					|| InputValue->Type != EJson::Object)
				{
					OutErrors.Add(FString::Printf(
						TEXT("Scenario '%s' inputSteps entries must be objects."),
						*Id));
					continue;
				}
				ValidateSuiteInputStep(
					Id,
					InputValue->AsObject(),
					OutErrors);
			}
		}
		if (!Scenario->HasTypedField<EJson::Array>(TEXT("checks"))
			|| Scenario->GetArrayField(TEXT("checks")).IsEmpty())
		{
			OutErrors.Add(FString::Printf(
				TEXT("Scenario '%s' requires at least one threshold check."),
				*Id));
		}
	}
	return OutErrors.IsEmpty();
}

TArray<TSharedPtr<FJsonObject>> FPerformanceSuiteService::LoadDefinitions(
	TArray<FString>& OutErrors) const
{
	TMap<FString, TSharedPtr<FJsonObject>> ById;
	for (const FString& Root : DefinitionRoots())
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(
			Files,
			*Root,
			TEXT("*.json"),
			true,
			false);
		Files.Sort();
		for (const FString& File : Files)
		{
			TSharedPtr<FJsonObject> Definition;
			if (!ReadSuiteJsonFile(File, Definition))
			{
				OutErrors.Add(FString::Printf(TEXT("Could not parse %s."), *File));
				continue;
			}
			TArray<FString> Errors;
			if (!ValidateDefinition(Definition, Errors))
			{
				for (const FString& Error : Errors)
				{
					OutErrors.Add(FString::Printf(TEXT("%s: %s"), *File, *Error));
				}
				continue;
			}
			ById.Add(StringField(Definition, TEXT("suiteId")), Definition);
		}
	}
	TArray<FString> Ids;
	ById.GetKeys(Ids);
	Ids.Sort();
	TArray<TSharedPtr<FJsonObject>> Result;
	for (const FString& Id : Ids)
	{
		Result.Add(ById.FindChecked(Id));
	}
	return Result;
}

TSharedPtr<FJsonObject> FPerformanceSuiteService::FindDefinition(
	const FString& SuiteId,
	const FString& ScenarioId,
	TArray<FString>& OutErrors) const
{
	for (const TSharedPtr<FJsonObject>& Definition : LoadDefinitions(OutErrors))
	{
		if (StringField(Definition, TEXT("suiteId")) != SuiteId)
		{
			continue;
		}
		if (ScenarioId.IsEmpty())
		{
			return Definition;
		}
		for (const TSharedPtr<FJsonValue>& Value :
			Definition->GetArrayField(TEXT("scenarios")))
		{
			if (StringField(Value->AsObject(), TEXT("id")) == ScenarioId)
			{
				return Value->AsObject();
			}
		}
		OutErrors.Add(FString::Printf(
			TEXT("Scenario '%s' does not exist in suite '%s'."),
			*ScenarioId,
			*SuiteId));
		return nullptr;
	}
	OutErrors.Add(FString::Printf(TEXT("Unknown suite '%s'."), *SuiteId));
	return nullptr;
}

FMCPToolResult FPerformanceSuiteService::ListSuites() const
{
	TArray<FString> Errors;
	const TArray<TSharedPtr<FJsonObject>> Definitions = LoadDefinitions(Errors);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TSharedPtr<FJsonObject>& Definition : Definitions)
	{
		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(
			TEXT("suiteId"),
			StringField(Definition, TEXT("suiteId")));
		Summary->SetStringField(
			TEXT("title"),
			StringField(Definition, TEXT("title")));
		Summary->SetNumberField(
			TEXT("version"),
			Definition->GetNumberField(TEXT("version")));
		Summary->SetNumberField(
			TEXT("scenarioCount"),
			Definition->GetArrayField(TEXT("scenarios")).Num());
		Summary->SetStringField(
			TEXT("digest"),
			TEXT("sha256:") + DigestJson(Definition));
		Values.Add(MakeShared<FJsonValueObject>(Summary));
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("suites"), Values);
	Data->SetNumberField(TEXT("total"), Values.Num());
	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& Error : Errors)
	{
		ErrorValues.Add(MakeShared<FJsonValueString>(Error));
	}
	Data->SetArrayField(TEXT("definitionErrors"), ErrorValues);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FPerformanceSuiteService::ValidateSuites(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString SuiteId = StringField(Params, TEXT("suiteId"));
	bool bValidateAssets = true;
	if (Params.IsValid())
	{
		Params->TryGetBoolField(TEXT("validateAssets"), bValidateAssets);
	}
	TArray<FString> LoadErrors;
	const TArray<TSharedPtr<FJsonObject>> Definitions = LoadDefinitions(LoadErrors);
	TArray<TSharedPtr<FJsonValue>> Results;
	for (const TSharedPtr<FJsonObject>& Definition : Definitions)
	{
		if (!SuiteId.IsEmpty()
			&& StringField(Definition, TEXT("suiteId")) != SuiteId)
		{
			continue;
		}
		TArray<FString> Errors;
		ValidateDefinition(Definition, Errors);
		if (bValidateAssets)
		{
			for (const TSharedPtr<FJsonValue>& Value :
				Definition->GetArrayField(TEXT("scenarios")))
			{
				FString Filename;
				FString Digest;
				FString Error;
				if (!ResolveSuiteMap(
						Value->AsObject(),
						Filename,
						Digest,
						Error))
				{
					Errors.Add(Error);
				}
			}
		}
		const bool bValid = Errors.IsEmpty();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("suiteId"),
			StringField(Definition, TEXT("suiteId")));
		Result->SetBoolField(TEXT("valid"), bValid);
		Result->SetStringField(
			TEXT("digest"),
			TEXT("sha256:") + DigestJson(Definition));
		TArray<TSharedPtr<FJsonValue>> ErrorValues;
		for (const FString& Error : Errors)
		{
			ErrorValues.Add(MakeShared<FJsonValueString>(Error));
		}
		Result->SetArrayField(TEXT("errors"), ErrorValues);
		Result->SetBoolField(TEXT("assetsValidated"), bValidateAssets);
		Results.Add(MakeShared<FJsonValueObject>(Result));
	}
	if (!SuiteId.IsEmpty() && Results.IsEmpty())
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Unknown performance suite '%s'."), *SuiteId),
			TEXT("performance_suite_invalid"),
			404);
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("results"), Results);
	Data->SetBoolField(TEXT("valid"), LoadErrors.IsEmpty()
		&& Algo::AllOf(
			Results,
			[](const TSharedPtr<FJsonValue>& Value)
			{
				return Value->AsObject()->GetBoolField(TEXT("valid"));
			}));
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FPerformanceSuiteService::StartSuite(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!ActiveRunId.IsEmpty())
	{
		if (const FSuiteRun* Active = Runs.Find(ActiveRunId);
			Active && Active->Status == TEXT("running"))
		{
			return FMCPToolResult::Error(
				TEXT("A performance suite is already running."),
				TEXT("job_busy"),
				409);
		}
		ActiveRunId.Reset();
	}
	const FString SuiteId = StringField(Params, TEXT("suiteId"));
	TArray<FString> Errors;
	const TSharedPtr<FJsonObject> Definition =
		FindDefinition(SuiteId, FString(), Errors);
	if (!Definition.IsValid())
	{
		return FMCPToolResult::Error(
			Errors.IsEmpty() ? TEXT("Invalid performance suite.") : Errors[0],
			TEXT("performance_suite_invalid"),
			422);
	}
	TSet<FString> Requested;
	if (Params.IsValid()
		&& Params->HasTypedField<EJson::Array>(TEXT("scenarioIds")))
	{
		for (const TSharedPtr<FJsonValue>& Value :
			Params->GetArrayField(TEXT("scenarioIds")))
		{
			Requested.Add(Value->AsString());
		}
	}
	FSuiteRun Run;
	Run.Id = NewId(TEXT("performance-suite"));
	Run.SuiteId = SuiteId;
	Run.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
	for (const TSharedPtr<FJsonValue>& Value :
		Definition->GetArrayField(TEXT("scenarios")))
	{
		const TSharedPtr<FJsonObject> ScenarioDefinition = Value->AsObject();
		const FString Id = StringField(ScenarioDefinition, TEXT("id"));
		if (!Requested.IsEmpty() && !Requested.Contains(Id))
		{
			continue;
		}
		FScenarioRun Scenario;
		Scenario.Id = Id;
		Scenario.Definition = CopySuiteObject(ScenarioDefinition);
		FString MapFilename;
		FString MapDigest;
		FString MapError;
		if (!ResolveSuiteMap(
				Scenario.Definition,
				MapFilename,
				MapDigest,
				MapError))
		{
			return FMCPToolResult::Error(
				MapError,
				TEXT("performance_suite_invalid"),
				422);
		}
		Scenario.Definition->SetStringField(TEXT("mapFilename"), MapFilename);
		Scenario.Definition->SetStringField(
			TEXT("resolvedMapSha256"),
			TEXT("sha256:") + MapDigest);
		Run.Scenarios.Add(MoveTemp(Scenario));
	}
	if (Run.Scenarios.IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("No matching scenarios were selected."),
			TEXT("performance_suite_invalid"),
			422);
	}
	ActiveRunId = Run.Id;
	const FString RunId = Run.Id;
	Runs.Add(Run.Id, MoveTemp(Run));
	ApplyMeasurementCVars(Runs.FindChecked(RunId));
	PersistSuiteRun(Runs.FindChecked(RunId));
	return FMCPToolResult::Ok(MakeRunSummary(Runs.FindChecked(RunId), false));
}

void FPerformanceSuiteService::Tick()
{
	if (FSuiteRun* Run = Runs.Find(ActiveRunId))
	{
		if (Run->Status == TEXT("running"))
		{
			TickSuite(*Run);
			PersistSuiteRun(*Run);
		}
	}
}

void FPerformanceSuiteService::TickSuite(FSuiteRun& Run)
{
	if (!Run.Scenarios.IsValidIndex(Run.CurrentIndex))
	{
		bool bFailed = false;
		for (const FScenarioRun& Scenario : Run.Scenarios)
		{
			bFailed |= Scenario.Status == TEXT("failed")
				|| Scenario.Verdict == TEXT("regression");
		}
		FinishSuite(Run, bFailed ? TEXT("failed") : TEXT("succeeded"));
		return;
	}
	FScenarioRun& Scenario = Run.Scenarios[Run.CurrentIndex];
	if (Scenario.Status == TEXT("pending"))
	{
		BeginScenario(Run, Scenario);
		return;
	}
	if (Scenario.Status == TEXT("running"))
	{
		PollScenario(Run, Scenario);
		return;
	}
	++Run.CurrentIndex;
	Run.Phase = TEXT("starting");
}

void FPerformanceSuiteService::BeginScenario(
	FSuiteRun& Run,
	FScenarioRun& Scenario)
{
	FString EnvironmentError;
	if (!ApplyScenarioEnvironment(Run, Scenario, EnvironmentError))
	{
		Scenario.Status = TEXT("failed");
		Scenario.Verdict = TEXT("inconclusive");
		Scenario.ErrorCode = TEXT("performance_suite_invalid");
		Scenario.ErrorMessage = EnvironmentError;
		return;
	}
	TSharedPtr<FJsonObject> Request = CopySuiteObject(
		Scenario.Definition->GetObjectField(TEXT("request")));
	Request->SetStringField(
		TEXT("requestId"),
		FString::Printf(TEXT("%s-%s"), *Run.Id, *Scenario.Id));
	FMCPToolResult Start = Operation(TEXT("production.performance.run"), Request);
	if (!Start.bSuccess || !Start.Data.IsValid())
	{
		Scenario.Status = TEXT("failed");
		Scenario.Verdict = TEXT("inconclusive");
		Scenario.ErrorCode = Start.ErrorCode;
		Scenario.ErrorMessage = Start.ErrorMessage;
		return;
	}
	Scenario.RunId = StringField(Start.Data, TEXT("runId"));
	if (Scenario.RunId.IsEmpty())
	{
		Scenario.RunId = StringField(Start.Data, TEXT("jobId"));
	}
	if (Scenario.RunId.IsEmpty())
	{
		Scenario.Status = TEXT("failed");
		Scenario.ErrorCode = TEXT("performance_suite_invalid");
		Scenario.ErrorMessage = TEXT("Performance run returned no runId.");
		return;
	}
	Scenario.Status = TEXT("running");
	Run.Phase = TEXT("sampling");
}

FString FPerformanceSuiteService::ExtractStatus(
	const TSharedPtr<FJsonObject>& Data)
{
	FString Status = StringField(Data, TEXT("status"));
	if (!Status.IsEmpty())
	{
		return Status;
	}
	for (const TCHAR* Field : {TEXT("job"), TEXT("result"), TEXT("summary")})
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		if (Data.IsValid() && Data->TryGetObjectField(Field, Child)
			&& Child && Child->IsValid())
		{
			Status = ExtractStatus(*Child);
			if (!Status.IsEmpty())
			{
				return Status;
			}
		}
	}
	return FString();
}

void FPerformanceSuiteService::PollScenario(
	FSuiteRun& Run,
	FScenarioRun& Scenario)
{
	TSharedRef<FJsonObject> Query = MakeShared<FJsonObject>();
	Query->SetStringField(TEXT("runId"), Scenario.RunId);
	FMCPToolResult Result = Operation(
		TEXT("production.performance.result.get"),
		Query);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		Scenario.Status = TEXT("failed");
		Scenario.Verdict = TEXT("inconclusive");
		Scenario.ErrorCode = Result.ErrorCode;
		Scenario.ErrorMessage = Result.ErrorMessage;
		return;
	}
	const FString Status = ExtractStatus(Result.Data);
	if (!IsSuiteTerminal(Status))
	{
		return;
	}
	Scenario.Result = CopySuiteObject(Result.Data);
	if (Status == TEXT("failed") || Status == TEXT("cancelled")
		|| Status == TEXT("timedOut") || Status == TEXT("timeout"))
	{
		Scenario.Status = TEXT("failed");
		Scenario.Verdict = TEXT("inconclusive");
		return;
	}

	const TSharedPtr<FJsonObject> Baseline = LoadBaseline(Run.SuiteId, Scenario.Id);
	if (Baseline.IsValid())
	{
		TSharedRef<FJsonObject> Compare = MakeShared<FJsonObject>();
		Compare->SetStringField(
			TEXT("baselineRunId"),
			StringField(Baseline, TEXT("runId")));
		Compare->SetStringField(TEXT("candidateRunId"), Scenario.RunId);
		Compare->SetArrayField(
			TEXT("checks"),
			Scenario.Definition->GetArrayField(TEXT("checks")));
		Compare->SetBoolField(TEXT("autoTraceOnRegression"), true);
		TSharedPtr<FJsonObject> TraceRerun = CopySuiteObject(
			Scenario.Definition->GetObjectField(TEXT("request")));
		TraceRerun->SetBoolField(TEXT("captureTrace"), true);
		Compare->SetObjectField(TEXT("traceRerun"), TraceRerun);
		FMCPToolResult Comparison = Operation(
			TEXT("production.performance.compare"),
			Compare);
		if (Comparison.bSuccess && Comparison.Data.IsValid())
		{
			Scenario.ComparisonId = StringField(
				Comparison.Data,
				TEXT("comparisonId"));
			Scenario.Verdict = StringField(
				Comparison.Data,
				TEXT("verdict"),
				TEXT("inconclusive"));
		}
		else
		{
			Scenario.Verdict = TEXT("inconclusive");
			Scenario.ErrorCode = Comparison.ErrorCode;
			Scenario.ErrorMessage = Comparison.ErrorMessage;
		}
	}
	else
	{
		Scenario.Verdict = TEXT("unbaselined");
	}

	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetStringField(TEXT("runId"), Scenario.RunId);
	Report->SetStringField(
		TEXT("title"),
		FString::Printf(TEXT("%s / %s"), *Run.SuiteId, *Scenario.Id));
	if (!Scenario.ComparisonId.IsEmpty())
	{
		Report->SetStringField(TEXT("comparisonId"), Scenario.ComparisonId);
	}
	const FMCPToolResult ReportResult = Operation(
		TEXT("production.performance.report.generate"),
		Report);
	if (ReportResult.bSuccess && ReportResult.Data.IsValid())
	{
		Scenario.ReportJobId = StringField(
			ReportResult.Data,
			TEXT("jobId"));
	}
	Scenario.Status = Scenario.Verdict == TEXT("regression")
		? TEXT("failed")
		: TEXT("succeeded");
	Run.Phase = TEXT("reporting");
}

void FPerformanceSuiteService::ApplyMeasurementCVars(FSuiteRun& Run)
{
	if (Run.bCVarsApplied)
	{
		return;
	}
	for (const TCHAR* Name : {TEXT("r.VSync"), TEXT("t.MaxFPS")})
	{
		if (IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Run.OriginalCVars.Add(Name, Variable->GetString());
			Variable->Set(TEXT("0"), ECVF_SetByCode);
		}
	}
	Run.bCVarsApplied = true;
}

bool FPerformanceSuiteService::ApplyScenarioEnvironment(
	FSuiteRun& Run,
	const FScenarioRun& Scenario,
	FString& OutError)
{
	OutError.Reset();
	const TSharedPtr<FJsonObject>* Standard = nullptr;
	const TSharedPtr<FJsonObject>* CVars = nullptr;
	if (!Scenario.Definition.IsValid()
		|| !Scenario.Definition->TryGetObjectField(TEXT("request"), Standard)
		|| !Standard || !Standard->IsValid()
		|| !(*Standard)->TryGetObjectField(TEXT("standardProfile"), Standard)
		|| !Standard || !Standard->IsValid()
		|| !(*Standard)->TryGetObjectField(TEXT("cvars"), CVars)
		|| !CVars || !CVars->IsValid())
	{
		OutError = TEXT("The standard scenario has no cvar contract.");
		return false;
	}

	const FString RequiredMap = StringField(*Standard, TEXT("map"));
	UWorld* CurrentWorld = GEditor
		? GEditor->GetEditorWorldContext().World()
		: nullptr;
	const FString CurrentMap = CurrentWorld
		? CurrentWorld->GetOutermost()->GetName()
		: FString();
	if (RequiredMap.IsEmpty())
	{
		OutError = TEXT("The standard scenario has no fixed map contract.");
		return false;
	}
	if (CurrentMap != RequiredMap)
	{
		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyPackages(DirtyPackages);
		if (!DirtyPackages.IsEmpty())
		{
			TArray<FString> Names;
			for (const UPackage* Package : DirtyPackages)
			{
				if (Package)
				{
					Names.Add(Package->GetName());
				}
			}
			Names.Sort();
			OutError = FString::Printf(
				TEXT("Cannot open standard scenario map while dirty packages exist: %s"),
				*FString::Join(Names, TEXT(", ")));
			return false;
		}
		if (Run.OriginalMapPackage.IsEmpty())
		{
			Run.OriginalMapPackage = CurrentMap;
		}
		const FString MapFilename =
			StringField(Scenario.Definition, TEXT("mapFilename"));
		if (MapFilename.IsEmpty()
			|| !UEditorLoadingAndSavingUtils::LoadMap(MapFilename))
		{
			OutError = FString::Printf(
				TEXT("Failed to open fixed standard scenario map '%s'."),
				*RequiredMap);
			return false;
		}
		UWorld* LoadedWorld = GEditor
			? GEditor->GetEditorWorldContext().World()
			: nullptr;
		if (!LoadedWorld
			|| LoadedWorld->GetOutermost()->GetName() != RequiredMap)
		{
			OutError = FString::Printf(
				TEXT("Fixed standard scenario map '%s' did not become active."),
				*RequiredMap);
			return false;
		}
		Run.bMapChanged = true;
		Run.bMapRestored = false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CVars)->Values)
	{
		IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(*Pair.Key);
		if (!Variable)
		{
			OutError = FString::Printf(
				TEXT("Required scenario CVar '%s' is unavailable."),
				*Pair.Key);
			return false;
		}
		if (!Run.OriginalCVars.Contains(Pair.Key))
		{
			Run.OriginalCVars.Add(Pair.Key, Variable->GetString());
		}
		FString Value;
		if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
		{
			Value = Pair.Value->AsString();
		}
		else if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
		{
			Value = FString::SanitizeFloat(Pair.Value->AsNumber());
		}
		else if (Pair.Value.IsValid()
			&& (Pair.Value->Type == EJson::Boolean))
		{
			Value = Pair.Value->AsBool() ? TEXT("1") : TEXT("0");
		}
		else
		{
			OutError = FString::Printf(
				TEXT("Scenario CVar '%s' must be string, number or boolean."),
				*Pair.Key);
			return false;
		}
		Variable->Set(*Value, ECVF_SetByCode);
	}
	return true;
}

bool FPerformanceSuiteService::RestoreOriginalMap(
	FSuiteRun& Run,
	FString& OutError)
{
	OutError.Reset();
	if (!Run.bMapChanged || Run.OriginalMapPackage.IsEmpty())
	{
		Run.bMapRestored = true;
		return true;
	}
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyPackages(DirtyPackages);
	if (!DirtyPackages.IsEmpty())
	{
		OutError = TEXT("The original Editor map was not restored because the performance scenario left dirty packages.");
		Run.bMapRestored = false;
		return false;
	}
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			Run.OriginalMapPackage,
			Filename,
			FPackageName::GetMapPackageExtension())
		|| !IFileManager::Get().FileExists(*Filename)
		|| !UEditorLoadingAndSavingUtils::LoadMap(Filename))
	{
		OutError = FString::Printf(
			TEXT("Could not restore the original Editor map '%s'."),
			*Run.OriginalMapPackage);
		Run.bMapRestored = false;
		return false;
	}
	Run.bMapChanged = false;
	Run.bMapRestored = true;
	return true;
}

void FPerformanceSuiteService::RestoreMeasurementCVars(FSuiteRun& Run)
{
	if (!Run.bCVarsApplied)
	{
		return;
	}
	for (const TPair<FString, FString>& Pair : Run.OriginalCVars)
	{
		if (IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
		{
			Variable->Set(*Pair.Value, ECVF_SetByCode);
		}
	}
	Run.bCVarsApplied = false;
}

void FPerformanceSuiteService::FinishSuite(
	FSuiteRun& Run,
	const FString& Status)
{
	RestoreMeasurementCVars(Run);
	FString RestoreError;
	const bool bMapRestored = RestoreOriginalMap(Run, RestoreError);
	Run.EnvironmentRestoreError = RestoreError;
	Run.Status = bMapRestored ? Status : TEXT("failed");
	Run.Phase = TEXT("complete");
	Run.FinishedAtUtc = FDateTime::UtcNow().ToIso8601();
	PersistSuiteRun(Run);
	if (ActiveRunId == Run.Id)
	{
		ActiveRunId.Reset();
	}
}

TSharedPtr<FJsonObject> FPerformanceSuiteService::MakeRunSummary(
	const FSuiteRun& Run,
	const bool bIncludeResults)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.performance-suite-result.v1"));
	Data->SetStringField(TEXT("suiteRunId"), Run.Id);
	Data->SetStringField(TEXT("suiteId"), Run.SuiteId);
	Data->SetStringField(TEXT("status"), Run.Status);
	Data->SetStringField(TEXT("phase"), Run.Phase);
	Data->SetStringField(TEXT("createdAtUtc"), Run.CreatedAtUtc);
	Data->SetStringField(TEXT("finishedAtUtc"), Run.FinishedAtUtc);
	Data->SetStringField(TEXT("originalMap"), Run.OriginalMapPackage);
	Data->SetBoolField(TEXT("mapRestored"), Run.bMapRestored);
	Data->SetStringField(
		TEXT("environmentRestoreError"),
		Run.EnvironmentRestoreError);
	Data->SetNumberField(TEXT("currentScenarioIndex"), Run.CurrentIndex);
	Data->SetStringField(TEXT("artifactPath"), SuiteResultPath(Run.Id));
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FScenarioRun& Scenario : Run.Scenarios)
	{
		TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("scenarioId"), Scenario.Id);
		Value->SetStringField(TEXT("status"), Scenario.Status);
		Value->SetStringField(TEXT("verdict"), Scenario.Verdict);
		Value->SetStringField(TEXT("runId"), Scenario.RunId);
		Value->SetStringField(TEXT("comparisonId"), Scenario.ComparisonId);
		Value->SetStringField(TEXT("reportJobId"), Scenario.ReportJobId);
		Value->SetStringField(TEXT("errorCode"), Scenario.ErrorCode);
		Value->SetStringField(TEXT("errorMessage"), Scenario.ErrorMessage);
		if (bIncludeResults && Scenario.Result.IsValid())
		{
			Value->SetObjectField(TEXT("result"), CopySuiteObject(Scenario.Result));
		}
		Values.Add(MakeShared<FJsonValueObject>(Value));
	}
	Data->SetArrayField(TEXT("scenarios"), Values);
	return Data;
}

FMCPToolResult FPerformanceSuiteService::GetSuiteResult(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString RunId = StringField(Params, TEXT("suiteRunId"));
	const FSuiteRun* Run = Runs.Find(RunId);
	if (!Run)
	{
		TSharedPtr<FJsonObject> Persisted;
		if (ReadSuiteJsonFile(SuiteResultPath(RunId), Persisted))
		{
			return FMCPToolResult::Ok(Persisted);
		}
		return FMCPToolResult::Error(
			TEXT("Unknown performance suite run."),
			TEXT("job_not_found"),
			404);
	}
	return FMCPToolResult::Ok(MakeRunSummary(*Run, true));
}

FString FPerformanceSuiteService::SuiteResultPath(const FString& SuiteRunId)
{
	if (!SuiteRunId.StartsWith(TEXT("performance-suite-"))
		|| SuiteRunId.Contains(TEXT("/"))
		|| SuiteRunId.Contains(TEXT("\\"))
		|| SuiteRunId.Contains(TEXT("..")))
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("UE_AI_integration/PerformanceSuites/invalid.json"));
	}
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration/PerformanceSuites"),
		SuiteRunId + TEXT(".json"));
}

void FPerformanceSuiteService::PersistSuiteRun(const FSuiteRun& Run) const
{
	WriteSuiteJsonAtomic(
		SuiteResultPath(Run.Id),
		MakeRunSummary(Run, true));
}

FString FPerformanceSuiteService::BaselinePath(
	const FString& SuiteId,
	const FString& ScenarioId)
{
	return FPaths::Combine(
		FPaths::ProjectConfigDir(),
		TEXT("UE_AI_integration/PerformanceBaselines"),
		SuiteId,
		ScenarioId + TEXT(".json"));
}

TSharedPtr<FJsonObject> FPerformanceSuiteService::LoadBaseline(
	const FString& SuiteId,
	const FString& ScenarioId) const
{
	TSharedPtr<FJsonObject> Baseline;
	return ReadSuiteJsonFile(BaselinePath(SuiteId, ScenarioId), Baseline)
		? Baseline
		: nullptr;
}

FMCPToolResult FPerformanceSuiteService::PromoteBaseline(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bConfirmWrite = false;
	if (!Params.IsValid()
		|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite)
		|| !bConfirmWrite
		|| StringField(Params, TEXT("requestId")).IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("Baseline promotion requires confirmWrite=true and requestId."),
			TEXT("confirmation_required"),
			409);
	}
	const FString SuiteRunId = StringField(Params, TEXT("suiteRunId"));
	const FString ScenarioId = StringField(Params, TEXT("scenarioId"));
	const FSuiteRun* Run = Runs.Find(SuiteRunId);
	if (!Run || Run->Status == TEXT("running"))
	{
		return FMCPToolResult::Error(
			TEXT("A completed suite run is required."),
			TEXT("job_not_found"),
			404);
	}
	const FScenarioRun* Scenario = Run->Scenarios.FindByPredicate(
		[&ScenarioId](const FScenarioRun& Candidate)
		{
			return Candidate.Id == ScenarioId;
		});
	if (!Scenario || Scenario->Status != TEXT("succeeded")
		|| !Scenario->Result.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("Only a succeeded scenario result can be promoted."),
			TEXT("performance_suite_invalid"),
			422);
	}
	const TSharedPtr<FJsonObject>* Fingerprint = nullptr;
	if (!Scenario->Result->TryGetObjectField(
			TEXT("environmentFingerprint"),
			Fingerprint)
		|| !Fingerprint || !Fingerprint->IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("Candidate result has no environment fingerprint."),
			TEXT("performance_suite_invalid"),
			422);
	}
	const TSharedPtr<FJsonObject> Existing = LoadBaseline(Run->SuiteId, ScenarioId);
	if (Existing.IsValid()
		&& Existing->HasTypedField<EJson::Object>(TEXT("environmentFingerprint"))
		&& DigestJson(Existing->GetObjectField(TEXT("environmentFingerprint")))
			!= DigestJson(*Fingerprint))
	{
		return FMCPToolResult::Error(
			TEXT("Candidate fingerprint does not match the target baseline group."),
			TEXT("performance_suite_invalid"),
			409);
	}
	TSharedRef<FJsonObject> Baseline = MakeShared<FJsonObject>();
	Baseline->SetStringField(TEXT("schema"), TEXT("ue.performance-baseline.v1"));
	Baseline->SetStringField(TEXT("suiteId"), Run->SuiteId);
	Baseline->SetStringField(TEXT("scenarioId"), ScenarioId);
	Baseline->SetStringField(TEXT("runId"), Scenario->RunId);
	Baseline->SetStringField(TEXT("promotedAtUtc"), FDateTime::UtcNow().ToIso8601());
	Baseline->SetStringField(
		TEXT("requestId"),
		StringField(Params, TEXT("requestId")));
	Baseline->SetObjectField(
		TEXT("environmentFingerprint"),
		CopySuiteObject(*Fingerprint));
	Baseline->SetObjectField(TEXT("result"), CopySuiteObject(Scenario->Result));
	Baseline->SetStringField(
		TEXT("digest"),
		TEXT("sha256:") + DigestJson(Baseline));
	const FString Path = BaselinePath(Run->SuiteId, ScenarioId);
	if (!WriteSuiteJsonAtomic(Path, Baseline))
	{
		return FMCPToolResult::Error(
			TEXT("Could not atomically write the project baseline."),
			TEXT("recovery_storage_unavailable"),
			507);
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("suiteId"), Run->SuiteId);
	Data->SetStringField(TEXT("scenarioId"), ScenarioId);
	Data->SetStringField(TEXT("runId"), Scenario->RunId);
	Data->SetStringField(TEXT("path"), Path);
	Data->SetStringField(TEXT("digest"), Baseline->GetStringField(TEXT("digest")));
	return FMCPToolResult::Ok(Data);
}
}
