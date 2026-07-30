# Blueprint Graph 可信排版与视觉证据

0.6.1 将 Blueprint Graph 的编辑边界从“单个节点坐标命令”扩展为可搜索、可预览、
可审批和可验证的排版闭环。它借鉴批量 Graph authoring 的使用体验，但不绕过
manifest、plan digest、Workflow、结构 Diff 或 rollback 合同。

## 发现与 Unicode

Capability manifest 可用 `search.title`、`search.keywords` 和
`search.aliases` 描述用户可见的名称。Editor、MCP、`ue capabilities` 与
`ue-workflow capabilities` 使用同一 tokenization、AND 匹配、稳定排序和
`match.score/matchedFields/matchedTokens` 结果合同。因此 `layout`、`align`、
`comment group`、`graph organize` 与“排版/分组/注释框”等查询能定位同一能力。

Windows 的 `ue` 与 `ue-workflow` 使用宽字符 argv，并把文件、stdin 和重定向
输出统一到无 BOM UTF-8；输入还接受带 BOM 的 UTF-8、UTF-16LE 和 UTF-16BE。
复杂 PowerShell JSON 推荐 `--params-file <path>` 或 `--params-file -`。Windows
PowerShell 5.1 必须先设置 `$OutputEncoding`；上游已替换为 `?` 的字符无法恢复。
完整说明见 [Capability Search and Windows Unicode](CAPABILITY_SEARCH_AND_UNICODE.md)。

## 几何、诊断与排版

`blueprint.graph.get` 支持 `geometryMode=auto|stored|editor`。每个节点都会返回
`bounds`（位置、尺寸、来源与精确性）；Comment 会返回真实 bounds、完整包含的
节点和相交节点。Graph 级别的 `geometryStatus=exact|partial|storedOnly` 明确说明
Slate 几何是否可用，绝不把缺失尺寸伪装成精确结果。显式 `editor` 模式在没有
目标 `SGraphEditor` 时返回 `graph_geometry_unavailable`。

`blueprint.layout.validate` 基于几何报告节点重叠、水平/垂直间距不足、Comment
包围/Padding 问题与 Comment 相互覆盖。它只验证视觉布局；节点身份、pin 和边的
正确性仍须使用 Graph Snapshot/Diff。

`blueprint.layout.organize` 将多个 group 的 `align`、`distribute`、
`straighten` 和 Comment 创建合并到一个操作：

1. 先用 `dryRun=true` 生成预测坐标、Comment bounds、碰撞/警告、当前
   `expectedGraphHash`、`geometryFingerprint`、`predictedLayoutHash` 和审批
   `planDigest`；digest 绑定规范化坐标、Comment bounds、诊断与完整几何指纹。
   dry-run 不修改资产、Dirty、选择或 Undo 栈。
2. 应用时带回当前 hash 与已审批 digest。每个节点只能属于一个 group，数量受限。
   即使操作位于已审批 Workflow 内，这两个字段仍必须显式存在；外层 Workflow
   digest 不替代针对具体 Graph 坐标的排版审批。
3. Runtime 用相同的 transient/off-screen `SGraphEditor` 原生
   align/distribute/straighten 命令重建预测并重新校验 digest，然后在一个
   `FScopedTransaction` 中应用已审批的精确坐标和原生 Comment bounds。它公开
   `native-sgraph-editor-v1` 语义，并通过几何指纹锁定预测环境。
4. 预测与应用后的诊断都会把受影响节点/Comment 和 Graph 内所有未分组障碍交叉
   检查。外部节点重叠、Comment 部分相交等错误会恢复完整结构快照并验证 hash；
   成功结果返回位置变更、Comment GUID、布局诊断及 Mutation 证据。

该操作是 Workflow `editStep`。直接调用失败会自动回滚；成功后的显式 rollback
继续使用已有 `ue_workflow rollback`，不会发明第二套 layout token。单步
selection/align/distribute/straighten/comment 能力仍面向人工微调保留。

