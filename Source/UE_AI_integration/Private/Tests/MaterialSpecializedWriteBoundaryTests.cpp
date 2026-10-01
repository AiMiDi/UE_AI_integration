#if WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "UEAIIntegrationSubsystem.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialGraphIdentity.h"
#include "Infrastructure/MaterialGraphSnapshot.h"
#include "Infrastructure/MaterialSharedWriteProtection.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionNamedReroute.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
template <typename T>
T* NewSharedWriterExpression(UObject* Owner)
{
	T* Expression = NewObject<T>(Owner, NAME_None, RF_Transactional);
	if (auto* Material = Cast<UMaterial>(Owner))
	{
		Material->GetExpressionCollection().AddExpression(Expression);
		Expression->Material = Material;
	}
	else if (auto* Function = Cast<UMaterialFunction>(Owner))
	{
		Function->GetExpressionCollection().AddExpression(Expression);
		Expression->Function = Function;
	}
	return Expression;
}

bool AttachSharedWriterProof(
	UObject* Asset,
	const TArray<FString>& NodeIds,
	const TSharedRef<FJsonObject>& Params,
	TArray<FString>& Snapshots,
	const FString& PreviewId = FString(),
	const bool bIncludeNamedReroutes = true,
	const bool bConsumerSelectionOnly = false)
{
	using namespace UEAIIntegration::MaterialQuery;
	const FMCPToolResult Snapshot = Capture(Asset, FString(), PreviewId, nullptr, bIncludeNamedReroutes);
	if (!Snapshot.bSuccess || !Snapshot.Data) return false;
	const FString SnapshotId = Snapshot.Data->GetStringField(TEXT("snapshotId"));
	Snapshots.Add(SnapshotId);
	auto Query = MakeShared<FJsonObject>();
	Query->SetStringField(TEXT("snapshotId"), SnapshotId);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Id : NodeIds) Values.Add(MakeShared<FJsonValueString>(Id));
	Query->SetArrayField(TEXT("nodeIds"), Values);
	Query->SetStringField(TEXT("direction"), bConsumerSelectionOnly ? TEXT("downstream") : TEXT("upstream"));
	Query->SetNumberField(TEXT("depth"), 8);
	const FMCPToolResult Boundary = UEAIIntegration::MaterialQuery::Boundary(Query);
	if (!Boundary.bSuccess || !Boundary.Data) return false;
	Params->SetStringField(TEXT("snapshotId"), SnapshotId);
	Params->SetStringField(TEXT("boundaryId"), Boundary.Data->GetStringField(TEXT("boundaryId")));
	Params->SetStringField(TEXT("expectedProjectionHash"), Snapshot.Data->GetStringField(TEXT("projectionHash")));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialSpecializedSharedWriterTest,
	"UE_AI_integration.MaterialCustom.SharedSpecializedWriterContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSpecializedSharedWriterTest::RunTest(const FString&)
{
	auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	auto* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	if (!TestNotNull(TEXT("Specialized writer registry exists"), Registry)) return false;
	TArray<FString> Snapshots;
	ON_SCOPE_EXIT
	{
		for (const FString& Id : Snapshots) UEAIIntegration::MaterialQuery::Release(Id);
	};
	for (int32 Writer = 0; Writer < 3; ++Writer)
	{
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(
			CreatePackage(*(TEXT("/Game/Automation/UEAI_SharedWriter_") + Suffix)), TEXT("Material"), RF_Transactional));
		TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>(
			CreatePackage(*(TEXT("/Game/Automation/UEAI_SharedWriterFunction_") + Suffix)), TEXT("Function"), RF_Transactional));
		TStrongObjectPtr<UMaterialFunction> InitialFunction(NewObject<UMaterialFunction>(
			CreatePackage(*(TEXT("/Game/Automation/UEAI_SharedWriterInitialFunction_") + Suffix)), TEXT("Function"), RF_Transactional));
		auto* Output = NewSharedWriterExpression<UMaterialExpressionFunctionOutput>(Function.Get());
		Output->OutputName = TEXT("Result");
		Output->Id = FGuid::NewGuid();
		Output->A.Connect(0, NewSharedWriterExpression<UMaterialExpressionConstant>(Function.Get()));
		auto* InitialOutput = NewSharedWriterExpression<UMaterialExpressionFunctionOutput>(InitialFunction.Get());
		InitialOutput->OutputName = TEXT("Result");
		InitialOutput->Id = FGuid::NewGuid();
		InitialOutput->A.Connect(0, NewSharedWriterExpression<UMaterialExpressionConstant>(InitialFunction.Get()));
		UMaterialExpression* Expression = Writer == 0
			? static_cast<UMaterialExpression*>(NewSharedWriterExpression<UMaterialExpressionCustom>(Material.Get()))
			: Writer == 1
				? static_cast<UMaterialExpression*>(NewSharedWriterExpression<UMaterialExpressionScalarParameter>(Material.Get()))
				: static_cast<UMaterialExpression*>(NewSharedWriterExpression<UMaterialExpressionMaterialFunctionCall>(Material.Get()));
		if (auto* Parameter = Cast<UMaterialExpressionScalarParameter>(Expression)) Parameter->ParameterName = TEXT("SharedGain");
		auto* Consumer = NewSharedWriterExpression<UMaterialExpressionAdd>(Material.Get());
		const TCHAR* SetId = Writer == 0 ? TEXT("content.material.custom.set")
			: Writer == 1 ? TEXT("content.material.parameter.set") : TEXT("content.material.function.call.set");
		const TCHAR* GetId = Writer == 0 ? TEXT("content.material.custom.get")
			: Writer == 1 ? TEXT("content.material.parameter.list") : TEXT("content.material.function.call.get");
		auto Params = [&]()
		{
			auto P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("material"), Material->GetPathName());
			P->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Expression));
			auto Context = MakeShared<FJsonObject>();
			Context->SetBoolField(TEXT("deferCompile"), true);
			P->SetObjectField(TEXT("__ueWorkflow"), Context);
			return P;
		};
		auto ReadHash = [&]()
		{
			const FMCPToolResult Read = Registry->FindTool(GetId)->Execute(Params());
			if (!Read.bSuccess || !Read.Data) return FString();
			if (Writer == 1)
			{
				const auto& Rows = Read.Data->GetArrayField(TEXT("parameters"));
				return Rows.Num() == 1 ? Rows[0]->AsObject()->GetStringField(TEXT("stateHash")) : FString();
			}
			return Read.Data->GetStringField(TEXT("stateHash"));
		};
		auto Mutation = [&]()
		{
			auto P = Params();
			if (Writer == 0) P->SetStringField(TEXT("code"), TEXT("return 0.75;"));
			else if (Writer == 1) P->SetNumberField(TEXT("defaultValue"), 0.75);
			else { P->SetStringField(TEXT("function"), Function->GetPathName()); P->SetBoolField(TEXT("disconnectRemoved"), true); }
			return P;
		};
		auto Initial = Mutation();
		if (Writer == 0) Initial->SetStringField(TEXT("code"), TEXT("return 0.25;"));
		else if (Writer == 1) Initial->SetNumberField(TEXT("defaultValue"), 0.25);
		else Initial->SetStringField(TEXT("function"), InitialFunction->GetPathName());
		const FMCPToolResult Ordinary = Registry->FindTool(SetId)->Execute(Initial);
		if (!TestTrue(TEXT("Unshared specialized write stays compatible"), Ordinary.bSuccess)) return false;
		TestFalse(TEXT("Unshared write does not invent a verified boundary"), Ordinary.Data->GetBoolField(TEXT("writeBoundaryVerified")));
		Consumer->A.Connect(0, Expression);
		Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Expression);
		Material->GetOutermost()->SetDirtyFlag(false);
		const FString BaselineHash = ReadHash();
		if (!TestFalse(TEXT("Specialized baseline has a state hash"), BaselineHash.IsEmpty())) return false;
		auto CheckRefusal = [&](const TSharedRef<FJsonObject>& P, const TCHAR* Code)
		{
			const FMCPToolResult Rejected = Registry->FindTool(SetId)->Execute(P);
			TestFalse(TEXT("Unsafe specialized edit is rejected"), Rejected.bSuccess);
			TestEqual(TEXT("Refusal publishes the boundary conflict"), Rejected.ErrorCode, FString(Code));
			TestEqual(TEXT("Refusal preserves the full specialized state"), ReadHash(), BaselineHash);
			TestFalse(TEXT("Refusal leaves the package clean"), Material->GetOutermost()->IsDirty());
			TestTrue(TEXT("Refusal preserves the other consumer"), Consumer->A.Expression == Expression);
			TestTrue(TEXT("Refusal preserves the root consumer"), Material->GetExpressionInputForProperty(MP_EmissiveColor)->Expression == Expression);
		};
		auto P = Mutation();
		P->GetObjectField(TEXT("__ueWorkflow"))->SetBoolField(TEXT("approvedPlan"), true);
		CheckRefusal(P, TEXT("material_boundary_required_for_mutation"));
		P = Mutation();
		if (!TestTrue(TEXT("Shared writer boundary captures"), AttachSharedWriterProof(Material.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Expression)}, P, Snapshots))) return false;
		CheckRefusal(P, TEXT("material_boundary_shared_node_confirmation_required"));
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		P->SetStringField(TEXT("expectedProjectionHash"), TEXT("sha256:wrong"));
		CheckRefusal(P, TEXT("material_boundary_identity_mismatch"));
		P = Mutation();
		if (!TestTrue(TEXT("Wrong-selection proof captures"), AttachSharedWriterProof(Material.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Consumer)}, P, Snapshots, FString(), true, true))) return false;
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		CheckRefusal(P, TEXT("material_boundary_node_outside_selection"));
		TStrongObjectPtr<UMaterial> OtherMaterial(NewObject<UMaterial>(
			CreatePackage(*(TEXT("/Game/Automation/UEAI_SharedWriterOther_") + Suffix)), TEXT("Material"), RF_Transactional));
		auto* OtherExpression = NewSharedWriterExpression<UMaterialExpressionConstant>(OtherMaterial.Get());
		P = Mutation();
		if (!TestTrue(TEXT("Cross-asset proof captures"), AttachSharedWriterProof(OtherMaterial.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(OtherExpression)}, P, Snapshots))) return false;
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		CheckRefusal(P, TEXT("material_boundary_asset_mismatch"));
		P = Mutation();
		if (!TestTrue(TEXT("Preview proof captures"), AttachSharedWriterProof(Material.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Expression)}, P, Snapshots, TEXT("contract-preview")))) return false;
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		CheckRefusal(P, TEXT("material_boundary_preview_writer_unsupported"));
		P = Mutation();
		if (!TestTrue(TEXT("Stale proof captures"), AttachSharedWriterProof(Material.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Expression)}, P, Snapshots))) return false;
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		Expression->MaterialExpressionEditorX += 17;
		CheckRefusal(P, TEXT("material_boundary_stale"));
		P = Mutation();
		if (!TestTrue(TEXT("Released proof captures"), AttachSharedWriterProof(Material.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Expression)}, P, Snapshots))) return false;
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		UEAIIntegration::MaterialQuery::Release(P->GetStringField(TEXT("snapshotId")));
		CheckRefusal(P, TEXT("material_boundary_unavailable"));
		P = Mutation();
		if (!TestTrue(TEXT("All-consumer proof captures"), AttachSharedWriterProof(Material.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Expression), MCPMaterialInfrastructure::ExpressionNodeId(Consumer), TEXT("root")}, P, Snapshots))) return false;
		CheckRefusal(P, TEXT("material_boundary_shared_node_confirmation_required"));
		P->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		const FMCPToolResult Applied = Registry->FindTool(SetId)->Execute(P);
		if (!TestTrue(TEXT("Fresh explicitly confirmed shared edit succeeds"), Applied.bSuccess)) return false;
		TestTrue(TEXT("Confirmed request actually changes the authored state"), Applied.Data->GetBoolField(TEXT("changed")));
		TestNotEqual(TEXT("Confirmed write has a different state hash"), ReadHash(), BaselineHash);
		TestTrue(TEXT("Confirmed edit preserves the other consumer"), Consumer->A.Expression == Expression);
		TestTrue(TEXT("Confirmed edit preserves the root consumer"), Material->GetExpressionInputForProperty(MP_EmissiveColor)->Expression == Expression);
		TestTrue(TEXT("Confirmed result proves its write boundary"), Applied.Data->GetBoolField(TEXT("writeBoundaryVerified")));
		TestTrue(TEXT("Confirmed result reports shared-node detection"), Applied.Data->GetBoolField(TEXT("sharedNodeImpactDetected")));
		TestTrue(TEXT("Confirmed result reports explicit confirmation"), Applied.Data->GetBoolField(TEXT("sharedNodeImpactConfirmed")));
		TestEqual(TEXT("Confirmed result retains the exact fresh projection"), Applied.Data->GetStringField(TEXT("freshProjectionHash")), P->GetStringField(TEXT("expectedProjectionHash")));
		TestFalse(TEXT("Confirmed deferred result never saves"), Applied.Data->GetBoolField(TEXT("saved")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialSpecializedNamedRerouteWriterTest,
	"UE_AI_integration.MaterialCustom.NamedRerouteSharedWriterContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSpecializedNamedRerouteWriterTest::RunTest(const FString&)
{
	auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	auto* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	if (!TestNotNull(TEXT("Named-reroute writer registry exists"), Registry)) return false;
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(CreatePackage(
		*(TEXT("/Game/Automation/UEAI_NamedSharedWriter_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), TEXT("Material"), RF_Transactional));
	auto* Parameter = NewSharedWriterExpression<UMaterialExpressionScalarParameter>(Material.Get());
	Parameter->ParameterName = TEXT("SharedGain");
	auto* Declaration = NewSharedWriterExpression<UMaterialExpressionNamedRerouteDeclaration>(Material.Get());
	Declaration->VariableGuid = FGuid::NewGuid();
	Declaration->Input.Connect(0, Parameter);
	auto* First = NewSharedWriterExpression<UMaterialExpressionNamedRerouteUsage>(Material.Get());
	auto* Second = NewSharedWriterExpression<UMaterialExpressionNamedRerouteUsage>(Material.Get());
	First->Declaration = Declaration; First->DeclarationGuid = Declaration->VariableGuid;
	Second->Declaration = Declaration; Second->DeclarationGuid = Declaration->VariableGuid;
	auto* Consumer = NewSharedWriterExpression<UMaterialExpressionAdd>(Material.Get());
	Consumer->A.Connect(0, First);
	Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Second);
	Material->GetOutermost()->SetDirtyFlag(false);
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("material"), Material->GetPathName());
	Params->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Parameter));
	Params->SetNumberField(TEXT("defaultValue"), 3.0);
	const FMCPToolResult Rejected = Registry->FindTool(TEXT("content.material.parameter.set"))->Execute(Params);
	TestEqual(TEXT("Named reroutes do not hide shared consumers"), Rejected.ErrorCode, FString(TEXT("material_boundary_required_for_mutation")));
	TestEqual(TEXT("Named-reroute refusal preserves the value"), Parameter->DefaultValue, 0.0f);
	TestFalse(TEXT("Named-reroute refusal preserves dirty state"), Material->GetOutermost()->IsDirty());
	TArray<FString> Snapshots;
	ON_SCOPE_EXIT
	{
		for (const FString& Id : Snapshots) UEAIIntegration::MaterialQuery::Release(Id);
	};
	if (!TestTrue(TEXT("Incomplete named-reroute proof captures"), AttachSharedWriterProof(Material.Get(),
		{MCPMaterialInfrastructure::ExpressionNodeId(Parameter)}, Params, Snapshots, FString(), false))) return false;
	Params->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
	const FMCPToolResult Incomplete = Registry->FindTool(TEXT("content.material.parameter.set"))->Execute(Params);
	TestEqual(TEXT("Omitted implicit-edge coverage is rejected"), Incomplete.ErrorCode,
		FString(TEXT("material_boundary_named_reroute_coverage_required")));
	TestEqual(TEXT("Incomplete coverage does not edit the value"), Parameter->DefaultValue, 0.0f);
	TestFalse(TEXT("Incomplete coverage preserves dirty state"), Material->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialSpecializedDeferredGraphWriterTest,
	"UE_AI_integration.MaterialCustom.DeferredGraphPinsSharedWriterContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialSpecializedDeferredGraphWriterTest::RunTest(const FString&)
{
	auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	auto* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	if (!TestNotNull(TEXT("Deferred graph writer registry exists"), Registry)) return false;
	TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(CreatePackage(
		*(TEXT("/Game/Automation/UEAI_DeferredSharedWriter_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))), TEXT("Material"), RF_Transactional));
	auto* Parameter = NewSharedWriterExpression<UMaterialExpressionScalarParameter>(Material.Get());
	Parameter->ParameterName = TEXT("DeferredGain");
	auto* Consumer = NewSharedWriterExpression<UMaterialExpressionAdd>(Material.Get());
	Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Parameter);
	MCPMaterialInfrastructure::EnsureMaterialGraph(Material.Get());
	auto* SourceNode = Cast<UMaterialGraphNode>(Parameter->GraphNode);
	auto* ConsumerNode = Cast<UMaterialGraphNode>(Consumer->GraphNode);
	if (!TestNotNull(TEXT("Deferred source has a native graph node"), SourceNode)
		|| !TestNotNull(TEXT("Deferred consumer has a native graph node"), ConsumerNode)) return false;
	if (!TestNotNull(TEXT("Deferred source has an output pin"), SourceNode->GetOutputPin(0))
		|| !TestNotNull(TEXT("Deferred consumer has an input pin"), ConsumerNode->GetInputPin(0))) return false;
	SourceNode->GetOutputPin(0)->MakeLinkTo(ConsumerNode->GetInputPin(0));
	Material->GetOutermost()->SetDirtyFlag(false);
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("material"), Material->GetPathName());
	Params->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Parameter));
	Params->SetNumberField(TEXT("defaultValue"), 4.0);
	const FMCPToolResult Rejected = Registry->FindTool(TEXT("content.material.parameter.set"))->Execute(Params);
	TestEqual(TEXT("Uncopied graph links cannot bypass shared-node protection"), Rejected.ErrorCode,
		FString(TEXT("material_boundary_required_for_mutation")));
	TestEqual(TEXT("Deferred-link refusal preserves the value"), Parameter->DefaultValue, 0.0f);
	TestNull(TEXT("Refusal does not copy graph pins into authored inputs"), Consumer->A.Expression);
	TestTrue(TEXT("Refusal preserves the deferred graph link"),
		SourceNode->GetOutputPin(0)->LinkedTo.Contains(ConsumerNode->GetInputPin(0)));
	TestFalse(TEXT("Deferred-link refusal preserves dirty state"), Material->GetOutermost()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialMixedConsumerIdentityTest,
	"UE_AI_integration.MaterialCustom.MixedConsumerIdentityContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialMixedConsumerIdentityTest::RunTest(const FString&)
{
	using namespace UEAIIntegration::MaterialEditing;
	// Material Function shared-graph protection has a dedicated regression below.
	// Keep this mixed-consumer identity test on the two UMaterial shapes whose
	// native graph fixture can be constructed without a preview UMaterial.
	for (int32 Kind = 0; Kind < 2; ++Kind)
	{
		const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>(CreatePackage(
			*(TEXT("/Game/Automation/UEAI_MixedConsumerMaterial_") + Suffix)), TEXT("Material"), RF_Transactional));
		UObject* Asset = Material.Get();
		auto* Source = NewSharedWriterExpression<UMaterialExpressionScalarParameter>(Asset);
		Source->ParameterName = TEXT("MixedGain");
		auto* Consumer = NewSharedWriterExpression<UMaterialExpressionAdd>(Asset);
		if (Kind == 0) Material->GetExpressionInputForProperty(MP_BaseColor)->Connect(0, Source);
		else Consumer->A.Connect(0, Source);
		MCPMaterialInfrastructure::EnsureMaterialGraph(Material.Get());
		UMaterialGraph* Graph = Cast<UMaterialGraph>(Material->MaterialGraph);
		auto* SourceNode = Cast<UMaterialGraphNode>(Source->GraphNode);
		auto* ConsumerNode = Cast<UMaterialGraphNode>(Consumer->GraphNode);
		if (!TestNotNull(TEXT("Mixed source graph exists"), SourceNode)
			|| !TestNotNull(TEXT("Mixed consumer graph exists"), ConsumerNode)
			|| !TestNotNull(TEXT("Mixed graph exists"), Graph)) return false;
		FMaterialSharedWriteProof Proof;
		InspectMaterialExpressionConsumers(Asset, Source, Proof);
		TestEqual(TEXT("Synchronized authored/graph edge counts once"), Proof.ConsumerConnectionCount, 1);
		TestFalse(TEXT("Synchronized single edge is unshared"), Proof.bShared);
		UEdGraphPin* FirstPin = Kind == 0
			? Graph->RootNode->GetInputPin(Graph->GetInputIndexForProperty(MP_BaseColor))
			: ConsumerNode->GetInputPin(0);
		UEdGraphPin* SecondPin = Kind == 0
			? Graph->RootNode->GetInputPin(Graph->GetInputIndexForProperty(MP_EmissiveColor))
			: ConsumerNode->GetInputPin(1);
		if (!TestNotNull(TEXT("First input exists"), FirstPin)
			|| !TestNotNull(TEXT("Second input exists"), SecondPin)
			|| !TestNotNull(TEXT("Source output exists"), SourceNode->GetOutputPin(0))) return false;
		// The graph now carries only the second edge, while authored state keeps
		// only the first. Both representations have count one and one consumer.
		SourceNode->GetOutputPin(0)->BreakLinkTo(FirstPin);
		SourceNode->GetOutputPin(0)->MakeLinkTo(SecondPin);
		Asset->GetOutermost()->SetDirtyFlag(false);
		InspectMaterialExpressionConsumers(Asset, Source, Proof);
		TestEqual(TEXT("Mixed different inputs count as two connections"), Proof.ConsumerConnectionCount, 2);
		TestEqual(TEXT("Both connections reach one consumer node"), Proof.ConsumerNodeIds.Num(), 1);
		TestTrue(TEXT("Mixed connections on one consumer are shared"), Proof.bShared);
		auto Params = MakeShared<FJsonObject>();
		const FMCPToolResult Refused = ValidateMaterialExpressionSharedWrite(Asset, Source, Params, Proof);
		TestEqual(TEXT("Mixed shared state requires boundary before a value write"), Refused.ErrorCode,
			FString(TEXT("material_boundary_required_for_mutation")));
		TestFalse(TEXT("Read-only protection preserves package dirty state"), Asset->GetOutermost()->IsDirty());
		TestTrue(TEXT("Read-only protection preserves deferred link"), SourceNode->GetOutputPin(0)->LinkedTo.Contains(SecondPin));
		TestFalse(TEXT("Read-only protection does not restore stale graph link"), SourceNode->GetOutputPin(0)->LinkedTo.Contains(FirstPin));
		if (Kind == 0)
		{
			TestTrue(TEXT("Authored BaseColor remains connected"), Material->GetExpressionInputForProperty(MP_BaseColor)->Expression == Source);
			TestNull(TEXT("Deferred Emissive is not copied into authored inputs"), Material->GetExpressionInputForProperty(MP_EmissiveColor)->Expression);
		}
		else
		{
			TestTrue(TEXT("Authored first input remains connected"), Consumer->A.Expression == Source);
			TestNull(TEXT("Deferred second input is not copied into authored inputs"), Consumer->B.Expression);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialFunctionSharedMutationTest,
	"UE_AI_integration.MaterialFunction.SharedGraphMutationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialFunctionSharedMutationTest::RunTest(const FString&)
{
	auto* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	auto* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	if (!TestNotNull(TEXT("Function mutation registry exists"), Registry)) return false;
	TArray<FString> Snapshots;
	ON_SCOPE_EXIT
	{
		for (const FString& Id : Snapshots) UEAIIntegration::MaterialQuery::Release(Id);
	};
	for (int32 Operation = 0; Operation < 5; ++Operation)
	{
		TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>(CreatePackage(
			*(TEXT("/Game/Automation/UEAI_SharedFunctionMutation_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))),
			TEXT("Function"), RF_Transactional));
		auto* Source = NewSharedWriterExpression<UMaterialExpressionScalarParameter>(Function.Get());
		Source->ParameterName = TEXT("SharedGain");
		auto* Replacement = NewSharedWriterExpression<UMaterialExpressionConstant>(Function.Get());
		auto* Consumer = NewSharedWriterExpression<UMaterialExpressionAdd>(Function.Get());
		auto* Output = NewSharedWriterExpression<UMaterialExpressionFunctionOutput>(Function.Get());
		Output->OutputName = TEXT("Result");
		Output->Id = FGuid::NewGuid();
		Consumer->A.Connect(0, Source);
		Output->A.Connect(0, Source);
		Function->GetOutermost()->SetDirtyFlag(false);
		const TCHAR* Capability = Operation < 2 ? TEXT("content.material.pin.connect")
			: Operation < 4 ? TEXT("content.material.pin.disconnect") : TEXT("content.material.expression.delete");
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("materialFunction"), Function->GetPathName());
		auto Context = MakeShared<FJsonObject>();
		Context->SetBoolField(TEXT("deferCompile"), true);
		Context->SetBoolField(TEXT("approvedPlan"), true);
		Params->SetObjectField(TEXT("__ueWorkflow"), Context);
		if (Operation < 2)
		{
			Params->SetStringField(TEXT("sourceNodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Operation == 0 ? Source : static_cast<UMaterialExpression*>(Replacement)));
			Params->SetStringField(TEXT("sourcePinName"), TEXT("Output"));
			Params->SetStringField(TEXT("targetNodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Consumer));
			Params->SetStringField(TEXT("targetPinName"), Operation == 0 ? TEXT("B") : TEXT("A"));
		}
		else
		{
			Params->SetStringField(TEXT("nodeId"), MCPMaterialInfrastructure::ExpressionNodeId(Operation == 2
				? static_cast<UMaterialExpression*>(Consumer) : static_cast<UMaterialExpression*>(Source)));
			if (Operation < 4) Params->SetStringField(TEXT("pinName"), Operation == 2 ? TEXT("A") : TEXT("Output"));
		}
		auto CheckRefusal = [&](const TCHAR* Code)
		{
			const FMCPToolResult Refused = Registry->FindTool(Capability)->Execute(Params);
			TestEqual(TEXT("Function shared write refuses before mutation"), Refused.ErrorCode, FString(Code));
			TestTrue(TEXT("Refusal preserves first consumer"), Consumer->A.Expression == Source);
			TestTrue(TEXT("Refusal preserves second consumer"), Output->A.Expression == Source);
			TestNull(TEXT("Refusal preserves unconnected second input"), Consumer->B.Expression);
			TestTrue(TEXT("Refusal preserves source membership"), Function->GetExpressions().Contains(Source));
			TestEqual(TEXT("Refusal preserves function expression count"), Function->GetExpressions().Num(), 4);
			TestFalse(TEXT("Refusal preserves package dirty state"), Function->GetOutermost()->IsDirty());
		};
		CheckRefusal(TEXT("material_boundary_required_for_mutation"));
		// Selecting every node removes external-boundary impact. The actual native
		// fan-out still requires explicit confirmation for every operation.
		if (!TestTrue(TEXT("Whole-function proof captures"), AttachSharedWriterProof(Function.Get(),
			{MCPMaterialInfrastructure::ExpressionNodeId(Source), MCPMaterialInfrastructure::ExpressionNodeId(Replacement),
			 MCPMaterialInfrastructure::ExpressionNodeId(Consumer), MCPMaterialInfrastructure::ExpressionNodeId(Output)}, Params, Snapshots))) return false;
		CheckRefusal(TEXT("material_boundary_shared_node_confirmation_required"));
		Params->SetBoolField(TEXT("confirmSharedNodeImpact"), true);
		const FMCPToolResult Applied = Registry->FindTool(Capability)->Execute(Params);
		if (!TestTrue(TEXT("Confirmed function graph write succeeds"), Applied.bSuccess)) return false;
		TestTrue(TEXT("Function result proves shared detection"), Applied.Data->GetBoolField(TEXT("sharedNodeImpactDetected")));
		TestTrue(TEXT("Whole-function proof retains explicit confirmation"), Applied.Data->GetBoolField(TEXT("sharedNodeImpactConfirmed")));
		TestTrue(TEXT("Function result proves boundary verification"), Applied.Data->GetBoolField(TEXT("writeBoundaryVerified")));
		TestFalse(TEXT("Deferred function write does not save"), Applied.Data->GetBoolField(TEXT("saved")));
		TestEqual(TEXT("Function result preserves fresh identity"), Applied.Data->GetStringField(TEXT("freshProjectionHash")), Params->GetStringField(TEXT("expectedProjectionHash")));
		if (Operation == 0)
		{
			TestTrue(TEXT("Connect modifies requested second input"), Consumer->B.Expression == Source);
			TestTrue(TEXT("Connect preserves existing first input"), Consumer->A.Expression == Source);
			TestTrue(TEXT("Connect preserves other consumer"), Output->A.Expression == Source);
		}
		else if (Operation == 1)
		{
			TestTrue(TEXT("Replacement changes the requested input"), Consumer->A.Expression == Replacement);
			TestTrue(TEXT("Replacement preserves old source's other consumer"), Output->A.Expression == Source);
		}
		else if (Operation == 2)
		{
			TestNull(TEXT("Input disconnect removes requested edge"), Consumer->A.Expression);
			TestTrue(TEXT("Input disconnect preserves other consumer"), Output->A.Expression == Source);
		}
		else
		{
			TestNull(TEXT("Output disconnect/delete removes first consumer"), Consumer->A.Expression);
			TestNull(TEXT("Output disconnect/delete removes second consumer"), Output->A.Expression);
			TestEqual(TEXT("Only deletion removes source membership"), Function->GetExpressions().Contains(Source), Operation != 4);
		}
	}
	return true;
}

#endif
