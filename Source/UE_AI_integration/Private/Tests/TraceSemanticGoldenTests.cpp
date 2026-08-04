#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "TraceAnalysisService.h"

namespace UEAIIntegration::Tests
{
namespace
{
TSharedPtr<FJsonValue> TraceValueToJson(const UEAI::Trace::FTraceValue& Value)
{
	switch (Value.Type)
	{
	case UEAI::Trace::ETraceValueType::Boolean:
		return MakeShared<FJsonValueBoolean>(Value.BooleanValue);
	case UEAI::Trace::ETraceValueType::Integer:
		return MakeShared<FJsonValueNumber>(
			static_cast<double>(Value.IntegerValue));
	case UEAI::Trace::ETraceValueType::Number:
		if (FMath::IsFinite(Value.NumberValue))
		{
			return MakeShared<FJsonValueNumber>(Value.NumberValue);
		}
		return MakeShared<FJsonValueNull>();
	case UEAI::Trace::ETraceValueType::String:
		return MakeShared<FJsonValueString>(Value.StringValue);
	case UEAI::Trace::ETraceValueType::Null:
	default:
		return MakeShared<FJsonValueNull>();
	}
}

TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Result.Add(MakeShared<FJsonValueString>(Value));
	}
	return Result;
}

TSharedPtr<FJsonObject> QueryToJson(
	const UEAI::Trace::FTraceQueryResult& Query)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("schema"), Query.Schema);
	Result->SetStringField(TEXT("provider"), Query.Provider);
	Result->SetStringField(TEXT("operation"), Query.Operation);
	Result->SetNumberField(
		TEXT("intervalStartSeconds"), Query.IntervalStartSeconds);
	Result->SetNumberField(
		TEXT("intervalEndSeconds"), Query.IntervalEndSeconds);
	Result->SetNumberField(TEXT("total"), static_cast<double>(Query.TotalRows));
	Result->SetBoolField(TEXT("truncated"), Query.bTruncated);
	if (Query.bHasNextCursor)
	{
		Result->SetStringField(
			TEXT("nextCursor"), LexToString(Query.NextCursor));
	}
	else
	{
		Result->SetField(TEXT("nextCursor"), MakeShared<FJsonValueNull>());
	}
	Result->SetArrayField(TEXT("columns"), StringsToJson(Query.Columns));

	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(Query.Rows.Num());
	for (const UEAI::Trace::FTraceRow& QueryRow : Query.Rows)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		for (const FString& Column : Query.Columns)
		{
			const UEAI::Trace::FTraceValue* Value =
				QueryRow.Fields.Find(Column);
			Row->SetField(
				Column,
				Value ? TraceValueToJson(*Value) : MakeShared<FJsonValueNull>());
		}
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	Result->SetArrayField(TEXT("rows"), Rows);

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const UEAI::Trace::FTraceDiagnostic& Diagnostic : Query.Diagnostics)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("severity"), Diagnostic.Severity);
		Item->SetStringField(TEXT("code"), Diagnostic.Code);
		Item->SetStringField(TEXT("message"), Diagnostic.Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Item));
	}
	Result->SetArrayField(TEXT("diagnostics"), Diagnostics);
	return Result;
}

