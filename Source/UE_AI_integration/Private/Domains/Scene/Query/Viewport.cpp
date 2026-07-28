// Viewport and editor utility tools: capture, logs, console, level info
// Ported from unreal-claude's MCPTool_CaptureViewport, MCPTool_GetOutputLog, MCPTool_OpenLevel

#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Editor.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "UnrealClient.h"
#include "Slate/SceneViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"
#if PLATFORM_WINDOWS
#include "GenericPlatform/GenericWindow.h"
#include "Windows/WindowsHWrapper.h"
#endif
#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"
#include "Editor/UnrealEdEngine.h"
#include "UnrealEdGlobals.h"
#include "Misc/PackageName.h"
#include "ContentStreaming.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// capture_viewport
// ---------------------------------------------------------------------------

namespace
{
	constexpr int32 CaptureWidth = 1024;
	constexpr int32 CaptureHeight = 576;
	constexpr int32 JPEGQuality = 70;

	void ResizePixels(const TArray<FColor>& InPixels, int32 InW, int32 InH,
		TArray<FColor>& OutPixels, int32 OutW, int32 OutH)
	{
		OutPixels.SetNumUninitialized(OutW * OutH);
		const float SX = static_cast<float>(InW) / OutW;
		const float SY = static_cast<float>(InH) / OutH;
		for (int32 Y = 0; Y < OutH; ++Y)
		{
			for (int32 X = 0; X < OutW; ++X)
			{
				const int32 SrcX = FMath::Clamp(static_cast<int32>(X * SX), 0, InW - 1);
				const int32 SrcY = FMath::Clamp(static_cast<int32>(Y * SY), 0, InH - 1);
				OutPixels[Y * OutW + X] = InPixels[SrcY * InW + SrcX];
			}
		}
	}

#if PLATFORM_WINDOWS
	bool CaptureNativeWindowPixels(
		const TSharedRef<SWindow>& Window,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize)
	{
		const TSharedPtr<FGenericWindow> NativeWindow =
			Window->GetNativeWindow();
		HWND WindowHandle = NativeWindow.IsValid()
			? static_cast<HWND>(NativeWindow->GetOSWindowHandle())
			: nullptr;
		if (!WindowHandle || !::IsWindow(WindowHandle))
		{
			return false;
		}

		RECT ClientRect{};
		POINT ClientOrigin{};
		if (!::GetClientRect(WindowHandle, &ClientRect)
			|| !::ClientToScreen(WindowHandle, &ClientOrigin))
		{
			return false;
		}

		const int32 Width = ClientRect.right - ClientRect.left;
		const int32 Height = ClientRect.bottom - ClientRect.top;
		if (Width <= 0 || Height <= 0)
		{
			return false;
		}

		HDC ScreenDc = ::GetDC(nullptr);
		if (!ScreenDc)
		{
			return false;
		}
		HDC MemoryDc = ::CreateCompatibleDC(ScreenDc);
		if (!MemoryDc)
		{
			::ReleaseDC(nullptr, ScreenDc);
			return false;
		}

		BITMAPINFO BitmapInfo{};
		BitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		BitmapInfo.bmiHeader.biWidth = Width;
		// A negative height requests top-down rows, matching UE pixel arrays.
		BitmapInfo.bmiHeader.biHeight = -Height;
		BitmapInfo.bmiHeader.biPlanes = 1;
		BitmapInfo.bmiHeader.biBitCount = 32;
		BitmapInfo.bmiHeader.biCompression = BI_RGB;

		void* BitmapData = nullptr;
		HBITMAP Bitmap = ::CreateDIBSection(
			ScreenDc,
			&BitmapInfo,
			DIB_RGB_COLORS,
			&BitmapData,
			nullptr,
			0);
		if (!Bitmap || !BitmapData)
		{
			if (Bitmap)
			{
				::DeleteObject(Bitmap);
			}
			::DeleteDC(MemoryDc);
			::ReleaseDC(nullptr, ScreenDc);
			return false;
		}

		HGDIOBJ PreviousObject = ::SelectObject(MemoryDc, Bitmap);
		const bool bCopied =
			::BitBlt(
				MemoryDc,
				0,
				0,
				Width,
				Height,
				ScreenDc,
				ClientOrigin.x,
				ClientOrigin.y,
				SRCCOPY | CAPTUREBLT) != 0;
		if (bCopied)
		{
			OutPixels.SetNumUninitialized(Width * Height);
			FMemory::Memcpy(
				OutPixels.GetData(),
				BitmapData,
				OutPixels.Num() * sizeof(FColor));
			for (FColor& Pixel : OutPixels)
			{
				Pixel.A = 255;
			}
			OutSize = FIntPoint(Width, Height);
		}

		::SelectObject(MemoryDc, PreviousObject);
		::DeleteObject(Bitmap);
		::DeleteDC(MemoryDc);
		::ReleaseDC(nullptr, ScreenDc);
		return bCopied;
	}
#endif
}

