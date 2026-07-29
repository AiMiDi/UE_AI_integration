// Plan-gated Static Mesh and Texture settings workflows.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/EngineeringContractUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "StaticMeshResources.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Subsystems/EditorAssetSubsystem.h"

namespace
{
using UEAIIntegration::Infrastructure::CanonicalizeJson;
using UEAIIntegration::Infrastructure::DigestJson;
using UEAIIntegration::Infrastructure::ValidateChangeApproval;

enum class EContentSettingsKind
{
	StaticMesh,
	Texture,
};

struct FLoadedSettingsAsset
{
	UObject* Asset = nullptr;
	FString ObjectPath;
	FString PackagePath;
};

struct FStaticMeshSettingsState
{
	bool bNaniteEnabled = false;
	bool bAllowCpuAccess = false;
	int32 LightMapCoordinateIndex = 0;
	int32 LightMapResolution = 0;
};

struct FTextureSettingsState
{
	TextureCompressionSettings Compression = TC_Default;
	bool bSRGB = true;
	bool bVirtualTextureStreaming = false;
	int32 LODBias = 0;
};

FString KindName(const EContentSettingsKind Kind)
{
	return Kind == EContentSettingsKind::StaticMesh
		? TEXT("staticMesh")
		: TEXT("texture");
}

FString KindDomain(const EContentSettingsKind Kind)
{
	return Kind == EContentSettingsKind::StaticMesh
		? TEXT("content.static_mesh")
		: TEXT("content.texture");
}

FString KindPlanName(const EContentSettingsKind Kind)
{
	return Kind == EContentSettingsKind::StaticMesh
		? TEXT("staticMeshSettings")
		: TEXT("textureSettings");
}

bool IsSettingsGamePackagePath(const FString& Path)
{
	return Path.StartsWith(TEXT("/Game/"))
		&& !Path.Contains(TEXT(".."))
		&& FPackageName::IsValidLongPackageName(Path);
}

bool ResolveSettingsAssetData(
	IAssetRegistry& Registry,
	const FString& Path,
	FAssetData& OutData)
{
	if (Path.Contains(TEXT(".")))
	{
		OutData = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
		if (OutData.IsValid())
		{
			return true;
		}
	}
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(FName(*Path), Assets);
	if (!Assets.IsEmpty())
	{
		OutData = Assets[0];
		return true;
	}
	return false;
}

bool LoadSettingsAsset(
	const EContentSettingsKind Kind,
	const FString& RequestedPath,
	const bool bRequireGameAsset,
	FLoadedSettingsAsset& OutAsset,
	FString& OutErrorCode,
	FString& OutError)
{
	if (RequestedPath.IsEmpty())
	{
		OutErrorCode = TEXT("invalid_asset_path");
		OutError = TEXT("asset must be a non-empty package or object path.");
		return false;
	}

	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			TEXT("AssetRegistry"))
			.Get();
	FAssetData Data;
	if (!ResolveSettingsAssetData(Registry, RequestedPath, Data))
	{
		OutErrorCode = TEXT("asset_not_found");
		OutError = FString::Printf(
			TEXT("Asset '%s' was not found."),
			*RequestedPath);
		return false;
	}

	UObject* Asset = Data.GetAsset();
	const bool bMatches =
		Kind == EContentSettingsKind::StaticMesh
			? Asset && Asset->IsA<UStaticMesh>()
			: Asset && Asset->IsA<UTexture2D>();
	if (!bMatches)
	{
		OutErrorCode = TEXT("asset_type_mismatch");
		OutError = FString::Printf(
			TEXT("Asset '%s' is not a %s."),
			*RequestedPath,
			Kind == EContentSettingsKind::StaticMesh
				? TEXT("Static Mesh")
				: TEXT("Texture2D"));
		return false;
	}

	const FString PackagePath = Data.PackageName.ToString();
	if (bRequireGameAsset && !IsSettingsGamePackagePath(PackagePath))
	{
		OutErrorCode = TEXT("asset_scope_forbidden");
		OutError = TEXT("Settings writes are restricted to project assets under /Game.");
		return false;
	}

	OutAsset.Asset = Asset;
	OutAsset.ObjectPath = Data.GetObjectPathString();
	OutAsset.PackagePath = PackagePath;
	return true;
}

FString EnumName(const UEnum* Enum, const int64 Value)
{
	return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(Value);
}

bool ParseCompression(
	const FString& Name,
	TextureCompressionSettings& OutValue)
{
	const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
	if (!Enum)
	{
		return false;
	}
	int64 Value = Enum->GetValueByNameString(Name);
	if (Value == INDEX_NONE)
	{
		Value = Enum->GetValueByName(FName(*Name));
	}
	if (Value == INDEX_NONE
		|| Value < 0
		|| Value >= static_cast<int64>(TC_MAX))
	{
		return false;
	}
	OutValue = static_cast<TextureCompressionSettings>(Value);
	return true;
}

FStaticMeshSettingsState CaptureStaticMeshState(const UStaticMesh* Mesh)
{
	FStaticMeshSettingsState State;
	State.bNaniteEnabled = Mesh->NaniteSettings.bEnabled;
	State.bAllowCpuAccess = Mesh->bAllowCPUAccess;
	State.LightMapCoordinateIndex = Mesh->GetLightMapCoordinateIndex();
	State.LightMapResolution = Mesh->GetLightMapResolution();
	return State;
}

FTextureSettingsState CaptureTextureState(const UTexture2D* Texture)
{
	FTextureSettingsState State;
	State.Compression = Texture->CompressionSettings;
	State.bSRGB = Texture->SRGB;
	State.bVirtualTextureStreaming = Texture->VirtualTextureStreaming;
	State.LODBias = Texture->LODBias;
	return State;
}

TSharedRef<FJsonObject> SerializeStaticMeshSettings(
	const FStaticMeshSettingsState& State)
{
	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetBoolField(TEXT("naniteEnabled"), State.bNaniteEnabled);
	Settings->SetBoolField(TEXT("allowCpuAccess"), State.bAllowCpuAccess);
	Settings->SetNumberField(
		TEXT("lightMapCoordinateIndex"),
		State.LightMapCoordinateIndex);
	Settings->SetNumberField(
		TEXT("lightMapResolution"),
		State.LightMapResolution);
	return Settings;
}