bool JsonEqual(
	const TSharedPtr<FJsonValue>& Expected,
	const TSharedPtr<FJsonValue>& Actual,
	const FString& Path,
	FString& OutDifference)
{
	if (!Expected.IsValid() || !Actual.IsValid())
	{
		if (Expected.IsValid() == Actual.IsValid())
		{
			return true;
		}
		OutDifference = Path + TEXT(": one JSON value is invalid");
		return false;
	}
	if (Expected->Type != Actual->Type)
	{
		OutDifference = FString::Printf(
			TEXT("%s: JSON type differs (%d != %d)"),
			*Path,
			static_cast<int32>(Expected->Type),
			static_cast<int32>(Actual->Type));
		return false;
	}
	switch (Expected->Type)
	{
	case EJson::None:
	case EJson::Null:
		return true;
	case EJson::String:
		if (Expected->AsString() == Actual->AsString())
		{
			return true;
		}
		OutDifference = FString::Printf(
			TEXT("%s: string differs ('%s' != '%s')"),
			*Path, *Expected->AsString(), *Actual->AsString());
		return false;
	case EJson::Number:
		if (FMath::IsNearlyEqual(
			Expected->AsNumber(), Actual->AsNumber(), 1.0e-9))
		{
			return true;
		}
		OutDifference = FString::Printf(
			TEXT("%s: number differs (%.17g != %.17g)"),
			*Path, Expected->AsNumber(), Actual->AsNumber());
		return false;
	case EJson::Boolean:
		if (Expected->AsBool() == Actual->AsBool())
		{
			return true;
		}
		OutDifference = Path + TEXT(": boolean differs");
		return false;
	case EJson::Array:
	{
		const TArray<TSharedPtr<FJsonValue>>& ExpectedValues =
			Expected->AsArray();
		const TArray<TSharedPtr<FJsonValue>>& ActualValues = Actual->AsArray();
		if (ExpectedValues.Num() != ActualValues.Num())
		{
			OutDifference = FString::Printf(
				TEXT("%s: array size differs (%d != %d)"),
				*Path, ExpectedValues.Num(), ActualValues.Num());
			return false;
		}
		for (int32 Index = 0; Index < ExpectedValues.Num(); ++Index)
		{
			if (!JsonEqual(
					ExpectedValues[Index],
					ActualValues[Index],
					FString::Printf(TEXT("%s[%d]"), *Path, Index),
					OutDifference))
			{
				return false;
			}
		}
		return true;
	}
	case EJson::Object:
	{
		const TSharedPtr<FJsonObject> ExpectedObject = Expected->AsObject();
		const TSharedPtr<FJsonObject> ActualObject = Actual->AsObject();
		if (!ExpectedObject.IsValid() || !ActualObject.IsValid()
			|| ExpectedObject->Values.Num() != ActualObject->Values.Num())
		{
			OutDifference = Path + TEXT(": object size or validity differs");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: ExpectedObject->Values)
		{
			const TSharedPtr<FJsonValue>* ActualValue =
				ActualObject->Values.Find(Pair.Key);
			if (!ActualValue)
			{
				OutDifference = Path + TEXT(": missing field '")
					+ Pair.Key + TEXT("'");
				return false;
			}
			if (!JsonEqual(
					Pair.Value,
					*ActualValue,
					Path + TEXT(".") + Pair.Key,
					OutDifference))
			{
				return false;
			}
		}
		return true;
	}
	default:
		OutDifference = Path + TEXT(": unsupported JSON type");
		return false;
	}
}

bool MapVectorRequest(
	const TSharedPtr<FJsonObject>& Vector,
	UEAI::Trace::FTraceQueryRequest& OutRequest,
	FString& OutError)
{
	const TSharedPtr<FJsonObject>* CoreRequest = nullptr;
	if (!Vector.IsValid()
		|| !Vector->TryGetObjectField(TEXT("coreRequest"), CoreRequest)
		|| !CoreRequest || !CoreRequest->IsValid()
		|| !(*CoreRequest)->TryGetStringField(
			TEXT("provider"), OutRequest.Provider))
	{
		OutError = TEXT("Golden vector is missing coreRequest.provider.");
		return false;
	}
	const TSharedPtr<FJsonObject>* Params = nullptr;
	if (!(*CoreRequest)->TryGetObjectField(TEXT("params"), Params)
		|| !Params || !Params->IsValid()
		|| !(*Params)->TryGetStringField(TEXT("operation"), OutRequest.Operation))
	{
		OutError = TEXT("Golden vector is missing coreRequest.params.operation.");
		return false;
	}
	(*Params)->TryGetStringField(TEXT("filter"), OutRequest.Filter);
	double Limit = UEAI::Trace::DefaultPageLimit;
	(*Params)->TryGetNumberField(TEXT("limit"), Limit);
	OutRequest.Page.Limit = FMath::Clamp(
		static_cast<int32>(Limit), 1, UEAI::Trace::MaximumPageLimit);
	FString Cursor;
	if ((*Params)->TryGetStringField(TEXT("cursor"), Cursor)
		&& !LexTryParseString(OutRequest.Page.Cursor, *Cursor))
	{
		OutError = TEXT("Golden vector cursor is not an unsigned integer.");
		return false;
	}
	if ((*Params)->HasField(TEXT("startTimeSeconds")))
	{
		OutRequest.TimeRange.bHasStart = true;
		(*Params)->TryGetNumberField(
			TEXT("startTimeSeconds"), OutRequest.TimeRange.StartSeconds);
	}
	if ((*Params)->HasField(TEXT("endTimeSeconds")))
	{
		OutRequest.TimeRange.bHasEnd = true;
		(*Params)->TryGetNumberField(
			TEXT("endTimeSeconds"), OutRequest.TimeRange.EndSeconds);
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Params)->Values)
	{
		if (!Pair.Value.IsValid()
			|| Pair.Key == TEXT("operation")
			|| Pair.Key == TEXT("filter")
			|| Pair.Key == TEXT("cursor")
			|| Pair.Key == TEXT("limit")
			|| Pair.Key == TEXT("startTimeSeconds")
			|| Pair.Key == TEXT("endTimeSeconds"))
		{
			continue;
		}
		if (Pair.Value->Type == EJson::String)
		{
			OutRequest.Options.Add(Pair.Key, Pair.Value->AsString());
		}
		else if (Pair.Value->Type == EJson::Boolean)
		{
			OutRequest.Options.Add(
				Pair.Key, Pair.Value->AsBool() ? TEXT("true") : TEXT("false"));
		}
		else if (Pair.Value->Type == EJson::Number)
		{
			OutRequest.Options.Add(
				Pair.Key, LexToString(Pair.Value->AsNumber()));
		}
	}
	return true;
}

