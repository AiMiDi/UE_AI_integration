# 渲染调试证据与离线 Unreal Insights

`0.8.1–0.9.0` 将两类容易混淆的证据拆开：

- Editor/PIE Viewport 调试视图用于可重复的视觉证据。
- Unreal Trace 用于 Provider 级结构化数据，并可在 Editor 关闭后分析。

两者都使用有界语义合同。系统不读取任意 RDG Resource，不用鼠标模拟操作
Unreal Insights，也不把截图颜色猜成未经定义的底层数值。

## 1. Viewport 调试视图

公开能力：

- `scene.viewport.visualization.list`
- `scene.viewport.visualization.capture`
- `scene.viewport.visualization.compare`
- `scene.viewport.visualization.analyze`

`list` 对当前 Engine、RHI、插件和指定 Viewport 计算真实可用性。基础 View Mode
覆盖 Lit、Unlit、Wireframe、Detail Lighting、Lighting Only、Reflections 和
Collision；扩展 family 包括 Buffer Visualization、Ray Tracing Debug、Nanite、
Lumen、Virtual Shadow Map、GPU Skin Cache、当前版本可用的 Strata/Substrate
与 Groom。缺少模块、项目设置或兼容 RHI 时返回 `available=false` 和原因，不会
因为可选功能未启用而让插件链接失败。

目标可以是活动 Level Editor Viewport，或精确的 PIE
`sessionId + generation`。PIE 重启后的旧 generation 不会自动绑定到新会话。

一次 capture 固定执行：

1. 保存 View Mode、ShowFlags、相机、曝光、选择和 Viewport Transform。
2. 应用已确认可用的 mode，等待 Slate 布局、两个渲染帧和 Render Fence。
3. 直接从指定 `FViewport` 读取像素；不截桌面、整个窗口或其他 Viewport。
4. 在成功、超时和异常路径恢复保存状态并返回恢复验证。

默认 artifact 是无损 PNG，包含 capture ID、像素 SHA-256、World/Camera、RHI、
GPU、分辨率、DPI、主题、ShowFlags、View Transform 和渲染指纹。只有来源确实
是浮点 Render Target 时才能声明 EXR；普通 8-bit Viewport ReadPixels 不能包装
成 EXR。

### 明确边界

这是“调试 View Mode 的最终显示结果”，不是以下接口：

- 任意 RDG Texture 导出。
- GBuffer A–E、SceneDepth、Velocity 等内部 Resource Readback。
- 未经文档定义的材质、法线、深度或照明数值采样。

`analyze` 只对有明确编码语义的模式给出有界直方图、异常/缺失覆盖比例和变化
区域。`compare` 先比较 Engine/plugin、RHI/GPU、分辨率、DPI、主题、mode 与
View Transform 指纹；不兼容时返回 `inconclusive`，而不是把环境变化判为回归。

示例流程：

```text
visualization.list
  → visualization.capture(before)
  → 修改或切换验证状态
  → visualization.capture(after)
  → visualization.analyze(after)
  → visualization.compare(before, after)
```

## 2. Trace 进程边界

```text
Editor Module
  └── FTraceAuxiliary（Editor / PIE 录制）

Development or DebugGame Game Target
  └── UEAITraceRuntime（受约束录制；Shipping 不包含）

UEAITraceWorker Program
  └── UEAITraceAnalysisCore + TraceServices（不加载 UnrealEd / Slate）
```

`UEAITraceAnalysisCore` 是 Editor 与 Worker 共用的 Provider 解析层，不依赖 HTTP、
MCP 或项目 UObject。`UEAITraceRuntime` 只解释版本控制的 Launch Profile 和受限
Trace 参数，不启动 HTTP 服务，也不包含 Editor UI；Shipping target 通过模块描述
和编译宏排除。

`UEAITraceWorker` 与产生 Trace 的 Engine 次版本绑定。握手返回 Worker/plugin、
Engine、协议、contract digest、Provider 表和匹配的 Unreal Insights 路径；可解析
的 Engine major/minor 不一致时拒绝分析，不静默降级。

源码 Engine 的 Trace `BuildVersion` 可能只有 `UE5-CL-0`，无法证明次版本。由本
插件管理的 Editor、PIE、Development 和诊断录制会额外写入
`UEAI_TRACE_ENGINE_VERSION=<major>.<minor>` Bookmark；只有该 marker 精确匹配
Worker 时才接受不可解析的 BuildVersion，并返回 `matchedManagedMarker`。无 marker、
marker 畸形/冲突或跨次版本的 Trace 继续 fail closed。

