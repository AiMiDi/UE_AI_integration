#include "Core/MCPCapabilityCatalog.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UEAIIntegration
{
namespace Core
{
namespace
{
struct FManifestSpec
{
	const TCHAR* FileName;
	const TCHAR* Domain;
};

const FManifestSpec ManifestSpecs[] = {
	{TEXT("blueprint.json"), TEXT("blueprint")},
	{TEXT("scene.json"), TEXT("scene")},
	{TEXT("content.json"), TEXT("content")},
	{TEXT("animation.json"), TEXT("animation")},
	{TEXT("ai.json"), TEXT("ai")},
	{TEXT("production.json"), TEXT("production")},
};

void AddError(TArray<FString>& Errors, const FString& Error)
{
	Errors.AddUnique(Error);
}

bool HasRequiredString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Context,
	FString& OutValue,
	TArray<FString>& Errors)
{
	if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue) || OutValue.IsEmpty())
	{
		AddError(Errors, FString::Printf(TEXT("%s must contain non-empty string '%s'."), *Context, Field));
		return false;
	}
	return true;
}

bool HasRequiredObject(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Context,
	TSharedPtr<FJsonObject>& OutValue,
	TArray<FString>& Errors)
{
	if (!Object.IsValid() || !Object->HasTypedField<EJson::Object>(Field))
	{
		AddError(Errors, FString::Printf(TEXT("%s must contain object '%s'."), *Context, Field));
		return false;
	}
	OutValue = Object->GetObjectField(Field);
	return OutValue.IsValid();
}

bool HasRequiredBool(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Context,
	TArray<FString>& Errors)
{
	if (!Object.IsValid() || !Object->HasTypedField<EJson::Boolean>(Field))
	{
		AddError(Errors, FString::Printf(TEXT("%s must contain boolean '%s'."), *Context, Field));
		return false;
	}
	return true;
}

bool ValidateOptionalStringArray(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field,
	const FString& Context,
	TArray<FString>& Errors)
{
	if (!Object.IsValid() || !Object->HasField(Field))
	{
		return true;
	}
	if (!Object->HasTypedField<EJson::Array>(Field))
	{
		AddError(
			Errors,
			FString::Printf(TEXT("%s.%s must be an array."), *Context, Field));
		return false;
	}

	bool bValid = true;
	for (const TSharedPtr<FJsonValue>& Value : Object->GetArrayField(Field))
	{
		if (!Value.IsValid() || Value->Type != EJson::String || Value->AsString().IsEmpty())
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s.%s must contain only non-empty strings."),
					*Context,
					Field));
			bValid = false;
			break;
		}
	}
	return bValid;
}

bool ValidateSearchMetadata(
	const TSharedPtr<FJsonObject>& Descriptor,
	const FString& Context,
	TArray<FString>& Errors)
{
	if (!Descriptor.IsValid() || !Descriptor->HasField(TEXT("search")))
	{
		return true;
	}
	if (!Descriptor->HasTypedField<EJson::Object>(TEXT("search")))
	{
		AddError(Errors, Context + TEXT(".search must be an object."));
		return false;
	}
	const TSharedPtr<FJsonObject> Search =
		Descriptor->GetObjectField(TEXT("search"));
	static const TSet<FString> AllowedFields = {
		TEXT("title"),
		TEXT("keywords"),
		TEXT("aliases"),
	};
	bool bValid = true;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
		Search->Values)
	{
		if (!AllowedFields.Contains(Field.Key))
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s.search.%s is not supported."),
					*Context,
					*Field.Key));
			bValid = false;
		}
	}
	if (Search->HasField(TEXT("title")))
	{
		FString Title;
		if (!Search->TryGetStringField(TEXT("title"), Title)
			|| Title.TrimStartAndEnd().IsEmpty())
		{
			AddError(
				Errors,
				Context
					+ TEXT(".search.title must be a non-empty string."));
			bValid = false;
		}
	}
	for (const TCHAR* FieldName : {TEXT("keywords"), TEXT("aliases")})
	{
		if (!Search->HasField(FieldName))
		{
			continue;
		}
		if (!Search->HasTypedField<EJson::Array>(FieldName)
			|| Search->GetArrayField(FieldName).IsEmpty())
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s.search.%s must be a non-empty array."),
					*Context,
					FieldName));
			bValid = false;
			continue;
		}
		TSet<FString> Unique;
		for (const TSharedPtr<FJsonValue>& Value :
			Search->GetArrayField(FieldName))
		{
			if (!Value.IsValid()
				|| Value->Type != EJson::String
				|| Value->AsString().TrimStartAndEnd().IsEmpty())
			{
				AddError(
					Errors,
					FString::Printf(
						TEXT("%s.search.%s must contain non-empty strings."),
						*Context,
						FieldName));
				bValid = false;
				continue;
			}
			const FString Normalized =
				Value->AsString().TrimStartAndEnd().ToLower();
			if (Unique.Contains(Normalized))
			{
				AddError(
					Errors,
					FString::Printf(
						TEXT("%s.search.%s must not contain duplicates."),
						*Context,
						FieldName));
				bValid = false;
			}
			Unique.Add(Normalized);
		}
	}
	if (!Search->HasField(TEXT("title"))
		&& !Search->HasField(TEXT("keywords"))
		&& !Search->HasField(TEXT("aliases")))
	{
		AddError(
			Errors,
			Context
				+ TEXT(".search must declare title, keywords, or aliases."));
		bValid = false;
	}
	return bValid;
}

