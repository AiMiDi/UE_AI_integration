#include "Infrastructure/PerformanceRegressionService.h"

#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Sha256.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealEngine.h"

#if WITH_SSL
#include <openssl/sha.h>
#endif

namespace UEAIIntegration::Infrastructure
{
namespace
{
	constexpr int32 PerformanceMaxArtifactChunkBytes = 1024 * 1024;
	constexpr int32 MaxRegressionJobs = 64;

	TSharedPtr<FJsonObject> CopyPerformanceObject(
		const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		if (Source.IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
				Source->Values)
			{
				Copy->SetField(
					Pair.Key,
					FJsonValue::Duplicate(Pair.Value));
			}
		}
		return Copy;
	}

	FString StringField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& DefaultValue = FString())
	{
		FString Value;
		return Object.IsValid()
			&& Object->TryGetStringField(Field, Value)
				? Value
				: DefaultValue;
	}

	double NumberField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const double DefaultValue)
	{
		double Value = DefaultValue;
		return Object.IsValid()
			&& Object->TryGetNumberField(Field, Value)
				? Value
				: DefaultValue;
	}

	bool BoolField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const bool DefaultValue)
	{
		bool Value = DefaultValue;
		return Object.IsValid()
			&& Object->TryGetBoolField(Field, Value)
				? Value
				: DefaultValue;
	}

	TSharedPtr<FJsonObject> MakeStep(
		const FString& Id,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params =
			TSharedPtr<FJsonObject>())
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("id"), Id);
		Step->SetStringField(TEXT("action"), Action);
		if (Params.IsValid())
		{
			Step->SetObjectField(
				TEXT("params"),
				CopyPerformanceObject(Params));
		}
		return Step;
	}

	FString NormalizeMapName(FString Map)
	{
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(Map);
		if (AssetName.StartsWith(TEXT("UEDPIE_")))
		{
			int32 FirstSeparator = INDEX_NONE;
			int32 SecondSeparator = INDEX_NONE;
			if (AssetName.FindChar(TEXT('_'), FirstSeparator))
			{
				SecondSeparator = AssetName.Find(
					TEXT("_"),
					ESearchCase::CaseSensitive,
					ESearchDir::FromStart,
					FirstSeparator + 1);
			}
			if (SecondSeparator != INDEX_NONE)
			{
				Map =
					FPackageName::GetLongPackagePath(Map)
					+ TEXT("/")
					+ AssetName.Mid(SecondSeparator + 1);
			}
		}
		return Map;
	}

	FString EscapePerformanceXml(FString Value)
	{
		Value.ReplaceInline(TEXT("&"), TEXT("&amp;"));
		Value.ReplaceInline(TEXT("<"), TEXT("&lt;"));
		Value.ReplaceInline(TEXT(">"), TEXT("&gt;"));
		Value.ReplaceInline(TEXT("\""), TEXT("&quot;"));
		Value.ReplaceInline(TEXT("'"), TEXT("&apos;"));
		return Value;
	}

	FString ReadConsoleVariable(const TCHAR* Name)
	{
		if (const IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(Name))
		{
			return Variable->GetString();
		}
		return TEXT("unavailable");
	}

	FString ComputeFileSha256Streaming(const FString& Path)
	{
#if WITH_SSL
		TUniquePtr<IFileHandle> File(
			FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		if (!File.IsValid())
		{
			return FString();
		}
		SHA256_CTX Context;
		if (::SHA256_Init(&Context) != 1)
		{
			return FString();
		}
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(1024 * 1024);
		int64 Remaining = File->Size();
		while (Remaining > 0)
		{
			const int64 Chunk = FMath::Min<int64>(Remaining, Buffer.Num());
			if (!File->Read(Buffer.GetData(), Chunk)
				|| ::SHA256_Update(
					&Context,
					Buffer.GetData(),
					static_cast<size_t>(Chunk)) != 1)
			{
				return FString();
			}
			Remaining -= Chunk;
		}
		uint8 Digest[SHA256_DIGEST_LENGTH] = {};
		if (::SHA256_Final(Digest, &Context) != 1)
		{
			return FString();
		}
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
#else
		return FString();
#endif
	}

	FString GitRevision()
	{
		const FString ProjectDirectory =
			FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		const FString Arguments = FString::Printf(
			TEXT("-C \"%s\" rev-parse HEAD"),
			*ProjectDirectory.Replace(TEXT("\""), TEXT("\\\"")));
		int32 ReturnCode = INDEX_NONE;
		FString StdOut;
		FString StdErr;
		if (!FPlatformProcess::ExecProcess(
				TEXT("git"),
				*Arguments,
				&ReturnCode,
				&StdOut,
				&StdErr)
			|| ReturnCode != 0)
		{
			return TEXT("unavailable");
		}
		return StdOut.TrimStartAndEnd();
	}

	TSharedPtr<FJsonObject> MakeCVarSnapshot()
	{
		static const TCHAR* Names[] = {
			TEXT("r.VSync"),
			TEXT("t.MaxFPS"),
			TEXT("r.ScreenPercentage"),
			TEXT("r.DynamicRes.OperationMode"),
			TEXT("sg.ResolutionQuality"),
			TEXT("sg.ViewDistanceQuality"),
			TEXT("sg.AntiAliasingQuality"),
			TEXT("sg.ShadowQuality"),
			TEXT("sg.GlobalIlluminationQuality"),
			TEXT("sg.ReflectionQuality"),
			TEXT("sg.PostProcessQuality"),
			TEXT("sg.TextureQuality"),
			TEXT("sg.EffectsQuality"),
			TEXT("sg.FoliageQuality"),
			TEXT("sg.ShadingQuality")
		};
		TSharedPtr<FJsonObject> Snapshot = MakeShared<FJsonObject>();
		for (const TCHAR* Name : Names)
		{
			Snapshot->SetStringField(Name, ReadConsoleVariable(Name));
		}
		return Snapshot;
	}

	TSharedPtr<FJsonObject> MakeScalabilitySnapshot(
		const TSharedPtr<FJsonObject>& CVars)
	{
		TSharedPtr<FJsonObject> Scalability = MakeShared<FJsonObject>();
		static const TCHAR* Names[] = {
			TEXT("sg.ResolutionQuality"),
			TEXT("sg.ViewDistanceQuality"),
			TEXT("sg.AntiAliasingQuality"),
			TEXT("sg.ShadowQuality"),
			TEXT("sg.GlobalIlluminationQuality"),
			TEXT("sg.ReflectionQuality"),
			TEXT("sg.PostProcessQuality"),
			TEXT("sg.TextureQuality"),
			TEXT("sg.EffectsQuality"),
			TEXT("sg.FoliageQuality"),
			TEXT("sg.ShadingQuality")
		};
		for (const TCHAR* Name : Names)
		{
			Scalability->SetStringField(
				Name,
				StringField(CVars, Name, TEXT("unavailable")));
		}
		return Scalability;
	}

	TSharedPtr<FJsonObject> FindResultObject(
		const TSharedPtr<FJsonObject>& Response)
	{
		if (!Response.IsValid()
			|| !Response->HasTypedField<EJson::Object>(TEXT("result")))
		{
			return nullptr;
		}
		return Response->GetObjectField(TEXT("result"));
	}

	FString ResponseStatus(const TSharedPtr<FJsonObject>& Response)
	{
		return StringField(Response, TEXT("status"));
	}

	bool IsTerminal(const FString& Status)
	{
		return Status == TEXT("succeeded")
			|| Status == TEXT("failed")
			|| Status == TEXT("cancelled");
	}
}

