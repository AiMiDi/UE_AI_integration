#include "Tools/MCPToolRegistry.h"

#include "Infrastructure/Compatibility/UEVersionCompat.h"

#if WITH_UEAI_PCG_GRAPH
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "Factories/Factory.h"
#include "HAL/FileManager.h"
#include "Infrastructure/DomainChangePlan.h"
#include "Infrastructure/MCPToolHelpers.h"
#include "Infrastructure/Sha256.h"
#include "Interfaces/IPluginManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PCGComponent.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGGraphSettings.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "ScopedTransaction.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/PackageReload.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#endif

namespace UEAIIntegrationTools
{
namespace
{
enum class EGraphOperation
{
	List,
	Get,
	Create,
	Prepare,
	Plan,
	Apply,
	Rollback,
	Save
};

#if WITH_UEAI_PCG_GRAPH
using FJson = TSharedPtr<FJsonObject>;
using FValues = TArray<TSharedPtr<FJsonValue>>;
using namespace UEAIIntegration::Infrastructure;

constexpr int32 MaxNodes = 4096;
constexpr int32 MaxEdges = 16384;
constexpr int32 MaxPins = 32768;
constexpr int32 MaxContextActions = 4096;
constexpr int32 MaxReceipts = 256;
constexpr int32 MaxNodeChanges = 32;
constexpr int64 MaxSnapshotBytes = 32 * 1024 * 1024;

FMCPToolResult Error(const FString& Message, const TCHAR* Code = TEXT("pcg_graph_invalid"))
{
	return FMCPToolResult::Error(Message, Code, 409);
}

FString Field(const FJson& Object, const TCHAR* Name)
{
	FString Value;
	Object->TryGetStringField(Name, Value);
	return Value;
}

FString NewId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FJson Page(const FJson& Params, const TCHAR* FieldName, const FValues& Values)
{
	double OffsetValue = 0;
	double LimitValue = 100;
	Params->TryGetNumberField(TEXT("offset"), OffsetValue);
	Params->TryGetNumberField(TEXT("limit"), LimitValue);
	const int32 Offset = FMath::Clamp(static_cast<int32>(OffsetValue), 0, Values.Num());
	const int32 Count = FMath::Min(FMath::Clamp(static_cast<int32>(LimitValue), 1, 200), Values.Num() - Offset);
	FValues Items;
	for (int32 Index = Offset; Index < Offset + Count; ++Index)
	{
		Items.Add(Values[Index]);
	}

	FJson Result = MakeShared<FJsonObject>();
	Result->SetArrayField(FieldName, MoveTemp(Items));
	Result->SetNumberField(TEXT("total"), Values.Num());
	Result->SetNumberField(TEXT("offset"), Offset);
	Result->SetNumberField(TEXT("count"), Count);
	Result->SetBoolField(TEXT("hasMore"), Offset + Count < Values.Num());
	if (Offset + Count < Values.Num())
	{
		Result->SetNumberField(TEXT("nextOffset"), Offset + Count);
	}
	return Result;
}

struct FEndpoint
{
	FString Node;
	FString Pin;

	bool operator==(const FEndpoint& Other) const = default;

	friend uint32 GetTypeHash(const FEndpoint& Endpoint)
	{
		return HashCombine(GetTypeHash(Endpoint.Node), GetTypeHash(Endpoint.Pin));
	}
};

struct FEdge
{
	FEndpoint From;
	FEndpoint To;

	bool operator==(const FEdge& Other) const = default;

	friend uint32 GetTypeHash(const FEdge& Edge)
	{
		return HashCombine(GetTypeHash(Edge.From), GetTypeHash(Edge.To));
	}
};

FJson EndpointJson(const FEndpoint& Endpoint)
{
	FJson Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("nodeId"), Endpoint.Node);
	Result->SetStringField(TEXT("pin"), Endpoint.Pin);
	return Result;
}

FValues EdgeValues(const TSet<FEdge>& Edges)
{
	TArray<FEdge> Sorted = Edges.Array();
	Sorted.Sort([](const FEdge& A, const FEdge& B)
	{
		return Tie(A.From.Node, A.From.Pin, A.To.Node, A.To.Pin) < Tie(B.From.Node, B.From.Pin, B.To.Node, B.To.Pin);
	});
	FValues Values;
	for (const FEdge& Edge : Sorted)
	{
		FJson Object = MakeShared<FJsonObject>();
		Object->SetObjectField(TEXT("from"), EndpointJson(Edge.From));
		Object->SetObjectField(TEXT("to"), EndpointJson(Edge.To));
		Values.Add(MakeShared<FJsonValueObject>(Object));
	}
	return Values;
}

struct FGraphState
{
	TMap<FString, UPCGNode*> Nodes;
	TSet<FEdge> Edges;
	FValues NodeValues;
	FString Hash;
	FString ReadError;
};

bool ReadGraph(UPCGGraph* Graph, FGraphState& State)
{
	State = FGraphState();
	TArray<UPCGNode*> Nodes = Graph->GetNodes();
	Nodes.Add(Graph->GetInputNode());
	Nodes.Add(Graph->GetOutputNode());
	if (Nodes.Num() > MaxNodes || Nodes.Contains(nullptr))
	{
		return false;
	}
	Nodes.Sort([](const UPCGNode& A, const UPCGNode& B) { return A.GetName() < B.GetName(); });
	TSet<const UPCGNode*> Membership;
	for (UPCGNode* Node : Nodes)
	{
		if (!IsValid(Node) || Node->GetGraph() != Graph || Membership.Contains(Node))
		{
			return false;
		}
		Membership.Add(Node);
	}
	int32 PinCount = 0;
	int32 RemainingSettingsChars = 2 * 1024 * 1024;
	for (UPCGNode* Node : Nodes)
	{
		const FString Id = Node->GetName();
		State.Nodes.Add(Id, Node);
		FJson NodeJson = MakeShared<FJsonObject>();
		NodeJson->SetStringField(TEXT("nodeId"), Id);
		NodeJson->SetStringField(TEXT("nodeClass"), Node->GetClass()->GetPathName());
		const TCHAR* Role = TEXT("node");
		if (Node == Graph->GetInputNode())
		{
			Role = TEXT("input");
		}
		else if (Node == Graph->GetOutputNode())
		{
			Role = TEXT("output");
		}
		NodeJson->SetStringField(TEXT("role"), Role);
		if (const UPCGSettings* Settings = Node->GetSettings())
		{
			NodeJson->SetStringField(TEXT("settingsClass"), Settings->GetClass()->GetPathName());
			NodeJson->SetStringField(TEXT("title"), Settings->GetDefaultNodeTitle().ToString());
			FJson Values;
			if (!PCGGraphSettings::Read(*Settings, Values, RemainingSettingsChars))
			{
				State.ReadError = TEXT("Complete Settings readback exceeds the field, depth, path or size limit on node ") + Id;
				return false;
			}
			if (Values)
			{
				NodeJson->SetObjectField(TEXT("settingsValues"), MoveTemp(Values));
			}
		}
		if (const UPCGSettingsInterface* SettingsInterface = Node->GetSettingsInterface())
		{
			NodeJson->SetStringField(TEXT("settingsPath"), SettingsInterface->GetPathName());
		}

		int32 X = 0;
		int32 Y = 0;
		Node->GetNodePosition(X, Y);
		NodeJson->SetNumberField(TEXT("posX"), X);
		NodeJson->SetNumberField(TEXT("posY"), Y);
		for (const bool bOutput : {false, true})
		{
			FValues Pins;
			TSet<FName> Labels;
			for (const UPCGPin* Pin : bOutput ? Node->GetOutputPins() : Node->GetInputPins())
			{
				if (!IsValid(Pin) || Pin->Node != Node || Labels.Contains(Pin->Properties.Label) || ++PinCount > MaxPins)
				{
					return false;
				}
				Labels.Add(Pin->Properties.Label);
				FJson PinJson = MakeShared<FJsonObject>();
				PinJson->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
				PinJson->SetNumberField(TEXT("allowedTypes"), static_cast<uint32>(Pin->Properties.AllowedTypes));
				PinJson->SetNumberField(TEXT("currentTypes"), static_cast<uint32>(Pin->GetCurrentTypes()));
				PinJson->SetBoolField(TEXT("allowsMultipleConnections"), Pin->AllowsMultipleConnections());
				Pins.Add(MakeShared<FJsonValueObject>(PinJson));
				for (const UPCGEdge* Edge : Pin->Edges)
				{
					if (!Edge || !Edge->InputPin || !Edge->OutputPin
						|| (bOutput ? Edge->InputPin : Edge->OutputPin) != Pin
						|| !Membership.Contains(Edge->InputPin->Node.Get())
						|| !Membership.Contains(Edge->OutputPin->Node.Get())
						|| !Edge->InputPin->Edges.Contains(Edge) || !Edge->OutputPin->Edges.Contains(Edge))
					{
						return false;
					}
					if (bOutput)
					{
						const FEdge Key{{Id, Pin->Properties.Label.ToString()},
							{Edge->OutputPin->Node->GetName(), Edge->OutputPin->Properties.Label.ToString()}};
						if (State.Edges.Contains(Key) || State.Edges.Num() >= MaxEdges)
						{
							return false;
						}
						State.Edges.Add(Key);
					}
				}
			}
			NodeJson->SetArrayField(bOutput ? TEXT("outputs") : TEXT("inputs"), MoveTemp(Pins));
		}
		State.NodeValues.Add(MakeShared<FJsonValueObject>(NodeJson));
	}

	FJson Digest = MakeShared<FJsonObject>();
	Digest->SetStringField(TEXT("graphPath"), Graph->GetPathName());
	Digest->SetArrayField(TEXT("nodes"), State.NodeValues);
	Digest->SetArrayField(TEXT("edges"), EdgeValues(State.Edges));
	return TryDigestJson(Digest, State.Hash);
}

bool WritablePackage(const FString& PackageName, FString& Filename)
{
	if (!FPackageName::IsValidLongPackageName(PackageName)
		|| !FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, FPackageName::GetAssetPackageExtension()))
	{
		return false;
	}
	const FString FullFilename = FPaths::ConvertRelativePathToFull(Filename);
	if (PackageName.StartsWith(TEXT("/Game/"))
		&& FPaths::IsUnderDirectory(FullFilename, FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
	{
		return true;
	}
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project && Plugin->CanContainContent()
			&& PackageName.StartsWith(Plugin->GetMountedAssetPath())
			&& FPaths::IsUnderDirectory(FullFilename, FPaths::ConvertRelativePathToFull(Plugin->GetContentDir())))
		{
			return true;
		}
	}
	return false;
}

