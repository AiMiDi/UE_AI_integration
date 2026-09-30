#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialShared.h"
#include "RHI.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
using namespace MCPMaterialInfrastructure;
FMCPToolRegistry* CustomRegistry()
{
	auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	return Subsystem ? Subsystem->GetRegistry() : nullptr;
}
TSharedRef<FJsonObject> EditParams(UObject* Asset, UMaterialExpression* Expression = nullptr, bool bDeferred = true)
{
	auto P = MakeShared<FJsonObject>(); P->SetStringField(Asset->IsA<UMaterial>() ? TEXT("material") : TEXT("materialFunction"), Asset->GetPathName());
	if (Expression) P->SetStringField(TEXT("nodeId"), ExpressionNodeId(Expression));
	if (bDeferred) { auto Context = MakeShared<FJsonObject>(); Context->SetBoolField(TEXT("deferCompile"), true); P->SetObjectField(TEXT("__ueWorkflow"), Context); }
	return P;
}
TSharedPtr<FJsonValue> Pin(const TCHAR* Name, const TCHAR* Previous = nullptr, const TCHAR* Type = nullptr)
{
	auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("name"), Name);
	if (Previous) P->SetStringField(TEXT("previousName"), Previous);
	if (Type) P->SetStringField(TEXT("type"), Type);
	return MakeShared<FJsonValueObject>(P);
}
template <typename T, typename TOwner> T* NewExpression(TOwner* Owner)
{
	auto* E = NewObject<T>(Owner, NAME_None, RF_Transactional); Owner->GetExpressionCollection().AddExpression(E);
	if constexpr (TIsSame<TOwner, UMaterial>::Value) E->Material = Owner; else E->Function = Owner;
	return E;
}
FMCPToolResult Call(const TCHAR* Id, TSharedPtr<FJsonObject> Params)
{
	// Internal handler tests inject trusted execution metadata. Public manifest
	// validation is tested separately and rejects this reserved field.
	return CustomRegistry()->FindTool(Id)->Execute(Params);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCustomInterfaceTest, "UE_AI_integration.MaterialCustom.InterfaceAndAtomicity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialCustomInterfaceTest::RunTest(const FString& Parameters)
{
	if (!CustomRegistry()) return false;
	for (bool bGraph : {false, true})
	{
		TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(CreatePackage(*FString::Printf(TEXT("/Game/Automation/Custom_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))), TEXT("Material"), RF_Transactional));
		auto* Custom = NewExpression<UMaterialExpressionCustom>(Material.Get());
		auto* A = NewExpression<UMaterialExpressionConstant>(Material.Get()); auto* B = NewExpression<UMaterialExpressionConstant>(Material.Get());
		auto* Consumer = NewExpression<UMaterialExpressionAdd>(Material.Get());
		Custom->Inputs.Reset(); FCustomInput InA, InB; InA.InputName = TEXT("A"); InA.Input.Connect(0, A); InB.InputName = TEXT("B"); InB.Input.Connect(0, B); Custom->Inputs = {InA, InB};
		FCustomOutput Mask, Glow; Mask.OutputName = TEXT("Mask"); Glow.OutputName = TEXT("Glow");
		// The editor can leave an unnamed output row; it does not create a pin.
		Custom->AdditionalOutputs = {FCustomOutput(), Mask, Glow}; UEAIIntegration::MaterialEditing::RebuildCustomOutputs(Custom);
		Custom->Code = TEXT("Mask = A; Glow = B; return A+B;"); Consumer->A.Connect(1, Custom);
		Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(2, Custom);
		if (bGraph) EnsureMaterialGraph(Material.Get());
		Material->GetOutermost()->SetDirtyFlag(false);
		const FString Id = ExpressionNodeId(Custom);
		auto Read = Call(TEXT("content.material.custom.get"), EditParams(Material.Get(), Custom, false));
		if (!TestTrue(TEXT("Read Custom without compiling"), Read.bSuccess)) return false;
		TestEqual(TEXT("Unnamed output has no graph pin"), Read.Data->GetArrayField(TEXT("additionalOutputs"))[0]->AsObject()->GetIntegerField(TEXT("index")), INDEX_NONE);
		TestEqual(TEXT("Named output uses compact pin index"), Read.Data->GetArrayField(TEXT("additionalOutputs"))[1]->AsObject()->GetIntegerField(TEXT("index")), 1);
		TestFalse(TEXT("Read leaves package clean"), Material->GetOutermost()->IsDirty());
		auto P = EditParams(Material.Get(), Custom); P->SetStringField(TEXT("expectedStateHash"), Read.Data->GetStringField(TEXT("stateHash")));
		P->SetStringField(TEXT("code"), TEXT("Mask = Strength; Glow = B; return Strength+B;"));
		P->SetArrayField(TEXT("inputs"), {Pin(TEXT("B")), Pin(TEXT("Strength"), TEXT("A"))});
		P->SetArrayField(TEXT("additionalOutputs"), {Pin(TEXT("Glow"), nullptr, TEXT("Float1")), Pin(TEXT("Mask"), nullptr, TEXT("Float1"))});
		P->SetBoolField(TEXT("dryRun"), true);
		TestTrue(TEXT("Dry-run validated"), Call(TEXT("content.material.custom.set"), P).bSuccess);
		TestFalse(TEXT("Dry-run does not dirty"), Material->GetOutermost()->IsDirty());
		TestEqual(TEXT("Dry-run keeps names"), Custom->Inputs[0].InputName, FName(TEXT("A")));
		P->SetBoolField(TEXT("dryRun"), false);
		int32 Notifications = 0;
		const auto Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == Material.Get()) ++Notifications; });
		const auto Changed = Call(TEXT("content.material.custom.set"), P);
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
		if (!TestTrue(TEXT("Rename/reorder succeeds"), Changed.bSuccess)) { AddError(Changed.ErrorMessage); return false; }
		TestEqual(TEXT("No intermediate material refresh"), Notifications, 0);
		TestFalse(TEXT("Deferred edit did not save"), Changed.Data->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("Node identity retained"), ExpressionNodeId(Custom), Id);
		TestEqual(TEXT("Reordered B retains source"), Custom->Inputs[0].Input.Expression, static_cast<UMaterialExpression*>(B));
		TestEqual(TEXT("Renamed A retains source"), Custom->Inputs[1].Input.Expression, static_cast<UMaterialExpression*>(A));
		TestEqual(TEXT("Consumer follows Mask output"), Consumer->A.OutputIndex, 2);
		TestEqual(TEXT("Material root follows Glow output"), Material->GetExpressionInputForProperty(MP_EmissiveColor)->OutputIndex, 1);
		if (bGraph)
		{
			Material->MaterialGraph->LinkMaterialExpressionsFromGraph();
			TestEqual(TEXT("Rebuilt graph preserves remapped consumer"), Consumer->A.OutputIndex, 2);
		}
		TestEqual(TEXT("Stale hash rejected"), Call(TEXT("content.material.custom.set"), P).ErrorCode, FString(TEXT("material_edit_conflict")));
		P = EditParams(Material.Get(), Custom); P->SetStringField(TEXT("code"), Custom->Code);
		Material->GetOutermost()->SetDirtyFlag(false);
		TestFalse(TEXT("Identical code is a no-op"), Call(TEXT("content.material.custom.set"), P).Data->GetBoolField(TEXT("changed")));
		TestFalse(TEXT("No-op leaves package clean"), Material->GetOutermost()->IsDirty());
		P->SetArrayField(TEXT("inputs"), {Pin(TEXT("x")), Pin(TEXT("X"))});
		P->SetStringField(TEXT("code"), TEXT("this must never replace existing code"));
		TestFalse(TEXT("Duplicate FName fails before write"), Call(TEXT("content.material.custom.set"), P).bSuccess);
		TestFalse(TEXT("Rejected edit does not dirty"), Material->GetOutermost()->IsDirty());
		TestNotEqual(TEXT("Invalid interface did not partly write code"), Custom->Code, P->GetStringField(TEXT("code")));
		P = EditParams(Material.Get(), Custom); P->SetArrayField(TEXT("additionalOutputs"), {Pin(TEXT("Glow"), nullptr, TEXT("Float1"))});
		TestEqual(TEXT("Connected removal needs explicit disconnect"), Call(TEXT("content.material.custom.set"), P).ErrorCode, FString(TEXT("connected_pin_removal")));
		P->SetBoolField(TEXT("disconnectRemoved"), true);
		TestTrue(TEXT("Explicit removal succeeds"), Call(TEXT("content.material.custom.set"), P).bSuccess);
		TestNull(TEXT("Removed output clears consumer"), Consumer->A.Expression);
		TestEqual(TEXT("Unremoved root link retained"), Material->GetExpressionInputForProperty(MP_EmissiveColor)->OutputIndex, 1);
		Material->GetOutermost()->SetDirtyFlag(false);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCustomFunctionParametersTest, "UE_AI_integration.MaterialCustom.FunctionParametersAndCrud", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialCustomFunctionParametersTest::RunTest(const FString& Parameters)
{
	if (!CustomRegistry()) return false;
	TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>(CreatePackage(*FString::Printf(TEXT("/Game/Automation/CustomMF_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))), TEXT("Function"), RF_Transactional));
	auto* Custom = NewExpression<UMaterialExpressionCustom>(Function.Get());
	auto* Scalar = NewExpression<UMaterialExpressionScalarParameter>(Function.Get()); Scalar->ParameterName = TEXT("Gain"); Scalar->ExpressionGUID = FGuid::NewGuid();
	auto* Vector = NewExpression<UMaterialExpressionVectorParameter>(Function.Get()); Vector->ParameterName = TEXT("Tint");
	auto* Switch = NewExpression<UMaterialExpressionStaticSwitchParameter>(Function.Get()); Switch->ParameterName = TEXT("Enabled");
	auto* Texture = NewExpression<UMaterialExpressionTextureObjectParameter>(Function.Get()); Texture->ParameterName = TEXT("Albedo");
	auto P = EditParams(Function.Get(), Custom); P->SetStringField(TEXT("code"), TEXT("return Gain * FACTOR;")); P->SetArrayField(TEXT("inputs"), {Pin(TEXT("Gain"))});
	auto Define = MakeShared<FJsonObject>(); Define->SetStringField(TEXT("name"), TEXT("FACTOR")); Define->SetStringField(TEXT("value"), TEXT("2.0"));
	P->SetArrayField(TEXT("defines"), {MakeShared<FJsonValueObject>(Define)}); P->SetArrayField(TEXT("includePaths"), {MakeShared<FJsonValueString>(TEXT("/Engine/Private/Common.ush"))});
	TestTrue(TEXT("Configure function Custom"), Call(TEXT("content.material.custom.set"), P).bSuccess);
	TestNull(TEXT("No function graph constructed"), Function->MaterialGraph);
	const auto Read = Call(TEXT("content.material.custom.get"), EditParams(Function.Get(), Custom, false));
	TestEqual(TEXT("Missing input is diagnosed"), Read.Data->GetArrayField(TEXT("structuralDiagnostics")).Num(), 1);
	P = EditParams(Function.Get()); P->SetStringField(TEXT("sourceNodeId"), ExpressionNodeId(Scalar)); P->SetStringField(TEXT("sourcePinName"), TEXT("Output"));
	P->SetStringField(TEXT("targetNodeId"), ExpressionNodeId(Custom)); P->SetStringField(TEXT("targetPinName"), TEXT("Gain"));
	TestTrue(TEXT("Connect named function Custom input"), Call(TEXT("content.material.pin.connect"), P).bSuccess);
	const FGuid Guid = Scalar->ExpressionGUID;
	P = EditParams(Function.Get(), Scalar); P->SetStringField(TEXT("name"), TEXT("Intensity")); P->SetStringField(TEXT("group"), TEXT("Custom Controls")); P->SetNumberField(TEXT("defaultValue"), 0.75); P->SetNumberField(TEXT("sliderMax"), 2);
	const auto Set = Call(TEXT("content.material.parameter.set"), P);
	if (!TestTrue(TEXT("Configure scalar metadata/default"), Set.bSuccess)) { AddError(Set.ErrorMessage); return false; }
	TestEqual(TEXT("Parameter GUID stable"), Scalar->ExpressionGUID, Guid);
	TestEqual(TEXT("Default applied"), Scalar->DefaultValue, 0.75f);
	TestEqual(TEXT("Rename leaves Custom wire intact"), Custom->Inputs[0].Input.Expression, static_cast<UMaterialExpression*>(Scalar));
	P->SetStringField(TEXT("name"), TEXT("Tint")); TestFalse(TEXT("Collision rejected"), Call(TEXT("content.material.parameter.set"), P).bSuccess);
	P = EditParams(Function.Get(), Switch); P->SetBoolField(TEXT("defaultValue"), true);
	TestTrue(TEXT("StaticSwitch boolean supported"), Call(TEXT("content.material.parameter.set"), P).bSuccess); TestTrue(TEXT("Static value applied"), Switch->DefaultValue);
	P = EditParams(Function.Get(), Texture); P->SetStringField(TEXT("samplerType"), TEXT("LinearColor"));
	TestTrue(TEXT("Texture sampler metadata supported"), Call(TEXT("content.material.parameter.set"), P).bSuccess);
	P = EditParams(Function.Get(), nullptr, false); P->SetNumberField(TEXT("limit"), 2);
	const auto Page = Call(TEXT("content.material.parameter.list"), P);
	TestEqual(TEXT("Local parameter count"), Page.Data->GetIntegerField(TEXT("total")), 4); TestTrue(TEXT("Parameters paged"), Page.Data->GetBoolField(TEXT("hasMore")));
	P = EditParams(Function.Get()); P->SetStringField(TEXT("expressionClass"), TEXT("ScalarParameter"));
	const auto Added = Call(TEXT("content.material.expression.add"), P); if (!TestTrue(TEXT("Parameter add uses existing operation"), Added.bSuccess)) return false;
	P = EditParams(Function.Get()); P->SetStringField(TEXT("nodeId"), Added.Data->GetStringField(TEXT("nodeId")));
	TestTrue(TEXT("Parameter delete uses existing operation"), Call(TEXT("content.material.expression.delete"), P).bSuccess);
	P = EditParams(Function.Get(), Custom); P->SetArrayField(TEXT("defines"), {}); P->SetArrayField(TEXT("includePaths"), {});
	TestTrue(TEXT("Clear optional lists"), Call(TEXT("content.material.custom.set"), P).bSuccess); TestEqual(TEXT("Defines removed"), Custom->AdditionalDefines.Num(), 0); TestEqual(TEXT("Includes removed"), Custom->IncludeFilePaths.Num(), 0);
	TArray<FString> SchemaErrors;
	TestFalse(TEXT("Public schema rejects forged Workflow context"), CustomRegistry()->ValidateParams(TEXT("content.material.custom.set"), P, SchemaErrors));
	Function->GetOutermost()->SetDirtyFlag(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialCustomCompilerTest, "UE_AI_integration.MaterialCustom.NativeCompilerCorrection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialCustomCompilerTest::RunTest(const FString& Parameters)
{
	if (!CustomRegistry()) return false;
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient));
	const auto Uncompiled = UEAIIntegration::MaterialEditing::ReadDiagnostics(Material.Get());
	TestNotEqual(TEXT("Uncompiled resource is not success"), Uncompiled->GetStringField(TEXT("compileState")), FString(TEXT("succeeded")));
	UEAIIntegration::MaterialEditing::RecordMaterialCompileRequest(Material.Get());
	auto* Probe = NewExpression<UMaterialExpressionCustom>(Material.Get());
	TestEqual(TEXT("Editing invalidates an older compiler verdict"), UEAIIntegration::MaterialEditing::ReadDiagnostics(Material.Get())->GetStringField(TEXT("compileState")), FString(TEXT("stale")));
	Material->GetExpressionCollection().RemoveExpression(Probe);
	if (!FApp::CanEverRender() || GUsingNullRHI) { AddInfo(TEXT("Native shader correction requires a rendering Editor; unavailable state checked under NullRHI.")); return true; }
	Material->SetShadingModel(MSM_Unlit);
	auto* Custom = NewExpression<UMaterialExpressionCustom>(Material.Get()); Custom->Inputs.Reset(); Custom->Code = TEXT("return UEAI_MISSING_IDENTIFIER;");
	Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Custom);
	AddExpectedError(TEXT("UEAI_MISSING_IDENTIFIER"), EAutomationExpectedErrorFlags::Contains, 0);
	auto P = EditParams(Material.Get(), nullptr, false); P->SetBoolField(TEXT("waitForCompilation"), true);
	const auto Bad = Call(TEXT("content.material.validate"), P);
	if (!TestTrue(TEXT("Native validation ran"), Bad.bSuccess)) return false;
	TestEqual(TEXT("Invalid HLSL fails actual compilation"), Bad.Data->GetStringField(TEXT("compileState")), FString(TEXT("failed")));
	bool bValid = true;
	TestTrue(TEXT("Compiler returns a validity verdict"), Bad.Data->TryGetBoolField(TEXT("valid"), bValid));
	TestFalse(TEXT("Compiler rejects unknown identifier"), bValid);
	TestTrue(TEXT("Compiler errors exposed"), Bad.Data->GetArrayField(TEXT("diagnostics")).Num() > 0);
	auto Fix = EditParams(Material.Get(), Custom); Fix->SetStringField(TEXT("code"), TEXT("return 0.25;"));
	if (!TestTrue(TEXT("Apply correction through Custom tool"), Call(TEXT("content.material.custom.set"), Fix).bSuccess)) return false;
	const auto Good = Call(TEXT("content.material.validate"), P);
	if (!TestTrue(TEXT("Corrected validation ran"), Good.bSuccess)) return false;
	TestEqual(TEXT("Corrected HLSL compiles"), Good.Data->GetStringField(TEXT("compileState")), FString(TEXT("succeeded")));
	const auto Poll = Call(TEXT("content.material.diagnostics.get"), EditParams(Material.Get(), nullptr, false));
	if (!TestTrue(TEXT("Read compiler verdict"), Poll.bSuccess)) return false;
	TestFalse(TEXT("Polling does not trigger compilation"), Poll.Data->GetBoolField(TEXT("compileTriggered")));
	TestEqual(TEXT("Previous errors cleared"), Poll.Data->GetIntegerField(TEXT("errorCount")), 0);
	return true;
}

#endif
