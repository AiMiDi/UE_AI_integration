import { existsSync, readFileSync, readdirSync, realpathSync, statSync } from "node:fs";
import { createConnection } from "node:net";
import { isAbsolute, join, relative, resolve, sep } from "node:path";
import { UEApiError } from "./ue-bridge.js";
const MAX_FRAME_BYTES = 64 * 1024;
const REQUEST_TIMEOUT_MS = 2_000;
function inside(root, candidate) {
    const rel = relative(root, candidate);
    return rel === "" || (!rel.startsWith(`..${sep}`) && rel !== ".." && !isAbsolute(rel));
}
function projectRoot(params) {
    if (typeof params.projectRoot !== "string" || !isAbsolute(params.projectRoot)) {
        throw new UEApiError({ code: "project_root_required", message: "projectRoot must be an explicit absolute project directory." });
    }
    const resolved = resolve(params.projectRoot);
    if (!existsSync(resolved) || !statSync(resolved).isDirectory()) {
        throw new UEApiError({ code: "project_root_invalid", message: "projectRoot must be an existing directory." });
    }
    return realpathSync(resolved);
}
function processAlive(pid) {
    if (!Number.isInteger(pid) || pid <= 0)
        return false;
    try {
        process.kill(pid, 0);
        return true;
    }
    catch (error) {
        return error.code === "EPERM";
    }
}
function discoveryDirectory(root) {
    const directory = resolve(root, "Saved", "UEAI", "DevelopmentBridge");
    if (!inside(root, directory))
        throw new UEApiError({ code: "path_outside_allowed_root", message: "Bridge discovery must stay inside projectRoot." });
    return directory;
}
function parseRecord(path) {
    try {
        const value = JSON.parse(readFileSync(path, "utf8"));
        if (value.schema !== "ue.development-bridge.v1" ||
            typeof value.pid !== "number" ||
            typeof value.processStartTime !== "string" ||
            typeof value.buildId !== "string" ||
            typeof value.projectDigest !== "string" ||
            typeof value.endpoint !== "string" ||
            (value.transport !== "namedPipe" && value.transport !== "unixSocket") ||
            typeof value.pairingToken !== "string" ||
            typeof value.pairingExpiresAtUtc !== "string" ||
            typeof value.observePlanDigest !== "string" ||
            typeof value.controlPlanDigest !== "string" ||
            value.shippingExcluded !== true ||
            value.httpEnabled !== false)
            return undefined;
        return value;
    }
    catch {
        return undefined;
    }
}
function records(root) {
    const directory = discoveryDirectory(root);
    if (!existsSync(directory))
        return [];
    const realDirectory = realpathSync(directory);
    if (!inside(root, realDirectory))
        throw new UEApiError({ code: "path_outside_allowed_root", message: "Bridge discovery directory escapes projectRoot." });
    return readdirSync(realDirectory, { withFileTypes: true })
        .filter((entry) => entry.isFile() && /^\d+\.json$/.test(entry.name))
        .map((entry) => parseRecord(join(realDirectory, entry.name)))
        .filter((record) => record !== undefined && processAlive(record.pid))
        .sort((left, right) => left.pid - right.pid);
}
function recordFor(root, params) {
    const pid = params.pid;
    if (!Number.isInteger(pid) || Number(pid) <= 0)
        throw new UEApiError({ code: "development_target_required", message: "pid must select a discovered Development/DebugGame target." });
    const record = records(root).find((candidate) => candidate.pid === pid);
    if (!record)
        throw new UEApiError({ code: "development_target_unavailable", message: "Target is absent, stale, or no longer opted in." });
    if (typeof params.expectedBuildId === "string" && params.expectedBuildId !== record.buildId) {
        throw new UEApiError({ code: "development_build_mismatch", message: "Target Build ID differs from the attach request." });
    }
    if (typeof params.expectedProjectDigest === "string" && params.expectedProjectDigest !== record.projectDigest) {
        throw new UEApiError({ code: "development_project_mismatch", message: "Target project digest differs from the attach request." });
    }
    return record;
}
function publicTarget(record) {
    return {
        pid: record.pid,
        processStartTime: record.processStartTime,
        buildId: record.buildId,
        projectDigest: record.projectDigest,
        transport: record.transport,
        optedIn: true,
        shippingExcluded: true,
        pairingAvailable: Date.parse(record.pairingExpiresAtUtc) > Date.now(),
        pairingExpiresAtUtc: record.pairingExpiresAtUtc,
    };
}
function plan(record, scope) {
    if (scope !== "observe" && scope !== "control")
        throw new UEApiError({ code: "development_scope_invalid", message: "scope must be observe or control." });
    return {
        schema: "ue.development-attach-plan.v1",
        target: publicTarget(record),
        scope,
        requestedPermissions: scope === "observe" ? ["runtime.status"] : ["runtime.status", "runtime.control"],
        planDigest: scope === "observe" ? record.observePlanDigest : record.controlPlanDigest,
        requires: ["approvePlanDigest", "confirmAttach"],
        expiresAtUtc: record.pairingExpiresAtUtc,
        targetTerminationAllowed: false,
    };
}
function send(record, request, context) {
    return new Promise((resolvePromise, rejectPromise) => {
        const payload = Buffer.from(JSON.stringify(request), "utf8");
        if (payload.length === 0 || payload.length > MAX_FRAME_BYTES) {
            rejectPromise(new UEApiError({ code: "development_request_too_large", message: "Bridge request exceeds 64 KiB." }));
            return;
        }
        const frame = Buffer.allocUnsafe(4 + payload.length);
        frame.writeUInt32LE(payload.length, 0);
        payload.copy(frame, 4);
        const socket = createConnection(record.endpoint);
        let response = Buffer.alloc(0);
        let expected;
        let settled = false;
        const finishError = (error) => {
            if (settled)
                return;
            settled = true;
            cleanup();
            socket.destroy();
            rejectPromise(error);
        };
        const finish = (value) => {
            if (settled)
                return;
            settled = true;
            cleanup();
            socket.end();
            resolvePromise(value);
        };
        const abort = () => finishError(new UEApiError({ code: "request_cancelled", message: "Development Bridge request was cancelled." }));
        const cleanup = () => context?.signal?.removeEventListener("abort", abort);
        context?.signal?.addEventListener("abort", abort, { once: true });
        socket.setTimeout(REQUEST_TIMEOUT_MS);
        socket.once("connect", () => socket.write(frame));
        socket.on("data", (chunk) => {
            response = Buffer.concat([response, chunk], Math.min(4 + MAX_FRAME_BYTES, response.length + chunk.length));
            if (expected === undefined && response.length >= 4) {
                expected = response.readUInt32LE(0);
                if (expected <= 0 || expected > MAX_FRAME_BYTES) {
                    finishError(new UEApiError({ code: "development_bridge_invalid_response", message: "Bridge response frame is invalid." }));
                    return;
                }
            }
            if (expected !== undefined && response.length >= 4 + expected) {
                try {
                    const envelope = JSON.parse(response.subarray(4, 4 + expected).toString("utf8"));
                    if (envelope.ok !== true || !envelope.data) {
                        finishError(new UEApiError({ code: envelope.error?.code ?? "development_bridge_failed", message: envelope.error?.message ?? "Development Bridge rejected the request." }));
                        return;
                    }
                    finish(envelope.data);
                }
                catch (error) {
                    finishError(new UEApiError({ code: "development_bridge_invalid_response", message: error.message }));
                }
            }
        });
        socket.once("timeout", () => finishError(new UEApiError({ code: "development_bridge_timeout", message: "Development Bridge did not respond within two seconds." })));
        socket.once("error", (error) => finishError(new UEApiError({ code: "development_bridge_unavailable", message: error.message })));
        if (context?.signal?.aborted)
            abort();
    });
}
export class DevelopmentBridgeExecutor {
    async execute(id, params = {}, _requestId, context) {
        const root = projectRoot(params);
        if (id === "production.development.target.list") {
            return { schema: "ue.development-targets.v1", targets: records(root).map(publicTarget) };
        }
        const target = recordFor(root, params);
        if (id === "production.development.attach.plan")
            return plan(target, params.scope ?? "observe");
        if (id === "production.development.attach") {
            const scope = params.scope ?? "observe";
            const attachPlan = plan(target, scope);
            if (Date.parse(target.pairingExpiresAtUtc) <= Date.now())
                throw new UEApiError({ code: "pairing_token_expired", message: "The 60-second pairing token has expired; opt in again to create a new target record." });
            if (params.confirmAttach !== true || params.approvePlanDigest !== attachPlan.planDigest) {
                throw new UEApiError({ code: "attach_approval_required", message: "Attach requires the exact approvePlanDigest and confirmAttach=true.", details: { planDigest: attachPlan.planDigest } });
            }
            return send(target, { action: "attach", pairingToken: target.pairingToken, scope, approvePlanDigest: params.approvePlanDigest, confirmAttach: true }, context);
        }
        if (id === "production.development.session.status" || id === "production.development.session.detach") {
            if (typeof params.sessionToken !== "string" || params.sessionToken.length < 16)
                throw new UEApiError({ code: "development_session_required", message: "sessionToken is required." });
            return send(target, { action: id.endsWith("detach") ? "detach" : "status", sessionToken: params.sessionToken }, context);
        }
        throw new UEApiError({ code: "development_operation_unsupported", message: `Unsupported Development Bridge capability ${id}.` });
    }
}
//# sourceMappingURL=development-bridge.js.map