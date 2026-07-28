#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
using UEAIIntegration::Infrastructure::FPIESessionController;
using UEAIIntegration::Infrastructure::FRuntimeSceneService;
using UEAIIntegration::Infrastructure::FRuntimeServiceResult;

FMCPToolResult ToRuntimeQueryToolResult(
	const FRuntimeServiceResult& RuntimeResult)
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

class FRuntimeQueryToolBase : public FMCPToolBase
{
public:
	explicit FRuntimeQueryToolBase(FPIESessionController& InController)
		: Service(InController.GetRuntimeService())
	{
	}

protected:
	FRuntimeSceneService& Service;
};

class FTool_ListWorldContexts final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.world.contexts.list"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.ListWorldContexts(Params));
	}
};

class FTool_FindRuntimeObjects final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.object.find"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.FindObjects(Params));
	}
};

class FTool_GetRuntimeObject final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.object.get"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.GetObject(Params));
	}
};

class FTool_GetRuntimeWidgetTree final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.widget.tree.get"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.GetWidgetTree(Params));
	}
};

class FTool_GetRuntimeWidgetState final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.widget.state.get"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.GetWidgetState(Params));
	}
};

class FTool_HitTestRuntimeWidget final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.widget.hit_test"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.HitTestWidget(Params));
	}
};

class FTool_GetRuntimePointerSequence final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override
	{
		return TEXT("scene.runtime.input.pointer_sequence.get");
	}
	virtual FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.GetPointerSequence(Params));
	}
};

class FTool_CaptureRuntimeViewport final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override
	{
		return TEXT("scene.runtime.viewport.capture");
	}
	virtual FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.CapturePIEViewport(Params));
	}
};

class FTool_StartRuntimeWait final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override
	{
		return TEXT("scene.runtime.wait.until");
	}
	virtual FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.StartWait(Params));
	}
};

class FTool_GetRuntimeWait final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override
	{
		return TEXT("scene.runtime.wait.get");
	}
	virtual FMCPToolResult Execute(
		const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.GetWait(Params));
	}
};

class FTool_ListRuntimeDelegates final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.delegate.list"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.ListDelegates(Params));
	}
};

class FTool_IsRuntimeDelegateBound final : public FRuntimeQueryToolBase
{
public:
	using FRuntimeQueryToolBase::FRuntimeQueryToolBase;
	virtual FString GetCapabilityId() const override { return TEXT("scene.runtime.delegate.is_bound"); }
	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ToRuntimeQueryToolResult(Service.IsDelegateBound(Params));
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneRuntimeQueryTools(
	FMCPToolRegistry& Registry,
	UEAIIntegration::Infrastructure::FPIESessionController& Controller)
{
	Registry.Register(MakeShared<FTool_ListWorldContexts>(Controller));
	Registry.Register(MakeShared<FTool_FindRuntimeObjects>(Controller));
	Registry.Register(MakeShared<FTool_GetRuntimeObject>(Controller));
	Registry.Register(MakeShared<FTool_GetRuntimeWidgetTree>(Controller));
	Registry.Register(MakeShared<FTool_GetRuntimeWidgetState>(Controller));
	Registry.Register(MakeShared<FTool_HitTestRuntimeWidget>(Controller));
	Registry.Register(MakeShared<FTool_GetRuntimePointerSequence>(Controller));
	Registry.Register(MakeShared<FTool_CaptureRuntimeViewport>(Controller));
	Registry.Register(MakeShared<FTool_StartRuntimeWait>(Controller));
	Registry.Register(MakeShared<FTool_GetRuntimeWait>(Controller));
	Registry.Register(MakeShared<FTool_ListRuntimeDelegates>(Controller));
	Registry.Register(MakeShared<FTool_IsRuntimeDelegateBound>(Controller));
}
}
