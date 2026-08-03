#include "Infrastructure/LandscapeWaterService.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetData.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "Infrastructure/Sha256.h"
#include "Landscape.h"
#include "LandscapeBlueprintBrushBase.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "UnrealEdGlobals.h"
#if WITH_UEAI_WATER
#include "WaterBodyActor.h"
#include "WaterBodyComponent.h"
#endif
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
constexpr int32 MaxLandscapeOperations = 32;
constexpr int64 MaxSnapshotBytes = 512ll * 1024ll * 1024ll;
constexpr int32 MaxDiffPaths = 128;
const FName ManagedWaterTag(TEXT("UEAI.ManagedWater"));
const FString ManagedWaterIdPrefix = TEXT("UEAI.WaterId.");
const FString ManagedWaterTypePrefix = TEXT("UEAI.WaterType.");
#if WITH_DEV_AUTOMATION_TESTS
const FName FailureAfterFirstOperation(TEXT("afterFirstOperation"));
const FName FailureAfterFirstOperationRollbackArtifact(
	TEXT("afterFirstOperationRollbackArtifact"));
#endif

struct FLandscapeTarget
{
	FGuid Guid;
	TWeakObjectPtr<ALandscapeProxy> Representative;
	TWeakObjectPtr<ULandscapeInfo> Info;
	TArray<TWeakObjectPtr<ALandscapeProxy>> Proxies;
};

struct FRasterBackup
{
	FString Kind;
	FString LandscapeGuid;
	FString Layer;
	FIntRect Extent;
	FString Path;
	FString Sha256;
};

struct FMaterialBackup
{
	FString LandscapeGuid;
	FString MaterialPath;
};

struct FLayerInfoBackup
{
	FString LandscapeGuid;
	FString Layer;
	FString BeforeLayerInfoPath;
	FString AfterLayerInfoPath;
};

struct FPackageFileBackup
{
	FString PackageName;
	FString OriginalPath;
	FString BackupPath;
	FString Sha256;
	bool bExisted = false;
};

struct FWaterState
{
	FString Identity;
	FString ManagedId;
	FString Type;
	FString Name;
	FString Label;
	FString ClassPath;
	FString ActorGuid;
	FString LevelPath;
	FString PackageName;
	FString ExternalPackageName;
	FString PropertyDigest;
	FTransform Transform;
	TArray<FName> Tags;
	int64 PropertyBytes = 0;
	bool bPresent = false;
	bool bManaged = false;
	bool bExternalActor = false;
};

struct FWaterSideEffectActor
{
	FString ActorGuid;
	FString ClassPath;
	FString Name;
	FString LevelPath;
	FString PackageName;
	FString ExternalPackageName;
	bool bExternalActor = false;
};

struct FWaterLayerSideEffect
{
	FString LandscapeGuid;
	FString LayerGuid;
};

struct FWaterBackup
{
	FString Action;
	FWaterState Before;
	FWaterState Created;
	FString ActorExportPath;
	FString ActorExportSha256;
	TArray<FWaterSideEffectActor> CreatedSideEffectActors;
	TArray<FWaterLayerSideEffect> CreatedLandscapeLayers;
};

UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FString GuidString(const FGuid& Guid)
{
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}

FString ShaPrefixed(const FString& Hash)
{
	return Hash.StartsWith(TEXT("sha256:")) ? Hash : TEXT("sha256:") + Hash;
}

FString HashBytes(const void* Data, const uint64 Size)
{
	FString Hash;
	return TrySha256Hex(Data, Size, Hash) ? ShaPrefixed(Hash) : FString();
}

FString HashBytes(const TArray<uint8>& Bytes)
{
	return HashBytes(Bytes.GetData(), static_cast<uint64>(Bytes.Num()));
}

FMCPToolResult ErrorWithDetails(
	const FString& Message,
	const FString& Code,
	const int32 HttpStatus,
	const TSharedPtr<FJsonObject>& Details)
{
	FMCPToolResult Result =
		FMCPToolResult::Error(Message, Code, HttpStatus);
	Result.Data = Details;
	return Result;
}

TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& Value)
{
	return {
		MakeShared<FJsonValueNumber>(Value.X),
		MakeShared<FJsonValueNumber>(Value.Y),
		MakeShared<FJsonValueNumber>(Value.Z)
	};
}

TSharedRef<FJsonObject> TransformToJson(const FTransform& Transform)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("location"), VectorToJson(Transform.GetLocation()));
	const FRotator Rotation = Transform.Rotator();
	Result->SetArrayField(
		TEXT("rotation"),
		{
			MakeShared<FJsonValueNumber>(Rotation.Pitch),
			MakeShared<FJsonValueNumber>(Rotation.Yaw),
			MakeShared<FJsonValueNumber>(Rotation.Roll)
		});
	Result->SetArrayField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
	return Result;
}

bool ParseVector(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	FVector& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid()
		|| !Object->TryGetArrayField(Field, Values)
		|| !Values
		|| Values->Num() != 3)
	{
		return false;
	}
	OutValue = FVector(
		(*Values)[0]->AsNumber(),
		(*Values)[1]->AsNumber(),
		(*Values)[2]->AsNumber());
	return true;
}

bool ParseTransform(
	const TSharedPtr<FJsonObject>& Object,
	FTransform& OutTransform,
	FString& OutError)
{
	OutError.Reset();
	if (!Object.IsValid())
	{
		OutError = TEXT("transform must be an object.");
		return false;
	}
	FVector Location = FVector::ZeroVector;
	FVector Rotation = FVector::ZeroVector;
	FVector Scale = FVector::OneVector;
	if (Object->HasField(TEXT("location"))
		&& !ParseVector(Object, TEXT("location"), Location))
	{
		OutError = TEXT("transform.location must contain three numbers.");
		return false;
	}
	if (Object->HasField(TEXT("rotation"))
		&& !ParseVector(Object, TEXT("rotation"), Rotation))
	{
		OutError = TEXT("transform.rotation must contain pitch, yaw, and roll.");
		return false;
	}
	if (Object->HasField(TEXT("scale"))
		&& !ParseVector(Object, TEXT("scale"), Scale))
	{
		OutError = TEXT("transform.scale must contain three numbers.");
		return false;
	}
	if (Scale.IsNearlyZero())
	{
		OutError = TEXT("transform.scale cannot be zero.");
		return false;
	}
	OutTransform = FTransform(
		FRotator(Rotation.X, Rotation.Y, Rotation.Z),
		Location,
		Scale);
	return true;
}

TSharedRef<FJsonObject> BoundsToJson(const FBox& Bounds)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
	if (Bounds.IsValid)
	{
		Result->SetArrayField(TEXT("min"), VectorToJson(Bounds.Min));
		Result->SetArrayField(TEXT("max"), VectorToJson(Bounds.Max));
		Result->SetArrayField(TEXT("center"), VectorToJson(Bounds.GetCenter()));
		Result->SetArrayField(TEXT("extent"), VectorToJson(Bounds.GetExtent()));
	}
	return Result;
}

TArray<FLandscapeTarget> CollectLandscapes(UWorld* World)
{
	TMap<FGuid, FLandscapeTarget> ByGuid;
	if (!World)
	{
		return {};
	}
	for (TActorIterator<ALandscapeProxy> It(World); It; ++It)
	{
		ALandscapeProxy* Proxy = *It;
		ULandscapeInfo* Info = Proxy ? Proxy->GetLandscapeInfo() : nullptr;
		if (!Proxy || !Info || !Proxy->GetLandscapeGuid().IsValid())
		{
			continue;
		}
		FLandscapeTarget& Target = ByGuid.FindOrAdd(Proxy->GetLandscapeGuid());
		Target.Guid = Proxy->GetLandscapeGuid();
		Target.Info = Info;
		Target.Proxies.AddUnique(Proxy);
		if (!Target.Representative.IsValid()
			|| Proxy->IsA<ALandscape>())
		{
			Target.Representative = Proxy;
		}
	}
	TArray<FLandscapeTarget> Targets;
	ByGuid.GenerateValueArray(Targets);
	Targets.Sort(
		[](const FLandscapeTarget& Left, const FLandscapeTarget& Right)
		{
			return GuidString(Left.Guid) < GuidString(Right.Guid);
		});
	return Targets;
}

bool TargetMatches(const FLandscapeTarget& Target, const FString& Selector)
{
	if (Selector.IsEmpty())
	{
		return false;
	}
	if (GuidString(Target.Guid).Equals(Selector, ESearchCase::IgnoreCase))
	{
		return true;
	}
	for (const TWeakObjectPtr<ALandscapeProxy>& WeakProxy : Target.Proxies)
	{
		const ALandscapeProxy* Proxy = WeakProxy.Get();
		if (Proxy
			&& (Proxy->GetPathName().Equals(Selector, ESearchCase::IgnoreCase)
				|| Proxy->GetName().Equals(Selector, ESearchCase::IgnoreCase)
				|| Proxy->GetActorLabel().Equals(Selector, ESearchCase::IgnoreCase)))
		{
			return true;
		}
	}
	return false;
}

bool ResolveLandscape(
	UWorld* World,
	const FString& Selector,
	FLandscapeTarget& OutTarget,
	FString& OutError,
	FString& OutCode)
{
	const TArray<FLandscapeTarget> Targets = CollectLandscapes(World);
	if (Targets.IsEmpty())
	{
		OutError = TEXT("The current Editor world contains no loaded Landscape.");
		OutCode = TEXT("landscape_not_found");
		return false;
	}
	if (Selector.IsEmpty())
	{
		if (Targets.Num() != 1)
		{
			OutError = TEXT("landscape is required when more than one Landscape is loaded.");
			OutCode = TEXT("landscape_ambiguous");
			return false;
		}
		OutTarget = Targets[0];
		return true;
	}
	for (const FLandscapeTarget& Target : Targets)
	{
		if (TargetMatches(Target, Selector))
		{
			OutTarget = Target;
			return true;
		}
	}
	OutError = FString::Printf(TEXT("Landscape '%s' was not found."), *Selector);
	OutCode = TEXT("landscape_not_found");
	return false;
}

bool LandscapeExtent(
	const FLandscapeTarget& Target,
	FIntRect& OutExtent,
	int32& OutWidth,
	int32& OutHeight)
{
	ULandscapeInfo* Info = Target.Info.Get();
	if (!Info || !Info->GetLandscapeExtent(OutExtent))
	{
		return false;
	}
	OutWidth = OutExtent.Width() + 1;
	OutHeight = OutExtent.Height() + 1;
	return OutWidth > 0 && OutHeight > 0;
}

TSharedRef<FJsonObject> LayerToJson(
	const FLandscapeInfoLayerSettings& Layer,
	const bool bIncludeOwner)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Layer.GetLayerName().ToString());
	Result->SetBoolField(TEXT("hasLayerInfo"), Layer.LayerInfoObj != nullptr);
	if (Layer.LayerInfoObj)
	{
		Result->SetStringField(
			TEXT("layerInfo"),
			Layer.LayerInfoObj->GetPathName());
		Result->SetBoolField(
			TEXT("noWeightBlend"),
			Layer.LayerInfoObj->bNoWeightBlend);
		Result->SetStringField(
			TEXT("physicalMaterial"),
			Layer.LayerInfoObj->PhysMaterial
				? Layer.LayerInfoObj->PhysMaterial->GetPathName()
				: TEXT(""));
	}
#if WITH_EDITORONLY_DATA
	Result->SetBoolField(TEXT("valid"), Layer.bValid);
	if (bIncludeOwner)
	{
		Result->SetStringField(
			TEXT("owner"),
			Layer.Owner ? Layer.Owner->GetPathName() : TEXT(""));
	}
#endif
	return Result;
}

TSharedRef<FJsonObject> DescribeLandscape(
	const FLandscapeTarget& Target,
	const bool bIncludeLayers)
{
	ALandscapeProxy* Proxy = Target.Representative.Get();
	ULandscapeInfo* Info = Target.Info.Get();
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("landscapeId"), GuidString(Target.Guid));
	Result->SetStringField(
		TEXT("path"),
		Proxy ? Proxy->GetPathName() : TEXT(""));
	Result->SetStringField(
		TEXT("label"),
		Proxy ? Proxy->GetActorLabel() : TEXT(""));
	Result->SetStringField(
		TEXT("class"),
		Proxy ? Proxy->GetClass()->GetPathName() : TEXT(""));
	Result->SetNumberField(TEXT("proxyCount"), Target.Proxies.Num());
	Result->SetNumberField(
		TEXT("componentCount"),
		Info ? Info->XYtoComponentMap.Num() : 0);
	Result->SetBoolField(
		TEXT("supportsEditing"),
		Info && Info->SupportsLandscapeEditing());
	Result->SetBoolField(
		TEXT("hasEditLayers"),
		Proxy && Proxy->HasLayersContent());
	Result->SetBoolField(
		TEXT("packageDirty"),
		Proxy && Proxy->GetPackage()->IsDirty());
	Result->SetStringField(
		TEXT("material"),
		Proxy && Proxy->GetLandscapeMaterial()
			? Proxy->GetLandscapeMaterial()->GetPathName()
			: TEXT(""));
	if (Proxy)
	{
		Result->SetObjectField(TEXT("transform"), TransformToJson(Proxy->GetTransform()));
	}
	FIntRect Extent;
	int32 Width = 0;
	int32 Height = 0;
	if (LandscapeExtent(Target, Extent, Width, Height))
	{
		TSharedRef<FJsonObject> ExtentJson = MakeShared<FJsonObject>();
		ExtentJson->SetNumberField(TEXT("minX"), Extent.Min.X);
		ExtentJson->SetNumberField(TEXT("minY"), Extent.Min.Y);
		ExtentJson->SetNumberField(TEXT("maxX"), Extent.Max.X);
		ExtentJson->SetNumberField(TEXT("maxY"), Extent.Max.Y);
		ExtentJson->SetNumberField(TEXT("width"), Width);
		ExtentJson->SetNumberField(TEXT("height"), Height);
		Result->SetObjectField(TEXT("extent"), ExtentJson);
	}
	Result->SetObjectField(
		TEXT("loadedBounds"),
		BoundsToJson(Info ? Info->GetLoadedBounds() : FBox(ForceInit)));
	TArray<TSharedPtr<FJsonValue>> Proxies;
	for (const TWeakObjectPtr<ALandscapeProxy>& WeakProxy : Target.Proxies)
	{
		if (const ALandscapeProxy* Item = WeakProxy.Get())
		{
			TSharedRef<FJsonObject> ProxyJson = MakeShared<FJsonObject>();
			ProxyJson->SetStringField(TEXT("path"), Item->GetPathName());
			ProxyJson->SetStringField(TEXT("label"), Item->GetActorLabel());
			ProxyJson->SetNumberField(
				TEXT("componentCount"),
				Item->GetComponents().Num());
			ProxyJson->SetBoolField(
				TEXT("packageDirty"),
				Item->GetPackage()->IsDirty());
			Proxies.Add(MakeShared<FJsonValueObject>(ProxyJson));
		}
	}
	Result->SetArrayField(TEXT("proxies"), Proxies);
	if (bIncludeLayers && Info)
	{
		TArray<TSharedPtr<FJsonValue>> Layers;
		for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
		{
			Layers.Add(MakeShared<FJsonValueObject>(LayerToJson(Layer, true)));
		}
		Layers.Sort(
			[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
			{
				return Left->AsObject()->GetStringField(TEXT("name"))
					< Right->AsObject()->GetStringField(TEXT("name"));
			});
		Result->SetArrayField(TEXT("layers"), Layers);
		Result->SetNumberField(TEXT("layerCount"), Layers.Num());
	}
	return Result;
}

ULandscapeLayerInfoObject* FindLayerInfo(
	const FLandscapeTarget& Target,
	const FString& LayerName)
{
	ULandscapeInfo* Info = Target.Info.Get();
	if (!Info)
	{
		return nullptr;
	}
	for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
	{
		if (Layer.GetLayerName().ToString().Equals(
				LayerName,
				ESearchCase::IgnoreCase))
		{
			return Layer.LayerInfoObj;
		}
	}
	return nullptr;
}

bool CaptureHeight(
	const FLandscapeTarget& Target,
	FIntRect& OutExtent,
	int32& OutWidth,
	int32& OutHeight,
	TArray<uint16>& OutData)
{
	if (!LandscapeExtent(Target, OutExtent, OutWidth, OutHeight)
		|| static_cast<int64>(OutWidth) * OutHeight
			> MaxSnapshotBytes / static_cast<int64>(sizeof(uint16)))
	{
		return false;
	}
	OutData.SetNumZeroed(OutWidth * OutHeight);
	FLandscapeEditDataInterface Edit(Target.Info.Get());
	Edit.GetHeightDataFast(
		OutExtent.Min.X,
		OutExtent.Min.Y,
		OutExtent.Max.X,
		OutExtent.Max.Y,
		OutData.GetData(),
		0);
	return true;
}

bool CaptureWeight(
	const FLandscapeTarget& Target,
	ULandscapeLayerInfoObject* LayerInfo,
	FIntRect& OutExtent,
	int32& OutWidth,
	int32& OutHeight,
	TArray<uint8>& OutData)
{
	if (!LayerInfo
		|| !LandscapeExtent(Target, OutExtent, OutWidth, OutHeight)
		|| static_cast<int64>(OutWidth) * OutHeight > MaxSnapshotBytes)
	{
		return false;
	}
	OutData.SetNumZeroed(OutWidth * OutHeight);
	FLandscapeEditDataInterface Edit(Target.Info.Get());
	Edit.GetWeightDataFast(
		LayerInfo,
		OutExtent.Min.X,
		OutExtent.Min.Y,
		OutExtent.Max.X,
		OutExtent.Max.Y,
		OutData.GetData(),
		0);
	return true;
}

TArray<uint8> EncodeR16(const TArray<uint16>& Values)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Values.Num() * 2);
	for (int32 Index = 0; Index < Values.Num(); ++Index)
	{
		Bytes[Index * 2] = static_cast<uint8>(Values[Index] & 0xffu);
		Bytes[Index * 2 + 1] =
			static_cast<uint8>((Values[Index] >> 8u) & 0xffu);
	}
	return Bytes;
}

bool DecodeR16(
	const TArray<uint8>& Bytes,
	const int32 Width,
	const int32 Height,
	const bool bFlipY,
	TArray<uint16>& OutValues)
{
	const int64 ValueCount = static_cast<int64>(Width) * Height;
	if (ValueCount <= 0 || Bytes.Num() != ValueCount * 2)
	{
		return false;
	}
	OutValues.SetNumUninitialized(static_cast<int32>(ValueCount));
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 SourceY = bFlipY ? Height - 1 - Y : Y;
		for (int32 X = 0; X < Width; ++X)
		{
			const int32 SourceIndex = (SourceY * Width + X) * 2;
			OutValues[Y * Width + X] =
				static_cast<uint16>(Bytes[SourceIndex])
				| static_cast<uint16>(Bytes[SourceIndex + 1] << 8u);
		}
	}
	return true;
}

bool DecodeR8(
	const TArray<uint8>& Bytes,
	const int32 Width,
	const int32 Height,
	const bool bFlipY,
	TArray<uint8>& OutValues)
{
	const int64 ValueCount = static_cast<int64>(Width) * Height;
	if (ValueCount <= 0 || Bytes.Num() != ValueCount)
	{
		return false;
	}
	OutValues.SetNumUninitialized(static_cast<int32>(ValueCount));
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const int32 SourceY = bFlipY ? Height - 1 - Y : Y;
		FMemory::Memcpy(
			OutValues.GetData() + Y * Width,
			Bytes.GetData() + SourceY * Width,
			Width);
	}
	return true;
}

bool IsPathWithin(const FString& Path, const FString& Root)
{
	FString FullPath = FPaths::ConvertRelativePathToFull(Path);
	FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeFilename(FullPath);
	FPaths::NormalizeDirectoryName(FullRoot);
	if (!FullRoot.EndsWith(TEXT("/")))
	{
		FullRoot += TEXT("/");
	}
	return FullPath.StartsWith(FullRoot, ESearchCase::IgnoreCase);
}

bool ResolveImportPath(
	const FString& Input,
	FString& OutPath,
	FString& OutError)
{
	OutError.Reset();
	if (Input.IsEmpty())
	{
		OutError = TEXT("sourcePath is required.");
		return false;
	}
	OutPath = FPaths::IsRelative(Input)
		? FPaths::Combine(FPaths::ProjectDir(), Input)
		: Input;
	OutPath = FPaths::ConvertRelativePathToFull(OutPath);
	FPaths::NormalizeFilename(OutPath);
	if (!IsPathWithin(OutPath, FPaths::ProjectDir()))
	{
		OutError = TEXT("sourcePath must remain inside the current project directory.");
		return false;
	}
	if (!FPaths::FileExists(OutPath))
	{
		OutError = FString::Printf(TEXT("Source file '%s' does not exist."), *OutPath);
		return false;
	}
	FString PhysicalPath =
		IFileManager::Get().GetFilenameOnDisk(*OutPath);
	PhysicalPath = FPaths::ConvertRelativePathToFull(PhysicalPath);
	FPaths::NormalizeFilename(PhysicalPath);
	if (!FPaths::IsSamePath(OutPath, PhysicalPath))
	{
		OutError =
			TEXT("sourcePath must be a regular project file and cannot traverse a symbolic link or directory junction.");
		return false;
	}
	return true;
}

FString ArtifactRoot()
{
	return FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration"),
		TEXT("Landscape"));
}

FString SanitizeFileStem(FString Value, const FString& Fallback)
{
	Value = FPaths::GetBaseFilename(FPaths::GetCleanFilename(Value));
	for (TCHAR& Character : Value)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TCHAR('-')
			&& Character != TCHAR('_'))
		{
			Character = TCHAR('_');
		}
	}
	return Value.IsEmpty() ? Fallback : Value.Left(96);
}

