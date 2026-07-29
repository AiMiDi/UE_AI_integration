# Content Asset Pipeline

UE_AI_integration 0.6.0 adds a narrow, plan-gated settings workflow for
Static Mesh and Texture assets. It deliberately reuses the existing
`ue.change-plan.v1` contract instead of adding a second import or approval
protocol.

## Import and reimport

Use the existing capabilities:

- `content.asset.change.plan`
- `content.asset.change.execute`
- `content.asset.change.rollback`

The `import` and `reimport` actions now accept only normalized, absolute local
files. Relative paths, URLs and network shares are rejected. Import
destinations and all reimported assets must be under `/Game`. Import never
overwrites an existing destination. Reimport remains a one-action plan and
reports rollback as unavailable because UE import factories cannot provide a
general lossless inverse.

An approved execute call must include an `/api/execute` envelope `requestId`,
the exact `approvePlanDigest` returned by the current plan, and
`confirmWrite: true`.

## Static Mesh settings

- `content.static_mesh.settings.get`
- `content.static_mesh.settings.plan`
- `content.static_mesh.settings.apply`
- `content.static_mesh.settings.validate`

The allowlist is `naniteEnabled`, `allowCpuAccess`,
`lightMapCoordinateIndex`, and `lightMapResolution`. Apply is restricted to
`/Game`, performs one `PostEditChange` rebuild, reads the settings back, and
optionally saves. UE 5.3 requires the Static Mesh editor to be closed before
apply; the capability rejects an open editor instead of risking a rebuild
crash.

## Texture settings

- `content.texture.settings.get`
- `content.texture.settings.plan`
- `content.texture.settings.apply`
- `content.texture.settings.validate`

The allowlist is `compression`, `sRGB`, `virtualTextureStreaming`, and
`lodBias`. Apply is restricted to `/Game`, rebuilds the texture through
`PostEditChange`, verifies the exact read-back hash, and optionally saves.

Both settings planners accept:

```json
{
  "request": {
    "asset": "/Game/Props/SM_Chair.SM_Chair",
    "settings": {
      "naniteEnabled": true,
      "allowCpuAccess": false
    },
    "persistence": "dirtyOnly"
  }
}
```

Apply sends the same request plus:

```json
{
  "approvePlanDigest": "<64 character digest>",
  "confirmWrite": true
}
```

The `requestId` belongs in the `/api/execute` envelope. `saveOnSuccess` is
refused when the package was already Dirty so that an automation call cannot
silently save unrelated editor changes.

## 中文说明

0.6.0 不新增重复的导入接口，而是继续使用
`content.asset.change.plan/execute/rollback`。导入与重导入仅允许本机绝对
文件路径，拒绝相对路径、URL 和网络共享；目标资产必须位于 `/Game`。

Static Mesh 与 Texture 各提供 `get/plan/apply/validate` 四项能力。写入必须
使用当前计划返回的 `approvePlanDigest`、`confirmWrite: true` 和执行
envelope 中的 `requestId`。`apply` 会重建并读回验证；如果选择
`saveOnSuccess` 且包在调用前已经 Dirty，则直接拒绝，避免误保存用户的
其他未提交修改。
