#pragma once

#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "MaterialGraph/MaterialGraphSchema.h"

namespace UEAIIntegration::Infrastructure
{
inline bool TryCreateConnection(
	const UEdGraphSchema* Schema,
	UEdGraphPin* SourcePin,
	UEdGraphPin* TargetPin,
	bool bDeferAssetRefresh)
{
	if (bDeferAssetRefresh
		&& (Schema->GetClass() == UEdGraphSchema_K2::StaticClass()
			|| Schema->GetClass() == UMaterialGraphSchema::StaticClass()))
	{
		// These two schema overrides add an immediate asset/editor refresh.
		// Keep the base implementation's virtual validation, conversion/promotion,
		// replacement of existing links and pin notifications. Workflow performs
		// its asset refresh at finalization. Required node callbacks can still
		// notify (e.g. K2 input defaults). Custom schema overrides remain intact.
		return Schema->UEdGraphSchema::TryCreateConnection(SourcePin, TargetPin);
	}
	return Schema->TryCreateConnection(SourcePin, TargetPin);
}
}
