import { type CapabilityDomain } from "./capability-catalog.js";
import { type AgentSkillCatalog, type AgentSkillRisk } from "./skill-catalog.js";
import { type MCPResponse } from "./helpers.js";
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
export declare function handleAgentSkills(catalog: AgentSkillCatalog, args: AgentSkillRequest): MCPResponse;
