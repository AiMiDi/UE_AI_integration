# UE_AI_integration

[简体中文](README.md) | [English](README_EN.md)

`UE_AI_integration` 是一个面向 Unreal Editor 的 MCP 集成插件。它通过单个
Editor Module 和 TypeScript stdio bridge，让 Codex CLI、Claude Code 等
MCP 客户端查询或修改 Blueprint、场景、内容资产、动画、AI 与生产流程。

当前插件版本为 `0.7.0`，以 Unreal Engine 5.3 为实际构建基线；UE
5.4–5.7 的差异集中在兼容层，但尚未全部完成本地编译验证。

## 核心特性

- 当前发布快照包含 338 项 manifest 驱动的 Editor 与 PIE Runtime 能力；
  服务启动时从 manifest 动态计算数量。
- 十二个稳定的 MCP 工具，不把全部能力直接展开成工具列表。
- 六个领域路由：Blueprint、Scene、Content、Animation、AI、Production。
- 专用 PIE 生命周期、Runtime 对象/Widget/Delegate/真实输入与 Scenario 能力。
- Blueprint/UMG 写入返回编译、保存、重载和读回验证证据。
- 统一的 HTTP envelope、状态码和参数错误模型。
- 查询、命令与校验分层，所有 UObject 操作进入 Game Thread 队列。
- MCP 只连接已经运行的 Unreal Editor，不负责启动或关闭 Editor。
- 默认监听 `127.0.0.1:9847`；启动 Editor、CLI 与 MCP 时可通过同一个
  `UE_PORT` 覆盖端口。
- [UE Workflow DSL/CLI](docs/UE_WORKFLOW_DSL.md) 的 v1 保留单资产连续编辑，
  v2 支持最多 16 个具名资产 scope、确定 DAG、持久 Journal、重启恢复和全量回滚；
  调试和长任务不进入 Workflow。
- [UE Engineering Copilot](docs/UE_ENGINEERING_COPILOT.md) 提供性能/Trace、
  自动化测试、Blueprint 分析、资产审计、大世界/渲染诊断以及生产任务闭环。
- 性能 Job 支持窗口或 Scenario 重复采样、测量区间、TraceServices 聚合、
  环境指纹和结构化回归判定。
- Blueprint 调试支持 PIE 会话、Trace、断点、Watch 与
  continue/step/abort；暂停期间由 Slate pre-tick 先发布不可变快照，再主动泵送
  同一 HTTP listener，并只消费 POD 控制命令。
- Runtime Evidence 支持多点 Pointer Sequence、坐标空间与目标命中校验、
  Tick 驱动的条件等待，以及严格绑定 `sessionId + generation` 的 PIE Viewport
  截图；无法解析目标 PIE 窗口时会失败，不会退化为桌面截图。
- `content.widget.event.ensure_handler` 可按 Delegate 精确签名创建或修复 UMG
  Handler、Event Node、调用边和动态绑定，并在编译后验证。
- Workflow 默认返回压缩摘要；完整 ReadBack、Diff 与结构快照按 section 获取。
- capability 目录支持搜索、trait 过滤与分页，默认最多返回 25 项摘要。
- 0.6.1 补齐 Blueprint Graph 的可信排版闭环：跨端模糊搜索与 Windows Unicode、
  节点/Comment 几何、布局诊断、原子化 dry-run/审批排版，以及可信 Graph
  截图和视觉 Diff。详见 [Graph 可信闭环](docs/UE_BLUEPRINT_GRAPH.md)。
- 0.6.2 增加性能诊断和自包含 HTML 证据报告；受 VSync/FPS cap 限制时会返回
  `frameLimited` 或 `inconclusive`，不会伪造 CPU/GPU 瓶颈结论。
- 0.7.0 增加确定性的 Landscape/Water Capability Pack：高度/权重导出、
  有限的导入与结构化变更计划、恢复 artifact 和同一 Editor 会话 rollback；
  不包含任意笔刷式 Sculpt/Paint。详见 [Landscape / Water](docs/UE_LANDSCAPE_WATER.md)。
- [UE 短操作 CLI](docs/UE_SHORT_CLI.md) 以 manifest capability ID 作为首参数，
  默认用本地 schema 单次调用 `/api/execute`，`--live-schema` 可强制在线校验，
  `ue shell` 可复用目录与连接；`ue-workflow` 只保留 DSL。
