#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Infrastructure/EngineeringContractUtils.h"
#include "RHIGlobals.h"
#include "RHIStats.h"
#include "RHIStrings.h"
#include "RenderUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"

namespace UEAISceneEngineeringQueryPrivate
{
UWorld* GetEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FMCPToolResult Unavailable(const FString& Feature, const FString& Reason)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("feature"), Feature);
	Data->SetBoolField(TEXT("available"), false);
	Data->SetStringField(TEXT("reason"), Reason);
	return FMCPToolResult::Ok(Data);
}

TArray<TSharedPtr<FJsonValue>> VectorJson(const FVector& Value)
{
	return {
		MakeShared<FJsonValueNumber>(Value.X),
		MakeShared<FJsonValueNumber>(Value.Y),
		MakeShared<FJsonValueNumber>(Value.Z)
	};
}

FString CellStateName(const EWorldPartitionRuntimeCellState State)
{
	switch (State)
	{
	case EWorldPartitionRuntimeCellState::Unloaded: return TEXT("unloaded");
	case EWorldPartitionRuntimeCellState::Loaded: return TEXT("loaded");
	case EWorldPartitionRuntimeCellState::Activated: return TEXT("activated");
	default: return TEXT("unknown");
	}
}

FString DataLayerStateName(const EDataLayerRuntimeState State)
{
	switch (State)
	{
	case EDataLayerRuntimeState::Unloaded: return TEXT("unloaded");
	case EDataLayerRuntimeState::Loaded: return TEXT("loaded");
	case EDataLayerRuntimeState::Activated: return TEXT("activated");
	default: return TEXT("unknown");
	}
}

UWorldPartitionRuntimeHash* GetRuntimeHash(UWorldPartition* WorldPartition)
{
	if (!WorldPartition)
	{
		return nullptr;
	}
	const FObjectProperty* Property =
		FindFProperty<FObjectProperty>(UWorldPartition::StaticClass(), TEXT("RuntimeHash"));
	return Property
		? Cast<UWorldPartitionRuntimeHash>(
			Property->GetObjectPropertyValue_InContainer(WorldPartition))
		: nullptr;
}

TSharedRef<FJsonObject> DescribeWorldPartition(UWorld* World)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("available"), World != nullptr);
	if (!World)
	{
		Result->SetStringField(TEXT("reason"), TEXT("No editor world is loaded."));
		return Result;
	}

	UWorldPartition* WorldPartition = World->GetWorldPartition();
	Result->SetStringField(TEXT("world"), World->GetPathName());
	Result->SetBoolField(TEXT("partitioned"), WorldPartition != nullptr);
	if (!WorldPartition)
	{
		Result->SetStringField(TEXT("reason"), TEXT("The current world is not partitioned."));
		return Result;
	}

	Result->SetStringField(TEXT("worldPartition"), WorldPartition->GetPathName());
	Result->SetBoolField(TEXT("initialized"), WorldPartition->IsInitialized());
	Result->SetBoolField(TEXT("supportsStreaming"), WorldPartition->SupportsStreaming());
	Result->SetBoolField(TEXT("streamingEnabled"), WorldPartition->IsStreamingEnabled());
	Result->SetBoolField(
		TEXT("streamingCompleted"),
		WorldPartition->IsStreamingCompleted(nullptr));
	Result->SetNumberField(
		TEXT("streamingSourceCount"),
		WorldPartition->GetStreamingSources().Num());
	Result->SetStringField(
		TEXT("runtimeHashClass"),
		GetRuntimeHash(WorldPartition)
			? GetRuntimeHash(WorldPartition)->GetClass()->GetPathName()
			: TEXT(""));
	return Result;
}

