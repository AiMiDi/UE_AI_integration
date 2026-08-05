import assert from "node:assert/strict";
import { createHash, randomUUID } from "node:crypto";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { test } from "node:test";

import { loadCapabilityCatalog } from "../capability-catalog.js";
import { LocalAssetExecutor, LocalProjectExecutor } from "../project-executor.js";
import { DevelopmentBridgeExecutor } from "../development-bridge.js";
import { RecipeRunner, validateRecipe } from "../recipe-runner.js";
import { parseSalSource, salLint, salPlan } from "../sal-runner.js";
import { UEApiError, UEClient } from "../ue-bridge.js";

function fixtureProject(): string {
  const root = mkdtempSync(join(tmpdir(), "ueai-offline-"));
  mkdirSync(join(root, "Config"));
  writeFileSync(join(root, "Fixture.uproject"), JSON.stringify({ EngineAssociation: "5.3", Plugins: [] }), "utf8");
  writeFileSync(join(root, "Config", "DefaultEngine.ini"), "[/Script/Engine.Engine]\nr.DefaultFeature.AutoExposure=False\n", "utf8");
  return root;
}

function ue53PackageWithRegistryTag(tagCount = 1, tagValue = "x.png"): Buffer {
  const chunks: Buffer[] = [];
  const i32 = (value: number) => { const bytes = Buffer.alloc(4); bytes.writeInt32LE(value); chunks.push(bytes); };
  const u32 = (value: number) => { const bytes = Buffer.alloc(4); bytes.writeUInt32LE(value >>> 0); chunks.push(bytes); };
  const u16 = (value: number) => { const bytes = Buffer.alloc(2); bytes.writeUInt16LE(value); chunks.push(bytes); };
  const i64 = (value: number) => { const bytes = Buffer.alloc(8); bytes.writeBigInt64LE(BigInt(value)); chunks.push(bytes); };
  const string = (value: string) => { const content = Buffer.from(`${value}\0`, "latin1"); i32(content.length); chunks.push(content); };
  const guid = () => { u32(0); u32(0); u32(0); u32(0); };
  const version = () => { u16(5); u16(3); u16(0); u32(0); string(""); };

  u32(0x9e2a83c1); i32(-8); i32(864); i32(522); i32(1009); i32(0); i32(0);
  const totalHeaderOffset = chunks.reduce((total, chunk) => total + chunk.length, 0);
  i32(0); string(""); u32(0);
  i32(0); i32(0); i32(0); i32(0); string(""); i32(0); i32(0);
  i32(0); i32(0); i32(0); i32(0); i32(0); i32(0); i32(0); i32(0); i32(0);
  guid(); guid(); i32(0); version(); version(); u32(0); i32(0); u32(0); i32(0);
  const registryOffsetField = chunks.reduce((total, chunk) => total + chunk.length, 0);
  i32(0); i64(0); i32(0); i32(0); i32(0); i32(0); i32(0); i64(-1); i32(-1);
  const registryOffset = chunks.reduce((total, chunk) => total + chunk.length, 0);
  i64(0); i32(1); string("Header"); string("/Script/Engine.Texture2D"); i32(tagCount);
  for (let index = 0; index < tagCount; index += 1) { string(index === 0 ? "SourceFile" : `Tag${index}`); string(tagValue); }
  const result = Buffer.concat(chunks);
  result.writeInt32LE(result.length, totalHeaderOffset);
  result.writeInt32LE(registryOffset, registryOffsetField);
  return result;
}

test("validates bounded Recipe retries and restricted SAL plan-only source", () => {
  const recipe = {
    schema: "ue.recipe.v2",
    id: "roadmap-test",
    version: "1.0.0",
    steps: [{ id: "provider", kind: "capability", capability: "production.source_control.provider.get", retry: { maxAttempts: 6 } }],
  };
  const invalid = validateRecipe(recipe);
  assert.equal(invalid.ok, false);
  assert.ok(invalid.diagnostics.some((diagnostic) => diagnostic.code === "retry_unbounded"));
  assert.throws(() => parseSalSource("import fs from 'node:fs';"), (error: unknown) => error instanceof UEApiError && error.code === "sal_ast_forbidden");
  recipe.steps[0]!.retry.maxAttempts = 3;
  const source = `plan(${JSON.stringify({ kind: "recipe", value: recipe })})`;
  assert.equal(salLint(source).ok, true);
  const first = salPlan(source);
  const second = salPlan(source);
  assert.equal(first.planDigest, second.planDigest);
  assert.equal(first.executable, false);
});

