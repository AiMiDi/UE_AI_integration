#include "Infrastructure/PIESessionController.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
using UEAIIntegration::Infrastructure::FPIEControlResult;
using UEAIIntegration::Infrastructure::FPIESessionController;

FMCPToolResult ToToolResult(const TCHAR* Action, const FPIEControlResult& ControlResult)
{
	if (!ControlResult.bSuccess)
	{
		return FMCPToolResult::Error(
			ControlResult.ErrorMessage,
			ControlResult.ErrorCode,
			ControlResult.HttpStatus);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("action"), Action);
	Data->SetBoolField(TEXT("requested"), ControlResult.bRequested);
	Data->SetStringField(TEXT("state"), ControlResult.State);
	Data->SetStringField(TEXT("sessionId"), ControlResult.SessionId);
	Data->SetNumberField(
		TEXT("generation"),
		static_cast<double>(ControlResult.Generation));
	Data->SetBoolField(TEXT("paused"), ControlResult.bPaused);
	return FMCPToolResult::Ok(Data);
}

class FTool_StartPIE final : public FMCPToolBase
{
public:
	explicit FTool_StartPIE(FPIESessionController& InController)
		: Controller(InController)
	{
	}

	virtual FString GetCapabilityId() const override { return TEXT("scene.pie.start"); }

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(TEXT("start"), Controller.Start());
	}

private:
	FPIESessionController& Controller;
};

class FTool_StopPIE final : public FMCPToolBase
{
public:
	explicit FTool_StopPIE(FPIESessionController& InController)
		: Controller(InController)
	{
	}

	virtual FString GetCapabilityId() const override { return TEXT("scene.pie.stop"); }

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(TEXT("stop"), Controller.Stop());
	}

private:
	FPIESessionController& Controller;
};

class FTool_RestartPIE final : public FMCPToolBase
{
public:
	explicit FTool_RestartPIE(FPIESessionController& InController)
		: Controller(InController)
	{
	}

	virtual FString GetCapabilityId() const override { return TEXT("scene.pie.restart"); }

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(TEXT("restart"), Controller.Restart());
	}

private:
	FPIESessionController& Controller;
};

class FTool_GetPIEStatus final : public FMCPToolBase
{
public:
	explicit FTool_GetPIEStatus(FPIESessionController& InController)
		: Controller(InController)
	{
	}

	virtual FString GetCapabilityId() const override { return TEXT("scene.pie.status"); }

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(TEXT("status"), Controller.Status());
	}

private:
	FPIESessionController& Controller;
};

class FTool_PausePIE final : public FMCPToolBase
{
public:
	explicit FTool_PausePIE(FPIESessionController& InController)
		: Controller(InController)
	{
	}

	virtual FString GetCapabilityId() const override { return TEXT("scene.pie.pause"); }

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(TEXT("pause"), Controller.Pause());
	}

private:
	FPIESessionController& Controller;
};

class FTool_ResumePIE final : public FMCPToolBase
{
public:
	explicit FTool_ResumePIE(FPIESessionController& InController)
		: Controller(InController)
	{
	}

	virtual FString GetCapabilityId() const override { return TEXT("scene.pie.resume"); }

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(TEXT("resume"), Controller.Resume());
	}

private:
	FPIESessionController& Controller;
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneRuntimeQueryTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller);
void RegisterSceneRuntimeCommandTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller);

void RegisterPIETools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller)
{
	Registry.Register(MakeShared<FTool_StartPIE>(Controller));
	Registry.Register(MakeShared<FTool_StopPIE>(Controller));
	Registry.Register(MakeShared<FTool_RestartPIE>(Controller));
	Registry.Register(MakeShared<FTool_GetPIEStatus>(Controller));
	Registry.Register(MakeShared<FTool_PausePIE>(Controller));
	Registry.Register(MakeShared<FTool_ResumePIE>(Controller));
	RegisterSceneRuntimeQueryTools(Registry, Controller);
	RegisterSceneRuntimeCommandTools(Registry, Controller);
}
}
