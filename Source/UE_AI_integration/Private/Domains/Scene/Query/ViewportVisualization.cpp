// Lossless, state-restoring level/PIE viewport visualization evidence.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "BufferVisualizationData.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "GameFramework/PlayerController.h"
#include "GPUSkinCacheVisualizationData.h"
#include "GroomVisualizationData.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Sha256.h"
#include "Internationalization/Text.h"
#include "LevelEditorViewport.h"
#include "LumenVisualizationData.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "NaniteVisualizationData.h"
#include "RenderCommandFence.h"
#include "RHIGlobals.h"
#include "RHIStrings.h"
#include "RenderUtils.h"
#include "SEditorViewport.h"
#include "Selection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ShaderCore.h"
#include "ShowFlags.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 3
#include "StrataVisualizationData.h"
#define UEAI_HAS_STRATA_VISUALIZATION 1
#else
#define UEAI_HAS_STRATA_VISUALIZATION 0
#endif
#include "Styling/AppStyle.h"
#include "VirtualShadowMapVisualizationData.h"
#include "Widgets/SViewport.h"

namespace UEAIIntegration::ViewportVisualization
{
namespace
{
constexpr int32 MaxCaptureDimension = 4096;
constexpr int64 MaxDecodedBytes = 64ll * 1024ll * 1024ll;
constexpr int64 MaxMetadataBytes = 1024ll * 1024ll;
constexpr int64 MaxInlinePngBytes = 8ll * 1024ll * 1024ll;
constexpr int32 HistogramBinCount = 16;
constexpr int32 MaxChangedRegions = 64;
constexpr double RenderSettlementTimeoutSeconds = 10.0;

struct FModeDescriptor
{
	FString Family;
	FString Mode;
	/** Engine/culture-specific token; never exposed as the stable public id. */
	FString RuntimeToken;
	FString DisplayName;
	FString Description;
	EViewModeIndex ViewMode = VMI_Unknown;
	bool bAvailable = false;
	FString UnavailableReason;
};

struct FTargetContext
{
	FString Kind;
	FViewport* Viewport = nullptr;
	FEditorViewportClient* EditorClient = nullptr;
	UGameViewportClient* GameClient = nullptr;
	UWorld* World = nullptr;
	FString SessionId;
	uint64 Generation = 0;
};

struct FEditorState
{
	EViewModeIndex ViewMode = VMI_Unknown;
	FEngineShowFlags ShowFlags = FEngineShowFlags(ESFIM_Editor);
	FExposureSettings Exposure;
	FVector ViewLocation = FVector::ZeroVector;
	FRotator ViewRotation = FRotator::ZeroRotator;
	ELevelViewportType ViewportType = LVT_Perspective;
	float OrthoZoom = 1.0f;
	float ViewFov = 90.0f;
	FName BufferMode;
	FName NaniteMode;
	FName LumenMode;
	FName StrataMode;
	FName GroomMode;
	FName VirtualShadowMapMode;
	FName RayTracingDebugMode;
	FName GPUSkinCacheMode;
	int32 ViewModeParam = INDEX_NONE;
	FName ViewModeParamName;
	TMap<int32, FName> ViewModeParamNameMap;
	TArray<TWeakObjectPtr<AActor>> SelectedActors;
	TArray<TWeakObjectPtr<UActorComponent>> SelectedComponents;
};

struct FPIEState
{
	int32 ViewMode = VMI_Lit;
	FEngineShowFlags ShowFlags = FEngineShowFlags(ESFIM_Game);
	FString ConsoleVariableName;
	FString ConsoleValue;
	EConsoleVariableFlags ConsoleSetBy = ECVF_SetByConstructor;
	bool bHasConsoleVariable = false;
	TWeakObjectPtr<APlayerCameraManager> CameraManager;
	FVector CameraLocation = FVector::ZeroVector;
	FRotator CameraRotation = FRotator::ZeroRotator;
	float CameraFov = 0.0f;
	bool bHasCamera = false;
};

const TCHAR* FamilyConsoleVariable(const FString& Family);

FString CaptureDirectory()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("ViewportVisualization"));
}

bool IsCaptureIdValid(const FString& CaptureId)
{
	if (CaptureId.IsEmpty() || CaptureId.Len() > 96)
	{
		return false;
	}
	for (const TCHAR Character : CaptureId)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TEXT('-')
			&& Character != TEXT('_'))
		{
			return false;
		}
	}
	return true;
}

FString MetadataPath(const FString& CaptureId)
{
	return FPaths::Combine(CaptureDirectory(), CaptureId + TEXT(".json"));
}

FString ImagePath(const FString& CaptureId)
{
	return FPaths::Combine(CaptureDirectory(), CaptureId + TEXT(".png"));
}

FMCPToolResult InvalidRequest(const FString& Message)
{
	return FMCPToolResult::Error(Message, TEXT("invalid_request"), 400);
}

FMCPToolResult Unavailable(
	const FString& Message,
	const FString& Code = TEXT("viewport_visualization_unavailable"))
{
	return FMCPToolResult::Error(Message, Code, 409);
}

bool SerializeJson(const TSharedPtr<FJsonObject>& Object, FString& OutJson)
{
	OutJson.Reset();
	if (!Object.IsValid())
	{
		return false;
	}
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&OutJson);
	return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}

bool SaveCapture(
	const FString& CaptureId,
	const TArray<uint8>& Png,
	const TSharedPtr<FJsonObject>& Metadata,
	FString& OutError)
{
	IFileManager::Get().MakeDirectory(*CaptureDirectory(), true);
	if (!FFileHelper::SaveArrayToFile(Png, *ImagePath(CaptureId)))
	{
		OutError = TEXT("Could not save the viewport PNG artifact.");
		return false;
	}
	FString Json;
	if (!SerializeJson(Metadata, Json)
		|| !FFileHelper::SaveStringToFile(
			Json,
			*MetadataPath(CaptureId),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		IFileManager::Get().Delete(*ImagePath(CaptureId));
		OutError = TEXT("Could not save viewport capture metadata.");
		return false;
	}
	return true;
}

bool LoadCapture(
	const FString& CaptureId,
	TSharedPtr<FJsonObject>& OutMetadata,
	TArray<uint8>& OutPng,
	FString& OutError)
{
	if (!IsCaptureIdValid(CaptureId))
	{
		OutError = TEXT("captureId is invalid.");
		return false;
	}
	FString Json;
	const int64 MetadataSize =
		IFileManager::Get().FileSize(*MetadataPath(CaptureId));
	if (MetadataSize <= 0 || MetadataSize > MaxMetadataBytes
		|| !FFileHelper::LoadFileToString(Json, *MetadataPath(CaptureId)))
	{
		OutError = TEXT("Viewport capture metadata was not found.");
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Json);
	FString Schema;
	FString MetadataCaptureId;
	FString Format;
	FString StoredHash;
	FString Family;
	FString Mode;
	FString TargetKind;
	if (!FJsonSerializer::Deserialize(Reader, OutMetadata)
		|| !OutMetadata.IsValid()
		|| !OutMetadata->TryGetStringField(TEXT("schema"), Schema)
		|| Schema != TEXT("ue.viewport-visualization-capture.v1")
		|| !OutMetadata->TryGetStringField(
			TEXT("captureId"), MetadataCaptureId)
		|| MetadataCaptureId != CaptureId
		|| !OutMetadata->TryGetStringField(TEXT("format"), Format)
		|| Format != TEXT("png")
		|| !OutMetadata->TryGetStringField(TEXT("imageSha256"), StoredHash)
		|| StoredHash.IsEmpty()
		|| !OutMetadata->TryGetStringField(TEXT("family"), Family)
		|| Family.IsEmpty()
		|| !OutMetadata->TryGetStringField(TEXT("mode"), Mode)
		|| Mode.IsEmpty()
		|| !OutMetadata->TryGetStringField(TEXT("targetKind"), TargetKind)
		|| TargetKind.IsEmpty()
		|| !OutMetadata->HasTypedField<EJson::Object>(
			TEXT("renderFingerprint")))
	{
		OutError = TEXT("Viewport capture metadata is invalid.");
		return false;
	}
	const int64 PngSize = IFileManager::Get().FileSize(*ImagePath(CaptureId));
	if (PngSize <= 0 || PngSize > MaxDecodedBytes
		|| !FFileHelper::LoadFileToArray(OutPng, *ImagePath(CaptureId))
		|| OutPng.IsEmpty()
		|| OutPng.Num() > MaxDecodedBytes)
	{
		OutError = TEXT("Viewport capture PNG was not found or is too large.");
		return false;
	}
	FString ActualHash;
	if (!Infrastructure::TrySha256Hex(OutPng, ActualHash)
		|| ActualHash != StoredHash)
	{
		OutError = TEXT("Viewport capture PNG checksum verification failed.");
		return false;
	}
	return true;
}

bool EncodePng(
	const TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	TArray<uint8>& OutPng,
	FString& OutError)
{
	if (Width <= 0 || Height <= 0
		|| Width > MaxCaptureDimension
		|| Height > MaxCaptureDimension
		|| Pixels.Num() != Width * Height)
	{
		OutError = TEXT("Viewport pixels have invalid dimensions.");
		return false;
	}
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
		OutError = TEXT("Could not initialize the PNG encoder.");
		return false;
	}
	const TArray64<uint8> Compressed = Wrapper->GetCompressed(100);
	if (Compressed.IsEmpty() || Compressed.Num() > MaxDecodedBytes)
	{
		OutError = TEXT("Viewport PNG is empty or exceeds 64 MiB.");
		return false;
	}
	OutPng.SetNumUninitialized(static_cast<int32>(Compressed.Num()));
	FMemory::Memcpy(OutPng.GetData(), Compressed.GetData(), Compressed.Num());
	return true;
}

bool DecodePng(
	const TArray<uint8>& Png,
	TArray<FColor>& OutPixels,
	int32& OutWidth,
	int32& OutHeight,
	FString& OutError)
{
	IImageWrapperModule& Module =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(
			TEXT("ImageWrapper"));
	if (Module.DetectImageFormat(Png.GetData(), Png.Num())
		!= EImageFormat::PNG)
	{
		OutError = TEXT("Viewport evidence must be a lossless PNG.");
		return false;
	}
	const TSharedPtr<IImageWrapper> Wrapper =
		Module.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid()
		|| !Wrapper->SetCompressed(Png.GetData(), Png.Num()))
	{
		OutError = TEXT("Viewport PNG could not be decoded.");
		return false;
	}
	const int64 Width = Wrapper->GetWidth();
	const int64 Height = Wrapper->GetHeight();
	const int64 PixelCount = Width * Height;
	if (Width <= 0 || Height <= 0
		|| Width > MaxCaptureDimension
		|| Height > MaxCaptureDimension
		|| PixelCount <= 0
		|| PixelCount * sizeof(FColor) > MaxDecodedBytes)
	{
		OutError = TEXT("Viewport PNG dimensions exceed the supported range.");
		return false;
	}
	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw)
		|| Raw.Num() != PixelCount * sizeof(FColor))
	{
		OutError = TEXT("Viewport PNG pixel data is invalid.");
		return false;
	}
	OutWidth = static_cast<int32>(Width);
	OutHeight = static_cast<int32>(Height);
	OutPixels.SetNumUninitialized(static_cast<int32>(PixelCount));
	FMemory::Memcpy(OutPixels.GetData(), Raw.GetData(), Raw.Num());
	return true;
}

void ResizePixels(
	const TArray<FColor>& Source,
	const int32 SourceWidth,
	const int32 SourceHeight,
	TArray<FColor>& OutPixels,
	const int32 TargetWidth,
	const int32 TargetHeight)
{
	if (SourceWidth == TargetWidth && SourceHeight == TargetHeight)
	{
		OutPixels = Source;
		return;
	}
	OutPixels.SetNumUninitialized(TargetWidth * TargetHeight);
	for (int32 Y = 0; Y < TargetHeight; ++Y)
	{
		const int32 SourceY = FMath::Clamp(
			static_cast<int32>(
				static_cast<int64>(Y) * SourceHeight / TargetHeight),
			0,
			SourceHeight - 1);
		for (int32 X = 0; X < TargetWidth; ++X)
		{
			const int32 SourceX = FMath::Clamp(
				static_cast<int32>(
					static_cast<int64>(X) * SourceWidth / TargetWidth),
				0,
				SourceWidth - 1);
			OutPixels[Y * TargetWidth + X] =
				Source[SourceY * SourceWidth + SourceX];
		}
	}
}

bool IsCVarEnabled(const TCHAR* Name, const bool bDefault = false)
{
	const IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(Name);
	return Variable
		? FCString::Atoi(*Variable->GetString()) != 0
		: bDefault;
}

FString TargetRenderUnavailableReason(
	const FTargetContext* Target,
	const bool bRequireDebugViewModes)
{
	if (!GDynamicRHI || GUsingNullRHI || !FApp::CanEverRender())
	{
		return TEXT("No rendered dynamic RHI is active (NullRHI is evidence-only).");
	}
	if (!Target || !Target->Viewport || !Target->World)
	{
		return TEXT("The requested rendered viewport target is unavailable.");
	}
	const FIntPoint Size = Target->Viewport->GetSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return TEXT("The requested viewport has no renderable surface.");
	}
	if (bRequireDebugViewModes && !AllowDebugViewmodes())
	{
		return TEXT("Debug view modes are disabled for the active shader platform.");
	}
	return FString();
}

FString BasicViewportUnavailableReason(const FTargetContext* Target)
{
	return TargetRenderUnavailableReason(Target, false);
}

void AddStandardModes(
	const FTargetContext* Target,
	TArray<FModeDescriptor>& OutModes)
{
	const FString Reason = BasicViewportUnavailableReason(Target);
	auto Add = [&OutModes, &Reason](
		const TCHAR* Mode,
		const TCHAR* DisplayName,
		const EViewModeIndex ViewMode)
	{
		FModeDescriptor Item;
		Item.Family = TEXT("viewMode");
		Item.Mode = Mode;
		Item.RuntimeToken = Mode;
		Item.DisplayName = DisplayName;
		Item.ViewMode = ViewMode;
		Item.bAvailable = Reason.IsEmpty();
		Item.UnavailableReason = Reason;
		OutModes.Add(MoveTemp(Item));
	};
	Add(TEXT("lit"), TEXT("Lit"), VMI_Lit);
	Add(TEXT("unlit"), TEXT("Unlit"), VMI_Unlit);
	Add(TEXT("wireframe"), TEXT("Wireframe"), VMI_Wireframe);
	Add(
		TEXT("detailLighting"),
		TEXT("Detail Lighting"),
		VMI_Lit_DetailLighting);
	Add(TEXT("lightingOnly"), TEXT("Lighting Only"), VMI_LightingOnly);
	Add(TEXT("reflections"), TEXT("Reflections"), VMI_ReflectionOverride);
	Add(
		TEXT("playerCollision"),
		TEXT("Player Collision"),
		VMI_CollisionPawn);
	Add(
		TEXT("visibilityCollision"),
		TEXT("Visibility Collision"),
		VMI_CollisionVisibility);
}

struct FBufferModeCollector
{
	TArray<FModeDescriptor>& Modes;
	FString GlobalReason;
	TSet<FString> Seen;

