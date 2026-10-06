#include "Infrastructure/ReflectionInspectService.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Infrastructure/Sha256.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/FieldIterator.h"
#include "UObject/EnumProperty.h"
#include "UObject/UObjectIterator.h"

namespace UEAIIntegration::Infrastructure
{
FString EnginePythonExecutableRelativePath(const FString& Platform)
{
	if (Platform == TEXT("Win64"))
	{
		return TEXT("Binaries/ThirdParty/Python3/Win64/python.exe");
	}
	if (Platform == TEXT("Linux") || Platform == TEXT("Mac"))
	{
		return FString::Printf(TEXT("Binaries/ThirdParty/Python3/%s/bin/python3"), *Platform);
	}
	return FString();
}

namespace
{
FString RefString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

UStruct* FindReflectedStruct(const FString& Name)
{
	for (TObjectIterator<UStruct> It; It; ++It)
	{
		if (It->GetName() == Name || It->GetPathName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

UEnum* FindReflectedEnum(const FString& Name)
{
	for (TObjectIterator<UEnum> It; It; ++It)
	{
		if (It->GetName() == Name || It->GetPathName() == Name)
		{
			return *It;
		}
	}
	return nullptr;
}

template <typename TField>
TSharedPtr<FJsonObject> MetadataJson(const TField* Field)
{
	TSharedRef<FJsonObject> Metadata = MakeShared<FJsonObject>();
	for (const TCHAR* Key : {
		TEXT("Category"), TEXT("DisplayName"), TEXT("ToolTip"),
		TEXT("DeprecatedFunction"), TEXT("DeprecationMessage"),
		TEXT("BlueprintInternalUseOnly"), TEXT("AdvancedDisplay"),
		TEXT("ClampMin"), TEXT("ClampMax"), TEXT("Units")})
	{
		if (Field && Field->HasMetaData(Key))
		{
			Metadata->SetStringField(Key, Field->GetMetaData(Key).Left(4096));
		}
	}
	return Metadata;
}

FString ModuleName(const UObject* Object)
{
	FString Module = Object && Object->GetOutermost()
		? Object->GetOutermost()->GetName()
		: FString();
	Module.RemoveFromStart(TEXT("/Script/"));
	return Module;
}

void AddOriginAndRecommendations(
	const UObject* Object,
	const FString& Name,
	const TSharedRef<FJsonObject>& Result)
{
	const FString Module = ModuleName(Object);
	Result->SetStringField(TEXT("module"), Module);
	Result->SetStringField(
		TEXT("availableInEngineVersion"),
		FEngineVersion::Current().ToString());
	if (!Module.IsEmpty() && IPluginManager::Get().FindPlugin(Module).IsValid())
	{
		Result->SetStringField(TEXT("plugin"), Module);
	}
	TArray<TSharedPtr<FJsonValue>> Recommendations;
	auto Recommend = [&Recommendations](const TCHAR* Capability)
	{
		Recommendations.Add(MakeShared<FJsonValueString>(Capability));
	};
	if (Name.Contains(TEXT("Blueprint"), ESearchCase::IgnoreCase))
	{
		Recommend(TEXT("blueprint.asset.get"));
		Recommend(TEXT("blueprint.graph.get"));
	}
	else if (Name.Contains(TEXT("Texture"), ESearchCase::IgnoreCase))
	{
		Recommend(TEXT("content.texture.get"));
	}
	else if (Name.Contains(TEXT("StaticMesh"), ESearchCase::IgnoreCase))
	{
		Recommend(TEXT("content.static_mesh.get"));
	}
	Result->SetArrayField(TEXT("recommendedCapabilities"), Recommendations);
}

TSharedPtr<FJsonObject> PropertyJson(FProperty* Property)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Property->GetName());
	Result->SetStringField(TEXT("type"), Property->GetCPPType());
	Result->SetNumberField(TEXT("flags"), static_cast<double>(Property->GetPropertyFlags()));
	Result->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit));
	Result->SetBoolField(TEXT("blueprintVisible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
	Result->SetObjectField(TEXT("metadata"), MetadataJson(Property));
	return Result;
}

TSharedPtr<FJsonObject> FunctionJson(UFunction* Function)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Function->GetName());
	Result->SetStringField(
		TEXT("declaringType"),
		Function->GetOuterUClass()
			? Function->GetOuterUClass()->GetPathName()
			: FString());
	Result->SetNumberField(TEXT("flags"), static_cast<double>(Function->FunctionFlags));
	Result->SetObjectField(TEXT("metadata"), MetadataJson(Function));
	TArray<TSharedPtr<FJsonValue>> Parameters;
	for (TFieldIterator<FProperty> It(Function); It; ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_Parm))
		{
			TSharedPtr<FJsonObject> Parameter = PropertyJson(*It);
			Parameter->SetBoolField(TEXT("return"), It->HasAnyPropertyFlags(CPF_ReturnParm));
			const FString DefaultKey = TEXT("CPP_Default_") + It->GetName();
			if (Function->HasMetaData(*DefaultKey))
			{
				Parameter->SetStringField(
					TEXT("defaultValue"),
					Function->GetMetaData(*DefaultKey).Left(4096));
			}
			Parameters.Add(MakeShared<FJsonValueObject>(Parameter));
		}
	}
	Result->SetArrayField(TEXT("parameters"), Parameters);
	return Result;
}

