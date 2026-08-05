// Native Blueprint Graph Editor capture, artifact retrieval, and image diff.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Domains/Blueprint/BlueprintGraphEditorSupport.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Infrastructure/Sha256.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"
#include "Widgets/SWindow.h"

namespace
{
using namespace UEAIIntegration::BlueprintGraph;

constexpr int32 MaxCaptureDimension = 4096;
constexpr int64 MaxCaptureBytes = 64ll * 1024ll * 1024ll;
constexpr float MinGraphCaptureZoom = 0.10f;

FString CaptureDirectory()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("BlueprintGraphCaptures"));
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
	return FPaths::Combine(
		CaptureDirectory(),
		CaptureId + TEXT(".json"));
}

FString ImagePath(const FString& CaptureId, const FString& Format)
{
	return FPaths::Combine(
		CaptureDirectory(),
		CaptureId + (Format == TEXT("jpeg") ? TEXT(".jpg") : TEXT(".png")));
}

bool SerializeJson(
	const TSharedPtr<FJsonObject>& Object,
	FString& OutJson)
{
	OutJson.Reset();
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&OutJson);
	return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
}

bool SaveMetadata(
	const FString& CaptureId,
	const TSharedPtr<FJsonObject>& Metadata,
	FString& OutError)
{
	FString Json;
	if (!Metadata.IsValid() || !SerializeJson(Metadata, Json))
	{
		OutError = TEXT("Could not serialize capture metadata.");
		return false;
	}
	IFileManager::Get().MakeDirectory(*CaptureDirectory(), true);
	if (!FFileHelper::SaveStringToFile(
			Json,
			*MetadataPath(CaptureId),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("Could not save capture metadata.");
		return false;
	}
	return true;
}

bool LoadMetadata(
	const FString& CaptureId,
	TSharedPtr<FJsonObject>& OutMetadata,
	FString& OutError)
{
	if (!IsCaptureIdValid(CaptureId))
	{
		OutError = TEXT("captureId is invalid.");
		return false;
	}
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *MetadataPath(CaptureId)))
	{
		OutError = TEXT("Capture metadata was not found.");
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, OutMetadata)
		|| !OutMetadata.IsValid()
		|| OutMetadata->GetStringField(TEXT("captureId")) != CaptureId)
	{
		OutError = TEXT("Capture metadata is invalid.");
		return false;
	}
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

bool HasValidCapture(
	const TArray<FColor>& Pixels,
	const FIntVector& Size)
{
	return Size.X > 0
		&& Size.Y > 0
		&& Size.X <= MaxCaptureDimension
		&& Size.Y <= MaxCaptureDimension
		&& Pixels.Num()
			>= static_cast<int64>(Size.X) * static_cast<int64>(Size.Y);
}

bool CaptureWithWidgetRenderer(
	const TSharedRef<SWidget>& Widget,
	const FIntPoint& Size,
	TArray<FColor>& OutPixels)
{
	OutPixels.Reset();
	if (Size.X <= 0
		|| Size.Y <= 0
		|| Size.X > MaxCaptureDimension
		|| Size.Y > MaxCaptureDimension)
	{
		return false;
	}

	FWidgetRenderer WidgetRenderer(true, true);
	TStrongObjectPtr<UTextureRenderTarget2D> RenderTarget(
		WidgetRenderer.DrawWidget(
			Widget,
			FVector2D(Size.X, Size.Y)));
	if (!RenderTarget.IsValid())
	{
		return false;
	}
	FRenderTarget* Resource =
		RenderTarget->GameThread_GetRenderTargetResource();
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
	ReadFlags.SetLinearToGamma(false);
	if (!Resource
		|| !Resource->ReadPixels(
			OutPixels,
			ReadFlags,
			FIntRect(0, 0, Size.X, Size.Y))
		|| OutPixels.Num()
			< static_cast<int64>(Size.X) * static_cast<int64>(Size.Y))
	{
		OutPixels.Reset();
		return false;
	}
	OutPixels.SetNum(Size.X * Size.Y, false);
	for (FColor& Pixel : OutPixels)
	{
		Pixel.A = 255;
	}
	return true;
}

bool EncodeImage(
	const TArray<FColor>& Pixels,
	const int32 Width,
	const int32 Height,
	const FString& Format,
	const int32 Quality,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	IImageWrapperModule& Module =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(
			TEXT("ImageWrapper"));
	const EImageFormat ImageFormat =
		Format == TEXT("jpeg") ? EImageFormat::JPEG : EImageFormat::PNG;
	const TSharedPtr<IImageWrapper> Wrapper =
		Module.CreateImageWrapper(ImageFormat);
	if (!Wrapper.IsValid()
		|| !Wrapper->SetRaw(
			Pixels.GetData(),
			Pixels.Num() * sizeof(FColor),
			Width,
			Height,
			ERGBFormat::BGRA,
			8))
	{
		OutError = TEXT("Could not initialize the image encoder.");
		return false;
	}
	const TArray64<uint8> Compressed =
		Wrapper->GetCompressed(Format == TEXT("jpeg") ? Quality : 100);
	if (Compressed.IsEmpty() || Compressed.Num() > MaxCaptureBytes)
	{
		OutError = TEXT("Encoded capture is empty or exceeds 64 MiB.");
		return false;
	}
	OutBytes.SetNumUninitialized(static_cast<int32>(Compressed.Num()));
	FMemory::Memcpy(
		OutBytes.GetData(),
		Compressed.GetData(),
		Compressed.Num());
	return true;
}

