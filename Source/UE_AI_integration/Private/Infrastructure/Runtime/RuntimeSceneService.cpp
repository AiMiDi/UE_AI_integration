#include "Infrastructure/Runtime/RuntimeSceneService.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Infrastructure/Runtime/SlateRuntimeInputService.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Layout/WidgetPath.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Widgets/SWidget.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
constexpr int32 DefaultFindLimit = 100;
constexpr int32 MaxFindLimit = 500;
constexpr int32 DefaultWidgetDepth = 32;
constexpr int32 MaxWidgetDepth = 64;

FRuntimeServiceResult InvalidParams(const FString& Message)
{
	return FRuntimeServiceResult::Error(TEXT("invalid_params"), Message, 422);
}

FRuntimeServiceResult RuntimeObjectNotFound(const FString& Message)
{
	return FRuntimeServiceResult::Error(TEXT("runtime_object_not_found"), Message, 404);
}

FString WorldTypeToString(EWorldType::Type WorldType)
{
	switch (WorldType)
	{
	case EWorldType::Game:
		return TEXT("Game");
	case EWorldType::Editor:
		return TEXT("Editor");
	case EWorldType::PIE:
		return TEXT("PIE");
	case EWorldType::EditorPreview:
		return TEXT("Preview");
	case EWorldType::GamePreview:
		return TEXT("GamePreview");
	case EWorldType::GameRPC:
		return TEXT("GameRPC");
	case EWorldType::Inactive:
		return TEXT("Inactive");
	default:
		return TEXT("None");
	}
}

bool IsMutableWorld(const UWorld* World)
{
	return World &&
		(World->WorldType == EWorldType::PIE || World->WorldType == EWorldType::Game);
}

bool IsQueryableWorld(const UWorld* World)
{
	return World &&
		(World->WorldType == EWorldType::PIE ||
			World->WorldType == EWorldType::Game ||
			World->WorldType == EWorldType::Editor ||
			World->WorldType == EWorldType::EditorPreview ||
			World->WorldType == EWorldType::GamePreview);
}

UWorld* GetObjectWorld(UObject* Object)
{
	if (!Object)
	{
		return nullptr;
	}
	if (UWorld* World = Cast<UWorld>(Object))
	{
		return World;
	}
	return Object->GetWorld();
}

TSharedPtr<FJsonObject> VectorToJson(const FVector2D& Vector)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("x"), Vector.X);
	Result->SetNumberField(TEXT("y"), Vector.Y);
	return Result;
}

bool ReadVector2D(
	const TSharedPtr<FJsonObject>& Params,
	const TCHAR* FieldName,
	TOptional<FVector2D>& OutValue)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Params->TryGetObjectField(FieldName, Object) || !Object || !Object->IsValid())
	{
		return false;
	}

	double X = 0.0;
	double Y = 0.0;
	if (!(*Object)->TryGetNumberField(TEXT("x"), X) ||
		!(*Object)->TryGetNumberField(TEXT("y"), Y))
	{
		return false;
	}
	OutValue = FVector2D(X, Y);
	return true;
}

FString VisibilityToString(ESlateVisibility Visibility)
{
	switch (Visibility)
	{
	case ESlateVisibility::Visible:
		return TEXT("visible");
	case ESlateVisibility::Collapsed:
		return TEXT("collapsed");
	case ESlateVisibility::Hidden:
		return TEXT("hidden");
	case ESlateVisibility::HitTestInvisible:
		return TEXT("hitTestInvisible");
	case ESlateVisibility::SelfHitTestInvisible:
		return TEXT("selfHitTestInvisible");
	default:
		return TEXT("unknown");
	}
}

bool IsCallableFunction(const UFunction* Function)
{
	if (!Function)
	{
		return false;
	}
	const bool bCallable =
		Function->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent);
	const bool bNetworked = Function->HasAnyFunctionFlags(FUNC_Net);
	const bool bLatent = Function->HasMetaData(TEXT("Latent"));
	return bCallable && !bNetworked && !bLatent;
}

bool IsInputParameter(const FProperty* Property)
{
	return Property->HasAnyPropertyFlags(CPF_Parm) &&
		!Property->HasAnyPropertyFlags(CPF_ReturnParm) &&
		!(Property->HasAnyPropertyFlags(CPF_OutParm) &&
			!Property->HasAnyPropertyFlags(CPF_ReferenceParm));
}

bool IsOutputParameter(const FProperty* Property)
{
	return Property->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm);
}

FString GetWorldId(UWorld* World)
{
	if (!GEngine || !World)
	{
		return FString();
	}
	if (const FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
	{
		return Context->ContextHandle.ToString();
	}
	return FString();
}
}

FRuntimeServiceResult FRuntimeServiceResult::Ok(const TSharedPtr<FJsonObject>& InData)
{
	FRuntimeServiceResult Result;
	Result.Data = InData;
	return Result;
}

FRuntimeServiceResult FRuntimeServiceResult::Error(
	const FString& InCode,
	const FString& InMessage,
	int32 InHttpStatus)
{
	FRuntimeServiceResult Result;
	Result.bSuccess = false;
	Result.ErrorCode = InCode;
	Result.ErrorMessage = InMessage;
	Result.HttpStatus = InHttpStatus;
	return Result;
}

class FRuntimeSceneService::FImpl
{
public:
	void PrepareNextSession()
	{
		if (bSessionPrepared)
		{
			return;
		}

		++Generation;
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		Objects.Reset();
		bSessionActive = false;
		bSessionPrepared = true;
		bPaused = false;
	}

