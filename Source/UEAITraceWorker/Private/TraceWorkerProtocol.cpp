#include "TraceWorkerProtocol.h"
#include "TraceWorkerStore.h"
#include "TraceWorkerLaunch.h"
#include "TraceWorkerCommandLine.h"

#include "HAL/FileManager.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "TraceAnalysisContracts.h"
#include "TraceAnalysisService.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/WindowsPlatformMisc.h"
#endif

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#ifndef UEAI_TRACE_WORKER_VERSION
#define UEAI_TRACE_WORKER_VERSION "0.9.0"
#endif

#ifndef UEAI_TRACE_CONTRACT_DIGEST
#define UEAI_TRACE_CONTRACT_DIGEST "unbound"
#endif

#ifndef UEAI_TRACE_PROVIDER_SCHEMA_DIGEST
#define UEAI_TRACE_PROVIDER_SCHEMA_DIGEST "unbound"
#endif

namespace UEAI::TraceWorker
{
namespace
{
using namespace UEAI::Trace;

constexpr int32 MaximumRequestIdLength = 128;
constexpr int64 MaximumFixtureReceiptBytes = 1024 * 1024;
constexpr int32 AnalysisSessionCacheCapacity = 2;
constexpr int64 DefaultMaximumExportBytes = 64ll * 1024ll * 1024ll;
constexpr int64 DefaultMaximumExportCellBytes = 1ll * 1024ll * 1024ll;

struct FCachedAnalysisSession
{
	FString Sha256;
	FString TracePath;
	TSharedPtr<FTraceAnalysisSession> Session;
	uint64 LastUse = 0;
};

FCriticalSection AnalysisSessionCacheMutex;
TArray<FCachedAnalysisSession> AnalysisSessionCache;
uint64 AnalysisSessionCacheClock = 0;

FString GetString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Name,
	const FString& Default = FString())
{
	FString Value;
	return Object.IsValid() && Object->TryGetStringField(Name, Value)
		? Value
		: Default;
}

double GetNumber(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Name,
	const double Default)
{
	double Value = Default;
	return Object.IsValid() && Object->TryGetNumberField(Name, Value)
		&& FMath::IsFinite(Value)
		? Value
		: Default;
}

bool IsVerifiedDiagnosticFixture(
	const FTraceRecord& Record,
	const FString& CommandLine)
{
	if (!HasCommandLineSwitch(
			*CommandLine,
			TEXT("allowTestFixtureUnknownEngineVersion")))
	{
		return false;
	}
	const FString ReceiptPath = Record.SourcePath + TEXT(".receipt.json");
	const int64 ReceiptSize = IFileManager::Get().FileSize(*ReceiptPath);
	if (ReceiptSize <= 0 || ReceiptSize > MaximumFixtureReceiptBytes)
	{
		return false;
	}
	FString Text;
	TSharedPtr<FJsonObject> Receipt;
	bool bTestOnly = false;
	return FFileHelper::LoadFileToString(Text, *ReceiptPath)
		&& FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Text), Receipt)
		&& Receipt.IsValid()
		&& GetString(Receipt, TEXT("schema"))
			== TEXT("ue.trace-diagnostic-fixture.v1")
		&& Receipt->TryGetBoolField(TEXT("testOnly"), bTestOnly)
		&& bTestOnly
		&& FPaths::IsSamePath(
			GetString(Receipt, TEXT("tracePath")), Record.SourcePath)
		&& GetString(Receipt, TEXT("engineVersion"))
			== FEngineVersion::Current().ToString()
		&& static_cast<int64>(GetNumber(
			Receipt, TEXT("traceSizeBytes"), -1.0)) == Record.SizeBytes;
}

TSharedPtr<FTraceAnalysisSession> AcquireAnalysisSession(
	const FTraceRecord& Record,
	const FString& CommandLine,
	const double TimeoutSeconds,
	FString& OutErrorCode,
	FString& OutErrorMessage)
{
	FScopeLock Lock(&AnalysisSessionCacheMutex);
	++AnalysisSessionCacheClock;
	for (FCachedAnalysisSession& Cached : AnalysisSessionCache)
	{
		if (Cached.Sha256 == Record.Sha256
			&& Cached.Session.IsValid()
			&& Cached.Session->IsOpen())
		{
			Cached.LastUse = AnalysisSessionCacheClock;
			return Cached.Session;
		}
	}
	TSharedPtr<FTraceAnalysisSession> Session =
		MakeShared<FTraceAnalysisSession>();
	if (!Session->Open(
			Record.TracePath,
			TimeoutSeconds,
			OutErrorCode,
			OutErrorMessage,
			IsVerifiedDiagnosticFixture(Record, CommandLine)))
	{
		return nullptr;
	}
	if (AnalysisSessionCache.Num() >= AnalysisSessionCacheCapacity)
	{
		int32 OldestIndex = 0;
		for (int32 Index = 1; Index < AnalysisSessionCache.Num(); ++Index)
		{
			if (AnalysisSessionCache[Index].LastUse
				< AnalysisSessionCache[OldestIndex].LastUse)
			{
				OldestIndex = Index;
			}
		}
		AnalysisSessionCache.RemoveAt(OldestIndex);
	}
	FCachedAnalysisSession& Cached = AnalysisSessionCache.AddDefaulted_GetRef();
	Cached.Sha256 = Record.Sha256;
	Cached.TracePath = Record.TracePath;
	Cached.Session = Session;
	Cached.LastUse = AnalysisSessionCacheClock;
	return Session;
}

TSharedPtr<FJsonValue> ToJsonValue(const FTraceValue& Value)
{
	switch (Value.Type)
	{
	case ETraceValueType::Boolean:
		return MakeShared<FJsonValueBoolean>(Value.BooleanValue);
	case ETraceValueType::Integer:
		return MakeShared<FJsonValueNumber>(
			static_cast<double>(Value.IntegerValue));
	case ETraceValueType::Number:
		if (FMath::IsFinite(Value.NumberValue))
		{
			return MakeShared<FJsonValueNumber>(Value.NumberValue);
		}
		return MakeShared<FJsonValueNull>();
	case ETraceValueType::String:
		return MakeShared<FJsonValueString>(Value.StringValue);
	case ETraceValueType::Null:
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

TSharedPtr<FJsonObject> ProviderDescriptorToJson(
	const FTraceProviderDescriptor& Descriptor)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Descriptor.Id);
	Object->SetStringField(TEXT("displayName"), Descriptor.DisplayName);
	Object->SetStringField(TEXT("insightsPanel"), Descriptor.InsightsPanel);
	Object->SetArrayField(
		TEXT("providerNames"), StringsToJson(Descriptor.ProviderNames));
	Object->SetArrayField(
		TEXT("requiredChannels"), StringsToJson(Descriptor.RequiredChannels));
	Object->SetArrayField(TEXT("operations"), StringsToJson(Descriptor.Operations));
	Object->SetBoolField(TEXT("queryImplemented"), Descriptor.bQueryImplemented);
	return Object;
}

TSharedPtr<FJsonObject> ProviderStatusToJson(
	const FTraceProviderStatus& Status)
{
	TSharedPtr<FJsonObject> Object = ProviderDescriptorToJson(Status.Descriptor);
	Object->SetBoolField(TEXT("recorded"), Status.bRecorded);
	Object->SetArrayField(
		TEXT("recordedProviderNames"),
		StringsToJson(Status.RecordedProviderNames));
	Object->SetArrayField(
		TEXT("missingProviderNames"),
		StringsToJson(Status.MissingProviderNames));
	Object->SetStringField(TEXT("channelStatus"), Status.ChannelStatus);
	Object->SetArrayField(
		TEXT("recordedChannels"), StringsToJson(Status.RecordedChannels));
	Object->SetArrayField(
		TEXT("missingChannels"), StringsToJson(Status.MissingChannels));
	Object->SetStringField(TEXT("unavailableReason"), Status.UnavailableReason);
	return Object;
}

