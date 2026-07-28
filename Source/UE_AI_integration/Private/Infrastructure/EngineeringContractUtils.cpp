#include "Infrastructure/EngineeringContractUtils.h"

#include "Infrastructure/Sha256.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UEAIIntegration::Infrastructure
{
namespace
{
FString QuoteJsonString(const FString& Value)
{
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(MakeShared<FJsonValueString>(Value), TEXT(""), Writer);
	Writer->Close();
	return Serialized;
}

FString CanonicalizeObject(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid())
	{
		return TEXT("null");
	}

	TArray<FString> Keys;
	Object->Values.GetKeys(Keys);
	Keys.Sort();

	FString Result = TEXT("{");
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		if (Index > 0)
		{
			Result += TEXT(",");
		}
		Result += QuoteJsonString(Keys[Index]);
		Result += TEXT(":");
		Result += CanonicalizeJsonValue(Object->Values.FindRef(Keys[Index]));
	}
	Result += TEXT("}");
	return Result;
}
}

FString CanonicalizeJsonValue(const TSharedPtr<FJsonValue>& Value)
{
	if (!Value.IsValid())
	{
		return TEXT("null");
	}

	switch (Value->Type)
	{
	case EJson::Null:
		return TEXT("null");
	case EJson::String:
		return QuoteJsonString(Value->AsString());
	case EJson::Number:
		return FString::Printf(TEXT("%.17g"), Value->AsNumber());
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Array:
	{
		FString Result = TEXT("[");
		const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			if (Index > 0)
			{
				Result += TEXT(",");
			}
			Result += CanonicalizeJsonValue(Values[Index]);
		}
		Result += TEXT("]");
		return Result;
	}
	case EJson::Object:
		return CanonicalizeObject(Value->AsObject());
	default:
		return TEXT("null");
	}
}

FString DigestJson(const TSharedPtr<FJsonObject>& Object)
{
	const FString Canonical =
		CanonicalizeJsonValue(MakeShared<FJsonValueObject>(Object));
	FTCHARToUTF8 Utf8(*Canonical);
	FString Digest;
	if (!TrySha256Hex(Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest))
	{
		return FString();
	}
	return Digest;
}

FString MakeStableId(const FString& Prefix, const TArray<FString>& Components)
{
	TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
	Identity->SetStringField(TEXT("namespace"), Prefix);
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(Components.Num());
	for (const FString& Component : Components)
	{
		Values.Add(MakeShared<FJsonValueString>(Component));
	}
	Identity->SetArrayField(TEXT("components"), Values);
	const FString Digest = DigestJson(Identity);
	return Digest.IsEmpty()
		? FString()
		: FString::Printf(TEXT("%s-%s"), *Prefix, *Digest.Left(24));
}

TSharedRef<FJsonObject> MakeFinding(
	const FString& RuleId,
	const FString& Severity,
	const double Confidence,
	const FString& AssetPath,
	const FString& GraphName,
	const FString& NodeGuid,
	const FString& Message,
	const TSharedPtr<FJsonObject>& Evidence)
{
	TSharedRef<FJsonObject> Finding = MakeShared<FJsonObject>();
	Finding->SetStringField(TEXT("schema"), TEXT("ue.finding.v1"));
	Finding->SetStringField(
		TEXT("findingId"),
		MakeStableId(
			TEXT("finding"),
			{RuleId, AssetPath, GraphName, NodeGuid, Message}));
	Finding->SetStringField(TEXT("ruleId"), RuleId);
	FString NormalizedSeverity = Severity.ToLower();
	if (NormalizedSeverity == TEXT("error"))
	{
		NormalizedSeverity = TEXT("high");
	}
	else if (NormalizedSeverity == TEXT("warning"))
	{
		NormalizedSeverity = TEXT("medium");
	}
	static const TSet<FString> SupportedSeverities = {
		TEXT("info"),
		TEXT("low"),
		TEXT("medium"),
		TEXT("high"),
		TEXT("critical"),
	};
	if (!SupportedSeverities.Contains(NormalizedSeverity))
	{
		NormalizedSeverity = TEXT("info");
	}
	Finding->SetStringField(TEXT("severity"), NormalizedSeverity);
	Finding->SetNumberField(TEXT("confidence"), FMath::Clamp(Confidence, 0.0, 1.0));
	Finding->SetStringField(TEXT("message"), Message);

	TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
	Location->SetStringField(TEXT("assetPath"), AssetPath);
	if (!GraphName.IsEmpty())
	{
		Location->SetStringField(TEXT("graph"), GraphName);
	}
	if (!NodeGuid.IsEmpty())
	{
		Location->SetStringField(TEXT("nodeGuid"), NodeGuid);
	}
	Finding->SetObjectField(TEXT("location"), Location);
	Finding->SetObjectField(
		TEXT("evidence"),
		Evidence.IsValid() ? Evidence.ToSharedRef() : MakeShared<FJsonObject>());
	Finding->SetStringField(TEXT("runtimeStatus"), TEXT("hypothesis"));
	return Finding;
}

void SetBoundedArray(
	const TSharedRef<FJsonObject>& Target,
	const FString& Field,
	const TArray<TSharedPtr<FJsonValue>>& Values,
	const int32 Total,
	const int32 Limit)
{
	const int32 EffectiveLimit = FMath::Max(0, Limit);
	const int32 Count = FMath::Min(Values.Num(), EffectiveLimit);
	TArray<TSharedPtr<FJsonValue>> Bounded;
	Bounded.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Bounded.Add(Values[Index]);
	}
	Target->SetArrayField(Field, Bounded);
	Target->SetNumberField(Field + TEXT("Count"), Count);
	Target->SetNumberField(Field + TEXT("Total"), FMath::Max(Total, Count));
	Target->SetBoolField(Field + TEXT("Truncated"), FMath::Max(Total, Values.Num()) > Count);
}
}
