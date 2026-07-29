# UE_AI_integration contributor guide

## System boundary

```text
MCP client ←stdio→ MCP/src ←HTTP /api→ UE_AI_integration Editor module
```

The TypeScript process is a connect-only protocol adapter. It must never
launch, kill, or request shutdown of an Editor process.

## Source layout

The UE plugin remains one Editor module with four internal layers:

- `Private/Core` — capability descriptors, registry and execution.
- `Private/Transport` — HTTP routes and the Game Thread request queue.
- `Private/Workflow` — the UE adapter for shared planning, transactions,
  finalization, journals and rollback.
- `Private/Domains` — six domains, each split into Query, Command and
  Validation where applicable.
- `Private/Infrastructure` — UE-specific lookup, serialization, persistence,
  snapshots, crash boundaries and version compatibility.

`Resources/Capabilities/*.json` is the public capability contract. Handler
names, MCP routing and documentation must agree with it.

## Invariants

- The catalog contains six domain manifests. Capability and per-domain counts
  are derived from those manifests and may grow; every previously shipped ID
  remains a compatibility contract.
- Capability IDs are lower-case dotted identifiers.
- A manifest entry and an implementation must have a one-to-one mapping.
- Domain code contains no UE version macros.
- HTTP handlers do not execute Unreal object operations directly.
- MCP routing uses manifest metadata, never name-pattern classification.
- The legacy HTTP routes remain `/api/health`, `/api/capabilities` and
  `/api/execute`; Workflow adds `/api/v1/workflow/handshake` and
  `/api/v1/workflow`. The best-effort diagnostic caller protocol uses only
  `/api/v1/clients/register`, `/heartbeat` and `/unregister`; it is not an
  authorization boundary and must never block legacy business requests.
- CMake and UBT compile the same `Workflow/src/WorkflowCore.cpp`; TypeScript
  must not duplicate validation or planning.
- Workflow v1 preserves deterministic single-asset planning and digest
  compatibility. Workflow v2 admits deterministic multi-asset edit DAGs with
  named scopes and durable journals. Interactive debugging and long-running
  jobs remain single operations or separate jobs.
- Public capability input fields use lower camelCase. Capability IDs remain
  lower-case dotted contracts and are never camel-cased.

## Adding or moving a capability

1. Choose the domain and `query`, `command` or `validation` kind.
2. Add or update the descriptor in the domain manifest.
3. Implement the capability with the exact dotted ID.
4. Bind it in the domain registrar.
5. Run `node scripts/validate_capabilities.mjs`.
6. Run the MCP build and tests.
7. Compile with `RunUAT BuildPlugin`.

## Commands

```bash
node scripts/validate_capabilities.mjs
cd MCP
npm ci
npm run build
npm test
```

```bat
scripts\build_plugin.bat "D:\code\D5\d5render-ue5_3" "BuiltPlugin\UE5.3"
```

UE 5.3 is the local compile baseline. Compatibility code for UE 5.4-5.7
belongs exclusively under `Private/Infrastructure/Compatibility`.
