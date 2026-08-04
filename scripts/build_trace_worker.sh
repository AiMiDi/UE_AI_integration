#!/usr/bin/env bash
# Build and stage the headless UEAITraceWorker Program.
# Usage: build_trace_worker.sh <engine_root> <plugin_root> <staging_plugin_root>

set -euo pipefail

engine_root="${1:?engine root is required}"
plugin_root="${2:?plugin root is required}"
staging_root="${3:?staging plugin root is required}"
host_template="${plugin_root}/Programs/UEAITraceWorker/UEAITraceWorker.uproject"
target_template="${plugin_root}/Programs/UEAITraceWorker/Source/UEAITraceWorker.Target.cs"

case "$(uname -s)" in
    Linux)
        platform="Linux"
        build_script="${engine_root}/Engine/Build/BatchFiles/Linux/Build.sh"
        executable="UEAITraceWorker"
        ;;
    Darwin)
        platform="Mac"
        build_script="${engine_root}/Engine/Build/BatchFiles/Mac/Build.sh"
        executable="UEAITraceWorker"
        ;;
    *)
        echo "ERROR: unsupported host platform." >&2
        exit 2
        ;;
esac

host_root="$(mktemp -d "${TMPDIR:-/tmp}/UEAITraceWorkerHost.XXXXXX")"
plugin_link="${host_root}/Plugins/UE_AI_integration"
cleanup() {
    rm -f -- "${plugin_link}"
    rm -rf -- "${host_root}"
}
trap cleanup EXIT
mkdir -p -- "${host_root}/Source" "${host_root}/Plugins"
cp -- "${host_template}" "${host_root}/UEAITraceWorker.uproject"
cp -- "${target_template}" "${host_root}/Source/UEAITraceWorker.Target.cs"
ln -s -- "${plugin_root}" "${plugin_link}"

"${build_script}" UEAITraceWorker "${platform}" Development \
    "-Project=${host_root}/UEAITraceWorker.uproject" \
    -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles

worker_bin="${host_root}/Binaries/${platform}"
if [[ ! -x "${worker_bin}/${executable}" ]]; then
    echo "ERROR: worker executable was not produced in ${worker_bin}." >&2
    exit 1
fi

engine_major="$(sed -n 's/.*"MajorVersion"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "${engine_root}/Engine/Build/Build.version" | head -n 1)"
engine_minor="$(sed -n 's/.*"MinorVersion"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "${engine_root}/Engine/Build/Build.version" | head -n 1)"
engine_version="${engine_major}.${engine_minor}"
trace_stage="${staging_root}/Tools/Trace/${platform}/${engine_version}"
mkdir -p "${trace_stage}"
cp "${worker_bin}/${executable}" "${trace_stage}/${executable}"
if [[ -f "${worker_bin}/UEAITraceWorker.target" ]]; then
    cp "${worker_bin}/UEAITraceWorker.target" "${trace_stage}/"
fi
if [[ "${platform}" == "Linux" && -f "${worker_bin}/${executable}.debug" ]]; then
    cp "${worker_bin}/${executable}.debug" "${trace_stage}/"
fi
if [[ "${platform}" == "Mac" && -d "${worker_bin}/${executable}.dSYM" ]]; then
    cp -R "${worker_bin}/${executable}.dSYM" "${trace_stage}/"
fi
cp "${plugin_root}/Resources/Trace/insights-actions.${engine_version}.json" "${trace_stage}/"
cp "${plugin_root}/Resources/Trace/launch-profiles.json" "${trace_stage}/"
cp "${plugin_root}/Resources/Trace/worker-protocol.v1.json" "${trace_stage}/"

if ! command -v node >/dev/null 2>&1; then
    echo "ERROR: Node.js is required to validate the staged Trace Worker bundle." >&2
    exit 1
fi

staged_worker="${trace_stage}/${executable}"
node - \
    "${staged_worker}" \
    "${trace_stage}" \
    "${plugin_root}" \
    "${engine_version}" <<'NODE'
const childProcess = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const [worker, traceStage, pluginRoot, engineVersion] = process.argv.slice(2);
const descriptor = JSON.parse(
    fs.readFileSync(path.join(pluginRoot, "UE_AI_integration.uplugin"), "utf8")
);
const requestSuffix = `${process.pid}-${Date.now()}`;

