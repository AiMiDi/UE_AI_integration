---
name: ue-blueprint-buildgraph
description: Declaratively build or reconcile one Unreal Blueprint Graph through stable managed refs, an approved Workflow v2 plan, persistent recovery, graph diff, layout validation, and optional capture evidence. Use when a Blueprint graph should be generated or updated idempotently from a versioned definition.
---

# UE Blueprint BuildGraph

BuildGraph is a declarative Blueprint authoring contract. It is not Epic's
BuildGraph build system and it is not an alternate execution engine.

## Load the recipe

1. Call `ue_skills` with `action=get`, `skill=ue-blueprint-buildgraph`, and
   `recipe=build-and-verify`.
2. Load `references/buildgraph-recipe.md`.
3. Discover current schemas through `ue_context`; never guess supported node
   types or pin names.

## Build

1. Read `blueprint.graph.build.definition.get` and the current graph/hash.
   Preserve returned node GUIDs plus pin GUIDs and use those stable identities
   for read-back; display names are descriptive, not mutation identities.
2. Validate the complete `ue.blueprint-buildgraph.v1` definition.
3. Plan it. Review managed-node conflicts, the normalized Workflow v2, graph
   hash, managed ref mapping, removals, and plan digest.
4. Obtain approval for the returned Workflow digest and execute the definition
   with `blueprint.graph.build.execute`, passing `confirmWrite=true`, the
   requested `saveOnSuccess` policy, and a stable `requestId` when retrying.
   The handler re-plans against the current graph and rejects stale or missing
   Workflow runtime state before execution.
5. Read the definition and graph again. Verify compile, structural diff,
   managed refs, stable pin identities and links, layout diagnostics, and
   optional capture evidence. When the definition owns a named event or
   function entry, read that exact execution subflow with
   `blueprint.graph.describe`; use the managed entry node GUID when its name is
   ambiguous and page nodes and edges independently.

Use `merge` unless explicit deletion of obsolete nodes owned by the same
`buildId` is intended. `replaceManaged` must never remove unowned nodes.

BuildGraph owns graph topology only. Do not embed direct
`blueprint.component.property.set` calls or other component-template mutations
in a managed graph definition. Those are separate asset edits with their own
local-SCS, stale-state, transaction, compile, save, and read-back contract.

For a Monolith-compatible one-shot spec that owns variables, SCS components,
nodes, connections, and pin defaults, use `blueprint.build.from_spec`. The
handler normalizes the spec into the same managed BuildGraph Workflow, so all
owned phases share one approval, transaction, compile, read-back, and rollback
boundary. Variable entries use `variableName`/`variableType` (with `name`/`type`
accepted as input aliases); component entries use `name`/`componentClass` and
optional `parentComponent`; node and connection IDs are stable spec refs.

The Workflow owns deferred compile/save and final rollback. A direct graph or
component mutation response is not a substitute for the approved Workflow run
and finalizer evidence.

## Evidence

Report the build ID, mode, before/after graph hashes, Workflow run ID and
digest, ref-to-GUID mapping, created/updated/removed managed refs, compile and
layout diagnostics, structural diff, exact entry-subflow evidence when
requested, and rollback/recovery state.
