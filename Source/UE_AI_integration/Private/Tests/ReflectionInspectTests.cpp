#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/ReflectionInspectService.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/AutomationTest.h"

namespace
{
using UEAIIntegration::Infrastructure::FReflectionInspectService;

TSharedRef<FJsonObject> MakeStringParams(
	const TCHAR* Field,
	const FString& Value)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(Field, Value);
	return Params;
}

TSharedRef<FJsonObject> MakeInspectParams(
	const FString& Expression,
	const TSharedPtr<FJsonObject>& Snapshot)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("expression"), Expression);
	Params->SetObjectField(TEXT("snapshot"), Snapshot);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FReflectionContractAndSnapshotTest,
	"UE_AI_integration.Reflection.ContractAndSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FReflectionContractAndSnapshotTest::RunTest(const FString& Parameters)
{
	FReflectionInspectService Service;
	const FString ActorType = AActor::StaticClass()->GetPathName();

	const FMCPToolResult TypeResult = Service.Execute(
		TEXT("production.reflection.type.get"),
		MakeStringParams(TEXT("type"), ActorType));
	TestTrue(TEXT("Actor reflection succeeds"), TypeResult.bSuccess);
	if (!TypeResult.bSuccess || !TypeResult.Data.IsValid())
	{
		AddError(TypeResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Actor reflection reports class kind"),
		TypeResult.Data->GetStringField(TEXT("kind")),
		FString(TEXT("class")));
	TestTrue(
		TEXT("Actor reflection exposes its superclass"),
		TypeResult.Data->HasTypedField<EJson::String>(TEXT("super")));
	TestTrue(
		TEXT("Actor reflection returns bounded functions"),
		TypeResult.Data->GetArrayField(TEXT("functions")).Num() <= 512);

	TSharedRef<FJsonObject> MemberParams = MakeShared<FJsonObject>();
	MemberParams->SetStringField(TEXT("type"), APawn::StaticClass()->GetPathName());
	MemberParams->SetStringField(TEXT("member"), TEXT("K2_SetActorLocation"));
	const FMCPToolResult MemberResult = Service.Execute(
		TEXT("production.reflection.member.get"),
		MemberParams);
	TestTrue(TEXT("Inherited function lookup succeeds"), MemberResult.bSuccess);
	if (MemberResult.bSuccess && MemberResult.Data.IsValid())
	{
		TestEqual(
			TEXT("Member result identifies the requested function"),
			MemberResult.Data->GetStringField(TEXT("name")),
			FString(TEXT("K2_SetActorLocation")));
		TestTrue(
			TEXT("Member lookup identifies the function as inherited"),
			MemberResult.Data->GetBoolField(TEXT("inherited")));
		TestEqual(
			TEXT("Member lookup reports the declaring class"),
			MemberResult.Data->GetStringField(TEXT("declaringType")),
			AActor::StaticClass()->GetPathName());
		TestTrue(
			TEXT("Function parameters are bounded"),
			MemberResult.Data->GetArrayField(TEXT("parameters")).Num() <= 512);
	}

	TSharedRef<FJsonObject> SnapshotParams = MakeShared<FJsonObject>();
	SnapshotParams->SetArrayField(
		TEXT("types"),
		{
			MakeShared<FJsonValueString>(ActorType),
			MakeShared<FJsonValueString>(TEXT("/Script/CoreUObject.Vector")),
		});
	const FMCPToolResult SnapshotResult = Service.Execute(
		TEXT("production.reflection.snapshot.create"),
		SnapshotParams);
	TestTrue(TEXT("Reflection snapshot succeeds"), SnapshotResult.bSuccess);
	if (!SnapshotResult.bSuccess || !SnapshotResult.Data.IsValid())
	{
		AddError(SnapshotResult.ErrorMessage);
		return false;
	}
	TestEqual(
		TEXT("Snapshot schema is immutable contract v1"),
		SnapshotResult.Data->GetStringField(TEXT("schema")),
		FString(TEXT("ue.reflection-snapshot.v1")));
	TestTrue(
		TEXT("Snapshot artifact exists"),
		IFileManager::Get().FileExists(
			*SnapshotResult.Data->GetStringField(TEXT("path"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRestrictedPythonInspectTest,
	"UE_AI_integration.Reflection.RestrictedPython",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRestrictedPythonInspectTest::RunTest(const FString& Parameters)
{
	FReflectionInspectService Service;
	TSharedRef<FJsonObject> Snapshot = MakeShared<FJsonObject>();
	Snapshot->SetStringField(TEXT("schema"), TEXT("ue.reflection-snapshot.v1"));
	Snapshot->SetArrayField(
		TEXT("types"),
		{
			MakeShared<FJsonValueString>(TEXT("Actor")),
			MakeShared<FJsonValueString>(TEXT("Vector")),
		});

	const FMCPToolResult Good = Service.Execute(
		TEXT("production.python.inspect"),
		MakeInspectParams(TEXT("len(data['types'])"), Snapshot));
	TestTrue(TEXT("Pure JSON expression succeeds"), Good.bSuccess);
	if (Good.bSuccess && Good.Data.IsValid())
	{
		TestEqual(
			TEXT("Pure JSON expression returns the expected value"),
			Good.Data->GetIntegerField(TEXT("result")),
			2);
	}

	for (const FString& Expression : {
		TEXT("__import__('os')"),
		TEXT("data.__class__"),
		TEXT("open('forbidden.txt')"),
		TEXT("'x' * 200000"),
		TEXT("[x for x in data['types'] if x.__class__]"),
	})
	{
		const FMCPToolResult Rejected = Service.Execute(
			TEXT("production.python.inspect"),
			MakeInspectParams(Expression, Snapshot));
		TestFalse(
			*FString::Printf(TEXT("Expression is rejected: %s"), *Expression),
			Rejected.bSuccess);
		TestEqual(
			TEXT("Rejected expression uses the stable error code"),
			Rejected.ErrorCode,
			FString(TEXT("python_expression_rejected")));
	}
	return true;
}

#endif
