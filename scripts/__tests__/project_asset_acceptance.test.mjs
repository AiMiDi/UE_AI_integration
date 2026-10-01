import assert from "node:assert/strict";
import { EventEmitter } from "node:events";
import { PassThrough } from "node:stream";
import { test } from "node:test";

import { createCliTransport, parseOptions, runAcceptance, verifyModule } from "../project_asset_acceptance.mjs";

const expected = {
  processId: 12345,
  dll: { path: "S:/tmp/test/UnrealEditor-UE_AI_integration.dll", sha256: "a".repeat(64) },
  pdb: { path: "S:/tmp/test/UnrealEditor-UE_AI_integration.pdb", sha256: "b".repeat(64) },
};
const options = { endpoint: "http://127.0.0.1:39001", material: "/Game/Production/M_Shared" };

function moduleData() {
  return {
    plugin: "UE_AI_integration", module: "UE_AI_integration", loaded: true,
    processId: expected.processId, modulePath: expected.dll.path,
    dll: { ...expected.dll, exists: true }, pdb: { ...expected.pdb, exists: true },
    latestBuildArtifactExists: true, matchesLatestBuildArtifact: true,
    latestBuildArtifactPath: expected.dll.path, latestBuildArtifactSha256: expected.dll.sha256,
    editorStartedAtUtc: "2026-10-02T01:00:00Z",
    loadedModuleIdentity: {
      schema: "ue.loaded-module-identity.v1", plugin: "UE_AI_integration", module: "UE_AI_integration",
      processId: expected.processId, modulePath: expected.dll.path, moduleSha256: expected.dll.sha256,
      pdbPath: expected.pdb.path, pdbSha256: expected.pdb.sha256,
      latestBuildArtifactPath: expected.dll.path, latestBuildArtifactSha256: expected.dll.sha256,
      editorStartedAtUtc: "2026-10-02T01:00:00Z",
    },
    liveCoding: { available: true, enabledForSession: false, compiling: false, lastPatchResult: "none" },
  };
}

function graph(snapshotId) {
  return {
    schema: "ue.material.graph-query/1", snapshotId,
    assetPath: "/Game/Production/M_Shared.M_Shared", targetContext: "asset",
    includeNamedReroutes: true, unresolvedNamedReroutes: 0, projectionHash: "c".repeat(64),
    totalNodes: 2, totalEdges: 1,
  };
}

function fixture(overrides = {}) {
  const calls = [];
  let snapshotCount = 0;
  const transport = {
    async describe(id) {
      const result = {
        ok: true, meta: { schemaSource: "editor" }, data: {
          id, available: true, lifecycle: { status: "active", canonicalId: id },
          inputSchema: { type: "object", properties: {}, additionalProperties: false },
          effects: { asset: "read", world: "none", editorSession: "none", external: "none" },
          traits: { destructive: false },
        },
      };
      return overrides.describe ? overrides.describe(id, result) : result;
    },
    async execute(id, params) {
      calls.push({ id, params });
      if (overrides.execute) {
        const replacement = overrides.execute(id, params, calls);
        if (replacement !== undefined) return replacement;
      }
      if (id === "production.module.loaded.get") return moduleData();
      if (id === "content.material.graph.index") return graph(`snapshot-${++snapshotCount}`);
      if (id === "content.material.graph.nodes.list") {
        // An empty page still has a continuation; acceptance must exhaust it.
        return params.cursor ? {
          ...graph(params.snapshotId), nodes: [{ nodeId: "root" }, { nodeId: "shared" }], hasMore: false,
        } : { ...graph(params.snapshotId), nodes: [], hasMore: true, nextCursor: "nodes-1" };
      }
      if (id === "content.material.graph.boundary.get") {
        return {
          ...graph(params.snapshotId), sourceSnapshotId: params.snapshotId,
          sourceProjectionHash: "c".repeat(64), writableNodeIds: ["shared"],
          boundaryId: `boundary:${"d".repeat(64)}`,
          boundaryDigest: "d".repeat(64), boundaryEdges: [{ sourceNodeId: "shared", targetNodeId: "root" }],
          boundaryEdgeCount: 1, externallyConsumedNodeIds: ["shared"],
          requiresSharedNodeConfirmation: true, requiresLiveFingerprintCheck: true,
        };
      }
      if (id === "content.material.graph.snapshots.diff") {
        return {
          schema: "ue.material.graph-diff/1", before: graph(params.beforeSnapshotId),
          after: graph(params.afterSnapshotId), projectionEqual: true, changes: [],
          hasMore: !params.cursor, ...(params.cursor ? {} : { nextCursor: "diff-1" }),
        };
      }
      if (id === "content.material.graph.snapshot.release") return { released: true };
      if (id === "blueprint.asset.runtime.verify") {
        return {
          schema: "ue.blueprint.runtime-acceptance.v1", blueprint: "/Game/Production/BP_Actor.BP_Actor",
          generatedClass: "/Game/Production/BP_Actor.BP_Actor_C",
          observedGeneratedClass: "/Game/Production/BP_Actor.BP_Actor_C", compiled: true,
          runtimeVerified: true, instanceIdentityComplete: true, worldType: "pie",
          runtimeWorldCount: 1, instanceCount: 1,
          instance: "current-actor", instanceClass: "current-class", worldIdentity: "current-world",
        };
      }
      throw new Error(`unexpected acceptance capability ${id}`);
    },
  };
  return { transport, calls };
}

