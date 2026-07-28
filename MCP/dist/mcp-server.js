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
function resolveLocalCapabilities(catalog, args) {
    if (args.operation) {
        const capability = args.domain
            ? validateDomainOperation(catalog, args.domain, args.operation)
            : catalog.get(args.operation);
        if (!capability) {
            throw new UEApiError({
                code: "capability_not_found",
                message: `Unknown capability "${args.operation}"`,
            });
        }
        return capabilityMatches(capability, args) ? [capability] : [];
    }
    return [...catalog.capabilities]
        .filter((capability) => capabilityMatches(capability, args))
        .sort((left, right) => left.id.localeCompare(right.id));
}
function capabilityMatches(capability, args) {
    const query = args.query?.trim().toLocaleLowerCase();
    return ((args.domain === undefined || capability.domain === args.domain) &&
        (args.operation === undefined || capability.id === args.operation) &&
        (args.kind === undefined || capability.kind === args.kind) &&
        (args.readOnly === undefined ||
            capability.traits.readOnly === args.readOnly) &&
        (args.destructive === undefined ||
            capability.traits.destructive === args.destructive) &&
        (args.expensive === undefined ||
            capability.traits.expensive === args.expensive) &&
        (args.outputKind === undefined ||
            capability.output.kind === args.outputKind) &&
        (query === undefined ||
            query.length === 0 ||
            capability.id.toLocaleLowerCase().includes(query) ||
            capability.description.toLocaleLowerCase().includes(query)));
}
function summarizeCapability(capability) {
    return {
        id: capability.id,
        domain: capability.domain,
        kind: capability.kind,
        description: capability.description,
        traits: capability.traits,
        output: capability.output,
    };
}
function pageLocalCapabilities(catalog, args, detail, defaultLimit, maxLimit) {
    const filtered = resolveLocalCapabilities(catalog, args);
    const offset = args.operation ? 0 : (args.offset ?? 0);
    const limit = args.operation
        ? 1
        : Math.min(args.limit ?? defaultLimit, maxLimit);
    const page = filtered.slice(offset, offset + limit);
    return {
        total: filtered.length,
        offset,
        limit,
        hasMore: offset + page.length < filtered.length,
        detail: args.operation ? "full" : detail,
        capabilities: args.operation || detail === "full"
            ? page
            : page.map(summarizeCapability),
    };
}
export async function handleCapabilities(catalog, client, args) {
    try {
        if (!args.live) {
            return formatJsonResponse({
                source: "local",
                ...catalog.summary(),
                ...pageLocalCapabilities(catalog, args, args.detail ?? "summary", 25, 100),
            });
        }
        const live = await client.getCapabilities({
            query: args.query,
            domain: args.domain,
            operation: args.operation,
            kind: args.kind,
            readOnly: args.readOnly,
            destructive: args.destructive,
            expensive: args.expensive,
            outputKind: args.outputKind,
            offset: args.offset ?? 0,
            limit: args.operation ? 1 : (args.limit ?? 25),
            detail: args.operation ? "full" : (args.detail ?? "summary"),
        });
        return formatJsonResponse({
            source: "editor",
            ...live,
        });
    }
    catch (error) {
        return formatErrorResponse(error);
    }
}
export function handleContext(catalog, args) {
    try {
        if (Object.values(args).every((value) => value === undefined)) {
            return formatJsonResponse({
                source: "local-manifest",
                usage: "Pass filters for paged full schemas, or an operation dotted ID for one exact schema.",
                domains: CAPABILITY_DOMAINS,
                ...catalog.summary(),
            });
        }
        return formatJsonResponse({
            source: "local-manifest",
            ...pageLocalCapabilities(catalog, args, "full", 10, 25),
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
        version: "0.3.1",
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
        query: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Search capability IDs and descriptions"),
        domain: z.enum(CAPABILITY_DOMAINS).optional(),
        operation: z
            .string()
            .optional()
            .describe("Optional dotted capability ID to inspect"),
        kind: z.enum(["query", "command", "validation"]).optional(),
        readOnly: z.boolean().optional(),
        destructive: z.boolean().optional(),
        expensive: z.boolean().optional(),
        outputKind: z.enum(["json", "image"]).optional(),
        offset: z.number().int().min(0).optional().default(0),
        limit: z.number().int().min(1).max(100).optional().default(25),
        detail: z.enum(["summary", "full"]).optional().default("summary"),
        live: z
            .boolean()
            .optional()
            .default(false)
            .describe("Fetch GET /api/capabilities from the running editor"),
    }, async (args) => handleCapabilities(catalog, client, {
        query: args.query,
        domain: args.domain,
        operation: args.operation,
        kind: args.kind,
        readOnly: args.readOnly,
        destructive: args.destructive,
        expensive: args.expensive,
        outputKind: args.outputKind,
        offset: args.offset,
        limit: args.limit,
        detail: args.detail,
        live: args.live,
    }));
    server.tool("ue_context", "Read detailed operation schemas, traits, and output kinds from the local manifests.", {
        query: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Search capability IDs and descriptions"),
        domain: z.enum(CAPABILITY_DOMAINS).optional(),
        operation: z
            .string()
            .optional()
            .describe("Optional dotted capability ID to inspect"),
        kind: z.enum(["query", "command", "validation"]).optional(),
        readOnly: z.boolean().optional(),
        destructive: z.boolean().optional(),
        expensive: z.boolean().optional(),
        outputKind: z.enum(["json", "image"]).optional(),
        offset: z.number().int().min(0).optional(),
        limit: z.number().int().min(1).max(25).optional(),
    }, async (args) => handleContext(catalog, {
        query: args.query,
        domain: args.domain,
        operation: args.operation,
        kind: args.kind,
        readOnly: args.readOnly,
        destructive: args.destructive,
        expensive: args.expensive,
        outputKind: args.outputKind,
        offset: args.offset,
        limit: args.limit,
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