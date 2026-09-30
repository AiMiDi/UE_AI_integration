#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UMaterial;
class UMaterialExpressionCustom;

namespace UEAIIntegration::MaterialEditing
{
// UE 5.4 declares RebuildOutputs without exporting it from Engine. Mirror its
// small data-only update without invoking PostEditChange/compilation.
void RebuildCustomOutputs(UMaterialExpressionCustom* Custom);
void RecordMaterialCompileRequest(UMaterial* Material);
void NotifyMaterialSourceEdited(UObject* Asset);
// Before native resource invalidation: refresh shader file caches only if the
// explicit Custom include closure differs from disk or a source disappeared.
bool PrepareMaterialSourceValidation(UMaterial* Material);
// Caller has already invalidated/updated source. Submit only this resource.
TSharedRef<FJsonObject> CompleteMaterialValidation(UMaterial* Material, bool bWait);
// Reads current native compiler state. Never triggers or waits for compilation.
TSharedRef<FJsonObject> ReadDiagnostics(UMaterial* Material);
}
