#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Self.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Tools/MCPToolRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UEAIIntegrationTools
{
void RegisterBlueprintAnalysisTools(FMCPToolRegistry& Registry);

namespace BlueprintAnalysisTesting
{
TArray<TSharedPtr<FJsonObject>> ScanBlueprintForTesting(
	UBlueprint* Blueprint,
	const FString& AssetPath);
}
}

namespace
{
using UEAIIntegrationTools::BlueprintAnalysisTesting::ScanBlueprintForTesting;

constexpr const TCHAR* CorpusRoot = TEXT("/Game/__UEAICorpus/BlueprintAnalysis");

struct FCorpusFixture
{
	FString Id;
	FString Recipe;
	FString Pair;
	FString Rule;
	FString Severity;
	FString Graph;
	FString NodeType;
	FString Function;
	FString Pin;
	FString RuntimeStatus;
	bool bExpected = false;
	FString PackageName;
	FString ObjectPath;
	UBlueprint* Blueprint = nullptr;
};

struct FRuleMetrics
{
	int32 TruePositive = 0;
	int32 FalsePositive = 0;
	int32 FalseNegative = 0;
	int32 TrueNegative = 0;
};

FString ReadString(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

FString CorpusPath()
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	return Plugin.IsValid()
		? FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Tests/Corpus/BlueprintAnalysis/portable.json"))
		: FString();
}

FString D5OverlayPath()
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("UE_AI_integration"));
	return Plugin.IsValid()
		? FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Tests/Corpus/BlueprintAnalysis/d5-overlay.json"))
		: FString();
}

struct FD5OverlayLabel
{
	FString Rule;
	FString Severity;
	FString Graph;
	FString NodeType;
	FString Function;
	FString Pin;
	FString RuntimeStatus;
};

struct FD5OverlayAsset
{
	FString Asset;
	FString ReviewStatus;
	FString RetiredReason;
	TArray<FD5OverlayLabel> Labels;
};

FString OverlayLabelKey(
	const FString& Asset,
	const FD5OverlayLabel& Label)
{
	TArray<FString> Parts;
	Parts.Add(Asset);
	Parts.Add(Label.Rule);
	Parts.Add(Label.Severity.ToLower());
	Parts.Add(Label.Graph);
	Parts.Add(Label.NodeType);
	Parts.Add(Label.Function.ToLower());
	Parts.Add(Label.Pin.ToLower());
	Parts.Add(
		Label.RuntimeStatus.IsEmpty()
			? TEXT("hypothesis")
			: Label.RuntimeStatus.ToLower());
	return FString::Join(Parts, TEXT("|"));
}

FString OverlayFindingKey(
	const FString& Asset,
	const TSharedPtr<FJsonObject>& Finding)
{
	const TSharedPtr<FJsonObject>* Location = nullptr;
	const TSharedPtr<FJsonObject>* Evidence = nullptr;
	Finding->TryGetObjectField(TEXT("location"), Location);
	Finding->TryGetObjectField(TEXT("evidence"), Evidence);
	FString RuntimeStatus = ReadString(Finding, TEXT("runtimeStatus"));
	if (RuntimeStatus.IsEmpty())
	{
		RuntimeStatus = TEXT("hypothesis");
	}
	FD5OverlayLabel Label;
	Label.Rule = ReadString(Finding, TEXT("ruleId"));
	Label.Severity = ReadString(Finding, TEXT("severity"));
	Label.Graph = Location && Location->IsValid()
		? ReadString(*Location, TEXT("graph"))
		: FString();
	Label.NodeType = Evidence && Evidence->IsValid()
		? ReadString(*Evidence, TEXT("nodeClass"))
		: FString();
	Label.Function = Evidence && Evidence->IsValid()
		? ReadString(*Evidence, TEXT("function"))
		: FString();
	Label.Pin = Evidence && Evidence->IsValid()
		? ReadString(*Evidence, TEXT("pinName"))
		: FString();
	Label.RuntimeStatus = RuntimeStatus;
	return OverlayLabelKey(Asset, Label);
}

