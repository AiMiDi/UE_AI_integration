# UE Workflow DSL / CLI

`UE Workflow DSL` 用于把围绕一个或一组 Unreal 资产的连续编辑合并为一次确定、
可审批、可恢复、可回滚的执行。命令行程序名为 `ue-workflow-cli`，MCP 工具名为
`ue_workflow`，HTTP 入口继续为 `/api/v1/workflow`；路由版本与 DSL 版本彼此
独立。

## 适用边界

只有同时满足以下条件的 operation 才能标记为 `editStep`：

- 围绕一个 `blueprint`、`widgetBlueprint`、`material` 或 `materialFunction` 主 scope。
- 前一步的结构化输出可以通过 JSON Pointer 绑定给后一步。
- 能参加同一个 UE Transaction。
- 中间步骤可以延迟 Compile 和 Save。
- 执行前可以生成完整、确定的 plan。

Blueprint/PIE 调试、断点、调用栈、日志分析、性能采样、Cook、Build 和 Package
不属于资产编辑 Workflow。它们继续使用单次领域 MCP/CLI operation，或独立 Job
接口。

## Workflow AST

### 材质函数与大图查询

Custom HLSL 的代码、输入输出、宏及 include 可通过 `content.material.custom.set`
一次配置，参数通过 `content.material.parameter.set` 修改；两者均支持材质和材质函数
scope，连续步骤延迟刷新和保存。使用方法与编译诊断边界见
[Custom HLSL 编辑](MATERIAL_CUSTOM_HLSL.md)。

`materialFunction` scope 支持 v1 和 v2：创建函数、增删/移动表达式、修改值、
连接/断开引脚，以及 `content.material.function.interface.set` 配置函数输入输出。
目标函数由 scope 注入，operation 和 bindings 不能切换到其他材质或函数。
连续步骤只标脏，最后执行一次 `content.material.function.validate`，统一更新函数
及已加载的依赖。该 finalizer 校验结构并调用原生更新，不表示 Shader 或画面验证通过。
最终保存和故障恢复仍遵守 Workflow 的 persistence、checkpoint 和 rollback 契约。

可运行示例：[材质函数批处理](../Workflow/tests/fixtures/material-function.workflow.json)。
表达式使用 `expr:<对象名>` 稳定标识；仍接受已有编辑器节点 GUID。稳定标识仅限同一资产，
删除、替换或重命名后需要重新查询。函数引脚支持名称或 `index:N`；断开操作要求名称
能唯一确定输入或输出，引脚名称有歧义时拒绝执行。

大图查询通过 `content.material.graph.index` 建立只读快照，再用
`content.material.graph.nodes.list` 和 `content.material.graph.subgraph.get`
分页或取局部子图。每页不会重新读取资产。修改后应重新捕获，再准备当前 Workflow plan；
查询的 `projectionHash` 不能替代 Workflow 的资产前置条件。
设计、预算、覆盖边界见 [大图查询方案](MATERIAL_GRAPH_QUERY.md)。

```json
{
  "dsl": "ue.workflow",
  "dslVersion": "1.0",
  "workflowKind": "assetEdit",
  "workflowId": "build-login-widget",
  "scope": {
    "kind": "widgetBlueprint",
    "asset": "/Game/UI/WBP_Login",
    "createIfMissing": true
  },
  "persistence": "dirtyOnly",
  "operations": [
    {
      "id": "title",
      "type": "content.widget.child.add",
      "params": {
        "parent": "RootCanvas",
        "class": "TextBlock",
        "name": "Title"
      }
    },
    {
      "id": "layoutTitle",
      "type": "content.widget.slot.layout.set",
      "bindings": {
        "/target": {
          "from": "title",
          "path": "/widgetRef"
        }
      },
      "params": {
        "anchors": [0.5, 0.0, 0.5, 0.0],
        "alignment": [0.5, 0.0],
        "offsets": [-200, 40, 400, 64]
      }
    }
  ],
  "verify": {
    "compile": true,
    "readBack": ["widgetTree", "bindings", "layout"]
  }
}
```

