import type { CapabilityExecutionContext, CapabilityExecutor } from "./domain-router.js";
import { RecipeRunner } from "./recipe-runner.js";
import { type UEExecuteData } from "./ue-bridge.js";
export declare class RecipeRunnerExecutor implements CapabilityExecutor {
    private readonly runner;
    constructor(runner?: RecipeRunner);
    execute(id: string, params?: Record<string, unknown>, _requestId?: string, context?: CapabilityExecutionContext): Promise<UEExecuteData>;
}
