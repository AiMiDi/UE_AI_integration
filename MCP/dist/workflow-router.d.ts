import { z } from "zod";
import { type MCPResponse } from "./helpers.js";
import { type UEWorkflowData, type UEWorkflowRequest } from "./ue-bridge.js";
export declare const UE_WORKFLOW_DETAIL_LEVELS: readonly ["summary", "standard", "full"];
export declare const UE_WORKFLOW_SECTIONS: readonly ["operations", "finalizers", "readBack", "assetDiff", "structures", "rollback", "diagnostics"];
export declare const UE_WORKFLOW_TOOL_SCHEMA: z.ZodObject<{
    action: z.ZodEnum<["validate", "plan", "execute", "resume", "status", "rollback"]>;
    requestId: z.ZodOptional<z.ZodEffects<z.ZodString, string, string>>;
    workflow: z.ZodOptional<z.ZodRecord<z.ZodString, z.ZodUnknown>>;
    approvePlanDigest: z.ZodOptional<z.ZodEffects<z.ZodString, string, string>>;
    runId: z.ZodOptional<z.ZodEffects<z.ZodString, string, string>>;
    saveOnSuccess: z.ZodOptional<z.ZodBoolean>;
    confirmWrite: z.ZodOptional<z.ZodBoolean>;
    details: z.ZodOptional<z.ZodBoolean>;
    detailLevel: z.ZodOptional<z.ZodEnum<["summary", "standard", "full"]>>;
    sections: z.ZodOptional<z.ZodArray<z.ZodEnum<["operations", "finalizers", "readBack", "assetDiff", "structures", "rollback", "diagnostics"]>, "many">>;
}, "strict", z.ZodTypeAny, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    requestId?: string | undefined;
    approvePlanDigest?: string | undefined;
    workflow?: Record<string, unknown> | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
}, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    requestId?: string | undefined;
    approvePlanDigest?: string | undefined;
    workflow?: Record<string, unknown> | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
}>;
export declare const UE_WORKFLOW_INPUT_SCHEMA: z.ZodEffects<z.ZodObject<{
    action: z.ZodEnum<["validate", "plan", "execute", "resume", "status", "rollback"]>;
    requestId: z.ZodOptional<z.ZodEffects<z.ZodString, string, string>>;
    workflow: z.ZodOptional<z.ZodRecord<z.ZodString, z.ZodUnknown>>;
    approvePlanDigest: z.ZodOptional<z.ZodEffects<z.ZodString, string, string>>;
    runId: z.ZodOptional<z.ZodEffects<z.ZodString, string, string>>;
    saveOnSuccess: z.ZodOptional<z.ZodBoolean>;
    confirmWrite: z.ZodOptional<z.ZodBoolean>;
    details: z.ZodOptional<z.ZodBoolean>;
    detailLevel: z.ZodOptional<z.ZodEnum<["summary", "standard", "full"]>>;
    sections: z.ZodOptional<z.ZodArray<z.ZodEnum<["operations", "finalizers", "readBack", "assetDiff", "structures", "rollback", "diagnostics"]>, "many">>;
}, "strict", z.ZodTypeAny, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    requestId?: string | undefined;
    approvePlanDigest?: string | undefined;
    workflow?: Record<string, unknown> | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
}, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    requestId?: string | undefined;
    approvePlanDigest?: string | undefined;
    workflow?: Record<string, unknown> | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
}>, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    requestId?: string | undefined;
    approvePlanDigest?: string | undefined;
    workflow?: Record<string, unknown> | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
}, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    requestId?: string | undefined;
    approvePlanDigest?: string | undefined;
    workflow?: Record<string, unknown> | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
}>;
export type UEWorkflowInput = z.infer<typeof UE_WORKFLOW_INPUT_SCHEMA>;
export interface WorkflowExecutor {
    workflow(request: UEWorkflowRequest, signal?: AbortSignal): Promise<UEWorkflowData>;
}
export declare function runWorkflowAction(executor: WorkflowExecutor, request: UEWorkflowInput, signal?: AbortSignal): Promise<MCPResponse>;
export declare function handleWorkflowInput(executor: WorkflowExecutor, value: unknown, signal?: AbortSignal): Promise<MCPResponse>;
export declare function parseWorkflowInput(value: unknown): UEWorkflowInput;