	void BeginSession()
	{
		if (bSessionActive)
		{
			return;
		}
		if (!bSessionPrepared)
		{
			PrepareNextSession();
		}
		bSessionPrepared = false;
		bSessionActive = true;
		bPaused = false;
		Objects.Reset();
	}

	void CancelPreparedSession()
	{
		if (bSessionPrepared)
		{
			Objects.Reset();
			bSessionPrepared = false;
			bSessionActive = false;
			bPaused = false;
		}
	}

	void EndSession()
	{
		Objects.Reset();
		bSessionActive = false;
		bPaused = false;
	}

	FRuntimeServiceResult Resolve(
		const TSharedPtr<FJsonObject>& ObjectRef,
		UObject*& OutObject) const
	{
		OutObject = nullptr;
		if (!ObjectRef.IsValid())
		{
			return InvalidParams(TEXT("Missing objectRef."));
		}

		FString RequestedSession;
		FString ObjectId;
		double RequestedGeneration = 0.0;
		if (!ObjectRef->TryGetStringField(TEXT("sessionId"), RequestedSession) ||
			!ObjectRef->TryGetNumberField(TEXT("generation"), RequestedGeneration) ||
			!ObjectRef->TryGetStringField(TEXT("objectId"), ObjectId))
		{
			return InvalidParams(
				TEXT("objectRef requires sessionId, generation, and objectId."));
		}

		if (RequestedSession != SessionId ||
			static_cast<uint64>(RequestedGeneration) != Generation)
		{
			return FRuntimeServiceResult::Error(
				TEXT("stale_session_handle"),
				TEXT("The objectRef belongs to a different PIE generation."),
				410);
		}
		if (!bSessionActive)
		{
			return FRuntimeServiceResult::Error(
				TEXT("pie_not_running"),
				TEXT("The referenced PIE session is not running."),
				409);
		}

		FGuid Id;
		if (!FGuid::Parse(ObjectId, Id))
		{
			return InvalidParams(TEXT("objectRef.objectId is not a valid GUID."));
		}
		const TWeakObjectPtr<UObject>* WeakObject = Objects.Find(Id);
		if (!WeakObject || !WeakObject->IsValid())
		{
			return RuntimeObjectNotFound(
				TEXT("The runtime object no longer exists in this PIE session."));
		}

		OutObject = WeakObject->Get();
		return FRuntimeServiceResult::Ok(MakeShared<FJsonObject>());
	}

	FRuntimeServiceResult MakeRef(
		UObject* Object,
		TSharedPtr<FJsonObject>& OutObjectRef)
	{
		OutObjectRef.Reset();
		if (!bSessionActive)
		{
			return FRuntimeServiceResult::Error(
				TEXT("pie_not_running"),
				TEXT("PIE must be running before runtime handles can be created."),
				409);
		}
		if (!IsValid(Object))
		{
			return RuntimeObjectNotFound(TEXT("Cannot create a handle for an invalid object."));
		}
		if (!IsQueryableWorld(GetObjectWorld(Object)))
		{
			return RuntimeObjectNotFound(
				TEXT("Object does not belong to a queryable Editor or runtime world."));
		}

		FGuid ObjectId;
		for (const TPair<FGuid, TWeakObjectPtr<UObject>>& Pair : Objects)
		{
			if (Pair.Value.Get() == Object)
			{
				ObjectId = Pair.Key;
				break;
			}
		}
		if (!ObjectId.IsValid())
		{
			ObjectId = FGuid::NewGuid();
			Objects.Add(ObjectId, Object);
		}

		OutObjectRef = MakeShared<FJsonObject>();
		OutObjectRef->SetStringField(TEXT("sessionId"), SessionId);
		OutObjectRef->SetNumberField(TEXT("generation"), static_cast<double>(Generation));
		OutObjectRef->SetStringField(
			TEXT("objectId"),
			ObjectId.ToString(EGuidFormats::DigitsWithHyphensLower));
		return FRuntimeServiceResult::Ok(OutObjectRef);
	}

	TSharedPtr<FJsonObject> DescribeObject(UObject* Object)
	{
		TSharedPtr<FJsonObject> ObjectRef;
		if (!MakeRef(Object, ObjectRef).bSuccess)
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Description = MakeShared<FJsonObject>();
		Description->SetObjectField(TEXT("objectRef"), ObjectRef);
		Description->SetStringField(TEXT("name"), Object->GetName());
		Description->SetStringField(TEXT("path"), Object->GetPathName());
		Description->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
		if (UWorld* World = GetObjectWorld(Object))
		{
			Description->SetStringField(TEXT("worldId"), GetWorldId(World));
			Description->SetStringField(TEXT("worldType"), WorldTypeToString(World->WorldType));
			Description->SetBoolField(TEXT("mutable"), IsMutableWorld(World));
		}
		return Description;
	}

