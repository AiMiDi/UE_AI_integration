import { type CapabilityCatalog } from "./capability-catalog.js";
export declare const RECIPE_SCHEMA = "ue.recipe.v2";
export declare const RECIPE_PLAN_SCHEMA = "ue.recipe-plan.v2";
export declare const RECIPE_RUN_SCHEMA = "ue.recipe-run.v2";
type JsonObject = Record<string, unknown>;
type RunStatus = "running" | "interrupted" | "awaitingApproval" | "completed" | "failed" | "cancelled";
interface RetryPolicy {
    maxAttempts: number;
    backoffMs: number;
    transientErrors: string[];
}
export interface RecipeStep extends JsonObject {
    id: string;
    kind: "capability" | "sessionCapability" | "workflow" | "poll" | "condition" | "approval" | "sourceControlCheckout";
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
        kind: "capability" | "sessionCapability" | "workflow";
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
    sessionPolicy?: {
        requirePieStopped: true;
        runnerStartsPie: true;
        lease: "pie";
        editorInstanceBound: true;
    };
    steps: RecipeStep[];
}
interface RecipeSessionBinding extends JsonObject {
    catalogDigest: string;
    serverInstanceId: string;
    processId?: number;
    processStartTime?: string;
    plannedPieState: string;
    plannedPieGeneration?: number;
    activePieGeneration?: number;
    activePieSessionId?: string;
    leaseId?: string;
    runnerStartedPie: boolean;
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
    status: "pending" | "dispatching" | "running" | "completed" | "failed" | "compensated";
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
    workerPid?: number;
    workerInstanceId?: string;
    workerHeartbeatAt?: string;
    workerLeaseExpiresAt?: string;
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
    session?: RecipeSessionBinding;
}
export declare function recipeHasSessionSteps(value: unknown): boolean;
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
    private prepare;
    private planResult;
    plan(recipe: unknown, inputs?: JsonObject): JsonObject;
    planOnline(recipe: unknown, inputs?: JsonObject): Promise<JsonObject>;
    private startPrepared;
    start(recipe: unknown, inputs?: JsonObject, approvePlanDigest?: string): RecipeRunState;
    startOnline(recipe: unknown, inputs?: JsonObject, approvePlanDigest?: string): Promise<RecipeRunState>;
    status(runId: string): RecipeRunState;
    resume(runId: string, approvePlanDigest: string, approveStepDigest?: string): RecipeRunState;
    cancel(runId: string): RecipeRunState;
    result(runId: string): RecipeRunState;
    list(limit?: number): RecipeRunState[];
}
export declare function executeRecipeRun(runId: string, endpoint?: string, workerInstanceId?: string): Promise<void>;
export {};