	void ProcessValue(
		const FString& Name,
		UMaterialInterface* Material,
		const FText& DisplayName)
	{
		if (Seen.Contains(Name))
		{
			return;
		}
		Seen.Add(Name);
		FModeDescriptor Item;
		Item.Family = TEXT("buffer");
		Item.Mode = Name;
		Item.RuntimeToken = Name;
		Item.DisplayName = DisplayName.ToString();
		Item.ViewMode = VMI_VisualizeBuffer;
		Item.bAvailable = GlobalReason.IsEmpty() && Material != nullptr;
		Item.UnavailableReason = GlobalReason.IsEmpty() && !Material
			? TEXT("The buffer visualization material is not loaded.")
			: GlobalReason;
		Modes.Add(MoveTemp(Item));
	}
};

template<typename TModeMap>
void AddMapModes(
	const FString& Family,
	const EViewModeIndex ViewMode,
	const TModeMap& ModeMap,
	const bool bFeatureAvailable,
	const FString& FeatureReason,
	TArray<FModeDescriptor>& OutModes)
{
	TSet<FString> Seen;
	for (const auto& Pair : ModeMap)
	{
		const auto& Record = Pair.Value;
		const FString Name = Record.ModeName.ToString();
		if (Name.IsEmpty() || Seen.Contains(Name))
		{
			continue;
		}
		Seen.Add(Name);
		FModeDescriptor Item;
		Item.Family = Family;
		Item.Mode = Name;
		Item.RuntimeToken = Name;
		Item.DisplayName = Record.ModeText.ToString();
		Item.Description = Record.ModeDesc.ToString();
		Item.ViewMode = ViewMode;
		Item.bAvailable = bFeatureAvailable;
		Item.UnavailableReason = bFeatureAvailable
			? FString()
			: FeatureReason;
		OutModes.Add(MoveTemp(Item));
	}
}

void AddUnavailableFamilyPlaceholder(
	const FString& Family,
	const FString& DisplayName,
	const EViewModeIndex ViewMode,
	const FString& Reason,
	const int32 FamilyStartIndex,
	TArray<FModeDescriptor>& OutModes)
{
	if (OutModes.Num() != FamilyStartIndex)
	{
		return;
	}
	FModeDescriptor Item;
	Item.Family = Family;
	Item.Mode = TEXT("unavailable");
	Item.RuntimeToken = TEXT("unavailable");
	Item.DisplayName = DisplayName;
	Item.Description =
		TEXT("This visualization family is not available in the active Engine/RHI configuration.");
	Item.ViewMode = ViewMode;
	Item.bAvailable = false;
	Item.UnavailableReason = Reason.IsEmpty()
		? TEXT("No visualization modes are registered for this family.")
		: Reason;
	OutModes.Add(MoveTemp(Item));
}

#if UEAI_HAS_STRATA_VISUALIZATION
void AddStrataModes(
	const FStrataVisualizationData::TModeMap& ModeMap,
	const bool bFeatureAvailable,
	const FString& FeatureReason,
	TArray<FModeDescriptor>& OutModes)
{
	TSet<FString> Seen;
	for (const auto& Pair : ModeMap)
	{
		const FStrataVisualizationData::FModeRecord& Record = Pair.Value;
		const FString Name = Record.ModeName.ToString();
		if (Name.IsEmpty() || Seen.Contains(Name))
		{
			continue;
		}
		Seen.Add(Name);
		FModeDescriptor Item;
		Item.Family = TEXT("strata");
		Item.Mode = Name;
		Item.RuntimeToken = Name;
		Item.DisplayName = Record.ModeText.ToString();
		Item.Description = Record.ModeDesc.ToString();
		Item.ViewMode = VMI_VisualizeSubstrate;
		Item.bAvailable = bFeatureAvailable && Record.bAvailableCommand;
		if (!bFeatureAvailable)
		{
			Item.UnavailableReason = FeatureReason;
		}
		else if (!Record.bAvailableCommand)
		{
			Item.UnavailableReason = Record.UnavailableReason.IsEmpty()
				? TEXT("This Substrate/Strata visualization mode is unavailable.")
				: Record.UnavailableReason.ToString();
		}
		OutModes.Add(MoveTemp(Item));
	}
}
#endif

void AddRayTracingDebugModes(
	const FTargetContext* Target,
	TArray<FModeDescriptor>& OutModes)
{
	const FString GlobalReason = TargetRenderUnavailableReason(Target, true);
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 3
	constexpr bool bCompatibilityTableVerified = true;
	#else
	constexpr bool bCompatibilityTableVerified = false;
	#endif
	const bool bAvailable = bCompatibilityTableVerified
		&& GlobalReason.IsEmpty()
		&& GRHISupportsRayTracing
		&& IsRayTracingEnabled();
	const FString Reason = bAvailable
		? FString()
		: (!bCompatibilityTableVerified
			? FString::Printf(
				TEXT("Ray Tracing Debug token compatibility has not been verified for UE %d.%d; modes remain discoverable but unavailable."),
				ENGINE_MAJOR_VERSION,
				ENGINE_MINOR_VERSION)
			: (!GlobalReason.IsEmpty()
			? GlobalReason
			: TEXT("Hardware ray tracing is not enabled for the active RHI/project.")));
	auto Add = [&OutModes, bAvailable, &Reason](
		const TCHAR* CanonicalId,
		const FText& RuntimeText,
		const bool bNeedsTimestamps = false)
	{
		FModeDescriptor Item;
		Item.Family = TEXT("rayTracingDebug");
		Item.Mode = CanonicalId;
		// UE 5.3 Renderer and menu commands both key their map with the
		// localized LOCTEXT value. Keep that unstable token behind a stable id.
		Item.RuntimeToken = RuntimeText.ToString();
		Item.DisplayName = RuntimeText.ToString();
		Item.ViewMode = VMI_RayTracingDebug;
		const bool bTimestampModeUnavailable =
			bNeedsTimestamps && !GRHISupportsShaderTimestamp;
		Item.bAvailable = bAvailable && !bTimestampModeUnavailable;
		Item.UnavailableReason = !bAvailable
			? Reason
			: (bTimestampModeUnavailable
				? TEXT("The active RHI does not support shader timestamp queries.")
				: FString());
		OutModes.Add(MoveTemp(Item));
	};

	// Versioned compatibility table. Canonical ids are stable; namespace/key
	// pairs intentionally match the Engine renderer for the current culture.
	#define UEAI_RT_MODE(Id, Key, Source) \
		Add(TEXT(Id), NSLOCTEXT("RayTracingDebugVisualizationMenuCommands", Key, Source))
	UEAI_RT_MODE("radiance", "Radiance", "Radiance");
	UEAI_RT_MODE("worldNormal", "World Normal", "World Normal");
	UEAI_RT_MODE("baseColor", "BaseColor", "BaseColor");
	UEAI_RT_MODE("diffuseColor", "DiffuseColor", "DiffuseColor");
	UEAI_RT_MODE("specularColor", "SpecularColor", "SpecularColor");
	UEAI_RT_MODE("opacity", "Opacity", "Opacity");
	UEAI_RT_MODE("metallic", "Metallic", "Metallic");
	UEAI_RT_MODE("specular", "Specular", "Specular");
	UEAI_RT_MODE("roughness", "Roughness", "Roughness");
	UEAI_RT_MODE("ior", "Ior", "Ior");
	UEAI_RT_MODE("shadingModelId", "ShadingModelID", "ShadingModelID");
	UEAI_RT_MODE("blendingMode", "BlendingMode", "BlendingMode");
	UEAI_RT_MODE("primitiveLightingChannelMask", "PrimitiveLightingChannelMask", "PrimitiveLightingChannelMask");
	UEAI_RT_MODE("customData", "CustomData", "CustomData");
	UEAI_RT_MODE("gBufferAo", "GBufferAO", "GBufferAO");
	UEAI_RT_MODE("indirectIrradiance", "IndirectIrradiance", "IndirectIrradiance");
	UEAI_RT_MODE("worldPosition", "World Position", "World Position");
	UEAI_RT_MODE("hitKind", "HitKind", "HitKind");
	UEAI_RT_MODE("barycentrics", "Barycentrics", "Barycentrics");
	UEAI_RT_MODE("primaryRays", "PrimaryRays", "PrimaryRays");
	UEAI_RT_MODE("worldTangent", "World Tangent", "World Tangent");
	UEAI_RT_MODE("anisotropy", "Anisotropy", "Anisotropy");
	UEAI_RT_MODE("instances", "Instances", "Instances");
	UEAI_RT_MODE("instanceOverlap", "Instance Overlap", "Instance Overlap");
	Add(
		TEXT("performance"),
		NSLOCTEXT(
			"RayTracingDebugVisualizationMenuCommands",
			"Performance",
			"Performance"),
		true);
	UEAI_RT_MODE("triangles", "Triangles", "Triangles");
	UEAI_RT_MODE("farField", "FarField", "FarField");
	UEAI_RT_MODE("dynamicInstances", "Dynamic Instances", "Dynamic Instances");
	UEAI_RT_MODE("proxyType", "Proxy Type", "Proxy Type");
	UEAI_RT_MODE("picker", "Picker", "Picker");
	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 3
	// The renderer map contains these modes from the supported 5.3 baseline,
	// even though some Engine minors omit them from the public viewport menu.
	// Keep the table version-gated because there is no public enumeration API.
	UEAI_RT_MODE("traversalNode", "Traversal Node", "Traversal Node");
	UEAI_RT_MODE("traversalCluster", "Traversal Cluster", "Traversal Cluster");
	UEAI_RT_MODE("traversalTriangle", "Traversal Triangle", "Traversal Triangle");
	UEAI_RT_MODE("traversalAll", "Traversal All", "Traversal All");
	UEAI_RT_MODE("traversalStatistics", "Traversal Statistics", "Traversal Statistics");
	#endif
	#undef UEAI_RT_MODE
}

TArray<FModeDescriptor> BuildModeCatalog(const FTargetContext* Target)
{
	TArray<FModeDescriptor> Modes;
	AddStandardModes(Target, Modes);
	const FString GlobalReason =
		TargetRenderUnavailableReason(Target, true);
	const EShaderPlatform ShaderPlatform =
		Target && Target->World
			? GShaderPlatformForFeatureLevel[Target->World->GetFeatureLevel()]
			: GMaxRHIShaderPlatform;

	const int32 BufferStartIndex = Modes.Num();
	FBufferModeCollector BufferCollector{Modes, GlobalReason, {}};
	GetBufferVisualizationData().IterateOverAvailableMaterials(BufferCollector);
	AddUnavailableFamilyPlaceholder(
		TEXT("buffer"),
		TEXT("Buffer Visualization"),
		VMI_VisualizeBuffer,
		GlobalReason.IsEmpty()
			? TEXT("No Buffer Visualization materials are registered.")
			: GlobalReason,
		BufferStartIndex,
		Modes);

	const bool bNaniteProjectEnabled =
		IsCVarEnabled(TEXT("r.Nanite.ProjectEnabled"));
	const bool bNanitePlatformSupported =
		DoesPlatformSupportNanite(ShaderPlatform, false);
	const bool bNanite = GlobalReason.IsEmpty()
		&& bNaniteProjectEnabled
		&& bNanitePlatformSupported;
	const int32 NaniteStartIndex = Modes.Num();
	const FString NaniteReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: (!bNanitePlatformSupported
			? TEXT("Nanite is unsupported by the active shader platform/RHI.")
			: (!bNaniteProjectEnabled
				? TEXT("Nanite is not enabled for the project.")
				: TEXT("No Nanite visualization modes are registered.")));
	AddMapModes(
		TEXT("nanite"),
		VMI_VisualizeNanite,
		GetNaniteVisualizationData().GetModeMap(),
		bNanite,
		NaniteReason,
		Modes);
	AddUnavailableFamilyPlaceholder(
		TEXT("nanite"),
		TEXT("Nanite"),
		VMI_VisualizeNanite,
		NaniteReason,
		NaniteStartIndex,
		Modes);

	const int32 LumenStartIndex = Modes.Num();
	const bool bLumen = GlobalReason.IsEmpty()
		&& IsCVarEnabled(TEXT("r.Lumen.DiffuseIndirect.Allow"), true);
	const FString LumenReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: (bLumen
			? TEXT("No Lumen visualization modes are registered.")
			: TEXT("Lumen diffuse indirect is disabled."));
	AddMapModes(
		TEXT("lumen"),
		VMI_VisualizeLumen,
		GetLumenVisualizationData().GetModeMap(),
		bLumen,
		LumenReason,
		Modes);
	AddUnavailableFamilyPlaceholder(
		TEXT("lumen"),
		TEXT("Lumen"),
		VMI_VisualizeLumen,
		LumenReason,
		LumenStartIndex,
		Modes);

	const int32 VirtualShadowMapStartIndex = Modes.Num();
	const bool bVirtualShadowMaps = GlobalReason.IsEmpty()
		&& IsCVarEnabled(TEXT("r.Shadow.Virtual.Enable"));
	const FString VirtualShadowMapReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: (bVirtualShadowMaps
			? TEXT("No Virtual Shadow Map visualization modes are registered.")
			: TEXT("Virtual Shadow Maps are disabled."));
	AddMapModes(
		TEXT("virtualShadowMap"),
		VMI_VisualizeVirtualShadowMap,
		GetVirtualShadowMapVisualizationData().GetModeMap(),
		bVirtualShadowMaps,
		VirtualShadowMapReason,
		Modes);
	AddUnavailableFamilyPlaceholder(
		TEXT("virtualShadowMap"),
		TEXT("Virtual Shadow Map"),
		VMI_VisualizeVirtualShadowMap,
		VirtualShadowMapReason,
		VirtualShadowMapStartIndex,
		Modes);

	const int32 SkinCacheStartIndex = Modes.Num();
	const bool bSkinCache = GlobalReason.IsEmpty()
		&& IsCVarEnabled(TEXT("r.SkinCache.CompileShaders"));
	const FString SkinCacheReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: (bSkinCache
			? TEXT("No GPU Skin Cache visualization modes are registered.")
			: TEXT("GPU Skin Cache shaders are disabled."));
	AddMapModes(
		TEXT("gpuSkinCache"),
		VMI_VisualizeGPUSkinCache,
		GetGPUSkinCacheVisualizationData().GetModeMap(),
		bSkinCache,
		SkinCacheReason,
		Modes);
	AddUnavailableFamilyPlaceholder(
		TEXT("gpuSkinCache"),
		TEXT("GPU Skin Cache"),
		VMI_VisualizeGPUSkinCache,
		SkinCacheReason,
		SkinCacheStartIndex,
		Modes);

	const int32 StrataStartIndex = Modes.Num();
	#if UEAI_HAS_STRATA_VISUALIZATION
	const bool bStrata = GlobalReason.IsEmpty() && Strata::IsStrataEnabled();
	const FString StrataReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: (bStrata
			? TEXT("No Substrate/Strata visualization modes are registered.")
			: TEXT("Substrate/Strata is disabled."));
	AddStrataModes(
		GetStrataVisualizationData().GetModeMap(),
		bStrata,
		StrataReason,
		Modes);
	#else
	const FString StrataReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: TEXT("Substrate/Strata visualization requires a version-specific Compatibility adapter for this Engine minor version.");
	#endif
	AddUnavailableFamilyPlaceholder(
		TEXT("strata"),
		TEXT("Substrate/Strata"),
		#if UEAI_HAS_STRATA_VISUALIZATION
		VMI_VisualizeSubstrate,
		#else
		VMI_Unknown,
		#endif
		StrataReason,
		StrataStartIndex,
		Modes);

	const bool bGroom = GlobalReason.IsEmpty() && IsGroomEnabled();
	const int32 GroomStartIndex = Modes.Num();
	const FString GroomReason = !GlobalReason.IsEmpty()
		? GlobalReason
		: (bGroom
			? TEXT("No Groom/Hair Strands visualization modes are registered.")
			: TEXT("Hair strands rendering is disabled."));
	AddMapModes(
		TEXT("groom"),
		VMI_VisualizeGroom,
		GetGroomVisualizationData().GetModeMap(),
		bGroom,
		GroomReason,
		Modes);
	AddUnavailableFamilyPlaceholder(
		TEXT("groom"),
		TEXT("Groom/Hair Strands"),
		VMI_VisualizeGroom,
		GroomReason,
		GroomStartIndex,
		Modes);

	AddRayTracingDebugModes(Target, Modes);
	Modes.Sort([](const FModeDescriptor& Left, const FModeDescriptor& Right)
	{
		const int32 FamilyCompare = Left.Family.Compare(Right.Family);
		return FamilyCompare == 0
			? Left.Mode < Right.Mode
			: FamilyCompare < 0;
	});
	return Modes;
}

