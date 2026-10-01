#!/usr/bin/env node

// Read-only acceptance against an already running project Editor. This entry
// never starts UE, compiles, saves, edits assets, or controls the current PIE.
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHash, randomUUID } from "node:crypto";
import { createReadStream } from "node:fs";
import { mkdir, realpath, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";

const GRAPH_SCHEMA = "ue.material.graph-query/1";
const MAX_PAGES = 1024;

function sha256(value) {
  return createHash("sha256").update(value).digest("hex");
}

function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value !== null && typeof value === "object") {
    return Object.fromEntries(Object.keys(value).sort().map((key) => [key, canonical(value[key])]));
  }
  return value;
}

function samePath(actual, expected) {
  // Unreal may return slash-separated Windows paths. Do not resolve an empty
  // reported path into the working directory and accidentally accept it.
  return typeof actual === "string" && actual.length > 0
    && resolve(actual).replaceAll("\\", "/").toLowerCase()
      === resolve(expected).replaceAll("\\", "/").toLowerCase();
}

function objectPath(path) {
  const separator = path.lastIndexOf("/");
  return path.slice(separator).includes(".") ? path : `${path}.${path.slice(separator + 1)}`;
}

function hashField(value, label) {
  assert.match(value ?? "", /^[0-9a-f]{64}$/i, `${label} must be a SHA-256 digest`);
  return value.toLowerCase();
}

export async function fileIdentity(path) {
  const actualPath = await realpath(resolve(path));
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(actualPath)) hash.update(chunk);
  return { path: actualPath, sha256: hash.digest("hex") };
}

export function verifyModule(data, expected) {
  assert.equal(data.plugin, "UE_AI_integration", "wrong plugin identity");
  assert.equal(data.module, "UE_AI_integration", "wrong native module identity");
  assert.equal(data.loaded, true, "UEAI module is not loaded");
  assert.equal(data.processId, expected.processId, "Editor PID differs from the selected process");
  assert.ok(samePath(data.modulePath, expected.dll.path), "loaded module path mismatch");
  assert.ok(samePath(data.dll?.path, expected.dll.path), "DLL provenance path mismatch");
  assert.equal(data.dll?.exists, true, "reported DLL does not exist");
  assert.equal(hashField(data.dll?.sha256, "DLL"), expected.dll.sha256, "DLL hash mismatch");
  assert.ok(samePath(data.pdb?.path, expected.pdb.path), "PDB provenance path mismatch");
  assert.equal(data.pdb?.exists, true, "reported PDB does not exist");
  assert.equal(hashField(data.pdb?.sha256, "PDB"), expected.pdb.sha256, "PDB hash mismatch");
  assert.equal(data.latestBuildArtifactExists, true, "latest build artifact is absent");
  assert.equal(data.matchesLatestBuildArtifact, true, "loaded module is not the latest artifact");
  assert.ok(samePath(data.latestBuildArtifactPath, expected.dll.path), "latest artifact path mismatch");
  assert.equal(hashField(data.latestBuildArtifactSha256, "latest artifact"), expected.dll.sha256);
  assert.ok(typeof data.editorStartedAtUtc === "string" && data.editorStartedAtUtc.length > 0,
    "Editor start identity is absent");
  assert.ok(data.liveCoding && typeof data.liveCoding.available === "boolean", "Live Coding state is absent");
  assert.notEqual(data.liveCoding.compiling, true, "Live Coding is compiling");
  assert.notEqual(data.liveCoding.enabledForSession, true, "Live Coding session cannot prove the unpatched DLL");
  assert.equal(data.liveCoding.lastPatchResult, "none", "a Live Coding patch invalidates exact DLL identity");
  const identity = data.loadedModuleIdentity;
  assert.equal(identity?.schema, "ue.loaded-module-identity.v1", "loaded module identity schema is absent");
  assert.equal(identity?.plugin, "UE_AI_integration", "loaded module identity plugin mismatch");
  assert.equal(identity?.module, "UE_AI_integration", "loaded module identity module mismatch");
  assert.equal(identity?.processId, expected.processId, "loaded module identity PID differs");
  assert.ok(samePath(identity?.modulePath, expected.dll.path), "loaded module identity path mismatch");
  assert.equal(hashField(identity?.moduleSha256, "loaded module identity DLL"), expected.dll.sha256);
  assert.ok(samePath(identity?.pdbPath, expected.pdb.path), "loaded module identity PDB path mismatch");
  assert.equal(hashField(identity?.pdbSha256, "loaded module identity PDB"), expected.pdb.sha256);
  assert.ok(samePath(identity?.latestBuildArtifactPath, expected.dll.path), "loaded module identity latest artifact path mismatch");
  assert.equal(hashField(identity?.latestBuildArtifactSha256, "loaded module identity latest artifact"), expected.dll.sha256);
  assert.equal(identity?.editorStartedAtUtc, data.editorStartedAtUtc, "loaded module identity start time mismatch");
  return {
    processId: data.processId,
    editorStartedAtUtc: data.editorStartedAtUtc,
    modulePath: data.modulePath,
    dll: expected.dll,
    pdb: expected.pdb,
    latestBuildArtifactPath: data.latestBuildArtifactPath,
    latestBuildArtifactSha256: data.latestBuildArtifactSha256,
    liveCoding: data.liveCoding,
    loadedModuleIdentity: identity,
  };
}