- [UE Agent Skills](docs/UE_AGENT_SKILLS.md) 提供七个已验证领域 Skill 和
  capability recipe，形成 Load → Discover → Execute → See Results 闭环，
  但不新增任意脚本执行器。

## 架构

```text
MCP client
    │ stdio
    ▼
TypeScript MCP bridge
    │ HTTP :9847 (/api)
    ▼
UE_AI_integration Editor Module
    ├── Core            manifest、registry、validation、executor
    ├── Transport       HTTP envelope、状态码、Game Thread 队列
    ├── Domains         Blueprint、Scene、Content、Animation、AI、Production
    └── Infrastructure  资产解析、序列化、保存、编译、快照、PIE 生命周期、UE 兼容层
```

`Resources/Capabilities/*.json` 是 C++ 插件与 TypeScript bridge 共享的能力
元数据源。MCP 路由只读取 manifest，不根据工具名称做正则分类。

| Domain | 数量 | 能力范围 |
|---|---:|---|
| Blueprint | 82 | 资产生命周期、Graph 几何/排版/截图、变量、组件、调用图、规则扫描、运行时调试、Diff、Validation |
| Scene | 93 | Actor、PIE Runtime、可信输入/等待/截图、World Partition、Data Layer、HLOD、PCG、渲染诊断、Landscape/Water |
| Content | 78 | 资产查询/依赖/审计、安全导入/重导入、Static Mesh/Texture 配置、Material、Niagara、UMG 与事件 Handler 验证 |
| Animation | 19 | AnimBlueprint、状态机与 BlendSpace 的创建、读取、校验和 Diff |
| AI | 17 | Behavior Tree 与 Blackboard 的创建、读取、引用、校验和 Diff |
| Production | 49 | Durable Job、Trace、性能诊断/HTML 报告、测试、Cook/Package、Source Control、DDC、BuildGraph |

## 环境要求

- Unreal Engine 5.3–5.7
- Node.js 20 或更高版本
- C++ Unreal 项目，或与目标引擎匹配的预编译插件
- 支持 MCP stdio server 的客户端

## 安装 UE 插件

将仓库放到工程插件目录：

```text
YourProject/
└── Plugins/
    └── UE_AI_integration/
        ├── UE_AI_integration.uplugin
        ├── Source/
        ├── Resources/
        └── MCP/
```

构建 TypeScript bridge：

```powershell
cd YourProject\Plugins\UE_AI_integration\MCP
npm ci
npm run build
npm test
```

启动 Unreal Editor 并确认插件已启用。Editor 侧服务启动后，可检查：

```powershell
Invoke-RestMethod http://127.0.0.1:9847/api/health
```

Level Editor 右下角会显示 `UE AI · N` 状态入口。绿色表示服务可用，
黄色表示 manifest/Handler 绑定降级，红色表示监听失败，`Off` 表示用户已禁用
本地 HTTP 服务。点击后打开 UE 原生快捷菜单，可在本地启停服务，并查看：

- 通过 5 秒心跳注册的 MCP 调用方；15 秒无心跳后自动离线。
- 一次性 `ue` / `ue-workflow` CLI 调用，按 `invocationId` 归属但不计入在线连接数。
- Capability、Workflow Run 与 Durable Job 的状态、耗时、错误码和
  `requestId/runId/jobId`；不会保存请求参数、响应正文或图片数据。

启用状态和端口保存在项目用户级 Editor 配置中；`UE_PORT` 仍具有最高优先级。
禁用服务会立即清空在线调用方，但保留本次 Editor 会话的元数据执行历史。

不要覆盖正在加载的插件 DLL。替换已有安装时，应先关闭 Unreal Editor，
替换插件目录，再重新启动 Editor。

## 配置 Codex CLI

在 `~/.codex/config.toml` 中加入：

```toml
[mcp_servers.ue_ai_integration]
command = 'C:\Program Files\nodejs\node.exe'
args = ['D:\Path\To\YourProject\Plugins\UE_AI_integration\MCP\dist\index.js']

[mcp_servers.ue_ai_integration.env]
UE_PORT = "9847"
```

