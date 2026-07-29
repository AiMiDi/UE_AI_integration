# UE Engineering Copilot

`UE Engineering Copilot` 在现有 UE MCP 与 `ue-workflow` 之上补齐工程分析和
生产验证闭环。它不把所有能力塞进 Workflow：单资产或确定的多资产连续编辑
使用 UE Workflow DSL；性能采样、Trace、测试、Cook、Package、BuildGraph
等长任务使用 Durable Job；查询和交互式调试继续使用单次 capability。

## CLI 与 MCP 的能力关系

- MCP 的十一个稳定工具负责发现、查询、执行和 Workflow 入口。
- `ue <capability>` 在线获取精确 schema 后通过同一 `/api/execute` 合同调用
  Editor，因此短操作 CLI 的能力范围与领域 MCP 相同；它不经过 Workflow DSL。
- `ue-workflow validate` 与非执行型 Core plan 可离线运行；用于审批执行的计划
  必须由 `plan --connect` 绑定 Editor 资产基线。`execute`、`ue` 单次 capability
  和 Job 都要求连接 Editor。
- UE Workflow 只接纳 manifest 中声明为 `editStep` 的确定资产连续编辑；v1
  为单 scope，v2 最多为 16 个具名 scope。
  Trace、性能、测试、Blueprint 分析、Cook/Package、Source Control、
  DDC、BuildGraph、HLOD 和 PCG 均不会被伪装成 Workflow。

## 1. Trace 与窗口性能证据

- `production.trace.start/status/stop/analyze`
- `production.performance.run/result.get/compare`
- `performance.run` 支持 `mode=window|scenario`、warmup、采样窗口、
  `repeatCount`、帧预算和可选自动 Trace。
- Scenario 可用 `metrics.begin/metrics.end` 标记测量区间；未标记的准备和清理
  步骤不计入性能统计。
- 结果包含环境指纹、各次重复采样、p50/p95/p99、峰值、超预算帧，以及
  Game/Render/RHI/GPU 聚合；Scenario 日志从启动 cursor 起读取，不混入 Editor
  历史日志。
- Trace 文件作为 `ue.artifact.v1` 分块读取；CLI 可用 `--output` 原子导出。

`trace.analyze` 使用 UE 5.3 TraceServices 读取 Frame、Timing、Counter 等
provider，返回受约束的 CPU scope、Counter、Game/Render/RHI/GPU 聚合和 artifact
引用。它不返回原始事件洪流，也不宣称代替 Unreal Insights 的完整交互式
Timing、Memory、Network 或 Asset Loading 分析界面。

`performance.compare` 可同时检查多个指标阈值；环境指纹不兼容时返回
`inconclusive`，证据兼容时返回结构化 `pass` 或 `regression`。

## 2. 自动化测试

- `production.test.list/run/result.get`
- 统一 Automation、Functional Test 与项目自定义 Gauntlet 入口。
- 测试执行使用 `ue.job.v1`，结果和日志可在 Editor 重启后查询。
- Gauntlet 只运行项目明确提供的测试；系统不会生成或猜测测试节点。

## 3. Blueprint 分析

- `blueprint.scan`
- `blueprint.call_graph.get`
- `blueprint.findings.correlate`

Finding 使用 `ue.finding.v1`，包含稳定 ID、严重度、证据位置、置信度和运行时
关联状态。`blueprint.findings.correlate` 优先使用真实
`debugSessionId/traceRange`，也保留 `observedNodeIds` 作为明确的手工证据输入。

## 4. Blueprint 运行时调试

- `blueprint.debug.session.get`
- `blueprint.debug.trace.get`
- `blueprint.debug.breakpoint.list/set/remove`
- `blueprint.debug.watch.list/set/remove/value.get`
- `blueprint.debug.control`

调试引用绑定当前 PIE 的 `sessionId + generation`；PIE 重启后旧引用返回
`stale_session_handle`。Trace 使用 cursor 分页，`debug.control` 支持
`continue/stepInto/stepOver/stepOut/abort` 并立即返回 `commandId + accepted`。

蓝图暂停时 Game Thread 处在 Kismet/Slate 内部循环，普通 Tick 队列无法处理
continue。UE 5.3 的 HttpServer ticker 同样位于 Game Thread，因此服务在 Slate
pre-tick 中先发布不可变暂停快照，再主动泵送同一个 listener。暂停路由只读取
快照或写入 POD 命令队列，不访问 UObject；非调试与 Workflow 请求立即返回
`423 debug_session_paused`，控制命令随后在同一次 pre-tick 中消费。调用方通过
`session.get` 的 `recentCommands` 和状态快照确认最终结果。

这些能力为 `interactiveOnly`，不能加入 Workflow DSL。

## 5. Runtime 输入、等待与可信视觉证据

- `scene.runtime.input.pointer_sequence/get`
- `scene.runtime.wait.until/get`
- `scene.runtime.viewport.capture`
- `content.widget.event.ensure_handler`

Pointer Sequence 由 Editor Tick 驱动，支持多点轨迹、
`screenAbsolute/window/widgetLocal/widgetNormalized/viewportNormalized`
坐标空间，以及 `requireTargetHit`。每步证据包含解析后位置、Hit Path、handled
Widget、焦点和 Capture owner；失败、取消或 PIE 结束都会释放鼠标按钮、键盘键
和 Capture。

