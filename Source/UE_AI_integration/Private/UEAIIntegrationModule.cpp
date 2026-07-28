#include "UEAIIntegrationModule.h"

#define LOCTEXT_NAMESPACE "FUEAIIntegrationModule"

void FUEAIIntegrationModule::StartupModule()
{
	UE_LOG(LogTemp, Log, TEXT("[UE_AI_integration] Module started. Server will initialize via EditorSubsystem."));
}

void FUEAIIntegrationModule::ShutdownModule()
{
	UE_LOG(LogTemp, Log, TEXT("[UE_AI_integration] Module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUEAIIntegrationModule, UE_AI_integration)
