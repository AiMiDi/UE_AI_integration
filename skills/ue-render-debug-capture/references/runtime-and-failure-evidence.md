# Runtime and rendering failure evidence

The local SilverPalace 5.4.1 validation record is
[`2026-09-08-niagara-render-evidence.json`](../../../docs/validation/2026-09-08-niagara-render-evidence.json).
It records the module hashes, CPU contract results, D3D12 GPU smoke result and
remaining scene acceptance limits. Restart the MCP stdio server after updating
its built `dist` and manifests so discovery exposes the new operations.

## Niagara AsyncGpuTrace

1. Discover schemas with `ue_context` for `content.niagara.runtime.inspect` and
   `content.niagara.runtime.capture`.
2. Execute `content.niagara.runtime.inspect` through `ue_content`. Select exact
   loaded world and Data Interface paths; use `offset` and `limit` to page both
   object arrays independently. Loaded asset DIs can be shared: inventory alone
   does not prove that a component uses a particular proxy at runtime.
3. For PIE, get the current `sessionId` and `generation` from `scene.pie.status`.
   Pass them to capture. Do not start/restart PIE just to obtain a session.
4. Capture with `world`, `dataInterface`, `resultLimit` (1–1024; default 64), and
   `timeoutSeconds` (1–30; default 5). The asynchronous request observes the next
   matching dispatch. It never waits for GPU idle or changes provider settings.
5. Require `state=complete` before using GPU counts. A timeout without a matching
   dispatch means no execution was observed in the window; it does not mean zero
   queries or prove that the DI is unused. Cancellation and unsupported bridge
   builds have distinct errors. World cleanup and PIE generation changes cancel.

Evidence is **one dispatch per DI in one world, aggregated across instances**.
`queryHighWatermark` is TraceCounts[0], including reservations, rejected indices
and possible holes. It can exceed capacity. It is not an issued-ray count.
`hitDistancesByQueryIndex` is a bounded prefix. Unwritten holes can retain old
allocation bytes. `positiveHitSlots` counts positive finite distances in that
prefix, not verified collision events or a statistical hit rate. Non-finite
distances are JSON null and counted separately. Missing readback metrics are null.

`configuredProvider` is the dispatch's resolved configuration; compare it with
the inventory's original asset enum before discussing fallback. The ordered
provider rows report availability reasons, dispatch path, and relevant resource
references. Missing providers can be unsupported or absent from the configured
priority list. `selectedProvider=none` plus `resultsCleared=true` observes the
clear path. Setup frames describe the helper's last PostRenderOpaque call and
its first ViewFamily; a provider can retain older state if it skipped that call.
TLAS/view/resource reference presence does not prove build completion, ownership
or safe lifetime. Previous result allocation presence and frame do not prove
valid particle Query IDs or previous-frame consumption.

### Optional engine bridge

Inventory works with Niagara enabled. GPU capture additionally needs the
`NiagaraAsyncGpuTraceDiagnostics.h` bridge in the local engine. Without the
header, the plugin compiles and returns `niagara_runtime_evidence_unavailable`.
Stock engines are not silently given an approximation.

The SilverPalace implementation is in `Engine/Plugins/FX/Niagara/Source/`:

- `Niagara/Public/NiagaraAsyncGpuTraceDiagnostics.h` and
  `Niagara/Private/NiagaraAsyncGpuTraceDiagnostics.cpp` own requests and readbacks.
- `Niagara/Private/NiagaraAsyncGpuTraceHelper.{h,cpp}` observes dispatch and setup.
- `NiagaraShader/Public/NiagaraAsyncGpuTraceProvider.h` and the HWRT/GSDF provider
  headers and implementations expose diagnostic context.

All bridge code and provider virtual additions are `WITH_EDITOR` only. Keep the
plugin and engine changes together; rebuild `Niagara`, `NiagaraShader`, and
`UE_AI_integration`, then load matching DLLs before testing. Disabling the plugin
does not leave a background collector running. Requests are bounded to 16 pending
or in-flight readbacks, and completed callbacks release their budget even when
discarded during world teardown. The plugin allows one pending capture at a time.

CPU contract tests: `Niagara.AsyncGpuTrace.Evidence.ReadbackSemantics`,
`Niagara.AsyncGpuTrace.Evidence.FailureAndCancellation` and
`UE_AI_integration.Niagara.Runtime`. These test readback interpretation, failure,
late callbacks and cancellation. `Niagara.AsyncGpuTrace.Evidence.GpuReadback`
requires a real RHI and uses an isolated helper with temporary scratch buffers to
exercise the clear shader, GPU-written count, bounded copy and asynchronous fence.
Neither set replaces a live HWRT/GSDF collision scene test.
For GPU acceptance, collect complete captures from known HWRT, GSDF and no-provider
cases, and test world teardown/PIE restart while pending. Keep each result with
engine/module identity and the exact world/DI; never call a CPU test GPU evidence.

## Offline render failures

Execute `scene.render.failure.analyze` through `ue_scene` with:

```json
{
  "projectRoot": "S:/SilverPalace/Project",
  "files": ["Saved/Crashes/example/CrashContext.runtime-xml", "Saved/Crashes/example/Project.log"],
  "maxBytesPerFile": 2097152,
  "limit": 64
}
```

Use actual discovered files; `example` is a placeholder. The `localProject`
backend works with the Editor offline and also accepts a directory containing
only archived diagnostics. No .uproject is required for this operation. Input
paths must stay within the canonical root, including through links/junctions.
No directory-wide search or binary debugger runs implicitly.

Each text source includes an analyzed byte range, SHA-256 for exactly that range,
mtime, size, line-number scope and truncation. Logs use bounded UTF-8 tails;
partial first lines are discarded. XML must fit completely; DTD/entity
declarations are rejected. CrashContext uses a small whitelist extractor, not a
general XML validator. Command lines, credentials and URLs are excluded/redacted.
Logs changing during the read are rejected; use finalized crash artifacts.

DRED command-list/queue context, last-completed operations, fault addresses,
active/recently freed objects, tracked and released resource ranges, frame IDs,
device-removal codes, Ensures, rendering call stacks, module load errors and
resource-state errors remain observations. Reported call-stack symbols are not
independently verified by this analyzer.
The analyzer does not merge different files into a causal timeline. Binary
`.dmp` and `.nv-gpudmp` artifacts are metadata-only and require a debugger/vendor
adapter plus matching symbols. Empty or unmatched logs are inconclusive.

Keep root cause **unproven** until matching build/symbols, resource lifetime or
reproduction establishes it. An active breadcrumb marks in-flight work; a nearby
allocation or previous Ensure alone cannot explain a later device removal.
