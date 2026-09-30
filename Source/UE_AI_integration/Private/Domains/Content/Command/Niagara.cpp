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
#include "NiagaraEditorUtilities.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraScriptSource.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "HAL/FileManager.h"
#include "Infrastructure/Sha256.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

// ─────────────────────────────────────────────────────────────
// Shared Niagara System/emitter-handle helpers. Kept local to this
// file so the save/remove/set_enabled handlers stay small and share one
// loading and identity resolution path.
// ─────────────────────────────────────────────────────────────
namespace
{
	constexpr int32 MaxSystemPathCharacters = 2048;

	FMCPToolResult LoadNiagaraSystem(const TSharedPtr<FJsonObject>& Params, UNiagaraSystem*& OutSystem)
	{
		FString ObjectPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), ObjectPath)
			|| ObjectPath.IsEmpty() || ObjectPath.Len() > MaxSystemPathCharacters)
		{
			return FMCPToolResult::Error(
				TEXT("system must be an exact Niagara System package or object path."),
				TEXT("invalid_niagara_system_request"), 422);
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return FMCPToolResult::Error(
				TEXT("system must be a valid long package or object path."),
				TEXT("invalid_niagara_system_request"), 422);
		}
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		}
		OutSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		return OutSystem
			       ? FMCPToolResult::Ok(nullptr)
			       : FMCPToolResult::Error(TEXT("The Niagara System was not found."), TEXT("system_not_found"), 404);
	}

	FMCPToolResult ResolveEmitterHandle(
		const TSharedPtr<FJsonObject>& Params,
		UNiagaraSystem* System,
		int32& OutHandleIndex)
	{
		FString EmitterHandleId;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("emitterHandleId"), EmitterHandleId)
			|| EmitterHandleId.IsEmpty() || EmitterHandleId.Len() > 64)
		{
			return FMCPToolResult::Error(
				TEXT("emitterHandleId must be an emitter handle GUID from content.niagara.system.inspect."),
				TEXT("invalid_emitter_handle_id"), 422);
		}
		FGuid RequestedId;
		if (!FGuid::Parse(EmitterHandleId, RequestedId))
		{
			return FMCPToolResult::Error(
				TEXT("emitterHandleId must be a valid GUID."),
				TEXT("invalid_emitter_handle_id"), 422);
		}
		// GetEmitterHandles() returns a non-const array in the editor, so the
		// enabled-state setter below can mutate the handle in place.
		TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		for (int32 Index = 0; Index < Handles.Num(); ++Index)
		{
			if (Handles[Index].GetId() == RequestedId)
			{
				OutHandleIndex = Index;
				return FMCPToolResult::Ok(nullptr);
			}
		}
		return FMCPToolResult::Error(
			TEXT("The emitter handle was not found in this Niagara System."),
			TEXT("emitter_handle_not_found"), 404);
	}
}

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
			return FMCPToolResult::Error(FString::Printf(
				TEXT("An asset already exists at '%s'. Delete it first or use a different name."), *PackagePath));
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
		FString FilePath = FPackageName::LongPackageNameToFilename(PackageName,
		                                                           FPackageName::GetAssetPackageExtension());
		UPackage::SavePackage(Package, System, *FilePath, SaveArgs);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("path"), PackagePath);
		Result->SetStringField(TEXT("name"), Name);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// duplicate_niagara_system
