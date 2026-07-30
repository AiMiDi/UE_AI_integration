#!/usr/bin/env bash
# Build a host-platform UE_AI_integration package and its native CLIs from source.
#
# Usage:
#   bash scripts/build_plugin.sh <engine_path> [output_path]
#
# UE_ENGINE_ROOT may be used instead of the first positional argument.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
plugin_root="$(cd -- "${script_dir}/.." && pwd)"
engine_path="${1:-${UE_ENGINE_ROOT:-}}"
output_path="${2:-$(cd -- "${plugin_root}/.." && pwd)/UE_AI_integration-BuiltPlugin}"

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
echo " Building UE_AI_integration Plugin"
echo "========================================"
echo " Engine:   ${engine_path}"
echo " Platform: ${target_platform}"
echo " Plugin:   ${plugin_path}"
echo " Output:   ${output_path}"
echo "========================================"
echo

node "${plugin_root}/scripts/validate_capabilities.mjs"
node "${plugin_root}/scripts/validate_skills.mjs"

"${uat}" BuildPlugin \
    "-Plugin=${plugin_path}" \
    "-Package=${output_path}" \
    "-TargetPlatforms=${target_platform}" \
    -Rocket

echo
echo "Restoring packaged MCP production dependencies..."
npm ci --omit=dev --prefix "${output_path}/MCP"

echo
echo "Building and packaging native ue and ue-workflow CLIs..."
cmake \
    -S "${plugin_root}" \
    -B "${cli_build_dir}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DUE_WORKFLOW_BUILD_TESTS=OFF \
    -DUE_WORKFLOW_BUILD_CLI=ON
cmake --build "${cli_build_dir}" --config Release --target ue ue-workflow
cmake --install "${cli_build_dir}" --config Release --prefix "${output_path}/CLI"

echo
echo "========================================"
echo " BUILD SUCCESSFUL"
echo "========================================"
echo
echo "Plugin built to: ${output_path}"
echo "Native CLIs:"
echo "  ${output_path}/CLI/bin/ue"
echo "  ${output_path}/CLI/bin/ue-workflow"
