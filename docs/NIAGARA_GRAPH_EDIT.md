# Niagara 计算节点与引脚连接

`content.niagara.graph.edit.*` 在一个批次中创建原生计算节点、设置新节点输入常量并连接引脚。适用于一次接好比较、布尔判断或算术表达式后统一编译的场景。

| 能力 | 行为 |
| --- | --- |
| `content.niagara.graph.operations.list` | 按图查询原生运算 ID、名称、输入输出和默认值；支持 query、offset、limit |
| `content.niagara.graph.edit.plan` | 在临时图副本中创建并校验整组编辑，返回绑定当前图的 planDigest |
| `content.niagara.graph.edit.apply` | 校验批准的计划，创建节点、连线、编译 System 并读回；失败则恢复本次新增内容 |
| `content.niagara.graph.edit.rollback` | 同一 Editor 内使用 receiptId 回滚；图发生变化时拒绝覆盖后续编辑 |

调用前读取 `ue-cli help <id> --live-schema --json`。本能力使用自己的 plan/receipt 协议，`dsl.admission=none`；不作为普通 Workflow editStep 组合。

## 调用顺序

先用 `content.niagara.graph.inspect` 取得 system、完整 graph 路径和已有 nodeGuid/pinId，再查询 `operations.list`。节点名称及运算 ID 来自当前 Niagara schema，不由客户端推测。

以下是计划参数示例，替换 system 和 graph 路径后通过 `--params-file` 提交：

```json
{
  "system": "/Game/Effects/NS_Example.NS_Example",
  "graph": "/Game/Effects/NS_Example.NS_Example:ExampleModule.NiagaraScriptSource_0.NiagaraGraph_0",
  "nodes": [
    { "ref": "systemInactive", "operation": "Integer::EnumNEq", "inputs": [{ "pinName": "B", "value": "0" }] },
    { "ref": "emitterInactive", "operation": "Integer::EnumNEq", "inputs": [{ "pinName": "B", "value": "0" }] },
    { "ref": "skip", "operation": "Boolean::LogicOr" }
  ],
  "connections": [
    { "from": { "nodeRef": "systemInactive", "pinName": "Result" }, "to": { "nodeRef": "skip", "pinName": "A" } },
    { "from": { "nodeRef": "emitterInactive", "pinName": "Result" }, "to": { "nodeRef": "skip", "pinName": "B" } }
  ]
}
```

这只是表达式子图示例，尚未连接输入状态或最终使用者。可在同一 `connections` 中用 `nodeGuid` 引用已有 Parameter Map Get 的状态输出及目标 `SkipRemove` 输入。引脚按 `pinName` 或 `pinId` 二选一定位；有多个同名引脚时必须使用 pinId。`nodeRef` 只在当前批次内有效，成功回执给出实际 nodeGuid、nodePath 和引脚信息。

此引擎的 `ENiagaraExecutionState::Active` 为 0；迁移引擎后需核对枚举。上例使用 Enum Not Equal 的原生整数输入表达该值。输入的 `type: "int"` / `"float"` 用于原生数值引脚特化；显式常量支持 bool、int32 和有限 float。

向同一份计划参数增加 `approvePlanDigest`、`confirmWrite: true` 和唯一 `requestId`，调用 `edit.apply`。成功后用 graph.inspect 读回图。相同请求可重试；同一 requestId 携带不同编辑、图已变化或已回滚时不会再次执行。

回滚参数：

```json
{ "rollbackId": "<apply 返回的 receiptId>", "requestId": "<原 apply requestId>", "confirmWrite": true }
```

## 边界

- 只编辑 `/Game/` Niagara System 所有的图，不修改外部共享 Module 资产。
- 每批最多 32 个计算节点、128 条连接；可只创建节点或只连接引脚，两个数组均须传入。
- 只增加连接，不替换已有输入连线，不隐式创建转换节点。环路、类型不兼容和错误方向由原生 schema 拒绝。
- 动态 Add、wildcard、static 引脚不在本接口范围内。已有 generic numeric 引脚需要先特化，防止连接时重建原节点。
- 本版创建 `NiagaraNodeOp` 运算节点。Parameter Map Get、模块插入和任意脚本/自定义 HLSL 创建不属于此接口。
- plan 不编译或保存原资产。apply 完成整组编辑后编译 System；成功保持 dirty，保存由后续明确操作处理。
- 回滚使用本次新增节点/实际创建连接的日志及图 ChangeID/结构指纹；不会恢复整份资产覆盖用户修改。最多保留 256 个会话回执。
- 编译和图读回通过只证明资产结构与编译结果。传送唤醒、首帧生成、休眠成本及 GSDF 碰撞需要独立运行验证。

原生测试入口：`Automation RunTests UE_AI_integration.Niagara.GraphEdit`。构建及实际验证结果见 `docs/validation/niagara-graph-edit-20260914.md`。
