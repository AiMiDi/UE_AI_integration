# UE_AI_integration

[简体中文](README.md) | [English](README_EN.md)

`UE_AI_integration` is an MCP integration plugin for Unreal Editor. It combines
a single Editor Module with a TypeScript stdio bridge so MCP clients such as
Codex CLI and Claude Code can inspect or modify Blueprints, scenes, content
assets, animation, AI, and production workflows.

The current plugin version is `0.6.0`. Unreal Engine 5.3 is the verified build
baseline. Differences for UE 5.4–5.7 are isolated in the compatibility layer,
but those versions have not all been compiled locally.

## Highlights

- The current release snapshot contains 317 manifest-driven Editor and PIE
  runtime capabilities; the service derives the count from the manifests at
  startup.
- Eleven stable MCP tools instead of exposing every capability as a tool.
- Six domain routers: Blueprint, Scene, Content, Animation, AI, and Production.
- Dedicated PIE lifecycle, runtime object/widget/delegate, real input, and
  scenario operations.
- Blueprint and UMG mutations report compile, save, reload, and readback evidence.
- A consistent HTTP envelope, status-code policy, and parameter error model.
- Query, command, and validation layers, with all UObject operations queued onto
  the Game Thread.
- The MCP bridge only connects to an already-running Unreal Editor. It never
  starts or terminates the Editor.
- The server listens on `127.0.0.1:9847` by default. The Editor, CLI, and MCP
  bridge use the same `UE_PORT` environment override.
- [UE Workflow DSL/CLI](docs/UE_WORKFLOW_DSL.md) keeps the v1 single-asset
  contract and adds v2 workflows with up to 16 named asset scopes, a
  deterministic DAG, durable journals, restart recovery, and all-asset rollback;
  debugging sessions and long-running jobs stay outside Workflow.
- [UE Engineering Copilot](docs/UE_ENGINEERING_COPILOT.md) adds performance and
  trace evidence, automated tests, Blueprint analysis, asset auditing,
  world/rendering diagnostics, and durable production jobs.
- Performance jobs support window or Scenario repeats, explicit measurement
  intervals, bounded TraceServices aggregation, environment fingerprints, and
  structured regression verdicts.
- Blueprint debugging supports PIE sessions, trace cursors, breakpoints,
  watches, and continue/step/abort. Slate pre-tick publishes an immutable pause
  snapshot before pumping the same HTTP listener and consumes only POD control
  commands.
- Runtime evidence includes multi-point pointer sequences, explicit coordinate
  spaces and target-hit checks, Tick-driven predicate waits, and PIE viewport
  capture strictly bound to `sessionId + generation`. Capture fails when the
  requested live PIE window cannot be resolved and never falls back to a
  desktop screenshot.
- `content.widget.event.ensure_handler` creates or repairs an exact
  delegate-signature UMG handler, event node, execution edge, and generated
  dynamic binding, then verifies the compiled result.
- Workflow returns compact summaries by default; readback, diff, and structure
  snapshots are retrieved as explicit sections.
- Capability discovery supports search, trait filters, and pagination, with at
  most 25 summaries returned by default.
- The [short-operation CLI](docs/UE_SHORT_CLI.md) takes a manifest capability ID
  as its first argument. It uses the local schema for one `/api/execute` call by
  default, supports forced live validation with `--live-schema`, and reuses the
  catalog and connection in `ue shell`; `ue-workflow` contains only DSL commands.

## Architecture

```text
MCP client
    │ stdio
    ▼
TypeScript MCP bridge
    │ HTTP :9847 (/api)
    ▼
UE_AI_integration Editor Module
    ├── Core            manifest, registry, validation, executor
    ├── Transport       HTTP envelope, status codes, Game Thread queue
    ├── Domains         Blueprint, Scene, Content, Animation, AI, Production
    └── Infrastructure  asset lookup, serialization, persistence, compilation,
                        snapshots, PIE lifecycle, UE compatibility
```

`Resources/Capabilities/*.json` is the shared capability metadata source for
the C++ plugin and the TypeScript bridge. MCP routing is driven entirely by the
manifests and does not infer categories from operation names.

| Domain | Count | Scope |
|---|---:|---|
| Blueprint | 77 | Asset lifecycle, graphs, variables, components, call graphs, rule scans, runtime debugging, diff, validation |
| Scene | 79 | Actors, PIE runtime, trusted input/waits/capture, World Partition, Data Layers, HLOD, PCG, rendering diagnostics |
| Content | 78 | Asset query/dependency/audit, safe import/reimport, Static Mesh and Texture settings, materials, Niagara, UMG, and event-handler verification |
| Animation | 19 | Animation Blueprint, state machine, and BlendSpace authoring, inspection, validation, and diff |
| AI | 17 | Behavior Tree and Blackboard authoring, inspection, references, validation, and diff |
| Production | 47 | Durable jobs, trace, performance, tests, cook/package, source control, DDC, and BuildGraph |