TSharedRef<FJsonObject> DescribeDataLayer(
	const UDataLayerManager* Manager,
	const UDataLayerInstance* Instance)
{
	TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("name"), Instance->GetDataLayerShortName());
	Item->SetStringField(TEXT("fullName"), Instance->GetDataLayerFullName());
	Item->SetStringField(TEXT("instanceName"), Instance->GetDataLayerFName().ToString());
	Item->SetStringField(TEXT("path"), Instance->GetPathName());
	Item->SetBoolField(TEXT("runtime"), Instance->IsRuntime());
	Item->SetBoolField(TEXT("visible"), Instance->IsVisible());
	Item->SetBoolField(TEXT("effectiveVisible"), Instance->IsEffectiveVisible());
	Item->SetStringField(
		TEXT("initialRuntimeState"),
		DataLayerStateName(Instance->GetInitialRuntimeState()));
	if (Manager && Instance->IsRuntime())
	{
		Item->SetStringField(
			TEXT("runtimeState"),
			DataLayerStateName(Manager->GetDataLayerInstanceRuntimeState(Instance)));
		Item->SetStringField(
			TEXT("effectiveRuntimeState"),
			DataLayerStateName(
				Manager->GetDataLayerInstanceEffectiveRuntimeState(Instance)));
	}
	if (const UDataLayerInstance* Parent = Instance->GetParent())
	{
		Item->SetStringField(TEXT("parent"), Parent->GetDataLayerShortName());
	}
	Item->SetNumberField(TEXT("childCount"), Instance->GetChildren().Num());
	return Item;
}

const UDataLayerInstance* FindDataLayer(
	const UDataLayerManager* Manager,
	const FString& Identifier)
{
	if (!Manager)
	{
		return nullptr;
	}
	const UDataLayerInstance* Match = nullptr;
	Manager->ForEachDataLayerInstance(
		[&](UDataLayerInstance* Instance)
		{
			if (Instance
				&& (Instance->GetDataLayerShortName().Equals(
						Identifier,
						ESearchCase::IgnoreCase)
					|| Instance->GetDataLayerFullName().Equals(
						Identifier,
						ESearchCase::IgnoreCase)
					|| Instance->GetDataLayerFName().ToString().Equals(
						Identifier,
						ESearchCase::IgnoreCase)
					|| Instance->GetPathName().Equals(
						Identifier,
						ESearchCase::IgnoreCase)))
			{
				Match = Instance;
				return false;
			}
			return true;
		});
	return Match;
}

TSharedRef<FJsonObject> CVarAudit(
	const FString& Feature,
	const TCHAR* CVarName,
	const bool bHardwareSupported,
	const FString& HardwareReason = FString())
{
	TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
	Item->SetStringField(TEXT("feature"), Feature);
	Item->SetStringField(TEXT("cvar"), CVarName);
	const IConsoleVariable* CVar =
		IConsoleManager::Get().FindConsoleVariable(CVarName);
	Item->SetBoolField(TEXT("cvarAvailable"), CVar != nullptr);
	Item->SetBoolField(TEXT("hardwareSupported"), bHardwareSupported);
	if (!HardwareReason.IsEmpty())
	{
		Item->SetStringField(TEXT("hardwareReason"), HardwareReason);
	}
	if (CVar)
	{
		Item->SetStringField(TEXT("effectiveValue"), CVar->GetString());
		Item->SetBoolField(
			TEXT("enabled"),
			FCString::Atoi(*CVar->GetString()) != 0);
	}
	Item->SetStringField(
		TEXT("status"),
		!bHardwareSupported
			? TEXT("unsupported")
			: (CVar ? TEXT("observed") : TEXT("unavailable")));
	return Item;
}

class FTool_WorldPartitionGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		return FMCPToolResult::Ok(DescribeWorldPartition(GetEditorWorld()));
	}
};