	UWorld* FindWorld(const FString& WorldId) const
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (WorldId.IsEmpty() || Context.ContextHandle.ToString() == WorldId)
			{
				if (UWorld* World = Context.World())
				{
					if (IsQueryableWorld(World))
					{
						return World;
					}
				}
			}
		}
		return nullptr;
	}

	bool IsMutationAllowed(UObject* Object, FRuntimeServiceResult& OutError) const
	{
		if (!IsMutableWorld(GetObjectWorld(Object)))
		{
			OutError = FRuntimeServiceResult::Error(
				TEXT("runtime_world_read_only"),
				TEXT("Mutations are allowed only for PIE or Game world objects."),
				403);
			return false;
		}
		return true;
	}

	FRuntimeServiceResult ResolveParamObject(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* FieldName,
		UObject*& OutObject) const
	{
		const TSharedPtr<FJsonObject>* ObjectRef = nullptr;
		if (!Params->TryGetObjectField(FieldName, ObjectRef) ||
			!ObjectRef ||
			!ObjectRef->IsValid())
		{
			return InvalidParams(
				FString::Printf(TEXT("Missing '%s' objectRef."), FieldName));
		}
		return Resolve(*ObjectRef, OutObject);
	}

	FRuntimeServiceResult ResolveDelegate(
		const TSharedPtr<FJsonObject>& Params,
		UObject*& OutOwner,
		FMulticastDelegateProperty*& OutProperty,
		void*& OutPropertyValue,
		const FMulticastScriptDelegate*& OutDelegate) const
	{
		OutOwner = nullptr;
		OutProperty = nullptr;
		OutPropertyValue = nullptr;
		OutDelegate = nullptr;

		FRuntimeServiceResult OwnerResult =
			ResolveParamObject(Params, TEXT("objectRef"), OutOwner);
		if (!OwnerResult.bSuccess)
		{
			return OwnerResult;
		}

		FString DelegateName;
		if (!Params->TryGetStringField(TEXT("delegate"), DelegateName) ||
			DelegateName.IsEmpty())
		{
			return InvalidParams(TEXT("Missing 'delegate'."));
		}
		OutProperty = FindFProperty<FMulticastDelegateProperty>(
			OutOwner->GetClass(),
			FName(*DelegateName));
		if (!OutProperty)
		{
			return RuntimeObjectNotFound(
				FString::Printf(TEXT("Dynamic multicast delegate '%s' was not found."), *DelegateName));
		}
		OutPropertyValue = OutProperty->ContainerPtrToValuePtr<void>(OutOwner);
		OutDelegate = OutProperty->GetMulticastDelegate(OutPropertyValue);
		if (!OutDelegate)
		{
			return RuntimeObjectNotFound(
				FString::Printf(TEXT("Delegate '%s' has no runtime value."), *DelegateName));
		}
		return FRuntimeServiceResult::Ok(MakeShared<FJsonObject>());
	}

	FSlateRuntimeInputService Input;
	TMap<FGuid, TWeakObjectPtr<UObject>> Objects;
	FString SessionId;
	uint64 Generation = 0;
	bool bSessionPrepared = false;
	bool bSessionActive = false;
	bool bPaused = false;
};

FRuntimeSceneService::FRuntimeSceneService()
	: Impl(MakeUnique<FImpl>())
{
}

FRuntimeSceneService::~FRuntimeSceneService() = default;

void FRuntimeSceneService::PrepareNextSession()
{
	Impl->PrepareNextSession();
}

void FRuntimeSceneService::BeginSession()
{
	Impl->BeginSession();
}

void FRuntimeSceneService::CancelPreparedSession()
{
	Impl->CancelPreparedSession();
}

void FRuntimeSceneService::EndSession()
{
	Impl->EndSession();
}

void FRuntimeSceneService::SetPaused(bool bInPaused)
{
	Impl->bPaused = bInPaused;
}

const FString& FRuntimeSceneService::GetSessionId() const
{
	return Impl->SessionId;
}

uint64 FRuntimeSceneService::GetGeneration() const
{
	return Impl->Generation;
}

bool FRuntimeSceneService::IsSessionActive() const
{
	return Impl->bSessionActive;
}

bool FRuntimeSceneService::IsPaused() const
{
	return Impl->bPaused;
}

FRuntimeServiceResult FRuntimeSceneService::ResolveObjectRef(
	const TSharedPtr<FJsonObject>& ObjectRef,
	UObject*& OutObject) const
{
	return Impl->Resolve(ObjectRef, OutObject);
}

FRuntimeServiceResult FRuntimeSceneService::MakeObjectRef(
	UObject* Object,
	TSharedPtr<FJsonObject>& OutObjectRef)
{
	return Impl->MakeRef(Object, OutObjectRef);
}

