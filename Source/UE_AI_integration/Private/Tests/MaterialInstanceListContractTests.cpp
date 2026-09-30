#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"

namespace UEAIIntegrationTools
{
void RegisterMaterialInstanceTools(FMCPToolRegistry& Registry);
}

namespace
{
template <typename TAsset>
TAsset* MaterialInstanceListRegisterFixture(const FString& PackageName)
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

void MaterialInstanceListUnregisterFixture(UObject* Asset)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMaterialInstanceListContractTest,
	"UE_AI_integration.Material.InstanceListContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMaterialInstanceListContractTest::RunTest(const FString&)
{
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("content"));
	UEAIIntegrationTools::RegisterMaterialInstanceTools(Registry);
	Registry.EndDomainRegistration();

	FMCPToolBase* Tool = Registry.FindTool(TEXT("content.material.instance.list"));
	if (!TestNotNull(TEXT("instance.list tool registered"), Tool))
	{
		return false;
	}
	const auto Query = [&](const TSharedPtr<FJsonObject>& Params)
	{
		return Tool->Execute(Params);
	};

	// (a) Unknown parent path -> material_parent_not_found (404).
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(
			TEXT("parent"),
			FString::Printf(
				TEXT("/Game/Automation/UEAI_ListMissing_%s.Missing"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		const FMCPToolResult Result = Query(Params);
		TestFalse(TEXT("Unknown parent is rejected"), Result.bSuccess);
		TestEqual(
			TEXT("Unknown parent error code"),
			Result.ErrorCode,
			FString(TEXT("material_parent_not_found")));
		TestEqual(TEXT("Unknown parent maps to 404"), Result.HttpStatus, 404);
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ParentPackage =
		TEXT("/Game/Automation/UEAI_ListParent_") + Suffix;
	const FString ChildPackage =
		TEXT("/Game/Automation/UEAI_ListChild_") + Suffix;
	const FString GrandChildPackage =
		TEXT("/Game/Automation/UEAI_ListGrandChild_") + Suffix;

	UMaterial* Parent =
		MaterialInstanceListRegisterFixture<UMaterial>(ParentPackage);
	UMaterialInstanceConstant* Child = nullptr;
	UMaterialInstanceConstant* GrandChild = nullptr;
	ON_SCOPE_EXIT
	{
		MaterialInstanceListUnregisterFixture(GrandChild);
		MaterialInstanceListUnregisterFixture(Child);
		MaterialInstanceListUnregisterFixture(Parent);
	};

	if (!TestNotNull(TEXT("Parent material fixture"), Parent))
	{
		return false;
	}
	Parent->SetShadingModel(MSM_Unlit);

	// (b) Parent exists but has no instances -> empty list, truncated false.
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("parent"), Parent->GetPathName());
		const FMCPToolResult Result = Query(Params);
		if (TestTrue(TEXT("Empty parent inventory succeeds"), Result.bSuccess)
			&& TestNotNull(
				TEXT("Empty parent inventory has data"),
				Result.Data.Get()))
		{
			TestEqual(
				TEXT("Empty parent totalFound"),
				Result.Data->GetIntegerField(TEXT("totalFound")),
				0);
			TestEqual(
				TEXT("Empty parent returned"),
				Result.Data->GetIntegerField(TEXT("returned")),
				0);
			TestFalse(
				TEXT("Empty parent not truncated"),
				Result.Data->GetBoolField(TEXT("truncated")));
			TestEqual(
				TEXT("Empty parent instances array"),
				Result.Data->GetArrayField(TEXT("instances")).Num(),
				0);
		}
	}

	// (c) Build a transient-safe child/grandchild without saving, then verify
	// the authored-asset inventory lists them. Skip if instance creation is
	// not possible in this environment.
	Child = MaterialInstanceListRegisterFixture<UMaterialInstanceConstant>(
		ChildPackage);
	GrandChild = MaterialInstanceListRegisterFixture<UMaterialInstanceConstant>(
		GrandChildPackage);
	if (!Child || !GrandChild)
	{
		AddInfo(TEXT("Skipping instance listing case: transient material instance creation is unavailable in this environment."));
		return true;
	}
	Child->SetParentEditorOnly(Parent, false);
	GrandChild->SetParentEditorOnly(Child, false);

	// Non-recursive listing includes only the immediate child.
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("parent"), Parent->GetPathName());
		const FMCPToolResult Result = Query(Params);
		if (TestTrue(TEXT("Non-recursive listing succeeds"), Result.bSuccess)
			&& TestNotNull(
				TEXT("Non-recursive listing has data"),
				Result.Data.Get()))
		{
			TestEqual(
				TEXT("Non-recursive schema"),
				Result.Data->GetStringField(TEXT("schema")),
				FString(TEXT("ue.material.instance-list.v1")));
			TestEqual(
				TEXT("Non-recursive resolved parent"),
				Result.Data->GetStringField(TEXT("parent")),
				Parent->GetPathName());
			TestFalse(
				TEXT("Non-recursive flag echoed"),
				Result.Data->GetBoolField(TEXT("recursive")));
			TestEqual(
				TEXT("Default maxResults"),
				Result.Data->GetIntegerField(TEXT("maxResults")),
				256);
			TestEqual(
				TEXT("Non-recursive totalFound"),
				Result.Data->GetIntegerField(TEXT("totalFound")),
				1);
			TestEqual(
				TEXT("Non-recursive returned"),
				Result.Data->GetIntegerField(TEXT("returned")),
				1);
			TestFalse(
				TEXT("Non-recursive not truncated"),
				Result.Data->GetBoolField(TEXT("truncated")));
			TestFalse(
				TEXT("Non-recursive reports not saved"),
				Result.Data->GetBoolField(TEXT("saved")));
			TestFalse(
				TEXT("Non-recursive reports not compiled"),
				Result.Data->GetBoolField(TEXT("compiled")));
			const TArray<TSharedPtr<FJsonValue>>& Rows =
				Result.Data->GetArrayField(TEXT("instances"));
			if (TestEqual(TEXT("Non-recursive row count"), Rows.Num(), 1))
			{
				const TSharedPtr<FJsonObject> Row = Rows[0]->AsObject();
				TestEqual(
					TEXT("Direct child path listed"),
					Row->GetStringField(TEXT("path")),
					Child->GetPathName());
				TestEqual(
					TEXT("Direct child class"),
					Row->GetStringField(TEXT("class")),
					FString(TEXT("MaterialInstanceConstant")));
				TestEqual(
					TEXT("Direct child parent path"),
					Row->GetStringField(TEXT("parentPath")),
					Parent->GetPathName());
			}
		}
	}

	// Recursive listing reaches the grandchild through the child.
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("parent"), Parent->GetPathName());
		Params->SetBoolField(TEXT("recursive"), true);
		const FMCPToolResult Result = Query(Params);
		if (TestTrue(TEXT("Recursive listing succeeds"), Result.bSuccess)
			&& TestNotNull(
				TEXT("Recursive listing has data"),
				Result.Data.Get()))
		{
			TestTrue(
				TEXT("Recursive flag echoed"),
				Result.Data->GetBoolField(TEXT("recursive")));
			TestEqual(
				TEXT("Recursive totalFound"),
				Result.Data->GetIntegerField(TEXT("totalFound")),
				2);
			TestEqual(
				TEXT("Recursive returned"),
				Result.Data->GetIntegerField(TEXT("returned")),
				2);
			const TArray<TSharedPtr<FJsonValue>>& Rows =
				Result.Data->GetArrayField(TEXT("instances"));
			TSet<FString> Paths;
			for (const TSharedPtr<FJsonValue>& Value : Rows)
			{
				Paths.Add(Value->AsObject()->GetStringField(TEXT("path")));
			}
			TestTrue(
				TEXT("Recursive includes direct child"),
				Paths.Contains(Child->GetPathName()));
			TestTrue(
				TEXT("Recursive includes grandchild"),
				Paths.Contains(GrandChild->GetPathName()));
		}
	}

	// Bounded maxResults reports totalFound fully and sets truncated.
	{
		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("parent"), Parent->GetPathName());
		Params->SetBoolField(TEXT("recursive"), true);
		Params->SetNumberField(TEXT("maxResults"), 1);
		const FMCPToolResult Result = Query(Params);
		if (TestTrue(TEXT("Bounded recursive listing succeeds"), Result.bSuccess)
			&& TestNotNull(
				TEXT("Bounded recursive listing has data"),
				Result.Data.Get()))
		{
			TestEqual(
				TEXT("Bounded returned respects maxResults"),
				Result.Data->GetIntegerField(TEXT("returned")),
				1);
			TestEqual(
				TEXT("Bounded totalFound counts all matches"),
				Result.Data->GetIntegerField(TEXT("totalFound")),
				2);
			TestTrue(
				TEXT("Bounded listing is truncated"),
				Result.Data->GetBoolField(TEXT("truncated")));
		}
	}

	return true;
}

#endif
