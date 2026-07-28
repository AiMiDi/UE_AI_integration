#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
using UEAIIntegration::Infrastructure::FPIESessionController;
using UEAIIntegration::Infrastructure::FRuntimeSceneService;
using UEAIIntegration::Infrastructure::FRuntimeServiceResult;

FMCPToolResult ToToolResult(const FRuntimeServiceResult& RuntimeResult)
{
	if (!RuntimeResult.bSuccess)
	{
		return FMCPToolResult::Error(
			RuntimeResult.ErrorMessage,
			RuntimeResult.ErrorCode,
			RuntimeResult.HttpStatus);
	}
	return FMCPToolResult::Ok(
		RuntimeResult.Data.IsValid()
			? RuntimeResult.Data
			: MakeShared<FJsonObject>());
}

class FRuntimeCommandToolBase : public FMCPToolBase
{
public:
	explicit FRuntimeCommandToolBase(FPIESessionController& InController)
		: Service(InController.GetRuntimeService())
	{
	}

protected:
	FRuntimeSceneService& Service;
};

class FTool_SetRuntimeObject final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.object.set"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.SetObject(Params));
	}
};

class FTool_CallRuntimeObject final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.object.call"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.CallObject(Params));
	}
};

class FTool_FocusRuntimeWidget final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.widget.focus.set"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.FocusWidget(Params));
	}
};

class FTool_BindRuntimeDelegate final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.delegate.bind"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.BindDelegate(Params));
	}
};

class FTool_UnbindRuntimeDelegate final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.delegate.unbind"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.UnbindDelegate(Params));
	}
};

class FTool_BroadcastRuntimeDelegate final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.delegate.broadcast"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.BroadcastDelegate(Params));
	}
};

class FTool_RuntimePointerInput final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.input.pointer"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.PointerInput(Params));
	}
};

class FTool_RuntimeKeyInput final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.input.key"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.KeyInput(Params));
	}
};

class FTool_SetRuntimeInputMode final : public FRuntimeCommandToolBase
{
public:
	using FRuntimeCommandToolBase::FRuntimeCommandToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.input.mode.set"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToToolResult(Service.SetInputMode(Params));
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneRuntimeCommandTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller)
{
	Registry.Register(MakeShared<FTool_SetRuntimeObject>(Controller));
	Registry.Register(MakeShared<FTool_CallRuntimeObject>(Controller));
	Registry.Register(MakeShared<FTool_FocusRuntimeWidget>(Controller));
	Registry.Register(MakeShared<FTool_BindRuntimeDelegate>(Controller));
	Registry.Register(MakeShared<FTool_UnbindRuntimeDelegate>(Controller));
	Registry.Register(MakeShared<FTool_BroadcastRuntimeDelegate>(Controller));
	Registry.Register(MakeShared<FTool_RuntimePointerInput>(Controller));
	Registry.Register(MakeShared<FTool_RuntimeKeyInput>(Controller));
	Registry.Register(MakeShared<FTool_SetRuntimeInputMode>(Controller));
}
}