test("requires source-control checkout before every controlled Workflow write", () => {
  const workflow = { dsl: "ue.workflow", dslVersion: "2.0", workflowKind: "assetEdit", steps: [] };
  const withoutCheckout = validateRecipe({ schema: "ue.recipe.v2", id: "missing-checkout", version: "1.0.0", steps: [{ id: "write", kind: "workflow", workflow }] });
  assert.equal(withoutCheckout.ok, false);
  assert.ok(withoutCheckout.diagnostics.some((diagnostic) => diagnostic.code === "source_control_checkout_required"));
  const withCheckout = validateRecipe({
    schema: "ue.recipe.v2",
    id: "with-checkout",
    version: "1.0.0",
    steps: [
      { id: "checkout", kind: "sourceControlCheckout", params: { files: ["Content/Fixture.uasset"] } },
      { id: "write", kind: "workflow", workflow },
    ],
  });
  assert.equal(withCheckout.ok, true);
  assert.equal(withCheckout.requiresApproval, true);
});

test("keeps offline project/config reads inside the explicit project root", async () => {
  const root = fixtureProject();
  const executor = new LocalProjectExecutor();
  const summary = await executor.execute("production.project.summary.get", { projectRoot: root });
  assert.equal(summary.engineAssociation, "5.3");
  const config = await executor.execute("production.project.config.get", { projectRoot: root });
  assert.ok((config.merged as Record<string, unknown>)["/Script/Engine.Engine"]);
  await assert.rejects(
    executor.execute("production.project.config.get", { projectRoot: root, files: ["../outside.ini"] }),
    (error: unknown) => error instanceof UEApiError && error.code === "path_outside_allowed_root",
  );
});

test("migrates v1 Recipe journals and rejects duplicate recovery of an active Worker", () => {
  const root = mkdtempSync(join(tmpdir(), "ueai-recipe-journal-"));
  const env = { ...process.env, UEAI_RECIPE_ROOT: root };
  const runId = `recipe-run-${randomUUID()}`;
  const directory = join(root, runId);
  mkdirSync(directory, { recursive: true });
  const timestamp = new Date().toISOString();
  const recipe = {
    schema: "ue.recipe.v2",
    id: "migration-fixture",
    version: "1.0.0",
    steps: [{ id: "health", kind: "capability", capability: "production.health.get" }],
  };
  const base = {
    runId,
    recipeId: recipe.id,
    recipeVersion: recipe.version,
    planDigest: `sha256:${"1".repeat(64)}`,
    status: "running",
    phase: "step:health",
    createdAt: timestamp,
    updatedAt: timestamp,
    heartbeatAt: timestamp,
    lastProgressAt: timestamp,
    lastLog: "legacy",
    nextStep: 0,
    inputs: {},
    recipe,
    steps: [{ id: "health", status: "running", attempts: 1 }],
    cancelRequested: false,
    cancelPending: false,
    approvedSteps: [],
    compensations: [],
  };
  writeFileSync(join(directory, "run.json"), JSON.stringify({ schema: "ue.recipe-run.v1", ...base }), "utf8");
  const runner = new RecipeRunner({ env });
  const migrated = runner.status(runId);
  assert.equal(migrated.schema, "ue.recipe-run.v2");
  assert.equal(migrated.status, "interrupted");
  assert.equal(migrated.steps[0]?.status, "dispatching");

  writeFileSync(join(directory, "run.json"), JSON.stringify({
    schema: "ue.recipe-run.v2",
    ...base,
    workerPid: process.pid,
    workerInstanceId: randomUUID(),
    workerHeartbeatAt: timestamp,
    workerLeaseExpiresAt: new Date(Date.now() + 10_000).toISOString(),
  }), "utf8");
  assert.throws(
    () => runner.resume(runId, base.planDigest),
    (error: unknown) => error instanceof UEApiError && error.code === "recipe_run_active",
  );
});

