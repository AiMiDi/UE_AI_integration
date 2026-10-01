import type { CapabilityDescriptor } from "./capability-catalog.js";
import { UEApiError } from "./ue-bridge.js";

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

const GENERIC_TRUNCATION_NEXT_ACTION =
  "Use the capability's bounded pagination or detail controls; this generic preview has no continuation.";

function formatTruncatedJson(
  serialized: string,
  maxLength: number,
): string {
  const createEnvelope = (previewLength: number) => {
    const envelope: MCPTruncatedOutput = {
      schema: "ue.mcp-output-truncated.v1",
      truncated: true,
      totalCharacters: serialized.length,
      preview: serialized.slice(0, previewLength),
      continuation: {
        status: "unavailable",
        reason: "generic_serialization",
        safeToInfer: false,
        nextAction: GENERIC_TRUNCATION_NEXT_ACTION,
      },
    };
    return JSON.stringify(envelope, null, 2);
  };

  let low = 0;
  let high = Math.min(serialized.length, Math.max(0, maxLength));
  let best = createEnvelope(0);
  while (low <= high) {
    const middle = Math.floor((low + high) / 2);
    const candidate = createEnvelope(middle);
    if (candidate.length <= maxLength) {
      best = candidate;
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }
  return best;
}

export function safeStringify(data: unknown, maxLength = 1_000_000): string {
  const serialized = JSON.stringify(data, null, 2) ?? String(data);
  if (serialized.length <= maxLength) {
    return serialized;
  }
  return formatTruncatedJson(serialized, maxLength);
}

export function formatJsonResponse(data: unknown): MCPResponse {
  return {
    content: [
      {
        type: "text",
        text: safeStringify(data),
      },
    ],
    isError: false,
  };
}

export function formatErrorResponse(error: unknown): MCPResponse {
  const payload =
    error instanceof UEApiError
      ? {
          code: error.code,
          message: error.message,
          ...(error.details === undefined ? {} : { details: error.details }),
          ...(error.status === undefined ? {} : { status: error.status }),
        }
      : {
          code: "mcp_error",
          message: error instanceof Error ? error.message : String(error),
        };

  return {
    content: [
      {
        type: "text",
        text: safeStringify({
          ok: false,
          error: payload,
        }),
      },
    ],
    isError: true,
  };
}

export function formatCapabilityResponse(
  capability: CapabilityDescriptor,
  data: Record<string, unknown>,
): MCPResponse {
  if (capability.output.kind === "json") {
    return formatJsonResponse(data);
  }

  if (
    typeof data.image_base64 !== "string" ||
    data.image_base64.length === 0
  ) {
    return formatErrorResponse(
      new UEApiError({
        code: "invalid_image_output",
        message: `Capability "${capability.id}" declared image output but returned no image_base64`,
        details: data,
      }),
    );
  }

  const mimeType =
    typeof data.mime_type === "string" && data.mime_type.length > 0
      ? data.mime_type
      : "image/jpeg";
  const content: MCPContent[] = [
    {
      type: "image",
      data: data.image_base64,
      mimeType,
    },
  ];

  const metadata = { ...data };
  delete metadata.image_base64;
  delete metadata.mime_type;
  if (Object.keys(metadata).length > 0) {
    content.push({
      type: "text",
      text: safeStringify(metadata),
    });
  }

  return {
    content,
    isError: false,
  };
}
