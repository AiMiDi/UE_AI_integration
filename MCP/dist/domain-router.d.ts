import type { CapabilityCatalog, CapabilityDescriptor, CapabilityDomain } from "./capability-catalog.js";
import { type MCPResponse } from "./helpers.js";
import { type UEExecuteData } from "./ue-bridge.js";
export declare const DOMAIN_TOOL_NAMES: Record<CapabilityDomain, string>;
export declare const DOMAIN_DESCRIPTIONS: Record<CapabilityDomain, string>;
export interface CapabilityExecutor {
    execute(id: string, params?: Record<string, unknown>, requestId?: string): Promise<UEExecuteData>;
}
export declare function isLocalTraceOwnedId(value: unknown): value is string;
export declare class BackendRoutingExecutor implements CapabilityExecutor {
    private readonly catalog;
    private readonly editor;
    private readonly localTrace;
    constructor(catalog: CapabilityCatalog, editor: CapabilityExecutor, localTrace: CapabilityExecutor);
    execute(id: string, params?: Record<string, unknown>, requestId?: string): Promise<UEExecuteData>;
    private unsupported;
}
export declare function validateDomainOperation(catalog: CapabilityCatalog, domain: CapabilityDomain, operation: string): CapabilityDescriptor;
export declare function runDomainOperation(catalog: CapabilityCatalog, executor: CapabilityExecutor, domain: CapabilityDomain, operation: string, params?: Record<string, unknown>, requestId?: string): Promise<MCPResponse>;
