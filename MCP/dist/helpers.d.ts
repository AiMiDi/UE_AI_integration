import type { CapabilityDescriptor } from "./capability-catalog.js";
export interface TextContent {
    type: "text";
    text: string;
}
export interface ImageContent {
    type: "image";
    data: string;
    mimeType: string;
}
export type MCPContent = TextContent | ImageContent;
export interface MCPResponse {
    [key: string]: unknown;
    content: MCPContent[];
    isError?: boolean;
}
/**
 * Generic MCP serialization has no stable source cursor to resume from.  A
 * capability that can page its result must expose that continuation itself;
 * this marker prevents consumers from treating a JSON preview as complete.
 */
export interface MCPUnavailableContinuation {
    status: "unavailable";
    reason: "generic_serialization";
    safeToInfer: false;
    nextAction: string;
}
export interface MCPTruncatedOutput {
    schema: "ue.mcp-output-truncated.v1";
    truncated: true;
    totalCharacters: number;
    preview: string;
    continuation: MCPUnavailableContinuation;
}
export declare function safeStringify(data: unknown, maxLength?: number): string;
export declare function formatJsonResponse(data: unknown): MCPResponse;
export declare function formatErrorResponse(error: unknown): MCPResponse;
export declare function formatCapabilityResponse(capability: CapabilityDescriptor, data: Record<string, unknown>): MCPResponse;