FPerformanceRegressionService::FPerformanceRegressionService(
	FOperation InOperation)
	: Operation(MoveTemp(InOperation))
{
	const FString Root = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("PerformanceRegressions"));
	TArray<FString> Directories;
	IFileManager::Get().FindFiles(
		Directories,
		*FPaths::Combine(Root, TEXT("*")),
		false,
		true);
	for (const FString& Directory : Directories)
	{
		const FString ReportPath = FPaths::Combine(
			Root,
			Directory,
			TEXT("regression.json"));
		FString Json;
		TSharedPtr<FJsonObject> Comparison;
		if (!FFileHelper::LoadFileToString(Json, *ReportPath))
		{
			continue;
		}
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Comparison)
			|| !Comparison.IsValid())
		{
			continue;
		}
		FRegressionJob Job;
		Job.Id =
			StringField(Comparison, TEXT("comparisonId"), Directory);
		Job.CreatedAtUtc =
			IFileManager::Get().GetTimeStamp(*ReportPath).ToIso8601();
		Job.Comparison = Comparison;
		if (Comparison->HasTypedField<EJson::Object>(
				TEXT("diagnosticTrace")))
		{
			TSharedPtr<FJsonObject> Diagnostic =
				Comparison->GetObjectField(TEXT("diagnosticTrace"));
			const FString Status =
				StringField(Diagnostic, TEXT("status"));
			if (Status == TEXT("submitted")
				|| Status == TEXT("analyzing"))
			{
				Diagnostic->SetStringField(
					TEXT("status"),
					TEXT("interrupted"));
				Diagnostic->SetStringField(
					TEXT("message"),
					TEXT(
						"Editor restarted before the optional trace "
						"diagnostic completed; the comparison verdict "
						"and reports remain valid."));
			}
		}
		WriteRegressionArtifacts(Job);
		RegressionJobs.Add(Job.Id, MoveTemp(Job));
	}
}

void FPerformanceRegressionService::Tick()
{
	TArray<FString> JobIds;
	RegressionJobs.GetKeys(JobIds);
	for (const FString& JobId : JobIds)
	{
		if (FRegressionJob* Job = RegressionJobs.Find(JobId))
		{
			if (Job->Status == TEXT("running"))
			{
				TickRegressionJob(*Job);
			}
		}
	}
}

FMCPToolResult FPerformanceRegressionService::Execute(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params)
{
	if (CapabilityId == TEXT("production.performance.run"))
	{
		return StartPerformanceRun(Params);
	}
	if (CapabilityId == TEXT("production.performance.result.get"))
	{
		return GetPerformanceResult(Params);
	}
	if (CapabilityId == TEXT("production.performance.compare"))
	{
		return ComparePerformanceRuns(Params);
	}
	if (CapabilityId == TEXT("production.job.status")
		|| CapabilityId == TEXT("production.job.result.get"))
	{
		const FString JobId = StringField(Params, TEXT("jobId"));
		if (RegressionJobs.Contains(JobId))
		{
			return GetRegressionJob(CapabilityId, Params);
		}
	}
	if (CapabilityId == TEXT("production.job.artifact.get"))
	{
		const FString JobId = StringField(Params, TEXT("jobId"));
		if (RegressionJobs.Contains(JobId))
		{
			return GetRegressionArtifact(Params);
		}
	}
	return Operation(CapabilityId, Params);
}