bool IsWritable(UPCGGraph* Graph)
{
	FString Filename;
	return Graph && Graph->IsAsset() && !Graph->GetOutermost()->HasAnyPackageFlags(PKG_Cooked)
		&& WritablePackage(Graph->GetOutermost()->GetName(), Filename)
		&& !IFileManager::Get().IsReadOnly(*Filename);
}

UPCGGraph* LoadGraph(const FString& Path)
{
	FString Package;
	FString Object;
	if (!FPackageName::IsValidObjectPath(Path) || !Path.Split(TEXT("."), &Package, &Object)
		|| Object.IsEmpty() || Object.Contains(TEXT(":")))
	{
		return nullptr;
	}
	UPCGGraph* Graph = LoadObject<UPCGGraph>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_NoRedirects);
	if (Graph && Graph->IsAsset() && Graph->GetPathName().Equals(Path, ESearchCase::IgnoreCase))
	{
		return Graph;
	}
	return nullptr;
}

UObject* ReadObjectProperty(UObject* Object, const TCHAR* Name)
{
	const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name);
	return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
}

UEdGraph* FindEditorGraph(UPCGGraph* Graph)
{
	TArray<UObject*> Children;
	GetObjectsWithOuter(Graph, Children, false);
	UEdGraph* Result = nullptr;
	for (UObject* Child : Children)
	{
		UEdGraph* Candidate = Cast<UEdGraph>(Child);
		if (!IsValid(Candidate) || Candidate->GetClass()->GetPathName() != TEXT("/Script/PCGEditor.PCGEditorGraph")
			|| ReadObjectProperty(Candidate, TEXT("PCGGraph")) != Graph)
		{
			continue;
		}
		const UEdGraphSchema* Schema = Candidate->GetSchema();
		if (Result || !Schema || Schema->GetClass()->GetPathName() != TEXT("/Script/PCGEditor.PCGEditorGraphSchema"))
		{
			return nullptr;
		}
		Result = Candidate;
	}
	return Result;
}

bool ReadEditorNodes(UEdGraph* EditorGraph, const FGraphState& State, TMap<FString, UEdGraphNode*>& Nodes)
{
	for (UEdGraphNode* View : EditorGraph->Nodes)
	{
		UPCGNode* Node = View ? Cast<UPCGNode>(ReadObjectProperty(View, TEXT("PCGNode"))) : nullptr;
		if (!Node)
		{
			continue;
		}
		const FString Id = Node->GetName();
		if (State.Nodes.FindRef(Id) != Node || Nodes.Contains(Id))
		{
			return false;
		}
		Nodes.Add(Id, View);
	}
	return Nodes.Num() == State.Nodes.Num();
}

UEdGraphPin* FindEditorPin(const TMap<FString, UEdGraphNode*>& Nodes, const FEndpoint& Endpoint, EEdGraphPinDirection Direction)
{
	UEdGraphNode* Node = Nodes.FindRef(Endpoint.Node);
	return Node ? Node->FindPin(FName(*Endpoint.Pin), Direction) : nullptr;
}

bool MatchesEditor(UEdGraph* EditorGraph, const FGraphState& State)
{
	TMap<FString, UEdGraphNode*> Nodes;
	if (!ReadEditorNodes(EditorGraph, State, Nodes))
	{
		return false;
	}

	TMap<const UEdGraphNode*, FString> Ids;
	for (const auto& Pair : Nodes)
	{
		Ids.Add(Pair.Value, Pair.Key);
	}

	TSet<FEdge> Visible;
	for (const auto& Pair : Nodes)
	{
		for (const UEdGraphPin* Pin : Pair.Value->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Link : Pin->LinkedTo)
			{
				const FString* Target = Link ? Ids.Find(Link->GetOwningNode()) : nullptr;
				if (!Target || Link->Direction != EGPD_Input || !Link->LinkedTo.Contains(Pin))
				{
					return false;
				}
				const FEdge Edge{{Pair.Key, Pin->PinName.ToString()}, {*Target, Link->PinName.ToString()}};
				if (!State.Edges.Contains(Edge) || Visible.Contains(Edge))
				{
					return false;
				}
				Visible.Add(Edge);
			}
		}
	}
	for (const FEdge& Edge : State.Edges)
	{
		// PCG keeps some authored edges on intentionally hidden editor pins.
		if (FindEditorPin(Nodes, Edge.From, EGPD_Output) && FindEditorPin(Nodes, Edge.To, EGPD_Input)
			&& !Visible.Contains(Edge))
		{
			return false;
		}
	}
	return true;
}

bool SameEdges(const TSet<FEdge>& A, const TSet<FEdge>& B)
{
	if (A.Num() != B.Num())
	{
		return false;
	}
	for (const FEdge& Edge : A)
	{
		if (!B.Contains(Edge))
		{
			return false;
		}
	}
	return true;
}

bool WouldCycle(const TSet<FEdge>& Edges, const FEdge& Link)
{
	TMultiMap<FString, FString> Adjacency;
	for (const FEdge& Edge : Edges)
	{
		Adjacency.Add(Edge.From.Node, Edge.To.Node);
	}
	TArray<FString> Pending{Link.To.Node};
	TSet<FString> Visited;
	while (!Pending.IsEmpty())
	{
		const FString Node = Pending.Pop(EAllowShrinking::No);
		if (Node == Link.From.Node)
		{
			return true;
		}
		if (!Visited.Contains(Node))
		{
			Visited.Add(Node);
			Adjacency.MultiFind(Node, Pending);
		}
	}
	return false;
}

FValues AffectedComponents(UPCGGraph* Graph, bool& bTruncated)
{
	FValues Items;
	bTruncated = false;
	const UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	for (TObjectIterator<UPCGComponent> It; It; ++It)
	{
		const UPCGComponent* Component = *It;
		if (!IsValid(Component) || !World || Component->GetWorld() != World
			|| Component->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}
		const UPCGGraph* Bound = Component->GetGraph();
		if (Bound && (Bound == Graph || Bound->Contains(Graph)))
		{
			if (Items.Num() >= 128)
			{
				bTruncated = true;
				break;
			}
			Items.Add(MakeShared<FJsonValueString>(Component->GetPathName()));
		}
	}
	Items.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return A->AsString() < B->AsString();
	});
	return Items;
}

