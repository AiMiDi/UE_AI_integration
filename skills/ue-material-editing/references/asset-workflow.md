# Authored assets and functions

For continuous authored edits, use `ue-workflow-cli` and the current Workflow
schema. A single scope uses kind material or materialFunction; Workflow v2 can
include the function and its actual host material together. Keep persistence
`dirtyOnly` while iterating, and request final persistence only when required.

Workflow operations use `{id, type, params}` and structured bindings, unlike the
preview batch's `{id, capability, params}` and `$id` shorthand. For example,
`bindings:{"/nodeId":{"from":"custom","path":"/nodeId"}}` connects a
custom.set step to the node returned by the earlier expression.add step.
The scope supplies material/materialFunction; do not add competing ownership
selectors to operation parameters. Set up a function call's reference before
connecting its discovered interface pins.

Plan the exact request, use the required digest/confirmation contract when
executing, and keep its run/recovery evidence. Existing user authorization can
cover execution; do not ask again solely because a recipe is labelled confirmWrite.
Do not use direct per-node writes with invented deferCompile metadata to bypass
Workflow admission, checkpoint or recovery. Ordinary direct asset operations may
still refresh/save independently; the material-instance save:false default does
not apply to every asset mutation.

Let the Workflow finalizer update functions before host compilation and perform
its readback/rollback. Read its compile result before deciding another validation
is needed; do not automatically compile again after a successful finalizer.
When only structure was validated, use function.validate with the actual
validationMaterial host if shader verification is required. A disconnected
function call, or one connected only to an unused function input, is not a host.
The selected function output and caller bindings determine structural reachability.
Static branch/variant coverage is still not guaranteed.

Read the changed Custom/parameter/call state and current diagnostics. Unknown,
pending, stale or unverified shader state is not success. Preserve explicit
readback failures and rollback verification; save failure may leave applied
memory edits and must not trigger an automatic full edit retry.

Legacy graph.restore is intentionally dirty-only and is not a Workflow disk
checkpoint. If a requested recovery must survive process loss, use Workflow's
approved plan, durable checkpoint and final persistence path. A compile request,
successful structural readback, verified rollback and saved package bytes are
separate evidence and must be reported separately.

If the visible editor contains unapplied changes, this authored-asset route
cannot make that preview current. Resolve which context the requested change
belongs to before proceeding.
