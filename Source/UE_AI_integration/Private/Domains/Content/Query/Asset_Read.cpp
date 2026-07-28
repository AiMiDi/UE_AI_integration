// Generic Asset Registry and common asset inspection capabilities.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/EngineeringContractUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "PhysicsEngine/BodySetup.h"
#include "PixelFormat.h"
#include "StaticMeshResources.h"

namespace
{
using UEAIIntegration::Infrastructure::DigestJson;
using UEAIIntegration::Infrastructure::MakeFinding;
using UEAIIntegration::Infrastructure::MakeStableId;
using UEAIIntegration::Infrastructure::SetBoundedArray;

constexpr int32 DefaultPageLimit = 50;
constexpr int32 MaxPageLimit = 500;
constexpr int32 DefaultGraphLimit = 500;
constexpr int32 MaxGraphLimit = 5000;
constexpr int32 MaxGraphDepth = 8;
constexpr int32 MaxTagCount = 32;

int32 ReadInteger(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* Field,
	const int32 Default,
	const int32 Minimum,
	const int32 Maximum)
{
	double Number = Default;
	Params->TryGetNumberField(Field, Number);
	return FMath::Clamp(static_cast<int32>(Number), Minimum, Maximum);
}

int64 PackageDiskSize(const FString& PackageName)
{
	FString Filename;
	if (!FPackageName::DoesPackageExist(PackageName, &Filename))
	{
		return -1;
	}
	return IFileManager::Get().FileSize(*Filename);
}

TSharedRef<FJsonObject> SerializeAsset(
	const FAssetData& Asset,
	const bool bIncludeTags)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
	Result->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
	Result->SetStringField(TEXT("packagePath"), Asset.PackagePath.ToString());
	Result->SetStringField(TEXT("objectPath"), Asset.GetObjectPathString());
	Result->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
	Result->SetBoolField(TEXT("isRedirector"), Asset.IsRedirector());
	const int64 DiskSize = PackageDiskSize(Asset.PackageName.ToString());
	if (DiskSize >= 0)
	{
		Result->SetNumberField(TEXT("diskSizeBytes"), static_cast<double>(DiskSize));
	}

	if (bIncludeTags)
	{
		TArray<FString> TagNames;
		Asset.TagsAndValues.ForEach(
			[&TagNames](const auto& Pair)
			{
				TagNames.Add(Pair.Key.ToString());
			});
		TagNames.Sort();
		TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
		const int32 TagCount = FMath::Min(TagNames.Num(), MaxTagCount);
		for (int32 Index = 0; Index < TagCount; ++Index)
		{
			FString Value;
			if (Asset.GetTagValue(FName(*TagNames[Index]), Value))
			{
				Tags->SetStringField(TagNames[Index], Value.Left(1024));
			}
		}
		Result->SetObjectField(TEXT("tags"), Tags);
		Result->SetNumberField(TEXT("tagTotal"), TagNames.Num());
		Result->SetBoolField(TEXT("tagsTruncated"), TagNames.Num() > TagCount);
	}
	return Result;
}

bool ResolveAsset(
	IAssetRegistry& Registry,
	const FString& Path,
	FAssetData& OutAsset)
{
	if (Path.IsEmpty())
	{
		return false;
	}

	if (Path.Contains(TEXT(".")))
	{
		OutAsset = Registry.GetAssetByObjectPath(FSoftObjectPath(Path));
		if (OutAsset.IsValid())
		{
			return true;
		}
	}

	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(FName(*Path), Assets);
	if (!Assets.IsEmpty())
	{
		Assets.Sort(
			[](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.AssetName.LexicalLess(Right.AssetName);
			});
		OutAsset = Assets[0];
		return true;
	}
	return false;
}