const FModeDescriptor* FindMode(
	const TArray<FModeDescriptor>& Modes,
	const FString& Family,
	const FString& Mode)
{
	return Modes.FindByPredicate(
		[&Family, &Mode](const FModeDescriptor& Candidate)
		{
			return Candidate.Family.Equals(Family, ESearchCase::IgnoreCase)
				&& Candidate.Mode.Equals(Mode, ESearchCase::IgnoreCase);
		});
}

TSharedRef<FJsonObject> ModeToJson(const FModeDescriptor& Mode)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("family"), Mode.Family);
	Json->SetStringField(TEXT("mode"), Mode.Mode);
	Json->SetStringField(TEXT("displayName"), Mode.DisplayName);
	Json->SetStringField(TEXT("description"), Mode.Description);
	Json->SetBoolField(TEXT("available"), Mode.bAvailable);
	if (!Mode.bAvailable)
	{
		Json->SetStringField(
			TEXT("unavailableReason"),
			Mode.UnavailableReason);
	}
	return Json;
}

bool ResolveEditorTarget(FTargetContext& OutTarget, FString& OutError)
{
	if (!GEditor)
	{
		OutError = TEXT("Unreal Editor is unavailable.");
		return false;
	}
	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	FLevelEditorViewportClient* LevelClient = nullptr;
	for (FLevelEditorViewportClient* Candidate :
		GEditor->GetLevelViewportClients())
	{
		if (Candidate && Candidate->Viewport == ActiveViewport)
		{
			LevelClient = Candidate;
			break;
		}
	}
	if (!LevelClient && GCurrentLevelEditingViewportClient)
	{
		LevelClient = GCurrentLevelEditingViewportClient;
		ActiveViewport = LevelClient->Viewport;
	}
	if (!LevelClient || !ActiveViewport)
	{
		OutError = TEXT("No active Level Editor viewport is available.");
		return false;
	}
	OutTarget.Kind = TEXT("editor");
	OutTarget.Viewport = ActiveViewport;
	OutTarget.EditorClient = LevelClient;
	OutTarget.World = LevelClient->GetWorld();
	return true;
}

bool ResolvePIETarget(
	const TSharedPtr<FJsonObject>& TargetObject,
	Infrastructure::FPIESessionController& Controller,
	FTargetContext& OutTarget,
	FString& OutError,
	FString& OutErrorCode)
{
	FString SessionId;
	double GenerationNumber = 0.0;
	if (!TargetObject.IsValid()
		|| !TargetObject->TryGetStringField(TEXT("sessionId"), SessionId)
		|| SessionId.IsEmpty()
		|| !TargetObject->TryGetNumberField(
			TEXT("generation"),
			GenerationNumber)
		|| !FMath::IsFinite(GenerationNumber)
		|| GenerationNumber < 1.0
		|| GenerationNumber > 9007199254740991.0
		|| FMath::FloorToDouble(GenerationNumber) != GenerationNumber)
	{
		OutError = TEXT("PIE targets require sessionId and integer generation.");
		OutErrorCode = TEXT("invalid_request");
		return false;
	}
	const Infrastructure::FPIEControlResult Status = Controller.Status();
	if (!Status.bSuccess
		|| (Status.State != TEXT("running") && Status.State != TEXT("paused")))
	{
		OutError = TEXT("The requested PIE session is not running.");
		OutErrorCode = TEXT("pie_unavailable");
		return false;
	}
	if (Status.SessionId != SessionId
		|| Status.Generation != static_cast<uint64>(GenerationNumber))
	{
		OutError = TEXT("The viewport target belongs to another PIE generation.");
		OutErrorCode = TEXT("pie_generation_conflict");
		return false;
	}
	FViewport* Viewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
	FViewportClient* ViewportClient = Viewport ? Viewport->GetClient() : nullptr;
	UWorld* World = ViewportClient ? ViewportClient->GetWorld() : nullptr;
	UGameViewportClient* GameClient = World ? World->GetGameViewport() : nullptr;
	if (!Viewport || !World || World->WorldType != EWorldType::PIE || !GameClient)
	{
		OutError = TEXT("The requested PIE session has no rendered game viewport.");
		OutErrorCode = TEXT("pie_viewport_unavailable");
		return false;
	}
	int32 RenderedPIEViewportCount = 0;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* CandidateWorld = Context.World();
			UGameViewportClient* CandidateClient = CandidateWorld
				? CandidateWorld->GetGameViewport()
				: nullptr;
			if (CandidateWorld
				&& CandidateWorld->WorldType == EWorldType::PIE
				&& CandidateClient
				&& CandidateClient->Viewport)
			{
				++RenderedPIEViewportCount;
			}
		}
	}
	const bool bIdentityMatches =
		RenderedPIEViewportCount == 1
		&& ViewportClient == static_cast<FViewportClient*>(GameClient)
		&& GameClient->Viewport == Viewport
		&& GameClient->GetGameViewport() == Viewport
		&& GameClient->GetWorld() == World
		&& World->GetGameViewport() == GameClient
		&& GameClient->Viewport->GetClient() == ViewportClient;
	if (!bIdentityMatches)
	{
		OutError = RenderedPIEViewportCount > 1
			? TEXT("Multiple rendered PIE viewports are active; sessionId and generation do not identify one viewport client.")
			: TEXT("The PIE viewport, World, and GameViewportClient do not resolve to one consistent rendered target.");
		OutErrorCode = TEXT("pie_viewport_ambiguous");
		return false;
	}
	OutTarget.Kind = TEXT("pie");
	OutTarget.Viewport = Viewport;
	OutTarget.GameClient = GameClient;
	OutTarget.World = World;
	OutTarget.SessionId = SessionId;
	OutTarget.Generation = static_cast<uint64>(GenerationNumber);
	return true;
}

