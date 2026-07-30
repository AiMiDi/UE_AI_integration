#!/usr/bin/env node
/**
 * UE_AI_integration stdio entry point.
 *
 * Tool registration is entirely local and manifest-driven, so all twelve MCP
 * tools are available even when Unreal Editor is offline. Calls that require
 * UE connect to an already-running editor on UE_PORT; this process never
 * launches or shuts down the editor.
 */
export type LocalClose = () => Promise<void>;
export type LocalExit = (code: number) => void;
export declare function createLocalShutdownHandler(close: LocalClose, exit: LocalExit): () => Promise<void>;
export declare function main(): Promise<void>;
