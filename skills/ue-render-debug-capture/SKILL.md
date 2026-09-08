---
name: ue-render-debug-capture
description: Capture Unreal viewport debug views, collect Niagara AsyncGpuTrace runtime evidence, or analyze DRED and rendering failure logs offline. Use for GPU crashes, Niagara runtime diagnosis, Buffer Visualization, Ray Tracing Debug, Nanite, Lumen, and before/after rendering evidence.
---

# UE Render Debug Capture

For retained crash logs, load recipe `offline-render-failure`. For Niagara GPU
queries, load `niagara-runtime-evidence`. Both use
`references/runtime-and-failure-evidence.md`. Offline log analysis does not
require a running Editor. Read operation schemas before executing them.

Treat a debug view as bounded visual evidence, not as a raw render-resource
readback. Never infer unavailable GBuffer or RDG values from PNG colors.

## Load the recipe

1. Call `ue_skills` with `action=get`,
   `skill=ue-render-debug-capture`, and `recipe=capture-and-compare`.
2. Load `references/render-debug-evidence.md` for target, restoration, and
   pixel-analysis boundaries.
3. Discover every operation through `ue_context`; do not guess visualization
   IDs or version-specific availability.

## Capture and verify

1. List modes for the exact Editor viewport or PIE `sessionId + generation`.
2. Select only a mode reported `available=true`. Keep an unavailable result as
   evidence instead of switching to a vaguely similar mode.
3. Capture the named mode. Require a lossless PNG, render fingerprint, target
   identity, camera/view state, and successful state-restoration evidence.
4. Analyze the persisted capture for mode-aware bounded statistics.
5. When a compatible baseline capture exists, compare the two capture IDs.
   Treat fingerprint incompatibility as `inconclusive`, not as a regression.

## Evidence

Report mode and target identity, availability reasons, capture IDs and hashes,
render fingerprints, restoration status, bounded diagnostics, changed-pixel
ratio, changed regions, and diff artifact ID. State explicitly that this is an
8-bit viewport debug-view capture unless the result proves a true floating-point
render target and EXR source.

