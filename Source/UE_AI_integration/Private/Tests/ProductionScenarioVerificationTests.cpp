#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Infrastructure/PIESessionController.h"
#include "Infrastructure/ProductionRuntimeController.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Tools/MCPToolRegistry.h"

namespace
{
class FScenarioVerificationTool final : public FMCPToolBase
{
public:
	FScenarioVerificationTool(const FString& InCapability, const bool bInFails)
		: Capability(InCapability)
		, bFails(bInFails)
	{
	}

	virtual FString GetCapabilityId() const override
	{
		return Capability;
	}

	virtual FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		++ExecuteCount;
		if (bFails)
		{
			return FMCPToolResult::Error(
				TEXT("Scenario fixture failed."),
				TEXT("fixture_execution_failed"),
				500);
		}
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("fixture"), TEXT("scenario"));
		// Explicit false and null values must survive the generic annotation.
		Data->SetBoolField(TEXT("readbackVerified"), false);
		Data->SetField(TEXT("runtimeVerified"), MakeShared<FJsonValueNull>());
		return FMCPToolResult::Ok(Data);
	}

	int32 GetExecuteCount() const
	{
		return ExecuteCount;
	}

private:
	FString Capability;
	bool bFails = false;
	int32 ExecuteCount = 0;
};

TSharedPtr<FJsonObject> MakeScenario(const FString& Action, const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
	Step->SetStringField(TEXT("id"), TEXT("step"));
	Step->SetStringField(TEXT("action"), Action);
	Step->SetObjectField(TEXT("params"), Params.IsValid() ? Params : MakeShared<FJsonObject>());
	TSharedPtr<FJsonObject> Scenario = MakeShared<FJsonObject>();
	Scenario->SetStringField(TEXT("name"), TEXT("verification"));
	Scenario->SetArrayField(
		TEXT("steps"),
		{MakeShared<FJsonValueObject>(Step)});
	return Scenario;
}

TSharedPtr<FJsonObject> MakeStartParams(const TSharedPtr<FJsonObject>& Scenario)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetObjectField(TEXT("scenario"), Scenario);
	return Params;
}

TSharedPtr<FJsonObject> MakeRunParams(const FString& RunId)
{
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("runId"), RunId);
	return Params;
}

