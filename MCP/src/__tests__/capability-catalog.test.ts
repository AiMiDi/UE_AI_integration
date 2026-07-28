import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, test } from "node:test";

import {
  CAPABILITY_DOMAINS,
  CapabilityManifestError,
  loadCapabilityCatalog,
} from "../capability-catalog.js";

const temporaryDirectories: string[] = [];

function createTemporaryDirectory(): string {
  const directory = mkdtempSync(join(tmpdir(), "ue5-mcp-catalog-"));
  temporaryDirectories.push(directory);
  return directory;
}

afterEach(() => {
  for (const directory of temporaryDirectories.splice(0)) {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("loads all six shipped manifests offline with stable catalog counts", () => {
  const catalog = loadCapabilityCatalog();
  assert.deepEqual(catalog.summary(), {
    schemaVersion: 2,
    capabilityCount: 212,
    domainCounts: {
      blueprint: 58,
      scene: 54,
      content: 59,
      animation: 10,
      ai: 9,
      production: 22,
    },
  });
  assert.equal(catalog.manifests.size, CAPABILITY_DOMAINS.length);
  assert.equal(
    new Set(catalog.capabilities.map((capability) => capability.id)).size,
    212,
  );
  for (const operation of [
    "scene.pie.restart",
    "scene.pie.start",
    "scene.pie.stop",
    "scene.pie.status",
    "scene.pie.pause",
    "scene.pie.resume",
  ]) {
    const capability = catalog.capabilities.find(({ id }) => id === operation);
    assert.equal(capability?.domain, "scene");
    assert.equal(
      capability?.kind,
      operation === "scene.pie.status" ? "query" : "command",
    );
    assert.equal(capability?.output.kind, "json");
  }
});

test("ships the exact 0.3.0 capability additions with strict root schemas", () => {
  const catalog = loadCapabilityCatalog();
  const additions = [
    "blueprint.asset.compile",
    "blueprint.asset.save",
    "blueprint.asset.reload",
    "blueprint.asset.dirty.get",
    "scene.pie.status",
    "scene.pie.pause",
    "scene.pie.resume",
    "scene.world.contexts.list",
    "scene.runtime.object.find",
    "scene.runtime.object.get",
    "scene.runtime.object.set",
    "scene.runtime.object.call",
    "scene.runtime.widget.tree.get",
    "scene.runtime.widget.state.get",
    "scene.runtime.widget.hit_test",
    "scene.runtime.widget.focus.set",
    "scene.runtime.delegate.list",
    "scene.runtime.delegate.bind",
    "scene.runtime.delegate.unbind",
    "scene.runtime.delegate.is_bound",
    "scene.runtime.delegate.broadcast",
    "scene.runtime.input.pointer",
    "scene.runtime.input.key",
    "scene.runtime.input.mode.set",
    "content.widget.binding.list",
    "content.widget.event.unbind",
    "content.widget.child.rename",
    "content.widget.child.copy",
    "content.widget.child.reparent",
    "content.widget.named_slot.set",
    "content.widget.root.set",
    "content.widget.slot.properties.set",
    "content.widget.animation.get",
    "content.widget.animation.create",
    "content.widget.animation.track.set",
    "content.widget.animation.delete",
    "content.widget.designer.settings.get",
    "content.widget.designer.settings.set",
    "production.scenario.validate",
    "production.scenario.start",
    "production.scenario.status",
    "production.scenario.cancel",
    "production.scenario.result.get",
    "production.scenario.artifact.get",
    "production.module.loaded.get",
    "production.build.target",
    "production.build.job.get",
  ];

  assert.equal(additions.length, 47);
  for (const id of additions) {
    const capability = catalog.get(id);
    assert.ok(capability, `missing ${id}`);
    assert.equal(capability.inputSchema.type, "object", id);
    assert.deepEqual(
      capability.inputSchema.additionalProperties,
      false,
      id,
    );
    assert.equal(typeof capability.traits.readOnly, "boolean", id);
    assert.equal(typeof capability.traits.destructive, "boolean", id);
    assert.equal(typeof capability.traits.expensive, "boolean", id);
  }
  assert.equal(
    catalog.get("production.scenario.artifact.get")?.output.kind,
    "image",
  );
});

test("preserves optional workflow DSL admission metadata", () => {
  const directory = createTemporaryDirectory();
  for (const domain of CAPABILITY_DOMAINS) {
    const capabilities =
      domain === "blueprint"
        ? [
            {
              id: "blueprint.asset.create",
              domain,
              kind: "command",
              description: "Create a Blueprint asset",
              inputSchema: {
                type: "object",
                properties: {},
                additionalProperties: false,
              },
              traits: {
                readOnly: false,
                destructive: false,
                expensive: false,
              },
              output: {
                kind: "json",
              },
              dsl: {
                admission: "editStep",
                scopeKinds: ["blueprint", "widgetBlueprint"],
                transactionDomain: "asset",
                deferCompile: true,
                risk: "safeWrite",
              },
            },
            {
              id: "blueprint.asset.get",
              domain,
              kind: "query",
              description: "Get a Blueprint asset",
              inputSchema: {
                type: "object",
                properties: {},
                additionalProperties: false,
              },
              traits: {
                readOnly: true,
                destructive: false,
                expensive: false,
              },
              output: {
                kind: "json",
              },
            },
          ]
        : [];
    writeFileSync(
      join(directory, `${domain}.json`),
      JSON.stringify({
        schemaVersion: 2,
        domain,
        capabilities,
      }),
      "utf8",
    );
  }

  const catalog = loadCapabilityCatalog(directory);
  assert.deepEqual(catalog.get("blueprint.asset.create")?.dsl, {
    admission: "editStep",
    scopeKinds: ["blueprint", "widgetBlueprint"],
    transactionDomain: "asset",
    deferCompile: true,
    risk: "safeWrite",
  });
  assert.equal(catalog.get("blueprint.asset.get")?.dsl, undefined);
});

test("reports a clear error when a required manifest is missing", () => {
  const directory = createTemporaryDirectory();
  assert.throws(
    () => loadCapabilityCatalog(directory),
    (error: unknown) =>
      error instanceof CapabilityManifestError &&
      error.message.includes('Missing capability manifest "') &&
      error.message.includes("blueprint.json"),
  );
});

test("reports a clear error when a manifest is malformed", () => {
  const directory = createTemporaryDirectory();
  writeFileSync(
    join(directory, "blueprint.json"),
    JSON.stringify({
      schemaVersion: 2,
      domain: "blueprint",
      capabilities: [{}],
    }),
    "utf8",
  );

  assert.throws(
    () => loadCapabilityCatalog(directory),
    (error: unknown) =>
      error instanceof CapabilityManifestError &&
      error.message.includes("capabilities[0].id"),
  );
});
