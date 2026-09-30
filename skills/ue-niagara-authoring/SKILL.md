---
name: ue-niagara-authoring
description: Inspect and edit authored Niagara Systems, emitter graphs, stack modules, node state, pin defaults, renderer materials, and inline User parameter defaults with plan, readback, rollback, and receipt release. Use for Niagara System or NS authoring; component overrides, SimCache particles, collision diagnosis, and runtime trace behavior have separate boundaries.
---

# UE Niagara Authoring

Use this Skill for authored Niagara System and graph state. It does not inspect
or edit loaded component overrides, runtime execution state, recorded particles,
or Data Interface payloads.

## Discover and choose a recipe

Resolve `ue-cli` through the UE entry Skill, load this Skill's machine recipe,
and read the exact local and live schemas before calling a capability. Re-read
the System, emitter handle, graph, node or renderer identity, state digest, and
outstanding compilation state in the current Editor; never reuse object paths or
availability from an earlier Editor or PIE session.

Choose the narrowest recipe:

| Intent | Recipe |
| --- | --- |
| Read authored System, emitter, renderer, and typed User parameter state | `inspect-system` |
| Read a graph and its available native computation operations | `inspect-graph` |
| Add native computation nodes and additive connections | `edit-computation-graph` |
| Insert one Module script into an emitter stack | `add-stack-module` |
| Change one function-call node's enabled state | `set-node-enabled` |
| Change one unlinked input pin default | `set-pin-default` |
| Change one authored renderer material slot | `edit-renderer-material` |
| Change one authored inline User parameter default | `edit-user-parameter` |

## Graph authoring

- Start with `content.niagara.graph.inspect`. For computation-node additions,
  also page `content.niagara.graph.operations.list`; use the returned operation
  and pin definitions instead of inventing types or pin names.
- Prefer the specialized Module, enabled-state, or pin-default chain when it
  matches the intent. The generic `graph.edit` chain only adds supported native
  computation nodes and additive connections; it is not a generic delete,
  relink, stack-reordering, or arbitrary UObject-property writer.
- Preserve the exact plan digest and use a stable `requestId` for one logical
  apply. Re-plan after any graph ChangeID, target identity, plan, or contract
  digest changes. Do not bypass stale-state or graph-drift refusal.
- Verify the exact graph, node path/GUID, pin value, added module or connections,
  and compilation result after apply. A rollback is a same-Editor recovery
  boundary and may refuse when later authored graph changes are detected.

## Renderer materials and authored parameters

- Renderer identity is the exact System, emitter handle, and current renderer
  path. Read the material inventory again after a change and verify the intended
  slot, override state, and binding. A binding can still take precedence over an
  explicit material.
- Parameter writes cover existing `User.*` variables whose authored default mode
  is `Value`, and synchronize both the exposed parameter store and the
  `UNiagaraScriptVariable` default. Supported values are float, int, bool, vec2,
  vec3, position, vec4, quat, and color. String, object, enum, Data Interface,
  Binding/Custom defaults, and component overrides are outside this contract.
- Parameter state uses non-reversible raw-value digests as part of its identity.
  A JSON `null` projection for NaN, infinity, or an invalid Niagara bool is not
  treated as the value identity; check the representability fields before using
  the projected value.

## Persistence, rollback, and receipts

- Treat renderer-material and User-parameter persistence as `dirtyOnly`; those
  chains do not save packages. Follow each graph operation's declared persistence
  schema rather than assuming a save. `compileRequested:true` only proves an
  asynchronous request was submitted; it does not mean compilation completed or
  runtime behavior passed.
- Distinguish owned semantic restoration, restoration of the package dirty bit,
  preservation of possibly unrelated dirtiness, and full-package restoration.
  Without a package fingerprint, semantic readback plus a matching dirty bit does
  not prove full-package restoration. A manual rollback may therefore report
  `rolledBack:true`, `dirtyPreserved:true`, `dirtyRestored:false`, and
  `fullPackageStateRestored:false` together.
- Release a rolled-back or no-op renderer/parameter receipt when finished.
  Releasing a changed, still-applied receipt requires explicit confirmation that
  its rollback boundary may be discarded. Released request identities are kept
  only in bounded recent same-Editor history; after eviction an old `requestId`
  is no longer guaranteed idempotent.

## Collision and Async GPU Trace boundary

`content.niagara.graph.collision.audit` is a diagnostic inventory, not proof of
runtime provider selection or collision correctness. The
`collision.policy.*` and `async_trace.configure.*` chains are specialized
authored remediation paths: use them only when the request explicitly concerns
collision policy or Async GPU Trace settings, begin from the matching audit, and
verify their own readback and runtime evidence. Do not substitute generic graph
editing for those policies, and do not present an authored configuration change
as GSDF/HWRT scheduling, shader, or live particle acceptance.

Report authored readback, semantic rollback, dirty-bit outcome, compile request,
compile completion, disk persistence, and runtime acceptance as separate evidence
levels.

## Wave 5 capabilities

Wave 5 adds module-input authoring, System duplication, and emitter module
clearing:

