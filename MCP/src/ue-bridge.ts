/**
 * Thin HTTP client for the UE_AI_integration API.
 *
 * The MCP process never launches, owns, or shuts down Unreal Editor. It only
 * connects to an editor that is already running the plugin.
 */

import type {
  CapabilityDescriptor,
  CapabilityDomain,
  CapabilityDslRisk,
  CapabilityKind,
  CapabilityOutputKind,
} from "./capability-catalog.js";
import type { CapabilitySearchMatch } from "./capability-search.js";
import { randomUUID } from "node:crypto";

function parsePositiveInteger(
  value: string | undefined,
  fallback: number,
): number {
  if (value === undefined) {
    return fallback;
  }
  const parsed = Number.parseInt(value, 10);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}

export const UE_PORT = parsePositiveInteger(process.env.UE_PORT, 9847);
export const UE_BASE_URL = `http://127.0.0.1:${UE_PORT}`;
export const REQUEST_TIMEOUT_MS = parsePositiveInteger(
  process.env.UE_TIMEOUT_MS,
  300_000,
);

export const log = {
  info: (message: string, data?: Record<string, unknown>) =>
    console.error(
      `[UE_AI_integration] ${message}`,
      data ? JSON.stringify(data) : "",
    ),
  error: (message: string, data?: Record<string, unknown>) =>
    console.error(
      `[UE_AI_integration:ERROR] ${message}`,
      data ? JSON.stringify(data) : "",
    ),
  debug: (message: string, data?: Record<string, unknown>) => {
    if (process.env.DEBUG) {
      console.error(
        `[UE_AI_integration:DEBUG] ${message}`,
        data ? JSON.stringify(data) : "",
      );
    }
  },
};

export interface UEApiErrorPayload {
  code: string;
  message: string;
  details?: unknown;
}

