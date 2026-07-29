// Editor subsystem that owns the MCP server and ticks it on the game thread.
#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Tickable.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.generated.h"

class FMCPToolRegistry;
class FMCPExecutor;
namespace UEAIIntegration::Infrastructure
{
class FBlueprintDebugService;
class FClientActivityService;
class FPIESessionController;
class FProductionRuntimeController;
}

UCLASS()
class UUEAIIntegrationSubsystem : public UEditorSubsystem, public FTickableEditorObject
{
	GENERATED_BODY()

public:
	UUEAIIntegrationSubsystem();
	virtual ~UUEAIIntegrationSubsystem();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableEditorObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickable() const override
	{
		return ClientActivityService.IsValid();
	}

	FUEAIIntegrationServer* GetServer() const { return Server.Get(); }
	FMCPToolRegistry* GetRegistry() const { return Registry.Get(); }
	UEAIIntegration::Infrastructure::FClientActivityService*
	GetClientActivityService() const
	{
		return ClientActivityService.Get();
	}
	bool SetServerEnabled(bool bEnabled);
	bool RestartServer();
	bool IsServerEnabled() const;
	bool IsServerEnableRequested() const { return bServerEnableRequested; }
	int32 GetConfiguredPort() const { return ServerPort; }
#if WITH_DEV_AUTOMATION_TESTS
	UEAIIntegration::Infrastructure::FBlueprintDebugService*
	GetBlueprintDebugServiceForTesting() const
	{
		return BlueprintDebugService.Get();
	}
#endif

private:
	TSharedPtr<UEAIIntegration::Infrastructure::FPIESessionController> PIEController;
	TSharedPtr<UEAIIntegration::Infrastructure::FBlueprintDebugService>
		BlueprintDebugService;
	TSharedPtr<UEAIIntegration::Infrastructure::FProductionRuntimeController>
		ProductionController;
	TSharedPtr<UEAIIntegration::Infrastructure::FClientActivityService>
		ClientActivityService;
	TUniquePtr<FMCPToolRegistry> Registry;
	TUniquePtr<FMCPExecutor> Executor;
	TUniquePtr<FUEAIIntegrationServer> Server;
	int32 ServerPort = 9847;
	bool bServerEnableRequested = true;
};
