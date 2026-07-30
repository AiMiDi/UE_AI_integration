---
name: ue-performance-regression
description: Measure and compare repeatable Unreal performance runs with environment fingerprints, durable jobs, percentile evidence, thresholds, and optional trace escalation. Use when checking frame-time regressions, comparing before and after changes, creating a performance gate, or explaining a failed budget.
---

# UE Performance Regression

Produce a comparable baseline and candidate result. A percentile without a
matching environment fingerprint is not regression evidence.

## Load the recipe

1. Call `ue_skills` with `action=get`, `skill=ue-performance-regression`, and
   `recipe=compare-repeatable-runs`.
2. Load `references/performance-recipe.md` for scenario and trace details.
3. Require a description of the intended change plus mode, warmup, sample
   window, repeat count, and checks. Ask the caller for any missing value.
4. Confirm the controls that should remain fixed. Record only fields actually
   returned by the current render context and performance fingerprint; do not
   claim that an unmeasured CVar or setting matched.

## Discover exact APIs

Call `ue_context` for every operation before using it. Check live availability
with `ue_capabilities` when the Editor configuration can affect support.
Never infer an API named `production.performance.status`; poll
`production.job.status`.

## Measure

1. Capture render context, PIE state, and a memory baseline.
2. If the caller supplies a succeeded baseline run ID, use it. Otherwise run
   the baseline with fixed warmup, sample window, repeat count, and checks.
3. Poll the returned job ID to a terminal state, then read the performance run.
4. Pause for the caller's already-authorized intended change. This Skill does
   not edit assets, switch revisions, or manufacture a candidate state.
5. Capture context again and run the candidate with the same measurement
   settings.
6. Compare the two run IDs with explicit metrics and thresholds.
7. If the comparison regresses, or attribution was requested, trace escalation
   becomes required: start tracing, replay the same candidate workload, stop
   tracing even if replay fails, analyze it, and poll the analysis job.

`captureTrace=true` pre-captures a trace during a performance run. It does not
replace failure escalation unless that trace contains the same candidate
workload and is usable for the requested analysis.

Performance capture is a heavy job, not a UE Workflow asset-edit step. Put the
MCP `requestId` in the domain-tool envelope, not inside business parameters.

## See results

Report:

- baseline and candidate run IDs;
- the declared intended change and which side produced each run;
- environment compatibility or the exact mismatch;
- sample count and Frame/Game/Render/RHI/GPU p50, p95, and p99;
- threshold result per metric;
- worst-frame or budget evidence;
- trace and artifact IDs when escalation ran.

Return `inconclusive`, not pass or fail, when environment fingerprints differ
or either run is incomplete.
