#if WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraSystem.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterNiagaraTools(FMCPToolRegistry& Registry);
}

namespace
{
// Deliberately omit the factory/compiler: these tests inspect registry metadata
// and an uncompiled asset. Missing compiled data must never become success.
UNiagaraSystem* CreateNiagaraQueryFixture(const FString& PackageName)
{
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return nullptr;
	}
	const FString Name = FPackageName::GetLongPackageAssetName(PackageName);
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(
		Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	if (System)
	{
		FAssetRegistryModule::AssetCreated(System);
		Package->SetDirtyFlag(false);
	}
	return System;
}

void ReleaseNiagaraQueryFixture(UNiagaraSystem* System)
{
	if (!System)
	{
		return;
	}
	FAssetRegistryModule::AssetDeleted(System);
	System->GetOutermost()->SetDirtyFlag(false);
	System->ClearFlags(RF_Public | RF_Standalone);
	System->MarkAsGarbage();
}

void RegisterNiagaraQueryTools(FMCPToolRegistry& Registry)
{
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterNiagaraTools(Registry);
	Registry.EndDomainRegistration();
}

TSharedRef<FJsonObject> NiagaraPageParams(const FString& Filter, int32 Offset, int32 Limit)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("filter"), Filter);
	Params->SetNumberField(TEXT("offset"), Offset);
	Params->SetNumberField(TEXT("limit"), Limit);
	return Params;
}

