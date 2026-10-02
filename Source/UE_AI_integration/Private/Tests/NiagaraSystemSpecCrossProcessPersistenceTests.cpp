// Opt-in cross-process acceptance for Niagara system-spec persistence.
//
// The ordinary system-spec contracts exercise export/import in one Editor. This
// test deliberately leaves an authored package on disk between three isolated
// Editor processes. The host runner owns the process/module identity gate; this
// test owns the native export/import/save/read-back evidence. The generated
// package is private to the run GUID and is deleted by the verify phase.
#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Tools/MCPToolRegistry.h"
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraTools(FMCPToolRegistry& Registry);
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry);
void RegisterNiagaraDynamicInputTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace UEAINiagaraSpecPersistencePrivate
{
constexpr TCHAR RunEnv[] = TEXT("UEAI_NIAGARA_SPEC_RUN");
constexpr TCHAR PhaseEnv[] = TEXT("UEAI_NIAGARA_SPEC_PHASE");
constexpr TCHAR ArtifactEnv[] = TEXT("UEAI_NIAGARA_SPEC_ARTIFACT");

struct FFixture
{
	FString RunId;
	FString PackagePath;
	FString SystemPath;
	FString EmitterName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	UNiagaraGraph* ParticleGraph = nullptr;
	UNiagaraNodeOutput* ParticleUpdateOutput = nullptr;
	UNiagaraNodeFunctionCall* Module = nullptr;
	UNiagaraEmitter* Emitter = nullptr;
	FGuid EmitterVersion;
	UNiagaraSpriteRendererProperties* Sprite = nullptr;
	UNiagaraMeshRendererProperties* Mesh = nullptr;
	UNiagaraRibbonRendererProperties* Ribbon = nullptr;
	FNiagaraVariable UserFloat;
	bool bDynamicInputCovered = false;
};

bool ReadEnvironment(FString& OutRunId, FString& OutPhase, FString& OutArtifact)
{
	OutRunId = FPlatformMisc::GetEnvironmentVariable(RunEnv);
	OutPhase = FPlatformMisc::GetEnvironmentVariable(PhaseEnv);
	OutArtifact = FPlatformMisc::GetEnvironmentVariable(ArtifactEnv);
	return !OutRunId.IsEmpty() &&
		(OutPhase == TEXT("prepare") || OutPhase == TEXT("import") || OutPhase == TEXT("verify")) &&
		!OutArtifact.IsEmpty();
}

TSharedPtr<FJsonObject> CloneJson(const TSharedPtr<FJsonObject>& Source)
{
	if (!Source.IsValid()) return nullptr;
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Source.ToSharedRef(), Writer)) return nullptr;
	TSharedPtr<FJsonObject> Clone;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Clone) ? Clone : nullptr;
}

bool SaveJsonFile(const FString& Filename, const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid()) return false;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer) &&
		FFileHelper::SaveStringToFile(Text, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

TSharedPtr<FJsonObject> LoadJsonFile(const FString& Filename)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Filename)) return nullptr;
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
}

bool ResetGraph(UNiagaraGraph* Graph, UNiagaraScript* Script, ENiagaraScriptUsage Usage)
{
	return Graph && Script && FNiagaraStackGraphUtilities::ResetGraphForOutput(
		*Graph, Usage, Script->GetUsageId());
}

UNiagaraNodeOutput* FindOutput(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage)
{
	if (!Graph) return nullptr;
	TArray<UNiagaraNodeOutput*> Outputs;
	Graph->GetNodesOfClass(Outputs);
	for (UNiagaraNodeOutput* Output : Outputs)

		if (Output && Output->GetUsage() == Usage) return Output;
	return nullptr;
}

UNiagaraScript* LoadModuleScript()
{
	UNiagaraScript* Script = LoadObject<UNiagaraScript>(
		nullptr, TEXT("/Niagara/Modules/Update/Lifetime/UpdateAge.UpdateAge"), nullptr, LOAD_NoWarn);
	return Script && Script->GetUsage() == ENiagaraScriptUsage::Module ? Script : nullptr;
}

