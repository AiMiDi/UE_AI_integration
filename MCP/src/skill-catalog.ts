import {
  existsSync,
  readFileSync,
  readdirSync,
  realpathSync,
  statSync,
} from "node:fs";
import {
  basename,
  isAbsolute,
  relative,
  resolve,
  sep,
} from "node:path";
import { fileURLToPath } from "node:url";

import {
  CAPABILITY_DOMAINS,
  type CapabilityCatalog,
  type CapabilityDescriptor,
  type CapabilityDomain,
} from "./capability-catalog.js";

export type AgentSkillRisk =
  | "readOnly"
  | "safeWrite"
  | "confirmWrite"
  | "mixed";
export type AgentSkillRecipePhase = "discover" | "execute" | "verify";

export interface AgentSkillRequirement {
  engine?: {
    min?: string;
    maxExclusive?: string;
  };
  plugins?: string[];
  capabilities: string[];
  optionalCapabilities?: string[];
}

export interface AgentSkillInput {
  name: string;
  type: "string" | "integer" | "number" | "boolean" | "object" | "array";
  required: boolean;
  description: string;
  example?: unknown;
}

export interface AgentSkillRecipeStep {
  id: string;
  phase: AgentSkillRecipePhase;
  purpose: string;
  operations: string[];
  route?: "domain" | "workflow";
  optional?: boolean;
}

export interface AgentSkillRecipe {
  id: string;
  title: string;
  description: string;
  risk: AgentSkillRisk;
  inputs: AgentSkillInput[];
  steps: AgentSkillRecipeStep[];
  result: {
    summary: string;
    evidence: string[];
    success: string[];
  };
}

export interface AgentSkillResource {
  path: string;
  description: string;
}

export interface AgentSkillDescriptor {
  schema: "ue.agent-skill.v1";
  schemaVersion: 1;
  id: string;
  version: string;
  title: string;
  description: string;
  domains: CapabilityDomain[];
  risk: AgentSkillRisk;
  triggers: string[];
  entrypoint: "SKILL.md";
  requirements: AgentSkillRequirement;
  recipes: AgentSkillRecipe[];
  resources: AgentSkillResource[];
}

export interface AgentSkillSummary {
  id: string;
  version: string;
  title: string;
  description: string;
  domains: CapabilityDomain[];
  risk: AgentSkillRisk;
  triggers: string[];
  recipeIds: string[];
  requiredCapabilityCount: number;
  optionalCapabilityCount: number;
}

export const DEFAULT_SKILL_DIR = fileURLToPath(
  new URL("../../skills/", import.meta.url),
);

export class AgentSkillCatalogError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "AgentSkillCatalogError";
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function requireRecord(
  value: unknown,
  location: string,
): Record<string, unknown> {
  if (!isRecord(value)) {
    throw new AgentSkillCatalogError(`${location} must be an object`);
  }
  return value;
}

function requireString(
  record: Record<string, unknown>,
  key: string,
  location: string,
): string {
  const value = record[key];
  if (typeof value !== "string" || value.trim().length === 0) {
    throw new AgentSkillCatalogError(
      `${location}.${key} must be a non-empty string`,
    );
  }
  return value;
}

function requireStringArray(
  value: unknown,
  location: string,
  allowEmpty = false,
): string[] {
  if (
    !Array.isArray(value) ||
    (!allowEmpty && value.length === 0) ||
    value.some(
      (item) => typeof item !== "string" || item.trim().length === 0,
    )
  ) {
    throw new AgentSkillCatalogError(
      `${location} must be ${allowEmpty ? "an" : "a non-empty"} array of non-empty strings`,
    );
  }
  const unique = [...new Set(value as string[])];
  if (unique.length !== value.length) {
    throw new AgentSkillCatalogError(`${location} must not contain duplicates`);
  }
  return unique;
}

function parseRisk(value: unknown, location: string): AgentSkillRisk {
  const risks: readonly AgentSkillRisk[] = [
    "readOnly",
    "safeWrite",
    "confirmWrite",
    "mixed",
  ];
  if (typeof value !== "string" || !risks.includes(value as AgentSkillRisk)) {
    throw new AgentSkillCatalogError(
      `${location} must be readOnly, safeWrite, confirmWrite, or mixed`,
    );
  }
  return value as AgentSkillRisk;
}

function parseDomains(value: unknown, location: string): CapabilityDomain[] {
  const domains = requireStringArray(value, location);
  for (const domain of domains) {
    if (!CAPABILITY_DOMAINS.includes(domain as CapabilityDomain)) {
      throw new AgentSkillCatalogError(
        `${location} contains unsupported domain "${domain}"`,
      );
    }
  }
  return domains as CapabilityDomain[];
}