// ─────────────────────────────────────────────────────────────
class FTool_DuplicateNiagaraSystem : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.system.duplicate");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult LoadResult = LoadNiagaraSystem(Params, System);
		if (!LoadResult.bSuccess)
		{
			return LoadResult;
		}

		// Direct write: only non-transient /Game/ systems may be duplicated.
		UPackage* SourcePackage = System->GetOutermost();
		const FString SourcePackageName = SourcePackage ? SourcePackage->GetName() : FString();
		if (!SourcePackage || SourcePackage->HasAnyFlags(RF_Transient)
			|| SourcePackage == GetTransientPackage()
			|| !SourcePackageName.StartsWith(TEXT("/Game/")))
		{
			return FMCPToolResult::Error(
				TEXT("The source Niagara System must be a non-transient asset under /Game/."),
				TEXT("system_read_only"), 409);
		}

		const FString SourceName = System->GetName();

		// newName defaults to <Source>_Copy.
		FString NewName = SourceName + TEXT("_Copy");
		if (Params.IsValid() && Params->HasField(TEXT("newName")))
		{
			const TSharedPtr<FJsonValue> NewNameValue = Params->TryGetField(TEXT("newName"));
			if (!NewNameValue.IsValid() || NewNameValue->Type != EJson::String)
			{
				return FMCPToolResult::Error(
					TEXT("newName must be a string."),
					TEXT("invalid_asset_name"), 422);
			}
			NewNameValue->TryGetString(NewName);
		}

		// targetPath defaults to the source system's package folder.
		FString TargetPath = FPackageName::GetLongPackagePath(SourcePackageName);
		if (Params.IsValid() && Params->HasField(TEXT("targetPath")))
		{
			const TSharedPtr<FJsonValue> TargetPathValue = Params->TryGetField(TEXT("targetPath"));
			if (!TargetPathValue.IsValid() || TargetPathValue->Type != EJson::String)
			{
				return FMCPToolResult::Error(
					TEXT("targetPath must be a string."),
					TEXT("invalid_target_path"), 422);
			}
			TargetPathValue->TryGetString(TargetPath);
		}

		// Normalize a trailing slash so /Game/Effects/ and /Game/Effects match.
		while (TargetPath.EndsWith(TEXT("/")) && TargetPath.Len() > 1)
		{
			TargetPath.RemoveFromEnd(TEXT("/"));
		}
		if (TargetPath.IsEmpty() || !TargetPath.StartsWith(TEXT("/Game/"))
			|| !FPackageName::IsValidLongPackageName(TargetPath))
		{
			return FMCPToolResult::Error(
				TEXT("targetPath must be a valid package folder under /Game/."),
				TEXT("invalid_target_path"), 422);
		}

		bool bInvalidAssetName = NewName.IsEmpty();
		for (const TCHAR Ch : NewName)
		{
			if (Ch == TEXT(' ') || Ch == TEXT('\t') || Ch == TEXT('\r')
				|| Ch == TEXT('\n') || Ch == TEXT('/') || Ch == TEXT('\\'))
			{
				bInvalidAssetName = true;
				break;
			}
		}
		if (bInvalidAssetName)
		{
			return FMCPToolResult::Error(
				TEXT("newName must be a non-empty asset name without spaces or slashes."),
				TEXT("invalid_asset_name"), 422);
		}

		const FString TargetPackageName = TargetPath + TEXT("/") + NewName;
		if (FPackageName::DoesPackageExist(TargetPackageName))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("An asset already exists at '%s'."), *TargetPackageName),
				TEXT("asset_exists"), 409);
		}

		// IAssetTools::DuplicateAsset duplicates the object and package in memory
		// (shallow: emitter asset references stay shared with the source), but it
		// only persists when source control is enabled, so save explicitly below.
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Duplicated = AssetTools.DuplicateAsset(NewName, TargetPath, System);
		UNiagaraSystem* NewSystem = Cast<UNiagaraSystem>(Duplicated);
		if (!NewSystem)
		{
			return FMCPToolResult::Error(
				FString::Printf(
					TEXT("Failed to duplicate Niagara System '%s' to '%s'."),
					*SourcePackageName, *TargetPackageName),
				TEXT("system_duplicate_failed"), 500);
		}

		// SavePackage does not create missing content directories.
		const FString FilePath = FPackageName::LongPackageNameToFilename(
			TargetPackageName, FPackageName::GetAssetPackageExtension());
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(NewSystem->GetOutermost(), NewSystem, *FilePath, SaveArgs))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Failed to save duplicated Niagara System '%s'."), *TargetPackageName),
				TEXT("system_duplicate_failed"), 500);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-duplicate.v1"));
		Result->SetStringField(TEXT("sourceSystem"), SourceName);
		Result->SetStringField(TEXT("sourceSystemPath"), System->GetPathName());
		Result->SetStringField(TEXT("newSystem"), NewSystem->GetName());
		Result->SetStringField(TEXT("newSystemPath"), NewSystem->GetPathName());
		Result->SetBoolField(TEXT("saved"), true);
		Result->SetStringField(TEXT("scope"),
		                       TEXT("shallow asset duplication; emitter asset references are shared with the source"));
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

		FVector Location(LocObj->GetNumberField(TEXT("x")), LocObj->GetNumberField(TEXT("y")),
		                 LocObj->GetNumberField(TEXT("z")));

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

		const int32 HandleCountBefore = System->GetEmitterHandles().Num();
		const FGuid NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(
			*System,
			*EmitterTemplate,
			EmitterTemplate->GetExposedVersion().VersionGuid);
		if (!NewHandleId.IsValid() || System->GetEmitterHandles().Num() <= HandleCountBefore)
		{
			return FMCPToolResult::Error(
				TEXT("Niagara editor could not add the emitter to the system."),
				TEXT("emitter_add_failed"),
				500);
		}

		int32 NewHandleIndex = INDEX_NONE;
		for (int32 Index = 0; Index < System->GetEmitterHandles().Num(); ++Index)
		{
			if (System->GetEmitterHandles()[Index].GetId() == NewHandleId)
			{
				NewHandleIndex = Index;
				break;
			}
		}
		if (NewHandleIndex == INDEX_NONE)
		{
			// AddEmitterToSystem already mutated the authored handle array. Do not
			// return with a partially applied emitter when the identity readback
			// cannot resolve the new handle.
			for (int32 Index = System->GetEmitterHandles().Num() - 1; Index >= 0; --Index)
			{
				if (System->GetEmitterHandles()[Index].GetId() == NewHandleId)
				{
					System->RemoveEmitterHandle(System->GetEmitterHandles()[Index]);
					break;
				}
			}
			return FMCPToolResult::Error(
				TEXT("The newly added Niagara emitter handle could not be resolved."),
				TEXT("emitter_readback_failed"),
				500);
		}

		bool bGraphRebuilt = false;
		if (FVersionedNiagaraEmitterData* EmitterData = System->GetEmitterHandles()[NewHandleIndex].GetEmitterData())
		{
			if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
			{
				if (Source->NodeGraph)
				{
					for (UEdGraphNode* Node : Source->NodeGraph->Nodes)
					{
						if (Node)
						{
							Node->CreateNewGuid();
						}
					}
					Source->NodeGraph->NotifyGraphChanged();
					bGraphRebuilt = true;
				}
			}
		}
		if (!bGraphRebuilt)
		{
			System->RemoveEmitterHandle(System->GetEmitterHandles()[NewHandleIndex]);
			return FMCPToolResult::Error(
				TEXT("The emitter was added without an authored graph source; the mutation was rolled back."),
				TEXT("emitter_graph_missing"),
				500);
		}

		System->MarkPackageDirty();
		System->RequestCompile(false);
		System->WaitForCompilationComplete(false, false);

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), SystemPath);
		Result->SetStringField(TEXT("emitter"), EmitterPath);
		Result->SetStringField(TEXT("handle_id"), NewHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetStringField(TEXT("handle_name"), System->GetEmitterHandles()[NewHandleIndex].GetName().ToString());
		Result->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
		Result->SetBoolField(TEXT("graphRebuilt"), bGraphRebuilt);
		Result->SetBoolField(TEXT("compileRequested"), true);
		Result->SetBoolField(TEXT("compilationPending"), System->HasOutstandingCompilationRequests(false));
		Result->SetBoolField(TEXT("saved"), false);
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
		FString ParamType = Params->HasField(TEXT("paramType"))
			                    ? Params->GetStringField(TEXT("paramType")).ToLower()
			                    : TEXT("float");

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
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Unknown paramType '%s'. Use: float, int, bool, vector, color."), *ParamType));
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
		FString Filter;
		if (Params.IsValid() && Params->HasField(TEXT("filter")))
		{
			const TSharedPtr<FJsonValue> FilterValue = Params->TryGetField(TEXT("filter"));
			if (!FilterValue.IsValid() || FilterValue->Type != EJson::String)
			{
				return FMCPToolResult::Error(
					TEXT("filter must be a string."), TEXT("invalid_params"), 422);
			}
			FilterValue->TryGetString(Filter);
		}
		int32 Limit = 50;
		int32 Offset = 0;
		auto ReadPageInteger = [&](const TCHAR* Field, int32 DefaultValue, int32 Maximum, int32& OutValue)
		{
			OutValue = DefaultValue;
			if (!Params.IsValid() || !Params->HasField(Field))
			{
				return true;
			}
			double Number = 0.0;
			return Params->TryGetNumberField(Field, Number)
				&& FMath::IsFinite(Number)
				&& Number >= 0.0
				&& Number <= static_cast<double>(Maximum)
				&& Number == FMath::FloorToDouble(Number)
				&& (OutValue = static_cast<int32>(Number), true);
		};
		if (!ReadPageInteger(TEXT("limit"), 50, 200, Limit)
			|| !ReadPageInteger(TEXT("offset"), 0, MAX_int32, Offset)
			|| Limit < 1)
		{
			return FMCPToolResult::Error(
				TEXT("limit must be an integer from 1 to 200 and offset must be a non-negative integer."),
				TEXT("invalid_params"), 422);
		}

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).
			Get();

		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByClass(UNiagaraSystem::StaticClass()->GetClassPathName(), Assets, true);

		Assets.Sort([](const FAssetData& Left, const FAssetData& Right)
		{
			const FString LeftKey = Left.PackageName.ToString() + TEXT("|") + Left.AssetName.ToString();
			const FString RightKey = Right.PackageName.ToString() + TEXT("|") + Right.AssetName.ToString();
			return LeftKey < RightKey;
		});
		TArray<const FAssetData*> Matches;
		for (const FAssetData& Asset : Assets)
		{
			FString AssetName = Asset.AssetName.ToString();
			if (!Filter.IsEmpty()
				&& !AssetName.Contains(Filter, ESearchCase::IgnoreCase)
				&& !Asset.PackageName.ToString().Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
			Matches.Add(&Asset);
		}

		const int32 Start = FMath::Min(Offset, Matches.Num());
		const int32 End = FMath::Min(Start + Limit, Matches.Num());
		TArray<TSharedPtr<FJsonValue>> SystemsArray;
		for (int32 Index = Start; Index < End; ++Index)
		{
			const FAssetData& Asset = *Matches[Index];
			FString AssetName = Asset.AssetName.ToString();

			TSharedPtr<FJsonObject> SysObj = MakeShared<FJsonObject>();
			SysObj->SetStringField(TEXT("name"), AssetName);
			SysObj->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			SysObj->SetStringField(TEXT("package"), Asset.PackageName.ToString());
			SystemsArray.Add(MakeShared<FJsonValueObject>(SysObj));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("count"), SystemsArray.Num());
		Result->SetNumberField(TEXT("total"), Matches.Num());
		Result->SetNumberField(TEXT("offset"), Offset);
		Result->SetNumberField(TEXT("limit"), Limit);
		const bool bHasMore = End < Matches.Num();
		Result->SetBoolField(TEXT("hasMore"), bHasMore);
		if (bHasMore)
		{
			Result->SetNumberField(TEXT("nextOffset"), End);
		}
		Result->SetArrayField(TEXT("systems"), SystemsArray);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// save_niagara_system
// ─────────────────────────────────────────────────────────────
class FTool_SaveNiagaraSystem : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.system.save");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult LoadResult = LoadNiagaraSystem(Params, System);
		if (!LoadResult.bSuccess)
		{
			return LoadResult;
		}

		UPackage* Package = System->GetOutermost();
		if (!Package)
		{
			return FMCPToolResult::Error(
				TEXT("The Niagara System has no owning package."),
				TEXT("asset_package_missing"), 500);
		}
		const FString PackageName = Package->GetName();
		if (!FPackageName::IsValidLongPackageName(PackageName)
			|| Package->HasAnyFlags(RF_Transient) || Package == GetTransientPackage())
		{
			return FMCPToolResult::Error(
				TEXT("Transient Niagara System packages cannot be saved."),
				TEXT("system_read_only"), 409);
		}

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		// SavePackage does not create missing content directories; ensure the
		// parent folder exists first (mirrors the plugin recovery save helper).
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);

		// Save directly without requesting any compile. This capability must never
		// block on the Niagara async compile pipeline.
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, System, *Filename, SaveArgs))
		{
			return FMCPToolResult::Error(
				FString::Printf(TEXT("Failed to save Niagara System package '%s'."), *PackageName),
				TEXT("asset_save_failed"), 500);
		}
		if (!FPaths::FileExists(Filename) || Package->IsDirty())
		{
			return FMCPToolResult::Error(
				TEXT("SavePackage returned success but the file or dirty-state verification failed."),
				TEXT("post_save_verification_failed"), 500);
		}

		TArray<uint8> FileBytes;
		FString FileDigest;
		if (FFileHelper::LoadFileToArray(FileBytes, *Filename))
		{
			UEAIIntegration::Infrastructure::TrySha256Hex(FileBytes, FileDigest);
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(TEXT("package"), PackageName);
		Result->SetStringField(TEXT("file"), Filename);
		Result->SetStringField(TEXT("sha256"), FileDigest);
		Result->SetBoolField(TEXT("saved"), true);
		Result->SetBoolField(TEXT("dirty"), Package->IsDirty());
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// remove_niagara_emitter
// ─────────────────────────────────────────────────────────────
class FTool_RemoveNiagaraEmitter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.emitter.remove");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult LoadResult = LoadNiagaraSystem(Params, System);
		if (!LoadResult.bSuccess)
		{
			return LoadResult;
		}

		int32 HandleIndex = INDEX_NONE;
		FMCPToolResult ResolveResult = ResolveEmitterHandle(Params, System, HandleIndex);
		if (!ResolveResult.bSuccess)
		{
			return ResolveResult;
		}

		// Capture identity before RemoveEmitterHandle invalidates the array entry.
		const FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[HandleIndex];
		const FGuid RemovedId = Handle.GetId();
		const FString RemovedName = Handle.GetName().ToString();
		// Editor-only UNiagaraSystem API that removes the handle and refreshes
		// cached system parameters without requesting a compile.
		System->RemoveEmitterHandle(Handle);
		System->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(TEXT("removedHandleId"), RemovedId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetStringField(TEXT("removedName"), RemovedName);
		Result->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// set_niagara_emitter_enabled
// ─────────────────────────────────────────────────────────────
class FTool_SetNiagaraEmitterEnabled : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.emitter.set_enabled");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult LoadResult = LoadNiagaraSystem(Params, System);
		if (!LoadResult.bSuccess)
		{
			return LoadResult;
		}
		bool bEnabled = false;
		if (!Params.IsValid() || !Params->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			return FMCPToolResult::Error(
				TEXT("enabled must be a boolean."),
				TEXT("invalid_enabled_state"), 422);
		}

		int32 HandleIndex = INDEX_NONE;
		FMCPToolResult ResolveResult = ResolveEmitterHandle(Params, System, HandleIndex);
		if (!ResolveResult.bSuccess)
		{
			return ResolveResult;
		}

		FNiagaraEmitterHandle& Handle = System->GetEmitterHandles()[HandleIndex];
		// SetIsEnabled(bool, UNiagaraSystem&, bool bRecompileIfChanged) is the
		// UE 5.4 emitter-handle setter. Passing false for the last argument means
		// this command never requests a compile; it still invalidates compiled
		// results and refreshes system parameters.
		const bool bChanged = Handle.SetIsEnabled(bEnabled, *System, /*bRecompileIfChanged=*/false);
		System->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(TEXT("emitterHandleId"), Handle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetStringField(TEXT("emitterName"), Handle.GetName().ToString());
		Result->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
		Result->SetBoolField(TEXT("changed"), bChanged);
		return FMCPToolResult::Ok(Result);
	}
};

// ─────────────────────────────────────────────────────────────
// duplicate_niagara_emitter
// ─────────────────────────────────────────────────────────────
class FTool_DuplicateNiagaraEmitter : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.emitter.duplicate");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult LoadResult = LoadNiagaraSystem(Params, System);
		if (!LoadResult.bSuccess)
		{
			return LoadResult;
		}

		int32 HandleIndex = INDEX_NONE;
		FMCPToolResult ResolveResult = ResolveEmitterHandle(Params, System, HandleIndex);
		if (!ResolveResult.bSuccess)
		{
			return ResolveResult;
		}

		const FNiagaraEmitterHandle& Source = System->GetEmitterHandles()[HandleIndex];
		const FString SourceId = Source.GetId().ToString(EGuidFormats::DigitsWithHyphensLower);

		// FNiagaraEditorUtilities::DuplicateEmitter does not exist; the exported
		// UE 5.4 API is UNiagaraSystem::DuplicateEmitterHandle (NIAGARA_API). It
		// appends a new handle that references the same source asset but owns a
		// duplicated instance value. Give it a collision-free "_Copy" name so the
		// system never ends up with two identically named emitter handles.
		auto IsNameTaken = [&System](const FName& Name)
		{
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetName() == Name)
				{
					return true;
				}
			}
			return false;
		};
		const FString BaseName = Source.GetName().ToString() + TEXT("_Copy");
		FName DupName(*BaseName);
		for (int32 Suffix = 2; IsNameTaken(DupName); ++Suffix)
		{
			DupName = FName(*(BaseName + FString::FromInt(Suffix)));
		}

		FNiagaraEmitterHandle NewHandle = System->DuplicateEmitterHandle(Source, DupName);
		System->MarkPackageDirty();

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(TEXT("sourceEmitterHandleId"), SourceId);
		Result->SetStringField(
			TEXT("emitterHandleId"), NewHandle.GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetStringField(TEXT("emitterName"), NewHandle.GetName().ToString());
		Result->SetNumberField(TEXT("emitterCount"), System->GetEmitterHandles().Num());
		return FMCPToolResult::Ok(Result);
	}
};

