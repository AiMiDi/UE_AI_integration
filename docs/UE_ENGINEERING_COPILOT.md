# UE Engineering Copilot

`UE Engineering Copilot` 在现有 UE MCP 与 `ue-workflow` 之上补齐工程分析和
生产验证闭环。它不把所有能力塞进 Workflow：单资产或确定的多资产连续编辑
使用 UE Workflow DSL；性能采样、Trace、测试、Cook、Package、BuildGraph
等长任务使用 Durable Job；查询和交互式调试继续使用单次 capability。

## CLI 与 MCP 的能力关系

- MCP 的十二个稳定工具负责 Skill 加载、发现、查询、执行和 Workflow 入口。
- `ue <capability>` 在线获取精确 schema 后通过同一 `/api/execute` 合同调用
  Editor，因此短操作 CLI 的能力范围与领域 MCP 相同；它不经过 Workflow DSL。
- `ue-workflow validate` 与非执行型 Core plan 可离线运行；用于审批执行的计划
  必须由 `plan --connect` 绑定 Editor 资产基线。Workflow、普通 `ue` capability
  和 Editor Job 要求连接 Editor；manifest 声明 `localTrace` 的 Trace
  import/query/export/open 可由 `UEAITraceWorker` 在 Editor 关闭时执行。
- UE Workflow 只接纳 manifest 中声明为 `editStep` 的确定资产连续编辑；v1
  为单 scope，v2 最多为 16 个具名 scope。
  Trace、性能、测试、Blueprint 分析、Cook/Package、Source Control、
  DDC、BuildGraph、HLOD 和 PCG 均不会被伪装成 Workflow。

## 1. Trace 与窗口性能证据

- `production.trace.start/status/stop/analyze`
- `production.trace.target.list/channel.list/launch.plan/import/provider.list`
- `production.trace.*.query/export/open_in_insights`
- `production.performance.run/result.get/compare/diagnose/report.generate`
- `performance.run` 支持 `mode=window|scenario`、warmup、采样窗口、
  `repeatCount`、帧预算和可选自动 Trace。
- `profile=standardScenario` 提供标准回归模板：直接调用 `performance.run` 时
  要求当前 Editor 已打开指定且已保存的 `/Game` 地图；
  `performance.suite.run` 会在确认没有 Dirty Package 后打开定义中的固定地图，
  并在套件终态恢复原地图和 CVar，任何恢复失败都会使套件失败。PIE 后定位 Camera Actor 并在 PIE World 写入固定
  Location/Rotation，按固定顺序执行输入步骤，在
  `metrics.begin/end` 之间采样并清理 PIE。模板默认至少重复 5 次；普通
  `window`/自定义 `scenario` 的重复次数和行为保持不变。
- Scenario 可用 `metrics.begin/metrics.end` 标记测量区间；未标记的准备和清理
  步骤不计入性能统计。
- 结果包含 Git revision、地图 package/hash、Scalability、关键 CVar、
  VSync/FPS cap、ScreenPercentage、GPU/驱动版本等环境指纹，以及各次重复
  采样、p50/p95/p99、峰值、超预算帧与
  Game/Render/RHI/GPU 聚合；Scenario 日志从启动 cursor 起读取，不混入 Editor
  历史日志。
- Trace 文件作为 `ue.artifact.v1` 分块读取；CLI 可用 `--output` 原子导出。

`performance.run` 可选择 `executionTarget=pie|standalone`。Standalone 是一个
受 Durable Job 管理的独立 Editor/Game 进程：为保证真实渲染路径，子进程使用
可见窗口而不是隐藏窗口。每次 repeat 都由子进程用实际 wall-clock 分别计时
warmup 和 sample，并生成独立 CSV；命名 Camera 必须在子 World 中解析成功，
Object Name 会优先精确匹配，使用 Actor Label 时必须唯一；其声明的 Transform
和 Player ViewTarget 会在整个采样期间持续锁定。`startupTimeoutSeconds`
经 Durable Job 统一解析并传给子控制器，范围为 1–3600 秒，默认 300 秒。子进程同时
产出独立日志、可选 Trace、Camera 验证结果和实际 RHI/GPU/驱动/分辨率/CVar
runtime fingerprint；父 Editor 只补充项目、地图和 Profile 等稳定字段，不用父
进程渲染环境冒充子进程证据。PIE 保留 Scenario、相机和交互步骤；Standalone
不接受无法确定重放的输入步骤。

