# Custom HLSL 编辑与诊断

Custom 代码与接口编辑复用 Workflow，支持 `material` 和 `materialFunction` scope。
本功能不把 HLSL 转译成普通节点，不自动猜测或改写 HLSL 算法。

## 能力与编辑顺序

| 任务 | capability |
|---|---|
| 查找 Custom 或参数节点 | `content.material.graph.index` → `graph.nodes.list` |
| 读取 Custom 代码、引脚、宏、include、配置哈希及缺失输入 | `content.material.custom.get` |
| 一次修改 Custom 代码与接口 | `content.material.custom.set` |
| 查询常用参数与默认值、GUID、元数据 | `content.material.parameter.list` |
| 修改参数默认值、名称、分组、排序、说明、Scalar 滑条、Texture sampler | `content.material.parameter.set` |
| 创建或删除 Custom／参数节点 | `content.material.expression.add` / `expression.delete` |
| 连接或断开参数与 Custom | `content.material.pin.connect` / `pin.disconnect` |
| 在打开的编辑器中整批创建、配置、连线并验证回滚 | `content.material.editor.batch` |
| 查询、配置函数调用节点 | `content.material.function.call.get` / `function.call.set` |
| 触发材质编译 | `content.material.validate` |
| 读取编译状态和错误，不重复触发编译 | `content.material.diagnostics.get` |

单次命令使用 `ue-cli help <id> --json` 查看本地 schema，执行前用
`--live-schema` 检查已加载模块。连续修改用 `ue-workflow-cli` prepare/execute，
中间步骤不刷新材质、不编译、不保存；最后统一更新。默认 dirtyOnly，显式选择
save-on-success 才执行最终保存。持久化 checkpoint 自身的保存成本另外计算。
独立调用写接口保留即时更新、保存行为；`dryRun` 和无变化请求不会更新或保存。

针对已打开的材质编辑器，优先使用[显式预览上下文和批次](MATERIAL_EDITOR_PREVIEW.md)。
预览中的普通连续修改不逐次编译、Apply 或保存；读写与诊断都指向同一工作副本。

## Custom 配置契约

省略字段表示保留。`inputs`、`additionalOutputs`、`defines`、`includePaths`
分别替换各自的完整列表，空数组表示清空。可以在一次请求里修改代码和多个列表，
所有参数先校验，再应用变更，避免部分字段已写入后才发现输入格式错误。

```json
{
  "nodeId": "expr:MaterialExpressionCustom_0",
  "code": "Mask = saturate(Strength); return Color * Strength * FACTOR;",
  "outputType": "Float3",
  "inputs": [
    {"name": "Color"},
    {"name": "Strength", "previousName": "Gain"}
  ],
  "additionalOutputs": [{"name": "Mask", "type": "Float1"}],
  "defines": [{"name": "FACTOR", "value": "2.0"}],
  "includePaths": [],
  "disconnectRemoved": false
}
```

上例为 Workflow operation 的 params；独立调用还需要 `material` 或
`materialFunction`，只能指定一个目标。

- 引脚按名称关联，重排不按数组索引迁移连线。重命名使用 `previousName`，
  同时自行修改代码中的引用；工具不进行可能破坏宏、注释、成员访问的文本替换。
- 删除已连接的输入或输出需要 `disconnectRemoved:true`；否则整个操作失败。
  重排或删除附加输出时同步修正消费者及材质 root 的输出索引。
- 新输入初始未连接，由后续同一 Workflow 中的 `pin.connect` 连接。
  Custom 输入类型由上游表达式决定，不存在独立的 inputType 字段；纹理输入需要
  TextureObject/TextureObjectParameter，普通采样结果只是数值。
- 支持 Float1/2/3/4、MaterialAttributes 输出。主返回值是输出 0。
  输入、输出及 UE 生成的 `NameSampler` 名称不能冲突；UE FName 不支持可靠的
  仅大小写改名，本接口拒绝这种修改。
