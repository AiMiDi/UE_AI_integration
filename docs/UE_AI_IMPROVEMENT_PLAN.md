# UE AI 使用改进计划

更新时间：2026-09-30

这份文档记录 UE_AI_integration 在实际使用中的稳定问题、证据边界和改进顺序。它不是某一轮 Wave 的进度表，也不替代 capability manifest、Skill recipe 或 Workflow contract。能力数量、参数和可用性始终以当前目录和当前加载的 Editor 为准。

## 当前基线

本次检查使用插件仓库 `S:\SilverPalace\unrealengine\Engine\Plugins\Developer\UE_AI_integration`：

- `ue-cli` 1.0.0，source revision `04e5ff7d3bf1`。
- 本地目录 564 capabilities、14 skills；按 manifest 的域计数为 Blueprint 111、Scene 99、Content 208、Animation 19、AI 17、Production 110。
- `ue-cli doctor --json` 的本地目录、CLI 和 Workflow contract 检查通过；当前 Editor 不可达，不能把本地 catalog 当作当前 Editor 的运行能力。
- doctor 发现 37 条过期实例记录。它们不会直接证明插件故障，但会干扰连接选择和诊断阅读。

## 证据和历史会话

历史会话可以作为问题线索，不能直接作为当前能力证明。旧会话中有些使用的是 `S:\SilverPalace\Project\Plugins\UE_AI_integration`，而当前提交仓库位于 Engine 插件目录；旧的 capability 数量、DLL 身份和 Editor 状态必须重新核对。

本次读取或检索到的主要会话：

| 会话 | 结论 | 当前可复用内容 | 证据限制 |
| --- | --- | --- | --- |
| `01a07f81-b8a0-79a2-8a8a-4e0dce35a495` | SimCache 观察能力完成 | 有界采集、属性分页、导出、释放；GPU 采集与 timing 分开 | 需要匹配版本和 NonNullRHI；不是 GPU 私有 buffer 证据 |
| `01a07fb3-1f78-7fd3-9b39-48a53d93db1e` | GraphIR 查询和属性覆盖部分完成 | 快照、分页、边界、投影 hash、Custom HLSL 配置 hash | 大图输出曾超过 25 万 token；旧 DLL 安装和项目 Editor 验收不完整 |
| `01a0a915-d72a-7902-bbac-2d62947430ab` | NS、材质、蓝图对齐验证部分完成 | 需要分别比较三域；静态、隔离编译、实时 Editor 必须分层 | 曾出现本地 463、加载 Editor 461 的 catalog mismatch；完整 Monolith 行为未闭环 |
| `01a0c3eb-becd-7922-83a2-9eb31ef04d94` | 材质导入注册和 Niagara 保存路径修复完成 | 缺失注册会让服务 degraded；子对象必须保存所属顶层 package | 落叶 Cutout 尚无代表场景视觉和 GPU 收益验收 |
| `01a0d230-17ee-7be3-add1-60c032e53e54` | Trace 离线分析完成 | Worker 版本不匹配时切换匹配的 Unreal Insights；统计、日志和源码分开 | `UE5-CL-0` 不能安全映射 Worker；GPU pass 排名未证实 |

直接读取历史线程时，有些线程返回 `notLoaded` 或输出量过大；这些会话使用已归档摘要作为来源，并在下文标为历史线索。不能因为旧线程仍能打开，就认为其中的 Editor、DLL、资产或提交状态仍然有效。

## 主要缺口和使用困难

### P0：运行时真实性不够直接

使用者最难判断的是“能力已经写进 manifest”与“当前 Editor 真能执行”之间的差异。历史上出现过 `editor_unreachable`、`capability_catalog_mismatch`、旧模块仍被 Editor 加载，以及单个缺失注册导致全服务 `degraded`。

改进要求：

- 提供一个统一的 surface preflight，分别显示 CLI/catalog、Editor 连接、模块 hash/source revision、catalog digest、Skill/Workflow digest、Trace Worker 和权限状态。
- 每个 capability 的执行结果都要明确区分 `localDeclared`、`handlerRegistered`、`liveAvailable`、`executed`、`readbackVerified` 和 `runtimeVerified`。
- 注册缺失只影响对应 capability；服务级 degraded 必须列出受影响的 ID 和下一步动作。

验收：旧 DLL、目录漂移、Editor 不可达、单个 handler 缺失各有独立 fixture；CLI 先给出可执行的修复动作，不能只返回一个总状态。

### P0：发现和参数调用成本高