`scope.asset` 由 Runtime 注入 operation，作者不能混入第二个主资产。
`content.widget.child.add` 的 DSL 别名 `parent/class/name` 会在 plan 中规范化为
底层 handler 字段。`widgetRef` 是带 `kind`、`widgetBlueprint` 和 `name` 的
typed object；绑定目标 JSON Pointer 相对于目标 operation 的 `params`。

v1 没有循环、条件、字符串插值、脚本、事件等待和人工分支。

## Workflow v2：多资产执行

`dslVersion: "2.0"` 将单个 `scope` 改为最多 16 个具名 `scopes`，每个 operation
必须显式选择一个 scope。最多允许 256 个 operation；`dependsOn` 与 typed JSON
Pointer binding 共同形成确定 DAG。

```json
{
  "dsl": "ue.workflow",
  "dslVersion": "2.0",
  "workflowKind": "assetEdit",
  "workflowId": "add-shared-state",
  "scopes": {
    "controller": {
      "kind": "blueprint",
      "asset": "/Game/Automation/BP_Controller",
      "createIfMissing": false
    },
    "view": {
      "kind": "widgetBlueprint",
      "asset": "/Game/Automation/WBP_View",
      "createIfMissing": false
    }
  },
  "persistence": "dirtyOnly",
  "operations": [
    {
      "id": "addState",
      "scope": "controller",
      "type": "blueprint.variable.add",
      "params": {
        "variableName": "SharedState",
        "variableType": "String"
      }
    },
    {
      "id": "addLabel",
      "scope": "view",
      "type": "content.widget.child.add",
      "dependsOn": ["addState"],
      "params": {
        "parent": "RootCanvas",
        "class": "TextBlock",
        "name": "StateLabel"
      }
    }
  ]
}
```

v2 planner 会：

- 将资产路径规范化后排序，执行前锁定完整集合。
- 为每个 scope 生成 initializer、一次最终 compile/read-back/diff 和结构 hash。
- 按拓扑顺序执行跨 scope operation；binding 只能引用已声明输出，且目标参数
  必须通过类型校验。
- 在所有资产验证通过后才执行显式请求的统一保存。

v1 与 v2 并存。v1 planner 和 `planDigest` 算法保持不变；Editor 校验 v1 digest
后把执行映射为单 scope v2 模型，不要求调用方迁移已有 Workflow。

v2 仍然不支持循环、条件、事件等待、调试、性能采样、Cook 或其他长任务。

## Plan 与审批

```powershell
ue-workflow-cli validate --file .\workflow.json
ue-workflow-cli plan --connect --file .\workflow.json
ue-workflow-cli execute --file .\workflow.json `
  --approve-plan sha256:<64-hex-digest> `
  --receipt .\workflow.receipt.json
```

离线 `plan` 会规范化 AST、解析依赖和 typed binding、计算风险，并自动添加 initializer
（仅在 `createIfMissing` 需要时）、一次 compile、read-back 和结构 diff。
该结果明确标记 `executionReady=false`，不能用于执行审批。`plan --connect`
还会让 Editor 解析目标资产，并把 Package GUID、磁盘 SHA-256、完整可写对象
内存摘要、结构 hash、Dirty 状态和生成类版本绑定进新的审批 digest。既有目标
资产必须是 Clean；否则返回 `asset_dirty`，要求先保存或还原。Editor 在
`execute` 前重新核对 Core contract 与全部资产前置条件；任一变化都在零写入
状态返回 `asset_precondition_failed`。

`verify` 只用于选择 read-back 的详细内容，不能关闭 v1 的自动 finalizer。为兼容
已有 AST，schema 仍接受布尔型 `compile` 和数组型 `readBack`，但 planner 会把
`compile: false` 规范化为 `true`，并把缺失或空的 `readBack` 规范化为当前 scope
的非空默认值（Blueprint 为 `asset`、Widget Blueprint 为
`widgetTree/bindings/layout`、Material 为 `graph`）。这些兼容写法与省略 `verify`
的标准写法生成相同的规范化计划和 `planDigest`；结构 diff 始终自动追加。

