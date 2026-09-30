#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintReadTools(FMCPToolRegistry& Registry);
void RegisterMaterialReadTools(FMCPToolRegistry& Registry);
}

namespace
{
template <typename TAsset>
TAsset* RegisterQueryFixture(const FString& PackageName)
{
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	TAsset* Asset = NewObject<TAsset>(
		Package,
		*AssetName,
		RF_Public | RF_Standalone | RF_Transactional);
	if (Asset)
	{
		FAssetRegistryModule::AssetCreated(Asset);
	}
	return Asset;
}

void UnregisterQueryFixture(UObject* Asset)
{
	if (!Asset)
	{
		return;
	}

	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->MarkAsGarbage();
	if (UPackage* Package = Asset->GetOutermost())
	{
		Package->SetDirtyFlag(false);
	}
}

TSharedRef<FJsonObject> QueryPageParams(
	const FString& Filter,
	int32 Offset,
	int32 Limit,
	const TCHAR* Type)
{
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("filter"), Filter);
	Params->SetStringField(TEXT("type"), Type);
	Params->SetNumberField(TEXT("offset"), Offset);
	Params->SetNumberField(TEXT("limit"), Limit);
	return Params;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintListPaginationContractTest,
	"UE_AI_integration.Blueprint.Read.ListPaginationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintListPaginationContractTest::RunTest(const FString&)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Filter = TEXT("UEAI_ListBlueprint_") + Suffix;
	const FString PackageA = TEXT("/Game/Automation/") + Filter + TEXT("_A");
	const FString PackageB = TEXT("/Game/Automation/") + Filter + TEXT("_B");

	// Register in reverse lexical order so the assertion proves the query sorts
	// before applying offset/limit rather than inheriting registry insertion order.
	UBlueprint* BlueprintB = RegisterQueryFixture<UBlueprint>(PackageB);
	UBlueprint* BlueprintA = RegisterQueryFixture<UBlueprint>(PackageA);
	ON_SCOPE_EXIT
	{
		UnregisterQueryFixture(BlueprintA);
		UnregisterQueryFixture(BlueprintB);
	};
	TestNotNull(TEXT("Blueprint A query fixture"), BlueprintA);
	TestNotNull(TEXT("Blueprint B query fixture"), BlueprintB);
	if (!BlueprintA || !BlueprintB)
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintReadTools(Registry);
	Registry.EndDomainRegistration();