bool SaveJson(const FString& Path, const TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
			&Text);
	return FJsonSerializer::Serialize(Object.ToSharedRef(), Writer)
		&& FFileHelper::SaveStringToFile(
			Text,
			*Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTraceSemanticCrossRuntimeGoldenTest,
	"UE_AI_integration.Trace.Core.CrossRuntimeSemanticGolden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTraceSemanticCrossRuntimeGoldenTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	if (!FParse::Param(FCommandLine::Get(), TEXT("UEAITraceSemanticGolden")))
	{
		AddInfo(TEXT(
			"Cross-runtime Trace golden is an external-process release gate; pass "
			"-UEAITraceSemanticGolden with fixture and expected paths to execute it."));
		return true;
	}

	FString FixturePath;
	FString ExpectedPath;
	FString ActualPath;
	if (!FParse::Value(
			FCommandLine::Get(), TEXT("UEAITraceGoldenFixture="), FixturePath)
		|| !FParse::Value(
			FCommandLine::Get(), TEXT("UEAITraceGoldenExpected="), ExpectedPath)
		|| !FParse::Value(
			FCommandLine::Get(), TEXT("UEAITraceGoldenActual="), ActualPath))
	{
		AddError(TEXT(
			"Cross-runtime Trace golden requires fixture, expected, and actual paths."));
		return false;
	}

	FString ExpectedText;
	TSharedPtr<FJsonObject> ExpectedRoot;
	FString ExpectedSchema;
	if (!FFileHelper::LoadFileToString(ExpectedText, *ExpectedPath)
		|| !FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(ExpectedText), ExpectedRoot)
		|| !ExpectedRoot.IsValid()
		|| !ExpectedRoot->TryGetStringField(TEXT("schema"), ExpectedSchema)
		|| ExpectedSchema
			!= TEXT("ue.trace-semantic-cross-runtime-golden.v1"))
	{
		AddError(TEXT("The Worker golden result file is invalid."));
		return false;
	}

	UEAI::Trace::FTraceAnalysisSession Analysis;
	FString ErrorCode;
	FString ErrorMessage;
	if (!Analysis.Open(
			FixturePath,
			60.0,
			ErrorCode,
			ErrorMessage))
	{
		AddError(FString::Printf(
			TEXT("Editor TraceAnalysisCore could not open the shared fixture: %s: %s"),
			*ErrorCode,
			*ErrorMessage));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Vectors = nullptr;
	if (!ExpectedRoot->TryGetArrayField(TEXT("vectors"), Vectors)
		|| !Vectors || Vectors->IsEmpty())
	{
		AddError(TEXT("The Worker golden result contains no vectors."));
		return false;
	}

	TSharedPtr<FJsonObject> ActualRoot = MakeShared<FJsonObject>();
	ActualRoot->SetStringField(
		TEXT("schema"), TEXT("ue.trace-semantic-cross-runtime-golden.v1"));
	TArray<TSharedPtr<FJsonValue>> ActualVectors;
	bool bAllEqual = true;
	for (const TSharedPtr<FJsonValue>& VectorValue : *Vectors)
	{
		const TSharedPtr<FJsonObject> Vector = VectorValue.IsValid()
			? VectorValue->AsObject() : nullptr;
		FString VectorId;
		if (!Vector.IsValid()
			|| !Vector->TryGetStringField(TEXT("id"), VectorId))
		{
			AddError(TEXT("A cross-runtime Trace vector has no id."));
			bAllEqual = false;
			continue;
		}
		UEAI::Trace::FTraceQueryRequest Request;
		FString MappingError;
		if (!MapVectorRequest(Vector, Request, MappingError))
		{
			AddError(VectorId + TEXT(": ") + MappingError);
			bAllEqual = false;
			continue;
		}

		UEAI::Trace::FTraceQueryResult Query;
		ErrorCode.Reset();
		ErrorMessage.Reset();
		const bool bSucceeded = Analysis.Query(
			Request, Query, ErrorCode, ErrorMessage);
		TSharedPtr<FJsonObject> Actual = MakeShared<FJsonObject>();
		Actual->SetBoolField(TEXT("ok"), bSucceeded);
		if (bSucceeded)
		{
			Actual->SetObjectField(TEXT("query"), QueryToJson(Query));
		}
		else
		{
			Actual->SetStringField(TEXT("errorCode"), ErrorCode);
		}

		TSharedPtr<FJsonObject> ActualVector = MakeShared<FJsonObject>();
		ActualVector->SetStringField(TEXT("id"), VectorId);
		ActualVector->SetObjectField(TEXT("actual"), Actual);
		ActualVectors.Add(MakeShared<FJsonValueObject>(ActualVector));

		const TSharedPtr<FJsonObject>* Expected = nullptr;
		if (!Vector->TryGetObjectField(TEXT("expected"), Expected)
			|| !Expected || !Expected->IsValid())
		{
			AddError(VectorId + TEXT(": Worker expected result is missing."));
			bAllEqual = false;
			continue;
		}
		FString Difference;
		if (!JsonEqual(
				MakeShared<FJsonValueObject>(*Expected),
				MakeShared<FJsonValueObject>(Actual),
				VectorId,
				Difference))
		{
			AddError(TEXT("Worker/Editor Trace semantic drift: ") + Difference);
			bAllEqual = false;
		}

		if (VectorId == TEXT("timing.events.page0") && bSucceeded)
		{
			TestTrue(
				TEXT("The first timing page is a real bounded page"),
				Query.Rows.Num() == 1 && Query.TotalRows >= 2
					&& Query.bHasNextCursor && Query.NextCursor == 1);
		}
		else if (VectorId == TEXT("timing.events.maxCursor") && bSucceeded)
		{
			TestTrue(
				TEXT("Maximum uint64 cursor does not wrap pagination"),
				Query.Rows.IsEmpty() && !Query.bHasNextCursor);
		}
		else if (VectorId == TEXT("timing.threads.range") && bSucceeded)
		{
			TestTrue(
				TEXT("Timing thread metadata reports the requested bounded interval"),
				Query.IntervalStartSeconds == 0.0
					&& Query.IntervalEndSeconds > 0.0
					&& Query.IntervalEndSeconds <= 60.0);
			bool bFoundActiveThread = false;
			for (int32 Index = 0; Index < Query.Rows.Num(); ++Index)
			{
				const UEAI::Trace::FTraceValue* Active =
					Query.Rows[Index].Fields.Find(TEXT("activeInRange"));
				bFoundActiveThread |= Active
					&& Active->Type == UEAI::Trace::ETraceValueType::Boolean
					&& Active->BooleanValue;
				if (Index == 0)
				{
					continue;
				}
				const UEAI::Trace::FTraceValue* Previous =
					Query.Rows[Index - 1].Fields.Find(TEXT("threadId"));
				const UEAI::Trace::FTraceValue* Current =
					Query.Rows[Index].Fields.Find(TEXT("threadId"));
				TestTrue(
					TEXT("Timing threads are stably ordered by thread id"),
					Previous && Current
						&& Previous->IntegerValue <= Current->IntegerValue);
			}
			TestTrue(
				TEXT("Timing thread activity is evaluated inside the requested interval"),
				bFoundActiveThread);
		}
	}
	ActualRoot->SetArrayField(TEXT("vectors"), ActualVectors);
	if (!SaveJson(ActualPath, ActualRoot))
	{
		AddError(TEXT("The Editor Trace golden artifact could not be written."));
		return false;
	}
	return bAllEqual;
}
}

#endif