test("module proof rejects every mismatched identity and patched sessions", () => {
  assert.equal(verifyModule(moduleData(), expected).processId, expected.processId);
  for (const mutate of [
    (data) => { data.plugin = "OtherPlugin"; },
    (data) => { data.module = "OtherModule"; },
    (data) => { data.processId++; },
    (data) => { data.dll.sha256 = "e".repeat(64); },
    (data) => { data.modulePath = "S:/tmp/other/module.dll"; },
    (data) => { data.pdb.sha256 = "e".repeat(64); },
    (data) => { data.latestBuildArtifactPath = "S:/tmp/other/module.dll"; },
    (data) => { data.latestBuildArtifactSha256 = "e".repeat(64); },
    (data) => { data.matchesLatestBuildArtifact = false; },
    (data) => { data.liveCoding.enabledForSession = true; },
    (data) => { data.liveCoding.lastPatchResult = "success"; },
    (data) => { data.loadedModuleIdentity.moduleSha256 = "e".repeat(64); },
    (data) => { data.loadedModuleIdentity.processId++; },
    (data) => { data.loadedModuleIdentity.modulePath = "S:/tmp/other/module.dll"; },
  ]) {
    const data = moduleData();
    mutate(data);
    assert.throws(() => verifyModule(data, expected));
  }
});

test("Material acceptance exhausts empty pages, checks shared risk, and releases snapshots", async () => {
  const { transport, calls } = fixture();
  const result = await runAcceptance(options, transport, expected);
  assert.equal(result.verification.moduleLoaded, true);
  assert.equal(result.verification.assetReadback, true);
  assert.equal(result.verification.runtimeVerified, null);
  assert.equal(result.verification.restorationVerified, null);
  assert.equal(result.material.mutationProtectionVerified, null);
  assert.equal(result.material.boundary.requiresSharedNodeConfirmation, true);
  assert.equal(result.cleanup.released, true);
  assert.equal(calls.filter((call) => call.id.endsWith("nodes.list")).length, 2);
  assert.equal(calls.filter((call) => call.id.endsWith("snapshots.diff")).length, 2);
  assert.equal(calls.filter((call) => call.id.endsWith("snapshot.release")).length, 2);
  assert.ok(calls.every((call) => !/apply|restore|save|compile|pie\.(start|stop)/.test(call.id)));
  const boundaryCall = calls.find((call) => call.id.endsWith("boundary.get"));
  assert.equal(boundaryCall.params.direction, "upstream");
  assert.equal(boundaryCall.params.depth, 32);
});

test("a write effect or local schema refuses dispatch and still releases owned snapshots", async () => {
  for (const mismatch of ["effect", "source"]) {
    const { transport, calls } = fixture({
      describe(id, response) {
        if (id.endsWith("nodes.list")) {
          if (mismatch === "effect") response.data.effects.asset = "write";
          else response.meta.schemaSource = "local";
        }
        return response;
      },
    });
    await assert.rejects(runAcceptance(options, transport, expected));
    assert.equal(calls.some((call) => call.id.endsWith("nodes.list")), false);
    assert.equal(calls.filter((call) => call.id.endsWith("snapshot.release")).length, 1);
  }
});

test("drift and repeating cursors fail without claiming asset acceptance", async () => {
  for (const failure of ["drift", "cursor", "sharedFlag"]) {
    const { transport, calls } = fixture({
      execute(id, params) {
        if (failure === "drift" && id.endsWith("snapshots.diff")) {
          return {
            schema: "ue.material.graph-diff/1", before: graph(params.beforeSnapshotId),
            after: graph(params.afterSnapshotId), projectionEqual: false, changes: [], hasMore: false,
          };
        }
        if (failure === "cursor" && id.endsWith("nodes.list")) {
          return { ...graph(params.snapshotId), nodes: [], hasMore: true, nextCursor: "unchanged" };
        }
        if (failure === "sharedFlag" && id.endsWith("boundary.get")) {
          return {
            ...graph(params.snapshotId), sourceSnapshotId: params.snapshotId,
            sourceProjectionHash: "c".repeat(64), writableNodeIds: ["shared"],
            boundaryEdges: [], boundaryEdgeCount: 0, externallyConsumedNodeIds: ["shared"],
            requiresSharedNodeConfirmation: false, requiresLiveFingerprintCheck: true,
          };
        }
      },
    });
    await assert.rejects(runAcceptance(options, transport, expected), (error) => {
      assert.equal(error.evidence.verification.assetReadback, null);
      assert.equal(error.evidence.cleanup.released, true);
      return true;
    });
    assert.ok(calls.filter((call) => call.id.endsWith("nodes.list")).length <= 2);
  }
});

