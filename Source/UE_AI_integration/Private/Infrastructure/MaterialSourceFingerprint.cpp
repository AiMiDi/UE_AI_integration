#include "Infrastructure/MaterialSourceFingerprint.h"
#include "Infrastructure/Sha256.h"
#include "Workflow/UEWorkflowRuntime.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"
#include "ShaderCore.h"
#include "ShaderCompilerCore.h"
#include "RHI.h"
#include "UObject/UnrealType.h"

namespace UEAIIntegration::MaterialEditing
{
namespace
{
constexpr int32 MaxFunctions = 128, MaxExpressions = 20000, MaxIncludes = 256;
constexpr int64 MaxFileBytes = 512 * 1024, MaxIncludeBytes = 4 * 1024 * 1024;
TMap<TWeakObjectPtr<UObject>, uint64> EditRevisions;
uint64 NextRevision = 0, TrackingGeneration = 0;

FString HashText(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text); FString Hash;
	Infrastructure::TrySha256Hex(Utf8.Get(), Utf8.Length(), Hash); return Hash;
}

// Splice physical lines and strip comments without treating comment markers in
// string literals as comments. Conditional branches are conservatively included.
FString IncludeDirectives(FString Text)
{
	Text.ReplaceInline(TEXT("\\\r\n"), TEXT("")); Text.ReplaceInline(TEXT("\\\n"), TEXT(""));
	bool bBlock = false, bLine = false; TCHAR Quote = 0;
	for (int32 I = 0; I < Text.Len(); ++I)
	{
		const TCHAR C = Text[I], Next = I + 1 < Text.Len() ? Text[I + 1] : 0;
		if (bLine) { if (C == '\n') bLine = false; else Text[I] = ' '; continue; }
		if (bBlock)
		{
			if (C == '*' && Next == '/') { Text[I] = Text[I + 1] = ' '; ++I; bBlock = false; }
			else if (C != '\n' && C != '\r') Text[I] = ' ';
			continue;
		}
		if (Quote) { if (C == '\\') ++I; else if (C == Quote) Quote = 0; continue; }
		if (C == '"' || C == '\'') { Quote = C; continue; }
		if (C == '/' && (Next == '/' || Next == '*')) { Text[I] = Text[I + 1] = ' '; ++I; bLine = Next == '/'; bBlock = Next == '*'; }
	}
	return Text;
}
}