bool FindSingleReceipt(
	const TSharedPtr<FJsonObject>& Result,
	TSharedPtr<FJsonObject>& OutReceipt)
{
	if (!Result.IsValid() || !Result->HasTypedField<EJson::Array>(TEXT("steps")))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Steps = Result->GetArrayField(TEXT("steps"));
	if (Steps.Num() != 1 || !Steps[0].IsValid() || Steps[0]->Type != EJson::Object)
	{
		return false;
	}
	OutReceipt = Steps[0]->AsObject();
	return OutReceipt.IsValid();
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionScenarioVerificationContractTest,
	"UE_AI_integration.Production.ScenarioVerification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionScenarioVerificationContractTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	TestTrue(TEXT("UE_AI_integration plugin is available"), Plugin.IsValid());
	if (!Plugin.IsValid())
	{
		return false;
	}

	FMCPToolRegistry Registry;
	const FString CatalogDirectory =
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Capabilities"));
	AddExpectedError(
		TEXT("Capability catalog is degraded"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	Registry.LoadCapabilityManifestsFromDirectory(CatalogDirectory);
	const FString Capability = TEXT("scene.runtime.object.find");
	Registry.BeginDomainRegistration(TEXT("scene"));
	TSharedPtr<FScenarioVerificationTool> SuccessTool =
		MakeShared<FScenarioVerificationTool>(Capability, false);
	TSharedPtr<FScenarioVerificationTool> FailureTool =
		MakeShared<FScenarioVerificationTool>(TEXT("scene.runtime.object.get"), true);
	Registry.Register(SuccessTool);
	Registry.Register(FailureTool);
	Registry.EndDomainRegistration();

	UEAIIntegration::Infrastructure::FPIESessionController PIEController;
	UEAIIntegration::Infrastructure::FProductionRuntimeController Controller(
		Registry,
		PIEController);

	TSharedPtr<FJsonObject> SuccessParams = MakeShared<FJsonObject>();
	SuccessParams->SetStringField(TEXT("class"), TEXT("AActor"));
	FMCPToolResult Started = Controller.StartScenario(
		MakeStartParams(MakeScenario(TEXT("object.find"), SuccessParams)));
	TestTrue(TEXT("Success scenario starts"), Started.bSuccess);
	const FString SuccessRunId = Started.Data->GetStringField(TEXT("runId"));
	Controller.Tick(0.016f);
	Controller.Tick(0.016f);
	FMCPToolResult SuccessResult = Controller.GetScenarioResult(
		MakeRunParams(SuccessRunId));
	TSharedPtr<FJsonObject> SuccessReceipt;
	TestTrue(TEXT("Success scenario has one receipt"), FindSingleReceipt(SuccessResult.Data, SuccessReceipt));
	if (SuccessReceipt.IsValid())
	{
		TestEqual(TEXT("Success receipt capability"), SuccessReceipt->GetStringField(TEXT("capability")), Capability);
		for (const TCHAR* Field : {
			TEXT("localDeclared"),
			TEXT("handlerRegistered"),
			TEXT("liveAvailable"),
			TEXT("executed"),
			TEXT("readbackVerified"),
			TEXT("runtimeVerified")})
		{
			TestTrue(FString::Printf(TEXT("Success receipt includes %s"), Field), SuccessReceipt->HasField(Field));
		}
		TestTrue(TEXT("Success receipt says handler executed"), SuccessReceipt->GetBoolField(TEXT("executed")));
		TestFalse(TEXT("Explicit false readback is preserved"), SuccessReceipt->GetBoolField(TEXT("readbackVerified")));
		const TSharedPtr<FJsonValue>* RuntimeVerified = SuccessReceipt->Values.Find(TEXT("runtimeVerified"));
		TestTrue(
			TEXT("Explicit null runtime state is preserved"),
			RuntimeVerified && RuntimeVerified->IsValid() && (*RuntimeVerified)->IsNull());
		TestTrue(TEXT("Success receipt has verification state"), SuccessReceipt->HasField(TEXT("verificationState")));
	}
	TestEqual(TEXT("Success handler executes once"), SuccessTool->GetExecuteCount(), 1);

	TSharedPtr<FJsonObject> FailureParams = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ObjectRef = MakeShared<FJsonObject>();
	ObjectRef->SetStringField(TEXT("sessionId"), TEXT("scenario-session"));
	ObjectRef->SetNumberField(TEXT("generation"), 1);
	ObjectRef->SetStringField(TEXT("objectId"), TEXT("scenario-object"));
	FailureParams->SetObjectField(TEXT("objectRef"), ObjectRef);
	FMCPToolResult FailureStarted = Controller.StartScenario(
		MakeStartParams(MakeScenario(TEXT("object.get"), FailureParams)));
	TestTrue(TEXT("Failure scenario starts"), FailureStarted.bSuccess);
	const FString FailureRunId = FailureStarted.Data->GetStringField(TEXT("runId"));
	Controller.Tick(0.016f);
	FMCPToolResult FailureResult = Controller.GetScenarioResult(
		MakeRunParams(FailureRunId));
	TSharedPtr<FJsonObject> FailureReceipt;
	TestTrue(TEXT("Failure scenario has one bounded receipt"), FindSingleReceipt(FailureResult.Data, FailureReceipt));
	if (FailureReceipt.IsValid())
	{
		TestEqual(TEXT("Failure receipt capability"), FailureReceipt->GetStringField(TEXT("capability")), TEXT("scene.runtime.object.get"));
		for (const TCHAR* Field : {
			TEXT("localDeclared"),
			TEXT("handlerRegistered"),
			TEXT("liveAvailable"),
			TEXT("executed"),
			TEXT("readbackVerified"),
			TEXT("runtimeVerified")})
		{
			TestTrue(FString::Printf(TEXT("Failure receipt includes %s"), Field), FailureReceipt->HasField(Field));
		}
		TestFalse(TEXT("Failure receipt is unsuccessful"), FailureReceipt->GetBoolField(TEXT("ok")));
		TestEqual(TEXT("Failure receipt attempt count is bounded"), FailureReceipt->GetIntegerField(TEXT("attempts")), 1);
		TestTrue(TEXT("Failure receipt has verification state"), FailureReceipt->HasField(TEXT("verificationState")));
		TestTrue(TEXT("Failed handler is marked executed"), FailureReceipt->GetBoolField(TEXT("executed")));
	}

	TSharedPtr<FJsonObject> InvalidParams = MakeShared<FJsonObject>();
	FMCPToolResult PreflightStarted = Controller.StartScenario(
		MakeStartParams(MakeScenario(TEXT("object.find"), InvalidParams)));
	TestTrue(TEXT("Preflight scenario starts"), PreflightStarted.bSuccess);
	const FString PreflightRunId = PreflightStarted.Data->GetStringField(TEXT("runId"));
	Controller.Tick(0.016f);
	FMCPToolResult PreflightResult = Controller.GetScenarioResult(
		MakeRunParams(PreflightRunId));
	TSharedPtr<FJsonObject> PreflightReceipt;
	TestTrue(TEXT("Preflight failure has one receipt"), FindSingleReceipt(PreflightResult.Data, PreflightReceipt));
	if (PreflightReceipt.IsValid())
	{
		TestEqual(TEXT("Preflight receipt capability"), PreflightReceipt->GetStringField(TEXT("capability")), Capability);
		TestEqual(TEXT("Preflight failure code"), PreflightReceipt->GetStringField(TEXT("errorCode")), TEXT("invalid_params"));
		TestFalse(TEXT("Preflight does not claim execution"), PreflightReceipt->GetBoolField(TEXT("executed")));
		TestTrue(TEXT("Preflight receipt has verification state"), PreflightReceipt->HasField(TEXT("verificationState")));
	}
	TestEqual(TEXT("Preflight never calls handler"), SuccessTool->GetExecuteCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FProductionLoadedModuleIdentityContractTest,
	"UE_AI_integration.Production.LoadedModuleIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FProductionLoadedModuleIdentityContractTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	TestTrue(TEXT("UE_AI_integration plugin is available"), Plugin.IsValid());
	if (!Plugin.IsValid())
	{
		return false;
	}

	FMCPToolRegistry Registry;
	UEAIIntegration::Infrastructure::FPIESessionController PIEController;
	UEAIIntegration::Infrastructure::FProductionRuntimeController Controller(
		Registry,
		PIEController);
	const FMCPToolResult Result =
		Controller.GetLoadedModule(MakeShared<FJsonObject>());
	TestTrue(TEXT("Loaded-module query succeeds"), Result.bSuccess);
	if (!Result.bSuccess || !Result.Data.IsValid())
	{
		return false;
	}

	TestTrue(TEXT("Loaded-module query reports a loaded module"), Result.Data->GetBoolField(TEXT("loaded")));
	TestTrue(TEXT("Loaded-module identity is complete"), Result.Data->GetBoolField(TEXT("identityComplete")));
	TestTrue(
		TEXT("Loaded-module exact identity is complete"),
		Result.Data->GetBoolField(TEXT("exactIdentityComplete")));
	const TArray<TSharedPtr<FJsonValue>>* IdentityFailureReasons = nullptr;
	if (TestTrue(
			TEXT("Loaded-module identity publishes failure reasons"),
			Result.Data->TryGetArrayField(TEXT("identityFailureReasons"), IdentityFailureReasons))
		&& IdentityFailureReasons)
	{
		TestEqual(
			TEXT("Loaded-module identity has no failure reasons"),
			IdentityFailureReasons->Num(),
			0);
	}
	TestEqual(TEXT("Top-level plugin identity is exact"), Result.Data->GetStringField(TEXT("plugin")), FString(TEXT("UE_AI_integration")));
	TestEqual(TEXT("Top-level module identity is exact"), Result.Data->GetStringField(TEXT("module")), FString(TEXT("UE_AI_integration")));
	TestEqual(
		TEXT("Loaded-module process is the current Editor process"),
		static_cast<uint32>(Result.Data->GetIntegerField(TEXT("processId"))),
		FPlatformProcess::GetCurrentProcessId());
	TestTrue(TEXT("Loaded module path is reported"), !Result.Data->GetStringField(TEXT("modulePath")).IsEmpty());
	TestTrue(TEXT("Loaded module provenance includes a DLL object"), Result.Data->HasTypedField<EJson::Object>(TEXT("dll")));
	TestTrue(TEXT("Loaded module provenance includes a PDB object"), Result.Data->HasTypedField<EJson::Object>(TEXT("pdb")));
	const TSharedPtr<FJsonObject>* Dll = nullptr;
	const TSharedPtr<FJsonObject>* Pdb = nullptr;
	if (!TestTrue(
			TEXT("DLL provenance object is readable"),
			Result.Data->TryGetObjectField(TEXT("dll"), Dll))
		|| !TestTrue(
			TEXT("PDB provenance object is readable"),
			Result.Data->TryGetObjectField(TEXT("pdb"), Pdb))
		|| !Dll || !Dll->IsValid() || !Pdb || !Pdb->IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("DLL provenance path matches the loaded module path"),
		(*Dll)->GetStringField(TEXT("path")),
		Result.Data->GetStringField(TEXT("modulePath")));

	const TSharedPtr<FJsonObject>* Identity = nullptr;
	TestTrue(
		TEXT("Loaded-module query publishes one identity tuple"),
		Result.Data->TryGetObjectField(TEXT("loadedModuleIdentity"), Identity));
	if (!Identity || !Identity->IsValid())
	{
		return false;
	}

	for (const TCHAR* Field : {
		TEXT("schema"), TEXT("plugin"), TEXT("module"), TEXT("processId"),
		TEXT("modulePath"), TEXT("moduleSha256"), TEXT("pdbPath"),
		TEXT("pdbSha256"), TEXT("latestBuildArtifactPath"),
		TEXT("latestBuildArtifactSha256"), TEXT("editorStartedAtUtc")})
	{
		TestTrue(
			FString::Printf(TEXT("Identity tuple includes %s"), Field),
			(*Identity)->HasField(Field));
	}
	TestEqual(TEXT("Identity schema is versioned"), (*Identity)->GetStringField(TEXT("schema")), FString(TEXT("ue.loaded-module-identity.v1")));
	TestEqual(TEXT("Identity plugin is UE_AI_integration"), (*Identity)->GetStringField(TEXT("plugin")), FString(TEXT("UE_AI_integration")));
	TestEqual(TEXT("Identity module is UE_AI_integration"), (*Identity)->GetStringField(TEXT("module")), FString(TEXT("UE_AI_integration")));
	TestTrue(TEXT("Identity tuple is complete"), (*Identity)->GetBoolField(TEXT("identityComplete")));
	TestTrue(TEXT("Identity tuple proves the exact module/artifact tuple"), (*Identity)->GetBoolField(TEXT("exactIdentityComplete")));
	TestTrue(TEXT("Identity tuple matches its module filename"), (*Identity)->GetBoolField(TEXT("moduleFilenameMatches")));
	TestTrue(TEXT("Identity tuple keeps the module inside the plugin"), (*Identity)->GetBoolField(TEXT("modulePathWithinPlugin")));
	TestTrue(TEXT("Identity tuple contains a complete module hash"), (*Identity)->GetBoolField(TEXT("moduleSha256Complete")));
	TestTrue(TEXT("Identity tuple contains a complete PDB hash"), (*Identity)->GetBoolField(TEXT("pdbSha256Complete")));
	TestTrue(TEXT("Identity tuple matches the latest artifact"), (*Identity)->GetBoolField(TEXT("latestArtifactIdentityComplete")));
	TestEqual(
		TEXT("Identity process matches top-level process"),
		static_cast<uint32>((*Identity)->GetIntegerField(TEXT("processId"))),
		static_cast<uint32>(Result.Data->GetIntegerField(TEXT("processId"))));
	TestEqual(TEXT("Identity module path matches top-level path"), (*Identity)->GetStringField(TEXT("modulePath")), Result.Data->GetStringField(TEXT("modulePath")));
	TestEqual(TEXT("Identity DLL hash matches the DLL provenance hash"), (*Identity)->GetStringField(TEXT("moduleSha256")), (*Dll)->GetStringField(TEXT("sha256")));
	TestEqual(TEXT("Identity PDB path matches the PDB provenance path"), (*Identity)->GetStringField(TEXT("pdbPath")), (*Pdb)->GetStringField(TEXT("path")));
	TestEqual(TEXT("Identity PDB hash matches the PDB provenance hash"), (*Identity)->GetStringField(TEXT("pdbSha256")), (*Pdb)->GetStringField(TEXT("sha256")));
	TestEqual(TEXT("Identity latest artifact path matches top-level path"), (*Identity)->GetStringField(TEXT("latestBuildArtifactPath")), Result.Data->GetStringField(TEXT("latestBuildArtifactPath")));
	TestEqual(TEXT("Identity latest artifact hash matches top-level hash"), (*Identity)->GetStringField(TEXT("latestBuildArtifactSha256")), Result.Data->GetStringField(TEXT("latestBuildArtifactSha256")));
	TestTrue(TEXT("Identity module hash is empty or a SHA-256 digest"), (*Identity)->GetStringField(TEXT("moduleSha256")).IsEmpty() || (*Identity)->GetStringField(TEXT("moduleSha256")).Len() == 64);
	TestTrue(TEXT("Identity Editor start time is reported"), !(*Identity)->GetStringField(TEXT("editorStartedAtUtc")).IsEmpty());
	return true;
}

#endif
