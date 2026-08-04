# Trace and Unreal Insights workflow boundaries

## Recording targets

- `editor` records the current Editor process with `FTraceAuxiliary`.
- `pie` is still an Editor-process trace. Use its unique begin/end Region and
  Bookmark as the default query range; never label it an isolated PIE process.
- `development` launches only a version-controlled profile. Require
  `production.trace.launch.plan`, its exact `approvePlanDigest`, and
  `confirmLaunch=true`. Do not supply an executable or raw command line.

Every recording must have explicit duration/file-size limits. Development
recording belongs to the lightweight runtime module in Development or
DebugGame Game targets. It is absent from Shipping and never hosts HTTP, MCP,
or Editor UI.

## Offline Worker

Resolve the Worker from `UEAI_TRACE_WORKER`, the installed Engine-versioned
plugin tool, or a source build. Verify the handshake before analysis: plugin,
Engine, protocol, contract digest, provider list, and Unreal Insights path.
Reject an Engine minor mismatch. Do not silently reroute an explicitly selected
backend.

Import is the only operation that accepts an external path. It copies and
hashes the trace into the Worker store and returns a registered `traceId`.
MCP imports must remain within project `Saved`, the Worker store, or
`UEAI_TRACE_ROOTS`. Later queries use the ID, not an arbitrary path.

For an explicit absolute path, the CLI's default `copy` import grants the
canonical parent only to an isolated one-shot Worker child. It does not widen
the CLI process or resident Worker. A `reference` import receives no temporary
grant and must already be under the Worker store or persistent
`UEAI_TRACE_ROOTS`, so the registered trace remains resolvable later.

## Semantic queries

Call `production.trace.provider.list` before querying. Each descriptor states
whether it was recorded, which semantic operations are implemented, missing
channels, and the related Unreal Insights panel. Provider availability varies
with the trace and Engine version.

Every query must provide a bounded time range where applicable, stable sort,
cursor, and limit. Export normalized JSON/CSV artifacts instead of returning an
unbounded event stream. Treat corrupt or partial traces and missing providers as
structured diagnostics; do not reinterpret them as empty successful results.

## Unreal Insights

`production.trace.open_in_insights` launches the matching
`UnrealInsights.exe -OpenTraceFile=<path>`. A versioned action mapping may apply
a supported panel or time range. If it cannot, return `viewApplied=false` while
still opening the trace. Unreal Insights is the visual viewer; TraceServices is
the structured-result backend. Do not use mouse/keyboard automation for panel,
track, zoom, or window-layout operations.