struct FSnapshotObject
{
	TStrongObjectPtr<UObject> Object;
	TArray<uint8> Bytes;
};

struct FSnapshot
{
	TArray<FSnapshotObject> Objects;
	FString Digest;
	int64 Size = 0;
	bool bWasDirty = false;

	bool Capture(UPCGGraph* Graph)
	{
		TArray<UObject*> Owned;
		GetObjectsWithOuter(Graph, Owned);
		Owned.RemoveAll([](const UObject* Object) { return !IsValid(Object); });
		Owned.Add(Graph);
		if (Owned.Num() > MaxNodes + MaxEdges + MaxPins + MaxNodes)
		{
			return false;
		}
		Owned.Sort([](const UObject& A, const UObject& B) { return A.GetPathName() < B.GetPathName(); });
		FJson Fingerprint = MakeShared<FJsonObject>();
		for (UObject* Object : Owned)
		{
			FSnapshotObject Entry;
			Entry.Object.Reset(Object);
			FObjectWriter Writer(Object, Entry.Bytes, false, false, false, PPF_DuplicateVerbatim);
			Size += Entry.Bytes.Num();
			FString ObjectHash;
			if (Writer.IsError() || Size > MaxSnapshotBytes || !TrySha256Hex(Entry.Bytes.GetData(), Entry.Bytes.Num(), ObjectHash))
			{
				return false;
			}
			Fingerprint->SetStringField(Object->GetPathName(), ObjectHash);
			Objects.Add(MoveTemp(Entry));
		}
		bWasDirty = Graph->GetOutermost()->IsDirty();
		return TryDigestJson(Fingerprint, Digest);
	}

	bool Restore(UPCGGraph* Graph, UEdGraph* EditorGraph, const TArray<TWeakObjectPtr<UPCGNode>>& Added,
		const TMap<TWeakObjectPtr<UPCGSettings>, TArray<FString>>& EditedSettings, bool bDirty) const
	{
		for (const FSnapshotObject& Entry : Objects)
		{
			if (!IsValid(Entry.Object.Get()))
			{
				return false;
			}
			Entry.Object->Modify();
		}

		TArray<UPCGNode*> ToRemove;
		for (const TWeakObjectPtr<UPCGNode>& Node : Added)
		{
			if (Node.IsValid() && Graph->GetNodes().Contains(Node.Get()))
			{
				ToRemove.Add(Node.Get());
			}
		}
		Graph->RemoveNodes(ToRemove);

		bool bRestored = true;
		for (const FSnapshotObject& Entry : Objects)
		{
			Entry.Object->PreEditUndo();
		}
		for (const FSnapshotObject& Entry : Objects)
		{
			FObjectReader Reader(Entry.Bytes);
			Reader.SetPortFlags(PPF_DuplicateVerbatim);
			Entry.Object->Serialize(Reader);
			bRestored = bRestored && !Reader.IsError();
		}
		UEdGraphPin::ResolveAllPinReferences();
		for (const FSnapshotObject& Entry : Objects)
		{
			Entry.Object->PostEditUndo();
		}
		for (const auto& Entry : EditedSettings)
		{
			if (UPCGSettings* Settings = Entry.Key.Get())
			{
				const UPCGNode* Node = Cast<UPCGNode>(Settings->GetOuter());
				if (Node && Graph->GetNodes().Contains(Node) && Node->GetSettingsInterface() == Settings)
				{
					bRestored = PCGGraphSettings::RefreshAfterRestore(*Settings, Entry.Value) && bRestored;
				}
			}
		}
		EditorGraph->NotifyGraphChanged();
		Graph->GetOutermost()->SetDirtyFlag(bDirty);
		return bRestored;
	}
};

struct FContext
{
	TWeakObjectPtr<UPCGGraph> Graph;
	TWeakObjectPtr<UEdGraph> EditorGraph;
	FString Id;
	TMap<FString, TSharedPtr<FEdGraphSchemaAction>> Factories;
	FValues Catalog;
};

struct FReceipt
{
	TWeakObjectPtr<UPCGGraph> Graph;
	TWeakObjectPtr<UEdGraph> EditorGraph;
	FString Id;
	FString RequestDigest;
	FString BeforeHash;
	FString AfterHash;
	FString AfterObjectDigest;
	FJson Result;
	FSnapshot Before;
	TArray<TWeakObjectPtr<UPCGNode>> Added;
	TMap<TWeakObjectPtr<UPCGSettings>, TArray<FString>> EditedSettings;
	TSet<FString> UpdatedNodes;
	TSet<FString> RemovedNodes;
	bool bRolledBack = false;
};

struct FSession
{
	FSession()
	{
		ReloadHandle = FCoreUObjectDelegates::OnPackageReloaded.AddRaw(this, &FSession::OnPackageReloaded);
	}

	~FSession()
	{
		FCoreUObjectDelegates::OnPackageReloaded.Remove(ReloadHandle);
	}

	TMap<FString, FContext> Contexts;
	TMap<FString, FReceipt> Receipts;

private:
	void OnPackageReloaded(EPackageReloadPhase Phase, FPackageReloadedEvent* Event)
	{
		if (Phase != EPackageReloadPhase::PrePackageFixup || !Event)
		{
			return;
		}

		const UPackage* OldPackage = Event->GetOldPackage();
		for (auto It = Contexts.CreateIterator(); It; ++It)
		{
			const UPCGGraph* Graph = It.Value().Graph.Get();
			if (!Graph || Graph->GetOutermost() == OldPackage)
			{
				It.RemoveCurrent();
			}
		}
		for (auto& Pair : Receipts)
		{
			FReceipt& Receipt = Pair.Value;
			const UPCGGraph* Graph = Receipt.Graph.Get();
			if (Graph && Graph->GetOutermost() == OldPackage)
			{
				// Release graph-owned objects before the old package is collected.
				Receipt.Before = FSnapshot{};
				Receipt.Added.Reset();
				Receipt.EditedSettings.Reset();
				Receipt.Graph.Reset();
				Receipt.EditorGraph.Reset();
			}
		}
	}

	FDelegateHandle ReloadHandle;
};

struct FEditOperation
{
	FString Kind;
	FString Ref;
	FString FactoryId;
	FVector2D Position = FVector2D::ZeroVector;
	FString NodeId;
	FString SettingsClass;
	FValues Changes;
	FEdge Edge;
};

struct FEdit
{
	TArray<FEditOperation> Operations;
	FString Digest;
	FJson Result;
};

bool EndpointFromJson(const FJson& Json, const TSet<FString>& Refs, const FGraphState& State, FEndpoint& Endpoint)
{
	const FString NodeId = Field(Json, TEXT("nodeId"));
	const FString Ref = Field(Json, TEXT("ref"));
	const FString Pin = Field(Json, TEXT("pin"));
	if (NodeId.IsEmpty() == Ref.IsEmpty() || Pin.IsEmpty())
	{
		return false;
	}
	if (!Ref.IsEmpty())
	{
		if (!Refs.Contains(Ref))
		{
			return false;
		}
		Endpoint = {TEXT("$") + Ref, Pin};
		return true;
	}
	const UPCGNode* Node = State.Nodes.FindRef(NodeId);
	if (!Node)
	{
		return false;
	}
	Endpoint = {NodeId, Pin};
	return true;
}

bool HasIncidentEdges(const TSet<FEdge>& Edges, const FString& NodeId)
{
	for (const FEdge& Edge : Edges)
	{
		if (Edge.From.Node == NodeId || Edge.To.Node == NodeId)
		{
			return true;
		}
	}
	return false;
}

bool IsOrdinaryNode(const UPCGGraph& Graph, const UPCGNode* Node)
{
	return Node && Node != Graph.GetInputNode() && Node != Graph.GetOutputNode();
}

UPCGSettings* GetEditableSettings(UPCGNode* Node)
{
	UPCGSettings* Settings = Cast<UPCGSettings>(Node->GetSettingsInterface());
	return Settings && Settings->GetOuter() == Node ? Settings : nullptr;
}