UNiagaraScript* LoadCompatibleDynamicInput(const FNiagaraTypeDefinition& Type)
{
	static const TCHAR* Paths[] = {
		TEXT("/Niagara/DynamicInputs/Add/Add_Float.Add_Float"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Integer.Add_Integer"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Vector.Add_Vector"),
		TEXT("/Niagara/DynamicInputs/Add/Add_Vector2.Add_Vector2"),
		TEXT("/Niagara/DynamicInputs/Bool/InvertBool.InvertBool")};
	for (const TCHAR* Path : Paths)
	{
		UNiagaraScript* Candidate = LoadObject<UNiagaraScript>(nullptr, Path, nullptr, LOAD_NoWarn);
		if (!Candidate || Candidate->GetUsage() != ENiagaraScriptUsage::DynamicInput) continue;
		UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Candidate->GetLatestSource());
		if (!Source || !Source->NodeGraph) continue;
		TArray<UNiagaraNodeOutput*> Outputs;
		Source->NodeGraph->GetNodesOfClass(Outputs);
		for (UNiagaraNodeOutput* Output : Outputs)
		{
			if (Output && Output->GetUsage() == ENiagaraScriptUsage::DynamicInput &&
				Output->GetOutputs().Num() > 0 && Output->GetOutputs()[0].GetType() == Type)
				return Candidate;
		}
	}
	return nullptr;
}

