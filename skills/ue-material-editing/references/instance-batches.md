# Material instance overrides

Use instance.parameters.get with an exact material instance path. Read the
local override, inherited and effective values plus stateHash. Query pages may
use expectedStateHash to prevent mixing revisions. This is a parameter-state
hash, not GraphIR or shader-validity evidence.

Use one instance.parameters.batch with that expectedStateHash and 1..128 unique
operations. Each operation is set or clear and has type scalar/vector/texture/
switch plus name, association and index. Global index is -1; layer/blend need
the actual index. Same names in different layers are different parameters.
Changing a Layer layout invalidates old state. Inspect instead of guessing a
new index after conflict.

- Set values use a finite number, finite r/g/b/a object, exact compatible texture
  path, or boolean switch. Use live schema for the selected type.
- Clear omits value and removes the local override so inheritance resumes.
  Orphan overrides may be cleared, not set. An inherited value is not a local
  override to delete.
- dryRun:true validates the plan without mutation. Default save:false marks
  changes dirty and finalizes once. Explicit save:true persists; even a no-op
  batch may save earlier dirty content.
- Require readbackVerified after mutation and inspect saved separately. A failed
  readback reports restoreVerified; a failed save can retain memory changes.

Re-query and compare the intended identities, local/effective values and cleared
inheritance. finalizeCount measures the tool finalizer, not shader jobs or frame
time. Static switches can compile shaders; shaderValidationPerformed:false
means parameter readback has not proved shader validity.

The legacy instance.parameter.set now defaults to no save but updates each
individual call; use a batch for continuous edits. Do not route an instance
through a material/materialFunction Workflow scope. Parent migration, function
instances, Layer stack edits and Atlas curve editing remain outside this API.