重新启动 Codex CLI 后，可以用以下命令检查配置：

```powershell
codex mcp get ue_ai_integration
```

Claude Code 可使用：

```powershell
claude mcp add ue_ai_integration -- node Plugins\UE_AI_integration\MCP\dist\index.js
```

## MCP 工具

bridge 始终注册以下十二个工具，即使 Unreal Editor 暂时离线：

- `ue_status`
- `ue_capabilities`
- `ue_context`
- `ue_skills`
- `ue_cli`
- `ue_blueprint`
- `ue_scene`
- `ue_content`
- `ue_animation`
- `ue_ai`
- `ue_production`
- `ue_workflow`

`ue_cli` 不连接 Editor。它分别按 `UE_CLI` / `UE_WORKFLOW_CLI`、插件包内
`CLI/bin/ue.exe` / `ue-workflow.exe`、`PATH` 和开发构建目录定位两套 CLI。
原有 Workflow locator 字段保持不变，并新增 `shortCli`。
`scripts/build_plugin.bat` 会将 CLI 及其 Contracts/Capabilities 安装到插件包
的 `CLI/` 目录，同时恢复 MCP 的生产依赖。

六个领域工具统一接收：

```json
{
  "operation": "scene.actor.spawn",
  "requestId": "client-generated-uuid",
  "params": {
    "type": "PointLight",
    "name": "MCP_Light",
    "location": [0, 0, 300]
  }
}
```

先用 `ue_capabilities` 搜索分页摘要，或用 `ue_context` 按域读取完整 schema。
精确指定 `operation` 会返回单项完整描述符。领域工具会拒绝调用其他领域的
operation。

`ue_skills` 离线搜索、加载和读取发布包内的 Agent Skill/recipe；它不连接
Editor，也不执行 operation。加载 recipe 后仍须用 `ue_context` 发现精确
schema，再由领域工具或 `ue_workflow` 执行，最后调用 recipe 的 verify
operation 读回结果。

短操作 CLI 与六个领域 MCP 工具使用同一份 capability contract：

```powershell
ue blueprint.asset.get --name /Game/UI/WBP_Login
ue scene.actor.spawn --type PointLight --name KeyLight --location '[0,0,300]'
ue production.job.status --job-id job-123
ue skills --query blueprint
ue shell
```

`ue` 不加载 WorkflowCore。默认从随程序分发的 manifest 做参数映射，只发送
一次 `/api/execute`，由 Editor 做最终校验；`--live-schema` 会先向当前
Editor 获取单项完整 schema，适合诊断 contract 漂移或强制校验 availability。
`ue shell` 启动后加载一次目录并复用 HTTP keep-alive 连接，普通调用仍立即
退出。长任务只启动 Job 并立即返回 `jobId`，不会自动等待；复杂连续资产编辑
仍使用 `ue-workflow`。

Workflow CLI 使用离线/在线目录查询合同：

```powershell
ue-workflow capabilities --query debug --domain blueprint --risk interactive --limit 10
ue-workflow capabilities --connect --available-only --domain scene --limit 25
ue-workflow capabilities --connect --operation blueprint.debug.session.get --detail full
```

未加 `--connect` 时直接读取随 CLI 分发的 manifest；加上后把过滤、risk、
availability、分页和详情参数透传给当前 Editor。`--available-only` 需要在线
查询，因为离线目录无法判定目标工程的插件与模块可用性。

### PIE 生命周期

通过 `ue_scene` 调用 PIE 生命周期能力：

```json
{
  "operation": "scene.pie.restart",
  "params": {}
}
```

| Operation | 行为 |
|---|---|
| `scene.pie.start` | 在活动关卡视口请求启动 PIE；若 PIE 已启动或正在启动，则幂等返回当前状态 |
| `scene.pie.stop` | 请求停止活动 PIE；若启动仍在队列中，则取消该启动请求 |
| `scene.pie.restart` | 先请求退出当前 PIE，确认旧 Play World 完全销毁后，在后续 Editor Tick 中重新启动 |

响应数据包含 `action`、`requested`、`state`、`sessionId` 和 `generation`。
PIE 重启后旧 Runtime object handle 会返回 `stale_session_handle`。

