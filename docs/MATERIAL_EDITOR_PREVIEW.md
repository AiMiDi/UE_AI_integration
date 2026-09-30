# 在已打开的材质编辑器中编辑 Custom 和节点图

UE 原生 Material Editor 使用独立预览副本，Apply 时才把它复制回资产。外部资产
修改通知不会把 authored expressions 同步到这份预览。因此读、改、编译必须明确
针对同一上下文，不能从资产读取后假定当前编辑器已经同步。

## 连续编辑

连续操作优先使用下文的 `editor.batch`，在一个 Undo/回滚边界内创建、配置和连线，
并在末尾刷新一次。插件的 `ue-material-editing` Skill 提供 `edit-preview` 配方；
下列独立调用顺序适用于分次累积编辑，不需要在已刷新成功的 batch 后再刷新一次。

1. `content.material.editor.context.get`：传入持久化 `material` 或
   `materialFunction` 路径，获得 `previewId`、图 `stateHash`、`hasUnappliedChanges` 和节点分页。
   没有打开编辑器时返回 `editorOpen:false`；查询不会自行打开它。
2. 在 `custom.get/set`、`parameter.list/set`、`function.call.get/set`，以及
   `expression.add/delete/move/value.set`、`pin.connect/disconnect`、
   `function.interface.set` 中传入
   `targetContext:"editorPreview"`。写入还必须提供
   `expectedPreviewId:<上一步 previewId>`。节点仍用 `expr:<object name>`，可继续
   使用 `expectedStateHash` 防止同一节点被并发修改。
3. 连续执行所需修改。每个成功变更建立一次原生撤销事务，保留现有的原子校验和
   保线规则。普通修改延迟基础预览编译，不 Apply 或保存。删除节点使用原生编辑器
   删除入口以清除选中节点预览，会额外刷新并报告 `nativeEditorRefresh:true`。
4. 最后调用 `content.material.editor.refresh`，传原资产路径、
   `expectedPreviewId`，可选 `waitForCompilation`（默认 true）。工具调用一次原生
   编辑器刷新，更新图、Details 和基础预览资源，不改变 Live Preview 开关。开关
   关闭时也显式编译基础预览一次。此操作不会 Apply 或保存。
5. 使用相同预览上下文读取 `diagnostics.get`。错误时继续修改、刷新；完成后调用
   `content.material.editor.apply.prepare` 审阅差异，使用编辑器原生 Apply 接受修改，
   再用 `content.material.editor.apply.verify` 核对资产内容，最后按需要保存。

## Apply 前后内容核验

`apply.prepare` 是只读查询，接受原资产的 `material` 或 `materialFunction`、当前
`expectedPreviewId` 和可选 `limit`（1..100，默认 50）。返回 `receiptId`、资产
`assetContentHash`、预览 `expectedContentHash`、完整 `changeCount` 和有界差异。
回执在当前 Editor 进程保留 300 秒，最多 64 条；旧回执可能因容量而被淘汰。
这些内容哈希属于 Apply 比较投影，不能作为节点 setter 或 batch 的 expectedStateHash。

比较范围是可编辑属性、带类型的对象引用、普通表达式/注释、连接、材质根输入及
其常量，以及节点位置和注释尺寸。预览图尚未同步到表达式的位置/注释文本按图中
当前值审阅。只归一化对象引用；HLSL 和普通字符串中的路径原文不被替换。
这不是完整 UObject 字节快照，也不比较 transient 状态、磁盘包或全部派生数据。
引用到其他资产时比较引用身份，不快照被引用资产的内容；依赖内容变化仍由材质
诊断的新鲜度检查和实际宿主验证处理。
复合图或无法完整捕获上述范围时拒绝生成回执，避免把部分比较冒充完整比较。