bool JsonValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
{
	return Left.IsValid()
		&& Right.IsValid()
		&& FJsonValue::CompareEqual(*Left, *Right);
}

bool MatchesType(const TSharedPtr<FJsonValue>& Value, const FString& TypeName)
{
	if (!Value.IsValid())
	{
		return false;
	}
	if (TypeName == TEXT("string"))
	{
		return Value->Type == EJson::String;
	}
	if (TypeName == TEXT("number"))
	{
		return Value->Type == EJson::Number;
	}
	if (TypeName == TEXT("integer"))
	{
		return Value->Type == EJson::Number
			&& FMath::IsNearlyEqual(Value->AsNumber(), FMath::RoundToDouble(Value->AsNumber()));
	}
	if (TypeName == TEXT("boolean"))
	{
		return Value->Type == EJson::Boolean;
	}
	if (TypeName == TEXT("object"))
	{
		return Value->Type == EJson::Object;
	}
	if (TypeName == TEXT("array"))
	{
		return Value->Type == EJson::Array;
	}
	if (TypeName == TEXT("null"))
	{
		return Value->Type == EJson::Null;
	}
	return true;
}

bool ValidateJsonValue(
	const TSharedPtr<FJsonValue>& Value,
	const TSharedPtr<FJsonObject>& Schema,
	const FString& Path,
	TArray<FString>& Errors);

bool ValidateObject(
	const TSharedPtr<FJsonObject>& Value,
	const TSharedPtr<FJsonObject>& Schema,
	const FString& Path,
	TArray<FString>& Errors)
{
	if (!Value.IsValid())
	{
		AddError(Errors, FString::Printf(TEXT("%s must be an object."), *Path));
		return false;
	}

	if (Schema->HasTypedField<EJson::Array>(TEXT("required")))
	{
		for (const TSharedPtr<FJsonValue>& RequiredValue : Schema->GetArrayField(TEXT("required")))
		{
			if (!RequiredValue.IsValid() || RequiredValue->Type != EJson::String)
			{
				continue;
			}
			const FString RequiredName = RequiredValue->AsString();
			if (!Value->HasField(RequiredName))
			{
				AddError(
					Errors,
					FString::Printf(TEXT("%s.%s is required."), *Path, *RequiredName));
			}
		}
	}

	TSharedPtr<FJsonObject> Properties;
	if (Schema->HasTypedField<EJson::Object>(TEXT("properties")))
	{
		Properties = Schema->GetObjectField(TEXT("properties"));
	}

	bool bAllowAdditionalProperties = true;
	if (Schema->HasTypedField<EJson::Boolean>(TEXT("additionalProperties")))
	{
		bAllowAdditionalProperties = Schema->GetBoolField(TEXT("additionalProperties"));
	}

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Value->Values)
	{
		TSharedPtr<FJsonValue> PropertySchemaValue =
			Properties.IsValid() ? Properties->Values.FindRef(Pair.Key) : nullptr;
		if (!PropertySchemaValue.IsValid())
		{
			if (!bAllowAdditionalProperties)
			{
				AddError(
					Errors,
					FString::Printf(TEXT("%s.%s is not an allowed parameter."), *Path, *Pair.Key));
			}
			continue;
		}
		if (PropertySchemaValue->Type == EJson::Object)
		{
			ValidateJsonValue(
				Pair.Value,
				PropertySchemaValue->AsObject(),
				FString::Printf(TEXT("%s.%s"), *Path, *Pair.Key),
				Errors);
		}
	}

	return Errors.IsEmpty();
}

