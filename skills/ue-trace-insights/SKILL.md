---
name: ue-trace-insights
description: Record bounded Unreal Editor, PIE, or approved Development traces and analyze registered .utrace files through offline TraceServices semantic queries without starting Unreal Editor. Use for Timing, Counter, Memory, Asset Loading, Network, Tasks, context switches, File IO, Log, Bookmark, Region, trace screenshot, export, or opening a trace in matching Unreal Insights.
---

# UE Trace Insights

Separate recording from analysis. Use the Editor or Development runtime only to
produce a bounded trace; use the local Trace Worker for offline semantic data.

## Load a recipe

1. Use `record-and-analyze` for a new Editor, PIE, or approved Development
   capture.
2. Use `analyze-existing-trace` for an existing `.utrace` file, including when
   Unreal Editor is closed.
3. Load `references/trace-insights-workflow.md` and discover every operation
   through `ue_context` or `ue-cli help` before execution.

## Guardrails

- Bind PIE evidence to its begin/end Region and Bookmark; do not describe it as
  a separate process trace.
- Launch Development only from a versioned profile whose exact plan digest was
  approved. Never accept an arbitrary executable or command line.
- Import an external trace before querying it. Use its registered `traceId`,
  bounded time ranges, filters, stable sorting, cursor, and limit.
- Check `production.trace.provider.list` before each provider-specific query.
  Missing channels or unsupported providers are explicit unavailable evidence.
- Require the Worker and trace Engine minor versions to match.
- Use `production.trace.open_in_insights` only for visualization. Never automate
  Unreal Insights with mouse clicks or treat window state as analysis data.

## Evidence

Report trace/job IDs, SHA-256, Engine/protocol/contract identity, target and
Region, provider availability, exact query range/filter/page, normalized export
artifact IDs, partial/crash status, and whether an Insights view mapping was
applied. Do not return unbounded raw event streams.