TSharedPtr<FJsonObject> StructJson(UStruct* Struct, const bool bMembers)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Struct->GetName());
	Result->SetStringField(TEXT("path"), Struct->GetPathName());
	Result->SetStringField(TEXT("kind"), Struct->IsA<UClass>() ? TEXT("class") : TEXT("struct"));
	AddOriginAndRecommendations(Struct, Struct->GetName(), Result);
	Result->SetObjectField(TEXT("metadata"), MetadataJson(Struct));
	if (UStruct* Super = Struct->GetSuperStruct())
	{
		Result->SetStringField(TEXT("super"), Super->GetPathName());
	}
	if (bMembers)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Properties.Add(MakeShared<FJsonValueObject>(PropertyJson(*It)));
			if (Properties.Num() >= 512) break;
		}
		TArray<TSharedPtr<FJsonValue>> Functions;
		for (TFieldIterator<UFunction> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			Functions.Add(MakeShared<FJsonValueObject>(FunctionJson(*It)));
			if (Functions.Num() >= 512) break;
		}
		Result->SetArrayField(TEXT("properties"), Properties);
		Result->SetArrayField(TEXT("functions"), Functions);
	}
	return Result;
}

TSharedPtr<FJsonObject> EnumJson(UEnum* Enum, const bool bMembers)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Enum->GetName());
	Result->SetStringField(TEXT("path"), Enum->GetPathName());
	Result->SetStringField(TEXT("kind"), TEXT("enum"));
	AddOriginAndRecommendations(Enum, Enum->GetName(), Result);
	Result->SetObjectField(TEXT("metadata"), MetadataJson(Enum));
	if (bMembers)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 Count = FMath::Min(Enum->NumEnums(), 1024);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(Index));
			Value->SetNumberField(
				TEXT("value"),
				static_cast<double>(Enum->GetValueByIndex(Index)));
			Values.Add(MakeShared<FJsonValueObject>(Value));
		}
		Result->SetArrayField(TEXT("values"), Values);
	}
	return Result;
}

FString JsonText(const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
	return Text;
}

TSharedPtr<FJsonObject> ParseJson(const FString& Text)
{
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	return FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid() ? Object : nullptr;
}

bool SaveJsonFileAtomic(const FString& Path, const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid()) return false;
	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	if (!FJsonSerializer::Serialize(Object.ToSharedRef(), Writer)) return false;
	const FString Temporary = Path + TEXT(".tmp-") + FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		&& IFileManager::Get().Move(*Path, *Temporary, true, true, false, true))
	{
		return true;
	}
	IFileManager::Get().Delete(*Temporary, false, true);
	return false;
}
}