test("Blueprint acceptance requires the exact current actor, class, and world", async () => {
  const blueprintOptions = {
    endpoint: options.endpoint, blueprint: "/Game/Production/BP_Actor",
    expectedInstance: "current-actor", expectedClass: "current-class", expectedWorld: "current-world",
  };
  const result = await runAcceptance(blueprintOptions, fixture().transport, expected);
  assert.equal(result.verification.runtimeVerified, true);
  for (const field of ["expectedInstance", "expectedClass", "expectedWorld"]) {
    await assert.rejects(runAcceptance({ ...blueprintOptions, [field]: "stale" }, fixture().transport, expected));
  }
  const noRuntime = fixture({ execute(id) {
    if (id === "blueprint.asset.runtime.verify") return {
      schema: "ue.blueprint.runtime-acceptance.v1", blueprint: "/Game/Production/BP_Actor.BP_Actor",
      compiled: true, runtimeVerified: false, runtimeVerificationReason: "no_runtime_world",
    };
  } });
  await assert.rejects(runAcceptance(blueprintOptions, noRuntime.transport, expected), (error) => {
    assert.match(error.message, /no_runtime_world/);
    assert.equal(error.evidence.verification.compiled, true);
    assert.equal(error.evidence.verification.runtimeVerified, false);
    assert.equal(error.evidence.blueprint.runtimeVerificationReason, "no_runtime_world");
    return true;
  });
});

test("Editor restart between module proofs invalidates acceptance", async () => {
  const { transport } = fixture({ execute(id, _params, calls) {
    if (id === "production.module.loaded.get" && calls.length > 1) {
      const restarted = moduleData();
      restarted.editorStartedAtUtc = "2026-10-02T02:00:00Z";
      restarted.loadedModuleIdentity.editorStartedAtUtc = restarted.editorStartedAtUtc;
      return restarted;
    }
  } });
  await assert.rejects(runAcceptance(options, transport, expected), (error) => {
    assert.equal(error.evidence.verification.moduleLoaded, false);
    assert.match(error.message, /restarted/);
    return true;
  });
});

test("entrypoint rejects remote endpoints, missing runtime identity, and missing assets", () => {
  const base = ["--cli", "S:/tmp/ue-cli.exe", "--endpoint", options.endpoint,
    "--process-id", "12345", "--dll", expected.dll.path, "--pdb", expected.pdb.path];
  assert.equal(parseOptions([...base, "--material", options.material]).processId, 12345);
  assert.throws(() => parseOptions(base), /select --material or --blueprint/);
  assert.throws(() => parseOptions([...base, "--blueprint", "/Game/BP"]), /expected-instance/);
  const remote = [...base];
  remote[3] = "http://example.com:39001";
  assert.throws(() => parseOptions([...remote, "--material", options.material]), /loopback/);
});

test("native CLI transport preserves stdin JSON and multibyte UTF-8 without a shell", async () => {
  const input = { blueprint: "/Game/中文路径/BP_角色.BP_角色", value: "literal ` and $()" };
  let received;
  const transport = createCliTransport("S:/tmp/UE AI/ue-cli.exe", options.endpoint, (file, args, config) => {
    assert.equal(file, "S:/tmp/UE AI/ue-cli.exe");
    assert.equal(config.shell, false);
    assert.ok(args.includes("--live-schema"));
    assert.deepEqual(args.slice(args.indexOf("--params-file"), args.indexOf("--params-file") + 2), ["--params-file", "-"]);
    const child = new EventEmitter();
    child.stdout = new PassThrough();
    child.stderr = new PassThrough();
    child.stdin = new PassThrough();
    child.stdin.on("data", (chunk) => { received = JSON.parse(chunk.toString("utf8")); });
    child.stdin.on("finish", () => {
      const bytes = Buffer.from(JSON.stringify({ ok: true, data: input }), "utf8");
      for (const byte of bytes) child.stdout.write(Buffer.from([byte]));
      child.stdout.end();
      child.emit("close", 0);
    });
    child.kill = () => child.emit("close", 1);
    return child;
  });
  assert.deepEqual(await transport.execute("blueprint.asset.get", input), input);
  assert.deepEqual(received, input);
});
