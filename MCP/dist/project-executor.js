import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync, realpathSync, statSync } from "node:fs";
import { basename, dirname, extname, isAbsolute, relative, resolve, sep } from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { UEApiError } from "./ue-bridge.js";
const MAX_TEXT = 4 * 1024 * 1024;
const MAX_ASSET_WORKER_OUTPUT = 1024 * 1024;
function inside(root, candidate) {
    const rel = relative(root, candidate);
    return rel === "" || (!rel.startsWith(`..${sep}`) && rel !== ".." && !isAbsolute(rel));
}
function projectRoot(params) {
    if (typeof params.projectRoot !== "string" || params.projectRoot.trim() === "") {
        throw new UEApiError({ code: "project_root_required", message: "projectRoot must be an explicit absolute project directory." });
    }
    const root = resolve(params.projectRoot);
    if (!isAbsolute(params.projectRoot) || !existsSync(root) || !statSync(root).isDirectory()) {
        throw new UEApiError({ code: "project_root_invalid", message: "projectRoot must be an existing absolute directory." });
    }
    return realpathSync(root);
}
function boundedText(path) {
    const size = statSync(path).size;
    if (size > MAX_TEXT)
        throw new UEApiError({ code: "offline_file_too_large", message: `${basename(path)} exceeds the 4 MiB offline text limit.` });
    const bytes = readFileSync(path);
    const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    if (text.includes("\0"))
        throw new UEApiError({ code: "invalid_text_encoding", message: `${basename(path)} is not valid UTF-8 text.` });
    return text;
}
function resolveProjectFile(root, value, extensions) {
    if (typeof value !== "string" || value.trim() === "")
        throw new UEApiError({ code: "offline_path_required", message: "A project-relative path is required." });
    const candidate = resolve(root, value);
    if (!inside(root, candidate))
        throw new UEApiError({ code: "path_outside_allowed_root", message: "Offline paths must stay inside projectRoot." });
    if (!extensions.includes(extname(candidate).toLowerCase()))
        throw new UEApiError({ code: "offline_extension_forbidden", message: `Allowed extensions: ${extensions.join(", ")}.` });
    if (!existsSync(candidate) || !statSync(candidate).isFile())
        throw new UEApiError({ code: "offline_file_not_found", message: "The requested project file does not exist." });
    const realCandidate = realpathSync(candidate);
    if (!inside(root, realCandidate))
        throw new UEApiError({ code: "path_outside_allowed_root", message: "Offline paths must not escape projectRoot through a link or junction." });
    return realCandidate;
}
function uproject(root, requested) {
    const candidates = requested === undefined
        ? readdirSync(root).filter((name) => extname(name).toLowerCase() === ".uproject").map((name) => resolve(root, name))
        : [resolveProjectFile(root, requested, [".uproject"])];
    if (candidates.length !== 1)
        throw new UEApiError({ code: "uproject_ambiguous", message: "projectRoot must contain exactly one .uproject or params.uproject must select one." });
    try {
        return { path: candidates[0], data: JSON.parse(boundedText(candidates[0])) };
    }
    catch (error) {
        if (error instanceof UEApiError)
            throw error;
        throw new UEApiError({ code: "uproject_invalid", message: `.uproject is invalid JSON: ${error.message}` });
    }
}
function parseIni(text) {
    const result = {};
    let section = "";
    for (const raw of text.replace(/^\uFEFF/, "").split(/\r?\n/)) {
        const line = raw.trim();
        if (line === "" || line.startsWith(";") || line.startsWith("#"))
            continue;
        const header = /^\[([^\]]+)\]$/.exec(line);
        if (header) {
            section = header[1];
            result[section] ??= {};
            continue;
        }
        const equals = line.indexOf("=");
        if (equals <= 0)
            continue;
        result[section] ??= {};
        result[section][line.slice(0, equals).trim()] = line.slice(equals + 1).trim();
    }
    return result;
}
function sha256(value) {
    return `sha256:${createHash("sha256").update(value, "utf8").digest("hex")}`;
}
function sensitiveConfigKey(key) {
    const words = key
        .replace(/([A-Z]+)([A-Z][a-z])/g, "$1 $2")
        .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
        .split(/[^a-z0-9]+/i)
        .filter(Boolean)
        .map((word) => word.toLowerCase());
    const normalized = words.join("");
    return words.some((word) => ["key", "token", "secret", "password", "credential"].includes(word))
        || normalized.includes("privateexponent");
}
function publicConfigValue(key, value, source) {
    if (!sensitiveConfigKey(key))
        return { value, source };
    return {
        value: "<redacted>",
        present: true,
        valueDigest: sha256(value),
        source,
    };
}
function engineRoot(params) {
    if (params.engineRoot === undefined)
        return undefined;
    if (typeof params.engineRoot !== "string" || !isAbsolute(params.engineRoot)) {
        throw new UEApiError({ code: "engine_root_invalid", message: "engineRoot must be an explicit absolute Engine checkout root." });
    }
    const candidate = resolve(params.engineRoot);
    if (!existsSync(candidate) || !statSync(candidate).isDirectory()) {
        throw new UEApiError({ code: "engine_root_invalid", message: "engineRoot must be an existing directory." });
    }
    const real = realpathSync(candidate);
    if (!existsSync(resolve(real, "Engine", "Build", "Build.version"))
        || !existsSync(resolve(real, "Engine", "Plugins"))) {
        throw new UEApiError({ code: "engine_root_invalid", message: "engineRoot must contain Engine/Build/Build.version and Engine/Plugins." });
    }
    return real;
}
function parseDescriptor(path, code) {
    try {
        const parsed = JSON.parse(boundedText(path));
        if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed))
            throw new Error("descriptor root is not an object");
        return parsed;
    }
    catch (error) {
        if (error instanceof UEApiError)
            throw error;
        throw new UEApiError({ code, message: `${basename(path)} is invalid JSON: ${error.message}` });
    }
}
function scanPluginRoot(scanRoot, source, diagnostics, maximumDescriptors) {
    if (!existsSync(scanRoot) || !statSync(scanRoot).isDirectory())
        return [];
    const canonicalRoot = realpathSync(scanRoot);
    const descriptors = [];
    const pending = [{ path: canonicalRoot, depth: 0 }];
    while (pending.length > 0) {
        const current = pending.pop();
        if (current.depth > 16) {
            diagnostics.push({ severity: "warning", code: "plugin_scan_depth_exceeded", source, path: relative(canonicalRoot, current.path) });
            continue;
        }
        for (const entry of readdirSync(current.path, { withFileTypes: true }).sort((left, right) => left.name.localeCompare(right.name))) {
            const candidate = resolve(current.path, entry.name);
            if (entry.isSymbolicLink()) {
                diagnostics.push({ severity: "warning", code: "plugin_scan_link_skipped", source, path: relative(canonicalRoot, candidate) });
                continue;
            }
            if (entry.isDirectory()) {
                const real = realpathSync(candidate);
                if (inside(canonicalRoot, real))
                    pending.push({ path: real, depth: current.depth + 1 });
                else
                    diagnostics.push({ severity: "error", code: "plugin_root_escape", source, path: relative(canonicalRoot, candidate) });
                continue;
            }
            if (!entry.isFile() || extname(entry.name).toLowerCase() !== ".uplugin")
                continue;
            maximumDescriptors.value += 1;
            if (maximumDescriptors.value > 20_000) {
                throw new UEApiError({ code: "plugin_scan_limit_exceeded", message: "Plugin descriptor discovery exceeded the 20,000 file limit." });
            }
            try {
                descriptors.push({
                    name: basename(entry.name, extname(entry.name)),
                    source,
                    descriptorPath: realpathSync(candidate),
                    root: canonicalRoot,
                    data: parseDescriptor(candidate, "uplugin_invalid"),
                });
            }
            catch (error) {
                const payload = error;
                diagnostics.push({ severity: "warning", code: payload.code ?? "uplugin_invalid", source, path: relative(canonicalRoot, candidate), message: payload.message });
            }
        }
    }
    return descriptors;
}
function pluginKey(name) {
    return process.platform === "win32" ? name.toLowerCase() : name;
}
function engineBuildVersion(root) {
    return parseDescriptor(resolve(root, "Engine", "Build", "Build.version"), "engine_build_version_invalid");
}
function validatePlugins(root, project, params) {
    const diagnostics = [];
    const engine = engineRoot(params);
    const descriptorCount = { value: 0 };
    const indexed = [];
    indexed.push(...scanPluginRoot(resolve(root, "Plugins"), "project", diagnostics, descriptorCount));
    const additional = Array.isArray(project.data.AdditionalPluginDirectories)
        ? project.data.AdditionalPluginDirectories
        : [];
    for (const raw of additional) {
        if (typeof raw !== "string" || raw.trim() === "")
            continue;
        const candidate = resolve(dirname(project.path), raw);
        if (!existsSync(candidate) || !statSync(candidate).isDirectory()) {
            diagnostics.push({ severity: "error", code: "additional_plugin_root_missing", path: raw });
            continue;
        }
        const real = realpathSync(candidate);
        if (!inside(root, real) && (!engine || !inside(engine, real))) {
            diagnostics.push({ severity: "error", code: "additional_plugin_root_outside_allowed_roots", path: raw });
            continue;
        }
        indexed.push(...scanPluginRoot(real, "additional", diagnostics, descriptorCount));
    }
    if (engine)
        indexed.push(...scanPluginRoot(resolve(engine, "Engine", "Plugins"), "engine", diagnostics, descriptorCount));
    const precedence = { project: 0, additional: 1, engine: 2 };
    indexed.sort((left, right) => precedence[left.source] - precedence[right.source] || left.name.localeCompare(right.name));
    const byName = new Map();
    for (const descriptor of indexed) {
        const key = pluginKey(descriptor.name);
        const existing = byName.get(key);
        if (!existing)
            byName.set(key, descriptor);
        else
            diagnostics.push({
                severity: "warning",
                code: "plugin_shadowed",
                plugin: descriptor.name,
                selectedSource: existing.source,
                shadowedSource: descriptor.source,
                shadowedPath: relative(descriptor.root, descriptor.descriptorPath),
            });
    }
    const resolvedPlugins = [];
    const visited = new Set();
    const resolvePlugin = (name, optional, requestedBy) => {
        const key = pluginKey(name);
        if (visited.has(key))
            return;
        visited.add(key);
        const descriptor = byName.get(key);
        if (!descriptor) {
            const unresolvedSeverity = engine ? (optional ? "warning" : "error") : "info";
            diagnostics.push({
                severity: unresolvedSeverity,
                code: engine ? (optional ? "plugin_optional_dependency_missing" : "plugin_dependency_missing") : "plugin_unresolved_external",
                plugin: name,
                requestedBy,
            });
            resolvedPlugins.push({ name, source: "unresolved", requestedBy, optional });
            return;
        }
        resolvedPlugins.push({
            name,
            source: descriptor.source,
            requestedBy,
            optional,
            descriptorPath: relative(descriptor.root, descriptor.descriptorPath),
            version: descriptor.data.Version ?? null,
            versionName: descriptor.data.VersionName ?? null,
        });
        for (const dependency of Array.isArray(descriptor.data.Plugins) ? descriptor.data.Plugins : []) {
            if (typeof dependency !== "object" || dependency === null)
                continue;
            const value = dependency;
            if (value.Enabled === false || typeof value.Name !== "string")
                continue;
            resolvePlugin(value.Name, value.Optional === true, name);
        }
    };
    for (const plugin of Array.isArray(project.data.Plugins) ? project.data.Plugins : []) {
        if (typeof plugin !== "object" || plugin === null)
            continue;
        const value = plugin;
        if (value.Enabled === false || typeof value.Name !== "string")
            continue;
        resolvePlugin(value.Name, value.Optional === true, null);
    }
    let engineData = null;
    if (engine) {
        const build = engineBuildVersion(engine);
        const major = Number(build.MajorVersion);
        const minor = Number(build.MinorVersion);
        const association = typeof project.data.EngineAssociation === "string" ? project.data.EngineAssociation : "";
        const associationMatches = /^\d+\.\d+/.test(association) ? association.startsWith(`${major}.${minor}`) : null;
        if (associationMatches === false)
            diagnostics.push({ severity: "error", code: "engine_association_mismatch", engineAssociation: association, engineVersion: `${major}.${minor}` });
        if (associationMatches === null && association !== "")
            diagnostics.push({ severity: "info", code: "engine_association_unresolved", engineAssociation: association });
        engineData = { major, minor, patch: Number(build.PatchVersion), associationMatches };
    }
    return {
        schema: "ue.local-project-validation.v2",
        valid: !diagnostics.some((item) => item.severity === "error"),
        diagnostics,
        plugins: resolvedPlugins,
        pluginIndex: { descriptorCount: descriptorCount.value, engineRootSupplied: engine !== undefined },
        engine: engineData,
        buildConfiguration: { present: existsSync(resolve(root, "BuildConfiguration.xml")) },
    };
}
export class LocalProjectExecutor {
    async execute(id, params = {}) {
        const root = projectRoot(params);
        const project = uproject(root, params.uproject);
        if (id === "production.project.summary.get") {
            return {
                schema: "ue.local-project-summary.v1",
                projectRoot: root,
                projectFile: project.path,
                projectName: basename(project.path, ".uproject"),
                engineAssociation: project.data.EngineAssociation ?? null,
                category: project.data.Category ?? null,
                pluginCount: Array.isArray(project.data.Plugins) ? project.data.Plugins.length : 0,
                moduleCount: Array.isArray(project.data.Modules) ? project.data.Modules.length : 0,
                digest: sha256(JSON.stringify(project.data)),
            };
        }
        if (id === "production.project.config.get") {
            const requested = Array.isArray(params.files) ? params.files : [];
            const configDirectory = resolve(root, "Config");
            const files = requested.length > 0
                ? requested.map((value) => resolveProjectFile(root, value, [".ini", ".xml", ".uplugin", ".uproject"]))
                : (existsSync(configDirectory) ? readdirSync(configDirectory, { withFileTypes: true }) : [])
                    .filter((entry) => entry.isFile() && extname(entry.name).toLowerCase() === ".ini")
                    .map((entry) => resolve(configDirectory, entry.name)).sort();
            const merged = {};
            for (const file of files) {
                if (extname(file).toLowerCase() !== ".ini")
                    continue;
                for (const [section, values] of Object.entries(parseIni(boundedText(file)))) {
                    merged[section] ??= {};
                    for (const [key, value] of Object.entries(values)) {
                        merged[section][key] = publicConfigValue(key, value, relative(root, file));
                    }
                }
            }
            return { schema: "ue.local-project-config.v1", projectRoot: root, files: files.map((file) => relative(root, file)), merged };
        }
        if (id === "production.project.validate") {
            return validatePlugins(root, project, params);
        }
        throw new UEApiError({ code: "local_project_operation_unsupported", message: `Unsupported localProject capability ${id}.` });
    }
}
export class LocalAssetExecutor {
    async execute(id, params = {}, _requestId, context) {
        const root = projectRoot(params);
        const asset = resolveProjectFile(root, params.assetPath, [".uasset", ".uexp"]);
        const script = fileURLToPath(new URL("./asset-worker.js", import.meta.url));
        return new Promise((resolvePromise, rejectPromise) => {
            const child = spawn(process.execPath, [script], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
            let output = "";
            let errors = "";
            let outputExceeded = false;
            const abort = () => child.kill();
            context?.signal?.addEventListener("abort", abort, { once: true });
            child.stdout.setEncoding("utf8");
            child.stderr.setEncoding("utf8");
            child.stdout.on("data", (chunk) => {
                if (outputExceeded)
                    return;
                output += chunk;
                if (Buffer.byteLength(output, "utf8") > MAX_ASSET_WORKER_OUTPUT) {
                    outputExceeded = true;
                    child.kill();
                }
            });
            child.stderr.on("data", (chunk) => { errors = (errors + chunk).slice(-8192); });
            child.on("error", (error) => rejectPromise(new UEApiError({ code: "asset_worker_unavailable", message: error.message })));
            child.on("close", (code) => {
                context?.signal?.removeEventListener("abort", abort);
                if (context?.signal?.aborted)
                    return rejectPromise(new UEApiError({ code: "request_cancelled", message: "Asset Worker request was cancelled." }));
                if (outputExceeded)
                    return rejectPromise(new UEApiError({ code: "asset_output_budget_exceeded", message: "Asset Worker output exceeded the 1 MiB transport ceiling." }));
                try {
                    const payload = JSON.parse(output);
                    if (!payload.ok || !payload.data)
                        return rejectPromise(new UEApiError(payload.error ?? { code: "asset_worker_failed", message: errors || `Asset Worker exited ${code}.` }));
                    resolvePromise(payload.data);
                }
                catch (error) {
                    rejectPromise(new UEApiError({ code: "asset_worker_invalid_response", message: errors || error.message }));
                }
            });
            const otherAssetPath = id === "production.asset.package.diff"
                ? resolveProjectFile(root, params.otherAssetPath, [".uasset", ".uexp"])
                : undefined;
            child.stdin.end(JSON.stringify({
                operation: id,
                assetPath: asset,
                otherAssetPath,
                engineVersion: params.engineVersion ?? "5.3",
                detail: params.detail,
                sections: params.sections,
                offset: params.offset,
                limit: params.limit,
                targetTokens: params.targetTokens,
                maxBytes: params.maxBytes,
            }));
        });
    }
}
//# sourceMappingURL=project-executor.js.map