bool MatchesAssetSearch(
	const FAssetData& Asset,
	const FString& Query,
	const FString& PathPrefix,
	const FString& ClassFilter)
{
	const FString Name = Asset.AssetName.ToString();
	const FString Package = Asset.PackageName.ToString();
	const FString ClassPath = Asset.AssetClassPath.ToString();
	return (Query.IsEmpty()
			|| Name.Contains(Query, ESearchCase::IgnoreCase)
			|| Package.Contains(Query, ESearchCase::IgnoreCase))
		&& (PathPrefix.IsEmpty()
			|| Package.StartsWith(PathPrefix, ESearchCase::IgnoreCase))
		&& (ClassFilter.IsEmpty()
			|| ClassPath.Contains(ClassFilter, ESearchCase::IgnoreCase));
}

class FTool_AssetSearch final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.search");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Query;
		FString PathPrefix;
		FString ClassFilter;
		Params->TryGetStringField(TEXT("query"), Query);
		Params->TryGetStringField(TEXT("pathPrefix"), PathPrefix);
		Params->TryGetStringField(TEXT("class"), ClassFilter);
		bool bIncludeTags = false;
		Params->TryGetBoolField(TEXT("includeTags"), bIncludeTags);
		const int32 Limit =
			ReadInteger(Params, TEXT("limit"), DefaultPageLimit, 1, MaxPageLimit);
		const int32 Offset =
			ReadInteger(Params, TEXT("offset"), 0, 0, 1000000);

		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAllAssets(Assets, true);
		Assets.Sort(
			[](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.PackageName == Right.PackageName
					? Left.AssetName.LexicalLess(Right.AssetName)
					: Left.PackageName.LexicalLess(Right.PackageName);
			});

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 Matched = 0;
		for (const FAssetData& Asset : Assets)
		{
			if (!MatchesAssetSearch(Asset, Query, PathPrefix, ClassFilter))
			{
				continue;
			}
			if (Matched >= Offset && Results.Num() < Limit)
			{
				Results.Add(
					MakeShared<FJsonValueObject>(
						SerializeAsset(Asset, bIncludeTags)));
			}
			++Matched;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.asset-search.v1"));
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetNumberField(TEXT("total"), Matched);
		Result->SetNumberField(TEXT("count"), Results.Num());
		Result->SetBoolField(TEXT("truncated"), Offset + Results.Num() < Matched);
		Result->SetArrayField(TEXT("assets"), Results);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_AssetGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.get");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("asset"), Path) || Path.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("asset is required."),
				TEXT("invalid_request"),
				400);
		}
		bool bIncludeTags = false;
		Params->TryGetBoolField(TEXT("includeTags"), bIncludeTags);
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FAssetData Asset;
		if (!ResolveAsset(Registry, Path, Asset))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Asset '%s' was not found."), *Path),
				TEXT("asset_not_found"),
				404);
		}
		TSharedRef<FJsonObject> Result = SerializeAsset(Asset, bIncludeTags);
		Result->SetStringField(TEXT("schema"), TEXT("ue.asset.v1"));
		Result->SetStringField(TEXT("snapshotDigest"), DigestJson(Result));
		return FMCPToolResult::Ok(Result);
	}
};

