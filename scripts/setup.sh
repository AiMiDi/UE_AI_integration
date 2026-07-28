#!/bin/bash
# Quick setup script — run from your UE5 project root
# Usage: bash path/to/UE_AI_integration/scripts/setup.sh

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PLUGIN_DIR="$SCRIPT_DIR/.."

echo ""
echo "========================================"
echo " UE_AI_integration — Setup"
echo "========================================"
echo ""

# 1. Check Node.js
if ! command -v node &> /dev/null; then
    echo "ERROR: Node.js not found. Install Node.js 20+ first."
    exit 1
fi
NODE_MAJOR="$(node -p 'Number(process.versions.node.split(".")[0])')"
if [ "$NODE_MAJOR" -lt 20 ]; then
    echo "ERROR: Node.js 20+ is required; found $(node --version)."
    exit 1
fi
echo "[OK] Node.js $(node --version)"

# 2. Verify the capability catalog and build the TypeScript bridge if needed
if [ ! -d "$PLUGIN_DIR/Resources/Capabilities" ]; then
    echo "ERROR: Capability manifests are missing from Resources/Capabilities."
    exit 1
fi

node "$PLUGIN_DIR/scripts/validate_capabilities.mjs"

echo "[..] Installing locked MCP dependencies..."
cd "$PLUGIN_DIR/MCP"
npm ci --silent
echo "[OK] MCP dependencies installed"

if [ ! -f "$PLUGIN_DIR/MCP/dist/index.js" ]; then
    echo "[..] Building TypeScript bridge..."
    npm run build
    echo "[OK] Bridge built"
else
    echo "[OK] Bridge already built"
fi

# 3. Find project root (look for .uproject file)
PROJECT_ROOT="$(pwd)"
if ! ls "$PROJECT_ROOT"/*.uproject &> /dev/null 2>&1; then
    echo ""
    echo "WARNING: No .uproject found in current directory."
    echo "Run this script from your UE5 project root."
    echo ""
fi

# 4. Register MCP server with Claude Code
BRIDGE_PATH="$PLUGIN_DIR/MCP/dist/index.js"
echo "[..] Registering MCP server with Claude Code..."
claude mcp add ue_ai_integration -- node "$BRIDGE_PATH" 2>/dev/null && echo "[OK] MCP server registered" || echo "[!!] Could not register (is Claude Code installed?)"

echo ""
echo "========================================"
echo " Setup complete!"
echo "========================================"
echo ""
echo " Next steps:"
echo "   1. Open your project in UE5"
echo "   2. Run 'claude' in this folder"
echo "   3. Call ue_status, then inspect ue_capabilities"
echo ""
