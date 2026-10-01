// Contract tests for content.niagara.system.spec.export.
//
// The export is a read-only authored-system snapshot, so the tests never
// compile, never save, and never assume runtime/PIE. The unknown-system path is
// asserted directly; the valid-system path builds a transient /Game/ system +
// owned emitter/renderer fixture (mirroring NiagaraModuleStackContractTests.cpp)
// and verifies the schema envelope plus changed-default and renderer-property
// round trips, skipping with AddInfo when the fixture cannot build.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tools/MCPToolRegistry.h"

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EditorAssetLibrary.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/Package.h"
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
};

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
	// Fully initialize the System scripts so the system carries the authored
	// spawn/update stacks the export walks (the export itself never compiles).
	UNiagaraSystemFactoryNew::InitializeSystem(OutFixture.System, true);
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
	OutFixture.Package->SetDirtyFlag(false);
	return true;
}

bool NiagaraSystemSpecExportDeleteFixture(const FString& PackageName)
{
	const bool bDeleted = !UEditorAssetLibrary::DoesAssetExist(PackageName)
		|| UEditorAssetLibrary::DeleteAsset(PackageName);
	return bDeleted
		&& !UEditorAssetLibrary::DoesAssetExist(PackageName)
		&& !FPackageName::DoesPackageExist(PackageName);
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
	TestTrue(TEXT("Export includes a canonical spec digest"),
		!Result.Data->GetStringField(TEXT("specDigest")).IsEmpty());

	const FString OriginalSpecDigest = Result.Data->GetStringField(TEXT("specDigest"));

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
#endif // WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