## Requirements

- Unreal Engine 5.3–5.7
- Node.js 20 or newer
- A C++ Unreal project, or a prebuilt plugin matching the target engine
- An MCP client with stdio server support

## Install the Unreal Plugin

Place the repository under the project's plugin directory:

```text
YourProject/
└── Plugins/
    └── UE_AI_integration/
        ├── UE_AI_integration.uplugin
        ├── Source/
        ├── Resources/
        └── MCP/
```

Build and test the TypeScript bridge:

```powershell
cd YourProject\Plugins\UE_AI_integration\MCP
npm ci
npm run build
npm test
```

Start Unreal Editor and make sure the plugin is enabled. Once the Editor-side
service is running, check its health:

```powershell
Invoke-RestMethod http://127.0.0.1:9847/api/health
```

The bottom-right Level Editor status entry shows `UE AI · N`. Green means the
service is ready, yellow means manifest/handler registration is degraded, red
means the listener failed, and `Off` means the local HTTP service was disabled
by the user. Clicking it opens a native UE quick menu that can enable or
disable the service locally and shows:

- MCP callers registered with a five-second heartbeat and expired after
  fifteen seconds without one.
- One-shot `ue` and `ue-workflow` invocations, attributed by `invocationId` but
  excluded from the online-connection count.
- Capability, Workflow Run, and Durable Job status, duration, error code, and
  `requestId/runId/jobId`. Request parameters, response bodies, and image data
  are never retained.

Enabled state and port use per-project Editor user configuration; `UE_PORT`
still has highest precedence. Disabling the service clears caller presence
immediately while retaining metadata-only activity for the current Editor
session.

Do not overwrite a loaded plugin DLL. When replacing an existing installation,
close Unreal Editor, replace the plugin directory, and then restart the Editor.

## Configure Codex CLI

Add the following entry to `~/.codex/config.toml`:

```toml
[mcp_servers.ue_ai_integration]
command = 'C:\Program Files\nodejs\node.exe'
args = ['D:\Path\To\YourProject\Plugins\UE_AI_integration\MCP\dist\index.js']

[mcp_servers.ue_ai_integration.env]
UE_PORT = "9847"
```

Restart Codex CLI and inspect the registered server:

```powershell
codex mcp get ue_ai_integration
```

For Claude Code:

```powershell
claude mcp add ue_ai_integration -- node Plugins\UE_AI_integration\MCP\dist\index.js
```

## MCP Tools

The bridge always registers these eleven tools, even while Unreal Editor is
offline:

- `ue_status`
- `ue_capabilities`
- `ue_context`
- `ue_cli`
- `ue_blueprint`
- `ue_scene`
- `ue_content`
- `ue_animation`
- `ue_ai`
- `ue_production`
- `ue_workflow`

`ue_cli` does not contact the Editor. It locates both executables through
`UE_CLI` / `UE_WORKFLOW_CLI`, packaged `CLI/bin`, `PATH`, and development build
directories. The existing workflow locator fields remain stable and a new
`shortCli` result describes `ue`. `scripts/build_plugin.bat` packages both
executables, the Workflow Contracts/Capabilities, and MCP production
dependencies.

All six domain tools accept the same input shape:

```json
{
  "operation": "scene.actor.spawn",
  "requestId": "client-generated-uuid",
  "params": {
    "type": "PointLight",
    "name": "MCP_Light",
    "location": [0, 0, 300]
  }
}
```

Use `ue_capabilities` for paged summaries and `ue_context` for full schemas.
An exact `operation` returns one full descriptor. A domain tool rejects
operations owned by another domain.

The short-operation CLI uses the same capability contract as the six domain MCP
tools:

```powershell
ue blueprint.asset.get --name /Game/UI/WBP_Login
ue scene.actor.spawn --type PointLight --name KeyLight --location '[0,0,300]'
ue production.job.status --job-id job-123
ue shell
```

`ue` does not link WorkflowCore. By default it maps arguments with the packaged
manifests and sends one `/api/execute` request; the Editor performs final
validation. `--live-schema` first fetches the exact Editor descriptor to
diagnose contract drift or force an availability check. `ue shell` loads the
catalog once and reuses an HTTP keep-alive connection, while ordinary commands
still exit immediately. Starting a durable job returns its `jobId` immediately;
the CLI never waits automatically. Deterministic multi-step asset edits remain
in `ue-workflow`.

The Workflow CLI uses the offline/online catalog query contract:

```powershell
ue-workflow capabilities --query debug --domain blueprint --risk interactive --limit 10
ue-workflow capabilities --connect --available-only --domain scene --limit 25
ue-workflow capabilities --connect --operation blueprint.debug.session.get --detail full
```

Without `--connect`, the command reads the manifests packaged with the CLI.
With `--connect`, it forwards filtering, risk, availability, paging, and detail
options to the running Editor. `--available-only` requires a live query because
an offline catalog cannot know which plugins and modules the target project
loads.

### PIE Lifecycle

Call the PIE lifecycle operations through `ue_scene`:

```json
{
  "operation": "scene.pie.restart",
  "params": {}
}
```

| Operation | Behavior |
|---|---|
| `scene.pie.start` | Requests PIE in the active level viewport. If PIE is already running or queued, it returns the current state idempotently. |
| `scene.pie.stop` | Requests the active PIE session to stop, or cancels a start that is still queued. |
| `scene.pie.restart` | Requests shutdown first, waits until the old Play World is fully destroyed, and starts PIE on a later Editor Tick. |

The result includes `action`, `requested`, `state`, `sessionId`, and
`generation`. Runtime object handles from an earlier PIE generation fail with
`stale_session_handle`.

## HTTP API

The Editor plugin preserves its three original routes, adds two Workflow
routes, and exposes three diagnostic-session routes for the MCP bridge and
CLIs:

```text
GET  /api/health
GET  /api/capabilities?query=<text>&domain=<domain>&offset=0&limit=25
POST /api/execute
GET  /api/v1/workflow/handshake
POST /api/v1/workflow
POST /api/v1/clients/register
POST /api/v1/clients/heartbeat
POST /api/v1/clients/unregister
```

The MCP bridge registers a random `instanceId` after initialization, sends
`X-UEAI-Session-Id` on later requests, and maintains a heartbeat. Each CLI
process generates one `invocationId`, registers a session on a best-effort
basis, and reuses it for plan/execute pairs or shell requests. It automatically
falls back to Legacy HTTP when an older Editor does not expose the session
routes. These values are for status UI attribution and diagnostics only; they
are not authentication identities.

Execution request:

```json
{
  "capability": "blueprint.asset.list",
  "requestId": "client-generated-uuid",
  "params": {
    "filter": "Player"
  }
}
```

Successful response:

```json
{
  "ok": true,
  "data": {}
}
```

Error response:

```json
{
  "ok": false,
  "error": {
    "code": "invalid_params",
    "message": "..."
  }
}
```

Status-code policy:

| HTTP | Meaning |
|---:|---|
| 400 | Invalid JSON |
| 404 | Unknown capability |
| 409 | `requestId` payload conflict or asynchronous job conflict |
| 410 | Stale PIE session handle |
| 422 | Invalid parameters or domain mismatch |
| 500 | Editor execution failure |
| 503 | Editor service unavailable |

At startup, the Registry verifies a one-to-one mapping between every manifest
entry and C++ handler. A missing, duplicate, or cross-domain binding places the
service in `degraded` state. Health checks and capability discovery remain
available, but execution is disabled.

`/api/capabilities` returns summary pages without `inputSchema` by default and
includes `total/offset/limit/hasMore`. Filters include `kind`, `readOnly`,
`destructive`, `expensive`, and `outputKind`. Use `detail=full` for full
descriptors or `operation=<dotted-id>` for one exact schema. The maximum
`limit` is 100.

## Development and Validation

When adding or moving a capability:

1. Declare its descriptor in the relevant domain manifest.
2. Implement a handler with the same dotted ID under
   `Private/Domains/<Domain>/<Kind>/`.
3. Register the handler through the domain registrar.
4. Move reusable Unreal-specific logic into `Private/Infrastructure/`.
5. Keep UE version differences inside
   `Private/Infrastructure/Compatibility/`.

Run static and TypeScript validation:

```powershell
node scripts\validate_capabilities.mjs
cd MCP
npm ci
npm run build
npm test
npm audit --omit=dev
```

Build a standalone plugin against UE 5.3:

```powershell
scripts\build_plugin.bat "D:\code\D5\d5render-ue5_3" "..\UE_AI_integration-BuiltPlugin\UE5.3"
```

When a compatible Editor is running, continue with read-only and reversible
smoke tests:

```powershell
python tests\test_health.py
python tests\test_tools.py
```

## Credits

The project draws on ideas and implementation patterns from:

- [BlueprintMCP](https://github.com/mirno-ehf/ue5-mcp)
- [UnrealClaude](https://github.com/Natfii/UnrealClaude)
- [unreal-engine-mcp](https://github.com/flopperam/unreal-engine-mcp)

This project is licensed under the MIT License.
