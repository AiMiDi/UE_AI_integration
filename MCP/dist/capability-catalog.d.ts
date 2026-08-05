export declare const CAPABILITY_DOMAINS: readonly ["blueprint", "scene", "content", "animation", "ai", "production"];
export type CapabilityDomain = (typeof CAPABILITY_DOMAINS)[number];
export type CapabilityKind = "query" | "command" | "validation";
export type CapabilityOutputKind = "json" | "image";
export type CapabilityEffect = "none" | "read" | "write";
export type CapabilityLifecycleStatus = "active" | "deprecated";
export type CapabilityDslAdmission = "editStep" | "finalizer" | "observeOnly" | "interactiveOnly" | "none";
export type CapabilityDslRisk = "readOnly" | "safeWrite" | "confirmWrite" | "notOpen";
export interface CapabilityRequirements {
    features?: string[];
    plugins?: string[];
    modules?: string[];
    platforms?: string[];
    engine?: {
        min?: string;
        maxExclusive?: string;
    };
}
export interface CapabilitySearchMetadata {
    title?: string;
    keywords?: string[];
    aliases?: string[];
}
export type CapabilityExecutionBackend = "editor" | "localTrace" | "localRecipe" | "localProject" | "localAsset" | "localSal" | "developmentRuntime";
export interface CapabilityExecutionMetadata {
    backends: CapabilityExecutionBackend[];
    preferred: CapabilityExecutionBackend;
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
        destructive: boolean;
        expensive: boolean;
    };
    effects: {
        asset: CapabilityEffect;
        world: CapabilityEffect;
        editorSession: CapabilityEffect;
        external: CapabilityEffect;
    };
    lifecycle: {
        status: CapabilityLifecycleStatus;
        since: string;
        canonicalId: string;
        replacement?: string;
    };
    output: {
        kind: CapabilityOutputKind;
    };
    search?: CapabilitySearchMetadata;
    dsl?: CapabilityDslMetadata;
    requires?: CapabilityRequirements;
    execution?: CapabilityExecutionMetadata;
}
export interface CapabilityManifest {
    schemaVersion: 3;
    domain: CapabilityDomain;
    capabilities: CapabilityDescriptor[];
    tombstones: CapabilityTombstone[];
}
export interface CapabilityTombstone {
    id: string;
    removedIn: string;
    replacement: string;
}
export interface CapabilityCatalogSummary {
    schemaVersion: 3;
    capabilityCount: number;
    domainCounts: Record<CapabilityDomain, number>;
}
export declare const DEFAULT_MANIFEST_DIR: string;
export declare class CapabilityManifestError extends Error {
    constructor(message: string);
}
export declare function capabilityIsReadOnly(capability: CapabilityDescriptor): boolean;
export declare class CapabilityCatalog {
    readonly manifests: ReadonlyMap<CapabilityDomain, CapabilityManifest>;
    readonly capabilities: readonly CapabilityDescriptor[];
    readonly tombstones: ReadonlyMap<string, CapabilityTombstone>;
    private readonly byId;
    private readonly byDomain;
    constructor(manifests: CapabilityManifest[]);
    get(id: string): CapabilityDescriptor | undefined;
    removed(id: string): CapabilityTombstone | undefined;
    forDomain(domain: CapabilityDomain): readonly CapabilityDescriptor[];
    summary(): CapabilityCatalogSummary;
}
export declare function loadCapabilityCatalog(manifestDir?: string): CapabilityCatalog;