Standalone 的 `standardProfile.gameInstanceMode` 固定为
`project|minimal`，默认 `project`。`project` 使用工程配置的 GameInstance；
`minimal` 仅为该子进程覆盖为基础 `Engine.GameInstance`，用于规避依赖外部
Launcher 或项目服务的启动守卫，覆盖不会写回工程配置。两种模式会写入不同的
runtime fingerprint；`minimal` 不等价于完整游戏启动，也不能与 `project`
结果作为同一环境直接比较。

0.9.0 把 Trace 拆为三个编译边界：`UE_AI_integration` 的 Editor 控制与录制、
不依赖 UnrealEd/Slate 的 `UEAITraceAnalysisCore`，以及只进入 Development/
DebugGame Game Target 的 `UEAITraceRuntime`。独立 `UEAITraceWorker` Program 复用
同一 Core 做离线 TraceServices 查询；Runtime 不启动 HTTP/MCP/UI，Shipping
通过模块描述和编译宏排除。详细边界见
[渲染调试证据与离线 Insights](UE_TRACE_INSIGHTS.md)。

`performance.diagnose` 将现有 run/compare/Trace 证据投影为
`cpuBound`、`gpuBound`、`mixed`、`frameLimited` 或 `inconclusive`，并返回最差帧、
Top CPU scope、GPU 区间、日志健康度与下一步建议。只有帧时间匹配显式 FPS cap，
或 VSync 已启用且帧时间匹配常见刷新率时，才判为 `frameLimited`；单独观察到稳定
16.6667 ms 不能作为 limiter 或性能余量证据。若 Fingerprint 明确显示 VSync 和
FPS cap 均关闭但仍观察到稳定 60Hz cadence，诊断返回 `inconclusive`，并以
`frameLimiter.suspected=true` 和
`classification=suspectedExternalLimiter` 记录外部限帧或未上报呈现约束的
可能性；此时不会输出 CPU/GPU bound 结论。运行没有可用的本次日志窗口时，
`logHealth.status=unavailable`，不得报告为 `clean`。
`performance.report.generate` 同时保留 JSON/JUnit artifact，并生成可离线打开的
自包含 HTML 报告。HTML 呈现 run-bounded 日志健康度、Top CPU scope、GPU
interval/aggregate 证据和下一步建议；Artifact 只以转义后的不透明 ID、页内安全
锚点和 `production.job.artifact.get(jobId, artifactId)` 读取合同展示，不嵌入
本地路径或可执行链接。传入 `comparisonId` 时，该比较的 `candidateRunId` 必须与
报告的 `runId` 完全相同，否则返回
`performance_comparison_candidate_mismatch`，避免将无关 Run 的阈值结论写入
当前报告。

仓库内 `Performance.StandaloneEvidenceContract` 是使用替身 Operation 的合同
测试，不会启动 `UnrealEditor -game`，也不构成真实 Standalone E2E 证据。发布
门禁仍需在实际工程中启动子进程并核对 CSV、日志、Trace 与最终 artifact。

`trace.analyze` 与离线 query 使用 UE 5.3 TraceServices。`traceId` 可以是成功的
独立 Trace Job、成功的 Standalone performance run ID，或 Worker Store 中已
hash 登记的 import；Performance run 只有在自己登记了
`application/x-unreal-trace` artifact 时才会接受。传给
`performance.diagnose` 的 analysis job 还必须来自同一 performance run 的
Trace，否则返回 provenance mismatch。

`production.trace.provider.list` 是 Provider 可用性的权威，分别报告是否录制、
缺失 Channel、已启用 semantic adapter 和对应 Insights Panel。所有 query 使用
时间范围、过滤、稳定排序、cursor 与 limit，不返回原始事件洪流；尚未适配的
Provider 返回 `trace_query_unsupported`，不能用空结果伪装成功。Unreal Insights
只负责可视化打开，结构化结果不依赖其 UI，也不做鼠标自动化。

