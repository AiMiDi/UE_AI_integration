#pragma once
#include "CoreMinimal.h"

class UMaterial;
class UMaterialExpressionCustom;
namespace UEAIIntegration::MaterialEditing
{
struct FMaterialSourceFingerprint
{
	FString Hash;
	TArray<FString> Issues;
	int32 FunctionCount = 0;
	int32 IncludeCount = 0;
	bool bComplete = true;
	bool bShaderCacheMismatch = false;
	// Weak, bounded candidates for mapping the actual compiler input. No assets retained.
	TArray<TWeakObjectPtr<UMaterialExpressionCustom>> CustomExpressions;
	bool bCustomExpressionsComplete = true;
};

// Bounded authored dependency traversal and literal Custom include closure.
// Cache comparison is reserved for validation; polling only reads disk/model data.
FMaterialSourceFingerprint CaptureMaterialSourceFingerprint(UMaterial* Material, bool bCompareShaderCache = false, bool bHashModel = true);
void RecordMaterialSourceEdit(UObject* Asset);
}
