---
name: ue-blueprint-authoring
description: Inspect and edit authored Blueprint member variables, local SCS components, and event dispatchers with bounded structural readback. Use for Blueprint variable/component authoring; runtime debugging, declarative graph build, and node layout have separate skills.
---

# UE Blueprint Authoring

Use this Skill for authored Blueprint structure: member variables, local SCS
components, and event dispatchers. It does not inspect runtime/debugger state,
execute PIE, or describe the declarative build graph (see `ue-blueprint-buildgraph`,
`ue-blueprint-diagnose`, and `ue-blueprint-graph-organize` for those boundaries).

## Discover and choose a recipe

Resolve `ue-cli` through the UE entry Skill, load this Skill's machine recipe,
and read the exact local and live schemas before calling a capability. Re-read
the Blueprint, graph, component, and variable identity in the current Editor;
never reuse object paths or availability from an earlier Editor or PIE session.

Choose the narrowest recipe:

| Intent | Recipe |
| --- | --- |
| Read member variables, SCS components, graphs, and dispatchers | `inspect-blueprint` |
| Add, remove, rename, or retype a member variable | `edit-variables` |
| Add, remove, reparent, or set a property on a local SCS component | `edit-components` |
| Read and change one generated-class default on a Blueprint CDO | `set-cdo-property` |
| Create a native or plugin UObject/DataAsset under `/Game` | `create-data-asset` |
| Spawn multiple instances of a Blueprint actor in an editor level | `batch-spawn-blueprint-actors` |

## Variable and component authoring

- Start from `blueprint.asset.get` + `blueprint.property.list` to resolve the
  current member variables before adding, removing, renaming, or retyping.
  Rename updates every referencing graph node (get/set/delegate) and refreshes
  the CDO default; reject names that collide with an existing member variable.
- Component identity is the exact local SCS variable name or VariableGuid. Read
  the hierarchy with `blueprint.component.list` before reparenting; reparent
  rejects self-parenting and descendant cycles and preserves child components.
- Component writes are structural and may not preserve every native/inherited
  template detail; verify the local hierarchy after the change.

## Generated-class defaults

Use `set-cdo-property` for one reflected property on a Blueprint generated-class
CDO. Read `blueprint.cdo.properties.get` first so the property type, owner, and
editability are current. The write accepts scalar, object, and array JSON values,
rejects transient/deprecated/edit-const/disabled-on-instance properties, and
returns the exported old and new values. `saveOnSuccess` follows the workflow
save policy and does not turn an in-memory readback into disk persistence proof.
After a write, call `blueprint.cdo.properties.get` again and report the authored
CDO value, package dirty state, compile/save result, and disk reload evidence as
separate boundaries.

## Persistence and evidence

- Follow each capability's declared persistence schema rather than assuming a
  save. A member-variable rename refreshes the Blueprint structurally and marks
  it changed; component reparent marks the Blueprint dirty without saving.
- Report authored readback, structural change, reference update, dirty state,
  and disk persistence as separate evidence levels. Do not present authored
  structure as runtime or debugger state.

## Wave 5 capabilities

Wave 5 adds structural graph export, graph duplication, and node copying:

| Intent | Recipe |
| --- | --- |
| Duplicate a graph or copy nodes between graphs | `manage-blueprint-graph` |

Covered capability IDs:

- `blueprint.graph.export`
- `blueprint.graph.duplicate`
- `blueprint.graph.copy_nodes`

`blueprint.graph.export` is a read-only structural query with stable node and
pin GUIDs and never compiles or saves. `blueprint.graph.duplicate` and
`blueprint.graph.copy_nodes` write only non-transient `/Game/` Blueprints,
re-wire internal links, and compile plus read back node counts.

## Native UObject and DataAsset creation

Use `create-data-asset` for one loaded native or plugin UObject class. Supply a
new `/Game` package path, the class name or full `/Script` path, and the object
path that the create step returns for the CDO readback. The operation rejects
Blueprint, Actor, abstract, deprecated, and superseded classes before mutation.
It registers the object with the Asset Registry and returns explicit object
path, class path, registration, package-dirty, save-attempted, and saved
evidence. A successful command still needs the `blueprint.cdo.properties.get`
readback against the returned object path before claiming persistence; a
`skipSave` result is intentionally dirty in memory.

## Batch Blueprint actor spawning

Use `blueprint.actor.batch_spawn` for bounded editor-world placement of a
Blueprint actor class. The operation accepts a grid or linear layout, shared
transform and actor metadata, including the lower-camel-case `labelPrefix`, and an optional loaded streaming sublevel. It
creates one undo transaction and reports successful actor identities and
per-actor failures separately. A successful command is authored world state;
re-read the level actors before claiming persistence or runtime behavior.