bool FPerformanceRegressionService::NormalizeRunRequest(
	const TSharedPtr<FJsonObject>& Request,
	const FString& CurrentMap,
	TSharedPtr<FJsonObject>& OutRequest,
	TSharedPtr<FJsonObject>& OutProfile,
	FString& OutError)
{
	OutError.Reset();
	OutRequest = CopyPerformanceObject(Request);
	OutProfile.Reset();
	const FString Profile =
		StringField(Request, TEXT("profile"), TEXT("custom"));
	if (Profile == TEXT("custom") || Profile.IsEmpty())
	{
		return true;
	}
	if (Profile != TEXT("standardScenario"))
	{
		OutError =
			TEXT("profile must be 'custom' or 'standardScenario'.");
		return false;
	}
	if (!Request.IsValid()
		|| !Request->HasTypedField<EJson::Object>(
			TEXT("standardProfile")))
	{
		OutError =
			TEXT("standardScenario requires a standardProfile object.");
		return false;
	}
	const TSharedPtr<FJsonObject> Standard =
		Request->GetObjectField(TEXT("standardProfile"));
	const FString Map = StringField(Standard, TEXT("map"));
	if (!Map.StartsWith(TEXT("/Game/")))
	{
		OutError =
			TEXT("standardProfile.map must be a /Game package path.");
		return false;
	}
	const FString NormalizedCurrentMap = NormalizeMapName(CurrentMap);
	if (NormalizedCurrentMap != Map)
	{
		OutError = FString::Printf(
			TEXT(
				"standardProfile.map is '%s', but the active Editor map is "
				"'%s'. Open the fixed map before starting the regression run."),
			*Map,
			*NormalizedCurrentMap);
		return false;
	}
	if (!Standard->HasTypedField<EJson::Object>(TEXT("camera")))
	{
		OutError =
			TEXT("standardProfile.camera must identify the fixed runtime camera.");
		return false;
	}
	const TSharedPtr<FJsonObject> Camera =
		Standard->GetObjectField(TEXT("camera"));
	const FString CameraName = StringField(Camera, TEXT("name"));
	if (CameraName.IsEmpty() || CameraName.Len() > 256
		|| CameraName.Contains(TEXT("*"))
		|| CameraName.Contains(TEXT("?")))
	{
		OutError =
			TEXT(
				"standardProfile.camera.name must contain 1-256 exact "
				"characters without wildcards.");
		return false;
	}
	if (!Camera->HasTypedField<EJson::Array>(TEXT("location"))
		|| !Camera->HasTypedField<EJson::Array>(TEXT("rotation"))
		|| Camera->GetArrayField(TEXT("location")).Num() != 3
		|| Camera->GetArrayField(TEXT("rotation")).Num() != 3)
	{
		OutError =
			TEXT(
				"standardProfile.camera requires three-component location "
				"and rotation arrays.");
		return false;
	}
	for (const TCHAR* Field : {TEXT("location"), TEXT("rotation")})
	{
		for (const TSharedPtr<FJsonValue>& Component :
			Camera->GetArrayField(Field))
		{
			if (!Component.IsValid() || Component->Type != EJson::Number)
			{
				OutError =
					TEXT(
						"standardProfile.camera location/rotation "
						"components must be numbers.");
				return false;
			}
		}
	}

	const double RequestedWarmup = FMath::Clamp(
		NumberField(Request, TEXT("warmupSeconds"), 5.0),
		0.0,
		300.0);
	const double RequestedSample = FMath::Clamp(
		NumberField(Request, TEXT("sampleSeconds"), 10.0),
		0.1,
		3600.0);
	const int32 RequestedRepeats = static_cast<int32>(
		NumberField(Request, TEXT("repeatCount"), 5.0));
	const int32 RepeatCount = FMath::Clamp(
		FMath::Max(RequestedRepeats, 5),
		5,
		20);
	const bool bCaptureScreenshot =
		BoolField(Standard, TEXT("captureScreenshot"), false);

	TArray<TSharedPtr<FJsonValue>> Steps;
	Steps.Add(MakeShared<FJsonValueObject>(
		MakeStep(TEXT("standard.pie.start"), TEXT("pie.start"))));

	TSharedPtr<FJsonObject> FindCameraParams = MakeShared<FJsonObject>();
	FindCameraParams->SetStringField(TEXT("name"), CameraName);
	FindCameraParams->SetStringField(TEXT("class"), TEXT("*Actor"));
	FindCameraParams->SetNumberField(TEXT("limit"), 1);
	TSharedPtr<FJsonObject> FindCamera = MakeStep(
		TEXT("standard.camera.find"),
		TEXT("object.find"),
		FindCameraParams);
	TSharedPtr<FJsonObject> CameraExists = MakeShared<FJsonObject>();
	CameraExists->SetStringField(TEXT("type"), TEXT("exists"));
	CameraExists->SetStringField(TEXT("actual"), TEXT("#/objects/0"));
	FindCamera->SetArrayField(
		TEXT("assertions"),
		{MakeShared<FJsonValueObject>(CameraExists)});
	Steps.Add(MakeShared<FJsonValueObject>(FindCamera));

	const TArray<TSharedPtr<FJsonValue>>& Location =
		Camera->GetArrayField(TEXT("location"));
	const TArray<TSharedPtr<FJsonValue>>& Rotation =
		Camera->GetArrayField(TEXT("rotation"));
	TSharedPtr<FJsonObject> LocationObject = MakeShared<FJsonObject>();
	LocationObject->SetNumberField(TEXT("x"), Location[0]->AsNumber());
	LocationObject->SetNumberField(TEXT("y"), Location[1]->AsNumber());
	LocationObject->SetNumberField(TEXT("z"), Location[2]->AsNumber());
	TSharedPtr<FJsonObject> RotationObject = MakeShared<FJsonObject>();
	RotationObject->SetNumberField(TEXT("pitch"), Rotation[0]->AsNumber());
	RotationObject->SetNumberField(TEXT("yaw"), Rotation[1]->AsNumber());
	RotationObject->SetNumberField(TEXT("roll"), Rotation[2]->AsNumber());
	TSharedPtr<FJsonObject> CameraArgs = MakeShared<FJsonObject>();
	CameraArgs->SetObjectField(TEXT("NewLocation"), LocationObject);
	CameraArgs->SetObjectField(TEXT("NewRotation"), RotationObject);
	CameraArgs->SetBoolField(TEXT("bSweep"), false);
	CameraArgs->SetBoolField(TEXT("bTeleport"), true);
	TSharedPtr<FJsonObject> CameraCall = MakeShared<FJsonObject>();
	CameraCall->SetStringField(
		TEXT("objectRef"),
		TEXT("${standard.camera.find#/objects/0/objectRef}"));
	CameraCall->SetStringField(
		TEXT("function"),
		TEXT("K2_SetActorLocationAndRotation"));
	CameraCall->SetObjectField(TEXT("args"), CameraArgs);
	Steps.Add(MakeShared<FJsonValueObject>(
		MakeStep(
			TEXT("standard.camera.set"),
			TEXT("object.call"),
			CameraCall)));

	if (RequestedWarmup > 0.0)
	{
		TSharedPtr<FJsonObject> Wait = MakeShared<FJsonObject>();
		Wait->SetNumberField(
			TEXT("durationMs"),
			RequestedWarmup * 1000.0);
		Steps.Add(MakeShared<FJsonValueObject>(
			MakeStep(
				TEXT("standard.warmup"),
				TEXT("wait"),
				Wait)));
	}

	static const TSet<FString> AllowedInputActions = {
		TEXT("wait"),
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
		TEXT("input.mode")
	};
	TSet<FString> StepIds = {
		TEXT("standard.pie.start"),
		TEXT("standard.camera.find"),
		TEXT("standard.camera.set"),
		TEXT("standard.warmup"),
		TEXT("standard.metrics.begin"),
		TEXT("standard.measure"),
		TEXT("standard.metrics.end"),
		TEXT("standard.capture"),
		TEXT("standard.pie.stop")
	};
	TArray<TSharedPtr<FJsonValue>> InputSteps;
	if (Standard->HasTypedField<EJson::Array>(TEXT("inputSteps")))
	{
		InputSteps = Standard->GetArrayField(TEXT("inputSteps"));
	}
	if (InputSteps.Num() > 64)
	{
		OutError =
			TEXT("standardProfile.inputSteps may contain at most 64 steps.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& InputValue : InputSteps)
	{
		if (!InputValue.IsValid() || InputValue->Type != EJson::Object)
		{
			OutError =
				TEXT("Every standardProfile.inputSteps entry must be an object.");
			return false;
		}
		const TSharedPtr<FJsonObject> Input = InputValue->AsObject();
		const FString Id = StringField(Input, TEXT("id"));
		const FString Action = StringField(Input, TEXT("action"));
		if (Id.IsEmpty() || Id.StartsWith(TEXT("standard."))
			|| StepIds.Contains(Id))
		{
			OutError =
				TEXT(
					"Standard input step ids must be unique and may not use "
					"the reserved 'standard.' prefix.");
			return false;
		}
		if (!AllowedInputActions.Contains(Action))
		{
			OutError = FString::Printf(
				TEXT(
					"Standard input action '%s' is not deterministic input; "
					"PIE lifecycle and metrics markers are generated."),
				*Action);
			return false;
		}
		StepIds.Add(Id);
		Steps.Add(
			MakeShared<FJsonValueObject>(
				CopyPerformanceObject(Input)));
	}

	Steps.Add(MakeShared<FJsonValueObject>(
		MakeStep(TEXT("standard.metrics.begin"), TEXT("metrics.begin"))));
	TSharedPtr<FJsonObject> MeasureWait = MakeShared<FJsonObject>();
	MeasureWait->SetNumberField(
		TEXT("durationMs"),
		RequestedSample * 1000.0);
	Steps.Add(MakeShared<FJsonValueObject>(
		MakeStep(
			TEXT("standard.measure"),
			TEXT("wait"),
			MeasureWait)));
	Steps.Add(MakeShared<FJsonValueObject>(
		MakeStep(TEXT("standard.metrics.end"), TEXT("metrics.end"))));
	if (bCaptureScreenshot)
	{
		Steps.Add(MakeShared<FJsonValueObject>(
			MakeStep(
				TEXT("standard.capture"),
				TEXT("viewport.capture"))));
	}
	Steps.Add(MakeShared<FJsonValueObject>(
		MakeStep(TEXT("standard.pie.stop"), TEXT("pie.stop"))));

	TSharedPtr<FJsonObject> Scenario = MakeShared<FJsonObject>();
	Scenario->SetStringField(
		TEXT("name"),
		StringField(
			Standard,
			TEXT("name"),
			TEXT("standard-performance-regression")));
	Scenario->SetArrayField(TEXT("steps"), Steps);
	Scenario->SetNumberField(
		TEXT("timeoutMs"),
		FMath::Clamp(
			(RequestedWarmup + RequestedSample) * 1000.0 + 120000.0,
			1.0,
			3600000.0));
	TSharedPtr<FJsonObject> Cleanup = MakeShared<FJsonObject>();
	Cleanup->SetBoolField(TEXT("stopPie"), true);
	Scenario->SetObjectField(TEXT("cleanup"), Cleanup);

	OutProfile = CopyPerformanceObject(Standard);
	OutProfile->SetStringField(TEXT("kind"), TEXT("standardScenario"));
	OutProfile->SetNumberField(TEXT("warmupSeconds"), RequestedWarmup);
	OutProfile->SetNumberField(TEXT("sampleSeconds"), RequestedSample);
	OutProfile->SetNumberField(TEXT("repeatCount"), RepeatCount);
	const FString ProfileDigest = DigestJson(OutProfile);
	if (!ProfileDigest.IsEmpty())
	{
		OutProfile->SetStringField(
			TEXT("digest"),
			TEXT("sha256:") + ProfileDigest);
	}

	OutRequest->SetStringField(TEXT("mode"), TEXT("scenario"));
	OutRequest->SetObjectField(TEXT("scenario"), Scenario);
	OutRequest->SetNumberField(TEXT("repeatCount"), RepeatCount);
	// Standard warmup is an explicit Scenario step before metrics.begin.
	OutRequest->SetNumberField(TEXT("warmupSeconds"), 0.0);
	OutRequest->SetNumberField(TEXT("sampleSeconds"), RequestedSample);
	return true;
}

TSharedPtr<FJsonObject>
FPerformanceRegressionService::BuildRegressionSummary(
	const TSharedPtr<FJsonObject>& Comparison)
{
	TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetStringField(
		TEXT("schema"),
		TEXT("ue.performance-regression-summary.v1"));
	const FString Verdict =
		StringField(Comparison, TEXT("verdict"), TEXT("inconclusive"));
	Summary->SetStringField(TEXT("verdict"), Verdict);
	int32 Passed = 0;
	int32 Regressions = 0;
	int32 Inconclusive = 0;
	int32 Total = 0;
	if (Comparison.IsValid()
		&& Comparison->HasTypedField<EJson::Array>(TEXT("checks")))
	{
		for (const TSharedPtr<FJsonValue>& Value :
			Comparison->GetArrayField(TEXT("checks")))
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			++Total;
			const FString CheckVerdict =
				StringField(Value->AsObject(), TEXT("verdict"));
			if (CheckVerdict == TEXT("pass"))
			{
				++Passed;
			}
			else if (CheckVerdict == TEXT("regression"))
			{
				++Regressions;
			}
			else
			{
				++Inconclusive;
			}
		}
	}
	Summary->SetNumberField(TEXT("checkCount"), Total);
	Summary->SetNumberField(TEXT("passedCount"), Passed);
	Summary->SetNumberField(TEXT("regressionCount"), Regressions);
	Summary->SetNumberField(TEXT("inconclusiveCount"), Inconclusive);
	Summary->SetBoolField(
		TEXT("fingerprintCompatible"),
		BoolField(Comparison, TEXT("fingerprintCompatible"), true));
	if (Comparison.IsValid()
		&& Comparison->HasTypedField<EJson::Object>(
			TEXT("diagnosticTrace")))
	{
		Summary->SetObjectField(
			TEXT("diagnosticTrace"),
			CopyPerformanceObject(
				Comparison->GetObjectField(TEXT("diagnosticTrace"))));
	}
	return Summary;
}

FString FPerformanceRegressionService::BuildJUnitReport(
	const FString& ComparisonId,
	const TSharedPtr<FJsonObject>& Comparison)
{
	int32 TestCount = 0;
	int32 Failures = 0;
	int32 Skipped = 0;
	FString TestCases;
	const bool bComparisonInconclusive =
		StringField(Comparison, TEXT("verdict"))
			== TEXT("inconclusive");
	if (Comparison.IsValid()
		&& Comparison->HasTypedField<EJson::Array>(TEXT("checks")))
	{
		for (const TSharedPtr<FJsonValue>& Value :
			Comparison->GetArrayField(TEXT("checks")))
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Check = Value->AsObject();
			++TestCount;
			const FString Name = FString::Printf(
				TEXT("%s.%s"),
				*StringField(Check, TEXT("metric"), TEXT("unknown")),
				*StringField(Check, TEXT("statistic"), TEXT("unknown")));
			const FString Verdict =
				StringField(Check, TEXT("verdict"), TEXT("inconclusive"));
			TestCases += FString::Printf(
				TEXT("  <testcase classname=\"UEPerformance\" name=\"%s\">"),
				*EscapePerformanceXml(Name));
			if (Verdict == TEXT("regression")
				&& !bComparisonInconclusive)
			{
				++Failures;
				TestCases += FString::Printf(
					TEXT(
						"<failure message=\"regression\">baseline=%g "
						"candidate=%g regressionPercent=%g</failure>"),
					NumberField(Check, TEXT("baseline"), 0.0),
					NumberField(Check, TEXT("candidate"), 0.0),
					NumberField(Check, TEXT("regressionPercent"), 0.0));
			}
			else if (Verdict != TEXT("pass")
				|| bComparisonInconclusive)
			{
				++Skipped;
				TestCases += FString::Printf(
					TEXT("<skipped message=\"%s\"/>"),
					*EscapePerformanceXml(
						bComparisonInconclusive
							? StringField(
								Comparison,
								TEXT("reason"),
								TEXT("comparison inconclusive"))
							: StringField(
								Check,
								TEXT("reason"),
								TEXT("inconclusive"))));
			}
			TestCases += TEXT("</testcase>\n");
		}
	}
	if (TestCount == 0)
	{
		++TestCount;
		if (StringField(Comparison, TEXT("verdict")) == TEXT("pass"))
		{
			TestCases +=
				TEXT(
					"  <testcase classname=\"UEPerformance\" "
					"name=\"comparison\"/>\n");
		}
		else
		{
			++Skipped;
			TestCases += FString::Printf(
				TEXT(
					"  <testcase classname=\"UEPerformance\" "
					"name=\"comparison\"><skipped message=\"%s\"/>"
					"</testcase>\n"),
				*EscapePerformanceXml(
					StringField(
						Comparison,
						TEXT("reason"),
						TEXT("comparison inconclusive"))));
		}
	}
	return FString::Printf(
		TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n")
		TEXT(
			"<testsuite name=\"UEPerformanceRegression\" id=\"%s\" "
			"tests=\"%d\" failures=\"%d\" skipped=\"%d\">\n")
		TEXT("%s</testsuite>\n"),
		*EscapePerformanceXml(ComparisonId),
		TestCount,
		Failures,
		Skipped,
		*TestCases);
}