bool SaveJsonFile(const FString& Path, const TSharedPtr<FJsonObject>& Object)
{
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer))
	{
		return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FString TemporaryPath = FString::Printf(
		TEXT("%s.%s.tmp"),
		*Path,
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!FFileHelper::SaveStringToFile(
			Serialized,
			*TemporaryPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	if (!IFileManager::Get().Move(
			*Path,
			*TemporaryPath,
			true,
			true,
			false,
			false))
	{
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path)
{
	FString Serialized;
	if (!FFileHelper::LoadFileToString(Serialized, *Path))
	{
		return nullptr;
	}
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(Serialized);
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid()
		? Object
		: nullptr;
}

TSharedRef<FJsonObject> ArtifactJson(
	const FString& Path,
	const FString& Format,
	const FString& Sha256,
	const int32 Width,
	const int32 Height,
	const FString& SidecarPath)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.landscape-artifact.v1"));
	Result->SetStringField(TEXT("path"), Path);
	Result->SetStringField(TEXT("sidecarPath"), SidecarPath);
	Result->SetStringField(TEXT("format"), Format);
	Result->SetStringField(TEXT("mimeType"), TEXT("application/octet-stream"));
	Result->SetStringField(TEXT("sha256"), Sha256);
	Result->SetNumberField(TEXT("size"), IFileManager::Get().FileSize(*Path));
	Result->SetNumberField(TEXT("width"), Width);
	Result->SetNumberField(TEXT("height"), Height);
	return Result;
}

FString WaterTypeFromClass(const UClass* Class)
{
	if (!Class)
	{
		return FString();
	}
	const FString Name = Class->GetName();
	if (Name.Contains(TEXT("WaterBodyRiver")))
	{
		return TEXT("river");
	}
	if (Name.Contains(TEXT("WaterBodyLake")))
	{
		return TEXT("lake");
	}
	if (Name.Contains(TEXT("WaterBodyOcean")))
	{
		return TEXT("ocean");
	}
	if (Name.Contains(TEXT("WaterBodyCustom")))
	{
		return TEXT("custom");
	}
	return FString();
}

bool IsWaterActor(const AActor* Actor)
{
	return Actor && !WaterTypeFromClass(Actor->GetClass()).IsEmpty();
}

FString TagValue(const AActor* Actor, const FString& Prefix)
{
	if (!Actor)
	{
		return FString();
	}
	for (const FName& Tag : Actor->Tags)
	{
		const FString Value = Tag.ToString();
		if (Value.StartsWith(Prefix))
		{
			return Value.Mid(Prefix.Len());
		}
	}
	return FString();
}

FString WaterManagedId(const AActor* Actor)
{
	return TagValue(Actor, ManagedWaterIdPrefix);
}

bool IsManagedWater(const AActor* Actor)
{
	return Actor
		&& Actor->Tags.Contains(ManagedWaterTag)
		&& !WaterManagedId(Actor).IsEmpty();
}

FString WaterIdentity(const AActor* Actor)
{
	const FString ManagedId = WaterManagedId(Actor);
	return ManagedId.IsEmpty()
		? GuidString(Actor->GetActorGuid())
		: ManagedId;
}

bool CanonicalizeWaterActorText(
	const FString& Text,
	FString& OutCanonical)
{
	OutCanonical.Reset();
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);

	// Actor text is a recovery artifact, not a stable serialization format.
	// In particular, CopyActors may nest a component-owned object under its
	// component while ImportObjectProperties subsequently emits the same object
	// as a sibling default subobject. Archetype and ExportPath attributes also
	// differ between a freshly placed actor and an equivalent recreated actor.
	// Build a property digest from a flat, object-scoped multiset instead of
	// hashing the incidental Begin/End nesting and block order.
	auto HeaderValue = [](const FString& Header, const TCHAR* Key)
	{
		FString Value;
		FParse::Value(*Header, Key, Value);
		Value.TrimQuotesInline();
		return Value;
	};
	int32 ActorBeginIndex = INDEX_NONE;
	int32 ActorEndIndex = INDEX_NONE;
	int32 ActorDepth = 0;
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const FString Line = Lines[Index].TrimStartAndEnd();
		if (Line.StartsWith(TEXT("Begin Actor"), ESearchCase::IgnoreCase))
		{
			if (ActorBeginIndex != INDEX_NONE)
			{
				return false;
			}
			ActorBeginIndex = Index;
			ActorDepth = 1;
			continue;
		}
		if (ActorBeginIndex == INDEX_NONE)
		{
			continue;
		}
		if (Line.StartsWith(TEXT("Begin "), ESearchCase::IgnoreCase))
		{
			++ActorDepth;
		}
		else if (Line.StartsWith(TEXT("End "), ESearchCase::IgnoreCase))
		{
			--ActorDepth;
			if (ActorDepth == 0)
			{
				ActorEndIndex = Index;
				break;
			}
			if (ActorDepth < 0)
			{
				return false;
			}
		}
	}
	if (ActorBeginIndex == INDEX_NONE || ActorEndIndex == INDEX_NONE)
	{
		return false;
	}

	TMap<FString, FString> ClassByObjectName;
	for (int32 Index = ActorBeginIndex; Index <= ActorEndIndex; ++Index)
	{
		const FString Line = Lines[Index].TrimStartAndEnd();
		if (!Line.StartsWith(TEXT("Begin "), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString ObjectName = HeaderValue(Line, TEXT("Name="));
		const FString ClassName = HeaderValue(Line, TEXT("Class="));
		if (!ObjectName.IsEmpty() && !ClassName.IsEmpty())
		{
			ClassByObjectName.FindOrAdd(ObjectName) = ClassName;
		}
	}

	TArray<FString> ScopeStack;
	TMap<FString, int32> EntryCounts;
	auto AddEntry = [&EntryCounts](const FString& Entry)
	{
		++EntryCounts.FindOrAdd(Entry);
	};
	for (int32 Index = ActorBeginIndex; Index <= ActorEndIndex; ++Index)
	{
		const FString Line = Lines[Index].TrimStartAndEnd();
		if (Line.IsEmpty())
		{
			continue;
		}
		if (Line.StartsWith(TEXT("Begin "), ESearchCase::IgnoreCase))
		{
			if (Line.StartsWith(TEXT("Begin Actor"), ESearchCase::IgnoreCase))
			{
				if (!ScopeStack.IsEmpty())
				{
					return false;
				}
				const FString ActorClass =
					HeaderValue(Line, TEXT("Class="));
				const FString RootScope =
					TEXT("actor|")
					+ (ActorClass.IsEmpty()
						? FString(TEXT("<unknown>"))
						: ActorClass);
				ScopeStack.Add(RootScope);
				AddEntry(TEXT("object|") + RootScope);
				continue;
			}
			if (!Line.StartsWith(TEXT("Begin Object"), ESearchCase::IgnoreCase))
			{
				return false;
			}
			const FString ObjectName = HeaderValue(Line, TEXT("Name="));
			FString ClassName = HeaderValue(Line, TEXT("Class="));
			if (ClassName.IsEmpty() && !ObjectName.IsEmpty())
			{
				if (const FString* KnownClass =
						ClassByObjectName.Find(ObjectName))
				{
					ClassName = *KnownClass;
				}
			}
			const FString Scope = FString::Printf(
				TEXT("%s|%s"),
				ClassName.IsEmpty() ? TEXT("<unknown>") : *ClassName,
				ObjectName.IsEmpty() ? TEXT("<unnamed>") : *ObjectName);
			ScopeStack.Add(Scope);
			AddEntry(TEXT("object|") + Scope);
			continue;
		}
		if (Line.StartsWith(TEXT("End "), ESearchCase::IgnoreCase))
		{
			if (ScopeStack.IsEmpty())
			{
				return false;
			}
			ScopeStack.Pop();
			continue;
		}
		if (ScopeStack.IsEmpty())
		{
			return false;
		}
		AddEntry(ScopeStack.Last() + TEXT("|") + Line);
	}
	if (!ScopeStack.IsEmpty() || EntryCounts.IsEmpty())
	{
		return false;
	}
	TArray<FString> SortedEntries;
	EntryCounts.GenerateKeyArray(SortedEntries);
	SortedEntries.Sort();
	for (const FString& Entry : SortedEntries)
	{
		OutCanonical += FString::Printf(
			TEXT("%d|%s"),
			EntryCounts.FindChecked(Entry),
			*Entry);
		OutCanonical += TEXT("\n");
	}
	return !OutCanonical.IsEmpty();
}

bool ExportWaterActorText(
	const AActor* Actor,
	FString& OutText,
	FString& OutDigest,
	int64& OutBytes,
	FString& OutError)
{
	OutText.Reset();
	OutDigest.Reset();
	OutBytes = 0;
	OutError.Reset();
	if (!Actor || !Actor->GetWorld() || !Actor->GetLevel() || !GUnrealEd)
	{
		OutError = TEXT("The Water actor export context is unavailable.");
		return false;
	}
	UWorld* World = Actor->GetWorld();
	ULevel* PreviousLevel = World->GetCurrentLevel();
	if (!PreviousLevel)
	{
		OutError =
			TEXT("The Water actor export has no current Editor level to restore.");
		return false;
	}
	if (PreviousLevel != Actor->GetLevel())
	{
		if (!World->SetCurrentLevel(Actor->GetLevel()))
		{
			OutError =
				TEXT("The Water actor level could not be made current for export.");
			return false;
		}
	}
	ON_SCOPE_EXIT
	{
		if (World && PreviousLevel && World->GetCurrentLevel() != PreviousLevel)
		{
			World->SetCurrentLevel(PreviousLevel);
		}
	};

	TArray<AActor*> Actors;
	Actors.Add(const_cast<AActor*>(Actor));
	GUnrealEd->CopyActors(Actors, World, &OutText);
	if (OutText.IsEmpty())
	{
		OutError = TEXT("The Water actor could not be exported as UE actor text.");
		return false;
	}
	// Water components populate OverrideMaterials with runtime-created MIDs
	// under /Engine/Transient. Those references are not durable state and
	// cannot be imported into a recreated external actor package. Preserve the
	// factory-generated runtime values by excluding only lines that reference
	// transient objects from both the recovery artifact and its stable digest.
	FString DurableText;
	const TCHAR* Cursor = *OutText;
	FString Line;
	int32 SkippedDerivedObjectDepth = 0;
	auto IsDerivedWaterObjectName = [](const FString& ObjectName)
	{
		return ObjectName == TEXT("LakeMeshComponent")
			|| ObjectName == TEXT("LakeCollisionComponent")
			|| ObjectName == TEXT("BodySetup")
			|| ObjectName == TEXT("WaterInfoMeshComponent")
			|| ObjectName == TEXT("DilatedWaterInfoMeshComponent")
			|| ObjectName.StartsWith(TEXT("WaterInfoMesh_"))
			|| ObjectName.StartsWith(TEXT("WaterInfoDilatedMesh_"));
	};
	while (FParse::Line(&Cursor, Line))
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (Trimmed.StartsWith(
				TEXT("Begin Object"),
				ESearchCase::IgnoreCase))
		{
			if (SkippedDerivedObjectDepth > 0)
			{
				++SkippedDerivedObjectDepth;
				continue;
			}
			FString ObjectName;
			FParse::Value(*Trimmed, TEXT("Name="), ObjectName);
			ObjectName.TrimQuotesInline();
			if (IsDerivedWaterObjectName(ObjectName))
			{
				SkippedDerivedObjectDepth = 1;
				continue;
			}
		}
		else if (Trimmed.StartsWith(
					TEXT("End Object"),
					ESearchCase::IgnoreCase)
			&& SkippedDerivedObjectDepth > 0)
		{
			--SkippedDerivedObjectDepth;
			continue;
		}
		if (SkippedDerivedObjectDepth > 0)
		{
			continue;
		}
		if (Line.Contains(
				TEXT("/Engine/Transient"),
				ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (Trimmed.StartsWith(TEXT("LakeMeshComp="))
			|| Trimmed.StartsWith(TEXT("LakeCollision=")))
		{
			continue;
		}
		DurableText += Line;
		DurableText += TEXT("\r\n");
	}
	if (SkippedDerivedObjectDepth != 0)
	{
		OutError =
			TEXT("The Water actor export contains an incomplete derived object block.");
		return false;
	}
	OutText = MoveTemp(DurableText);
	if (OutText.IsEmpty())
	{
		OutError =
			TEXT("The Water actor export contains no durable properties.");
		return false;
	}
	FString CanonicalText;
	if (!CanonicalizeWaterActorText(OutText, CanonicalText))
	{
		OutError =
			TEXT("The durable Water actor export could not be canonicalized.");
		return false;
	}
	const FTCHARToUTF8 ArtifactUtf8(*OutText);
	const FTCHARToUTF8 CanonicalUtf8(*CanonicalText);
	if (ArtifactUtf8.Length() <= 0
		|| ArtifactUtf8.Length() > MaxSnapshotBytes)
	{
		OutError =
			TEXT("The durable Water actor export exceeds the 512 MiB limit.");
		return false;
	}
	// propertyBytes describes the canonical semantic input to PropertyDigest.
	// The raw artifact has its own SHA-256 and may legitimately differ in
	// ExportPath text or Begin/End nesting after an equivalent restore.
	OutBytes = CanonicalUtf8.Length();
	OutDigest = HashBytes(
		CanonicalUtf8.Get(),
		static_cast<uint64>(CanonicalUtf8.Length()));
	if (OutDigest.IsEmpty())
	{
		OutError = TEXT("The Water actor export digest could not be computed.");
		return false;
	}
	return true;
}

bool ExtractSingleActorPropertyText(
	const FString& ExportText,
	FString& OutPropertyText,
	FString& OutError)
{
	OutPropertyText.Reset();
	OutError.Reset();
	bool bInsideActor = false;
	bool bFinishedActor = false;
	int32 ActorCount = 0;
	const TCHAR* Cursor = *ExportText;
	FString Line;
	while (FParse::Line(&Cursor, Line))
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (!bInsideActor
			&& Trimmed.StartsWith(
				TEXT("BEGIN ACTOR"),
				ESearchCase::IgnoreCase))
		{
			++ActorCount;
			bInsideActor = true;
			continue;
		}
		if (bInsideActor
			&& Trimmed.Equals(
				TEXT("END ACTOR"),
				ESearchCase::IgnoreCase))
		{
			bInsideActor = false;
			bFinishedActor = true;
			continue;
		}
		if (bInsideActor)
		{
			OutPropertyText += Line;
			OutPropertyText += TEXT("\r\n");
		}
	}
	if (ActorCount != 1 || !bFinishedActor || OutPropertyText.IsEmpty())
	{
		OutError =
			TEXT("The Water recovery artifact must contain exactly one complete actor.");
		OutPropertyText.Reset();
		return false;
	}
	return true;
}

FWaterState CaptureWaterState(const AActor* Actor)
{
	FWaterState State;
	if (!Actor)
	{
		return State;
	}
	State.bPresent = true;
	State.Identity = WaterIdentity(Actor);
	State.ManagedId = WaterManagedId(Actor);
	State.Type = WaterTypeFromClass(Actor->GetClass());
	State.Name = Actor->GetName();
	State.Label = Actor->GetActorLabel();
	State.ClassPath = Actor->GetClass()->GetPathName();
	State.ActorGuid = GuidString(Actor->GetActorGuid());
	State.LevelPath =
		Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString();
	State.PackageName =
		Actor->GetPackage() ? Actor->GetPackage()->GetName() : FString();
	State.bExternalActor = Actor->IsPackageExternal();
	State.ExternalPackageName =
		State.bExternalActor && Actor->GetExternalPackage()
			? Actor->GetExternalPackage()->GetName()
			: FString();
	State.Transform = Actor->GetActorTransform();
	State.Tags = Actor->Tags;
	State.bManaged = IsManagedWater(Actor);
	FString ExportText;
	FString ExportError;
	ExportWaterActorText(
		Actor,
		ExportText,
		State.PropertyDigest,
		State.PropertyBytes,
		ExportError);
	return State;
}

FWaterSideEffectActor CaptureWaterSideEffectActor(const AActor* Actor)
{
	FWaterSideEffectActor State;
	if (!Actor)
	{
		return State;
	}
	State.ActorGuid = GuidString(Actor->GetActorGuid());
	State.ClassPath = Actor->GetClass()->GetPathName();
	State.Name = Actor->GetName();
	State.LevelPath =
		Actor->GetLevel() ? Actor->GetLevel()->GetPathName() : FString();
	State.PackageName =
		Actor->GetPackage() ? Actor->GetPackage()->GetName() : FString();
	State.bExternalActor = Actor->IsPackageExternal();
	State.ExternalPackageName =
		State.bExternalActor && Actor->GetExternalPackage()
			? Actor->GetExternalPackage()->GetName()
			: FString();
	return State;
}

TSet<FString> CollectActorGuids(UWorld* World)
{
	TSet<FString> Result;
	if (!World)
	{
		return Result;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (IsValid(*It) && It->GetActorGuid().IsValid())
		{
			Result.Add(GuidString(It->GetActorGuid()));
		}
	}
	return Result;
}

TMap<FString, FString> CollectWaterLandscapeLayers(UWorld* World)
{
	TMap<FString, FString> Result;
	if (!World)
	{
		return Result;
	}
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		const int32 WaterLayerIndex = It->GetLayerIndex(TEXT("Water"));
		const FLandscapeLayer* Layer =
			WaterLayerIndex != INDEX_NONE
				? It->GetLayer(WaterLayerIndex)
				: nullptr;
		if (Layer)
		{
			Result.Add(
				GuidString(It->GetLandscapeGuid()),
				GuidString(Layer->Guid));
		}
	}
	return Result;
}

void CaptureWaterCreateSideEffects(
	UWorld* World,
	const TSet<FString>& BeforeActorGuids,
	const TMap<FString, FString>& BeforeWaterLayers,
	const AActor* CreatedWater,
	FWaterBackup& Backup)
{
	Backup.CreatedSideEffectActors.Reset();
	Backup.CreatedLandscapeLayers.Reset();
	if (!World)
	{
		return;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor)
			|| Actor == CreatedWater
			|| !Actor->GetActorGuid().IsValid())
		{
			continue;
		}
		const FString ActorGuid = GuidString(Actor->GetActorGuid());
		if (!BeforeActorGuids.Contains(ActorGuid))
		{
			Backup.CreatedSideEffectActors.Add(
				CaptureWaterSideEffectActor(Actor));
		}
	}
	Backup.CreatedSideEffectActors.Sort(
		[](const FWaterSideEffectActor& Left,
			const FWaterSideEffectActor& Right)
		{
			return Left.ActorGuid < Right.ActorGuid;
		});

	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		const FString LandscapeGuid = GuidString(It->GetLandscapeGuid());
		if (BeforeWaterLayers.Contains(LandscapeGuid))
		{
			continue;
		}
		const int32 WaterLayerIndex = It->GetLayerIndex(TEXT("Water"));
		const FLandscapeLayer* Layer =
			WaterLayerIndex != INDEX_NONE
				? It->GetLayer(WaterLayerIndex)
				: nullptr;
		if (Layer)
		{
			FWaterLayerSideEffect SideEffect;
			SideEffect.LandscapeGuid = LandscapeGuid;
			SideEffect.LayerGuid = GuidString(Layer->Guid);
			Backup.CreatedLandscapeLayers.Add(MoveTemp(SideEffect));
		}
	}
	Backup.CreatedLandscapeLayers.Sort(
		[](const FWaterLayerSideEffect& Left,
			const FWaterLayerSideEffect& Right)
		{
			return Left.LandscapeGuid < Right.LandscapeGuid;
		});
}

bool IsCompleteWaterState(
	const FWaterState& State,
	const bool bRequireManagedIdentity)
{
	return State.bPresent
		&& !State.Type.IsEmpty()
		&& !State.Name.IsEmpty()
		&& !State.ClassPath.IsEmpty()
		&& !State.ActorGuid.IsEmpty()
		&& !State.LevelPath.IsEmpty()
		&& !State.PackageName.IsEmpty()
		&& !State.PropertyDigest.IsEmpty()
		&& State.PropertyBytes > 0
		&& (!State.bExternalActor
			|| !State.ExternalPackageName.IsEmpty())
		&& (!bRequireManagedIdentity
			|| (State.bManaged && !State.ManagedId.IsEmpty()));
}

TSharedRef<FJsonObject> WaterStateToJson(
	const FWaterState& State,
	const bool bIncludeVolatileIdentity)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("present"), State.bPresent);
	Result->SetStringField(TEXT("identity"), State.Identity);
	Result->SetStringField(TEXT("managedId"), State.ManagedId);
	if (!State.bPresent)
	{
		return Result;
	}
	Result->SetStringField(TEXT("type"), State.Type);
	Result->SetStringField(TEXT("label"), State.Label);
	Result->SetStringField(TEXT("class"), State.ClassPath);
	Result->SetStringField(TEXT("objectName"), State.Name);
	Result->SetStringField(TEXT("actorGuid"), State.ActorGuid);
	Result->SetStringField(TEXT("level"), State.LevelPath);
	Result->SetStringField(TEXT("package"), State.PackageName);
	Result->SetBoolField(TEXT("externalActor"), State.bExternalActor);
	Result->SetStringField(
		TEXT("externalPackage"),
		State.ExternalPackageName);
	Result->SetStringField(TEXT("propertyDigest"), State.PropertyDigest);
	Result->SetNumberField(
		TEXT("propertyBytes"),
		static_cast<double>(State.PropertyBytes));
	Result->SetObjectField(TEXT("transform"), TransformToJson(State.Transform));
	Result->SetBoolField(TEXT("managed"), State.bManaged);
	TArray<TSharedPtr<FJsonValue>> Tags;
	for (const FName& Tag : State.Tags)
	{
		Tags.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Tags.Sort(
		[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsString() < Right->AsString();
		});
	Result->SetArrayField(TEXT("tags"), Tags);
	if (bIncludeVolatileIdentity)
	{
		Result->SetStringField(TEXT("name"), State.Name);
	}
	return Result;
}

TSharedRef<FJsonObject> DescribeWaterActor(const AActor* Actor)
{
	const FWaterState State = CaptureWaterState(Actor);
	TSharedRef<FJsonObject> Result = WaterStateToJson(State, true);
	Result->SetStringField(TEXT("path"), Actor ? Actor->GetPathName() : TEXT(""));
	Result->SetObjectField(
		TEXT("bounds"),
		BoundsToJson(Actor ? Actor->GetComponentsBoundingBox(true) : FBox(ForceInit)));
	Result->SetBoolField(
		TEXT("packageDirty"),
		Actor && Actor->GetPackage()->IsDirty());
	TArray<TSharedPtr<FJsonValue>> DataLayers;
	if (Actor)
	{
		for (const UDataLayerAsset* Layer : Actor->GetDataLayerAssets())
		{
			if (Layer)
			{
				DataLayers.Add(
					MakeShared<FJsonValueString>(Layer->GetPathName()));
			}
		}
	}
	Result->SetArrayField(TEXT("dataLayers"), DataLayers);
	Result->SetStringField(
		TEXT("hlodLayer"),
		Actor && Actor->GetHLODLayer()
			? Actor->GetHLODLayer()->GetPathName()
			: TEXT(""));
	return Result;
}

TArray<AActor*> CollectWaterActors(UWorld* World)
{
	TArray<AActor*> Actors;
	if (!World)
	{
		return Actors;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (IsWaterActor(*It))
		{
			Actors.Add(*It);
		}
	}
	Actors.Sort(
		[](const AActor& Left, const AActor& Right)
		{
			return WaterIdentity(&Left) < WaterIdentity(&Right);
		});
	return Actors;
}

AActor* ResolveWater(UWorld* World, const FString& Selector)
{
	for (AActor* Actor : CollectWaterActors(World))
	{
		if (WaterIdentity(Actor).Equals(Selector, ESearchCase::IgnoreCase)
			|| Actor->GetPathName().Equals(Selector, ESearchCase::IgnoreCase)
			|| Actor->GetName().Equals(Selector, ESearchCase::IgnoreCase)
			|| Actor->GetActorLabel().Equals(Selector, ESearchCase::IgnoreCase)
			|| GuidString(Actor->GetActorGuid()).Equals(
				Selector,
				ESearchCase::IgnoreCase))
		{
			return Actor;
		}
	}
	return nullptr;
}

UActorFactory* FindWaterFactory(const FString& Type)
{
	if (!GEditor)
	{
		return nullptr;
	}
	const FString ExpectedClass =
		Type == TEXT("river") ? TEXT("WaterBodyRiver")
		: Type == TEXT("lake") ? TEXT("WaterBodyLake")
		: Type == TEXT("ocean") ? TEXT("WaterBodyOcean")
		: Type == TEXT("custom") ? TEXT("WaterBodyCustom")
		: FString();
	if (ExpectedClass.IsEmpty())
	{
		return nullptr;
	}
	for (UActorFactory* Factory : GEditor->ActorFactories)
	{
		UClass* ActorClass =
			Factory ? Factory->GetDefaultActorClass(FAssetData()) : nullptr;
		if (ActorClass && ActorClass->GetName() == ExpectedClass)
		{
			return Factory;
		}
	}
	return nullptr;
}

AActor* SpawnManagedWater(
	const FString& Type,
	const FString& ManagedId,
	const FString& Label,
	const FTransform& Transform,
	FString& OutError)
{
	OutError.Reset();
	UActorFactory* Factory = FindWaterFactory(Type);
	if (!Factory || !GEditor)
	{
		OutError = FString::Printf(
			TEXT("The enabled Water editor plugin did not provide a '%s' actor factory."),
			*Type);
		return nullptr;
	}
	UWorld* World = GetEditorWorld();
	ULevel* Level = World ? World->GetCurrentLevel() : nullptr;
	UClass* ActorClass = Factory->GetDefaultActorClass(FAssetData());
	// UEditorEngine::UseActorFactory only places an actor when its FAssetData
	// resolves to a non-null asset. Water body factories are class factories,
	// so an empty FAssetData can never reach them. Pass the factory's declared
	// actor class as the non-null placement context and invoke the factory
	// directly on the current level so its Water-specific PostSpawnActor setup
	// still runs.
	AActor* Actor =
		Level && ActorClass
			? Factory->CreateActor(
				ActorClass,
				Level,
				Transform,
				RF_Transactional)
			: nullptr;
	if (!Actor)
	{
		OutError = TEXT("The Water actor factory failed to create an actor.");
		return nullptr;
	}
	Actor->Modify();
	Actor->Tags.AddUnique(ManagedWaterTag);
	Actor->Tags.AddUnique(
		FName(*(ManagedWaterIdPrefix + ManagedId)));
	Actor->Tags.AddUnique(
		FName(*(ManagedWaterTypePrefix + Type)));
	if (!Label.IsEmpty())
	{
		Actor->SetActorLabel(Label);
	}
	Actor->SetActorTransform(Transform);
	Actor->MarkPackageDirty();
	return Actor;
}

TSharedRef<FJsonObject> BuildImpactSummary(
	UWorld* World,
	const TArray<FBox>& AffectedBounds,
	const TArray<AActor*>& AffectedActors)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
	TSharedRef<FJsonObject> Partition = MakeShared<FJsonObject>();
	Partition->SetBoolField(TEXT("partitioned"), WorldPartition != nullptr);
	if (WorldPartition)
	{
		Partition->SetBoolField(
			TEXT("streamingEnabled"),
			WorldPartition->IsStreamingEnabled());
		Partition->SetBoolField(
			TEXT("streamingCompleted"),
			WorldPartition->IsStreamingCompleted(nullptr));
	}
	Result->SetObjectField(TEXT("worldPartition"), Partition);

	int32 DataLayerCount = 0;
	int32 RuntimeDataLayerCount = 0;
	if (World)
	{
		if (UDataLayerManager* Manager = World->GetDataLayerManager())
		{
			Manager->ForEachDataLayerInstance(
				[&](UDataLayerInstance* Instance)
				{
					if (Instance)
					{
						++DataLayerCount;
						RuntimeDataLayerCount += Instance->IsRuntime() ? 1 : 0;
					}
					return true;
				});
		}
	}
	TSharedRef<FJsonObject> DataLayers = MakeShared<FJsonObject>();
	DataLayers->SetNumberField(TEXT("total"), DataLayerCount);
	DataLayers->SetNumberField(TEXT("runtime"), RuntimeDataLayerCount);
	TSet<FString> AffectedDataLayerPaths;
	TSet<FString> HLODLayerPaths;
	TSet<FString> AffectedActorPaths;
	for (const AActor* Actor : AffectedActors)
	{
		if (!Actor)
		{
			continue;
		}
		AffectedActorPaths.Add(Actor->GetPathName());
		for (const UDataLayerAsset* Layer : Actor->GetDataLayerAssets())
		{
			if (Layer)
			{
				AffectedDataLayerPaths.Add(Layer->GetPathName());
			}
		}
		if (Actor->GetHLODLayer())
		{
			HLODLayerPaths.Add(Actor->GetHLODLayer()->GetPathName());
		}
	}
	TArray<FString> SortedDataLayerPaths = AffectedDataLayerPaths.Array();
	TArray<FString> SortedHLODLayerPaths = HLODLayerPaths.Array();
	TArray<FString> SortedAffectedActorPaths = AffectedActorPaths.Array();
	SortedDataLayerPaths.Sort();
	SortedHLODLayerPaths.Sort();
	SortedAffectedActorPaths.Sort();
	TArray<TSharedPtr<FJsonValue>> AffectedDataLayers;
	TArray<TSharedPtr<FJsonValue>> HLODLayers;
	TArray<TSharedPtr<FJsonValue>> AffectedActorValues;
	for (const FString& Path : SortedDataLayerPaths)
	{
		AffectedDataLayers.Add(MakeShared<FJsonValueString>(Path));
	}
	for (const FString& Path : SortedHLODLayerPaths)
	{
		HLODLayers.Add(MakeShared<FJsonValueString>(Path));
	}
	for (const FString& Path : SortedAffectedActorPaths)
	{
		AffectedActorValues.Add(MakeShared<FJsonValueString>(Path));
	}
	Result->SetArrayField(TEXT("affectedActors"), AffectedActorValues);
	DataLayers->SetArrayField(TEXT("affected"), AffectedDataLayers);
	Result->SetObjectField(TEXT("dataLayers"), DataLayers);
	TSharedRef<FJsonObject> HLOD = MakeShared<FJsonObject>();
	HLOD->SetArrayField(TEXT("affectedLayers"), HLODLayers);
	HLOD->SetBoolField(
		TEXT("rebuildMayBeRequired"),
		!AffectedBounds.IsEmpty() || !AffectedActors.IsEmpty());
	Result->SetObjectField(TEXT("hlod"), HLOD);

	TSet<FString> PCGComponentPaths;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			const FBox ActorBounds = Actor->GetComponentsBoundingBox(true);
			bool bOverlaps = AffectedBounds.IsEmpty();
			for (const FBox& Bounds : AffectedBounds)
			{
				bOverlaps |= Bounds.IsValid && ActorBounds.IsValid
					&& Bounds.Intersect(ActorBounds);
			}
			if (!bOverlaps)
			{
				continue;
			}
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (Component
					&& Component->GetClass()->GetName().Contains(
						TEXT("PCGComponent")))
				{
					PCGComponentPaths.Add(Component->GetPathName());
					if (PCGComponentPaths.Num() >= 100)
					{
						break;
					}
				}
			}
			if (PCGComponentPaths.Num() >= 100)
			{
				break;
			}
		}
	}
	TArray<FString> SortedPCGPaths = PCGComponentPaths.Array();
	SortedPCGPaths.Sort();
	TArray<TSharedPtr<FJsonValue>> PCGComponents;
	for (const FString& Path : SortedPCGPaths)
	{
		PCGComponents.Add(MakeShared<FJsonValueString>(Path));
	}
	TSharedRef<FJsonObject> PCG = MakeShared<FJsonObject>();
	PCG->SetArrayField(TEXT("components"), PCGComponents);
	PCG->SetNumberField(TEXT("returned"), PCGComponents.Num());
	PCG->SetBoolField(TEXT("truncated"), PCGComponents.Num() >= 100);
	PCG->SetBoolField(
		TEXT("regenerationMayBeRequired"),
		!PCGComponents.IsEmpty());
	Result->SetObjectField(TEXT("pcg"), PCG);
	Result->SetStringField(
		TEXT("evidenceBoundary"),
		TEXT("Impact is a bounded loaded-world overlap and assignment summary; unloaded World Partition actors and downstream build products require their dedicated audits."));
	return Result;
}