`confirmWrite` 风险还要求：

```powershell
ue-workflow-cli execute --file .\workflow.json `
  --approve-plan sha256:<64-hex-digest> `
  --confirm-write `
  --receipt .\workflow.receipt.json
```

默认成功后只保持 Dirty。只有显式 `--save-on-success` 才会在全部验证通过后保存
一次。

## Journal、恢复与回滚

Editor 将完整恢复数据写入项目 `Saved/UEWorkflow/`，对外 receipt 只保留稳定的
`runId`、digest、scope/hash 摘要和 rollback 状态。

- v2 在 operation 或 segment 边界落盘，可在 Editor 重启后通过现有
  `status`/`resume` action 重附着并从安全边界继续。
- Handler 内部执行不是检查点；进程在单个 Handler 中断时，会从上一个已完成
  segment 恢复，而不宣称恢复 Handler 的内部状态。
- `resume` 会重新核对插件版本、contract digest、当前 package hash 与 Journal
  记录。同一 Editor 实例还会比较完整可写对象内存摘要；重启后的 Editor 拒绝
  已加载为 Dirty 的目标。任一资产被外部修改时返回 `resume_conflict`，绝不
  覆盖外部变化。
- 失败或显式 `rollback` 会从持久快照恢复所有既有资产、删除本次新建资产，
  然后重新 compile、read-back 并校验结构 hash。

## CLI

```text
ue-workflow-cli --help|--version [--json]
ue-workflow-cli doctor [--connect] --json
ue-workflow-cli capabilities [--connect] [--query <text>] [--domain <domain>]
                         [--kind <kind>] [--risk <risk>] [--available-only]
                         [--offset <n>] [--limit <n>]
                         [--detail summary|full]
ue-workflow-cli help composable [blueprint|widget|material] --json
ue-workflow-cli help operation <type> --json
ue-workflow-cli validate --file <workflow.json|->
ue-workflow-cli plan [--connect] --file <workflow.json|->
ue-workflow-cli execute --file <workflow.json|-> --approve-plan <digest>
                    --receipt <path> [--save-on-success] [--confirm-write]
                    [--detail-level summary|standard|full]
                    [--section <name>]...
ue-workflow-cli resume|status|rollback --receipt <path>
                    [--detail-level summary|standard|full]
                    [--section <name>]...
ue-workflow-cli shell
```

机器可读结果写 stdout；连接进度和日志写 stderr。`validate`、离线 `plan` 和
help 可离线使用，但可执行审批必须来自 `plan --connect`。`execute`、run 状态
与 rollback 需要正在运行的 Unreal Editor。`capabilities --available-only`
同样要求 `--connect`。
`--section` 可重复使用；`--details` 暂作为 `--detail-level full` 的兼容别名。
每个子命令都支持无副作用的分级 `--help`；帮助在 contract 加载和 Editor
连接之前返回。额外 positional 参数会以 `invalid_arguments` 拒绝，不再落入
默认命令或执行路径。

`doctor` 分别报告 DSL 1.0 与 2.0 的本地 `contractSetDigest`。使用
`doctor --connect` 时还会分别展示 Editor digest 和 `match`，只有两套合同都
匹配时 `editor.contractMatch=true`。

每个 `ue-workflow-cli` 进程生成一个 `invocationId` 并最佳努力注册客户端会话。
同一条 `execute` 命令的在线 plan 与 execute、以及 shell 中的后续请求复用
同一个 `X-UEAI-Session-Id`，因此 Editor 状态菜单只统计一次 CLI invocation。
旧 Editor 缺少会话路由时自动退回 Legacy HTTP；会话失效时最多重新注册并
重放原请求一次。该诊断身份不参与 Workflow 审批、digest 或权限判断。

`operation run` 已在 0.6.0 移除。单次 capability 迁移到独立短操作 CLI：

```powershell
# 旧：ue-workflow-cli operation run scene.pie.status --params '{}'
ue-cli scene.pie.status

