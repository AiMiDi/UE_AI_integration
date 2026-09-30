# Preview batches

1. Query `content.material.editor.context.get` with the original `material` or
   `materialFunction` path. Retain `previewId`, graph `stateHash` and existing
   unapplied-change state. Use paged context results or GraphIR for large graphs.
2. Read the needed Custom/parameter/call configuration in that same preview.
   Set `targetContext:"editorPreview"`; node writes require `expectedPreviewId`.
   A node setter's state hash is its node configuration, while editor.batch's
   expectedStateHash is the whole preview graph. Do not interchange them with
   a GraphIR projection hash or an Apply content hash.
3. Submit up to 128 operations to `content.material.editor.batch` using the
   original asset path, expectedPreviewId and current graph expectedStateHash.
   Operations use `{id, capability, params}`. `$id` may reference an earlier
   expression.add in nodeId/sourceNodeId/targetNodeId. Do not put asset/context
   selectors or internal execution metadata inside child params.
4. Default `refresh:true` updates at the end. Adjacent deletions are coalesced,
   and default batch refresh defers their intermediate preview compilation.
   `refresh:false` lets ordinary edits accumulate, but preserves native deletion
   refresh behavior. Do not promise zero intermediate updates for that mode.
5. Use `requireValidShader:true` when a material batch must roll back on shader
   failure. Leave it false when intentionally retaining broken code to inspect
   diagnostics. Function previews do not support requireValidShader; their base
   preview is not an actual-host proof. Read diagnostics after the batch; poll
   pending compilation without resubmitting the edit or triggering refresh.

Standalone preview changes can be accumulated and followed by one editor.refresh;
prefer editor.batch when one Undo/rollback boundary is wanted. Refresh does not
Apply or save. Review `success`, `status`, `rollbackVerified` and any explicit
rollback error instead of treating transport `ok` as edit success.

For Custom interface edits, read custom.get, retain unaffected fields, and send
complete desired arrays for inputs/additionalOutputs/defines/includePaths.
Use previousName when renaming connected pins; removing connected pins requires
disconnectRemoved:true. Do not silently drop wires to make a validation pass.

## Accepting preview content

Use editor.apply.prepare to inspect the current preview-to-asset difference and
retain its receiptId. It is read-only and does not authorize or perform Apply.
After native Apply occurs, editor.apply.verify checks assetMatchesPreparedPreview.
The receipt expires after 300 seconds and is Editor-process-local. Re-prepare if
expired; do not claim disk persistence from content equality. A later preview
edit can coexist with an asset that matches the earlier prepared preview.
There is no automatic Apply capability; do not emulate it with blind asset
writes, a fake receipt, or forced modal-dialog acceptance.
