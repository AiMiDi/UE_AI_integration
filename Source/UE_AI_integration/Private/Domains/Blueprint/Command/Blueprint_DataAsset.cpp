// Blueprint-domain raw UObject/DataAsset creation, matching Monolith's
// create_data_asset contract.  The created object is intentionally generic:
// callers choose a native or plugin UObject class and can inspect its CDO
// properties through blueprint.cdo.properties.get.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "UEAIEmptyDataAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace
{
	UClass* ResolveDataAssetClass(const FString& ClassName)
	{
		if (ClassName.IsEmpty())
		{
			return nullptr;
		}

		// A full /Script or package object path is the least ambiguous form.  It
		// must be resolved before the short-name fallback, otherwise a similarly
		// named class from another module can be selected.
		UClass* ResolvedClass = FindObject<UClass>(nullptr, *ClassName);
		if (!ResolvedClass && ClassName.StartsWith(TEXT("/")))
		{
			ResolvedClass = LoadObject<UClass>(nullptr, *ClassName, nullptr, LOAD_NoWarn);
		}
		if (!ResolvedClass && ClassName.StartsWith(TEXT("/")))
		{
			ResolvedClass = StaticLoadClass(
				UObject::StaticClass(), nullptr, *ClassName, nullptr, LOAD_NoWarn);
		}
		if (ResolvedClass)
		{
			return ResolvedClass;
		}

		ResolvedClass = FindFirstObject<UClass>(
			*ClassName,
			EFindFirstObjectOptions::NativeFirst);
		if (!ResolvedClass)
		{
			ResolvedClass = FindFirstObject<UClass>(
				*(TEXT("U") + ClassName),
				EFindFirstObjectOptions::NativeFirst);
		}
		if (!ResolvedClass)
		{
			ResolvedClass = FindFirstObject<UClass>(
				*(TEXT("A") + ClassName),
				EFindFirstObjectOptions::NativeFirst);
		}
		return ResolvedClass;
	}

	class FTool_CreateDataAsset final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override
		{
			return TEXT("blueprint.data_asset.create");
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			if (!Params.IsValid())
			{
				return FMCPToolResult::Error(
					TEXT("Parameters are required."), TEXT("invalid_params"), 422);
			}

			FString SavePath;
			FString ClassName;
			if ((!Params->TryGetStringField(TEXT("savePath"), SavePath)
					&& !Params->TryGetStringField(TEXT("save_path"), SavePath))
				|| SavePath.IsEmpty())
			{
				return FMCPToolResult::Error(
					TEXT("Missing required parameter: save_path"),
					TEXT("invalid_params"),
					422);
			}
			if ((!Params->TryGetStringField(TEXT("className"), ClassName)
					&& !Params->TryGetStringField(TEXT("class_name"), ClassName))
				|| ClassName.IsEmpty())
			{
				return FMCPToolResult::Error(
					TEXT("Missing required parameter: class_name"),
					TEXT("invalid_params"),
					422);
			}
			if (!SavePath.StartsWith(TEXT("/Game/")))
			{
				return FMCPToolResult::Error(
					TEXT("save_path must be a /Game package path."),
					TEXT("invalid_package_path"),
					422);
			}

			int32 LastSlash = INDEX_NONE;
			if (!SavePath.FindLastChar(TEXT('/'), LastSlash)
				|| LastSlash == SavePath.Len() - 1)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("save_path must contain a non-empty asset name: %s"),
						*SavePath),
					TEXT("invalid_package_path"),
					422);
			}
			const FString AssetName = SavePath.Mid(LastSlash + 1);
			if (AssetName.Contains(TEXT(".")))
			{
				return FMCPToolResult::Error(
					TEXT("save_path must be a package path, without an object suffix."),
					TEXT("invalid_package_path"),
					422);
			}
			if (!FPackageName::IsValidLongPackageName(SavePath))
			{
				return FMCPToolResult::Error(
					TEXT("save_path must be a valid long package name under /Game."),
					TEXT("invalid_package_path"),
					422);
			}

			bool bSkipSave = false;
			if (Params->HasField(TEXT("skipSave")))
			{
				if (!Params->TryGetBoolField(TEXT("skipSave"), bSkipSave))
				{
					return FMCPToolResult::Error(
						TEXT("skipSave must be a boolean."), TEXT("invalid_params"), 422);
				}
			}
			else if (Params->HasField(TEXT("skip_save"))
				&& !Params->TryGetBoolField(TEXT("skip_save"), bSkipSave))
			{
				return FMCPToolResult::Error(
					TEXT("skipSave must be a boolean."), TEXT("invalid_params"), 422);
			}

			UClass* RequestedClass = ResolveDataAssetClass(ClassName);
			if (!RequestedClass)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Class not found: '%s'. Use a loaded class name or full /Script path."),
						*ClassName),
					TEXT("class_not_found"),
					404);
			}
			if (RequestedClass->IsChildOf(UBlueprint::StaticClass())
				|| RequestedClass->IsChildOf(UBlueprintGeneratedClass::StaticClass()))
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Class '%s' is a Blueprint class. Use blueprint.asset.create instead."),
						*RequestedClass->GetName()),
					TEXT("blueprint_class_unsupported"),
					422);
			}
			// UDataAsset itself is abstract by design. Use the plugin's concrete
			// empty schema for persistence while keeping every other abstract,
			// deprecated, or superseded class rejected.
			const bool bUseEmptyDataAssetSchema = RequestedClass == UDataAsset::StaticClass();
			if (RequestedClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				&& !(bUseEmptyDataAssetSchema && !RequestedClass->HasAnyClassFlags(
					CLASS_Deprecated | CLASS_NewerVersionExists)))
			{
				const TCHAR* Reason = RequestedClass->HasAnyClassFlags(CLASS_Abstract)
					                      ? TEXT("abstract")
					                      : RequestedClass->HasAnyClassFlags(CLASS_Deprecated)
					                      ? TEXT("deprecated")
					                      : TEXT("superseded by a newer version");
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Cannot instantiate class '%s': it is %s."),
						*RequestedClass->GetName(), Reason),
					TEXT("class_not_instantiable"),
					422);
			}
			if (RequestedClass->IsChildOf(AActor::StaticClass()))
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Class '%s' is Actor-derived; use an actor spawn operation."),
						*RequestedClass->GetName()),
					TEXT("actor_class_unsupported"),
					422);
			}

			const FString ObjectPath = SavePath + TEXT(".") + AssetName;
			IAssetRegistry& AssetRegistry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
					TEXT("AssetRegistry"))
				.Get();
			if (AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid()
				|| FindObject<UObject>(nullptr, *ObjectPath)
				|| FPackageName::DoesPackageExist(SavePath))
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Asset already exists at '%s'. Delete it first."),
						*SavePath),
					TEXT("asset_already_exists"),
					409);
			}

			FScopedTransaction Transaction(
				NSLOCTEXT("UEAIIntegration", "CreateDataAsset", "Create Data Asset"));
			UPackage* Package = CreatePackage(*SavePath);
			if (!Package)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Failed to create package at path: %s"), *SavePath),
					TEXT("package_create_failed"),
					500);
			}
			Package->FullyLoad();
			FString PackageFilename;
			if (!bSkipSave)
			{
				// SavePackage (used by SaveLoadedAsset) does not create missing
				// Content subdirectories. Automation commonly uses a fresh
				// /Game/Automation path, so create its mapped directory first.
				PackageFilename = FPackageName::LongPackageNameToFilename(
					SavePath,
					FPackageName::GetAssetPackageExtension());
				const FString PackageDirectory = FPaths::GetPath(PackageFilename);
				if (PackageDirectory.IsEmpty()
					|| !IFileManager::Get().MakeDirectory(*PackageDirectory, true))
				{
					Transaction.Cancel();
					return FMCPToolResult::Error(
						FString::Printf(
							TEXT("Failed to create the package directory for '%s' (resolved file '%s')."),
							*SavePath,
							*PackageFilename),
						TEXT("package_directory_create_failed"),
						500);
				}
			}

			// Unreal's package saver nulls instances whose class is abstract. Keep
			// UDataAsset as the requested schema, but store that alias as a concrete
			// empty plugin DataAsset so save/reload has a valid class token.
			UClass* CreationClass = bUseEmptyDataAssetSchema
				                        ? UUEAIEmptyDataAsset::StaticClass()
				                        : RequestedClass;
			auto CreateAsset = [&]() -> UObject*
			{
				return NewObject<UObject>(
					Package,
					CreationClass,
					FName(*AssetName),
					RF_Public | RF_Standalone | RF_Transactional);
			};
			UObject* NewAsset = CreateAsset();
			if (!NewAsset)
			{
				return FMCPToolResult::Error(
					FString::Printf(
						TEXT("Failed to instantiate class '%s' at '%s'."),
						*CreationClass->GetName(), *SavePath),
					TEXT("asset_create_failed"),
					500);
			}

			// Modify and mark every authored property so editor-only override state
			// is initialized consistently with a Details-panel edit.
			NewAsset->Modify();
			FAssetRegistryModule::AssetCreated(NewAsset);
			const bool bMarkedDirty = Package->MarkPackageDirty();
			(void)bMarkedDirty;
			const bool bAssetRegistryRegistered =
				AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();

			const bool bSaveAttempted = !bSkipSave;
			bool bSaved = false;
			if (!bSkipSave)
			{
				bSaved = UEditorAssetLibrary::SaveLoadedAsset(NewAsset, false);
				if (!bSaved)
				{
					// The editor helper validates registry membership before saving. A
					// freshly-created package can fail that check even after AssetCreated;
					// the package save path is the authoritative persistence fallback.
					FSavePackageArgs SaveArgs;
					SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
					bSaved = UPackage::SavePackage(
						Package,
						NewAsset,
						*PackageFilename,
						SaveArgs);
				}
				if (!bSaved)
				{
					// Saving is part of the create contract. Remove the transient
					// registry entry and standalone object when persistence fails.
					const bool bPackageDirtyBeforeRollback = Package->IsDirty();
					IFileManager::Get().Delete(*PackageFilename, false, true, true);
					FAssetRegistryModule::AssetDeleted(NewAsset);
					NewAsset->ClearFlags(RF_Public | RF_Standalone);
					NewAsset->MarkAsGarbage();
					Package->SetDirtyFlag(false);
					Transaction.Cancel();
					return FMCPToolResult::Error(
						FString::Printf(
							TEXT(
								"Data Asset creation was rolled back because the package could not be saved. objectPath='%s', filePath='%s', assetRegistryRegistered=%s, packageDirtyBeforeRollback=%s."),
							*ObjectPath,
							*PackageFilename,
							bAssetRegistryRegistered ? TEXT("true") : TEXT("false"),
							bPackageDirtyBeforeRollback ? TEXT("true") : TEXT("false")),
						TEXT("asset_save_failed"),
						500);
				}
			}

			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("asset_path"), SavePath);
			Result->SetStringField(TEXT("object_path"), ObjectPath);
			Result->SetStringField(TEXT("class_name"), ClassName);
			Result->SetStringField(TEXT("actual_class"), CreationClass->GetName());
			// Report the requested schema class rather than the internal concrete
			// persistence class used only for the abstract UDataAsset alias.
			Result->SetStringField(TEXT("class_path"), RequestedClass->GetPathName());
			Result->SetBoolField(TEXT("asset_registry_registered"), bAssetRegistryRegistered);
			Result->SetBoolField(TEXT("package_dirty"), Package->IsDirty());
			Result->SetBoolField(TEXT("save_attempted"), bSaveAttempted);
			Result->SetBoolField(TEXT("saved"), bSaved);
			Result->SetBoolField(TEXT("success"), true);
			return FMCPToolResult::Ok(Result);
		}
	};

	class FUnavailableCreateDataAsset final : public FMCPToolBase
	{
	public:
		explicit FUnavailableCreateDataAsset(const TCHAR* InId)
			: Id(InId)
		{
		}

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
		{
			return FMCPToolResult::Error(
				TEXT("Blueprint DataAsset creation is unavailable in this build."),
				TEXT("capability_unavailable"),
				503);
		}

		FString GetCapabilityId() const override
		{
			return Id;
		}

	private:
		FString Id;
	};
} // namespace

namespace UEAIIntegrationTools
{
	void RegisterBlueprintDataAssetTools(FMCPToolRegistry& Registry)
	{
#if WITH_EDITOR
		Registry.Register(MakeShared<FTool_CreateDataAsset>());
#else
		Registry.Register(MakeShared<FUnavailableCreateDataAsset>(
			TEXT("blueprint.data_asset.create")));
#endif
	}
} // namespace UEAIIntegrationTools
