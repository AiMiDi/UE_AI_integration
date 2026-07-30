# Landscape / Water Capability Pack

This pack adds a deterministic terrain-production boundary for Unreal Engine
5.3–5.7. It intentionally does not expose arbitrary real-time Sculpt or Paint
strokes.

## Read and evidence capabilities

- `scene.landscape.list`
- `scene.landscape.get`
- `scene.landscape.layers.get`
- `scene.landscape.validate`
- `scene.landscape.snapshot`
- `scene.landscape.diff`
- `scene.landscape.heightmap.export`
- `scene.landscape.weightmap.export`
- `scene.water.list`
- `scene.water.get`
- `scene.water.validate`

A Landscape snapshot hashes the complete loaded height extent, every loaded
weight layer with a Layer Info asset, material/layer assignments, related Water
state, and a bounded World Partition/Data Layer/HLOD/PCG impact summary.
Separate package evidence records loaded dirty state, disk path, size, and
SHA-256 when the package is saved and at most 512 MiB.

Height artifacts use row-major little-endian unsigned 16-bit samples
(`r16le`). Weight artifacts use one row-major unsigned 8-bit channel (`r8`).
Each export includes a JSON sidecar with width, height, source identity, and
SHA-256.

## Planned writes

`scene.landscape.change.plan` accepts 1–32 operations:

- `importHeightmap`
- `importWeightmap`
- `setMaterial`
- `replaceLayerInfo`
- `waterCreate`
- `waterUpdate`
- `waterDelete`

`waterDelete` is restricted to Water actors created and tagged by this service.
Planning captures the actor class, object name, actor GUID, original Level,
transform, complete UE actor-property export, package identity, and external
actor package state. Execution persists the complete single-actor T3D export
and its SHA-256 before deletion. Rollback recreates the actor through the
matching Water actor factory and restores the export into the original
Level/package. It is verified only when class, GUID, Level, package mode,
external package, managed identity, and the complete exported-property digest
all match the baseline.

The plan resolves each target, verifies source bytes and dimensions, detects
conflicting operations, captures semantic/package baselines, estimates recovery
bytes, and returns an exact `planDigest`.

An `importWeightmap` and `replaceLayerInfo` operation cannot target the same
Landscape layer in one request. The layer binding is part of the weight data
contract, so changing both in one transaction would make recovery order
ambiguous.

`scene.landscape.change.execute` requires the original `request`, current
`approvePlanDigest`, `confirmWrite=true`, and a non-empty `requestId`. It
re-plans before mutation. A repeated request ID with the same payload returns
the original run; a different payload is rejected.

Before the single Editor transaction starts, the service writes
`before.snapshot.json`, `plan.json`, `recovery.manifest.json`, and full raw
backups for affected height/weight data and deleted Water actors under:

```text
Saved/UE_AI_integration/Landscape/Runs/<runId>/
```

After application it captures and persists a new snapshot and bounded diff.
Any operation, artifact write, or read-back/Diff failure triggers automatic
restoration and a persisted rollback snapshot. Failed runs remain registered
for a safe retry. Explicit `scene.landscape.change.rollback` requires the same
Editor world object/package, `runId`, the exact original execution `requestId`,
and confirmation. It first verifies that the current semantic and package
digests still match the recorded post-execution or failed-run retry baseline;
success requires the restored semantic hash to equal the original hash.

## Safety boundaries

- Import files must remain under the current project directory.
- Import sources must be regular files. Symbolic links and directory junctions
  are rejected after resolving the physical on-disk path, even when their
  lexical path appears to be inside the project.
- Raw import covers the complete Landscape extent and is rejected while any
  component in that extent is unloaded or World Partition streaming is active.
- Raw import is rejected for Landscapes with Edit Layers content.
- Water actors are created through an enabled Unreal Water actor factory.
- A managed Water create records the resulting actor GUID and external package.
  Create rollback detaches and removes that external package and fails
  verification if an external actor package remains attached or appears on
  disk.
- Water update is limited to transform and label. Water delete is limited to
  service-managed actors with a complete bounded actor export; unmanaged Water
  actors are rejected.
- Water delete rollback requires the original loaded Level and external actor
  package identity in the same Editor instance. Missing/corrupt actor export,
  class/factory drift, package drift, or property-digest mismatch fails
  rollback instead of claiming a shallow restoration.
- No asset is automatically saved.
- Rollback ownership is memory-resident and does not survive Editor restart.
- Impact data covers loaded actors/components only. Dedicated HLOD, PCG,
  navigation, cook, and package jobs remain the completion authority.

## Release acceptance

Static manifest/handler tests are insufficient. Run the pack against a real
saved Partitioned World containing:

- a Landscape with material and at least one Layer Info/weight layer;
- a Water Body;
- Data Layer and HLOD assignments;
- an overlapping PCG component.

The acceptance run must cover r16/r8 export, import, material/layer changes,
Water create/update/delete, delete rollback, delete failure restoration,
create rollback with no external-package residue, unmanaged-delete rejection,
source or baseline conflict, explicit rollback,
and equality of the original/restored semantic digest. A
non-partitioned Main map is not valid World Partition evidence.

---

## 中文边界说明

0.7.0 的 Landscape/Water 包只覆盖确定性的地形生产路径：读取、快照/Diff、
高度图与权重图导出，以及经过 plan digest、`confirmWrite` 和 `requestId` 的
完整范围导入、材质/Layer Info 修改和受限 Water Actor 创建/更新/删除。
导入源必须是工程目录内的普通文件；符号链接与目录联接会在解析物理路径后被拒绝。
同一请求不能同时对同一层执行权重图导入和 Layer Info 替换。
`waterDelete` 只允许删除插件创建并带管理标记的 Water Actor；计划和
执行会记录完整 Actor T3D 属性、类、对象名、Actor GUID、原 Level、包与外部
Actor 包身份。回滚必须通过 Water Factory 在同一 Editor 实例中恢复这些信息，
并重新核对完整属性摘要；任一证据不一致都返回失败。它不会提供任意
笔刷式实时 Sculpt/Paint，也不会自动保存资产。

执行前会写入原始高度/权重备份、语义快照、包证据和恢复 manifest；执行或读回
失败时自动恢复并持久化恢复证据；失败 run 仍可用原始 requestId 重试 rollback。
显式 rollback 需要同一个 Editor world 对象与 package，并先检查当前状态没有被
外部修改，再恢复并重新校验原始
语义 hash，因此不能宣称跨 Editor 重启的恢复能力。World Partition、Data Layer、
HLOD 与 PCG 只给出当前已加载对象的影响摘要；最终验收必须在真实的 Partitioned
World 测试地图中完成，并覆盖 Water Create 回滚后外部 Actor 包无残留；普通
Main 地图不构成 WP 闭环证据。
