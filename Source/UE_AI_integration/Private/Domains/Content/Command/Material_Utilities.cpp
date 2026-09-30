#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/MaterialAssetHelpers.h"
#include "Infrastructure/MaterialCustomEditing.h"
#include "Workflow/UEWorkflowExecutionContext.h"

#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureBase.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "ImageUtils.h"
#include "ObjectTools.h"
#include "RHI.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"

namespace
{
	FString ReadAssetPath(const TSharedPtr<FJsonObject>& Params)
	{
		FString Path;
		Params->TryGetStringField(TEXT("assetPath"), Path);
		return Path;
	}

	FMCPToolResult InvalidMaterialUtility(const FString& Message, const TCHAR* Code = TEXT("invalid_material_utility"),
	                                      int32 Status = 400)
	{
		return FMCPToolResult::Error(Message, Code, Status);
	}

	bool NormalizeExactAssetPath(const FString& Input, FString& OutObjectPath, FString& OutError)
	{
		if (Input.Len() == 0 || Input.Len() > 1024 || !Input.StartsWith(TEXT("/")))
		{
			OutError = TEXT("assetPath must be an absolute Unreal asset path (maximum 1024 characters).");
			return false;
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(Input);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("assetPath must contain a valid long package name.");
			return false;
		}
		OutObjectPath = Input.Contains(TEXT("."))
			                ? Input
			                : PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		if (OutObjectPath.Contains(TEXT(":")) || !FPackageName::IsValidObjectPath(OutObjectPath))
		{
			OutError = TEXT("assetPath must identify a top-level asset using a valid package or object path.");
			return false;
		}
		return true;
	}

	bool LoadMaterialInterface(const FString& Path, UMaterialInterface*& OutMaterial, FString& OutError)
	{
		OutMaterial = nullptr;
		FString ObjectPath;
		if (!NormalizeExactAssetPath(Path, ObjectPath, OutError))
		{
			return false;
		}
		OutMaterial = LoadObject<UMaterialInterface>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		if (!OutMaterial)
		{
			OutError = TEXT("assetPath does not identify a material or material instance.");
		}
		return OutMaterial != nullptr;
	}

	void SetCommonAssetFields(const UObject* Asset, const FString& RequestedPath, const TSharedRef<FJsonObject>& Result)
	{
		Result->SetStringField(TEXT("assetPath"), Asset ? Asset->GetPathName() : RequestedPath);
		Result->SetStringField(TEXT("assetClass"), Asset ? Asset->GetClass()->GetName() : TEXT(""));
	}

	void SetCompileErrors(const FMaterialResource* Resource, const TSharedRef<FJsonObject>& Result)
	{
		constexpr int32 MaxErrors = 64;
		constexpr int32 MaxErrorChars = 2048;
		TArray<TSharedPtr<FJsonValue>> ErrorValues;
		bool bTruncated = false;
		if (Resource)
		{
			const TArray<FString>& Errors = Resource->GetCompileErrors();
			bTruncated = Errors.Num() > MaxErrors;
			for (int32 Index = 0; Index < FMath::Min(Errors.Num(), MaxErrors); ++Index)
			{
				ErrorValues.Add(MakeShared<FJsonValueString>(Errors[Index].Left(MaxErrorChars)));
				bTruncated |= Errors[Index].Len() > MaxErrorChars;
			}
		}
		Result->SetArrayField(TEXT("compileErrors"), ErrorValues);
		Result->SetBoolField(TEXT("compileErrorsTruncated"), bTruncated);
	}

	void SetAvailableStatistics(UMaterialInterface* Material, const TSharedRef<FJsonObject>& Result)
	{
		// Resource accessors return sentinel/zero values while compilation is
		// incomplete. Null keeps that state distinct from a measured zero.
		for (const TCHAR* Field : {
			     TEXT("samplerCount"), TEXT("estimatedVertexTextureSamples"),
			     TEXT("estimatedPixelTextureSamples"), TEXT("estimatedVirtualTextureLookups"),
			     TEXT("usedUvScalars"), TEXT("usedCustomInterpolatorScalars"),
			     TEXT("vertexShaderInstructions"), TEXT("pixelShaderInstructions")
		     })
		{
			Result->SetField(Field, MakeShared<FJsonValueNull>());
		}
		const FMaterialResource* Resource = Material->GetMaterialResource(
			GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel));
		const bool bComplete = Resource && Resource->IsCompilationFinished()
			&& Resource->IsGameThreadShaderMapComplete();
		Result->SetBoolField(TEXT("resourceAvailable"), Resource != nullptr);
		Result->SetBoolField(TEXT("isCompiled"), bComplete);
		Result->SetBoolField(TEXT("statisticsAvailable"), bComplete);
		if (!bComplete)
		{
			return;
		}

