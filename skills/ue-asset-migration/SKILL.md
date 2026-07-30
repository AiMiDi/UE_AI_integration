---
name: ue-asset-migration
description: Audit, plan, execute, verify, and optionally roll back Unreal asset import, reimport, copy, move, rename, delete, or redirector repair with dependency evidence and exact plan approval. Use when reorganizing Content Browser assets, migrating assets between folders, repairing redirectors, or checking whether a destructive asset change is safe.
---

# UE Asset Migration

Treat asset migration as a guarded change batch, not as a sequence of ad hoc
rename and delete calls. Inspect dependencies first, approve the normalized
plan digest, and verify the resulting asset graph.

## Load the recipe

1. Call `ue_skills` with `action=get`, `skill=ue-asset-migration`, and
   `recipe=plan-execute-verify`.
2. Read `references/asset-change-recipe.md` for per-action constraints.
3. Confirm the exact sources, destinations, and persistence policy with the
   user before execution.

## Discover exact APIs

Call `ue_context` for every operation. Use the live availability check when
the action depends on an importer, source file, plugin, or current Editor
state. There is no `content.asset.migrate` capability; use the dedicated
change plan and execute contract.

## Change assets

1. Search and inspect every source asset.
2. Read dependencies and referencers before planning.
3. Submit the full requested batch to `content.asset.change.plan`.
4. Review the normalized request, preconditions, risk, rollback availability,
   and exact plan digest.
5. Execute the original request with that digest and explicit confirmation.
   Put a stable `requestId` in the `ue_content` envelope.
6. Keep assets dirty unless the user explicitly selects save-on-success.
7. Roll back only from the same Editor instance when the receipt says it is
   available. Rollback is another confirmed `ue_content` call with its own
   request ID, followed by the same asset/dependency/referencer readback.

Never overwrite a destination. Never claim that accepted execution means the
dependency graph is correct.

## See results

Report the receipt ID, normalized actions, structural diff, changed asset
paths, persistence state, rollback availability, and dependency/readback
results. Reimport and redirector repair may be non-rollbackable; say so before
execution.
