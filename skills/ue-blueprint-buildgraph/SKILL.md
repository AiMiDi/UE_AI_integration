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
2. Validate the complete `ue.blueprint-buildgraph.v1` definition.
3. Plan it. Review managed-node conflicts, the normalized Workflow v2, graph
   hash, managed ref mapping, removals, and plan digest.
4. Obtain approval for the returned Workflow digest and execute that exact
   Workflow with `ue_workflow`.
5. Read the definition and graph again. Verify compile, structural diff,
   managed refs, layout diagnostics, and optional capture evidence.

Use `merge` unless explicit deletion of obsolete nodes owned by the same
`buildId` is intended. `replaceManaged` must never remove unowned nodes.

## Evidence

Report the build ID, mode, before/after graph hashes, Workflow run ID and
digest, ref-to-GUID mapping, created/updated/removed managed refs, compile and
layout diagnostics, structural diff, and rollback/recovery state.