export type UEApiEnvelope<T> =
  | {
      ok: true;
      data: T;
    }
  | {
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
  capabilities: Array<
    | (CapabilityDescriptor & { match?: CapabilitySearchMatch })
    | (Omit<CapabilityDescriptor, "inputSchema" | "dsl"> & {
        match?: CapabilitySearchMatch;
      })
  >;
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

export const UE_WORKFLOW_ACTIONS = [
  "validate",
  "plan",
  "execute",
  "resume",
  "status",
  "rollback",
] as const;

export type UEWorkflowAction = (typeof UE_WORKFLOW_ACTIONS)[number];

export interface UEWorkflowRequest {
  action: UEWorkflowAction;
  requestId?: string;
  workflow?: Record<string, unknown>;
  approvePlanDigest?: string;
  runId?: string;
  saveOnSuccess?: boolean;
  confirmWrite?: boolean;
  details?: boolean;
  detailLevel?: "summary" | "standard" | "full";
  sections?: Array<
    | "operations"
    | "finalizers"
    | "readBack"
    | "assetDiff"
    | "structures"
    | "rollback"
    | "diagnostics"
  >;
}

export interface UEWorkflowHandshakeData {
  [key: string]: unknown;
}

export type UEWorkflowData = Record<string, unknown>;

export class UEApiError extends Error {
  readonly code: string;
  readonly details?: unknown;
  readonly status?: number;

  constructor(payload: UEApiErrorPayload, status?: number) {
    super(payload.message);
    this.name = "UEApiError";
    this.code = payload.code;
    this.details = payload.details;
    this.status = status;
  }
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

interface UEClientRegistrationData {
  sessionId: string;
  heartbeatIntervalMs: number;
  expiresAfterMs: number;
}

export const MCP_BRIDGE_NAME = "ue-ai-integration";
export const MCP_BRIDGE_VERSION = "1.0.0";

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function parseErrorPayload(
  value: unknown,
  fallbackMessage: string,
): UEApiErrorPayload {
  if (isRecord(value)) {
    const nested = isRecord(value.error) ? value.error : value;
    return {
      code:
        typeof nested.code === "string" ? nested.code : "transport_error",
      message:
        typeof nested.message === "string"
          ? nested.message
          : fallbackMessage,
      details: nested.details,
    };
  }
  return {
    code: "transport_error",
    message: fallbackMessage,
  };
}

export class UEClient {
  readonly baseUrl: string;
  readonly timeoutMs: number;
  private readonly fetchImpl: typeof fetch;
  private caller?: UECallerMetadata;
  private sessionId?: string;
  private heartbeatTimer?: ReturnType<typeof setTimeout>;
  private registrationInFlight?: Promise<boolean>;
  private sessionRequested = false;
  private heartbeatIntervalMs = 5_000;
  private registrationBackoffIndex = 0;

  constructor(options: UEClientOptions = {}) {
    this.baseUrl = (options.baseUrl ?? UE_BASE_URL).replace(/\/+$/, "");
    this.timeoutMs = options.timeoutMs ?? REQUEST_TIMEOUT_MS;
    this.fetchImpl = options.fetchImpl ?? fetch;
  }

  async startSession(
    clientInfo?: { name?: string; version?: string },
  ): Promise<void> {
    this.sessionRequested = true;
    this.caller = {
      clientKind: "mcp",
      name: clientInfo?.name?.trim() || "Unknown MCP Client",
      version: clientInfo?.version,
      transport: "stdio",
      pid: process.pid,
      instanceId: randomUUID(),
    };
    const registered = await this.tryRegister();
    this.scheduleMaintenance(
      registered
        ? this.heartbeatIntervalMs
        : this.nextRegistrationBackoffMs(),
    );
  }

  async stopSession(): Promise<void> {
    this.sessionRequested = false;
    if (this.heartbeatTimer !== undefined) {
      clearTimeout(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
    await this.registrationInFlight;
    const sessionId = this.sessionId;
    this.sessionId = undefined;
    if (sessionId === undefined) {
      return;
    }
    try {
      await this.requestOnce<Record<string, unknown>>(
        "POST",
        "/api/v1/clients/unregister",
        {},
        sessionId,
        Math.min(this.timeoutMs, 1_000),
      );
    } catch {
      // Editor shutdown or a previously expired session needs no retry.
    }
  }

  async getHealth(): Promise<UEHealthData> {
    return this.request<UEHealthData>("GET", "/api/health");
  }

  async getCapabilities(
    query: UECapabilityQuery = {},
  ): Promise<UECapabilitiesData> {
    const search = new URLSearchParams();
    for (const [key, value] of Object.entries(query)) {
      if (value !== undefined) {
        search.set(key, String(value));
      }
    }
    const suffix = search.size > 0 ? `?${search.toString()}` : "";
    return this.request<UECapabilitiesData>(
      "GET",
      `/api/capabilities${suffix}`,
    );
  }

  async execute(
    id: string,
    params: Record<string, unknown> = {},
    requestId?: string,
    context?: { signal?: AbortSignal },
  ): Promise<UEExecuteData> {
    const request: UEExecuteRequest = {
      capability: id,
      params,
      ...(requestId === undefined ? {} : { requestId }),
    };
    return this.request<UEExecuteData>(
      "POST",
      "/api/execute",
      request,
      context?.signal,
      requestId,
    );
  }

  async getWorkflowHandshake(): Promise<UEWorkflowHandshakeData> {
    return this.request<UEWorkflowHandshakeData>(
      "GET",
      "/api/v1/workflow/handshake",
    );
  }

  async workflow(
    request: UEWorkflowRequest,
    signal?: AbortSignal,
  ): Promise<UEWorkflowData> {
    return this.request<UEWorkflowData>(
      "POST",
      "/api/v1/workflow",
      request,
      signal,
    );
  }

  private async request<T>(
    method: "GET" | "POST",
    endpoint: string,
    body?: object,
    signal?: AbortSignal,
    cancellationRequestId?: string,
  ): Promise<T> {
    if (
      this.sessionRequested &&
      this.caller?.clientKind === "mcp" &&
      this.sessionId === undefined
    ) {
      const registered = await this.tryRegister();
      this.scheduleMaintenance(
        registered
          ? this.heartbeatIntervalMs
          : this.nextRegistrationBackoffMs(),
      );
    }
    try {
      return await this.requestOnce<T>(
        method,
        endpoint,
        body,
        this.sessionId,
        this.timeoutMs,
        signal,
        cancellationRequestId,
      );
    } catch (error) {
      if (
        error instanceof UEApiError &&
        (error.code === "client_session_expired" ||
          error.code === "client_session_not_found") &&
        this.caller?.clientKind === "mcp"
      ) {
        this.sessionId = undefined;
        const registered = await this.tryRegister();
        this.scheduleMaintenance(
          registered
            ? this.heartbeatIntervalMs
            : this.nextRegistrationBackoffMs(),
        );
        return this.requestOnce<T>(
          method,
          endpoint,
          body,
          this.sessionId,
          this.timeoutMs,
          signal,
          cancellationRequestId,
        );
      }
      throw error;
    }
  }

  private async requestOnce<T>(
    method: "GET" | "POST",
    endpoint: string,
    body?: object,
    sessionId?: string,
    requestTimeoutMs: number = this.timeoutMs,
    externalSignal?: AbortSignal,
    cancellationRequestId?: string,
  ): Promise<T> {
    let response: Response;
    try {
      const headers: Record<string, string> = {};
      if (method === "POST") {
        headers["Content-Type"] = "application/json";
      }
      if (this.caller !== undefined) {
        // These compatibility headers are ignored when a session resolves,
        // but retain useful attribution in Legacy HTTP mode.
        headers["X-UEAI-Caller-Type"] = this.caller.clientKind;
        headers["X-UEAI-Caller"] = this.caller.name;
        if (this.caller.version !== undefined) {
          headers["X-UEAI-Caller-Version"] = this.caller.version;
        }
        headers["X-UEAI-Instance-Id"] = this.caller.instanceId;
        if (this.caller.invocationId !== undefined) {
          headers["X-UEAI-Invocation-Id"] = this.caller.invocationId;
        }
        headers["X-UEAI-Process-Id"] = String(this.caller.pid);
        headers["X-UEAI-Transport"] = this.caller.transport;
        if (this.caller.command !== undefined) {
          headers["X-UEAI-Command"] = this.caller.command;
        }
      }
      if (sessionId !== undefined) {
        headers["X-UEAI-Session-Id"] = sessionId;
      }
      const timeoutSignal = AbortSignal.timeout(requestTimeoutMs);
      const signal = externalSignal
        ? AbortSignal.any([externalSignal, timeoutSignal])
        : timeoutSignal;
      response = await this.fetchImpl(`${this.baseUrl}${endpoint}`, {
        method,
        headers: Object.keys(headers).length > 0 ? headers : undefined,
        body: body === undefined ? undefined : JSON.stringify(body),
        signal,
      });
    } catch (error) {
      if (externalSignal?.aborted) {
        let cancelAck: Record<string, unknown> = {
          requestId: cancellationRequestId,
          cancelPending: false,
          state: "cancelAckUnavailable",
        };
        if (cancellationRequestId !== undefined) {
          cancelAck = await this.cancelExecution(cancellationRequestId, sessionId);
        }
        throw new UEApiError({
          code: "request_cancelled",
          message: "The MCP client cancelled the Unreal execution request.",
          details: cancelAck,
        });
      }
      throw new UEApiError({
        code: "editor_unreachable",
        message: `Cannot connect to the running Unreal Editor at ${this.baseUrl}: ${(error as Error).message}`,
      });
    }

    let payload: unknown;
    try {
      payload = await response.json();
    } catch (error) {
      throw new UEApiError(
        {
          code: "invalid_response",
          message: `UE5 ${method} ${endpoint} returned invalid JSON: ${(error as Error).message}`,
        },
        response.status,
      );
    }

    if (!isRecord(payload) || typeof payload.ok !== "boolean") {
      throw new UEApiError(
        {
          code: "invalid_response",
          message: `UE5 ${method} ${endpoint} returned an invalid API envelope`,
          details: payload,
        },
        response.status,
      );
    }

    if (!response.ok || payload.ok === false) {
      throw new UEApiError(
        parseErrorPayload(
          payload,
          `UE5 ${method} ${endpoint} failed with HTTP ${response.status}`,
        ),
        response.status,
      );
    }

    if (!("data" in payload)) {
      throw new UEApiError(
        {
          code: "invalid_response",
          message: `UE5 ${method} ${endpoint} returned no data`,
        },
        response.status,
      );
    }

    return payload.data as T;
  }

  private async cancelExecution(
    requestId: string,
    sessionId?: string,
  ): Promise<Record<string, unknown>> {
    try {
      const headers: Record<string, string> = {
        "Content-Type": "application/json",
      };
      if (sessionId !== undefined) {
        headers["X-UEAI-Session-Id"] = sessionId;
      }
      const response = await this.fetchImpl(`${this.baseUrl}/api/execute/cancel`, {
        method: "POST",
        headers,
        body: JSON.stringify({ requestId }),
        signal: AbortSignal.timeout(1_000),
      });
      const payload = await response.json() as unknown;
      if (isRecord(payload) && isRecord(payload.data)) return payload.data;
    } catch {
      // Cancellation is best effort after the client has stopped waiting. The
      // Editor still honors its own safe-boundary timeout and journal.
    }
    return { requestId, cancelPending: false, state: "cancelAckUnavailable" };
  }

  private async tryRegister(): Promise<boolean> {
    if (
      !this.sessionRequested ||
      this.caller === undefined ||
      this.sessionId !== undefined
    ) {
      return this.sessionId !== undefined;
    }
    if (this.registrationInFlight !== undefined) {
      return this.registrationInFlight;
    }
    this.registrationInFlight = (async () => {
      try {
        const registration =
          await this.requestOnce<UEClientRegistrationData>(
            "POST",
            "/api/v1/clients/register",
            {
              clientKind: this.caller?.clientKind,
              name: this.caller?.name,
              version: this.caller?.version,
              transport: this.caller?.transport,
              pid: this.caller?.pid,
              instanceId: this.caller?.instanceId,
              invocationId: this.caller?.invocationId,
              command: this.caller?.command,
            },
            undefined,
            Math.min(this.timeoutMs, 2_000),
          );
        if (
          typeof registration.sessionId !== "string" ||
          registration.sessionId.length === 0 ||
          !Number.isFinite(registration.heartbeatIntervalMs) ||
          registration.heartbeatIntervalMs <= 0 ||
          !Number.isFinite(registration.expiresAfterMs) ||
          registration.expiresAfterMs <= 0
        ) {
          throw new UEApiError({
            code: "invalid_client_registration_response",
            message:
              "UE client registration returned an invalid session contract.",
            details: registration,
          });
        }
        this.sessionId = registration.sessionId;
        this.heartbeatIntervalMs = registration.heartbeatIntervalMs;
        this.registrationBackoffIndex = 0;
        return true;
      } catch (error) {
        log.debug("Editor client registration deferred", {
          error: (error as Error).message,
        });
        return false;
      }
    })();
    try {
      return await this.registrationInFlight;
    } finally {
      this.registrationInFlight = undefined;
    }
  }

  private async maintainSession(): Promise<void> {
    if (!this.sessionRequested) {
      return;
    }
    if (this.sessionId === undefined) {
      const registered = await this.tryRegister();
      this.scheduleMaintenance(
        registered
          ? this.heartbeatIntervalMs
          : this.nextRegistrationBackoffMs(),
      );
      return;
    }
    try {
      await this.requestOnce<Record<string, unknown>>(
        "POST",
        "/api/v1/clients/heartbeat",
        {},
        this.sessionId,
        Math.min(this.timeoutMs, 2_000),
      );
      this.registrationBackoffIndex = 0;
      this.scheduleMaintenance(this.heartbeatIntervalMs);
    } catch {
      this.sessionId = undefined;
      this.scheduleMaintenance(this.nextRegistrationBackoffMs());
    }
  }

  private scheduleMaintenance(delayMs: number): void {
    if (!this.sessionRequested) {
      return;
    }
    if (this.heartbeatTimer !== undefined) {
      clearTimeout(this.heartbeatTimer);
    }
    this.heartbeatTimer = setTimeout(() => {
      this.heartbeatTimer = undefined;
      void this.maintainSession();
    }, delayMs);
    this.heartbeatTimer.unref?.();
  }

  private nextRegistrationBackoffMs(): number {
    const delays = [1_000, 2_000, 5_000, 10_000, 30_000] as const;
    const index = Math.min(
      this.registrationBackoffIndex,
      delays.length - 1,
    );
    this.registrationBackoffIndex = Math.min(
      index + 1,
      delays.length - 1,
    );
    return delays[index] ?? 30_000;
  }
}

export const ueClient = new UEClient();
