#!/usr/bin/env node

import assert from "node:assert/strict";
import { randomUUID } from "node:crypto";
import { existsSync } from "node:fs";
import { resolve, join } from "node:path";
import { pathToFileURL } from "node:url";

const [mode, bundleArgument, endpoint, recipeRoot] = process.argv.slice(2);
if (!mode || !bundleArgument || !endpoint) {
  process.stderr.write("Usage: rc_online_acceptance.mjs <online|recipe-start|recipe-resume> <bundleRoot> <endpoint> [recipeRoot|runId] [runId|planDigest] [planDigest]\n");
  process.exit(2);
}

const bundleRoot = resolve(bundleArgument);
const bridgePath = join(bundleRoot, "MCP", "dist", "ue-bridge.js");
const recipePath = join(bundleRoot, "MCP", "dist", "recipe-runner.js");
if (!existsSync(bridgePath) || !existsSync(recipePath)) {
  throw new Error("The extracted release bundle is missing MCP runtime modules.");
}
const { UEApiError, UEClient } = await import(pathToFileURL(bridgePath).href);
const { RecipeRunner } = await import(pathToFileURL(recipePath).href);

function emit(data) {
  process.stdout.write(`${JSON.stringify({ ok: true, data })}\n`);
}

async function expectError(action, code) {
  try {
    await action();
  } catch (error) {
    assert.ok(error instanceof UEApiError, `Expected UEApiError, received ${String(error)}`);
    assert.equal(error.code, code);
    return error;
  }
  assert.fail(`Expected ${code}.`);
}

async function waitFor(predicate, timeoutMs = 15_000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const value = predicate();
    if (value) return value;
    await new Promise((resolvePromise) => setTimeout(resolvePromise, 100));
  }
  assert.fail("Acceptance state did not converge before the timeout.");
}