| Intent | Recipe |
| --- | --- |
| List one module's inputs and set an inline value or a parameter/attribute binding | `manage-module-input` |
| Mount one Dynamic Input script on an unconfigured module input | `add-dynamic-input` |
| Duplicate an authored Niagara System to a new package | `duplicate-system` |
| Remove every module from an emitter's stacks (destructive, no rollback) | `clear-emitter-modules` |

Covered capability IDs:

- `content.niagara.graph.module.inputs.list`
- `content.niagara.graph.module.dynamic_inputs.list`
- `content.niagara.graph.module.dynamic_inputs.tree`
- `content.niagara.graph.dynamic_input.inputs.get`
- `content.niagara.graph.module.dynamic_input.value.get`
- `content.niagara.graph.module.input.value.plan` / `.apply` / `.rollback`
- `content.niagara.graph.module.input.binding.set`
- `content.niagara.graph.module.dynamic_input.add.plan` / `.apply` / `.rollback`
- `content.niagara.system.duplicate`
- `content.niagara.emitter.modules.clear`

`input.value.*` follows the plan/apply/rollback receipt chain, while
`input.binding.set` is a single atomic command with no rollback. System
duplication is shallow (it shares emitter asset references) and saves to disk.
`emitter.modules.clear` removes every function-call node from all of one
emitter's stacks and is destructive with no rollback; page the ordered stack
first. `module.dynamic_inputs.list` follows authored override-pin links and
returns only attached Dynamic Input scripts; it does not compile, save, or
verify runtime/PIE behavior.

`module.dynamic_inputs.tree` extends that read boundary to nested Dynamic Input
function-call nodes. It accepts bounded `maxDepth` and `maxNodes` values and
reports `treeTruncated` or `cycleDetected` when the authored graph cannot be
represented within those limits; it remains read-only and does not compile,
save, or claim runtime behavior.

`dynamic_input.inputs.get` inspects an unattached Dynamic Input script. It
returns deterministic parameter input names, Niagara types, data-interface
flags, and the first non-parameter-map output type. The script must have
DynamicInput usage, and the query is bounded to 256 inputs; it does not
compile, save, or mutate the asset.

`module.dynamic_input.value.get` reads a mounted Dynamic Input node by the
GUID returned by `module.dynamic_inputs.list` or `module.dynamic_inputs.tree`.
It resolves a child input by short or fully-qualified name and reports whether
the authored source is a literal, parameter binding, nested Dynamic Input, or
another linked node. It remains read-only and does not compile, save, or claim
runtime behavior.

`module.dynamic_input.add.plan/apply/rollback` mounts one Dynamic Input script
on a module input that has no authored override. Existing literals, bindings,
or Dynamic Inputs are rejected rather than overwritten. Apply reads back the
mounted node GUID and script identity, requests compilation, and returns a
same-Editor receipt. Rollback removes only that receipt-owned node and an empty
override node, and refuses after graph drift. The chain changes dirty authored
state only; it does not save the package or prove runtime resolution.

`content.niagara.system.inspect` also reports each emitter's authored Event
Handler and Simulation Stage inventory. Event Handler rows include the event
source, execution mode, spawn and per-frame limits, and the linked script
identity. Simulation Stage rows preserve stack order and include the stage
name, enabled state, class, and linked script identity. Both inventories are
bounded to 128 rows per emitter and expose `eventHandlersTruncated` or
`simulationStagesTruncated` when the authored asset exceeds that bound. These
fields describe authored configuration only; they do not prove event delivery,
stage execution, compilation success, or runtime particle behavior.

## Event Handler authoring

The `content.niagara.event_handler.add.*` chain adds one authored
`ParticleEventScript` to an owned emitter. Start from `system.inspect` so the
current emitter and graph identities are fresh, then supply a caller-owned
stable `usageId` and run `add.plan` followed by one approved `add.apply`.
`sourceEventName`, `sourceEmitterId`, execution mode, spawn limits, and the
initial-value option are bounded and read back from the resulting
`FNiagaraEventScriptProperties`. The apply initializes the matching graph
output through Niagara's editor stack utility, requests compilation, and
returns a same-Editor receipt. `add.rollback` removes both the receipt-owned
handler and all nodes reachable from its `ParticleEventScript` output; it
refuses to run after the emitter change ID drifts. This proves authored
configuration and compile/readback state only; event delivery, runtime
execution, and package persistence remain separate evidence levels.

## Simulation Stage authoring

The `content.niagara.simulation_stage.add.*` chain adds one authored
`ParticleSimulationStageScript` to an owned emitter. Begin with
`content.niagara.system.inspect` so the current emitter and graph identities
are fresh, then provide a caller-owned stable `usageId`. The plan accepts a
non-abstract Niagara simulation-stage class (defaulting to
`NiagaraSimulationStageGeneric`), a bounded display name, enabled state, and
optional stack index. Apply creates the stage and its script, initializes a
minimal parameter-map graph output using public graph APIs, requests
compilation, and returns a same-Editor receipt. Rollback removes the
receipt-owned stage and graph output only while the emitter ChangeID remains
unchanged. This proves authored configuration and compile/readback state; GPU
scheduling and runtime particle behavior require independent acceptance.
