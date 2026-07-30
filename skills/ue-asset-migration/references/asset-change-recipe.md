# Asset change recipe

## Action constraints

Discover the exact input schema before building the request. The current
contract additionally enforces these business rules:

- import needs a source file visible to the Editor process and a `/Game`
  destination;
- reimport needs one existing source asset;
- copy, move, and rename need source and destination;
- delete needs a source and fails planning when referencers remain;
- fixRedirectors may default its source scope to `/Game`;
- no action may overwrite an existing destination.

Reimport and fixRedirectors must be single-action batches when reliable
rollback is unavailable.

## Guarded sequence

1. `content.asset.search`
2. `content.asset.get`
3. `content.asset.dependencies`
4. `content.asset.referencers`
5. `content.asset.change.plan`
6. Human or caller review of normalized request, preconditions, digest, and
   rollback availability
7. `content.asset.change.execute` with the original request, exact digest, and
   `confirmWrite=true`
8. Receipt diff plus asset/dependency/referencer readback
9. Optional `content.asset.change.rollback` using `receiptId`, a new envelope
   `requestId`, and `confirmWrite=true`
10. Repeat asset, dependency, referencer, and audit readback after rollback

For example, a move request is:

```json
{
  "persistence": "dirtyOnly",
  "actions": [
    {
      "action": "move",
      "source": "/Game/Old/A",
      "destination": "/Game/New/A"
    }
  ]
}
```

The domain-tool calls are:

```json
{
  "operation": "content.asset.change.plan",
  "params": {
    "request": "<the complete request above>"
  }
}
```

After reviewing `rollbackAvailable`, preconditions, normalized request, and
the exact digest:

```json
{
  "operation": "content.asset.change.execute",
  "requestId": "<stable execute request id>",
  "params": {
    "request": "<the unchanged complete request>",
    "approvePlanDigest": "<exact plan digest>",
    "confirmWrite": true
  }
}
```

Rollback, when eligible, is:

```json
{
  "operation": "content.asset.change.rollback",
  "requestId": "<new rollback request id>",
  "params": {
    "receiptId": "<execute receipt id>",
    "confirmWrite": true
  }
}
```

For MCP, put `requestId` in the domain-tool envelope. The `ue` CLI generates it
when supported. Persistence has no hidden default in this Skill: the caller
must choose `dirtyOnly` or `saveOnSuccess`; recommend `dirtyOnly` unless the
caller explicitly authorizes saving.

Rollback is tied to the same Editor instance and receipt state. A successful
plan is not authorization to execute, and an accepted execute response is not
proof until readback succeeds.
