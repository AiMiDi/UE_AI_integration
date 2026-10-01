// Contract tests for content.niagara.system.spec.export.
//
// The export is a read-only authored-system snapshot; imports may compile
// changed authored graphs, but tests never save or assume runtime/PIE. The unknown-system path is
// asserted directly; the valid-system path builds a transient /Game/ system +
// owned emitter/renderer fixture (mirroring NiagaraModuleStackContractTests.cpp)
// and verifies the schema envelope plus changed-default and renderer-property
// round trips, skipping with AddInfo when the fixture cannot build.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EdGraph/EdGraphPin.h"
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterStore.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#endif

namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphModuleTools(FMCPToolRegistry& Registry);
}

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
namespace
{
struct FNiagaraSystemSpecExportFixture
{
	FString PackageName;
	UPackage* Package = nullptr;
	UNiagaraSystem* System = nullptr;
	FNiagaraVariable UserFloat;
	UNiagaraSpriteRendererProperties* SpriteRenderer = nullptr;
	UNiagaraGraph* Graph = nullptr;
};

struct FNiagaraSystemSpecEventStackFixture
{
	FGuid UsageId;
	UNiagaraScript* Script = nullptr;
	UNiagaraNodeOutput* Output = nullptr;
	UNiagaraNodeFunctionCall* Module = nullptr;
};

TSharedPtr<FJsonObject> NiagaraSystemSpecExportCloneSpec(const TSharedPtr<FJsonObject>& Spec)
{
	if (!Spec.IsValid())
	{
		return nullptr;
	}
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Spec.ToSharedRef(), Writer))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Clone;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Serialized);
	return FJsonSerializer::Deserialize(Reader, Clone) ? Clone : nullptr;
}

TSharedPtr<FJsonObject> NiagaraSystemSpecExportFirstObject(
	const TSharedPtr<FJsonObject>& Owner, const TCHAR* ArrayField)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	return Owner.IsValid() && Owner->TryGetArrayField(ArrayField, Rows) && Rows
		&& Rows->Num() > 0 && (*Rows)[0].IsValid() && (*Rows)[0]->Type == EJson::Object
		? (*Rows)[0]->AsObject() : nullptr;
}

bool NiagaraSystemSpecExportCreateFixture(FNiagaraSystemSpecExportFixture& OutFixture)
{
	OutFixture.PackageName = TEXT("/Game/Automation/UEAI_SystemSpecExport_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString AssetName =
		FPackageName::GetLongPackageAssetName(OutFixture.PackageName);
	OutFixture.Package = CreatePackage(*OutFixture.PackageName);
	OutFixture.System = OutFixture.Package
		? NewObject<UNiagaraSystem>(
			OutFixture.Package,
			*AssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!OutFixture.System)
	{
		return false;
	}
	// Build the system graph without loading the optional RequiredSystemUpdate
	// module from engine content.  The isolated HostProject may not mount that
	// asset; create the two parameter-map outputs directly so renderer-only spec
	// imports still have a valid system graph to compile.
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, false);
	UNiagaraScript* SystemSpawnScript = OutFixture.System->GetSystemSpawnScript();
	UNiagaraScript* SystemUpdateScript = OutFixture.System->GetSystemUpdateScript();
	UNiagaraScriptSource* SystemSource = SystemSpawnScript
		? Cast<UNiagaraScriptSource>(SystemSpawnScript->GetLatestSource())
		: nullptr;
	if (!SystemSpawnScript || !SystemUpdateScript || !SystemSource || !SystemSource->NodeGraph
		|| !FNiagaraStackGraphUtilities::ResetGraphForOutput(
			*SystemSource->NodeGraph,
			ENiagaraScriptUsage::SystemSpawnScript,
			SystemSpawnScript->GetUsageId())
		|| !FNiagaraStackGraphUtilities::ResetGraphForOutput(
			*SystemSource->NodeGraph,
			ENiagaraScriptUsage::SystemUpdateScript,
			SystemUpdateScript->GetUsageId()))
	{
		return false;
	}
	// Keep one real authored user parameter in the fixture.  This makes the
	// import contract exercise a changed default and a post-import export,
	// rather than only accepting an untouched empty spec.
	OutFixture.UserFloat = FNiagaraVariable(
		FNiagaraTypeDefinition::GetFloatDef(),
		TEXT("User.SpecRoundTrip"));
	if (!OutFixture.System->GetExposedParameters().AddParameter(OutFixture.UserFloat)
		|| !OutFixture.System->GetExposedParameters().SetParameterValue<float>(
			1.0f, OutFixture.UserFloat, false))
	{
		return false;
	}

	// An "empty" emitter: ResetGraphForOutput builds the four output nodes and
	// their input-connector stack sources, but no function-call modules.
	UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(
		OutFixture.System, TEXT("EmptyEmitter"), RF_Transactional);
	if (!Emitter)
	{
		return false;
	}
	UNiagaraEmitterFactoryNew::InitializeEmitter(Emitter, false);
	OutFixture.SpriteRenderer = NewObject<UNiagaraSpriteRendererProperties>(
		Emitter, TEXT("SpecSpriteRenderer"), RF_Transactional);
	if (!OutFixture.SpriteRenderer)
	{
		return false;
	}
	Emitter->AddRenderer(
		OutFixture.SpriteRenderer,
		Emitter->GetExposedVersion().VersionGuid);
	FNiagaraEmitterHandle Handle(*Emitter, Emitter->GetExposedVersion().VersionGuid);
	OutFixture.System->AddEmitterHandleDirect(Handle);
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	UNiagaraScriptSource* Source = EmitterData ? Cast<UNiagaraScriptSource>(EmitterData->GraphSource) : nullptr;
	OutFixture.Graph = Source ? Source->NodeGraph : nullptr;
	OutFixture.Package->SetDirtyFlag(false);
	return OutFixture.Graph != nullptr;
}

bool NiagaraSystemSpecExportDeleteFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
}

bool NiagaraSystemSpecExportAddEventStack(
	const FNiagaraSystemSpecExportFixture& Fixture,
	UNiagaraScript* ModuleScript,
	const int32 Index,
	FNiagaraSystemSpecEventStackFixture& OutStack)
{
	if (!Fixture.System || !Fixture.Graph || !ModuleScript
		|| Fixture.System->GetEmitterHandles().Num() != 1)
	{
		return false;
	}
	const FNiagaraEmitterHandle& Handle = Fixture.System->GetEmitterHandles()[0];
	const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
	FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
	UNiagaraScriptSource* Source = Data ? Cast<UNiagaraScriptSource>(Data->GraphSource) : nullptr;
	if (!Instance.Emitter || !Source || Source->NodeGraph != Fixture.Graph)
	{
		return false;
	}

	OutStack.UsageId = FGuid::NewGuid();
	FNiagaraEventScriptProperties Properties;
	Properties.SourceEventName = FName(*FString::Printf(TEXT("SpecEvent%d"), Index));
	Properties.Script = NewObject<UNiagaraScript>(Instance.Emitter,
		*FString::Printf(TEXT("SpecEventScript%d"), Index), RF_Transactional);
	if (!Properties.Script)
	{
		return false;
	}
	Properties.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	Properties.Script->SetUsageId(OutStack.UsageId);
	Properties.Script->SetLatestSource(Source);
	Instance.Emitter->AddEventHandler(Properties, Instance.Version);
	OutStack.Script = Properties.Script;

	FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Fixture.Graph);
	OutStack.Output = OutputCreator.CreateNode();
	OutStack.Output->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
	OutStack.Output->SetUsageId(OutStack.UsageId);
	OutStack.Output->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Out")));
	OutputCreator.Finalize();
	FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Fixture.Graph);
	UNiagaraNodeInput* Input = InputCreator.CreateNode();
	Input->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
	Input->Usage = ENiagaraInputNodeUsage::Parameter;
	InputCreator.Finalize();
	UEdGraphPin* OutputInput = OutStack.Output->GetInputPin(0);
	UEdGraphPin* InputOutput = Input->GetOutputPin(0);
	if (!OutputInput || !InputOutput)
	{
		return false;
	}
	OutputInput->MakeLinkTo(InputOutput);
	OutStack.Module = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
		ModuleScript, *OutStack.Output, INDEX_NONE, FString::Printf(TEXT("SpecEventModule%d"), Index));
	Fixture.Graph->NotifyGraphChanged();
	return OutStack.Module != nullptr;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSpecExportUnknownSystemTest,
	"UE_AI_integration.Niagara.SystemSpecExport.UnknownSystem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSpecExportUnknownSystemTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(
		TEXT("system"),
		TEXT("/Game/Automation/UEAI_SystemSpecExport_Missing.UEAI_SystemSpecExport_Missing"));
	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.export"),
		Params);
	TestFalse(TEXT("Unknown system export is rejected"), Result.bSuccess);
	TestEqual(
		TEXT("Unknown system uses system_not_found"),
		Result.ErrorCode,
		FString(TEXT("system_not_found")));
	TestEqual(
		TEXT("Unknown system maps to HTTP 404"),
		Result.HttpStatus,
		404);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSpecExportContractTest,
	"UE_AI_integration.Niagara.SystemSpecExport.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSpecExportContractTest::RunTest(const FString&)
{
	FNiagaraSystemSpecExportFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(
			TEXT("System-spec-export fixture and package are deleted"),
			NiagaraSystemSpecExportDeleteFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("System-spec-export fixture builds"), NiagaraSystemSpecExportCreateFixture(Fixture))
		|| !TestNotNull(TEXT("System-spec-export system"), Fixture.System))
	{
		AddInfo(TEXT("The /Game/ system + empty emitter fixture could not be built; skipping the spec-export contract."));
		return true;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	const FMCPToolResult Result = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.export"),
		Params);
	if (!TestTrue(TEXT("System spec export succeeds"), Result.bSuccess) || !Result.Data)
	{
		return false;
	}

	TestEqual(
		TEXT("Spec schema is the system-spec envelope"),
		Result.Data->GetStringField(TEXT("schema")),
		FString(TEXT("ue.niagara.system-spec.v1")));
	TestEqual(
		TEXT("Spec version is 1"),
		Result.Data->GetIntegerField(TEXT("specVersion")),
		1);

	const TArray<TSharedPtr<FJsonValue>>* Emitters = nullptr;
	TestTrue(
		TEXT("Spec exposes a non-empty emitters array"),
		Result.Data->TryGetArrayField(TEXT("emitters"), Emitters)
			&& Emitters != nullptr
			&& Emitters->Num() >= 1);

	const TArray<TSharedPtr<FJsonValue>>* UserParameters = nullptr;
	TestTrue(
		TEXT("Spec exposes a userParameters array"),
		Result.Data->TryGetArrayField(TEXT("userParameters"), UserParameters)
			&& UserParameters != nullptr);

	TestFalse(TEXT("Read-only export never reports saved"), Result.Data->GetBoolField(TEXT("saved")));
	TestFalse(TEXT("Read-only export never reports compiled"), Result.Data->GetBoolField(TEXT("compiled")));
	TestTrue(TEXT("Export reports bounded output"), Result.Data->GetBoolField(TEXT("bounded")));
	TestEqual(TEXT("Spec publishes a global dynamic-input node limit"),
		Result.Data->GetIntegerField(TEXT("dynamicInputNodesLimit")), 512);
	TestEqual(TEXT("Empty fixture consumes no dynamic-input node budget"),
		Result.Data->GetIntegerField(TEXT("dynamicInputNodesWritten")), 0);
	TestFalse(TEXT("Empty fixture has no truncated dynamic-input tree"),
		Result.Data->GetBoolField(TEXT("dynamicInputsTruncated")));
	TestTrue(TEXT("Export includes a canonical spec digest"),
		!Result.Data->GetStringField(TEXT("specDigest")).IsEmpty());

	const FString OriginalSpecDigest = Result.Data->GetStringField(TEXT("specDigest"));

	// Each rejected snapshot also requests an observable authored edit. A
	// successful transport response or transaction rollback is insufficient:
	// invalid input must be rejected before it touches the parameter, renderer,
	// graph change ID, or package dirty state.
	auto VerifyPreflightSnapshotRejected = [&](const TCHAR* CaseName,
		const TFunction<void(const TSharedPtr<FJsonObject>&)>& MakeInvalid,
		const TCHAR* ExpectedErrorCode = TEXT("spec_invalid"))
	{
		const TSharedPtr<FJsonObject> Snapshot = NiagaraSystemSpecExportCloneSpec(Result.Data);
		if (!TestNotNull(FString::Printf(TEXT("%s snapshot clones"), CaseName), Snapshot.Get()))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& ParameterValue : Snapshot->GetArrayField(TEXT("userParameters")))
		{
			const TSharedPtr<FJsonObject> Parameter = ParameterValue.IsValid() ? ParameterValue->AsObject() : nullptr;
			if (Parameter.IsValid() && Parameter->GetStringField(TEXT("name")) == TEXT("User.SpecRoundTrip"))
			{
				Parameter->SetNumberField(TEXT("default"), 99.0);
			}
		}
		const TSharedPtr<FJsonObject> EmitterSpec = NiagaraSystemSpecExportFirstObject(Snapshot, TEXT("emitters"));
		const TSharedPtr<FJsonObject> RendererSpec = NiagaraSystemSpecExportFirstObject(EmitterSpec, TEXT("renderers"));
		const TSharedPtr<FJsonObject>* RendererProperties = nullptr;
		if (!TestTrue(TEXT("Rejected snapshot includes the fixture renderer properties"),
			RendererSpec.IsValid() && RendererSpec->TryGetObjectField(TEXT("properties"), RendererProperties)
			&& RendererProperties && RendererProperties->IsValid()))
		{
			return;
		}
		(*RendererProperties)->SetStringField(TEXT("bSubImageBlend"), Fixture.SpriteRenderer->bSubImageBlend ? TEXT("False") : TEXT("True"));
		MakeInvalid(Snapshot);
		const float BeforeDefault = Fixture.System->GetExposedParameters().GetParameterValue<float>(Fixture.UserFloat);
		const bool bBeforeBlend = Fixture.SpriteRenderer->bSubImageBlend;
		const bool bBeforeDirty = Fixture.Package->IsDirty();
		const FGuid BeforeChangeId = Fixture.Graph->GetChangeID();
		const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
		auto RejectedImportParams = MakeShared<FJsonObject>();
		RejectedImportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		RejectedImportParams->SetObjectField(TEXT("spec"), Snapshot);
		RejectedImportParams->SetStringField(TEXT("requestId"), FString(TEXT("spec-truncated-")) + CaseName);
		RejectedImportParams->SetBoolField(TEXT("confirmWrite"), true);
		const FMCPToolResult Rejected = Registry.ExecuteTool(
			TEXT("content.niagara.system.spec.import"), RejectedImportParams);
		TestFalse(FString::Printf(TEXT("%s import is rejected"), CaseName), Rejected.bSuccess);
		TestEqual(FString::Printf(TEXT("%s uses the expected preflight error"), CaseName), Rejected.ErrorCode, FString(ExpectedErrorCode));
		TestEqual(FString::Printf(TEXT("%s uses HTTP 422"), CaseName), Rejected.HttpStatus, 422);
		TestEqual(FString::Printf(TEXT("%s preserves the user default"), CaseName),
			Fixture.System->GetExposedParameters().GetParameterValue<float>(Fixture.UserFloat), BeforeDefault);
		TestEqual(FString::Printf(TEXT("%s preserves the renderer"), CaseName), static_cast<bool>(Fixture.SpriteRenderer->bSubImageBlend), bBeforeBlend);
		TestEqual(FString::Printf(TEXT("%s preserves package dirty state"), CaseName), Fixture.Package->IsDirty(), bBeforeDirty);
		TestEqual(FString::Printf(TEXT("%s preserves graph change ID"), CaseName), Fixture.Graph->GetChangeID(), BeforeChangeId);
		TestEqual(FString::Printf(TEXT("%s preserves graph node count"), CaseName), Fixture.Graph->Nodes.Num(), BeforeNodeCount);
	};
	VerifyPreflightSnapshotRejected(TEXT("global-dynamic-inputs"), [](const TSharedPtr<FJsonObject>& Snapshot)
	{
		Snapshot->SetBoolField(TEXT("dynamicInputsTruncated"), true);
	});
	VerifyPreflightSnapshotRejected(TEXT("emitter-renderers"), [](const TSharedPtr<FJsonObject>& Snapshot)
	{
		NiagaraSystemSpecExportFirstObject(Snapshot, TEXT("emitters"))->SetBoolField(TEXT("renderersTruncated"), true);
	});
	VerifyPreflightSnapshotRejected(TEXT("renderer-properties"), [](const TSharedPtr<FJsonObject>& Snapshot)
	{
		const TSharedPtr<FJsonObject> Emitter = NiagaraSystemSpecExportFirstObject(Snapshot, TEXT("emitters"));
		NiagaraSystemSpecExportFirstObject(Emitter, TEXT("renderers"))->SetBoolField(TEXT("propertiesTruncated"), true);
	});
	VerifyPreflightSnapshotRejected(TEXT("renderer-value"), [](const TSharedPtr<FJsonObject>& Snapshot)
	{
		const TSharedPtr<FJsonObject> Emitter = NiagaraSystemSpecExportFirstObject(Snapshot, TEXT("emitters"));
		NiagaraSystemSpecExportFirstObject(Emitter, TEXT("renderers"))->SetBoolField(TEXT("valueTruncated"), true);
	});
	VerifyPreflightSnapshotRejected(TEXT("renderer-text-trailing-data"), [](const TSharedPtr<FJsonObject>& Snapshot)
	{
		const TSharedPtr<FJsonObject> Emitter = NiagaraSystemSpecExportFirstObject(Snapshot, TEXT("emitters"));
		const TSharedPtr<FJsonObject> Renderer = NiagaraSystemSpecExportFirstObject(Emitter, TEXT("renderers"));
		const TSharedPtr<FJsonObject> Properties = Renderer->GetObjectField(TEXT("properties"));
		// The prefix is the requested changed value and is valid on its own.
		// A parser that runs directly on the asset can apply that prefix before
		// discovering the suffix. Preflight must reject before the pending user
		// parameter or this renderer property is touched.
		Properties->SetStringField(TEXT("bSubImageBlend"),
			Properties->GetStringField(TEXT("bSubImageBlend")) + TEXT(" UEAI_InvalidTrailingText"));
	}, TEXT("renderer_property_invalid"));

	// Feed the untouched export back through both verification surfaces.  The
	// round-trip digest must exclude the transport-only specDigest field; this
	// catches the common error where a valid export can never verify itself.
	auto RoundTripParams = MakeShared<FJsonObject>();
	RoundTripParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	RoundTripParams->SetObjectField(TEXT("spec"), Result.Data);
	const FMCPToolResult RoundTrip = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.round_trip"), RoundTripParams);
	if (TestTrue(TEXT("Untouched system spec round-trip succeeds"), RoundTrip.bSuccess)
		&& RoundTrip.Data)
	{
		TestTrue(TEXT("Round-trip structure matches"), RoundTrip.Data->GetBoolField(TEXT("structureMatch")));
		TestTrue(TEXT("Round-trip digest verifies"), RoundTrip.Data->GetBoolField(TEXT("roundTripVerified")));
		TestTrue(TEXT("Round-trip readback is explicit"), RoundTrip.Data->GetBoolField(TEXT("readbackVerified")));
	}

	// Change one exported authored default before importing.  The structure
	// digest deliberately excludes values, so the importer must accept this
	// change and write it back to the live parameter store.
	const TArray<TSharedPtr<FJsonValue>>& ExportedParameters =
		Result.Data->GetArrayField(TEXT("userParameters"));
	TSharedPtr<FJsonObject> ChangedParameter;
	for (const TSharedPtr<FJsonValue>& Value : ExportedParameters)
	{
		const TSharedPtr<FJsonObject> Candidate =
			Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
		if (Candidate.IsValid()
			&& Candidate->GetStringField(TEXT("name")) == TEXT("User.SpecRoundTrip"))
		{
			ChangedParameter = Candidate;
			break;
		}
	}
	if (TestNotNull(TEXT("Export contains the authored round-trip parameter"), ChangedParameter.Get()))
	{
		ChangedParameter->SetNumberField(TEXT("default"), 42.0);
	}
	// Renderer properties are part of the same spec transaction.  Flip the
	// sprite SubUV blend flag through exported text and verify that it survives
	// import/export as well.
	bool bChangedRendererSpec = false;
	FString ExpectedRendererBlend;
	const TArray<TSharedPtr<FJsonValue>>& ExportedEmitters =
		Result.Data->GetArrayField(TEXT("emitters"));
	if (ExportedEmitters.Num() > 0 && ExportedEmitters[0].IsValid())
	{
		const TSharedPtr<FJsonObject> EmitterSpec = ExportedEmitters[0]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Renderers = nullptr;
		if (EmitterSpec.IsValid()
			&& EmitterSpec->TryGetArrayField(TEXT("renderers"), Renderers)
			&& Renderers && Renderers->Num() > 0 && (*Renderers)[0].IsValid())
		{
			const TSharedPtr<FJsonObject> RendererSpec = (*Renderers)[0]->AsObject();
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (RendererSpec.IsValid()
				&& RendererSpec->TryGetObjectField(TEXT("properties"), Properties)
				&& Properties && Properties->IsValid()
				&& (*Properties)->HasField(TEXT("bSubImageBlend")))
			{
				FString ExistingRendererBlend;
				(*Properties)->TryGetStringField(TEXT("bSubImageBlend"), ExistingRendererBlend);
				ExpectedRendererBlend = ExistingRendererBlend.Equals(TEXT("True"), ESearchCase::IgnoreCase)
					? TEXT("False")
					: TEXT("True");
				(*Properties)->SetStringField(TEXT("bSubImageBlend"), ExpectedRendererBlend);
				bChangedRendererSpec = true;
			}
		}
	}
	TestTrue(TEXT("Export contains a mutable sprite SubUV renderer property"), bChangedRendererSpec);

	auto ImportParams = MakeShared<FJsonObject>();
	ImportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	ImportParams->SetObjectField(TEXT("spec"), Result.Data);
	ImportParams->SetStringField(TEXT("requestId"), TEXT("system-spec-import-contract"));
	ImportParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult Import = Registry.ExecuteTool(
		TEXT("content.niagara.system.spec.import"), ImportParams);
	if (!Import.bSuccess)
	{
		AddError(FString::Printf(
			TEXT("Changed system spec import failed: code=%s message=%s"),
			*Import.ErrorCode,
			*Import.ErrorMessage));
	}
	if (TestTrue(TEXT("Changed system spec import succeeds"), Import.bSuccess)
		&& Import.Data)
	{
		TestEqual(TEXT("Changed spec applies one user parameter"),
			Import.Data->GetIntegerField(TEXT("appliedUserParameters")), 1);
		TestFalse(TEXT("Spec import never claims a save"), Import.Data->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("Spec import verifies readback"), Import.Data->GetBoolField(TEXT("readbackVerified")));
		TestTrue(TEXT("Spec import reports a changed authored state"),
			Import.Data->GetBoolField(TEXT("changed")));
		TestEqual(TEXT("Spec import applies one renderer edit"),
			Import.Data->GetIntegerField(TEXT("appliedRendererProperties")),
			bChangedRendererSpec ? 1 : 0);

		auto VerifyExportParams = MakeShared<FJsonObject>();
		VerifyExportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		const FMCPToolResult AfterImport = Registry.ExecuteTool(
			TEXT("content.niagara.system.spec.export"), VerifyExportParams);
		if (TestTrue(TEXT("Export after spec import succeeds"), AfterImport.bSuccess)
			&& AfterImport.Data)
		{
			TestNotEqual(TEXT("Spec digest changes after importing the new default"),
				AfterImport.Data->GetStringField(TEXT("specDigest")), OriginalSpecDigest);
			const TArray<TSharedPtr<FJsonValue>>& AfterParameters =
				AfterImport.Data->GetArrayField(TEXT("userParameters"));
			bool bFoundChangedDefault = false;
			for (const TSharedPtr<FJsonValue>& Value : AfterParameters)
			{
				const TSharedPtr<FJsonObject> Candidate =
					Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				if (Candidate.IsValid()
					&& Candidate->GetStringField(TEXT("name")) == TEXT("User.SpecRoundTrip"))
				{
					bFoundChangedDefault = FMath::IsNearlyEqual(
						static_cast<float>(Candidate->GetNumberField(TEXT("default"))), 42.0f);
					break;
				}
			}
			TestTrue(TEXT("Post-import export reads back the changed default"), bFoundChangedDefault);
			if (bChangedRendererSpec && AfterImport.Data)
			{
				bool bFoundChangedRenderer = false;
				const TArray<TSharedPtr<FJsonValue>>& AfterEmitters =
					AfterImport.Data->GetArrayField(TEXT("emitters"));
				if (AfterEmitters.Num() > 0 && AfterEmitters[0].IsValid())
				{
					const TSharedPtr<FJsonObject> AfterEmitter = AfterEmitters[0]->AsObject();
					const TArray<TSharedPtr<FJsonValue>>* AfterRenderers = nullptr;
					if (AfterEmitter.IsValid()
						&& AfterEmitter->TryGetArrayField(TEXT("renderers"), AfterRenderers)
						&& AfterRenderers && AfterRenderers->Num() > 0
						&& (*AfterRenderers)[0].IsValid())
					{
						const TSharedPtr<FJsonObject> AfterRenderer = (*AfterRenderers)[0]->AsObject();
						const TSharedPtr<FJsonObject>* AfterProperties = nullptr;
						if (AfterRenderer.IsValid()
							&& AfterRenderer->TryGetObjectField(TEXT("properties"), AfterProperties)
							&& AfterProperties && AfterProperties->IsValid())
						{
							FString BlendValue;
							bFoundChangedRenderer = (*AfterProperties)->TryGetStringField(
								TEXT("bSubImageBlend"), BlendValue)
								&& BlendValue.Equals(ExpectedRendererBlend, ESearchCase::IgnoreCase);
						}
					}
				}
				TestTrue(TEXT("Post-import export reads back the renderer change"), bFoundChangedRenderer);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSpecRepeatedUsageRoundTripTest,
	"UE_AI_integration.Niagara.SystemSpecExport.RepeatedUsageRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSpecRepeatedUsageRoundTripTest::RunTest(const FString&)
{
	UNiagaraScript* ModuleScript = LoadObject<UNiagaraScript>(nullptr,
		TEXT("/Niagara/Modules/Update/Lifetime/UpdateAge.UpdateAge"));
	if (!ModuleScript || ModuleScript->GetUsage() != ENiagaraScriptUsage::Module)
	{
		AddInfo(TEXT("The engine UpdateAge module is unavailable; skipping repeated-usage spec round-trip."));
		return true;
	}
	FNiagaraSystemSpecExportFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(TEXT("Repeated-usage spec fixture and package are deleted"),
			NiagaraSystemSpecExportDeleteFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Repeated-usage system fixture builds"), NiagaraSystemSpecExportCreateFixture(Fixture)))
	{
		return false;
	}
	FNiagaraSystemSpecEventStackFixture EventStacks[2];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(EventStacks); ++Index)
	{
		if (!TestTrue(FString::Printf(TEXT("Event stack %d has a real module"), Index),
			NiagaraSystemSpecExportAddEventStack(Fixture, ModuleScript, Index, EventStacks[Index])))
		{
			return false;
		}
	}
	TestNotEqual(TEXT("The two event stacks have different usage IDs"), EventStacks[0].UsageId, EventStacks[1].UsageId);
	Fixture.Package->SetDirtyFlag(false);
	const FGuid BeforeChangeId = Fixture.Graph->GetChangeID();
	const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();
	auto ExportParams = MakeShared<FJsonObject>();
	ExportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	const FMCPToolResult Export = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), ExportParams);
	if (!TestTrue(TEXT("Repeated-usage spec exports"), Export.bSuccess) || !Export.Data)
	{
		return false;
	}
	const TSharedPtr<FJsonObject> EmitterSpec = NiagaraSystemSpecExportFirstObject(Export.Data, TEXT("emitters"));
	const TArray<TSharedPtr<FJsonValue>>* Stacks = nullptr;
	if (!TestTrue(TEXT("Repeated-usage emitter exports stacks"),
		EmitterSpec.IsValid() && EmitterSpec->TryGetArrayField(TEXT("stacks"), Stacks) && Stacks))
	{
		return false;
	}
	int32 EventStackCount = 0;
	bool bFoundEventStacks[2] = {false, false};
	for (const TSharedPtr<FJsonValue>& StackValue : *Stacks)
	{
		const TSharedPtr<FJsonObject> Stack = StackValue.IsValid() && StackValue->Type == EJson::Object
			? StackValue->AsObject() : nullptr;
		if (!Stack.IsValid() || Stack->GetStringField(TEXT("stackName")) != TEXT("particleEvent"))
		{
			continue;
		}
		++EventStackCount;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(EventStacks); ++Index)
		{
			const FNiagaraSystemSpecEventStackFixture& Expected = EventStacks[Index];
			if (Stack->GetStringField(TEXT("usageId")) != Expected.UsageId.ToString(EGuidFormats::DigitsWithHyphensLower))
			{
				continue;
			}
			bFoundEventStacks[Index] = true;
			TestEqual(TEXT("Event stack exports its script identity"), Stack->GetStringField(TEXT("scriptPath")), Expected.Script->GetPathName());
			TestEqual(TEXT("Event stack exports its output GUID"), Stack->GetStringField(TEXT("outputNodeGuid")),
				Expected.Output->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			TestEqual(TEXT("Event stack exports its output path"), Stack->GetStringField(TEXT("outputNodePath")), Expected.Output->GetPathName());
			const TArray<TSharedPtr<FJsonValue>>* Modules = nullptr;
			if (!TestTrue(TEXT("Event stack exports exactly one authored module"),
				Stack->TryGetArrayField(TEXT("modules"), Modules) && Modules && Modules->Num() == 1))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Module = (*Modules)[0]->AsObject();
			TestEqual(TEXT("Event stack module identity is specific to its output"), Module->GetStringField(TEXT("nodeGuid")),
				Expected.Module->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			// Nonempty inputs force import to resolve this module against this
			// output. An empty row would miss the old first-same-usage selection bug.
			if (!TestTrue(TEXT("Event module exports inputs that require exact stack resolution"),
				Module->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs && Inputs->Num() > 0))
			{
				return false;
			}
		}
	}
	if (!TestEqual(TEXT("Spec keeps both same-name event stacks"), EventStackCount, 2)
		|| !TestTrue(TEXT("Spec contains the first event stack identity"), bFoundEventStacks[0])
		|| !TestTrue(TEXT("Spec contains the second event stack identity"), bFoundEventStacks[1]))
	{
		return false;
	}

	auto ImportParams = MakeShared<FJsonObject>();
	ImportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	ImportParams->SetObjectField(TEXT("spec"), Export.Data);
	ImportParams->SetStringField(TEXT("requestId"), TEXT("system-spec-repeated-usage"));
	ImportParams->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult Imported = Registry.ExecuteTool(TEXT("content.niagara.system.spec.import"), ImportParams);
	if (!Imported.bSuccess)
	{
		AddError(FString::Printf(TEXT("Repeated-usage spec import failed: code=%s message=%s"),
			*Imported.ErrorCode, *Imported.ErrorMessage));
	}
	if (!TestTrue(TEXT("Repeated-usage spec imports without choosing the first event stack twice"), Imported.bSuccess)
		|| !Imported.Data)
	{
		return false;
	}
	TestTrue(TEXT("Repeated-usage import verifies authored readback"), Imported.Data->GetBoolField(TEXT("readbackVerified")));
	TestEqual(TEXT("Untouched event module inputs require no edits"), Imported.Data->GetIntegerField(TEXT("appliedModuleInputs")), 0);
	TestEqual(TEXT("Repeated-usage import preserves graph change ID"), Fixture.Graph->GetChangeID(), BeforeChangeId);
	TestEqual(TEXT("Repeated-usage import preserves graph node count"), Fixture.Graph->Nodes.Num(), BeforeNodeCount);
	for (const FNiagaraSystemSpecEventStackFixture& EventStack : EventStacks)
	{
		TestTrue(TEXT("Repeated-usage import preserves each live module identity"), Fixture.Graph->Nodes.Contains(EventStack.Module));
		TestTrue(TEXT("Repeated-usage import preserves each live output identity"), Fixture.Graph->Nodes.Contains(EventStack.Output));
	}
	const FMCPToolResult AfterImport = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), ExportParams);
	if (!TestTrue(TEXT("Repeated-usage spec re-exports"), AfterImport.bSuccess) || !AfterImport.Data)
	{
		return false;
	}
	TestEqual(TEXT("Repeated-usage authored content survives export/import/export"),
		AfterImport.Data->GetStringField(TEXT("specDigest")), Export.Data->GetStringField(TEXT("specDigest")));
	auto RoundTripParams = MakeShared<FJsonObject>();
	RoundTripParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	RoundTripParams->SetObjectField(TEXT("spec"), AfterImport.Data);
	const FMCPToolResult RoundTrip = Registry.ExecuteTool(TEXT("content.niagara.system.spec.round_trip"), RoundTripParams);
	if (TestTrue(TEXT("Repeated-usage spec round-trip verifies"), RoundTrip.bSuccess) && RoundTrip.Data)
	{
		TestTrue(TEXT("Repeated-usage structure remains exact"), RoundTrip.Data->GetBoolField(TEXT("structureMatch")));
		TestTrue(TEXT("Repeated-usage content digest verifies"), RoundTrip.Data->GetBoolField(TEXT("roundTripVerified")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraSystemSpecRendererSemanticPreflightTest,
	"UE_AI_integration.Niagara.SystemSpecExport.RendererSemanticPreflight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraSystemSpecRendererSemanticPreflightTest::RunTest(const FString&)
{
	FNiagaraSystemSpecExportFixture Fixture;
	ON_SCOPE_EXIT
	{
		if (Fixture.System)
		{
			Fixture.System->WaitForCompilationComplete(false, false);
		}
		TestTrue(TEXT("Renderer semantic spec fixture and package are deleted"),
			NiagaraSystemSpecExportDeleteFixture(Fixture.PackageName));
	};
	if (!TestTrue(TEXT("Renderer semantic system fixture builds"), NiagaraSystemSpecExportCreateFixture(Fixture)))
	{
		return false;
	}
	const FNiagaraEmitterHandle& Handle = Fixture.System->GetEmitterHandles()[0];
	const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
	UNiagaraMeshRendererProperties* Mesh = NewObject<UNiagaraMeshRendererProperties>(
		Instance.Emitter, TEXT("SpecSemanticMesh"), RF_Transactional);
	UNiagaraRibbonRendererProperties* Ribbon = NewObject<UNiagaraRibbonRendererProperties>(
		Instance.Emitter, TEXT("SpecSemanticRibbon"), RF_Transactional);
	if (!TestNotNull(TEXT("Semantic mesh renderer builds"), Mesh)
		|| !TestNotNull(TEXT("Semantic ribbon renderer builds"), Ribbon))
	{
		return false;
	}
	Instance.Emitter->AddRenderer(Mesh, Instance.Version);
	Instance.Emitter->AddRenderer(Ribbon, Instance.Version);
	Fixture.Package->SetDirtyFlag(false);
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraGraphModuleTools(Registry);
	Registry.EndDomainRegistration();
	auto ExportParams = MakeShared<FJsonObject>();
	ExportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
	const FMCPToolResult Baseline = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), ExportParams);
	if (!TestTrue(TEXT("Renderer semantic baseline exports"), Baseline.bSuccess) || !Baseline.Data)
	{
		return false;
	}

	auto VerifyInvalidRendererValue = [&](const TCHAR* CaseName, UNiagaraRendererProperties* Renderer,
		const TCHAR* PropertyName, const TCHAR* InvalidText)
	{
		const TSharedPtr<FJsonObject> Spec = NiagaraSystemSpecExportCloneSpec(Baseline.Data);
		if (!TestNotNull(TEXT("Renderer semantic snapshot clones"), Spec.Get()))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& ParameterValue : Spec->GetArrayField(TEXT("userParameters")))
		{
			const TSharedPtr<FJsonObject> Parameter = ParameterValue->AsObject();
			if (Parameter->GetStringField(TEXT("name")) == TEXT("User.SpecRoundTrip"))
			{
				Parameter->SetNumberField(TEXT("default"), 99.0);
			}
		}
		const TSharedPtr<FJsonObject> EmitterSpec = NiagaraSystemSpecExportFirstObject(Spec, TEXT("emitters"));
		bool bChangedRequestedRenderer = false;
		for (const TSharedPtr<FJsonValue>& RendererValue : EmitterSpec->GetArrayField(TEXT("renderers")))
		{
			const TSharedPtr<FJsonObject> RendererSpec = RendererValue->AsObject();
			if (RendererSpec->GetStringField(TEXT("rendererPath")) == Renderer->GetPathName())
			{
				const TSharedPtr<FJsonObject> Properties = RendererSpec->GetObjectField(TEXT("properties"));
				if (!TestTrue(TEXT("Semantic test property is present in the export"), Properties->HasField(PropertyName)))
				{
					return;
				}
				Properties->SetStringField(PropertyName, InvalidText);
				bChangedRequestedRenderer = true;
			}
		}
		if (!TestTrue(TEXT("Semantic test selects the exact renderer"), bChangedRequestedRenderer))
		{
			return;
		}
		const FGuid BeforeChangeId = Fixture.Graph->GetChangeID();
		const int32 BeforeNodeCount = Fixture.Graph->Nodes.Num();
		const float BeforeDefault = Fixture.System->GetExposedParameters().GetParameterValue<float>(Fixture.UserFloat);
		const bool bBeforeDirty = Fixture.Package->IsDirty();
		auto ImportParams = MakeShared<FJsonObject>();
		ImportParams->SetStringField(TEXT("system"), Fixture.System->GetPathName());
		ImportParams->SetObjectField(TEXT("spec"), Spec);
		ImportParams->SetStringField(TEXT("requestId"), FString(TEXT("renderer-semantic-")) + CaseName);
		ImportParams->SetBoolField(TEXT("confirmWrite"), true);
		const FMCPToolResult Rejected = Registry.ExecuteTool(TEXT("content.niagara.system.spec.import"), ImportParams);
		TestFalse(FString::Printf(TEXT("%s renderer spec is rejected"), CaseName), Rejected.bSuccess);
		TestEqual(FString::Printf(TEXT("%s uses a renderer semantic error"), CaseName), Rejected.ErrorCode, FString(TEXT("renderer_property_invalid")));
		TestEqual(FString::Printf(TEXT("%s is rejected before the transaction"), CaseName), Rejected.HttpStatus, 422);
		TestEqual(TEXT("Renderer semantic rejection preserves the pending user default"),
			Fixture.System->GetExposedParameters().GetParameterValue<float>(Fixture.UserFloat), BeforeDefault);
		TestEqual(TEXT("Renderer semantic rejection preserves package dirty state"), Fixture.Package->IsDirty(), bBeforeDirty);
		TestEqual(TEXT("Renderer semantic rejection preserves graph change ID"), Fixture.Graph->GetChangeID(), BeforeChangeId);
		TestEqual(TEXT("Renderer semantic rejection preserves graph node count"), Fixture.Graph->Nodes.Num(), BeforeNodeCount);
		const FMCPToolResult After = Registry.ExecuteTool(TEXT("content.niagara.system.spec.export"), ExportParams);
		if (TestTrue(TEXT("Renderer semantic rejection re-exports"), After.bSuccess) && After.Data)
		{
			TestEqual(TEXT("Renderer semantic rejection preserves all authored renderer content"),
				After.Data->GetStringField(TEXT("specDigest")), Baseline.Data->GetStringField(TEXT("specDigest")));
		}
	};
	VerifyInvalidRendererValue(TEXT("tube-below-min"), Ribbon, TEXT("TubeSubdivisions"), TEXT("2"));
	VerifyInvalidRendererValue(TEXT("tube-above-max"), Ribbon, TEXT("TubeSubdivisions"), TEXT("17"));
	VerifyInvalidRendererValue(TEXT("tessellation-below-min"), Ribbon, TEXT("TessellationFactor"), TEXT("0"));
	VerifyInvalidRendererValue(TEXT("tessellation-above-max"), Ribbon, TEXT("TessellationFactor"), TEXT("17"));
	VerifyInvalidRendererValue(TEXT("metadata-clamp-min"), Mesh, TEXT("MinCameraDistance"), TEXT("-1.000000"));
	VerifyInvalidRendererValue(TEXT("metadata-clamp-max"), Mesh, TEXT("FlipbookSuffixNumDigits"), TEXT("11"));
	// UE's numeric text importer consumes decimal digits, not exponent
	// notation. A fully consumed overflow exercises finite-value validation
	// rather than only the trailing-text parser check.
	const FString OverflowFloat = FString::ChrN(400, TCHAR('9')) + TEXT(".000000");
	const FString OverflowSubImage = FString::Printf(TEXT("(X=%s,Y=4.000000)"), *OverflowFloat);
	VerifyInvalidRendererValue(TEXT("scalar-nonfinite"), Mesh, TEXT("MinCameraDistance"), *OverflowFloat);
	VerifyInvalidRendererValue(TEXT("struct-nonfinite"), Fixture.SpriteRenderer, TEXT("SubImageSize"), *OverflowSubImage);
	// Sprite SubImageSize has a native hook clamp rather than ClampMin
	// metadata. This proves the scratch hook runs before the live asset edit.
	VerifyInvalidRendererValue(TEXT("native-subuv-hook-clamp"), Fixture.SpriteRenderer,
		TEXT("SubImageSize"), TEXT("(X=0.500000,Y=4.000000)"));
	return true;
}
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