FRuntimeServiceResult FRuntimeSceneService::ListWorldContexts(
	const TSharedPtr<FJsonObject>& Params)
{
	TArray<TSharedPtr<FJsonValue>> Worlds;
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World || !IsQueryableWorld(World))
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("worldId"), Context.ContextHandle.ToString());
			Entry->SetStringField(TEXT("type"), WorldTypeToString(Context.WorldType));
			Entry->SetStringField(TEXT("name"), World->GetName());
			Entry->SetStringField(TEXT("path"), World->GetPathName());
			Entry->SetBoolField(TEXT("mutable"), IsMutableWorld(World));
			Entry->SetBoolField(
				TEXT("currentSession"),
				Impl->bSessionActive && IsMutableWorld(World));
			Worlds.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("worlds"), Worlds);
	Data->SetNumberField(TEXT("count"), Worlds.Num());
	Data->SetStringField(TEXT("sessionId"), Impl->SessionId);
	Data->SetNumberField(TEXT("generation"), static_cast<double>(Impl->Generation));
	Data->SetBoolField(TEXT("sessionActive"), Impl->bSessionActive);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::FindObjects(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before runtime objects can be found."),
			409);
	}

	FString WorldId;
	FString ClassFilter;
	FString NameFilter;
	FString PathFilter;
	Params->TryGetStringField(TEXT("worldId"), WorldId);
	Params->TryGetStringField(TEXT("class"), ClassFilter);
	Params->TryGetStringField(TEXT("name"), NameFilter);
	Params->TryGetStringField(TEXT("path"), PathFilter);
	if (ClassFilter.IsEmpty() && NameFilter.IsEmpty() && PathFilter.IsEmpty())
	{
		return InvalidParams(TEXT("At least one of class, name, or path is required."));
	}

	int32 Limit = DefaultFindLimit;
	double LimitNumber = 0.0;
	if (Params->TryGetNumberField(TEXT("limit"), LimitNumber))
	{
		Limit = FMath::Clamp(static_cast<int32>(LimitNumber), 1, MaxFindLimit);
	}
	if (!WorldId.IsEmpty() && !Impl->FindWorld(WorldId))
	{
		return RuntimeObjectNotFound(
			FString::Printf(TEXT("World context '%s' was not found."), *WorldId));
	}

	TArray<TSharedPtr<FJsonValue>> Matches;
	for (TObjectIterator<UObject> It; It && Matches.Num() < Limit; ++It)
	{
		UObject* Object = *It;
		if (!IsValid(Object) ||
			Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) ||
			!IsQueryableWorld(GetObjectWorld(Object)))
		{
			continue;
		}
		if (!WorldId.IsEmpty() && GetWorldId(GetObjectWorld(Object)) != WorldId)
		{
			continue;
		}
		if (WorldId.IsEmpty() && !IsMutableWorld(GetObjectWorld(Object)))
		{
			continue;
		}

		const bool bClassMatches =
			ClassFilter.IsEmpty() ||
			Object->GetClass()->GetName().MatchesWildcard(ClassFilter, ESearchCase::IgnoreCase) ||
			Object->GetClass()->GetPathName().MatchesWildcard(ClassFilter, ESearchCase::IgnoreCase);
		const bool bNameMatches =
			NameFilter.IsEmpty() ||
			Object->GetName().MatchesWildcard(NameFilter, ESearchCase::IgnoreCase);
		const bool bPathMatches =
			PathFilter.IsEmpty() ||
			Object->GetPathName().MatchesWildcard(PathFilter, ESearchCase::IgnoreCase);
		if (!bClassMatches || !bNameMatches || !bPathMatches)
		{
			continue;
		}

		if (TSharedPtr<FJsonObject> Description = Impl->DescribeObject(Object))
		{
			Matches.Add(MakeShared<FJsonValueObject>(Description));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("objects"), Matches);
	Data->SetNumberField(TEXT("count"), Matches.Num());
	Data->SetBoolField(TEXT("truncated"), Matches.Num() == Limit);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::GetObject(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	TSet<FString> RequestedProperties;
	const TArray<TSharedPtr<FJsonValue>>* PropertyNames = nullptr;
	if (Params->TryGetArrayField(TEXT("properties"), PropertyNames))
	{
		for (const TSharedPtr<FJsonValue>& Value : *PropertyNames)
		{
			RequestedProperties.Add(Value->AsString());
		}
	}

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		const FString PropertyName = Property->GetName();
		if (!RequestedProperties.IsEmpty() && !RequestedProperties.Contains(PropertyName))
		{
			continue;
		}
		if (RequestedProperties.IsEmpty() &&
			!Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
		{
			continue;
		}

		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		TSharedPtr<FJsonValue> JsonValue =
			FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			Properties->SetField(PropertyName, JsonValue);
		}
	}

	TSharedPtr<FJsonObject> Data = Impl->DescribeObject(Object);
	if (!Data.IsValid())
	{
		return RuntimeObjectNotFound(TEXT("Failed to describe the runtime object."));
	}
	Data->SetObjectField(TEXT("properties"), Properties);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::SetObject(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Object, MutationError))
	{
		return MutationError;
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property"), PropertyName) ||
		PropertyName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'property'."));
	}
	const TSharedPtr<FJsonValue> JsonValue = Params->TryGetField(TEXT("value"));
	if (!JsonValue.IsValid())
	{
		return InvalidParams(TEXT("Missing 'value'."));
	}

	FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), FName(*PropertyName));
	if (!Property)
	{
		return RuntimeObjectNotFound(
			FString::Printf(TEXT("Property '%s' was not found."), *PropertyName));
	}
	if (!Property->HasAnyPropertyFlags(CPF_BlueprintVisible) ||
		Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
	{
		return FRuntimeServiceResult::Error(
			TEXT("unsupported_property_type"),
			FString::Printf(TEXT("Property '%s' is not runtime writable."), *PropertyName),
			422);
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
	FText FailureReason;
	if (!FJsonObjectConverter::JsonValueToUProperty(
			JsonValue,
			Property,
			ValuePtr,
			0,
			0,
			true,
			&FailureReason))
	{
		return FRuntimeServiceResult::Error(
			TEXT("unsupported_property_type"),
			FString::Printf(
				TEXT("Failed to set '%s': %s"),
				*PropertyName,
				*FailureReason.ToString()),
			422);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("property"), PropertyName);
	Data->SetBoolField(TEXT("changed"), true);
	const TSharedPtr<FJsonValue> ReadBack =
		FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr);
	if (ReadBack.IsValid())
	{
		Data->SetField(TEXT("value"), ReadBack);
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::CallObject(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Object, MutationError))
	{
		return MutationError;
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}
	UFunction* Function = Object->FindFunction(FName(*FunctionName));
	if (!Function)
	{
		return RuntimeObjectNotFound(
			FString::Printf(TEXT("Function '%s' was not found."), *FunctionName));
	}
	if (!IsCallableFunction(Function))
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			TEXT("Only non-latent, non-network BlueprintCallable/BlueprintEvent functions may be called."),
			422);
	}

	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	Params->TryGetObjectField(TEXT("args"), Arguments);
	FStructOnScope ParameterMemory(Function);
	void* ParameterBuffer = ParameterMemory.GetStructMemory();

	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsInputParameter(Property))
		{
			continue;
		}
		const TSharedPtr<FJsonValue>* Argument =
			Arguments && Arguments->IsValid()
				? (*Arguments)->Values.Find(Property->GetName())
				: nullptr;
		if (!Argument || !Argument->IsValid())
		{
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ParameterBuffer);
		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const TSharedPtr<FJsonObject>* ArgumentRef = nullptr;
			if ((*Argument)->Type != EJson::Object ||
				!(*Argument)->AsObject()->TryGetObjectField(
					TEXT("objectRef"),
					ArgumentRef))
			{
				return InvalidParams(
					FString::Printf(
						TEXT("Object parameter '%s' requires {objectRef:{...}}."),
						*Property->GetName()));
			}
			UObject* ArgumentObject = nullptr;
			FRuntimeServiceResult ArgumentResult = Impl->Resolve(*ArgumentRef, ArgumentObject);
			if (!ArgumentResult.bSuccess)
			{
				return ArgumentResult;
			}
			if (!ArgumentObject->IsA(ObjectProperty->PropertyClass))
			{
				return FRuntimeServiceResult::Error(
					TEXT("signature_mismatch"),
					FString::Printf(
						TEXT("Object parameter '%s' has incompatible class."),
						*Property->GetName()),
					422);
			}
			ObjectProperty->SetObjectPropertyValue(ValuePtr, ArgumentObject);
			continue;
		}

		FText FailureReason;
		if (!FJsonObjectConverter::JsonValueToUProperty(
				*Argument,
				Property,
				ValuePtr,
				0,
				0,
				true,
				&FailureReason))
		{
			return FRuntimeServiceResult::Error(
				TEXT("signature_mismatch"),
				FString::Printf(
					TEXT("Invalid parameter '%s': %s"),
					*Property->GetName(),
					*FailureReason.ToString()),
				422);
		}
	}

	Object->ProcessEvent(Function, ParameterBuffer);

	TSharedPtr<FJsonObject> Outputs = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsOutputParameter(Property))
		{
			continue;
		}
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(ParameterBuffer);
		TSharedPtr<FJsonValue> JsonValue =
			FJsonObjectConverter::UPropertyToJsonValue(Property, ValuePtr);
		if (JsonValue.IsValid())
		{
			Outputs->SetField(Property->GetName(), JsonValue);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetObjectField(TEXT("outputs"), Outputs);
	Data->SetBoolField(TEXT("called"), true);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::GetWidgetTree(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	UWidget* Widget = Cast<UWidget>(Object);
	UWidget* RootWidget = Widget;
	if (UUserWidget* UserWidget = Cast<UUserWidget>(Object))
	{
		RootWidget = UserWidget->WidgetTree ? UserWidget->WidgetTree->RootWidget : nullptr;
	}
	if (!RootWidget)
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("The object has no runtime widget tree."),
			422);
	}

	int32 MaxDepth = DefaultWidgetDepth;
	double MaxDepthNumber = 0.0;
	if (Params->TryGetNumberField(TEXT("maxDepth"), MaxDepthNumber))
	{
		MaxDepth = FMath::Clamp(
			static_cast<int32>(MaxDepthNumber),
			0,
			MaxWidgetDepth);
	}

	TFunction<TSharedPtr<FJsonObject>(UWidget*, int32)> BuildNode =
		[this, MaxDepth, &BuildNode](UWidget* Current, int32 Depth)
		{
			if (!Current)
			{
				return TSharedPtr<FJsonObject>();
			}

			TSharedPtr<FJsonObject> ObjectRef;
			if (!Impl->MakeRef(Current, ObjectRef).bSuccess)
			{
				return TSharedPtr<FJsonObject>();
			}

			TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
			Node->SetObjectField(TEXT("objectRef"), ObjectRef);
			Node->SetStringField(TEXT("name"), Current->GetName());
			Node->SetStringField(TEXT("class"), Current->GetClass()->GetPathName());
			Node->SetStringField(TEXT("visibility"), VisibilityToString(Current->GetVisibility()));
			Node->SetBoolField(TEXT("enabled"), Current->GetIsEnabled());
			Node->SetNumberField(TEXT("depth"), Depth);
			if (Current->Slot)
			{
				Node->SetStringField(TEXT("slotClass"), Current->Slot->GetClass()->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> Children;
			if (Depth < MaxDepth)
			{
				if (UPanelWidget* Panel = Cast<UPanelWidget>(Current))
				{
					for (int32 Index = 0; Index < Panel->GetChildrenCount(); ++Index)
					{
						if (TSharedPtr<FJsonObject> Child =
								BuildNode(Panel->GetChildAt(Index), Depth + 1))
						{
							Children.Add(MakeShared<FJsonValueObject>(Child));
						}
					}
				}
			}
			Node->SetArrayField(TEXT("children"), Children);
			Node->SetBoolField(
				TEXT("truncated"),
				Depth >= MaxDepth &&
					Cast<UPanelWidget>(Current) &&
					Cast<UPanelWidget>(Current)->GetChildrenCount() > 0);
			return Node;
		};

	TSharedPtr<FJsonObject> Root = BuildNode(RootWidget, 0);
	if (!Root.IsValid())
	{
		return RuntimeObjectNotFound(TEXT("Failed to build the runtime widget tree."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("root"), Root);
	Data->SetNumberField(TEXT("maxDepth"), MaxDepth);
	if (Object != RootWidget)
	{
		TSharedPtr<FJsonObject> OwnerRef;
		if (Impl->MakeRef(Object, OwnerRef).bSuccess)
		{
			Data->SetObjectField(TEXT("ownerRef"), OwnerRef);
		}
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::GetWidgetState(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	UWidget* Widget = Cast<UWidget>(Object);
	if (!Widget)
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("objectRef does not resolve to a UWidget."),
			422);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Widget->GetName());
	Data->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());
	Data->SetStringField(TEXT("visibility"), VisibilityToString(Widget->GetVisibility()));
	Data->SetBoolField(TEXT("enabled"), Widget->GetIsEnabled());
	Data->SetBoolField(TEXT("hovered"), Widget->IsHovered());
	Data->SetBoolField(TEXT("keyboardFocused"), Widget->HasKeyboardFocus());
	Data->SetObjectField(TEXT("desiredSize"), VectorToJson(Widget->GetDesiredSize()));

	const TSharedPtr<SWidget> CachedWidget = Widget->GetCachedWidget();
	Data->SetBoolField(TEXT("constructed"), CachedWidget.IsValid());
	if (CachedWidget.IsValid())
	{
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		Data->SetObjectField(
			TEXT("absolutePosition"),
			VectorToJson(FVector2D(Geometry.GetAbsolutePosition())));
		Data->SetObjectField(
			TEXT("absoluteSize"),
			VectorToJson(FVector2D(Geometry.GetAbsoluteSize())));
		Data->SetObjectField(
			TEXT("localSize"),
			VectorToJson(FVector2D(Geometry.GetLocalSize())));
		Data->SetStringField(TEXT("slateType"), CachedWidget->GetTypeAsString());
	}
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::HitTestWidget(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!FSlateApplication::IsInitialized())
	{
		return FRuntimeServiceResult::Error(
			TEXT("input_dispatch_failed"),
			TEXT("Slate application is not initialized."),
			422);
	}

	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	UWidget* Widget = Cast<UWidget>(Object);
	if (!Widget || !Widget->GetCachedWidget().IsValid())
	{
		return FRuntimeServiceResult::Error(
			TEXT("widget_not_interactable"),
			TEXT("Widget has no cached Slate widget."),
			422);
	}

	TOptional<FVector2D> Position;
	if (!ReadVector2D(Params, TEXT("position"), Position))
	{
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		Position = FVector2D(
			Geometry.LocalToAbsolute(Geometry.GetLocalSize() * 0.5f));
	}

	const FWidgetPath Path = FSlateApplication::Get().LocateWindowUnderMouse(
		Position.GetValue(),
		FSlateApplication::Get().GetInteractiveTopLevelWindows(),
		true);

	TArray<TSharedPtr<FJsonValue>> PathEntries;
	const TSharedPtr<SWidget> TargetSlateWidget = Widget->GetCachedWidget();
	int32 TargetIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Path.Widgets.Num(); ++Index)
	{
		const TSharedRef<SWidget> SlateWidget = Path.Widgets[Index].Widget;
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"), Index);
		Entry->SetStringField(TEXT("type"), SlateWidget->GetTypeAsString());
		Entry->SetBoolField(TEXT("target"), SlateWidget == TargetSlateWidget);
		if (SlateWidget == TargetSlateWidget)
		{
			TargetIndex = Index;
		}
		PathEntries.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("position"), VectorToJson(Position.GetValue()));
	Data->SetBoolField(TEXT("pathValid"), Path.IsValid());
	Data->SetBoolField(TEXT("containsTarget"), TargetIndex != INDEX_NONE);
	Data->SetNumberField(TEXT("targetIndex"), TargetIndex);
	Data->SetArrayField(TEXT("path"), PathEntries);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::FocusWidget(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Object, MutationError))
	{
		return MutationError;
	}

	double UserIndexNumber = 0.0;
	Params->TryGetNumberField(TEXT("userIndex"), UserIndexNumber);
	return Impl->Input.Focus(
		Cast<UWidget>(Object),
		static_cast<uint32>(FMath::Max(0.0, UserIndexNumber)));
}

