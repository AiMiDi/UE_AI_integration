# PCG Graph Editing

The Scene domain serves graph structure from UPCGGraph and performs writes through the native PCG Editor schema on the Game Thread. Prepare opens or reuses the target editor and returns a paged catalog of engine-created Settings and Blueprint Element actions. The plugin reads the editor node's reflected PCGNode reference to map editor pins to authored nodes.

PCG Graph editing requires UE 5.4.0 or later within the plugin's supported range, with the PCG plugin and the PCG and PCGEditor modules available. The Graph commands remain discoverable below that minimum and report their engine requirement.

## CLI sequence

1. List by package directory or create an empty graph with a new package path. Use the exact object path for subsequent operations.
2. Read the graph. Nodes and edges have independent pagination. Use the returned graph hash to bind the edit to the inspected structure.
3. Prepare the editor. Use contextId with subsequent catalog pages and select factoryId from the returned catalog. The catalog belongs to that graph and editor session.
4. Plan with graphPath, contextId, expectedGraphHash and ordered operations. addNode takes ref, factoryId and optional posX/posY. connect and disconnect take from and to endpoints with pin and exactly one of nodeId or ref. See the Settings and removal rules below.
5. Apply the same inputs with approvePlanDigest, confirmWrite and a stable requestId. Read back nodes and edges. Save explicitly using the resulting graph hash.
6. Roll back using receiptId, editRequestId and confirmWrite while the recorded editor and graph state are current. Save again if the restored state should persist.

Use --params-file for nested input. Inspect schemas with ue-cli help and --live-schema.

## Authoring boundaries

The catalog exposes the native menu's Settings and Blueprint Element actions. It preserves native defaults and presets. Direct compatible links are supported. Conversion nodes and implicit replacement are rejected. Explicit disconnect followed by connect is supported in one batch. The graph is editable in interactive Editor sessions. Reads include normalized `settingsValues` for supported instance properties and include those values in `graphHash`.

A batch edits one project graph and leaves it dirty. Recovery snapshots cover graph-owned objects and the editor view. Failed recovery is reported explicitly and preserves the Editor transaction. Rollback receipts are session-bound. Save validates the project package, current structure and source control, and retains in-memory work on failure.

Graph notifications may refresh loaded bound PCG components. Component generation results are outside graph rollback.

## Settings and removal

`setSettings` requires an existing `nodeId` or an earlier `addNode` `ref`, the exact `expectedSettingsClass`, and up to 16 distinct `changes`. A change has a `path` and a typed JSON `value`. The node must own its Settings instance. Plan and Apply share path and value validation. Plan checks the live instance when earlier operations cannot affect that node. Apply checks the actual instance at each step, stages the whole change, sends native property chain notifications, and verifies the values and visible pins. Disconnect edges that the Settings change would remove before editing.

Paths use exact reflected names with up to eight segments and 128 characters. For example, `MinThreshold.AttributeTypes.DoubleValue` resolves through two nested structs. Each segment must be editable on an instance. The supported leaves are Boolean, int32, finite double, enum, FString, FName, bounded FString and supported struct arrays, and PCG input or output attribute selectors. Selector structs are edited as a value. Object references, fixed C++ arrays, and paths through container elements are outside this interface.

Apply uses the native Settings instance's `CanEditChange` with the full path. A connected PCG override pin can make the backing field read-only. Disconnect the override before writing that field. Dynamic editability affects writes. The backing value remains visible in `settingsValues` and contributes to `graphHash`. The complete read is limited to 512 fields and 64 Ki UTF-16 code units per node's Settings JSON and 2 Mi code units per graph. Reads that exceed the limits return an error without a partial hash.

For each PCG metadata constant struct, set `Type` and its matching value in the same operation. The supported pairs are Boolean / BoolValue, Integer32 / Int32Value, Double / DoubleValue, String / StringValue and Name / NameValue. A JSON number is written to the declared numeric type. Enum values use exact native member names. Switch `StringOptions` additionally requires distinct nonempty labels and reserves `Default` for the fallback output.

Pass the array property's path and an ordered JSON array to replace its entire value. An empty array clears it. A fixed size array requires the current length. A fixed size member inside a struct element retains its length at the same index. New elements use the native default length for those members.

A supported struct element has a complete representation in the supported value types, including nested structs and supported arrays. Supply each persistent reflected member with its exact name. An unknown or omitted member rejects the whole change. Transient, deprecated, and skipped serialization members use native defaults. Generic structs with persistent read-only or unsupported members, or custom native serialization, are excluded from whole value edits. PCG selectors use their existing typed representation. Reads retain array order and include the entire supported array in `graphHash`.

Each array allows up to 32 items per write and 512 items per read. Array elements and struct members share the eight-level depth limit and a budget of 4096 values per property. Arrays are validated in scratch storage before the Editor receives a property chain notification for the whole replacement.

Write a named attribute selector with `{"selection":"attribute","name":"Interior_Generate"}`. Input selectors also accept `@Last` and `@LastCreated`, and source-aware output selectors accept `@Source`. Reads include the existing selector kind and any extra path segments.

`removeNode` requires the existing `nodeId` and exact `expectedSettingsClass`. It only accepts ordinary nodes whose incident edges are absent when the operation runs. Use earlier `disconnect` operations for connected nodes. Apply removes the Editor node and authored node, then checks that the graph and active view have no persistent references to the removed node or Settings. Undo and same-session recovery can still hold the detached objects. A batch supports up to 32 Settings operations and 32 removals. Successful Apply returns `updatedNodeIds` and `removedNodeIds`.
