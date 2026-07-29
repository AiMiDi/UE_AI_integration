# Shared engineering contracts

These schemas define the stable result shapes shared by the UE HTTP API,
the MCP bridge, and the standalone `ue <capability>` CLI:

- `ue.job.v1`: durable long-running work and its terminal result.
- `ue.artifact.v1`: bounded artifact metadata with optional on-demand base64 content.
- `ue.finding.v1`: static or runtime-correlated engineering evidence.
- `ue.change-plan.v1`: digest-gated writes that intentionally do not belong to
  the deterministic asset-edit Workflow DSL.
- `canonical-json-vectors.v1`: cross-runtime canonical JSON and SHA-256 golden
  vectors shared by the UE Infrastructure implementation and portable
  `UEWorkflowCore`.

The Editor owns execution and persistence. MCP forwards structured objects
without reinterpreting them, and the CLI can export artifact payloads with
`--output`.