TSharedPtr<FJsonObject> BuildLandscapeState(
	const FLandscapeTarget& Target,
	FString& OutError)
{
	OutError.Reset();
	TSharedRef<FJsonObject> State = DescribeLandscape(Target, true);
	// Dirty flags are mutation evidence, not content identity. Keeping them in
	// the semantic digest would make a verified rollback impossible for a
	// package that was clean before the transaction.
	State->RemoveField(TEXT("packageDirty"));
	for (const TSharedPtr<FJsonValue>& ProxyValue :
		State->GetArrayField(TEXT("proxies")))
	{
		const TSharedPtr<FJsonObject> Proxy = ProxyValue->AsObject();
		if (Proxy.IsValid())
		{
			Proxy->RemoveField(TEXT("packageDirty"));
		}
	}
	FIntRect Extent;
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint16> HeightData;
	if (!CaptureHeight(Target, Extent, Width, Height, HeightData))
	{
		OutError =
			TEXT("Landscape height data could not be captured within the 512 MiB snapshot limit.");
		return nullptr;
	}
	TSharedRef<FJsonObject> HeightState = MakeShared<FJsonObject>();
	HeightState->SetNumberField(TEXT("width"), Width);
	HeightState->SetNumberField(TEXT("height"), Height);
	HeightState->SetNumberField(
		TEXT("byteLength"),
		static_cast<double>(HeightData.Num() * sizeof(uint16)));
	HeightState->SetStringField(
		TEXT("sha256"),
		HashBytes(
			HeightData.GetData(),
			static_cast<uint64>(HeightData.Num() * sizeof(uint16))));
	State->SetObjectField(TEXT("heightData"), HeightState);

	TArray<TSharedPtr<FJsonValue>> LayerStates;
	if (ULandscapeInfo* Info = Target.Info.Get())
	{
		for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
		{
			TSharedRef<FJsonObject> LayerState = LayerToJson(Layer, false);
			if (Layer.LayerInfoObj)
			{
				TArray<uint8> Data;
				FIntRect LayerExtent;
				int32 LayerWidth = 0;
				int32 LayerHeight = 0;
				if (!CaptureWeight(
						Target,
						Layer.LayerInfoObj,
						LayerExtent,
						LayerWidth,
						LayerHeight,
						Data))
				{
					OutError = FString::Printf(
						TEXT("Landscape layer '%s' could not be captured."),
						*Layer.GetLayerName().ToString());
					return nullptr;
				}
				LayerState->SetNumberField(TEXT("width"), LayerWidth);
				LayerState->SetNumberField(TEXT("height"), LayerHeight);
				LayerState->SetNumberField(TEXT("byteLength"), Data.Num());
				LayerState->SetStringField(TEXT("sha256"), HashBytes(Data));
			}
			LayerStates.Add(MakeShared<FJsonValueObject>(LayerState));
		}
	}
	LayerStates.Sort(
		[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("name"))
				< Right->AsObject()->GetStringField(TEXT("name"));
		});
	State->SetArrayField(TEXT("layerData"), LayerStates);
	return State;
}

TSharedRef<FJsonObject> BuildPackageEvidence(const TSet<FString>& PackageNames)
{
	TArray<FString> SortedNames = PackageNames.Array();
	SortedNames.Sort();
	TArray<TSharedPtr<FJsonValue>> Packages;
	for (const FString& PackageName : SortedNames)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("package"), PackageName);
		if (const UPackage* LoadedPackage =
			FindPackage(nullptr, *PackageName))
		{
			Item->SetBoolField(TEXT("dirty"), LoadedPackage->IsDirty());
		}
		FString Filename;
		const bool bExists =
			FPackageName::DoesPackageExist(PackageName, &Filename);
		Item->SetBoolField(TEXT("existsOnDisk"), bExists);
		Item->SetStringField(TEXT("filename"), Filename);
		if (bExists)
		{
			const int64 FileSize = IFileManager::Get().FileSize(*Filename);
			Item->SetNumberField(TEXT("sizeBytes"), static_cast<double>(FileSize));
			if (FileSize >= 0 && FileSize <= MaxSnapshotBytes)
			{
				TArray<uint8> Bytes;
				if (FFileHelper::LoadFileToArray(Bytes, *Filename))
				{
					Item->SetStringField(TEXT("sha256"), HashBytes(Bytes));
					Item->SetStringField(TEXT("hashStatus"), TEXT("available"));
				}
				else
				{
					Item->SetStringField(TEXT("hashStatus"), TEXT("readFailed"));
				}
			}
			else
			{
				Item->SetStringField(TEXT("hashStatus"), TEXT("sizeLimit"));
			}
		}
		else
		{
			Item->SetStringField(TEXT("hashStatus"), TEXT("notSaved"));
		}
		Packages.Add(MakeShared<FJsonValueObject>(Item));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("packages"), Packages);
	Result->SetNumberField(TEXT("total"), Packages.Num());
	Result->SetStringField(TEXT("digest"), DigestJson(Result));
	return Result;
}

TSharedRef<FJsonObject> BuildWaterEnvironmentState(
	UWorld* World,
	TSet<FString>& InOutPackageNames)
{
	TMap<UClass*, FString> HelperClasses;
	if (GEditor)
	{
		for (UActorFactory* Factory : GEditor->ActorFactories)
		{
			if (!Factory)
			{
				continue;
			}
			const FString FactoryName = Factory->GetClass()->GetName();
			FString Role;
			if (FactoryName.Contains(TEXT("WaterBrushManagerFactory")))
			{
				Role = TEXT("brushManager");
			}
			else if (FactoryName.Contains(TEXT("WaterZoneActorFactory")))
			{
				Role = TEXT("waterZone");
			}
			if (!Role.IsEmpty())
			{
				if (UClass* ActorClass =
					Factory->GetDefaultActorClass(FAssetData()))
				{
					HelperClasses.Add(ActorClass, Role);
				}
			}
		}
	}
	TArray<TSharedPtr<FJsonValue>> Helpers;
	if (World)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!IsValid(Actor) || IsWaterActor(Actor))
			{
				continue;
			}
			const FString ClassPath = Actor->GetClass()->GetPathName();
			FString Role;
			for (const TPair<UClass*, FString>& Pair : HelperClasses)
			{
				if (Actor->IsA(Pair.Key))
				{
					Role = Pair.Value;
					break;
				}
			}
			if (Role.IsEmpty())
			{
				continue;
			}
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("role"), Role);
			Item->SetStringField(
				TEXT("actorGuid"),
				GuidString(Actor->GetActorGuid()));
			Item->SetStringField(TEXT("class"), ClassPath);
			Item->SetStringField(
				TEXT("level"),
				Actor->GetLevel()
					? Actor->GetLevel()->GetPathName()
					: FString());
			Item->SetStringField(
				TEXT("package"),
				Actor->GetPackage()->GetName());
			Item->SetBoolField(
				TEXT("externalActor"),
				Actor->IsPackageExternal());
			Item->SetStringField(
				TEXT("externalPackage"),
				Actor->IsPackageExternal()
					&& Actor->GetExternalPackage()
					? Actor->GetExternalPackage()->GetName()
					: FString());
			Item->SetObjectField(
				TEXT("transform"),
				TransformToJson(Actor->GetActorTransform()));
			Helpers.Add(MakeShared<FJsonValueObject>(Item));
			InOutPackageNames.Add(Actor->GetPackage()->GetName());
			if (Actor->IsPackageExternal()
				&& Actor->GetExternalPackage())
			{
				InOutPackageNames.Add(
					Actor->GetExternalPackage()->GetName());
			}
		}
	}
	Helpers.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("actorGuid"))
				< Right->AsObject()->GetStringField(TEXT("actorGuid"));
		});

	TArray<TSharedPtr<FJsonValue>> Landscapes;
	if (World)
	{
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			TSharedRef<FJsonObject> Landscape = MakeShared<FJsonObject>();
			Landscape->SetStringField(
				TEXT("landscapeGuid"),
				GuidString(It->GetLandscapeGuid()));
			Landscape->SetStringField(
				TEXT("editingLayerGuid"),
				GuidString(It->GetEditingLayer()));
			TArray<TSharedPtr<FJsonValue>> Layers;
			for (int32 LayerIndex = 0;
				LayerIndex < It->GetLayerCount();
				++LayerIndex)
			{
				const FLandscapeLayer* Layer = It->GetLayer(LayerIndex);
				if (!Layer)
				{
					continue;
				}
				TSharedRef<FJsonObject> LayerState =
					MakeShared<FJsonObject>();
				LayerState->SetNumberField(TEXT("index"), LayerIndex);
				LayerState->SetStringField(
					TEXT("guid"),
					GuidString(Layer->Guid));
				LayerState->SetStringField(
					TEXT("name"),
					Layer->Name.ToString());
				LayerState->SetBoolField(TEXT("locked"), Layer->bLocked);
				LayerState->SetBoolField(TEXT("visible"), Layer->bVisible);
				LayerState->SetNumberField(
					TEXT("heightmapAlpha"),
					Layer->HeightmapAlpha);
				LayerState->SetNumberField(
					TEXT("weightmapAlpha"),
					Layer->WeightmapAlpha);
				LayerState->SetNumberField(
					TEXT("blendMode"),
					static_cast<int32>(Layer->BlendMode.GetValue()));
				TArray<TSharedPtr<FJsonValue>> Brushes;
				for (const FLandscapeLayerBrush& Brush : Layer->Brushes)
				{
					const ALandscapeBlueprintBrushBase* BrushActor =
						Brush.GetBrush();
					TSharedRef<FJsonObject> BrushState =
						MakeShared<FJsonObject>();
					BrushState->SetStringField(
						TEXT("actorGuid"),
						BrushActor
							? GuidString(BrushActor->GetActorGuid())
							: FString());
					BrushState->SetStringField(
						TEXT("class"),
						BrushActor
							? BrushActor->GetClass()->GetPathName()
							: FString());
					Brushes.Add(
						MakeShared<FJsonValueObject>(BrushState));
				}
				LayerState->SetArrayField(TEXT("brushes"), Brushes);
				Layers.Add(MakeShared<FJsonValueObject>(LayerState));
			}
			Landscape->SetArrayField(TEXT("layers"), Layers);
			Landscapes.Add(MakeShared<FJsonValueObject>(Landscape));
			InOutPackageNames.Add(It->GetPackage()->GetName());
		}
	}
	Landscapes.Sort(
		[](const TSharedPtr<FJsonValue>& Left,
			const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()
				->GetStringField(TEXT("landscapeGuid"))
				< Right->AsObject()
					->GetStringField(TEXT("landscapeGuid"));
		});

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("helperActors"), Helpers);
	Result->SetArrayField(TEXT("landscapes"), Landscapes);
	Result->SetStringField(TEXT("digest"), DigestJson(Result));
	return Result;
}

TSharedPtr<FJsonObject> BuildSnapshotForTargets(
	UWorld* World,
	const TArray<FString>& LandscapeGuids,
	const TArray<FString>& WaterIdentities,
	FString& OutError,
	const TSet<FString>* RequiredPackageNames = nullptr)
{
	OutError.Reset();
	TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
	State->SetStringField(
		TEXT("world"),
		World ? World->GetPathName() : TEXT(""));
	TArray<TSharedPtr<FJsonValue>> Landscapes;
	TArray<FBox> AffectedBounds;
	TArray<AActor*> AffectedActors;
	TSet<FString> PackageNames;
	if (RequiredPackageNames)
	{
		for (const FString& PackageName : *RequiredPackageNames)
		{
			PackageNames.Add(PackageName);
		}
	}
	if (World && World->GetPackage())
	{
		PackageNames.Add(World->GetPackage()->GetName());
	}
	for (const FString& Guid : LandscapeGuids)
	{
		FLandscapeTarget Target;
		FString Error;
		FString Code;
		if (!ResolveLandscape(World, Guid, Target, Error, Code))
		{
			OutError = Error;
			return nullptr;
		}
		TSharedPtr<FJsonObject> LandscapeState =
			BuildLandscapeState(Target, Error);
		if (!LandscapeState.IsValid())
		{
			OutError = Error;
			return nullptr;
		}
		Landscapes.Add(MakeShared<FJsonValueObject>(LandscapeState));
		for (const TWeakObjectPtr<ALandscapeProxy>& WeakProxy : Target.Proxies)
		{
			if (const ALandscapeProxy* Proxy = WeakProxy.Get())
			{
				PackageNames.Add(Proxy->GetPackage()->GetName());
			}
		}
		if (ALandscapeProxy* Proxy = Target.Representative.Get())
		{
			AffectedActors.AddUnique(Proxy);
			if (const UMaterialInterface* Material =
				Proxy->GetLandscapeMaterial())
			{
				PackageNames.Add(Material->GetPackage()->GetName());
			}
		}
		if (const ULandscapeInfo* Info = Target.Info.Get())
		{
			for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
			{
				if (Layer.LayerInfoObj)
				{
					PackageNames.Add(
						Layer.LayerInfoObj->GetPackage()->GetName());
				}
			}
		}
		if (Target.Info.IsValid())
		{
			AffectedBounds.Add(Target.Info->GetLoadedBounds());
		}
	}
	Landscapes.Sort(
		[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("landscapeId"))
				< Right->AsObject()->GetStringField(TEXT("landscapeId"));
		});
	State->SetArrayField(TEXT("landscapes"), Landscapes);

	TArray<TSharedPtr<FJsonValue>> Water;
	for (const FString& Identity : WaterIdentities)
	{
		AActor* Actor = ResolveWater(World, Identity);
		FWaterState WaterState;
		if (Actor)
		{
			WaterState = CaptureWaterState(Actor);
			AffectedActors.Add(Actor);
			AffectedBounds.Add(Actor->GetComponentsBoundingBox(true));
			PackageNames.Add(Actor->GetPackage()->GetName());
			if (Actor->IsPackageExternal()
				&& Actor->GetExternalPackage())
			{
				PackageNames.Add(
					Actor->GetExternalPackage()->GetName());
			}
		}
		else
		{
			WaterState.Identity = Identity;
			WaterState.ManagedId = Identity;
		}
		Water.Add(
			MakeShared<FJsonValueObject>(
				WaterStateToJson(WaterState, false)));
	}
	Water.Sort(
		[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("identity"))
				< Right->AsObject()->GetStringField(TEXT("identity"));
		});
	State->SetArrayField(TEXT("water"), Water);
	State->SetObjectField(
		TEXT("waterEnvironment"),
		BuildWaterEnvironmentState(World, PackageNames));
	State->SetObjectField(
		TEXT("impact"),
		BuildImpactSummary(World, AffectedBounds, AffectedActors));

	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema"), TEXT("ue.landscape-snapshot.v1"));
	Snapshot->SetObjectField(TEXT("state"), State);
	Snapshot->SetStringField(TEXT("snapshotDigest"), DigestJson(State));
	Snapshot->SetObjectField(
		TEXT("packageEvidence"),
		BuildPackageEvidence(PackageNames));
	return Snapshot;
}

TSet<FString> PackageNamesFromSnapshot(
	const TSharedPtr<FJsonObject>& Snapshot)
{
	TSet<FString> Names;
	const TSharedPtr<FJsonObject>* Evidence = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (!Snapshot.IsValid()
		|| !Snapshot->TryGetObjectField(
			TEXT("packageEvidence"),
			Evidence)
		|| !Evidence
		|| !(*Evidence)->TryGetArrayField(TEXT("packages"), Packages)
		|| !Packages)
	{
		return Names;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Packages)
	{
		const TSharedPtr<FJsonObject> Package = Value->AsObject();
		FString Name;
		if (Package.IsValid()
			&& Package->TryGetStringField(TEXT("package"), Name)
			&& !Name.IsEmpty())
		{
			Names.Add(Name);
		}
	}
	return Names;
}

bool VerifySnapshotDigest(
	const TSharedPtr<FJsonObject>& Snapshot,
	FString& OutDigest)
{
	OutDigest.Reset();
	const TSharedPtr<FJsonObject>* State = nullptr;
	FString Claimed;
	if (!Snapshot.IsValid()
		|| !Snapshot->TryGetObjectField(TEXT("state"), State)
		|| !State
		|| !State->IsValid()
		|| !Snapshot->TryGetStringField(TEXT("snapshotDigest"), Claimed))
	{
		return false;
	}
	OutDigest = DigestJson(*State);
	return !OutDigest.IsEmpty() && OutDigest == Claimed;
}

void CollectChangedPaths(
	const TSharedPtr<FJsonValue>& Before,
	const TSharedPtr<FJsonValue>& After,
	const FString& Path,
	TArray<FString>& OutPaths,
	int32& OutTotal)
{
	if (CanonicalizeJsonValue(Before) == CanonicalizeJsonValue(After))
	{
		return;
	}
	if (Before.IsValid() && After.IsValid()
		&& Before->Type == EJson::Object
		&& After->Type == EJson::Object)
	{
		TSet<FString> Keys;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
			Before->AsObject()->Values)
		{
			Keys.Add(Pair.Key);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
			After->AsObject()->Values)
		{
			Keys.Add(Pair.Key);
		}
		TArray<FString> SortedKeys = Keys.Array();
		SortedKeys.Sort();
		for (const FString& Key : SortedKeys)
		{
			CollectChangedPaths(
				Before->AsObject()->Values.FindRef(Key),
				After->AsObject()->Values.FindRef(Key),
				Path + TEXT("/") + Key,
				OutPaths,
				OutTotal);
		}
		return;
	}
	++OutTotal;
	if (OutPaths.Num() < MaxDiffPaths)
	{
		OutPaths.Add(Path.IsEmpty() ? TEXT("/") : Path);
	}
}

FString LandscapeReadString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Fallback = FString())
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Field, Value)
		? Value
		: Fallback;
}

bool ReadBool(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const bool Fallback = false)
{
	bool Value = Fallback;
	if (Object.IsValid())
	{
		Object->TryGetBoolField(Field, Value);
	}
	return Value;
}

UMaterialInterface* LoadMaterial(const FString& Path)
{
	return Path.IsEmpty()
		? nullptr
		: LoadObject<UMaterialInterface>(nullptr, *Path);
}

void SetLandscapeMaterialEditor(
	ALandscapeProxy* Proxy,
	UMaterialInterface* Material)
{
	if (!Proxy || !Proxy->GetWorld() || Proxy->GetWorld()->IsGameWorld())
	{
		return;
	}
	// ALandscapeProxy::EditorSetLandscapeMaterial is reflected but is not
	// exported from UE 5.3's Landscape module. Mirror its small editor-only
	// implementation through the public property and standard property-change
	// notification so packaged plugins do not acquire an unresolved symbol.
	Proxy->LandscapeMaterial = Material;
	FPropertyChangedEvent Changed(
		FindFProperty<FProperty>(
			ALandscapeProxy::StaticClass(),
			GET_MEMBER_NAME_CHECKED(
				ALandscapeProxy,
				LandscapeMaterial)));
	Proxy->PostEditChangeProperty(Changed);
}

ULandscapeLayerInfoObject* LoadLayerInfo(const FString& Path)
{
	return Path.IsEmpty()
		? nullptr
		: LoadObject<ULandscapeLayerInfoObject>(nullptr, *Path);
}

void MarkLandscapeTransactional(const FLandscapeTarget& Target)
{
	for (const TWeakObjectPtr<ALandscapeProxy>& WeakProxy : Target.Proxies)
	{
		if (ALandscapeProxy* Proxy = WeakProxy.Get())
		{
			Proxy->Modify();
			TInlineComponentArray<ULandscapeComponent*> Components(Proxy);
			for (ULandscapeComponent* Component : Components)
			{
				if (Component)
				{
					Component->Modify();
				}
			}
		}
	}
}

bool SaveRasterBackup(
	const FString& Directory,
	const FString& Stem,
	const TArray<uint8>& Bytes,
	FString& OutPath,
	FString& OutSha)
{
	IFileManager::Get().MakeDirectory(*Directory, true);
	OutPath = FPaths::Combine(Directory, Stem);
	OutSha = HashBytes(Bytes);
	return !OutSha.IsEmpty()
		&& FFileHelper::SaveArrayToFile(Bytes, *OutPath);
}

bool ReadRasterBackup(
	const FRasterBackup& Backup,
	TArray<uint8>& OutBytes)
{
	return FFileHelper::LoadFileToArray(OutBytes, *Backup.Path)
		&& HashBytes(OutBytes) == Backup.Sha256;
}

bool SaveWaterActorBackup(
	const FString& Directory,
	const FString& Stem,
	AActor* Actor,
	FWaterBackup& OutBackup,
	FString& OutError)
{
	OutError.Reset();
	FString ExportText;
	FString ExportDigest;
	int64 ExportBytes = 0;
	if (!ExportWaterActorText(
			Actor,
			ExportText,
			ExportDigest,
			ExportBytes,
			OutError))
	{
		return false;
	}
	if (ExportBytes <= 0 || ExportBytes > MaxSnapshotBytes)
	{
		OutError =
			TEXT("The Water actor recovery export exceeds the 512 MiB limit.");
		return false;
	}
	if (OutBackup.Before.PropertyDigest.IsEmpty()
		|| ExportDigest != OutBackup.Before.PropertyDigest
		|| ExportBytes != OutBackup.Before.PropertyBytes)
	{
		OutError =
			TEXT("The Water actor changed while its recovery artifact was being captured.");
		return false;
	}
	IFileManager::Get().MakeDirectory(*Directory, true);
	OutBackup.ActorExportPath =
		FPaths::Combine(Directory, Stem + TEXT(".t3d"));
	const FTCHARToUTF8 ArtifactUtf8(*ExportText);
	OutBackup.ActorExportSha256 = HashBytes(
		ArtifactUtf8.Get(),
		static_cast<uint64>(ArtifactUtf8.Length()));
	if (OutBackup.ActorExportSha256.IsEmpty())
	{
		OutError =
			TEXT("The Water actor recovery artifact digest could not be computed.");
		return false;
	}
	if (!FFileHelper::SaveStringToFile(
			ExportText,
			*OutBackup.ActorExportPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("The Water actor recovery artifact could not be written.");
		return false;
	}
	TArray<uint8> PersistedBytes;
	if (!FFileHelper::LoadFileToArray(
			PersistedBytes,
			*OutBackup.ActorExportPath)
		|| HashBytes(PersistedBytes) != OutBackup.ActorExportSha256)
	{
		OutError =
			TEXT("The persisted Water actor recovery artifact failed digest verification.");
		IFileManager::Get().Delete(*OutBackup.ActorExportPath);
		OutBackup.ActorExportPath.Reset();
		OutBackup.ActorExportSha256.Reset();
		return false;
	}
	return true;
}

bool ReadWaterActorBackup(
	const FWaterBackup& Backup,
	FString& OutExportText,
	FString& OutError)
{
	OutExportText.Reset();
	OutError.Reset();
	TArray<uint8> Bytes;
	if (Backup.ActorExportPath.IsEmpty()
		|| Backup.ActorExportSha256.IsEmpty()
		|| !FFileHelper::LoadFileToArray(Bytes, *Backup.ActorExportPath)
		|| HashBytes(Bytes) != Backup.ActorExportSha256)
	{
		OutError =
			TEXT("The Water actor recovery artifact is missing or has a digest mismatch.");
		return false;
	}
	const FUTF8ToTCHAR Converter(
		reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
		Bytes.Num());
	OutExportText = FString(Converter.Length(), Converter.Get());
	if (OutExportText.IsEmpty())
	{
		OutError = TEXT("The Water actor recovery artifact is empty.");
		return false;
	}
	return true;
}

TArray<FName> JsonTags(const TSharedPtr<FJsonObject>& Object)
{
	TArray<FName> Tags;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object.IsValid()
		&& Object->TryGetArrayField(TEXT("tags"), Values)
		&& Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Tags.Add(FName(*Value->AsString()));
		}
	}
	return Tags;
}

