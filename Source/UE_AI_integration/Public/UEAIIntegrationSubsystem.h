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
		return Server.IsValid() && Server->IsRunning();
	}

	FUEAIIntegrationServer* GetServer() const { return Server.Get(); }
	FMCPToolRegistry* GetRegistry() const { return Registry.Get(); }

private:
	TSharedPtr<UEAIIntegration::Infrastructure::FPIESessionController> PIEController;
	TSharedPtr<UEAIIntegration::Infrastructure::FProductionRuntimeController>
		ProductionController;
	TUniquePtr<FMCPToolRegistry> Registry;
	TUniquePtr<FMCPExecutor> Executor;
	TUniquePtr<FUEAIIntegrationServer> Server;
};
