# Blueprint diagnosis recipe

## Scope first

Use one canonical Blueprint asset whenever possible. If the user requests a
project scan, require an explicit `/Game` path prefix plus bounded asset and
finding limits. An empty `blueprint.scan` request can enumerate broader
content, so never use it as the default.

## Operation order

1. `blueprint.asset.summary` with the selected asset name.
2. `blueprint.asset.get` for graphs, variables, components, and stable IDs.
3. `blueprint.scan` with `asset`, or an explicit `/Game` `pathPrefix`.
4. Group findings client-side by severity and `ruleId`; no separate findings
   filter capability exists.
5. Use `blueprint.graph.get` to inspect the strongest finding at its exact
   graph and node location. For a Tick-related finding, trace connected
   execution output pins starting at `Event Tick`; stop at broken links and do
   not infer reachability from spatial layout or data connections.
6. Use `blueprint.call_graph.get` and `blueprint.asset.references` only after
   locating the candidate when cross-asset impact matters.
7. Run `blueprint.asset.validate`.
8. Optionally run `blueprint.findings.correlate` only with a retained `scanId`
   and runtime `runId` from the same Editor process.

Always call `ue_context` before constructing parameters. For CLI usage,
`ue help <operation> --json` is equivalent; add `--live-schema` only when an
exact online schema and availability check is required.

## Interpretation

- `same graph` is not proof of execution-flow reachability.
- A Tick-reachable claim needs either a connected execution-pin path from
  `Event Tick` or matching runtime evidence.
- A static finding without runtime evidence remains a hypothesis.
- The scan cache is bounded and process-local; stale scan IDs cannot be
  correlated after Editor restart.
- Informational disconnected-output findings should be summarized instead of
  flooding the result.
- Debug sessions are interactive operations, not Workflow DSL steps.