void RecordMaterialSourceEdit(UObject* Asset)
{
	if (!Asset) return;
	for (auto It = EditRevisions.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
	// Rare capacity eviction is conservatively visible to all stored fingerprints.
	if (EditRevisions.Num() >= 1024 && !EditRevisions.Contains(TWeakObjectPtr<UObject>(Asset))) { EditRevisions.Reset(); ++TrackingGeneration; }
	EditRevisions.Add(TWeakObjectPtr<UObject>(Asset), ++NextRevision);
}

FMaterialSourceFingerprint CaptureMaterialSourceFingerprint(UMaterial* Material, bool bCompareShaderCache, bool bHashModel)
{
	FMaterialSourceFingerprint Result;
	auto Issue = [&](const FString& Reason) { Result.bComplete = false; if (Result.Issues.Num() < 16) Result.Issues.AddUnique(Reason.Left(512)); };
	if (!Material) { Issue(TEXT("material_unavailable")); return Result; }
	TArray<FString> Parts, IncludeQueue; TSet<FString> SeenIncludes;
	Parts.Add(FString::Printf(TEXT("tracking:%llu"), TrackingGeneration));
	const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel);
	Parts.Add(FString::Printf(TEXT("platform:%d"), static_cast<int32>(ShaderPlatform)));
	auto AddInclude = [&](FString Path, const FString& From)
	{
		if (Path.Len() > 2048) { Issue(TEXT("include_path_budget")); return; }
		if (!Path.StartsWith(TEXT("/"))) Path = FPaths::GetPath(From) / Path;
		if (!FPaths::CollapseRelativeDirectories(Path) || !Path.StartsWith(TEXT("/"))) { Issue(TEXT("invalid_include_path:" ) + Path); return; }
		ReplaceVirtualFilePathForShaderPlatform(Path, ShaderPlatform);
		ReplaceVirtualFilePathForShaderAutogen(Path, ShaderPlatform);
		if (SeenIncludes.Contains(Path)) return;
		if (SeenIncludes.Num() >= MaxIncludes) { Issue(TEXT("include_count_budget")); return; }
		SeenIncludes.Add(Path); IncludeQueue.Add(Path);
	};
	auto ScanIncludes = [&](const FString& Code, const FString& From)
	{
		const FString Clean = IncludeDirectives(Code);
		FRegexMatcher Match(FRegexPattern(TEXT("(?m)^[\\t ]*#[\\t ]*(include_next|include)\\b([^\\r\\n]*)")), Clean);
		while (Match.FindNext())
		{
			FString Tail = Match.GetCaptureGroup(2).TrimStartAndEnd();
			if (Match.GetCaptureGroup(1) != TEXT("include") || Tail.IsEmpty() || (Tail[0] != '"' && Tail[0] != '<')) { Issue(TEXT("nonliteral_include:") + From); continue; }
			const TCHAR End = Tail[0] == '"' ? '"' : '>'; int32 Close = 1;
			while (Close < Tail.Len() && Tail[Close] != End) ++Close;
			if (Close == Tail.Len() || Close == 1 || !Tail.Mid(Close + 1).TrimStartAndEnd().IsEmpty()) { Issue(TEXT("unsupported_include_directive:") + From); continue; }
			AddInclude(Tail.Mid(1, Close - 1), From);
		}
	};
	TArray<UObject*> Owners{Material}; TSet<UObject*> SeenOwners{Material};
	TMap<UObject*, TArray<UObject*>> Edges; TMap<UObject*, int32> Indegree;
	int32 ExpressionCount = 0; int64 CustomChars = 0;
	for (int32 Index = 0; Index < Owners.Num(); ++Index)
	{
		auto* Owner = Owners[Index]; Indegree.FindOrAdd(Owner);
		Parts.Add(Owner->GetPathName() + FString::Printf(TEXT(":edit:%llu"), EditRevisions.FindRef(TWeakObjectPtr<UObject>(Owner))));
		auto AddFunction = [&](UObject* Child)
		{
			if (!Child) { Issue(TEXT("missing_function_parent")); return; }
			if (!SeenOwners.Contains(Child))
			{
				if (SeenOwners.Num() >= MaxFunctions + 1) { Issue(TEXT("function_count_budget")); return; }
				SeenOwners.Add(Child); Owners.Add(Child);
			}
			Edges.FindOrAdd(Owner).Add(Child); ++Indegree.FindOrAdd(Child);
		};
		if (auto* Instance = Cast<UMaterialFunctionInstance>(Owner))
		{
			AddFunction(Instance->Parent); FString Values;
			if (!bHashModel) continue;
			for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Transient)) continue;
				if (auto* Array = CastField<FArrayProperty>(*It); Array && FScriptArrayHelper(Array, Array->ContainerPtrToValuePtr<void>(Instance)).Num() > 4096) { Issue(TEXT("function_instance_parameter_budget")); break; }
				FString Value; It->ExportTextItem_Direct(Value, It->ContainerPtrToValuePtr<void>(Instance), nullptr, Instance, PPF_None);
				Values += It->GetName() + TEXT("=") + Value + TEXT("\n");
				if (Values.Len() > 1024 * 1024) { Issue(TEXT("function_instance_text_budget")); break; }
			}
			Parts.Add(Owner->GetPathName() + TEXT(":") + HashText(Values)); continue;
		}
		TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions;
		if (auto* M = Cast<UMaterial>(Owner)) Expressions = M->GetExpressions();
		else if (auto* F = Cast<UMaterialFunction>(Owner)) Expressions = F->GetExpressions();
		else { Issue(TEXT("unsupported_function_class:") + Owner->GetClass()->GetName()); continue; }
		if ((ExpressionCount += Expressions.Num()) > MaxExpressions) { Issue(TEXT("expression_count_budget")); break; }
		for (UMaterialExpression* Expression : Expressions)
		{
			if (!Expression) continue;
			if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Expression); Call && Call->MaterialFunction) AddFunction(Call->MaterialFunction);
			if (auto* Layers = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression))
			{
				if (Layers->GetLayers().Num() + Layers->GetBlends().Num() > MaxFunctions) { Issue(TEXT("layer_function_count_budget")); continue; }
				FString ActiveLayers;
				for (auto* Function : Layers->GetLayers()) { if (Function) AddFunction(Function); ActiveLayers += TEXT("L:") + GetPathNameSafe(Function) + TEXT("\n"); }
				for (auto* Function : Layers->GetBlends()) { if (Function) AddFunction(Function); ActiveLayers += TEXT("B:") + GetPathNameSafe(Function) + TEXT("\n"); }
				for (bool State : Layers->GetLayerStates()) ActiveLayers += State ? TEXT("1") : TEXT("0");
				Parts.Add(Expression->GetPathName() + TEXT(":layers:") + HashText(ActiveLayers));
			}
			if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression))
			{
				if (Result.CustomExpressions.Num() < 128) Result.CustomExpressions.Add(Custom);
				else Result.bCustomExpressionsComplete = false;
				CustomChars += Custom->Code.Len();
				if (CustomChars > 1024 * 1024) { Issue(TEXT("custom_source_budget")); break; }
				ScanIncludes(Custom->Code, TEXT("/Engine/Generated/Material.ush"));
				if (Custom->IncludeFilePaths.Num() > MaxIncludes) { Issue(TEXT("include_count_budget")); break; }
				for (const auto& Path : Custom->IncludeFilePaths) AddInclude(Path, TEXT("/Engine/Generated/Material.ush"));
			}
		}
		if (CustomChars > 1024 * 1024) break;
		if (bHashModel) Parts.Add(Owner->GetPathName() + TEXT(":") + Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Owner));
	}
	Result.FunctionCount = Owners.Num() - 1;
	TArray<UObject*> Ready; for (const auto& Pair : Indegree) if (!Pair.Value) Ready.Add(Pair.Key);
	for (int32 Index = 0; Index < Ready.Num(); ++Index) if (const auto* Children = Edges.Find(Ready[Index])) for (auto* Child : *Children) if (--Indegree.FindChecked(Child) == 0) Ready.Add(Child);
	if (Ready.Num() != Owners.Num()) Issue(TEXT("function_dependency_cycle_or_incomplete_traversal"));
	int64 TotalBytes = 0;
	for (int32 Index = 0; Index < IncludeQueue.Num(); ++Index)
	{
		const FString Path = IncludeQueue[Index]; TArray<FShaderCompilerError> Errors;
		// GetShaderSourceFilePath logs global errors even with an error array.
		// Resolve the same longest registered directory here so a read-only source
		// check can report unavailable dependencies without polluting editor logs.
		FString Directory = FPaths::GetPath(Path), Relative = FPaths::GetCleanFilename(Path), Filename;
		while (!Directory.IsEmpty())
		{
			if (const auto* Mapped = AllShaderSourceDirectoryMappings().Find(Directory)) { Filename = *Mapped / Relative; break; }
			Relative = FPaths::GetCleanFilename(Directory) / Relative; Directory = FPaths::GetPath(Directory);
		}
		TUniquePtr<FArchive> Reader(Filename.IsEmpty() ? nullptr : IFileManager::Get().CreateFileReader(*Filename, FILEREAD_Silent));
		if (!Reader) { Issue(TEXT("include_unavailable:") + Path); Parts.Add(Path + TEXT(":missing")); Result.bShaderCacheMismatch |= bCompareShaderCache; continue; }
		const int64 Size = Reader->TotalSize();
		if (Size < 0 || Size > MaxFileBytes || TotalBytes + Size > MaxIncludeBytes) { Issue(TEXT("include_byte_budget:") + Path); continue; }
		TotalBytes += Size; TArray<uint8> Bytes; Bytes.SetNumUninitialized(static_cast<int32>(Size));
		Reader->Serialize(Bytes.GetData(), Bytes.Num());
		if (Reader->IsError()) { Issue(TEXT("include_read_failed:") + Path); continue; }
		FString Contents; FFileHelper::BufferToString(Contents, Bytes.GetData(), Bytes.Num());
		Parts.Add(Path + TEXT(":") + HashText(Contents)); ScanIncludes(Contents, Path);
		if (bCompareShaderCache)
		{
			FString Cached;
			if (!LoadShaderSourceFile(*Path, ShaderPlatform, &Cached, &Errors)) Issue(TEXT("shader_source_cache_unavailable:") + Path);
			else if (Cached != Contents) { Result.bShaderCacheMismatch = true; Issue(TEXT("shader_source_cache_differs_from_disk:") + Path); }
		}
	}
	Result.IncludeCount = IncludeQueue.Num(); Parts.Sort();
	Result.Hash = HashText(FString::Join(Parts, TEXT("\n")));
	if (Result.Hash.IsEmpty()) Issue(TEXT("source_hash_unavailable"));
	return Result;
}
}
