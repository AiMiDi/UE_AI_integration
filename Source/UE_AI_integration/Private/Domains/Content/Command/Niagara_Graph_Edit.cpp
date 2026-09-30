// Plan-gated additive Niagara graph edits. The native schema owns node
// creation, numeric specialization and connection validation.
#include "Tools/MCPToolBase.h"
#include "Tools/MCPToolRegistry.h"
#include "Infrastructure/DomainChangePlan.h"
#ifndef WITH_UEAI_NIAGARA
#define WITH_UEAI_NIAGARA 0
#endif
#if WITH_UEAI_NIAGARA && WITH_EDITORONLY_DATA
#include "EdGraphSchema_Niagara.h"
#include "NiagaraActions.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeOp.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"
#include "UObject/StrongObjectPtr.h"
#include "String/LexFromString.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraSystemFactoryNew.h"
#endif

namespace UEAINiagaraEditPrivate
{
using namespace UEAIIntegration::Infrastructure;
using FJson = TSharedPtr<FJsonObject>;
constexpr int32 MaxNodes = 32, MaxConnections = 128, MaxReceipts = 256;
FString Guid(const FGuid& Id) { return Id.ToString(EGuidFormats::DigitsWithHyphensLower); }
FString String(const FJson& Object, const TCHAR* Field)
{
	FString Value;
	if (Object) Object->TryGetStringField(Field, Value);
	return Value;
}
FMCPToolResult Error(const FString& Message, const FString& Code = TEXT("invalid_graph_edit"))
{ return FMCPToolResult::Error(Message, Code, 422); }
bool Fail(FString& Error, const FString& Message) { Error = Message; return false; }

struct FEdge { FGuid FromNode, FromPin, ToNode, ToPin; };
struct FJournal { TMap<FString, FGuid> Added; TArray<FEdge> Edges; };
struct FEdit
{
	UNiagaraSystem* System = nullptr;
	UNiagaraGraph* Graph = nullptr;
	FJson Request, Plan;
	FString RequestDigest, Before, ChangeId, Digest;
};
struct FReceipt
{
	TWeakObjectPtr<UNiagaraSystem> System;
	TWeakObjectPtr<UNiagaraGraph> Graph;
	FJournal Journal;
	FString RequestId, RequestDigest, Digest, Id, Before, After, ChangeIdAfter;
	bool bRolledBack = false;
};
TMap<FString, FReceipt> Receipts;
TMap<FString, FString> RequestReceipts;

bool Array(const FJson& Object, const TCHAR* Field, int32 Limit,
	TArray<TSharedPtr<FJsonValue>>& Out, FString& Error)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object || !Object->TryGetArrayField(Field, Values) || Values->Num() > Limit)
		return Fail(Error, FString::Printf(TEXT("%s must be an array of at most %d items."), Field, Limit));
	Out = *Values;
	for (const auto& Value : Out)
		if (!Value || Value->Type != EJson::Object) return Fail(Error, TEXT("Array items must be objects."));
	return true;
}
FJson EditRequest(const FJson& Params)
{
	FJson Request = MakeShared<FJsonObject>();
	for (const TCHAR* Key : {TEXT("system"), TEXT("graph"), TEXT("nodes"), TEXT("connections")})
		if (Params && Params->HasField(Key)) Request->SetField(Key, Params->Values[Key]);
	return Request;
}
bool Resolve(const FJson& Params, FEdit& Edit, FString& Error)
{
	const FString SystemPath = String(Params, TEXT("system")), GraphPath = String(Params, TEXT("graph"));
	if (!SystemPath.StartsWith(TEXT("/Game/")) || SystemPath.Contains(TEXT("..")) || GraphPath.IsEmpty())
		return Fail(Error, TEXT("Use a /Game/ Niagara System and an exact owned graph path from graph.inspect."));
	Edit.System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
	if (!Edit.System) return Fail(Error, TEXT("Niagara System was not found."));
	TArray<UObject*> Objects;
	GetObjectsWithOuter(Edit.System, Objects, true);
	for (UObject* Object : Objects)
		if (Object->GetPathName() == GraphPath) Edit.Graph = Cast<UNiagaraGraph>(Object);
	if (!Edit.Graph || Edit.Graph->GetOutermost() != Edit.System->GetOutermost()
		|| !Edit.Graph->GetSchema() || !Edit.Graph->GetSchema()->IsA<UEdGraphSchema_Niagara>())
		return Fail(Error, TEXT("Graph is not an owned Niagara graph. Shared/external graphs are not editable."));
	if (Edit.Graph->Nodes.Num() > 4096) return Fail(Error, TEXT("Graph exceeds the 4096-node edit limit."));
	int32 Pins = 0;
	TSet<FGuid> Ids;
	for (UEdGraphNode* Node : Edit.Graph->Nodes)
	{
		if (!Node || !Node->NodeGuid.IsValid() || Ids.Contains(Node->NodeGuid))
			return Fail(Error, TEXT("Graph has invalid or duplicate node identities."));
		Ids.Add(Node->NodeGuid); Pins += Node->Pins.Num();
	}
	return Pins <= 65536 || Fail(Error, TEXT("Graph exceeds the 65536-pin edit limit."));
}
UEdGraphNode* FindNode(UNiagaraGraph* Graph, const FGuid& Id)
{
	for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Id) return Node;
	return nullptr;
}
UEdGraphPin* FindPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const FString& Name, const FString& Id = FString())
{
	UEdGraphPin* Found = nullptr;
	if (!Node) return nullptr;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->Direction != Direction || Pin->bOrphanedPin) continue;
		if (Id.IsEmpty() ? Pin->PinName.ToString() != Name : Guid(Pin->PinId) != Id) continue;
		if (Found) return nullptr; // Ambiguous names (e.g. None) require pinId.
		Found = Pin;
	}
	return Found;
}
UEdGraphPin* Endpoint(UNiagaraGraph* Graph, const FJson& Ref, EEdGraphPinDirection Direction,
	const FJournal& Journal, FString& Error)
{
	const FString Alias = String(Ref, TEXT("nodeRef")), Existing = String(Ref, TEXT("nodeGuid"));
	FGuid Id;
	if (Alias.IsEmpty() == Existing.IsEmpty()) { Fail(Error, TEXT("Specify exactly one of nodeRef and nodeGuid.")); return nullptr; }
	if (!Alias.IsEmpty())
	{
		const FGuid* Found = Journal.Added.Find(Alias);
		if (!Found) { Fail(Error, TEXT("Unknown new-node ref: ") + Alias); return nullptr; }
		Id = *Found;
	}
	else if (!FGuid::Parse(Existing, Id)) { Fail(Error, TEXT("Invalid nodeGuid.")); return nullptr; }
	const FString Name = String(Ref, TEXT("pinName")), PinId = String(Ref, TEXT("pinId"));
	if (Name.IsEmpty() == PinId.IsEmpty()) { Fail(Error, TEXT("Specify exactly one of pinName and pinId.")); return nullptr; }
	UEdGraphPin* Pin = FindPin(FindNode(Graph, Id), Direction, Name, PinId);
	if (!Pin) { Fail(Error, TEXT("Pin not found, wrong direction, or ambiguous in this graph.")); return nullptr; }
	if (Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc || UEdGraphSchema_Niagara::IsPinStatic(Pin))
	{ Fail(Error, TEXT("Dynamic Add, wildcard and static pins are not admitted.")); return nullptr; }
	// These connections can reconstruct original nodes. Keep this edit additive.
	if (Alias.IsEmpty() && UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetGenericNumericDef())
	{ Fail(Error, TEXT("Specialize the existing numeric pin before connecting it.")); return nullptr; }
	return Pin;
}
FJson PinJson(const UEdGraphPin* Pin)
{
	FJson Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("pinName"), Pin->PinName.ToString());
	Result->SetStringField(TEXT("pinId"), Guid(Pin->PinId));
	Result->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
	Result->SetStringField(TEXT("type"), UEdGraphSchema_Niagara::PinToTypeDefinition(Pin).GetName());
	Result->SetStringField(TEXT("defaultValue"), Pin->DefaultValue);
	return Result;
}
FString Fingerprint(UNiagaraGraph* Graph)
{
	TArray<FString> Rows;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		FJson Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("guid"), Guid(Node->NodeGuid));
		Row->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
		Row->SetNumberField(TEXT("x"), Node->NodePosX); Row->SetNumberField(TEXT("y"), Node->NodePosY);
		Row->SetNumberField(TEXT("enabled"), static_cast<int32>(Node->GetDesiredEnabledState()));
		if (UNiagaraNodeOp* Op = Cast<UNiagaraNodeOp>(Node))
		{ Row->SetStringField(TEXT("op"), Op->OpName.ToString()); Row->SetBoolField(TEXT("allStatic"), Op->bAllStatic); }
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			FJson P = PinJson(Pin);
			P->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
			P->SetStringField(TEXT("subCategory"), Pin->PinType.PinSubCategory.ToString());
			P->SetStringField(TEXT("typeObject"), GetPathNameSafe(Pin->PinType.PinSubCategoryObject.Get()));
			P->SetStringField(TEXT("defaultObject"), GetPathNameSafe(Pin->DefaultObject));
			P->SetStringField(TEXT("defaultText"), Pin->DefaultTextValue.ToString());
			P->SetStringField(TEXT("autoDefault"), Pin->AutogeneratedDefaultValue);
			P->SetBoolField(TEXT("orphan"), Pin->bOrphanedPin);
			TArray<FString> Links;
			for (UEdGraphPin* Other : Pin->LinkedTo)
				if (Other) Links.Add(Guid(Other->GetOwningNode()->NodeGuid) + TEXT(":") + Guid(Other->PinId));
			Links.Sort();
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Link : Links) Values.Add(MakeShared<FJsonValueString>(Link));
			P->SetArrayField(TEXT("links"), Values); Pins.Add(MakeShared<FJsonValueObject>(P));
		}
		Row->SetArrayField(TEXT("pins"), Pins); Rows.Add(CanonicalizeJson(Row));
	}
	Rows.Sort();
	FJson State = MakeShared<FJsonObject>(); TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& Row : Rows) Values.Add(MakeShared<FJsonValueString>(Row));
	State->SetArrayField(TEXT("nodes"), Values);
	FString Digest; TryDigestJson(State, Digest); return Digest;
}
TMap<FString, TSharedPtr<FNiagaraAction_NewNode>> Operations(UNiagaraGraph* Graph, UEdGraph* Owner)
{
	TMap<FString, TSharedPtr<FNiagaraAction_NewNode>> Result;
	for (const auto& Action : GetDefault<UEdGraphSchema_Niagara>()->GetGraphActions(Graph, nullptr, Owner))
		if (UNiagaraNodeOp* Op = Cast<UNiagaraNodeOp>(Action->WeakNodeTemplate.Get())) Result.Add(Op->OpName.ToString(), Action);
	return Result;
}
bool SetInput(UNiagaraNode* Node, const FJson& Input, FString& Error)
{
	const FString Name = String(Input, TEXT("pinName"));
	UEdGraphPin* Pin = FindPin(Node, EGPD_Input, Name);
	if (!Pin || Pin->LinkedTo.Num() || Pin->bDefaultValueIsReadOnly || Pin->bDefaultValueIsIgnored
		|| Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryMisc)
		return Fail(Error, TEXT("Default requires an editable, unlinked input: ") + Name);
	const FString Type = String(Input, TEXT("type"));
	if (!Type.IsEmpty())
	{
		FNiagaraTypeDefinition Desired;
		if (Type == TEXT("int")) Desired = FNiagaraTypeDefinition::GetIntDef();
		else if (Type == TEXT("float")) Desired = FNiagaraTypeDefinition::GetFloatDef();
		else return Fail(Error, TEXT("Numeric specialization supports int or float."));
		if (UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) != Desired && !Node->ConvertNumericPinToType(Pin, Desired))
			return Fail(Error, TEXT("Niagara rejected numeric specialization."));
		Pin = FindPin(Node, EGPD_Input, Name); // Native conversion reallocates pins.
		if (!Pin) return Fail(Error, TEXT("Pin disappeared during specialization."));
	}
	FString Value;
	if (!Input->TryGetStringField(TEXT("value"), Value)) return Fail(Error, TEXT("Input value must be a serialized string."));
	const FNiagaraTypeDefinition Def = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
	if (Def == FNiagaraTypeDefinition::GetBoolDef())
	{ if (Value != TEXT("true") && Value != TEXT("false")) return Fail(Error, TEXT("Use true or false for bool.")); }
	else if (Def == FNiagaraTypeDefinition::GetIntDef())
	{ int32 Parsed; if (!LexTryParseString(Parsed, *Value) || LexToString(Parsed) != Value) return Fail(Error, TEXT("Invalid int32 default.")); }
	else if (Def == FNiagaraTypeDefinition::GetFloatDef())
	{ double Parsed; if (!LexTryParseString(Parsed, *Value) || !FMath::IsFinite(Parsed) || FMath::Abs(Parsed) > MAX_flt) return Fail(Error, TEXT("Invalid finite float default.")); }
	else return Fail(Error, TEXT("Explicit defaults support bool, int and float. Specialize generic numeric pins first."));
	Node->GetSchema()->TrySetDefaultValue(*Pin, Value);
	Pin = FindPin(Node, EGPD_Input, Name);
	return Pin && Pin->DefaultValue == Value ? true : Fail(Error, TEXT("Default read-back mismatch."));
}
bool Mutate(UNiagaraGraph* Graph, const FJson& Request, FJournal& Journal, FString& Error)
{
	TArray<TSharedPtr<FJsonValue>> Nodes, Connections;
	if (!Array(Request, TEXT("nodes"), MaxNodes, Nodes, Error)
		|| !Array(Request, TEXT("connections"), MaxConnections, Connections, Error)) return false;
	if (Nodes.IsEmpty() && Connections.IsEmpty()) return Fail(Error, TEXT("The edit is empty."));
	TStrongObjectPtr<UEdGraph> Templates(NewObject<UEdGraph>(GetTransientPackage()));
	const auto Catalog = Operations(Graph, Templates.Get());
	for (const auto& Value : Nodes)
	{
		const FJson Spec = Value->AsObject();
		const FString Ref = String(Spec, TEXT("ref")), Op = String(Spec, TEXT("operation"));
		if (Ref.IsEmpty() || Ref.Len() > 64 || Journal.Added.Contains(Ref)) return Fail(Error, TEXT("Node refs must be unique, nonempty and at most 64 characters."));
		const auto* Action = Catalog.Find(Op);
		if (!Action) return Fail(Error, TEXT("Operation unavailable; query operations.list: ") + Op);
		double X = 0, Y = 0; Spec->TryGetNumberField(TEXT("x"), X); Spec->TryGetNumberField(TEXT("y"), Y);
		if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || FMath::Abs(X) > 1000000 || FMath::Abs(Y) > 1000000)
			return Fail(Error, TEXT("Node coordinates must be finite and within +/-1000000."));
		UNiagaraNode* Template = Cast<UNiagaraNode>((*Action)->WeakNodeTemplate.Get());
		if (!Template || !Template->CanAddToGraph(Graph, Error)) return false;
		// Menu actions consume their template and start an editor transaction.
		// Clone each template so repeated operations create distinct nodes and a
		// transient plan never starts an Undo transaction or selects a node.
		UNiagaraNode* Node = DuplicateObject<UNiagaraNode>(Template, Graph);
		Node->SetFlags(RF_Transactional);
		Graph->AddNode(Node, false, false);
		Node->CreateNewGuid(); Node->PostPlacedNewNode(); Node->AllocateDefaultPins();
		Node->NodePosX = static_cast<int32>(X); Node->NodePosY = static_cast<int32>(Y);
		Journal.Added.Add(Ref, Node->NodeGuid);
		if (Spec->HasField(TEXT("inputs")))
		{
			TArray<TSharedPtr<FJsonValue>> Inputs;
			if (!Array(Spec, TEXT("inputs"), 32, Inputs, Error)) return false;
			TSet<FString> Names;
			for (const auto& Input : Inputs)
			{
				const FString Name = String(Input->AsObject(), TEXT("pinName"));
				if (Names.Contains(Name)) return Fail(Error, TEXT("Duplicate input default."));
				Names.Add(Name);
				if (!SetInput(Node, Input->AsObject(), Error)) return false;
			}
		}
	}
	for (const auto& Value : Connections)
	{
		const FJson Spec = Value->AsObject(); const FJson* FromRef = nullptr; const FJson* ToRef = nullptr;
		if (!Spec->TryGetObjectField(TEXT("from"), FromRef) || !Spec->TryGetObjectField(TEXT("to"), ToRef))
			return Fail(Error, TEXT("Each connection needs from and to references."));
		UEdGraphPin* From = Endpoint(Graph, *FromRef, EGPD_Output, Journal, Error);
		if (!From) return false;
		UEdGraphPin* To = Endpoint(Graph, *ToRef, EGPD_Input, Journal, Error);
		if (!To) return false;
		if (To->LinkedTo.Num() != 0) return Fail(Error, TEXT("Input already connected; replacement and duplicate edges are refused."));
		const UEdGraphSchema* Schema = Graph->GetSchema();
		const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
		if (Response.Response != CONNECT_RESPONSE_MAKE)
			return Fail(Error, TEXT("Niagara rejected a direct connection (conversion/replacement is not implicit): ") + Response.Message.ToString());
		FEdge Edge{From->GetOwningNode()->NodeGuid, From->PinId, To->GetOwningNode()->NodeGuid, To->PinId};
		if (!Schema->TryCreateConnection(From, To)) return Fail(Error, TEXT("Niagara connection failed."));
		Journal.Edges.Add(Edge);
	}
	Graph->NotifyGraphChanged(); return true;
}
bool Verify(UNiagaraGraph* Graph, const FJournal& Journal)
{
	for (const auto& Entry : Journal.Added) if (!FindNode(Graph, Entry.Value)) return false;
	for (const FEdge& Edge : Journal.Edges)
	{
		UEdGraphPin* From = FindPin(FindNode(Graph, Edge.FromNode), EGPD_Output, FString(), Guid(Edge.FromPin));
		UEdGraphPin* To = FindPin(FindNode(Graph, Edge.ToNode), EGPD_Input, FString(), Guid(Edge.ToPin));
		if (!From || !To || To->LinkedTo.Num() != 1 || !From->LinkedTo.Contains(To) || !To->LinkedTo.Contains(From)) return false;
	}
	return true;
}
bool Restore(UNiagaraGraph* Graph, const FJournal& Journal, const FString& Before)
{
	for (int32 Index = Journal.Edges.Num() - 1; Index >= 0; --Index)
	{
		const FEdge& Edge = Journal.Edges[Index];
		UEdGraphPin* From = FindPin(FindNode(Graph, Edge.FromNode), EGPD_Output, FString(), Guid(Edge.FromPin));
		UEdGraphPin* To = FindPin(FindNode(Graph, Edge.ToNode), EGPD_Input, FString(), Guid(Edge.ToPin));
		if (From && To && From->LinkedTo.Contains(To)) Graph->GetSchema()->BreakSinglePinLink(From, To);
	}
	for (const auto& Entry : Journal.Added)
		if (UEdGraphNode* Node = FindNode(Graph, Entry.Value))
		{ Node->Modify(); Node->BreakAllNodeLinks(); Graph->RemoveNode(Node); }
	Graph->NotifyGraphChanged(); return Fingerprint(Graph) == Before;
}
bool Compile(UNiagaraSystem* System)
{
	System->RequestCompile(false); System->WaitForCompilationComplete(true, false);
	TArray<UNiagaraScript*> Scripts{System->GetSystemSpawnScript(), System->GetSystemUpdateScript()};
	for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
		if (Handle.GetIsEnabled()) if (auto* Data = Handle.GetEmitterData())
		{ TArray<UNiagaraScript*> EmitterScripts; Data->GetScripts(EmitterScripts, false, false); Scripts.Append(EmitterScripts); }
	bool Known = false;
	for (UNiagaraScript* Script : Scripts)
	{
		if (!Script) continue;
		const auto Status = Script->GetLastCompileStatus();
		if (Status == ENiagaraScriptCompileStatus::NCS_Error || Status == ENiagaraScriptCompileStatus::NCS_Dirty
			|| Status == ENiagaraScriptCompileStatus::NCS_BeingCreated) return false;
		Known |= Status == ENiagaraScriptCompileStatus::NCS_UpToDate
			|| Status == ENiagaraScriptCompileStatus::NCS_UpToDateWithWarnings
			|| Status == ENiagaraScriptCompileStatus::NCS_ComputeUpToDateWithWarnings;
	}
	return Known;
}
FJson AddedJson(UNiagaraGraph* Graph, const FJournal& Journal)
{
	FJson Result = MakeShared<FJsonObject>();
	for (const auto& Entry : Journal.Added)
	{
		UEdGraphNode* Node = FindNode(Graph, Entry.Value); if (!Node) continue;
		FJson N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("nodeGuid"), Guid(Node->NodeGuid)); N->SetStringField(TEXT("nodePath"), Node->GetPathName());
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (UEdGraphPin* Pin : Node->Pins) if (Pin) Pins.Add(MakeShared<FJsonValueObject>(PinJson(Pin)));
		N->SetArrayField(TEXT("pins"), Pins); Result->SetObjectField(Entry.Key, N);
	}
	return Result;
}
bool Plan(const FJson& Params, FEdit& Edit, FString& Error)
{
	if (!Resolve(Params, Edit, Error)) return false;
	Edit.Request = EditRequest(Params);
	if (!TryDigestJson(Edit.Request, Edit.RequestDigest)) return Fail(Error, TEXT("Cannot digest request."));
	Edit.Before = Fingerprint(Edit.Graph); Edit.ChangeId = Guid(Edit.Graph->GetChangeID());
	TStrongObjectPtr<UNiagaraScript> Owner(NewObject<UNiagaraScript>(GetTransientPackage()));
	UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Owner.Get());
	Owner->SetLatestSource(Source);
	UNiagaraGraph* Shadow = DuplicateObject<UNiagaraGraph>(Edit.Graph, Source); Source->NodeGraph = Shadow;
	FJournal Journal;
	if (!Mutate(Shadow, Edit.Request, Journal, Error) || !Verify(Shadow, Journal)) return false;
	if (Edit.Before != Fingerprint(Edit.Graph) || Edit.ChangeId != Guid(Edit.Graph->GetChangeID()))
		return Fail(Error, TEXT("Source graph changed during planning; re-inspect before continuing."));
	Edit.Plan = MakeShared<FJsonObject>();
	Edit.Plan->SetStringField(TEXT("schema"), TEXT("ue.change-plan.v1"));
	Edit.Plan->SetStringField(TEXT("planKind"), TEXT("niagaraGraphEdit")); Edit.Plan->SetStringField(TEXT("status"), TEXT("planned"));
	Edit.Plan->SetObjectField(TEXT("edit"), Edit.Request);
	Edit.Plan->SetStringField(TEXT("graphChangeId"), Edit.ChangeId); Edit.Plan->SetStringField(TEXT("graphFingerprint"), Edit.Before);
	Edit.Plan->SetBoolField(TEXT("confirmWriteRequired"), true); Edit.Plan->SetBoolField(TEXT("compiled"), false);
	Edit.Plan->SetStringField(TEXT("persistence"), TEXT("dirtyOnly")); Edit.Plan->SetStringField(TEXT("rollbackBoundary"), TEXT("sameEditorInstance"));
	if (!TryDigestJson(Edit.Plan, Edit.Digest)) return Fail(Error, TEXT("Cannot digest plan."));
	Edit.Plan->SetStringField(TEXT("planDigest"), Edit.Digest); return true;
}
FJson Result(const FReceipt& Receipt, bool Replay)
{
	FJson Value = MakeShared<FJsonObject>();
	Value->SetStringField(TEXT("receiptId"), Receipt.Id); Value->SetStringField(TEXT("requestId"), Receipt.RequestId);
	Value->SetStringField(TEXT("planDigest"), Receipt.Digest); Value->SetStringField(TEXT("status"), TEXT("succeeded"));
	Value->SetBoolField(TEXT("compiled"), true); Value->SetBoolField(TEXT("verified"), true); Value->SetBoolField(TEXT("saved"), false);
	Value->SetBoolField(TEXT("rolledBack"), Receipt.bRolledBack); Value->SetBoolField(TEXT("idempotentReplay"), Replay);
	Value->SetStringField(TEXT("rollbackDurability"), TEXT("session"));
	if (Receipt.Graph.IsValid()) Value->SetObjectField(TEXT("nodes"), AddedJson(Receipt.Graph.Get(), Receipt.Journal));
	return Value;
}
class FOperations final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.operations.list"); }
	FMCPToolResult Execute(const FJson& Params) override
	{
		FEdit Edit; FString Message; if (!Resolve(Params, Edit, Message)) return Error(Message);
		TStrongObjectPtr<UEdGraph> Owner(NewObject<UEdGraph>(GetTransientPackage()));
		const auto Catalog = Operations(Edit.Graph, Owner.Get()); TArray<FString> Names; Catalog.GetKeys(Names); Names.Sort();
		const FString Query = String(Params, TEXT("query"));
		int32 Limit = 32, Offset = 0; Params->TryGetNumberField(TEXT("limit"), Limit); Params->TryGetNumberField(TEXT("offset"), Offset);
		if (Limit < 1 || Limit > 128 || Offset < 0) return Error(TEXT("Invalid pagination."));
		TArray<TSharedPtr<FJsonValue>> Items; int32 Total = 0;
		for (const FString& Name : Names)
		{
			UEdGraphNode* Node = Catalog[Name]->WeakNodeTemplate.Get(); if (!Node) continue;
			const FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (!Query.IsEmpty() && !Name.Contains(Query) && !Title.Contains(Query)) continue;
			const int32 Index = Total++; if (Index < Offset || Items.Num() >= Limit) continue;
			Node->AllocateDefaultPins(); FJson Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("operation"), Name); Item->SetStringField(TEXT("title"), Title);
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (UEdGraphPin* Pin : Node->Pins)
				if (Pin && Pin->PinType.PinCategory != UEdGraphSchema_Niagara::PinCategoryMisc)
				{ FJson P = PinJson(Pin); P->RemoveField(TEXT("pinId")); Pins.Add(MakeShared<FJsonValueObject>(P)); }
			Item->SetArrayField(TEXT("pins"), Pins); Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		FJson Value = MakeShared<FJsonObject>(); Value->SetArrayField(TEXT("operations"), Items);
		Value->SetNumberField(TEXT("total"), Total); Value->SetNumberField(TEXT("offset"), Offset);
		Value->SetBoolField(TEXT("hasMore"), Offset + Items.Num() < Total); return FMCPToolResult::Ok(Value);
	}
};
class FPlan final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.edit.plan"); }
	FMCPToolResult Execute(const FJson& Params) override
	{ FEdit Edit; FString Message; return Plan(Params, Edit, Message) ? FMCPToolResult::Ok(Edit.Plan) : Error(Message); }
};
class FApply final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.edit.apply"); }
	FMCPToolResult Execute(const FJson& Params) override
	{
		const FString RequestId = String(Params, TEXT("requestId")); FString Code, Message, RequestDigest;
		if (!TryDigestJson(EditRequest(Params), RequestDigest)) return Error(TEXT("Cannot digest request."));
		if (const FString* ReceiptId = RequestReceipts.Find(RequestId))
		{
			FReceipt& Receipt = Receipts.FindChecked(*ReceiptId);
			if (RequestDigest != Receipt.RequestDigest) return Error(TEXT("requestId was used for a different edit."), TEXT("request_id_conflict"));
			if (!ValidateChangeApproval(Params, Receipt.Digest, Code, Message)) return Error(Message, Code);
			if (Receipt.bRolledBack || !Receipt.Graph.IsValid() || Guid(Receipt.Graph->GetChangeID()) != Receipt.ChangeIdAfter
				|| Fingerprint(Receipt.Graph.Get()) != Receipt.After)
				return Error(TEXT("Prior edit is no longer current; use a new plan and requestId."), TEXT("receipt_state_changed"));
			return FMCPToolResult::Ok(Result(Receipt, true));
		}
		if (Receipts.Num() >= MaxReceipts) return Error(TEXT("Session receipt capacity reached."), TEXT("receipt_capacity"));
		FEdit Edit; if (!Plan(Params, Edit, Message)) return Error(Message);
		if (!ValidateChangeApproval(Params, Edit.Digest, Code, Message)) return Error(Message, Code);
		const bool WasDirty = Edit.System->GetOutermost()->IsDirty();
		FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Edit Niagara Graph")));
		Edit.System->Modify(); Edit.Graph->Modify(); FReceipt Receipt;
		Receipt.System = Edit.System; Receipt.Graph = Edit.Graph; Receipt.RequestId = RequestId;
		Receipt.RequestDigest = RequestDigest; Receipt.Digest = Edit.Digest; Receipt.Id = Guid(FGuid::NewGuid()); Receipt.Before = Edit.Before;
		const bool Changed = Mutate(Edit.Graph, Edit.Request, Receipt.Journal, Message);
		const bool Compiled = Changed && Compile(Edit.System);
		if (!Compiled || !Verify(Edit.Graph, Receipt.Journal))
		{
			const bool Restored = Restore(Edit.Graph, Receipt.Journal, Edit.Before);
			if (!Restored || !Compile(Edit.System)) return Error(TEXT("Edit failed; restoration was not verified. Editor Undo was retained."), TEXT("restore_verification_failed"));
			Transaction.Cancel(); Edit.System->GetOutermost()->SetDirtyFlag(WasDirty);
			return Error(Changed ? TEXT("Compilation/read-back failed; graph additions were restored.") : Message, Changed ? TEXT("compile_failed") : TEXT("edit_failed"));
		}
		Receipt.After = Fingerprint(Edit.Graph); Receipt.ChangeIdAfter = Guid(Edit.Graph->GetChangeID()); Edit.System->MarkPackageDirty();
		Receipts.Add(Receipt.Id, Receipt); RequestReceipts.Add(RequestId, Receipt.Id); return FMCPToolResult::Ok(Result(Receipt, false));
	}
};
class FRollback final : public FMCPToolBase
{
public:
	FString GetCapabilityId() const override { return TEXT("content.niagara.graph.edit.rollback"); }
	FMCPToolResult Execute(const FJson& Params) override
	{
		bool Confirm = false;
		if (!Params || !Params->TryGetBoolField(TEXT("confirmWrite"), Confirm) || !Confirm)
			return Error(TEXT("confirmWrite=true is required."), TEXT("write_confirmation_required"));
		FReceipt* Receipt = Receipts.Find(String(Params, TEXT("rollbackId")));
		if (!Receipt) return Error(TEXT("Receipt is not known in this Editor."), TEXT("receipt_not_found"));
		if (Receipt->RequestId != String(Params, TEXT("requestId"))) return Error(TEXT("requestId does not match receipt."), TEXT("request_id_mismatch"));
		UNiagaraGraph* Graph = Receipt->Graph.Get(); UNiagaraSystem* System = Receipt->System.Get();
		if (!Graph || !System || Fingerprint(Graph) != Receipt->After || Guid(Graph->GetChangeID()) != Receipt->ChangeIdAfter)
			return Error(TEXT("Graph changed after receipt; rollback refused."), TEXT("rollback_conflict"));
		if (Receipt->bRolledBack) return FMCPToolResult::Ok(Result(*Receipt, true));
		FScopedTransaction Transaction(FText::FromString(TEXT("UE AI Rollback Niagara Graph Edit")));
		System->Modify(); Graph->Modify();
		if (!Restore(Graph, Receipt->Journal, Receipt->Before) || !Compile(System))
			return Error(TEXT("Rollback verification failed; Editor Undo was retained."), TEXT("rollback_verification_failed"));
		Receipt->bRolledBack = true; Receipt->After = Fingerprint(Graph); Receipt->ChangeIdAfter = Guid(Graph->GetChangeID());
		System->MarkPackageDirty(); return FMCPToolResult::Ok(Result(*Receipt, false));
	}
};