bool ResolveTarget(
	const TSharedPtr<FJsonObject>& Params,
	Infrastructure::FPIESessionController& Controller,
	FTargetContext& OutTarget,
	FString& OutError,
	FString& OutErrorCode)
{
	TSharedPtr<FJsonObject> TargetObject;
	const TSharedPtr<FJsonObject>* TargetPointer = nullptr;
	if (Params.IsValid()
		&& Params->TryGetObjectField(TEXT("target"), TargetPointer)
		&& TargetPointer)
	{
		TargetObject = *TargetPointer;
	}
	FString Kind = TEXT("editor");
	if (TargetObject.IsValid())
	{
		TargetObject->TryGetStringField(TEXT("kind"), Kind);
	}
	if (Kind.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
	{
		OutErrorCode = TEXT("viewport_unavailable");
		return ResolveEditorTarget(OutTarget, OutError);
	}
	if (Kind.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
	{
		return ResolvePIETarget(
			TargetObject,
			Controller,
			OutTarget,
			OutError,
			OutErrorCode);
	}
	OutError = TEXT("target.kind must be editor or pie.");
	OutErrorCode = TEXT("invalid_request");
	return false;
}

void CaptureSelectedActors(TArray<TWeakObjectPtr<AActor>>& OutActors)
{
	OutActors.Reset();
	if (!GEditor || !GEditor->GetSelectedActors())
	{
		return;
	}
	for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
	{
		if (AActor* Actor = Cast<AActor>(*It))
		{
			OutActors.Add(Actor);
		}
	}
}

void CaptureSelectedComponents(
	TArray<TWeakObjectPtr<UActorComponent>>& OutComponents)
{
	OutComponents.Reset();
	if (!GEditor || !GEditor->GetSelectedComponents())
	{
		return;
	}
	for (FSelectionIterator It(*GEditor->GetSelectedComponents()); It; ++It)
	{
		if (UActorComponent* Component = Cast<UActorComponent>(*It))
		{
			OutComponents.Add(Component);
		}
	}
}

template <typename TObject>
bool WeakSelectionMatches(
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

bool SelectionMatches(
	const TArray<TWeakObjectPtr<AActor>>& Actors,
	const TArray<TWeakObjectPtr<UActorComponent>>& Components)
{
	TArray<TWeakObjectPtr<AActor>> CurrentActors;
	TArray<TWeakObjectPtr<UActorComponent>> CurrentComponents;
	CaptureSelectedActors(CurrentActors);
	CaptureSelectedComponents(CurrentComponents);
	return WeakSelectionMatches(CurrentActors, Actors)
		&& WeakSelectionMatches(CurrentComponents, Components);
}

void RestoreSelection(
	const TArray<TWeakObjectPtr<AActor>>& Actors,
	const TArray<TWeakObjectPtr<UActorComponent>>& Components)
{
	if (!GEditor || SelectionMatches(Actors, Components))
	{
		return;
	}
	USelection* ActorSelection = GEditor->GetSelectedActors();
	USelection* ComponentSelection = GEditor->GetSelectedComponents();
	if (!ActorSelection || !ComponentSelection)
	{
		return;
	}

	// This capture owns only Level Viewport Actor/Component selection. Do not
	// use SelectNone here: it also changes BSP surfaces and unrelated editor
	// selection domains such as Content Browser assets.
	ActorSelection->BeginBatchSelectOperation();
	ComponentSelection->BeginBatchSelectOperation();
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
	ActorSelection->EndBatchSelectOperation(false);
	ComponentSelection->EndBatchSelectOperation(false);
	GEditor->NoteSelectionChange();
}

FEditorState CaptureEditorState(FEditorViewportClient& Client)
{
	FEditorState State;
	State.ViewMode = Client.GetViewMode();
	State.ShowFlags = Client.EngineShowFlags;
	State.Exposure = Client.ExposureSettings;
	State.ViewLocation = Client.GetViewLocation();
	State.ViewRotation = Client.GetViewRotation();
	State.ViewportType = Client.GetViewportType();
	State.OrthoZoom = Client.GetOrthoZoom();
	State.ViewFov = Client.ViewFOV;
	State.BufferMode = Client.CurrentBufferVisualizationMode;
	State.NaniteMode = Client.CurrentNaniteVisualizationMode;
	State.LumenMode = Client.CurrentLumenVisualizationMode;
	#if UEAI_HAS_STRATA_VISUALIZATION
	State.StrataMode = Client.CurrentStrataVisualizationMode;
	#endif
	State.GroomMode = Client.CurrentGroomVisualizationMode;
	State.VirtualShadowMapMode = Client.CurrentVirtualShadowMapVisualizationMode;
	State.RayTracingDebugMode = Client.CurrentRayTracingDebugVisualizationMode;
	State.GPUSkinCacheMode = Client.CurrentGPUSkinCacheVisualizationMode;
	State.ViewModeParamNameMap = Client.GetViewModeParamNameMap();
	for (const TPair<int32, FName>& Pair : State.ViewModeParamNameMap)
	{
		if (Client.IsViewModeParam(Pair.Key))
		{
			State.ViewModeParam = Pair.Key;
			State.ViewModeParamName = Pair.Value;
			break;
		}
	}
	CaptureSelectedActors(State.SelectedActors);
	CaptureSelectedComponents(State.SelectedComponents);
	return State;
}

bool ViewModeParamStateMatches(
	FEditorViewportClient& Client,
	const FEditorState& State)
{
	const TMap<int32, FName>& CurrentMap =
		Client.GetViewModeParamNameMap();
	if (CurrentMap.Num() != State.ViewModeParamNameMap.Num())
	{
		return false;
	}
	for (const TPair<int32, FName>& Pair : State.ViewModeParamNameMap)
	{
		const FName* CurrentName = CurrentMap.Find(Pair.Key);
		if (!CurrentName || *CurrentName != Pair.Value)
		{
			return false;
		}
	}
	if (State.ViewModeParam == INDEX_NONE)
	{
		return true;
	}
	const FName* CurrentName = CurrentMap.Find(State.ViewModeParam);
	return CurrentName
		&& *CurrentName == State.ViewModeParamName
		&& Client.IsViewModeParam(State.ViewModeParam);
}

bool RestoreEditorState(
	FEditorViewportClient& Client,
	const FEditorState& State)
{
	Client.SetViewportType(State.ViewportType);
	Client.SetViewMode(State.ViewMode);
	Client.EngineShowFlags = State.ShowFlags;
	Client.ExposureSettings = State.Exposure;
	Client.SetViewLocation(State.ViewLocation);
	Client.SetViewRotation(State.ViewRotation);
	Client.SetOrthoZoom(State.OrthoZoom);
	Client.ViewFOV = State.ViewFov;
	Client.CurrentBufferVisualizationMode = State.BufferMode;
	Client.CurrentNaniteVisualizationMode = State.NaniteMode;
	Client.CurrentLumenVisualizationMode = State.LumenMode;
	#if UEAI_HAS_STRATA_VISUALIZATION
	Client.CurrentStrataVisualizationMode = State.StrataMode;
	#endif
	Client.CurrentGroomVisualizationMode = State.GroomMode;
	Client.CurrentVirtualShadowMapVisualizationMode =
		State.VirtualShadowMapMode;
	Client.CurrentRayTracingDebugVisualizationMode =
		State.RayTracingDebugMode;
	Client.CurrentGPUSkinCacheVisualizationMode = State.GPUSkinCacheMode;
	Client.GetViewModeParamNameMap() = State.ViewModeParamNameMap;
	if (State.ViewModeParam != INDEX_NONE)
	{
		Client.SetViewModeParam(State.ViewModeParam);
	}
	RestoreSelection(State.SelectedActors, State.SelectedComponents);
	Client.UpdateHiddenCollisionDrawing();
	Client.Invalidate();
	#if UEAI_HAS_STRATA_VISUALIZATION
	const bool bStrataStateMatches =
		Client.CurrentStrataVisualizationMode == State.StrataMode;
	#else
	const bool bStrataStateMatches = true;
	#endif
	return Client.GetViewMode() == State.ViewMode
		&& Client.EngineShowFlags.ToString() == State.ShowFlags.ToString()
		&& Client.ExposureSettings.ToString() == State.Exposure.ToString()
		&& Client.GetViewLocation().Equals(State.ViewLocation)
		&& Client.GetViewRotation().Equals(State.ViewRotation)
		&& Client.GetViewportType() == State.ViewportType
		&& FMath::IsNearlyEqual(Client.GetOrthoZoom(), State.OrthoZoom)
		&& FMath::IsNearlyEqual(Client.ViewFOV, State.ViewFov)
		&& Client.CurrentBufferVisualizationMode == State.BufferMode
		&& Client.CurrentNaniteVisualizationMode == State.NaniteMode
		&& Client.CurrentLumenVisualizationMode == State.LumenMode
		&& bStrataStateMatches
		&& Client.CurrentGroomVisualizationMode == State.GroomMode
		&& Client.CurrentVirtualShadowMapVisualizationMode
			== State.VirtualShadowMapMode
		&& Client.CurrentRayTracingDebugVisualizationMode
			== State.RayTracingDebugMode
		&& Client.CurrentGPUSkinCacheVisualizationMode
			== State.GPUSkinCacheMode
		&& ViewModeParamStateMatches(Client, State)
		&& SelectionMatches(
			State.SelectedActors,
			State.SelectedComponents);
}

FPIEState CapturePIEState(
	UGameViewportClient& Client,
	const FModeDescriptor& Mode)
{
	FPIEState State;
	State.ViewMode = Client.ViewModeIndex;
	State.ShowFlags = Client.EngineShowFlags;
	if (const TCHAR* Name = FamilyConsoleVariable(Mode.Family))
	{
		if (const IConsoleVariable* Variable =
				IConsoleManager::Get().FindConsoleVariable(Name))
		{
			State.ConsoleVariableName = Name;
			State.ConsoleValue = Variable->GetString();
			State.ConsoleSetBy = static_cast<EConsoleVariableFlags>(
				Variable->GetFlags() & ECVF_SetByMask);
			State.bHasConsoleVariable = true;
		}
	}
	if (UWorld* World = Client.GetWorld())
	{
		if (APlayerController* Player = World->GetFirstPlayerController())
		{
			if (APlayerCameraManager* Camera = Player->PlayerCameraManager)
			{
				State.CameraManager = Camera;
				State.CameraLocation = Camera->GetCameraLocation();
				State.CameraRotation = Camera->GetCameraRotation();
				State.CameraFov = Camera->GetFOVAngle();
				State.bHasCamera = true;
			}
		}
	}
	return State;
}

bool RestorePIEState(UGameViewportClient& Client, const FPIEState& State)
{
	Client.ViewModeIndex = State.ViewMode;
	Client.EngineShowFlags = State.ShowFlags;
	if (State.bHasConsoleVariable)
	{
		if (IConsoleVariable* Variable =
				IConsoleManager::Get().FindConsoleVariable(
					*State.ConsoleVariableName))
		{
			Variable->SetWithCurrentPriority(*State.ConsoleValue);
		}
	}
	if (Client.Viewport)
	{
		Client.Viewport->Invalidate();
	}
	bool bCvarRestored = !State.bHasConsoleVariable;
	if (State.bHasConsoleVariable)
	{
		const IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(
				*State.ConsoleVariableName);
		bCvarRestored = Variable
			&& Variable->GetString() == State.ConsoleValue
			&& static_cast<EConsoleVariableFlags>(
				Variable->GetFlags() & ECVF_SetByMask)
				== State.ConsoleSetBy;
	}
	bool bCameraMatches = !State.bHasCamera;
	if (State.bHasCamera)
	{
		if (const APlayerCameraManager* Camera = State.CameraManager.Get())
		{
			bCameraMatches = Camera->GetCameraLocation().Equals(
					State.CameraLocation)
				&& Camera->GetCameraRotation().Equals(State.CameraRotation)
				&& FMath::IsNearlyEqual(
					Camera->GetFOVAngle(),
					State.CameraFov);
		}
	}
	return Client.ViewModeIndex == State.ViewMode
		&& Client.EngineShowFlags.ToString() == State.ShowFlags.ToString()
		&& bCvarRestored
		&& bCameraMatches;
}

const TCHAR* FamilyConsoleVariable(const FString& Family)
{
	if (Family == TEXT("buffer")) return TEXT("r.BufferVisualizationTarget");
	if (Family == TEXT("nanite")) return TEXT("r.Nanite.Visualize");
	if (Family == TEXT("lumen")) return TEXT("r.Lumen.Visualize");
	if (Family == TEXT("virtualShadowMap")) return TEXT("r.Shadow.Virtual.Visualize");
	if (Family == TEXT("gpuSkinCache")) return TEXT("r.SkinCache.Visualize");
	if (Family == TEXT("strata")) return TEXT("r.Substrate.ViewMode");
	if (Family == TEXT("groom")) return TEXT("r.HairStrands.ViewMode");
	if (Family == TEXT("rayTracingDebug")) return TEXT("r.RayTracing.DebugVisualizationMode");
	return nullptr;
}

bool SetGameVisualizationConsoleVariable(
	const FModeDescriptor& Mode,
	FString& OutError)
{
	const TCHAR* ConsoleName = FamilyConsoleVariable(Mode.Family);
	if (!ConsoleName)
	{
		return true;
	}
	IConsoleVariable* Variable =
		IConsoleManager::Get().FindConsoleVariable(ConsoleName);
	if (!Variable)
	{
		OutError = FString::Printf(
			TEXT("Required console variable %s is unavailable."),
			ConsoleName);
		return false;
	}

	const FName ModeName(*Mode.RuntimeToken);
	int32 ExpectedIntegerValue = INDEX_NONE;
	if (Mode.Family == TEXT("lumen"))
	{
		ExpectedIntegerValue =
			GetLumenVisualizationData().GetModeID(ModeName);
		Variable->SetWithCurrentPriority(ExpectedIntegerValue);
	}
	#if UEAI_HAS_STRATA_VISUALIZATION
	else if (Mode.Family == TEXT("strata"))
	{
		ExpectedIntegerValue = static_cast<int32>(
			GetStrataVisualizationData().GetViewMode(ModeName));
		Variable->SetWithCurrentPriority(ExpectedIntegerValue);
	}
	#endif
	else if (Mode.Family == TEXT("groom"))
	{
		ExpectedIntegerValue = static_cast<int32>(
			GetGroomVisualizationData().GetViewMode(ModeName));
		Variable->SetWithCurrentPriority(ExpectedIntegerValue);
	}
	else
	{
		// Buffer, Ray Tracing Debug, Nanite, VSM, and GPU Skin Cache are
		// string-addressed in UE 5.3. Their registries parse canonical names.
		Variable->SetWithCurrentPriority(*Mode.RuntimeToken);
	}
	const bool bApplied = ExpectedIntegerValue != INDEX_NONE
		? Variable->GetInt() == ExpectedIntegerValue
		: Variable->GetString().Equals(
			Mode.RuntimeToken,
			ESearchCase::IgnoreCase);
	if (!bApplied)
	{
		OutError = FString::Printf(
			TEXT("Visualization mode '%s/%s' was not accepted by %s."),
			*Mode.Family,
			*Mode.Mode,
			ConsoleName);
		return false;
	}
	return true;
}

bool ApplyMode(
	const FTargetContext& Target,
	const FModeDescriptor& Mode,
	FString& OutError)
{
	if (!Mode.bAvailable)
	{
		OutError = Mode.UnavailableReason;
		return false;
	}
	if (Target.EditorClient)
	{
		FEditorViewportClient& Client = *Target.EditorClient;
		if (Mode.Family == TEXT("viewMode"))
		{
			Client.SetViewMode(Mode.ViewMode);
		}
		else if (Mode.Family == TEXT("buffer"))
		{
			Client.ChangeBufferVisualizationMode(FName(*Mode.RuntimeToken));
		}
		else if (Mode.Family == TEXT("nanite"))
		{
			Client.ChangeNaniteVisualizationMode(FName(*Mode.RuntimeToken));
		}
		else if (Mode.Family == TEXT("lumen"))
		{
			Client.ChangeLumenVisualizationMode(FName(*Mode.RuntimeToken));
		}
		else if (Mode.Family == TEXT("virtualShadowMap"))
		{
			Client.ChangeVirtualShadowMapVisualizationMode(FName(*Mode.RuntimeToken));
		}
		else if (Mode.Family == TEXT("gpuSkinCache"))
		{
			Client.ChangeGPUSkinCacheVisualizationMode(FName(*Mode.RuntimeToken));
		}
		#if UEAI_HAS_STRATA_VISUALIZATION
		else if (Mode.Family == TEXT("strata"))
		{
			Client.ChangeStrataVisualizationMode(FName(*Mode.RuntimeToken));
		}
		#endif
		else if (Mode.Family == TEXT("groom"))
		{
			Client.ChangeGroomVisualizationMode(FName(*Mode.RuntimeToken));
		}
		else if (Mode.Family == TEXT("rayTracingDebug"))
		{
			Client.ChangeRayTracingDebugVisualizationMode(
				FName(*Mode.RuntimeToken));
		}
		else
		{
			OutError = TEXT("Unsupported visualization family.");
			return false;
		}
		Client.Invalidate();
		return true;
	}
	if (!Target.GameClient)
	{
		OutError = TEXT("The target has no compatible viewport client.");
		return false;
	}
	UGameViewportClient& Client = *Target.GameClient;
	if (!SetGameVisualizationConsoleVariable(Mode, OutError))
	{
		return false;
	}
	Client.ViewModeIndex = Mode.ViewMode;
	ApplyViewMode(Mode.ViewMode, true, Client.EngineShowFlags);
	if (Client.Viewport)
	{
		Client.Viewport->Invalidate();
	}
	return true;
}

bool VerifyAppliedMode(
	const FTargetContext& Target,
	const FModeDescriptor& Mode,
	FString& OutError)
{
	const FName ExpectedToken(*Mode.RuntimeToken);
	if (Target.EditorClient)
	{
		const FEditorViewportClient& Client = *Target.EditorClient;
		bool bMatches = false;
		if (Mode.Family == TEXT("viewMode"))
		{
			bMatches = Client.GetViewMode() == Mode.ViewMode;
		}
		else if (Mode.Family == TEXT("buffer"))
		{
			bMatches = Client.CurrentBufferVisualizationMode == ExpectedToken;
		}
		else if (Mode.Family == TEXT("nanite"))
		{
			bMatches = Client.CurrentNaniteVisualizationMode == ExpectedToken;
		}
		else if (Mode.Family == TEXT("lumen"))
		{
			bMatches = Client.CurrentLumenVisualizationMode == ExpectedToken;
		}
		else if (Mode.Family == TEXT("virtualShadowMap"))
		{
			bMatches =
				Client.CurrentVirtualShadowMapVisualizationMode == ExpectedToken;
		}
		else if (Mode.Family == TEXT("gpuSkinCache"))
		{
			bMatches =
				Client.CurrentGPUSkinCacheVisualizationMode == ExpectedToken;
		}
		#if UEAI_HAS_STRATA_VISUALIZATION
		else if (Mode.Family == TEXT("strata"))
		{
			bMatches = Client.CurrentStrataVisualizationMode == ExpectedToken;
		}
		#endif
		else if (Mode.Family == TEXT("groom"))
		{
			bMatches = Client.CurrentGroomVisualizationMode == ExpectedToken;
		}
		else if (Mode.Family == TEXT("rayTracingDebug"))
		{
			bMatches =
				Client.CurrentRayTracingDebugVisualizationMode == ExpectedToken;
		}
		if (bMatches)
		{
			return true;
		}
	}
	else if (Target.GameClient)
	{
		if (Target.GameClient->ViewModeIndex == Mode.ViewMode)
		{
			if (const TCHAR* ConsoleName = FamilyConsoleVariable(Mode.Family))
			{
				const IConsoleVariable* Variable =
					IConsoleManager::Get().FindConsoleVariable(ConsoleName);
				int32 ExpectedIntegerValue = INDEX_NONE;
				if (Mode.Family == TEXT("lumen"))
				{
					ExpectedIntegerValue =
						GetLumenVisualizationData().GetModeID(ExpectedToken);
				}
				#if UEAI_HAS_STRATA_VISUALIZATION
				else if (Mode.Family == TEXT("strata"))
				{
					ExpectedIntegerValue = static_cast<int32>(
						GetStrataVisualizationData().GetViewMode(ExpectedToken));
				}
				#endif
				else if (Mode.Family == TEXT("groom"))
				{
					ExpectedIntegerValue = static_cast<int32>(
						GetGroomVisualizationData().GetViewMode(ExpectedToken));
				}
				const bool bConsoleMatches = Variable
					&& (ExpectedIntegerValue != INDEX_NONE
						? Variable->GetInt() == ExpectedIntegerValue
						: Variable->GetString().Equals(
							Mode.RuntimeToken,
							ESearchCase::IgnoreCase));
				if (bConsoleMatches)
				{
					return true;
				}
			}
			else
			{
				return true;
			}
		}
	}
	OutError = FString::Printf(
		TEXT("Visualization mode '%s/%s' was not observable on the resolved target after apply."),
		*Mode.Family,
		*Mode.Mode);
	return false;
}

#undef UEAI_HAS_STRATA_VISUALIZATION

bool ResolveSameTarget(
	const TSharedPtr<FJsonObject>& Params,
	Infrastructure::FPIESessionController& Controller,
	const FTargetContext& Original,
	const FIntPoint& OriginalSourceSize,
	FTargetContext& OutTarget,
	FString& OutError,
	FString& OutErrorCode)
{
	if (!ResolveTarget(
			Params,
			Controller,
			OutTarget,
			OutError,
			OutErrorCode))
	{
		return false;
	}
	const bool bSameIdentity =
		OutTarget.Kind == Original.Kind
		&& OutTarget.Viewport == Original.Viewport
		&& OutTarget.EditorClient == Original.EditorClient
		&& OutTarget.GameClient == Original.GameClient
		&& OutTarget.World == Original.World
		&& OutTarget.SessionId == Original.SessionId
		&& OutTarget.Generation == Original.Generation;
	if (!bSameIdentity)
	{
		OutError =
			TEXT("The resolved viewport identity changed while capture was pending.");
		OutErrorCode = TEXT("viewport_target_changed");
		return false;
	}
	if (!OutTarget.Viewport
		|| OutTarget.Viewport->GetSizeXY() != OriginalSourceSize)
	{
		OutError = TEXT("The target viewport source dimensions changed during capture.");
		OutErrorCode = TEXT("viewport_target_changed");
		return false;
	}
	return true;
}

bool IsOriginalEditorTargetLive(const FTargetContext& Original)
{
	if (!GEditor || !Original.EditorClient || !Original.Viewport)
	{
		return false;
	}
	for (FLevelEditorViewportClient* Candidate :
		GEditor->GetLevelViewportClients())
	{
		if (Candidate == Original.EditorClient
			&& Candidate->Viewport == Original.Viewport)
		{
			return true;
		}
	}
	return false;
}

TSharedRef<FJsonObject> CameraToJson(const FTargetContext& Target)
{
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float Fov = 0.0f;
	FString Source = TEXT("unavailable");
	if (Target.EditorClient)
	{
		Location = Target.EditorClient->GetViewLocation();
		Rotation = Target.EditorClient->GetViewRotation();
		Fov = Target.EditorClient->ViewFOV;
		Source = TEXT("editorViewportClient");
	}
	else if (Target.World)
	{
		if (APlayerController* Controller =
				Target.World->GetFirstPlayerController())
		{
			if (APlayerCameraManager* Camera = Controller->PlayerCameraManager)
			{
				Location = Camera->GetCameraLocation();
				Rotation = Camera->GetCameraRotation();
				Fov = Camera->GetFOVAngle();
				Source = TEXT("playerCameraManager");
			}
		}
	}
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("source"), Source);
	TSharedRef<FJsonObject> LocationJson = MakeShared<FJsonObject>();
	LocationJson->SetNumberField(TEXT("x"), Location.X);
	LocationJson->SetNumberField(TEXT("y"), Location.Y);
	LocationJson->SetNumberField(TEXT("z"), Location.Z);
	Json->SetObjectField(TEXT("location"), LocationJson);
	TSharedRef<FJsonObject> RotationJson = MakeShared<FJsonObject>();
	RotationJson->SetNumberField(TEXT("pitch"), Rotation.Pitch);
	RotationJson->SetNumberField(TEXT("yaw"), Rotation.Yaw);
	RotationJson->SetNumberField(TEXT("roll"), Rotation.Roll);
	Json->SetObjectField(TEXT("rotation"), RotationJson);
	Json->SetNumberField(TEXT("fovDegrees"), Fov);
	if (Target.EditorClient)
	{
		Json->SetNumberField(
			TEXT("viewportType"),
			static_cast<int32>(Target.EditorClient->GetViewportType()));
		Json->SetNumberField(
			TEXT("orthoZoom"),
			Target.EditorClient->GetOrthoZoom());
	}
	return Json;
}

TSharedRef<FJsonObject> BuildFingerprint(
	const FTargetContext& Target,
	const FModeDescriptor& Mode,
	const FIntPoint& SourceSize,
	const int32 Width,
	const int32 Height,
	const FString& ShowFlags)
{
	TSharedRef<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(TEXT("targetKind"), Target.Kind);
	Fingerprint->SetStringField(TEXT("family"), Mode.Family);
	Fingerprint->SetStringField(TEXT("mode"), Mode.Mode);
	Fingerprint->SetStringField(TEXT("runtimeModeToken"), Mode.RuntimeToken);
	Fingerprint->SetNumberField(TEXT("width"), Width);
	Fingerprint->SetNumberField(TEXT("height"), Height);
	Fingerprint->SetNumberField(TEXT("sourceWidth"), SourceSize.X);
	Fingerprint->SetNumberField(TEXT("sourceHeight"), SourceSize.Y);
	Fingerprint->SetNumberField(
		TEXT("sourceAspectRatio"),
		SourceSize.Y > 0
			? static_cast<double>(SourceSize.X) / SourceSize.Y
			: 0.0);
	Fingerprint->SetNumberField(
		TEXT("outputAspectRatio"),
		Height > 0 ? static_cast<double>(Width) / Height : 0.0);
	Fingerprint->SetBoolField(
		TEXT("resampled"),
		SourceSize.X != Width || SourceSize.Y != Height);
	Fingerprint->SetStringField(
		TEXT("resampleFilter"),
		SourceSize.X == Width && SourceSize.Y == Height
			? TEXT("none")
			: TEXT("nearest"));
	Fingerprint->SetStringField(
		TEXT("worldPath"),
		Target.World ? Target.World->GetPathName() : FString());
	Fingerprint->SetStringField(
		TEXT("worldPackage"),
		Target.World && Target.World->GetOutermost()
			? Target.World->GetOutermost()->GetName()
			: FString());
	Fingerprint->SetStringField(
		TEXT("map"),
		Target.World ? Target.World->GetMapName() : FString());
	Fingerprint->SetNumberField(
		TEXT("worldType"),
		Target.World ? static_cast<int32>(Target.World->WorldType) : -1);
	Fingerprint->SetStringField(TEXT("sessionId"), Target.SessionId);
	Fingerprint->SetNumberField(
		TEXT("generation"),
		static_cast<double>(Target.Generation));
	Fingerprint->SetStringField(
		TEXT("rhi"),
		GDynamicRHI ? GDynamicRHI->GetName() : TEXT("unavailable"));
	Fingerprint->SetStringField(TEXT("gpuAdapter"), GRHIAdapterName);
	Fingerprint->SetStringField(
		TEXT("featureLevel"),
		LexToString(
			Target.World
				? Target.World->GetFeatureLevel()
				: GMaxRHIFeatureLevel));
	Fingerprint->SetStringField(
		TEXT("engineVersion"),
		FEngineVersion::Current().ToString(EVersionComponent::Patch));
	Fingerprint->SetStringField(
		TEXT("pluginVersion"),
		UTF8_TO_TCHAR(UE_AI_INTEGRATION_VERSION));
	Fingerprint->SetStringField(TEXT("showFlags"), ShowFlags);
	Fingerprint->SetStringField(
		TEXT("exposure"),
		Target.EditorClient
			? Target.EditorClient->ExposureSettings.ToString()
			: TEXT("gameViewport"));
	float DpiScale = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().GetApplicationScale()
		: 1.0f;
	if (Target.EditorClient)
	{
		if (const TSharedPtr<SEditorViewport> Widget =
				Target.EditorClient->GetEditorViewportWidget())
		{
			DpiScale = Widget->GetCachedGeometry()
				.GetAccumulatedLayoutTransform().GetScale();
		}
	}
	else if (Target.GameClient)
	{
		if (const TSharedPtr<SViewport> Widget =
				Target.GameClient->GetGameViewportWidget())
		{
			DpiScale = Widget->GetCachedGeometry()
				.GetAccumulatedLayoutTransform().GetScale();
		}
	}
	Fingerprint->SetNumberField(TEXT("dpiScale"), DpiScale);
	Fingerprint->SetNumberField(
		TEXT("displayGamma"),
		Target.Viewport ? Target.Viewport->GetDisplayGamma() : 0.0f);
	const IConsoleVariable* ScreenPercentage =
		IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	Fingerprint->SetStringField(
		TEXT("screenPercentage"),
		ScreenPercentage ? ScreenPercentage->GetString() : TEXT("unavailable"));
	Fingerprint->SetStringField(
		TEXT("theme"),
		FAppStyle::GetAppStyleSetName().ToString());

	TArray<FString> ActorSelectionPaths;
	TArray<FString> ComponentSelectionPaths;
	if (Target.Kind == TEXT("editor"))
	{
		TArray<TWeakObjectPtr<AActor>> SelectedActors;
		TArray<TWeakObjectPtr<UActorComponent>> SelectedComponents;
		CaptureSelectedActors(SelectedActors);
		CaptureSelectedComponents(SelectedComponents);
		for (const TWeakObjectPtr<AActor>& Actor : SelectedActors)
		{
			if (Actor.IsValid())
			{
				ActorSelectionPaths.Add(Actor->GetPathName());
			}
		}
		for (const TWeakObjectPtr<UActorComponent>& Component :
			SelectedComponents)
		{
			if (Component.IsValid())
			{
				ComponentSelectionPaths.Add(Component->GetPathName());
			}
		}
	}
	ActorSelectionPaths.Sort();
	ComponentSelectionPaths.Sort();
	TArray<FString> SelectionPaths;
	SelectionPaths.Reserve(
		ActorSelectionPaths.Num() + ComponentSelectionPaths.Num());
	for (const FString& Path : ActorSelectionPaths)
	{
		SelectionPaths.Add(TEXT("actor:") + Path);
	}
	for (const FString& Path : ComponentSelectionPaths)
	{
		SelectionPaths.Add(TEXT("component:") + Path);
	}
	const FString SelectionCanonical = FString::Join(SelectionPaths, TEXT("\n"));
	FTCHARToUTF8 SelectionUtf8(*SelectionCanonical);
	FString SelectionHash;
	Infrastructure::TrySha256Hex(
		SelectionUtf8.Get(),
		static_cast<uint64>(SelectionUtf8.Length()),
		SelectionHash);
	Fingerprint->SetStringField(TEXT("selectionPolicy"), TEXT("preserved"));
	Fingerprint->SetNumberField(TEXT("selectionCount"), SelectionPaths.Num());
	Fingerprint->SetNumberField(
		TEXT("selectionActorCount"),
		ActorSelectionPaths.Num());
	Fingerprint->SetNumberField(
		TEXT("selectionComponentCount"),
		ComponentSelectionPaths.Num());
	Fingerprint->SetStringField(TEXT("selectionSha256"), SelectionHash);

	static const TCHAR* const CriticalCVars[] = {
		TEXT("r.ScreenPercentage"),
		TEXT("r.ViewDistanceScale"),
		TEXT("r.PostProcessAAQuality"),
		TEXT("r.TemporalAA.Upsampling"),
		TEXT("r.DynamicRes.OperationMode"),
		TEXT("r.VSync"),
		TEXT("t.MaxFPS")
	};
	TSharedRef<FJsonObject> CVarValues = MakeShared<FJsonObject>();
	for (const TCHAR* Name : CriticalCVars)
	{
		const IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(Name);
		CVarValues->SetStringField(
			Name,
			Variable ? Variable->GetString() : TEXT("unavailable"));
	}
	if (const TCHAR* ModeCVar = FamilyConsoleVariable(Mode.Family))
	{
		const IConsoleVariable* Variable =
			IConsoleManager::Get().FindConsoleVariable(ModeCVar);
		CVarValues->SetStringField(
			ModeCVar,
			Variable ? Variable->GetString() : TEXT("unavailable"));
	}
	Fingerprint->SetObjectField(TEXT("criticalCVars"), CVarValues);
	Fingerprint->SetObjectField(TEXT("camera"), CameraToJson(Target));
	return Fingerprint;
}

TSharedRef<FJsonObject> BuildImageResult(
	const TSharedPtr<FJsonObject>& Metadata,
	const TArray<uint8>& Png)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
		Metadata->Values)
	{
		Result->SetField(Pair.Key, Pair.Value);
	}
	Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
	if (Png.Num() <= MaxInlinePngBytes)
	{
		Result->SetStringField(TEXT("image_base64"), FBase64::Encode(Png));
		Result->SetNumberField(TEXT("base64Length"),
			Result->GetStringField(TEXT("image_base64")).Len());
		Result->SetBoolField(TEXT("inlineImageOmitted"), false);
	}
	else
	{
		Result->SetBoolField(TEXT("inlineImageOmitted"), true);
		Result->SetStringField(
			TEXT("inlineImageOmissionReason"),
			TEXT("PNG exceeds the bounded 8 MiB inline response limit; use artifactPath."));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> FingerprintMismatchReasons(
	const TSharedPtr<FJsonObject>& Before,
	const TSharedPtr<FJsonObject>& After)
{
	TArray<TSharedPtr<FJsonValue>> Reasons;
	static const TCHAR* const Fields[] = {
		TEXT("targetKind"), TEXT("family"), TEXT("mode"),
		TEXT("runtimeModeToken"), TEXT("width"), TEXT("height"),
		TEXT("sourceWidth"), TEXT("sourceHeight"),
		TEXT("sourceAspectRatio"), TEXT("outputAspectRatio"),
		TEXT("resampled"), TEXT("resampleFilter"),
		TEXT("worldPath"), TEXT("worldPackage"), TEXT("map"),
		TEXT("worldType"), TEXT("sessionId"), TEXT("generation"),
		TEXT("rhi"), TEXT("gpuAdapter"),
		TEXT("featureLevel"), TEXT("engineVersion"), TEXT("pluginVersion"),
		TEXT("showFlags"), TEXT("exposure"), TEXT("dpiScale"),
		TEXT("displayGamma"), TEXT("screenPercentage"), TEXT("theme"),
		TEXT("selectionPolicy"), TEXT("selectionCount"),
		TEXT("selectionActorCount"), TEXT("selectionComponentCount"),
		TEXT("selectionSha256")
	};
	for (const TCHAR* Field : Fields)
	{
		auto StableValue = [Field](const TSharedPtr<FJsonObject>& Object)
		{
			if (!Object.IsValid())
			{
				return FString();
			}
			const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Field);
			if (!Value || !Value->IsValid())
			{
				return FString();
			}
			switch ((*Value)->Type)
			{
			case EJson::Number:
				return FString::SanitizeFloat((*Value)->AsNumber(), 0);
			case EJson::Boolean:
				return FString(
					(*Value)->AsBool() ? TEXT("true") : TEXT("false"));
			case EJson::String:
				return (*Value)->AsString();
			default:
				return FString();
			}
		};
		const FString BeforeValue = StableValue(Before);
		const FString AfterValue = StableValue(After);
		if (BeforeValue != AfterValue)
		{
			Reasons.Add(MakeShared<FJsonValueString>(
				FString(TEXT("renderFingerprint.")) + Field));
		}
	}
	FString BeforeCamera;
	FString AfterCamera;
	if (Before.IsValid()
		&& Before->HasTypedField<EJson::Object>(TEXT("camera")))
	{
		SerializeJson(Before->GetObjectField(TEXT("camera")), BeforeCamera);
	}
	if (After.IsValid()
		&& After->HasTypedField<EJson::Object>(TEXT("camera")))
	{
		SerializeJson(After->GetObjectField(TEXT("camera")), AfterCamera);
	}
	if (BeforeCamera != AfterCamera)
	{
		Reasons.Add(MakeShared<FJsonValueString>(
			TEXT("renderFingerprint.camera")));
	}
	FString BeforeCVars;
	FString AfterCVars;
	if (Before.IsValid()
		&& Before->HasTypedField<EJson::Object>(TEXT("criticalCVars")))
	{
		SerializeJson(Before->GetObjectField(TEXT("criticalCVars")), BeforeCVars);
	}
	if (After.IsValid()
		&& After->HasTypedField<EJson::Object>(TEXT("criticalCVars")))
	{
		SerializeJson(After->GetObjectField(TEXT("criticalCVars")), AfterCVars);
	}
	if (BeforeCVars != AfterCVars)
	{
		Reasons.Add(MakeShared<FJsonValueString>(
			TEXT("renderFingerprint.criticalCVars")));
	}
	return Reasons;
}

TSharedRef<FJsonObject> BoundsJson(
	const int32 MinX,
	const int32 MinY,
	const int32 MaxX,
	const int32 MaxY)
{
	TSharedRef<FJsonObject> Bounds = MakeShared<FJsonObject>();
	Bounds->SetNumberField(TEXT("x"), MinX);
	Bounds->SetNumberField(TEXT("y"), MinY);
	Bounds->SetNumberField(TEXT("width"), MaxX - MinX + 1);
	Bounds->SetNumberField(TEXT("height"), MaxY - MinY + 1);
	return Bounds;
}

class FTool_ListViewportVisualizations final : public FMCPToolBase
{
public:
	explicit FTool_ListViewportVisualizations(
		Infrastructure::FPIESessionController& InController)
		: Controller(InController)
	{
	}

	FString GetCapabilityId() const override
	{
		return TEXT("scene.viewport.visualization.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FTargetContext Target;
		FString Error;
		FString ErrorCode;
		FString RequestedKind = TEXT("editor");
		const TSharedPtr<FJsonObject>* TargetPointer = nullptr;
		if (Params.IsValid()
			&& Params->TryGetObjectField(TEXT("target"), TargetPointer)
			&& TargetPointer && TargetPointer->IsValid())
		{
			(*TargetPointer)->TryGetStringField(TEXT("kind"), RequestedKind);
		}
		bool bTargetAvailable = false;
		if (RequestedKind.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
		{
			Target.Kind = TEXT("editor");
			bTargetAvailable = ResolveEditorTarget(Target, Error);
		}
		else if (RequestedKind.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
		{
			if (!ResolveTarget(Params, Controller, Target, Error, ErrorCode))
			{
				return FMCPToolResult::Error(
					Error,
					ErrorCode,
					ErrorCode == TEXT("invalid_request") ? 400 : 409);
			}
			bTargetAvailable = true;
		}
		else
		{
			return FMCPToolResult::Error(
				TEXT("target.kind must be editor or pie."),
				TEXT("invalid_request"),
				400);
		}
		FString FamilyFilter;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("family"), FamilyFilter);
		}
		const TArray<FModeDescriptor> Modes = BuildModeCatalog(
			bTargetAvailable ? &Target : nullptr);
		TArray<TSharedPtr<FJsonValue>> ModeValues;
		TMap<FString, int32> FamilyCounts;
		TMap<FString, int32> AvailableFamilyCounts;
		for (const FModeDescriptor& Mode : Modes)
		{
			if (!FamilyFilter.IsEmpty()
				&& !Mode.Family.Equals(
					FamilyFilter,
					ESearchCase::IgnoreCase))
			{
				continue;
			}
			FModeDescriptor EffectiveMode = Mode;
			if (!bTargetAvailable)
			{
				EffectiveMode.bAvailable = false;
				EffectiveMode.UnavailableReason = Error;
			}
			ModeValues.Add(
				MakeShared<FJsonValueObject>(ModeToJson(EffectiveMode)));
			++FamilyCounts.FindOrAdd(Mode.Family);
			if (EffectiveMode.bAvailable)
			{
				++AvailableFamilyCounts.FindOrAdd(Mode.Family);
			}
		}
		TArray<FString> Families;
		FamilyCounts.GetKeys(Families);
		Families.Sort();
		TArray<TSharedPtr<FJsonValue>> FamilyValues;
		for (const FString& Family : Families)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("family"), Family);
			Item->SetNumberField(TEXT("modeCount"), FamilyCounts[Family]);
			Item->SetNumberField(
				TEXT("availableModeCount"),
				AvailableFamilyCounts.FindRef(Family));
			Item->SetBoolField(
				TEXT("available"),
				AvailableFamilyCounts.FindRef(Family) > 0);
			FamilyValues.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.viewport-visualization-catalog.v1"));
		Result->SetStringField(TEXT("targetKind"), Target.Kind);
		Result->SetBoolField(TEXT("targetAvailable"), bTargetAvailable);
		if (!bTargetAvailable)
		{
			Result->SetStringField(TEXT("targetUnavailableReason"), Error);
		}
		if (Target.Kind == TEXT("pie"))
		{
			Result->SetStringField(TEXT("sessionId"), Target.SessionId);
			Result->SetNumberField(
				TEXT("generation"),
				static_cast<double>(Target.Generation));
		}
		Result->SetArrayField(TEXT("families"), FamilyValues);
		Result->SetArrayField(TEXT("modes"), ModeValues);
		Result->SetNumberField(TEXT("modeCount"), ModeValues.Num());
		Result->SetBoolField(
			TEXT("renderingAvailable"),
			GDynamicRHI != nullptr && !GUsingNullRHI && FApp::CanEverRender());
		Result->SetStringField(
			TEXT("evidenceBoundary"),
			TEXT("Availability is derived from the active Engine, RHI, CVars, registered visualization data, and target viewport; it does not prove that a mode contains useful scene content."));
		return FMCPToolResult::Ok(Result);
	}

private:
	Infrastructure::FPIESessionController& Controller;
};

class FTool_CaptureViewportVisualization final : public FMCPToolBase
{
private:
	struct FActiveCapture;

public:
	explicit FTool_CaptureViewportVisualization(
		Infrastructure::FPIESessionController& InController)
		: Controller(InController)
	{
	}

	~FTool_CaptureViewportVisualization() override
	{
		CancelAsyncExecution(
			TEXT("The viewport visualization handler is being destroyed."));
	}

	FString GetCapabilityId() const override
	{
		return TEXT("scene.viewport.visualization.capture");
	}

	bool SupportsAsyncExecution() const override { return true; }

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(
			TEXT("Viewport capture must use the queued async execution boundary so real Editor and Slate frames can settle."),
			TEXT("async_execution_required"),
			409);
	}

	bool BeginExecuteAsync(
		const TSharedPtr<FJsonObject>& Params,
		FMCPToolAsyncCompletion Completion) override
	{
		check(IsInGameThread());
		if (!Completion)
		{
			return false;
		}
		if (ActiveCapture.IsValid())
		{
			Completion(FMCPToolResult::Error(
				TEXT("Another viewport visualization capture is already settling."),
				TEXT("viewport_capture_busy"),
				409));
			return true;
		}
		if (!Params.IsValid())
		{
			Completion(InvalidRequest(TEXT("A capture request is required.")));
			return true;
		}
		const TSharedPtr<FJsonObject>* VisualizationPointer = nullptr;
		FString Family;
		FString ModeName;
		if (!Params->TryGetObjectField(
				TEXT("visualization"),
				VisualizationPointer)
			|| !VisualizationPointer
			|| !(*VisualizationPointer)->TryGetStringField(
				TEXT("family"), Family)
			|| !(*VisualizationPointer)->TryGetStringField(
				TEXT("mode"), ModeName)
			|| Family.IsEmpty()
			|| ModeName.IsEmpty())
		{
			Completion(InvalidRequest(
				TEXT("visualization.family and visualization.mode are required.")));
			return true;
		}
		FTargetContext Target;
		FString Error;
		FString ErrorCode;
		if (!ResolveTarget(Params, Controller, Target, Error, ErrorCode))
		{
			Completion(FMCPToolResult::Error(
				Error,
				ErrorCode,
				ErrorCode == TEXT("invalid_request") ? 400 : 409));
			return true;
		}
		const TArray<FModeDescriptor> Modes = BuildModeCatalog(&Target);
		const FModeDescriptor* Mode = FindMode(Modes, Family, ModeName);
		if (!Mode)
		{
			Completion(InvalidRequest(
				TEXT("The requested visualization family/mode is not registered for this Engine version.")));
			return true;
		}
		if (!Mode->bAvailable)
		{
			Completion(Unavailable(Mode->UnavailableReason));
			return true;
		}
		const FIntPoint SourceSize = Target.Viewport->GetSizeXY();
		if (SourceSize.X <= 0 || SourceSize.Y <= 0)
		{
			Completion(Unavailable(TEXT("The target viewport has invalid dimensions.")));
			return true;
		}
		double WidthNumber = 0.0;
		double HeightNumber = 0.0;
		const bool bHasWidth = Params->TryGetNumberField(TEXT("width"), WidthNumber);
		const bool bHasHeight = Params->TryGetNumberField(TEXT("height"), HeightNumber);
		if (bHasWidth != bHasHeight
			|| (bHasWidth
				&& (!FMath::IsFinite(WidthNumber)
					|| !FMath::IsFinite(HeightNumber)
					|| WidthNumber < 64.0
					|| WidthNumber > MaxCaptureDimension
					|| HeightNumber < 64.0
					|| HeightNumber > MaxCaptureDimension
					|| FMath::FloorToDouble(WidthNumber) != WidthNumber
					|| FMath::FloorToDouble(HeightNumber) != HeightNumber)))
		{
			Completion(InvalidRequest(
				TEXT("width and height must be provided together as integers.")));
			return true;
		}
		const int32 OutputWidth = bHasWidth
			? static_cast<int32>(WidthNumber)
			: SourceSize.X;
		const int32 OutputHeight = bHasHeight
			? static_cast<int32>(HeightNumber)
			: SourceSize.Y;
		if (OutputWidth < 64 || OutputHeight < 64
			|| OutputWidth > MaxCaptureDimension
			|| OutputHeight > MaxCaptureDimension)
		{
			Completion(InvalidRequest(
				TEXT("Capture dimensions must be between 64 and 4096 pixels.")));
			return true;
		}

		TSharedRef<FActiveCapture> Active = MakeShared<FActiveCapture>();
		Active->Params = MakeShared<FJsonObject>(*Params);
		Active->Mode = *Mode;
		Active->OriginalTarget = Target;
		Active->SourceSize = SourceSize;
		Active->OutputWidth = OutputWidth;
		Active->OutputHeight = OutputHeight;
		Active->Completion = MoveTemp(Completion);
		Active->DeadlineSeconds =
			FPlatformTime::Seconds() + RenderSettlementTimeoutSeconds;
		Active->StartFrameCounter = GFrameCounter;
		Active->LastEngineFrameCounter = GFrameCounter;
		Active->LastSlateFrameCounter = GFrameCounter;
		Active->GameClient = Target.GameClient;
		Active->World = Target.World;
		#if WITH_DEV_AUTOMATION_TESTS
		Params->TryGetStringField(TEXT("_testFault"), Active->TestFault);
		#endif
		if (Target.EditorClient)
		{
			Active->EditorState = CaptureEditorState(*Target.EditorClient);
		}
		else
		{
			Active->PIEState = CapturePIEState(*Target.GameClient, *Mode);
		}
		ActiveCapture = Active;

		if (!ApplyMode(Target, Active->Mode, Error)
			|| !VerifyAppliedMode(Target, Active->Mode, Error))
		{
			FinishFailure(
				Error,
				TEXT("viewport_visualization_unavailable"),
				409,
				false);
			return true;
		}
		#if WITH_DEV_AUTOMATION_TESTS
		if (Active->TestFault == TEXT("afterApply")
			|| Active->TestFault == TEXT("afterApplyRestoreFailure"))
		{
			if (Target.EditorClient)
			{
				// Exercise restoration of both Level Viewport selection sets on
				// injected failure without changing BSP or asset selection.
				const TArray<TWeakObjectPtr<AActor>> NoActors;
				const TArray<TWeakObjectPtr<UActorComponent>> NoComponents;
				RestoreSelection(NoActors, NoComponents);
			}
			FinishFailure(
				TEXT("Injected failure after visualization apply."),
				TEXT("viewport_capture_test_fault"),
				500,
				false);
			return true;
		}
		#endif
		if (!FSlateApplication::IsInitialized())
		{
			FinishFailure(
				TEXT("Slate is unavailable; two UI/render frames cannot be observed."),
				TEXT("viewport_capture_unavailable"),
				409,
				false);
			return true;
		}
		if (Target.EditorClient)
		{
			if (const TSharedPtr<SEditorViewport> Widget =
					Target.EditorClient->GetEditorViewportWidget())
			{
				Widget->SlatePrepass(
					FSlateApplication::Get().GetApplicationScale());
			}
		}
		else if (const TSharedPtr<SViewport> Widget =
				Target.GameClient->GetGameViewportWidget())
		{
			Widget->SlatePrepass(
				FSlateApplication::Get().GetApplicationScale());
		}
		Target.Viewport->Invalidate();
		Active->SlateTickHandle = FSlateApplication::Get()
			.OnPostTick()
			.AddRaw(this, &FTool_CaptureViewportVisualization::OnSlatePostTick);
		Active->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			TEXT("UEAI.ViewportVisualizationCapture"),
			0.0f,
			[this](float DeltaTime)
			{
				return TickCapture(DeltaTime);
			});
		return true;
	}

	void CancelAsyncExecution(const FString& Reason) override
	{
		check(IsInGameThread());
		if (ActiveCapture.IsValid())
		{
			FinishFailure(
				Reason.IsEmpty()
					? TEXT("Viewport capture was cancelled.")
					: Reason,
				TEXT("request_cancelled"),
				499,
				false);
		}
	}