TSharedRef<FJsonObject> SerializeTextureSettings(
	const FTextureSettingsState& State)
{
	TSharedRef<FJsonObject> Settings = MakeShared<FJsonObject>();
	Settings->SetStringField(
		TEXT("compression"),
		EnumName(StaticEnum<TextureCompressionSettings>(), State.Compression));
	Settings->SetBoolField(TEXT("sRGB"), State.bSRGB);
	Settings->SetBoolField(
		TEXT("virtualTextureStreaming"),
		State.bVirtualTextureStreaming);
	Settings->SetNumberField(TEXT("lodBias"), State.LODBias);
	return Settings;
}

TSharedRef<FJsonObject> SerializeCurrentSettings(
	const EContentSettingsKind Kind,
	UObject* Asset)
{
	return Kind == EContentSettingsKind::StaticMesh
		? SerializeStaticMeshSettings(
			CaptureStaticMeshState(CastChecked<UStaticMesh>(Asset)))
		: SerializeTextureSettings(
			CaptureTextureState(CastChecked<UTexture2D>(Asset)));
}

TSharedRef<FJsonObject> BuildAssetFingerprint(
	const EContentSettingsKind Kind,
	const FLoadedSettingsAsset& Loaded)
{
	TSharedRef<FJsonObject> Fingerprint = MakeShared<FJsonObject>();
	Fingerprint->SetStringField(TEXT("objectPath"), Loaded.ObjectPath);
	Fingerprint->SetStringField(TEXT("package"), Loaded.PackagePath);
	Fingerprint->SetStringField(
		TEXT("class"),
		Loaded.Asset->GetClass()->GetPathName());
	Fingerprint->SetBoolField(
		TEXT("packageDirty"),
		Loaded.Asset->GetOutermost()
			&& Loaded.Asset->GetOutermost()->IsDirty());
	FString PackageFilename;
	if (FPackageName::DoesPackageExist(
		Loaded.PackagePath,
		&PackageFilename))
	{
		Fingerprint->SetNumberField(
			TEXT("packageSizeBytes"),
			static_cast<double>(
				IFileManager::Get().FileSize(*PackageFilename)));
		Fingerprint->SetStringField(
			TEXT("packageTimestamp"),
			IFileManager::Get().GetTimeStamp(*PackageFilename).ToIso8601());
	}
	if (Kind == EContentSettingsKind::StaticMesh)
	{
		const UStaticMesh* Mesh =
			CastChecked<UStaticMesh>(Loaded.Asset);
		Fingerprint->SetStringField(
			TEXT("lightingGuid"),
			Mesh->GetLightingGuid().ToString(
				EGuidFormats::DigitsWithHyphensLower));
		Fingerprint->SetNumberField(
			TEXT("sourceModelCount"),
			Mesh->GetSourceModels().Num());
		Fingerprint->SetNumberField(TEXT("lodCount"), Mesh->GetNumLODs());
	}
	else
	{
		const UTexture2D* Texture =
			CastChecked<UTexture2D>(Loaded.Asset);
		Fingerprint->SetStringField(
			TEXT("sourceId"),
			Texture->Source.GetId().ToString(
				EGuidFormats::DigitsWithHyphensLower));
		Fingerprint->SetNumberField(
			TEXT("sourceSizeX"),
			Texture->Source.GetSizeX());
		Fingerprint->SetNumberField(
			TEXT("sourceSizeY"),
			Texture->Source.GetSizeY());
		Fingerprint->SetNumberField(
			TEXT("sourceMipCount"),
			Texture->Source.GetNumMips());
	}
	return Fingerprint;
}

int32 GetStaticMeshUvChannelCount(const UStaticMesh* Mesh)
{
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.IsEmpty())
	{
		return 0;
	}
	return RenderData->LODResources[0]
		.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords();
}

void AddDiagnostic(
	TArray<TSharedPtr<FJsonValue>>& Diagnostics,
	const FString& Severity,
	const FString& Code,
	const FString& Message,
	const FString& Field = FString())
{
	TSharedRef<FJsonObject> Diagnostic = MakeShared<FJsonObject>();
	Diagnostic->SetStringField(TEXT("severity"), Severity);
	Diagnostic->SetStringField(TEXT("code"), Code);
	Diagnostic->SetStringField(TEXT("message"), Message);
	if (!Field.IsEmpty())
	{
		Diagnostic->SetStringField(TEXT("field"), Field);
	}
	Diagnostics.Add(MakeShared<FJsonValueObject>(Diagnostic));
}

bool ValidateStaticMeshState(
	const UStaticMesh* Mesh,
	const FStaticMeshSettingsState& State,
	TArray<TSharedPtr<FJsonValue>>& OutDiagnostics)
{
	bool bValid = true;
	if (State.LightMapResolution < 4
		|| State.LightMapResolution > 4096
		|| State.LightMapResolution % 4 != 0)
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("error"),
			TEXT("invalid_lightmap_resolution"),
			TEXT("lightMapResolution must be from 4 to 4096 and divisible by 4."),
			TEXT("lightMapResolution"));
		bValid = false;
	}
	if (State.LightMapCoordinateIndex < 0
		|| State.LightMapCoordinateIndex > 7)
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("error"),
			TEXT("invalid_lightmap_coordinate"),
			TEXT("lightMapCoordinateIndex must be from 0 to 7."),
			TEXT("lightMapCoordinateIndex"));
		bValid = false;
	}
	const int32 UvChannels = GetStaticMeshUvChannelCount(Mesh);
	if (UvChannels > 0 && State.LightMapCoordinateIndex >= UvChannels)
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("error"),
			TEXT("lightmap_uv_missing"),
			FString::Printf(
				TEXT("LOD 0 exposes %d UV channel(s), so lightMapCoordinateIndex %d is unavailable."),
				UvChannels,
				State.LightMapCoordinateIndex),
			TEXT("lightMapCoordinateIndex"));
		bValid = false;
	}
	if (State.bNaniteEnabled && !Mesh->GetRenderData())
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("warning"),
			TEXT("nanite_build_data_unavailable"),
			TEXT("Nanite is requested but current render data is unavailable; applying the plan will rebuild the mesh."),
			TEXT("naniteEnabled"));
	}
	return bValid;
}