FRuntimeServiceResult FRuntimeSceneService::ListDelegates(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Object = nullptr;
	FRuntimeServiceResult ResolveResult =
		Impl->ResolveParamObject(Params, TEXT("objectRef"), Object);
	if (!ResolveResult.bSuccess)
	{
		return ResolveResult;
	}

	TArray<TSharedPtr<FJsonValue>> Delegates;
	for (TFieldIterator<FMulticastDelegateProperty> It(Object->GetClass()); It; ++It)
	{
		FMulticastDelegateProperty* Property = *It;
		void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
		const FMulticastScriptDelegate* Delegate =
			Property->GetMulticastDelegate(PropertyValue);
		if (!Delegate)
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Property->GetName());
		Entry->SetStringField(
			TEXT("signature"),
			Property->SignatureFunction
				? Property->SignatureFunction->GetPathName()
				: FString());
		Entry->SetBoolField(TEXT("bound"), Delegate->IsBound());

		TArray<TSharedPtr<FJsonValue>> BoundObjects;
		for (UObject* BoundObject : Delegate->GetAllObjects())
		{
			TSharedPtr<FJsonObject> BoundRef;
			if (Impl->MakeRef(BoundObject, BoundRef).bSuccess)
			{
				BoundObjects.Add(MakeShared<FJsonValueObject>(BoundRef));
			}
		}
		Entry->SetNumberField(TEXT("boundObjectCount"), BoundObjects.Num());
		Entry->SetArrayField(TEXT("boundObjects"), BoundObjects);
		Delegates.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("delegates"), Delegates);
	Data->SetNumberField(TEXT("count"), Delegates.Num());
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::BindDelegate(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Owner, MutationError))
	{
		return MutationError;
	}

	UObject* FunctionTarget = nullptr;
	FRuntimeServiceResult TargetResult =
		Impl->ResolveParamObject(Params, TEXT("functionTargetRef"), FunctionTarget);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}
	if (GetObjectWorld(Owner) != GetObjectWorld(FunctionTarget))
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			TEXT("Delegate owner and function target must belong to the same world."),
			422);
	}

	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}
	UFunction* Function = FunctionTarget->FindFunction(FName(*FunctionName));
	if (!Function || !Property->SignatureFunction ||
		!Property->SignatureFunction->IsSignatureCompatibleWith(Function))
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			FString::Printf(
				TEXT("Function '%s' is missing or incompatible with delegate '%s'."),
				*FunctionName,
				*Property->GetName()),
			422);
	}

	const bool bAlreadyBound =
		Delegate->Contains(FunctionTarget, FName(*FunctionName));
	if (!bAlreadyBound)
	{
		FScriptDelegate ScriptDelegate;
		ScriptDelegate.BindUFunction(FunctionTarget, FName(*FunctionName));
		Property->AddDelegate(ScriptDelegate, Owner, PropertyValue);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetBoolField(TEXT("changed"), !bAlreadyBound);
	Data->SetBoolField(TEXT("bound"), true);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::UnbindDelegate(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Owner, MutationError))
	{
		return MutationError;
	}

	UObject* FunctionTarget = nullptr;
	FRuntimeServiceResult TargetResult =
		Impl->ResolveParamObject(Params, TEXT("functionTargetRef"), FunctionTarget);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}

	FScriptDelegate ScriptDelegate;
	ScriptDelegate.BindUFunction(FunctionTarget, FName(*FunctionName));
	const bool bWasBound = Delegate->Contains(FunctionTarget, FName(*FunctionName));
	if (bWasBound)
	{
		Property->RemoveDelegate(ScriptDelegate, Owner, PropertyValue);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetBoolField(TEXT("changed"), bWasBound);
	Data->SetBoolField(TEXT("bound"), false);
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::IsDelegateBound(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}

	UObject* FunctionTarget = nullptr;
	FRuntimeServiceResult TargetResult =
		Impl->ResolveParamObject(Params, TEXT("functionTargetRef"), FunctionTarget);
	if (!TargetResult.bSuccess)
	{
		return TargetResult;
	}
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function"), FunctionName) ||
		FunctionName.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'function'."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetStringField(TEXT("function"), FunctionName);
	Data->SetBoolField(
		TEXT("bound"),
		Delegate->Contains(FunctionTarget, FName(*FunctionName)));
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::BroadcastDelegate(
	const TSharedPtr<FJsonObject>& Params)
{
	UObject* Owner = nullptr;
	FMulticastDelegateProperty* Property = nullptr;
	void* PropertyValue = nullptr;
	const FMulticastScriptDelegate* Delegate = nullptr;
	FRuntimeServiceResult DelegateResult =
		Impl->ResolveDelegate(Params, Owner, Property, PropertyValue, Delegate);
	if (!DelegateResult.bSuccess)
	{
		return DelegateResult;
	}
	FRuntimeServiceResult MutationError;
	if (!Impl->IsMutationAllowed(Owner, MutationError))
	{
		return MutationError;
	}
	if (!Property->SignatureFunction)
	{
		return FRuntimeServiceResult::Error(
			TEXT("signature_mismatch"),
			TEXT("Delegate has no signature function."),
			422);
	}

	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	Params->TryGetObjectField(TEXT("args"), Arguments);
	FStructOnScope ParameterMemory(Property->SignatureFunction);
	void* ParameterBuffer = ParameterMemory.GetStructMemory();
	for (TFieldIterator<FProperty> It(Property->SignatureFunction); It; ++It)
	{
		FProperty* Parameter = *It;
		if (!IsInputParameter(Parameter))
		{
			continue;
		}
		const TSharedPtr<FJsonValue>* JsonValue =
			Arguments && Arguments->IsValid()
				? (*Arguments)->Values.Find(Parameter->GetName())
				: nullptr;
		if (!JsonValue || !JsonValue->IsValid())
		{
			continue;
		}
		FText FailureReason;
		if (!FJsonObjectConverter::JsonValueToUProperty(
				*JsonValue,
				Parameter,
				Parameter->ContainerPtrToValuePtr<void>(ParameterBuffer),
				0,
				0,
				true,
				&FailureReason))
		{
			return FRuntimeServiceResult::Error(
				TEXT("signature_mismatch"),
				FString::Printf(
					TEXT("Invalid delegate argument '%s': %s"),
					*Parameter->GetName(),
					*FailureReason.ToString()),
				422);
		}
	}

	Delegate->ProcessMulticastDelegate<UObject>(ParameterBuffer);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("delegate"), Property->GetName());
	Data->SetBoolField(TEXT("broadcast"), true);
	Data->SetNumberField(TEXT("boundObjectCount"), Delegate->GetAllObjects().Num());
	return FRuntimeServiceResult::Ok(Data);
}