- include 使用已经注册的虚拟 shader 路径，例如 `/Engine/...`、`/Plugin/...`，
  不创建 shader 路径映射，不写入磁盘头文件。路径格式通过不代表文件可解析，
  是否存在和 HLSL 语义由实际材质编译检查。
- `expectedStateHash` 可使用 get/list 返回的值，检测配置变更；它不替代 Workflow
  的资产前置条件。重命名请求重试前需重新读取，旧 previousName 不会被猜测为新名称。
- 单个 Custom 编辑上限：代码 65,536 字符、64 输入、32 附加输出、64 defines、
  32 includes，整体配置最多 240 KiB UTF-8 JSON。超限明确报错。
- 读取既有节点时，未命名的附加输出行返回 `index:-1`；它没有图引脚。
  `configurationIndex` 为属性列表中的位置，`index` 为连线使用的实际输出位置。

## 材质参数

`parameter.set` 支持 ScalarParameter、VectorParameter、StaticBoolParameter /
StaticSwitchParameter，以及 TextureSampleParameter 派生类型（含 TextureObjectParameter）。
它保留参数 GUID；改名不会自动改写蓝图/C++ 中硬编码的参数名。
新增和删除复用 expression.add/delete，在同一 Workflow 中完成配置与连线。

`defaultValue` 按参数类型分别使用 number、`{r,g,b,a}`、boolean、texture asset path。
向量省略的通道保留旧值，空纹理路径清空默认纹理。数值要求有限，纹理类型由 UE
的 `TextureIsValid` 校验。`samplerType` 是原生枚举后缀，例如 `Color`、`Normal`。

查询为当前资产内的声明，支持名称/nodeId 过滤和 offset/limit 分页；修改后重新读取。
每页还受 240 KiB 内容预算限制，继续读取应使用返回的 nextOffset。
它不展开函数、Layer 或 Material Instance override。其他参数类型可被查询，但返回
`editable:false`；实例覆盖值继续使用现有 instance 接口。

## 编译错误处理

推荐流程：读取 → 批量修正 → validate → diagnostics.get → 根据诊断继续修正。
普通 validate 默认不等待，编译中时轮询 diagnostics.get，避免每次查看错误都重编。
validate 会完成该材质的缓存准备并显式提交当前资源的 Shader 任务；即使材质没有
被场景使用，也会请求编译。缓存准备自身可能耗时，非等待模式并非完全无阻塞。
显式 `waitForCompilation:true` 会同步等待当前材质的编译任务，耗时由编译器决定，
不具备固定超时保证，也不会主动等待整个 Editor 的全部 shader 任务。

| 状态 | 含义 |
|---|---|
| pending | 当前资源还在编译，旧错误可能过期 |
| failed | 当前资源返回编译错误 |
| succeeded | 当前资源编译完成且 ShaderMap 完整 |
| unavailable | 没有可用于证明 HLSL 编译的资源，例如 NullRHI |
| stale | 编译之后材质、依赖函数/实例/Layer 或显式 Custom include 内容发生变化 |
| unverified | 依赖无法完整核对、超过预算，或编译时缓存与磁盘内容不一致 |

`valid` 只在编译结果已关联当前源码且完成时返回 true/false，其余为 null。
`sourceState:untracked` 表示本进程没有该资产的 validate 记录，应先 validate。
最多保留 64 条弱引用记录；淘汰后回到 untracked，不保留 UObject。
源码检查按当前材质追踪传递函数依赖、函数实例父链及覆盖参数、Layer/Blend 函数，
并读取 Custom `includePaths` 与代码内字面量 include 的传递内容。手工修改函数也会
使指纹变化，无关函数编辑不再使所有材质失效。延迟 graph 编辑另有按资产记录的修订。

