# Blueprint BuildGraph recipe

## Ownership

Each generated node has a stable `ref`. The plugin stores the
`buildId + ref -> NodeGuid` mapping in Blueprint package metadata. `merge`
creates or updates managed nodes and preserves every unmanaged node.
`replaceManaged` may remove only nodes previously owned by the same build ID.
Treat `buildgraph_managed_node_conflict` as a blocking ownership conflict.

## Planning

Call `blueprint.graph.build.validate` before planning. A definition is limited
to one Blueprint and one Graph, 128 nodes, and 256 connections. Node types must
be supported by the current `blueprint.node.add` schema.

`blueprint.graph.build.plan` compiles the definition into normalized Workflow
v2 operations. Retain the returned graph hash, managed-ref mapping, Workflow,
and Workflow plan digest. Do not alter the Workflow after approval.

## Execution and recovery

Execute only through `ue_workflow`. BuildGraph deliberately has no separate
execute, rollback, transaction, or journal system. The Workflow owns the
single transaction, deferred compile, read-back, durable checkpoints, resume,
and rollback. After a process interruption, continue by the original `runId`;
do not re-plan against unknown partial state.

## Verification

After execution:

- compile validate the Blueprint;
- compare Graph snapshots and hashes;
- verify that every declared ref resolves to the intended GUID;
- verify unmanaged nodes were preserved;
- run layout validation;
- capture and compare the Graph only when a rendered Graph Editor is present.

An idempotent replay should produce no additional nodes, connections, or
comments. Unexpected structural changes require Workflow rollback.