TSharedPtr<FJsonObject> QueryResultToJson(const FTraceQueryResult& Result)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("schema"), Result.Schema);
	Object->SetStringField(TEXT("provider"), Result.Provider);
	Object->SetStringField(TEXT("operation"), Result.Operation);
	Object->SetNumberField(
		TEXT("intervalStartSeconds"), Result.IntervalStartSeconds);
	Object->SetNumberField(
		TEXT("intervalEndSeconds"), Result.IntervalEndSeconds);
	Object->SetNumberField(TEXT("total"), static_cast<double>(Result.TotalRows));
	Object->SetBoolField(TEXT("truncated"), Result.bTruncated);
	if (Result.bHasNextCursor)
	{
		Object->SetStringField(
			TEXT("nextCursor"), LexToString(Result.NextCursor));
	}
	else
	{
		Object->SetField(TEXT("nextCursor"), MakeShared<FJsonValueNull>());
	}
	Object->SetArrayField(TEXT("columns"), StringsToJson(Result.Columns));

	TArray<TSharedPtr<FJsonValue>> RowValues;
	RowValues.Reserve(Result.Rows.Num());
	for (const FTraceRow& Row : Result.Rows)
	{
		TSharedPtr<FJsonObject> RowObject = MakeShared<FJsonObject>();
		for (const FString& Column : Result.Columns)
		{
			const FTraceValue* Value = Row.Fields.Find(Column);
			RowObject->SetField(
				Column,
				Value ? ToJsonValue(*Value) : MakeShared<FJsonValueNull>());
		}
		RowValues.Add(MakeShared<FJsonValueObject>(RowObject));
	}
	Object->SetArrayField(TEXT("rows"), RowValues);

	TArray<TSharedPtr<FJsonValue>> Diagnostics;
	for (const FTraceDiagnostic& Diagnostic : Result.Diagnostics)
	{
		TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("severity"), Diagnostic.Severity);
		Value->SetStringField(TEXT("code"), Diagnostic.Code);
		Value->SetStringField(TEXT("message"), Diagnostic.Message);
		Diagnostics.Add(MakeShared<FJsonValueObject>(Value));
	}
	Object->SetArrayField(TEXT("diagnostics"), Diagnostics);
	return Object;
}

FString CsvEscape(const FString& Value)
{
	FString Escaped = Value.Replace(TEXT("\""), TEXT("\"\""));
	return FString::Printf(TEXT("\"%s\""), *Escaped);
}

FString TraceValueToCsv(const FTraceValue& Value)
{
	switch (Value.Type)
	{
	case ETraceValueType::Boolean:
		return Value.BooleanValue ? TEXT("true") : TEXT("false");
	case ETraceValueType::Integer:
		return LexToString(Value.IntegerValue);
	case ETraceValueType::Number:
		return FMath::IsFinite(Value.NumberValue)
			? LexToString(Value.NumberValue)
			: FString();
	case ETraceValueType::String:
		return CsvEscape(Value.StringValue);
	case ETraceValueType::Null:
	default:
		return FString();
	}
}

bool AccountExportPage(
	const FTraceQueryResult& Page,
	const FString& Format,
	const bool bFirstPage,
	const int64 MaximumBytes,
	const int64 MaximumCellBytes,
	int64& InOutEstimatedBytes,
	FString& OutErrorCode,
	FString& OutError)
{
	auto AccountText = [&](const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		if (Utf8.Length() < 0
			|| InOutEstimatedBytes + Utf8.Length() > MaximumBytes)
		{
			OutErrorCode = TEXT("trace_export_too_large");
			OutError = TEXT(
				"The normalized Trace export exceeded maxBytes before rows were retained.");
			return false;
		}
		InOutEstimatedBytes += Utf8.Length();
		return true;
	};
	auto CellWithinBound = [&](const FTraceValue* Value)
	{
		if (!Value || Value->Type != ETraceValueType::String)
		{
			return true;
		}
		const FTCHARToUTF8 Utf8(*Value->StringValue);
		if (Utf8.Length() <= MaximumCellBytes)
		{
			return true;
		}
		OutErrorCode = TEXT("trace_export_cell_too_large");
		OutError = TEXT(
			"A Trace export cell exceeded maxCellBytes before rows were retained.");
		return false;
	};

	if (bFirstPage)
	{
		if (Format == TEXT("json"))
		{
			FTraceQueryResult Metadata = Page;
			Metadata.Rows.Reset();
			TSharedPtr<FJsonObject> MetadataJson = QueryResultToJson(Metadata);
			MetadataJson->RemoveField(TEXT("rows"));
			FString MetadataText;
			const TSharedRef<
				TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<
						TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
							&MetadataText);
			FJsonSerializer::Serialize(MetadataJson.ToSharedRef(), Writer);
			if (!AccountText(
					TEXT("{\"schema\":\"ue.trace-export.v1\",\"query\":")
					+ MetadataText + TEXT(",\"rows\":[")))
			{
				return false;
			}
		}
		else
		{
			TArray<FString> EscapedColumns;
			for (const FString& Column : Page.Columns)
			{
				const FTCHARToUTF8 Utf8(*Column);
				if (Utf8.Length() > MaximumCellBytes)
				{
					OutErrorCode = TEXT("trace_export_cell_too_large");
					OutError = TEXT(
						"A Trace export column name exceeded maxCellBytes.");
					return false;
				}
				EscapedColumns.Add(CsvEscape(Column));
			}
			if (!AccountText(
					FString::Join(EscapedColumns, TEXT(","))
					+ LINE_TERMINATOR))
			{
				return false;
			}
		}
	}

	for (int32 RowIndex = 0; RowIndex < Page.Rows.Num(); ++RowIndex)
	{
		const FTraceRow& Row = Page.Rows[RowIndex];
		if (Format == TEXT("json"))
		{
			TSharedPtr<FJsonObject> RowObject = MakeShared<FJsonObject>();
			for (const FString& Column : Page.Columns)
			{
				const FTraceValue* Value = Row.Fields.Find(Column);
				if (!CellWithinBound(Value))
				{
					return false;
				}
				RowObject->SetField(
					Column,
					Value ? ToJsonValue(*Value) : MakeShared<FJsonValueNull>());
			}
			FString RowText;
			const TSharedRef<
				TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<
						TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
							&RowText);
			FJsonSerializer::Serialize(RowObject.ToSharedRef(), Writer);
			const bool bNeedsSeparator = InOutEstimatedBytes > 0
				&& !(bFirstPage && RowIndex == 0);
			if (!AccountText(
					FString(bNeedsSeparator ? TEXT(",") : TEXT("")) + RowText))
			{
				return false;
			}
		}
		else
		{
			TArray<FString> Values;
			for (const FString& Column : Page.Columns)
			{
				const FTraceValue* Value = Row.Fields.Find(Column);
				if (!CellWithinBound(Value))
				{
					return false;
				}
				Values.Add(Value ? TraceValueToCsv(*Value) : FString());
			}
			if (!AccountText(
					FString::Join(Values, TEXT(",")) + LINE_TERMINATOR))
			{
				return false;
			}
		}
	}
	return true;
}

