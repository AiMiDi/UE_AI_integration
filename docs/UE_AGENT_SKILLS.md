# UE Agent Skills 与 Capability Recipes

UE Agent Skill 层把领域知识、调用顺序、风险边界和验收证据包装成可按需加载的
配方，同时保留 manifest capability、UE Workflow 和 Editor handler 的唯一
执行权威。

它实现四段闭环：

```text
ue-cli skills (MCP fallback: ue_skills)
        │ Load Skills
        ▼
ue-cli help (MCP fallback: ue_context)
        │ Discover exact API
        ▼
ue-cli <capability> / ue-workflow-cli
  (MCP fallback: ue_<domain> / ue_workflow)
        │ Execute through existing safety gates
        ▼
structured result + recipe verify operations
          See Results
```

Skill 不是新的任意执行器。`ue_skills` 不连接 Editor，也没有 `run` action；
Skill 内嵌的指导 recipe 只负责路由。独立 Recipe v2 Runner 只接受有界 poll、
受限条件、审批、补偿和显式数据绑定，不接受任意脚本或无限循环。精确参数始终
优先来自 `ue-cli help`（MCP 回退使用 `ue_context`）。连续 authored 资产编辑经过 Workflow 的
plan digest、事务、readback 和 rollback；材质编辑器预览和实例覆盖使用其专用 batch
契约，不能伪装成未支持的 Workflow scope。

## 客户端入口 Skill

`skills/ue-ai/` 是安装到 Codex 或 Claude Code 的入口 Skill。默认优先使用
`ue-cli` / `ue-workflow-cli` 完成发现、执行和验证：先定位可执行文件并检查版本，
用 `ue-cli skills` 查找领域配方，读取其正文，再用 `ue-cli help` 获取精确 schema。
需要 Editor 时检查 `ue-cli status` 和 live schema。入口 Skill 自身不执行
UE operation，也不复制领域 recipe。

CLI 缺失、不可用、版本不兼容、不支持所需操作或用户明确要求 MCP 时，使用
`ue_status`、`ue_skills`、`ue_context`、领域工具和 `ue_workflow` 回退。
领域 Skill 中已有的 MCP 示例可映射到相同 capability 的 CLI 调用；参数、
审批、requestId、恢复与验收要求保持一致。操作失败或写入结果不明时，不能
通过切换传输直接重发。

