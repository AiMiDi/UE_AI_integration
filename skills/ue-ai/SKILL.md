---
name: ue-ai
description: Route Unreal Engine work through the UE_AI_integration MCP, native CLIs, Workflow, Recipe Runner, and packaged domain Skills. Use whenever an agent must inspect, modify, debug, test, profile, recover, or automate a UE 5.3-5.7 project with UE_AI_integration; when choosing among ue_status, ue_context, ue_skills, the six UE domain tools, ue_workflow, ue-cli, and ue-workflow-cli; or when a UE request should be delegated to a Blueprint, UMG, asset, world, landscape, rendering, performance, Trace, or recovery Skill.
---

# UE AI entrypoint

Use this Skill as the default front door for UE_AI_integration. Discover the
installed contract and the most specific domain Skill before executing work.

## Establish the available surface

1. Call `ue_status` when the Editor may be running.
2. Call `ue_cli` when MCP is offline or a native CLI fallback may be needed.
3. Treat the capability manifest and live availability as authoritative. Do not
   rely on remembered capability IDs, counts, parameters, or engine support.
4. If neither MCP nor CLI is installed, use `setup-ue5`; do not invent a raw
   HTTP or arbitrary Python substitute.

Editor unavailability does not block `ue_skills`, `ue_cli`, or capabilities
whose declared backend is local Project, Asset, Recipe, SAL, or Trace.

## Route to a domain Skill

1. Call `ue_skills` with `action=list` and a short query derived from the user
   intent. Add `domain` only when it is unambiguous.
2. If a match exists, call `ue_skills` with `action=get`, the exact `skill`, and
   the selected `recipe` when one is known.
3. Read a declared Skill reference with `action=read` only when its detailed
   boundary or acceptance contract is needed.
4. Follow the loaded Skill's discover, execute, and verify phases. Do not copy
   its operation list from memory.
5. If no specialized Skill matches, search `ue_capabilities`, then request the
   exact descriptor with `ue_context.operation`.

Read [skill routing](references/skill-routing.md) when choosing among the
packaged domain Skills or between MCP and CLI entry points.

## Discover the exact API

Before every capability call, obtain its current schema through
`ue_context.operation` or `ue-cli help <capability-id> --json`. Check:

- live availability and unavailable reasons;
- `effects`, `risk`, `destructive`, and `expensive` metadata;
- lifecycle status and canonical replacement;
- execution backend and Editor/PIE/session requirements;
- bounded output, pagination, and artifact fields.

Never guess parameter names or call a deprecated alias when its canonical ID
is available.

## Select the execution contract

- Use a domain tool for one bounded query, validation, or independent short
  command.
- Use `ue_workflow` for an approved multi-step asset edit requiring one plan,
  transaction, read-back, diff, and rollback boundary.
- Use Recipe Runner for bounded retry, polling, checkpoints, approvals, source
  control, compensation, or restart-durable continuation.
- Use Durable Job or Scenario capabilities for long tests, performance work,
  Trace capture, cook/package, or PIE automation.
- Use local backends for project/config inspection, UE 5.3 package-header
  inspection, SAL planning, or Trace analysis while Editor is closed.
- Use `ue-cli` and `ue-workflow-cli` only when MCP is unavailable, the user requests a
  CLI, or a terminal/release workflow requires them.

Generate and reuse a stable `requestId` for retryable commands. Re-plan after
any precondition, asset, world, session, or contract digest changes.

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
