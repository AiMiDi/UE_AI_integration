#!/usr/bin/env node

import {
  existsSync,
  readFileSync,
  readdirSync,
  realpathSync,
  statSync,
} from "node:fs";
import { dirname, isAbsolute, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const repositoryRoot = resolve(
  dirname(fileURLToPath(import.meta.url)),
  "..",
);
const skillRoot = resolve(repositoryRoot, "skills");
const capabilityRoot = resolve(repositoryRoot, "Resources", "Capabilities");
const domains = new Set([
  "blueprint",
  "scene",
  "content",
  "animation",
  "ai",
  "production",
]);
const risks = new Set([
  "readOnly",
  "safeWrite",
  "confirmWrite",
  "mixed",
]);
const inputTypes = new Set([
  "string",
  "integer",
  "number",
  "boolean",
  "object",
  "array",
]);

function fail(message) {
  throw new Error(message);
}

function requireString(value, location) {
  if (typeof value !== "string" || value.trim().length === 0) {
    fail(`${location} must be a non-empty string`);
  }
  return value;
}

function requireStrings(value, location, allowEmpty = false) {
  if (
    !Array.isArray(value) ||
    (!allowEmpty && value.length === 0) ||
    value.some(
      (entry) => typeof entry !== "string" || entry.trim().length === 0,
    )
  ) {
    fail(`${location} must be an array of non-empty strings`);
  }
  if (new Set(value).size !== value.length) {
    fail(`${location} contains duplicate values`);
  }
  return value;
}

function loadCapabilities() {
  const capabilities = new Map();
  for (const entry of readdirSync(capabilityRoot, {
    withFileTypes: true,
  })) {
    if (!entry.isFile() || !entry.name.endsWith(".json")) {
      continue;
    }
    const manifestPath = resolve(capabilityRoot, entry.name);
    const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
    if (!Array.isArray(manifest.capabilities)) {
      fail(`${manifestPath} has no capabilities array`);
    }
    for (const capability of manifest.capabilities) {
      const id = requireString(
        capability?.id,
        `${manifestPath}.capability.id`,
      );
      if (capabilities.has(id)) {
        fail(`Duplicate capability "${id}"`);
      }
      capabilities.set(id, capability);
    }
  }
  return capabilities;
}

function validateRelativeFile(skillDirectory, path, location) {
  if (
    isAbsolute(path) ||
    path.includes("\\") ||
    path.split("/").some((part) => part === "" || part === "..")
  ) {
    fail(`${location} must be a normalized relative path`);
  }
  const target = resolve(skillDirectory, path);
  if (!existsSync(target) || !statSync(target).isFile()) {
    fail(`${location} does not exist: ${path}`);
  }
  const suffix = relative(realpathSync(skillDirectory), realpathSync(target));
  if (suffix === ".." || suffix.startsWith(`..${sep}`) || isAbsolute(suffix)) {
    fail(`${location} escapes the skill directory`);
  }
}

function validateSkill(skillDirectory, capabilities) {
  const manifestPath = resolve(skillDirectory, "skill.json");
  const skill = JSON.parse(readFileSync(manifestPath, "utf8"));
  if (skill.schema !== "ue.agent-skill.v1" || skill.schemaVersion !== 1) {
    fail(`${manifestPath} must use ue.agent-skill.v1`);
  }
  const id = requireString(skill.id, `${manifestPath}.id`);
  if (id !== skillDirectory.split(/[\\/]/).at(-1)) {
    fail(`${manifestPath}.id must match its directory name`);
  }
  requireString(skill.version, `${manifestPath}.version`);
  requireString(skill.title, `${manifestPath}.title`);
  requireString(skill.description, `${manifestPath}.description`);
  const skillDomains = new Set(
    requireStrings(skill.domains, `${manifestPath}.domains`),
  );
  for (const domain of skillDomains) {
    if (!domains.has(domain)) {
      fail(`${manifestPath}.domains contains unsupported domain "${domain}"`);
    }
  }
  if (!risks.has(skill.risk)) {
    fail(`${manifestPath}.risk is invalid`);
  }
  requireStrings(skill.triggers, `${manifestPath}.triggers`);
  if (skill.entrypoint !== "SKILL.md") {
    fail(`${manifestPath}.entrypoint must be SKILL.md`);
  }
  validateRelativeFile(skillDirectory, "SKILL.md", `${manifestPath}.entrypoint`);

  const instructions = readFileSync(resolve(skillDirectory, "SKILL.md"), "utf8");
  const frontmatterName = instructions
    .match(/^---\r?\n([\s\S]*?)\r?\n---/)?.[1]
    ?.split(/\r?\n/)
    .find((line) => line.startsWith("name:"))
    ?.slice("name:".length)
    .trim();
  if (frontmatterName !== id) {
    fail(`${manifestPath} id must match SKILL.md frontmatter name`);
  }
  if (/\bTODO\b/.test(instructions)) {
    fail(`${manifestPath} entrypoint contains TODO text`);
  }

  const openAiPath = resolve(skillDirectory, "agents", "openai.yaml");
  validateRelativeFile(
    skillDirectory,
    "agents/openai.yaml",
    `${manifestPath}.agents`,
  );
  if (!readFileSync(openAiPath, "utf8").includes(`$${id}`)) {
    fail(`${openAiPath} default_prompt must mention $${id}`);
  }

  const required = requireStrings(
    skill.requirements?.capabilities,
    `${manifestPath}.requirements.capabilities`,
  );
  const optional = requireStrings(
    skill.requirements?.optionalCapabilities ?? [],
    `${manifestPath}.requirements.optionalCapabilities`,
    true,
  );
  for (const operation of optional) {
    if (required.includes(operation)) {
      fail(
        `${manifestPath}.requirements declares "${operation}" as required and optional`,
      );
    }
  }
  const declared = new Set([...required, ...optional]);
  for (const operation of declared) {
    const capability = capabilities.get(operation);
    if (!capability) {
      fail(`${manifestPath} references unknown capability "${operation}"`);
    }
    if (!skillDomains.has(capability.domain)) {
      fail(
        `${manifestPath}.domains must include "${capability.domain}" for "${operation}"`,
      );
    }
  }

  if (!Array.isArray(skill.recipes) || skill.recipes.length === 0) {
    fail(`${manifestPath}.recipes must be a non-empty array`);
  }
  const recipeIds = new Set();
  const recipeRisks = new Set();
  for (const [recipeIndex, recipe] of skill.recipes.entries()) {
    const location = `${manifestPath}.recipes[${recipeIndex}]`;
    const recipeId = requireString(recipe.id, `${location}.id`);
    if (recipeIds.has(recipeId)) {
      fail(`${location}.id is duplicated`);
    }
    recipeIds.add(recipeId);
    requireString(recipe.title, `${location}.title`);
    requireString(recipe.description, `${location}.description`);
    if (!risks.has(recipe.risk)) {
      fail(`${location}.risk is invalid`);
    }
    recipeRisks.add(recipe.risk);
    if (!Array.isArray(recipe.inputs)) {
      fail(`${location}.inputs must be an array`);
    }
    const inputNames = new Set();
    for (const [inputIndex, input] of recipe.inputs.entries()) {
      const inputLocation = `${location}.inputs[${inputIndex}]`;
      const inputName = requireString(input?.name, `${inputLocation}.name`);
      if (inputNames.has(inputName)) {
        fail(`${inputLocation}.name is duplicated`);
      }
      inputNames.add(inputName);
      if (!inputTypes.has(input?.type)) {
        fail(`${inputLocation}.type is invalid`);
      }
      if (typeof input?.required !== "boolean") {
        fail(`${inputLocation}.required must be boolean`);
      }
      requireString(input?.description, `${inputLocation}.description`);
    }
    if (!Array.isArray(recipe.steps) || recipe.steps.length === 0) {
      fail(`${location}.steps must be non-empty`);
    }
    const phases = new Set();
    const stepIds = new Set();
    let hasGuardedOperation = false;
    for (const [stepIndex, step] of recipe.steps.entries()) {
      const stepLocation = `${location}.steps[${stepIndex}]`;
      const stepId = requireString(step.id, `${stepLocation}.id`);
      if (stepIds.has(stepId)) {
        fail(`${stepLocation}.id is duplicated`);
      }
      stepIds.add(stepId);
      if (!["discover", "execute", "verify"].includes(step.phase)) {
        fail(`${stepLocation}.phase is invalid`);
      }
      if (
        step.route !== undefined &&
        !["domain", "workflow"].includes(step.route)
      ) {
        fail(`${stepLocation}.route must be domain or workflow`);
      }
      if (step.route === "workflow" && step.phase !== "execute") {
        fail(`${stepLocation}.route workflow requires execute phase`);
      }
      if (
        step.optional !== undefined &&
        typeof step.optional !== "boolean"
      ) {
        fail(`${stepLocation}.optional must be boolean`);
      }
      phases.add(step.phase);
      requireString(step.purpose, `${stepLocation}.purpose`);
      for (const operation of requireStrings(
        step.operations,
        `${stepLocation}.operations`,
      )) {
        if (!declared.has(operation)) {
          fail(
            `${stepLocation} uses undeclared capability "${operation}"`,
          );
        }
        const capability = capabilities.get(operation);
        const readOnly = capability?.traits?.readOnly === true;
        const destructive = capability?.traits?.destructive === true;
        const guarded =
          destructive || capability?.dsl?.risk === "confirmWrite";
        hasGuardedOperation ||= guarded;
        if (
          step.route === "workflow" &&
          capability?.dsl?.admission !== "editStep"
        ) {
          fail(
            `${stepLocation}.route workflow requires editStep admission for "${operation}"`,
          );
        }
        if (
          step.phase === "verify" &&
          step.optional !== true &&
          !readOnly
        ) {
          fail(
            `${stepLocation} contains a write in a non-optional verify step`,
          );
        }
        if (recipe.risk === "readOnly" && (!readOnly || destructive)) {
          fail(
            `${location}.risk readOnly may reference only read-only, non-destructive capabilities`,
          );
        }
        if (recipe.risk === "safeWrite" && guarded) {
          fail(
            `${location}.risk safeWrite may not reference destructive or confirmWrite capabilities`,
          );
        }
      }
    }
    for (const phase of ["discover", "execute", "verify"]) {
      if (!phases.has(phase)) {
        fail(`${location} is missing ${phase} phase`);
      }
    }
    if (recipe.risk === "confirmWrite" && !hasGuardedOperation) {
      fail(
        `${location}.risk confirmWrite must include a destructive or confirmWrite capability`,
      );
    }
    requireString(recipe.result?.summary, `${location}.result.summary`);
    requireStrings(recipe.result?.evidence, `${location}.result.evidence`);
    requireStrings(recipe.result?.success, `${location}.result.success`);
  }
  const expectedRisk =
    recipeRisks.size === 1 ? [...recipeRisks][0] : "mixed";
  if (skill.risk !== expectedRisk) {
    fail(
      `${manifestPath}.risk must be "${expectedRisk}" for its recipe risks`,
    );
  }

  if (!Array.isArray(skill.resources)) {
    fail(`${manifestPath}.resources must be an array`);
  }
  for (const [index, resource] of skill.resources.entries()) {
    const location = `${manifestPath}.resources[${index}]`;
    const path = requireString(resource.path, `${location}.path`);
    requireString(resource.description, `${location}.description`);
    validateRelativeFile(skillDirectory, path, `${location}.path`);
  }
  return {
    id,
    recipeCount: skill.recipes.length,
    operationCount: declared.size,
  };
}

try {
  const capabilities = loadCapabilities();
  const packages = readdirSync(skillRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => resolve(skillRoot, entry.name))
    .filter((directory) => existsSync(resolve(directory, "skill.json")))
    .sort();
  if (packages.length === 0) {
    fail("No machine-readable skill packages found");
  }
  const summaries = packages.map((directory) =>
    validateSkill(directory, capabilities),
  );
  const ids = new Set(summaries.map((summary) => summary.id));
  if (ids.size !== summaries.length) {
    fail("Duplicate skill package IDs");
  }
  console.log(
    `Validated ${summaries.length} UE Agent Skills, ` +
      `${summaries.reduce((sum, skill) => sum + skill.recipeCount, 0)} recipes, ` +
      `against ${capabilities.size} capabilities`,
  );
} catch (error) {
  console.error(`Skill validation failed: ${error.message}`);
  process.exitCode = 1;
}