FMCPToolResult FPerformanceRegressionService::StartPerformanceRun(
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Normalized;
	TSharedPtr<FJsonObject> Profile;
	FString Error;
	if (!NormalizeRunRequest(
			Params,
			CurrentMapPackage(),
			Normalized,
			Profile,
			Error))
	{
		return FMCPToolResult::Error(
			Error,
			TEXT("invalid_performance_profile"),
			422);
	}
	if (Profile.IsValid() && GEditor)
	{
		if (const UWorld* World =
			GEditor->GetEditorWorldContext().World())
		{
			if (World->GetOutermost()
				&& World->GetOutermost()->IsDirty())
			{
				return FMCPToolResult::Error(
					TEXT(
						"standardScenario requires a saved, clean map so "
						"its package hash is reproducible."),
					TEXT("asset_dirty"),
					409);
			}
		}
	}
	const TSharedPtr<FJsonObject> Fingerprint =
		CaptureFingerprint(Profile);
	FMCPToolResult Result = Operation(
		TEXT("production.performance.run"),
		Normalized);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return Result;
	}
	FString RunId = StringField(Result.Data, TEXT("runId"));
	if (RunId.IsEmpty())
	{
		RunId = StringField(Result.Data, TEXT("jobId"));
	}
	if (!RunId.IsEmpty())
	{
		Fingerprints.Add(RunId, Fingerprint);
		PersistFingerprint(RunId, Fingerprint);
	}
	Result.Data->SetObjectField(
		TEXT("environmentFingerprint"),
		CopyPerformanceObject(Fingerprint));
	if (Profile.IsValid())
	{
		Result.Data->SetObjectField(
			TEXT("profile"),
			CopyPerformanceObject(Profile));
	}
	return Result;
}

FMCPToolResult FPerformanceRegressionService::GetPerformanceResult(
	const TSharedPtr<FJsonObject>& Params)
{
	FMCPToolResult Result = Operation(
		TEXT("production.performance.result.get"),
		Params);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return Result;
	}
	const FString RunId = StringField(Params, TEXT("runId"));
	const TSharedPtr<FJsonObject> Fingerprint = LoadFingerprint(RunId);
	if (!Fingerprint.IsValid())
	{
		return Result;
	}
	Result.Data->SetObjectField(
		TEXT("environmentFingerprint"),
		CopyPerformanceObject(Fingerprint));
	if (const TSharedPtr<FJsonObject> JobResult =
		FindResultObject(Result.Data))
	{
		if (JobResult->HasTypedField<EJson::Object>(TEXT("context")))
		{
			JobResult->GetObjectField(TEXT("context"))->SetObjectField(
				TEXT("environmentFingerprint"),
				CopyPerformanceObject(Fingerprint));
		}
	}
	return Result;
}

