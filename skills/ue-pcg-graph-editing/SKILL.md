---
name: ue-pcg-graph-editing
description: Read, create and edit PCG Graph assets through native Editor actions and explicit save.
---

Use for PCG Graph structure, Settings and Blueprint Element nodes, or PCG pin connections. Use the installed CLI to discover exact schemas and availability.

1. Resolve an exact graph via scene.pcg.graph.list and graph.get. Create a new package with graph.create when requested.
2. Prepare the native graph editor. Page its factory catalog using the returned context ID. Select the returned factory ID, not a guessed class or title.
3. Plan a batch of addNode, setSettings, connect, disconnect and removeNode. Existing endpoints use nodeId and pin. New endpoints can refer to an earlier addNode ref.
4. Apply the exact approved digest with a stable request ID. Use exact reflected paths from graph.get settingsValues for Settings edits. Read back nodes, Settings values and edges. Disconnect affected edges before setting a field that changes pins or removing a node.
5. Save the intended package explicitly. Use the same-Editor receipt for rollback while the graph matches its recorded state.

See [PCG Graph Editing](../../docs/UE_PCG_GRAPH_EDITING.md) for the authored graph, display, and persistence contracts.
