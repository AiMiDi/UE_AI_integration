# Capability Search and Windows Unicode

## Capability search

Capability manifests may add reader-facing search metadata without changing the
capability ID or input schema:

```json
{
  "search": {
    "title": "Organize Blueprint Graph",
    "keywords": ["layout", "align", "排版"],
    "aliases": ["graph organize", "comment group"]
  }
}
```

`title`, `keywords`, and `aliases` are optional individually, but a `search`
object must contain at least one of them. Arrays must be non-empty and may not
contain duplicate values.

Editor HTTP, MCP local/live discovery, `ue capabilities`, and
`ue-workflow capabilities` share the `ue.capability-search-v1` contract:

- ASCII whitespace, punctuation, `.`, `_`, and `-` separate tokens.
- camelCase and acronym-to-word boundaries also separate tokens.
- Query tokens use AND matching.
- Results rank exact full ID first, then ID segments, title,
  keywords/aliases, and description. Equal scores sort by capability ID.
- A searched result includes `match.score`, `match.matchedFields`, and
  `match.matchedTokens`.

The portable vectors in
`Resources/Contracts/capability-search-v1.json` are the compatibility source
for all implementations.

## Windows text encoding

Both Windows executables use wide-character command-line entry points and
convert arguments to UTF-8 internally. Text read from `--file`,
`--params-file`, receipts, and stdin accepts:

- UTF-8 without BOM;
- UTF-8 with BOM;
- UTF-16LE with BOM;
- UTF-16BE with BOM.

Invalid or unmarked non-UTF-8 input returns `invalid_text_encoding`. Redirected
stdout and stderr are BOM-free UTF-8. Interactive Windows console output is
written through the Unicode console API.

For complex JSON in PowerShell, prefer:

```powershell
ue blueprint layout.organize --params-file .\layout.json
Get-Content -Raw -Encoding utf8 .\layout.json |
  ue blueprint layout.organize --params-file -
```

In Windows PowerShell 5.1, explicitly set the pipeline encoding before piping
non-ASCII text:

```powershell
$OutputEncoding = [Console]::OutputEncoding =
  [System.Text.UTF8Encoding]::new($false)
```

The CLI cannot recover characters that an upstream shell has already replaced
with `?`. `--params <json>` remains available for simple inputs, but is not
recommended for complex PowerShell JSON because shell quoting happens before
the CLI receives the argument.

---

# 能力搜索与 Windows Unicode

Capability manifest 可增加 `search.title`、`search.keywords` 和
`search.aliases`，用于描述面向用户的标题、关键词和别名，不改变能力 ID
或输入 Schema。Editor、MCP、本地 CLI 与在线 CLI 使用同一搜索合同：

- 空白、标点、点、下划线、连字符和 camelCase 边界都会分词。
- 多个查询词按 AND 匹配。
- 排序优先级为完整 ID、ID 段、标题、关键词/别名、描述；同分按 ID
  稳定排序。
- 搜索结果返回 `match.score`、`matchedFields` 和 `matchedTokens`。

Windows 下两个 CLI 都从宽字符命令行转换为内部 UTF-8。文件和标准输入
支持 UTF-8、带 BOM 的 UTF-8，以及带 BOM 的 UTF-16LE/BE；非法编码返回
`invalid_text_encoding`。重定向输出为无 BOM UTF-8。

复杂 PowerShell JSON 推荐使用 `--params-file <path>` 或
`--params-file -`。Windows PowerShell 5.1 必须显式设置
`$OutputEncoding`；如果上游已经把字符替换成 `?`，CLI 无法恢复原字符。
