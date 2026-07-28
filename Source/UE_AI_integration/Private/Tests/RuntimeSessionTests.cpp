#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/Runtime/RuntimeSceneService.h"
#include "Misc/AutomationTest.h"

using UEAIIntegration::Infrastructure::FRuntimeSceneService;
using UEAIIntegration::Infrastructure::FRuntimeServiceResult;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeSessionGenerationTest,
	"UE_AI_integration.Runtime.SessionGenerationInvalidatesHandles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeSessionGenerationTest::RunTest(const FString& Parameters)
{
	FRuntimeSceneService Service;
	TestFalse(TEXT("Session begins inactive"), Service.IsSessionActive());
	TestEqual(TEXT("Initial generation is zero"), Service.GetGeneration(), uint64(0));

	Service.PrepareNextSession();
	const FString FirstSessionId = Service.GetSessionId();
	const uint64 FirstGeneration = Service.GetGeneration();
	TestFalse(TEXT("Prepared session is not active"), Service.IsSessionActive());
	TestTrue(TEXT("Prepared session has an id"), !FirstSessionId.IsEmpty());
	TestEqual(TEXT("First generation is one"), FirstGeneration, uint64(1));

	Service.BeginSession();
	TestTrue(TEXT("Begin activates prepared session"), Service.IsSessionActive());
	TestEqual(TEXT("Begin reuses prepared id"), Service.GetSessionId(), FirstSessionId);
	TestEqual(TEXT("Begin does not increment twice"), Service.GetGeneration(), FirstGeneration);

	TSharedPtr<FJsonObject> OldRef = MakeShared<FJsonObject>();
	OldRef->SetStringField(TEXT("sessionId"), FirstSessionId);
	OldRef->SetNumberField(TEXT("generation"), static_cast<double>(FirstGeneration));
	OldRef->SetStringField(
		TEXT("objectId"),
		FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));

	Service.EndSession();
	UObject* ResolvedObject = nullptr;
	const FRuntimeServiceResult StoppedResult =
		Service.ResolveObjectRef(OldRef, ResolvedObject);
	TestFalse(TEXT("Stopped sessions do not resolve handles"), StoppedResult.bSuccess);
	TestEqual(
		TEXT("Stopped matching session reports pie_not_running"),
		StoppedResult.ErrorCode,
		FString(TEXT("pie_not_running")));
	TestEqual(TEXT("Stopped session status"), StoppedResult.HttpStatus, 409);

	Service.PrepareNextSession();
	TestNotEqual(
		TEXT("A new session gets a new id"),
		Service.GetSessionId(),
		FirstSessionId);
	TestEqual(
		TEXT("A new session increments generation"),
		Service.GetGeneration(),
		FirstGeneration + 1);

	const FRuntimeServiceResult StaleResult =
		Service.ResolveObjectRef(OldRef, ResolvedObject);
	TestFalse(TEXT("Old generation is stale"), StaleResult.bSuccess);
	TestEqual(
		TEXT("Old generation has stable error code"),
		StaleResult.ErrorCode,
		FString(TEXT("stale_session_handle")));
	TestEqual(TEXT("Old generation uses HTTP 410"), StaleResult.HttpStatus, 410);

	Service.BeginSession();
	Service.BeginSession();
	TestEqual(
		TEXT("Repeated BeginSession is idempotent"),
		Service.GetGeneration(),
		FirstGeneration + 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRuntimeWorldContextQueryTest,
	"UE_AI_integration.Runtime.WorldContextsAreQueryableWithoutPIE",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRuntimeWorldContextQueryTest::RunTest(const FString& Parameters)
{
	FRuntimeSceneService Service;
	const FRuntimeServiceResult Result =
		Service.ListWorldContexts(MakeShared<FJsonObject>());
	TestTrue(TEXT("World context query succeeds without PIE"), Result.bSuccess);
	TestTrue(TEXT("World context query returns data"), Result.Data.IsValid());
	if (Result.Data.IsValid())
	{
		TestTrue(TEXT("World context query returns worlds"), Result.Data->HasField(TEXT("worlds")));
		TestFalse(
			TEXT("World context query reports inactive session"),
			Result.Data->GetBoolField(TEXT("sessionActive")));
	}
	return true;
}

#endif
