#if WITH_DEV_AUTOMATION_TESTS

#include "Infrastructure/ReflectionInspectService.h"

#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

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
	const FString& Script,
	const TSharedPtr<FJsonObject>& Snapshot)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("script"), Script);
	Params->SetStringField(TEXT("modificationLevel"), TEXT("readOnly"));
	Params->SetObjectField(TEXT("input"), Snapshot);
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
	FFullPythonExecutionTest,
	"UE_AI_integration.Reflection.FullPythonExecution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFullPythonExecutionTest::RunTest(const FString& Parameters)
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
		MakeInspectParams(TEXT(
			"from __future__ import annotations\n"
			"import __main__\nfrom dataclasses import dataclass\n"
			"@dataclass\nclass Entry:\n    count: int\n"
			"def main():\n    global result\n"
			"    assert __main__.Entry is Entry\n"
			"    result = Entry(len(data['types']) + 2).count\n"
			"if __name__ == '__main__':\n    main()\n"), Snapshot));
	TestTrue(TEXT("Complete Python script succeeds"), Good.bSuccess);
	if (Good.bSuccess && Good.Data.IsValid())
	{
		TestEqual(
			TEXT("Complete Python script returns the expected value"),
			Good.Data->GetIntegerField(TEXT("result")),
			4);
		TestEqual(
			TEXT("Execution records its modification level"),
			Good.Data->GetStringField(TEXT("modificationLevel")),
			FString(TEXT("readOnly")));
		TestTrue(
			TEXT("Execution returns an audit artifact"),
			Good.Data->HasTypedField<EJson::String>(TEXT("auditPath")));
		TestFalse(TEXT("Legacy callers need not supply the optional acknowledgement"), Good.Data->GetBoolField(TEXT("confirmWriteProvided")));
		if (Good.Data->HasTypedField<EJson::String>(TEXT("auditPath")))
		{
			const FString AuditPath = Good.Data->GetStringField(TEXT("auditPath"));
			TestTrue(
				TEXT("Audit receipt does not expose an absolute local path"),
				FPaths::IsRelative(AuditPath));
			const FString AbsoluteAuditPath = FPaths::Combine(FPaths::ProjectDir(), AuditPath);
			TestTrue(
				TEXT("Audit artifact is written before completion"),
				IFileManager::Get().FileExists(*AbsoluteAuditPath));
		}
	}

	TSharedRef<FJsonObject> Confirmed = MakeInspectParams(TEXT("result = 7"), Snapshot);
	Confirmed->SetBoolField(TEXT("confirmWrite"), true);
	const FMCPToolResult ConfirmedResult = Service.Execute(TEXT("production.python.inspect"), Confirmed);
	if (TestTrue(TEXT("CLI acknowledgement is accepted by the host"), ConfirmedResult.bSuccess && ConfirmedResult.Data.IsValid()))
	{
		TestTrue(TEXT("The host records explicit acknowledgement"), ConfirmedResult.Data->GetBoolField(TEXT("confirmWriteProvided")));
		TestEqual(TEXT("Acknowledgement preserves the declared modification level"), ConfirmedResult.Data->GetStringField(TEXT("modificationLevel")), FString(TEXT("readOnly")));
	}

	for (const FString& Level : {
		TEXT("unknown"),
		TEXT("write"),
	})
	{
		TSharedRef<FJsonObject> Invalid = MakeInspectParams(TEXT("result = 1"), Snapshot);
		Invalid->SetStringField(TEXT("modificationLevel"), Level);
		const FMCPToolResult Rejected = Service.Execute(
			TEXT("production.python.inspect"),
			Invalid);
		TestFalse(
			*FString::Printf(TEXT("Invalid modification level is rejected: %s"), *Level),
			Rejected.bSuccess);
		TestEqual(
			TEXT("Invalid modification level uses the stable error code"),
			Rejected.ErrorCode,
			FString(TEXT("invalid_modification_level")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEnginePythonPlatformPathsTest,
	"UE_AI_integration.Reflection.EnginePythonPlatformPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEnginePythonPlatformPathsTest::RunTest(const FString& Parameters)
{
	using UEAIIntegration::Infrastructure::EnginePythonExecutableRelativePath;
	TestEqual(TEXT("Windows uses the Engine executable"), EnginePythonExecutableRelativePath(TEXT("Win64")), FString(TEXT("Binaries/ThirdParty/Python3/Win64/python.exe")));
	TestEqual(TEXT("Linux uses the Engine bin directory"), EnginePythonExecutableRelativePath(TEXT("Linux")), FString(TEXT("Binaries/ThirdParty/Python3/Linux/bin/python3")));
	TestEqual(TEXT("Mac uses the Engine bin directory"), EnginePythonExecutableRelativePath(TEXT("Mac")), FString(TEXT("Binaries/ThirdParty/Python3/Mac/bin/python3")));
	TestTrue(TEXT("Unknown platforms never fall back to Windows Python"), EnginePythonExecutableRelativePath(TEXT("Unknown")).IsEmpty());
	return true;
}

#endif