bool IsPowerOfTwo(const int32 Value)
{
	return Value > 0 && (Value & (Value - 1)) == 0;
}

bool ValidateTextureState(
	const UTexture2D* Texture,
	const FTextureSettingsState& State,
	TArray<TSharedPtr<FJsonValue>>& OutDiagnostics)
{
	bool bValid = true;
	if (State.LODBias < -16 || State.LODBias > 16)
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("error"),
			TEXT("invalid_lod_bias"),
			TEXT("lodBias must be from -16 to 16."),
			TEXT("lodBias"));
		bValid = false;
	}
	if (State.Compression == TC_Normalmap && State.bSRGB)
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("warning"),
			TEXT("normalmap_srgb_enabled"),
			TEXT("Normal-map compression normally requires sRGB=false."),
			TEXT("sRGB"));
	}
	if (State.bVirtualTextureStreaming
		&& (!IsPowerOfTwo(Texture->Source.GetSizeX())
			|| !IsPowerOfTwo(Texture->Source.GetSizeY())))
	{
		AddDiagnostic(
			OutDiagnostics,
			TEXT("warning"),
			TEXT("virtual_texture_non_power_of_two"),
			TEXT("Virtual texture streaming on a non-power-of-two source may be rejected by the texture build."),
			TEXT("virtualTextureStreaming"));
	}
	return bValid;
}

bool NormalizeStaticMeshSettings(
	const UStaticMesh* Mesh,
	const TSharedPtr<FJsonObject>& Requested,
	FStaticMeshSettingsState& OutTarget,
	TSharedRef<FJsonObject>& OutNormalized,
	FString& OutError)
{
	if (!Requested.IsValid() || Requested->Values.IsEmpty())
	{
		OutError = TEXT("request.settings must contain at least one supported field.");
		return false;
	}
	static const TSet<FString> Allowed = {
		TEXT("naniteEnabled"),
		TEXT("allowCpuAccess"),
		TEXT("lightMapCoordinateIndex"),
		TEXT("lightMapResolution"),
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Requested->Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			OutError = FString::Printf(
				TEXT("Unsupported Static Mesh setting '%s'."),
				*Pair.Key);
			return false;
		}
	}

	OutTarget = CaptureStaticMeshState(Mesh);
	OutNormalized = MakeShared<FJsonObject>();
	bool BoolValue = false;
	double NumberValue = 0.0;
	if (Requested->HasField(TEXT("naniteEnabled"))
		&& !Requested->HasTypedField<EJson::Boolean>(TEXT("naniteEnabled")))
	{
		OutError = TEXT("naniteEnabled must be a boolean.");
		return false;
	}
	if (Requested->HasField(TEXT("allowCpuAccess"))
		&& !Requested->HasTypedField<EJson::Boolean>(TEXT("allowCpuAccess")))
	{
		OutError = TEXT("allowCpuAccess must be a boolean.");
		return false;
	}
	if (Requested->HasField(TEXT("lightMapCoordinateIndex"))
		&& !Requested->HasTypedField<EJson::Number>(
			TEXT("lightMapCoordinateIndex")))
	{
		OutError = TEXT("lightMapCoordinateIndex must be a number.");
		return false;
	}
	if (Requested->HasField(TEXT("lightMapResolution"))
		&& !Requested->HasTypedField<EJson::Number>(
			TEXT("lightMapResolution")))
	{
		OutError = TEXT("lightMapResolution must be a number.");
		return false;
	}
	if (Requested->TryGetBoolField(TEXT("naniteEnabled"), BoolValue))
	{
		OutTarget.bNaniteEnabled = BoolValue;
		OutNormalized->SetBoolField(TEXT("naniteEnabled"), BoolValue);
	}
	if (Requested->TryGetBoolField(TEXT("allowCpuAccess"), BoolValue))
	{
		OutTarget.bAllowCpuAccess = BoolValue;
		OutNormalized->SetBoolField(TEXT("allowCpuAccess"), BoolValue);
	}
	if (Requested->TryGetNumberField(TEXT("lightMapCoordinateIndex"), NumberValue))
	{
		if (!FMath::IsNearlyEqual(NumberValue, FMath::RoundToDouble(NumberValue)))
		{
			OutError = TEXT("lightMapCoordinateIndex must be an integer.");
			return false;
		}
		OutTarget.LightMapCoordinateIndex = static_cast<int32>(NumberValue);
		OutNormalized->SetNumberField(
			TEXT("lightMapCoordinateIndex"),
			OutTarget.LightMapCoordinateIndex);
	}
	if (Requested->TryGetNumberField(TEXT("lightMapResolution"), NumberValue))
	{
		if (!FMath::IsNearlyEqual(NumberValue, FMath::RoundToDouble(NumberValue)))
		{
			OutError = TEXT("lightMapResolution must be an integer.");
			return false;
		}
		OutTarget.LightMapResolution = static_cast<int32>(NumberValue);
		OutNormalized->SetNumberField(
			TEXT("lightMapResolution"),
			OutTarget.LightMapResolution);
	}

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	if (!ValidateStaticMeshState(Mesh, OutTarget, Diagnostics))
	{
		OutError = Diagnostics[0]->AsObject()->GetStringField(TEXT("message"));
		return false;
	}
	return true;
}

