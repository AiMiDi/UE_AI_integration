# Runtime inspection and rendering failure evidence

The plugin uses existing engine APIs; it does not install an engine patch.
Historical validation is retained with the originating task evidence rather
than treated as current runtime proof. See
[`UE AI improvement plan`](../../../docs/UE_AI_IMPROVEMENT_PLAN.md) for the
evidence boundary and the current follow-up matrix.

## Niagara runtime inventory

1. Discover `content.niagara.runtime.inspect` with
   `ue-cli help content.niagara.runtime.inspect --json`; check its live schema
   when connected to Editor.
2. Execute `ue-cli content.niagara.runtime.inspect --limit 32 --json` to list
   loaded Editor/PIE worlds, components, and AsyncGpuTrace Data Interfaces.
3. Select an exact world returned by the inventory and pass `--world <path>`
   when narrowing component ownership. Use `--offset` and `--limit` to page
   the component and DI arrays independently.
4. Verify the returned identities, page bounds, and `bindingEvidence`. A loaded
   asset DI can be shared across components and worlds; its presence does not
   establish which component uses its proxy during simulation.
5. Report `active`, `paused`, the System asset, and the configured provider enum
   as observed object state. Do not promote configuration to the actual selected
   GPU provider or infer a dispatch count, collision rate, or particle result.
   This inspection neither starts PIE nor changes its current generation.

MCP fallback uses `ue_context` for schema discovery and `ue_content` with the
same operation and parameters. Niagara must be enabled; no custom engine header,
provider extension, GPU readback hook, or session controller is required.

### Scope without engine changes

The plugin retains graph/configuration audits, loaded-object inventory, viewport
debug captures, existing Trace capabilities, and offline rendering-failure
analysis. The former `content.niagara.runtime.capture` operation has been
removed from the handler registry, capability catalog, and Skill recipes.

Use recipe `niagara-simcache-observe` for recorded particle/system attributes.
The plugin implements capture, inspect, read, export and release through public
UE SimCache APIs; no engine patch is required. This is separate from inventory
and cannot read AsyncGpuTrace's private request/result buffers.

### SimCache observation through CLI

Discover the live schema before each call with `ue-cli help <operation>
--live-schema --json`. Then execute the same operation through `ue-cli`, using
`--params-file` for JSON parameters. Do not infer runtime support from the local
catalog when Editor still has an older plugin DLL loaded.

1. Call `content.niagara.runtime.inspect` and select its exact `world` and
   `component` paths. Emitter rows include `captureAttributePrefix`, for example
   `Fountain.Particles.`. Append compiled attribute names such as `Position`,
   `Velocity`, `Color`, `Age` or `ID` only when they exist in this system.
2. Call `content.niagara.simcache.capture` with `world`, `component`, `attributes`
   (1-32 fully qualified names), `frames` (1-32, default 8), `captureRate`
   (1-16, default 1), and `timeoutSeconds` (1-20, default 10). The component must
   already be active and unpaused. No activation, manual simulation advance,
   asset edit, replay attachment, or PIE restart occurs.
3. Check `status`, `reason`, `frameCount` and `retained`. `complete` means the
   requested frame count was written. `partial` preserves fewer valid frames
   after timeout/cancellation/target loss. `failed` is not usable evidence even
   when the transport returned successfully. A simulation reset invalidates the
   cache; a paused simulation cannot create duplicate frames.
4. Call `content.niagara.simcache.inspect` with `captureId`. Its emitter indices,
   stored attribute names and `frameInstanceCounts` are authoritative. Index -1
   selects system attributes. These names differ from the fully qualified
   capture input: a particle attribute is usually stored as `Position`. Omit
   `captureId` to list retained recordings, including after a disconnected client.
5. Call `content.niagara.simcache.read` with `captureId`, `frame`, `emitterIndex`,
   `attribute`, `offset` and `limit` (1-256, default 64). Read `hasMore` and
   `nextOffset`. Each instance contains separate `floats`, `halfs` and `ints`,
   with the corresponding component counts and Niagara type. Values are raw
   recorded simulation coordinates, without world-space conversion or rebasing.
   Nonfinite values become JSON null with `nonFiniteValues`; row indices are
   frame-local, so use an explicitly recorded particle ID for correlation.
6. `content.niagara.simcache.export` accepts the same parameters as `read` and
   saves that page plus capture metadata under `Saved/UEAI/NiagaraSimCache`.
   It returns the UTF-8 file path, byte count and SHA-256. This exports bounded
   diagnostic JSON, not an entire replayable `.uasset` cache. Follow pagination
   and iterate frames/attributes when collecting more evidence.
7. Call `content.niagara.simcache.release` with `captureId` after use. Four
   recordings can be retained per Editor instance, with no silent eviction.
   Shutdown clears transient caches; exported JSON files remain on disk.

Capture uses explicit attributes and disables Data Interface caching, debug
data, interpolation and rebasing. At most 64 emitters are supported. A 64 MiB
logical attribute payload budget is checked after each captured frame; an
oversized cache is discarded. This is **not** a hard process-memory limit:
UE ID tables, cache overhead and GPU readback staging are additional. A public
`WriteFrame` call cannot be preempted by the plugin's wall-clock deadline.
GPU recording can flush pending ticks and wait for readback, so these captures
are unsuitable as undisturbed GPU timing measurements. Attribute reads reject
more than 16 MiB of scratch data or 64 components before calling the public API,
which otherwise reads the entire attribute before output pagination.

The previous exact-dispatch sampler depended on Niagara's internal provider
selection and buffer lifetime points. Those details are not exposed as a
complete supported public capture contract in this branch. Do not restore the
engine bridge or reach into private headers to recreate it. A loaded TLAS
reference, a configured provider, or an active component alone does not prove
a successful collision.

### Upgrade and verification

Rebuild the affected engine modules and plugin after removing an already
compiled bridge, then reload matching binaries during an authorized Editor
restart. Removing source files does not unload the bridge from an existing
Editor process. Do not use its old capture operation as verification of this
change.

Automation under `UE_AI_integration.Niagara.Runtime` checks missing-world
handling, removed bridge capture registration, inventory pagination and scope.
`UE_AI_integration.Niagara.SimCache` adds attribute layout/nonfinite/pagination
checks, input preflight, and CPU/GPU lifecycle fixtures covering two real frames,
attribute reads, JSON export/hash verification, release and partial cancellation.
Both lifecycle fixtures require a real RHI: Niagara does not activate components
under NullRHI, including CPU emitters. Run isolated Automation with the existing
`-UEAIDisableServer` process flag to avoid unrelated clients' HTTP errors entering
test results. No private GPU collision capture is claimed by these tests.

## Offline render failures

Discover `scene.render.failure.analyze` through `ue-cli help` and execute it
through the short CLI; MCP fallback uses `ue_scene` with these parameters:

```json
{
  "projectRoot": "C:/Projects/ExampleProject",
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
