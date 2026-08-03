// Niagara Particle System tools for UE_AI_integration
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA
#include "NiagaraSystem.h"
#include "NiagaraActor.h"
#include "NiagaraComponent.h"
#include "NiagaraEmitter.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraEditorModule.h"
#include "NiagaraSystemFactoryNew.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// ─────────────────────────────────────────────────────────────
// create_niagara_system
// ─────────────────────────────────────────────────────────────
class FTool_CreateNiagaraSystem : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.system.create");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Name = Params->GetStringField(TEXT("name"));
		if (Name.IsEmpty())
		{
			return FMCPToolResult::Error(TEXT("Parameter 'name' is required."));
		}

		FString PackagePath = FString::Printf(TEXT("/Game/Effects/%s"), *Name);
		FString PackageName = FPackageName::ObjectPathToPackageName(PackagePath);

		// Crash-safety: bail gracefully if the asset already exists instead of letting
		// the engine creation path fatal-assert and take down the editor.
		if (FPackageName::DoesPackageExist(PackageName))
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *PackagePath));
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Failed to create package at '%s'."), *PackageName));
		}

		// The factory UObject class is not exported by NiagaraEditor in UE 5.3, but
		// its initialization routine is. Create the asset directly, then initialize
		// the default spawn/update scripts before saving.
		UNiagaraSystem* System = NewObject<UNiagaraSystem>(
			Package,
			UNiagaraSystem::StaticClass(),
			*Name,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!System)
		{
			return FMCPToolResult::Error(TEXT("Failed to create UNiagaraSystem."));
		}
		UNiagaraSystemFactoryNew::InitializeSystem(System, true);
		System->RequestCompile(false);

		FAssetRegistryModule::AssetCreated(System);
		System->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FString FilePath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		UPackage::SavePackage(Package, System, *FilePath, SaveArgs);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), PackagePath);
		Result->SetStringField(TEXT("name"), Name);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// spawn_niagara_actor
// ─────────────────────────────────────────────────────────────
class FTool_SpawnNiagaraActor : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.actor.spawn");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString SystemPath = Params->GetStringField(TEXT("system"));
		const TSharedPtr<FJsonObject>& LocObj = Params->GetObjectField(TEXT("location"));

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
		if (!System)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Niagara System not found at '%s'."), *SystemPath));
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Error(TEXT("No editor world available."));
		}

		FVector Location(LocObj->GetNumberField(TEXT("x")), LocObj->GetNumberField(TEXT("y")), LocObj->GetNumberField(TEXT("z")));

		FRotator Rotation = FRotator::ZeroRotator;
		if (Params->HasField(TEXT("rotation")))
		{
			const TSharedPtr<FJsonObject>& RotObj = Params->GetObjectField(TEXT("rotation"));
			Rotation.Pitch = (float)RotObj->GetNumberField(TEXT("pitch"));
			Rotation.Yaw = (float)RotObj->GetNumberField(TEXT("yaw"));
			Rotation.Roll = (float)RotObj->GetNumberField(TEXT("roll"));
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ANiagaraActor* NiagaraActor = World->SpawnActor<ANiagaraActor>(Location, Rotation, SpawnParams);

		if (!NiagaraActor)
		{
			return FMCPToolResult::Error(TEXT("Failed to spawn ANiagaraActor."));
		}

		UNiagaraComponent* NiagaraComp = NiagaraActor->GetNiagaraComponent();
		if (NiagaraComp)
		{
			NiagaraComp->SetAsset(System);
			NiagaraComp->Activate(true);
		}

		if (Params->HasField(TEXT("label")))
		{
			NiagaraActor->SetActorLabel(Params->GetStringField(TEXT("label")));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), NiagaraActor->GetName());
		Result->SetStringField(TEXT("label"), NiagaraActor->GetActorLabel());
		Result->SetStringField(TEXT("system"), SystemPath);

		TSharedPtr<FJsonObject> LocResult = MakeShared<FJsonObject>();
		LocResult->SetNumberField(TEXT("x"), Location.X);
		LocResult->SetNumberField(TEXT("y"), Location.Y);
		LocResult->SetNumberField(TEXT("z"), Location.Z);
		Result->SetObjectField(TEXT("location"), LocResult);

		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// add_niagara_emitter
// ─────────────────────────────────────────────────────────────
class FTool_AddNiagaraEmitter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.emitter.add");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString SystemPath = Params->GetStringField(TEXT("system"));
		FString EmitterPath = Params->GetStringField(TEXT("emitterTemplate"));

		UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
		if (!System)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Niagara System not found at '%s'."), *SystemPath));
		}

		UNiagaraEmitter* EmitterTemplate = LoadObject<UNiagaraEmitter>(nullptr, *EmitterPath);
		if (!EmitterTemplate)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Niagara Emitter not found at '%s'."), *EmitterPath));
		}

		// Add the emitter handle to the system
		FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(*EmitterTemplate, FName(*EmitterTemplate->GetName()), EmitterTemplate->GetExposedVersion().VersionGuid);

		System->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), SystemPath);
		Result->SetStringField(TEXT("emitter"), EmitterPath);
		Result->SetStringField(TEXT("handle_name"), NewHandle.GetName().ToString());
		Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// set_niagara_parameter