class FTool_CaptureViewport : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.viewport.capture");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		if (!GEditor) return FMCPToolResult::Error(TEXT("Editor not available"));

		// Try PIE viewport first, then editor viewport
		FViewport* Viewport = GEditor->GetPIEViewport();
		FString ViewportType = TEXT("PIE");
		if (!Viewport)
		{
			Viewport = GEditor->GetActiveViewport();
			ViewportType = TEXT("Editor");
		}
		if (!Viewport) return FMCPToolResult::Error(TEXT("No viewport available. Open a level or start PIE."));

		FIntPoint SourceSize = Viewport->GetSizeXY();
		if (SourceSize.X <= 0 || SourceSize.Y <= 0) return FMCPToolResult::Error(TEXT("Viewport has invalid size"));

		TArray<FColor> Pixels;
		bool bCapturedSlateComposite = false;
		bool bSlateViewportWidgetValid = false;
		int32 SlateWindowCandidateCount = 0;
		FString CapturedSlateWindowTitle;
		FString CaptureSource = TEXT("viewport_pixels");
		if (ViewportType == TEXT("PIE") && FSlateApplication::IsInitialized())
		{
			// FViewport::ReadPixels only contains the rendered scene. The
			// SViewport screenshot also contains the game-layer Slate tree, so
			// runtime UMG widgets remain visible in scenario artifacts.
			FSceneViewport* SceneViewport = static_cast<FSceneViewport*>(Viewport);
			const TSharedPtr<SViewport> ViewportWidget =
				SceneViewport->GetViewportWidget().Pin();
			bSlateViewportWidgetValid = ViewportWidget.IsValid();
			FIntVector CompositeSize = FIntVector::ZeroValue;
			if (ViewportWidget.IsValid())
			{
				bCapturedSlateComposite =
					FSlateApplication::Get().TakeScreenshot(
						ViewportWidget.ToSharedRef(),
						Pixels,
						CompositeSize)
					&& CompositeSize.X > 0
					&& CompositeSize.Y > 0;
				if (bCapturedSlateComposite)
				{
					SourceSize = FIntPoint(CompositeSize.X, CompositeSize.Y);
					CaptureSource = TEXT("slate_viewport_composited");
				}
				else
				{
					Pixels.Reset();
				}
			}

			// Some embedded PIE layouts do not expose a valid SViewport or
			// cannot snapshot it directly. Capturing the active top-level
			// window still preserves the UMG game layer in the artifact.
			if (!bCapturedSlateComposite)
			{
				TArray<TSharedRef<SWindow>> CaptureWindows;
				auto AddCaptureWindow =
					[&CaptureWindows](const TSharedPtr<SWindow>& Candidate)
					{
						if (!Candidate.IsValid())
						{
							return;
						}
						const bool bAlreadyAdded =
							CaptureWindows.ContainsByPredicate(
								[&Candidate](
									const TSharedRef<SWindow>& Existing)
								{
									return &Existing.Get() == Candidate.Get();
								});
						if (!bAlreadyAdded)
						{
							CaptureWindows.Add(Candidate.ToSharedRef());
						}
					};

				if (ViewportWidget.IsValid())
				{
					AddCaptureWindow(
						FSlateApplication::Get().FindWidgetWindow(
							ViewportWidget.ToSharedRef()));
				}
				AddCaptureWindow(
					FSlateApplication::Get().GetActiveTopLevelWindow());
				for (const TSharedRef<SWindow>& TopLevelWindow :
					FSlateApplication::Get().GetTopLevelWindows())
				{
					AddCaptureWindow(TopLevelWindow);
				}
				SlateWindowCandidateCount = CaptureWindows.Num();

				for (const TSharedRef<SWindow>& CaptureWindow :
					CaptureWindows)
				{
					Pixels.Reset();
					CompositeSize = FIntVector::ZeroValue;
					bCapturedSlateComposite =
						FSlateApplication::Get().TakeScreenshot(
							CaptureWindow,
							Pixels,
							CompositeSize)
						&& CompositeSize.X > 0
						&& CompositeSize.Y > 0;
					if (bCapturedSlateComposite)
					{
						SourceSize =
							FIntPoint(CompositeSize.X, CompositeSize.Y);
						CaptureSource =
							TEXT("slate_window_composited");
						CapturedSlateWindowTitle =
							CaptureWindow->GetTitle().ToString();
						break;
					}
				}
				if (!bCapturedSlateComposite)
				{
					Pixels.Reset();
				}

#if PLATFORM_WINDOWS
				if (!bCapturedSlateComposite)
				{
					for (const TSharedRef<SWindow>& CaptureWindow :
						CaptureWindows)
					{
						FIntPoint NativeSize = FIntPoint::ZeroValue;
						if (CaptureNativeWindowPixels(
								CaptureWindow,
								Pixels,
								NativeSize))
						{
							bCapturedSlateComposite = true;
							SourceSize = NativeSize;
							CaptureSource =
								TEXT("native_window_composited");
							CapturedSlateWindowTitle =
								CaptureWindow->GetTitle().ToString();
							break;
						}
					}
				}
#endif
			}
		}

		if (!bCapturedSlateComposite && !Viewport->ReadPixels(Pixels))
			return FMCPToolResult::Error(TEXT("Failed to read viewport pixels"));

		if (Pixels.Num() != SourceSize.X * SourceSize.Y)
			return FMCPToolResult::Error(TEXT("Pixel array size mismatch"));

		// Optional params
		int32 OutW = Params->HasField(TEXT("width")) ? static_cast<int32>(Params->GetNumberField(TEXT("width"))) : CaptureWidth;
		int32 OutH = Params->HasField(TEXT("height")) ? static_cast<int32>(Params->GetNumberField(TEXT("height"))) : CaptureHeight;
		int32 Quality = Params->HasField(TEXT("quality")) ? FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("quality"))), 1, 100) : JPEGQuality;

		OutW = FMath::Clamp(OutW, 64, 4096);
		OutH = FMath::Clamp(OutH, 64, 4096);

		// Resize
		TArray<FColor> Resized;
		ResizePixels(Pixels, SourceSize.X, SourceSize.Y, Resized, OutW, OutH);

		// Compress to JPEG
		IImageWrapperModule& IWM = FModuleManager::LoadModuleChecked<IImageWrapperModule>("ImageWrapper");
		TSharedPtr<IImageWrapper> Wrapper = IWM.CreateImageWrapper(EImageFormat::JPEG);
		if (!Wrapper.IsValid()) return FMCPToolResult::Error(TEXT("Failed to create image wrapper"));

		if (!Wrapper->SetRaw(Resized.GetData(), Resized.Num() * sizeof(FColor), OutW, OutH, ERGBFormat::BGRA, 8))
			return FMCPToolResult::Error(TEXT("Failed to set raw image data"));

		TArray64<uint8> Compressed = Wrapper->GetCompressed(Quality);
		if (Compressed.Num() == 0) return FMCPToolResult::Error(TEXT("JPEG compression failed"));

		FString Base64 = FBase64::Encode(Compressed.GetData(), Compressed.Num());

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("image_base64"), Base64);
		Result->SetNumberField(TEXT("width"), OutW);
		Result->SetNumberField(TEXT("height"), OutH);
		Result->SetStringField(TEXT("format"), TEXT("jpeg"));
		Result->SetNumberField(TEXT("quality"), Quality);
		Result->SetStringField(TEXT("viewport_type"), ViewportType);
		Result->SetStringField(
			TEXT("capture_source"),
			CaptureSource);
		Result->SetBoolField(
			TEXT("slate_viewport_widget_valid"),
			bSlateViewportWidgetValid);
		Result->SetNumberField(
			TEXT("slate_window_candidates"),
			SlateWindowCandidateCount);
		if (!CapturedSlateWindowTitle.IsEmpty())
		{
			Result->SetStringField(
				TEXT("slate_window_title"),
				CapturedSlateWindowTitle);
		}
		Result->SetNumberField(TEXT("original_width"), SourceSize.X);
		Result->SetNumberField(TEXT("original_height"), SourceSize.Y);
		Result->SetNumberField(TEXT("base64_length"), Base64.Len());

		UE_LOG(LogTemp, Log, TEXT("[UE_AI_integration] Captured %s viewport: %dx%d -> %dx%d JPEG (%d bytes b64)"),
			*ViewportType, SourceSize.X, SourceSize.Y, OutW, OutH, Base64.Len());

		return FMCPToolResult::Ok(Result);
	}
};

