#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UEAIIntegrationTools
{
	void RegisterBlueprintAuthoringOperationsTools(FMCPToolRegistry& Registry);
}

namespace
{
	struct FBlueprintTimelineFixture
	{
		FString PackageName;
		UPackage* Package = nullptr;
		UBlueprint* Blueprint = nullptr;
		UEdGraph* EventGraph = nullptr;
	};

	FBlueprintTimelineFixture CreateTimelineFixture()
	{
		FBlueprintTimelineFixture Fixture;
		Fixture.PackageName = TEXT("/Game/Automation/UEAI_TimelineAuthoring_")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString AssetName =
			FPackageName::GetLongPackageAssetName(Fixture.PackageName);
		Fixture.Package = CreatePackage(*Fixture.PackageName);
		Fixture.Blueprint = Fixture.Package
			                    ? FKismetEditorUtilities::CreateBlueprint(
				                    AActor::StaticClass(),
				                    Fixture.Package,
				                    *AssetName,
				                    BPTYPE_Normal,
				                    UBlueprint::StaticClass(),
				                    UBlueprintGeneratedClass::StaticClass(),
				                    FName(TEXT("UEAI.BlueprintTimelineAuthoring")))
			                    : nullptr;
		if (!Fixture.Blueprint)
		{
			return Fixture;
		}

		Fixture.Blueprint->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		Fixture.EventGraph = Fixture.Blueprint->UbergraphPages.IsEmpty()
			                     ? nullptr
			                     : Fixture.Blueprint->UbergraphPages[0];
		FAssetRegistryModule::AssetCreated(Fixture.Blueprint);
		FKismetEditorUtilities::CompileBlueprint(
			Fixture.Blueprint,
			EBlueprintCompileOptions::SkipSave);
		return Fixture;
	}

	bool DeleteTimelineFixture(FBlueprintTimelineFixture& Fixture)
	{
		if (Fixture.Package)
		{
			Fixture.Package->SetDirtyFlag(false);
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Fixture.PackageName,
			FPackageName::GetAssetPackageExtension());
		bool bDeleted = true;
		if (Fixture.Blueprint)
		{
			// The fixture is intentionally never saved. Remove its registry row and
			// release the transient UObject directly so cleanup does not invoke an
			// editor asset-delete transaction for an in-memory package.
			FAssetRegistryModule::AssetDeleted(Fixture.Blueprint);
			Fixture.Blueprint->ClearFlags(RF_Public | RF_Standalone);
			Fixture.Blueprint->MarkAsGarbage();
		}
		if (IFileManager::Get().FileExists(*PackageFilename))
		{
			bDeleted = IFileManager::Get().Delete(
					*PackageFilename,
					/*RequireExists=*/false,
					/*EvenReadOnly=*/true,
					/*Quiet=*/true)
				&& bDeleted;
		}
		return bDeleted
			&& !IFileManager::Get().FileExists(*PackageFilename)
			&& !FPackageName::DoesPackageExist(Fixture.PackageName);
	}

	TSharedRef<FJsonObject> DeferredContext()
	{
		TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetBoolField(TEXT("deferCompile"), true);
		return Context;
	}

