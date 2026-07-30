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
4. Read the finding's exact graph. For a Tick claim, begin at `Event Tick` and
   follow connected execution output pins node by node to the candidate call.
   Data-pin proximity, membership in the same graph, and a cross-asset call
   edge do not establish execution reachability.
5. Follow cross-asset calls and references only after locating the candidate
   node.
6. Compile-validate the Blueprint without saving it.
7. Correlate a retained scan with Kismet trace evidence only when a valid
   runtime run exists.

Do not claim that a call is Tick-reachable merely because it appears in the
same graph. Do not put Blueprint debug sessions, breakpoints, or trace capture
inside UE Workflow DSL.

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
