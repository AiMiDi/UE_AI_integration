import { formatCapabilityResponse, formatErrorResponse, } from "./helpers.js";
import { UEApiError } from "./ue-bridge.js";
export const DOMAIN_TOOL_NAMES = {
    blueprint: "ue_blueprint",
    scene: "ue_scene",
    content: "ue_content",
    animation: "ue_animation",
    ai: "ue_ai",
    production: "ue_production",
};
export const DOMAIN_DESCRIPTIONS = {
    blueprint: "Execute Blueprint inspection, authoring, mutation, and validation capabilities.",
    scene: "Execute level, actor, component, world-building, viewport, and navigation capabilities.",
    content: "Execute asset, material, DataTable, Niagara, UI, and content-management capabilities.",
    animation: "Execute animation asset, AnimBlueprint, state-machine, and BlendSpace capabilities.",
    ai: "Execute Behavior Tree, Blackboard, and related AI authoring capabilities.",
    production: "Execute Sequencer, build, packaging, diagnostics, and production workflow capabilities.",
};
const LOCAL_TRACE_ID_PREFIXES = [
    "trace-local-",
    "trace-analysis-local-",
    "trace-launch-local-",
];
export function isLocalTraceOwnedId(value) {
    return (typeof value === "string" &&
        LOCAL_TRACE_ID_PREFIXES.some((prefix) => value.startsWith(prefix)));
}
function idBoundBackend(capability, params) {
    const id = params.traceId ?? params.jobId ?? params.analysisId;
    if (typeof id !== "string" || id.length === 0)
        return undefined;
    if (capability.startsWith("production.trace.") ||
        capability.startsWith("production.job.")) {
        return isLocalTraceOwnedId(id) ? "localTrace" : "editor";
    }
    return undefined;
}
function targetBoundBackend(capability, params) {
    if (capability === "production.trace.start") {
        const target = params.target;
        if (typeof target === "object" && target !== null) {
            const kind = target.kind;
            if (kind === "development")
                return "localTrace";
            if (kind === "editor" || kind === "pie")
                return "editor";
        }
        // The backward-compatible target-less start records the current Editor.
        return "editor";
    }
    if (capability === "production.trace.channel.list") {
        if (params.targetKind === "development")
            return "localTrace";
        if (params.targetKind === "editor" || params.targetKind === "pie") {
            return "editor";
        }
    }
    return undefined;
}
export class BackendRoutingExecutor {
    catalog;
    editor;
    localTrace;
    constructor(catalog, editor, localTrace) {
        this.catalog = catalog;
        this.editor = editor;
        this.localTrace = localTrace;
    }
    async execute(id, params = {}, requestId) {
        const capability = this.catalog.get(id);
        const execution = capability?.execution;
        const requested = params.backend ?? "auto";
        if (requested !== "auto" &&
            requested !== "editor" &&
            requested !== "local") {
            throw new UEApiError({
                code: "invalid_execution_backend",
                message: "backend must be auto, editor, or local.",
                details: { capability: id, backend: requested },
            });
        }
        const forced = targetBoundBackend(id, params) ?? idBoundBackend(id, params);
        if (forced !== undefined &&
            requested !== "auto" &&
            (requested === "local" ? "localTrace" : requested) !== forced) {
            throw new UEApiError({
                code: "execution_backend_conflict",
                message: `backend "${requested}" conflicts with the target or owning ID for capability "${id}".`,
                details: { capability: id, backend: requested, required: forced },
            });
        }
        const dynamicJobRoute = forced !== undefined && id.startsWith("production.job.");
        if (execution === undefined && !dynamicJobRoute && forced === undefined) {
            return this.editor.execute(id, params, requestId);
        }
        const declared = execution?.backends ?? ["editor", "localTrace"];
        const supportsEditor = declared.includes("editor");
        const supportsLocal = declared.includes("localTrace");
        if (forced === "editor") {
            if (!supportsEditor)
                throw this.unsupported(id, "editor", declared);
            return this.editor.execute(id, params, requestId);
        }
        if (forced === "localTrace") {
            if (!supportsLocal)
                throw this.unsupported(id, "local", declared);
            return this.localTrace.execute(id, params, requestId);
        }
        if (requested === "editor") {
            if (!supportsEditor) {
                throw this.unsupported(id, "editor", declared);
            }
            return this.editor.execute(id, params, requestId);
        }
        if (requested === "local") {
            if (!supportsLocal) {
                throw this.unsupported(id, "local", declared);
            }
            return this.localTrace.execute(id, params, requestId);
        }
        if (execution?.preferred === "localTrace" && supportsLocal) {
            try {
                return await this.localTrace.execute(id, params, requestId);
            }
            catch (error) {
                if (!supportsEditor ||
                    !(error instanceof UEApiError) ||
                    error.code !== "trace_worker_unavailable") {
                    throw error;
                }
                return this.editor.execute(id, params, requestId);
            }
        }
        if (execution?.preferred === "editor" && supportsEditor) {
            try {
                return await this.editor.execute(id, params, requestId);
            }
            catch (error) {
                if (!supportsLocal ||
                    !(error instanceof UEApiError) ||
                    error.code !== "editor_unreachable") {
                    throw error;
                }
                return this.localTrace.execute(id, params, requestId);
            }
        }
        if (supportsEditor) {
            return this.editor.execute(id, params, requestId);
        }
        if (supportsLocal) {
            return this.localTrace.execute(id, params, requestId);
        }
        throw new UEApiError({
            code: "execution_backend_unavailable",
            message: `Capability "${id}" declares no usable execution backend.`,
        });
    }
    unsupported(capability, requested, declared) {
        return new UEApiError({
            code: "execution_backend_unsupported",
            message: `Capability "${capability}" does not support backend "${requested}".`,
            details: { capability, requested, declared },
        });
    }
}
export function validateDomainOperation(catalog, domain, operation) {
    const capability = catalog.get(operation);
    if (!capability) {
        throw new UEApiError({
            code: "capability_not_found",
            message: `Unknown capability "${operation}"`,
            details: {
                requestedDomain: domain,
            },
        });
    }
    if (capability.domain !== domain) {
        throw new UEApiError({
            code: "cross_domain_operation",
            message: `Capability "${operation}" belongs to domain "${capability.domain}", not "${domain}"`,
            details: {
                requestedDomain: domain,
                actualDomain: capability.domain,
            },
        });
    }
    return capability;
}
export async function runDomainOperation(catalog, executor, domain, operation, params = {}, requestId) {
    try {
        const capability = validateDomainOperation(catalog, domain, operation);
        const data = await executor.execute(capability.id, params, requestId);
        return formatCapabilityResponse(capability, data);
    }
    catch (error) {
        return formatErrorResponse(error);
    }
}
//# sourceMappingURL=domain-router.js.map