import { z } from "zod";
import { type MCPResponse } from "./helpers.js";
import { type UEWorkflowData, type UEWorkflowRequest } from "./ue-bridge.js";
export declare const UE_WORKFLOW_DETAIL_LEVELS: readonly ["summary", "standard", "full"];
export declare const UE_WORKFLOW_SECTIONS: readonly ["operations", "finalizers", "readBack", "assetDiff", "structures", "rollback", "diagnostics"];
export declare const UE_WORKFLOW_TOOL_SCHEMA: z.ZodObject<{
    action: z.ZodEnum<["validate", "plan", "execute", "resume", "status", "rollback"]>;
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
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    workflow?: Record<string, unknown> | undefined;
    approvePlanDigest?: string | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
}, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    workflow?: Record<string, unknown> | undefined;
    approvePlanDigest?: string | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
}>;
export declare const UE_WORKFLOW_INPUT_SCHEMA: z.ZodEffects<z.ZodObject<{
    action: z.ZodEnum<["validate", "plan", "execute", "resume", "status", "rollback"]>;
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
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    workflow?: Record<string, unknown> | undefined;
    approvePlanDigest?: string | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
}, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    workflow?: Record<string, unknown> | undefined;
    approvePlanDigest?: string | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
}>, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    workflow?: Record<string, unknown> | undefined;
    approvePlanDigest?: string | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
}, {
    action: "status" | "validate" | "plan" | "execute" | "resume" | "rollback";
    confirmWrite?: boolean | undefined;
    details?: boolean | undefined;
    workflow?: Record<string, unknown> | undefined;
    approvePlanDigest?: string | undefined;
    runId?: string | undefined;
    saveOnSuccess?: boolean | undefined;
    detailLevel?: "summary" | "full" | "standard" | undefined;
    sections?: ("rollback" | "operations" | "finalizers" | "readBack" | "assetDiff" | "structures" | "diagnostics")[] | undefined;
}>;
export type UEWorkflowInput = z.infer<typeof UE_WORKFLOW_INPUT_SCHEMA>;
export interface WorkflowExecutor {
    workflow(request: UEWorkflowRequest): Promise<UEWorkflowData>;
}
export declare function runWorkflowAction(executor: WorkflowExecutor, request: UEWorkflowInput): Promise<MCPResponse>;
export declare function handleWorkflowInput(executor: WorkflowExecutor, value: unknown): Promise<MCPResponse>;
export declare function parseWorkflowInput(value: unknown): UEWorkflowInput;
