---
name: ue-recovery-operator
description: Recover interrupted or stalled Unreal Editor agent work with bounded retry, reconnection, durable-job inspection, source-control preflight, checkpoints, and final read-back. Use when Editor becomes unresponsive, an MCP connection drops, compilation or a long job stalls, a previous workflow must be resumed, or an operator needs a safe recovery plan. Never restart Editor without explicit approval.
---

# UE Recovery Operator

Recover evidence before repeating work. Load
`references/recovery-contract.md`, discover exact schemas through `ue_context`
or `ue help`, and keep all writes inside Workflow or Recipe Runner.

## Run the recovery loop

1. Capture the current Editor instance, health, job/scenario ID, last phase,
   heartbeat, last progress time, and bounded log tail.
2. Classify the failure. Retry only errors explicitly marked transient. Never
   retry schema, approval, permission, plan-digest, or source-control failures.
3. Retry at most three times with delays of 1, 2, and 4 seconds. Reconnect and
   inspect durable state before each attempt; do not replay a completed write.
4. Check source control before any asset write. For Perforce, require an active
   provider, refresh status, and check out every target first. Checkout failure
   means zero asset writes.
5. When the Editor is unresponsive, preserve instance and recovery records.
   Pause at `awaitingApproval` before any restart. Never infer restart consent.
6. Resume from the latest verified checkpoint. Use declared compensation or
   Workflow rollback in reverse order after a terminal failure.
7. Finish with read-back, compile/validation state, artifact evidence, and
   source-control status. Report any recovery action that remains pending.

## Guardrails

- Treat GameThread/UObject steps as atomic. Record `cancelPending` and wait for
  the next safe boundary instead of killing a transaction.
- Poll with bounded intervals and deadlines; never create an infinite loop.
- Use bounded log reads and preserve the last meaningful line in every status.
- Permit only provider preflight, status refresh, checkout, and revert unchanged
  for Perforce. Never submit, shelve, or manage changelists.
- Treat lease override, Editor restart, Development attach, and permission
  expansion as separate approval events with their own plan digest.

## Evidence

Return attempt count, transient classification, delays, instance identity,
checkpoint, workflow receipt, job phase and heartbeat, source-control actions,
compensation/rollback result, and final acceptance checks.