bool ValidateJsonValue(
	const TSharedPtr<FJsonValue>& Value,
	const TSharedPtr<FJsonObject>& Schema,
	const FString& Path,
	TArray<FString>& Errors)
{
	if (!Schema.IsValid())
	{
		return true;
	}

	FString TypeName;
	if (Schema->TryGetStringField(TEXT("type"), TypeName) && !MatchesType(Value, TypeName))
	{
		AddError(
			Errors,
			FString::Printf(TEXT("%s must be of type %s."), *Path, *TypeName));
		return false;
	}

	if (Schema->HasTypedField<EJson::Array>(TEXT("enum")))
	{
		bool bMatched = false;
		for (const TSharedPtr<FJsonValue>& Allowed : Schema->GetArrayField(TEXT("enum")))
		{
			if (JsonValuesEqual(Value, Allowed))
			{
				bMatched = true;
				break;
			}
		}
		if (!bMatched)
		{
			AddError(Errors, FString::Printf(TEXT("%s is not one of the allowed values."), *Path));
		}
	}

	if (const TSharedPtr<FJsonValue>* Constant = Schema->Values.Find(TEXT("const")))
	{
		if (!JsonValuesEqual(Value, *Constant))
		{
			AddError(
				Errors,
				FString::Printf(TEXT("%s must equal the declared constant."), *Path));
		}
	}

	if (Value.IsValid() && Value->Type == EJson::String)
	{
		const FString StringValue = Value->AsString();
		double MinLength = 0.0;
		if (Schema->TryGetNumberField(TEXT("minLength"), MinLength)
			&& StringValue.Len() < static_cast<int32>(MinLength))
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s must contain at least %d characters."),
					*Path,
					static_cast<int32>(MinLength)));
		}
		double MaxLength = 0.0;
		if (Schema->TryGetNumberField(TEXT("maxLength"), MaxLength)
			&& StringValue.Len() > static_cast<int32>(MaxLength))
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s must contain at most %d characters."),
					*Path,
					static_cast<int32>(MaxLength)));
		}
		FString Pattern;
		if (Schema->TryGetStringField(TEXT("pattern"), Pattern))
		{
			FRegexMatcher Matcher(FRegexPattern(Pattern), StringValue);
			if (!Matcher.FindNext())
			{
				AddError(
					Errors,
					FString::Printf(TEXT("%s does not match the required pattern."), *Path));
			}
		}
	}

	if (Value.IsValid() && Value->Type == EJson::Number)
	{
		double Minimum = 0.0;
		if (Schema->TryGetNumberField(TEXT("minimum"), Minimum) && Value->AsNumber() < Minimum)
		{
			AddError(Errors, FString::Printf(TEXT("%s must be at least %g."), *Path, Minimum));
		}
		double Maximum = 0.0;
		if (Schema->TryGetNumberField(TEXT("maximum"), Maximum) && Value->AsNumber() > Maximum)
		{
			AddError(Errors, FString::Printf(TEXT("%s must be at most %g."), *Path, Maximum));
		}
	}

	if (Value.IsValid() && Value->Type == EJson::Object)
	{
		ValidateObject(Value->AsObject(), Schema, Path, Errors);
	}
	else if (Value.IsValid() && Value->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
		double MinItems = 0.0;
		if (Schema->TryGetNumberField(TEXT("minItems"), MinItems)
			&& Items.Num() < static_cast<int32>(MinItems))
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s must contain at least %d items."),
					*Path,
					static_cast<int32>(MinItems)));
		}
		double MaxItems = 0.0;
		if (Schema->TryGetNumberField(TEXT("maxItems"), MaxItems)
			&& Items.Num() > static_cast<int32>(MaxItems))
		{
			AddError(
				Errors,
				FString::Printf(
					TEXT("%s must contain at most %d items."),
					*Path,
					static_cast<int32>(MaxItems)));
		}
		if (Schema->HasTypedField<EJson::Object>(TEXT("items")))
		{
			const TSharedPtr<FJsonObject> ItemSchema =
				Schema->GetObjectField(TEXT("items"));
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				ValidateJsonValue(
					Items[Index],
					ItemSchema,
					FString::Printf(TEXT("%s[%d]"), *Path, Index),
					Errors);
			}
		}
	}

	if (Schema->HasTypedField<EJson::Array>(TEXT("allOf")))
	{
		for (const TSharedPtr<FJsonValue>& SubschemaValue :
			Schema->GetArrayField(TEXT("allOf")))
		{
			if (SubschemaValue.IsValid() && SubschemaValue->Type == EJson::Object)
			{
				ValidateJsonValue(Value, SubschemaValue->AsObject(), Path, Errors);
			}
		}
	}

	if (Schema->HasTypedField<EJson::Array>(TEXT("anyOf")))
	{
		bool bMatched = false;
		for (const TSharedPtr<FJsonValue>& SubschemaValue :
			Schema->GetArrayField(TEXT("anyOf")))
		{
			if (!SubschemaValue.IsValid() || SubschemaValue->Type != EJson::Object)
			{
				continue;
			}
			TArray<FString> CandidateErrors;
			if (ValidateJsonValue(
					Value,
					SubschemaValue->AsObject(),
					Path,
					CandidateErrors))
			{
				bMatched = true;
				break;
			}
		}
		if (!bMatched)
		{
			AddError(
				Errors,
				FString::Printf(TEXT("%s does not match any allowed shape."), *Path));
		}
	}

	if (Schema->HasTypedField<EJson::Object>(TEXT("if")))
	{
		TArray<FString> ConditionErrors;
		const bool bConditionMatched = ValidateJsonValue(
			Value,
			Schema->GetObjectField(TEXT("if")),
			Path,
			ConditionErrors);
		const TCHAR* Branch = bConditionMatched ? TEXT("then") : TEXT("else");
		if (Schema->HasTypedField<EJson::Object>(Branch))
		{
			ValidateJsonValue(
				Value,
				Schema->GetObjectField(Branch),
				Path,
				Errors);
		}
	}

	return Errors.IsEmpty();
}