test("resumes an interrupted Recipe from the next checkpoint without replaying completed steps", async () => {
  const dispatched: Array<{ capability?: string; requestId?: string }> = [];
  const server = createServer(async (request, response) => {
    let text = "";
    for await (const chunk of request) text += String(chunk);
    const payload = text === "" ? {} : JSON.parse(text) as Record<string, unknown>;
    let data: Record<string, unknown> = {};
    if (request.url === "/api/v1/clients/register") {
      data = { sessionId: randomUUID(), heartbeatIntervalMs: 5_000, expiresAfterMs: 30_000 };
    } else if (request.url === "/api/execute") {
      const item = payload as { capability?: string; requestId?: string };
      dispatched.push({ capability: item.capability, requestId: item.requestId });
      data = { capability: item.capability, requestId: item.requestId, observed: true };
    }
    const body = JSON.stringify({ ok: true, data });
    response.writeHead(200, { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) });
    response.end(body);
  });
  await new Promise<void>((resolvePromise) => server.listen(0, "127.0.0.1", resolvePromise));
  const address = server.address();
  assert.ok(address && typeof address === "object");
  const root = mkdtempSync(join(tmpdir(), "ueai-recipe-recovery-"));
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    UEAI_RECIPE_ROOT: root,
    UEAI_RECIPE_TEST_MODE: "1",
    UEAI_RECIPE_TEST_LEASE_MS: "2000",
    UEAI_RECIPE_TEST_EXIT_AFTER_STEP: "first",
  };
  const runner = new RecipeRunner({
    env,
    endpoint: `http://127.0.0.1:${address.port}`,
  });
  const recipe = {
    schema: "ue.recipe.v2",
    id: "checkpoint-recovery",
    version: "1.0.0",
    steps: [
      { id: "first", kind: "capability", capability: "production.trace.status" },
      { id: "second", kind: "capability", capability: "production.trace.status" },
    ],
  };
  const waitFor = async (predicate: () => boolean, timeoutMs = 10_000): Promise<void> => {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (predicate()) return;
      await new Promise((resolvePromise) => setTimeout(resolvePromise, 25));
    }
    assert.fail("Timed out waiting for Recipe Runner state.");
  };
  try {
    const started = runner.start(recipe);
    await waitFor(() => {
      const status = runner.status(started.runId);
      return status.status === "interrupted";
    });
    const interrupted = runner.status(started.runId);
    assert.equal(interrupted.steps[0]?.status, "completed");
    assert.equal(interrupted.steps[0]?.attempts, 1);
    assert.equal(interrupted.steps[1]?.status, "pending");
    delete env.UEAI_RECIPE_TEST_EXIT_AFTER_STEP;
    runner.resume(started.runId, started.planDigest);
    await waitFor(() => runner.status(started.runId).status === "completed");
    const completed = runner.result(started.runId);
    assert.equal(completed.steps[0]?.attempts, 1);
    assert.equal(completed.steps[1]?.attempts, 1);
    assert.deepEqual(
      dispatched.map((item) => item.requestId),
      [`${started.runId}-first`, `${started.runId}-second`],
    );
  } finally {
    await new Promise<void>((resolvePromise) => server.close(() => resolvePromise()));
  }
});