private:
	struct FActiveCapture
	{
		TSharedPtr<FJsonObject> Params;
		FModeDescriptor Mode;
		FTargetContext OriginalTarget;
		FIntPoint SourceSize = FIntPoint::ZeroValue;
		int32 OutputWidth = 0;
		int32 OutputHeight = 0;
		TOptional<FEditorState> EditorState;
		TOptional<FPIEState> PIEState;
		TWeakObjectPtr<UGameViewportClient> GameClient;
		TWeakObjectPtr<UWorld> World;
		FMCPToolAsyncCompletion Completion;
		FTSTicker::FDelegateHandle TickerHandle;
		FDelegateHandle SlateTickHandle;
		TUniquePtr<FRenderCommandFence> FinalFence;
		double DeadlineSeconds = 0.0;
		uint64 StartFrameCounter = 0;
		uint64 LastEngineFrameCounter = 0;
		uint64 LastSlateFrameCounter = 0;
		uint64 FirstSettledFrameCounter = 0;
		uint64 LastSettledFrameCounter = 0;
		int32 EngineFrameCount = 0;
		int32 SlateFrameCount = 0;
		int32 TargetRevalidationCount = 0;
		FString DeferredError;
		FString DeferredErrorCode;
		#if WITH_DEV_AUTOMATION_TESTS
		FString TestFault;
		#endif
	};

	void OnSlatePostTick(float DeltaTime)
	{
		check(IsInGameThread());
		if (!ActiveCapture.IsValid() || !ActiveCapture->DeferredError.IsEmpty())
		{
			return;
		}
		FTargetContext Current;
		FString Error;
		FString ErrorCode;
		if (!ResolveSameTarget(
				ActiveCapture->Params,
				Controller,
				ActiveCapture->OriginalTarget,
				ActiveCapture->SourceSize,
				Current,
				Error,
				ErrorCode))
		{
			ActiveCapture->DeferredError = Error;
			ActiveCapture->DeferredErrorCode = ErrorCode;
			return;
		}
		++ActiveCapture->TargetRevalidationCount;
		if (GFrameCounter > ActiveCapture->StartFrameCounter
			&& GFrameCounter != ActiveCapture->LastSlateFrameCounter)
		{
			ActiveCapture->LastSlateFrameCounter = GFrameCounter;
			++ActiveCapture->SlateFrameCount;
		}
	}

	bool TickCapture(float DeltaTime)
	{
		check(IsInGameThread());
		if (!ActiveCapture.IsValid())
		{
			return false;
		}
		if (!ActiveCapture->DeferredError.IsEmpty())
		{
			FinishFailure(
				ActiveCapture->DeferredError,
				ActiveCapture->DeferredErrorCode.IsEmpty()
					? TEXT("viewport_target_changed")
					: ActiveCapture->DeferredErrorCode,
				409,
				true);
			return false;
		}
		if (FPlatformTime::Seconds() >= ActiveCapture->DeadlineSeconds)
		{
			FinishFailure(
				TEXT("Timed out before two real Editor/Slate render frames and the final render fence completed."),
				TEXT("viewport_capture_timeout"),
				504,
				true);
			return false;
		}
		#if WITH_DEV_AUTOMATION_TESTS
		if (ActiveCapture->TestFault == TEXT("settleTimeout"))
		{
			FinishFailure(
				TEXT("Injected render settlement timeout."),
				TEXT("viewport_capture_timeout"),
				504,
				true);
			return false;
		}
		#endif

		FTargetContext Current;
		FString Error;
		FString ErrorCode;
		if (!ResolveSameTarget(
				ActiveCapture->Params,
				Controller,
				ActiveCapture->OriginalTarget,
				ActiveCapture->SourceSize,
				Current,
				Error,
				ErrorCode)
			|| !VerifyAppliedMode(Current, ActiveCapture->Mode, Error))
		{
			FinishFailure(
				Error,
				ErrorCode.IsEmpty()
					? TEXT("viewport_mode_changed")
					: ErrorCode,
				409,
				true);
			return false;
		}
		++ActiveCapture->TargetRevalidationCount;

		if (GFrameCounter != ActiveCapture->LastEngineFrameCounter)
		{
			ActiveCapture->LastEngineFrameCounter = GFrameCounter;
			if (GFrameCounter > ActiveCapture->StartFrameCounter)
			{
				if (ActiveCapture->EngineFrameCount == 0)
				{
					ActiveCapture->FirstSettledFrameCounter = GFrameCounter;
				}
				ActiveCapture->LastSettledFrameCounter = GFrameCounter;
				++ActiveCapture->EngineFrameCount;
			}
		}
		Current.Viewport->Invalidate();
		if (ActiveCapture->EngineFrameCount < 2
			|| ActiveCapture->SlateFrameCount < 2)
		{
			return true;
		}
		if (!ActiveCapture->FinalFence.IsValid())
		{
			ActiveCapture->FinalFence = MakeUnique<FRenderCommandFence>();
			ActiveCapture->FinalFence->BeginFence();
			return true;
		}
		if (!ActiveCapture->FinalFence->IsFenceComplete())
		{
			return true;
		}
		FinalizeCapture(Current);
		return false;
	}

	bool RestoreAndVerify(
		const TSharedRef<FActiveCapture>& Active,
		FString& OutError)
	{
		bool bRestored = false;
		if (Active->EditorState.IsSet())
		{
			if (IsOriginalEditorTargetLive(Active->OriginalTarget))
			{
				bRestored = RestoreEditorState(
					*Active->OriginalTarget.EditorClient,
					Active->EditorState.GetValue());
			}
		}
		else if (Active->PIEState.IsSet())
		{
			if (UGameViewportClient* Client = Active->GameClient.Get())
			{
				bRestored = Client->Viewport == Active->OriginalTarget.Viewport
					&& RestorePIEState(*Client, Active->PIEState.GetValue());
			}
		}
		#if WITH_DEV_AUTOMATION_TESTS
		if (bRestored
			&& Active->TestFault == TEXT("afterApplyRestoreFailure"))
		{
			OutError =
				TEXT("Injected viewport restoration verification failure.");
			return false;
		}
		#endif
		if (!bRestored)
		{
			OutError =
				TEXT("The original viewport state could not be restored and verified.");
		}
		return bRestored;
	}

	void DetachCallbacks(
		const TSharedRef<FActiveCapture>& Active,
		const bool bFromTicker)
	{
		if (FSlateApplication::IsInitialized()
			&& Active->SlateTickHandle.IsValid())
		{
			FSlateApplication::Get().OnPostTick().Remove(
				Active->SlateTickHandle);
			Active->SlateTickHandle.Reset();
		}
		if (!bFromTicker && Active->TickerHandle.IsValid())
		{
			FTSTicker::RemoveTicker(Active->TickerHandle);
		}
		Active->TickerHandle.Reset();
	}

	void FinishFailure(
		const FString& OriginalMessage,
		const FString& OriginalCode,
		const int32 OriginalStatus,
		const bool bFromTicker)
	{
		if (!ActiveCapture.IsValid())
		{
			return;
		}
		const TSharedRef<FActiveCapture> Active = ActiveCapture.ToSharedRef();
		ActiveCapture.Reset();
		DetachCallbacks(Active, bFromTicker);
		FString RestoreError;
		const bool bRestored = RestoreAndVerify(Active, RestoreError);
		FMCPToolResult Result;
		if (bRestored)
		{
			Result = FMCPToolResult::Error(
				OriginalMessage,
				OriginalCode,
				OriginalStatus);
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("originalErrorCode"), OriginalCode);
			Details->SetBoolField(TEXT("stateRestored"), true);
			Result.Data = Details;
		}
		else
		{
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("originalErrorCode"), OriginalCode);
			Details->SetStringField(TEXT("originalErrorMessage"), OriginalMessage);
			Details->SetStringField(TEXT("restorationError"), RestoreError);
			Details->SetBoolField(TEXT("stateRestored"), false);
			Result = FMCPToolResult::Error(
				FString::Printf(
					TEXT("Capture failed (%s), and viewport restoration also failed: %s"),
					*OriginalMessage,
					*RestoreError),
				TEXT("viewport_state_restore_failed"),
				500);
			Result.Data = Details;
		}
		FMCPToolAsyncCompletion Completion = MoveTemp(Active->Completion);
		Completion(MoveTemp(Result));
	}

	void FinalizeCapture(const FTargetContext& Current)
	{
		check(ActiveCapture.IsValid());
		const TSharedRef<FActiveCapture> Active = ActiveCapture.ToSharedRef();
		#if WITH_DEV_AUTOMATION_TESTS
		if (Active->TestFault == TEXT("readPixels"))
		{
			FinishFailure(
				TEXT("Injected viewport pixel read failure."),
				TEXT("viewport_capture_unavailable"),
				500,
				true);
			return;
		}
		#endif
		TArray<FColor> SourcePixels;
		if (!Current.Viewport->ReadPixels(SourcePixels)
			|| SourcePixels.Num()
				!= Active->SourceSize.X * Active->SourceSize.Y)
		{
			FinishFailure(
				TEXT("The target viewport did not return a complete pixel buffer."),
				TEXT("viewport_capture_unavailable"),
				500,
				true);
			return;
		}
		TArray<FColor> Pixels;
		ResizePixels(
			SourcePixels,
			Active->SourceSize.X,
			Active->SourceSize.Y,
			Pixels,
			Active->OutputWidth,
			Active->OutputHeight);
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		const FString AppliedShowFlags = Current.EditorClient
			? Current.EditorClient->EngineShowFlags.ToString()
			: Current.GameClient->EngineShowFlags.ToString();
		TArray<uint8> Png;
		FString Error;
		if (!EncodePng(
				Pixels,
				Active->OutputWidth,
				Active->OutputHeight,
				Png,
				Error))
		{
			FinishFailure(
				Error,
				TEXT("viewport_capture_encode_failed"),
				500,
				true);
			return;
		}
		const TSharedRef<FJsonObject> Fingerprint = BuildFingerprint(
			Current,
			Active->Mode,
			Active->SourceSize,
			Active->OutputWidth,
			Active->OutputHeight,
			AppliedShowFlags);

		DetachCallbacks(Active, true);
		ActiveCapture.Reset();
		FString RestoreError;
		if (!RestoreAndVerify(Active, RestoreError))
		{
			FMCPToolResult Result = FMCPToolResult::Error(
				TEXT("Viewport pixels were captured, but the original viewport state could not be verified after restoration."),
				TEXT("viewport_state_restore_failed"),
				500);
			TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
			Details->SetStringField(TEXT("restorationError"), RestoreError);
			Details->SetBoolField(TEXT("stateRestored"), false);
			Result.Data = Details;
			FMCPToolAsyncCompletion Completion = MoveTemp(Active->Completion);
			Completion(MoveTemp(Result));
			return;
		}

		FString ImageHash;
		if (!Infrastructure::TrySha256Hex(Png, ImageHash))
		{
			FMCPToolAsyncCompletion Completion = MoveTemp(Active->Completion);
			Completion(FMCPToolResult::Error(
				TEXT("Could not calculate the viewport PNG checksum.")));
			return;
		}
		const FString CaptureId =
			TEXT("viewport-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(
			TEXT("schema"),
			TEXT("ue.viewport-visualization-capture.v1"));
		Metadata->SetStringField(TEXT("captureId"), CaptureId);
		Metadata->SetStringField(TEXT("format"), TEXT("png"));
		Metadata->SetStringField(TEXT("mimeType"), TEXT("image/png"));
		Metadata->SetStringField(TEXT("imageSha256"), ImageHash);
		Metadata->SetStringField(TEXT("family"), Active->Mode.Family);
		Metadata->SetStringField(TEXT("mode"), Active->Mode.Mode);
		Metadata->SetStringField(TEXT("displayName"), Active->Mode.DisplayName);
		Metadata->SetStringField(TEXT("targetKind"), Current.Kind);
		Metadata->SetStringField(
			TEXT("world"),
			Current.World ? Current.World->GetPathName() : TEXT(""));
		Metadata->SetStringField(
			TEXT("map"),
			Current.World ? Current.World->GetMapName() : TEXT(""));
		Metadata->SetNumberField(TEXT("width"), Active->OutputWidth);
		Metadata->SetNumberField(TEXT("height"), Active->OutputHeight);
		Metadata->SetNumberField(TEXT("sourceWidth"), Active->SourceSize.X);
		Metadata->SetNumberField(TEXT("sourceHeight"), Active->SourceSize.Y);
		Metadata->SetBoolField(
			TEXT("resampled"),
			Active->SourceSize.X != Active->OutputWidth
				|| Active->SourceSize.Y != Active->OutputHeight);
		Metadata->SetStringField(
			TEXT("resampleFilter"),
			Active->SourceSize.X == Active->OutputWidth
				&& Active->SourceSize.Y == Active->OutputHeight
					? TEXT("none")
					: TEXT("nearest"));
		Metadata->SetNumberField(
			TEXT("settledEngineFrameCount"),
			Active->EngineFrameCount);
		Metadata->SetNumberField(
			TEXT("slateTickFrameCount"),
			Active->SlateFrameCount);
		Metadata->SetNumberField(
			TEXT("targetRevalidationCount"),
			Active->TargetRevalidationCount);
		Metadata->SetNumberField(
			TEXT("startFrameCounter"),
			static_cast<double>(Active->StartFrameCounter));
		Metadata->SetNumberField(
			TEXT("firstSettledFrameCounter"),
			static_cast<double>(Active->FirstSettledFrameCounter));
		Metadata->SetNumberField(
			TEXT("lastSettledFrameCounter"),
			static_cast<double>(Active->LastSettledFrameCounter));
		Metadata->SetNumberField(
			TEXT("readbackFrameCounter"),
			static_cast<double>(GFrameCounter));
		Metadata->SetBoolField(TEXT("renderFenceCompleted"), true);
		Metadata->SetBoolField(
			TEXT("slatePrepassAttempted"),
			FSlateApplication::IsInitialized());
		Metadata->SetBoolField(TEXT("stateRestored"), true);
		Metadata->SetStringField(
			TEXT("capturedAtUtc"),
			FDateTime::UtcNow().ToIso8601());
		Metadata->SetStringField(TEXT("artifactPath"), ImagePath(CaptureId));
		if (Current.Kind == TEXT("pie"))
		{
			Metadata->SetStringField(TEXT("sessionId"), Current.SessionId);
			Metadata->SetNumberField(
				TEXT("generation"),
				static_cast<double>(Current.Generation));
		}
		Metadata->SetObjectField(TEXT("renderFingerprint"), Fingerprint);
		Metadata->SetStringField(
			TEXT("evidenceBoundary"),
			TEXT("This is an 8-bit lossless PNG of a named Editor debug view, not a direct RDG/GBuffer texture export."));
		if (!SaveCapture(CaptureId, Png, Metadata, Error))
		{
			FMCPToolAsyncCompletion Completion = MoveTemp(Active->Completion);
			Completion(FMCPToolResult::Error(Error));
			return;
		}
		FMCPToolAsyncCompletion Completion = MoveTemp(Active->Completion);
		Completion(FMCPToolResult::Ok(BuildImageResult(Metadata, Png)));
	}

	Infrastructure::FPIESessionController& Controller;
	TSharedPtr<FActiveCapture> ActiveCapture;
};