function parseRequirements(
  value: unknown,
  location: string,
): AgentSkillRequirement {
  const requirements = requireRecord(value, location);
  const allowed = new Set([
    "engine",
    "plugins",
    "capabilities",
    "optionalCapabilities",
  ]);
  for (const key of Object.keys(requirements)) {
    if (!allowed.has(key)) {
      throw new AgentSkillCatalogError(`${location}.${key} is not supported`);
    }
  }

  let engine: AgentSkillRequirement["engine"];
  if (requirements.engine !== undefined) {
    const value = requireRecord(requirements.engine, `${location}.engine`);
    for (const key of Object.keys(value)) {
      if (key !== "min" && key !== "maxExclusive") {
        throw new AgentSkillCatalogError(
          `${location}.engine.${key} is not supported`,
        );
      }
    }
    const min =
      value.min === undefined
        ? undefined
        : requireString(value, "min", `${location}.engine`);
    const maxExclusive =
      value.maxExclusive === undefined
        ? undefined
        : requireString(value, "maxExclusive", `${location}.engine`);
    engine = {
      ...(min === undefined ? {} : { min }),
      ...(maxExclusive === undefined ? {} : { maxExclusive }),
    };
  }

  const plugins =
    requirements.plugins === undefined
      ? undefined
      : requireStringArray(requirements.plugins, `${location}.plugins`, true);
  const capabilities = requireStringArray(
    requirements.capabilities,
    `${location}.capabilities`,
  );
  const optionalCapabilities =
    requirements.optionalCapabilities === undefined
      ? undefined
      : requireStringArray(
          requirements.optionalCapabilities,
          `${location}.optionalCapabilities`,
          true,
        );
  const requiredSet = new Set(capabilities);
  for (const capability of optionalCapabilities ?? []) {
    if (requiredSet.has(capability)) {
      throw new AgentSkillCatalogError(
        `${location} declares "${capability}" as both required and optional`,
      );
    }
  }

  return {
    ...(engine === undefined ? {} : { engine }),
    ...(plugins === undefined ? {} : { plugins }),
    capabilities,
    ...(optionalCapabilities === undefined ? {} : { optionalCapabilities }),
  };
}

function parseInput(value: unknown, location: string): AgentSkillInput {
  const input = requireRecord(value, location);
  const types = ["string", "integer", "number", "boolean", "object", "array"];
  if (typeof input.type !== "string" || !types.includes(input.type)) {
    throw new AgentSkillCatalogError(
      `${location}.type must be string, integer, number, boolean, object, or array`,
    );
  }
  if (typeof input.required !== "boolean") {
    throw new AgentSkillCatalogError(`${location}.required must be a boolean`);
  }
  return {
    name: requireString(input, "name", location),
    type: input.type as AgentSkillInput["type"],
    required: input.required,
    description: requireString(input, "description", location),
    ...(input.example === undefined ? {} : { example: input.example }),
  };
}

function parseStep(
  value: unknown,
  location: string,
  capabilityIds: ReadonlySet<string>,
  catalog: CapabilityCatalog,
): AgentSkillRecipeStep {
  const step = requireRecord(value, location);
  const phases: readonly AgentSkillRecipePhase[] = [
    "discover",
    "execute",
    "verify",
  ];
  if (
    typeof step.phase !== "string" ||
    !phases.includes(step.phase as AgentSkillRecipePhase)
  ) {
    throw new AgentSkillCatalogError(
      `${location}.phase must be discover, execute, or verify`,
    );
  }
  if (step.optional !== undefined && typeof step.optional !== "boolean") {
    throw new AgentSkillCatalogError(`${location}.optional must be a boolean`);
  }
  if (
    step.route !== undefined &&
    step.route !== "domain" &&
    step.route !== "workflow"
  ) {
    throw new AgentSkillCatalogError(
      `${location}.route must be domain or workflow`,
    );
  }
  const operations = requireStringArray(
    step.operations,
    `${location}.operations`,
  );
  for (const operation of operations) {
    if (!capabilityIds.has(operation)) {
      throw new AgentSkillCatalogError(
        `${location}.operations references undeclared capability "${operation}"`,
      );
    }
    const capability = catalog.get(operation);
    if (
      step.route === "workflow" &&
      capability?.dsl?.admission !== "editStep"
    ) {
      throw new AgentSkillCatalogError(
        `${location}.route workflow requires editStep admission for "${operation}"`,
      );
    }
  }
  if (step.route === "workflow" && step.phase !== "execute") {
    throw new AgentSkillCatalogError(
      `${location}.route workflow is only valid in the execute phase`,
    );
  }
  if (
    step.phase === "verify" &&
    step.optional !== true &&
    operations.some((operation) => !catalog.get(operation)?.traits.readOnly)
  ) {
    throw new AgentSkillCatalogError(
      `${location} contains a write in a non-optional verify step`,
    );
  }
  return {
    id: requireString(step, "id", location),
    phase: step.phase as AgentSkillRecipePhase,
    purpose: requireString(step, "purpose", location),
    operations,
    ...(step.route === undefined
      ? {}
      : { route: step.route as "domain" | "workflow" }),
    ...(step.optional === undefined ? {} : { optional: step.optional }),
  };
}

