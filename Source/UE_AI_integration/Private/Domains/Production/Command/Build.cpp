// Build / Package / Lighting tools for UE_AI_integration
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorBuildUtils.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"

// ─────────────────────────────────────────────────────────────
// build_lighting
// ─────────────────────────────────────────────────────────────
class FTool_BuildLighting : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.build.lighting");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Quality = Params->HasField(TEXT("quality")) ? Params->GetStringField(TEXT("quality")).ToLower() : TEXT("preview");

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Error(TEXT("No editor world available."));
		}

		// Set lighting quality via config
		int32 QualityLevel = 0; // Preview
		if (Quality == TEXT("medium")) QualityLevel = 1;
		else if (Quality == TEXT("high")) QualityLevel = 2;
		else if (Quality == TEXT("production")) QualityLevel = 3;

		// Use EditorBuildUtils to build lighting
		FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("lighting_build_initiated"));
		Result->SetStringField(TEXT("quality"), Quality);
		Result->SetNumberField(TEXT("quality_level"), QualityLevel);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// build_navigation_only
// ─────────────────────────────────────────────────────────────
class FTool_BuildNavigationOnly : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.build.navigation");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Error(TEXT("No editor world available."));
		}

		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
		if (!NavSys)
		{
			return FMCPToolResult::Error(TEXT("Navigation system not available."));
		}

		NavSys->Build();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("navigation_build_complete"));
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// cook_project
// ─────────────────────────────────────────────────────────────
class FTool_CookProject : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.project.cook");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Platform = Params->GetStringField(TEXT("platform"));
		FString Config = Params->HasField(TEXT("config")) ? Params->GetStringField(TEXT("config")) : TEXT("Development");

		if (Platform.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Parameter 'platform' is required."));
		}

		// Locate UAT
		FString EngineDir = FPaths::EngineDir();
		FString UATPath = FPaths::Combine(EngineDir, TEXT("Build"), TEXT("BatchFiles"));

#if PLATFORM_WINDOWS
		FString UATExe = FPaths::Combine(UATPath, TEXT("RunUAT.bat"));
#else
		FString UATExe = FPaths::Combine(UATPath, TEXT("RunUAT.sh"));
#endif

		if (!IFileManager::Get().FileExists(*UATExe))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("UAT not found at '%s'."), *UATExe));
		}

		FString ProjectFile = FPaths::GetProjectFilePath();

		FString CommandLine = FString::Printf(
			TEXT("BuildCookRun -project=\"%s\" -targetplatform=%s -clientconfig=%s -cook -allmaps -NoP4 -UTF8Output"),
			*ProjectFile, *Platform, *Config);

		// Launch UAT asynchronously
		FPlatformProcess::CreateProc(*UATExe, *CommandLine, true, false, false, nullptr, 0, nullptr, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("cook_initiated"));
		Result->SetStringField(TEXT("platform"), Platform);
		Result->SetStringField(TEXT("config"), Config);
		Result->SetStringField(TEXT("uat_path"), UATExe);
		Result->SetStringField(TEXT("command"), CommandLine);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// package_project
// ─────────────────────────────────────────────────────────────
class FTool_PackageProject : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.project.package");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Platform = Params->GetStringField(TEXT("platform"));
		FString OutputDir = Params->GetStringField(TEXT("output_dir"));
		FString Config = Params->HasField(TEXT("config")) ? Params->GetStringField(TEXT("config")) : TEXT("Shipping");

		if (Platform.IsEmpty() || OutputDir.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Both 'platform' and 'output_dir' are required."));
		}

		FString EngineDir = FPaths::EngineDir();
		FString UATPath = FPaths::Combine(EngineDir, TEXT("Build"), TEXT("BatchFiles"));

#if PLATFORM_WINDOWS
		FString UATExe = FPaths::Combine(UATPath, TEXT("RunUAT.bat"));
