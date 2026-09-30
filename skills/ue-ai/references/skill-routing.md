# UE_AI_integration routing

## Domain Skill selection

| User intent | Load this Skill | Primary evidence |
|---|---|---|
| Diagnose Blueprint structure, calls, references, compile state, or suspicious nodes | `ue-blueprint-diagnose` | scoped findings, graph/call evidence, compile validation |
| Reconcile a declarative managed Blueprint graph | `ue-blueprint-buildgraph` | definition, approved Workflow digest, structural diff, idempotent read-back |
| Align, distribute, straighten, or group Blueprint nodes | `ue-blueprint-graph-organize` | dry-run geometry, approved layout digest, structural and visual diff |
| Add, rename, remove, or retype Blueprint member variables, local SCS components, or event dispatchers | `ue-blueprint-authoring` | variable/component identity, reference update, hierarchy readback, cycle/collision rejection |
| Create or edit Widget Blueprints and bindings | `ue-umg-authoring` | hierarchy, binding signature, compile and rendered evidence |
| Edit material/function nodes, Custom HLSL, instance overrides, or query large material graphs | `ue-material-editing` | explicit asset/preview identity, one batch/final update, current shader and readback evidence |
| Inspect authored Niagara Systems or edit renderer materials and typed inline User parameter defaults | `ue-niagara-authoring` | exact System/emitter/renderer identity, typed readback, dirty-only compile and same-Editor rollback evidence |
| Move, rename, import, reimport, or safely delete assets | `ue-asset-migration` | dependencies, referencers, approved change receipt, rollback state |
| Audit World Partition, cells, streaming, Data Layers, HLOD, or PCG | `ue-world-partition-validate` | applicability, bounded cells/sources, subsystem findings |
| Inspect or change Landscape/Water deterministically | `ue-landscape-authoring` | snapshot/export hash, plan digest, semantic diff, rollback evidence |
| Capture Nanite, Lumen, ray tracing, or buffer visualization | `ue-render-debug-capture` | exact viewport identity, restored view state, compatible image evidence |
| Observe Niagara SimCache, inspect loaded objects, or analyze DRED/render failures offline | `ue-render-debug-capture` | public SimCache capture/read/export/release, particle attribute pages, world/component/DI inventory, retained log ranges and hashes |
| Compare frame-time or memory performance | `ue-performance-regression` | environment fingerprint, percentiles, thresholds, optional Trace |
| Record, import, query, or export `.utrace` evidence | `ue-trace-insights` | Worker handshake, provider availability, bounded semantic query |
| Recover an interrupted job, dropped MCP connection, or source-control preflight | `ue-recovery-operator` | checkpoint, bounded attempts, approval state, terminal read-back |

Prefer the recovery Skill over replaying a write when work may already have
partially completed. Prefer the performance Skill for regression verdicts and
the Trace Skill for provider-level diagnosis.

For a LevelScriptBlueprint, use Workflow v2 with a `levelBlueprint` scope.
The logical asset is the LevelScriptBlueprint while persistence and rollback
use its owning `.umap`; do not route a map through an ordinary `blueprint`
scope. Direct graph writes are atomic only for one request.

For bounded PIE subsystem validation, plan and start the packaged
`ue-pie-subsystem-validation` Recipe. Session Recipes may call only
`sessionSafe` capabilities and are bound to one Editor instance and one
Runner-owned PIE generation.

## CLI routing (preferred)

Resolve the executable paths first; see the entry Skill's availability steps.

| Need | CLI entry point |
|---|---|
| Connection, project, engine, Editor/PIE state | `ue-cli status --json` |
| Installation and contract diagnostics | `ue-cli doctor --json`; full checks on installation or version drift |
| Compact local capability search | `ue-cli capabilities --query <intent> --json` |
| Exact schema, effects, risk, and backend | `ue-cli help <capability-id> --json`; add `--live-schema` for Editor availability |
| Discover packaged domain Skills | `ue-cli skills --query <intent> --json` |
| Load a machine recipe | `ue-cli skills --name <skill-id> --recipe <recipe-id> --detail full --json` |
| Read Skill instructions and references | Read the entrypoint and declared resources under returned `data.skillRoot/<skill-id>` |
| A bounded operation in any domain | `ue-cli <capability-id> ... --json` |
| Planned transactional asset edits | `ue-workflow-cli` plan/execute/read-back flow from its current help |
| Recipe, job, or scenario orchestration | The corresponding `ue-cli` surface discovered through help |

Use the same CLI route for the selected recipe's verification phase. Domain
Skills may show MCP examples: retain their capability IDs and execution
contracts, and discover the corresponding CLI parameters before execution.

## MCP fallback mapping

Use this mapping when CLI is absent, incompatible, or lacks the needed feature,
or when the user explicitly requests MCP. An operation failure or unavailable
Editor is not by itself a reason to retry through another transport.

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

## Availability and recovery

For installation acceptance or version drift, run
`ue-cli doctor --full --no-clean-stale-instances --json`. Resolve relevant
revision, module, catalog, and Workflow digest mismatches before writing.
Report local CLI/catalog, Editor, and Trace Worker readiness separately;
missing Editor or Worker does not invalidate unrelated local discovery.

Do not assume every MCP backend is exposed by the short CLI. Respect the
current descriptor and CLI surface status. If a write result is unknown,
use the recovery Skill and inspect the existing request/run before retrying.