`performance.compare` 可同时检查多个指标阈值；环境指纹不兼容时返回
`inconclusive`，证据兼容时返回结构化 `pass` 或 `regression`。每次比较生成
受 `production.job.artifact.get` 分块读取的 JSON 与 JUnit artifact，可直接
作为 CI 门禁。`autoTraceOnRegression=true` 配合 `traceRerun` 会在阈值失败后
异步复跑同一受约束 workload、录制 Trace、运行 TraceServices，并把最多 25 个
Top Scope 和分析 artifact 附到 comparison job；诊断录制失败不会覆盖原始
回归判定。

## 2. 自动化测试

- `production.test.list/run/result.get`
- 统一 Automation、Functional Test 与项目自定义 Gauntlet 入口。
- 测试执行使用 `ue.job.v1`，结果和日志可在 Editor 重启后查询。
- 每次 Editor 测试进程使用 Job 目录内独立的 `editor.log`，并固定启用
  `-stdout -FullStdOutLogOutput -NoSound`；`job.log`、Editor 主日志、
  Automation 原生 JSON、标准化 JSON 和 JUnit 都登记为 artifact。
- `headlessProfile=minimal|project|rendering` 是固定枚举，不接受附加命令行。
  默认 `minimal` 禁用 OpenXR、音频、无关 Engine 插件并使用 NullRHI；
  需要项目 Engine 插件或渲染验证时显式选择 `project` 或 `rendering`。
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
- `scene.viewport.visualization.list/capture/compare/analyze`

写入仅覆盖 allowlist 内的 Session CVar，带精确 digest 和读回；不会静默修改
项目 DefaultEngine.ini 或平台配置。

Viewport Visualization 从真实 Engine/RHI/插件状态枚举 View Mode、Buffer、
Ray Tracing Debug、Nanite、Lumen、Virtual Shadow Map、GPU Skin Cache、
Strata/Substrate 与 Groom。Capture 只读指定 Editor/PIE `FViewport`，等待渲染并
在所有路径恢复原状态；输出是 8-bit 调试视图 PNG 证据，不是任意 GBuffer/RDG
Texture 导出。指纹不兼容的比较返回 `inconclusive`。

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
- 进程 Job 公开
  `launching/loading/discovering/running/reporting/exiting/complete` 阶段、
  阶段历史和各阶段耗时。Automation 的冷启动、测试发现、真实执行与退出
  不再被压缩成单一 `running` 状态。
- `startupTimeoutSeconds`、`executionTimeoutSeconds` 和
  `shutdownTimeoutSeconds` 分别约束启动/发现、执行/报告和退出；另有
  `hardTimeoutSeconds` 覆盖完整生命周期，运行时硬限制为 86400 秒。
  超时分别返回 `job_startup_timeout`、`job_execution_timeout`、
  `job_shutdown_timeout` 或 `job_hard_timeout`。终止会杀死受控子进程树，
  Runtime 析构也会清理所有仍存活的 Job，避免留下孤儿进程。
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

能力发现会根据 manifest 的 `requires.features/plugins/modules/platforms/engine` 返回
`available` 与结构化原因。默认目录是动态的，不再依赖每个领域的硬编码数量。

Niagara、Water 与 PCG 是可选 feature pack。通用 `BuildPlugin` 默认不链接这些
模块，也不会因启用 `UE_AI_integration` 自动拉起其依赖链。工程在 `.uproject`
中显式启用对应插件时，UBT 自动定义 `WITH_UEAI_NIAGARA`、
`WITH_UEAI_WATER` 或 `WITH_UEAI_PCG`；隔离构建也可通过
`UEAI_OPTIONAL_FEATURES=Niagara,Water,PCG` 明确选择。未编译的能力仍可在目录中
发现，但 `available=false`，执行时统一返回 `capability_unavailable`，不会进入
缺失模块的 Handler。