export function createCliTransport(cliPath, endpoint, spawnProcess = spawn) {
  async function invoke(args, input = "") {
    const child = spawnProcess(cliPath, [...args, "--endpoint", endpoint, "--timeout-ms", "30000", "--json"], {
      windowsHide: true, shell: false, stdio: ["pipe", "pipe", "pipe"],
    });
    const chunks = [];
    let size = 0;
    // Do not echo stderr, which may contain project-local configuration.
    child.stderr.resume();
    child.stdout.on("data", (chunk) => {
      size += chunk.length;
      if (size > 4 * 1024 * 1024) child.kill();
      else chunks.push(chunk);
    });
    child.stdin.on("error", () => {});
    child.stdin.end(input);
    const timer = setTimeout(() => child.kill(), 45_000);
    let exitCode;
    try {
      exitCode = await new Promise((resolveExit, reject) => {
        child.once("error", reject);
        child.once("close", resolveExit);
      });
    } finally { clearTimeout(timer); }
    assert.ok(size <= 4 * 1024 * 1024, "CLI response exceeded the evidence budget");
    let envelope;
    try { envelope = JSON.parse(Buffer.concat(chunks).toString("utf8")); }
    catch { throw new Error(`CLI returned invalid JSON for ${args[0]}`); }
    assert.equal(exitCode, 0, `CLI failed for ${args[0]}: ${envelope.error?.code ?? "execution_failed"}`);
    assert.equal(envelope.ok, true, `CLI failed for ${args[0]}: ${envelope.error?.code ?? "invalid_envelope"}`);
    return envelope;
  }
  return {
    describe: (id) => invoke(["help", id, "--live-schema"]),
    execute: async (id, params) => (await invoke([
      id, "--live-schema", "--request-id", `project-acceptance-${randomUUID()}`, "--params-file", "-",
    ], JSON.stringify(params))).data,
  };
}