test("recovers the Workflow acknowledgement gap with a stable requestId", async () => {
  const workflowDigest = `sha256:${"2".repeat(64)}`;
  const workflowRuns = new Map<string, string>();
  let workflowExecuteCalls = 0;
  let workflowWrites = 0;
  const server = createServer(async (request, response) => {
    let text = "";
    for await (const chunk of request) text += String(chunk);
    const payload = text === "" ? {} : JSON.parse(text) as Record<string, unknown>;
    let data: Record<string, unknown> = {};
    if (request.url === "/api/v1/clients/register") {
      data = { sessionId: randomUUID(), heartbeatIntervalMs: 5_000, expiresAfterMs: 30_000 };
    } else if (request.url === "/api/v1/workflow") {
      if (payload.action === "plan") {
        data = { planDigest: workflowDigest, executionReady: true };
      } else if (payload.action === "execute") {
        workflowExecuteCalls += 1;
        const requestId = String(payload.requestId);
        const existing = workflowRuns.get(requestId);
        if (existing) {
          data = { runId: existing, requestId, status: "completed", idempotentReplay: true };
        } else {
          const runId = randomUUID();
          workflowRuns.set(requestId, runId);
          workflowWrites += 1;
          data = { runId, requestId, status: "completed" };
        }
      }
    } else if (request.url === "/api/execute") {
      data = { checkedOut: true };
    }
    const body = JSON.stringify({ ok: true, data });
    response.writeHead(200, { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) });
    response.end(body);
  });
  await new Promise<void>((resolvePromise) => server.listen(0, "127.0.0.1", resolvePromise));
  const address = server.address();
  assert.ok(address && typeof address === "object");
  const root = mkdtempSync(join(tmpdir(), "ueai-workflow-idempotency-"));
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    UEAI_RECIPE_ROOT: root,
    UEAI_RECIPE_TEST_MODE: "1",
    UEAI_RECIPE_TEST_LEASE_MS: "2000",
    UEAI_RECIPE_TEST_EXIT_AFTER_WORKFLOW_ACK: "write",
  };
  const runner = new RecipeRunner({ env, endpoint: `http://127.0.0.1:${address.port}` });
  const recipe = {
    schema: "ue.recipe.v2",
    id: "workflow-ack-gap",
    version: "1.0.0",
    steps: [
      { id: "checkout", kind: "sourceControlCheckout", params: { files: ["Content/Fixture.uasset"] } },
      {
        id: "write",
        kind: "workflow",
        workflow: {
          dsl: "ue.workflow",
          dslVersion: "2.0",
          workflowKind: "assetEdit",
          workflowId: "idempotency-fixture",
          scopes: {},
          operations: [],
        },
      },
    ],
  };
  const planned = runner.plan(recipe);
  const recipeDigest = String(planned.planDigest);
  const waitFor = async (predicate: () => boolean, timeoutMs = 10_000): Promise<void> => {
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
      if (predicate()) return;
      await new Promise((resolvePromise) => setTimeout(resolvePromise, 25));
    }
    assert.fail("Timed out waiting for Workflow idempotency recovery state.");
  };
  try {
    const started = runner.start(recipe, {}, recipeDigest);
    await waitFor(() => runner.status(started.runId).status === "awaitingApproval");
    const approval = runner.status(started.runId).awaitingApproval;
    assert.equal(approval?.stepId, "write");
    assert.equal(approval?.planDigest, workflowDigest);
    runner.resume(started.runId, recipeDigest, workflowDigest);
    await waitFor(() => runner.status(started.runId).status === "interrupted");
    assert.equal(workflowWrites, 1);
    delete env.UEAI_RECIPE_TEST_EXIT_AFTER_WORKFLOW_ACK;
    runner.resume(started.runId, recipeDigest);
    await waitFor(() => runner.status(started.runId).status === "completed");
    assert.equal(workflowExecuteCalls, 2);
    assert.equal(workflowWrites, 1);
    assert.equal(workflowRuns.size, 1);
    assert.equal(runner.result(started.runId).steps[0]?.attempts, 1);
  } finally {
    await new Promise<void>((resolvePromise) => server.close(() => resolvePromise()));
  }
});

test("redacts credential-bearing project config values", async () => {
  const root = fixtureProject();
  const secrets = {
    encryptionKey: "base64-encryption-material",
    privateExponent: "private-rsa-exponent",
    androidToken: "android-file-server-token",
    clientSecret: "oauth-client-secret",
    password: "database-password",
    credential: "provider-credential",
  };
  writeFileSync(join(root, "Config", "DefaultCrypto.ini"), [
    "[/Script/CryptoKeys.CryptoKeysSettings]",
    `EncryptionKey=${secrets.encryptionKey}`,
    `SigningPrivateExponent=${secrets.privateExponent}`,
    "Enabled=True",
  ].join("\n"));
  writeFileSync(join(root, "Config", "DefaultAndroidFileServer.ini"), [
    "[/Script/AndroidFileServerEditor.AndroidFileServerRuntimeSettings]",
    `SecurityToken=${secrets.androidToken}`,
    "ConnectionType=USBOnly",
  ].join("\n"));
  writeFileSync(join(root, "Config", "DefaultOAuth.ini"), [
    "[OAuth]",
    `ClientSecret=${secrets.clientSecret}`,
    `Password=${secrets.password}`,
    `ProviderCredential=${secrets.credential}`,
    "Endpoint=https://auth.example.invalid",
  ].join("\n"));

  const result = await new LocalProjectExecutor().execute(
    "production.project.config.get",
    { projectRoot: root },
  ) as {
    merged: Record<string, Record<string, { value: string; source: string; present?: boolean; valueDigest?: string }>>;
  };
  const serialized = JSON.stringify(result);
  for (const secret of Object.values(secrets)) assert.equal(serialized.includes(secret), false);

  const crypto = result.merged["/Script/CryptoKeys.CryptoKeysSettings"]!;
  assert.deepEqual(crypto.EncryptionKey, {
    value: "<redacted>",
    present: true,
    valueDigest: `sha256:${createHash("sha256").update(secrets.encryptionKey).digest("hex")}`,
    source: join("Config", "DefaultCrypto.ini"),
  });
  assert.equal(crypto.Enabled?.value, "True");
  assert.equal(result.merged.OAuth?.Endpoint?.value, "https://auth.example.invalid");
});

