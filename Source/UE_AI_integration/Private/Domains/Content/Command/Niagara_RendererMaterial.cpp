// Authored Niagara renderer material inspection and plan-gated editing.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"
#include "NiagaraReceiptSupport.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraRibbonRendererProperties.h"
#include "NiagaraLightRendererProperties.h"
#include "NiagaraComponentRendererProperties.h"
#include "NiagaraDecalRendererProperties.h"
#include "NiagaraVolumeRendererProperties.h"
#include "NiagaraSystem.h"
#include "Materials/MaterialInterface.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAINiagaraRendererMaterial, Log, All);

namespace UEAINiagaraRendererMaterial
{
	using UEAIIntegration::Infrastructure::TryDigestJson;
	using UEAIIntegration::Infrastructure::ValidateChangeApproval;
	using UEAINiagaraReceiptSupport::FReleasedRequest;
	using UEAINiagaraReceiptSupport::FReleasedRequestHistory;
	using UEAINiagaraReceiptSupport::RestoreDirtyState;

	constexpr int32 MaxMaterialSlots = 256;
	constexpr int32 MaxPropertyCharacters = 64 * 1024;
	constexpr int32 MaxStateCharacters = 1024 * 1024;
	constexpr int32 MaxReceipts = UEAINiagaraReceiptSupport::MaxActiveReceiptCount;
	constexpr int32 MaxTerminalRequestHistory = UEAINiagaraReceiptSupport::MaxTerminalRequestHistory;

	struct FTarget
	{
		UNiagaraSystem* System = nullptr;
		UNiagaraEmitter* Emitter = nullptr;
		UNiagaraRendererProperties* Renderer = nullptr;
		FGuid EmitterId;
		FGuid EmitterVersion;
		FString EmitterName;
		int32 RendererIndex = INDEX_NONE;
		bool bEmitterEnabled = false;
	};

	struct FMaterialSlot
	{
		FObjectPropertyBase* MaterialProperty = nullptr;
		void* MaterialAddress = nullptr;
		FProperty* ChangedProperty = nullptr;
		FBoolProperty* OverrideProperty = nullptr;
		FString Binding;
		FString BindingProperty;
		bool bOverridesEnabled = true;
		bool bMissingArrayEntry = false;

		UMaterialInterface* GetMaterial() const
		{
			return MaterialProperty && MaterialAddress
				       ? Cast<UMaterialInterface>(MaterialProperty->GetObjectPropertyValue(MaterialAddress))
				       : nullptr;
		}
	};

	struct FPlan
	{
		FTarget Target;
		FMaterialSlot Slot;
		UMaterialInterface* Material = nullptr;
		int32 MaterialSlot = 0;
		bool bEnableOverrides = false;
		FString BeforeDigest;
		TMap<FString, FString> BeforeProperties;
		FString PlanDigest;
		TSharedPtr<FJsonObject> Json;
	};

	struct FReceipt
	{
		FString Id;
		FString RequestId;
		FString RequestDigest;
		FString PlanDigest;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraRendererProperties> Renderer;
		FString SystemPath;
		FString RendererPath;
		FString RendererClass;
		int32 RendererIndex = INDEX_NONE;
		FString EmitterId;
		int32 MaterialSlot = 0;
		FString BeforeMaterial;
		FString AfterMaterial;
		bool bBeforeOverrides = true;
		bool bAfterOverrides = true;
		bool bBeforeSlotMissing = false;
		bool bBeforeDirty = false;
		bool bAfterDirty = false;
		bool bDirtyRestored = false;
		bool bDirtyPreserved = false;
		bool bTrackedStateRestored = false;
		bool bFullPackageStateRestored = false;
		bool bChanged = false;
		bool bCompileRequested = false;
		bool bVerified = false;
		bool bRollbackAttempted = false;
		bool bRolledBack = false;
		bool bSemanticRollbackVerified = false;
		FString BeforeDigest;
		FString AfterDigest;
		FString RestoredDigest;
		TMap<FString, FString> BeforeProperties;
		TMap<FString, FString> AfterProperties;
		TMap<FString, FString> RestoredProperties;
	};

	// Active receipts retain same-Editor rollback boundaries. Released receipts keep
	// only bounded request identity history, so active boundaries are never evicted
	// merely to make room for another write.
	TMap<FString, FReceipt> Receipts;
	TMap<FString, FString> RequestReceipts;
	FReleasedRequestHistory ReleasedRequests;

	FMCPToolResult Error(const FString& Message, const TCHAR* Code = TEXT("invalid_renderer_request"),
	                     int32 Status = 422)
	{
		return FMCPToolResult::Error(Message, Code, Status);
	}

	bool ReadIndex(const TSharedPtr<FJsonObject>& Params, const TCHAR* Name, int32 Default, int32 Max, int32& Out)
	{
		double Number = Default;
		if (Params->HasField(Name) && !Params->TryGetNumberField(Name, Number)) return false;
		if (!FMath::IsFinite(Number) || Number < 0 || Number > Max || Number != FMath::FloorToDouble(Number))
			return
				false;
		Out = static_cast<int32>(Number);
		return true;
	}

