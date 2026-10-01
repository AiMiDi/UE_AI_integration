import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { capabilityIsReadOnly, loadCapabilityCatalog, } from "../capability-catalog.js";
import { DEFAULT_SKILL_DIR, loadAgentSkillCatalog, } from "../skill-catalog.js";
const PLUGIN_ROOT = process.env.UEAI_PLUGIN_ROOT ?? join(DEFAULT_SKILL_DIR, "..");
const CAPABILITY_ROOT = process.env.UEAI_CAPABILITY_ROOT ?? join(PLUGIN_ROOT, "Resources", "Capabilities");
const SKILL_ROOT = process.env.UEAI_SKILL_ROOT ?? join(PLUGIN_ROOT, "skills");
function loadCatalogs() {
    const capabilities = loadCapabilityCatalog(CAPABILITY_ROOT);
    const skills = loadAgentSkillCatalog(capabilities, SKILL_ROOT);
    return { capabilities, skills };
}
/**
 * These are the minimum read-after-write promises shared by the three
 * authoring skills.  The table deliberately names public capability IDs so a
 * recipe cannot silently switch to a similarly named legacy operation.
 */
const AUTHORING_CONTRACTS = [
    {
        skill: "ue-niagara-authoring",
        readRecipe: {
            id: "inspect-system",
            verify: ["content.niagara.system.inspect"],
        },
        writeRecipes: [
            {
                id: "edit-computation-graph",
                writes: ["content.niagara.graph.edit.apply"],
                readback: ["content.niagara.graph.inspect"],
            },
            {
                id: "edit-renderer-material",
                writes: ["content.niagara.renderer.material.apply"],
                readback: [
                    "content.niagara.renderer.materials.get",
                    "content.niagara.system.inspect",
                ],
            },
            {
                id: "edit-user-parameter",
                writes: ["content.niagara.system.parameter.apply"],
                readback: [
                    "content.niagara.system.parameter.get",
                    "content.niagara.system.inspect",
                ],
            },
        ],
    },
    {
        skill: "ue-material-editing",
        readRecipe: {
            id: "inspect-graph",
            verify: [
                "content.material.graph.nodes.list",
                "content.material.graph.snapshot.release",
            ],
        },
        writeRecipes: [
            {
                id: "edit-preview",
                writes: ["content.material.editor.batch"],
                readback: [
                    "content.material.editor.context.get",
                    "content.material.custom.get",
                    "content.material.parameter.list",
                    "content.material.diagnostics.get",
                    "content.material.editor.apply.prepare",
                ],
            },
            {
                id: "edit-assets",
                writes: [
                    "content.material.expression.add",
                    "content.material.custom.set",
                    "content.material.parameter.set",
                    "content.material.function.call.set",
                    "content.material.pin.connect",
                ],
                readback: [
                    "content.material.custom.get",
                    "content.material.parameter.list",
                    "content.material.function.call.get",
                    "content.material.diagnostics.get",
                ],
            },
            {
                id: "edit-instance",
                writes: ["content.material.instance.parameters.batch"],
                readback: ["content.material.instance.parameters.get"],
            },
        ],
    },
    {
        skill: "ue-blueprint-authoring",
        readRecipe: {
            id: "inspect-blueprint",
            verify: [
                "blueprint.graph.describe",
                "blueprint.component.list",
            ],
        },
        writeRecipes: [
            {
                id: "edit-variables",
                writes: [
                    "blueprint.variable.add",
                    "blueprint.variable.remove",
                    "blueprint.variable.rename",
                    "blueprint.variable.type.set",
                    "blueprint.variable.metadata.set",
                ],
                readback: ["blueprint.asset.get", "blueprint.property.list"],
            },
            {
                id: "edit-components",
                writes: [
                    "blueprint.component.add",
                    "blueprint.component.remove",
                    "blueprint.component.reparent",
                    "blueprint.component.property.set",
                ],
                readback: [
                    "blueprint.component.list",
                    "blueprint.component.get",
                ],
            },
        ],
    },
];
function recipe(skill, id) {
    const found = skill.recipes.find((candidate) => candidate.id === id);
    assert.ok(found, `${skill.id} is missing recipe ${id}`);
    return found;
}
function operationsInPhase(selected, phase) {
    return selected.steps
        .filter((step) => step.phase === phase)
        .flatMap((step) => step.operations);
}
function descriptorsFor(catalog, operations) {
    return operations.map((operation) => {
        const descriptor = catalog.get(operation);
        assert.ok(descriptor, `recipe references unknown capability ${operation}`);
        return descriptor;
    });
}
test("freezes read-after-write closure for Niagara, material, and Blueprint skills", () => {
    const { capabilities, skills } = loadCatalogs();
    for (const contract of AUTHORING_CONTRACTS) {
        const selected = skills.get(contract.skill);
        assert.ok(selected, `missing authoring skill ${contract.skill}`);
        const inspect = recipe(selected, contract.readRecipe.id);
        const inspectVerify = operationsInPhase(inspect, "verify");
        for (const operation of contract.readRecipe.verify) {
            assert.ok(inspectVerify.includes(operation), `${contract.skill}:${inspect.id} must verify with ${operation}`);
        }
        for (const descriptor of descriptorsFor(capabilities, inspectVerify)) {
            assert.equal(capabilityIsReadOnly(descriptor), true, `${contract.skill}:${inspect.id} verify operation ${descriptor.id} must be read-only`);
        }
        for (const writeContract of contract.writeRecipes) {
            const selectedRecipe = recipe(selected, writeContract.id);
            const executeOperations = operationsInPhase(selectedRecipe, "execute");
            const verifyOperations = operationsInPhase(selectedRecipe, "verify");
            for (const operation of writeContract.writes) {
                assert.ok(executeOperations.includes(operation), `${contract.skill}:${selectedRecipe.id} must execute ${operation}`);
                const descriptor = capabilities.get(operation);
                assert.ok(descriptor);
                assert.equal(descriptor.effects.asset === "write" ||
                    descriptor.effects.editorSession === "write", true, `${operation} must expose an asset/editor write effect`);
            }
            for (const operation of writeContract.readback) {
                assert.ok(verifyOperations.includes(operation), `${contract.skill}:${selectedRecipe.id} must read back with ${operation}`);
                const descriptor = capabilities.get(operation);
                assert.ok(descriptor);
                assert.equal(capabilityIsReadOnly(descriptor), true, `${contract.skill}:${selectedRecipe.id} readback ${operation} must be read-only`);
            }
            // A rollback/release may be a write, but it must be explicitly optional
            // so it cannot masquerade as a successful readback.
            for (const step of selectedRecipe.steps.filter((candidate) => candidate.phase === "verify")) {
                const hasWrite = descriptorsFor(capabilities, step.operations).some((descriptor) => !capabilityIsReadOnly(descriptor));
                if (hasWrite) {
                    assert.equal(step.optional, true, `${contract.skill}:${selectedRecipe.id}:${step.id} write verify step must be optional`);
                }
            }
        }
    }
});
test("keeps bounded identity and pagination schemas for the three authoring surfaces", () => {
    const { capabilities: catalog } = loadCatalogs();
    const expectObjectSchema = (id) => {
        const descriptor = catalog.get(id);
        assert.ok(descriptor, `missing ${id}`);
        assert.equal(descriptor.inputSchema.type, "object", id);
        assert.equal(descriptor.inputSchema.additionalProperties, false, id);
        return descriptor.inputSchema.properties;
    };
    const systemInspect = expectObjectSchema("content.niagara.system.inspect");
    assert.deepEqual(catalog.get("content.niagara.system.inspect")?.inputSchema.required, ["system"]);
    assert.equal(systemInspect.system.minLength, 1);
    assert.equal(systemInspect.system.maxLength, 2048);
    for (const field of [
        "emitterLimit",
        "parameterLimit",
        "rendererLimit",
        "eventHandlerLimit",
        "simulationStageLimit",
    ]) {
        assert.equal(systemInspect[field].type, "integer", field);
        assert.equal(systemInspect[field].minimum, 1, field);
        assert.equal(systemInspect[field].maximum, 128, field);
    }
    for (const field of [
        "emitterOffset",
        "parameterOffset",
        "eventHandlerOffset",
        "simulationStageOffset",
    ]) {
        assert.equal(systemInspect[field].type, "integer", field);
        assert.equal(systemInspect[field].minimum, 0, field);
        assert.equal(systemInspect[field].maximum, 65536, field);
    }
    // Monolith's Niagara asset inventory is pageable. Keep the list contract
    // bounded even when the project contains more systems than one response can
    // safely return. This also prevents a filter-only implementation from
    // silently regressing to an unbounded asset scan.
    const systemList = expectObjectSchema("content.niagara.system.list");
    assert.ok(systemList.limit, "content.niagara.system.list must expose a bounded limit field");
    assert.ok(systemList.offset, "content.niagara.system.list must expose a bounded offset field");
    assert.equal(systemList.limit.type, "integer");
    assert.equal(systemList.limit.minimum, 1);
    assert.equal(systemList.limit.maximum, 200);
    assert.equal(systemList.offset.type, "integer");
    assert.equal(systemList.offset.minimum, 0);
    assert.equal(systemList.offset.maximum, 2147483647);
    const graphIndex = expectObjectSchema("content.material.graph.index");
    assert.equal(graphIndex.assetPath.minLength, 1);
    assert.equal(graphIndex.assetPath.maxLength, 1024);
    assert.deepEqual(graphIndex.targetContext.enum, ["asset", "editorPreview"]);
    assert.match(String(graphIndex.expectedPreviewId.description), /preview/i);
    const graphNodes = expectObjectSchema("content.material.graph.nodes.list");
    assert.deepEqual(catalog.get("content.material.graph.nodes.list")?.inputSchema.required, ["snapshotId"]);
    assert.equal(graphNodes.snapshotId.minLength, 1);
    assert.equal(graphNodes.snapshotId.maxLength, 64);
    assert.equal(graphNodes.limit.type, "integer");
    assert.equal(graphNodes.limit.minimum, 1);
    assert.equal(graphNodes.limit.maximum, 200);
    const blueprintGraph = expectObjectSchema("blueprint.graph.get");
    assert.deepEqual(catalog.get("blueprint.graph.get")?.inputSchema.required, ["name", "graph"]);
    assert.equal(blueprintGraph.name.type, "string");
    assert.equal(blueprintGraph.graph.type, "string");
    const blueprintComponents = expectObjectSchema("blueprint.component.list");
    assert.ok(blueprintComponents.blueprint, "component list needs Blueprint identity");
    assert.equal(blueprintComponents.offset.type, "integer");
    assert.equal(blueprintComponents.offset.minimum, 0);
    assert.equal(blueprintComponents.limit.type, "integer");
    assert.equal(blueprintComponents.limit.minimum, 1);
    assert.equal(blueprintComponents.limit.maximum, 200);
});
test("keeps canonical handlers discoverable for the minimum Monolith parity families", () => {
    const { capabilities: catalog } = loadCatalogs();
    const sourceGroups = [
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Query/NiagaraSystemRead.cpp",
            ids: ["content.niagara.system.inspect"],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Command/Niagara.cpp",
            ids: [
                "content.niagara.system.list",
                "content.niagara.system.create",
                "content.niagara.system.save",
            ],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Content/Query/Material_GraphQuery.cpp",
            ids: [
                "content.material.graph.index",
                "content.material.graph.nodes.list",
                "content.material.graph.subgraph.get",
                "content.material.graph.boundary.get",
                "content.material.graph.snapshot.release",
            ],
        },
        {
            source: "Source/UE_AI_integration/Private/Domains/Blueprint/Query/Blueprint_Read.cpp",
            ids: [
                "blueprint.asset.get",
                "blueprint.graph.get",
                "blueprint.graph.describe",
            ],
        },
    ];
    for (const group of sourceGroups) {
        const source = readFileSync(join(PLUGIN_ROOT, group.source), "utf8");
        for (const id of group.ids) {
            assert.ok(catalog.get(id), `manifest omits ${id}`);
            assert.ok(source.includes(`TEXT("${id}")`), `${group.source} does not bind ${id}`);
        }
    }
});
//# sourceMappingURL=authoring-readback-contract.test.js.map