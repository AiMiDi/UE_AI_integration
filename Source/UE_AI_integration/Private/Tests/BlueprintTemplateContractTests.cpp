#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Components/SphereComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "Tools/MCPToolRegistry.h"
#include "UEAIIntegrationServer.h"
#include "UEAIIntegrationSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Workflow/UEWorkflowRuntime.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintBuildGraphTools(FMCPToolRegistry& Registry);
}

namespace
{
TSharedRef<FJsonObject> MakeTemplateParams(const TCHAR* Name)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("templateName"), Name);
	Params->SetStringField(TEXT("blueprint"), TEXT("/Game/Automation/UEAI_MissingTemplateTarget"));
	return Params;
}

FMCPResult ExecuteTemplateWorkflow(
	UEAIIntegration::Workflow::FWorkflowRuntime& Runtime,
	const TSharedPtr<FJsonObject>& Workflow,
	const FString& Digest)
{
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("action"), TEXT("execute"));
	Request->SetObjectField(TEXT("workflow"), Workflow);
	Request->SetStringField(TEXT("approvePlanDigest"), Digest);
	Request->SetBoolField(TEXT("confirmWrite"), true);
	Request->SetBoolField(TEXT("saveOnSuccess"), true);
	Request->SetStringField(TEXT("detailLevel"), TEXT("summary"));
	return Runtime.HandleRequest(Request);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintTemplateDiscoveryContractTest,
	"UE_AI_integration.Blueprint.Template.DiscoveryAndParameterContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintTemplateDiscoveryContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintBuildGraphTools(Registry);
	Registry.EndDomainRegistration();
	const FMCPToolResult Catalog = Registry.ExecuteTool(
		TEXT("blueprint.template.list"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Named template discovery succeeds"), Catalog.bSuccess)
		|| !TestNotNull(TEXT("Template catalog exists"), Catalog.Data.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Template catalog schema is explicit"),
		Catalog.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.blueprint-template-catalog.v1")));
	TestEqual(TEXT("All three named behavior templates are discoverable"),
		Catalog.Data->GetIntegerField(TEXT("count")), 3);
	TestFalse(TEXT("Discovery does not claim runtime verification"),
		Catalog.Data->GetBoolField(TEXT("runtimeVerified")));
	TSet<FString> Names;
	for (const TSharedPtr<FJsonValue>& Value : Catalog.Data->GetArrayField(TEXT("templates")))
	{
		const TSharedPtr<FJsonObject> Template = Value->AsObject();
		Names.Add(Template->GetStringField(TEXT("name")));
		TestEqual(TEXT("Each template declares its bounded behavior scope"),
			Template->GetStringField(TEXT("scope")), FString(TEXT("minimal_runtime_behavior")));
		TestEqual(TEXT("Executable template contract has a new version"), Template->GetStringField(TEXT("version")), FString(TEXT("2.0")));
		TestTrue(TEXT("Each template declares its limits"),
			!Template->GetArrayField(TEXT("limitations")).IsEmpty());
		TestFalse(TEXT("Template parameter schemas reject unknown parameters"),
			Template->GetObjectField(TEXT("parameterSchema"))->GetBoolField(TEXT("additionalProperties")));
	}
	for (const TCHAR* Name : {TEXT("health_system"), TEXT("timer_loop"), TEXT("interactable_actor")})
	{
		TestTrue(FString::Printf(TEXT("Template '%s' is published"), Name), Names.Contains(Name));
	}

	auto ExpectRejected = [this, &Registry](const TSharedRef<FJsonObject>& Params, const TCHAR* Code)
	{
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("blueprint.template.apply"), Params);
		TestFalse(TEXT("Invalid template requests never plan or write"), Result.bSuccess);
		TestEqual(TEXT("Invalid template requests have a stable error"), Result.ErrorCode, FString(Code));
	};
	ExpectRejected(MakeShared<FJsonObject>(), TEXT("invalid_params"));
	ExpectRejected(MakeTemplateParams(TEXT("unknown")), TEXT("template_not_found"));
	TSharedRef<FJsonObject> ShortName = MakeTemplateParams(TEXT("timer_loop"));
	ShortName->SetStringField(TEXT("blueprint"), TEXT("BP_Ambiguous"));
	ExpectRejected(ShortName, TEXT("invalid_params"));
	TSharedRef<FJsonObject> UnexpectedWrite = MakeTemplateParams(TEXT("timer_loop"));
	UnexpectedWrite->SetBoolField(TEXT("confirmWrite"), true);
	ExpectRejected(UnexpectedWrite, TEXT("invalid_params"));
	TSharedRef<FJsonObject> EncodedParameters = MakeTemplateParams(TEXT("timer_loop"));
	EncodedParameters->SetStringField(TEXT("parameters"), TEXT("{\"delay\":1}"));
	ExpectRejected(EncodedParameters, TEXT("invalid_params"));
	TSharedRef<FJsonObject> InvalidEvent = MakeTemplateParams(TEXT("timer_loop"));
	TSharedRef<FJsonObject> EventParameters = MakeShared<FJsonObject>();
	EventParameters->SetStringField(TEXT("eventName"), TEXT("event\"}], injected"));
	InvalidEvent->SetObjectField(TEXT("parameters"), EventParameters);
	ExpectRejected(InvalidEvent, TEXT("invalid_params"));
	for (const TCHAR* Reserved : {TEXT("StartTimer"), TEXT("StopTimer"), TEXT("ReceiveEndPlay"),
		TEXT("LoopCount"), TEXT("bTimerRunning"), TEXT("bTimerEnded"), TEXT("None")})
	{
		auto InvalidLifecycle = MakeTemplateParams(TEXT("timer_loop"));
		auto LifecycleValues = MakeShared<FJsonObject>();
		LifecycleValues->SetStringField(TEXT("eventName"), Reserved);
		InvalidLifecycle->SetObjectField(TEXT("parameters"), LifecycleValues);
		ExpectRejected(InvalidLifecycle, TEXT("invalid_params"));
	}
	for (const double InvalidDelay : {0.0, 0.0001, -1.0})
	{
		auto Delay = MakeTemplateParams(TEXT("timer_loop"));
		auto DelayValues = MakeShared<FJsonObject>();
		DelayValues->SetNumberField(TEXT("delay"), InvalidDelay);
		Delay->SetObjectField(TEXT("parameters"), DelayValues);
		ExpectRejected(Delay, TEXT("invalid_params"));
	}
	{
		auto InvalidGuard = MakeTemplateParams(TEXT("interactable_actor"));
		auto GuardValues = MakeShared<FJsonObject>();
		GuardValues->SetStringField(TEXT("initiallyInteractable"), TEXT("false"));
		InvalidGuard->SetObjectField(TEXT("parameters"), GuardValues);
		ExpectRejected(InvalidGuard, TEXT("invalid_params"));
	}
	TSharedRef<FJsonObject> UnsupportedRadius = MakeTemplateParams(TEXT("interactable_actor"));
	TSharedRef<FJsonObject> RadiusParameters = MakeShared<FJsonObject>();
	RadiusParameters->SetNumberField(TEXT("radius"), 200.0);
	UnsupportedRadius->SetObjectField(TEXT("parameters"), RadiusParameters);
	ExpectRejected(UnsupportedRadius, TEXT("invalid_params"));
	for (const double Value : {-1.0, 1.0e9 + 1.0})
	{
		TSharedRef<FJsonObject> OutOfRange = MakeTemplateParams(TEXT("health_system"));
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		Values->SetNumberField(TEXT("maxHealth"), Value);
		OutOfRange->SetObjectField(TEXT("parameters"), Values);
		ExpectRejected(OutOfRange, TEXT("invalid_params"));
	}
	TSharedRef<FJsonObject> StringNumber = MakeTemplateParams(TEXT("health_system"));
	TSharedRef<FJsonObject> StringValues = MakeShared<FJsonObject>();
	StringValues->SetStringField(TEXT("maxHealth"), TEXT("100"));
	StringNumber->SetObjectField(TEXT("parameters"), StringValues);
	ExpectRejected(StringNumber, TEXT("invalid_params"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintTemplateWorkflowContractTest,
	"UE_AI_integration.Blueprint.Template.ApprovedWorkflowSaveReloadAndRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintTemplateWorkflowContractTest::RunTest(const FString&)
{
	UUEAIIntegrationSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>() : nullptr;
	FMCPToolRegistry* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	FUEAIIntegrationServer* Server = Subsystem ? Subsystem->GetServer() : nullptr;
	auto* Runtime = Server ? Server->GetWorkflowRuntimeForTesting() : nullptr;
	if (!Registry || !Runtime)
	{
		AddError(TEXT("Template acceptance requires the integration registry and Workflow runtime."));
		return false;
	}

	for (const TCHAR* Name : {TEXT("health_system"), TEXT("timer_loop"), TEXT("interactable_actor")})
	{
		const FString AssetName = FString::Printf(TEXT("BP_Template_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
		const FString BlueprintPath = TEXT("/Game/Automation/") + AssetName;
		TSharedRef<FJsonObject> Create = MakeShared<FJsonObject>();
		Create->SetStringField(TEXT("blueprintName"), AssetName);
		Create->SetStringField(TEXT("packagePath"), TEXT("/Game/Automation"));
		Create->SetStringField(TEXT("parentClass"), TEXT("Actor"));
		const FMCPToolResult Created = Registry->ExecuteTool(TEXT("blueprint.asset.create"), Create);
		if (!TestTrue(TEXT("Template fixture creates"), Created.bSuccess))
		{
			AddError(Created.ErrorMessage);
			return false;
		}
		ON_SCOPE_EXIT
		{
			TestTrue(TEXT("Template fixture is removed"), UEditorAssetLibrary::DeleteAsset(BlueprintPath));
		};
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!TestNotNull(TEXT("Template fixture exists"), Blueprint)
			|| !TestTrue(TEXT("Template baseline is saved"), UEditorAssetLibrary::SaveAsset(BlueprintPath, false)))
		{
			return false;
		}
		const FString BeforeHash = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint);
		const int32 BeforeVariableCount = Blueprint->NewVariables.Num();
		const bool bBeforeDirty = Blueprint->GetOutermost()->IsDirty();
		TSharedRef<FJsonObject> Params = MakeTemplateParams(Name);
		Params->SetStringField(TEXT("blueprint"), BlueprintPath);
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		if (FCString::Strcmp(Name, TEXT("health_system")) == 0)
		{
			Values->SetNumberField(TEXT("maxHealth"), 125.5);
			Values->SetNumberField(TEXT("damageAmount"), 25.0);
			Values->SetNumberField(TEXT("healAmount"), 15.0);
		}
		else if (FCString::Strcmp(Name, TEXT("timer_loop")) == 0)
		{
			Values->SetStringField(TEXT("eventName"), TEXT("FixtureLoop"));
			Values->SetNumberField(TEXT("delay"), 0.125);
		}
		else
		{
			Values->SetBoolField(TEXT("initiallyInteractable"), false);
		}
		Params->SetObjectField(TEXT("parameters"), Values);
		const FMCPToolResult Planned = Registry->ExecuteTool(TEXT("blueprint.template.apply"), Params);
		if (!TestTrue(FString::Printf(TEXT("Template '%s' plans (%s)"), Name, *Planned.ErrorMessage), Planned.bSuccess)
			|| !TestNotNull(TEXT("Template application plan exists"), Planned.Data.Get()))
		{
			return false;
		}
		TestFalse(TEXT("Planning explicitly reports no application"), Planned.Data->GetBoolField(TEXT("applied")));
		TestTrue(TEXT("Planning requires approved execution"), Planned.Data->GetBoolField(TEXT("executionRequired")));
		TestFalse(TEXT("Planning does not claim runtime acceptance"), Planned.Data->GetBoolField(TEXT("runtimeVerified")));
		TestEqual(TEXT("Planning preserves asset structure"),
			UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint), BeforeHash);
		TestEqual(TEXT("Planning preserves dirty state"), Blueprint->GetOutermost()->IsDirty(), bBeforeDirty);
		const TSharedPtr<FJsonObject> Definition = Planned.Data->GetObjectField(TEXT("normalizedDefinition"));
		TestEqual(TEXT("Template normalizes through the BuildGraph contract"),
			Definition->GetStringField(TEXT("schema")), FString(TEXT("ue.blueprint-buildgraph.v1")));
		TestTrue(TEXT("Behavior template authors its declared execution and value links"), !Definition->GetArrayField(TEXT("connections")).IsEmpty());
		const TSharedPtr<FJsonObject> Workflow = Planned.Data->GetObjectField(TEXT("workflow"));
		const FString Digest = Planned.Data->GetStringField(TEXT("planDigest"));
		TestFalse(TEXT("Workflow plan digest is present"), Digest.IsEmpty());
		TSharedRef<FJsonObject> Unapproved = MakeShared<FJsonObject>();
		Unapproved->SetStringField(TEXT("action"), TEXT("execute"));
		Unapproved->SetObjectField(TEXT("workflow"), Workflow);
		Unapproved->SetBoolField(TEXT("confirmWrite"), true);
		TestFalse(TEXT("Workflow refuses execution without digest approval"), Runtime->HandleRequest(Unapproved).bOk);
		TestEqual(TEXT("Denied execution preserves the baseline"),
			UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint), BeforeHash);

		const FMCPResult Executed = ExecuteTemplateWorkflow(*Runtime, Workflow, Digest);
		if (!TestTrue(FString::Printf(TEXT("Approved template executes (%s: %s)"), *Executed.Error.Code, *Executed.Error.Message), Executed.bOk)
			|| !TestNotNull(TEXT("Template execution receipt exists"), Executed.Data.Get()))
		{
			return false;
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!TestNotNull(TEXT("Authored template Blueprint exists"), Blueprint))
		{
			return false;
		}
		TestEqual(TEXT("Workflow compiles the behavior graph"), Blueprint->Status, BS_UpToDate);
		TestFalse(TEXT("Workflow saves the resulting package"), Blueprint->GetOutermost()->IsDirty());
		if (FCString::Strcmp(Name, TEXT("health_system")) == 0)
		{
			for (const TCHAR* VariableName : {TEXT("Health"), TEXT("MaxHealth")})
			{
				const FBPVariableDescription* Variable = Blueprint->NewVariables.FindByPredicate(
					[VariableName](const FBPVariableDescription& Candidate)
					{
						return Candidate.VarName == FName(VariableName);
					});
				if (TestNotNull(TEXT("Authored health variable exists"), Variable))
				{
					TestEqual(TEXT("Typed maxHealth parameter reaches the authored default"),
						Variable->DefaultValue, FString(TEXT("125.5")));
				}
			}
		}
		TSharedRef<FJsonObject> ReadDefinition = MakeShared<FJsonObject>();
		ReadDefinition->SetStringField(TEXT("blueprint"), BlueprintPath);
		ReadDefinition->SetStringField(TEXT("graph"), Definition->GetStringField(TEXT("graph")));
		ReadDefinition->SetStringField(TEXT("buildId"), Definition->GetStringField(TEXT("buildId")));
		const FMCPToolResult ReadBack = Registry->ExecuteTool(TEXT("blueprint.graph.build.definition.get"), ReadDefinition);
		if (!TestTrue(TEXT("Template managed ownership reads back"), ReadBack.bSuccess)
			|| !TestNotNull(TEXT("Template ownership read-back exists"), ReadBack.Data.Get()))
		{
			return false;
		}
		const FString AuthoredHash = UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint);
		const int32 ManagedCount = ReadBack.Data->GetObjectField(TEXT("definition"))->GetObjectField(TEXT("managedRefs"))->Values.Num();
		TestEqual(TEXT("Every behavior node has a stable managed ref"), ManagedCount, Definition->GetArrayField(TEXT("nodes")).Num());
		TArray<UPackage*> ReloadPackages{Blueprint->GetOutermost()};
		FText ReloadError;
		if (!TestTrue(TEXT("Template package reloads from disk"),
			UPackageTools::ReloadPackages(ReloadPackages, ReloadError, EReloadPackagesInteractionMode::AssumePositive)))
		{
			AddError(ReloadError.ToString());
			return false;
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!TestNotNull(TEXT("Reloaded template Blueprint exists"), Blueprint))
		{
			return false;
		}
		TestEqual(TEXT("Template structure survives disk reload"),
			UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint), AuthoredHash);
		const FMCPToolResult Reloaded = Registry->ExecuteTool(TEXT("blueprint.graph.build.definition.get"), ReadDefinition);
		TestTrue(TEXT("Template ownership survives disk reload"), Reloaded.bSuccess);
		if (Reloaded.bSuccess && Reloaded.Data.IsValid())
		{
			TestEqual(TEXT("Reloaded template keeps every managed ref"),
				Reloaded.Data->GetObjectField(TEXT("definition"))->GetObjectField(TEXT("managedRefs"))->Values.Num(), ManagedCount);
		}
		// Run the approved, saved and reloaded generated class. Every observable
		// behavior below comes from ordinary Workflow-authored Blueprint nodes.
		{
			UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
			if (!TestNotNull(TEXT("Template runtime world exists"), World)
				|| !TestNotNull(TEXT("Template runtime engine exists"), GEngine))
				return false;
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
			FURL URL;
			World->InitializeActorsForPlay(URL);
			World->BeginPlay();
			AActor* Actor = World->SpawnActor<AActor>(Blueprint->GeneratedClass, FTransform::Identity);
			if (!TestNotNull(TEXT("Saved and reloaded template spawns an actor"), Actor))
				return false;
			auto RuntimeParams = MakeShared<FJsonObject>();
			RuntimeParams->SetStringField(TEXT("blueprint"), BlueprintPath);
			const FMCPToolResult Observed = Registry->ExecuteTool(TEXT("blueprint.asset.runtime.verify"), RuntimeParams);
			TestTrue(TEXT("Runtime acceptance identifies the actual template actor"),
				Observed.bSuccess && Observed.Data && Observed.Data->GetBoolField(TEXT("runtimeVerified"))
				&& Observed.Data->GetStringField(TEXT("instance")) == Actor->GetPathName()
				&& Observed.Data->GetStringField(TEXT("instanceClass")) == Actor->GetClass()->GetPathName());
			auto Invoke = [this, Actor](const TCHAR* EventName)
			{
				UFunction* Event = Actor->FindFunction(FName(EventName));
				if (!TestNotNull(FString::Printf(TEXT("Runtime event %s exists"), EventName), Event)) return false;
				if (!TestEqual(TEXT("Template event has the declared no-argument signature"), Event->ParmsSize, static_cast<uint16>(0))) return false;
				Actor->ProcessEvent(Event, nullptr);
				return true;
			};
			auto ReadNumber = [this, Actor](const TCHAR* Name) -> double
			{
				auto* Property = FindFProperty<FNumericProperty>(Actor->GetClass(), Name);
				if (!TestNotNull(FString::Printf(TEXT("Runtime numeric member %s exists"), Name), Property)) return 0.0;
				const void* Address = Property->ContainerPtrToValuePtr<void>(Actor);
				return Property->IsInteger() ? static_cast<double>(Property->GetSignedIntPropertyValue(Address))
					: Property->GetFloatingPointPropertyValue(Address);
			};
			auto SetNumber = [this, Actor](const TCHAR* Name, double Value)
			{
				auto* Property = FindFProperty<FNumericProperty>(Actor->GetClass(), Name);
				if (!TestNotNull(FString::Printf(TEXT("Runtime writable member %s exists"), Name), Property)) return false;
				Property->SetFloatingPointPropertyValue(Property->ContainerPtrToValuePtr<void>(Actor), Value);
				return true;
			};
			auto ReadBool = [this, Actor](const TCHAR* Name)
			{
				auto* Property = FindFProperty<FBoolProperty>(Actor->GetClass(), Name);
				return TestNotNull(FString::Printf(TEXT("Runtime bool member %s exists"), Name), Property)
					&& Property->GetPropertyValue_InContainer(Actor);
			};
			if (FCString::Strcmp(Name, TEXT("health_system")) == 0)
			{
				TestEqual(TEXT("Actor has the saved maxHealth default"), ReadNumber(TEXT("Health")), 125.5);
				TestEqual(TEXT("Actor has the typed damageAmount default"), ReadNumber(TEXT("DamageAmount")), 25.0);
				TestEqual(TEXT("Actor has the typed healAmount default"), ReadNumber(TEXT("HealAmount")), 15.0);
				if (!Invoke(TEXT("TakeDamage"))) return false;
				TestEqual(TEXT("Damage executes authored subtraction"), ReadNumber(TEXT("Health")), 100.5);
				if (!Invoke(TEXT("Heal"))) return false;
				TestEqual(TEXT("Heal executes authored addition"), ReadNumber(TEXT("Health")), 115.5);
				if (!SetNumber(TEXT("DamageAmount"), 1000.0) || !Invoke(TEXT("TakeDamage"))) return false;
				TestEqual(TEXT("Damage clamps at zero"), ReadNumber(TEXT("Health")), 0.0);
				if (!SetNumber(TEXT("HealAmount"), 1000.0) || !Invoke(TEXT("Heal"))) return false;
				TestEqual(TEXT("Healing clamps at MaxHealth"), ReadNumber(TEXT("Health")), 125.5);
				if (!SetNumber(TEXT("DamageAmount"), -10.0) || !Invoke(TEXT("TakeDamage"))) return false;
				TestEqual(TEXT("Negative damage cannot heal"), ReadNumber(TEXT("Health")), 125.5);
				if (!SetNumber(TEXT("HealAmount"), -10.0) || !Invoke(TEXT("Heal"))) return false;
				TestEqual(TEXT("Negative healing cannot cause damage"), ReadNumber(TEXT("Health")), 125.5);
				if (!SetNumber(TEXT("MaxHealth"), -1.0) || !Invoke(TEXT("Heal"))) return false;
				TestEqual(TEXT("Runtime negative maximum safely clamps to zero"), ReadNumber(TEXT("Health")), 0.0);
			}
			else if (FCString::Strcmp(Name, TEXT("timer_loop")) == 0)
			{
				auto Advance = [World](float Seconds)
				{
					// TimerManager permits one tick per engine frame. Advance that
					// frame identity while exercising the real isolated Game world.
					++GFrameCounter;
					World->Tick(LEVELTICK_All, Seconds);
				};
				TestFalse(TEXT("Timer is initially stopped"), ReadBool(TEXT("bTimerRunning")));
				if (!Invoke(TEXT("StartTimer")) || !Invoke(TEXT("StartTimer"))) return false;
				TestTrue(TEXT("Start event exposes running state"), ReadBool(TEXT("bTimerRunning")));
				Advance(0.01f);
				TestEqual(TEXT("Timer does not increment before interval"), ReadNumber(TEXT("LoopCount")), 0.0);
				Advance(0.14f);
				TestEqual(TEXT("Repeated starts schedule one callback"), ReadNumber(TEXT("LoopCount")), 1.0);
				Advance(0.14f);
				TestEqual(TEXT("Real world ticks repeatedly execute the callback"), ReadNumber(TEXT("LoopCount")), 2.0);
				if (!Invoke(TEXT("StartTimer")) || !Invoke(TEXT("StartTimer"))) return false;
				Advance(0.05f);
				TestEqual(TEXT("Active restart resets the interval"), ReadNumber(TEXT("LoopCount")), 2.0);
				Advance(0.09f);
				TestEqual(TEXT("Active restart does not stack timers"), ReadNumber(TEXT("LoopCount")), 3.0);
				if (!Invoke(TEXT("StopTimer"))) return false;
				TestFalse(TEXT("Stop event exposes stopped state"), ReadBool(TEXT("bTimerRunning")));
				TestFalse(TEXT("Stop removes the actual scheduled timer"), UKismetSystemLibrary::K2_TimerExists(Actor, TEXT("FixtureLoop")));
				Advance(0.28f);
				if (!Invoke(TEXT("FixtureLoop"))) return false;
				TestEqual(TEXT("Stopped timer and direct callback leave counter unchanged"), ReadNumber(TEXT("LoopCount")), 3.0);
				if (!Invoke(TEXT("StartTimer"))) return false;
				Advance(0.14f);
				TestEqual(TEXT("Stopped timer can restart while retaining its counter"), ReadNumber(TEXT("LoopCount")), 4.0);
				Actor->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
				TestFalse(TEXT("Actual EndPlay executes authored running-state cleanup"), ReadBool(TEXT("bTimerRunning")));
				TestTrue(TEXT("Actual EndPlay records terminal lifecycle state"), ReadBool(TEXT("bTimerEnded")));
				TestFalse(TEXT("EndPlay removes the actual timer binding"), UKismetSystemLibrary::K2_TimerExists(Actor, TEXT("FixtureLoop")));
				if (!Invoke(TEXT("StartTimer"))) return false;
				TestFalse(TEXT("Start cannot reactivate an ended actor"), ReadBool(TEXT("bTimerRunning")));
				TestFalse(TEXT("Post-EndPlay Start cannot recreate a binding"), UKismetSystemLibrary::K2_TimerExists(Actor, TEXT("FixtureLoop")));
				Advance(0.5f);
				TestEqual(TEXT("Ended actor receives no repeated callback"), ReadNumber(TEXT("LoopCount")), 4.0);
			}
			else if (FCString::Strcmp(Name, TEXT("interactable_actor")) == 0)
			{
				TestNotNull(TEXT("Interaction template creates its real SCS component"), Actor->FindComponentByClass<USphereComponent>());
				TestFalse(TEXT("Typed initiallyInteractable default reaches the actor"), ReadBool(TEXT("bIsInteractable")));
				if (!Invoke(TEXT("Interact"))) return false;
				TestEqual(TEXT("False guard prevents interaction"), ReadNumber(TEXT("InteractionCount")), 0.0);
				auto* Guard = FindFProperty<FBoolProperty>(Actor->GetClass(), TEXT("bIsInteractable"));
				if (!TestNotNull(TEXT("Interaction guard is writable"), Guard)) return false;
				Guard->SetPropertyValue_InContainer(Actor, true);
				if (!Invoke(TEXT("Interact")) || !Invoke(TEXT("Interact"))) return false;
				TestEqual(TEXT("True guard allows repeated authored interactions"), ReadNumber(TEXT("InteractionCount")), 2.0);
				Guard->SetPropertyValue_InContainer(Actor, false);
				if (!Invoke(TEXT("Interact"))) return false;
				TestEqual(TEXT("Disabling guard prevents later interaction"), ReadNumber(TEXT("InteractionCount")), 2.0);
			}
			Actor->Destroy();
		}
		TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
		Rollback->SetStringField(TEXT("action"), TEXT("rollback"));
		Rollback->SetStringField(TEXT("runId"), Executed.Data->GetStringField(TEXT("runId")));
		Rollback->SetStringField(TEXT("approvePlanDigest"), Digest);
		if (!TestTrue(TEXT("Template uses the existing Workflow rollback"), Runtime->HandleRequest(Rollback).bOk))
		{
			return false;
		}
		Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath);
		if (!TestNotNull(TEXT("Restored template Blueprint exists"), Blueprint))
		{
			return false;
		}
		TestEqual(TEXT("Rollback restores the pre-template structure"),
			UEAIIntegration::Workflow::FWorkflowRuntime::ComputeAssetStructureHash(Blueprint), BeforeHash);
		TestEqual(TEXT("Rollback removes template-owned variable additions"), Blueprint->NewVariables.Num(), BeforeVariableCount);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintTemplateCrossAssetAcceptanceTest,
	"UE_AI_integration.Blueprint.Template.CrossAssetRenameRedirectorAndBytecodeAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintTemplateCrossAssetAcceptanceTest::RunTest(const FString&)
{
	UUEAIIntegrationSubsystem* Subsystem = GEditor
		? GEditor->GetEditorSubsystem<UUEAIIntegrationSubsystem>()
		: nullptr;
	FMCPToolRegistry* Registry = Subsystem ? Subsystem->GetRegistry() : nullptr;
	FUEAIIntegrationServer* Server = Subsystem ? Subsystem->GetServer() : nullptr;
	auto* Runtime = Server ? Server->GetWorkflowRuntimeForTesting() : nullptr;
	if (!TestNotNull(TEXT("Template cross-asset acceptance has the integration registry"), Registry)
		|| !TestNotNull(TEXT("Template cross-asset acceptance has the Workflow runtime"), Runtime))
	{
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ParentPath = TEXT("/Game/Automation/UEAI_TemplateParent_") + Suffix;
	const FString ChildPath = TEXT("/Game/Automation/UEAI_TemplateChild_") + Suffix;
	const FString RenamedPath = ParentPath + TEXT("_Renamed");
	const FString ParentName = FPackageName::GetLongPackageAssetName(ParentPath);
	const FString ParentObjectPath = ParentPath + TEXT(".") + ParentName;
	ON_SCOPE_EXIT
	{
		for (const FString& Path : {ChildPath, RenamedPath, ParentPath})
		{
			if (UEditorAssetLibrary::DoesAssetExist(Path))
			{
				TestTrue(TEXT("Template cross-asset fixture is removed"),
					UEditorAssetLibrary::DeleteAsset(Path));
			}
		}
	};

	auto Create = MakeShared<FJsonObject>();
	Create->SetStringField(TEXT("blueprintName"), ParentName);
	Create->SetStringField(TEXT("packagePath"), TEXT("/Game/Automation"));
	Create->SetStringField(TEXT("parentClass"), TEXT("Actor"));
	const FMCPToolResult Created = Registry->ExecuteTool(TEXT("blueprint.asset.create"), Create);
	if (!TestTrue(TEXT("Template cross-asset parent fixture creates"), Created.bSuccess))
	{
		AddError(Created.ErrorCode + TEXT(": ") + Created.ErrorMessage);
		return false;
	}
	UBlueprint* Parent = LoadObject<UBlueprint>(nullptr, *ParentPath);
	if (!TestNotNull(TEXT("Template cross-asset parent fixture loads"), Parent)
		|| !TestTrue(TEXT("Template cross-asset parent baseline saves"),
			UEditorAssetLibrary::SaveAsset(ParentPath, false)))
	{
		return false;
	}

	auto Apply = MakeTemplateParams(TEXT("health_system"));
	Apply->SetStringField(TEXT("blueprint"), ParentPath);
	auto Values = MakeShared<FJsonObject>();
	Values->SetNumberField(TEXT("maxHealth"), 125.5);
	Values->SetNumberField(TEXT("damageAmount"), 25.0);
	Values->SetNumberField(TEXT("healAmount"), 15.0);
	Apply->SetObjectField(TEXT("parameters"), Values);
	const FMCPToolResult Planned = Registry->ExecuteTool(TEXT("blueprint.template.apply"), Apply);
	if (!TestTrue(TEXT("Template cross-asset application plans"), Planned.bSuccess)
		|| !TestNotNull(TEXT("Template cross-asset plan has a workflow"), Planned.Data.Get()))
	{
		return false;
	}
	const FMCPResult Executed = ExecuteTemplateWorkflow(
		*Runtime,
		Planned.Data->GetObjectField(TEXT("workflow")),
		Planned.Data->GetStringField(TEXT("planDigest")));
	if (!TestTrue(TEXT("Template cross-asset application executes"), Executed.bOk))
	{
		AddError(Executed.Error.Code + TEXT(": ") + Executed.Error.Message);
		return false;
	}
	Parent = LoadObject<UBlueprint>(nullptr, *ParentPath);
	if (!TestNotNull(TEXT("Template cross-asset parent reloads after application"), Parent)
		|| !TestNotNull(TEXT("Template cross-asset parent has a generated class"), Parent->GeneratedClass))
	{
		return false;
	}

	UPackage* ChildPackage = CreatePackage(*ChildPath);
	UBlueprint* Child = ChildPackage
		? FKismetEditorUtilities::CreateBlueprint(
			Parent->GeneratedClass,
			ChildPackage,
			FName(*FPackageName::GetLongPackageAssetName(ChildPath)),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UEAI.TemplateCrossAssetAcceptance")))
		: nullptr;
	if (!TestNotNull(TEXT("Template cross-asset child fixture creates"), Child))
	{
		return false;
	}
	FAssetRegistryModule::AssetCreated(Child);
	FKismetEditorUtilities::CompileBlueprint(Child, EBlueprintCompileOptions::SkipSave);
	if (!TestEqual(TEXT("Template cross-asset child compiles"), Child->Status, BS_UpToDate)
		|| !TestTrue(TEXT("Template cross-asset child saves"), UEditorAssetLibrary::SaveAsset(ChildPath, false)))
	{
		return false;
	}

	IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
	AssetRegistry.ScanPathsSynchronous({TEXT("/Game/Automation")}, true);
	auto FindReferences = [Registry](const FString& AssetPath)
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("assetPath"), AssetPath);
		return Registry->ExecuteTool(TEXT("blueprint.asset.references"), Params);
	};
	auto ContainsExactReferencer = [Child](const TSharedPtr<FJsonObject>& Data)
	{
		if (!Data.IsValid()) return false;
		const FString ChildIdentity = FAssetIdentifier::FromString(Child->GetPathName()).ToString();
		const TArray<TSharedPtr<FJsonValue>>* Exact = nullptr;
		if (!Data->TryGetArrayField(TEXT("exactObjectReferencers"), Exact) || !Exact) return false;
		return Exact->ContainsByPredicate([&ChildIdentity](const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid() && Value->Type == EJson::String
				&& Value->AsString() == ChildIdentity;
		});
	};
	FMCPToolResult BeforeRename = FindReferences(Parent->GetPathName());
	if (!TestTrue(TEXT("Template parent references query succeeds before rename"), BeforeRename.bSuccess)
		|| !TestNotNull(TEXT("Template parent references read-back exists"), BeforeRename.Data.Get()))
	{
		return false;
	}
	TestTrue(TEXT("Template child is an exact referencer before rename"),
		BeforeRename.Data->GetBoolField(TEXT("exactObjectReferenceAvailable"))
			&& ContainsExactReferencer(BeforeRename.Data));
	TestTrue(TEXT("Template parent references have complete identity before rename"),
		BeforeRename.Data->GetBoolField(TEXT("identityComplete")));

	auto Rename = MakeShared<FJsonObject>();
	Rename->SetStringField(TEXT("assetPath"), ParentPath);
	Rename->SetStringField(TEXT("newPath"), RenamedPath);
	const FMCPToolResult Renamed = Registry->ExecuteTool(TEXT("blueprint.asset.rename"), Rename);
	if (!TestTrue(TEXT("Template parent rename succeeds"), Renamed.bSuccess)
		|| !TestNotNull(TEXT("Template parent rename receipt exists"), Renamed.Data.Get()))
	{
		return false;
	}
	TestTrue(TEXT("Template parent rename creates an Asset Registry redirector"),
		Renamed.Data->GetBoolField(TEXT("redirectorCreated")));
	Parent = LoadObject<UBlueprint>(nullptr, *RenamedPath);
	Child = LoadObject<UBlueprint>(nullptr, *ChildPath);
	if (!TestNotNull(TEXT("Renamed template parent reloads"), Parent)
		|| !TestNotNull(TEXT("Renamed template child reloads"), Child))
	{
		return false;
	}
	TestEqual(TEXT("Template child parent class follows the renamed asset"),
		Child->ParentClass.Get(), Parent->GeneratedClass.Get());
	if (!TestTrue(TEXT("Renamed template child saves its fixed-up reference"),
		UEditorAssetLibrary::SaveAsset(ChildPath, false)))
	{
		return false;
	}
	const FAssetData Redirector = AssetRegistry.GetAssetByObjectPath(
		FSoftObjectPath(ParentObjectPath));
	TestTrue(TEXT("Template rename leaves a redirector at the old object path"), Redirector.IsValid() && Redirector.IsRedirector());

	FMCPToolResult AfterRename = FindReferences(Parent->GetPathName());
	if (!TestTrue(TEXT("Renamed template parent references query succeeds"), AfterRename.bSuccess)
		|| !TestNotNull(TEXT("Renamed template references read-back exists"), AfterRename.Data.Get()))
	{
		return false;
	}
	TestTrue(TEXT("Template child remains an exact referencer after rename"),
		AfterRename.Data->GetBoolField(TEXT("exactObjectReferenceAvailable"))
			&& ContainsExactReferencer(AfterRename.Data));
	TestTrue(TEXT("Renamed template reference identity remains complete"),
		AfterRename.Data->GetBoolField(TEXT("identityComplete")));

	TArray<UPackage*> PackagesToReload{Parent->GetOutermost(), Child->GetOutermost()};
	FText ReloadError;
	if (!TestTrue(TEXT("Renamed template parent and child reload from disk"),
		UPackageTools::ReloadPackages(PackagesToReload, ReloadError, EReloadPackagesInteractionMode::AssumePositive)))
	{
		AddError(ReloadError.ToString());
		return false;
	}
	Parent = LoadObject<UBlueprint>(nullptr, *RenamedPath);
	Child = LoadObject<UBlueprint>(nullptr, *ChildPath);
	if (!TestNotNull(TEXT("Reloaded renamed template parent exists"), Parent)
		|| !TestNotNull(TEXT("Reloaded renamed template child exists"), Child))
	{
		return false;
	}
	TestEqual(TEXT("Reloaded child keeps the renamed parent class"),
		Child->ParentClass.Get(), Parent->GeneratedClass.Get());

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("Renamed template runtime world creates"), World)
		|| !TestNotNull(TEXT("Renamed template runtime engine exists"), GEngine))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	FURL URL;
	World->InitializeActorsForPlay(URL);
	World->BeginPlay();
	AActor* Actor = World->SpawnActor<AActor>(Child->GeneratedClass, FTransform::Identity);
	if (!TestNotNull(TEXT("Renamed template child spawns in a Game world"), Actor))
	{
		return false;
	}
	ON_SCOPE_EXIT { if (Actor) Actor->Destroy(); };
	TSharedRef<FJsonObject> RuntimeParams = MakeShared<FJsonObject>();
	RuntimeParams->SetStringField(TEXT("blueprint"), ChildPath);
	const FMCPToolResult RuntimeObserved = Registry->ExecuteTool(
		TEXT("blueprint.asset.runtime.verify"), RuntimeParams);
	TestTrue(TEXT("Runtime acceptance identifies the renamed template child"),
		RuntimeObserved.bSuccess && RuntimeObserved.Data
			&& RuntimeObserved.Data->GetBoolField(TEXT("runtimeVerified"))
			&& RuntimeObserved.Data->GetStringField(TEXT("instance")) == Actor->GetPathName()
			&& RuntimeObserved.Data->GetStringField(TEXT("instanceClass")) == Actor->GetClass()->GetPathName());
	FNumericProperty* Health = FindFProperty<FNumericProperty>(Actor->GetClass(), TEXT("Health"));
	UFunction* TakeDamage = Actor->FindFunction(FName(TEXT("TakeDamage")));
	if (!TestNotNull(TEXT("Renamed template child retains Health bytecode state"), Health)
		|| !TestNotNull(TEXT("Renamed template child retains TakeDamage bytecode"), TakeDamage))
	{
		return false;
	}
	TestEqual(TEXT("Renamed template child starts with the authored health default"),
		Health->GetFloatingPointPropertyValue(Health->ContainerPtrToValuePtr<void>(Actor)), 125.5);
	Actor->ProcessEvent(TakeDamage, nullptr);
	TestEqual(TEXT("Renamed template child executes the persisted TakeDamage bytecode"),
		Health->GetFloatingPointPropertyValue(Health->ContainerPtrToValuePtr<void>(Actor)), 100.5);
	return true;
}

#endif