export async function runAcceptance(options, transport, expected) {
  const evidence = {
    schema: "ue.project-asset-acceptance.v1",
    lane: "existing-project-readonly",
    endpoint: options.endpoint,
    schemas: [],
    verification: {
      staticVerified: null, compiled: null, moduleLoaded: null, assetReadback: null,
      runtimeVerified: null, visualVerified: null, restorationVerified: null,
      unknownReasons: [
        "Read-only graph inspection does not prove mutation refusal or restoration after an edit.",
        "This entry does not render or validate visual output.",
        "Native module provenance does not prove that the current source compiled.",
      ],
    },
  };
  const snapshots = [];
  async function execute(id, params) {
    // Refresh the exact live contract before every dispatch, including cleanup.
    const help = await transport.describe(id);
    assert.equal(help.meta?.schemaSource, "editor", `${id} must use live schema`);
    const descriptor = help.data;
    assert.equal(descriptor?.id, id, "live descriptor identifies a different operation");
    assert.equal(descriptor.available, true, `${id} is unavailable in this Editor`);
    assert.equal(descriptor.lifecycle?.status, "active", `${id} is not active`);
    assert.equal(descriptor.lifecycle?.canonicalId, id, `${id} is not canonical`);
    assert.equal(descriptor.inputSchema?.type, "object", `${id} has no exact input schema`);
    assert.equal(descriptor.inputSchema.additionalProperties, false);
    assert.ok(descriptor.effects, `${id} has no declared effects`);
    for (const field of ["asset", "world", "editorSession", "external"]) {
      assert.ok(["none", "read"].includes(descriptor.effects[field]), `${id} is not read-only`);
    }
    assert.notEqual(descriptor.traits?.destructive, true, `${id} is destructive`);
    const digest = sha256(JSON.stringify(canonical(descriptor.inputSchema)));
    evidence.schemas.push({ capability: id, inputSchemaSha256: digest });
    return transport.execute(id, params);
  }
  async function pageAll(id, params, field, verifyPage) {
    const rows = [];
    const cursors = new Set();
    let cursor;
    for (let page = 0; page < MAX_PAGES; page++) {
      const data = await execute(id, { ...params, limit: 200, ...(cursor ? { cursor } : {}) });
      verifyPage(data);
      assert.ok(Array.isArray(data[field]), `${id} returned no ${field} array`);
      assert.equal(typeof data.hasMore, "boolean", `${id} has no pagination verdict`);
      rows.push(...data[field]);
      if (!data.hasMore) return rows;
      assert.ok(typeof data.nextCursor === "string" && data.nextCursor.length > 0, "missing continuation");
      assert.ok(!cursors.has(data.nextCursor), "pagination repeated a cursor");
      cursors.add(data.nextCursor);
      cursor = data.nextCursor;
    }
    throw new Error(`${id} exceeded the bounded page count`);
  }
  function graphIdentity(data, snapshot) {
    assert.equal(data.schema, GRAPH_SCHEMA, "unexpected GraphIR response schema");
    assert.equal(data.assetPath, objectPath(options.material), "Material asset identity mismatch");
    assert.equal(data.targetContext, "asset", "preview state is not project-asset readback");
    assert.equal(data.includeNamedReroutes, true, "named reroute coverage was not requested");
    assert.equal(data.unresolvedNamedReroutes, 0, "Material contains unresolved named reroutes");
    assert.ok(typeof data.snapshotId === "string" && data.snapshotId.length > 0, "missing snapshot identity");
    hashField(data.projectionHash, "GraphIR projection");
    if (snapshot) {
      assert.equal(data.snapshotId, snapshot.snapshotId, "page switched snapshots");
      assert.equal(data.projectionHash, snapshot.projectionHash, "page switched projections");
    }
  }
  let failure;
  try {
    evidence.moduleBefore = verifyModule(await execute("production.module.loaded.get", {}), expected);
    if (options.material) {
      const params = { assetPath: options.material, targetContext: "asset", includeNamedReroutes: true };
      const before = await execute("content.material.graph.index", params);
      if (before?.snapshotId) snapshots.push(before.snapshotId);
      graphIdentity(before);
      const nodes = await pageAll("content.material.graph.nodes.list", { snapshotId: before.snapshotId }, "nodes",
        (page) => graphIdentity(page, before));
      assert.equal(nodes.length, before.totalNodes, "Material node inventory is incomplete");
      const ids = nodes.map((node) => node.nodeId);
      assert.equal(new Set(ids).size, ids.length, "Material node inventory repeats an identity");
      const seedIds = options.materialNodes?.length ? options.materialNodes : [ids.find((id) => id !== "root")];
      assert.ok(seedIds.every((id) => typeof id === "string" && ids.includes(id) && id !== "root"),
        "Material boundary needs existing expression node identities");
      const boundary = await execute("content.material.graph.boundary.get", {
        snapshotId: before.snapshotId, nodeIds: seedIds,
        direction: "upstream", depth: 32, maxNodes: 200, maxEdges: 1000,
      });
      graphIdentity(boundary, before);
      assert.equal(boundary.sourceSnapshotId, before.snapshotId);
      assert.equal(boundary.sourceProjectionHash, before.projectionHash);
      assert.ok(Array.isArray(boundary.writableNodeIds));
      const selectedIds = boundary.writableNodeIds;
      assert.equal(new Set(selectedIds).size, selectedIds.length, "boundary repeats a selected identity");
      assert.ok(seedIds.every((id) => selectedIds.includes(id)), "boundary omitted a requested seed");
      assert.ok(selectedIds.every((id) => ids.includes(id)), "boundary selected a foreign node");
      assert.ok(Array.isArray(boundary.externallyConsumedNodeIds));
      assert.ok(Array.isArray(boundary.boundaryEdges));
      assert.equal(boundary.boundaryEdgeCount, boundary.boundaryEdges.length);
      const consumed = new Set();
      for (const edge of boundary.boundaryEdges) {
        assert.ok(ids.includes(edge.sourceNodeId) && ids.includes(edge.targetNodeId), "boundary edge has a foreign node");
        const sourceInside = selectedIds.includes(edge.sourceNodeId);
        const targetInside = selectedIds.includes(edge.targetNodeId);
        assert.notEqual(sourceInside, targetInside, "boundary edge does not cross the selection");
        if (sourceInside) consumed.add(edge.sourceNodeId);
      }
      assert.deepEqual([...boundary.externallyConsumedNodeIds].sort(), [...consumed].sort(),
        "shared-node risk does not match the crossing edges");
      assert.equal(boundary.requiresSharedNodeConfirmation, boundary.externallyConsumedNodeIds.length > 0);
      assert.equal(boundary.requiresLiveFingerprintCheck, true);
      hashField(boundary.boundaryDigest, "Material boundary");
      assert.equal(boundary.boundaryId, `boundary:${boundary.boundaryDigest}`);
      const after = await execute("content.material.graph.index", params);
      if (after?.snapshotId) snapshots.push(after.snapshotId);
      graphIdentity(after);
      const changes = await pageAll("content.material.graph.snapshots.diff", {
        beforeSnapshotId: before.snapshotId, afterSnapshotId: after.snapshotId,
      }, "changes", (page) => {
        assert.equal(page.schema, "ue.material.graph-diff/1");
        graphIdentity(page.before, before);
        graphIdentity(page.after, after);
        assert.equal(page.projectionEqual, true, "Material authored state drifted during acceptance");
      });
      assert.equal(changes.length, 0, "Material changed during read-only acceptance");
      evidence.material = {
        asset: before.assetPath, projectionHash: before.projectionHash,
        totalNodes: nodes.length, totalEdges: before.totalEdges,
        boundary, authoredStateStable: true, mutationProtectionVerified: null, restorationVerified: null,
        coverage: "Captured expression properties and explicit/named-reroute edges; referenced functions remain unexpanded.",
      };
    }
    if (options.blueprint) {
      const runtime = await execute("blueprint.asset.runtime.verify", { blueprint: options.blueprint });
      evidence.blueprint = runtime;
      evidence.verification.compiled = typeof runtime.compiled === "boolean" ? runtime.compiled : null;
      evidence.verification.runtimeVerified = false;
      assert.equal(runtime.schema, "ue.blueprint.runtime-acceptance.v1");
      assert.equal(runtime.blueprint, objectPath(options.blueprint), "Blueprint asset identity mismatch");
      assert.equal(runtime.compiled, true, "Blueprint compile state is invalid");
      assert.equal(runtime.runtimeVerified, true, runtime.runtimeVerificationReason ?? "no runtime instance");
      assert.equal(runtime.instanceIdentityComplete, true, "runtime instance identity is incomplete");
      assert.ok(["pie", "game"].includes(runtime.worldType), "observed world is not PIE or Game");
      assert.ok(runtime.runtimeWorldCount > 0 && runtime.instanceCount > 0);
      assert.equal(runtime.instance, options.expectedInstance, "wrong runtime actor instance");
      assert.equal(runtime.instanceClass, options.expectedClass, "wrong runtime actor class");
      assert.equal(runtime.worldIdentity, options.expectedWorld, "wrong runtime world");
      assert.equal(runtime.observedGeneratedClass, runtime.generatedClass, "generated class drifted");
      evidence.verification.runtimeVerified = true;
      evidence.verification.unknownReasons.push(
        "Runtime actor identity does not prove project gameplay or cross-asset behavior.",
      );
    }
    evidence.verification.assetReadback = true;
  } catch (error) { failure = error; }
  // Release every snapshot even if a coverage/schema check failed after capture.
  const cleanupFailures = [];
  for (const snapshotId of new Set(snapshots)) {
    try {
      const release = await execute("content.material.graph.snapshot.release", { snapshotId });
      assert.equal(typeof release.released, "boolean", "snapshot release returned no outcome");
    }
    catch { cleanupFailures.push(snapshotId); }
  }
  evidence.cleanup = { released: cleanupFailures.length === 0, unreleasedSnapshots: cleanupFailures };
  if (cleanupFailures.length && !failure) failure = new Error("Material snapshots could not be released");
  try {
    evidence.moduleAfter = verifyModule(await execute("production.module.loaded.get", {}), expected);
    assert.equal(evidence.moduleAfter.editorStartedAtUtc, evidence.moduleBefore?.editorStartedAtUtc,
      "Editor restarted during acceptance");
    evidence.verification.moduleLoaded = true;
  } catch (error) {
    evidence.verification.moduleLoaded = false;
    failure ??= error;
  }
  if (failure) { failure.evidence = evidence; throw failure; }
  return evidence;
}

