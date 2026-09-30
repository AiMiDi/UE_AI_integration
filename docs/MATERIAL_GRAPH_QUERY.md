# Material graph query design

Status: implemented; isolated Editor and CLI validation recorded in
[validation report](validation/material-function-graph-query-2026-09-08.md).
Named reroute expansion is verified separately in the
[2026-09-09 validation](validation/material-named-reroute-query-2026-09-09.md).

方案要点：GraphIR 作为查询数据模型，第一版采用“不可变快照＋邻接索引”。
一次捕获一张材质或函数图，随后按页找节点、按范围查上下游；查询期间不刷新、
编译或保存资产。资产编辑交给 Workflow，打开的编辑器工作副本使用显式预览批次。
快照明确标注历史状态，编辑后重新捕获，
不能把查询哈希当作写入许可。跨资产全文检索与依赖索引留给后续独立组件。

## Read model

Use an immutable, bounded snapshot of one material or material function, with
stable asset-local node IDs and incoming/outgoing adjacency indexes. GraphIR is
the representation at this boundary; it does not require a mutable editing
session or a database. Workflow remains the owner of writes, asset preconditions,
deferred compilation, saving and rollback.

```
Material / MaterialFunction expressions (Editor game thread)
    -> explicit capture -> immutable nodes + edges + adjacency indexes
        -> filtered node pages
        -> bounded upstream / downstream / both subgraphs
        -> release / TTL / capacity eviction
Query results -> inspect -> prepare current Workflow plan -> execute -> recapture
```

Capturing never opens an asset editor, constructs a UMaterialGraph, compiles,
saves, or expands referenced functions. By default it reads authored expressions.
With `targetContext:"editorPreview"`, it captures the already-open editor's
working expression collection, including unapplied Custom and parameter nodes.
Pass the persistent `assetPath`, with optional `expectedPreviewId` to detect a
reopened editor. Responses identify `source`, `targetContext` and `previewId`;
asset and preview snapshots never silently replace each other's source.
See [preview editing](MATERIAL_EDITOR_PREVIEW.md) for the matching write contract.

## Identity and consistency

- `nodeId = expr:<object name>` identifies an expression within one asset. It
  survives editor graph reconstruction and package reload. Rename, deletion and
  replacement require a new capture; the ID is not a cross-asset identity or a
  guarantee that a deleted/recreated object is the same node.
- `root` is the synthetic material output node. Material functions use their
  authored FunctionInput / FunctionOutput expressions.
- `snapshotId` identifies one immutable capture. Pages and traversals never
  silently switch to a newer capture. Cursors bind the snapshot and filters.
- `projectionHash` hashes the captured query representation, including exposed
  node details and edges. It is not a package hash or a Workflow write token.
- Every response states `liveStateChecked: false`. Recapture before preparing
  edits. Workflow verifies its own current asset preconditions at execution.
- Expired, evicted or released snapshots return an explicit error requiring a
  new capture; they do not automatically rebuild and reuse an old cursor.

## Bounded queries

| Capability | Input | Result |
|---|---|---|
| `content.material.graph.index` | `assetPath`; optional targetContext and expectedPreviewId | snapshot ID, source, projection hash, counts, capture time |
| `content.material.graph.nodes.list` | snapshot ID; optional class, search, cursor, limit | node page, scanned count, continuation |
| `content.material.graph.subgraph.get` | snapshot ID, seed IDs, direction, depth, budgets | nodes, internal and boundary edges, completeness |
| `content.material.graph.snapshot.release` | snapshot ID | idempotent release result |

Current limits: 20,000 captured nodes, 100,000 captured edges, 256 inputs/outputs
per expression, estimated 32 MiB per snapshot, estimated 64 MiB cache and eight
entries; TTL is 300 seconds from capture. Memory limits can reject a graph before
it reaches the node limit. A page returns up to 200 nodes and scans up to 4,096
candidates. Subgraphs accept up to 32 seeds, depth 32, 200 returned nodes and
1,000 internal plus boundary edges. The adjacency scan budget is
`min(10000, max(256, maxEdges * 4))`. Payloads are bounded to 256 KiB.

