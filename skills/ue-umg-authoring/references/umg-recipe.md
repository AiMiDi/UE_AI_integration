# UMG authoring recipe

## Route by operation shape

- Use one `ue_content` call for one atomic property, child, layout, or event
  edit.
- Use `ue_workflow` for a continuous hierarchy/layout batch on one Widget
  Blueprint.
- Do not use Workflow for interactive preview, visual inspection, PIE input,
  or `content.widget.event.ensure_handler`.

## Workflow rules

Use the complete v1 shape below. Every `operation` needs a stable `id`, exact
capability `type`, and `params`. The scope injects the Widget Blueprint path
into supported operations. Typed bindings use JSON Pointer; do not use string
interpolation.

```json
{
  "dsl": "ue.workflow",
  "dslVersion": "1.0",
  "workflowKind": "assetEdit",
  "workflowId": "author-login-widget",
  "scope": {
    "kind": "widgetBlueprint",
    "asset": "/Game/UI/WBP_Login",
    "createIfMissing": false
  },
  "persistence": "dirtyOnly",
  "operations": [
    {
      "id": "addTitle",
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
          "from": "addTitle",
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

Plan first:

```json
{
  "action": "plan",
  "workflow": "<the complete workflow object above>"
}
```

Review the normalized workflow, risk, finalizers, expected diff, and exact
`planDigest`. Execute the byte-for-byte unchanged workflow:

```json
{
  "action": "execute",
  "workflow": "<the same complete workflow object>",
  "approvePlanDigest": "<exact digest returned by plan>",
  "saveOnSuccess": false,
  "confirmWrite": false,
  "detailLevel": "summary"
}
```

Set `saveOnSuccess=true` only after explicit authorization. If the plan
requires confirm-write risk, set `confirmWrite=true`; never infer it from this
example. Let the planner append compile, readback, and diff finalizers.

`content.widget.child.add` accepts the short-operation fields declared by its
live schema and Workflow aliases for class and name. Always inspect
`ue_context` before choosing a form. `content.widget.slot.layout.set` targets a
widget name or a typed target and only supports Canvas Panel slots.
For HorizontalBox, VerticalBox, Grid, UniformGrid, or Overlay parents, discover
and use `content.widget.slot.properties.set` with the matching `slotType`.

## Persistence and verification

Default to `dirtyOnly`. Use save-on-success only after explicit authorization.
After execution:

1. Read `content.widget.hierarchy.get`.
2. Read `content.widget.binding.list`.
3. Run `blueprint.asset.validate`.
4. Read `blueprint.asset.dirty.get`.
5. For visual acceptance, require an existing live PIE `sessionId` and
   `generation`, read `scene.runtime.widget.tree.get`, then capture
   `scene.runtime.viewport.capture` from that exact session and inspect the
   image. Starting or stopping PIE is a separate interactive flow.

Successful creation is not established by returned names alone.