#if WITH_DEV_AUTOMATION_TESTS
namespace Tests
{
FJson Node(const TCHAR* Ref, const TCHAR* Operation)
{
	FJson N = MakeShared<FJsonObject>(); N->SetStringField(TEXT("ref"), Ref); N->SetStringField(TEXT("operation"), Operation); return N;
}
FJson Pin(const TCHAR* Ref, const TCHAR* Name)
{
	FJson P = MakeShared<FJsonObject>(); P->SetStringField(TEXT("nodeRef"), Ref); P->SetStringField(TEXT("pinName"), Name); return P;
}
FJson Edge(const TCHAR* From, const TCHAR* Output, const TCHAR* To, const TCHAR* Input)
{
	FJson E = MakeShared<FJsonObject>(); E->SetObjectField(TEXT("from"), Pin(From, Output)); E->SetObjectField(TEXT("to"), Pin(To, Input)); return E;
}
void SetArray(const FJson& Object, const TCHAR* Name, std::initializer_list<FJson> Entries)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FJson& Entry : Entries) Values.Add(MakeShared<FJsonValueObject>(Entry)); Object->SetArrayField(Name, Values);
}
FJson Request()
{
	FJson R = MakeShared<FJsonObject>();
	SetArray(R, TEXT("nodes"), {Node(TEXT("left"), TEXT("Boolean::LogicNot")), Node(TEXT("right"), TEXT("Boolean::LogicNot"))});
	SetArray(R, TEXT("connections"), {Edge(TEXT("left"), TEXT("Result"), TEXT("right"), TEXT("A"))}); return R;
}
UNiagaraGraph* Graph(UNiagaraScript* Owner)
{
	UNiagaraScriptSource* Source = NewObject<UNiagaraScriptSource>(Owner); Owner->SetLatestSource(Source);
	UNiagaraGraph* G = NewObject<UNiagaraGraph>(Source); Source->NodeGraph = G;
	UNiagaraNodeOutput* Output = NewObject<UNiagaraNodeOutput>(G); Output->SetUsage(ENiagaraScriptUsage::Module);
	G->AddNode(Output, false, false); Output->CreateNewGuid(); return G;
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraGraphAdditiveTest, "UE_AI_integration.Niagara.GraphEdit.AdditiveRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUEAINiagaraGraphAdditiveTest::RunTest(const FString&)
{
	TStrongObjectPtr<UNiagaraScript> Owner(NewObject<UNiagaraScript>(GetTransientPackage()));
	UNiagaraGraph* Graph = Tests::Graph(Owner.Get()); const FString Before = Fingerprint(Graph);
	FJournal Journal; FString Message;
	if (!TestTrue(TEXT("Two nodes and connection created"), Mutate(Graph, Tests::Request(), Journal, Message))) { AddError(Message); return false; }
	TestEqual(TEXT("Repeated operation creates distinct nodes"), Journal.Added.Num(), 2);
	TestNotEqual(TEXT("Distinct native identities"), Journal.Added[TEXT("left")], Journal.Added[TEXT("right")]);
	TestTrue(TEXT("All edges read back"), Verify(Graph, Journal));
	TestTrue(TEXT("Native removal restores original nodes, pins and links"), Restore(Graph, Journal, Before));
	TestEqual(TEXT("Only original output remains"), Graph->Nodes.Num(), 1);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraGraphRejectTest, "UE_AI_integration.Niagara.GraphEdit.ConnectionRejection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUEAINiagaraGraphRejectTest::RunTest(const FString&)
{
	for (int32 Case = 0; Case < 5; ++Case)
	{
		TStrongObjectPtr<UNiagaraScript> Owner(NewObject<UNiagaraScript>(GetTransientPackage()));
		UNiagaraGraph* Graph = Tests::Graph(Owner.Get()); const FString Before = Fingerprint(Graph);
		FJson Request = Tests::Request();
		if (Case == 0) Tests::SetArray(Request, TEXT("connections"), {Tests::Edge(TEXT("left"), TEXT("Result"), TEXT("right"), TEXT("A")), Tests::Edge(TEXT("right"), TEXT("Result"), TEXT("left"), TEXT("A"))});
		if (Case == 1) Tests::SetArray(Request, TEXT("connections"), {Tests::Edge(TEXT("left"), TEXT("Result"), TEXT("right"), TEXT("A")), Tests::Edge(TEXT("left"), TEXT("Result"), TEXT("right"), TEXT("A"))});
		if (Case == 2) Tests::SetArray(Request, TEXT("connections"), {Tests::Edge(TEXT("unknown"), TEXT("Result"), TEXT("right"), TEXT("A"))});
		if (Case == 3) Tests::SetArray(Request, TEXT("nodes"), {Tests::Node(TEXT("left"), TEXT("Boolean::LogicNot")), Tests::Node(TEXT("right"), TEXT("NoSuchOperation"))});
		if (Case == 4) Tests::SetArray(Request, TEXT("connections"), {Tests::Edge(TEXT("left"), TEXT("A"), TEXT("right"), TEXT("Result"))});
		FJournal Journal; FString Message;
		TestFalse(FString::Printf(TEXT("Invalid graph edit rejected, case %d"), Case), Mutate(Graph, Request, Journal, Message));
		TestFalse(TEXT("Rejection explains the problem"), Message.IsEmpty());
		TestTrue(TEXT("Partial additions restore without deleting previous edges"), Restore(Graph, Journal, Before));
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraGraphExistingTest, "UE_AI_integration.Niagara.GraphEdit.ExistingConnections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUEAINiagaraGraphExistingTest::RunTest(const FString&)
{
	TStrongObjectPtr<UNiagaraScript> Owner(NewObject<UNiagaraScript>(GetTransientPackage()));
	UNiagaraGraph* Graph = Tests::Graph(Owner.Get());
	FJson Seed = Tests::Request(); Tests::SetArray(Seed, TEXT("connections"), {});
	FJournal SeedJournal; FString Message;
	if (!TestTrue(TEXT("Seed unlinked nodes"), Mutate(Graph, Seed, SeedJournal, Message))) return false;
	const FString Unlinked = Fingerprint(Graph);
	FJson From = MakeShared<FJsonObject>(), To = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("nodeGuid"), Guid(SeedJournal.Added[TEXT("left")])); From->SetStringField(TEXT("pinName"), TEXT("Result"));
	To->SetStringField(TEXT("nodeGuid"), Guid(SeedJournal.Added[TEXT("right")])); To->SetStringField(TEXT("pinName"), TEXT("A"));
	FJson Edge = MakeShared<FJsonObject>(); Edge->SetObjectField(TEXT("from"), From); Edge->SetObjectField(TEXT("to"), To);
	FJson Request = MakeShared<FJsonObject>(); Tests::SetArray(Request, TEXT("nodes"), {}); Tests::SetArray(Request, TEXT("connections"), {Edge});
	FJournal ConnectionJournal;
	if (!TestTrue(TEXT("Connect existing pins without adding nodes"), Mutate(Graph, Request, ConnectionJournal, Message))) { AddError(Message); return false; }
	TestEqual(TEXT("Connection-only journal adds no nodes"), ConnectionJournal.Added.Num(), 0);
	TestTrue(TEXT("Existing pins read back"), Verify(Graph, ConnectionJournal));
	const FString Linked = Fingerprint(Graph);
	FJson Addition = Tests::Node(TEXT("third"), TEXT("Boolean::LogicNot"));
	FJson Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("pinName"), TEXT("A")); Input->SetStringField(TEXT("value"), TEXT("true"));
	Tests::SetArray(Addition, TEXT("inputs"), {Input}); Tests::SetArray(Request, TEXT("nodes"), {Addition});
	Edge->SetObjectField(TEXT("from"), Tests::Pin(TEXT("third"), TEXT("Result")));
	FJournal RejectedJournal;
	TestFalse(TEXT("Occupied original input rejects replacement"), Mutate(Graph, Request, RejectedJournal, Message));
	TestEqual(TEXT("Refused edge is never journaled"), RejectedJournal.Edges.Num(), 0);
	TestTrue(TEXT("Failed edit preserves the pre-existing edge"), Restore(Graph, RejectedJournal, Linked));
	TestTrue(TEXT("Original edge still verifies"), Verify(Graph, ConnectionJournal));
	TestTrue(TEXT("Connection-only rollback preserves both original nodes"), Restore(Graph, ConnectionJournal, Unlinked));
	TestEqual(TEXT("Original nodes survive rollback"), Graph->Nodes.Num(), 3);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEAINiagaraGraphPlanTest, "UE_AI_integration.Niagara.GraphEdit.PlanApplyRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUEAINiagaraGraphPlanTest::RunTest(const FString&)
{
	UPackage* Package = CreatePackage(*(TEXT("/Game/Automation/UEAIGraphEdit_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	TStrongObjectPtr<UNiagaraSystem> System(NewObject<UNiagaraSystem>(Package, TEXT("System"), RF_Public | RF_Standalone | RF_Transactional));
	UNiagaraSystemFactoryNew::InitializeSystem(System.Get(), true);
	UNiagaraGraph* Graph = CastChecked<UNiagaraScriptSource>(System->GetSystemSpawnScript()->GetLatestSource())->NodeGraph;
	FJson Request = Tests::Request(); Request->SetStringField(TEXT("system"), System->GetPathName()); Request->SetStringField(TEXT("graph"), Graph->GetPathName());
	const FString Before = Fingerprint(Graph); const FGuid ChangeId = Graph->GetChangeID(); Package->SetDirtyFlag(false);
	FEdit Edit; FString Message;
	if (!TestTrue(TEXT("Read-only plan"), Plan(Request, Edit, Message))) { AddError(Message); return false; }
	TestEqual(TEXT("Plan preserves graph"), Fingerprint(Graph), Before); TestEqual(TEXT("Plan preserves ChangeID"), Graph->GetChangeID(), ChangeId);
	TestFalse(TEXT("Plan leaves source package clean"), Package->IsDirty());
	Request->SetStringField(TEXT("approvePlanDigest"), Edit.Digest); Request->SetBoolField(TEXT("confirmWrite"), true);
	const FString RequestId = Guid(FGuid::NewGuid()); Request->SetStringField(TEXT("requestId"), RequestId);
	// NotifyGraphChanged() alone only refreshes caches; change real graph state.
	Graph->Nodes[0]->NodePosX += 64;
	const FString BeforeApply = Fingerprint(Graph);
	TestFalse(TEXT("Stale plan rejected"), FApply().Execute(Request).bSuccess);
	TestEqual(TEXT("Rejected plan preserves the user's edit"), Fingerprint(Graph), BeforeApply);
	if (!TestTrue(TEXT("Replan"), Plan(Request, Edit, Message))) return false;
	Request->SetStringField(TEXT("approvePlanDigest"), Edit.Digest);
	FMCPToolResult Applied = FApply().Execute(Request);
	if (!TestTrue(TEXT("Apply compiles the system"), Applied.bSuccess)) { AddError(Applied.ErrorMessage); return false; }
	TestTrue(TEXT("Identical request replays"), FApply().Execute(Request).bSuccess);
	FJson Rollback = MakeShared<FJsonObject>(); Rollback->SetStringField(TEXT("rollbackId"), Applied.Data->GetStringField(TEXT("receiptId")));
	Rollback->SetStringField(TEXT("requestId"), TEXT("wrong")); Rollback->SetBoolField(TEXT("confirmWrite"), true);
	TestFalse(TEXT("Wrong rollback owner refused"), FRollback().Execute(Rollback).bSuccess);
	Rollback->SetStringField(TEXT("requestId"), RequestId);
	FMCPToolResult Restored = FRollback().Execute(Rollback);
	TestTrue(TEXT("Rollback compiles"), Restored.bSuccess); if (!Restored.bSuccess) AddError(Restored.ErrorMessage);
	TestEqual(TEXT("Graph restored with the user's pre-apply edit"), Fingerprint(Graph), BeforeApply);
	TestTrue(TEXT("Rollback replay"), FRollback().Execute(Rollback).bSuccess);
	TestFalse(TEXT("Apply replay cannot claim an undone edit exists"), FApply().Execute(Request).bSuccess);
	Receipts.Remove(Applied.Data->GetStringField(TEXT("receiptId"))); RequestReceipts.Remove(RequestId);
	System->ClearFlags(RF_Public | RF_Standalone); Package->SetDirtyFlag(false);
	return true;
}
#endif
}
namespace UEAIIntegrationTools
{
void RegisterNiagaraGraphEditTools(FMCPToolRegistry& Registry)
{
	using namespace UEAINiagaraEditPrivate;
	Registry.Register(MakeShared<FOperations>()); Registry.Register(MakeShared<FPlan>());
	Registry.Register(MakeShared<FApply>()); Registry.Register(MakeShared<FRollback>());
}
}
#else
namespace UEAIIntegrationTools
{
class FNiagaraEditUnavailable final : public FMCPToolBase
{
	FString Id;
public:
	explicit FNiagaraEditUnavailable(FString InId) : Id(MoveTemp(InId)) {}
	FString GetCapabilityId() const override { return Id; }
	FMCPToolResult Execute(const TSharedPtr<FJsonObject>&) override
	{ return FMCPToolResult::Error(TEXT("Niagara editor support is not compiled."), TEXT("capability_unavailable"), 409); }
};
void RegisterNiagaraGraphEditTools(FMCPToolRegistry& Registry)
{
	Registry.Register(MakeShared<FNiagaraEditUnavailable>(TEXT("content.niagara.graph.operations.list")));
	Registry.Register(MakeShared<FNiagaraEditUnavailable>(TEXT("content.niagara.graph.edit.plan")));
	Registry.Register(MakeShared<FNiagaraEditUnavailable>(TEXT("content.niagara.graph.edit.apply")));
	Registry.Register(MakeShared<FNiagaraEditUnavailable>(TEXT("content.niagara.graph.edit.rollback")));
}
}
#endif