bool FReflectionInspectService::Handles(const FString& CapabilityId) const
{
	return CapabilityId.StartsWith(TEXT("production.reflection."))
		|| CapabilityId == TEXT("production.python.inspect");
}

FMCPToolResult FReflectionInspectService::Execute(
	const FString& CapabilityId,
	const TSharedPtr<FJsonObject>& Params) const
{
	if (CapabilityId == TEXT("production.reflection.type.search")) return SearchTypes(Params);
	if (CapabilityId == TEXT("production.reflection.type.get")) return GetType(Params);
	if (CapabilityId == TEXT("production.reflection.member.get")) return GetMember(Params);
	if (CapabilityId == TEXT("production.reflection.object.describe")) return DescribeObject(Params);
	if (CapabilityId == TEXT("production.reflection.snapshot.create")) return CreateSnapshot(Params);
	if (CapabilityId == TEXT("production.python.inspect")) return InspectPython(Params);
	return FMCPToolResult::Error(TEXT("Reflection capability was not found."), TEXT("capability_not_found"), 404);
}

FMCPToolResult FReflectionInspectService::SearchTypes(const TSharedPtr<FJsonObject>& Params) const
{
	const FString Query = RefString(Params, TEXT("query"));
	const FString Kind = RefString(Params, TEXT("kind"));
	double OffsetValue = 0.0;
	double LimitValue = 25.0;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("offset"), OffsetValue);
		Params->TryGetNumberField(TEXT("limit"), LimitValue);
	}
	const int32 Offset = FMath::Max(0, static_cast<int32>(OffsetValue));
	const int32 Limit = FMath::Clamp(static_cast<int32>(LimitValue), 1, 100);
	TArray<TSharedPtr<FJsonObject>> Matches;
	for (TObjectIterator<UStruct> It; It; ++It)
	{
		const FString StructKind = It->IsA<UClass>() ? TEXT("class") : TEXT("struct");
		if (!Kind.IsEmpty() && Kind != StructKind)
		{
			continue;
		}
		if (Query.IsEmpty() || It->GetName().Contains(Query, ESearchCase::IgnoreCase)
			|| It->GetPathName().Contains(Query, ESearchCase::IgnoreCase))
		{
			Matches.Add(StructJson(*It, false));
		}
	}
	if (Kind.IsEmpty() || Kind == TEXT("enum"))
	{
		for (TObjectIterator<UEnum> It; It; ++It)
		{
			if (Query.IsEmpty()
				|| It->GetName().Contains(Query, ESearchCase::IgnoreCase)
				|| It->GetPathName().Contains(Query, ESearchCase::IgnoreCase))
			{
				Matches.Add(EnumJson(*It, false));
			}
		}
	}
	Matches.Sort([](const TSharedPtr<FJsonObject>& Left, const TSharedPtr<FJsonObject>& Right)
	{
		return RefString(Left, TEXT("path")) < RefString(Right, TEXT("path"));
	});
	TArray<TSharedPtr<FJsonValue>> Types;
	for (int32 Index = Offset;
		Index < Matches.Num() && Types.Num() < Limit;
		++Index)
	{
		Types.Add(MakeShared<FJsonValueObject>(Matches[Index]));
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("types"), Types);
	Data->SetNumberField(TEXT("total"), Matches.Num());
	Data->SetNumberField(TEXT("offset"), Offset);
	Data->SetNumberField(TEXT("limit"), Limit);
	Data->SetBoolField(TEXT("hasMore"), Offset + Types.Num() < Matches.Num());
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FReflectionInspectService::GetType(const TSharedPtr<FJsonObject>& Params) const
{
	const FString TypeName = RefString(Params, TEXT("type"));
	if (UStruct* Struct = FindReflectedStruct(TypeName))
	{
		return FMCPToolResult::Ok(StructJson(Struct, true));
	}
	if (UEnum* Enum = FindReflectedEnum(TypeName))
	{
		return FMCPToolResult::Ok(EnumJson(Enum, true));
	}
	return FMCPToolResult::Error(
		TEXT("Reflected type was not found."),
		TEXT("asset_not_found"),
		404);
}