class FTool_ReorderNiagaraEmitters : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override
	{
		return TEXT("content.niagara.emitter.reorder");
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult LoadResult = LoadNiagaraSystem(Params, System);
		if (!LoadResult.bSuccess)
		{
			return LoadResult;
		}

		int32 HandleIndex = INDEX_NONE;
		FMCPToolResult ResolveResult = ResolveEmitterHandle(Params, System, HandleIndex);
		if (!ResolveResult.bSuccess)
		{
			return ResolveResult;
		}

		double TargetNumber = 0.0;
		if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("targetIndex"), TargetNumber)
			|| !FMath::IsFinite(TargetNumber) || TargetNumber < 0.0
			|| TargetNumber != FMath::FloorToDouble(TargetNumber))
		{
			return FMCPToolResult::Error(
				TEXT("targetIndex must be a non-negative integer."),
				TEXT("invalid_target_index"), 422);
		}

		TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
		const int32 TargetIndex = FMath::Min(static_cast<int32>(TargetNumber), Handles.Num() - 1);
		const FGuid ReorderedId = Handles[HandleIndex].GetId();
		const FString ReorderedName = Handles[HandleIndex].GetName().ToString();

		if (HandleIndex != TargetIndex)
		{
			// Emitter handles are value types; a remove + insert reorders the array.
			const FNiagaraEmitterHandle Handle = Handles[HandleIndex];
			Handles.RemoveAt(HandleIndex);
			Handles.Insert(Handle, TargetIndex);
			System->MarkPackageDirty();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("system"), System->GetPathName());
		Result->SetStringField(
			TEXT("emitterHandleId"), ReorderedId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Result->SetStringField(TEXT("emitterName"), ReorderedName);
		Result->SetNumberField(TEXT("fromIndex"), HandleIndex);
		Result->SetNumberField(TEXT("targetIndex"), TargetIndex);
		Result->SetNumberField(TEXT("emitterCount"), Handles.Num());
		Result->SetBoolField(TEXT("changed"), HandleIndex != TargetIndex);
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
	void RegisterNiagaraRendererMaterialTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraSystemReadTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraSystemParameterTools(FMCPToolRegistry& Registry);
	void RegisterNiagaraCompileTools(FMCPToolRegistry& Registry);

	void RegisterNiagaraTools(FMCPToolRegistry& Registry)
	{
		RegisterNiagaraRendererMaterialTools(Registry);
		RegisterNiagaraSystemReadTools(Registry);
		RegisterNiagaraSystemParameterTools(Registry);
		RegisterNiagaraCompileTools(Registry);
#if WITH_UEAI_NIAGARA
		Registry.Register(MakeShared<FTool_CreateNiagaraSystem>());
		Registry.Register(MakeShared<FTool_DuplicateNiagaraSystem>());
		Registry.Register(MakeShared<FTool_SpawnNiagaraActor>());
		Registry.Register(MakeShared<FTool_AddNiagaraEmitter>());
		Registry.Register(MakeShared<FTool_SetNiagaraParameter>());
		Registry.Register(MakeShared<FTool_ListNiagaraSystems>());
		Registry.Register(MakeShared<FTool_SaveNiagaraSystem>());
		Registry.Register(MakeShared<FTool_RemoveNiagaraEmitter>());
		Registry.Register(MakeShared<FTool_SetNiagaraEmitterEnabled>());
		Registry.Register(MakeShared<FTool_DuplicateNiagaraEmitter>());
		Registry.Register(MakeShared<FTool_ReorderNiagaraEmitters>());
#else
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.system.create")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.system.duplicate")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.actor.spawn")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.emitter.add")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.parameter.set")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.system.list")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.system.save")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.emitter.remove")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.emitter.set_enabled")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.emitter.duplicate")));
		Registry.Register(MakeShared<FUnavailableNiagaraTool>(
			TEXT("content.niagara.emitter.reorder")));
#endif
	}
}