FWaterState WaterStateFromJson(const TSharedPtr<FJsonObject>& Object)
{
	FWaterState State;
	if (!Object.IsValid())
	{
		return State;
	}
	State.bPresent = ReadBool(Object, TEXT("present"));
	State.Identity = LandscapeReadString(Object, TEXT("identity"));
	State.ManagedId = LandscapeReadString(Object, TEXT("managedId"));
	State.Type = LandscapeReadString(Object, TEXT("type"));
	State.Name = LandscapeReadString(Object, TEXT("name"));
	State.Label = LandscapeReadString(Object, TEXT("label"));
	State.ClassPath = LandscapeReadString(Object, TEXT("class"));
	State.Name = LandscapeReadString(
		Object,
		TEXT("objectName"),
		LandscapeReadString(Object, TEXT("name")));
	State.ActorGuid = LandscapeReadString(Object, TEXT("actorGuid"));
	State.LevelPath = LandscapeReadString(Object, TEXT("level"));
	State.PackageName = LandscapeReadString(Object, TEXT("package"));
	State.ExternalPackageName =
		LandscapeReadString(Object, TEXT("externalPackage"));
	State.PropertyDigest =
		LandscapeReadString(Object, TEXT("propertyDigest"));
	double PropertyBytes = 0.0;
	if (Object->TryGetNumberField(TEXT("propertyBytes"), PropertyBytes)
		&& FMath::IsFinite(PropertyBytes)
		&& PropertyBytes >= 0.0)
	{
		State.PropertyBytes = static_cast<int64>(PropertyBytes);
	}
	State.bManaged = ReadBool(Object, TEXT("managed"));
	State.bExternalActor = ReadBool(Object, TEXT("externalActor"));
	const TSharedPtr<FJsonObject>* Transform = nullptr;
	FString IgnoredError;
	if (Object->TryGetObjectField(TEXT("transform"), Transform)
		&& Transform
		&& Transform->IsValid())
	{
		ParseTransform(*Transform, State.Transform, IgnoredError);
	}
	State.Tags = JsonTags(Object);
	return State;
}

bool DeleteManagedWater(
	UWorld* World,
	AActor* Actor,
	FString& OutError)
{
	OutError.Reset();
	if (!World || !Actor || Actor->GetWorld() != World || !IsManagedWater(Actor))
	{
		OutError = TEXT("Only a loaded managed Water actor can be deleted.");
		return false;
	}
	const FString Identity = WaterIdentity(Actor);
	const FName OriginalName = Actor->GetFName();
	const FName TombstoneName = MakeUniqueObjectName(
		Actor->GetOuter(),
		Actor->GetClass(),
		FName(*(Actor->GetName() + TEXT("_UEAIDeleted"))));
	Actor->Modify();
	if (ULevel* Level = Actor->GetLevel())
	{
		Level->Modify();
	}
	if (!Actor->Rename(
			*TombstoneName.ToString(),
			Actor->GetOuter(),
			REN_DontCreateRedirectors | REN_ForceNoResetLoaders))
	{
		OutError =
			TEXT("The managed Water actor name could not be reserved for recovery.");
		return false;
	}
	TArray<UObject*> DeletedObjects;
	ForEachObjectWithOuter(
		Actor,
		[&DeletedObjects](UObject* Object)
		{
			if (Object)
			{
				DeletedObjects.Add(Object);
			}
		},
		true);
	DeletedObjects.Add(Actor);
	if (!World->EditorDestroyActor(Actor, true))
	{
		Actor->Rename(
			*OriginalName.ToString(),
			Actor->GetOuter(),
			REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
		OutError = TEXT("The managed Water actor could not be deleted.");
		return false;
	}
	// EditorDestroyActor removes the actor from the level immediately but its
	// renamed subobjects can remain discoverable until the next GC. Recovery
	// imports actor text in the same tick; leaving those objects searchable can
	// resolve a Water mesh or component to the tombstone instead of the actor
	// being restored. This delete is backed by a persisted T3D artifact, so the
	// old object graph is deliberately made unreachable rather than retained as
	// an Undo-only copy.
	for (UObject* Object : DeletedObjects)
	{
		if (Object && !Object->IsRooted())
		{
			Object->MarkAsGarbage();
		}
	}
	if (ResolveWater(World, Identity))
	{
		OutError =
			TEXT("The managed Water actor remained discoverable after deletion.");
		return false;
	}
	return true;
}

bool RestoreDeletedWater(
	UWorld* World,
	const FWaterBackup& Backup,
	FString& OutError)
{
	OutError.Reset();
	const FWaterState& Before = Backup.Before;
	if (!World
		|| !Before.bPresent
		|| !Before.bManaged
		|| Before.ManagedId.IsEmpty()
		|| Before.Name.IsEmpty()
		|| Before.ClassPath.IsEmpty()
		|| Before.ActorGuid.IsEmpty()
		|| Before.LevelPath.IsEmpty()
		|| Before.PropertyDigest.IsEmpty())
	{
		OutError = TEXT("The Water actor recovery identity is incomplete.");
		return false;
	}
	if (ResolveWater(World, Before.Identity))
	{
		OutError =
			TEXT("The deleted Water identity was reused before recovery.");
		return false;
	}

	FString ExportText;
	if (!ReadWaterActorBackup(Backup, ExportText, OutError))
	{
		return false;
	}
	FString PropertyText;
	if (!ExtractSingleActorPropertyText(
			ExportText,
			PropertyText,
			OutError))
	{
		return false;
	}
	UClass* ActorClass = LoadObject<UClass>(nullptr, *Before.ClassPath);
	ULevel* Level = FindObject<ULevel>(nullptr, *Before.LevelPath);
	FGuid ActorGuid;
	if (!ActorClass
		|| !ActorClass->IsChildOf(AActor::StaticClass())
		|| !Level
		|| Level->GetWorld() != World
		|| !FGuid::Parse(Before.ActorGuid, ActorGuid)
		|| !ActorGuid.IsValid())
	{
		OutError =
			TEXT("The Water actor class, level, or actor GUID is unavailable.");
		return false;
	}
	UActorFactory* Factory = FindWaterFactory(Before.Type);
	if (!Factory
		|| Factory->GetDefaultActorClass(FAssetData()) != ActorClass)
	{
		OutError =
			TEXT("The matching Water actor factory is unavailable for recovery.");
		return false;
	}

	UPackage* ExternalPackage = nullptr;
	if (Before.bExternalActor)
	{
		if (Before.ExternalPackageName.IsEmpty())
		{
			OutError =
				TEXT("The external Water actor package identity is unavailable.");
			return false;
		}
		ExternalPackage =
			FindPackage(nullptr, *Before.ExternalPackageName);
		if (!ExternalPackage)
		{
			ExternalPackage = CreatePackage(*Before.ExternalPackageName);
			if (ExternalPackage)
			{
				ExternalPackage->SetPackageFlags(
					PKG_ContainsMapData);
			}
		}
		if (!ExternalPackage)
		{
			OutError =
				TEXT("The external Water actor package could not be recreated.");
			return false;
		}
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = FName(*Before.Name);
	SpawnParameters.OverrideLevel = Level;
	SpawnParameters.OverridePackage = ExternalPackage;
	SpawnParameters.OverrideActorGuid = ActorGuid;
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Recreate through the same Water actor factory as normal placement. A raw
	// UWorld::SpawnActor misses the factory's PostSpawnActor initialization,
	// including generated Water info meshes and collision state required for a
	// scoped T3D import. Keep the actor marked as a preview only while the
	// editor's OnLevelActorAdded hook runs, preventing it from adding a second
	// Water Zone/brush manager/edit layer. Factory PostSpawnActor still runs,
	// and the derived render/collision data is rebuilt after property import.
	SpawnParameters.ObjectFlags = RF_Transactional;
	SpawnParameters.CustomPreSpawnInitalization =
		[](AActor* SpawnedActor)
		{
			if (SpawnedActor)
			{
				SpawnedActor->bIsEditorPreviewActor = true;
			}
		};
	SpawnParameters.bCreateActorPackage = false;
	SpawnParameters.NameMode =
		FActorSpawnParameters::ESpawnActorNameMode::
			Required_ErrorAndReturnNull;
	AActor* Restored = Factory->CreateActor(
		ActorClass,
		Level,
		Before.Transform,
		SpawnParameters);
	if (!Restored)
	{
		OutError =
			TEXT("The deleted Water actor could not be recreated with its original identity.");
		return false;
	}
	Restored->bIsEditorPreviewActor = false;
	auto DestroyPartialRestore = [World, Restored]()
	{
		if (World && IsValid(Restored))
		{
			const FName TombstoneName = MakeUniqueObjectName(
				Restored->GetOuter(),
				Restored->GetClass(),
				FName(*(Restored->GetName() + TEXT("_UEAIRestoreFailed"))));
			Restored->Rename(
				*TombstoneName.ToString(),
				Restored->GetOuter(),
				REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
			World->EditorDestroyActor(Restored, true);
		}
	};

	Restored->PreEditChange(nullptr);
	FImportObjectParams ImportParams;
	ImportParams.DestData = reinterpret_cast<uint8*>(Restored);
	ImportParams.SourceText = *PropertyText;
	ImportParams.ObjectStruct = Restored->GetClass();
	ImportParams.SubobjectRoot = Restored;
	ImportParams.SubobjectOuter = Restored;
	ImportParams.Warn = GWarn;
	ImportParams.bShouldCallEditChange = false;
	if (!ImportObjectProperties(ImportParams))
	{
		DestroyPartialRestore();
		OutError =
			TEXT("The Water actor recovery properties could not be imported.");
		return false;
	}
	Restored->PostEditImport();
	Restored->PostEditChange();
	Restored->SetActorTransform(Before.Transform);
	Restored->SetActorLabel(Before.Label);
	Restored->Tags = Before.Tags;
	Restored->CheckDefaultSubobjects();
	Restored->InvalidateLightingCache();
	Restored->PostEditMove(true);
#if WITH_UEAI_WATER
	if (AWaterBody* RestoredWater = Cast<AWaterBody>(Restored))
	{
		if (UWaterBodyComponent* WaterComponent =
				RestoredWater->GetWaterBodyComponent())
		{
			FOnWaterBodyChangedParams ChangedParams;
			ChangedParams.bShapeOrPositionChanged = true;
			ChangedParams.bWeightmapSettingsChanged = true;
			WaterComponent->UpdateAll(ChangedParams);
			WaterComponent->UpdateWaterBodyRenderData();
		}
	}
#endif
	Restored->MarkPackageDirty();

	const FWaterState RestoredState = CaptureWaterState(Restored);
	const bool bPackageMatches =
		RestoredState.PackageName == Before.PackageName
		&& RestoredState.bExternalActor == Before.bExternalActor
		&& RestoredState.ExternalPackageName
			== Before.ExternalPackageName;
	TArray<FString> MismatchedFields;
	if (RestoredState.Identity != Before.Identity)
	{
		MismatchedFields.Add(TEXT("identity"));
	}
	if (RestoredState.ManagedId != Before.ManagedId)
	{
		MismatchedFields.Add(TEXT("managedId"));
	}
	if (RestoredState.ClassPath != Before.ClassPath)
	{
		MismatchedFields.Add(TEXT("class"));
	}
	if (RestoredState.ActorGuid != Before.ActorGuid)
	{
		MismatchedFields.Add(TEXT("actorGuid"));
	}
	if (RestoredState.LevelPath != Before.LevelPath)
	{
		MismatchedFields.Add(TEXT("level"));
	}
	if (RestoredState.PropertyDigest != Before.PropertyDigest)
	{
		FString RestoredExportText;
		FString RestoredExportDigest;
		int64 RestoredExportBytes = 0;
		FString RestoredExportError;
		TArray<FString> DifferenceLabels;
		if (ExportWaterActorText(
				Restored,
				RestoredExportText,
				RestoredExportDigest,
				RestoredExportBytes,
				RestoredExportError))
		{
			FString BeforeCanonical;
			FString RestoredCanonical;
			CanonicalizeWaterActorText(ExportText, BeforeCanonical);
			CanonicalizeWaterActorText(
				RestoredExportText,
				RestoredCanonical);
			TArray<FString> BeforeLines;
			TArray<FString> RestoredLines;
			BeforeCanonical.ParseIntoArrayLines(BeforeLines, false);
			RestoredCanonical.ParseIntoArrayLines(
				RestoredLines,
				false);
			const int32 LineCount =
				FMath::Max(BeforeLines.Num(), RestoredLines.Num());
			for (int32 Index = 0;
				Index < LineCount && DifferenceLabels.Num() < 8;
				++Index)
			{
				const FString BeforeLine =
					BeforeLines.IsValidIndex(Index)
						? BeforeLines[Index].TrimStartAndEnd()
						: TEXT("<missing>");
				const FString RestoredLine =
					RestoredLines.IsValidIndex(Index)
						? RestoredLines[Index].TrimStartAndEnd()
						: TEXT("<missing>");
				if (BeforeLine == RestoredLine)
				{
					continue;
				}
				auto LineLabel = [](const FString& Value)
				{
					return Value.Left(320);
				};
				DifferenceLabels.AddUnique(
					LineLabel(BeforeLine)
					+ TEXT(" -> ")
					+ LineLabel(RestoredLine));
			}
		}
		MismatchedFields.Add(
			DifferenceLabels.IsEmpty()
				? TEXT("propertyDigest")
				: FString::Printf(
					TEXT("propertyDigest[%s]"),
					*FString::Join(DifferenceLabels, TEXT("; "))));
	}
	if (!bPackageMatches)
	{
		MismatchedFields.Add(TEXT("package"));
	}
	if (!MismatchedFields.IsEmpty())
	{
		DestroyPartialRestore();
		OutError = FString::Printf(
			TEXT("The recreated Water actor does not match its recovery snapshot fields: %s."),
			*FString::Join(MismatchedFields, TEXT(", ")));
		return false;
	}
	return true;
}

bool IsExternalPackageAttached(
	const UWorld* World,
	const UPackage* ExternalPackage)
{
	if (!World || !ExternalPackage)
	{
		return false;
	}
	if (World->GetPackage()
		&& World->GetPackage()->GetExternalPackages().Contains(
			const_cast<UPackage*>(ExternalPackage)))
	{
		return true;
	}
	return World->PersistentLevel
		&& World->PersistentLevel
			->GetLoadedExternalObjectPackages()
			.Contains(const_cast<UPackage*>(ExternalPackage));
}

bool RemoveCreatedWater(
	UWorld* World,
	const FWaterBackup& Backup,
	FString& OutError)
{
	OutError.Reset();
	const FWaterState& Created =
		Backup.Created.bPresent ? Backup.Created : Backup.Before;
	const FString Identity = !Created.Identity.IsEmpty()
		? Created.Identity
		: Created.ManagedId;
	AActor* Current = ResolveWater(World, Identity);
	UPackage* ExternalPackage = nullptr;
	FString ExternalPackageName = Created.ExternalPackageName;
	if (Current)
	{
		if (Created.bPresent
			&& (Current->GetActorGuid().ToString(
					EGuidFormats::DigitsWithHyphensLower)
					!= Created.ActorGuid
				|| Current->GetClass()->GetPathName()
					!= Created.ClassPath))
		{
			OutError =
				TEXT("The created Water identity was reused before rollback.");
			return false;
		}
		if (Current->IsPackageExternal())
		{
			ExternalPackage = Current->GetExternalPackage();
			if (ExternalPackageName.IsEmpty() && ExternalPackage)
			{
				ExternalPackageName = ExternalPackage->GetName();
			}
			if (!Created.ExternalPackageName.IsEmpty()
				&& (!ExternalPackage
					|| ExternalPackage->GetName()
						!= Created.ExternalPackageName))
			{
				OutError =
					TEXT("The created Water external package changed before rollback.");
				return false;
			}
			// Detach the actor before destruction so the World package no
			// longer retains the now-empty external package.
			Current->SetPackageExternal(false, false);
		}
		if (!World->EditorDestroyActor(Current, true))
		{
			OutError = TEXT("A created Water actor could not be removed.");
			return false;
		}
	}
	if (ResolveWater(World, Identity))
	{
		OutError =
			TEXT("A created Water actor remained discoverable after rollback.");
		return false;
	}
	if (!ExternalPackage && !ExternalPackageName.IsEmpty())
	{
		ExternalPackage = FindPackage(nullptr, *ExternalPackageName);
	}
	if (ExternalPackage
		&& IsExternalPackageAttached(World, ExternalPackage))
	{
		OutError =
			TEXT("The created Water external package remained attached after rollback.");
		return false;
	}
	if (!ExternalPackageName.IsEmpty())
	{
		FString ExistingFilename;
		if (FPackageName::DoesPackageExist(
				ExternalPackageName,
				&ExistingFilename))
		{
			OutError =
				TEXT("The created Water external actor package exists on disk; rollback will not delete an externally saved package.");
			return false;
		}
	}
	if (ExternalPackage)
	{
		TArray<TWeakObjectPtr<UObject>> ExternalObjects;
		ForEachObjectWithPackage(
			ExternalPackage,
			[&ExternalObjects](UObject* Object)
			{
				if (Object)
				{
					ExternalObjects.Add(Object);
				}
				return true;
			});
		// EditorDestroyActor removes the actor from the world immediately, but
		// its components remain valid until the next GC. This package is a
		// per-actor World Partition package created by this operation, so make
		// those unreachable remnants garbage before verifying that the package
		// can be discarded.
		for (const TWeakObjectPtr<UObject>& WeakObject : ExternalObjects)
		{
			if (UObject* Object = WeakObject.Get())
			{
				Object->MarkAsGarbage();
			}
		}
		bool bHasLiveExternalObjects = false;
		for (const TWeakObjectPtr<UObject>& WeakObject : ExternalObjects)
		{
			if (IsValid(WeakObject.Get()))
			{
				bHasLiveExternalObjects = true;
				break;
			}
		}
		if (bHasLiveExternalObjects)
		{
			OutError =
				TEXT("The created Water external package still owns live objects after rollback.");
			return false;
		}
		ExternalPackage->SetDirtyFlag(false);
		ExternalPackage->MarkAsGarbage();
	}
	return true;
}

bool RemoveCreatedWaterSideEffectActor(
	UWorld* World,
	const FWaterSideEffectActor& SideEffect,
	FString& OutError)
{
	OutError.Reset();
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (GuidString(It->GetActorGuid()) == SideEffect.ActorGuid)
		{
			Actor = *It;
			break;
		}
	}
	if (!Actor)
	{
		return true;
	}
	const FWaterSideEffectActor Current =
		CaptureWaterSideEffectActor(Actor);
	if (Current.ClassPath != SideEffect.ClassPath
		|| Current.LevelPath != SideEffect.LevelPath
		|| Current.PackageName != SideEffect.PackageName
		|| Current.bExternalActor != SideEffect.bExternalActor
		|| Current.ExternalPackageName != SideEffect.ExternalPackageName)
	{
		OutError =
			TEXT("A Water placement side-effect actor changed before rollback.");
		return false;
	}

	if (ALandscapeBlueprintBrushBase* Brush =
			Cast<ALandscapeBlueprintBrushBase>(Actor))
	{
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			if (It->GetBrushLayer(Brush) != INDEX_NONE)
			{
				It->Modify();
				It->RemoveBrush(Brush);
			}
		}
	}

	UPackage* ExternalPackage =
		Actor->IsPackageExternal()
			? Actor->GetExternalPackage()
			: nullptr;
	if (ExternalPackage)
	{
		Actor->SetPackageExternal(false, false);
	}
	if (!World->EditorDestroyActor(Actor, true))
	{
		OutError =
			TEXT("A Water placement side-effect actor could not be removed.");
		return false;
	}
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (GuidString(It->GetActorGuid()) == SideEffect.ActorGuid)
		{
			OutError =
				TEXT("A Water placement side-effect actor remained after rollback.");
			return false;
		}
	}
	if (!ExternalPackage && !SideEffect.ExternalPackageName.IsEmpty())
	{
		ExternalPackage =
			FindPackage(nullptr, *SideEffect.ExternalPackageName);
	}
	if (ExternalPackage
		&& IsExternalPackageAttached(World, ExternalPackage))
	{
		OutError =
			TEXT("A Water placement side-effect package remained attached after rollback.");
		return false;
	}
	if (!SideEffect.ExternalPackageName.IsEmpty()
		&& FPackageName::DoesPackageExist(
			SideEffect.ExternalPackageName))
	{
		OutError =
			TEXT("A Water placement side-effect package exists on disk and was not deleted.");
		return false;
	}
	if (ExternalPackage)
	{
		TArray<TWeakObjectPtr<UObject>> Objects;
		ForEachObjectWithPackage(
			ExternalPackage,
			[&Objects](UObject* Object)
			{
				if (Object)
				{
					Objects.Add(Object);
				}
				return true;
			});
		for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
		{
			if (UObject* Object = WeakObject.Get())
			{
				Object->MarkAsGarbage();
			}
		}
		for (const TWeakObjectPtr<UObject>& WeakObject : Objects)
		{
			if (IsValid(WeakObject.Get()))
			{
				OutError =
					TEXT("A Water placement side-effect package still owns live objects.");
				return false;
			}
		}
		ExternalPackage->SetDirtyFlag(false);
		ExternalPackage->MarkAsGarbage();
	}
	return true;
}

bool RemoveWaterCreateSideEffects(
	UWorld* World,
	const FWaterBackup& Backup,
	FString& OutError)
{
	OutError.Reset();
	for (int32 Index = Backup.CreatedSideEffectActors.Num() - 1;
		Index >= 0;
		--Index)
	{
		if (!RemoveCreatedWaterSideEffectActor(
				World,
				Backup.CreatedSideEffectActors[Index],
				OutError))
		{
			return false;
		}
	}
	for (int32 Index = Backup.CreatedLandscapeLayers.Num() - 1;
		Index >= 0;
		--Index)
	{
		const FWaterLayerSideEffect& SideEffect =
			Backup.CreatedLandscapeLayers[Index];
		ALandscape* Landscape = nullptr;
		for (TActorIterator<ALandscape> It(World); It; ++It)
		{
			if (GuidString(It->GetLandscapeGuid())
				== SideEffect.LandscapeGuid)
			{
				Landscape = *It;
				break;
			}
		}
		if (!Landscape)
		{
			OutError =
				TEXT("A Landscape modified by Water placement is unavailable.");
			return false;
		}
		const int32 LayerIndex =
			Landscape->GetLayerIndex(FName(TEXT("Water")));
		if (LayerIndex == INDEX_NONE)
		{
			continue;
		}
		const FLandscapeLayer* Layer = Landscape->GetLayer(LayerIndex);
		if (!Layer
			|| GuidString(Layer->Guid) != SideEffect.LayerGuid)
		{
			OutError =
				TEXT("The Water Landscape layer changed before rollback.");
			return false;
		}
		if (!Layer->Brushes.IsEmpty())
		{
			OutError =
				TEXT("The Water Landscape layer still contains external brushes.");
			return false;
		}
		Landscape->Modify();
		Landscape->DeleteLayer(LayerIndex);
		if (Landscape->GetLayerIndex(FName(TEXT("Water")))
			!= INDEX_NONE)
		{
			OutError =
				TEXT("The Water Landscape layer remained after rollback.");
			return false;
		}
	}
	return true;
}

bool RestoreWaterState(
	UWorld* World,
	const FWaterBackup& Backup,
	FString& OutError)
{
	OutError.Reset();
	const FWaterState& Before = Backup.Before;
	const FString& Action = Backup.Action;
	AActor* Current = ResolveWater(World, Before.Identity);
	if (Action == TEXT("waterCreate"))
	{
		if (!RemoveCreatedWater(World, Backup, OutError))
		{
			return false;
		}
		return RemoveWaterCreateSideEffects(World, Backup, OutError);
	}
	if (!Before.bPresent)
	{
		return true;
	}
	if (Action == TEXT("waterDelete"))
	{
		if (Current)
		{
			const FWaterState CurrentState = CaptureWaterState(Current);
			const bool bAlreadyRestored =
				CurrentState.Identity == Before.Identity
				&& CurrentState.ClassPath == Before.ClassPath
				&& CurrentState.ActorGuid == Before.ActorGuid
				&& CurrentState.LevelPath == Before.LevelPath
				&& CurrentState.PackageName == Before.PackageName
				&& CurrentState.ExternalPackageName
					== Before.ExternalPackageName
				&& CurrentState.PropertyDigest
					== Before.PropertyDigest;
			if (!bAlreadyRestored)
			{
				OutError =
					TEXT("The deleted Water identity was reused with different state before recovery.");
			}
			return bAlreadyRestored;
		}
		return RestoreDeletedWater(World, Backup, OutError);
	}
	if (!Current)
	{
		Current = SpawnManagedWater(
			Before.Type,
			Before.ManagedId,
			Before.Label,
			Before.Transform,
			OutError);
		if (!Current)
		{
			return false;
		}
	}
	Current->Modify();
	Current->SetActorTransform(Before.Transform);
	Current->SetActorLabel(Before.Label);
	Current->Tags = Before.Tags;
	Current->MarkPackageDirty();
	const FWaterState RestoredState = CaptureWaterState(Current);
	const bool bMatches =
		RestoredState.Identity == Before.Identity
		&& RestoredState.ClassPath == Before.ClassPath
		&& RestoredState.ActorGuid == Before.ActorGuid
		&& RestoredState.LevelPath == Before.LevelPath
		&& RestoredState.PackageName == Before.PackageName
		&& RestoredState.ExternalPackageName == Before.ExternalPackageName
		&& RestoredState.PropertyDigest == Before.PropertyDigest;
	if (!bMatches)
	{
		OutError =
			TEXT("The restored Water actor does not match its original property or package snapshot.");
	}
	return bMatches;
}
}