async function runOnline() {
  const clientA = new UEClient({ baseUrl: endpoint, timeoutMs: 30_000 });
  const clientB = new UEClient({ baseUrl: endpoint, timeoutMs: 30_000 });
  await clientA.startSession({ name: "ue-rc-acceptance-a", version: "1.0.0" });
  await clientB.startSession({ name: "ue-rc-acceptance-b", version: "1.0.0" });
  const evidence = {};
  try {
    const catalog = await clientA.execute(
      "scene.viewport.visualization.list",
      { target: { kind: "editor" }, family: "buffer" },
      `accept-list-${randomUUID()}`,
    );
    const modes = Array.isArray(catalog.modes) ? catalog.modes : [];
    const selected = modes.find((item) => item?.available === true);
    assert.ok(selected && typeof selected.mode === "string", "No available non-RT buffer visualization was found.");
    const captureParams = {
      target: { kind: "editor" },
      visualization: { family: "buffer", mode: selected.mode },
      width: 512,
      height: 288,
      imagePolicy: { targetTokens: 512, maxWidth: 512, maxHeight: 288, inline: "none", color: "color" },
    };
    let cancelledRequestId;
    let cancelled;
    let cancelLatencyMs;
    for (const abortDelayMs of [5, 10, 20, 40]) {
      const candidateRequestId = `accept-cancel-${randomUUID()}`;
      const controller = new AbortController();
      const startedAt = Date.now();
      const pending = clientA.execute(
        "scene.viewport.visualization.capture",
        captureParams,
        candidateRequestId,
        { signal: controller.signal },
      );
      setTimeout(() => controller.abort(), abortDelayMs);
      try {
        await pending;
      } catch (error) {
        if (
          error instanceof UEApiError &&
          error.code === "request_cancelled" &&
          error.details?.cancelPending === true
        ) {
          cancelledRequestId = candidateRequestId;
          cancelled = error;
          cancelLatencyMs = Date.now() - startedAt;
          break;
        }
      }
    }
    assert.ok(cancelledRequestId && cancelled && Number.isFinite(cancelLatencyMs), "No capture reached an acknowledged cancellable state.");
    assert.ok(cancelLatencyMs <= 2_000, `Cancel took ${cancelLatencyMs} ms.`);
    assert.equal(cancelled.details?.requestId, cancelledRequestId);
    assert.equal(cancelled.details?.cancelPending, true);
    assert.equal(cancelled.details?.state, "cancellationRequested");
    assert.equal(cancelled.details?.capability, "scene.viewport.visualization.capture");
    const cachedCancellation = await expectError(
      () => clientA.execute("scene.viewport.visualization.capture", captureParams, cancelledRequestId),
      "request_cancelled",
    );
    assert.equal(cachedCancellation.details?.stateRestored, true);
    const freshCapture = await clientA.execute(
      "scene.viewport.visualization.capture",
      captureParams,
      `accept-capture-${randomUUID()}`,
    );
    assert.equal(freshCapture.stateRestored, true);
    assert.equal(typeof freshCapture.captureId, "string");
    evidence.cancel = {
      requestId: cancelledRequestId,
      cancelLatencyMs,
      ack: cancelled.details,
      cachedResult: cachedCancellation.details,
      freshCaptureId: freshCapture.captureId,
      stateRestored: freshCapture.stateRestored,
    };

    const leaseA = await clientA.execute("production.lease.acquire", { type: "pie" }, `lease-a-${randomUUID()}`);
    const conflict = await expectError(
      () => clientB.execute("production.lease.acquire", { type: "pie" }, `lease-b-${randomUUID()}`),
      "lease_conflict",
    );
    const overrideDigest = conflict.details?.overridePlanDigest;
    assert.match(String(overrideDigest), /^sha256:[0-9a-f]{64}$/);
    await expectError(
      () => clientB.execute("scene.pie.stop", {}, `lease-protected-${randomUUID()}`),
      "lease_conflict",
    );
    await expectError(
      () => clientB.execute(
        "production.lease.acquire",
        { type: "pie", override: true, approvePlanDigest: `sha256:${"0".repeat(64)}` },
        `lease-wrong-${randomUUID()}`,
      ),
      "lease_conflict",
    );
    const overridden = await clientB.execute(
      "production.lease.acquire",
      { type: "pie", override: true, approvePlanDigest: overrideDigest },
      `lease-override-${randomUUID()}`,
    );
    assert.notEqual(overridden.leaseId, leaseA.leaseId);
    await clientB.stopSession();
    const leaseStatus = await clientA.execute("production.lease.status", {}, `lease-status-${randomUUID()}`);
    const activeLeases = Array.isArray(leaseStatus.leases) ? leaseStatus.leases : [];
    assert.equal(activeLeases.some((lease) => lease?.type === "pie"), false);
    evidence.lease = {
      firstLeaseId: leaseA.leaseId,
      overrideLeaseId: overridden.leaseId,
      overridePlanDigest: overrideDigest,
      audit: leaseStatus.overrideAudit ?? [],
      releasedOnUnregister: true,
    };

    assert.ok(recipeRoot, "online acceptance requires a temporary Recipe root.");
    const sessionRecipe = {
      schema: "ue.recipe.v2",
      id: "rc-session-pie-lifecycle",
      version: "1.0.0",
      sessionPolicy: {
        requirePieStopped: true,
        runnerStartsPie: true,
        lease: "pie",
        editorInstanceBound: true,
      },
      steps: [
        {
          id: "status",
          kind: "sessionCapability",
          capability: "scene.pie.status",
          params: {},
        },
      ],
    };
    const sessionRunner = new RecipeRunner({
      env: { ...process.env, UEAI_RECIPE_ROOT: resolve(recipeRoot) },
      endpoint,
    });
    const sessionPlan = await sessionRunner.planOnline(sessionRecipe, {});
    const sessionStarted = await sessionRunner.startOnline(
      sessionRecipe,
      {},
      String(sessionPlan.planDigest),
    );
    const sessionCompleted = await waitFor(() => {
      const value = sessionRunner.status(sessionStarted.runId);
      return value.status === "completed" ? value : undefined;
    }, 30_000);
    assert.equal(sessionCompleted.steps[0].attempts, 1);
    assert.equal(sessionCompleted.session?.runnerStartedPie, true);
    assert.ok(Number.isInteger(sessionCompleted.session?.activePieGeneration));
    assert.ok(
      sessionCompleted.compensations.some(
        (item) => item.stepId === "$pie.stop" && item.ok === true,
      ),
      "Session Recipe did not stop its owned PIE generation.",
    );
    assert.ok(
      sessionCompleted.compensations.some(
        (item) => item.stepId === "$pie.lease" && item.ok === true,
      ),
      "Session Recipe did not release its caller-bound PIE lease.",
    );
    const pieAfterRecipe = await clientA.execute(
      "scene.pie.status",
      {},
      `session-recipe-pie-status-${randomUUID()}`,
    );
    assert.ok(
      ["stopped", "idle", "none", "inactive"].includes(
        String(pieAfterRecipe.state).toLowerCase(),
      ),
      `Runner-owned PIE remained active: ${JSON.stringify(pieAfterRecipe)}`,
    );
    evidence.sessionRecipe = {
      runId: sessionCompleted.runId,
      planDigest: sessionCompleted.planDigest,
      serverInstanceId: sessionCompleted.session?.serverInstanceId,
      generation: sessionCompleted.session?.activePieGeneration,
      firstAttempts: sessionCompleted.steps[0].attempts,
      compensationCount: sessionCompleted.compensations.length,
      pieStopped: true,
    };

    const levelMap = "/Game/UEAI/Acceptance/LevelBlueprintFixture";
    await clientA.execute(
      "scene.level.open",
      { levelPath: levelMap },
      `level-open-${randomUUID()}`,
    );
    const baselineLevelGraph = await clientA.execute(
      "blueprint.graph.get",
      { name: levelMap, graph: "EventGraph", geometryMode: "stored" },
      `level-baseline-${randomUUID()}`,
    );
    const baselineLevelNodes = Array.isArray(baselineLevelGraph.nodes)
      ? baselineLevelGraph.nodes.length
      : 0;
    const directComment = `RC2 direct 中文 + symbols !@# ${randomUUID()}`;
    const directAdd = await clientA.execute(
      "blueprint.node.add",
      {
        blueprint: levelMap,
        graph: "EventGraph",
        nodeType: "Comment",
        comment: directComment,
        posX: 256,
        posY: 128,
      },
      `level-direct-add-${randomUUID()}`,
    );
    assert.equal(directAdd.saved, true);
    assert.equal(directAdd.compiled, true);
    const directReadBack = await clientA.execute(
      "blueprint.graph.get",
      { name: levelMap, graph: "EventGraph", geometryMode: "stored" },
      `level-direct-readback-${randomUUID()}`,
    );
    assert.ok(
      directReadBack.nodes?.some((node) => node?.nodeId === directAdd.nodeId),
      "Direct Level Blueprint node add was not visible in graph read-back.",
    );
    const directDelete = await clientA.execute(
      "blueprint.node.delete",
      { blueprint: levelMap, nodeId: directAdd.nodeId },
      `level-direct-delete-${randomUUID()}`,
    );
    assert.equal(directDelete.saved, true);
    const cleanedDirectGraph = await clientA.execute(
      "blueprint.graph.get",
      { name: levelMap, graph: "EventGraph", geometryMode: "stored" },
      `level-direct-clean-${randomUUID()}`,
    );
    assert.equal(cleanedDirectGraph.nodes?.length, baselineLevelNodes);
    assert.equal(
      cleanedDirectGraph.nodes?.some((node) => node?.nodeId === directAdd.nodeId),
      false,
    );

    const levelSuffix = randomUUID().replaceAll("-", "");
    const levelWorkflow = {
      dsl: "ue.workflow",
      dslVersion: "2.0",
      workflowKind: "assetEdit",
      workflowId: `accept-level-blueprint-${levelSuffix}`,
      scopes: {
        level: { kind: "levelBlueprint", asset: levelMap },
      },
      persistence: "dirtyOnly",
      operations: [
        {
          id: "addFirstComment",
          type: "blueprint.node.add",
          scope: "level",
          params: {
            graph: "EventGraph",
            nodeType: "Comment",
            comment: `RC2 Workflow A ${levelSuffix}`,
            posX: 512,
            posY: 128,
          },
        },
        {
          id: "addSecondComment",
          type: "blueprint.node.add",
          scope: "level",
          params: {
            graph: "EventGraph",
            nodeType: "Comment",
            comment: `RC2 Workflow B ${levelSuffix}`,
            posX: 1024,
            posY: 128,
          },
        },
      ],
    };
    const levelPlan = await clientA.workflow({
      action: "plan",
      workflow: levelWorkflow,
      detailLevel: "standard",
    });
    assert.match(String(levelPlan.planDigest), /^sha256:[0-9a-f]{64}$/);
    const levelPlanAssets = levelPlan.preconditions?.assets ?? [];
    assert.equal(levelPlanAssets.length, 1);
    assert.equal(levelPlanAssets[0]?.packageType, "map");
    assert.equal(levelPlanAssets[0]?.packageExtension, ".umap");
    const levelExecute = await clientA.workflow({
      action: "execute",
      requestId: `accept-level-workflow-${levelSuffix}`,
      workflow: levelWorkflow,
      approvePlanDigest: levelPlan.planDigest,
      confirmWrite: true,
      saveOnSuccess: true,
      detailLevel: "standard",
    });
    assert.equal(levelExecute.status, "completed");
    const levelMutated = await clientA.execute(
      "blueprint.graph.get",
      { name: levelMap, graph: "EventGraph", geometryMode: "stored" },
      `level-workflow-readback-${randomUUID()}`,
    );
    assert.equal(levelMutated.nodes?.length, baselineLevelNodes + 2);
    const levelRollback = await clientA.workflow({
      action: "rollback",
      runId: levelExecute.runId,
      approvePlanDigest: levelPlan.planDigest,
      detailLevel: "standard",
    });
    assert.equal(levelRollback.rollbackVerified, true);
    const levelRestored = await clientA.execute(
      "blueprint.graph.get",
      { name: levelMap, graph: "EventGraph", geometryMode: "stored" },
      `level-workflow-restored-${randomUUID()}`,
    );
    assert.equal(levelRestored.nodes?.length, baselineLevelNodes);
    evidence.levelBlueprint = {
      map: levelMap,
      directNodeId: directAdd.nodeId,
      directSaved: directAdd.saved,
      directReadBack: true,
      directCleanupSaved: directDelete.saved,
      runId: levelExecute.runId,
      planDigest: levelPlan.planDigest,
      packageType: levelPlanAssets[0]?.packageType,
      packageExtension: levelPlanAssets[0]?.packageExtension,
      operations: 2,
      rollbackVerified: levelRollback.rollbackVerified,
      restoredNodeCount: levelRestored.nodes?.length,
    };

    const suffix = randomUUID().replaceAll("-", "");
    const asset = `/Game/UEAI/Acceptance/BP_WriteRollback_${suffix}`;
    await expectError(
      () => clientA.execute("blueprint.asset.get", { name: asset }, `baseline-${randomUUID()}`),
      "asset_not_found",
    );
    const workflow = {
      dsl: "ue.workflow",
      dslVersion: "2.0",
      workflowKind: "assetEdit",
      workflowId: `accept-write-rollback-${suffix}`,
      scopes: {
        main: { kind: "blueprint", asset, createIfMissing: true },
      },
      persistence: "dirtyOnly",
      operations: [
        {
          id: "addAcceptanceFlag",
          type: "blueprint.variable.add",
          scope: "main",
          params: { variableName: "AcceptanceFlag", variableType: "Boolean" },
        },
      ],
    };
    const plan = await clientA.workflow({ action: "plan", workflow, detailLevel: "summary" });
    assert.match(String(plan.planDigest), /^sha256:[0-9a-f]{64}$/);
    const execute = await clientA.workflow({
      action: "execute",
      requestId: `accept-workflow-${suffix}`,
      workflow,
      approvePlanDigest: plan.planDigest,
      confirmWrite: true,
      saveOnSuccess: true,
      detailLevel: "standard",
    });
    assert.equal(execute.status, "completed");
    const readBack = await clientA.execute("blueprint.asset.get", { name: asset }, `readback-${randomUUID()}`);
    assert.ok(JSON.stringify(readBack).includes("AcceptanceFlag"));
    const assetState = await clientA.execute(
      "blueprint.asset.dirty.get",
      { blueprint: asset },
      `asset-state-${randomUUID()}`,
    );
    assert.equal(assetState.packageDirty, false);
    assert.equal(typeof assetState.compileStatus, "string");
    const rollback = await clientA.workflow({
      action: "rollback",
      runId: execute.runId,
      approvePlanDigest: plan.planDigest,
      detailLevel: "standard",
    });
    assert.equal(rollback.rollbackVerified, true);
    await expectError(
      () => clientA.execute("blueprint.asset.get", { name: asset }, `rolledback-${randomUUID()}`),
      "asset_not_found",
    );
    evidence.workflow = {
      asset,
      runId: execute.runId,
      planDigest: plan.planDigest,
      requestId: execute.requestId,
      packageDirty: assetState.packageDirty,
      compileStatus: assetState.compileStatus,
      rollbackVerified: rollback.rollbackVerified,
    };
    emit(evidence);
  } finally {
    await clientB.stopSession().catch(() => undefined);
    await clientA.stopSession().catch(() => undefined);
  }
}