bool LoadD5Overlay(
	TArray<FD5OverlayAsset>& OutAssets,
	FString& OutError)
{
	const FString Path = D5OverlayPath();
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(
			TEXT("D5 Blueprint corpus overlay is missing from '%s'."),
			*Path);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root)
		|| !Root.IsValid()
		|| ReadString(Root, TEXT("schema")) != TEXT("ue.blueprint-corpus.v1")
		|| ReadString(Root, TEXT("blockingScope")) != TEXT("installation")
		|| !Root->HasTypedField<EJson::Array>(TEXT("assets")))
	{
		OutError = TEXT("D5 overlay does not satisfy ue.blueprint-corpus.v1 installation scope.");
		return false;
	}
	TSet<FString> SeenAssets;
	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("assets")))
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutError = TEXT("D5 overlay assets must be objects.");
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FD5OverlayAsset Asset;
		Asset.Asset = ReadString(Object, TEXT("asset"));
		Asset.ReviewStatus = ReadString(Object, TEXT("reviewStatus"));
		Asset.RetiredReason = ReadString(Object, TEXT("retiredReason"));
		if (Asset.Asset.IsEmpty() || SeenAssets.Contains(Asset.Asset)
			|| !Object->HasTypedField<EJson::Array>(TEXT("labels")))
		{
			OutError = TEXT("D5 overlay asset paths must be non-empty and unique and labels must be arrays.");
			return false;
		}
		SeenAssets.Add(Asset.Asset);
		for (const TSharedPtr<FJsonValue>& LabelValue :
			Object->GetArrayField(TEXT("labels")))
		{
			if (!LabelValue.IsValid() || LabelValue->Type != EJson::Object)
			{
				OutError = TEXT("D5 overlay labels must be objects.");
				return false;
			}
			const TSharedPtr<FJsonObject> LabelObject = LabelValue->AsObject();
			bool bExpected = false;
			LabelObject->TryGetBoolField(TEXT("expected"), bExpected);
			FD5OverlayLabel Label;
			Label.Rule = ReadString(LabelObject, TEXT("rule"));
			Label.Severity = ReadString(LabelObject, TEXT("severity"));
			Label.Graph = ReadString(LabelObject, TEXT("graph"));
			Label.NodeType = ReadString(LabelObject, TEXT("nodeType"));
			Label.Function = ReadString(LabelObject, TEXT("function"));
			Label.Pin = ReadString(LabelObject, TEXT("pin"));
			Label.RuntimeStatus = ReadString(LabelObject, TEXT("runtimeStatus"));
			if (!bExpected || Label.Rule.IsEmpty()
				|| (Label.Severity != TEXT("medium")
					&& Label.Severity != TEXT("high")
					&& Label.Severity != TEXT("critical")))
			{
				OutError = TEXT("D5 overlay labels must be positive medium, high or critical semantic labels.");
				return false;
			}
			Asset.Labels.Add(MoveTemp(Label));
		}
		if (Asset.ReviewStatus == TEXT("retired")
			&& (!Asset.Labels.IsEmpty() || Asset.RetiredReason.IsEmpty()))
		{
			OutError = TEXT("Retired D5 overlay entries require a reason and cannot retain active labels.");
			return false;
		}
		OutAssets.Add(MoveTemp(Asset));
	}
	if (OutAssets.IsEmpty())
	{
		OutError = TEXT("D5 overlay must contain at least one reviewed asset.");
		return false;
	}
	return true;
}

bool LoadCorpus(
	TArray<FCorpusFixture>& OutFixtures,
	TSharedPtr<FJsonObject>& OutRoot,
	FString& OutError)
{
	const FString Path = CorpusPath();
	FString Text;
	if (Path.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(
			TEXT("Portable corpus is missing from '%s'."),
			*Path);
		return false;
	}
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, OutRoot)
		|| !OutRoot.IsValid()
		|| ReadString(OutRoot, TEXT("schema")) != TEXT("ue.blueprint-corpus.v1")
		|| !OutRoot->HasTypedField<EJson::Array>(TEXT("fixtures")))
	{
		OutError = TEXT("Portable corpus does not satisfy ue.blueprint-corpus.v1.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Values =
		OutRoot->GetArrayField(TEXT("fixtures"));
	if (Values.Num() < 32)
	{
		OutError = TEXT("Portable corpus must contain at least 32 Blueprint fixtures.");
		return false;
	}
	TSet<FString> Ids;
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutError = TEXT("Every corpus fixture must be an object.");
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		const TSharedPtr<FJsonObject>* Label = nullptr;
		if (!Object->TryGetObjectField(TEXT("label"), Label)
			|| !Label || !Label->IsValid())
		{
			OutError = TEXT("Every corpus fixture requires one stable semantic label.");
			return false;
		}
		FCorpusFixture Fixture;
		Fixture.Id = ReadString(Object, TEXT("id"));
		Fixture.Recipe = ReadString(Object, TEXT("recipe"));
		Fixture.Pair = ReadString(Object, TEXT("pair"));
		Fixture.Rule = ReadString(*Label, TEXT("rule"));
		Fixture.Severity = ReadString(*Label, TEXT("severity"));
		Fixture.Graph = ReadString(*Label, TEXT("graph"));
		Fixture.NodeType = ReadString(*Label, TEXT("nodeType"));
		Fixture.Function = ReadString(*Label, TEXT("function"));
		Fixture.Pin = ReadString(*Label, TEXT("pin"));
		Fixture.RuntimeStatus = ReadString(*Label, TEXT("runtimeStatus"));
		(*Label)->TryGetBoolField(TEXT("expected"), Fixture.bExpected);
		if (Fixture.Id.IsEmpty() || Fixture.Recipe.IsEmpty()
			|| Fixture.Rule.IsEmpty() || Ids.Contains(Fixture.Id))
		{
			OutError = TEXT("Corpus fixture ids, recipes and rules must be non-empty and unique.");
			return false;
		}
		Ids.Add(Fixture.Id);
		const FString ObjectName = TEXT("BP_")
			+ Fixture.Id.Replace(TEXT("-"), TEXT("_"));
		Fixture.PackageName = FString::Printf(
			TEXT("%s/%s"),
			CorpusRoot,
			*ObjectName);
		Fixture.ObjectPath = FString::Printf(
			TEXT("%s.%s"),
			*Fixture.PackageName,
			*ObjectName);
		OutFixtures.Add(MoveTemp(Fixture));
	}
	return true;
}

template <typename TNode>
TNode* AddNode(UEdGraph* Graph)
{
	TNode* Node = NewObject<TNode>(Graph);
	Graph->AddNode(Node, false, false);
	Node->CreateNewGuid();
	return Node;
}

UEdGraphPin* EnsureExecPin(
	UEdGraphNode* Node,
	const EEdGraphPinDirection Direction,
	const FName Name)
{
	if (UEdGraphPin* Existing = Node ? Node->FindPin(Name) : nullptr)
	{
		return Existing;
	}
	return Node
		? Node->CreatePin(Direction, UEdGraphSchema_K2::PC_Exec, Name)
		: nullptr;
}

void LinkCorpusExec(UEdGraphPin* Output, UEdGraphPin* Input)
{
	if (Output && Input)
	{
		Output->MakeLinkTo(Input);
	}
}