# 旧：ue-workflow-cli operation run blueprint.asset.get --params '{"name":"/Game/BP_A"}'
ue-cli blueprint.asset.get --name /Game/BP_A
```

`ue-cli` 默认读取随程序分发的 schema，并可用 `--live-schema` 强制读取 Editor
精确 schema；它不属于 Workflow DSL，详见
[UE 短操作 CLI](UE_SHORT_CLI.md)。

### 构建与安装

```powershell
cmake -S . -B build-workflow -DUE_WORKFLOW_BUILD_TESTS=ON
cmake --build build-workflow --config Release
ctest --test-dir build-workflow -C Release --output-on-failure
cmake --install build-workflow --config Release --prefix C:\Tools\ue-workflow-cli
```

安装同时生成 `bin/ue-cli` 与 `bin/ue-workflow-cli`。后者会相对定位
`share/ue-workflow-cli/{Contracts,Capabilities}`，不依赖源码工作目录。

## MCP

```json
{
  "action": "execute",
  "workflow": {},
  "approvePlanDigest": "sha256:<64-hex-digest>",
  "saveOnSuccess": false,
  "confirmWrite": false,
  "detailLevel": "summary",
  "sections": ["readBack", "assetDiff"]
}
```

`ue_workflow` 支持 `validate`、`plan`、`execute`、`resume`、`status` 和
`rollback`。MCP 只接收内联 JSON object，不接收本地文件路径。`rollback`
还需要原 run 的 `runId` 和已审批 digest。

### 分级响应

`validate/plan` 默认使用 `standard`；`execute/resume/status/rollback` 默认使用
`summary`：

- `summary`：状态、Mutation、operation/finalizer 状态计数、Dirty Package 与
  diagnostics 数量、Diff 统计、rollback 摘要和 `resultRef`。
- `standard`：在 summary 上增加不含原始 output 的 operation/finalizer 清单。
- `full`：返回全部 section，但 ReadBack、Diff 和结构快照各只出现一次。

`sections` 可从 `operations`、`finalizers`、`readBack`、`assetDiff`、
`structures`、`rollback`、`diagnostics` 中按需附加。显式 section 不会改变
`detailLevel` 的默认投影。旧 `details:false/true` 继续映射到
`summary/full`；同时传 `details` 与 `detailLevel` 会返回
`422 invalid_workflow_request`。

Widget Blueprint 的 `widgetTree/bindings/layout` 共用一次
`content.widget.hierarchy.get`，再分别投影。finalizer 清单只保留执行元数据和
状态，原始读回只位于 `readBack` section；完整字段 Diff 只位于
`assetDiff` section。外部 receipt 是精简的运行凭据，完整恢复数据只保存在
`Saved/UEWorkflow` journal 中。

v1 的 Editor 执行是同步的，因此 `resume` 不会重新执行 editStep，也不接受
workflow 或参数修改。它只接受当前 Editor Runtime 内存中已经记录的 `runId`：
对 `completed`、`failed`、`blocked`、`rolledBack` 终态幂等返回原 receipt，
并标记 `resumeMode=terminalReattach`、`reattached=true`、
`resumedExecution=false`。来自其他 Editor 实例或未知的 run 会被拒绝；若遇到
不可安全续跑的非终态记录，则返回 `workflow_resume_not_safe`，不会伪装成已续跑。

HTTP action 返回 `ue.workflow-result.v1`；其中嵌套的精简
`ue.workflow-run.v1` receipt 才是 CLI 写入 `--receipt` 的持久化对象。
CLI 在落盘前校验 result、receipt、plan digest 和 contract digest。

## Editor Runtime

```text
GET  /api/v1/workflow/handshake
POST /api/v1/workflow
```

Editor 在 Game Thread 加载主资产，记录结构快照并开启单个 Transaction。
editStep 中使用 `deferCompile=true` 和 `dirtyOnly=true`，最后统一编译、读回和
生成结构 diff。失败时先撤销本 Workflow 仍位于 Undo 栈顶的 Transaction；若
结构 hash 不一致，则使用仅在本次执行期间保活的 domain-owned UObject 内存快照
恢复并再次验证。仍不能安全证明已恢复时，receipt 会标记 `manualReview`，不会
覆盖用户在 Workflow 之前已有的未保存修改。

run journal 位于项目的 `Saved/UEWorkflow/`。`status` 可以读取 journal。v2
会在确定的 operation/segment 边界持久化进度；Editor 重启后从已校验的 staged
package baseline 重放未完成部分。显式 rollback 优先使用同实例 Undo/内存快照，
跨实例则使用持久 package 快照，并在覆盖前重新检查外部修改冲突。

已存在的 Blueprint、Widget Blueprint 和 Material 连续编辑只执行一次最终
compile。UE 5.3 的 Blueprint/Widget Blueprint 创建工厂会在创建时同步生成
skeleton class，因此 `createIfMissing` 或显式 create 可能额外产生一次
bootstrap compile；该引擎边界会在 receipt 中与最终 edit finalizer 分开记录。

同一轮材质或蓝图的连续改线、改参数、移动节点，应把可组合操作放入同一个
Workflow。批次内保留图连线校验和节点通知，延迟资产级刷新；收尾时统一发出
Blueprint 修改通知，或同步 Material 表达式连线并更新预览。自定义 Graph Schema、
自动转换节点和节点自身的必要回调仍使用引擎行为，不能把一次 finalizer 等同于
所有情况下只触发一次 shader 编译。

`saveOnSuccess=true` 在验证通过后执行最终保存一次；`dirtyOnly` 不提交最终文件。
两者仍会为实际变化保存恢复检查点。检查点会比较 domain 对象内存摘要，复用未变化且
校验通过的 package 镜像，减少每步执行前、只读收尾及多资产中未变化 scope 的
重复保存；执行写操作的 scope 和编译后的依赖集合仍会保存检查点。恢复 journal
仍逐步持久化；这不表示批次期间完全没有磁盘写入。
独立 domain command 保留立即刷新和保存行为，不会跨请求自动合并。

材质的 `content.material.pin.disconnect` 和
`content.material.expression.delete` 已开放为 v1/v2 `material` scope 的
`editStep`，可与新增节点、改值及连线混合执行。两者均为 `confirmWrite`：
执行需要当前计划的 `approvePlanDigest` 和 `confirmWrite=true`，CLI 对应
`--approve-plan` 与 `--confirm-write`。编辑步骤不单独保存或编译，最终校验后按
`saveOnSuccess` 决定是否保存；恢复检查点仍会写盘。

删除同时移除图节点、表达式及引用它的下游/材质输出连线。复合子图及边界节点
暂不支持单表达式删除，命令会在修改前拒绝。MaterialFunction 的独立编辑入口
仍不属于 Workflow `material` scope，不能通过 operation 参数切换目标。

可运行示例：`Workflow/tests/fixtures/material-rewire.workflow.json`，演示
创建连接、断线、删除旧节点，再绑定新节点并重新连线。先执行本地
`ue-workflow-cli validate --file <示例路径> --json`，实际执行前通过
`plan --connect` 获取与当前 Editor 状态绑定的计划。

## Contract 与准入

版本化 schema 和 admission contract 位于 `Workflow/Contracts/`。能力 manifest
中的 `dsl` 元数据使用五种固定准入值：

| admission | 含义 |
|---|---|
| `editStep` | 作者可放入 Workflow AST |
| `finalizer` | planner 自动追加 |
| `observeOnly` | 只用于最终 read-back |
| `interactiveOnly` | 调试、PIE、日志等交互能力 |
| `none` | 普通单次 operation 或独立 Job |

当前首批开放 Blueprint Authoring、UMG Layout 和 Material Graph。删除 Blueprint、
reparent、批量替换调用、Animation State Machine、Behavior Tree、Sequencer 和
长任务编排不在 v1 范围内。
