# Render debug evidence boundaries

## Target and availability

Resolve the exact Level Editor viewport or PIE `sessionId + generation` before
capturing. Query live availability every time: Buffer Visualization, Ray
Tracing Debug, Nanite, Lumen, Virtual Shadow Map, GPU Skin Cache,
Strata/Substrate, Groom, and other extension modes depend on Engine version,
RHI, project settings, and optional modules. An unavailable entry plus its
reason is a valid result; do not approximate it with console commands.

## Capture and restoration

A capture must retain the initial View Mode, ShowFlags, camera, exposure,
selection, viewport transform, and zoom. It may temporarily change those values
only for the requested viewport. Require two rendered frames and the render
fence before readback. On success, timeout, or error, restore the saved state and
verify the restoration result.

The normal artifact is lossless PNG produced from the viewport's 8-bit pixel
readback. This is semantic debug-view evidence, not arbitrary access to RDG
textures, GBuffer A-E, SceneDepth, or a floating-point render target. Accept EXR
only when the result explicitly identifies a real floating-point source.

## Analysis and comparison

Use mode-aware statistics only when the encoded colors have a documented
meaning. Keep histograms, anomalous-pixel counts, coverage ratios, and changed
regions bounded. Do not turn a screenshot color into an undocumented material,
normal, depth, or lighting value.

Compare only matching target, dimensions, RHI/GPU, Engine/plugin version, DPI,
theme, mode, and view transform fingerprints. When the implementation reports a
fingerprint mismatch, return `inconclusive` even if the images look similar.

