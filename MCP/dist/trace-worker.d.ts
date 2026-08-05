import { spawn } from "node:child_process";
import { type UEExecuteData } from "./ue-bridge.js";
export declare const TRACE_WORKER_PROTOCOL = "ue.trace-worker-request.v1";
export interface TraceWorkerRequest {
    schema: typeof TRACE_WORKER_PROTOCOL;
    action: "handshake" | "execute";
    requestId: string;
    capability?: string;
    params?: Record<string, unknown>;
}
export interface TraceWorkerLocation {
    found: boolean;
    path?: string;
    source?: "environment" | "installed" | "source";
    engineVersion?: string;
    checked: string[];
    error?: string;
}
export interface TraceWorkerClientOptions {
    executable?: string;
    timeoutMs?: number;
    env?: NodeJS.ProcessEnv;
    moduleUrl?: string;
    spawnImpl?: typeof spawn;
    /** Prefix arguments used by tests that launch a script through Node. */
    workerArgs?: string[];
    /** Explicit Engine root/Engine directory; otherwise env/project association is used. */
    engineDirectory?: string;
    transport?: "service" | "stdio";
    serviceEndpoint?: string;
    restrictImportRoots?: boolean;
    projectRoot?: string;
}
export declare function locateTraceWorker(options?: Pick<TraceWorkerClientOptions, "env" | "moduleUrl">): TraceWorkerLocation;
export declare function deriveTraceWorkerEndpoint(executable: string, env?: NodeJS.ProcessEnv, engineVersion?: string, engineDirectory?: string): string;
export declare class TraceWorkerClient {
    readonly timeoutMs: number;
    private readonly executable?;
    private readonly env;
    private readonly moduleUrl?;
    private readonly spawnImpl;
    private readonly workerArgs;
    private readonly configuredEngineDirectory?;
    private readonly transport;
    private readonly configuredEndpoint?;
    private readonly restrictImportRoots;
    private readonly projectRoot;
    private serviceStarting?;
    constructor(options?: TraceWorkerClientOptions);
    location(): TraceWorkerLocation;
    handshake(requireKnownEngine?: boolean, signal?: AbortSignal): Promise<Record<string, unknown>>;
    execute(capability: string, params?: Record<string, unknown>, requestId?: string, context?: {
        signal?: AbortSignal;
    }): Promise<UEExecuteData>;
    private assertMcpImportPath;
    private request;
    private validateHandshake;
    private resolveExecutable;
    private launchArguments;
    private connect;
    private startService;
    private requestService;
    private requestStdio;
    private unwrapEnvelope;
}