当前调用依赖多次手工操作：查询 Skill、读取完整 schema、准备 `params-file`、补 requestId/审批字段、再执行验证。历史会话还出现过参数名猜错、旧 GUID 与 `expr:<object>` 混用、CLI 选项 `--requestId` 与 `--request-id` 混淆。

改进要求：

- 提供 schema 驱动的参数模板和本地 preflight，生成最小合法请求并提示缺失字段、审批字段、作用域和保存策略。
- 错误结果包含 `nextAction`、对应的 `help` 命令和是否可以安全重试；未知写入结果必须先 recover/readback。
- 把资产路径、GraphIR nodeId、Blueprint nodeGuid、Niagara graph/pinId 统一成带类型的引用对象，禁止客户端静默转换。

验收：同一个操作从 `skills -> help -> params-file -> execute -> verify` 可以复制运行；错误参数在发送到 Editor 前被拒绝，并能指出正确字段。

### P1：大数据读取不适合 Agent

Niagara 图和材质图可以成功读取，但历史会话曾产生约 25 万 token 的嵌套输出。结果过大时，Agent 只能截断、重复查询或凭部分结果推断，增加了错误操作概率。

改进要求：

- 所有图查询默认返回结构摘要、计数、类型分布和可继续查询的 handle；节点、边、属性和源码必须分页或按范围读取。
- 统一 cursor/handle 的资产、快照、projection、generation 绑定，过期时返回重新发现指令。
- 为每个读操作声明扫描预算、响应字节预算和推荐的下一页参数；CLI 默认不打印完整嵌套 JSON。

验收：同一资产的首次查询在固定输出预算内完成；客户端可以只取一个 emitter/module、一个 GraphIR boundary 或一个 Blueprint 子图，并能验证没有跨快照串页。

### P1：写入、保存、回滚边界不一致

不同域的 plan/apply、Workflow、专用 batch 和直接保存工具各有一套协议。历史上出现过 Niagara renderer 子对象直接 SavePackage 导致崩溃、写入成功但没有明确磁盘 readback、以及隔离 Editor 结果被误认为项目 Editor 已加载。

改进要求：

- 所有写操作统一返回 plan digest、receipt、dirty 前后状态、compile 前后状态、保存结果、磁盘 hash/readback 和 rollback 可用性。
- 保存子对象时统一解析所属顶层 package；保存失败必须保留内存回滚和明确的磁盘状态。
- `scene.level.save`、Blueprint/Material/Niagara save 统一 `onlyIfDirty`、PIE/transient 拒绝、文件存在和 dirty 清除语义。

验收：每个域至少有“成功写入、验证失败、保存失败、重复 requestId、资产已变化、回滚后再读回”六类测试；测试不能依赖用户项目资产。

### P1：验证环境分层不够自动

NullRHI 适合契约和拒绝分支，但 Niagara 生命周期、材质预览、缩略图和 GPU/Viewport 结果需要 NonNullRHI。历史会话中曾把 npm/MCP、隔离 BuildPlugin、Native Automation、项目 Editor 和真实资产结果混在同一结论里。

改进要求：

- 固定四条验证 lane：manifest/CLI、隔离编译、NullRHI contract、NonNullRHI/真实 Editor acceptance。
- HostProject harness 自动记录源码 snapshot、插件包 hash、DLL/PDB identity、Editor instance、RHI、测试过滤器和结果文件。
- 报告模板强制写出 `staticVerified`、`compiled`、`moduleLoaded`、`assetReadback`、`runtimeVerified`、`visualVerified`，未知项不能被汇总为通过。

验收：一次测试报告可以反向定位“代码未编译、模块未加载、Editor 不匹配、RHI 不适用、真实资产未验证”中的具体一类。

本阶段已补上渲染测试的 RHI 选择边界：创建原生 Material/Blueprint 编辑器、触发
Shader 编译、预览、PIE 调试/捕获或布局捕获的 Automation 测试声明
`EAutomationTestFlags::NonNullRHI`。NullRHI 仍用于契约和拒绝分支；即使测试主体因
环境不可用而提前返回成功，也不会再被当作该测试的渲染验收入口。HostProject 静态
harness 会检查这组测试源码仍保留 NonNullRHI 标志；报告中的
`runtimeVerified`/`visualVerified` 继续保持未知，直到有对应的真实 Editor/资产证据。

### P1：当前域能力仍有语义缺口

这些是从当前 564 capability 目录和历史 Monolith 对比中得到的候选缺口，实施前必须重新读取当前 Monolith 注册和 live schema：

