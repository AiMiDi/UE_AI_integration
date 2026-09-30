#if WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"
#include "Editor.h"
#include "Engine/World.h"

namespace UEAIIntegrationTools
{
void RegisterNiagaraRuntimeTools(FMCPToolRegistry&);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraRuntimePreflightTest,
	"UE_AI_integration.Niagara.Runtime.Preflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNiagaraRuntimePreflightTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRuntimeTools(Registry);
	Registry.EndDomainRegistration();
	auto* InspectTool = Registry.FindTool(TEXT("content.niagara.runtime.inspect"));
	if (!TestNotNull(TEXT("Inspection registered"), InspectTool)) return false;
	TestNull(TEXT("GPU capture is not exposed"), Registry.FindTool(TEXT("content.niagara.runtime.capture")));
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("world"), TEXT("/MissingInventoryWorld.NoWorld"));
	const auto Inspect = InspectTool->Execute(Params);
	TestEqual(TEXT("Inspection does not silently change world"), Inspect.ErrorCode, FString(TEXT("world_not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraRuntimeInventoryTest,
	"UE_AI_integration.Niagara.Runtime.Inventory", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FNiagaraRuntimeInventoryTest::RunTest(const FString&)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world required"), World)) return false;
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraRuntimeTools(Registry);
	Registry.EndDomainRegistration();
	auto* Tool = Registry.FindTool(TEXT("content.niagara.runtime.inspect"));
	if (!TestNotNull(TEXT("Inventory tool registered"), Tool)) return false;
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("world"), World->GetPathName());
	Params->SetNumberField(TEXT("limit"), 1);
	const auto Result = Tool->Execute(Params);
	if (!TestTrue(TEXT("Inventory succeeds without GPU capture"), Result.bSuccess && Result.Data.IsValid())) return false;
	TestTrue(TEXT("Component page bounded"), Result.Data->GetArrayField(TEXT("components")).Num() <= 1);
	TestTrue(TEXT("DI page bounded"), Result.Data->GetArrayField(TEXT("dataInterfaces")).Num() <= 1);
	for (const auto& Item : Result.Data->GetArrayField(TEXT("components")))
	{
		TestEqual(TEXT("Component belongs to requested world"),
			Item->AsObject()->GetStringField(TEXT("world")), World->GetPathName());
	}
	for (const auto& Item : Result.Data->GetArrayField(TEXT("dataInterfaces")))
	{
		TestEqual(TEXT("Loaded DI does not claim runtime binding"),
			Item->AsObject()->GetStringField(TEXT("bindingEvidence")), FString(TEXT("loadedObjectOnly")));
	}
	Params->SetNumberField(TEXT("offset"), 100000);
	const auto End = Tool->Execute(Params);
	if (!TestTrue(TEXT("End page succeeds"), End.bSuccess && End.Data.IsValid())) return false;
	TestEqual(TEXT("No components beyond inventory"), End.Data->GetArrayField(TEXT("components")).Num(), 0);
	TestEqual(TEXT("No DIs beyond inventory"), End.Data->GetArrayField(TEXT("dataInterfaces")).Num(), 0);
	TestFalse(TEXT("End page has no continuation"), End.Data->GetBoolField(TEXT("hasMore")));
	return true;
}
#endif
