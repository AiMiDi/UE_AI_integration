#pragma once

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

namespace MCPMaterialInfrastructure
{
	template <typename AssetType>
	AssetType* LoadAssetByName(
		const FString& Name,
		const TCHAR* AssetLabel,
		FString& OutError)
	{
		// Exact paths should not enumerate every material/function in the project.
		if (Name.StartsWith(TEXT("/")))
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(Name);
			const FString ObjectPath = Name.Contains(TEXT(".")) ? Name
				: PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			if (AssetType* Asset = LoadObject<AssetType>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn)) return Asset;
			OutError = FString::Printf(TEXT("%s '%s' not found."), AssetLabel, *Name);
			return nullptr;
		}
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(
			AssetType::StaticClass()->GetClassPathName(),
			Assets,
			false);

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString() == Name
				|| Asset.PackageName.ToString() == Name
				|| Asset.GetObjectPathString() == Name)
			{
				if (AssetType* Loaded = Cast<AssetType>(Asset.GetAsset()))
				{
					return Loaded;
				}
			}
		}

		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase)
				|| Asset.PackageName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				if (AssetType* Loaded = Cast<AssetType>(Asset.GetAsset()))
				{
					return Loaded;
				}
			}
		}

		OutError =
			FString::Printf(TEXT("%s '%s' not found."), AssetLabel, *Name);
		return nullptr;
	}

	inline UMaterial* LoadMaterialByName(const FString& Name, FString& OutError)
	{
		return LoadAssetByName<UMaterial>(Name, TEXT("Material"), OutError);
	}

	inline UMaterialFunction* LoadMaterialFunctionByName(
		const FString& Name,
		FString& OutError)
	{
		return LoadAssetByName<UMaterialFunction>(
			Name,
			TEXT("MaterialFunction"),
			OutError);
	}

	inline UMaterialInstanceConstant* LoadMaterialInstanceByName(
		const FString& Name,
		FString& OutError)
	{
		return LoadAssetByName<UMaterialInstanceConstant>(
			Name,
			TEXT("MaterialInstance"),
			OutError);
	}

	inline UMaterialInterface* LoadMaterialInterfaceByName(
		const FString& Name,
		FString& OutError)
	{
		if (Name.StartsWith(TEXT("/")))
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(Name);
			const FString ObjectPath = Name.Contains(TEXT("."))
				? Name
				: PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
			if (UMaterialInterface* Asset = LoadObject<UMaterialInterface>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn))
			{
				return Asset;
			}
		}

		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		for (const UClass* AssetClass : { UMaterial::StaticClass(), UMaterialInstanceConstant::StaticClass() })
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByClass(AssetClass->GetClassPathName(), Assets, false);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetName.ToString().Equals(Name, ESearchCase::IgnoreCase)
					|| Asset.PackageName.ToString().Equals(Name, ESearchCase::IgnoreCase)
					|| Asset.GetObjectPathString().Equals(Name, ESearchCase::IgnoreCase))
				{
					if (UMaterialInterface* Loaded = Cast<UMaterialInterface>(Asset.GetAsset()))
					{
						return Loaded;
					}
				}
			}
		}

		OutError = FString::Printf(TEXT("MaterialInterface '%s' not found."), *Name);
		return nullptr;
	}

	inline void EnsureMaterialGraph(UMaterial* Material)
	{
		if (!Material || Material->MaterialGraph)
		{
			return;
		}

		// Reconstructing the transient editor projection is a read operation.
		// RebuildGraph calls Modify(), but must not make a clean loaded asset
		// ineligible for the next Workflow plan (or clear existing dirty edits).
		UPackage* Package = Material->GetOutermost();
		const bool bWasDirty = Package->IsDirty();
		Material->MaterialGraph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(
				Material,
				NAME_None,
				UMaterialGraph::StaticClass(),
				UMaterialGraphSchema::StaticClass()));
		Material->MaterialGraph->Material = Material;
		Material->MaterialGraph->RebuildGraph();
		Package->SetDirtyFlag(bWasDirty);
	}

	inline bool SaveMaterialPackage(UObject* Asset)
	{
		if (!Asset)
		{
			return false;
		}

		UPackage* Package = Asset->GetPackage();
		if (!Package)
		{
			return false;
		}

		const FString PackageFilename = FPackageName::LongPackageNameToFilename(
			Package->GetName(),
			FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
	}
}