bool ResolveSettingsTarget(const FJson& Object, const UPCGGraph& Graph, const FGraphState& State,
	const TSet<FString>& Refs, const TSet<FString>& Removed, FEditOperation& Operation, FString& Message)
{
	Operation.NodeId = Field(Object, TEXT("nodeId"));
	Operation.Ref = Field(Object, TEXT("ref"));
	Operation.SettingsClass = Field(Object, TEXT("expectedSettingsClass"));
	const bool bRemove = Operation.Kind == TEXT("removeNode");
	if (Operation.SettingsClass.IsEmpty() || Operation.NodeId.IsEmpty() == Operation.Ref.IsEmpty()
		|| (bRemove && !Operation.Ref.IsEmpty()) || Removed.Contains(Operation.NodeId)
		|| Object->Values.Num() != (bRemove ? 3 : 4))
	{
		Message = TEXT("Specify the Settings class and a single live node target. Removal requires nodeId.");
		return false;
	}
	if (!Operation.Ref.IsEmpty())
	{
		if (!Refs.Contains(Operation.Ref))
		{
			Message = TEXT("The Settings ref must refer to an earlier addNode operation.");
			return false;
		}
		return true;
	}
	UPCGNode* Node = State.Nodes.FindRef(Operation.NodeId);
	if (!IsOrdinaryNode(Graph, Node))
	{
		Message = TEXT("Only an ordinary node in the target graph may be configured or removed.");
		return false;
	}
	const UPCGSettings* Settings = bRemove ? Node->GetSettings() : GetEditableSettings(Node);
	if (!Settings || Settings->GetClass()->GetPathName() != Operation.SettingsClass)
	{
		Message = TEXT("The node Settings class does not match, or the Settings are shared or read-only.");
		return false;
	}
	return true;
}

bool BuildPlan(const FJson& Params, FContext& Context, FEdit& Edit, FString& Message)
{
	UPCGGraph* Graph = Context.Graph.Get();
	UEdGraph* EditorGraph = Context.EditorGraph.Get();
	FGraphState State;
	if (!IsWritable(Graph) || !EditorGraph || !ReadGraph(Graph, State) || !MatchesEditor(EditorGraph, State))
	{
		Message = State.ReadError.IsEmpty() ? TEXT("The graph is not writable, too large, or its editor view is out of sync.") : State.ReadError;
		return false;
	}
	if (Field(Params, TEXT("expectedGraphHash")) != State.Hash)
	{
		Message = TEXT("Graph changed. Read it again before planning.");
		return false;
	}
	const FValues* Operations = nullptr;
	if (!Params->TryGetArrayField(TEXT("operations"), Operations) || Operations->IsEmpty())
	{
		Message = TEXT("At least one operation is required.");
		return false;
	}

	TSet<FString> Refs;
	TSet<FString> Reconfigured;
	TSet<FString> Relinked;
	TSet<FString> Removed;
	TSet<FEdge> Edges = State.Edges;
	int32 LinkCount = 0;
	int32 SettingsCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Operations)
	{
		const FJson Object = Value->AsObject();
		FEditOperation Operation;
		Operation.Kind = Field(Object, TEXT("op"));
		if (Operation.Kind == TEXT("addNode"))
		{
			Operation.Ref = Field(Object, TEXT("ref"));
			Operation.FactoryId = Field(Object, TEXT("factoryId"));
			if (Operation.Ref.IsEmpty() || Refs.Contains(Operation.Ref) || Refs.Num() >= 32
				|| !Context.Factories.Contains(Operation.FactoryId))
			{
				Message = TEXT("Invalid node ref or factory ID, duplicate ref, or node limit exceeded.");
				return false;
			}
			Object->TryGetNumberField(TEXT("posX"), Operation.Position.X);
			Object->TryGetNumberField(TEXT("posY"), Operation.Position.Y);
			Refs.Add(Operation.Ref);
		}
		else if (Operation.Kind == TEXT("setSettings") || Operation.Kind == TEXT("removeNode"))
		{
			if (!ResolveSettingsTarget(Object, *Graph, State, Refs, Removed, Operation, Message))
			{
				return false;
			}
			if (Operation.Kind == TEXT("setSettings"))
			{
				const FValues* Changes = nullptr;
				if (++SettingsCount > MaxNodeChanges || !Object->TryGetArrayField(TEXT("changes"), Changes))
				{
					Message = TEXT("Settings changes are missing or exceed the batch limit.");
					return false;
				}
				const UPCGSettings* Instance = nullptr;
				if (!Operation.NodeId.IsEmpty() && !Reconfigured.Contains(Operation.NodeId) && !Relinked.Contains(Operation.NodeId))
				{
					Instance = GetEditableSettings(State.Nodes.FindRef(Operation.NodeId));
				}
				if (!PCGGraphSettings::Validate(Operation.SettingsClass, Instance, *Changes, Message))
				{
					return false;
				}
				Operation.Changes = *Changes;
				Reconfigured.Add(Operation.NodeId);
			}
			else
			{
				if (Removed.Num() >= MaxNodeChanges || HasIncidentEdges(Edges, Operation.NodeId))
				{
					Message = TEXT("Disconnect every edge explicitly before removing the node, and respect the removal limit.");
					return false;
				}
				Removed.Add(Operation.NodeId);
			}
		}
		else
		{
			const FJson* From = nullptr;
			const FJson* To = nullptr;
			if (++LinkCount > 128 || !Object->TryGetObjectField(TEXT("from"), From) || !Object->TryGetObjectField(TEXT("to"), To)
				|| !EndpointFromJson(*From, Refs, State, Operation.Edge.From)
				|| !EndpointFromJson(*To, Refs, State, Operation.Edge.To)
				|| Removed.Contains(Operation.Edge.From.Node) || Removed.Contains(Operation.Edge.To.Node))
			{
				Message = TEXT("Invalid edge endpoint or link operation limit exceeded.");
				return false;
			}

			const UPCGNode* FromNode = Reconfigured.Contains(Operation.Edge.From.Node) ? nullptr : State.Nodes.FindRef(Operation.Edge.From.Node);
			const UPCGNode* ToNode = Reconfigured.Contains(Operation.Edge.To.Node) ? nullptr : State.Nodes.FindRef(Operation.Edge.To.Node);
			const UPCGPin* FromPin = FromNode ? FromNode->GetOutputPin(FName(*Operation.Edge.From.Pin)) : nullptr;
			const UPCGPin* ToPin = ToNode ? ToNode->GetInputPin(FName(*Operation.Edge.To.Pin)) : nullptr;
			if ((FromNode && !FromPin) || (ToNode && !ToPin))
			{
				Message = TEXT("An existing pin label does not match the requested direction.");
				return false;
			}

			if (Operation.Kind == TEXT("connect"))
			{
				if (Edges.Contains(Operation.Edge) || WouldCycle(Edges, Operation.Edge))
				{
					Message = TEXT("The edge already exists or would create a cycle.");
					return false;
				}
				// Earlier actions can change the effective types of dynamic pins.
				if (Edit.Operations.IsEmpty() && FromPin && ToPin
					&& (!ToPin->IsCompatible(FromPin) || FromPin->GetRequiredTypeConversion(ToPin) != EPCGTypeConversion::NoConversionRequired))
				{
					Message = TEXT("The existing pins are incompatible or require a conversion node.");
					return false;
				}
				if (ToPin && !ToPin->AllowsMultipleConnections())
				{
					for (const FEdge& Edge : Edges)
					{
						if (Edge.To == Operation.Edge.To)
						{
							Message = TEXT("Explicitly disconnect the existing edge before replacing it.");
							return false;
						}
					}
				}
				Edges.Add(Operation.Edge);
			}
			else if (Operation.Kind == TEXT("disconnect"))
			{
				if (Edges.Remove(Operation.Edge) != 1)
				{
					Message = TEXT("The edge to disconnect does not exist at this step.");
					return false;
				}
			}
			else
			{
				Message = TEXT("Unknown graph operation.");
				return false;
			}
			Relinked.Add(Operation.Edge.From.Node);
			Relinked.Add(Operation.Edge.To.Node);
		}
		Edit.Operations.Add(MoveTemp(Operation));
	}

	bool bAffectedTruncated = false;
	FJson Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("graphPath"), Graph->GetPathName());
	Plan->SetStringField(TEXT("contextId"), Context.Id);
	Plan->SetStringField(TEXT("beforeHash"), State.Hash);
	Plan->SetArrayField(TEXT("operations"), *Operations);
	Plan->SetArrayField(TEXT("affectedLoadedComponents"), AffectedComponents(Graph, bAffectedTruncated));
	Plan->SetBoolField(TEXT("affectedComponentsTruncated"), bAffectedTruncated);
	Plan->SetBoolField(TEXT("mayRefreshComponents"), true);
	if (!TryDigestJson(Plan, Edit.Digest))
	{
		Message = TEXT("Could not digest the edit plan.");
		return false;
	}
	Plan->SetStringField(TEXT("planDigest"), Edit.Digest);
	Plan->SetBoolField(TEXT("applied"), false);
	Edit.Result = MoveTemp(Plan);
	return true;
}

