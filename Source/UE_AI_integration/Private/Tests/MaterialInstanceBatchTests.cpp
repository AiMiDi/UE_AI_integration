#if WITH_DEV_AUTOMATION_TESTS
#include "Editor.h"
#include "EditorSupportDelegates.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ObjectSaveContext.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"

namespace
{
template<class T> T* InstanceExpression(UMaterial* M, const TCHAR* Name)
{
	auto* E = NewObject<T>(M, Name, RF_Transactional); E->Material = M; M->GetExpressionCollection().AddExpression(E); E->ExpressionGUID = FGuid::NewGuid(); return E;
}
TSharedPtr<FJsonObject> Edit(const TCHAR* Op, const TCHAR* Name, const TCHAR* Type, TSharedPtr<FJsonValue> Value = nullptr, const TCHAR* Association = TEXT("global"), int32 Index = INDEX_NONE)
{
	auto O = MakeShared<FJsonObject>(); O->SetStringField(TEXT("op"), Op); O->SetStringField(TEXT("name"), Name); O->SetStringField(TEXT("type"), Type);
	O->SetStringField(TEXT("association"), Association); O->SetNumberField(TEXT("index"), Index); if (Value) O->SetField(TEXT("value"), Value); return O;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceBatchTest, "UE_AI_integration.MaterialInstance.BatchAndInheritance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialInstanceBatchTest::RunTest(const FString&)
{
	auto* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/InstanceBatch_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	auto* M = NewObject<UMaterial>(Package, TEXT("Parent"), RF_Transactional); FGCObjectScopeGuard MaterialGuard(M); M->SetShadingModel(MSM_Unlit);
	auto* Scalar = InstanceExpression<UMaterialExpressionScalarParameter>(M, TEXT("Strength")); Scalar->ParameterName = TEXT("Strength"); Scalar->DefaultValue = 0.25f;
	auto* Vector = InstanceExpression<UMaterialExpressionVectorParameter>(M, TEXT("Tint")); Vector->ParameterName = TEXT("Tint"); Vector->DefaultValue = FLinearColor::White;
	auto* Texture = InstanceExpression<UMaterialExpressionTextureSampleParameter2D>(M, TEXT("Albedo")); Texture->ParameterName = TEXT("Albedo"); Texture->Texture = LoadObject<UTexture2D>(nullptr, TEXT("/Engine/EngineResources/DefaultTexture.DefaultTexture"));
	if (!TestNotNull(TEXT("Default texture fixture available"), Texture->Texture.Get())) return false;
	auto* Switch = InstanceExpression<UMaterialExpressionStaticSwitchParameter>(M, TEXT("Toggle")); Switch->ParameterName = TEXT("Toggle"); Switch->DefaultValue = false; Switch->A.Connect(0, Scalar); Switch->B.Connect(0, Scalar);
	M->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Switch); M->PostEditChange();
	auto* Parent = NewObject<UMaterialInstanceConstant>(Package, TEXT("Intermediate"), RF_Transactional); FGCObjectScopeGuard ParentGuard(Parent); Parent->SetParentEditorOnly(M); Parent->SetScalarParameterValueEditorOnly(TEXT("Strength"), 0.75f); Parent->PostEditChange();
	auto* MI = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Transactional); FGCObjectScopeGuard InstanceGuard(MI); MI->SetParentEditorOnly(Parent); MI->PostEditChange();
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	auto Target = [&]() { auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("instance"), MI->GetPathName()); return P; };
	auto Query = [&]() { return Registry->FindTool(TEXT("content.material.instance.parameters.get"))->Execute(Target()); };
	auto Row = [&](const FMCPToolResult& R, const TCHAR* Name) -> TSharedPtr<FJsonObject> { if (!R.bSuccess) return nullptr; for (const auto& V : R.Data->GetArrayField(TEXT("parameters"))) if (V->AsObject()->GetStringField(TEXT("name")) == Name) return V->AsObject(); return nullptr; };
	auto Batch = [&](const TArray<TSharedPtr<FJsonObject>>& Ops, bool Dry = false) { auto P = Target(); const auto Q = Query(); if (Q.bSuccess) P->SetStringField(TEXT("expectedStateHash"), Q.Data->GetStringField(TEXT("stateHash"))); TArray<TSharedPtr<FJsonValue>> Values; for (const auto& O : Ops) Values.Add(MakeShared<FJsonValueObject>(O)); P->SetArrayField(TEXT("operations"), Values); P->SetBoolField(TEXT("dryRun"), Dry); return Registry->FindTool(TEXT("content.material.instance.parameters.batch"))->Execute(P); };
	const auto Initial = Query(); if (!TestTrue(TEXT("Read full instance parameter contract"), Initial.bSuccess)) { AddError(Initial.ErrorMessage); return false; }
	const auto Inherited = Row(Initial, TEXT("Strength")); if (!TestTrue(TEXT("Inherited scalar is listed"), Inherited.IsValid())) return false;
	TestEqual(TEXT("Intermediate parent effective value is read"), Inherited->GetNumberField(TEXT("effectiveValue")), 0.75); TestFalse(TEXT("Inherited value is not a local override"), Inherited->GetBoolField(TEXT("overridden")));
	int32 Updates = 0, Refreshes = 0;
	const auto UpdateHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == MI) ++Updates; });
	const auto RefreshHandle = FEditorDelegates::RefreshEditor.AddLambda([&]() { ++Refreshes; });
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(UpdateHandle); FEditorDelegates::RefreshEditor.Remove(RefreshHandle); };
	Package->SetDirtyFlag(false);
	auto Color = MakeShared<FJsonObject>(); Color->SetNumberField(TEXT("r"), 0.1); Color->SetNumberField(TEXT("g"), 0.2); Color->SetNumberField(TEXT("b"), 0.3); Color->SetNumberField(TEXT("a"), 1.0);
	TArray<TSharedPtr<FJsonObject>> Ops = {Edit(TEXT("set"), TEXT("Strength"), TEXT("scalar"), MakeShared<FJsonValueNumber>(0.5)), Edit(TEXT("set"), TEXT("Tint"), TEXT("vector"), MakeShared<FJsonValueObject>(Color)), Edit(TEXT("set"), TEXT("Albedo"), TEXT("texture"), MakeShared<FJsonValueString>(Texture->Texture->GetPathName())), Edit(TEXT("set"), TEXT("Toggle"), TEXT("switch"), MakeShared<FJsonValueBoolean>(true))};
	TestTrue(TEXT("Dry run validates all four parameter types"), Batch(Ops, true).bSuccess); TestEqual(TEXT("Dry run produces no updates"), Updates, 0); TestFalse(TEXT("Dry run leaves package clean"), Package->IsDirty());
	auto InvalidOps = Ops; InvalidOps.Add(Edit(TEXT("set"), TEXT("Missing"), TEXT("scalar"), MakeShared<FJsonValueNumber>(2)));
	TestFalse(TEXT("Invalid later operation rejects entire batch"), Batch(InvalidOps).bSuccess); TestTrue(TEXT("No earlier overrides were applied"), MI->ScalarParameterValues.IsEmpty()); TestEqual(TEXT("Rejected batch produces no updates"), Updates, 0);
	TestFalse(TEXT("Duplicate edits are rejected before mutation"), Batch({Ops[0], Ops[0]}).bSuccess);
	TestFalse(TEXT("Out-of-range float is rejected"), Batch({Edit(TEXT("set"), TEXT("Strength"), TEXT("scalar"), MakeShared<FJsonValueNumber>(1.e100))}).bSuccess);
	const auto Applied = Batch(Ops); if (!TestTrue(TEXT("Four-type batch applied"), Applied.bSuccess)) { AddError(Applied.ErrorMessage); if (Applied.Data) { FString Json; auto W = TJsonWriterFactory<>::Create(&Json); FJsonSerializer::Serialize(Applied.Data.ToSharedRef(), W); AddInfo(Json); } return false; }
	TestEqual(TEXT("One instance notification for whole batch"), Updates, 1); TestEqual(TEXT("One editor refresh for whole batch"), Refreshes, 1); TestFalse(TEXT("Default batch did not save"), Applied.Data->GetBoolField(TEXT("saved"))); TestTrue(TEXT("Default batch marks dirty"), Package->IsDirty());
	TestEqual(TEXT("New override keeps the declaration GUID, not an ancestor override GUID"), MI->ScalarParameterValues[0].ExpressionGUID, Scalar->ExpressionGUID);
	const auto Readback = Query(); TestEqual(TEXT("Scalar effective value updated"), Row(Readback, TEXT("Strength"))->GetNumberField(TEXT("effectiveValue")), 0.5); TestTrue(TEXT("Static switch effective value updated"), Row(Readback, TEXT("Toggle"))->GetBoolField(TEXT("effectiveValue")));
	auto ReadLegacy = MakeShared<FJsonObject>(); ReadLegacy->SetStringField(TEXT("name"), MI->GetPathName());
	const auto OldReadback = Registry->FindTool(TEXT("content.material.get"))->Execute(ReadLegacy); bool bLegacySwitch = false;
	if (OldReadback.bSuccess) for (const auto& V : OldReadback.Data->GetArrayField(TEXT("overriddenParameters"))) if (V->AsObject()->GetStringField(TEXT("type")) == TEXT("StaticSwitch")) bLegacySwitch = V->AsObject()->GetBoolField(TEXT("overridden")) && V->AsObject()->GetBoolField(TEXT("value"));
	TestTrue(TEXT("Legacy material query also reads the static override reliably"), bLegacySwitch);
	GEditor->UndoTransaction(); TestTrue(TEXT("One Undo removes the whole batch"), MI->ScalarParameterValues.IsEmpty() && MI->VectorParameterValues.IsEmpty() && MI->TextureParameterValues.IsEmpty() && MI->GetStaticParameters().StaticSwitchParameters.IsEmpty());
	GEditor->RedoTransaction(); TestEqual(TEXT("Redo restores scalar override"), MI->ScalarParameterValues.Num(), 1); TestEqual(TEXT("Redo restores switch override"), MI->GetStaticParameters().StaticSwitchParameters.Num(), 1);
	Updates = Refreshes = 0; TestTrue(TEXT("Identical batch is a no-op"), Batch(Ops).bSuccess); TestEqual(TEXT("No-op does not refresh"), Updates, 0);
	auto Stale = Target(); Stale->SetStringField(TEXT("expectedStateHash"), Query().Data->GetStringField(TEXT("stateHash"))); Stale->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Ops[0])});
	Parent->SetScalarParameterValueEditorOnly(TEXT("Strength"), 0.875f); Parent->PostEditChange(); TestFalse(TEXT("Parent changes invalidate the prepared state"), Registry->FindTool(TEXT("content.material.instance.parameters.batch"))->Execute(Stale).bSuccess);
	const auto Cleared = Batch({Edit(TEXT("clear"), TEXT("Strength"), TEXT("scalar")), Edit(TEXT("clear"), TEXT("Tint"), TEXT("vector")), Edit(TEXT("clear"), TEXT("Albedo"), TEXT("texture")), Edit(TEXT("clear"), TEXT("Toggle"), TEXT("switch"))}); TestTrue(TEXT("Clear all four overrides"), Cleared.bSuccess);
	const auto ClearRead = Query(); TestEqual(TEXT("Cleared override follows intermediate parent"), Row(ClearRead, TEXT("Strength"))->GetNumberField(TEXT("effectiveValue")), 0.875); TestFalse(TEXT("Cleared switch falls back to parent"), Row(ClearRead, TEXT("Toggle"))->GetBoolField(TEXT("effectiveValue")));
	auto Legacy = Target(); Legacy->SetStringField(TEXT("parameterName"), TEXT("Strength")); Legacy->SetNumberField(TEXT("value"), 0.625);
	const auto Single = Registry->FindTool(TEXT("content.material.instance.parameter.set"))->Execute(Legacy); TestTrue(TEXT("Legacy setter uses verified mutation"), Single.bSuccess); if (Single.bSuccess) TestFalse(TEXT("Legacy setter defaults to no save"), Single.Data->GetBoolField(TEXT("saved")));
	bool bCorruptOnce = true;
	const auto FaultHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([&](UObject* Object, FPropertyChangedEvent&) { if (Object == MI && bCorruptOnce) { bCorruptOnce = false; MI->ScalarParameterValues[0].ParameterValue = 0.99f; } });
	const auto RejectedReadback = Batch({Edit(TEXT("set"), TEXT("Strength"), TEXT("scalar"), MakeShared<FJsonValueNumber>(0.125))});
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(FaultHandle);
	TestFalse(TEXT("Readback detects a conflicting post-edit callback"), RejectedReadback.bSuccess);
	TestTrue(TEXT("Failed readback restores prior parameter state"), RejectedReadback.Data.IsValid() && RejectedReadback.Data->GetBoolField(TEXT("restoreVerified")));
	TestEqual(TEXT("Restored effective value matches preceding edit"), Row(Query(), TEXT("Strength"))->GetNumberField(TEXT("effectiveValue")), 0.625);
	const FString Filename = FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension()));
	if (FPaths::IsUnderDirectory(Filename, TEXT("S:/tmp")))
	{
		Legacy->SetBoolField(TEXT("save"), true); Updates = 0;
		TestFalse(TEXT("Saving a non-standalone fixture is rejected before UE's assertion"), Registry->FindTool(TEXT("content.material.instance.parameter.set"))->Execute(Legacy).bSuccess);
		TestEqual(TEXT("Invalid save target caused no mutation notification"), Updates, 0);
		MI->SetFlags(RF_Public | RF_Standalone);
		ON_SCOPE_EXIT { MI->ClearFlags(RF_Public | RF_Standalone); };
		int32 SaveCount = 0; const auto SaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddLambda([&](UObject* Object, FObjectPreSaveContext) { if (Object == MI) ++SaveCount; });
		Legacy->SetBoolField(TEXT("save"), true); Updates = 0;
		const auto Saved = Registry->FindTool(TEXT("content.material.instance.parameter.set"))->Execute(Legacy);
		FCoreUObjectDelegates::OnObjectPreSave.Remove(SaveHandle);
		TestTrue(TEXT("Explicit save persists existing dirty content even for a no-op edit"), Saved.bSuccess && Saved.Data->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("Explicit save happens once"), SaveCount, 1); TestEqual(TEXT("Saving a no-op does not refresh again"), Updates, 0);
		TestTrue(TEXT("Explicit save wrote the fixture package"), IFileManager::Get().FileExists(*Filename));
		M->SetFlags(RF_Public | RF_Standalone); ON_SCOPE_EXIT { M->ClearFlags(RF_Public | RF_Standalone); };
		auto Add = MakeShared<FJsonObject>(); Add->SetStringField(TEXT("material"), M->GetPathName()); Add->SetStringField(TEXT("expressionClass"), TEXT("ScalarParameter"));
		const auto Added = Registry->FindTool(TEXT("content.material.expression.add"))->Execute(Add);
		TestTrue(TEXT("Asset expression creation returns a usable node without an open editor"), Added.bSuccess && !Added.Data->GetStringField(TEXT("nodeId")).IsEmpty());
		if (Added.bSuccess && !Added.Data->GetStringField(TEXT("nodeId")).IsEmpty())
		{
			auto Set = MakeShared<FJsonObject>(); Set->SetStringField(TEXT("material"), M->GetPathName()); Set->SetStringField(TEXT("nodeId"), Added.Data->GetStringField(TEXT("nodeId"))); Set->SetStringField(TEXT("parameterName"), TEXT("CreatedHeadless")); Set->SetNumberField(TEXT("value"), 0.125);
			TestTrue(TEXT("New asset node can be edited immediately"), Registry->FindTool(TEXT("content.material.expression.value.set"))->Execute(Set).bSuccess);
		}
		TestTrue(TEXT("Remove only the test-created package under S:/tmp"), IFileManager::Get().Delete(*Filename, false, true));
	}
	else AddWarning(TEXT("Explicit filesystem save acceptance skipped outside S:/tmp; dirty-only behavior still tested."));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceLayerBatchTest, "UE_AI_integration.MaterialInstance.LayerIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialInstanceLayerBatchTest::RunTest(const FString&)
{
	auto* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/InstanceLayer_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	auto* M = NewObject<UMaterial>(Package, TEXT("Material"), RF_Transactional); FGCObjectScopeGuard MaterialGuard(M);
	auto* Layer = NewObject<UMaterialFunctionMaterialLayer>(Package, TEXT("Layer"), RF_Transactional); FGCObjectScopeGuard LayerGuard(Layer);
	auto* Gain = NewObject<UMaterialExpressionScalarParameter>(Layer); Gain->Function = Layer; Gain->ParameterName = TEXT("Shared"); Gain->DefaultValue = 0.5; Gain->ExpressionGUID = FGuid::NewGuid(); Layer->GetExpressionCollection().AddExpression(Gain);
	auto* Attributes = NewObject<UMaterialExpressionMakeMaterialAttributes>(Layer); Attributes->Function = Layer; Attributes->Roughness.Connect(0, Gain); Layer->GetExpressionCollection().AddExpression(Attributes);
	auto* Output = NewObject<UMaterialExpressionFunctionOutput>(Layer); Output->Function = Layer; Output->OutputName = TEXT("Result"); Output->Id = FGuid::NewGuid(); Output->A.Connect(0, Attributes); Layer->GetExpressionCollection().AddExpression(Output);
	auto* Layers = NewObject<UMaterialExpressionMaterialAttributeLayers>(M); Layers->Material = M; M->GetExpressionCollection().AddExpression(Layers);
	Layers->DefaultLayers.Empty(); Layers->DefaultLayers.AddDefaultBackgroundLayer(); Layers->DefaultLayers.AppendBlendedLayer(); Layers->DefaultLayers.Layers[0] = Layer; Layers->DefaultLayers.Layers[1] = Layer;
	M->bUseMaterialAttributes = true; M->GetExpressionInputForProperty(MP_MaterialAttributes)->Connect(0, Layers); M->UpdateCachedExpressionData();
	auto* MI = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Transactional); FGCObjectScopeGuard InstanceGuard(MI); MI->SetParentEditorOnly(M, false);
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	auto Target = [&]() { auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("instance"), MI->GetPathName()); return P; };
	auto Query = [&]() { return Registry->FindTool(TEXT("content.material.instance.parameters.get"))->Execute(Target()); };
	const auto Before = Query(); if (!TestTrue(TEXT("Layer parameter catalog readable"), Before.bSuccess)) { AddError(Before.ErrorMessage); return false; }
	int32 LayerEntries = 0; for (const auto& V : Before.Data->GetArrayField(TEXT("parameters"))) if (V->AsObject()->GetStringField(TEXT("name")) == TEXT("Shared") && V->AsObject()->GetStringField(TEXT("association")) == TEXT("layer")) ++LayerEntries;
	if (!TestEqual(TEXT("Repeated function retains two distinct layer parameter identities"), LayerEntries, 2)) return false;
	auto P = Target(); P->SetStringField(TEXT("expectedStateHash"), Before.Data->GetStringField(TEXT("stateHash")));
	P->SetArrayField(TEXT("operations"), {MakeShared<FJsonValueObject>(Edit(TEXT("set"), TEXT("Shared"), TEXT("scalar"), MakeShared<FJsonValueNumber>(0.25), TEXT("layer"), 0)), MakeShared<FJsonValueObject>(Edit(TEXT("set"), TEXT("Shared"), TEXT("scalar"), MakeShared<FJsonValueNumber>(0.75), TEXT("layer"), 1))});
	const auto Applied = Registry->FindTool(TEXT("content.material.instance.parameters.batch"))->Execute(P); if (!TestTrue(TEXT("Separate layer overrides apply in one batch"), Applied.bSuccess)) { AddError(Applied.ErrorMessage); return false; }
	float First = 0, Second = 0; MI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Shared"), LayerParameter, 0), First); MI->GetScalarParameterValue(FMaterialParameterInfo(TEXT("Shared"), LayerParameter, 1), Second);
	TestEqual(TEXT("First layer has its own value"), First, 0.25f); TestEqual(TEXT("Second layer has its own value"), Second, 0.75f);
	P->SetStringField(TEXT("expectedStateHash"), Query().Data->GetStringField(TEXT("stateHash")));
	Layers->DefaultLayers.EditorOnly.LayerGuids.Swap(0,1); M->UpdateCachedExpressionData(); MI->UpdateCachedData();
	const auto Stale = Registry->FindTool(TEXT("content.material.instance.parameters.batch"))->Execute(P);
	TestFalse(TEXT("Layer layout change invalidates old indices even for identical functions"), Stale.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMaterialInstanceSetParentTest, "UE_AI_integration.MaterialInstance.SetParent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMaterialInstanceSetParentTest::RunTest(const FString&)
{
	auto* Package = CreatePackage(*FString::Printf(TEXT("/Game/Automation/InstanceSetParent_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	auto* A = NewObject<UMaterial>(Package, TEXT("ParentA"), RF_Transactional); FGCObjectScopeGuard AGuard(A); A->SetShadingModel(MSM_Unlit);
	auto* B = NewObject<UMaterial>(Package, TEXT("ParentB"), RF_Transactional); FGCObjectScopeGuard BGuard(B); B->SetShadingModel(MSM_Unlit);
	auto* StrengthA = InstanceExpression<UMaterialExpressionScalarParameter>(A, TEXT("StrengthA")); StrengthA->ParameterName = TEXT("Strength"); StrengthA->DefaultValue = 0.25f;
	auto* TintA = InstanceExpression<UMaterialExpressionVectorParameter>(A, TEXT("TintA")); TintA->ParameterName = TEXT("Tint"); TintA->DefaultValue = FLinearColor::White;
	A->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, StrengthA); A->PostEditChange();
	auto* StrengthB = InstanceExpression<UMaterialExpressionScalarParameter>(B, TEXT("StrengthB")); StrengthB->ParameterName = TEXT("Strength"); StrengthB->DefaultValue = 1.0f;
	auto* ExtraB = InstanceExpression<UMaterialExpressionScalarParameter>(B, TEXT("ExtraB")); ExtraB->ParameterName = TEXT("Extra"); ExtraB->DefaultValue = 2.0f;
	B->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, StrengthB); B->PostEditChange();
	auto* MI = NewObject<UMaterialInstanceConstant>(Package, TEXT("Instance"), RF_Transactional); FGCObjectScopeGuard MIGuard(MI);
	MI->SetParentEditorOnly(A); MI->PostEditChange();
	MI->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Strength")), 0.5f);
	MI->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Tint")), FLinearColor::Red);
	MI->PostEditChange();
	auto* Registry = GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()->GetRegistry();
	auto* Tool = Registry->FindTool(TEXT("content.material.instance.set_parent"));
	if (!TestNotNull(TEXT("set_parent tool registered"), Tool)) return false;
	auto Target = [&]() { auto P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("instance"), MI->GetPathName()); return P; };
	auto P = Target(); P->SetStringField(TEXT("parent"), B->GetPathName());
	const auto Result = Tool->Execute(P);
	if (!TestTrue(TEXT("set_parent succeeds"), Result.bSuccess)) { AddError(Result.ErrorMessage); if (Result.Data) { FString Json; auto W = TJsonWriterFactory<>::Create(&Json); FJsonSerializer::Serialize(Result.Data.ToSharedRef(), W); AddInfo(Json); } return false; }
	TestEqual(TEXT("Parent migrated"), MI->Parent.Get(), static_cast<UMaterialInterface*>(B));
	TestEqual(TEXT("Retained count"), Result.Data->GetIntegerField(TEXT("retainedCount")), 1);
	TestEqual(TEXT("Dropped count"), Result.Data->GetIntegerField(TEXT("droppedCount")), 1);
	bool bHasStrength = false, bHasTint = false;
	for (const auto& V : Result.Data->GetArrayField(TEXT("retainedParameters"))) if (V->AsObject()->GetStringField(TEXT("name")) == TEXT("Strength")) bHasStrength = true;
	for (const auto& V : Result.Data->GetArrayField(TEXT("droppedParameters"))) if (V->AsObject()->GetStringField(TEXT("name")) == TEXT("Tint")) bHasTint = true;
	TestTrue(TEXT("Strength reported retained"), bHasStrength);
	TestTrue(TEXT("Tint reported dropped"), bHasTint);
	auto* Child = NewObject<UMaterialInstanceConstant>(Package, TEXT("Child"), RF_Transactional); FGCObjectScopeGuard ChildGuard(Child);
	Child->SetParentEditorOnly(MI); Child->PostEditChange();
	auto Cycle = Target(); Cycle->SetStringField(TEXT("parent"), Child->GetPathName());
	const auto Rejected = Tool->Execute(Cycle);
	TestFalse(TEXT("Descendant parent rejected"), Rejected.bSuccess);
	TestEqual(TEXT("Cycle error code"), Rejected.ErrorCode, FString(TEXT("material_instance_parent_cycle")));
	TestEqual(TEXT("Parent unchanged after rejection"), MI->Parent.Get(), static_cast<UMaterialInterface*>(B));
	return true;
}
#endif