	const FMCPToolResult First = Registry.ExecuteTool(
		TEXT("blueprint.asset.list"),
		QueryPageParams(Filter, 0, 1, TEXT("regular")));
	if (TestTrue(TEXT("First Blueprint page succeeds"), First.bSuccess)
		&& TestNotNull(TEXT("First Blueprint page data"), First.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			First.Data->GetArrayField(TEXT("blueprints"));
		TestEqual(TEXT("First Blueprint page count"), Rows.Num(), 1);
		TestEqual(TEXT("Blueprint total is computed after filters"),
			First.Data->GetIntegerField(TEXT("total")), 2);
		TestEqual(TEXT("Blueprint page reports its limit"),
			First.Data->GetIntegerField(TEXT("limit")), 1);
		TestEqual(TEXT("Blueprint page reports its offset"),
			First.Data->GetIntegerField(TEXT("offset")), 0);
		TestTrue(TEXT("First Blueprint page has a continuation"),
			First.Data->GetBoolField(TEXT("hasMore")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Blueprint results are deterministically sorted"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")), PackageA);
		}
	}

	const FMCPToolResult Second = Registry.ExecuteTool(
		TEXT("blueprint.asset.list"),
		QueryPageParams(Filter, 1, 1, TEXT("regular")));
	if (TestTrue(TEXT("Second Blueprint page succeeds"), Second.bSuccess)
		&& TestNotNull(TEXT("Second Blueprint page data"), Second.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			Second.Data->GetArrayField(TEXT("blueprints"));
		TestEqual(TEXT("Second Blueprint page count"), Rows.Num(), 1);
		TestFalse(TEXT("Second Blueprint page is terminal"),
			Second.Data->GetBoolField(TEXT("hasMore")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Blueprint offset selects the second sorted asset"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")), PackageB);
		}
	}

	TSharedRef<FJsonObject> FractionalLimit = QueryPageParams(
		Filter, 0, 1, TEXT("regular"));
	FractionalLimit->SetNumberField(TEXT("limit"), 1.5);
	const FMCPToolResult Fractional = Registry.ExecuteTool(
		TEXT("blueprint.asset.list"), FractionalLimit);
	TestFalse(TEXT("Fractional Blueprint limit is rejected"), Fractional.bSuccess);
	TestEqual(TEXT("Fractional Blueprint limit error is stable"),
		Fractional.ErrorCode, FString(TEXT("invalid_params")));

	TSharedRef<FJsonObject> InvalidType = QueryPageParams(
		Filter, 0, 1, TEXT("widget"));
	const FMCPToolResult BadType = Registry.ExecuteTool(
		TEXT("blueprint.asset.list"), InvalidType);
	TestFalse(TEXT("Unknown Blueprint type is rejected"), BadType.bSuccess);
	TestEqual(TEXT("Unknown Blueprint type error is stable"),
		BadType.ErrorCode, FString(TEXT("invalid_params")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialListPaginationContractTest,
	"UE_AI_integration.Material.Read.ListPaginationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialListPaginationContractTest::RunTest(const FString&)
{
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Filter = TEXT("UEAI_ListMaterial_") + Suffix;
	const FString PackageA = TEXT("/Game/Automation/") + Filter + TEXT("_A");
	const FString PackageB = TEXT("/Game/Automation/") + Filter + TEXT("_B");
	const FString PackageC = TEXT("/Game/Automation/") + Filter + TEXT("_C");
	const FString FunctionFilter = TEXT("UEAI_ListFunction_") + Suffix;
	const FString FunctionPackageA =
		TEXT("/Game/Automation/") + FunctionFilter + TEXT("_A");
	const FString FunctionPackageB =
		TEXT("/Game/Automation/") + FunctionFilter + TEXT("_B");

	UMaterial* MaterialB = RegisterQueryFixture<UMaterial>(PackageB);
	UMaterial* MaterialA = RegisterQueryFixture<UMaterial>(PackageA);
	UMaterialInstanceConstant* MaterialInstance =
		RegisterQueryFixture<UMaterialInstanceConstant>(PackageC);
	UMaterialFunction* FunctionB =
		RegisterQueryFixture<UMaterialFunction>(FunctionPackageB);
	UMaterialFunction* FunctionA =
		RegisterQueryFixture<UMaterialFunction>(FunctionPackageA);
	ON_SCOPE_EXIT
	{
		UnregisterQueryFixture(FunctionA);
		UnregisterQueryFixture(FunctionB);
		UnregisterQueryFixture(MaterialInstance);
		UnregisterQueryFixture(MaterialA);
		UnregisterQueryFixture(MaterialB);
	};
	TestNotNull(TEXT("Material A query fixture"), MaterialA);
	TestNotNull(TEXT("Material B query fixture"), MaterialB);
	TestNotNull(TEXT("Material Instance query fixture"), MaterialInstance);
	TestNotNull(TEXT("Material Function A query fixture"), FunctionA);
	TestNotNull(TEXT("Material Function B query fixture"), FunctionB);
	if (!MaterialA || !MaterialB || !MaterialInstance || !FunctionA || !FunctionB)
	{
		return false;
	}

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialReadTools(Registry);
	Registry.EndDomainRegistration();

	const FMCPToolResult First = Registry.ExecuteTool(
		TEXT("content.material.list"),
		QueryPageParams(Filter, 0, 1, TEXT("material")));
	if (TestTrue(TEXT("First Material page succeeds"), First.bSuccess)
		&& TestNotNull(TEXT("First Material page data"), First.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			First.Data->GetArrayField(TEXT("materials"));
		TestEqual(TEXT("First Material page count"), Rows.Num(), 1);
		TestEqual(TEXT("Material total is computed after filters"),
			First.Data->GetIntegerField(TEXT("total")), 2);
		TestEqual(TEXT("Material page reports its limit"),
			First.Data->GetIntegerField(TEXT("limit")), 1);
		TestEqual(TEXT("Material page reports its offset"),
			First.Data->GetIntegerField(TEXT("offset")), 0);
		TestTrue(TEXT("First Material page has a continuation"),
			First.Data->GetBoolField(TEXT("hasMore")));
		TestTrue(TEXT("Material continuation is explicitly truncated"),
			First.Data->GetBoolField(TEXT("truncated")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Material results are deterministically sorted"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")), PackageA);
		}
	}

	const FMCPToolResult BeyondEnd = Registry.ExecuteTool(
		TEXT("content.material.list"),
		QueryPageParams(Filter, 3, 1, TEXT("all")));
	if (TestTrue(TEXT("Material offset at total succeeds"), BeyondEnd.bSuccess)
		&& TestNotNull(TEXT("Material end page data"), BeyondEnd.Data.Get()))
	{
		TestEqual(TEXT("Material end page is empty"),
			BeyondEnd.Data->GetArrayField(TEXT("materials")).Num(), 0);
		TestEqual(TEXT("Material end page preserves total"),
			BeyondEnd.Data->GetIntegerField(TEXT("total")), 3);
		TestFalse(TEXT("Material end page has no continuation"),
			BeyondEnd.Data->GetBoolField(TEXT("hasMore")));
	}

	const FMCPToolResult Mixed = Registry.ExecuteTool(
		TEXT("content.material.list"),
		QueryPageParams(Filter, 0, 3, TEXT("all")));
	if (TestTrue(TEXT("Mixed Material page succeeds"), Mixed.bSuccess)
		&& TestNotNull(TEXT("Mixed Material page data"), Mixed.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			Mixed.Data->GetArrayField(TEXT("materials"));
		TestEqual(TEXT("Mixed Material page includes all fixture types"),
			Rows.Num(), 3);
		if (Rows.Num() == 3)
		{
			TestEqual(TEXT("Mixed page first path"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")), PackageA);
			TestEqual(TEXT("Mixed page second path"),
				Rows[1]->AsObject()->GetStringField(TEXT("path")), PackageB);
			TestEqual(TEXT("Mixed page third path"),
				Rows[2]->AsObject()->GetStringField(TEXT("path")), PackageC);
			TestEqual(TEXT("Mixed page identifies Material assets"),
				Rows[0]->AsObject()->GetStringField(TEXT("type")),
				FString(TEXT("Material")));
			TestEqual(TEXT("Mixed page identifies MaterialInstance assets"),
				Rows[2]->AsObject()->GetStringField(TEXT("type")),
				FString(TEXT("MaterialInstance")));
		}
	}

	const FMCPToolResult Second = Registry.ExecuteTool(
		TEXT("content.material.list"),
		QueryPageParams(Filter, 1, 1, TEXT("material")));
	if (TestTrue(TEXT("Second Material page succeeds"), Second.bSuccess)
		&& TestNotNull(TEXT("Second Material page data"), Second.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			Second.Data->GetArrayField(TEXT("materials"));
		TestEqual(TEXT("Second Material page count"), Rows.Num(), 1);
		TestFalse(TEXT("Second Material page is terminal"),
			Second.Data->GetBoolField(TEXT("hasMore")));
		TestFalse(TEXT("Terminal Material page is not truncated"),
			Second.Data->GetBoolField(TEXT("truncated")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Material offset selects the second sorted asset"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")), PackageB);
		}
	}

	TSharedRef<FJsonObject> LimitWins = MakeShared<FJsonObject>();
	LimitWins->SetStringField(TEXT("query"), Filter);
	LimitWins->SetNumberField(TEXT("offset"), 0);
	LimitWins->SetNumberField(TEXT("limit"), 2);
	// maxResults is intentionally invalid if selected; the explicit limit must
	// take precedence for backward-compatible callers that send both fields.
	LimitWins->SetNumberField(TEXT("maxResults"), 0);
	const FMCPToolResult PreferredLimit = Registry.ExecuteTool(
		TEXT("content.material.search"), LimitWins);
	if (TestTrue(TEXT("Explicit search limit overrides maxResults"),
		PreferredLimit.bSuccess)
		&& TestNotNull(TEXT("Preferred search limit data"),
			PreferredLimit.Data.Get()))
	{
		TestEqual(TEXT("Search reports the explicit limit"),
			PreferredLimit.Data->GetIntegerField(TEXT("limit")), 2);
		TestEqual(TEXT("Explicit search limit returns both matches"),
			PreferredLimit.Data->GetArrayField(TEXT("results")).Num(), 2);
	}

	TSharedRef<FJsonObject> FunctionPageA = MakeShared<FJsonObject>();
	FunctionPageA->SetStringField(TEXT("filter"), FunctionFilter);
	FunctionPageA->SetNumberField(TEXT("offset"), 0);
	FunctionPageA->SetNumberField(TEXT("limit"), 1);
	const FMCPToolResult FirstFunctionPage = Registry.ExecuteTool(
		TEXT("content.material.function.list"), FunctionPageA);
	if (TestTrue(TEXT("First Material Function page succeeds"),
		FirstFunctionPage.bSuccess)
		&& TestNotNull(TEXT("First Material Function page data"),
			FirstFunctionPage.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			FirstFunctionPage.Data->GetArrayField(TEXT("functions"));
		TestEqual(TEXT("Material Function total is filter-scoped"),
			FirstFunctionPage.Data->GetIntegerField(TEXT("total")), 2);
		TestTrue(TEXT("First Material Function page has a continuation"),
			FirstFunctionPage.Data->GetBoolField(TEXT("hasMore")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Material Functions are deterministically sorted"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")),
				FunctionPackageA);
		}
	}

	TSharedRef<FJsonObject> FunctionPageB = MakeShared<FJsonObject>();
	FunctionPageB->SetStringField(TEXT("filter"), FunctionFilter);
	FunctionPageB->SetNumberField(TEXT("offset"), 1);
	FunctionPageB->SetNumberField(TEXT("limit"), 1);
	const FMCPToolResult SecondFunctionPage = Registry.ExecuteTool(
		TEXT("content.material.function.list"), FunctionPageB);
	if (TestTrue(TEXT("Second Material Function page succeeds"),
		SecondFunctionPage.bSuccess)
		&& TestNotNull(TEXT("Second Material Function page data"),
			SecondFunctionPage.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			SecondFunctionPage.Data->GetArrayField(TEXT("functions"));
		TestFalse(TEXT("Second Material Function page is terminal"),
			SecondFunctionPage.Data->GetBoolField(TEXT("hasMore")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Material Function offset selects the second asset"),
				Rows[0]->AsObject()->GetStringField(TEXT("path")),
				FunctionPackageB);
		}
	}

	TSharedRef<FJsonObject> LegacySearchPage = MakeShared<FJsonObject>();
	LegacySearchPage->SetStringField(TEXT("query"), Filter);
	LegacySearchPage->SetNumberField(TEXT("offset"), 0);
	LegacySearchPage->SetNumberField(TEXT("maxResults"), 1);
	const FMCPToolResult Search = Registry.ExecuteTool(
		TEXT("content.material.search"), LegacySearchPage);
	if (TestTrue(TEXT("Legacy maxResults search page succeeds"), Search.bSuccess)
		&& TestNotNull(TEXT("Legacy search page data"), Search.Data.Get()))
	{
		const TArray<TSharedPtr<FJsonValue>>& Rows =
			Search.Data->GetArrayField(TEXT("results"));
		TestEqual(TEXT("Legacy maxResults maps to the bounded limit"),
			Search.Data->GetIntegerField(TEXT("limit")), 1);
		TestEqual(TEXT("Search total is computed before pagination"),
			Search.Data->GetIntegerField(TEXT("total")), 2);
		TestEqual(TEXT("Search resultCount matches the returned page"),
			Search.Data->GetIntegerField(TEXT("resultCount")), 1);
		TestTrue(TEXT("First search page has a continuation"),
			Search.Data->GetBoolField(TEXT("hasMore")));
		if (!Rows.IsEmpty())
		{
			TestEqual(TEXT("Material search uses the same stable asset order"),
				Rows[0]->AsObject()->GetStringField(TEXT("materialPath")), PackageA);
		}
	}

	TSharedRef<FJsonObject> FractionalLegacySearch = MakeShared<FJsonObject>();
	FractionalLegacySearch->SetStringField(TEXT("query"), Filter);
	FractionalLegacySearch->SetNumberField(TEXT("maxResults"), 1.5);
	const FMCPToolResult InvalidLegacySearch = Registry.ExecuteTool(
		TEXT("content.material.search"), FractionalLegacySearch);
	TestFalse(TEXT("Fractional legacy maxResults is rejected"),
		InvalidLegacySearch.bSuccess);
	TestEqual(TEXT("Legacy maxResults uses the shared paging error contract"),
		InvalidLegacySearch.ErrorCode, FString(TEXT("invalid_request")));

	TSharedRef<FJsonObject> ZeroLimit = QueryPageParams(
		Filter, 0, 1, TEXT("material"));
	ZeroLimit->SetNumberField(TEXT("limit"), 0);
	const FMCPToolResult InvalidPage = Registry.ExecuteTool(
		TEXT("content.material.list"), ZeroLimit);
	TestFalse(TEXT("Zero Material limit is rejected"), InvalidPage.bSuccess);
	TestEqual(TEXT("Zero Material limit error is stable"),
		InvalidPage.ErrorCode, FString(TEXT("invalid_request")));

	TSharedRef<FJsonObject> InvalidType = QueryPageParams(
		Filter, 0, 1, TEXT("function"));
	const FMCPToolResult BadType = Registry.ExecuteTool(
		TEXT("content.material.list"), InvalidType);
	TestFalse(TEXT("Unknown Material type is rejected"), BadType.bSuccess);
	TestEqual(TEXT("Unknown Material type error is stable"),
		BadType.ErrorCode, FString(TEXT("invalid_request")));

	TSharedRef<FJsonObject> FunctionPage = MakeShared<FJsonObject>();
	FunctionPage->SetNumberField(TEXT("offset"), -1);
	const FMCPToolResult InvalidFunctionPage = Registry.ExecuteTool(
		TEXT("content.material.function.list"), FunctionPage);
	TestFalse(TEXT("Negative Material Function offset is rejected"),
		InvalidFunctionPage.bSuccess);
	TestEqual(TEXT("Material Function paging uses the shared error contract"),
		InvalidFunctionPage.ErrorCode, FString(TEXT("invalid_request")));

	return true;
}

#endif
