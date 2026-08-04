#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "TraceAnalysisService.h"

namespace UEAI::Trace::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTraceSemanticProviderContractTest,
	"UE_AI_integration.Trace.Core.SemanticProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FTraceSemanticProviderContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TMap<FString, TArray<FString>> ExpectedOperations = {
		{TEXT("timing"), {TEXT("frames"), TEXT("threads"), TEXT("events"),
			TEXT("timers"), TEXT("callers"), TEXT("callees"), TEXT("cpuSampling")}},
		{TEXT("counter"), {TEXT("list"), TEXT("series"), TEXT("aggregate")}},
		{TEXT("memory"), {TEXT("allocations"), TEXT("liveAllocations"),
			TEXT("tags"), TEXT("modules"), TEXT("callstacks")}},
		{TEXT("loading"), {TEXT("packages"), TEXT("objects"), TEXT("exports"),
			TEXT("requests"), TEXT("dependencies")}},
		{TEXT("network"), {TEXT("connections"), TEXT("packets"),
			TEXT("contentEvents"), TEXT("stats")}},
		{TEXT("tasks"), {TEXT("tasks"), TEXT("relations"),
			TEXT("waiting"), TEXT("criticalPath")}},
		{TEXT("contextSwitches"), {TEXT("cores"), TEXT("threads"), TEXT("intervals")}},
		{TEXT("io"), {TEXT("files"), TEXT("events"), TEXT("aggregate")}},
		{TEXT("log"), {TEXT("categories"), TEXT("messages")}},
		{TEXT("bookmark"), {TEXT("list")}},
		{TEXT("region"), {TEXT("list"), TEXT("ranges")}},
		{TEXT("screenshot"), {TEXT("list"), TEXT("get")}}
	};

	const TArray<FTraceProviderDescriptor> Descriptors =
		FTraceAnalysisSession::GetProviderDescriptors();
	for (const TPair<FString, TArray<FString>>& Expected : ExpectedOperations)
	{
		const FTraceProviderDescriptor* Descriptor = Descriptors.FindByPredicate(
			[&Expected](const FTraceProviderDescriptor& Candidate)
			{
				return Candidate.Id == Expected.Key;
			});
		TestNotNull(*FString::Printf(TEXT("Provider %s is described"), *Expected.Key), Descriptor);
		if (!Descriptor)
		{
			continue;
		}
		TestTrue(
			*FString::Printf(TEXT("Provider %s enables its semantic adapter"), *Expected.Key),
			Descriptor->bQueryImplemented);
		TestTrue(
			*FString::Printf(TEXT("Provider %s operations match public contract"), *Expected.Key),
			Descriptor->Operations == Expected.Value);
	}

	FTraceProviderStatus EmptyStatus;
	TestEqual(TEXT("Channel history defaults to unknown"),
		EmptyStatus.ChannelStatus, FString(TEXT("unknown")));
	return true;
}
}

#endif