bool DecodeImage(
	const TArray<uint8>& Bytes,
	TArray<FColor>& OutPixels,
	int32& OutWidth,
	int32& OutHeight,
	EImageFormat& OutFormat,
	FString& OutError)
{
	IImageWrapperModule& Module =
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(
			TEXT("ImageWrapper"));
	OutFormat =
		Module.DetectImageFormat(Bytes.GetData(), Bytes.Num());
	const TSharedPtr<IImageWrapper> Wrapper =
		Module.CreateImageWrapper(OutFormat);
	if (!Wrapper.IsValid()
		|| !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
	{
		OutError = TEXT("Capture image could not be decoded.");
		return false;
	}
	const int64 DecodedWidth = Wrapper->GetWidth();
	const int64 DecodedHeight = Wrapper->GetHeight();
	if (DecodedWidth <= 0
		|| DecodedHeight <= 0
		|| DecodedWidth > MaxCaptureDimension
		|| DecodedHeight > MaxCaptureDimension)
	{
		OutError = TEXT("Capture dimensions are outside the supported range.");
		return false;
	}
	const int64 PixelCount = DecodedWidth * DecodedHeight;
	const int64 ExpectedRawBytes = PixelCount * sizeof(FColor);
	if (PixelCount <= 0 || ExpectedRawBytes > MaxCaptureBytes)
	{
		OutError = TEXT("Decoded capture exceeds the supported pixel budget.");
		return false;
	}
	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
	{
		OutError = TEXT("Capture pixels could not be decoded.");
		return false;
	}
	if (Raw.Num() != ExpectedRawBytes)
	{
		OutError = TEXT("Capture dimensions are invalid.");
		return false;
	}
	OutWidth = static_cast<int32>(DecodedWidth);
	OutHeight = static_cast<int32>(DecodedHeight);
	OutPixels.SetNumUninitialized(static_cast<int32>(PixelCount));
	FMemory::Memcpy(
		OutPixels.GetData(),
		Raw.GetData(),
		Raw.Num());
	return true;
}

bool SaveCapture(
	const FString& CaptureId,
	const FString& Format,
	const TArray<uint8>& Bytes,
	const TSharedPtr<FJsonObject>& Metadata,
	FString& OutError)
{
	IFileManager::Get().MakeDirectory(*CaptureDirectory(), true);
	if (!FFileHelper::SaveArrayToFile(
			Bytes,
			*ImagePath(CaptureId, Format)))
	{
		OutError = TEXT("Could not save Graph capture image.");
		return false;
	}
	if (!SaveMetadata(CaptureId, Metadata, OutError))
	{
		IFileManager::Get().Delete(*ImagePath(CaptureId, Format));
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> BuildRenderFingerprint(
	const FContext& Context,
	const FString& Scope,
	const TArray<TSharedPtr<FJsonValue>>& CapturedNodeIds,
	const FString& CaptureProvider,
	const FString& Format,
	const FVector2D& ViewLocation,
	const float ZoomAmount,
	const int32 Width,
	const int32 Height)
{
	TSharedRef<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(
		TEXT("blueprint"),
		Context.Blueprint ? Context.Blueprint->GetPathName() : FString());
	Fingerprint->SetStringField(TEXT("graph"), Context.GraphName);
	Fingerprint->SetStringField(TEXT("scope"), Scope);
	if (Scope == TEXT("nodes"))
	{
		Fingerprint->SetArrayField(TEXT("capturedNodeIds"), CapturedNodeIds);
	}
	Fingerprint->SetStringField(
		TEXT("captureProvider"),
		CaptureProvider);
	Fingerprint->SetStringField(TEXT("format"), Format);
	TSharedRef<FJsonObject> ViewTransform = MakeShared<FJsonObject>();
	ViewTransform->SetNumberField(TEXT("x"), ViewLocation.X);
	ViewTransform->SetNumberField(TEXT("y"), ViewLocation.Y);
	ViewTransform->SetNumberField(TEXT("zoom"), ZoomAmount);
	// A currentView capture is explicitly tied to the user's pan/zoom. The
	// all/nodes modes use deterministic content fitting, so the resulting view
	// transform is evidence metadata rather than an environment compatibility
	// key; moving a node must remain a comparable layout change.
	if (Scope == TEXT("currentView"))
	{
		Fingerprint->SetObjectField(TEXT("viewTransform"), ViewTransform);
	}
	Fingerprint->SetNumberField(TEXT("width"), Width);
	Fingerprint->SetNumberField(TEXT("height"), Height);
	Fingerprint->SetNumberField(
		TEXT("applicationScale"),
		FSlateApplication::Get().GetApplicationScale());
	Fingerprint->SetStringField(
		TEXT("style"),
		FAppStyle::GetAppStyleSetName().ToString());
	USlateThemeManager& ThemeManager = USlateThemeManager::Get();
	Fingerprint->SetStringField(
		TEXT("themeId"),
		ThemeManager.GetCurrentThemeID().ToString(
			EGuidFormats::DigitsWithHyphensLower));
	Fingerprint->SetStringField(
		TEXT("themeBackground"),
		ThemeManager.GetColor(EStyleColor::Background)
			.ToFColorSRGB()
			.ToHex());
	Fingerprint->SetStringField(
		TEXT("themePanel"),
		ThemeManager.GetColor(EStyleColor::Panel)
			.ToFColorSRGB()
			.ToHex());
	Fingerprint->SetStringField(
		TEXT("themeForeground"),
		ThemeManager.GetColor(EStyleColor::Foreground)
			.ToFColorSRGB()
			.ToHex());
	Fingerprint->SetStringField(
		TEXT("themeSelection"),
		ThemeManager.GetColor(EStyleColor::Select)
			.ToFColorSRGB()
			.ToHex());
	if (Context.GraphEditor.IsValid())
	{
		const FGeometry& Geometry =
			Context.GraphEditor->GetCachedGeometry();
		const FVector2D WidgetSize = Geometry.GetLocalSize();
		Fingerprint->SetNumberField(
			TEXT("widgetLayoutScale"),
			Geometry.GetAccumulatedLayoutTransform().GetScale());
		Fingerprint->SetNumberField(
			TEXT("graphEditorWidth"),
			WidgetSize.X);
		Fingerprint->SetNumberField(
			TEXT("graphEditorHeight"),
			WidgetSize.Y);
		const TSharedPtr<SWindow> Window =
			FSlateApplication::Get().FindWidgetWindow(
				Context.GraphEditor.ToSharedRef());
		Fingerprint->SetNumberField(
			TEXT("windowDpiScale"),
			Window.IsValid() ? Window->GetDPIScaleFactor() : 0.0f);
	}
	Fingerprint->SetStringField(
		TEXT("engineVersion"),
		FEngineVersion::Current().ToString());
	Fingerprint->SetStringField(
		TEXT("pluginVersion"),
		UTF8_TO_TCHAR(UE_AI_INTEGRATION_VERSION));
	return Fingerprint;
}

TArray<TSharedPtr<FJsonValue>> FindFingerprintMismatchReasons(
	const TSharedPtr<FJsonObject>& Before,
	const TSharedPtr<FJsonObject>& After)
{
	TSet<FString> Fields;
	if (Before.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
			Before->Values)
		{
			Fields.Add(Pair.Key);
		}
	}
	if (After.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
			After->Values)
		{
			Fields.Add(Pair.Key);
		}
	}
	TArray<FString> SortedFields = Fields.Array();
	SortedFields.Sort();
	TArray<TSharedPtr<FJsonValue>> Reasons;
	for (const FString& Field : SortedFields)
	{
		const TSharedPtr<FJsonValue>* BeforeValue =
			Before.IsValid() ? Before->Values.Find(Field) : nullptr;
		const TSharedPtr<FJsonValue>* AfterValue =
			After.IsValid() ? After->Values.Find(Field) : nullptr;
		if (!BeforeValue || !AfterValue
			|| !BeforeValue->IsValid()
			|| !AfterValue->IsValid()
			|| !FJsonValue::CompareEqual(
				*BeforeValue->Get(),
				*AfterValue->Get()))
		{
			Reasons.Add(
				MakeShared<FJsonValueString>(
					TEXT("renderFingerprint.") + Field));
		}
	}
	return Reasons;
}

TSharedRef<FJsonObject> BuildImageResult(
	const TSharedPtr<FJsonObject>& Metadata,
	const TArray<uint8>& Bytes)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
		Metadata->Values)
	{
		Result->SetField(Pair.Key, Pair.Value);
	}
	Result->SetStringField(
		TEXT("image_base64"),
		FBase64::Encode(Bytes));
	Result->SetNumberField(TEXT("base64Length"), Result
		->GetStringField(TEXT("image_base64")).Len());
	return Result;
}

