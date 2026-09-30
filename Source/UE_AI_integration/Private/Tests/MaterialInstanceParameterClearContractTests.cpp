#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GCObjectScopeGuard.h"

namespace
{
// Unique prefix avoids unity-build symbol collisions with sibling test TUs.
FMCPToolBase* MaterialInstanceParamClearFindTool(FMCPToolRegistry* Registry)
{
	return Registry ? Registry->FindTool(TEXT("content.material.instance.parameter.clear")) : nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceParameterClearContractTest, "UE_AI_integration.Material.ParameterClearContract", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialInstanceParameterClearContractTest::RunTest(const FString&)
{
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	FMCPToolBase* Tool = MaterialInstanceParamClearFindTool(Registry);
	if (!TestNotNull(TEXT("clear tool is registered"), Tool)) return false;

	// (a) Unknown instance path -> instance_not_found (404).
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("instance"), TEXT("/Game/MaterialInstanceParamClear/DoesNotExist"));
		P->SetStringField(TEXT("parameter"), TEXT("SomeParameter"));
		const FMCPToolResult R = Tool->Execute(P);
		TestFalse(TEXT("(a) unknown instance is rejected"), R.bSuccess);
		TestEqual(TEXT("(a) instance_not_found code"), R.ErrorCode, FString(TEXT("instance_not_found")));
		TestEqual(TEXT("(a) instance_not_found status"), R.HttpStatus, 404);
	}

	// Build a non-transient /Game/ fixture (passes the write guard) with an
	// /Engine/ DefaultMaterial parent. Skipped when unavailable.
	UPackage* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/ParamClear_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	UMaterial* ParentMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"), nullptr, LOAD_NoWarn);
	UMaterialInstanceConstant* MI = Package ? NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Transactional) : nullptr;
	if (!Package || !MI || !ParentMaterial)
	{
		AddInfo(TEXT("Skipped (b)/(c): could not build a non-transient /Game/ MaterialInstanceConstant fixture with an /Engine/ parent."));
		return true;
	}
	FGCObjectScopeGuard InstanceGuard(MI);
	MI->SetParentEditorOnly(ParentMaterial, false);
	const FName ParamName(TEXT("ParamClearScalar"));
	{
		FScalarParameterValue& Override = MI->ScalarParameterValues.AddDefaulted_GetRef();
		Override.ParameterInfo = FMaterialParameterInfo(ParamName);
		Override.ParameterValue = 0.5f;
		Override.ExpressionGUID = FGuid::NewGuid();
	}
	auto Target = [&]() { auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("instance"), MI->GetPathName()); return P; };

	Package->SetDirtyFlag(false);
	// (b) Unknown parameter name -> parameter_not_found (404) with no mutation.
	{
		auto P = Target(); P->SetStringField(TEXT("parameter"), TEXT("NoSuchParameter"));
		const FMCPToolResult R = Tool->Execute(P);
		TestFalse(TEXT("(b) unknown parameter is rejected"), R.bSuccess);
		TestEqual(TEXT("(b) parameter_not_found code"), R.ErrorCode, FString(TEXT("parameter_not_found")));
		TestEqual(TEXT("(b) parameter_not_found status"), R.HttpStatus, 404);
		TestEqual(TEXT("(b) no mutation: override still present"), MI->ScalarParameterValues.Num(), 1);
		TestFalse(TEXT("(b) no mutation: package stays clean"), Package->IsDirty());
	}

	// (c) Clear the authored scalar override.
	{
		auto P = Target(); P->SetStringField(TEXT("parameter"), ParamName.ToString());
		const FMCPToolResult R = Tool->Execute(P);
		if (!TestTrue(TEXT("(c) clear succeeds"), R.bSuccess))
		{
			AddError(R.ErrorMessage);
			if (R.Data)
			{
				FString Json; auto W = TJsonWriterFactory<>::Create(&Json); FJsonSerializer::Serialize(R.Data.ToSharedRef(), W); AddInfo(Json);
			}
			return false;
		}
		TestEqual(TEXT("(c) removedCount == 1"), R.Data->GetIntegerField(TEXT("removedCount")), 1);
		TestEqual(TEXT("(c) scalar override removed from array"), MI->ScalarParameterValues.Num(), 0);
		TestEqual(TEXT("(c) schema"), R.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.material.instance-parameter-clear.v1")));
		TestFalse(TEXT("(c) never saves"), R.Data->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("(c) package marked dirty"), Package->IsDirty());
	}

	// The fixture package is never saved (dirty-only), per the capability contract.
	return true;
}
#endif
