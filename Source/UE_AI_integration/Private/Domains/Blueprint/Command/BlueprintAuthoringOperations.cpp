// Additional Blueprint authoring operations kept separate from the legacy
// mutation command file.  The implementations mirror the native Blueprint
// editor data model so that reads and writes have the same semantics as the
// Monolith actions (timeline templates, dispatcher signature graphs, and CDO
// reflection).
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/BlueprintMutationGuard.h"
#include "Infrastructure/BlueprintPersistence.h"
#include "Infrastructure/Sha256.h"
#include "Workflow/UEWorkflowExecutionContext.h"

#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Timeline.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/SoftObjectPtr.h"
#include "ScopedTransaction.h"
#include "JsonObjectConverter.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	struct FCDOPropertyPersistenceBaseline
	{
		UPackage* Package = nullptr;
		FString PackageFilename;
		FString DiskBaselineFilename;
		FString Error;
		bool bPackageWasDirty = false;
		bool bDiskFileExisted = false;
		bool bValid = false;

		explicit FCDOPropertyPersistenceBaseline(UObject* Asset)
		{
			Package = Asset ? Asset->GetOutermost() : nullptr;
			if (!Asset || !Package)
			{
				Error = TEXT("The CDO persistence target has no package.");
				return;
			}
			bPackageWasDirty = Package->IsDirty();

			if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				UEAIIntegration::Infrastructure::FBlueprintPersistenceTarget Target;
				UEAIIntegration::Infrastructure::FBlueprintPersistenceError PersistenceError;
				if (!UEAIIntegration::Infrastructure::ResolveBlueprintPersistenceTarget(
					Blueprint,
					Target,
					PersistenceError))
				{
					Error = PersistenceError.Message;
					return;
				}
				PackageFilename = Target.Filename;
			}
			else
			{
				PackageFilename = FPackageName::LongPackageNameToFilename(
					Package->GetName(),
					FPackageName::GetAssetPackageExtension());
			}

			bDiskFileExisted = IFileManager::Get().FileExists(*PackageFilename);
			if (bDiskFileExisted)
			{
				const FString BaselineRoot = FPaths::Combine(
					FPaths::ProjectSavedDir(),
					TEXT("UEAIIntegration"),
					TEXT("CDOPropertyMutations"));
				if (!IFileManager::Get().MakeDirectory(*BaselineRoot, true))
				{
					Error = TEXT("Could not create the CDO persistence baseline directory.");
					return;
				}
				DiskBaselineFilename = FPaths::Combine(
					BaselineRoot,
					FGuid::NewGuid().ToString(EGuidFormats::Digits)
					+ FPaths::GetExtension(PackageFilename, true));
				if (IFileManager::Get().Copy(
					*DiskBaselineFilename,
					*PackageFilename,
					true,
					true) != COPY_OK)
				{
					Error = TEXT("Could not capture the CDO package baseline before mutation.");
					Cleanup();
					return;
				}
			}
			bValid = true;
		}

		~FCDOPropertyPersistenceBaseline()
		{
			Cleanup();
		}

		void Cleanup()
		{
			if (!DiskBaselineFilename.IsEmpty())
			{
				IFileManager::Get().Delete(*DiskBaselineFilename, false, true);
				DiskBaselineFilename.Reset();
			}
		}

		bool Restore(FString& OutError) const
		{
			bool bRestored = true;
			if (bDiskFileExisted && !DiskBaselineFilename.IsEmpty())
			{
				const FString RestoreTemporary = PackageFilename
					+ TEXT(".ueai-cdo-restore-")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
				const bool bCopied = IFileManager::Get().Copy(
					*RestoreTemporary,
					*DiskBaselineFilename,
					true,
					true) == COPY_OK;
				const bool bMoved = bCopied && IFileManager::Get().Move(
					*PackageFilename,
					*RestoreTemporary,
					true,
					true,
					false,
					true);
				if (!bMoved)
				{
					IFileManager::Get().Delete(*RestoreTemporary, false, true);
					bRestored = false;
				}
			}
			else if (!bDiskFileExisted && IFileManager::Get().FileExists(*PackageFilename))
			{
				bRestored = IFileManager::Get().Delete(*PackageFilename, false, true);
			}
			if (Package)
			{
				Package->SetDirtyFlag(bPackageWasDirty);
				bRestored = bRestored && Package->IsDirty() == bPackageWasDirty;
			}
			if (!bRestored)
			{
				OutError = TEXT("The CDO package or dirty state could not be restored.");
			}
			return bRestored;
		}
	};

	bool ReparentTransientInstancedSubobjects(
		UObject* TargetObject,
		FProperty* Property,
		void* Container)
	{
		if (!TargetObject || !Property || !Container)
		{
			return true;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Container);
		if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
		{
			if (ObjectProperty->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance))
			{
				if (UObject* Subobject = ObjectProperty->GetObjectPropertyValue(ValuePtr))
				{
					if (Subobject->GetOutermost() == GetTransientPackage()
						&& !Subobject->Rename(
							nullptr,
							TargetObject,
							REN_DontCreateRedirectors | REN_NonTransactional))
					{
						return false;
					}
				}
			}
			return true;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
			{
				if (!ReparentTransientInstancedSubobjects(TargetObject, *It, ValuePtr))
				{
					return false;
				}
			}
			return true;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				if (!ReparentTransientInstancedSubobjects(
					TargetObject,
					ArrayProperty->Inner,
					Helper.GetRawPtr(Index)))
				{
					return false;
				}
			}
			return true;
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (Helper.IsValidIndex(Index)
					&& !ReparentTransientInstancedSubobjects(
						TargetObject,
						SetProperty->ElementProp,
						Helper.GetElementPtr(Index)))
				{
					return false;
				}
			}
			return true;
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (!ReparentTransientInstancedSubobjects(
						TargetObject,
						MapProperty->KeyProp,
						Helper.GetPairPtr(Index))
					|| !ReparentTransientInstancedSubobjects(
						TargetObject,
						MapProperty->ValueProp,
						Helper.GetPairPtr(Index)))
				{
					return false;
				}
			}
		}
		return true;
	}

	UBlueprint* LoadBlueprint(
		const TSharedPtr<FJsonObject>& Params,
		FString& OutName,
		FString& OutError)
	{
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("blueprint"), OutName)
			|| OutName.IsEmpty())
		{
			OutError = TEXT("blueprint is required");
			return nullptr;
		}
		return MCPHelpers::LoadBlueprintByName(OutName, OutError);
	}

	UObject* ResolveCDOTarget(UObject* Asset)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
		{
			return Blueprint->GeneratedClass
				       ? Blueprint->GeneratedClass->GetDefaultObject()
				       : nullptr;
		}
		// Callers may bind the generated class path (for example, /Game/BP.BP_C)
		// instead of the Blueprint asset path.  Reflected properties belong to the
		// class default object, never to the UClass metadata object itself.
		if (UClass* Class = Cast<UClass>(Asset))
		{
			return Class->GetDefaultObject();
		}
		return Asset;
	}

	FProperty* FindCDOProperty(UObject* Target, const FString& PropertyName)
	{
		if (!Target || PropertyName.IsEmpty())
		{
			return nullptr;
		}
		if (FProperty* Property = Target->GetClass()->FindPropertyByName(FName(*PropertyName)))
		{
			return Property;
		}
		for (TFieldIterator<FProperty> It(
			     Target->GetClass(),
			     EFieldIteratorFlags::IncludeSuper,
			     EFieldIteratorFlags::ExcludeDeprecated); It; ++It)
		{
			if (It->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	bool ShouldSave(
		UBlueprint* Blueprint,
		const TSharedPtr<FJsonObject>& Params)
	{
		return Blueprint
			&& UEAIIntegration::Workflow::ShouldSaveImmediately(Params)
			&& MCPHelpers::CompileAndSaveBlueprintPackage(Blueprint);
	}

	UTimelineTemplate* FindTimeline(UBlueprint* Blueprint, const FString& Name)
	{
		if (!Blueprint) return nullptr;
		for (UTimelineTemplate* Timeline : Blueprint->Timelines)
		{
			if (Timeline && Timeline->GetVariableName().ToString().Equals(
				Name, ESearchCase::IgnoreCase))
			{
				return Timeline;
			}
		}
		return nullptr;
	}

	enum class EKeyInterpolation : uint8 { Linear, Constant, Cubic };

	struct FPendingCurveKey
	{
		float Time = 0.0f;
		float Value = 0.0f;
		EKeyInterpolation Interpolation = EKeyInterpolation::Linear;
	};

	bool ParseInterpolation(const TSharedPtr<FJsonObject>& Object, EKeyInterpolation& Out, FString& OutError)
	{
		if (!Object->HasField(TEXT("interpMode")))
		{
			Out = EKeyInterpolation::Linear;
			return true;
		}
		FString Text;
		if (!Object->TryGetStringField(TEXT("interpMode"), Text))
		{
			OutError = TEXT("interpMode must be a string: linear, constant, or cubic");
			return false;
		}
		Text.TrimStartAndEndInline();
		if (Text.Equals(TEXT("linear"), ESearchCase::IgnoreCase)) Out = EKeyInterpolation::Linear;
		else if (Text.Equals(TEXT("constant"), ESearchCase::IgnoreCase)) Out = EKeyInterpolation::Constant;
		else if (Text.Equals(TEXT("cubic"), ESearchCase::IgnoreCase)) Out = EKeyInterpolation::Cubic;
		else
		{
			OutError = TEXT("interpMode must be linear, constant, or cubic");
			return false;
		}
		return true;
	}

	bool ParseFiniteFloat(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, float& Out, FString& OutError)
	{
		double Number = 0.0;
		if (!Object->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number)
			|| !FMath::IsFinite(static_cast<float>(Number)))
		{
			OutError = FString::Printf(TEXT("%s must be a finite number representable as float"), Field);
			return false;
		}
		Out = static_cast<float>(Number);
		return true;
	}

	bool ParseCurveKeys(const TArray<TSharedPtr<FJsonValue>>& Keys, TArray<FPendingCurveKey>& Out, FString& OutError)
	{
		if (Keys.Num() > 4096)
		{
			OutError = TEXT("keys exceeds the 4096-key limit");
			return false;
		}
		Out.Reset();
		TSet<float> Times;
		for (const TSharedPtr<FJsonValue>& Value : Keys)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				OutError = TEXT("each key must be an object");
				return false;
			}
			FPendingCurveKey Pending;
			if (!ParseFiniteFloat(*Object, TEXT("time"), Pending.Time, OutError)
				|| !ParseFiniteFloat(*Object, TEXT("value"), Pending.Value, OutError)
				|| !ParseInterpolation(*Object, Pending.Interpolation, OutError))
				return false;
			const float TimeAsFloat = Pending.Time;
			if (Times.Contains(TimeAsFloat))
			{
				OutError = FString::Printf(TEXT("duplicate key time %.6g"), TimeAsFloat);
				return false;
			}
			Times.Add(TimeAsFloat);
			Out.Add(Pending);
		}
		return true;
	}

	void ApplyCurveKeys(FRichCurve& Curve, const TArray<FPendingCurveKey>& Keys)
	{
		Curve.Reset();
		for (const FPendingCurveKey& Key : Keys)
		{
			const FKeyHandle Handle = Curve.AddKey(Key.Time, Key.Value);
			Curve.SetKeyInterpMode(Handle, Key.Interpolation == EKeyInterpolation::Constant
				                               ? RCIM_Constant
				                               : Key.Interpolation == EKeyInterpolation::Cubic
				                               ? RCIM_Cubic
				                               : RCIM_Linear);
		}
	}

	bool ParseEventKeys(const TArray<TSharedPtr<FJsonValue>>& Keys, TArray<FPendingCurveKey>& Out, FString& OutError)
	{
		if (Keys.Num() > 4096)
		{
			OutError = TEXT("keys exceeds the 4096-key limit");
			return false;
		}
		Out.Reset();
		TSet<float> Times;
		for (const TSharedPtr<FJsonValue>& Value : Keys)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				OutError = TEXT("each event key must be an object");
				return false;
			}
			FPendingCurveKey Pending;
			if (!ParseFiniteFloat(*Object, TEXT("time"), Pending.Time, OutError)
				|| !ParseInterpolation(*Object, Pending.Interpolation, OutError))
				return false;
			if (Times.Contains(Pending.Time))
			{
				OutError = FString::Printf(TEXT("duplicate event key time %.6g"), Pending.Time);
				return false;
			}
			Times.Add(Pending.Time);
			Out.Add(Pending);
		}
		return true;
	}

	bool ParseComponentKeys(const TArray<TSharedPtr<FJsonValue>>& Keys, const TCHAR* const* ComponentNames,
	                        int32 ComponentCount, TArray<TArray<FPendingCurveKey>>& Out, FString& OutError)
	{
		if (Keys.Num() > 4096)
		{
			OutError = TEXT("keys exceeds the 4096-key limit");
			return false;
		}
		Out.SetNum(ComponentCount);
		for (TArray<FPendingCurveKey>& Component : Out) Component.Reset();
		TSet<float> Times;
		for (const TSharedPtr<FJsonValue>& Value : Keys)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
			{
				OutError = TEXT("each component key must be an object");
				return false;
			}
			FPendingCurveKey Base;
			if (!ParseFiniteFloat(*Object, TEXT("time"), Base.Time, OutError)
				|| !ParseInterpolation(*Object, Base.Interpolation, OutError))
				return false;
			if (Times.Contains(Base.Time))
			{
				OutError = FString::Printf(TEXT("duplicate key time %.6g"), Base.Time);
				return false;
			}
			Times.Add(Base.Time);
			for (int32 ComponentIndex = 0; ComponentIndex < ComponentCount; ++ComponentIndex)
			{
				FPendingCurveKey Pending = Base;
				if (!ParseFiniteFloat(*Object, ComponentNames[ComponentIndex], Pending.Value, OutError)) return false;
				Out[ComponentIndex].Add(Pending);
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> SerializeCurve(const FRichCurve& Curve)
	{
		TArray<TSharedPtr<FJsonValue>> Keys;
		for (const FRichCurveKey& Key : Curve.GetConstRefOfKeys())
		{
			TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
			KeyObject->SetNumberField(TEXT("time"), Key.Time);
			KeyObject->SetNumberField(TEXT("value"), Key.Value);
			switch (Key.InterpMode)
			{
			case RCIM_Constant:
				KeyObject->SetStringField(TEXT("interpMode"), TEXT("constant"));
				break;
			case RCIM_Cubic:
				KeyObject->SetStringField(TEXT("interpMode"), TEXT("cubic"));
				break;
			default:
				KeyObject->SetStringField(TEXT("interpMode"), TEXT("linear"));
				break;
			}
			Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("keys"), Keys);
		Result->SetNumberField(TEXT("numKeys"), Keys.Num());
		return Result;
	}

	TSharedRef<FJsonObject> SerializeTimeline(const UTimelineTemplate* Timeline)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), Timeline->GetVariableName().ToString());
		Result->SetStringField(TEXT("guid"), Timeline->TimelineGuid.ToString());
		Result->SetNumberField(TEXT("length"), Timeline->TimelineLength);
		Result->SetStringField(
			TEXT("lengthMode"),
			Timeline->LengthMode == ETimelineLengthMode::TL_LastKeyFrame
				? TEXT("lastKeyFrame")
				: TEXT("timelineLength"));
		Result->SetBoolField(TEXT("autoPlay"), Timeline->bAutoPlay);
		Result->SetBoolField(TEXT("loop"), Timeline->bLoop);
		Result->SetBoolField(TEXT("replicated"), Timeline->bReplicated);
		Result->SetBoolField(TEXT("ignoreTimeDilation"), Timeline->bIgnoreTimeDilation);
		Result->SetStringField(TEXT("directionProperty"), Timeline->GetDirectionPropertyName().ToString());
		Result->SetStringField(TEXT("updateFunction"), Timeline->GetUpdateFunctionName().ToString());
		Result->SetStringField(TEXT("finishedFunction"), Timeline->GetFinishedFunctionName().ToString());
		Result->SetNumberField(TEXT("tickGroup"), static_cast<int32>(Timeline->TimelineTickGroup));

		TArray<TSharedPtr<FJsonValue>> Metadata;
		for (const FBPVariableMetaDataEntry& Entry : Timeline->MetaDataArray)
		{
			TSharedRef<FJsonObject> MetadataEntry = MakeShared<FJsonObject>();
			MetadataEntry->SetStringField(TEXT("key"), Entry.DataKey.ToString());
			MetadataEntry->SetStringField(TEXT("value"), Entry.DataValue);
			Metadata.Add(MakeShared<FJsonValueObject>(MetadataEntry));
		}
		Result->SetArrayField(TEXT("metadata"), Metadata);

		TArray<TSharedPtr<FJsonValue>> FloatTracks;
		for (const FTTFloatTrack& Track : Timeline->FloatTracks)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("trackName"), Track.GetTrackName().ToString());
			Item->SetStringField(TEXT("trackType"), TEXT("float"));
			Item->SetStringField(TEXT("propertyName"), Track.GetPropertyName().ToString());
			Item->SetBoolField(TEXT("externalCurve"), Track.bIsExternalCurve);
			Item->SetStringField(TEXT("curvePath"), Track.CurveFloat ? Track.CurveFloat->GetPathName() : TEXT("None"));
			if (Track.CurveFloat)
			{
				const TSharedRef<FJsonObject> Curve = SerializeCurve(Track.CurveFloat->FloatCurve);
				Item->SetArrayField(TEXT("keys"), Curve->GetArrayField(TEXT("keys")));
				Item->SetNumberField(TEXT("numKeys"), Curve->GetNumberField(TEXT("numKeys")));
			}
			else
			{
				Item->SetArrayField(TEXT("keys"), {});
				Item->SetNumberField(TEXT("numKeys"), 0);
			}
			FloatTracks.Add(MakeShared<FJsonValueObject>(Item));
		}
		Result->SetArrayField(TEXT("floatTracks"), FloatTracks);

		TArray<TSharedPtr<FJsonValue>> VectorTracks;
		for (const FTTVectorTrack& Track : Timeline->VectorTracks)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("trackName"), Track.GetTrackName().ToString());
			Item->SetStringField(TEXT("trackType"), TEXT("vector"));
			Item->SetStringField(TEXT("propertyName"), Track.GetPropertyName().ToString());
			Item->SetBoolField(TEXT("externalCurve"), Track.bIsExternalCurve);
			Item->SetStringField(
				TEXT("curvePath"), Track.CurveVector ? Track.CurveVector->GetPathName() : TEXT("None"));
			TArray<TSharedPtr<FJsonValue>> Channels;
			if (Track.CurveVector)
			{
				static const TCHAR* Names[] = {TEXT("x"), TEXT("y"), TEXT("z")};
				for (int32 ChannelIndex = 0; ChannelIndex < 3; ++ChannelIndex)
				{
					TSharedRef<FJsonObject> Channel = MakeShared<FJsonObject>();
					Channel->SetStringField(TEXT("channel"), Names[ChannelIndex]);
					const TSharedRef<FJsonObject> Curve = SerializeCurve(Track.CurveVector->FloatCurves[ChannelIndex]);
					Channel->SetArrayField(TEXT("keys"), Curve->GetArrayField(TEXT("keys")));
					Channels.Add(MakeShared<FJsonValueObject>(Channel));
				}
			}
			Item->SetArrayField(TEXT("channels"), Channels);
			VectorTracks.Add(MakeShared<FJsonValueObject>(Item));
		}
		Result->SetArrayField(TEXT("vectorTracks"), VectorTracks);

		TArray<TSharedPtr<FJsonValue>> EventTracks;
		for (const FTTEventTrack& Track : Timeline->EventTracks)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("trackName"), Track.GetTrackName().ToString());
			Item->SetStringField(TEXT("trackType"), TEXT("event"));
			Item->SetStringField(TEXT("functionName"), Track.GetFunctionName().ToString());
			Item->SetBoolField(TEXT("externalCurve"), Track.bIsExternalCurve);
			Item->SetStringField(TEXT("curvePath"), Track.CurveKeys ? Track.CurveKeys->GetPathName() : TEXT("None"));
			TArray<TSharedPtr<FJsonValue>> Keys;
			if (Track.CurveKeys)
			{
				for (const FRichCurveKey& Key : Track.CurveKeys->FloatCurve.GetConstRefOfKeys())
				{
					TSharedRef<FJsonObject> KeyObject = MakeShared<FJsonObject>();
					KeyObject->SetNumberField(TEXT("time"), Key.Time);
					switch (Key.InterpMode)
					{
					case RCIM_Constant:
						KeyObject->SetStringField(TEXT("interpMode"), TEXT("constant"));
						break;
					case RCIM_Cubic:
						KeyObject->SetStringField(TEXT("interpMode"), TEXT("cubic"));
						break;
					default:
						KeyObject->SetStringField(TEXT("interpMode"), TEXT("linear"));
						break;
					}
					Keys.Add(MakeShared<FJsonValueObject>(KeyObject));
				}
			}
			Item->SetArrayField(TEXT("keys"), Keys);
			Item->SetNumberField(TEXT("numKeys"), Keys.Num());
			EventTracks.Add(MakeShared<FJsonValueObject>(Item));
		}
		Result->SetArrayField(TEXT("eventTracks"), EventTracks);
		TArray<TSharedPtr<FJsonValue>> ColorTracks;
		for (const FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("trackName"), Track.GetTrackName().ToString());
			Item->SetStringField(TEXT("trackType"), TEXT("color"));
			Item->SetStringField(TEXT("propertyName"), Track.GetPropertyName().ToString());
			Item->SetBoolField(TEXT("externalCurve"), Track.bIsExternalCurve);
			Item->SetStringField(
				TEXT("curvePath"), Track.CurveLinearColor ? Track.CurveLinearColor->GetPathName() : TEXT("None"));
			TArray<TSharedPtr<FJsonValue>> Channels;
			if (Track.CurveLinearColor)
			{
				static const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
				for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
				{
					TSharedRef<FJsonObject> Channel = MakeShared<FJsonObject>();
					Channel->SetStringField(TEXT("channel"), Names[ChannelIndex]);
					const TSharedRef<FJsonObject> Curve = SerializeCurve(
						Track.CurveLinearColor->FloatCurves[ChannelIndex]);
					Channel->SetArrayField(TEXT("keys"), Curve->GetArrayField(TEXT("keys")));
					Channels.Add(MakeShared<FJsonValueObject>(Channel));
				}
			}
			Item->SetArrayField(TEXT("channels"), Channels);
			ColorTracks.Add(MakeShared<FJsonValueObject>(Item));
		}
		Result->SetArrayField(TEXT("colorTracks"), ColorTracks);

		TArray<TSharedPtr<FJsonValue>> DisplayOrder;
		for (int32 DisplayIndex = 0; DisplayIndex < Timeline->GetNumDisplayTracks(); ++DisplayIndex)
		{
			const FTTTrackId TrackId = const_cast<UTimelineTemplate*>(Timeline)->GetDisplayTrackId(DisplayIndex);
			TSharedRef<FJsonObject> DisplayEntry = MakeShared<FJsonObject>();
			DisplayEntry->SetNumberField(TEXT("trackType"), TrackId.TrackType);
			DisplayEntry->SetNumberField(TEXT("trackIndex"), TrackId.TrackIndex);
			DisplayEntry->SetNumberField(TEXT("displayIndex"), DisplayIndex);
			DisplayOrder.Add(MakeShared<FJsonValueObject>(DisplayEntry));
		}
		Result->SetArrayField(TEXT("displayOrder"), DisplayOrder);
		return Result;
	}

	struct FTimelineTrackBinding
	{
		FString Type;
		bool bExternal = false;
		UObject* CurveObject = nullptr;
		TArray<FRichCurve*> Curves;
	};

	bool ResolveTrackBinding(UTimelineTemplate* Timeline, const FString& TrackName, FTimelineTrackBinding& Out)
	{
		if (!Timeline) return false;
		for (FTTFloatTrack& Track : Timeline->FloatTracks)
			if (Track.GetTrackName().ToString().Equals(TrackName, ESearchCase::IgnoreCase))
			{
				Out.Type = TEXT("float");
				Out.bExternal = Track.bIsExternalCurve;
				Out.CurveObject = Track.CurveFloat;
				if (Track.CurveFloat) Out.Curves.Add(&Track.CurveFloat->FloatCurve);
				return true;
			}
		for (FTTVectorTrack& Track : Timeline->VectorTracks)
			if (Track.GetTrackName().ToString().Equals(TrackName, ESearchCase::IgnoreCase))
			{
				Out.Type = TEXT("vector");
				Out.bExternal = Track.bIsExternalCurve;
				Out.CurveObject = Track.CurveVector;
				if (Track.CurveVector) for (FRichCurve& Curve : Track.CurveVector->FloatCurves) Out.Curves.Add(&Curve);
				return true;
			}
		for (FTTLinearColorTrack& Track : Timeline->LinearColorTracks)
			if (Track.GetTrackName().ToString().Equals(TrackName, ESearchCase::IgnoreCase))
			{
				Out.Type = TEXT("color");
				Out.bExternal = Track.bIsExternalCurve;
				Out.CurveObject = Track.CurveLinearColor;
				if (Track.CurveLinearColor)
					for (FRichCurve& Curve : Track.CurveLinearColor->FloatCurves)
						Out.Curves.
						    Add(&Curve);
				return true;
			}
		for (FTTEventTrack& Track : Timeline->EventTracks)
			if (Track.GetTrackName().ToString().Equals(TrackName, ESearchCase::IgnoreCase))
			{
				Out.Type = TEXT("event");
				Out.bExternal = Track.bIsExternalCurve;
				Out.CurveObject = Track.CurveKeys;
				if (Track.CurveKeys) Out.Curves.Add(&Track.CurveKeys->FloatCurve);
				return true;
			}
		return false;
	}

	bool ParseTrackKeyRequest(
		const TSharedPtr<FJsonObject>& Params,
		const FTimelineTrackBinding& Binding,
		TArray<TArray<FPendingCurveKey>>& Out,
		FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!Params->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
		{
			OutError = TEXT("keys must be an array");
			return false;
		}
		if (Binding.Type == TEXT("float") || Binding.Type == TEXT("event"))
		{
			TArray<FPendingCurveKey> Values;
			const bool bValid = Binding.Type == TEXT("float")
				                    ? ParseCurveKeys(*Keys, Values, OutError)
				                    : ParseEventKeys(*Keys, Values, OutError);
			if (!bValid) return false;
			Out.SetNum(1);
			Out[0] = MoveTemp(Values);
			return true;
		}
		const TCHAR* VectorNames[] = {TEXT("x"), TEXT("y"), TEXT("z")};
		const TCHAR* ColorNames[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
		const int32 Count = Binding.Type == TEXT("vector") ? 3 : 4;
		const TCHAR* const* Names = Binding.Type == TEXT("vector") ? VectorNames : ColorNames;
		return ParseComponentKeys(*Keys, Names, Count, Out, OutError);
	}

	FMCPToolResult RollbackAuthoringMutation(
		UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard& Guard,
		const TCHAR* Message)
	{
		FString RollbackError;
		if (!Guard.Rollback(RollbackError) && !RollbackError.IsEmpty())
			return FMCPToolResult::Error(RollbackError, TEXT("rollback_failed"), 500);
		return FMCPToolResult::Error(Message, TEXT("asset_save_failed"), 500);
	}

	TSharedPtr<FJsonValue> SerializePropertyValue(
		FProperty* Property,
		const void* ValuePtr,
		const UObject* Owner,
		int32 Depth,
		int32& Budget)
	{
		if (!Property || !ValuePtr || Depth > 8 || Budget-- <= 0)
			return MakeShared<FJsonValueNull>();
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 Value = ByteProperty->GetPropertyValue(ValuePtr);
			if (ByteProperty->Enum)
				return MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(Value));
			return MakeShared<FJsonValueNumber>(Value);
		}
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				// JSON numbers are IEEE-754 doubles. Preserve integer values outside the
				// exact 53-bit range as strings instead of silently changing the CDO.
				constexpr uint64 MaxExactJsonInteger = 9007199254740991ULL;
				const bool bUnsigned = CastField<FUInt16Property>(Property)
					|| CastField<FUInt32Property>(Property)
					|| CastField<FUInt64Property>(Property);
				if (bUnsigned)
				{
					const uint64 Value = NumericProperty->GetUnsignedIntPropertyValue(ValuePtr);
					if (Value <= MaxExactJsonInteger)
						return MakeShared<FJsonValueNumber>(static_cast<double>(Value));
					return MakeShared<FJsonValueString>(LexToString(Value));
				}
				const int64 Value = NumericProperty->GetSignedIntPropertyValue(ValuePtr);
				if (Value >= -static_cast<int64>(MaxExactJsonInteger)
					&& Value <= static_cast<int64>(MaxExactJsonInteger))
					return MakeShared<FJsonValueNumber>(static_cast<double>(Value));
				return MakeShared<FJsonValueString>(LexToString(Value));
			}
			if (NumericProperty->IsFloatingPoint())
				return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			FString Value;
			EnumProperty->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(Value);
		}
		if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
			return MakeShared<FJsonValueString>(StringProperty->GetPropertyValue(ValuePtr));
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
			return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(ObjectValue ? ObjectValue->GetPathName() : TEXT("None"));
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(StructProperty->Struct); It && Budget > 0; ++It)
			{
				FProperty* Child = *It;
				if (!Child || Child->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				Object->SetField(Child->GetName(), SerializePropertyValue(
					                 Child, Child->ContainerPtrToValuePtr<void>(ValuePtr), Owner, Depth + 1, Budget));
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			const int32 Count = FMath::Min(Helper.Num(), 4096);
			for (int32 Index = 0; Index < Count && Budget > 0; ++Index)
				Values.Add(SerializePropertyValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Owner, Depth + 1,
				                                  Budget));
			return MakeShared<FJsonValueArray>(Values);
		}
		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex() && Values.Num() < 4096 && Budget > 0; ++Index)
				if (Helper.IsValidIndex(Index))
					Values.Add(SerializePropertyValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Owner,
					                                  Depth + 1, Budget));
			return MakeShared<FJsonValueArray>(Values);
		}
		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			for (int32 Index = 0; Index < Helper.GetMaxIndex() && Object->Values.Num() < 4096 && Budget > 0; ++Index)
			{
				if (!Helper.IsValidIndex(Index)) continue;
				FString Key;
				MapProperty->KeyProp->ExportTextItem_Direct(Key, Helper.GetKeyPtr(Index), nullptr, nullptr, PPF_None);
				Object->SetField(Key, SerializePropertyValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), Owner,
				                                             Depth + 1, Budget));
			}
			return MakeShared<FJsonValueObject>(Object);
		}
		FString Exported;
		Property->ExportTextItem_Direct(Exported, ValuePtr, nullptr, const_cast<UObject*>(Owner), PPF_None);
		return MakeShared<FJsonValueString>(Exported);
	}

	bool ComputeCDOPropertyStateHash(
		const UObject* Target,
		const FProperty* Property,
		const FString& ExportedValue,
		FString& OutHash)
	{
		if (!Target || !Property)
		{
			return false;
		}
		const FString CanonicalState = FString::Printf(
			TEXT("object=%s\nclass=%s\nproperty=%s\nvalue=%s"),
			*Target->GetPathName(),
			*Target->GetClass()->GetPathName(),
			*Property->GetPathName(),
			*ExportedValue);
		const FTCHARToUTF8 Utf8(*CanonicalState);
		FString Digest;
		if (!UEAIIntegration::Infrastructure::TrySha256Hex(
			Utf8.Get(),
			static_cast<uint64>(Utf8.Length()),
			Digest))
		{
			return false;
		}
		OutHash = TEXT("sha256:") + Digest;
		return true;
	}

	bool IsValidCDOStateHash(const FString& Value)
	{
		if (Value.Len() != 71 || !Value.StartsWith(TEXT("sha256:"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 7; Index < Value.Len(); ++Index)
		{
			if (!FChar::IsHexDigit(Value[Index]))
			{
				return false;
			}
		}
		return true;
	}

	bool RestoreCDOPropertyValue(
		UObject* Target,
		FProperty* Property,
		const FString& ExportedValue)
	{
		if (!Target || !Property)
		{
			return false;
		}
		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
		FEditPropertyChain PropertyChain;
		PropertyChain.AddHead(Property);
		PropertyChain.SetActivePropertyNode(Property);
		Target->PreEditChange(PropertyChain);
		const TCHAR* ImportResult = Property->ImportText_Direct(*ExportedValue, ValuePtr, Target, PPF_None);
		if (!ImportResult)
		{
			return false;
		}
		while (*ImportResult && FChar::IsWhitespace(*ImportResult))
		{
			++ImportResult;
		}
		if (*ImportResult != TEXT('\0')
			|| !ReparentTransientInstancedSubobjects(Target, Property, Target))
		{
			return false;
		}
		FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
		FPropertyChangedChainEvent ChangedChainEvent(PropertyChain, ChangedEvent);
		Target->PostEditChangeChainProperty(ChangedChainEvent);
		FString Readback;
		Property->ExportText_Direct(Readback, ValuePtr, ValuePtr, Target, PPF_None);
		return Readback == ExportedValue;
	}

	class FTool_AddTimeline final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.timeline.add"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Name, Error;
			UBlueprint* Blueprint = LoadBlueprint(Params, Name, Error);
			if (!Blueprint) return FMCPToolResult::Error(Error);
			if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
				return FMCPToolResult::Error(
					TEXT("Blueprint type does not support timelines"), TEXT("unsupported_blueprint_type"), 422);

			FString TimelineName;
			if (Params->HasField(TEXT("timelineName"))
				&& !Params->TryGetStringField(TEXT("timelineName"), TimelineName))
				return FMCPToolResult::Error(TEXT("timelineName must be a string"), TEXT("invalid_params"), 422);
			if (TimelineName.IsEmpty())
				TimelineName = FBlueprintEditorUtils::FindUniqueTimelineName(Blueprint).
					ToString();
			if (FindTimeline(Blueprint, TimelineName))
				return FMCPToolResult::Error(FString::Printf(TEXT("Timeline '%s' already exists"), *TimelineName),
				                             TEXT("already_exists"), 409);
			UEdGraph* Graph = nullptr;
			FString GraphName;
			if (Params->HasField(TEXT("graph"))
				&& !Params->TryGetStringField(TEXT("graph"), GraphName))
				return FMCPToolResult::Error(TEXT("graph must be a string"), TEXT("invalid_params"), 422);
			for (UEdGraph* Candidate : Blueprint->UbergraphPages)
			{
				if (Candidate && (GraphName.IsEmpty() || Candidate->GetName().
				                                                    Equals(GraphName, ESearchCase::IgnoreCase)))
				{
					Graph = Candidate;
					break;
				}
			}
			if (!Graph)
				return FMCPToolResult::Error(
					TEXT("An event graph is required for a timeline"), TEXT("graph_not_found"), 404);
			UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(Blueprint);
			if (!Guard.IsValid())
				return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
			FScopedTransaction Transaction(
				NSLOCTEXT("UEAIIntegration", "AddTimeline", "Add Blueprint Timeline"));
			Guard.MarkMutationStarted();

			bool bAutoPlay = false, bLoop = false;
			if ((Params->HasField(TEXT("autoPlay"))
					&& !Params->TryGetBoolField(TEXT("autoPlay"), bAutoPlay))
				|| (Params->HasField(TEXT("loop")) && !Params->TryGetBoolField(TEXT("loop"), bLoop)))
				return FMCPToolResult::Error(TEXT("autoPlay and loop must be booleans"), TEXT("invalid_params"), 422);
			UTimelineTemplate* Timeline = FBlueprintEditorUtils::AddNewTimeline(Blueprint, FName(*TimelineName));
			if (!Timeline)
				return FMCPToolResult::Error(
					TEXT("Failed to create timeline template"), TEXT("create_failed"), 500);
			Timeline->Modify();
			Timeline->bAutoPlay = bAutoPlay;
			Timeline->bLoop = bLoop;
			Graph->Modify();
			UK2Node_Timeline* Node = NewObject<UK2Node_Timeline>(Graph, NAME_None, RF_Transactional);
			Node->Modify();
			Node->TimelineName = Timeline->GetVariableName();
			Node->TimelineGuid = Timeline->TimelineGuid;
			Node->CreateNewGuid();
			Node->bAutoPlay = bAutoPlay;
			Node->bLoop = bLoop;
			Node->NodePosX = 0;
			Node->NodePosY = 0;
			const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
			if (Params->TryGetArrayField(TEXT("position"), Position) && Position && Position->Num() == 2
				&& (*Position)[0].IsValid() && (*Position)[1].IsValid()
				&& (*Position)[0]->Type == EJson::Number && (*Position)[1]->Type == EJson::Number
				&& FMath::IsFinite((*Position)[0]->AsNumber()) && FMath::IsFinite((*Position)[1]->AsNumber()))
			{
				Node->NodePosX = static_cast<int32>((*Position)[0]->AsNumber());
				Node->NodePosY = static_cast<int32>((*Position)[1]->AsNumber());
			}
			Graph->AddNode(Node, true, false);
			Node->AllocateDefaultPins();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			const bool bSaveRequested = UEAIIntegration::Workflow::ShouldSaveImmediately(Params);
			const bool bSaved = ShouldSave(Blueprint, Params);

			if (bSaveRequested && !bSaved)

			{
				Transaction.Cancel();
				FString RollbackError;
				Guard.Rollback(RollbackError);
				return FMCPToolResult::Error(
					TEXT("Blueprint timeline operation could not be compiled and saved"), TEXT("asset_save_failed"),
					500);
			}
			Guard.Commit();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("timelineName"), TimelineName);
			Result->SetStringField(TEXT("timelineGuid"), Timeline->TimelineGuid.ToString());
			Result->SetStringField(TEXT("nodeId"), Node->NodeGuid.ToString());
			Result->SetStringField(TEXT("graph"), Graph->GetName());
			Result->SetBoolField(TEXT("saved"), bSaved);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_GetTimeline final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.timeline.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Name, Error;
			UBlueprint* Blueprint = LoadBlueprint(Params, Name, Error);
			if (!Blueprint) return FMCPToolResult::Error(Error);
			FString Filter;
			if (Params->HasField(TEXT("timelineName"))
				&& !Params->TryGetStringField(TEXT("timelineName"), Filter))
				return FMCPToolResult::Error(TEXT("timelineName must be a string"), TEXT("invalid_params"), 422);
			TArray<TSharedPtr<FJsonValue>> Values;
			bool bIncludeCurves = true;
			if (Params->HasField(TEXT("includeCurves"))
				&& !Params->TryGetBoolField(TEXT("includeCurves"), bIncludeCurves))
				return FMCPToolResult::Error(TEXT("includeCurves must be a boolean"), TEXT("invalid_params"), 422);
			for (const UTimelineTemplate* Timeline : Blueprint->Timelines)
			{
				if (Timeline && (Filter.IsEmpty() || Timeline->GetVariableName().ToString().Equals(
					Filter, ESearchCase::IgnoreCase)))
				{
					TSharedRef<FJsonObject> TimelineJson = SerializeTimeline(Timeline);
					if (!bIncludeCurves)
					{
						TimelineJson->RemoveField(TEXT("floatTracks"));
						TimelineJson->RemoveField(TEXT("vectorTracks"));
						TimelineJson->RemoveField(TEXT("colorTracks"));
						TimelineJson->RemoveField(TEXT("eventTracks"));
					}
					Values.Add(MakeShared<FJsonValueObject>(TimelineJson));
				}
			}
			if (!Filter.IsEmpty() && Values.IsEmpty())
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Timeline '%s' not found"), *Filter), TEXT("timeline_not_found"), 404);
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("timelines"), Values);
			Result->SetNumberField(TEXT("count"), Values.Num());
			Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_AddTimelineTrack final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.timeline.track.add"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Name, Error, TimelineName, TrackName, TrackType(TEXT("float"));
			UBlueprint* Blueprint = LoadBlueprint(Params, Name, Error);
			if (!Blueprint) return FMCPToolResult::Error(Error);
			if (!Params->TryGetStringField(TEXT("timelineName"), TimelineName) || TimelineName.IsEmpty()
				|| !Params->TryGetStringField(TEXT("trackName"), TrackName) || TrackName.IsEmpty())
				return FMCPToolResult::Error(
					TEXT("timelineName and trackName are required"), TEXT("invalid_params"), 422);
			if (Params->HasField(TEXT("trackType"))
				&& !Params->TryGetStringField(TEXT("trackType"), TrackType))
				return FMCPToolResult::Error(TEXT("trackType must be a string"), TEXT("invalid_params"), 422);
			UTimelineTemplate* Timeline = FindTimeline(Blueprint, TimelineName);
			if (!Timeline) return FMCPToolResult::Error(TEXT("Timeline not found"), TEXT("timeline_not_found"), 404);
			if (!Timeline->IsNewTrackNameValid(FName(*TrackName)))
				return FMCPToolResult::Error(
					TEXT("Track name already exists"), TEXT("already_exists"), 409);
			if (!Blueprint->GeneratedClass)
				return FMCPToolResult::Error(
					TEXT("Blueprint has no generated class; compile it first"), TEXT("generated_class_unavailable"),
					422);
			if (!TrackType.Equals(TEXT("float"), ESearchCase::IgnoreCase)
				&& !TrackType.Equals(TEXT("vector"), ESearchCase::IgnoreCase)
				&& !TrackType.Equals(TEXT("event"), ESearchCase::IgnoreCase)
				&& !TrackType.Equals(TEXT("color"), ESearchCase::IgnoreCase))
				return FMCPToolResult::Error(
					TEXT("trackType must be float, vector, event, or color"), TEXT("invalid_track_type"), 422);
			UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(Blueprint);
			if (!Guard.IsValid())
				return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
			FScopedTransaction Transaction(
				NSLOCTEXT("UEAIIntegration", "AddTimelineTrack", "Add Blueprint Timeline Track"));
			Guard.MarkMutationStarted();
			Timeline->Modify();
			if (TrackType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
			{
				const int32 TrackIndex = Timeline->FloatTracks.Num();
				FTTFloatTrack Track;
				Track.SetTrackName(FName(*TrackName), Timeline);
				Track.CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public);
				Timeline->FloatTracks.Add(Track);
				Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, TrackIndex));
			}
			else if (TrackType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
			{
				const int32 TrackIndex = Timeline->VectorTracks.Num();
				FTTVectorTrack Track;
				Track.SetTrackName(FName(*TrackName), Timeline);
				Track.CurveVector = NewObject<UCurveVector>(Blueprint->GeneratedClass, NAME_None, RF_Public);
				Timeline->VectorTracks.Add(Track);
				Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, TrackIndex));
			}
			else if (TrackType.Equals(TEXT("event"), ESearchCase::IgnoreCase))
			{
				const int32 TrackIndex = Timeline->EventTracks.Num();
				FTTEventTrack Track;
				Track.SetTrackName(FName(*TrackName), Timeline);
				Track.CurveKeys = NewObject<UCurveFloat>(Blueprint->GeneratedClass, NAME_None, RF_Public);
				Track.CurveKeys->bIsEventCurve = true;
				Timeline->EventTracks.Add(Track);
				Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, TrackIndex));
			}
			else if (TrackType.Equals(TEXT("color"), ESearchCase::IgnoreCase))
			{
				const int32 TrackIndex = Timeline->LinearColorTracks.Num();
				FTTLinearColorTrack Track;
				Track.SetTrackName(FName(*TrackName), Timeline);
				Track.CurveLinearColor = NewObject<UCurveLinearColor>(Blueprint->GeneratedClass, NAME_None, RF_Public);
				Timeline->LinearColorTracks.Add(Track);
				Timeline->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_LinearColorInterp, TrackIndex));
			}
			for (UEdGraph* CandidateGraph : Blueprint->UbergraphPages)
			{
				if (!CandidateGraph) continue;
				for (UEdGraphNode* CandidateNode : CandidateGraph->Nodes)
					if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(CandidateNode))
						if (TimelineNode->TimelineGuid == Timeline->TimelineGuid)
						{
							TimelineNode->Modify();
							TimelineNode->ReconstructNode();
						}
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			const bool bSaveRequested = UEAIIntegration::Workflow::ShouldSaveImmediately(Params);

			const bool bSaved = ShouldSave(Blueprint, Params);

			if (bSaveRequested && !bSaved)

			{
				Transaction.Cancel();
				FString RollbackError;
				Guard.Rollback(RollbackError);
				return FMCPToolResult::Error(
					TEXT("Blueprint timeline operation could not be compiled and saved"), TEXT("asset_save_failed"),
					500);
			}
			Guard.Commit();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("timelineName"), TimelineName);
			Result->SetStringField(TEXT("trackName"), TrackName);
			Result->SetStringField(TEXT("trackType"), TrackType.ToLower());
			Result->SetBoolField(TEXT("saved"), bSaved);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_SetTimelineKeys final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.timeline.keys.set"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Name, Error, TimelineName, TrackName;
			UBlueprint* Blueprint = LoadBlueprint(Params, Name, Error);
			if (!Blueprint)
			{
				return FMCPToolResult::Error(Error);
			}
			if (!Params->TryGetStringField(TEXT("timelineName"), TimelineName) || TimelineName.IsEmpty()
				|| !Params->TryGetStringField(TEXT("trackName"), TrackName) || TrackName.IsEmpty())
			{
				return FMCPToolResult::Error(
					TEXT("timelineName and trackName are required"), TEXT("invalid_params"), 422);
			}
			UTimelineTemplate* Timeline = FindTimeline(Blueprint, TimelineName);
			if (!Timeline)
			{
				return FMCPToolResult::Error(TEXT("Timeline not found"), TEXT("timeline_not_found"), 404);
			}

			FTimelineTrackBinding Binding;
			if (!ResolveTrackBinding(Timeline, TrackName, Binding))
			{
				return FMCPToolResult::Error(
					TEXT("Timeline track not found"), TEXT("track_not_found"), 404);
			}
			if (!Binding.CurveObject)
			{
				return FMCPToolResult::Error(
					TEXT("Track has no backing curve"), TEXT("curve_not_found"), 422);
			}
			if (Binding.bExternal || Binding.CurveObject->GetOutermost() != Blueprint->GetOutermost())
			{
				return FMCPToolResult::Error(
					TEXT("External curve assets cannot be changed through a Blueprint Timeline request"),
					TEXT("external_curve_unsupported"), 422);
			}

			TArray<TArray<FPendingCurveKey>> PendingChannels;
			if (!Params->HasField(TEXT("keys")))
			{
				return FMCPToolResult::Error(
					TEXT("keys is required"), TEXT("invalid_params"), 422);
			}
			if (!ParseTrackKeyRequest(Params, Binding, PendingChannels, Error))
			{
				return FMCPToolResult::Error(Error, TEXT("invalid_keys"), 422);
			}

			UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(Blueprint);
			if (!Guard.IsValid())
			{
				return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
			}
			FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "SetTimelineKeys", "Set Timeline Keys"));
			Guard.MarkMutationStarted();
			Blueprint->Modify();
			Timeline->Modify();
			Binding.CurveObject->Modify();
			for (int32 Index = 0; Index < Binding.Curves.Num(); ++Index)
			{
				ApplyCurveKeys(*Binding.Curves[Index], PendingChannels[Index]);
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

			const bool bSaveRequested = UEAIIntegration::Workflow::ShouldSaveImmediately(Params);
			const bool bSaved = ShouldSave(Blueprint, Params);
			if (bSaveRequested && !bSaved)
			{
				Transaction.Cancel();
				return RollbackAuthoringMutation(
					Guard, TEXT("Timeline key replacement could not be compiled and saved"));
			}
			Guard.Commit();
			int32 KeysSet = 0;
			for (const TArray<FPendingCurveKey>& Channel : PendingChannels)
			{
				KeysSet += Channel.Num();
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("timelineName"), TimelineName);
			Result->SetStringField(TEXT("trackName"), TrackName);
			Result->SetStringField(TEXT("trackType"), Binding.Type);
			Result->SetNumberField(TEXT("keysSet"), KeysSet);
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetObjectField(TEXT("timeline"), SerializeTimeline(FindTimeline(Blueprint, TimelineName)));
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_GetCDOProperties final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.cdo.properties.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Path;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset"), Path) || Path.IsEmpty())
				return
					FMCPToolResult::Error(TEXT("asset is required"), TEXT("invalid_params"), 422);
			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
			if (!Asset)
				return FMCPToolResult::Error(FString::Printf(TEXT("Asset not found: %s"), *Path),
				                             TEXT("asset_not_found"), 404);
			UObject* Target = ResolveCDOTarget(Asset);
			if (!Target)
				return FMCPToolResult::Error(
					TEXT("Blueprint has no generated class"), TEXT("generated_class_unavailable"), 422);
			FString Filter;
			if (Params->HasField(TEXT("category"))
				&& !Params->TryGetStringField(TEXT("category"), Filter))
				return FMCPToolResult::Error(TEXT("category must be a string"), TEXT("invalid_params"), 422);
			bool bIncludeParentDefaults = true;
			if (Params->HasField(TEXT("includeParentDefaults"))
				&& !Params->TryGetBoolField(TEXT("includeParentDefaults"), bIncludeParentDefaults))
				return FMCPToolResult::Error(
					TEXT("includeParentDefaults must be a boolean"), TEXT("invalid_params"), 422);
			int32 MaxProperties = 1024;
			if (Params->HasField(TEXT("maxProperties"))
				&& !Params->TryGetNumberField(TEXT("maxProperties"), MaxProperties))
				return FMCPToolResult::Error(TEXT("maxProperties must be a number"), TEXT("invalid_params"), 422);
			MaxProperties = FMath::Clamp(MaxProperties, 1, 4096);
			TArray<TSharedPtr<FJsonValue>> Properties;
			int32 TotalProperties = 0;
			bool bTruncated = false;
			for (TFieldIterator<FProperty> It(Target->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property
					|| Property->GetOwnerClass() == UObject::StaticClass()
					|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated))
				{
					continue;
				}
				if (!bIncludeParentDefaults && Property->GetOwnerClass() != Target->GetClass()) continue;
				const FString Category = Property->GetMetaData(TEXT("Category"));
				if (!Filter.IsEmpty() && !Category.Contains(Filter)) continue;
				++TotalProperties;
				if (Properties.Num() >= MaxProperties)
				{
					bTruncated = true;
					continue;
				}
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Property->GetName());
				Item->SetStringField(TEXT("type"), Property->GetCPPType());
				Item->SetStringField(TEXT("category"), Category);
				int32 Budget = 16384;
				Item->SetField(TEXT("value"), SerializePropertyValue(
					               Property, Property->ContainerPtrToValuePtr<void>(Target), Target, 0, Budget));
				Item->SetStringField(
					TEXT("ownerClass"),
					Property->GetOwnerClass() ? Property->GetOwnerClass()->GetPathName() : TEXT(""));
				Item->SetBoolField(TEXT("replicated"), Property->HasAnyPropertyFlags(CPF_Net));
				Item->SetBoolField(TEXT("editConst"), Property->HasAnyPropertyFlags(CPF_EditConst));
				FString ExportedValue;
				Property->ExportText_Direct(
					ExportedValue,
					Property->ContainerPtrToValuePtr<void>(Target),
					Property->ContainerPtrToValuePtr<void>(Target),
					Target,
					PPF_None);
				FString StateHash;
				if (ComputeCDOPropertyStateHash(Target, Property, ExportedValue, StateHash))
				{
					Item->SetStringField(TEXT("stateHash"), StateHash);
				}
				Properties.Add(MakeShared<FJsonValueObject>(Item));
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset"), Asset->GetPathName());
			Result->SetStringField(TEXT("class"), Target->GetClass()->GetPathName());
			Result->SetArrayField(TEXT("properties"), Properties);
			Result->SetNumberField(TEXT("propertyCount"), Properties.Num());
			Result->SetNumberField(TEXT("totalProperties"), TotalProperties);
			Result->SetBoolField(TEXT("truncated"), bTruncated);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_SetCDOProperty final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.cdo.property.set"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Path;
			FString PropertyName;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("asset"), Path) || Path.IsEmpty()
				|| !Params->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty()
				|| !Params->HasField(TEXT("value")))
			{
				return FMCPToolResult::Error(
					TEXT("asset, property and value are required."), TEXT("invalid_params"), 422);
			}

			UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *Path);
			if (!Asset)
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Asset not found: %s"), *Path), TEXT("asset_not_found"), 404);
			}
			UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
			UObject* Target = ResolveCDOTarget(Asset);
			if (!Target)
			{
				return FMCPToolResult::Error(
					TEXT("Blueprint has no generated class."), TEXT("generated_class_unavailable"), 422);
			}

			FProperty* Property = FindCDOProperty(Target, PropertyName);
			if (!Property)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Property '%s' was not found on %s."),
						*PropertyName,
						*Target->GetClass()->GetName()),
					TEXT("property_not_found"),
					404);
			}
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst)
				|| !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Property '%s' is not writable."), *Property->GetName()),
					TEXT("property_not_writable"),
					422);
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
			FString OldValue;
			Property->ExportText_Direct(OldValue, ValuePtr, ValuePtr, Target, PPF_None);
			FString StateHashBefore;
			if (!ComputeCDOPropertyStateHash(Target, Property, OldValue, StateHashBefore))
			{
				return FMCPToolResult::Error(
					TEXT("Could not compute the CDO property state hash."),
					TEXT("state_hash_unavailable"),
					500);
			}

			if (Params->HasField(TEXT("expectedOldValue")))
			{
				FString ExpectedOldValue;
				if (!Params->TryGetStringField(TEXT("expectedOldValue"), ExpectedOldValue))
				{
					return FMCPToolResult::Error(
						TEXT("expectedOldValue must be a string."), TEXT("invalid_params"), 422);
				}
				if (ExpectedOldValue.Len() > 65536)
				{
					return FMCPToolResult::Error(
						TEXT("expectedOldValue exceeds the 65536 character limit."),
						TEXT("invalid_params"),
						422);
				}
				if (ExpectedOldValue != OldValue)
				{
					return FMCPToolResult::Error(
						TEXT("The CDO property changed since it was read."),
						TEXT("property_state_conflict"),
						409);
				}
			}
			if (Params->HasField(TEXT("expectedStateHash")))
			{
				FString ExpectedStateHash;
				if (!Params->TryGetStringField(TEXT("expectedStateHash"), ExpectedStateHash)
					|| !IsValidCDOStateHash(ExpectedStateHash))
				{
					return FMCPToolResult::Error(
						TEXT("expectedStateHash must be a sha256: value with 64 hexadecimal digits."),
						TEXT("invalid_params"),
						422);
				}
				if (!ExpectedStateHash.Equals(StateHashBefore, ESearchCase::IgnoreCase))
				{
					return FMCPToolResult::Error(
						TEXT("The CDO property changed since it was read."),
						TEXT("property_state_conflict"),
						409);
				}
			}

			const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
			if (!JsonValue.IsValid())
			{
				return FMCPToolResult::Error(TEXT("value is invalid."), TEXT("invalid_params"), 422);
			}
			const bool bSaveRequested = UEAIIntegration::Workflow::ShouldSaveImmediately(Params);

			TUniquePtr<FCDOPropertyPersistenceBaseline> PersistenceBaseline;
			if (bSaveRequested)
			{
				PersistenceBaseline = MakeUnique<FCDOPropertyPersistenceBaseline>(
					Blueprint ? static_cast<UObject*>(Blueprint) : Target);
				if (!PersistenceBaseline->bValid)
				{
					return FMCPToolResult::Error(
						PersistenceBaseline->Error.IsEmpty()
							? TEXT("Could not capture the CDO package baseline before mutation.")
							: PersistenceBaseline->Error,
						TEXT("persistence_baseline_failed"),
						500);
				}
			}

			auto RestoreMutation = [&](FString& OutError) -> bool
			{
				UObject* CurrentTarget = ResolveCDOTarget(Asset);
				FProperty* CurrentProperty = FindCDOProperty(CurrentTarget, PropertyName);
				const bool bMemoryRestored = CurrentTarget
					&& CurrentProperty
					&& RestoreCDOPropertyValue(CurrentTarget, CurrentProperty, OldValue);
				bool bDiskRestored = true;
				if (PersistenceBaseline.IsValid())
				{
					bDiskRestored = PersistenceBaseline->Restore(OutError);
				}
				if (!bMemoryRestored)
				{
					OutError = TEXT("The current CDO could not be restored to its previous value.");
				}
				if (!bMemoryRestored || !bDiskRestored)
				{
					return false;
				}
				FString Readback;
				CurrentProperty->ExportText_Direct(
					Readback,
					CurrentProperty->ContainerPtrToValuePtr<void>(CurrentTarget),
					CurrentProperty->ContainerPtrToValuePtr<void>(CurrentTarget),
					CurrentTarget,
					PPF_None);
				FString ReadbackHash;
				if (Readback != OldValue
					|| !ComputeCDOPropertyStateHash(CurrentTarget, CurrentProperty, Readback, ReadbackHash)
					|| ReadbackHash != StateHashBefore)
				{
					OutError = TEXT("The restored CDO value or state hash did not match the original value.");
					return false;
				}
				return true;
			};

			Target->SetFlags(RF_Transactional);
			FScopedTransaction Transaction(NSLOCTEXT(
				"UEAIIntegration",
				"SetBlueprintCDOProperty",
				"Set Blueprint CDO Property"));
			Target->Modify();
			FEditPropertyChain PropertyChain;
			PropertyChain.AddHead(Property);
			PropertyChain.SetActivePropertyNode(Property);
			Target->PreEditChange(PropertyChain);

			bool bImported = false;
			if (JsonValue->Type == EJson::Null)
			{
				// Match the editor's clear-value behavior for nullable reflected
				// properties (object references, strings, containers, and structs).
				Property->ClearValue(ValuePtr);
				bImported = ReparentTransientInstancedSubobjects(Target, Property, Target);
			}
			else if (JsonValue->Type == EJson::Object || JsonValue->Type == EJson::Array)
			{
				bImported = FJsonObjectConverter::JsonValueToUProperty(JsonValue, Property, ValuePtr, 0, 0)
					&& ReparentTransientInstancedSubobjects(Target, Property, Target);
			}
			else
			{
				FString TextValue;
				if (JsonValue->Type == EJson::Boolean)
				{
					TextValue = JsonValue->AsBool() ? TEXT("true") : TEXT("false");
				}
				else if (JsonValue->Type == EJson::Number)
				{
					TextValue = FString::SanitizeFloat(JsonValue->AsNumber());
				}
				else if (JsonValue->Type == EJson::String)
				{
					TextValue = JsonValue->AsString();
				}
				if (!TextValue.IsEmpty() || JsonValue->Type == EJson::String)
				{
					const TCHAR* ImportResult = Property->ImportText_Direct(*TextValue, ValuePtr, Target, PPF_None);
					if (ImportResult)
					{
						while (*ImportResult && FChar::IsWhitespace(*ImportResult))
						{
							++ImportResult;
						}
						bImported = *ImportResult == TEXT('\0')
							&& ReparentTransientInstancedSubobjects(Target, Property, Target);
					}
				}
			}
			if (!bImported)
			{
				FString RollbackError;
				const bool bRestored = RestoreMutation(RollbackError);
				Transaction.Cancel();
				if (!bRestored)
				{
					return FMCPToolResult::Error(
						FString::Printf(
							TEXT("The rejected property value could not be rolled back: %s"), *RollbackError),
						TEXT("property_rollback_failed"),
						500);
				}
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Value was rejected for property '%s'."), *Property->GetName()),
					TEXT("property_value_invalid"),
					422);
			}

			FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
			FPropertyChangedChainEvent ChangedChainEvent(PropertyChain, ChangedEvent);
			Target->PostEditChangeChainProperty(ChangedChainEvent);
			if (Blueprint)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			}
			else
			{
				(void)Target->MarkPackageDirty();
			}

			FString NewValue;
			Property->ExportText_Direct(NewValue, ValuePtr, ValuePtr, Target, PPF_None);
			FString StateHash;
			if (!ComputeCDOPropertyStateHash(Target, Property, NewValue, StateHash))
			{
				FString RollbackError;
				const bool bRestored = RestoreMutation(RollbackError);
				Transaction.Cancel();
				return FMCPToolResult::Error(
					bRestored
						? TEXT("Could not compute the updated CDO property state hash.")
						: TEXT("The CDO property state hash failed and the old value could not be restored."),
					bRestored ? TEXT("state_hash_unavailable") : TEXT("property_rollback_failed"),
					500);
			}

			bool bSaved = false;
			if (bSaveRequested)
			{
				bSaved = Blueprint
					         ? MCPHelpers::CompileAndSaveBlueprintPackage(Blueprint)
					         : MCPHelpers::SaveGenericPackage(Target);
				if (!bSaved)
				{
					FString RollbackError;
					const bool bRestored = RestoreMutation(RollbackError);
					Transaction.Cancel();
					return FMCPToolResult::Error(
						bRestored
							? TEXT(
								"CDO property, package, and dirty state were restored after the asset could not be saved.")
							: FString::Printf(
								TEXT("The asset could not be saved and the CDO property could not be restored: %s"),
								*RollbackError),
						bRestored ? TEXT("asset_save_failed") : TEXT("property_rollback_failed"),
						500);
				}

				Target = ResolveCDOTarget(Asset);
				Property = FindCDOProperty(Target, PropertyName);
				if (!Target || !Property)
				{
					FString RollbackError;
					const bool bRestored = RestoreMutation(RollbackError);
					Transaction.Cancel();
					return FMCPToolResult::Error(
						bRestored
							? TEXT("The saved asset could not be re-resolved for CDO readback.")
							: FString::Printf(
								TEXT("The asset could not be re-resolved and rollback failed: %s"), *RollbackError),
						bRestored ? TEXT("asset_readback_failed") : TEXT("property_rollback_failed"),
						500);
				}
				ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
				FString PersistedValue;
				Property->ExportText_Direct(PersistedValue, ValuePtr, ValuePtr, Target, PPF_None);
				if (PersistedValue != NewValue)
				{
					FString RollbackError;
					const bool bRestored = RestoreMutation(RollbackError);
					Transaction.Cancel();
					return FMCPToolResult::Error(
						bRestored
							? TEXT("The saved CDO value did not match the requested value.")
							: FString::Printf(
								TEXT("The saved value did not match and rollback failed: %s"), *RollbackError),
						bRestored ? TEXT("asset_readback_mismatch") : TEXT("property_rollback_failed"),
						500);
				}
				NewValue = MoveTemp(PersistedValue);
				if (!ComputeCDOPropertyStateHash(Target, Property, NewValue, StateHash))
				{
					FString RollbackError;
					const bool bRestored = RestoreMutation(RollbackError);
					Transaction.Cancel();
					return FMCPToolResult::Error(
						bRestored
							? TEXT("The saved CDO property state hash could not be computed.")
							: FString::Printf(
								TEXT("The saved state hash failed and rollback failed: %s"), *RollbackError),
						bRestored ? TEXT("state_hash_unavailable") : TEXT("property_rollback_failed"),
						500);
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("asset"), Asset->GetPathName());
			Result->SetStringField(TEXT("property"), Property->GetName());
			Result->SetStringField(TEXT("oldValue"), OldValue);
			Result->SetStringField(TEXT("newValue"), NewValue);
			Result->SetStringField(TEXT("stateHashBefore"), StateHashBefore);
			Result->SetStringField(TEXT("stateHash"), StateHash);
			Result->SetBoolField(TEXT("saveRequested"), bSaveRequested);
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetBoolField(TEXT("changed"), OldValue != NewValue);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_RemoveDispatcher final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.dispatcher.remove"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Name, Error, Dispatcher;
			UBlueprint* Blueprint = LoadBlueprint(Params, Name, Error);
			if (!Blueprint) return FMCPToolResult::Error(Error);
			if (!Params->TryGetStringField(TEXT("dispatcherName"), Dispatcher) || Dispatcher.IsEmpty())
				return
					FMCPToolResult::Error(TEXT("dispatcherName is required"), TEXT("invalid_params"), 422);
			UEdGraph* Signature = nullptr;
			for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
			{
				if (!Graph) continue;
				FString GraphName = Graph->GetName();
				if (GraphName.EndsWith(TEXT("_Signature"))) GraphName.LeftChopInline(10, EAllowShrinking::No);
				if (GraphName.Equals(Dispatcher, ESearchCase::IgnoreCase))
				{
					Signature = Graph;
					break;
				}
			}
			if (!Signature)
				return FMCPToolResult::Error(
					TEXT("Event dispatcher not found"), TEXT("dispatcher_not_found"), 404);
			UEAIIntegration::Infrastructure::FBlueprintSingleRequestMutationGuard Guard(Blueprint);
			if (!Guard.IsValid())
				return FMCPToolResult::Error(Guard.GetErrorMessage(), Guard.GetErrorCode(), 422);
			FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "RemoveDispatcher", "Remove Event Dispatcher"));
			Guard.MarkMutationStarted();
			Blueprint->Modify();
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Signature, EGraphRemoveFlags::Recompile);
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*Dispatcher));
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			const bool bSaveRequested = UEAIIntegration::Workflow::ShouldSaveImmediately(Params);

			const bool bSaved = ShouldSave(Blueprint, Params);

			if (bSaveRequested && !bSaved)

			{
				Transaction.Cancel();
				return RollbackAuthoringMutation(
					Guard, TEXT("Blueprint dispatcher operation could not be compiled and saved"));
			}
			Guard.Commit();
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), true);
			Result->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
			Result->SetStringField(TEXT("dispatcherName"), Dispatcher);
			Result->SetBoolField(TEXT("saved"), bSaved);
			return FMCPToolResult::Ok(Result);
		}
	};
} // namespace

namespace UEAIIntegrationTools
{
	void RegisterBlueprintAuthoringOperationsTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_AddTimeline>());
		Registry.Register(MakeShared<FTool_GetTimeline>());
		Registry.Register(MakeShared<FTool_AddTimelineTrack>());
		Registry.Register(MakeShared<FTool_SetTimelineKeys>());
		Registry.Register(MakeShared<FTool_GetCDOProperties>());
		Registry.Register(MakeShared<FTool_SetCDOProperty>());
		Registry.Register(MakeShared<FTool_RemoveDispatcher>());
	}
}