TSharedRef<FJsonObject> BuildCaptureMetadataResult(
	const TSharedPtr<FJsonObject>& Metadata,
	const TArray<uint8>& Bytes)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	for (const TCHAR* Field : {
		TEXT("schema"), TEXT("captureId"), TEXT("blueprint"), TEXT("graph"),
		TEXT("graphHash"), TEXT("format"), TEXT("mimeType"), TEXT("width"),
		TEXT("height"), TEXT("renderFingerprint") })
	{
		if (const TSharedPtr<FJsonValue>* Value = Metadata->Values.Find(Field))
		{
			Result->SetField(Field, *Value);
		}
	}
	const FString CaptureId = Metadata->GetStringField(TEXT("captureId"));
	const FString Format = Metadata->GetStringField(TEXT("format"));
	FString Sha256;
	UEAIIntegration::Infrastructure::TrySha256Hex(Bytes, Sha256);
	TSharedRef<FJsonObject> Artifact = MakeShared<FJsonObject>();
	Artifact->SetStringField(TEXT("kind"), TEXT("file"));
	Artifact->SetStringField(TEXT("path"), ImagePath(CaptureId, Format));
	Artifact->SetStringField(TEXT("mimeType"), Metadata->GetStringField(TEXT("mimeType")));
	Artifact->SetNumberField(TEXT("sizeBytes"), Bytes.Num());
	Artifact->SetStringField(TEXT("sha256"), Sha256);
	Result->SetObjectField(TEXT("artifact"), Artifact);
	Result->SetBoolField(TEXT("imageBase64Included"), false);
	return Result;
}

bool LoadCaptureBytes(
	const TSharedPtr<FJsonObject>& Metadata,
	TArray<uint8>& OutBytes,
	FString& OutError)
{
	const FString CaptureId =
		Metadata->GetStringField(TEXT("captureId"));
	const FString Format =
		Metadata->GetStringField(TEXT("format"));
	if (!FFileHelper::LoadFileToArray(
			OutBytes,
			*ImagePath(CaptureId, Format))
		|| OutBytes.IsEmpty()
		|| OutBytes.Num() > MaxCaptureBytes)
	{
		OutError = TEXT("Capture image is missing or invalid.");
		return false;
	}
	return true;
}

