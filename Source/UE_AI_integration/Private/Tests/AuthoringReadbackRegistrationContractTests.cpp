#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

namespace UEAIIntegrationTools
{
	void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
	void RegisterComponentTools(FMCPToolRegistry& Registry);
	void RegisterDispatcherTools(FMCPToolRegistry& Registry);
	void RegisterVariableTools(FMCPToolRegistry& Registry);

	void RegisterMaterialReadTools(FMCPToolRegistry& Registry);
	void RegisterMaterialGraphQueryTools(FMCPToolRegistry& Registry);
	void RegisterMaterialCustomTools(FMCPToolRegistry& Registry);

	void RegisterNiagaraTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraGraphTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraGraphEditTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry);
}

namespace
{
	void AssertRegistered(
		FAutomationTestBase& Test,
		const FMCPToolRegistry& Registry,
		const TCHAR* const* CapabilityIds,
		const int32 CapabilityCount,
		const TCHAR* Label)
	{
		for (int32 Index = 0; Index < CapabilityCount; ++Index)
		{
			const TCHAR* CapabilityId = CapabilityIds[Index];
			Test.TestNotNull(
				FString::Printf(TEXT("%s registers %s"), Label, CapabilityId),
				Registry.FindTool(CapabilityId));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAuthoringReadbackRegistrationContractTest,
	"UE_AI_integration.Authoring.ReadbackRegistrationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAuthoringReadbackRegistrationContractTest::RunTest(const FString&)
{
	// Keep this registry independent from the live subsystem. The test proves
	// that the public read/write/readback families can be bound together under
	// their owning domains even when optional Niagara support is unavailable.
	FMCPToolRegistry ContentRegistry;
	ContentRegistry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialReadTools(ContentRegistry);
	UEAIIntegrationTools::RegisterMaterialGraphQueryTools(ContentRegistry);
	UEAIIntegrationTools::RegisterMaterialCustomTools(ContentRegistry);
	UEAIIntegrationTools::RegisterNiagaraTools(ContentRegistry);
	UEAIIntegrationTools::RegisterNiagaraGraphTools(ContentRegistry);
	UEAIIntegrationTools::RegisterNiagaraGraphEditTools(ContentRegistry);
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(ContentRegistry);
	ContentRegistry.EndDomainRegistration();

	static const TCHAR* MaterialAndNiagaraIds[] = {
		TEXT("content.material.graph.index"),
		TEXT("content.material.graph.nodes.list"),
		TEXT("content.material.graph.subgraph.get"),
		TEXT("content.material.graph.boundary.get"),
		TEXT("content.material.graph.snapshot.release"),
		TEXT("content.material.custom.get"),
		TEXT("content.material.custom.set"),
		TEXT("content.material.editor.context.get"),
		TEXT("content.material.editor.batch"),
		TEXT("content.material.editor.apply.prepare"),
		TEXT("content.material.editor.apply.verify"),
		TEXT("content.material.instance.parameters.get"),
		TEXT("content.material.instance.parameters.batch"),
		TEXT("content.niagara.system.list"),
		TEXT("content.niagara.system.inspect"),
		TEXT("content.niagara.system.create"),
		TEXT("content.niagara.system.save"),
		TEXT("content.niagara.graph.inspect"),
		TEXT("content.niagara.graph.operations.list"),
		TEXT("content.niagara.renderer.list"),
		TEXT("content.niagara.renderer.materials.get"),
		TEXT("content.niagara.system.parameter.get"),
	};
	AssertRegistered(
		*this,
		ContentRegistry,
		MaterialAndNiagaraIds,
		UE_ARRAY_COUNT(MaterialAndNiagaraIds),
		TEXT("content"));

	FMCPToolRegistry BlueprintRegistry;
	BlueprintRegistry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(BlueprintRegistry);
	UEAIIntegrationTools::RegisterComponentTools(BlueprintRegistry);
	UEAIIntegrationTools::RegisterDispatcherTools(BlueprintRegistry);
	UEAIIntegrationTools::RegisterVariableTools(BlueprintRegistry);
	BlueprintRegistry.EndDomainRegistration();

	static const TCHAR* BlueprintIds[] = {
		TEXT("blueprint.asset.get"),
		TEXT("blueprint.graph.get"),
		TEXT("blueprint.graph.describe"),
		TEXT("blueprint.component.list"),
		TEXT("blueprint.component.get"),
		TEXT("blueprint.component.property.set"),
		TEXT("blueprint.component.reparent"),
		TEXT("blueprint.dispatcher.list"),
		TEXT("blueprint.variable.add"),
		TEXT("blueprint.variable.rename"),
		TEXT("blueprint.variable.type.set"),
	};
	AssertRegistered(
		*this,
		BlueprintRegistry,
		BlueprintIds,
		UE_ARRAY_COUNT(BlueprintIds),
		TEXT("blueprint"));

	// A nonexistent immutable snapshot is an expected recovery boundary. Reads
	// must return the canonical 410 error and must not invent an empty graph.
	const TSharedRef<FJsonObject> MissingSnapshot = MakeShared<FJsonObject>();
	MissingSnapshot->SetStringField(
		TEXT("snapshotId"),
		TEXT("ueai-readback-contract-missing-snapshot"));
	const FMCPToolResult MissingSnapshotResult = ContentRegistry.ExecuteTool(
		TEXT("content.material.graph.nodes.list"),
		MissingSnapshot);
	TestFalse(
		TEXT("Unknown Material snapshot is rejected"),
		MissingSnapshotResult.bSuccess);
	TestEqual(
		TEXT("Unknown Material snapshot uses graph_snapshot_unavailable"),
		MissingSnapshotResult.ErrorCode,
		FString(TEXT("graph_snapshot_unavailable")));
	TestEqual(
		TEXT("Unknown Material snapshot requires re-read via HTTP 410"),
		MissingSnapshotResult.HttpStatus,
		410);

	// This is a transport-independent error boundary: graph.get must reject an
	// unsupported geometry mode before trying to load a real Blueprint asset.
	const TSharedRef<FJsonObject> InvalidGeometry = MakeShared<FJsonObject>();
	InvalidGeometry->SetStringField(TEXT("geometryMode"), TEXT("unsupported"));
	const FMCPToolResult GeometryResult = BlueprintRegistry.ExecuteTool(
		TEXT("blueprint.graph.get"),
		InvalidGeometry);
	TestFalse(TEXT("Unsupported Blueprint geometry mode is rejected"), GeometryResult.bSuccess);
	TestEqual(
		TEXT("Unsupported geometry mode uses invalid_request"),
		GeometryResult.ErrorCode,
		FString(TEXT("invalid_request")));

	// Niagara is an optional module. Both the feature-enabled implementation and
	// the explicit unavailable implementation must reject an empty target rather
	// than touching unrelated loaded assets.
	const TSharedRef<FJsonObject> EmptySystemRequest = MakeShared<FJsonObject>();
	const FMCPToolResult NiagaraResult = ContentRegistry.ExecuteTool(
		TEXT("content.niagara.system.inspect"),
		EmptySystemRequest);
	TestFalse(TEXT("Empty Niagara inspection target is rejected"), NiagaraResult.bSuccess);
	TestTrue(
		TEXT("Empty Niagara inspection reports a bounded request/feature error"),
		NiagaraResult.ErrorCode == TEXT("invalid_niagara_system_request")
		|| NiagaraResult.ErrorCode == TEXT("capability_unavailable"));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