// ─────────────────────────────────────────────────────────────
class FTool_SetNiagaraParameter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.parameter.set");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString ActorName = Params->GetStringField(TEXT("actor"));
		FString ParamName = Params->GetStringField(TEXT("paramName"));
		FString Value = Params->GetStringField(TEXT("value"));
		FString ParamType = Params->HasField(TEXT("paramType")) ? Params->GetStringField(TEXT("paramType")).ToLower() : TEXT("float");

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Error(TEXT("No editor world available."));
		}

		// Find Niagara actor
		ANiagaraActor* FoundActor = nullptr;
		for (TActorIterator<ANiagaraActor> It(World); It; ++It)
		{
			if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
			{
				FoundActor = *It;
				break;
			}
		}
		if (!FoundActor)
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Niagara actor '%s' not found."), *ActorName));
		}

		UNiagaraComponent* NiagaraComp = FoundActor->GetNiagaraComponent();
		if (!NiagaraComp)
		{
			return FMCPToolResult::Error(TEXT("Actor has no NiagaraComponent."));
		}

		FName FParamName(*ParamName);

		if (ParamType == TEXT("float"))
		{
			float FloatVal = FCString::Atof(*Value);
			NiagaraComp->SetVariableFloat(FParamName, FloatVal);
		}
		else if (ParamType == TEXT("int"))
		{
			int32 IntVal = FCString::Atoi(*Value);
			NiagaraComp->SetVariableInt(FParamName, IntVal);
		}
		else if (ParamType == TEXT("bool"))
		{
			bool BoolVal = Value.ToBool();
			NiagaraComp->SetVariableBool(FParamName, BoolVal);
		}
		else if (ParamType == TEXT("vector"))
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() >= 3)
			{
				FVector VecVal(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]));
				NiagaraComp->SetVariableVec3(FParamName, VecVal);
			}
			else
			{
				return FMCPToolResult::Error(TEXT("Vector value must be 'x,y,z'."));
			}
		}
		else if (ParamType == TEXT("color"))
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			if (Parts.Num() >= 3)
			{
				float R = FCString::Atof(*Parts[0]);
				float G = FCString::Atof(*Parts[1]);
				float B = FCString::Atof(*Parts[2]);
				float A = Parts.Num() >= 4 ? FCString::Atof(*Parts[3]) : 1.0f;
				NiagaraComp->SetVariableLinearColor(FParamName, FLinearColor(R, G, B, A));
			}
			else
			{
				return FMCPToolResult::Error(TEXT("Color value must be 'r,g,b' or 'r,g,b,a'."));
			}
		}
		else
		{
			return FMCPToolResult::Error(FString::Printf(TEXT("Unknown paramType '%s'. Use: float, int, bool, vector, color."), *ParamType));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("actor"), ActorName);
		Result->SetStringField(TEXT("paramName"), ParamName);
		Result->SetStringField(TEXT("value"), Value);
		Result->SetStringField(TEXT("paramType"), ParamType);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// list_niagara_systems
// ─────────────────────────────────────────────────────────────
class FTool_ListNiagaraSystems : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.system.list");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		FString Filter = Params->HasField(TEXT("filter")) ? Params->GetStringField(TEXT("filter")) : TEXT("");

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Assets, true);

		TArray<TSharedPtr<FJsonValue>> SystemsArray;
		for (const FAssetData& Asset : Assets)
		{
			FString AssetName = Asset.AssetName.ToString();
			if (!Filter.IsEmpty() && !AssetName.Contains(Filter))
			{
				continue;
			}

			TSharedPtr<FJsonObject> SysObj = MakeShared<FJsonObject>();
			SysObj->SetStringField(TEXT("name"), AssetName);
			SysObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			SysObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			SystemsArray.Add(MakeShared<FJsonValueObject>(SysObj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), SystemsArray.Num());
		Result->SetArrayField(TEXT("systems"), SystemsArray);
		return FMCPToolResult::Ok(Result);
	}
};
#else
class FUnavailableNiagaraTool final : public FMCPToolBase
{
public:
	explicit FUnavailableNiagaraTool(FString InCapabilityId)
		: CapabilityId(MoveTemp(InCapabilityId))
	{
	}

	FString GetCapabilityId() const override
	{
		return CapabilityId;
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		return FMCPToolResult::Error(
			TEXT("Niagara support was not compiled into this plugin build."),
			TEXT("capability_unavailable"),
			409);
	}

private:
	FString CapabilityId;
};
#endif

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────
namespace UEAIIntegrationTools
{
	void RegisterNiagaraTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA
		Registry.Register(MakeShared<FTool_CreateNiagaraSystem>());
		Registry.Register(MakeShared<FTool_SpawnNiagaraActor>());
		Registry.Register(MakeShared<FTool_AddNiagaraEmitter>());
		Registry.Register(MakeShared<FTool_SetNiagaraParameter>());
		Registry.Register(MakeShared<FTool_ListNiagaraSystems>());
#else
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.system.create")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.actor.spawn")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.emitter.add")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.parameter.set")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.system.list")));
#endif
	}
}
