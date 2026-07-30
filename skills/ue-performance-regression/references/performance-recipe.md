# Performance regression recipe

## Fixed inputs

Allow only the caller-declared intended code, asset, or configuration change
to differ. Keep every other controlled input stable: map, RHI, GPU, resolution,
scalability, critical CVars, VSync/FPS cap, warmup, sample window, repeat
count, and scenario steps. Record revision and settings on both sides, but do
not require the revision to be identical when the intended change is a code
or asset revision. Capture `scene.render.context.get` before each run and
distinguish measured fingerprint fields from controls asserted by the caller.

## Operation order

1. Read `scene.pie.status`, `scene.render.context.get`, and optionally
   `scene.render.memory.sample`.
2. Use a supplied succeeded baseline run ID, or call
   `production.performance.run` before the intended change.
3. Poll `production.job.status` with the returned job ID.
4. Read `production.performance.result.get` with the resulting run ID.
5. Let the caller establish the intended candidate state. This Skill never
   edits code/assets or changes revisions.
6. Repeat context capture and steps 2–4 for the candidate using identical
   measurement settings.
7. Call `production.performance.compare` with both run IDs and explicit
   checks.
8. On regression, or when the caller explicitly requests attribution:
   start trace, check trace status, replay the same candidate workload, and
   stop trace in cleanup even if replay fails. Analyze the stopped trace, poll
   `production.job.status`, then fetch the terminal result and only the needed
   artifacts.
9. Diagnose the candidate with `production.performance.diagnose`. Pass the
   succeeded trace-analysis job when one exists.
10. Generate the final artifact set with
    `production.performance.report.generate`.

`production.performance.run.captureTrace=true` is a preplanned capture. Reuse
it only when it contains the same candidate workload and satisfies the
analysis request; otherwise perform the explicit replay above.

For a succeeded standalone run, the returned run ID may also be its `traceId`
when the run owns a registered `.utrace` artifact. Analyze that run ID
directly, then pass only the resulting analysis job from the same trace into
diagnose/report. Do not attach a successful but unrelated analysis job.

For scenario mode, the scenario must contain exactly one `metrics.begin`
before one `metrics.end`. Use `ue_context` for the current schema instead of
copying a scenario AST from this document.

## Verdict rules

- Compare only succeeded runs.
- The comparison is conclusive only when measured compatibility fields match
  and the caller confirms that the declared intended change was the only
  controlled difference.
- A `frameMs` near 16.67 ms does not by itself prove either a limiter or unused
  performance headroom. `frameLimited` requires timing that matches an explicit
  FPS cap, or enabled VSync plus a matching common refresh interval; repeat
  uncapped before CPU/GPU attribution. If the fingerprint says both controls
  are off but cadence remains stably near 16.6667 ms, require
  `inconclusive` plus `frameLimiter.suspected=true`; inspect driver/external
  limiters and child-process CVar capture before attributing CPU or GPU.
- `executionTarget=standalone` uses an independent durable Editor game process,
  visible render window, child runtime fingerprint, verified named Camera,
  per-repeat wall-clock CSV capture, log, and optional trace. It requires a
  fixed standard profile and rejects input replay that the process runner
  cannot reproduce exactly.
- `standardProfile.gameInstanceMode` defaults to `project`. Choose `minimal`
  only to bypass a project GameInstance launcher or service guard: it uses a
  process-local base `Engine.GameInstance` override without changing project
  configuration. Record it in the environment fingerprint, never compare it
  directly with `project`, and do not treat it as full game-startup evidence.
- If no run-bounded log window is available, report log health as
  `unavailable`; absence of evidence is not a clean log.
- The reader-facing HTML must surface log health, Top CPU scopes, GPU
  interval/aggregate evidence, and next steps. Artifact navigation must use
  escaped opaque IDs and the `production.job.artifact.get(jobId, artifactId)`
  contract; never embed source filesystem paths or executable links.
- Poll durable jobs; an accepted launch response is not a completed result.
- After trace start succeeds, always attempt trace stop before reporting any
  replay or analysis failure.
- Performance and Trace resource locks are long-task behavior, not Workflow
  DSL.