include 每次核对内容，不仅比较时间戳或大小；支持相对路径、注释过滤和续行。
条件分支保守地全部计入，宏展开的 include、未映射/生成头文件或预算不足明确返回
`unverified`、`valid:null`，原因见 `sourceCheckIssues`。`sourceCheckComplete` 仅指
`sourceCheckCoverage` 声明的检查范围；隐式引擎/生成 shader 依赖不在其中，返回
`implicitEngineShaderSourcesChecked:false`，不代表整个引擎编译环境被完整跟踪。

普通 `diagnostics.get` 不编译、不等待、不清缓存。显式 `material.validate`、有宿主的
`function.validate`、`editor.refresh` 和预览批次最终刷新会先核对缓存；已加载的
Custom include 与磁盘不一致或文件不可用时，调用原生 `FlushShaderFileCache` 后
再更新材质，返回 `shaderFileCacheRefreshed:true`。该缓存是引擎级缓存；未变化时
不会反复清理。编译前后再次核对依赖，无法关联当前源时不会把旧成功当成新成功。

预算为 128 个函数、20,000 个表达式、总计 1 Mi 字符 Custom 代码、256 个 include、
单文件 512 KiB/总计 4 MiB include 内容；函数实例单数组最多 4096 项、导出值最多
1 Mi 字符。1024 个资产修订记录超限时保守失效，编译记录仍最多 64 项弱引用。

错误包含原始 compiler message、能解析出的文件/行/列，以及常见缺失输入、
未声明符号、include 错误的修正提示。`location.space:compilerReported` 始终保留
编译器原始位置，不能直接当作 Custom 编辑框行号。

显式验证或预览刷新在提交编译后、等待结果前捕获本次实际编译输入。仅在源码
指纹仍匹配、编译结束且生成函数体与唯一 Custom 代码逐字对应时，返回
`sourceLineMapped:true` 和 `customSourceLocation`（所属资产、节点、表达式路径、
从 1 开始的代码行列）。它支持函数内部 Custom，以及 UE 自动添加的 `return`
前缀；不会修改 Custom 代码或为查询重新转译材质。
`diagnosticSourceMapCurrent` 表示当前错误可使用该次映射。插件观察原生编译完成
事件；后续原生重新编译会将旧映射标为 `native_recompile_since_capture`，即使
节点源码没有变化。再次显式验证可捕获新映射；多个资源/变体完成通知可能保守地
使映射失效，不据此猜测错误仍属于同一次编译。

`sourceMappingState:ambiguous_custom_body` 表示多个节点拥有相同生成函数体，
`customSourceLocations` 最多返回 8 个候选及完整候选数量；不会自动选一个节点。
源码过期、无编译输入、编译器位置位于 include 或包装代码、`#line` 后区域、
含预处理指令或续行的 Custom 都不会冒充精确映射。代码行含 Tab 或非 ASCII 时，
可以定位行但不推断字符列：`columnMapped:false,column:null`。宏展开错误定位到
编译器报告的使用位置，不代表宏定义本身就是错误根因。

此能力使用本项目 Engine/MaterialShared.h 的只读
`GetPendingMaterialCompilerEnvironment_GameThread` 访问器；没有该接口的引擎仍
能编译插件和使用其他编辑功能，但返回 `sourceMapState:compiler_source_unavailable`。
同步编译提前释放输入或不符合已识别包装格式的生成器也可能无法映射，不额外
发起编译重建证据。当前支持的实际生成格式为 `CustomExpressionN` 的 Pixel/Vertex
函数包装；其他生成器需实际输入匹配后才能宣称支持。

映射预算：128 个候选 Custom、512 Ki 字符原始代码、2 Mi 字符生成输入、2048 个
包装扫描项、256 个匹配范围；超限整体放弃映射，避免裁剪后产生虚假的唯一匹配。
最多缓存 64 次不同材质的编译记录；诊断查询只使用保存的映射，不重新构建它。

