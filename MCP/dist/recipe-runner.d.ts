import { type CapabilityCatalog } from "./capability-catalog.js";
export declare const RECIPE_SCHEMA = "ue.recipe.v2";
export declare const RECIPE_PLAN_SCHEMA = "ue.recipe-plan.v2";
export declare const RECIPE_RUN_SCHEMA = "ue.recipe-run.v1";
type JsonObject = Record<string, unknown>;
type RunStatus = "running" | "awaitingApproval" | "completed" | "failed" | "cancelled";
interface RetryPolicy {
    maxAttempts: number;
    backoffMs: number;
    transientErrors: string[];
}
export interface RecipeStep extends JsonObject {
    id: string;
    kind: "capability" | "workflow" | "poll" | "condition" | "approval" | "sourceControlCheckout";
    capability?: string;
    params?: JsonObject;
    workflow?: JsonObject;
    approvePlanDigest?: string;
    condition?: unknown;
    poll?: {
        capability: string;
        params?: JsonObject;
        until: unknown;
        maxAttempts: number;
        intervalMs: number;
    };
    retry?: Partial<RetryPolicy>;
    compensate?: {
        kind: "capability" | "workflow";
        capability?: string;
        params?: JsonObject;
        runId?: string;
        approvePlanDigest?: string;
    };
    highRisk?: boolean;
}
export interface RecipeDefinition extends JsonObject {
    schema: typeof RECIPE_SCHEMA;
    id: string;
    version: string;
    inputs?: JsonObject;
    steps: RecipeStep[];
}
export interface RecipeValidation {
    ok: boolean;
    schema: "ue.recipe-validation.v2";
    diagnostics: Array<{
        path: string;
        code: string;
        message: string;
    }>;
    normalized?: RecipeDefinition;
    planDigest?: string;
    requiresApproval?: boolean;
}
interface StepState {
    id: string;
    status: "pending" | "running" | "completed" | "failed" | "compensated";
    attempts: number;
    startedAt?: string;
    completedAt?: string;
    output?: unknown;
    error?: {
        code: string;
        message: string;
    };
    workflowReceipt?: unknown;
    approvalDigest?: string;
}
export interface RecipeRunState extends JsonObject {
    schema: typeof RECIPE_RUN_SCHEMA;
    runId: string;
    recipeId: string;
    recipeVersion: string;
    planDigest: string;
    status: RunStatus;
    phase: string;
    createdAt: string;
    updatedAt: string;
    heartbeatAt: string;
    lastProgressAt: string;
    lastLog: string;
    nextStep: number;
    inputs: JsonObject;
    recipe: RecipeDefinition;
    steps: StepState[];
    cancelRequested: boolean;
    cancelPending: boolean;
    awaitingApproval?: {
        stepId: string;
        reason: string;
        planDigest: string;
    };
    approvedSteps: string[];
    result?: unknown;
    error?: {
        code: string;
        message: string;
    };
    compensations: Array<{
        stepId: string;
        ok: boolean;
        output?: unknown;
        error?: unknown;
    }>;
}
export declare function validateRecipe(value: unknown, catalog?: CapabilityCatalog): RecipeValidation;
export declare class RecipeRunner {
    private readonly catalog;
    private readonly env;
    private readonly endpoint;
    constructor(options?: {
        catalog?: CapabilityCatalog;
        env?: NodeJS.ProcessEnv;
        endpoint?: string;
    });
    validate(recipe: unknown): RecipeValidation;
    plan(recipe: unknown): JsonObject;
    start(recipe: unknown, inputs?: JsonObject, approvePlanDigest?: string): RecipeRunState;
    status(runId: string): RecipeRunState;
    resume(runId: string, approvePlanDigest: string): RecipeRunState;
    cancel(runId: string): RecipeRunState;
    result(runId: string): RecipeRunState;
    list(limit?: number): RecipeRunState[];
}
export declare function executeRecipeRun(runId: string, endpoint?: string): Promise<void>;
export {};
