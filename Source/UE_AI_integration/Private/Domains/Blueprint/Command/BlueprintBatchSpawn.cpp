#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Containers/Set.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	constexpr int32 MaxBatchActors = 1000;

	bool ReadVector(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		FVector& OutValue,
		FString& OutError)
	{
		if (!Params->HasField(Field))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid())
		{
			OutError = FString::Printf(TEXT("%s must be a 3-element array or object."), Field);
			return false;
		}

		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			if (Array.Num() != 3)
			{
				OutError = FString::Printf(TEXT("%s must contain exactly 3 numbers."), Field);
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Item : Array)
			{
				if (!Item.IsValid() || Item->Type != EJson::Number || !FMath::IsFinite(Item->AsNumber()))
				{
					OutError = FString::Printf(TEXT("%s must contain only finite numbers."), Field);
					return false;
				}
			}
			OutValue = FVector(
				static_cast<float>(Array[0]->AsNumber()),
				static_cast<float>(Array[1]->AsNumber()),
				static_cast<float>(Array[2]->AsNumber()));
			return true;
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			double X = 0.0;
			double Y = 0.0;
			double Z = 0.0;
			if (!Object.IsValid()
				|| !Object->TryGetNumberField(TEXT("x"), X)
				|| !Object->TryGetNumberField(TEXT("y"), Y)
				|| !Object->TryGetNumberField(TEXT("z"), Z)
				|| !FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z))
			{
				OutError = FString::Printf(TEXT("%s must contain finite x, y, and z numbers."), Field);
				return false;
			}
			OutValue = FVector(X, Y, Z);
			return true;
		}

		OutError = FString::Printf(TEXT("%s must be a 3-element array or object."), Field);
		return false;
	}

	bool ReadRotator(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		FRotator& OutValue,
		FString& OutError)
	{
		if (!Params->HasField(Field))
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
		if (!Value.IsValid())
		{
			OutError = FString::Printf(TEXT("%s must be a 3-element array or object."), Field);
			return false;
		}
		double Pitch = 0.0;
		double Yaw = 0.0;
		double Roll = 0.0;
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			if (Array.Num() != 3
				|| !Array[0].IsValid() || !Array[1].IsValid() || !Array[2].IsValid()
				|| Array[0]->Type != EJson::Number || Array[1]->Type != EJson::Number || Array[2]->Type !=
				EJson::Number)
			{
				OutError = FString::Printf(TEXT("%s must contain exactly 3 numbers."), Field);
				return false;
			}
			Pitch = Array[0]->AsNumber();
			Yaw = Array[1]->AsNumber();
			Roll = Array[2]->AsNumber();
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid()
				|| !Object->TryGetNumberField(TEXT("pitch"), Pitch)
				|| !Object->TryGetNumberField(TEXT("yaw"), Yaw)
				|| !Object->TryGetNumberField(TEXT("roll"), Roll))
			{
				OutError = FString::Printf(TEXT("%s must contain pitch, yaw, and roll numbers."), Field);
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("%s must be a 3-element array or object."), Field);
			return false;
		}
		if (!FMath::IsFinite(Pitch) || !FMath::IsFinite(Yaw) || !FMath::IsFinite(Roll))
		{
			OutError = FString::Printf(TEXT("%s must contain finite numbers."), Field);
			return false;
		}
		OutValue = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	UClass* ResolveBlueprintClass(const FString& BlueprintPath)
	{
		FString ClassPath = BlueprintPath;
		if (ClassPath.EndsWith(TEXT("_C")))
		{
			return StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
		}
		if (!ClassPath.Contains(TEXT(".")))
		{
			const FString AssetName = FPaths::GetBaseFilename(ClassPath);
			ClassPath += TEXT(".") + AssetName + TEXT("_C");
		}
		UClass* Class = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath);
		if (Class)
		{
			return Class;
		}
		if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *BlueprintPath))
		{
			return Blueprint->GeneratedClass;
		}
		FAssetData Asset = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
		                   .Get().GetAssetByObjectPath(FSoftObjectPath(BlueprintPath));
		UBlueprint* Blueprint = Asset.IsValid() ? Cast<UBlueprint>(Asset.GetAsset()) : nullptr;
		return Blueprint ? Blueprint->GeneratedClass : nullptr;
	}

	ULevel* ResolveTargetLevel(UWorld* World, const FString& Sublevel, FString& OutError)
	{
		if (Sublevel.IsEmpty())
		{
			return World->PersistentLevel;
		}
		for (ULevelStreaming* Streaming : World->GetStreamingLevels())
		{
			if (Streaming && Streaming->GetWorldAssetPackageFName().ToString().Contains(Sublevel))
			{
				if (ULevel* Loaded = Streaming->GetLoadedLevel())
				{
					return Loaded;
				}
			}
		}
		OutError = FString::Printf(TEXT("Sublevel '%s' was not found or is not loaded."), *Sublevel);
		return nullptr;
	}

	void ApplyMetadata(AActor* Actor, const TSharedPtr<FJsonObject>& Params, int32 Index)
	{
		FString Prefix;
		const bool bHasPrefix = Params->TryGetStringField(TEXT("labelPrefix"), Prefix)
			|| Params->TryGetStringField(TEXT("label_prefix"), Prefix);
		if (bHasPrefix && !Prefix.IsEmpty())
		{
			Actor->SetActorLabel(FString::Printf(TEXT("%s_%d"), *Prefix, Index));
		}
		FString Folder;
		if (Params->TryGetStringField(TEXT("folder"), Folder) && !Folder.IsEmpty())
		{
			Actor->SetFolderPath(FName(*Folder));
		}
		const TArray<TSharedPtr<FJsonValue>>* TagValues = nullptr;
		if (Params->TryGetArrayField(TEXT("tags"), TagValues) && TagValues)
		{
			for (const TSharedPtr<FJsonValue>& TagValue : *TagValues)
			{
				FString Tag;
				if (TagValue.IsValid() && TagValue->TryGetString(Tag) && !Tag.IsEmpty())
				{
					Actor->Tags.AddUnique(FName(*Tag));
				}
			}
		}
		FString Mobility;
		if (Params->TryGetStringField(TEXT("mobility"), Mobility) && Actor->GetRootComponent())
		{
			EComponentMobility::Type MobilityType;
			if (Mobility.Equals(TEXT("static"), ESearchCase::IgnoreCase)) MobilityType = EComponentMobility::Static;
			else if (Mobility.Equals(TEXT("stationary"), ESearchCase::IgnoreCase))
				MobilityType =
					EComponentMobility::Stationary;
			else if (Mobility.Equals(TEXT("movable"), ESearchCase::IgnoreCase))
				MobilityType =
					EComponentMobility::Movable;
			else return;
			Actor->GetRootComponent()->SetMobility(MobilityType);
		}
	}

	struct FBatchPropertyApplicationResult
	{
		TArray<FString> PropertiesSet;
		TArray<TSharedPtr<FJsonValue>> PropertiesFailed;
		TArray<TSharedPtr<FJsonValue>> Readback;

		bool HasFailures() const
		{
			return PropertiesFailed.Num() > 0;
		}
	};

	struct FResolvedBatchProperty
	{
		FProperty* Property = nullptr;
		UObject* Container = nullptr;
		FString Target;
	};

	void AddPropertyFailure(
		FBatchPropertyApplicationResult& Result,
		const FString& RequestedName,
		const TCHAR* Reason,
		const FString& Target = FString(),
		const FString& Detail = FString())
	{
		TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
		Failure->SetStringField(TEXT("property"), RequestedName);
		Failure->SetStringField(TEXT("reason"), Reason);
		if (!Target.IsEmpty())
		{
			Failure->SetStringField(TEXT("target"), Target);
		}
		if (!Detail.IsEmpty())
		{
			Failure->SetStringField(TEXT("detail"), Detail);
		}
		Result.PropertiesFailed.Add(MakeShared<FJsonValueObject>(Failure));
	}

	bool TryJsonScalarToImportText(const TSharedPtr<FJsonValue>& Value, FString& OutText)
	{
		if (!Value.IsValid())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Boolean:
			OutText = Value->AsBool() ? TEXT("True") : TEXT("False");
			return true;
		case EJson::Number:
			if (!FMath::IsFinite(Value->AsNumber()))
			{
				return false;
			}
			OutText = FString::SanitizeFloat(Value->AsNumber());
			return true;
		case EJson::String:
			OutText = Value->AsString();
			return true;
		default:
			return false;
		}
	}

	void SortActorComponents(TArray<UActorComponent*>& Components)
	{
		Components.RemoveAll([](const UActorComponent* Component)
		{
			return !IsValid(Component);
		});
		Components.Sort([](const UActorComponent& Left, const UActorComponent& Right)
		{
			const FString LeftPath = Left.GetPathName();
			const FString RightPath = Right.GetPathName();
			return LeftPath == RightPath
				       ? Left.GetName() < Right.GetName()
				       : LeftPath < RightPath;
		});
	}

	bool SplitComponentPropertyName(
		const FString& RequestedName,
		FString& OutComponentName,
		FString& OutPropertyName)
	{
		int32 Separator = INDEX_NONE;
		int32 SeparatorLength = 0;
		for (const TCHAR Candidate : {TEXT('.'), TEXT('/')})
		{
			int32 CandidateIndex = INDEX_NONE;
			if (RequestedName.FindChar(Candidate, CandidateIndex)
				&& CandidateIndex > 0
				&& (Separator == INDEX_NONE || CandidateIndex < Separator))
			{
				Separator = CandidateIndex;
				SeparatorLength = 1;
			}
		}
		const int32 ScopeSeparator = RequestedName.Find(TEXT("::"), ESearchCase::CaseSensitive);
		if (ScopeSeparator > 0 && (Separator == INDEX_NONE || ScopeSeparator < Separator))
		{
			Separator = ScopeSeparator;
			SeparatorLength = 2;
		}
		if (Separator <= 0 || Separator + SeparatorLength >= RequestedName.Len())
		{
			return false;
		}
		OutComponentName = RequestedName.Left(Separator);
		OutPropertyName = RequestedName.Mid(Separator + SeparatorLength);
		return !OutComponentName.IsEmpty() && !OutPropertyName.IsEmpty();
	}

	UActorComponent* FindComponentByStableName(
		const TArray<UActorComponent*>& Components,
		const FString& RequestedName,
		bool& bOutAmbiguous)
	{
		bOutAmbiguous = false;
		UActorComponent* ExactMatch = nullptr;
		for (UActorComponent* Component : Components)
		{
			if (Component->GetName().Equals(RequestedName, ESearchCase::CaseSensitive))
			{
				ExactMatch = Component;
				break;
			}
		}
		if (ExactMatch)
		{
			return ExactMatch;
		}

		UActorComponent* CaseInsensitiveMatch = nullptr;
		for (UActorComponent* Component : Components)
		{
			if (!Component->GetName().Equals(RequestedName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (CaseInsensitiveMatch)
			{
				bOutAmbiguous = true;
				return nullptr;
			}
			CaseInsensitiveMatch = Component;
		}
		return CaseInsensitiveMatch;
	}

	bool ResolveBatchProperty(
		AActor* Actor,
		const FString& RequestedName,
		FResolvedBatchProperty& OutResolved,
		FString& OutFailureReason,
		FString& OutFailureDetail)
	{
		if (!Actor || RequestedName.IsEmpty())
		{
			OutFailureReason = TEXT("property_not_found");
			return false;
		}

		// Actor properties always win, including names that happen to contain a
		// component separator. This preserves the historical actor-first contract.
		if (FProperty* ActorProperty = Actor->GetClass()->FindPropertyByName(FName(*RequestedName)))
		{
			OutResolved.Property = ActorProperty;
			OutResolved.Container = Actor;
			OutResolved.Target = Actor->GetPathName();
			return true;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		SortActorComponents(Components);
		FString RequestedComponentName;
		FString RequestedPropertyName;
		const bool bQualified = SplitComponentPropertyName(
			RequestedName,
			RequestedComponentName,
			RequestedPropertyName);

		if (bQualified)
		{
			bool bAmbiguous = false;
			UActorComponent* Component = FindComponentByStableName(
				Components,
				RequestedComponentName,
				bAmbiguous);
			if (bAmbiguous)
			{
				OutFailureReason = TEXT("component_ambiguous");
				OutFailureDetail = RequestedComponentName;
				return false;
			}
			if (!Component)
			{
				OutFailureReason = TEXT("component_not_found");
				OutFailureDetail = RequestedComponentName;
				return false;
			}
			FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*RequestedPropertyName));
			if (!Property)
			{
				OutFailureReason = TEXT("property_not_found");
				OutFailureDetail = Component->GetPathName();
				return false;
			}
			OutResolved.Property = Property;
			OutResolved.Container = Component;
			OutResolved.Target = Component->GetPathName();
			return true;
		}

		// For an unqualified component property, use the stable component path
		// order instead of UObject iteration order. The resolved target is returned
		// so callers can disambiguate the request in readback.
		for (UActorComponent* Component : Components)
		{
			if (FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*RequestedName)))
			{
				OutResolved.Property = Property;
				OutResolved.Container = Component;
				OutResolved.Target = Component->GetPathName();
				return true;
			}
		}
		OutFailureReason = TEXT("property_not_found");
		OutFailureDetail = Actor->GetClass()->GetPathName();
		return false;
	}

	bool ExportBatchProperty(
		const FResolvedBatchProperty& Resolved,
		FString& OutValue)
	{
		if (!Resolved.Property || !Resolved.Container)
		{
			return false;
		}
		void* ValuePtr = Resolved.Property->ContainerPtrToValuePtr<void>(Resolved.Container);
		Resolved.Property->ExportText_Direct(
			OutValue,
			ValuePtr,
			ValuePtr,
			Resolved.Container,
			PPF_None);
		return true;
	}

	bool ImportBatchProperty(
		const FResolvedBatchProperty& Resolved,
		const FString& ImportText,
		FString& OutReadback,
		FString& OutFailureReason,
		FString& OutFailureDetail)
	{
		if (!Resolved.Property || !Resolved.Container)
		{
			OutFailureReason = TEXT("property_not_found");
			return false;
		}
		void* ValuePtr = Resolved.Property->ContainerPtrToValuePtr<void>(Resolved.Container);
		FString OldValue;
		Resolved.Property->ExportText_Direct(
			OldValue,
			ValuePtr,
			ValuePtr,
			Resolved.Container,
			PPF_None);

		Resolved.Container->Modify();
		Resolved.Container->PreEditChange(Resolved.Property);
		const TCHAR* ImportResult = Resolved.Property->ImportText_Direct(
			*ImportText,
			ValuePtr,
			Resolved.Container,
			PPF_None);
		const TCHAR* Trailing = ImportResult;
		while (Trailing && *Trailing && FChar::IsWhitespace(*Trailing))
		{
			++Trailing;
		}
		const bool bImported = ImportResult && *Trailing == TEXT('\0');
		if (!bImported)
		{
			if (ImportResult && *Trailing != TEXT('\0'))
			{
				OutFailureReason = TEXT("import_trailing_text");
				OutFailureDetail = FString::Printf(
					TEXT("trailing text begins with '%s'"),
					*FString(Trailing).Left(128));
			}
			else
			{
				OutFailureReason = TEXT("import_failed");
			}

			// ImportText_Direct is allowed to write before returning a trailing
			// pointer. Restore the original value before exposing the failure.
			const TCHAR* RestoreResult = Resolved.Property->ImportText_Direct(
				*OldValue,
				ValuePtr,
				Resolved.Container,
				PPF_None);
			const TCHAR* RestoreTrailing = RestoreResult;
			while (RestoreTrailing && *RestoreTrailing && FChar::IsWhitespace(*RestoreTrailing))
			{
				++RestoreTrailing;
			}
			if (!RestoreResult || *RestoreTrailing != TEXT('\0'))
			{
				OutFailureDetail += OutFailureDetail.IsEmpty()
					? TEXT("original value could not be restored")
					: TEXT("; original value could not be restored");
			}
			FPropertyChangedEvent RestoredEvent(
				Resolved.Property,
				EPropertyChangeType::ValueSet);
			Resolved.Container->PostEditChangeProperty(RestoredEvent);
			return false;
		}

		FPropertyChangedEvent ChangedEvent(
			Resolved.Property,
			EPropertyChangeType::ValueSet);
		Resolved.Container->PostEditChangeProperty(ChangedEvent);
		if (!ExportBatchProperty(Resolved, OutReadback))
		{
			OutFailureReason = TEXT("readback_failed");
			return false;
		}
		return true;
	}

	void ApplyProperties(
		AActor* Actor,
		const TSharedPtr<FJsonObject>& Properties,
		FBatchPropertyApplicationResult& OutResult)
	{
		if (!Actor || !Properties.IsValid())
		{
			return;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			FResolvedBatchProperty Resolved;
			FString FailureReason;
			FString FailureDetail;
			if (!ResolveBatchProperty(
					Actor,
					Pair.Key,
					Resolved,
					FailureReason,
					FailureDetail))
			{
				AddPropertyFailure(
					OutResult,
					Pair.Key,
					*FailureReason,
					Actor->GetPathName(),
					FailureDetail);
				continue;
			}

			FString ImportText;
			if (!TryJsonScalarToImportText(Pair.Value, ImportText))
			{
				AddPropertyFailure(
					OutResult,
					Pair.Key,
					TEXT("property_value_must_be_scalar"),
					Resolved.Target);
				continue;
			}

			FString ReadbackValue;
			if (!ImportBatchProperty(
					Resolved,
					ImportText,
					ReadbackValue,
					FailureReason,
					FailureDetail))
			{
				AddPropertyFailure(
					OutResult,
					Pair.Key,
					*FailureReason,
					Resolved.Target,
					FailureDetail);
				continue;
			}

			OutResult.PropertiesSet.Add(Pair.Key);
			TSharedRef<FJsonObject> Readback = MakeShared<FJsonObject>();
			Readback->SetStringField(TEXT("property"), Pair.Key);
			Readback->SetStringField(TEXT("target"), Resolved.Target);
			Readback->SetStringField(TEXT("requested"), ImportText);
			Readback->SetStringField(TEXT("value"), ReadbackValue);
			OutResult.Readback.Add(MakeShared<FJsonValueObject>(Readback));
		}
	}

	class FTool_BatchSpawnBlueprintActors final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("blueprint.actor.batch_spawn"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid())
				return FMCPToolResult::Error(
					TEXT("Parameters are required."), TEXT("invalid_params"), 422);
			FString BlueprintPath;
			int32 Count = 0;
			double CountNumber = 0.0;
			if (!Params->TryGetStringField(TEXT("blueprint"), BlueprintPath) || BlueprintPath.IsEmpty()
				|| !Params->TryGetNumberField(TEXT("count"), CountNumber)
				|| !FMath::IsFinite(CountNumber) || CountNumber < 1.0 || CountNumber > MaxBatchActors
				|| FMath::Abs(CountNumber - FMath::RoundToDouble(CountNumber)) > KINDA_SMALL_NUMBER)
			{
				return FMCPToolResult::Error(
					TEXT("blueprint and integer count (1..1000) are required."), TEXT("invalid_params"), 422);
			}
			Count = FMath::RoundToInt(CountNumber);
			UClass* BlueprintClass = ResolveBlueprintClass(BlueprintPath);
			if (!BlueprintClass)
				return FMCPToolResult::Error(
					TEXT("Blueprint class could not be resolved."), TEXT("blueprint_not_found"), 404);
			if (BlueprintClass->HasAnyClassFlags(CLASS_Abstract))
				return FMCPToolResult::Error(
					TEXT("Blueprint class is abstract."), TEXT("class_not_spawnable"), 422);
			if (!GEditor) return FMCPToolResult::Error(TEXT("Editor is unavailable."), TEXT("editor_unavailable"), 503);
			if (GEditor->IsPlayingSessionInEditor())
				return FMCPToolResult::Error(
					TEXT("Blueprint actor spawning is unavailable during Play-In-Editor."),
					TEXT("pie_active"),
					409);
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) return FMCPToolResult::Error(TEXT("No editor world is open."), TEXT("world_unavailable"), 503);

			FString Error;
			FString Sublevel;
			if (Params->HasField(TEXT("sublevel")) && !Params->TryGetStringField(TEXT("sublevel"), Sublevel))
				return FMCPToolResult::Error(TEXT("sublevel must be a string."), TEXT("invalid_params"), 422);
			ULevel* TargetLevel = ResolveTargetLevel(World, Sublevel, Error);
			if (!TargetLevel) return FMCPToolResult::Error(Error, TEXT("sublevel_not_found"), 404);
			FString Pattern = TEXT("grid");
			if (Params->HasField(TEXT("pattern")) && !Params->TryGetStringField(TEXT("pattern"), Pattern))
				return FMCPToolResult::Error(TEXT("pattern must be a string."), TEXT("invalid_params"), 422);
			if (!Pattern.Equals(TEXT("grid"), ESearchCase::IgnoreCase) && !Pattern.Equals(
				TEXT("linear"), ESearchCase::IgnoreCase))
				return FMCPToolResult::Error(TEXT("pattern must be grid or linear."), TEXT("invalid_params"), 422);
			FVector Origin = FVector::ZeroVector;
			FVector Direction = FVector::ForwardVector;
			FVector Scale = FVector::OneVector;
			FRotator Rotation = FRotator::ZeroRotator;
			if (!ReadVector(Params, TEXT("origin"), Origin, Error) || !ReadVector(
					Params, TEXT("direction"), Direction, Error)
				|| !ReadVector(Params, TEXT("scale"), Scale, Error) || !ReadRotator(
					Params, TEXT("rotation"), Rotation, Error))
				return FMCPToolResult::Error(Error, TEXT("invalid_params"), 422);
			if (Direction.IsNearlyZero()) Direction = FVector::ForwardVector;
			Direction.Normalize();
			double Spacing = 200.0;
			if (Params->HasField(TEXT("spacing")) && (!Params->TryGetNumberField(TEXT("spacing"), Spacing) || !
				FMath::IsFinite(Spacing) || Spacing <= 0.0))
				return FMCPToolResult::Error(
					TEXT("spacing must be a positive finite number."), TEXT("invalid_params"), 422);
			double ColumnsNumber = 10.0;
			if (Params->HasField(TEXT("columns")) && (!Params->TryGetNumberField(TEXT("columns"), ColumnsNumber) ||
				ColumnsNumber < 1.0 || ColumnsNumber > MaxBatchActors
				|| FMath::Abs(ColumnsNumber - FMath::RoundToDouble(ColumnsNumber)) > KINDA_SMALL_NUMBER))
				return FMCPToolResult::Error(
					TEXT("columns must be an integer between 1 and 1000."), TEXT("invalid_params"), 422);
			const int32 Columns = FMath::RoundToInt(ColumnsNumber);
			bool bSelect = false;
			if (Params->HasField(TEXT("select")) && !Params->TryGetBoolField(TEXT("select"), bSelect))
				return FMCPToolResult::Error(TEXT("select must be a boolean."), TEXT("invalid_params"), 422);
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Params->HasField(TEXT("properties"))
				&& (!Params->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->
					IsValid()))
				return FMCPToolResult::Error(TEXT("properties must be an object."), TEXT("invalid_params"), 422);
			const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
			if (Params->HasField(TEXT("tags"))
				&& (!Params->TryGetArrayField(TEXT("tags"), Tags) || !Tags))
				return FMCPToolResult::Error(TEXT("tags must be an array of strings."), TEXT("invalid_params"), 422);

			FScopedTransaction Transaction(NSLOCTEXT("UEAIIntegration", "BatchSpawnBlueprintActors",
			                                         "Batch Spawn Blueprint Actors"));
			TArray<TSharedPtr<FJsonValue>> Actors;
			TArray<TSharedPtr<FJsonValue>> AttemptedActors;
			TArray<TSharedPtr<FJsonValue>> Failures;
			TSet<int32> FailedActorIndices;
			TArray<AActor*> SpawnedActors;
			bool bSpawnFailure = false;
			bool bPropertyFailure = false;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FVector Location = Pattern.Equals(TEXT("linear"), ESearchCase::IgnoreCase)
					                         ? Origin + Direction * static_cast<float>(Index * Spacing)
					                         : Origin + FVector(static_cast<float>((Index % Columns) * Spacing),
					                                            static_cast<float>((Index / Columns) * Spacing), 0.0f);
				AActor* Actor = GEditor->AddActor(TargetLevel, BlueprintClass, FTransform(Rotation, Location), false,
				                                  RF_Transactional, bSelect);
				if (!Actor)
				{
					TSharedRef<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetNumberField(TEXT("index"), Index);
					Failure->SetStringField(TEXT("reason"), TEXT("spawn_failed"));
					Failures.Add(MakeShared<FJsonValueObject>(Failure));
					FailedActorIndices.Add(Index);
					bSpawnFailure = true;
					break;
				}
				SpawnedActors.Add(Actor);
				Actor->SetActorScale3D(Scale);
				ApplyMetadata(Actor, Params, Index);
				FBatchPropertyApplicationResult PropertyResult;
				ApplyProperties(Actor, Properties ? *Properties : nullptr, PropertyResult);
				bPropertyFailure |= PropertyResult.HasFailures();
				for (const TSharedPtr<FJsonValue>& PropertyFailure : PropertyResult.PropertiesFailed)
				{
					if (const TSharedPtr<FJsonObject> FailureObject = PropertyFailure.IsValid()
						? PropertyFailure->AsObject()
						: nullptr)
					{
						FailureObject->SetNumberField(TEXT("index"), Index);
						FailureObject->SetStringField(TEXT("stage"), TEXT("properties"));
					}
					Failures.Add(PropertyFailure);
					FailedActorIndices.Add(Index);
				}
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("index"), Index);
				Entry->SetStringField(TEXT("actor_name"), Actor->GetName());
				Entry->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
				Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetPathName());
				Entry->SetNumberField(TEXT("x"), Actor->GetActorLocation().X);
				Entry->SetNumberField(TEXT("y"), Actor->GetActorLocation().Y);
				Entry->SetNumberField(TEXT("z"), Actor->GetActorLocation().Z);
				TArray<TSharedPtr<FJsonValue>> PropertiesSet;
				for (const FString& PropertyName : PropertyResult.PropertiesSet)
				{
					PropertiesSet.Add(MakeShared<FJsonValueString>(PropertyName));
				}
				Entry->SetArrayField(TEXT("properties_set"), PropertiesSet);
				Entry->SetArrayField(TEXT("properties_failed"), PropertyResult.PropertiesFailed);
				Entry->SetArrayField(TEXT("readback"), PropertyResult.Readback);
				const TSharedPtr<FJsonValue> EntryValue = MakeShared<FJsonValueObject>(Entry);
				Actors.Add(EntryValue);
				AttemptedActors.Add(EntryValue);
			}
			const bool bRolledBack = FailedActorIndices.Num() > 0;
			if (bRolledBack)
			{
				for (AActor* Actor : SpawnedActors)
				{
					if (IsValid(Actor))
					{
						TargetLevel->GetWorld()->EditorDestroyActor(Actor, true);
					}
				}
				Transaction.Cancel();
				Actors.Reset();
			}
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetBoolField(TEXT("success"), !bRolledBack);
			Result->SetStringField(TEXT("schema"), TEXT("ue.blueprint.actor-batch-spawn.v1"));
			Result->SetStringField(TEXT("pattern"), Pattern.ToLower());
			Result->SetNumberField(TEXT("requested"), Count);
			Result->SetNumberField(TEXT("spawned"), Actors.Num());
			Result->SetNumberField(TEXT("failed"), FailedActorIndices.Num());
			Result->SetNumberField(TEXT("failureDetails"), Failures.Num());
			Result->SetBoolField(TEXT("rolledBack"), bRolledBack);
			Result->SetStringField(
				TEXT("rollbackCause"),
				bSpawnFailure && bPropertyFailure
					? TEXT("spawn_and_property_failure")
					: bPropertyFailure
						  ? TEXT("property_failure")
						  : bSpawnFailure
								? TEXT("spawn_failure")
								: TEXT("none"));
			Result->SetStringField(
				TEXT("persistence"), bRolledBack ? TEXT("none") : TEXT("editorTransaction"));
			Result->SetArrayField(TEXT("actors"), Actors);
			Result->SetArrayField(TEXT("failures"), Failures);
			if (bRolledBack)
			{
				// Actors is intentionally empty after an atomic rollback. Keep the
				// attempted per-actor property/readback records for diagnosis without
				// reporting destroyed actors as committed world state.
				Result->SetArrayField(TEXT("attempts"), AttemptedActors);
			}
			return FMCPToolResult::Ok(Result);
		}
	};
}

namespace UEAIIntegrationTools
{
	void RegisterBlueprintBatchSpawnTools(FMCPToolRegistry& Registry)
	{
#if WITH_EDITOR
		Registry.Register(MakeShared<FTool_BatchSpawnBlueprintActors>());
#endif
	}
}
