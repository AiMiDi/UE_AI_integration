#pragma once

#include "Engine/DataAsset.h"
#include "UEAIEmptyDataAsset.generated.h"

/**
 * Concrete persistence target for the generic UDataAsset creation contract.
 *
 * UDataAsset is intentionally abstract in Unreal. It can be allocated under
 * FScopedAllowAbstractClassAllocation, but the package saver will null the
 * object on disk. This class supplies the empty schema used when callers
 * explicitly request UDataAsset while keeping the public request independent
 * from the concrete storage class.
 */
UCLASS(BlueprintType, MinimalAPI)
class UUEAIEmptyDataAsset final : public UDataAsset
{
	GENERATED_BODY()
};
