# Bounded graph queries and current shader evidence

Capture once with graph.index for the intended asset or editorPreview, then
reuse snapshotId for graph.nodes.list and graph.subgraph.get. Include
includeNamedReroutes:true to trace local declaration-to-usage dependencies.
Use upstream for an input's source and downstream for affected consumers.
Inspect returned boundary edges, truncation/scan limits and snapshot coverage.
An empty search page can have hasMore:true. Release finished snapshots.

For changes between captures, use graph.snapshots.diff with beforeSnapshotId
and afterSnapshotId from graph.index in the same asset/preview session and
projection mode. Follow nextCursor even when a page has zero changes; compare
node property hashes/changedFields and typed edge additions/removals. Rewires
and channel-mask changes produce removed/added edges. This is query-projection
comparison, not shader equivalence or full asset verification. Keep both
snapshots until all pages are consumed. The older graph.diff uses different
asset snapshots; do not mix the IDs.

The snapshot is immutable and liveStateChecked:false. It survives neither
expiry/eviction nor Editor restart, and it does not update after edits. Re-capture
before planning a change; never use an old graph snapshot as a live precondition.
Named references use kind:namedRerouteReference, inputIndex:-1 and
editableConnection:false. They are not wire-edit input pins. Missing/foreign
references report incomplete coverage; functions/composite graphs are not
expanded. A complete local walk is not a cross-function or shader-liveness proof.

Graph node hashes cover Custom code and ordered interface/define/include-path
configuration separately. A customConfigurationHash change can occur with an
unchanged codeHash; read custom.get to inspect the exact change. Parameter value
projections include scalar/vector/static-switch defaults and texture paths.
Those hashes do not fingerprint included file contents or texture pixels.
Oversized text or non-finite projected values fail capture explicitly; do not
treat a failed capture as an empty/equal graph.

For a bounded edit review, call graph.boundary.get against the same immutable
snapshot and selection. Keep boundaryId, sourceSnapshotId, asset/preview
identity, projectionHash, external dependencies and externally consumed nodes
together. Incomplete traversal cannot produce a writable boundary. The current
writers do not accept and revalidate this proof, so do not describe a boundary
read as an executed safe write plan.

Use graph.definitions.list to inspect bounded class-default property contracts
and input/output templates. Its catalog hash covers the emitted contracts and
binds cursors to their version. Follow pagination even when the requested limit
is larger than the response-budget page. Property, enum, default-text and pin
counts have explicit limits and truncation flags. `writerSupported:true` means
the named class-specific capability currently handles that field; other
reflected Editor properties remain read-only through UE AI.
Pin templates come from the class default object. They are never proof that a
configured instance has that pin; dynamic Custom, function-call and material-
attribute nodes require instance readback. `connectionWriterSupportedWhenPresent`
only describes the existing runtime-pin route. The catalog also has hard loaded-
class and contract-build budgets; budget exhaustion fails instead of returning
an incomplete catalog.

The older graph.snapshot, graph.diff and graph.restore operations use a separate
connection-topology projection and snapshot store. Restore requires the current
digest returned by a fresh graph.diff, verifies asset/projection identity,
preflights all connections, and only adds missing snapshot connections. It
preserves current extra connections, does not restore nodes or properties, and
never claims full graph restoration. Direct persistence is unsupported:
`save:true` is rejected before mutation and successful restores remain dirty
memory changes. Use Workflow when durable disk recovery is required.

For Custom edits, custom.get returns bounded code/interface configuration and a
node state hash. For parameters, parameter.list is a node query, whereas
instance.parameters.get describes overrides. Use function.call.get for actual
function interface identities. Keep these different query models separate.

## Diagnose, correct, verify

Read diagnostics.get for the same asset/preview used by the edit. It does not
compile, wait or refresh shader-source caches. Interpret compileState, valid,
diagnosticsMayBeStale, sourceCheckComplete and sourceCheckIssues together:

- pending: poll diagnostics.get; do not trigger another compilation.
- stale: source changed; update/validate that context once, then re-read.
- unverified/unavailable or valid:null: preserve the stated limitation and fix
  the missing evidence; an empty error array cannot prove success.
- failed: retain raw compiler diagnostics, find the affected Custom/parameter
  and apply the correction in one appropriate batch/Workflow, then validate.
- succeeded with current source: report success for the identified resource,
  host, feature level and configuration, not every disconnected node/variant.

Compiler-reported line numbers are not necessarily Custom editor line numbers.
Use customSourceLocation only when sourceLineMapped:true with current source
mapping evidence. sourceMapState:compiler_source_unavailable can occur when the
optional engine accessor is absent; retain original diagnostics without inventing
an authored line. Error expression candidates are not guaranteed one-to-one
matches with compiler errors. Include freshness checks have explicit coverage
and budgets; they do not track every generated or implicit engine shader input.

Unconnected Custom nodes may be pruned. Function preview valid:null is not a
failure to fix by forcing a verdict: validate the applied function with its
actual host or use a function+host Workflow when editing authored assets.

A restore/readback digest is structural evidence for its declared projection.
It is not shader validation or save evidence. Track compile requested versus
compile verified, saved, structural postconditions and rollback verification as
independent fields.