function execute(request) {
    const result = childProcess.spawnSync(worker, ["--stdio"], {
        input: `${JSON.stringify(request)}\n`,
        encoding: "utf8",
        maxBuffer: 16 * 1024 * 1024
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        throw new Error(
            `Trace Worker exited with ${result.status}: ${String(result.stderr).trim()}`
        );
    }
    try {
        return JSON.parse(String(result.stdout).trim());
    } catch (error) {
        throw new Error(`Trace Worker returned invalid JSON: ${error.message}`);
    }
}

function sha256File(file) {
    return crypto.createHash("sha256").update(fs.readFileSync(file)).digest("hex");
}

function expectedContractDigest() {
    const relativeFiles = [
        "Resources/Capabilities/production.json",
        `Resources/Trace/insights-actions.${engineVersion}.json`,
        "Resources/Trace/launch-profiles.json",
        "Resources/Trace/worker-protocol.v1.json"
    ].sort();
    const hash = crypto.createHash("sha256");
    for (const relative of relativeFiles) {
        hash.update(Buffer.from(relative.replaceAll("\\", "/"), "utf8"));
        hash.update(Buffer.from([0]));
        hash.update(fs.readFileSync(path.join(pluginRoot, relative)));
        hash.update(Buffer.from([0]));
    }
    return `sha256:${hash.digest("hex")}`;
}

const handshakeRequestId = `package-${requestSuffix}`;
const handshake = execute({
    schema: "ue.trace-worker-request.v1",
    action: "handshake",
    requestId: handshakeRequestId
});
const expectedContract = expectedContractDigest();
const expectedProvider = `sha256:${sha256File(
    path.join(pluginRoot, `Resources/Trace/insights-actions.${engineVersion}.json`)
)}`;
if (
    handshake.schema !== "ue.trace-worker-response.v1" ||
    handshake.ok !== true ||
    handshake.meta?.requestId !== handshakeRequestId ||
    handshake.data?.schema !== "ue.trace-worker-handshake.v1" ||
    handshake.data?.protocolVersion !== 1 ||
    handshake.data?.workerVersion !== descriptor.VersionName ||
    !String(handshake.data?.engineVersion ?? "").startsWith(engineVersion) ||
    handshake.data?.contractBound !== true ||
    handshake.data?.contractDigest !== expectedContract ||
    handshake.data?.providerSchemaDigest !== expectedProvider
) {
    throw new Error("Staged Trace Worker handshake does not match the plugin/Engine contract.");
}

const targetRequestId = `package-targets-${requestSuffix}`;
const targets = execute({
    schema: "ue.trace-worker-request.v1",
    action: "execute",
    requestId: targetRequestId,
    capability: "production.trace.target.list",
    params: {}
});
const developmentTarget = targets.data?.targets?.find(
    (target) => target.kind === "development"
);
const developmentProfile = targets.data?.launchProfiles?.find(
    (profile) => profile.id === "projectDevelopment"
);
if (
    targets.schema !== "ue.trace-worker-response.v1" ||
    targets.ok !== true ||
    targets.meta?.requestId !== targetRequestId ||
    targets.data?.schema !== "ue.trace-target-list.v1" ||
    developmentTarget?.available !== true ||
    developmentProfile?.executableKind !== "projectDevelopment"
) {
    throw new Error("Staged Trace Worker does not expose the installed Development launch profile.");
}

const files = fs.readdirSync(traceStage, { withFileTypes: true })
    .filter((entry) => entry.isFile() && entry.name !== "bundle-manifest.json")
    .map((entry) => {
        const fullPath = path.join(traceStage, entry.name);
        return {
            name: entry.name,
            size: fs.statSync(fullPath).size,
            sha256: sha256File(fullPath).toUpperCase()
        };
    })
    .sort((left, right) => left.name.localeCompare(right.name, "en"));
const requiredFiles = [
    path.basename(worker),
    `insights-actions.${engineVersion}.json`,
    "launch-profiles.json",
    "worker-protocol.v1.json"
];
for (const required of requiredFiles) {
    if (!files.some((file) => file.name === required)) {
        throw new Error(`Trace Worker bundle omits required file: ${required}`);
    }
}

const manifest = {
    schema: "ue.trace-worker-bundle.v1",
    workerVersion: descriptor.VersionName,
    engineVersion,
    protocolVersion: 1,
    contractDigest: handshake.data.contractDigest,
    providerSchemaDigest: handshake.data.providerSchemaDigest,
    files
};
const manifestPath = path.join(traceStage, "bundle-manifest.json");
fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");

const persisted = JSON.parse(fs.readFileSync(manifestPath, "utf8"));
if (
    persisted.schema !== manifest.schema ||
    persisted.workerVersion !== descriptor.VersionName ||
    persisted.engineVersion !== engineVersion ||
    persisted.contractDigest !== expectedContract ||
    persisted.providerSchemaDigest !== expectedProvider ||
    !Array.isArray(persisted.files) ||
    persisted.files.length !== files.length
) {
    throw new Error("Persisted Trace Worker bundle manifest is inconsistent.");
}
for (const file of persisted.files) {
    const fullPath = path.join(traceStage, file.name);
    const info = fs.statSync(fullPath);
    if (
        info.size !== file.size ||
        sha256File(fullPath).toUpperCase() !== file.sha256
    ) {
        throw new Error(`Trace Worker bundle evidence does not match: ${file.name}`);
    }
}
NODE

echo "UEAITraceWorker staged to ${trace_stage}"
