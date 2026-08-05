import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import { CAPABILITY_DOMAINS, loadCapabilityCatalog, } from "./capability-catalog.js";
import { compareCapabilityIds, matchCapabilitySearch, } from "./capability-search.js";
import { BackendRoutingExecutor, DOMAIN_DESCRIPTIONS, DOMAIN_TOOL_NAMES, runDomainOperation, validateDomainOperation, } from "./domain-router.js";
import { formatErrorResponse, formatJsonResponse, } from "./helpers.js";
import { UEApiError, ueClient, } from "./ue-bridge.js";
import { handleWorkflowInput, UE_WORKFLOW_TOOL_SCHEMA, } from "./workflow-router.js";
import { locateShortCli, locateWorkflowCli, } from "./cli-locator.js";
import { loadAgentSkillCatalog, } from "./skill-catalog.js";
import { handleAgentSkills } from "./skill-router.js";
import { TraceWorkerClient } from "./trace-worker.js";
import { RecipeRunnerExecutor } from "./recipe-executor.js";
import { LocalAssetExecutor, LocalProjectExecutor } from "./project-executor.js";
import { SalExecutor } from "./sal-executor.js";
import { DevelopmentBridgeExecutor } from "./development-bridge.js";
export const MCP_TOOL_NAMES = [
    "ue_status",
    "ue_capabilities",
    "ue_context",
    "ue_skills",
    "ue_cli",
    "ue_blueprint",
    "ue_scene",
    "ue_content",
    "ue_animation",
    "ue_ai",
    "ue_production",
    "ue_workflow",
];
async function withMcpProgress(signal, progressToken, notify, phase, operation) {
    let heartbeat = 0;
    const startedAt = Date.now();
    const timer = progressToken === undefined
        ? undefined
        : setInterval(() => {
            if (signal.aborted)
                return;
            heartbeat += 1;
            void notify({
                method: "notifications/progress",
                params: {
                    progressToken,
                    progress: heartbeat,
                    message: JSON.stringify({
                        phase,
                        heartbeatAt: new Date().toISOString(),
                        lastProgressAt: new Date(startedAt).toISOString(),
                        lastLog: "Waiting for the current safe execution boundary.",
                    }),
                },
            }).catch(() => undefined);
        }, 1_000);
    timer?.unref?.();
    try {
        return await operation();
    }
    finally {
        if (timer !== undefined)
            clearInterval(timer);
    }
}
function resolveLocalCapabilities(catalog, args) {
    if (args.operation) {
        const capability = args.domain
            ? validateDomainOperation(catalog, args.domain, args.operation)
            : catalog.get(args.operation);
        if (!capability) {
            const removed = catalog.removed(args.operation);
            if (removed) {
                throw new UEApiError({
                    code: "capability_removed",
                    message: `Capability "${args.operation}" was removed; use "${removed.replacement}".`,
                    details: removed,
                });
            }
            throw new UEApiError({
                code: "capability_not_found",
                message: `Unknown capability "${args.operation}"`,
            });
        }
        const match = capabilityMatch(capability, args);
        return match === false
            ? []
            : [{ capability, ...(match === undefined ? {} : { match }) }];
    }
    return [...catalog.capabilities]
        .map((capability) => {
        const match = capabilityMatch(capability, args);
        return match === false
            ? undefined
            : { capability, ...(match === undefined ? {} : { match }) };
    })
        .filter((value) => value !== undefined)
        .sort((left, right) => {
        if (args.query?.trim() &&
            left.match !== undefined &&
            right.match !== undefined &&
            left.match.score !== right.match.score) {
            return right.match.score - left.match.score;
        }
        return compareCapabilityIds(left.capability.id, right.capability.id);
    });
}
function capabilityMatch(capability, args) {
    if (!((args.domain === undefined || capability.domain === args.domain) &&
        (args.operation === undefined || capability.id === args.operation) &&
        (args.kind === undefined || capability.kind === args.kind) &&
        (args.lifecycle === undefined || capability.lifecycle.status === args.lifecycle) &&
        (args.operation !== undefined || args.lifecycle !== undefined || capability.lifecycle.canonicalId === capability.id) &&
        (args.canonicalOnly !== true || capability.lifecycle.canonicalId === capability.id) &&
        (args.destructive === undefined ||
            capability.traits.destructive === args.destructive) &&
        (args.expensive === undefined ||
            capability.traits.expensive === args.expensive) &&
        (args.outputKind === undefined ||
            capability.output.kind === args.outputKind) &&
        (args.risk === undefined || capability.dsl?.risk === args.risk))) {
        return false;
    }
    if (args.effect !== undefined) {
        const [field, access] = args.effect.split(":");
        if (capability.effects[field] !== access)
            return false;
    }
    const query = args.query?.trim();
    if (!query) {
        return undefined;
    }
    return matchCapabilitySearch(query, capability) ?? false;
}
function summarizeCapability(ranked) {
    const { capability, match } = ranked;
    return {
        id: capability.id,
        domain: capability.domain,
        kind: capability.kind,
        description: capability.description,
        traits: capability.traits,
        effects: capability.effects,
        lifecycle: capability.lifecycle,
        output: capability.output,
        ...(capability.dsl === undefined ? {} : { risk: capability.dsl.risk }),
        ...(capability.requires === undefined
            ? {}
            : { requires: capability.requires }),
        ...(capability.execution === undefined
            ? {}
            : { execution: capability.execution }),
        ...(match === undefined ? {} : { match }),
    };
}
function fullCapability(ranked) {
    return {
        ...ranked.capability,
        ...(ranked.match === undefined ? {} : { match: ranked.match }),
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
            ? page.map(fullCapability)
            : page.map(summarizeCapability),
    };
}
export async function handleCapabilities(catalog, client, args) {
    try {
        if (!args.live) {
            if (args.availableOnly) {
                throw new UEApiError({
                    code: "editor_required",
                    message: "availableOnly requires live=true because plugin and module availability is Editor-specific.",
                });
            }
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
            effect: args.effect,
            lifecycle: args.lifecycle,
            canonicalOnly: args.canonicalOnly,
            destructive: args.destructive,
            expensive: args.expensive,
            outputKind: args.outputKind,
            risk: args.risk,
            availableOnly: args.availableOnly,
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
    const skillCatalog = options.skillCatalog ?? loadAgentSkillCatalog(catalog);
    const client = options.client ?? ueClient;
    const cliLocator = options.cliLocator ?? locateWorkflowCli;
    const shortCliLocator = options.shortCliLocator ?? locateShortCli;
    const localTraceExecutor = options.localTraceExecutor ??
        new TraceWorkerClient({
            restrictImportRoots: true,
            projectRoot: process.cwd(),
        });
    const domainExecutor = new BackendRoutingExecutor(catalog, client, localTraceExecutor, options.localRecipeExecutor ?? new RecipeRunnerExecutor(), options.localProjectExecutor ?? new LocalProjectExecutor(), options.localAssetExecutor ?? new LocalAssetExecutor(), options.localSalExecutor ?? new SalExecutor(), options.developmentRuntimeExecutor ?? new DevelopmentBridgeExecutor());
    const server = new McpServer({
        name: "ue-ai-integration",
        version: "1.0.0",
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
        effect: z.string().regex(/^(asset|world|editorSession|external):(none|read|write)$/).optional(),
        lifecycle: z.enum(["active", "deprecated"]).optional(),
        canonicalOnly: z.boolean().optional().default(false),
        destructive: z.boolean().optional(),
        expensive: z.boolean().optional(),
        outputKind: z.enum(["json", "image"]).optional(),
        risk: z
            .enum(["readOnly", "safeWrite", "confirmWrite", "notOpen"])
            .optional(),
        availableOnly: z
            .boolean()
            .optional()
            .default(false)
            .describe("With live=true, return only capabilities available in the current Editor."),
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
        effect: args.effect,
        lifecycle: args.lifecycle,
        canonicalOnly: args.canonicalOnly,
        destructive: args.destructive,
        expensive: args.expensive,
        outputKind: args.outputKind,
        risk: args.risk,
        availableOnly: args.availableOnly,
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
        effect: z.string().regex(/^(asset|world|editorSession|external):(none|read|write)$/).optional(),
        lifecycle: z.enum(["active", "deprecated"]).optional(),
        canonicalOnly: z.boolean().optional().default(false),
        destructive: z.boolean().optional(),
        expensive: z.boolean().optional(),
        outputKind: z.enum(["json", "image"]).optional(),
        risk: z
            .enum(["readOnly", "safeWrite", "confirmWrite", "notOpen"])
            .optional(),
        offset: z.number().int().min(0).optional(),
        limit: z.number().int().min(1).max(25).optional(),
    }, async (args) => handleContext(catalog, {
        query: args.query,
        domain: args.domain,
        operation: args.operation,
        kind: args.kind,
        effect: args.effect,
        lifecycle: args.lifecycle,
        canonicalOnly: args.canonicalOnly,
        destructive: args.destructive,
        expensive: args.expensive,
        outputKind: args.outputKind,
        risk: args.risk,
        offset: args.offset,
        limit: args.limit,
    }));
    server.tool("ue_skills", "Load bounded local UE Agent Skill packages and capability recipes. This tool never contacts Editor or executes operations; discover exact schemas with ue_context, then execute through the domain tools or ue_workflow.", {
        action: z
            .enum(["list", "get", "read"])
            .describe("list summaries, get one skill with execution guides, or read one declared reference"),
        query: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Search skill IDs, titles, descriptions, and triggers"),
        domain: z.enum(CAPABILITY_DOMAINS).optional(),
        operation: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Find skills whose recipes use this capability"),
        risk: z
            .enum(["readOnly", "safeWrite", "confirmWrite", "mixed"])
            .optional(),
        skill: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Stable skill ID for get/read"),
        recipe: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Optional recipe ID for get"),
        reference: z
            .string()
            .trim()
            .min(1)
            .optional()
            .describe("Declared relative reference path for read"),
        offset: z.number().int().min(0).optional(),
        limit: z.number().int().min(1).max(50).optional(),
    }, async (args) => handleAgentSkills(skillCatalog, {
        action: args.action,
        query: args.query,
        domain: args.domain,
        operation: args.operation,
        risk: args.risk,
        skill: args.skill,
        recipe: args.recipe,
        reference: args.reference,
        offset: args.offset,
        limit: args.limit,
    }));
    server.tool("ue_cli", "Locate both the ue-workflow-cli DSL CLI and the ue-cli short-operation CLI without contacting Unreal Editor.", {}, async () => {
        try {
            return formatJsonResponse({
                ...cliLocator(),
                shortCli: shortCliLocator(),
            });
        }
        catch (error) {
            return formatErrorResponse(error);
        }
    });
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
        }, async (args, extra) => {
            const generatedRequestId = args.requestId ??
                `mcp-${String(extra.requestId).replace(/[^A-Za-z0-9._:-]/g, "-")}`;
            const progressToken = extra._meta?.progressToken;
            return withMcpProgress(extra.signal, progressToken, (notification) => extra.sendNotification(notification), `execute:${args.operation}`, () => runDomainOperation(catalog, domainExecutor, domain, args.operation, args.params, generatedRequestId, { signal: extra.signal }));
        });
    }
    server.registerTool("ue_workflow", {
        description: "Validate, plan, execute, inspect, resume, or roll back a UE Workflow asset-edit run. The workflow must be passed inline; local file paths are not accepted.",
        inputSchema: UE_WORKFLOW_TOOL_SCHEMA,
    }, async (args, extra) => withMcpProgress(extra.signal, extra._meta?.progressToken, (notification) => extra.sendNotification(notification), `workflow:${String(args.action ?? "unknown")}`, () => handleWorkflowInput(client, args, extra.signal)));
    return {
        server,
        catalog,
        skillCatalog,
        registeredToolNames: MCP_TOOL_NAMES,
    };
}
//# sourceMappingURL=mcp-server.js.map