## 视觉 Capture 与 Compare

- `blueprint.graph.capture`：对 `currentView`、`all` 或指定节点截图，支持
  padding、输出尺寸与 PNG/JPEG；视觉比较固定使用无损 PNG。
- `blueprint.graph.capture.get`：读取 capture metadata 与 image artifact。
- `blueprint.graph.visual.compare`：比较两个 capture，返回 changed-pixel ratio、
  平均差异、变化区域、兼容性结论和 diff PNG。

Capture 会临时调整目标 `SGraphEditor` 的选择和 View Transform，等待 Slate 完成
布局后截图，并恢复原选择、View Location 和 Zoom。metadata 包含 `captureId`、
Graph hash、节点范围、DPI、主题、UE/插件版本与 View Transform。主题、DPI 或
窗口尺寸等渲染指纹不兼容时，compare 返回 `inconclusive`，而不是把环境变化当作
布局回归。无目标 Graph Editor 的自动化/无头环境必须返回失败或不可用，不能退回
Level Viewport 或桌面截图。

`currentView` 的 Pan/Zoom 属于兼容性指纹；`all/nodes` 使用确定性内容 framing，
实际 View Transform 只作为证据 metadata，避免节点移动本身被误判成环境不兼容。
目标范围在 Graph Editor 最小 Zoom 下仍无法完整容纳时返回
`graph_capture_clipped`。Pixel compare 只接受两张 PNG；JPEG 仍可用于人工查看，
但比较结论固定为 `inconclusive`。

`captureProvider` 会记录实际渲染路径：标准引擎优先使用 `slateLdr`；若定制引擎
关闭了 LDR Slate 截图钩子，则使用 `slateHdrFallback`；前两者均不可用时才使用
仍绑定同一 `SGraphEditor` 的 `widgetRendererFallback` 离屏渲染。Provider 进入
render fingerprint，不同路径之间的比较返回 `inconclusive`。

## 推荐 Skill 闭环

`ue-blueprint-graph-organize` Skill 的固定顺序是：

```text
search -> graph.get/snapshot -> organize dry-run -> approve -> Workflow execute
       -> graph diff -> layout validate -> capture/compare
```

其中结构 Diff 与 layout validation 是互补证据；视觉 capture 只在实际渲染的
目标 Graph Editor 可用时作为最终可视化验收。

---

# Blueprint Graph trustworthy layout and visual evidence

Release 0.6.1 turns Blueprint Graph editing into a discoverable, previewable,
approved, and verifiable layout loop. It keeps the manifest, plan-digest,
Workflow, structural-diff, and rollback contracts rather than treating native
Graph commands as untracked editor automation.

`blueprint.graph.get` reports explicit node/Comment bounds and an honest
`geometryStatus`; `blueprint.layout.validate` checks overlap, spacing, and
Comment containment; and `blueprint.layout.organize` performs a bounded,
single-transaction group layout after a no-mutation dry-run and approved
digest. Its digest binds normalized predicted coordinates, Comment bounds,
cross-graph diagnostics, and an exact geometry fingerprint. Apply rebuilds the
same deterministic UE 5.3-compatible prediction before mutation and then
applies those approved coordinates. Structural correctness remains the
responsibility of Graph Snapshot/Diff.

`blueprint.graph.capture`, `.capture.get`, and `.visual.compare` provide
targeted `SGraphEditor` PNG evidence. Each capture records graph hash, node
range, DPI, theme, UE/plugin versions, and view transform. Incompatible render
fingerprints produce `inconclusive`; capture never falls back to a Level
Viewport or desktop screenshot.

`captureProvider` records `slateLdr`, `slateHdrFallback`, or the final
same-widget `widgetRendererFallback`. The provider participates in the render
fingerprint, so captures produced by different rendering paths are
inconclusive rather than false layout regressions.
