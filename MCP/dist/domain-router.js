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