#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "RHIGlobals.h"
#include "RHIResources.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TextureResource.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterSceneEngineeringQueryTools(FMCPToolRegistry& Registry);
void RegisterContentAssetReadTools(FMCPToolRegistry& Registry);
}

namespace UEAISceneEngineeringQueryPrivate
{
FString RedactResourcePath(const FString& Input, bool& bRedacted);
}

namespace
{
void RegisterRHIQueries(FMCPToolRegistry& Registry)
{
	Registry.BeginDomainRegistration(TEXT("scene"));
	UEAIIntegrationTools::RegisterSceneEngineeringQueryTools(Registry);
	Registry.EndDomainRegistration();
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterContentAssetReadTools(Registry);
	Registry.EndDomainRegistration();
}

void CheckNoPrivateResourceFields(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data)
{
	if (!Data.IsValid())
	{
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Data->Values)
	{
		const FString Key = Field.Key.ToLower();
		Test.TestFalse(
			FString::Printf(TEXT("Resource evidence excludes private field %s"), *Field.Key),
			Key.Contains(TEXT("pointer")) || Key.Contains(TEXT("address"))
				|| Key.Contains(TEXT("handle")) || Key.Contains(TEXT("nativeresource"))
				|| Key.Contains(TEXT("heap")) || Key.Contains(TEXT("barrier")));
		if (Field.Value->Type == EJson::Object)
		{
			CheckNoPrivateResourceFields(Test, Field.Value->AsObject());
		}
		else if (Field.Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Value : Field.Value->AsArray())
			{
				if (Value.IsValid() && Value->Type == EJson::Object)
				{
					CheckNoPrivateResourceFields(Test, Value->AsObject());
				}
			}
		}
	}
}

