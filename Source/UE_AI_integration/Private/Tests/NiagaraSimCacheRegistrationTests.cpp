#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

namespace UEAIIntegrationTools { void RegisterNiagaraSimCacheTools(FMCPToolRegistry&); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNiagaraSimCacheRegistrationTest,
	"UE_AI_integration.Niagara.SimCache.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSimCacheRegistrationTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraSimCacheTools(Registry);
	Registry.EndDomainRegistration();
	for (const TCHAR* Id : {TEXT("content.niagara.simcache.capture"),
		TEXT("content.niagara.simcache.inspect"), TEXT("content.niagara.simcache.read"),
		TEXT("content.niagara.simcache.export"), TEXT("content.niagara.simcache.release")})
	{
		FMCPToolBase* Tool = Registry.FindTool(Id);
		if (!TestNotNull(Id, Tool)) return false;
#if !WITH_UEAI_NIAGARA
		const FMCPToolResult Result = Tool->Execute(MakeShared<FJsonObject>());
		TestFalse(TEXT("An omitted feature cannot execute"), Result.bSuccess);
		TestEqual(TEXT("Omitted Niagara reports feature unavailability"),
			Result.ErrorCode, FString(TEXT("niagara_unavailable")));
#endif
	}
	return true;
}
#endif