bool NormalizeTextureSettings(
	const UTexture2D* Texture,
	const TSharedPtr<FJsonObject>& Requested,
	FTextureSettingsState& OutTarget,
	TSharedRef<FJsonObject>& OutNormalized,
	FString& OutError)
{
	if (!Requested.IsValid() || Requested->Values.IsEmpty())
	{
		OutError = TEXT("request.settings must contain at least one supported field.");
		return false;
	}
	static const TSet<FString> Allowed = {
		TEXT("compression"),
		TEXT("sRGB"),
		TEXT("virtualTextureStreaming"),
		TEXT("lodBias"),
	};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Requested->Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			OutError = FString::Printf(
				TEXT("Unsupported Texture setting '%s'."),
				*Pair.Key);
			return false;
		}
	}

	OutTarget = CaptureTextureState(Texture);
	OutNormalized = MakeShared<FJsonObject>();
	FString StringValue;
	bool BoolValue = false;
	double NumberValue = 0.0;
	if (Requested->HasField(TEXT("compression"))
		&& !Requested->HasTypedField<EJson::String>(TEXT("compression")))
	{
		OutError = TEXT("compression must be a string.");
		return false;
	}
	if (Requested->HasField(TEXT("sRGB"))
		&& !Requested->HasTypedField<EJson::Boolean>(TEXT("sRGB")))
	{
		OutError = TEXT("sRGB must be a boolean.");
		return false;
	}
	if (Requested->HasField(TEXT("virtualTextureStreaming"))
		&& !Requested->HasTypedField<EJson::Boolean>(
			TEXT("virtualTextureStreaming")))
	{
		OutError = TEXT("virtualTextureStreaming must be a boolean.");
		return false;
	}
	if (Requested->HasField(TEXT("lodBias"))
		&& !Requested->HasTypedField<EJson::Number>(TEXT("lodBias")))
	{
		OutError = TEXT("lodBias must be a number.");
		return false;
	}
	if (Requested->TryGetStringField(TEXT("compression"), StringValue))
	{
		if (!ParseCompression(StringValue, OutTarget.Compression))
		{
			OutError = FString::Printf(
				TEXT("Unknown TextureCompressionSettings value '%s'."),
				*StringValue);
			return false;
		}
		OutNormalized->SetStringField(
			TEXT("compression"),
			EnumName(
				StaticEnum<TextureCompressionSettings>(),
				OutTarget.Compression));
	}
	if (Requested->TryGetBoolField(TEXT("sRGB"), BoolValue))
	{
		OutTarget.bSRGB = BoolValue;
		OutNormalized->SetBoolField(TEXT("sRGB"), BoolValue);
	}
	if (Requested->TryGetBoolField(TEXT("virtualTextureStreaming"), BoolValue))
	{
		OutTarget.bVirtualTextureStreaming = BoolValue;
		OutNormalized->SetBoolField(TEXT("virtualTextureStreaming"), BoolValue);
	}
	if (Requested->TryGetNumberField(TEXT("lodBias"), NumberValue))
	{
		if (!FMath::IsNearlyEqual(NumberValue, FMath::RoundToDouble(NumberValue)))
		{
			OutError = TEXT("lodBias must be an integer.");
			return false;
		}
		OutTarget.LODBias = static_cast<int32>(NumberValue);
		OutNormalized->SetNumberField(TEXT("lodBias"), OutTarget.LODBias);
	}

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	if (!ValidateTextureState(Texture, OutTarget, Diagnostics))
	{
		OutError = Diagnostics[0]->AsObject()->GetStringField(TEXT("message"));
		return false;
	}
	return true;
}

bool GetRequest(
	const TSharedPtr<FJsonObject>& Params,
	TSharedPtr<FJsonObject>& OutRequest,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* RequestPtr = nullptr;
	if (!Params.IsValid()
		|| !Params->TryGetObjectField(TEXT("request"), RequestPtr)
		|| !RequestPtr
		|| !RequestPtr->IsValid())
	{
		OutError = TEXT("request is required.");
		return false;
	}
	OutRequest = *RequestPtr;
	return true;
}