function parseRecipe(
  value: unknown,
  location: string,
  capabilityIds: ReadonlySet<string>,
  catalog: CapabilityCatalog,
): AgentSkillRecipe {
  const recipe = requireRecord(value, location);
  if (!Array.isArray(recipe.inputs)) {
    throw new AgentSkillCatalogError(`${location}.inputs must be an array`);
  }
  if (!Array.isArray(recipe.steps) || recipe.steps.length === 0) {
    throw new AgentSkillCatalogError(
      `${location}.steps must be a non-empty array`,
    );
  }
  const risk = parseRisk(recipe.risk, `${location}.risk`);
  const inputs = recipe.inputs.map((input, index) =>
    parseInput(input, `${location}.inputs[${index}]`),
  );
  const inputNames = new Set<string>();
  for (const input of inputs) {
    if (!inputNames.add(input.name)) {
      throw new AgentSkillCatalogError(
        `${location}.inputs contains duplicate name "${input.name}"`,
      );
    }
  }
  const steps = recipe.steps.map((step, index) =>
    parseStep(
      step,
      `${location}.steps[${index}]`,
      capabilityIds,
      catalog,
    ),
  );
  const stepIds = new Set<string>();
  for (const step of steps) {
    if (!stepIds.add(step.id)) {
      throw new AgentSkillCatalogError(
        `${location}.steps contains duplicate id "${step.id}"`,
      );
    }
  }
  for (const phase of ["discover", "execute", "verify"] as const) {
    if (!steps.some((step) => step.phase === phase)) {
      throw new AgentSkillCatalogError(
        `${location}.steps must include a ${phase} phase`,
      );
    }
  }
  const operationDescriptors = steps.flatMap((step) =>
    step.operations.map((operation) => catalog.get(operation)!),
  );
  if (
    risk === "readOnly" &&
    operationDescriptors.some(
      (capability) =>
        !capability.traits.readOnly || capability.traits.destructive,
    )
  ) {
    throw new AgentSkillCatalogError(
      `${location}.risk readOnly may reference only read-only, non-destructive capabilities`,
    );
  }
  if (
    risk === "safeWrite" &&
    operationDescriptors.some(
      (capability) =>
        capability.traits.destructive ||
        capability.dsl?.risk === "confirmWrite",
    )
  ) {
    throw new AgentSkillCatalogError(
      `${location}.risk safeWrite may not reference destructive or confirmWrite capabilities`,
    );
  }
  if (
    risk === "confirmWrite" &&
    !operationDescriptors.some(
      (capability) =>
        capability.traits.destructive ||
        capability.dsl?.risk === "confirmWrite",
    )
  ) {
    throw new AgentSkillCatalogError(
      `${location}.risk confirmWrite must include a destructive or confirmWrite capability`,
    );
  }

  const result = requireRecord(recipe.result, `${location}.result`);
  return {
    id: requireString(recipe, "id", location),
    title: requireString(recipe, "title", location),
    description: requireString(recipe, "description", location),
    risk,
    inputs,
    steps,
    result: {
      summary: requireString(result, "summary", `${location}.result`),
      evidence: requireStringArray(
        result.evidence,
        `${location}.result.evidence`,
      ),
      success: requireStringArray(
        result.success,
        `${location}.result.success`,
      ),
    },
  };
}

