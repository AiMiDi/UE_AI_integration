#pragma once

#include "TraceAnalysisContracts.h"
#include "Templates/SharedPointer.h"

namespace TraceServices
{
class IAnalysisSession;
}

namespace UEAI::Trace
{
/**
 * Bounded, UI-independent TraceServices facade shared by the Editor adapter
 * and UEAITraceWorker. The public contract intentionally contains no JSON,
 * Slate, UnrealEd, HTTP, or project UObject types.
 */
class UEAITRACEANALYSISCORE_API FTraceAnalysisSession
{
public:
	FTraceAnalysisSession();
	~FTraceAnalysisSession();

	FTraceAnalysisSession(const FTraceAnalysisSession&) = delete;
	FTraceAnalysisSession& operator=(const FTraceAnalysisSession&) = delete;

	bool Open(
		const FString& TracePath,
		double TimeoutSeconds,
		FString& OutErrorCode,
		FString& OutErrorMessage,
		bool bAllowUnknownEngineVersionForTestFixture = false);
	/** Attach an already completed same-process TraceServices session. */
	bool AttachCompletedSession(
		const FString& TracePath,
		TSharedPtr<const TraceServices::IAnalysisSession> CompletedSession,
		FString& OutErrorCode,
		FString& OutErrorMessage);
	void Close();

	bool IsOpen() const;
	const FString& GetTracePath() const;
	double GetDurationSeconds() const;
	const FString& GetRecordedBuildVersion() const;
	const FString& GetEngineVersionStatus() const;
	const FString& GetManagedEngineMarker() const;
	TArray<FTraceProviderStatus> GetProviderStatuses() const;

	bool Query(
		const FTraceQueryRequest& Request,
		FTraceQueryResult& OutResult,
		FString& OutErrorCode,
		FString& OutErrorMessage) const;

	static TArray<FTraceProviderDescriptor> GetProviderDescriptors();

private:
	bool InitializeCompletedSession(
		const FString& TracePath,
		bool bAllowUnknownEngineVersionForTestFixture,
		FString& OutErrorCode,
		FString& OutErrorMessage);

	TSharedPtr<const TraceServices::IAnalysisSession> Session;
	FString OpenTracePath;
	double DurationSeconds = 0.0;
	FString RecordedBuildVersion;
	FString EngineVersionStatus = TEXT("unknown");
	FString ManagedEngineMarker;
};
}