bool BuildSettingsPlan(
	const EContentSettingsKind Kind,
	const TSharedPtr<FJsonObject>& Request,
	TSharedPtr<FJsonObject>& OutPlan,
	FString& OutErrorCode,
	FString& OutError)
{
	FString AssetPath;
	const TSharedPtr<FJsonObject>* SettingsPtr = nullptr;
	if (!Request.IsValid()
		|| !Request->TryGetStringField(TEXT("asset"), AssetPath)
		|| AssetPath.IsEmpty()
		|| !Request->TryGetObjectField(TEXT("settings"), SettingsPtr)
		|| !SettingsPtr
		|| !SettingsPtr->IsValid())
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("request.asset and request.settings are required.");
		return false;
	}
	FString Persistence = TEXT("dirtyOnly");
	Request->TryGetStringField(TEXT("persistence"), Persistence);
	if (Persistence != TEXT("dirtyOnly")
		&& Persistence != TEXT("saveOnSuccess"))
	{
		OutErrorCode = TEXT("invalid_request");
		OutError = TEXT("persistence must be dirtyOnly or saveOnSuccess.");
		return false;
	}

	FLoadedSettingsAsset Loaded;
	if (!LoadSettingsAsset(
			Kind,
			AssetPath,
			true,
			Loaded,
			OutErrorCode,
			OutError))
	{
		return false;
	}
	const bool bPackageDirty =
		Loaded.Asset->GetOutermost()
		&& Loaded.Asset->GetOutermost()->IsDirty();
	if (Persistence == TEXT("saveOnSuccess") && bPackageDirty)
	{
		OutErrorCode = TEXT("asset_dirty");
		OutError =
			TEXT("saveOnSuccess is refused because the package already contains unsaved changes.");
		return false;
	}

	TSharedRef<FJsonObject> Current = SerializeCurrentSettings(Kind, Loaded.Asset);
	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> NormalizedChanges = MakeShared<FJsonObject>();
	if (Kind == EContentSettingsKind::StaticMesh)
	{
		FStaticMeshSettingsState TargetState;
		if (!NormalizeStaticMeshSettings(
			CastChecked<UStaticMesh>(Loaded.Asset),
			*SettingsPtr,
			TargetState,
			NormalizedChanges,
			OutError))
		{
			OutErrorCode = TEXT("invalid_settings");
			return false;
		}
		Target = SerializeStaticMeshSettings(TargetState);
	}
	else
	{
		FTextureSettingsState TargetState;
		if (!NormalizeTextureSettings(
			CastChecked<UTexture2D>(Loaded.Asset),
			*SettingsPtr,
			TargetState,
			NormalizedChanges,
			OutError))
		{
			OutErrorCode = TEXT("invalid_settings");
			return false;
		}
		Target = SerializeTextureSettings(TargetState);
	}

	TArray<TSharedPtr<FJsonValue>> ChangedFields;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair :
		NormalizedChanges->Values)
	{
		const TSharedPtr<FJsonValue> Before = Current->Values.FindRef(Pair.Key);
		if (!Before.IsValid()
			|| UEAIIntegration::Infrastructure::CanonicalizeJsonValue(Before)
				!= UEAIIntegration::Infrastructure::CanonicalizeJsonValue(
					Target->Values.FindRef(Pair.Key)))
		{
			ChangedFields.Add(MakeShared<FJsonValueString>(Pair.Key));
		}
	}
	ChangedFields.Sort(
		[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
		{
			return A->AsString() < B->AsString();
		});

	TSharedRef<FJsonObject> NormalizedRequest = MakeShared<FJsonObject>();
	NormalizedRequest->SetStringField(
		TEXT("schema"),
		TEXT("ue.content-settings-request.v1"));
	NormalizedRequest->SetStringField(TEXT("asset"), Loaded.ObjectPath);
	NormalizedRequest->SetStringField(TEXT("assetKind"), KindName(Kind));
	NormalizedRequest->SetStringField(TEXT("persistence"), Persistence);
	NormalizedRequest->SetObjectField(TEXT("settings"), NormalizedChanges);

	TSharedRef<FJsonObject> Preconditions = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> AssetFingerprint =
		BuildAssetFingerprint(Kind, Loaded);
	Preconditions->SetStringField(TEXT("asset"), Loaded.ObjectPath);
	Preconditions->SetStringField(TEXT("package"), Loaded.PackagePath);
	Preconditions->SetBoolField(TEXT("packageDirty"), bPackageDirty);
	Preconditions->SetStringField(TEXT("settingsHash"), DigestJson(Current));
	Preconditions->SetStringField(
		TEXT("assetHash"),
		DigestJson(AssetFingerprint));
	Preconditions->SetObjectField(
		TEXT("assetFingerprint"),
		AssetFingerprint);

	TSharedRef<FJsonObject> DigestInput = MakeShared<FJsonObject>();
	DigestInput->SetStringField(TEXT("contract"), TEXT("ue.change-plan.v1"));
	DigestInput->SetStringField(TEXT("domain"), KindDomain(Kind));
	DigestInput->SetStringField(TEXT("planKind"), KindPlanName(Kind));
	DigestInput->SetStringField(TEXT("persistence"), Persistence);
	DigestInput->SetObjectField(TEXT("request"), NormalizedRequest);
	DigestInput->SetObjectField(TEXT("preconditions"), Preconditions);
	const FString PlanDigest = DigestJson(DigestInput);
	if (PlanDigest.IsEmpty())
	{
		OutErrorCode = TEXT("digest_failed");
		OutError = TEXT("Unable to calculate the settings plan digest.");
		return false;
	}

	OutPlan = MakeShared<FJsonObject>();
	OutPlan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	OutPlan->SetStringField(TEXT("domain"), KindDomain(Kind));
	OutPlan->SetStringField(TEXT("planKind"), KindPlanName(Kind));
	OutPlan->SetStringField(TEXT("action"), TEXT("settingsApply"));
	OutPlan->SetStringField(TEXT("scope"), Loaded.ObjectPath);
	OutPlan->SetStringField(TEXT("persistence"), Persistence);
	OutPlan->SetStringField(TEXT("planDigest"), PlanDigest);
	OutPlan->SetStringField(TEXT("risk"), TEXT("confirmWrite"));
	OutPlan->SetBoolField(TEXT("changesState"), !ChangedFields.IsEmpty());
	OutPlan->SetBoolField(TEXT("confirmWriteRequired"), true);
	OutPlan->SetStringField(
		TEXT("rollbackBoundary"),
		TEXT("sameCallBeforeSave"));
	OutPlan->SetObjectField(TEXT("request"), NormalizedRequest);
	OutPlan->SetObjectField(TEXT("preconditions"), Preconditions);
	OutPlan->SetObjectField(TEXT("before"), Current);
	OutPlan->SetObjectField(TEXT("after"), Target);
	OutPlan->SetStringField(TEXT("beforeHash"), DigestJson(Current));
	OutPlan->SetStringField(TEXT("afterHash"), DigestJson(Target));
	OutPlan->SetArrayField(TEXT("changedFields"), ChangedFields);
	return true;
}

bool StaticMeshStateEquals(
	const FStaticMeshSettingsState& A,
	const FStaticMeshSettingsState& B)
{
	return A.bNaniteEnabled == B.bNaniteEnabled
		&& A.bAllowCpuAccess == B.bAllowCpuAccess
		&& A.LightMapCoordinateIndex == B.LightMapCoordinateIndex
		&& A.LightMapResolution == B.LightMapResolution;
}

bool TextureStateEquals(
	const FTextureSettingsState& A,
	const FTextureSettingsState& B)
{
	return A.Compression == B.Compression
		&& A.bSRGB == B.bSRGB
		&& A.bVirtualTextureStreaming == B.bVirtualTextureStreaming
		&& A.LODBias == B.LODBias;
}

void AssignStaticMeshState(
	UStaticMesh* Mesh,
	const FStaticMeshSettingsState& State)
{
	Mesh->NaniteSettings.bEnabled = State.bNaniteEnabled;
	Mesh->bAllowCPUAccess = State.bAllowCpuAccess;
	Mesh->SetLightMapCoordinateIndex(State.LightMapCoordinateIndex);
	Mesh->SetLightMapResolution(State.LightMapResolution);
}

void AssignTextureState(
	UTexture2D* Texture,
	const FTextureSettingsState& State)
{
	Texture->CompressionSettings = State.Compression;
	Texture->SRGB = State.bSRGB;
	Texture->VirtualTextureStreaming = State.bVirtualTextureStreaming;
	Texture->LODBias = State.LODBias;
}