// ---------------------------------------------------------------------------
// get_output_log
// ---------------------------------------------------------------------------

class FTool_GetOutputLog : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.output_log.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		int32 NumLines = 100;
		if (Params->HasField(TEXT("lines")))
			NumLines = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("lines"))), 1, 2000);

		FString Filter;
		Params->TryGetStringField(TEXT("filter"), Filter);

		// Search for log file across candidate paths
		FString ProjectLogDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
		FString EngineLogDir = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Saved/Logs"));

		FString LogFilePath;
		bool bFound = false;

		// 1. Project-named log
		{
			FString Candidate = ProjectLogDir / FApp::GetProjectName() + TEXT(".log");
			if (FPaths::FileExists(Candidate)) { LogFilePath = Candidate; bFound = true; }
		}
		// 2. UnrealEditor.log in project logs
		if (!bFound)
		{
			FString Candidate = ProjectLogDir / TEXT("UnrealEditor.log");
			if (FPaths::FileExists(Candidate)) { LogFilePath = Candidate; bFound = true; }
		}
		// 3. Any .log in project logs
		if (!bFound)
		{
			TArray<FString> LogFiles;
			IFileManager::Get().FindFiles(LogFiles, *ProjectLogDir, TEXT("*.log"));
			if (LogFiles.Num() > 0) { LogFilePath = ProjectLogDir / LogFiles[0]; bFound = true; }
		}
		// 4. UnrealEditor.log in engine logs
		if (!bFound)
		{
			FString Candidate = EngineLogDir / TEXT("UnrealEditor.log");
			if (FPaths::FileExists(Candidate)) { LogFilePath = Candidate; bFound = true; }
		}
		// 5. Any .log in engine logs
		if (!bFound)
		{
			TArray<FString> LogFiles;
			IFileManager::Get().FindFiles(LogFiles, *EngineLogDir, TEXT("*.log"));
			if (LogFiles.Num() > 0) { LogFilePath = EngineLogDir / LogFiles[0]; bFound = true; }
		}

		if (!bFound)
			return FMCPToolResult::Error(FString::Printf(TEXT("No log file found. Searched: %s, %s"), *ProjectLogDir, *EngineLogDir));

		FString LogContent;
		if (!FFileHelper::LoadFileToString(LogContent, *LogFilePath, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to read log: %s"), *LogFilePath));

		TArray<FString> AllLines;
		LogContent.ParseIntoArrayLines(AllLines);

		// Filter
		TArray<FString> Filtered;
		if (Filter.IsEmpty())
		{
			Filtered = AllLines;
		}
		else
		{
			for (const FString& Line : AllLines)
			{
				if (Line.Contains(Filter, ESearchCase::IgnoreCase))
					Filtered.Add(Line);
			}
		}

		// Take last N
		int32 Start = FMath::Max(0, Filtered.Num() - NumLines);
		TArray<FString> ResultLines;
		for (int32 i = Start; i < Filtered.Num(); ++i)
			ResultLines.Add(Filtered[i]);

		FString Output = FString::Join(ResultLines, TEXT("\n"));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("log_file"), LogFilePath);
		Result->SetNumberField(TEXT("total_lines"), AllLines.Num());
		Result->SetNumberField(TEXT("returned_lines"), ResultLines.Num());
		if (!Filter.IsEmpty())
		{
			Result->SetStringField(TEXT("filter"), Filter);
			Result->SetNumberField(TEXT("filtered_lines"), Filtered.Num());
		}
		Result->SetStringField(TEXT("content"), Output);
		return FMCPToolResult::Ok(Result);
	}
};