class FTool_WorldPartitionCellsList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.cells.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UWorld* World = GetEditorWorld();
		UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
		UWorldPartitionRuntimeHash* RuntimeHash = GetRuntimeHash(WorldPartition);
		if (!RuntimeHash)
		{
			return Unavailable(
				TEXT("worldPartitionCells"),
				TEXT("The current world has no generated runtime hash."));
		}

		double LimitValue = 100.0;
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
		const int32 Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 500);
		TArray<TSharedPtr<FJsonValue>> Cells;
		int32 Total = 0;
		RuntimeHash->ForEachStreamingCells(
			[&](const UWorldPartitionRuntimeCell* Cell)
			{
				if (!Cell)
				{
					return true;
				}
				++Total;
				if (Cells.Num() >= Limit)
				{
					return true;
				}
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Cell->GetDebugName());
				Item->SetStringField(TEXT("state"), CellStateName(Cell->GetCurrentState()));
				Item->SetBoolField(TEXT("isHLOD"), Cell->GetIsHLOD());
				const FBox Bounds = Cell->GetCellBounds();
				Item->SetArrayField(TEXT("boundsMin"), VectorJson(Bounds.Min));
				Item->SetArrayField(TEXT("boundsMax"), VectorJson(Bounds.Max));
				TArray<TSharedPtr<FJsonValue>> DataLayers;
				for (const FName Name : Cell->GetDataLayers())
				{
					DataLayers.Add(MakeShared<FJsonValueString>(Name.ToString()));
				}
				Item->SetArrayField(TEXT("dataLayers"), DataLayers);
				Cells.Add(MakeShared<FJsonValueObject>(Item));
				return true;
			});

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), true);
		Result->SetArrayField(TEXT("cells"), Cells);
		Result->SetNumberField(TEXT("total"), Total);
		Result->SetNumberField(TEXT("returned"), Cells.Num());
		Result->SetBoolField(TEXT("truncated"), Cells.Num() < Total);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_StreamingSourcesList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.streaming_sources.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		UWorld* World = GetEditorWorld();
		UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
		if (!WorldPartition)
		{
			return Unavailable(
				TEXT("worldPartitionStreamingSources"),
				TEXT("The current world is not partitioned."));
		}
		TArray<TSharedPtr<FJsonValue>> Sources;
		for (const FWorldPartitionStreamingSource& Source :
			WorldPartition->GetStreamingSources())
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Source.Name.ToString());
			Item->SetArrayField(TEXT("location"), VectorJson(Source.Location));
			Item->SetStringField(
				TEXT("targetState"),
				Source.TargetState == EStreamingSourceTargetState::Activated
					? TEXT("activated")
					: TEXT("loaded"));
			Item->SetBoolField(TEXT("blockOnSlowLoading"), Source.bBlockOnSlowLoading);
			Item->SetBoolField(TEXT("remote"), Source.bRemote);
			Item->SetNumberField(TEXT("velocity"), Source.Velocity);
			Sources.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), true);
		Result->SetArrayField(TEXT("sources"), Sources);
		Result->SetNumberField(TEXT("count"), Sources.Num());
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_StreamingAudit final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.world_partition.streaming.audit"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		TSharedRef<FJsonObject> Result = DescribeWorldPartition(GetEditorWorld());
		bool bPartitioned = false;
		if (Result->TryGetBoolField(TEXT("partitioned"), bPartitioned)
			&& bPartitioned)
		{
			TArray<TSharedPtr<FJsonValue>> Findings;
			UWorldPartition* WorldPartition = GetEditorWorld()->GetWorldPartition();
			if (WorldPartition->SupportsStreaming()
				&& !WorldPartition->IsStreamingEnabled())
			{
				Findings.Add(MakeShared<FJsonValueObject>(
					UEAIIntegration::Infrastructure::MakeFinding(
						TEXT("wp.streaming.disabled"),
						TEXT("medium"),
						0.95,
						GetEditorWorld()->GetPathName(),
						TEXT(""),
						TEXT(""),
						TEXT("World Partition supports streaming but streaming is disabled."))));
			}
			if (WorldPartition->IsStreamingEnabled()
				&& WorldPartition->GetStreamingSources().IsEmpty())
			{
				Findings.Add(MakeShared<FJsonValueObject>(
					UEAIIntegration::Infrastructure::MakeFinding(
						TEXT("wp.streaming.no_sources"),
						TEXT("info"),
						0.8,
						GetEditorWorld()->GetPathName(),
						TEXT(""),
						TEXT(""),
						TEXT("No active streaming source is currently observed."))));
			}
			Result->SetArrayField(TEXT("findings"), Findings);
			Result->SetNumberField(TEXT("findingCount"), Findings.Num());
		}
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_DataLayerList final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.data_layer.list"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		UWorld* World = GetEditorWorld();
		UDataLayerManager* Manager = World ? World->GetDataLayerManager() : nullptr;
		if (!Manager)
		{
			return Unavailable(
				TEXT("dataLayers"),
				TEXT("The current world has no Data Layer manager."));
		}
		TArray<TSharedPtr<FJsonValue>> Items;
		Manager->ForEachDataLayerInstance(
			[&](UDataLayerInstance* Instance)
			{
				if (Instance)
				{
					Items.Add(MakeShared<FJsonValueObject>(
						DescribeDataLayer(Manager, Instance)));
				}
				return true;
			});
		Items.Sort(
			[](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("fullName"))
					< B->AsObject()->GetStringField(TEXT("fullName"));
			});
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), true);
		Result->SetArrayField(TEXT("dataLayers"), Items);
		Result->SetNumberField(TEXT("count"), Items.Num());
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_DataLayerGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.data_layer.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Identifier;
		if (!Params->TryGetStringField(TEXT("dataLayer"), Identifier)
			|| Identifier.IsEmpty())
		{
			return FMCPToolResult::Error(
				TEXT("dataLayer is required."),
				TEXT("invalid_request"),
				400);
		}
		UWorld* World = GetEditorWorld();
		UDataLayerManager* Manager = World ? World->GetDataLayerManager() : nullptr;
		const UDataLayerInstance* Instance = FindDataLayer(Manager, Identifier);
		if (!Instance)
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Data Layer '%s' was not found."), *Identifier),
				TEXT("data_layer_not_found"),
				404);
		}
		return FMCPToolResult::Ok(DescribeDataLayer(Manager, Instance));
	}
};

