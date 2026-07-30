#include "UEAIIntegrationModule.h"

#include "Infrastructure/StandalonePerformanceController.h"
#include "ToolMenus.h"
#include "UI/SUEAIStatusBar.h"

#define LOCTEXT_NAMESPACE "FUEAIIntegrationModule"

void FUEAIIntegrationModule::StartupModule()
{
	StandalonePerformanceController =
		MakeUnique<FStandalonePerformanceController>();
	const bool bPerformanceChild =
		StandalonePerformanceController->StartFromCommandLine();
	if (!bPerformanceChild)
	{
		StandalonePerformanceController.Reset();
		UToolMenus::RegisterStartupCallback(
			FSimpleMulticastDelegate::FDelegate::CreateRaw(
				this,
				&FUEAIIntegrationModule::RegisterMenus));
	}
	UE_LOG(LogTemp, Log, TEXT("[UE_AI_integration] Module started. Server will initialize via EditorSubsystem."));
}

void FUEAIIntegrationModule::ShutdownModule()
{
	StandalonePerformanceController.Reset();
	if (UToolMenus::TryGet())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}
	UE_LOG(LogTemp, Log, TEXT("[UE_AI_integration] Module shutdown."));
}

void FUEAIIntegrationModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* Menu =
		UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar"));
	if (!Menu)
	{
		return;
	}

	FToolMenuSection& Section = Menu->AddSection(
		TEXT("UEAIIntegration"),
		FText::GetEmpty());
	Section.AddEntry(FToolMenuEntry::InitWidget(
		TEXT("UEAIIntegrationStatus"),
		CreateUEAIStatusBarWidget(),
		FText::GetEmpty(),
		true,
		false));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUEAIIntegrationModule, UE_AI_integration)
