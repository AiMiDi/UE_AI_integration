#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

namespace UEAIIntegrationTools
{
void RegisterSceneEngineeringCommandTools(FMCPToolRegistry& Registry);
}
namespace UEAISceneEngineeringCommandPrivate
{
FMCPToolResult RestoreRenderSettings(const TMap<FString, FString>& Before, const TMap<FString, FString>& Allowed);
}

namespace
{
struct FTemporaryRecoveryCVars
{
	FString FirstName = TEXT("ueai.Recovery.First.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FString SecondName = TEXT("ueai.Recovery.Second.") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IConsoleVariable* First = IConsoleManager::Get().RegisterConsoleVariable(*FirstName, 1, TEXT("Recovery fixture"), ECVF_Default);
	IConsoleVariable* Second = IConsoleManager::Get().RegisterConsoleVariable(*SecondName, 1, TEXT("Recovery fixture"), ECVF_Default);
	~FTemporaryRecoveryCVars()
	{
		IConsoleManager::Get().UnregisterConsoleObject(*FirstName, false);
		if (Second) IConsoleManager::Get().UnregisterConsoleObject(*SecondName, false);
	}
};

struct FRenderSettingGuard
{
	IConsoleVariable* CVar;
	FString Original;
	EConsoleVariableFlags Flags;
	FDelegateHandle Callback;
	explicit FRenderSettingGuard(IConsoleVariable* InCVar)
		: CVar(InCVar), Original(InCVar->GetString()), Flags(InCVar->GetFlags()) {}
	void RemoveCallback()
	{
		if (Callback.IsValid()) CVar->OnChangedDelegate().Remove(Callback);
		Callback.Reset();
	}
	~FRenderSettingGuard()
	{
		RemoveCallback();
		CVar->Set(*Original, ECVF_SetByConsole);
		CVar->SetFlags(Flags);
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderRecoveryReadbackTest,
	"UE_AI_integration.Rendering.Settings.RecoveryReadback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderRecoveryReadbackTest::RunTest(const FString& Parameters)
{
	using UEAISceneEngineeringCommandPrivate::RestoreRenderSettings;
	FTemporaryRecoveryCVars Fixture;
	const TMap<FString, FString> Allowed = {{TEXT("first"), Fixture.FirstName}, {TEXT("second"), Fixture.SecondName}};
	const TMap<FString, FString> Before = {{TEXT("first"), TEXT("0")}, {TEXT("second"), TEXT("0")}};
	const FMCPToolResult Good = RestoreRenderSettings(Before, Allowed);
	TestTrue(TEXT("Restoration requires actual readback success"), Good.bSuccess && Good.Data->GetBoolField(TEXT("rollbackVerified")));
	TestEqual(TEXT("The receipt contains the observed value"), Good.Data->GetObjectField(TEXT("readBack"))->GetStringField(TEXT("first")), FString(TEXT("0")));
	Fixture.Second->SetOnChangedCallback(FConsoleVariableDelegate::CreateLambda([&Fixture](IConsoleVariable*)
	{
		Fixture.First->Set(9, ECVF_SetByConsole);
	}));
	const FMCPToolResult Rejected = RestoreRenderSettings(Before, Allowed);
	TestFalse(TEXT("A later callback invalidating an earlier restoration cannot succeed"), Rejected.bSuccess);
	TestEqual(TEXT("Failed restoration has a stable error"), Rejected.ErrorCode, FString(TEXT("setting_rollback_failed")));
	TestFalse(TEXT("Failed restoration is never verified"), Rejected.Data->GetBoolField(TEXT("rollbackVerified")));
	TestTrue(TEXT("Failed restoration asks for review"), Rejected.Data->GetBoolField(TEXT("manualReview")));
	TestEqual(TEXT("Failure receipt retains the actual post-callback value"), Rejected.Data->GetObjectField(TEXT("readBack"))->GetStringField(TEXT("first")), FString(TEXT("9")));
	Fixture.Second->SetOnChangedCallback(FConsoleVariableDelegate());
	TestTrue(TEXT("Recovery can be retried after the interfering callback is removed"), RestoreRenderSettings(Before, Allowed).bSuccess);
	IConsoleManager::Get().UnregisterConsoleObject(*Fixture.SecondName, false);
	Fixture.Second = nullptr;
	const FMCPToolResult Missing = RestoreRenderSettings(Before, Allowed);
	TestFalse(TEXT("Missing CVars cannot silently count as restored"), Missing.bSuccess);
	TestEqual(TEXT("The missing setting is identified"), Missing.Data->GetArrayField(TEXT("failures"))[0]->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("setting_unavailable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRenderRecoveryHandlerTest,
	"UE_AI_integration.Rendering.Settings.HandlerFailureAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRenderRecoveryHandlerTest::RunTest(const FString& Parameters)
{
	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TemporalAA.Upsampling"));
	if (!TestNotNull(TEXT("The admitted session setting exists"), CVar)) return false;
	FRenderSettingGuard Guard(CVar);
	const FString Desired = CVar->GetBool() ? TEXT("0") : TEXT("1");
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("scene"));
	UEAIIntegrationTools::RegisterSceneEngineeringCommandTools(Registry);
	Registry.EndDomainRegistration();
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetBoolField(TEXT("tsrUpsampling"), Desired == TEXT("1"));
	Params->SetObjectField(TEXT("settings"), Settings);
	Params->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString());
	Params->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult Plan = Registry.ExecuteTool(TEXT("scene.render.settings.plan"), Params);
	if (!TestTrue(TEXT("Session settings can be planned"), Plan.bSuccess && Plan.Data.IsValid())) return false;
	Params->SetStringField(TEXT("approvePlanDigest"), Plan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult Applied = Registry.ExecuteTool(TEXT("scene.render.settings.execute"), Params);
	if (!TestTrue(TEXT("Session change succeeds"), Applied.bSuccess && Applied.Data.IsValid())) return false;
	TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("confirmWrite"), true);
	Rollback->SetStringField(TEXT("runId"), Applied.Data->GetStringField(TEXT("runId")));
	bool bChanging = false;
	Guard.Callback = CVar->OnChangedDelegate().AddLambda([&](IConsoleVariable* Changed)
	{
		if (!bChanging && Changed->GetString() == Guard.Original)
		{
			bChanging = true;
			Changed->Set(*Desired, ECVF_SetByConsole);
			bChanging = false;
		}
	});
	const FMCPToolResult FailedRollback = Registry.ExecuteTool(TEXT("scene.render.settings.rollback"), Rollback);
	TestFalse(TEXT("The public rollback handler reports interference as failure"), FailedRollback.bSuccess);
	TestFalse(TEXT("Failure does not mark the record rolled back"), FailedRollback.Data->GetBoolField(TEXT("rolledBack")));
	Guard.RemoveCallback();
	const FMCPToolResult Retried = Registry.ExecuteTool(TEXT("scene.render.settings.rollback"), Rollback);
	TestTrue(TEXT("The same run can be restored after a failed rollback"), Retried.bSuccess && Retried.Data->GetBoolField(TEXT("rollbackVerified")));
	TestEqual(TEXT("Successful retry restores the real CVar"), CVar->GetString(), Guard.Original);
	const FMCPToolResult Replayed = Registry.ExecuteTool(TEXT("scene.render.settings.rollback"), Rollback);
	TestTrue(TEXT("Successful rollback replay identifies its prior verified receipt"), Replayed.bSuccess && Replayed.Data->GetBoolField(TEXT("idempotentReplay")));

	// Fail both the requested change and its compensation, then recover the
	// failed run through the same public rollback endpoint.
	Guard.Callback = CVar->OnChangedDelegate().AddLambda([&](IConsoleVariable* Changed)
	{
		if (!bChanging)
		{
			bChanging = true;
			Changed->Set(2, ECVF_SetByConsole);
			bChanging = false;
		}
	});
	Params->SetStringField(TEXT("requestId"), FGuid::NewGuid().ToString());
	const FMCPToolResult SecondPlan = Registry.ExecuteTool(TEXT("scene.render.settings.plan"), Params);
	if (!TestTrue(TEXT("The failure case has an exact baseline plan"), SecondPlan.bSuccess && SecondPlan.Data.IsValid())) return false;
	Params->SetStringField(TEXT("approvePlanDigest"), SecondPlan.Data->GetStringField(TEXT("planDigest")));
	const FMCPToolResult FailedChange = Registry.ExecuteTool(TEXT("scene.render.settings.execute"), Params);
	if (!TestTrue(TEXT("Failed compensation returns an auditable failed run"), !FailedChange.bSuccess && FailedChange.Data.IsValid())) return false;
	TestEqual(TEXT("Execution distinguishes compensation failure"), FailedChange.ErrorCode, FString(TEXT("setting_rollback_failed")));
	TestFalse(TEXT("Execution cannot claim failed compensation was verified"), FailedChange.Data->GetBoolField(TEXT("rollbackVerified")));
	Guard.RemoveCallback();
	const FMCPToolResult FailedReplay = Registry.ExecuteTool(TEXT("scene.render.settings.execute"), Params);
	TestFalse(TEXT("Replaying a failed request cannot fabricate execution success"), FailedReplay.bSuccess);
	TestTrue(TEXT("Failed execution replay is explicit"), FailedReplay.Data->GetBoolField(TEXT("idempotentReplay")));
	Rollback->SetStringField(TEXT("runId"), FailedChange.Data->GetStringField(TEXT("runId")));
	const FMCPToolResult Recovered = Registry.ExecuteTool(TEXT("scene.render.settings.rollback"), Rollback);
	TestTrue(TEXT("The failed execution retains a usable recovery record"), Recovered.bSuccess && Recovered.Data->GetBoolField(TEXT("rollbackVerified")));
	TestEqual(TEXT("Recovery restores the original session value"), CVar->GetString(), Guard.Original);
	return true;
}

#endif
