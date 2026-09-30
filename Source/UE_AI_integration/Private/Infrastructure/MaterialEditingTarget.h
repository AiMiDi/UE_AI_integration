#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Tools/MCPToolBase.h"

class IMaterialEditor;
class UMaterial;
class UMaterialFunction;
class UMaterialExpression;
class UMaterialGraph;

namespace UEAIIntegration::MaterialEditing
{
// One explicit authored-asset or open-editor working-copy target.
struct FTarget
{
    UObject* Asset = nullptr;
    UObject* OriginalAsset = nullptr;
    UMaterial* Material = nullptr;
    UMaterialFunction* Function = nullptr;
    TSharedPtr<IMaterialEditor> Editor;
    TUniquePtr<FScopedTransaction> Transaction;
    TArray<UMaterialExpression*> Expressions;
    UMaterialGraph* Graph() const;
    UMaterialExpression* Find(const FString& Id) const;
};

FString PreviewId(const FTarget& Target);
bool ResolvePreview(FTarget& Out, FString& Error);
bool Resolve(const TSharedPtr<FJsonObject>& Params, FTarget& Out, FString& Error, bool bMutating = false);
void BeginEdit(FTarget& Target);
void DescribeTarget(const FTarget& Target, const TSharedRef<FJsonObject>& Result);
void FinishEdit(const FTarget& Target, UMaterialExpression* Expression, const TSharedPtr<FJsonObject>& Params, const TSharedRef<FJsonObject>& Result);
bool ValidatePreviewDependencies(const FTarget& Target, FString& Error, const TCHAR*& ErrorCode);
// Complete a native update that has already refreshed the base preview material.
TSharedRef<FJsonObject> CompletePreviewUpdate(const FTarget& Target, bool bWait);

inline bool RoutesToPreview(const TSharedPtr<FJsonObject>& Params)
{
    FString Context;
    return Params->HasField(TEXT("expectedPreviewId")) || (Params->TryGetStringField(TEXT("targetContext"), Context) && Context != TEXT("asset"));
}
FMCPToolResult MutatePreviewGraph(const FString& Capability, const TSharedPtr<FJsonObject>& Params);
}
