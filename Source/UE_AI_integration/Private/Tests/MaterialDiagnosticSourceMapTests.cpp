#if WITH_DEV_AUTOMATION_TESTS
#include "Infrastructure/MaterialDiagnosticSourceMap.h"
#include "Infrastructure/MaterialSourceFingerprint.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialShared.h"
#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "RHI.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
using namespace UEAIIntegration::MaterialEditing;
template<class T> constexpr bool HasCompilerEnvironmentAccessor()
{
	return requires(T* Value) { Value->GetPendingMaterialCompilerEnvironment_GameThread(); };
}
template<class T> T* AddMappedExpression(UObject* Owner)
{
	auto* E = NewObject<T>(Owner);
	if (auto* M = Cast<UMaterial>(Owner)) { M->GetExpressionCollection().AddExpression(E); E->Material = M; }
	else { auto* F = CastChecked<UMaterialFunction>(Owner); F->GetExpressionCollection().AddExpression(E); E->Function = F; }
	return E;
}
TSharedRef<FJsonObject> Lookup(const FMaterialDiagnosticSourceMap& Map, int32 Line, int32 Column = 8, bool bCurrent = true, const FString& File = TEXT("/Engine/Generated/Material.ush"))
{
	auto Diagnostic = MakeShared<FJsonObject>(); MapMaterialDiagnosticLocation(Map, File, Line, Column, bCurrent, Diagnostic); return Diagnostic;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSourceMapBoundariesTest, "UE_AI_integration.MaterialSourceMap.Boundaries", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialSourceMapBoundariesTest::RunTest(const FString&)
{
	FString File; int32 Line = 0, Column = 0;
	TestTrue(TEXT("DXC colon location parses through ICU"), ParseMaterialCompilerLocation(TEXT("/Engine/Generated/Material.ush:3597:8: error: missing"), File, Line, Column));
	TestEqual(TEXT("Virtual compiler file retained"), File, FString(TEXT("/Engine/Generated/Material.ush")));
	TestEqual(TEXT("Compiler line parsed"), Line, 3597);
	TestTrue(TEXT("Parenthesized compiler location parses"), ParseMaterialCompilerLocation(TEXT("[SM5] /Project/test.hlsl(17,9): error"), File, Line, Column));
	TestEqual(TEXT("Compiler column parsed"), Column, 9);
	TestFalse(TEXT("Text without a location stays unmapped"), ParseMaterialCompilerLocation(TEXT("Custom missing input"), File, Line, Column));
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>());
	auto* C = AddMappedExpression<UMaterialExpressionCustom>(M.Get()); C->Code = TEXT("float v = 0.5;\r\nreturn MISSING + v;");
	const FString Prefix = TEXT("// template\nMaterialFloat CustomExpression0(FMaterialPixelParameters Parameters)\n{\n");
	const FString Generated = Prefix + TEXT("float v = 0.5;\nreturn MISSING + v;\n}\n#line 20\n");
	auto Map = BuildMaterialDiagnosticSourceMap(Generated, CaptureMaterialSourceFingerprint(M.Get()));
	TestEqual(TEXT("Actual body match captured"), Map.State, FString(TEXT("captured")));
	auto D = Lookup(Map, 5);
	TestTrue(TEXT("Unique code location mapped"), D->GetBoolField(TEXT("sourceLineMapped")));
	if (D->GetBoolField(TEXT("sourceLineMapped"))) TestEqual(TEXT("CRLF authored line"), D->GetObjectField(TEXT("customSourceLocation"))->GetNumberField(TEXT("line")), 2.0);
	TestFalse(TEXT("Stale source never mapped"), Lookup(Map, 5, 8, false)->GetBoolField(TEXT("sourceLineMapped")));
	TestFalse(TEXT("Include file not treated as generated source"), Lookup(Map, 5, 8, true, TEXT("/Project/test.ush"))->GetBoolField(TEXT("sourceLineMapped")));
	TestFalse(TEXT("Wrapper is not authored source"), Lookup(Map, 3)->GetBoolField(TEXT("sourceLineMapped")));
	TestFalse(TEXT("Closing brace is not authored source"), Lookup(Map, 6)->GetBoolField(TEXT("sourceLineMapped")));
	auto* Duplicate = AddMappedExpression<UMaterialExpressionCustom>(M.Get()); Duplicate->Code = C->Code;
	auto Ambiguous = Lookup(BuildMaterialDiagnosticSourceMap(Generated, CaptureMaterialSourceFingerprint(M.Get())), 5);
	TestEqual(TEXT("Identical disconnected nodes remain ambiguous"), Ambiguous->GetStringField(TEXT("sourceMappingState")), FString(TEXT("ambiguous_custom_body")));
	TestFalse(TEXT("Ambiguity never fabricated as exact mapping"), Ambiguous->GetBoolField(TEXT("sourceLineMapped")));
	TestEqual(TEXT("Both candidates exposed"), Ambiguous->GetNumberField(TEXT("sourceLocationCandidateCount")), 2.0);
	Duplicate->Code = TEXT("return 1.0;"); C->Code = TEXT("MISSING");
	Map = BuildMaterialDiagnosticSourceMap(Prefix + TEXT("return MISSING;\n}\n"), CaptureMaterialSourceFingerprint(M.Get()));
	auto Implicit = Lookup(Map, 4);
	TestTrue(TEXT("Implicit return maps expression"), Implicit->GetBoolField(TEXT("sourceLineMapped")));
	if (Implicit->GetBoolField(TEXT("sourceLineMapped"))) TestEqual(TEXT("Generated return prefix excluded from column"), Implicit->GetObjectField(TEXT("customSourceLocation"))->GetNumberField(TEXT("column")), 1.0);
	TestFalse(TEXT("Generated prefix is not source"), Lookup(Map, 4, 2)->GetBoolField(TEXT("sourceLineMapped")));
	TestFalse(TEXT("Generated semicolon is not source"), Lookup(Map, 4, 15)->GetBoolField(TEXT("sourceLineMapped")));
	Map = BuildMaterialDiagnosticSourceMap(TEXT("#line 400 \"somewhere.ush\"\n") + Prefix + TEXT("return MISSING;\n}\n"), CaptureMaterialSourceFingerprint(M.Get()));
	TestTrue(TEXT("No guessing through line directives"), Map.Ranges.IsEmpty());
	C->Code = TEXT("#define X MISSING\nreturn X;");
	Map = BuildMaterialDiagnosticSourceMap(Prefix + C->Code + TEXT("\n}\n"), CaptureMaterialSourceFingerprint(M.Get()));
	TestTrue(TEXT("Authored preprocessor directives are left unmapped"), Map.Ranges.IsEmpty());
	C->Code = TEXT("\treturn MISSING;");
	Map = BuildMaterialDiagnosticSourceMap(Prefix + C->Code + TEXT("\n}\n"), CaptureMaterialSourceFingerprint(M.Get()));
	const auto Tab = Lookup(Map, 4, 9);
	TestTrue(TEXT("Tab line can still map"), Tab->GetBoolField(TEXT("sourceLineMapped")));
	if (Tab->GetBoolField(TEXT("sourceLineMapped"))) TestFalse(TEXT("Tab expansion does not invent a character column"), Tab->GetObjectField(TEXT("customSourceLocation"))->GetBoolField(TEXT("columnMapped")));
	auto Fingerprint = CaptureMaterialSourceFingerprint(M.Get()); Fingerprint.bCustomExpressionsComplete = false;
	TestEqual(TEXT("Candidate budget cannot create false uniqueness"), BuildMaterialDiagnosticSourceMap(Generated, Fingerprint).State, FString(TEXT("source_map_budget")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialSourceMapCompilerTest, "UE_AI_integration.MaterialSourceMap.CompilerCorrection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FMaterialSourceMapCompilerTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Source map compiler acceptance requires rendering.")); return true; }
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>()); M->SetShadingModel(MSM_Unlit);
	TStrongObjectPtr<UMaterialFunction> F(NewObject<UMaterialFunction>());
	auto* C = AddMappedExpression<UMaterialExpressionCustom>(F.Get()); C->Inputs.Reset(); C->Code = TEXT("float local = 0.25;\nreturn UEAI_MAP_UNDECLARED + local;");
	auto* Output = AddMappedExpression<UMaterialExpressionFunctionOutput>(F.Get()); Output->OutputName = TEXT("MappedValue"); Output->A.Connect(0, C);
	auto* Call = AddMappedExpression<UMaterialExpressionMaterialFunctionCall>(M.Get()); Call->SetMaterialFunction(F.Get());
	M->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Call);
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("material"), M->GetPathName()); Params->SetBoolField(TEXT("waitForCompilation"), true);
	const auto Bad = Registry->FindTool(TEXT("content.material.validate"))->Execute(Params);
	if (!TestTrue(TEXT("Native invalid HLSL validation completed"), Bad.bSuccess)) return false;
	TestEqual(TEXT("Invalid HLSL rejected"), Bad.Data->GetStringField(TEXT("compileState")), FString(TEXT("failed")));
	if constexpr (!HasCompilerEnvironmentAccessor<FMaterialResource>())
	{
		// The engine accessor is optional. Verify the supported fallback without
		// claiming that this run exercised actual-input source mapping.
		TestEqual(TEXT("Missing accessor explicitly reports unavailable mapping"), Bad.Data->GetStringField(TEXT("sourceMapState")), FString(TEXT("compiler_source_unavailable")));
		TestFalse(TEXT("Missing accessor cannot claim a current source map"), Bad.Data->GetBoolField(TEXT("diagnosticSourceMapCurrent")));
		TestTrue(TEXT("Compiler errors remain available without the accessor"), Bad.Data->GetNumberField(TEXT("errorCount")) > 0);
		for (const auto& Value : Bad.Data->GetArrayField(TEXT("diagnostics")))
			TestFalse(TEXT("Unavailable input never invents authored lines"), Value->AsObject()->GetBoolField(TEXT("sourceLineMapped")));
		C->Code = TEXT("return 0.5;");
		const auto Fixed = Registry->FindTool(TEXT("content.material.validate"))->Execute(Params);
		TestTrue(TEXT("Correction compiles without source-map accessor"), Fixed.bSuccess && Fixed.Data->GetBoolField(TEXT("valid")));
		AddWarning(TEXT("Optional engine compiler-environment accessor is absent: fallback diagnostics and correction verified; native source-line mapping NOT validated."));
		return true;
	}
	TestEqual(TEXT("Actual compiler environment captured"), Bad.Data->GetStringField(TEXT("sourceMapState")), FString(TEXT("captured")));
	TestTrue(TEXT("Saved source map still matches actual input after compilation"), Bad.Data->GetBoolField(TEXT("diagnosticSourceMapCurrent")));
	bool bMapped = false;
	for (const auto& Value : Bad.Data->GetArrayField(TEXT("diagnostics")))
	{
		const auto D = Value->AsObject();
		if (!D->GetStringField(TEXT("message")).Contains(TEXT("UEAI_MAP_UNDECLARED")) || !D->GetBoolField(TEXT("sourceLineMapped"))) continue;
		bMapped = true; const auto Location = D->GetObjectField(TEXT("customSourceLocation"));
		TestEqual(TEXT("Compiler error maps to function asset"), Location->GetStringField(TEXT("assetPath")), F->GetPathName());
		TestEqual(TEXT("Compiler error maps to Custom expression"), Location->GetStringField(TEXT("expressionPath")), C->GetPathName());
		TestEqual(TEXT("Compiler error maps authored line two"), Location->GetNumberField(TEXT("line")), 2.0);
		TestEqual(TEXT("Compiler column retained"), Location->GetNumberField(TEXT("column")), 8.0);
	}
	TestTrue(TEXT("At least one real shader diagnostic maps exactly"), bMapped);
	if (!bMapped) { FString Json; const auto Writer = TJsonWriterFactory<>::Create(&Json); FJsonSerializer::Serialize(Bad.Data.ToSharedRef(), Writer); AddInfo(Json); }
	// Recompile through UE directly, without changing authored code or the tool's
	// fingerprint. Its completion must retire the preceding tool-owned source map.
	M->ForceRecompileForRendering();
	if (auto* Resource = M->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel))) Resource->FinishCompilation();
	const auto NativeRecompile = ReadDiagnostics(M.Get());
	TestEqual(TEXT("Native recompile retires the prior source map"), NativeRecompile->GetStringField(TEXT("sourceMapState")), FString(TEXT("native_recompile_since_capture")));
	TestFalse(TEXT("Old input map is not current after native recompile"), NativeRecompile->GetBoolField(TEXT("diagnosticSourceMapCurrent")));
	C->Code = TEXT("return 0.5;");
	const auto Stale = ReadDiagnostics(M.Get());
	TestFalse(TEXT("Diagnostic polling did not compile"), Stale->GetBoolField(TEXT("compileTriggered")));
	for (const auto& Value : Stale->GetArrayField(TEXT("diagnostics"))) TestFalse(TEXT("Old error is not mapped onto edited source"), Value->AsObject()->GetBoolField(TEXT("sourceLineMapped")));
	const auto Fixed = Registry->FindTool(TEXT("content.material.validate"))->Execute(Params);
	TestTrue(TEXT("Correction really compiles"), Fixed.bSuccess && Fixed.Data->GetBoolField(TEXT("valid")));
	C->Code = TEXT("UEAI_MAP_IMPLICIT"); Params->SetBoolField(TEXT("waitForCompilation"), false);
	const auto Pending = Registry->FindTool(TEXT("content.material.validate"))->Execute(Params);
	TestTrue(TEXT("Async validation dispatched"), Pending.bSuccess);
	if (auto* Resource = M->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel))) Resource->FinishCompilation();
	const auto Implicit = ReadDiagnostics(M.Get()); bool bImplicitMapped = false;
	for (const auto& Value : Implicit->GetArrayField(TEXT("diagnostics")))
	{
		const auto D = Value->AsObject();
		if (!D->GetBoolField(TEXT("sourceLineMapped"))) continue;
		bImplicitMapped = true; const auto L = D->GetObjectField(TEXT("customSourceLocation"));
		TestEqual(TEXT("Async implicit return source line"), L->GetNumberField(TEXT("line")), 1.0);
		TestEqual(TEXT("Async implicit return source column"), L->GetNumberField(TEXT("column")), 1.0);
	}
	TestTrue(TEXT("Async result retains original source map"), bImplicitMapped);
	auto* Duplicate = AddMappedExpression<UMaterialExpressionCustom>(F.Get()); Duplicate->Inputs.Reset(); Duplicate->Code = C->Code;
	Params->SetBoolField(TEXT("waitForCompilation"), true);
	const auto Ambiguous = Registry->FindTool(TEXT("content.material.validate"))->Execute(Params);
	if (!TestTrue(TEXT("Duplicate-code material validation completed"), Ambiguous.bSuccess)) return false;
	bool bAmbiguous = false;
	for (const auto& Value : Ambiguous.Data->GetArrayField(TEXT("diagnostics")))
	{
		const auto D = Value->AsObject(); TestFalse(TEXT("Duplicate code never selects a node"), D->GetBoolField(TEXT("sourceLineMapped")));
		if (D->GetStringField(TEXT("sourceMappingState")) == TEXT("ambiguous_custom_body"))
		{
			bAmbiguous = true; TestEqual(TEXT("Both actual authored candidates returned"), D->GetNumberField(TEXT("sourceLocationCandidateCount")), 2.0);
		}
	}
	TestTrue(TEXT("Actual compilation preserves duplicate-body ambiguity"), bAmbiguous);
	C->Code = Duplicate->Code = TEXT("return 0.75;");
	const auto Final = Registry->FindTool(TEXT("content.material.validate"))->Execute(Params);
	TestTrue(TEXT("Final correction compiles successfully"), Final.bSuccess && Final.Data->GetBoolField(TEXT("valid")));
	return true;
}
#endif