class FTool_HLODAudit final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.hlod.audit"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			return Unavailable(TEXT("hlod"), TEXT("No editor world is loaded."));
		}
		int32 ActorCount = 0;
		int32 HiddenCount = 0;
		TArray<TSharedPtr<FJsonValue>> Actors;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !Actor->GetClass()->GetName().Contains(TEXT("WorldPartitionHLOD")))
			{
				continue;
			}
			++ActorCount;
			HiddenCount += Actor->IsHiddenEd() ? 1 : 0;
			if (Actors.Num() < 100)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Actor->GetActorLabel());
				Item->SetStringField(TEXT("path"), Actor->GetPathName());
				Item->SetBoolField(TEXT("hiddenInEditor"), Actor->IsHiddenEd());
				Actors.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), true);
		Result->SetBoolField(TEXT("partitioned"), World->IsPartitionedWorld());
		Result->SetNumberField(TEXT("hlodActorCount"), ActorCount);
		Result->SetNumberField(TEXT("hiddenActorCount"), HiddenCount);
		Result->SetArrayField(TEXT("actors"), Actors);
		Result->SetBoolField(TEXT("truncated"), ActorCount > Actors.Num());
		Result->SetBoolField(TEXT("buildRequired"), World->IsPartitionedWorld() && ActorCount == 0);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_PCGInspect final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.pcg.inspect"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		UWorld* World = GetEditorWorld();
		UClass* ComponentClass =
			FindFirstObject<UClass>(TEXT("PCGComponent"), EFindFirstObjectOptions::None);
		if (!ComponentClass)
		{
			return Unavailable(
				TEXT("pcg"),
				TEXT("The optional PCG plugin/module is not loaded."));
		}
		TArray<TSharedPtr<FJsonValue>> Components;
		for (TObjectIterator<UActorComponent> It; It; ++It)
		{
			UActorComponent* Component = *It;
			if (!Component || Component->GetWorld() != World
				|| !Component->IsA(ComponentClass))
			{
				continue;
			}
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("path"), Component->GetPathName());
			Item->SetStringField(
				TEXT("owner"),
				Component->GetOwner() ? Component->GetOwner()->GetPathName() : TEXT(""));
			Components.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), true);
		Result->SetStringField(TEXT("componentClass"), ComponentClass->GetPathName());
		Result->SetArrayField(TEXT("components"), Components);
		Result->SetNumberField(TEXT("componentCount"), Components.Num());
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_RenderContextGet final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.render.context.get"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		UWorld* World = GetEditorWorld();
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), GDynamicRHI != nullptr);
		Result->SetStringField(
			TEXT("rhi"),
			GDynamicRHI ? GDynamicRHI->GetName() : TEXT(""));
		Result->SetStringField(TEXT("adapter"), GRHIAdapterName);
		Result->SetNumberField(TEXT("vendorId"), GRHIVendorId);
		Result->SetNumberField(TEXT("deviceId"), GRHIDeviceId);
		Result->SetStringField(
			TEXT("featureLevel"),
			LexToString(World ? World->GetFeatureLevel() : GMaxRHIFeatureLevel));
		Result->SetStringField(
			TEXT("world"),
			World ? World->GetPathName() : TEXT(""));
		Result->SetBoolField(TEXT("rayTracingHardware"), GRHISupportsRayTracing);
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_RenderFeatureAudit final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.render.feature.audit"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		TArray<TSharedPtr<FJsonValue>> Features;
		Features.Add(MakeShared<FJsonValueObject>(
			CVarAudit(TEXT("lumen"), TEXT("r.Lumen.DiffuseIndirect.Allow"), true)));
		const bool bNaniteRuntimeSupported =
			GDynamicRHI
			&& DoesRuntimeSupportNanite(
				GMaxRHIShaderPlatform,
				true,
				false);
		TSharedRef<FJsonObject> Nanite = CVarAudit(
			TEXT("nanite"),
			TEXT("r.Nanite.ProjectEnabled"),
			bNaniteRuntimeSupported,
			bNaniteRuntimeSupported
				? FString()
				: TEXT(
					"The active shader platform does not satisfy Nanite runtime requirements."));
		const IConsoleVariable* NaniteProjectCVar =
			IConsoleManager::Get().FindConsoleVariable(
				TEXT("r.Nanite.ProjectEnabled"));
		Nanite->SetBoolField(
			TEXT("projectEnabled"),
			NaniteProjectCVar
			&& FCString::Atoi(*NaniteProjectCVar->GetString()) != 0);
		Nanite->SetBoolField(
			TEXT("runtimeSupported"),
			bNaniteRuntimeSupported);
		Nanite->SetBoolField(
			TEXT("enabled"),
			GDynamicRHI
			&& UseNanite(GMaxRHIShaderPlatform));
		Features.Add(MakeShared<FJsonValueObject>(Nanite));
		Features.Add(MakeShared<FJsonValueObject>(
			CVarAudit(TEXT("virtualShadowMaps"), TEXT("r.Shadow.Virtual.Enable"), GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5)));
		Features.Add(MakeShared<FJsonValueObject>(
			CVarAudit(
				TEXT("hardwareRayTracing"),
				TEXT("r.RayTracing"),
				GRHISupportsRayTracing,
				GRHISupportsRayTracing ? FString() : TEXT("The active RHI reports no ray tracing support."))));
		Features.Add(MakeShared<FJsonValueObject>(
			CVarAudit(TEXT("tsr"), TEXT("r.TemporalAA.Upsampling"), GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5)));
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), GDynamicRHI != nullptr);
		Result->SetArrayField(TEXT("features"), Features);
		Result->SetStringField(
			TEXT("evidenceBoundary"),
			TEXT("Values report current CVars and RHI support; they do not prove an active view rendered a feature."));
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_RenderMemorySample final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("scene.render.memory.sample"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{
		if (!GDynamicRHI)
		{
			return Unavailable(TEXT("renderMemory"), TEXT("No dynamic RHI is active."));
		}
		FTextureMemoryStats Stats;
		RHIGetTextureMemoryStats(Stats);
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("available"), true);
		Result->SetBoolField(TEXT("hardwareStatsValid"), Stats.AreHardwareStatsValid());
		Result->SetNumberField(TEXT("dedicatedVideoMemoryBytes"), Stats.DedicatedVideoMemory);
		Result->SetNumberField(TEXT("totalGraphicsMemoryBytes"), Stats.TotalGraphicsMemory);
		Result->SetNumberField(TEXT("streamingMemoryBytes"), static_cast<double>(Stats.StreamingMemorySize));
		Result->SetNumberField(TEXT("nonStreamingMemoryBytes"), static_cast<double>(Stats.NonStreamingMemorySize));
		Result->SetNumberField(TEXT("texturePoolBytes"), Stats.TexturePoolSize);
		Result->SetNumberField(TEXT("texturePoolAvailableBytes"), Stats.ComputeAvailableMemorySize());
		Result->SetStringField(
			TEXT("sampleKind"),
			TEXT("instantaneous"));
		return FMCPToolResult::Ok(Result);
	}
};
}

namespace UEAIIntegrationTools
{
void RegisterSceneEngineeringQueryTools(FMCPToolRegistry& Registry)
{
	using namespace UEAISceneEngineeringQueryPrivate;
	Registry.Register(MakeShared<FTool_WorldPartitionGet>());
	Registry.Register(MakeShared<FTool_WorldPartitionCellsList>());
	Registry.Register(MakeShared<FTool_StreamingSourcesList>());
	Registry.Register(MakeShared<FTool_StreamingAudit>());
	Registry.Register(MakeShared<FTool_DataLayerList>());
	Registry.Register(MakeShared<FTool_DataLayerGet>());
	Registry.Register(MakeShared<FTool_HLODAudit>());
	Registry.Register(MakeShared<FTool_PCGInspect>());
	Registry.Register(MakeShared<FTool_RenderContextGet>());
	Registry.Register(MakeShared<FTool_RenderFeatureAudit>());
	Registry.Register(MakeShared<FTool_RenderMemorySample>());
}
}
