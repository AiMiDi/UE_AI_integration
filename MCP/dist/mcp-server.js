import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { CAPABILITY_DOMAINS, loadCapabilityCatalog, } from "./capability-catalog.js";
import { DOMAIN_DESCRIPTIONS, DOMAIN_TOOL_NAMES, runDomainOperation, validateDomainOperation, } from "./domain-router.js";
import { formatErrorResponse, formatJsonResponse, } from "./helpers.js";
import { UEApiError, ueClient, } from "./ue-bridge.js";
import { handleWorkflowInput, UE_WORKFLOW_TOOL_SCHEMA, } from "./workflow-router.js";
export const MCP_TOOL_NAMES = [
    "ue_status",
    "ue_capabilities",
    "ue_context",
    "ue_blueprint",
    "ue_scene",
    "ue_content",
    "ue_animation",
    "ue_ai",
    "ue_production",
    "ue_workflow",
];
function resolveLocalCapabilities(catalog, domain, operation) {
    if (operation) {
        const capability = domain
            ? validateDomainOperation(catalog, domain, operation)
            : catalog.get(operation);
        if (!capability) {
            throw new UEApiError({
                code: "capability_not_found",
                message: `Unknown capability "${operation}"`,
            });
        }
        return [capability];
    }
    return domain
        ? [...catalog.forDomain(domain)]
        : [...catalog.capabilities];
}
function filterLiveCapabilities(capabilities, domain, operation) {
    return capabilities.filter((capability) => (domain === undefined || capability.domain === domain) &&
        (operation === undefined || capability.id === operation));
}
async function handleCapabilities(catalog, client, args) {
    try {
        const localCapabilities = resolveLocalCapabilities(catalog, args.domain, args.operation);
        if (!args.live) {
            return formatJsonResponse({
                source: "local",
                ...catalog.summary(),
                capabilities: localCapabilities,
            });
        }
        const live = await client.getCapabilities();
        const capabilities = filterLiveCapabilities(live.capabilities, args.domain, args.operation);
        return formatJsonResponse({
            source: "editor",
            capabilityCount: capabilities.length,
            capabilities,
        });
    }
    catch (error) {
        return formatErrorResponse(error);
    }
}
function handleContext(catalog, args) {
    try {
        if (args.domain === undefined && args.operation === undefined) {
            return formatJsonResponse({
                source: "local-manifest",
                usage: "Pass a domain for its capability schemas, or an operation dotted ID for one exact schema.",
                domains: CAPABILITY_DOMAINS,
                ...catalog.summary(),
            });
        }
        const capabilities = resolveLocalCapabilities(catalog, args.domain, args.operation);
        return formatJsonResponse({
            source: "local-manifest",
            capabilities,
        });
    }
    catch (error) {
        return formatErrorResponse(error);
    }
}
export function createMcpServer(options = {}) {
    const catalog = options.catalog ?? loadCapabilityCatalog();
    const client = options.client ?? ueClient;
    const server = new McpServer({
        name: "ue-ai-integration",
        version: "0.3.0",
    });
    server.tool("ue_status", "Check the running Unreal Editor's UE_AI_integration health status.", {}, async () => {
        try {
            return formatJsonResponse(await client.getHealth());
        }
        catch (error) {
            return formatErrorResponse(error);
        }
    });
    server.tool("ue_capabilities", "List the locally shipped capability catalog offline, or compare against the running editor with live=true.", {
        domain: z.enum(CAPABILITY_DOMAINS).optional(),
        operation: z
            .string()
            .optional()
            .describe("Optional dotted capability ID to inspect"),
        live: z
            .boolean()
            .optional()
            .default(false)
            .describe("Fetch GET /api/capabilities from the running editor"),
    }, async (args) => handleCapabilities(catalog, client, {
        domain: args.domain,
        operation: args.operation,
        live: args.live,
    }));
    server.tool("ue_context", "Read detailed operation schemas, traits, and output kinds from the local manifests.", {
        domain: z.enum(CAPABILITY_DOMAINS).optional(),
        operation: z
            .string()
            .optional()
            .describe("Optional dotted capability ID to inspect"),
    }, async (args) => handleContext(catalog, {
        domain: args.domain,
        operation: args.operation,
    }));
    for (const domain of CAPABILITY_DOMAINS) {
        const toolName = DOMAIN_TOOL_NAMES[domain];
        const operationCount = catalog.forDomain(domain).length;
        server.tool(toolName, `${DOMAIN_DESCRIPTIONS[domain]} This router accepts one of ${operationCount} dotted operation IDs; use ue_capabilities or ue_context to inspect them.`, {
            operation: z
                .string()
                .describe(`Dotted capability ID in the "${domain}" domain`),
            params: z
                .record(z.unknown())
                .optional()
                .default({})
                .describe("Parameters for the selected operation"),
            requestId: z
                .string()
                .trim()
                .min(1)
                .max(128)
                .regex(/^[A-Za-z0-9][A-Za-z0-9._:-]*$/)
                .optional()
                .describe("Optional idempotency key forwarded to POST /api/execute"),
        }, async (args) => runDomainOperation(catalog, client, domain, args.operation, args.params, args.requestId));
    }
    server.registerTool("ue_workflow", {
        description: "Validate, plan, execute, inspect, resume, or roll back a UE Workflow asset-edit run. The workflow must be passed inline; local file paths are not accepted.",
        inputSchema: UE_WORKFLOW_TOOL_SCHEMA,
    }, async (args) => handleWorkflowInput(client, args));
    return {
        server,
        catalog,
        registeredToolNames: MCP_TOOL_NAMES,
    };
}
//# sourceMappingURL=mcp-server.js.map