TSharedRef<FJsonObject> BuildRelationshipGraph(
	const TSharedPtr<FJsonObject>& Params,
	const bool bReferencers)
{
	FString Root;
	Params->TryGetStringField(TEXT("asset"), Root);
	const int32 MaxDepth =
		ReadInteger(Params, TEXT("maxDepth"), 2, 1, MaxGraphDepth);
	const int32 Limit =
		ReadInteger(Params, TEXT("limit"), DefaultGraphLimit, 1, MaxGraphLimit);

	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData RootAsset;
	FString RootPackage = Root;
	if (ResolveAsset(Registry, Root, RootAsset))
	{
		RootPackage = RootAsset.PackageName.ToString();
	}

	struct FQueuedPackage
	{
		FName Package;
		int32 Depth = 0;
	};
	TArray<FQueuedPackage> Queue = {{FName(*RootPackage), 0}};
	TSet<FName> Seen = {FName(*RootPackage)};
	TArray<TSharedPtr<FJsonValue>> Nodes;
	TArray<TSharedPtr<FJsonValue>> Edges;
	int32 QueueIndex = 0;
	bool bTruncated = false;

	while (QueueIndex < Queue.Num())
	{
		const FQueuedPackage Current = Queue[QueueIndex++];
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("package"), Current.Package.ToString());
		Node->SetNumberField(TEXT("depth"), Current.Depth);
		Nodes.Add(MakeShared<FJsonValueObject>(Node));
		if (Current.Depth >= MaxDepth)
		{
			continue;
		}

		TArray<FName> Related;
		if (bReferencers)
		{
			Registry.GetReferencers(
				Current.Package,
				Related,
				UE::AssetRegistry::EDependencyCategory::Package);
		}
		else
		{
			Registry.GetDependencies(
				Current.Package,
				Related,
				UE::AssetRegistry::EDependencyCategory::Package);
		}
		Related.Sort(FNameLexicalLess());
		for (const FName& Package : Related)
		{
			if (Edges.Num() >= Limit || Seen.Num() >= Limit)
			{
				bTruncated = true;
				break;
			}
			TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
			const FString From =
				bReferencers ? Package.ToString() : Current.Package.ToString();
			const FString To =
				bReferencers ? Current.Package.ToString() : Package.ToString();
			Edge->SetStringField(
				TEXT("edgeId"),
				MakeStableId(TEXT("assetedge"), {From, To}));
			Edge->SetStringField(TEXT("from"), From);
			Edge->SetStringField(TEXT("to"), To);
			Edges.Add(MakeShared<FJsonValueObject>(Edge));
			if (!Seen.Contains(Package))
			{
				Seen.Add(Package);
				Queue.Add({Package, Current.Depth + 1});
			}
		}
		if (bTruncated)
		{
			break;
		}
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(
		TEXT("schema"),
		bReferencers
			? TEXT("ue.asset-referencers.v1")
			: TEXT("ue.asset-dependencies.v1"));
	Result->SetStringField(TEXT("root"), RootPackage);
	Result->SetNumberField(TEXT("maxDepth"), MaxDepth);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetArrayField(TEXT("nodes"), Nodes);
	Result->SetArrayField(TEXT("edges"), Edges);
	Result->SetNumberField(TEXT("nodeCount"), Nodes.Num());
	Result->SetNumberField(TEXT("edgeCount"), Edges.Num());
	Result->SetBoolField(TEXT("truncated"), bTruncated || QueueIndex < Queue.Num());
	Result->SetStringField(TEXT("snapshotDigest"), DigestJson(Result));
	return Result;
}

class FTool_AssetDependencies final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.dependencies");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Ok(BuildRelationshipGraph(Params, false));
	}
};

class FTool_AssetReferencers final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.referencers");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Ok(BuildRelationshipGraph(Params, true));
	}
};

