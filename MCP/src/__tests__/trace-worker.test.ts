import assert from "node:assert/strict";
import { createHash, randomUUID } from "node:crypto";
import {
  chmodSync,
  copyFileSync,
  linkSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  realpathSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import test from "node:test";

import { loadCapabilityCatalog } from "../capability-catalog.js";
import { BackendRoutingExecutor } from "../domain-router.js";
import { createMcpServer } from "../mcp-server.js";
import {
  deriveTraceWorkerEndpoint,
  locateTraceWorker,
  TraceWorkerClient,
} from "../trace-worker.js";
import { UEApiError } from "../ue-bridge.js";

function serviceEndpoint(directory: string): string {
  return process.platform === "win32"
    ? `\\\\.\\pipe\\ue-trace-worker-test-${process.pid}-${randomUUID()}`
    : join(directory, "worker.sock");
}

const workerScript = `import net from "node:net";
import fs from "node:fs";
const endpointArg = process.argv.find(value => value.startsWith("--endpoint="));
const engineArg = process.argv.find(value => /^-{1,2}EngineDir=/.test(value));
const engineDir = engineArg ? engineArg.replace(/^-{1,2}EngineDir=/, "") : "";
const insightsPath = engineDir ? engineDir + (process.platform === "win32" ? "\\\\Binaries\\\\Win64\\\\UnrealInsights.exe" : "/Binaries/Linux/UnrealInsights") : "";
const handle = request => {
  const data = request.action === "handshake"
    ? {
        schema: "ue.trace-worker-handshake.v1",
        protocolVersion: 1,
        workerVersion: "0.9.0",
        engineVersion: "5.3.2-fixture",
        contractBound: true,
        contractDigest: process.env.FAKE_CONTRACT_DIGEST || "sha256:" + "1".repeat(64),
        providerSchemaDigest: process.env.FAKE_PROVIDER_DIGEST || "sha256:" + "2".repeat(64),
        maximumResidentSessions: process.argv.includes("--serve") ? 2 : 1,
        transport: process.argv.includes("--serve")
          ? (process.platform === "win32" ? "named-pipe" : "unix-socket")
          : "stdio-one-shot",
        storeRoot: process.env.FAKE_STORE_ROOT,
        serverPid: process.pid,
        unrealInsightsAvailable: Boolean(engineDir),
        unrealInsightsPath: insightsPath,
        unrealInsightsEngineDir: engineDir,
        unrealInsightsEngineVersion: "5.3",
        unrealInsightsSource: "commandLine",
        noLog: process.argv.includes("-NoLog"),
        noDefaultLog: process.argv.includes("-NoDefaultLog"),
        saveToUserDir: process.argv.includes("-SaveToUserDir")
      }
    : { capability: request.capability, params: request.params, backend: "localTrace", serverPid: process.pid };
  return {
    schema: "ue.trace-worker-response.v1",
    ok: true,
    data,
    meta: { requestId: request.requestId, workerVersion: "0.9.0", engineVersion: "5.3.2-fixture", backend: "localTrace" }
  };
};
if (process.argv.includes("--stdio")) {
  let input="";
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", chunk => input += chunk);
  process.stdin.on("end", () => process.stdout.write(JSON.stringify(handle(JSON.parse(input)))));
} else if (process.argv.includes("--serve") && endpointArg) {
  const endpoint = endpointArg.slice("--endpoint=".length);
  if (process.platform !== "win32") { try { fs.unlinkSync(endpoint); } catch {} }
  const server = net.createServer(socket => {
    let bytes = Buffer.alloc(0);
    socket.on("data", chunk => {
      bytes = Buffer.concat([bytes, chunk]);
      if (bytes.length < 4) return;
      const length = bytes.readUInt32LE(0);
      if (bytes.length < length + 4) return;
      const response = Buffer.from(JSON.stringify(handle(JSON.parse(bytes.subarray(4, length + 4).toString("utf8")))), "utf8");
      const header = Buffer.alloc(4); header.writeUInt32LE(response.length, 0);
      socket.end(Buffer.concat([header, response]));
    });
  });
  let idle;
  const reset = () => { clearTimeout(idle); idle = setTimeout(() => server.close(), 50); };
  server.on("connection", reset);
  server.listen(endpoint, reset);
} else {
  process.exitCode = 2;
}`;

const repositoryRoot = resolve(
  dirname(fileURLToPath(import.meta.url)),
  "..",
  "..",
  "..",
);

interface FakeWorkerBundle {
  root: string;
  executable: string;
  moduleUrl: string;
  env: NodeJS.ProcessEnv;
  resource(relativePath: string): string;
  refreshDigests(): void;
}

function fakeWorkerBundle(directory: string): FakeWorkerBundle {
  const root = join(directory, "Plugin");
  const platform =
    process.platform === "win32"
      ? "Win64"
      : process.platform === "darwin"
        ? "Mac"
        : "Linux";
  const executable = join(
    root,
    "Tools",
    "Trace",
    platform,
    "5.3",
    process.platform === "win32" ? "UEAITraceWorker.exe" : "UEAITraceWorker",
  );
  mkdirSync(dirname(executable), { recursive: true });
  try {
    linkSync(process.execPath, executable);
  } catch {
    copyFileSync(process.execPath, executable);
  }
  if (process.platform !== "win32") chmodSync(executable, 0o755);

  const relativeFiles = [
    "Resources/Capabilities/production.json",
    "Resources/Trace/insights-actions.5.3.json",
    "Resources/Trace/launch-profiles.json",
    "Resources/Trace/worker-protocol.v1.json",
  ].sort();
  for (const relativePath of relativeFiles) {
    const destination = join(root, relativePath);
    mkdirSync(dirname(destination), { recursive: true });
    copyFileSync(join(repositoryRoot, relativePath), destination);
  }
  const env: NodeJS.ProcessEnv = {
    ...process.env,
    UE_ENGINE_VERSION: "5.3",
  };
  const engineRoot = join(directory, "EngineRoot");
  const engineDirectory = join(engineRoot, "Engine");
  mkdirSync(join(engineDirectory, "Build"), { recursive: true });
  writeFileSync(
    join(engineDirectory, "Build", "Build.version"),
    JSON.stringify({ MajorVersion: 5, MinorVersion: 3, PatchVersion: 0 }),
    "utf8",
  );
  const insightsPath =
    process.platform === "darwin"
      ? join(
          engineDirectory,
          "Binaries",
          "Mac",
          "UnrealInsights.app",
          "Contents",
          "MacOS",
          "UnrealInsights",
        )
      : join(
          engineDirectory,
          "Binaries",
          process.platform === "win32" ? "Win64" : "Linux",
          process.platform === "win32" ? "UnrealInsights.exe" : "UnrealInsights",
        );
  mkdirSync(dirname(insightsPath), { recursive: true });
  writeFileSync(insightsPath, "fixture", "utf8");
  env.UEAI_ENGINE_ROOT = engineRoot;
  const refreshDigests = () => {
    const contract = createHash("sha256");
    for (const relativePath of relativeFiles) {
      contract.update(relativePath.replaceAll("\\", "/"), "utf8");
      contract.update(Buffer.from([0]));
      contract.update(readFileSync(join(root, relativePath)));
      contract.update(Buffer.from([0]));
    }
    env.FAKE_CONTRACT_DIGEST = `sha256:${contract.digest("hex")}`;
    env.FAKE_PROVIDER_DIGEST = `sha256:${createHash("sha256")
      .update(readFileSync(join(root, "Resources/Trace/insights-actions.5.3.json")))
      .digest("hex")}`;
  };
  refreshDigests();
  return {
    root,
    executable,
    moduleUrl: pathToFileURL(join(root, "MCP", "dist", "trace-worker.js")).href,
    env,
    resource: (relativePath: string) => join(root, relativePath),
    refreshDigests,
  };
}

async function allowFakeServiceToExit(): Promise<void> {
  await new Promise((resolveDelay) => setTimeout(resolveDelay, 100));
}

function rewriteJson(
  path: string,
  mutate: (value: Record<string, unknown>) => void,
): void {
  const value = JSON.parse(readFileSync(path, "utf8")) as Record<string, unknown>;
  mutate(value);
  writeFileSync(path, `${JSON.stringify(value, null, 2)}\n`, "utf8");
}

test("locates packaged and source-built Trace Workers deterministically", () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-locator-"));
  const platform =
    process.platform === "win32"
      ? "Win64"
      : process.platform === "darwin"
        ? "Mac"
        : "Linux";
  const name = process.platform === "win32" ? "UEAITraceWorker.exe" : "UEAITraceWorker";
  const moduleUrl = pathToFileURL(
    join(directory, "MCP", "dist", "trace-worker.js"),
  ).href;
  const packaged = join(directory, "Tools", "Trace", platform, "5.3", name);
  mkdirSync(dirname(packaged), { recursive: true });
  writeFileSync(packaged, "fixture", "utf8");
  try {
    const installed = locateTraceWorker({
      moduleUrl,
      env: { UE_ENGINE_VERSION: "5.3" },
    });
    assert.equal(installed.path, packaged);
    assert.equal(installed.source, "installed");
    rmSync(join(directory, "Tools"), { recursive: true, force: true });
    const source = join(
      directory,
      "Programs",
      "UEAITraceWorker",
      "Binaries",
      platform,
      name,
    );
    mkdirSync(dirname(source), { recursive: true });
    writeFileSync(source, "fixture", "utf8");
    const staged = join(
      directory,
      "Intermediate",
      "TraceWorkerStage",
      "Tools",
      "Trace",
      platform,
      "5.3",
      name,
    );
    mkdirSync(dirname(staged), { recursive: true });
    writeFileSync(staged, "fixture", "utf8");
    const stagedBuild = locateTraceWorker({
      moduleUrl,
      env: { UE_ENGINE_VERSION: "5.3" },
    });
    assert.equal(stagedBuild.path, staged);
    assert.equal(stagedBuild.source, "source");
    assert.equal(stagedBuild.engineVersion, "5.3");

    rmSync(join(directory, "Intermediate"), { recursive: true, force: true });
    const built = locateTraceWorker({
      moduleUrl,
      env: { UE_ENGINE_VERSION: "5.3" },
    });
    assert.equal(built.path, source);
    assert.equal(built.source, "source");
    assert.equal(built.engineVersion, "5.3");
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("binds service endpoints to the canonical Engine directory", () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-endpoint-engine-"));
  const worker = process.execPath;
  const first = join(directory, "First", "Engine");
  const second = join(directory, "Second", "Engine");
  mkdirSync(first, { recursive: true });
  mkdirSync(second, { recursive: true });
  try {
    const endpointA = deriveTraceWorkerEndpoint(worker, process.env, "5.3", first);
    const endpointAEquivalent = deriveTraceWorkerEndpoint(
      worker,
      process.env,
      "5.3",
      resolve(first, "."),
    );
    const endpointB = deriveTraceWorkerEndpoint(worker, process.env, "5.3", second);
    assert.equal(endpointA, endpointAEquivalent);
    assert.notEqual(endpointA, endpointB);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("reuses the user-scoped Trace Worker service and keeps stdio diagnostic fallback", async () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-worker-test-"));
  const script = join(directory, "worker.mjs");
  const bundle = fakeWorkerBundle(directory);
  writeFileSync(script, workerScript, "utf8");
  try {
    const client = new TraceWorkerClient({
      executable: bundle.executable,
      workerArgs: [script],
      serviceEndpoint: serviceEndpoint(directory),
      timeoutMs: 5_000,
      moduleUrl: bundle.moduleUrl,
      env: bundle.env,
    });
    const handshake = await client.handshake();
    assert.equal(handshake.protocolVersion, 1);
    assert.equal(handshake.noLog, true);
    assert.equal(handshake.noDefaultLog, true);
    assert.equal(handshake.saveToUserDir, true);
    assert.equal(
      resolve(String(handshake.unrealInsightsEngineDir)),
      realpathSync.native(String(bundle.env.UEAI_ENGINE_ROOT) + sep + "Engine"),
    );
    const serverPid = handshake.serverPid;
    const result = await client.execute(
      "production.trace.provider.list",
      { tracePath: "sample.utrace", backend: "local" },
      "request-1",
    );
    assert.deepEqual(result, {
      capability: "production.trace.provider.list",
      params: { tracePath: "sample.utrace", backend: "local" },
      backend: "localTrace",
      serverPid,
    });
    const stdio = new TraceWorkerClient({
      executable: bundle.executable,
      workerArgs: [script],
      transport: "stdio",
      timeoutMs: 5_000,
      moduleUrl: bundle.moduleUrl,
      env: bundle.env,
    });
    assert.equal(
      (await stdio.handshake()).protocolVersion,
      1,
    );
  } finally {
    await allowFakeServiceToExit();
    rmSync(directory, { recursive: true, force: true });
  }
});

test("fails closed when UEAI_TRACE_WORKER has no Worker-bundled Resources", async () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-worker-unbound-"));
  const script = join(directory, "worker.mjs");
  writeFileSync(script, workerScript, "utf8");
  try {
    const client = new TraceWorkerClient({
      workerArgs: [script],
      transport: "stdio",
      timeoutMs: 5_000,
      env: {
        ...process.env,
        UEAI_TRACE_WORKER: process.execPath,
        UE_ENGINE_VERSION: "5.3",
      },
    });
    await assert.rejects(
      client.handshake(true),
      (error: unknown) =>
        error instanceof UEApiError &&
        error.code === "trace_worker_contract_mismatch" &&
        /Worker-bundled contract resources/.test(error.message),
    );
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("rejects a valid but divergent Worker resource bundle", async () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-worker-divergent-"));
  const script = join(directory, "worker.mjs");
  const bundle = fakeWorkerBundle(directory);
  writeFileSync(script, workerScript, "utf8");
  rewriteJson(
    bundle.resource("Resources/Capabilities/production.json"),
    (production) => {
      production.testOnlyDivergence = true;
    },
  );
  bundle.refreshDigests();
  try {
    const client = new TraceWorkerClient({
      executable: bundle.executable,
      workerArgs: [script],
      transport: "stdio",
      timeoutMs: 5_000,
      moduleUrl: pathToFileURL(
        join(repositoryRoot, "MCP", "dist", "trace-worker.js"),
      ).href,
      env: bundle.env,
    });
    await assert.rejects(
      client.handshake(true),
      (error: unknown) =>
        error instanceof UEApiError &&
        error.code === "trace_worker_contract_mismatch" &&
        /do not match the MCP-owned resources/.test(error.message),
    );
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("validates every trusted Trace contract resource before accepting its digest", async (t) => {
  const cases: Array<{
    name: string;
    relativePath: string;
    mutate(value: Record<string, unknown>): void;
  }> = [
    {
      name: "worker protocol",
      relativePath: "Resources/Trace/worker-protocol.v1.json",
      mutate: (value) => {
        value.requestSchema = "untrusted.request.v1";
      },
    },
    {
      name: "resident service policy",
      relativePath: "Resources/Trace/worker-protocol.v1.json",
      mutate: (value) => {
        const resident = value.residentService as Record<string, unknown>;
        resident.tcp = true;
        resident.analysisSessionCacheCapacity = 0;
        resident.analysisSessionPolicy = "perRequest";
      },
    },
    {
      name: "production manifest",
      relativePath: "Resources/Capabilities/production.json",
      mutate: (value) => {
        const capabilities = value.capabilities as Array<Record<string, unknown>>;
        value.capabilities = capabilities.filter(
          (capability) => capability.id !== "production.trace.timing.query",
        );
      },
    },
    {
      name: "engine Insights mapping",
      relativePath: "Resources/Trace/insights-actions.5.3.json",
      mutate: (value) => {
        value.engineVersion = "5.4";
      },
    },
    {
      name: "missing Insights panel",
      relativePath: "Resources/Trace/insights-actions.5.3.json",
      mutate: (value) => {
        const panels = value.panels as Array<Record<string, unknown>>;
        value.panels = panels.filter(
          (panel) => panel.capability !== "production.trace.screenshot.query",
        );
      },
    },
    {
      name: "missing Insights operation",
      relativePath: "Resources/Trace/insights-actions.5.3.json",
      mutate: (value) => {
        const panels = value.panels as Array<Record<string, unknown>>;
        const timing = panels.find(
          (panel) => panel.capability === "production.trace.timing.query",
        );
        const operations = timing?.operations as string[];
        timing!.operations = operations.filter(
          (operation) => operation !== "cpuSampling",
        );
      },
    },
    {
      name: "launch profiles",
      relativePath: "Resources/Trace/launch-profiles.json",
      mutate: (value) => {
        value.schema = "untrusted.launch-profiles.v1";
      },
    },
  ];
  for (const testCase of cases) {
    await t.test(testCase.name, async () => {
      const directory = mkdtempSync(join(tmpdir(), "ue-trace-resource-invalid-"));
      const script = join(directory, "worker.mjs");
      const bundle = fakeWorkerBundle(directory);
      writeFileSync(script, workerScript, "utf8");
      rewriteJson(bundle.resource(testCase.relativePath), testCase.mutate);
      bundle.refreshDigests();
      try {
        const client = new TraceWorkerClient({
          executable: bundle.executable,
          workerArgs: [script],
          transport: "stdio",
          timeoutMs: 5_000,
          moduleUrl: bundle.moduleUrl,
          env: bundle.env,
        });
        await assert.rejects(
          client.handshake(true),
          (error: unknown) =>
            error instanceof UEApiError &&
            error.code === "trace_worker_contract_mismatch" &&
            /missing, invalid, or outside/.test(error.message),
        );
      } finally {
        rmSync(directory, { recursive: true, force: true });
      }
    });
  }
});

test("rejects handshake digests that differ from validated bundled Resources", async () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-worker-digest-"));
  const script = join(directory, "worker.mjs");
  const bundle = fakeWorkerBundle(directory);
  writeFileSync(script, workerScript, "utf8");
  bundle.env.FAKE_CONTRACT_DIGEST = `sha256:${"f".repeat(64)}`;
  try {
    const client = new TraceWorkerClient({
      executable: bundle.executable,
      workerArgs: [script],
      transport: "stdio",
      timeoutMs: 5_000,
      moduleUrl: bundle.moduleUrl,
      env: bundle.env,
    });
    await assert.rejects(
      client.handshake(true),
      (error: unknown) =>
        error instanceof UEApiError &&
        error.code === "trace_worker_contract_mismatch" &&
        /digest does not match/.test(error.message),
    );
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("restricts MCP imports to Saved, Worker store, and declared roots", async () => {
  const directory = mkdtempSync(join(tmpdir(), "ue-trace-import-roots-"));
  const script = join(directory, "worker.mjs");
  const project = join(directory, "Project");
  const saved = join(project, "Saved");
  const store = join(directory, "Store");
  const declared = join(directory, "Declared");
  const outside = join(directory, "Outside");
  const bundle = fakeWorkerBundle(directory);
  for (const path of [saved, store, declared, outside]) mkdirSync(path, { recursive: true });
  writeFileSync(script, workerScript, "utf8");
  const files = [saved, store, declared, outside].map((root, index) => {
    const path = join(root, `${index}.utrace`);
    writeFileSync(path, "fixture", "utf8");
    return path;
  });
  try {
    const client = new TraceWorkerClient({
      executable: bundle.executable,
      workerArgs: [script],
      serviceEndpoint: serviceEndpoint(directory),
      timeoutMs: 5_000,
      moduleUrl: bundle.moduleUrl,
      restrictImportRoots: true,
      projectRoot: project,
      env: {
        ...bundle.env,
        FAKE_STORE_ROOT: store,
        UEAI_TRACE_ROOTS: declared,
      },
    });
    for (const tracePath of files.slice(0, 3)) {
      const result = await client.execute("production.trace.import", { tracePath });
      assert.equal(result.capability, "production.trace.import");
    }
    await assert.rejects(
      client.execute("production.trace.import", { tracePath: files[3] }),
      (error: unknown) =>
        error instanceof UEApiError && error.code === "trace_path_not_allowed",
    );
  } finally {
    await allowFakeServiceToExit();
    rmSync(directory, { recursive: true, force: true });
  }
});

test("routes declared localTrace capabilities and honors explicit backend selection", async () => {
  const catalog = loadCapabilityCatalog();
  const descriptor = catalog.get("production.trace.analyze");
  assert.ok(descriptor);
  descriptor.execution = {
    backends: ["editor", "localTrace"],
    preferred: "localTrace",
  };
  const calls: string[] = [];
  const router = new BackendRoutingExecutor(
    catalog,
    {
      execute: async () => {
        calls.push("editor");
        return { backend: "editor" };
      },
    },
    {
      execute: async () => {
        calls.push("local");
        return { backend: "localTrace" };
      },
    },
  );

  assert.deepEqual(await router.execute(descriptor.id, {}), {
    backend: "localTrace",
  });
  assert.deepEqual(
    await router.execute(descriptor.id, { backend: "editor" }),
    { backend: "editor" },
  );
  assert.deepEqual(calls, ["local", "editor"]);
});

test("auto falls back only when the local worker is unavailable", async () => {
  const catalog = loadCapabilityCatalog();
  const descriptor = catalog.get("production.trace.analyze");
  assert.ok(descriptor);
  descriptor.execution = {
    backends: ["editor", "localTrace"],
    preferred: "localTrace",
  };
  let editorCalls = 0;
  const router = new BackendRoutingExecutor(
    catalog,
    {
      execute: async () => {
        editorCalls += 1;
        return { backend: "editor" };
      },
    },
    {
      execute: async () => {
        throw new UEApiError({
          code: "trace_worker_unavailable",
          message: "missing",
        });
      },
    },
  );
  assert.deepEqual(await router.execute(descriptor.id, { backend: "auto" }), {
    backend: "editor",
  });
  assert.equal(editorCalls, 1);
  await assert.rejects(
    router.execute(descriptor.id, { backend: "local" }),
    (error: unknown) =>
      error instanceof UEApiError &&
      error.code === "trace_worker_unavailable",
  );
});

test("editor-preferred auto falls back only when the Editor is unreachable", async () => {
  const catalog = loadCapabilityCatalog();
  const descriptor = catalog.get("production.trace.target.list");
  assert.ok(descriptor);
  descriptor.execution = {
    backends: ["editor", "localTrace"],
    preferred: "editor",
  };
  let editorError = "editor_unreachable";
  let localCalls = 0;
  const router = new BackendRoutingExecutor(
    catalog,
    {
      execute: async () => {
        throw new UEApiError({
          code: editorError,
          message: "Editor result",
        });
      },
    },
    {
      execute: async () => {
        localCalls += 1;
        return { backend: "localTrace" };
      },
    },
  );
  assert.deepEqual(await router.execute(descriptor.id, { backend: "auto" }), {
    backend: "localTrace",
  });
  assert.equal(localCalls, 1);
  await assert.rejects(
    router.execute(descriptor.id, { backend: "editor" }),
    (error: unknown) =>
      error instanceof UEApiError && error.code === "editor_unreachable",
  );
  assert.equal(localCalls, 1);
  editorError = "invalid_request";
  await assert.rejects(
    router.execute(descriptor.id, { backend: "auto" }),
    (error: unknown) =>
      error instanceof UEApiError && error.code === "invalid_request",
  );
  assert.equal(localCalls, 1);
});

test("binds trace starts and durable IDs to their owning backend", async () => {
  const catalog = loadCapabilityCatalog();
  const start = catalog.get("production.trace.start");
  assert.ok(start);
  start.execution = {
    backends: ["editor", "localTrace"],
    preferred: "editor",
  };
  const calls: string[] = [];
  const router = new BackendRoutingExecutor(
    catalog,
    {
      execute: async (capability) => {
        calls.push(`editor:${capability}`);
        return { backend: "editor" };
      },
    },
    {
      execute: async (capability) => {
        calls.push(`local:${capability}`);
        return { backend: "localTrace" };
      },
    },
  );

  await router.execute(start.id, {
    backend: "auto",
    target: { kind: "editor" },
  });
  await router.execute(start.id, {
    backend: "auto",
    target: { kind: "pie", sessionId: "pie-1", generation: 1 },
  });
  await router.execute(start.id, {
    backend: "auto",
    target: { kind: "development", launchProfileId: "dev" },
  });
  await router.execute("production.trace.status", {
    traceId: "trace-local-123",
  });
  await router.execute("production.job.result.get", {
    jobId: "trace-analysis-local-456",
  });
  await router.execute("production.job.status", {
    jobId: "automation-editor-owned",
  });

  assert.deepEqual(calls, [
    "editor:production.trace.start",
    "editor:production.trace.start",
    "local:production.trace.start",
    "local:production.trace.status",
    "local:production.job.result.get",
    "editor:production.job.status",
  ]);
  await assert.rejects(
    router.execute(start.id, {
      backend: "editor",
      target: { kind: "development", launchProfileId: "dev" },
    }),
    (error: unknown) =>
      error instanceof UEApiError &&
      error.code === "execution_backend_conflict",
  );
});

test("ue_production executes a local trace query while Editor is offline", async () => {
  const catalog = loadCapabilityCatalog();
  const operation = "production.trace.provider.list";
  const descriptor = catalog.get(operation);
  assert.ok(descriptor);
  descriptor.execution = {
    backends: ["editor", "localTrace"],
    preferred: "localTrace",
  };
  let editorCalls = 0;
  let localCalls = 0;
  const runtime = createMcpServer({
    catalog,
    client: {
      getHealth: async () => {
        throw new UEApiError({
          code: "editor_unreachable",
          message: "offline",
        });
      },
      getCapabilities: async () => {
        throw new Error("not expected");
      },
      execute: async () => {
        editorCalls += 1;
        throw new UEApiError({
          code: "editor_unreachable",
          message: "offline",
        });
      },
      workflow: async () => {
        throw new Error("not expected");
      },
    },
    localTraceExecutor: {
      execute: async (capability, params) => {
        localCalls += 1;
        return { capability, traceId: params?.traceId, offline: true };
      },
    },
  });
  const registered = (
    runtime.server as unknown as {
      _registeredTools: Record<
        string,
        {
          handler: (
            input: Record<string, unknown>,
            context: Record<string, never>,
          ) => Promise<{
            content: Array<{ type: string; text?: string }>;
            isError?: boolean;
          }>;
        }
      >;
    }
  )._registeredTools;
  const response = await registered.ue_production.handler(
    {
      operation,
      params: { traceId: "trace-local-offline" },
    },
    {},
  );
  assert.equal(response.isError, false);
  assert.equal(editorCalls, 0);
  assert.equal(localCalls, 1);
  const payload = JSON.parse(response.content[0]?.text ?? "{}");
  assert.equal(payload.offline, true);
});
