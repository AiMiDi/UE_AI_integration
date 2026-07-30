import { type CapabilityDomain } from "./capability-catalog.js";
import {
  type AgentSkillCatalog,
  type AgentSkillDescriptor,
  type AgentSkillRecipe,
  type AgentSkillRisk,
  type AgentSkillSummary,
} from "./skill-catalog.js";
import {
  formatErrorResponse,
  formatJsonResponse,
  type MCPResponse,
} from "./helpers.js";
import { UEApiError } from "./ue-bridge.js";

export type AgentSkillAction = "list" | "get" | "read";

export interface AgentSkillRequest {
  action: AgentSkillAction;
  query?: string;
  domain?: CapabilityDomain;
  operation?: string;
  risk?: AgentSkillRisk;
  skill?: string;
  recipe?: string;
  reference?: string;
  offset?: number;
  limit?: number;
}

function includesOperation(
  skill: AgentSkillDescriptor,
  operation: string,
): boolean {
  return skill.recipes.some((recipe) =>
    recipe.steps.some((step) => step.operations.includes(operation)),
  );
}

function matches(
  skill: AgentSkillDescriptor,
  args: AgentSkillRequest,
): boolean {
  const queryTokens =
    args.query
      ?.trim()
      .toLocaleLowerCase()
      .split(/[^\p{L}\p{N}._-]+/u)
      .filter((token) => token.length > 0) ?? [];
  const searchable = [
    skill.id,
    skill.title,
    skill.description,
    ...skill.triggers,
    ...skill.recipes.flatMap((recipe) => [
      recipe.id,
      recipe.title,
      recipe.description,
    ]),
  ]
    .join(" ")
    .toLocaleLowerCase();
  return (
    (args.domain === undefined || skill.domains.includes(args.domain)) &&
    (args.risk === undefined ||
      skill.risk === args.risk ||
      skill.recipes.some((recipe) => recipe.risk === args.risk)) &&
    (args.operation === undefined ||
      includesOperation(skill, args.operation)) &&
    (queryTokens.length === 0 ||
      queryTokens.some((token) => searchable.includes(token)))
  );
}

function executionTool(operation: string): string {
  const domain = operation.split(".", 1)[0];
  return `ue_${domain}`;
}

function recipeGuide(
  catalog: AgentSkillCatalog,
  recipe: AgentSkillRecipe,
) {
  const operations = [
    ...new Set(recipe.steps.flatMap((step) => step.operations)),
  ];
  const guideStep = (step: AgentSkillRecipe["steps"][number]) => {
    if (step.route === "workflow") {
      return {
        id: step.id,
        phase: step.phase,
        purpose: step.purpose,
        optional: step.optional ?? false,
        route: "workflow",
        tool: "ue_workflow",
        actions: ["plan", "execute"],
        operations: step.operations,
        workflowContract: {
          requiredAstFields: [
            "dsl",
            "dslVersion",
            "workflowKind",
            "workflowId",
            "scope",
            "persistence",
            "operations",
          ],
          operationFields: [
            "id",
            "type",
            "params",
            "bindings when a typed prior output is consumed",
          ],
          planEnvelope: {
            action: "plan",
            workflow: "<complete workflow object>",
          },
          executeEnvelope: {
            action: "execute",
            workflow: "<the same complete workflow object>",
            approvePlanDigest: "<exact digest returned by plan>",
            saveOnSuccess: false,
            confirmWrite: false,
          },
        },
        note:
          "Build one deterministic asset-edit workflow, approve its exact plan digest, and execute the unchanged workflow. Do not issue these operations as unrelated short calls. Read the declared recipe reference for the exact scope and AST example.",
      };
    }
    return {
      id: step.id,
      phase: step.phase,
      purpose: step.purpose,
      optional: step.optional ?? false,
      route: "domain",
      operations: step.operations.map((operation) => {
        const capability = catalog.operation(operation);
        return {
          tool: executionTool(operation),
          operation,
          traits: capability?.traits,
          risk: capability?.dsl?.risk,
          note: capability?.traits.readOnly
            ? "Use the exact params returned by ue_context."
            : "Execute only when this step's condition and caller authorization are satisfied. Use exact ue_context params and put requestId in the domain-tool envelope.",
        };
      }),
    };
  };
  const resultGuide = (step: AgentSkillRecipe["steps"][number]) => ({
    id: step.id,
    phase: "verify" as const,
    purpose: `Read result evidence associated with "${step.id}" without executing mutation, persistence, or rollback operations.`,
    optional: step.optional ?? false,
    route: "domain" as const,
    operations: step.operations
      .filter((operation) => catalog.operation(operation)?.traits.readOnly)
      .map((operation) => ({
        tool: executionTool(operation),
        operation,
        note: "Use the exact params returned by ue_context.",
      })),
  });
  return {
    contract:
      "Skill recipes are guidance, not executable workflows. Discover each exact schema before execution.",
    discoverApi: operations.map((operation) => ({
      tool: "ue_context",
      arguments: { operation },
      liveAvailabilityCheck: {
        tool: "ue_capabilities",
        arguments: {
          operation,
          detail: "full",
          live: true,
          availableOnly: true,
        },
      },
    })),
    performInOrder: recipe.steps.map(guideStep),
    seeResults: recipe.steps
      .filter((step) => step.phase === "verify")
      .map(resultGuide)
      .filter((step) => step.operations.length > 0),
    resultContract: recipe.result,
  };
}

