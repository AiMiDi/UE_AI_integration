#pragma once

#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionNamedReroute.h"

namespace UEAIIntegration::MaterialEditing
{
// Authored ordinary function calls. Bounded and iterative, so malformed cycles
// are rejected before native recursive update/compilation can open a dialog.
inline bool CollectFunctionDependencies(UObject* Asset, TArray<UMaterialFunctionInterface*>& Functions, FString& Error)
{
	Functions.Reset();
	TArray<UObject*> Queue{Asset};
	TSet<UObject*> Seen{Asset};
	TMap<UObject*, int32> Indegree;
	TMap<UObject*, TArray<UObject*>> Edges;
	int32 Scanned = 0;
	for (int32 Index = 0; Index < Queue.Num(); ++Index)
	{
		UObject* Owner = Queue[Index];
		Indegree.FindOrAdd(Owner);
		TArray<UMaterialExpression*> Expressions;
		if (auto* Material = Cast<UMaterial>(Owner)) for (UMaterialExpression* E : Material->GetExpressions()) Expressions.Add(E);
		else if (auto* Function = Cast<UMaterialFunctionInterface>(Owner))
		{
			Functions.Add(Function);
			for (UMaterialExpression* E : Function->GetExpressions()) Expressions.Add(E);
		}
		if ((Scanned += Expressions.Num()) > 50000) { Error = TEXT("Function dependency inspection exceeds 50000 expressions."); return false; }
		for (auto* E : Expressions) if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(E); Call && Call->MaterialFunction)
		{
			UObject* Child = Call->MaterialFunction.Get();
			Edges.FindOrAdd(Owner).Add(Child);
			++Indegree.FindOrAdd(Child);
			if (!Seen.Contains(Child))
			{
				if (Seen.Num() >= 129) { Error = TEXT("Function dependency inspection exceeds 128 functions."); return false; }
				Seen.Add(Child); Queue.Add(Child);
			}
		}
	}
	TArray<UObject*> Ready;
	for (const auto& Pair : Indegree) if (!Pair.Value) Ready.Add(Pair.Key);
	for (int32 Index = 0; Index < Ready.Num(); ++Index)
		if (const auto* Children = Edges.Find(Ready[Index])) for (auto* Child : *Children) if (--Indegree.FindChecked(Child) == 0) Ready.Add(Child);
	if (Ready.Num() != Seen.Num()) { Error = TEXT("Material function references contain a cycle."); return false; }
	return true;
}

// A validation host must actually reference the requested function from a
// material output. This is graph reachability, not static-permutation coverage.
inline bool ReferencesFunctionFromOutputs(UMaterial* Material, UMaterialFunction* Function, FString* OutReason = nullptr)
{
	if (OutReason) *OutReason = TEXT("No supported path from a material output to this function; unused caller inputs do not establish a host.");
	auto Unverified = [&](const TCHAR* Reason) { if (OutReason) *OutReason = Reason; return false; };
	if (!Material || !Function) return false;
	struct FContext { UMaterialExpressionMaterialFunctionCall* Call; int32 Parent; };
	struct FVisit { UMaterialExpression* Expression; int32 Output; int32 Context; };
	TArray<FContext> Contexts{{nullptr, INDEX_NONE}};
	TMap<TPair<int32, UMaterialExpressionMaterialFunctionCall*>, int32> ContextIds;
	TMap<int32, TSet<TPair<UMaterialExpression*, int32>>> Seen;
	TArray<FVisit> Queue;
	int32 Inspected = 0;
	bool bLimited = false;
	auto Enqueue = [&](UMaterialExpression* E, int32 Output, int32 Context)
	{
		if (++Inspected > 50000) { bLimited = true; return; }
		if (!IsValid(E) || Output < 0 || Output >= E->GetOutputs().Num()) return;
		auto& Visited = Seen.FindOrAdd(Context);
		const TPair<UMaterialExpression*, int32> Key{E, Output};
		if (Visited.Contains(Key)) return;
		Visited.Add(Key); Queue.Add({E, Output, Context});
	};
	for (int32 I = 0; I < MP_MAX; ++I)
		if (auto* Input = Material->GetExpressionInputForProperty(static_cast<EMaterialProperty>(I)); Input && Input->Expression)
			Enqueue(Input->Expression, Input->OutputIndex, 0);
	for (int32 Index = 0; Index < Queue.Num() && !bLimited; ++Index)
	{
		const FVisit Item = Queue[Index];
		if (auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Item.Expression))
		{
			if (!IsValid(Call->MaterialFunction) || !Call->FunctionOutputs.IsValidIndex(Item.Output)) continue;
			auto* Output = Call->FunctionOutputs[Item.Output].ExpressionOutput.Get();
			if (!IsValid(Output)) continue;
			if (Call->MaterialFunction == Function || Call->MaterialFunction->GetBaseFunction() == Function) { if (OutReason) OutReason->Reset(); return true; }
			const TPair<int32, UMaterialExpressionMaterialFunctionCall*> Key{Item.Context, Call};
			int32 Child;
			if (const int32* Existing = ContextIds.Find(Key)) Child = *Existing;
			else
			{
				if (Contexts.Num() >= 1024) return Unverified(TEXT("Host reachability is unverified: exceeded 1024 call contexts."));
				// Reject recursive expansion without using the native compiler.
				for (int32 C = Item.Context; C > 0; C = Contexts[C].Parent)
					if (Contexts[C].Call->MaterialFunction == Call->MaterialFunction) return Unverified(TEXT("Host reachability is unverified: recursive function expansion."));
				Child = Contexts.Add({Call, Item.Context}); ContextIds.Add(Key, Child);
			}
			Enqueue(Output, 0, Child);
			continue; // Never walk every caller input: only inputs used by this output count.
		}
		if (auto* Input = Cast<UMaterialExpressionFunctionInput>(Item.Expression))
		{
			if (Item.Context == 0) continue;
			const FContext Context = Contexts[Item.Context];
			const FFunctionExpressionInput* Binding = nullptr;
			for (const auto& Candidate : Context.Call->FunctionInputs)
			{
				if (++Inspected > 50000) return Unverified(TEXT("Host reachability is unverified: exceeded 50000 inspection steps."));
				if (Candidate.ExpressionInput == Input)
				{
					if (Binding) return Unverified(TEXT("Host reachability is unverified: ambiguous function input binding."));
					Binding = &Candidate;
				}
			}
			if (!Binding) continue;
			if (Binding->Input.Expression) Enqueue(Binding->Input.Expression, Binding->Input.OutputIndex, Context.Parent);
			else if (Input->bUsePreviewValueAsDefault) Enqueue(Input->Preview.Expression, Input->Preview.OutputIndex, Item.Context);
			continue;
		}
		if (auto* Usage = Cast<UMaterialExpressionNamedRerouteUsage>(Item.Expression))
		{
			if (IsValid(Usage->Declaration) && Usage->Declaration->GetOuter() == Usage->GetOuter()) Enqueue(Usage->Declaration, Item.Output, Item.Context);
			continue;
		}
		for (auto* Input : Item.Expression->GetInputsView())
		{
			if (++Inspected > 50000) { bLimited = true; break; }
			if (Input && Input->Expression) Enqueue(Input->Expression, Input->OutputIndex, Item.Context);
		}
	}
	if (bLimited) return Unverified(TEXT("Host reachability is unverified: exceeded 50000 inspection steps."));
	return false;
}
}