class FTool_CaptureBlueprintGraph final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.capture");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		if (!FSlateApplication::IsInitialized() || !FApp::CanEverRender())
		{
			return EditorUnavailable(
				TEXT("Graph capture requires a rendered Slate Editor."),
				TEXT("graph_capture_unavailable"));
		}

		FString Scope = TEXT("currentView");
		FString Format = TEXT("png");
		Params->TryGetStringField(TEXT("scope"), Scope);
		Params->TryGetStringField(TEXT("format"), Format);
		if (Scope != TEXT("currentView")
			&& Scope != TEXT("all")
			&& Scope != TEXT("nodes"))
		{
			return InvalidRequest(
				TEXT("scope must be currentView, all, or nodes."));
		}
		if (Format != TEXT("png") && Format != TEXT("jpeg"))
		{
			return InvalidRequest(TEXT("format must be png or jpeg."));
		}

		double Padding = 64.0;
		Params->TryGetNumberField(TEXT("padding"), Padding);
		if (!FMath::IsFinite(Padding)
			|| Padding < 0.0
			|| Padding > 1024.0)
		{
			return InvalidRequest(
				TEXT("padding must be between 0 and 1024."));
		}
		int32 RequestedWidth = 0;
		int32 RequestedHeight = 0;
		int32 Quality = 90;
		if (Params->HasField(TEXT("width")))
		{
			RequestedWidth = static_cast<int32>(
				Params->GetNumberField(TEXT("width")));
		}
		if (Params->HasField(TEXT("height")))
		{
			RequestedHeight = static_cast<int32>(
				Params->GetNumberField(TEXT("height")));
		}
		if (Params->HasField(TEXT("quality")))
		{
			Quality = FMath::Clamp(
				static_cast<int32>(
					Params->GetNumberField(TEXT("quality"))),
				1,
				100);
		}
		if ((RequestedWidth != 0
				&& (RequestedWidth < 64
					|| RequestedWidth > MaxCaptureDimension))
			|| (RequestedHeight != 0
				&& (RequestedHeight < 64
					|| RequestedHeight > MaxCaptureDimension)))
		{
			return InvalidRequest(
				TEXT("width and height must be between 64 and 4096."));
		}

		FContext Context;
		FMCPToolResult ContextResult =
			ResolveBlueprintGraph(Params, Context);
		if (!ContextResult.bSuccess)
		{
			return ContextResult;
		}
		ContextResult = ResolveGraphEditor(Context, true);
		if (!ContextResult.bSuccess)
		{
			return ContextResult;
		}
		Context.GraphEditor->SlatePrepass(1.0f);

		TArray<UEdGraphNode*> TargetNodes;
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (Scope == TEXT("nodes"))
		{
			if (!Params->TryGetArrayField(TEXT("nodeIds"), NodeValues)
				|| !NodeValues
				|| NodeValues->IsEmpty()
				|| NodeValues->Num() > 256)
			{
				return InvalidRequest(
					TEXT("scope=nodes requires between 1 and 256 nodeIds."));
			}
			for (const TSharedPtr<FJsonValue>& Value : *NodeValues)
			{
				FString NodeId;
				FGuid NodeGuid;
				FString Error;
				if (!Value.IsValid()
					|| !Value->TryGetString(NodeId)
					|| !TryParseNodeGuid(NodeId, NodeGuid, Error))
				{
					return InvalidRequest(
						Error.IsEmpty()
							? TEXT("nodeIds must contain Node GUIDs.")
							: Error);
				}
				UEdGraphNode* Node = FindNode(Context.Graph, NodeGuid);
				if (!Node)
				{
					return NotFound(
						FString::Printf(
							TEXT("Node '%s' was not found."),
							*NodeId));
				}
				TargetNodes.Add(Node);
			}
		}
		else if (Scope == TEXT("all"))
		{
			for (UEdGraphNode* Node : Context.Graph->Nodes)
			{
				if (Node)
				{
					TargetNodes.Add(Node);
				}
			}
			if (TargetNodes.IsEmpty())
			{
				return InvalidRequest(TEXT("The graph contains no nodes."));
			}
		}

		FVector2D OriginalView;
		float OriginalZoom = 1.0f;
		Context.GraphEditor->GetViewLocation(OriginalView, OriginalZoom);
		TArray<FGuid> OriginalSelection;
		for (UObject* Selected : Context.GraphEditor->GetSelectedNodes())
		{
			if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Selected))
			{
				OriginalSelection.Add(Node->NodeGuid);
			}
		}
		ON_SCOPE_EXIT
		{
			Context.GraphEditor->SetViewLocation(OriginalView, OriginalZoom);
			// Settle any deferred graph-panel rebuild before restoring the
			// selection. Refreshing after SetNodeSelection can otherwise clear
			// the restored set on the next Slate tick.
			RefreshGraphEditorLayout(Context.GraphEditor);
			Context.GraphEditor->ClearSelectionSet();
			for (const FGuid& NodeId : OriginalSelection)
			{
				if (UEdGraphNode* Node = FindNode(Context.Graph, NodeId))
				{
					Context.GraphEditor->SetNodeSelection(Node, true);
				}
			}
			const TSharedPtr<SWindow> RestoredWindow =
				FSlateApplication::Get().FindWidgetWindow(
					Context.GraphEditor.ToSharedRef());
			if (RestoredWindow.IsValid())
			{
				RestoredWindow->SlatePrepass(
					FSlateApplication::Get().GetApplicationScale()
					* RestoredWindow->GetDPIScaleFactor());
				FSlateApplication::Get().ForceRedrawWindow(
					RestoredWindow.ToSharedRef());
			}
		};

		// Captures must not inherit arbitrary interactive highlighting. A nodes
		// capture deliberately highlights exactly its requested set; all and
		// currentView use a clean selection. The scope guard restores it.
		bool bHighlightRequestedNodes = false;
		Params->TryGetBoolField(TEXT("highlightRequestedNodes"), bHighlightRequestedNodes);
		Context.GraphEditor->ClearSelectionSet();
		if (Scope == TEXT("nodes") && bHighlightRequestedNodes)
		{
			for (UEdGraphNode* Node : TargetNodes)
			{
				Context.GraphEditor->SetNodeSelection(Node, true);
			}
		}
		TSharedRef<FJsonObject> Framing = MakeShared<FJsonObject>();
		Framing->SetStringField(
			TEXT("mode"),
			Scope == TEXT("all")
				? TEXT("fitAll")
				: Scope == TEXT("nodes")
					? TEXT("fitNodes")
					: TEXT("currentView"));
		Framing->SetBoolField(TEXT("clipped"), false);
		float RequiredFitZoom = 0.0f;
		if (!TargetNodes.IsEmpty())
		{
			FNodeBounds Aggregate;
			Aggregate.X = TNumericLimits<double>::Max();
			Aggregate.Y = TNumericLimits<double>::Max();
			double MaxRight = TNumericLimits<double>::Lowest();
			double MaxBottom = TNumericLimits<double>::Lowest();
			for (UEdGraphNode* Node : TargetNodes)
			{
				FNodeBounds Bounds;
				if (!TryGetNodeBounds(Context, Node, true, Bounds)
					|| !Bounds.bExact)
				{
					return EditorUnavailable(
						TEXT("Exact target node geometry is unavailable."),
						TEXT("graph_geometry_unavailable"));
				}
				Aggregate.X = FMath::Min(Aggregate.X, Bounds.X);
				Aggregate.Y = FMath::Min(Aggregate.Y, Bounds.Y);
				MaxRight = FMath::Max(MaxRight, Bounds.Right());
				MaxBottom = FMath::Max(MaxBottom, Bounds.Bottom());
			}
			Aggregate.X -= Padding;
			Aggregate.Y -= Padding;
			Aggregate.Width = MaxRight - Aggregate.X + Padding;
			Aggregate.Height = MaxBottom - Aggregate.Y + Padding;
			if (!FMath::IsFinite(Aggregate.X)
				|| !FMath::IsFinite(Aggregate.Y)
				|| !FMath::IsFinite(Aggregate.Width)
				|| !FMath::IsFinite(Aggregate.Height)
				|| Aggregate.Width <= 0.0
				|| Aggregate.Height <= 0.0)
			{
				return EditorUnavailable(
					TEXT("Target node bounds are invalid."),
					TEXT("graph_geometry_unavailable"));
			}
			TSharedRef<FJsonObject> TargetBounds = MakeShared<FJsonObject>();
			TargetBounds->SetNumberField(TEXT("x"), Aggregate.X);
			TargetBounds->SetNumberField(TEXT("y"), Aggregate.Y);
			TargetBounds->SetNumberField(TEXT("width"), Aggregate.Width);
			TargetBounds->SetNumberField(TEXT("height"), Aggregate.Height);
			Framing->SetObjectField(TEXT("targetBounds"), TargetBounds);
			const FVector2D WidgetSize =
				Context.GraphEditor->GetCachedGeometry().GetLocalSize();
			if (WidgetSize.X <= 1.0
				|| WidgetSize.Y <= 1.0
				|| !FMath::IsFinite(WidgetSize.X)
				|| !FMath::IsFinite(WidgetSize.Y))
			{
				return EditorUnavailable(
					TEXT("The Graph Editor has no usable layout size."),
					TEXT("graph_capture_unavailable"));
			}
			RequiredFitZoom = static_cast<float>(
				FMath::Min(
					WidgetSize.X / Aggregate.Width,
					WidgetSize.Y / Aggregate.Height));
			if (!FMath::IsFinite(RequiredFitZoom)
				|| RequiredFitZoom < MinGraphCaptureZoom)
			{
				return EditorUnavailable(
					FString::Printf(
						TEXT("Target bounds require zoom %.4f, below the "
							"supported minimum %.2f; capture would be clipped."),
						RequiredFitZoom,
						MinGraphCaptureZoom),
					TEXT("graph_capture_clipped"));
			}

			float RequestedZoom = FMath::Min(RequiredFitZoom, 1.0f);
			float AppliedZoom = 1.0f;
			FVector2D AppliedView;
			// UE snaps requested zoom amounts to discrete Graph Editor levels.
			// Step down until the applied level is guaranteed to contain the
			// target bounds instead of silently accepting a clipped capture.
			for (int32 Attempt = 0; Attempt < 16; ++Attempt)
			{
				Context.GraphEditor->SetViewLocation(
					FVector2D(Aggregate.X, Aggregate.Y),
					RequestedZoom);
				Context.GraphEditor->GetViewLocation(
					AppliedView,
					AppliedZoom);
				if (AppliedZoom <= RequiredFitZoom + KINDA_SMALL_NUMBER)
				{
					break;
				}
				RequestedZoom = FMath::Max(
					MinGraphCaptureZoom,
					RequestedZoom * 0.75f);
			}
			if (!FMath::IsFinite(AppliedView.X)
				|| !FMath::IsFinite(AppliedView.Y)
				|| !FMath::IsFinite(AppliedZoom))
			{
				return EditorUnavailable(
					TEXT("The Graph Editor returned an invalid view transform."),
					TEXT("graph_capture_unavailable"));
			}
			if (AppliedZoom > RequiredFitZoom + KINDA_SMALL_NUMBER)
			{
				return EditorUnavailable(
					FString::Printf(
						TEXT("The nearest Graph Editor zoom %.4f exceeds the "
							"required fit zoom %.4f; capture would be clipped."),
						AppliedZoom,
						RequiredFitZoom),
					TEXT("graph_capture_clipped"));
			}
			Framing->SetNumberField(TEXT("requiredZoom"), RequiredFitZoom);
			Framing->SetNumberField(TEXT("appliedZoom"), AppliedZoom);
		}
		Context.GraphEditor->SlatePrepass(1.0f);
		const TSharedPtr<SWindow> CaptureWindow =
			FSlateApplication::Get().FindWidgetWindow(
				Context.GraphEditor.ToSharedRef());
		if (!CaptureWindow.IsValid())
		{
			return EditorUnavailable(
				TEXT("The specified Graph Editor has no rendered window."),
				TEXT("graph_capture_unavailable"));
		}
		// Force one complete Slate draw after selection/view changes. The
		// following TakeScreenshot performs the next draw, avoiding a capture of
		// the prior frame without running a nested Slate tick.
		FSlateApplication::Get().ForceRedrawWindow(
			CaptureWindow.ToSharedRef());
		Context.GraphEditor->SlatePrepass(1.0f);
		FVector2D CaptureView;
		float CaptureZoom = 1.0f;
		Context.GraphEditor->GetViewLocation(CaptureView, CaptureZoom);

		TArray<FColor> Pixels;
		FIntVector ScreenshotSize = FIntVector::ZeroValue;
		bool bCaptured =
			FSlateApplication::Get().TakeScreenshot(
				Context.GraphEditor.ToSharedRef(),
				Pixels,
				ScreenshotSize);
		FString CaptureProvider = TEXT("slateLdr");
		if (!bCaptured || !HasValidCapture(Pixels, ScreenshotSize))
		{
			TArray<FLinearColor> HdrPixels;
			FIntVector HdrSize = FIntVector::ZeroValue;
			const bool bHdrCaptured =
				FSlateApplication::Get().TakeHDRScreenshot(
					Context.GraphEditor.ToSharedRef(),
					HdrPixels,
					HdrSize);
			const int64 ExpectedHdrPixels =
				static_cast<int64>(HdrSize.X)
				* static_cast<int64>(HdrSize.Y);
			if (bHdrCaptured
				&& HdrSize.X > 0
				&& HdrSize.Y > 0
				&& HdrSize.X <= MaxCaptureDimension
				&& HdrSize.Y <= MaxCaptureDimension
				&& HdrPixels.Num() >= ExpectedHdrPixels)
			{
				ScreenshotSize = HdrSize;
				Pixels.SetNumUninitialized(
					static_cast<int32>(ExpectedHdrPixels));
				for (int32 Index = 0; Index < Pixels.Num(); ++Index)
				{
					Pixels[Index] = HdrPixels[Index].ToFColorSRGB();
					Pixels[Index].A = 255;
				}
				bCaptured = true;
				CaptureProvider = TEXT("slateHdrFallback");
			}
		}
		if (!bCaptured || !HasValidCapture(Pixels, ScreenshotSize))
		{
			const FVector2D CachedSize =
				Context.GraphEditor->GetCachedGeometry().GetLocalSize();
			const FIntPoint WidgetRenderSize(
				FMath::Clamp(
					FMath::CeilToInt(CachedSize.X),
					1,
					MaxCaptureDimension),
				FMath::Clamp(
					FMath::CeilToInt(CachedSize.Y),
					1,
					MaxCaptureDimension));
			if (CaptureWithWidgetRenderer(
					Context.GraphEditor.ToSharedRef(),
					WidgetRenderSize,
					Pixels))
			{
				ScreenshotSize =
					FIntVector(
						WidgetRenderSize.X,
						WidgetRenderSize.Y,
						0);
				bCaptured = true;
				CaptureProvider = TEXT("widgetRendererFallback");
			}
		}
		if (!bCaptured || !HasValidCapture(Pixels, ScreenshotSize))
		{
			const FVector2D CachedSize =
				Context.GraphEditor->GetCachedGeometry().GetLocalSize();
			return EditorUnavailable(
				FString::Printf(
					TEXT("The specified Graph Editor could not be captured "
						"(captured=%s, cachedSize=%.1fx%.1f, "
						"screenshotSize=%dx%d, pixels=%d)."),
					bCaptured ? TEXT("true") : TEXT("false"),
					CachedSize.X,
					CachedSize.Y,
					ScreenshotSize.X,
					ScreenshotSize.Y,
					Pixels.Num()),
				TEXT("graph_capture_unavailable"));
		}
		Pixels.SetNum(
			ScreenshotSize.X * ScreenshotSize.Y,
			false);

		const int32 OutputWidth =
			RequestedWidth > 0 ? RequestedWidth : ScreenshotSize.X;
		const int32 OutputHeight =
			RequestedHeight > 0 ? RequestedHeight : ScreenshotSize.Y;
		if (OutputWidth > MaxCaptureDimension
			|| OutputHeight > MaxCaptureDimension)
		{
			return InvalidRequest(
				TEXT("The Graph Editor is larger than 4096 pixels; specify "
					"bounded output dimensions."));
		}
		TArray<FColor> OutputPixels;
		ResizePixels(
			Pixels,
			ScreenshotSize.X,
			ScreenshotSize.Y,
			OutputPixels,
			OutputWidth,
			OutputHeight);

		TArray<uint8> Bytes;
		FString Error;
		if (!EncodeImage(
				OutputPixels,
				OutputWidth,
				OutputHeight,
				Format,
				Quality,
				Bytes,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}

		const FString CaptureId =
			TEXT("graph-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(
			TEXT("schema"),
			TEXT("ue.blueprint-graph-capture.v1"));
		Metadata->SetStringField(TEXT("captureId"), CaptureId);
		Metadata->SetStringField(
			TEXT("blueprint"),
			Context.Blueprint->GetPathName());
		Metadata->SetStringField(TEXT("graph"), Context.GraphName);
		Metadata->SetStringField(
			TEXT("graphHash"),
			ComputeGraphHash(Context.Graph));
		Metadata->SetStringField(TEXT("scope"), Scope);
		Metadata->SetStringField(TEXT("format"), Format);
		Metadata->SetStringField(
			TEXT("captureProvider"),
			CaptureProvider);
		Metadata->SetStringField(
			TEXT("mimeType"),
			Format == TEXT("jpeg") ? TEXT("image/jpeg") : TEXT("image/png"));
		Metadata->SetNumberField(TEXT("width"), OutputWidth);
		Metadata->SetNumberField(TEXT("height"), OutputHeight);
		Metadata->SetStringField(
			TEXT("capturedAtUtc"),
			FDateTime::UtcNow().ToIso8601());
		Metadata->SetNumberField(
			TEXT("dpiScale"),
			FSlateApplication::Get().GetApplicationScale());
		TSharedRef<FJsonObject> ViewTransform = MakeShared<FJsonObject>();
		ViewTransform->SetNumberField(TEXT("x"), CaptureView.X);
		ViewTransform->SetNumberField(TEXT("y"), CaptureView.Y);
		ViewTransform->SetNumberField(TEXT("zoom"), CaptureZoom);
		Metadata->SetObjectField(TEXT("viewTransform"), ViewTransform);
		Framing->SetObjectField(TEXT("viewTransform"), ViewTransform);
		Framing->SetNumberField(TEXT("appliedZoom"), CaptureZoom);
		Metadata->SetObjectField(TEXT("framing"), Framing);
		TArray<TSharedPtr<FJsonValue>> CapturedNodeIds;
		for (const UEdGraphNode* Node : TargetNodes)
		{
			CapturedNodeIds.Add(
				MakeShared<FJsonValueString>(Node->NodeGuid.ToString()));
		}
		CapturedNodeIds.Sort(
			[](const TSharedPtr<FJsonValue>& Left,
				const TSharedPtr<FJsonValue>& Right)
			{
				return Left->AsString() < Right->AsString();
			});
		Metadata->SetArrayField(
			TEXT("capturedNodeIds"),
			CapturedNodeIds);
		Metadata->SetStringField(
			TEXT("selectionPolicy"),
			Scope == TEXT("nodes") && bHighlightRequestedNodes
				? TEXT("requestedNodes")
				: TEXT("cleared"));
		Metadata->SetNumberField(
			TEXT("settledSlateDrawCount"),
			2);
		Metadata->SetObjectField(
			TEXT("renderFingerprint"),
			BuildRenderFingerprint(
				Context,
				Scope,
				CapturedNodeIds,
				CaptureProvider,
				Format,
				CaptureView,
				CaptureZoom,
				OutputWidth,
				OutputHeight));
		if (!SaveCapture(
				CaptureId,
				Format,
				Bytes,
				Metadata,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}
		bool bIncludeImageBase64 = false;
		Params->TryGetBoolField(TEXT("includeImageBase64"), bIncludeImageBase64);
		return FMCPToolResult::Ok(
			bIncludeImageBase64
				? BuildImageResult(Metadata, Bytes)
				: BuildCaptureMetadataResult(Metadata, Bytes));
	}
};