export function parseOptions(args) {
  const { values } = parseArgs({ args, options: {
    help: { type: "boolean" }, cli: { type: "string" }, endpoint: { type: "string" },
    "process-id": { type: "string" }, dll: { type: "string" }, pdb: { type: "string" },
    material: { type: "string" }, "material-node": { type: "string", multiple: true },
    blueprint: { type: "string" }, "expected-instance": { type: "string" },
    "expected-class": { type: "string" }, "expected-world": { type: "string" }, output: { type: "string" },
  } });
  if (values.help) return { help: true };
  for (const field of ["cli", "endpoint", "process-id", "dll", "pdb"]) {
    assert.ok(values[field], `--${field} is required`);
  }
  const endpoint = new URL(values.endpoint);
  assert.ok(endpoint.protocol === "http:" && ["127.0.0.1", "localhost", "[::1]"].includes(endpoint.hostname),
    "--endpoint must be a loopback HTTP endpoint");
  assert.ok(!endpoint.username && !endpoint.password && !endpoint.search && !endpoint.hash
    && endpoint.pathname === "/", "--endpoint must not contain credentials or a request path");
  const processId = Number(values["process-id"]);
  assert.ok(Number.isSafeInteger(processId) && processId > 0, "--process-id must identify the selected Editor");
  assert.ok(values.material || values.blueprint, "select --material or --blueprint");
  for (const field of ["material", "blueprint"]) {
    if (values[field]) assert.match(values[field], /^\/[A-Za-z0-9_]+\/.+/, `--${field} must be an exact asset path`);
  }
  if (values.blueprint) {
    for (const field of ["expected-instance", "expected-class", "expected-world"]) {
      assert.ok(values[field], `Blueprint runtime acceptance requires --${field} from the current session`);
    }
  }
  assert.ok(!values["material-node"] || values.material, "--material-node requires --material");
  assert.ok((values["material-node"]?.length ?? 0) <= 32, "at most 32 Material boundary seeds are supported");
  assert.equal(new Set(values["material-node"] ?? []).size, values["material-node"]?.length ?? 0,
    "Material boundary seeds must be unique");
  return {
    cli: resolve(values.cli), endpoint: endpoint.origin, processId, dll: values.dll, pdb: values.pdb,
    material: values.material, materialNodes: values["material-node"], blueprint: values.blueprint,
    expectedInstance: values["expected-instance"], expectedClass: values["expected-class"],
    expectedWorld: values["expected-world"], output: values.output,
  };
}