FMCPToolResult FPerformanceRegressionService::ComparePerformanceRuns(
	const TSharedPtr<FJsonObject>& Params)
{
	FMCPToolResult Result = Operation(
		TEXT("production.performance.compare"),
		Params);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return Result;
	}
	const FString BaselineRunId =
		StringField(Params, TEXT("baselineRunId"));
	const FString CandidateRunId =
		StringField(Params, TEXT("candidateRunId"));
	ApplyFingerprintCompatibility(
		BaselineRunId,
		CandidateRunId,
		Result.Data);
	Result.Data->SetObjectField(
		TEXT("regressionSummary"),
		BuildRegressionSummary(Result.Data));

	FRegressionJob Regression;
	Regression.Id = NewId(TEXT("perf-regression"));
	Regression.CreatedAtUtc = FDateTime::UtcNow().ToIso8601();
	Regression.Comparison = CopyPerformanceObject(Result.Data);

	const bool bRegression =
		StringField(Result.Data, TEXT("verdict")) == TEXT("regression");
	const bool bAutoTrace =
		BoolField(Params, TEXT("autoTraceOnRegression"), false);
	if (bRegression && bAutoTrace)
	{
		TSharedPtr<FJsonObject> Diagnostic =
			MakeShared<FJsonObject>();
		if (!Params.IsValid()
			|| !Params->HasTypedField<EJson::Object>(TEXT("traceRerun")))
		{
			Diagnostic->SetStringField(TEXT("status"), TEXT("notScheduled"));
			Diagnostic->SetStringField(
				TEXT("reason"),
				TEXT(
					"autoTraceOnRegression requires traceRerun so the "
					"candidate workload can be reproduced."));
			Regression.Comparison->SetObjectField(
				TEXT("diagnosticTrace"),
				Diagnostic);
		}
		else
		{
			TSharedPtr<FJsonObject> TraceRerun =
				CopyPerformanceObject(
					Params->GetObjectField(TEXT("traceRerun")));
			TraceRerun->SetBoolField(TEXT("captureTrace"), true);
			TraceRerun->SetStringField(
				TEXT("requestId"),
				NewId(TEXT("perf-trace-request")));
			const FMCPToolResult TraceRun =
				StartPerformanceRun(TraceRerun);
			if (!TraceRun.bSuccess || !TraceRun.Data.IsValid())
			{
				Diagnostic->SetStringField(TEXT("status"), TEXT("failed"));
				Diagnostic->SetStringField(
					TEXT("code"),
					TraceRun.ErrorCode);
				Diagnostic->SetStringField(
					TEXT("message"),
					TraceRun.ErrorMessage);
				Regression.Comparison->SetObjectField(
					TEXT("diagnosticTrace"),
					Diagnostic);
			}
			else
			{
				Regression.DiagnosticPerformanceRunId =
					StringField(TraceRun.Data, TEXT("runId"));
				Regression.Status = TEXT("running");
				Regression.Phase = TEXT("traceRerun");
				Diagnostic->SetStringField(TEXT("status"), TEXT("submitted"));
				Diagnostic->SetStringField(
					TEXT("performanceRunId"),
					Regression.DiagnosticPerformanceRunId);
				Regression.Comparison->SetObjectField(
					TEXT("diagnosticTrace"),
					Diagnostic);
			}
		}
	}

	WriteRegressionArtifacts(Regression);
	Result.Data = CopyPerformanceObject(Regression.Comparison);
	Result.Data->SetStringField(TEXT("comparisonId"), Regression.Id);
	Result.Data->SetStringField(TEXT("jobId"), Regression.Id);
	Result.Data->SetObjectField(
		TEXT("regressionJob"),
		MakeRegressionJobSummary(Regression, false));
	RegressionJobs.Add(Regression.Id, MoveTemp(Regression));

	while (RegressionJobs.Num() > MaxRegressionJobs)
	{
		FString Oldest;
		FString OldestTime;
		for (const TPair<FString, FRegressionJob>& Pair : RegressionJobs)
		{
			if (Pair.Value.Status == TEXT("running"))
			{
				continue;
			}
			if (Oldest.IsEmpty()
				|| Pair.Value.CreatedAtUtc < OldestTime)
			{
				Oldest = Pair.Key;
				OldestTime = Pair.Value.CreatedAtUtc;
			}
		}
		if (Oldest.IsEmpty())
		{
			break;
		}
		RegressionJobs.Remove(Oldest);
	}
	return Result;
}

FMCPToolResult FPerformanceRegressionService::GetRegressionJob(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = StringField(Params, TEXT("jobId"));
	const FRegressionJob* Job = RegressionJobs.Find(JobId);
	if (!Job)
	{
		return FMCPToolResult::Error(
			TEXT("Performance regression job was not found."),
			TEXT("job_not_found"),
			404);
	}
	return FMCPToolResult::Ok(
		MakeRegressionJobSummary(
			*Job,
			CapabilityId == TEXT("production.job.result.get")));
}

FMCPToolResult FPerformanceRegressionService::GetRegressionArtifact(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString JobId = StringField(Params, TEXT("jobId"));
	const FString ArtifactId =
		StringField(Params, TEXT("artifactId"));
	const FRegressionJob* Job = RegressionJobs.Find(JobId);
	if (!Job)
	{
		return FMCPToolResult::Error(
			TEXT("Performance regression job was not found."),
			TEXT("job_not_found"),
			404);
	}
	const FArtifact* Artifact = Job->Artifacts.FindByPredicate(
		[&ArtifactId](const FArtifact& Candidate)
		{
			return Candidate.Id == ArtifactId;
		});
	if (!Artifact)
	{
		return FMCPToolResult::Error(
			TEXT("Performance regression artifact was not found."),
			TEXT("artifact_not_found"),
			404);
	}
	if (IFileManager::Get().FileSize(*Artifact->Path) != Artifact->Size
		|| IFileManager::Get().GetTimeStamp(*Artifact->Path)
			!= Artifact->ModifiedAtUtc)
	{
		return FMCPToolResult::Error(
			TEXT("The regression artifact changed after registration."),
			TEXT("artifact_changed"),
			409);
	}
	TUniquePtr<IFileHandle> File(
		FPlatformFileManager::Get().GetPlatformFile().OpenRead(
			*Artifact->Path));
	if (!File.IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("The regression artifact is unavailable."),
			TEXT("artifact_unavailable"),
			410);
	}
	const int64 Offset = static_cast<int64>(
		NumberField(Params, TEXT("offset"), 0.0));
	if (Offset < 0 || Offset > Artifact->Size)
	{
		return FMCPToolResult::Error(
			TEXT("offset is outside the regression artifact."),
			TEXT("artifact_offset_invalid"),
			416);
	}
	const int32 MaxBytes = FMath::Clamp(
		static_cast<int32>(
			NumberField(
				Params,
				TEXT("maxBytes"),
				PerformanceMaxArtifactChunkBytes)),
		1,
		PerformanceMaxArtifactChunkBytes);
	const int32 ReadSize = static_cast<int32>(
		FMath::Min<int64>(MaxBytes, Artifact->Size - Offset));
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(ReadSize);
	if (!File->Seek(Offset)
		|| (ReadSize > 0 && !File->Read(Bytes.GetData(), ReadSize)))
	{
		return FMCPToolResult::Error(
			TEXT("The regression artifact could not be read."),
			TEXT("artifact_unavailable"),
			500);
	}
	TSharedPtr<FJsonObject> Data = MakeArtifactSummary(*Artifact);
	Data->SetStringField(TEXT("jobId"), JobId);
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("nextOffset"), Offset + ReadSize);
	Data->SetBoolField(
		TEXT("eof"),
		Offset + ReadSize >= Artifact->Size);
	Data->SetStringField(TEXT("contentBase64"), FBase64::Encode(Bytes));
	return FMCPToolResult::Ok(Data);
}