struct FLandscapeWaterService::FChangeRecord
{
	FString RunId;
	FString RequestId;
	FString RequestDigest;
	FString PlanDigest;
	FString WorldPath;
	uint32 WorldObjectId = 0;
	TWeakObjectPtr<UWorld> WorldObject;
	TWeakObjectPtr<UPackage> WorldPackage;
	FString ArtifactDirectory;
	TArray<FString> LandscapeGuids;
	TArray<FString> WaterIdentities;
	TSharedPtr<FJsonObject> BeforeSnapshot;
	TSharedPtr<FJsonObject> AfterSnapshot;
	FString BeforeSnapshotDigest;
	FString BeforePackageDigest;
	FString AfterSnapshotDigest;
	FString AfterPackageDigest;
	FString RollbackRetrySnapshotDigest;
	FString RollbackRetryPackageDigest;
	TArray<FRasterBackup> RasterBackups;
	TArray<FMaterialBackup> MaterialBackups;
	TArray<FLayerInfoBackup> LayerInfoBackups;
	TArray<FWaterBackup> WaterBackups;
	TArray<FPackageFileBackup> PackageFileBackups;
	FString Status = TEXT("preparing");
	FString FailureCode;
	FString FailureMessage;
	int32 FailureHttpStatus = 500;
	bool bVerified = false;
	bool bRollbackVerified = false;
	bool bRolledBack = false;
	bool bLoadedFromRecoveryJournal = false;
};

namespace
{
bool IsOriginalWorld(
	UWorld* World,
	const FLandscapeWaterService::FChangeRecord& Record)
{
	if (!World || Record.WorldPath != World->GetPathName())
	{
		return false;
	}
	if (Record.bLoadedFromRecoveryJournal)
	{
		return World->GetPackage()
			&& FPackageName::ObjectPathToPackageName(Record.WorldPath)
				== World->GetPackage()->GetName();
	}
	return Record.WorldObject.IsValid()
		&& Record.WorldObject.Get() == World
		&& Record.WorldPackage.IsValid()
		&& Record.WorldPackage.Get() == World->GetPackage()
		&& Record.WorldObjectId == World->GetUniqueID();
}

bool CapturePackageFileBackups(
	const TSharedPtr<FJsonObject>& BeforeSnapshot,
	const FString& ArtifactDirectory,
	TArray<FPackageFileBackup>& OutBackups,
	FString& OutError)
{
	OutBackups.Reset();
	OutError.Reset();
	const TSharedPtr<FJsonObject>* Evidence = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (!BeforeSnapshot.IsValid()
		|| !BeforeSnapshot->TryGetObjectField(TEXT("packageEvidence"), Evidence)
		|| !Evidence || !Evidence->IsValid()
		|| !(*Evidence)->TryGetArrayField(TEXT("packages"), Packages)
		|| !Packages)
	{
		OutError = TEXT("Package recovery evidence is unavailable.");
		return false;
	}
	const FString BackupDirectory =
		FPaths::Combine(ArtifactDirectory, TEXT("Packages"));
	if (!IFileManager::Get().MakeDirectory(*BackupDirectory, true)
		&& !IFileManager::Get().DirectoryExists(*BackupDirectory))
	{
		OutError = TEXT("Package recovery directory could not be created.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Packages)
	{
		const TSharedPtr<FJsonObject> Item = Value->AsObject();
		const FString PackageName = LandscapeReadString(Item, TEXT("package"));
		if (!PackageName.StartsWith(TEXT("/Game/")))
		{
			continue;
		}
		if (UPackage* Loaded = FindPackage(nullptr, *PackageName);
			Loaded && Loaded->IsDirty())
		{
			OutError = FString::Printf(
				TEXT("Package '%s' already contains unsaved changes."),
				*PackageName);
			return false;
		}
		FPackageFileBackup Backup;
		Backup.PackageName = PackageName;
		Backup.bExisted = FPackageName::DoesPackageExist(
			PackageName,
			&Backup.OriginalPath);
		if (Backup.bExisted)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Backup.OriginalPath))
			{
				OutError = FString::Printf(
					TEXT("Package '%s' could not be read for durable recovery."),
					*PackageName);
				return false;
			}
			Backup.Sha256 = HashBytes(Bytes);
			Backup.BackupPath = FPaths::Combine(
				BackupDirectory,
				FString::Printf(
					TEXT("%03d%s"),
					OutBackups.Num(),
					*FPaths::GetExtension(Backup.OriginalPath, true)));
			if (Backup.Sha256.IsEmpty()
				|| !FFileHelper::SaveArrayToFile(Bytes, *Backup.BackupPath))
			{
				OutError = FString::Printf(
					TEXT("Package '%s' recovery copy could not be written."),
					*PackageName);
				return false;
			}
			TArray<uint8> Persisted;
			if (!FFileHelper::LoadFileToArray(Persisted, *Backup.BackupPath)
				|| HashBytes(Persisted) != Backup.Sha256)
			{
				OutError = FString::Printf(
					TEXT("Package '%s' recovery copy failed checksum verification."),
					*PackageName);
				return false;
			}
		}
		OutBackups.Add(MoveTemp(Backup));
	}
	return true;
}

bool RestorePackageFileBackups(
	const TArray<FPackageFileBackup>& Backups,
	FString& OutError)
{
	OutError.Reset();
	for (const FPackageFileBackup& Backup : Backups)
	{
		if (!Backup.bExisted)
		{
			if (!Backup.OriginalPath.IsEmpty()
				&& IFileManager::Get().FileExists(*Backup.OriginalPath)
				&& !IFileManager::Get().Delete(
					*Backup.OriginalPath,
					false,
					true,
					true))
			{
				OutError = FString::Printf(
					TEXT("New package file '%s' could not be removed."),
					*Backup.OriginalPath);
				return false;
			}
			continue;
		}
		TArray<uint8> Bytes;
		if (Backup.BackupPath.IsEmpty()
			|| Backup.OriginalPath.IsEmpty()
			|| !FFileHelper::LoadFileToArray(Bytes, *Backup.BackupPath)
			|| HashBytes(Bytes) != Backup.Sha256)
		{
			OutError = FString::Printf(
				TEXT("Package recovery copy for '%s' is missing or corrupt."),
				*Backup.PackageName);
			return false;
		}
		const FString TemporaryPath = FString::Printf(
			TEXT("%s.ueai-restore-%s.tmp"),
			*Backup.OriginalPath,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!FFileHelper::SaveArrayToFile(Bytes, *TemporaryPath))
		{
			OutError = FString::Printf(
				TEXT("Package recovery temporary file for '%s' could not be written."),
				*Backup.PackageName);
			return false;
		}
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(
			*Backup.OriginalPath,
			false);
		if (!IFileManager::Get().Move(
				*Backup.OriginalPath,
				*TemporaryPath,
				true,
				true,
				false,
				false))
		{
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			OutError = FString::Printf(
				TEXT("Package '%s' could not be restored atomically."),
				*Backup.PackageName);
			return false;
		}
		TArray<uint8> Restored;
		if (!FFileHelper::LoadFileToArray(Restored, *Backup.OriginalPath)
			|| HashBytes(Restored) != Backup.Sha256)
		{
			OutError = FString::Printf(
				TEXT("Restored package '%s' failed checksum verification."),
				*Backup.PackageName);
			return false;
		}
	}
	return true;
}

bool RestoreChangeData(
	UWorld* World,
	const TSharedPtr<FLandscapeWaterService::FChangeRecord>& Record,
	FString& OutError)
{
	OutError.Reset();
	if (!Record.IsValid() || !IsOriginalWorld(World, *Record))
	{
		OutError =
			TEXT("Recovery requires the original world object and package in this Editor instance.");
		return false;
	}
	for (const FRasterBackup& Backup : Record->RasterBackups)
	{
		FLandscapeTarget Target;
		FString Error;
		FString Code;
		if (!ResolveLandscape(
				World,
				Backup.LandscapeGuid,
				Target,
				Error,
				Code))
		{
			OutError = Error;
			return false;
		}
		if (!Target.Info.IsValid()
			|| Target.Info->HasUnloadedComponentsInRegion(
				Backup.Extent.Min.X,
				Backup.Extent.Min.Y,
				Backup.Extent.Max.X,
				Backup.Extent.Max.Y))
		{
			OutError =
				TEXT("All Landscape components in the recovery extent must be loaded.");
			return false;
		}
		TArray<uint8> Bytes;
		if (!ReadRasterBackup(Backup, Bytes))
		{
			OutError =
				TEXT("A raster recovery artifact is missing or has a digest mismatch.");
			return false;
		}
		MarkLandscapeTransactional(Target);
		const int32 Width = Backup.Extent.Width() + 1;
		const int32 Height = Backup.Extent.Height() + 1;
		if (Backup.Kind == TEXT("height"))
		{
			TArray<uint16> Values;
			if (!DecodeR16(Bytes, Width, Height, false, Values))
			{
				OutError = TEXT("The height recovery artifact is invalid.");
				return false;
			}
			FHeightmapAccessor<false> Accessor(Target.Info.Get());
			Accessor.SetData(
				Backup.Extent.Min.X,
				Backup.Extent.Min.Y,
				Backup.Extent.Max.X,
				Backup.Extent.Max.Y,
				Values.GetData());
			Accessor.Flush();
		}
		else
		{
			ULandscapeLayerInfoObject* Layer =
				FindLayerInfo(Target, Backup.Layer);
			if (!Layer || Bytes.Num() != Width * Height)
			{
				OutError =
					TEXT("The weight recovery target is unavailable.");
				return false;
			}
			FAlphamapAccessor<false, false> Accessor(
				Target.Info.Get(),
				Layer);
			Accessor.SetData(
				Backup.Extent.Min.X,
				Backup.Extent.Min.Y,
				Backup.Extent.Max.X,
				Backup.Extent.Max.Y,
				Bytes.GetData(),
				ELandscapeLayerPaintingRestriction::None);
			Accessor.Flush();
		}
	}
	for (const FMaterialBackup& Backup : Record->MaterialBackups)
	{
		FLandscapeTarget Target;
		FString Error;
		FString Code;
		if (!ResolveLandscape(
				World,
				Backup.LandscapeGuid,
				Target,
				Error,
				Code))
		{
			OutError = Error;
			return false;
		}
		ALandscapeProxy* Proxy = Target.Representative.Get();
		UMaterialInterface* Material = LoadMaterial(Backup.MaterialPath);
		if (!Proxy || (!Backup.MaterialPath.IsEmpty() && !Material))
		{
			OutError =
				TEXT("The Landscape material recovery target is unavailable.");
			return false;
		}
		Proxy->Modify();
		SetLandscapeMaterialEditor(Proxy, Material);
		Proxy->MarkPackageDirty();
	}
	for (const FLayerInfoBackup& Backup : Record->LayerInfoBackups)
	{
		FLandscapeTarget Target;
		FString Error;
		FString Code;
		if (!ResolveLandscape(
				World,
				Backup.LandscapeGuid,
				Target,
				Error,
				Code))
		{
			OutError = Error;
			return false;
		}
		ULandscapeLayerInfoObject* Current =
			LoadLayerInfo(Backup.AfterLayerInfoPath);
		ULandscapeLayerInfoObject* Before =
			LoadLayerInfo(Backup.BeforeLayerInfoPath);
		if (!Current || !Before || !Target.Info.IsValid())
		{
			OutError = TEXT("A Layer Info recovery asset is unavailable.");
			return false;
		}
		Target.Info->ReplaceLayer(Current, Before);
	}
	for (int32 Index = Record->WaterBackups.Num() - 1;
		Index >= 0;
		--Index)
	{
		FString Error;
		if (!RestoreWaterState(
				World,
				Record->WaterBackups[Index],
				Error))
		{
			OutError = Error;
			return false;
		}
	}
	return true;
}

bool RestoreAndVerifyChange(
	UWorld* World,
	const TSharedPtr<FLandscapeWaterService::FChangeRecord>& Record,
	const FString& EvidenceFileName,
	const bool bForceEvidenceWriteFailure,
	TSharedPtr<FJsonObject>& OutRestoredSnapshot,
	FString& OutError)
{
	OutRestoredSnapshot.Reset();
	if (!RestoreChangeData(World, Record, OutError))
	{
		return false;
	}
	if (!RestorePackageFileBackups(Record->PackageFileBackups, OutError))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* PackageEvidence = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Packages = nullptr;
	if (!Record->BeforeSnapshot.IsValid()
		|| !Record->BeforeSnapshot->TryGetObjectField(
			TEXT("packageEvidence"),
			PackageEvidence)
		|| !PackageEvidence
		|| !(*PackageEvidence)->TryGetArrayField(
			TEXT("packages"),
			Packages)
		|| !Packages)
	{
		OutError = TEXT("The original package dirty-state evidence is unavailable.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Packages)
	{
		const TSharedPtr<FJsonObject> PackageState = Value->AsObject();
		FString PackageName;
		bool bDirty = false;
		if (!PackageState.IsValid()
			|| !PackageState->TryGetStringField(
				TEXT("package"),
				PackageName)
			|| !PackageState->TryGetBoolField(TEXT("dirty"), bDirty))
		{
			continue;
		}
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			OutError = FString::Printf(
				TEXT("Recovery package '%s' is no longer loaded."),
				*PackageName);
			return false;
		}
		Package->SetDirtyFlag(bDirty);
	}
	FString SnapshotError;
	const TSet<FString> RequiredPackageNames =
		PackageNamesFromSnapshot(Record->BeforeSnapshot);
	OutRestoredSnapshot = BuildSnapshotForTargets(
		World,
		Record->LandscapeGuids,
		Record->WaterIdentities,
		SnapshotError,
		&RequiredPackageNames);
	FString VerifiedDigest;
	if (!OutRestoredSnapshot.IsValid()
		|| !VerifySnapshotDigest(OutRestoredSnapshot, VerifiedDigest)
		|| VerifiedDigest != Record->BeforeSnapshotDigest
		|| OutRestoredSnapshot
				->GetObjectField(TEXT("packageEvidence"))
				->GetStringField(TEXT("digest"))
			!= Record->BeforePackageDigest)
	{
		OutError = SnapshotError.IsEmpty()
			? TEXT("Recovery read-back does not match the original snapshot.")
			: SnapshotError;
		return false;
	}
	if (bForceEvidenceWriteFailure
		|| !SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				EvidenceFileName),
			OutRestoredSnapshot))
	{
		OutError =
			TEXT("The verified rollback snapshot artifact could not be written.");
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> MakeFailureDetails(
	const FLandscapeWaterService::FChangeRecord& Record,
	const bool bRollbackVerified,
	const FString& RollbackError)
{
	TSharedRef<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("runId"), Record.RunId);
	Details->SetStringField(TEXT("status"), Record.Status);
	Details->SetBoolField(TEXT("verified"), false);
	Details->SetBoolField(TEXT("rollbackVerified"), bRollbackVerified);
	Details->SetStringField(TEXT("rollbackError"), RollbackError);
	Details->SetStringField(
		TEXT("beforeSnapshotDigest"),
		Record.BeforeSnapshotDigest);
	if (!Record.AfterSnapshotDigest.IsEmpty())
	{
		Details->SetStringField(
			TEXT("afterSnapshotDigest"),
			Record.AfterSnapshotDigest);
	}
	Details->SetStringField(
		TEXT("recoveryArtifactDirectory"),
		Record.ArtifactDirectory);
	return Details;
}
}

TSharedRef<FJsonObject> MakeRecoveryManifest(
	const FLandscapeWaterService::FChangeRecord& Record)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.landscape-recovery.v1"));
	Result->SetStringField(TEXT("changeSetId"), Record.RunId);
	Result->SetStringField(TEXT("runId"), Record.RunId);
	Result->SetStringField(TEXT("requestId"), Record.RequestId);
	Result->SetStringField(TEXT("requestDigest"), Record.RequestDigest);
	Result->SetStringField(TEXT("planDigest"), Record.PlanDigest);
	Result->SetStringField(TEXT("world"), Record.WorldPath);
	Result->SetNumberField(TEXT("worldObjectId"), Record.WorldObjectId);
	Result->SetStringField(TEXT("status"), Record.Status);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
	Result->SetStringField(TEXT("beforeHash"), Record.BeforeSnapshotDigest);
	Result->SetStringField(TEXT("afterHash"), Record.AfterSnapshotDigest);
	Result->SetStringField(
		TEXT("recoveryExpiresAt"),
		(FDateTime::UtcNow() + FTimespan::FromDays(7.0)).ToIso8601());
	Result->SetStringField(
		TEXT("beforeSnapshotDigest"),
		Record.BeforeSnapshotDigest);
	Result->SetStringField(
		TEXT("beforePackageDigest"),
		Record.BeforePackageDigest);
	if (!Record.AfterSnapshotDigest.IsEmpty())
	{
		Result->SetStringField(
			TEXT("afterSnapshotDigest"),
			Record.AfterSnapshotDigest);
		Result->SetStringField(
			TEXT("afterPackageDigest"),
			Record.AfterPackageDigest);
	}
	if (!Record.RollbackRetrySnapshotDigest.IsEmpty())
	{
		Result->SetStringField(
			TEXT("rollbackRetrySnapshotDigest"),
			Record.RollbackRetrySnapshotDigest);
		Result->SetStringField(
			TEXT("rollbackRetryPackageDigest"),
			Record.RollbackRetryPackageDigest);
	}
	Result->SetBoolField(TEXT("verified"), Record.bVerified);
	Result->SetBoolField(
		TEXT("rollbackVerified"),
		Record.bRollbackVerified);
	Result->SetBoolField(TEXT("rolledBack"), Record.bRolledBack);
	if (!Record.FailureCode.IsEmpty())
	{
		Result->SetStringField(TEXT("failureCode"), Record.FailureCode);
		Result->SetStringField(TEXT("failureMessage"), Record.FailureMessage);
		Result->SetNumberField(
			TEXT("failureHttpStatus"),
			Record.FailureHttpStatus);
	}
	Result->SetObjectField(
		TEXT("packageEvidence"),
		Record.BeforeSnapshot->GetObjectField(TEXT("packageEvidence")));

	TArray<TSharedPtr<FJsonValue>> Rasters;
	for (const FRasterBackup& Backup : Record.RasterBackups)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("kind"), Backup.Kind);
		Item->SetStringField(TEXT("landscapeId"), Backup.LandscapeGuid);
		Item->SetStringField(TEXT("layer"), Backup.Layer);
		Item->SetStringField(TEXT("path"), Backup.Path);
		Item->SetStringField(TEXT("sha256"), Backup.Sha256);
		Item->SetNumberField(TEXT("minX"), Backup.Extent.Min.X);
		Item->SetNumberField(TEXT("minY"), Backup.Extent.Min.Y);
		Item->SetNumberField(TEXT("maxX"), Backup.Extent.Max.X);
		Item->SetNumberField(TEXT("maxY"), Backup.Extent.Max.Y);
		Rasters.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("rasters"), Rasters);

	TArray<TSharedPtr<FJsonValue>> Materials;
	for (const FMaterialBackup& Backup : Record.MaterialBackups)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("landscapeId"), Backup.LandscapeGuid);
		Item->SetStringField(TEXT("material"), Backup.MaterialPath);
		Materials.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("materials"), Materials);

	TArray<TSharedPtr<FJsonValue>> LayerInfos;
	for (const FLayerInfoBackup& Backup : Record.LayerInfoBackups)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("landscapeId"), Backup.LandscapeGuid);
		Item->SetStringField(TEXT("layer"), Backup.Layer);
		Item->SetStringField(TEXT("beforeLayerInfo"), Backup.BeforeLayerInfoPath);
		Item->SetStringField(TEXT("afterLayerInfo"), Backup.AfterLayerInfoPath);
		LayerInfos.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("layerInfos"), LayerInfos);

	TArray<TSharedPtr<FJsonValue>> Water;
	for (const FWaterBackup& Backup : Record.WaterBackups)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("action"), Backup.Action);
		Item->SetObjectField(
			TEXT("before"),
			WaterStateToJson(Backup.Before, false));
		if (Backup.Created.bPresent)
		{
			Item->SetObjectField(
				TEXT("created"),
				WaterStateToJson(Backup.Created, false));
		}
		Item->SetStringField(
			TEXT("actorExportPath"),
			Backup.ActorExportPath);
		Item->SetStringField(
			TEXT("actorExportSha256"),
			Backup.ActorExportSha256);
		TArray<TSharedPtr<FJsonValue>> SideEffectActors;
		for (const FWaterSideEffectActor& SideEffect :
			Backup.CreatedSideEffectActors)
		{
			TSharedRef<FJsonObject> Actor = MakeShared<FJsonObject>();
			Actor->SetStringField(TEXT("actorGuid"), SideEffect.ActorGuid);
			Actor->SetStringField(TEXT("class"), SideEffect.ClassPath);
			Actor->SetStringField(TEXT("name"), SideEffect.Name);
			Actor->SetStringField(TEXT("level"), SideEffect.LevelPath);
			Actor->SetStringField(TEXT("package"), SideEffect.PackageName);
			Actor->SetBoolField(
				TEXT("externalActor"),
				SideEffect.bExternalActor);
			Actor->SetStringField(
				TEXT("externalPackage"),
				SideEffect.ExternalPackageName);
			SideEffectActors.Add(
				MakeShared<FJsonValueObject>(Actor));
		}
		Item->SetArrayField(
			TEXT("createdSideEffectActors"),
			SideEffectActors);
		TArray<TSharedPtr<FJsonValue>> SideEffectLayers;
		for (const FWaterLayerSideEffect& SideEffect :
			Backup.CreatedLandscapeLayers)
		{
			TSharedRef<FJsonObject> Layer = MakeShared<FJsonObject>();
			Layer->SetStringField(
				TEXT("landscapeGuid"),
				SideEffect.LandscapeGuid);
			Layer->SetStringField(
				TEXT("layerGuid"),
				SideEffect.LayerGuid);
			SideEffectLayers.Add(
				MakeShared<FJsonValueObject>(Layer));
		}
		Item->SetArrayField(
			TEXT("createdLandscapeLayers"),
			SideEffectLayers);
		Water.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("water"), Water);
	TArray<TSharedPtr<FJsonValue>> PackageFiles;
	for (const FPackageFileBackup& Backup : Record.PackageFileBackups)
	{
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("package"), Backup.PackageName);
		Item->SetStringField(TEXT("originalPath"), Backup.OriginalPath);
		Item->SetStringField(TEXT("backupPath"), Backup.BackupPath);
		Item->SetStringField(TEXT("sha256"), Backup.Sha256);
		Item->SetBoolField(TEXT("existed"), Backup.bExisted);
		PackageFiles.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("packageFiles"), PackageFiles);
	return Result;
}

