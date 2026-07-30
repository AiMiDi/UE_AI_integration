---
name: ue-world-partition-validate
description: Validate the current Unreal world across World Partition, runtime cells, streaming sources, Data Layers, HLOD, and optional PCG evidence, with a separate guarded path for session-scoped fixes. Use when auditing open-world streaming, checking whether a map is partitioned, investigating Data Layer state, reviewing HLOD readiness, or validating PCG availability.
---

# UE World Partition Validate

Default to a read-only audit. If the current map is not partitioned, report the
recipe as not applicable instead of calling deeper World Partition operations
and interpreting empty results as success.

## Load the recipe

1. Call `ue_skills` with `action=get`,
   `skill=ue-world-partition-validate`, and
   `recipe=audit-current-world`.
2. Read `references/world-partition-recipe.md` for result interpretation.
3. Use `plan-session-fix` only after the audit identifies one explicit,
   supported state change.

## Discover exact APIs

Call `ue_context` before each operation. Use a live capability lookup for PCG
and any module-dependent operation. There is no single
`scene.world_partition.validate` endpoint; the recipe combines bounded audits.

## Audit

1. Read level information and World Partition state.
2. Stop with `notApplicable` when `partitioned=false`.
3. Otherwise list bounded runtime cells and streaming sources.
4. Run the streaming audit.
5. Read Data Layers and inspect specific layers when needed.
6. Audit HLOD and inspect PCG if the module is available.

Do not start HLOD builds or PCG generation as part of validation. Those are
long or scheduled operations with their own completion evidence.

## Optional session fix

Plan one supported streaming or runtime Data Layer change, approve the exact
digest, execute with confirmation, and read the affected state again. Runtime
Data Layer changes are session-only. Rollback requires the same Editor and
world.

## See results

Report partition applicability, streaming state, bounded cell counts and
truncation, streaming-source evidence, findings, Data Layer effective states,
HLOD build requirement, and PCG availability. Distinguish unavailable,
not-applicable, incomplete, and healthy.