bool WriteExportAtomic(
	const FString& Path,
	const FTraceQueryResult& Result,
	const FString& Format,
	const int64 MaximumBytes,
	const int64 MaximumCellBytes,
	int64& OutSizeBytes,
	FString& OutErrorCode,
	FString& OutError)
{
	OutSizeBytes = 0;
	OutErrorCode.Reset();
	OutError.Reset();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FString Temporary = Path + TEXT(".tmp-")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*Temporary));
	if (!Writer)
	{
		OutErrorCode = TEXT("trace_export_failed");
		OutError = TEXT("The Trace export temporary artifact could not be opened.");
		return false;
	}
	auto WriteChunk = [&](const FString& Chunk)
	{
		const FTCHARToUTF8 Utf8(*Chunk);
		if (Utf8.Length() < 0
			|| OutSizeBytes + Utf8.Length() > MaximumBytes)
		{
			OutErrorCode = TEXT("trace_export_too_large");
			OutError = TEXT(
				"The normalized Trace export exceeded maxBytes; the temporary artifact was removed.");
			return false;
		}
		Writer->Serialize(
			const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
		OutSizeBytes += Utf8.Length();
		if (Writer->IsError())
		{
			OutErrorCode = TEXT("trace_export_failed");
			OutError = TEXT("The Trace export could not be written completely.");
			return false;
		}
		return true;
	};
	auto CellWithinBound = [&](const FTraceValue* Value)
	{
		if (!Value || Value->Type != ETraceValueType::String)
		{
			return true;
		}
		const FTCHARToUTF8 Utf8(*Value->StringValue);
		if (Utf8.Length() <= MaximumCellBytes)
		{
			return true;
		}
		OutErrorCode = TEXT("trace_export_cell_too_large");
		OutError = TEXT(
			"A Trace export cell exceeded maxCellBytes; the temporary artifact was removed.");
		return false;
	};
	bool bWritten = true;
	if (Format == TEXT("json"))
	{
		FTraceQueryResult Metadata = Result;
		Metadata.Rows.Reset();
		TSharedPtr<FJsonObject> MetadataJson = QueryResultToJson(Metadata);
		MetadataJson->RemoveField(TEXT("rows"));
		FString MetadataText;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
			MetadataWriter =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
					&MetadataText);
		FJsonSerializer::Serialize(MetadataJson.ToSharedRef(), MetadataWriter);
		bWritten = WriteChunk(
			TEXT("{\"schema\":\"ue.trace-export.v1\",\"query\":")
			+ MetadataText + TEXT(",\"rows\":["));
		for (int32 RowIndex = 0;
			bWritten && RowIndex < Result.Rows.Num();
			++RowIndex)
		{
			const FTraceRow& Row = Result.Rows[RowIndex];
			TSharedPtr<FJsonObject> RowObject = MakeShared<FJsonObject>();
			for (const FString& Column : Result.Columns)
			{
				const FTraceValue* Value = Row.Fields.Find(Column);
				if (!CellWithinBound(Value))
				{
					bWritten = false;
					break;
				}
				RowObject->SetField(
					Column,
					Value ? ToJsonValue(*Value) : MakeShared<FJsonValueNull>());
			}
			if (!bWritten)
			{
				break;
			}
			FString RowText;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				RowWriter =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&RowText);
			FJsonSerializer::Serialize(RowObject.ToSharedRef(), RowWriter);
			bWritten = WriteChunk(
				FString(RowIndex > 0 ? TEXT(",") : TEXT("")) + RowText);
		}
		bWritten = bWritten && WriteChunk(TEXT("]}"));
	}
	else
	{
		TArray<FString> EscapedColumns;
		for (const FString& Column : Result.Columns)
		{
			const FTCHARToUTF8 Utf8(*Column);
			if (Utf8.Length() > MaximumCellBytes)
			{
				OutErrorCode = TEXT("trace_export_cell_too_large");
				OutError = TEXT("A Trace export column name exceeded maxCellBytes.");
				bWritten = false;
				break;
			}
			EscapedColumns.Add(CsvEscape(Column));
		}
		bWritten = bWritten && WriteChunk(
			FString::Join(EscapedColumns, TEXT(",")) + LINE_TERMINATOR);
		for (const FTraceRow& Row : Result.Rows)
		{
			if (!bWritten)
			{
				break;
			}
			TArray<FString> Values;
			for (const FString& Column : Result.Columns)
			{
				const FTraceValue* Value = Row.Fields.Find(Column);
				if (!CellWithinBound(Value))
				{
					bWritten = false;
					break;
				}
				Values.Add(Value ? TraceValueToCsv(*Value) : FString());
			}
			bWritten = bWritten && WriteChunk(
				FString::Join(Values, TEXT(",")) + LINE_TERMINATOR);
		}
	}
	Writer->Flush();
	Writer->Close();
	const bool bArchiveError = Writer->IsError();
	Writer.Reset();
	if (!bWritten || bArchiveError)
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		if (OutErrorCode.IsEmpty())
		{
			OutErrorCode = TEXT("trace_export_failed");
			OutError = TEXT("The Trace export failed before publication.");
		}
		return false;
	}
	if (!IFileManager::Get().Move(
			*Path, *Temporary, true, true, false, true))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutErrorCode = TEXT("trace_export_failed");
		OutError = TEXT("The Trace export could not be published atomically.");
		return false;
	}
	return true;
}

FString PlatformBinariesDirectory()
{
#if PLATFORM_WINDOWS
	return TEXT("Win64");
#elif PLATFORM_MAC
	return TEXT("Mac");
#else
	return TEXT("Linux");
#endif
}

FString InsightsExecutableName()
{
#if PLATFORM_WINDOWS
	return TEXT("UnrealInsights.exe");
#else
	return TEXT("UnrealInsights");
#endif
}

struct FUnrealInsightsResolution
{
	FString Path;
	FString EngineDirectory;
	FString EngineVersion;
	FString Source;
	FString UnavailableReason;
};

FString NormalizeEngineDirectory(const FString& Candidate)
{
	FString EngineDirectory = FPaths::ConvertRelativePathToFull(Candidate);
	FPaths::NormalizeDirectoryName(EngineDirectory);
	FPaths::CollapseRelativeDirectories(EngineDirectory);
	if (!FPaths::GetCleanFilename(EngineDirectory).Equals(
			TEXT("Engine"), ESearchCase::IgnoreCase)
		&& IFileManager::Get().DirectoryExists(
			*FPaths::Combine(EngineDirectory, TEXT("Engine"))))
	{
		EngineDirectory = FPaths::Combine(EngineDirectory, TEXT("Engine"));
		FPaths::NormalizeDirectoryName(EngineDirectory);
	}
	return EngineDirectory;
}

bool ResolveProjectAssociatedEngine(
	FString& OutEngineRoot,
	FString& OutReason)
{
	OutEngineRoot.Reset();
	OutReason.Reset();
	FString Directory = FPaths::GetPath(FPlatformProcess::ExecutablePath());
	FPaths::NormalizeDirectoryName(Directory);
	for (int32 Depth = 0; Depth < 12 && !Directory.IsEmpty(); ++Depth)
	{
		TArray<FString> ProjectFiles;
		IFileManager::Get().FindFiles(
			ProjectFiles,
			*FPaths::Combine(Directory, TEXT("*.uproject")),
			true,
			false);
		if (ProjectFiles.Num() > 1)
		{
			OutReason = FString::Printf(
				TEXT("Multiple .uproject files were found beside the installed Worker at %s."),
				*Directory);
			return true;
		}
		if (ProjectFiles.Num() == 1)
		{
			FString Contents;
			TSharedPtr<FJsonObject> Project;
			if (!FFileHelper::LoadFileToString(
					Contents,
					*FPaths::Combine(Directory, ProjectFiles[0]))
				|| !FJsonSerializer::Deserialize(
					TJsonReaderFactory<>::Create(Contents), Project)
				|| !Project.IsValid())
			{
				OutReason = TEXT("The installed project's .uproject could not be parsed.");
				return true;
			}
			FString Association;
			Project->TryGetStringField(TEXT("EngineAssociation"), Association);
			Association.TrimStartAndEndInline();
			if (Association.IsEmpty())
			{
				// Engine-source Program hosts intentionally omit an association.
				return false;
			}
#if PLATFORM_WINDOWS
			const bool bVersionAssociation =
				Association.Len() >= 3
				&& FChar::IsDigit(Association[0])
				&& Association.Contains(TEXT("."));
			if (bVersionAssociation)
			{
				const FString RegistryKey = FString::Printf(
					TEXT("SOFTWARE\\EpicGames\\Unreal Engine\\%s"),
					*Association);
				if (!FWindowsPlatformMisc::QueryRegKey(
						HKEY_LOCAL_MACHINE,
						*RegistryKey,
						TEXT("InstalledDirectory"),
						OutEngineRoot))
				{
					OutReason = FString::Printf(
						TEXT("EngineAssociation %s is not registered as an exact launcher Engine installation."),
						*Association);
				}
			}
			else if (!FPlatformMisc::GetStoredValue(
				TEXT("Epic Games"),
				TEXT("Unreal Engine/Builds"),
				Association,
				OutEngineRoot))
			{
				OutReason = FString::Printf(
					TEXT("EngineAssociation %s is not registered for the current user."),
					*Association);
			}
#else
			OutReason = TEXT("Project EngineAssociation discovery is unavailable on this platform; set UEAI_ENGINE_ROOT.");
#endif
			return true;
		}
		const FString Parent = FPaths::GetPath(Directory);
		if (Parent.IsEmpty() || Parent == Directory)
		{
			break;
		}
		Directory = Parent;
	}
	return false;
}

