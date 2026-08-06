#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "DynamicRHI.h"
#include "Core/MCPExecutor.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Sha256.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "RenderUtils.h"
#include "Runtime/Launch/Resources/Version.h"
#include "RHIGlobals.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShaderCore.h"
#include "Selection.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 3
#include "StrataVisualizationData.h"
#define UEAI_TEST_HAS_STRATA_VISUALIZATION 1
#else
#define UEAI_TEST_HAS_STRATA_VISUALIZATION 0
#endif
#include "Tools/MCPToolRegistry.h"

namespace UEAIIntegrationTools
{
void RegisterViewportVisualizationTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller);
}

namespace
{
using UEAIIntegration::Infrastructure::FPIESessionController;

FString VisualizationArtifactDirectory()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("ViewportVisualization"));
}

void DeleteVisualizationArtifact(const FString& CaptureId)
{
	if (CaptureId.IsEmpty())
	{
		return;
	}
	IFileManager::Get().Delete(
		*FPaths::Combine(
			VisualizationArtifactDirectory(),
			CaptureId + TEXT(".png")),
		false,
		true);
	IFileManager::Get().Delete(
		*FPaths::Combine(
			VisualizationArtifactDirectory(),
			CaptureId + TEXT(".json")),
		false,
		true);
}

bool EncodeFixturePng(
	const TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	TArray<uint8>& OutPng)
{
	IImageWrapperModule& Module =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(
			TEXT("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Wrapper =
		Module.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid()
		|| !Wrapper->SetRaw(
			Pixels.GetData(),
			Pixels.Num() * sizeof(FColor),
			Width,
			Height,
			ERGBFormat::BGRA,
			8))
	{
		return false;
	}
	const TArray64<uint8> Compressed = Wrapper->GetCompressed(100);
	if (Compressed.IsEmpty() || Compressed.Num() > MAX_int32)
	{
		return false;
	}
	OutPng.SetNumUninitialized(static_cast<int32>(Compressed.Num()));
	FMemory::Memcpy(
		OutPng.GetData(),
		Compressed.GetData(),
		Compressed.Num());
	return true;
}

TSharedRef<FJsonObject> MakeFixtureFingerprint(
	const FString& Family,
	const FString& Mode,
	const int32 Width,
	const int32 Height)
{
	TSharedRef<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(TEXT("targetKind"), TEXT("editor"));
	Fingerprint->SetStringField(TEXT("family"), Family);
	Fingerprint->SetStringField(TEXT("mode"), Mode);
	Fingerprint->SetNumberField(TEXT("width"), Width);
	Fingerprint->SetNumberField(TEXT("height"), Height);
	Fingerprint->SetStringField(TEXT("rhi"), TEXT("fixture-rhi"));
	Fingerprint->SetStringField(TEXT("gpuAdapter"), TEXT("fixture-gpu"));
	Fingerprint->SetStringField(TEXT("featureLevel"), TEXT("SM5"));
	Fingerprint->SetStringField(TEXT("engineVersion"), TEXT("5.3.fixture"));
	Fingerprint->SetStringField(TEXT("pluginVersion"), TEXT("fixture"));
	Fingerprint->SetStringField(TEXT("showFlags"), TEXT("fixture-flags"));
	TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
	Camera->SetStringField(TEXT("source"), TEXT("fixture"));
	Camera->SetNumberField(TEXT("fovDegrees"), 90.0);
	Fingerprint->SetObjectField(TEXT("camera"), Camera);
	return Fingerprint;
}

bool WriteVisualizationFixture(
	const FString& CaptureId,
	const TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	const FString& Family,
	const FString& Mode,
	FString& OutError)
{
	TArray<uint8> Png;
	if (!EncodeFixturePng(Pixels, Width, Height, Png))
	{
		OutError = TEXT("Could not encode fixture PNG.");
		return false;
	}
	FString ImageHash;
	if (!UEAIIntegration::Infrastructure::TrySha256Hex(Png, ImageHash))
	{
		OutError = TEXT("Could not hash fixture PNG.");
		return false;
	}
	TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetStringField(
		TEXT("schema"),
		TEXT("ue.viewport-visualization-capture.v1"));
	Metadata->SetStringField(TEXT("captureId"), CaptureId);
	Metadata->SetStringField(TEXT("format"), TEXT("png"));
	Metadata->SetStringField(TEXT("mimeType"), TEXT("image/png"));
	Metadata->SetStringField(TEXT("imageSha256"), ImageHash);
	Metadata->SetStringField(TEXT("family"), Family);
	Metadata->SetStringField(TEXT("mode"), Mode);
	Metadata->SetStringField(TEXT("targetKind"), TEXT("editor"));
	Metadata->SetNumberField(TEXT("width"), Width);
	Metadata->SetNumberField(TEXT("height"), Height);
	Metadata->SetObjectField(
		TEXT("renderFingerprint"),
		MakeFixtureFingerprint(Family, Mode, Width, Height));

	FString MetadataJson;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&MetadataJson);
	IFileManager::Get().MakeDirectory(
		*VisualizationArtifactDirectory(),
		true);
	if (!FJsonSerializer::Serialize(Metadata, Writer)
		|| !FFileHelper::SaveArrayToFile(
			Png,
			*FPaths::Combine(
				VisualizationArtifactDirectory(),
				CaptureId + TEXT(".png")))
		|| !FFileHelper::SaveStringToFile(
			MetadataJson,
			*FPaths::Combine(
				VisualizationArtifactDirectory(),
				CaptureId + TEXT(".json")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		DeleteVisualizationArtifact(CaptureId);
		OutError = TEXT("Could not persist visualization fixture.");
		return false;
	}
	return true;
}

void RegisterVisualizationTools(
	FMCPToolRegistry& Registry,
	FPIESessionController& Controller)
{
	Registry.BeginDomainRegistration(TEXT("scene"));
	UEAIIntegrationTools::RegisterViewportVisualizationTools(
		Registry,
		Controller);
	Registry.EndDomainRegistration();
}

TSharedRef<FJsonObject> MakeCompareParams(
	const FString& Before,
	const FString& After)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("beforeCaptureId"), Before);
	Params->SetStringField(TEXT("afterCaptureId"), After);
	Params->SetNumberField(TEXT("pixelThreshold"), 0.0);
	return Params;
}

