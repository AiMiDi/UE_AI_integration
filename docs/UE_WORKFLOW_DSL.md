# UE Workflow DSL / CLI

`UE Workflow DSL` 用于把围绕同一个 Unreal 资产的连续编辑合并为一次确定、
可审批、可回滚的执行。命令行程序名为 `ue-workflow`，MCP 工具名为
`ue_workflow`，HTTP 入口为 `/api/v1/workflow`。

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

## Plan 与审批

```powershell
ue-workflow validate --file .\workflow.json
ue-workflow plan --file .\workflow.json
ue-workflow execute --file .\workflow.json `
  --approve-plan sha256:<64-hex-digest> `
  --receipt .\workflow.receipt.json
```

`plan` 会规范化 AST、解析依赖和 typed binding、计算风险，并自动添加 initializer
（仅在 `createIfMissing` 需要时）、一次 compile、read-back 和结构 diff。
`planDigest` 覆盖规范化计划和 contract digest。Editor 在 `execute` 前使用同一
套 `UEWorkflowCore` 重新规划，digest 不一致时拒绝写入。

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

## CLI

```text
ue-workflow --help|--version [--json]
ue-workflow doctor [--connect] --json
ue-workflow help composable [blueprint|widget|material] --json
ue-workflow help operation <type> --json
ue-workflow validate --file <workflow.json|->
ue-workflow plan --file <workflow.json|->
ue-workflow execute --file <workflow.json|-> --approve-plan <digest>
                    --receipt <path> [--save-on-success] [--confirm-write]
ue-workflow resume|status|rollback --receipt <path>
ue-workflow operation run <type> ...
ue-workflow shell
```

机器可读结果写 stdout；连接进度和日志写 stderr。`validate`、`plan` 和 help
可离线使用。`execute`、run 状态与 rollback 需要正在运行的 Unreal Editor。

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
  "confirmWrite": false
}
```

`ue_workflow` 支持 `validate`、`plan`、`execute`、`resume`、`status` 和
`rollback`。MCP 只接收内联 JSON object，不接收本地文件路径。`rollback`
还需要原 run 的 `runId` 和已审批 digest。

v1 的 Editor 执行是同步的，因此 `resume` 不会重新执行 editStep，也不接受
workflow 或参数修改。它只接受当前 Editor Runtime 内存中已经记录的 `runId`：
对 `completed`、`failed`、`blocked`、`rolledBack` 终态幂等返回原 receipt，
并标记 `resumeMode=terminalReattach`、`reattached=true`、
`resumedExecution=false`。来自其他 Editor 实例或未知的 run 会被拒绝；若遇到
不可安全续跑的非终态记录，则返回 `workflow_resume_not_safe`，不会伪装成已续跑。

HTTP action 返回 `ue.workflow-result.v1`；其中嵌套的
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

run journal 位于项目的 `Saved/UEWorkflow/`。`status` 可以读取 journal；
`resume` 不能借 journal 跨 Editor 实例恢复执行。rollback 只对同一 Editor
实例且仍位于 Undo 栈顶的未保存 run 可用。

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