Runtime Wait 用 Job ID 等待 PIE Ready、Widget 出现、属性值、Session generation
或有界日志谓词，不用固定 Sleep 伪装 GPU readback 等异步完成。

可信截图只接受当前 `sessionId + generation` 对应的真实 PIE `SViewport`，返回
原生窗口句柄、Viewport Rect、Capture Source、像素尺寸与 raw-pixel SHA-256。
无法取得目标窗口（包括 NullRHI）时返回失败，不会回退到活动窗口或桌面截图。

UMG `ensure_handler` 使用 Delegate 的真实 UFunction 签名创建或修复 Function
Graph、Component-Bound Event、调用执行边和动态 Binding；最终编译后同时验证
GeneratedClass 函数、签名与生成绑定。重复执行保持幂等。

## 6. 通用资产与 Mesh/Texture 审计

- `content.asset.search/get/dependencies/referencers/audit`
- `content.static_mesh.inspect`
- `content.texture.inspect`
- `content.asset.change.plan/execute/rollback`

变更限制在 `/Game`，批量最多 100 项。所有写入都要求精确 plan digest、
`confirmWrite=true` 和顶层 `requestId`；Delete 有 Referencer 门禁。无法可靠
逆转的 Reimport 与 Fix Redirectors 会明确返回 `rollbackAvailable=false`。

## 7. World Partition、Data Layer、HLOD、PCG

- World Partition 状态、Cell、Streaming Source 和审计。
- Data Layer 列表与详情。
- HLOD 审计与受约束构建任务。
- PCG 检查，以及显式确认后的 Generate/Cleanup。
- World Partition 设置变更采用 plan/execute/rollback。

HLOD 是长任务，不进入资产 Workflow；PCG 只在可用插件/模块满足 manifest
`requires` 时暴露为可执行能力。PCG Generate/Cleanup 使用同一 Editor 实例内的
`requestId` 回执做幂等重放；其异步图执行仍由 PCG 自身调度，不伪报为已经完成。

## 8. 渲染配置与诊断

- `scene.render.context.get`
- `scene.render.feature.audit`
- `scene.render.memory.sample`
- `scene.render.settings.plan/execute/rollback`

写入仅覆盖 allowlist 内的 Session CVar，带精确 digest 和读回；不会静默修改
项目 DefaultEngine.ini 或平台配置。

## 9. Animation 与 AI 只读闭环

- Animation Blueprint、状态机与 BlendSpace 的 list/get/validate/diff。
- Behavior Tree 与 Blackboard 的 list/get/references/validate/diff。

这些能力补齐已有创建型操作的检查闭环，但不宣称覆盖 Control Rig、IK Rig、
StateTree、Mass、EQS 或运行时 AI Debugger。

## 10. Cook、Package 与生产 Job

- `production.job.status/cancel/result.get/log.get/artifact.get`
- 现有 Cook、Package 与 Commandlet 已迁移到 Durable Job。
- Job journal 位于项目 `Saved/UEAIIntegration/Jobs/`；Editor 重启后未完成任务
  会明确变为 `interrupted`，而不是伪造继续运行。
- 日志和 artifact 均为游标/分块读取，避免一次返回淹没 MCP 上下文。
- Package 输出限制在项目 `Saved/UEAIIntegration/Packages/`；Commandlet 仅允许
  `FixupRedirects`、`ResavePackages` 和 `WorldPartitionBuilderCommandlet`，
  新调用要求顶层 `requestId` 与显式 `confirmWrite=true`。为保持既有
  `/api/execute` 客户端兼容，仅无 `requestId` 的旧调用可省略确认。
- 每个 Job 最多同步读取并哈希 64 MiB 的 artifact；超出单文件或总预算时返回
  `sha256Deferred=true`，避免在 Game Thread 连续读取大型 Package 产物。
- Runtime 在每个 chunk 前后校验登记时的文件大小与修改时间；CLI 还会校验
  offset、总大小和已有 receipt SHA-256。Deferred artifact 导出完成后由 CLI
  计算并返回本地 SHA-256，供后续持久化或外部验签。

## 11. Source Control、DDC、BuildGraph 与 Horde

- Source Control repository/status/diff，以及 change plan/execute。
- DDC 状态与受约束清理/填充 Job。
- BuildGraph XML/Target/标量属性校验和本地 Durable Job。
- Horde 只提供本地配置与可用性上下文。v1 不提交 Horde Job，也不读取或返回
  凭据；结果会明确报告 `submissionSupported=false`。

## 公共安全合同

- `ue.job.v1`：长任务状态、进度、诊断和 artifact 引用。
- `ue.artifact.v1`：有界 artifact 元数据和分块内容。
- `ue.finding.v1`：静态/运行时工程发现。
- `ue.change-plan.v1`：可审批写入计划和稳定 digest。
- `/api/execute.requestId` 是新增幂等写入/Job 的权威来源；Executor 会要求顶层
  值并注入参数，重复值冲突会被拒绝。既有
  `production.project.cook/package/commandlet.run` 保留无 `requestId` 兼容入口；
  一旦提供 `requestId`，仍以顶层 envelope 为准。

能力发现会根据 manifest 的 `requires.plugins/modules/platforms/engine` 返回
`available` 与结构化原因。默认目录是动态的，不再依赖每个领域的硬编码数量。