bool LoadLandscapeRecoveryRecord(
	const FString& RunId,
	TSharedPtr<FLandscapeWaterService::FChangeRecord>& OutRecord,
	FString& OutErrorCode,
	FString& OutError)
{
	OutRecord.Reset();
	OutErrorCode.Reset();
	OutError.Reset();
	FGuid ParsedRunId;
	if (!FGuid::Parse(RunId, ParsedRunId))
	{
		OutErrorCode = TEXT("run_not_found");
		OutError = TEXT("The Landscape change runId is invalid.");
		return false;
	}
	const FString Directory =
		FPaths::Combine(ArtifactRoot(), TEXT("Runs"), RunId);
	const TSharedPtr<FJsonObject> Manifest = LoadJsonFile(
		FPaths::Combine(Directory, TEXT("recovery.manifest.json")));
	if (!Manifest.IsValid()
		|| LandscapeReadString(Manifest, TEXT("schema"))
			!= TEXT("ue.landscape-recovery.v1")
		|| LandscapeReadString(Manifest, TEXT("runId")) != RunId)
	{
		OutErrorCode = TEXT("run_not_found");
		OutError = TEXT("The Landscape recovery journal was not found.");
		return false;
	}

	TSharedPtr<FLandscapeWaterService::FChangeRecord> Record =
		MakeShared<FLandscapeWaterService::FChangeRecord>();
	Record->RunId = RunId;
	Record->RequestId = LandscapeReadString(Manifest, TEXT("requestId"));
	Record->RequestDigest = LandscapeReadString(Manifest, TEXT("requestDigest"));
	Record->PlanDigest = LandscapeReadString(Manifest, TEXT("planDigest"));
	Record->WorldPath = LandscapeReadString(Manifest, TEXT("world"));
	Record->ArtifactDirectory = Directory;
	Record->Status = LandscapeReadString(Manifest, TEXT("status"));
	Record->BeforeSnapshotDigest =
		LandscapeReadString(Manifest, TEXT("beforeSnapshotDigest"));
	Record->BeforePackageDigest =
		LandscapeReadString(Manifest, TEXT("beforePackageDigest"));
	Record->AfterSnapshotDigest =
		LandscapeReadString(Manifest, TEXT("afterSnapshotDigest"));
	Record->AfterPackageDigest =
		LandscapeReadString(Manifest, TEXT("afterPackageDigest"));
	Record->RollbackRetrySnapshotDigest =
		LandscapeReadString(Manifest, TEXT("rollbackRetrySnapshotDigest"));
	Record->RollbackRetryPackageDigest =
		LandscapeReadString(Manifest, TEXT("rollbackRetryPackageDigest"));
	Record->FailureCode = LandscapeReadString(Manifest, TEXT("failureCode"));
	Record->FailureMessage = LandscapeReadString(Manifest, TEXT("failureMessage"));
	double FailureHttpStatus = 500.0;
	Manifest->TryGetNumberField(TEXT("failureHttpStatus"), FailureHttpStatus);
	Record->FailureHttpStatus = static_cast<int32>(FailureHttpStatus);
	Record->bVerified = ReadBool(Manifest, TEXT("verified"));
	Record->bRollbackVerified =
		ReadBool(Manifest, TEXT("rollbackVerified"));
	Record->bRolledBack = ReadBool(Manifest, TEXT("rolledBack"));
	Record->bLoadedFromRecoveryJournal = true;

	Record->BeforeSnapshot = LoadJsonFile(
		FPaths::Combine(Directory, TEXT("before.snapshot.json")));
	if (!Record->BeforeSnapshot.IsValid()
		|| LandscapeReadString(
			Record->BeforeSnapshot,
			TEXT("snapshotDigest")) != Record->BeforeSnapshotDigest
		|| LandscapeReadString(
			Record->BeforeSnapshot->GetObjectField(TEXT("packageEvidence")),
			TEXT("digest")) != Record->BeforePackageDigest)
	{
		OutErrorCode = TEXT("recovery_checkpoint_corrupt");
		OutError = TEXT("The Landscape baseline snapshot is missing or corrupt.");
		return false;
	}
	if (!Record->AfterSnapshotDigest.IsEmpty())
	{
		Record->AfterSnapshot = LoadJsonFile(
			FPaths::Combine(Directory, TEXT("after.snapshot.json")));
		if (!Record->AfterSnapshot.IsValid()
			|| LandscapeReadString(
				Record->AfterSnapshot,
				TEXT("snapshotDigest")) != Record->AfterSnapshotDigest)
		{
			OutErrorCode = TEXT("recovery_checkpoint_corrupt");
			OutError = TEXT("The Landscape post-change snapshot is missing or corrupt.");
			return false;
		}
	}

	const TSharedPtr<FJsonObject> BeforeState =
		Record->BeforeSnapshot->GetObjectField(TEXT("state"));
	for (const TSharedPtr<FJsonValue>& Value :
		BeforeState->GetArrayField(TEXT("landscapes")))
	{
		const TSharedPtr<FJsonObject> Item = Value->AsObject();
		const FString LandscapeId =
			LandscapeReadString(Item, TEXT("landscapeId"));
		if (!LandscapeId.IsEmpty())
		{
			Record->LandscapeGuids.AddUnique(LandscapeId);
		}
	}
	for (const TSharedPtr<FJsonValue>& Value :
		BeforeState->GetArrayField(TEXT("water")))
	{
		const TSharedPtr<FJsonObject> Item = Value->AsObject();
		const FString Identity =
			LandscapeReadString(Item, TEXT("identity"));
		if (!Identity.IsEmpty())
		{
			Record->WaterIdentities.AddUnique(Identity);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Manifest->TryGetArrayField(TEXT("rasters"), Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			FRasterBackup Backup;
			Backup.Kind = LandscapeReadString(Item, TEXT("kind"));
			Backup.LandscapeGuid = LandscapeReadString(Item, TEXT("landscapeId"));
			Backup.Layer = LandscapeReadString(Item, TEXT("layer"));
			Backup.Path = LandscapeReadString(Item, TEXT("path"));
			Backup.Sha256 = LandscapeReadString(Item, TEXT("sha256"));
			Backup.Extent = FIntRect(
				static_cast<int32>(Item->GetNumberField(TEXT("minX"))),
				static_cast<int32>(Item->GetNumberField(TEXT("minY"))),
				static_cast<int32>(Item->GetNumberField(TEXT("maxX"))),
				static_cast<int32>(Item->GetNumberField(TEXT("maxY"))));
			Record->RasterBackups.Add(MoveTemp(Backup));
		}
	}
	Values = nullptr;
	if (Manifest->TryGetArrayField(TEXT("materials"), Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			FMaterialBackup Backup;
			Backup.LandscapeGuid = LandscapeReadString(Item, TEXT("landscapeId"));
			Backup.MaterialPath = LandscapeReadString(Item, TEXT("material"));
			Record->MaterialBackups.Add(MoveTemp(Backup));
		}
	}
	Values = nullptr;
	if (Manifest->TryGetArrayField(TEXT("layerInfos"), Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			FLayerInfoBackup Backup;
			Backup.LandscapeGuid = LandscapeReadString(Item, TEXT("landscapeId"));
			Backup.Layer = LandscapeReadString(Item, TEXT("layer"));
			Backup.BeforeLayerInfoPath =
				LandscapeReadString(Item, TEXT("beforeLayerInfo"));
			Backup.AfterLayerInfoPath =
				LandscapeReadString(Item, TEXT("afterLayerInfo"));
			Record->LayerInfoBackups.Add(MoveTemp(Backup));
		}
	}
	Values = nullptr;
	if (Manifest->TryGetArrayField(TEXT("water"), Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			FWaterBackup Backup;
			Backup.Action = LandscapeReadString(Item, TEXT("action"));
			Backup.Before = WaterStateFromJson(Item->GetObjectField(TEXT("before")));
			const TSharedPtr<FJsonObject>* Created = nullptr;
			if (Item->TryGetObjectField(TEXT("created"), Created)
				&& Created && Created->IsValid())
			{
				Backup.Created = WaterStateFromJson(*Created);
			}
			Backup.ActorExportPath =
				LandscapeReadString(Item, TEXT("actorExportPath"));
			Backup.ActorExportSha256 =
				LandscapeReadString(Item, TEXT("actorExportSha256"));
			const TArray<TSharedPtr<FJsonValue>>* SideEffects = nullptr;
			if (Item->TryGetArrayField(TEXT("createdSideEffectActors"), SideEffects)
				&& SideEffects)
			{
				for (const TSharedPtr<FJsonValue>& SideValue : *SideEffects)
				{
					const TSharedPtr<FJsonObject> Side = SideValue->AsObject();
					FWaterSideEffectActor Actor;
					Actor.ActorGuid = LandscapeReadString(Side, TEXT("actorGuid"));
					Actor.ClassPath = LandscapeReadString(Side, TEXT("class"));
					Actor.Name = LandscapeReadString(Side, TEXT("name"));
					Actor.LevelPath = LandscapeReadString(Side, TEXT("level"));
					Actor.PackageName = LandscapeReadString(Side, TEXT("package"));
					Actor.ExternalPackageName =
						LandscapeReadString(Side, TEXT("externalPackage"));
					Actor.bExternalActor = ReadBool(Side, TEXT("externalActor"));
					Backup.CreatedSideEffectActors.Add(MoveTemp(Actor));
				}
			}
			SideEffects = nullptr;
			if (Item->TryGetArrayField(TEXT("createdLandscapeLayers"), SideEffects)
				&& SideEffects)
			{
				for (const TSharedPtr<FJsonValue>& SideValue : *SideEffects)
				{
					const TSharedPtr<FJsonObject> Side = SideValue->AsObject();
					FWaterLayerSideEffect Layer;
					Layer.LandscapeGuid =
						LandscapeReadString(Side, TEXT("landscapeGuid"));
					Layer.LayerGuid = LandscapeReadString(Side, TEXT("layerGuid"));
					Backup.CreatedLandscapeLayers.Add(MoveTemp(Layer));
				}
			}
			Record->WaterBackups.Add(MoveTemp(Backup));
		}
	}
	Values = nullptr;
	if (Manifest->TryGetArrayField(TEXT("packageFiles"), Values) && Values)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			FPackageFileBackup Backup;
			Backup.PackageName = LandscapeReadString(Item, TEXT("package"));
			Backup.OriginalPath = LandscapeReadString(Item, TEXT("originalPath"));
			Backup.BackupPath = LandscapeReadString(Item, TEXT("backupPath"));
			Backup.Sha256 = LandscapeReadString(Item, TEXT("sha256"));
			Backup.bExisted = ReadBool(Item, TEXT("existed"));
			Record->PackageFileBackups.Add(MoveTemp(Backup));
		}
	}
	if (Record->PackageFileBackups.IsEmpty())
	{
		OutErrorCode = TEXT("recovery_checkpoint_corrupt");
		OutError = TEXT("The Landscape package recovery set is missing.");
		return false;
	}

	UWorld* World = GetEditorWorld();
	if (World && World->GetPathName() == Record->WorldPath)
	{
		Record->WorldObject = World;
		Record->WorldPackage = World->GetPackage();
	}
	OutRecord = MoveTemp(Record);
	return true;
}

FLandscapeWaterService& FLandscapeWaterService::Get()
{
	static FLandscapeWaterService Service;
	return Service;
}

#if WITH_DEV_AUTOMATION_TESTS
void FLandscapeWaterService::SetAutomationFailurePoint(
	const FName FailurePoint)
{
	check(IsInGameThread());
	AutomationFailurePoint = FailurePoint;
}

void FLandscapeWaterService::SimulateEditorRestartForTest()
{
	check(IsInGameThread());
	Runs.Reset();
	RequestRuns.Reset();
}
#endif