	TSharedRef<FJsonObject> BlueprintParams(
		const FString& PackageName,
		const FString& TimelineName,
		const FString& GraphName,
		const bool bDeferred = true)
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("blueprint"), PackageName);
		if (!TimelineName.IsEmpty())
		{
			Params->SetStringField(TEXT("timelineName"), TimelineName);
		}
		if (!GraphName.IsEmpty())
		{
			Params->SetStringField(TEXT("graph"), GraphName);
		}
		if (bDeferred)
		{
			Params->SetObjectField(TEXT("__ueWorkflow"), DeferredContext());
		}
		return Params;
	}

	TSharedRef<FJsonObject> MakeKey(
		const double Time,
		const double Value,
		const TCHAR* Interpolation = TEXT("linear"))
	{
		TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
		Key->SetNumberField(TEXT("time"), Time);
		Key->SetNumberField(TEXT("value"), Value);
		Key->SetStringField(TEXT("interpMode"), Interpolation);
		return Key;
	}

	TSharedRef<FJsonObject> MakeComponentKey(
		const double Time,
		const double X,
		const double Y,
		const double Z)
	{
		TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
		Key->SetNumberField(TEXT("time"), Time);
		Key->SetNumberField(TEXT("x"), X);
		Key->SetNumberField(TEXT("y"), Y);
		Key->SetNumberField(TEXT("z"), Z);
		Key->SetStringField(TEXT("interpMode"), TEXT("linear"));
		return Key;
	}

	TSharedRef<FJsonObject> MakeColorKey(
		const double Time,
		const double Red,
		const double Green,
		const double Blue,
		const double Alpha)
	{
		TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
		Key->SetNumberField(TEXT("time"), Time);
		Key->SetNumberField(TEXT("r"), Red);
		Key->SetNumberField(TEXT("g"), Green);
		Key->SetNumberField(TEXT("b"), Blue);
		Key->SetNumberField(TEXT("a"), Alpha);
		Key->SetStringField(TEXT("interpMode"), TEXT("linear"));
		return Key;
	}

	TSharedRef<FJsonObject> MakeEventKey(const double Time)
	{
		TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
		Key->SetNumberField(TEXT("time"), Time);
		Key->SetStringField(TEXT("interpMode"), TEXT("constant"));
		return Key;
	}

	TSharedPtr<FJsonObject> FindTrack(
		const TArray<TSharedPtr<FJsonValue>>& Tracks,
		const FString& TrackName)
	{
		for (const TSharedPtr<FJsonValue>& Value : Tracks)
		{
			const TSharedPtr<FJsonObject> Track =
				Value.IsValid() ? Value->AsObject() : nullptr;
			if (Track.IsValid()
				&& Track->GetStringField(TEXT("trackName")) == TrackName)
			{
				return Track;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindChannel(
		const TArray<TSharedPtr<FJsonValue>>& Channels,
		const FString& ChannelName)
	{
		for (const TSharedPtr<FJsonValue>& Value : Channels)
		{
			const TSharedPtr<FJsonObject> Channel =
				Value.IsValid() ? Value->AsObject() : nullptr;
			if (Channel.IsValid()
				&& Channel->GetStringField(TEXT("channel")) == ChannelName)
			{
				return Channel;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> SetKeysParams(
		const FString& PackageName,
		const FString& TimelineName,
		const FString& TrackName,
		const TArray<TSharedPtr<FJsonValue>>& Keys)
	{
		TSharedRef<FJsonObject> Params =
			BlueprintParams(PackageName, TimelineName, FString());
		Params->SetStringField(TEXT("trackName"), TrackName);
		Params->SetArrayField(TEXT("keys"), Keys);
		return Params;
	}

	bool AssertKey(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Key,
		const double ExpectedTime,
		const double ExpectedValue,
		const FString& ExpectedInterpolation,
		const TCHAR* Label)
	{
		if (!Test.TestNotNull(Label, Key.Get()))
		{
			return false;
		}
		Test.TestTrue(
			FString::Printf(TEXT("%s has expected time"), Label),
			FMath::IsNearlyEqual(
				static_cast<float>(Key->GetNumberField(TEXT("time"))),
				static_cast<float>(ExpectedTime)));
		Test.TestTrue(
			FString::Printf(TEXT("%s has expected value"), Label),
			FMath::IsNearlyEqual(
				static_cast<float>(Key->GetNumberField(TEXT("value"))),
				static_cast<float>(ExpectedValue)));
		Test.TestEqual(
			FString::Printf(TEXT("%s has expected interpolation"), Label),
			Key->GetStringField(TEXT("interpMode")),
			ExpectedInterpolation);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintTimelineAuthoringReadbackContractTest,
	"UE_AI_integration.Blueprint.Authoring.TimelineReadbackAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintTimelineAuthoringReadbackContractTest::RunTest(const FString&)
{
	FBlueprintTimelineFixture Fixture = CreateTimelineFixture();
	ON_SCOPE_EXIT
	{
		TestTrue(
			TEXT("Timeline authoring fixture and package are deleted"),
			DeleteTimelineFixture(Fixture));
	};

	if (!TestNotNull(TEXT("Timeline Blueprint fixture"), Fixture.Blueprint)
		|| !TestNotNull(TEXT("Timeline event graph fixture"), Fixture.EventGraph))
	{
		return false;
	}
	TestFalse(
		TEXT("Timeline fixture has no package on disk before authoring"),
		IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(
			Fixture.PackageName,
			FPackageName::GetAssetPackageExtension())));

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintAuthoringOperationsTools(Registry);
	Registry.EndDomainRegistration();
	TestNotNull(
		TEXT("Timeline add tool is registered"),
		Registry.FindTool(TEXT("blueprint.timeline.add")));
	TestNotNull(
		TEXT("Timeline get tool is registered"),
		Registry.FindTool(TEXT("blueprint.timeline.get")));
	TestNotNull(
		TEXT("Timeline track add tool is registered"),
		Registry.FindTool(TEXT("blueprint.timeline.track.add")));
	TestNotNull(
		TEXT("Timeline key set tool is registered"),
		Registry.FindTool(TEXT("blueprint.timeline.keys.set")));

	const FString TimelineName(TEXT("AuthoringTimeline"));
	const FMCPToolResult Added = Registry.ExecuteTool(
		TEXT("blueprint.timeline.add"),
		BlueprintParams(
			Fixture.PackageName,
			TimelineName,
			Fixture.EventGraph->GetName()));
	if (!TestTrue(TEXT("Timeline template is added through the registered tool"), Added.bSuccess)
		|| !TestNotNull(TEXT("Timeline add result"), Added.Data.Get()))
	{
		if (!Added.bSuccess)
		{
			AddError(Added.ErrorMessage);
		}
		return false;
	}
	TestFalse(
		TEXT("Deferred timeline add does not save"),
		Added.Data->GetBoolField(TEXT("saved")));

	struct FTrackSpec
	{
		const TCHAR* Name;
		const TCHAR* Type;
	};
	static const FTrackSpec TrackSpecs[] = {
		{TEXT("FloatTrack"), TEXT("float")},
		{TEXT("VectorTrack"), TEXT("vector")},
		{TEXT("EventTrack"), TEXT("event")},
		{TEXT("ColorTrack"), TEXT("color")},
	};
	for (const FTrackSpec& Spec : TrackSpecs)
	{
		TSharedRef<FJsonObject> Params = BlueprintParams(
			Fixture.PackageName,
			TimelineName,
			FString());
		Params->SetStringField(TEXT("trackName"), Spec.Name);
		Params->SetStringField(TEXT("trackType"), Spec.Type);
		const FMCPToolResult AddedTrack = Registry.ExecuteTool(
			TEXT("blueprint.timeline.track.add"),
			Params);
		if (!TestTrue(
			FString::Printf(TEXT("%s track is added through the registered tool"), Spec.Type),
			AddedTrack.bSuccess))
		{
			AddError(AddedTrack.ErrorMessage);
			return false;
		}
		TestFalse(
			FString::Printf(TEXT("Deferred %s track add does not save"), Spec.Type),
			AddedTrack.Data->GetBoolField(TEXT("saved")));
	}

	TArray<TSharedPtr<FJsonValue>> FloatKeys;
	FloatKeys.Add(MakeShared<FJsonValueObject>(MakeKey(0.0, 1.25)));
	FloatKeys.Add(MakeShared<FJsonValueObject>(MakeKey(1.0, 4.5, TEXT("cubic"))));
	TArray<TSharedPtr<FJsonValue>> VectorKeys;
	VectorKeys.Add(MakeShared<FJsonValueObject>(MakeComponentKey(0.0, 1.0, 2.0, 3.0)));
	VectorKeys.Add(MakeShared<FJsonValueObject>(MakeComponentKey(1.0, 4.0, 5.0, 6.0)));
	TArray<TSharedPtr<FJsonValue>> EventKeys;
	EventKeys.Add(MakeShared<FJsonValueObject>(MakeEventKey(0.25)));
	EventKeys.Add(MakeShared<FJsonValueObject>(MakeEventKey(1.25)));
	TArray<TSharedPtr<FJsonValue>> ColorKeys;
	ColorKeys.Add(MakeShared<FJsonValueObject>(MakeColorKey(0.0, 0.1, 0.2, 0.3, 1.0)));
	ColorKeys.Add(MakeShared<FJsonValueObject>(MakeColorKey(1.0, 0.4, 0.5, 0.6, 0.7)));

	struct FKeySpec
	{
		const TCHAR* TrackName;
		const TArray<TSharedPtr<FJsonValue>>* Keys;
	};
	const FKeySpec KeySpecs[] = {
		{TEXT("FloatTrack"), &FloatKeys},
		{TEXT("VectorTrack"), &VectorKeys},
		{TEXT("EventTrack"), &EventKeys},
		{TEXT("ColorTrack"), &ColorKeys},
	};
	for (const FKeySpec& Spec : KeySpecs)
	{
		const FMCPToolResult SetKeys = Registry.ExecuteTool(
			TEXT("blueprint.timeline.keys.set"),
			SetKeysParams(
				Fixture.PackageName,
				TimelineName,
				Spec.TrackName,
				*Spec.Keys));
		if (!TestTrue(
			FString::Printf(TEXT("%s keys are set through the registered tool"), Spec.TrackName),
			SetKeys.bSuccess))
		{
			AddError(SetKeys.ErrorMessage);
			return false;
		}
		TestFalse(
			FString::Printf(TEXT("Deferred %s keys do not save"), Spec.TrackName),
			SetKeys.Data->GetBoolField(TEXT("saved")));
	}

	const FMCPToolResult Readback = Registry.ExecuteTool(
		TEXT("blueprint.timeline.get"),
		BlueprintParams(Fixture.PackageName, TimelineName, FString(), false));
	if (!TestTrue(TEXT("Timeline get reads the authored template"), Readback.bSuccess)
		|| !TestNotNull(TEXT("Timeline readback data"), Readback.Data.Get()))
	{
		if (!Readback.bSuccess)
		{
			AddError(Readback.ErrorMessage);
		}
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Timelines =
		Readback.Data->GetArrayField(TEXT("timelines"));
	if (!TestEqual(TEXT("One timeline is read back"), Timelines.Num(), 1))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Timeline = Timelines[0]->AsObject();
	if (!TestNotNull(TEXT("Timeline readback object"), Timeline.Get()))
	{
		return false;
	}
	TestEqual(TEXT("Timeline name round trips"), Timeline->GetStringField(TEXT("name")), TimelineName);
	TestTrue(TEXT("Timeline auto-play is false by default"), !Timeline->GetBoolField(TEXT("autoPlay")));
	TestTrue(TEXT("Timeline loop is false by default"), !Timeline->GetBoolField(TEXT("loop")));

	const TSharedPtr<FJsonObject> FloatTrack = FindTrack(
		Timeline->GetArrayField(TEXT("floatTracks")), TEXT("FloatTrack"));
	const TSharedPtr<FJsonObject> VectorTrack = FindTrack(
		Timeline->GetArrayField(TEXT("vectorTracks")), TEXT("VectorTrack"));
	const TSharedPtr<FJsonObject> EventTrack = FindTrack(
		Timeline->GetArrayField(TEXT("eventTracks")), TEXT("EventTrack"));
	const TSharedPtr<FJsonObject> ColorTrack = FindTrack(
		Timeline->GetArrayField(TEXT("colorTracks")), TEXT("ColorTrack"));
	if (!TestNotNull(TEXT("Float track readback"), FloatTrack.Get())
		|| !TestNotNull(TEXT("Vector track readback"), VectorTrack.Get())
		|| !TestNotNull(TEXT("Event track readback"), EventTrack.Get())
		|| !TestNotNull(TEXT("Color track readback"), ColorTrack.Get()))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>& FloatReadbackKeys =
		FloatTrack->GetArrayField(TEXT("keys"));
	TestEqual(TEXT("Float track has two keys"), FloatReadbackKeys.Num(), 2);
	if (FloatReadbackKeys.Num() == 2)
	{
		AssertKey(*this, FloatReadbackKeys[0]->AsObject(), 0.0, 1.25, TEXT("linear"), TEXT("Float key 0"));
		AssertKey(*this, FloatReadbackKeys[1]->AsObject(), 1.0, 4.5, TEXT("cubic"), TEXT("Float key 1"));
	}

	const TArray<TSharedPtr<FJsonValue>>& VectorChannels =
		VectorTrack->GetArrayField(TEXT("channels"));
	TestEqual(TEXT("Vector track has three channels"), VectorChannels.Num(), 3);
	for (const TPair<FString, double>& Expected : {
		     TPair<FString, double>(TEXT("x"), 4.0),
		     TPair<FString, double>(TEXT("y"), 5.0),
		     TPair<FString, double>(TEXT("z"), 6.0)
	     })
	{
		const TSharedPtr<FJsonObject> Channel = FindChannel(VectorChannels, Expected.Key);
		TestNotNull(FString::Printf(TEXT("Vector %s channel readback"), *Expected.Key), Channel.Get());
		if (Channel.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>& Keys = Channel->GetArrayField(TEXT("keys"));
			TestEqual(FString::Printf(TEXT("Vector %s channel has two keys"), *Expected.Key), Keys.Num(), 2);
			if (Keys.Num() == 2)
			{
				TestTrue(
					FString::Printf(TEXT("Vector %s final value round trips"), *Expected.Key),
					FMath::IsNearlyEqual(
						static_cast<float>(Keys[1]->AsObject()->GetNumberField(TEXT("value"))),
						static_cast<float>(Expected.Value)));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>& EventReadbackKeys =
		EventTrack->GetArrayField(TEXT("keys"));
	TestEqual(TEXT("Event track has two time keys"), EventReadbackKeys.Num(), 2);
	if (EventReadbackKeys.Num() == 2)
	{
		TestTrue(
			TEXT("Event key 0 time round trips"),
			FMath::IsNearlyEqual(
				static_cast<float>(EventReadbackKeys[0]->AsObject()->GetNumberField(TEXT("time"))),
				0.25f));
		TestTrue(
			TEXT("Event key 1 time round trips"),
			FMath::IsNearlyEqual(
				static_cast<float>(EventReadbackKeys[1]->AsObject()->GetNumberField(TEXT("time"))),
				1.25f));
	}

	const TArray<TSharedPtr<FJsonValue>>& ColorChannels =
		ColorTrack->GetArrayField(TEXT("channels"));
	TestEqual(TEXT("Color track has four channels"), ColorChannels.Num(), 4);
	for (const TPair<FString, double>& Expected : {
		     TPair<FString, double>(TEXT("r"), 0.4),
		     TPair<FString, double>(TEXT("g"), 0.5),
		     TPair<FString, double>(TEXT("b"), 0.6),
		     TPair<FString, double>(TEXT("a"), 0.7)
	     })
	{
		const TSharedPtr<FJsonObject> Channel = FindChannel(ColorChannels, Expected.Key);
		TestNotNull(FString::Printf(TEXT("Color %s channel readback"), *Expected.Key), Channel.Get());
		if (Channel.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>& Keys = Channel->GetArrayField(TEXT("keys"));
			TestEqual(FString::Printf(TEXT("Color %s channel has two keys"), *Expected.Key), Keys.Num(), 2);
			if (Keys.Num() == 2)
			{
				TestTrue(
					FString::Printf(TEXT("Color %s final value round trips"), *Expected.Key),
					FMath::IsNearlyEqual(
						static_cast<float>(Keys[1]->AsObject()->GetNumberField(TEXT("value"))),
						static_cast<float>(Expected.Value)));
			}
		}
	}

	// All validation happens before a curve is modified. A bad follow-up key
	// must therefore leave the existing float curve exactly as it was.
	TArray<TSharedPtr<FJsonValue>> DuplicateKeys;
	DuplicateKeys.Add(MakeShared<FJsonValueObject>(MakeKey(0.0, 99.0)));
	DuplicateKeys.Add(MakeShared<FJsonValueObject>(MakeKey(0.0, 100.0)));
	const FMCPToolResult DuplicateKeyResult = Registry.ExecuteTool(
		TEXT("blueprint.timeline.keys.set"),
		SetKeysParams(Fixture.PackageName, TimelineName, TEXT("FloatTrack"), DuplicateKeys));
	TestFalse(TEXT("Duplicate key times are rejected"), DuplicateKeyResult.bSuccess);
	TestEqual(TEXT("Duplicate key times use invalid_keys"), DuplicateKeyResult.ErrorCode,
	          FString(TEXT("invalid_keys")));

	TArray<TSharedPtr<FJsonValue>> MissingValueKeys;
	TSharedRef<FJsonObject> MissingValue = MakeShared<FJsonObject>();
	MissingValue->SetNumberField(TEXT("time"), 0.5);
	MissingValueKeys.Add(MakeShared<FJsonValueObject>(MissingValue));
	const FMCPToolResult MissingValueResult = Registry.ExecuteTool(
		TEXT("blueprint.timeline.keys.set"),
		SetKeysParams(Fixture.PackageName, TimelineName, TEXT("FloatTrack"), MissingValueKeys));
	TestFalse(TEXT("Missing key values are rejected"), MissingValueResult.bSuccess);
	TestEqual(TEXT("Missing key values use invalid_keys"), MissingValueResult.ErrorCode, FString(TEXT("invalid_keys")));

	const FMCPToolResult ReadbackAfterInvalid = Registry.ExecuteTool(
		TEXT("blueprint.timeline.get"),
		BlueprintParams(Fixture.PackageName, TimelineName, FString(), false));
	if (TestTrue(TEXT("Readback after rejected keys succeeds"), ReadbackAfterInvalid.bSuccess)
		&& TestNotNull(TEXT("Readback after rejected keys has data"), ReadbackAfterInvalid.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& TimelinesAfterInvalid =
			ReadbackAfterInvalid.Data->GetArrayField(TEXT("timelines"));
		if (TestEqual(TEXT("Readback after rejected keys has one timeline"), TimelinesAfterInvalid.Num(), 1))
		{
			const TSharedPtr<FJsonObject> TimelineAfterInvalid = TimelinesAfterInvalid[0]->AsObject();
			const TArray<TSharedPtr<FJsonValue>>& FloatTracksAfterInvalid =
				TimelineAfterInvalid->GetArrayField(TEXT("floatTracks"));
			if (TestEqual(TEXT("Readback after rejected keys has one float track"), FloatTracksAfterInvalid.Num(), 1))
			{
				const TSharedPtr<FJsonObject> FloatAfterInvalid = FloatTracksAfterInvalid[0]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>& FloatKeysAfterInvalid =
					FloatAfterInvalid->GetArrayField(TEXT("keys"));
				TestEqual(TEXT("Rejected follow-up keys preserve curve key count"), FloatKeysAfterInvalid.Num(), 2);
				if (FloatKeysAfterInvalid.Num() == 2)
				{
					AssertKey(*this, FloatKeysAfterInvalid[0]->AsObject(), 0.0, 1.25, TEXT("linear"),
					          TEXT("Preserved float key 0"));
					AssertKey(*this, FloatKeysAfterInvalid[1]->AsObject(), 1.0, 4.5, TEXT("cubic"),
					          TEXT("Preserved float key 1"));
				}
			}
		}
	}

	const FMCPToolResult DuplicateTimeline = Registry.ExecuteTool(
		TEXT("blueprint.timeline.add"),
		BlueprintParams(Fixture.PackageName, TimelineName, Fixture.EventGraph->GetName()));
	TestFalse(TEXT("Duplicate timeline names are rejected"), DuplicateTimeline.bSuccess);
	TestEqual(TEXT("Duplicate timeline names use already_exists"), DuplicateTimeline.ErrorCode,
	          FString(TEXT("already_exists")));

	TSharedRef<FJsonObject> DuplicateTrackParams = BlueprintParams(
		Fixture.PackageName, TimelineName, FString());
	DuplicateTrackParams->SetStringField(TEXT("trackName"), TEXT("FloatTrack"));
	DuplicateTrackParams->SetStringField(TEXT("trackType"), TEXT("float"));
	const FMCPToolResult DuplicateTrack = Registry.ExecuteTool(
		TEXT("blueprint.timeline.track.add"), DuplicateTrackParams);
	TestFalse(TEXT("Duplicate track names are rejected"), DuplicateTrack.bSuccess);
	TestEqual(TEXT("Duplicate track names use already_exists"), DuplicateTrack.ErrorCode,
	          FString(TEXT("already_exists")));

	TSharedRef<FJsonObject> InvalidTypeParams = BlueprintParams(
		Fixture.PackageName, TimelineName, FString());
	InvalidTypeParams->SetStringField(TEXT("trackName"), TEXT("InvalidTrack"));
	InvalidTypeParams->SetStringField(TEXT("trackType"), TEXT("scalar"));
	const FMCPToolResult InvalidType = Registry.ExecuteTool(
		TEXT("blueprint.timeline.track.add"), InvalidTypeParams);
	TestFalse(TEXT("Unknown track types are rejected"), InvalidType.bSuccess);
	TestEqual(TEXT("Unknown track types use invalid_track_type"), InvalidType.ErrorCode,
	          FString(TEXT("invalid_track_type")));

	TSharedRef<FJsonObject> MissingTrackName = BlueprintParams(
		Fixture.PackageName, TimelineName, FString());
	const FMCPToolResult MissingTrack = Registry.ExecuteTool(
		TEXT("blueprint.timeline.track.add"), MissingTrackName);
	TestFalse(TEXT("Missing track name is rejected"), MissingTrack.bSuccess);
	TestEqual(TEXT("Missing track name uses invalid_params"), MissingTrack.ErrorCode, FString(TEXT("invalid_params")));

	TSharedRef<FJsonObject> MissingKeys = BlueprintParams(
		Fixture.PackageName, TimelineName, FString());
	MissingKeys->SetStringField(TEXT("trackName"), TEXT("FloatTrack"));
	const FMCPToolResult MissingKeysResult = Registry.ExecuteTool(
		TEXT("blueprint.timeline.keys.set"), MissingKeys);
	TestFalse(TEXT("Missing keys array is rejected"), MissingKeysResult.bSuccess);
	TestEqual(TEXT("Missing keys array uses invalid_params"), MissingKeysResult.ErrorCode,
	          FString(TEXT("invalid_params")));

	TArray<TSharedPtr<FJsonValue>> UnknownTrackKeys;
	UnknownTrackKeys.Add(MakeShared<FJsonValueObject>(MakeKey(0.0, 1.0)));
	const FMCPToolResult UnknownTrack = Registry.ExecuteTool(
		TEXT("blueprint.timeline.keys.set"),
		SetKeysParams(Fixture.PackageName, TimelineName, TEXT("MissingTrack"), UnknownTrackKeys));
	TestFalse(TEXT("Unknown timeline tracks are rejected"), UnknownTrack.bSuccess);
	TestEqual(TEXT("Unknown timeline tracks use track_not_found"), UnknownTrack.ErrorCode,
	          FString(TEXT("track_not_found")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGenericPackageSubobjectSaveTest,
	"UEAI.Persistence.GenericPackage.SubobjectSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGenericPackageSubobjectSaveTest::RunTest(const FString& Parameters)
{
	FBlueprintTimelineFixture Fixture = CreateTimelineFixture();
	ON_SCOPE_EXIT
	{
		TestTrue(TEXT("Saved fixture is cleaned up"), DeleteTimelineFixture(Fixture));
	};
	if (!TestNotNull(TEXT("Owned graph exists"), Fixture.EventGraph))
	{
		return false;
	}
	TestFalse(TEXT("The graph is not a standalone asset"), Fixture.EventGraph->HasAnyFlags(RF_Standalone));
	TestTrue(TEXT("Saving an owned graph saves its Blueprint package"),
		MCPHelpers::SaveGenericPackage(Fixture.EventGraph));
	TestTrue(TEXT("The owner package exists on disk"), FPackageName::DoesPackageExist(Fixture.PackageName));
	TestFalse(TEXT("Saving does not promote the graph to an asset"), Fixture.EventGraph->HasAnyFlags(RF_Standalone));
	TestFalse(TEXT("Transient objects are rejected without entering SavePackage"),
		MCPHelpers::SaveGenericPackage(NewObject<UEdGraph>(GetTransientPackage())));
	TestFalse(TEXT("Null targets are rejected"), MCPHelpers::SaveGenericPackage(nullptr));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
