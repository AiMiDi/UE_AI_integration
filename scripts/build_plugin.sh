#!/usr/bin/env bash
# Build a host-platform UE_AI_integration package and its native CLIs from source.
#
# Usage:
#   bash scripts/build_plugin.sh <engine_path> [output_path]
#
# UE_ENGINE_ROOT may be used instead of the first positional argument.
# Set UEAI_RUN_PORTABLE_TESTS=1 to build and run the portable CTest suite.
# This packaging entry point never runs UE Automation.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd -- "${script_dir}/.." && pwd)"
engine_path="${1:-${UE_ENGINE_ROOT:-}}"
output_path="${2:-$(cd -- "${plugin_root}/.." && pwd)/UE_AI_integration-BuiltPlugin}"
mkdir -p -- "$(dirname -- "${output_path}")"
output_path="$(cd -- "$(dirname -- "${output_path}")" && pwd)/$(basename -- "${output_path}")"
case "${output_path}" in
    "${plugin_root}"|"${plugin_root}"/*)
        echo "ERROR: BuildPlugin output must be outside the plugin source tree." >&2
        exit 2
        ;;
esac

case "$(uname -s)" in
    Linux)
        target_platform="Linux"
        ;;
    Darwin)
        target_platform="Mac"
        ;;
    *)
        echo "ERROR: build_plugin.sh supports Linux and macOS hosts." >&2
        echo "Use scripts/build_plugin.bat on Windows." >&2
        exit 2
        ;;
esac

if [[ -z "${engine_path}" ]]; then
    echo "ERROR: Unreal Engine root is required." >&2
    echo "Usage: bash scripts/build_plugin.sh <engine_path> [output_path]" >&2
    echo "Alternatively set UE_ENGINE_ROOT." >&2
    exit 2
fi

uat="${engine_path}/Engine/Build/BatchFiles/RunUAT.sh"
plugin_path="${plugin_root}/UE_AI_integration.uplugin"

if [[ ! -x "${uat}" ]]; then
    echo "ERROR: executable RunUAT.sh not found at ${uat}" >&2
    exit 1
fi
if ! command -v node >/dev/null 2>&1; then
    echo "ERROR: Node.js 20+ is required." >&2
    exit 1
fi
if ! command -v npm >/dev/null 2>&1; then
    echo "ERROR: npm is required." >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo "ERROR: CMake 3.24+ is required." >&2
    exit 1
fi

node_major="$(node -p 'Number(process.versions.node.split(".")[0])')"
if (( node_major < 20 )); then
    echo "ERROR: Node.js 20+ is required; found $(node --version)." >&2
    exit 1
fi

cli_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/ue-ai-cli.XXXXXX")"
cleanup() {
    rm -rf -- "${cli_build_dir}"
}
trap cleanup EXIT

echo
echo "========================================"
echo " Packaging UE_AI_integration Plugin"
echo "========================================"
echo " Engine:   ${engine_path}"
echo " Platform: ${target_platform}"
echo " Plugin:   ${plugin_path}"
echo " Output:   ${output_path}"
echo "========================================"
echo

node "${plugin_root}/scripts/validate_capabilities.mjs"
node "${plugin_root}/scripts/validate_skills.mjs"

echo
echo "Building and testing the MCP bridge from current TypeScript sources..."
npm ci --prefix "${plugin_root}/MCP"
npm run build --prefix "${plugin_root}/MCP"
npm test --prefix "${plugin_root}/MCP"
npm audit --omit=dev --prefix "${plugin_root}/MCP"

"${uat}" BuildPlugin \
    "-Plugin=${plugin_path}" \
    "-Package=${output_path}" \
    "-TargetPlatforms=${target_platform}" \
    -Rocket

echo
echo "Building and staging UEAITraceWorker..."
bash "${plugin_root}/scripts/build_trace_worker.sh" \
    "${engine_path}" \
    "${plugin_root}" \
    "${output_path}"

echo
echo "Restoring packaged MCP production dependencies..."
npm ci --omit=dev --prefix "${output_path}/MCP"

echo
cli_tests="OFF"
if [[ "${UEAI_RUN_PORTABLE_TESTS:-0}" == "1" ]]; then
    cli_tests="ON"
fi
echo "Building and packaging native ue-cli and ue-workflow-cli (portable tests: ${cli_tests})..."
cmake \
    -S "${plugin_root}" \
    -B "${cli_build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DUE_WORKFLOW_BUILD_TESTS=${cli_tests}" \
    -DUE_WORKFLOW_BUILD_CLI=ON
if [[ "${cli_tests}" == "ON" ]]; then
    cmake --build "${cli_build_dir}" --config Release
    ctest --test-dir "${cli_build_dir}" -C Release --output-on-failure
else
    cmake --build "${cli_build_dir}" --config Release --target ue ue-workflow
fi
cmake --install "${cli_build_dir}" --config Release --prefix "${output_path}/CLI"

echo
echo "========================================"
echo " PACKAGE BUILD SUCCESSFUL"
echo "========================================"
echo
echo "Plugin built to: ${output_path}"
echo "Native CLIs:"
echo "  ${output_path}/CLI/bin/ue-cli"
echo "  ${output_path}/CLI/bin/ue-workflow-cli"
if [[ "${cli_tests}" == "ON" ]]; then
    echo "Portable CTest gate: PASSED"
else
    echo "Portable CTest gate: NOT RUN (set UEAI_RUN_PORTABLE_TESTS=1 to enable)"
fi
echo "UE Automation: NOT RUN by this packaging entry point."
echo "A successful package build is not, by itself, release qualification."
