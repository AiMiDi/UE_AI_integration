#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UEWorkflowCore/WorkflowCore.h"

namespace
{
std::string ToUtf8(const FString& Value)
{
	const FTCHARToUTF8 Converted(*Value);
	return std::string(Converted.Get(), Converted.Length());
}

std::vector<std::string> ReadStrings(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	std::vector<std::string> Result;
	if (!Object.IsValid()
		|| !Object->HasTypedField<EJson::Array>(Field))
	{
		return Result;
	}
	for (const TSharedPtr<FJsonValue>& Value :
		Object->GetArrayField(Field))
	{
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			Result.push_back(ToUtf8(Value->AsString()));
		}
	}
	return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCapabilitySearchGoldenVectorTest,
	"UE_AI_integration.Registry.CapabilitySearchGoldenVectors",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FCapabilitySearchGoldenVectorTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	if (!TestTrue(TEXT("Plugin is available"), Plugin.IsValid()))
	{
		return false;
	}
	FString JsonText;
	const FString ContractPath = FPaths::Combine(
		Plugin->GetBaseDir(),
		TEXT("Resources/Contracts/capability-search-v1.json"));
	if (!TestTrue(
		TEXT("Capability search contract is readable"),
		FFileHelper::LoadFileToString(JsonText, *ContractPath)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(JsonText);
	if (!TestTrue(
		TEXT("Capability search contract is valid JSON"),
		FJsonSerializer::Deserialize(Reader, Root)
			&& Root.IsValid()))
	{
		return false;
	}

	struct FDocument
	{
		std::string Id;
		ue::workflow::CapabilitySearchDocument Search;
	};
	TArray<FDocument> Documents;
	for (const TSharedPtr<FJsonValue>& Value :
		Root->GetArrayField(TEXT("documents")))
	{
		const TSharedPtr<FJsonObject> Descriptor = Value->AsObject();
		const TSharedPtr<FJsonObject> Search =
			Descriptor->GetObjectField(TEXT("search"));
		FDocument Document;
		Document.Id = ToUtf8(
			Descriptor->GetStringField(TEXT("id")));
		Document.Search.id = Document.Id;
		Document.Search.description = ToUtf8(
			Descriptor->GetStringField(TEXT("description")));
		FString Title;
		if (Search->TryGetStringField(TEXT("title"), Title))
		{
			Document.Search.title = ToUtf8(Title);
		}
		Document.Search.keywords =
			ReadStrings(Search, TEXT("keywords"));
		Document.Search.aliases =
			ReadStrings(Search, TEXT("aliases"));
		Documents.Add(MoveTemp(Document));
	}

	for (const TSharedPtr<FJsonValue>& Value :
		Root->GetArrayField(TEXT("vectors")))
	{
		const TSharedPtr<FJsonObject> Vector = Value->AsObject();
		const std::string Query =
			ToUtf8(Vector->GetStringField(TEXT("query")));
		struct FActual
		{
			std::string Id;
			ue::workflow::CapabilitySearchMatch Match;
		};
		TArray<FActual> Actual;
		for (const FDocument& Document : Documents)
		{
			const std::optional<ue::workflow::CapabilitySearchMatch>
				Match = ue::workflow::MatchCapabilitySearch(
					Query,
					Document.Search);
			if (Match)
			{
				Actual.Add({Document.Id, *Match});
			}
		}
		Actual.Sort(
			[](const FActual& Left, const FActual& Right)
			{
				return Left.Match.score != Right.Match.score
					? Left.Match.score > Right.Match.score
					: Left.Id < Right.Id;
			});

		const TArray<TSharedPtr<FJsonValue>>& Expected =
			Vector->GetArrayField(TEXT("matches"));
		TestEqual(
			*FString::Printf(
				TEXT("Match count for '%s'"),
				*Vector->GetStringField(TEXT("query"))),
			Actual.Num(),
			Expected.Num());
		for (int32 Index = 0;
			Index < FMath::Min(Actual.Num(), Expected.Num());
			++Index)
		{
			const TSharedPtr<FJsonObject> ExpectedMatch =
				Expected[Index]->AsObject();
			TestEqual(
				TEXT("Search result id"),
				FString(UTF8_TO_TCHAR(Actual[Index].Id.c_str())),
				ExpectedMatch->GetStringField(TEXT("id")));
			TestEqual(
				TEXT("Search result score"),
				Actual[Index].Match.score,
				static_cast<int32>(
					ExpectedMatch->GetNumberField(TEXT("score"))));
			TestTrue(
				TEXT("Search result matchedFields"),
				Actual[Index].Match.matched_fields
					== ReadStrings(
						ExpectedMatch,
						TEXT("matchedFields")));
			TestTrue(
				TEXT("Search result matchedTokens"),
				Actual[Index].Match.matched_tokens
					== ReadStrings(
						ExpectedMatch,
						TEXT("matchedTokens")));
		}
	}
	return true;
}

#endif
