/**
 * Thin HTTP client for the UE_AI_integration API.
 *
 * The MCP process never launches, owns, or shuts down Unreal Editor. It only
 * connects to an editor that is already running the plugin.
 */
function parsePositiveInteger(value, fallback) {
    if (value === undefined) {
        return fallback;
    }
    const parsed = Number.parseInt(value, 10);
    return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback;
}
export const UE_PORT = parsePositiveInteger(process.env.UE_PORT, 9847);
export const UE_BASE_URL = `http://127.0.0.1:${UE_PORT}`;
export const REQUEST_TIMEOUT_MS = parsePositiveInteger(process.env.UE_TIMEOUT_MS, 300_000);
export const log = {
    info: (message, data) => console.error(`[UE_AI_integration] ${message}`, data ? JSON.stringify(data) : ""),
    error: (message, data) => console.error(`[UE_AI_integration:ERROR] ${message}`, data ? JSON.stringify(data) : ""),
    debug: (message, data) => {
        if (process.env.DEBUG) {
            console.error(`[UE_AI_integration:DEBUG] ${message}`, data ? JSON.stringify(data) : "");
        }
    },
};
export const UE_WORKFLOW_ACTIONS = [
    "validate",
    "plan",
    "execute",
    "resume",
    "status",
    "rollback",
];
export class UEApiError extends Error {
    code;
    details;
    status;
    constructor(payload, status) {
        super(payload.message);
        this.name = "UEApiError";
        this.code = payload.code;
        this.details = payload.details;
        this.status = status;
    }
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
function parseErrorPayload(value, fallbackMessage) {
    if (isRecord(value)) {
        const nested = isRecord(value.error) ? value.error : value;
        return {
            code: typeof nested.code === "string" ? nested.code : "transport_error",
            message: typeof nested.message === "string"
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
    baseUrl;
    timeoutMs;
    fetchImpl;
    constructor(options = {}) {
        this.baseUrl = (options.baseUrl ?? UE_BASE_URL).replace(/\/+$/, "");
        this.timeoutMs = options.timeoutMs ?? REQUEST_TIMEOUT_MS;
        this.fetchImpl = options.fetchImpl ?? fetch;
    }
    async getHealth() {
        return this.request("GET", "/api/health");
    }
    async getCapabilities(query = {}) {
        const search = new URLSearchParams();
        for (const [key, value] of Object.entries(query)) {
            if (value !== undefined) {
                search.set(key, String(value));
            }
        }
        const suffix = search.size > 0 ? `?${search.toString()}` : "";
        return this.request("GET", `/api/capabilities${suffix}`);
    }
    async execute(id, params = {}, requestId) {
        const request = {
            capability: id,
            params,
            ...(requestId === undefined ? {} : { requestId }),
        };
        return this.request("POST", "/api/execute", request);
    }
    async getWorkflowHandshake() {
        return this.request("GET", "/api/v1/workflow/handshake");
    }
    async workflow(request) {
        return this.request("POST", "/api/v1/workflow", request);
    }
    async request(method, endpoint, body) {
        let response;
        try {
            response = await this.fetchImpl(`${this.baseUrl}${endpoint}`, {
                method,
                headers: method === "POST" ? { "Content-Type": "application/json" } : undefined,
                body: body === undefined ? undefined : JSON.stringify(body),
                signal: AbortSignal.timeout(this.timeoutMs),
            });
        }
        catch (error) {
            throw new UEApiError({
                code: "editor_unreachable",
                message: `Cannot connect to the running Unreal Editor at ${this.baseUrl}: ${error.message}`,
            });
        }
        let payload;
        try {
            payload = await response.json();
        }
        catch (error) {
            throw new UEApiError({
                code: "invalid_response",
                message: `UE5 ${method} ${endpoint} returned invalid JSON: ${error.message}`,
            }, response.status);
        }
        if (!isRecord(payload) || typeof payload.ok !== "boolean") {
            throw new UEApiError({
                code: "invalid_response",
                message: `UE5 ${method} ${endpoint} returned an invalid API envelope`,
                details: payload,
            }, response.status);
        }
        if (!response.ok || payload.ok === false) {
            throw new UEApiError(parseErrorPayload(payload, `UE5 ${method} ${endpoint} failed with HTTP ${response.status}`), response.status);
        }
        if (!("data" in payload)) {
            throw new UEApiError({
                code: "invalid_response",
                message: `UE5 ${method} ${endpoint} returned no data`,
            }, response.status);
        }
        return payload.data;
    }
}
export const ueClient = new UEClient();
//# sourceMappingURL=ue-bridge.js.map