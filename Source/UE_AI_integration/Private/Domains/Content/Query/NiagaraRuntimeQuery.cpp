#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif
#if WITH_UEAI_NIAGARA
#include "NiagaraComponent.h"
#include "NiagaraDataInterfaceAsyncGpuTrace.h"
#include "NiagaraSystem.h"
#endif

namespace UEAINiagaraRuntimePrivate
{
UWorld* FindWorld(const FString& Path)
{
	if (!GEngine) return nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && World->GetPathName() == Path
			&& (World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::PIE)) return World;
	}
	return nullptr;
}

class FTool_NiagaraRuntimeInspect final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.runtime.inspect"); }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
#if WITH_UEAI_NIAGARA
		FString WorldPath;
		Params->TryGetStringField(TEXT("world"), WorldPath);
		UWorld* SelectedWorld = WorldPath.IsEmpty() ? nullptr : FindWorld(WorldPath);
		if (!WorldPath.IsEmpty() && !SelectedWorld) return FMCPToolResult::Error(TEXT("Select a loaded Editor or PIE world returned by runtime.inspect."), TEXT("world_not_found"), 404);
		const int32 Offset = Params->HasField(TEXT("offset")) ? FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("offset"))), 0, 100000) : 0;
		const int32 Limit = Params->HasField(TEXT("limit")) ? FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("limit"))), 1, 128) : 32;
		TArray<TSharedPtr<FJsonValue>> Worlds;
		if (GEngine) for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || (World->WorldType != EWorldType::Editor && World->WorldType != EWorldType::PIE)) continue;
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), World->GetPathName());
			Row->SetStringField(TEXT("type"), World->WorldType == EWorldType::PIE ? TEXT("pie") : TEXT("editor"));
			Row->SetBoolField(TEXT("sceneAvailable"), World->Scene != nullptr);
			Worlds.Add(MakeShared<FJsonValueObject>(Row));
		}
		TArray<UNiagaraComponent*> Components;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			if (!IsValid(*It) || It->IsTemplate() || !It->GetWorld() || !FindWorld(It->GetWorld()->GetPathName())) continue;
			if (SelectedWorld && It->GetWorld() != SelectedWorld) continue;
			Components.Add(*It);
		}
		Components.Sort([](const auto& A, const auto& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> ComponentRows;
		for (int32 Index = Offset; Index < FMath::Min(Components.Num(), Offset + Limit); ++Index)
		{
			UNiagaraComponent* Component = Components[Index];
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), Component->GetPathName());
			Row->SetStringField(TEXT("world"), Component->GetWorld()->GetPathName());
			Row->SetStringField(TEXT("system"), Component->GetAsset() ? Component->GetAsset()->GetPathName() : TEXT(""));
			Row->SetBoolField(TEXT("active"), Component->IsActive());
			Row->SetBoolField(TEXT("paused"), Component->IsPaused());
			TArray<TSharedPtr<FJsonValue>> Emitters;
			if (const UNiagaraSystem* System = Component->GetAsset())
			{
				for (const auto& Handle : System->GetEmitterHandles())
				{
					if (Emitters.Num() == 64) break;
					auto Emitter = MakeShared<FJsonObject>();
					Emitter->SetStringField(TEXT("name"), Handle.GetName().ToString());
					Emitter->SetStringField(TEXT("captureAttributePrefix"), Handle.GetUniqueInstanceName() + TEXT(".Particles."));
					Emitters.Add(MakeShared<FJsonValueObject>(Emitter));
				}
				Row->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
			}
			Row->SetArrayField(TEXT("emitters"), Emitters);
			ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		TArray<UNiagaraDataInterfaceAsyncGpuTrace*> Interfaces;
		for (TObjectIterator<UNiagaraDataInterfaceAsyncGpuTrace> It; It; ++It)
		{
			if (!IsValid(*It) || It->HasAnyFlags(RF_ClassDefaultObject) || !It->GetProxy()) continue;
			const UNiagaraComponent* Owner = It->GetTypedOuter<UNiagaraComponent>();
			if (Owner && SelectedWorld && Owner->GetWorld() != SelectedWorld) continue;
			Interfaces.Add(*It);
		}
		Interfaces.Sort([](const auto& A, const auto& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> InterfaceRows;
		for (int32 Index = Offset; Index < FMath::Min(Interfaces.Num(), Offset + Limit); ++Index)
		{
			const auto* DI = Interfaces[Index];
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("path"), DI->GetPathName());
			Row->SetStringField(TEXT("outer"), DI->GetOuter() ? DI->GetOuter()->GetPathName() : TEXT(""));
			Row->SetNumberField(TEXT("configuredProviderValue"), static_cast<int32>(DI->TraceProvider.GetValue()));
			Row->SetStringField(TEXT("bindingEvidence"), TEXT("loadedObjectOnly"));
			InterfaceRows.Add(MakeShared<FJsonValueObject>(Row));
		}
		auto Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara-runtime-inventory.v1"));
		Result->SetArrayField(TEXT("worlds"), Worlds);
		Result->SetArrayField(TEXT("components"), ComponentRows);
		Result->SetArrayField(TEXT("dataInterfaces"), InterfaceRows);
		Result->SetNumberField(TEXT("componentCount"), Components.Num());
		Result->SetNumberField(TEXT("dataInterfaceCount"), Interfaces.Num());
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(TEXT("limit"), Limit);
		Result->SetBoolField(TEXT("hasMore"), Offset + Limit < FMath::Max(Components.Num(), Interfaces.Num()));
		Result->SetStringField(TEXT("scope"), TEXT("Loaded objects and configured values only; asset DIs may be shared across components and worlds. This inventory does not establish runtime binding, selected GPU provider, dispatch execution or collision results."));
		return FMCPToolResult::Ok(Result);
#else
		return FMCPToolResult::Error(TEXT("Niagara support is not enabled in this plugin build."),
			TEXT("niagara_unavailable"), 503);
#endif
	}
};

}

namespace UEAIIntegrationTools
{
void RegisterNiagaraRuntimeTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<UEAINiagaraRuntimePrivate::FTool_NiagaraRuntimeInspect>());
}
}
