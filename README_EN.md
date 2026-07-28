# UE_AI_integration

[简体中文](README.md) | [English](README_EN.md)

`UE_AI_integration` is an MCP integration plugin for Unreal Editor. It combines
a single Editor Module with a TypeScript stdio bridge so MCP clients such as
Codex CLI and Claude Code can inspect or modify Blueprints, scenes, content
assets, animation, AI, and production workflows.

The current plugin version is `0.3.0`. Unreal Engine 5.3 is the verified build
baseline. Differences for UE 5.4–5.7 are isolated in the compatibility layer,
but those versions have not all been compiled locally.

## Highlights

- 212 manifest-driven Editor and PIE runtime capabilities.
- Ten stable MCP tools instead of exposing all 212 capabilities as tools.
- Six domain routers: Blueprint, Scene, Content, Animation, AI, and Production.
- Dedicated PIE lifecycle, runtime object/widget/delegate, real input, and
  scenario operations.
- Blueprint and UMG mutations report compile, save, reload, and readback evidence.
- A consistent HTTP envelope, status-code policy, and parameter error model.
- Query, command, and validation layers, with all UObject operations queued onto
  the Game Thread.
- The MCP bridge only connects to an already-running Unreal Editor. It never
  starts or terminates the Editor.
- The server listens on `127.0.0.1:9847` by default. Clients can override the
  port with `UE_PORT`.
- [UE Workflow DSL/CLI](docs/UE_WORKFLOW_DSL.md) combines deterministic
  single-asset edits into one planned, approved, and reversible execution;
  debugging sessions and long-running jobs stay outside Workflow.

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
| Blueprint | 58 | Asset lifecycle, graphs, variables, components, interfaces, discovery, diff, validation |
| Scene | 54 | Actors, PIE runtime, widget/delegate/input, viewport, world generation, foliage, navigation |
| Content | 59 | Materials, DataTables, user types, Niagara, UMG authoring and animation |
| Animation | 10 | Animation Blueprints, states, transitions, BlendSpaces |
| AI | 9 | Behavior Trees and Blackboards |
| Production | 22 | Sequencer, scenarios, module provenance, build, cook, and package |

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

The bridge always registers these ten tools, even while Unreal Editor is
offline:

- `ue_status`
- `ue_capabilities`
- `ue_context`
- `ue_blueprint`
- `ue_scene`
- `ue_content`
- `ue_animation`
- `ue_ai`
- `ue_production`
- `ue_workflow`

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

Use `ue_capabilities` or `ue_context` to inspect an operation's schema, traits,
and output type first. A domain tool rejects operations owned by another
domain.

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

The Editor plugin preserves its three original routes and adds two Workflow
routes:

```text
GET  /api/health
GET  /api/capabilities?domain=<domain>
POST /api/execute
GET  /api/v1/workflow/handshake
POST /api/v1/workflow
```

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
