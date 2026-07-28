# UE Engineering Copilot

`UE Engineering Copilot` 在现有 UE MCP 与 `ue-workflow` 之上补齐工程分析和
生产验证闭环。它不把所有能力塞进 Workflow：单资产、可预先规划的连续编辑
使用 UE Workflow DSL；性能采样、Trace、测试、Cook、Package、BuildGraph
等长任务使用 Durable Job；查询和交互式调试继续使用单次 capability。

## CLI 与 MCP 的能力关系

- MCP 的十一个稳定工具负责发现、查询、执行和 Workflow 入口。
- `ue-workflow operation run <capability>` 通过同一 `/api/execute` 合同调用
  Editor，因此单次 capability 的能力范围与 MCP 相同。
- `ue-workflow validate|plan` 可离线运行；`execute`、单次 capability 和 Job
  都要求连接 Editor。
- UE Workflow 只接纳 manifest 中声明为 `editStep` 的单资产连续编辑。
  Trace、性能、测试、Blueprint 分析、Cook/Package、Source Control、
  DDC、BuildGraph、HLOD 和 PCG 均不会被伪装成 Workflow。

## 1. Trace 与窗口性能证据

- `production.trace.start/status/stop/analyze`
- `production.performance.run/result.get/compare`
- 支持 warmup、采样窗口、帧预算、p50/p95/p99、峰值、超预算帧，以及
  Game/Render/RHI/GPU 指标。
- Trace 文件作为 `ue.artifact.v1` 分块读取；CLI 可用 `--output` 原子导出。

`trace.analyze` v1 返回受约束的采集元数据和 trace artifact，不宣称已经代替
Unreal Insights 的完整 Timing、Memory、Network 或 Asset Loading provider
语义分析。

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
关联状态。当前运行时关联接受调用方提供的已观察 Node ID；直接解析 Trace
provider 属于后续增强。

## 4. 通用资产与 Mesh/Texture 审计

- `content.asset.search/get/dependencies/referencers/audit`
- `content.static_mesh.inspect`
- `content.texture.inspect`
- `content.asset.change.plan/execute/rollback`

变更限制在 `/Game`，批量最多 100 项。所有写入都要求精确 plan digest、
`confirmWrite=true` 和顶层 `requestId`；Delete 有 Referencer 门禁。无法可靠
逆转的 Reimport 与 Fix Redirectors 会明确返回 `rollbackAvailable=false`。

## 5. World Partition、Data Layer、HLOD、PCG

- World Partition 状态、Cell、Streaming Source 和审计。
- Data Layer 列表与详情。
- HLOD 审计与受约束构建任务。
- PCG 检查，以及显式确认后的 Generate/Cleanup。
- World Partition 设置变更采用 plan/execute/rollback。

HLOD 是长任务，不进入资产 Workflow；PCG 只在可用插件/模块满足 manifest
`requires` 时暴露为可执行能力。PCG Generate/Cleanup 使用同一 Editor 实例内的
`requestId` 回执做幂等重放；其异步图执行仍由 PCG 自身调度，不伪报为已经完成。

## 6. 渲染配置与诊断

- `scene.render.context.get`
- `scene.render.feature.audit`
- `scene.render.memory.sample`
- `scene.render.settings.plan/execute/rollback`

写入仅覆盖 allowlist 内的 Session CVar，带精确 digest 和读回；不会静默修改
项目 DefaultEngine.ini 或平台配置。

## 7. Animation 与 AI 只读闭环

- Animation Blueprint、状态机与 BlendSpace 的 list/get/validate/diff。
- Behavior Tree 与 Blackboard 的 list/get/references/validate/diff。

这些能力补齐已有创建型操作的检查闭环，但不宣称覆盖 Control Rig、IK Rig、
StateTree、Mass、EQS 或运行时 AI Debugger。

## 8. Cook、Package 与生产 Job

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

## 9. Source Control、DDC、BuildGraph 与 Horde

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