bool ValidateDescriptor(
	const TSharedPtr<FJsonObject>& Descriptor,
	const FString& ExpectedDomain,
	const FString& Context,
	TArray<FString>& Errors)
{
	FString Id;
	FString Domain;
	FString Kind;
	FString Description;
	bool bValid = true;
	bValid &= HasRequiredString(Descriptor, TEXT("id"), Context, Id, Errors);
	bValid &= HasRequiredString(Descriptor, TEXT("domain"), Context, Domain, Errors);
	bValid &= HasRequiredString(Descriptor, TEXT("kind"), Context, Kind, Errors);
	bValid &= HasRequiredString(Descriptor, TEXT("description"), Context, Description, Errors);

	if (!Id.IsEmpty() && !IsDottedCapabilityId(Id))
	{
		AddError(Errors, FString::Printf(TEXT("%s has invalid dotted id '%s'."), *Context, *Id));
		bValid = false;
	}
	if (!Domain.IsEmpty() && Domain != ExpectedDomain)
	{
		AddError(
			Errors,
			FString::Printf(
				TEXT("%s domain '%s' does not match manifest domain '%s'."),
				*Context,
				*Domain,
				*ExpectedDomain));
		bValid = false;
	}
	if (!Id.IsEmpty() && !ExpectedDomain.IsEmpty() && !Id.StartsWith(ExpectedDomain + TEXT(".")))
	{
		AddError(
			Errors,
			FString::Printf(TEXT("%s id '%s' must begin with '%s.'."), *Context, *Id, *ExpectedDomain));
		bValid = false;
	}
	if (!Kind.IsEmpty()
		&& Kind != TEXT("query")
		&& Kind != TEXT("command")
		&& Kind != TEXT("validation"))
	{
		AddError(Errors, FString::Printf(TEXT("%s has unsupported kind '%s'."), *Context, *Kind));
		bValid = false;
	}

	TSharedPtr<FJsonObject> InputSchema;
	TSharedPtr<FJsonObject> Traits;
	TSharedPtr<FJsonObject> Output;
	bValid &= HasRequiredObject(Descriptor, TEXT("inputSchema"), Context, InputSchema, Errors);
	bValid &= HasRequiredObject(Descriptor, TEXT("traits"), Context, Traits, Errors);
	bValid &= HasRequiredObject(Descriptor, TEXT("output"), Context, Output, Errors);

	if (InputSchema.IsValid())
	{
		FString SchemaType;
		if (!InputSchema->TryGetStringField(TEXT("type"), SchemaType) || SchemaType != TEXT("object"))
		{
			AddError(Errors, FString::Printf(TEXT("%s inputSchema.type must be 'object'."), *Context));
			bValid = false;
		}
	}
	if (Traits.IsValid())
	{
		bValid &= HasRequiredBool(Traits, TEXT("readOnly"), Context + TEXT(".traits"), Errors);
		bValid &= HasRequiredBool(Traits, TEXT("destructive"), Context + TEXT(".traits"), Errors);
		bValid &= HasRequiredBool(Traits, TEXT("expensive"), Context + TEXT(".traits"), Errors);
	}
	if (Output.IsValid())
	{
		FString OutputKind;
		if (!Output->TryGetStringField(TEXT("kind"), OutputKind)
			|| (OutputKind != TEXT("json") && OutputKind != TEXT("image")))
		{
			AddError(
				Errors,
				FString::Printf(TEXT("%s output.kind must be 'json' or 'image'."), *Context));
			bValid = false;
		}
	}

	bValid &= ValidateSearchMetadata(Descriptor, Context, Errors);

	if (Descriptor->HasField(TEXT("requires")))
	{
		if (!Descriptor->HasTypedField<EJson::Object>(TEXT("requires")))
		{
			AddError(Errors, FString::Printf(TEXT("%s.requires must be an object."), *Context));
			bValid = false;
		}
		else
		{
			const TSharedPtr<FJsonObject> Requirements =
				Descriptor->GetObjectField(TEXT("requires"));
			static const TSet<FString> AllowedRequirementFields = {
				TEXT("features"),
				TEXT("plugins"),
				TEXT("modules"),
				TEXT("platforms"),
				TEXT("engine"),
			};
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Requirements->Values)
			{
				if (!AllowedRequirementFields.Contains(Field.Key))
				{
					AddError(
						Errors,
						FString::Printf(
							TEXT("%s.requires.%s is not supported."),
							*Context,
							*Field.Key));
					bValid = false;
				}
			}
			bValid &= ValidateOptionalStringArray(
				Requirements, TEXT("features"), Context + TEXT(".requires"), Errors);
			bValid &= ValidateOptionalStringArray(
				Requirements, TEXT("plugins"), Context + TEXT(".requires"), Errors);
			bValid &= ValidateOptionalStringArray(
				Requirements, TEXT("modules"), Context + TEXT(".requires"), Errors);
			bValid &= ValidateOptionalStringArray(
				Requirements, TEXT("platforms"), Context + TEXT(".requires"), Errors);

			if (Requirements->HasField(TEXT("engine")))
			{
				if (!Requirements->HasTypedField<EJson::Object>(TEXT("engine")))
				{
					AddError(
						Errors,
						FString::Printf(TEXT("%s.requires.engine must be an object."), *Context));
					bValid = false;
				}
				else
				{
					const TSharedPtr<FJsonObject> Engine =
						Requirements->GetObjectField(TEXT("engine"));
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Engine->Values)
					{
						if (Field.Key != TEXT("min") && Field.Key != TEXT("maxExclusive"))
						{
							AddError(
								Errors,
								FString::Printf(
									TEXT("%s.requires.engine.%s is not supported."),
									*Context,
									*Field.Key));
							bValid = false;
						}
						else if (!Field.Value.IsValid()
							|| Field.Value->Type != EJson::String
							|| Field.Value->AsString().IsEmpty())
						{
							AddError(
								Errors,
								FString::Printf(
									TEXT("%s.requires.engine.%s must be a non-empty string."),
									*Context,
									*Field.Key));
							bValid = false;
						}
					}
				}
			}
		}
	}

	return bValid;
}
}