bool CreateFixture(FFixture& Out)
{
	Out.PackagePath = TEXT("/Game/Automation/UEAI_NiagaraSpecPersistence_") + Out.RunId;
	Out.SystemPath = Out.PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(Out.PackagePath);
	Out.Package = CreatePackage(*Out.PackagePath);
	Out.System = Out.Package ? NewObject<UNiagaraSystem>(
		Out.Package, *FPackageName::GetLongPackageAssetName(Out.PackagePath),
		RF_Public | RF_Standalone | RF_Transactional) : nullptr;
	if (!Out.System) return false;
	UNiagaraSystemFactoryNew::InitializeSystem(Out.System, false);
	UNiagaraScript* Spawn = Out.System->GetSystemSpawnScript();
	UNiagaraScript* Update = Out.System->GetSystemUpdateScript();
	UNiagaraScriptSource* SystemSource = Spawn ? Cast<UNiagaraScriptSource>(Spawn->GetLatestSource()) : nullptr;
	if (!SystemSource || !ResetGraph(SystemSource->NodeGraph, Spawn, ENiagaraScriptUsage::SystemSpawnScript) ||
		!ResetGraph(SystemSource->NodeGraph, Update, ENiagaraScriptUsage::SystemUpdateScript)) return false;

	Out.Emitter = NewObject<UNiagaraEmitter>(Out.System, TEXT("PersistenceEmitter"), RF_Transactional);
	if (!Out.Emitter) return false;
	UNiagaraEmitterFactoryNew::InitializeEmitter(Out.Emitter, false);
	Out.EmitterVersion = Out.Emitter->GetExposedVersion().VersionGuid;
	Out.Sprite = NewObject<UNiagaraSpriteRendererProperties>(Out.Emitter, TEXT("PersistenceSprite"), RF_Transactional);
	Out.Mesh = NewObject<UNiagaraMeshRendererProperties>(Out.Emitter, TEXT("PersistenceMesh"), RF_Transactional);
	Out.Ribbon = NewObject<UNiagaraRibbonRendererProperties>(Out.Emitter, TEXT("PersistenceRibbon"), RF_Transactional);
	if (!Out.Sprite || !Out.Mesh || !Out.Ribbon) return false;
	Out.Sprite->bSubImageBlend = true;
	Out.Sprite->SubImageSize = FVector2D(4.0, 2.0);
	Out.Mesh->bSubImageBlend = true;
	Out.Mesh->SubImageSize = FVector2D(2.0, 2.0);
	Out.Emitter->AddRenderer(Out.Sprite, Out.EmitterVersion);
	Out.Emitter->AddRenderer(Out.Mesh, Out.EmitterVersion);
	Out.Emitter->AddRenderer(Out.Ribbon, Out.EmitterVersion);
	FNiagaraEmitterHandle Handle(*Out.Emitter, Out.EmitterVersion);
	Out.System->AddEmitterHandleDirect(Handle);
	Out.EmitterName = Handle.GetName().ToString();
	Out.UserFloat = FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.SpecRoundTrip"));
	if (!Out.System->GetExposedParameters().AddParameter(Out.UserFloat) ||
		!Out.System->GetExposedParameters().SetParameterValue<float>(1.0f, Out.UserFloat, false))
	{
		return false;
	}
	FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
	UNiagaraScriptSource* EmitterSource = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
	Out.ParticleGraph = EmitterSource ? EmitterSource->NodeGraph : nullptr;
	Out.ParticleUpdateOutput = FindOutput(Out.ParticleGraph, ENiagaraScriptUsage::ParticleUpdateScript);
	if (!Out.ParticleGraph || !Out.ParticleUpdateOutput) return false;

	// Author one event handler and one generic simulation stage so the
	// cross-process spec contract exercises their authored metadata and stable
	// usage identities.  Their dedicated capabilities own structural edits;
	// system-spec import treats these arrays as structure guarded fields.
	Out.EventHandlerUsageId = FGuid::NewGuid();
	FNiagaraEventScriptProperties EventHandler;
	EventHandler.ExecutionMode = EScriptExecutionMode::SpawnedParticles;
	EventHandler.SpawnNumber = 4;
	EventHandler.MinSpawnNumber = 2;
	EventHandler.MaxEventsPerFrame = 32;
	EventHandler.bRandomSpawnNumber = true;
	EventHandler.UpdateAttributeInitialValues = false;
	EventHandler.SourceEmitterID = Handle.GetId();
	EventHandler.SourceEventName = FName(TEXT("PersistenceBurst"));
	EventHandler.Script = NewObject<UNiagaraScript>(
		Out.Emitter, TEXT("PersistenceEventScript"), RF_Transactional);
	if (!EventHandler.Script)
	{
		return false;
	}
	EventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	EventHandler.Script->SetUsageId(Out.EventHandlerUsageId);
	EventHandler.Script->SetLatestSource(EmitterSource);
	Out.Emitter->AddEventHandler(EventHandler, Out.EmitterVersion);
	if (!ResetGraph(EmitterSource->NodeGraph, EventHandler.Script, ENiagaraScriptUsage::ParticleEventScript))
	{
		return false;
	}

	Out.SimulationStageUsageId = FGuid::NewGuid();
	UNiagaraSimulationStageGeneric* SimulationStage = NewObject<UNiagaraSimulationStageGeneric>(
		Out.Emitter, TEXT("PersistenceSimulationStage"), RF_Transactional);
	if (!SimulationStage)
	{
		return false;
	}
	SimulationStage->SimulationStageName = FName(TEXT("PersistenceStage"));
	SimulationStage->bEnabled = true;
	SimulationStage->OuterEmitterVersion = Out.EmitterVersion;
	SimulationStage->NumIterations.SetDefaultParameter(FNiagaraTypeDefinition::GetIntDef(), 3);
	SimulationStage->Script = NewObject<UNiagaraScript>(
		SimulationStage, TEXT("PersistenceSimulationStageScript"), RF_Transactional);
	if (!SimulationStage->Script)
	{
		return false;
	}
	SimulationStage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
	SimulationStage->Script->SetUsageId(Out.SimulationStageUsageId);
	SimulationStage->Script->SetLatestSource(EmitterSource);
	Out.Emitter->AddSimulationStage(SimulationStage, Out.EmitterVersion);
	if (!ResetGraph(EmitterSource->NodeGraph, SimulationStage->Script, ENiagaraScriptUsage::ParticleSimulationStageScript))
	{
		return false;
	}

	if (UNiagaraScript* ModuleScript = LoadModuleScript())
	{
		Out.Module = FNiagaraStackGraphUtilities::AddScriptModuleToStack(ModuleScript, *Out.ParticleUpdateOutput);
		if (Out.Module)
		{
			FCompileConstantResolver Resolver(Handle.GetInstance(), ENiagaraScriptUsage::ParticleUpdateScript);
			TArray<FNiagaraVariable> Inputs;
			TSet<FNiagaraVariable> Hidden;
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(
				*Out.Module, Inputs, Hidden, Resolver,
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);
			for (const FNiagaraVariable& Input : Inputs)
			{
				if (Input.IsDataInterface() || Input.IsUObject()) continue;
				if (UNiagaraScript* Dynamic = LoadCompatibleDynamicInput(Input.GetType()))
				{
					FNiagaraParameterHandle Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
						FNiagaraParameterHandle(Input.GetName()), Out.Module);
					UEdGraphPin& Pin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(
						*Out.Module, Aliased, Input.GetType(), FGuid(), FGuid());
					UNiagaraNodeFunctionCall* DynamicNode = nullptr;
					FNiagaraStackGraphUtilities::SetDynamicInputForFunctionInput(Pin, Dynamic, DynamicNode);
					Out.bDynamicInputCovered = DynamicNode != nullptr && DynamicNode->FunctionScript == Dynamic;
					if (Out.bDynamicInputCovered) break;
				}
			}
		}
	}
	Out.ParticleGraph->NotifyGraphChanged();
	Out.Package->MarkPackageDirty();
	return true;
}

