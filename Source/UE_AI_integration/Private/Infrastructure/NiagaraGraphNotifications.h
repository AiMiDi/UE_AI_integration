#pragma once

#include "NiagaraGraph.h"

namespace UEAIIntegration::NiagaraEditing
{
// UNiagaraGraph is MinimalAPI in UE 5.4: its convenience recompile and
// synchronization methods are not exported. Dispatch through Niagara's
// exported graph-change virtual so it invalidates its editor caches without
// linking the non-exported convenience methods.
inline void NotifyRestoredGraph(UNiagaraGraph* Graph)
{
	if (!Graph)
	{
		return;
	}
	// The parameterless notification invalidates editor caches but intentionally
	// does not mark a structural graph synchronization.  Snapshot restore has
	// changed graph membership, so emit a short lived Add/Remove pair to drive
	// Niagara's exported structural notification path while leaving the restored
	// graph contents untouched.
	UEdGraphNode* Marker = NewObject<UEdGraphNode>(Graph, NAME_None, RF_Transient);
	if (Marker)
	{
		Graph->AddNode(Marker, false, false);
		Graph->RemoveNode(Marker, false);
	}
	Graph->NotifyGraphChanged();
	// Snapshot restore rewires existing pins without recreating the graph.  The
	// compiler also consults Niagara's parameter-reference cache, so invalidate
	// that cache explicitly after the change notification.  This keeps restored
	// output identities visible to compilation as well as to graph queries.
	Graph->ConditionalRefreshParameterReferences();
}
}
