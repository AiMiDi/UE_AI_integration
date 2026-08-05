import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtempSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { test } from "node:test";

import { loadCapabilityCatalog } from "../capability-catalog.js";
import { LocalAssetExecutor, LocalProjectExecutor } from "../project-executor.js";
import { DevelopmentBridgeExecutor } from "../development-bridge.js";
import { validateRecipe } from "../recipe-runner.js";
import { parseSalSource, salLint, salPlan } from "../sal-runner.js";
import { UEApiError, UEClient } from "../ue-bridge.js";

function fixtureProject(): string {
  const root = mkdtempSync(join(tmpdir(), "ueai-offline-"));
  mkdirSync(join(root, "Config"));
  writeFileSync(join(root, "Fixture.uproject"), JSON.stringify({ EngineAssociation: "5.3", Plugins: [] }), "utf8");
  writeFileSync(join(root, "Config", "DefaultEngine.ini"), "[/Script/Engine.Engine]\nr.DefaultFeature.AutoExposure=False\n", "utf8");
  return root;
}

function ue53PackageWithRegistryTag(): Buffer {
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
  i64(0); i32(1); string("Header"); string("/Script/Engine.Texture2D"); i32(1); string("SourceFile"); string("x.png");
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
    { projectRoot: root, assetPath: "Header.uasset", engineVersion: "5.3" },
  );
  assert.equal((result.packageSummary as Record<string, unknown>).tag, "0x9e2a83c1");
  assert.equal((result.assetRegistry as { objectCount: number }).objectCount, 1);
  assert.equal(
    (((result.assetRegistry as { assets: Array<{ tags: Record<string, string> }> }).assets[0]?.tags) ?? {}).SourceFile,
    "x.png",
  );
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
  await assert.rejects(pending, (error: unknown) => error instanceof UEApiError && error.code === "request_cancelled");
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