class FTool_GetBlueprintGraphCapture final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.capture.get");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		FString CaptureId;
		if (!Params.IsValid()
			|| !Params->TryGetStringField(TEXT("captureId"), CaptureId))
		{
			return InvalidRequest(TEXT("captureId is required."));
		}
		TSharedPtr<FJsonObject> Metadata;
		FString Error;
		if (!LoadMetadata(CaptureId, Metadata, Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("capture_not_found"),
				404);
		}
		TArray<uint8> Bytes;
		if (!LoadCaptureBytes(Metadata, Bytes, Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("capture_not_found"),
				404);
		}
		return FMCPToolResult::Ok(
			BuildImageResult(Metadata, Bytes));
	}
};

class FTool_CompareBlueprintGraphCaptures final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.graph.visual.compare");
	}

	FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
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

		TSharedPtr<FJsonObject> BeforeMetadata;
		TSharedPtr<FJsonObject> AfterMetadata;
		FString Error;
		if (!LoadMetadata(BeforeCaptureId, BeforeMetadata, Error)
			|| !LoadMetadata(AfterCaptureId, AfterMetadata, Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("capture_not_found"),
				404);
		}
		TArray<uint8> BeforeBytes;
		TArray<uint8> AfterBytes;
		if (!LoadCaptureBytes(BeforeMetadata, BeforeBytes, Error)
			|| !LoadCaptureBytes(AfterMetadata, AfterBytes, Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("capture_not_found"),
				404);
		}

		TArray<FColor> BeforePixels;
		TArray<FColor> AfterPixels;
		int32 BeforeWidth = 0;
		int32 BeforeHeight = 0;
		int32 AfterWidth = 0;
		int32 AfterHeight = 0;
		EImageFormat BeforeImageFormat = EImageFormat::Invalid;
		EImageFormat AfterImageFormat = EImageFormat::Invalid;
		if (!DecodeImage(
				BeforeBytes,
				BeforePixels,
				BeforeWidth,
				BeforeHeight,
				BeforeImageFormat,
				Error)
			|| !DecodeImage(
				AfterBytes,
				AfterPixels,
				AfterWidth,
				AfterHeight,
				AfterImageFormat,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}

		const TSharedPtr<FJsonObject> BeforeFingerprint =
			BeforeMetadata->GetObjectField(TEXT("renderFingerprint"));
		const TSharedPtr<FJsonObject> AfterFingerprint =
			AfterMetadata->GetObjectField(TEXT("renderFingerprint"));
		TArray<TSharedPtr<FJsonValue>> IncompatibilityReasons =
			FindFingerprintMismatchReasons(
				BeforeFingerprint,
				AfterFingerprint);
		FString BeforeFormat;
		FString AfterFormat;
		BeforeMetadata->TryGetStringField(TEXT("format"), BeforeFormat);
		AfterMetadata->TryGetStringField(TEXT("format"), AfterFormat);
		if (BeforeFormat != TEXT("png")
			|| AfterFormat != TEXT("png")
			|| BeforeImageFormat != EImageFormat::PNG
			|| AfterImageFormat != EImageFormat::PNG)
		{
			IncompatibilityReasons.Add(
				MakeShared<FJsonValueString>(
					TEXT("visualCompareRequiresPng")));
		}
		if ((BeforeWidth != AfterWidth || BeforeHeight != AfterHeight)
			&& !IncompatibilityReasons.ContainsByPredicate(
				[](const TSharedPtr<FJsonValue>& Reason)
				{
					return Reason.IsValid()
						&& (Reason->AsString()
								== TEXT("renderFingerprint.width")
							|| Reason->AsString()
								== TEXT("renderFingerprint.height"));
				}))
		{
			IncompatibilityReasons.Add(
				MakeShared<FJsonValueString>(
					TEXT("decodedImageDimensions")));
		}
		const bool bCompatible = IncompatibilityReasons.IsEmpty();

		TArray<FColor> ComparableAfter;
		ResizePixels(
			AfterPixels,
			AfterWidth,
			AfterHeight,
			ComparableAfter,
			BeforeWidth,
			BeforeHeight);
		TArray<FColor> DiffPixels;
		DiffPixels.SetNumUninitialized(BeforePixels.Num());
		int64 ChangedPixels = 0;
		int32 MinX = BeforeWidth;
		int32 MinY = BeforeHeight;
		int32 MaxX = -1;
		int32 MaxY = -1;
		double TotalDelta = 0.0;
		for (int32 Index = 0; Index < BeforePixels.Num(); ++Index)
		{
			const FColor& Before = BeforePixels[Index];
			const FColor& After = ComparableAfter[Index];
			const int32 Delta =
				FMath::Abs(static_cast<int32>(Before.R) - After.R)
				+ FMath::Abs(static_cast<int32>(Before.G) - After.G)
				+ FMath::Abs(static_cast<int32>(Before.B) - After.B);
			TotalDelta += Delta / (3.0 * 255.0);
			if (Delta > 24)
			{
				++ChangedPixels;
				const int32 X = Index % BeforeWidth;
				const int32 Y = Index / BeforeWidth;
				MinX = FMath::Min(MinX, X);
				MinY = FMath::Min(MinY, Y);
				MaxX = FMath::Max(MaxX, X);
				MaxY = FMath::Max(MaxY, Y);
				DiffPixels[Index] = FColor::Red;
			}
			else
			{
				const uint8 Luminance = static_cast<uint8>(
					(static_cast<int32>(Before.R)
						+ Before.G
						+ Before.B)
					/ 6);
				DiffPixels[Index] =
					FColor(Luminance, Luminance, Luminance, 255);
			}
		}

		TArray<uint8> DiffBytes;
		if (!EncodeImage(
				DiffPixels,
				BeforeWidth,
				BeforeHeight,
				TEXT("png"),
				100,
				DiffBytes,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}
		const FString DiffCaptureId =
			TEXT("graph-diff-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
		Metadata->SetStringField(
			TEXT("schema"),
			TEXT("ue.blueprint-graph-visual-diff.v1"));
		Metadata->SetStringField(TEXT("captureId"), DiffCaptureId);
		Metadata->SetStringField(
			TEXT("beforeCaptureId"),
			BeforeCaptureId);
		Metadata->SetStringField(
			TEXT("afterCaptureId"),
			AfterCaptureId);
		Metadata->SetStringField(TEXT("format"), TEXT("png"));
		Metadata->SetStringField(TEXT("mimeType"), TEXT("image/png"));
		Metadata->SetNumberField(TEXT("width"), BeforeWidth);
		Metadata->SetNumberField(TEXT("height"), BeforeHeight);
		Metadata->SetStringField(
			TEXT("comparison"),
			bCompatible ? TEXT("comparable") : TEXT("inconclusive"));
		Metadata->SetBoolField(TEXT("compatible"), bCompatible);
		Metadata->SetArrayField(
			TEXT("incompatibilityReasons"),
			IncompatibilityReasons);
		if (bCompatible && !BeforePixels.IsEmpty())
		{
			Metadata->SetNumberField(
				TEXT("changedPixelCount"),
				static_cast<double>(ChangedPixels));
			Metadata->SetNumberField(
				TEXT("changedPixelRatio"),
				static_cast<double>(ChangedPixels) / BeforePixels.Num());
			Metadata->SetNumberField(
				TEXT("meanAbsoluteDifference"),
				TotalDelta / BeforePixels.Num());
		}
		else
		{
			Metadata->SetField(
				TEXT("changedPixelCount"),
				MakeShared<FJsonValueNull>());
			Metadata->SetField(
				TEXT("changedPixelRatio"),
				MakeShared<FJsonValueNull>());
			Metadata->SetField(
				TEXT("meanAbsoluteDifference"),
				MakeShared<FJsonValueNull>());
		}
		if (bCompatible && MaxX >= MinX && MaxY >= MinY)
		{
			TSharedRef<FJsonObject> ChangedBounds =
				MakeShared<FJsonObject>();
			ChangedBounds->SetNumberField(TEXT("x"), MinX);
			ChangedBounds->SetNumberField(TEXT("y"), MinY);
			ChangedBounds->SetNumberField(
				TEXT("width"),
				MaxX - MinX + 1);
			ChangedBounds->SetNumberField(
				TEXT("height"),
				MaxY - MinY + 1);
			Metadata->SetObjectField(
				TEXT("changedBounds"),
				ChangedBounds);
		}
		Metadata->SetObjectField(
			TEXT("renderFingerprint"),
			BeforeFingerprint);
		if (!SaveCapture(
				DiffCaptureId,
				TEXT("png"),
				DiffBytes,
				Metadata,
				Error))
		{
			return FMCPToolResult::Error(Error);
		}
		return FMCPToolResult::Ok(
			BuildImageResult(Metadata, DiffBytes));
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintGraphVisualTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_CaptureBlueprintGraph>());
	Registry.Register(MakeShared<FTool_GetBlueprintGraphCapture>());
	Registry.Register(MakeShared<FTool_CompareBlueprintGraphCaptures>());
}
}
