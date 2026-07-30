# World Partition validation recipe

## Applicability gate

Start with `scene.level.info` and `scene.world_partition.get`. When the world
is not partitioned, return `notApplicable` and stop. Empty cell or source lists
from a non-partitioned map are not proof of healthy streaming.

## Read-only audit

1. List cells with an explicit limit and retain `total` and `truncated`.
2. List streaming sources.
3. Run the streaming audit and keep its `ue.finding.v1` records.
4. List Data Layers; call `scene.data_layer.get` only for selected layers.
5. Run `scene.hlod.audit`.
6. Run `scene.pcg.inspect` only when live availability confirms the PCG module.

## Session fix

The guarded change API supports streaming enablement and runtime Data Layer
state. Call plan first, review the exact digest, then execute the same action
with explicit confirmation. Only runtime Data Layers are mutable, and the
change is session-only. Read the affected state again after execution.

Rollback requires the same Editor instance and original world. HLOD build and
PCG generate/cleanup are separate long or scheduled operations; do not call
them as validation or Workflow DSL steps. A scheduled PCG response is not
completion evidence—re-inspect when explicitly running such an operation.