	FString ObjectPath(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	FMCPToolResult LoadSystem(const TSharedPtr<FJsonObject>& Params, UNiagaraSystem*& Out)
	{
		FString Path;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), Path) || Path.Len() > 2048)
			return Error(TEXT("system must be an exact Niagara System package or object path."));
		FString Package = FPackageName::ObjectPathToPackageName(Path);
		if (!FPackageName::IsValidLongPackageName(Package))
			return Error(TEXT("system must be a valid long package or object path."));
		if (!Path.Contains(TEXT("."))) Path = Package + TEXT(".") + FPackageName::GetShortName(Package);
		Out = LoadObject<UNiagaraSystem>(nullptr, *Path, nullptr, LOAD_NoWarn);
		return Out
			       ? FMCPToolResult::Ok(nullptr)
			       : Error(TEXT("The Niagara System was not found."), TEXT("system_not_found"), 404);
	}

	FMCPToolResult FindEmitter(UNiagaraSystem* System, const FString& Selector, const FNiagaraEmitterHandle*& Out)
	{
		if (Selector.IsEmpty() || Selector.Len() > 2048)
			return Error(TEXT("emitter must be an exact handle ID or unambiguous display name."));
		FGuid Id;
		const bool bId = FGuid::Parse(Selector, Id);
		TArray<const FNiagaraEmitterHandle*> Matches;
		for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		{
			if (bId ? Handle.GetId() == Id : Handle.GetName().ToString() == Selector) Matches.Add(&Handle);
		}
		if (Matches.Num() == 0) return Error(TEXT("The emitter was not found."), TEXT("emitter_not_found"), 404);
		if (Matches.Num() != 1)
			return Error(
				TEXT("Emitter name is ambiguous; use its handle ID."), TEXT("ambiguous_emitter"), 409);
		Out = Matches[0];
		if (!Out->GetEmitterData())
			return Error(
				TEXT("This emitter does not expose standard versioned renderer data."), TEXT("emitter_unsupported"));
		return FMCPToolResult::Ok(nullptr);
	}

	bool Owned(const FTarget& Target)
	{
		return Target.System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))
			&& !Target.System->HasAnyFlags(RF_Transient)
			&& Target.Emitter && Target.Emitter->GetOutermost() == Target.System->GetOutermost()
			&& Target.Renderer->IsIn(Target.Emitter)
			&& Target.Renderer->GetOutermost() == Target.System->GetOutermost();
	}

	FTarget MakeTarget(UNiagaraSystem* System, const FNiagaraEmitterHandle& Handle,
	                   UNiagaraRendererProperties* Renderer, int32 Index)
	{
		FTarget Target;
		Target.System = System;
		Target.Emitter = Handle.GetInstance().Emitter;
		Target.EmitterVersion = Handle.GetInstance().Version;
		Target.EmitterId = Handle.GetId();
		Target.EmitterName = Handle.GetName().ToString();
		Target.bEmitterEnabled = Handle.GetIsEnabled();
		Target.Renderer = Renderer;
		Target.RendererIndex = Index;
		return Target;
	}

	FMCPToolResult ResolveTarget(const TSharedPtr<FJsonObject>& Params, FTarget& Out)
	{
		UNiagaraSystem* System = nullptr;
		FMCPToolResult Result = LoadSystem(Params, System);
		if (!Result.bSuccess) return Result;
		FString EmitterSelector, RendererPath;
		if (!Params->TryGetStringField(TEXT("emitter"), EmitterSelector)
			|| !Params->TryGetStringField(TEXT("rendererPath"), RendererPath)
			|| RendererPath.IsEmpty() || RendererPath.Len() > 2048)
			return Error(TEXT("emitter and exact rendererPath from renderer.list are required."));
		const FNiagaraEmitterHandle* Handle = nullptr;
		Result = FindEmitter(System, EmitterSelector, Handle);
		if (!Result.bSuccess) return Result;
		const auto& Renderers = Handle->GetEmitterData()->GetRenderers();
		for (int32 Index = 0; Index < Renderers.Num(); ++Index)
		{
			if (IsValid(Renderers[Index]) && Renderers[Index]->GetPathName() == RendererPath)
			{
				Out = MakeTarget(System, *Handle, Renderers[Index], Index);
				return FMCPToolResult::Ok(nullptr);
			}
		}
		return Error(
			TEXT("The renderer is no longer present in the selected emitter version."), TEXT("renderer_not_found"),
			404);
	}

	FMCPToolResult ResolveReadTarget(const TSharedPtr<FJsonObject>& Params, FTarget& Out)
	{
		if (Params.IsValid() && Params->HasField(TEXT("rendererPath")))
		{
			if (Params->HasField(TEXT("rendererIndex")))
			{
				return Error(TEXT("Use rendererPath or rendererIndex, not both."));
			}
			return ResolveTarget(Params, Out);
		}

		UNiagaraSystem* System = nullptr;
		FMCPToolResult Result = LoadSystem(Params, System);
		if (!Result.bSuccess)
		{
			return Result;
		}

		const FNiagaraEmitterHandle* Handle = nullptr;
		FString EmitterSelector;
		if (Params->TryGetStringField(TEXT("emitter"), EmitterSelector))
		{
			Result = FindEmitter(System, EmitterSelector, Handle);
			if (!Result.bSuccess)
			{
				return Result;
			}
		}
		else if (Params->HasField(TEXT("emitter")))
		{
			return Error(TEXT("emitter must be a string."));
		}
		else
		{
			TArray<const FNiagaraEmitterHandle*> Candidates;
			for (const FNiagaraEmitterHandle& Candidate : System->GetEmitterHandles())
			{
				const FVersionedNiagaraEmitterData* Data = Candidate.GetEmitterData();
				if (Data && Data->GetRenderers().Num() > 0)
				{
					Candidates.Add(&Candidate);
				}
			}
			if (Candidates.Num() == 0)
			{
				return Error(TEXT("The System has no emitter renderer."), TEXT("renderer_not_found"), 404);
			}
			if (Candidates.Num() != 1)
			{
				return Error(
					TEXT("emitter is required when more than one emitter has renderers."), TEXT("ambiguous_emitter"),
					409);
			}
			Handle = Candidates[0];
		}

		int32 RendererIndex = 0;
		if (!ReadIndex(Params, TEXT("rendererIndex"), 0, MaxMaterialSlots - 1, RendererIndex))
		{
			return Error(TEXT("rendererIndex must be an integer from 0 through 255."));
		}
		const TArray<UNiagaraRendererProperties*>& Renderers = Handle->GetEmitterData()->GetRenderers();
		if (!Renderers.IsValidIndex(RendererIndex) || !IsValid(Renderers[RendererIndex]))
		{
			return Error(TEXT("rendererIndex does not select a current renderer."), TEXT("renderer_not_found"), 404);
		}
		Out = MakeTarget(System, *Handle, Renderers[RendererIndex], RendererIndex);
		return FMCPToolResult::Ok(nullptr);
	}

	TSharedRef<FJsonObject> DescribeTarget(const FTarget& Target)
	{
		auto Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("system"), ObjectPath(Target.System));
		Row->SetStringField(TEXT("emitter"), Target.EmitterName);
		Row->SetStringField(TEXT("emitterId"), Target.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
		Row->SetStringField(
			TEXT("emitterVersion"), Target.EmitterVersion.ToString(EGuidFormats::DigitsWithHyphensLower));
		Row->SetBoolField(TEXT("emitterEnabled"), Target.bEmitterEnabled);
		Row->SetNumberField(TEXT("rendererIndex"), Target.RendererIndex);
		Row->SetStringField(TEXT("rendererPath"), ObjectPath(Target.Renderer));
		Row->SetStringField(TEXT("rendererClass"), Target.Renderer->GetClass()->GetPathName());
		Row->SetBoolField(TEXT("enabled"), Target.Renderer->GetIsEnabled());
		Row->SetBoolField(TEXT("editable"), Owned(Target));
		return Row;
	}

	// Compilation refreshes cached fields inside Niagara binding structs. Export
	// only authored fields so receipt identity survives those notifications.
	bool IsDerivedBindingField(const FProperty& Property)
	{
		const FName Name = Property.GetFName();
		return Name == TEXT("ParamMapVariable") || Name == TEXT("DataSetName")
			|| Name == TEXT("bBindingExistsOnSource") || Name == TEXT("bIsCachedParticleValue")
			|| Name == TEXT("CachedDisplayName")
			|| Name == TEXT("ResolvedNiagaraVariable") || Name == TEXT("RootVariable_DEPRECATED")
			|| Name == TEXT("DataSetVariable_DEPRECATED")
			|| Name == TEXT("LODLevel") || Name == TEXT("LODBias");
	}

	bool IsAuthoredValueProperty(const FProperty& Property)
	{
		return !Property.HasAnyPropertyFlags(
				CPF_Transient | CPF_Deprecated | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient)
			&& !IsDerivedBindingField(Property);
	}

	bool AppendAuthoredValue(const FProperty& Property, const void* Address, FString& Out, int32 Depth = 0)
	{
		if (Depth > 32) return false;
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				const FProperty* Field = *It;
				if (!IsAuthoredValueProperty(*Field)) continue;
				FString Value;
				if (!AppendAuthoredValue(*Field, Field->ContainerPtrToValuePtr<void>(Address), Value, Depth + 1))
					return
						false;
				Fields.Add(Field->GetName() + TEXT("=") + Value);
			}
			Out = Struct->Struct->GetPathName() + TEXT("{") + FString::Join(Fields, TEXT(";")) + TEXT("}");
			return true;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			FScriptArrayHelper Values(Array, Address);
			if (Values.Num() > MaxMaterialSlots) return false;
			TArray<FString> Items;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				FString Value;
				if (!AppendAuthoredValue(*Array->Inner, Values.GetRawPtr(Index), Value, Depth + 1)) return false;
				Items.Add(MoveTemp(Value));
			}
			Out = TEXT("[") + FString::Join(Items, TEXT(",")) + TEXT("]");
			return true;
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
		{
			FScriptSetHelper Values(Set, Address);
			if (Values.Num() > MaxMaterialSlots) return false;
			TArray<FString> Items;
			for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
			{
				if (!Values.IsValidIndex(Index)) continue;
				FString Value;
				if (!AppendAuthoredValue(*Set->ElementProp, Values.GetElementPtr(Index), Value, Depth + 1))
					return
						false;
				Items.Add(MoveTemp(Value));
			}
			Items.Sort();
			Out = TEXT("{") + FString::Join(Items, TEXT(",")) + TEXT("}");
			return true;
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
		{
			FScriptMapHelper Values(Map, Address);
			if (Values.Num() > MaxMaterialSlots) return false;
			TArray<FString> Items;
			for (int32 Index = 0; Index < Values.GetMaxIndex(); ++Index)
			{
				if (!Values.IsValidIndex(Index)) continue;
				FString Key;
				FString Value;
				if (!AppendAuthoredValue(*Map->KeyProp, Values.GetKeyPtr(Index), Key, Depth + 1)
					|| !AppendAuthoredValue(*Map->ValueProp, Values.GetValuePtr(Index), Value, Depth + 1))
					return false;
				Items.Add(Key + TEXT("=") + Value);
			}
			Items.Sort();
			Out = TEXT("{") + FString::Join(Items, TEXT(",")) + TEXT("}");
			return true;
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			const UObject* Value = Object->GetObjectPropertyValue(Address);
			Out = Value ? Value->GetPathName() : TEXT("None");
			return true;
		}
		Property.ExportTextItem_Direct(Out, Address, nullptr, nullptr, PPF_None);
		return true;
	}

	// Hash authored editable properties, including bindings and parameter arrays.
	// Native notifications can rebuild transient MIC caches without changing these.
	FMCPToolResult CaptureState(
		const FTarget& Target,
		FString& Digest,
		TMap<FString, FString>* OutProperties = nullptr)
	{
		if (OutProperties) OutProperties->Reset();
		auto State = DescribeTarget(Target);
		// Niagara compilation can replace a renderer UObject and assign a new
		// transient object path while preserving the authored renderer slot. The
		// path remains useful in read responses and initial write plans, but it is
		// not semantic authored state and must not invalidate a receipt replay or
		// rollback after that replacement.
		State->RemoveField(TEXT("rendererPath"));
		auto Properties = MakeShared<FJsonObject>();
		int32 Characters = 0;
		for (TFieldIterator<FProperty> It(Target.Renderer->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			// bIsEnabled is intentionally exposed through the renderer alias API but
			// is not marked CPF_Edit in UE 5.4. Include that authored field in the
			// semantic snapshot so enabled mutations participate in drift detection
			// and rollback verification.
			if ((!Property->HasAnyPropertyFlags(CPF_Edit) && Property->GetFName() != TEXT("bIsEnabled"))
				|| !IsAuthoredValueProperty(*Property))
				continue;
			const void* Address = Property->ContainerPtrToValuePtr<void>(Target.Renderer);
			if (const auto* Array = CastField<FArrayProperty>(Property))
			{
				if (FScriptArrayHelper(Array, Address).Num() > MaxMaterialSlots)
					return Error(
						TEXT("Renderer arrays exceed the bounded state capture limit."), TEXT("renderer_state_limit"));
			}
			FString Value;
			if (!AppendAuthoredValue(*Property, Address, Value))
				return Error(
					TEXT("Renderer properties exceed the bounded state capture limit."), TEXT("renderer_state_limit"));
			Characters += Value.Len();
			if (Value.Len() > MaxPropertyCharacters || Characters > MaxStateCharacters)
				return Error(
					TEXT("Renderer properties exceed the bounded state capture limit."), TEXT("renderer_state_limit"));
			Properties->SetStringField(Property->GetName(), Value);
			if (OutProperties)
			{
				OutProperties->Add(Property->GetName(), Value);
			}
		}
		State->SetObjectField(TEXT("properties"), Properties);
		if (!TryDigestJson(State, Digest))
			return Error(
				TEXT("Could not hash renderer state."), TEXT("digest_unavailable"), 500);
		return FMCPToolResult::Ok(nullptr);
	}

	FString BindingText(UStruct* Struct, void* Container, const TCHAR* Name)
	{
		FProperty* Property = FindFProperty<FProperty>(Struct, Name);
		if (!Property) return FString();
		FString Text;
		Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(Container), nullptr, nullptr,
		                                PPF_None);
		return Text;
	}

	FMCPToolResult ResolveSlot(
		UNiagaraRendererProperties* Renderer,
		int32 Index,
		FMaterialSlot& Out,
		bool bAllowMissingMeshSlot = false)
	{
		UStruct* ContainerType = Renderer->GetClass();
		void* Container = Renderer;
		FString MaterialField = TEXT("Material");
		if (FArrayProperty* Array = FindFProperty<FArrayProperty>(ContainerType, TEXT("OverrideMaterials")))
		{
			FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
			FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Renderer));
			if (!Inner || Index < 0 || Values.Num() > MaxMaterialSlots)
				return Error(
					TEXT("materialSlot must select an existing mesh OverrideMaterials entry."),
					TEXT("material_slot_invalid"));
			ContainerType = Inner->Struct;
			MaterialField = TEXT("ExplicitMat");
			Out.ChangedProperty = Array;
			Out.OverrideProperty = FindFProperty<FBoolProperty>(Renderer->GetClass(), TEXT("bOverrideMaterials"));
			if (!Out.OverrideProperty)
				return Error(
					TEXT("Mesh override state is unavailable."), TEXT("renderer_unsupported"));
			Out.bOverridesEnabled = Out.OverrideProperty->GetPropertyValue_InContainer(Renderer);
			Out.BindingProperty = TEXT("UserParamBinding");
			if (Index >= Values.Num())
			{
				if (!bAllowMissingMeshSlot || Index != 0 || Values.Num() != 0)
					return Error(
						TEXT("materialSlot must select an existing mesh OverrideMaterials entry."),
						TEXT("material_slot_invalid"));
				Out.bMissingArrayEntry = true;
				Out.MaterialProperty = FindFProperty<FObjectPropertyBase>(ContainerType, *MaterialField);
				if (!Out.MaterialProperty || !Out.MaterialProperty->PropertyClass->IsChildOf(
					UMaterialInterface::StaticClass()))
					return Error(
						TEXT("This renderer does not expose a supported explicit material property."),
						TEXT("renderer_unsupported"));
				return FMCPToolResult::Ok(nullptr);
			}
			Container = Values.GetRawPtr(Index);
		}
		else
		{
			if (Index != 0) return Error(TEXT("This renderer only has materialSlot 0."), TEXT("material_slot_invalid"));
			Out.BindingProperty = FindFProperty<FProperty>(ContainerType, TEXT("MaterialUserParamBinding"))
				                      ? TEXT("MaterialUserParamBinding")
				                      : TEXT("MaterialParameterBinding");
		}
		Out.MaterialProperty = FindFProperty<FObjectPropertyBase>(ContainerType, *MaterialField);
		if (!Out.MaterialProperty || !Out.MaterialProperty->PropertyClass->IsChildOf(UMaterialInterface::StaticClass()))
			return Error(
				TEXT("This renderer does not expose a supported explicit material property."),
				TEXT("renderer_unsupported"));
		Out.MaterialAddress = Out.MaterialProperty->ContainerPtrToValuePtr<void>(Container);
		if (!Out.ChangedProperty) Out.ChangedProperty = Out.MaterialProperty;
		Out.Binding = BindingText(ContainerType, Container, *Out.BindingProperty);
		return FMCPToolResult::Ok(nullptr);
	}

	int32 SlotCount(UNiagaraRendererProperties* Renderer)
	{
		if (FArrayProperty* Array = FindFProperty<FArrayProperty>(Renderer->GetClass(), TEXT("OverrideMaterials")))
			return FScriptArrayHelper(Array, Array->ContainerPtrToValuePtr<void>(Renderer)).Num();
		return FindFProperty<FObjectPropertyBase>(Renderer->GetClass(), TEXT("Material")) ? 1 : 0;
	}

	FMCPToolResult ReadMaterials(const FTarget& Target)
	{
		FString Digest;
		FMCPToolResult Result = CaptureState(Target, Digest);
		if (!Result.bSuccess) return Result;
		auto Json = DescribeTarget(Target);
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-materials.v1"));
		Json->SetStringField(TEXT("stateDigest"), Digest);
		TArray<TSharedPtr<FJsonValue>> Slots;
		const int32 Count = SlotCount(Target.Renderer);
		if (Count > MaxMaterialSlots) return Error(TEXT("Too many material slots."), TEXT("renderer_state_limit"));
		for (int32 Index = 0; Index < Count; ++Index)
		{
			FMaterialSlot Slot;
			Result = ResolveSlot(Target.Renderer, Index, Slot);
			if (!Result.bSuccess) return Result;
			auto Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("materialSlot"), Index);
			Row->SetStringField(TEXT("explicitMaterial"), ObjectPath(Slot.GetMaterial()));
			Row->SetStringField(
				TEXT("materialClass"), Slot.GetMaterial() ? Slot.GetMaterial()->GetClass()->GetPathName() : FString());
			Row->SetBoolField(TEXT("overridesEnabled"), Slot.bOverridesEnabled);
			Row->SetStringField(TEXT("bindingProperty"), Slot.BindingProperty);
			Row->SetStringField(TEXT("bindingText"), Slot.Binding);
			Row->SetStringField(TEXT("precedence"), Slot.OverrideProperty
				                                        ? TEXT(
					                                        "When overrides are enabled: resolved user binding, then explicit material, then mesh material. Disabled overrides use the mesh material.")
				                                        : TEXT(
					                                        "A resolved material binding takes precedence over the explicit material."));
			Slots.Add(MakeShared<FJsonValueObject>(Row));
		}
		Json->SetArrayField(TEXT("materials"), Slots);
		if (FProperty* Parameters = FindFProperty<FProperty>(Target.Renderer->GetClass(), TEXT("MaterialParameters")))
		{
			const auto Value = FJsonObjectConverter::UPropertyToJsonValue(
				Parameters, Parameters->ContainerPtrToValuePtr<void>(Target.Renderer), 0, CPF_Transient);
			if (Value.IsValid()) Json->SetField(TEXT("materialParameters"), Value);
		}
		Json->SetStringField(
			TEXT("scope"),
			TEXT("authored renderer configuration; runtime binding resolution and component overrides are unverified"));
		return FMCPToolResult::Ok(Json);
	}

	FMCPToolResult BuildPlan(const TSharedPtr<FJsonObject>& Params, FPlan& Out)
	{
		FMCPToolResult Result = ResolveTarget(Params, Out.Target);
		if (!Result.bSuccess) return Result;
		if (!Owned(Out.Target))
			return Error(
				TEXT("Writes require a /Game/ System and an owned emitter renderer."), TEXT("renderer_read_only"), 409);
		if (!ReadIndex(Params, TEXT("materialSlot"), 0, MaxMaterialSlots - 1, Out.MaterialSlot))
			return Error(TEXT("materialSlot must be an integer from 0 through 255."));
		if (Params->HasField(TEXT("enableMaterialOverrides")) && !Params->TryGetBoolField(
			TEXT("enableMaterialOverrides"), Out.bEnableOverrides))
			return Error(TEXT("enableMaterialOverrides must be a boolean."));
		FString MaterialPath;
		if (!Params->TryGetStringField(TEXT("material"), MaterialPath) || MaterialPath.Len() > 2048)
			return Error(TEXT("material is required; use an empty string to clear the explicit material."));
		if (!MaterialPath.IsEmpty())
		{
			FString Package = FPackageName::ObjectPathToPackageName(MaterialPath);
			if (!FPackageName::IsValidLongPackageName(Package))
				return Error(
					TEXT("material must be a valid material package or object path."));
			if (!MaterialPath.Contains(TEXT(".")))
				MaterialPath = Package + TEXT(".") +
					FPackageName::GetShortName(Package);
			Out.Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath, nullptr, LOAD_NoWarn);
			if (!Out.Material)
				return Error(
					TEXT("The requested MaterialInterface asset was not found."), TEXT("material_not_found"), 404);
		}
		Result = ResolveSlot(Out.Target.Renderer, Out.MaterialSlot, Out.Slot, true);
		if (!Result.bSuccess) return Result;
		if (!Out.Slot.bOverridesEnabled && !Out.bEnableOverrides)
			return Error(
				TEXT(
					"Mesh material overrides are disabled; explicitly set enableMaterialOverrides=true to enable them in this plan."),
				TEXT("material_overrides_disabled"), 409);
		if (Out.bEnableOverrides && !Out.Slot.OverrideProperty)
			return Error(TEXT("enableMaterialOverrides only applies to mesh renderers."));
		Result = CaptureState(Out.Target, Out.BeforeDigest, &Out.BeforeProperties);
		if (!Result.bSuccess) return Result;
		Out.Json = DescribeTarget(Out.Target);
		Out.Json->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		Out.Json->SetStringField(TEXT("domain"), TEXT("content.niagara.renderer"));
		Out.Json->SetStringField(TEXT("planKind"), TEXT("niagaraRendererMaterial"));
		Out.Json->SetStringField(TEXT("action"), TEXT("setExplicitMaterial"));
		Out.Json->SetStringField(TEXT("status"), TEXT("planned"));
		Out.Json->SetStringField(TEXT("stateDigest"), Out.BeforeDigest);
		Out.Json->SetNumberField(TEXT("materialSlot"), Out.MaterialSlot);
		Out.Json->SetStringField(TEXT("beforeMaterial"), ObjectPath(Out.Slot.GetMaterial()));
		Out.Json->SetStringField(TEXT("afterMaterial"), ObjectPath(Out.Material));
		Out.Json->SetBoolField(TEXT("beforeSlotMissing"), Out.Slot.bMissingArrayEntry);
		Out.Json->SetBoolField(TEXT("beforeOverridesEnabled"), Out.Slot.bOverridesEnabled);
		Out.Json->SetBoolField(TEXT("afterOverridesEnabled"), Out.Slot.bOverridesEnabled || Out.bEnableOverrides);
		Out.Json->SetStringField(TEXT("bindingText"), Out.Slot.Binding);
		Out.Json->SetStringField(
			TEXT("bindingPolicy"), TEXT("preserve; a resolved binding can still override the explicit material"));
		Out.Json->SetBoolField(
			TEXT("changesState"),
			Out.Slot.GetMaterial() != Out.Material || (!Out.Slot.bOverridesEnabled && Out.bEnableOverrides));
		Out.Json->SetBoolField(TEXT("compileWillBeRequested"), Out.Json->GetBoolField(TEXT("changesState")));
		Out.Json->SetBoolField(TEXT("confirmWriteRequired"), true);
		Out.Json->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
		Out.Json->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		Out.Json->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		if (!TryDigestJson(Out.Json, Out.PlanDigest))
			return Error(
				TEXT("Could not hash renderer material plan."), TEXT("digest_unavailable"), 500);
		return FMCPToolResult::Ok(nullptr);
	}

	bool RequestDigest(const TSharedPtr<FJsonObject>& Params, FString& Out)
	{
		auto Request = MakeShared<FJsonObject>();
		for (const TCHAR* Field : {
			     TEXT("system"), TEXT("emitter"), TEXT("rendererPath"), TEXT("material"), TEXT("materialSlot"),
			     TEXT("enableMaterialOverrides")
		     })
		{
			if (const TSharedPtr<FJsonValue>* Value = Params->Values.Find(Field)) Request->SetField(Field, *Value);
		}
		return TryDigestJson(Request, Out);
	}

	bool SetOverrideState(const FTarget& Target, FBoolProperty* OverrideProperty, bool bEnabled)
	{
		if (!OverrideProperty || OverrideProperty->GetPropertyValue_InContainer(Target.Renderer) == bEnabled)
		{
			return false;
		}
		Target.Renderer->PreEditChange(OverrideProperty);
		OverrideProperty->SetPropertyValue_InContainer(Target.Renderer, bEnabled);
		FPropertyChangedEvent Event(OverrideProperty, EPropertyChangeType::ValueSet);
		Target.Renderer->PostEditChangeProperty(Event);
		return true;
	}

	FMCPToolResult ApplySlotState(
		const FTarget& Target,
		int32 MaterialSlot,
		const FMaterialSlot& InitialSlot,
		UMaterialInterface* Material,
		bool bOverridesEnabled,
		bool& bOutMutated)
	{
		bOutMutated = false;
		FMaterialSlot Slot = InitialSlot;
		if (Slot.bMissingArrayEntry && Material)
		{
			FArrayProperty* Array = CastField<FArrayProperty>(Slot.ChangedProperty);
			if (!Array || MaterialSlot != 0)
			{
				return Error(
					TEXT("The prospective mesh material slot is no longer structurally valid."),
					TEXT("material_slot_invalid"), 409);
			}
			FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Target.Renderer));
			if (Values.Num() != 0)
			{
				return Error(
					TEXT("The mesh material array changed after planning."), TEXT("renderer_state_changed"), 409);
			}
			Target.Renderer->PreEditChange(Array);
			const int32 AddedIndex = Values.AddValue();
			if (AddedIndex != MaterialSlot)
			{
				if (Values.IsValidIndex(AddedIndex))
				{
					Values.RemoveValues(AddedIndex, 1);
				}
				FPropertyChangedEvent Event(Array, EPropertyChangeType::ArrayRemove);
				Target.Renderer->PostEditChangeProperty(Event);
				return Error(
					TEXT("Could not create the requested mesh material slot."), TEXT("material_slot_create_failed"),
					500);
			}
			FMCPToolResult Result = ResolveSlot(Target.Renderer, MaterialSlot, Slot);
			if (!Result.bSuccess)
			{
				Values.RemoveValues(AddedIndex, 1);
				FPropertyChangedEvent Event(Array, EPropertyChangeType::ArrayRemove);
				Target.Renderer->PostEditChangeProperty(Event);
				return Result;
			}
			Slot.MaterialProperty->SetObjectPropertyValue(Slot.MaterialAddress, Material);
			FPropertyChangedEvent Event(Array, EPropertyChangeType::ArrayAdd);
			Target.Renderer->PostEditChangeProperty(Event);
			bOutMutated = true;
		}
		else if (!Slot.bMissingArrayEntry && Slot.GetMaterial() != Material)
		{
			Target.Renderer->PreEditChange(Slot.ChangedProperty);
			Slot.MaterialProperty->SetObjectPropertyValue(Slot.MaterialAddress, Material);
			FPropertyChangedEvent Event(Slot.ChangedProperty, EPropertyChangeType::ValueSet);
			Target.Renderer->PostEditChangeProperty(Event);
			bOutMutated = true;
		}

		bOutMutated |= SetOverrideState(Target, Slot.OverrideProperty, bOverridesEnabled);
		if (bOutMutated)
		{
			Target.System->MarkPackageDirty();
		}
		return FMCPToolResult::Ok(nullptr);
	}

	FMCPToolResult RestoreSlotState(
		const FTarget& Target,
		int32 MaterialSlot,
		bool bBeforeSlotMissing,
		UMaterialInterface* Material,
		bool bOverridesEnabled,
		bool& bOutMutated)
	{
		bOutMutated = false;
		if (bBeforeSlotMissing)
		{
			FArrayProperty* Array = FindFProperty<FArrayProperty>(Target.Renderer->GetClass(),
			                                                      TEXT("OverrideMaterials"));
			FBoolProperty* OverrideProperty = FindFProperty<FBoolProperty>(
				Target.Renderer->GetClass(), TEXT("bOverrideMaterials"));
			if (!Array || !OverrideProperty || MaterialSlot != 0)
			{
				return Error(
					TEXT("The original empty mesh material structure is unavailable."),
					TEXT("rollback_structure_unavailable"), 409);
			}
			FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(Target.Renderer));
			if (Values.Num() > 1)
			{
				return Error(
					TEXT("The mesh material array no longer matches the created slot boundary."),
					TEXT("rollback_structure_conflict"), 409);
			}
			if (Values.Num() == 1)
			{
				Target.Renderer->PreEditChange(Array);
				Values.RemoveValues(0, 1);
				FPropertyChangedEvent Event(Array, EPropertyChangeType::ArrayRemove);
				Target.Renderer->PostEditChangeProperty(Event);
				bOutMutated = true;
			}
			bOutMutated |= SetOverrideState(Target, OverrideProperty, bOverridesEnabled);
		}
		else
		{
			FMaterialSlot Slot;
			FMCPToolResult Result = ResolveSlot(Target.Renderer, MaterialSlot, Slot);
			if (!Result.bSuccess)
			{
				return Result;
			}
			Result = ApplySlotState(Target, MaterialSlot, Slot, Material, bOverridesEnabled, bOutMutated);
			if (!Result.bSuccess)
			{
				return Result;
			}
		}
		if (bOutMutated)
		{
			Target.System->MarkPackageDirty();
		}
		return FMCPToolResult::Ok(nullptr);
	}

	TSharedRef<FJsonObject> ReceiptJson(const FReceipt& Receipt, bool bReplay)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-material-receipt.v1"));
		Json->SetStringField(TEXT("status"), !Receipt.bVerified
			                                     ? TEXT("verificationFailed")
			                                     : Receipt.bRolledBack
			                                     ? TEXT("rolledBack")
			                                     : TEXT("applied"));
		Json->SetStringField(TEXT("outcome"), !Receipt.bVerified
			                                      ? TEXT("unknown")
			                                      : Receipt.bRolledBack && Receipt.bDirtyPreserved
			                                      ? TEXT("semantic_restored_dirty_preserved")
			                                      : Receipt.bRolledBack
			                                      ? TEXT("rolledBack")
			                                      : TEXT("applied"));
		Json->SetStringField(TEXT("receiptId"), Receipt.Id);
		Json->SetStringField(TEXT("rollbackId"), Receipt.Id);
		Json->SetStringField(TEXT("requestId"), Receipt.RequestId);
		Json->SetStringField(TEXT("planDigest"), Receipt.PlanDigest);
		Json->SetStringField(TEXT("system"), Receipt.SystemPath);
		Json->SetStringField(TEXT("emitterId"), Receipt.EmitterId);
		Json->SetStringField(TEXT("rendererPath"), Receipt.RendererPath);
		Json->SetNumberField(TEXT("materialSlot"), Receipt.MaterialSlot);
		Json->SetStringField(TEXT("beforeMaterial"), Receipt.BeforeMaterial);
		Json->SetStringField(TEXT("afterMaterial"), Receipt.AfterMaterial);
		Json->SetBoolField(TEXT("beforeSlotMissing"), Receipt.bBeforeSlotMissing);
		Json->SetStringField(TEXT("stateDigest"), Receipt.bRollbackAttempted && !Receipt.RestoredDigest.IsEmpty()
			                                          ? Receipt.RestoredDigest
			                                          : Receipt.AfterDigest);
		Json->SetBoolField(TEXT("changed"), Receipt.bChanged);
		Json->SetBoolField(TEXT("verified"), Receipt.bVerified);
		Json->SetStringField(TEXT("verification"), TEXT("authoredPropertyReadback"));
		Json->SetBoolField(TEXT("rollbackAttempted"), Receipt.bRollbackAttempted);
		Json->SetBoolField(TEXT("semanticRollbackVerified"), Receipt.bSemanticRollbackVerified);
		Json->SetBoolField(TEXT("dirtyBefore"), Receipt.bBeforeDirty);
		Json->SetBoolField(TEXT("dirtyAfter"), Receipt.bAfterDirty);
		Json->SetBoolField(TEXT("dirtyRestored"), Receipt.bDirtyRestored);
		Json->SetBoolField(TEXT("dirtyPreserved"), Receipt.bDirtyPreserved);
		Json->SetBoolField(TEXT("trackedStateRestored"), Receipt.bTrackedStateRestored);
		Json->SetBoolField(TEXT("fullPackageStateRestored"), Receipt.bFullPackageStateRestored);
		Json->SetStringField(TEXT("stateCoverage"), TEXT("rendererMaterial"));
		Json->SetBoolField(TEXT("packageStateVerified"), false);
		Json->SetBoolField(TEXT("saved"), false);
		Json->SetBoolField(TEXT("compiled"), false);
		Json->SetBoolField(TEXT("compileRequested"), Receipt.bCompileRequested);
		Json->SetStringField(TEXT("compileStatus"), Receipt.bCompileRequested
			                                            ? TEXT("requestedAsync; completion not awaited")
			                                            : TEXT("notRequested; authored state was unchanged"));
		Json->SetBoolField(TEXT("runtimeVerified"), false);
		Json->SetBoolField(TEXT("idempotentReplay"), bReplay);
		Json->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
		Json->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
		Json->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
		Json->SetStringField(TEXT("retention"), TEXT("boundedRecentSessionHistory"));
		Json->SetNumberField(TEXT("activeReceiptLimit"), MaxReceipts);
		Json->SetNumberField(TEXT("terminalHistoryLimit"), MaxTerminalRequestHistory);
		return Json;
	}

	TSharedRef<FJsonObject> ReleasedReceiptJson(const FString& ReceiptId, const FString& RequestId, bool bReplay)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-material-receipt-release.v1"));
		Json->SetStringField(TEXT("status"), TEXT("released"));
		Json->SetStringField(TEXT("receiptId"), ReceiptId);
		Json->SetStringField(TEXT("rollbackId"), ReceiptId);
		Json->SetStringField(TEXT("requestId"), RequestId);
		Json->SetBoolField(TEXT("released"), true);
		Json->SetBoolField(TEXT("assetMutated"), false);
		Json->SetBoolField(TEXT("idempotentReplay"), bReplay);
		Json->SetStringField(TEXT("retention"), TEXT("boundedRecentSessionHistory"));
		Json->SetNumberField(TEXT("activeReceiptLimit"), MaxReceipts);
		Json->SetNumberField(TEXT("terminalHistoryLimit"), MaxTerminalRequestHistory);
		Json->SetStringField(
			TEXT("idempotencyBoundary"),
			TEXT("recent same-Editor session history; an evicted requestId may execute again"));
		return Json;
	}

	void LogPropertyDiff(
		const TCHAR* Phase,
		const TMap<FString, FString>& Expected,
		const TMap<FString, FString>& Current)
	{
		for (const TPair<FString, FString>& Pair : Current)
		{
			const FString* ExpectedValue = Expected.Find(Pair.Key);
			if (!ExpectedValue || *ExpectedValue != Pair.Value)
			{
				UE_LOG(
					LogUEAINiagaraRendererMaterial,
					Warning,
					TEXT("%s property drift: property=%s expected=%s current=%s"),
					Phase,
					*Pair.Key,
					ExpectedValue ? **ExpectedValue : TEXT("<missing>"),
					*Pair.Value);
			}
		}
		for (const TPair<FString, FString>& Pair : Expected)
		{
			if (!Current.Contains(Pair.Key))
			{
				UE_LOG(
					LogUEAINiagaraRendererMaterial,
					Warning,
					TEXT("%s property removed: property=%s expected=%s"),
					Phase,
					*Pair.Key,
					*Pair.Value);
			}
		}
	}

	// Niagara 5.4 can replace a renderer properties UObject during an
	// asynchronous compile. A newly-created mesh renderer may also normalize
	// its default SortMode from None to ViewDepth in that replacement. Keep this
	// exception local to receipt verification; ordinary reads and all other
	// property changes retain exact authored-state semantics.
	bool IsKnownCompileNormalization(
		const TMap<FString, FString>& Expected,
		const TMap<FString, FString>& Current,
		bool bRendererReplaced)
	{
		if (!bRendererReplaced || Expected.Num() != Current.Num()) return false;
		bool bSortModeNormalized = false;
		for (const TPair<FString, FString>& Pair : Current)
		{
			const FString* ExpectedValue = Expected.Find(Pair.Key);
			if (!ExpectedValue) return false;
			if (*ExpectedValue == Pair.Value) continue;
			if (Pair.Key != TEXT("SortMode") || bSortModeNormalized
				|| *ExpectedValue != TEXT("None") || Pair.Value != TEXT("ViewDepth"))
			{
				return false;
			}
			bSortModeNormalized = true;
		}
		return bSortModeNormalized;
	}

	bool MatchReceiptStateOrCanonicalize(
		FString& ExpectedDigest,
		TMap<FString, FString>& ExpectedProperties,
		const FString& CurrentDigest,
		const TMap<FString, FString>& CurrentProperties,
		bool bRendererReplaced)
	{
		if (ExpectedDigest == CurrentDigest) return true;
		if (!IsKnownCompileNormalization(ExpectedProperties, CurrentProperties, bRendererReplaced)) return false;
		ExpectedDigest = CurrentDigest;
		ExpectedProperties = CurrentProperties;
		return true;
	}

	FMCPToolResult ReceiptTarget(const FReceipt& Receipt, FTarget& Target, bool* bOutRendererReplaced = nullptr)
	{
		// Niagara compilation may reinstantiate renderer properties. Resolve the
		// current object from the owning System and emitter identity, preferring the
		// live renderer pointer and then falling back to the authored path/index.
		// The digest checks at the call site still reject authored state drift before
		// replay or rollback.
		if (!Receipt.System.IsValid())
			return Error(
				TEXT("The receipt target no longer exists in this Editor instance."), TEXT("target_unavailable"), 409);
		UNiagaraSystem* System = Receipt.System.Get();
		FGuid EmitterGuid;
		if (!FGuid::Parse(Receipt.EmitterId, EmitterGuid))
			return Error(TEXT("The receipt emitter identity is invalid."), TEXT("target_unavailable"), 409);

		const FNiagaraEmitterHandle* Handle = nullptr;
		for (const FNiagaraEmitterHandle& Candidate : System->GetEmitterHandles())
		{
			if (Candidate.GetId() == EmitterGuid)
			{
				Handle = &Candidate;
				break;
			}
		}
		if (!Handle || !Handle->GetEmitterData())
			return Error(TEXT("The receipt emitter no longer exists."), TEXT("target_unavailable"), 409);

		UNiagaraRendererProperties* CurrentRenderer = nullptr;
		const TArray<UNiagaraRendererProperties*>& Renderers = Handle->GetEmitterData()->GetRenderers();
		if (Receipt.Renderer.IsValid())
		{
			UNiagaraRendererProperties* LiveRenderer = Receipt.Renderer.Get();
			for (UNiagaraRendererProperties* Candidate : Renderers)
			{
				if (Candidate == LiveRenderer)
				{
					CurrentRenderer = Candidate;
					break;
				}
			}
		}
		if (!CurrentRenderer && !Receipt.RendererPath.IsEmpty())
		{
			for (UNiagaraRendererProperties* Candidate : Renderers)
			{
				if (IsValid(Candidate) && Candidate->GetPathName() == Receipt.RendererPath)
				{
					CurrentRenderer = Candidate;
					break;
				}
			}
		}
		if (!CurrentRenderer && Renderers.IsValidIndex(Receipt.RendererIndex))
		{
			UNiagaraRendererProperties* Candidate = Renderers[Receipt.RendererIndex];
			if (IsValid(Candidate)
				&& (Receipt.RendererClass.IsEmpty() || Candidate->GetClass()->GetPathName() == Receipt.RendererClass))
			{
				CurrentRenderer = Candidate;
			}
		}
		if (!CurrentRenderer)
			return Error(
				TEXT("The receipt target was replaced or is no longer owned."), TEXT("target_unavailable"), 409);

		Target = MakeTarget(System, *Handle, CurrentRenderer, Renderers.IndexOfByKey(CurrentRenderer));
		if (Target.System != Receipt.System.Get() || !Owned(Target))
			return Error(
				TEXT("The receipt target was replaced or is no longer owned."), TEXT("target_unavailable"), 409);
		if (bOutRendererReplaced)
		{
			const UNiagaraRendererProperties* OriginalRenderer = Receipt.Renderer.Get();
			*bOutRendererReplaced = OriginalRenderer == nullptr || OriginalRenderer != CurrentRenderer;
		}
		return FMCPToolResult::Ok(nullptr);
	}

	class FRendererList final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.list"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			UNiagaraSystem* System = nullptr;
			FMCPToolResult Result = LoadSystem(Params, System);
			if (!Result.bSuccess) return Result;
			int32 Offset, Limit;
			if (!ReadIndex(Params, TEXT("offset"), 0, 65536, Offset) || !ReadIndex(
				Params, TEXT("limit"), 32, 128, Limit) || Limit == 0)
				return Error(TEXT("offset must be 0..65536 and limit must be 1..128."));
			FString Selector;
			const FNiagaraEmitterHandle* Selected = nullptr;
			if (Params->HasField(TEXT("emitter")))
			{
				if (!Params->TryGetStringField(TEXT("emitter"), Selector))
					return Error(
						TEXT("emitter must be a string."));
				Result = FindEmitter(System, Selector, Selected);
				if (!Result.bSuccess) return Result;
			}
			TArray<TSharedPtr<FJsonValue>> Rows;
			int32 Count = 0;
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Selected && Selected != &Handle) continue;
				const FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
				if (!Data) continue;
				const auto& Renderers = Data->GetRenderers();
				for (int32 Index = 0; Index < Renderers.Num(); ++Index)
				{
					if (!IsValid(Renderers[Index])) continue;
					if (Count >= Offset && Rows.Num() < Limit)
					{
						auto Row = DescribeTarget(MakeTarget(System, Handle, Renderers[Index], Index));
						Row->SetNumberField(TEXT("materialSlotCount"), SlotCount(Renderers[Index]));
						Rows.Add(MakeShared<FJsonValueObject>(Row));
					}
					++Count;
				}
			}
			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-list.v1"));
			Json->SetStringField(TEXT("system"), ObjectPath(System));
			Json->SetArrayField(TEXT("renderers"), Rows);
			Json->SetNumberField(TEXT("total"), Count);
			Json->SetNumberField(TEXT("offset"), Offset);
			Json->SetBoolField(TEXT("hasMore"), Offset + Rows.Num() < Count);
			Json->SetNumberField(TEXT("nextOffset"), Offset + Rows.Num());
			return FMCPToolResult::Ok(Json);
		}
	};

	class FMaterialsGet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.materials.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveReadTarget(Params, Target);
			return Result.bSuccess ? ReadMaterials(Target) : Result;
		}
	};

	class FMaterialPlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.material.plan"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FPlan Plan;
			FMCPToolResult Result = BuildPlan(Params, Plan);
			if (!Result.bSuccess) return Result;
			Plan.Json->SetStringField(TEXT("planDigest"), Plan.PlanDigest);
			return FMCPToolResult::Ok(Plan.Json);
		}
	};

	class FMaterialApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.material.apply"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId, InputDigest;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty() ||
				RequestId.Len() > 128)
				return Error(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			if (!RequestDigest(Params, InputDigest))
				return Error(
					TEXT("Could not hash request."), TEXT("digest_unavailable"), 500);
			FString Code, Message;
			if (FString* ExistingId = RequestReceipts.Find(RequestId))
			{
				FReceipt* Existing = Receipts.Find(*ExistingId);
				if (!Existing || Existing->RequestDigest != InputDigest)
					return Error(
						TEXT("requestId was already used for different material arguments."),
						TEXT("request_id_conflict"),
						409);
				if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Message))
					return Error(
						Message, *Code, 409);
				if (!Existing->bVerified)
					return Error(
						FString::Printf(
							TEXT(
								"The previous write failed verification. Inspect renderer state and receipt %s; this request will not run again."),
							*Existing->Id), TEXT("receipt_verification_failed"), 409);
				FTarget Target;
				bool bRendererReplaced = false;
				FMCPToolResult ReplayResult = ReceiptTarget(*Existing, Target, &bRendererReplaced);
				if (!ReplayResult.bSuccess)
				{
					UE_LOG(
						LogUEAINiagaraRendererMaterial,
						Error,
						TEXT("Replay target resolution failed: code=%s message=%s request=%s receipt=%s"),
						*ReplayResult.ErrorCode,
						*ReplayResult.ErrorMessage,
						*RequestId,
						*Existing->Id);
					return ReplayResult;
				}
				FString Digest;
				TMap<FString, FString> CurrentProperties;
				ReplayResult = CaptureState(Target, Digest, &CurrentProperties);
				if (!ReplayResult.bSuccess)
				{
					UE_LOG(
						LogUEAINiagaraRendererMaterial,
						Error,
						TEXT("Replay state capture failed: code=%s message=%s request=%s receipt=%s"),
						*ReplayResult.ErrorCode,
						*ReplayResult.ErrorMessage,
						*RequestId,
						*Existing->Id);
					return ReplayResult;
				}
				FString& ExpectedDigest = Existing->bRolledBack ? Existing->RestoredDigest : Existing->AfterDigest;
				TMap<FString, FString>& ExpectedProperties = Existing->bRolledBack
					                                             ? Existing->RestoredProperties
					                                             : Existing->AfterProperties;
				if (!MatchReceiptStateOrCanonicalize(
					ExpectedDigest, ExpectedProperties, Digest, CurrentProperties, bRendererReplaced))
				{
					LogPropertyDiff(
						TEXT("Replay"),
						ExpectedProperties.Num() > 0 ? ExpectedProperties : Existing->BeforeProperties,
						CurrentProperties);
					UE_LOG(
						LogUEAINiagaraRendererMaterial,
						Warning,
						TEXT("Replay state drift: current=%s expected=%s path=%s index=%d class=%s"),
						*Digest,
						*ExpectedDigest,
						*Target.Renderer->GetPathName(),
						Target.RendererIndex,
						*Target.Renderer->GetClass()->GetPathName());
					return Error(
						TEXT("Renderer state changed after this request."), TEXT("receipt_state_changed"), 409);
				}
				return FMCPToolResult::Ok(ReceiptJson(*Existing, true));
			}
			if (const FReleasedRequest* Released = ReleasedRequests.Find(RequestId))
			{
				if (Released->RequestDigest != InputDigest)
				{
					return Error(
						TEXT("requestId was already used for different material arguments."),
						TEXT("request_id_conflict"), 409);
				}
				return Error(FString::Printf(
					             TEXT(
						             "requestId was already completed and receipt %s was released; use a new requestId for another write."),
					             *Released->ReceiptId), TEXT("receipt_released"), 409);
			}
			if (Receipts.Num() >= MaxReceipts)
				return Error(
					TEXT("The session receipt limit was reached."), TEXT("receipt_limit"), 409);
			FPlan Plan;
			FMCPToolResult Result = BuildPlan(Params, Plan);
			if (!Result.bSuccess) return Result;
			if (!ValidateChangeApproval(Params, Plan.PlanDigest, Code, Message)) return Error(Message, *Code, 409);
			FReceipt Receipt;
			Receipt.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.RequestDigest = InputDigest;
			Receipt.PlanDigest = Plan.PlanDigest;
			Receipt.System = Plan.Target.System;
			Receipt.Renderer = Plan.Target.Renderer;
			Receipt.SystemPath = ObjectPath(Plan.Target.System);
			Receipt.RendererPath = ObjectPath(Plan.Target.Renderer);
			Receipt.RendererClass = Plan.Target.Renderer
				                        ? Plan.Target.Renderer->GetClass()->GetPathName()
				                        : FString();
			Receipt.RendererIndex = Plan.Target.RendererIndex;
			Receipt.EmitterId = Plan.Target.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.MaterialSlot = Plan.MaterialSlot;
			Receipt.BeforeMaterial = ObjectPath(Plan.Slot.GetMaterial());
			Receipt.AfterMaterial = ObjectPath(Plan.Material);
			Receipt.bBeforeOverrides = Plan.Slot.bOverridesEnabled;
			Receipt.bAfterOverrides = Plan.Slot.bOverridesEnabled || Plan.bEnableOverrides;
			Receipt.bBeforeSlotMissing = Plan.Slot.bMissingArrayEntry;
			Receipt.bBeforeDirty = Plan.Target.System->GetOutermost()->IsDirty();
			Receipt.BeforeDigest = Plan.BeforeDigest;
			Receipt.BeforeProperties = Plan.BeforeProperties;
			FString ReceiptBeforeDigest;
			Result = CaptureState(Plan.Target, ReceiptBeforeDigest, &Receipt.BeforeProperties);
			if (!Result.bSuccess) return Result;
			if (ReceiptBeforeDigest != Receipt.BeforeDigest)
				return Error(
					TEXT("The renderer changed after planning; create a fresh material plan."),
					TEXT("renderer_state_changed"), 409);
			Receipt.bChanged = Receipt.BeforeMaterial != Receipt.AfterMaterial || Receipt.bBeforeOverrides != Receipt.
				bAfterOverrides;
			// Reserve the request before notifications run. Even an unexpected native
			// readback failure must never turn a retry into another write.
			Receipts.Add(Receipt.Id, Receipt);
			RequestReceipts.Add(RequestId, Receipt.Id);
			UMaterialInterface* OriginalMaterial = Plan.Slot.GetMaterial();
			TUniquePtr<FScopedTransaction> Transaction;
			FMCPToolResult MutationResult = FMCPToolResult::Ok(nullptr);
			bool bMutationMade = false;
			if (Receipt.bChanged)
			{
				Transaction = MakeUnique<FScopedTransaction>(
					FText::FromString(TEXT("UE AI Set Niagara Renderer Material")));
				Plan.Target.System->Modify();
				Plan.Target.Emitter->Modify();
				Plan.Target.Renderer->Modify();
				MutationResult = ApplySlotState(
					Plan.Target,
					Plan.MaterialSlot,
					Plan.Slot,
					Plan.Material,
					Receipt.bAfterOverrides,
					bMutationMade);
				if (bMutationMade)
				{
					Plan.Target.System->RequestCompile(false);
					Receipt.bCompileRequested = true;
				}
			}
			FMaterialSlot Readback;
			Result = ResolveSlot(Plan.Target.Renderer, Plan.MaterialSlot, Readback, Plan.Slot.bMissingArrayEntry);
			const bool bExpectMissingSlot = Plan.Slot.bMissingArrayEntry && Plan.Material == nullptr;
			const bool bReadBack = MutationResult.bSuccess && Result.bSuccess
				&& Readback.bMissingArrayEntry == bExpectMissingSlot
				&& Readback.GetMaterial() == Plan.Material
				&& Readback.bOverridesEnabled == Receipt.bAfterOverrides
				&& (Plan.Slot.bMissingArrayEntry || Readback.Binding == Plan.Slot.Binding);
			Receipt.bAfterDirty = Plan.Target.System->GetOutermost()->IsDirty();
			const FMCPToolResult AfterStateResult = CaptureState(Plan.Target, Receipt.AfterDigest,
			                                                     &Receipt.AfterProperties);
			if (!bReadBack || !AfterStateResult.bSuccess)
			{
				Receipt.bRollbackAttempted = true;
				bool bRestoreMutationMade = false;
				const FMCPToolResult RestoreResult = RestoreSlotState(
					Plan.Target,
					Plan.MaterialSlot,
					Receipt.bBeforeSlotMissing,
					OriginalMaterial,
					Receipt.bBeforeOverrides,
					bRestoreMutationMade);
				if (bRestoreMutationMade)
				{
					Plan.Target.System->RequestCompile(false);
					Receipt.bCompileRequested = true;
				}
				const FMCPToolResult Restored = CaptureState(Plan.Target, Receipt.RestoredDigest,
				                                             &Receipt.RestoredProperties);
				Receipt.bSemanticRollbackVerified = Restored.bSuccess && Receipt.RestoredDigest == Receipt.BeforeDigest;
				if (Receipt.bSemanticRollbackVerified)
				{
					RestoreDirtyState(Plan.Target.System->GetOutermost(), Receipt.bBeforeDirty, true, Receipt);
				}
				Receipt.bRolledBack = Receipt.bSemanticRollbackVerified;
				Receipt.bVerified = Receipt.bSemanticRollbackVerified;
				if (Receipt.bSemanticRollbackVerified)
				{
					if (Transaction) Transaction->Cancel();
				}
				Receipts.Add(Receipt.Id, Receipt);
				return Error(FString::Printf(
					             TEXT(
						             "Material write verification failed; receipt %s records semanticRollback=%s, dirtyRestored=%s, restoreOperation=%s. Unverified changes retain the transaction for Editor Undo."),
					             *Receipt.Id,
					             Receipt.bSemanticRollbackVerified ? TEXT("verified") : TEXT("unverified"),
					             Receipt.bDirtyRestored ? TEXT("true") : TEXT("false"),
					             RestoreResult.bSuccess ? TEXT("completed") : TEXT("failed")),
				             TEXT("material_verification_failed"), 500);
			}
			Receipt.bVerified = true;
			Receipts.Add(Receipt.Id, Receipt);
			return FMCPToolResult::Ok(ReceiptJson(Receipt, false));
		}
	};

	class FMaterialRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.material.rollback"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Id, RequestId;
			bool bConfirm = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), Id) || !Params->
				TryGetStringField(TEXT("requestId"), RequestId) || !Params->TryGetBoolField(
					TEXT("confirmWrite"), bConfirm) || !bConfirm)
				return Error(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"));
			FReceipt* Receipt = Receipts.Find(Id);
			if (!Receipt)
				return Error(
					TEXT("The receipt is unavailable in this Editor instance."), TEXT("receipt_not_found"), 404);
			if (Receipt->RequestId != RequestId)
				return Error(
					TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			if (!Receipt->bVerified)
				return Error(
					TEXT(
						"The original write has no verified state for automatic rollback; use Editor Undo after inspection."),
					TEXT("receipt_verification_failed"), 409);
			FTarget Target;
			bool bRendererReplaced = false;
			FMCPToolResult Result = ReceiptTarget(*Receipt, Target, &bRendererReplaced);
			if (!Result.bSuccess)
			{
				UE_LOG(LogUEAINiagaraRendererMaterial, Error,
				       TEXT("Rollback target resolution failed: code=%s message=%s request=%s receipt=%s"),
				       *Result.ErrorCode, *Result.ErrorMessage, *RequestId, *Receipt->Id);
				return Result;
			}
			FString CurrentDigest;
			TMap<FString, FString> CurrentProperties;
			Result = CaptureState(Target, CurrentDigest, &CurrentProperties);
			if (!Result.bSuccess)
			{
				UE_LOG(LogUEAINiagaraRendererMaterial, Error,
				       TEXT("Rollback state capture failed: code=%s message=%s request=%s receipt=%s"),
				       *Result.ErrorCode, *Result.ErrorMessage, *RequestId, *Receipt->Id);
				return Result;
			}
			FString& ExpectedDigest = Receipt->bRolledBack ? Receipt->RestoredDigest : Receipt->AfterDigest;
			TMap<FString, FString>& ExpectedProperties = Receipt->bRolledBack
				                                             ? Receipt->RestoredProperties
				                                             : Receipt->AfterProperties;
			if (!MatchReceiptStateOrCanonicalize(
				ExpectedDigest,
				ExpectedProperties,
				CurrentDigest,
				CurrentProperties,
				bRendererReplaced))
			{
				LogPropertyDiff(
					TEXT("Rollback"),
					ExpectedProperties.Num() > 0 ? ExpectedProperties : Receipt->BeforeProperties,
					CurrentProperties);
				UE_LOG(LogUEAINiagaraRendererMaterial, Warning,
				       TEXT("Rollback state drift: current=%s expected=%s path=%s index=%d class=%s"),
				       *CurrentDigest,
				       *ExpectedDigest,
				       *Target.Renderer->GetPathName(), Target.RendererIndex,
				       *Target.Renderer->GetClass()->GetPathName());
				return Error(
					TEXT("The renderer changed after apply; rollback was refused."), TEXT("rollback_conflict"), 409);
			}
			if (Receipt->bRolledBack) return FMCPToolResult::Ok(ReceiptJson(*Receipt, true));
			UMaterialInterface* Before = Receipt->BeforeMaterial.IsEmpty()
				                             ? nullptr
				                             : LoadObject<UMaterialInterface>(
					                             nullptr, *Receipt->BeforeMaterial, nullptr, LOAD_NoWarn);
			if (!Receipt->BeforeMaterial.IsEmpty() && !Before)
				return Error(
					TEXT("The original material is no longer available."), TEXT("rollback_material_unavailable"), 409);
			TUniquePtr<FScopedTransaction> Transaction;
			Receipt->bRollbackAttempted = true;
			Receipt->bVerified = false;
			bool bMutationMade = false;
			FMCPToolResult RestoreResult = FMCPToolResult::Ok(nullptr);
			if (Receipt->bChanged)
			{
				Transaction = MakeUnique<FScopedTransaction>(
					FText::FromString(TEXT("UE AI Rollback Niagara Renderer Material")));
				Target.System->Modify();
				Target.Emitter->Modify();
				Target.Renderer->Modify();
				RestoreResult = RestoreSlotState(
					Target,
					Receipt->MaterialSlot,
					Receipt->bBeforeSlotMissing,
					Before,
					Receipt->bBeforeOverrides,
					bMutationMade);
				if (bMutationMade)
				{
					Target.System->RequestCompile(false);
					Receipt->bCompileRequested = true;
				}
			}
			Result = CaptureState(Target, Receipt->RestoredDigest, &Receipt->RestoredProperties);
			Receipt->bSemanticRollbackVerified = Result.bSuccess
				&& MatchReceiptStateOrCanonicalize(
					Receipt->BeforeDigest,
					Receipt->BeforeProperties,
					Receipt->RestoredDigest,
					Receipt->RestoredProperties,
					bRendererReplaced);
			if (Receipt->bSemanticRollbackVerified)
			{
				RestoreDirtyState(Target.System->GetOutermost(), Receipt->bBeforeDirty, false, *Receipt);
			}
			Receipt->bRolledBack = Receipt->bSemanticRollbackVerified;
			Receipt->bVerified = Receipt->bSemanticRollbackVerified;
			if (!Receipt->bSemanticRollbackVerified)
			{
				return Error(FString::Printf(
					             TEXT(
						             "Rollback outcome is unknown; semanticRollback=%s, dirtyRestored=%s, restoreOperation=%s. Use the retained Editor transaction and inspect the renderer before another write."),
					             Receipt->bSemanticRollbackVerified ? TEXT("verified") : TEXT("unverified"),
					             Receipt->bDirtyRestored ? TEXT("true") : TEXT("false"),
					             RestoreResult.bSuccess ? TEXT("completed") : TEXT("failed")),
				             TEXT("rollback_verification_failed"), 500);
			}
			return FMCPToolResult::Ok(ReceiptJson(*Receipt, false));
		}
	};

	class FMaterialReceiptRelease final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.material.receipt.release"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Id, RequestId;
			bool bConfirmWrite = false;
			bool bConfirmDiscardRollback = false;
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("rollbackId"), Id) || Id.IsEmpty() || Id.Len() > 128
				|| !Params->TryGetStringField(TEXT("requestId"), RequestId) || RequestId.IsEmpty() || RequestId.Len() >
				128
				|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite) || !bConfirmWrite)
			{
				return Error(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"));
			}
			if (Params->HasField(TEXT("confirmDiscardRollback"))
				&& !Params->TryGetBoolField(TEXT("confirmDiscardRollback"), bConfirmDiscardRollback))
			{
				return Error(TEXT("confirmDiscardRollback must be a boolean when provided."));
			}

			FReceipt* Receipt = Receipts.Find(Id);
			if (!Receipt)
			{
				const FReleasedRequest* Released = ReleasedRequests.Find(RequestId);
				if (Released && Released->ReceiptId == Id)
				{
					return FMCPToolResult::Ok(ReleasedReceiptJson(Id, RequestId, true));
				}
				if (Released)
				{
					return Error(
						TEXT("requestId does not match the released receipt."), TEXT("request_id_mismatch"), 409);
				}
				return Error(
					TEXT("The receipt is unavailable in recent history for this Editor instance."),
					TEXT("receipt_not_found"), 404);
			}
			if (Receipt->RequestId != RequestId)
			{
				return Error(TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			}
			if (Receipt->bChanged && !Receipt->bRolledBack && !bConfirmDiscardRollback)
			{
				return Error(
					TEXT(
						"This changed receipt still owns a same-Editor rollback boundary; set confirmDiscardRollback=true to release it."),
					TEXT("rollback_discard_confirmation_required"), 409);
			}

			const TSharedRef<FJsonObject> Json = ReleasedReceiptJson(Receipt->Id, Receipt->RequestId, false);
			const FString ActiveRequestId = Receipt->RequestId;
			ReleasedRequests.Remember(Receipt->RequestId, Receipt->Id, Receipt->RequestDigest);
			RequestReceipts.Remove(ActiveRequestId);
			Receipts.Remove(Id);
			return FMCPToolResult::Ok(Json);
		}
	};

	// ---------------------------------------------------------------------------
	// Renderer structure and attribute-binding authoring.
	//
	// Non-runtime boundaries (house rules):
	//  - Writes require a non-transient /Game/ Niagara System with an owned emitter
	//    renderer. Edits are authored-only: dirty-only, never saved here, and any
	//    requested compile is asynchronous and never awaited.
	//  - Read queries never compile or save; compiled / runtimeVerified are false.
	//  - property.set uses a bounded, enumerated property whitelist (never generic
	//    arbitrary-UProperty reflection); bindings.set only mutates the renderer's
	//    declared FNiagaraVariableAttributeBinding properties.
	// ---------------------------------------------------------------------------

	constexpr int32 MaxRendererClassCharacters = 256;
	constexpr int32 MaxRendererPropertyCharacters = 256;
	constexpr int32 MaxBindingsPerRequest = 64;
	constexpr int32 MaxBindingCharacters = 512;

	FString NormalizeRendererClassName(FString ClassName)
	{
		ClassName.TrimStartAndEndInline();
		if (ClassName.StartsWith(TEXT("UNiagara"), ESearchCase::IgnoreCase))
		{
			ClassName.RightChopInline(8, EAllowShrinking::No);
		}
		else if (ClassName.StartsWith(TEXT("Niagara"), ESearchCase::IgnoreCase))
		{
			ClassName.RightChopInline(7, EAllowShrinking::No);
		}
		if (ClassName.EndsWith(TEXT("RendererProperties"), ESearchCase::IgnoreCase))
		{
			ClassName.LeftChopInline(18, EAllowShrinking::No);
		}
		else if (ClassName.EndsWith(TEXT("Renderer"), ESearchCase::IgnoreCase))
		{
			ClassName.LeftChopInline(8, EAllowShrinking::No);
		}
		return ClassName.ToLower();
	}

	UClass* ResolveRendererClass(const FString& ClassName)
	{
		const FString Normalized = NormalizeRendererClassName(ClassName);
		if (Normalized == TEXT("sprite")) return UNiagaraSpriteRendererProperties::StaticClass();
		if (Normalized == TEXT("mesh")) return UNiagaraMeshRendererProperties::StaticClass();
		if (Normalized == TEXT("ribbon")) return UNiagaraRibbonRendererProperties::StaticClass();
		if (Normalized == TEXT("light")) return UNiagaraLightRendererProperties::StaticClass();
		if (Normalized == TEXT("component")) return UNiagaraComponentRendererProperties::StaticClass();
		if (Normalized == TEXT("decal")) return UNiagaraDecalRendererProperties::StaticClass();
		if (Normalized == TEXT("volume")) return UNiagaraVolumeRendererProperties::StaticClass();
		return nullptr;
	}

	FString SupportedRendererClasses()
	{
		return TEXT("sprite, mesh, ribbon, light, component, decal, volume");
	}

	bool SystemWritable(const UNiagaraSystem* System)
	{
		return System
			&& System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))
			&& !System->HasAnyFlags(RF_Transient);
	}

	enum class ERendererPropertyKey : uint8
	{
		Unknown,
		Enabled,
		SortOrderHint,
		SortMode,
		FacingMode,
		Alignment,
		SubImageSize,
		SubImageBlend,
		Shape,
		TessellationMode,
		TessellationFactor,
		TubeSubdivisions,
	};

	ERendererPropertyKey ResolveRendererPropertyKey(const FString& Key)
	{
		const FString Normalized = Key.TrimStartAndEnd().ToLower();
		if (Normalized == TEXT("enabled")) return ERendererPropertyKey::Enabled;
		if (Normalized == TEXT("sortorderhint") || Normalized == TEXT("sort_order_hint"))
			return
				ERendererPropertyKey::SortOrderHint;
		if (Normalized == TEXT("sortmode") || Normalized == TEXT("sort_mode")) return ERendererPropertyKey::SortMode;
		if (Normalized == TEXT("facingmode") || Normalized == TEXT("facing_mode"))
			return
				ERendererPropertyKey::FacingMode;
		if (Normalized == TEXT("alignment")) return ERendererPropertyKey::Alignment;
		if (Normalized == TEXT("subimagesize") || Normalized == TEXT("sub_image_size"))
			return
				ERendererPropertyKey::SubImageSize;
		if (Normalized == TEXT("subimageblend") || Normalized == TEXT("sub_image_blend"))
			return
				ERendererPropertyKey::SubImageBlend;
		if (Normalized == TEXT("shape")) return ERendererPropertyKey::Shape;
		if (Normalized == TEXT("tessellationmode") || Normalized == TEXT("tessellation_mode"))
			return
				ERendererPropertyKey::TessellationMode;
		if (Normalized == TEXT("tessellationfactor") || Normalized == TEXT("tessellation_factor"))
			return
				ERendererPropertyKey::TessellationFactor;
		if (Normalized == TEXT("tubesubdivisions") || Normalized == TEXT("tube_subdivisions"))
			return
				ERendererPropertyKey::TubeSubdivisions;
		return ERendererPropertyKey::Unknown;
	}

	FString SupportedRendererProperties()
	{
		return TEXT(
			"enabled, sortOrderHint, sortMode, facingMode, alignment, subImageSize, subImageBlend, shape, tessellationMode, tessellationFactor, tubeSubdivisions");
	}

	bool ReadBoolValue(const TSharedPtr<FJsonValue>& Value, bool& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Boolean) return false;
		Out = Value->AsBool();
		return true;
	}

	bool ReadInt32Value(const TSharedPtr<FJsonValue>& Value, int32& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Number) return false;
		const double Number = Value->AsNumber();
		if (!FMath::IsFinite(Number) || Number != FMath::FloorToDouble(Number)
			|| Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
		{
			return false;
		}
		Out = static_cast<int32>(Number);
		return true;
	}

	bool ReadStringValue(const TSharedPtr<FJsonValue>& Value, FString& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::String) return false;
		Out = Value->AsString();
		return !Out.IsEmpty();
	}

	bool ReadVector2DValue(const TSharedPtr<FJsonValue>& Value, FVector2D& Out)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object) return false;
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		if (!Object.IsValid()) return false;
		double X = 0.0, Y = 0.0;
		if (!Object->TryGetNumberField(TEXT("x"), X) || !Object->TryGetNumberField(TEXT("y"), Y)) return false;
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y)) return false;
		Out = FVector2D(X, Y);
		return true;
	}

	template <typename TEnum>
	bool ParseEnumValue(const FString& Name, TEnum& Out)
	{
		UEnum* Enum = StaticEnum<TEnum>();
		if (!Enum) return false;
		const FString Clean = Name.TrimStartAndEnd();
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			FString Entry = Enum->GetNameStringByIndex(Index);
			int32 Colon = INDEX_NONE;
			if (Entry.FindLastChar(TEXT(':'), Colon))
			{
				Entry.RightChopInline(Colon + 1, EAllowShrinking::No);
			}
			if (Entry.Equals(Clean, ESearchCase::IgnoreCase))
			{
				Out = static_cast<TEnum>(Enum->GetValueByIndex(Index));
				return true;
			}
		}
		return false;
	}

	// Wrap a direct authored member edit in the renderer's native property
	// notification so derived data stays consistent with the Editor's path.
	template <typename TSetFn>
	FMCPToolResult ApplyRendererPropertyEdit(UNiagaraRendererProperties* Renderer, const TCHAR* PropertyName,
	                                         TSetFn&& SetFn)
	{
		if (!Renderer || !PropertyName)
			return Error(TEXT("The renderer property is unavailable."), TEXT("property_unsupported_for_renderer"));
		FProperty* Property = FindFProperty<FProperty>(Renderer->GetClass(), PropertyName);
		if (!Property)
			return Error(TEXT("The renderer property is unavailable."), TEXT("property_unsupported_for_renderer"));
#if WITH_EDITOR
		if (!Renderer->CanEditChange(Property))
			return Error(
				FString::Printf(TEXT("Renderer property '%s' cannot be edited in its current state."), PropertyName),
				TEXT("property_not_editable"), 409);
#endif
		if (Property)
		{
			Renderer->PreEditChange(Property);
		}
		SetFn();
		if (Property)
		{
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Renderer->PostEditChangeProperty(Event);
		}
		return FMCPToolResult::Ok(nullptr);
	}

	template <typename TRenderer, typename TEnum>
	FMCPToolResult SetRendererEnum(
		UNiagaraRendererProperties* Renderer,
		const TCHAR* PropertyName,
		const TCHAR* KeyName,
		const TSharedPtr<FJsonValue>& Value,
		TEnum TRenderer::* Member,
		bool& bOutChanged)
	{
		FString Text;
		if (!ReadStringValue(Value, Text))
		{
			return Error(FString::Printf(TEXT("%s expects an enum name string."), KeyName),
			             TEXT("property_value_invalid"));
		}
		TEnum Parsed{};
		if (!ParseEnumValue(Text, Parsed))
		{
			return Error(FString::Printf(TEXT("Unsupported %s value '%s'."), KeyName, *Text),
			             TEXT("property_value_invalid"));
		}
		TRenderer* Concrete = Cast<TRenderer>(Renderer);
		if (!Concrete)
		{
			return Error(FString::Printf(TEXT("%s does not apply to this renderer class."), KeyName),
			             TEXT("property_unsupported_for_renderer"));
		}
		if (Concrete->*Member == Parsed)
		{
			return FMCPToolResult::Ok(nullptr);
		}
		FMCPToolResult EditResult = ApplyRendererPropertyEdit(Renderer, PropertyName, [&]()
		{
			Concrete->*Member = Parsed;
		});
		if (!EditResult.bSuccess) return EditResult;
		bOutChanged = true;
		return FMCPToolResult::Ok(nullptr);
	}

	template <typename TRenderer>
	FMCPToolResult SetRendererInt32(
		UNiagaraRendererProperties* Renderer,
		const TCHAR* PropertyName,
		const TCHAR* KeyName,
		const TSharedPtr<FJsonValue>& Value,
		int32 TRenderer::* Member,
		bool& bOutChanged)
	{
		int32 Parsed = 0;
		if (!ReadInt32Value(Value, Parsed))
		{
			return Error(FString::Printf(TEXT("%s expects an integer value."), KeyName),
			             TEXT("property_value_invalid"));
		}
		TRenderer* Concrete = Cast<TRenderer>(Renderer);
		if (!Concrete)
		{
			return Error(FString::Printf(TEXT("%s does not apply to this renderer class."), KeyName),
			             TEXT("property_unsupported_for_renderer"));
		}
		if (Concrete->*Member == Parsed)
		{
			return FMCPToolResult::Ok(nullptr);
		}
		FMCPToolResult EditResult = ApplyRendererPropertyEdit(Renderer, PropertyName, [&]()
		{
			Concrete->*Member = Parsed;
		});
		if (!EditResult.bSuccess) return EditResult;
		bOutChanged = true;
		return FMCPToolResult::Ok(nullptr);
	}

	FMCPToolResult SetRendererPropertyValue(
		UNiagaraRendererProperties* Renderer,
		ERendererPropertyKey Key,
		const TSharedPtr<FJsonValue>& Value,
		bool& bOutChanged)
	{
		bOutChanged = false;
		switch (Key)
		{
		case ERendererPropertyKey::Enabled:
			{
				FProperty* EnabledProperty = FindFProperty<FProperty>(Renderer->GetClass(), TEXT("bIsEnabled"));
				if (!EnabledProperty)
					return Error(
						TEXT("enabled is unavailable for this renderer class."),
						TEXT("property_unsupported_for_renderer"));
#if WITH_EDITOR
				if (!Renderer->CanEditChange(EnabledProperty))
					return Error(
						TEXT("enabled cannot be edited in the renderer's current state."),
						TEXT("property_not_editable"), 409);
#endif
				bool bEnabled = false;
				if (!ReadBoolValue(Value, bEnabled))
				{
					return Error(TEXT("enabled expects a boolean value."), TEXT("property_value_invalid"));
				}
				bOutChanged = Renderer->GetIsEnabled() != bEnabled;
				if (bOutChanged)
				{
					Renderer->SetIsEnabled(bEnabled);
				}
				return FMCPToolResult::Ok(nullptr);
			}
		case ERendererPropertyKey::SortOrderHint:
			{
				int32 Hint = 0;
				if (!ReadInt32Value(Value, Hint))
				{
					return Error(TEXT("sortOrderHint expects an integer value."), TEXT("property_value_invalid"));
				}
				if (Renderer->SortOrderHint == Hint)
				{
					return FMCPToolResult::Ok(nullptr);
				}
				FMCPToolResult EditResult = ApplyRendererPropertyEdit(
					Renderer, TEXT("SortOrderHint"), [&]() { Renderer->SortOrderHint = Hint; });
				if (!EditResult.bSuccess) return EditResult;
				bOutChanged = true;
				return FMCPToolResult::Ok(nullptr);
			}
		case ERendererPropertyKey::SortMode:
			{
				if (Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					return SetRendererEnum<UNiagaraSpriteRendererProperties, ENiagaraSortMode>(
						Renderer, TEXT("SortMode"), TEXT("sortMode"), Value,
						&UNiagaraSpriteRendererProperties::SortMode, bOutChanged);
				}
				if (Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					return SetRendererEnum<UNiagaraMeshRendererProperties, ENiagaraSortMode>(
						Renderer, TEXT("SortMode"), TEXT("sortMode"), Value, &UNiagaraMeshRendererProperties::SortMode,
						bOutChanged);
				}
				return Error(
					TEXT("sortMode does not apply to this renderer class."), TEXT("property_unsupported_for_renderer"));
			}
		case ERendererPropertyKey::FacingMode:
			{
				if (Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					return SetRendererEnum<UNiagaraSpriteRendererProperties, ENiagaraSpriteFacingMode>(
						Renderer, TEXT("FacingMode"), TEXT("facingMode"), Value,
						&UNiagaraSpriteRendererProperties::FacingMode, bOutChanged);
				}
				if (Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					return SetRendererEnum<UNiagaraMeshRendererProperties, ENiagaraMeshFacingMode>(
						Renderer, TEXT("FacingMode"), TEXT("facingMode"), Value,
						&UNiagaraMeshRendererProperties::FacingMode, bOutChanged);
				}
				if (Cast<UNiagaraRibbonRendererProperties>(Renderer))
				{
					return SetRendererEnum<UNiagaraRibbonRendererProperties, ENiagaraRibbonFacingMode>(
						Renderer, TEXT("FacingMode"), TEXT("facingMode"), Value,
						&UNiagaraRibbonRendererProperties::FacingMode, bOutChanged);
				}
				return Error(
					TEXT("facingMode does not apply to this renderer class."),
					TEXT("property_unsupported_for_renderer"));
			}
		case ERendererPropertyKey::Alignment:
			{
				return SetRendererEnum<UNiagaraSpriteRendererProperties, ENiagaraSpriteAlignment>(
					Renderer, TEXT("Alignment"), TEXT("alignment"), Value, &UNiagaraSpriteRendererProperties::Alignment,
					bOutChanged);
			}
		case ERendererPropertyKey::SubImageSize:
			{
				FVector2D Size;
				if (!ReadVector2DValue(Value, Size))
				{
					return Error(TEXT("subImageSize expects a numeric {x, y} object."), TEXT("property_value_invalid"));
				}
				if (auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					if (Sprite->SubImageSize == Size) return FMCPToolResult::Ok(nullptr);
					FMCPToolResult EditResult = ApplyRendererPropertyEdit(
						Renderer, TEXT("SubImageSize"), [&]() { Sprite->SubImageSize = Size; });
					if (!EditResult.bSuccess) return EditResult;
					bOutChanged = true;
					return FMCPToolResult::Ok(nullptr);
				}
				if (auto* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					if (Mesh->SubImageSize == Size) return FMCPToolResult::Ok(nullptr);
					FMCPToolResult EditResult = ApplyRendererPropertyEdit(
						Renderer, TEXT("SubImageSize"), [&]() { Mesh->SubImageSize = Size; });
					if (!EditResult.bSuccess) return EditResult;
					bOutChanged = true;
					return FMCPToolResult::Ok(nullptr);
				}
				return Error(
					TEXT("subImageSize does not apply to this renderer class."),
					TEXT("property_unsupported_for_renderer"));
			}
		case ERendererPropertyKey::SubImageBlend:
			{
				bool bBlend = false;
				if (!ReadBoolValue(Value, bBlend))
				{
					return Error(TEXT("subImageBlend expects a boolean value."), TEXT("property_value_invalid"));
				}
				const uint8 BlendByte = bBlend ? 1 : 0;
				if (auto* Sprite = Cast<UNiagaraSpriteRendererProperties>(Renderer))
				{
					if (Sprite->bSubImageBlend == BlendByte) return FMCPToolResult::Ok(nullptr);
					FMCPToolResult EditResult = ApplyRendererPropertyEdit(
						Renderer, TEXT("bSubImageBlend"), [&]() { Sprite->bSubImageBlend = BlendByte; });
					if (!EditResult.bSuccess) return EditResult;
					bOutChanged = true;
					return FMCPToolResult::Ok(nullptr);
				}
				if (auto* Mesh = Cast<UNiagaraMeshRendererProperties>(Renderer))
				{
					if (Mesh->bSubImageBlend == BlendByte) return FMCPToolResult::Ok(nullptr);
					FMCPToolResult EditResult = ApplyRendererPropertyEdit(
						Renderer, TEXT("bSubImageBlend"), [&]() { Mesh->bSubImageBlend = BlendByte; });
					if (!EditResult.bSuccess) return EditResult;
					bOutChanged = true;
					return FMCPToolResult::Ok(nullptr);
				}
				return Error(
					TEXT("subImageBlend does not apply to this renderer class."),
					TEXT("property_unsupported_for_renderer"));
			}
		case ERendererPropertyKey::Shape:
			{
				return SetRendererEnum<UNiagaraRibbonRendererProperties, ENiagaraRibbonShapeMode>(
					Renderer, TEXT("Shape"), TEXT("shape"), Value, &UNiagaraRibbonRendererProperties::Shape,
					bOutChanged);
			}
		case ERendererPropertyKey::TessellationMode:
			{
				return SetRendererEnum<UNiagaraRibbonRendererProperties, ENiagaraRibbonTessellationMode>(
					Renderer, TEXT("TessellationMode"), TEXT("tessellationMode"), Value,
					&UNiagaraRibbonRendererProperties::TessellationMode, bOutChanged);
			}
		case ERendererPropertyKey::TessellationFactor:
			{
				return SetRendererInt32<UNiagaraRibbonRendererProperties>(
					Renderer, TEXT("TessellationFactor"), TEXT("tessellationFactor"), Value,
					&UNiagaraRibbonRendererProperties::TessellationFactor, bOutChanged);
			}
		case ERendererPropertyKey::TubeSubdivisions:
			{
				return SetRendererInt32<UNiagaraRibbonRendererProperties>(
					Renderer, TEXT("TubeSubdivisions"), TEXT("tubeSubdivisions"), Value,
					&UNiagaraRibbonRendererProperties::TubeSubdivisions, bOutChanged);
			}
		default:
			return Error(TEXT("Unsupported renderer property."), TEXT("renderer_property_unsupported"));
		}
	}

	TSharedPtr<FJsonValue> SerializeRendererPropertyValue(FProperty& Property, const void* Address)
	{
		if (!Address) return nullptr;
		return FJsonObjectConverter::UPropertyToJsonValue(&Property, Address, 0, CPF_Transient);
	}

	FProperty* FindKnownRendererProperty(UNiagaraRendererProperties* Renderer, ERendererPropertyKey Key)
	{
		if (!Renderer) return nullptr;
		const TCHAR* PropertyName = nullptr;
		switch (Key)
		{
		case ERendererPropertyKey::Enabled: PropertyName = TEXT("bIsEnabled");
			break;
		case ERendererPropertyKey::SortOrderHint: PropertyName = TEXT("SortOrderHint");
			break;
		case ERendererPropertyKey::SortMode: PropertyName = TEXT("SortMode");
			break;
		case ERendererPropertyKey::FacingMode: PropertyName = TEXT("FacingMode");
			break;
		case ERendererPropertyKey::Alignment: PropertyName = TEXT("Alignment");
			break;
		case ERendererPropertyKey::SubImageSize: PropertyName = TEXT("SubImageSize");
			break;
		case ERendererPropertyKey::SubImageBlend: PropertyName = TEXT("bSubImageBlend");
			break;
		case ERendererPropertyKey::Shape: PropertyName = TEXT("Shape");
			break;
		case ERendererPropertyKey::TessellationMode: PropertyName = TEXT("TessellationMode");
			break;
		case ERendererPropertyKey::TessellationFactor: PropertyName = TEXT("TessellationFactor");
			break;
		case ERendererPropertyKey::TubeSubdivisions: PropertyName = TEXT("TubeSubdivisions");
			break;
		default: break;
		}
		return PropertyName ? FindFProperty<FProperty>(Renderer->GetClass(), PropertyName) : nullptr;
	}

	TSharedPtr<FJsonValue> ReadKnownRendererProperty(UNiagaraRendererProperties* Renderer, ERendererPropertyKey Key)
	{
		if (Key == ERendererPropertyKey::Enabled)
			return MakeShared<FJsonValueBoolean>(Renderer->GetIsEnabled());
		FProperty* Property = FindKnownRendererProperty(Renderer, Key);
		return Property
			       ? SerializeRendererPropertyValue(*Property, Property->ContainerPtrToValuePtr<void>(Renderer))
			       : nullptr;
	}

	// ImportText is used only to restore one of the explicit properties returned
	// by FindKnownRendererProperty; it is not an input path for arbitrary UProperty
	// reflection or user-selected container/object values.
	bool RestoreKnownRendererPropertyFromText(
		UNiagaraRendererProperties* Renderer,
		ERendererPropertyKey Key,
		const FString& ExportedValue)
	{
		FProperty* Property = FindKnownRendererProperty(Renderer, Key);
		if (!Property || ExportedValue.IsEmpty()) return false;
		void* Address = Property->ContainerPtrToValuePtr<void>(Renderer);
		const TCHAR* ImportEnd = nullptr;
		FMCPToolResult EditResult = ApplyRendererPropertyEdit(Renderer, *Property->GetName(), [&]()
		{
			ImportEnd = Property->ImportText_Direct(*ExportedValue, Address, Renderer, PPF_None);
		});
		if (!EditResult.bSuccess || !ImportEnd) return false;
		while (*ImportEnd && FChar::IsWhitespace(*ImportEnd)) ++ImportEnd;
		return *ImportEnd == TEXT('\0');
	}

	bool ValidateRendererIntegerRange(
		const ERendererPropertyKey Key,
		const TSharedPtr<FJsonValue>& Value)
	{
		if (Key != ERendererPropertyKey::TessellationFactor
			&& Key != ERendererPropertyKey::TubeSubdivisions)
		{
			return true;
		}

		int32 Parsed = 0;
		if (!ReadInt32Value(Value, Parsed))
		{
			return false;
		}
		const int32 Minimum = Key == ERendererPropertyKey::TubeSubdivisions ? 3 : 1;
		return Parsed >= Minimum && Parsed <= 16;
	}

	FMCPToolResult ReadBindings(const FTarget& Target)
	{
		auto Json = DescribeTarget(Target);
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-bindings.v1"));
		const UScriptStruct* BindingStruct = FNiagaraVariableAttributeBinding::StaticStruct();
		TArray<TSharedPtr<FJsonValue>> Rows;
		for (TFieldIterator<FProperty> It(Target.Renderer->GetClass()); It; ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
			FStructProperty* StructProperty = CastField<FStructProperty>(*It);
			if (!StructProperty || StructProperty->Struct != BindingStruct) continue;
			const FNiagaraVariableAttributeBinding* Binding =
				StructProperty->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Target.Renderer);
			const FNiagaraVariableBase& Bound = Binding->GetParamMapBindableVariable();
			const FName BoundName = Bound.GetName();
			auto Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), StructProperty->GetName());
			Row->SetStringField(TEXT("boundTo"), BoundName.IsNone() ? TEXT("(unbound)") : BoundName.ToString());
			Row->SetStringField(TEXT("type"), Bound.GetType().GetName());
			Row->SetBoolField(TEXT("valid"), Binding->IsValid());
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		Json->SetArrayField(TEXT("bindings"), Rows);
		Json->SetBoolField(TEXT("saved"), false);
		Json->SetBoolField(TEXT("compiled"), false);
		Json->SetBoolField(TEXT("dirty"), Target.System->GetOutermost()->IsDirty());
		Json->SetStringField(
			TEXT("scope"),
			TEXT(
				"authored renderer attribute bindings; runtime binding resolution and component overrides are unverified"));
		return FMCPToolResult::Ok(Json);
	}

	FMCPToolResult SetBindings(
		const FTarget& Target,
		const TArray<TSharedPtr<FJsonValue>>& Bindings,
		bool& bOutChanged,
		TArray<TSharedPtr<FJsonValue>>& OutApplied)
	{
		bOutChanged = false;
		const UScriptStruct* BindingStruct = FNiagaraVariableAttributeBinding::StaticStruct();
		struct FPendingBinding
		{
			FStructProperty* Property = nullptr;
			FString Value;
		};
		TArray<FPendingBinding> Pending;
		Pending.Reserve(Bindings.Num());

		// Validate and resolve every entry before touching the renderer. A
		// transaction does not reliably undo direct UObject member writes when a
		// later entry is rejected, so mutation must be an all-or-nothing operation.
		for (const TSharedPtr<FJsonValue>& EntryValue : Bindings)
		{
			const TSharedPtr<FJsonObject> Entry = EntryValue.IsValid() ? EntryValue->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				return Error(
					TEXT("Each bindings entry must be an object with 'attribute' and 'value' string fields."),
					TEXT("binding_entry_invalid"));
			}
			FString Attribute;
			FString Value;
			if (!Entry->TryGetStringField(TEXT("attribute"), Attribute)
				|| Attribute.IsEmpty() || Attribute.Len() > MaxBindingCharacters)
			{
				return Error(
					TEXT("Each bindings entry requires a non-empty 'attribute' binding property name."),
					TEXT("binding_attribute_invalid"));
			}
			if (!Entry->TryGetStringField(TEXT("value"), Value)
				|| Value.IsEmpty() || Value.Len() > MaxBindingCharacters)
			{
				return Error(
					TEXT("Each bindings entry requires a non-empty 'value' bound attribute name."),
					TEXT("binding_value_invalid"));
			}

			FStructProperty* Found = nullptr;
			for (TFieldIterator<FProperty> It(Target.Renderer->GetClass()); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) continue;
				FStructProperty* StructProperty = CastField<FStructProperty>(*It);
				if (StructProperty && StructProperty->Struct == BindingStruct
					&& FCString::Stricmp(*StructProperty->GetName(), *Attribute) == 0)
				{
					Found = StructProperty;
					break;
				}
			}
			if (!Found)
			{
				return Error(
					FString::Printf(
						TEXT(
							"Unknown binding '%s' on renderer class %s. Use renderer.bindings.get to list available binding names."),
						*Attribute, *Target.Renderer->GetClass()->GetName()),
					TEXT("binding_not_found"), 404);
			}
			Pending.Add({Found, MoveTemp(Value)});
		}

		for (const FPendingBinding& Item : Pending)
		{
			FNiagaraVariableAttributeBinding* Binding =
				Item.Property->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Target.Renderer);
			const FName NewName(*Item.Value);
			const FName BeforeName = Binding->GetParamMapBindableVariable().GetName();
			Binding->SetValue(NewName, Target.Renderer->GetOuterEmitterBase(), Target.Renderer->GetCurrentSourceMode());
			const FName AfterName = Binding->GetParamMapBindableVariable().GetName();
			bOutChanged |= BeforeName != AfterName;

			auto Applied = MakeShared<FJsonObject>();
			Applied->SetStringField(TEXT("attribute"), Item.Property->GetName());
			Applied->SetStringField(TEXT("value"), Item.Value);
			OutApplied.Add(MakeShared<FJsonValueObject>(Applied));
		}
		return FMCPToolResult::Ok(nullptr);
	}

	class FRendererAdd final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.add"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			UNiagaraSystem* System = nullptr;
			FMCPToolResult Result = LoadSystem(Params, System);
			if (!Result.bSuccess) return Result;

			FString ClassName;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("class"), ClassName)
				|| ClassName.IsEmpty() || ClassName.Len() > MaxRendererClassCharacters)
			{
				return Error(
					FString::Printf(
						TEXT("class must name a supported renderer class: %s."), *SupportedRendererClasses()));
			}
			UClass* RendererClass = ResolveRendererClass(ClassName);
			if (!RendererClass)
			{
				return Error(
					FString::Printf(
						TEXT("Unsupported renderer class '%s'. Supported: %s."), *ClassName,
						*SupportedRendererClasses()),
					TEXT("renderer_class_unsupported"));
			}

			if (!SystemWritable(System))
			{
				return Error(
					TEXT("Writes require a non-transient /Game/ Niagara System."), TEXT("system_read_only"), 409);
			}

			FString EmitterSelector;
			if (!Params->TryGetStringField(TEXT("emitter"), EmitterSelector))
			{
				return Error(TEXT("emitter must be an exact handle ID or unambiguous display name."));
			}
			const FNiagaraEmitterHandle* Handle = nullptr;
			Result = FindEmitter(System, EmitterSelector, Handle);
			if (!Result.bSuccess) return Result;

			UNiagaraEmitter* Emitter = Handle->GetInstance().Emitter;
			const FGuid Version = Handle->GetInstance().Version;
			if (!Emitter || Emitter->GetOutermost() != System->GetOutermost())
			{
				return Error(
					TEXT("Writes require the emitter to be owned by the System package."), TEXT("emitter_read_only"),
					409);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Add Niagara Renderer")));
			System->Modify();
			Emitter->Modify();
			UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(
				Emitter, RendererClass, NAME_None, RF_Transactional);
			if (!Renderer)
			{
				// Cancel the no-op transaction so the Modify() calls are not left as
				// a committed empty change in the Editor Undo history.
				Transaction.Cancel();
				return Error(TEXT("Failed to create the requested renderer."), TEXT("renderer_create_failed"), 500);
			}
			Emitter->AddRenderer(Renderer, Version);
			System->MarkPackageDirty();
			System->RequestCompile(false);

			// Read back: locate the new renderer in the current emitter inventory.
			int32 RendererIndex = INDEX_NONE;
			const auto& Renderers = Handle->GetEmitterData()->GetRenderers();
			for (int32 Index = 0; Index < Renderers.Num(); ++Index)
			{
				if (IsValid(Renderers[Index]) && Renderers[Index] == Renderer)
				{
					RendererIndex = Index;
					break;
				}
			}

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-added.v1"));
			Json->SetStringField(TEXT("system"), ObjectPath(System));
			Json->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
			Json->SetStringField(TEXT("emitterId"), Handle->GetId().ToString(EGuidFormats::DigitsWithHyphensLower));
			Json->SetStringField(TEXT("emitterVersion"), Version.ToString(EGuidFormats::DigitsWithHyphensLower));
			Json->SetStringField(TEXT("rendererClass"), Renderer->GetClass()->GetPathName());
			Json->SetStringField(TEXT("rendererClassKey"), NormalizeRendererClassName(ClassName));
			Json->SetStringField(TEXT("rendererPath"), ObjectPath(Renderer));
			Json->SetNumberField(TEXT("rendererIndex"), RendererIndex);
			Json->SetNumberField(TEXT("rendererCount"), Renderers.Num());
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), System->GetOutermost()->IsDirty());
			Json->SetBoolField(TEXT("compileRequested"), true);
			Json->SetStringField(
				TEXT("scope"), TEXT("authored renderer structure; runtime binding resolution is unverified"));
			return FMCPToolResult::Ok(Json);
		}
	};

	class FRendererRemove final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.remove"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target);
			if (!Result.bSuccess) return Result;
			if (!Owned(Target))
			{
				return Error(
					TEXT("Writes require a /Game/ System and an owned emitter renderer."), TEXT("renderer_read_only"),
					409);
			}

			const FNiagaraEmitterHandle* Handle = nullptr;
			Result = FindEmitter(Target.System, Target.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower),
			                     Handle);
			if (!Result.bSuccess) return Result;
			const int32 BeforeCount = Handle->GetEmitterData()->GetRenderers().Num();

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Remove Niagara Renderer")));
			Target.System->Modify();
			Target.Emitter->Modify();
			Target.Renderer->Modify();
			Target.Emitter->RemoveRenderer(Target.Renderer, Target.EmitterVersion);
			Target.System->MarkPackageDirty();
			Target.System->RequestCompile(false);

			const auto& Renderers = Handle->GetEmitterData()->GetRenderers();
			bool bRemoved = Renderers.Num() == BeforeCount - 1;
			for (const UNiagaraRendererProperties* Remaining : Renderers)
			{
				if (Remaining == Target.Renderer)
				{
					bRemoved = false;
					break;
				}
			}

			// Read-back failure must restore the change rather than report a silent
			// "removed:false" success, matching the module remove restore-on-failure
			// contract.
			if (!bRemoved)
			{
				Transaction.Cancel();
				return Error(
					TEXT("Renderer removal read-back failed; the change was restored."),
					TEXT("renderer_remove_verification_failed"),
					500);
			}

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-removed.v1"));
			Json->SetStringField(TEXT("system"), ObjectPath(Target.System));
			Json->SetStringField(TEXT("emitter"), Target.EmitterName);
			Json->SetStringField(TEXT("emitterId"), Target.EmitterId.ToString(EGuidFormats::DigitsWithHyphensLower));
			Json->SetStringField(TEXT("rendererPath"), ObjectPath(Target.Renderer));
			Json->SetNumberField(TEXT("rendererIndex"), Target.RendererIndex);
			Json->SetNumberField(TEXT("rendererCount"), Renderers.Num());
			Json->SetBoolField(TEXT("removed"), bRemoved);
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), Target.System->GetOutermost()->IsDirty());
			Json->SetBoolField(TEXT("compileRequested"), true);
			Json->SetStringField(
				TEXT("scope"), TEXT("authored renderer structure; runtime binding resolution is unverified"));
			return FMCPToolResult::Ok(Json);
		}
	};

	class FRendererPropertySet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.property.set"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target);
			if (!Result.bSuccess) return Result;
			if (!Owned(Target))
			{
				return Error(
					TEXT("Writes require a /Game/ System and an owned emitter renderer."), TEXT("renderer_read_only"),
					409);
			}

			FString Property;
			if (!Params->TryGetStringField(TEXT("property"), Property)
				|| Property.IsEmpty() || Property.Len() > MaxRendererPropertyCharacters)
			{
				return Error(FString::Printf(
					TEXT("property must name a supported renderer property: %s."), *SupportedRendererProperties()));
			}
			const ERendererPropertyKey Key = ResolveRendererPropertyKey(Property);
			if (Key == ERendererPropertyKey::Unknown)
			{
				// ComponentCountLimit is intentionally outside the allowlisted write
				// surface, but it is a reflected uint32 on component renderers. Check
				// malformed numeric input before reporting the bounded-property
				// rejection so overflow cannot be mistaken for an unknown-property
				// failure (and can never reach ImportText or mutate the renderer).
				if (Property.Equals(TEXT("ComponentCountLimit"), ESearchCase::IgnoreCase))
				{
					const TSharedPtr<FJsonValue>* ValuePtr = Params->Values.Find(TEXT("value"));
					const bool bValidUint32 = ValuePtr && ValuePtr->IsValid()
						&& (*ValuePtr)->Type == EJson::Number
						&& FMath::IsFinite((*ValuePtr)->AsNumber())
						&& FMath::FloorToDouble((*ValuePtr)->AsNumber()) == (*ValuePtr)->AsNumber()
						&& (*ValuePtr)->AsNumber() >= 0.0
						&& (*ValuePtr)->AsNumber() <= static_cast<double>(MAX_uint32);
					if (!bValidUint32)
					{
						return Error(
							TEXT("ComponentCountLimit expects an integer in the uint32 range."),
							TEXT("property_value_invalid"));
					}
				}
				return Error(
					FString::Printf(
						TEXT("Unsupported renderer property '%s'. Supported: %s."), *Property,
						*SupportedRendererProperties()),
					TEXT("renderer_property_unsupported"));
			}

			const TSharedPtr<FJsonValue>* ValuePtr = Params->Values.Find(TEXT("value"));
			if (!ValuePtr || !(*ValuePtr).IsValid())
			{
				return Error(TEXT("value is required."), TEXT("property_value_required"));
			}
			if (!ValidateRendererIntegerRange(Key, *ValuePtr))
			{
				return Error(
					Key == ERendererPropertyKey::TubeSubdivisions
						? TEXT("tubeSubdivisions must be an integer in the range [3, 16].")
						: TEXT("tessellationFactor must be an integer in the range [1, 16]."),
					TEXT("property_value_out_of_range"));
			}

			// Resolve the renderer-specific property and capture its current value
			// before opening a transaction.  `Modify()` can dirty the package even
			// when a later dispatch check rejects the request, which would make a
			// failed cross-renderer write observable as an authored change.
			FProperty* KnownProperty = FindKnownRendererProperty(Target.Renderer, Key);
			if (!KnownProperty)
			{
				return Error(
					FString::Printf(TEXT("Renderer property '%s' is unavailable for this renderer."), *Property),
					TEXT("property_unsupported_for_renderer"));
			}
			FString BeforeText;
		KnownProperty->ExportTextItem_Direct(
				BeforeText,
				KnownProperty->ContainerPtrToValuePtr<void>(Target.Renderer),
				nullptr,
				Target.Renderer,
				PPF_None);
			const TSharedPtr<FJsonValue> BeforeReadback = ReadKnownRendererProperty(Target.Renderer, Key);
			if (!BeforeReadback.IsValid())
			{
				return Error(
					FString::Printf(TEXT("Renderer property '%s' cannot be read before mutation."), *Property),
					TEXT("property_readback_failed"), 500);
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Set Niagara Renderer Property")));
			Target.System->Modify();
			Target.Renderer->Modify();
			bool bChanged = false;
			TSharedPtr<FJsonValue> Readback;
			Result = SetRendererPropertyValue(Target.Renderer, Key, *ValuePtr, bChanged);
			if (!Result.bSuccess)
			{
				Transaction.Cancel();
				return Result;
			}
			if (!Readback.IsValid()) Readback = ReadKnownRendererProperty(Target.Renderer, Key);
			if (!Readback.IsValid())
			{
				// A transaction cancel is not sufficient to restore a direct UObject
				// member write. Reapply the approved old value through the same
				// allowlisted setter before returning the readback failure.
				const bool bRestored = RestoreKnownRendererPropertyFromText(Target.Renderer, Key, BeforeText);
				FString RestoredText;
				KnownProperty->ExportTextItem_Direct(
					RestoredText,
					KnownProperty->ContainerPtrToValuePtr<void>(Target.Renderer),
					nullptr,
					Target.Renderer,
					PPF_None);
				Transaction.Cancel();
				if (!bRestored || RestoredText != BeforeText || !ReadKnownRendererProperty(Target.Renderer, Key).
					IsValid())
				{
					return Error(
						FString::Printf(
							TEXT("Renderer property '%s' changed and could not be restored after readback failure."),
							*Property),
						TEXT("property_restore_failed"), 500);
				}
				return Error(
					FString::Printf(TEXT("Renderer property '%s' changed but could not be read back."), *Property),
					TEXT("property_readback_failed"), 500);
			}
			if (!bChanged)
			{
				Transaction.Cancel();
			}
			else
			{
				Target.System->MarkPackageDirty();
				Target.System->RequestCompile(false);
			}

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-property.v1"));
			Json->SetStringField(TEXT("system"), ObjectPath(Target.System));
			Json->SetStringField(TEXT("rendererPath"), ObjectPath(Target.Renderer));
			Json->SetStringField(TEXT("property"), Property);
			Json->SetStringField(TEXT("propertyName"), Property);
			Json->SetStringField(TEXT("propertyType"), TEXT("renderer_alias"));
			Json->SetField(TEXT("value"), Readback);
			Json->SetField(TEXT("readback"), Readback);
			Json->SetBoolField(TEXT("changed"), bChanged);
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), Target.System->GetOutermost()->IsDirty());
			Json->SetBoolField(TEXT("compileRequested"), bChanged);
			Json->SetStringField(
				TEXT("scope"),
				TEXT(
					"authored renderer configuration; runtime binding resolution and component overrides are unverified"));
			return FMCPToolResult::Ok(Json);
		}
	};

	class FRendererBindingsGet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.bindings.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target);
			if (!Result.bSuccess) return Result;
			return ReadBindings(Target);
		}
	};

	class FRendererBindingsSet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.renderer.bindings.set"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target);
			if (!Result.bSuccess) return Result;
			if (!Owned(Target))
			{
				return Error(
					TEXT("Writes require a /Game/ System and an owned emitter renderer."), TEXT("renderer_read_only"),
					409);
			}

			const TSharedPtr<FJsonValue>* BindingsValue = Params->Values.Find(TEXT("bindings"));
			if (!BindingsValue || !(*BindingsValue).IsValid() || (*BindingsValue)->Type != EJson::Array)
			{
				return Error(
					TEXT("bindings must be an array of {attribute, value} objects."), TEXT("bindings_invalid"));
			}
			const TArray<TSharedPtr<FJsonValue>>& Bindings = (*BindingsValue)->AsArray();
			if (Bindings.Num() == 0 || Bindings.Num() > MaxBindingsPerRequest)
			{
				return Error(FString::Printf(TEXT("bindings must contain 1..%d entries."), MaxBindingsPerRequest),
				             TEXT("bindings_invalid"));
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Set Niagara Renderer Bindings")));
			Target.System->Modify();
			Target.Renderer->Modify();
			bool bChanged = false;
			TArray<TSharedPtr<FJsonValue>> Applied;
			Result = SetBindings(Target, Bindings, bChanged, Applied);
			if (!Result.bSuccess)
			{
				Transaction.Cancel();
				return Result;
			}
			if (!bChanged)
			{
				Transaction.Cancel();
			}
			else
			{
				Target.System->MarkPackageDirty();
				Target.System->RequestCompile(false);
			}

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.renderer-bindings-set.v1"));
			Json->SetStringField(TEXT("system"), ObjectPath(Target.System));
			Json->SetStringField(TEXT("rendererPath"), ObjectPath(Target.Renderer));
			Json->SetArrayField(TEXT("bindings"), Applied);
			Json->SetBoolField(TEXT("changed"), bChanged);
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), Target.System->GetOutermost()->IsDirty());
			Json->SetBoolField(TEXT("compileRequested"), bChanged);
			Json->SetStringField(
				TEXT("scope"), TEXT("authored renderer attribute bindings; runtime binding resolution is unverified"));
			return FMCPToolResult::Ok(Json);
		}
	};
}
#endif