FUnrealInsightsResolution ValidateUnrealInsightsEngine(
	const FString& Candidate,
	const FString& Source)
{
	FUnrealInsightsResolution Result;
	Result.Source = Source;
	Result.EngineDirectory = NormalizeEngineDirectory(Candidate);
	if (!IFileManager::Get().DirectoryExists(*Result.EngineDirectory))
	{
		Result.UnavailableReason = FString::Printf(
			TEXT("%s Engine directory does not exist: %s"),
			*Source,
			*Result.EngineDirectory);
		return Result;
	}
	const FString BuildVersionPath = FPaths::Combine(
		Result.EngineDirectory, TEXT("Build/Build.version"));
	FString BuildVersionContents;
	TSharedPtr<FJsonObject> BuildVersion;
	if (!FFileHelper::LoadFileToString(
			BuildVersionContents, *BuildVersionPath)
		|| !FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(BuildVersionContents), BuildVersion)
		|| !BuildVersion.IsValid())
	{
		Result.UnavailableReason = FString::Printf(
			TEXT("%s has no readable Engine/Build/Build.version: %s"),
			*Source,
			*BuildVersionPath);
		return Result;
	}
	double MajorValue = -1.0;
	double MinorValue = -1.0;
	if (!BuildVersion->TryGetNumberField(TEXT("MajorVersion"), MajorValue)
		|| !BuildVersion->TryGetNumberField(TEXT("MinorVersion"), MinorValue)
		|| MajorValue != FMath::FloorToDouble(MajorValue)
		|| MinorValue != FMath::FloorToDouble(MinorValue))
	{
		Result.UnavailableReason = FString::Printf(
			TEXT("%s Engine Build.version has invalid MajorVersion/MinorVersion fields."),
			*Source);
		return Result;
	}
	const int32 Major = static_cast<int32>(MajorValue);
	const int32 Minor = static_cast<int32>(MinorValue);
	Result.EngineVersion = FString::Printf(TEXT("%d.%d"), Major, Minor);
	const FEngineVersion WorkerVersion = FEngineVersion::Current();
	if (Major != static_cast<int32>(WorkerVersion.GetMajor())
		|| Minor != static_cast<int32>(WorkerVersion.GetMinor()))
	{
		Result.UnavailableReason = FString::Printf(
			TEXT("%s Engine version %s does not match this Worker (%d.%d)."),
			*Source,
			*Result.EngineVersion,
			static_cast<int32>(WorkerVersion.GetMajor()),
			static_cast<int32>(WorkerVersion.GetMinor()));
		return Result;
	}
#if PLATFORM_MAC
	Result.Path = FPaths::Combine(
			Result.EngineDirectory,
			TEXT("Binaries/Mac/UnrealInsights.app/Contents/MacOS"),
			InsightsExecutableName());
#else
	Result.Path = FPaths::Combine(
			Result.EngineDirectory,
			TEXT("Binaries"),
			PlatformBinariesDirectory(),
			InsightsExecutableName());
#endif
	Result.Path = FPaths::ConvertRelativePathToFull(Result.Path);
	if (!IFileManager::Get().FileExists(*Result.Path))
	{
		Result.UnavailableReason = FString::Printf(
			TEXT("The matching %s Engine has no Unreal Insights executable at %s."),
			*Result.EngineVersion,
			*Result.Path);
		Result.Path.Reset();
	}
	return Result;
}

FUnrealInsightsResolution ResolveUnrealInsights(const FString& CommandLine)
{
	FString Candidate;
	if (ReadCommandLineValue(*CommandLine, TEXT("EngineDir="), Candidate))
	{
		return ValidateUnrealInsightsEngine(Candidate, TEXT("commandLine"));
	}
	Candidate = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAI_ENGINE_ROOT"));
	if (!Candidate.IsEmpty())
	{
		return ValidateUnrealInsightsEngine(Candidate, TEXT("UEAI_ENGINE_ROOT"));
	}
	Candidate = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_ENGINE_ROOT"));
	if (!Candidate.IsEmpty())
	{
		return ValidateUnrealInsightsEngine(Candidate, TEXT("UE_ENGINE_ROOT"));
	}
	FString AssociationReason;
	if (ResolveProjectAssociatedEngine(Candidate, AssociationReason))
	{
		if (Candidate.IsEmpty())
		{
			FUnrealInsightsResolution Result;
			Result.Source = TEXT("projectAssociation");
			Result.UnavailableReason = AssociationReason;
			return Result;
		}
		return ValidateUnrealInsightsEngine(Candidate, TEXT("projectAssociation"));
	}
	return ValidateUnrealInsightsEngine(FPaths::EngineDir(), TEXT("programEngineDir"));
}

TSharedPtr<FJsonObject> MakeResponseMeta(
	const FString& RequestId,
	const double StartedSeconds)
{
	TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("requestId"), RequestId);
	Meta->SetStringField(TEXT("workerVersion"), TEXT(UEAI_TRACE_WORKER_VERSION));
	Meta->SetStringField(
		TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Meta->SetStringField(TEXT("backend"), TEXT("localTrace"));
	Meta->SetNumberField(
		TEXT("durationMs"),
		FMath::Max(0.0, (FPlatformTime::Seconds() - StartedSeconds) * 1000.0));
	return Meta;
}

TSharedPtr<FJsonObject> Success(
	const FString& RequestId,
	const double StartedSeconds,
	const TSharedPtr<FJsonObject>& Data)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("schema"), TEXT("ue.trace-worker-response.v1"));
	Response->SetBoolField(TEXT("ok"), true);
	Response->SetObjectField(TEXT("data"), Data);
	Response->SetObjectField(
		TEXT("meta"), MakeResponseMeta(RequestId, StartedSeconds));
	return Response;
}

TSharedPtr<FJsonObject> Failure(
	const FString& RequestId,
	const double StartedSeconds,
	const FString& Code,
	const FString& Message)
{
	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetStringField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("schema"), TEXT("ue.trace-worker-response.v1"));
	Response->SetBoolField(TEXT("ok"), false);
	Response->SetObjectField(TEXT("error"), Error);
	Response->SetObjectField(
		TEXT("meta"), MakeResponseMeta(RequestId, StartedSeconds));
	return Response;
}

FString ReadTracePath(const TSharedPtr<FJsonObject>& Params)
{
	FString Path = GetString(Params, TEXT("tracePath"));
	if (Path.IsEmpty())
	{
		Path = GetString(Params, TEXT("path"));
	}
	if (Path.IsEmpty())
	{
		Path = GetString(Params, TEXT("file"));
	}
	return Path.IsEmpty() ? Path : FPaths::ConvertRelativePathToFull(Path);
}