- **Niagara**：动态输入目前有 list/tree/get 和 add 流程，但 set/remove/search/完整树编辑仍需核对；事件处理器和仿真阶段已有 add 流程，缺少完整查询、修改和删除闭环；系统 spec 目前有 export，import/round-trip 仍需补；模块 override/duplicate、renderer 的 mesh/ribbon/subUV 细节也需要专项核对。
- **Material**：GraphIR `execute_plan` 已存在，但“读 boundary → 执行 → 读回 → 恢复”的语义完整性、共享节点保护和失败恢复仍要按真实资产验收；预览、缩略图、tiling、Custom HLSL、函数实例和父级迁移需要 NonNullRHI/Editor context 的端到端证据，而不能只看 handler。
- **Blueprint**：Timeline、CDO、批量节点、复制/导出、DataAsset 和批量生成已有目录入口；模板发现/应用、跨资产引用稳定性、声明式 build 的真实编译保存和运行态验收仍需补齐。
- **项目专属能力**：WidgetTree、ViewModel、Lua binding/packing 仍属于项目 MCP 边界，不应为了“通用能力数量”硬塞进 UE_AI_integration；需要提供清晰路由和互操作说明。

### P2：Trace 和性能证据容易误读

Trace Worker 必须与 Engine 版本匹配；`UE5-CL-0`、未知 breadcrumb、GPU 导出为空或等待作用域都不能直接推出 GPU pass 或 FPS 收益。应把 Worker 失败、原生 Insights fallback、日志交叉验证和最终结论分开。

验收：性能报告必须包含 trace/build identity、采样窗口、统计口径、日志对应关系和未证实项；并明确区分引擎 frame、显示 FPS、GPU 时间、并行累计时间和等待时间。

### P2：本地实例和过程材料干扰诊断

当前 doctor 看到 37 条 stale instance records。旧过程文档还把过时 capability 数量、旧路径和一次性验证文件留在仓库，容易被 Agent 当成当前事实。改进方向是提供显式的 stale-record 清理报告/可选清理动作，并把一次性验证输出放到任务归档或临时证据目录，不再进入长期文档树。

## 实施顺序

### 阶段 1：运行面和调用面收口

1. 实现 surface preflight 和可读的 degraded/unreachable 诊断。
2. 增加 manifest-handler 重复/缺失检查，并把受影响 capability 逐项列出。
3. 生成 schema 参数模板，统一 `requestId`、审批、保存和 retry/recover 提示。
4. 清理 stale instance 的只读诊断路径，确认不会误删仍存活的 Editor 记录。

完成标准：用户只需执行一次 preflight，就能知道当前请求是否适合继续，以及失败后应重载、重启、切换 RHI、修正参数还是停止重试。

### 阶段 2：引用和结果模型统一

1. 发布 typed asset/graph/node handles，并在所有分页结果中绑定 source fingerprint/generation。
2. 为大图和 Niagara 查询增加摘要模式、预算字段和稳定的 continuation。
3. 为所有写操作统一 receipt/readback/save/rollback 结果字段。

完成标准：Agent 不需要从大段 JSON 猜节点身份，也不会把旧快照、旧 GUID 或未保存内存状态当成当前资产。

### 阶段 3：验证矩阵和真实资产通道

1. 保留 NullRHI 契约测试，增加 NonNullRHI 渲染/SimCache/材质预览通道。
2. 建立“隔离包”和“项目 Editor”两套明确身份，测试结果不可互相冒充。
3. 为 `scene.level.save`、Niagara renderer/material、Material GraphIR、Blueprint build/compile 各建立真实 Editor acceptance recipe。

完成标准：每个能力的报告都能说明编译、加载、资产读写、保存、运行和视觉证据分别是否通过。

### 阶段 4：按真实缺口补齐语义

按阶段 1–3 的基础完成度，依次推进 Niagara 动态输入/事件/stage/spec import、Material GraphIR 完整恢复与 NonNullRHI 预览、Blueprint template/build 端到端验证。每次只闭合一个语义族，保持 manifest、handler、Skill、测试和验收报告同步。

## 维护规则

- 能力数量和状态从 manifest/loaded Editor 实时读取，不在文档中维护手工计数。
- 历史会话只作为线索，引用时保留 thread ID、时间、路径和证据等级；旧会话不可读取时不补猜结论。
- 一次性验证报告放在任务证据目录；长期文档只保留调用合同、边界、失败处理和可复现验收入口。
- 未完成真实 Editor、NonNullRHI 或项目资产验收时，统一使用“静态/隔离验证通过，运行时未证实”的措辞。