FRuntimeServiceResult FRuntimeSceneService::PointerInput(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before input can be dispatched."),
			409);
	}

	FString Action;
	if (!Params->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'action'."));
	}
	FString Button = TEXT("left");
	Params->TryGetStringField(TEXT("button"), Button);

	UWidget* TargetWidget = nullptr;
	const TSharedPtr<FJsonObject>* TargetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("target"), TargetRef) &&
		TargetRef &&
		TargetRef->IsValid())
	{
		UObject* TargetObject = nullptr;
		FRuntimeServiceResult TargetResult = Impl->Resolve(*TargetRef, TargetObject);
		if (!TargetResult.bSuccess)
		{
			return TargetResult;
		}
		if (!Impl->IsMutationAllowed(TargetObject, TargetResult))
		{
			return TargetResult;
		}
		TargetWidget = Cast<UWidget>(TargetObject);
		if (!TargetWidget)
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Pointer target must resolve to a UWidget."),
				422);
		}
	}

	TOptional<FVector2D> Position;
	TOptional<FVector2D> EndPosition;
	ReadVector2D(Params, TEXT("position"), Position);
	ReadVector2D(Params, TEXT("endPosition"), EndPosition);
	return Impl->Input.Pointer(
		Action,
		TargetWidget,
		Position,
		EndPosition,
		Button);
}