By default the topology covers direct `FExpressionInput` connections and material
root inputs. Pass `includeNamedReroutes:true` to `graph.index` to include local
named reroute declaration-to-usage dependencies. The choice is fixed for that
snapshot and participates in its projection hash. Function calls and composite
interfaces remain unexpanded; `referencesExpanded:false` continues to identify
that wider boundary. A complete traversal is not a cross-function dependency proof.

Named reroute edges use `kind:"namedRerouteReference"`, `inputIndex:-1` and
`editableConnection:false`: they represent semantic references, not input pins
that can be passed to a wire-edit command. Direct edges use `kind:"expressionInput"`.
Both kinds share the adjacency, depth, node, edge, scan and response-byte budgets;
boundary edges preserve their kind. Upstream walks can follow consumer → usage
→ declaration → producer, and downstream walks use the reverse dependency direction.

The compiler uses the actual declaration object, so capture does not infer a
connection from a matching name or GUID. Missing/deleted declarations or declarations
outside the working collection emit no synthetic edge. Their usage nodes expose
`referenceStatus` (`missingDeclaration`, `invalidDeclaration`, `outsideSnapshot`),
and metadata reports `unresolvedNamedReroutes` and `namedRerouteCoverage:"incomplete"`.
Resolved usages expose `declarationNodeId`; declarations and usages are searchable
by `rerouteName`. `namedRerouteCoverage:"completeWithinSnapshot"` only covers this
reference kind. Traversal `truncated:false` still requires checking the snapshot's
coverage metadata before claiming the input source has been fully resolved.

This optional projection works for authored materials/functions and the explicitly
selected editor preview. It neither repairs broken references nor touches their
assets. Re-capture after editing; existing snapshots keep the original reference.

Example `content.material.graph.index` parameters:

```json
{
  "assetPath": "/Game/Materials/M_Example.M_Example",
  "includeNamedReroutes": true
}
```

Use the returned `snapshotId` with `graph.subgraph.get`, seed it with the relevant
Custom node ID and select `direction:"upstream"` to inspect its input sources.
`direction:"downstream"` inspects consumers affected by a source edit. This is
structural dependency inspection, not selected-output shader liveness analysis.

The capture response contains metadata and counts, not every node. Subsequent
queries reuse the snapshot without accessing UObjects. Nodes are sorted by ID;
exact class filters use an index. Text search has a scan budget and may return
an empty page with a continuation cursor. Consumers must follow `hasMore`, not
infer completion from an empty result.

Capture costs O(V log V + E log E) including deterministic sorting, plus the exposed
property/text hashing cost. An unfiltered page costs O(returned payload); search
costs O(scanned candidates + returned payload). Traversal costs are limited by
the inspected adjacency budget, not the total size of the asset.

Subgraph traversal is iterative and cycle-safe, uses adjacency lists, and has
independent limits on depth, nodes, inspected edges and returned edges. It
reports incomplete coverage and boundary edges explicitly. This covers shared
upstream expressions and cycles without exponential path enumeration. A
high-degree hub cannot force unbounded work simply because the node limit is
small. Large text, such as Custom HLSL, is represented by size/hash metadata.

Caches have TTL, entry-count and memory budgets and explicit release. Snapshots
contain value data, never strong UObject references. UE access stays on the game
thread; future worker queries can operate only on immutable value data.

## Extension order

1. Single-asset capture, node search/pages and bounded subgraphs; measure capture
   separately from repeated queries and response serialization.
2. Node detail projections, selected-output dependency
   slices, unreachable-node diagnostics and explicit nested-function expansion
   with per-asset visited sets and a global budget.
3. If capture latency itself is a measured problem, reuse captures behind an
   asset change generation and validate undo/redo, external writes, reload,
   deletion and Workflow rollback. Do not rely on PostEditChange alone while
   batch editing intentionally defers those notifications.
   First capture currently runs on the game thread as one coherent read. Do not
   move UObject traversal to a worker thread. A future capture spread across
   frames needs a change generation/retry boundary to prevent mixing revisions;
   workers can index or serialize detached value data after a coherent capture.
