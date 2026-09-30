// Authored Niagara System user-parameter inspection and plan-gated editing.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/Sha256.h"
#include "NiagaraReceiptSupport.h"

#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif

#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "NiagaraParameterStore.h"
#include "NiagaraScriptVariable.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemEditorData.h"
#include "NiagaraUserRedirectionParameterStore.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UEAINiagaraSystemParameters
{
	using UEAIIntegration::Infrastructure::TryDigestJson;
	using UEAIIntegration::Infrastructure::TrySha256Hex;
	using UEAIIntegration::Infrastructure::ValidateChangeApproval;
	using UEAINiagaraReceiptSupport::FReleasedRequest;
	using UEAINiagaraReceiptSupport::FReleasedRequestHistory;
	using UEAINiagaraReceiptSupport::RestoreDirtyState;

	constexpr int32 MaxPathCharacters = 2048;
	constexpr int32 MaxParameterCharacters = 512;
	constexpr int32 MaxReceipts = UEAINiagaraReceiptSupport::MaxActiveReceiptCount;
	constexpr int32 MaxTerminalRequestHistory = UEAINiagaraReceiptSupport::MaxTerminalRequestHistory;

	struct FTarget
	{
		UNiagaraSystem* System = nullptr;
		UNiagaraScriptVariable* ScriptVariable = nullptr;
		FNiagaraVariable Variable;
		FString ParameterName;
	};

	struct FState
	{
		TArray<uint8> StoreData;
		TArray<uint8> ScriptData;
		TSharedPtr<FJsonObject> Json;
		FString Digest;
		FString StoreRawDigest;
		FString AuthoredDefaultRawDigest;
		bool bStoreValueRepresentable = false;
		bool bAuthoredDefaultValueRepresentable = false;
	};

	struct FPlan
	{
		FTarget Target;
		FState Before;
		TArray<uint8> DesiredData;
		FString DesiredRawDigest;
		TSharedPtr<FJsonObject> Json;
		FString PlanDigest;
	};

	struct FReceipt
	{
		FString Id;
		FString RequestId;
		FString RequestDigest;
		FString PlanDigest;
		TWeakObjectPtr<UNiagaraSystem> System;
		TWeakObjectPtr<UNiagaraScriptVariable> ScriptVariable;
		FString SystemPath;
		FString ParameterName;
		FString TypeName;
		TArray<uint8> BeforeStoreData;
		TArray<uint8> BeforeScriptData;
		TArray<uint8> DesiredData;
		FString BeforeDigest;
		FString AfterDigest;
		FString RestoredDigest;
		FString BeforeStoreRawDigest;
		FString BeforeAuthoredDefaultRawDigest;
		FString DesiredRawDigest;
		bool bBeforeStoreValueRepresentable = false;
		bool bBeforeAuthoredDefaultValueRepresentable = false;
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
		bool bSemanticRollbackVerified = false;
		bool bRolledBack = false;
	};

	TMap<FString, FReceipt> Receipts;
	TMap<FString, FString> RequestReceipts;
	FReleasedRequestHistory ReleasedRequests;

	FMCPToolResult Error(const FString& Message, const TCHAR* Code = TEXT("invalid_system_parameter_request"),
	                     int32 Status = 422)
	{
		return FMCPToolResult::Error(Message, Code, Status);
	}

	FMCPToolResult LoadSystem(const TSharedPtr<FJsonObject>& Params, UNiagaraSystem*& OutSystem)
	{
		FString ObjectPath;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("system"), ObjectPath)
			|| ObjectPath.IsEmpty() || ObjectPath.Len() > MaxPathCharacters)
		{
			return Error(TEXT("system must be an exact Niagara System package or object path."));
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			return Error(TEXT("system must be a valid long package or object path."));
		}
		if (!ObjectPath.Contains(TEXT(".")))
		{
			ObjectPath = PackageName + TEXT(".") + FPackageName::GetShortName(PackageName);
		}
		OutSystem = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn);
		return OutSystem
			       ? FMCPToolResult::Ok(nullptr)
			       : Error(TEXT("The Niagara System was not found."), TEXT("system_not_found"), 404);
	}

	FString ShortParameterName(FString Name)
	{
		if (Name.StartsWith(TEXT("User."), ESearchCase::IgnoreCase))
		{
			Name.RightChopInline(5, EAllowShrinking::No);
		}
		return Name;
	}

	UNiagaraScriptVariable* FindScriptVariable(UNiagaraSystemEditorData* EditorData, const FNiagaraVariable& Variable)
	{
		FArrayProperty* Array = FindFProperty<FArrayProperty>(EditorData->GetClass(), TEXT("UserParameterMetaData"));
		FObjectPropertyBase* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
		if (!Array || !Inner)
		{
			return nullptr;
		}
		FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(EditorData));
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			UNiagaraScriptVariable* Candidate = Cast<UNiagaraScriptVariable>(
				Inner->GetObjectPropertyValue(Values.GetRawPtr(Index)));
			if (Candidate
				&& Candidate->Variable.GetType() == Variable.GetType()
				&& ShortParameterName(Candidate->Variable.GetName().ToString()).Equals(
					ShortParameterName(Variable.GetName().ToString()), ESearchCase::IgnoreCase))
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	FMCPToolResult ResolveTarget(const TSharedPtr<FJsonObject>& Params, FTarget& OutTarget, bool bRequireOwned)
	{
		FMCPToolResult Result = LoadSystem(Params, OutTarget.System);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (bRequireOwned && (!OutTarget.System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))
			|| OutTarget.System->HasAnyFlags(RF_Transient)))
		{
			return Error(TEXT("Writes require a non-transient /Game/ Niagara System."), TEXT("system_read_only"), 409);
		}

		FString Selector;
		if (!Params->TryGetStringField(TEXT("parameter"), Selector)
			|| Selector.IsEmpty() || Selector.Len() > MaxParameterCharacters)
		{
			return Error(TEXT("parameter must be an exact user parameter name, with or without the User. prefix."));
		}
		Selector = ShortParameterName(Selector);
		TArray<FNiagaraVariable> UserParameters;
		OutTarget.System->GetExposedParameters().GetUserParameters(UserParameters);
		for (const FNiagaraVariable& Variable : UserParameters)
		{
			if (Variable.GetName().ToString().Equals(Selector, ESearchCase::IgnoreCase))
			{
				if (OutTarget.Variable.IsValid())
				{
					return Error(TEXT("The user parameter name is ambiguous."), TEXT("ambiguous_parameter"), 409);
				}
				OutTarget.Variable = Variable;
			}
		}
		if (!OutTarget.Variable.IsValid())
		{
			return Error(TEXT("The authored user parameter was not found."), TEXT("parameter_not_found"), 404);
		}
		OutTarget.ParameterName = TEXT("User.") + ShortParameterName(OutTarget.Variable.GetName().ToString());
		UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(OutTarget.System->GetEditorData());
		OutTarget.ScriptVariable = EditorData ? FindScriptVariable(EditorData, OutTarget.Variable) : nullptr;
		if (!OutTarget.ScriptVariable)
		{
			return Error(
				TEXT("The user parameter has no synchronized authored metadata entry."),
				TEXT("parameter_metadata_missing"), 409);
		}
		return FMCPToolResult::Ok(nullptr);
	}

	bool IsSupportedType(const FNiagaraTypeDefinition& Type)
	{
		return Type == FNiagaraTypeDefinition::GetFloatDef()
			|| Type == FNiagaraTypeDefinition::GetIntDef()
			|| Type == FNiagaraTypeDefinition::GetBoolDef()
			|| Type == FNiagaraTypeDefinition::GetVec2Def()
			|| Type == FNiagaraTypeDefinition::GetVec3Def()
			|| Type == FNiagaraTypeDefinition::GetPositionDef()
			|| Type == FNiagaraTypeDefinition::GetVec4Def()
			|| Type == FNiagaraTypeDefinition::GetQuatDef()
			|| Type == FNiagaraTypeDefinition::GetColorDef();
	}

	bool ResolveTypeName(const FString& TypeName, FNiagaraTypeDefinition& OutType)
	{
		const FString L = TypeName.TrimStartAndEnd().ToLower();
		if (L == TEXT("float"))
		{
			OutType = FNiagaraTypeDefinition::GetFloatDef();
			return true;
		}
		if (L == TEXT("int") || L == TEXT("int32") || L == TEXT("integer"))
		{
			OutType = FNiagaraTypeDefinition::GetIntDef();
			return true;
		}
		if (L == TEXT("bool") || L == TEXT("boolean"))
		{
			OutType = FNiagaraTypeDefinition::GetBoolDef();
			return true;
		}
		if (L == TEXT("vec2"))
		{
			OutType = FNiagaraTypeDefinition::GetVec2Def();
			return true;
		}
		if (L == TEXT("vec3") || L == TEXT("vector") || L == TEXT("vector3"))
		{
			OutType = FNiagaraTypeDefinition::GetVec3Def();
			return true;
		}
		if (L == TEXT("vec4") || L == TEXT("vector4"))
		{
			OutType = FNiagaraTypeDefinition::GetVec4Def();
			return true;
		}
		if (L == TEXT("position"))
		{
			OutType = FNiagaraTypeDefinition::GetPositionDef();
			return true;
		}
		if (L == TEXT("quat") || L == TEXT("quaternion"))
		{
			OutType = FNiagaraTypeDefinition::GetQuatDef();
			return true;
		}
		if (L == TEXT("color") || L == TEXT("linearcolor"))
		{
			OutType = FNiagaraTypeDefinition::GetColorDef();
			return true;
		}
		return false;
	}

	template <typename TValue>
	TValue ValueFromBytes(const TArray<uint8>& Bytes)
	{
		TValue Value{};
		check(Bytes.Num() == sizeof(TValue));
		FMemory::Memcpy(&Value, Bytes.GetData(), sizeof(TValue));
		return Value;
	}

	template <typename TValue>
	TArray<uint8> BytesFromValue(const TValue& Value)
	{
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(sizeof(TValue));
		FMemory::Memcpy(Bytes.GetData(), &Value, sizeof(TValue));
		return Bytes;
	}

	TSharedPtr<FJsonValue> FiniteNumber(double Value)
	{
		if (FMath::IsFinite(Value))
		{
			return MakeShared<FJsonValueNumber>(Value);
		}
		return MakeShared<FJsonValueNull>();
	}

	TSharedPtr<FJsonValue> DataJson(const FNiagaraTypeDefinition& Type, const TArray<uint8>& Data)
	{
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return FiniteNumber(ValueFromBytes<float>(Data));
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return MakeShared<FJsonValueNumber>(ValueFromBytes<int32>(Data));
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			const FNiagaraBool Value = ValueFromBytes<FNiagaraBool>(Data);
			if (Value.IsValid())
			{
				return MakeShared<FJsonValueBoolean>(Value.GetValue());
			}
			return MakeShared<FJsonValueNull>();
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			const FVector2f Value = ValueFromBytes<FVector2f>(Data);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			const FVector3f Value = ValueFromBytes<FVector3f>(Data);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), FiniteNumber(Value.Z));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			const FVector4f Value = ValueFromBytes<FVector4f>(Data);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), FiniteNumber(Value.Z));
			Json->SetField(TEXT("w"), FiniteNumber(Value.W));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			const FQuat4f Value = ValueFromBytes<FQuat4f>(Data);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("x"), FiniteNumber(Value.X));
			Json->SetField(TEXT("y"), FiniteNumber(Value.Y));
			Json->SetField(TEXT("z"), FiniteNumber(Value.Z));
			Json->SetField(TEXT("w"), FiniteNumber(Value.W));
			return MakeShared<FJsonValueObject>(Json);
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const FLinearColor Value = ValueFromBytes<FLinearColor>(Data);
			auto Json = MakeShared<FJsonObject>();
			Json->SetField(TEXT("r"), FiniteNumber(Value.R));
			Json->SetField(TEXT("g"), FiniteNumber(Value.G));
			Json->SetField(TEXT("b"), FiniteNumber(Value.B));
			Json->SetField(TEXT("a"), FiniteNumber(Value.A));
			return MakeShared<FJsonValueObject>(Json);
		}
		return MakeShared<FJsonValueNull>();
	}

	bool DataRepresentable(const FNiagaraTypeDefinition& Type, const TArray<uint8>& Data)
	{
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			return FMath::IsFinite(ValueFromBytes<float>(Data));
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			return ValueFromBytes<FNiagaraBool>(Data).IsValid();
		}
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			const FVector2f Value = ValueFromBytes<FVector2f>(Data);
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y);
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			const FVector3f Value = ValueFromBytes<FVector3f>(Data);
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			const FVector4f Value = ValueFromBytes<FVector4f>(Data);
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
				&& FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W);
		}
		if (Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			const FQuat4f Value = ValueFromBytes<FQuat4f>(Data);
			return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y)
				&& FMath::IsFinite(Value.Z) && FMath::IsFinite(Value.W);
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			const FLinearColor Value = ValueFromBytes<FLinearColor>(Data);
			return FMath::IsFinite(Value.R) && FMath::IsFinite(Value.G)
				&& FMath::IsFinite(Value.B) && FMath::IsFinite(Value.A);
		}
		return false;
	}

	bool RawDataDigest(const FNiagaraTypeDefinition& Type, const TArray<uint8>& Data, FString& OutDigest)
	{
		const FString Prefix = FString::Printf(
			TEXT("ue.niagara.parameter.raw.v1\ntype=%s\nsize=%d\n"),
			*Type.GetName(), Data.Num());
		FTCHARToUTF8 PrefixUtf8(*Prefix);
		TArray<uint8> Identity;
		Identity.Reserve(PrefixUtf8.Length() + Data.Num());
		Identity.Append(reinterpret_cast<const uint8*>(PrefixUtf8.Get()), PrefixUtf8.Length());
		Identity.Append(Data);
		return TrySha256Hex(Identity, OutDigest);
	}

	bool ReadFloat(const TSharedPtr<FJsonValue>& Json, float& OutValue)
	{
		if (!Json.IsValid() || Json->Type != EJson::Number)
		{
			return false;
		}
		const double Number = Json->AsNumber();
		const float Value = static_cast<float>(Number);
		if (!FMath::IsFinite(Number) || !FMath::IsFinite(Value))
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	bool ReadComponent(const TSharedPtr<FJsonObject>& Json, const TCHAR* Name, float& OutValue)
	{
		double Number = 0.0;
		if (!Json.IsValid() || !Json->TryGetNumberField(Name, Number))
		{
			return false;
		}
		const float Value = static_cast<float>(Number);
		if (!FMath::IsFinite(Number) || !FMath::IsFinite(Value))
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	bool JsonData(const FNiagaraTypeDefinition& Type, const TSharedPtr<FJsonValue>& Json, TArray<uint8>& OutData)
	{
		if (!Json.IsValid())
		{
			return false;
		}
		if (Type == FNiagaraTypeDefinition::GetFloatDef())
		{
			float Value = 0.0f;
			if (!ReadFloat(Json, Value)) return false;
			OutData = BytesFromValue(Value);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetIntDef())
		{
			if (Json->Type != EJson::Number) return false;
			const double Number = Json->AsNumber();
			if (!FMath::IsFinite(Number) || Number != FMath::FloorToDouble(Number)
				|| Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutData = BytesFromValue(static_cast<int32>(Number));
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetBoolDef())
		{
			if (Json->Type != EJson::Boolean) return false;
			FNiagaraBool Value;
			Value.SetValue(Json->AsBool());
			OutData = BytesFromValue(Value);
			return true;
		}
		if (Json->Type != EJson::Object)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Json->AsObject();
		if (Type == FNiagaraTypeDefinition::GetVec2Def())
		{
			if (Object->Values.Num() != 2) return false;
			FVector2f Value;
			if (!ReadComponent(Object, TEXT("x"), Value.X) || !ReadComponent(Object, TEXT("y"), Value.Y)) return false;
			OutData = BytesFromValue(Value);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
		{
			if (Object->Values.Num() != 3) return false;
			FVector3f Value;
			if (!ReadComponent(Object, TEXT("x"), Value.X) || !ReadComponent(Object, TEXT("y"), Value.Y)
				|| !ReadComponent(Object, TEXT("z"), Value.Z))
				return false;
			OutData = BytesFromValue(Value);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetVec4Def())
		{
			if (Object->Values.Num() != 4) return false;
			FVector4f Value;
			if (!ReadComponent(Object, TEXT("x"), Value.X) || !ReadComponent(Object, TEXT("y"), Value.Y)
				|| !ReadComponent(Object, TEXT("z"), Value.Z) || !ReadComponent(Object, TEXT("w"), Value.W))
				return false;
			OutData = BytesFromValue(Value);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetQuatDef())
		{
			if (Object->Values.Num() != 4) return false;
			FQuat4f Value;
			if (!ReadComponent(Object, TEXT("x"), Value.X) || !ReadComponent(Object, TEXT("y"), Value.Y)
				|| !ReadComponent(Object, TEXT("z"), Value.Z) || !ReadComponent(Object, TEXT("w"), Value.W))
				return false;
			OutData = BytesFromValue(Value);
			return true;
		}
		if (Type == FNiagaraTypeDefinition::GetColorDef())
		{
			if (Object->Values.Num() != 3 && Object->Values.Num() != 4) return false;
			FLinearColor Value;
			if (!ReadComponent(Object, TEXT("r"), Value.R) || !ReadComponent(Object, TEXT("g"), Value.G)
				|| !ReadComponent(Object, TEXT("b"), Value.B))
				return false;
			if (Object->HasField(TEXT("a")))
			{
				if (!ReadComponent(Object, TEXT("a"), Value.A)) return false;
			}
			else
			{
				Value.A = 1.0f;
			}
			OutData = BytesFromValue(Value);
			return true;
		}
		return false;
	}

	FMCPToolResult CaptureState(const FTarget& Target, FState& OutState)
	{
		const FNiagaraTypeDefinition& Type = Target.Variable.GetType();
		if (!IsSupportedType(Type) || Type.GetSize() <= 0 || Type.GetSize() > 64)
		{
			return Error(
				TEXT(
					"Only float, int, bool, vec2, vec3, position, vec4, quat and color user parameters are supported."),
				TEXT("parameter_type_unsupported"));
		}
		if (Target.ScriptVariable->DefaultMode != ENiagaraDefaultMode::Value)
		{
			return Error(
				TEXT("The authored parameter default is not an inline value; binding/custom defaults are read-only."),
				TEXT("parameter_default_mode_unsupported"), 409);
		}
		const uint8* StoreData = Target.System->GetExposedParameters().GetParameterData(Target.Variable);
		const uint8* ScriptData = Target.ScriptVariable->GetDefaultValueData();
		if (!StoreData || !ScriptData)
		{
			return Error(TEXT("The parameter value data is unavailable."), TEXT("parameter_data_unavailable"), 409);
		}
		OutState.StoreData.SetNumUninitialized(Type.GetSize());
		OutState.ScriptData.SetNumUninitialized(Type.GetSize());
		FMemory::Memcpy(OutState.StoreData.GetData(), StoreData, Type.GetSize());
		FMemory::Memcpy(OutState.ScriptData.GetData(), ScriptData, Type.GetSize());
		if (!RawDataDigest(Type, OutState.StoreData, OutState.StoreRawDigest)
			|| !RawDataDigest(Type, OutState.ScriptData, OutState.AuthoredDefaultRawDigest))
		{
			return Error(TEXT("Could not hash the raw authored parameter state."), TEXT("digest_unavailable"), 500);
		}
		OutState.bStoreValueRepresentable = DataRepresentable(Type, OutState.StoreData);
		OutState.bAuthoredDefaultValueRepresentable = DataRepresentable(Type, OutState.ScriptData);

		OutState.Json = MakeShared<FJsonObject>();
		OutState.Json->SetStringField(TEXT("system"), Target.System->GetPathName());
		OutState.Json->SetStringField(TEXT("parameter"), Target.ParameterName);
		OutState.Json->SetStringField(TEXT("type"), Type.GetName());
		OutState.Json->SetStringField(TEXT("defaultMode"), TEXT("Value"));
		OutState.Json->SetField(TEXT("storeValue"), DataJson(Type, OutState.StoreData));
		OutState.Json->SetField(TEXT("authoredDefaultValue"), DataJson(Type, OutState.ScriptData));
		OutState.Json->SetStringField(TEXT("storeRawDigest"), OutState.StoreRawDigest);
		OutState.Json->SetStringField(TEXT("authoredDefaultRawDigest"), OutState.AuthoredDefaultRawDigest);
		OutState.Json->SetBoolField(TEXT("storeValueRepresentable"), OutState.bStoreValueRepresentable);
		OutState.Json->SetBoolField(
			TEXT("authoredDefaultValueRepresentable"), OutState.bAuthoredDefaultValueRepresentable);
		OutState.Json->SetBoolField(TEXT("valuesAligned"), OutState.StoreData == OutState.ScriptData);
		if (!TryDigestJson(OutState.Json, OutState.Digest))
		{
			return Error(TEXT("Could not hash the authored parameter state."), TEXT("digest_unavailable"), 500);
		}
		return FMCPToolResult::Ok(nullptr);
	}

	template <typename TValue>
	bool SetStoreTyped(FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Variable,
	                   const TArray<uint8>& Data)
	{
		return Store.SetParameterValue<TValue>(ValueFromBytes<TValue>(Data), Variable, false);
	}

	bool SetStoreData(FNiagaraUserRedirectionParameterStore& Store, const FNiagaraVariable& Variable,
	                  const TArray<uint8>& Data)
	{
		const FNiagaraTypeDefinition& Type = Variable.GetType();
		if (Type == FNiagaraTypeDefinition::GetFloatDef()) return SetStoreTyped<float>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetIntDef()) return SetStoreTyped<int32>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetBoolDef()) return SetStoreTyped<FNiagaraBool>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetVec2Def()) return SetStoreTyped<FVector2f>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
			return
				SetStoreTyped<FVector3f>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetVec4Def()) return SetStoreTyped<FVector4f>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetQuatDef()) return SetStoreTyped<FQuat4f>(Store, Variable, Data);
		if (Type == FNiagaraTypeDefinition::GetColorDef()) return SetStoreTyped<FLinearColor>(Store, Variable, Data);
		return false;
	}

	FMCPToolResult WriteState(
		const FTarget& Target,
		const TArray<uint8>& StoreData,
		const TArray<uint8>& ScriptData,
		bool& bOutMutated)
	{
		bOutMutated = false;
		FState Current;
		FMCPToolResult Result = CaptureState(Target, Current);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (Current.ScriptData != ScriptData)
		{
			FProperty* DefaultValueProperty = FindFProperty<FProperty>(
				UNiagaraScriptVariable::StaticClass(), TEXT("DefaultValueVariant"));
			if (!DefaultValueProperty)
			{
				return Error(
					TEXT("The authored default-value property is unavailable."), TEXT("parameter_metadata_unavailable"),
					500);
			}
			Target.ScriptVariable->PreEditChange(DefaultValueProperty);
			Target.ScriptVariable->SetDefaultValueData(ScriptData.GetData());
			Target.ScriptVariable->UpdateChangeId();
			FPropertyChangedEvent Event(DefaultValueProperty, EPropertyChangeType::ValueSet);
			Target.ScriptVariable->PostEditChangeProperty(Event);
			bOutMutated = true;
		}
		if (Current.StoreData != StoreData)
		{
			FNiagaraUserRedirectionParameterStore& Store = Target.System->GetExposedParameters();
			FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(Store);
			if (!SetStoreData(Store, Target.Variable, StoreData))
			{
				return Error(
					TEXT("Failed to write the exposed parameter store."), TEXT("parameter_store_write_failed"), 500);
			}
			bOutMutated = true;
		}
		if (bOutMutated)
		{
			Target.System->MarkPackageDirty();
		}
		return FMCPToolResult::Ok(nullptr);
	}

	FMCPToolResult BuildPlan(const TSharedPtr<FJsonObject>& Params, FPlan& OutPlan)
	{
		FMCPToolResult Result = ResolveTarget(Params, OutPlan.Target, true);
		if (!Result.bSuccess)
		{
			return Result;
		}
		Result = CaptureState(OutPlan.Target, OutPlan.Before);
		if (!Result.bSuccess)
		{
			return Result;
		}
		const TSharedPtr<FJsonValue>* Value = Params->Values.Find(TEXT("value"));
		if (!Value || !JsonData(OutPlan.Target.Variable.GetType(), *Value, OutPlan.DesiredData))
		{
			return Error(
				TEXT("value does not match the parameter type or contains non-finite/out-of-range data."),
				TEXT("parameter_value_invalid"));
		}
		if (!RawDataDigest(OutPlan.Target.Variable.GetType(), OutPlan.DesiredData, OutPlan.DesiredRawDigest))
		{
			return Error(TEXT("Could not hash the requested parameter value."), TEXT("digest_unavailable"), 500);
		}

		OutPlan.Json = MakeShared<FJsonObject>();
		OutPlan.Json->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
		OutPlan.Json->SetStringField(TEXT("domain"), TEXT("content.niagara.system.parameter"));
		OutPlan.Json->SetStringField(TEXT("planKind"), TEXT("niagaraSystemParameterDefault"));
		OutPlan.Json->SetStringField(TEXT("status"), TEXT("planned"));
		OutPlan.Json->SetStringField(TEXT("system"), OutPlan.Target.System->GetPathName());
		OutPlan.Json->SetStringField(TEXT("parameter"), OutPlan.Target.ParameterName);
		OutPlan.Json->SetStringField(TEXT("type"), OutPlan.Target.Variable.GetType().GetName());
		OutPlan.Json->SetStringField(TEXT("stateDigest"), OutPlan.Before.Digest);
		OutPlan.Json->SetField(
			TEXT("beforeStoreValue"), DataJson(OutPlan.Target.Variable.GetType(), OutPlan.Before.StoreData));
		OutPlan.Json->SetField(
			TEXT("beforeAuthoredDefaultValue"), DataJson(OutPlan.Target.Variable.GetType(), OutPlan.Before.ScriptData));
		OutPlan.Json->SetField(TEXT("afterValue"), DataJson(OutPlan.Target.Variable.GetType(), OutPlan.DesiredData));
		OutPlan.Json->SetStringField(TEXT("beforeStoreRawDigest"), OutPlan.Before.StoreRawDigest);
		OutPlan.Json->SetStringField(TEXT("beforeAuthoredDefaultRawDigest"), OutPlan.Before.AuthoredDefaultRawDigest);
		OutPlan.Json->SetStringField(TEXT("afterRawDigest"), OutPlan.DesiredRawDigest);
		OutPlan.Json->SetBoolField(TEXT("beforeStoreValueRepresentable"), OutPlan.Before.bStoreValueRepresentable);
		OutPlan.Json->SetBoolField(
			TEXT("beforeAuthoredDefaultValueRepresentable"), OutPlan.Before.bAuthoredDefaultValueRepresentable);
		OutPlan.Json->SetBoolField(TEXT("afterValueRepresentable"), true);
		OutPlan.Json->SetBoolField(TEXT("changesState"),
		                           OutPlan.Before.StoreData != OutPlan.DesiredData || OutPlan.Before.ScriptData !=
		                           OutPlan.DesiredData);
		OutPlan.Json->SetBoolField(TEXT("compileWillBeRequested"), OutPlan.Json->GetBoolField(TEXT("changesState")));
		OutPlan.Json->SetBoolField(TEXT("confirmWriteRequired"), true);
		OutPlan.Json->SetStringField(TEXT("persistence"), TEXT("dirtyOnly"));
		OutPlan.Json->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
		if (!TryDigestJson(OutPlan.Json, OutPlan.PlanDigest))
		{
			return Error(TEXT("Could not hash the parameter plan."), TEXT("digest_unavailable"), 500);
		}
		return FMCPToolResult::Ok(nullptr);
	}

	bool RequestDigest(const TSharedPtr<FJsonObject>& Params, FString& OutDigest)
	{
		auto Request = MakeShared<FJsonObject>();
		for (const TCHAR* Field : {TEXT("system"), TEXT("parameter"), TEXT("value")})
		{
			if (const TSharedPtr<FJsonValue>* Value = Params->Values.Find(Field))
			{
				Request->SetField(Field, *Value);
			}
		}
		return TryDigestJson(Request, OutDigest);
	}

	TSharedRef<FJsonObject> ReceiptJson(const FReceipt& Receipt, const FNiagaraTypeDefinition& Type, bool bReplay)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-parameter-receipt.v1"));
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
		Json->SetStringField(TEXT("parameter"), Receipt.ParameterName);
		Json->SetStringField(TEXT("type"), Receipt.TypeName);
		Json->SetField(TEXT("beforeStoreValue"), DataJson(Type, Receipt.BeforeStoreData));
		Json->SetField(TEXT("beforeAuthoredDefaultValue"), DataJson(Type, Receipt.BeforeScriptData));
		Json->SetField(TEXT("afterValue"), DataJson(Type, Receipt.DesiredData));
		Json->SetStringField(TEXT("beforeStoreRawDigest"), Receipt.BeforeStoreRawDigest);
		Json->SetStringField(TEXT("beforeAuthoredDefaultRawDigest"), Receipt.BeforeAuthoredDefaultRawDigest);
		Json->SetStringField(TEXT("requestedRawDigest"), Receipt.DesiredRawDigest);
		Json->SetBoolField(TEXT("beforeStoreValueRepresentable"), Receipt.bBeforeStoreValueRepresentable);
		Json->SetBoolField(
			TEXT("beforeAuthoredDefaultValueRepresentable"), Receipt.bBeforeAuthoredDefaultValueRepresentable);
		Json->SetBoolField(TEXT("requestedValueRepresentable"), true);
		Json->SetBoolField(TEXT("currentRawStateVerified"), Receipt.bVerified);
		if (Receipt.bVerified)
		{
			Json->SetStringField(TEXT("storeRawDigest"), Receipt.bRolledBack
				                                             ? Receipt.BeforeStoreRawDigest
				                                             : Receipt.DesiredRawDigest);
			Json->SetStringField(TEXT("authoredDefaultRawDigest"), Receipt.bRolledBack
				                                                       ? Receipt.BeforeAuthoredDefaultRawDigest
				                                                       : Receipt.DesiredRawDigest);
			Json->SetBoolField(TEXT("storeValueRepresentable"), Receipt.bRolledBack
				                                                    ? Receipt.bBeforeStoreValueRepresentable
				                                                    : true);
			Json->SetBoolField(TEXT("authoredDefaultValueRepresentable"), Receipt.bRolledBack
				                                                              ? Receipt.
				                                                              bBeforeAuthoredDefaultValueRepresentable
				                                                              : true);
		}
		Json->SetStringField(TEXT("stateDigest"), Receipt.bRollbackAttempted && !Receipt.RestoredDigest.IsEmpty()
			                                          ? Receipt.RestoredDigest
			                                          : Receipt.AfterDigest);
		Json->SetBoolField(TEXT("changed"), Receipt.bChanged);
		Json->SetBoolField(TEXT("verified"), Receipt.bVerified);
		Json->SetBoolField(TEXT("rollbackAttempted"), Receipt.bRollbackAttempted);
		Json->SetBoolField(TEXT("semanticRollbackVerified"), Receipt.bSemanticRollbackVerified);
		Json->SetBoolField(TEXT("dirtyBefore"), Receipt.bBeforeDirty);
		Json->SetBoolField(TEXT("dirtyAfter"), Receipt.bAfterDirty);
		Json->SetBoolField(TEXT("dirtyRestored"), Receipt.bDirtyRestored);
		Json->SetBoolField(TEXT("dirtyPreserved"), Receipt.bDirtyPreserved);
		Json->SetBoolField(TEXT("trackedStateRestored"), Receipt.bTrackedStateRestored);
		Json->SetBoolField(TEXT("fullPackageStateRestored"), Receipt.bFullPackageStateRestored);
		Json->SetStringField(TEXT("stateCoverage"), TEXT("systemUserParameter"));
		Json->SetBoolField(TEXT("packageStateVerified"), false);
		Json->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack);
		Json->SetBoolField(TEXT("compileRequested"), Receipt.bCompileRequested);
		Json->SetStringField(TEXT("compileStatus"), Receipt.bCompileRequested
			                                            ? TEXT("requestedAsync; completion not awaited")
			                                            : TEXT("notRequested; authored state was unchanged"));
		Json->SetBoolField(TEXT("compiled"), false);
		Json->SetBoolField(TEXT("saved"), false);
		Json->SetBoolField(TEXT("runtimeVerified"), false);
		Json->SetBoolField(TEXT("idempotentReplay"), bReplay);
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
		Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-parameter-receipt-release.v1"));
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

	FMCPToolResult ReceiptTarget(const FReceipt& Receipt, FTarget& OutTarget)
	{
		if (!Receipt.System.IsValid() || !Receipt.ScriptVariable.IsValid())
		{
			return Error(
				TEXT("The receipt target no longer exists in this Editor instance."), TEXT("target_unavailable"), 409);
		}
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("system"), Receipt.SystemPath);
		Params->SetStringField(TEXT("parameter"), Receipt.ParameterName);
		FMCPToolResult Result = ResolveTarget(Params, OutTarget, true);
		if (!Result.bSuccess)
		{
			return Result;
		}
		if (OutTarget.System != Receipt.System.Get() || OutTarget.ScriptVariable != Receipt.ScriptVariable.Get()
			|| OutTarget.Variable.GetType().GetName() != Receipt.TypeName)
		{
			return Error(
				TEXT("The receipt parameter target was replaced or changed type."), TEXT("target_unavailable"), 409);
		}
		return FMCPToolResult::Ok(nullptr);
	}

	class FParameterGet final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.get"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target, false);
			if (!Result.bSuccess) return Result;
			FState State;
			Result = CaptureState(Target, State);
			if (!Result.bSuccess) return Result;
			State.Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-parameter.v1"));
			State.Json->SetStringField(TEXT("stateDigest"), State.Digest);
			State.Json->SetBoolField(TEXT("dirty"), Target.System->GetOutermost()->IsDirty());
			State.Json->SetStringField(
				TEXT("scope"), TEXT("authored user parameter default; loaded component overrides are not inspected"));
			return FMCPToolResult::Ok(State.Json);
		}
	};

	class FParameterAdd final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.add"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			UNiagaraSystem* System = nullptr;
			FMCPToolResult Result = LoadSystem(Params, System);
			if (!Result.bSuccess) return Result;

			// Writes require a non-transient /Game/ Niagara System, matching ResolveTarget.
			if (!System->GetOutermost()->GetName().StartsWith(TEXT("/Game/"))
				|| System->HasAnyFlags(RF_Transient))
			{
				return Error(
					TEXT("Writes require a non-transient /Game/ Niagara System."), TEXT("system_read_only"), 409);
			}

			FString Name;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name)
				|| Name.IsEmpty() || Name.Len() > MaxParameterCharacters)
			{
				return Error(TEXT("name must be a non-empty user parameter name."));
			}
			Name = ShortParameterName(Name);
			if (Name.IsEmpty())
			{
				return Error(TEXT("name must not be empty after stripping the User. prefix."));
			}

			FString TypeName;
			if (!Params->TryGetStringField(TEXT("type"), TypeName) || TypeName.IsEmpty())
			{
				return Error(TEXT("type must be a non-empty Niagara type name."));
			}
			FNiagaraTypeDefinition Type;
			if (!ResolveTypeName(TypeName, Type))
			{
				return Error(
					FString::Printf(
						TEXT(
							"Unsupported parameter type '%s'. Supported: float, int, bool, vec2, vec3, position, vec4, quat, color."),
						*TypeName),
					TEXT("parameter_type_unsupported"));
			}

			// User parameters surface as bare names from GetUserParameters, so compare
			// against the stripped name to reject case-insensitive duplicates.
			TArray<FNiagaraVariable> Existing;
			System->GetExposedParameters().GetUserParameters(Existing);
			for (const FNiagaraVariable& Variable : Existing)
			{
				if (Variable.GetName().ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Error(
						FString::Printf(TEXT("A user parameter named '%s' already exists."), *Name),
						TEXT("parameter_exists"), 409);
				}
			}

			const TSharedPtr<FJsonValue>* Value = Params->Values.Find(TEXT("value"));
			TArray<uint8> Data;
			if (!Value || !JsonData(Type, *Value, Data))
			{
				return Error(
					TEXT("value does not match the requested type or contains non-finite/out-of-range data."),
					TEXT("parameter_value_invalid"));
			}

			// FNiagaraEditorUtilities::AddParameter is not exported (no
			// NIAGARAEDITOR_API), so add directly via the exposed parameter store.
			const FNiagaraVariable Variable(Type, FName(*(TEXT("User.") + Name)));
			if (!System->GetExposedParameters().AddParameter(Variable))
			{
				return Error(
					TEXT("Failed to add the user parameter to the exposed parameter store."),
					TEXT("parameter_store_write_failed"), 500);
			}

			// Replicate the fixture's authored-parameter sync: SyncUserScriptVariables
			// then FindOrAddUserScriptVariable so the new parameter has an authored
			// metadata entry that persists its default across recompiles.
			UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData());
			UNiagaraScriptVariable* ScriptVariable = nullptr;
			if (EditorData)
			{
				EditorData->SyncUserScriptVariables(System);
				ScriptVariable = EditorData->FindOrAddUserScriptVariable(Variable, *System);
			}
			if (!ScriptVariable)
			{
				return Error(
					TEXT("The user parameter has no synchronized authored metadata entry."),
					TEXT("parameter_metadata_unavailable"), 500);
			}
			ScriptVariable->DefaultMode = ENiagaraDefaultMode::Value;
			ScriptVariable->SetDefaultValueData(Data.GetData());

			{
				// Suppress the store change delegate so the default write does not
				// trigger another user-parameter rebuild cascade.
				FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(System->GetExposedParameters());
				if (!SetStoreData(System->GetExposedParameters(), Variable, Data))
				{
					return Error(
						TEXT("Failed to write the exposed parameter store."),
						TEXT("parameter_store_write_failed"), 500);
				}
			}

			// Mark dirty only; this capability never requests a blocking compile.
			System->MarkPackageDirty();

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-parameter-added.v1"));
			Json->SetStringField(TEXT("system"), System->GetPathName());
			Json->SetStringField(TEXT("parameter"), TEXT("User.") + Name);
			Json->SetStringField(TEXT("type"), Type.GetName());
			Json->SetField(TEXT("value"), DataJson(Type, Data));
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), System->GetOutermost()->IsDirty());
			return FMCPToolResult::Ok(Json);
		}
	};

	class FParameterRemove final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.remove"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target, true);
			if (!Result.bSuccess)
			{
				// ResolveTarget reports parameter_not_found when the authored user
				// parameter name does not exist on the System.
				return Result;
			}

			UNiagaraSystem* System = Target.System;
			// Capture the authored (User.-prefixed) identity before any mutation;
			// RemoveUserScriptVariable compares the full variable, not the bare name.
			const FNiagaraVariable AuthoredVariable = Target.ScriptVariable->Variable;

			{
				// Suppress the store change delegate so the removal does not trigger
				// another user-parameter sync/rebuild cascade; the authored metadata
				// is removed explicitly below.
				FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(System->GetExposedParameters());
				if (!System->GetExposedParameters().RemoveParameter(Target.Variable))
				{
					return Error(
						TEXT("Failed to remove the user parameter from the exposed store."),
						TEXT("parameter_store_write_failed"), 500);
				}
			}

			// Remove the authored UNiagaraScriptVariable metadata so the parameter
			// stays gone across recompiles and re-syncs.
			UNiagaraSystemEditorData* EditorData = Cast<UNiagaraSystemEditorData>(System->GetEditorData());
			if (EditorData)
			{
				EditorData->RemoveUserScriptVariable(AuthoredVariable);
			}

			// Mark dirty only; this capability never requests a blocking compile.
			System->MarkPackageDirty();

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-parameter-removed.v1"));
			Json->SetStringField(TEXT("system"), System->GetPathName());
			Json->SetStringField(TEXT("parameter"), Target.ParameterName);
			Json->SetStringField(TEXT("type"), Target.Variable.GetType().GetName());
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), System->GetOutermost()->IsDirty());
			return FMCPToolResult::Ok(Json);
		}
	};

	class FParameterRename final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.rename"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FTarget Target;
			FMCPToolResult Result = ResolveTarget(Params, Target, true);
			if (!Result.bSuccess)
			{
				return Result;
			}

			FString NewName;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("newName"), NewName)
				|| NewName.IsEmpty() || NewName.Len() > MaxParameterCharacters)
			{
				return Error(TEXT("newName must be a non-empty user parameter name of at most 512 characters."));
			}
			NewName = ShortParameterName(NewName);
			const FString OldName = ShortParameterName(Target.Variable.GetName().ToString());
			if (NewName.Equals(OldName, ESearchCase::IgnoreCase))
			{
				return Error(TEXT("The new name matches the current parameter name."), TEXT("already_exists"), 409);
			}
			TArray<FNiagaraVariable> UserParameters;
			Target.System->GetExposedParameters().GetUserParameters(UserParameters);
			for (const FNiagaraVariable& Variable : UserParameters)
			{
				if (ShortParameterName(Variable.GetName().ToString()).Equals(NewName, ESearchCase::IgnoreCase))
				{
					return Error(
						TEXT("A user parameter with the new name already exists."), TEXT("already_exists"), 409);
				}
			}

			const FNiagaraVariable OldVariable = Target.Variable;
			const FName NewFullName(*(TEXT("User.") + NewName));

			{
				// Suppress the store change delegate so the rename does not trigger a
				// redundant user-parameter sync/rebuild cascade.
				FNiagaraParameterStore::FScopedSuppressOnChanged Suppress(Target.System->GetExposedParameters());
				Target.System->GetExposedParameters().RenameParameter(OldVariable, NewFullName);
			}
			if (Target.ScriptVariable)
			{
				Target.ScriptVariable->Modify();
				Target.ScriptVariable->Variable.SetName(NewFullName);
			}
			Target.System->MarkPackageDirty();

			auto Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("schema"), TEXT("ue.niagara.system-parameter-renamed.v1"));
			Json->SetStringField(TEXT("system"), Target.System->GetPathName());
			Json->SetStringField(TEXT("oldParameter"), OldName);
			Json->SetStringField(TEXT("parameter"), NewName);
			Json->SetStringField(TEXT("type"), OldVariable.GetType().GetName());
			Json->SetBoolField(TEXT("saved"), false);
			Json->SetBoolField(TEXT("dirty"), Target.System->GetOutermost()->IsDirty());
			return FMCPToolResult::Ok(Json);
		}
	};

	class FParameterPlan final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.plan"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FPlan Plan;
			FMCPToolResult Result = BuildPlan(Params, Plan);
			if (!Result.bSuccess) return Result;
			Plan.Json->SetStringField(TEXT("planDigest"), Plan.PlanDigest);
			return FMCPToolResult::Ok(Plan.Json);
		}
	};

	class FParameterApply final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.apply"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString RequestId;
			FString InputDigest;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("requestId"), RequestId)
				|| RequestId.IsEmpty() || RequestId.Len() > 128)
			{
				return Error(TEXT("A non-empty requestId is required."), TEXT("request_id_required"));
			}
			if (!RequestDigest(Params, InputDigest))
			{
				return Error(TEXT("Could not hash the request."), TEXT("digest_unavailable"), 500);
			}
			FString Code;
			FString Message;
			if (FString* ExistingId = RequestReceipts.Find(RequestId))
			{
				FReceipt* Existing = Receipts.Find(*ExistingId);
				if (!Existing || Existing->RequestDigest != InputDigest)
				{
					return Error(
						TEXT("requestId was already used for different parameter arguments."),
						TEXT("request_id_conflict"), 409);
				}
				if (!ValidateChangeApproval(Params, Existing->PlanDigest, Code, Message))
				{
					return Error(Message, *Code, 409);
				}
				if (!Existing->bVerified)
				{
					return Error(
						FString::Printf(
							TEXT("The previous write failed verification. Inspect receipt %s before another write."),
							*Existing->Id),
						TEXT("receipt_verification_failed"), 409);
				}
				FTarget Target;
				FMCPToolResult Result = ReceiptTarget(*Existing, Target);
				if (!Result.bSuccess) return Result;
				FState Current;
				Result = CaptureState(Target, Current);
				if (!Result.bSuccess) return Result;
				if (Current.Digest != (Existing->bRolledBack ? Existing->RestoredDigest : Existing->AfterDigest))
				{
					return Error(
						TEXT("The authored parameter changed after this request."), TEXT("receipt_state_changed"), 409);
				}
				return FMCPToolResult::Ok(ReceiptJson(*Existing, Target.Variable.GetType(), true));
			}
			if (const FReleasedRequest* Released = ReleasedRequests.Find(RequestId))
			{
				if (Released->RequestDigest != InputDigest)
				{
					return Error(
						TEXT("requestId was already used for different parameter arguments."),
						TEXT("request_id_conflict"), 409);
				}
				return Error(FString::Printf(
					             TEXT(
						             "requestId was already completed and receipt %s was released; use a new requestId for another write."),
					             *Released->ReceiptId), TEXT("receipt_released"), 409);
			}
			if (Receipts.Num() >= MaxReceipts)
			{
				return Error(TEXT("The session receipt limit was reached."), TEXT("receipt_limit"), 409);
			}

			FPlan Plan;
			FMCPToolResult Result = BuildPlan(Params, Plan);
			if (!Result.bSuccess) return Result;
			if (!ValidateChangeApproval(Params, Plan.PlanDigest, Code, Message))
			{
				return Error(Message, *Code, 409);
			}
			FReceipt Receipt;
			Receipt.Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			Receipt.RequestId = RequestId;
			Receipt.RequestDigest = InputDigest;
			Receipt.PlanDigest = Plan.PlanDigest;
			Receipt.System = Plan.Target.System;
			Receipt.ScriptVariable = Plan.Target.ScriptVariable;
			Receipt.SystemPath = Plan.Target.System->GetPathName();
			Receipt.ParameterName = Plan.Target.ParameterName;
			Receipt.TypeName = Plan.Target.Variable.GetType().GetName();
			Receipt.BeforeStoreData = Plan.Before.StoreData;
			Receipt.BeforeScriptData = Plan.Before.ScriptData;
			Receipt.DesiredData = Plan.DesiredData;
			Receipt.BeforeDigest = Plan.Before.Digest;
			Receipt.BeforeStoreRawDigest = Plan.Before.StoreRawDigest;
			Receipt.BeforeAuthoredDefaultRawDigest = Plan.Before.AuthoredDefaultRawDigest;
			Receipt.DesiredRawDigest = Plan.DesiredRawDigest;
			Receipt.bBeforeStoreValueRepresentable = Plan.Before.bStoreValueRepresentable;
			Receipt.bBeforeAuthoredDefaultValueRepresentable = Plan.Before.bAuthoredDefaultValueRepresentable;
			Receipt.bBeforeDirty = Plan.Target.System->GetOutermost()->IsDirty();
			Receipt.bChanged = Plan.Before.StoreData != Plan.DesiredData || Plan.Before.ScriptData != Plan.DesiredData;
			Receipts.Add(Receipt.Id, Receipt);
			RequestReceipts.Add(RequestId, Receipt.Id);

			TUniquePtr<FScopedTransaction> Transaction;
			FMCPToolResult MutationResult = FMCPToolResult::Ok(nullptr);
			bool bMutationMade = false;
			if (Receipt.bChanged)
			{
				Transaction = MakeUnique<FScopedTransaction>(
					FText::FromString(TEXT("UE AI Set Niagara User Parameter Default")));
				Plan.Target.System->Modify();
				Plan.Target.ScriptVariable->Modify();
				MutationResult = WriteState(Plan.Target, Plan.DesiredData, Plan.DesiredData, bMutationMade);
				if (bMutationMade)
				{
					Plan.Target.System->RequestCompile(false);
					Receipt.bCompileRequested = true;
				}
			}
			Receipt.bAfterDirty = Plan.Target.System->GetOutermost()->IsDirty();
			FState After;
			const FMCPToolResult AfterResult = CaptureState(Plan.Target, After);
			if (AfterResult.bSuccess)
			{
				Receipt.AfterDigest = After.Digest;
			}
			const bool bReadback = MutationResult.bSuccess && AfterResult.bSuccess
				&& After.StoreData == Plan.DesiredData && After.ScriptData == Plan.DesiredData;
			if (!bReadback)
			{
				Receipt.bRollbackAttempted = true;
				bool bRestoreMutation = false;
				const FMCPToolResult RestoreResult = WriteState(
					Plan.Target, Receipt.BeforeStoreData, Receipt.BeforeScriptData, bRestoreMutation);
				if (bRestoreMutation)
				{
					Plan.Target.System->RequestCompile(false);
					Receipt.bCompileRequested = true;
				}
				FState Restored;
				const FMCPToolResult RestoredResult = CaptureState(Plan.Target, Restored);
				if (RestoredResult.bSuccess)
				{
					Receipt.RestoredDigest = Restored.Digest;
				}
				Receipt.bSemanticRollbackVerified = RestoredResult.bSuccess && Restored.Digest == Receipt.BeforeDigest;
				if (Receipt.bSemanticRollbackVerified)
				{
					RestoreDirtyState(Plan.Target.System->GetOutermost(), Receipt.bBeforeDirty, true, Receipt);
				}
				Receipt.bRolledBack = Receipt.bSemanticRollbackVerified;
				Receipt.bVerified = Receipt.bSemanticRollbackVerified;
				if (Receipt.bSemanticRollbackVerified && Transaction)
				{
					Transaction->Cancel();
				}
				Receipts.Add(Receipt.Id, Receipt);
				return Error(FString::Printf(
					             TEXT(
						             "Parameter write verification failed; receipt %s records semanticRollback=%s, dirtyRestored=%s, restoreOperation=%s."),
					             *Receipt.Id,
					             Receipt.bSemanticRollbackVerified ? TEXT("verified") : TEXT("unverified"),
					             Receipt.bDirtyRestored ? TEXT("true") : TEXT("false"),
					             RestoreResult.bSuccess ? TEXT("completed") : TEXT("failed")),
				             TEXT("parameter_verification_failed"), 500);
			}
			Receipt.bVerified = true;
			Receipts.Add(Receipt.Id, Receipt);
			return FMCPToolResult::Ok(ReceiptJson(Receipt, Plan.Target.Variable.GetType(), false));
		}
	};

	class FParameterRollback final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.rollback"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Id;
			FString RequestId;
			bool bConfirmWrite = false;
			if (!Params.IsValid() || !Params->TryGetStringField(TEXT("rollbackId"), Id)
				|| !Params->TryGetStringField(TEXT("requestId"), RequestId)
				|| !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite) || !bConfirmWrite)
			{
				return Error(
					TEXT("rollbackId, requestId and confirmWrite=true are required."),
					TEXT("write_confirmation_required"));
			}
			FReceipt* Receipt = Receipts.Find(Id);
			if (!Receipt)
			{
				return Error(
					TEXT("The receipt is unavailable in this Editor instance."), TEXT("receipt_not_found"), 404);
			}
			if (Receipt->RequestId != RequestId)
			{
				return Error(TEXT("requestId does not match the receipt."), TEXT("request_id_mismatch"), 409);
			}
			if (!Receipt->bVerified)
			{
				return Error(
					TEXT(
						"The original write has no verified state for automatic rollback; inspect the retained Editor transaction."),
					TEXT("receipt_verification_failed"), 409);
			}
			FTarget Target;
			FMCPToolResult Result = ReceiptTarget(*Receipt, Target);
			if (!Result.bSuccess) return Result;
			FState Current;
			Result = CaptureState(Target, Current);
			if (!Result.bSuccess) return Result;
			if (Current.Digest != (Receipt->bRolledBack ? Receipt->RestoredDigest : Receipt->AfterDigest))
			{
				return Error(
					TEXT("The authored parameter changed after apply; rollback was refused."),
					TEXT("rollback_conflict"), 409);
			}
			if (Receipt->bRolledBack)
			{
				return FMCPToolResult::Ok(ReceiptJson(*Receipt, Target.Variable.GetType(), true));
			}

			TUniquePtr<FScopedTransaction> Transaction;
			Receipt->bRollbackAttempted = true;
			Receipt->bVerified = false;
			bool bMutationMade = false;
			FMCPToolResult RestoreResult = FMCPToolResult::Ok(nullptr);
			if (Receipt->bChanged)
			{
				Transaction = MakeUnique<FScopedTransaction>(
					FText::FromString(TEXT("UE AI Rollback Niagara User Parameter Default")));
				Target.System->Modify();
				Target.ScriptVariable->Modify();
				RestoreResult = WriteState(Target, Receipt->BeforeStoreData, Receipt->BeforeScriptData, bMutationMade);
				if (bMutationMade)
				{
					Target.System->RequestCompile(false);
					Receipt->bCompileRequested = true;
				}
			}
			FState Restored;
			Result = CaptureState(Target, Restored);
			if (Result.bSuccess)
			{
				Receipt->RestoredDigest = Restored.Digest;
			}
			Receipt->bSemanticRollbackVerified = Result.bSuccess && Restored.Digest == Receipt->BeforeDigest;
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
						             "Rollback outcome is unknown; semanticRollback=%s, dirtyRestored=%s, restoreOperation=%s."),
					             Receipt->bSemanticRollbackVerified ? TEXT("verified") : TEXT("unverified"),
					             Receipt->bDirtyRestored ? TEXT("true") : TEXT("false"),
					             RestoreResult.bSuccess ? TEXT("completed") : TEXT("failed")),
				             TEXT("rollback_verification_failed"), 500);
			}
			return FMCPToolResult::Ok(ReceiptJson(*Receipt, Target.Variable.GetType(), false));
		}
	};

	class FParameterReceiptRelease final : public FMCPToolBase
	{
	public:
		FString GetCapabilityId() const override { return TEXT("content.niagara.system.parameter.receipt.release"); }

		FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
		{
			FString Id;
			FString RequestId;
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
}
#endif

namespace UEAIIntegrationTools
{
	void RegisterNiagaraSystemParameterTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
		using namespace UEAINiagaraSystemParameters;
		Registry.Register(MakeShared<FParameterAdd>());
		Registry.Register(MakeShared<FParameterRemove>());
		Registry.Register(MakeShared<FParameterRename>());
		Registry.Register(MakeShared<FParameterGet>());
		Registry.Register(MakeShared<FParameterPlan>());
		Registry.Register(MakeShared<FParameterApply>());
		Registry.Register(MakeShared<FParameterRollback>());
#else
		class FUnavailableSystemParameter final : public FMCPToolBase
		{
		public:
			explicit FUnavailableSystemParameter(const TCHAR* InId) : Id(InId)
			{
			}

			FString GetCapabilityId() const override { return Id; }

			FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
			{
				return FMCPToolResult::Error(
					TEXT("Niagara editor parameter support is unavailable in this build."),
					TEXT("capability_unavailable"), 409);
			}

		private:
			FString Id;
		};
		for (const TCHAR* Id : {
			     TEXT("content.niagara.system.parameter.add"),
			     TEXT("content.niagara.system.parameter.remove"),
			     TEXT("content.niagara.system.parameter.rename"),
			     TEXT("content.niagara.system.parameter.get"),
			     TEXT("content.niagara.system.parameter.plan"),
			     TEXT("content.niagara.system.parameter.apply"),
			     TEXT("content.niagara.system.parameter.rollback")
		     })
		{
			Registry.Register(MakeShared<FUnavailableSystemParameter>(Id));
		}
#endif
	}

	void RegisterNiagaraSystemParameterReceiptTools(FMCPToolRegistry& Registry)
	{
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
		using namespace UEAINiagaraSystemParameters;
		Registry.Register(MakeShared<FParameterReceiptRelease>());
#else
		class FUnavailableSystemParameterReceipt final : public FMCPToolBase
		{
		public:
			explicit FUnavailableSystemParameterReceipt(const TCHAR* InId)
				: Id(InId)
			{
			}

			FString GetCapabilityId() const override { return Id; }

			FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
			{
				return FMCPToolResult::Error(
					TEXT("Niagara editor parameter support is unavailable in this build."),
					TEXT("capability_unavailable"), 409);
			}

		private:
			FString Id;
		};
		Registry.Register(MakeShared<FUnavailableSystemParameterReceipt>(
			TEXT("content.niagara.system.parameter.receipt.release")));
#endif
	}
}
