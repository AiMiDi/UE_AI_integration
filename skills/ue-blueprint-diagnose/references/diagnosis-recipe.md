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
   graph and node location, then use `blueprint.graph.describe` to obtain the
   bounded K2 execution-edge projection. Prefer an exact `entryPoint` for a
   named event or function. If the name is ambiguous, take the intended stable
   node GUID from the graph read and retry with `entryNodeId`. For a
   Tick-related finding, trace connected execution output pins from `Event
   Tick` through the returned node GUID, pin GUID, and `edgeId` tuples; stop at
   broken links and do not infer reachability from spatial layout or data
   connections. Omit both selectors only when all execution roots are needed.
6. Use `blueprint.call_graph.get` and `blueprint.asset.references` only after
   locating the candidate when cross-asset impact matters.
7. For an explicit type-impact question, use
   `blueprint.asset.search_by_type`. Supply a narrow asset filter and bounded
   `maxAssets`, `assetOffset`, result, connection, variable, node, pin, and link
   limits from the current schema. Continue with `nextAssetOffset` only when it
   is present. `resultTruncated`, `scanExhausted`, or `partial` means the result
   is not exhaustive; narrow the filter or raise limits instead of treating an
   absent match as proof.
8. Run `blueprint.asset.validate`.
9. Optionally run `blueprint.findings.correlate` only with a retained `scanId`
   and runtime `runId` from the same Editor process.

Always call `ue_context` before constructing parameters. For CLI usage,
`ue-cli help <operation> --json` is equivalent; add `--live-schema` only when an
exact online schema and availability check is required.

## Interpretation

- `same graph` is not proof of execution-flow reachability.
- `graph.describe` reports only K2 execution-pin edges. Its graph totals are
  complete only when `executionTotalsComplete=true`; entry selection, node
  pagination, edge pagination, and graph scanning are independent. Continue
  only with returned next offsets, and never treat
  `execution_entry_scan_incomplete` as entry absence.
- `asset.search_by_type` reports variable, parameter, and connected-pin type
  usage. It does not imply that the matching pin is reached at runtime.
- A Tick-reachable claim needs either a connected execution-pin path from
  `Event Tick` or matching runtime evidence.
- A static finding without runtime evidence remains a hypothesis.
- The scan cache is bounded and process-local; stale scan IDs cannot be
  correlated after Editor restart.
- Informational disconnected-output findings should be summarized instead of
  flooding the result.
- Debug sessions are interactive operations, not Workflow DSL steps.
- Component list/get/set is local Actor Blueprint SCS coverage. It does not
  enumerate or mutate native/inherited, Widget Blueprint, or Level Blueprint
  components.