// ---------------------------------------------------------------------------
// run_console_command
// ---------------------------------------------------------------------------

class FTool_RunConsoleCommand : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.console.execute");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Command;
		if (!Params->TryGetStringField(TEXT("command"), Command))
			return FMCPToolResult::Error(TEXT("Missing 'command' parameter"));

		Command = Command.TrimStartAndEnd();
		if (Command.IsEmpty())
			return FMCPToolResult::Error(TEXT("Command is empty"));

		// Block dangerous commands
		FString CmdLower = Command.ToLower();
		static const TArray<FString> Blocked = {
			TEXT("exit"), TEXT("quit"), TEXT("open "), TEXT("restartlevel"),
			TEXT("disconnect"), TEXT("reconnect"), TEXT("servertravel")
		};
		for (const FString& B : Blocked)
		{
			if (CmdLower.StartsWith(B))
				return FMCPToolResult::Error(FString::Printf(TEXT("Command '%s' is blocked for safety. Use dedicated tools for level management."), *Command));
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPToolResult::Error(TEXT("No editor world"));

		GEditor->Exec(World, *Command, *GLog);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("command"), Command);
		Result->SetBoolField(TEXT("executed"), true);
		return FMCPToolResult::Ok(Result);
	}
};

// ---------------------------------------------------------------------------
// open_level
// ---------------------------------------------------------------------------

