// Opt-in acceptance driven by tests/hostproject/run-blueprint-persistence.ps1.
// Each phase runs in a different isolated Editor; no in-process reload can
// satisfy the PID checks. Assets live only in that harness's /Game directory.
#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
void RegisterBlueprintMutationTools(FMCPToolRegistry& Registry);
}

namespace UEAIBlueprintCrossProcessPrivate
{
constexpr TCHAR PreparePidKey[] = TEXT("UEAI.Persistence.PreparePid");
constexpr TCHAR RenamePidKey[] = TEXT("UEAI.Persistence.RenamePid");
constexpr TCHAR EventName[] = TEXT("SetPersistentCounter");
constexpr TCHAR CounterName[] = TEXT("PersistentCounter");

UBlueprint* CreateFixture(const FString& PackagePath, UClass* Parent)
{
	if (!Parent || UEditorAssetLibrary::DoesAssetExist(PackagePath))
		return nullptr;
	UPackage* Package = CreatePackage(*PackagePath);
	UBlueprint* Blueprint = Package ? FKismetEditorUtilities::CreateBlueprint(
		Parent, Package, FName(*FPackageName::GetLongPackageAssetName(PackagePath)),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("UEAI.CrossProcessAcceptance"))) : nullptr;
	if (Blueprint)
		FAssetRegistryModule::AssetCreated(Blueprint);
	return Blueprint;
}

bool SaveFixture(UBlueprint* Blueprint, FString& Error)
{
	if (!Blueprint)
		return false;
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipSave);
	if (Blueprint->Status == BS_Error || !Blueprint->GeneratedClass)
	{
		Error = TEXT("Persistence fixture failed to compile.");
		return false;
	}
	UEAIIntegration::Infrastructure::FBlueprintPersistenceError SaveError;
	const bool bSaved = UEAIIntegration::Infrastructure::SaveBlueprintPackage(Blueprint, nullptr, SaveError);
	Error = SaveError.Message;
	return bSaved;
}