FMCPToolResult FReflectionInspectService::GetMember(const TSharedPtr<FJsonObject>& Params) const
{
	UStruct* Struct = FindReflectedStruct(RefString(Params, TEXT("type")));
	const FString Member = RefString(Params, TEXT("member"));
	if (!Struct) return FMCPToolResult::Error(TEXT("Reflected type was not found."), TEXT("asset_not_found"), 404);
	if (FProperty* Property = FindFProperty<FProperty>(Struct, *Member)) return FMCPToolResult::Ok(PropertyJson(Property));
	if (UClass* Class = Cast<UClass>(Struct))
	{
		for (UClass* SearchClass = Class;
			SearchClass;
			SearchClass = SearchClass->GetSuperClass())
		{
			if (UFunction* Function = SearchClass->FindFunctionByName(
					*Member,
					EIncludeSuperFlag::ExcludeSuper))
			{
				TSharedPtr<FJsonObject> Result = FunctionJson(Function);
				Result->SetBoolField(TEXT("inherited"), SearchClass != Class);
				return FMCPToolResult::Ok(Result);
			}
		}
	}
	return FMCPToolResult::Error(TEXT("Reflected member was not found."), TEXT("asset_not_found"), 404);
}

FMCPToolResult FReflectionInspectService::DescribeObject(const TSharedPtr<FJsonObject>& Params) const
{
	UObject* Object = StaticLoadObject(UObject::StaticClass(), nullptr, *RefString(Params, TEXT("objectPath")), nullptr, LOAD_NoWarn);
	if (!Object) return FMCPToolResult::Error(TEXT("Object was not found."), TEXT("asset_not_found"), 404);
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), Object->GetName());
	Data->SetStringField(TEXT("path"), Object->GetPathName());
	Data->SetObjectField(TEXT("type"), StructJson(Object->GetClass(), false));
	TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
	int32 Count = 0;
	for (TFieldIterator<FProperty> It(Object->GetClass()); It && Count < 128; ++It, ++Count)
	{
		FString Exported;
		It->ExportText_InContainer(0, Exported, Object, Object, Object, PPF_None);
		Values->SetStringField(It->GetName(), Exported.Left(4096));
	}
	Data->SetObjectField(TEXT("values"), Values);
	return FMCPToolResult::Ok(Data);
}

FMCPToolResult FReflectionInspectService::CreateSnapshot(const TSharedPtr<FJsonObject>& Params) const
{
	const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
	if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("types"), Types) || !Types || Types->Num() > 64)
	{
		return FMCPToolResult::Error(TEXT("types must be an array with at most 64 entries."), TEXT("invalid_params"), 422);
	}
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema"), TEXT("ue.reflection-snapshot.v1"));
	Snapshot->SetStringField(TEXT("createdAtUtc"), FDateTime::UtcNow().ToIso8601());
	Snapshot->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const TSharedPtr<FJsonValue>& Value : *Types)
	{
		const FString TypeName = Value.IsValid() && Value->Type == EJson::String
			? Value->AsString()
			: FString();
		if (UStruct* Struct = FindReflectedStruct(TypeName))
		{
			Values.Add(MakeShared<FJsonValueObject>(StructJson(Struct, true)));
		}
		else if (UEnum* Enum = FindReflectedEnum(TypeName))
		{
			Values.Add(MakeShared<FJsonValueObject>(EnumJson(Enum, true)));
		}
	}
	Snapshot->SetArrayField(TEXT("types"), Values);
	const FString Text = JsonText(Snapshot);
	FTCHARToUTF8 Utf8(*Text);
	FString Digest;
	if (!TrySha256Hex(Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest))
	{
		return FMCPToolResult::Error(TEXT("Reflection snapshot digest failed."), TEXT("recovery_snapshot_failed"), 500);
	}
	const FString Id = Digest.Left(32);
	const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE_AI_integration/Reflection"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = FPaths::Combine(Directory, Id + TEXT(".json"));
	const FString Temporary = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(
			Text,
			*Temporary,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*Path, *Temporary, true, true, false, true))
	{
		return FMCPToolResult::Error(TEXT("Reflection snapshot could not be written."), TEXT("recovery_storage_unavailable"), 507);
	}
	Snapshot->SetStringField(TEXT("snapshotId"), Id);
	Snapshot->SetStringField(TEXT("path"), Path);
	return FMCPToolResult::Ok(Snapshot);
}

