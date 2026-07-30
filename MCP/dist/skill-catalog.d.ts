import { type CapabilityCatalog, type CapabilityDescriptor, type CapabilityDomain } from "./capability-catalog.js";
export type AgentSkillRisk = "readOnly" | "safeWrite" | "confirmWrite" | "mixed";
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
export declare const DEFAULT_SKILL_DIR: string;
export declare class AgentSkillCatalogError extends Error {
    constructor(message: string);
}
export declare class AgentSkillCatalog {
    readonly root: string;
    readonly skills: readonly AgentSkillDescriptor[];
    private readonly byId;
    private readonly directories;
    private readonly capabilityCatalog;
    constructor(root: string, skills: AgentSkillDescriptor[], directories: Map<string, string>, capabilityCatalog: CapabilityCatalog);
    get(id: string): AgentSkillDescriptor | undefined;
    operation(id: string): CapabilityDescriptor | undefined;
    summary(skill: AgentSkillDescriptor): AgentSkillSummary;
    read(id: string, path?: string): string;
}
export declare function loadAgentSkillCatalog(capabilityCatalog: CapabilityCatalog, skillDir?: string): AgentSkillCatalog;
