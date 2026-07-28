import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const manifestsRoot = path.join(projectRoot, "Resources", "Capabilities");
const domainsRoot = path.join(
  projectRoot,
  "Source",
  "UE_AI_integration",
  "Private",
  "Domains"
);
const legacyHandlersRoot = path.join(
  projectRoot,
  "Source",
  "UE_AI_integration",
  "Private",
  "Handlers"
);

const expectedCounts = Object.freeze({
  blueprint: 58,
  scene: 54,
  content: 59,
  animation: 10,
  ai: 9,
  production: 22
});
const expectedCapabilityTotal = 212;
const addedCapabilityIds = new Set([
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
  "production.build.job.get"
]);
const expectedDomains = new Set(Object.keys(expectedCounts));
const validKinds = new Set(["query", "command", "validation"]);
const validOutputKinds = new Set(["json", "image"]);
const validBuckets = new Set(["Query", "Command", "Validation"]);
const idPattern = /^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*){2,}$/;
const errors = [];

function fail(message) {
  errors.push(message);
}

function isPlainObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function listFilesRecursive(directory, predicate) {
  if (!fs.existsSync(directory)) return [];
  const results = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const absolutePath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      results.push(...listFilesRecursive(absolutePath, predicate));
    } else if (predicate(absolutePath)) {
      results.push(absolutePath);
    }
  }
  return results.sort((a, b) => a.localeCompare(b));
}

const manifestIds = new Set();
const manifestById = new Map();
let manifestTotal = 0;

for (const [domain, expectedCount] of Object.entries(expectedCounts)) {
  const manifestPath = path.join(manifestsRoot, `${domain}.json`);
  if (!fs.existsSync(manifestPath)) {
    fail(`Missing manifest: ${path.relative(projectRoot, manifestPath)}`);
    continue;
  }

  let manifest;
  try {
    manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
  } catch (error) {
    fail(`${domain}.json is not valid JSON: ${error.message}`);
    continue;
  }

  if (manifest.schemaVersion !== 2) {
    fail(`${domain}.json schemaVersion must be 2`);
  }
  if (manifest.domain !== domain) {
    fail(`${domain}.json domain must be "${domain}"`);
  }
  if (!Array.isArray(manifest.capabilities)) {
    fail(`${domain}.json capabilities must be an array`);
    continue;
  }
  if (manifest.capabilities.length !== expectedCount) {
    fail(
      `${domain}.json expected ${expectedCount} capabilities, found ${manifest.capabilities.length}`
    );
  }

  for (const [index, capability] of manifest.capabilities.entries()) {
    const location = `${domain}.json capabilities[${index}]`;
    if (!isPlainObject(capability)) {
      fail(`${location} must be an object`);
      continue;
    }
    if (typeof capability.id !== "string" || !idPattern.test(capability.id)) {
      fail(`${location}.id is not a valid dotted capability ID`);
    } else {
      if (!capability.id.startsWith(`${domain}.`)) {
        fail(`${location}.id must start with "${domain}."`);
      }
      if (manifestIds.has(capability.id)) {
        fail(`Duplicate capability ID: ${capability.id}`);
      }
      manifestIds.add(capability.id);
      manifestById.set(capability.id, capability);
    }
    if (capability.domain !== domain || !expectedDomains.has(capability.domain)) {
      fail(`${location}.domain must be "${domain}"`);
    }
    if (!validKinds.has(capability.kind)) {
      fail(`${location}.kind must be query, command, or validation`);
    }
    if (
      typeof capability.description !== "string" ||
      capability.description.trim().length === 0
    ) {
      fail(`${location}.description must be non-empty`);
    }
    if (!isPlainObject(capability.inputSchema)) {
      fail(`${location}.inputSchema must be an object`);
    } else if (addedCapabilityIds.has(capability.id)) {
      if (capability.inputSchema.type !== "object") {
        fail(`${location}.inputSchema.type must be object`);
      }
      if (!isPlainObject(capability.inputSchema.properties)) {
        fail(`${location}.inputSchema.properties must be an object`);
      }
      if (capability.inputSchema.additionalProperties !== false) {
        fail(`${location}.inputSchema.additionalProperties must be false`);
      }
      if (
        Array.isArray(capability.inputSchema.required) &&
        capability.inputSchema.required.some(
          (field) => !Object.hasOwn(capability.inputSchema.properties, field)
        )
      ) {
        fail(`${location}.inputSchema.required references an unknown property`);
      }
    }
    if (!isPlainObject(capability.traits)) {
      fail(`${location}.traits must be an object`);
    } else {
      for (const trait of ["readOnly", "destructive", "expensive"]) {
        if (typeof capability.traits[trait] !== "boolean") {
          fail(`${location}.traits.${trait} must be boolean`);
        }
      }
    }
    if (
      !isPlainObject(capability.output) ||
      !validOutputKinds.has(capability.output.kind)
    ) {
      fail(`${location}.output.kind must be json or image`);
    }
  }
  manifestTotal += manifest.capabilities.length;
}

const unexpectedManifestFiles = fs.existsSync(manifestsRoot)
  ? fs
      .readdirSync(manifestsRoot)
      .filter(
        (name) =>
          name.endsWith(".json") &&
          !Object.hasOwn(expectedCounts, path.basename(name, ".json"))
      )
  : [];