class FTool_AssetAudit final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.asset.audit");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString PathPrefix = TEXT("/Game");
		Params->TryGetStringField(TEXT("pathPrefix"), PathPrefix);
		const int32 Limit =
			ReadInteger(Params, TEXT("limit"), DefaultPageLimit, 1, MaxPageLimit);
		double MaxPackageBytesValue = 100.0 * 1024.0 * 1024.0;
		Params->TryGetNumberField(TEXT("maxPackageBytes"), MaxPackageBytesValue);
		const int64 MaxPackageBytes =
			FMath::Max<int64>(1, static_cast<int64>(MaxPackageBytesValue));

		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAllAssets(Assets, true);
		Assets.Sort(
			[](const FAssetData& Left, const FAssetData& Right)
			{
				return Left.PackageName.LexicalLess(Right.PackageName);
			});

		TArray<TSharedPtr<FJsonValue>> Findings;
		int32 AuditedAssets = 0;
		int32 TotalFindings = 0;
		for (const FAssetData& Asset : Assets)
		{
			const FString Package = Asset.PackageName.ToString();
			if (!Package.StartsWith(PathPrefix, ESearchCase::IgnoreCase))
			{
				continue;
			}
			++AuditedAssets;

			auto AddAuditFinding =
				[&](const FString& Rule, const FString& Severity, const FString& Message,
					const TSharedPtr<FJsonObject>& Evidence)
				{
					++TotalFindings;
					if (Findings.Num() < Limit)
					{
						Findings.Add(
							MakeShared<FJsonValueObject>(
								MakeFinding(
									Rule,
									Severity,
									1.0,
									Package,
									FString(),
									FString(),
									Message,
									Evidence)));
					}
				};

			if (Asset.IsRedirector())
			{
				AddAuditFinding(
					TEXT("content.asset.redirector"),
					TEXT("warning"),
					TEXT("Redirector remains in the audited content path."),
					MakeShared<FJsonObject>());
			}

			const int64 DiskSize = PackageDiskSize(Package);
			if (DiskSize > MaxPackageBytes)
			{
				TSharedRef<FJsonObject> Evidence = MakeShared<FJsonObject>();
				Evidence->SetNumberField(TEXT("diskSizeBytes"), static_cast<double>(DiskSize));
				Evidence->SetNumberField(
					TEXT("maxPackageBytes"),
					static_cast<double>(MaxPackageBytes));
				AddAuditFinding(
					TEXT("content.asset.package_size"),
					TEXT("warning"),
					TEXT("Asset package exceeds the configured size threshold."),
					Evidence);
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.asset-audit.v1"));
		Result->SetStringField(TEXT("pathPrefix"), PathPrefix);
		Result->SetNumberField(TEXT("auditedAssetCount"), AuditedAssets);
		SetBoundedArray(Result, TEXT("findings"), Findings, TotalFindings, Limit);
		Result->SetStringField(TEXT("snapshotDigest"), DigestJson(Result));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_StaticMeshInspect final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.static_mesh.inspect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FAssetData Asset;
		if (!ResolveAsset(Registry, Path, Asset))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Static Mesh '%s' was not found."), *Path),
				TEXT("asset_not_found"),
				404);
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.GetAsset());
		if (!Mesh)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Asset '%s' is not a Static Mesh."), *Path),
				TEXT("asset_type_mismatch"),
				400);
		}

		TSharedRef<FJsonObject> Result = SerializeAsset(Asset, false);
		Result->SetStringField(TEXT("schema"), TEXT("ue.static-mesh-inspection.v1"));
		Result->SetNumberField(TEXT("lodCount"), Mesh->GetNumLODs());
		Result->SetBoolField(TEXT("naniteEnabled"), Mesh->NaniteSettings.bEnabled);
		Result->SetNumberField(
			TEXT("lightMapCoordinateIndex"),
			Mesh->GetLightMapCoordinateIndex());
		Result->SetNumberField(
			TEXT("lightMapResolution"),
			Mesh->GetLightMapResolution());
		Result->SetNumberField(TEXT("materialSlotCount"), Mesh->GetStaticMaterials().Num());
		Result->SetNumberField(
			TEXT("collisionPrimitiveCount"),
			Mesh->GetBodySetup()
				? Mesh->GetBodySetup()->AggGeom.GetElementCount()
				: 0);

		const FBoxSphereBounds Bounds = Mesh->GetBounds();
		TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
		BoundsJson->SetNumberField(TEXT("extentX"), Bounds.BoxExtent.X);
		BoundsJson->SetNumberField(TEXT("extentY"), Bounds.BoxExtent.Y);
		BoundsJson->SetNumberField(TEXT("extentZ"), Bounds.BoxExtent.Z);
		BoundsJson->SetNumberField(TEXT("sphereRadius"), Bounds.SphereRadius);
		Result->SetObjectField(TEXT("bounds"), BoundsJson);

		TArray<TSharedPtr<FJsonValue>> Lods;
		if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			const int32 Count = FMath::Min(RenderData->LODResources.Num(), 16);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FStaticMeshLODResources& Lod = RenderData->LODResources[Index];
				TSharedRef<FJsonObject> LodJson = MakeShared<FJsonObject>();
				LodJson->SetNumberField(TEXT("index"), Index);
				LodJson->SetNumberField(TEXT("vertices"), Lod.GetNumVertices());
				LodJson->SetNumberField(TEXT("sections"), Lod.Sections.Num());
				LodJson->SetNumberField(
					TEXT("uvChannels"),
					Lod.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());
				Lods.Add(MakeShared<FJsonValueObject>(LodJson));
			}
			Result->SetBoolField(
				TEXT("lodsTruncated"),
				RenderData->LODResources.Num() > Count);
		}
		Result->SetArrayField(TEXT("lods"), Lods);
		Result->SetNumberField(
			TEXT("estimatedResourceBytes"),
			static_cast<double>(
				Mesh->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)));
		Result->SetStringField(TEXT("snapshotDigest"), DigestJson(Result));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_TextureInspect final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.texture.inspect");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Path;
		Params->TryGetStringField(TEXT("asset"), Path);
		IAssetRegistry& Registry =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FAssetData Asset;
		if (!ResolveAsset(Registry, Path, Asset))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Texture '%s' was not found."), *Path),
				TEXT("asset_not_found"),
				404);
		}
		UTexture2D* Texture = Cast<UTexture2D>(Asset.GetAsset());
		if (!Texture)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Asset '%s' is not a Texture2D."), *Path),
				TEXT("asset_type_mismatch"),
				400);
		}

		TSharedRef<FJsonObject> Result = SerializeAsset(Asset, false);
		Result->SetStringField(TEXT("schema"), TEXT("ue.texture-inspection.v1"));
		Result->SetNumberField(TEXT("sizeX"), Texture->GetSizeX());
		Result->SetNumberField(TEXT("sizeY"), Texture->GetSizeY());
		Result->SetNumberField(TEXT("sourceSizeX"), Texture->Source.GetSizeX());
		Result->SetNumberField(TEXT("sourceSizeY"), Texture->Source.GetSizeY());
		Result->SetNumberField(TEXT("mipCount"), Texture->GetNumMips());
		const EPixelFormat PixelFormat = Texture->GetPixelFormat();
		Result->SetStringField(
			TEXT("pixelFormat"),
			PixelFormat >= 0 && PixelFormat < PF_MAX
				? GPixelFormats[PixelFormat].Name
				: TEXT("Unknown"));
		const UEnum* CompressionEnum =
			StaticEnum<TextureCompressionSettings>();
		Result->SetStringField(
			TEXT("compression"),
			CompressionEnum
				? CompressionEnum->GetNameStringByValue(Texture->CompressionSettings)
				: FString::FromInt(Texture->CompressionSettings));
		Result->SetBoolField(TEXT("sRGB"), Texture->SRGB);
		Result->SetBoolField(
			TEXT("virtualTextureStreaming"),
			Texture->VirtualTextureStreaming);
		Result->SetNumberField(
			TEXT("estimatedResourceBytes"),
			static_cast<double>(
				Texture->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal)));
		Result->SetStringField(TEXT("snapshotDigest"), DigestJson(Result));
		return FMCPToolResult::Ok(Result);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterContentAssetReadTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FTool_AssetSearch>());
	Registry.Register(MakeShared<FTool_AssetGet>());
	Registry.Register(MakeShared<FTool_AssetDependencies>());
	Registry.Register(MakeShared<FTool_AssetReferencers>());
	Registry.Register(MakeShared<FTool_AssetAudit>());
	Registry.Register(MakeShared<FTool_StaticMeshInspect>());
	Registry.Register(MakeShared<FTool_TextureInspect>());
}
}