FMCPToolResult FReflectionInspectService::InspectPython(const TSharedPtr<FJsonObject>& Params) const
{
	const FString Script = RefString(Params, TEXT("script"));
	const FString ModificationLevel = RefString(Params, TEXT("modificationLevel"));
	bool bConfirmWrite = false;
	if (Params.IsValid() && Params->HasField(TEXT("confirmWrite"))
		&& !Params->TryGetBoolField(TEXT("confirmWrite"), bConfirmWrite))
	{
		return FMCPToolResult::Error(TEXT("confirmWrite must be boolean."), TEXT("invalid_params"), 422);
	}
	if (Script.IsEmpty())
	{
		return FMCPToolResult::Error(TEXT("script is required."), TEXT("invalid_params"), 422);
	}
	if (Script.Len() > 256 * 1024)
	{
		return FMCPToolResult::Error(TEXT("Python script exceeds the 256 KiB limit."), TEXT("python_script_rejected"), 413);
	}
	if (ModificationLevel != TEXT("readOnly")
		&& ModificationLevel != TEXT("safeWrite")
		&& ModificationLevel != TEXT("confirmWrite")
		&& ModificationLevel != TEXT("destructive"))
	{
		return FMCPToolResult::Error(
			TEXT("modificationLevel must be readOnly, safeWrite, confirmWrite, or destructive."),
			TEXT("invalid_modification_level"),
			422);
	}
	const TSharedPtr<FJsonObject>* Input = nullptr;
	if (Params.IsValid() && Params->HasField(TEXT("input")))
	{
		if (!Params->TryGetObjectField(TEXT("input"), Input) || !Input || !Input->IsValid())
		{
			return FMCPToolResult::Error(TEXT("input must be a JSON object."), TEXT("invalid_params"), 422);
		}
	}
	else if (Params.IsValid() && Params->HasField(TEXT("snapshot")))
	{
		if (!Params->TryGetObjectField(TEXT("snapshot"), Input) || !Input || !Input->IsValid())
		{
			return FMCPToolResult::Error(TEXT("snapshot must be a JSON object."), TEXT("invalid_params"), 422);
		}
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	const FString Worker = Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Python/full_execute.py")) : FString();
	const FString PythonRelativePath = EnginePythonExecutableRelativePath(FPlatformProcess::GetBinariesSubdirectory());
	const FString Python = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::EngineDir(), PythonRelativePath));
	if (PythonRelativePath.IsEmpty() || !IFileManager::Get().FileExists(*Python) || !IFileManager::Get().FileExists(*Worker))
	{
		return FMCPToolResult::Error(TEXT("The Engine Python worker is unavailable."), TEXT("job_runtime_unavailable"), 503);
	}
	FTCHARToUTF8 ScriptUtf8(*Script);
	FString ScriptDigest;
	if (!TrySha256Hex(ScriptUtf8.Get(), static_cast<uint64>(ScriptUtf8.Length()), ScriptDigest))
	{
		return FMCPToolResult::Error(TEXT("Python script digest failed."), TEXT("python_audit_failed"), 500);
	}
	const FString ExecutionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	const FString AuditDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UE_AI_integration/Python/audit"));
	const FString AuditPath = FPaths::Combine(AuditDirectory, ExecutionId + TEXT(".json"));
	const FString AuditRelativePath = FPaths::Combine(TEXT("Saved/UE_AI_integration/Python/audit"), ExecutionId + TEXT(".json"));
	TSharedPtr<FJsonObject> Audit = MakeShared<FJsonObject>();
	Audit->SetStringField(TEXT("schema"), TEXT("ue.python-execution-audit.v1"));
	Audit->SetStringField(TEXT("executionId"), ExecutionId);
	Audit->SetStringField(TEXT("scriptExecutionId"), ExecutionId);
	Audit->SetStringField(TEXT("modificationLevel"), ModificationLevel);
	Audit->SetStringField(TEXT("scriptModificationLevel"), ModificationLevel);
	Audit->SetStringField(TEXT("modificationLevelSource"), TEXT("callerDeclared"));
	Audit->SetBoolField(TEXT("confirmWriteProvided"), bConfirmWrite);
	Audit->SetStringField(TEXT("executionMode"), TEXT("fullScript"));
	Audit->SetBoolField(TEXT("auditRecorded"), true);
	Audit->SetStringField(TEXT("scriptDigest"), ScriptDigest);
	Audit->SetNumberField(TEXT("scriptBytes"), static_cast<double>(ScriptUtf8.Length()));
	Audit->SetStringField(TEXT("startedAtUtc"), FDateTime::UtcNow().ToIso8601());
	Audit->SetStringField(TEXT("status"), TEXT("running"));
	Audit->SetStringField(TEXT("worker"), TEXT("engine-python3"));
	IFileManager::Get().MakeDirectory(*AuditDirectory, true);
	if (!SaveJsonFileAtomic(AuditPath, Audit))
	{
		return FMCPToolResult::Error(TEXT("Python execution audit record could not be written."), TEXT("python_audit_failed"), 507);
	}
	const auto Complete = [&Audit, &ExecutionId, &AuditPath, &AuditRelativePath, &ModificationLevel, &ScriptDigest, bConfirmWrite](FMCPToolResult Result, const TCHAR* Status, const FString& ErrorCode)
	{
		Audit->SetStringField(TEXT("status"), Status);
		Audit->SetStringField(TEXT("completedAtUtc"), FDateTime::UtcNow().ToIso8601());
		if (!ErrorCode.IsEmpty()) Audit->SetStringField(TEXT("errorCode"), ErrorCode);
		if (!Result.ErrorMessage.IsEmpty()) Audit->SetStringField(TEXT("error"), Result.ErrorMessage);
		const bool bAuditCompleted = SaveJsonFileAtomic(AuditPath, Audit);
		TSharedPtr<FJsonObject> Receipt = Result.bSuccess && Result.Data.IsValid()
			? Result.Data : MakeShared<FJsonObject>();
		Receipt->SetStringField(TEXT("executionId"), ExecutionId);
		Receipt->SetStringField(TEXT("scriptExecutionId"), ExecutionId);
		// Keep machine-specific project roots out of the client-facing receipt. The
		// artifact remains under Project/Saved and can be resolved by the host.
		Receipt->SetStringField(TEXT("auditPath"), AuditRelativePath);
		Receipt->SetStringField(TEXT("modificationLevel"), ModificationLevel);
		Receipt->SetStringField(TEXT("scriptModificationLevel"), ModificationLevel);
		Receipt->SetStringField(TEXT("modificationLevelSource"), TEXT("callerDeclared"));
		Receipt->SetBoolField(TEXT("confirmWriteProvided"), bConfirmWrite);
		Receipt->SetStringField(TEXT("executionMode"), TEXT("fullScript"));
		Receipt->SetBoolField(TEXT("auditRecorded"), true);
		Receipt->SetBoolField(TEXT("auditCompleted"), bAuditCompleted);
		Receipt->SetStringField(TEXT("scriptDigest"), ScriptDigest);
		Receipt->SetStringField(TEXT("status"), Status);
		if (!bAuditCompleted)
		{
			Result = FMCPToolResult::Error(TEXT("Python execution finished, but its terminal audit record could not be written."), TEXT("python_audit_failed"), 507);
		}
		Result.Data = Receipt;
		return Result;
	};
	TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
	Request->SetStringField(TEXT("script"), Script);
	Request->SetStringField(TEXT("modificationLevel"), ModificationLevel);
	if (Input)
	{
		Request->SetObjectField(TEXT("data"), *Input);
	}
	else
	{
		Request->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
	}
	const FString RequestText = JsonText(Request);
	FTCHARToUTF8 RequestUtf8(*RequestText);
	if (RequestUtf8.Length() > 8 * 1024 * 1024)
	{
		return Complete(FMCPToolResult::Error(TEXT("Python request exceeds the eight MiB limit."), TEXT("python_script_rejected"), 413), TEXT("failed"), TEXT("python_script_rejected"));
	}
	void* StdoutRead = nullptr; void* StdoutWrite = nullptr;
	void* StdinRead = nullptr; void* StdinWrite = nullptr;
	FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite);
	FPlatformProcess::CreatePipe(StdinRead, StdinWrite, true);
	const FString Args = FString::Printf(TEXT("-I -E -u \"%s\""), *Worker);
	FProcHandle Process = FPlatformProcess::CreateProc(*Python, *Args, false, true, true, nullptr, 0, nullptr, StdoutWrite, StdinRead);
	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
		FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
		return Complete(FMCPToolResult::Error(TEXT("Python worker could not be started."), TEXT("job_runtime_unavailable"), 503), TEXT("failed"), TEXT("job_runtime_unavailable"));
	}
	FPlatformProcess::WritePipe(StdinWrite, RequestText);
	FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
	const double Deadline = FPlatformTime::Seconds() + 30.0;
	FString Output;
	bool bOutputLimitExceeded = false;
	while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
	{
		Output += FPlatformProcess::ReadPipe(StdoutRead);
		if (FTCHARToUTF8(*Output).Length() > 1024 * 1024)
		{
			bOutputLimitExceeded = true;
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	if (bOutputLimitExceeded)
	{
		FPlatformProcess::TerminateProc(Process, true);
		FPlatformProcess::CloseProc(Process);
		FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
		return Complete(FMCPToolResult::Error(
			TEXT("Python worker output exceeded the one MiB limit."),
			TEXT("python_output_limit"),
			413), TEXT("failed"), TEXT("python_output_limit"));
	}
	if (FPlatformProcess::IsProcRunning(Process))
	{
		FPlatformProcess::TerminateProc(Process, true);
		FPlatformProcess::CloseProc(Process);
		FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
		return Complete(FMCPToolResult::Error(TEXT("Python worker exceeded the thirty second limit."), TEXT("python_worker_timeout"), 408), TEXT("timedOut"), TEXT("python_worker_timeout"));
	}
	Output += FPlatformProcess::ReadPipe(StdoutRead);
	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(Process, &ReturnCode);
	FPlatformProcess::CloseProc(Process);
	FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
	Audit->SetNumberField(TEXT("exitCode"), static_cast<double>(ReturnCode));
	const TSharedPtr<FJsonObject> Result =
		FTCHARToUTF8(*Output).Length() <= 1024 * 1024
			? ParseJson(Output)
			: nullptr;
	if (!Result.IsValid())
	{
		return Complete(FMCPToolResult::Error(TEXT("Python script returned invalid JSON."), TEXT("python_script_failed"), 422), TEXT("failed"), TEXT("python_script_failed"));
	}
	bool bOk = false;
	Result->TryGetBoolField(TEXT("ok"), bOk);
	for (const TCHAR* Field : { TEXT("stdout"), TEXT("stderr"), TEXT("exceptionType"), TEXT("traceback") })
	{
		if (Result->HasTypedField<EJson::String>(Field)) Audit->SetStringField(Field, RefString(Result, Field));
	}
	if (bOk && ReturnCode == 0)
	{
		return Complete(FMCPToolResult::Ok(Result), TEXT("completed"), FString());
	}
	return Complete(FMCPToolResult::Error(RefString(Result, TEXT("error")), TEXT("python_script_failed"), 422), TEXT("failed"), TEXT("python_script_failed"));
}
}