bool ParseCursor(
	const TSharedPtr<FJsonObject>& Params,
	uint64& OutCursor)
{
	OutCursor = 0;
	if (!Params->HasField(TEXT("cursor")))
	{
		return true;
	}
	FString CursorString;
	if (!Params->TryGetStringField(TEXT("cursor"), CursorString)
		|| CursorString.IsEmpty()
		|| CursorString.Len() > 20)
	{
		return false;
	}
	for (const TCHAR Character : CursorString)
	{
		if (Character < TEXT('0') || Character > TEXT('9'))
		{
			return false;
		}
		const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
		const uint64 Maximum = TNumericLimits<uint64>::Max();
		if (OutCursor > (Maximum - Digit) / 10u)
		{
			OutCursor = 0;
			return false;
		}
		OutCursor = OutCursor * 10u + Digit;
	}
	return true;
}

bool MapCapabilityToQuery(
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Params,
	FTraceQueryRequest& OutRequest)
{
	if (Capability == TEXT("production.trace.export"))
	{
		OutRequest.Provider = GetString(Params, TEXT("provider"));
		OutRequest.Operation = GetString(Params, TEXT("operation"));
		if (OutRequest.Provider.IsEmpty() || OutRequest.Operation.IsEmpty())
		{
			return false;
		}
	}
	else if (Capability == TEXT("production.trace.timing.query"))
	{
		OutRequest.Provider = TEXT("timing");
		OutRequest.Operation = GetString(Params, TEXT("operation"), TEXT("timers"));
	}
	else if (Capability == TEXT("production.trace.counter.query"))
	{
		OutRequest.Provider = TEXT("counter");
		OutRequest.Operation = GetString(Params, TEXT("operation"), TEXT("list"));
	}
	else if (Capability == TEXT("production.trace.log.query"))
	{
		OutRequest.Provider = TEXT("log");
		OutRequest.Operation = GetString(
			Params, TEXT("operation"), TEXT("messages"));
	}
	else if (Capability == TEXT("production.trace.bookmark.query"))
	{
		OutRequest.Provider = TEXT("bookmark");
		OutRequest.Operation = GetString(
			Params, TEXT("operation"), TEXT("list"));
	}
	else if (Capability == TEXT("production.trace.region.query"))
	{
		OutRequest.Provider = TEXT("region");
		OutRequest.Operation = GetString(
			Params, TEXT("operation"), TEXT("ranges"));
	}
	else if (Capability == TEXT("production.trace.thread.query"))
	{
		OutRequest.Provider = TEXT("threads");
		OutRequest.Operation = TEXT("list");
	}
	else
	{
		const TMap<FString, FString> KnownProviders = {
			{TEXT("production.trace.memory.query"), TEXT("memory")},
			{TEXT("production.trace.loading.query"), TEXT("loading")},
			{TEXT("production.trace.network.query"), TEXT("network")},
			{TEXT("production.trace.tasks.query"), TEXT("tasks")},
			{TEXT("production.trace.context_switches.query"), TEXT("contextSwitches")},
			{TEXT("production.trace.io.query"), TEXT("io")},
			{TEXT("production.trace.screenshot.query"), TEXT("screenshot")}
		};
		const FString* Provider = KnownProviders.Find(Capability);
		if (!Provider)
		{
			return false;
		}
		OutRequest.Provider = *Provider;
		OutRequest.Operation = GetString(Params, TEXT("operation"), TEXT("list"));
	}

	OutRequest.Filter = GetString(Params, TEXT("filter"));
	OutRequest.Page.Limit = FMath::Clamp(
		static_cast<int32>(GetNumber(Params, TEXT("limit"), DefaultPageLimit)),
		1,
		MaximumPageLimit);
	if (!ParseCursor(Params, OutRequest.Page.Cursor))
	{
		return false;
	}
	if (Params->HasField(TEXT("startTimeSeconds")))
	{
		OutRequest.TimeRange.bHasStart = true;
		OutRequest.TimeRange.StartSeconds =
			GetNumber(Params, TEXT("startTimeSeconds"), 0.0);
	}
	if (Params->HasField(TEXT("endTimeSeconds")))
	{
		OutRequest.TimeRange.bHasEnd = true;
		OutRequest.TimeRange.EndSeconds =
			GetNumber(Params, TEXT("endTimeSeconds"), 0.0);
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Params->Values)
	{
		if (!Pair.Value.IsValid())
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
			OutRequest.Options.Add(Pair.Key, LexToString(Pair.Value->AsNumber()));
		}
		else if (Pair.Key == TEXT("names")
			&& Pair.Value->Type == EJson::Array)
		{
			FString SerializedNames;
			const TSharedRef<
				TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<
						TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(
							&SerializedNames);
			if (FJsonSerializer::Serialize(Pair.Value->AsArray(), Writer))
			{
				OutRequest.Options.Add(Pair.Key, MoveTemp(SerializedNames));
			}
		}
	}
	return true;
}

TSharedPtr<FJsonObject> BuildProviderData(
	FTraceAnalysisSession& Session,
	const FTraceRecord& Record,
	const FString& AnalysisId)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), TEXT("ue.trace-provider-list.v1"));
	Data->SetStringField(TEXT("traceId"), Record.TraceId);
	Data->SetStringField(TEXT("analysisId"), AnalysisId);
	Data->SetStringField(TEXT("tracePath"), Session.GetTracePath());
	Data->SetStringField(TEXT("sha256"), Record.Sha256);
	Data->SetNumberField(TEXT("durationSeconds"), Session.GetDurationSeconds());
	Data->SetStringField(
		TEXT("recordedBuildVersion"), Session.GetRecordedBuildVersion());
	Data->SetStringField(
		TEXT("engineVersionStatus"), Session.GetEngineVersionStatus());
	Data->SetStringField(
		TEXT("managedEngineMarker"), Session.GetManagedEngineMarker());
	TArray<TSharedPtr<FJsonValue>> Providers;
	for (const FTraceProviderStatus& Status : Session.GetProviderStatuses())
	{
		Providers.Add(MakeShared<FJsonValueObject>(ProviderStatusToJson(Status)));
	}
	Data->SetArrayField(TEXT("providers"), Providers);
	Data->SetNumberField(TEXT("total"), Providers.Num());
	return Data;
}

FTraceDiagnostic ApplyManagedRegionDefault(
	FTraceAnalysisSession& Session,
	const FTraceRecord& Record,
	FTraceQueryRequest& Request)
{
	FTraceDiagnostic Diagnostic;
	if (Request.TimeRange.bHasStart || Request.TimeRange.bHasEnd
		|| Request.Provider == TEXT("region"))
	{
		return Diagnostic;
	}
	FTraceQueryRequest RegionRequest;
	RegionRequest.Provider = TEXT("region");
	RegionRequest.Operation = TEXT("ranges");
	RegionRequest.Filter = TEXT("UEAI.Trace.");
	RegionRequest.TimeRange.bHasStart = true;
	RegionRequest.TimeRange.StartSeconds = 0.0;
	RegionRequest.TimeRange.bHasEnd = true;
	RegionRequest.TimeRange.EndSeconds = Session.GetDurationSeconds();
	RegionRequest.Page.Limit = MaximumPageLimit;
	FTraceQueryResult Regions;
	FString ErrorCode;
	FString ErrorMessage;
	if (!Session.Query(
			RegionRequest, Regions, ErrorCode, ErrorMessage))
	{
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Code = TEXT("managed_region_unavailable");
		Diagnostic.Message = TEXT(
			"No managed UEAI.Trace region was available; the bounded whole-trace default is used.");
		return Diagnostic;
	}
	const FString SourceJobId = FPaths::GetCleanFilename(
		FPaths::GetPath(Record.SourcePath));
	const FString Expected = SourceJobId.StartsWith(TEXT("trace-launch-local-"))
		? TEXT("UEAI.Trace.") + SourceJobId
		: FString();
	const FTraceRow* Selected = nullptr;
	int32 CandidateCount = 0;
	for (const FTraceRow& Row : Regions.Rows)
	{
		const FTraceValue* Name = Row.Fields.Find(TEXT("name"));
		const FTraceValue* Begin = Row.Fields.Find(TEXT("beginSeconds"));
		const FTraceValue* End = Row.Fields.Find(TEXT("endSeconds"));
		if (!Name || Name->Type != ETraceValueType::String
			|| !Begin || Begin->Type != ETraceValueType::Number
			|| !End || End->Type != ETraceValueType::Number
			|| !Name->StringValue.StartsWith(TEXT("UEAI.Trace.")))
		{
			continue;
		}
		++CandidateCount;
		if (!Expected.IsEmpty() && Name->StringValue == Expected)
		{
			Selected = &Row;
			break;
		}
		if (CandidateCount == 1)
		{
			Selected = &Row;
		}
		else
		{
			Selected = nullptr;
		}
	}
	if (Selected)
	{
		Request.TimeRange.bHasStart = true;
		Request.TimeRange.StartSeconds =
			Selected->Fields.FindChecked(TEXT("beginSeconds")).NumberValue;
		Request.TimeRange.bHasEnd = true;
		Request.TimeRange.EndSeconds =
			Selected->Fields.FindChecked(TEXT("endSeconds")).NumberValue;
		Diagnostic.Severity = TEXT("info");
		Diagnostic.Code = TEXT("managed_region_applied");
		Diagnostic.Message = TEXT(
			"The query defaulted to the managed UEAI.Trace begin/end region.");
	}
	else
	{
		Diagnostic.Severity = TEXT("warning");
		Diagnostic.Code = CandidateCount > 1
			? TEXT("managed_region_ambiguous")
			: TEXT("managed_region_unavailable");
		Diagnostic.Message = CandidateCount > 1
			? TEXT("Multiple managed UEAI.Trace regions were found; the bounded whole-trace default is used.")
			: TEXT("No managed UEAI.Trace region was found; the bounded whole-trace default is used.");
	}
	return Diagnostic;
}
}

