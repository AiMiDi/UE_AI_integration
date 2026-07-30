---
name: ue-landscape-authoring
description: Inspect, validate, snapshot, export, and safely change Unreal Landscape and Water content through deterministic raw imports, exact plan approval, recovery artifacts, and verified rollback. Use for heightmap or weightmap import, Landscape material or Layer Info assignment, managed Water Body creation/update/delete, and terrain change validation.
---

# UE Landscape Authoring

Treat terrain writes as recoverable production changes, not interactive brush
strokes. The first version accepts complete `r16le` heightmaps and `r8`
weightmaps, material and Layer Info assignments, and managed Water Body
operations. It does not expose arbitrary Sculpt or Paint input.

## Load the recipe

1. Call `ue_skills` with `action=get`, `skill=ue-landscape-authoring`.
2. Use `audit-and-export` for read-only baselines.
3. Use `plan-apply-verify` only after the exact target and source artifacts are
   known.
4. Read `references/landscape-recipe.md` before any write.

## Establish the baseline

Resolve the Landscape by GUID or full actor path when possible. Read its extent,
proxies, material, layers, and validation findings. Capture a semantic snapshot
before changing data. In a World Partition map, explicitly retain the impact
summary and inspect Data Layer, HLOD, and PCG evidence when available.

Do not import raw data when validation reports Landscape Edit Layers content or
unloaded components. Do not infer full-world coverage from a loaded subset.
Use a regular source file under the project directory; symbolic links and
directory junctions are deliberately rejected. Do not combine a weightmap
import and Layer Info replacement for the same Landscape layer in one request.

## Plan and execute

Build one request containing at most 32 deterministic operations. Call
`scene.landscape.change.plan`, show the normalized operations, source hashes,
recovery size, package digest, and impact summary, then obtain approval for the
exact `planDigest`.

Execute with that digest, `confirmWrite=true`, and a fresh `requestId`. Never
reuse a request ID for a different payload. The execute operation re-plans and
rechecks source files and package/semantic baselines before writing.

## Verify and roll back

Require `verified=true`, before/after snapshot digests, a bounded diff, and the
recovery artifact directory. Re-run Landscape and Water validation. If rollback
is requested, use the returned `runId`, the original caller-owned request ID,
and explicit confirmation while the same Editor and world are active.

Do not claim cross-restart rollback. Water deletion is admitted only when the
target is service-managed and planning returns a complete bounded actor export,
class/GUID/Level identity, and package/external-package evidence. After a
delete, require the actor export artifact and verify the complete property
digest after rollback; never treat class/transform-only recreation as success.
For Water creation, require external-actor package identity in a Partitioned
World and verify that rollback removes the actor, detaches the package, restores
the original package digest, and leaves no external package file behind.
