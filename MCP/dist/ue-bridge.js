/**
 * Thin HTTP client for the UE_AI_integration API.
 *
 * The MCP process never launches, owns, or shuts down Unreal Editor. It only
 * connects to an editor that is already running the plugin.
 */
import { randomUUID } from "node:crypto";
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
export const MCP_BRIDGE_NAME = "ue-ai-integration";
export const MCP_BRIDGE_VERSION = "1.0.0";
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
    caller;
    sessionId;
    heartbeatTimer;
    registrationInFlight;
    sessionRequested = false;
    heartbeatIntervalMs = 5_000;
    registrationBackoffIndex = 0;
    constructor(options = {}) {
        this.baseUrl = (options.baseUrl ?? UE_BASE_URL).replace(/\/+$/, "");
        this.timeoutMs = options.timeoutMs ?? REQUEST_TIMEOUT_MS;
        this.fetchImpl = options.fetchImpl ?? fetch;
    }
    async startSession(clientInfo) {
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
        this.scheduleMaintenance(registered
            ? this.heartbeatIntervalMs
            : this.nextRegistrationBackoffMs());
    }
    async stopSession() {
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
            await this.requestOnce("POST", "/api/v1/clients/unregister", {}, sessionId, Math.min(this.timeoutMs, 1_000));
        }
        catch {
            // Editor shutdown or a previously expired session needs no retry.
        }
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
    async execute(id, params = {}, requestId, context) {
        const request = {
            capability: id,
            params,
            ...(requestId === undefined ? {} : { requestId }),
        };
        return this.request("POST", "/api/execute", request, context?.signal, requestId);
    }
    async getWorkflowHandshake() {
        return this.request("GET", "/api/v1/workflow/handshake");
    }
    async workflow(request, signal) {
        return this.request("POST", "/api/v1/workflow", request, signal);
    }
    async request(method, endpoint, body, signal, cancellationRequestId) {
        if (this.sessionRequested &&
            this.caller?.clientKind === "mcp" &&
            this.sessionId === undefined) {
            const registered = await this.tryRegister();
            this.scheduleMaintenance(registered
                ? this.heartbeatIntervalMs
                : this.nextRegistrationBackoffMs());
        }
        try {
            return await this.requestOnce(method, endpoint, body, this.sessionId, this.timeoutMs, signal, cancellationRequestId);
        }
        catch (error) {
            if (error instanceof UEApiError &&
                (error.code === "client_session_expired" ||
                    error.code === "client_session_not_found") &&
                this.caller?.clientKind === "mcp") {
                this.sessionId = undefined;
                const registered = await this.tryRegister();
                this.scheduleMaintenance(registered
                    ? this.heartbeatIntervalMs
                    : this.nextRegistrationBackoffMs());
                return this.requestOnce(method, endpoint, body, this.sessionId, this.timeoutMs, signal, cancellationRequestId);
            }
            throw error;
        }
    }
    async requestOnce(method, endpoint, body, sessionId, requestTimeoutMs = this.timeoutMs, externalSignal, cancellationRequestId) {
        let response;
        try {
            const headers = {};
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
        }
        catch (error) {
            if (externalSignal?.aborted) {
                if (cancellationRequestId !== undefined) {
                    await this.cancelExecution(cancellationRequestId, sessionId);
                }
                throw new UEApiError({
                    code: "request_cancelled",
                    message: "The MCP client cancelled the Unreal execution request.",
                    details: {
                        requestId: cancellationRequestId,
                        cancelPending: true,
                    },
                });
            }
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
    async cancelExecution(requestId, sessionId) {
        try {
            const headers = {
                "Content-Type": "application/json",
            };
            if (sessionId !== undefined) {
                headers["X-UEAI-Session-Id"] = sessionId;
            }
            await this.fetchImpl(`${this.baseUrl}/api/execute/cancel`, {
                method: "POST",
                headers,
                body: JSON.stringify({ requestId }),
                signal: AbortSignal.timeout(1_000),
            });
        }
        catch {
            // Cancellation is best effort after the client has stopped waiting. The
            // Editor still honors its own safe-boundary timeout and journal.
        }
    }
    async tryRegister() {
        if (!this.sessionRequested ||
            this.caller === undefined ||
            this.sessionId !== undefined) {
            return this.sessionId !== undefined;
        }
        if (this.registrationInFlight !== undefined) {
            return this.registrationInFlight;
        }
        this.registrationInFlight = (async () => {
            try {
                const registration = await this.requestOnce("POST", "/api/v1/clients/register", {
                    clientKind: this.caller?.clientKind,
                    name: this.caller?.name,
                    version: this.caller?.version,
                    transport: this.caller?.transport,
                    pid: this.caller?.pid,
                    instanceId: this.caller?.instanceId,
                    invocationId: this.caller?.invocationId,
                    command: this.caller?.command,
                }, undefined, Math.min(this.timeoutMs, 2_000));
                if (typeof registration.sessionId !== "string" ||
                    registration.sessionId.length === 0 ||
                    !Number.isFinite(registration.heartbeatIntervalMs) ||
                    registration.heartbeatIntervalMs <= 0 ||
                    !Number.isFinite(registration.expiresAfterMs) ||
                    registration.expiresAfterMs <= 0) {
                    throw new UEApiError({
                        code: "invalid_client_registration_response",
                        message: "UE client registration returned an invalid session contract.",
                        details: registration,
                    });
                }
                this.sessionId = registration.sessionId;
                this.heartbeatIntervalMs = registration.heartbeatIntervalMs;
                this.registrationBackoffIndex = 0;
                return true;
            }
            catch (error) {
                log.debug("Editor client registration deferred", {
                    error: error.message,
                });
                return false;
            }
        })();
        try {
            return await this.registrationInFlight;
        }
        finally {
            this.registrationInFlight = undefined;
        }
    }
    async maintainSession() {
        if (!this.sessionRequested) {
            return;
        }
        if (this.sessionId === undefined) {
            const registered = await this.tryRegister();
            this.scheduleMaintenance(registered
                ? this.heartbeatIntervalMs
                : this.nextRegistrationBackoffMs());
            return;
        }
        try {
            await this.requestOnce("POST", "/api/v1/clients/heartbeat", {}, this.sessionId, Math.min(this.timeoutMs, 2_000));
            this.registrationBackoffIndex = 0;
            this.scheduleMaintenance(this.heartbeatIntervalMs);
        }
        catch {
            this.sessionId = undefined;
            this.scheduleMaintenance(this.nextRegistrationBackoffMs());
        }
    }
    scheduleMaintenance(delayMs) {
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
    nextRegistrationBackoffMs() {
        const delays = [1_000, 2_000, 5_000, 10_000, 30_000];
        const index = Math.min(this.registrationBackoffIndex, delays.length - 1);
        this.registrationBackoffIndex = Math.min(index + 1, delays.length - 1);
        return delays[index] ?? 30_000;
    }
}
export const ueClient = new UEClient();
//# sourceMappingURL=ue-bridge.js.map