`errorExpressionCandidates` 是 UE 返回的资产级候选节点，不伪造逐错误一一对应。
诊断有条数和文本长度限制，并返回 truncated。

材质 Workflow finalizer 等待该材质编译；含 Custom 的材质在无法获得实际编译结论时
失败并进入回滚，避免以空错误列表通过保存。未连接到输出的 Custom 可能被 UE 裁剪，
材质编译成功不代表所有游离 Custom 都经过编译。当前目标平台/feature level 的成功
也不代表其他平台、全部变体或画面已验证。

MaterialFunction 单独作为 Workflow scope 时，finalizer 为结构检查及原生更新。
验证其中的 HLSL 可选以下方式：

- 独立调用 `content.material.function.validate`，传入 `materialFunction`、
  `validationMaterial` 和可选 `waitForCompilation:true`。宿主必须从材质输出
  引用该函数，未连接的调用节点不算验证上下文；错误位于 `materialDiagnostics`。
  工具复用原生函数更新准备的宿主资源，不再对宿主重复 PostEditChange，不保存资产。
- 宿主路径检查支持命名重路由，并沿实际选中的函数输出进入函数体；遇到函数输入时
  才返回对应调用点的连线。未被该输出使用的调用输入不算宿主引用。同一个函数的
  多次调用保留独立输入上下文；已连接的调用输入覆盖默认预览表达式，未连接时仅在
  `bUsePreviewValueAsDefault` 开启时追踪默认表达式。超过 1024 个调用上下文或 50,000
  次检查步骤时拒绝验证并说明未验证原因，不将预算耗尽当作成功。
- 在 Workflow v2 中同时声明 function 和 host material scope。先更新所有函数，
  再编译宿主，最后读取与比较；失败时两者一起回滚。含函数内 Custom 的宿主在
  NullRHI 下也不会被当作已通过 Shader 验证。

验证覆盖实际宿主和当前 feature level，不代表每个函数输出、静态分支或变体都经过
编译。工具不会猜测输入类型并临时构造宿主材质。完整创建示例见
[函数与宿主 Workflow](../Workflow/tests/fixtures/material-function-host.v2.workflow.json)。
示例的材质 root 引脚采用英文显示名；不同本地化环境先查询图中的实际引脚名。

`errorExpressions` 为原生编译器返回的候选节点附带 `assetPath`、`nodeId`、
`expressionPath`，可继续对该资产调用 Custom/GraphIR 查询。它仍不是逐错误的精确
映射；部分 DXC 错误没有对应的原生表达式候选。

## 函数调用节点

先用 `expression.add` 创建 `MaterialFunctionCall`，再用 `function.call.set`
指定 `function` 资产路径。`material` / `materialFunction` 是节点的所属资产；
`function` 是被调用的资产，二者含义不同。

同一函数重设路径会刷新接口并按 FunctionInput/Output GUID 保持连线；更换函数时
按名称匹配。清空引用用空字符串，删除已连接接口需 `disconnectRemoved:true`。
支持 dry-run、无变化跳过、expectedStateHash，以及 Workflow 中延迟更新/保存。
同一批次中，配置引用后再依赖该步骤连接引脚，避免调用接口尚未建立时连接。

只配置普通 MaterialFunction，最多 128 输入/输出；Material Layers 使用不同接口。
函数引用检查按普通调用边迭代遍历，预算 128 个函数、50,000 表达式，拒绝循环和
超限，不进行全项目搜索。它不展开 Layer 栈的隐式依赖。

默认编辑持久化资产的 authored expression。Custom、参数和函数调用也支持显式
`targetContext:"editorPreview"`，在已打开的编辑器中连续修改并最后刷新一次；
见[预览编辑流程与覆盖边界](MATERIAL_EDITOR_PREVIEW.md)。读写和验证必须针对
同一上下文，函数预览的基础材质资源不能证明所有函数输出 HLSL 有效。