test("runs UE package header reads in the isolated Asset Worker", async () => {
  const root = fixtureProject();
  const asset = join(root, "Header.uasset");
  writeFileSync(asset, ue53PackageWithRegistryTag());
  const result = await new LocalAssetExecutor().execute(
    "production.asset.package.summary.get",
    { projectRoot: root, assetPath: "Header.uasset", engineVersion: "5.3", detail: "full", sections: ["assetRegistry"] },
  );
  assert.equal((result.packageSummary as Record<string, unknown>).tag, "0x9e2a83c1");
  const registry = (result.sections as { assetRegistry: { objectCount: number; assets: Array<{ tags: Array<{ key: string; value: string }> }> } }).assetRegistry;
  assert.equal(registry.objectCount, 1);
  assert.equal(
    registry.assets[0]?.tags.find((tag) => tag.key === "SourceFile")?.value,
    "x.png",
  );
});

test("keeps default Asset summaries inside the token budget and full tables valid", async () => {
  const root = fixtureProject();
  const asset = join(root, "LargeHeader.uasset");
  writeFileSync(asset, ue53PackageWithRegistryTag(128, "x".repeat(4096)));
  const executor = new LocalAssetExecutor();
  const compact = await executor.execute(
    "production.asset.package.summary.get",
    { projectRoot: root, assetPath: "LargeHeader.uasset", engineVersion: "5.3" },
  );
  assert.equal(compact.schema, "ue.local-asset-summary.v3");
  assert.equal("sections" in compact, false);
  assert.ok(Buffer.byteLength(JSON.stringify(compact), "utf8") <= 8192);
  assert.equal(typeof compact.truncated, "boolean");
  assert.ok(Array.isArray(compact.omittedSections));
  const full = await executor.execute(
    "production.asset.package.summary.get",
    {
      projectRoot: root,
      assetPath: "LargeHeader.uasset",
      engineVersion: "5.3",
      detail: "full",
      sections: ["assetRegistry"],
      limit: 64,
      targetTokens: 4096,
      maxBytes: 16384,
    },
  );
  assert.ok(Buffer.byteLength(JSON.stringify(full), "utf8") <= 16384);
  assert.equal(typeof (full.budget as { estimatedBytes: number }).estimatedBytes, "number");

  const paged = await executor.execute(
    "production.asset.package.summary.get",
    {
      projectRoot: root,
      assetPath: "LargeHeader.uasset",
      engineVersion: "5.3",
      detail: "full",
      sections: ["names", "imports", "exports", "assetRegistry", "dependencies"],
      limit: 2,
      targetTokens: 32768,
      maxBytes: 1048576,
    },
  ) as { sections: Record<string, Record<string, unknown>> };
  for (const sectionName of ["names", "imports", "exports", "assetRegistry", "dependencies"]) {
    const section = paged.sections[sectionName]!;
    assert.equal(typeof section.returned, "number", `${sectionName}.returned`);
    assert.equal(typeof section.total, "number", `${sectionName}.total`);
    assert.ok(section.nextOffset === null || typeof section.nextOffset === "number", `${sectionName}.nextOffset`);
  }

  const compactDiff = await executor.execute(
    "production.asset.package.diff",
    {
      projectRoot: root,
      assetPath: "LargeHeader.uasset",
      otherAssetPath: "LargeHeader.uasset",
      engineVersion: "5.3",
    },
  );
  assert.equal(compactDiff.schema, "ue.local-asset-diff.v3");
  assert.equal("sections" in compactDiff, false);
  assert.deepEqual(compactDiff.changedSections, []);
});