原生 Apply 后，使用 `apply.verify` 并传 `receiptId`。检查
`assetMatchesPreparedPreview`，不要把传输成功当作内容一致。它同时返回
`assetChangedSincePrepare`、`previewStillMatchesPreparedContent`、`samePreviewSession`
和 `hasUnappliedChanges`（编辑器仍打开时）：Apply 后继续编辑预览，不会让已应用
到资产的上一批内容被误报为失败。资产之后被修改则会返回内容不匹配。

回执核验只读取已加载资产，不为旧回执重新加载资产或触发 PostLoad；原对象卸载、
回执失效或投影超预算时返回明确错误。它不触发编译、Apply 或保存；返回
`nativeApplyExecutionProven:false` 和 `diskPersistenceVerified:false`，因为内容一致
本身不能证明哪次按钮操作执行过，也不能证明文件已落盘。Shader 结论仍由对应
上下文的 refresh/validate/diagnostics 提供。

预算：20,000 表达式（含注释）、100,000 属性值访问、16 层嵌套、单容器最多
20,000 项、原始属性文本共 2 Mi 字符、规范化比较文本最多 4 Mi 字符。差异值
前后各显示最多 256 字符，并标记 `valueTruncated`；限制显示条数不影响内容哈希。

**自动执行 Apply 尚未实现。** 原生 Apply 可能检查其他平台/派生实例并打开模态
窗口，还会刷新 shader 文件缓存与材质。上述回执没有绕过这些行为，也不是写入
许可或自动提交承诺；后续自动入口仍需接入原生错误/交互检查。

关闭/重开编辑器后旧 previewId 失效；最多跟踪 64 个弱引用身份，身份淘汰后也需
重新查询。身份校验与节点 stateHash 是不同约束。节点分页是实时编辑器状态，
不是不可变 GraphIR 快照；并发增删节点时应重新查询。大图使用 `graph.index`，传入
原资产 `assetPath`、`targetContext:"editorPreview"` 和可选 `expectedPreviewId`；其
快照直接读取工作副本节点集合，返回相同预览身份。后续分页、子图查询继续使用
`snapshotId`；修改后的重新捕获不影响旧快照，旧快照也不会自动变为最新状态。

## 一个批次完成创建、配置和连线

`content.material.editor.batch` 把最多 128 个操作放入一个原生撤销事务，默认末尾
刷新。示例中的资产路径、previewId、stateHash 和目标引脚需用当前查询结果替换：

```json
{
  "material": "/Game/Materials/M_Custom",
  "expectedPreviewId": "<editor.context.get 的 previewId>",
  "expectedStateHash": "<editor.context.get 的 stateHash>",
  "refresh": true,
  "requireValidShader": true,
  "operations": [
    {"id":"gain","capability":"content.material.expression.add","params":{"expressionClass":"ScalarParameter"}},
    {"id":"gainValue","capability":"content.material.parameter.set","params":{"nodeId":"$gain","name":"Strength","defaultValue":0.5}},
    {"id":"custom","capability":"content.material.expression.add","params":{"expressionClass":"Custom"}},
    {"id":"code","capability":"content.material.custom.set","params":{"nodeId":"$custom","code":"return Strength * float3(0.2,0.4,0.6);","outputType":"Float3","inputs":[{"name":"Strength"}]}},
    {"id":"input","capability":"content.material.pin.connect","params":{"sourceNodeId":"$gain","sourcePinName":"Output","targetNodeId":"$custom","targetPinName":"Strength"}},
    {"id":"output","capability":"content.material.pin.connect","params":{"sourceNodeId":"$custom","sourcePinName":"Output","targetNodeId":"root","targetPinName":"Emissive Color"}}
  ]
}
```

- 全部操作的公开 schema 在写入前校验；`params` 不得覆盖资产、预览身份或内部
  执行元数据。`$id` 只能引用之前的 `expression.add`，用于三个 nodeId 字段。
- `expectedStateHash` 在批次入口校验整个预览图；单节点 `custom.set` 等接口中的
  同名字段仍表示该节点配置哈希。两者不可混用。
