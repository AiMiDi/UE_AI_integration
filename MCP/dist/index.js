#!/usr/bin/env node
/**
 * UE_AI_integration stdio entry point.
 *
 * Tool registration is entirely local and manifest-driven, so all eleven MCP
 * tools are available even when Unreal Editor is offline. Calls that require
 * UE connect to an already-running editor on UE_PORT; this process never
 * launches or shuts down the editor.
 */
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { createMcpServer } from "./mcp-server.js";
import { log, UE_PORT } from "./ue-bridge.js";
export function createLocalShutdownHandler(close, exit) {
    let closing = false;
    return async () => {
        if (closing) {
            return;
        }
        closing = true;
        try {
            await close();
        }
        finally {
            exit(0);
        }
    };
}
export async function main() {
    const runtime = createMcpServer();
    const transport = new StdioServerTransport();
    await runtime.server.connect(transport);
    const summary = runtime.catalog.summary();
    log.info("UE_AI_integration ready", {
        port: UE_PORT,
        registeredTools: runtime.registeredToolNames.length,
        capabilityCount: summary.capabilityCount,
        domainCounts: summary.domainCounts,
    });
    const shutdown = createLocalShutdownHandler(() => runtime.server.close(), (code) => process.exit(code));
    process.once("SIGINT", () => {
        void shutdown();
    });
    process.once("SIGTERM", () => {
        void shutdown();
    });
}
const isEntrypoint = process.argv[1] !== undefined &&
    import.meta.url === pathToFileURL(resolve(process.argv[1])).href;
if (isEntrypoint) {
    main().catch((error) => {
        log.error("Fatal error", {
            error: error.message,
            stack: error.stack,
        });
        process.exit(1);
    });
}
//# sourceMappingURL=index.js.map