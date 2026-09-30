#include "Infrastructure/MaterialDiagnosticSourceMap.h"
#include "Infrastructure/MaterialSourceFingerprint.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/Sha256.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "MaterialShared.h"
#include "ShaderCore.h"
#include "Internationalization/Regex.h"

namespace UEAIIntegration::MaterialEditing
{
namespace
{
template<class T> const FSharedShaderCompilerEnvironment* PendingEnvironment(T* Resource)
{
	if constexpr (requires(T* Value) { Value->GetPendingMaterialCompilerEnvironment_GameThread(); })
		return Resource->GetPendingMaterialCompilerEnvironment_GameThread();
	else return nullptr;
}
FString CompilerInputHash(const FString& Generated)
{
	if (Generated.Len() > 2 * 1024 * 1024) return {};
	const FTCHARToUTF8 Utf8(*Generated); FString Hash;
	Infrastructure::TrySha256Hex(Utf8.Get(), Utf8.Length(), Hash); return Hash;
}
}
bool ParseMaterialCompilerLocation(const FString& Message, FString& File, int32& Line, int32& Column)
{
	// Explicit alternation avoids ICU interpreting an opening [: as a POSIX class.
	FRegexMatcher Location(FRegexPattern(TEXT("([^\\s()\"]+\\.(?:ush|usf|hlsl))(?::|\\()([0-9]{1,9})[,:]([0-9]{1,9})")), Message);
	if (!Location.FindNext()) return false;
	File = Location.GetCaptureGroup(1); Line = FCString::Atoi(*Location.GetCaptureGroup(2)); Column = FCString::Atoi(*Location.GetCaptureGroup(3));
	return Line > 0 && Column > 0;
}
FMaterialDiagnosticSourceMap BuildMaterialDiagnosticSourceMap(const FString& Generated, const FMaterialSourceFingerprint& Fingerprint)
{
	FMaterialDiagnosticSourceMap Result;
	if (!Fingerprint.bComplete) { Result.State = TEXT("dependency_check_incomplete"); return Result; }
	if (!Fingerprint.bCustomExpressionsComplete || Generated.Len() > 2 * 1024 * 1024) { Result.State = TEXT("source_map_budget"); return Result; }
	int32 TotalChars = 0;
	for (const auto& Weak : Fingerprint.CustomExpressions)
	{
		auto* Custom = Weak.Get();
		if (!Custom) { Result.State = TEXT("source_expression_unavailable"); return Result; }
		if ((TotalChars += Custom->Code.Len()) > 512 * 1024) { Result.State = TEXT("source_map_budget"); Result.Sources.Reset(); return Result; }
		FCustomDiagnosticSource Source;
		UObject* Owner = Custom->Function ? static_cast<UObject*>(Custom->Function.Get()) : static_cast<UObject*>(Custom->Material.Get());
		Source.AssetPath = GetPathNameSafe(Owner ? Owner : Custom->GetOuter());
		Source.NodeId = MCPMaterialInfrastructure::ExpressionNodeId(Custom);
		Source.ExpressionPath = Custom->GetPathName();
		Source.Code = Custom->Code.Replace(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
		// Match the legacy translator's implicit-return rule exactly. New generators
		// are mapped only if their actual emitted wrapper also matches this format.
		Source.bImplicitReturn = !Custom->Code.Contains(TEXT("return"));
		Result.Sources.Add(MoveTemp(Source));
	}
	// A #line can change the reported filename/line. Do not infer through one,
	// including conditional/user-authored directives. Includes restore parent lines.
	FRegexMatcher Directive(FRegexPattern(TEXT("(?m)^[ \\t]*#[ \\t]*(?:line\\b|[0-9])")), Generated);
	const int32 FirstDirective = Directive.FindNext() ? Directive.GetMatchBeginning() : Generated.Len();
	FRegexMatcher Wrapper(FRegexPattern(TEXT("(?m)^[A-Za-z_][A-Za-z0-9_]* CustomExpression[0-9]+\\(FMaterial(?:Pixel|Vertex)Parameters Parameters[^\\r\\n]*\\)\\n\\{\\n")), Generated);
	int32 Offset = 0, PhysicalLine = 1, WrapperCount = 0;
	while (Wrapper.FindNext())
	{
		if (++WrapperCount > 2048) { Result.State = TEXT("source_map_budget"); Result.Ranges.Reset(); return Result; }
		const int32 Start = Wrapper.GetMatchEnding();
		if (Start >= FirstDirective) break;
		while (Offset < Start) if (Generated[Offset++] == '\n') ++PhysicalLine;
		FCustomDiagnosticRange Range; Range.FirstLine = PhysicalLine;
		for (int32 Index = 0; Index < Result.Sources.Num(); ++Index)
		{
			const auto& Source = Result.Sources[Index];
			// Preprocessor directives/line splices need a preprocessor source map;
			// keep these unmapped instead of confusing expansion and authored lines.
			if (Source.Code.IsEmpty() || Source.Code.Contains(TEXT("#")) || Source.Code.Contains(TEXT("\\\n"))) continue;
			const FString Body = Source.bImplicitReturn ? TEXT("return ") + Source.Code + TEXT(";") : Source.Code;
			const FString Expected = Body + TEXT("\n}\n");
			if (Start + Expected.Len() > FirstDirective || FCString::Strncmp(*Generated + Start, *Expected, Expected.Len()) != 0) continue;
			Range.Candidates.Add(Index);
			Range.LastLine = Range.FirstLine;
			for (TCHAR Ch : Body) if (Ch == '\n') ++Range.LastLine;
		}
		if (!Range.Candidates.IsEmpty())
		{
			if (Result.Ranges.Num() >= 256) { Result.State = TEXT("source_map_budget"); Result.Ranges.Reset(); return Result; }
			Result.Ranges.Add(MoveTemp(Range));
		}
	}
	Result.State = Result.Ranges.IsEmpty() ? TEXT("no_supported_custom_ranges") : TEXT("captured");
	Result.CompilerInputHash = CompilerInputHash(Generated);
	return Result;
}

FMaterialDiagnosticSourceMap CaptureMaterialDiagnosticSourceMap(FMaterialResource* Resource, const FMaterialSourceFingerprint& Fingerprint)
{
	if (Resource)
		if (const auto* Environment = PendingEnvironment(Resource))
				if (const auto* Generated = Environment->IncludeVirtualPathToContentsMap.Find(TEXT("/Engine/Generated/Material.ush")))
					return BuildMaterialDiagnosticSourceMap(*Generated, Fingerprint);
	return {};
}

void MapMaterialDiagnosticLocation(const FMaterialDiagnosticSourceMap& Map, const FString& File, int32 Line, int32 Column, bool bCurrentSource, const TSharedRef<FJsonObject>& Diagnostic)
{
	Diagnostic->SetBoolField(TEXT("sourceLineMapped"), false);
	FString State = Map.State != TEXT("captured") ? Map.State : !bCurrentSource ? TEXT("source_not_current") : File != TEXT("/Engine/Generated/Material.ush") ? TEXT("external_compiler_file") : TEXT("outside_custom_body");
	TArray<TSharedPtr<FJsonValue>> Locations;
	int32 CandidateCount = 0;
	if (bCurrentSource && Map.State == TEXT("captured") && File == TEXT("/Engine/Generated/Material.ush") && Line > 0 && Column > 0)
	{
		for (const auto& Range : Map.Ranges)
		{
			if (Line < Range.FirstLine || Line > Range.LastLine) continue;
			for (int32 Index : Range.Candidates)
			{
				const auto& Source = Map.Sources[Index];
				const int32 SourceLine = Line - Range.FirstLine + 1;
				const int32 SourceColumn = Column - (Source.bImplicitReturn && SourceLine == 1 ? 7 : 0);
				TArray<FString> Lines; Source.Code.ParseIntoArray(Lines, TEXT("\n"), false);
				if (!Lines.IsValidIndex(SourceLine - 1) || SourceColumn < 1) continue;
				// Compiler column units/tab expansion vary. The line remains exact,
				// but only plain ASCII lines establish an authored character column.
				bool bColumnMapped = true;
				for (TCHAR Ch : Lines[SourceLine - 1]) if (Ch == '\t' || Ch > 127) bColumnMapped = false;
				if (bColumnMapped && SourceColumn > Lines[SourceLine - 1].Len()) continue;
				++CandidateCount;
				if (Locations.Num() >= 8) continue;
				auto Location = MakeShared<FJsonObject>();
				Location->SetStringField(TEXT("assetPath"), Source.AssetPath); Location->SetStringField(TEXT("nodeId"), Source.NodeId);
				Location->SetStringField(TEXT("expressionPath"), Source.ExpressionPath); Location->SetStringField(TEXT("space"), TEXT("customCode"));
				Location->SetNumberField(TEXT("line"), SourceLine); Location->SetBoolField(TEXT("columnMapped"), bColumnMapped);
				if (bColumnMapped) Location->SetNumberField(TEXT("column"), SourceColumn);
				else Location->SetField(TEXT("column"), MakeShared<FJsonValueNull>());
				Location->SetStringField(TEXT("evidence"), TEXT("exactBodyInActualCompilerInput"));
				Locations.Add(MakeShared<FJsonValueObject>(Location));
			}
		}
		State = CandidateCount == 1 ? TEXT("mapped") : CandidateCount > 1 ? TEXT("ambiguous_custom_body") : TEXT("outside_custom_body");
	}
	Diagnostic->SetStringField(TEXT("sourceMappingState"), State);
	Diagnostic->SetNumberField(TEXT("sourceLocationCandidateCount"), CandidateCount);
	Diagnostic->SetBoolField(TEXT("sourceLocationsTruncated"), CandidateCount > Locations.Num());
	Diagnostic->SetArrayField(TEXT("customSourceLocations"), Locations);
	if (CandidateCount == 1) { Diagnostic->SetBoolField(TEXT("sourceLineMapped"), true); Diagnostic->SetObjectField(TEXT("customSourceLocation"), Locations[0]->AsObject()); }
}
}