test("resolves project, additional, and Engine plugin dependencies offline", async () => {
  const root = fixtureProject();
  const engine = mkdtempSync(join(tmpdir(), "ueai-engine-"));
  mkdirSync(join(engine, "Engine", "Build"), { recursive: true });
  mkdirSync(join(engine, "Engine", "Plugins", "FX", "Niagara"), { recursive: true });
  mkdirSync(join(engine, "Engine", "Plugins", "Runtime", "OpenXR"), { recursive: true });
  writeFileSync(join(engine, "Engine", "Build", "Build.version"), JSON.stringify({ MajorVersion: 5, MinorVersion: 3, PatchVersion: 2 }));
  writeFileSync(join(engine, "Engine", "Plugins", "FX", "Niagara", "Niagara.uplugin"), JSON.stringify({
    Version: 1,
    Plugins: [
      { Name: "OpenXR", Enabled: true },
      { Name: "MissingOptional", Enabled: true, Optional: true },
    ],
  }));
  writeFileSync(join(engine, "Engine", "Plugins", "Runtime", "OpenXR", "OpenXR.uplugin"), JSON.stringify({ Version: 2 }));
  mkdirSync(join(engine, "Engine", "Plugins", "Runtime", "LocalFeature"), { recursive: true });
  writeFileSync(join(engine, "Engine", "Plugins", "Runtime", "LocalFeature", "LocalFeature.uplugin"), JSON.stringify({ VersionName: "engine-copy" }));
  mkdirSync(join(root, "Plugins", "Company", "LocalFeature"), { recursive: true });
  writeFileSync(join(root, "Plugins", "Company", "LocalFeature", "LocalFeature.uplugin"), JSON.stringify({ VersionName: "1.0" }));
  mkdirSync(join(root, "ExtraPlugins", "ExtraFeature"), { recursive: true });
  writeFileSync(join(root, "ExtraPlugins", "ExtraFeature", "ExtraFeature.uplugin"), JSON.stringify({ VersionName: "2.0" }));
  writeFileSync(join(root, "Fixture.uproject"), JSON.stringify({
    EngineAssociation: "5.3",
    AdditionalPluginDirectories: ["ExtraPlugins"],
    Plugins: [
      { Name: "Niagara", Enabled: true },
      { Name: "LocalFeature", Enabled: true },
      { Name: "ExtraFeature", Enabled: true },
    ],
  }));
  const result = await new LocalProjectExecutor().execute(
    "production.project.validate",
    { projectRoot: root, engineRoot: engine },
  ) as { valid: boolean; plugins: Array<{ name: string; source: string }>; diagnostics: Array<{ code: string; severity: string; plugin?: string }> };
  assert.equal(result.valid, true);
  assert.equal(result.plugins.find((plugin) => plugin.name === "Niagara")?.source, "engine");
  assert.equal(result.plugins.find((plugin) => plugin.name === "OpenXR")?.source, "engine");
  assert.equal(result.plugins.find((plugin) => plugin.name === "LocalFeature")?.source, "project");
  assert.equal(result.plugins.find((plugin) => plugin.name === "ExtraFeature")?.source, "additional");
  assert.equal(result.diagnostics.some((diagnostic) => diagnostic.code === "plugin_not_project_local"), false);
  assert.ok(result.diagnostics.some((diagnostic) => diagnostic.code === "plugin_shadowed" && diagnostic.plugin === "LocalFeature"));
  assert.ok(result.diagnostics.some((diagnostic) => diagnostic.code === "plugin_optional_dependency_missing" && diagnostic.severity === "warning"));

  writeFileSync(join(root, "Fixture.uproject"), JSON.stringify({
    EngineAssociation: "5.3",
    Plugins: [{ Name: "MissingExternal", Enabled: true }],
  }));
  const withoutEngine = await new LocalProjectExecutor().execute(
    "production.project.validate",
    { projectRoot: root },
  ) as { valid: boolean; diagnostics: Array<{ code: string; severity: string }> };
  assert.equal(withoutEngine.valid, true);
  assert.ok(withoutEngine.diagnostics.some(
    (diagnostic) => diagnostic.code === "plugin_unresolved_external" && diagnostic.severity === "info",
  ));
  const missingRequired = await new LocalProjectExecutor().execute(
    "production.project.validate",
    { projectRoot: root, engineRoot: engine },
  ) as { valid: boolean; diagnostics: Array<{ code: string; severity: string }> };
  assert.equal(missingRequired.valid, false);
  assert.ok(missingRequired.diagnostics.some(
    (diagnostic) => diagnostic.code === "plugin_dependency_missing" && diagnostic.severity === "error",
  ));
});

