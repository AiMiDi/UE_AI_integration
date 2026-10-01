#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Infrastructure/MaterialFunctionDependencies.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "RHI.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

namespace
{
FMCPToolRegistry* FunctionCallRegistry()
{
	auto* S = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	return S ? S->GetRegistry() : nullptr;
}
template<class T> T* FunctionCallExpression(UObject* Owner)
{
	auto* E = NewObject<T>(Owner, NAME_None, RF_Transactional);
	if (auto* M = Cast<UMaterial>(Owner)) { M->GetExpressionCollection().AddExpression(E); E->Material = M; }
	else { auto* F = CastChecked<UMaterialFunction>(Owner); F->GetExpressionCollection().AddExpression(E); E->Function = F; }
	return E;
}
TSharedRef<FJsonObject> FunctionCallParams(UObject* Owner, UMaterialExpression* E = nullptr, bool Deferred = true)
{
	auto P = MakeShared<FJsonObject>(); P->SetStringField(Owner->IsA<UMaterial>() ? TEXT("material") : TEXT("materialFunction"), Owner->GetPathName());
	if (E) P->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(E));
	if (Deferred) { auto C = MakeShared<FJsonObject>(); C->SetBoolField(TEXT("deferCompile"), true); P->SetObjectField(TEXT("__ueWorkflow"), C); }
	return P;
}
FMCPToolResult FunctionCallTool(const TCHAR* Id, TSharedPtr<FJsonObject> P) { return FunctionCallRegistry()->FindTool(Id)->Execute(P); }
void MakeFunctionInterface(UMaterialFunction* F, bool Reverse)
{
	for (int32 I = 0; I < 2; ++I)
	{
		auto* In = FunctionCallExpression<UMaterialExpressionFunctionInput>(F); In->InputName = I ? TEXT("B") : TEXT("A"); In->Id = FGuid::NewGuid(); In->InputType = FunctionInput_Scalar; In->SortPriority = Reverse ? 1-I : I;
		auto* Out = FunctionCallExpression<UMaterialExpressionFunctionOutput>(F); Out->OutputName = I ? TEXT("Glow") : TEXT("Mask"); Out->Id = FGuid::NewGuid(); Out->SortPriority = Reverse ? 1-I : I; Out->A.Connect(0, In);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionCallInterfaceTest, "UE_AI_integration.MaterialFunctionCall.InterfaceAndAtomicity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialFunctionCallInterfaceTest::RunTest(const FString&)
{
	if (!FunctionCallRegistry()) return false;
	for (bool Graph : {false, true})
	{
		TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient));
		TStrongObjectPtr<UMaterialFunction> A(NewObject<UMaterialFunction>(GetTransientPackage(), NAME_None, RF_Transient)), B(NewObject<UMaterialFunction>(GetTransientPackage(), NAME_None, RF_Transient));
		MakeFunctionInterface(A.Get(), false); MakeFunctionInterface(B.Get(), true);
		auto* Call = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(M.Get());
		auto* Source = FunctionCallExpression<UMaterialExpressionConstant>(M.Get()); auto* Consumer = FunctionCallExpression<UMaterialExpressionAdd>(M.Get());
		if (Graph) MCPMaterialInfrastructure::EnsureMaterialGraph(M.Get());
		auto P = FunctionCallParams(M.Get(), Call); P->SetStringField(TEXT("function"), A->GetPathName());
		if (!TestTrue(TEXT("Assign function"), FunctionCallTool(TEXT("content.material.function.call.set"), P).bSuccess)) return false;
		Call->FunctionInputs[0].Input.Connect(0, Source); Consumer->A.Connect(1, Call); M->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Call);
		if (Graph) M->MaterialGraph->LinkGraphNodesFromMaterial();
		const auto Read = FunctionCallTool(TEXT("content.material.function.call.get"), FunctionCallParams(M.Get(), Call, false));
		P->SetStringField(TEXT("expectedStateHash"), Read.Data->GetStringField(TEXT("stateHash"))); P->SetStringField(TEXT("function"), B->GetPathName()); P->SetBoolField(TEXT("dryRun"), true);
		TestTrue(TEXT("Dry run accepted"), FunctionCallTool(TEXT("content.material.function.call.set"), P).bSuccess); TestEqual(TEXT("Dry run preserves function"), Call->MaterialFunction.Get(), static_cast<UMaterialFunctionInterface*>(A.Get()));
		P->SetBoolField(TEXT("dryRun"), false);
		const auto Changed = FunctionCallTool(TEXT("content.material.function.call.set"), P);
		if (!TestTrue(TEXT("Switch function preserving names"), Changed.bSuccess)) { AddError(Changed.ErrorMessage); return false; }
		TestFalse(TEXT("Batch did not save"), Changed.Data->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("Input follows A after reorder"), Call->FunctionInputs[1].Input.Expression, static_cast<UMaterialExpression*>(Source));
		TestEqual(TEXT("Consumer follows Glow"), Consumer->A.OutputIndex, 0); TestEqual(TEXT("Root follows Mask"), M->GetExpressionInputForProperty(MP_EmissiveColor)->OutputIndex, 1);
		TestEqual(TEXT("Stale precondition rejected"), FunctionCallTool(TEXT("content.material.function.call.set"), P).ErrorCode, FString(TEXT("material_edit_conflict")));
		P = FunctionCallParams(M.Get(), Call); P->SetStringField(TEXT("function"), B->GetPathName());
		TestFalse(TEXT("Identical assignment no-op"), FunctionCallTool(TEXT("content.material.function.call.set"), P).Data->GetBoolField(TEXT("changed")));
		Call->FunctionInputs[1].ExpressionInput->InputName = TEXT("RenamedA"); Call->FunctionOutputs[1].ExpressionOutput->OutputName = TEXT("RenamedMask");
		TestTrue(TEXT("Refresh same function by GUID"), FunctionCallTool(TEXT("content.material.function.call.set"), P).bSuccess);
		TestEqual(TEXT("Rename preserves source"), Call->FunctionInputs[1].Input.Expression, static_cast<UMaterialExpression*>(Source));
		TestEqual(TEXT("Rename propagated to call"), Call->FunctionInputs[1].Input.InputName, FName(TEXT("RenamedA")));
		P->SetStringField(TEXT("function"), TEXT(""));
		TestEqual(TEXT("Clear requires explicit disconnect"), FunctionCallTool(TEXT("content.material.function.call.set"), P).ErrorCode, FString(TEXT("connected_pin_removal")));
		P->SetBoolField(TEXT("disconnectRemoved"), true); TestTrue(TEXT("Explicit clear accepted"), FunctionCallTool(TEXT("content.material.function.call.set"), P).bSuccess);
		TestNull(TEXT("Consumer cleared"), Consumer->A.Expression); TestNull(TEXT("Root cleared"), M->GetExpressionInputForProperty(MP_EmissiveColor)->Expression);
		auto* Nested = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(A.Get()); P = FunctionCallParams(A.Get(), Nested); P->SetStringField(TEXT("function"), B->GetPathName());
		TestTrue(TEXT("Nested function call supported"), FunctionCallTool(TEXT("content.material.function.call.set"), P).bSuccess);
		auto* Back = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(B.Get()); P = FunctionCallParams(B.Get(), Back); P->SetStringField(TEXT("function"), A->GetPathName());
		TestEqual(TEXT("Cycle rejected before native update"), FunctionCallTool(TEXT("content.material.function.call.set"), P).ErrorCode, FString(TEXT("function_call_cycle")));
		TestNull(TEXT("Rejected recursive call remains unassigned"), Back->MaterialFunction.Get());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionHostValidationTest, "UE_AI_integration.MaterialFunctionCall.HostShaderCorrection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FMaterialFunctionHostValidationTest::RunTest(const FString&)
{
	if (!FunctionCallRegistry()) return false;
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient));
	TStrongObjectPtr<UMaterialFunction> F(NewObject<UMaterialFunction>(GetTransientPackage(), NAME_None, RF_Transient));
	M->SetShadingModel(MSM_Unlit);
	auto* Custom = FunctionCallExpression<UMaterialExpressionCustom>(F.Get()); Custom->Inputs.Reset(); Custom->Code = TEXT("return UEAI_FUNCTION_MISSING_IDENTIFIER;");
	auto* Output = FunctionCallExpression<UMaterialExpressionFunctionOutput>(F.Get()); Output->OutputName = TEXT("Result"); Output->Id = FGuid::NewGuid(); Output->A.Connect(0, Custom);
	auto* Call = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(M.Get());
	auto P = FunctionCallParams(M.Get(), Call); P->SetStringField(TEXT("function"), F->GetPathName());
	if (!TestTrue(TEXT("Host assignment through public tool"), FunctionCallTool(TEXT("content.material.function.call.set"), P).bSuccess)) return false;
	auto Validate = FunctionCallParams(F.Get(), nullptr, false); Validate->SetStringField(TEXT("validationMaterial"), M->GetPathName()); Validate->SetBoolField(TEXT("waitForCompilation"), true);
	TestFalse(TEXT("Disconnected call is not a validation host"), FunctionCallTool(TEXT("content.material.function.validate"), Validate).bSuccess);
	auto* Declaration = FunctionCallExpression<UMaterialExpressionNamedRerouteDeclaration>(M.Get()); Declaration->Input.Connect(0, Call);
	auto* Usage = FunctionCallExpression<UMaterialExpressionNamedRerouteUsage>(M.Get()); Usage->Declaration = Declaration;
	M->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Usage);
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Host shader correction requires a rendering Editor.")); return true; }
	AddExpectedError(TEXT("UEAI_FUNCTION_MISSING_IDENTIFIER"), EAutomationExpectedErrorFlags::Contains, 0);
	int32 HostRefreshes = 0;
	const auto RefreshHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == M.Get()) ++HostRefreshes; });
	const auto Bad = FunctionCallTool(TEXT("content.material.function.validate"), Validate);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(RefreshHandle);
	TestEqual(TEXT("Host validation reuses native function update without a second PostEditChange"), HostRefreshes, 0);
	if (!TestTrue(TEXT("Explicit host validation ran"), Bad.bSuccess)) { AddError(Bad.ErrorMessage); return false; }
	TestEqual(TEXT("Function HLSL error reported by host"), Bad.Data->GetStringField(TEXT("compileState")), FString(TEXT("failed")));
	TestTrue(TEXT("Function diagnostic errors exposed"), Bad.Data->GetIntegerField(TEXT("errorCount")) > 0);
	P = FunctionCallParams(F.Get(), Custom); P->SetStringField(TEXT("code"), TEXT("return 0.25;"));
	TestTrue(TEXT("Function correction via Custom tool"), FunctionCallTool(TEXT("content.material.custom.set"), P).bSuccess);
	const auto Good = FunctionCallTool(TEXT("content.material.function.validate"), Validate);
	TestEqual(TEXT("Corrected host compiles"), Good.Data->GetStringField(TEXT("compileState")), FString(TEXT("succeeded")));
	TestTrue(TEXT("Host verdict is shader-validated"), Good.Data->GetBoolField(TEXT("shaderValidationPerformed")));
	// Translation errors carry native expression locations, unlike some DXC errors.
	FCustomInput Missing; Missing.InputName = TEXT("MissingInput"); Custom->Inputs = {Missing}; Custom->Code = TEXT("return MissingInput;");
	const auto MissingResult = FunctionCallTool(TEXT("content.material.function.validate"), Validate);
	const auto& Locations = MissingResult.Data->GetObjectField(TEXT("materialDiagnostics"))->GetArrayField(TEXT("errorExpressions"));
	TestTrue(TEXT("Native expression locations exposed"), Locations.Num() > 0);
	bool LocatedFunction = false;
	for (const auto& Location : Locations) if (Location->AsObject()->GetStringField(TEXT("assetPath")) == F->GetPathName()) LocatedFunction = true;
	TestTrue(TEXT("Function error includes owning asset"), LocatedFunction);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionHostReachabilityTest, "UE_AI_integration.MaterialFunctionCall.HostOutputReachability", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialFunctionHostReachabilityTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialEditing;
	TStrongObjectPtr<UMaterial> M(NewObject<UMaterial>());
	TStrongObjectPtr<UMaterialFunction> Target(NewObject<UMaterialFunction>()), Wrapper(NewObject<UMaterialFunction>());
	auto* Constant = FunctionCallExpression<UMaterialExpressionConstant>(Target.Get());
	auto* Output = FunctionCallExpression<UMaterialExpressionFunctionOutput>(Target.Get()); Output->Id = FGuid::NewGuid(); Output->OutputName = TEXT("Result"); Output->A.Connect(0, Constant);
	MakeFunctionInterface(Wrapper.Get(), false);
	auto* TargetCall = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(M.Get()); TargetCall->SetMaterialFunction(Target.Get());
	auto* First = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(M.Get()); First->SetMaterialFunction(Wrapper.Get());
	auto* Second = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(M.Get()); Second->SetMaterialFunction(Wrapper.Get());
	auto* Value = FunctionCallExpression<UMaterialExpressionConstant>(M.Get());
	First->FunctionInputs[0].Input.Connect(0, Value); First->FunctionInputs[1].Input.Connect(0, TargetCall);
	auto* Root = M->GetExpressionInputForProperty(MP_EmissiveColor); Root->Connect(0, First);
	TestFalse(TEXT("A target on an input unused by the selected output is not a host"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	// The actual validator must reject this before native function update or compilation.
	auto P = FunctionCallParams(Target.Get(), nullptr, false); P->SetStringField(TEXT("validationMaterial"), M->GetPathName());
	TestFalse(TEXT("Public validator rejects unused input context"), FunctionCallTool(TEXT("content.material.function.validate"), P).bSuccess);
	Root->Connect(1, First);
	TestTrue(TEXT("Selecting the output that uses the input establishes a host"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	Root->OutputIndex = 99; // Native Connect rejects invalid indices and leaves the previous connection intact.
	TestFalse(TEXT("Invalid output is not proof of a host"), ReferencesFunctionFromOutputs(M.Get(), Wrapper.Get()));
	Second->FunctionInputs[0].Input.Connect(0, TargetCall);
	auto* Sum = FunctionCallExpression<UMaterialExpressionAdd>(M.Get()); Sum->A.Connect(0, First); Sum->B.Connect(0, Second); Root->Connect(0, Sum);
	TestTrue(TEXT("Shared function body keeps separate caller input bindings"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	Second->FunctionInputs[0].Input.Connect(0, Value);
	TestFalse(TEXT("No used caller input references target after disconnect"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));

	auto* DefaultTarget = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(Wrapper.Get()); DefaultTarget->SetMaterialFunction(Target.Get());
	auto* InputA = First->FunctionInputs[0].ExpressionInput.Get(); InputA->Preview.Connect(0, DefaultTarget); InputA->bUsePreviewValueAsDefault = true;
	Root->Connect(0, First); First->FunctionInputs[0].Input.Expression = nullptr;
	TestTrue(TEXT("Unconnected optional input uses its function-local preview expression"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	First->FunctionInputs[0].Input.Connect(0, Value);
	TestFalse(TEXT("Caller override suppresses the preview expression"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	First->FunctionInputs[0].Input.Expression = nullptr; InputA->bUsePreviewValueAsDefault = false;
	TestFalse(TEXT("Required input does not use preview fallback"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));

	auto* Declaration = FunctionCallExpression<UMaterialExpressionNamedRerouteDeclaration>(M.Get()); Declaration->Input.Connect(0, TargetCall);
	auto* Usage = FunctionCallExpression<UMaterialExpressionNamedRerouteUsage>(M.Get()); Usage->Declaration = Declaration; Root->Connect(0, Usage);
	TestTrue(TEXT("Named reroute follows actual local declaration"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	Usage->Declaration = nullptr; Usage->DeclarationGuid = Declaration->VariableGuid;
	TestFalse(TEXT("Missing named declaration is not repaired from GUID"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	Usage->Declaration = Declaration; Declaration->Input.Connect(0, Usage);
	TestFalse(TEXT("Named reroute cycle terminates without a target"), ReferencesFunctionFromOutputs(M.Get(), Target.Get()));
	UMaterialExpression* Previous = TargetCall;
	for (int32 I = 0; I < 1025; ++I)
	{
		auto* Pass = FunctionCallExpression<UMaterialExpressionMaterialFunctionCall>(M.Get()); Pass->SetMaterialFunction(Wrapper.Get());
		Pass->FunctionInputs[0].Input.Connect(0, Previous); Previous = Pass;
	}
	Root->Connect(0, Previous); FString Reason;
	TestFalse(TEXT("Excess caller contexts do not prove reachability"), ReferencesFunctionFromOutputs(M.Get(), Target.Get(), &Reason));
	TestTrue(TEXT("Budget exhaustion is explicitly unverified"), Reason.Contains(TEXT("unverified")) && Reason.Contains(TEXT("1024")));
	TestNull(TEXT("Read-only host inspection builds no editor graph"), M->MaterialGraph.Get());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialFunctionInstanceCreateTest, "UE_AI_integration.MaterialFunctionCall.FunctionInstanceCreate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialFunctionInstanceCreateTest::RunTest(const FString&)
{
	if (!FunctionCallRegistry()) return false;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto* ParentPkg = CreatePackage(*FString::Printf(TEXT("/Game/Automation/FunctionInstanceParent_%s"), *Id));
	TStrongObjectPtr<UMaterialFunction> Parent(NewObject<UMaterialFunction>(ParentPkg, TEXT("ParentFunc"), RF_Transactional));
	auto* Scalar = FunctionCallExpression<UMaterialExpressionScalarParameter>(Parent.Get());
	Scalar->ParameterName = TEXT("Gain"); Scalar->DefaultValue = 0.25f; Scalar->ExpressionGUID = FGuid::NewGuid();
	auto* Vector = FunctionCallExpression<UMaterialExpressionVectorParameter>(Parent.Get());
	Vector->ParameterName = TEXT("Tint"); Vector->DefaultValue = FLinearColor::White; Vector->ExpressionGUID = FGuid::NewGuid();
	auto P = MakeShared<FJsonObject>();
	P->SetStringField(TEXT("parentFunction"), Parent->GetPathName());
	P->SetStringField(TEXT("name"), FString::Printf(TEXT("Inst_%s"), *Id));
	P->SetStringField(TEXT("packagePath"), TEXT("/Game/Automation"));
	auto Color = MakeShared<FJsonObject>(); Color->SetNumberField(TEXT("r"), 0.1); Color->SetNumberField(TEXT("g"), 0.2); Color->SetNumberField(TEXT("b"), 0.3); Color->SetNumberField(TEXT("a"), 1.0);
	auto O1 = MakeShared<FJsonObject>(); O1->SetStringField(TEXT("parameter"), TEXT("Gain")); O1->SetNumberField(TEXT("value"), 0.5);
	auto O2 = MakeShared<FJsonObject>(); O2->SetStringField(TEXT("parameter"), TEXT("Tint")); O2->SetObjectField(TEXT("value"), Color);
	P->SetArrayField(TEXT("overrides"), {MakeShared<FJsonValueObject>(O1), MakeShared<FJsonValueObject>(O2)});
	const auto Result = FunctionCallTool(TEXT("content.material.function.instance.create"), P);
	if (!TestTrue(TEXT("Function instance created"), Result.bSuccess)) { AddError(Result.ErrorMessage); return false; }
	TestEqual(TEXT("Two overrides applied"), Result.Data->GetIntegerField(TEXT("overridesApplied")), 2);
	const FString Path = Result.Data->GetStringField(TEXT("path"));
	auto* MFI = LoadObject<UMaterialFunctionInstance>(nullptr, *Path, nullptr, LOAD_NoWarn);
	if (!TestNotNull(TEXT("Instance loads by returned path"), MFI)) return false;
	TestEqual(TEXT("Parent assigned"), MFI->Parent.Get(), static_cast<UMaterialFunctionInterface*>(Parent.Get()));
	TestEqual(TEXT("Scalar override count"), MFI->ScalarParameterValues.Num(), 1);
	TestEqual(TEXT("Vector override count"), MFI->VectorParameterValues.Num(), 1);
	if (MFI->ScalarParameterValues.Num() == 1)
	{
		TestEqual(TEXT("Scalar value"), MFI->ScalarParameterValues[0].ParameterValue, 0.5f);
		TestEqual(TEXT("Scalar GUID resolves"), MFI->ScalarParameterValues[0].ExpressionGUID, Scalar->ExpressionGUID);
	}
	if (MFI->VectorParameterValues.Num() == 1)
	{
		const FLinearColor& C = MFI->VectorParameterValues[0].ParameterValue;
		TestEqual(TEXT("Vector R"), C.R, 0.1f);
		TestEqual(TEXT("Vector G"), C.G, 0.2f);
		TestEqual(TEXT("Vector B"), C.B, 0.3f);
		TestEqual(TEXT("Vector A"), C.A, 1.0f);
		TestEqual(TEXT("Vector GUID resolves"), MFI->VectorParameterValues[0].ExpressionGUID, Vector->ExpressionGUID);
	}
	auto P2 = MakeShared<FJsonObject>();
	P2->SetStringField(TEXT("parentFunction"), Parent->GetPathName());
	P2->SetStringField(TEXT("name"), FString::Printf(TEXT("InstBad_%s"), *Id));
	P2->SetStringField(TEXT("packagePath"), TEXT("/Game/Automation"));
	auto Bad = MakeShared<FJsonObject>(); Bad->SetStringField(TEXT("parameter"), TEXT("Missing")); Bad->SetNumberField(TEXT("value"), 1.0);
	P2->SetArrayField(TEXT("overrides"), {MakeShared<FJsonValueObject>(Bad)});
	const auto Missing = FunctionCallTool(TEXT("content.material.function.instance.create"), P2);
	TestTrue(TEXT("Unknown override reported without failing"), Missing.bSuccess && Missing.Data->GetIntegerField(TEXT("overridesApplied")) == 0 && Missing.Data->GetArrayField(TEXT("errors")).Num() == 1);
	return true;
}

#endif