function readOnlyRecipe() {
  return {
    schema: "ue.recipe.v2",
    id: "rc-editor-restart-recovery",
    version: "1.0.0",
    steps: [
      { id: "first", kind: "capability", capability: "scene.viewport.visualization.list", params: { target: { kind: "editor" }, family: "buffer" } },
      { id: "second", kind: "capability", capability: "blueprint.asset.list", params: {} },
    ],
  };
}

async function runRecipeStart() {
  assert.ok(recipeRoot, "recipe-start requires recipeRoot.");
  const env = {
    ...process.env,
    UEAI_RECIPE_ROOT: resolve(recipeRoot),
    UEAI_RECIPE_TEST_MODE: "1",
    UEAI_RECIPE_TEST_EXIT_AFTER_STEP: "first",
  };
  const runner = new RecipeRunner({ env, endpoint });
  const started = runner.start(readOnlyRecipe());
  const interrupted = await waitFor(() => {
    const value = runner.status(started.runId);
    return value.status === "interrupted" ? value : undefined;
  }, 20_000);
  assert.equal(interrupted.steps[0].status, "completed");
  assert.equal(interrupted.steps[0].attempts, 1);
  emit({ runId: started.runId, planDigest: started.planDigest, firstAttempts: 1 });
}

async function runRecipeResume() {
  const runId = process.argv[6];
  const planDigest = process.argv[7];
  assert.ok(recipeRoot && runId && planDigest, "recipe-resume requires recipeRoot, runId, and planDigest.");
  const env = { ...process.env, UEAI_RECIPE_ROOT: resolve(recipeRoot) };
  const runner = new RecipeRunner({ env, endpoint });
  const before = runner.status(runId);
  assert.equal(before.status, "interrupted");
  const firstAttempts = before.steps[0].attempts;
  runner.resume(runId, planDigest);
  const completed = await waitFor(() => {
    const value = runner.status(runId);
    return value.status === "completed" ? value : undefined;
  }, 20_000);
  assert.equal(completed.steps[0].attempts, firstAttempts);
  assert.equal(completed.steps[1].attempts, 1);
  emit({ runId, planDigest, firstAttempts, secondAttempts: 1, status: completed.status });
}

try {
  if (mode === "online") await runOnline();
  else if (mode === "recipe-start") await runRecipeStart();
  else if (mode === "recipe-resume") await runRecipeResume();
  else throw new Error(`Unknown acceptance mode ${mode}.`);
} catch (error) {
  process.stdout.write(`${JSON.stringify({
    ok: false,
    error: {
      code: error instanceof UEApiError ? error.code : "rc_acceptance_failed",
      message: error instanceof Error ? error.message : String(error),
      details: error instanceof UEApiError ? error.details : undefined,
    },
  })}\n`);
  process.exitCode = 1;
}
