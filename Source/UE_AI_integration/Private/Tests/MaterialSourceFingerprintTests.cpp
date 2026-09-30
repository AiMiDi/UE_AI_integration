#if WITH_DEV_AUTOMATION_TESTS
#include "Infrastructure/MaterialSourceFingerprint.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "ShaderCore.h"
#include "RHI.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
using namespace UEAIIntegration::MaterialEditing;
template<class T> T* AddSourceExpression(UObject* Owner)
{
	auto* E = NewObject<T>(Owner);
	if (auto* M = Cast<UMaterial>(Owner)) { M->GetExpressionCollection().AddExpression(E); E->Material = M; }
	else { auto* F = CastChecked<UMaterialFunction>(Owner); F->GetExpressionCollection().AddExpression(E); E->Function = F; }
	return E;
}

struct FIncludeFixture
{
	FString Directory, Root, Leaf, RootVirtual, LeafName;
	FIncludeFixture()
	{
		Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/UEAIMaterialSources"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Prefix = TEXT("/UEAI_Automation_MaterialSources");
		if (!AllShaderSourceDirectoryMappings().Contains(Prefix)) AddShaderSourceDirectoryMapping(Prefix, Directory);
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Root = Directory / (Id + TEXT("_root.ush")); LeafName = Id + TEXT("_leaf.ush"); Leaf = Directory / LeafName;
		RootVirtual = Prefix / FPaths::GetCleanFilename(Root);
		Write(Root, TEXT("// #include \"not_a_dependency.ush\"\n/* #include \"also_not_a_dependency.ush\" */\n#include \\\n\"") + LeafName + TEXT("\"\n"));
		Write(Leaf, TEXT("#define UEAI_SOURCE_VALUE 0.25\n"));
	}
	~FIncludeFixture() { IFileManager::Get().Delete(*Root); IFileManager::Get().Delete(*Leaf); }
	static bool Write(const FString& Path, const FString& Code) { return FFileHelper::SaveStringToFile(Code, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM); }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialDependencyFingerprintTest, "UE_AI_integration.MaterialSource.Dependencies", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialDependencyFingerprintTest::RunTest(const FString&)
{
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>());
	TStrongObjectPtr<UMaterialFunction> Outer(NewObject<UMaterialFunction>()), Inner(NewObject<UMaterialFunction>()), Unrelated(NewObject<UMaterialFunction>());
	auto* RootCall = AddSourceExpression<UMaterialExpressionMaterialFunctionCall>(M.Get()); RootCall->MaterialFunction = Outer.Get();
	auto* Nested = AddSourceExpression<UMaterialExpressionMaterialFunctionCall>(Outer.Get()); Nested->MaterialFunction = Inner.Get();
	auto* Custom = AddSourceExpression<UMaterialExpressionCustom>(Inner.Get()); Custom->Inputs.Reset(); Custom->Code = TEXT("return 0.25;");
	const auto Before = CaptureMaterialSourceFingerprint(M.Get());
	TestTrue(TEXT("Ordinary nested dependency check complete"), Before.bComplete); TestEqual(TEXT("Both functions captured"), Before.FunctionCount, 2);
	RecordMaterialCompileRequest(M.Get());
	Custom->Code = TEXT("return 0.75;"); // Deliberately no notification or PostEditChange.
	TestNotEqual(TEXT("Raw dependent edit changes source fingerprint"), CaptureMaterialSourceFingerprint(M.Get()).Hash, Before.Hash);
	TestEqual(TEXT("Manual dependent edit invalidates previous verdict"), ReadDiagnostics(M.Get())->GetStringField(TEXT("sourceState")), FString(TEXT("stale")));
	Custom->Code = TEXT("return 0.25;");
	NotifyMaterialSourceEdited(Unrelated.Get());
	TestEqual(TEXT("Unrelated tool edit does not invalidate this source"), ReadDiagnostics(M.Get())->GetStringField(TEXT("sourceState")), FString(TEXT("matched")));
	NotifyMaterialSourceEdited(Inner.Get());
	TestEqual(TEXT("Deferred edit notification is dependency scoped"), ReadDiagnostics(M.Get())->GetStringField(TEXT("sourceState")), FString(TEXT("stale")));
	TStrongObjectPtr<UMaterialFunctionInstance> Instance(NewObject<UMaterialFunctionInstance>()); Instance->SetParent(Outer.Get()); RootCall->MaterialFunction = Instance.Get();
	const auto InstanceBefore = CaptureMaterialSourceFingerprint(M.Get());
	TestTrue(TEXT("Instance parent chain is covered"), InstanceBefore.bComplete); TestEqual(TEXT("Instance plus ordinary parents captured"), InstanceBefore.FunctionCount, 3);
	FScalarParameterValue Override; Override.ParameterInfo = FMaterialParameterInfo(TEXT("Strength")); Override.ParameterValue = 0.5f; Instance->ScalarParameterValues.Add(Override);
	TestNotEqual(TEXT("Instance overrides invalidate source"), CaptureMaterialSourceFingerprint(M.Get()).Hash, InstanceBefore.Hash);
	auto* Layers = AddSourceExpression<UMaterialExpressionMaterialAttributeLayers>(M.Get()); Layers->DefaultLayers.Layers.Add(Unrelated.Get());
	auto* LayerCustom = AddSourceExpression<UMaterialExpressionCustom>(Unrelated.Get()); LayerCustom->Code = TEXT("return 0.25;");
	const auto LayerBefore = CaptureMaterialSourceFingerprint(M.Get());
	TestTrue(TEXT("Layer function dependency covered"), LayerBefore.bComplete);
	LayerCustom->Code = TEXT("return 0.50;");
	TestNotEqual(TEXT("Layer function code invalidates source"), CaptureMaterialSourceFingerprint(M.Get()).Hash, LayerBefore.Hash);
	auto* Back = AddSourceExpression<UMaterialExpressionMaterialFunctionCall>(Inner.Get()); Back->MaterialFunction = Outer.Get();
	TestFalse(TEXT("Cycles cannot be claimed completely verified"), CaptureMaterialSourceFingerprint(M.Get()).bComplete);
	Back->MaterialFunction = nullptr;
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialIncludeFingerprintTest, "UE_AI_integration.MaterialSource.IncludeFreshness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialIncludeFingerprintTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Virtual shader mapping test requires a rendering Editor.")); return true; }
	FIncludeFixture Files;
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>());
	auto* Custom = AddSourceExpression<UMaterialExpressionCustom>(M.Get()); Custom->Inputs.Reset(); Custom->Code = TEXT("return UEAI_SOURCE_VALUE;"); Custom->IncludeFilePaths = {Files.RootVirtual};
	const auto Before = CaptureMaterialSourceFingerprint(M.Get(), true);
	TestTrue(TEXT("Literal transitive includes are complete"), Before.bComplete); TestEqual(TEXT("Comments ignored and continued include followed"), Before.IncludeCount, 2);
	RecordMaterialCompileRequest(M.Get());
	const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*Files.Leaf);
	TestTrue(TEXT("Write same-size leaf edit"), Files.Write(Files.Leaf, TEXT("#define UEAI_SOURCE_VALUE 0.75\n"))); IFileManager::Get().SetTimeStamp(*Files.Leaf, Timestamp);
	const auto Changed = ReadDiagnostics(M.Get());
	TestEqual(TEXT("Same timestamp and size do not hide changed content"), Changed->GetStringField(TEXT("sourceState")), FString(TEXT("stale")));
	TestFalse(TEXT("Read-only poll does not trigger compilation"), Changed->GetBoolField(TEXT("compileTriggered")));
	TestTrue(TEXT("Poll did not refresh the stale native source cache"), CaptureMaterialSourceFingerprint(M.Get(), true).bShaderCacheMismatch);
	Custom->Code = TEXT("#define UEAIFILE \"unknown.ush\"\n#include UEAIFILE\nreturn 1;");
	TestFalse(TEXT("Macro include is explicitly incomplete"), CaptureMaterialSourceFingerprint(M.Get()).bComplete);
	RecordMaterialCompileRequest(M.Get());
	const auto Unknown = ReadDiagnostics(M.Get());
	TestEqual(TEXT("Incomplete source returns unverified state"), Unknown->GetStringField(TEXT("sourceState")), FString(TEXT("unverified")));
	bool bValid = false; TestFalse(TEXT("Incomplete source cannot assert validity"), Unknown->TryGetBoolField(TEXT("valid"), bValid));
	Custom->Code = TEXT("return 1;"); Custom->IncludeFilePaths = {TEXT("/UEAI_UnmappedSource/Missing.ush")};
	TestFalse(TEXT("Unmapped shader paths are nonfatal and incomplete"), CaptureMaterialSourceFingerprint(M.Get()).bComplete);
	Custom->IncludeFilePaths = {Files.RootVirtual};
	TestTrue(TEXT("Write an over-budget include"), Files.Write(Files.Leaf, FString::ChrN(512 * 1024 + 1, ' ')));
	TestFalse(TEXT("Oversized include cannot claim a complete fingerprint"), CaptureMaterialSourceFingerprint(M.Get()).bComplete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialIncludeCompilerTest, "UE_AI_integration.MaterialSource.IncludeCompilerCorrection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialIncludeCompilerTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Include compiler test requires a rendering Editor.")); return true; }
	FIncludeFixture Files;
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>()); M->SetShadingModel(MSM_Unlit);
	auto* Custom = AddSourceExpression<UMaterialExpressionCustom>(M.Get()); Custom->Inputs.Reset(); Custom->Code = TEXT("return UEAI_SOURCE_VALUE;"); Custom->IncludeFilePaths = {Files.RootVirtual};
	M->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Custom);
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("material"), M->GetPathName()); Params->SetBoolField(TEXT("waitForCompilation"), true);
	auto Validate = [&]() { return Registry->FindTool(TEXT("content.material.validate"))->Execute(Params); };
	const auto Good = Validate();
	if (!TestTrue(TEXT("Initial include material compiles"), Good.bSuccess && Good.Data->GetBoolField(TEXT("valid")))) { AddError(Good.ErrorMessage); return false; }
	Files.Write(Files.Leaf, TEXT("#define UEAI_SOURCE_VALUE UEAI_INCLUDE_MISSING_SYMBOL\n"));
	TestEqual(TEXT("Old successful verdict immediately becomes stale"), ReadDiagnostics(M.Get())->GetStringField(TEXT("compileState")), FString(TEXT("stale")));
	const auto Bad = Validate();
	if (!TestTrue(TEXT("Changed include validation completed"), Bad.bSuccess)) { AddError(Bad.ErrorMessage); return false; }
	TestTrue(TEXT("Explicit validation refreshes mismatched file cache"), Bad.Data->GetBoolField(TEXT("shaderFileCacheRefreshed")));
	TestEqual(TEXT("Native compiler reads changed include and rejects it"), Bad.Data->GetStringField(TEXT("compileState")), FString(TEXT("failed")));
	TestFalse(TEXT("Compiler rejects invalid include"), Bad.Data->GetBoolField(TEXT("valid")));
	Files.Write(Files.Leaf, TEXT("#define UEAI_SOURCE_VALUE 0.50\n"));
	const auto Fixed = Validate();
	if (!TestTrue(TEXT("Fixed include recompiled successfully"), Fixed.bSuccess && Fixed.Data->GetBoolField(TEXT("valid")))) return false;
	TestTrue(TEXT("Correction refreshes cached bad source"), Fixed.Data->GetBoolField(TEXT("shaderFileCacheRefreshed")));
	const auto Again = Validate();
	TestTrue(TEXT("Unchanged include remains valid"), Again.bSuccess && Again.Data->GetBoolField(TEXT("valid")));
	TestFalse(TEXT("Unchanged includes do not repeatedly flush caches"), Again.Data->GetBoolField(TEXT("shaderFileCacheRefreshed")));
	return true;
}
#endif