bool SaveSettingsAsset(UObject* Asset)
{
	if (!GEditor || !Asset)
	{
		return false;
	}
	UEditorAssetSubsystem* AssetSubsystem =
		GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
	return AssetSubsystem
		&& AssetSubsystem->SaveLoadedAsset(Asset, false);
}

FMCPToolResult ApplySettings(
	const EContentSettingsKind Kind,
	const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Request;
	FString Error;
	if (!GetRequest(Params, Request, Error))
	{
		return FMCPToolResult::Error(
			Error,
			TEXT("invalid_request"),
			400);
	}

	TSharedPtr<FJsonObject> Plan;
	FString ErrorCode;
	if (!BuildSettingsPlan(Kind, Request, Plan, ErrorCode, Error))
	{
		return FMCPToolResult::Error(
			Error,
			ErrorCode,
			ErrorCode == TEXT("asset_not_found") ? 404 : 422);
	}
	if (!ValidateChangeApproval(
		Params,
		Plan->GetStringField(TEXT("planDigest")),
		ErrorCode,
		Error))
	{
		return FMCPToolResult::Error(
			Error,
			ErrorCode,
			ErrorCode == TEXT("plan_digest_mismatch") ? 409 : 422);
	}

	const TSharedPtr<FJsonObject> NormalizedRequest =
		Plan->GetObjectField(TEXT("request"));
	const FString AssetPath =
		NormalizedRequest->GetStringField(TEXT("asset"));
	FLoadedSettingsAsset Loaded;
	if (!LoadSettingsAsset(
		Kind,
		AssetPath,
		true,
		Loaded,
		ErrorCode,
		Error))
	{
		return FMCPToolResult::Error(
			Error,
			ErrorCode,
			ErrorCode == TEXT("asset_not_found") ? 404 : 422);
	}

	FString RequestId;
	Params->TryGetStringField(TEXT("requestId"), RequestId);
	const FString Persistence =
		NormalizedRequest->GetStringField(TEXT("persistence"));
	const TSharedPtr<FJsonObject> TargetJson =
		Plan->GetObjectField(TEXT("after"));
	const bool bChanged = Plan->GetBoolField(TEXT("changesState"));
	if (!bChanged)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.content-settings-mutation.v1"));
		Result->SetStringField(TEXT("asset"), Loaded.ObjectPath);
		Result->SetStringField(TEXT("assetKind"), KindName(Kind));
		Result->SetStringField(TEXT("requestId"), RequestId);
		Result->SetStringField(
			TEXT("planDigest"),
			Plan->GetStringField(TEXT("planDigest")));
		Result->SetObjectField(TEXT("settings"), TargetJson);
		TSharedRef<FJsonObject> Mutation = MakeShared<FJsonObject>();
		Mutation->SetBoolField(TEXT("changed"), false);
		Mutation->SetBoolField(TEXT("compiled"), false);
		Mutation->SetBoolField(TEXT("saved"), false);
		Mutation->SetBoolField(TEXT("verified"), true);
		Mutation->SetStringField(
			TEXT("beforeHash"),
			Plan->GetStringField(TEXT("beforeHash")));
		Mutation->SetStringField(
			TEXT("afterHash"),
			Plan->GetStringField(TEXT("afterHash")));
		Mutation->SetArrayField(
			TEXT("warnings"),
			TArray<TSharedPtr<FJsonValue>>());
		Mutation->SetArrayField(
			TEXT("errors"),
			TArray<TSharedPtr<FJsonValue>>());
		Result->SetObjectField(TEXT("mutation"), Mutation);
		return FMCPToolResult::Ok(Result);
	}

	if (Kind == EContentSettingsKind::StaticMesh && GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem =
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem
			&& AssetEditorSubsystem->FindEditorForAsset(Loaded.Asset, false))
		{
			return FMCPToolResult::Error(
				TEXT("Close the Static Mesh editor before applying settings; rebuilding an asset while its editor is open is unsafe on UE 5.3."),
				TEXT("asset_editor_open"),
				409);
		}
	}

	FScopedTransaction Transaction(
		FText::FromString(
			FString::Printf(
				TEXT("UE AI Apply %s Settings"),
				*KindName(Kind))));
	Loaded.Asset->Modify();

	bool bVerified = false;
	FStaticMeshSettingsState StaticMeshBefore;
	FTextureSettingsState TextureBefore;
	if (Kind == EContentSettingsKind::StaticMesh)
	{
		UStaticMesh* Mesh = CastChecked<UStaticMesh>(Loaded.Asset);
		StaticMeshBefore = CaptureStaticMeshState(Mesh);
		FStaticMeshSettingsState Target;
		TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
		if (!NormalizeStaticMeshSettings(
			Mesh,
			NormalizedRequest->GetObjectField(TEXT("settings")),
			Target,
			Ignored,
			Error))
		{
			Transaction.Cancel();
			return FMCPToolResult::Error(
				Error,
				TEXT("invalid_settings"),
				422);
		}
		Mesh->PreEditChange(nullptr);
		AssignStaticMeshState(Mesh, Target);
		Mesh->PostEditChange();
		bVerified =
			StaticMeshStateEquals(CaptureStaticMeshState(Mesh), Target);
		if (!bVerified)
		{
			Mesh->PreEditChange(nullptr);
			AssignStaticMeshState(Mesh, StaticMeshBefore);
			Mesh->PostEditChange();
			Transaction.Cancel();
		}
	}
	else
	{
		UTexture2D* Texture = CastChecked<UTexture2D>(Loaded.Asset);
		TextureBefore = CaptureTextureState(Texture);
		FTextureSettingsState Target;
		TSharedRef<FJsonObject> Ignored = MakeShared<FJsonObject>();
		if (!NormalizeTextureSettings(
			Texture,
			NormalizedRequest->GetObjectField(TEXT("settings")),
			Target,
			Ignored,
			Error))
		{
			Transaction.Cancel();
			return FMCPToolResult::Error(
				Error,
				TEXT("invalid_settings"),
				422);
		}
		Texture->PreEditChange(nullptr);
		AssignTextureState(Texture, Target);
		Texture->PostEditChange();
		bVerified =
			TextureStateEquals(CaptureTextureState(Texture), Target);
		if (!bVerified)
		{
			Texture->PreEditChange(nullptr);
			AssignTextureState(Texture, TextureBefore);
			Texture->PostEditChange();
			Transaction.Cancel();
		}
	}

	if (!bVerified)
	{
		return FMCPToolResult::Error(
			TEXT("Settings read-back did not match the approved target; the in-memory change was restored."),
			TEXT("verification_failed"),
			500);
	}

	Loaded.Asset->MarkPackageDirty();
	bool bSaved = false;
	if (Persistence == TEXT("saveOnSuccess"))
	{
		bSaved = SaveSettingsAsset(Loaded.Asset);
		if (!bSaved)
		{
			Loaded.Asset->PreEditChange(nullptr);
			if (Kind == EContentSettingsKind::StaticMesh)
			{
				AssignStaticMeshState(
					CastChecked<UStaticMesh>(Loaded.Asset),
					StaticMeshBefore);
			}
			else
			{
				AssignTextureState(
					CastChecked<UTexture2D>(Loaded.Asset),
					TextureBefore);
			}
			Loaded.Asset->PostEditChange();
			Transaction.Cancel();
			return FMCPToolResult::Error(
				TEXT("The package could not be saved; the in-memory settings were restored."),
				TEXT("asset_save_failed"),
				500);
		}
	}

	TSharedRef<FJsonObject> ReadBack =
		SerializeCurrentSettings(Kind, Loaded.Asset);
	const FString AfterHash = DigestJson(ReadBack);
	if (AfterHash != Plan->GetStringField(TEXT("afterHash")))
	{
		return FMCPToolResult::Error(
			TEXT("Final settings hash did not match the approved plan."),
			TEXT("verification_failed"),
			500);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("schema"),
		TEXT("ue.content-settings-mutation.v1"));
	Result->SetStringField(TEXT("asset"), Loaded.ObjectPath);
	Result->SetStringField(TEXT("assetKind"), KindName(Kind));
	Result->SetStringField(TEXT("requestId"), RequestId);
	Result->SetStringField(
		TEXT("planDigest"),
		Plan->GetStringField(TEXT("planDigest")));
	Result->SetObjectField(TEXT("settings"), ReadBack);
	TSharedRef<FJsonObject> Mutation = MakeShared<FJsonObject>();
	Mutation->SetBoolField(TEXT("changed"), true);
	Mutation->SetBoolField(
		TEXT("compiled"),
		Kind == EContentSettingsKind::StaticMesh);
	Mutation->SetBoolField(TEXT("saved"), bSaved);
	Mutation->SetBoolField(TEXT("verified"), true);
	Mutation->SetStringField(
		TEXT("beforeHash"),
		Plan->GetStringField(TEXT("beforeHash")));
	Mutation->SetStringField(TEXT("afterHash"), AfterHash);
	Mutation->SetArrayField(
		TEXT("warnings"),
		TArray<TSharedPtr<FJsonValue>>());
	Mutation->SetArrayField(
		TEXT("errors"),
		TArray<TSharedPtr<FJsonValue>>());
	Result->SetObjectField(TEXT("mutation"), Mutation);
	return FMCPToolResult::Ok(Result);
}

