import assert from "node:assert/strict";
import {
  cpSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { test } from "node:test";

import { loadCapabilityCatalog } from "../capability-catalog.js";
import { runDomainOperation } from "../domain-router.js";
import {
  handleContext,
  type UEConnectionClient,
} from "../mcp-server.js";
import {
  AgentSkillCatalogError,
  DEFAULT_SKILL_DIR,
  loadAgentSkillCatalog,
} from "../skill-catalog.js";
import { handleAgentSkills } from "../skill-router.js";

function textPayload(response: ReturnType<typeof handleAgentSkills>) {
  assert.equal(response.content[0]?.type, "text");
  if (response.content[0]?.type !== "text") {
    assert.fail("Expected text response");
  }
  return JSON.parse(response.content[0].text);
}

test("loads ten validated skill packages with complete recipe phases", () => {
  const capabilities = loadCapabilityCatalog();
  const skills = loadAgentSkillCatalog(capabilities);

  assert.deepEqual(
    skills.skills.map((skill) => skill.id),
    [
      "ue-asset-migration",
      "ue-blueprint-buildgraph",
      "ue-blueprint-diagnose",
      "ue-blueprint-graph-organize",
      "ue-landscape-authoring",
      "ue-performance-regression",
      "ue-render-debug-capture",
      "ue-trace-insights",
      "ue-umg-authoring",
      "ue-world-partition-validate",
    ],
  );
  for (const skill of skills.skills) {
    assert.equal(
      skills
        .read(skill.id)
        .replaceAll("\r\n", "\n")
        .startsWith("---\nname:"),
      true,
    );
    for (const recipe of skill.recipes) {
      assert.deepEqual(
        [...new Set(recipe.steps.map((step) => step.phase))].sort(),
        ["discover", "execute", "verify"],
      );
      for (const operation of recipe.steps.flatMap(
        (step) => step.operations,
      )) {
        assert.ok(capabilities.get(operation), operation);
      }
    }
  }
});

test("projects only read-only operations into See Results", () => {
  const capabilities = loadCapabilityCatalog();
  const skills = loadAgentSkillCatalog(capabilities);
  for (const skill of skills.skills) {
    const loaded = textPayload(
      handleAgentSkills(skills, {
        action: "get",
        skill: skill.id,
      }),
    );
    for (const guide of loaded.guides) {
      for (const step of guide.seeResults) {
        for (const operation of step.operations) {
          assert.equal(
            capabilities.get(operation.operation)?.traits.readOnly,
            true,
            `${skill.id}:${operation.operation}`,
          );
        }
      }
    }
  }
});

test("lists compact matches, then loads instructions and generated API guides", () => {
  const skills = loadAgentSkillCatalog(loadCapabilityCatalog());

  const list = textPayload(
    handleAgentSkills(skills, {
      action: "list",
      operation: "content.widget.child.add",
    }),
  );
  assert.equal(list.total, 1);
  assert.equal(list.skills[0].id, "ue-umg-authoring");
  assert.equal(Object.hasOwn(list.skills[0], "recipes"), false);
  assert.equal(Object.hasOwn(list.skills[0], "instructions"), false);

  const intentMatch = textPayload(
    handleAgentSkills(skills, {
      action: "list",
      query: "check World Partition Data Layer HLOD and PCG health",
      risk: "readOnly",
      operation: "scene.world_partition.get",
    }),
  );
  assert.ok(intentMatch.total >= 1);
  assert.ok(
    intentMatch.skills.some(
      (skill: { id: string }) =>
        skill.id === "ue-world-partition-validate",
    ),
  );

  const loaded = textPayload(
    handleAgentSkills(skills, {
      action: "get",
      skill: "ue-blueprint-diagnose",
      recipe: "scan-and-verify",
    }),
  );
  assert.equal(loaded.schema, "ue.agent-skill-loaded.v1");
  assert.match(loaded.instructions, /# UE Blueprint Diagnose/);
  assert.equal(loaded.selectedRecipe, "scan-and-verify");
  assert.equal(loaded.guides.length, 1);
  const discovery = loaded.guides[0].discoverApi.find(
    (entry: { arguments: { operation: string } }) =>
      entry.arguments.operation === "blueprint.scan",
  );
  assert.deepEqual(discovery.tool, "ue_context");
  assert.equal(discovery.liveAvailabilityCheck.tool, "ue_capabilities");
  assert.equal(
    loaded.guides[0].performInOrder[0].phase,
    "discover",
  );
  assert.ok(
    loaded.guides[0].seeResults.some(
      (step: { operations: Array<{ operation: string }> }) =>
        step.operations.some(
          (operation) =>
            operation.operation === "blueprint.asset.validate",
        ),
    ),
  );

  const umg = textPayload(
    handleAgentSkills(skills, {
      action: "get",
      skill: "ue-umg-authoring",
      recipe: "author-and-read-back",
    }),
  );
  const authorStep = umg.guides[0].performInOrder.find(
    (step: { id: string }) => step.id === "author-ui",
  );
  assert.equal(authorStep.route, "workflow");
  assert.equal(authorStep.tool, "ue_workflow");
  assert.deepEqual(authorStep.actions, ["plan", "execute"]);
  assert.ok(
    authorStep.workflowContract.requiredAstFields.includes("dslVersion"),
  );
  assert.equal(
    authorStep.workflowContract.executeEnvelope.approvePlanDigest,
    "<exact digest returned by plan>",
  );
  const umgResultOperations = umg.guides[0].seeResults.flatMap(
    (step: { operations: Array<{ operation: string }> }) =>
      step.operations.map((operation) => operation.operation),
  );
  assert.equal(
    umgResultOperations.includes("blueprint.asset.save"),
    false,
  );

  const migration = textPayload(
    handleAgentSkills(skills, {
      action: "get",
      skill: "ue-asset-migration",
      recipe: "plan-execute-verify",
    }),
  );
  const migrationResultOperations =
    migration.guides[0].seeResults.flatMap(
      (step: { operations: Array<{ operation: string }> }) =>
        step.operations.map((operation) => operation.operation),
    );
  assert.equal(
    migrationResultOperations.includes("content.asset.change.rollback"),
    false,
  );
  assert.equal(
    migration.guides[0].performInOrder.some(
      (step: {
        optional: boolean;
        operations: Array<{ operation: string }>;
      }) =>
        step.optional &&
        step.operations.some(
          (operation) =>
            operation.operation === "content.asset.change.rollback",
        ),
    ),
    true,
  );
});

test("reads only declared skill resources and rejects traversal", () => {
  const skills = loadAgentSkillCatalog(loadCapabilityCatalog());
  const resource = textPayload(
    handleAgentSkills(skills, {
      action: "read",
      skill: "ue-world-partition-validate",
      reference: "references/world-partition-recipe.md",
    }),
  );
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
    assert.throws(
      () => loadAgentSkillCatalog(loadCapabilityCatalog(), root),
      (error: unknown) =>
        error instanceof AgentSkillCatalogError &&
        /normalized relative path/.test(error.message),
    );
  } finally {
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
    assert.throws(
      () => loadAgentSkillCatalog(loadCapabilityCatalog(), root),
      (error: unknown) =>
        error instanceof AgentSkillCatalogError &&
        /risk readOnly/.test(error.message),
    );

    const invalidRoute = structuredClone(original);
    invalidRoute.recipes[0].steps
      .find((step: { id: string }) => step.id === "author-ui")
      .operations.push("content.widget.event.ensure_handler");
    writeFileSync(manifestPath, JSON.stringify(invalidRoute));
    assert.throws(
      () => loadAgentSkillCatalog(loadCapabilityCatalog(), root),
      (error: unknown) =>
        error instanceof AgentSkillCatalogError &&
        /requires editStep admission/.test(error.message),
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("rejects skill manifests that reference unknown capabilities", () => {
  const root = mkdtempSync(join(tmpdir(), "ue-agent-skill-test-"));
  const directory = join(root, "ue-bad-skill");
  mkdirSync(directory);
  writeFileSync(
    join(directory, "SKILL.md"),
    "---\nname: ue-bad-skill\ndescription: test\n---\n# Bad\n",
  );
  writeFileSync(
    join(directory, "skill.json"),
    JSON.stringify({
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
    }),
  );

  try {
    assert.throws(
      () => loadAgentSkillCatalog(loadCapabilityCatalog(), root),
      (error: unknown) =>
        error instanceof AgentSkillCatalogError &&
        /unknown capability/.test(error.message),
    );
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test("closes load, API discovery, execute, and result verification without a skill executor", async () => {
  const capabilities = loadCapabilityCatalog();
  const skills = loadAgentSkillCatalog(capabilities);
  const calls: Array<{
    operation: string;
    params: Record<string, unknown>;
  }> = [];
  const client: UEConnectionClient = {
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

  const loaded = textPayload(
    handleAgentSkills(skills, {
      action: "get",
      skill: "ue-blueprint-diagnose",
      recipe: "scan-and-verify",
    }),
  );
  assert.equal(loaded.selectedRecipe, "scan-and-verify");

  const scanContext = textPayload(
    await handleContext(capabilities, { operation: "blueprint.scan" }),
  );
  assert.equal(scanContext.capabilities[0].id, "blueprint.scan");
  assert.ok(scanContext.capabilities[0].inputSchema);

  const scan = textPayload(
    await runDomainOperation(
      capabilities,
      client,
      "blueprint",
      "blueprint.scan",
      { asset: "/Game/Blueprints/BP_Player" },
    ),
  );
  assert.equal(scan.scanId, "scan-1");
  assert.equal(scan.findings[0].findingId, "finding-1");

  const verify = textPayload(
    await runDomainOperation(
      capabilities,
      client,
      "blueprint",
      "blueprint.asset.validate",
      { blueprint: "/Game/Blueprints/BP_Player" },
    ),
  );
  assert.equal(verify.valid, true);
  assert.deepEqual(
    calls.map((call) => call.operation),
    ["blueprint.scan", "blueprint.asset.validate"],
  );
});