void FPerformanceRegressionService::TickRegressionJob(
	FRegressionJob& Job)
{
	if (Job.Phase == TEXT("traceRerun"))
	{
		TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
		Query->SetStringField(
			TEXT("runId"),
			Job.DiagnosticPerformanceRunId);
		const FMCPToolResult Result = Operation(
			TEXT("production.performance.result.get"),
			Query);
		if (!Result.bSuccess || !Result.Data.IsValid())
		{
			FinishDiagnostic(
				Job,
				TEXT("failed"),
				Result.ErrorCode,
				Result.ErrorMessage);
			return;
		}
		const FString Status = ResponseStatus(Result.Data);
		if (!IsTerminal(Status))
		{
			return;
		}
		if (Status != TEXT("succeeded"))
		{
			FinishDiagnostic(
				Job,
				TEXT("failed"),
				TEXT("diagnostic_performance_failed"),
				TEXT("The automatic trace performance rerun failed."));
			return;
		}
		const TSharedPtr<FJsonObject> PerformanceResult =
			FindResultObject(Result.Data);
		Job.TraceId =
			StringField(PerformanceResult, TEXT("traceId"));
		if (Job.TraceId.IsEmpty())
		{
			FinishDiagnostic(
				Job,
				TEXT("failed"),
				TEXT("trace_artifact_unavailable"),
				TEXT("The diagnostic performance rerun produced no trace."));
			return;
		}
		TSharedPtr<FJsonObject> Analyze = MakeShared<FJsonObject>();
		Analyze->SetStringField(TEXT("traceId"), Job.TraceId);
		Analyze->SetNumberField(TEXT("maxDurationSeconds"), 60.0);
		Analyze->SetNumberField(TEXT("maxFrames"), 2000);
		Analyze->SetNumberField(TEXT("maxTimers"), 25);
		Analyze->SetNumberField(TEXT("maxCounters"), 32);
		Analyze->SetNumberField(TEXT("maxCounterValues"), 2000);
		Analyze->SetNumberField(TEXT("timeoutSeconds"), 180.0);
		const FMCPToolResult Analysis = Operation(
			TEXT("production.trace.analyze"),
			Analyze);
		if (!Analysis.bSuccess || !Analysis.Data.IsValid())
		{
			FinishDiagnostic(
				Job,
				TEXT("failed"),
				Analysis.ErrorCode,
				Analysis.ErrorMessage);
			return;
		}
		Job.TraceAnalysisJobId =
			StringField(Analysis.Data, TEXT("jobId"));
		Job.Phase = TEXT("traceAnalysis");
		TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
		Diagnostic->SetStringField(TEXT("status"), TEXT("analyzing"));
		Diagnostic->SetStringField(
			TEXT("performanceRunId"),
			Job.DiagnosticPerformanceRunId);
		Diagnostic->SetStringField(TEXT("traceId"), Job.TraceId);
		Diagnostic->SetStringField(
			TEXT("analysisJobId"),
			Job.TraceAnalysisJobId);
		Job.Comparison->SetObjectField(
			TEXT("diagnosticTrace"),
			Diagnostic);
		WriteRegressionArtifacts(Job);
		return;
	}
	if (Job.Phase != TEXT("traceAnalysis"))
	{
		return;
	}
	TSharedPtr<FJsonObject> Query = MakeShared<FJsonObject>();
	Query->SetStringField(TEXT("jobId"), Job.TraceAnalysisJobId);
	const FMCPToolResult Result = Operation(
		TEXT("production.job.result.get"),
		Query);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		FinishDiagnostic(
			Job,
			TEXT("failed"),
			Result.ErrorCode,
			Result.ErrorMessage);
		return;
	}
	const FString Status = ResponseStatus(Result.Data);
	if (!IsTerminal(Status))
	{
		return;
	}
	if (Status != TEXT("succeeded"))
	{
		FinishDiagnostic(
			Job,
			TEXT("failed"),
			TEXT("trace_analysis_failed"),
			TEXT("The automatic bounded trace analysis failed."));
		return;
	}
	TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
	Diagnostic->SetStringField(TEXT("status"), TEXT("succeeded"));
	Diagnostic->SetStringField(
		TEXT("performanceRunId"),
		Job.DiagnosticPerformanceRunId);
	Diagnostic->SetStringField(TEXT("traceId"), Job.TraceId);
	Diagnostic->SetStringField(
		TEXT("analysisJobId"),
		Job.TraceAnalysisJobId);
	if (const TSharedPtr<FJsonObject> AnalysisResult =
		FindResultObject(Result.Data))
	{
		if (AnalysisResult->HasTypedField<EJson::Object>(TEXT("analysis")))
		{
			const TSharedPtr<FJsonObject> Analysis =
				AnalysisResult->GetObjectField(TEXT("analysis"));
			if (Analysis->HasTypedField<EJson::Array>(TEXT("timers")))
			{
				Diagnostic->SetArrayField(
					TEXT("topScopes"),
					Analysis->GetArrayField(TEXT("timers")));
			}
			if (Analysis->HasTypedField<EJson::Object>(
				TEXT("threadGroups")))
			{
				Diagnostic->SetObjectField(
					TEXT("threadGroups"),
					CopyPerformanceObject(
						Analysis->GetObjectField(
							TEXT("threadGroups"))));
			}
		}
	}
	if (Result.Data->HasTypedField<EJson::Array>(TEXT("artifacts")))
	{
		Diagnostic->SetArrayField(
			TEXT("analysisArtifacts"),
			Result.Data->GetArrayField(TEXT("artifacts")));
	}
	Job.Comparison->SetObjectField(TEXT("diagnosticTrace"), Diagnostic);
	Job.Status = TEXT("succeeded");
	Job.Phase = TEXT("complete");
	Job.Comparison->SetObjectField(
		TEXT("regressionSummary"),
		BuildRegressionSummary(Job.Comparison));
	WriteRegressionArtifacts(Job);
}

void FPerformanceRegressionService::FinishDiagnostic(
	FRegressionJob& Job,
	const FString& Status,
	const FString& Code,
	const FString& Message)
{
	TSharedPtr<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
	Diagnostic->SetStringField(TEXT("status"), Status);
	Diagnostic->SetStringField(
		TEXT("performanceRunId"),
		Job.DiagnosticPerformanceRunId);
	if (!Job.TraceId.IsEmpty())
	{
		Diagnostic->SetStringField(TEXT("traceId"), Job.TraceId);
	}
	if (!Job.TraceAnalysisJobId.IsEmpty())
	{
		Diagnostic->SetStringField(
			TEXT("analysisJobId"),
			Job.TraceAnalysisJobId);
	}
	if (!Code.IsEmpty())
	{
		Diagnostic->SetStringField(TEXT("code"), Code);
	}
	if (!Message.IsEmpty())
	{
		Diagnostic->SetStringField(TEXT("message"), Message);
	}
	Job.Comparison->SetObjectField(TEXT("diagnosticTrace"), Diagnostic);
	// Diagnostic capture failure does not erase the original comparison verdict.
	Job.Status = TEXT("succeeded");
	Job.Phase = TEXT("complete");
	Job.Comparison->SetObjectField(
		TEXT("regressionSummary"),
		BuildRegressionSummary(Job.Comparison));
	WriteRegressionArtifacts(Job);
}