它故意不提供 `skill.json`，因此不会出现在 `ue_skills` 的领域 Skill 计数中，
也不会形成入口 Skill 递归加载自身。完成
[CLI 构建和验收](UE_SHORT_CLI.md#构建与分发) 后可显式安装它：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\scripts\install_entry_skill.ps1 -Client codex
```

```bash
bash ./scripts/install_entry_skill.sh --client claude
```

重复安装相同版本是幂等的；不同内容默认拒绝覆盖。先检查并合并本地定制，
显式替换时保留时间戳备份。重新加载客户端技能或开启新会话后读取更新内容。

## 发布 Skill

| Skill ID | 默认用途 | 主要闭环 |
|---|---|---|
| `ue-blueprint-diagnose` | 单 Blueprint 诊断 | scoped scan → graph/call/reference evidence → compile validate → optional runtime correlate |
| `ue-blueprint-buildgraph` | 声明式 Blueprint Graph 构建 | definition → validate/plan → approved Workflow → managed-node/diff/layout evidence |
| `ue-blueprint-graph-organize` | Blueprint Graph 原子排版 | exact geometry → dry-run/digest → Workflow → structural/layout/image evidence |
| `ue-performance-regression` | Before/After 性能门禁 | context → durable runs → poll/result → fingerprint compare → optional Trace |
| `ue-render-debug-capture` | 渲染调试视图、Niagara SimCache、离线渲染故障 | Viewport capture/restore/diff；SimCache capture → inspect/read → JSON export → release；保留日志证据分析 |
| `ue-trace-insights` | Trace 录制与离线分析 | target/channels → bounded capture or import → provider discovery → semantic query/export |
| `ue-umg-authoring` | Widget Blueprint 连续编辑 | hierarchy baseline → short op/Workflow → hierarchy/binding/compile/dirty readback |
| `ue-material-editing` | 材质/函数/Custom HLSL、实例覆盖与大图查询 | 选择资产/预览上下文 → batch/Workflow → 当前诊断、读回与保存证据 |
| `ue-asset-migration` | 资产移动与重构 | dependency audit → plan → exact digest execute → graph/diff readback → optional rollback |
| `ue-world-partition-validate` | 大世界只读验证 | applicability → cells/sources/audit → Data Layer/HLOD/PCG evidence |
| `ue-landscape-authoring` | Landscape/Water 确定性变更 | applicability → export/snapshot → change plan → execute → validate/diff → rollback |
| `ue-recovery-operator` | Editor/Worker/源码控制恢复 | bounded retry → reconnect/checkpoint → explicit restart approval → final acceptance |

每个包自包含：

```text
skills/<skill-id>/
├── SKILL.md
├── skill.json
├── agents/openai.yaml
└── references/
```

- `SKILL.md`：触发条件、决策边界和精简流程；默认按需加载。
- `skill.json`：`ue.agent-skill.v1` 机器索引、recipe phases、capability 引用和
  结果合同。
- `references/`：较长的参数边界、风险解释和验收规则；按需从本地目录读取，
  MCP 回退时通过 `ue_skills read` 单独加载。
- `agents/openai.yaml`：Agent UI 元数据，不参与执行。

## MCP 用法

先搜索摘要，不加载正文：

```json
{
  "action": "list",
  "query": "blueprint",
  "domain": "blueprint"
}
```

通过 `ue_skills` 加载一个 recipe：

```json
{
  "action": "get",
  "skill": "ue-blueprint-diagnose",
  "recipe": "scan-and-verify"
}
```

返回结果包含 `SKILL.md` 正文、机器 recipe，以及为每个 operation 生成的
`ue_context`、live availability、领域工具和验证提示。它不会执行这些提示。
`performInOrder` 保留完整顺序；`seeResults` 只投影 manifest 标记为只读的
verify operation，不会重复暴露 save、rollback 或其他写操作。

随后发现精确 schema：

```json
{
  "operation": "blueprint.scan"
}
```

再通过 `ue_blueprint` 执行，并按 recipe 的 verify phase 调用
`blueprint.asset.validate`、`blueprint.graph.get` 等 readback operation。

大型 reference 仅在需要时读取：

```json
{
  "action": "read",
  "skill": "ue-blueprint-diagnose",
  "reference": "references/diagnosis-recipe.md"
}
```

只允许读取 `skill.json` 已声明且仍位于该 Skill 目录内的文件；绝对路径、
`..` 和符号链接越界被拒绝。

## Level Blueprint 与 Session Recipe

关卡蓝图的多步骤修改必须使用 Workflow v2 的 `levelBlueprint` scope。逻辑目标
是 `ULevelScriptBlueprint`，持久化目标是其 Persistent Level 的 `UWorld/.umap`；
Workflow 只允许 graph/node/pin/comment/layout 变化，检测到 Actor、External
Actor 或其他 package 逃逸时自动失败并回滚。普通 `blueprint` scope 指向 map
会返回 `workflow_scope_kind_mismatch`。

直接 node/pin/comment 请求仍保留兼容入口，但仅提供单请求保护：compile、save
或 read-back 失败时恢复内存图、Dirty 和磁盘 package。多个相关写步骤不要逐条
调用，必须由 Workflow 持有完整事务。

运行态验证使用 `Recipes/ue-pie-subsystem-validation.recipe.json`。它只允许
manifest 明确标记为 `sessionSafe` 的能力，plan digest 绑定 materialized inputs、
能力目录、Editor instance 和 PIE 状态；Runner 自己取得 PIE lease、启动并拥有
generation，拒绝旧 objectRef、迟到回调和跨 Editor 恢复。

## CLI 用法（默认优先）

短 CLI 从本地包加载机器 recipe，不连接 Editor：

```powershell
ue-cli skills --query blueprint
ue-cli skills --name ue-blueprint-diagnose --recipe scan-and-verify --detail full --json
```

`--detail full` 返回机器 manifest，`--recipe` 用于筛选匹配的 Skill；它不返回
`SKILL.md` 正文，也不会执行 recipe。按返回的 `data.skillRoot`，读取
`<skillRoot>/<skill-id>/SKILL.md` 及 manifest 声明的必要 resources，再执行选中的
recipe。无法访问本地文件时，使用 MCP `ue_skills` 的 get/read 加载正文。

再用本地 capability manifest 发现参数：

```powershell
ue-cli help blueprint.scan --json
ue-cli help blueprint.scan --live-schema --json
```

执行和验证仍是普通短操作：

```powershell
ue-cli blueprint.scan --asset /Game/Blueprints/BP_Player --json
ue-cli blueprint.asset.validate --blueprint /Game/Blueprints/BP_Player --json
```

`UE_SKILL_ROOT` 或 `--skill-root` 可覆盖本地 Skill 根目录。普通
`ue-cli <capability>` 不加载 SkillCatalog，因此不会增加短操作冷启动成本。

## `ue.agent-skill.v1` 约束

- `id` 必须与目录和 `SKILL.md` frontmatter name 一致。
- 每个操作型 recipe 必须包含 `discover`、`execute`、`verify` 三个 phase。
- recipe 只能引用 `requirements.capabilities` 或
  `optionalCapabilities` 中声明、且当前 capability manifest 存在的 ID。
- `optionalCapabilities` 表示目标工程可能 unavailable，不表示发布包可缺少
  该 contract。
- resources 必须使用声明的相对路径并位于 Skill 目录内。
- recipe result 必须声明摘要、证据和成功条件。
- `readOnly` recipe 只能引用只读、非破坏 capability；`safeWrite` 不得引用
  `confirmWrite` 或 destructive capability；Skill 总风险由 recipe 风险推导，
  多种风险时必须为 `mixed`。
- `route=workflow` 只能出现在 execute phase，且所有 operation 都必须由
  capability manifest 标记为 `editStep`。
- verify phase 中的写操作必须是 optional，并只在调用方明确请求或配方条件
  成立时执行；See Results 始终保持只读。
- Skill prose 不复制 JSON Schema；参数约束变化只需更新 capability manifest。

合同见 `Resources/Contracts/ue.agent-skill.v1.schema.json`。本地一致性检查：

```powershell
node scripts/validate_capabilities.mjs
node scripts/validate_skills.mjs
```

新增或修改 Skill 时，还应使用 `skill-creator` 的 `quick_validate.py` 检查
frontmatter 和 Agent 元数据，并运行 MCP/CLI tests。

## 与 VibeUE 的取舍

本设计借鉴了“摘要列表、正文懒加载、领域 reference、配方先于 API
discovery”的体验。Python 脚本通过可终止的独立 worker 执行，调用方在请求中
声明修改级别，宿主把脚本摘要、状态和回执写入审计记录；worker 不注入 UE
`unreal` 模块：

- 使用稳定短 ID 和机器合同，不依赖运行时生成路径。
- metadata 包含 UE/capability/plugin/risk/result 依赖并由 CI 校验。
- live discovery 面向受约束 capability，而不是把整个 `unreal.*` 反射面作为
  主入口。
- Skill 不能自动保存 dirty package、绕过确认、替代 Workflow rollback，或把
  node ID 当作完成证据。Python 的修改级别是调用方声明并记录的合同字段，
  需要确认的写入仍由上层审批/工作流负责。
