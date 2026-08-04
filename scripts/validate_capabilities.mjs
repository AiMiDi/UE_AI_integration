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

const baselineCounts = Object.freeze({
  blueprint: 58,
  scene: 54,
  content: 59,
  animation: 10,
  ai: 9,
  production: 22
});
const baselineCapabilityTotal = 212;
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
const expectedDomains = new Set(Object.keys(baselineCounts));
const validKinds = new Set(["query", "command", "validation"]);
const validOutputKinds = new Set(["json", "image"]);
const validBuckets = new Set(["Query", "Command", "Validation"]);
const idPattern = /^[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)+$/;
const errors = [];

function fail(message) {
  errors.push(message);
}

function isPlainObject(value) {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function validateSearchMetadata(search, location) {
  if (!isPlainObject(search)) {
    fail(`${location} must be an object`);
    return;
  }
  const allowedFields = new Set(["title", "keywords", "aliases"]);
  for (const field of Object.keys(search)) {
    if (!allowedFields.has(field)) {
      fail(`${location}.${field} is not supported`);
    }
  }
  if (
    search.title !== undefined &&
    (typeof search.title !== "string" || search.title.trim().length === 0)
  ) {
    fail(`${location}.title must be a non-empty string`);
  }
  for (const field of ["keywords", "aliases"]) {
    const values = search[field];
    if (values === undefined) continue;
    if (
      !Array.isArray(values) ||
      values.length === 0 ||
      values.some(
        (value) =>
          typeof value !== "string" || value.trim().length === 0
      )
    ) {
      fail(`${location}.${field} must be a non-empty array of non-empty strings`);
      continue;
    }
    const normalized = values.map((value) => value.trim().toLowerCase());
    if (new Set(normalized).size !== normalized.length) {
      fail(`${location}.${field} must not contain duplicates`);
    }
  }
  if (
    search.title === undefined &&
    search.keywords === undefined &&
    search.aliases === undefined
  ) {
    fail(`${location} must declare title, keywords, or aliases`);
  }
}

function validateCamelCaseProperties(schema, location) {
  if (!isPlainObject(schema)) return;
  if (isPlainObject(schema.properties)) {
    for (const [field, child] of Object.entries(schema.properties)) {
      if (!/^[a-z][A-Za-z0-9]*$/.test(field)) {
        fail(`${location}.properties.${field} must use lower camelCase`);
      }
      validateCamelCaseProperties(child, `${location}.properties.${field}`);
    }
  }
  if (schema.items !== undefined) {
    validateCamelCaseProperties(schema.items, `${location}.items`);
  }
  for (const combinator of ["allOf", "anyOf", "oneOf"]) {
    if (Array.isArray(schema[combinator])) {
      schema[combinator].forEach((entry, index) =>
        validateCamelCaseProperties(
          entry,
          `${location}.${combinator}[${index}]`
        )
      );
    }
  }
}

function validateExecutionMetadata(execution, location) {
  if (!isPlainObject(execution)) {
    fail(`${location} must be an object`);
    return;
  }
  const allowedFields = new Set(["backends", "preferred"]);
  for (const field of Object.keys(execution)) {
    if (!allowedFields.has(field)) {
      fail(`${location}.${field} is not supported`);
    }
  }
  const validBackends = new Set(["editor", "localTrace"]);
  if (
    !Array.isArray(execution.backends) ||
    execution.backends.length === 0 ||
    execution.backends.some((backend) => !validBackends.has(backend)) ||
    new Set(execution.backends).size !== execution.backends.length
  ) {
    fail(`${location}.backends must contain unique editor/localTrace values`);
    return;
  }
  if (
    typeof execution.preferred !== "string" ||
    !execution.backends.includes(execution.preferred)
  ) {
    fail(`${location}.preferred must be one of the declared backends`);
  }
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

const actualDomainCounts = {};
for (const [domain, baselineCount] of Object.entries(baselineCounts)) {
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
  if (manifest.capabilities.length < baselineCount) {
    fail(
      `${domain}.json must preserve at least ${baselineCount} shipped capabilities, found ${manifest.capabilities.length}`
    );
  }
  actualDomainCounts[domain] = manifest.capabilities.length;

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
    if (capability.search !== undefined) {
      validateSearchMetadata(capability.search, `${location}.search`);
    }
    if (capability.execution !== undefined) {
      validateExecutionMetadata(capability.execution, `${location}.execution`);
    }
    if (!isPlainObject(capability.inputSchema)) {
      fail(`${location}.inputSchema must be an object`);
    } else {
      validateCamelCaseProperties(
        capability.inputSchema,
        `${location}.inputSchema`
      );
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
    if (capability.requires !== undefined) {
      if (!isPlainObject(capability.requires)) {
        fail(`${location}.requires must be an object`);
      } else {
        const allowedRequirementFields = new Set([
          "features",
          "plugins",
          "modules",
          "platforms",
          "engine"
        ]);
        for (const field of Object.keys(capability.requires)) {
          if (!allowedRequirementFields.has(field)) {
            fail(`${location}.requires.${field} is not supported`);
          }
        }
        for (const field of ["features", "plugins", "modules", "platforms"]) {
          const value = capability.requires[field];
          if (
            value !== undefined &&
            (!Array.isArray(value) ||
              value.some(
                (item) =>
                  typeof item !== "string" || item.trim().length === 0
              ))
          ) {
            fail(
              `${location}.requires.${field} must be an array of non-empty strings`
            );
          }
        }
        if (capability.requires.engine !== undefined) {
          const engine = capability.requires.engine;
          if (!isPlainObject(engine)) {
            fail(`${location}.requires.engine must be an object`);
          } else {
            for (const field of Object.keys(engine)) {
              if (field !== "min" && field !== "maxExclusive") {
                fail(
                  `${location}.requires.engine.${field} is not supported`
                );
              } else if (
                typeof engine[field] !== "string" ||
                engine[field].trim().length === 0
              ) {
                fail(
                  `${location}.requires.engine.${field} must be a non-empty string`
                );
              }
            }
          }
        }
      }
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
          !Object.hasOwn(baselineCounts, path.basename(name, ".json"))
      )
  : [];
if (unexpectedManifestFiles.length > 0) {
  fail(`Unexpected capability manifests: ${unexpectedManifestFiles.join(", ")}`);
}

if (manifestTotal < baselineCapabilityTotal || manifestIds.size !== manifestTotal) {
  fail(
    `Manifest catalog must preserve at least ${baselineCapabilityTotal} unique capabilities; total=${manifestTotal}, unique=${manifestIds.size}`
  );
}
try {
  const descriptor = JSON.parse(
    fs.readFileSync(path.join(projectRoot, "UE_AI_integration.uplugin"), "utf8")
  );
  const mcpPackage = JSON.parse(
    fs.readFileSync(path.join(projectRoot, "MCP", "package.json"), "utf8")
  );
  const claudePlugin = JSON.parse(
    fs.readFileSync(path.join(projectRoot, ".claude-plugin", "plugin.json"), "utf8")
  );
  const setupSkill = fs.readFileSync(
    path.join(projectRoot, "skills", "setup-ue5", "SKILL.md"),
    "utf8"
  );
  if (
    typeof descriptor.VersionName !== "string" ||
    descriptor.VersionName !== mcpPackage.version ||
    descriptor.VersionName !== claudePlugin.version
  ) {
    fail("UE descriptor, MCP package, and Claude plugin versions must match");
  }
  if (!claudePlugin.description.includes(`${manifestTotal} manifest-driven capabilities`)) {
    fail("Claude plugin description must contain the manifest-derived capability count");
  }
  if (
    !setupSkill.includes(`Release ${descriptor.VersionName} currently ships ${manifestTotal} capabilities`)
  ) {
    fail("setup-ue5 release guidance must match the current version and capability count");
  }
} catch (error) {
  fail(`Release metadata could not be validated: ${error.message}`);
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

if (handlerIdTotal !== manifestTotal || handlerIds.size !== manifestIds.size) {
  fail(
    `Handlers must exactly match the manifest catalog; manifest=${manifestTotal}, handlerTotal=${handlerIdTotal}, handlerUnique=${handlerIds.size}`
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

// Keep the semantic API surface aligned with the Unreal Insights panels it
// replaces. This file is shipped with the Worker, so a stale mapping would
// otherwise make a capability appear supported while its panel/operations are
// undiscoverable (or vice versa).
const insightsMappingVersions = ["5.3", "5.4", "5.5", "5.6", "5.7"];
const traceWorkerProtocolPath = path.join(
  projectRoot,
  "Resources",
  "Trace",
  "worker-protocol.v1.json"
);
if (!fs.existsSync(traceWorkerProtocolPath)) {
  fail("Missing Resources/Trace/worker-protocol.v1.json");
} else {
  const protocol = JSON.parse(
    fs.readFileSync(traceWorkerProtocolPath, "utf8")
  );
  if (
    protocol.schema !== "ue.trace-worker-protocol.v1" ||
    protocol.protocolVersion !== 1 ||
    protocol.requestSchema !== "ue.trace-worker-request.v1" ||
    protocol.responseSchema !== "ue.trace-worker-response.v1" ||
    protocol.frame?.lengthPrefix !== "uint32-little-endian" ||
    protocol.frame?.maximumBytes !== 4 * 1024 * 1024 ||
    protocol.frame?.requestsPerConnection !== 1 ||
    protocol.residentService?.tcp !== false ||
    protocol.residentService?.defaultIdleSeconds !== 600 ||
    protocol.residentService?.maximumConcurrentConnections !== 2 ||
    protocol.residentService?.maximumConcurrentAnalyses !== 1 ||
    protocol.residentService?.analysisSessionCacheCapacity !== 2 ||
    protocol.residentService?.analysisSessionPolicy !== "sha256-lru"
  ) {
    fail("Trace Worker protocol manifest does not match the bounded v1 IPC contract");
  }
}
const expectedInsightsCapabilities = new Set([
  "production.trace.timing.query",
  "production.trace.counter.query",
  "production.trace.memory.query",
  "production.trace.loading.query",
  "production.trace.network.query",
  "production.trace.tasks.query",
  "production.trace.context_switches.query",
  "production.trace.io.query",
  "production.trace.log.query",
  "production.trace.bookmark.query",
  "production.trace.region.query",
  "production.trace.screenshot.query"
]);
const boundedTraceQueryProperties = [
  "startTimeSeconds",
  "endTimeSeconds",
  "filter",
  "cursor",
  "limit"
];
const traceIdPattern = "^[A-Za-z0-9][A-Za-z0-9-]{0,127}$";
const traceCursorPattern = "^[0-9]+$";
for (const capabilityId of expectedInsightsCapabilities) {
  const capability = manifestById.get(capabilityId);
  const properties = capability?.inputSchema?.properties;
  if (!isPlainObject(properties)) {
    fail(`${capabilityId} must expose an object input schema`);
    continue;
  }
  for (const property of boundedTraceQueryProperties) {
    if (!Object.hasOwn(properties, property)) {
      fail(`${capabilityId} must expose bounded query property ${property}`);
    }
  }
  const traceId = properties.traceId;
  if (
    !isPlainObject(traceId) ||
    traceId.type !== "string" ||
    traceId.minLength !== 1 ||
    traceId.maxLength !== 128 ||
    traceId.pattern !== traceIdPattern
  ) {
    fail(`${capabilityId}.traceId must use the bounded safe-id contract`);
  }
  const cursor = properties.cursor;
  if (
    !isPlainObject(cursor) ||
    cursor.type !== "string" ||
    cursor.maxLength !== 20 ||
    cursor.pattern !== traceCursorPattern
  ) {
    fail(`${capabilityId}.cursor must be a decimal uint64 string`);
  }
}
for (const capabilityId of [
  "production.trace.provider.list",
  "production.trace.export",
  "production.trace.open_in_insights"
]) {
  const traceId = manifestById.get(capabilityId)?.inputSchema?.properties?.traceId;
  if (
    !isPlainObject(traceId) ||
    traceId.type !== "string" ||
    traceId.minLength !== 1 ||
    traceId.maxLength !== 128 ||
    traceId.pattern !== traceIdPattern
  ) {
    fail(`${capabilityId}.traceId must use the bounded safe-id contract`);
  }
}
for (const insightsVersion of insightsMappingVersions) {
  const insightsMappingPath = path.join(
    projectRoot,
    "Resources",
    "Trace",
    `insights-actions.${insightsVersion}.json`
  );
  if (!fs.existsSync(insightsMappingPath)) {
    fail(`Missing Resources/Trace/insights-actions.${insightsVersion}.json`);
    continue;
  }
  try {
    const mapping = JSON.parse(fs.readFileSync(insightsMappingPath, "utf8"));
    if (
      mapping.schema !== "ue.trace-insights-actions.v1" ||
      mapping.engineVersion !== insightsVersion ||
      !Array.isArray(mapping.panels)
    ) {
      fail(`Insights action mapping must use the ${insightsVersion} ue.trace-insights-actions.v1 contract`);
    } else {
      const mapped = new Set();
      const panelIds = new Set();
      for (const [index, panel] of mapping.panels.entries()) {
        const location = `insights-actions.${insightsVersion}.json panels[${index}]`;
        if (
          !isPlainObject(panel) ||
          typeof panel.id !== "string" || panel.id.length === 0 ||
          typeof panel.provider !== "string" || panel.provider.length === 0 ||
          typeof panel.capability !== "string" ||
          !Array.isArray(panel.operations) || panel.operations.length === 0 ||
          panel.operations.some((operation) =>
            typeof operation !== "string" || operation.length === 0)
        ) {
          fail(`${location} is invalid`);
          continue;
        }
        if (panelIds.has(panel.id)) fail(`${location}.id is duplicated`);
        panelIds.add(panel.id);
        if (mapped.has(panel.capability)) {
          fail(`${location}.capability is duplicated: ${panel.capability}`);
        }
        mapped.add(panel.capability);
        const capability = manifestById.get(panel.capability);
        if (!capability) {
          fail(`${location}.capability is not in the manifest: ${panel.capability}`);
        } else if (!capability.execution?.backends?.includes("localTrace")) {
          fail(`${panel.capability} must declare the localTrace backend`);
        } else {
          const operationEnum =
            capability.inputSchema?.properties?.operation?.enum;
          if (Array.isArray(operationEnum)) {
            const declared = new Set(operationEnum);
            const mappedOperations = new Set(panel.operations);
            const missingOperations = operationEnum.filter(
              (operation) => !mappedOperations.has(operation)
            );
            const unknownOperations = panel.operations.filter(
              (operation) => !declared.has(operation)
            );
            if (missingOperations.length > 0 || unknownOperations.length > 0) {
              fail(
                `${location}.operations must exactly match ${panel.capability} operation enum; ` +
                `missing=${missingOperations.join(",") || "none"}, ` +
                `unknown=${unknownOperations.join(",") || "none"}`
              );
            }
          }
        }
        if (new Set(panel.operations).size !== panel.operations.length) {
          fail(`${location}.operations contains duplicates`);
        }
      }
      const missingMappings = [...expectedInsightsCapabilities]
        .filter((id) => !mapped.has(id));
      const unexpectedMappings = [...mapped]
        .filter((id) => !expectedInsightsCapabilities.has(id));
      if (missingMappings.length > 0) {
        fail(`Insights action mapping is missing: ${missingMappings.join(", ")}`);
      }
      if (unexpectedMappings.length > 0) {
        fail(`Insights action mapping has unexpected capabilities: ${unexpectedMappings.join(", ")}`);
      }
    }
  } catch (error) {
    fail(`Insights action mapping is not valid JSON: ${error.message}`);
  }
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
      manifests: Object.keys(baselineCounts).length,
      handlers: handlerFiles.length,
      capabilities: manifestIds.size,
      domainCounts: actualDomainCounts,
      kindCounts
    },
    null,
    2
  )
);