TSharedPtr<FJsonObject> FProtocol::HandleRequest(
	const TSharedPtr<FJsonObject>& Request,
	const FString& CommandLine)
{
	const double StartedSeconds = FPlatformTime::Seconds();
	const FString RequestId = GetString(Request, TEXT("requestId"));
	if (GetString(Request, TEXT("schema")) != TEXT("ue.trace-worker-request.v1"))
	{
		return Failure(
			RequestId,
			StartedSeconds,
			TEXT("trace_worker_protocol_mismatch"),
			TEXT("The request schema must be ue.trace-worker-request.v1."));
	}
	if (RequestId.IsEmpty() || RequestId.Len() > MaximumRequestIdLength)
	{
		return Failure(
			RequestId,
			StartedSeconds,
			TEXT("trace_worker_request_invalid"),
			TEXT("requestId is required and must be at most 128 characters."));
	}
	const FString Action = GetString(Request, TEXT("action"));
	if (Action == TEXT("handshake"))
	{
		return HandleHandshake(RequestId, CommandLine);
	}
	if (Action != TEXT("execute"))
	{
		return Failure(
			RequestId,
			StartedSeconds,
			TEXT("trace_worker_action_unsupported"),
			TEXT("The worker supports only handshake and execute actions."));
	}
	const TSharedPtr<FJsonObject>* ParamsField = nullptr;
	const TSharedPtr<FJsonObject> Params =
		Request->TryGetObjectField(TEXT("params"), ParamsField)
			&& ParamsField && ParamsField->IsValid()
		? *ParamsField
		: MakeShared<FJsonObject>();
	return HandleExecute(
		RequestId,
		GetString(Request, TEXT("capability")),
		Params,
		CommandLine);
}

void FProtocol::ShutdownAnalysisCache()
{
	FScopeLock Lock(&AnalysisSessionCacheMutex);
	AnalysisSessionCache.Reset();
	AnalysisSessionCacheClock = 0;
}

TSharedPtr<FJsonObject> FProtocol::HandleHandshake(
	const FString& RequestId,
	const FString& CommandLine)
{
	const double StartedSeconds = FPlatformTime::Seconds();
	FTraceWorkerHandshake Handshake;
	Handshake.WorkerVersion = TEXT(UEAI_TRACE_WORKER_VERSION);
	Handshake.EngineVersion = FEngineVersion::Current().ToString();
	Handshake.ContractDigest = TEXT(UEAI_TRACE_CONTRACT_DIGEST);
	Handshake.ProviderSchemaDigest = TEXT(UEAI_TRACE_PROVIDER_SCHEMA_DIGEST);
	Handshake.Providers = FTraceAnalysisSession::GetProviderDescriptors();
	const bool bServe = HasCommandLineSwitch(*CommandLine, TEXT("serve"));
	int32 MaximumSessions = 2;
	ReadCommandLineValue(*CommandLine, TEXT("maxSessions="), MaximumSessions);
	Handshake.MaximumResidentSessions = bServe
		? FMath::Clamp(MaximumSessions, 1, 2)
		: 1;
#if PLATFORM_WINDOWS
	Handshake.Transport = bServe ? TEXT("named-pipe") : TEXT("stdio-one-shot");
#else
	Handshake.Transport = bServe ? TEXT("unix-socket") : TEXT("stdio-one-shot");
#endif
	const FUnrealInsightsResolution Insights =
		ResolveUnrealInsights(CommandLine);
	Handshake.UnrealInsightsPath = Insights.Path;
	Handshake.bUnrealInsightsAvailable = !Handshake.UnrealInsightsPath.IsEmpty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("schema"), Handshake.Schema);
	Data->SetNumberField(TEXT("protocolVersion"), Handshake.Protocol);
	Data->SetStringField(TEXT("workerVersion"), Handshake.WorkerVersion);
	Data->SetStringField(TEXT("engineVersion"), Handshake.EngineVersion);
	Data->SetStringField(TEXT("contractDigest"), Handshake.ContractDigest);
	Data->SetBoolField(
		TEXT("contractBound"), Handshake.ContractDigest != TEXT("unbound"));
	Data->SetStringField(
		TEXT("providerSchemaDigest"), Handshake.ProviderSchemaDigest);
	Data->SetStringField(TEXT("transport"), Handshake.Transport);
	Data->SetNumberField(
		TEXT("maximumResidentSessions"), Handshake.MaximumResidentSessions);
	// Compatibility field above means concurrent resident transport sessions,
	// not retained TraceServices analyses. State both limits explicitly so
	// clients cannot mistake connection capacity for an analysis cache.
	Data->SetStringField(TEXT("residentSessionKind"), TEXT("connection"));
	Data->SetNumberField(
		TEXT("maximumConcurrentConnections"),
		Handshake.MaximumResidentSessions);
	Data->SetNumberField(TEXT("maximumConcurrentAnalyses"), 1);
	Data->SetNumberField(
		TEXT("analysisSessionCacheCapacity"), AnalysisSessionCacheCapacity);
	Data->SetStringField(
		TEXT("analysisSessionPolicy"), TEXT("sha256-lru"));
	Data->SetStringField(
		TEXT("unrealInsightsPath"), Handshake.UnrealInsightsPath);
	Data->SetBoolField(
		TEXT("unrealInsightsAvailable"), Handshake.bUnrealInsightsAvailable);
	Data->SetStringField(
		TEXT("unrealInsightsEngineDir"), Insights.EngineDirectory);
	Data->SetStringField(
		TEXT("unrealInsightsEngineVersion"), Insights.EngineVersion);
	Data->SetStringField(
		TEXT("unrealInsightsSource"), Insights.Source);
	Data->SetStringField(
		TEXT("unrealInsightsUnavailableReason"), Insights.UnavailableReason);
	FString Endpoint;
	ReadCommandLineValue(*CommandLine, TEXT("endpoint="), Endpoint);
	Data->SetStringField(TEXT("endpoint"), bServe ? Endpoint : FString());
	const FTraceStore Store(CommandLine);
	Data->SetStringField(TEXT("storeRoot"), Store.GetRoot());
	TArray<TSharedPtr<FJsonValue>> Providers;
	for (const FTraceProviderDescriptor& Descriptor : Handshake.Providers)
	{
		Providers.Add(MakeShared<FJsonValueObject>(
			ProviderDescriptorToJson(Descriptor)));
	}
	Data->SetArrayField(TEXT("providers"), Providers);
	return Success(RequestId, StartedSeconds, Data);
}

