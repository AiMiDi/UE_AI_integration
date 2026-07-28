import { existsSync, statSync } from "node:fs";
import { delimiter, dirname, extname, isAbsolute, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
function defaultIsFile(path) {
    try {
        return existsSync(path) && statSync(path).isFile();
    }
    catch {
        return false;
    }
}
function environmentValue(env, key) {
    const match = Object.entries(env).find(([candidate]) => candidate.toLocaleLowerCase() === key.toLocaleLowerCase());
    return match?.[1];
}
function executableNames(command, platform, env) {
    if (platform !== "win32" || extname(command).length > 0) {
        return [command];
    }
    const extensions = (environmentValue(env, "PATHEXT") ?? ".EXE;.CMD;.BAT;.COM")
        .split(";")
        .filter(Boolean);
    return [command, ...extensions.map((extension) => `${command}${extension}`)];
}
function pathCandidates(command, platform, env) {
    const pathValue = environmentValue(env, "PATH") ?? "";
    const names = executableNames(command, platform, env);
    return pathValue
        .split(delimiter)
        .map((entry) => entry.trim().replace(/^"(.*)"$/, "$1"))
        .filter(Boolean)
        .flatMap((entry) => names.map((name) => join(entry, name)));
}
function pathEntryCount(env) {
    return (environmentValue(env, "PATH") ?? "")
        .split(delimiter)
        .map((entry) => entry.trim())
        .filter(Boolean).length;
}
function quoteCommand(path) {
    return /\s/.test(path) ? `"${path}"` : path;
}
export function locateWorkflowCli(options = {}) {
    const env = options.env ?? process.env;
    const platform = options.platform ?? process.platform;
    const moduleUrl = options.moduleUrl ?? import.meta.url;
    const isFile = options.isFile ?? defaultIsFile;
    const pluginRoot = resolve(dirname(fileURLToPath(moduleUrl)), "..", "..");
    const executableName = platform === "win32" ? "ue-workflow.exe" : "ue-workflow";
    const configuredPath = env.UE_WORKFLOW_CLI?.trim() || null;
    const candidates = [];
    const seen = new Set();
    const addCandidate = (source, path) => {
        const absolutePath = isAbsolute(path) ? path : resolve(path);
        const key = platform === "win32" ? absolutePath.toLocaleLowerCase() : absolutePath;
        if (seen.has(key)) {
            return;
        }
        seen.add(key);
        candidates.push({ source, path: absolutePath, exists: isFile(absolutePath) });
    };
    if (configuredPath) {
        if (isAbsolute(configuredPath) ||
            configuredPath.includes("/") ||
            configuredPath.includes("\\")) {
            addCandidate("environment", configuredPath);
        }
        else {
            const configuredMatch = pathCandidates(configuredPath, platform, env).find(isFile);
            if (configuredMatch) {
                addCandidate("environment", configuredMatch);
            }
        }
    }
    addCandidate("packaged", join(pluginRoot, "CLI", "bin", executableName));
    addCandidate("packaged", join(pluginRoot, "bin", executableName));
    const pathMatch = pathCandidates("ue-workflow", platform, env).find(isFile);
    if (pathMatch) {
        addCandidate("path", pathMatch);
    }
    addCandidate("development", join(pluginRoot, "build-eval-workflow", "CLI", "Release", executableName));
    addCandidate("development", join(pluginRoot, "build-workflow", "CLI", "Release", executableName));
    const match = candidates.find((candidate) => candidate.exists);
    return {
        found: match !== undefined,
        executablePath: match?.path ?? null,
        source: match?.source ?? "not_found",
        command: match ? quoteCommand(match.path) : "ue-workflow",
        configuredPath,
        pluginRoot,
        candidates,
        pathEntriesSearched: pathEntryCount(env),
        guidance: match
            ? `${quoteCommand(match.path)} --json doctor --connect`
            : "Install ue-workflow or set UE_WORKFLOW_CLI to its absolute executable path.",
    };
}
//# sourceMappingURL=cli-locator.js.map