class FTool_SettingsGet : public FMCPToolBase
{
public:
	explicit FTool_SettingsGet(const EContentSettingsKind InKind)
		: Kind(InKind)
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		FLoadedSettingsAsset Loaded;
		FString ErrorCode;
		FString Error;
		if (!LoadSettingsAsset(
			Kind,
			AssetPath,
			false,
			Loaded,
			ErrorCode,
			Error))
		{
			return FMCPToolResult::Error(
				Error,
				ErrorCode,
				ErrorCode == TEXT("asset_not_found") ? 404 : 422);
		}
		TSharedRef<FJsonObject> Settings =
			SerializeCurrentSettings(Kind, Loaded.Asset);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.content-settings.v1"));
		Result->SetStringField(TEXT("asset"), Loaded.ObjectPath);
		Result->SetStringField(TEXT("assetKind"), KindName(Kind));
		Result->SetStringField(TEXT("package"), Loaded.PackagePath);
		Result->SetBoolField(
			TEXT("packageDirty"),
			Loaded.Asset->GetOutermost()
				&& Loaded.Asset->GetOutermost()->IsDirty());
		Result->SetObjectField(TEXT("settings"), Settings);
		Result->SetStringField(TEXT("settingsHash"), DigestJson(Settings));
		return FMCPToolResult::Ok(Result);
	}

private:
	EContentSettingsKind Kind;
};

class FTool_SettingsPlan : public FMCPToolBase
{
public:
	explicit FTool_SettingsPlan(const EContentSettingsKind InKind)
		: Kind(InKind)
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		TSharedPtr<FJsonObject> Request;
		FString Error;
		if (!GetRequest(Params, Request, Error))
		{
			return FMCPToolResult::Error(
				Error,
				TEXT("invalid_request"),
				400);
		}
		TSharedPtr<FJsonObject> Plan;
		FString ErrorCode;
		if (!BuildSettingsPlan(
			Kind,
			Request,
			Plan,
			ErrorCode,
			Error))
		{
			return FMCPToolResult::Error(
				Error,
				ErrorCode,
				ErrorCode == TEXT("asset_not_found") ? 404 : 422);
		}
		return FMCPToolResult::Ok(Plan);
	}

private:
	EContentSettingsKind Kind;
};

class FTool_SettingsApply : public FMCPToolBase
{
public:
	explicit FTool_SettingsApply(const EContentSettingsKind InKind)
		: Kind(InKind)
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return ApplySettings(Kind, Params);
	}

private:
	EContentSettingsKind Kind;
};

class FTool_SettingsValidate : public FMCPToolBase
{
public:
	explicit FTool_SettingsValidate(const EContentSettingsKind InKind)
		: Kind(InKind)
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString AssetPath;
		Params->TryGetStringField(TEXT("asset"), AssetPath);
		FLoadedSettingsAsset Loaded;
		FString ErrorCode;
		FString Error;
		if (!LoadSettingsAsset(
			Kind,
			AssetPath,
			false,
			Loaded,
			ErrorCode,
			Error))
		{
			return FMCPToolResult::Error(
				Error,
				ErrorCode,
				ErrorCode == TEXT("asset_not_found") ? 404 : 422);
		}

