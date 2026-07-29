#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UEAIIntegrationEditorSettings.generated.h"

/** Per-user, per-project settings for the loopback Editor integration service. */
UCLASS(
	config=EditorPerProjectUserSettings,
	meta=(DisplayName="UE AI Integration"))
class UE_AI_INTEGRATION_API UUEAIIntegrationEditorSettings
	: public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(
		config,
		EditAnywhere,
		Category="Service",
		meta=(DisplayName="Enable Server"))
	bool bServerEnabled = true;

	UPROPERTY(
		config,
		EditAnywhere,
		Category="Service",
		meta=(ClampMin="1", ClampMax="65535", DisplayName="Port"))
	int32 Port = 9847;

	virtual FName GetCategoryName() const override
	{
		return TEXT("Plugins");
	}
};