FMCPToolResult FLandscapeWaterService::ListLandscapes(
	const TSharedPtr<FJsonObject>&) const
{
	UWorld* World = GetEditorWorld();
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FLandscapeTarget& Target : CollectLandscapes(World))
	{
		Values.Add(
			MakeShared<FJsonValueObject>(
				DescribeLandscape(Target, false)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("world"), World ? World->GetPathName() : TEXT(""));
	Result->SetArrayField(TEXT("landscapes"), Values);
	Result->SetNumberField(TEXT("total"), Values.Num());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::GetLandscape(
	const TSharedPtr<FJsonObject>& Params) const
{
	FLandscapeTarget Target;
	FString Error;
	FString Code;
	if (!ResolveLandscape(
			GetEditorWorld(),
			LandscapeReadString(Params, TEXT("landscape")),
			Target,
			Error,
			Code))
	{
		return FMCPToolResult::Error(Error, Code, Code == TEXT("landscape_not_found") ? 404 : 409);
	}
	return FMCPToolResult::Ok(DescribeLandscape(Target, true));
}

FMCPToolResult FLandscapeWaterService::GetLandscapeLayers(
	const TSharedPtr<FJsonObject>& Params) const
{
	FLandscapeTarget Target;
	FString Error;
	FString Code;
	if (!ResolveLandscape(
			GetEditorWorld(),
			LandscapeReadString(Params, TEXT("landscape")),
			Target,
			Error,
			Code))
	{
		return FMCPToolResult::Error(Error, Code, 404);
	}
	TArray<TSharedPtr<FJsonValue>> Layers;
	if (ULandscapeInfo* Info = Target.Info.Get())
	{
		for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
		{
			Layers.Add(MakeShared<FJsonValueObject>(LayerToJson(Layer, true)));
		}
	}
	Layers.Sort(
		[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			return Left->AsObject()->GetStringField(TEXT("name"))
				< Right->AsObject()->GetStringField(TEXT("name"));
		});
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("landscapeId"), GuidString(Target.Guid));
	Result->SetArrayField(TEXT("layers"), Layers);
	Result->SetNumberField(TEXT("total"), Layers.Num());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::ValidateLandscape(
	const TSharedPtr<FJsonObject>& Params) const
{
	FLandscapeTarget Target;
	FString Error;
	FString Code;
	if (!ResolveLandscape(
			GetEditorWorld(),
			LandscapeReadString(Params, TEXT("landscape")),
			Target,
			Error,
			Code))
	{
		return FMCPToolResult::Error(Error, Code, 404);
	}
	TArray<TSharedPtr<FJsonValue>> Findings;
	ULandscapeInfo* Info = Target.Info.Get();
	ALandscapeProxy* Proxy = Target.Representative.Get();
	auto AddFinding =
		[&Findings, &Target](
			const FString& Rule,
			const FString& Severity,
			const FString& Message,
			const TSharedPtr<FJsonObject>& Evidence = nullptr)
		{
			Findings.Add(
				MakeShared<FJsonValueObject>(
					MakeFinding(
						Rule,
						Severity,
						1.0,
						GuidString(Target.Guid),
						TEXT("Landscape"),
						TEXT(""),
						Message,
						Evidence)));
		};
	if (!Info || !Info->SupportsLandscapeEditing())
	{
		AddFinding(
			TEXT("landscape.editing_unavailable"),
			TEXT("high"),
			TEXT("The loaded Landscape does not support editor data access."));
	}
	if (!Proxy || !Proxy->GetLandscapeMaterial())
	{
		AddFinding(
			TEXT("landscape.material_missing"),
			TEXT("medium"),
			TEXT("The Landscape has no effective material."));
	}
	if (Proxy && Proxy->HasLayersContent())
	{
		AddFinding(
			TEXT("landscape.edit_layers_present"),
			TEXT("info"),
			TEXT("Landscape Edit Layers are enabled; raw v1 import is intentionally rejected."));
	}
	if (Info)
	{
		FIntRect Extent;
		if (!Info->GetLandscapeExtent(Extent))
		{
			AddFinding(
				TEXT("landscape.extent_unavailable"),
				TEXT("high"),
				TEXT("The Landscape has no valid loaded extent."));
		}
		else if (Info->HasUnloadedComponentsInRegion(
					Extent.Min.X,
					Extent.Min.Y,
					Extent.Max.X,
					Extent.Max.Y))
		{
			AddFinding(
				TEXT("landscape.components_unloaded"),
				TEXT("low"),
				TEXT("Some Landscape components in the complete extent are not loaded; validation is partial."));
		}
		for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
		{
			if (!Layer.LayerInfoObj)
			{
				TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
				Evidence->SetStringField(
					TEXT("layer"),
					Layer.GetLayerName().ToString());
				AddFinding(
					TEXT("landscape.layer_info_missing"),
					TEXT("medium"),
					TEXT("A material layer has no Layer Info asset."),
					Evidence);
			}
		}
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("valid"), Findings.IsEmpty());
	Result->SetStringField(TEXT("landscapeId"), GuidString(Target.Guid));
	Result->SetArrayField(TEXT("findings"), Findings);
	Result->SetNumberField(TEXT("findingCount"), Findings.Num());
	Result->SetStringField(
		TEXT("evidenceBoundary"),
		TEXT("Validation covers loaded editor Landscape structure and assignments; it does not run HLOD, PCG, navigation, or cook jobs."));
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::SnapshotLandscape(
	const TSharedPtr<FJsonObject>& Params) const
{
	FLandscapeTarget Target;
	FString Error;
	FString Code;
	if (!ResolveLandscape(
			GetEditorWorld(),
			LandscapeReadString(Params, TEXT("landscape")),
			Target,
			Error,
			Code))
	{
		return FMCPToolResult::Error(Error, Code, 404);
	}
	TArray<FString> WaterIdentities;
	if (ReadBool(Params, TEXT("includeWater"), true))
	{
		const FBox LandscapeBounds =
			Target.Info.IsValid()
				? Target.Info->GetLoadedBounds()
				: FBox(ForceInit);
		for (AActor* Actor : CollectWaterActors(GetEditorWorld()))
		{
			if (!LandscapeBounds.IsValid
				|| LandscapeBounds.Intersect(
					Actor->GetComponentsBoundingBox(true)))
			{
				WaterIdentities.Add(WaterIdentity(Actor));
			}
		}
	}
	TSharedPtr<FJsonObject> Snapshot = BuildSnapshotForTargets(
		GetEditorWorld(),
		{GuidString(Target.Guid)},
		WaterIdentities,
		Error);
	if (!Snapshot.IsValid())
	{
		return FMCPToolResult::Error(
			Error,
			TEXT("snapshot_failed"),
			500);
	}
	return FMCPToolResult::Ok(Snapshot);
}

FMCPToolResult FLandscapeWaterService::DiffSnapshotObjects(
	const TSharedPtr<FJsonObject>& Before,
	const TSharedPtr<FJsonObject>& After)
{
	FString BeforeDigest;
	FString AfterDigest;
	if (!VerifySnapshotDigest(Before, BeforeDigest)
		|| !VerifySnapshotDigest(After, AfterDigest))
	{
		return FMCPToolResult::Error(
			TEXT("Both snapshots must be valid ue.landscape-snapshot.v1 objects with matching digests."),
			TEXT("invalid_snapshot"),
			422);
	}
	const TSharedPtr<FJsonObject>* BeforeState = nullptr;
	const TSharedPtr<FJsonObject>* AfterState = nullptr;
	Before->TryGetObjectField(TEXT("state"), BeforeState);
	After->TryGetObjectField(TEXT("state"), AfterState);
	TArray<FString> ChangedPaths;
	int32 ChangedTotal = 0;
	CollectChangedPaths(
		MakeShared<FJsonValueObject>(*BeforeState),
		MakeShared<FJsonValueObject>(*AfterState),
		TEXT(""),
		ChangedPaths,
		ChangedTotal);
	TArray<TSharedPtr<FJsonValue>> ChangedValues;
	for (const FString& Path : ChangedPaths)
	{
		ChangedValues.Add(MakeShared<FJsonValueString>(Path));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.landscape-diff.v1"));
	Result->SetBoolField(TEXT("changed"), BeforeDigest != AfterDigest);
	Result->SetStringField(TEXT("beforeDigest"), BeforeDigest);
	Result->SetStringField(TEXT("afterDigest"), AfterDigest);
	Result->SetArrayField(TEXT("changedPaths"), ChangedValues);
	Result->SetNumberField(TEXT("changedPathCount"), ChangedTotal);
	Result->SetBoolField(TEXT("truncated"), ChangedTotal > ChangedPaths.Num());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::DiffLandscapeSnapshots(
	const TSharedPtr<FJsonObject>& Params) const
{
	const TSharedPtr<FJsonObject>* Before = nullptr;
	const TSharedPtr<FJsonObject>* After = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetObjectField(TEXT("before"), Before)
		|| !Before
		|| !Before->IsValid()
		|| !Params->TryGetObjectField(TEXT("after"), After)
		|| !After
		|| !After->IsValid())
	{
		return FMCPToolResult::Error(
			TEXT("before and after snapshot objects are required."),
			TEXT("invalid_params"),
			422);
	}
	return DiffSnapshotObjects(*Before, *After);
}

FMCPToolResult FLandscapeWaterService::ExportHeightmap(
	const TSharedPtr<FJsonObject>& Params) const
{
	FLandscapeTarget Target;
	FString Error;
	FString Code;
	if (!ResolveLandscape(
			GetEditorWorld(),
			LandscapeReadString(Params, TEXT("landscape")),
			Target,
			Error,
			Code))
	{
		return FMCPToolResult::Error(Error, Code, 404);
	}
	FIntRect Extent;
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint16> Values;
	if (!CaptureHeight(Target, Extent, Width, Height, Values))
	{
		return FMCPToolResult::Error(
			TEXT("Height data could not be captured within the 512 MiB export limit."),
			TEXT("landscape_export_failed"),
			500);
	}
	const TArray<uint8> Bytes = EncodeR16(Values);
	const FString Stem = SanitizeFileStem(
		LandscapeReadString(Params, TEXT("outputName")),
		TEXT("heightmap-") + GuidString(Target.Guid).Left(12));
	const FString Directory = FPaths::Combine(ArtifactRoot(), TEXT("Exports"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = FPaths::Combine(Directory, Stem + TEXT(".r16"));
	if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
	{
		return FMCPToolResult::Error(
			TEXT("The raw heightmap artifact could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	const FString Sha = HashBytes(Bytes);
	const FString Sidecar = FPaths::Combine(Directory, Stem + TEXT(".json"));
	TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetStringField(TEXT("schema"), TEXT("ue.landscape-raster.v1"));
	Metadata->SetStringField(TEXT("kind"), TEXT("heightmap"));
	Metadata->SetStringField(TEXT("format"), TEXT("r16le"));
	Metadata->SetStringField(TEXT("landscapeId"), GuidString(Target.Guid));
	Metadata->SetNumberField(TEXT("width"), Width);
	Metadata->SetNumberField(TEXT("height"), Height);
	Metadata->SetStringField(TEXT("sha256"), Sha);
	if (!SaveJsonFile(Sidecar, Metadata))
	{
		IFileManager::Get().Delete(*Path);
		return FMCPToolResult::Error(
			TEXT("The heightmap sidecar could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	return FMCPToolResult::Ok(
		ArtifactJson(Path, TEXT("r16le"), Sha, Width, Height, Sidecar));
}

FMCPToolResult FLandscapeWaterService::ExportWeightmap(
	const TSharedPtr<FJsonObject>& Params) const
{
	FLandscapeTarget Target;
	FString Error;
	FString Code;
	if (!ResolveLandscape(
			GetEditorWorld(),
			LandscapeReadString(Params, TEXT("landscape")),
			Target,
			Error,
			Code))
	{
		return FMCPToolResult::Error(Error, Code, 404);
	}
	const FString LayerName = LandscapeReadString(Params, TEXT("layer"));
	ULandscapeLayerInfoObject* Layer = FindLayerInfo(Target, LayerName);
	if (!Layer)
	{
		return FMCPToolResult::Error(
			FString::Printf(
				TEXT("Landscape layer '%s' has no Layer Info asset."),
				*LayerName),
			TEXT("landscape_layer_not_found"),
			404);
	}
	FIntRect Extent;
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> Values;
	if (!CaptureWeight(Target, Layer, Extent, Width, Height, Values))
	{
		return FMCPToolResult::Error(
			TEXT("Weight data could not be captured within the 512 MiB export limit."),
			TEXT("landscape_export_failed"),
			500);
	}
	const FString Stem = SanitizeFileStem(
		LandscapeReadString(Params, TEXT("outputName")),
		TEXT("weightmap-") + LayerName);
	const FString Directory = FPaths::Combine(ArtifactRoot(), TEXT("Exports"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = FPaths::Combine(Directory, Stem + TEXT(".r8"));
	if (!FFileHelper::SaveArrayToFile(Values, *Path))
	{
		return FMCPToolResult::Error(
			TEXT("The raw weightmap artifact could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	const FString Sha = HashBytes(Values);
	const FString Sidecar = FPaths::Combine(Directory, Stem + TEXT(".json"));
	TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	Metadata->SetStringField(TEXT("schema"), TEXT("ue.landscape-raster.v1"));
	Metadata->SetStringField(TEXT("kind"), TEXT("weightmap"));
	Metadata->SetStringField(TEXT("format"), TEXT("r8"));
	Metadata->SetStringField(TEXT("landscapeId"), GuidString(Target.Guid));
	Metadata->SetStringField(TEXT("layer"), LayerName);
	Metadata->SetNumberField(TEXT("width"), Width);
	Metadata->SetNumberField(TEXT("height"), Height);
	Metadata->SetStringField(TEXT("sha256"), Sha);
	if (!SaveJsonFile(Sidecar, Metadata))
	{
		IFileManager::Get().Delete(*Path);
		return FMCPToolResult::Error(
			TEXT("The weightmap sidecar could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	return FMCPToolResult::Ok(
		ArtifactJson(Path, TEXT("r8"), Sha, Width, Height, Sidecar));
}

FMCPToolResult FLandscapeWaterService::PlanChange(
	const TSharedPtr<FJsonObject>& Params) const
{
	const TSharedPtr<FJsonObject>* Request = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetObjectField(TEXT("request"), Request)
		|| !Request
		|| !Request->IsValid()
		|| !(*Request)->TryGetArrayField(TEXT("operations"), Operations)
		|| !Operations
		|| Operations->IsEmpty()
		|| Operations->Num() > MaxLandscapeOperations)
	{
		return FMCPToolResult::Error(
			TEXT("request.operations must contain between 1 and 32 operations."),
			TEXT("invalid_change"),
			422);
	}
	UWorld* World = GetEditorWorld();
	if (!World)
	{
		return FMCPToolResult::Error(
			TEXT("No Editor world is loaded."),
			TEXT("world_unavailable"),
			409);
	}
	const FString DefaultLandscape =
		LandscapeReadString(*Request, TEXT("landscape"));
	TArray<TSharedPtr<FJsonValue>> Normalized;
	TArray<FString> LandscapeGuids;
	TArray<FString> WaterIdentities;
	TSet<FString> ConflictKeys;
	TArray<FBox> AffectedBounds;
	TArray<AActor*> AffectedActors;
	int64 SnapshotBytes = 0;

	for (int32 Index = 0; Index < Operations->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> Operation = (*Operations)[Index]->AsObject();
		const FString Action = LandscapeReadString(Operation, TEXT("action"));
		if (Action.IsEmpty())
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("operations[%d].action is required."), Index),
				TEXT("invalid_change"),
				422);
		}
		TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("action"), Action);

		if (Action == TEXT("importHeightmap")
			|| Action == TEXT("importWeightmap")
			|| Action == TEXT("setMaterial")
			|| Action == TEXT("replaceLayerInfo"))
		{
			const FString Selector =
				LandscapeReadString(Operation, TEXT("landscape"), DefaultLandscape);
			FLandscapeTarget Target;
			FString Error;
			FString Code;
			if (!ResolveLandscape(World, Selector, Target, Error, Code))
			{
				return FMCPToolResult::Error(Error, Code, 404);
			}
			ALandscapeProxy* Proxy = Target.Representative.Get();
			if (!Target.Info.IsValid()
				|| !Target.Info->SupportsLandscapeEditing())
			{
				return FMCPToolResult::Error(
					TEXT("The selected Landscape does not support editor data access."),
					TEXT("landscape_editing_unavailable"),
					409);
			}
			if ((Action == TEXT("importHeightmap")
					|| Action == TEXT("importWeightmap"))
				&& Proxy && Proxy->HasLayersContent())
			{
				return FMCPToolResult::Error(
					TEXT("Raw v1 raster import is not admitted for Landscapes with Edit Layers content."),
					TEXT("landscape_edit_layers_unsupported"),
					409);
			}
			const FString TargetGuid = GuidString(Target.Guid);
			Item->SetStringField(TEXT("landscapeId"), TargetGuid);
			LandscapeGuids.AddUnique(TargetGuid);
			if (Proxy)
			{
				AffectedActors.AddUnique(Proxy);
			}
			if (Target.Info.IsValid())
			{
				AffectedBounds.Add(Target.Info->GetLoadedBounds());
			}
			FIntRect Extent;
			int32 Width = 0;
			int32 Height = 0;
			if (!LandscapeExtent(Target, Extent, Width, Height))
			{
				return FMCPToolResult::Error(
					TEXT("The selected Landscape has no valid extent."),
					TEXT("landscape_extent_unavailable"),
					409);
			}
			Item->SetNumberField(TEXT("width"), Width);
			Item->SetNumberField(TEXT("height"), Height);

			if (Action == TEXT("importHeightmap")
				|| Action == TEXT("importWeightmap"))
			{
				if (Target.Info->HasUnloadedComponentsInRegion(
						Extent.Min.X,
						Extent.Min.Y,
						Extent.Max.X,
						Extent.Max.Y))
				{
					return FMCPToolResult::Error(
						TEXT("Raw raster import requires every Landscape component in the complete extent to be loaded."),
						TEXT("landscape_components_unloaded"),
						409);
				}
				if (UWorldPartition* WorldPartition =
						World->GetWorldPartition();
					WorldPartition
					&& WorldPartition->IsStreamingEnabled()
					&& !WorldPartition->IsStreamingCompleted(nullptr))
				{
					return FMCPToolResult::Error(
						TEXT("Raw raster import requires World Partition streaming to be complete."),
						TEXT("world_partition_streaming_incomplete"),
						409);
				}
				FString SourcePath;
				FString ImportError;
				if (!ResolveImportPath(
						LandscapeReadString(Operation, TEXT("sourcePath")),
						SourcePath,
						ImportError))
				{
					return FMCPToolResult::Error(
						ImportError,
						TEXT("source_path_not_permitted"),
						422);
				}
				TArray<uint8> SourceBytes;
				if (!FFileHelper::LoadFileToArray(SourceBytes, *SourcePath))
				{
					return FMCPToolResult::Error(
						TEXT("The raster source could not be read."),
						TEXT("source_read_failed"),
						500);
				}
				const int64 ExpectedBytes =
					static_cast<int64>(Width) * Height
					* (Action == TEXT("importHeightmap") ? 2 : 1);
				if (SourceBytes.Num() != ExpectedBytes)
				{
					return FMCPToolResult::Error(
						FString::Printf(
							TEXT("Raster size mismatch: expected %lld bytes for %dx%d, found %d."),
							ExpectedBytes,
							Width,
							Height,
							SourceBytes.Num()),
						TEXT("raster_size_mismatch"),
						422);
				}
				Item->SetStringField(TEXT("sourcePath"), SourcePath);
				Item->SetStringField(TEXT("sourceSha256"), HashBytes(SourceBytes));
				Item->SetStringField(
					TEXT("format"),
					Action == TEXT("importHeightmap") ? TEXT("r16le") : TEXT("r8"));
				Item->SetBoolField(
					TEXT("flipY"),
					ReadBool(Operation, TEXT("flipY")));
				SnapshotBytes += ExpectedBytes;
			}
			if (Action == TEXT("importHeightmap"))
			{
				const FString Conflict =
					Action + TEXT(":") + TargetGuid;
				if (ConflictKeys.Contains(Conflict))
				{
					return FMCPToolResult::Error(
						TEXT("Only one heightmap import is allowed per Landscape in a change."),
						TEXT("change_conflict"),
						422);
				}
				ConflictKeys.Add(Conflict);
			}
			else if (Action == TEXT("importWeightmap"))
			{
				const FString LayerName = LandscapeReadString(Operation, TEXT("layer"));
				ULandscapeLayerInfoObject* Layer =
					FindLayerInfo(Target, LayerName);
				if (!Layer)
				{
					return FMCPToolResult::Error(
						FString::Printf(
							TEXT("Layer '%s' has no Layer Info asset."),
							*LayerName),
						TEXT("landscape_layer_not_found"),
						404);
				}
				Item->SetStringField(TEXT("layer"), LayerName);
				Item->SetStringField(TEXT("layerInfo"), Layer->GetPathName());
				const FString Conflict =
					TEXT("weightLayer:")
					+ TargetGuid
					+ TEXT(":")
					+ LayerName.ToLower();
				if (ConflictKeys.Contains(Conflict))
				{
					return FMCPToolResult::Error(
						TEXT("A Landscape weight layer can be imported or have its Layer Info replaced at most once per change."),
						TEXT("change_conflict"),
						422);
				}
				ConflictKeys.Add(Conflict);
			}
			else if (Action == TEXT("setMaterial"))
			{
				const FString MaterialPath =
					LandscapeReadString(Operation, TEXT("material"));
				UMaterialInterface* Material = LoadMaterial(MaterialPath);
				if (!Material)
				{
					return FMCPToolResult::Error(
						FString::Printf(
							TEXT("Landscape material '%s' was not found."),
							*MaterialPath),
						TEXT("material_not_found"),
						404);
				}
				Item->SetStringField(TEXT("material"), Material->GetPathName());
				Item->SetStringField(
					TEXT("beforeMaterial"),
					Proxy && Proxy->GetLandscapeMaterial()
						? Proxy->GetLandscapeMaterial()->GetPathName()
						: TEXT(""));
				const FString Conflict =
					Action + TEXT(":") + TargetGuid;
				if (ConflictKeys.Contains(Conflict))
				{
					return FMCPToolResult::Error(
						TEXT("Only one material assignment is allowed per Landscape in a change."),
						TEXT("change_conflict"),
						422);
				}
				ConflictKeys.Add(Conflict);
			}
			else
			{
				const FString LayerName = LandscapeReadString(Operation, TEXT("layer"));
				ULandscapeLayerInfoObject* BeforeLayer =
					FindLayerInfo(Target, LayerName);
				ULandscapeLayerInfoObject* AfterLayer =
					LoadLayerInfo(
						LandscapeReadString(Operation, TEXT("layerInfo")));
				if (!BeforeLayer || !AfterLayer)
				{
					return FMCPToolResult::Error(
						TEXT("replaceLayerInfo requires an existing layer and a valid Layer Info asset."),
						TEXT("landscape_layer_not_found"),
						404);
				}
				Item->SetStringField(TEXT("layer"), LayerName);
				Item->SetStringField(
					TEXT("beforeLayerInfo"),
					BeforeLayer->GetPathName());
				Item->SetStringField(
					TEXT("afterLayerInfo"),
					AfterLayer->GetPathName());
				const FString Conflict =
					TEXT("weightLayer:")
					+ TargetGuid
					+ TEXT(":")
					+ LayerName.ToLower();
				if (ConflictKeys.Contains(Conflict))
				{
					return FMCPToolResult::Error(
						TEXT("A Landscape weight layer can be imported or have its Layer Info replaced at most once per change."),
						TEXT("change_conflict"),
						422);
				}
				ConflictKeys.Add(Conflict);
			}
		}
		else if (Action == TEXT("waterCreate"))
		{
			const FString Type = LandscapeReadString(Operation, TEXT("type"));
			if (!FindWaterFactory(Type))
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Water actor type '%s' is unavailable; enable the Water editor plugin."),
						*Type),
					TEXT("water_plugin_unavailable"),
					409);
			}
			const TSharedPtr<FJsonObject>* TransformObject = nullptr;
			FTransform Transform = FTransform::Identity;
			FString Error;
			if (Operation->TryGetObjectField(
					TEXT("transform"),
					TransformObject)
				&& (!TransformObject
					|| !ParseTransform(*TransformObject, Transform, Error)))
			{
				return FMCPToolResult::Error(
					Error,
					TEXT("invalid_change"),
					422);
			}
			const FString Label =
				LandscapeReadString(Operation, TEXT("label"), TEXT("UEAI Water"));
			const FString ManagedId = MakeStableId(
				TEXT("water"),
				{
					World->GetPathName(),
					Type,
					Label,
					CanonicalizeJson(TransformToJson(Transform)),
					FString::FromInt(Index)
				});
			if (ResolveWater(World, ManagedId))
			{
				return FMCPToolResult::Error(
					TEXT("The deterministic managed Water identity already exists."),
					TEXT("water_already_exists"),
					409);
			}
			Item->SetStringField(TEXT("type"), Type);
			Item->SetStringField(TEXT("managedId"), ManagedId);
			Item->SetStringField(TEXT("label"), Label);
			Item->SetObjectField(TEXT("transform"), TransformToJson(Transform));
			WaterIdentities.AddUnique(ManagedId);
		}
		else if (Action == TEXT("waterUpdate")
			|| Action == TEXT("waterDelete"))
		{
			const FString Selector = LandscapeReadString(Operation, TEXT("water"));
			AActor* Actor = ResolveWater(World, Selector);
			if (!Actor)
			{
				return FMCPToolResult::Error(
					FString::Printf(TEXT("Water actor '%s' was not found."), *Selector),
					TEXT("water_not_found"),
					404);
			}
			if (Action == TEXT("waterDelete") && !IsManagedWater(Actor))
			{
				return FMCPToolResult::Error(
					TEXT("Only Water actors created and tagged by UE AI Integration can be deleted."),
					TEXT("water_delete_not_managed"),
					409);
			}
			const FWaterState BeforeState = CaptureWaterState(Actor);
			if (!IsCompleteWaterState(
					BeforeState,
					Action == TEXT("waterDelete")))
			{
				return FMCPToolResult::Error(
					TEXT("The complete Water actor properties, GUID, level, or external package state could not be captured."),
					Action == TEXT("waterDelete")
						? TEXT("water_delete_snapshot_failed")
						: TEXT("water_update_snapshot_failed"),
					500);
			}
			const FString Identity = WaterIdentity(Actor);
			Item->SetStringField(TEXT("waterIdentity"), Identity);
			Item->SetObjectField(
				TEXT("before"),
				WaterStateToJson(BeforeState, false));
			WaterIdentities.AddUnique(Identity);
			AffectedActors.Add(Actor);
			AffectedBounds.Add(Actor->GetComponentsBoundingBox(true));
			const FString Conflict = TEXT("water:") + Identity;
			if (ConflictKeys.Contains(Conflict))
			{
				return FMCPToolResult::Error(
					TEXT("A Water actor can be changed at most once per request."),
					TEXT("change_conflict"),
					422);
			}
			ConflictKeys.Add(Conflict);
			if (Action == TEXT("waterDelete"))
			{
				SnapshotBytes += BeforeState.PropertyBytes;
			}
			if (Action == TEXT("waterUpdate"))
			{
				const TSharedPtr<FJsonObject>* TransformObject = nullptr;
				FTransform Transform = Actor->GetActorTransform();
				FString Error;
				if (Operation->TryGetObjectField(
						TEXT("transform"),
						TransformObject)
					&& (!TransformObject
						|| !ParseTransform(*TransformObject, Transform, Error)))
				{
					return FMCPToolResult::Error(
						Error,
						TEXT("invalid_change"),
						422);
				}
				Item->SetObjectField(
					TEXT("transform"),
					TransformToJson(Transform));
				Item->SetStringField(
					TEXT("label"),
					LandscapeReadString(
						Operation,
						TEXT("label"),
						Actor->GetActorLabel()));
			}
		}
		else
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Operation '%s' is not admitted."),
					*Action),
				TEXT("change_not_admitted"),
				422);
		}
		Normalized.Add(MakeShared<FJsonValueObject>(Item));
	}
	if (SnapshotBytes > MaxSnapshotBytes)
	{
		return FMCPToolResult::Error(
			TEXT("The affected raster and Water actor backups exceed the 512 MiB execution limit."),
			TEXT("snapshot_too_large"),
			413);
	}
	FString SnapshotError;
	TSharedPtr<FJsonObject> BeforeSnapshot = BuildSnapshotForTargets(
		World,
		LandscapeGuids,
		WaterIdentities,
		SnapshotError);
	if (!BeforeSnapshot.IsValid())
	{
		return FMCPToolResult::Error(
			SnapshotError,
			TEXT("snapshot_failed"),
			500);
	}
	TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Plan->SetStringField(TEXT("domain"), TEXT("scene.landscape"));
	Plan->SetStringField(TEXT("world"), World->GetPathName());
	Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
	Plan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("editorRestart"));
	Plan->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
	Plan->SetBoolField(TEXT("rollbackAvailable"), true);
	Plan->SetNumberField(TEXT("operationCount"), Normalized.Num());
	Plan->SetArrayField(TEXT("operations"), Normalized);
	Plan->SetArrayField(
		TEXT("landscapeIds"),
		[&LandscapeGuids]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Value : LandscapeGuids)
			{
				Values.Add(MakeShared<FJsonValueString>(Value));
			}
			return Values;
		}());
	Plan->SetArrayField(
		TEXT("waterIdentities"),
		[&WaterIdentities]()
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Value : WaterIdentities)
			{
				Values.Add(MakeShared<FJsonValueString>(Value));
			}
			return Values;
		}());
	Plan->SetStringField(
		TEXT("beforeSnapshotDigest"),
		BeforeSnapshot->GetStringField(TEXT("snapshotDigest")));
	Plan->SetStringField(
		TEXT("beforePackageDigest"),
		BeforeSnapshot->GetObjectField(TEXT("packageEvidence"))
			->GetStringField(TEXT("digest")));
	Plan->SetObjectField(
		TEXT("impact"),
		BuildImpactSummary(World, AffectedBounds, AffectedActors));
	const FString PlanDigest = DigestJson(Plan);
	if (PlanDigest.IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("The normalized change plan could not be hashed."),
			TEXT("digest_unavailable"),
			500);
	}
	Plan->SetStringField(TEXT("planDigest"), PlanDigest);
	return FMCPToolResult::Ok(Plan);
}

FMCPToolResult FLandscapeWaterService::ExecuteChange(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString RequestId = LandscapeReadString(Params, TEXT("requestId"));
	if (RequestId.IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("requestId is required for Landscape change execution."),
			TEXT("request_id_required"),
			422);
	}
	TSharedPtr<FJsonObject> RequestIntent =
		MakeShared<FJsonObject>(*Params);
	RequestIntent->RemoveField(TEXT("requestId"));
	RequestIntent->RemoveField(TEXT("approvePlanDigest"));
	RequestIntent->RemoveField(TEXT("confirmWrite"));
	const FString RequestDigest = DigestJson(RequestIntent);
	if (RequestDigest.IsEmpty())
	{
		return FMCPToolResult::Error(
			TEXT("The execution request could not be hashed."),
			TEXT("digest_unavailable"),
			500);
	}
	if (const FString* ExistingRunId = RequestRuns.Find(RequestId))
	{
		const TSharedPtr<FChangeRecord>* Existing = Runs.Find(*ExistingRunId);
		if (!Existing || !Existing->IsValid()
			|| (*Existing)->RequestDigest != RequestDigest)
		{
			return FMCPToolResult::Error(
				TEXT("requestId was already used with a different Landscape change."),
				TEXT("request_id_conflict"),
				409);
		}
		FString ErrorCode;
		FString ErrorMessage;
		if (!ValidateChangeApproval(
				Params,
				(*Existing)->PlanDigest,
				ErrorCode,
				ErrorMessage))
		{
			return FMCPToolResult::Error(ErrorMessage, ErrorCode, 409);
		}
		if (!(*Existing)->FailureCode.IsEmpty())
		{
			TSharedRef<FJsonObject> Details = MakeFailureDetails(
				**Existing,
				(*Existing)->bRollbackVerified,
				(*Existing)->bRollbackVerified
					? FString()
					: TEXT("The previous automatic rollback was not verified; use the original runId and requestId to retry rollback."));
			Details->SetBoolField(TEXT("idempotentReplay"), true);
			return ErrorWithDetails(
				(*Existing)->FailureMessage,
				(*Existing)->FailureCode,
				(*Existing)->FailureHttpStatus,
				Details);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("runId"), (*Existing)->RunId);
		Result->SetStringField(TEXT("planDigest"), (*Existing)->PlanDigest);
		Result->SetStringField(
			TEXT("status"),
			(*Existing)->bRolledBack ? TEXT("rolledBack") : TEXT("succeeded"));
		Result->SetBoolField(TEXT("idempotentReplay"), true);
		Result->SetBoolField(TEXT("rolledBack"), (*Existing)->bRolledBack);
		Result->SetBoolField(TEXT("verified"), (*Existing)->bVerified);
		Result->SetBoolField(
			TEXT("rollbackVerified"),
			(*Existing)->bRollbackVerified);
		Result->SetStringField(
			TEXT("beforePackageDigest"),
			(*Existing)->BeforePackageDigest);
		if (!(*Existing)->AfterPackageDigest.IsEmpty())
		{
			Result->SetStringField(
				TEXT("afterPackageDigest"),
				(*Existing)->AfterPackageDigest);
		}
		Result->SetStringField(
			TEXT("artifactDirectory"),
			(*Existing)->ArtifactDirectory);
		return FMCPToolResult::Ok(Result);
	}

	const FMCPToolResult PlanResult = PlanChange(Params);
	if (!PlanResult.bSuccess || !PlanResult.Data.IsValid())
	{
		return PlanResult;
	}
	const TSharedPtr<FJsonObject> Plan = PlanResult.Data;
	const FString PlanDigest = Plan->GetStringField(TEXT("planDigest"));
	FString ApprovalCode;
	FString ApprovalMessage;
	if (!ValidateChangeApproval(
			Params,
			PlanDigest,
			ApprovalCode,
			ApprovalMessage))
	{
		return FMCPToolResult::Error(
			ApprovalMessage,
			ApprovalCode,
			409);
	}
	UWorld* World = GetEditorWorld();
	if (!World || World->GetPathName() != Plan->GetStringField(TEXT("world")))
	{
		return FMCPToolResult::Error(
			TEXT("The Editor world changed after planning."),
			TEXT("world_changed"),
			409);
	}
	TSharedPtr<FChangeRecord> Record = MakeShared<FChangeRecord>();
	Record->RunId =
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	Record->RequestId = RequestId;
	Record->RequestDigest = RequestDigest;
	Record->PlanDigest = PlanDigest;
	Record->WorldPath = World->GetPathName();
	Record->WorldObjectId = World->GetUniqueID();
	Record->WorldObject = World;
	Record->WorldPackage = World->GetPackage();
	Record->ArtifactDirectory =
		FPaths::Combine(ArtifactRoot(), TEXT("Runs"), Record->RunId);
	if (!IFileManager::Get().MakeDirectory(
			*Record->ArtifactDirectory,
			true)
		&& !IFileManager::Get().DirectoryExists(
			*Record->ArtifactDirectory))
	{
		return FMCPToolResult::Error(
			TEXT("The recovery artifact directory could not be created."),
			TEXT("artifact_write_failed"),
			500);
	}
	bool bRunRegistered = false;
	ON_SCOPE_EXIT
	{
		if (!bRunRegistered)
		{
			IFileManager::Get().DeleteDirectory(
				*Record->ArtifactDirectory,
				false,
				true);
		}
	};

	TArray<FString> LandscapeGuids;
	for (const TSharedPtr<FJsonValue>& Value :
		Plan->GetArrayField(TEXT("landscapeIds")))
	{
		LandscapeGuids.Add(Value->AsString());
	}
	TArray<FString> WaterIdentities;
	for (const TSharedPtr<FJsonValue>& Value :
		Plan->GetArrayField(TEXT("waterIdentities")))
	{
		WaterIdentities.Add(Value->AsString());
	}
	Record->LandscapeGuids = LandscapeGuids;
	Record->WaterIdentities = WaterIdentities;
	FString SnapshotError;
	Record->BeforeSnapshot = BuildSnapshotForTargets(
		World,
		LandscapeGuids,
		WaterIdentities,
		SnapshotError);
	if (!Record->BeforeSnapshot.IsValid()
		|| Record->BeforeSnapshot->GetStringField(TEXT("snapshotDigest"))
			!= Plan->GetStringField(TEXT("beforeSnapshotDigest"))
		|| Record->BeforeSnapshot->GetObjectField(TEXT("packageEvidence"))
				->GetStringField(TEXT("digest"))
			!= Plan->GetStringField(TEXT("beforePackageDigest")))
	{
		return FMCPToolResult::Error(
			TEXT("The Landscape or Water baseline changed after planning."),
			TEXT("plan_baseline_changed"),
			409);
	}
	Record->BeforeSnapshotDigest =
		Record->BeforeSnapshot->GetStringField(TEXT("snapshotDigest"));
	Record->BeforePackageDigest =
		Record->BeforeSnapshot->GetObjectField(TEXT("packageEvidence"))
			->GetStringField(TEXT("digest"));
	if (!SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				TEXT("before.snapshot.json")),
			Record->BeforeSnapshot)
		|| !SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				TEXT("plan.json")),
			Plan))
	{
		return FMCPToolResult::Error(
			TEXT("The recovery snapshot metadata could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	FString PackageBackupError;
	if (!CapturePackageFileBackups(
			Record->BeforeSnapshot,
			Record->ArtifactDirectory,
			Record->PackageFileBackups,
			PackageBackupError))
	{
		return FMCPToolResult::Error(
			PackageBackupError,
			TEXT("recovery_snapshot_failed"),
			409);
	}

	const TArray<TSharedPtr<FJsonValue>>& Operations =
		Plan->GetArrayField(TEXT("operations"));
	for (const TSharedPtr<FJsonValue>& Value : Operations)
	{
		const TSharedPtr<FJsonObject> Operation = Value->AsObject();
		const FString Action = LandscapeReadString(Operation, TEXT("action"));
		if (Action == TEXT("importHeightmap")
			|| Action == TEXT("importWeightmap"))
		{
			FLandscapeTarget Target;
			FString Error;
			FString Code;
			ResolveLandscape(
				World,
				LandscapeReadString(Operation, TEXT("landscapeId")),
				Target,
				Error,
				Code);
			FIntRect Extent;
			int32 Width = 0;
			int32 Height = 0;
			TArray<uint8> BackupBytes;
			FRasterBackup Backup;
			Backup.Kind =
				Action == TEXT("importHeightmap")
					? TEXT("height")
					: TEXT("weight");
			Backup.LandscapeGuid =
				LandscapeReadString(Operation, TEXT("landscapeId"));
			Backup.Layer = LandscapeReadString(Operation, TEXT("layer"));
			if (Action == TEXT("importHeightmap"))
			{
				TArray<uint16> HeightData;
				if (!CaptureHeight(
						Target,
						Extent,
						Width,
						Height,
						HeightData))
				{
					return FMCPToolResult::Error(
						TEXT("The height recovery snapshot could not be captured."),
						TEXT("snapshot_failed"),
						500);
				}
				BackupBytes = EncodeR16(HeightData);
			}
			else
			{
				ULandscapeLayerInfoObject* Layer =
					FindLayerInfo(Target, Backup.Layer);
				if (!CaptureWeight(
						Target,
						Layer,
						Extent,
						Width,
						Height,
						BackupBytes))
				{
					return FMCPToolResult::Error(
						TEXT("The weight recovery snapshot could not be captured."),
						TEXT("snapshot_failed"),
						500);
				}
			}
			Backup.Extent = Extent;
			const FString Stem = FString::Printf(
				TEXT("%02d-%s-%s.%s"),
				static_cast<int32>(Operation->GetNumberField(TEXT("index"))),
				*Backup.Kind,
				*SanitizeFileStem(Backup.Layer, TEXT("all")),
				Action == TEXT("importHeightmap") ? TEXT("r16") : TEXT("r8"));
			if (!SaveRasterBackup(
					Record->ArtifactDirectory,
					Stem,
					BackupBytes,
					Backup.Path,
					Backup.Sha256))
			{
				return FMCPToolResult::Error(
					TEXT("The raster recovery artifact could not be written."),
					TEXT("artifact_write_failed"),
					500);
			}
			Record->RasterBackups.Add(MoveTemp(Backup));
		}
		else if (Action == TEXT("setMaterial"))
		{
			FMaterialBackup Backup;
			Backup.LandscapeGuid =
				LandscapeReadString(Operation, TEXT("landscapeId"));
			Backup.MaterialPath =
				LandscapeReadString(Operation, TEXT("beforeMaterial"));
			Record->MaterialBackups.Add(MoveTemp(Backup));
		}
		else if (Action == TEXT("replaceLayerInfo"))
		{
			FLayerInfoBackup Backup;
			Backup.LandscapeGuid =
				LandscapeReadString(Operation, TEXT("landscapeId"));
			Backup.Layer = LandscapeReadString(Operation, TEXT("layer"));
			Backup.BeforeLayerInfoPath =
				LandscapeReadString(Operation, TEXT("beforeLayerInfo"));
			Backup.AfterLayerInfoPath =
				LandscapeReadString(Operation, TEXT("afterLayerInfo"));
			Record->LayerInfoBackups.Add(MoveTemp(Backup));
		}
		else if (Action == TEXT("waterCreate"))
		{
			FWaterBackup Backup;
			Backup.Action = Action;
			Backup.Before.Identity =
				LandscapeReadString(Operation, TEXT("managedId"));
			Backup.Before.ManagedId = Backup.Before.Identity;
			Record->WaterBackups.Add(MoveTemp(Backup));
		}
		else if (Action == TEXT("waterUpdate")
			|| Action == TEXT("waterDelete"))
		{
			FWaterBackup Backup;
			Backup.Action = Action;
			Backup.Before = WaterStateFromJson(
				Operation->GetObjectField(TEXT("before")));
			if (Action == TEXT("waterDelete"))
			{
				AActor* Actor = ResolveWater(
					World,
					LandscapeReadString(
						Operation,
						TEXT("waterIdentity")));
				FString BackupError;
				const FString Stem = FString::Printf(
					TEXT("%02d-water-%s"),
					static_cast<int32>(
						Operation->GetNumberField(TEXT("index"))),
					*SanitizeFileStem(
						Backup.Before.ManagedId,
						TEXT("actor")));
				if (!Actor
					|| !SaveWaterActorBackup(
						Record->ArtifactDirectory,
						Stem,
						Actor,
						Backup,
						BackupError))
				{
					return FMCPToolResult::Error(
						BackupError.IsEmpty()
							? TEXT("The Water actor recovery artifact could not be captured.")
							: BackupError,
						TEXT("snapshot_failed"),
						500);
				}
			}
			Record->WaterBackups.Add(MoveTemp(Backup));
		}
	}
	Record->Status = TEXT("running");
	if (!SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				TEXT("recovery.manifest.json")),
			MakeRecoveryManifest(*Record)))
	{
		return FMCPToolResult::Error(
			TEXT("The recovery manifest could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	// Register the run before the first mutation. A failed execution remains
	// addressable by runId so the original caller can retry rollback.
	Runs.Add(Record->RunId, Record);
	RequestRuns.Add(RequestId, Record->RunId);
	bRunRegistered = true;

	FString ApplyError;
	bool bForceAutomaticRollbackEvidenceFailure = false;
	int32 AppliedOperationCount = 0;
	auto FailExecution =
		[&](
			const FString& Message,
			const FString& Code,
			const int32 HttpStatus) -> FMCPToolResult
		{
			Record->FailureMessage = Message;
			Record->FailureCode = Code;
			Record->FailureHttpStatus = HttpStatus;
			Record->bVerified = false;
			Record->Status = TEXT("failed");
			TSharedPtr<FJsonObject> RestoredSnapshot;
			FString RollbackError;
			const bool bRollbackVerified = RestoreAndVerifyChange(
				World,
				Record,
				TEXT("automatic-rollback.snapshot.json"),
				bForceAutomaticRollbackEvidenceFailure,
				RestoredSnapshot,
				RollbackError);
			Record->bRollbackVerified = bRollbackVerified;
			Record->bRolledBack = bRollbackVerified;
			Record->Status = bRollbackVerified
				? TEXT("failedRolledBack")
				: TEXT("rollbackFailed");
			if (!bRollbackVerified)
			{
				if (!RestoredSnapshot.IsValid())
				{
					FString RetrySnapshotError;
					const TSet<FString> RequiredPackageNames =
						PackageNamesFromSnapshot(
							Record->BeforeSnapshot);
					RestoredSnapshot = BuildSnapshotForTargets(
						World,
						Record->LandscapeGuids,
						Record->WaterIdentities,
						RetrySnapshotError,
						&RequiredPackageNames);
				}
				FString RetryDigest;
				if (RestoredSnapshot.IsValid()
					&& VerifySnapshotDigest(
						RestoredSnapshot,
						RetryDigest))
				{
					Record->RollbackRetrySnapshotDigest = RetryDigest;
					Record->RollbackRetryPackageDigest =
						RestoredSnapshot
							->GetObjectField(TEXT("packageEvidence"))
							->GetStringField(TEXT("digest"));
				}
			}
			// The initial recovery manifest is already durable. This rewrite
			// enriches diagnostics but is deliberately best-effort on a path
			// that is already returning an error.
			SaveJsonFile(
				FPaths::Combine(
					Record->ArtifactDirectory,
					TEXT("recovery.manifest.json")),
				MakeRecoveryManifest(*Record));
			return ErrorWithDetails(
				Message,
				Code,
				HttpStatus,
				MakeFailureDetails(
					*Record,
					bRollbackVerified,
					RollbackError));
		};
	{
		FScopedTransaction Transaction(
			NSLOCTEXT(
				"UEAIIntegration",
				"LandscapeWaterChange",
				"UE AI Landscape and Water Change"));
		for (const TSharedPtr<FJsonValue>& Value : Operations)
		{
			const TSharedPtr<FJsonObject> Operation = Value->AsObject();
			const FString Action = LandscapeReadString(Operation, TEXT("action"));
			if (Action == TEXT("importHeightmap")
				|| Action == TEXT("importWeightmap")
				|| Action == TEXT("setMaterial")
				|| Action == TEXT("replaceLayerInfo"))
			{
				FLandscapeTarget Target;
				FString Error;
				FString Code;
				if (!ResolveLandscape(
						World,
						LandscapeReadString(Operation, TEXT("landscapeId")),
						Target,
						Error,
						Code))
				{
					ApplyError = Error;
					break;
				}
				MarkLandscapeTransactional(Target);
				if (Action == TEXT("setMaterial"))
				{
					ALandscapeProxy* Proxy = Target.Representative.Get();
					UMaterialInterface* Material =
						LoadMaterial(LandscapeReadString(Operation, TEXT("material")));
					if (!Proxy || !Material)
					{
						ApplyError = TEXT("The planned Landscape material is unavailable.");
						break;
					}
					SetLandscapeMaterialEditor(Proxy, Material);
					Proxy->MarkPackageDirty();
				}
				else if (Action == TEXT("replaceLayerInfo"))
				{
					ULandscapeLayerInfoObject* Before =
						LoadLayerInfo(
							LandscapeReadString(
								Operation,
								TEXT("beforeLayerInfo")));
					ULandscapeLayerInfoObject* After =
						LoadLayerInfo(
							LandscapeReadString(
								Operation,
								TEXT("afterLayerInfo")));
					if (!Target.Info.IsValid() || !Before || !After)
					{
						ApplyError = TEXT("The planned Layer Info assets are unavailable.");
						break;
					}
					Target.Info->ReplaceLayer(Before, After);
				}
				else
				{
					TArray<uint8> Bytes;
					const FString SourcePath =
						LandscapeReadString(Operation, TEXT("sourcePath"));
					if (!FFileHelper::LoadFileToArray(Bytes, *SourcePath)
						|| HashBytes(Bytes)
							!= LandscapeReadString(Operation, TEXT("sourceSha256")))
					{
						ApplyError =
							TEXT("The raster source changed after planning.");
						break;
					}
					FIntRect Extent;
					int32 Width = 0;
					int32 Height = 0;
					if (!LandscapeExtent(Target, Extent, Width, Height))
					{
						ApplyError = TEXT("The Landscape extent is unavailable.");
						break;
					}
					if (!Target.Info.IsValid()
						|| Target.Info->HasUnloadedComponentsInRegion(
							Extent.Min.X,
							Extent.Min.Y,
							Extent.Max.X,
							Extent.Max.Y))
					{
						ApplyError =
							TEXT("Landscape components became unloaded after planning.");
						break;
					}
					const bool bFlipY = ReadBool(Operation, TEXT("flipY"));
					if (Action == TEXT("importHeightmap"))
					{
						TArray<uint16> Heights;
						if (!DecodeR16(Bytes, Width, Height, bFlipY, Heights))
						{
							ApplyError = TEXT("The r16le height source is invalid.");
							break;
						}
						FHeightmapAccessor<false> Accessor(Target.Info.Get());
						Accessor.SetData(
							Extent.Min.X,
							Extent.Min.Y,
							Extent.Max.X,
							Extent.Max.Y,
							Heights.GetData());
						Accessor.Flush();
					}
					else
					{
						TArray<uint8> Weights;
						ULandscapeLayerInfoObject* Layer =
							FindLayerInfo(
								Target,
								LandscapeReadString(Operation, TEXT("layer")));
						if (!Layer
							|| !DecodeR8(
								Bytes,
								Width,
								Height,
								bFlipY,
								Weights))
						{
							ApplyError = TEXT("The r8 weight source or target layer is invalid.");
							break;
						}
						FAlphamapAccessor<false, false> Accessor(
							Target.Info.Get(),
							Layer);
						Accessor.SetData(
							Extent.Min.X,
							Extent.Min.Y,
							Extent.Max.X,
							Extent.Max.Y,
							Weights.GetData(),
							ELandscapeLayerPaintingRestriction::None);
						Accessor.Flush();
					}
				}
			}
			else if (Action == TEXT("waterCreate"))
			{
				FTransform Transform;
				FString Error;
				const TSet<FString> BeforeActorGuids =
					CollectActorGuids(World);
				const TMap<FString, FString> BeforeWaterLayers =
					CollectWaterLandscapeLayers(World);
				ParseTransform(
					Operation->GetObjectField(TEXT("transform")),
					Transform,
					Error);
				AActor* CreatedActor = SpawnManagedWater(
						LandscapeReadString(Operation, TEXT("type")),
						LandscapeReadString(Operation, TEXT("managedId")),
						LandscapeReadString(Operation, TEXT("label")),
						Transform,
						Error);
				if (!CreatedActor)
				{
					ApplyError = Error;
					break;
				}
				const FString ManagedId =
					LandscapeReadString(Operation, TEXT("managedId"));
				FWaterBackup* Backup =
					Record->WaterBackups.FindByPredicate(
						[&ManagedId](const FWaterBackup& Candidate)
						{
							return Candidate.Action == TEXT("waterCreate")
								&& Candidate.Before.ManagedId == ManagedId;
						});
				if (!Backup)
				{
					ApplyError =
						TEXT("The created Water recovery record is unavailable.");
					break;
				}
				Backup->Created = CaptureWaterState(CreatedActor);
				CaptureWaterCreateSideEffects(
					World,
					BeforeActorGuids,
					BeforeWaterLayers,
					CreatedActor,
					*Backup);
				if (!IsCompleteWaterState(Backup->Created, true)
					|| Backup->Created.ManagedId != ManagedId)
				{
					ApplyError =
						TEXT("The complete created Water state could not be captured for rollback.");
					break;
				}
			}
			else
			{
				AActor* Actor = ResolveWater(
					World,
					LandscapeReadString(Operation, TEXT("waterIdentity")));
				if (!Actor)
				{
					ApplyError = TEXT("The planned Water actor is unavailable.");
					break;
				}
				if (Action == TEXT("waterDelete"))
				{
					if (!DeleteManagedWater(
							World,
							Actor,
							ApplyError))
					{
						break;
					}
				}
				else
				{
					FTransform Transform;
					FString Error;
					if (!ParseTransform(
							Operation->GetObjectField(TEXT("transform")),
							Transform,
							Error))
					{
						ApplyError = Error;
						break;
					}
					Actor->Modify();
					Actor->SetActorTransform(Transform);
					Actor->SetActorLabel(
						LandscapeReadString(Operation, TEXT("label")));
					Actor->MarkPackageDirty();
				}
			}
			++AppliedOperationCount;
#if WITH_DEV_AUTOMATION_TESTS
			if (AppliedOperationCount == 1
				&& (AutomationFailurePoint
						== FailureAfterFirstOperation
					|| AutomationFailurePoint
						== FailureAfterFirstOperationRollbackArtifact))
			{
				bForceAutomaticRollbackEvidenceFailure =
					AutomationFailurePoint
						== FailureAfterFirstOperationRollbackArtifact;
				AutomationFailurePoint = NAME_None;
				ApplyError =
					TEXT("Automation injected a failure after the first operation.");
				break;
			}
#endif
		}
		if (!ApplyError.IsEmpty())
		{
			Transaction.Cancel();
		}
	}

	if (!ApplyError.IsEmpty())
	{
		return FailExecution(
			ApplyError,
			TEXT("landscape_change_failed"),
			500);
	}

	const TSet<FString> RequiredPackageNames =
		PackageNamesFromSnapshot(Record->BeforeSnapshot);
	Record->AfterSnapshot = BuildSnapshotForTargets(
		World,
		LandscapeGuids,
		WaterIdentities,
		SnapshotError,
		&RequiredPackageNames);
	if (!Record->AfterSnapshot.IsValid())
	{
		return FailExecution(
			TEXT("Post-change read-back snapshot failed."),
			TEXT("verification_failed"),
			500);
	}
	FString VerifiedAfterDigest;
	if (!VerifySnapshotDigest(
			Record->AfterSnapshot,
			VerifiedAfterDigest))
	{
		return FailExecution(
			TEXT("The post-change snapshot digest could not be verified."),
			TEXT("verification_failed"),
			500);
	}
	Record->AfterSnapshotDigest = VerifiedAfterDigest;
	Record->AfterPackageDigest =
		Record->AfterSnapshot->GetObjectField(TEXT("packageEvidence"))
			->GetStringField(TEXT("digest"));
	if (!SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				TEXT("after.snapshot.json")),
			Record->AfterSnapshot))
	{
		return FailExecution(
			TEXT("The post-change snapshot artifact could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	const FMCPToolResult Diff = DiffSnapshotObjects(
		Record->BeforeSnapshot,
		Record->AfterSnapshot);
	if (!Diff.bSuccess || !Diff.Data.IsValid())
	{
		return FailExecution(
			Diff.ErrorMessage.IsEmpty()
				? TEXT("The post-change structural diff could not be verified.")
				: Diff.ErrorMessage,
			TEXT("verification_failed"),
			500);
	}
	if (!SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				TEXT("diff.json")),
			Diff.Data))
	{
		return FailExecution(
			TEXT("The verified structural diff artifact could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}
	Record->Status = TEXT("succeeded");
	Record->bVerified = true;
	if (!SaveJsonFile(
			FPaths::Combine(
				Record->ArtifactDirectory,
				TEXT("recovery.manifest.json")),
			MakeRecoveryManifest(*Record)))
	{
		return FailExecution(
			TEXT("The completed recovery manifest could not be written."),
			TEXT("artifact_write_failed"),
			500);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), TEXT("ue.landscape-change-result.v1"));
	Result->SetStringField(TEXT("changeSetId"), Record->RunId);
	Result->SetStringField(TEXT("runId"), Record->RunId);
	Result->SetStringField(TEXT("planDigest"), PlanDigest);
	Result->SetStringField(TEXT("status"), TEXT("succeeded"));
	Result->SetBoolField(TEXT("idempotentReplay"), false);
	Result->SetBoolField(TEXT("rolledBack"), false);
	Result->SetBoolField(TEXT("verified"), true);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
	Result->SetStringField(TEXT("beforeHash"), Record->BeforeSnapshotDigest);
	Result->SetStringField(TEXT("afterHash"), Record->AfterSnapshotDigest);
	Result->SetStringField(
		TEXT("recoveryExpiresAt"),
		(FDateTime::UtcNow() + FTimespan::FromDays(7.0)).ToIso8601());
	Result->SetStringField(
		TEXT("beforeSnapshotDigest"),
		Record->BeforeSnapshot->GetStringField(TEXT("snapshotDigest")));
	Result->SetStringField(
		TEXT("beforePackageDigest"),
		Record->BeforePackageDigest);
	Result->SetStringField(
		TEXT("afterSnapshotDigest"),
		Record->AfterSnapshotDigest);
	Result->SetStringField(
		TEXT("afterPackageDigest"),
		Record->AfterPackageDigest);
	Result->SetStringField(
		TEXT("artifactDirectory"),
		Record->ArtifactDirectory);
	Result->SetObjectField(TEXT("diff"), Diff.Data);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::RollbackChange(
	const TSharedPtr<FJsonObject>& Params)
{
	const FString RunId = LandscapeReadString(Params, TEXT("runId"));
	const FString RequestId = LandscapeReadString(Params, TEXT("requestId"));
	if (RunId.IsEmpty()
		|| RequestId.IsEmpty()
		|| !ReadBool(Params, TEXT("confirmWrite")))
	{
		return FMCPToolResult::Error(
			TEXT("runId, requestId, and confirmWrite=true are required."),
			TEXT("rollback_confirmation_required"),
			422);
	}
	TSharedPtr<FChangeRecord> Record;
	if (const TSharedPtr<FChangeRecord>* RecordPtr = Runs.Find(RunId);
		RecordPtr && RecordPtr->IsValid())
	{
		Record = *RecordPtr;
	}
	else
	{
		FString LoadCode;
		FString LoadError;
		if (!LoadLandscapeRecoveryRecord(
				RunId,
				Record,
				LoadCode,
				LoadError))
		{
			return FMCPToolResult::Error(
				LoadError,
				LoadCode,
				LoadCode == TEXT("run_not_found") ? 404 : 409);
		}
		Runs.Add(RunId, Record);
		RequestRuns.Add(Record->RequestId, RunId);
	}
	if (RequestId != Record->RequestId)
	{
		return FMCPToolResult::Error(
			TEXT("Rollback must use the requestId that owns the original Landscape change run."),
			TEXT("rollback_owner_mismatch"),
			409);
	}
	UWorld* World = GetEditorWorld();
	if (!IsOriginalWorld(World, *Record))
	{
		return FMCPToolResult::Error(
			TEXT("Rollback requires the original map to be loaded and unchanged."),
			TEXT("rollback_conflict"),
			409);
	}

	auto ReadCurrentSnapshot =
		[&](
			TSharedPtr<FJsonObject>& OutSnapshot,
			FString& OutSnapshotDigest,
			FString& OutPackageDigest,
			FString& OutError) -> bool
		{
			const TSet<FString> RequiredPackageNames =
				PackageNamesFromSnapshot(Record->BeforeSnapshot);
			OutSnapshot = BuildSnapshotForTargets(
				World,
				Record->LandscapeGuids,
				Record->WaterIdentities,
				OutError,
				&RequiredPackageNames);
			if (!OutSnapshot.IsValid()
				|| !VerifySnapshotDigest(
					OutSnapshot,
					OutSnapshotDigest))
			{
				if (OutError.IsEmpty())
				{
					OutError =
						TEXT("The current Landscape/Water state could not be verified.");
				}
				return false;
			}
			OutPackageDigest =
				OutSnapshot->GetObjectField(TEXT("packageEvidence"))
					->GetStringField(TEXT("digest"));
			return true;
		};

	if (Record->bRolledBack)
	{
		TSharedPtr<FJsonObject> Current;
		FString CurrentDigest;
		FString CurrentPackageDigest;
		FString CurrentError;
		if (!ReadCurrentSnapshot(
				Current,
				CurrentDigest,
				CurrentPackageDigest,
				CurrentError)
			|| CurrentDigest != Record->BeforeSnapshotDigest
			|| CurrentPackageDigest != Record->BeforePackageDigest)
		{
			return FMCPToolResult::Error(
				CurrentError.IsEmpty()
					? TEXT("The already-rolled-back run no longer matches its verified baseline.")
					: CurrentError,
				TEXT("rollback_conflict"),
				409);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("changeSetId"), RunId);
		Result->SetStringField(TEXT("runId"), RunId);
		Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
		Result->SetBoolField(TEXT("idempotentReplay"), true);
		Result->SetBoolField(TEXT("rollbackVerified"), true);
		Result->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
		Result->SetStringField(TEXT("snapshotDigest"), CurrentDigest);
		Result->SetStringField(
			TEXT("packageDigest"),
			CurrentPackageDigest);
		Result->SetStringField(
			TEXT("artifactDirectory"),
			Record->ArtifactDirectory);
		return FMCPToolResult::Ok(Result);
	}

	TSharedPtr<FJsonObject> Current;
	FString CurrentDigest;
	FString CurrentPackageDigest;
	FString CurrentError;
	if (!ReadCurrentSnapshot(
			Current,
			CurrentDigest,
			CurrentPackageDigest,
			CurrentError))
	{
		return FMCPToolResult::Error(
			CurrentError,
			TEXT("rollback_state_unavailable"),
			500);
	}

	const bool bSuccessfulRun = Record->bVerified
		&& Record->FailureCode.IsEmpty()
		&& !Record->AfterSnapshotDigest.IsEmpty();
	// A dirty-only change can disappear naturally when the Editor process exits
	// without saving. If the newly loaded project already matches the complete
	// pre-change semantic and package baselines, rollback is durably satisfied;
	// record that terminal state instead of misclassifying it as an external
	// modification conflict.
	if (bSuccessfulRun
		&& CurrentDigest == Record->BeforeSnapshotDigest
		&& CurrentPackageDigest == Record->BeforePackageDigest)
	{
		Record->Status = TEXT("rolledBack");
		Record->bRolledBack = true;
		Record->bRollbackVerified = true;
		if (!SaveJsonFile(
				FPaths::Combine(
					Record->ArtifactDirectory,
					TEXT("recovery.manifest.json")),
				MakeRecoveryManifest(*Record)))
		{
			Record->Status = TEXT("succeeded");
			Record->bRolledBack = false;
			Record->bRollbackVerified = false;
			return FMCPToolResult::Error(
				TEXT("The baseline is restored, but the recovery manifest could not be updated."),
				TEXT("artifact_write_failed"),
				500);
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("changeSetId"), RunId);
		Result->SetStringField(TEXT("runId"), RunId);
		Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
		Result->SetBoolField(TEXT("idempotentReplay"), true);
		Result->SetBoolField(TEXT("restoredByProcessRestart"), true);
		Result->SetBoolField(TEXT("rollbackVerified"), true);
		Result->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
		Result->SetStringField(TEXT("snapshotDigest"), CurrentDigest);
		Result->SetStringField(TEXT("packageDigest"), CurrentPackageDigest);
		Result->SetStringField(TEXT("artifactDirectory"), Record->ArtifactDirectory);
		return FMCPToolResult::Ok(Result);
	}
	if (bSuccessfulRun
		&& CurrentDigest != Record->AfterSnapshotDigest)
	{
		return FMCPToolResult::Error(
			TEXT("The current Landscape/Water semantic state changed after execution; rollback will not overwrite external changes."),
			TEXT("rollback_conflict"),
			409);
	}
	if (!bSuccessfulRun)
	{
		if (Record->RollbackRetrySnapshotDigest.IsEmpty()
			|| CurrentDigest != Record->RollbackRetrySnapshotDigest
			|| CurrentPackageDigest != Record->RollbackRetryPackageDigest)
		{
			return FMCPToolResult::Error(
				TEXT("A failed run can be retried only while the current state matches the captured rollback-retry baseline."),
				TEXT("rollback_conflict"),
				409);
		}
	}

	TSharedPtr<FJsonObject> Restored;
	FString RestoreError;
	if (!RestoreAndVerifyChange(
			World,
			Record,
			TEXT("rollback.snapshot.json"),
			false,
			Restored,
			RestoreError))
	{
		Record->Status = TEXT("rollbackFailed");
		Record->bRollbackVerified = false;
		return ErrorWithDetails(
			RestoreError,
			TEXT("rollback_failed"),
			500,
			MakeFailureDetails(*Record, false, RestoreError));
	}
	const FString PreviousStatus = Record->Status;
	Record->Status = Record->FailureCode.IsEmpty()
		? TEXT("rolledBack")
		: TEXT("failedRolledBack");
	Record->bRolledBack = true;
	Record->bRollbackVerified = true;
	if (!SaveJsonFile(
		FPaths::Combine(
			Record->ArtifactDirectory,
			TEXT("recovery.manifest.json")),
		MakeRecoveryManifest(*Record)))
	{
		Record->Status = PreviousStatus;
		Record->bRolledBack = false;
		Record->bRollbackVerified = false;
		return ErrorWithDetails(
			TEXT("Rollback data was restored, but the recovery manifest could not be updated."),
			TEXT("artifact_write_failed"),
			500,
			MakeFailureDetails(
				*Record,
				false,
				TEXT("recovery.manifest.json could not be written")));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("changeSetId"), RunId);
	Result->SetStringField(TEXT("runId"), RunId);
	Result->SetStringField(TEXT("status"), TEXT("rolledBack"));
	Result->SetBoolField(TEXT("idempotentReplay"), false);
	Result->SetBoolField(TEXT("rollbackVerified"), true);
	Result->SetStringField(TEXT("rollbackDurability"), TEXT("restart"));
	Result->SetStringField(
		TEXT("snapshotDigest"),
		Record->BeforeSnapshotDigest);
	Result->SetStringField(
		TEXT("packageDigest"),
		Record->BeforePackageDigest);
	Result->SetStringField(
		TEXT("artifactDirectory"),
		Record->ArtifactDirectory);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::ListWater(
	const TSharedPtr<FJsonObject>&) const
{
	UWorld* World = GetEditorWorld();
	TArray<TSharedPtr<FJsonValue>> Water;
	for (AActor* Actor : CollectWaterActors(World))
	{
		Water.Add(
			MakeShared<FJsonValueObject>(
				DescribeWaterActor(Actor)));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(
		TEXT("available"),
		FindWaterFactory(TEXT("lake")) != nullptr
			|| !Water.IsEmpty());
	Result->SetStringField(TEXT("world"), World ? World->GetPathName() : TEXT(""));
	Result->SetArrayField(TEXT("waterBodies"), Water);
	Result->SetNumberField(TEXT("total"), Water.Num());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult FLandscapeWaterService::GetWater(
	const TSharedPtr<FJsonObject>& Params) const
{
	const FString Selector = LandscapeReadString(Params, TEXT("water"));
	AActor* Actor = ResolveWater(GetEditorWorld(), Selector);
	if (!Actor)
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Water actor '%s' was not found."), *Selector),
			TEXT("water_not_found"),
			404);
	}
	return FMCPToolResult::Ok(DescribeWaterActor(Actor));
}

FMCPToolResult FLandscapeWaterService::ValidateWater(
	const TSharedPtr<FJsonObject>& Params) const
{
	TArray<AActor*> Actors;
	const FString Selector = LandscapeReadString(Params, TEXT("water"));
	if (Selector.IsEmpty())
	{
		Actors = CollectWaterActors(GetEditorWorld());
	}
	else if (AActor* Actor = ResolveWater(GetEditorWorld(), Selector))
	{
		Actors.Add(Actor);
	}
	else
	{
		return FMCPToolResult::Error(
			FString::Printf(TEXT("Water actor '%s' was not found."), *Selector),
			TEXT("water_not_found"),
			404);
	}
	TArray<TSharedPtr<FJsonValue>> Findings;
	for (AActor* Actor : Actors)
	{
		if (!Actor->GetRootComponent())
		{
			Findings.Add(
				MakeShared<FJsonValueObject>(
					MakeFinding(
						TEXT("water.root_component_missing"),
						TEXT("high"),
						1.0,
						Actor->GetPathName(),
						TEXT("Water"),
						TEXT(""),
						TEXT("The Water actor has no root component."))));
		}
		if (Actor->GetActorScale3D().IsNearlyZero())
		{
			Findings.Add(
				MakeShared<FJsonValueObject>(
					MakeFinding(
						TEXT("water.zero_scale"),
						TEXT("high"),
						1.0,
						Actor->GetPathName(),
						TEXT("Water"),
						TEXT(""),
						TEXT("The Water actor has a zero scale."))));
		}
		bool bHasWaterComponent = false;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			bHasWaterComponent |= Component
				&& Component->GetClass()->GetName().Contains(
					TEXT("WaterBody"));
		}
		if (!bHasWaterComponent)
		{
			Findings.Add(
				MakeShared<FJsonValueObject>(
					MakeFinding(
						TEXT("water.body_component_missing"),
						TEXT("high"),
						1.0,
						Actor->GetPathName(),
						TEXT("Water"),
						TEXT(""),
						TEXT("The Water actor has no loaded Water Body component."))));
		}
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(
		TEXT("available"),
		FindWaterFactory(TEXT("lake")) != nullptr
			|| !Actors.IsEmpty());
	Result->SetBoolField(TEXT("valid"), Findings.IsEmpty());
	Result->SetNumberField(TEXT("waterBodyCount"), Actors.Num());
	Result->SetArrayField(TEXT("findings"), Findings);
	Result->SetNumberField(TEXT("findingCount"), Findings.Num());
	Result->SetStringField(
		TEXT("evidenceBoundary"),
		TEXT("Validation covers loaded Water actors and components; water rendering, brush rebuild, collision, and cook require their dedicated runtime or build evidence."));
	return FMCPToolResult::Ok(Result);
}
}