void FPerformanceRegressionService::ApplyFingerprintCompatibility(
	const FString& BaselineRunId,
	const FString& CandidateRunId,
	TSharedPtr<FJsonObject>& Comparison) const
{
	const TSharedPtr<FJsonObject> Baseline =
		LoadFingerprint(BaselineRunId);
	const TSharedPtr<FJsonObject> Candidate =
		LoadFingerprint(CandidateRunId);
	if (!Baseline.IsValid() || !Candidate.IsValid())
	{
		Comparison->SetBoolField(TEXT("fingerprintCompatible"), false);
		Comparison->SetStringField(
			TEXT("fingerprintStatus"),
			TEXT("unavailable"));
		Comparison->SetStringField(
			TEXT("verdict"),
			TEXT("inconclusive"));
		Comparison->SetStringField(
			TEXT("reason"),
			TEXT(
				"One or both runs predate the extended environment "
				"fingerprint."));
		return;
	}
	Comparison->SetObjectField(
		TEXT("baselineFingerprint"),
		CopyPerformanceObject(Baseline));
	Comparison->SetObjectField(
		TEXT("candidateFingerprint"),
		CopyPerformanceObject(Candidate));
	const FString BaselineCompatibility =
		StringField(Baseline, TEXT("compatibilityKey"));
	const FString CandidateCompatibility =
		StringField(Candidate, TEXT("compatibilityKey"));
	const bool bCompatible =
		!BaselineCompatibility.IsEmpty()
		&& BaselineCompatibility == CandidateCompatibility;
	Comparison->SetBoolField(TEXT("fingerprintCompatible"), bCompatible);
	Comparison->SetStringField(
		TEXT("fingerprintStatus"),
		bCompatible ? TEXT("compatible") : TEXT("mismatch"));
	if (!bCompatible)
	{
		TArray<TSharedPtr<FJsonValue>> Mismatches;
		static const TCHAR* Fields[] = {
			TEXT("mapPackage"),
			TEXT("mapDirtyState"),
			TEXT("rhi"),
			TEXT("resolution"),
			TEXT("gpuAdapter"),
			TEXT("gpuDriver"),
			TEXT("profileDigest"),
			TEXT("cvarDigest"),
			TEXT("scalabilityDigest")
		};
		for (const TCHAR* Field : Fields)
		{
			const FString Before = StringField(Baseline, Field);
			const FString After = StringField(Candidate, Field);
			if (Before == After)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Mismatch = MakeShared<FJsonObject>();
			Mismatch->SetStringField(TEXT("field"), Field);
			Mismatch->SetStringField(TEXT("baseline"), Before);
			Mismatch->SetStringField(TEXT("candidate"), After);
			Mismatches.Add(MakeShared<FJsonValueObject>(Mismatch));
		}
		Comparison->SetArrayField(
			TEXT("fingerprintMismatches"),
			Mismatches);
		Comparison->SetStringField(TEXT("verdict"), TEXT("inconclusive"));
		Comparison->SetStringField(
			TEXT("reason"),
			TEXT(
				"Performance fingerprint differs in a regression-critical "
				"environment field."));
	}
	TSharedPtr<FJsonObject> Changes = MakeShared<FJsonObject>();
	Changes->SetStringField(
		TEXT("baselineGitRevision"),
		StringField(Baseline, TEXT("gitRevision")));
	Changes->SetStringField(
		TEXT("candidateGitRevision"),
		StringField(Candidate, TEXT("gitRevision")));
	Changes->SetStringField(
		TEXT("baselineMapPackageHash"),
		StringField(Baseline, TEXT("mapPackageHash")));
	Changes->SetStringField(
		TEXT("candidateMapPackageHash"),
		StringField(Candidate, TEXT("mapPackageHash")));
	Comparison->SetObjectField(TEXT("changeEvidence"), Changes);
}

void FPerformanceRegressionService::WriteRegressionArtifacts(
	FRegressionJob& Job)
{
	const FString Directory = RegressionDirectory(Job.Id);
	IFileManager::Get().MakeDirectory(*Directory, true);
	Job.Comparison->SetObjectField(
		TEXT("regressionSummary"),
		BuildRegressionSummary(Job.Comparison));
	TSharedPtr<FJsonObject> Report =
		CopyPerformanceObject(Job.Comparison);
	Report->SetStringField(
		TEXT("schema"),
		TEXT("ue.performance-regression.v1"));
	Report->SetStringField(TEXT("comparisonId"), Job.Id);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (FJsonSerializer::Serialize(Report.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(
			Json,
			*FPaths::Combine(Directory, TEXT("regression.json")));
	}
	FFileHelper::SaveStringToFile(
		BuildJUnitReport(Job.Id, Job.Comparison),
		*FPaths::Combine(Directory, TEXT("junit.xml")));

	Job.Artifacts.Reset();
	const auto AddArtifact =
		[&Job](const FString& Path, const FString& Name, const FString& MimeType)
		{
			const int64 Size = IFileManager::Get().FileSize(*Path);
			if (Size < 0)
			{
				return;
			}
			FArtifact Artifact;
			Artifact.Id =
				Name == TEXT("junit.xml")
					? TEXT("junit")
					: TEXT("regression-json");
			Artifact.Name = Name;
			Artifact.Path = Path;
			Artifact.MimeType = MimeType;
			Artifact.Size = Size;
			Artifact.ModifiedAtUtc =
				IFileManager::Get().GetTimeStamp(*Path);
			Artifact.Sha256 = ComputeFileSha256Streaming(Path);
			Job.Artifacts.Add(MoveTemp(Artifact));
		};
	AddArtifact(
		FPaths::Combine(Directory, TEXT("regression.json")),
		TEXT("regression.json"),
		TEXT("application/json"));
	AddArtifact(
		FPaths::Combine(Directory, TEXT("junit.xml")),
		TEXT("junit.xml"),
		TEXT("application/xml"));
}

void FPerformanceRegressionService::PersistFingerprint(
	const FString& RunId,
	const TSharedPtr<FJsonObject>& Fingerprint) const
{
	if (RunId.IsEmpty() || !Fingerprint.IsValid())
	{
		return;
	}
	const FString Directory = FingerprintDirectory(RunId);
	IFileManager::Get().MakeDirectory(*Directory, true);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Json);
	if (FJsonSerializer::Serialize(Fingerprint.ToSharedRef(), Writer))
	{
		FFileHelper::SaveStringToFile(
			Json,
			*FPaths::Combine(Directory, TEXT("fingerprint.json")));
	}
}

TSharedPtr<FJsonObject> FPerformanceRegressionService::LoadFingerprint(
	const FString& RunId) const
{
	if (const TSharedPtr<FJsonObject>* Cached = Fingerprints.Find(RunId))
	{
		return *Cached;
	}
	FString Json;
	if (!FFileHelper::LoadFileToString(
			Json,
			*FPaths::Combine(
				FingerprintDirectory(RunId),
				TEXT("fingerprint.json"))))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Fingerprint;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Json);
	return FJsonSerializer::Deserialize(Reader, Fingerprint)
		&& Fingerprint.IsValid()
			? Fingerprint
			: nullptr;
}

