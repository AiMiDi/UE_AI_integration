# Capability schema v3 migration

Version 1.0 switches the public capability contract directly to schema v3.
There is no compatibility alias for the removed `traits.readOnly` field.

## Effects replace readOnly

Every descriptor now declares four independent effects:

```json
{
  "effects": {
    "asset": "none|read|write",
    "world": "none|read|write",
    "editorSession": "none|read|write",
    "external": "none|read|write"
  }
}
```

This makes session-only actions explicit. For example,
`blueprint.selection.set` reads the asset but writes Editor session state.

## Lifecycle and canonical discovery

Descriptors declare `lifecycle.status`, `lifecycle.since`,
`lifecycle.canonicalId`, and optional `lifecycle.replacement`. Discovery folds
deprecated aliases by default; an exact capability ID query still returns the
deprecated descriptor. Removed IDs return `capability_removed` with a stable
replacement and are not executable.

`blueprint.node.comment.set` was removed. Migrate to:

- `blueprint.comment.title.set`
- `blueprint.comment.bubble.set`
- `blueprint.comment.bounds.set`

Blueprint asset state no longer returns `dirty` or `status`. Read
`packageDirty`, `compileStatus`, `needsCompile`, and
`generatedClassUpToDate` instead. Graph capture no longer inlines Base64 by
default; use the returned artifact descriptor and an explicit artifact read.

## CLI and MCP filters

Use `--effect`, `--lifecycle`, and `--canonical-only` with `ue capabilities`.
The `ue_capabilities` and `ue_context` MCP tools accept the equivalent fields.
`ue mcp surface-status` compares manifests, local backends, live Editor
availability, lifecycle state, and handler bindings.
