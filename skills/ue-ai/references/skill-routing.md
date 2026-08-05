# UE_AI_integration routing

## Domain Skill selection

| User intent | Load this Skill | Primary evidence |
|---|---|---|
| Diagnose Blueprint structure, calls, references, compile state, or suspicious nodes | `ue-blueprint-diagnose` | scoped findings, graph/call evidence, compile validation |
| Reconcile a declarative managed Blueprint graph | `ue-blueprint-buildgraph` | definition, approved Workflow digest, structural diff, idempotent read-back |
| Align, distribute, straighten, or group Blueprint nodes | `ue-blueprint-graph-organize` | dry-run geometry, approved layout digest, structural and visual diff |
| Create or edit Widget Blueprints and bindings | `ue-umg-authoring` | hierarchy, binding signature, compile and rendered evidence |
| Move, rename, import, reimport, or safely delete assets | `ue-asset-migration` | dependencies, referencers, approved change receipt, rollback state |
| Audit World Partition, cells, streaming, Data Layers, HLOD, or PCG | `ue-world-partition-validate` | applicability, bounded cells/sources, subsystem findings |
| Inspect or change Landscape/Water deterministically | `ue-landscape-authoring` | snapshot/export hash, plan digest, semantic diff, rollback evidence |
| Capture Nanite, Lumen, ray tracing, or buffer visualization | `ue-render-debug-capture` | exact viewport identity, restored view state, compatible image evidence |
| Compare frame-time or memory performance | `ue-performance-regression` | environment fingerprint, percentiles, thresholds, optional Trace |
| Record, import, query, or export `.utrace` evidence | `ue-trace-insights` | Worker handshake, provider availability, bounded semantic query |
| Recover an interrupted job, dropped MCP connection, or source-control preflight | `ue-recovery-operator` | checkpoint, bounded attempts, approval state, terminal read-back |

Prefer the recovery Skill over replaying a write when work may already have
partially completed. Prefer the performance Skill for regression verdicts and
the Trace Skill for provider-level diagnosis.

## MCP tool routing

| Need | Tool |
|---|---|
| Connection, project, engine, Editor/PIE state | `ue_status` |
| Compact capability search and live availability | `ue_capabilities` |
| Exact schema, effects, lifecycle, risk, and backend | `ue_context` |
| Discover/load packaged domain Skills and references | `ue_skills` |
| Locate packaged native CLIs without contacting Editor | `ue_cli` |
| Blueprint operations | `ue_blueprint` |
| World, Actor, PIE, viewport, landscape, and rendering operations | `ue_scene` |
| Assets, materials, textures, Niagara, and UMG operations | `ue_content` |
| Animation Blueprint, state machine, and BlendSpace operations | `ue_animation` |
| Behavior Tree and Blackboard operations | `ue_ai` |
| Jobs, tests, performance, Trace, source control, project, and recovery operations | `ue_production` |
| Planned transactional asset edits | `ue_workflow` |

Every domain tool receives an exact dotted `operation`, a `params` object, and
an optional stable `requestId`. A domain tool must reject operations from a
different domain.

## CLI fallback

Use this sequence when MCP is unavailable or a terminal workflow is requested:

1. `ue-cli doctor --full --json`
2. `ue-cli skills --query <intent> --json`
3. `ue-cli help <capability-id> --json`
4. `ue-cli <capability-id> ... --json`, `ue-cli recipe ...`, or `ue-workflow-cli ...`
5. Run the same read-only verification required by the selected Skill.

Do not assume every local MCP backend is available through the short CLI;
respect the backend reported by the current descriptor and CLI surface status.