class FRegisteredTextureFixture
{
public:
	FRegisteredTextureFixture()
	{
		const FString Name = TEXT("T_RHI_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		Package = CreatePackage(*(TEXT("/Game/__UEAIRHIQueries/") + Name));
		Texture = UTexture2D::CreateTransient(16, 8, PF_B8G8R8A8, FName(*Name));
		if (!Texture)
		{
			return;
		}
		Texture->AddToRoot();
		if (!Texture->Rename(*Name, Package, REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty))
		{
			return;
		}
		Texture->ClearFlags(RF_Transient);
		Texture->SetFlags(RF_Public | RF_Standalone);
		Texture->NeverStream = true;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* Pixels = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memzero(Pixels, 16 * 8 * 4);
		Mip.BulkData.Unlock();
		FAssetRegistryModule::AssetCreated(Texture);
		bRegistered = true;
	}

	~FRegisteredTextureFixture()
	{
		if (Texture)
		{
			Texture->ReleaseResource();
			FlushRenderingCommands();
			if (bRegistered)
			{
				FAssetRegistryModule::AssetDeleted(Texture);
			}
			Texture->ClearFlags(RF_Public | RF_Standalone);
			Texture->RemoveFromRoot();
			Texture->MarkAsGarbage();
		}
		if (Package)
		{
			Package->SetDirtyFlag(false);
			Package->MarkAsGarbage();
		}
	}

	UTexture2D* Texture = nullptr;
	bool bRegistered = false;

private:
	UPackage* Package = nullptr;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAIRHIBackendEvidenceTest,
	"UE_AI_integration.Rendering.RHI.BackendEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAIRHIBackendEvidenceTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	RegisterRHIQueries(Registry);
	const bool bHardwareRHI = GDynamicRHI != nullptr && !GUsingNullRHI;
	const FMCPToolResult Context = Registry.ExecuteTool(TEXT("scene.render.context.get"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Render context query returns evidence"), Context.bSuccess && Context.Data.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Context availability requires a hardware backend"), Context.Data->GetBoolField(TEXT("available")), bHardwareRHI);
	TestEqual(TEXT("Context reports the actual NullRHI state"), Context.Data->GetBoolField(TEXT("usingNullRHI")), GUsingNullRHI);
	TestEqual(TEXT("Context reports whether a dynamic backend exists"), Context.Data->GetBoolField(TEXT("dynamicRHI")), GDynamicRHI != nullptr);
	TestFalse(TEXT("Context identifies its evidence boundary"), Context.Data->GetStringField(TEXT("evidenceBoundary")).IsEmpty());
	if (!bHardwareRHI)
	{
		TestFalse(TEXT("Unavailable context explains why it cannot prove hardware rendering"), Context.Data->GetStringField(TEXT("availabilityReason")).IsEmpty());
	}

	const FMCPToolResult Audit = Registry.ExecuteTool(TEXT("scene.render.feature.audit"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Feature audit returns evidence"), Audit.bSuccess && Audit.Data.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Feature audit availability requires hardware"), Audit.Data->GetBoolField(TEXT("available")), bHardwareRHI);
	if (!bHardwareRHI)
	{
		for (const TSharedPtr<FJsonValue>& Feature : Audit.Data->GetArrayField(TEXT("features")))
		{
			TestFalse(TEXT("No feature claims hardware support without hardware RHI"), Feature->AsObject()->GetBoolField(TEXT("hardwareSupported")));
		}
	}

	const FMCPToolResult Memory = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Memory query returns evidence"), Memory.bSuccess && Memory.Data.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Memory availability requires hardware"), Memory.Data->GetBoolField(TEXT("available")), bHardwareRHI);
	if (GUsingNullRHI && GDynamicRHI)
	{
		TestFalse(TEXT("NullRHI statistics are never valid hardware evidence"), Memory.Data->GetBoolField(TEXT("hardwareStatsValid")));
		TestFalse(TEXT("NullRHI cannot claim valid memory statistics"), Memory.Data->GetBoolField(TEXT("statsValid")));
	}
	if (GDynamicRHI)
	{
		const TSharedPtr<FJsonObject> Tracked = Memory.Data->GetObjectField(TEXT("trackedResources"));
		TestFalse(TEXT("Default memory sampling does not request resource enumeration"), Tracked->GetBoolField(TEXT("requested")));
		TestFalse(TEXT("Omitted resource enumeration does not claim an available snapshot"), Tracked->GetBoolField(TEXT("available")));
	}
	CheckNoPrivateResourceFields(*this, Context.Data);
	CheckNoPrivateResourceFields(*this, Memory.Data);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAIRHIMemoryBoundariesTest,
	"UE_AI_integration.Rendering.RHI.MemoryBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAIRHIMemoryBoundariesTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	RegisterRHIQueries(Registry);
	if (!GDynamicRHI)
	{
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), MakeShared<FJsonObject>());
		TestTrue(TEXT("NoRHI produces explicit unavailable evidence"), Result.bSuccess && Result.Data.IsValid() && !Result.Data->GetBoolField(TEXT("available")));
		AddInfo(TEXT("Parameter execution requires a dynamic RHI; the NoRHI unavailable path was checked."));
		return true;
	}
	for (const double Limit : {-1.0, 0.0, 1.5, 257.0})
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), Limit);
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), Params);
		TestFalse(FString::Printf(TEXT("Invalid limit %.1f is rejected by the handler"), Limit), Result.bSuccess);
		TestEqual(TEXT("Invalid limit has a parameter error"), Result.ErrorCode, FString(TEXT("invalid_params")));
	}
	for (const TCHAR* Field : {TEXT("nameContains"), TEXT("ownerContains"), TEXT("resourceType")})
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(Field, FString::ChrN(257, TEXT('x')));
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), Params);
		TestFalse(FString::Printf(TEXT("Oversized filter %s is rejected"), Field), Result.bSuccess);
		TestEqual(TEXT("Oversized filter has a parameter error"), Result.ErrorCode, FString(TEXT("invalid_params")));
	}
	TSharedRef<FJsonObject> InvalidTransient = MakeShared<FJsonObject>();
	InvalidTransient->SetStringField(TEXT("transient"), TEXT("sometimes"));
	TestEqual(TEXT("Unsupported transient filter is rejected"), Registry.ExecuteTool(TEXT("scene.render.memory.sample"), InvalidTransient).ErrorCode, FString(TEXT("invalid_params")));

	for (const double Limit : {1.0, 256.0})
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("limit"), Limit);
		Params->SetBoolField(TEXT("includeResources"), true);
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), Params);
		if (!TestTrue(TEXT("Inclusive limit boundary is accepted"), Result.bSuccess && Result.Data.IsValid()))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Tracked = Result.Data->GetObjectField(TEXT("trackedResources"));
		TestTrue(TEXT("Resource request is acknowledged"), Tracked->GetBoolField(TEXT("requested")));
		const int32 Returned = static_cast<int32>(Tracked->GetNumberField(TEXT("returned")));
		const int32 Matched = static_cast<int32>(Tracked->GetNumberField(TEXT("matched")));
		TestTrue(TEXT("A snapshot never returns more than its limit"), Returned >= 0 && Returned <= Limit);
		TestTrue(TEXT("Matched includes every returned resource"), Matched >= Returned);
		TestEqual(TEXT("Truncation agrees with this snapshot's counts"), Tracked->GetBoolField(TEXT("truncated")), Matched > Returned);
