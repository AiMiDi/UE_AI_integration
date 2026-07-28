import { UEApiError } from "./ue-bridge.js";
function formatTruncatedJson(serialized, maxLength) {
    const createEnvelope = (previewLength) => JSON.stringify({
        truncated: true,
        totalCharacters: serialized.length,
        preview: serialized.slice(0, previewLength),
    }, null, 2);
    let low = 0;
    let high = Math.min(serialized.length, Math.max(0, maxLength));
    let best = createEnvelope(0);
    while (low <= high) {
        const middle = Math.floor((low + high) / 2);
        const candidate = createEnvelope(middle);
        if (candidate.length <= maxLength) {
            best = candidate;
            low = middle + 1;
        }
        else {
            high = middle - 1;
        }
    }
    return best;
}
export function safeStringify(data, maxLength = 1_000_000) {
    const serialized = JSON.stringify(data, null, 2) ?? String(data);
    if (serialized.length <= maxLength) {
        return serialized;
    }
    return formatTruncatedJson(serialized, maxLength);
}
export function formatJsonResponse(data) {
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
export function formatErrorResponse(error) {
    const payload = error instanceof UEApiError
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
export function formatCapabilityResponse(capability, data) {
    if (capability.output.kind === "json") {
        return formatJsonResponse(data);
    }
    if (typeof data.image_base64 !== "string" ||
        data.image_base64.length === 0) {
        return formatErrorResponse(new UEApiError({
            code: "invalid_image_output",
            message: `Capability "${capability.id}" declared image output but returned no image_base64`,
            details: data,
        }));
    }
    const mimeType = typeof data.mime_type === "string" && data.mime_type.length > 0
        ? data.mime_type
        : "image/jpeg";
    const content = [
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
//# sourceMappingURL=helpers.js.map