bool MatchesVisiblePins(const UEdGraphNode& View, const UPCGNode& Node)
{
	int32 Visible = 0;
	for (const bool bOutput : {false, true})
	{
		const EEdGraphPinDirection Direction = bOutput ? EGPD_Output : EGPD_Input;
		for (const UPCGPin* Pin : bOutput ? Node.GetOutputPins() : Node.GetInputPins())
		{
			if (!Pin->Properties.bInvisiblePin)
			{
				if (!View.FindPin(Pin->Properties.Label, Direction))
				{
					return false;
				}
				++Visible;
			}
		}
	}
	return View.Pins.Num() == Visible;
}

bool VerifyDetached(UPCGGraph& Graph, UEdGraph& View, const TSet<const UObject*>& Removed)
{
	// Follow persistent references from the graph and its active view, not every object with that Outer.
	TArray<UObject*> Queue{&Graph, &View};
	TSet<const UObject*> Visited{&Graph, &View};
	for (int32 Index = 0; Index < Queue.Num(); ++Index)
	{
		TArray<UObject*> References;
		FReferenceFinder Finder(References, nullptr, false, true, false, true);
		Finder.FindReferences(Queue[Index]);
		for (UObject* Reference : References)
		{
			if (Removed.Contains(Reference))
			{
				return false;
			}
			if (Reference->IsIn(&Graph) && !Visited.Contains(Reference))
			{
				if (Queue.Num() >= MaxNodes * 2 + MaxPins + MaxEdges)
				{
					return false;
				}
				Visited.Add(Reference);
				Queue.Add(Reference);
			}
		}
	}
	return true;
}

bool ApplyNodeChange(const FEditOperation& Operation, UPCGGraph& Graph, UEdGraph& View,
	const FGraphState& Before, const TMap<FString, FString>& Created, FReceipt& Receipt, FString& Message)
{
	const FString Id = Operation.Ref.IsEmpty() ? Operation.NodeId : Created.FindRef(TEXT("$") + Operation.Ref);
	UPCGNode* Node = Before.Nodes.FindRef(Id);
	TMap<FString, UEdGraphNode*> Views;
	if (!IsOrdinaryNode(Graph, Node) || !ReadEditorNodes(&View, Before, Views))
	{
		Message = TEXT("The requested authored node or editor view is unavailable.");
		return false;
	}
	UEdGraphNode* NodeView = Views.FindRef(Id);
	const bool bRemove = Operation.Kind == TEXT("removeNode");
	UPCGSettings* Settings = bRemove ? Node->GetSettings() : GetEditableSettings(Node);
	if (!NodeView || !Settings || Settings->GetClass()->GetPathName() != Operation.SettingsClass)
	{
		Message = TEXT("The native node Settings class or ownership differs from the plan.");
		return false;
	}

	FGraphState After;
	if (!bRemove)
	{
		TArray<FString>& EditedPaths = Receipt.EditedSettings.FindOrAdd(Settings);
		if (!PCGGraphSettings::Apply(*Settings, Operation.Changes, EditedPaths, Message))
		{
			return false;
		}
		if (!ReadGraph(&Graph, After))
		{
			Message = After.ReadError.IsEmpty() ? TEXT("Cannot read the resulting PCG Graph.") : After.ReadError;
			return false;
		}
		if (After.Nodes.Num() != Before.Nodes.Num() || After.Nodes.FindRef(Id) != Node
			|| !SameEdges(After.Edges, Before.Edges) || !MatchesEditor(&View, After) || !MatchesVisiblePins(*NodeView, *Node))
		{
			Message = TEXT("Settings changed the existing edge set or left the editor pins out of sync. Disconnect affected edges first.");
			return false;
		}
		Receipt.UpdatedNodes.Add(Id);
		return true;
	}

	if (HasIncidentEdges(Before.Edges, Id) || !NodeView->CanUserDeleteNode())
	{
		Message = TEXT("The node is protected or still has incident edges.");
		return false;
	}
	const TSet<const UObject*> Removed{Node, NodeView, Settings, Node->GetSettingsInterface()};
	NodeView->Modify();
	NodeView->DestroyNode();
	Graph.RemoveNode(Node);
	if (!ReadGraph(&Graph, After) || After.Nodes.Num() + 1 != Before.Nodes.Num() || After.Nodes.Contains(Id)
		|| View.Nodes.Contains(NodeView) || !SameEdges(After.Edges, Before.Edges)
		|| !MatchesEditor(&View, After) || !VerifyDetached(Graph, View, Removed))
	{
		Message = TEXT("Native deletion did not detach the node and its Settings from the graph.");
		return false;
	}
	Receipt.UpdatedNodes.Remove(Id);
	Receipt.RemovedNodes.Add(Id);
	return true;
}

bool ApplyStep(const FEditOperation& Operation, FContext& Context, TMap<FString, FString>& Created,
	FReceipt& Receipt, FString& Message)
{
	UPCGGraph* Graph = Context.Graph.Get();
	UEdGraph* View = Context.EditorGraph.Get();
	const UEdGraphSchema* Schema = View->GetSchema();
	FGraphState Before;
	if (!ReadGraph(Graph, Before))
	{
		Message = TEXT("Could not read the current graph.");
		return false;
	}
	TSet<FEdge> Expected = Before.Edges;
	if (Operation.Kind == TEXT("addNode"))
	{
		const TSharedPtr<FEdGraphSchemaAction> Action = Context.Factories.FindRef(Operation.FactoryId);
		if (!Action)
		{
			Message = TEXT("The node factory is stale.");
			return false;
		}
		UEdGraphNode* NewView = Action->PerformAction(View, static_cast<UEdGraphPin*>(nullptr), Operation.Position, false);
		UPCGNode* NewNode = NewView ? Cast<UPCGNode>(ReadObjectProperty(NewView, TEXT("PCGNode"))) : nullptr;
		for (UPCGNode* Node : Graph->GetNodes())
		{
			if (Node && !Before.Nodes.Contains(Node->GetName()))
			{
				Receipt.Added.AddUnique(Node);
			}
		}
		FGraphState After;
		if (!NewNode || !ReadGraph(Graph, After) || After.Nodes.Num() != Before.Nodes.Num() + 1
			|| Before.Nodes.Contains(NewNode->GetName()) || After.Nodes.FindRef(NewNode->GetName()) != NewNode
			|| !SameEdges(After.Edges, Expected) || !MatchesEditor(View, After))
		{
			Message = TEXT("Native node creation did not produce exactly one matching node.");
			return false;
		}
		Created.Add(TEXT("$") + Operation.Ref, NewNode->GetName());
		return true;
	}

	if (Operation.Kind == TEXT("setSettings") || Operation.Kind == TEXT("removeNode"))
	{
		return ApplyNodeChange(Operation, *Graph, *View, Before, Created, Receipt, Message);
	}

	FEdge Edge = Operation.Edge;
	if (const FString* Id = Created.Find(Edge.From.Node))
	{
		Edge.From.Node = *Id;
	}
	if (const FString* Id = Created.Find(Edge.To.Node))
	{
		Edge.To.Node = *Id;
	}
	TMap<FString, UEdGraphNode*> Views;
	if (!ReadEditorNodes(View, Before, Views))
	{
		Message = TEXT("The PCG editor nodes no longer match the graph.");
		return false;
	}
	UEdGraphPin* From = FindEditorPin(Views, Edge.From, EGPD_Output);
	UEdGraphPin* To = FindEditorPin(Views, Edge.To, EGPD_Input);
	UPCGNode* FromNode = Before.Nodes.FindRef(Edge.From.Node);
	UPCGNode* ToNode = Before.Nodes.FindRef(Edge.To.Node);
	const UPCGPin* FromPin = FromNode ? FromNode->GetOutputPin(FName(*Edge.From.Pin)) : nullptr;
	const UPCGPin* ToPin = ToNode ? ToNode->GetInputPin(FName(*Edge.To.Pin)) : nullptr;
	if (!From || !To || !FromPin || !ToPin || FromNode == ToNode)
	{
		Message = TEXT("The current editor pins could not be resolved.");
		return false;
	}

	if (Operation.Kind == TEXT("connect"))
	{
		if (Expected.Contains(Edge) || WouldCycle(Expected, Edge) || !ToPin->IsCompatible(FromPin)
			|| FromPin->GetRequiredTypeConversion(ToPin) != EPCGTypeConversion::NoConversionRequired
			|| (!ToPin->AllowsMultipleConnections() && ToPin->EdgeCount() != 0)
			|| Schema->CanCreateConnection(From, To).Response != CONNECT_RESPONSE_MAKE)
		{
			Message = TEXT("Direct connection rejected. Check types, cycles, and existing input edges.");
			return false;
		}
		Expected.Add(Edge);
		if (!Schema->TryCreateConnection(From, To))
		{
			Message = TEXT("The native schema rejected the connection.");
			return false;
		}
	}
	else
	{
		if (Expected.Remove(Edge) != 1 || !From->LinkedTo.Contains(To))
		{
			Message = TEXT("The exact edge to disconnect is missing.");
			return false;
		}
		Schema->BreakSinglePinLink(From, To);
	}

	FGraphState After;
	if (!ReadGraph(Graph, After) || After.Nodes.Num() != Before.Nodes.Num()
		|| !SameEdges(After.Edges, Expected) || !MatchesEditor(View, After))
	{
		Message = TEXT("The native operation produced unexpected graph changes.");
		return false;
	}
	return true;
}