Win64 使用当前用户专属 Named Pipe；Unix 平台使用用户专属 Unix domain socket。
CLI/MCP 按需启动 Worker，空闲十分钟后退出，最多保留两个 Analysis Session；
回收后可以按已登记 trace hash 重新分析。stdio one-shot 仅保留给隔离测试和诊断，
不开放 TCP/HTTP 监听端口。

Worker 定位顺序：

1. `UEAI_TRACE_WORKER`。
2. 插件包 `Tools/Trace/<platform>/<engineVersion>/UEAITraceWorker.exe`。
3. 当前源码树的匹配构建产物。

Worker 可执行文件与它使用的 Engine 是两个独立的解析合同。CLI/MCP 找到 Worker
后，按以下顺序解析并传入匹配的 Engine 目录；Worker 直接启动时使用相同顺序：

1. 显式 `-EngineDir=`。
2. `UEAI_ENGINE_ROOT`。
3. 兼容环境变量 `UE_ENGINE_ROOT`。
4. 从已安装 Worker 向上查找唯一 `.uproject`，读取其 `EngineAssociation`，并在
   Windows Launcher/用户自定义 Engine 注册表中解析精确安装目录。
5. 仅在前述来源均未声明 Engine 时，使用 Program 自身的 `FPaths::EngineDir()`。

每个候选都必须有可读的 `Engine/Build/Build.version`，且 `MajorVersion` 与
`MinorVersion` 必须和 Worker 完全一致。一个更高优先级来源一旦存在但无效、注册
缺失或次版本不匹配，解析立即失败，不会继续尝试较低优先级 Engine。安装目录附近
发现多个 `.uproject` 也视为歧义错误。CLI/MCP 在启动前以
`trace_worker_engine_mismatch` 拒绝这类配置；握手同时公开
`unrealInsightsEngineDir`、`unrealInsightsEngineVersion`、
`unrealInsightsSource` 和 `unrealInsightsUnavailableReason`，供发布验收核对真实
选择，而不是只检查某个 `UnrealInsights.exe` 是否存在。

### 完整 Trace 能力表（23 项）

下表是 `Resources/Capabilities/production.json` 的公开 Trace 合同。`localTrace`
表示独立 Worker；`editor` 表示当前 Editor 进程。Manifest 的 `command` 仅表示会
创建录制、Job、artifact 或外部 UI 动作，不等同于修改 UE 资产；是否只读仍以
`traits.readOnly` 为准。

| Capability | Kind | Backend | 用途 / 可见副作用 |
|---|---|---|---|
| `production.trace.start` | command | editor + localTrace | 启动受限 Editor/PIE/Development 录制；创建 Trace Job |
| `production.trace.status` | query | editor + localTrace | 查询录制状态、阶段和 artifact |
| `production.trace.stop` | command | editor + localTrace | 请求结束录制并完成 artifact/后处理 |
| `production.trace.analyze` | command, read-only | editor + localTrace | 创建有界 TraceServices 分析 Job/摘要 |
| `production.trace.target.list` | query | editor + localTrace | 列出 Editor、PIE 和受约束 Development 目标 |
| `production.trace.launch.plan` | validation | localTrace | 生成 Development 启动计划与审批 digest，不启动进程 |
| `production.trace.channel.list` | query | editor + localTrace | 列出 preset、Channel 和可用性 |
| `production.trace.import` | command, read-only | localTrace | 将外部 `.utrace` 复制/登记到受控 Store 并返回 `traceId` |
| `production.trace.provider.list` | query | editor + localTrace | 返回已录制 Provider、operation 和缺失 Channel |
| `production.trace.timing.query` | query | editor + localTrace | 查询 Frame、线程、事件、Timer、调用树和 CPU Sampling |
| `production.trace.counter.query` | query | editor + localTrace | 查询 Counter 定义、序列和聚合 |
| `production.trace.memory.query` | query | editor + localTrace | 查询分配、存活分配、Tag、Module 和 Callstack |
| `production.trace.loading.query` | query | editor + localTrace | 查询 Package、Object、Export、Request 和依赖 |
| `production.trace.network.query` | query | editor + localTrace | 查询 Connection、Packet、Content Event 和统计 |
| `production.trace.tasks.query` | query | editor + localTrace | 查询 Task、Relation、Wait 和 Critical Path |
| `production.trace.context_switches.query` | query | editor + localTrace | 查询 CPU Core、线程调度和 Context Switch 区间 |
| `production.trace.log.query` | query | editor + localTrace | 查询 Trace Log Category 和 Message |
| `production.trace.io.query` | query | editor + localTrace | 查询文件路径、IO Event 和聚合 |
| `production.trace.bookmark.query` | query | editor + localTrace | 查询 Bookmark |
| `production.trace.region.query` | query | editor + localTrace | 查询命名 Region 和时间范围 |
| `production.trace.screenshot.query` | query | editor + localTrace | 列出/读取有界的 Trace Screenshot |
| `production.trace.export` | command, read-only | editor + localTrace | 将一个有界语义查询导出为 JSON/CSV artifact |
| `production.trace.open_in_insights` | command, read-only | localTrace | 用匹配 Engine 的 Unreal Insights 打开已登记 Trace |

