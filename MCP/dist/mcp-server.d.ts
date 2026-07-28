import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { type CapabilityCatalog } from "./capability-catalog.js";
import { type UECapabilitiesData, type UEHealthData, type UEWorkflowData, type UEWorkflowRequest } from "./ue-bridge.js";
export declare const MCP_TOOL_NAMES: readonly ["ue_status", "ue_capabilities", "ue_context", "ue_blueprint", "ue_scene", "ue_content", "ue_animation", "ue_ai", "ue_production", "ue_workflow"];
export type MCPToolName = (typeof MCP_TOOL_NAMES)[number];
export interface UEConnectionClient {
    getHealth(): Promise<UEHealthData>;
    getCapabilities(): Promise<UECapabilitiesData>;
    execute(id: string, params?: Record<string, unknown>, requestId?: string): Promise<Record<string, unknown>>;
    workflow(request: UEWorkflowRequest): Promise<UEWorkflowData>;
}
export interface CreateMcpServerOptions {
    catalog?: CapabilityCatalog;
    client?: UEConnectionClient;
}
export interface UEAIIntegrationMcpServer {
    server: McpServer;
    catalog: CapabilityCatalog;
    registeredToolNames: readonly MCPToolName[];
}
export declare function createMcpServer(options?: CreateMcpServerOptions): UEAIIntegrationMcpServer;