FRuntimeServiceResult FRuntimeSceneService::KeyInput(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before input can be dispatched."),
			409);
	}

	FString Action;
	FString KeyName;
	FString Text;
	Params->TryGetStringField(TEXT("action"), Action);
	Params->TryGetStringField(TEXT("key"), KeyName);
	Params->TryGetStringField(TEXT("text"), Text);
	if (Action.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'action'."));
	}

	TArray<FString> KeyNames;
	const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
	if (Params->TryGetArrayField(TEXT("keys"), Keys))
	{
		for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
		{
			KeyNames.Add(KeyValue->AsString());
		}
	}

	UWidget* TargetWidget = nullptr;
	const TSharedPtr<FJsonObject>* TargetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("target"), TargetRef) &&
		TargetRef &&
		TargetRef->IsValid())
	{
		UObject* TargetObject = nullptr;
		FRuntimeServiceResult TargetResult = Impl->Resolve(*TargetRef, TargetObject);
		if (!TargetResult.bSuccess)
		{
			return TargetResult;
		}
		if (!Impl->IsMutationAllowed(TargetObject, TargetResult))
		{
			return TargetResult;
		}
		TargetWidget = Cast<UWidget>(TargetObject);
		if (!TargetWidget)
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Key target must resolve to a UWidget."),
				422);
		}
	}

	return Impl->Input.Key(Action, KeyName, Text, KeyNames, TargetWidget);
}

