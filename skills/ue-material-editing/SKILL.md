---
name: ue-material-editing
description: Edit Unreal materials, material functions, Custom HLSL and material instance overrides; query large graphs and diagnose shader errors. Use for material nodes, wiring, parameters, GraphIR, 材质编辑, 材质函数, 材质实例 or HLSL 修错.
---

# UE Material Editing

Choose the execution context before editing. The Material Editor owns an
unapplied working copy; editing the asset does not synchronize that copy.
Prefer one batch for continuous edits and one final update. Keep persistence
`dirtyOnly` or `save:false` unless the requested outcome requires saving.

## Discover

Resolve `ue-cli` and `ue-workflow-cli` from the plugin's `CLI/bin` or the UE
entry Skill. Read the chosen recipe with
`ue-cli skills --name ue-material-editing --recipe <id> --detail full --json`.
This loads guidance, not an executable Recipe Runner job.
Select that recipe ID from `data.skills[].recipes`; the CLI's recipe option
filters matching Skill packages and can still return their other recipes.
Before every capability call, use `ue-cli help <id> --live-schema --json` against
the intended Editor endpoint. For Workflow steps, inspect their live admission
and the Workflow CLI's current plan/execute help. Existing task authorization
applies; recipe risk metadata does not require asking again for authorized work.

| Task | Recipe | Execution |
| --- | --- | --- |
| Find nodes, input sources or affected consumers | `inspect-graph` | Immutable GraphIR snapshot and bounded pages/subgraphs |
| Edit the already-open working copy | `edit-preview` | `content.material.editor.batch` |
| Edit authored material/function assets | `edit-assets` | UE Workflow material/materialFunction scopes |
| Change material instance overrides | `edit-instance` | `content.material.instance.parameters.batch` |

For explicit Material Editor lifecycle and node selection, use
`material-editor-lifecycle`: call `content.material.editor.open` or
`content.material.editor.state` for the requested asset, then optionally call
`content.material.editor.nodes.select` with stable node IDs. The lifecycle
commands only change editor session state; they do not edit, compile, Apply or
save the material graph. Close an editor explicitly with
`content.material.editor.close` and read the resulting state when the workflow
owns the editor lifecycle.

For an open editor, read `content.material.editor.context.get` before deciding
whether the requested target is its preview. `editorOpen:false` does not open
an editor. When editing the visible material, do not silently switch to asset
writes. A graph/instance snapshot is not an asset Workflow checkpoint.

For graph planning, use `content.material.graph.boundary.get` only with the
snapshot, asset/preview identity and projection hash that produced it. Use
`content.material.graph.definitions.list` to discover bounded property and pin
contracts. Only fields explicitly marked with class-specific writer support are
writable by the current APIs; the catalog is not a generic reflection writer.
Use `content.material.graph.node.source.resolve` with the same snapshot to map a
node to its current authored UE object identity. The response is diagnostic-only
and reports `sourceMappingState:"unavailable"` when the optional compiler
authored-line accessor is absent. `content.material.graph.execute_plan` consumes
that proof when supplied and rejects any referenced expression outside the
writable node set. On authored MaterialFunction assets, its
`set_function_metadata` operation updates and reads back description, library
exposure and library categories in the same transaction as topology edits. On
authored Material assets, `duplicate_node` duplicates one expression with an
optional explicit `position` or relative `offset`; the operation result returns
the new stable node identity and a failed postcondition removes the created
expression during rollback.
Unsupported operations are rejected before any mutation.

## Execute and verify

- For preview edits, read [preview batches](references/preview-batches.md).
  Put create/configure/connect/delete operations in one batch. Do not also call
  refresh after a batch that already refreshed. Report unapplied preview edits
  as unapplied; automatic Apply is not implemented.
- For authored assets and functions, read [asset Workflow](references/asset-workflow.md).
  Let Workflow own deferred compilation, finalization, readback and rollback;
  do not inject internal `__ueWorkflow` metadata into direct calls.
- For instances, read [instance batches](references/instance-batches.md).
  Keep full parameter identities and expected state hash; clear overrides to
  inherit. Instance batches are not supported as a Workflow scope yet.
- For queries or HLSL failures, read [queries and diagnostics](references/queries-and-diagnostics.md).
  Distinguish a successful transport response, a completed edit, current shader
  validity, applied asset content and disk persistence.

The legacy `content.material.graph.snapshot` / `graph.diff` /
`graph.restore` path is separate from immutable GraphIR query snapshots.
Restore requires a fresh `expectedCurrentDigest`, adds only missing connections,
and rejects any connection that would break or convert existing wiring. It is
not a node/property restore or a full graph restore. It is dirty-only and rejects
`save:true`; persist through an approved Workflow with a durable checkpoint.

Keep the asset/preview identity and relevant state hashes with the result.
On timeout or an unknown write outcome, read the current state and recover the
existing operation before submitting again. A hash conflict requires a fresh
read and re-plan, not removal of the precondition. Report verified rollback
separately from an attempted restore.

Keep these evidence layers separate: mutation completion, compile requested,
compile verified, structural readback, save requested, disk persistence, and
rollback verification. No single boolean implies the others.

Parent migration, function-instance management, Layer stack editing, automatic
Apply and a general HLSL-to-node transpiler are not covered by these recipes.
Layer parameter overrides and ordinary function editing do not imply those
features exist. Shader success is scoped to the reported host/resource and
configuration; TA performance and visual acceptance require their own evidence.

## Wave 5 capabilities

Wave 5 adds authored material-instance inventory and single-override clearing:

| Task | Recipe |
| --- | --- |
| Enumerate instances for a parent and clear one parameter override | `material-instance-inventory` |

For disk-backed texture authoring, use `content.material.texture.import` with
an explicit source file and destination asset path. The operation validates the
source and destination before invoking the Editor AssetTools importer, applies
the requested compression/sRGB/LOD/max-size settings, and reads those settings
back from the resulting `UTexture2D`. Existing assets are rejected unless
`replaceExisting:true`; package persistence is controlled by `save` and is
reported separately from import and readback success.

Covered capability IDs:

- `content.material.instance.list`
- `content.material.instance.parameter.clear`

`content.material.instance.list` is a read-only authored inventory over
immediate or recursive parent ancestry; it never compiles or saves.
`content.material.instance.parameter.clear` removes one named override so the
instance inherits the parent value. Read the inherited value back with
`content.material.instance.parameters.get` rather than assuming it.

## Material thumbnail query

Use `content.material.thumbnail.get` for one bounded PNG thumbnail of a
material, material instance, or texture. The default response is base64 PNG
data; set `saveToFile:true` to write below `Saved/UEAI/MaterialPreviews`.
Resolution is limited to 16–1024 pixels. The capability reports
`capability_unavailable` when the Editor cannot render (including NullRHI),
and never compiles, saves, or mutates the source asset.