bool IsDottedCapabilityId(const FString& Id)
{
	TArray<FString> Segments;
	Id.ParseIntoArray(Segments, TEXT("."), false);
	if (Segments.Num() < 2)
	{
		return false;
	}

	for (const FString& Segment : Segments)
	{
		if (Segment.IsEmpty() || !FChar::IsLower(Segment[0]))
		{
			return false;
		}
		for (const TCHAR Character : Segment)
		{
			if (!FChar::IsLower(Character)
				&& !FChar::IsDigit(Character)
				&& Character != TEXT('_'))
			{
				return false;
			}
		}
	}
	return true;
}

bool LoadCapabilityCatalog(
	const FString& Directory,
	FCapabilityCatalogData& OutCatalog,
	TArray<FString>& OutErrors)
{
	OutCatalog = FCapabilityCatalogData();
	OutErrors.Reset();
	TSet<FString> SeenIds;

	for (const FManifestSpec& Spec : ManifestSpecs)
	{
		const FString ManifestPath = FPaths::Combine(Directory, Spec.FileName);
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
		{
			AddError(
				OutErrors,
				FString::Printf(TEXT("Capability manifest is missing or unreadable: %s"), *ManifestPath));
			continue;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			AddError(
				OutErrors,
				FString::Printf(TEXT("Capability manifest contains invalid JSON: %s"), *ManifestPath));
			continue;
		}

		double SchemaVersion = 0.0;
		if (!Root->TryGetNumberField(TEXT("schemaVersion"), SchemaVersion)
			|| !FMath::IsNearlyEqual(SchemaVersion, 2.0))
		{
			AddError(
				OutErrors,
				FString::Printf(TEXT("%s must declare schemaVersion 2."), Spec.FileName));
		}

		FString Domain;
		if (!Root->TryGetStringField(TEXT("domain"), Domain) || Domain != Spec.Domain)
		{
			AddError(
				OutErrors,
				FString::Printf(TEXT("%s must declare domain '%s'."), Spec.FileName, Spec.Domain));
		}

		if (!Root->HasTypedField<EJson::Array>(TEXT("capabilities")))
		{
			AddError(
				OutErrors,
				FString::Printf(TEXT("%s must contain a capabilities array."), Spec.FileName));
			continue;
		}

		int32 DomainCount = 0;
		const TArray<TSharedPtr<FJsonValue>>& Capabilities =
			Root->GetArrayField(TEXT("capabilities"));
		for (int32 Index = 0; Index < Capabilities.Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = Capabilities[Index];
			const FString Context =
				FString::Printf(TEXT("%s capabilities[%d]"), Spec.FileName, Index);
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				AddError(OutErrors, Context + TEXT(" must be an object."));
				continue;
			}

			const TSharedPtr<FJsonObject> Descriptor = Value->AsObject();
			if (!ValidateDescriptor(Descriptor, Spec.Domain, Context, OutErrors))
			{
				continue;
			}

			const FString Id = Descriptor->GetStringField(TEXT("id"));
			if (SeenIds.Contains(Id))
			{
				AddError(
					OutErrors,
					FString::Printf(TEXT("Duplicate capability id '%s' in manifests."), *Id));
				continue;
			}

			SeenIds.Add(Id);
			const TSharedPtr<FJsonObject> Traits = Descriptor->GetObjectField(TEXT("traits"));
			const TSharedPtr<FJsonObject> Output = Descriptor->GetObjectField(TEXT("output"));

			FMCPCapabilityDescriptor TypedDescriptor;
			TypedDescriptor.Id = Id;
			TypedDescriptor.Domain = Descriptor->GetStringField(TEXT("domain"));
			TypedDescriptor.Kind = Descriptor->GetStringField(TEXT("kind"));
			TypedDescriptor.Description = Descriptor->GetStringField(TEXT("description"));
			TypedDescriptor.InputSchema = Descriptor->GetObjectField(TEXT("inputSchema"));
			TypedDescriptor.bReadOnly = Traits->GetBoolField(TEXT("readOnly"));
			TypedDescriptor.bDestructive = Traits->GetBoolField(TEXT("destructive"));
			TypedDescriptor.bExpensive = Traits->GetBoolField(TEXT("expensive"));
			TypedDescriptor.OutputKind = Output->GetStringField(TEXT("kind"));
			TypedDescriptor.Json = Descriptor;
			OutCatalog.Descriptors.Add(MoveTemp(TypedDescriptor));
			OutCatalog.InputSchemas.Add(Id, Descriptor->GetObjectField(TEXT("inputSchema")));
			++DomainCount;
		}
		OutCatalog.DomainCounts.Add(Spec.Domain, DomainCount);
	}

	return OutErrors.IsEmpty();
}

bool ValidateCapabilityParams(
	const TSharedPtr<FJsonObject>& Schema,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutErrors)
{
	OutErrors.Reset();
	if (!Schema.IsValid())
	{
		AddError(OutErrors, TEXT("Capability input schema is unavailable."));
		return false;
	}
	if (!Params.IsValid())
	{
		AddError(OutErrors, TEXT("params must be an object."));
		return false;
	}
	return ValidateJsonValue(
		MakeShared<FJsonValueObject>(Params),
		Schema,
		TEXT("params"),
		OutErrors);
}
}
}
