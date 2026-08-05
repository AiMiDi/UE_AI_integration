#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/Runtime/BlueprintDebugService.h"

namespace
{
using UEAIIntegration::Infrastructure::FBlueprintDebugResult;
using UEAIIntegration::Infrastructure::FBlueprintDebugService;

FMCPToolResult ConvertBlueprintDebugQueryResult(
	const FBlueprintDebugResult& Result)
{
	return Result.bSuccess
		? FMCPToolResult::Ok(Result.Data)
		: FMCPToolResult::Error(
			Result.ErrorMessage,
			Result.ErrorCode,
			Result.HttpStatus);
}

enum class EBlueprintDebugQuery : uint8
{
	Session,
	Trace,
	Breakpoints,
	Watches,
	WatchValue,
};

class FBlueprintDebugQueryTool : public FMCPToolBase
{
public:
	FBlueprintDebugQueryTool(
		EBlueprintDebugQuery InQuery,
		FBlueprintDebugService& InService)
		: Query(InQuery)
		, Service(InService)
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		switch (Query)
		{
		case EBlueprintDebugQuery::Session:
			return ConvertBlueprintDebugQueryResult(Service.GetSession(Params));
		case EBlueprintDebugQuery::Trace:
			return ConvertBlueprintDebugQueryResult(Service.GetTrace(Params));
		case EBlueprintDebugQuery::Breakpoints:
			return ConvertBlueprintDebugQueryResult(
				Service.ListBreakpoints(Params));
		case EBlueprintDebugQuery::Watches:
			return ConvertBlueprintDebugQueryResult(Service.ListWatches(Params));
		case EBlueprintDebugQuery::WatchValue:
			return ConvertBlueprintDebugQueryResult(
				Service.GetWatchValue(Params));
		default:
			return FMCPToolResult::Error(
				TEXT("Unsupported Blueprint debug query."),
				TEXT("execution_failed"),
				500);
		}
	}

private:
	EBlueprintDebugQuery Query;
	FBlueprintDebugService& Service;
};

class FTool_BlueprintDebugSessionGet final : public FBlueprintDebugQueryTool
{
public:
	explicit FTool_BlueprintDebugSessionGet(FBlueprintDebugService& Service)
		: FBlueprintDebugQueryTool(EBlueprintDebugQuery::Session, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.session.get");
	}
};

class FTool_BlueprintDebugTraceGet final : public FBlueprintDebugQueryTool
{
public:
	explicit FTool_BlueprintDebugTraceGet(FBlueprintDebugService& Service)
		: FBlueprintDebugQueryTool(EBlueprintDebugQuery::Trace, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.trace.get");
	}
};

class FTool_BlueprintDebugBreakpointList final : public FBlueprintDebugQueryTool
{
public:
	explicit FTool_BlueprintDebugBreakpointList(FBlueprintDebugService& Service)
		: FBlueprintDebugQueryTool(EBlueprintDebugQuery::Breakpoints, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.breakpoint.list");
	}
};

class FTool_BlueprintDebugWatchList final : public FBlueprintDebugQueryTool
{
public:
	explicit FTool_BlueprintDebugWatchList(FBlueprintDebugService& Service)
		: FBlueprintDebugQueryTool(EBlueprintDebugQuery::Watches, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.watch.list");
	}
};

class FTool_BlueprintDebugWatchValueGet final : public FBlueprintDebugQueryTool
{
public:
	explicit FTool_BlueprintDebugWatchValueGet(FBlueprintDebugService& Service)
		: FBlueprintDebugQueryTool(EBlueprintDebugQuery::WatchValue, Service)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("blueprint.debug.watch.value.get");
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterBlueprintDebugQueryTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FBlueprintDebugService& Service)
{
	Registry.Register(MakeShared<FTool_BlueprintDebugSessionGet>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugTraceGet>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugBreakpointList>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugWatchList>(Service));
	Registry.Register(MakeShared<FTool_BlueprintDebugWatchValueGet>(Service));
}
}
