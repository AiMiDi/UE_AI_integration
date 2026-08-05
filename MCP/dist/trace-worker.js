import { createHash, randomUUID } from "node:crypto";
import { existsSync, readFileSync, readdirSync, realpathSync, statSync, } from "node:fs";
import { spawn, spawnSync } from "node:child_process";
import { createConnection } from "node:net";
import { basename, dirname, isAbsolute, relative, resolve, sep, } from "node:path";
import { fileURLToPath } from "node:url";
import { MCP_BRIDGE_VERSION, UEApiError, } from "./ue-bridge.js";
export const TRACE_WORKER_PROTOCOL = "ue.trace-worker-request.v1";
const TRACE_WORKER_RESPONSE = "ue.trace-worker-response.v1";
const TRACE_WORKER_HANDSHAKE = "ue.trace-worker-handshake.v1";
const TRACE_WORKER_PROTOCOL_VERSION = 1;
const MAX_FRAME_BYTES = 4 * 1024 * 1024;
const STARTUP_TIMEOUT_MS = 10_000;
function engineMajorMinor(value) {
    const match = /^(\d+)\.(\d+)(?:$|[.\-+])/.exec(value);
    return match ? [Number(match[1]), Number(match[2])] : undefined;
}
function validateEngineDirectory(candidate, expectedVersion, source) {
    let engineDirectory = resolve(candidate);
    if (basename(engineDirectory).toLowerCase() !== "engine" &&
        existsSync(resolve(engineDirectory, "Engine"))) {
        engineDirectory = resolve(engineDirectory, "Engine");
    }
    try {
        engineDirectory = realpathSync.native(engineDirectory);
        const buildVersion = JSON.parse(readFileSync(resolve(engineDirectory, "Build", "Build.version"), "utf8"));
        const expected = engineMajorMinor(expectedVersion);
        if (expected === undefined ||
            !Number.isInteger(buildVersion.MajorVersion) ||
            !Number.isInteger(buildVersion.MinorVersion)) {
            return { source, error: `${source} has an invalid Engine Build.version.` };
        }
        if (buildVersion.MajorVersion !== expected[0] ||
            buildVersion.MinorVersion !== expected[1]) {
            return {
                source,
                error: `${source} Engine ${String(buildVersion.MajorVersion)}.${String(buildVersion.MinorVersion)} does not match Worker ${expectedVersion}.`,
            };
        }
        const insights = process.platform === "darwin"
            ? resolve(engineDirectory, "Binaries", "Mac", "UnrealInsights.app", "Contents", "MacOS", "UnrealInsights")
            : resolve(engineDirectory, "Binaries", platformDirectory(), process.platform === "win32" ? "UnrealInsights.exe" : "UnrealInsights");
        if (!existsSync(insights) || !statSync(insights).isFile()) {
            return { source, error: `${source} has no matching Unreal Insights executable.` };
        }
        return { path: engineDirectory, source };
    }
    catch {
        return { source, error: `${source} has no readable Engine/Build/Build.version.` };
    }
}
function queryWindowsEngineAssociation(association) {
    if (process.platform !== "win32" || association.length === 0 || association.length > 64) {
        return undefined;
    }
    const isVersion = /^\d+\.\d+$/.test(association);
    const isGuid = /^\{[0-9a-fA-F-]+\}$/.test(association);
    if (!isVersion && !isGuid)
        return undefined;
    const key = isVersion
        ? `HKLM\\SOFTWARE\\EpicGames\\Unreal Engine\\${association}`
        : "HKCU\\Software\\Epic Games\\Unreal Engine\\Builds";
    const value = isVersion ? "InstalledDirectory" : association;
    const result = spawnSync("reg.exe", ["query", key, "/v", value], {
        encoding: "utf8",
        windowsHide: true,
    });
    if (result.status !== 0)
        return undefined;
    for (const line of result.stdout.split(/\r?\n/)) {
        const match = /\sREG_SZ\s+(.+?)\s*$/.exec(line);
        if (match?.[1])
            return match[1];
    }
    return undefined;
}
function projectAssociatedEngine(executable) {
    let directory = dirname(executable);
    for (let depth = 0; depth < 12; depth += 1) {
        let projects = [];
        try {
            projects = readdirSync(directory, { withFileTypes: true })
                .filter((entry) => entry.isFile() && entry.name.endsWith(".uproject"))
                .map((entry) => resolve(directory, entry.name));
        }
        catch {
            projects = [];
        }
        if (projects.length > 1) {
            return {
                source: "projectAssociation",
                error: "Multiple .uproject files were found beside the installed Worker.",
            };
        }
        if (projects.length === 1) {
            try {
                if (statSync(projects[0]).size > 1024 * 1024)
                    throw new Error("oversized");
                const project = JSON.parse(readFileSync(projects[0], "utf8"));
                const association = typeof project.EngineAssociation === "string"
                    ? project.EngineAssociation
                    : "";
                if (!association)
                    return {};
                const engineRoot = queryWindowsEngineAssociation(association);
                return engineRoot
                    ? { path: engineRoot, source: "projectAssociation" }
                    : {
                        source: "projectAssociation",
                        error: "The project's exact EngineAssociation is not registered.",
                    };
            }
            catch {
                return {
                    source: "projectAssociation",
                    error: "The installed project's .uproject could not be parsed.",
                };
            }
        }
        const parent = dirname(directory);
        if (parent === directory)
            break;
        directory = parent;
    }
    return {};
}
function resolveTraceEngineDirectory(executable, expectedVersion, env, explicit) {
    if (explicit) {
        return validateEngineDirectory(explicit, expectedVersion, "argument");
    }
    if (env.UEAI_ENGINE_ROOT) {
        return validateEngineDirectory(env.UEAI_ENGINE_ROOT, expectedVersion, "UEAI_ENGINE_ROOT");
    }
    if (env.UE_ENGINE_ROOT) {
        return validateEngineDirectory(env.UE_ENGINE_ROOT, expectedVersion, "UE_ENGINE_ROOT");
    }
    const associated = projectAssociatedEngine(executable);
    return associated.path
        ? validateEngineDirectory(associated.path, expectedVersion, "projectAssociation")
        : associated;
}
function platformDirectory() {
    if (process.platform === "win32")
        return "Win64";
    if (process.platform === "darwin")
        return "Mac";
    return "Linux";
}
function executableName() {
    return process.platform === "win32"
        ? "UEAITraceWorker.exe"
        : "UEAITraceWorker";
}
function addCandidate(checked, candidate) {
    const normalized = resolve(candidate);
    if (!checked.includes(normalized))
        checked.push(normalized);
    try {
        return existsSync(normalized) && statSync(normalized).isFile()
            ? normalized
            : undefined;
    }
    catch {
        return undefined;
    }
}
function inferEngineVersion(executable) {
    const parent = dirname(executable).split(/[\\/]/).at(-1) ?? "";
    return /^\d+\.\d+/.test(parent) ? parent : "unknown";
}
export function locateTraceWorker(options = {}) {
    const env = options.env ?? process.env;
    const checked = [];
    if (env.UEAI_TRACE_WORKER) {
        const found = addCandidate(checked, env.UEAI_TRACE_WORKER);
        return found
            ? {
                found: true,
                path: found,
                source: "environment",
                engineVersion: env.UE_ENGINE_VERSION ?? env.UE_VERSION ?? inferEngineVersion(found),
                checked,
            }
            : {
                found: false,
                checked,
                error: "UEAI_TRACE_WORKER does not name a readable executable.",
            };
    }
    const modulePath = fileURLToPath(options.moduleUrl ?? import.meta.url);
    const packageRoot = resolve(dirname(modulePath), "..", "..");
    const traceRoot = resolve(packageRoot, "Tools", "Trace", platformDirectory());
    const selectedVersion = env.UE_ENGINE_VERSION ?? env.UE_VERSION;
    if (selectedVersion) {
        const found = addCandidate(checked, resolve(traceRoot, selectedVersion, executableName()));
        if (found) {
            return {
                found: true,
                path: found,
                source: "installed",
                engineVersion: selectedVersion,
                checked,
            };
        }
    }
    else if (existsSync(traceRoot)) {
        const matches = readdirSync(traceRoot, { withFileTypes: true })
            .filter((entry) => entry.isDirectory())
            .map((entry) => addCandidate(checked, resolve(traceRoot, entry.name, executableName())))
            .filter((value) => value !== undefined)
            .sort();
        if (matches.length === 1) {
            return {
                found: true,
                path: matches[0],
                source: "installed",
                engineVersion: inferEngineVersion(matches[0]),
                checked,
            };
        }
        if (matches.length > 1) {
            return {
                found: false,
                checked,
                error: "Multiple engine-specific Trace Workers are installed; set UE_ENGINE_VERSION.",
            };
        }
    }
    // build_trace_worker.ps1 uses this source-local staging layout for real
    // UE/TraceServices validation. Prefer its engine-versioned output over the
    // unversioned Programs/Binaries location, which may contain a stale build
    // from a different Engine checkout.
    const sourceStageRoot = resolve(packageRoot, "Intermediate", "TraceWorkerStage", "Tools", "Trace", platformDirectory());
    if (selectedVersion) {
        const found = addCandidate(checked, resolve(sourceStageRoot, selectedVersion, executableName()));
        if (found) {
            return {
                found: true,
                path: found,
                source: "source",
                engineVersion: selectedVersion,
                checked,
            };
        }
    }
    else if (existsSync(sourceStageRoot)) {
        const matches = readdirSync(sourceStageRoot, { withFileTypes: true })
            .filter((entry) => entry.isDirectory())
            .map((entry) => addCandidate(checked, resolve(sourceStageRoot, entry.name, executableName())))
            .filter((value) => value !== undefined)
            .sort();
        if (matches.length === 1) {
            return {
                found: true,
                path: matches[0],
                source: "source",
                engineVersion: inferEngineVersion(matches[0]),
                checked,
            };
        }
        if (matches.length > 1) {
            return {
                found: false,
                checked,
                error: "Multiple source-built Trace Workers were found; set UE_ENGINE_VERSION.",
            };
        }
    }
    // Programs/Binaries has no Engine version component. Keep it only as a
    // source-tree fallback after every matching staged Worker candidate.
    const programWorker = addCandidate(checked, resolve(packageRoot, "Programs", "UEAITraceWorker", "Binaries", platformDirectory(), executableName()));
    if (programWorker) {
        return {
            found: true,
            path: programWorker,
            source: "source",
            engineVersion: selectedVersion ?? "unknown",
            checked,
        };
    }
    for (const parts of [
        ["build", "Tools", "Trace", executableName()],
        ["build-workflow", "Tools", "Trace", executableName()],
        ["build-workflow-final", "Tools", "Trace", executableName()],
    ]) {
        const found = addCandidate(checked, resolve(packageRoot, ...parts));
        if (found) {
            return {
                found: true,
                path: found,
                source: "source",
                engineVersion: selectedVersion ?? "unknown",
                checked,
            };
        }
    }
    return {
        found: false,
        checked,
        error: "UEAITraceWorker was not found. Set UEAI_TRACE_WORKER or install the worker for this Engine version.",
    };
}
function userIdentity(env) {
    if (process.platform === "win32") {
        return (env.USERNAME ?? "unknown-user").toLowerCase();
    }
    return String(process.getuid?.() ?? 0);
}
function fnv1a64(value) {
    let hash = 0xcbf29ce484222325n;
    const prime = 0x100000001b3n;
    for (const byte of Buffer.from(value, "utf8")) {
        hash ^= BigInt(byte);
        hash = BigInt.asUintN(64, hash * prime);
    }
    return hash.toString(16).padStart(16, "0");
}
export function deriveTraceWorkerEndpoint(executable, env = process.env, engineVersion = env.UE_ENGINE_VERSION ?? env.UE_VERSION ?? inferEngineVersion(executable), engineDirectory = env.UEAI_ENGINE_ROOT ?? env.UE_ENGINE_ROOT ?? "unresolved-engine-dir") {
    let worker = realpathSync.native(executable).replaceAll("\\", "/");
    if (process.platform === "win32")
        worker = worker.toLowerCase();
    const user = userIdentity(env);
    let normalizedEngine = engineDirectory === "unresolved-engine-dir"
        ? engineDirectory
        : resolve(engineDirectory).replaceAll("\\", "/");
    if (process.platform === "win32")
        normalizedEngine = normalizedEngine.toLowerCase();
    const token = fnv1a64(`${worker}|${engineVersion}|${normalizedEngine}|${MCP_BRIDGE_VERSION}|${user}`);
    if (process.platform === "win32") {
        return `\\\\.\\pipe\\UEAITraceWorker-${token}`;
    }
    const directory = env.XDG_RUNTIME_DIR ?? env.TMPDIR ?? "/tmp";
    return resolve(directory, `ueai-trace-worker-${user}-${token}.sock`);
}
function isRecord(value) {
    return typeof value === "object" && value !== null && !Array.isArray(value);
}
function retryableConnectError(error) {
    const code = error?.code;
    return code === "ENOENT" || code === "ECONNREFUSED" || code === "EBUSY";
}
function delay(milliseconds) {
    return new Promise((resolveDelay) => {
        setTimeout(resolveDelay, milliseconds);
    });
}
function canonicalPath(path) {
    const canonical = realpathSync.native(path);
    return process.platform === "win32" ? canonical.toLowerCase() : canonical;
}
function rootContains(root, child) {
    const difference = relative(root, child);
    return (difference === "" ||
        (difference !== ".." &&
            !difference.startsWith(`..${sep}`) &&
            !isAbsolute(difference)));
}
function findTraceResourceRoot(worker) {
    let current = dirname(worker);
    for (let depth = 0; depth < 12; depth += 1) {
        if (existsSync(resolve(current, "Resources", "Capabilities", "production.json")) &&
            existsSync(resolve(current, "Resources", "Trace", "worker-protocol.v1.json"))) {
            return current;
        }
        const parent = dirname(current);
        if (parent === current)
            break;
        current = parent;
    }
    return undefined;
}
const MAX_TRACE_CONTRACT_FILE_BYTES = 16 * 1024 * 1024;
const INSIGHTS_QUERY_CAPABILITIES = [
    "production.trace.bookmark.query",
    "production.trace.context_switches.query",
    "production.trace.counter.query",
    "production.trace.io.query",
    "production.trace.loading.query",
    "production.trace.log.query",
    "production.trace.memory.query",
    "production.trace.network.query",
    "production.trace.region.query",
    "production.trace.screenshot.query",
    "production.trace.tasks.query",
    "production.trace.timing.query",
];
function stringArray(value) {
    return (Array.isArray(value) &&
        value.every((entry) => typeof entry === "string" && entry.length > 0));
}
function exactStringSet(value, expected) {
    return (stringArray(value) &&
        value.length === expected.length &&
        [...value].sort().every((entry, index) => entry === [...expected].sort()[index]));
}
function readBoundedContractFile(root, relativePath) {
    const canonicalRoot = canonicalPath(root);
    const requested = resolve(root, relativePath);
    const canonicalFile = canonicalPath(requested);
    if (!rootContains(canonicalRoot, canonicalFile)) {
        throw new Error(`${relativePath} resolves outside the Trace resource root.`);
    }
    const status = statSync(canonicalFile);
    if (!status.isFile() || status.size <= 0 || status.size > MAX_TRACE_CONTRACT_FILE_BYTES) {
        throw new Error(`${relativePath} must be a non-empty regular file no larger than 16 MiB.`);
    }
    return readFileSync(canonicalFile);
}
function parseContractJson(relativePath, contents) {
    let parsed;
    try {
        parsed = JSON.parse(contents.toString("utf8"));
    }
    catch (error) {
        throw new Error(`${relativePath} is not valid UTF-8 JSON: ${error.message}`);
    }
    if (!isRecord(parsed)) {
        throw new Error(`${relativePath} must contain one JSON object.`);
    }
    return parsed;
}
function validateWorkerProtocol(protocol) {
    if (protocol.schema !== "ue.trace-worker-protocol.v1" ||
        protocol.protocolVersion !== TRACE_WORKER_PROTOCOL_VERSION ||
        protocol.requestSchema !== TRACE_WORKER_PROTOCOL ||
        protocol.responseSchema !== TRACE_WORKER_RESPONSE ||
        !exactStringSet(protocol.actions, ["execute", "handshake"])) {
        throw new Error("worker-protocol.v1.json does not describe the bounded v1 protocol.");
    }
    const frame = isRecord(protocol.frame) ? protocol.frame : undefined;
    if (frame === undefined ||
        frame.encoding !== "utf-8-json" ||
        frame.lengthPrefix !== "uint32-little-endian" ||
        frame.maximumBytes !== MAX_FRAME_BYTES ||
        frame.requestsPerConnection !== 1) {
        throw new Error("worker-protocol.v1.json has an incompatible frame contract.");
    }
    const resident = isRecord(protocol.residentService)
        ? protocol.residentService
        : undefined;
    if (resident === undefined ||
        resident.windows !== "current-user-named-pipe" ||
        resident.unix !== "current-user-unix-domain-socket" ||
        resident.tcp !== false ||
        resident.defaultIdleSeconds !== 600 ||
        resident.maximumConcurrentConnections !== 2 ||
        resident.analysisSessionCacheCapacity !== 2 ||
        resident.analysisSessionPolicy !== "sha256-lru" ||
        resident.maximumConcurrentAnalyses !== 1) {
        throw new Error("worker-protocol.v1.json has an incompatible resident service contract.");
    }
}
function validateLaunchProfiles(profiles) {
    if (profiles.schema !== "ue.trace-launch-profiles.v1" ||
        !Array.isArray(profiles.profiles) ||
        profiles.profiles.length === 0 ||
        profiles.profiles.length > 32) {
        throw new Error("launch-profiles.json has an invalid profile catalog.");
    }
    const identifiers = new Set();
    for (const candidate of profiles.profiles) {
        if (!isRecord(candidate)) {
            throw new Error("launch-profiles.json contains a non-object profile.");
        }
        const id = candidate.id;
        if (typeof id !== "string" ||
            !/^[A-Za-z][A-Za-z0-9._-]{0,63}$/.test(id) ||
            identifiers.has(id) ||
            typeof candidate.executableKind !== "string" ||
            !stringArray(candidate.configurations) ||
            !candidate.configurations.every((configuration) => configuration === "Development" || configuration === "DebugGame") ||
            !stringArray(candidate.allowedMaps) ||
            !stringArray(candidate.fixedArguments) ||
            !stringArray(candidate.allowedCvars) ||
            typeof candidate.maxDurationSeconds !== "number" ||
            candidate.maxDurationSeconds <= 0 ||
            typeof candidate.maxFileSizeMiB !== "number" ||
            candidate.maxFileSizeMiB <= 0 ||
            typeof candidate.startupTimeoutSeconds !== "number" ||
            candidate.startupTimeoutSeconds <= 0 ||
            typeof candidate.shutdownTimeoutSeconds !== "number" ||
            candidate.shutdownTimeoutSeconds <= 0 ||
            typeof candidate.allowForcedTermination !== "boolean") {
            throw new Error(`launch-profiles.json profile ${String(id)} is invalid.`);
        }
        identifiers.add(id);
    }
}
function productionCapabilities(production) {
    if (production.schemaVersion !== 3 ||
        production.domain !== "production" ||
        !Array.isArray(production.capabilities) ||
        production.capabilities.length === 0) {
        throw new Error("production.json is not a production capability manifest v3.");
    }
    const capabilities = new Map();
    for (const candidate of production.capabilities) {
        if (!isRecord(candidate) || typeof candidate.id !== "string") {
            throw new Error("production.json contains a capability without a valid id.");
        }
        if (capabilities.has(candidate.id)) {
            throw new Error(`production.json contains duplicate capability ${candidate.id}.`);
        }
        capabilities.set(candidate.id, candidate);
    }
    return capabilities;
}
function validateInsightsMapping(mapping, majorMinor, capabilities) {
    if (mapping.schema !== "ue.trace-insights-actions.v1" ||
        mapping.engineVersion !== majorMinor ||
        !Array.isArray(mapping.panels) ||
        mapping.panels.length === 0 ||
        mapping.panels.length > 64) {
        throw new Error(`insights-actions.${majorMinor}.json does not match Engine ${majorMinor}.`);
    }
    const panelIds = new Set();
    const mappedCapabilities = new Set();
    for (const candidate of mapping.panels) {
        if (!isRecord(candidate) ||
            typeof candidate.id !== "string" ||
            candidate.id.length === 0 ||
            panelIds.has(candidate.id) ||
            typeof candidate.provider !== "string" ||
            candidate.provider.length === 0 ||
            typeof candidate.capability !== "string" ||
            mappedCapabilities.has(candidate.capability) ||
            !stringArray(candidate.operations) ||
            new Set(candidate.operations).size !== candidate.operations.length) {
            throw new Error(`insights-actions.${majorMinor}.json contains an invalid panel mapping.`);
        }
        const descriptor = capabilities.get(candidate.capability);
        const execution = descriptor && isRecord(descriptor.execution)
            ? descriptor.execution
            : undefined;
        const inputSchema = descriptor && isRecord(descriptor.inputSchema)
            ? descriptor.inputSchema
            : undefined;
        const properties = inputSchema && isRecord(inputSchema.properties)
            ? inputSchema.properties
            : undefined;
        const operationSchema = properties && isRecord(properties.operation)
            ? properties.operation
            : undefined;
        const operationEnum = operationSchema?.enum;
        if (descriptor === undefined ||
            execution === undefined ||
            !stringArray(execution.backends) ||
            !execution.backends.includes("localTrace") ||
            !stringArray(operationEnum) ||
            new Set(operationEnum).size !== operationEnum.length ||
            !exactStringSet(candidate.operations, operationEnum)) {
            throw new Error(`${candidate.capability} is not an exact localTrace operation mapping from production.json.`);
        }
        panelIds.add(candidate.id);
        mappedCapabilities.add(candidate.capability);
    }
    if (!exactStringSet([...mappedCapabilities], INSIGHTS_QUERY_CAPABILITIES)) {
        throw new Error(`insights-actions.${majorMinor}.json must cover exactly the twelve public Trace query capabilities.`);
    }
}
function loadTraceContractEvidence(root, majorMinor) {
    const relativeFiles = [
        "Resources/Capabilities/production.json",
        `Resources/Trace/insights-actions.${majorMinor}.json`,
        "Resources/Trace/launch-profiles.json",
        "Resources/Trace/worker-protocol.v1.json",
    ].sort();
    const contents = new Map();
    const fileDigests = {};
    for (const relativePath of relativeFiles) {
        const value = readBoundedContractFile(root, relativePath);
        contents.set(relativePath, value);
        fileDigests[relativePath] =
            `sha256:${createHash("sha256").update(value).digest("hex")}`;
    }
    const productionPath = "Resources/Capabilities/production.json";
    const mappingPath = `Resources/Trace/insights-actions.${majorMinor}.json`;
    const launchPath = "Resources/Trace/launch-profiles.json";
    const protocolPath = "Resources/Trace/worker-protocol.v1.json";
    const production = parseContractJson(productionPath, contents.get(productionPath));
    const capabilities = productionCapabilities(production);
    validateWorkerProtocol(parseContractJson(protocolPath, contents.get(protocolPath)));
    validateLaunchProfiles(parseContractJson(launchPath, contents.get(launchPath)));
    validateInsightsMapping(parseContractJson(mappingPath, contents.get(mappingPath)), majorMinor, capabilities);
    const contract = createHash("sha256");
    for (const relativePath of relativeFiles) {
        contract.update(relativePath.replaceAll("\\", "/"), "utf8");
        contract.update(Buffer.from([0]));
        contract.update(contents.get(relativePath));
        contract.update(Buffer.from([0]));
    }
    return {
        root: canonicalPath(root),
        contractDigest: `sha256:${contract.digest("hex")}`,
        providerSchemaDigest: fileDigests[mappingPath],
        fileDigests,
    };
}
function traceContractDigests(worker, engineVersion, moduleUrl) {
    const majorMinor = /^(\d+\.\d+)/.exec(engineVersion)?.[1];
    if (majorMinor === undefined) {
        return {
            ok: false,
            reason: "The Trace Worker Engine version cannot select a versioned Insights mapping.",
            details: { worker, engineVersion },
        };
    }
    const modulePath = fileURLToPath(moduleUrl ?? import.meta.url);
    const trustedRoot = findTraceResourceRoot(modulePath);
    const workerRoot = findTraceResourceRoot(worker);
    if (trustedRoot === undefined || workerRoot === undefined) {
        return {
            ok: false,
            reason: "Trace Worker execution requires both MCP-owned and Worker-bundled contract resources.",
            details: {
                worker,
                engineVersion: majorMinor,
                trustedResourceRoot: trustedRoot ?? null,
                workerResourceRoot: workerRoot ?? null,
            },
        };
    }
    try {
        const trusted = loadTraceContractEvidence(trustedRoot, majorMinor);
        const bundled = canonicalPath(workerRoot) === canonicalPath(trustedRoot)
            ? trusted
            : loadTraceContractEvidence(workerRoot, majorMinor);
        if (bundled.contractDigest !== trusted.contractDigest ||
            bundled.providerSchemaDigest !== trusted.providerSchemaDigest) {
            return {
                ok: false,
                reason: "Worker-bundled Trace resources do not match the MCP-owned resources.",
                details: {
                    worker,
                    engineVersion: majorMinor,
                    trusted,
                    bundled,
                },
            };
        }
        return {
            ok: true,
            evidence: trusted,
        };
    }
    catch (error) {
        return {
            ok: false,
            reason: "Trace contract resources are missing, invalid, or outside their trusted root.",
            details: {
                worker,
                engineVersion: majorMinor,
                trustedResourceRoot: trustedRoot,
                workerResourceRoot: workerRoot,
                error: error.message,
            },
        };
    }
}
export class TraceWorkerClient {
    timeoutMs;
    executable;
    env;
    moduleUrl;
    spawnImpl;
    workerArgs;
    configuredEngineDirectory;
    transport;
    configuredEndpoint;
    restrictImportRoots;
    projectRoot;
    serviceStarting;
    constructor(options = {}) {
        this.executable = options.executable;
        this.timeoutMs = options.timeoutMs ?? 300_000;
        this.env = options.env ?? process.env;
        this.moduleUrl = options.moduleUrl;
        this.spawnImpl = options.spawnImpl ?? spawn;
        this.workerArgs = options.workerArgs ?? [];
        this.configuredEngineDirectory = options.engineDirectory;
        this.transport = options.transport ?? "service";
        this.configuredEndpoint = options.serviceEndpoint;
        this.restrictImportRoots = options.restrictImportRoots ?? false;
        this.projectRoot = resolve(options.projectRoot ?? process.cwd());
    }
    location() {
        if (this.executable !== undefined) {
            const checked = [];
            const found = addCandidate(checked, this.executable);
            return found
                ? {
                    found: true,
                    path: found,
                    source: "environment",
                    engineVersion: this.env.UE_ENGINE_VERSION ??
                        this.env.UE_VERSION ??
                        inferEngineVersion(found),
                    checked,
                }
                : { found: false, checked, error: "Configured Trace Worker is missing." };
        }
        return locateTraceWorker({ env: this.env, moduleUrl: this.moduleUrl });
    }
    async handshake(requireKnownEngine = false, signal) {
        const requestId = randomUUID();
        const data = await this.request({
            schema: TRACE_WORKER_PROTOCOL,
            action: "handshake",
            requestId,
        }, signal);
        this.validateHandshake(data, requireKnownEngine);
        return data;
    }
    async execute(capability, params = {}, requestId, context) {
        await this.handshake(true, context?.signal);
        let effectiveParams = params;
        if (this.restrictImportRoots && capability === "production.trace.import") {
            effectiveParams = {
                ...params,
                tracePath: await this.assertMcpImportPath(params.tracePath, context?.signal),
            };
        }
        return this.request({
            schema: TRACE_WORKER_PROTOCOL,
            action: "execute",
            requestId: requestId ?? randomUUID(),
            capability,
            params: effectiveParams,
        }, context?.signal);
    }
    async assertMcpImportPath(tracePath, signal) {
        if (typeof tracePath !== "string" || tracePath.length === 0) {
            throw new UEApiError({
                code: "trace_path_required",
                message: "production.trace.import requires tracePath.",
            });
        }
        let source;
        try {
            source = canonicalPath(tracePath);
        }
        catch {
            throw new UEApiError({
                code: "trace_file_not_found",
                message: "The Trace import file does not exist or cannot be resolved.",
            });
        }
        const handshake = await this.handshake(true, signal);
        const roots = [resolve(this.projectRoot, "Saved")];
        if (typeof handshake.storeRoot === "string" && handshake.storeRoot) {
            roots.push(resolve(handshake.storeRoot));
        }
        for (const configured of (this.env.UEAI_TRACE_ROOTS ?? "")
            .split(";")
            .filter(Boolean)) {
            roots.push(resolve(configured));
        }
        const canonicalRoots = roots.map((root) => {
            try {
                return canonicalPath(root);
            }
            catch {
                return process.platform === "win32" ? root.toLowerCase() : root;
            }
        });
        if (!canonicalRoots.some((root) => rootContains(root, source))) {
            throw new UEApiError({
                code: "trace_path_not_allowed",
                message: "MCP Trace imports are limited to the project Saved directory, the Worker store, and UEAI_TRACE_ROOTS.",
                details: { tracePath: source, allowedRoots: canonicalRoots },
            });
        }
        return source;
    }
    async request(request, signal) {
        if (signal?.aborted) {
            throw new UEApiError({
                code: "request_cancelled",
                message: "The MCP client cancelled the local Worker request.",
                details: { requestId: request.requestId, cancelPending: false },
            });
        }
        const encoded = Buffer.from(JSON.stringify(request), "utf8");
        if (encoded.byteLength === 0 || encoded.byteLength > MAX_FRAME_BYTES) {
            throw new UEApiError({
                code: "trace_worker_request_too_large",
                message: "Trace Worker request must be between 1 byte and 4 MiB.",
            });
        }
        return this.transport === "stdio"
            ? this.requestStdio(encoded, request.requestId, signal)
            : this.requestService(encoded, request.requestId, signal);
    }
    validateHandshake(data, requireKnownEngine) {
        const location = this.resolveExecutable();
        const expectedEngine = location.engineVersion;
        const actualEngine = data.engineVersion;
        const digestPattern = /^sha256:[0-9a-f]{64}$/;
        const mismatch = (message, details) => {
            throw new UEApiError({
                code: "trace_worker_contract_mismatch",
                message,
                details,
            });
        };
        if (data.schema !== TRACE_WORKER_HANDSHAKE ||
            data.protocolVersion !== TRACE_WORKER_PROTOCOL_VERSION ||
            data.workerVersion !== MCP_BRIDGE_VERSION ||
            data.contractBound !== true ||
            typeof data.contractDigest !== "string" ||
            !digestPattern.test(data.contractDigest) ||
            typeof data.providerSchemaDigest !== "string" ||
            !digestPattern.test(data.providerSchemaDigest) ||
            typeof data.maximumResidentSessions !== "number" ||
            data.maximumResidentSessions < 1 ||
            data.maximumResidentSessions > 2 ||
            data.transport !==
                (this.transport === "stdio"
                    ? "stdio-one-shot"
                    : process.platform === "win32"
                        ? "named-pipe"
                        : "unix-socket")) {
            mismatch("Trace Worker handshake does not match the bounded v1 contract.", {
                handshake: data,
            });
        }
        if (typeof actualEngine !== "string" || actualEngine.length === 0) {
            mismatch("Trace Worker did not report its Engine version.");
        }
        const actualEngineVersion = actualEngine;
        if (expectedEngine === "unknown") {
            if (requireKnownEngine) {
                mismatch("The Trace Worker Engine version is ambiguous; set UE_ENGINE_VERSION before local execution.", { worker: location.path, actualEngine: actualEngineVersion });
            }
        }
        else if (actualEngineVersion !== expectedEngine &&
            !actualEngineVersion.startsWith(`${expectedEngine}.`) &&
            !actualEngineVersion.startsWith(`${expectedEngine}-`)) {
            mismatch("Trace Worker Engine version does not match the selected Worker.", {
                expectedEngine,
                actualEngine: actualEngineVersion,
            });
        }
        const expected = traceContractDigests(location.path, expectedEngine === "unknown" ? actualEngineVersion : expectedEngine, this.moduleUrl);
        if (!expected.ok) {
            return mismatch(expected.reason, expected.details);
        }
        if (data.contractDigest !== expected.evidence.contractDigest ||
            data.providerSchemaDigest !== expected.evidence.providerSchemaDigest) {
            mismatch("Trace Worker contract digest does not match the installed resources.", {
                expected: expected.evidence,
                actual: {
                    contractDigest: data.contractDigest,
                    providerSchemaDigest: data.providerSchemaDigest,
                },
            });
        }
    }
    resolveExecutable() {
        const location = this.location();
        if (!location.found || location.path === undefined) {
            throw new UEApiError({
                code: "trace_worker_unavailable",
                message: location.error ?? "Trace Worker is unavailable.",
                details: { checked: location.checked },
            });
        }
        const explicitArgument = this.workerArgs
            .find((argument) => /^-{1,2}EngineDir=/.test(argument))
            ?.replace(/^-{1,2}EngineDir=/, "");
        const engine = resolveTraceEngineDirectory(location.path, location.engineVersion ?? inferEngineVersion(location.path), this.env, explicitArgument ?? this.configuredEngineDirectory);
        if (engine.error) {
            throw new UEApiError({
                code: "trace_worker_engine_mismatch",
                message: engine.error,
                details: { source: engine.source, worker: location.path },
            });
        }
        return {
            path: location.path,
            engineVersion: location.engineVersion ?? inferEngineVersion(location.path),
            engineDirectory: engine.path,
            checked: location.checked,
        };
    }
    launchArguments(engineDirectory) {
        const hasEngineArgument = this.workerArgs.some((argument) => /^-{1,2}EngineDir=/.test(argument));
        return [
            ...this.workerArgs,
            "-NoLog",
            "-NoDefaultLog",
            "-SaveToUserDir",
            ...(engineDirectory && !hasEngineArgument
                ? [`-EngineDir=${engineDirectory}`]
                : []),
        ];
    }
    connect(endpoint) {
        return new Promise((resolveSocket, reject) => {
            const socket = createConnection(endpoint);
            let settled = false;
            const timer = setTimeout(() => {
                if (settled)
                    return;
                settled = true;
                socket.destroy();
                const error = new Error("Trace Worker endpoint connect timed out.");
                error.code = "ETIMEDOUT";
                reject(error);
            }, 250);
            timer.unref?.();
            socket.once("connect", () => {
                if (settled)
                    return;
                settled = true;
                clearTimeout(timer);
                socket.removeAllListeners("error");
                resolveSocket(socket);
            });
            socket.once("error", (error) => {
                if (settled)
                    return;
                settled = true;
                clearTimeout(timer);
                socket.destroy();
                reject(error);
            });
        });
    }
    startService(executable, endpoint, engineDirectory) {
        if (this.serviceStarting)
            return this.serviceStarting;
        this.serviceStarting = new Promise((resolveChild, reject) => {
            const child = this.spawnImpl(executable, [
                ...this.launchArguments(engineDirectory),
                "--serve",
                `--endpoint=${endpoint}`,
                "--idleSeconds=600",
                "--maxSessions=2",
            ], {
                stdio: "ignore",
                windowsHide: true,
                detached: true,
                env: this.env,
            });
            child.once("spawn", () => {
                child.unref();
                child.once("exit", () => {
                    this.serviceStarting = undefined;
                });
                resolveChild(child);
            });
            child.once("error", (error) => {
                this.serviceStarting = undefined;
                reject(new UEApiError({
                    code: "trace_worker_unavailable",
                    message: `Trace Worker service could not start: ${error.message}`,
                }));
            });
        });
        return this.serviceStarting;
    }
    async requestService(encoded, requestId, signal) {
        const { path: executable, engineVersion, engineDirectory } = this.resolveExecutable();
        const endpoint = this.configuredEndpoint ??
            deriveTraceWorkerEndpoint(executable, this.env, engineVersion, engineDirectory);
        const startedAt = Date.now();
        const startupDeadline = startedAt + Math.min(this.timeoutMs, STARTUP_TIMEOUT_MS);
        let socket;
        let spawned;
        try {
            socket = await this.connect(endpoint);
        }
        catch (error) {
            if (!retryableConnectError(error)) {
                throw new UEApiError({
                    code: "trace_worker_unavailable",
                    message: `Trace Worker endpoint could not be opened: ${error.message}`,
                });
            }
            spawned = await this.startService(executable, endpoint, engineDirectory);
        }
        while (!socket && Date.now() < startupDeadline) {
            if (signal?.aborted) {
                throw new UEApiError({
                    code: "request_cancelled",
                    message: "The MCP client cancelled while the Trace Worker service was starting.",
                    details: { requestId, cancelPending: false },
                });
            }
            if (spawned?.exitCode !== null && spawned?.exitCode !== undefined) {
                throw new UEApiError({
                    code: "trace_worker_crashed",
                    message: "Trace Worker exited before its service endpoint became ready.",
                    details: { exitCode: spawned.exitCode },
                });
            }
            await delay(50);
            try {
                socket = await this.connect(endpoint);
            }
            catch (error) {
                if (!retryableConnectError(error)) {
                    throw new UEApiError({
                        code: "trace_worker_unavailable",
                        message: `Trace Worker endpoint could not be opened: ${error.message}`,
                    });
                }
            }
        }
        if (!socket) {
            throw new UEApiError({
                code: "trace_worker_service_start_timeout",
                message: `Trace Worker endpoint was not ready within ${Math.min(this.timeoutMs, STARTUP_TIMEOUT_MS)} ms.`,
            });
        }
        return new Promise((resolveResponse, reject) => {
            let settled = false;
            let buffer = Buffer.alloc(0);
            let expected;
            const remaining = Math.max(1, this.timeoutMs - (Date.now() - startedAt));
            const timer = setTimeout(() => {
                if (settled)
                    return;
                settled = true;
                socket?.destroy();
                reject(new UEApiError({
                    code: "trace_worker_timeout",
                    message: `Trace Worker exceeded ${this.timeoutMs} ms.`,
                }));
            }, remaining);
            timer.unref?.();
            const onAbort = () => {
                if (settled)
                    return;
                settled = true;
                clearTimeout(timer);
                socket?.destroy();
                reject(new UEApiError({
                    code: "request_cancelled",
                    message: "The MCP client cancelled the local Worker request.",
                    details: { requestId, cancelPending: true },
                }));
            };
            signal?.addEventListener("abort", onAbort, { once: true });
            const clearAbort = () => signal?.removeEventListener("abort", onAbort);
            const fail = (error) => {
                if (settled)
                    return;
                settled = true;
                clearTimeout(timer);
                clearAbort();
                socket?.destroy();
                reject(error);
            };
            socket.on("data", (chunk) => {
                if (settled)
                    return;
                buffer = Buffer.concat([buffer, chunk]);
                if (expected === undefined && buffer.length >= 4) {
                    expected = buffer.readUInt32LE(0);
                    if (expected === 0 || expected > MAX_FRAME_BYTES) {
                        fail(new UEApiError({
                            code: "trace_worker_invalid_response",
                            message: "Trace Worker returned an invalid frame length.",
                        }));
                        return;
                    }
                }
                if (expected !== undefined && buffer.length >= expected + 4) {
                    let envelope;
                    try {
                        envelope = JSON.parse(buffer.subarray(4, expected + 4).toString("utf8"));
                    }
                    catch (error) {
                        fail(new UEApiError({
                            code: "trace_worker_invalid_response",
                            message: `Trace Worker returned invalid JSON: ${error.message}`,
                        }));
                        return;
                    }
                    try {
                        const data = this.unwrapEnvelope(envelope, 0, requestId);
                        settled = true;
                        clearTimeout(timer);
                        clearAbort();
                        socket?.end();
                        resolveResponse(data);
                    }
                    catch (error) {
                        fail(error);
                    }
                }
            });
            socket.once("error", (error) => {
                fail(new UEApiError({
                    code: "trace_worker_crashed",
                    message: `Trace Worker service connection failed: ${error.message}`,
                }));
            });
            socket.once("close", () => {
                if (!settled) {
                    fail(new UEApiError({
                        code: "trace_worker_crashed",
                        message: "Trace Worker service closed before returning a complete frame.",
                    }));
                }
            });
            const header = Buffer.allocUnsafe(4);
            header.writeUInt32LE(encoded.byteLength, 0);
            socket.write(Buffer.concat([header, encoded]));
        });
    }
    requestStdio(encoded, requestId, signal) {
        const { path: executable, engineDirectory } = this.resolveExecutable();
        return new Promise((resolvePromise, reject) => {
            const child = this.spawnImpl(executable, [...this.launchArguments(engineDirectory), "--stdio"], {
                stdio: ["pipe", "pipe", "pipe"],
                windowsHide: true,
                env: this.env,
            });
            let stdout = "";
            let stderr = "";
            let settled = false;
            const finishError = (error) => {
                if (settled)
                    return;
                settled = true;
                clearTimeout(timer);
                signal?.removeEventListener("abort", onAbort);
                reject(error);
            };
            const onAbort = () => {
                if (settled)
                    return;
                child.kill();
                finishError(new UEApiError({
                    code: "request_cancelled",
                    message: "The MCP client cancelled the local Worker process.",
                    details: { requestId, cancelPending: false },
                }));
            };
            const timer = setTimeout(() => {
                child.kill();
                finishError(new UEApiError({
                    code: "trace_worker_timeout",
                    message: `Trace Worker exceeded ${this.timeoutMs} ms.`,
                }));
            }, this.timeoutMs);
            timer.unref?.();
            signal?.addEventListener("abort", onAbort, { once: true });
            child.stdout?.setEncoding("utf8");
            child.stderr?.setEncoding("utf8");
            child.stdout?.on("data", (chunk) => {
                stdout += chunk;
            });
            child.stderr?.on("data", (chunk) => {
                stderr += chunk;
            });
            child.once("error", (error) => {
                finishError(new UEApiError({
                    code: "trace_worker_unavailable",
                    message: `Trace Worker could not start: ${error.message}`,
                }));
            });
            child.once("close", (code) => {
                if (settled)
                    return;
                clearTimeout(timer);
                let envelope;
                try {
                    envelope = JSON.parse(stdout.trim());
                    const data = this.unwrapEnvelope(envelope, code ?? 1, requestId);
                    settled = true;
                    signal?.removeEventListener("abort", onAbort);
                    resolvePromise(data);
                }
                catch (error) {
                    if (error instanceof UEApiError) {
                        finishError(error);
                    }
                    else {
                        finishError(new UEApiError({
                            code: "trace_worker_invalid_response",
                            message: `Trace Worker returned invalid JSON: ${error.message}`,
                            details: { exitCode: code, stderr: stderr.slice(0, 8192) },
                        }));
                    }
                }
            });
            child.stdin?.end(encoded, "utf8");
        });
    }
    unwrapEnvelope(envelope, exitCode, requestId) {
        const meta = isRecord(envelope) && isRecord(envelope.meta)
            ? envelope.meta
            : undefined;
        if (!isRecord(envelope) ||
            envelope.schema !== TRACE_WORKER_RESPONSE ||
            typeof envelope.ok !== "boolean" ||
            meta === undefined ||
            meta.requestId !== requestId) {
            throw new UEApiError({
                code: "trace_worker_invalid_response",
                message: "Trace Worker returned an invalid or uncorrelated response envelope.",
                details: envelope,
            });
        }
        if (exitCode !== 0 || envelope.ok !== true || !isRecord(envelope.data)) {
            const payload = isRecord(envelope.error) ? envelope.error : {};
            throw new UEApiError({
                code: typeof payload.code === "string"
                    ? payload.code
                    : "trace_worker_failed",
                message: typeof payload.message === "string"
                    ? payload.message
                    : `Trace Worker exited with code ${exitCode}.`,
                details: payload.details,
            });
        }
        return envelope.data;
    }
}
//# sourceMappingURL=trace-worker.js.map