4. Add a persistent cross-asset index only for project-wide search/dependencies.
   Store asset identity, source revision/hash and completeness with each record.
   SQLite is a reasonable initial choice for metadata, text indexes and bounded
   recursive dependency queries. A graph database is justified only by measured
   cross-asset multi-hop workloads and operational needs.

SQLite's [recursive CTE documentation](https://sqlite.org/lang_with.html) provides
the relevant dependency-walk primitive. Database selection above is an
architecture recommendation, not a benchmark comparison with this implementation.

Do not start with automatic transitive function expansion, a universal graph
database, or incremental mutable GraphIR synchronization. They add invalidation,
ownership and recovery work before solving the current repeated full-graph read.

## Comparing revisions without rereading full graphs

Use `content.material.graph.snapshots.diff` with `beforeSnapshotId`,
`afterSnapshotId`, optional `limit` (1..200, default 50), and its continuation
`cursor`. Both captures must identify the same asset, asset/preview context,
preview session and includeNamedReroutes mode. It does not load or re-capture
expired snapshots. Keep both alive until comparison completes, then release them.

Changes have entity node/edge and change added/removed/modified. Node records
identify nodeId and before/after hashes of the captured node properties;
modified records list changedFields. Detailed code/values are not repeated in
the diff: query the indicated node in the intended snapshot/context. Adjacency
counts are not node-property changes; edge records describe connection changes.
Rewiring or changing component masks produces a removed edge and an added edge.
Edges preserve their explicit/named-reference kind, pin indexes and mask fields.

Nodes and edges use deterministic sorted indexes built at capture. Each page
merges from the cursor positions, scanning at most 4096 merge steps and returning
at most 256 KiB. An unchanged 4096-node prefix can yield zero changes with
hasMore:true; continue with nextCursor. Limits can change between pages, but
the cursor cannot switch or reverse the snapshot pair. No complete total-change
count is claimed before all pages have been read. Equal projection hashes allow
an immediate empty result without scanning.

This compares the captured query projection, not full UObject properties, shader
semantics, pixel output or current live state. Re-capture after editing. Renaming
or replacing objects may change their asset-local IDs; no semantic rename matching
is inferred. Capture adds O(E log E) edge sorting and retained comparison keys;
it does not make the initial capture incremental or move UObject reads off-thread.

This API is separate from the older `graph.diff` asset snapshot mechanism; IDs
from `graph.index` are not interchangeable with graph.snapshot/restore IDs.

## Editable settings in the compact projection

Node hashes include scalar values/ranges, vector defaults, static Boolean/switch
defaults, texture parameter asset paths/sampler types, parameter group/order/GUID,
constant values and function input preview values/interface GUIDs. A texture path
does not fingerprint the texture's pixel content. Preview positions use the live
graph node coordinates; authored asset positions use expression coordinates.

Custom nodes include codeHash/codeCharacters, customOutputType,
customDescriptionHash and customConfigurationHash. The configuration hash covers
ordered input names, additional output names/types, define names/values and
include paths. Therefore an unchanged HLSL body does not hide a configuration
edit. Node descriptions include a full descriptionHash even when display text
is clipped. Read custom.get for detailed editable content; graph pages do not
repeat raw HLSL, macro values or include lists.

Capture rejects non-finite projected numbers and oversized text/configuration
with graph_node_projection_unavailable, publishing no partial snapshot. Limits
are 1 Mi characters per hashed text field, 8 Mi total projected text characters,
2 Mi serialized Custom configuration characters and 256 entries per Custom
configuration array. These are query bounds, not changes to editor write limits.
Other UObject properties, include file contents and referenced function bodies
are still outside this local query projection.

## Acceptance cases

Verify deterministic pages and filtering; cursor mismatch; cycles and fan-out;
limits and truncation; expired/released snapshots; old snapshots after editing;
stable IDs after editor-graph rebuild; query-only package dirty state; and
MaterialFunction captures without an open function editor. Report synthetic
timings separately from TA material measurements and visual/shader validation.
