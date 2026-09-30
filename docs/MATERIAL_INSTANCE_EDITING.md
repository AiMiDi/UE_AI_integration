# 材质实例参数编辑

实例参数有独立入口，不能用材质图参数节点的 CRUD 代替。当前支持 scalar、vector、texture、
switch 的本地覆盖设置/清除，保留 `(name, association, index)` 完整身份。

## 连续编辑

1. 用 CLI live schema 确认 `content.material.instance.parameters.get` 和
   `content.material.instance.parameters.batch` 的当前参数。
2. `parameters.get` 输入实例完整资产路径，返回本地覆盖、继承值、生效值与 `stateHash`。
   `declared:false` 表示失去声明的旧覆盖，它可以清除，不能继续设置。
3. 将返回的哈希传给批处理的 `expectedStateHash`，先 `dryRun:true` 可只检查计划。
4. 默认只标脏，整批最终更新；需要持久化时显式 `save:true`。原生 Undo/Redo 按整批处理。

批处理示例（哈希必须替换为刚读取的值）：

```json
{
  "instance": "/Game/Materials/MI_Example.MI_Example",
  "expectedStateHash": "<parameters.get 返回的 stateHash>",
  "operations": [
    {"op": "set", "name": "Strength", "type": "scalar", "value": 0.8},
    {"op": "set", "name": "Tint", "type": "vector", "value": {"r": 1, "g": 0.5, "b": 0.25, "a": 1}},
    {"op": "set", "name": "EnableDetail", "type": "switch", "value": true},
    {"op": "clear", "name": "Roughness", "type": "scalar", "association": "layer", "index": 1}
  ],
  "save": false
}
```

global 的 index 为 -1；layer/blend 必须指定实际索引。同名参数可存在于不同层，
不能只按名字选第一个。继承读取使用 UE 的层索引映射；声明 GUID 与已有覆盖 GUID
分别记录，避免把上级实例缺失的覆盖 GUID 复制为参数声明身份。

## 刷新、失败与保存

整批先解析到副本，校验类型、参数存在性、有限浮点数、纹理引用及参数身份，再写回。
不循环调用会逐项提交渲染更新的单参数 setter。静态开关仅在需要时请求静态参数更新，
最后发送一次实例 PostEditChange 和一次编辑器刷新；不额外调用强制静态变体刷新的
无参 UpdateStaticPermutation。`finalizeCount` 是工具最终更新次数，不是跨平台 shader
任务数量、渲染线程提交总数或帧耗时。

读回同时核对本地覆盖及生效值。失败恢复参数副本，并返回 `restoreVerified` 和
`mismatches`；未验证恢复成功时不能视为已回滚。整批预校验失败不产生部分参数写入。
保存失败会明确报告已保留内存修改，不能把错误当作“没有修改”直接重试整个批次。
没有变化的批次不刷新，但显式 `save:true` 仍可保存之前的脏内容。

旧 `content.material.instance.parameter.set` 现在复用同一条设置/更新/读回路径，
仍接受标量数值和颜色对象，**默认改为不保存**；要保留旧的持久化效果需传 `save:true`。
连续修改应使用新的批处理入口，避免反复调用旧单项接口。

## 查询与验证边界

- 每次批次 1..128 项，同一完整参数身份不允许重复。查询默认返回 100 项，上限 500；
  后续页传入同一 `expectedStateHash`，状态变化会拒绝继续拼接旧分页结果。
- 状态检查覆盖受支持的参数目录/覆盖/生效值、父链路径与版本、当前 Layer 布局及 GUID。
  它不是完整材质源码或 shader 有效性的证明。
- 参数目录最多 4096 项，父链最多 64 项，Layer/Blend 最多各 256 项，状态文本最多 2 Mi 字符。
  UE 原生参数目录提取发生在分页之前，因此分页并不保证原生遍历耗时固定。
- 本轮不编辑 Layer 堆栈、父材质关系、函数实例、Atlas 曲线参数、双精度向量、虚拟纹理或字体覆盖。
  支持查询和修改 Layer 内的上述四类参数，不等于 Layer 创建/重排已经完成。
- 静态开关修改可能触发 shader 编译；`shaderValidationPerformed:false` 明确表示读回成功
  不能代替 shader 编译成功。当前入口未接入 Workflow materialInstance scope，不能伪装成
  material/materialFunction scope 绕过其资产快照契约。

本轮编译、运行及实际项目 DLL 的验收状态另见 validation 下的实例批处理记录。