function requireSkill(
  catalog: AgentSkillCatalog,
  id: string | undefined,
): AgentSkillDescriptor {
  if (!id) {
    throw new UEApiError({
      code: "skill_required",
      message: "The skill field is required for this action.",
    });
  }
  const skill = catalog.get(id);
  if (!skill) {
    throw new UEApiError({
      code: "skill_not_found",
      message: `Unknown skill "${id}".`,
    });
  }
  return skill;
}

function requireRecipe(
  skill: AgentSkillDescriptor,
  id: string | undefined,
): AgentSkillRecipe | undefined {
  if (id === undefined) {
    return undefined;
  }
  const recipe = skill.recipes.find((candidate) => candidate.id === id);
  if (!recipe) {
    throw new UEApiError({
      code: "skill_recipe_not_found",
      message: `Skill "${skill.id}" does not declare recipe "${id}".`,
    });
  }
  return recipe;
}

export function handleAgentSkills(
  catalog: AgentSkillCatalog,
  args: AgentSkillRequest,
): MCPResponse {
  try {
    if (args.action === "list") {
      if (args.skill || args.recipe || args.reference) {
        throw new UEApiError({
          code: "invalid_skill_request",
          message:
            "list does not accept skill, recipe, or reference fields.",
        });
      }
      const offset = args.offset ?? 0;
      const limit = Math.min(args.limit ?? 20, 50);
      const matches = [...catalog.skills]
        .filter((skill) => matchesSkill(skill, args))
        .sort((left, right) => left.id.localeCompare(right.id));
      const page = matches.slice(offset, offset + limit);
      return formatJsonResponse({
        schema: "ue.agent-skill-directory.v1",
        source: "local-skill-package",
        total: matches.length,
        offset,
        limit,
        hasMore: offset + page.length < matches.length,
        skills: page.map((skill): AgentSkillSummary =>
          catalog.summary(skill),
        ),
        next:
          "Call ue_skills action=get for one package, then use ue_context for every operation before executing it.",
      });
    }

    const skill = requireSkill(catalog, args.skill);
    if (args.action === "get") {
      if (
        args.query ||
        args.domain ||
        args.operation ||
        args.risk ||
        args.reference ||
        args.offset !== undefined ||
        args.limit !== undefined
      ) {
        throw new UEApiError({
          code: "invalid_skill_request",
          message:
            "get accepts only skill and optional recipe fields.",
        });
      }
      const recipe = requireRecipe(skill, args.recipe);
      const selectedRecipes = recipe ? [recipe] : skill.recipes;
      return formatJsonResponse({
        schema: "ue.agent-skill-loaded.v1",
        source: "local-skill-package",
        skill,
        instructions: catalog.read(skill.id),
        selectedRecipe: recipe?.id ?? null,
        guides: selectedRecipes.map((selected) => ({
          recipeId: selected.id,
          ...recipeGuide(catalog, selected),
        })),
        resources: skill.resources,
      });
    }

    if (args.action === "read") {
      if (
        !args.reference ||
        args.recipe ||
        args.query ||
        args.domain ||
        args.operation ||
        args.risk ||
        args.offset !== undefined ||
        args.limit !== undefined
      ) {
        throw new UEApiError({
          code: "invalid_skill_request",
          message:
            "read requires skill and one declared reference, without filters.",
        });
      }
      const resource = skill.resources.find(
        (candidate) => candidate.path === args.reference,
      );
      if (!resource) {
        throw new UEApiError({
          code: "skill_resource_not_found",
          message: `Skill "${skill.id}" does not declare resource "${args.reference}".`,
        });
      }
      return formatJsonResponse({
        schema: "ue.agent-skill-resource.v1",
        source: "local-skill-package",
        skill: skill.id,
        resource,
        content: catalog.read(skill.id, resource.path),
      });
    }

    throw new UEApiError({
      code: "invalid_skill_action",
      message: `Unsupported skill action "${String(args.action)}".`,
    });
  } catch (error) {
    return formatErrorResponse(error);
  }
}

// Kept separate so the predicate remains easy to unit test through list calls.
function matchesSkill(
  skill: AgentSkillDescriptor,
  args: AgentSkillRequest,
): boolean {
  return matches(skill, args);
}
