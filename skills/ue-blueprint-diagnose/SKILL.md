---
name: ue-blueprint-diagnose
description: Diagnose an Unreal Blueprint with bounded static findings, graph and reference evidence, compile validation, and optional runtime correlation. Use when investigating Blueprint correctness, performance smells, Tick-related work, disconnected pins, call chains, or when a finding needs evidence before a fix.
---

# UE Blueprint Diagnose

Build an evidence-backed diagnosis without editing the asset. Treat static
findings as hypotheses until graph reachability or runtime evidence supports
them.

## Load the recipe

1. Call `ue_skills` with `action=get`, `skill=ue-blueprint-diagnose`, and
   `recipe=scan-and-verify`.
2. Read `references/diagnosis-recipe.md` only when the compact recipe is not
   enough.
3. Use the stable asset path supplied by the user. Never run an unscoped scan;
   constrain it to one asset or an explicit `/Game` path prefix.

## Discover exact APIs

Before every operation, call `ue_context` with its dotted capability ID. If an
operation depends on the current Editor, also call `ue_capabilities` with the
same operation, `detail=full`, `live=true`, and `availableOnly=true`.

Do not infer parameter names from this Skill. The capability manifest is the
source of truth.

## Diagnose

1. Read the Blueprint summary and structure.
2. Run `blueprint.scan` only within the selected scope.
3. Inspect high-value findings before informational noise. Use stable
   `findingId`, `ruleId`, severity, confidence, graph, node GUID, and evidence.
4. Read the finding's exact graph, then use `blueprint.graph.describe` for a
   bounded K2 execution-edge projection. Select one subflow with the exact
   `entryPoint` name, or use the stable `entryNodeId` from the graph read when
   names are ambiguous. For a Tick claim, begin at `Event Tick` and follow the
   returned `edgeId` records by node GUID and pin GUID to the candidate call.
5. Continue independent node and edge pages only through `nextNodeOffset` and
   `nextEdgeOffset`. Treat `partial`, `scanExhausted`, `nodesTruncated`, or
   `edgesTruncated` as incomplete evidence. An
   `execution_entry_scan_incomplete` error means the entry itself was not
   disproved; narrow the graph or raise limits within the live schema.
6. Follow cross-asset calls and references only after locating the candidate
   node. Use `blueprint.asset.search_by_type` only for an explicit type-impact
   question and honor its asset page plus variable/node/pin/link budgets. A
   same-graph result or data-pin connection still does not establish execution
   reachability.
7. Compile-validate the Blueprint without saving it.
8. Correlate a retained scan with Kismet trace evidence only when a valid
   runtime run exists.

Do not claim that a call is Tick-reachable merely because it appears in the
same graph. Do not put Blueprint debug sessions, breakpoints, or trace capture
inside UE Workflow DSL.

`blueprint.component.list`, `blueprint.component.get`, and
`blueprint.component.property.set` currently address components declared by
the selected Actor Blueprint's local SCS. Read-only `BlueprintVisible`
properties may appear in `component.get`, but the setter accepts only editable,
persistent local-template properties. Native, inherited, Widget Blueprint, and
Level Blueprint components are not covered by this component mutation surface.

## See results

Return a compact diagnosis containing:

- the inspected asset and graph identity;
- finding counts grouped by severity and rule;
- the strongest findings with exact evidence locations;
- the Event Tick execution-pin path, or an explicit statement that it was not
  established;
- compile validation status;
- runtime status as `observed`, `notObserved`, or `notMeasured`;
- the next safe inspection or fix step.

Say explicitly when evidence is inconclusive. Do not edit or save the
Blueprint unless the user separately authorizes a fix.
