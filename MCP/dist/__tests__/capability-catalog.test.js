import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { afterEach, test } from "node:test";
import { CAPABILITY_DOMAINS, CapabilityManifestError, DEFAULT_MANIFEST_DIR, loadCapabilityCatalog, } from "../capability-catalog.js";
const temporaryDirectories = [];
function createTemporaryDirectory() {
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
    assert.equal(summary.schemaVersion, 3);
    assert.ok(summary.capabilityCount >= 212);
    for (const domain of CAPABILITY_DOMAINS) {
        assert.ok(summary.domainCounts[domain] >= baselines[domain]);
    }
    assert.ok(summary.domainCounts.blueprint >= 82);
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
        if (operation === "blueprint.comment.bounds.set") {
            assert.equal(capability.dsl?.admission, "editStep");
        }
        else {
            assert.equal(capability.dsl, undefined);
        }
    }
    for (const [operation, kind, outputKind] of [
        ["blueprint.layout.validate", "validation", "json"],
        ["blueprint.layout.organize", "command", "json"],
        ["blueprint.graph.capture", "query", "json"],
        ["blueprint.graph.capture.get", "query", "image"],
        ["blueprint.graph.visual.compare", "query", "image"],
    ]) {
        const capability = catalog.get(operation);
        assert.ok(capability, `missing ${operation}`);
        assert.equal(capability.domain, "blueprint");
        assert.equal(capability.kind, kind);
        assert.equal(capability.output.kind, outputKind);
        assert.deepEqual(capability.inputSchema.additionalProperties, false);
    }
    assert.equal(catalog.get("blueprint.layout.organize")?.dsl?.admission, "editStep");
    assert.equal(catalog.manifests.size, CAPABILITY_DOMAINS.length);
    assert.equal(new Set(catalog.capabilities.map((capability) => capability.id)).size, summary.capabilityCount);
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
        assert.equal(capability?.kind, operation === "scene.pie.status" ? "query" : "command");
        assert.equal(capability?.output.kind, "json");
    }
});
test("uses lower camelCase for every public capability input field", () => {
    const catalog = loadCapabilityCatalog();
    const visit = (schema, path) => {
        const properties = schema.properties;
        if (properties !== null &&
            typeof properties === "object" &&
            !Array.isArray(properties)) {
            for (const [field, child] of Object.entries(properties)) {
                assert.match(field, /^[a-z][A-Za-z0-9]*$/, `${path}.${field}`);
                if (child !== null && typeof child === "object" && !Array.isArray(child)) {
                    visit(child, `${path}.${field}`);
                }
            }
        }
        const items = schema.items;
        if (items !== null && typeof items === "object" && !Array.isArray(items)) {
            visit(items, `${path}.items`);
        }
        for (const combinator of ["allOf", "anyOf", "oneOf"]) {
            const entries = schema[combinator];
            if (Array.isArray(entries)) {
                entries.forEach((entry, index) => {
                    if (entry !== null && typeof entry === "object" && !Array.isArray(entry)) {
                        visit(entry, `${path}.${combinator}[${index}]`);
                    }
                });
            }
        }
    };
    for (const capability of catalog.capabilities) {
        visit(capability.inputSchema, capability.id);
    }
});
test("publishes bounded asset-query paging and Blueprint pin identity contracts", () => {
    const catalog = loadCapabilityCatalog();
    const pagedOperations = [
        "blueprint.asset.list",
        "content.material.list",
        "content.material.search",
        "content.material.function.list",
    ];
    for (const operation of pagedOperations) {
        const capability = catalog.get(operation);
        assert.ok(capability, `missing ${operation}`);
        assert.deepEqual(capability.inputSchema.additionalProperties, false);
        const properties = capability.inputSchema.properties;
        assert.deepEqual(properties.limit, {
            type: "integer",
            minimum: 1,
            maximum: 200,
            default: 50,
            description: properties.limit.description,
        }, `${operation}.limit`);
        assert.match(String(properties.limit.description), /determin/i);
        assert.deepEqual(properties.offset, {
            type: "integer",
            minimum: 0,
            maximum: 2147483647,
            default: 0,
            description: properties.offset.description,
        }, `${operation}.offset`);
    }
    const search = catalog.get("content.material.search");
    assert.ok(search);
    const searchProperties = search.inputSchema.properties;
    assert.deepEqual(searchProperties.maxResults, {
        type: "integer",
        minimum: 1,
        maximum: 200,
        description: searchProperties.maxResults.description,
    });
    assert.match(String(searchProperties.maxResults.description), /legacy alias/i);
    assert.match(String(searchProperties.maxResults.description), /ignored when limit is supplied/i);
    const pinDefault = catalog.get("blueprint.pin.default.set");
    assert.ok(pinDefault);
    assert.deepEqual(pinDefault.inputSchema.required, ["blueprint", "nodeId"]);
    assert.deepEqual(pinDefault.inputSchema.anyOf, [
        { required: ["pinId"] },
        { required: ["pinName"] },
    ]);
    assert.deepEqual(pinDefault.inputSchema.oneOf, [
        { required: ["value"], not: { required: ["typedValue"] } },
        { required: ["typedValue"], not: { required: ["value"] } },
    ]);
    const pinProperties = pinDefault.inputSchema.properties;
    assert.equal(pinProperties.pinId.type, "string");
    assert.equal(pinProperties.pinId.minLength, 1);
    assert.match(String(pinProperties.pinId.description), /exact nonzero pin GUID/i);
    assert.match(String(pinProperties.pinName.description), /ambiguous/i);
    const graphGet = catalog.get("blueprint.graph.get");
    assert.ok(graphGet);
    assert.deepEqual(graphGet.inputSchema.required, ["name", "graph"]);
    assert.deepEqual(graphGet.inputSchema.additionalProperties, false);
    const graphProperties = graphGet.inputSchema.properties;
    assert.deepEqual(Object.keys(graphProperties).sort(), [
        "geometryMode",
        "graph",
        "name",
    ]);
    assert.equal(graphProperties.name.type, "string");
    assert.equal(graphProperties.graph.type, "string");
});
test("publishes bounded Blueprint read and local-SCS component contracts", () => {
    const catalog = loadCapabilityCatalog();
    const propertyMap = (operation) => {
        const capability = catalog.get(operation);
        assert.ok(capability, `missing ${operation}`);
        assert.deepEqual(capability.inputSchema.additionalProperties, false);
        return capability.inputSchema.properties;
    };
    const expectIntegerLimit = (properties, field, minimum, maximum, defaultValue) => {
        assert.equal(properties[field]?.type, "integer", field);
        assert.equal(properties[field]?.minimum, minimum, field);
        assert.equal(properties[field]?.maximum, maximum, field);
        assert.equal(properties[field]?.default, defaultValue, field);
    };
    const assetList = catalog.get("blueprint.asset.list");
    assert.ok(assetList);
    const assetListProperties = propertyMap("blueprint.asset.list");
    assert.equal(assetListProperties.filter.maxLength, 512);
    assert.equal(assetListProperties.parentClass.maxLength, 256);
    assert.deepEqual(assetListProperties.type.enum, ["all", "regular", "level"]);
    expectIntegerLimit(assetListProperties, "limit", 1, 200, 50);
    expectIntegerLimit(assetListProperties, "offset", 0, 2147483647, 0);
    expectIntegerLimit(assetListProperties, "maxScannedAssets", 1, 50000, 5000);
    assert.match(String(assetList.description), /partial|scan/i);
    const legacySearch = catalog.get("blueprint.asset.search");
    assert.ok(legacySearch);
    const legacySearchProperties = propertyMap("blueprint.asset.search");
    assert.deepEqual(legacySearch.inputSchema.required, ["query"]);
    assert.equal(legacySearchProperties.query.minLength, 1);
    assert.equal(legacySearchProperties.query.maxLength, 512);
    assert.equal(legacySearchProperties.path.maxLength, 1024);
    expectIntegerLimit(legacySearchProperties, "maxResults", 1, 200, 50);
    expectIntegerLimit(legacySearchProperties, "maxAssets", 1, 5000, 500);
    expectIntegerLimit(legacySearchProperties, "maxScannedNodes", 1, 200000, 50000);
    const graphDescribe = catalog.get("blueprint.graph.describe");
    assert.ok(graphDescribe);
    const graphDescribeProperties = propertyMap("blueprint.graph.describe");
    assert.deepEqual(graphDescribe.inputSchema.required, ["name", "graph"]);
    assert.equal(graphDescribeProperties.name.minLength, 1);
    assert.equal(graphDescribeProperties.name.maxLength, 1024);
    assert.equal(graphDescribeProperties.graph.minLength, 1);
    assert.equal(graphDescribeProperties.graph.maxLength, 256);
    expectIntegerLimit(graphDescribeProperties, "maxNodes", 1, 500, 200);
    expectIntegerLimit(graphDescribeProperties, "maxEdges", 1, 2000, 1000);
    expectIntegerLimit(graphDescribeProperties, "maxScannedNodes", 1, 20000, 5000);
    expectIntegerLimit(graphDescribeProperties, "maxScannedPins", 1, 200000, 50000);
    expectIntegerLimit(graphDescribeProperties, "maxScannedLinks", 1, 500000, 100000);
    assert.match(String(graphDescribe.description), /exec|execution/i);
    assert.match(String(graphDescribe.description), /partial|scan|bound/i);
    const typeSearch = catalog.get("blueprint.asset.search_by_type");
    assert.ok(typeSearch);
    const typeSearchProperties = propertyMap("blueprint.asset.search_by_type");
    assert.deepEqual(typeSearch.inputSchema.required, ["typeName"]);
    assert.equal(typeSearchProperties.typeName.minLength, 1);
    assert.equal(typeSearchProperties.typeName.maxLength, 256);
    assert.equal(typeSearchProperties.filter.maxLength, 512);
    expectIntegerLimit(typeSearchProperties, "maxResults", 1, 500, 200);
    expectIntegerLimit(typeSearchProperties, "maxConnectionsPerPin", 1, 64, 16);
    expectIntegerLimit(typeSearchProperties, "maxAssets", 1, 5000, 500);
    expectIntegerLimit(typeSearchProperties, "assetOffset", 0, 2147483647, 0);
    expectIntegerLimit(typeSearchProperties, "maxScannedVariables", 1, 100000, 10000);
    expectIntegerLimit(typeSearchProperties, "maxScannedNodes", 1, 50000, 5000);
    expectIntegerLimit(typeSearchProperties, "maxScannedPins", 1, 200000, 50000);
    expectIntegerLimit(typeSearchProperties, "maxScannedLinks", 1, 500000, 100000);
    assert.match(String(typeSearch.description), /variable/i);
    assert.match(String(typeSearch.description), /parameter/i);
    assert.match(String(typeSearch.description), /pin/i);
    const componentList = catalog.get("blueprint.component.list");
    assert.ok(componentList);
    const componentListProperties = propertyMap("blueprint.component.list");
    assert.deepEqual(componentList.inputSchema.required, ["blueprint"]);
    assert.equal(componentListProperties.blueprint.minLength, 1);
    assert.equal(componentListProperties.blueprint.maxLength, 1024);
    expectIntegerLimit(componentListProperties, "limit", 1, 200, 50);
    expectIntegerLimit(componentListProperties, "offset", 0, 2147483647, 0);
    expectIntegerLimit(componentListProperties, "maxScannedNodes", 1, 5000, 200);
    expectIntegerLimit(componentListProperties, "maxChildrenPerComponent", 0, 256, 64);
    expectIntegerLimit(componentListProperties, "maxChildEntries", 0, 4096, 1024);
    assert.match(String(componentList.description), /local|SCS/i);
    assert.match(String(componentList.description), /413|hard|whole/i);
    const componentGet = catalog.get("blueprint.component.get");
    assert.ok(componentGet);
    const componentGetProperties = propertyMap("blueprint.component.get");
    assert.deepEqual(componentGet.inputSchema.required, ["blueprint"]);
    assert.deepEqual(componentGet.inputSchema.anyOf, [
        { required: ["componentNodeId"] },
        { required: ["componentName"] },
    ]);
    assert.equal(componentGetProperties.blueprint.maxLength, 1024);
    assert.equal(componentGetProperties.componentName.maxLength, 256);
    assert.equal(componentGetProperties.componentNodeId.maxLength, 36);
    assert.equal(componentGetProperties.property.maxLength, 256);
    assert.equal(componentGetProperties.filter.maxLength, 256);
    expectIntegerLimit(componentGetProperties, "limit", 1, 200, 50);
    expectIntegerLimit(componentGetProperties, "offset", 0, 2147483647, 0);
    assert.match(String(componentGet.description), /local|SCS/i);
    assert.match(String(componentGet.description), /state.?hash/i);
    assert.match(String(componentGet.description), /coverage|complete/i);
    const componentSet = catalog.get("blueprint.component.property.set");
    assert.ok(componentSet);
    const componentSetProperties = propertyMap("blueprint.component.property.set");
    assert.deepEqual(componentSet.inputSchema.required, [
        "blueprint",
        "property",
        "value",
    ]);
    assert.deepEqual(componentSet.inputSchema.anyOf, [
        { required: ["componentNodeId"] },
        { required: ["componentName"] },
    ]);
    assert.equal(componentSetProperties.blueprint.maxLength, 1024);
    assert.equal(componentSetProperties.componentName.maxLength, 256);
    assert.equal(componentSetProperties.componentNodeId.maxLength, 36);
    assert.equal(componentSetProperties.property.maxLength, 256);
    assert.equal(componentSetProperties.value.maxLength, 65536);
    assert.equal(componentSetProperties.expectedOldValue.maxLength, 65536);
    assert.equal(componentSetProperties.expectedTemplatePath.maxLength, 1024);
    assert.equal(componentSetProperties.expectedStateHash.minLength, 71);
    assert.equal(componentSetProperties.expectedStateHash.maxLength, 71);
    assert.match(String(componentSetProperties.expectedStateHash.pattern), /sha256/i);
    assert.equal(componentSet.kind, "command");
    assert.equal(componentSet.effects.asset, "write");
    assert.equal(componentSet.traits.destructive, true);
    assert.match(String(componentSet.description), /local|SCS/i);
    assert.match(String(componentSet.description), /hash|length/i);
    assert.match(String(componentSet.description), /compile/i);
    assert.match(String(componentSet.description), /save/i);
});
test("publishes missing-connection-only material restore semantics", () => {
    const catalog = loadCapabilityCatalog();
    const restore = catalog.get("content.material.graph.restore");
    assert.ok(restore);
    assert.equal(restore.kind, "command");
    assert.deepEqual(restore.inputSchema.required, [
        "material",
        "snapshotId",
        "expectedCurrentDigest",
    ]);
    assert.deepEqual(restore.inputSchema.additionalProperties, false);
    const properties = restore.inputSchema.properties;
    assert.equal(properties.material.type, "string");
    assert.equal(properties.material.minLength, 1);
    assert.equal(properties.snapshotId.type, "string");
    assert.equal(properties.snapshotId.minLength, 1);
    assert.equal(properties.expectedCurrentDigest.type, "string");
    assert.equal(properties.expectedCurrentDigest.minLength, 71);
    assert.equal(properties.expectedCurrentDigest.maxLength, 71);
    assert.match(String(properties.expectedCurrentDigest.pattern), /sha256/i);
    assert.equal(properties.dryRun.type, "boolean");
    assert.equal(properties.dryRun.default, false);
    assert.equal(properties.save.type, "boolean");
    assert.equal(properties.save.default, false);
    assert.match(String(properties.save.description), /true.*reject|not supported/i);
    assert.match(String(restore.description), /missing.*connection/i);
    assert.match(String(restore.description), /not.*full|does not.*full/i);
    assert.match(String(restore.description), /dirty.?only/i);
    assert.equal(restore.traits.destructive, true);
    assert.equal(restore.effects.asset, "write");
});
test("publishes bounded material boundary and definition discovery contracts", () => {
    const catalog = loadCapabilityCatalog();
    const boundary = catalog.get("content.material.graph.boundary.get");
    assert.ok(boundary);
    assert.equal(boundary.kind, "query");
    assert.deepEqual(boundary.inputSchema.required, ["snapshotId", "nodeIds"]);
    assert.deepEqual(boundary.inputSchema.additionalProperties, false);
    const boundaryProperties = boundary.inputSchema.properties;
    assert.deepEqual(boundaryProperties.direction.enum, [
        "upstream",
        "downstream",
        "both",
    ]);
    assert.equal(boundaryProperties.depth.minimum, 0);
    assert.equal(boundaryProperties.depth.maximum, 32);
    assert.equal(boundaryProperties.maxNodes.minimum, 1);
    assert.equal(boundaryProperties.maxNodes.maximum, 200);
    assert.equal(boundaryProperties.maxEdges.minimum, 1);
    assert.equal(boundaryProperties.maxEdges.maximum, 1000);
    assert.equal(boundaryProperties.nodeIds.minItems, 1);
    assert.equal(boundaryProperties.nodeIds.maxItems, 32);
    assert.equal(boundaryProperties.nodeIds.uniqueItems, true);
    const definitions = catalog.get("content.material.graph.definitions.list");
    assert.ok(definitions);
    assert.equal(definitions.kind, "query");
    assert.deepEqual(definitions.inputSchema.additionalProperties, false);
    assert.deepEqual(definitions.inputSchema.not, {
        required: ["definitionKey", "search"],
    });
    const definitionProperties = definitions.inputSchema.properties;
    assert.deepEqual(definitionProperties.assetKind.enum, [
        "material",
        "materialFunction",
    ]);
    assert.equal(definitionProperties.limit.minimum, 1);
    assert.equal(definitionProperties.limit.maximum, 200);
    assert.equal(definitionProperties.cursor.minLength, 1);
    assert.equal(definitionProperties.cursor.maxLength, 2048);
});
test("publishes plan-gated Niagara renderer material contracts", () => {
    const catalog = loadCapabilityCatalog();
    for (const [id, kind] of [
        ["content.niagara.renderer.list", "query"],
        ["content.niagara.renderer.materials.get", "query"],
        ["content.niagara.renderer.material.plan", "query"],
        ["content.niagara.renderer.material.apply", "command"],
        ["content.niagara.renderer.material.rollback", "command"],
    ]) {
        const capability = catalog.get(id);
        assert.ok(capability, `missing ${id}`);
        assert.equal(capability.kind, kind);
        assert.deepEqual(capability.inputSchema.additionalProperties, false);
        assert.equal(capability.lifecycle.status, "active");
        assert.equal(capability.lifecycle.canonicalId, id);
        assert.ok(capability.requires?.features?.includes("Niagara"));
    }
    const list = catalog.get("content.niagara.renderer.list");
    assert.ok(list);
    const listProperties = list.inputSchema.properties;
    assert.deepEqual(list.inputSchema.required, ["system"]);
    assert.equal(listProperties.offset.minimum, 0);
    assert.equal(listProperties.offset.maximum, 65536);
    assert.equal(listProperties.limit.minimum, 1);
    assert.equal(listProperties.limit.maximum, 128);
    const materials = catalog.get("content.niagara.renderer.materials.get");
    assert.ok(materials);
    assert.deepEqual(materials.inputSchema.required, ["system"]);
    assert.deepEqual(materials.inputSchema.allOf, [
        { not: { required: ["rendererPath", "rendererIndex"] } },
        {
            if: { required: ["rendererPath"] },
            then: { required: ["emitter"] },
        },
    ]);
    const plan = catalog.get("content.niagara.renderer.material.plan");
    assert.ok(plan);
    assert.deepEqual(plan.inputSchema.required, [
        "system",
        "emitter",
        "rendererPath",
        "material",
    ]);
    assert.equal(plan.traits.destructive, false);
    assert.equal(plan.effects.asset, "read");
    assert.equal(plan.dsl?.risk, "readOnly");
    const apply = catalog.get("content.niagara.renderer.material.apply");
    assert.ok(apply);
    assert.deepEqual(apply.inputSchema.required, [
        "system",
        "emitter",
        "rendererPath",
        "material",
        "requestId",
        "approvePlanDigest",
        "confirmWrite",
    ]);
    const applyProperties = apply.inputSchema.properties;
    assert.equal(applyProperties.requestId.minLength, 1);
    assert.equal(applyProperties.requestId.maxLength, 128);
    assert.equal(applyProperties.approvePlanDigest.minLength, 64);
    assert.equal(applyProperties.approvePlanDigest.maxLength, 64);
    assert.equal(applyProperties.confirmWrite.const, true);
    assert.equal(apply.traits.destructive, true);
    assert.equal(apply.effects.asset, "write");
    assert.equal(apply.effects.editorSession, "write");
    assert.equal(apply.dsl?.risk, "confirmWrite");
    const rollback = catalog.get("content.niagara.renderer.material.rollback");
    assert.ok(rollback);
    assert.deepEqual(rollback.inputSchema.required, [
        "rollbackId",
        "requestId",
        "confirmWrite",
    ]);
    const rollbackProperties = rollback.inputSchema.properties;
    assert.equal(rollbackProperties.confirmWrite.const, true);
    assert.equal(rollback.dsl?.risk, "confirmWrite");
    assert.equal(catalog.get("content.niagara.renderer.material.set"), undefined);
});
test("publishes bounded Niagara renderer receipt-release contracts", () => {
    const catalog = loadCapabilityCatalog();
    const release = catalog.get("content.niagara.renderer.material.receipt.release");
    assert.ok(release);
    assert.equal(release.kind, "command");
    assert.deepEqual(release.inputSchema.required, [
        "rollbackId",
        "requestId",
        "confirmWrite",
    ]);
    assert.deepEqual(release.inputSchema.additionalProperties, false);
    const properties = release.inputSchema.properties;
    for (const field of ["rollbackId", "requestId"]) {
        assert.equal(properties[field].type, "string", field);
        assert.equal(properties[field].minLength, 1, field);
        assert.equal(properties[field].maxLength, 128, field);
    }
    assert.equal(properties.confirmWrite.type, "boolean");
    assert.equal(properties.confirmWrite.const, true);
    assert.equal(properties.confirmDiscardRollback.type, "boolean");
    assert.equal(properties.confirmDiscardRollback.default, false);
    assert.match(String(properties.confirmDiscardRollback.description), /changed/i);
    assert.match(String(properties.confirmDiscardRollback.description), /rollback/i);
    assert.equal(release.traits.destructive, true);
    assert.equal(release.effects.asset, "none");
    assert.equal(release.effects.editorSession, "write");
    assert.equal(release.dsl?.risk, "confirmWrite");
    assert.match(String(release.description), /bounded|recent|history/i);
    assert.match(String(release.description), /does not.*asset|never.*asset/i);
});
test("publishes plan-gated Niagara authored system-parameter contracts", () => {
    const catalog = loadCapabilityCatalog();
    const ids = [
        "content.niagara.system.parameter.get",
        "content.niagara.system.parameter.plan",
        "content.niagara.system.parameter.apply",
        "content.niagara.system.parameter.rollback",
        "content.niagara.system.parameter.receipt.release",
    ];
    for (const id of ids) {
        const capability = catalog.get(id);
        assert.ok(capability, `missing ${id}`);
        assert.deepEqual(capability.inputSchema.additionalProperties, false, id);
        assert.equal(capability.lifecycle.status, "active", id);
        assert.equal(capability.lifecycle.canonicalId, id, id);
        assert.ok(capability.requires?.features?.includes("Niagara"), id);
        assert.ok(capability.requires?.plugins?.includes("Niagara"), id);
        assert.ok(capability.requires?.modules?.includes("Niagara"), id);
    }
    const expectTarget = (id) => {
        const capability = catalog.get(id);
        assert.ok(capability);
        const properties = capability.inputSchema.properties;
        assert.equal(properties.system.type, "string", `${id}.system`);
        assert.equal(properties.system.minLength, 1, `${id}.system`);
        assert.equal(properties.system.maxLength, 2048, `${id}.system`);
        assert.equal(properties.parameter.type, "string", `${id}.parameter`);
        assert.equal(properties.parameter.minLength, 1, `${id}.parameter`);
        assert.equal(properties.parameter.maxLength, 512, `${id}.parameter`);
        return { capability, properties };
    };
    const get = expectTarget("content.niagara.system.parameter.get");
    assert.deepEqual(get.capability.inputSchema.required, ["system", "parameter"]);
    assert.equal(get.capability.kind, "query");
    assert.equal(get.capability.effects.asset, "read");
    assert.match(String(get.capability.description), /authored.*default/i);
    assert.match(String(get.capability.description), /component.*not|not.*component/i);
    assert.match(String(get.capability.description), /state.*digest/i);
    const plan = expectTarget("content.niagara.system.parameter.plan");
    assert.deepEqual(plan.capability.inputSchema.required, [
        "system",
        "parameter",
        "value",
    ]);
    assert.ok(plan.properties.value, "plan.value schema is missing");
    assert.equal(plan.capability.kind, "query");
    assert.equal(plan.capability.effects.asset, "read");
    assert.equal(plan.capability.dsl?.risk, "readOnly");
    assert.match(String(plan.capability.description), /dirty.?only/i);
    assert.match(String(plan.capability.description), /does not.*compile|without.*compil/i);
    const apply = expectTarget("content.niagara.system.parameter.apply");
    assert.deepEqual(apply.capability.inputSchema.required, [
        "system",
        "parameter",
        "value",
        "requestId",
        "approvePlanDigest",
        "confirmWrite",
    ]);
    assert.ok(apply.properties.value, "apply.value schema is missing");
    assert.equal(apply.properties.requestId.minLength, 1);
    assert.equal(apply.properties.requestId.maxLength, 128);
    assert.equal(apply.properties.approvePlanDigest.minLength, 64);
    assert.equal(apply.properties.approvePlanDigest.maxLength, 64);
    assert.equal(apply.properties.confirmWrite.const, true);
    assert.equal(apply.capability.kind, "command");
    assert.equal(apply.capability.effects.asset, "write");
    assert.equal(apply.capability.effects.editorSession, "write");
    assert.equal(apply.capability.dsl?.risk, "confirmWrite");
    assert.match(String(apply.capability.description), /request.*compil|compile.*request/i);
    assert.match(String(apply.capability.description), /not.*complete|completion.*not/i);
    assert.match(String(apply.capability.description), /never.*save|does not.*save/i);
    const rollback = catalog.get("content.niagara.system.parameter.rollback");
    assert.ok(rollback);
    assert.deepEqual(rollback.inputSchema.required, [
        "rollbackId",
        "requestId",
        "confirmWrite",
    ]);
    const rollbackProperties = rollback.inputSchema.properties;
    for (const field of ["rollbackId", "requestId"]) {
        assert.equal(rollbackProperties[field].minLength, 1, field);
        assert.equal(rollbackProperties[field].maxLength, 128, field);
    }
    assert.equal(rollbackProperties.confirmWrite.const, true);
    assert.equal(rollback.kind, "command");
    assert.equal(rollback.effects.asset, "write");
    assert.equal(rollback.effects.editorSession, "write");
    assert.equal(rollback.dsl?.risk, "confirmWrite");
    assert.match(String(rollback.description), /same.*Editor|session/i);
    assert.match(String(rollback.description), /dirty/i);
    const release = catalog.get("content.niagara.system.parameter.receipt.release");
    assert.ok(release);
    assert.deepEqual(release.inputSchema.required, [
        "rollbackId",
        "requestId",
        "confirmWrite",
    ]);
    const releaseProperties = release.inputSchema.properties;
    assert.equal(releaseProperties.rollbackId.maxLength, 128);
    assert.equal(releaseProperties.requestId.maxLength, 128);
    assert.equal(releaseProperties.confirmWrite.const, true);
    assert.equal(releaseProperties.confirmDiscardRollback.type, "boolean");
    assert.equal(releaseProperties.confirmDiscardRollback.default, false);
    assert.equal(release.kind, "command");
    assert.equal(release.effects.asset, "none");
    assert.equal(release.effects.editorSession, "write");
    assert.equal(release.dsl?.risk, "confirmWrite");
    assert.match(String(release.description), /bounded|recent|history/i);
    assert.match(String(release.description), /changed/i);
    assert.match(String(release.description), /discard.*rollback|rollback.*discard/i);
});
test("keeps focused Blueprint, material, and Niagara handlers declared in the canonical manifests", () => {
    const catalog = loadCapabilityCatalog();
    const pluginRoot = join(DEFAULT_MANIFEST_DIR, "..", "..");
    const groups = [
        {
            source: "Source/UE_AI_integration/Private/Domains/Blueprint/Command/Components.cpp",
            ids: [
                "blueprint.component.list",
                "blueprint.component.get",
                "blueprint.component.property.set",
                "blueprint.component.add",
                "blueprint.component.remove",
            ],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Command/Material_Mutation.cpp",
            ids: ["content.material.graph.restore"],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Query/Material_GraphQuery.cpp",
            ids: [
                "content.material.graph.boundary.get",
                "content.material.graph.definitions.list",
            ],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Command/Niagara_RendererMaterial.cpp",
            ids: [
                "content.niagara.renderer.list",
                "content.niagara.renderer.materials.get",
                "content.niagara.renderer.material.plan",
                "content.niagara.renderer.material.apply",
                "content.niagara.renderer.material.rollback",
                "content.niagara.renderer.material.receipt.release",
            ],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Command/Niagara_SystemParameters.cpp",
            ids: [
                "content.niagara.system.parameter.get",
                "content.niagara.system.parameter.plan",
                "content.niagara.system.parameter.apply",
                "content.niagara.system.parameter.rollback",
                "content.niagara.system.parameter.receipt.release",
            ],
        },
    ];
    for (const group of groups) {
        const sourceText = readFileSync(join(pluginRoot, group.source), "utf8");
        const sourceIds = new Set([...sourceText.matchAll(/GetCapabilityId\(\)[\s\S]{0,160}?TEXT\("([^"]+)"\)/g)].map((match) => match[1]));
        for (const id of group.ids) {
            assert.ok(sourceIds.has(id), `${group.source} does not expose ${id}`);
            const descriptor = catalog.get(id);
            assert.ok(descriptor, `canonical manifest omits source handler ${id}`);
            assert.equal(descriptor.lifecycle.status, "active", id);
            assert.equal(descriptor.lifecycle.canonicalId, id, id);
            assert.equal(descriptor.lifecycle.since, "1.0.0", `${id} must use the current real product version`);
        }
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
        assert.deepEqual(capability.inputSchema.additionalProperties, false, id);
        assert.equal(typeof capability.effects.asset, "string", id);
        assert.equal(typeof capability.traits.destructive, "boolean", id);
        assert.equal(typeof capability.traits.expensive, "boolean", id);
    }
    assert.equal(catalog.get("production.scenario.artifact.get")?.output.kind, "image");
});
test("preserves optional workflow DSL admission metadata", () => {
    const directory = createTemporaryDirectory();
    for (const domain of CAPABILITY_DOMAINS) {
        const capabilities = domain === "blueprint"
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
                    traits: { destructive: false, expensive: false },
                    effects: { asset: "write", world: "none", editorSession: "none", external: "none" },
                    lifecycle: { status: "active", since: "0.10.0", canonicalId: "blueprint.asset.create" },
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
                        features: ["BlueprintAuthoring"],
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
                    traits: { destructive: false, expensive: false },
                    effects: { asset: "read", world: "none", editorSession: "none", external: "none" },
                    lifecycle: { status: "active", since: "0.10.0", canonicalId: "blueprint.asset.get" },
                    output: {
                        kind: "json",
                    },
                },
            ]
            : [];
        writeFileSync(join(directory, `${domain}.json`), JSON.stringify({
            schemaVersion: 3,
            domain,
            capabilities,
        }), "utf8");
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
        features: ["BlueprintAuthoring"],
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
    assert.throws(() => loadCapabilityCatalog(directory), (error) => error instanceof CapabilityManifestError &&
        error.message.includes('Missing capability manifest "') &&
        error.message.includes("blueprint.json"));
});
test("reports a clear error when a manifest is malformed", () => {
    const directory = createTemporaryDirectory();
    writeFileSync(join(directory, "blueprint.json"), JSON.stringify({
        schemaVersion: 3,
        domain: "blueprint",
        capabilities: [{}],
    }), "utf8");
    assert.throws(() => loadCapabilityCatalog(directory), (error) => error instanceof CapabilityManifestError &&
        error.message.includes("capabilities[0].id"));
});
test("rejects malformed optional capability search metadata", () => {
    const directory = createTemporaryDirectory();
    writeFileSync(join(directory, "blueprint.json"), JSON.stringify({
        schemaVersion: 3,
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
                traits: { destructive: false, expensive: false },
                effects: { asset: "read", world: "none", editorSession: "none", external: "none" },
                lifecycle: { status: "active", since: "0.10.0", canonicalId: "blueprint.test.search" },
                output: { kind: "json" },
                search: {
                    keywords: ["layout", "LAYOUT"],
                },
            },
        ],
    }), "utf8");
    assert.throws(() => loadCapabilityCatalog(directory), (error) => error instanceof CapabilityManifestError &&
        error.message.includes("search.keywords") &&
        error.message.includes("duplicates"));
});
//# sourceMappingURL=capability-catalog.test.js.map