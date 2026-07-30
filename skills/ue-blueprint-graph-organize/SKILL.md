---
name: ue-blueprint-graph-organize
description: Organize an Unreal Blueprint Graph with an exact geometry baseline, non-mutating layout preview, approved atomic Workflow edit, structural diff, layout validation, and before/after Graph screenshots. Use for Blueprint node alignment, distribution, connection straightening, Comment grouping, overlap cleanup, or visual layout acceptance.
---

# UE Blueprint Graph Organize

Treat layout as an asset edit with structural and visual evidence. Never infer
success from node coordinates alone.

## Load the recipe

1. Call `ue_skills` with `action=get`,
   `skill=ue-blueprint-graph-organize`, and
   `recipe=organize-and-verify`.
2. Load `references/organize-recipe.md` for geometry, approval, and capture
   boundaries.
3. Discover every operation through `ue_context`; do not guess enum values or
   parameter names.

## Organize

1. Read `blueprint.graph.get` with `geometryMode=editor`. If exact geometry is
   unavailable, stop visual layout work instead of treating stored dimensions
   as exact.
2. Call `blueprint.graph.snapshot`, retain the snapshot/hash, and capture the
   unchanged Graph Editor.
3. Build disjoint groups. A node may belong to only one group.
4. Call `blueprint.layout.organize` with `dryRun=true`. Review predicted
   coordinates, Comment bounds, all-graph obstacle diagnostics, graph hash,
   exact geometry fingerprint, predicted layout hash, and plan digest.
5. Obtain approval for that exact digest. Apply the same groups through
   `ue_workflow`; pass the expected graph hash and approved digest to the
   organizer operation. Apply rebuilds the same transient prediction and
   writes its approved coordinates/Comment bounds in one transaction. Do not
   generate a second layout rollback token.
6. Read the graph again, compare snapshots, validate the layout, compile
   validate the Blueprint, and inspect dirty state.
7. Capture the same Graph Editor after the edit and compare the two capture
   IDs. Treat an incompatible render fingerprint as `inconclusive`.

## Evidence

Report:

- Blueprint path, graph name, before/after graph hashes, and Workflow run ID;
- approved layout plan digest, predicted layout hash, geometry fingerprint,
  and position changes;
- created native Comment GUIDs and layout diagnostics;
- structural Graph diff, compile status, and dirty state;
- before, after, and diff capture IDs plus compatibility and changed region.

If structural diff shows an unexpected node, pin, or connection change, roll
back the Workflow even when the screenshot looks correct.