#if defined(RHI_ENABLE_RESOURCE_INFO) && RHI_ENABLE_RESOURCE_INFO
		if (!GUsingNullRHI)
		{
			TestTrue(TEXT("Hardware RHI with resource-info support exposes requested resource sampling"), Tracked->GetBoolField(TEXT("available")));
		}
#endif
		if (Tracked->GetBoolField(TEXT("available")))
		{
			TestEqual(TEXT("Returned count matches the actual array"), Tracked->GetArrayField(TEXT("resources")).Num(), Returned);
			TestEqual(TEXT("Counts identify backend-info coverage"), Tracked->GetStringField(TEXT("countScope")), FString(TEXT("resourcesWithBackendInfo")));
			TestEqual(TEXT("Resource evidence identifies partial backend coverage"), Tracked->GetStringField(TEXT("coverage")), FString(TEXT("backendProvided")));
			TestEqual(TEXT("The snapshot does not infer runtime tracking state"), Tracked->GetStringField(TEXT("trackingState")), FString(TEXT("notExposed")));
			TestTrue(TEXT("Backend owner-field availability is explicitly typed"), Tracked->HasTypedField<EJson::Boolean>(TEXT("ownerFieldSupported")));
		}
		if (GUsingNullRHI)
		{
			TestFalse(TEXT("NullRHI never supplies tracked hardware resources"), Tracked->GetBoolField(TEXT("available")));
			TestEqual(TEXT("NullRHI supplies no resource rows"), Returned, 0);
		}
		CheckNoPrivateResourceFields(*this, Result.Data);
	}

	TSharedRef<FJsonObject> NoMatches = MakeShared<FJsonObject>();
	NoMatches->SetBoolField(TEXT("includeResources"), true);
	NoMatches->SetStringField(TEXT("nameContains"), TEXT("UEAINoSuchResource_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FMCPToolResult Filtered = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), NoMatches);
	if (TestTrue(TEXT("A valid empty filter query succeeds"), Filtered.bSuccess && Filtered.Data.IsValid()))
	{
		TestEqual(TEXT("A unique unmatched label produces zero rows"), Filtered.Data->GetObjectField(TEXT("trackedResources"))->GetNumberField(TEXT("returned")), 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAITextureRHILiveDescriptorTest,
	"UE_AI_integration.Content.Texture.RHI.LiveDescriptor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAITextureRHILiveDescriptorTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	RegisterRHIQueries(Registry);
	FRegisteredTextureFixture Fixture;
	if (!TestTrue(TEXT("Unsaved texture fixture is registered"), Fixture.bRegistered))
	{
		return false;
	}
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset"), Fixture.Texture->GetPathName());
	const FMCPToolResult Before = Registry.ExecuteTool(TEXT("content.texture.rhi.inspect"), Params);
	if (!TestTrue(TEXT("Authored fixture can be inspected before GPU initialization"), Before.bSuccess && Before.Data.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("Authored metadata alone does not imply an initialized GPU resource"), Before.Data->GetBoolField(TEXT("resourceAvailable")));
	TestFalse(TEXT("No descriptor is invented before initialization"), Before.Data->HasField(TEXT("descriptor")));
	const bool bHardwareRHI = GDynamicRHI != nullptr && !GUsingNullRHI;
	if (bHardwareRHI)
	{
		Fixture.Texture->UpdateResource();
	}
	// The capability must wait for the queued initialization itself.
	const FMCPToolResult Live = Registry.ExecuteTool(TEXT("content.texture.rhi.inspect"), Params);
	if (!TestTrue(TEXT("Registered texture has a query result"), Live.bSuccess && Live.Data.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("Texture query identifies its schema"), Live.Data->GetStringField(TEXT("schema")), FString(TEXT("ue.texture-rhi-inspection.v1")));
	TestEqual(TEXT("Texture query reports actual hardware availability"), Live.Data->GetBoolField(TEXT("rhiAvailable")), bHardwareRHI);
	TestEqual(TEXT("Texture resource becomes available only with hardware"), Live.Data->GetBoolField(TEXT("resourceAvailable")), bHardwareRHI);
	TestEqual(TEXT("A hardware texture inspection includes its live descriptor"), Live.Data->HasField(TEXT("descriptor")), bHardwareRHI);
	TestEqual(TEXT("Texture query reports NullRHI correctly"), Live.Data->GetBoolField(TEXT("usingNullRHI")), GUsingNullRHI);
	if (bHardwareRHI && Live.Data->HasField(TEXT("descriptor")))
	{
		const TSharedPtr<FJsonObject> Descriptor = Live.Data->GetObjectField(TEXT("descriptor"));
		TestEqual(TEXT("Live descriptor width matches initialized pixels"), Descriptor->GetNumberField(TEXT("width")), 16.0);
		TestEqual(TEXT("Live descriptor height matches initialized pixels"), Descriptor->GetNumberField(TEXT("height")), 8.0);
		TestEqual(TEXT("Live descriptor uses the requested pixel format"), Descriptor->GetStringField(TEXT("format")), FString(TEXT("B8G8R8A8")));
		TestEqual(TEXT("Fixture has one initialized mip"), Descriptor->GetNumberField(TEXT("mipCount")), 1.0);
		TestEqual(TEXT("Fixture is a 2D texture"), Descriptor->GetStringField(TEXT("dimension")), FString(TEXT("Texture2D")));
		TestTrue(TEXT("Live descriptor has a positive memory estimate"), Live.Data->GetNumberField(TEXT("resourceMemoryEstimateBytes")) > 0.0);
		TestEqual(TEXT("Initialized state is explicit"), Live.Data->GetStringField(TEXT("resourceState")), FString(TEXT("initialized")));
	}
	else if (!bHardwareRHI)
	{
		TestFalse(TEXT("NullRHI/NoRHI includes no GPU descriptor"), Live.Data->HasField(TEXT("descriptor")));
		TestFalse(TEXT("Unavailable hardware has a reason"), Live.Data->GetStringField(TEXT("reason")).IsEmpty());
	}
	TestFalse(TEXT("Texture query documents its evidence boundary"), Live.Data->GetStringField(TEXT("evidenceBoundary")).IsEmpty());
	TestFalse(TEXT("Texture query returns a digest"), Live.Data->GetStringField(TEXT("snapshotDigest")).IsEmpty());
	CheckNoPrivateResourceFields(*this, Live.Data);
	FString Json;
	FJsonSerializer::Serialize(Live.Data.ToSharedRef(), TJsonWriterFactory<>::Create(&Json));
	FPaths::NormalizeFilename(Json);
	TestFalse(TEXT("Texture evidence excludes the OS project directory"), Json.Contains(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Texture evidence excludes the OS engine directory"), Json.Contains(FPaths::ConvertRelativePathToFull(FPaths::EngineDir()), ESearchCase::IgnoreCase));

#if defined(RHI_ENABLE_RESOURCE_INFO) && RHI_ENABLE_RESOURCE_INFO
	if (bHardwareRHI && Live.Data->GetBoolField(TEXT("resourceAvailable")))
	{
		const FString UniqueLabel = TEXT("UEAIRHIPath_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString RawLabel = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()) + TEXT("Textures/") + UniqueLabel;
		FPaths::NormalizeFilename(RawLabel);
		bool bFixtureHasBackendInfo = false;
		ENQUEUE_RENDER_COMMAND(UEAITestSetTextureResourceLabel)(
			[Texture = Fixture.Texture, RawLabel, &bFixtureHasBackendInfo](FRHICommandListImmediate&)
			{
				const FTextureResource* Resource = Texture->GetResource();
				FRHITexture* RHITexture = Resource ? Resource->GetTexture2DRHI() : nullptr;
				if (RHITexture)
				{
					RHITexture->SetName(FName(*RawLabel));
					FRHIResourceInfo Info;
					bFixtureHasBackendInfo = RHITexture->GetResourceInfo(Info);
				}
			});
		FlushRenderingCommands();
		TSharedRef<FJsonObject> ResourceParams = MakeShared<FJsonObject>();
		ResourceParams->SetBoolField(TEXT("includeResources"), true);
		ResourceParams->SetNumberField(TEXT("limit"), 8.0);
		ResourceParams->SetStringField(TEXT("nameContains"), UniqueLabel);
		const FMCPToolResult ResourceQuery = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), ResourceParams);
		if (TestTrue(TEXT("A live texture label can be queried through tracked-resource sampling"), ResourceQuery.bSuccess && ResourceQuery.Data.IsValid()))
		{
			const TSharedPtr<FJsonObject> Tracked = ResourceQuery.Data->GetObjectField(TEXT("trackedResources"));
			const bool bTrackingAvailable = TestTrue(TEXT("Hardware RHI with resource-info support exposes fixture resource sampling"), Tracked->GetBoolField(TEXT("available")));
			if (bTrackingAvailable && bFixtureHasBackendInfo && Tracked->GetNumberField(TEXT("resourceCount")) > 0.0)
			{
				const TArray<TSharedPtr<FJsonValue>>& Rows = Tracked->GetArrayField(TEXT("resources"));
				bool bLabelVerified = TestTrue(TEXT("Enabled backend tracking finds the initialized fixture by its unique raw label"), !Rows.IsEmpty());
				for (const TSharedPtr<FJsonValue>& Row : Rows)
				{
					const TSharedPtr<FJsonObject> Item = Row->AsObject();
					bLabelVerified &= TestEqual(TEXT("Tracked texture labels redact the project directory"), Item->GetStringField(TEXT("name")), TEXT("<project>/Textures/") + UniqueLabel);
					bLabelVerified &= TestTrue(TEXT("Resource row reports path redaction"), Item->GetBoolField(TEXT("pathRedacted")));
					bLabelVerified &= TestFalse(TEXT("The resource row does not expose its raw OS label"), Item->GetStringField(TEXT("name")).Contains(RawLabel));
				}
				if (!Rows.IsEmpty())
				{
					const FString FixtureType = Rows[0]->AsObject()->GetStringField(TEXT("type"));
					ResourceParams->SetStringField(TEXT("resourceType"), FixtureType);
					ResourceParams->SetStringField(TEXT("nameContains"), RawLabel);
					const FMCPToolResult TypedQuery = Registry.ExecuteTool(TEXT("scene.render.memory.sample"), ResourceParams);
					if (TestTrue(TEXT("Full raw label and backend type can filter a live resource"), TypedQuery.bSuccess && TypedQuery.Data.IsValid()))
					{
						const TArray<TSharedPtr<FJsonValue>>& TypedRows = TypedQuery.Data->GetObjectField(TEXT("trackedResources"))->GetArrayField(TEXT("resources"));
						bool bTypeVerified = TestTrue(TEXT("Type filtering retains the matching fixture"), !TypedRows.IsEmpty());
						for (const TSharedPtr<FJsonValue>& Row : TypedRows)
						{
							bTypeVerified &= TestEqual(TEXT("Type-filtered rows match the requested backend type"), Row->AsObject()->GetStringField(TEXT("type")), FixtureType);
						}
						CheckNoPrivateResourceFields(*this, TypedQuery.Data);
						if (bLabelVerified && bTypeVerified)
						{
							AddInfo(FString::Printf(
								TEXT("tracked fixture label/type/redaction verified; backend=%s; rows=%d; typedRows=%d; backendInfoRows=%d"),
								GDynamicRHI->GetName(), Rows.Num(), TypedRows.Num(), static_cast<int32>(Tracked->GetNumberField(TEXT("resourceCount")))));
						}
					}
				}
			}
			else if (bTrackingAvailable)
			{
				AddInfo(FString::Printf(
					TEXT("tracked fixture label/type/redaction evidence not acquired; backend=%s; fixtureBackendInfo=%s; backendInfoRows=%d. The backend did not provide fixture info or the tracked snapshot was empty."),
					GDynamicRHI->GetName(), bFixtureHasBackendInfo ? TEXT("yes") : TEXT("no"), static_cast<int32>(Tracked->GetNumberField(TEXT("resourceCount")))));
			}
			CheckNoPrivateResourceFields(*this, ResourceQuery.Data);
		}
	}
#endif

	Fixture.Texture->ReleaseResource();
	const FMCPToolResult Released = Registry.ExecuteTool(TEXT("content.texture.rhi.inspect"), Params);
	if (TestTrue(TEXT("Released texture remains an authored asset"), Released.bSuccess && Released.Data.IsValid()))
	{
		TestFalse(TEXT("A released resource is no longer claimed as live"), Released.Data->GetBoolField(TEXT("resourceAvailable")));
		TestFalse(TEXT("A released resource has no stale descriptor"), Released.Data->HasField(TEXT("descriptor")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEAIRHILabelRedactionTest,
	"UE_AI_integration.Rendering.RHI.LabelRedaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEAIRHILabelRedactionTest::RunTest(const FString& Parameters)
{
	using UEAISceneEngineeringQueryPrivate::RedactResourcePath;
	for (const TPair<FString, FString>& Root : TArray<TPair<FString, FString>>{
		{FPaths::ProjectDir(), TEXT("<project>/")},
		{FPaths::EngineDir(), TEXT("<engine>/")}})
	{
		FString AbsoluteRoot = FPaths::ConvertRelativePathToFull(Root.Key);
		FPaths::NormalizeDirectoryName(AbsoluteRoot);
		bool bRedacted = false;
		TestEqual(TEXT("An exact root without a trailing separator is redacted"), RedactResourcePath(AbsoluteRoot, bRedacted), Root.Value);
		TestTrue(TEXT("Exact-root redaction is disclosed"), bRedacted);
		TestEqual(TEXT("An exact root with a trailing separator is redacted"), RedactResourcePath(AbsoluteRoot + TEXT("/"), bRedacted), Root.Value);
		TestTrue(TEXT("Trailing-separator root redaction is disclosed"), bRedacted);
		FString WindowsRoot = AbsoluteRoot;
		WindowsRoot.ReplaceInline(TEXT("/"), TEXT("\\"));
		TestEqual(TEXT("A Windows exact-root label is redacted"), RedactResourcePath(WindowsRoot, bRedacted), Root.Value);
		TestTrue(TEXT("Windows exact-root redaction is disclosed"), bRedacted);
		TestEqual(TEXT("Absolute resource paths redact their local root"), RedactResourcePath(AbsoluteRoot + TEXT("/Textures/Fixture"), bRedacted), Root.Value + TEXT("Textures/Fixture"));
		TestTrue(TEXT("Redaction is disclosed"), bRedacted);
		FString WindowsLabel = AbsoluteRoot + TEXT("/Textures/Fixture");
		WindowsLabel.ReplaceInline(TEXT("/"), TEXT("\\"));
		TestEqual(TEXT("Windows separators cannot bypass prefix redaction"), RedactResourcePath(WindowsLabel, bRedacted), Root.Value + TEXT("Textures/Fixture"));
		TestTrue(TEXT("Windows prefix redaction is disclosed"), bRedacted);
		const FString Adjacent = AbsoluteRoot + TEXT("_Other/Textures/Fixture");
		const FString AdjacentResult = RedactResourcePath(Adjacent, bRedacted);
		TestFalse(TEXT("An adjacent directory is not mistaken for this root"), AdjacentResult.StartsWith(Root.Value));
	}
	bool bRedacted = true;
	TestEqual(TEXT("Logical resource labels are preserved"), RedactResourcePath(TEXT("SceneColor"), bRedacted), FString(TEXT("SceneColor")));
	TestFalse(TEXT("Logical labels are not marked as paths"), bRedacted);
	return true;
}

#endif