namespace UEAIIntegrationTools
{
	void RegisterNiagaraRendererMaterialTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
		using namespace UEAINiagaraRendererMaterial;
		Registry.Register(MakeShared<FRendererList>());
		Registry.Register(MakeShared<FMaterialsGet>());
		Registry.Register(MakeShared<FMaterialPlan>());
		Registry.Register(MakeShared<FMaterialApply>());
		Registry.Register(MakeShared<FMaterialRollback>());
		Registry.Register(MakeShared<FMaterialReceiptRelease>());
		Registry.Register(MakeShared<FRendererAdd>());
		Registry.Register(MakeShared<FRendererRemove>());
		Registry.Register(MakeShared<FRendererPropertySet>());
		Registry.Register(MakeShared<FRendererBindingsGet>());
		Registry.Register(MakeShared<FRendererBindingsSet>());
#else
		class FUnavailableRendererMaterial final : public FMCPToolBase
		{
		public:
			explicit FUnavailableRendererMaterial(const TCHAR* InId) : Id(InId)
			{
			}

			FString GetCapabilityId() const override { return Id; }

			FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
			{
				return FMCPToolResult::Error(
					TEXT("Niagara editor support is unavailable in this build."), TEXT("capability_unavailable"), 409);
			}

		private:
			FString Id;
		};
		for (const TCHAR* Id : {
			     TEXT("content.niagara.renderer.list"), TEXT("content.niagara.renderer.materials.get"),
			     TEXT("content.niagara.renderer.material.plan"), TEXT("content.niagara.renderer.material.apply"),
			     TEXT("content.niagara.renderer.material.rollback"),
			     TEXT("content.niagara.renderer.material.receipt.release"),
			     TEXT("content.niagara.renderer.add"), TEXT("content.niagara.renderer.remove"),
			     TEXT("content.niagara.renderer.property.set"), TEXT("content.niagara.renderer.bindings.get"),
			     TEXT("content.niagara.renderer.bindings.set")
		     })
		{
			Registry.Register(MakeShared<FUnavailableRendererMaterial>(Id));
		}
#endif
	}
}
