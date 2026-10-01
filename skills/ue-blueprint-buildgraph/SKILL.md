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

## Named behavior templates

Use the `plan-named-template` recipe to discover `health_system`, `timer_loop`,
and `interactable_actor` through `blueprint.template.list`. Read each returned
`parameterSchema` and `limitations` before selecting it. Version 2 templates
author the execution and value links for a minimal runtime behavior on an
existing Actor Blueprint event graph:

- `health_system`: no-argument `TakeDamage` and `Heal` read editable
  `DamageAmount` and `HealAmount` members. Each event treats negative amounts
  as zero and clamps `Health` into `[0, max(MaxHealth, 0)]`. Parameters are
  finite nonnegative `maxHealth`, `damageAmount`, and `healAmount`.
- `timer_loop`: `StartTimer` sets one looping timer for the named callback;
  repeated starts reset its interval. The guarded callback increments
  cumulative `LoopCount`, at most once per frame. `StopTimer` and actual
  `EndPlay` clear the timer and `bTimerRunning`; `bTimerEnded` prevents later
  Start calls from reactivating that ended instance. It starts explicitly, and
  `delay` must be at least 0.001 seconds. The callback name cannot conflict
  with generated lifecycle events or inherited functions.
- `interactable_actor`: no-argument `Interact` increments `InteractionCount`
  only while `bIsInteractable` is true. `initiallyInteractable` is a boolean.
  `InteractionSphere` retains engine defaults; `radius`, overlaps, input
  bindings, replication and project gameplay integration are outside the
  template contract.

`blueprint.template.apply` is a read-only planning query. Pass an explicit
existing Blueprint asset path, the discovered `templateName`, and the exact
typed `parameters` object. The handler shares `blueprint.build.from_spec`'s
normalizer and managed BuildGraph planner. It returns `applied=false`, the
normalized definition, a Workflow, and an Editor-bound `planDigest`; it does
not write or approve the asset edit. Variable/component/event name conflicts
and existing authored state still need normal planning review. Do not assume
that a repeated request can overwrite existing declarations.

Review and approve the returned Workflow, then execute that unmodified
Workflow through `ue-workflow-cli execute --file <workflow.json>
--approve-plan <planDigest> --confirm-write --receipt <receipt.json>`.
Add `--save-on-success` only when persistence is requested. Retain the receipt
and `runId` for read-back, recovery, and rollback. Changing the target, template
parameters, Workflow, or asset baseline requires a new plan and approval.

After execution, read the managed definition and graph, compile status, and
save/read-back evidence. Then exercise the authored events on a current
PIE/Game instance. Check health subtraction, healing and both clamp limits;
timer repeated ticks, repeated Start without duplicate callbacks, Stop and
EndPlay cleanup; interaction with both guard values. Use
`blueprint.asset.runtime.verify` for the instance identity boundary and retain
the separate event/state evidence. Discovery, planning, compilation and a CDO
do not establish this runtime behavior.

## Evidence

For native persistence acceptance after changing reference or lifecycle code,
run `tests/hostproject/run-blueprint-persistence.ps1` against an already built,
source-bound isolated HostProject package. It launches three sequential Editor
processes to save a parent/child Blueprint pair, rename the referenced parent,
and verify the saved reference and old-path redirector in a fresh process.
The final phase spawns a Game actor and executes an inherited authored event;
it checks the actor property change and `blueprint.asset.runtime.verify`
instance identity. The receipt requires three distinct PIDs and each phase's
exact loaded module proof. This is isolated native fixture acceptance, not
acceptance of a user's project asset or of template gameplay. The template
native contract independently authors each template through its approved
Workflow, saves and reloads it, exercises its generated Game actor and verifies
rollback. Ordinary suite
runs without the phase environment produce no cross-process evidence.

Report the build ID, mode, before/after graph hashes, Workflow run ID and
digest, ref-to-GUID mapping, created/updated/removed managed refs, compile and
layout diagnostics, structural diff, exact entry-subflow evidence when
requested, and rollback/recovery state.
