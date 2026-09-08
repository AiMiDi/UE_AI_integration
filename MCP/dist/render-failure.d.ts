import { type UEExecuteData } from "./ue-bridge.js";
type Json = Record<string, unknown>;
type Signal = {
    kind: string;
    line: number;
    byteOffset: number;
    excerpt: string;
    details: Json;
};
export declare function redactDiagnostic(text: string): string;
/** Parse only observed diagnostic signals. Nearby resources and active GPU
 * breadcrumbs are evidence of location/progress, never a root-cause verdict. */
export declare function parseRenderLog(text: string, limit?: number, startByte?: number): {
    signals: Signal[];
    matchedSignalCount: number;
    truncated: boolean;
    shortenedLineCount: number;
};
export declare function parseCrashContext(text: string): Json;
/** An offline localProject capability, also shared by the native CLI adapter. */
export declare function analyzeRenderFailure(root: string, params: Json): UEExecuteData;
export {};
