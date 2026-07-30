# Blueprint Graph organize recipe

## Geometry truth

Use `geometryMode=editor` for layout acceptance. `stored` geometry can preserve
Comment bounds but ordinary node width and height may be absent or stale.
`auto` is suitable for discovery only when the returned `geometryStatus` is
checked. Never label stored-only geometry as exact.

Capture and organize require the target Blueprint Graph to be available in a
rendered `SGraphEditor`. They return an explicit unavailable error under
NullRHI, headless commandlets, or when Slate cannot resolve the requested
Graph.

## Preview and approval

Call `blueprint.graph.snapshot` before the preview and retain the exact
structural baseline used by the final `blueprint.graph.diff`.

Run `blueprint.layout.organize` with `dryRun=true` first. Retain:

- `graphHash`;
- `planDigest`;
- `predictedLayoutHash` and exact `geometryFingerprint`;
- predicted position changes and Comment bounds;
- layout diagnostics, including collisions with ungrouped Graph obstacles.

The preview uses a transient duplicate Graph and must report
`assetModified=false` and `undoStackModified=false`. Apply only the unchanged
groups with both `expectedGraphHash` and `approvePlanDigest`. The plan digest
binds normalized positions, Comment bounds, diagnostics, and geometry
fingerprint. Apply rebuilds the same deterministic UE 5.3-compatible
prediction before any source mutation, verifies the digest, and writes the
approved coordinates and native Comment bounds rather than recalculating
against a different live-widget layout.

Place the apply operation inside `ue_workflow` and include both values in the
operation params. The outer Workflow digest does not replace the organizer's
Graph hash or layout digest. The organizer relies on the Workflow transaction
only for an approved Workflow execution; a direct call rejects an unrelated
active Editor transaction and otherwise owns exactly one transaction. On
failure it restores the graph snapshot and verifies the original hash. Use
`ue_workflow rollback` for a successful edit that must be reverted.

## Verification

Compare before and after `blueprint.graph.get` snapshots. Layout edits may
change node positions, Comment bounds, and add intended Comment nodes; they
must not change existing node GUIDs, pins, defaults, or links.

Run `blueprint.layout.validate` with exact Editor geometry. Review overlap,
gap, partial Comment containment, Comment padding, and Comment overlap
diagnostics independently from compile validation. Organizer diagnostics also
cross-check affected nodes and new Comments against every ungrouped node and
Comment in the Graph.

Capture before and after with the same scope, dimensions, DPI, theme, engine,
and plugin version. `blueprint.graph.visual.compare` returns `inconclusive`
when render fingerprints differ. A pixel change is visual evidence, not proof
that graph structure remained correct.
