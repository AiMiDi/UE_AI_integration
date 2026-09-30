#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class FMaterialResource;
namespace UEAIIntegration::MaterialEditing
{
struct FMaterialSourceFingerprint;
bool ParseMaterialCompilerLocation(const FString& Message, FString& File, int32& Line, int32& Column);
struct FCustomDiagnosticSource
{
	FString AssetPath, NodeId, ExpressionPath, Code;
	bool bImplicitReturn = false;
};
struct FCustomDiagnosticRange
{
	int32 FirstLine = 0, LastLine = 0;
	TArray<int32> Candidates;
};
struct FMaterialDiagnosticSourceMap
{
	FString State = TEXT("compiler_source_unavailable");
	FString CompilerInputHash;
	TArray<FCustomDiagnosticSource> Sources;
	TArray<FCustomDiagnosticRange> Ranges;
};
// Uses only the actual pending compiler environment, never retranslates or compiles.
FMaterialDiagnosticSourceMap CaptureMaterialDiagnosticSourceMap(FMaterialResource* Resource, const FMaterialSourceFingerprint& Fingerprint);
// Separated for testing generated-source boundaries without launching a shader job.
FMaterialDiagnosticSourceMap BuildMaterialDiagnosticSourceMap(const FString& Generated, const FMaterialSourceFingerprint& Fingerprint);
void MapMaterialDiagnosticLocation(const FMaterialDiagnosticSourceMap& Map, const FString& File, int32 Line, int32 Column, bool bCurrentSource, const TSharedRef<FJsonObject>& Diagnostic);
}
