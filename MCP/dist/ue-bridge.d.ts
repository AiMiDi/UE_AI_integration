/**
 * Thin HTTP client for the UE_AI_integration API.
 *
 * The MCP process never launches, owns, or shuts down Unreal Editor. It only
 * connects to an editor that is already running the plugin.
 */
import type { CapabilityDescriptor, CapabilityDomain, CapabilityDslRisk, CapabilityKind, CapabilityOutputKind } from "./capability-catalog.js";
import type { CapabilitySearchMatch } from "./capability-search.js";
export declare const UE_PORT: number;
export declare const UE_BASE_URL: string;
export declare const REQUEST_TIMEOUT_MS: number;
export declare const log: {
    info: (message: string, data?: Record<string, unknown>) => void;
    error: (message: string, data?: Record<string, unknown>) => void;
    debug: (message: string, data?: Record<string, unknown>) => void;
};
export interface UEApiErrorPayload {
    code: string;
    message: string;
    details?: unknown;
}
export type UEApiEnvelope<T> = {
    ok: true;
    data: T;
} | {
    ok: false;
    error: UEApiErrorPayload;
};
export interface UEHealthData {
    status: "ready" | "degraded";
    pluginVersion: string;
    engineVersion: string;
    projectName: string;
    mode: "editor";
    capabilityCount: number;
    domainCounts: Record<string, number>;
    validationErrors: unknown[];
    [key: string]: unknown;
}
export interface UECapabilitiesData {
    capabilities: Array<(CapabilityDescriptor & {
        match?: CapabilitySearchMatch;
    }) | (Omit<CapabilityDescriptor, "inputSchema" | "dsl"> & {
        match?: CapabilitySearchMatch;
    })>;
    total: number;
    offset: number;
    limit: number;
    hasMore: boolean;
    detail: "summary" | "full";
    [key: string]: unknown;
}
export interface UECapabilityQuery {
    query?: string;
    domain?: CapabilityDomain;
    operation?: string;
    kind?: CapabilityKind;
    effect?: string;
    lifecycle?: "active" | "deprecated";
    canonicalOnly?: boolean;
    destructive?: boolean;
    expensive?: boolean;
    outputKind?: CapabilityOutputKind;
    risk?: CapabilityDslRisk;
    availableOnly?: boolean;
    offset?: number;
    limit?: number;
    detail?: "summary" | "full";
}
export type UEExecuteData = Record<string, unknown>;
export interface UEExecuteRequest {
    capability: string;
    params: Record<string, unknown>;
    requestId?: string;
}
export declare const UE_WORKFLOW_ACTIONS: readonly ["validate", "plan", "execute", "resume", "status", "rollback"];
export type UEWorkflowAction = (typeof UE_WORKFLOW_ACTIONS)[number];
export interface UEWorkflowRequest {
    action: UEWorkflowAction;
    workflow?: Record<string, unknown>;
    approvePlanDigest?: string;
    runId?: string;
    saveOnSuccess?: boolean;
    confirmWrite?: boolean;
    details?: boolean;
    detailLevel?: "summary" | "standard" | "full";
    sections?: Array<"operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "rollback" | "diagnostics">;
}
export interface UEWorkflowHandshakeData {
    [key: string]: unknown;
}
export type UEWorkflowData = Record<string, unknown>;
export declare class UEApiError extends Error {
    readonly code: string;
    readonly details?: unknown;
    readonly status?: number;
    constructor(payload: UEApiErrorPayload, status?: number);
}
export interface UEClientOptions {
    baseUrl?: string;
    timeoutMs?: number;
    fetchImpl?: typeof fetch;
}
export interface UECallerMetadata {
    clientKind: "mcp" | "cli";
    name: string;
    version?: string;
    transport: string;
    pid: number;
    instanceId: string;
    invocationId?: string;
    command?: string;
}
export declare const MCP_BRIDGE_NAME = "ue-ai-integration";
export declare const MCP_BRIDGE_VERSION = "1.0.0";
export declare class UEClient {
    readonly baseUrl: string;
    readonly timeoutMs: number;
    private readonly fetchImpl;
    private caller?;
    private sessionId?;
    private heartbeatTimer?;
    private registrationInFlight?;
    private sessionRequested;
    private heartbeatIntervalMs;
    private registrationBackoffIndex;
    constructor(options?: UEClientOptions);
    startSession(clientInfo?: {
        name?: string;
        version?: string;
    }): Promise<void>;
    stopSession(): Promise<void>;
    getHealth(): Promise<UEHealthData>;
    getCapabilities(query?: UECapabilityQuery): Promise<UECapabilitiesData>;
    execute(id: string, params?: Record<string, unknown>, requestId?: string, context?: {
        signal?: AbortSignal;
    }): Promise<UEExecuteData>;
    getWorkflowHandshake(): Promise<UEWorkflowHandshakeData>;
    workflow(request: UEWorkflowRequest, signal?: AbortSignal): Promise<UEWorkflowData>;
    private request;
    private requestOnce;
    private cancelExecution;
    private tryRegister;
    private maintainSession;
    private scheduleMaintenance;
    private nextRegistrationBackoffMs;
}
export declare const ueClient: UEClient;
