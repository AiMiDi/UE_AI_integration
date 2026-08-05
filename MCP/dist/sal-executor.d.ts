import type { CapabilityExecutor } from "./domain-router.js";
import { type UEExecuteData } from "./ue-bridge.js";
export declare class SalExecutor implements CapabilityExecutor {
    execute(id: string, params?: Record<string, unknown>): Promise<UEExecuteData>;
}