FRuntimeServiceResult FRuntimeSceneService::SetInputMode(
	const TSharedPtr<FJsonObject>& Params)
{
	if (!Impl->bSessionActive)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("PIE must be running before input mode can be changed."),
			409);
	}

	FString Mode;
	if (!Params->TryGetStringField(TEXT("mode"), Mode) || Mode.IsEmpty())
	{
		return InvalidParams(TEXT("Missing 'mode'."));
	}
	FString LockMouse = TEXT("doNotLock");
	Params->TryGetStringField(TEXT("lockMouse"), LockMouse);

	UWidget* FocusWidget = nullptr;
	UWorld* RuntimeWorld = nullptr;
	const TSharedPtr<FJsonObject>* WidgetRef = nullptr;
	if (Params->TryGetObjectField(TEXT("widget"), WidgetRef) &&
		WidgetRef &&
		WidgetRef->IsValid())
	{
		UObject* WidgetObject = nullptr;
		FRuntimeServiceResult WidgetResult = Impl->Resolve(*WidgetRef, WidgetObject);
		if (!WidgetResult.bSuccess)
		{
			return WidgetResult;
		}
		FocusWidget = Cast<UWidget>(WidgetObject);
		if (!FocusWidget)
		{
			return FRuntimeServiceResult::Error(
				TEXT("widget_not_interactable"),
				TEXT("Input mode widget must resolve to a UWidget."),
				422);
		}
		RuntimeWorld = GetObjectWorld(FocusWidget);
	}
	if (!RuntimeWorld && GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (IsMutableWorld(Context.World()))
			{
				RuntimeWorld = Context.World();
				break;
			}
		}
	}
	if (!RuntimeWorld)
	{
		return FRuntimeServiceResult::Error(
			TEXT("pie_not_running"),
			TEXT("No PIE/Game world is available."),
			409);
	}

	double PlayerIndexNumber = 0.0;
	Params->TryGetNumberField(TEXT("playerIndex"), PlayerIndexNumber);
	APlayerController* PlayerController = UGameplayStatics::GetPlayerController(
		RuntimeWorld,
		FMath::Max(0, static_cast<int32>(PlayerIndexNumber)));
	TOptional<bool> ShowCursor;
	bool bShowCursor = false;
	if (Params->TryGetBoolField(TEXT("showCursor"), bShowCursor))
	{
		ShowCursor = bShowCursor;
	}
	return Impl->Input.SetPlayerInputMode(
		PlayerController,
		Mode,
		FocusWidget,
		LockMouse,
		ShowCursor);
}
}
