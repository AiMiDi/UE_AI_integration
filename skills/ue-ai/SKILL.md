---
name: ue-ai
description: Operate Unreal Engine projects through UE_AI_integration CLI or MCP. Use for UE inspection, edits, tests, profiling, and recovery; not for non-UE work.
version: 1.0.0
metadata:
  author: UE AI Integration Team <ue-ai-integration@local>
  tags:
    - unreal-engine
    - ue-ai-integration
    - automation
---

# UE AI entrypoint

## Purpose

Provide a safe, current-contract entrypoint for UE 5.3-5.7 projects that use
UE_AI_integration. Prefer the native CLI for discovery and verification, and
use MCP only when its fallback conditions apply.

## Requirements

- A project with the UE_AI_integration plugin; its `CLI/bin` directory or the
  `UE_CLI` and `UE_WORKFLOW_CLI` environment variables must expose the CLIs.
- A running Editor is required only for Editor-backed operations. Local
  inspection and catalog discovery may work while it is closed.

## Instructions

Use this Skill as the default front door for UE_AI_integration. Prefer
`ue-cli` and `ue-workflow-cli` for discovery, execution, and verification.
Discover the installed contract and the most specific domain Skill first.
Command examples below use the resolved executable paths; do not assume the
programs are already on PATH.

## Establish the available surface

1. Locate both executables through `UE_CLI` / `UE_WORKFLOW_CLI`, the project's
   plugin `CLI/bin`, or PATH. If MCP is available, `ue_cli` can locate them
   without contacting Editor.
2. On first use or after an upgrade, check both `--version --json` outputs and
   run `ue-cli doctor --json`. For installation acceptance or suspected version
   drift, use `ue-cli doctor --full --no-clean-stale-instances --json`;
   distinguish CLI/catalog, Editor/module, and Trace Worker failures.
3. Use `ue-cli status --json` when the task needs a running Editor. Local
   `capabilities`, `skills`, `help`, and `ue-workflow-cli doctor --json`
   do not require Editor. Follow each capability's declared backend.
4. Treat the current manifests and live availability as authoritative. Do not
   rely on remembered IDs, counts, parameters, or engine support.
5. If CLI is missing, follow `setup-ue5`: UE plugin compilation does not build
   the native CLIs. Use the MCP fallback below while CLI is unavailable.

Editor unavailability alone is not a reason to switch transports: both need the
same Editor for Editor-backed operations. Declared local Project, Asset, Recipe,
SAL, or Trace operations may remain usable with their local dependencies.

## Route to a domain Skill

1. Run `ue-cli skills --query <intent> --json`. Add `--domain` only when
   unambiguous.
2. Load the selected manifest with `ue-cli skills --name <skill-id> --detail
   full --json`; add `--recipe <recipe-id>` when known.
3. The full response contains a machine manifest, not the Markdown body. Read
   `<data.skillRoot>/<skill-id>/SKILL.md` and needed declared resources from
   that directory. Use MCP `ue_skills` get/read if local file access is absent.
4. Follow its discover, execute, and verify phases. Translate MCP operation
   examples to the same capability IDs through CLI where supported, preserving
   parameters, approvals, request IDs, and verification requirements.
5. If no specialized Skill matches, search `ue-cli capabilities`, then obtain
   the exact descriptor with `ue-cli help <capability-id> --json`.

Read [skill routing](references/skill-routing.md) when choosing among the
packaged domain Skills or between MCP and CLI entry points.

## Discover the exact API

Before every capability call, obtain its current schema through
`ue-cli help <capability-id> --json`. For Editor-backed work, use
`--live-schema` to check the loaded Editor's schema and availability; a local
manifest alone does not prove runtime support. Check:

- live availability and unavailable reasons;
- `effects`, `risk`, `destructive`, and `expensive` metadata;
- lifecycle status and canonical replacement;
- execution backend and Editor/PIE/session requirements;
- bounded output, pagination, and artifact fields.

Never guess parameter names or call a deprecated alias when its canonical ID
is available.

### File diagnostics with explicit paths

When a Rider file-diagnostic tool is available, require the caller to provide
both `rootFolder` and `filePath`. Resolve both values to canonical absolute
paths before making the call. Use the opened project root reported by Rider,
not the workspace folder, and convert an in-root file to a project-relative
path with `/` separators.