bool ConfirmWrite(const FJson& Params)
{
	bool bConfirm = false;
	return Params->TryGetBoolField(TEXT("confirmWrite"), bConfirm) && bConfirm;
}

FMCPToolResult CreateGraph(const FJson& Params)
{
	const FString Package = Field(Params, TEXT("packagePath"));
	FString Filename;
	if (!ConfirmWrite(Params) || !WritablePackage(Package, Filename))
	{
		return Error(TEXT("A confirmed, writable project package path is required."));
	}
	const FString Name = FPackageName::GetLongPackageAssetName(Package);
	if (FPackageName::DoesPackageExist(Package) || FindPackage(nullptr, *Package))
	{
		return Error(TEXT("The target package already exists."), TEXT("asset_exists"));
	}

	FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("PCGEditor"));
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/PCGEditor.PCGGraphFactory"));
	if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
	{
		return Error(TEXT("The native PCG Graph factory is unavailable."), TEXT("capability_unavailable"));
	}
	UFactory* Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UPCGGraph* Graph = Cast<UPCGGraph>(AssetTools.CreateAsset(Name,
		FPackageName::GetLongPackagePath(Package), UPCGGraph::StaticClass(), Factory));
	if (!Graph || Graph->GetOutermost()->GetName() != Package || !Graph->GetInputNode() || !Graph->GetOutputNode())
	{
		return Error(TEXT("The native factory did not create the expected graph and default nodes."), TEXT("pcg_create_failed"));
	}

	FJson Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("dirty"), Graph->MarkPackageDirty() || Graph->GetOutermost()->IsDirty());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult ListGraphs(const FJson& Params)
{
	const FString Path = Field(Params, TEXT("path"));
	if (!Path.StartsWith(TEXT("/")) || Path.Contains(TEXT("..")))
	{
		return Error(TEXT("An Unreal asset path prefix is required."));
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Path));
	Filter.ClassPaths.Add(UPCGGraph::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	Assets.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString();
	});
	FValues Values;
	for (const FAssetData& Asset : Assets)
	{
		FJson Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("graphPath"), Asset.GetSoftObjectPath().ToString());
		Values.Add(MakeShared<FJsonValueObject>(Entry));
	}
	FJson Result = Page(Params, TEXT("graphs"), Values);
	Result->SetBoolField(TEXT("registryLoading"), Registry.IsLoadingAssets());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult GetGraph(const FJson& Params)
{
	UPCGGraph* Graph = LoadGraph(Field(Params, TEXT("graphPath")));
	FGraphState State;
	if (!Graph || !ReadGraph(Graph, State))
	{
		if (!State.ReadError.IsEmpty())
		{
			return Error(State.ReadError, TEXT("pcg_settings_read_limit"));
		}
		return Error(TEXT("Cannot read a complete PCG Graph structure at the supplied path."), TEXT("pcg_graph_unavailable"));
	}

	FJson Result = Page(Params, TEXT("nodes"), State.NodeValues);
	FValues Edges;
	const FValues AllEdges = EdgeValues(State.Edges);
	double EdgeOffsetValue = 0;
	double EdgeLimitValue = 200;
	Params->TryGetNumberField(TEXT("edgeOffset"), EdgeOffsetValue);
	Params->TryGetNumberField(TEXT("edgeLimit"), EdgeLimitValue);
	const int32 Offset = FMath::Clamp(static_cast<int32>(EdgeOffsetValue), 0, AllEdges.Num());
	const int32 End = FMath::Min(Offset + FMath::Clamp(static_cast<int32>(EdgeLimitValue), 1, 500), AllEdges.Num());
	for (int32 Index = Offset; Index < End; ++Index)
	{
		Edges.Add(AllEdges[Index]);
	}
	Result->SetArrayField(TEXT("edges"), MoveTemp(Edges));
	Result->SetNumberField(TEXT("totalEdges"), AllEdges.Num());
	Result->SetBoolField(TEXT("edgesHaveMore"), End < AllEdges.Num());
	if (End < AllEdges.Num())
	{
		Result->SetNumberField(TEXT("nextEdgeOffset"), End);
	}
	Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
	Result->SetStringField(TEXT("graphHash"), State.Hash);
	Result->SetStringField(TEXT("settingsCoverage"), TEXT("summary"));
	Result->SetBoolField(TEXT("dirty"), Graph->GetOutermost()->IsDirty());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult Prepare(const FJson& Params, FSession& Session)
{
	UPCGGraph* Graph = LoadGraph(Field(Params, TEXT("graphPath")));
	if (!IsWritable(Graph) || !GEditor || IsRunningCommandlet() || !FApp::CanEverRender())
	{
		return Error(TEXT("A writable graph and an interactive Editor are required."), TEXT("pcg_editor_unavailable"));
	}
	UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Editors || (!Editors->FindEditorForAsset(Graph, false) && !Editors->OpenEditorForAsset(Graph)))
	{
		return Error(TEXT("Could not open the native PCG editor."), TEXT("pcg_editor_unavailable"));
	}
	UEdGraph* View = FindEditorGraph(Graph);
	FGraphState State;
	if (!View || !ReadGraph(Graph, State) || !MatchesEditor(View, State))
	{
		if (!State.ReadError.IsEmpty())
		{
			return Error(State.ReadError, TEXT("pcg_settings_read_limit"));
		}
		return Error(TEXT("The native graph view could not be resolved or does not match the asset."), TEXT("pcg_editor_out_of_sync"));
	}

	const FString Path = Graph->GetPathName();
	for (auto It = Session.Contexts.CreateIterator(); It; ++It)
	{
		if (!It.Value().Graph.IsValid() || !Editors->FindEditorForAsset(It.Value().Graph.Get(), false))
		{
			It.RemoveCurrent();
		}
	}
	FContext* Context = Session.Contexts.Find(Path);
	const FString RequestedContext = Field(Params, TEXT("contextId"));
	if (!RequestedContext.IsEmpty())
	{
		if (!Context || Context->Id != RequestedContext || Context->Graph.Get() != Graph || Context->EditorGraph.Get() != View)
		{
			return Error(TEXT("The factory context expired. Prepare the graph again."), TEXT("pcg_context_stale"));
		}
	}
	else
	{
		if (!Context && Session.Contexts.Num() >= 16)
		{
			return Error(TEXT("Too many PCG graph editor contexts are open."), TEXT("pcg_context_capacity"));
		}
		FContext NewContext;
		NewContext.Id = NewId();
		NewContext.Graph = Graph;
		NewContext.EditorGraph = View;
		FGraphContextMenuBuilder Builder(View);
		View->GetSchema()->GetGraphContextActions(Builder);
		for (int32 Index = 0; Index < Builder.GetNumActions(); ++Index)
		{
			for (const TSharedPtr<FEdGraphSchemaAction>& Action : Builder.GetAction(Index).Actions)
			{
				if (!Action)
				{
					continue;
				}
				const FName Type = Action->GetTypeId();
				const bool bBlueprint = Type == TEXT("FPCGEditorGraphSchemaAction_NewBlueprintElement");
				if (!bBlueprint && Type != TEXT("FPCGEditorGraphSchemaAction_NewNativeElement"))
				{
					continue;
				}
				if (NewContext.Factories.Num() >= MaxContextActions)
				{
					return Error(TEXT("The native action catalog exceeds the supported size."), TEXT("pcg_context_capacity"));
				}
				const FString Id = NewId();
				NewContext.Factories.Add(Id, Action);
				FJson Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("factoryId"), Id);
				Entry->SetStringField(TEXT("kind"), bBlueprint ? TEXT("blueprintElement") : TEXT("settings"));
				Entry->SetStringField(TEXT("title"), Action->GetMenuDescription().ToString());
				Entry->SetStringField(TEXT("category"), Action->GetCategory().ToString());
				NewContext.Catalog.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		Context = &Session.Contexts.Add(Path, MoveTemp(NewContext));
	}

	const FString Search = Field(Params, TEXT("filter"));
	FValues Filtered;
	for (const TSharedPtr<FJsonValue>& Value : Context->Catalog)
	{
		const FJson Entry = Value->AsObject();
		if (Search.IsEmpty() || Field(Entry, TEXT("title")).Contains(Search) || Field(Entry, TEXT("category")).Contains(Search))
		{
			Filtered.Add(Value);
		}
	}
	FJson Result = Page(Params, TEXT("factories"), Filtered);
	Result->SetStringField(TEXT("contextId"), Context->Id);
	Result->SetStringField(TEXT("graphPath"), Path);
	Result->SetStringField(TEXT("graphHash"), State.Hash);
	return FMCPToolResult::Ok(Result);
}

bool CanEditNow()
{
	return GEditor && !GEditor->IsPlaySessionInProgress() && !GEditor->IsTransactionActive();
}

FMCPToolResult PlanOrApply(const FJson& Params, FSession& Session, bool bApply)
{
	const FString RequestId = Field(Params, TEXT("requestId"));
	FString RequestDigest;
	if (bApply)
	{
		if (!ConfirmWrite(Params) || RequestId.IsEmpty() || !TryDigestJson(Params, RequestDigest))
		{
			return Error(TEXT("Confirmed edits require a request ID."));
		}
		if (const FReceipt* Old = Session.Receipts.Find(RequestId))
		{
			if (Old->RequestDigest != RequestDigest)
			{
				return Error(TEXT("The request ID was already used for a different edit."), TEXT("request_id_conflict"));
			}
			FGraphState State;
			if (Old->bRolledBack || !Old->Graph.IsValid() || !Old->EditorGraph.IsValid()
				|| !ReadGraph(Old->Graph.Get(), State) || State.Hash != Old->AfterHash
				|| !MatchesEditor(Old->EditorGraph.Get(), State))
			{
				return Error(TEXT("The edit receipt no longer describes the current graph."), TEXT("receipt_state_changed"));
			}
			FJson Result = MakeShared<FJsonObject>(*Old->Result);
			Result->SetBoolField(TEXT("replayed"), true);
			return FMCPToolResult::Ok(Result);
		}
		if (Session.Receipts.Num() >= MaxReceipts || !CanEditNow())
		{
			return Error(TEXT("An edit cannot start during PIE, an active transaction, or a full receipt catalog."));
		}
	}

	FContext* Context = Session.Contexts.Find(Field(Params, TEXT("graphPath")));
	UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!Context || !Editors || Context->Id != Field(Params, TEXT("contextId")) || !Context->Graph.IsValid()
		|| !Editors->FindEditorForAsset(Context->Graph.Get(), false)
		|| !Context->EditorGraph.IsValid() || FindEditorGraph(Context->Graph.Get()) != Context->EditorGraph.Get())
	{
		return Error(TEXT("Prepare the target graph before editing."), TEXT("pcg_context_stale"));
	}
	FEdit Edit;
	FString Message;
	if (!BuildPlan(Params, *Context, Edit, Message))
	{
		return Error(Message);
	}
	if (!bApply)
	{
		return FMCPToolResult::Ok(Edit.Result);
	}

	FString Code;
	if (!ValidateChangeApproval(Params, Edit.Digest, Code, Message))
	{
		return FMCPToolResult::Error(Message, Code, 409);
	}

	UPCGGraph* Graph = Context->Graph.Get();
	UEdGraph* View = Context->EditorGraph.Get();
	FReceipt Receipt;
	Receipt.Graph = Graph;
	Receipt.EditorGraph = View;
	Receipt.Id = NewId();
	Receipt.BeforeHash = Field(Edit.Result, TEXT("beforeHash"));
	Receipt.RequestDigest = RequestDigest;
	if (!Receipt.Before.Capture(Graph))
	{
		return Error(TEXT("The graph exceeded the in-memory recovery snapshot limits."), TEXT("pcg_snapshot_unavailable"));
	}
	int64 Retained = Receipt.Before.Size;
	for (const auto& Pair : Session.Receipts)
	{
		Retained += Pair.Value.Before.Size;
	}
	if (Retained > MaxSnapshotBytes * 4)
	{
		return Error(TEXT("The session recovery budget is full."), TEXT("receipt_capacity"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("UEAI", "PCGGraphEdit", "UE AI Edit PCG Graph"));
	if (!Transaction.IsOutstanding())
	{
		return Error(TEXT("Could not start an Editor transaction."));
	}
	Graph->Modify();
	View->Modify();
	Graph->DisableNotificationsForEditor();
	TMap<FString, FString> Created;
	bool bApplied = true;
	for (const FEditOperation& Operation : Edit.Operations)
	{
		if (!ApplyStep(Operation, *Context, Created, Receipt, Message))
		{
			bApplied = false;
			break;
		}
	}
	FGraphState After;
	FSnapshot AfterObjects;
	bApplied = bApplied && ReadGraph(Graph, After) && MatchesEditor(View, After) && AfterObjects.Capture(Graph);
	if (!bApplied)
	{
		const bool bRestored = Receipt.Before.Restore(Graph, View, Receipt.Added, Receipt.EditedSettings, Receipt.Before.bWasDirty);
		Graph->EnableNotificationsForEditor();
		FGraphState Restored;
		if (bRestored && ReadGraph(Graph, Restored) && Restored.Hash == Receipt.BeforeHash
			&& MatchesEditor(View, Restored) && Graph->GetOutermost()->IsDirty() == Receipt.Before.bWasDirty)
		{
			Transaction.Cancel();
			return Error(Message.IsEmpty() ? TEXT("Verification failed. The batch was restored.") :
				Message + TEXT(" The batch was restored."), TEXT("pcg_edit_restored"));
		}
		Graph->MarkPackageDirty();
		return Error(TEXT("Edit failed and full restoration was not verified. Editor Undo was retained."), TEXT("pcg_restore_unverified"));
	}
	Graph->EnableNotificationsForEditor();
	Graph->MarkPackageDirty();
	View->NotifyGraphChanged();

	Receipt.AfterHash = After.Hash;
	Receipt.AfterObjectDigest = AfterObjects.Digest;
	FJson Result = Edit.Result;
	FJson NodeIds = MakeShared<FJsonObject>();
	for (const auto& Pair : Created)
	{
		NodeIds->SetStringField(Pair.Key.Mid(1), Pair.Value);
	}
	Result->SetObjectField(TEXT("createdNodeIds"), NodeIds);
	const auto NodeIdValues = [](const TSet<FString>& Ids)
	{
		TArray<FString> Sorted = Ids.Array();
		Sorted.Sort();
		FValues Values;
		for (const FString& Id : Sorted)
		{
			Values.Add(MakeShared<FJsonValueString>(Id));
		}
		return Values;
	};
	Result->SetArrayField(TEXT("updatedNodeIds"), NodeIdValues(Receipt.UpdatedNodes));
	Result->SetArrayField(TEXT("removedNodeIds"), NodeIdValues(Receipt.RemovedNodes));
	Result->SetStringField(TEXT("afterHash"), After.Hash);
	Result->SetStringField(TEXT("receiptId"), Receipt.Id);
	Result->SetBoolField(TEXT("applied"), true);
	Result->SetBoolField(TEXT("saved"), false);
	Result->SetBoolField(TEXT("readbackVerified"), true);
	Result->SetBoolField(TEXT("replayed"), false);
	Receipt.Result = Result;
	Session.Receipts.Add(RequestId, MoveTemp(Receipt));
	Session.Contexts.Remove(Graph->GetPathName());
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult Rollback(const FJson& Params, FSession& Session)
{
	FReceipt* Receipt = Session.Receipts.Find(Field(Params, TEXT("editRequestId")));
	if (!ConfirmWrite(Params) || !CanEditNow() || !Receipt || Receipt->bRolledBack || Receipt->Id != Field(Params, TEXT("receiptId")))
	{
		return Error(TEXT("A current, confirmed edit receipt is required."));
	}
	UPCGGraph* Graph = Receipt->Graph.Get();
	UEdGraph* View = Receipt->EditorGraph.Get();
	FGraphState Current;
	FSnapshot CurrentObjects;
	if (!IsWritable(Graph) || !View || LoadGraph(Graph->GetPathName()) != Graph || FindEditorGraph(Graph) != View
		|| !ReadGraph(Graph, Current) || Current.Hash != Receipt->AfterHash
		|| !MatchesEditor(View, Current) || !CurrentObjects.Capture(Graph)
		|| CurrentObjects.Digest != Receipt->AfterObjectDigest)
	{
		return Error(TEXT("The graph, its editor view, or its owned settings changed after the edit."), TEXT("rollback_conflict"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("UEAI", "PCGGraphRollback", "UE AI Roll Back PCG Graph Edit"));
	if (!Transaction.IsOutstanding())
	{
		return Error(TEXT("Could not start a rollback transaction."));
	}
	Graph->DisableNotificationsForEditor();
	const bool bRestored = Receipt->Before.Restore(Graph, View, Receipt->Added, Receipt->EditedSettings, true);
	Graph->EnableNotificationsForEditor();

	FGraphState Restored;
	if (!bRestored || !ReadGraph(Graph, Restored) || Restored.Hash != Receipt->BeforeHash || !MatchesEditor(View, Restored))
	{
		Graph->DisableNotificationsForEditor();
		const bool bRecovered = CurrentObjects.Restore(Graph, View, {}, Receipt->EditedSettings, CurrentObjects.bWasDirty);
		Graph->EnableNotificationsForEditor();
		FGraphState Recovered;
		if (bRecovered && ReadGraph(Graph, Recovered) && Recovered.Hash == Current.Hash && MatchesEditor(View, Recovered))
		{
			Transaction.Cancel();
			return Error(TEXT("Rollback failed; the pre-rollback graph was restored."), TEXT("pcg_rollback_failed"));
		}
		return Error(TEXT("Rollback and recovery could not be verified. Editor Undo was retained."), TEXT("pcg_restore_unverified"));
	}

	Receipt->bRolledBack = true;
	Receipt->Before.Objects.Reset();
	Receipt->Before.Size = 0;
	Session.Contexts.Remove(Graph->GetPathName());
	FJson Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
	Result->SetStringField(TEXT("graphHash"), Restored.Hash);
	Result->SetBoolField(TEXT("rollbackVerified"), true);
	Result->SetBoolField(TEXT("saved"), false);
	return FMCPToolResult::Ok(Result);
}

FMCPToolResult Save(const FJson& Params)
{
	UPCGGraph* Graph = LoadGraph(Field(Params, TEXT("graphPath")));
	FGraphState State;
	if (!ConfirmWrite(Params) || !IsWritable(Graph) || !CanEditNow() || !ReadGraph(Graph, State)
		|| State.Hash != Field(Params, TEXT("expectedGraphHash")))
	{
		return Error(TEXT("Saving requires confirmation, a writable graph and its current hash."));
	}
	UEdGraph* View = FindEditorGraph(Graph);
	if (View && !MatchesEditor(View, State))
	{
		return Error(TEXT("The graph editor is out of sync. Save refused."));
	}

	FString Filename;
	if (!WritablePackage(Graph->GetOutermost()->GetName(), Filename))
	{
		return Error(TEXT("Invalid save target."));
	}
	const bool bExisting = IFileManager::Get().FileExists(*Filename);
	ISourceControlModule& SourceControl = ISourceControlModule::Get();
	if (bExisting && SourceControl.IsEnabled())
	{
		ISourceControlProvider& Provider = SourceControl.GetProvider();
		TArray<FSourceControlStateRef> States;
		if (!Provider.IsAvailable() || Provider.GetState(TArray<FString>{Filename}, States, EStateCacheUsage::ForceUpdate) != ECommandResult::Succeeded
			|| States.Num() != 1 || States[0]->CanCheckout() || States[0]->IsCheckedOutOther())
		{
			return Error(TEXT("Check out the target package or resolve its source control status before saving."), TEXT("source_control_preflight_failed"));
		}
	}

	const bool bSaved = MCPHelpers::SaveGenericPackage(Graph)
		&& IFileManager::Get().FileExists(*Filename) && !Graph->GetOutermost()->IsDirty();
	if (!bSaved)
	{
		Graph->GetOutermost()->SetDirtyFlag(true);
	}
	FJson Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("graphPath"), Graph->GetPathName());
	Result->SetBoolField(TEXT("saved"), bSaved);
	Result->SetBoolField(TEXT("dirty"), Graph->GetOutermost()->IsDirty());
	if (!bSaved)
	{
		Result->SetStringField(TEXT("error"), TEXT("Package save could not be verified. In-memory changes were retained."));
	}
	return FMCPToolResult::Ok(Result);
}
#else
struct FSession {};
#endif

class FPCGGraphTool : public FMCPToolBase
{
public:
	FPCGGraphTool(EGraphOperation InOperation, TSharedRef<FSession> InSession)
		: Operation(InOperation), Session(MoveTemp(InSession))
	{
	}

	FMCPToolResult Execute(const TSharedPtr<FJsonObject>& Params) override
	{
#if WITH_UEAI_PCG_GRAPH
		switch (Operation)
		{
		case EGraphOperation::List:
			return ListGraphs(Params);
		case EGraphOperation::Get:
			return GetGraph(Params);
		case EGraphOperation::Create:
			return CreateGraph(Params);
		case EGraphOperation::Prepare:
			return Prepare(Params, *Session);
		case EGraphOperation::Plan:
			return PlanOrApply(Params, *Session, false);
		case EGraphOperation::Apply:
			return PlanOrApply(Params, *Session, true);
		case EGraphOperation::Rollback:
			return Rollback(Params, *Session);
		case EGraphOperation::Save:
			return Save(Params);
		}
#endif
		return FMCPToolResult::Error(TEXT("PCG Graph support is unavailable."), TEXT("capability_unavailable"), 409);
	}

private:
	EGraphOperation Operation;
	TSharedRef<FSession> Session;
};

class FPCGGraphList final : public FPCGGraphTool
{
public:
	explicit FPCGGraphList(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::List, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.list"); }
};

class FPCGGraphGet final : public FPCGGraphTool
{
public:
	explicit FPCGGraphGet(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Get, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.get"); }
};

class FPCGGraphCreate final : public FPCGGraphTool
{
public:
	explicit FPCGGraphCreate(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Create, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.create"); }
};

class FPCGGraphPrepare final : public FPCGGraphTool
{
public:
	explicit FPCGGraphPrepare(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Prepare, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.editor.prepare"); }
};

class FPCGGraphPlan final : public FPCGGraphTool
{
public:
	explicit FPCGGraphPlan(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Plan, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.edit.plan"); }
};

class FPCGGraphApply final : public FPCGGraphTool
{
public:
	explicit FPCGGraphApply(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Apply, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.edit.apply"); }
};

class FPCGGraphRollback final : public FPCGGraphTool
{
public:
	explicit FPCGGraphRollback(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Rollback, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.edit.rollback"); }
};

class FPCGGraphSave final : public FPCGGraphTool
{
public:
	explicit FPCGGraphSave(TSharedRef<FSession> Session)
		: FPCGGraphTool(EGraphOperation::Save, MoveTemp(Session))
	{
	}

	FString GetCapabilityId() const override { return TEXT("scene.pcg.graph.save"); }
};
}

void RegisterPCGGraphTools(FMCPToolRegistry& Registry)
{
	const TSharedRef<FSession> Session = MakeShared<FSession>();
	Registry.Register(MakeShared<FPCGGraphList>(Session));
	Registry.Register(MakeShared<FPCGGraphGet>(Session));
	Registry.Register(MakeShared<FPCGGraphCreate>(Session));
	Registry.Register(MakeShared<FPCGGraphPrepare>(Session));
	Registry.Register(MakeShared<FPCGGraphPlan>(Session));
	Registry.Register(MakeShared<FPCGGraphApply>(Session));
	Registry.Register(MakeShared<FPCGGraphRollback>(Session));
	Registry.Register(MakeShared<FPCGGraphSave>(Session));
}
}