TSharedPtr<FJsonObject> FindCatalogMode(
	const TArray<TSharedPtr<FJsonValue>>& Modes,
	const FString& Family,
	const FString& Mode = FString())
{
	for (const TSharedPtr<FJsonValue>& Value : Modes)
	{
		const TSharedPtr<FJsonObject> Candidate = Value->AsObject();
		if (!Candidate.IsValid()
			|| !Candidate->GetStringField(TEXT("family")).Equals(
				Family,
				ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (Mode.IsEmpty()
			|| Candidate->GetStringField(TEXT("mode")).Equals(
				Mode,
				ESearchCase::IgnoreCase))
		{
			return Candidate;
		}
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewportVisualizationContractTest,
	"UE_AI_integration.ViewportVisualization.ContractAndNullRHI",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FViewportVisualizationContractTest::RunTest(const FString& Parameters)
{
	FPIESessionController Controller;
	FMCPToolRegistry Registry;
	RegisterVisualizationTools(Registry, Controller);

	TestEqual(TEXT("Exactly four visualization tools register"), Registry.Num(), 4);
	static const TCHAR* const ExpectedCapabilities[] = {
		TEXT("scene.viewport.visualization.list"),
		TEXT("scene.viewport.visualization.capture"),
		TEXT("scene.viewport.visualization.compare"),
		TEXT("scene.viewport.visualization.analyze")
	};
	for (const TCHAR* Capability : ExpectedCapabilities)
	{
		TestNotNull(Capability, Registry.FindTool(Capability));
	}

	TSharedRef<FJsonObject> InvalidTargetParams = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> InvalidTarget = MakeShared<FJsonObject>();
	InvalidTarget->SetStringField(TEXT("kind"), TEXT("standalone"));
	InvalidTargetParams->SetObjectField(TEXT("target"), InvalidTarget);
	const FMCPToolResult InvalidTargetResult = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.list"),
		InvalidTargetParams);
	TestFalse(TEXT("Unsupported target kind is rejected"), InvalidTargetResult.bSuccess);
	TestEqual(
		TEXT("Unsupported target has a stable code"),
		InvalidTargetResult.ErrorCode,
		FString(TEXT("invalid_request")));
	TSharedRef<FJsonObject> IncompletePIEParams = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> IncompletePIETarget = MakeShared<FJsonObject>();
	IncompletePIETarget->SetStringField(TEXT("kind"), TEXT("pie"));
	IncompletePIEParams->SetObjectField(TEXT("target"), IncompletePIETarget);
	const FMCPToolResult IncompletePIEResult = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.list"),
		IncompletePIEParams);
	TestFalse(
		TEXT("PIE target requires exact session identity"),
		IncompletePIEResult.bSuccess);
	TestEqual(
		TEXT("Incomplete PIE target has a stable code"),
		IncompletePIEResult.ErrorCode,
		FString(TEXT("invalid_request")));

	const FMCPToolResult ListResult = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.list"),
		MakeShared<FJsonObject>());
	TestTrue(
		TEXT("Mode catalog remains queryable without a rendered viewport"),
		ListResult.bSuccess);
	if (!ListResult.bSuccess)
	{
		AddError(ListResult.ErrorMessage);
		return false;
	}
	TestTrue(
		TEXT("Mode catalog is non-empty"),
		!ListResult.Data->GetArrayField(TEXT("modes")).IsEmpty());
	TestTrue(
		TEXT("Catalog declares whether its target viewport is available"),
		ListResult.Data->HasTypedField<EJson::Boolean>(TEXT("targetAvailable")));
	const TArray<TSharedPtr<FJsonValue>>& CatalogModes =
		ListResult.Data->GetArrayField(TEXT("modes"));
	TestTrue(
		TEXT("Strata remains discoverable when its renderer is disabled"),
		FindCatalogMode(CatalogModes, TEXT("strata")).IsValid());
	TestTrue(
		TEXT("Groom remains discoverable when its renderer is disabled"),
		FindCatalogMode(CatalogModes, TEXT("groom")).IsValid());
	const TSharedPtr<FJsonObject> RayTracingWorldNormal =
		FindCatalogMode(
			CatalogModes,
			TEXT("rayTracingDebug"),
			TEXT("worldNormal"));
	TestNotNull(
		TEXT("Ray tracing debug uses a culture-stable canonical world-normal id"),
		RayTracingWorldNormal.Get());
	TestNotNull(
		TEXT("UE 5.3 renderer-only traversal modes remain discoverable through the compatibility table"),
		FindCatalogMode(
			CatalogModes,
			TEXT("rayTracingDebug"),
			TEXT("traversalNode")).Get());
	if (const TSharedPtr<FJsonObject> StrataPlaceholder =
			FindCatalogMode(CatalogModes, TEXT("strata"), TEXT("unavailable")))
	{
		TestFalse(
			TEXT("Strata placeholder is never advertised as capturable"),
			StrataPlaceholder->GetBoolField(TEXT("available")));
		TestTrue(
			TEXT("Strata placeholder explains why it is unavailable"),
			!StrataPlaceholder->GetStringField(TEXT("unavailableReason")).IsEmpty());
	}
	if (const TSharedPtr<FJsonObject> GroomPlaceholder =
			FindCatalogMode(CatalogModes, TEXT("groom"), TEXT("unavailable")))
	{
		TestFalse(
			TEXT("Groom placeholder is never advertised as capturable"),
			GroomPlaceholder->GetBoolField(TEXT("available")));
		TestTrue(
			TEXT("Groom placeholder explains why it is unavailable"),
			!GroomPlaceholder->GetStringField(TEXT("unavailableReason")).IsEmpty());
	}
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform, false))
	{
		const TSharedPtr<FJsonObject> NaniteMode =
			FindCatalogMode(CatalogModes, TEXT("nanite"));
		TestNotNull(
			TEXT("Nanite remains discoverable on an unsupported platform"),
			NaniteMode.Get());
		if (NaniteMode.IsValid())
		{
			TestFalse(
				TEXT("Unsupported shader platform never advertises Nanite capture"),
				NaniteMode->GetBoolField(TEXT("available")));
			TestTrue(
				TEXT("Unavailable Nanite mode includes a reason"),
				!NaniteMode->GetStringField(TEXT("unavailableReason")).IsEmpty());
		}
	}
	#if UEAI_TEST_HAS_STRATA_VISUALIZATION
	if (ListResult.Data->GetBoolField(TEXT("targetAvailable"))
		&& ListResult.Data->GetBoolField(TEXT("renderingAvailable"))
		&& Strata::IsStrataEnabled())
	{
		TSet<FString> SeenStrataModes;
		for (const auto& Pair : GetStrataVisualizationData().GetModeMap())
		{
			const FStrataVisualizationData::FModeRecord& Record = Pair.Value;
			const FString ModeName = Record.ModeName.ToString();
			if (ModeName.IsEmpty() || SeenStrataModes.Contains(ModeName))
			{
				continue;
			}
			SeenStrataModes.Add(ModeName);
			if (Record.bAvailableCommand)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> CatalogMode =
				FindCatalogMode(CatalogModes, TEXT("strata"), ModeName);
			TestNotNull(
				TEXT("Engine-disabled Strata submode remains discoverable"),
				CatalogMode.Get());
			if (CatalogMode.IsValid())
			{
				TestFalse(
					TEXT("Engine-disabled Strata submode remains unavailable"),
					CatalogMode->GetBoolField(TEXT("available")));
				const FString ExpectedReason =
					Record.UnavailableReason.ToString();
				if (!ExpectedReason.IsEmpty())
				{
					TestEqual(
						TEXT("Strata submode reports the Engine-provided reason"),
						CatalogMode->GetStringField(TEXT("unavailableReason")),
						ExpectedReason);
				}
			}
		}
	}
	#endif

	if (!GDynamicRHI || GUsingNullRHI || !FApp::CanEverRender())
	{
		TestFalse(
			TEXT("NullRHI is explicitly reported as unavailable"),
			ListResult.Data->GetBoolField(TEXT("renderingAvailable")));
		for (const TSharedPtr<FJsonValue>& Value :
			ListResult.Data->GetArrayField(TEXT("modes")))
		{
			const TSharedPtr<FJsonObject> Mode = Value->AsObject();
			TestFalse(
				TEXT("NullRHI never advertises a visualization as capturable"),
				Mode->GetBoolField(TEXT("available")));
			TestTrue(
				TEXT("Unavailable mode includes a reason"),
				!Mode->GetStringField(TEXT("unavailableReason")).IsEmpty());
		}

		TSharedRef<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Visualization = MakeShared<FJsonObject>();
		Visualization->SetStringField(TEXT("family"), TEXT("viewMode"));
		Visualization->SetStringField(TEXT("mode"), TEXT("lit"));
		CaptureParams->SetObjectField(TEXT("visualization"), Visualization);
		TOptional<FMCPToolResult> CaptureResult;
		int32 CompletionCount = 0;
		const bool bAccepted = Registry.BeginExecuteToolAsync(
			TEXT("scene.viewport.visualization.capture"),
			CaptureParams,
			[&CaptureResult, &CompletionCount](FMCPToolResult&& Result)
			{
				++CompletionCount;
				CaptureResult.Emplace(MoveTemp(Result));
			});
		TestTrue(
			TEXT("NullRHI capture is handled by the async capture boundary"),
			bAccepted);
		TestEqual(
			TEXT("NullRHI capture completes exactly once"),
			CompletionCount,
			1);
		TestTrue(
			TEXT("NullRHI capture returns an immediate unavailable result"),
			CaptureResult.IsSet());
		TestFalse(
			TEXT("NullRHI cannot masquerade as a successful screenshot"),
			CaptureResult.IsSet() && CaptureResult->bSuccess);
		if (CaptureResult.IsSet())
		{
			TestTrue(
				TEXT("NullRHI reports a target/render availability error, not an async plumbing error"),
				CaptureResult->ErrorCode != TEXT("async_execution_required")
					&& (CaptureResult->ErrorCode == TEXT("viewport_target_unavailable")
						|| CaptureResult->ErrorCode
							== TEXT("viewport_visualization_unavailable")));
		}
	}
	return true;
}

namespace
{
class FAsyncExecutorFixtureTool final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.test.async");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(
			TEXT("The async fixture must not use the synchronous Execute path."),
			TEXT("unexpected_sync_execution"),
			500);
	}

	bool SupportsAsyncExecution() const override { return true; }

	bool BeginExecuteAsync(
		const TSharedPtr<FJsonObject>& Params,
		FMCPToolAsyncCompletion InCompletion) override
	{
		if (Completion)
		{
			InCompletion(FMCPToolResult::Error(
				TEXT("Fixture is already active."),
				TEXT("fixture_busy"),
				409));
			return true;
		}
		++BeginCount;
		Params->TryGetNumberField(TEXT("value"), Value);
		Completion = MoveTemp(InCompletion);
		return true;
	}

	void CompleteSuccess()
	{
		if (!Completion)
		{
			return;
		}
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("value"), Value);
		FMCPToolAsyncCompletion Local = MoveTemp(Completion);
		Local(FMCPToolResult::Ok(Data));
	}

	void CancelAsyncExecution(const FString& Reason) override
	{
		if (!Completion)
		{
			return;
		}
		FMCPToolAsyncCompletion Local = MoveTemp(Completion);
		Local(FMCPToolResult::Error(
			Reason,
			TEXT("request_cancelled"),
			499));
	}

	int32 BeginCount = 0;

private:
	FMCPToolAsyncCompletion Completion;
	double Value = 0.0;
};

