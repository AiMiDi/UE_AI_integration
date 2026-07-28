#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/Runtime/BlueprintDebugService.h"

namespace
{
using UEAIIntegration::Infrastructure::FBlueprintDebugResult;
using UEAIIntegration::Infrastructure::FBlueprintDebugService;

FMCPToolResult ConvertBlueprintDebugCommandResult(
	const FBlueprintDebugResult& Result)
{
	return Result.bSuccess
		? FMCPToolResult::Ok(Result.Data)
		: FMCPToolResult::Error(
			Result.ErrorMessage,
			Result.ErrorCode,
			Result.HttpStatus);
}

enum class EBlueprintDebugCommand : uint8
{
	BreakpointSet,
	BreakpointRemove,
	WatchSet,
	WatchRemove,
	Control,
};

class FBlueprintDebugCommandTool : public FMCPToolBase
{
public:
	FBlueprintDebugCommandTool(
		EBlueprintDebugCommand InCommand,
		FBlueprintDebugService& InService)
		: Command(InCommand)
		, Service(InService)
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		switch (Command)
		{
		case EBlueprintDebugCommand::BreakpointSet:
			return ConvertBlueprintDebugCommandResult(
				Service.SetBreakpoint(Params));
		case EBlueprintDebugCommand::BreakpointRemove:
			return ConvertBlueprintDebugCommandResult(
				Service.RemoveBreakpoint(Params));
		case EBlueprintDebugCommand::WatchSet:
			return ConvertBlueprintDebugCommandResult(Service.SetWatch(Params));
		case EBlueprintDebugCommand::WatchRemove:
			return ConvertBlueprintDebugCommandResult(
				Service.RemoveWatch(Params));
		case EBlueprintDebugCommand::Control:
			return ConvertBlueprintDebugCommandResult(Service.Control(Params));
		default:
			return FMCPToolResult::Error(
				TEXT("Unsupported Blueprint debug command."),
				TEXT("execution_failed"),
				500);
		}
	}

private:
	EBlueprintDebugCommand Command;
	FBlueprintDebugService& Service;
};

class FTool_BlueprintDebugBreakpointSet final : public FBlueprintDebugCommandTool
{
public:
	explicit FTool_BlueprintDebugBreakpointSet(FBlueprintDebugService& Service)
		: FBlueprintDebugCommandTool(
			EBlueprintDebugCommand::BreakpointSet,
			Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.breakpoint.set");
	}
};

class FTool_BlueprintDebugBreakpointRemove final : public FBlueprintDebugCommandTool
{
public:
	explicit FTool_BlueprintDebugBreakpointRemove(FBlueprintDebugService& Service)
		: FBlueprintDebugCommandTool(
			EBlueprintDebugCommand::BreakpointRemove,
			Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.breakpoint.remove");
	}
};

class FTool_BlueprintDebugWatchSet final : public FBlueprintDebugCommandTool
{
public:
	explicit FTool_BlueprintDebugWatchSet(FBlueprintDebugService& Service)
		: FBlueprintDebugCommandTool(EBlueprintDebugCommand::WatchSet, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.watch.set");
	}
};

class FTool_BlueprintDebugWatchRemove final : public FBlueprintDebugCommandTool
{
public:
	explicit FTool_BlueprintDebugWatchRemove(FBlueprintDebugService& Service)
		: FBlueprintDebugCommandTool(EBlueprintDebugCommand::WatchRemove, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.watch.remove");
	}
};

class FTool_BlueprintDebugControl final : public FBlueprintDebugCommandTool
{
public:
	explicit FTool_BlueprintDebugControl(FBlueprintDebugService& Service)
		: FBlueprintDebugCommandTool(EBlueprintDebugCommand::Control, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.control");
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintDebugCommandTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FBlueprintDebugService& Service)
{
	Registry.Register(MakeShared<FTool_BlueprintDebugBreakpointSet>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugBreakpointRemove>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugWatchSet>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugWatchRemove>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugControl>(Service));
}
}
