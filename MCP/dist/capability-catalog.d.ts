export declare const CAPABILITY_DOMAINS: readonly ["blueprint", "scene", "content", "animation", "ai", "production"];
export type CapabilityDomain = (typeof CAPABILITY_DOMAINS)[number];
export type CapabilityKind = "query" | "command" | "validation";
export type CapabilityOutputKind = "json" | "image";
export type CapabilityDslAdmission = "editStep" | "finalizer" | "observeOnly" | "interactiveOnly" | "none";
export type CapabilityDslRisk = "readOnly" | "safeWrite" | "confirmWrite" | "notOpen";
export interface CapabilityRequirements {
    plugins?: string[];
    modules?: string[];
    platforms?: string[];
    engine?: {
        min?: string;
        maxExclusive?: string;
    };
}
export interface CapabilityInputSchema {
    type: "object";
    properties: Record<string, unknown>;
    additionalProperties: false;
    required?: string[];
    [key: string]: unknown;
}
export interface CapabilityDslMetadata {
    admission: CapabilityDslAdmission;
    scopeKinds: string[];
    transactionDomain: string;
    deferCompile: boolean;
    risk: CapabilityDslRisk;
}
export interface CapabilityDescriptor {
    id: string;
    domain: CapabilityDomain;
    kind: CapabilityKind;
    description: string;
    inputSchema: CapabilityInputSchema;
    traits: {
        readOnly: boolean;
        destructive: boolean;
        expensive: boolean;
    };
    output: {
        kind: CapabilityOutputKind;
    };
    dsl?: CapabilityDslMetadata;
    requires?: CapabilityRequirements;
}
export interface CapabilityManifest {
    schemaVersion: 2;
    domain: CapabilityDomain;
    capabilities: CapabilityDescriptor[];
}
export interface CapabilityCatalogSummary {
    schemaVersion: 2;
    capabilityCount: number;
    domainCounts: Record<CapabilityDomain, number>;
}
export declare const DEFAULT_MANIFEST_DIR: string;
export declare class CapabilityManifestError extends Error {
    constructor(message: string);
}
export declare class CapabilityCatalog {
    readonly manifests: ReadonlyMap<CapabilityDomain, CapabilityManifest>;
    readonly capabilities: readonly CapabilityDescriptor[];
    private readonly byId;
    private readonly byDomain;
    constructor(manifests: CapabilityManifest[]);
    get(id: string): CapabilityDescriptor | undefined;
    forDomain(domain: CapabilityDomain): readonly CapabilityDescriptor[];
    summary(): CapabilityCatalogSummary;
}
export declare function loadCapabilityCatalog(manifestDir?: string): CapabilityCatalog;