		// GetStatistics can submit/wait for an incomplete resource. The explicit
		// completion check above makes this a read of its completed shader map.
		const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Material);
		Result->SetNumberField(TEXT("samplerCount"), Stats.NumSamplers);
		Result->SetNumberField(TEXT("estimatedVertexTextureSamples"), Stats.NumVertexTextureSamples);
		Result->SetNumberField(TEXT("estimatedPixelTextureSamples"), Stats.NumPixelTextureSamples);
		Result->SetNumberField(TEXT("estimatedVirtualTextureLookups"), Stats.NumVirtualTextureSamples);
		Result->SetNumberField(TEXT("usedUvScalars"), Stats.NumUVScalars);
		Result->SetNumberField(TEXT("usedCustomInterpolatorScalars"), Stats.NumInterpolatorScalars);
		Result->SetNumberField(TEXT("vertexShaderInstructions"), Stats.NumVertexShaderInstructions);
		Result->SetNumberField(TEXT("pixelShaderInstructions"), Stats.NumPixelShaderInstructions);
	}

	bool ParseTextureCompressionSetting(const FString& Requested, TextureCompressionSettings& OutSetting)
	{
		const FString Normalized = Requested.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("default"))
		{
			OutSetting = TC_Default;
			return true;
		}
		static const TMap<FString, TextureCompressionSettings> Aliases = {
			{TEXT("normalmap"), TC_Normalmap},
			{TEXT("grayscale"), TC_Grayscale},
			{TEXT("alpha"), TC_Alpha},
			{TEXT("masks"), TC_Masks},
			{TEXT("hdr"), TC_HDR},
			{TEXT("bc7"), TC_BC7},
			{TEXT("halffloat"), TC_HalfFloat},
		};
		if (const TextureCompressionSettings* Alias = Aliases.Find(Normalized))
		{
			OutSetting = *Alias;
			return true;
		}
		if (const UEnum* CompressionEnum = StaticEnum<TextureCompressionSettings>())
		{
			const int64 Value = CompressionEnum->GetValueByNameString(Requested);
			if (Value != INDEX_NONE)
			{
				OutSetting = static_cast<TextureCompressionSettings>(Value);
				return true;
			}
		}
		return false;
	}

	bool ParseTextureLodGroup(const FString& Requested, TextureGroup& OutGroup)
	{
		const FString Normalized = Requested.TrimStartAndEnd().ToLower();
		if (Normalized.IsEmpty() || Normalized == TEXT("world"))
		{
			OutGroup = TEXTUREGROUP_World;
			return true;
		}
		static const TMap<FString, TextureGroup> Aliases = {
			{TEXT("worldnormalmap"), TEXTUREGROUP_WorldNormalMap},
			{TEXT("worldspecular"), TEXTUREGROUP_WorldSpecular},
			{TEXT("character"), TEXTUREGROUP_Character},
			{TEXT("characternormalmap"), TEXTUREGROUP_CharacterNormalMap},
			{TEXT("characterspecular"), TEXTUREGROUP_CharacterSpecular},
			{TEXT("weapon"), TEXTUREGROUP_Weapon},
			{TEXT("weaponnormalmap"), TEXTUREGROUP_WeaponNormalMap},
			{TEXT("weaponspecular"), TEXTUREGROUP_WeaponSpecular},
			{TEXT("vehicle"), TEXTUREGROUP_Vehicle},
			{TEXT("vehiclenormalmap"), TEXTUREGROUP_VehicleNormalMap},
			{TEXT("vehiclespecular"), TEXTUREGROUP_VehicleSpecular},
			{TEXT("effects"), TEXTUREGROUP_Effects},
			{TEXT("ui"), TEXTUREGROUP_UI},
			{TEXT("skybox"), TEXTUREGROUP_Skybox},
		};
		if (const TextureGroup* Alias = Aliases.Find(Normalized))
		{
			OutGroup = *Alias;
			return true;
		}
		if (const UEnum* GroupEnum = StaticEnum<TextureGroup>())
		{
			const int64 Value = GroupEnum->GetValueByNameString(Requested);
			if (Value != INDEX_NONE)
			{
				OutGroup = static_cast<TextureGroup>(Value);
				return true;
			}
		}
		return false;
	}

	bool ReadOptionalInteger(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
	                         int32 MinValue, int32 MaxValue, int32& OutValue, FString& OutError)
	{
		OutValue = 0;
		if (!Params->HasField(Field))
		{
			return true;
		}
		double Number = 0.0;
		if (!Params->TryGetNumberField(Field, Number) || !FMath::IsFinite(Number)
			|| !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
		{
			OutError = FString::Printf(TEXT("%s must be an integer."), Field);
			return false;
		}
		OutValue = static_cast<int32>(Number);
		if (OutValue < MinValue || OutValue > MaxValue)
		{
			OutError = FString::Printf(TEXT("%s must be between %d and %d."), Field, MinValue, MaxValue);
			return false;
		}
		return true;
	}

	bool NormalizeTextureDestination(const FString& Requested, FString& OutObjectPath, FString& OutPackagePath,
	                                 FString& OutAssetName, FString& OutError)
	{
		if (!NormalizeExactAssetPath(Requested, OutObjectPath, OutError))
		{
			return false;
		}
		OutPackagePath = FPackageName::ObjectPathToPackageName(OutObjectPath);
		OutAssetName = FPackageName::GetShortName(OutObjectPath);
		if (OutPackagePath.IsEmpty() || OutAssetName.IsEmpty() || !FPackageName::IsValidObjectPath(OutObjectPath))
		{
			OutError = TEXT("destPath must identify one top-level texture package and asset.");
			return false;
		}
		return true;
	}

	class FTool_ImportMaterialTexture final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.texture.import"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString SourceFile;
			FString Destination;
			if (!Params->TryGetStringField(TEXT("sourceFile"), SourceFile) || SourceFile.TrimStartAndEnd().IsEmpty())
			{
				return InvalidMaterialUtility(TEXT("sourceFile is required."));
			}
			if (!Params->TryGetStringField(TEXT("destPath"), Destination) || Destination.IsEmpty())
			{
				return InvalidMaterialUtility(TEXT("destPath is required."));
			}
			SourceFile = FPaths::ConvertRelativePathToFull(SourceFile);
			if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*SourceFile))
			{
				return InvalidMaterialUtility(TEXT("sourceFile does not exist."), TEXT("source_file_not_found"), 404);
			}

			FString ObjectPath, PackagePath, AssetName, PathError;
			if (!NormalizeTextureDestination(Destination, ObjectPath, PackagePath, AssetName, PathError))
			{
				return InvalidMaterialUtility(PathError);
			}
			if (Params->HasField(TEXT("destName")))
			{
				if (!Params->TryGetStringField(TEXT("destName"), AssetName) || AssetName.IsEmpty()
					|| AssetName.Contains(TEXT("/")) || AssetName.Contains(TEXT(".")))
				{
					return InvalidMaterialUtility(TEXT("destName must be a non-empty asset name without '/' or '.'."));
				}
				ObjectPath = PackagePath + TEXT(".") + AssetName;
			}

			bool bReplaceExisting = false;
			bool bSave = true;
			if (Params->HasField(TEXT("replaceExisting")) && !Params->TryGetBoolField(
				TEXT("replaceExisting"), bReplaceExisting))
			{
				return InvalidMaterialUtility(TEXT("replaceExisting must be boolean."));
			}
			if (Params->HasField(TEXT("save")) && !Params->TryGetBoolField(TEXT("save"), bSave))
			{
				return InvalidMaterialUtility(TEXT("save must be boolean."));
			}
			if (!bReplaceExisting && LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn))
			{
				return InvalidMaterialUtility(
					TEXT("A texture already exists at destPath; set replaceExisting=true to replace it."),
					TEXT("texture_already_exists"), 409);
			}

			TextureCompressionSettings Compression = TC_Default;
			if (Params->HasField(TEXT("compression")))
			{
				FString CompressionName;
				if (!Params->TryGetStringField(TEXT("compression"), CompressionName)
					|| !ParseTextureCompressionSetting(CompressionName, Compression))
				{
					return InvalidMaterialUtility(
						TEXT("compression is not a supported TextureCompressionSettings value."));
				}
			}
			TextureGroup LodGroup = TEXTUREGROUP_World;
			if (Params->HasField(TEXT("lodGroup")))
			{
				FString GroupName;
				if (!Params->TryGetStringField(TEXT("lodGroup"), GroupName) || !ParseTextureLodGroup(
					GroupName, LodGroup))
				{
					return InvalidMaterialUtility(TEXT("lodGroup is not a supported TextureGroup value."));
				}
			}
			bool bSRGB = true;
			if (Params->HasField(TEXT("srgb")) && !Params->TryGetBoolField(TEXT("srgb"), bSRGB))
			{
				return InvalidMaterialUtility(TEXT("srgb must be boolean."));
			}
			int32 MaxTextureSize = 0;
			FString IntegerError;
			if (!ReadOptionalInteger(Params, TEXT("maxTextureSize"), 0, 16384, MaxTextureSize, IntegerError))
			{
				return InvalidMaterialUtility(IntegerError);
			}

			UAssetImportTask* ImportTask = NewObject<UAssetImportTask>();
			ImportTask->Filename = SourceFile;
			ImportTask->DestinationPath = PackagePath;
			ImportTask->DestinationName = AssetName;
			ImportTask->bAutomated = true;
			ImportTask->bReplaceExisting = bReplaceExisting;
			ImportTask->bReplaceExistingSettings = bReplaceExisting;
			ImportTask->bSave = false;
			ImportTask->bAsync = false;
			TArray<UAssetImportTask*> Tasks;
			Tasks.Add(ImportTask);
			FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks(Tasks);

			UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
			if (!Texture && !ImportTask->GetObjects().IsEmpty())
			{
				Texture = Cast<UTexture2D>(ImportTask->GetObjects()[0]);
			}
			if (!Texture)
			{
				return InvalidMaterialUtility(TEXT("Texture import did not produce a UTexture2D at destPath."),
				                              TEXT("texture_import_failed"), 500);
			}

			Texture->Modify();
			Texture->CompressionSettings = Compression;
			Texture->SRGB = bSRGB;
			Texture->LODGroup = LodGroup;
			if (MaxTextureSize > 0)
			{
				Texture->MaxTextureSize = MaxTextureSize;
			}
			Texture->PostEditChange();
			Texture->GetOutermost()->MarkPackageDirty();

			bool bSaved = false;
			if (bSave)
			{
				bSaved = UEditorAssetLibrary::SaveLoadedAsset(Texture, false);
			}
			const UEnum* CompressionEnum = StaticEnum<TextureCompressionSettings>();
			const UEnum* LodGroupEnum = StaticEnum<TextureGroup>();
			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(Texture, ObjectPath, Result);
			Result->SetStringField(TEXT("sourceFile"), SourceFile);
			Result->SetNumberField(TEXT("width"), Texture->GetSizeX());
			Result->SetNumberField(TEXT("height"), Texture->GetSizeY());
			Result->SetBoolField(TEXT("replaced"), bReplaceExisting);
			Result->SetBoolField(TEXT("saved"), bSave && bSaved);
			Result->SetBoolField(TEXT("saveRequested"), bSave);
			Result->SetBoolField(TEXT("readbackVerified"), Texture->SRGB == bSRGB
			                     && Texture->CompressionSettings == Compression && Texture->LODGroup == LodGroup
			                     && (MaxTextureSize <= 0 || Texture->MaxTextureSize == MaxTextureSize));
			if (CompressionEnum)
			{
				Result->SetStringField(
					TEXT("compression"), CompressionEnum->GetNameStringByValue(Texture->CompressionSettings));
			}
			if (LodGroupEnum)
			{
				Result->SetStringField(TEXT("lodGroup"), LodGroupEnum->GetNameStringByValue(Texture->LODGroup));
			}
			Result->SetBoolField(TEXT("srgb"), Texture->SRGB);
			Result->SetNumberField(TEXT("maxTextureSize"), Texture->MaxTextureSize);
			if (bSave && !bSaved)
			{
				FMCPToolResult Failure = InvalidMaterialUtility(
					TEXT("Texture imported and configured, but package save failed."),
					TEXT("texture_save_failed"), 500);
				Failure.Data = Result;
				return Failure;
			}
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_SaveMaterial final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.save"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (UEAIIntegration::Workflow::IsApprovedWorkflowExecution(Params))
			{
				return InvalidMaterialUtility(
					TEXT("Immediate material save is unavailable inside Workflow; use its final persistence policy."),
					TEXT("material_save_workflow_forbidden"));
			}
			const FString Path = ReadAssetPath(Params);
			UMaterialInterface* Material = nullptr;
			FString LoadError;
			if (!LoadMaterialInterface(Path, Material, LoadError))
			{
				return InvalidMaterialUtility(LoadError, TEXT("material_not_found"), 404);
			}
			if (!Material || !Material->GetOutermost() || Material->GetOutermost() == GetTransientPackage()
				|| Material->HasAnyFlags(RF_Transient) || !Material->HasAnyFlags(RF_Standalone))
			{
				return InvalidMaterialUtility(
					TEXT("Saving requires a standalone material asset in a mounted package."),
					TEXT("material_save_not_persistable"));
			}
			bool bOnlyIfDirty = true;
			if (Params->HasField(TEXT("onlyIfDirty")) && !Params->TryGetBoolField(TEXT("onlyIfDirty"), bOnlyIfDirty))
			{
				return InvalidMaterialUtility(TEXT("onlyIfDirty must be boolean."));
			}
			FString PackageFilename;
			if (!FPackageName::TryConvertLongPackageNameToFilename(Material->GetOutermost()->GetName(), PackageFilename,
			                                                       FPackageName::GetAssetPackageExtension()))
			{
				return InvalidMaterialUtility(
					TEXT("Material package is not mounted for persistence."), TEXT("material_save_not_persistable"));
			}
			const bool bFileExisted = IFileManager::Get().FileExists(*PackageFilename);
			const bool bWasDirty = Material->GetOutermost()->IsDirty();
			const bool bSaveAttempted = !bOnlyIfDirty || bWasDirty || !bFileExisted;
			bool bSaved = true;
			if (bSaveAttempted)
			{
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);
				bSaved = MCPMaterialInfrastructure::SaveMaterialPackage(Material);
				bSaved = bSaved && IFileManager::Get().FileExists(*PackageFilename)
					&& !Material->GetOutermost()->IsDirty();
			}
			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(Material, Path, Result);
			Result->SetBoolField(TEXT("wasDirty"), bWasDirty);
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetBoolField(TEXT("saveAttempted"), bSaveAttempted);
			if (!bSaved)
			{
				FMCPToolResult Failure = InvalidMaterialUtility(
					TEXT("Material package save or persistence readback failed; in-memory edits were retained."),
					TEXT("material_save_failed"), 500);
				Failure.Data = Result;
				return Failure;
			}
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_RecompileMaterial final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.recompile"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const FString Path = ReadAssetPath(Params);
			UMaterialInterface* MaterialInterface = nullptr;
			FString LoadError;
			if (!LoadMaterialInterface(Path, MaterialInterface, LoadError))
			{
				return InvalidMaterialUtility(LoadError, TEXT("material_not_found"), 404);
			}
			UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
			if (!Material)
			{
				return InvalidMaterialUtility(
					TEXT("The material interface has no base material."), TEXT("material_base_unavailable"));
			}
			bool bIncludeStats = false;
			if (Params->HasField(TEXT("includeStats")) && !Params->TryGetBoolField(TEXT("includeStats"), bIncludeStats))
			{
				return InvalidMaterialUtility(TEXT("includeStats must be boolean."));
			}
			const bool bShaderCacheRefreshed = UEAIIntegration::MaterialEditing::PrepareMaterialSourceValidation(
				Material);
			UMaterialEditingLibrary::RecompileMaterial(Material);
			const auto Diagnostics = UEAIIntegration::MaterialEditing::CompleteMaterialValidation(
				Material, bIncludeStats);
			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(MaterialInterface, Path, Result);
			Result->SetStringField(TEXT("status"), TEXT("recompileRequested"));
			Result->SetStringField(TEXT("compiledAssetPath"), Material->GetPathName());
			Result->SetBoolField(TEXT("shaderFileCacheRefreshed"), bShaderCacheRefreshed);
			Result->SetObjectField(TEXT("diagnostics"), Diagnostics);
			Result->SetBoolField(
				TEXT("compileCompleted"), Diagnostics->GetBoolField(TEXT("shaderValidationPerformed")));
			Result->SetBoolField(TEXT("statisticsAvailable"), false);
			Result->SetBoolField(TEXT("saved"), false);
			Result->SetArrayField(TEXT("compileErrors"), TArray<TSharedPtr<FJsonValue>>());
			Result->SetBoolField(TEXT("compileErrorsTruncated"), false);
			const FMaterialResource* RequestedResource = MaterialInterface->GetMaterialResource(
				GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel));
			if (bIncludeStats && RequestedResource && RequestedResource->IsGameThreadShaderMapComplete())
			{
				const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(MaterialInterface);
				Result->SetNumberField(TEXT("vertexShaderInstructions"), Stats.NumVertexShaderInstructions);
				Result->SetNumberField(TEXT("pixelShaderInstructions"), Stats.NumPixelShaderInstructions);
				Result->SetBoolField(TEXT("statisticsAvailable"), true);
				const FMaterialResource* Resource = MaterialInterface->GetMaterialResource(
					GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel));
				if (Resource)
				{
					const TArray<FString>& Errors = Resource->GetCompileErrors();
					TArray<TSharedPtr<FJsonValue>> ErrorValues;
					for (int32 Index = 0; Index < FMath::Min(Errors.Num(), 64); ++Index)
					{
						ErrorValues.Add(MakeShared<FJsonValueString>(Errors[Index].Left(2048)));
					}
					Result->SetArrayField(TEXT("compileErrors"), ErrorValues);
					Result->SetBoolField(
						TEXT("compileErrorsTruncated"),
						Errors.Num() > ErrorValues.Num() || Errors.ContainsByPredicate([](const FString& Error)
						{
							return Error.Len() > 2048;
						}));
				}
			}
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_GetMaterialCompilationStats final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.compilation.stats"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const FString Path = ReadAssetPath(Params);
			UMaterialInterface* MaterialInterface = nullptr;
			FString LoadError;
			if (!LoadMaterialInterface(Path, MaterialInterface, LoadError))
			{
				return InvalidMaterialUtility(LoadError, TEXT("material_not_found"), 404);
			}
			UMaterial* BaseMaterial = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
			if (!BaseMaterial)
			{
				return InvalidMaterialUtility(
					TEXT("Could not resolve a base material."), TEXT("material_base_unavailable"));
			}
			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(MaterialInterface, Path, Result);
			Result->SetStringField(TEXT("featureLevel"), GMaxRHIFeatureLevel == ERHIFeatureLevel::SM5
				                                             ? TEXT("SM5")
				                                             : GMaxRHIFeatureLevel == ERHIFeatureLevel::SM6
				                                             ? TEXT("SM6")
				                                             : TEXT("ES3_1"));
			Result->SetNumberField(TEXT("expressionCount"), BaseMaterial->GetExpressions().Num());
			SetAvailableStatistics(MaterialInterface, Result);
			if (!Result->GetBoolField(TEXT("resourceAvailable")))
			{
				Result->SetStringField(
					TEXT("note"),
					TEXT("Material resource is unavailable; run content.material.recompile in a rendering Editor."));
			}
			SetCompileErrors(
				MaterialInterface->GetMaterialResource(GetFeatureLevelShaderPlatform(GMaxRHIFeatureLevel)), Result);
			Result->SetBoolField(TEXT("compileTriggered"), false);
			Result->SetBoolField(TEXT("saved"), false);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_GetMaterialTextureProperties final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.texture.properties"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const FString Path = ReadAssetPath(Params);
			if (Path.IsEmpty())
				return InvalidMaterialUtility(
					TEXT("assetPath is required and must identify a texture."));
			FString ObjectPath, PathError;
			if (!NormalizeExactAssetPath(Path, ObjectPath, PathError)) return InvalidMaterialUtility(PathError);
			UTexture* Texture = LoadObject<UTexture>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
			if (!Texture)
				return InvalidMaterialUtility(FString::Printf(TEXT("Texture '%s' was not found."), *Path),
				                              TEXT("texture_not_found"), 404);
			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(Texture, Path, Result);
			Result->SetStringField(TEXT("textureType"), Texture->GetClass()->GetName());
			Result->SetStringField(TEXT("addressX"), UEnum::GetValueAsString(Texture->GetTextureAddressX()));
			Result->SetStringField(TEXT("addressY"), UEnum::GetValueAsString(Texture->GetTextureAddressY()));
			Result->SetBoolField(TEXT("sRGB"), Texture->SRGB);
			Result->SetBoolField(TEXT("virtualTextureStreaming"), Texture->VirtualTextureStreaming);
			Result->SetNumberField(
				TEXT("estimatedResourceBytes"),
				static_cast<double>(Texture->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)));
			if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
			{
				Result->SetNumberField(TEXT("width"), Texture2D->GetSizeX());
				Result->SetNumberField(TEXT("height"), Texture2D->GetSizeY());
				Result->SetNumberField(TEXT("mipCount"), Texture2D->GetNumMips());
				Result->SetNumberField(TEXT("sourceWidth"), Texture2D->Source.GetSizeX());
				Result->SetNumberField(TEXT("sourceHeight"), Texture2D->Source.GetSizeY());
				const EPixelFormat Format = Texture2D->GetPixelFormat();
				Result->SetStringField(
					TEXT("pixelFormat"), Format >= 0 && Format < PF_MAX ? GPixelFormats[Format].Name : TEXT("Unknown"));
				Result->SetBoolField(TEXT("hasAlpha"), Texture2D->HasAlphaChannel());
			}
			else if (UTextureCube* Cube = Cast<UTextureCube>(Texture))
			{
				Result->SetNumberField(TEXT("width"), Cube->GetSizeX());
				Result->SetNumberField(TEXT("height"), Cube->GetSizeX());
			}
			else if (UTextureRenderTarget2D* RenderTarget = Cast<UTextureRenderTarget2D>(Texture))
			{
				Result->SetNumberField(TEXT("width"), RenderTarget->SizeX);
				Result->SetNumberField(TEXT("height"), RenderTarget->SizeY);
				if (RenderTarget->OverrideFormat >= 0 && RenderTarget->OverrideFormat < PF_MAX)
				{
					Result->SetStringField(TEXT("pixelFormat"), GPixelFormats[RenderTarget->OverrideFormat].Name);
				}
			}
			if (UEnum* CompressionEnum = StaticEnum<TextureCompressionSettings>())
			{
				Result->SetStringField(
					TEXT("compression"), CompressionEnum->GetNameStringByValue(Texture->CompressionSettings));
			}
			Result->SetStringField(
				TEXT("recommendedSamplerType"),
				UEnum::GetValueAsString(UMaterialExpressionTextureBase::GetSamplerTypeForTexture(Texture)));
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_GetMaterialThumbnail final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.thumbnail.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!GIsEditor || !FApp::CanEverRender())
			{
				return InvalidMaterialUtility(
					TEXT(
						"Material thumbnail rendering requires an Editor build with a rendering RHI; no image was produced."),
					TEXT("capability_unavailable"), 503);
			}

			const FString RequestedPath = ReadAssetPath(Params);
			FString ObjectPath;
			FString PathError;
			if (!NormalizeExactAssetPath(RequestedPath, ObjectPath, PathError))
			{
				return InvalidMaterialUtility(PathError);
			}

			UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
			if (!Asset)
			{
				return InvalidMaterialUtility(
					TEXT("assetPath does not identify a loaded material or texture asset."),
					TEXT("material_not_found"), 404);
			}
			if (!Asset->IsA<UMaterialInterface>() && !Asset->IsA<UTexture>())
			{
				return InvalidMaterialUtility(
					TEXT("Thumbnail queries support material interfaces and textures only."),
					TEXT("unsupported_asset_type"), 422);
			}

			int32 Resolution = 256;
			if (Params->HasField(TEXT("resolution")))
			{
				double RequestedResolution = 0.0;
				if (!Params->TryGetNumberField(TEXT("resolution"), RequestedResolution)
					|| !FMath::IsFinite(RequestedResolution)
					|| !FMath::IsNearlyEqual(RequestedResolution, FMath::RoundToDouble(RequestedResolution)))
				{
					return InvalidMaterialUtility(TEXT("resolution must be an integer from 16 to 1024."));
				}
				Resolution = static_cast<int32>(RequestedResolution);
			}
			if (Resolution < 16 || Resolution > 1024)
			{
				return InvalidMaterialUtility(TEXT("resolution must be an integer from 16 to 1024."));
			}

			FObjectThumbnail Thumbnail;
			ThumbnailTools::RenderThumbnail(
				Asset,
				static_cast<uint32>(Resolution),
				static_cast<uint32>(Resolution),
				ThumbnailTools::EThumbnailTextureFlushMode::NeverFlush,
				nullptr,
				&Thumbnail);
			const int32 Width = Thumbnail.GetImageWidth();
			const int32 Height = Thumbnail.GetImageHeight();
			if (Width <= 0 || Height <= 0 || Thumbnail.AccessImageData().Num() < Width * Height * 4)
			{
				return InvalidMaterialUtility(
					TEXT("Thumbnail rendering produced no pixel data for this asset."),
					TEXT("thumbnail_empty"), 409);
			}

			TArray64<uint8> PngData;
			const TArray<uint8>& ImageData = Thumbnail.AccessImageData();
			const FImageView ImageView(const_cast<uint8*>(ImageData.GetData()), Width, Height,
			                           ERawImageFormat::BGRA8);
			FImageUtils::CompressImage(PngData, TEXT(".png"), ImageView);
			if (PngData.Num() <= 0 || PngData.Num() > 8ll * 1024ll * 1024ll)
			{
				return InvalidMaterialUtility(
					TEXT("Thumbnail PNG compression failed or exceeded the 8 MiB response limit."),
					TEXT("thumbnail_encode_failed"), 500);
			}

			bool bSaveToFile = false;
			if (Params->HasField(TEXT("saveToFile"))
				&& !Params->TryGetBoolField(TEXT("saveToFile"), bSaveToFile))
			{
				return InvalidMaterialUtility(TEXT("saveToFile must be boolean."));
			}

			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(Asset, RequestedPath, Result);
			Result->SetStringField(TEXT("format"), TEXT("png"));
			Result->SetNumberField(TEXT("width"), Width);
			Result->SetNumberField(TEXT("height"), Height);
			Result->SetNumberField(TEXT("bytes"), static_cast<double>(PngData.Num()));
			Result->SetStringField(TEXT("renderMethod"), TEXT("thumbnail"));
			Result->SetBoolField(TEXT("saved"), bSaveToFile);

			if (bSaveToFile)
			{
				const FString PreviewDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UEAI"),
				                                                 TEXT("MaterialPreviews"));
				if (!IFileManager::Get().MakeDirectory(*PreviewDirectory, true))
				{
					return InvalidMaterialUtility(
						TEXT("Could not create the project Saved/UEAI/MaterialPreviews directory."),
						TEXT("thumbnail_file_failed"), 500);
				}
				const FString SafeName = FPaths::MakeValidFileName(FPaths::GetBaseFilename(Asset->GetPathName()),
				                                                   TCHAR('_'));
				const FString FilePath = FPaths::Combine(
					PreviewDirectory,
					FString::Printf(TEXT("%s_%d.png"), *SafeName, Resolution));
				if (!FFileHelper::SaveArrayToFile(PngData, *FilePath))
				{
					return InvalidMaterialUtility(
						TEXT("Could not persist the rendered thumbnail PNG."),
						TEXT("thumbnail_file_failed"), 500);
				}
				Result->SetStringField(TEXT("filePath"), FilePath);
			}
			else
			{
				Result->SetStringField(TEXT("encoding"), TEXT("base64"));
				Result->SetStringField(
					TEXT("data"), FBase64::Encode(PngData.GetData(), static_cast<uint32>(PngData.Num())));
			}
			return FMCPToolResult::Ok(Result);
		}
	};

	class FTool_CheckMaterialTilingQuality final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.material.tiling.quality"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			const FString RequestedPath = ReadAssetPath(Params);
			UMaterialInterface* MaterialInterface = nullptr;
			FString LoadError;
			if (!LoadMaterialInterface(RequestedPath, MaterialInterface, LoadError))
			{
				return InvalidMaterialUtility(LoadError, TEXT("material_not_found"), 404);
			}
			UMaterial* Material = MaterialInterface ? MaterialInterface->GetMaterial() : nullptr;
			if (!Material)
			{
				return InvalidMaterialUtility(
					TEXT("The material interface has no base material."), TEXT("material_base_unavailable"));
			}

			constexpr int32 MaxExpressions = 20000;
			constexpr int32 MaxIssues = 256;
			constexpr int32 MaxTraversalNodes = 4096;
			constexpr int32 MaxTraversalDepth = 4;
			TArray<TSharedPtr<FJsonValue>> Issues;
			bool bHasAntiTiling = false;
			bool bHasMacroVariation = false;
			bool bHasWorldPositionUVs = false;
			bool bHasNoiseNodes = false;
			bool bHasCustomHlsl = false;
			bool bAnalysisTruncated = false;

			auto AddIssue = [&](const TSharedRef<FJsonObject>& Issue)
			{
				if (Issues.Num() < MaxIssues)
				{
					Issues.Add(MakeShared<FJsonValueObject>(Issue));
				}
				else
				{
					bAnalysisTruncated = true;
				}
			};

			auto InspectExpression = [&](UMaterialExpression* Expression)
			{
				if (!Expression)
				{
					return;
				}
				const FString ClassName = Expression->GetClass()->GetName();
				const FString LowerClassName = ClassName.ToLower();
				if (LowerClassName.Contains(TEXT("worldposition")))
				{
					bHasWorldPositionUVs = true;
					bHasAntiTiling = true;
				}
				if (LowerClassName.Contains(TEXT("noise")))
				{
					bHasNoiseNodes = true;
					bHasAntiTiling = true;
				}
				if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(Expression))
				{
					bHasCustomHlsl = true;
					const FString Code = Custom->Code.ToLower();
					if (Code.Contains(TEXT("noise")) || Code.Contains(TEXT("hash"))
						|| Code.Contains(TEXT("random")) || Code.Contains(TEXT("fbm"))
						|| Code.Contains(TEXT("voronoi")) || Code.Contains(TEXT("perlin")))
					{
						bHasAntiTiling = true;
					}
				}
				if (UMaterialExpressionMaterialFunctionCall* FunctionCall =
					Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
				{
					if (FunctionCall->MaterialFunction)
					{
						const FString FunctionName = FunctionCall->MaterialFunction->GetName().ToLower();
						if (FunctionName.Contains(TEXT("antitile")) || FunctionName.Contains(TEXT("anti_tile"))
							|| FunctionName.Contains(TEXT("triplanar")) || FunctionName.Contains(TEXT("worldaligned"))
							|| FunctionName.Contains(TEXT("world_aligned")))
						{
							bHasAntiTiling = true;
						}
						if (FunctionName.Contains(TEXT("macro")) || FunctionName.Contains(TEXT("variation")))
						{
							bHasMacroVariation = true;
							bHasAntiTiling = true;
						}
					}
				}
			};

			int32 ExpressionCount = 0;
			int32 TextureSampleCount = 0;
			for (const TObjectPtr<UMaterialExpression>& Expression : Material->GetExpressions())
			{
				if (ExpressionCount >= MaxExpressions)
				{
					bAnalysisTruncated = true;
					break;
				}
				++ExpressionCount;
				InspectExpression(Expression);
				if (UMaterialExpressionTextureSample* TextureSample = Cast<
					UMaterialExpressionTextureSample>(Expression))
				{
					++TextureSampleCount;
					const bool bDirectTextureCoordinates = !TextureSample->Coordinates.Expression
						|| TextureSample->Coordinates.Expression->GetClass()->GetName().Contains(
							TEXT("TextureCoordinate"), ESearchCase::IgnoreCase);
					if (bDirectTextureCoordinates)
					{
						auto Issue = MakeShared<FJsonObject>();
						Issue->SetStringField(TEXT("type"), TEXT("directUv"));
						Issue->SetStringField(TEXT("expression"), TextureSample->GetName());
						Issue->SetStringField(TEXT("uvSource"), TextureSample->Coordinates.Expression
							                                        ? TextureSample->Coordinates.Expression->GetClass()
							                                        ->GetName()
							                                        : TEXT("defaultTexCoord0"));
						Issue->SetStringField(
							TEXT("suggestion"),
							TEXT(
								"Direct texture coordinates may show visible repetition; consider world-aligned UVs, macro variation, or an anti-tiling function."));
						if (TextureSample->Texture)
						{
							Issue->SetStringField(TEXT("texture"), TextureSample->Texture->GetPathName());
						}
						AddIssue(Issue);
					}
				}
			}

			auto AnalyzeUpstream = [&](const FExpressionInput* Root)
			{
				if (!Root || !Root->Expression)
				{
					return;
				}
				TArray<UMaterialExpression*> Current;
				Current.Add(Root->Expression);
				TSet<UMaterialExpression*> Seen;
				int32 Traversed = 0;
				for (int32 Depth = 0; Depth < MaxTraversalDepth && Current.Num() > 0; ++Depth)
				{
					TArray<UMaterialExpression*> Next;
					for (UMaterialExpression* Expression : Current)
					{
						if (!Expression || Seen.Contains(Expression))
						{
							continue;
						}
						if (Traversed++ >= MaxTraversalNodes)
						{
							bAnalysisTruncated = true;
							return;
						}
						Seen.Add(Expression);
						InspectExpression(Expression);
						if (UMaterialExpressionMaterialFunctionCall* FunctionCall =
							Cast<UMaterialExpressionMaterialFunctionCall>(Expression))
						{
							if (FunctionCall->MaterialFunction)
							{
								const FString FunctionName = FunctionCall->MaterialFunction->GetName().ToLower();
								bHasMacroVariation |= FunctionName.Contains(TEXT("macro"))
									|| FunctionName.Contains(TEXT("variation"));
							}
						}
						for (FExpressionInput* Input : Expression->GetInputsView())
						{
							if (Input && Input->Expression)
							{
								Next.Add(Input->Expression);
							}
						}
					}
					Current = MoveTemp(Next);
				}
			};
			AnalyzeUpstream(Material->GetExpressionInputForProperty(MP_BaseColor));
			AnalyzeUpstream(Material->GetExpressionInputForProperty(MP_Roughness));

			auto Result = MakeShared<FJsonObject>();
			SetCommonAssetFields(MaterialInterface, RequestedPath, Result);
			Result->SetStringField(TEXT("baseMaterial"), Material->GetPathName());
			Result->SetNumberField(TEXT("expressionCount"), ExpressionCount);
			Result->SetNumberField(TEXT("textureSampleCount"), TextureSampleCount);
			Result->SetNumberField(TEXT("tilingIssueCount"), Issues.Num());
			Result->SetBoolField(TEXT("hasAntiTiling"), bHasAntiTiling);
			Result->SetBoolField(TEXT("hasMacroVariation"), bHasMacroVariation);
			Result->SetBoolField(TEXT("hasWorldPositionUVs"), bHasWorldPositionUVs);
			Result->SetBoolField(TEXT("hasNoiseNodes"), bHasNoiseNodes);
			Result->SetBoolField(TEXT("hasCustomHlsl"), bHasCustomHlsl);
			Result->SetBoolField(TEXT("analysisTruncated"), bAnalysisTruncated);
			Result->SetArrayField(TEXT("tilingIssues"), Issues);
			Result->SetBoolField(TEXT("compileTriggered"), false);
			Result->SetBoolField(TEXT("saved"), false);
			if (!bHasAntiTiling && TextureSampleCount > 0)
			{
				Result->SetStringField(
					TEXT("recommendation"),
					TEXT(
						"No anti-tiling signal was detected; consider world-aligned UVs, macro variation, or an anti-tiling material function."));
			}
			return FMCPToolResult::Ok(Result);
		}
	};
}

namespace UEAIIntegrationTools
{
	void RegisterMaterialUtilityTools(FMCPToolRegistry& Registry)
	{
		Registry.Register(MakeShared<FTool_ImportMaterialTexture>());
		Registry.Register(MakeShared<FTool_SaveMaterial>());
		Registry.Register(MakeShared<FTool_RecompileMaterial>());
		Registry.Register(MakeShared<FTool_GetMaterialCompilationStats>());
		Registry.Register(MakeShared<FTool_GetMaterialTextureProperties>());
		Registry.Register(MakeShared<FTool_GetMaterialThumbnail>());
		Registry.Register(MakeShared<FTool_CheckMaterialTilingQuality>());
	}
}
