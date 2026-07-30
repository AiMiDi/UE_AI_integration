import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { type CapabilityCatalog, type CapabilityDomain, type CapabilityDslRisk, type CapabilityKind, type CapabilityOutputKind } from "./capability-catalog.js";
import { type MCPResponse } from "./helpers.js";
import { type UECapabilitiesData, type UECapabilityQuery, type UEHealthData, type UEWorkflowData, type UEWorkflowRequest } from "./ue-bridge.js";
import { type CliLocationResult } from "./cli-locator.js";
import { type AgentSkillCatalog } from "./skill-catalog.js";
export declare const MCP_TOOL_NAMES: readonly ["ue_status", "ue_capabilities", "ue_context", "ue_skills", "ue_cli", "ue_blueprint", "ue_scene", "ue_content", "ue_animation", "ue_ai", "ue_production", "ue_workflow"];
export type MCPToolName = (typeof MCP_TOOL_NAMES)[number];
export interface UEConnectionClient {
    getHealth(): Promise<UEHealthData>;
    getCapabilities(query?: UECapabilityQuery): Promise<UECapabilitiesData>;
    execute(id: string, params?: Record<string, unknown>, requestId?: string): Promise<Record<string, unknown>>;
    workflow(request: UEWorkflowRequest): Promise<UEWorkflowData>;
}
export interface CreateMcpServerOptions {
    catalog?: CapabilityCatalog;
    skillCatalog?: AgentSkillCatalog;
    client?: UEConnectionClient;
    cliLocator?: () => CliLocationResult;
    shortCliLocator?: () => CliLocationResult;
}
export interface UEAIIntegrationMcpServer {
    server: McpServer;
    catalog: CapabilityCatalog;
    skillCatalog: AgentSkillCatalog;
    registeredToolNames: readonly MCPToolName[];
}
type CapabilityDetail = "summary" | "full";
export declare function handleCapabilities(catalog: CapabilityCatalog, client: UEConnectionClient, args: {
    query?: string;
    domain?: CapabilityDomain;
    operation?: string;
    kind?: CapabilityKind;
    readOnly?: boolean;
    destructive?: boolean;
    expensive?: boolean;
    outputKind?: CapabilityOutputKind;
    risk?: CapabilityDslRisk;
    availableOnly?: boolean;
    offset?: number;
    limit?: number;
    detail?: CapabilityDetail;
    live?: boolean;
}): Promise<MCPResponse>;
export declare function handleContext(catalog: CapabilityCatalog, args: {
    query?: string;
    domain?: CapabilityDomain;
    operation?: string;
    kind?: CapabilityKind;
    readOnly?: boolean;
    destructive?: boolean;
    expensive?: boolean;
    outputKind?: CapabilityOutputKind;
    risk?: CapabilityDslRisk;
    offset?: number;
    limit?: number;
}): MCPResponse;
export declare function createMcpServer(options?: CreateMcpServerOptions): UEAIIntegrationMcpServer;
export {};
