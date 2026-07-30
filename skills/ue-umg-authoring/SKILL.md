---
name: ue-umg-authoring
description: Plan, author, and verify Unreal Widget Blueprints with hierarchy, layout, property, binding, compile, dirty-state, and readback evidence. Use when creating or modifying UMG UI, arranging widgets, changing slot layout, binding events, or reviewing whether a UI edit actually compiled.
---

# UE UMG Authoring

Use short operations for one atomic change. Use UE Workflow DSL for a
continuous multi-step hierarchy or layout edit so the asset loads once,
compiles once, reads back once, and can roll back as one run.

## Load the recipe

1. Call `ue_skills` with `action=get`, `skill=ue-umg-authoring`, and
   `recipe=author-and-read-back`.
2. Read `references/umg-recipe.md` before constructing a multi-operation
   Workflow; it contains the complete v1 AST and MCP plan/execute envelopes.
3. Establish one Widget Blueprint as the primary scope.

## Discover exact APIs

Call `ue_context` for each capability in the selected recipe. Use
`ue_capabilities` live lookup when widget classes, plugins, or Editor state can
affect availability. Never copy parameter names from prose when the manifest
can provide them.

## Author

1. Read the existing hierarchy, bindings, and dirty state.
2. If the task is one atomic edit, call the corresponding `ue_content`
   operation.
3. If the task adds and lays out multiple widgets, construct a
   `widgetBlueprint` Workflow, plan it, approve the exact digest, and execute
   the unchanged Workflow.
4. Use `content.widget.slot.layout.set` only for a Canvas Panel slot. Use
   `content.widget.slot.properties.set` with the actual `slotType` for
   HorizontalBox, VerticalBox, Grid, UniformGrid, or Overlay parents.
5. Keep persistence `dirtyOnly` unless the user explicitly asks to save.
6. Do not put `content.widget.event.ensure_handler` in Workflow; it is an
   independent atomic operation that compiles and verifies itself.

## See results

Read the hierarchy and bindings again, compile-validate the Blueprint, and
report its dirty state. A returned widget name alone is not success.

For visual acceptance, use a caller-provided live PIE `sessionId` and
`generation`, confirm the runtime widget tree, capture that exact runtime
viewport, and actually inspect the image. This Skill does not silently start
or stop PIE. Do not infer good layout from serialized offsets alone.
