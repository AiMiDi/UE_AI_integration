#include "UEAIIntegrationSubsystem.h"

#include "Core/MCPExecutor.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/ProductionRuntimeController.h"
#include "Infrastructure/Runtime/BlueprintDebugService.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationServer.h"
#include "HAL/PlatformMisc.h"

// Constructor and destructor are out-of-line because the subsystem owns
// forward-declared unique pointers. Keeping construction here also prevents
// UHT's generated constructor from instantiating their cleanup paths while the
// pointee types are incomplete.
UUEAIIntegrationSubsystem::UUEAIIntegrationSubsystem() = default;
UUEAIIntegrationSubsystem::~UUEAIIntegrationSubsystem() = default;

namespace UEAIIntegrationTools
{
	void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
	void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry);
	void RegisterBlueprintAssetLifecycleTools(FMCPToolRegistry& Registry);
	void RegisterBlueprintAssetStateTools(FMCPToolRegistry& Registry);
	void RegisterBlueprintGraphTools(FMCPToolRegistry& Registry);
	void RegisterVariableTools(FMCPToolRegistry& Registry);
	void RegisterParamTools(FMCPToolRegistry& Registry);
	void RegisterInterfaceTools(FMCPToolRegistry& Registry);
	void RegisterDispatcherTools(FMCPToolRegistry& Registry);
	void RegisterComponentTools(FMCPToolRegistry& Registry);
	void RegisterSnapshotTools(FMCPToolRegistry& Registry);
	void RegisterValidationTools(FMCPToolRegistry& Registry);
	void RegisterDiscoveryTools(FMCPToolRegistry& Registry);
	void RegisterBlueprintAnalysisTools(
		FMCPToolRegistry& Registry,
		UEAIIntegration::Infrastructure::FBlueprintDebugService* DebugService);
	void RegisterBlueprintDebugQueryTools(
		FMCPToolRegistry& Registry,
		UEAIIntegration::Infrastructure::FBlueprintDebugService& Service);
	void RegisterBlueprintDebugCommandTools(
		FMCPToolRegistry& Registry,
		UEAIIntegration::Infrastructure::FBlueprintDebugService& Service);
	void RegisterUserTypeTools(FMCPToolRegistry& Registry);
	void RegisterDiffTools(FMCPToolRegistry& Registry);
	void RegisterContentAssetReadTools(FMCPToolRegistry& Registry);
	void RegisterContentAssetChangeTools(FMCPToolRegistry& Registry);
	void RegisterMaterialReadTools(FMCPToolRegistry& Registry);
	void RegisterMaterialMutationTools(FMCPToolRegistry& Registry);
	void RegisterAnimationTools(FMCPToolRegistry& Registry);
	void RegisterActorTools(FMCPToolRegistry& Registry);
	void RegisterPIETools(
		FMCPToolRegistry& Registry,
		UEAIIntegration::Infrastructure::FPIESessionController& Controller);
	void RegisterViewportTools(FMCPToolRegistry& Registry);
	void RegisterSequencerTools(FMCPToolRegistry& Registry);
	void RegisterBehaviorTreeTools(FMCPToolRegistry& Registry);
	void RegisterNavigationTools(FMCPToolRegistry& Registry);
	void RegisterDataTableTools(FMCPToolRegistry& Registry);
	void RegisterFoliageTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraTools(FMCPToolRegistry& Registry);
	void RegisterUITools(FMCPToolRegistry& Registry);
	void RegisterBuildTools(FMCPToolRegistry& Registry);
	void RegisterProductionRuntimeTools(
		FMCPToolRegistry& Registry,
		UEAIIntegration::Infrastructure::FProductionRuntimeController& Controller);
	void RegisterWorldGenTools(FMCPToolRegistry& Registry);
}

void UUEAIIntegrationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogTemp, Log, TEXT("[UE_AI_integration] Subsystem initializing API..."));

	PIEController =
		MakeShared<UEAIIntegration::Infrastructure::FPIESessionController>();
	BlueprintDebugService =
		MakeShared<UEAIIntegration::Infrastructure::FBlueprintDebugService>(
			*PIEController);
	Registry = MakeUnique<FMCPToolRegistry>();
	ProductionController =
		MakeShared<UEAIIntegration::Infrastructure::FProductionRuntimeController>(
			*Registry,
			*PIEController);

	Registry->BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(*Registry);
	UEAIIntegrationTools::RegisterBlueprintMutationTools(*Registry);
	UEAIIntegrationTools::RegisterBlueprintAssetLifecycleTools(*Registry);
	UEAIIntegrationTools::RegisterBlueprintAssetStateTools(*Registry);
	UEAIIntegrationTools::RegisterBlueprintGraphTools(*Registry);
	UEAIIntegrationTools::RegisterVariableTools(*Registry);
	UEAIIntegrationTools::RegisterParamTools(*Registry);
	UEAIIntegrationTools::RegisterInterfaceTools(*Registry);
	UEAIIntegrationTools::RegisterDispatcherTools(*Registry);
	UEAIIntegrationTools::RegisterComponentTools(*Registry);
	UEAIIntegrationTools::RegisterSnapshotTools(*Registry);
	UEAIIntegrationTools::RegisterValidationTools(*Registry);
	UEAIIntegrationTools::RegisterDiscoveryTools(*Registry);
	UEAIIntegrationTools::RegisterBlueprintAnalysisTools(
		*Registry,
		BlueprintDebugService.Get());
	UEAIIntegrationTools::RegisterBlueprintDebugQueryTools(
		*Registry,
		*BlueprintDebugService);
	UEAIIntegrationTools::RegisterBlueprintDebugCommandTools(
		*Registry,
		*BlueprintDebugService);
	UEAIIntegrationTools::RegisterDiffTools(*Registry);
	Registry->EndDomainRegistration();

	Registry->BeginDomainRegistration(TEXT("scene"));
	UEAIIntegrationTools::RegisterActorTools(*Registry);
	UEAIIntegrationTools::RegisterPIETools(*Registry, *PIEController);
	UEAIIntegrationTools::RegisterViewportTools(*Registry);
	UEAIIntegrationTools::RegisterNavigationTools(*Registry);
	UEAIIntegrationTools::RegisterFoliageTools(*Registry);
	UEAIIntegrationTools::RegisterWorldGenTools(*Registry);
	Registry->EndDomainRegistration();

	Registry->BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetReadTools(*Registry);
	UEAIIntegrationTools::RegisterContentAssetChangeTools(*Registry);
	UEAIIntegrationTools::RegisterUserTypeTools(*Registry);
	UEAIIntegrationTools::RegisterMaterialReadTools(*Registry);
	UEAIIntegrationTools::RegisterMaterialMutationTools(*Registry);
	UEAIIntegrationTools::RegisterDataTableTools(*Registry);
	UEAIIntegrationTools::RegisterNiagaraTools(*Registry);
	UEAIIntegrationTools::RegisterUITools(*Registry);
	Registry->EndDomainRegistration();

	Registry->BeginDomainRegistration(TEXT("animation"));
	UEAIIntegrationTools::RegisterAnimationTools(*Registry);
	Registry->EndDomainRegistration();

	Registry->BeginDomainRegistration(TEXT("ai"));
	UEAIIntegrationTools::RegisterBehaviorTreeTools(*Registry);
	Registry->EndDomainRegistration();

	Registry->BeginDomainRegistration(TEXT("production"));
	UEAIIntegrationTools::RegisterSequencerTools(*Registry);
	UEAIIntegrationTools::RegisterBuildTools(*Registry);
	UEAIIntegrationTools::RegisterProductionRuntimeTools(
		*Registry,
		*ProductionController);
	Registry->EndDomainRegistration();

	// A catalog mismatch degrades discovery/execution but must not hide the health endpoint.
	Registry->LoadCapabilityManifests();

	Executor = MakeUnique<FMCPExecutor>(*Registry);
	int32 ServerPort = 9847;
	const FString ConfiguredPort =
		FPlatformMisc::GetEnvironmentVariable(TEXT("UE_PORT"));
	if (!ConfiguredPort.IsEmpty())
	{
		const int32 ParsedPort = FCString::Atoi(*ConfiguredPort);
		if (ParsedPort > 0 && ParsedPort <= 65535)
		{
			ServerPort = ParsedPort;
		}
		else
		{
			UE_LOG(
				LogTemp,
				Warning,
				TEXT("[UE_AI_integration] Ignoring invalid UE_PORT value '%s'; using default port %d."),
				*ConfiguredPort,
				ServerPort);
		}
	}

	Server = MakeUnique<FUEAIIntegrationServer>(
		*Registry,
		*Executor,
		BlueprintDebugService.Get());
	if (Server->Start(ServerPort))
	{
		UE_LOG(
			LogTemp,
			Log,
			TEXT("[UE_AI_integration] Server started on port %d with %d/%d exact bindings (%s)."),
			Server->GetPort(),
			Registry->Num(),
			Registry->GetCapabilityCount(),
			Registry->IsReady() ? TEXT("ready") : TEXT("degraded"));
	}
	else
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("[UE_AI_integration] Failed to start server on port %d."),
			ServerPort);
	}
}

void UUEAIIntegrationSubsystem::Deinitialize()
{
	if (Server.IsValid())
	{
		Server->Stop();
		Server.Reset();
	}
	Executor.Reset();
	ProductionController.Reset();
	Registry.Reset();
	BlueprintDebugService.Reset();
	PIEController.Reset();
	Super::Deinitialize();
}

void UUEAIIntegrationSubsystem::Tick(float DeltaTime)
{
	if (PIEController.IsValid())
	{
		PIEController->Tick();
	}
	if (ProductionController.IsValid())
	{
		ProductionController->Tick(DeltaTime);
	}
	if (BlueprintDebugService.IsValid())
	{
		BlueprintDebugService->Tick();
	}
	if (Server.IsValid())
	{
		Server->Tick(DeltaTime);
	}
}

TStatId UUEAIIntegrationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UUEAIIntegrationSubsystem, STATGROUP_Tickables);
}
