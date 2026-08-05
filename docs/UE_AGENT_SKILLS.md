# UE Agent Skills 与 Capability Recipes

UE Agent Skill 层把领域知识、调用顺序、风险边界和验收证据包装成可按需加载的
配方，同时保留 manifest capability、UE Workflow 和 Editor handler 的唯一
执行权威。

它实现四段闭环：

```text
ue_skills / ue skills
        │ Load Skills
        ▼
ue_context / ue help
        │ Discover exact API
        ▼
ue_<domain> / ue_workflow / ue <capability>
        │ Execute through existing safety gates
        ▼
structured result + recipe verify operations
          See Results
```

Skill 不是新的任意编排器。`ue_skills` 不连接 Editor，也没有 `run` action；
recipe 不包含循环、条件、脚本、参数 schema 或数据绑定。精确参数始终来自
`ue_context` 或 `ue help`，写入仍经过 capability 的 `confirmWrite`、
Workflow 的 plan digest、事务、readback 和 rollback。

## 客户端入口 Skill

`skills/ue-ai/` 是安装到 Codex 或 Claude Code 的入口 Skill。它先检查
`ue_status` / `ue_cli`，再通过 `ue_skills` 加载最匹配的领域 Skill，并用
`ue_context` 发现精确 schema。入口 Skill 自身不执行 UE operation，也不复制
领域 recipe。

它故意不提供 `skill.json`，因此不会出现在 `ue_skills` 的领域 Skill 计数中，
也不会形成入口 Skill 递归加载自身。发布包安装 MCP 时可显式安装它：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\scripts\install_entry_skill.ps1 -Client codex
```

```bash
bash ./scripts/install_entry_skill.sh --client claude
```

重复安装相同版本是幂等的；不同内容默认拒绝覆盖，显式升级会先保留时间戳备份。

## 发布 Skill

| Skill ID | 默认用途 | 主要闭环 |
|---|---|---|
| `ue-blueprint-diagnose` | 单 Blueprint 诊断 | scoped scan → graph/call/reference evidence → compile validate → optional runtime correlate |
| `ue-blueprint-buildgraph` | 声明式 Blueprint Graph 构建 | definition → validate/plan → approved Workflow → managed-node/diff/layout evidence |
| `ue-blueprint-graph-organize` | Blueprint Graph 原子排版 | exact geometry → dry-run/digest → Workflow → structural/layout/image evidence |
| `ue-performance-regression` | Before/After 性能门禁 | context → durable runs → poll/result → fingerprint compare → optional Trace |
| `ue-render-debug-capture` | Editor/PIE 渲染调试视图 | live availability → exact Viewport PNG → restore → bounded analysis/compatible diff |
| `ue-trace-insights` | Trace 录制与离线分析 | target/channels → bounded capture or import → provider discovery → semantic query/export |
| `ue-umg-authoring` | Widget Blueprint 连续编辑 | hierarchy baseline → short op/Workflow → hierarchy/binding/compile/dirty readback |
| `ue-asset-migration` | 资产移动与重构 | dependency audit → plan → exact digest execute → graph/diff readback → optional rollback |
| `ue-world-partition-validate` | 大世界只读验证 | applicability → cells/sources/audit → Data Layer/HLOD/PCG evidence |
| `ue-landscape-authoring` | Landscape/Water 确定性变更 | applicability → export/snapshot → change plan → execute → validate/diff → rollback |

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
- `references/`：较长的参数边界、风险解释和验收规则；通过 `ue_skills read`
  单独加载。
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

## CLI 用法

短 CLI 从本地包加载机器 recipe，不连接 Editor：

```powershell
ue skills --query blueprint
ue skills --name ue-blueprint-diagnose --recipe scan-and-verify --detail full --json
```

再用本地 capability manifest 发现参数：

```powershell
ue help blueprint.scan --json
ue help blueprint.scan --live-schema --json
```

执行和验证仍是普通短操作：

```powershell
ue blueprint.scan --asset /Game/Blueprints/BP_Player --json
ue blueprint.asset.validate --blueprint /Game/Blueprints/BP_Player --json
```

`UE_SKILL_ROOT` 或 `--skill-root` 可覆盖本地 Skill 根目录。普通
`ue <capability>` 不加载 SkillCatalog，因此不会增加短操作冷启动成本。

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
discovery”的体验，但不复制其任意 Python 执行模型：

- 使用稳定短 ID 和机器合同，不依赖运行时生成路径。
- metadata 包含 UE/capability/plugin/risk/result 依赖并由 CI 校验。
- live discovery 面向受约束 capability，而不是把整个 `unreal.*` 反射面作为
  主入口。
- Skill 不能自动保存 dirty package、绕过确认、替代 Workflow rollback，或把
  node ID 当作完成证据。