Do not pass a parent traversal such as `../unrealengine/...` to
`get_file_problems`. Rider rejects files outside the opened project even when
the traversal resolves to a real source file. If the canonical file is outside
`rootFolder`, follow the external-file fallback in
`references/rider-file-diagnostics.md`; preserve the user-specified absolute
path and report which diagnostic backend was used.

## Select the execution contract

- Use `ue-cli <capability-id> ... --json` for one bounded query, validation,
  or independent short command.
- Use `ue-workflow-cli` for an approved multi-step asset edit requiring one
  plan, transaction, read-back, diff, and rollback boundary.
- Group consecutive composable material or Blueprint edits in one Workflow:
  discover each operation's `dsl` admission, defer asset refresh within the
  batch, and request `saveOnSuccess` once if the user wants the result saved.
  Independent domain commands still refresh/save immediately. Recovery
  checkpoints still write changed packages; they reuse verified unchanged images.
- Use Recipe Runner for bounded retry, polling, checkpoints, approvals, source
  control, compensation, or restart-durable continuation.
- Use Durable Job or Scenario capabilities for long tests, performance work,
  Trace capture, cook/package, or PIE automation.
- Use local backends for project/config inspection, UE 5.3 package-header
  inspection, SAL planning, or Trace analysis while Editor is closed.
- Use the CLI surfaces for Recipe Runner, Durable Job, and Scenario capabilities
  when supported; inspect their help before constructing commands.

Generate and reuse a stable `requestId` for retryable commands. Re-plan after
any precondition, asset, world, session, or contract digest changes.

## MCP fallback

Use MCP when the user explicitly requests it, a CLI executable is absent,
unusable, or incompatible, or the required feature is not exposed by that CLI.
Use `ue_status`, `ue_capabilities`, `ue_context`, `ue_skills`, the appropriate
domain tool, and `ue_workflow` for the corresponding steps. Explain the concrete
fallback reason. Fix a catalog/module mismatch before dependent writes; changing
transports does not resolve it.

Do not switch transports to bypass an approval, validation failure, or denied
operation. If a write times out or its outcome is unknown, recover and read back
the existing request/run before any retry; do not blindly resubmit through MCP.

## Verify before reporting success

Run the loaded recipe's verify phase or an equivalent read-only acceptance
check. Prefer structural state, compile result, diff, artifact hash, rendered
evidence, job result, or rollback receipt over a transport-level `ok` value.
Report unavailable, partial, truncated, stale, or inconclusive evidence as
such.

## Preserve safety boundaries

- Never bypass `confirmWrite`, `approvePlanDigest`, lease ownership, source
  control preflight, or Workflow rollback contracts.
- Never start, stop, kill, or restart a user-owned Editor or game process
  without explicit approval and an operation that declares that authority.
- Never expose credentials or secret configuration values in prompts, logs, or
  summaries; retain only redacted presence and non-reversible evidence.
- Never use arbitrary Unreal Python as a shortcut around missing capabilities.
- Keep read-only discovery separate from writes, and keep unrelated user work
  outside the requested scope.

## Examples

Discover the installed contract without contacting an Editor:

```powershell
ue-cli --version --json
ue-cli doctor --json
ue-cli skills --query "diagnose Blueprint compile errors" --json
```

Before an Editor-backed capability, inspect its current live schema:

```powershell
ue-cli help <capability-id> --live-schema --json
```

## Limitations

- A local catalog does not prove that a loaded Editor supports a capability;
  use `--live-schema` when Editor state matters.
- Neither CLI nor MCP can perform Editor-backed work when the required Editor
  is unavailable.
- This entrypoint does not authorize writes, process lifecycle changes, or
  bypassing workflow approvals.

## Troubleshooting

- **CLI unavailable:** locate `CLI/bin` or follow the `setup-ue5` skill; plugin
  compilation alone does not install the native CLIs.
- **Catalog/module mismatch:** run `ue-cli doctor --full
  --no-clean-stale-instances --json`, fix the reported mismatch, then retry.
- **Write outcome unknown:** recover and read back the existing request or run
  before retrying; do not submit a duplicate operation.