test("propagates AbortSignal and posts best-effort Editor cancellation", async () => {
  const calls: string[] = [];
  const fetchImpl: typeof fetch = async (input, init) => {
    const url = String(input);
    calls.push(url);
    if (url.endsWith("/api/execute/cancel")) {
      return new Response(JSON.stringify({ ok: true, data: { cancelPending: true } }), { status: 200, headers: { "Content-Type": "application/json" } });
    }
    return new Promise<Response>((_resolve, reject) => {
      init?.signal?.addEventListener("abort", () => reject(new Error("aborted")), { once: true });
    });
  };
  const controller = new AbortController();
  const client = new UEClient({ baseUrl: "http://127.0.0.1:19847", fetchImpl });
  const pending = client.execute("production.job.status", { jobId: "x" }, "cancel-me", { signal: controller.signal });
  controller.abort();
  await assert.rejects(pending, (error: unknown) => error instanceof UEApiError
    && error.code === "request_cancelled"
    && (error.details as { cancelPending?: boolean }).cancelPending === true);
  assert.ok(calls.some((url) => url.endsWith("/api/execute/cancel")));
});

test("catalog returns stable tombstone replacement and canonical lifecycle", () => {
  const catalog = loadCapabilityCatalog();
  assert.equal(catalog.removed("blueprint.node.comment.set")?.replacement, "blueprint.comment.title.set");
  assert.equal(catalog.get("blueprint.layout.straighten")?.lifecycle.status, "deprecated");
  assert.equal(catalog.get("blueprint.selection.set")?.effects.editorSession, "write");
  assert.equal(catalog.get("blueprint.selection.set")?.effects.asset, "read");
});

test("discovers opted-in Development targets and requires the exact attach digest", async () => {
  const root = fixtureProject();
  const directory = join(root, "Saved", "UEAI", "DevelopmentBridge");
  mkdirSync(directory, { recursive: true });
  writeFileSync(join(directory, `${process.pid}.json`), JSON.stringify({
    schema: "ue.development-bridge.v1",
    pid: process.pid,
    processStartTime: "2026-08-04T00:00:00Z",
    buildId: "fixture-build",
    projectDigest: `sha256:${"1".repeat(64)}`,
    endpoint: "\\\\.\\pipe\\ueai-development-fixture",
    transport: "namedPipe",
    pairingToken: "single-use-fixture-token",
    pairingExpiresAtUtc: new Date(Date.now() + 60_000).toISOString(),
    observePlanDigest: `sha256:${"2".repeat(64)}`,
    controlPlanDigest: `sha256:${"3".repeat(64)}`,
    shippingExcluded: true,
    httpEnabled: false,
  }), "utf8");
  const executor = new DevelopmentBridgeExecutor();
  const listed = await executor.execute("production.development.target.list", { projectRoot: root });
  assert.equal((listed.targets as unknown[]).length, 1);
  const attachPlan = await executor.execute("production.development.attach.plan", { projectRoot: root, pid: process.pid, scope: "observe" });
  assert.equal(attachPlan.planDigest, `sha256:${"2".repeat(64)}`);
  await assert.rejects(
    executor.execute("production.development.attach", { projectRoot: root, pid: process.pid, scope: "observe", approvePlanDigest: `sha256:${"4".repeat(64)}`, confirmAttach: true }),
    (error: unknown) => error instanceof UEApiError && error.code === "attach_approval_required",
  );
});
