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

test("loads all six shipped manifests without regressing the shipped baseline", () => {
  const catalog = loadCapabilityCatalog();
  const summary = catalog.summary();
  const baselines = {
    blueprint: 58,
    scene: 54,
    content: 59,
    animation: 10,
    ai: 9,
    production: 22,
  };
  assert.equal(summary.schemaVersion, 2);
  assert.ok(summary.capabilityCount >= 212);
  for (const domain of CAPABILITY_DOMAINS) {
    assert.ok(summary.domainCounts[domain] >= baselines[domain]);
  }
  assert.equal(summary.domainCounts.blueprint, 82);
  for (const operation of [
    "blueprint.selection.set",
    "blueprint.layout.align",
    "blueprint.layout.straighten",
    "blueprint.layout.distribute",
    "blueprint.comment.create_from_selection",
    "blueprint.comment.bounds.set",
  ]) {
    const capability = catalog.get(operation);
    assert.ok(capability, `missing ${operation}`);
    assert.equal(capability.domain, "blueprint");
    assert.equal(capability.kind, "command");
    assert.deepEqual(capability.inputSchema.additionalProperties, false);
    assert.equal(capability.dsl, undefined);
  }
  for (const [operation, kind, outputKind] of [
    ["blueprint.layout.validate", "validation", "json"],
    ["blueprint.layout.organize", "command", "json"],
    ["blueprint.graph.capture", "query", "image"],
    ["blueprint.graph.capture.get", "query", "image"],
    ["blueprint.graph.visual.compare", "query", "image"],
  ] as const) {
    const capability = catalog.get(operation);
    assert.ok(capability, `missing ${operation}`);
    assert.equal(capability.domain, "blueprint");
    assert.equal(capability.kind, kind);
    assert.equal(capability.output.kind, outputKind);
    assert.deepEqual(capability.inputSchema.additionalProperties, false);
  }
  assert.equal(
    catalog.get("blueprint.layout.organize")?.dsl?.admission,
    "editStep",
  );
  assert.equal(catalog.manifests.size, CAPABILITY_DOMAINS.length);
  assert.equal(
    new Set(catalog.capabilities.map((capability) => capability.id)).size,
    summary.capabilityCount,
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

test("uses lower camelCase for every public capability input field", () => {
  const catalog = loadCapabilityCatalog();
  const visit = (schema: Record<string, unknown>, path: string): void => {
    const properties = schema.properties;
    if (
      properties !== null &&
      typeof properties === "object" &&
      !Array.isArray(properties)
    ) {
      for (const [field, child] of Object.entries(properties)) {
        assert.match(field, /^[a-z][A-Za-z0-9]*$/, `${path}.${field}`);
        if (child !== null && typeof child === "object" && !Array.isArray(child)) {
          visit(child as Record<string, unknown>, `${path}.${field}`);
        }
      }
    }
    const items = schema.items;
    if (items !== null && typeof items === "object" && !Array.isArray(items)) {
      visit(items as Record<string, unknown>, `${path}.items`);
    }
    for (const combinator of ["allOf", "anyOf", "oneOf"]) {
      const entries = schema[combinator];
      if (Array.isArray(entries)) {
        entries.forEach((entry, index) => {
          if (entry !== null && typeof entry === "object" && !Array.isArray(entry)) {
            visit(
              entry as Record<string, unknown>,
              `${path}.${combinator}[${index}]`,
            );
          }
        });
      }
    }
  };

  for (const capability of catalog.capabilities) {
    visit(
      capability.inputSchema as Record<string, unknown>,
      capability.id,
    );
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
              requires: {
                plugins: ["EditorScriptingUtilities"],
                modules: ["AssetRegistry"],
                platforms: ["Windows"],
                engine: {
                  min: "5.3.0",
                  maxExclusive: "5.4.0",
                },
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
  assert.deepEqual(catalog.get("blueprint.asset.create")?.requires, {
    plugins: ["EditorScriptingUtilities"],
    modules: ["AssetRegistry"],
    platforms: ["Windows"],
    engine: {
      min: "5.3.0",
      maxExclusive: "5.4.0",
    },
  });
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

test("rejects malformed optional capability search metadata", () => {
  const directory = createTemporaryDirectory();
  writeFileSync(
    join(directory, "blueprint.json"),
    JSON.stringify({
      schemaVersion: 2,
      domain: "blueprint",
      capabilities: [
        {
          id: "blueprint.test.search",
          domain: "blueprint",
          kind: "query",
          description: "Search metadata fixture.",
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
          output: { kind: "json" },
          search: {
            keywords: ["layout", "LAYOUT"],
          },
        },
      ],
    }),
    "utf8",
  );

  assert.throws(
    () => loadCapabilityCatalog(directory),
    (error: unknown) =>
      error instanceof CapabilityManifestError &&
      error.message.includes("search.keywords") &&
      error.message.includes("duplicates"),
  );
});