## HTTP API

Editor 插件保留三条原有路由、两条 Workflow 路由，并提供三条供 MCP bridge
与 CLI 维护诊断会话的客户端路由：

```text
GET  /api/health
GET  /api/capabilities?query=<text>&domain=<domain>&offset=0&limit=25
POST /api/execute
GET  /api/v1/workflow/handshake
POST /api/v1/workflow
POST /api/v1/clients/register
POST /api/v1/clients/heartbeat
POST /api/v1/clients/unregister
```

MCP bridge 在初始化后以随机 `instanceId` 注册，后续请求携带
`X-UEAI-Session-Id` 并维持心跳。短 CLI 为每个进程生成一个
`invocationId`，最佳努力注册一次会话，并让同一命令中的 plan/execute 或
shell 请求复用该会话；旧 Editor 不支持会话路由时自动退回 Legacy HTTP。
这些字段仅用于状态 UI、执行归属和诊断，不是权限身份，也不替代未来的鉴权。

执行请求：

```json
{
  "capability": "blueprint.asset.list",
  "requestId": "client-generated-uuid",
  "params": {
    "filter": "Player"
  }
}
```

成功响应：

```json
{
  "ok": true,
  "data": {}
}
```

错误响应：

```json
{
  "ok": false,
  "error": {
    "code": "invalid_params",
    "message": "..."
  }
}
```

状态码约定：

| HTTP | 含义 |
|---:|---|
| 400 | JSON 无效 |
| 404 | capability 不存在 |
| 409 | `requestId` 与首次 payload 冲突，或异步任务冲突 |
| 410 | PIE 会话句柄已经失效 |
| 422 | 参数或领域不匹配 |
| 500 | Editor 执行失败 |
| 503 | Editor 服务暂不可用 |

Registry 启动时会验证 manifest 与 C++ Handler 是否一一对应。缺失、重复或
跨域绑定会让服务进入 `degraded` 状态：健康检查和能力查询仍然可用，但执行
被禁用。

`/api/capabilities` 默认返回不含 `inputSchema` 的摘要页，并包含
`total/offset/limit/hasMore`。可按 `kind`、`readOnly`、`destructive`、
`expensive`、`outputKind` 过滤；使用 `detail=full` 获取完整描述符，或用
`operation=<dotted-id>` 精确获取单项完整 schema。`limit` 最大为 100。

## 开发与验证

新增或迁移能力时：

1. 在对应领域 manifest 中声明 descriptor。
2. 在 `Private/Domains/<Domain>/<Kind>/` 实现相同点分 ID 的 Handler。
3. 从领域 registrar 注册 Handler。
4. 将复用逻辑下沉到 `Private/Infrastructure/`。
5. 将 UE 版本差异限制在 `Private/Infrastructure/Compatibility/`。

运行静态与 TypeScript 验证：

```powershell
node scripts\validate_capabilities.mjs
cd MCP
npm ci
npm run build
npm test
npm audit --omit=dev
```

使用 UE 5.3 构建独立插件：

```powershell
scripts\build_plugin.bat "D:\code\D5\d5render-ue5_3" "..\UE_AI_integration-BuiltPlugin\UE5.3"
```

Linux 或 macOS 在对应宿主机上构建插件和原生 CLI：

```bash
bash scripts/build_plugin.sh /path/to/UnrealEngine ../UE_AI_integration-BuiltPlugin
```

`BuildPlugin` 包保留根 `CMakeLists.txt`、`CLI` 与 `Workflow` 源码。
`CLI/bin` 中的预编译程序只适用于执行打包脚本的宿主平台；把包复制到另一平台后，
可在包根目录重新执行 `cmake -S .` 构建本机 `ue` 和 `ue-workflow`。

有运行中的 Editor 时，可继续执行只读和可回滚的 smoke test：

```powershell
python tests\test_health.py
python tests\test_tools.py
```

## Credits

项目参考了以下开源实现与设计：

- [BlueprintMCP](https://github.com/mirno-ehf/ue5-mcp)
- [UnrealClaude](https://github.com/Natfii/UnrealClaude)
- [unreal-engine-mcp](https://github.com/flopperam/unreal-engine-mcp)

本项目使用 MIT License。