TSharedRef<FJsonObject> NiagaraTargetParams(const UNiagaraSystem* System)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("system"), System->GetPathName());
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraAuthoringListPaginationTest,
	"UE_AI_integration.Niagara.Authoring.ListPagination",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraAuthoringListPaginationTest::RunTest(const FString&)
{
	const FString Folder = TEXT("/Game/Automation/UEAI_NiagaraQuery_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	// Registration order differs from lexical order. The filter only occurs in
	// the folder, so this also verifies package-path matching.
	UNiagaraSystem* Third = CreateNiagaraQueryFixture(Folder + TEXT("/Third"));
	UNiagaraSystem* First = CreateNiagaraQueryFixture(Folder + TEXT("/First"));
	UNiagaraSystem* Second = CreateNiagaraQueryFixture(Folder + TEXT("/Second"));
	ON_SCOPE_EXIT
	{
		ReleaseNiagaraQueryFixture(First);
		ReleaseNiagaraQueryFixture(Second);
		ReleaseNiagaraQueryFixture(Third);
	};
	if (!TestNotNull(TEXT("First list fixture"), First)
		|| !TestNotNull(TEXT("Second list fixture"), Second)
		|| !TestNotNull(TEXT("Third list fixture"), Third))
	{
		return false;
	}

	FMCPToolRegistry Registry;
	RegisterNiagaraQueryTools(Registry);
	const FMCPToolResult Page = Registry.ExecuteTool(
		TEXT("content.niagara.system.list"), NiagaraPageParams(Folder.ToLower(), 0, 2));
	if (!TestTrue(TEXT("First page succeeds"), Page.bSuccess)
		|| !TestNotNull(TEXT("First page data"), Page.Data.Get()))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Rows = Page.Data->GetArrayField(TEXT("systems"));
	TestEqual(TEXT("Filter-scoped total precedes paging"), Page.Data->GetIntegerField(TEXT("total")), 3);
	TestEqual(TEXT("First page count"), Page.Data->GetIntegerField(TEXT("count")), 2);
	TestTrue(TEXT("First page has continuation"), Page.Data->GetBoolField(TEXT("hasMore")));
	TestEqual(TEXT("Next offset advances by returned rows"), Page.Data->GetIntegerField(TEXT("nextOffset")), 2);
	if (!TestEqual(TEXT("First page returns two rows"), Rows.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("First row uses stable object-path order"),
		Rows[0]->AsObject()->GetStringField(TEXT("path")), First->GetPathName());
	TestEqual(TEXT("Second row uses stable object-path order"),
		Rows[1]->AsObject()->GetStringField(TEXT("path")), Second->GetPathName());

	const FMCPToolResult LastPage = Registry.ExecuteTool(
		TEXT("content.niagara.system.list"), NiagaraPageParams(Folder, 2, 2));
	if (TestTrue(TEXT("Final page succeeds"), LastPage.bSuccess)
		&& TestNotNull(TEXT("Final page data"), LastPage.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& LastRows = LastPage.Data->GetArrayField(TEXT("systems"));
		TestFalse(TEXT("Final page is terminal"), LastPage.Data->GetBoolField(TEXT("hasMore")));
		TestFalse(TEXT("Terminal page has no continuation offset"), LastPage.Data->HasField(TEXT("nextOffset")));
		if (TestEqual(TEXT("Final page returns one row"), LastRows.Num(), 1))
		{
			TestEqual(TEXT("Final page has no duplication or skipped asset"),
				LastRows[0]->AsObject()->GetStringField(TEXT("path")), Third->GetPathName());
		}
	}

	const FMCPToolResult PastEnd = Registry.ExecuteTool(
		TEXT("content.niagara.system.list"), NiagaraPageParams(Folder, MAX_int32, 200));
	if (TestTrue(TEXT("Maximum valid offset succeeds"), PastEnd.bSuccess)
		&& TestNotNull(TEXT("Past-end data"), PastEnd.Data.Get()))
	{
		TestEqual(TEXT("Past-end page is empty"), PastEnd.Data->GetArrayField(TEXT("systems")).Num(), 0);
		TestEqual(TEXT("Past-end retains the filtered total"), PastEnd.Data->GetIntegerField(TEXT("total")), 3);
		TestFalse(TEXT("Past-end page is terminal"), PastEnd.Data->GetBoolField(TEXT("hasMore")));
	}

	TSharedRef<FJsonObject> LegacyRequest = MakeShared<FJsonObject>();
	LegacyRequest->SetStringField(TEXT("filter"), Folder);
	const FMCPToolResult DefaultPage = Registry.ExecuteTool(TEXT("content.niagara.system.list"), LegacyRequest);
	if (TestTrue(TEXT("Paging arguments remain optional"), DefaultPage.bSuccess)
		&& TestNotNull(TEXT("Default page data"), DefaultPage.Data.Get()))
	{
		TestEqual(TEXT("Default limit is bounded"), DefaultPage.Data->GetIntegerField(TEXT("limit")), 50);
		TestEqual(TEXT("Default offset starts at zero"), DefaultPage.Data->GetIntegerField(TEXT("offset")), 0);
		TestEqual(TEXT("Default page returns the three matching systems"), DefaultPage.Data->GetArrayField(TEXT("systems")).Num(), 3);
	}

	struct FInvalidPage
	{
		const TCHAR* Field;
		double Value;
	};
	const FInvalidPage InvalidPages[] = {
		{TEXT("limit"), 0.0}, {TEXT("limit"), 201.0}, {TEXT("limit"), 1.5},
		{TEXT("offset"), -1.0}, {TEXT("offset"), 0.5}, {TEXT("offset"), 2147483648.0},
	};
	for (const FInvalidPage& Invalid : InvalidPages)
	{
		TSharedRef<FJsonObject> Params = NiagaraPageParams(Folder, 0, 2);
		Params->SetNumberField(Invalid.Field, Invalid.Value);
		const FMCPToolResult Result = Registry.ExecuteTool(TEXT("content.niagara.system.list"), Params);
		const FString Label = FString::Printf(TEXT("Reject %s=%g"), Invalid.Field, Invalid.Value);
		TestFalse(Label, Result.bSuccess);
		TestEqual(Label + TEXT(" error code"), Result.ErrorCode, FString(TEXT("invalid_params")));
		TestEqual(Label + TEXT(" HTTP status"), Result.HttpStatus, 422);
	}
	TSharedRef<FJsonObject> InvalidFilter = NiagaraPageParams(Folder, 0, 2);
	InvalidFilter->SetBoolField(TEXT("filter"), true);
	const FMCPToolResult BadFilter = Registry.ExecuteTool(TEXT("content.niagara.system.list"), InvalidFilter);
	TestFalse(TEXT("A non-string filter is rejected"), BadFilter.bSuccess);
	for (const UNiagaraSystem* System : {First, Second, Third})
	{
		TestFalse(TEXT("Inventory queries preserve clean fixture packages"), System->GetOutermost()->IsDirty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FNiagaraAuthoringDiagnosticsQueryTest,
	"UE_AI_integration.Niagara.Authoring.UncompiledDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FNiagaraAuthoringDiagnosticsQueryTest::RunTest(const FString&)
{
	const FString PackageName = TEXT("/Game/Automation/UEAI_NiagaraDiagnostics_")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UNiagaraSystem* System = CreateNiagaraQueryFixture(PackageName);
	ON_SCOPE_EXIT { ReleaseNiagaraQueryFixture(System); };
	if (!TestNotNull(TEXT("Uncompiled system fixture"), System))
	{
		return false;
	}
	// A freshly constructed UNiagaraSystem eagerly owns uncompiled spawn/update
	// script objects; "uncompiled" here means no compiled data, not a null script.
	if (!TestNotNull(TEXT("Baseline owns a spawn script"), System->GetSystemSpawnScript())
		|| !TestNotNull(TEXT("Baseline owns an update script"), System->GetSystemUpdateScript()))
	{
		return false;
	}
	TestFalse(TEXT("Baseline has no compilation requests"), System->HasOutstandingCompilationRequests(true));

	FMCPToolRegistry Registry;
	RegisterNiagaraQueryTools(Registry);
	for (bool bInitiallyDirty : {false, true})
	{
		System->GetOutermost()->SetDirtyFlag(bInitiallyDirty);
		const FMCPToolResult Diagnostics = Registry.ExecuteTool(
			TEXT("content.niagara.system.diagnostics.get"), NiagaraTargetParams(System));
		if (!TestTrue(TEXT("Uncompiled diagnostics is a successful observation"), Diagnostics.bSuccess)
			|| !TestNotNull(TEXT("Diagnostics data"), Diagnostics.Data.Get()))
		{
			return false;
		}
		TestEqual(TEXT("Missing scripts report unknown status"), Diagnostics.Data->GetStringField(TEXT("status")), FString(TEXT("unknown")));
		TestFalse(TEXT("Missing scripts cannot report compiled"), Diagnostics.Data->GetBoolField(TEXT("compiled")));
		TestTrue(TEXT("Missing scripts mark diagnostics stale"), Diagnostics.Data->GetBoolField(TEXT("diagnosticsMayBeStale")));
		TestEqual(TEXT("Uncompiled spawn and update scripts are reported"), Diagnostics.Data->GetIntegerField(TEXT("scriptCount")), 2);
		TestEqual(TEXT("Query preserves the original dirty state"), System->GetOutermost()->IsDirty(), bInitiallyDirty);
		TestFalse(TEXT("Query does not queue compilation"), System->HasOutstandingCompilationRequests(true));
		TestNotNull(TEXT("Query preserves the spawn script object"), System->GetSystemSpawnScript());
		TestNotNull(TEXT("Query preserves the update script object"), System->GetSystemUpdateScript());
	}

	for (const TCHAR* Query : {TEXT("content.niagara.system.diagnostics.get"), TEXT("content.niagara.emitter.gpu_hlsl.get")})
	{
		TSharedRef<FJsonObject> Params = NiagaraTargetParams(System);
		Params->SetBoolField(TEXT("compileFirst"), true);
		const FMCPToolResult Rejected = Registry.ExecuteTool(Query, Params);
		TestFalse(FString(Query) + TEXT(" rejects implicit compilation"), Rejected.bSuccess);
		TestEqual(TEXT("CompileFirst rejection is an invalid request"), Rejected.HttpStatus, 422);
		TestFalse(TEXT("Rejected query still has no compilation requests"), System->HasOutstandingCompilationRequests(true));
	}

	TSharedRef<FJsonObject> InvalidCompile = NiagaraTargetParams(System);
	InvalidCompile->SetStringField(TEXT("waitForCompletion"), TEXT("true"));
	const FMCPToolResult RejectedCompile = Registry.ExecuteTool(TEXT("content.niagara.system.compile.request"), InvalidCompile);
	TestFalse(TEXT("Compile wait flag must be a boolean"), RejectedCompile.bSuccess);
	TestEqual(TEXT("Invalid wait flag is rejected before compilation"), RejectedCompile.HttpStatus, 422);
	TestFalse(TEXT("Invalid compile request has no side effects"), System->HasOutstandingCompilationRequests(true));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
