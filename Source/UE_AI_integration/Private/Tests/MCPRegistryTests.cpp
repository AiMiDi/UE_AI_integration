#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
class FRegistryTestTool final : public FMCPToolBase
{
public:
	explicit FRegistryTestTool(FString InId)
		: Id(MoveTemp(InId))
	{
	}

	virtual FString GetCapabilityId() const override
	{
		return Id;
	}

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Ok(MakeShared<FJsonObject>());
	}

private:
	FString Id;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMCPRegistryCatalogTest,
	"UE_AI_integration.Registry.ExactManifestBindings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPRegistryCatalogTest::RunTest(const FString& Parameters)
{
	AddExpectedError(
		TEXT("Capability catalog is degraded"),
		EAutomationExpectedErrorFlags::Contains,
		3);

	FMCPToolRegistry Registry;

	TestFalse(
		TEXT("Catalog without implementations is degraded"),
		Registry.LoadCapabilityManifests());
	TestTrue(
		TEXT("Catalog preserves the shipped capability baseline"),
		Registry.GetCapabilityCount() >= 212);
	TestTrue(
		TEXT("Missing bindings produce validation errors"),
		!Registry.GetValidationErrors().IsEmpty());

	for (const TSharedPtr<FJsonObject>& Descriptor : Registry.GetCapabilityDescriptors())
	{
		Registry.BeginDomainRegistration(Descriptor->GetStringField(TEXT("domain")));
		Registry.Register(
			MakeShared<FRegistryTestTool>(Descriptor->GetStringField(TEXT("id"))));
		Registry.EndDomainRegistration();
	}

	TestTrue(TEXT("Exact manifest-to-implementation binding is ready"), Registry.LoadCapabilityManifests());
	TestTrue(TEXT("Registry reports ready"), Registry.IsReady());
	TestEqual(
		TEXT("Every manifest capability has one implementation"),
		Registry.Num(),
		Registry.GetCapabilityCount());
	TestEqual(TEXT("All six domains are present"), Registry.GetDomainCounts().Num(), 6);
	TestTrue(
		TEXT("Blueprint capability baseline"),
		Registry.GetDomainCounts().FindRef(TEXT("blueprint")) >= 58);
	TestTrue(
		TEXT("Scene capability baseline"),
		Registry.GetDomainCounts().FindRef(TEXT("scene")) >= 54);
	TestTrue(
		TEXT("Content capability baseline"),
		Registry.GetDomainCounts().FindRef(TEXT("content")) >= 59);
	TestTrue(
		TEXT("Animation capability baseline"),
		Registry.GetDomainCounts().FindRef(TEXT("animation")) >= 10);
	TestTrue(
		TEXT("AI capability baseline"),
		Registry.GetDomainCounts().FindRef(TEXT("ai")) >= 9);
	TestTrue(
		TEXT("Production capability baseline"),
		Registry.GetDomainCounts().FindRef(TEXT("production")) >= 22);
	TestTrue(TEXT("Exact binding has no validation errors"), Registry.GetValidationErrors().IsEmpty());
	TestNotNull(TEXT("PIE start is declared"), Registry.FindTool(TEXT("scene.pie.start")));
	TestNotNull(TEXT("PIE stop is declared"), Registry.FindTool(TEXT("scene.pie.stop")));
	TestNotNull(TEXT("PIE restart is declared"), Registry.FindTool(TEXT("scene.pie.restart")));
	TestNotNull(
		TEXT("Widget slot layout is declared"),
		Registry.FindTool(TEXT("content.widget.slot.layout.set")));
	TestNotNull(
		TEXT("Widget child removal is declared"),
		Registry.FindTool(TEXT("content.widget.child.remove")));
	TestNotNull(
		TEXT("Widget child reorder is declared"),
		Registry.FindTool(TEXT("content.widget.child.reorder")));
	TestNotNull(
		TEXT("Widget hierarchy query is declared"),
		Registry.FindTool(TEXT("content.widget.hierarchy.get")));

	TArray<FString> ParamErrors;
	TestFalse(
		TEXT("Manifest required fields reject empty params"),
		Registry.ValidateParams(
			TEXT("blueprint.asset.create"),
			MakeShared<FJsonObject>(),
			ParamErrors));
	TestTrue(TEXT("Invalid params include details"), !ParamErrors.IsEmpty());

	TSharedPtr<FJsonObject> BuildWithoutConfirmation = MakeShared<FJsonObject>();
	BuildWithoutConfirmation->SetStringField(TEXT("mode"), TEXT("ubt"));
	BuildWithoutConfirmation->SetBoolField(TEXT("confirmBuild"), false);
	ParamErrors.Reset();
	TestFalse(
		TEXT("JSON Schema const rejects an unconfirmed build"),
		Registry.ValidateParams(
			TEXT("production.build.target"),
			BuildWithoutConfirmation,
			ParamErrors));

	TSharedPtr<FJsonObject> ConfirmedBuild = MakeShared<FJsonObject>();
	ConfirmedBuild->SetStringField(TEXT("mode"), TEXT("ubt"));
	ConfirmedBuild->SetBoolField(TEXT("confirmBuild"), true);
	ParamErrors.Reset();
	TestTrue(
		TEXT("JSON Schema const accepts explicit build confirmation"),
		Registry.ValidateParams(
			TEXT("production.build.target"),
			ConfirmedBuild,
			ParamErrors));

	TSharedPtr<FJsonObject> PointerWithoutTarget = MakeShared<FJsonObject>();
	PointerWithoutTarget->SetStringField(TEXT("action"), TEXT("click"));
	ParamErrors.Reset();
	TestFalse(
		TEXT("JSON Schema allOf/anyOf requires a pointer target or position"),
		Registry.ValidateParams(
			TEXT("scene.runtime.input.pointer"),
			PointerWithoutTarget,
			ParamErrors));

	FMCPToolRegistry InvalidIdRegistry;
	InvalidIdRegistry.BeginDomainRegistration(TEXT("blueprint"));
	InvalidIdRegistry.Register(MakeShared<FRegistryTestTool>(TEXT("legacy_snake_case")));
	InvalidIdRegistry.EndDomainRegistration();
	TestEqual(TEXT("Non-dotted implementation IDs are rejected"), InvalidIdRegistry.Num(), 0);

	FMCPToolRegistry DuplicateRegistry;
	DuplicateRegistry.BeginDomainRegistration(TEXT("blueprint"));
	DuplicateRegistry.Register(
		MakeShared<FRegistryTestTool>(TEXT("blueprint.asset.create")));
	DuplicateRegistry.Register(
		MakeShared<FRegistryTestTool>(TEXT("blueprint.asset.create")));
	DuplicateRegistry.EndDomainRegistration();
	TestFalse(
		TEXT("Duplicate implementation IDs degrade the registry"),
		DuplicateRegistry.LoadCapabilityManifests());

	FMCPToolRegistry UndeclaredRegistry;
	UndeclaredRegistry.BeginDomainRegistration(TEXT("blueprint"));
	UndeclaredRegistry.Register(
		MakeShared<FRegistryTestTool>(TEXT("blueprint.undeclared.operation")));
	UndeclaredRegistry.EndDomainRegistration();
	TestFalse(
		TEXT("An implementation without a manifest declaration is rejected"),
		UndeclaredRegistry.LoadCapabilityManifests());

	FMCPToolRegistry CrossDomainRegistry;
	CrossDomainRegistry.BeginDomainRegistration(TEXT("blueprint"));
	CrossDomainRegistry.Register(
		MakeShared<FRegistryTestTool>(TEXT("scene.actor.spawn")));
	CrossDomainRegistry.EndDomainRegistration();
	TestEqual(
		TEXT("Cross-domain registrar bindings are rejected immediately"),
		CrossDomainRegistry.Num(),
		0);

	return true;
}

#endif
