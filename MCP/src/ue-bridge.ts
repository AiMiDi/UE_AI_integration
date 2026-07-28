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
    CapabilityDescriptor | Omit<CapabilityDescriptor, "inputSchema" | "dsl">
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
  readOnly?: boolean;
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

  constructor(options: UEClientOptions = {}) {
    this.baseUrl = (options.baseUrl ?? UE_BASE_URL).replace(/\/+$/, "");
    this.timeoutMs = options.timeoutMs ?? REQUEST_TIMEOUT_MS;
    this.fetchImpl = options.fetchImpl ?? fetch;
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
  ): Promise<UEExecuteData> {
    const request: UEExecuteRequest = {
      capability: id,
      params,
      ...(requestId === undefined ? {} : { requestId }),
    };
    return this.request<UEExecuteData>("POST", "/api/execute", request);
  }

  async getWorkflowHandshake(): Promise<UEWorkflowHandshakeData> {
    return this.request<UEWorkflowHandshakeData>(
      "GET",
      "/api/v1/workflow/handshake",
    );
  }

  async workflow(request: UEWorkflowRequest): Promise<UEWorkflowData> {
    return this.request<UEWorkflowData>(
      "POST",
      "/api/v1/workflow",
      request,
    );
  }

  private async request<T>(
    method: "GET" | "POST",
    endpoint: string,
    body?: object,
  ): Promise<T> {
    let response: Response;
    try {
      response = await this.fetchImpl(`${this.baseUrl}${endpoint}`, {
        method,
        headers:
          method === "POST" ? { "Content-Type": "application/json" } : undefined,
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: AbortSignal.timeout(this.timeoutMs),
      });
    } catch (error) {
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
}

export const ueClient = new UEClient();