## 3. 录制目标

`production.trace.start` 支持 `backend=auto|editor|local`：

- `target.kind=editor` 固定走当前 Editor 的 `FTraceAuxiliary`。
- `target.kind=pie` 仍然是 Editor 进程级 Trace，但写入唯一 begin/end Region 和
  Bookmark；后续默认查询只使用该 Region，不能称为独立 PIE 进程 Trace。
- `target.kind=development` 固定走本地 Worker 和版本控制的 Launch Profile。

PIE 的 Network Trace 还有一条严格边界：如果 NetDriver/连接早于本次录制开始，
插件只能重放有界的连接元数据，不能补造录制前已经发生的 Packet 或 Content
Event。默认结果因此标记 `partial=true`，并返回
`network_session_predates_trace`。调用方需要完整网络证据时，应传入
`requireCompleteNetwork=true`；此时 start 会在写入误导性证据前返回
`trace_network_session_predates_trace`。由 Development Launch Profile 在进入
`recording` 阶段后创建的连接不受此限制。

先用以下能力发现真实目标和 Channel：

- `production.trace.target.list`
- `production.trace.channel.list`
- `production.trace.launch.plan`

Development 启动必须先取得 Launch Plan，再把完全相同的
`approvePlanDigest + confirmLaunch=true` 传给 start。Plan 展示解析后的 executable、
文件 hash、project、map、固定参数、CVar allowlist、preset、时间/文件上限和输出
目录；接口不接受任意 executable 或原始 command line。首版只管理由 AI 启动且
持有的子进程。

录制阶段为：

```text
launching → loading → recording → finalizing → analyzing → completed
```

每次录制有 `maxDurationSeconds` 与 `maxFileSizeMiB`。子进程崩溃后仍保留
`.utrace`、日志和 receipt，返回 `partial=true`，Worker 只对实际存在的 Provider
做有界分析。Stop 优先优雅完成；只有 AI 持有、且 Profile 明确允许的 Development
子进程，才能在 shutdown timeout 后终止。

## 4. Editor 关闭后的离线分析

公开能力：

- `production.trace.import`
- `production.trace.provider.list`
- `production.trace.timing.query`
- `production.trace.counter.query`
- `production.trace.memory.query`
- `production.trace.loading.query`
- `production.trace.network.query`
- `production.trace.tasks.query`
- `production.trace.context_switches.query`
- `production.trace.log.query`
- `production.trace.io.query`
- `production.trace.bookmark.query`
- `production.trace.region.query`
- `production.trace.screenshot.query`
- `production.trace.export`
- `production.trace.open_in_insights`

Import 是唯一接受外部 Trace 路径的能力。Worker 将文件复制或登记到受控 Store、
计算 SHA-256 并返回 `trace-local-*` ID；后续只接受 `traceId`。MCP 可读范围限制在
项目 `Saved`、Worker Store 或 `UEAI_TRACE_ROOTS` 明确声明的目录。

CLI 对显式绝对 `.utrace` 路径的默认 `copy` 导入使用隔离的 one-shot Worker，
只在该子进程环境中临时加入文件的规范化父目录；CLI 父进程和常驻 Worker 的
允许根目录不会被扩大。`copyMode=reference` 不使用临时授权，源文件必须已经位于
Worker Store 或持久配置的 `UEAI_TRACE_ROOTS` 中，保证后续常驻 Worker 仍能安全解析。

`provider.list` 是查询能力的权威：每个 Provider 都返回是否录制、
`queryImplemented`、可用 operation、缺失 Channel、不可用原因和对应 Insights
Panel。合同中存在某个 query ID 不代表当前 Trace 录制了它，也不代表当前 Engine
适配器已经启用；调用方必须先检查 Provider 状态。