TSharedPtr<FJsonObject> FProtocol::HandleExecute(
	const FString& RequestId,
	const FString& Capability,
	const TSharedPtr<FJsonObject>& Params,
	const FString& CommandLine)
{
	const double StartedSeconds = FPlatformTime::Seconds();
	if (Capability.IsEmpty())
	{
		return Failure(
			RequestId, StartedSeconds, TEXT("trace_worker_request_invalid"),
			TEXT("execute requires a capability."));
	}
	FTraceStore Store(CommandLine);
	FString ErrorCode;
	FString ErrorMessage;
	FTraceWorkerLaunch Launch(CommandLine, Store);
	if (Launch.Handles(Capability))
	{
		TSharedPtr<FJsonObject> Data;
		if (!Launch.Execute(
				Capability,
				Params,
				RequestId,
				Data,
				ErrorCode,
				ErrorMessage))
		{
			return Failure(
				RequestId, StartedSeconds, ErrorCode, ErrorMessage);
		}
		return Success(RequestId, StartedSeconds, Data);
	}
	if (Capability == TEXT("production.trace.import"))
	{
		const FString SourcePath = ReadTracePath(Params);
		if (SourcePath.IsEmpty())
		{
			return Failure(
				RequestId, StartedSeconds, TEXT("trace_path_required"),
				TEXT("production.trace.import requires tracePath."));
		}
		FTraceRecord Imported;
		if (!Store.Import(
				SourcePath,
				GetString(Params, TEXT("copyMode"), TEXT("copy")),
				Imported,
				ErrorCode,
				ErrorMessage))
		{
			return Failure(
				RequestId, StartedSeconds, ErrorCode, ErrorMessage);
		}
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("schema"), TEXT("ue.trace-import.v1"));
		Data->SetStringField(TEXT("traceId"), Imported.TraceId);
		Data->SetStringField(TEXT("sha256"), Imported.Sha256);
		Data->SetNumberField(
			TEXT("sizeBytes"), static_cast<double>(Imported.SizeBytes));
		Data->SetStringField(TEXT("copyMode"), Imported.CopyMode);
		Data->SetStringField(TEXT("importedAtUtc"), Imported.ImportedAtUtc);
		Data->SetStringField(TEXT("storeRoot"), Store.GetRoot());
		return Success(RequestId, StartedSeconds, Data);
	}

	const FString TraceId = GetString(Params, TEXT("traceId"));
	if (TraceId.IsEmpty())
	{
		return Failure(
			RequestId, StartedSeconds, TEXT("trace_id_required"),
			TEXT("Registered local Trace operations require traceId."));
	}
	FTraceRecord Record;
	if (!Store.Resolve(
			TraceId, Record, ErrorCode, ErrorMessage))
	{
		return Failure(
			RequestId, StartedSeconds, ErrorCode, ErrorMessage);
	}

	if (Capability == TEXT("production.trace.open_in_insights"))
	{
		const FUnrealInsightsResolution Insights =
			ResolveUnrealInsights(CommandLine);
		if (Insights.Path.IsEmpty())
		{
			return Failure(
				RequestId, StartedSeconds, TEXT("unreal_insights_unavailable"),
				Insights.UnavailableReason.IsEmpty()
					? TEXT("UnrealInsights could not be resolved for this Engine version.")
					: Insights.UnavailableReason);
		}
		const FString RequestedView = GetString(Params, TEXT("view"));
		const bool bHasStart = Params->HasField(TEXT("startTimeSeconds"));
		const bool bHasEnd = Params->HasField(TEXT("endTimeSeconds"));
		const double RequestedStart = GetNumber(
			Params, TEXT("startTimeSeconds"), 0.0);
		const double RequestedEnd = GetNumber(
			Params, TEXT("endTimeSeconds"), 0.0);
		const FString Arguments = FString::Printf(
			TEXT("-OpenTraceFile=\"%s\""), *Record.TracePath);
		uint32 ProcessId = 0;
		FProcHandle Process = FPlatformProcess::CreateProc(
			*Insights.Path,
			*Arguments,
			true,
			false,
			false,
			&ProcessId,
			0,
			*FPaths::GetPath(Insights.Path),
			nullptr,
			nullptr);
		if (!Process.IsValid())
		{
			return Failure(
				RequestId, StartedSeconds, TEXT("unreal_insights_launch_failed"),
				TEXT("UnrealInsights could not be launched."));
		}
		FPlatformProcess::CloseProc(Process);
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("traceId"), Record.TraceId);
		Data->SetStringField(TEXT("tracePath"), Record.TracePath);
		Data->SetStringField(TEXT("unrealInsightsPath"), Insights.Path);
		Data->SetStringField(
			TEXT("unrealInsightsEngineDir"), Insights.EngineDirectory);
		Data->SetStringField(
			TEXT("unrealInsightsEngineVersion"), Insights.EngineVersion);
		Data->SetStringField(
			TEXT("unrealInsightsSource"), Insights.Source);
		Data->SetNumberField(TEXT("processId"), ProcessId);
		Data->SetStringField(TEXT("requestedView"), RequestedView);
		if (bHasStart)
		{
			Data->SetNumberField(TEXT("requestedStartTimeSeconds"), RequestedStart);
		}
		if (bHasEnd)
		{
			Data->SetNumberField(TEXT("requestedEndTimeSeconds"), RequestedEnd);
		}
		Data->SetBoolField(TEXT("viewApplied"), false);
		Data->SetStringField(
			TEXT("viewApplyReason"),
			RequestedView.IsEmpty() && !bHasStart && !bHasEnd
				? TEXT("No view or time range was requested; the trace was opened with the matching Unreal Insights executable.")
				: TEXT("UE 5.3 exposes no stable command-line contract for applying this view/time range; the trace was opened without UI automation."));
		return Success(RequestId, StartedSeconds, Data);
	}

	const TSharedPtr<FTraceAnalysisSession> CachedSession =
		AcquireAnalysisSession(
			Record,
			CommandLine,
			GetNumber(Params, TEXT("timeoutSeconds"), 120.0),
			ErrorCode,
			ErrorMessage);
	if (!CachedSession.IsValid())
	{
		return Failure(
			RequestId, StartedSeconds, ErrorCode, ErrorMessage);
	}
	FTraceAnalysisSession& Session = *CachedSession;
	const FString AnalysisId = Store.MakeAnalysisId();
	if (Capability == TEXT("production.trace.provider.list")
		|| Capability == TEXT("production.trace.analyze"))
	{
		TSharedPtr<FJsonObject> Data =
			BuildProviderData(Session, Record, AnalysisId);
		if (!Store.PersistAnalysisJob(
				AnalysisId, Record.TraceId, Capability, Data, ErrorMessage))
		{
			return Failure(
				RequestId,
				StartedSeconds,
				TEXT("trace_analysis_record_failed"),
				ErrorMessage);
		}
		return Success(RequestId, StartedSeconds, Data);
	}

	FTraceQueryRequest QueryRequest;
	uint64 ParsedCursor = 0;
	if (!ParseCursor(Params, ParsedCursor))
	{
		return Failure(
			RequestId,
			StartedSeconds,
			TEXT("trace_query_invalid"),
			TEXT("cursor must be a decimal uint64 string."));
	}
	if (!MapCapabilityToQuery(Capability, Params, QueryRequest))
	{
		return Failure(
			RequestId, StartedSeconds, TEXT("trace_query_unsupported"),
			FString::Printf(
				TEXT("Capability '%s' is not implemented by this worker."),
				*Capability));
	}
	if (QueryRequest.Provider == TEXT("timing")
		&& QueryRequest.Operation == TEXT("threads"))
	{
		QueryRequest.Provider = TEXT("threads");
		QueryRequest.Operation = TEXT("list");
	}
	FString ExportFormat;
	int64 ExportMaximumBytes = DefaultMaximumExportBytes;
	int64 ExportMaximumCellBytes = DefaultMaximumExportCellBytes;
	if (Capability == TEXT("production.trace.export"))
	{
		ExportFormat = GetString(Params, TEXT("format"));
		if (ExportFormat != TEXT("json") && ExportFormat != TEXT("csv"))
		{
			return Failure(
				RequestId, StartedSeconds, TEXT("trace_export_invalid"),
				TEXT("Trace export format must be json or csv."));
		}
		ExportMaximumBytes = static_cast<int64>(FMath::Clamp(
			GetNumber(
				Params,
				TEXT("maxBytes"),
				static_cast<double>(DefaultMaximumExportBytes)),
			1024.0,
			static_cast<double>(DefaultMaximumExportBytes)));
		ExportMaximumCellBytes = static_cast<int64>(FMath::Clamp(
			GetNumber(
				Params,
				TEXT("maxCellBytes"),
				static_cast<double>(DefaultMaximumExportCellBytes)),
			256.0,
			static_cast<double>(DefaultMaximumExportCellBytes)));
	}
	const FTraceDiagnostic RegionDiagnostic =
		ApplyManagedRegionDefault(Session, Record, QueryRequest);
	FTraceQueryResult QueryResult;
	if (Capability == TEXT("production.trace.export"))
	{
		const int32 RequestedLimit = FMath::Clamp(
			static_cast<int32>(GetNumber(Params, TEXT("limit"), 1000.0)),
			1,
			100000);
		uint64 Cursor = QueryRequest.Page.Cursor;
		bool bFirstPage = true;
		bool bHasMore = true;
		int64 EstimatedExportBytes = ExportFormat == TEXT("json") ? 2 : 0;
		while (QueryResult.Rows.Num() < RequestedLimit && bHasMore)
		{
			FTraceQueryRequest PageRequest = QueryRequest;
			PageRequest.Page.Cursor = Cursor;
			PageRequest.Page.Limit = FMath::Min(
				MaximumPageLimit,
				RequestedLimit - QueryResult.Rows.Num());
			FTraceQueryResult Page;
			if (!Session.Query(
					PageRequest, Page, ErrorCode, ErrorMessage))
			{
				return Failure(
					RequestId, StartedSeconds, ErrorCode, ErrorMessage);
			}
			if (Page.Rows.Num() > PageRequest.Page.Limit)
			{
				return Failure(
					RequestId,
					StartedSeconds,
					TEXT("trace_export_page_invalid"),
					TEXT("A TraceServices query page exceeded its requested bound."));
			}
			if (!AccountExportPage(
					Page,
					ExportFormat,
					bFirstPage,
					ExportMaximumBytes,
					ExportMaximumCellBytes,
					EstimatedExportBytes,
					ErrorCode,
					ErrorMessage))
			{
				return Failure(
					RequestId,
					StartedSeconds,
					ErrorCode,
					ErrorMessage);
			}
			if (bFirstPage)
			{
				QueryResult = Page;
				bFirstPage = false;
			}
			else
			{
				if (Page.Provider != QueryResult.Provider
					|| Page.Operation != QueryResult.Operation
					|| Page.Columns != QueryResult.Columns)
				{
					return Failure(
						RequestId,
						StartedSeconds,
						TEXT("trace_export_page_inconsistent"),
						TEXT("TraceServices returned inconsistent export pages."));
				}
				QueryResult.TotalRows = FMath::Max(
					QueryResult.TotalRows, Page.TotalRows);
				QueryResult.Diagnostics.Append(Page.Diagnostics);
				QueryResult.Rows.Append(Page.Rows);
				QueryResult.bTruncated = Page.bTruncated;
				QueryResult.bHasNextCursor = Page.bHasNextCursor;
				QueryResult.NextCursor = Page.NextCursor;
			}
			bHasMore = Page.bHasNextCursor;
			if (bHasMore)
			{
				if (Page.NextCursor <= Cursor)
				{
					return Failure(
						RequestId,
						StartedSeconds,
						TEXT("trace_export_cursor_stalled"),
						TEXT("TraceServices returned a non-progressing export cursor."));
				}
				Cursor = Page.NextCursor;
			}
		}
		QueryResult.bHasNextCursor = bHasMore;
		QueryResult.NextCursor = bHasMore ? Cursor : 0;
		QueryResult.bTruncated = bHasMore
			|| QueryResult.Rows.Num()
				< static_cast<int64>(QueryResult.TotalRows);
	}
	else if (!Session.Query(
			QueryRequest,
			QueryResult,
			ErrorCode,
			ErrorMessage))
	{
		return Failure(
			RequestId, StartedSeconds, ErrorCode, ErrorMessage);
	}
	if (!RegionDiagnostic.Code.IsEmpty())
	{
		QueryResult.Diagnostics.Add(RegionDiagnostic);
	}
	if (Capability == TEXT("production.trace.export"))
	{
		const FString ExportPath = Store.MakeExportPath(
			Record.TraceId, QueryRequest.Provider, ExportFormat);
		int64 ExportSizeBytes = 0;
		if (!WriteExportAtomic(
				ExportPath,
				QueryResult,
				ExportFormat,
				ExportMaximumBytes,
				ExportMaximumCellBytes,
				ExportSizeBytes,
				ErrorCode,
				ErrorMessage))
		{
			return Failure(
				RequestId,
				StartedSeconds,
				ErrorCode.IsEmpty() ? TEXT("trace_export_failed") : ErrorCode,
				ErrorMessage);
		}
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("traceId"), Record.TraceId);
		Data->SetStringField(TEXT("analysisId"), AnalysisId);
		Data->SetStringField(TEXT("provider"), QueryRequest.Provider);
		Data->SetStringField(TEXT("operation"), QueryRequest.Operation);
		Data->SetNumberField(TEXT("rowCount"), QueryResult.Rows.Num());
		Data->SetBoolField(TEXT("truncated"), QueryResult.bTruncated);
		TSharedPtr<FJsonObject> Artifact = MakeShared<FJsonObject>();
		Artifact->SetStringField(TEXT("kind"), TEXT("traceExport"));
		Artifact->SetStringField(TEXT("path"), ExportPath);
		Artifact->SetStringField(TEXT("format"), ExportFormat);
		Artifact->SetStringField(
			TEXT("mimeType"),
			ExportFormat == TEXT("json")
				? TEXT("application/json") : TEXT("text/csv"));
		Artifact->SetNumberField(
			TEXT("sizeBytes"), ExportSizeBytes);
		Data->SetObjectField(TEXT("artifact"), Artifact);
		if (!Store.PersistAnalysisJob(
				AnalysisId, Record.TraceId, Capability, Data, ErrorMessage))
		{
			return Failure(
				RequestId,
				StartedSeconds,
				TEXT("trace_analysis_record_failed"),
				ErrorMessage);
		}
		return Success(RequestId, StartedSeconds, Data);
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("traceId"), Record.TraceId);
	Data->SetStringField(TEXT("analysisId"), AnalysisId);
	Data->SetStringField(TEXT("tracePath"), Session.GetTracePath());
	Data->SetNumberField(TEXT("durationSeconds"), Session.GetDurationSeconds());
	Data->SetStringField(
		TEXT("recordedBuildVersion"), Session.GetRecordedBuildVersion());
	Data->SetStringField(
		TEXT("engineVersionStatus"), Session.GetEngineVersionStatus());
	Data->SetStringField(
		TEXT("managedEngineMarker"), Session.GetManagedEngineMarker());
	Data->SetObjectField(TEXT("query"), QueryResultToJson(QueryResult));
	if (!Store.PersistAnalysisJob(
			AnalysisId, Record.TraceId, Capability, Data, ErrorMessage))
	{
		return Failure(
			RequestId,
			StartedSeconds,
			TEXT("trace_analysis_record_failed"),
			ErrorMessage);
	}
	return Success(RequestId, StartedSeconds, Data);
}
}