UK2Node_Event* AddEvent(UEdGraph* Graph, const FName FunctionName)
{
	UK2Node_Event* Event = AddNode<UK2Node_Event>(Graph);
	Event->EventReference.SetExternalMember(FunctionName, AActor::StaticClass());
	Event->bOverrideFunction = true;
	Event->AllocateDefaultPins();
	EnsureExecPin(Event, EGPD_Output, UEdGraphSchema_K2::PN_Then);
	return Event;
}

UK2Node_CallFunction* AddResolvedCall(
	UEdGraph* Graph,
	UClass* Owner,
	const FName FunctionName)
{
	UK2Node_CallFunction* Call = AddNode<UK2Node_CallFunction>(Graph);
	if (UFunction* Function = Owner ? Owner->FindFunctionByName(FunctionName) : nullptr)
	{
		Call->FunctionReference.SetFromField<UFunction>(Function, false);
	}
	Call->AllocateDefaultPins();
	EnsureExecPin(Call, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
	EnsureExecPin(Call, EGPD_Output, UEdGraphSchema_K2::PN_Then);
	return Call;
}

UK2Node_CallFunction* AddUnresolvedCall(
	UEdGraph* Graph,
	const FName FunctionName)
{
	UK2Node_CallFunction* Call = AddNode<UK2Node_CallFunction>(Graph);
	Call->FunctionReference.SetSelfMember(FunctionName);
	Call->AllocateDefaultPins();
	EnsureExecPin(Call, EGPD_Input, UEdGraphSchema_K2::PN_Execute);
	EnsureExecPin(Call, EGPD_Output, UEdGraphSchema_K2::PN_Then);
	return Call;
}

UEdGraph* FindForLoopMacro()
{
	UBlueprint* StandardMacros = LoadObject<UBlueprint>(
		nullptr,
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"));
	if (!StandardMacros)
	{
		return nullptr;
	}
	TArray<UEdGraph*> Graphs;
	StandardMacros->GetAllGraphs(Graphs);
	for (UEdGraph* Graph : Graphs)
	{
		if (Graph && Graph->GetName().Contains(TEXT("ForLoop")))
		{
			return Graph;
		}
	}
	return nullptr;
}

bool PopulateFixture(FCorpusFixture& Fixture, FString& OutError)
{
	UPackage* Package = CreatePackage(*Fixture.PackageName);
	const FString AssetName = FPackageName::GetLongPackageAssetName(
		Fixture.PackageName);
	UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
	Factory->ParentClass = AActor::StaticClass();
	Fixture.Blueprint = Cast<UBlueprint>(
		Factory->FactoryCreateNew(
			UBlueprint::StaticClass(),
			Package,
			FName(*AssetName),
			RF_Public | RF_Standalone | RF_Transactional,
			nullptr,
			GWarn));
	if (!Fixture.Blueprint)
	{
		OutError = FString::Printf(TEXT("Could not create %s."), *Fixture.Id);
		return false;
	}
	FAssetRegistryModule::AssetCreated(Fixture.Blueprint);
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Fixture.Blueprint);
	if (!EventGraph)
	{
		OutError = FString::Printf(TEXT("%s has no EventGraph."), *Fixture.Id);
		return false;
	}
	// BlueprintFactory installs template events. Corpus labels describe only
	// nodes declared by each recipe, so remove the template graph content first.
	TArray<UEdGraphNode*> TemplateNodes = EventGraph->Nodes;
	for (UEdGraphNode* Node : TemplateNodes)
	{
		if (Node)
		{
			FBlueprintEditorUtils::RemoveNode(
				Fixture.Blueprint,
				Node,
				true);
		}
	}

	const bool bReachable = Fixture.Recipe.EndsWith(TEXT("-reachable"));
	const bool bTick = Fixture.Recipe.Contains(TEXT("tick"))
		|| bReachable
		|| Fixture.Recipe == TEXT("global-traversal-function");
	UK2Node_Event* Entry = nullptr;
	if (Fixture.Recipe == TEXT("begin-play"))
	{
		Entry = AddEvent(EventGraph, TEXT("ReceiveBeginPlay"));
	}
	else if (bTick)
	{
		Entry = AddEvent(EventGraph, TEXT("ReceiveTick"));
	}
	UEdGraphPin* EntryExec = Entry
		? EnsureExecPin(Entry, EGPD_Output, UEdGraphSchema_K2::PN_Then)
		: nullptr;

	if (Fixture.Recipe.StartsWith(TEXT("global-traversal"))
		|| Fixture.Recipe == TEXT("disconnected-output"))
	{
		UEdGraph* TargetGraph = EventGraph;
		UEdGraphPin* ConnectFrom = EntryExec;
		if (Fixture.Recipe == TEXT("global-traversal-function"))
		{
			TargetGraph = FBlueprintEditorUtils::CreateNewGraph(
				Fixture.Blueprint,
				TEXT("HotPath"),
				UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UFunction>(
				Fixture.Blueprint,
				TargetGraph,
				false,
				nullptr);
			UK2Node_CallFunction* LocalCall = AddUnresolvedCall(
				EventGraph,
				TEXT("HotPath"));
			LinkCorpusExec(
				EntryExec,
				EnsureExecPin(LocalCall, EGPD_Input, UEdGraphSchema_K2::PN_Execute));
			UK2Node_FunctionEntry* FunctionEntry = nullptr;
			for (UEdGraphNode* Node : TargetGraph->Nodes)
			{
				FunctionEntry = Cast<UK2Node_FunctionEntry>(Node);
				if (FunctionEntry)
				{
					break;
				}
			}
			ConnectFrom = FunctionEntry
				? EnsureExecPin(
					FunctionEntry,
					EGPD_Output,
					UEdGraphSchema_K2::PN_Then)
				: nullptr;
		}
		UK2Node_CallFunction* Traversal = AddResolvedCall(
			TargetGraph,
			UGameplayStatics::StaticClass(),
			GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetAllActorsOfClass));
		if (bReachable
			|| Fixture.Recipe == TEXT("global-traversal-function"))
		{
			LinkCorpusExec(
				ConnectFrom,
				EnsureExecPin(Traversal, EGPD_Input, UEdGraphSchema_K2::PN_Execute));
		}
	}
	else if (Fixture.Recipe.StartsWith(TEXT("sync-load")))
	{
		UK2Node_CallFunction* Call = AddResolvedCall(
			EventGraph,
			UKismetSystemLibrary::StaticClass(),
			GET_FUNCTION_NAME_CHECKED(
				UKismetSystemLibrary,
				LoadAsset_Blocking));
		if (bReachable)
		{
			LinkCorpusExec(
				EntryExec,
				EnsureExecPin(Call, EGPD_Input, UEdGraphSchema_K2::PN_Execute));
		}
	}
	else if (Fixture.Recipe.StartsWith(TEXT("unresolved-call")))
	{
		AddUnresolvedCall(
			EventGraph,
			Fixture.Recipe.EndsWith(TEXT("secondary"))
				? TEXT("MissingCorpusFunctionSecondary")
				: TEXT("MissingCorpusFunction"));
	}
	else if (Fixture.Recipe == TEXT("resolved-call"))
	{
		AddResolvedCall(
			EventGraph,
			UGameplayStatics::StaticClass(),
			GET_FUNCTION_NAME_CHECKED(UGameplayStatics, GetTimeSeconds));
	}
	else if (Fixture.Recipe.StartsWith(TEXT("dynamic-cast")))
	{
		UK2Node_DynamicCast* CastNode = AddNode<UK2Node_DynamicCast>(EventGraph);
		CastNode->TargetType = APawn::StaticClass();
		CastNode->AllocateDefaultPins();
		UK2Node_Self* SelfNode = AddNode<UK2Node_Self>(EventGraph);
		SelfNode->AllocateDefaultPins();
		if (UEdGraphPin* SelfOutput = SelfNode->FindPin(
			UEdGraphSchema_K2::PN_Self))
		{
			if (UEdGraphPin* ObjectInput = CastNode->GetCastSourcePin())
			{
				EventGraph->GetSchema()->TryCreateConnection(
					SelfOutput,
					ObjectInput);
			}
		}
		if (bReachable)
		{
			LinkCorpusExec(
				EntryExec,
				EnsureExecPin(CastNode, EGPD_Input, UEdGraphSchema_K2::PN_Execute));
		}
	}
	else if (Fixture.Recipe.StartsWith(TEXT("loop")))
	{
		UEdGraph* MacroGraph = FindForLoopMacro();
		if (!MacroGraph)
		{
			OutError = TEXT("The Engine StandardMacros ForLoop graph is unavailable.");
			return false;
		}
		UK2Node_MacroInstance* Loop = AddNode<UK2Node_MacroInstance>(EventGraph);
		Loop->SetMacroGraph(MacroGraph);
		Loop->AllocateDefaultPins();
		if (bReachable)
		{
			LinkCorpusExec(
				EntryExec,
				EnsureExecPin(Loop, EGPD_Input, UEdGraphSchema_K2::PN_Execute));
		}
	}
	else if (Fixture.Recipe.Contains(TEXT("delegate")))
	{
		const int32 Count = Fixture.Recipe.StartsWith(TEXT("duplicate")) ? 2 : 1;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			UK2Node_AddDelegate* Delegate = AddNode<UK2Node_AddDelegate>(EventGraph);
			Delegate->AllocateDefaultPins();
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Fixture.Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Fixture.Blueprint);
	Package->MarkPackageDirty();
	const FString Filename = FPackageName::LongPackageNameToFilename(
		Fixture.PackageName,
		FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(
			Package,
			Fixture.Blueprint,
			*Filename,
			SaveArgs)
		|| !IFileManager::Get().FileExists(*Filename))
	{
		OutError = FString::Printf(TEXT("Could not save real corpus package %s."), *Filename);
		return false;
	}
	Package->ClearPackageFlags(PKG_NewlyCreated);
	Package->SetDirtyFlag(false);
	return true;
}

bool HasRuleFinding(
	const FCorpusFixture& Fixture,
	const TArray<TSharedPtr<FJsonObject>>& Findings)
{
	for (const TSharedPtr<FJsonObject>& Finding : Findings)
	{
		if (!Finding.IsValid()
			|| ReadString(Finding, TEXT("ruleId")) != Fixture.Rule)
		{
			continue;
		}
		FString ExpectedSeverity = Fixture.Severity.ToLower();
		if (ExpectedSeverity == TEXT("error"))
		{
			ExpectedSeverity = TEXT("high");
		}
		else if (ExpectedSeverity == TEXT("warning"))
		{
			ExpectedSeverity = TEXT("medium");
		}
		if (!ExpectedSeverity.IsEmpty()
			&& ReadString(Finding, TEXT("severity")) != ExpectedSeverity)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Location = nullptr;
		Finding->TryGetObjectField(TEXT("location"), Location);
		if (!Fixture.Graph.IsEmpty()
			&& (!Location || !Location->IsValid()
				|| ReadString(*Location, TEXT("graph")) != Fixture.Graph))
		{
			continue;
		}
		FString RuntimeStatus = ReadString(Finding, TEXT("runtimeStatus"));
		if (RuntimeStatus.IsEmpty())
		{
			// A static scan has not yet been correlated with runtime evidence.
			RuntimeStatus = TEXT("hypothesis");
		}
		if (!Fixture.RuntimeStatus.IsEmpty()
			&& RuntimeStatus != Fixture.RuntimeStatus)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Evidence = nullptr;
		Finding->TryGetObjectField(TEXT("evidence"), Evidence);
		if (!Fixture.NodeType.IsEmpty()
			&& (!Evidence || !Evidence->IsValid()
				|| ReadString(*Evidence, TEXT("nodeClass")) != Fixture.NodeType))
		{
			continue;
		}
		if (!Fixture.Pin.IsEmpty()
			&& (!Evidence || !Evidence->IsValid()
				|| !ReadString(*Evidence, TEXT("pinName")).Equals(
					Fixture.Pin,
					ESearchCase::IgnoreCase)))
		{
			continue;
		}
		if (!Fixture.Function.IsEmpty())
		{
			if (!Evidence || !Evidence->IsValid())
			{
				continue;
			}
			const FString EvidenceFunction = ReadString(*Evidence, TEXT("function"));
			const FString Message = ReadString(Finding, TEXT("message"));
			if (!EvidenceFunction.Equals(Fixture.Function, ESearchCase::IgnoreCase)
				&& !Message.Contains(Fixture.Function, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}
		return true;
	}
	return false;
}

void WriteCorpusReport(
	const TArray<FCorpusFixture>& Fixtures,
	const TMap<FString, FRuleMetrics>& Metrics,
	const TArray<TSharedPtr<FJsonValue>>& Cases,
	const bool bPassed)
{
	const FString Directory = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration/Corpus/BlueprintAnalysis"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("ue.blueprint-corpus-result.v1"));
	Root->SetStringField(TEXT("corpusId"), TEXT("portable.blueprint-analysis"));
	Root->SetNumberField(TEXT("fixtureCount"), Fixtures.Num());
	Root->SetBoolField(TEXT("passed"), bPassed);
	Root->SetArrayField(TEXT("cases"), Cases);
	TArray<TSharedPtr<FJsonValue>> RuleValues;
	TArray<FString> Rules;
	Metrics.GetKeys(Rules);
	Rules.Sort();
	for (const FString& Rule : Rules)
	{
		const FRuleMetrics& Value = Metrics.FindChecked(Rule);
		const double Precision = Value.TruePositive + Value.FalsePositive > 0
			? static_cast<double>(Value.TruePositive)
				/ (Value.TruePositive + Value.FalsePositive)
			: 1.0;
		const double Recall = Value.TruePositive + Value.FalseNegative > 0
			? static_cast<double>(Value.TruePositive)
				/ (Value.TruePositive + Value.FalseNegative)
			: 1.0;
		TSharedRef<FJsonObject> RuleObject = MakeShared<FJsonObject>();
		RuleObject->SetStringField(TEXT("rule"), Rule);
		RuleObject->SetNumberField(TEXT("truePositive"), Value.TruePositive);
		RuleObject->SetNumberField(TEXT("falsePositive"), Value.FalsePositive);
		RuleObject->SetNumberField(TEXT("falseNegative"), Value.FalseNegative);
		RuleObject->SetNumberField(TEXT("trueNegative"), Value.TrueNegative);
		RuleObject->SetNumberField(TEXT("precision"), Precision);
		RuleObject->SetNumberField(TEXT("recall"), Recall);
		RuleValues.Add(MakeShared<FJsonValueObject>(RuleObject));
	}
	Root->SetArrayField(TEXT("rules"), RuleValues);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	FFileHelper::SaveStringToFile(
		Json,
		*FPaths::Combine(Directory, TEXT("result.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	int32 Failures = 0;
	FString TestCases;
	for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		const bool bCasePassed = Case->GetBoolField(TEXT("passed"));
		Failures += bCasePassed ? 0 : 1;
		TestCases += FString::Printf(
			TEXT("  <testcase classname=\"BlueprintCorpus\" name=\"%s\">%s</testcase>\n"),
			*ReadString(Case, TEXT("id")),
			bCasePassed ? TEXT("") : TEXT("<failure message=\"label mismatch\"/>") );
	}
	const FString JUnit = FString::Printf(
		TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"BlueprintCorpus\" tests=\"%d\" failures=\"%d\">\n%s</testsuite>\n"),
		Cases.Num(),
		Failures,
		*TestCases);
	FFileHelper::SaveStringToFile(
		JUnit,
		*FPaths::Combine(Directory, TEXT("junit.xml")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void WriteD5OverlayReport(
	const TArray<TSharedPtr<FJsonValue>>& Cases,
	const bool bPassed)
{
	const FString Directory = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("UE_AI_integration/Corpus/BlueprintAnalysis/D5Overlay"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("schema"), TEXT("ue.blueprint-corpus-result.v1"));
	Root->SetStringField(
		TEXT("corpusId"),
		TEXT("d5.fusion-effect-build.blueprint-analysis"));
	Root->SetStringField(TEXT("blockingScope"), TEXT("installation"));
	Root->SetBoolField(TEXT("passed"), bPassed);
	Root->SetArrayField(TEXT("cases"), Cases);
	FString Json;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	FFileHelper::SaveStringToFile(
		Json,
		*FPaths::Combine(Directory, TEXT("result.json")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	int32 Failures = 0;
	FString TestCases;
	for (const TSharedPtr<FJsonValue>& CaseValue : Cases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		const bool bCasePassed = Case->GetBoolField(TEXT("passed"));
		Failures += bCasePassed ? 0 : 1;
		TestCases += FString::Printf(
			TEXT("  <testcase classname=\"D5BlueprintCorpus\" name=\"%s\">%s</testcase>\n"),
			*ReadString(Case, TEXT("id")),
			bCasePassed ? TEXT("") : TEXT("<failure message=\"semantic label mismatch\"/>"));
	}
	const FString JUnit = FString::Printf(
		TEXT("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuite name=\"D5BlueprintCorpus\" tests=\"%d\" failures=\"%d\">\n%s</testsuite>\n"),
		Cases.Num(),
		Failures,
		*TestCases);
	FFileHelper::SaveStringToFile(
		JUnit,
		*FPaths::Combine(Directory, TEXT("junit.xml")),
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void CleanupFixtures(const TArray<FCorpusFixture>& Fixtures)
{
	TArray<UObject*> Assets;
	for (const FCorpusFixture& Fixture : Fixtures)
	{
		if (Fixture.Blueprint)
		{
			Assets.Add(Fixture.Blueprint);
		}
	}
	if (!Assets.IsEmpty())
	{
		ObjectTools::DeleteObjectsUnchecked(Assets);
	}
	for (const FCorpusFixture& Fixture : Fixtures)
	{
		const FString Filename = FPackageName::LongPackageNameToFilename(
			Fixture.PackageName,
			FPackageName::GetAssetPackageExtension());
		IFileManager::Get().Delete(*Filename, false, true, true);
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintAnalysisRealPackageCorpusTest,
	"UE_AI_integration.BlueprintAnalysis.GoldenCorpus.RealPackages",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAnalysisRealPackageCorpusTest::RunTest(const FString& Parameters)
{
	TArray<FCorpusFixture> Fixtures;
	TSharedPtr<FJsonObject> Corpus;
	FString Error;
	if (!LoadCorpus(Fixtures, Corpus, Error))
	{
		AddError(Error);
		return false;
	}

	bool bCreated = true;
	for (FCorpusFixture& Fixture : Fixtures)
	{
		if (Fixture.Recipe.StartsWith(TEXT("dependency-")))
		{
			// Dependency links are installed after both members of a pair exist.
			if (!PopulateFixture(Fixture, Error))
			{
				AddError(Error);
				bCreated = false;
				break;
			}
			continue;
		}
		if (!PopulateFixture(Fixture, Error))
		{
			AddError(Error);
			bCreated = false;
			break;
		}
	}
	if (!bCreated)
	{
		CleanupFixtures(Fixtures);
		return false;
	}

	// Add reciprocal or one-way UObject properties only after both generated
	// classes exist. Saving the real packages makes AssetRegistry dependency
	// edges observable to blueprint.scan.
	TMap<FString, TArray<FCorpusFixture*>> Pairs;
	for (FCorpusFixture& Fixture : Fixtures)
	{
		if (!Fixture.Pair.IsEmpty())
		{
			Pairs.FindOrAdd(Fixture.Pair).Add(&Fixture);
		}
	}
	for (TPair<FString, TArray<FCorpusFixture*>>& Pair : Pairs)
	{
		if (Pair.Value.Num() != 2)
		{
			AddError(TEXT("Every dependency corpus pair must have exactly two assets."));
			CleanupFixtures(Fixtures);
			return false;
		}
		FCorpusFixture* A = Pair.Value[0];
		FCorpusFixture* B = Pair.Value[1];
		FEdGraphPinType AToB;
		AToB.PinCategory = UEdGraphSchema_K2::PC_Object;
		AToB.PinSubCategoryObject = B->Blueprint->GeneratedClass;
		FBlueprintEditorUtils::AddMemberVariable(
			A->Blueprint,
			TEXT("CorpusDependency"),
			AToB);
		if (Pair.Key.Contains(TEXT("cycle")))
		{
			FEdGraphPinType BToA;
			BToA.PinCategory = UEdGraphSchema_K2::PC_Object;
			BToA.PinSubCategoryObject = A->Blueprint->GeneratedClass;
			FBlueprintEditorUtils::AddMemberVariable(
				B->Blueprint,
				TEXT("CorpusDependency"),
				BToA);
		}
		for (FCorpusFixture* Fixture : Pair.Value)
		{
			FKismetEditorUtilities::CompileBlueprint(Fixture->Blueprint);
			UPackage* Package = Fixture->Blueprint->GetOutermost();
			Package->MarkPackageDirty();
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Fixture->PackageName,
				FPackageName::GetAssetPackageExtension());
			UPackage::SavePackage(
				Package,
				Fixture->Blueprint,
				*Filename,
				SaveArgs);
			Package->SetDirtyFlag(false);
		}
	}
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
			.Get();
	TArray<FString> ScanPaths;
	ScanPaths.Add(CorpusRoot);
	AssetRegistry.ScanPathsSynchronous(ScanPaths, true);

	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintAnalysisTools(Registry);
	Registry.EndDomainRegistration();
	TSharedRef<FJsonObject> ScanParams = MakeShared<FJsonObject>();
	ScanParams->SetStringField(TEXT("pathPrefix"), CorpusRoot);
	ScanParams->SetNumberField(TEXT("assetLimit"), Fixtures.Num() + 8);
	ScanParams->SetNumberField(TEXT("findingLimit"), 1000);
	const FMCPToolResult FullScan = Registry.ExecuteTool(
		TEXT("blueprint.scan"),
		ScanParams);
	if (!FullScan.bSuccess || !FullScan.Data.IsValid())
	{
		AddError(FString::Printf(
			TEXT("Corpus blueprint.scan failed: %s"),
			*FullScan.ErrorMessage));
		CleanupFixtures(Fixtures);
		return false;
	}
	TMap<FString, TArray<TSharedPtr<FJsonObject>>> FindingsByAsset;
	for (const TSharedPtr<FJsonValue>& Value :
		FullScan.Data->GetArrayField(TEXT("findings")))
	{
		const TSharedPtr<FJsonObject> Finding = Value->AsObject();
		const TSharedPtr<FJsonObject>* Location = nullptr;
		if (Finding.IsValid()
			&& Finding->TryGetObjectField(TEXT("location"), Location)
			&& Location && Location->IsValid())
		{
			FindingsByAsset.FindOrAdd(
				ReadString(*Location, TEXT("assetPath"))).Add(Finding);
		}
	}

	TMap<FString, FRuleMetrics> Metrics;
	TArray<TSharedPtr<FJsonValue>> Cases;
	bool bPassed = true;
	for (const FCorpusFixture& Fixture : Fixtures)
	{
		TArray<TSharedPtr<FJsonObject>> LocalFindings =
			ScanBlueprintForTesting(Fixture.Blueprint, Fixture.PackageName);
		if (const TArray<TSharedPtr<FJsonObject>>* FullFindings =
			FindingsByAsset.Find(Fixture.PackageName))
		{
			for (const TSharedPtr<FJsonObject>& Finding : *FullFindings)
			{
				if (ReadString(Finding, TEXT("ruleId"))
					== TEXT("blueprint.dependency.cycle"))
				{
					LocalFindings.Add(Finding);
				}
			}
		}
		const bool bFound = HasRuleFinding(Fixture, LocalFindings);
		const bool bCasePassed = bFound == Fixture.bExpected;
		FRuleMetrics& Rule = Metrics.FindOrAdd(Fixture.Rule);
		if (Fixture.bExpected && bFound) ++Rule.TruePositive;
		else if (!Fixture.bExpected && bFound) ++Rule.FalsePositive;
		else if (Fixture.bExpected) ++Rule.FalseNegative;
		else ++Rule.TrueNegative;
		TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
		Case->SetStringField(TEXT("id"), Fixture.Id);
		Case->SetStringField(TEXT("asset"), Fixture.PackageName);
		Case->SetStringField(TEXT("rule"), Fixture.Rule);
		Case->SetBoolField(TEXT("expected"), Fixture.bExpected);
		Case->SetBoolField(TEXT("observed"), bFound);
		Case->SetBoolField(TEXT("passed"), bCasePassed);
		Cases.Add(MakeShared<FJsonValueObject>(Case));
		if (!bCasePassed)
		{
			bPassed = false;
			AddError(FString::Printf(
				TEXT("Corpus label mismatch: %s expected %s for %s."),
				*Fixture.Id,
				Fixture.bExpected ? TEXT("finding") : TEXT("no finding"),
				*Fixture.Rule));
		}
	}

	for (const TPair<FString, FRuleMetrics>& Pair : Metrics)
	{
		const FRuleMetrics& Value = Pair.Value;
		const double Precision = Value.TruePositive + Value.FalsePositive > 0
			? static_cast<double>(Value.TruePositive)
				/ (Value.TruePositive + Value.FalsePositive)
			: 1.0;
		const double Recall = Value.TruePositive + Value.FalseNegative > 0
			? static_cast<double>(Value.TruePositive)
				/ (Value.TruePositive + Value.FalseNegative)
			: 1.0;
		const bool bHighOrError = Pair.Key == TEXT("blueprint.dependency.cycle")
			|| Pair.Key == TEXT("blueprint.reference.unresolved_function")
			|| Pair.Key == TEXT("blueprint.call.global_traversal")
			|| Pair.Key == TEXT("blueprint.call.synchronous_load");
		const double Required = bHighOrError ? 1.0 : 0.9;
		if (Precision + KINDA_SMALL_NUMBER < Required
			|| Recall + KINDA_SMALL_NUMBER < Required)
		{
			bPassed = false;
			AddError(FString::Printf(
				TEXT("Rule %s precision %.3f recall %.3f is below %.3f."),
				*Pair.Key,
				Precision,
				Recall,
				Required));
		}
	}

	WriteCorpusReport(Fixtures, Metrics, Cases, bPassed);
	CleanupFixtures(Fixtures);
	TestTrue(TEXT("Portable real-package corpus passes its release thresholds"), bPassed);
	return bPassed;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBlueprintAnalysisD5OverlayCorpusTest,
	"UE_AI_integration.BlueprintAnalysis.GoldenCorpus.D5Overlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAnalysisD5OverlayCorpusTest::RunTest(const FString& Parameters)
{
	TArray<FD5OverlayAsset> Assets;
	FString Error;
	if (!LoadD5Overlay(Assets, Error))
	{
		AddError(Error);
		return false;
	}

	int32 ExistingAssetCount = 0;
	for (const FD5OverlayAsset& Asset : Assets)
	{
		ExistingAssetCount += FPackageName::DoesPackageExist(Asset.Asset) ? 1 : 0;
	}
	if (ExistingAssetCount == 0)
	{
		AddInfo(TEXT("FusionEffectBuild overlay assets are absent; the project installation gate is not applicable to this host."));
		return true;
	}
	FMCPToolRegistry Registry;
	Registry.BeginDomainRegistration(TEXT("blueprint"));
	UEAIIntegrationTools::RegisterBlueprintAnalysisTools(Registry);
	Registry.EndDomainRegistration();

	TArray<TSharedPtr<FJsonValue>> Cases;
	bool bPassed = true;
	for (const FD5OverlayAsset& Asset : Assets)
	{
		const bool bExists = FPackageName::DoesPackageExist(Asset.Asset);
		if (Asset.ReviewStatus == TEXT("retired"))
		{
			if (bExists)
			{
				bPassed = false;
				AddError(FString::Printf(
					TEXT("corpus_label_stale: retired D5 asset %s exists again and requires review."),
					*Asset.Asset));
			}
			continue;
		}
		if (Asset.ReviewStatus != TEXT("reviewed"))
		{
			bPassed = false;
			AddError(FString::Printf(
				TEXT("corpus_label_stale: %s has not been explicitly reviewed."),
				*Asset.Asset));
			continue;
		}
		if (!bExists)
		{
			bPassed = false;
			AddError(FString::Printf(
				TEXT("corpus_label_stale: reviewed D5 asset %s is missing."),
				*Asset.Asset));
			continue;
		}

		TSet<FString> ExpectedKeys;
		for (const FD5OverlayLabel& Label : Asset.Labels)
		{
			const FString Key = OverlayLabelKey(Asset.Asset, Label);
			if (ExpectedKeys.Contains(Key))
			{
				bPassed = false;
				AddError(FString::Printf(
					TEXT("corpus_label_stale: duplicate D5 overlay label '%s'."),
					*Key));
			}
			ExpectedKeys.Add(Key);
		}

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset"), Asset.Asset);
		Params->SetNumberField(TEXT("findingLimit"), 1000);
		TArray<TSharedPtr<FJsonValue>> Severity;
		Severity.Add(MakeShared<FJsonValueString>(TEXT("medium")));
		Severity.Add(MakeShared<FJsonValueString>(TEXT("high")));
		Severity.Add(MakeShared<FJsonValueString>(TEXT("critical")));
		Params->SetArrayField(TEXT("severity"), Severity);
		const FMCPToolResult Scan = Registry.ExecuteTool(
			TEXT("blueprint.scan"),
			Params);
		if (!Scan.bSuccess || !Scan.Data.IsValid())
		{
			bPassed = false;
			AddError(FString::Printf(
				TEXT("D5 corpus scan failed for %s: %s"),
				*Asset.Asset,
				*Scan.ErrorMessage));
			continue;
		}

		TMap<FString, TSharedPtr<FJsonObject>> ObservedByKey;
		TSet<FString> AmbiguousKeys;
		if (Scan.Data->HasTypedField<EJson::Array>(TEXT("findings")))
		{
			for (const TSharedPtr<FJsonValue>& FindingValue :
				Scan.Data->GetArrayField(TEXT("findings")))
			{
				if (!FindingValue.IsValid() || FindingValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Finding = FindingValue->AsObject();
				const FString Key = OverlayFindingKey(Asset.Asset, Finding);
				if (ObservedByKey.Contains(Key))
				{
					AmbiguousKeys.Add(Key);
				}
				ObservedByKey.Add(Key, Finding);
			}
		}

		for (const FString& Key : ExpectedKeys)
		{
			const bool bFound = ObservedByKey.Contains(Key)
				&& !AmbiguousKeys.Contains(Key);
			TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
			Case->SetStringField(TEXT("id"), TEXT("expected:") + Key);
			Case->SetStringField(TEXT("asset"), Asset.Asset);
			Case->SetStringField(TEXT("semanticKey"), Key);
			Case->SetStringField(
				TEXT("outcome"),
				bFound ? TEXT("truePositive") : TEXT("falseNegative"));
			Case->SetBoolField(TEXT("passed"), bFound);
			Cases.Add(MakeShared<FJsonValueObject>(Case));
			if (!bFound)
			{
				bPassed = false;
				AddError(FString::Printf(
					TEXT("D5 corpus expected finding is missing or semantically ambiguous: %s"),
					*Key));
			}
		}

		for (const TPair<FString, TSharedPtr<FJsonObject>>& Pair : ObservedByKey)
		{
			if (ExpectedKeys.Contains(Pair.Key)
				&& !AmbiguousKeys.Contains(Pair.Key))
			{
				continue;
			}
			TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
			Case->SetStringField(TEXT("id"), TEXT("unexpected:") + Pair.Key);
			Case->SetStringField(TEXT("asset"), Asset.Asset);
			Case->SetStringField(TEXT("semanticKey"), Pair.Key);
			const TSharedPtr<FJsonObject>* Evidence = nullptr;
			Pair.Value->TryGetObjectField(TEXT("evidence"), Evidence);
			Case->SetStringField(
				TEXT("nodeId"),
				Evidence && Evidence->IsValid()
					? ReadString(*Evidence, TEXT("nodeId"))
					: FString());
			Case->SetStringField(
				TEXT("findingId"),
				ReadString(Pair.Value, TEXT("findingId")));
			Case->SetStringField(
				TEXT("outcome"),
				AmbiguousKeys.Contains(Pair.Key)
					? TEXT("ambiguous")
					: TEXT("falsePositive"));
			Case->SetBoolField(TEXT("passed"), false);
			Cases.Add(MakeShared<FJsonValueObject>(Case));
			bPassed = false;
			AddError(FString::Printf(
				TEXT("D5 corpus contains an unlabelled or ambiguous blocking finding: %s"),
				*Pair.Key));
		}

		if (ExpectedKeys.IsEmpty() && ObservedByKey.IsEmpty())
		{
			TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
			Case->SetStringField(TEXT("id"), TEXT("clean:") + Asset.Asset);
			Case->SetStringField(TEXT("asset"), Asset.Asset);
			Case->SetStringField(TEXT("outcome"), TEXT("trueNegative"));
			Case->SetBoolField(TEXT("passed"), true);
			Cases.Add(MakeShared<FJsonValueObject>(Case));
		}
	}

	WriteD5OverlayReport(Cases, bPassed);
	TestTrue(TEXT("D5 reviewed Blueprint overlay matches high/medium findings"), bPassed);
	return bPassed;
}

#endif
