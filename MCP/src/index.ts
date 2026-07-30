#!/usr/bin/env node

/**
 * UE_AI_integration stdio entry point.
 *
 * Tool registration is entirely local and manifest-driven, so all twelve MCP
 * tools are available even when Unreal Editor is offline. Calls that require
 * UE connect to an already-running editor on UE_PORT; this process never
 * launches or shuts down the editor.
 */

import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";

import { createMcpServer } from "./mcp-server.js";
import { log, UE_PORT, ueClient } from "./ue-bridge.js";

export type LocalClose = () => Promise<void>;
export type LocalExit = (code: number) => void;

export function createLocalShutdownHandler(
  close: LocalClose,
  exit: LocalExit,
): () => Promise<void> {
  let closing = false;
  return async () => {
    if (closing) {
      return;
    }
    closing = true;
    try {
      await close();
    } finally {
      exit(0);
    }
  };
}

export async function main(): Promise<void> {
  const runtime = createMcpServer();
  const transport = new StdioServerTransport();
  runtime.server.server.oninitialized = () => {
    const clientInfo = runtime.server.server.getClientVersion();
    void ueClient.startSession(clientInfo);
  };
  await runtime.server.connect(transport);

  const summary = runtime.catalog.summary();
  log.info("UE_AI_integration ready", {
    port: UE_PORT,
    registeredTools: runtime.registeredToolNames.length,
    capabilityCount: summary.capabilityCount,
    domainCounts: summary.domainCounts,
  });

  const shutdown = createLocalShutdownHandler(
    async () => {
      await ueClient.stopSession();
      await runtime.server.close();
    },
    (code) => process.exit(code),
  );
  process.once("SIGINT", () => {
    void shutdown();
  });
  process.once("SIGTERM", () => {
    void shutdown();
  });
  process.stdin.once("end", () => {
    void shutdown();
  });
  process.stdin.once("close", () => {
    void shutdown();
  });
}

const isEntrypoint =
  process.argv[1] !== undefined &&
  import.meta.url === pathToFileURL(resolve(process.argv[1])).href;

if (isEntrypoint) {
  main().catch((error) => {
    log.error("Fatal error", {
      error: (error as Error).message,
      stack: (error as Error).stack,
    });
    process.exit(1);
  });
}