bool SaveSystem(UNiagaraSystem* System)
{
	if (!System || !System->GetOutermost()) return false;
	const FString Filename = FPackageName::LongPackageNameToFilename(
		System->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	return UPackage::SavePackage(System->GetOutermost(), System, *Filename, Args) &&
		FPaths::FileExists(Filename) && !System->GetOutermost()->IsDirty();
}

void Register(FMCPToolRegistry& Registry)
{
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	UEAIIntegrationTools::RegisterNiagaraDynamicInputTools(Registry);
	Registry.EndDomainRegistration();
}

bool FindDynamicTree(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid()) return false;
	if (Value->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (Object->HasField(TEXT("dynamicInputTree"))) return true;
		for (const auto& Pair : Object->Values) if (FindDynamicTree(Pair.Value)) return true;
	}
	else if (Value->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& Child : Value->AsArray()) if (FindDynamicTree(Child)) return true;
	}
	return false;
}

bool HasRendererClass(const TSharedPtr<FJsonObject>& Spec, const TCHAR* Suffix)
{
	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("emitters"), Emitters) || !Emitters) return false;
	for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
	{
		const TSharedPtr<FJsonObject> Emitter = EmitterValue.IsValid() ? EmitterValue->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
		if (!Emitter.IsValid() || !Emitter->TryGetArrayField(TEXT("renderers"), Renderers) || !Renderers) continue;
		for (const TSharedPtr<FJsonValue>& RendererValue : *Renderers)
		{
			const TSharedPtr<FJsonObject> Renderer = RendererValue.IsValid() ? RendererValue->AsObject() : nullptr;
			if (Renderer.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(Suffix)) return true;
		}
	}
	return false;
}

bool HasEventHandlerUsage(const TSharedPtr<FJsonObject>& Spec, const FGuid& UsageId, const bool bImported = false)
{
	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("emitters"), Emitters) || !Emitters)
	{
		return false;
	}
	const FString ExpectedUsageId = UsageId.ToString(EGuidFormats::DigitsWithHyphensLower);
	for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
	{
		const TSharedPtr<FJsonObject> Emitter = EmitterValue.IsValid() ? EmitterValue->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Handlers = nullptr;
		if (!Emitter.IsValid() || !Emitter->TryGetArrayField(TEXT("eventHandlers"), Handlers) || !Handlers)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& HandlerValue : *Handlers)
		{
			const TSharedPtr<FJsonObject> Handler = HandlerValue.IsValid() ? HandlerValue->AsObject() : nullptr;
			if (Handler.IsValid() && Handler->GetStringField(TEXT("usageId")) == ExpectedUsageId)
			{
				return Handler->GetStringField(TEXT("sourceEventName")) == (bImported ? TEXT("PersistenceBurstImported") : TEXT("PersistenceBurst"))
					&& Handler->GetBoolField(TEXT("randomSpawnNumber"))
					&& Handler->GetNumberField(TEXT("spawnNumber")) == (bImported ? 7.0 : 4.0)
					&& Handler->GetNumberField(TEXT("minSpawnNumber")) == (bImported ? 2.0 : 2.0)
					&& Handler->GetNumberField(TEXT("maxEventsPerFrame")) == 32.0
					&& (!bImported || Handler->GetStringField(TEXT("sourceEmitterId")).IsEmpty());
			}
		}
	}
	return false;
}

