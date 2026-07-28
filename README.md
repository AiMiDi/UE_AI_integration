# UE_AI_integration

[简体中文](README.md) | [English](README_EN.md)

`UE_AI_integration` 是一个面向 Unreal Editor 的 MCP 集成插件。它通过单个
Editor Module 和 TypeScript stdio bridge，让 Codex CLI、Claude Code 等
MCP 客户端查询或修改 Blueprint、场景、内容资产、动画、AI 与生产流程。

当前插件版本为 `0.3.1`，以 Unreal Engine 5.3 为实际构建基线；UE
5.4–5.7 的差异集中在兼容层，但尚未全部完成本地编译验证。

## 核心特性

- 212 项 manifest 驱动的 Editor 与 PIE Runtime 能力。
- 十个稳定的 MCP 工具，不把 212 项能力直接展开成工具列表。
- 六个领域路由：Blueprint、Scene、Content、Animation、AI、Production。
- 专用 PIE 生命周期、Runtime 对象/Widget/Delegate/真实输入与 Scenario 能力。
- Blueprint/UMG 写入返回编译、保存、重载和读回验证证据。
- 统一的 HTTP envelope、状态码和参数错误模型。
- 查询、命令与校验分层，所有 UObject 操作进入 Game Thread 队列。
- MCP 只连接已经运行的 Unreal Editor，不负责启动或关闭 Editor。
- 默认监听 `127.0.0.1:9847`，客户端可通过 `UE_PORT` 覆盖端口。
- [UE Workflow DSL/CLI](docs/UE_WORKFLOW_DSL.md) 将单资产连续编辑合并为一次
  可规划、可审批、可回滚的执行；调试和长任务不进入 Workflow。
- Workflow 默认返回压缩摘要；完整 ReadBack、Diff 与结构快照按 section 获取。
- capability 目录支持搜索、trait 过滤与分页，默认最多返回 25 项摘要。

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
| Blueprint | 58 | 资产生命周期、Graph、变量、组件、接口、Discovery、Diff、Validation |
| Scene | 54 | Actor、PIE Runtime、Widget/Delegate/Input、Viewport、WorldGen、Foliage、Navigation |
| Content | 59 | Material、DataTable、UserTypes、Niagara、UMG Authoring/Animation |
| Animation | 10 | AnimBlueprint、State、Transition、BlendSpace |
| AI | 9 | Behavior Tree、Blackboard |
| Production | 22 | Sequencer、Scenario、模块溯源、Build、Cook、Package |

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

bridge 始终注册以下十个工具，即使 Unreal Editor 暂时离线：

- `ue_status`
- `ue_capabilities`
- `ue_context`
- `ue_blueprint`
- `ue_scene`
- `ue_content`
- `ue_animation`
- `ue_ai`
- `ue_production`
- `ue_workflow`

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

Editor 插件保留三条原有路由，并新增两条 Workflow 路由：

```text
GET  /api/health
GET  /api/capabilities?query=<text>&domain=<domain>&offset=0&limit=25
POST /api/execute
GET  /api/v1/workflow/handshake
POST /api/v1/workflow
```

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