class FTool_CompareViewportVisualizations final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.viewport.visualization.compare");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString BeforeCaptureId;
		FString AfterCaptureId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(
				TEXT("beforeCaptureId"),
				BeforeCaptureId)
			|| !Params->TryGetStringField(
				TEXT("afterCaptureId"),
				AfterCaptureId))
		{
			return InvalidRequest(
				TEXT("beforeCaptureId and afterCaptureId are required."));
		}
		double ThresholdNumber = 8.0;
		Params->TryGetNumberField(TEXT("pixelThreshold"), ThresholdNumber);
		if (!FMath::IsFinite(ThresholdNumber)
			|| ThresholdNumber < 0.0 || ThresholdNumber > 255.0)
		{
			return InvalidRequest(TEXT("pixelThreshold must be between 0 and 255."));
		}
		const int32 Threshold = FMath::RoundToInt(ThresholdNumber);

		TSharedPtr<FJsonObject> BeforeMetadata;
		TSharedPtr<FJsonObject> AfterMetadata;
		TArray<uint8> BeforePng;
		TArray<uint8> AfterPng;
		FString Error;
		if (!LoadCapture(
				BeforeCaptureId,
				BeforeMetadata,
				BeforePng,
				Error)
			|| !LoadCapture(
				AfterCaptureId,
				AfterMetadata,
				AfterPng,
				Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("capture_not_found"),
				404);
		}
		const TSharedPtr<FJsonObject> BeforeFingerprint =
			BeforeMetadata->GetObjectField(TEXT("renderFingerprint"));
		const TSharedPtr<FJsonObject> AfterFingerprint =
			AfterMetadata->GetObjectField(TEXT("renderFingerprint"));
		TArray<TSharedPtr<FJsonValue>> Reasons =
			FingerprintMismatchReasons(BeforeFingerprint, AfterFingerprint);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.viewport-visualization-compare.v1"));
		Result->SetStringField(TEXT("beforeCaptureId"), BeforeCaptureId);
		Result->SetStringField(TEXT("afterCaptureId"), AfterCaptureId);
		Result->SetNumberField(TEXT("pixelThreshold"), Threshold);
		Result->SetArrayField(TEXT("incompatibilityReasons"), Reasons);
		Result->SetBoolField(TEXT("compatible"), Reasons.IsEmpty());
		if (!Reasons.IsEmpty())
		{
			Result->SetStringField(TEXT("comparison"), TEXT("inconclusive"));
			Result->SetStringField(
				TEXT("evidenceBoundary"),
				TEXT("Pixel metrics are omitted because the render fingerprints are incompatible."));
			return FMCPToolResult::Ok(Result);
		}

		TArray<FColor> BeforePixels;
		TArray<FColor> AfterPixels;
		int32 BeforeWidth = 0;
		int32 BeforeHeight = 0;
		int32 AfterWidth = 0;
		int32 AfterHeight = 0;
		if (!DecodePng(
				BeforePng,
				BeforePixels,
				BeforeWidth,
				BeforeHeight,
				Error)
			|| !DecodePng(
				AfterPng,
				AfterPixels,
				AfterWidth,
				AfterHeight,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}
		if (BeforeWidth != AfterWidth || BeforeHeight != AfterHeight)
		{
			return FMCPToolResult::Error(
				TEXT("Compatible fingerprints decoded to different dimensions."),
				TEXT("capture_corrupt"),
				409);
		}

		TArray<FColor> DiffPixels;
		DiffPixels.SetNumUninitialized(BeforePixels.Num());
		int64 ChangedPixelCount = 0;
		double TotalDifference = 0.0;
		int32 MinX = BeforeWidth;
		int32 MinY = BeforeHeight;
		int32 MaxX = -1;
		int32 MaxY = -1;
		struct FTileBounds
		{
			int32 MinX = MAX_int32;
			int32 MinY = MAX_int32;
			int32 MaxX = -1;
			int32 MaxY = -1;
			int32 Count = 0;
		};
		TMap<int32, FTileBounds> ChangedTiles;
		const int32 TileSize = 64;
		const int32 TilesX = FMath::DivideAndRoundUp(BeforeWidth, TileSize);
		for (int32 Index = 0; Index < BeforePixels.Num(); ++Index)
		{
			const FColor& Before = BeforePixels[Index];
			const FColor& After = AfterPixels[Index];
			const int32 DeltaR = FMath::Abs(
				static_cast<int32>(Before.R) - After.R);
			const int32 DeltaG = FMath::Abs(
				static_cast<int32>(Before.G) - After.G);
			const int32 DeltaB = FMath::Abs(
				static_cast<int32>(Before.B) - After.B);
			const int32 MaxDelta = FMath::Max3(DeltaR, DeltaG, DeltaB);
			TotalDifference += (DeltaR + DeltaG + DeltaB) / (3.0 * 255.0);
			if (MaxDelta > Threshold)
			{
				++ChangedPixelCount;
				const int32 X = Index % BeforeWidth;
				const int32 Y = Index / BeforeWidth;
				MinX = FMath::Min(MinX, X);
				MinY = FMath::Min(MinY, Y);
				MaxX = FMath::Max(MaxX, X);
				MaxY = FMath::Max(MaxY, Y);
				FTileBounds& Tile = ChangedTiles.FindOrAdd(
					(Y / TileSize) * TilesX + X / TileSize);
				Tile.MinX = FMath::Min(Tile.MinX, X);
				Tile.MinY = FMath::Min(Tile.MinY, Y);
				Tile.MaxX = FMath::Max(Tile.MaxX, X);
				Tile.MaxY = FMath::Max(Tile.MaxY, Y);
				++Tile.Count;
				DiffPixels[Index] = FColor::Red;
			}
			else
			{
				const uint8 Luminance = static_cast<uint8>(
					(static_cast<int32>(Before.R) + Before.G + Before.B) / 3);
				DiffPixels[Index] = FColor(Luminance, Luminance, Luminance, 255);
			}
		}
		TArray<uint8> DiffPng;
		if (!EncodePng(
				DiffPixels,
				BeforeWidth,
				BeforeHeight,
				DiffPng,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}
		FString DiffHash;
		if (!Infrastructure::TrySha256Hex(DiffPng, DiffHash))
		{
			return FMCPToolResult::Error(TEXT("Could not checksum the diff PNG."));
		}
		const FString DiffCaptureId =
			TEXT("viewport-diff-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		TSharedRef<FJsonObject> DiffMetadata = MakeShared<FJsonObject>();
		DiffMetadata->SetStringField(
			TEXT("schema"),
			TEXT("ue.viewport-visualization-capture.v1"));
		DiffMetadata->SetStringField(TEXT("captureId"), DiffCaptureId);
		DiffMetadata->SetStringField(TEXT("format"), TEXT("png"));
		DiffMetadata->SetStringField(TEXT("mimeType"), TEXT("image/png"));
		DiffMetadata->SetStringField(TEXT("imageSha256"), DiffHash);
		DiffMetadata->SetStringField(TEXT("captureKind"), TEXT("visualDiff"));
		DiffMetadata->SetStringField(TEXT("family"),
			BeforeMetadata->GetStringField(TEXT("family")));
		DiffMetadata->SetStringField(TEXT("mode"),
			BeforeMetadata->GetStringField(TEXT("mode")));
		DiffMetadata->SetStringField(TEXT("targetKind"),
			BeforeMetadata->GetStringField(TEXT("targetKind")));
		DiffMetadata->SetNumberField(TEXT("width"), BeforeWidth);
		DiffMetadata->SetNumberField(TEXT("height"), BeforeHeight);
		DiffMetadata->SetStringField(TEXT("artifactPath"), ImagePath(DiffCaptureId));
		DiffMetadata->SetObjectField(TEXT("renderFingerprint"), BeforeFingerprint);
		if (!SaveCapture(DiffCaptureId, DiffPng, DiffMetadata, Error))
		{
			return FMCPToolResult::Error(Error);
		}

		Result->SetStringField(TEXT("comparison"),
			ChangedPixelCount > 0 ? TEXT("changed") : TEXT("identical"));
		Result->SetNumberField(TEXT("width"), BeforeWidth);
		Result->SetNumberField(TEXT("height"), BeforeHeight);
		Result->SetNumberField(
			TEXT("changedPixelCount"),
			static_cast<double>(ChangedPixelCount));
		Result->SetNumberField(
			TEXT("changedPixelRatio"),
			static_cast<double>(ChangedPixelCount) / BeforePixels.Num());
		Result->SetNumberField(
			TEXT("meanAbsoluteDifference"),
			TotalDifference / BeforePixels.Num());
		if (ChangedPixelCount > 0)
		{
			Result->SetObjectField(
				TEXT("changedBounds"),
				BoundsJson(MinX, MinY, MaxX, MaxY));
		}
		TArray<int32> TileIds;
		ChangedTiles.GetKeys(TileIds);
		TileIds.Sort();
		TArray<TSharedPtr<FJsonValue>> ChangedRegions;
		for (const int32 TileId : TileIds)
		{
			if (ChangedRegions.Num() >= MaxChangedRegions)
			{
				break;
			}
			const FTileBounds& Tile = ChangedTiles[TileId];
			TSharedRef<FJsonObject> Region =
				BoundsJson(Tile.MinX, Tile.MinY, Tile.MaxX, Tile.MaxY);
			Region->SetNumberField(TEXT("changedPixelCount"), Tile.Count);
			ChangedRegions.Add(MakeShared<FJsonValueObject>(Region));
		}
		Result->SetArrayField(TEXT("changedRegions"), ChangedRegions);
		Result->SetBoolField(
			TEXT("changedRegionsTruncated"),
			ChangedTiles.Num() > MaxChangedRegions);
		Result->SetStringField(TEXT("captureId"), DiffCaptureId);
		Result->SetStringField(TEXT("artifactPath"), ImagePath(DiffCaptureId));
		Result->SetStringField(TEXT("mimeType"), TEXT("image/png"));
		if (DiffPng.Num() <= MaxInlinePngBytes)
		{
			Result->SetStringField(TEXT("image_base64"), FBase64::Encode(DiffPng));
			Result->SetNumberField(
				TEXT("base64Length"),
				Result->GetStringField(TEXT("image_base64")).Len());
			Result->SetBoolField(TEXT("inlineImageOmitted"), false);
		}
		else
		{
			Result->SetBoolField(TEXT("inlineImageOmitted"), true);
			Result->SetStringField(
				TEXT("inlineImageOmissionReason"),
				TEXT("Diff PNG exceeds the bounded 8 MiB inline response limit; use artifactPath."));
		}
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_AnalyzeViewportVisualization final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.viewport.visualization.analyze");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString CaptureId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("captureId"), CaptureId))
		{
			return InvalidRequest(TEXT("captureId is required."));
		}
		TSharedPtr<FJsonObject> Metadata;
		TArray<uint8> Png;
		FString Error;
		if (!LoadCapture(CaptureId, Metadata, Png, Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("capture_not_found"),
				404);
		}
		TArray<FColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		if (!DecodePng(Png, Pixels, Width, Height, Error))
		{
			return FMCPToolResult::Error(Error);
		}
		TArray<int64> Histogram;
		Histogram.Init(0, HistogramBinCount);
		int32 MinR = 255;
		int32 MinG = 255;
		int32 MinB = 255;
		int32 MaxR = 0;
		int32 MaxG = 0;
		int32 MaxB = 0;
		double SumR = 0.0;
		double SumG = 0.0;
		double SumB = 0.0;
		int64 NearBlack = 0;
		int64 NearWhite = 0;
		int64 Saturated = 0;
		int64 NonOpaque = 0;
		int64 InvalidNormal = 0;
		int64 ZeroNormal = 0;
		TSet<uint32> QuantizedColors;
		bool bColorsCapped = false;
		const FString Family = Metadata->GetStringField(TEXT("family"));
		const FString Mode = Metadata->GetStringField(TEXT("mode"));
		const bool bNormalMode =
			Mode.Contains(TEXT("Normal"), ESearchCase::IgnoreCase)
			|| Mode.Contains(TEXT("Tangent"), ESearchCase::IgnoreCase);
		for (const FColor& Pixel : Pixels)
		{
			MinR = FMath::Min(MinR, static_cast<int32>(Pixel.R));
			MinG = FMath::Min(MinG, static_cast<int32>(Pixel.G));
			MinB = FMath::Min(MinB, static_cast<int32>(Pixel.B));
			MaxR = FMath::Max(MaxR, static_cast<int32>(Pixel.R));
			MaxG = FMath::Max(MaxG, static_cast<int32>(Pixel.G));
			MaxB = FMath::Max(MaxB, static_cast<int32>(Pixel.B));
			SumR += Pixel.R;
			SumG += Pixel.G;
			SumB += Pixel.B;
			const int32 Luminance =
				(54 * Pixel.R + 183 * Pixel.G + 19 * Pixel.B) >> 8;
			++Histogram[FMath::Clamp(
				Luminance * HistogramBinCount / 256,
				0,
				HistogramBinCount - 1)];
			if (Pixel.R <= 4 && Pixel.G <= 4 && Pixel.B <= 4) ++NearBlack;
			if (Pixel.R >= 251 && Pixel.G >= 251 && Pixel.B >= 251) ++NearWhite;
			if (FMath::Min3(Pixel.R, Pixel.G, Pixel.B) <= 4
				&& FMath::Max3(Pixel.R, Pixel.G, Pixel.B) >= 251)
			{
				++Saturated;
			}
			if (Pixel.A != 255) ++NonOpaque;
			if (bNormalMode)
			{
				const FVector3f Normal(
					Pixel.R / 127.5f - 1.0f,
					Pixel.G / 127.5f - 1.0f,
					Pixel.B / 127.5f - 1.0f);
				const float Length = Normal.Size();
				if (FMath::Abs(Length - 1.0f) > 0.2f) ++InvalidNormal;
				if (Length < 0.1f) ++ZeroNormal;
			}
			if (!bColorsCapped)
			{
				QuantizedColors.Add(
					((Pixel.R >> 3) << 10)
					| ((Pixel.G >> 3) << 5)
					| (Pixel.B >> 3));
				if (QuantizedColors.Num() > 4096)
				{
					bColorsCapped = true;
				}
			}
		}
		const double PixelCount = Pixels.Num();
		TSharedRef<FJsonObject> Channels = MakeShared<FJsonObject>();
		auto AddChannel = [&Channels, PixelCount](
			const TCHAR* Name,
			const int32 Min,
			const int32 Max,
			const double Sum)
		{
			TSharedRef<FJsonObject> Channel = MakeShared<FJsonObject>();
			Channel->SetNumberField(TEXT("min"), Min);
			Channel->SetNumberField(TEXT("max"), Max);
			Channel->SetNumberField(TEXT("mean"), Sum / PixelCount);
			Channels->SetObjectField(Name, Channel);
		};
		AddChannel(TEXT("r"), MinR, MaxR, SumR);
		AddChannel(TEXT("g"), MinG, MaxG, SumG);
		AddChannel(TEXT("b"), MinB, MaxB, SumB);
		TArray<TSharedPtr<FJsonValue>> HistogramValues;
		for (const int64 Count : Histogram)
		{
			HistogramValues.Add(MakeShared<FJsonValueNumber>(
				static_cast<double>(Count)));
		}

		TSharedRef<FJsonObject> Ratios = MakeShared<FJsonObject>();
		Ratios->SetNumberField(TEXT("nearBlack"), NearBlack / PixelCount);
		Ratios->SetNumberField(TEXT("nearWhite"), NearWhite / PixelCount);
		Ratios->SetNumberField(TEXT("saturated"), Saturated / PixelCount);
		Ratios->SetNumberField(TEXT("nonOpaque"), NonOpaque / PixelCount);

		TArray<TSharedPtr<FJsonValue>> Regions;
		constexpr int32 GridSize = 4;
		for (int32 GridY = 0; GridY < GridSize; ++GridY)
		{
			for (int32 GridX = 0; GridX < GridSize; ++GridX)
			{
				const int32 StartX = GridX * Width / GridSize;
				const int32 EndX = (GridX + 1) * Width / GridSize;
				const int32 StartY = GridY * Height / GridSize;
				const int32 EndY = (GridY + 1) * Height / GridSize;
				double Sum = 0.0;
				int32 Count = 0;
				for (int32 Y = StartY; Y < EndY; ++Y)
				{
					for (int32 X = StartX; X < EndX; ++X)
					{
						const FColor& Pixel = Pixels[Y * Width + X];
						Sum += (Pixel.R + Pixel.G + Pixel.B) / (3.0 * 255.0);
						++Count;
					}
				}
				TSharedRef<FJsonObject> Region = MakeShared<FJsonObject>();
				Region->SetNumberField(TEXT("x"), StartX);
				Region->SetNumberField(TEXT("y"), StartY);
				Region->SetNumberField(TEXT("width"), EndX - StartX);
				Region->SetNumberField(TEXT("height"), EndY - StartY);
				Region->SetNumberField(
					TEXT("meanLuminance"),
					Count > 0 ? Sum / Count : 0.0);
				Regions.Add(MakeShared<FJsonValueObject>(Region));
			}
		}

		TSharedRef<FJsonObject> Semantic = MakeShared<FJsonObject>();
		Semantic->SetStringField(TEXT("kind"), bNormalMode
			? TEXT("encodedNormal")
			: (Mode.Contains(TEXT("ID"), ESearchCase::IgnoreCase)
				|| Mode.Contains(TEXT("Instance"), ESearchCase::IgnoreCase)
				|| Mode.Contains(TEXT("Triangle"), ESearchCase::IgnoreCase)
				? TEXT("categoricalColor")
				: TEXT("boundedColorStatistics")));
		Semantic->SetNumberField(
			TEXT("quantizedColorCount"),
			QuantizedColors.Num());
		Semantic->SetBoolField(
			TEXT("quantizedColorCountCapped"),
			bColorsCapped);
		if (bNormalMode)
		{
			Semantic->SetNumberField(
				TEXT("invalidEncodedNormalRatio"),
				InvalidNormal / PixelCount);
			Semantic->SetNumberField(
				TEXT("nearZeroEncodedNormalRatio"),
				ZeroNormal / PixelCount);
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.viewport-visualization-analysis.v1"));
		Result->SetStringField(TEXT("captureId"), CaptureId);
		Result->SetStringField(TEXT("family"), Family);
		Result->SetStringField(TEXT("mode"), Mode);
		Result->SetNumberField(TEXT("width"), Width);
		Result->SetNumberField(TEXT("height"), Height);
		Result->SetNumberField(TEXT("pixelCount"), PixelCount);
		Result->SetObjectField(TEXT("channels"), Channels);
		Result->SetArrayField(TEXT("luminanceHistogram"), HistogramValues);
		Result->SetObjectField(TEXT("ratios"), Ratios);
		Result->SetArrayField(TEXT("regions"), Regions);
		Result->SetObjectField(TEXT("semantic"), Semantic);
		Result->SetStringField(
			TEXT("evidenceBoundary"),
			TEXT("Analysis describes the encoded 8-bit debug-view image. It does not recover source GBuffer precision or infer scene correctness without a mode-specific baseline."));
		return FMCPToolResult::Ok(Result);
	}
};
}
}

namespace UEAIIntegrationTools
{
void RegisterViewportVisualizationTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller)
{
	using namespace UEAIIntegration::ViewportVisualization;
	Registry.Register(MakeShared<FTool_ListViewportVisualizations>(Controller));
	Registry.Register(MakeShared<FTool_CaptureViewportVisualization>(Controller));
	Registry.Register(MakeShared<FTool_CompareViewportVisualizations>());
	Registry.Register(MakeShared<FTool_AnalyzeViewportVisualization>());
}
}
