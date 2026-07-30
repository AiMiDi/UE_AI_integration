# Landscape and Water recipe boundaries

## Deterministic raster contract

- Height import/export is the complete loaded extent in row-major little-endian
  unsigned 16-bit form (`r16le`).
- Weight import/export is one complete loaded layer in row-major unsigned
  8-bit form (`r8`).
- `flipY` is explicit. Width and height come from the current Landscape extent.
- Import paths must stay inside the current project directory and resolve to
  regular files; symbolic links and directory junctions are rejected.
- A weightmap import and Layer Info replacement cannot target the same layer in
  one request.
- Landscape Edit Layers are not modified by raw v1 imports.

## Approval and recovery

Planning resolves every target, reads every raster source, records its SHA-256,
captures the current semantic snapshot and related package evidence, and reports
the recovery byte budget. Execution repeats planning and rejects a changed
digest, source, world, Landscape, Water actor, or package baseline.

Before mutation, execution writes:

- `before.snapshot.json`
- the normalized plan
- full raw recovery rasters for affected height and weight data
- a recovery manifest containing material, Layer Info, Water, and file hashes
- one full verified T3D actor export for every admitted Water deletion

The first version keeps the live run owner in the same Editor instance. Recovery
artifacts are evidence and restoration input, but are not a durable cross-restart
journal.

## Water ownership

Water creation uses an enabled Unreal Water actor factory and assigns a stable
managed identity. Its complete created state and external package are retained
for rollback; verified rollback must remove the actor and leave neither an
attached nor on-disk external package. Updates may change transform and label.
Delete is admitted
only for service-managed actors and only after planning captures a complete
actor export plus class, object name, Actor GUID, level, package, external-actor
package, and full property digest. Execution reserves the original object
identity, deletes deterministically, and retains the verified export. Failure
or explicit rollback must recreate the actor through its Water factory, import
the complete property state, and match the original identity and property
digest. An unmanaged actor or incomplete recovery snapshot is rejected before
mutation.

## World Partition boundary

The change plan reports whether the current map is partitioned, assigned Data
Layers and HLOD layers for affected actors, and bounded loaded PCG components
whose bounds overlap the target region. This is impact evidence, not completion
proof. Unloaded actor descriptors, HLOD builds, PCG graph completion, navigation,
cook, and package validation require their dedicated capabilities and jobs.

Release acceptance must use a real saved Partitioned World test map containing
a Landscape, Data Layer assignment, HLOD assignment, PCG component, and Water
Body. A non-partitioned Main map is not valid evidence for this pack.