TSharedPtr<FJsonObject>
FPerformanceRegressionService::CaptureFingerprint(
	const TSharedPtr<FJsonObject>& Profile)
{
	TSharedPtr<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(
		TEXT("schema"),
		TEXT("ue.performance-fingerprint.v2"));
	Fingerprint->SetStringField(
		TEXT("capturedAtUtc"),
		FDateTime::UtcNow().ToIso8601());
	Fingerprint->SetStringField(TEXT("gitRevision"), GitRevision());
	const FString MapPackage = CurrentMapPackage();
	Fingerprint->SetStringField(TEXT("mapPackage"), MapPackage);
	bool bMapDirty = false;
	if (GEditor)
	{
		if (const UWorld* World =
			GEditor->GetEditorWorldContext().World())
		{
			bMapDirty =
				World->GetOutermost()
				&& World->GetOutermost()->IsDirty();
		}
	}
	Fingerprint->SetBoolField(TEXT("mapDirty"), bMapDirty);
	Fingerprint->SetStringField(
		TEXT("mapDirtyState"),
		bMapDirty ? TEXT("dirty") : TEXT("clean"));
	FString MapFilename;
	if (!MapPackage.IsEmpty()
		&& FPackageName::DoesPackageExist(MapPackage, &MapFilename))
	{
		const FString FullMapFilename =
			FPaths::ConvertRelativePathToFull(MapFilename);
		const FString MapHash =
			ComputeFileSha256Streaming(FullMapFilename);
		Fingerprint->SetStringField(
			TEXT("mapPackageHash"),
			MapHash.IsEmpty()
				? TEXT("unavailable")
				: TEXT("sha256:") + MapHash);
		Fingerprint->SetNumberField(
			TEXT("mapPackageSizeBytes"),
			static_cast<double>(
				IFileManager::Get().FileSize(*FullMapFilename)));
	}
	else
	{
		Fingerprint->SetStringField(
			TEXT("mapPackageHash"),
			TEXT("unavailable"));
	}
	Fingerprint->SetStringField(
		TEXT("rhi"),
		GDynamicRHI ? FString(GDynamicRHI->GetName()) : TEXT("unavailable"));
	Fingerprint->SetStringField(TEXT("gpuAdapter"), GRHIAdapterName);
	Fingerprint->SetStringField(
		TEXT("gpuDriver"),
		GRHIAdapterUserDriverVersion);
	Fingerprint->SetStringField(
		TEXT("gpuDriverInternal"),
		GRHIAdapterInternalDriverVersion);
	Fingerprint->SetNumberField(TEXT("gpuVendorId"), GRHIVendorId);
	Fingerprint->SetNumberField(TEXT("gpuDeviceId"), GRHIDeviceId);
	Fingerprint->SetStringField(
		TEXT("resolution"),
		FString::Printf(
			TEXT("%dx%d"),
			GSystemResolution.ResX,
			GSystemResolution.ResY));
	const TSharedPtr<FJsonObject> CVars = MakeCVarSnapshot();
	Fingerprint->SetObjectField(TEXT("cvars"), CVars);
	const TSharedPtr<FJsonObject> Scalability =
		MakeScalabilitySnapshot(CVars);
	Fingerprint->SetObjectField(TEXT("scalability"), Scalability);
	const FString CVarDigest = DigestJson(CVars);
	const FString ScalabilityDigest = DigestJson(Scalability);
	Fingerprint->SetStringField(
		TEXT("cvarDigest"),
		CVarDigest.IsEmpty() ? FString() : TEXT("sha256:") + CVarDigest);
	Fingerprint->SetStringField(
		TEXT("scalabilityDigest"),
		ScalabilityDigest.IsEmpty()
			? FString()
			: TEXT("sha256:") + ScalabilityDigest);
	Fingerprint->SetStringField(
		TEXT("vsync"),
		StringField(CVars, TEXT("r.VSync"), TEXT("unavailable")));
	Fingerprint->SetStringField(
		TEXT("fpsCap"),
		StringField(CVars, TEXT("t.MaxFPS"), TEXT("unavailable")));
	Fingerprint->SetStringField(
		TEXT("screenPercentage"),
		StringField(
			CVars,
			TEXT("r.ScreenPercentage"),
			TEXT("unavailable")));
	if (Profile.IsValid())
	{
		Fingerprint->SetObjectField(
			TEXT("profile"),
			CopyPerformanceObject(Profile));
		Fingerprint->SetStringField(
			TEXT("profileDigest"),
			StringField(Profile, TEXT("digest")));
	}

	TSharedPtr<FJsonObject> Compatibility = MakeShared<FJsonObject>();
	Compatibility->SetStringField(TEXT("mapPackage"), MapPackage);
	Compatibility->SetBoolField(TEXT("mapDirty"), bMapDirty);
	Compatibility->SetStringField(
		TEXT("rhi"),
		StringField(Fingerprint, TEXT("rhi")));
	Compatibility->SetStringField(
		TEXT("gpuAdapter"),
		StringField(Fingerprint, TEXT("gpuAdapter")));
	Compatibility->SetStringField(
		TEXT("gpuDriver"),
		StringField(Fingerprint, TEXT("gpuDriver")));
	Compatibility->SetStringField(
		TEXT("resolution"),
		StringField(Fingerprint, TEXT("resolution")));
	Compatibility->SetStringField(
		TEXT("profileDigest"),
		StringField(Fingerprint, TEXT("profileDigest")));
	Compatibility->SetObjectField(
		TEXT("cvars"),
		CopyPerformanceObject(CVars));
	Compatibility->SetObjectField(
		TEXT("scalability"),
		CopyPerformanceObject(
			Fingerprint->GetObjectField(TEXT("scalability"))));
	const FString CompatibilityDigest = DigestJson(Compatibility);
	Fingerprint->SetStringField(
		TEXT("compatibilityKey"),
		CompatibilityDigest.IsEmpty()
			? FString()
			: TEXT("sha256:") + CompatibilityDigest);

	TSharedPtr<FJsonObject> Stable =
		CopyPerformanceObject(Fingerprint);
	Stable->RemoveField(TEXT("capturedAtUtc"));
	const FString FullDigest = DigestJson(Stable);
	Fingerprint->SetStringField(
		TEXT("fingerprint"),
		FullDigest.IsEmpty() ? FString() : TEXT("sha256:") + FullDigest);
	return Fingerprint;
}

TSharedPtr<FJsonObject>
FPerformanceRegressionService::MakeArtifactSummary(
	const FArtifact& Artifact)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.artifact.v1"));
	Data->SetStringField(TEXT("artifactId"), Artifact.Id);
	Data->SetStringField(TEXT("kind"), TEXT("report"));
	Data->SetStringField(TEXT("name"), Artifact.Name);
	Data->SetStringField(TEXT("mimeType"), Artifact.MimeType);
	Data->SetNumberField(
		TEXT("sizeBytes"),
		static_cast<double>(Artifact.Size));
	Data->SetStringField(TEXT("sha256"), Artifact.Sha256);
	return Data;
}

TSharedPtr<FJsonObject>
FPerformanceRegressionService::MakeRegressionJobSummary(
	const FRegressionJob& Job,
	const bool bIncludeResult)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.job.v1"));
	Data->SetStringField(TEXT("jobId"), Job.Id);
	Data->SetStringField(TEXT("kind"), TEXT("performanceRegression"));
	Data->SetStringField(TEXT("status"), Job.Status);
	Data->SetStringField(TEXT("phase"), Job.Phase);
	Data->SetStringField(TEXT("createdAtUtc"), Job.CreatedAtUtc);
	TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
	Progress->SetStringField(TEXT("phase"), Job.Phase);
	Progress->SetNumberField(
		TEXT("fraction"),
		Job.Status == TEXT("running") ? 0.5 : 1.0);
	Data->SetObjectField(TEXT("progress"), Progress);
	TArray<TSharedPtr<FJsonValue>> ArtifactValues;
	TArray<TSharedPtr<FJsonValue>> ArtifactRefs;
	for (const FArtifact& Artifact : Job.Artifacts)
	{
		ArtifactValues.Add(
			MakeShared<FJsonValueObject>(
				MakeArtifactSummary(Artifact)));
		ArtifactRefs.Add(
			MakeShared<FJsonValueString>(Artifact.Id));
	}
	Data->SetArrayField(TEXT("artifacts"), ArtifactValues);
	Data->SetArrayField(TEXT("artifactRefs"), ArtifactRefs);
	if (bIncludeResult)
	{
		Data->SetObjectField(
			TEXT("result"),
			CopyPerformanceObject(Job.Comparison));
	}
	return Data;
}

FString FPerformanceRegressionService::CurrentMapPackage()
{
	if (GEditor)
	{
		if (const UWorld* World =
			GEditor->GetEditorWorldContext().World())
		{
			return NormalizeMapName(World->GetOutermost()->GetName());
		}
	}
	return FString();
}

FString FPerformanceRegressionService::FingerprintDirectory(
	const FString& RunId)
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("Performance"),
		RunId);
}

FString FPerformanceRegressionService::RegressionDirectory(
	const FString& ComparisonId)
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("PerformanceRegressions"),
		ComparisonId);
}

FString FPerformanceRegressionService::NewId(const TCHAR* Prefix)
{
	return FString::Printf(
		TEXT("%s-%s"),
		Prefix,
		*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
}
}
