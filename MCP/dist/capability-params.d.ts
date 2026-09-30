import type { CapabilityDescriptor } from "./capability-catalog.js";
type JsonObject = Record<string, unknown>;
export interface ParameterIssue {
    path: string;
    code: string;
    message: string;
    expected?: unknown;
    actual?: unknown;
}
export interface CapabilityParameterTemplate {
    capability: string;
    schemaSource: "local-manifest";
    schemaDigest: string;
    params: JsonObject;
    required: string[];
    optional: string[];
    unresolved: string[];
    approval: {
        required: boolean;
        fields: string[];
        guidance: string;
    };
    request: {
        acceptsRequestId: boolean;
        generatedByCli: boolean;
    };
    persistence: {
        fields: string[];
        guidance: string;
    };
    retry: {
        safeToRetry: boolean;
        guidance: string;
    };
}
export interface CapabilityParameterPreflight {
    capability: string;
    schemaSource: "local-manifest";
    schemaDigest: string;
    valid: boolean;
    safeToProceed: boolean;
    errors: ParameterIssue[];
    warnings: ParameterIssue[];
    approval: CapabilityParameterTemplate["approval"];
    request: CapabilityParameterTemplate["request"];
    persistence: CapabilityParameterTemplate["persistence"];
    retry: CapabilityParameterTemplate["retry"];
    nextAction: string;
    helpCommand: string;
    safeToRetry: boolean;
}
export declare function createParameterTemplate(capability: CapabilityDescriptor): CapabilityParameterTemplate;
export declare function preflightParameters(capability: CapabilityDescriptor, params: unknown): CapabilityParameterPreflight;
export {};
