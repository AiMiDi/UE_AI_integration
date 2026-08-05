import type { CapabilityExecutionContext, CapabilityExecutor } from "./domain-router.js";
import { type UEExecuteData } from "./ue-bridge.js";
type JsonObject = Record<string, unknown>;
export declare class DevelopmentBridgeExecutor implements CapabilityExecutor {
    execute(id: string, params?: JsonObject, _requestId?: string, context?: CapabilityExecutionContext): Promise<UEExecuteData>;
}
export {};