namespace
{
	/** Wait for async loading and texture streaming to settle after level load. */
	double WaitForLevelStreaming()
	{
		const double Start = FPlatformTime::Seconds();
		constexpr double MaxWait = 30.0;
		constexpr int32 RequiredStable = 3;

		FlushAsyncLoading();

		if (IStreamingManager::HasShutdown())
			return FPlatformTime::Seconds() - Start;

		IStreamingManager& SM = IStreamingManager::Get();
		int32 StableCount = 0;

		while (FPlatformTime::Seconds() - Start < MaxWait)
		{
			SM.BlockTillAllRequestsFinished(0.1f, false);

			if (SM.GetNumWantingResources() == 0 && !IsAsyncLoading())
			{
				if (++StableCount >= RequiredStable) break;
			}
			else
			{
				StableCount = 0;
			}
		}

		return FPlatformTime::Seconds() - Start;
	}
}

class FTool_OpenLevel : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.level.open");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString LevelPath;
		if (!Params->TryGetStringField(TEXT("levelPath"), LevelPath))
			return FMCPToolResult::Error(TEXT("Missing 'levelPath' parameter"));

		if (LevelPath.IsEmpty() || LevelPath.Len() > 512)
			return FMCPToolResult::Error(TEXT("Invalid level path"));
		if (!LevelPath.StartsWith(TEXT("/Game/")))
			return FMCPToolResult::Error(TEXT("Level path must start with '/Game/'"));
		if (LevelPath.Contains(TEXT("..")))
			return FMCPToolResult::Error(TEXT("Path traversal not allowed"));

		FString PackagePath = LevelPath;
		if (!FPackageName::DoesPackageExist(PackagePath))
			return FMCPToolResult::Error(FString::Printf(TEXT("Level not found: %s"), *LevelPath));

		FString Filename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePath, Filename, FPackageName::GetMapPackageExtension()))
			return FMCPToolResult::Error(FString::Printf(TEXT("Cannot resolve level path: %s"), *LevelPath));

		UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(Filename);
		if (!LoadedWorld)
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to load: %s"), *LevelPath));

		double WaitTime = WaitForLevelStreaming();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("levelPath"), LevelPath);
		Result->SetStringField(TEXT("map_name"), LoadedWorld->GetMapName());
		Result->SetNumberField(TEXT("streaming_wait_seconds"), WaitTime);
		return FMCPToolResult::Ok(Result);
	}
};

// ---------------------------------------------------------------------------
// get_level_info
// ---------------------------------------------------------------------------

class FTool_GetLevelInfo : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("scene.level.info");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World) return FMCPToolResult::Error(TEXT("No editor world"));

		TArray<AActor*> AllActors;
		UGameplayStatics::GetAllActorsOfClass(World, AActor::StaticClass(), AllActors);

		// Count actors by class
		TMap<FString, int32> ClassCounts;
		for (AActor* A : AllActors)
		{
			if (A)
			{
				FString ClassName = A->GetClass()->GetName();
				ClassCounts.FindOrAdd(ClassName)++;
			}
		}

		TSharedPtr<FJsonObject> ClassCountsJson = MakeShared<FJsonObject>();
		for (const auto& Pair : ClassCounts)
		{
			ClassCountsJson->SetNumberField(Pair.Key, Pair.Value);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("map_name"), World->GetMapName());
		Result->SetStringField(TEXT("world_name"), World->GetName());
		Result->SetNumberField(TEXT("actor_count"), AllActors.Num());
		Result->SetObjectField(TEXT("actor_class_counts"), ClassCountsJson);

		// World settings
		if (AWorldSettings* WS = World->GetWorldSettings())
		{
			Result->SetStringField(TEXT("game_mode"), WS->DefaultGameMode ? WS->DefaultGameMode->GetName() : TEXT("None"));
		}

		return FMCPToolResult::Ok(Result);
	}
};

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

namespace UEAIIntegrationTools
{
	void RegisterViewportTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_CaptureViewport>());
		Registry.Register(MakeShared<FTool_GetOutputLog>());
		Registry.Register(MakeShared<FTool_RunConsoleCommand>());
		Registry.Register(MakeShared<FTool_OpenLevel>());
		Registry.Register(MakeShared<FTool_GetLevelInfo>());
	}
}
