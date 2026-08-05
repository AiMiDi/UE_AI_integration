import { createHash } from "node:crypto";
import { existsSync, readFileSync, readdirSync, realpathSync, statSync } from "node:fs";
import { basename, extname, isAbsolute, relative, resolve, sep } from "node:path";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { UEApiError } from "./ue-bridge.js";
const MAX_TEXT = 4 * 1024 * 1024;
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
                    for (const [key, value] of Object.entries(values))
                        merged[section][key] = { value, source: relative(root, file) };
                }
            }
            return { schema: "ue.local-project-config.v1", projectRoot: root, files: files.map((file) => relative(root, file)), merged };
        }
        if (id === "production.project.validate") {
            const diagnostics = [];
            for (const plugin of Array.isArray(project.data.Plugins) ? project.data.Plugins : []) {
                if (typeof plugin !== "object" || plugin === null || typeof plugin.Name !== "string")
                    continue;
                if (plugin.Enabled === false)
                    continue;
                const name = String(plugin.Name);
                const local = resolve(root, "Plugins", name, `${name}.uplugin`);
                if (!existsSync(local))
                    diagnostics.push({ severity: "warning", code: "plugin_not_project_local", plugin: name, message: "Enabled plugin was not found under the project Plugins directory; it must be supplied by Engine or another allowed plugin root." });
            }
            const buildConfig = resolve(root, "BuildConfiguration.xml");
            return { schema: "ue.local-project-validation.v1", valid: !diagnostics.some((item) => item.severity === "error"), diagnostics, buildConfiguration: { present: existsSync(buildConfig) } };
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
            const abort = () => child.kill();
            context?.signal?.addEventListener("abort", abort, { once: true });
            child.stdout.setEncoding("utf8");
            child.stderr.setEncoding("utf8");
            child.stdout.on("data", (chunk) => { output = (output + chunk).slice(-4 * 1024 * 1024); });
            child.stderr.on("data", (chunk) => { errors = (errors + chunk).slice(-8192); });
            child.on("error", (error) => rejectPromise(new UEApiError({ code: "asset_worker_unavailable", message: error.message })));
            child.on("close", (code) => {
                context?.signal?.removeEventListener("abort", abort);
                if (context?.signal?.aborted)
                    return rejectPromise(new UEApiError({ code: "request_cancelled", message: "Asset Worker request was cancelled." }));
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
            child.stdin.end(JSON.stringify({ operation: id, assetPath: asset, otherAssetPath, engineVersion: params.engineVersion ?? "5.3" }));
        });
    }
}
//# sourceMappingURL=project-executor.js.map