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
export declare function safeStringify(data: unknown, maxLength?: number): string;
export declare function formatJsonResponse(data: unknown): MCPResponse;
export declare function formatErrorResponse(error: unknown): MCPResponse;
export declare function formatCapabilityResponse(capability: CapabilityDescriptor, data: Record<string, unknown>): MCPResponse;