bool HasSimulationStageUsage(const TSharedPtr<FJsonObject>& Spec, const FGuid& UsageId, const bool bImported = false)
{
	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	if (!Spec.IsValid() || !Spec->TryGetArrayField(TEXT("emitters"), Emitters) || !Emitters)
	{
		return false;
	}
	const FString ExpectedUsageId = UsageId.ToString(EGuidFormats::DigitsWithHyphensLower);
	for (const TSharedPtr<FJsonValue>& EmitterValue : *Emitters)
	{
		const TSharedPtr<FJsonObject> Emitter = EmitterValue.IsValid() ? EmitterValue->AsObject() : nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Stages = nullptr;
		if (!Emitter.IsValid() || !Emitter->TryGetArrayField(TEXT("simulationStages"), Stages) || !Stages)
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& StageValue : *Stages)
		{
			const TSharedPtr<FJsonObject> Stage = StageValue.IsValid() ? StageValue->AsObject() : nullptr;
			if (Stage.IsValid() && Stage->GetStringField(TEXT("usageId")) == ExpectedUsageId)
			{
				const TSharedPtr<FJsonObject> Properties = Stage->GetObjectField(TEXT("properties"));
				return Stage->GetStringField(TEXT("name")) == (bImported ? TEXT("PersistenceStageImported") : TEXT("PersistenceStage"))
					&& Stage->GetBoolField(TEXT("enabled")) == !bImported
					&& Stage->GetStringField(TEXT("stageClass")).EndsWith(TEXT("NiagaraSimulationStageGeneric"))
					&& Stage->GetNumberField(TEXT("numIterations")) == (bImported ? 5.0 : 3.0)
					&& Properties.IsValid()
					&& (!bImported || Properties->GetBoolField(TEXT("bGpuDispatchForceLinear")));
			}
		}
	}
	return false;
}
} // namespace UEAINiagaraSpecPersistencePrivate

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAINiagaraSystemSpecCrossProcessPersistenceTest,
	"UE_AI_integration.Niagara.SystemSpec.CrossProcessPersistence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAINiagaraSystemSpecCrossProcessPersistenceTest::RunTest(const FString&)
{
	using namespace UEAINiagaraSpecPersistencePrivate;
	FString RunId, Phase, ArtifactPath;
	if (!ReadEnvironment(RunId, Phase, ArtifactPath))
	{
		AddInfo(TEXT("Opt-in only: use tests/hostproject/run-niagara-spec-persistence.ps1."));
		return true;
	}
	FGuid ParsedRun;
	if (!TestTrue(TEXT("Persistence run ID is a GUID"), FGuid::ParseExact(RunId, EGuidFormats::Digits, ParsedRun))) return false;
	FFixture Fixture;
	Fixture.RunId = RunId;
	if (Phase == TEXT("prepare"))
	{
		if (!TestTrue(TEXT("Native Niagara fixture creates"), CreateFixture(Fixture))) return false;
		FMCPToolRegistry Registry; Register(Registry);
		auto Params = MakeShared<FJsonObject>(); Params->SetStringField(TEXT("system"), Fixture.SystemPath);
		const FMCPToolResult Export = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), Params);
		if (!TestTrue(TEXT("Native system spec export succeeds"), Export.bSuccess) || !Export.Data) return false;
		TestTrue(TEXT("Export includes sprite renderer"), HasRendererClass(Export.Data, TEXT("NiagaraSpriteRendererProperties")));
		TestTrue(TEXT("Export includes mesh renderer"), HasRendererClass(Export.Data, TEXT("NiagaraMeshRendererProperties")));
		TestTrue(TEXT("Export includes ribbon renderer"), HasRendererClass(Export.Data, TEXT("NiagaraRibbonRendererProperties")));
		TestTrue(TEXT("Export includes authored SubUV property"), Export.Data->GetStringField(TEXT("specDigest")).Len() > 0);
		if (!Fixture.bDynamicInputCovered) AddInfo(TEXT("Dynamic input tree was not created: required Niagara module/dynamic-input assets were unavailable."));
		TestTrue(TEXT("Generated package saves to disk"), SaveSystem(Fixture.System));
		TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-spec-cross-process.v1"));
		Evidence->SetStringField(TEXT("runId"), RunId); Evidence->SetStringField(TEXT("phase"), Phase);
		Evidence->SetStringField(TEXT("systemPath"), Fixture.SystemPath);
		Evidence->SetStringField(TEXT("specPath"), ArtifactPath + TEXT(".spec.json"));
		// The usage IDs are generated per fixture and are the stable identity
		// used by the fresh-process read-back assertions. Persist them beside the
		// exported spec instead of reconstructing a new zero/temporary GUID after
		// the package crosses the process boundary.
		Evidence->SetStringField(TEXT("eventHandlerUsageId"), Fixture.EventHandlerUsageId.ToString(EGuidFormats::Digits));
		Evidence->SetStringField(TEXT("simulationStageUsageId"), Fixture.SimulationStageUsageId.ToString(EGuidFormats::Digits));
		Evidence->SetBoolField(TEXT("dynamicInputCovered"), Fixture.bDynamicInputCovered);
		Evidence->SetBoolField(TEXT("spriteCovered"), HasRendererClass(Export.Data, TEXT("NiagaraSpriteRendererProperties")));
		Evidence->SetBoolField(TEXT("meshCovered"), HasRendererClass(Export.Data, TEXT("NiagaraMeshRendererProperties")));
		Evidence->SetBoolField(TEXT("ribbonCovered"), HasRendererClass(Export.Data, TEXT("NiagaraRibbonRendererProperties")));
		Evidence->SetBoolField(TEXT("eventHandlerCovered"), false);
		Evidence->SetBoolField(TEXT("simulationStageCovered"), false);
		Evidence->SetStringField(TEXT("runtimeEvidence"), TEXT("not_run: this contract proves authored persistence only"));
		Evidence->SetStringField(TEXT("preparePid"), FString::FromInt(FPlatformProcess::GetCurrentProcessId()));
		SaveJsonFile(ArtifactPath, Evidence); SaveJsonFile(ArtifactPath + TEXT(".spec.json"), Export.Data);
		return true;
	}

	FAssetRegistryModule::GetRegistry().ScanPathsSynchronous({TEXT("/Game/Automation")}, true);
	Fixture.PackagePath = TEXT("/Game/Automation/UEAI_NiagaraSpecPersistence_") + RunId;
	Fixture.SystemPath = Fixture.PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(Fixture.PackagePath);
	Fixture.System = LoadObject<UNiagaraSystem>(nullptr, *Fixture.SystemPath);
	if (!TestNotNull(TEXT("Persisted Niagara system loads in a fresh Editor"), Fixture.System)) return false;
	FMCPToolRegistry Registry; Register(Registry);
	if (Phase == TEXT("import"))
	{
		TSharedPtr<FJsonObject> Spec = LoadJsonFile(ArtifactPath + TEXT(".spec.json"));
		if (!TestNotNull(TEXT("Exported spec artifact reloads"), Spec.Get())) return false;
		for (const TSharedPtr<FJsonValue>& Value : Spec->GetArrayField(TEXT("userParameters")))
		{
			const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Row.IsValid() && Row->GetStringField(TEXT("name")) == TEXT("User.SpecRoundTrip")) Row->SetNumberField(TEXT("default"), 42.0);
		}
		const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
		if (Spec->TryGetArrayField(TEXT("emitters"), Emitters) && Emitters && Emitters->Num() > 0)
		{
			const TSharedPtr<FJsonObject> Emitter = (*Emitters)[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* EventHandlers = nullptr;
			if (Emitter.IsValid() && Emitter->TryGetArrayField(TEXT("eventHandlers"), EventHandlers) && EventHandlers && EventHandlers->Num() > 0)
			{
				const TSharedPtr<FJsonObject> EventHandler = (*EventHandlers)[0]->AsObject();
				if (EventHandler.IsValid())
				{
					EventHandler->SetStringField(TEXT("sourceEventName"), TEXT("PersistenceBurstImported"));
					EventHandler->SetStringField(TEXT("sourceEmitterId"), TEXT(""));
					EventHandler->SetNumberField(TEXT("spawnNumber"), 7.0);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* SimulationStages = nullptr;
			if (Emitter.IsValid() && Emitter->TryGetArrayField(TEXT("simulationStages"), SimulationStages) && SimulationStages && SimulationStages->Num() > 0)
			{
				const TSharedPtr<FJsonObject> SimulationStage = (*SimulationStages)[0]->AsObject();
				if (SimulationStage.IsValid())
				{
					SimulationStage->SetStringField(TEXT("name"), TEXT("PersistenceStageImported"));
					SimulationStage->SetBoolField(TEXT("enabled"), false);
					SimulationStage->SetNumberField(TEXT("numIterations"), 5.0);
					SimulationStage->GetObjectField(TEXT("properties"))->SetBoolField(TEXT("bGpuDispatchForceLinear"), true);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
			if (Emitter.IsValid() && Emitter->TryGetArrayField(TEXT("renderers"), Renderers) && Renderers)
				for (const TSharedPtr<FJsonValue>& Value : *Renderers)
				{
					const TSharedPtr<FJsonObject> Renderer = Value.IsValid() ? Value->AsObject() : nullptr;
					if (Renderer.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraSpriteRendererProperties")))
					{
						const TSharedPtr<FJsonObject> Properties = Renderer->GetObjectField(TEXT("properties"));
						Properties->SetStringField(TEXT("bSubImageBlend"), TEXT("False"));
					}
					else if (Renderer.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraMeshRendererProperties")))
					{
						Renderer->GetObjectField(TEXT("properties"))->SetStringField(TEXT("bSubImageBlend"), TEXT("False"));
					}
					else if (Renderer.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraRibbonRendererProperties")))
					{
						Renderer->GetObjectField(TEXT("properties"))->SetStringField(TEXT("MaxNumRibbons"), TEXT("64"));
					}
				}
		}
		auto ImportParams = MakeShared<FJsonObject>(); ImportParams->SetStringField(TEXT("system"), Fixture.SystemPath);
		ImportParams->SetObjectField(TEXT("spec"), Spec); ImportParams->SetStringField(TEXT("requestId"), TEXT("niagara-cross-process-import")); ImportParams->SetBoolField(TEXT("confirmWrite"), true);
		const FMCPToolResult Import = Registry.ExecuteTool(TEXT("content.niagara.system.spec.import"), ImportParams);
		if (!TestTrue(TEXT("Native system spec import succeeds"), Import.bSuccess)) { AddError(Import.ErrorMessage); return false; }
		TestTrue(TEXT("Native system spec import reports authored read-back"), Import.Data.IsValid() && Import.Data->GetBoolField(TEXT("readbackVerified")));
		auto SaveParams = MakeShared<FJsonObject>(); SaveParams->SetStringField(TEXT("system"), Fixture.SystemPath);
		const FMCPToolResult Saved = Registry.ExecuteTool(TEXT("content.niagara.system.save"), SaveParams);
		TestTrue(TEXT("Native system save persists imported authored state"), Saved.bSuccess);
		TSharedPtr<FJsonObject> Evidence = LoadJsonFile(ArtifactPath); if (!Evidence.IsValid()) Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("phase"), Phase); Evidence->SetStringField(TEXT("importPid"), FString::FromInt(FPlatformProcess::GetCurrentProcessId())); Evidence->SetBoolField(TEXT("importSaved"), Saved.bSuccess); SaveJsonFile(ArtifactPath, Evidence);
		return !HasAnyErrors();
	}

	auto ExportParams = MakeShared<FJsonObject>(); ExportParams->SetStringField(TEXT("system"), Fixture.SystemPath);
	const FMCPToolResult Export = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), ExportParams);
	if (!TestTrue(TEXT("Post-reload native spec export succeeds"), Export.bSuccess) || !Export.Data) return false;
	TSharedPtr<FJsonObject> Evidence = LoadJsonFile(ArtifactPath);
	FGuid PersistedEventHandlerUsageId;
	FGuid PersistedSimulationStageUsageId;
	TestTrue(TEXT("Persisted Event Handler usage ID is valid"),
		Evidence.IsValid()
		&& FGuid::ParseExact(Evidence->GetStringField(TEXT("eventHandlerUsageId")), EGuidFormats::Digits, PersistedEventHandlerUsageId));
	TestTrue(TEXT("Persisted Simulation Stage usage ID is valid"),
		Evidence.IsValid()
		&& FGuid::ParseExact(Evidence->GetStringField(TEXT("simulationStageUsageId")), EGuidFormats::Digits, PersistedSimulationStageUsageId));
	if (Evidence.IsValid())
	{
		Fixture.EventHandlerUsageId = PersistedEventHandlerUsageId;
		Fixture.SimulationStageUsageId = PersistedSimulationStageUsageId;
	}
	TestTrue(TEXT("Dynamic input coverage was completed before persistence"), Evidence.IsValid() && Evidence->GetBoolField(TEXT("dynamicInputCovered")));
	TestTrue(TEXT("Event Handler authored state survived the process boundary"),
		Evidence.IsValid() && Evidence->GetBoolField(TEXT("eventHandlerCovered"))
		&& HasEventHandlerUsage(Export.Data, Fixture.EventHandlerUsageId, true));
	TestTrue(TEXT("Simulation Stage authored state survived the process boundary"),
		Evidence.IsValid() && Evidence->GetBoolField(TEXT("simulationStageCovered"))
		&& HasSimulationStageUsage(Export.Data, Fixture.SimulationStageUsageId, true));
	bool bPersistedUserDefault = false;
	for (const TSharedPtr<FJsonValue>& Value : Export.Data->GetArrayField(TEXT("userParameters")))
	{
		const TSharedPtr<FJsonObject> Row = Value.IsValid() ? Value->AsObject() : nullptr;
		if (Row.IsValid() && Row->GetStringField(TEXT("name")) == TEXT("User.SpecRoundTrip"))
		{
			bPersistedUserDefault = FMath::IsNearlyEqual(static_cast<float>(Row->GetNumberField(TEXT("default"))), 42.0f);
			break;
		}
	}
	TestTrue(TEXT("Imported user parameter default survived the process boundary"), bPersistedUserDefault);
	TestTrue(TEXT("Persisted sprite renderer is readable"), HasRendererClass(Export.Data, TEXT("NiagaraSpriteRendererProperties")));
	TestTrue(TEXT("Persisted mesh renderer is readable"), HasRendererClass(Export.Data, TEXT("NiagaraMeshRendererProperties")));
	TestTrue(TEXT("Persisted ribbon renderer is readable"), HasRendererClass(Export.Data, TEXT("NiagaraRibbonRendererProperties")));
	bool bPersistedSpriteSubUvEdit = false;
	bool bPersistedMeshSubUvEdit = false;
	bool bPersistedRibbonEdit = false;
	const TArray<TSharedPtr<FJsonValue>>* PersistedEmitters = nullptr;
	if (Export.Data->TryGetArrayField(TEXT("emitters"), PersistedEmitters) && PersistedEmitters)
	{
		for (const TSharedPtr<FJsonValue>& EmitterValue : *PersistedEmitters)
		{
			const TSharedPtr<FJsonObject> Emitter = EmitterValue.IsValid() ? EmitterValue->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
			if (!Emitter.IsValid() || !Emitter->TryGetArrayField(TEXT("renderers"), Renderers) || !Renderers) continue;
			for (const TSharedPtr<FJsonValue>& RendererValue : *Renderers)
			{
				const TSharedPtr<FJsonObject> Renderer = RendererValue.IsValid() ? RendererValue->AsObject() : nullptr;
				if (!Renderer.IsValid() || !Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraSpriteRendererProperties"))) continue;
				const TSharedPtr<FJsonObject> Properties = Renderer->GetObjectField(TEXT("properties"));
				if (Properties.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraSpriteRendererProperties")))
					bPersistedSpriteSubUvEdit = Properties->GetStringField(TEXT("bSubImageBlend")).Equals(TEXT("False"), ESearchCase::IgnoreCase);
				if (Properties.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraMeshRendererProperties")))
					bPersistedMeshSubUvEdit = Properties->GetStringField(TEXT("bSubImageBlend")).Equals(TEXT("False"), ESearchCase::IgnoreCase);
				if (Properties.IsValid() && Renderer->GetStringField(TEXT("rendererClass")).EndsWith(TEXT("NiagaraRibbonRendererProperties")))
					bPersistedRibbonEdit = Properties->GetStringField(TEXT("MaxNumRibbons")).Equals(TEXT("64"), ESearchCase::IgnoreCase);
			}
		}
	}
	TestTrue(TEXT("Imported sprite SubUV property survived the process boundary"), bPersistedSpriteSubUvEdit);
	TestTrue(TEXT("Imported mesh SubUV property survived the process boundary"), bPersistedMeshSubUvEdit);
	TestTrue(TEXT("Imported ribbon property survived the process boundary"), bPersistedRibbonEdit);
	TestTrue(TEXT("Persisted dynamic input tree is readable"), FindDynamicTree(MakeShared<FJsonValueObject>(Export.Data)));
	const TSharedPtr<FJsonObject> RoundTripSpec = CloneJson(Export.Data);
	auto RoundTripParams = MakeShared<FJsonObject>(); RoundTripParams->SetStringField(TEXT("system"), Fixture.SystemPath); RoundTripParams->SetObjectField(TEXT("spec"), RoundTripSpec);
	const FMCPToolResult RoundTrip = Registry.ExecuteTool(TEXT("content.niagara.system.spec.round_trip"), RoundTripParams);
	TestTrue(TEXT("Post-reload native round-trip verifies"), RoundTrip.bSuccess && RoundTrip.Data && RoundTrip.Data->GetBoolField(TEXT("roundTripVerified")));
	TestTrue(TEXT("Isolated package is deleted after verification"), !UEditorAssetLibrary::DoesAssetExist(Fixture.SystemPath) || UEditorAssetLibrary::DeleteAsset(Fixture.SystemPath));
	Evidence->SetStringField(TEXT("phase"), Phase); Evidence->SetStringField(TEXT("verifyPid"), FString::FromInt(FPlatformProcess::GetCurrentProcessId())); Evidence->SetBoolField(TEXT("verifyRoundTrip"), RoundTrip.bSuccess); SaveJsonFile(ArtifactPath, Evidence);
	return !HasAnyErrors();
}

#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#endif // WITH_DEV_AUTOMATION_TESTS
