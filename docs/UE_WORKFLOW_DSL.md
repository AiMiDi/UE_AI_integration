# UE Workflow DSL / CLI

`UE Workflow DSL` 用于把围绕一个或一组 Unreal 资产的连续编辑合并为一次确定、
可审批、可恢复、可回滚的执行。命令行程序名为 `ue-workflow`，MCP 工具名为
`ue_workflow`，HTTP 入口继续为 `/api/v1/workflow`；路由版本与 DSL 版本彼此
独立。

## 适用边界

只有同时满足以下条件的 operation 才能标记为 `editStep`：

- 围绕一个 `blueprint`、`widgetBlueprint` 或 `material` 主 scope。
- 前一步的结构化输出可以通过 JSON Pointer 绑定给后一步。
- 能参加同一个 UE Transaction。
- 中间步骤可以延迟 Compile 和 Save。
- 执行前可以生成完整、确定的 plan。

Blueprint/PIE 调试、断点、调用栈、日志分析、性能采样、Cook、Build 和 Package
不属于资产编辑 Workflow。它们继续使用单次领域 MCP/CLI operation，或独立 Job
接口。

## Workflow AST

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
ue-workflow validate --file .\workflow.json
ue-workflow plan --connect --file .\workflow.json
ue-workflow execute --file .\workflow.json `
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
ue-workflow execute --file .\workflow.json `
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
ue-workflow --help|--version [--json]
ue-workflow doctor [--connect] --json
ue-workflow capabilities [--connect] [--query <text>] [--domain <domain>]
                         [--kind <kind>] [--risk <risk>] [--available-only]
                         [--offset <n>] [--limit <n>]
                         [--detail summary|full]
ue-workflow help composable [blueprint|widget|material] --json
ue-workflow help operation <type> --json
ue-workflow validate --file <workflow.json|->
ue-workflow plan [--connect] --file <workflow.json|->
ue-workflow execute --file <workflow.json|-> --approve-plan <digest>
                    --receipt <path> [--save-on-success] [--confirm-write]
                    [--detail-level summary|standard|full]
                    [--section <name>]...
ue-workflow resume|status|rollback --receipt <path>
                    [--detail-level summary|standard|full]
                    [--section <name>]...
ue-workflow operation run <type> ...
ue-workflow shell
```

机器可读结果写 stdout；连接进度和日志写 stderr。`validate`、离线 `plan` 和
help 可离线使用，但可执行审批必须来自 `plan --connect`。`execute`、run 状态
与 rollback 需要正在运行的 Unreal Editor。`capabilities --available-only`
同样要求 `--connect`。
`--section` 可重复使用；`--details` 暂作为 `--detail-level full` 的兼容别名。

### 构建与安装

```powershell
cmake -S . -B build-workflow -DUE_WORKFLOW_BUILD_TESTS=ON
cmake --build build-workflow --config Release
ctest --test-dir build-workflow -C Release --output-on-failure
cmake --install build-workflow --config Release --prefix C:\Tools\ue-workflow
```

安装后的 `bin/ue-workflow` 会相对定位
`share/ue-workflow/{Contracts,Capabilities}`，不依赖源码工作目录。

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