if (unexpectedManifestFiles.length > 0) {
  fail(`Unexpected capability manifests: ${unexpectedManifestFiles.join(", ")}`);
}

if (
  manifestTotal !== expectedCapabilityTotal ||
  manifestIds.size !== expectedCapabilityTotal
) {
  fail(
    `Manifest catalog must contain ${expectedCapabilityTotal} unique capabilities; total=${manifestTotal}, unique=${manifestIds.size}`
  );
}
for (const id of addedCapabilityIds) {
  if (!manifestIds.has(id)) fail(`Missing 0.3.0 capability ID: ${id}`);
}
if (addedCapabilityIds.size !== 47) {
  fail(`0.3.0 capability set must contain 47 IDs, found ${addedCapabilityIds.size}`);
}

const handlerFiles = listFilesRecursive(domainsRoot, (filePath) =>
  filePath.endsWith(".cpp")
);
if (handlerFiles.length === 0) {
  fail("No domain handler .cpp files found");
}

const legacyHandlerFiles = listFilesRecursive(legacyHandlersRoot, (filePath) =>
  filePath.endsWith(".cpp")
);
if (legacyHandlerFiles.length !== 0) {
  fail(
    `Legacy Private/Handlers still contains .cpp files: ${legacyHandlerFiles
      .map((filePath) => path.basename(filePath))
      .join(", ")}`
  );
}

const handlerIds = new Set();
let handlerIdTotal = 0;
let registrationCount = 0;

for (const handlerPath of handlerFiles) {
  const relativePath = path
    .relative(domainsRoot, handlerPath)
    .split(path.sep);
  if (relativePath.length !== 3) {
    fail(
      `${path.relative(projectRoot, handlerPath)} must live at Domains/<Domain>/<Kind>/<File>.cpp`
    );
    continue;
  }
  const [domainDirectory, bucket] = relativePath;
  const pathDomain = domainDirectory.toLowerCase();
  if (!expectedDomains.has(pathDomain)) {
    fail(`${path.relative(projectRoot, handlerPath)} has unknown domain directory`);
  }
  if (!validBuckets.has(bucket)) {
    fail(`${path.relative(projectRoot, handlerPath)} has unknown kind bucket ${bucket}`);
  }

  const source = fs.readFileSync(handlerPath, "utf8");
  const ids = [
    ...source.matchAll(
      /FString\s+GetCapabilityId\(\)\s+const\s+override\s*\{\s*return\s+TEXT\("([^"]+)"\)\s*;\s*\}/g
    )
  ].map((match) => match[1]);
  for (const id of ids) {
    handlerIdTotal += 1;
    if (handlerIds.has(id)) fail(`Duplicate handler capability ID: ${id}`);
    handlerIds.add(id);
    if (!id.startsWith(`${pathDomain}.`)) {
      fail(
        `${path.relative(projectRoot, handlerPath)} capability ${id} does not match path domain ${pathDomain}`
      );
    }
  }

  const registrations = [
    ...source.matchAll(
      /void\s+Register\w+Tools\s*\(\s*FMCPToolRegistry\s*&\s*Registry(?:\s*,[^)]*)?\)/g
    )
  ];
  if (ids.length > 0 && registrations.length === 0) {
    fail(
      `${path.relative(projectRoot, handlerPath)} declares capabilities but has no Register*Tools registrar whose first parameter is FMCPToolRegistry& Registry`
    );
  }
  registrationCount += registrations.length;
  if (source.includes("FMCPToolRegistry::Get()")) {
    fail(
      `${path.relative(projectRoot, handlerPath)} still calls FMCPToolRegistry::Get()`
    );
  }
}

if (
  handlerIdTotal !== expectedCapabilityTotal ||
  handlerIds.size !== expectedCapabilityTotal
) {
  fail(
    `Handlers must expose ${expectedCapabilityTotal} unique capabilities; total=${handlerIdTotal}, unique=${handlerIds.size}`
  );
}
const missingFromHandlers = [...manifestIds].filter((id) => !handlerIds.has(id));
const missingFromManifests = [...handlerIds].filter((id) => !manifestIds.has(id));
if (missingFromHandlers.length > 0) {
  fail(`Manifest-only IDs: ${missingFromHandlers.join(", ")}`);
}
if (missingFromManifests.length > 0) {
  fail(`Handler-only IDs: ${missingFromManifests.join(", ")}`);
}

if (errors.length > 0) {
  console.error(`Capability validation failed with ${errors.length} error(s):`);
  for (const error of errors) console.error(`- ${error}`);
  process.exit(1);
}

const kindCounts = { query: 0, command: 0, validation: 0 };
for (const capability of manifestById.values()) {
  kindCounts[capability.kind] += 1;
}

console.log(
  JSON.stringify(
    {
      ok: true,
      schemaVersion: 2,
      manifests: Object.keys(expectedCounts).length,
      handlers: handlerFiles.length,
      capabilities: manifestIds.size,
      domainCounts: expectedCounts,
      kindCounts
    },
    null,
    2
  )
);
