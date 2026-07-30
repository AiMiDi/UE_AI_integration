#pragma once

#include "Modules/ModuleManager.h"

class FStandalonePerformanceController;

class FUEAIIntegrationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	TUniquePtr<FStandalonePerformanceController> StandalonePerformanceController;
};