当前 UE 5.3 Core 已实现 Timing（Frame、线程、事件、Timer、Callers/Callees、
CPU Sampling）、Counter、Memory、Asset Loading、Network、Tasks、Context Switch、
File IO、Log、Bookmark、Region 和 Trace Screenshot 的有界语义适配器。实现适配器
不等于某份 Trace 一定包含对应数据：只有 `provider.list` 同时返回
`queryImplemented=true` 和 `recorded=true` 时，该数据域才可查询。缺少 Provider 或
Channel 时会返回明确的不可用原因，不会把“没有录到数据”伪装成空结果。

所有查询都支持有界时间范围、过滤、稳定排序、cursor 和 limit。结果返回规范化
行、总数、截断状态和下一 cursor；`export` 将同一有界查询写为 JSON 或 CSV
artifact，不返回无限事件流。

### CLI

```powershell
ue trace doctor
ue trace target list
ue trace import --trace-path D:\Traces\sample.utrace --backend local
ue trace providers --trace-id trace-local-...
ue trace query timing --trace-id trace-local-... --operation frames --limit 100
ue trace export --trace-id trace-local-... --provider timing --operation timers --format json
ue trace open --trace-id trace-local-... --view timing
```

原有 `ue production.trace.*` 调用继续有效。`backend=auto` 只按 manifest 和目标
规则路由；显式 `editor` 或 `local` 失败时不会静默切换。带
`trace-local-*`、`trace-analysis-local-*` 或 `trace-launch-local-*` 命名空间的
Job/Trace ID 会由 CLI/MCP 路由到 Worker。Editor 未启动时，`ue_production` 仅允许
manifest 声明 `localTrace` 的能力；其他 Production capability 保持连接错误。

## 5. Unreal Insights 的角色

`production.trace.open_in_insights` 使用同一 Engine 的：

```text
UnrealInsights.exe -OpenTraceFile=<path>
```

Worker 使用第 2 节的严格 Engine 解析合同定位 Insights，包括已安装项目的精确
`EngineAssociation`；只有没有任何显式、环境或项目关联来源时才使用 Program
Engine 目录。选中的 Engine 没有同次版本 `UnrealInsights` 时，握手明确返回
`unrealInsightsAvailable=false` 和原因，`open_in_insights` 失败而不会启动其他
Engine 版本的 Insights。

`Resources/Trace/insights-actions.5.3.json` 维护 Provider、语义能力和主要 Insights
Panel 的版本化映射。能稳定应用目标 View/时间范围时返回 `viewApplied=true`；不能
应用时仍打开 Trace，并返回 `viewApplied=false`。

Unreal Insights 只负责人工可视化。结构化结果来自 TraceServices；不提供鼠标点击
Panel、拖动时间轴、窗口排列、Track 高度或缩放等 UI 自动化合同。

## 6. 发布边界

- 正式基线是 UE 5.3；5.4–5.7 的发布候选必须分别通过 Compatibility 构建验证。
  当前仓库不把仅有版本化映射文件视为这些版本已经实编译通过。
- Worker 与 Trace 的 Engine 次版本必须匹配，不承诺跨次版本解析。
- Win64 Worker、握手、Editor/PIE 录制、无 Editor Development 录制和离线查询
  是 FusionEffectBuild 安装门禁。
- CLI、MCP、构建与安装校验启动 Worker 时固定加入
  `-NoLog -NoDefaultLog -SaveToUserDir`；Worker 自身的 Program PreInit 也强制
  这三个开关。
  `-NoDefaultLog` 是 UE 5.3 中真正阻止默认文件输出设备的开关，可保证 stdio
  只包含协议 JSON；`-SaveToUserDir` 将 UE Program 仍需创建的空目录或诊断状态
  重定向到当前用户目录，避免在插件包的
  `Tools/UEAITraceWorker/Saved` 下生成日志。staging 或激活目录中出现该生成目录
  会被发布校验拒绝，运行日志应进入调用方管理的 Job artifact，而不是污染插件包。
- 可选 Niagara、Water、PCG、Ray Tracing、Nanite、Lumen 等关闭时，相关模式返回
  unavailable，不得造成编译或加载失败。
- 本阶段不附加到用户自行启动的 Development/Shipping 进程，不把 Runtime 模块
  编入 Shipping，也不直接导出任意 GPU Texture。