		TArray<TSharedPtr<FJsonValue>> Diagnostics;
		bool bValid = false;
		if (Kind == EContentSettingsKind::StaticMesh)
		{
			bValid = ValidateStaticMeshState(
				CastChecked<UStaticMesh>(Loaded.Asset),
				CaptureStaticMeshState(
					CastChecked<UStaticMesh>(Loaded.Asset)),
				Diagnostics);
		}
		else
		{
			bValid = ValidateTextureState(
				CastChecked<UTexture2D>(Loaded.Asset),
				CaptureTextureState(
					CastChecked<UTexture2D>(Loaded.Asset)),
				Diagnostics);
		}

		bool bMatchesExpected = true;
		const TSharedPtr<FJsonObject>* ExpectedPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("settings"), ExpectedPtr)
			&& ExpectedPtr
			&& ExpectedPtr->IsValid())
		{
			TSharedRef<FJsonObject> Normalized = MakeShared<FJsonObject>();
			TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
			if (Kind == EContentSettingsKind::StaticMesh)
			{
				FStaticMeshSettingsState TargetState;
				if (!NormalizeStaticMeshSettings(
					CastChecked<UStaticMesh>(Loaded.Asset),
					*ExpectedPtr,
					TargetState,
					Normalized,
					Error))
				{
					return FMCPToolResult::Error(
						Error,
						TEXT("invalid_settings"),
						422);
				}
				Target = SerializeStaticMeshSettings(TargetState);
			}
			else
			{
				FTextureSettingsState TargetState;
				if (!NormalizeTextureSettings(
					CastChecked<UTexture2D>(Loaded.Asset),
					*ExpectedPtr,
					TargetState,
					Normalized,
					Error))
				{
					return FMCPToolResult::Error(
						Error,
						TEXT("invalid_settings"),
						422);
				}
				Target = SerializeTextureSettings(TargetState);
			}
			bMatchesExpected =
				CanonicalizeJson(SerializeCurrentSettings(Kind, Loaded.Asset))
				== CanonicalizeJson(Target);
			if (!bMatchesExpected)
			{
				AddDiagnostic(
					Diagnostics,
					TEXT("error"),
					TEXT("settings_mismatch"),
					TEXT("Current settings do not match the requested values."));
				bValid = false;
			}
		}

		TSharedRef<FJsonObject> Settings =
			SerializeCurrentSettings(Kind, Loaded.Asset);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(
			TEXT("schema"),
			TEXT("ue.content-settings-validation.v1"));
		Result->SetStringField(TEXT("asset"), Loaded.ObjectPath);
		Result->SetStringField(TEXT("assetKind"), KindName(Kind));
		Result->SetBoolField(TEXT("valid"), bValid);
		Result->SetBoolField(TEXT("matchesExpected"), bMatchesExpected);
		Result->SetObjectField(TEXT("settings"), Settings);
		Result->SetStringField(TEXT("settingsHash"), DigestJson(Settings));
		Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
		Result->SetNumberField(
			TEXT("diagnosticCount"),
			Diagnostics.Num());
		return FMCPToolResult::Ok(Result);
	}

private:
	EContentSettingsKind Kind;
};

class FTool_StaticMeshSettingsGet final : public FTool_SettingsGet
{
public:
	FTool_StaticMeshSettingsGet()
		: FTool_SettingsGet(EContentSettingsKind::StaticMesh)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.static_mesh.settings.get");
	}
};

class FTool_StaticMeshSettingsPlan final : public FTool_SettingsPlan
{
public:
	FTool_StaticMeshSettingsPlan()
		: FTool_SettingsPlan(EContentSettingsKind::StaticMesh)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.static_mesh.settings.plan");
	}
};

class FTool_StaticMeshSettingsApply final : public FTool_SettingsApply
{
public:
	FTool_StaticMeshSettingsApply()
		: FTool_SettingsApply(EContentSettingsKind::StaticMesh)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.static_mesh.settings.apply");
	}
};

class FTool_StaticMeshSettingsValidate final : public FTool_SettingsValidate
{
public:
	FTool_StaticMeshSettingsValidate()
		: FTool_SettingsValidate(EContentSettingsKind::StaticMesh)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.static_mesh.settings.validate");
	}
};

class FTool_TextureSettingsGet final : public FTool_SettingsGet
{
public:
	FTool_TextureSettingsGet()
		: FTool_SettingsGet(EContentSettingsKind::Texture)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.texture.settings.get");
	}
};

class FTool_TextureSettingsPlan final : public FTool_SettingsPlan
{
public:
	FTool_TextureSettingsPlan()
		: FTool_SettingsPlan(EContentSettingsKind::Texture)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.texture.settings.plan");
	}
};

class FTool_TextureSettingsApply final : public FTool_SettingsApply
{
public:
	FTool_TextureSettingsApply()
		: FTool_SettingsApply(EContentSettingsKind::Texture)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.texture.settings.apply");
	}
};

class FTool_TextureSettingsValidate final : public FTool_SettingsValidate
{
public:
	FTool_TextureSettingsValidate()
		: FTool_SettingsValidate(EContentSettingsKind::Texture)
	{
	}
	FString GetCapabilityId() const override
	{
		return TEXT("content.texture.settings.validate");
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterContentAssetSettingsTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_StaticMeshSettingsGet>());
	Registry.Register(MakeShared<FTool_StaticMeshSettingsPlan>());
	Registry.Register(MakeShared<FTool_StaticMeshSettingsApply>());
	Registry.Register(MakeShared<FTool_StaticMeshSettingsValidate>());
	Registry.Register(MakeShared<FTool_TextureSettingsGet>());
	Registry.Register(MakeShared<FTool_TextureSettingsPlan>());
	Registry.Register(MakeShared<FTool_TextureSettingsApply>());
	Registry.Register(MakeShared<FTool_TextureSettingsValidate>());
}
}