bool WriteAsyncExecutorFixtureManifests(
	const FString& Directory,
	FString& OutError)
{
	static const TCHAR* const Domains[] = {
		TEXT("blueprint"), TEXT("scene"), TEXT("content"),
		TEXT("animation"), TEXT("ai"), TEXT("production")
	};
	IFileManager::Get().MakeDirectory(*Directory, true);
	for (const TCHAR* Domain : Domains)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("ue.capability-manifest.v3"));
		Root->SetNumberField(TEXT("schemaVersion"), 3);
		Root->SetStringField(TEXT("domain"), Domain);
		TArray<TSharedPtr<FJsonValue>> Capabilities;
		if (FString(Domain) == TEXT("scene"))
		{
			TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
			Descriptor->SetStringField(TEXT("id"), TEXT("scene.test.async"));
			Descriptor->SetStringField(TEXT("domain"), TEXT("scene"));
			Descriptor->SetStringField(TEXT("kind"), TEXT("command"));
			Descriptor->SetStringField(
				TEXT("description"),
				TEXT("Automation-only async executor fixture."));
			TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> ValueSchema = MakeShared<FJsonObject>();
			ValueSchema->SetStringField(TEXT("type"), TEXT("number"));
			Properties->SetObjectField(TEXT("value"), ValueSchema);
			Schema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
			Schema->SetArrayField(TEXT("required"), Required);
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			Descriptor->SetObjectField(TEXT("inputSchema"), Schema);
			TSharedRef<FJsonObject> Traits = MakeShared<FJsonObject>();
			Traits->SetBoolField(TEXT("destructive"), false);
			Traits->SetBoolField(TEXT("expensive"), false);
			Descriptor->SetObjectField(TEXT("traits"), Traits);
			TSharedRef<FJsonObject> Effects = MakeShared<FJsonObject>();
			Effects->SetStringField(TEXT("asset"), TEXT("none"));
			Effects->SetStringField(TEXT("world"), TEXT("none"));
			Effects->SetStringField(TEXT("editorSession"), TEXT("write"));
			Effects->SetStringField(TEXT("external"), TEXT("none"));
			Descriptor->SetObjectField(TEXT("effects"), Effects);
			TSharedRef<FJsonObject> Lifecycle = MakeShared<FJsonObject>();
			Lifecycle->SetStringField(TEXT("status"), TEXT("active"));
			Lifecycle->SetStringField(TEXT("since"), TEXT("1.0.0"));
			Lifecycle->SetStringField(
				TEXT("canonicalId"),
				TEXT("scene.test.async"));
			Descriptor->SetObjectField(TEXT("lifecycle"), Lifecycle);
			TSharedRef<FJsonObject> Execution = MakeShared<FJsonObject>();
			Execution->SetArrayField(
				TEXT("backends"),
				{ MakeShared<FJsonValueString>(TEXT("editor")) });
			Execution->SetStringField(TEXT("preferred"), TEXT("editor"));
			Descriptor->SetObjectField(TEXT("execution"), Execution);
			TSharedRef<FJsonObject> Output = MakeShared<FJsonObject>();
			Output->SetStringField(TEXT("kind"), TEXT("json"));
			Descriptor->SetObjectField(TEXT("output"), Output);
			Capabilities.Add(MakeShared<FJsonValueObject>(Descriptor));
		}
		Root->SetArrayField(TEXT("capabilities"), Capabilities);
		Root->SetArrayField(TEXT("tombstones"), {});
		FString Json;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&Json);
		const FString Path =
			FPaths::Combine(Directory, FString(Domain) + TEXT(".json"));
		if (!FJsonSerializer::Serialize(Root, Writer)
			|| !FFileHelper::SaveStringToFile(
				Json,
				*Path,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(
				TEXT("Could not write async executor fixture manifest: %s"),
				*Path);
			return false;
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewportVisualizationAsyncExecutorTest,
	"UE_AI_integration.ViewportVisualization.AsyncExecutorIdempotency",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FViewportVisualizationAsyncExecutorTest::RunTest(
	const FString& Parameters)
{
	const FString FixtureDirectory = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("AsyncExecutorFixture"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString FixtureError;
	if (!WriteAsyncExecutorFixtureManifests(FixtureDirectory, FixtureError))
	{
		AddError(FixtureError);
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().DeleteDirectory(
			*FixtureDirectory,
			false,
			true);
	};

	FMCPToolRegistry Registry;
	const TSharedPtr<FAsyncExecutorFixtureTool> Tool =
		MakeShared<FAsyncExecutorFixtureTool>();
	Registry.BeginDomainRegistration(TEXT("scene"));
	Registry.Register(Tool);
	Registry.EndDomainRegistration();
	TestTrue(
		TEXT("Async executor fixture manifest has exact bindings"),
		Registry.LoadCapabilityManifestsFromDirectory(FixtureDirectory));
	if (!Registry.IsReady())
	{
		for (const FString& Error : Registry.GetValidationErrors())
		{
			AddError(Error);
		}
		return false;
	}

	FMCPExecutor Executor(Registry);
	FMCPExecutionContext Context;
	Context.Capability = TEXT("scene.test.async");
	Context.RequestId = TEXT("async-request-a");
	Context.Params = MakeShared<FJsonObject>();
	Context.Params->SetNumberField(TEXT("value"), 7.0);
	int32 CompletionCount = 0;
	TOptional<FMCPResult> CompletionResult;
	FMCPResult Immediate;
	const bool bDeferred = Executor.BeginExecuteAsync(
		Context,
		[&CompletionCount, &CompletionResult](FMCPResult&& Result)
		{
			++CompletionCount;
			CompletionResult.Emplace(MoveTemp(Result));
		},
		Immediate);
	TestTrue(TEXT("Async executor starts handler-owned request"), bDeferred);
	TestEqual(TEXT("Handler starts once"), Tool->BeginCount, 1);

	FMCPResult DuplicateImmediate;
	const bool bDuplicateDeferred = Executor.BeginExecuteAsync(
		Context,
		[](FMCPResult&& Result) {},
		DuplicateImmediate);
	TestFalse(
		TEXT("In-flight duplicate does not start a second handler"),
		bDuplicateDeferred);
	TestEqual(
		TEXT("In-flight duplicate has a stable response"),
		DuplicateImmediate.Error.Code,
		FString(TEXT("async_request_in_progress")));
	TestEqual(TEXT("Handler remains single-start"), Tool->BeginCount, 1);

	Tool->CompleteSuccess();
	TestEqual(TEXT("Handler completion is delivered exactly once"), CompletionCount, 1);
	TestTrue(
		TEXT("Async completion succeeds"),
		CompletionResult.IsSet() && CompletionResult->bOk);

	FMCPResult CachedImmediate;
	const bool bCachedDeferred = Executor.BeginExecuteAsync(
		Context,
		[](FMCPResult&& Result) {},
		CachedImmediate);
	TestFalse(TEXT("Completed retry is served from cache"), bCachedDeferred);
	TestTrue(TEXT("Completed retry preserves success"), CachedImmediate.bOk);
	TestEqual(TEXT("Cached retry never restarts handler"), Tool->BeginCount, 1);

	FMCPExecutionContext ConflictContext = Context;
	ConflictContext.Params = MakeShared<FJsonObject>();
	ConflictContext.Params->SetNumberField(TEXT("value"), 8.0);
	FMCPResult ConflictImmediate;
	TestFalse(
		TEXT("Same request id with different payload is rejected"),
		Executor.BeginExecuteAsync(
			ConflictContext,
			[](FMCPResult&& Result) {},
			ConflictImmediate));
	TestEqual(
		TEXT("Payload conflict has stable idempotency error"),
		ConflictImmediate.Error.Code,
		FString(TEXT("idempotency_conflict")));

	FMCPExecutionContext CancelContext = Context;
	CancelContext.RequestId = TEXT("async-request-cancel");
	int32 CancelCompletionCount = 0;
	TOptional<FMCPResult> CancelResult;
	FMCPResult CancelImmediate;
	TestTrue(
		TEXT("Second async request starts before cancellation"),
		Executor.BeginExecuteAsync(
			CancelContext,
			[&CancelCompletionCount, &CancelResult](FMCPResult&& Result)
			{
				++CancelCompletionCount;
				CancelResult.Emplace(MoveTemp(Result));
			},
			CancelImmediate));
	const FMCPResult CancelAck = Executor.CancelAsyncOperation(
		CancelContext.RequestId,
		TEXT("Server stopped for Automation."));
	TestTrue(TEXT("Per-request cancellation returns an acknowledgement"),
		CancelAck.bOk && CancelAck.Data.IsValid());
	if (CancelAck.Data.IsValid())
	{
		TestEqual(TEXT("Cancel ACK retains the exact capability after synchronous completion"),
			CancelAck.Data->GetStringField(TEXT("capability")),
			FString(TEXT("scene.test.async")));
		TestTrue(TEXT("Cancel ACK marks cancellation pending"),
			CancelAck.Data->GetBoolField(TEXT("cancelPending")));
	}
	TestEqual(
		TEXT("Cancellation completes the request exactly once"),
		CancelCompletionCount,
		1);
	TestTrue(
		TEXT("Cancellation publishes the canonical cancelled result"),
		CancelResult.IsSet()
			&& !CancelResult->bOk
			&& CancelResult->Error.Code == TEXT("request_cancelled"));
	FMCPResult CancelCached;
	TestFalse(TEXT("Cancelled request retry is served from cache"),
		Executor.BeginExecuteAsync(
			CancelContext,
			[](FMCPResult&& Result) {},
			CancelCached));
	TestEqual(TEXT("Cancelled request retry preserves canonical result"),
		CancelCached.Error.Code,
		FString(TEXT("request_cancelled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewportVisualizationEvidenceTest,
	"UE_AI_integration.ViewportVisualization.CompareAndAnalyzeFixtures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FViewportVisualizationEvidenceTest::RunTest(const FString& Parameters)
{
	FPIESessionController Controller;
	FMCPToolRegistry Registry;
	RegisterVisualizationTools(Registry, Controller);

	constexpr int32 Width = 16;
	constexpr int32 Height = 16;
	TArray<FColor> BeforePixels;
	BeforePixels.Init(FColor(16, 32, 48, 255), Width * Height);
	TArray<FColor> AfterPixels = BeforePixels;
	AfterPixels[7 * Width + 5] = FColor(255, 0, 0, 255);
	TArray<FColor> NormalPixels;
	NormalPixels.Init(FColor(128, 128, 255, 255), Width * Height);

	const FString Suffix =
		FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString BeforeId = TEXT("viewport-test-before-") + Suffix;
	const FString AfterId = TEXT("viewport-test-after-") + Suffix;
	const FString IncompatibleId = TEXT("viewport-test-normal-") + Suffix;
	FString Error;
	if (!WriteVisualizationFixture(
			BeforeId,
			BeforePixels,
			Width,
			Height,
			TEXT("buffer"),
			TEXT("BaseColor"),
			Error)
		|| !WriteVisualizationFixture(
			AfterId,
			AfterPixels,
			Width,
			Height,
			TEXT("buffer"),
			TEXT("BaseColor"),
			Error)
		|| !WriteVisualizationFixture(
			IncompatibleId,
			NormalPixels,
			Width,
			Height,
			TEXT("rayTracingDebug"),
			TEXT("World Normal"),
			Error))
	{
		DeleteVisualizationArtifact(BeforeId);
		DeleteVisualizationArtifact(AfterId);
		DeleteVisualizationArtifact(IncompatibleId);
		AddError(Error);
		return false;
	}

	FString DiffId;
	const FMCPToolResult CompareResult = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.compare"),
		MakeCompareParams(BeforeId, AfterId));
	TestTrue(TEXT("Compatible PNG fixtures compare"), CompareResult.bSuccess);
	if (CompareResult.bSuccess)
	{
		TestTrue(
			TEXT("Equal fingerprints permit pixel metrics"),
			CompareResult.Data->GetBoolField(TEXT("compatible")));
		TestEqual(
			TEXT("One changed pixel is counted"),
			static_cast<int32>(CompareResult.Data->GetNumberField(
				TEXT("changedPixelCount"))),
			1);
		TestTrue(
			TEXT("Changed pixel is localized"),
			CompareResult.Data->HasTypedField<EJson::Object>(
				TEXT("changedBounds")));
		TestTrue(
			TEXT("Compare produces a lossless PNG artifact"),
			!CompareResult.Data->GetStringField(TEXT("image_base64")).IsEmpty());
		DiffId = CompareResult.Data->GetStringField(TEXT("captureId"));
	}

	TSharedRef<FJsonObject> AnalyzeParams = MakeShared<FJsonObject>();
	AnalyzeParams->SetStringField(TEXT("captureId"), AfterId);
	const FMCPToolResult AnalyzeResult = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.analyze"),
		AnalyzeParams);
	TestTrue(TEXT("Bounded image analysis succeeds"), AnalyzeResult.bSuccess);
	if (AnalyzeResult.bSuccess)
	{
		TestEqual(
			TEXT("Luminance histogram is bounded to sixteen bins"),
			AnalyzeResult.Data->GetArrayField(TEXT("luminanceHistogram")).Num(),
			16);
		TestEqual(
			TEXT("Spatial analysis is bounded to a four-by-four grid"),
			AnalyzeResult.Data->GetArrayField(TEXT("regions")).Num(),
			16);
		TestTrue(
			TEXT("Analysis declares its 8-bit evidence boundary"),
			AnalyzeResult.Data->GetStringField(TEXT("evidenceBoundary"))
				.Contains(TEXT("8-bit")));
	}

	const FMCPToolResult IncompatibleCompare = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.compare"),
		MakeCompareParams(BeforeId, IncompatibleId));
	TestTrue(
		TEXT("Fingerprint incompatibility returns bounded evidence"),
		IncompatibleCompare.bSuccess);
	if (IncompatibleCompare.bSuccess)
	{
		TestFalse(
			TEXT("Different debug modes are incompatible"),
			IncompatibleCompare.Data->GetBoolField(TEXT("compatible")));
		TestEqual(
			TEXT("Incompatible comparison is inconclusive"),
			IncompatibleCompare.Data->GetStringField(TEXT("comparison")),
			FString(TEXT("inconclusive")));
		double InvalidRatio = 0.0;
		TestFalse(
			TEXT("Inconclusive comparison omits false pixel metrics"),
			IncompatibleCompare.Data->TryGetNumberField(
				TEXT("changedPixelRatio"),
				InvalidRatio));
	}

	TSharedRef<FJsonObject> NormalAnalyzeParams = MakeShared<FJsonObject>();
	NormalAnalyzeParams->SetStringField(TEXT("captureId"), IncompatibleId);
	const FMCPToolResult NormalAnalyze = Registry.ExecuteTool(
		TEXT("scene.viewport.visualization.analyze"),
		NormalAnalyzeParams);
	TestTrue(TEXT("Normal debug image analysis succeeds"), NormalAnalyze.bSuccess);
	if (NormalAnalyze.bSuccess)
	{
		TestEqual(
			TEXT("Normal modes use explicit encoded-normal semantics"),
			NormalAnalyze.Data->GetObjectField(TEXT("semantic"))
				->GetStringField(TEXT("kind")),
			FString(TEXT("encodedNormal")));
	}

	DeleteVisualizationArtifact(BeforeId);
	DeleteVisualizationArtifact(AfterId);
	DeleteVisualizationArtifact(IncompatibleId);
	DeleteVisualizationArtifact(DiffId);
	return true;
}

namespace
{
enum class ERenderedCaptureStage : uint8
{
	Success,
	RayTracingDebug,
	AfterApplyFailure,
	CompoundRestoreFailure,
	SettlementTimeout,
	ReadPixelsFailure,
	Cancellation,
	Finished
};

void CaptureLevelViewportSelection(
	TArray<TWeakObjectPtr<AActor>>& OutActors,
	TArray<TWeakObjectPtr<UActorComponent>>& OutComponents)
{
	OutActors.Reset();
	OutComponents.Reset();
	if (!GEditor)
	{
		return;
	}
	if (USelection* ActorSelection = GEditor->GetSelectedActors())
	{
		for (FSelectionIterator It(*ActorSelection); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				OutActors.Add(Actor);
			}
		}
	}
	if (USelection* ComponentSelection = GEditor->GetSelectedComponents())
	{
		for (FSelectionIterator It(*ComponentSelection); It; ++It)
		{
			if (UActorComponent* Component = Cast<UActorComponent>(*It))
			{
				OutComponents.Add(Component);
			}
		}
	}
}

template <typename TObject>
bool WeakSelectionSetMatches(
	const TArray<TWeakObjectPtr<TObject>>& Current,
	const TArray<TWeakObjectPtr<TObject>>& Expected)
{
	if (Current.Num() != Expected.Num())
	{
		return false;
	}
	for (const TWeakObjectPtr<TObject>& Object : Expected)
	{
		if (!Current.Contains(Object))
		{
			return false;
		}
	}
	return true;
}

bool LevelViewportSelectionMatches(
	const TArray<TWeakObjectPtr<AActor>>& Actors,
	const TArray<TWeakObjectPtr<UActorComponent>>& Components)
{
	TArray<TWeakObjectPtr<AActor>> CurrentActors;
	TArray<TWeakObjectPtr<UActorComponent>> CurrentComponents;
	CaptureLevelViewportSelection(CurrentActors, CurrentComponents);
	return WeakSelectionSetMatches(CurrentActors, Actors)
		&& WeakSelectionSetMatches(CurrentComponents, Components);
}

FString LevelViewportSelectionHash(
	const TArray<TWeakObjectPtr<AActor>>& Actors,
	const TArray<TWeakObjectPtr<UActorComponent>>& Components)
{
	TArray<FString> ActorPaths;
	TArray<FString> ComponentPaths;
	for (const TWeakObjectPtr<AActor>& Actor : Actors)
	{
		if (Actor.IsValid())
		{
			ActorPaths.Add(Actor->GetPathName());
		}
	}
	for (const TWeakObjectPtr<UActorComponent>& Component : Components)
	{
		if (Component.IsValid())
		{
			ComponentPaths.Add(Component->GetPathName());
		}
	}
	ActorPaths.Sort();
	ComponentPaths.Sort();
	TArray<FString> TypedPaths;
	for (const FString& Path : ActorPaths)
	{
		TypedPaths.Add(TEXT("actor:") + Path);
	}
	for (const FString& Path : ComponentPaths)
	{
		TypedPaths.Add(TEXT("component:") + Path);
	}
	const FString Canonical = FString::Join(TypedPaths, TEXT("\n"));
	const FTCHARToUTF8 Utf8(*Canonical);
	FString Hash;
	UEAIIntegration::Infrastructure::TrySha256Hex(
		Utf8.Get(),
		static_cast<uint64>(Utf8.Length()),
		Hash);
	return Hash;
}

void RestoreLevelViewportSelection(
	const TArray<TWeakObjectPtr<AActor>>& Actors,
	const TArray<TWeakObjectPtr<UActorComponent>>& Components)
{
	if (!GEditor || LevelViewportSelectionMatches(Actors, Components))
	{
		return;
	}
	USelection* ActorSelection = GEditor->GetSelectedActors();
	USelection* ComponentSelection = GEditor->GetSelectedComponents();
	if (!ActorSelection || !ComponentSelection)
	{
		return;
	}
	ActorSelection->DeselectAll(AActor::StaticClass());
	ComponentSelection->DeselectAll(UActorComponent::StaticClass());
	for (const TWeakObjectPtr<AActor>& WeakActor : Actors)
	{
		if (AActor* Actor = WeakActor.Get())
		{
			GEditor->SelectActor(Actor, true, false, true);
		}
	}
	for (const TWeakObjectPtr<UActorComponent>& WeakComponent : Components)
	{
		if (UActorComponent* Component = WeakComponent.Get())
		{
			GEditor->SelectComponent(Component, true, false, true);
		}
	}
	GEditor->NoteSelectionChange();
}

struct FRenderedCaptureState
{
	TUniquePtr<FPIESessionController> Controller;
	TUniquePtr<FMCPToolRegistry> Registry;
	FLevelEditorViewportClient* Client = nullptr;
	ERenderedCaptureStage Stage = ERenderedCaptureStage::Success;
	TOptional<FMCPToolResult> Result;
	FString CaptureId;
	FString VisualizationFamily = TEXT("viewMode");
	FString VisualizationMode = TEXT("unlit");
	int32 StartedCount = 0;
	int32 CompletionCount = 0;
	double DeadlineSeconds = 0.0;
	EViewModeIndex OriginalViewMode = VMI_Unknown;
	FEngineShowFlags OriginalShowFlags = FEngineShowFlags(ESFIM_Editor);
	FExposureSettings OriginalExposure;
	FVector OriginalLocation = FVector::ZeroVector;
	FRotator OriginalRotation = FRotator::ZeroRotator;
	ELevelViewportType OriginalViewportType = LVT_Perspective;
	float OriginalOrthoZoom = 1.0f;
	float OriginalFov = 90.0f;
	TMap<int32, FName> OriginalViewModeParamNameMap;
	int32 OriginalViewModeParam = INDEX_NONE;
	EViewModeIndex BaselineViewMode = VMI_Unknown;
	FString BaselineShowFlags;
	FString BaselineExposure;
	FVector BaselineLocation = FVector::ZeroVector;
	FRotator BaselineRotation = FRotator::ZeroRotator;
	ELevelViewportType BaselineViewportType = LVT_Perspective;
	float BaselineOrthoZoom = 1.0f;
	float BaselineFov = 90.0f;
	TMap<int32, FName> BaselineViewModeParamNameMap;
	TArray<TWeakObjectPtr<AActor>> OriginalSelectedActors;
	TArray<TWeakObjectPtr<UActorComponent>> OriginalSelectedComponents;
	TArray<TWeakObjectPtr<AActor>> BaselineSelectedActors;
	TArray<TWeakObjectPtr<UActorComponent>> BaselineSelectedComponents;
	FString BaselineSelectionSha256;
	TWeakObjectPtr<AActor> SelectionFixtureActor;
	TWeakObjectPtr<UActorComponent> SelectionFixtureComponent;
	int32 TestViewModeParam = INDEX_NONE;
	FName TestViewModeParamName;
	FIntPoint SourceSize = FIntPoint::ZeroValue;
};

using FRenderedCaptureStateRef =
	TSharedRef<FRenderedCaptureState, ESPMode::ThreadSafe>;

bool IsRenderedClientLive(const FRenderedCaptureState& State)
{
	if (!GEditor || !State.Client || !State.Client->Viewport)
	{
		return false;
	}
	for (const FLevelEditorViewportClient* Candidate :
		GEditor->GetLevelViewportClients())
	{
		if (Candidate == State.Client)
		{
			return true;
		}
	}
	return false;
}

bool ConfigureRenderedSelectionFixture(
	FRenderedCaptureState& State,
	FAutomationTestBase& Test)
{
	CaptureLevelViewportSelection(
		State.OriginalSelectedActors,
		State.OriginalSelectedComponents);
	UWorld* World = State.Client ? State.Client->GetWorld() : nullptr;
	if (!World)
	{
		Test.AddError(TEXT("The rendered Level Editor viewport has no world."));
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(
		World,
		AActor::StaticClass(),
		TEXT("UEAIViewportSelectionFixture"));
	SpawnParameters.ObjectFlags = RF_Transient | RF_Transactional;
	AActor* Actor = World->SpawnActor<AActor>(
		AActor::StaticClass(),
		FTransform::Identity,
		SpawnParameters);
	if (!Actor)
	{
		Test.AddError(TEXT("Could not spawn the viewport selection fixture Actor."));
		return false;
	}
	USceneComponent* Component = NewObject<USceneComponent>(
		Actor,
		TEXT("UEAIViewportSelectionFixtureComponent"),
		RF_Transient | RF_Transactional);
	if (!Component)
	{
		World->DestroyActor(Actor, false, false);
		Test.AddError(TEXT("Could not create the viewport selection fixture Component."));
		return false;
	}
	Actor->SetRootComponent(Component);
	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	State.SelectionFixtureActor = Actor;
	State.SelectionFixtureComponent = Component;

	TArray<TWeakObjectPtr<AActor>> FixtureActors;
	FixtureActors.Add(Actor);
	TArray<TWeakObjectPtr<UActorComponent>> FixtureComponents;
	FixtureComponents.Add(Component);
	RestoreLevelViewportSelection(FixtureActors, FixtureComponents);
	CaptureLevelViewportSelection(
		State.BaselineSelectedActors,
		State.BaselineSelectedComponents);
	State.BaselineSelectionSha256 = LevelViewportSelectionHash(
		State.BaselineSelectedActors,
		State.BaselineSelectedComponents);
	const bool bFixtureSelected =
		LevelViewportSelectionMatches(FixtureActors, FixtureComponents);
	Test.TestTrue(
		TEXT("Rendered fixture selects one Actor and one ActorComponent"),
		bFixtureSelected);
	return bFixtureSelected;
}

void RestoreOriginalRenderedState(FRenderedCaptureState& State)
{
	RestoreLevelViewportSelection(
		State.OriginalSelectedActors,
		State.OriginalSelectedComponents);
	if (AActor* FixtureActor = State.SelectionFixtureActor.Get())
	{
		if (UWorld* World = FixtureActor->GetWorld())
		{
			World->DestroyActor(FixtureActor, false, false);
		}
	}
	State.SelectionFixtureActor.Reset();
	State.SelectionFixtureComponent.Reset();
	if (!IsRenderedClientLive(State))
	{
		return;
	}
	FLevelEditorViewportClient& Client = *State.Client;
	Client.SetViewportType(State.OriginalViewportType);
	Client.SetViewMode(State.OriginalViewMode);
	Client.EngineShowFlags = State.OriginalShowFlags;
	Client.ExposureSettings = State.OriginalExposure;
	Client.SetViewLocation(State.OriginalLocation);
	Client.SetViewRotation(State.OriginalRotation);
	Client.SetOrthoZoom(State.OriginalOrthoZoom);
	Client.ViewFOV = State.OriginalFov;
	Client.GetViewModeParamNameMap() = State.OriginalViewModeParamNameMap;
	if (State.OriginalViewModeParam != INDEX_NONE)
	{
		Client.SetViewModeParam(State.OriginalViewModeParam);
	}
	Client.UpdateHiddenCollisionDrawing();
	Client.Invalidate();
}

bool VerifyRenderedBaseline(
	const FRenderedCaptureState& State,
	FAutomationTestBase& Test,
	const FString& Context)
{
	if (!IsRenderedClientLive(State))
	{
		Test.AddError(Context + TEXT(": the original Level Editor target is no longer live."));
		return false;
	}
	FLevelEditorViewportClient& Client = *State.Client;
	bool bMatches = true;
	bMatches &= Test.TestEqual(
		Context + TEXT(": view mode restored"),
		static_cast<int32>(Client.GetViewMode()),
		static_cast<int32>(State.BaselineViewMode));
	bMatches &= Test.TestEqual(
		Context + TEXT(": show flags restored"),
		Client.EngineShowFlags.ToString(),
		State.BaselineShowFlags);
	bMatches &= Test.TestEqual(
		Context + TEXT(": exposure restored"),
		Client.ExposureSettings.ToString(),
		State.BaselineExposure);
	bMatches &= Test.TestTrue(
		Context + TEXT(": camera location restored"),
		Client.GetViewLocation().Equals(State.BaselineLocation));
	bMatches &= Test.TestTrue(
		Context + TEXT(": camera rotation restored"),
		Client.GetViewRotation().Equals(State.BaselineRotation));
	bMatches &= Test.TestEqual(
		Context + TEXT(": viewport type restored"),
		static_cast<int32>(Client.GetViewportType()),
		static_cast<int32>(State.BaselineViewportType));
	bMatches &= Test.TestTrue(
		Context + TEXT(": orthographic zoom restored"),
		FMath::IsNearlyEqual(Client.GetOrthoZoom(), State.BaselineOrthoZoom));
	bMatches &= Test.TestTrue(
		Context + TEXT(": field of view restored"),
		FMath::IsNearlyEqual(Client.ViewFOV, State.BaselineFov));
	bMatches &= Test.TestEqual(
		Context + TEXT(": view-mode parameter map restored"),
		Client.GetViewModeParamNameMap().Num(),
		State.BaselineViewModeParamNameMap.Num());
	for (const TPair<int32, FName>& Pair :
		State.BaselineViewModeParamNameMap)
	{
		const FName* Current =
			Client.GetViewModeParamNameMap().Find(Pair.Key);
		bMatches &= Test.TestTrue(
			Context + TEXT(": view-mode parameter entry restored"),
			Current && *Current == Pair.Value);
	}
	bMatches &= Test.TestTrue(
		Context + TEXT(": active view-mode parameter restored"),
		Client.IsViewModeParam(State.TestViewModeParam));
	bMatches &= Test.TestTrue(
		Context + TEXT(": Actor and ActorComponent selections restored"),
		LevelViewportSelectionMatches(
			State.BaselineSelectedActors,
			State.BaselineSelectedComponents));
	return bMatches;
}

TSharedRef<FJsonObject> MakeRenderedCaptureParams(
	const FRenderedCaptureState& State)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("kind"), TEXT("editor"));
	Params->SetObjectField(TEXT("target"), Target);
	TSharedRef<FJsonObject> Visualization = MakeShared<FJsonObject>();
	Visualization->SetStringField(TEXT("family"), State.VisualizationFamily);
	Visualization->SetStringField(TEXT("mode"), State.VisualizationMode);
	Params->SetObjectField(TEXT("visualization"), Visualization);
	Params->SetNumberField(TEXT("width"), 128);
	Params->SetNumberField(TEXT("height"), 128);
	switch (State.Stage)
	{
	case ERenderedCaptureStage::AfterApplyFailure:
		Params->SetStringField(TEXT("_testFault"), TEXT("afterApply"));
		break;
	case ERenderedCaptureStage::CompoundRestoreFailure:
		Params->SetStringField(
			TEXT("_testFault"),
			TEXT("afterApplyRestoreFailure"));
		break;
	case ERenderedCaptureStage::SettlementTimeout:
		Params->SetStringField(TEXT("_testFault"), TEXT("settleTimeout"));
		break;
	case ERenderedCaptureStage::ReadPixelsFailure:
		Params->SetStringField(TEXT("_testFault"), TEXT("readPixels"));
		break;
	default:
		break;
	}
	return Params;
}

bool BeginRenderedCapture(
	const FRenderedCaptureStateRef& State,
	FAutomationTestBase& Test)
{
	State->Result.Reset();
	++State->StartedCount;
	const bool bAccepted = State->Registry->BeginExecuteToolAsync(
		TEXT("scene.viewport.visualization.capture"),
		MakeRenderedCaptureParams(*State),
		[State](FMCPToolResult&& Result)
		{
			++State->CompletionCount;
			if (!State->Result.IsSet())
			{
				State->Result.Emplace(MoveTemp(Result));
			}
		});
	Test.TestTrue(TEXT("Rendered capture async handler accepts request"), bAccepted);
	State->DeadlineSeconds = FPlatformTime::Seconds() + 15.0;
	if (State->Stage == ERenderedCaptureStage::Cancellation && bAccepted)
	{
		State->Registry->CancelAsyncTools(
			TEXT("Injected server-stop cancellation."));
	}
	return bAccepted;
}

void VerifyFailureRestored(
	const FRenderedCaptureStateRef& State,
	FAutomationTestBase& Test,
	const FString& ExpectedCode,
	const FString& Context)
{
	Test.TestTrue(Context + TEXT(": completion returned"), State->Result.IsSet());
	if (!State->Result.IsSet())
	{
		return;
	}
	Test.TestFalse(Context + TEXT(": operation failed"), State->Result->bSuccess);
	Test.TestEqual(
		Context + TEXT(": stable error code"),
		State->Result->ErrorCode,
		ExpectedCode);
	if (State->Result->Data.IsValid())
	{
		bool bStateRestored = false;
		if (State->Result->Data->TryGetBoolField(
				TEXT("stateRestored"),
				bStateRestored))
		{
			Test.TestTrue(
				Context + TEXT(": handler verifies state restoration"),
				bStateRestored);
		}
	}
	VerifyRenderedBaseline(*State, Test, Context);
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FWaitForRenderedVisualizationCapture,
	FRenderedCaptureStateRef,
	State,
	FAutomationTestBase*,
	Test);

bool FWaitForRenderedVisualizationCapture::Update()
{
	if (!State->Result.IsSet())
	{
		if (FPlatformTime::Seconds() < State->DeadlineSeconds)
		{
			return false;
		}
		State->Registry->CancelAsyncTools(
			TEXT("Rendered visualization Automation timeout."));
		Test->AddError(TEXT("Timed out waiting for async viewport capture."));
		RestoreOriginalRenderedState(*State);
		return true;
	}
	Test->TestEqual(
		TEXT("Every async capture attempt completes exactly once"),
		State->CompletionCount,
		State->StartedCount);

	switch (State->Stage)
	{
	case ERenderedCaptureStage::Success:
	{
		const FMCPToolResult& Result = State->Result.GetValue();
		Test->TestTrue(TEXT("Rendered viewport capture succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			State->CaptureId = Result.Data->GetStringField(TEXT("captureId"));
			Test->TestTrue(
				TEXT("Capture returns a bounded lossless PNG"),
				Result.Data->GetStringField(TEXT("mimeType")) == TEXT("image/png")
					&& !Result.Data->GetStringField(TEXT("image_base64")).IsEmpty());
			Test->TestTrue(
				TEXT("Success verifies state restoration"),
				Result.Data->GetBoolField(TEXT("stateRestored")));
			const int32 EngineFrames = static_cast<int32>(
				Result.Data->GetNumberField(TEXT("settledEngineFrameCount")));
			const int32 SlateFrames = static_cast<int32>(
				Result.Data->GetNumberField(TEXT("slateTickFrameCount")));
			const double StartFrame =
				Result.Data->GetNumberField(TEXT("startFrameCounter"));
			const double LastFrame =
				Result.Data->GetNumberField(TEXT("lastSettledFrameCounter"));
			const double ReadbackFrame =
				Result.Data->GetNumberField(TEXT("readbackFrameCounter"));
			Test->TestTrue(
				TEXT("Capture spans at least two real Engine frames"),
				EngineFrames >= 2 && LastFrame >= StartFrame + 2.0);
			Test->TestTrue(
				TEXT("Capture observes at least two Slate post-tick frames"),
				SlateFrames >= 2);
			Test->TestTrue(
				TEXT("Final readback occurs after the settled frame range"),
				ReadbackFrame >= LastFrame);
			Test->TestTrue(
				TEXT("Final render fence completed before readback"),
				Result.Data->GetBoolField(TEXT("renderFenceCompleted")));
			Test->TestTrue(
				TEXT("Target identity is revalidated on Slate and Engine ticks"),
				Result.Data->GetNumberField(TEXT("targetRevalidationCount")) >= 4.0);
			Test->TestEqual(
				TEXT("Capture source width is the exact target viewport width"),
				static_cast<int32>(Result.Data->GetNumberField(TEXT("sourceWidth"))),
				State->SourceSize.X);
			Test->TestEqual(
				TEXT("Capture source height is the exact target viewport height"),
				static_cast<int32>(Result.Data->GetNumberField(TEXT("sourceHeight"))),
				State->SourceSize.Y);
			const TSharedPtr<FJsonObject> Fingerprint =
				Result.Data->GetObjectField(TEXT("renderFingerprint"));
			static const TCHAR* const RequiredFingerprintFields[] = {
				TEXT("worldPath"), TEXT("worldPackage"), TEXT("map"),
				TEXT("sourceWidth"), TEXT("sourceHeight"),
				TEXT("sourceAspectRatio"), TEXT("outputAspectRatio"),
				TEXT("displayGamma"), TEXT("screenPercentage"),
				TEXT("dpiScale"), TEXT("sessionId"), TEXT("generation"),
				TEXT("selectionCount"), TEXT("selectionActorCount"),
				TEXT("selectionComponentCount"), TEXT("selectionSha256")
			};
			for (const TCHAR* Field : RequiredFingerprintFields)
			{
				Test->TestTrue(
					FString::Printf(TEXT("Fingerprint includes %s"), Field),
					Fingerprint.IsValid() && Fingerprint->HasField(Field));
			}
			Test->TestTrue(
				TEXT("Fingerprint includes key CVars"),
				Fingerprint.IsValid()
					&& Fingerprint->HasTypedField<EJson::Object>(
						TEXT("criticalCVars")));
			Test->TestTrue(
				TEXT("Fingerprint includes target camera transform"),
				Fingerprint.IsValid()
					&& Fingerprint->HasTypedField<EJson::Object>(TEXT("camera")));
			if (Fingerprint.IsValid())
			{
				Test->TestEqual(
					TEXT("Fingerprint counts the selected Actor"),
					static_cast<int32>(Fingerprint->GetNumberField(
						TEXT("selectionActorCount"))),
					1);
				Test->TestEqual(
					TEXT("Fingerprint counts the selected ActorComponent"),
					static_cast<int32>(Fingerprint->GetNumberField(
						TEXT("selectionComponentCount"))),
					1);
				Test->TestEqual(
					TEXT("Fingerprint counts both Level Viewport selections"),
					static_cast<int32>(Fingerprint->GetNumberField(
						TEXT("selectionCount"))),
					2);
				Test->TestFalse(
					TEXT("Fingerprint hashes Actor and Component paths"),
					Fingerprint->GetStringField(TEXT("selectionSha256")).IsEmpty());
				Test->TestEqual(
					TEXT("Fingerprint hash includes typed Actor and Component paths"),
					Fingerprint->GetStringField(TEXT("selectionSha256")),
					State->BaselineSelectionSha256);
			}
		}
		else if (!Result.bSuccess)
		{
			Test->AddError(FString::Printf(
				TEXT("Viewport capture failed [%s]: %s"),
				*Result.ErrorCode,
				*Result.ErrorMessage));
		}
		VerifyRenderedBaseline(*State, *Test, TEXT("successful capture"));

		const FMCPToolResult Catalog = State->Registry->ExecuteTool(
			TEXT("scene.viewport.visualization.list"),
			MakeShared<FJsonObject>());
		Test->TestTrue(
			TEXT("Rendered target exposes the live visualization catalog"),
			Catalog.bSuccess);
		TSharedPtr<FJsonObject> AvailableRayTracingMode;
		if (Catalog.bSuccess && Catalog.Data.IsValid())
		{
			for (const TSharedPtr<FJsonValue>& Value :
				Catalog.Data->GetArrayField(TEXT("modes")))
			{
				const TSharedPtr<FJsonObject> Mode = Value->AsObject();
				if (!Mode.IsValid()
					|| Mode->GetStringField(TEXT("family"))
						!= TEXT("rayTracingDebug"))
				{
					continue;
				}
				if (Mode->GetBoolField(TEXT("available")))
				{
					AvailableRayTracingMode = Mode;
					break;
				}
				Test->TestFalse(
					TEXT("Unavailable Ray Tracing Debug modes include an explicit reason"),
					Mode->GetStringField(TEXT("unavailableReason")).IsEmpty());
			}
		}
		if (AvailableRayTracingMode.IsValid())
		{
			DeleteVisualizationArtifact(State->CaptureId);
			State->CaptureId.Reset();
			State->VisualizationFamily = TEXT("rayTracingDebug");
			State->VisualizationMode =
				AvailableRayTracingMode->GetStringField(TEXT("mode"));
			State->Stage = ERenderedCaptureStage::RayTracingDebug;
		}
		else
		{
			Test->AddInfo(
				TEXT("Ray Tracing Debug capture is unavailable for this rendered project/RHI; the catalog supplied explicit unavailable reasons."));
			State->Stage = ERenderedCaptureStage::AfterApplyFailure;
		}
		break;
	}
	case ERenderedCaptureStage::RayTracingDebug:
	{
		const FMCPToolResult& Result = State->Result.GetValue();
		Test->TestTrue(
			TEXT("An advertised Ray Tracing Debug mode captures successfully"),
			Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			State->CaptureId = Result.Data->GetStringField(TEXT("captureId"));
			Test->TestEqual(
				TEXT("Ray Tracing Debug capture preserves the requested family"),
				Result.Data->GetStringField(TEXT("family")),
				FString(TEXT("rayTracingDebug")));
			Test->TestEqual(
				TEXT("Ray Tracing Debug capture preserves the requested mode"),
				Result.Data->GetStringField(TEXT("mode")),
				State->VisualizationMode);
			Test->TestTrue(
				TEXT("Ray Tracing Debug capture is a lossless PNG"),
				Result.Data->GetStringField(TEXT("mimeType")) == TEXT("image/png")
					&& !Result.Data->GetStringField(TEXT("image_base64")).IsEmpty());
			Test->TestTrue(
				TEXT("Ray Tracing Debug capture verifies state restoration"),
				Result.Data->GetBoolField(TEXT("stateRestored")));
		}
		else if (!Result.bSuccess)
		{
			Test->AddError(FString::Printf(
				TEXT("Advertised Ray Tracing Debug capture failed [%s]: %s"),
				*Result.ErrorCode,
				*Result.ErrorMessage));
		}
		VerifyRenderedBaseline(
			*State,
			*Test,
			TEXT("Ray Tracing Debug capture"));
		State->VisualizationFamily = TEXT("viewMode");
		State->VisualizationMode = TEXT("unlit");
		State->Stage = ERenderedCaptureStage::AfterApplyFailure;
		break;
	}
	case ERenderedCaptureStage::AfterApplyFailure:
		VerifyFailureRestored(
			State,
			*Test,
			TEXT("viewport_capture_test_fault"),
			TEXT("post-apply fault"));
		State->Stage = ERenderedCaptureStage::CompoundRestoreFailure;
		break;
	case ERenderedCaptureStage::CompoundRestoreFailure:
		Test->TestFalse(
			TEXT("Compound restoration fault fails"),
			State->Result->bSuccess);
		Test->TestEqual(
			TEXT("Compound restoration fault has the stable outer error"),
			State->Result->ErrorCode,
			FString(TEXT("viewport_state_restore_failed")));
		if (State->Result->Data.IsValid())
		{
			Test->TestEqual(
				TEXT("Compound error preserves the original failure code"),
				State->Result->Data->GetStringField(TEXT("originalErrorCode")),
				FString(TEXT("viewport_capture_test_fault")));
			Test->TestFalse(
				TEXT("Compound error declares failed restoration verification"),
				State->Result->Data->GetBoolField(TEXT("stateRestored")));
		}
		VerifyRenderedBaseline(
			*State,
			*Test,
			TEXT("compound restoration fault actual cleanup"));
		State->Stage = ERenderedCaptureStage::SettlementTimeout;
		break;
	case ERenderedCaptureStage::SettlementTimeout:
		VerifyFailureRestored(
			State,
			*Test,
			TEXT("viewport_capture_timeout"),
			TEXT("settlement timeout"));
		State->Stage = ERenderedCaptureStage::ReadPixelsFailure;
		break;
	case ERenderedCaptureStage::ReadPixelsFailure:
		VerifyFailureRestored(
			State,
			*Test,
			TEXT("viewport_capture_unavailable"),
			TEXT("pixel read fault"));
		State->Stage = ERenderedCaptureStage::Cancellation;
		break;
	case ERenderedCaptureStage::Cancellation:
		VerifyFailureRestored(
			State,
			*Test,
			TEXT("request_cancelled"),
			TEXT("server-stop cancellation"));
		State->Stage = ERenderedCaptureStage::Finished;
		break;
	default:
		break;
	}

	if (State->Stage == ERenderedCaptureStage::Finished)
	{
		DeleteVisualizationArtifact(State->CaptureId);
		RestoreOriginalRenderedState(*State);
		State->Registry.Reset();
		State->Controller.Reset();
		return true;
	}
	BeginRenderedCapture(State, *Test);
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewportVisualizationRenderedCaptureTest,
	"UE_AI_integration.ViewportVisualization.RenderedCaptureRestoresState",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter
		| EAutomationTestFlags::NonNullRHI)

bool FViewportVisualizationRenderedCaptureTest::RunTest(
	const FString& Parameters)
{
	if (!FApp::CanEverRender() || !GDynamicRHI || GUsingNullRHI)
	{
		AddError(
			TEXT("This NonNullRHI release test requires a real rendered RHI."));
		return false;
	}
	FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
	if (!Client || !Client->Viewport)
	{
		AddError(
			TEXT("A rendered Level Editor viewport is required; the release test may not silently skip."));
		return false;
	}

	const FRenderedCaptureStateRef State =
		MakeShared<FRenderedCaptureState, ESPMode::ThreadSafe>();
	State->Controller = MakeUnique<FPIESessionController>();
	State->Registry = MakeUnique<FMCPToolRegistry>();
	RegisterVisualizationTools(*State->Registry, *State->Controller);
	State->Client = Client;
	State->SourceSize = Client->Viewport->GetSizeXY();
	State->OriginalViewMode = Client->GetViewMode();
	State->OriginalShowFlags = Client->EngineShowFlags;
	State->OriginalExposure = Client->ExposureSettings;
	State->OriginalLocation = Client->GetViewLocation();
	State->OriginalRotation = Client->GetViewRotation();
	State->OriginalViewportType = Client->GetViewportType();
	State->OriginalOrthoZoom = Client->GetOrthoZoom();
	State->OriginalFov = Client->ViewFOV;
	State->OriginalViewModeParamNameMap = Client->GetViewModeParamNameMap();
	for (const TPair<int32, FName>& Pair :
		State->OriginalViewModeParamNameMap)
	{
		if (Client->IsViewModeParam(Pair.Key))
		{
			State->OriginalViewModeParam = Pair.Key;
			break;
		}
	}
	if (!ConfigureRenderedSelectionFixture(*State, *this))
	{
		RestoreOriginalRenderedState(*State);
		return false;
	}
	State->TestViewModeParam = MAX_int32;
	while (Client->GetViewModeParamNameMap().Contains(
		State->TestViewModeParam))
	{
		--State->TestViewModeParam;
	}
	State->TestViewModeParamName = FName(*FString::Printf(
		TEXT("UEAIViewportTest_%d"),
		State->TestViewModeParam));
	Client->GetViewModeParamNameMap().Add(
		State->TestViewModeParam,
		State->TestViewModeParamName);
	Client->SetViewModeParam(State->TestViewModeParam);
	State->BaselineViewMode = Client->GetViewMode();
	State->BaselineShowFlags = Client->EngineShowFlags.ToString();
	State->BaselineExposure = Client->ExposureSettings.ToString();
	State->BaselineLocation = Client->GetViewLocation();
	State->BaselineRotation = Client->GetViewRotation();
	State->BaselineViewportType = Client->GetViewportType();
	State->BaselineOrthoZoom = Client->GetOrthoZoom();
	State->BaselineFov = Client->ViewFOV;
	State->BaselineViewModeParamNameMap = Client->GetViewModeParamNameMap();

	if (!BeginRenderedCapture(State, *this))
	{
		RestoreOriginalRenderedState(*State);
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		FWaitForRenderedVisualizationCapture(State, this));
	return true;
}

namespace
{
enum class EPIEVisualizationStage : uint8
{
	WaitingForPIE,
	WaitingForCapture,
	WaitingForShutdown
};

struct FPIEVisualizationState
{
	TUniquePtr<FPIESessionController> Controller;
	TUniquePtr<FMCPToolRegistry> Registry;
	EPIEVisualizationStage Stage = EPIEVisualizationStage::WaitingForPIE;
	TOptional<FMCPToolResult> CaptureResult;
	FString CaptureId;
	FString SessionId;
	uint64 Generation = 0;
	int32 CompletionCount = 0;
	double DeadlineSeconds = 0.0;
};

using FPIEVisualizationStateRef =
	TSharedRef<FPIEVisualizationState, ESPMode::ThreadSafe>;

void StopPIEVisualizationTest(FPIEVisualizationState& State)
{
	if (State.Registry.IsValid())
	{
		State.Registry->CancelAsyncTools(
			TEXT("PIE visualization test is stopping."));
	}
	if (State.Controller.IsValid())
	{
		State.Controller->Stop();
	}
	else if (GEditor && GEditor->IsPlayingSessionInEditor())
	{
		GEditor->RequestEndPlayMap();
	}
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FWaitForPIEVisualizationCapture,
	FPIEVisualizationStateRef,
	State,
	FAutomationTestBase*,
	Test);

bool FWaitForPIEVisualizationCapture::Update()
{
	State->Controller->Tick();
	const double NowSeconds = FPlatformTime::Seconds();
	if (NowSeconds >= State->DeadlineSeconds)
	{
		if (State->Stage != EPIEVisualizationStage::WaitingForShutdown)
		{
			Test->AddError(
				TEXT("Timed out during rendered PIE visualization capture."));
			StopPIEVisualizationTest(*State);
			State->Stage = EPIEVisualizationStage::WaitingForShutdown;
			State->DeadlineSeconds = NowSeconds + 10.0;
			return false;
		}
		Test->AddError(
			TEXT("Timed out waiting for PIE visualization test shutdown."));
		StopPIEVisualizationTest(*State);
		DeleteVisualizationArtifact(State->CaptureId);
		return true;
	}

	if (State->Stage == EPIEVisualizationStage::WaitingForPIE)
	{
		const UEAIIntegration::Infrastructure::FPIEControlResult Status =
			State->Controller->Status();
		if (!Status.bSuccess
			|| Status.State != TEXT("running")
			|| Status.SessionId.IsEmpty()
			|| Status.Generation == 0
			|| !GEditor
			|| !GEditor->GetPIEViewport())
		{
			return false;
		}
		State->SessionId = Status.SessionId;
		State->Generation = Status.Generation;

		TSharedRef<FJsonObject> WrongIdentityParams = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> WrongIdentityTarget = MakeShared<FJsonObject>();
		WrongIdentityTarget->SetStringField(TEXT("kind"), TEXT("pie"));
		WrongIdentityTarget->SetStringField(TEXT("sessionId"), State->SessionId);
		WrongIdentityTarget->SetNumberField(
			TEXT("generation"),
			static_cast<double>(State->Generation + 1));
		WrongIdentityParams->SetObjectField(TEXT("target"), WrongIdentityTarget);
		const FMCPToolResult WrongIdentity = State->Registry->ExecuteTool(
			TEXT("scene.viewport.visualization.list"),
			WrongIdentityParams);
		Test->TestFalse(
			TEXT("PIE visualization rejects a stale generation"),
			WrongIdentity.bSuccess);
		Test->TestEqual(
			TEXT("PIE stale generation has a stable identity error"),
			WrongIdentity.ErrorCode,
			FString(TEXT("pie_generation_conflict")));

		TSharedRef<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("kind"), TEXT("pie"));
		Target->SetStringField(TEXT("sessionId"), State->SessionId);
		Target->SetNumberField(
			TEXT("generation"),
			static_cast<double>(State->Generation));
		CaptureParams->SetObjectField(TEXT("target"), Target);
		TSharedRef<FJsonObject> Visualization = MakeShared<FJsonObject>();
		Visualization->SetStringField(TEXT("family"), TEXT("viewMode"));
		Visualization->SetStringField(TEXT("mode"), TEXT("unlit"));
		CaptureParams->SetObjectField(TEXT("visualization"), Visualization);
		CaptureParams->SetNumberField(TEXT("width"), 128);
		CaptureParams->SetNumberField(TEXT("height"), 128);
		const FPIEVisualizationStateRef CaptureState = State;
		const bool bAccepted = State->Registry->BeginExecuteToolAsync(
			TEXT("scene.viewport.visualization.capture"),
			CaptureParams,
			[CaptureState](FMCPToolResult&& Result)
			{
				++CaptureState->CompletionCount;
				CaptureState->CaptureResult.Emplace(MoveTemp(Result));
			});
		Test->TestTrue(
			TEXT("Exact PIE session capture is accepted asynchronously"),
			bAccepted);
		State->Stage = EPIEVisualizationStage::WaitingForCapture;
		State->DeadlineSeconds = NowSeconds + 15.0;
		return false;
	}

	if (State->Stage == EPIEVisualizationStage::WaitingForCapture)
	{
		if (!State->CaptureResult.IsSet())
		{
			return false;
		}
		Test->TestEqual(
			TEXT("PIE capture completes exactly once"),
			State->CompletionCount,
			1);
		const FMCPToolResult& Result = State->CaptureResult.GetValue();
		Test->TestTrue(TEXT("PIE visualization capture succeeds"), Result.bSuccess);
		if (Result.bSuccess && Result.Data.IsValid())
		{
			State->CaptureId = Result.Data->GetStringField(TEXT("captureId"));
			Test->TestEqual(
				TEXT("PIE result preserves the exact session id"),
				Result.Data->GetStringField(TEXT("sessionId")),
				State->SessionId);
			Test->TestEqual(
				TEXT("PIE result preserves the exact generation"),
				static_cast<uint64>(Result.Data->GetNumberField(TEXT("generation"))),
				State->Generation);
			Test->TestEqual(
				TEXT("PIE fingerprint names the PIE target"),
				Result.Data->GetObjectField(TEXT("renderFingerprint"))
					->GetStringField(TEXT("targetKind")),
				FString(TEXT("pie")));
			Test->TestTrue(
				TEXT("PIE capture restores and verifies game viewport/camera state"),
				Result.Data->GetBoolField(TEXT("stateRestored")));
		}
		else if (!Result.bSuccess)
		{
			Test->AddError(FString::Printf(
				TEXT("PIE capture failed [%s]: %s"),
				*Result.ErrorCode,
				*Result.ErrorMessage));
		}
		State->Controller->Stop();
		State->Stage = EPIEVisualizationStage::WaitingForShutdown;
		State->DeadlineSeconds = NowSeconds + 15.0;
		return false;
	}

	if (State->Controller->Status().State != TEXT("stopped")
		|| (GEditor && GEditor->IsPlayingSessionInEditor()))
	{
		return false;
	}
	DeleteVisualizationArtifact(State->CaptureId);
	State->Registry.Reset();
	State->Controller.Reset();
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FViewportVisualizationRenderedPIECaptureTest,
	"UE_AI_integration.ViewportVisualization.RenderedPIETargetIdentity",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter
		| EAutomationTestFlags::NonNullRHI)

bool FViewportVisualizationRenderedPIECaptureTest::RunTest(
	const FString& Parameters)
{
	if (!GEditor || !FApp::CanEverRender() || !GDynamicRHI || GUsingNullRHI)
	{
		AddError(TEXT("Rendered PIE visualization requires Editor and a real RHI."));
		return false;
	}
	if (GEditor->IsPlayingSessionInEditor()
		|| GEditor->IsPlaySessionRequestQueued())
	{
		AddError(TEXT("A PIE session was already active before the test."));
		return false;
	}
	// This customized 5.3 renderer reads the project-owned Fusion photo CVar
	// unconditionally for game views. The portable validation host does not
	// load FusionEffectBuild's game module, so provide its inert default only
	// for this dev-only process test. Stock engines and the real project either
	// do not reference the CVar or have already registered it.
	if (!IConsoleManager::Get().FindConsoleVariable(
			TEXT("r.FusionPhoto.OutputOpt")))
	{
		IConsoleManager::Get().RegisterConsoleVariable(
			TEXT("r.FusionPhoto.OutputOpt"),
			0,
			TEXT("Inert compatibility value for isolated viewport Automation."),
			ECVF_Default);
	}
	const FPIEVisualizationStateRef State =
		MakeShared<FPIEVisualizationState, ESPMode::ThreadSafe>();
	State->Controller = MakeUnique<FPIESessionController>();
	State->Registry = MakeUnique<FMCPToolRegistry>();
	RegisterVisualizationTools(*State->Registry, *State->Controller);
	State->DeadlineSeconds = FPlatformTime::Seconds() + 20.0;
	const UEAIIntegration::Infrastructure::FPIEControlResult Start =
		State->Controller->Start();
	TestTrue(
		TEXT("Rendered PIE visualization queues an embedded PIE session"),
		Start.bSuccess && Start.bRequested);
	if (!Start.bSuccess)
	{
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(
		FWaitForPIEVisualizationCapture(State, this));
	return true;
}

#undef UEAI_TEST_HAS_STRATA_VISUALIZATION
#endif