- 运行中失败会撤销当前批次，并比较操作前后的图结构哈希。验证恢复后返回
  `success:false,status:"rolledBack",rollbackVerified:true`，调用方必须检查这些
  业务状态，不能把传输成功当作编辑成功。无法验证恢复返回
  `preview_batch_rollback_failed`，需要重新查询现场。
- `requireValidShader:true` 要求普通材质在最终刷新后编译成功，否则回滚；默认
  false 允许保留待修正代码并查看诊断。函数预览不支持此选项，需要实际宿主验证。
- 相邻 `expression.delete` 合并为一次原生删除；不会越过其他操作重排，重复删除
  同一节点在整组写入前拒绝。每个 operation 保留回执，新增 `deletionGroupId`
  和 `deletionGroupSize`；每组仅最后一条回执标记 `nativeEditorRefresh:true`。
- 默认 `refresh:true`：含删除的批次临时暂停原先开启的 Live Preview。完成图和
  参数面板更新后恢复原开关，并进行一次基础预览更新；失败时先撤销再恢复开关。
  原开关关闭时保持关闭，最终显式更新一次。中间穿插代码/参数修改也不逐次编译。
- `refresh:false` 可继续积累普通修改；相邻删除仍合并，但保留原生 Live Preview
  刷新行为。`nativeDeletionRefreshes` 表示原生删除调用数，
  `nativeDeletionCompileDeferred` 表示批次是否临时暂停了删除编译，
  `livePreviewRestored` 表示开关恢复结果；调用数不是基础预览编译次数。
- 以上基础预览更新次数不包含所有 shader job、节点缩略图或失败回滚所需更新。
  不保存、不 Apply 的约束保持不变。
- 成功批次支持一次 Undo/Redo；失败后只撤销可确认属于该批次的记录。恢复范围
  是预览图结构，编辑器 dirty 标志、选择和节点预览显示不承诺完全恢复。
- 请求上限 512 KiB UTF-8 JSON、128 操作、20,000 表达式；活动原生事务期间拒绝
  新批次。该能力仅供当前编辑器会话，不提供跨进程 checkpoint。

## 已支持的边界

- 预览上下文覆盖既有 Custom 代码、输入/输出接口、宏、include 配置，以及既有
  参数默认值/元数据、函数调用引用。原子替换、保线、断线保护仍适用。
- 支持普通表达式新增、删除、移动、常量值修改、直接连线及断线；Custom/参数值
  修改复用已有严格 setter。预览断线的 `index:N` 可用 `direction:input/output`
  消除输入/输出索引歧义；资产上下文拒绝该新增字段。
- Composite、PinBase、Comment 不支持通过此入口新增；删除拒绝复合节点和函数
  Input/Output 接口节点，后者的原生删除需要确认对话框。函数接口新增和配置
  已支持。函数实例、Layer、隐式跨图依赖不由本功能覆盖。
- 普通材质可读取基础预览资源的真实 HLSL 编译结果。只覆盖所报告的 feature level
  和静态配置；选中表达式的独立预览资源不是全部输出的验证证明。
- 函数编辑器也能编辑其工作副本。原生引擎可能另建选中输出的着色资源，该资源
  未通过当前公共接口暴露，因此函数基础预览的 `valid` 保持 null，
  `shaderValidationPerformed:false`。Apply 后用 `function.validate` 的真实
  `validationMaterial` 宿主获得编译结论。
- 普通函数调用配置及刷新会检查预览在 Apply 后是否对原函数形成递归依赖。
- 默认 `targetContext:"asset"` 保持现有资产编辑行为。预览写入拒绝资产 Workflow
  上下文，防止操作绕过它只覆盖持久化资产的 checkpoint；跨操作预览事务使用
  上述 `editor.batch`。不要在同一操作序列中混用两种编辑上下文。
- 本次没有提供自动 Apply 命令。原生 Apply 的编译警告、多平台统计、对象替换、
  用户已有未应用更改需要独立的接受与恢复协议；当前由原生编辑器完成接受。

真实 TA 大材质的操作耗时、项目 SM6 和画面验收仍单独跟踪。