bool AddRuntimeEvent(UBlueprint* Blueprint)
{
	if (!Blueprint || Blueprint->UbergraphPages.IsEmpty())
		return false;
	FEdGraphPinType Type;
	Type.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(CounterName), Type, TEXT("0")))
		return false;
	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(Graph, NAME_None, RF_Transactional);
	Event->CustomFunctionName = FName(EventName);
	Event->CreateNewGuid();
	Graph->AddNode(Event, false, false);
	Event->AllocateDefaultPins();
	UK2Node_VariableSet* Setter = NewObject<UK2Node_VariableSet>(Graph, NAME_None, RF_Transactional);
	Setter->VariableReference.SetSelfMember(FName(CounterName));
	Setter->CreateNewGuid();
	Graph->AddNode(Setter, false, false);
	Setter->AllocateDefaultPins();
	UEdGraphPin* Value = Setter->FindPin(FName(CounterName));
	UEdGraphPin* Execute = Setter->FindPin(UEdGraphSchema_K2::PN_Execute);
	UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then);
	const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
	if (!Value || !Execute || !Then)
		return false;
	Schema->TrySetDefaultValue(*Value, TEXT("73"));
	return Value->DefaultValue == TEXT("73") && Schema->TryCreateConnection(Then, Execute);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAIBlueprintCrossProcessContractTest,
	"UE_AI_integration.Blueprint.CrossProcess.PersistenceReferencesAndRuntime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAIBlueprintCrossProcessContractTest::RunTest(const FString&)
{
	using namespace UEAIBlueprintCrossProcessPrivate;
	const FString RunId = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_BP_PERSISTENCE_RUN"));
	const FString Phase = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_BP_PERSISTENCE_PHASE"));
	if (RunId.IsEmpty() && Phase.IsEmpty())
	{
		AddInfo(TEXT("Cross-process acceptance requires run-blueprint-persistence.ps1; no persistence evidence produced by this ordinary suite run."));
		return true;
	}
	FGuid ParsedRun;
	if (!TestTrue(TEXT("Persistence run uses an exact GUID"), FGuid::ParseExact(RunId, EGuidFormats::Digits, ParsedRun))
		|| !TestTrue(TEXT("Persistence phase is explicit"), Phase == TEXT("prepare") || Phase == TEXT("rename") || Phase == TEXT("verify")))
		return false;
	const FString Root = TEXT("/Game/Automation/UEAI_Persistence_") + ParsedRun.ToString(EGuidFormats::Digits);
	const FString TargetPath = Root + TEXT("_Target");
	const FString SourcePath = Root + TEXT("_Source");
	const FString RenamedPath = Root + TEXT("_Renamed");
	const FString CurrentPid = FString::FromInt(FPlatformProcess::GetCurrentProcessId());
	FString Error;
	if (Phase == TEXT("prepare"))
	{
		UBlueprint* Target = CreateFixture(TargetPath, AActor::StaticClass());
		if (!TestNotNull(TEXT("Parent fixture creates"), Target)
			|| !TestTrue(TEXT("Parent authors real runtime event"), AddRuntimeEvent(Target)))
			return false;
		Target->GetOutermost()->GetMetaData()->SetValue(Target, PreparePidKey, *CurrentPid);
		if (!TestTrue(TEXT("Parent compiles and saves"), SaveFixture(Target, Error)))
		{
			AddError(Error);
			return false;
		}
		UBlueprint* Source = CreateFixture(SourcePath, Target->GeneratedClass);
		if (!TestNotNull(TEXT("Child fixture references parent asset"), Source))
			return false;
		Source->GetOutermost()->GetMetaData()->SetValue(Source, PreparePidKey, *CurrentPid);
		if (!TestTrue(TEXT("Child compiles and saves"), SaveFixture(Source, Error)))
			AddError(Error);
		return !HasAnyErrors();
	}

	// Load saved packages before compiling or touching them. The recorded PID
	// proves that this is disk persistence across processes, not reload in place.
	UBlueprint* Source = LoadObject<UBlueprint>(nullptr, *SourcePath);
	UBlueprint* Target = LoadObject<UBlueprint>(nullptr, *(Phase == TEXT("verify") ? RenamedPath : TargetPath));
	if (!TestNotNull(TEXT("Child loads in a fresh process"), Source)
		|| !TestNotNull(TEXT("Parent loads in a fresh process"), Target))
		return false;
	for (UBlueprint* Blueprint : {Source, Target})
	{
		const FString PreparePid = Blueprint->GetOutermost()->GetMetaData()->GetValue(Blueprint, PreparePidKey);
		TestFalse(TEXT("Saved prepare process identity exists"), PreparePid.IsEmpty());
		TestNotEqual(TEXT("Readback uses a different process from preparation"), PreparePid, CurrentPid);
		TestFalse(TEXT("Disk-loaded fixture is clean"), Blueprint->GetOutermost()->IsDirty());
	}
	TestEqual(TEXT("Child parent reference resolves exactly"), Source->ParentClass.Get(), Target->GeneratedClass.Get());
	TestNotNull(TEXT("Inherited event survives disk persistence"), Source->GeneratedClass
		? Source->GeneratedClass->FindFunctionByName(FName(EventName)) : nullptr);
	if (HasAnyErrors())
		return false;
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(Registry);
	UEAIIntegrationTools::RegisterBlueprintMutationTools(Registry);
	Registry.EndDomainRegistration();
	FAssetRegistryModule::GetRegistry().ScanPathsSynchronous({TEXT("/Game/Automation")}, true);
	auto QueryParams = MakeShared<FJsonObject>();
	QueryParams->SetStringField(TEXT("assetPath"), Target->GetPathName());
	const FMCPToolResult References = Registry.ExecuteTool(TEXT("blueprint.asset.references"), QueryParams);
	if (!TestTrue(TEXT("Persisted parent reference query succeeds"), References.bSuccess) || !References.Data)
		return false;
	TestTrue(TEXT("Reference identity is complete"), References.Data->GetBoolField(TEXT("identityComplete")));
	TestTrue(TEXT("Saved child is a Blueprint referencer"), References.Data->GetIntegerField(TEXT("blueprintReferencerCount")) >= 1);
	if (Phase == TEXT("rename"))
	{
		auto RenameParams = MakeShared<FJsonObject>();
		RenameParams->SetStringField(TEXT("assetPath"), TargetPath);
		RenameParams->SetStringField(TEXT("newPath"), RenamedPath);
		const FMCPToolResult Renamed = Registry.ExecuteTool(TEXT("blueprint.asset.rename"), RenameParams);
		if (!TestTrue(TEXT("Referenced parent renames through native capability"), Renamed.bSuccess) || !Renamed.Data)
		{
			AddError(Renamed.ErrorMessage);
			return false;
		}
		TestTrue(TEXT("Rename creates a persisted redirector"), Renamed.Data->GetBoolField(TEXT("redirectorCreated")));
		Source->GetOutermost()->GetMetaData()->SetValue(Source, RenamePidKey, *CurrentPid);
		if (!TestTrue(TEXT("Renamed reference saves to child package"), SaveFixture(Source, Error)))
			AddError(Error);
		return !HasAnyErrors();
	}
	const FString RenamePid = Source->GetOutermost()->GetMetaData()->GetValue(Source, RenamePidKey);
	TestFalse(TEXT("Rename phase identity survives"), RenamePid.IsEmpty());
	TestNotEqual(TEXT("Verification uses a third process after rename"), RenamePid, CurrentPid);
	TestEqual(TEXT("Original asset path redirects to exact renamed parent"), LoadObject<UBlueprint>(nullptr, *TargetPath), Target);

	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
		if (!TestNotNull(TEXT("Persistence runtime world creates"), World) || !TestNotNull(TEXT("Runtime engine exists"), GEngine))
			return false;
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
		FURL URL;
		World->InitializeActorsForPlay(URL);
		World->BeginPlay();
		AActor* Actor = World->SpawnActor<AActor>(Source->GeneratedClass, FTransform::Identity);
		if (!TestNotNull(TEXT("Saved child spawns an actual runtime instance"), Actor))
			return false;
		FIntProperty* Counter = FindFProperty<FIntProperty>(Actor->GetClass(), FName(CounterName));
		UFunction* Event = Actor->FindFunction(FName(EventName));
		if (!TestNotNull(TEXT("Runtime instance resolves persisted property"), Counter)
			|| !TestNotNull(TEXT("Runtime instance resolves inherited event"), Event))
			return false;
		TestEqual(TEXT("Runtime starts at authored baseline"), Counter->GetPropertyValue_InContainer(Actor), 0);
		Actor->ProcessEvent(Event, nullptr);
		TestEqual(TEXT("Inherited authored event changes actual runtime state"), Counter->GetPropertyValue_InContainer(Actor), 73);
		auto RuntimeParams = MakeShared<FJsonObject>();
		RuntimeParams->SetStringField(TEXT("blueprint"), SourcePath);
		const FMCPToolResult Runtime = Registry.ExecuteTool(TEXT("blueprint.asset.runtime.verify"), RuntimeParams);
		TestTrue(TEXT("Runtime capability observes saved actor"), Runtime.bSuccess && Runtime.Data
			&& Runtime.Data->GetBoolField(TEXT("runtimeVerified"))
			&& Runtime.Data->GetStringField(TEXT("instance")) == Actor->GetPathName());
		Actor->Destroy();
	}
	if (!HasAnyErrors())
	{
		for (const FString& Path : {SourcePath, TargetPath, RenamedPath})
			TestTrue(TEXT("Only this run's persisted fixture is removed"),
				!UEditorAssetLibrary::DoesAssetExist(Path) || UEditorAssetLibrary::DeleteAsset(Path));
	}
	return !HasAnyErrors();
}
#endif
