import assert from "node:assert/strict";
import { cpSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync, } from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { test } from "node:test";
import { capabilityIsReadOnly as catalogCapabilityIsReadOnly, loadCapabilityCatalog } from "../capability-catalog.js";
import { runDomainOperation } from "../domain-router.js";
import { handleContext, } from "../mcp-server.js";
import { AgentSkillCatalogError, DEFAULT_SKILL_DIR, loadAgentSkillCatalog, } from "../skill-catalog.js";
import { handleAgentSkills } from "../skill-router.js";
function capabilityIsReadOnlyForContract(capability) {
    return capability !== undefined
        && catalogCapabilityIsReadOnly(capability)
        && capability.effects.editorSession !== "write";
}
function capabilityHasWriteEffect(capability) {
    return capability !== undefined && (capability.effects.asset === "write"
        || capability.effects.world === "write"
        || capability.effects.editorSession === "write"
        || capability.effects.external === "write");
}
function textPayload(response) {
    assert.equal(response.content[0]?.type, "text");
    if (response.content[0]?.type !== "text") {
        assert.fail("Expected text response");
    }
    return JSON.parse(response.content[0].text);
}
test("loads fourteen validated skill packages with complete recipe phases", () => {
    const capabilities = loadCapabilityCatalog();
    const skills = loadAgentSkillCatalog(capabilities);
    assert.deepEqual(skills.skills.map((skill) => skill.id), [
        "ue-asset-migration",
        "ue-blueprint-authoring",
        "ue-blueprint-buildgraph",
        "ue-blueprint-diagnose",
        "ue-blueprint-graph-organize",
        "ue-landscape-authoring",
        "ue-material-editing",
        "ue-niagara-authoring",
        "ue-performance-regression",
        "ue-recovery-operator",
        "ue-render-debug-capture",
        "ue-trace-insights",
        "ue-umg-authoring",
        "ue-world-partition-validate",
    ]);
    for (const skill of skills.skills) {
        assert.equal(skills
            .read(skill.id)
            .replaceAll("\r\n", "\n")
            .startsWith("---\nname:"), true);
        for (const recipe of skill.recipes) {
            assert.deepEqual([...new Set(recipe.steps.map((step) => step.phase))].sort(), ["discover", "execute", "verify"]);
            for (const operation of recipe.steps.flatMap((step) => step.operations)) {
                assert.ok(capabilities.get(operation), operation);
            }
        }
    }
});
test("routes Niagara graph and authored-parameter work through the authoring skill", () => {
    const manifestPath = join(DEFAULT_SKILL_DIR, "ue-niagara-authoring", "skill.json");
    const instructionsPath = join(DEFAULT_SKILL_DIR, "ue-niagara-authoring", "SKILL.md");
    const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
    assert.equal(manifest.id, "ue-niagara-authoring");
    assert.deepEqual(manifest.domains, ["content"]);
    assert.equal(manifest.risk, "mixed");
    const requiredOperationGroups = [
        ["content.niagara.graph.operations.list", "content.niagara.graph.inspect"],
        [
            "content.niagara.graph.edit.plan",
            "content.niagara.graph.edit.apply",
            "content.niagara.graph.edit.rollback",
        ],
        [
            "content.niagara.graph.module.add.plan",
            "content.niagara.graph.module.add.apply",
            "content.niagara.graph.module.add.rollback",
        ],
        [
            "content.niagara.graph.node.set_enabled.plan",
            "content.niagara.graph.node.set_enabled.apply",
            "content.niagara.graph.node.set_enabled.rollback",
        ],
        [
            "content.niagara.graph.pin.set_default.plan",
            "content.niagara.graph.pin.set_default.apply",
            "content.niagara.graph.pin.set_default.rollback",
        ],
        [
            "content.niagara.renderer.material.plan",
            "content.niagara.renderer.material.apply",
            "content.niagara.renderer.material.rollback",
            "content.niagara.renderer.material.receipt.release",
        ],
        [
            "content.niagara.system.parameter.get",
            "content.niagara.system.parameter.plan",
            "content.niagara.system.parameter.apply",
            "content.niagara.system.parameter.rollback",
            "content.niagara.system.parameter.receipt.release",
        ],
    ];
    for (const group of requiredOperationGroups) {
        for (const operation of group) {
            assert.ok(manifest.requirements.capabilities.includes(operation), `ue-niagara-authoring requirements omit ${operation}`);
        }
        const routedOperations = manifest.recipes.flatMap((recipe) => recipe.steps.flatMap((step) => step.operations));
        for (const operation of group) {
            assert.ok(routedOperations.includes(operation), `ue-niagara-authoring recipes do not route ${operation}`);
        }
    }
    for (const recipe of manifest.recipes) {
        assert.deepEqual([...new Set(recipe.steps.map((step) => step.phase))].sort(), ["discover", "execute", "verify"], `${recipe.id} must close discover/execute/verify`);
        for (const step of recipe.steps) {
            const containsWrite = step.operations.some((operation) => /\.(?:apply|rollback|release)$/.test(operation));
            if (step.phase === "verify" && containsWrite) {
                assert.equal(step.optional, true, `${recipe.id} write-capable verify step must be optional`);
            }
        }
    }
    const parameterRecipe = manifest.recipes.find((recipe) => recipe.id === "edit-user-parameter");
    assert.ok(parameterRecipe);
    assert.equal(parameterRecipe.risk, "confirmWrite");
    assert.equal(parameterRecipe.inputs.find((input) => input.name === "value")?.type, "json", "typed Niagara values need a scalar-or-object JSON input contract");
    const serializedManifest = JSON.stringify(manifest);
    const instructions = readFileSync(instructionsPath, "utf8");
    assert.doesNotMatch(serializedManifest, /renderer\.material\.set/);
    assert.doesNotMatch(instructions, /renderer\.material\.set/);
    assert.match(instructions, /dirtyOnly/);
    assert.match(instructions, /does not mean compilation completed/i);
    assert.match(instructions, /component overrides.*outside/i);
});
test("validates Niagara authoring operation domains and risk boundaries", () => {
    const capabilities = loadCapabilityCatalog();
    const skills = loadAgentSkillCatalog(capabilities);
    const niagara = skills.skills.find((skill) => skill.id === "ue-niagara-authoring");
    assert.ok(niagara);
    for (const operation of niagara.requirements.capabilities) {
        const capability = capabilities.get(operation);
        assert.ok(capability, `missing ${operation}`);
        assert.equal(capability.domain, "content", operation);
    }
    for (const recipe of niagara.recipes) {
        const operations = recipe.steps.flatMap((step) => step.operations);
        if (recipe.risk === "readOnly") {
            for (const operation of operations) {
                assert.equal(capabilityIsReadOnlyForContract(capabilities.get(operation)), true, `${recipe.id}:${operation}`);
            }
        }
        else if (recipe.risk === "safeWrite") {
            // Direct non-destructive writes (save, add) carry no plan/apply chain.
            const writeOperations = operations.filter((operation) => !capabilityIsReadOnlyForContract(capabilities.get(operation)));
            for (const operation of writeOperations) {
                const capability = capabilities.get(operation);
                assert.ok(capability, operation);
                assert.equal(capabilityHasWriteEffect(capability), true, `${recipe.id}:${operation}`);
                assert.notEqual(capability.traits.destructive, true, `${recipe.id}:${operation}`);
            }
        }
        else {
            assert.equal(recipe.risk, "confirmWrite", recipe.id);
            const writeOperations = operations.filter((operation) => !capabilityIsReadOnlyForContract(capabilities.get(operation)));
            assert.ok(writeOperations.length > 0, `${recipe.id} has no write operation`);
            const isGuarded = (operation) => {
                const capability = capabilities.get(operation);
                return (capability?.traits.destructive === true ||
                    capability?.dsl?.risk === "confirmWrite");
            };
            assert.ok(writeOperations.some(isGuarded), `${recipe.id} has no destructive/confirmWrite operation`);
            for (const operation of writeOperations) {
                const capability = capabilities.get(operation);
                assert.ok(capability, operation);
                assert.equal(capabilityHasWriteEffect(capability), true, `${recipe.id}:${operation}`);
            }
        }
    }
});
test("selects Niagara authoring instead of read-only render diagnostics for writes", () => {
    const capabilities = loadCapabilityCatalog();
    const skills = loadAgentSkillCatalog(capabilities);
    for (const operation of [
        "content.niagara.graph.edit.apply",
        "content.niagara.graph.module.add.apply",
        "content.niagara.graph.node.set_enabled.apply",
        "content.niagara.graph.pin.set_default.apply",
        "content.niagara.renderer.material.apply",
        "content.niagara.system.parameter.apply",
    ]) {
        const result = textPayload(handleAgentSkills(skills, {
            action: "list",
            operation,
            risk: "confirmWrite",
        }));
        assert.ok(result.skills.some((skill) => skill.id === "ue-niagara-authoring"), operation);
        assert.equal(result.skills.some((skill) => skill.id === "ue-render-debug-capture"), false, operation);
    }
});
test("projects only read-only operations into See Results", () => {
    const capabilities = loadCapabilityCatalog();
    const skills = loadAgentSkillCatalog(capabilities);
    for (const skill of skills.skills) {
        const loaded = textPayload(handleAgentSkills(skills, {
            action: "get",
            skill: skill.id,
        }));
        for (const guide of loaded.guides) {
            for (const step of guide.seeResults) {
                for (const operation of step.operations) {
                    const capability = capabilities.get(operation.operation);
                    assert.equal(capabilityIsReadOnlyForContract(capability), true, `${skill.id}:${operation.operation}`);
                }
            }
        }
    }
});
test("lists compact matches, then loads instructions and generated API guides", () => {
    const skills = loadAgentSkillCatalog(loadCapabilityCatalog());
    const list = textPayload(handleAgentSkills(skills, {
        action: "list",
        operation: "content.widget.child.add",
    }));
    assert.equal(list.total, 1);
    assert.equal(list.skills[0].id, "ue-umg-authoring");
    assert.equal(Object.hasOwn(list.skills[0], "recipes"), false);
    assert.equal(Object.hasOwn(list.skills[0], "instructions"), false);
    const intentMatch = textPayload(handleAgentSkills(skills, {
        action: "list",
        query: "check World Partition Data Layer HLOD and PCG health",
        risk: "readOnly",
        operation: "scene.world_partition.get",
    }));
    assert.ok(intentMatch.total >= 1);
    assert.ok(intentMatch.skills.some((skill) => skill.id === "ue-world-partition-validate"));
    const loaded = textPayload(handleAgentSkills(skills, {
        action: "get",
        skill: "ue-blueprint-diagnose",
        recipe: "scan-and-verify",
    }));
    assert.equal(loaded.schema, "ue.agent-skill-loaded.v1");
    assert.match(loaded.instructions, /# UE Blueprint Diagnose/);
    assert.equal(loaded.selectedRecipe, "scan-and-verify");
    assert.equal(loaded.guides.length, 1);
    const discovery = loaded.guides[0].discoverApi.find((entry) => entry.arguments.operation === "blueprint.scan");
    assert.deepEqual(discovery.tool, "ue_context");
    assert.equal(discovery.liveAvailabilityCheck.tool, "ue_capabilities");
    assert.equal(loaded.guides[0].performInOrder[0].phase, "discover");
    assert.ok(loaded.guides[0].seeResults.some((step) => step.operations.some((operation) => operation.operation === "blueprint.asset.validate")));
    const umg = textPayload(handleAgentSkills(skills, {
        action: "get",
        skill: "ue-umg-authoring",
        recipe: "author-and-read-back",
    }));
    const authorStep = umg.guides[0].performInOrder.find((step) => step.id === "author-ui");
    assert.equal(authorStep.route, "workflow");
    assert.equal(authorStep.tool, "ue_workflow");
    assert.deepEqual(authorStep.actions, ["plan", "execute"]);
    assert.ok(authorStep.workflowContract.requiredAstFields.includes("dslVersion"));
    assert.equal(authorStep.workflowContract.executeEnvelope.approvePlanDigest, "<exact digest returned by plan>");
    const umgResultOperations = umg.guides[0].seeResults.flatMap((step) => step.operations.map((operation) => operation.operation));
    assert.equal(umgResultOperations.includes("blueprint.asset.save"), false);
    const migration = textPayload(handleAgentSkills(skills, {
        action: "get",
        skill: "ue-asset-migration",
        recipe: "plan-execute-verify",
    }));
    const migrationResultOperations = migration.guides[0].seeResults.flatMap((step) => step.operations.map((operation) => operation.operation));
    assert.equal(migrationResultOperations.includes("content.asset.change.rollback"), false);
    assert.equal(migration.guides[0].performInOrder.some((step) => step.optional &&
        step.operations.some((operation) => operation.operation === "content.asset.change.rollback")), true);
});
test("reads only declared skill resources and rejects traversal", () => {
    const skills = loadAgentSkillCatalog(loadCapabilityCatalog());
    const resource = textPayload(handleAgentSkills(skills, {
        action: "read",
        skill: "ue-world-partition-validate",
        reference: "references/world-partition-recipe.md",
    }));
    assert.match(resource.content, /# World Partition validation recipe/);
    const traversal = handleAgentSkills(skills, {
        action: "read",
        skill: "ue-world-partition-validate",
        reference: "../skill.json",
    });
    assert.equal(traversal.isError, true);
    assert.equal(textPayload(traversal).error.code, "skill_resource_not_found");
});
test("rejects a package manifest whose declared resource escapes its directory", () => {
    const root = mkdtempSync(join(tmpdir(), "ue-agent-skill-path-test-"));
    const directory = join(root, "ue-blueprint-diagnose");
    cpSync(join(DEFAULT_SKILL_DIR, "ue-blueprint-diagnose"), directory, {
        recursive: true,
    });
    const manifestPath = join(directory, "skill.json");
    const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
    manifest.resources[0].path = "../outside.md";
    writeFileSync(manifestPath, JSON.stringify(manifest));
    try {
        assert.throws(() => loadAgentSkillCatalog(loadCapabilityCatalog(), root), (error) => error instanceof AgentSkillCatalogError &&
            /normalized relative path/.test(error.message));
    }
    finally {
        rmSync(root, { recursive: true, force: true });
    }
});
test("rejects unsafe readOnly claims and non-editStep workflow routes", () => {
    const root = mkdtempSync(join(tmpdir(), "ue-agent-skill-risk-test-"));
    const directory = join(root, "ue-umg-authoring");
    cpSync(join(DEFAULT_SKILL_DIR, "ue-umg-authoring"), directory, {
        recursive: true,
    });
    const manifestPath = join(directory, "skill.json");
    const original = JSON.parse(readFileSync(manifestPath, "utf8"));
    const unsafeRisk = structuredClone(original);
    unsafeRisk.risk = "readOnly";
    unsafeRisk.recipes[0].risk = "readOnly";
    writeFileSync(manifestPath, JSON.stringify(unsafeRisk));
    try {
        assert.throws(() => loadAgentSkillCatalog(loadCapabilityCatalog(), root), (error) => error instanceof AgentSkillCatalogError &&
            /risk readOnly/.test(error.message));
        const invalidRoute = structuredClone(original);
        invalidRoute.recipes[0].steps
            .find((step) => step.id === "author-ui")
            .operations.push("content.widget.event.ensure_handler");
        writeFileSync(manifestPath, JSON.stringify(invalidRoute));
        assert.throws(() => loadAgentSkillCatalog(loadCapabilityCatalog(), root), (error) => error instanceof AgentSkillCatalogError &&
            /requires editStep admission/.test(error.message));
    }
    finally {
        rmSync(root, { recursive: true, force: true });
    }
});
test("rejects skill manifests that reference unknown capabilities", () => {
    const root = mkdtempSync(join(tmpdir(), "ue-agent-skill-test-"));
    const directory = join(root, "ue-bad-skill");
    mkdirSync(directory);
    writeFileSync(join(directory, "SKILL.md"), "---\nname: ue-bad-skill\ndescription: test\n---\n# Bad\n");
    writeFileSync(join(directory, "skill.json"), JSON.stringify({
        schema: "ue.agent-skill.v1",
        schemaVersion: 1,
        id: "ue-bad-skill",
        version: "1.0.0",
        title: "Bad",
        description: "Bad fixture",
        domains: ["blueprint"],
        risk: "readOnly",
        triggers: ["bad"],
        entrypoint: "SKILL.md",
        requirements: {
            capabilities: ["blueprint.operation.does_not_exist"],
        },
        recipes: [
            {
                id: "bad",
                title: "Bad",
                description: "Bad",
                risk: "readOnly",
                inputs: [],
                steps: [
                    {
                        id: "d",
                        phase: "discover",
                        purpose: "Bad",
                        operations: ["blueprint.operation.does_not_exist"],
                    },
                    {
                        id: "e",
                        phase: "execute",
                        purpose: "Bad",
                        operations: ["blueprint.operation.does_not_exist"],
                    },
                    {
                        id: "v",
                        phase: "verify",
                        purpose: "Bad",
                        operations: ["blueprint.operation.does_not_exist"],
                    },
                ],
                result: {
                    summary: "Bad",
                    evidence: ["Bad"],
                    success: ["Bad"],
                },
            },
        ],
        resources: [],
    }));
    try {
        assert.throws(() => loadAgentSkillCatalog(loadCapabilityCatalog(), root), (error) => error instanceof AgentSkillCatalogError &&
            /unknown capability/.test(error.message));
    }
    finally {
        rmSync(root, { recursive: true, force: true });
    }
});
test("closes load, API discovery, execute, and result verification without a skill executor", async () => {
    const capabilities = loadCapabilityCatalog();
    const skills = loadAgentSkillCatalog(capabilities);
    const calls = [];
    const client = {
        getHealth: async () => {
            throw new Error("not expected");
        },
        getCapabilities: async () => {
            throw new Error("not expected");
        },
        execute: async (operation, params = {}) => {
            calls.push({ operation, params });
            if (operation === "blueprint.scan") {
                return {
                    scanId: "scan-1",
                    asset: params.asset,
                    findings: [
                        {
                            findingId: "finding-1",
                            ruleId: "tick-expensive-call",
                            severity: "warning",
                        },
                    ],
                };
            }
            return {
                asset: params.blueprint,
                valid: true,
                errors: 0,
            };
        },
        workflow: async () => {
            throw new Error("not expected");
        },
    };
    const loaded = textPayload(handleAgentSkills(skills, {
        action: "get",
        skill: "ue-blueprint-diagnose",
        recipe: "scan-and-verify",
    }));
    assert.equal(loaded.selectedRecipe, "scan-and-verify");
    const scanContext = textPayload(await handleContext(capabilities, { operation: "blueprint.scan" }));
    assert.equal(scanContext.capabilities[0].id, "blueprint.scan");
    assert.ok(scanContext.capabilities[0].inputSchema);
    const scan = textPayload(await runDomainOperation(capabilities, client, "blueprint", "blueprint.scan", { asset: "/Game/Blueprints/BP_Player" }));
    assert.equal(scan.scanId, "scan-1");
    assert.equal(scan.findings[0].findingId, "finding-1");
    const verify = textPayload(await runDomainOperation(capabilities, client, "blueprint", "blueprint.asset.validate", { blueprint: "/Game/Blueprints/BP_Player" }));
    assert.equal(verify.valid, true);
    assert.deepEqual(calls.map((call) => call.operation), ["blueprint.scan", "blueprint.asset.validate"]);
});
//# sourceMappingURL=skill-catalog.test.js.map