function ensureRelativeFile(
  skillDirectory: string,
  path: string,
  location: string,
): void {
  if (
    isAbsolute(path) ||
    path.includes("\\") ||
    path.split("/").some((part) => part === ".." || part.length === 0)
  ) {
    throw new AgentSkillCatalogError(
      `${location} must be a normalized relative path`,
    );
  }
  const target = resolve(skillDirectory, path);
  if (!existsSync(target) || !statSync(target).isFile()) {
    throw new AgentSkillCatalogError(`${location} does not exist: ${path}`);
  }
  const realDirectory = realpathSync(skillDirectory);
  const realTarget = realpathSync(target);
  const suffix = relative(realDirectory, realTarget);
  if (suffix.startsWith(`..${sep}`) || suffix === ".." || isAbsolute(suffix)) {
    throw new AgentSkillCatalogError(`${location} escapes the skill directory`);
  }
}

function parseDescriptor(
  value: unknown,
  skillDirectory: string,
  catalog: CapabilityCatalog,
): AgentSkillDescriptor {
  const location = resolve(skillDirectory, "skill.json");
  const skill = requireRecord(value, location);
  if (skill.schema !== "ue.agent-skill.v1" || skill.schemaVersion !== 1) {
    throw new AgentSkillCatalogError(
      `${location} must declare schema ue.agent-skill.v1 and schemaVersion 1`,
    );
  }
  const id = requireString(skill, "id", location);
  if (!/^[a-z][a-z0-9-]*$/.test(id)) {
    throw new AgentSkillCatalogError(
      `${location}.id must be a lowercase kebab-case ID`,
    );
  }
  if (basename(skillDirectory) !== id) {
    throw new AgentSkillCatalogError(
      `${location}.id "${id}" must match its directory name`,
    );
  }
  if (skill.entrypoint !== "SKILL.md") {
    throw new AgentSkillCatalogError(`${location}.entrypoint must be SKILL.md`);
  }
  ensureRelativeFile(skillDirectory, "SKILL.md", `${location}.entrypoint`);

  const requirements = parseRequirements(
    skill.requirements,
    `${location}.requirements`,
  );
  const allCapabilityIds = new Set([
    ...requirements.capabilities,
    ...(requirements.optionalCapabilities ?? []),
  ]);
  const domains = parseDomains(skill.domains, `${location}.domains`);
  for (const capabilityId of allCapabilityIds) {
    const capability = catalog.get(capabilityId);
    if (!capability) {
      throw new AgentSkillCatalogError(
        `${location} references unknown capability "${capabilityId}"`,
      );
    }
    if (!domains.includes(capability.domain)) {
      throw new AgentSkillCatalogError(
        `${location}.domains must include "${capability.domain}" for "${capabilityId}"`,
      );
    }
  }

  if (!Array.isArray(skill.recipes) || skill.recipes.length === 0) {
    throw new AgentSkillCatalogError(
      `${location}.recipes must be a non-empty array`,
    );
  }
  const recipes = skill.recipes.map((recipe, index) =>
    parseRecipe(
      recipe,
      `${location}.recipes[${index}]`,
      allCapabilityIds,
      catalog,
    ),
  );
  const recipeIds = new Set<string>();
  for (const recipe of recipes) {
    if (!recipeIds.add(recipe.id)) {
      throw new AgentSkillCatalogError(
        `${location}.recipes contains duplicate id "${recipe.id}"`,
      );
    }
  }

  if (!Array.isArray(skill.resources)) {
    throw new AgentSkillCatalogError(`${location}.resources must be an array`);
  }
  const resources = skill.resources.map((value, index) => {
    const resource = requireRecord(
      value,
      `${location}.resources[${index}]`,
    );
    const path = requireString(
      resource,
      "path",
      `${location}.resources[${index}]`,
    );
    ensureRelativeFile(
      skillDirectory,
      path,
      `${location}.resources[${index}].path`,
    );
    return {
      path,
      description: requireString(
        resource,
        "description",
        `${location}.resources[${index}]`,
      ),
    };
  });
  if (new Set(resources.map((resource) => resource.path)).size !== resources.length) {
    throw new AgentSkillCatalogError(
      `${location}.resources must not contain duplicate paths`,
    );
  }

  const risk = parseRisk(skill.risk, `${location}.risk`);
  const recipeRisks = new Set(recipes.map((recipe) => recipe.risk));
  const expectedRisk =
    recipeRisks.size === 1 ? [...recipeRisks][0] : "mixed";
  if (risk !== expectedRisk) {
    throw new AgentSkillCatalogError(
      `${location}.risk must be "${expectedRisk}" for its declared recipe risks`,
    );
  }

  const descriptor: AgentSkillDescriptor = {
    schema: "ue.agent-skill.v1",
    schemaVersion: 1,
    id,
    version: requireString(skill, "version", location),
    title: requireString(skill, "title", location),
    description: requireString(skill, "description", location),
    domains,
    risk,
    triggers: requireStringArray(skill.triggers, `${location}.triggers`),
    entrypoint: "SKILL.md",
    requirements,
    recipes,
    resources,
  };

  const frontmatter = readFileSync(resolve(skillDirectory, "SKILL.md"), "utf8")
    .match(/^---\r?\n([\s\S]*?)\r?\n---/)?.[1];
  const frontmatterName = frontmatter
    ?.split(/\r?\n/)
    .find((line) => line.startsWith("name:"))
    ?.slice("name:".length)
    .trim();
  if (frontmatterName !== id) {
    throw new AgentSkillCatalogError(
      `${location} id must match SKILL.md frontmatter name`,
    );
  }
  return descriptor;
}

