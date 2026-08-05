# Recovery contract

## Retry classification

Retry only `editor_unreachable`, `worker_unavailable`, `connection_reset`,
`operation_temporarily_unavailable`, and an explicitly declared transient
timeout. The maximum is three attempts with exponential delays of 1, 2, and 4
seconds. Persist the attempt before waiting so a restarted runner cannot exceed
the limit.

Never retry `schema_invalid`, `approval_required`, `approval_mismatch`,
`plan_digest_mismatch`, `permission_denied`, `source_control_checkout_failed`,
or any validation failure.

## Unresponsive Editor

An Editor is unresponsive only after its process identity still matches the
instance record while bounded health probes and progress timestamps exceed the
declared deadline. A missing or reused PID is a stale record, not an
unresponsive Editor. Preserve `serverInstanceId`, PID, process start time,
endpoint, last health, job ID, checkpoint, and bounded log evidence.

Restart is never automatic. Produce an `awaitingApproval` record whose digest
binds the instance identity, project summary, affected checkpoint, requested
restart action, and recovery continuation.

## Perforce boundary

Before the first asset write, require provider availability, refresh status,
and check out every target path. Abort with zero writes if any target cannot be
checked out. After success, only `revert unchanged` is permitted as automatic
cleanup. Submit, shelve, changelist creation/editing, force sync, and workspace
mutation remain unavailable.

## Final acceptance

Read back the affected assets/configuration, compile or validate where
applicable, confirm workflow/recipe terminal status, retrieve bounded result
and log evidence, and report package dirty/compile state plus source-control
status separately.