async function main() {
  let result;
  let options;
  try {
    options = parseOptions(process.argv.slice(2));
    if (options.help) {
      process.stdout.write("Usage: node scripts/project_asset_acceptance.mjs --cli <ue-cli.exe> --endpoint <loopback-url> --process-id <EditorPID> --dll <UEAI.dll> --pdb <UEAI.pdb> [--material </Game/M.M> --material-node <currentNodeId>] [--blueprint </Game/BP.BP> --expected-instance <currentActorPath> --expected-class <currentClassPath> --expected-world <currentWorldPath>] [--output <evidence.json>]\nRead-only: connect to an existing Editor; no UE launch/build/edit/save/PIE control. Material restore and visual acceptance remain unknown.\n");
      return;
    }
    const expected = {
      processId: options.processId, dll: await fileIdentity(options.dll), pdb: await fileIdentity(options.pdb),
    };
    const data = await runAcceptance(options, createCliTransport(options.cli, options.endpoint), expected);
    // Native artifacts must also remain stable locally throughout the run.
    assert.equal((await fileIdentity(options.dll)).sha256, expected.dll.sha256, "DLL changed during acceptance");
    assert.equal((await fileIdentity(options.pdb)).sha256, expected.pdb.sha256, "PDB changed during acceptance");
    result = { ok: true, data };
  } catch (error) {
    result = { ok: false, error: {
      code: "project_asset_acceptance_failed", message: error.message, evidence: error.evidence,
    } };
    process.exitCode = 1;
  }
  const text = `${JSON.stringify(result, null, 2)}\n`;
  if (options?.output) {
    const path = resolve(options.output);
    await mkdir(dirname(path), { recursive: true });
    await writeFile(path, text, "utf8");
  }
  process.stdout.write(text);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) await main();