export class AgentSkillCatalog {
  readonly root: string;
  readonly skills: readonly AgentSkillDescriptor[];

  private readonly byId: ReadonlyMap<string, AgentSkillDescriptor>;
  private readonly directories: ReadonlyMap<string, string>;
  private readonly capabilityCatalog: CapabilityCatalog;

  constructor(
    root: string,
    skills: AgentSkillDescriptor[],
    directories: Map<string, string>,
    capabilityCatalog: CapabilityCatalog,
  ) {
    const byId = new Map<string, AgentSkillDescriptor>();
    for (const skill of skills) {
      if (byId.has(skill.id)) {
        throw new AgentSkillCatalogError(`Duplicate skill ID "${skill.id}"`);
      }
      byId.set(skill.id, skill);
    }
    this.root = root;
    this.skills = skills;
    this.byId = byId;
    this.directories = directories;
    this.capabilityCatalog = capabilityCatalog;
  }

  get(id: string): AgentSkillDescriptor | undefined {
    return this.byId.get(id);
  }

  operation(id: string): CapabilityDescriptor | undefined {
    return this.capabilityCatalog.get(id);
  }

  summary(skill: AgentSkillDescriptor): AgentSkillSummary {
    return {
      id: skill.id,
      version: skill.version,
      title: skill.title,
      description: skill.description,
      domains: skill.domains,
      risk: skill.risk,
      triggers: skill.triggers,
      recipeIds: skill.recipes.map((recipe) => recipe.id),
      requiredCapabilityCount: skill.requirements.capabilities.length,
      optionalCapabilityCount:
        skill.requirements.optionalCapabilities?.length ?? 0,
    };
  }

  read(id: string, path = "SKILL.md"): string {
    const skill = this.get(id);
    const directory = this.directories.get(id);
    if (!skill || !directory) {
      throw new AgentSkillCatalogError(`Unknown skill "${id}"`);
    }
    const allowed = new Set([
      skill.entrypoint,
      ...skill.resources.map((resource) => resource.path),
    ]);
    if (!allowed.has(path)) {
      throw new AgentSkillCatalogError(
        `Resource "${path}" is not declared by skill "${id}"`,
      );
    }
    ensureRelativeFile(directory, path, `${id}:${path}`);
    return readFileSync(resolve(directory, path), "utf8");
  }
}

export function loadAgentSkillCatalog(
  capabilityCatalog: CapabilityCatalog,
  skillDir = DEFAULT_SKILL_DIR,
): AgentSkillCatalog {
  let root: string;
  try {
    root = realpathSync(skillDir);
  } catch (error) {
    throw new AgentSkillCatalogError(
      `Missing skill directory "${skillDir}": ${(error as Error).message}`,
    );
  }

  const directories = readdirSync(root, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => resolve(root, entry.name))
    .filter((directory) => existsSync(resolve(directory, "skill.json")))
    .sort((left, right) => left.localeCompare(right));
  if (directories.length === 0) {
    throw new AgentSkillCatalogError(
      `Skill directory "${skillDir}" contains no skill.json packages`,
    );
  }

  const byDirectory = new Map<string, string>();
  const skills = directories.map((directory) => {
    const path = resolve(directory, "skill.json");
    let parsed: unknown;
    try {
      parsed = JSON.parse(readFileSync(path, "utf8"));
    } catch (error) {
      throw new AgentSkillCatalogError(
        `Invalid JSON in "${path}": ${(error as Error).message}`,
      );
    }
    const descriptor = parseDescriptor(parsed, directory, capabilityCatalog);
    byDirectory.set(descriptor.id, directory);
    return descriptor;
  });
  return new AgentSkillCatalog(root, skills, byDirectory, capabilityCatalog);
}