#else
		FString UATExe = FPaths::Combine(UATPath, TEXT("RunUAT.sh"));
#endif

		if (!IFileManager::Get().FileExists(*UATExe))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("UAT not found at '%s'."), *UATExe));
		}

		FString ProjectFile = FPaths::GetProjectFilePath();

		FString CommandLine = FString::Printf(
			TEXT("BuildCookRun -project=\"%s\" -targetplatform=%s -clientconfig=%s -build -cook -stage -package -archive -archivedirectory=\"%s\" -allmaps -NoP4 -UTF8Output"),
			*ProjectFile, *Platform, *Config, *OutputDir);

		FPlatformProcess::CreateProc(*UATExe, *CommandLine, true, false, false, nullptr, 0, nullptr, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("package_initiated"));
		Result->SetStringField(TEXT("platform"), Platform);
		Result->SetStringField(TEXT("config"), Config);
		Result->SetStringField(TEXT("output_dir"), OutputDir);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// get_build_status
// ─────────────────────────────────────────────────────────────
class FTool_GetBuildStatus : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.build.status");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

		// Check if lighting build is running
		bool bLightingInProgress = GEditor ? GEditor->IsLightingBuildCurrentlyRunning() : false;
		Result->SetBoolField(TEXT("lighting_build_active"), bLightingInProgress);

		// Check for any build (map, lighting, etc.)
		bool bAnyBuildInProgress = FEditorBuildUtils::IsBuildCurrentlyRunning();
		Result->SetBoolField(TEXT("map_build_active"), bAnyBuildInProgress);
		Result->SetBoolField(TEXT("any_build_active"), bLightingInProgress || bAnyBuildInProgress);

		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// run_commandlet
// ─────────────────────────────────────────────────────────────
class FTool_RunCommandlet : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("production.commandlet.run");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString CommandletName = Params->GetStringField(TEXT("commandlet_name"));
		FString Args = Params->HasField(TEXT("args")) ? Params->GetStringField(TEXT("args")) : TEXT("");

		if (CommandletName.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Parameter 'commandlet_name' is required."));
		}

		// Security: block dangerous commandlets
		static TArray<FString> BlockedCommandlets = {
			TEXT("DeletePackages"),
			TEXT("WipeContent"),
		};
		for (const FString& Blocked : BlockedCommandlets)
		{
			if (CommandletName.Contains(Blocked))
			{
				return FMCPToolResult::Error(FString::Printf(TEXT("Commandlet '%s' is blocked for safety."), *CommandletName));
			}
		}

		// Build the command line for the editor subprocess
		FString EngineDir = FPaths::EngineDir();

#if PLATFORM_WINDOWS
		FString EditorExe = FPaths::Combine(EngineDir, TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealEditor-Cmd.exe"));
#else
		FString EditorExe = FPaths::Combine(EngineDir, TEXT("Binaries"), TEXT("Linux"), TEXT("UnrealEditor-Cmd"));
#endif

		FString ProjectFile = FPaths::GetProjectFilePath();
		FString CommandLine = FString::Printf(
			TEXT("\"%s\" -run=%s %s -NoP4 -UTF8Output"),
			*ProjectFile, *CommandletName, *Args);

		FPlatformProcess::CreateProc(*EditorExe, *CommandLine, true, false, false, nullptr, 0, nullptr, nullptr);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("commandlet_launched"));
		Result->SetStringField(TEXT("commandlet"), CommandletName);
		Result->SetStringField(TEXT("args"), Args);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────
namespace UEAIIntegrationTools
{
	void RegisterBuildTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_BuildLighting>());
		Registry.Register(MakeShared<FTool_BuildNavigationOnly>());
		Registry.Register(MakeShared<FTool_CookProject>());
		Registry.Register(MakeShared<FTool_PackageProject>());
		Registry.Register(MakeShared<FTool_GetBuildStatus>());
		Registry.Register(MakeShared<FTool_RunCommandlet>());
	}
}
